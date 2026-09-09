/**************************************************************************/
/*  memory.cpp                                                            */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "memory.h"

#include "core/os/memory_tracker.h"
#include "core/profiling/profiling.h"
#include "core/templates/safe_refcount.h"

#ifdef DEV_ENABLED
#include "core/math/math_funcs_binary.h"
#endif

#include <cstdlib>

#ifdef _MSC_VER
#include <intrin.h>
#include <malloc.h>
#endif

#if defined(_MSC_VER)
#define MEMORY_TRACKER_CALLER_PC() reinterpret_cast<uintptr_t>(_ReturnAddress())
#elif defined(__GNUC__) || defined(__clang__)
#define MEMORY_TRACKER_CALLER_PC() reinterpret_cast<uintptr_t>(__builtin_extract_return_addr(__builtin_return_address(0)))
#else
#define MEMORY_TRACKER_CALLER_PC() uintptr_t(0)
#endif

#ifdef DEBUG_ENABLED
static SafeNumeric<uint64_t> _current_mem_usage;
static SafeNumeric<uint64_t> _max_mem_usage;
#endif

static size_t _allocator_usable_size(void *p_raw_base, bool &r_available) {
#ifdef _MSC_VER
	r_available = p_raw_base != nullptr;
	return p_raw_base != nullptr ? _msize(p_raw_base) : 0;
#else
	r_available = false;
	return 0;
#endif
}

static _FORCE_INLINE_ void *_realloc_with_tracking(void *p_old_user_base, void *p_old_raw_base, size_t p_requested_bytes, size_t p_raw_requested_bytes, bool p_prepad, uintptr_t p_caller_pc) {
	MemoryTracker::ReallocationTransaction transaction;
	MemoryTracker::begin_reallocation(p_old_user_base, transaction);
	void *new_raw_base = realloc(p_old_raw_base, p_raw_requested_bytes);
	if (new_raw_base == nullptr) {
		if (p_requested_bytes == 0) {
			MemoryTracker::finish_reallocation_free(transaction);
		} else {
			MemoryTracker::finish_reallocation_failure(transaction);
		}
		return nullptr;
	}
	bool usable_available = false;
	const size_t usable_bytes = _allocator_usable_size(new_raw_base, usable_available);
	void *new_user_base = p_prepad ? static_cast<void *>(static_cast<uint8_t *>(new_raw_base) + Memory::DATA_OFFSET) : new_raw_base;
	MemoryTracker::finish_reallocation(transaction, new_user_base, new_raw_base, p_requested_bytes, p_raw_requested_bytes, p_prepad, usable_bytes, usable_available, p_caller_pc);
	return new_raw_base;
}

#define MEMORY_REALLOC_WITH_OPTIONAL_TRACKING(m_old_user_base, m_old_raw_base, m_requested_bytes, m_raw_requested_bytes, m_prepad) \
	(likely(!MemoryTracker::is_enabled()) ? realloc(m_old_raw_base, m_raw_requested_bytes) : _realloc_with_tracking(m_old_user_base, m_old_raw_base, m_requested_bytes, m_raw_requested_bytes, m_prepad, MemoryTracker::is_caller_pc_enabled() ? MEMORY_TRACKER_CALLER_PC() : 0))

void *operator new(size_t p_size, DefaultAllocator p_allocator) {
	return Memory::alloc_static(p_size);
}

void *operator new(size_t p_size, void *(*p_allocfunc)(size_t p_size)) {
	return p_allocfunc(p_size);
}

