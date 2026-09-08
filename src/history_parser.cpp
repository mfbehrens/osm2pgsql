/**
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This file is part of osm2pgsql (https://osm2pgsql.org/).
 *
 * Copyright (C) 2006-2026 by the osm2pgsql developer community.
 * For a full list of authors see the git log.
 */

#include "history_parser.hpp"

#include "idlist.hpp"
#include "input.hpp"
#include "logging.hpp"
#include "middle.hpp"
#include "osmdata.hpp"
#include "progress-display.hpp"

#include <osmium/io/reader.hpp>
#include <osmium/visitor.hpp>

#include <algorithm>
#include <map>
#include <utility>

namespace {

/**
 * Check that the input is ordered by type and id. Unlike the normal
 * input check, the same id may appear multiple times here, because in a
 * history file every version of an object has its own entry.
 */
type_id check_history_input(type_id const &last, osmium::OSMObject const &object)
{
    type_id const curr{object.type(), object.id()};

    if (curr.id < 0) {
        throw fmt_error("Negative OSM object ids are not allowed: {} id {}.",
                        osmium::item_type_to_name(curr.type), curr.id);
    }

    if (last.type == curr.type) {
        if (last.id > curr.id) {
            throw fmt_error("History input data is not ordered: {} id {}"
                            " after {}.",
                            osmium::item_type_to_name(curr.type), curr.id,
                            last.id);
        }
        return curr;
    }

    if (osmium::item_type_to_nwr_index(last.type) >
        osmium::item_type_to_nwr_index(curr.type)) {
        throw fmt_error("History input data is not ordered: {} after {}.",
                        osmium::item_type_to_name(curr.type),
                        osmium::item_type_to_name(last.type));
    }

    return curr;
}

/**
 * Collect the geometry change event times of the given node ids strictly
 * inside the given validity range: every node version potentially changes
 * the geometry of the way or relation referencing it.
 */
void add_node_events(std::vector<osmium::Timestamp> *events,
                     std::vector<osmid_t> const &node_ids,
                     std::map<osmid_t, std::vector<osmium::Timestamp>> const
                         &node_versions,
                     valid_range_t const &range)
{
    for (auto const id : node_ids) {
        auto const it = node_versions.find(id);
        if (it == node_versions.end()) {
            continue;
        }
        for (auto const ts : it->second) {
            if (ts > range.from && (range.to_infinity || ts < range.to)) {
                events->push_back(ts);
            }
        }
    }
}

/** Sort event times and remove duplicates. */
void normalize_events(std::vector<osmium::Timestamp> *events)
{
    std::sort(events->begin(), events->end());
    events->erase(std::unique(events->begin(), events->end()), events->end());
}

} // anonymous namespace

void history_parser_t::parse(osmium::io::File const &file)
{
    log_info("Reading OSM history file '{}'...", file.filename());

    osmium::io::Reader reader{file};
    type_id last{osmium::item_type::node, 0};

    while (osmium::memory::Buffer buffer = reader.read()) {
        for (auto &object : buffer.select<osmium::OSMObject>()) {
            last = check_history_input(last, object);

            if (m_last_type != object.type()) {
                if (m_last_type == osmium::item_type::node) {
                    m_osmdata->after_nodes();
                    m_progress->start_way_counter();
                }
                if (object.type() == osmium::item_type::relation) {
                    m_osmdata->after_ways();
                    m_progress->start_relation_counter();
                }
                m_last_type = object.type();
            }

            osmium::apply_item(object, *m_progress);
            process(object);
        }
    }

    reader.close();

    if (m_in_group) {
        flush_group();
    }

    switch (m_last_type) {
    case osmium::item_type::node:
        m_osmdata->after_nodes();
        // fallthrough
    case osmium::item_type::way:
        m_osmdata->after_ways();
        break;
    default:
        break;
    }
    m_osmdata->after_relations();
    m_progress->print_summary();

    log_info(
        "History file '{}': read {} versions of {} objects"
        " ({} single-version objects, {} deleted versions,"
        " {} invalid ranges, {} geometry segments,"
        " at most {} versions per object).",
        file.filename(), m_versions_total, m_objects, m_single, m_deleted,
        m_invalid, m_segments, m_max_versions);
}

