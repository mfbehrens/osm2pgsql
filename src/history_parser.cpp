/**
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This file is part of osm2pgsql (https://osm2pgsql.org/).
 *
 * Copyright (C) 2006-2026 by the osm2pgsql developer community.
 * For a full list of authors see the git log.
 */

#include "history_parser.hpp"

#include <osmium/io/reader.hpp>
#include <osmium/visitor.hpp>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <utility>

#include "idlist.hpp"
#include "input.hpp"
#include "logging.hpp"
#include "middle.hpp"
#include "options.hpp"
#include "osmdata.hpp"
#include "output.hpp"
#include "progress-display.hpp"
#include "properties.hpp"
#include "thread-pool.hpp"

namespace {

/// Maximum number of object groups collected in one replay batch.
constexpr std::size_t MAX_BATCH_GROUPS = 512;

/// Maximum number of node/member references collected in one replay
/// batch, used to bound the memory of the worker-side history caches.
constexpr std::size_t MAX_BATCH_REFS = 100000;

/**
 * Check that the input is ordered by type and id. Unlike the normal
 * input check, the same id may appear multiple times here, because in
 * a history file every version of an object has its own entry.
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
                     node_history_map_t const &node_history,
                     valid_range_t const &range)
{
    for (auto const id : node_ids) {
        auto const it = node_history.find(id);
        if (it == node_history.end()) {
            continue;
        }
        for (auto const &version : it->second) {
            if (version.ts > range.from &&
                (range.to_infinity || version.ts < range.to)) {
                events->push_back(version.ts);
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

/**
 * Compute the geometry change events of the member ways of a relation
 * from their version histories: the versions of the way itself plus,
 * per version interval, the node events of that version's node list.
 */
std::map<osmid_t, std::vector<osmium::Timestamp>>
build_way_events(way_history_map_t const &way_history,
                 node_history_map_t const &node_history)
{
    std::map<osmid_t, std::vector<osmium::Timestamp>> way_events;
    for (auto const &entry : way_history) {
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
                add_node_events(&events, versions[k].nodes, node_history,
                                way_range);
            }
        }
        normalize_events(&events);
        way_events.emplace(entry.first, std::move(events));
    }
    return way_events;
}

/**
 * Replay all versions of one way group, splitting every version range
 * at the geometry change events of its member nodes.
 */
void replay_way_group(replay_group_t &group, output_t &output,
                      node_history_map_t const &node_history,
                      std::atomic<uint64_t> *segments)
{
    auto const num_versions = group.versions.size();
    for (std::size_t i = 0; i < num_versions; ++i) {
        if (!group.versions[i].visible) {
            // Tombstone: closes the previous version's range, no own row.
            continue;
        }

        auto objects = group.copies[i].select<osmium::OSMObject>();
        auto &way = static_cast<osmium::Way &>(*objects.begin());

        std::vector<osmid_t> version_node_ids;
        for (auto const &nr : way.nodes()) {
            version_node_ids.push_back(nr.ref());
        }

        std::vector<osmium::Timestamp> events;
        add_node_events(&events, version_node_ids, node_history,
                        group.ranges[i]);
        normalize_events(&events);

        // Replay one segment per event: within a segment no member node
        // changes, so the geometry is constant (and therefore exact).
        osmium::Timestamp start = group.ranges[i].from;
        for (auto const ts : events) {
            valid_range_t segment;
            segment.from = start;
            segment.to = ts;
            current_valid_range = &segment;
            output.way_add(&way);
            current_valid_range = nullptr;
            segments->fetch_add(1, std::memory_order_relaxed);
            start = ts;
        }

        valid_range_t segment;
        segment.from = start;
        segment.to = group.ranges[i].to;
        segment.to_infinity = group.ranges[i].to_infinity;
        current_valid_range = &segment;
        output.way_add(&way);
        current_valid_range = nullptr;
        segments->fetch_add(1, std::memory_order_relaxed);
    }
}

/**
 * Replay all versions of one relation group, splitting every version
 * range at the geometry change events of its member nodes and member
 * ways.
 */
