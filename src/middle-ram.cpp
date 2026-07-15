/**
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This file is part of osm2pgsql (https://osm2pgsql.org/).
 *
 * Copyright (C) 2006-2026 by the osm2pgsql developer community.
 * For a full list of authors see the git log.
 */

#include "middle-ram.hpp"

#include "logging.hpp"
#include "node-persistent-cache.hpp"
#include "options.hpp"
#include "output-requirements.hpp"
#include "pgsql.hpp"

#include <osmium/builder/osm_object_builder.hpp>
#include <osmium/util/delta.hpp>

// Workaround: This must be included before buffer_string.hpp due to a missing
// include in the upstream code. https://github.com/mapbox/protozero/pull/104
#include <protozero/config.hpp>

#include <protozero/buffer_string.hpp>
#include <protozero/varint.hpp>

#include <cassert>
#include <memory>

namespace {

void add_delta_encoded_way_node_list(std::string *data,
                                     osmium::WayNodeList const &wnl)
{
    assert(data);

    // Add number of nodes in list
    protozero::add_varint_to_buffer(data, wnl.size());

    // Add delta encoded node ids
    osmium::DeltaEncode<osmid_t> delta;
    for (auto const &nr : wnl) {
        protozero::add_varint_to_buffer(
            data, protozero::encode_zigzag64(delta.update(nr.ref())));
    }
}

void get_delta_encoded_way_nodes_list(std::string const &data,
                                      std::size_t offset,
                                      osmium::builder::WayBuilder *builder)
{
    assert(builder);

    char const *begin = data.data() + offset;
    char const *const end = data.data() + data.size();

    auto count = protozero::decode_varint(&begin, end);

    osmium::DeltaDecode<osmid_t> delta;
    osmium::builder::WayNodeListBuilder wnl_builder{*builder};
    while (count > 0) {
        auto const val =
            protozero::decode_zigzag64(protozero::decode_varint(&begin, end));
        wnl_builder.add_node_ref(delta.update(val));
        --count;
    }
}

} // anonymous namespace

middle_ram_t::middle_ram_t(std::shared_ptr<thread_pool_t> thread_pool,
                           options_t const *options)
: middle_t(std::move(thread_pool))
{
    assert(options);

    if (options->extra_attributes) {
        m_store_options.untagged_nodes = true;
    }

    if (options->temporal) {
        m_store_options.temporal = true;
    }

    if (!options->flat_node_file.empty()) {
        m_persistent_cache = std::make_shared<node_persistent_cache_t>(
            options->flat_node_file, !options->append, options->droptemp);
    }
}

void middle_ram_t::set_requirements(output_requirements const &requirements)
{
    if (m_store_options.temporal) {
        // In temporal mode, store all object types for version metadata.
        m_store_options.nodes = true;
        m_store_options.untagged_nodes = true;
        m_store_options.ways = true;
        m_store_options.way_nodes = false;
        m_store_options.relations = true;
    } else {
        if (requirements.full_nodes) {
            m_store_options.nodes = true;
        }

        if (requirements.full_ways) {
            m_store_options.ways = true;
            m_store_options.way_nodes = false;
        }

        if (requirements.full_relations) {
            m_store_options.relations = true;
        }
    }

    log_debug("Middle 'ram' options:");
    log_debug("  locations: {}", m_store_options.locations);
    log_debug("  locations_on_disk: {}", !!m_persistent_cache);
    log_debug("  way_nodes: {}", m_store_options.way_nodes);
    log_debug("  nodes: {}", m_store_options.nodes);
    log_debug("  untagged_nodes: {}", m_store_options.untagged_nodes);
    log_debug("  ways: {}", m_store_options.ways);
    log_debug("  relations: {}", m_store_options.relations);
}

