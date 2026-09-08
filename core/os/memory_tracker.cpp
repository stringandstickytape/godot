/**************************************************************************/
/*  memory_tracker.cpp                                                    */
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

#include "memory_tracker.h"

#include <chrono>
#include <cstdlib>
#include <mutex>
#include <new>

namespace MemoryTracker {

std::atomic<bool> enabled{ false };

namespace {

enum SlotState : uint8_t {
	SLOT_EMPTY,
	SLOT_OCCUPIED,
	SLOT_TOMBSTONE,
};

struct Slot {
	AllocationRecord record;
	SlotState state = SLOT_EMPTY;
};

struct TrackerState {
	std::mutex mutex;
	Slot *slots = nullptr;
	size_t slot_count = 0;
	size_t active_count = 0;
	size_t tombstone_count = 0;
	uint64_t next_allocation_id = 1;
	uint64_t sequence = 0;
	Statistics statistics;
};

constexpr size_t INITIAL_SLOT_COUNT = 1024;
constexpr size_t MAX_OCCUPANCY_NUMERATOR = 7;
constexpr size_t MAX_OCCUPANCY_DENOMINATOR = 10;

std::atomic<bool> enable_requested{ false };
std::atomic<bool> enable_failed{ false };
TrackerState *tracker_state = nullptr;
thread_local bool tracker_suppressed = false;

struct ScopedSuppression {
	bool previous = false;
	ScopedSuppression() {
		previous = tracker_suppressed;
		tracker_suppressed = true;
	}
	~ScopedSuppression() {
		tracker_suppressed = previous;
	}
};

static uint64_t _hash_pointer(uintptr_t p_pointer) {
	uint64_t value = static_cast<uint64_t>(p_pointer);
	value ^= value >> 30;
	value *= UINT64_C(0xbf58476d1ce4e5b9);
	value ^= value >> 27;
	value *= UINT64_C(0x94d049bb133111eb);
	value ^= value >> 31;
	return value;
}

static size_t _find_index(const TrackerState &p_state, uintptr_t p_user_base) {
	if (p_state.slot_count == 0) {
		return SIZE_MAX;
	}
	size_t index = static_cast<size_t>(_hash_pointer(p_user_base)) & (p_state.slot_count - 1);
	for (size_t probe = 0; probe < p_state.slot_count; probe++) {
		const Slot &slot = p_state.slots[index];
		if (slot.state == SLOT_EMPTY) {
			return SIZE_MAX;
		}
		if (slot.state == SLOT_OCCUPIED && slot.record.user_base == p_user_base) {
			return index;
		}
		index = (index + 1) & (p_state.slot_count - 1);
	}
	return SIZE_MAX;
}

static bool _insert_without_growth(TrackerState &r_state, const AllocationRecord &p_record) {
	size_t index = static_cast<size_t>(_hash_pointer(p_record.user_base)) & (r_state.slot_count - 1);
	size_t first_tombstone = SIZE_MAX;
	for (size_t probe = 0; probe < r_state.slot_count; probe++) {
		Slot &slot = r_state.slots[index];
		if (slot.state == SLOT_OCCUPIED && slot.record.user_base == p_record.user_base) {
			slot.record = p_record;
			return true;
		}
		if (slot.state == SLOT_TOMBSTONE && first_tombstone == SIZE_MAX) {
			first_tombstone = index;
		}
		if (slot.state == SLOT_EMPTY) {
			const size_t destination = first_tombstone != SIZE_MAX ? first_tombstone : index;
			Slot &target = r_state.slots[destination];
			if (target.state == SLOT_TOMBSTONE) {
				r_state.tombstone_count--;
			}
			target.record = p_record;
			target.state = SLOT_OCCUPIED;
			r_state.active_count++;
			return true;
		}
		index = (index + 1) & (r_state.slot_count - 1);
	}
	if (first_tombstone != SIZE_MAX) {
		Slot &target = r_state.slots[first_tombstone];
		target.record = p_record;
		target.state = SLOT_OCCUPIED;
		r_state.active_count++;
		r_state.tombstone_count--;
		return true;
	}
	return false;
}

static uint64_t _tracker_storage_bytes(const TrackerState &p_state) {
	return sizeof(TrackerState) + p_state.slot_count * sizeof(Slot);
}

static void _update_tracker_storage_statistics(TrackerState &r_state) {
	r_state.statistics.table_slot_count = r_state.slot_count;
	r_state.statistics.table_tombstone_count = r_state.tombstone_count;
	r_state.statistics.tracker_storage_current_bytes = _tracker_storage_bytes(r_state);
	if (r_state.statistics.tracker_storage_current_bytes > r_state.statistics.tracker_storage_peak_bytes) {
		r_state.statistics.tracker_storage_peak_bytes = r_state.statistics.tracker_storage_current_bytes;
	}
}

static bool _rehash(TrackerState &r_state, size_t p_new_slot_count) {
	if (p_new_slot_count < INITIAL_SLOT_COUNT) {
		p_new_slot_count = INITIAL_SLOT_COUNT;
	}
	if (p_new_slot_count > SIZE_MAX / sizeof(Slot)) {
		return false;
	}
	Slot *new_slots = static_cast<Slot *>(calloc(p_new_slot_count, sizeof(Slot)));
	if (new_slots == nullptr) {
		return false;
	}

	Slot *old_slots = r_state.slots;
	const size_t old_slot_count = r_state.slot_count;
	const size_t old_active_count = r_state.active_count;
	const size_t old_tombstone_count = r_state.tombstone_count;
	const uint64_t growth_peak_bytes = sizeof(TrackerState) + (old_slot_count + p_new_slot_count) * sizeof(Slot);
	if (growth_peak_bytes > r_state.statistics.tracker_storage_peak_bytes) {
		r_state.statistics.tracker_storage_peak_bytes = growth_peak_bytes;
	}
	r_state.slots = new_slots;
	r_state.slot_count = p_new_slot_count;
	r_state.active_count = 0;
	r_state.tombstone_count = 0;
	for (size_t i = 0; i < old_slot_count; i++) {
		if (old_slots[i].state == SLOT_OCCUPIED && !_insert_without_growth(r_state, old_slots[i].record)) {
			free(new_slots);
			r_state.slots = old_slots;
			r_state.slot_count = old_slot_count;
			r_state.active_count = old_active_count;
			r_state.tombstone_count = old_tombstone_count;
			return false;
		}
	}
	free(old_slots);
	_update_tracker_storage_statistics(r_state);
	return true;
}

static bool _ensure_insert_capacity(TrackerState &r_state) {
	if (r_state.slot_count == 0) {
		return _rehash(r_state, INITIAL_SLOT_COUNT);
	}
	const size_t occupied_or_deleted = r_state.active_count + r_state.tombstone_count + 1;
	if (occupied_or_deleted * MAX_OCCUPANCY_DENOMINATOR <= r_state.slot_count * MAX_OCCUPANCY_NUMERATOR) {
		return true;
	}
	if (r_state.active_count * 2 < r_state.slot_count) {
		return _rehash(r_state, r_state.slot_count);
	}
	if (r_state.slot_count > SIZE_MAX / 2) {
		return false;
	}
	return _rehash(r_state, r_state.slot_count * 2);
}

static void _subtract_live_totals(TrackerState &r_state, const AllocationRecord &p_record) {
	r_state.statistics.active_requested_bytes -= p_record.requested_bytes;
	r_state.statistics.active_raw_requested_bytes -= p_record.raw_requested_bytes;
	if (p_record.allocator_usable_bytes_available) {
		r_state.statistics.active_allocator_usable_bytes -= p_record.allocator_usable_bytes;
	}
}

static void _add_live_totals(TrackerState &r_state, const AllocationRecord &p_record) {
	r_state.statistics.active_requested_bytes += p_record.requested_bytes;
	r_state.statistics.active_raw_requested_bytes += p_record.raw_requested_bytes;
	if (p_record.allocator_usable_bytes_available) {
		r_state.statistics.active_allocator_usable_bytes += p_record.allocator_usable_bytes;
	}
	r_state.statistics.active_allocation_count = r_state.active_count;
	if (r_state.statistics.active_allocation_count > r_state.statistics.peak_active_allocation_count) {
		r_state.statistics.peak_active_allocation_count = r_state.statistics.active_allocation_count;
	}
	if (r_state.statistics.active_requested_bytes > r_state.statistics.peak_active_requested_bytes) {
		r_state.statistics.peak_active_requested_bytes = r_state.statistics.active_requested_bytes;
	}
	if (r_state.statistics.active_raw_requested_bytes > r_state.statistics.peak_active_raw_requested_bytes) {
		r_state.statistics.peak_active_raw_requested_bytes = r_state.statistics.active_raw_requested_bytes;
	}
	if (r_state.statistics.active_allocator_usable_bytes > r_state.statistics.peak_active_allocator_usable_bytes) {
		r_state.statistics.peak_active_allocator_usable_bytes = r_state.statistics.active_allocator_usable_bytes;
	}
}

static void _erase_index(TrackerState &r_state, size_t p_index) {
	r_state.slots[p_index].state = SLOT_TOMBSTONE;
	r_state.active_count--;
	r_state.tombstone_count++;
	r_state.statistics.active_allocation_count = r_state.active_count;
	r_state.statistics.table_tombstone_count = r_state.tombstone_count;
}

static AllocationRecord _make_record(TrackerState &r_state, void *p_user_base, void *p_raw_base, size_t p_requested_bytes, size_t p_raw_requested_bytes, bool p_aligned, bool p_prepad, bool p_zeroed, size_t p_alignment, size_t p_allocator_usable_bytes, bool p_allocator_usable_bytes_available, AllocationOrigin p_origin) {
	AllocationRecord record;
	record.allocation_id = r_state.next_allocation_id++;
	record.user_base = reinterpret_cast<uintptr_t>(p_user_base);
	record.raw_base = reinterpret_cast<uintptr_t>(p_raw_base);
	record.requested_bytes = p_requested_bytes;
	record.raw_requested_bytes = p_raw_requested_bytes;
	record.allocator_usable_bytes = p_allocator_usable_bytes;
	record.created_sequence = ++r_state.sequence;
	record.last_resize_sequence = record.created_sequence;
	record.alignment = p_alignment;
	record.origin = p_origin;
	record.allocator_usable_bytes_available = p_allocator_usable_bytes_available;
	record.aligned = p_aligned;
	record.prepad = p_prepad;
	record.zeroed = p_zeroed;
	return record;
}

static bool _insert_new_record(TrackerState &r_state, const AllocationRecord &p_record) {
	if (!_ensure_insert_capacity(r_state)) {
		r_state.statistics.dropped_record_count++;
		r_state.statistics.instrumentation_failure_count++;
		return false;
	}
	const size_t existing = _find_index(r_state, p_record.user_base);
	if (existing != SIZE_MAX) {
		r_state.statistics.duplicate_active_address_count++;
		r_state.statistics.dropped_record_count++;
		r_state.statistics.instrumentation_failure_count++;
		_subtract_live_totals(r_state, r_state.slots[existing].record);
		r_state.slots[existing].record = p_record;
		_add_live_totals(r_state, p_record);
		return true;
	}
	if (!_insert_without_growth(r_state, p_record)) {
		r_state.statistics.dropped_record_count++;
		r_state.statistics.instrumentation_failure_count++;
		return false;
	}
	_add_live_totals(r_state, p_record);
	_update_tracker_storage_statistics(r_state);
	return true;
}

static bool _table_self_check(SelfCheckResult &r_result) {
	TrackerState state;
	if (!_rehash(state, 8)) {
		return false;
	}
	AllocationRecord initial = _make_record(state, reinterpret_cast<void *>(uintptr_t(0x1000)), reinterpret_cast<void *>(uintptr_t(0x1000)), 16, 16, false, false, false, 0, 16, true, AllocationOrigin::ALLOCATION);
	r_result.allocation_inserted = _insert_new_record(state, initial) && _find_index(state, initial.user_base) != SIZE_MAX;
	if (!r_result.allocation_inserted) {
		free(state.slots);
		return false;
	}

	const uint64_t original_id = initial.allocation_id;
	size_t index = _find_index(state, initial.user_base);
	AllocationRecord resized = state.slots[index].record;
	_subtract_live_totals(state, resized);
	_erase_index(state, index);
	resized.user_base = 0x2000;
	resized.raw_base = 0x2000;
	resized.requested_bytes = 32;
	resized.raw_requested_bytes = 32;
	resized.allocator_usable_bytes = 32;
	resized.last_resize_sequence = ++state.sequence;
	const bool resize_inserted = _insert_without_growth(state, resized);
	if (resize_inserted) {
		_add_live_totals(state, resized);
	}
	index = _find_index(state, resized.user_base);
	r_result.reallocation_preserved_id = resize_inserted && index != SIZE_MAX && state.slots[index].record.allocation_id == original_id;
	if (!r_result.reallocation_preserved_id) {
		free(state.slots);
		return false;
	}

	const AllocationRecord before_failure = state.slots[index].record;
	state.statistics.failed_reallocation_count++;
	const AllocationRecord after_failure = state.slots[index].record;
	r_result.failed_reallocation_preserved_record = before_failure.allocation_id == after_failure.allocation_id && before_failure.user_base == after_failure.user_base && before_failure.raw_base == after_failure.raw_base && before_failure.requested_bytes == after_failure.requested_bytes && before_failure.raw_requested_bytes == after_failure.raw_requested_bytes && before_failure.allocator_usable_bytes == after_failure.allocator_usable_bytes && before_failure.created_sequence == after_failure.created_sequence && before_failure.last_resize_sequence == after_failure.last_resize_sequence;

	_subtract_live_totals(state, state.slots[index].record);
	_erase_index(state, index);
	r_result.free_removed_record = _find_index(state, resized.user_base) == SIZE_MAX;

	AllocationRecord reused = _make_record(state, reinterpret_cast<void *>(uintptr_t(0x2000)), reinterpret_cast<void *>(uintptr_t(0x2000)), 8, 8, false, false, false, 0, 8, true, AllocationOrigin::ALLOCATION);
	const bool reuse_inserted = _insert_new_record(state, reused);
	r_result.address_reuse_changed_id = reuse_inserted && reused.allocation_id != original_id;
	r_result.passed = r_result.allocation_inserted && r_result.reallocation_preserved_id && r_result.failed_reallocation_preserved_record && r_result.free_removed_record && r_result.address_reuse_changed_id;
	free(state.slots);
	return r_result.passed;
}

} // namespace

bool enable() {
	bool expected = false;
	if (!enable_requested.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
		return is_enabled();
	}
	ScopedSuppression suppression;
	void *state_memory = malloc(sizeof(TrackerState));
	if (state_memory == nullptr) {
		enable_failed.store(true, std::memory_order_release);
		return false;
	}
	TrackerState *state = new (state_memory) TrackerState;
	if (!_rehash(*state, INITIAL_SLOT_COUNT)) {
		state->~TrackerState();
		free(state_memory);
		enable_failed.store(true, std::memory_order_release);
		return false;
	}
	state->statistics.enable_requested = true;
	state->statistics.enabled = true;
	state->statistics.tracker_start_unix_usec = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
#ifdef _MSC_VER
	state->statistics.allocator_usable_bytes_supported = true;
#endif
	tracker_state = state;
	enabled.store(true, std::memory_order_release);
	return true;
}

void record_allocation(void *p_user_base, void *p_raw_base, size_t p_requested_bytes, size_t p_raw_requested_bytes, bool p_aligned, bool p_prepad, bool p_zeroed, size_t p_alignment, size_t p_allocator_usable_bytes, bool p_allocator_usable_bytes_available) {
	if (tracker_suppressed || p_user_base == nullptr) {
		return;
	}
	ScopedSuppression suppression;
	TrackerState &state = *tracker_state;
	std::lock_guard<std::mutex> lock(state.mutex);
	state.statistics.allocation_count++;
	AllocationRecord record = _make_record(state, p_user_base, p_raw_base, p_requested_bytes, p_raw_requested_bytes, p_aligned, p_prepad, p_zeroed, p_alignment, p_allocator_usable_bytes, p_allocator_usable_bytes_available, AllocationOrigin::ALLOCATION);
	_insert_new_record(state, record);
}

static void _finish_transaction(ReallocationTransaction &r_transaction) {
	TrackerState &state = *tracker_state;
	state.mutex.unlock();
	tracker_suppressed = r_transaction.previous_suppression;
	r_transaction = ReallocationTransaction{};
}

void begin_reallocation(void *p_old_user_base, ReallocationTransaction &r_transaction) {
	r_transaction = ReallocationTransaction{};
	if (!is_enabled() || tracker_suppressed || p_old_user_base == nullptr) {
		return;
	}
	r_transaction.previous_suppression = tracker_suppressed;
	tracker_suppressed = true;
	TrackerState &state = *tracker_state;
	state.mutex.lock();
	r_transaction.old_user_base = reinterpret_cast<uintptr_t>(p_old_user_base);
	r_transaction.active = true;
}

void finish_reallocation(ReallocationTransaction &r_transaction, void *p_new_user_base, void *p_new_raw_base, size_t p_requested_bytes, size_t p_raw_requested_bytes, bool p_prepad, size_t p_allocator_usable_bytes, bool p_allocator_usable_bytes_available) {
	if (!r_transaction.active) {
		return;
	}
	TrackerState &state = *tracker_state;
	state.statistics.reallocation_count++;
	const size_t old_index = _find_index(state, r_transaction.old_user_base);
	if (old_index == SIZE_MAX) {
		state.statistics.adopted_reallocation_count++;
		AllocationRecord adopted = _make_record(state, p_new_user_base, p_new_raw_base, p_requested_bytes, p_raw_requested_bytes, false, p_prepad, false, 0, p_allocator_usable_bytes, p_allocator_usable_bytes_available, AllocationOrigin::REALLOCATION_OF_PRE_ENABLE_ALLOCATION);
		_insert_new_record(state, adopted);
		_finish_transaction(r_transaction);
		return;
	}

	AllocationRecord resized = state.slots[old_index].record;
	_subtract_live_totals(state, resized);
	if (reinterpret_cast<uintptr_t>(p_new_user_base) != resized.user_base) {
		_erase_index(state, old_index);
	}
	resized.user_base = reinterpret_cast<uintptr_t>(p_new_user_base);
	resized.raw_base = reinterpret_cast<uintptr_t>(p_new_raw_base);
	resized.requested_bytes = p_requested_bytes;
	resized.raw_requested_bytes = p_raw_requested_bytes;
	resized.allocator_usable_bytes = p_allocator_usable_bytes;
	resized.allocator_usable_bytes_available = p_allocator_usable_bytes_available;
	resized.prepad = p_prepad;
	resized.last_resize_sequence = ++state.sequence;
	if (reinterpret_cast<uintptr_t>(p_new_user_base) == state.slots[old_index].record.user_base) {
		state.slots[old_index].record = resized;
	} else if (!_insert_without_growth(state, resized)) {
		state.statistics.dropped_record_count++;
		state.statistics.instrumentation_failure_count++;
		_finish_transaction(r_transaction);
		return;
	}
	_add_live_totals(state, resized);
	_update_tracker_storage_statistics(state);
	_finish_transaction(r_transaction);
}

void finish_reallocation_failure(ReallocationTransaction &r_transaction) {
	if (!r_transaction.active) {
		return;
	}
	TrackerState &state = *tracker_state;
	state.statistics.failed_reallocation_count++;
	state.sequence++;
	if (_find_index(state, r_transaction.old_user_base) == SIZE_MAX) {
		// This can be a pre-enable allocation; keep it distinct from an actual
		// free whose allocation record was unavailable.
		state.statistics.failed_reallocation_without_record_count++;
	}
	_finish_transaction(r_transaction);
}

void finish_reallocation_free(ReallocationTransaction &r_transaction) {
	if (!r_transaction.active) {
		return;
	}
	TrackerState &state = *tracker_state;
	state.statistics.free_count++;
	state.sequence++;
	const size_t index = _find_index(state, r_transaction.old_user_base);
	if (index == SIZE_MAX) {
		state.statistics.free_without_record_count++;
	} else {
		_subtract_live_totals(state, state.slots[index].record);
		_erase_index(state, index);
	}
	_finish_transaction(r_transaction);
}

void record_free(void *p_user_base) {
	if (tracker_suppressed) {
		return;
	}
	ScopedSuppression suppression;
	TrackerState &state = *tracker_state;
	std::lock_guard<std::mutex> lock(state.mutex);
	state.statistics.free_count++;
	state.sequence++;
	const size_t index = _find_index(state, reinterpret_cast<uintptr_t>(p_user_base));
	if (index == SIZE_MAX) {
		state.statistics.free_without_record_count++;
		return;
	}
	_subtract_live_totals(state, state.slots[index].record);
	_erase_index(state, index);
}

bool query_allocation(const void *p_user_base, AllocationRecord &r_record) {
	if (!is_enabled() || tracker_suppressed || p_user_base == nullptr) {
		return false;
	}
	ScopedSuppression suppression;
	TrackerState &state = *tracker_state;
	std::lock_guard<std::mutex> lock(state.mutex);
	const size_t index = _find_index(state, reinterpret_cast<uintptr_t>(p_user_base));
	if (index == SIZE_MAX) {
		return false;
	}
	r_record = state.slots[index].record;
	return true;
}

uint64_t get_tracker_start_unix_usec() {
	if (!is_enabled()) {
		return 0;
	}
	return tracker_state->statistics.tracker_start_unix_usec;
}

bool get_statistics(Statistics &r_statistics) {
	r_statistics = Statistics{};
	if (!is_enabled()) {
		r_statistics.enable_requested = enable_requested.load(std::memory_order_acquire);
		r_statistics.enable_failed = enable_failed.load(std::memory_order_acquire);
		r_statistics.inventory_complete = false;
		if (r_statistics.enable_failed) {
			r_statistics.instrumentation_failure_count = 1;
		}
		return true;
	}
	ScopedSuppression suppression;
	TrackerState &state = *tracker_state;
	std::lock_guard<std::mutex> lock(state.mutex);
	r_statistics = state.statistics;
	r_statistics.snapshot_sequence = state.sequence;
	r_statistics.inventory_complete = state.statistics.instrumentation_failure_count == 0;
	return true;
}

bool capture_snapshot(Snapshot &r_snapshot) {
	r_snapshot = Snapshot{};
	if (!is_enabled()) {
		r_snapshot.statistics.enable_requested = enable_requested.load(std::memory_order_acquire);
		r_snapshot.statistics.enable_failed = enable_failed.load(std::memory_order_acquire);
		r_snapshot.statistics.enabled = false;
		r_snapshot.statistics.inventory_complete = false;
		if (r_snapshot.statistics.enable_failed) {
			r_snapshot.statistics.instrumentation_failure_count = 1;
		}
		return true;
	}

	ScopedSuppression suppression;
	TrackerState &state = *tracker_state;
	std::lock_guard<std::mutex> lock(state.mutex);
	AllocationRecord *records = nullptr;
	uint64_t copy_bytes = 0;
	if (state.active_count > 0) {
		if (state.active_count > SIZE_MAX / sizeof(AllocationRecord) || state.active_count > UINT64_MAX / sizeof(AllocationRecord)) {
			state.statistics.snapshot_failure_count++;
			state.statistics.instrumentation_failure_count++;
			r_snapshot.statistics = state.statistics;
			r_snapshot.statistics.inventory_complete = false;
			return false;
		}
		copy_bytes = static_cast<uint64_t>(state.active_count * sizeof(AllocationRecord));
		records = static_cast<AllocationRecord *>(malloc(static_cast<size_t>(copy_bytes)));
		if (records == nullptr) {
			state.statistics.snapshot_failure_count++;
			state.statistics.instrumentation_failure_count++;
			r_snapshot.statistics = state.statistics;
			r_snapshot.statistics.inventory_complete = false;
			return false;
		}
	}

	size_t write_index = 0;
	for (size_t i = 0; i < state.slot_count; i++) {
		if (state.slots[i].state == SLOT_OCCUPIED) {
			records[write_index++] = state.slots[i].record;
		}
	}
	r_snapshot.records = records;
	r_snapshot.record_count = write_index;
	r_snapshot.statistics = state.statistics;
	r_snapshot.statistics.snapshot_sequence = state.sequence;
	r_snapshot.statistics.snapshot_copy_bytes = copy_bytes;
	r_snapshot.statistics.inventory_complete = state.statistics.instrumentation_failure_count == 0;
	return true;
}

void release_snapshot(Snapshot &r_snapshot) {
	free(r_snapshot.records);
	r_snapshot = Snapshot{};
}

SelfCheckResult run_self_check() {
	ScopedSuppression suppression;
	SelfCheckResult result;
	_table_self_check(result);
	return result;
}

} // namespace MemoryTracker
