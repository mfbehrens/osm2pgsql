#ifndef OSM2PGSQL_HISTORY_ELEMENT_HPP
#define OSM2PGSQL_HISTORY_ELEMENT_HPP

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
 * Data types for the temporal history import.
 */

#include <osmium/osm/timestamp.hpp>

#include <cstdint>
#include <string>
/**
 * One version of an OSM object as needed for computing temporal validity
 * ranges. The object type and id are stored in the parser per group.
 */
struct history_version_t
{
    /// When this version was created.
    osmium::Timestamp timestamp{};

    /// Version number of the object.
    uint32_t version = 0;

    /// False if this version is a deletion (tombstone).
    bool visible = true;
};

/**
 * Temporal validity range of one version of an OSM object. The range starts
 * at the timestamp of this version and ends at the timestamp of the next
 * version of the same object. If there is no next version (or the next
 * version is a deletion), see the parser for how the end is determined.
 */
struct valid_range_t
{
    /// Timestamp when this version was created.
    osmium::Timestamp from{};

    /// Timestamp when the next version replaced this one.
    osmium::Timestamp to{};

    /// True if the version is still the newest and the object still exists.
    bool to_infinity = false;

    /// Format the range in PostgreSQL tsrange literal syntax. An
    /// infinite upper bound is written as an empty bound, because the
    /// literal 'infinity' would be stored as a timestamp value, not as
    /// the infinite bound (upper_inf() would return false).
    std::string to_tsrange() const
    {
        std::string result = "[" + from.to_iso();
        if (to_infinity) {
            result += ",)";
        } else {
            result += "," + to.to_iso() + ")";
        }
        return result;
    }
};

/**
 * Validity range of the OSM object version currently being processed.
 * Only set on the replay thread during a temporal history import while a
 * version is being forwarded to the output; nullptr at all other times.
 * The flex output reads this in the object:valid_at() Lua function.
 */
inline thread_local valid_range_t const *current_valid_range = nullptr;

#endif // OSM2PGSQL_HISTORY_ELEMENT_HPP
