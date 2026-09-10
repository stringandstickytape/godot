/**************************************************************************/
/*  crt_heap_inventory.cpp                                                */
/**************************************************************************/

#include "crt_heap_inventory.h"

#if defined(WINDOWS_ENABLED) && defined(_MSC_VER)

#include "platform/windows/windows.h"

#include <cstdio>
#include <cwchar>
#include <malloc.h>

namespace {

constexpr uint64_t DEFAULT_INITIAL_BUFFER_BYTES = 128ULL * 1024ULL * 1024ULL;
constexpr uint64_t DEFAULT_MAX_BUFFER_BYTES = 512ULL * 1024ULL * 1024ULL;
constexpr const wchar_t *MAX_BUFFER_ENVIRONMENT = L"ANTHROPOLIS_CRT_HEAP_INVENTORY_MAX_BYTES";
constexpr const char *FORMAT = "anthropolis-crt-heap-inventory-jsonl-v1";

struct CrtHeapEntryPod {
	uint64_t data = 0;
	uint64_t data_size = 0;
	uint64_t overhead = 0;
	uint64_t region_index = 0;
	uint64_t flags = 0;
	uint64_t region_committed_size = 0;
	uint64_t region_uncommitted_size = 0;
	uint64_t region_first_block = 0;
	uint64_t region_last_block = 0;
};

struct CapturePhase {
	CrtHeapEntryPod *entries = nullptr;
	uint64_t capacity_entries = 0;
	uint64_t buffer_requested_bytes = 0;
	uint64_t entry_count = 0;
	uint64_t started_unix_usec = 0;
	uint64_t finished_unix_usec = 0;
	uint64_t elapsed_usec = 0;
	uint32_t terminal_error = 0;
	uint32_t buffer_allocation_error = 0;
	uint32_t lock_error = 0;
	uint32_t unlock_error = 0;
	bool lock_succeeded = false;
	bool unlock_succeeded = false;
	bool terminal_no_more_items = false;
	bool limit_reached = false;
	bool complete = false;
};

uint64_t get_unix_usec() {
	FILETIME file_time;
	GetSystemTimePreciseAsFileTime(&file_time);
	ULARGE_INTEGER value;
	value.LowPart = file_time.dwLowDateTime;
	value.HighPart = file_time.dwHighDateTime;
	constexpr uint64_t WINDOWS_TO_UNIX_100NS = 116444736000000000ULL;
	return value.QuadPart >= WINDOWS_TO_UNIX_100NS ? (value.QuadPart - WINDOWS_TO_UNIX_100NS) / 10ULL : 0;
}

uint64_t elapsed_usec(LARGE_INTEGER p_started, LARGE_INTEGER p_finished, LARGE_INTEGER p_frequency) {
	if (p_frequency.QuadPart <= 0 || p_finished.QuadPart < p_started.QuadPart) {
		return 0;
	}
	const uint64_t ticks = static_cast<uint64_t>(p_finished.QuadPart - p_started.QuadPart);
	return ticks > UINT64_MAX / 1000000ULL ? 0 : ticks * 1000000ULL / static_cast<uint64_t>(p_frequency.QuadPart);
}

uint64_t configured_max_buffer_bytes() {
	wchar_t text[64] = {};
	constexpr DWORD TEXT_CAPACITY = static_cast<DWORD>(sizeof(text) / sizeof(text[0]));
	const DWORD length = GetEnvironmentVariableW(MAX_BUFFER_ENVIRONMENT, text, TEXT_CAPACITY);
	if (length == 0 || length >= TEXT_CAPACITY) {
		return DEFAULT_MAX_BUFFER_BYTES;
	}
	wchar_t *end = nullptr;
	const unsigned long long value = _wcstoui64(text, &end, 10);
	return end != text && *end == L'\0' && value >= sizeof(CrtHeapEntryPod) ? static_cast<uint64_t>(value) : DEFAULT_MAX_BUFFER_BYTES;
}

void copy_entry(const PROCESS_HEAP_ENTRY &p_source, CrtHeapEntryPod &r_target) {
	r_target.data = reinterpret_cast<uintptr_t>(p_source.lpData);
	r_target.data_size = p_source.cbData;
	r_target.overhead = p_source.cbOverhead;
	r_target.region_index = p_source.iRegionIndex;
	r_target.flags = p_source.wFlags;
	if ((p_source.wFlags & PROCESS_HEAP_REGION) != 0) {
		r_target.region_committed_size = p_source.Region.dwCommittedSize;
		r_target.region_uncommitted_size = p_source.Region.dwUnCommittedSize;
		r_target.region_first_block = reinterpret_cast<uintptr_t>(p_source.Region.lpFirstBlock);
		r_target.region_last_block = reinterpret_cast<uintptr_t>(p_source.Region.lpLastBlock);
	}
}

void capture_phase(HANDLE p_heap, uint64_t p_capacity_entries, CapturePhase &r_phase) {
	r_phase.capacity_entries = p_capacity_entries;
	r_phase.buffer_requested_bytes = p_capacity_entries * sizeof(CrtHeapEntryPod);
	r_phase.entries = static_cast<CrtHeapEntryPod *>(VirtualAlloc(nullptr, r_phase.buffer_requested_bytes,
			MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
	if (r_phase.entries == nullptr) {
		r_phase.buffer_allocation_error = GetLastError();
		return;
	}
	r_phase.started_unix_usec = get_unix_usec();
	LARGE_INTEGER frequency;
	LARGE_INTEGER started;
	LARGE_INTEGER finished;
	QueryPerformanceFrequency(&frequency);
	QueryPerformanceCounter(&started);
	SetLastError(ERROR_SUCCESS);
	r_phase.lock_succeeded = HeapLock(p_heap) != FALSE;
	if (!r_phase.lock_succeeded) {
		r_phase.lock_error = GetLastError();
		QueryPerformanceCounter(&finished);
		r_phase.finished_unix_usec = get_unix_usec();
		r_phase.elapsed_usec = elapsed_usec(started, finished, frequency);
		return;
	}

	PROCESS_HEAP_ENTRY entry = {};
	while (r_phase.entry_count < r_phase.capacity_entries) {
		SetLastError(ERROR_SUCCESS);
		if (HeapWalk(p_heap, &entry) == FALSE) {
			r_phase.terminal_error = GetLastError();
			r_phase.terminal_no_more_items = r_phase.terminal_error == ERROR_NO_MORE_ITEMS;
			break;
		}
		copy_entry(entry, r_phase.entries[r_phase.entry_count++]);
	}
	if (r_phase.entry_count == r_phase.capacity_entries && !r_phase.terminal_no_more_items) {
		PROCESS_HEAP_ENTRY probe = entry;
		SetLastError(ERROR_SUCCESS);
		if (HeapWalk(p_heap, &probe) != FALSE) {
			r_phase.limit_reached = true;
			r_phase.terminal_error = ERROR_SUCCESS;
		} else {
			r_phase.terminal_error = GetLastError();
			r_phase.terminal_no_more_items = r_phase.terminal_error == ERROR_NO_MORE_ITEMS;
		}
	}
	SetLastError(ERROR_SUCCESS);
	r_phase.unlock_succeeded = HeapUnlock(p_heap) != FALSE;
	if (!r_phase.unlock_succeeded) {
		r_phase.unlock_error = GetLastError();
	}
	QueryPerformanceCounter(&finished);
	r_phase.finished_unix_usec = get_unix_usec();
	r_phase.elapsed_usec = elapsed_usec(started, finished, frequency);
	r_phase.complete = r_phase.lock_succeeded && r_phase.unlock_succeeded && r_phase.terminal_no_more_items && !r_phase.limit_reached;
}

bool store_line(FILE *p_file, const char *p_line, int p_length, uint64_t &r_output_bytes) {
	if (p_length < 0 || fwrite(p_line, 1, static_cast<size_t>(p_length), p_file) != static_cast<size_t>(p_length)) {
		return false;
	}
	r_output_bytes += static_cast<uint64_t>(p_length);
	return true;
}

const char *json_bool(bool p_value) {
	return p_value ? "true" : "false";
}

} // namespace

CrtHeapInventoryResult CrtHeapInventory::write_jsonl(const String &p_absolute_path) {
	CrtHeapInventoryResult result;
	result.output_path = p_absolute_path;
	result.process_id = GetCurrentProcessId();
	result.heap_handle = static_cast<uint64_t>(_get_heap_handle());
	result.observer_record_size_bytes = sizeof(CrtHeapEntryPod);
	result.configured_max_buffer_bytes = configured_max_buffer_bytes();
	result.capture_started_unix_usec = get_unix_usec();
	if (result.heap_handle == 0) {
		result.error = "The Microsoft CRT returned no heap handle.";
		return result;
	}

	const uint64_t max_entries = result.configured_max_buffer_bytes / sizeof(CrtHeapEntryPod);
	const uint64_t default_initial_entries = DEFAULT_INITIAL_BUFFER_BYTES / sizeof(CrtHeapEntryPod);
	const uint64_t initial_entries = max_entries < default_initial_entries ? max_entries : default_initial_entries;
	if (initial_entries == 0) {
		result.error = "The CRT heap inventory buffer budget cannot hold one POD entry.";
		return result;
	}
	CapturePhase phases[2];
	uint64_t phase_count = 0;
	capture_phase(reinterpret_cast<HANDLE>(result.heap_handle), initial_entries, phases[phase_count++]);
	CapturePhase *selected = &phases[0];
	if (selected->limit_reached && selected->unlock_succeeded && initial_entries < max_entries) {
		VirtualFree(selected->entries, 0, MEM_RELEASE);
		selected->entries = nullptr;
		capture_phase(reinterpret_cast<HANDLE>(result.heap_handle), max_entries, phases[phase_count++]);
		selected = &phases[1];
	}

	result.phase_count = phase_count;
	result.entry_count = selected->entry_count;
	result.observer_buffer_requested_bytes = selected->buffer_requested_bytes;
	result.observer_buffer_capacity_entries = selected->capacity_entries;
	result.enumeration_complete = selected->complete;
	result.heap_lock_succeeded = selected->lock_succeeded;
	result.heap_unlock_succeeded = selected->unlock_succeeded;
	result.terminal_no_more_items = selected->terminal_no_more_items;
	result.limit_reached = selected->limit_reached;
	result.terminal_win32_error = selected->terminal_error;
	result.buffer_allocation_win32_error = selected->buffer_allocation_error;
	result.lock_win32_error = selected->lock_error;
	result.unlock_win32_error = selected->unlock_error;
	result.selected_phase_started_unix_usec = selected->started_unix_usec;
	result.selected_phase_finished_unix_usec = selected->finished_unix_usec;
	result.capture_finished_unix_usec = get_unix_usec();
	result.capture_elapsed_usec = result.capture_finished_unix_usec >= result.capture_started_unix_usec
			? result.capture_finished_unix_usec - result.capture_started_unix_usec
			: 0;

	Char16String path_utf16 = p_absolute_path.utf16();
	FILE *file = nullptr;
	const errno_t open_error = _wfopen_s(&file, reinterpret_cast<const wchar_t *>(path_utf16.get_data()), L"wbx");
	if (open_error != 0 || file == nullptr) {
		if (selected->entries != nullptr) {
			VirtualFree(selected->entries, 0, MEM_RELEASE);
		}
		result.error = "Could not exclusively create the CRT heap inventory file.";
		return result;
	}

	constexpr int LINE_CAPACITY = 8192;
	char line[LINE_CAPACITY];
	int length = snprintf(line, LINE_CAPACITY,
			"{\"record_type\":\"metadata\",\"schema_version\":1,\"format\":\"%s\",\"process_id\":%llu,\"crt_heap_handle\":\"0x%llx\",\"enumeration_complete\":%s,\"entry_count\":%llu,\"phase_count\":%llu,\"selected_phase\":%llu,\"initial_phase_capacity_entries\":%llu,\"initial_phase_entry_count\":%llu,\"initial_phase_elapsed_usec\":%llu,\"initial_phase_limit_reached\":%s,\"retry_performed\":%s,\"observer_buffer_allocation\":\"VirtualAlloc MEM_RESERVE|MEM_COMMIT outside CRT heap lock\",\"observer_record_size_bytes\":%llu,\"observer_buffer_requested_bytes\":%llu,\"observer_buffer_capacity_entries\":%llu,\"configured_max_buffer_bytes\":%llu,\"observer_buffer_lifetime\":\"Allocated before HeapLock, held through post-unlock serialization, released before method return\",\"capture_started_unix_usec\":%llu,\"capture_finished_unix_usec\":%llu,\"capture_elapsed_usec\":%llu,\"selected_phase_started_unix_usec\":%llu,\"selected_phase_finished_unix_usec\":%llu,\"selected_phase_elapsed_usec\":%llu,\"buffer_allocation_win32_error\":%lu,\"heap_lock_succeeded\":%s,\"heap_unlock_succeeded\":%s,\"lock_win32_error\":%lu,\"unlock_win32_error\":%lu,\"terminal_win32_error\":%lu,\"terminal_error_no_more_items\":%s,\"limit_reached\":%s,\"method\":\"_get_heap_handle; fresh HeapWalk from zeroed PROCESS_HEAP_ENTRY under HeapLock; POD copy only while locked\",\"phase_note\":\"A buffer retry is a discarded fresh capture phase, never a continuation. Timing brackets include lock acquisition and unlock calls; exact lock-held time is not observed.\",\"coverage_note\":\"Only the Microsoft CRT heap returned by _get_heap_handle is enumerated; secondary process heaps are outside scope.\",\"accounting_note\":\"Data ranges and scalar cbOverhead are raw PROCESS_HEAP_ENTRY evidence. Overhead has no inferred address. Busy/free/control classification implies neither residency nor reclaimability.\"}\n",
			FORMAT, static_cast<unsigned long long>(result.process_id), static_cast<unsigned long long>(result.heap_handle), json_bool(result.enumeration_complete), static_cast<unsigned long long>(result.entry_count), static_cast<unsigned long long>(phase_count), static_cast<unsigned long long>(phase_count), static_cast<unsigned long long>(phases[0].capacity_entries), static_cast<unsigned long long>(phases[0].entry_count), static_cast<unsigned long long>(phases[0].elapsed_usec), json_bool(phases[0].limit_reached), json_bool(phase_count > 1), static_cast<unsigned long long>(result.observer_record_size_bytes), static_cast<unsigned long long>(result.observer_buffer_requested_bytes), static_cast<unsigned long long>(result.observer_buffer_capacity_entries), static_cast<unsigned long long>(result.configured_max_buffer_bytes), static_cast<unsigned long long>(result.capture_started_unix_usec), static_cast<unsigned long long>(result.capture_finished_unix_usec), static_cast<unsigned long long>(result.capture_elapsed_usec), static_cast<unsigned long long>(result.selected_phase_started_unix_usec), static_cast<unsigned long long>(result.selected_phase_finished_unix_usec), static_cast<unsigned long long>(selected->elapsed_usec), static_cast<unsigned long>(result.buffer_allocation_win32_error), json_bool(result.heap_lock_succeeded), json_bool(result.heap_unlock_succeeded), static_cast<unsigned long>(result.lock_win32_error), static_cast<unsigned long>(result.unlock_win32_error), static_cast<unsigned long>(result.terminal_win32_error), json_bool(result.terminal_no_more_items), json_bool(result.limit_reached));
	bool stream_ok = length >= 0 && length < LINE_CAPACITY && store_line(file, line, length, result.output_bytes);
	uint64_t records_written = 0;
	for (uint64_t index = 0; index < selected->entry_count && stream_ok; index++) {
		const CrtHeapEntryPod &entry = selected->entries[index];
		const bool interval_valid = entry.data <= UINT64_MAX - entry.data_size;
		const uint64_t interval_end = interval_valid ? entry.data + entry.data_size : 0;
		const bool region = (entry.flags & PROCESS_HEAP_REGION) != 0;
		const uint64_t known_flags = PROCESS_HEAP_REGION | PROCESS_HEAP_UNCOMMITTED_RANGE | PROCESS_HEAP_ENTRY_BUSY | PROCESS_HEAP_SEG_ALLOC | PROCESS_HEAP_ENTRY_MOVEABLE | PROCESS_HEAP_ENTRY_DDESHARE;
		const char *classification = region ? "region" : (entry.flags & PROCESS_HEAP_UNCOMMITTED_RANGE) != 0 ? "uncommitted-range"
				: (entry.flags & PROCESS_HEAP_ENTRY_BUSY) != 0 ? "busy"
				: (entry.flags & PROCESS_HEAP_SEG_ALLOC) != 0 ? "segment-allocation" : "free";
		char region_committed[32] = "null";
		char region_uncommitted[32] = "null";
		char region_first[32] = "null";
		char region_last[32] = "null";
		if (region) {
			snprintf(region_committed, sizeof(region_committed), "%llu", static_cast<unsigned long long>(entry.region_committed_size));
			snprintf(region_uncommitted, sizeof(region_uncommitted), "%llu", static_cast<unsigned long long>(entry.region_uncommitted_size));
			snprintf(region_first, sizeof(region_first), "\"0x%llx\"", static_cast<unsigned long long>(entry.region_first_block));
			snprintf(region_last, sizeof(region_last), "\"0x%llx\"", static_cast<unsigned long long>(entry.region_last_block));
		}
		length = snprintf(line, LINE_CAPACITY,
				"{\"record_type\":\"entry\",\"schema_version\":1,\"id\":%llu,\"data_start\":\"0x%llx\",\"data_size_bytes\":%llu,\"data_end_exclusive\":\"0x%llx\",\"data_interval_valid\":%s,\"overhead_bytes\":%llu,\"region_index\":%llu,\"flags_value\":%llu,\"flag_classification\":\"%s\",\"is_busy\":%s,\"is_segment_allocation\":%s,\"is_moveable\":%s,\"is_ddeshare\":%s,\"is_region\":%s,\"is_uncommitted_range\":%s,\"unknown_flags_value\":%llu,\"region_fields_valid\":%s,\"region_committed_size_bytes\":%s,\"region_uncommitted_size_bytes\":%s,\"region_first_block\":%s,\"region_last_block\":%s}\n",
				static_cast<unsigned long long>(index + 1), static_cast<unsigned long long>(entry.data), static_cast<unsigned long long>(entry.data_size), static_cast<unsigned long long>(interval_end), json_bool(interval_valid), static_cast<unsigned long long>(entry.overhead), static_cast<unsigned long long>(entry.region_index), static_cast<unsigned long long>(entry.flags), classification, json_bool((entry.flags & PROCESS_HEAP_ENTRY_BUSY) != 0), json_bool((entry.flags & PROCESS_HEAP_SEG_ALLOC) != 0), json_bool((entry.flags & PROCESS_HEAP_ENTRY_MOVEABLE) != 0), json_bool((entry.flags & PROCESS_HEAP_ENTRY_DDESHARE) != 0), json_bool(region), json_bool((entry.flags & PROCESS_HEAP_UNCOMMITTED_RANGE) != 0), static_cast<unsigned long long>(entry.flags & ~known_flags), json_bool(region), region_committed, region_uncommitted, region_first, region_last);
		stream_ok = length >= 0 && length < LINE_CAPACITY && store_line(file, line, length, result.output_bytes);
		if (stream_ok) {
			records_written++;
		}
	}
	if (stream_ok) {
		length = snprintf(line, LINE_CAPACITY,
				"{\"record_type\":\"receipt\",\"schema_version\":1,\"format\":\"%s\",\"process_id\":%llu,\"crt_heap_handle\":\"0x%llx\",\"expected_record_count\":%llu,\"records_written\":%llu,\"stream_complete\":true,\"enumeration_complete\":%s,\"heap_lock_succeeded\":%s,\"heap_unlock_succeeded\":%s,\"terminal_win32_error\":%lu,\"terminal_error_no_more_items\":%s,\"limit_reached\":%s}\n",
				FORMAT, static_cast<unsigned long long>(result.process_id), static_cast<unsigned long long>(result.heap_handle), static_cast<unsigned long long>(result.entry_count), static_cast<unsigned long long>(records_written), json_bool(result.enumeration_complete), json_bool(result.heap_lock_succeeded), json_bool(result.heap_unlock_succeeded), static_cast<unsigned long>(result.terminal_win32_error), json_bool(result.terminal_no_more_items), json_bool(result.limit_reached));
		stream_ok = length >= 0 && length < LINE_CAPACITY && records_written == result.entry_count && store_line(file, line, length, result.output_bytes);
	}
	if (stream_ok) {
		stream_ok = fflush(file) == 0;
	}
	if (fclose(file) != 0) {
		stream_ok = false;
	}
	if (selected->entries != nullptr) {
		VirtualFree(selected->entries, 0, MEM_RELEASE);
		selected->entries = nullptr;
	}
	result.write_succeeded = stream_ok;
	result.capture_succeeded = stream_ok && result.enumeration_complete;
	if (!stream_ok) {
		result.error = "CRT heap inventory serialization failed; the partial file has no guaranteed valid receipt.";
	} else if (!result.enumeration_complete) {
		result.error = result.buffer_allocation_win32_error != 0
				? "VirtualAlloc could not reserve and commit the bounded CRT heap inventory POD buffer."
				: !result.heap_lock_succeeded
				? "HeapLock failed for the CRT heap; no heap entries were enumerated."
				: !result.heap_unlock_succeeded
				? "HeapUnlock failed after CRT heap enumeration; the capture is invalid."
				: result.limit_reached
				? "CRT heap enumeration reached the configured POD buffer limit before ERROR_NO_MORE_ITEMS."
				: "CRT heap enumeration ended without the required ERROR_NO_MORE_ITEMS terminal proof.";
	}
	return result;
}

#else

CrtHeapInventoryResult CrtHeapInventory::write_jsonl(const String &p_absolute_path) {
	CrtHeapInventoryResult result;
	result.output_path = p_absolute_path;
	result.error = "CRT heap inventory is supported only on Windows MSVC builds.";
	return result;
}

#endif