void middle_ram_t::stop()
{
    assert(m_middle_state == middle_state::done);

    constexpr auto MBYTE = 1024 * 1024;

    if (m_persistent_cache) {
        log_debug("Middle 'ram': Node locations on disk: size={} bytes={}M",
                  m_persistent_cache->size(),
                  m_persistent_cache->used_memory() / MBYTE);
    } else if (m_store_options.temporal) {
        log_debug("Middle 'ram': Temporal node locations: size={} bytes={}M",
                  m_temporal_node_locations.size(),
                  (m_temporal_node_locations.size() *
                   sizeof(std::pair<osmid_t, osmium::Location>)) /
                      MBYTE);
    } else {
        log_debug("Middle 'ram': Node locations in memory: size={} bytes={}M",
                  m_node_locations.size(),
                  m_node_locations.used_memory() / MBYTE);
    }

    log_debug("Middle 'ram': Way nodes data: size={} capacity={} bytes={}M",
              m_way_nodes_data.size(), m_way_nodes_data.capacity(),
              m_way_nodes_data.capacity() / MBYTE);

    log_debug("Middle 'ram': Way nodes index: size={} capacity={} bytes={}M",
              m_way_nodes_index.size(), m_way_nodes_index.capacity(),
              m_way_nodes_index.used_memory() / MBYTE);

    log_debug("Middle 'ram': Object data: size={} capacity={} bytes={}M",
              m_object_buffer.committed(), m_object_buffer.capacity(),
              m_object_buffer.capacity() / MBYTE);

    std::size_t index_size = 0;
    std::size_t index_capacity = 0;
    std::size_t index_mem = 0;
    for (auto const &index : m_object_index) {
        index_size += index.size();
        index_capacity += index.capacity();
        index_mem += index.used_memory();
    }
    log_debug("Middle 'ram': Object indexes: size={} capacity={} bytes={}M",
              index_size, index_capacity, index_mem / MBYTE);

    if (m_store_options.temporal) {
        log_debug("Middle 'ram': Temporal metadata: {} nodes, {} ways, {} rels",
                  m_temporal_node_metadata.size(),
                  m_temporal_way_metadata.size(),
                  m_temporal_rel_metadata.size());
    }

    log_debug("Middle 'ram': Memory used overall: {}MBytes",
              (m_node_locations.used_memory() + m_way_nodes_data.capacity() +
               m_way_nodes_index.used_memory() + m_object_buffer.capacity() +
               index_mem) /
                  MBYTE);

    m_node_locations.clear();

    m_way_nodes_index.clear();
    m_way_nodes_data.clear();
    m_way_nodes_data.shrink_to_fit();

    m_object_buffer = osmium::memory::Buffer{};

    for (auto &index : m_object_index) {
        index.clear();
    }

    // Clear temporal data structures
    m_temporal_node_locations.clear();
    m_temporal_node_locations.rehash(0);
    m_temporal_node_index.clear();
    m_temporal_node_index.rehash(0);
    m_temporal_way_index.clear();
    m_temporal_way_index.rehash(0);
    m_temporal_rel_index.clear();
    m_temporal_rel_index.rehash(0);
    m_temporal_node_metadata.clear();
    m_temporal_node_metadata.shrink_to_fit();
    m_temporal_way_metadata.clear();
    m_temporal_way_metadata.shrink_to_fit();
    m_temporal_rel_metadata.clear();
    m_temporal_rel_metadata.shrink_to_fit();
}

void middle_ram_t::store_object(osmium::OSMObject const &object)
{
    auto const offset = m_object_buffer.committed();
    m_object_buffer.add_item(object);
    m_object_buffer.commit();
    m_object_index(object.type()).add(object.id(), offset);
}

bool middle_ram_t::get_object(osmium::item_type type, osmid_t id,
                              osmium::memory::Buffer *buffer) const
{
    assert(buffer);

    if (m_store_options.temporal) {
        return get_object_temporal(type, id, buffer);
    }

    auto const offset = m_object_index(type).get(id);
    if (offset == ordered_index_t::not_found_value()) {
        return false;
    }
    buffer->add_item(m_object_buffer.get<osmium::memory::Item>(offset));
    buffer->commit();
    return true;
}

void middle_ram_t::store_object_temporal(
    osmium::OSMObject const &object)
{
    auto const offset = m_object_buffer.committed();
    m_object_buffer.add_item(object);
    m_object_buffer.commit();

    // Update index to point to the latest version.
    switch (object.type()) {
    case osmium::item_type::node:
        m_temporal_node_index[object.id()] = offset;
        break;
    case osmium::item_type::way:
        m_temporal_way_index[object.id()] = offset;
        break;
    case osmium::item_type::relation:
        m_temporal_rel_index[object.id()] = offset;
        break;
    default:
        break;
    }
}

bool middle_ram_t::get_object_temporal(osmium::item_type type, osmid_t id,
                                       osmium::memory::Buffer *buffer) const
{
    assert(buffer);

    std::size_t offset = 0;
    switch (type) {
    case osmium::item_type::node: {
        auto const it = m_temporal_node_index.find(id);
        if (it == m_temporal_node_index.end()) {
            return false;
        }
        offset = it->second;
        break;
    }
    case osmium::item_type::way: {
        auto const it = m_temporal_way_index.find(id);
        if (it == m_temporal_way_index.end()) {
            return false;
        }
        offset = it->second;
        break;
    }
    case osmium::item_type::relation: {
        auto const it = m_temporal_rel_index.find(id);
        if (it == m_temporal_rel_index.end()) {
            return false;
        }
        offset = it->second;
        break;
    }
    default:
        return false;
    }

    buffer->add_item(m_object_buffer.get<osmium::memory::Item>(offset));
    buffer->commit();
    return true;
}