void replay_relation_group(
    replay_group_t &group, output_t &output,
    node_history_map_t const &node_history, way_history_map_t const &way_history,
    std::map<osmid_t, std::vector<osmium::Timestamp>> const &way_events,
    std::atomic<uint64_t> *segments)
{
    auto const num_versions = group.versions.size();
    for (std::size_t i = 0; i < num_versions; ++i) {
        if (!group.versions[i].visible) {
            continue;
        }

        auto objects = group.copies[i].select<osmium::OSMObject>();
        auto const &relation =
            static_cast<osmium::Relation const &>(*objects.begin());

        if (relation.members().size() > 32767) {
            log_warn("Relation id {} ignored, because it has more than 32767"
                     " members",
                     relation.id());
            continue;
        }

        std::vector<osmium::Timestamp> events;
        // Geometry events of this version: its member node versions and
        // the geometry events of its member ways.
        std::vector<osmid_t> version_node_ids;
        for (auto const &member : relation.members()) {
            if (member.type() == osmium::item_type::node) {
                version_node_ids.push_back(member.ref());
            }
        }
        add_node_events(&events, version_node_ids, node_history,
                        group.ranges[i]);
        for (auto const &member : relation.members()) {
            if (member.type() != osmium::item_type::way) {
                continue;
            }
            auto const it = way_events.find(member.ref());
            if (it == way_events.end()) {
                continue;
            }
            for (auto const ts : it->second) {
                if (ts > group.ranges[i].from &&
                    (group.ranges[i].to_infinity ||
                     ts < group.ranges[i].to)) {
                    events.push_back(ts);
                }
            }
        }
        normalize_events(&events);

        osmium::Timestamp start = group.ranges[i].from;
        for (auto const ts : events) {
            valid_range_t segment;
            segment.from = start;
            segment.to = ts;
            current_valid_range = &segment;
            output.relation_add(relation);
            current_valid_range = nullptr;
            segments->fetch_add(1, std::memory_order_relaxed);
            start = ts;
        }

        valid_range_t segment;
        segment.from = start;
        segment.to = group.ranges[i].to;
        segment.to_infinity = group.ranges[i].to_infinity;
        current_valid_range = &segment;
        output.relation_add(relation);
        current_valid_range = nullptr;
        segments->fetch_add(1, std::memory_order_relaxed);
    }
}

/**
 * Replay a batch of way or relation groups: load the node (and way)
 * history of the whole batch in one query each, then replay all groups
 * with all segment geometries resolved from those caches.
 */
void replay_batch(osmium::item_type type, std::vector<replay_group_t> &groups,
                  middle_query_t const &middle, output_t &output,
                  std::atomic<uint64_t> *segments)
{
    if (type == osmium::item_type::way) {
        idlist_t node_ids;
        for (auto const &group : groups) {
            for (auto const &buffer : group.copies) {
                auto const &way =
                    static_cast<osmium::Way const &>(*buffer.begin());
                for (auto const &nr : way.nodes()) {
                    node_ids.push_back(nr.ref());
                }
            }
        }
        auto const &node_history = middle.load_node_history(node_ids);
        for (auto &group : groups) {
            replay_way_group(group, output, node_history, segments);
        }
        return;
    }

    idlist_t way_ids;
    idlist_t node_ids;
    for (auto const &group : groups) {
        for (auto const &buffer : group.copies) {
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
    }

    auto const &way_history = middle.load_way_history(way_ids);
    for (auto const &entry : way_history) {
        for (auto const &version : entry.second) {
            for (auto const id : version.nodes) {
                node_ids.push_back(id);
            }
        }
    }
    auto const &node_history = middle.load_node_history(node_ids);

    auto const way_events = build_way_events(way_history, node_history);
    for (auto &group : groups) {
        replay_relation_group(group, output, node_history, way_history,
                              way_events, segments);
    }
}

} // anonymous namespace

/**
 * Pool of worker threads replaying way and relation version segments.
 *
 * Every worker has its own output instance (with its own Lua state and
 * copy thread) and its own middle query instance (with its own history
 * cache). The reading thread collects object groups into batches and
 * submits them; the queue is bounded, so memory stays limited. Worker
 * exceptions are captured and rethrown by wait_all()/submit().
 */
