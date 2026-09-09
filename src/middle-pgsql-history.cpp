/**
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This file is part of osm2pgsql (https://osm2pgsql.org/).
 *
 * Copyright (C) 2006-2026 by the osm2pgsql developer community.
 * For a full list of authors see the git log.
 */

/*
 * Middle used for the temporal history import (--temporal). It stores the
 * version history of nodes and ways in the database and answers as-of
 * queries on that history, so that every historical way and relation
 * version gets the geometry valid at its creation time.
 */

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <algorithm>
#include <stdexcept>
#include <unordered_map>
#include <utility>

#include <osmium/builder/osm_object_builder.hpp>
#include <osmium/memory/buffer.hpp>
#include <osmium/osm/types_from_string.hpp>

#include "format.hpp"
#include "idlist.hpp"
#include "logging.hpp"
#include "middle-pgsql-history.hpp"
#include "options.hpp"
#include "pgsql-helper.hpp"
#include "template.hpp"
#include "util.hpp"

namespace {

[[noreturn]] void not_supported(char const *what)
{
    throw std::runtime_error{fmt::format(
        "The temporal history middle does not support {}:"
        " with --temporal there are no current-state middle tables.",
        what)};
}

/**
 * Build way in buffer from a way history version (columns: way_id,
 * visible, nodes). The way gets no locations on its nodes.
 */
void build_history_way(osmid_t id, std::vector<osmid_t> const &nodes,
                        osmium::memory::Buffer *buffer)
{
    osmium::builder::WayBuilder builder{*buffer};
    builder.set_id(id);
    osmium::builder::WayNodeListBuilder wnl_builder{*buffer, &builder};
    for (auto const node_id : nodes) {
        wnl_builder.add_node_ref(node_id);
    }
}

/**
 * Parse a PostgreSQL array of object ids in the form "[1,2,3]" into a
 * vector.
 */
std::vector<osmid_t> parse_id_array(char const *str)
{
    std::vector<osmid_t> ids;

    if (str == nullptr || *str != '{') {
        return ids;
    }

    char const *p = str + 1;
    while (*p != '\0' && *p != '}') {
        char *end = nullptr;
        auto const id = std::strtoll(p, &end, 10);
        if (end == p) {
            break;
        }
        ids.push_back(static_cast<osmid_t>(id));
        p = (*end == ',') ? end + 1 : end;
    }

    return ids;
}

/**
 * Get the newest version with ts <= as_of from a version list sorted by
 * (ts, version), or nullptr if there is none. Used for as-of resolution.
 */
template <typename T>
T const *newest_version(std::vector<T> const &versions,
                        osmium::Timestamp as_of)
{
    auto const it =
        std::upper_bound(versions.begin(), versions.end(), as_of,
                         [](osmium::Timestamp ts, T const &version) {
                             return ts < version.ts;
                         });
    if (it == versions.begin()) {
        return nullptr;
    }
    return &*std::prev(it);
}

/// Render an id list as a PostgreSQL array literal for ANY($1).
std::string build_id_array(idlist_t const &ids)
{
    util::string_joiner_t id_list{',', '\0', '{', '}'};
    for (auto const id : ids) {
        id_list.add(fmt::to_string(id));
    }
    return id_list();
}

} // anonymous namespace

middle_pgsql_history_t::middle_pgsql_history_t(
    std::shared_ptr<thread_pool_t> thread_pool, options_t const *options)
