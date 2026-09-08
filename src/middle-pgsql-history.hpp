#ifndef OSM2PGSQL_MIDDLE_PGSQL_HISTORY_HPP
#define OSM2PGSQL_MIDDLE_PGSQL_HISTORY_HPP

/**
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This file is part of osm2pgsql (https://osm2pgsql.org/).
 *
 * Copyright (C) 2006-2026 by the osm2pgsql developer community.
 * For a full list of authors see the git log.
 */

#include <memory>
#include <string_view>
#include <vector>

#include <osmium/osm/timestamp.hpp>

#include "db-copy-mgr.hpp"
#include "middle.hpp"
#include "params.hpp"
#include "pgsql.hpp"

class options_t;

/**
 * Middle used for the temporal history import (--temporal).
 *
 * This middle only stores the version history of nodes and ways in the
 * {prefix}_nodes_history and {prefix}_ways_history tables. There are no
 * current-state middle tables: during the temporal import the output gets
 * its geometry data through the as-of queries on the history tables.
 *
 * The current-state interfaces of middle_t/middle_query_t are not
 * supported by this middle; calling them results in an error.
 */
class middle_query_pgsql_history_t;

class middle_pgsql_history_t : public middle_t
{
public:
    middle_pgsql_history_t(std::shared_ptr<thread_pool_t> thread_pool,
                           options_t const *options);

    ~middle_pgsql_history_t() override = default;

    void start() override;
    void stop() override;
    void wait() override {}

    /// Not supported, temporal imports only use node_history().
    void node(osmium::Node const &node) override;

    /// Not supported, temporal imports only use way_history().
    void way(osmium::Way const &way) override;

    /// Not supported, the history middle stores no relations.
    void relation(osmium::Relation const &relation) override;

    /// Store one version of a node in the node history table.
    void node_history(osmium::Node const &node) override;

    /// Store one version of a way (its node list) in the way history table.
    void way_history(osmium::Way const &way) override;

    void after_nodes() override;
    void after_ways() override;
    void after_relations() override;

    std::shared_ptr<middle_query_t> get_query_instance() override;

private:
    void dbexec(std::string_view templ) const;
    std::string render_template(std::string_view templ) const;

    options_t const *m_options;
    params_t m_params;
    pg_conn_t m_db_connection;

    // middle keeps its own thread for writing to the database.
    std::shared_ptr<db_copy_thread_t> m_copy_thread;
    db_copy_mgr_t<db_deleter_by_id_t> m_db_copy;

    std::shared_ptr<db_target_descr_t> m_nodes_history;
    std::shared_ptr<db_target_descr_t> m_ways_history;
}; // class middle_pgsql_history_t

/**
 * Read access to the history tables for the temporal import. Implements
 * the as-of queries used to build geometries for historical way and
 * relation versions. The current-state query interface is not supported
 * and results in an error.
 */
class middle_query_pgsql_history_t : public middle_query_t
{
public:
    middle_query_pgsql_history_t(connection_params_t const &connection_params,
                                 options_t const &options);

    osmium::Location get_node_location(osmid_t id) const override;

    size_t nodes_get_list(osmium::WayNodeList *nodes) const override;

    size_t nodes_get_list_as_of(osmium::WayNodeList *nodes,
                                osmium::Timestamp as_of) const override;

    bool node_get(osmid_t id, osmium::memory::Buffer *buffer) const override;

    bool way_get(osmid_t id, osmium::memory::Buffer *buffer) const override;

    size_t rel_members_get(osmium::Relation const &rel,
                           osmium::memory::Buffer *buffer,
                           osmium::osm_entity_bits::type types) const override;

    size_t rel_members_get_as_of(osmium::Relation const &rel,
                                 osmium::memory::Buffer *buffer,
                                 osmium::osm_entity_bits::type types,
                                 osmium::Timestamp as_of) const override;

    std::map<osmid_t, std::vector<osmium::Timestamp>>
    node_version_timestamps(idlist_t const &ids) const override;

    std::map<osmid_t, std::vector<way_history_version_t>>
    way_histories(idlist_t const &ids) const override;

    bool relation_get(osmid_t id,
                      osmium::memory::Buffer *buffer) const override;

    void prepare(std::string const &stmt, std::string const &sql_cmd) const;

private:
    std::unordered_map<osmid_t, osmium::Location>
    get_node_locations_as_of_db(idlist_t const &ids,
                                osmium::Timestamp as_of) const;

    pg_conn_t m_db_connection;
}; // class middle_query_pgsql_history_t

#endif // OSM2PGSQL_MIDDLE_PGSQL_HISTORY_HPP
