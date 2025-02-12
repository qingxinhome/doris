// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "runtime/memory/global_memory_arbitrator.h"

#include <bvar/bvar.h>

#include "runtime/process_profile.h"
#include "runtime/thread_context.h"

namespace doris {

bvar::PassiveStatus<int64_t> g_process_memory_usage(
        "meminfo_process_memory_usage",
        [](void*) { return GlobalMemoryArbitrator::process_memory_usage(); }, nullptr);
bvar::PassiveStatus<int64_t> g_sys_mem_avail(
        "meminfo_sys_mem_avail", [](void*) { return GlobalMemoryArbitrator::sys_mem_available(); },
        nullptr);

std::atomic<int64_t> GlobalMemoryArbitrator::_process_reserved_memory = 0;
std::atomic<int64_t> GlobalMemoryArbitrator::refresh_interval_memory_growth = 0;
std::mutex GlobalMemoryArbitrator::cache_adjust_capacity_lock;
std::condition_variable GlobalMemoryArbitrator::cache_adjust_capacity_cv;
std::atomic<bool> GlobalMemoryArbitrator::cache_adjust_capacity_notify {false};
std::atomic<double> GlobalMemoryArbitrator::last_cache_capacity_adjust_weighted {1};
std::mutex GlobalMemoryArbitrator::memtable_memory_refresh_lock;
std::condition_variable GlobalMemoryArbitrator::memtable_memory_refresh_cv;
std::atomic<bool> GlobalMemoryArbitrator::memtable_memory_refresh_notify {false};

bool GlobalMemoryArbitrator::try_reserve_process_memory(int64_t bytes) {
    if (sys_mem_available() - bytes < MemInfo::sys_mem_available_warning_water_mark()) {
        doris::ProcessProfile::instance()->memory_profile()->print_log_process_usage();
        return false;
    }
    int64_t old_reserved_mem = _process_reserved_memory.load(std::memory_order_relaxed);
    int64_t new_reserved_mem = 0;
    do {
        new_reserved_mem = old_reserved_mem + bytes;
        if (UNLIKELY(PerfCounters::get_vm_rss() +
                             refresh_interval_memory_growth.load(std::memory_order_relaxed) +
                             new_reserved_mem >=
                     MemInfo::soft_mem_limit())) {
            doris::ProcessProfile::instance()->memory_profile()->print_log_process_usage();
            return false;
        }
    } while (!_process_reserved_memory.compare_exchange_weak(old_reserved_mem, new_reserved_mem,
                                                             std::memory_order_relaxed));
    return true;
}

void GlobalMemoryArbitrator::shrink_process_reserved(int64_t bytes) {
    _process_reserved_memory.fetch_sub(bytes, std::memory_order_relaxed);
}

int64_t GlobalMemoryArbitrator::sub_thread_reserve_memory(int64_t bytes) {
    doris::ThreadContext* thread_context = doris::thread_context(true);
    if (thread_context) {
        return bytes - doris::thread_context()->thread_mem_tracker_mgr->reserved_mem();
    }
    return bytes;
}

} // namespace doris