: middle_t(std::move(thread_pool)), m_options(options),
  m_db_connection(options->connection_params, "middle.history.main"),
  m_copy_thread(
      std::make_shared<db_copy_thread_t>(options->connection_params)),
  m_db_copy(m_copy_thread),
  m_nodes_history(std::make_shared<db_target_descr_t>(
      options->middle_dbschema, options->prefix + "_nodes_history",
      "node_id")),
  m_ways_history(std::make_shared<db_target_descr_t>(
      options->middle_dbschema, options->prefix + "_ways_history", "way_id"))
{
    std::string const schema = "\"" + options->middle_dbschema + "\".";

    m_params.set("prefix", options->prefix);
    m_params.set("schema", schema);
    m_params.set("unlogged", options->droptemp ? "UNLOGGED" : "");
    m_params.set("data_tablespace",
                 tablespace_clause(options->tblsslim_data));
    m_params.set("using_tablespace",
                 options->tblsslim_index.empty()
                     ? ""
                     : "USING INDEX TABLESPACE " + options->tblsslim_index);
}

void middle_pgsql_history_t::dbexec(std::string_view templ) const
{
    m_db_connection.exec(render_template(templ));
}

std::string middle_pgsql_history_t::render_template(
    std::string_view templ) const
{
    template_t sql_template{templ};
    sql_template.set_params(m_params);
    return sql_template.render();
}

void middle_pgsql_history_t::start()
{
    assert(m_middle_state == middle_state::constructed);
#ifndef NDEBUG
    m_middle_state = middle_state::node;
#endif

    log_debug("Setting up table 'nodes_history'");
    dbexec(R"(DROP TABLE IF EXISTS {schema}"{prefix}_nodes_history" CASCADE)");
    dbexec("CREATE {unlogged} TABLE {schema}\"{prefix}_nodes_history\" ("
           " node_id int8 NOT NULL,"
           " ts timestamp NOT NULL,"
           " version int4 NOT NULL,"
           " visible boolean NOT NULL,"
           " lat int4 NOT NULL,"
           " lon int4 NOT NULL,"
           " PRIMARY KEY (node_id, ts, version) {using_tablespace}"
           ") {data_tablespace}");

    log_debug("Setting up table 'ways_history'");
    dbexec(R"(DROP TABLE IF EXISTS {schema}"{prefix}_ways_history" CASCADE)");
    dbexec("CREATE {unlogged} TABLE {schema}\"{prefix}_ways_history\" ("
           " way_id int8 NOT NULL,"
           " ts timestamp NOT NULL,"
           " version int4 NOT NULL,"
           " visible boolean NOT NULL,"
           " nodes int8[] NOT NULL,"
           " PRIMARY KEY (way_id, ts, version) {using_tablespace}"
           ") {data_tablespace}");
}

void middle_pgsql_history_t::stop()
{
    assert(m_middle_state == middle_state::done);

    if (m_options->droptemp) {
        // Dropping the tables is fast, so do it synchronously to guarantee
        // that the space is freed before creating the other indices.
        dbexec(
            R"(DROP TABLE IF EXISTS {schema}"{prefix}_nodes_history" CASCADE)");
        dbexec(
            R"(DROP TABLE IF EXISTS {schema}"{prefix}_ways_history" CASCADE)");
    }
}

void middle_pgsql_history_t::node(osmium::Node const &)
{
    not_supported("node(): temporal history import uses node_history()");
}

void middle_pgsql_history_t::way(osmium::Way const &)
{
    not_supported("way(): temporal history import uses way_history()");
}

void middle_pgsql_history_t::relation(osmium::Relation const &)
{
    not_supported("relation(): the history middle stores no relations");
}

void middle_pgsql_history_t::node_history(osmium::Node const &node)
{
    assert(m_middle_state == middle_state::node);

    m_db_copy.new_line(m_nodes_history);

    m_db_copy.add_columns(node.id(), node.timestamp().to_iso(),
                          node.version(), node.visible() ? 1 : 0,
                          node.location().y(), node.location().x());

    m_db_copy.finish_line();
}

void middle_pgsql_history_t::way_history(osmium::Way const &way)
{
    assert(m_middle_state == middle_state::way);

    m_db_copy.new_line(m_ways_history);

    m_db_copy.add_columns(way.id(), way.timestamp().to_iso(), way.version(),
                          way.visible() ? 1 : 0);

    m_db_copy.new_array();
    for (auto const &n : way.nodes()) {
        m_db_copy.add_array_elem(n.ref());
    }
    m_db_copy.finish_array();

    m_db_copy.finish_line();
}