void history_parser_t::process(osmium::OSMObject const &object)
{
    if (!m_in_group || object.type() != m_current_type ||
        object.id() != m_current_id) {
        if (m_in_group) {
            flush_group();
        }
        m_current_type = object.type();
        m_current_id = object.id();
        m_in_group = true;
        ++m_objects;
    }

    // Copy the object, because the reader buffer it lives in is recycled.
    m_copies.emplace_back(object.padded_size(),
                          osmium::memory::Buffer::auto_grow::yes);
    m_copies.back().add_item(object);
    m_copies.back().commit();

    m_versions.push_back(history_version_t{object.timestamp(),
                                           object.version(),
                                           object.visible()});
    ++m_versions_total;
}

void history_parser_t::flush_group()
{
    auto const num_versions = m_versions.size();

    if (num_versions == 1) {
        ++m_single;
    }
    m_max_versions = std::max(m_max_versions, num_versions);

    // A visible version is valid from its own timestamp until the
    // timestamp of the next version. A deleted (invisible) version is a
    // tombstone: it ends the validity of the previous version but has no
    // validity range of its own. The newest visible version of an object
    // that still exists is valid to infinity.
    std::vector<valid_range_t> ranges(num_versions);
    for (std::size_t i = 0; i < num_versions; ++i) {
        if (!m_versions[i].visible) {
            ++m_deleted;
            continue;
        }
        ranges[i].from = m_versions[i].timestamp;
        if (i + 1 < num_versions) {
            ranges[i].to = m_versions[i + 1].timestamp;
            if (ranges[i].to <= ranges[i].from) {
                // Data anomaly: the next version has an earlier (or the
                // same) timestamp than this one. This version's validity
                // can not be expressed as a proper interval; emit an
                // empty range instead.
                ranges[i].to = ranges[i].from;
                ++m_invalid;
            }
        } else {
            ranges[i].to_infinity = true;
        }
        log_debug("History: {} {} v{} valid {}",
                  osmium::item_type_to_name(m_current_type), m_current_id,
                  m_versions[i].version, ranges[i].to_tsrange());
    }

    switch (m_current_type) {
    case osmium::item_type::node:
        // Node geometries are their own locations; one replay per visible
        // version is exact.
        for (std::size_t i = 0; i < num_versions; ++i) {
            auto objects = m_copies[i].select<osmium::OSMObject>();
            m_osmdata->temporal_node(
                static_cast<osmium::Node const &>(*objects.begin()),
                ranges[i]);
        }
        break;
    case osmium::item_type::way:
        replay_way_segments(ranges);
        break;
    case osmium::item_type::relation:
        replay_relation_segments(ranges);
        break;
    default:
        break;
    }

    m_versions.clear();
    m_copies.clear();
    m_in_group = false;
}

void history_parser_t::replay_way_segments(
    std::vector<valid_range_t> const &ranges)
{
    // One query per way group: all version timestamps of all nodes
    // referenced by any version of this way.
    idlist_t node_ids;
    for (auto const &buffer : m_copies) {
        auto const &way =
            static_cast<osmium::Way const &>(*buffer.begin());
        for (auto const &nr : way.nodes()) {
            node_ids.push_back(nr.ref());
        }
    }
    auto const node_versions = m_middle->node_version_timestamps(node_ids);

    auto const num_versions = m_versions.size();
    for (std::size_t i = 0; i < num_versions; ++i) {
        auto objects = m_copies[i].select<osmium::OSMObject>();
        auto &way = static_cast<osmium::Way &>(*objects.begin());

        // Every version goes into the way history exactly once.
        m_osmdata->temporal_way_history(way);

        if (!m_versions[i].visible) {
            // Tombstone: closes the previous version's range, no own row.
            continue;
        }

        std::vector<osmid_t> version_node_ids;
        for (auto const &nr : way.nodes()) {
            version_node_ids.push_back(nr.ref());
        }

        std::vector<osmium::Timestamp> events;
        add_node_events(&events, version_node_ids, node_versions, ranges[i]);
        normalize_events(&events);

        // Replay one segment per event: within a segment no member node
        // changes, so the geometry is constant (and therefore exact).
        osmium::Timestamp start = ranges[i].from;
        for (auto const ts : events) {
            valid_range_t segment;
            segment.from = start;
            segment.to = ts;
            m_osmdata->temporal_way(way, segment);
            ++m_segments;
            start = ts;
        }

        valid_range_t segment;
        segment.from = start;
        segment.to = ranges[i].to;
        segment.to_infinity = ranges[i].to_infinity;
        m_osmdata->temporal_way(way, segment);
        ++m_segments;
    }
}