void *Memory::alloc_aligned_static(size_t p_bytes, size_t p_alignment) {
	DEV_ASSERT(Math::is_power_of_2(p_alignment));

	void *p1, *p2;
	if ((p1 = (void *)malloc(p_bytes + p_alignment - 1 + sizeof(uint32_t))) == nullptr) {
		return nullptr;
	}
	GodotProfileAlloc(p1, p_bytes + p_alignment - 1 + sizeof(uint32_t));

	p2 = (void *)(((uintptr_t)p1 + sizeof(uint32_t) + p_alignment - 1) & ~((p_alignment)-1));
	*((uint32_t *)p2 - 1) = (uint32_t)((uintptr_t)p2 - (uintptr_t)p1);
	if (unlikely(MemoryTracker::is_enabled())) {
		bool usable_available = false;
		const size_t usable_bytes = _allocator_usable_size(p1, usable_available);
		const uintptr_t caller_pc = MemoryTracker::is_caller_pc_enabled() ? MEMORY_TRACKER_CALLER_PC() : 0;
		MemoryTracker::record_allocation(p2, p1, p_bytes, p_bytes + p_alignment - 1 + sizeof(uint32_t), true, false, false, p_alignment, usable_bytes, usable_available, caller_pc);
	}
	return p2;
}

void *Memory::realloc_aligned_static(void *p_memory, size_t p_bytes, size_t p_prev_bytes, size_t p_alignment) {
	if (p_memory == nullptr) {
		return alloc_aligned_static(p_bytes, p_alignment);
	}

	void *ret = alloc_aligned_static(p_bytes, p_alignment);
	if (ret) {
		memcpy(ret, p_memory, p_prev_bytes);
	}
	free_aligned_static(p_memory);
	return ret;
}

void Memory::free_aligned_static(void *p_memory) {
	uint32_t offset = *((uint32_t *)p_memory - 1);
	void *p = (void *)((uint8_t *)p_memory - offset);
	if (unlikely(MemoryTracker::is_enabled())) {
		MemoryTracker::record_free(p_memory);
	}
	GodotProfileFree(p);
	free(p);
}

template <bool p_ensure_zero>
void *Memory::alloc_static(size_t p_bytes, bool p_pad_align) {
#ifdef DEBUG_ENABLED
	bool prepad = true;
#else
	bool prepad = p_pad_align;
#endif

	void *mem;
	if constexpr (p_ensure_zero) {
		mem = calloc(1, p_bytes + (prepad ? DATA_OFFSET : 0));
	} else {
		mem = malloc(p_bytes + (prepad ? DATA_OFFSET : 0));
	}

	ERR_FAIL_NULL_V(mem, nullptr);
	GodotProfileAlloc(mem, p_bytes + (prepad ? DATA_OFFSET : 0));

	if (prepad) {
		uint8_t *s8 = (uint8_t *)mem;

		uint64_t *s = (uint64_t *)(s8 + SIZE_OFFSET);
		*s = p_bytes;

#ifdef DEBUG_ENABLED
		uint64_t new_mem_usage = _current_mem_usage.add(p_bytes);
		_max_mem_usage.exchange_if_greater(new_mem_usage);
#endif
		void *user_mem = s8 + DATA_OFFSET;
		if (unlikely(MemoryTracker::is_enabled())) {
			bool usable_available = false;
			const size_t usable_bytes = _allocator_usable_size(mem, usable_available);
			const uintptr_t caller_pc = MemoryTracker::is_caller_pc_enabled() ? MEMORY_TRACKER_CALLER_PC() : 0;
			MemoryTracker::record_allocation(user_mem, mem, p_bytes, p_bytes + DATA_OFFSET, false, true, p_ensure_zero, 0, usable_bytes, usable_available, caller_pc);
		}
		return user_mem;
	} else {
		if (unlikely(MemoryTracker::is_enabled())) {
			bool usable_available = false;
			const size_t usable_bytes = _allocator_usable_size(mem, usable_available);
			const uintptr_t caller_pc = MemoryTracker::is_caller_pc_enabled() ? MEMORY_TRACKER_CALLER_PC() : 0;
			MemoryTracker::record_allocation(mem, mem, p_bytes, p_bytes, false, false, p_ensure_zero, 0, usable_bytes, usable_available, caller_pc);
		}
		return mem;
	}
}

template void *Memory::alloc_static<true>(size_t p_bytes, bool p_pad_align);
template void *Memory::alloc_static<false>(size_t p_bytes, bool p_pad_align);