void middle_pgsql_history_t::after_nodes()
{
    assert(m_middle_state == middle_state::node);
#ifndef NDEBUG
    m_middle_state = middle_state::way;
#endif

    m_db_copy.sync();
    analyze_table(m_db_connection, m_options->middle_dbschema,
                  m_options->prefix + "_nodes_history");
}

void middle_pgsql_history_t::after_ways()
{
    assert(m_middle_state == middle_state::way);
#ifndef NDEBUG
    m_middle_state = middle_state::relation;
#endif

    m_db_copy.sync();
    analyze_table(m_db_connection, m_options->middle_dbschema,
                  m_options->prefix + "_ways_history");
}

void middle_pgsql_history_t::after_relations()
{
    assert(m_middle_state == middle_state::relation);
#ifndef NDEBUG
    m_middle_state = middle_state::done;
#endif

    m_db_copy.sync();

    // release the copy thread and its database connection
    m_copy_thread->finish();
}

std::shared_ptr<middle_query_t> middle_pgsql_history_t::get_query_instance()
{
    auto mid = std::make_unique<middle_query_pgsql_history_t>(
        m_options->connection_params, *m_options);

    // The visible filter has to be applied after picking the newest
    // version, because a deleted version must hide the older visible
    // versions for times after its deletion.
    mid->prepare("get_node_list_as_of",
                 render_template(
                     "SELECT node_id, lon, lat FROM ("
                     " SELECT DISTINCT ON (node_id) node_id, lon, lat,"
                     " visible"
                     " FROM {schema}\"{prefix}_nodes_history\""
                     " WHERE node_id = ANY($1::int8[])"
                     " AND ts <= $2::timestamp"
                     " ORDER BY node_id, ts DESC, version DESC"
                     ") t WHERE visible"));

    mid->prepare("get_way_list_as_of",
                 render_template(
                     "SELECT way_id, visible, nodes FROM ("
                     " SELECT DISTINCT ON (way_id) way_id, visible, nodes"
                     " FROM {schema}\"{prefix}_ways_history\""
                     " WHERE way_id = ANY($1::int8[])"
                     " AND ts <= $2::timestamp"
                     " ORDER BY way_id, ts DESC, version DESC"
                     ") t WHERE visible"));

    mid->prepare("get_node_histories",
                 render_template(
                     "SELECT node_id, EXTRACT(EPOCH FROM ts)::int8,"
                     " version, visible, lat, lon"
                     " FROM {schema}\"{prefix}_nodes_history\""
                     " WHERE node_id = ANY($1::int8[])"
                     " ORDER BY node_id, ts, version"));

    mid->prepare("get_way_histories",
                 render_template("SELECT way_id,"
                                 " EXTRACT(EPOCH FROM ts)::int8,"
                                 " visible, nodes"
                                 " FROM {schema}\"{prefix}_ways_history\""
                                 " WHERE way_id = ANY($1::int8[])"
                                 " ORDER BY way_id, ts, version"));

    return std::shared_ptr<middle_query_t>(mid.release());
}

void middle_query_pgsql_history_t::prepare(std::string const &stmt,
                                           std::string const &sql_cmd) const
{
    m_db_connection.prepare(stmt, fmt::runtime(sql_cmd));
}

middle_query_pgsql_history_t::middle_query_pgsql_history_t(
    connection_params_t const &connection_params, options_t const &)
: m_db_connection(connection_params, "middle.history.query")
{
    // Disable JIT and parallel workers as they are known to cause
    // problems when accessing the intarrays.
    m_db_connection.set_config("jit_above_cost", "-1");
    m_db_connection.set_config("max_parallel_workers_per_gather", "0");
}

