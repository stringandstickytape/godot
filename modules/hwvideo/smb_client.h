/**************************************************************************/
/*  smb_client.h                                                          */
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

#pragma once

#include "core/os/condition_variable.h"
#include "core/os/mutex.h"
#include "core/os/thread.h"
#include "core/string/ustring.h"
#include "core/templates/safe_refcount.h"
#include "core/variant/dictionary.h"

struct smb2_context;
struct smb2fh;

// SMB2/3 client pieces on libsmb2 (static, build/libsmb2-build.sh): the credentials registry, smb:// URLs, a session to
// one share with an event loop that honours a deadline and an abort, and SmbFile, a file read through a readahead ring on
// its own thread for FFmpeg's AVIOContext.

// Credentials by host (case-insensitive), shared by MediaSource and MediaBrowser. Never logged.
class SmbCredentials {
public:
	struct Entry {
		String user;
		String password;
		String domain;
	};
	static void set(const String &p_host, const String &p_user, const String &p_password, const String &p_domain);
	static bool get(const String &p_host, Entry &r_entry);
	static void clear(); // At module shutdown.
};

// smb://host/share/dir/file.mkv. Paths are taken as they are (no percent-decoding): names with spaces, '#' or '%' work.
struct SmbUrl {
	String host;
	String share;
	String path; // Inside the share, '/'-separated, no leading '/'; empty for the share's root.
	bool valid = false;

	static bool is_smb(const String &p_path);
	static SmbUrl parse(const String &p_url);
};

// One connection (TCP, negotiate, NTLMSSP session, tree connect) to one share. Used by one thread at a time.
class SmbSession {
public:
	enum Wait {
		WAIT_DONE,
		WAIT_TIMEOUT,
		WAIT_ABORTED,
		WAIT_ERROR, // The connection is unusable; close() it.
	};

	// What an async libsmb2 call reports to its callback.
	struct Result {
		bool done = false;
		int status = 0;
		void *data = nullptr;
	};
	static void result_callback(struct smb2_context *p_smb2, int p_status, void *p_command_data, void *p_private);

	~SmbSession();

	// Within p_timeout seconds; p_abort and p_wake_fd (optional) end the wait early.
	bool connect(const String &p_host, const String &p_share, double p_timeout, String &r_error, const SafeFlag *p_abort = nullptr, int p_wake_fd = -1);
	void close();
	bool is_connected() const { return context != nullptr; }
	smb2_context *get_context() const { return context; }
	const String &get_host() const { return host; }
	const String &get_share() const { return share; }
	String get_description() const; // "SMB 3.1.1, signed (AES-CMAC: ARMv8 Crypto Extensions)"

	// Runs the event loop until p_result->done, the deadline (OS ticks, usec), p_abort, or a fatal error.
	Wait run(const Result *p_result, uint64_t p_deadline_usec, const SafeFlag *p_abort = nullptr, int p_wake_fd = -1);
	// One poll round of at most p_max_wait_ms; returns early on p_wake_fd (drained). False on a fatal error.
	bool poll_once(int p_wake_fd, int p_max_wait_ms);
	// Clears libsmb2's last error, so describe_error() doesn't report an older NT status.
	void clear_error();
	// A readable message for a failed call; p_what names the object ("Movies/film.mkv").
	String describe_error(int p_status, const String &p_what, Wait p_wait = WAIT_DONE) const;

private:
	smb2_context *context = nullptr;
	String host;
	String share;
	double timeout = 8.0;
	bool guest = false; // No credentials were registered for the host.
};

