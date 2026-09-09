#ifndef OSM2PGSQL_HISTORY_PARSER_HPP
#define OSM2PGSQL_HISTORY_PARSER_HPP

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
 * It contains the history_parser_t class.
 */

#include <osmium/io/file.hpp>
#include <osmium/memory/buffer.hpp>
#include <osmium/osm/types.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "history_element.hpp"
#include "osmtypes.hpp"

class middle_t;
class osmdata_t;
class progress_display_t;
struct options_t;
class properties_t;
class thread_pool_t;

/**
 * All versions of one object collected for the replay in a worker
 * thread: the version copies, their version data and validity ranges.
 */
struct replay_group_t
{
    /// Copies of all versions of the object, one buffer each.
    std::vector<osmium::memory::Buffer> copies;

    /// Version data (timestamp, version, visible) per version.
    std::vector<history_version_t> versions;

    /// Validity range per version.
    std::vector<valid_range_t> ranges;
};

/// Worker pool replaying way and relation version segments in parallel.
class replay_pool_t;

/**
 * Parser for OSM history files (.osh.pbf).
 *
 * History files are sorted by object type, object id, and version. The
 * parser reads all versions of one object into memory until the object
 * id changes, then computes the temporal validity range for every
 * version and replays all versions through the osmdata_t temporal
 * processing. Because most objects only have one or a few versions, a
 * group easily fits into RAM.
 *
 * The geometry of a way (or relation member geometry) changes whenever
 * the object itself or one of its member nodes (or member ways) gets a
 * new version. To get exact geometries, way and relation version ranges
 * are therefore split at those geometry change events and every segment
 * is replayed separately with the geometry valid at the segment start.
 *
 * Nodes are replayed directly on the reading thread. Ways and relations
 * are collected into batches and handed to replay_pool_t worker
 * threads. Every worker has its own output instance and middle query
 * instance; the node/way history of a whole batch is loaded with one
 * query per batch and all segment geometries are then resolved from
 * that cache without further database round trips.
 */
class history_parser_t
{
public:
    history_parser_t(osmdata_t *osmdata, std::shared_ptr<middle_t> middle,
                     std::shared_ptr<thread_pool_t> thread_pool,
                     options_t const &options, properties_t const &properties,
                     progress_display_t *progress) noexcept;

    ~history_parser_t();

    history_parser_t(history_parser_t const &) = delete;
    history_parser_t &operator=(history_parser_t const &) = delete;

    /**
     * Parse a history file, compute the validity range for every object
     * version and forward all versions through the osmdata_t temporal
     * processing.
     */
    void parse(osmium::io::File const &file);

private:
    void process(osmium::OSMObject const &object);

    /// Compute validity ranges and replay all versions of the current
    /// object. Ways and relations are appended to the current batch.
    void flush_group();

    /// Hand the current batch to the replay pool (if non-empty).
    void flush_batch();

    osmdata_t *m_osmdata;

    /// Middle (for creating the worker query instances).
    std::shared_ptr<middle_t> m_middle;

    /// Thread pool (for creating the worker output instances).
    std::shared_ptr<thread_pool_t> m_thread_pool;

    options_t const *m_options;
    properties_t const *m_properties;

    /// Progress display (counts versions of each object type).
    progress_display_t *m_progress;

    /// Replay worker pool, created when the node phase is done.
    std::unique_ptr<replay_pool_t> m_pool;

    /// Batch of way (or relation) groups waiting to be replayed.
    std::vector<replay_group_t> m_batch;

    /// Type of the objects in the current batch.
    osmium::item_type m_batch_type = osmium::item_type::undefined;

    /// Number of node (or member) references in the current batch, used
    /// to keep the worker-side history cache bounded.
    std::size_t m_batch_refs = 0;

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
