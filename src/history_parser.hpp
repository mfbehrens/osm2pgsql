#ifndef OSM2PGSQL_HISTORY_PARSER_HPP
#define OSM2PGSQL_HISTORY_PARSER_HPP

/**
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This file is part of osm2pgsql (https://osm2pgsql.org/).
 * For a full list of authors see the git log.
 */

/**
 * \file
 *
 * It contains the history_parser_t class.
 */

#include "history_element.hpp"
#include "osmtypes.hpp"

#include <osmium/io/file.hpp>
#include <osmium/memory/buffer.hpp>
#include <osmium/osm/types.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

class osmdata_t;
class progress_display_t;

/**
 * Parser for OSM history files (.osh.pbf).
 *
 * History files are sorted by object type, object id, and version. The
 * parser reads all versions of one object into memory until the object
 * id changes, then computes the temporal validity range for every
 * version and replays all versions through the osmdata_t temporal
 * processing. Because most objects only have one or a few versions, a
 * group easily fits into RAM.
 */
class history_parser_t
{
public:
    history_parser_t(osmdata_t *osmdata, progress_display_t *progress) noexcept
    : m_osmdata(osmdata), m_progress(progress)
    {
    }

    /**
     * Parse a history file, compute the validity range for every object
     * version and forward all versions through the osmdata_t temporal
     * processing.
     */
    void parse(osmium::io::File const &file);

private:
    void process(osmium::OSMObject const &object);

    /// Compute validity ranges and replay all versions of the current
    /// object.
    void flush_group();

    osmdata_t *m_osmdata;

    /// Progress display (counts versions of each object type).
    progress_display_t *m_progress;

    osmium::item_type m_current_type = osmium::item_type::undefined;
    osmid_t m_current_id = 0;
    bool m_in_group = false;

    /// Type of the last object seen, for the after_nodes()/after_ways()
    /// phase transitions.
    osmium::item_type m_last_type = osmium::item_type::node;

    /// Version data (timestamp, version, visible) of the current object.
    std::vector<history_version_t> m_versions;

    /// Copies of all versions of the current object, one buffer each.
    /// Copies are needed because the reader recycles its buffers.
    std::vector<osmium::memory::Buffer> m_copies;

    // Statistics.
    uint64_t m_objects = 0;         ///< number of distinct objects
    uint64_t m_versions_total = 0;  ///< total number of versions
    uint64_t m_single = 0;          ///< objects with exactly one version
    uint64_t m_deleted = 0;         ///< invisible (deleted) versions
    uint64_t m_invalid = 0;         ///< versions with non-monotonic timestamps
    std::size_t m_max_versions = 0; ///< largest version count of one object
};

#endif // OSM2PGSQL_HISTORY_PARSER_HPP
