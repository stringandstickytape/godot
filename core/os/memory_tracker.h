/**************************************************************************/
/*  memory_tracker.h                                                      */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                          */
/*                                                                        */
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including  */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,       */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY    */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace MemoryTracker {

enum class AllocationOrigin : uint8_t {
	ALLOCATION,
	REALLOCATION_OF_PRE_ENABLE_ALLOCATION,
};

struct AllocationRecord {
	uint64_t allocation_id = 0;
	uintptr_t user_base = 0;
	uintptr_t raw_base = 0;
	// Raw return addresses. Zero means unavailable. Offline symbolication must
	// use address - 1 to resolve the instruction containing the call.
	uintptr_t origin_caller_pc = 0;
	// Updated only after a successful ordinary realloc; failed reallocs leave
	// the complete record unchanged.
	uintptr_t last_reallocation_caller_pc = 0;
	uint64_t requested_bytes = 0;
	uint64_t raw_requested_bytes = 0;
	uint64_t allocator_usable_bytes = 0;
	uint64_t created_sequence = 0;
	uint64_t last_resize_sequence = 0;
	uint64_t alignment = 0;
	AllocationOrigin origin = AllocationOrigin::ALLOCATION;
	bool allocator_usable_bytes_available = false;
	bool aligned = false;
	bool prepad = false;
	bool zeroed = false;
};

struct Statistics {
	uint64_t tracker_start_unix_usec = 0;
	uint64_t snapshot_sequence = 0;
	uint64_t active_allocation_count = 0;
	uint64_t active_requested_bytes = 0;
	uint64_t active_raw_requested_bytes = 0;
	uint64_t active_allocator_usable_bytes = 0;
	uint64_t peak_active_allocation_count = 0;
	uint64_t peak_active_requested_bytes = 0;
	uint64_t peak_active_raw_requested_bytes = 0;
	uint64_t peak_active_allocator_usable_bytes = 0;
	uint64_t allocation_count = 0;
	uint64_t reallocation_count = 0;
	uint64_t failed_reallocation_count = 0;
	uint64_t failed_reallocation_without_record_count = 0;
	uint64_t free_count = 0;
	uint64_t free_without_record_count = 0;
	uint64_t adopted_reallocation_count = 0;
	uint64_t duplicate_active_address_count = 0;
	uint64_t dropped_record_count = 0;
	uint64_t snapshot_failure_count = 0;
	uint64_t instrumentation_failure_count = 0;
	uint64_t table_slot_count = 0;
	uint64_t table_tombstone_count = 0;
	uint64_t tracker_storage_current_bytes = 0;
	uint64_t tracker_storage_peak_bytes = 0;
	uint64_t snapshot_copy_bytes = 0;
	bool enable_requested = false;
	bool enable_failed = false;
	bool enabled = false;
	bool inventory_complete = false;
	bool allocator_usable_bytes_supported = false;
	bool caller_pc_requested = false;
	bool caller_pc_enabled = false;
	bool caller_pc_supported = false;
};

struct Snapshot {
	AllocationRecord *records = nullptr;
	uint64_t record_count = 0;
	Statistics statistics;
};

struct CompactionResult {
	Statistics before;
	Statistics after;
	uint64_t target_slot_count = 0;
	uint64_t planned_transient_storage_bytes = 0;
	uint64_t elapsed_usec = 0;
	bool enabled = false;
	bool attempted = false;
	bool success = false;
	bool no_op = false;
	bool storage_shrunk = false;
};

struct ReallocationTransaction {
	uintptr_t old_user_base = 0;
	bool active = false;
	bool previous_suppression = false;
};

struct SelfCheckResult {
	bool passed = false;
	bool allocation_inserted = false;
	bool reallocation_preserved_id = false;
	bool reallocation_preserved_origin_caller_pc = false;
	bool reallocation_updated_caller_pc = false;
	bool failed_reallocation_preserved_record = false;
	bool free_removed_record = false;
	bool address_reuse_changed_id = false;
};

extern std::atomic<bool> enabled;
extern std::atomic<bool> caller_pc_enabled;

inline bool is_enabled() {
	return enabled.load(std::memory_order_acquire);
}

inline bool is_caller_pc_enabled() {
	return caller_pc_enabled.load(std::memory_order_acquire);
}

// May be called once during early process setup. Tracker storage is intentionally
// retained until process termination so allocator hooks remain valid during
// engine and static-object teardown.
bool enable();

void record_allocation(void *p_user_base, void *p_raw_base, size_t p_requested_bytes, size_t p_raw_requested_bytes, bool p_aligned, bool p_prepad, bool p_zeroed, size_t p_alignment, size_t p_allocator_usable_bytes, bool p_allocator_usable_bytes_available, uintptr_t p_caller_pc);
void record_free(void *p_user_base);

// The transaction holds the tracker mutex across the CRT realloc call. This
// prevents a moved allocation's old address from being reused and published by
// another thread before its old record has been removed.
void begin_reallocation(void *p_old_user_base, ReallocationTransaction &r_transaction);
void finish_reallocation(ReallocationTransaction &r_transaction, void *p_new_user_base, void *p_new_raw_base, size_t p_requested_bytes, size_t p_raw_requested_bytes, bool p_prepad, size_t p_allocator_usable_bytes, bool p_allocator_usable_bytes_available, uintptr_t p_caller_pc);
void finish_reallocation_failure(ReallocationTransaction &r_transaction);
void finish_reallocation_free(ReallocationTransaction &r_transaction);

bool query_allocation(const void *p_user_base, AllocationRecord &r_record);
uint64_t get_tracker_start_unix_usec();
bool get_statistics(Statistics &r_statistics);
bool capture_snapshot(Snapshot &r_snapshot);
void release_snapshot(Snapshot &r_snapshot);

// Explicit diagnostic operation. Rebuilds the table at the smallest power-of-two
// capacity that keeps the current live set plus one insertion within the normal
// 70% load limit.
// Never called from allocation/free/snapshot paths.
bool compact_storage(CompactionResult &r_result);

// Exercises the CRT-backed table without touching the live tracker.
SelfCheckResult run_self_check();

} // namespace MemoryTracker