void *Memory::realloc_static(void *p_memory, size_t p_bytes, bool p_pad_align) {
	if (p_memory == nullptr) {
		return alloc_static(p_bytes, p_pad_align);
	}

	uint8_t *mem = (uint8_t *)p_memory;
#ifdef DEBUG_ENABLED
	bool prepad = true;
#else
	bool prepad = p_pad_align;
#endif

	if (prepad) {
		mem -= DATA_OFFSET;
		uint8_t *old_user_mem = mem + DATA_OFFSET;
		uint64_t *s = (uint64_t *)(mem + SIZE_OFFSET);

#ifdef DEBUG_ENABLED
		if (p_bytes > *s) {
			uint64_t new_mem_usage = _current_mem_usage.add(p_bytes - *s);
			_max_mem_usage.exchange_if_greater(new_mem_usage);
		} else {
			_current_mem_usage.sub(*s - p_bytes);
		}
#endif

		if (p_bytes == 0) {
			if (unlikely(MemoryTracker::is_enabled())) {
				MemoryTracker::record_free(old_user_mem);
			}
			GodotProfileFree(mem);
			free(mem);
			return nullptr;
		} else {
			*s = p_bytes;

			GodotProfileFree(mem);
			uint8_t *new_mem = (uint8_t *)MEMORY_REALLOC_WITH_OPTIONAL_TRACKING(old_user_mem, mem, p_bytes, p_bytes + DATA_OFFSET, true);
			if (new_mem == nullptr) {
				ERR_FAIL_NULL_V(new_mem, nullptr);
			}
			mem = new_mem;
			s = (uint64_t *)(mem + SIZE_OFFSET);
			*s = p_bytes;

			void *new_user_mem = mem + DATA_OFFSET;
			GodotProfileAlloc(mem, p_bytes + DATA_OFFSET);
			return new_user_mem;
		}
	} else {
		void *old_user_mem = mem;
		GodotProfileFree(mem);
		uint8_t *new_mem = (uint8_t *)MEMORY_REALLOC_WITH_OPTIONAL_TRACKING(old_user_mem, mem, p_bytes, p_bytes, false);

		if (new_mem == nullptr && p_bytes > 0) {
			ERR_FAIL_COND_V(new_mem == nullptr, nullptr);
		}
		mem = new_mem;
		GodotProfileAlloc(mem, p_bytes);

		return mem;
	}
}

void Memory::free_static(void *p_ptr, bool p_pad_align) {
	ERR_FAIL_NULL(p_ptr);

	uint8_t *mem = (uint8_t *)p_ptr;

#ifdef DEBUG_ENABLED
	bool prepad = true;
#else
	bool prepad = p_pad_align;
#endif

	if (prepad) {
		mem -= DATA_OFFSET;

#ifdef DEBUG_ENABLED
		uint64_t *s = (uint64_t *)(mem + SIZE_OFFSET);
		_current_mem_usage.sub(*s);
#endif

		if (unlikely(MemoryTracker::is_enabled())) {
			MemoryTracker::record_free(p_ptr);
		}
		GodotProfileFree(mem);
		free(mem);
	} else {
		if (unlikely(MemoryTracker::is_enabled())) {
			MemoryTracker::record_free(p_ptr);
		}
		GodotProfileFree(mem);
		free(mem);
	}
}

uint64_t Memory::get_mem_available() {
	return -1; // 0xFFFF...
}

uint64_t Memory::get_mem_usage() {
#ifdef DEBUG_ENABLED
	return _current_mem_usage.get();
#else
	return 0;
#endif
}

uint64_t Memory::get_mem_max_usage() {
#ifdef DEBUG_ENABLED
	return _max_mem_usage.get();
#else
	return 0;
#endif
}

_GlobalNil::_GlobalNil() {
	left = this;
	right = this;
	parent = this;
}

_GlobalNil _GlobalNilClass::_nil;