// A file on a share, read ahead into a ring buffer by a thread of its own, so network latency doesn't reach the reader
// (MediaSource's demux thread, through an AVIOContext).
//
// The ring holds a contiguous range of the file: what was read and not yet passed (up to the readahead limit) plus up to
// 1/8 of the ring behind the read position, for short backward seeks. Requests of up to 1 MiB (aligned, so none wraps the
// ring) are read with up to 4 in flight, straight into the ring. After a seek outside the ring the readahead starts at
// 256 KiB and doubles as the reader consumes it, so the probing seeks of a demuxer don't fetch megabytes each. A lost
// connection is reopened (up to 5 tries, 0.5 s to 8 s apart) and the reading resumes where it stopped.
class SmbFile {
public:
	struct Stats {
		uint64_t size = 0;
		uint64_t buffer_size = 0;
		uint64_t buffered = 0; // Ready ahead of the read position.
		uint64_t bytes_read = 0; // From the network.
		uint64_t requests = 0;
		uint64_t resets = 0; // Seeks outside the buffered range.
		uint64_t waits = 0; // Reads that had to wait for data.
		uint64_t stalls = 0; // Of those, waits while reading on (not right after a reset).
		double wait_seconds = 0.0;
		uint64_t reconnects = 0;
		uint64_t errors = 0;
		double busy_seconds = 0.0; // Time with at least one request in flight.
		double open_seconds = 0.0; // Since open.
		double thread_cpu_seconds = 0.0;
	};

	SmbFile();
	~SmbFile();

	Error open(const String &p_url, uint64_t p_buffer_size, String &r_error);
	// Ends every wait (reads return AVERROR_EXIT) and stops the thread. close() still has to be called.
	void abort();
	void close();
	int64_t get_size() const { return size; }
	String get_description() const;
	Stats get_stats() const;
	// After the reconnect tries ran out: reads fail with AVERROR(EIO), get_error() says why.
	bool has_failed() const;
	String get_error() const;

	// AVIOContext callbacks; p_opaque is the SmbFile.
	static int avio_read(void *p_opaque, uint8_t *p_buffer, int p_size);
	static int64_t avio_seek(void *p_opaque, int64_t p_offset, int p_whence);

private:
	static constexpr uint32_t MAX_REQUESTS = 4;
	static constexpr uint64_t CHUNK_MAX = 1 << 20;
	static constexpr uint64_t FIRST_AHEAD = 256 << 10;
	static constexpr double CONNECT_TIMEOUT = 8.0;
	static constexpr int RECONNECT_TRIES = 5;

	struct Request {
		SmbFile *owner = nullptr;
		uint64_t offset = 0;
		uint32_t length = 0;
		uint32_t generation = 0;
		bool done = false;
		int status = 0;
	};

	SmbUrl url;
	SmbSession session;
	smb2fh *handle = nullptr;
	int64_t size = 0;
	String description;

	uint8_t *ring = nullptr;
	uint64_t capacity = 0;
	uint64_t chunk = CHUNK_MAX;
	uint64_t keep_back = 0;
	uint64_t max_ahead = 0;
	int wake_fd = -1;
	Thread thread;
	SafeFlag abort_flag;
	uint64_t opened_usec = 0;

	// Shared, under mutex.
	mutable BinaryMutex mutex;
	ConditionVariable condition;
	uint64_t position = 0; // The reader's position.
	uint64_t ring_start = 0; // [ring_start, ring_end): in the ring.
	uint64_t ring_end = 0;
	uint64_t issue_offset = 0; // Requested up to here.
	uint64_t ahead_limit = FIRST_AHEAD;
	uint64_t reset_point = 0;
	uint64_t reset_to = 0;
	bool reset_pending = false;
	uint32_t generation = 0; // Requests of an older generation are dropped.
	bool consumer_waiting = false;
	bool reader_wants_space = false;
	bool failed = false;
	String error;
	Stats stats;

	// Reader thread only.
	Request requests[MAX_REQUESTS];
	uint32_t request_head = 0;
	uint32_t request_count = 0;
	bool restart_pending = false;
	uint64_t busy_since_usec = 0; // Written by the reader thread under mutex.
	uint64_t drop_after_bytes = 0; // Test hook: HWVIDEO_SMB_DROP_AFTER_MB drops the connection once after that much.

	bool _open_handle(String &r_error, const SafeFlag *p_abort);
	bool _reconnect();
	void _wake();
	void _reader_loop();
	static void _thread_func(void *p_userdata);
	static void _read_done(struct smb2_context *p_smb2, int p_status, void *p_command_data, void *p_private);
	int _read(uint8_t *p_buffer, int p_size);
	int64_t _seek(int64_t p_offset, int p_whence);
};