void history_parser_t::replay_relation_segments(
    std::vector<valid_range_t> const &ranges)
{
    // One query per relation group: the version history of all member
    // ways...  Collect the member way ids and node ids (direct member
    // nodes of all versions plus the nodes of every version of every
    // member way) of all versions of this relation.
    idlist_t way_ids;
    idlist_t node_ids;
    for (auto const &buffer : m_copies) {
        auto const &relation =
            static_cast<osmium::Relation const &>(*buffer.begin());
        for (auto const &member : relation.members()) {
            switch (member.type()) {
            case osmium::item_type::node:
                node_ids.push_back(member.ref());
                break;
            case osmium::item_type::way:
                way_ids.push_back(member.ref());
                break;
            default:
                break;
            }
        }
    }

    auto const way_versions = m_middle->way_histories(way_ids);

    for (auto const &entry : way_versions) {
        for (auto const &version : entry.second) {
            for (auto const id : version.nodes) {
                node_ids.push_back(id);
            }
        }
    }
    auto const node_versions = m_middle->node_version_timestamps(node_ids);

    // The geometry change events of a member way: its own version
    // timestamps plus, per version interval, the node events of that
    // version's node list.
    std::map<osmid_t, std::vector<osmium::Timestamp>> way_events;
    for (auto const &entry : way_versions) {
        auto const &versions = entry.second;
        std::vector<osmium::Timestamp> events;
        for (std::size_t k = 0; k < versions.size(); ++k) {
            events.push_back(versions[k].ts);
            valid_range_t way_range;
            way_range.from = versions[k].ts;
            if (k + 1 < versions.size()) {
                way_range.to = versions[k + 1].ts;
                if (way_range.to <= way_range.from) {
                    way_range.to = way_range.from;
                    continue;
                }
            } else {
                way_range.to_infinity = true;
            }
            if (versions[k].visible) {
                add_node_events(&events, versions[k].nodes, node_versions,
                                way_range);
            }
        }
        normalize_events(&events);
        way_events.emplace(entry.first, std::move(events));
    }

    auto const num_versions = m_versions.size();
    for (std::size_t i = 0; i < num_versions; ++i) {
        auto objects = m_copies[i].select<osmium::OSMObject>();
        auto &relation =
            static_cast<osmium::Relation const &>(*objects.begin());

        std::vector<osmium::Timestamp> events;
        if (m_versions[i].visible) {
            // Geometry events of this version: its member node versions
            // and the geometry events of its member ways.
            std::vector<osmid_t> version_node_ids;
            for (auto const &member : relation.members()) {
                if (member.type() == osmium::item_type::node) {
                    version_node_ids.push_back(member.ref());
                }
            }
            add_node_events(&events, version_node_ids, node_versions,
                            ranges[i]);
            for (auto const &member : relation.members()) {
                if (member.type() != osmium::item_type::way) {
                    continue;
                }
                auto const it = way_events.find(member.ref());
                if (it == way_events.end()) {
                    continue;
                }
                for (auto const ts : it->second) {
                    if (ts > ranges[i].from &&
                        (ranges[i].to_infinity || ts < ranges[i].to)) {
                        events.push_back(ts);
                    }
                }
            }
            normalize_events(&events);
        }

        osmium::Timestamp start = ranges[i].from;
        for (auto const ts : events) {
            valid_range_t segment;
            segment.from = start;
            segment.to = ts;
            m_osmdata->temporal_relation(relation, segment);
            ++m_segments;
            start = ts;
        }

        valid_range_t segment;
        segment.from = start;
        segment.to = ranges[i].to;
        segment.to_infinity = ranges[i].to_infinity;
        m_osmdata->temporal_relation(relation, segment);
        if (m_versions[i].visible) {
            ++m_segments;
        }
    }
}
