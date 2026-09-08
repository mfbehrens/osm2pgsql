/**
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This file is part of osm2pgsql (https://osm2pgsql.org/).
 *
 * Copyright (C) 2006-2026 by the osm2pgsql developer community.
 * For a full list of authors see the git log.
 */

#include "history_parser.hpp"

#include "input.hpp"
#include "logging.hpp"
#include "osmdata.hpp"
#include "progress-display.hpp"

#include <osmium/io/reader.hpp>
#include <osmium/visitor.hpp>

#include <algorithm>

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
        " {} invalid ranges, at most {} versions per object).",
        file.filename(), m_versions_total, m_objects, m_single, m_deleted,
        m_invalid, m_max_versions);
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

    for (std::size_t i = 0; i < num_versions; ++i) {
        auto objects = m_copies[i].select<osmium::OSMObject>();
        auto &object = *objects.begin();

        switch (m_current_type) {
        case osmium::item_type::node:
            m_osmdata->temporal_node(
                static_cast<osmium::Node const &>(object), ranges[i]);
            break;
        case osmium::item_type::way:
            m_osmdata->temporal_way(static_cast<osmium::Way &>(object),
                                    ranges[i]);
            break;
        case osmium::item_type::relation:
            m_osmdata->temporal_relation(
                static_cast<osmium::Relation const &>(object), ranges[i]);
            break;
        default:
            break;
        }
    }

    m_versions.clear();
    m_copies.clear();
    m_in_group = false;
}
