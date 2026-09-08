#ifndef OSM2PGSQL_OSMDATA_HPP
#define OSM2PGSQL_OSMDATA_HPP

/**
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This file is part of osm2pgsql (https://osm2pgsql.org/).
 *
 * Copyright (C) 2006-2026 by the osm2pgsql developer community.
 * For a full list of authors see the git log.
 */

/**
 * \file
 *
 * It contains the osmdata_t class.
 */

#include <memory>
#include <string>

#include <osmium/fwd.hpp>
#include <osmium/handler.hpp>
#include <osmium/osm/box.hpp>

#include "history_element.hpp"
#include "idlist.hpp"
#include "osmtypes.hpp"
#include "pgsql-params.hpp"

class middle_t;
class output_t;
struct options_t;

/**
 * This class guides the processing of the OSM data through its multiple
 * stages. It calls upon the the middle and the output to do their work.
 *
 * It also does the dependency management, keeping track of the dependencies
 * between OSM objects (nodes in ways and members of relations).
 */
class osmdata_t : public osmium::handler::Handler
{
public:
    osmdata_t(std::shared_ptr<middle_t> mid, std::shared_ptr<output_t> output,
              options_t const &options);

    void node(osmium::Node const &node);
    void way(osmium::Way &way);
    void relation(osmium::Relation const &rel);

    /**
     * Temporal history import: Process one version of an object. Every
     * visible version is forwarded to the output together with its
     * validity range (available to the flex output as object:valid_at()).
     * The middle stores every version in its history tables (there are
     * no current-state middle tables in temporal mode). Deleted versions
     * are tombstones: they end the validity of the previous version
     * (which is already recorded in its range) and do not get their own
     * output row.
     *
     * \param object The object version.
     * \param range Validity range of this version.
     */
    void temporal_node(osmium::Node const &node, valid_range_t const &range);

    /**
     * Store a way version in the way history (called once per version,
     * including tombstones).
     */
    void temporal_way_history(osmium::Way const &way);

    /**
     * Replay a way (segment) with the given validity range. Geometry
     * change events split a version's range into several segments, each
     * replayed separately; only visible versions produce output.
     */
    void temporal_way(osmium::Way &way, valid_range_t const &range);
    void temporal_relation(osmium::Relation const &rel,
                           valid_range_t const &range);

    void after_nodes();
    void after_ways();
    void after_relations();

    /**
     * Rest of the processing (stages 1b, 1c, 2, and database postprocessing).
     * This is called once after the input files are processed.
     */
    void stop();

    // These getters are needed only for tests
    idlist_t const &get_pending_way_ids() const noexcept
    {
        return m_ways_pending_tracker;
    }

    idlist_t const &get_pending_relation_ids() const noexcept
    {
        return m_rels_pending_tracker;
    }

private:
    /**
     * Run stage 1b and stage 1c processing: Process dependent objects in
     * append mode.
     */
    void process_dependents();

    /**
     * In append mode all new and changed nodes will be added to this. After
     * all nodes are read this is used to figure out which parent ways and
     * relations reference these nodes. Deleted nodes are not stored in here,
     * because all ways and relations that referenced deleted nodes must be in
     * the change file, too, and so we don't have to find out which ones they
     * are.
     */
    idlist_t m_changed_nodes;

    /**
     * In append mode all new and changed ways will be added to this. After
     * all ways are read this is used to figure out which parent relations
     * reference these ways. Deleted ways are not stored in here, because all
     * relations that referenced deleted ways must be in the change file, too,
     * and so we don't have to find out which ones they are.
     */
    idlist_t m_changed_ways;

    /**
     * In append mode all new and changed relations will be added to this.
     * This is then used to remove already processed relations from the
     * pending list.
     */
    idlist_t m_changed_relations;

    idlist_t m_ways_pending_tracker;
    idlist_t m_rels_pending_tracker;

    std::shared_ptr<middle_t> m_mid;
    std::shared_ptr<output_t> m_output;

    connection_params_t m_connection_params;

    // Bounding box for node import (or invalid Box if everything should be
    // imported).
    osmium::Box m_bbox;

    unsigned int m_num_procs;
    bool m_append;
    bool m_droptemp;
};

#endif // OSM2PGSQL_OSMDATA_HPP
