/* **********************************************************
 * Copyright (c) 2017-2023 Google, Inc.  All rights reserved.
 * **********************************************************/

/*
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 *
 * * Neither the name of Google, Inc. nor the names of its contributors may be
 *   used to endorse or promote products derived from this software without
 *   specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL VMWARE, INC. OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 */

#define NOMINMAX // Avoid windows.h messing up std::max.

#include "schedule_stats.h"

#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <cassert>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "analysis_tool.h"
#include "memref.h"
#include "trace_entry.h"
#include "utils.h"

namespace dynamorio {
namespace drmemtrace {

const std::string schedule_stats_t::TOOL_NAME = "Schedule stats tool";

analysis_tool_t *
schedule_stats_tool_create(uint64_t print_every, unsigned int verbose)
{
    return new schedule_stats_t(print_every, verbose);
}

schedule_stats_t::schedule_stats_t(uint64_t print_every, unsigned int verbose)
    : knob_print_every_(print_every)
    , knob_verbose_(verbose)
{
    // Empty.
}

schedule_stats_t::~schedule_stats_t()
{
    for (auto &iter : shard_map_) {
        delete iter.second;
    }
}

std::string
schedule_stats_t::initialize_stream(memtrace_stream_t *serial_stream)
{
    if (serial_stream != nullptr)
        return "Only core-sharded operation is supported";
    return "";
}

std::string
schedule_stats_t::initialize_shard_type(shard_type_t shard_type)
{
    if (shard_type != SHARD_BY_CORE)
        return "Only core-sharded operation is supported";
    return "";
}

bool
schedule_stats_t::process_memref(const memref_t &memref)
{
    error_string_ = "Only core-sharded operation is supported.";
    return false;
}

bool
schedule_stats_t::parallel_shard_supported()
{
    return true;
}

void *
schedule_stats_t::parallel_shard_init_stream(int shard_index, void *worker_data,
                                             memtrace_stream_t *stream)
{
    auto per_shard = new per_shard_t;
    std::lock_guard<std::mutex> guard(shard_map_mutex_);
    per_shard->stream = stream;
    per_shard->core = stream->get_output_cpuid();
    shard_map_[shard_index] = per_shard;
    return reinterpret_cast<void *>(per_shard);
}

bool
schedule_stats_t::parallel_shard_exit(void *shard_data)
{
    // Nothing (we read the shard data in print_results).
    return true;
}

std::string
schedule_stats_t::parallel_shard_error(void *shard_data)
{
    per_shard_t *per_shard = reinterpret_cast<per_shard_t *>(shard_data);
    return per_shard->error;
}

bool
schedule_stats_t::parallel_shard_memref(void *shard_data, const memref_t &memref)
{
    static constexpr char THREAD_LETTER_START = 'A';
    static constexpr char THREAD_SEPARATOR = ',';
    static constexpr char WAIT_SYMBOL = '-';
    per_shard_t *shard = reinterpret_cast<per_shard_t *>(shard_data);
    if (knob_verbose_ >= 4) {
        std::ostringstream line;
        line << "Core #" << std::setw(2) << shard->core << " @" << std::setw(9)
             << shard->stream->get_record_ordinal() << " refs, " << std::setw(9)
             << shard->stream->get_instruction_ordinal() << " instrs: input "
             << std::setw(4) << shard->stream->get_input_id() << " @" << std::setw(9)
             << shard->stream->get_input_interface()->get_record_ordinal() << " refs, "
             << std::setw(9)
             << shard->stream->get_input_interface()->get_instruction_ordinal()
             << " instrs: " << std::setw(16) << trace_type_names[memref.marker.type];
        if (type_is_instr(memref.instr.type))
            line << " pc=" << std::hex << memref.instr.addr << std::dec;
        else if (memref.marker.type == TRACE_TYPE_MARKER) {
            line << " " << memref.marker.marker_type
                 << " val=" << memref.marker.marker_value;
        }
        line << "\n";
        std::cerr << line.str();
    }
    if (memref.marker.type == TRACE_TYPE_MARKER &&
        memref.marker.marker_type == TRACE_MARKER_TYPE_CORE_WAIT) {
        ++shard->counters.waits;
        if (!shard->prev_was_wait) {
            shard->thread_sequence += '-';
            shard->cur_segment_instrs = 0;
            shard->prev_was_wait = true;
        } else {
            ++shard->cur_segment_instrs;
            if (shard->cur_segment_instrs == knob_print_every_) {
                shard->thread_sequence += WAIT_SYMBOL;
            }
        }
        return true;
    }
    int64_t input = shard->stream->get_input_id();
    if (input != shard->prev_input) {
        // We convert to letters which only works well for <=26 inputs.
        if (!shard->thread_sequence.empty()) {
            ++shard->counters.total_switches;
            if (shard->saw_maybe_blocking || shard->saw_exit)
                ++shard->counters.voluntary_switches;
            if (shard->direct_switch_target == memref.marker.tid)
                ++shard->counters.direct_switches;
            // A comma separating each sequence makes it a little easier to
            // read, and helps distinguish a switch from two threads with the
            // same %26 letter.  (We could remove this though to compact it.)
            shard->thread_sequence += THREAD_SEPARATOR;
        }
        shard->thread_sequence += THREAD_LETTER_START + static_cast<char>(input % 26);
        shard->cur_segment_instrs = 0;
        if (knob_verbose_ >= 2) {
            std::ostringstream line;
            line << "Core #" << std::setw(2) << shard->core << " @" << std::setw(9)
                 << shard->stream->get_record_ordinal() << " refs, " << std::setw(9)
                 << shard->stream->get_instruction_ordinal() << " instrs: input "
                 << std::setw(4) << input << " @" << std::setw(9)
                 << shard->stream->get_input_interface()->get_record_ordinal()
                 << " refs, " << std::setw(9)
                 << shard->stream->get_input_interface()->get_instruction_ordinal()
                 << " instrs, time "
                 << std::setw(16)
                 // TODO i#5843: For time quanta, provide some way to get the
                 // latest time and print that here instead of the the timestamp?
                 << shard->stream->get_input_interface()->get_last_timestamp()
                 << " == thread " << memref.instr.tid << "\n";
            std::cerr << line.str();
        }
        shard->prev_input = input;
    }
    if (type_is_instr(memref.instr.type)) {
        ++shard->counters.instrs;
        ++shard->cur_segment_instrs;
        if (shard->cur_segment_instrs == knob_print_every_) {
            shard->thread_sequence += THREAD_LETTER_START + static_cast<char>(input % 26);
            shard->cur_segment_instrs = 0;
        }
        shard->direct_switch_target = INVALID_THREAD_ID;
        shard->saw_maybe_blocking = false;
        shard->saw_exit = false;
    }
    if (memref.instr.tid != INVALID_THREAD_ID)
        shard->counters.threads.insert(memref.instr.tid);
    if (memref.marker.type == TRACE_TYPE_MARKER) {
        if (memref.marker.marker_type == TRACE_MARKER_TYPE_SYSCALL)
            ++shard->counters.syscalls;
        if (memref.marker.marker_type == TRACE_MARKER_TYPE_MAYBE_BLOCKING_SYSCALL) {
            ++shard->counters.maybe_blocking_syscalls;
            shard->saw_maybe_blocking = true;
        }
        if (memref.marker.marker_type == TRACE_MARKER_TYPE_DIRECT_THREAD_SWITCH) {
            ++shard->counters.direct_switch_requests;
            shard->direct_switch_target = memref.marker.marker_value;
        }
    } else if (memref.exit.type == TRACE_TYPE_THREAD_EXIT)
        shard->saw_exit = true;
    shard->prev_was_wait = false;
    return true;
}

void
schedule_stats_t::print_counters(const counters_t &counters)
{
    std::cerr << std::setw(12) << counters.threads.size() << " threads\n";
    std::cerr << std::setw(12) << counters.instrs << " instructions\n";
    std::cerr << std::setw(12) << counters.total_switches << " total context switches\n";
    std::cerr << std::setw(12) << std::fixed << std::setprecision(7)
              << (1000 * counters.total_switches / static_cast<double>(counters.instrs))
              << " CSPKI (context switches per 1000 instructions)\n";
    std::cerr << std::setw(12) << std::fixed << std::setprecision(0)
              << (counters.instrs / static_cast<double>(counters.total_switches))
              << " instructions per context switch\n";
    std::cerr << std::setw(12) << std::fixed << std::setprecision(7)
              << counters.voluntary_switches << " voluntary context switches\n";
    std::cerr << std::setw(12) << counters.direct_switches
              << " direct context switches\n";
    if (counters.total_switches > 0) {
        std::cerr << std::setw(12) << std::setprecision(2)
                  << 100 *
                (counters.voluntary_switches /
                 static_cast<double>(counters.total_switches))
                  << "% voluntary switches\n";
        std::cerr << std::setw(12) << std::setprecision(2)
                  << 100 *
                (counters.direct_switches / static_cast<double>(counters.total_switches))
                  << "% direct switches\n";
    }
    std::cerr << std::setw(12) << counters.syscalls << " system calls\n";
    std::cerr << std::setw(12) << counters.maybe_blocking_syscalls
              << " maybe-blocking system calls\n";
    std::cerr << std::setw(12) << counters.direct_switch_requests
              << " direct switch requests\n";
    std::cerr << std::setw(12) << counters.waits << " waits\n";
}

bool
schedule_stats_t::print_results()
{
    std::cerr << TOOL_NAME << " results:\n";
    std::cerr << "Total counts:\n";
    counters_t total;
    for (const auto &shard : shard_map_) {
        total += shard.second->counters;
    }
    std::cerr << std::setw(12) << shard_map_.size() << " cores\n";
    print_counters(total);
    for (const auto &shard : shard_map_) {
        std::cerr << "Core #" << shard.second->core << " counts:\n";
        print_counters(shard.second->counters);
    }
    for (const auto &shard : shard_map_) {
        std::cerr << "Core #" << shard.second->core
                  << " schedule: " << shard.second->thread_sequence << "\n";
    }
    return true;
}

} // namespace drmemtrace
} // namespace dynamorio