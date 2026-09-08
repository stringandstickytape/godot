#pragma once

#include "core/os/memory_tracker.h"
#include "core/templates/local_vector.h"
#include "core/templates/vector.h"
#include "core/variant/dictionary.h"

namespace RendererRD::MemoryLinks {

inline String address(const void *p_pointer) {
	return "0x" + String::num_uint64(uint64_t(uintptr_t(p_pointer)), 16);
}

// Query while the renderer owner is stable. A later standalone snapshot may no
// longer contain this generation; consumers must never rejoin by pointer alone.
inline Dictionary backing(const void *p_user_base, const void *p_data, uint64_t p_logical_bytes, uint64_t p_capacity_bytes, bool p_shared, uint64_t p_refcount = 0) {
	MemoryTracker::AllocationRecord allocation;
	bool known = p_user_base && MemoryTracker::query_allocation(p_user_base, allocation);
	Dictionary result;
	result["tracking_enabled"] = MemoryTracker::is_enabled();
	result["tracker_start_unix_usec"] = MemoryTracker::get_tracker_start_unix_usec();
	result["has_backing"] = p_user_base != nullptr;
	result["allocation_available"] = known;
	result["user_base"] = address(p_user_base);
	result["data_address"] = address(p_data);
	result["logical_bytes"] = p_logical_bytes;
	result["container_capacity_bytes"] = p_capacity_bytes;
	result["container_slack_bytes"] = p_capacity_bytes - p_logical_bytes;
	result["shared_cow"] = p_shared;
	if (p_shared) {
		result["cow_refcount_observed"] = p_refcount;
	}
	if (known) {
		result["allocation_id"] = "memory-allocation:" + String::num_uint64(allocation.allocation_id);
		result["identity_scope"] = "process-memory-allocation-generation";
		result["requested_bytes"] = allocation.requested_bytes;
		result["raw_requested_bytes"] = allocation.raw_requested_bytes;
		result["raw_base"] = address((const void *)allocation.raw_base);
		result["allocator_usable_bytes_available"] = allocation.allocator_usable_bytes_available;
		result["allocator_usable_bytes"] = allocation.allocator_usable_bytes;
		result["created_sequence"] = allocation.created_sequence;
		result["last_resize_sequence"] = allocation.last_resize_sequence;
	} else {
		result["unavailable_reason"] = p_user_base ? "Allocation predates tracking, is excluded from tracking, or tracking is disabled/incomplete; no identity inferred." : "Container has no backing allocation.";
	}
	return result;
}

template <typename T>
Dictionary cow(const Vector<T> &p_vector) {
	return backing(p_vector.get_memory_allocation_base(), p_vector.ptr(), uint64_t(p_vector.size()) * sizeof(T), p_vector.capacity() * sizeof(T), true, p_vector.get_memory_refcount());
}

template <typename T, typename U, bool F, bool Tight>
Dictionary local(const LocalVector<T, U, F, Tight> &p_vector) {
	return backing(p_vector.ptr(), p_vector.ptr(), uint64_t(p_vector.size()) * sizeof(T), uint64_t(p_vector.get_capacity()) * sizeof(T), false);
}

} // namespace RendererRD::MemoryLinks