void middle_query_pgsql_history_t::query_node_history(
    idlist_t const &ids) const
{
    auto const res = m_db_connection.exec_prepared("get_node_histories",
                                                   build_id_array(ids));

    for (int i = 0; i < res.num_tuples(); ++i) {
        node_history_version_t version;
        version.ts = osmium::Timestamp{static_cast<uint32_t>(
            std::strtoll(res.get_value(i, 1), nullptr, 10))};
        version.version = static_cast<uint32_t>(
            std::strtoul(res.get_value(i, 2), nullptr, 10));
        version.visible = (*res.get_value(i, 3) == 't');
        version.lat =
            static_cast<int32_t>(std::strtol(res.get_value(i, 4), nullptr, 10));
        version.lon =
            static_cast<int32_t>(std::strtol(res.get_value(i, 5), nullptr, 10));
        m_node_cache[osmium::string_to_object_id(res.get_value(i, 0))]
            .push_back(version);
    }
}

node_history_map_t const &
middle_query_pgsql_history_t::load_node_history(idlist_t const &ids) const
{
    m_node_cache.clear();

    if (!ids.empty()) {
        query_node_history(ids);
    }

    return m_node_cache;
}

void middle_query_pgsql_history_t::query_way_history(
    idlist_t const &ids) const
{
    auto const res = m_db_connection.exec_prepared("get_way_histories",
                                                   build_id_array(ids));

    for (int i = 0; i < res.num_tuples(); ++i) {
        way_history_version_t version;
        version.ts = osmium::Timestamp{static_cast<uint32_t>(
            std::strtoll(res.get_value(i, 1), nullptr, 10))};
        version.visible = (*res.get_value(i, 2) == 't');
        version.nodes = parse_id_array(res.get_value(i, 3));
        m_way_cache[osmium::string_to_object_id(res.get_value(i, 0))]
            .push_back(std::move(version));
    }
}

way_history_map_t const &
middle_query_pgsql_history_t::load_way_history(idlist_t const &ids) const
{
    m_way_cache.clear();

    if (!ids.empty()) {
        query_way_history(ids);
    }

    return m_way_cache;
}

void middle_query_pgsql_history_t::ensure_nodes_cached(
    idlist_t const &ids) const
{
    idlist_t missing;
    for (auto const id : ids) {
        if (m_node_cache.find(id) == m_node_cache.end()) {
            missing.push_back(id);
        }
    }

    if (missing.empty()) {
        return;
    }

    // Insert empty entries first, so ids without any history rows are
    // cached as "no data" and not queried again.
    for (auto const id : missing) {
        m_node_cache.emplace(id, std::vector<node_history_version_t>{});
    }

    query_node_history(missing);
}

void middle_query_pgsql_history_t::ensure_ways_cached(
    idlist_t const &ids) const
{
    idlist_t missing;
    for (auto const id : ids) {
        if (m_way_cache.find(id) == m_way_cache.end()) {
            missing.push_back(id);
        }
    }

    if (missing.empty()) {
        return;
    }

    for (auto const id : missing) {
        m_way_cache.emplace(id, std::vector<way_history_version_t>{});
    }

    query_way_history(missing);
}

osmium::Location
middle_query_pgsql_history_t::get_node_location(osmid_t) const
{
    not_supported("get_node_location(): current node locations");
}

size_t middle_query_pgsql_history_t::nodes_get_list(
    osmium::WayNodeList *) const
{
    not_supported("nodes_get_list(): current node locations");
}

bool middle_query_pgsql_history_t::node_get(osmid_t,
                                            osmium::memory::Buffer *) const
{
    not_supported("node_get(): current nodes");
}

bool middle_query_pgsql_history_t::way_get(osmid_t,
                                           osmium::memory::Buffer *) const
{
    not_supported("way_get(): current ways");
}

size_t middle_query_pgsql_history_t::rel_members_get(
    osmium::Relation const &, osmium::memory::Buffer *,
    osmium::osm_entity_bits::type) const
{
    not_supported("rel_members_get(): current relation members");
}

