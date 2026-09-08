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

void pgsql_parse_nodes(char const *string, osmium::memory::Buffer *buffer,
                       osmium::builder::WayBuilder *obuilder)
{
    if (*string++ == '{') {
        osmium::builder::WayNodeListBuilder wnl_builder{*buffer, obuilder};
        while (*string != '}') {
            char *ptr = nullptr;
            wnl_builder.add_node_ref(std::strtoll(string, &ptr, 10));
            string = ptr;
            if (*string == ',') {
                ++string;
            }
        }
    }
}

/**
 * Build way in buffer from way history table results (columns: way_id,
 * visible, nodes). The way gets no locations on its nodes.
 */
void build_history_way(osmid_t id, pg_result_t const &res, int res_num,
                       osmium::memory::Buffer *buffer)
{
    osmium::builder::WayBuilder builder{*buffer};
    builder.set_id(id);
    pgsql_parse_nodes(res.get_value(res_num, 2), buffer, &builder);
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

std::unordered_map<osmid_t, osmium::Location>
middle_query_pgsql_history_t::get_node_locations_as_of_db(
    idlist_t const &ids, osmium::Timestamp as_of) const
{
    std::unordered_map<osmid_t, osmium::Location> locs;

    if (ids.empty()) {
        return locs;
    }

    util::string_joiner_t id_list{',', '\0', '{', '}'};
    for (auto const id : ids) {
        id_list.add(fmt::to_string(id));
    }

    auto const res = m_db_connection.exec_prepared("get_node_list_as_of",
                                                   id_list(), as_of.to_iso());
    for (int i = 0; i < res.num_tuples(); ++i) {
        locs.emplace(osmium::string_to_object_id(res.get_value(i, 0)),
                     osmium::Location{
                         (int)std::strtol(res.get_value(i, 1), nullptr, 10),
                         (int)std::strtol(res.get_value(i, 2), nullptr, 10)});
    }

    return locs;
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

    auto const locs = get_node_locations_as_of_db(ids, as_of);

    size_t count = 0;
    for (auto &n : *nodes) {
        auto const el = locs.find(n.ref());
        if (el != locs.end()) {
            n.set_location(el->second);
            ++count;
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

    pg_result_t res;
    if (types & osmium::osm_entity_bits::way) {
        // collect ids from all way members into a list..
        util::string_joiner_t way_ids{',', '\0', '{', '}'};
        for (auto const &member : rel.members()) {
            if (member.type() == osmium::item_type::way) {
                way_ids.add(fmt::to_string(member.ref()));
            }
        }

        // ...and get the way versions valid at that time from the database
        if (!way_ids.empty()) {
            res = m_db_connection.exec_prepared("get_way_list_as_of",
                                                way_ids(), as_of.to_iso());
        }
    }

    idlist_t wayidspg;
    if (res) {
        wayidspg = get_ids_from_result(res);
    }

    std::size_t members_found = 0;
    for (auto const &member : rel.members()) {
        if (member.type() == osmium::item_type::node &&
            (types & osmium::osm_entity_bits::node)) {
            osmium::builder::NodeBuilder builder{*buffer};
            builder.set_id(member.ref());
            ++members_found;
        } else if (member.type() == osmium::item_type::way &&
                   (types & osmium::osm_entity_bits::way) && res) {
            // Match the list of ways coming from postgres in a different
            // order back to the list of ways given by the caller
            for (int j = 0; j < res.num_tuples(); ++j) {
                if (member.ref() == wayidspg[static_cast<std::size_t>(j)]) {
                    build_history_way(member.ref(), res, j, buffer);
                    ++members_found;
                    break;
                }
            }
        }
    }

    buffer->commit();

    // Resolve the locations of all needed nodes as of the given time in
    // one query: the locations of relation member nodes and of the nodes
    // of all member ways built above.
    idlist_t node_ids;
    for (auto const &node : buffer->select<osmium::Node>()) {
        node_ids.push_back(node.id());
    }
    for (auto &way : buffer->select<osmium::Way>()) {
        for (auto const &nr : way.nodes()) {
            node_ids.push_back(nr.ref());
        }
    }

    auto const locs = get_node_locations_as_of_db(node_ids, as_of);

    for (auto &node : buffer->select<osmium::Node>()) {
        auto const el = locs.find(node.id());
        if (el != locs.end()) {
            node.set_location(el->second);
        }
    }
    for (auto &way : buffer->select<osmium::Way>()) {
        for (auto &nr : way.nodes()) {
            auto const el = locs.find(nr.ref());
            if (el != locs.end()) {
                nr.set_location(el->second);
            }
        }
    }

    return members_found;
}