void middle_ram_t::node(osmium::Node const &node)
{
    assert(m_middle_state == middle_state::node);

    if (m_store_options.temporal) {
        // In temporal mode, store all versions including deleted ones.
        // Record temporal metadata.
        m_temporal_node_metadata.push_back(
            {node.id(), node.version(), node.timestamp()});

        // Store node location (latest version wins).
        if (node.location().valid()) {
            m_temporal_node_locations[node.id()] = node.location();
        }

        // Also store in persistent cache if available.
        if (m_persistent_cache && node.location().valid()) {
            m_persistent_cache->set(node.id(), node.location());
        }

        // Store the object for relation member retrieval.
        if (m_store_options.nodes) {
            store_object_temporal(node);
        }
        return;
    }

    assert(node.visible());

    if (m_store_options.locations) {
        if (m_persistent_cache) {
            m_persistent_cache->set(node.id(), node.location());
        } else {
            m_node_locations.set(node.id(), node.location());
        }
    }

    if (m_store_options.nodes &&
        (!node.tags().empty() || m_store_options.untagged_nodes)) {
        store_object(node);
    }
}

void middle_ram_t::way(osmium::Way const &way)
{
    assert(m_middle_state == middle_state::way);

    if (m_store_options.temporal) {
        // In temporal mode, store all versions including deleted ones.
        m_temporal_way_metadata.push_back(
            {way.id(), way.version(), way.timestamp()});

        // Store the object for relation member retrieval.
        if (m_store_options.ways) {
            store_object_temporal(way);
        }
        return;
    }

    assert(way.visible());

    if (m_store_options.way_nodes) {
        auto const offset = m_way_nodes_data.size();
        add_delta_encoded_way_node_list(&m_way_nodes_data, way.nodes());
        m_way_nodes_index.add(way.id(), offset);
    }

    if (m_store_options.ways) {
        store_object(way);
    }
}

void middle_ram_t::relation(osmium::Relation const &relation)
{
    assert(m_middle_state == middle_state::relation);

    if (m_store_options.temporal) {
        // In temporal mode, store all versions including deleted ones.
        m_temporal_rel_metadata.push_back(
            {relation.id(), relation.version(), relation.timestamp()});

        // Store the object for relation member retrieval.
        if (m_store_options.relations) {
            store_object_temporal(relation);
        }
        return;
    }

    assert(relation.visible());

    if (m_store_options.relations) {
        store_object(relation);
    }
}

void middle_ram_t::after_nodes()
{
    assert(m_middle_state == middle_state::node);
#ifndef NDEBUG
    m_middle_state = middle_state::way;
#endif

    if (!m_persistent_cache) {
        m_node_locations.log_stats();
    }
}

osmium::Location middle_ram_t::get_node_location(osmid_t id) const
{
    return m_node_locations.get(id);
}

std::size_t middle_ram_t::nodes_get_list(osmium::WayNodeList *nodes) const
{
    assert(nodes);

    std::size_t count = 0;

    if (m_store_options.temporal) {
        for (auto &nr : *nodes) {
            auto const it = m_temporal_node_locations.find(nr.ref());
            if (it != m_temporal_node_locations.end()) {
                nr.set_location(it->second);
                ++count;
            }
        }
        return count;
    }

    if (m_store_options.locations) {
        if (m_persistent_cache) {
            for (auto &nr : *nodes) {
                nr.set_location(m_persistent_cache->get(nr.ref()));
                if (nr.location().valid()) {
                    ++count;
                }
            }
        } else {
            for (auto &nr : *nodes) {
                nr.set_location(m_node_locations.get(nr.ref()));
                if (nr.location().valid()) {
                    ++count;
                }
            }
        }
    }

    return count;
}

bool middle_ram_t::node_get(osmid_t id, osmium::memory::Buffer *buffer) const
{
    assert(buffer);

    if (m_store_options.nodes) {
        auto const got_it = get_object(osmium::item_type::node, id, buffer);
        if (got_it) {
            return true;
        }
    }

    if (m_store_options.locations) {
        osmium::Location location{};
        if (m_persistent_cache) {
            location = m_persistent_cache->get(id);
        }
        if (!location.valid()) {
            location = m_node_locations.get(id);
        }
        if (location.valid()) {
            {
                osmium::builder::NodeBuilder builder{*buffer};
                builder.set_id(id);
                builder.set_location(location);
            }

            buffer->commit();
            return true;
        }
    }

    return false;
}

bool middle_ram_t::way_get(osmid_t id, osmium::memory::Buffer *buffer) const
{
    assert(buffer);

    if (m_store_options.ways) {
        return get_object(osmium::item_type::way, id, buffer);
    }
    return false;
}