class replay_pool_t
{
public:
    replay_pool_t(options_t const &options,
                  std::shared_ptr<middle_t> const &middle,
                  std::shared_ptr<thread_pool_t> const &thread_pool,
                  properties_t const &properties, std::size_t num_workers)
    : m_max_queue(num_workers * 2)
    {
        assert(middle);
        assert(num_workers > 0);

        // Set up the workers on the reading thread first, so a failure
        // (bad database connection, Lua error) propagates immediately.
        for (std::size_t i = 0; i < num_workers; ++i) {
            auto midq = middle->get_query_instance();
            m_outputs.push_back(output_t::create_output(
                midq, thread_pool, options, properties));
            m_midqs.push_back(std::move(midq));
        }

        log_info("Starting {} temporal replay workers.", num_workers);

        for (std::size_t i = 0; i < num_workers; ++i) {
            m_threads.emplace_back([this, i] {
                worker_run(*m_midqs[i], *m_outputs[i]);
            });
        }
    }

    ~replay_pool_t()
    {
        {
            std::lock_guard<std::mutex> const lock{m_mutex};
            m_stop = true;
        }
        m_cv_item.notify_all();
        m_cv_space.notify_all();
        for (auto &thread : m_threads) {
            thread.join();
        }
        // The output instances (and with them their copy threads) are
        // destroyed after the workers have been joined, so all buffered
        // rows are flushed to the database.
    }

    replay_pool_t(replay_pool_t const &) = delete;
    replay_pool_t &operator=(replay_pool_t const &) = delete;

    /// Submit a batch for replay. Blocks while the queue is full.
    void submit(osmium::item_type type, std::vector<replay_group_t> &&groups)
    {
        std::unique_lock<std::mutex> lock{m_mutex};
        check_error();
        m_cv_space.wait(lock, [this] {
            return m_queue.size() < m_max_queue || m_stop;
        });
        check_error();
        m_queue.emplace_back(type, std::move(groups));
        lock.unlock();
        m_cv_item.notify_one();
    }

    /// Wait until all submitted batches have been replayed.
    void wait_all()
    {
        std::unique_lock<std::mutex> lock{m_mutex};
        m_cv_done.wait(lock, [this] { return m_queue.empty() && m_active == 0; });
        check_error();
    }

    /// Number of segments replayed so far.
    uint64_t segments() const noexcept
    {
        return m_segments.load(std::memory_order_relaxed);
    }

private:
    /// Rethrow a worker exception, if any. Caller must hold the mutex.
    void check_error()
    {
        if (m_exception) {
            std::rethrow_exception(m_exception);
        }
        if (m_stop) {
            throw std::runtime_error{"temporal replay workers have stopped"};
        }
    }

    void worker_run(middle_query_t &midq, output_t &output)
    {
        for (;;) {
            osmium::item_type type;
            std::vector<replay_group_t> groups;
            {
                std::unique_lock<std::mutex> lock{m_mutex};
                m_cv_item.wait(lock, [this] {
                    return m_stop || !m_queue.empty();
                });
                if (m_queue.empty()) {
                    // Stopped: flush the remaining rows of this output.
                    break;
                }
                type = m_queue.front().first;
                groups = std::move(m_queue.front().second);
                m_queue.pop_front();
                ++m_active;
                m_cv_space.notify_all();
            }

            try {
                replay_batch(type, groups, midq, output, &m_segments);
            } catch (...) {
                std::lock_guard<std::mutex> const lock{m_mutex};
                if (!m_exception) {
                    m_exception = std::current_exception();
                }
                m_stop = true;
                m_queue.clear();
                --m_active;
                m_cv_item.notify_all();
                m_cv_space.notify_all();
                m_cv_done.notify_all();
                return;
            }

            {
                std::lock_guard<std::mutex> const lock{m_mutex};
                --m_active;
                if (m_queue.empty()) {
                    m_cv_done.notify_all();
                }
            }
        }

        // The copy manager of the output only hands rows over to the
        // copy thread when the target table changes; everything since
        // the last table switch is still buffered here and has to be
        // flushed explicitly.
        output.sync();
    }

    std::vector<std::thread> m_threads;
    std::vector<std::shared_ptr<middle_query_t>> m_midqs;
    std::vector<std::shared_ptr<output_t>> m_outputs;

    std::deque<std::pair<osmium::item_type, std::vector<replay_group_t>>>
        m_queue;
    std::size_t m_active = 0;
    bool m_stop = false;
    std::size_t const m_max_queue;

    std::mutex m_mutex;
    std::condition_variable m_cv_item;
    std::condition_variable m_cv_space;
    std::condition_variable m_cv_done;
    std::exception_ptr m_exception;

