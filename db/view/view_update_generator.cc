/*
 * Copyright (C) 2018 ScyllaDB
 */

/*
 * This file is part of Scylla.
 *
 * Scylla is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Scylla is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Scylla.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <boost/range/adaptor/map.hpp>
#include "view_update_generator.hh"
#include "service/priority_manager.hh"

static logging::logger vug_logger("view_update_generator");

namespace db::view {

future<> view_update_generator::start() {
    thread_attributes attr;
    attr.sched_group = _db.get_streaming_scheduling_group();
    _started = seastar::async(std::move(attr), [this]() mutable {
        while (!_as.abort_requested()) {
            if (_sstables_with_tables.empty()) {
                _pending_sstables.wait().get();
            }

            // To ensure we don't race with updates, move the entire content
            // into a local variable.
            auto sstables_with_tables = std::exchange(_sstables_with_tables, {});

            // If we got here, we will process all tables we know about so far eventually so there
            // is no starvation
            for (auto table_it = sstables_with_tables.begin(); table_it != sstables_with_tables.end(); table_it = sstables_with_tables.erase(table_it)) {
                auto& [t, sstables] = *table_it;
                schema_ptr s = t->schema();

                vug_logger.trace("Processing {}.{}: {} sstables", s->ks_name(), s->cf_name(), sstables.size());

                const auto num_sstables = sstables.size();

                try {
                    // Exploit the fact that sstables in the staging directory
                    // are usually non-overlapping and use a partitioned set for
                    // the read.
                    auto ssts = make_lw_shared(sstables::make_partitioned_sstable_set(s, make_lw_shared<sstable_list>(sstable_list{}), false));
                    for (auto& sst : sstables) {
                        ssts->insert(sst);
                    }

                    flat_mutation_reader staging_sstable_reader = ::make_range_sstable_reader(s,
                            no_reader_permit(),
                            std::move(ssts),
                            query::full_partition_range,
                            s->full_slice(),
                            service::get_local_streaming_read_priority(),
                            nullptr,
                            ::streamed_mutation::forwarding::no,
                            ::mutation_reader::forwarding::no);

                    auto result = staging_sstable_reader.consume_in_thread(view_updating_consumer(s, *t, sstables, _as), db::no_timeout);
                    if (result == stop_iteration::yes) {
                        break;
                    }
                } catch (...) {
                    vug_logger.warn("Processing {} failed for table {}:{}. Will retry...", s->ks_name(), s->cf_name(), std::current_exception());
                    // Need to add sstables back to the set so we can retry later. By now it may
                    // have had other updates.
                    std::move(sstables.begin(), sstables.end(), std::back_inserter(_sstables_with_tables[t]));
                    break;
                }
                try {
                    // collect all staging sstables to move in a map, grouped by table.
                    std::move(sstables.begin(), sstables.end(), std::back_inserter(_sstables_to_move[t]));
                } catch (...) {
                    // Move from staging will be retried upon restart.
                    vug_logger.warn("Moving {} from staging failed: {}:{}. Ignoring...", s->ks_name(), s->cf_name(), std::current_exception());
                }
                _registration_sem.signal(num_sstables);
            }
            // For each table, move the processed staging sstables into the table's base dir.
            for (auto it = _sstables_to_move.begin(); it != _sstables_to_move.end(); ) {
                auto& [t, sstables] = *it;
                try {
                    t->move_sstables_from_staging(sstables).get();
                } catch (...) {
                    // Move from staging will be retried upon restart.
                    vug_logger.warn("Moving some sstable from staging failed: {}. Ignoring...", std::current_exception());
                }
                it = _sstables_to_move.erase(it);
            }
        }
    });
    return make_ready_future<>();
}

future<> view_update_generator::stop() {
    _as.request_abort();
    _pending_sstables.signal();
    return std::move(_started).then([this] {
        _registration_sem.broken();
    });
}

bool view_update_generator::should_throttle() const {
    return !_started.available();
}

future<> view_update_generator::register_staging_sstable(sstables::shared_sstable sst, lw_shared_ptr<table> table) {
    if (_as.abort_requested()) {
        return make_ready_future<>();
    }
    _sstables_with_tables[table].push_back(std::move(sst));

    _pending_sstables.signal();
    if (should_throttle()) {
        return _registration_sem.wait(1);
    } else {
        _registration_sem.consume(1);
        return make_ready_future<>();
    }
}

}
