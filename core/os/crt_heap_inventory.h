/**************************************************************************/
/*  crt_heap_inventory.h                                                  */
/**************************************************************************/

#ifndef CRT_HEAP_INVENTORY_H
#define CRT_HEAP_INVENTORY_H

#include "core/string/ustring.h"

#include <cstdint>

struct CrtHeapInventoryResult {
	bool capture_succeeded = false;
	bool write_succeeded = false;
	bool enumeration_complete = false;
	bool heap_lock_succeeded = false;
	bool heap_unlock_succeeded = false;
	bool terminal_no_more_items = false;
	bool limit_reached = false;
	uint64_t process_id = 0;
	uint64_t heap_handle = 0;
	uint64_t entry_count = 0;
	uint64_t output_bytes = 0;
	uint64_t observer_buffer_requested_bytes = 0;
	uint64_t observer_buffer_capacity_entries = 0;
	uint64_t observer_record_size_bytes = 0;
	uint64_t configured_max_buffer_bytes = 0;
	uint64_t phase_count = 0;
	uint64_t capture_started_unix_usec = 0;
	uint64_t capture_finished_unix_usec = 0;
	uint64_t capture_elapsed_usec = 0;
	uint64_t selected_phase_started_unix_usec = 0;
	uint64_t selected_phase_finished_unix_usec = 0;
	uint32_t terminal_win32_error = 0;
	uint32_t buffer_allocation_win32_error = 0;
	uint32_t lock_win32_error = 0;
	uint32_t unlock_win32_error = 0;
	String output_path;
	String error;
};

class CrtHeapInventory {
public:
	static CrtHeapInventoryResult write_jsonl(const String &p_absolute_path);
};

#endif // CRT_HEAP_INVENTORY_H