std::size_t
middle_ram_t::rel_members_get(osmium::Relation const &rel,
                              osmium::memory::Buffer *buffer,
                              osmium::osm_entity_bits::type types) const
{
    assert(buffer);

    std::size_t count = 0;

    for (auto const &member : rel.members()) {
        auto const member_entity_type =
            osmium::osm_entity_bits::from_item_type(member.type());
        if ((member_entity_type & types) == 0) {
            continue;
        }

        if (m_store_options.temporal) {
            switch (member.type()) {
            case osmium::item_type::node:
                if (m_store_options.nodes) {
                    if (get_object_temporal(osmium::item_type::node,
                                            member.ref(), buffer)) {
                        ++count;
                        continue;
                    }
                }
                {
                    osmium::builder::NodeBuilder builder{*buffer};
                    builder.set_id(member.ref());
                }
                buffer->commit();
                ++count;
                break;
            case osmium::item_type::way:
                if (m_store_options.ways) {
                    if (get_object_temporal(osmium::item_type::way,
                                            member.ref(), buffer)) {
                        ++count;
                    }
                }
                break;
            default: // osmium::item_type::relation
                if (m_store_options.relations) {
                    if (get_object_temporal(osmium::item_type::relation,
                                            member.ref(), buffer)) {
                        ++count;
                    }
                }
            }
            continue;
        }

        switch (member.type()) {
        case osmium::item_type::node:
            if (m_store_options.nodes) {
                auto const offset = m_object_index.nodes().get(member.ref());
                if (offset != ordered_index_t::not_found_value()) {
                    buffer->add_item(m_object_buffer.get<osmium::Node>(offset));
                    buffer->commit();
                    ++count;
                    continue;
                }
            }
            {
                osmium::builder::NodeBuilder builder{*buffer};
                builder.set_id(member.ref());
            }
            buffer->commit();
            ++count;
            break;
        case osmium::item_type::way:
            if (m_store_options.ways) {
                auto const offset = m_object_index.ways().get(member.ref());
                if (offset != ordered_index_t::not_found_value()) {
                    buffer->add_item(m_object_buffer.get<osmium::Way>(offset));
                    buffer->commit();
                    ++count;
                }
            } else if (m_store_options.way_nodes) {
                auto const offset = m_way_nodes_index.get(member.ref());
                if (offset != ordered_index_t::not_found_value()) {
                    osmium::builder::WayBuilder builder{*buffer};
                    builder.set_id(member.ref());
                    get_delta_encoded_way_nodes_list(m_way_nodes_data, offset,
                                                     &builder);
                }
                buffer->commit();
                ++count;
            }
            break;
        default: // osmium::item_type::relation
            if (m_store_options.relations) {
                auto const offset =
                    m_object_index.relations().get(member.ref());
                if (offset != ordered_index_t::not_found_value()) {
                    buffer->add_item(
                        m_object_buffer.get<osmium::Relation>(offset));
                    buffer->commit();
                    ++count;
                }
            }
        }
    }

    return count;
}

bool middle_ram_t::relation_get(osmid_t id,
                                osmium::memory::Buffer *buffer) const
{
    assert(buffer);

    if (m_store_options.relations) {
        return get_object(osmium::item_type::relation, id, buffer);
    }
    return false;
}

void middle_ram_t::create_temporal_tables(pg_conn_t &conn,
                                          std::string const &prefix) const
{
    auto const create_and_fill = [&](std::string const &suffix,
                                     std::vector<temporal_metadata_t> const
                                         &metadata) {
        auto const table_name = prefix + "_" + suffix;

        conn.exec("DROP TABLE IF EXISTS \"{}\"", table_name);
        conn.exec("CREATE TEMPORARY TABLE \"{}\" ("
                  "id int8 NOT NULL,"
                  "version int4 NOT NULL,"
                  "created timestamp with time zone"
                  ")",
                  table_name);

        if (metadata.empty()) {
            return;
        }

        std::string data;
        data.reserve(metadata.size() * 64);
        for (auto const &entry : metadata) {
            fmt::format_to(std::back_inserter(data), FMT_STRING("{}\t{}\t{}\n"),
                           entry.id, entry.version,
                           entry.created.to_iso());
        }

        auto const copy_sql =
            fmt::format("COPY \"{}\" FROM STDIN", table_name);
        conn.copy_start(copy_sql);
        conn.copy_send(data, table_name);
        conn.copy_end(table_name);
    };

    log_info("Creating temporary temporal metadata tables...");

    create_and_fill("nodes", m_temporal_node_metadata);
    create_and_fill("ways", m_temporal_way_metadata);
    create_and_fill("rels", m_temporal_rel_metadata);
}

std::shared_ptr<middle_query_t> middle_ram_t::get_query_instance()
{
    return shared_from_this();
}