    std::atomic<uint64_t> m_segments{0};
};

history_parser_t::history_parser_t(
    osmdata_t *osmdata, std::shared_ptr<middle_t> middle,
    std::shared_ptr<thread_pool_t> thread_pool, options_t const &options,
    properties_t const &properties, progress_display_t *progress) noexcept
: m_osmdata(osmdata), m_middle(std::move(middle)),
  m_thread_pool(std::move(thread_pool)), m_options(&options),
  m_properties(&properties), m_progress(progress)
{
}

history_parser_t::~history_parser_t() = default;

void history_parser_t::parse(osmium::io::File const &file)
{
    log_info("Reading OSM history file '{}'...", file.filename());

    osmium::io::Reader reader{file};
    type_id last{osmium::item_type::node, 0};

    while (osmium::memory::Buffer buffer = reader.read()) {
        for (auto &object : buffer.select<osmium::OSMObject>()) {
            last = check_history_input(last, object);

            if (m_last_type != object.type()) {
                // Flush the last group of the previous type before the
                // phase transition, so its history rows are written while
                // the middle is still in the previous phase.
                if (m_in_group) {
                    flush_group();
                }

                if (m_last_type == osmium::item_type::node) {
                    m_osmdata->after_nodes();
                    m_progress->start_way_counter();
                    m_pool = std::make_unique<replay_pool_t>(
                        *m_options, m_middle, m_thread_pool, *m_properties,
                        std::max<std::size_t>(1, m_options->num_procs));
                }
                if (object.type() == osmium::item_type::relation) {
                    flush_batch();
                    m_pool->wait_all();
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
    if (m_pool) {
        flush_batch();
        m_pool->wait_all();
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

    auto const segments = m_pool ? m_pool->segments() : 0;
    log_info(
        "History file '{}': read {} versions of {} objects"
        " ({} single-version objects, {} deleted versions,"
        " {} invalid ranges, {} geometry segments,"
        " at most {} versions per object).",
        file.filename(), m_versions_total, m_objects, m_single, m_deleted,
        m_invalid, segments, m_max_versions);
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
        // version is exact. Replayed on the reading thread, because the
        // node history rows are needed by the later phases.
        for (std::size_t i = 0; i < num_versions; ++i) {
            auto objects = m_copies[i].select<osmium::OSMObject>();
            m_osmdata->temporal_node(
                static_cast<osmium::Node const &>(*objects.begin()),
                ranges[i]);
        }
        break;
    case osmium::item_type::way:
        // Every version goes into the way history exactly once (written
        // on the reading thread) and the version segments are replayed
        // by a worker.
        for (std::size_t i = 0; i < num_versions; ++i) {
            auto objects = m_copies[i].select<osmium::OSMObject>();
            m_osmdata->temporal_way_history(
                static_cast<osmium::Way const &>(*objects.begin()));
            m_batch_refs +=
                static_cast<osmium::Way const &>(*objects.begin())
                    .nodes()
                    .size();
        }
        m_batch_type = osmium::item_type::way;
        m_batch.push_back(replay_group_t{std::move(m_copies),
                                         std::move(m_versions),
                                         std::move(ranges)});
        if (m_batch.size() >= MAX_BATCH_GROUPS ||
            m_batch_refs >= MAX_BATCH_REFS) {
            flush_batch();
        }
        break;
    case osmium::item_type::relation:
        for (std::size_t i = 0; i < num_versions; ++i) {
            auto objects = m_copies[i].select<osmium::OSMObject>();
            m_batch_refs +=
                static_cast<osmium::Relation const &>(*objects.begin())
                    .members()
                    .size();
        }
        m_batch_type = osmium::item_type::relation;
        m_batch.push_back(replay_group_t{std::move(m_copies),
                                         std::move(m_versions),
                                         std::move(ranges)});
        if (m_batch.size() >= MAX_BATCH_GROUPS ||
            m_batch_refs >= MAX_BATCH_REFS) {
            flush_batch();
        }
        break;
    default:
        break;
    }

    m_versions.clear();
    m_copies.clear();
    m_in_group = false;
}

void history_parser_t::flush_batch()
{
    if (m_batch.empty()) {
        return;
    }

    m_pool->submit(m_batch_type, std::move(m_batch));
    m_batch.clear();
    m_batch_refs = 0;
}