bool middle_query_pgsql_history_t::relation_get(
    osmid_t, osmium::memory::Buffer *) const
{
    not_supported("relation_get(): current relations");
}

size_t middle_query_pgsql_history_t::nodes_get_list_as_of(
    osmium::WayNodeList *nodes, osmium::Timestamp as_of) const
{
    if (nodes->empty()) {
        return 0;
    }

    idlist_t ids;
    ids.reserve(nodes->size());
    for (auto const &n : *nodes) {
        ids.push_back(n.ref());
    }

    ensure_nodes_cached(ids);

    size_t count = 0;
    for (auto &n : *nodes) {
        auto const it = m_node_cache.find(n.ref());
        if (it == m_node_cache.end()) {
            n.set_location(osmium::Location{});
            continue;
        }
        auto const *version = newest_version(it->second, as_of);
        if (version != nullptr && version->visible) {
            n.set_location(osmium::Location{version->lon, version->lat});
            ++count;
        } else {
            // No version at that time, or the newest version is a
            // tombstone: explicitly invalidate the location, because
            // the same (recycled) way buffer may still carry the
            // location from an earlier segment replay.
            n.set_location(osmium::Location{});
        }
    }

    return count;
}

std::size_t middle_query_pgsql_history_t::rel_members_get_as_of(
    osmium::Relation const &rel, osmium::memory::Buffer *buffer,
    osmium::osm_entity_bits::type types, osmium::Timestamp as_of) const
{
    assert(buffer);
    assert((types & osmium::osm_entity_bits::relation) == 0);

    if (types & osmium::osm_entity_bits::way) {
        // Make sure all member ways are in the way cache.
        idlist_t way_ids;
        for (auto const &member : rel.members()) {
            if (member.type() == osmium::item_type::way) {
                way_ids.push_back(member.ref());
            }
        }
        ensure_ways_cached(way_ids);
    }

    std::size_t members_found = 0;
    for (auto const &member : rel.members()) {
        if (member.type() == osmium::item_type::node &&
            (types & osmium::osm_entity_bits::node)) {
            osmium::builder::NodeBuilder builder{*buffer};
            builder.set_id(member.ref());
            ++members_found;
        } else if (member.type() == osmium::item_type::way &&
                   (types & osmium::osm_entity_bits::way)) {
            // The member way version valid at that time, if any.
            auto const it = m_way_cache.find(member.ref());
            if (it == m_way_cache.end()) {
                continue;
            }
            auto const *version = newest_version(it->second, as_of);
            if (version != nullptr && version->visible) {
                build_history_way(member.ref(), version->nodes, buffer);
                ++members_found;
            }
        }
    }

    buffer->commit();

    // Resolve the locations of all needed nodes as of the given time:
    // the locations of relation member nodes and of the nodes of all
    // member ways built above.
    idlist_t node_ids;
    for (auto const &node : buffer->select<osmium::Node>()) {
        node_ids.push_back(node.id());
    }
    for (auto &way : buffer->select<osmium::Way>()) {
        for (auto const &nr : way.nodes()) {
            node_ids.push_back(nr.ref());
        }
    }

    ensure_nodes_cached(node_ids);

    auto const resolve = [this, as_of](osmid_t id) {
        return newest_version(m_node_cache.find(id)->second, as_of);
    };

    for (auto &node : buffer->select<osmium::Node>()) {
        auto const *version = resolve(node.id());
        if (version != nullptr && version->visible) {
            node.set_location(osmium::Location{version->lon, version->lat});
        }
    }
    for (auto &way : buffer->select<osmium::Way>()) {
        for (auto &nr : way.nodes()) {
            auto const *version = resolve(nr.ref());
            if (version != nullptr && version->visible) {
                nr.set_location(osmium::Location{version->lon, version->lat});
            }
        }
    }

    return members_found;
}
