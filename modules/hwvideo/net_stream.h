/**************************************************************************/
/*  net_stream.h                                                          */
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

#include "core/io/stream_peer_tcp.h"
#include "core/io/stream_peer_tls.h"
#include "core/os/condition_variable.h"
#include "core/os/mutex.h"
#include "core/os/thread.h"
#include "core/string/ustring.h"
#include "core/templates/local_vector.h"
#include "core/templates/safe_refcount.h"

// http://, https:// and udp:// input for MediaSource, which reads it through an AVIOContext (avio_read, avio_seek),
// with a readahead thread and ring like SmbFile.
// - HTTP(S): a small HTTP/1.1 client on Godot's StreamPeerTCP and StreamPeerTLS (so HTTPS checks certificates against
//   Godot's CA store). GET with "Range: bytes=N-": a 206 answer (or Accept-Ranges) makes the file seekable, and a seek
//   outside the ring starts a new request there. Redirects (up to 5), chunked bodies, and a lost connection (reopened
//   at the same offset, up to 5 tries) are handled. A body without a length (a live MPEG-TS feed) is live.
// - UDP: udp://host:port (unicast to this machine; host empty, "@" or a local address) or udp://group:port
//   (multicast, joined on every interface). Datagrams go into the ring as they come: live.
// Live streams never wait for the reader: when the ring is full the oldest data goes, and a reader more than
// live_limit seconds (at the incoming rate) behind skips ahead (the MPEG-TS demuxer resyncs), so the delay stays bounded.
class NetStream {
public:
	struct Stats {
		int64_t size = -1; // -1: unknown (live).
		uint64_t buffer_size = 0;
		uint64_t buffered = 0; // Ready ahead of the read position.
		uint64_t bytes_read = 0; // From the network.
		uint64_t requests = 0; // HTTP requests sent (open, seeks, reconnects).
		uint64_t resets = 0; // Seeks outside the buffered range.
		uint64_t waits = 0; // Reads that had to wait for data.
		double wait_seconds = 0.0;
		uint64_t reconnects = 0;
		uint64_t errors = 0;
		uint64_t packets = 0; // UDP datagrams.
		uint64_t overruns = 0; // Live: bytes dropped because the reader was too far behind.
		uint64_t skips = 0; // Live: times the reader was moved ahead.
		uint64_t wakeups = 0; // Times the reader thread woke (loop turns): its share of the system calls.
		double open_seconds = 0.0;
		double thread_cpu_seconds = 0.0;
	};

	struct Url {
		bool valid = false;
		String scheme; // "http", "https", "udp".
		String host;
		int port = 0;
		String path; // HTTP: path and query, "/" if empty.
	};

	static bool is_url(const String &p_path);
	static Url parse(const String &p_url);

	NetStream();
	~NetStream();

	// Blocking: connects and (HTTP) reads the first response's headers, then starts the reader thread.
	Error open(const String &p_url, uint64_t p_buffer_size, bool p_tls_verify, String &r_error);
	void abort(); // Ends every wait (reads return AVERROR_EXIT). close() still has to be called.
	void close();
	int64_t get_size() const { return size; }
	bool is_seekable() const { return seekable; }
	bool is_live() const { return live; }
	String get_description() const { return description; }
	String get_content_type() const { return content_type; }
	Stats get_stats() const;
	bool has_failed() const;
	String get_error() const;
	// Live: the reader may be at most this many seconds behind the newest data, at the rate the data comes in
	// (measured each second); 0: no limit.
	void set_live_limit(double p_seconds);

	static int avio_read(void *p_opaque, uint8_t *p_buffer, int p_size);
	static int64_t avio_seek(void *p_opaque, int64_t p_offset, int p_whence);

private:
	static constexpr double CONNECT_TIMEOUT = 10.0;
	static constexpr int REDIRECTS_MAX = 5;
	static constexpr int RECONNECT_TRIES = 5;
	static constexpr int WAIT_MS = 25; // Socket waits, so seeks, close() and abort() are seen this quickly.
	static constexpr uint64_t SEEK_AHEAD = 256 << 10; // A seek this little past the ring waits for the reader instead.
	static constexpr uint64_t GATHER_BYTES = 32 << 10; // Live: wake a waiting reader for this much...
	static constexpr uint64_t GATHER_USEC = 20000; // ...or after this long (adds at most this to the latency).
	static constexpr int UDP_BATCH = 64; // Datagrams taken per wake-up of the UDP thread...
	static constexpr int UDP_GATHER_MS = 10; // ...which then waits this long for more to gather.

	Url url;
	bool tls_verify = true;
	int64_t size = -1;
	bool seekable = false;
	bool live = false;
	bool opened = false; // After open(): size and seekable no longer change (the thread reads them unlocked).
	String description;
	String content_type;

	uint8_t *ring = nullptr;
	uint64_t capacity = 0;
	uint64_t keep_back = 0;
	Thread thread;
	SafeFlag abort_flag;
	uint64_t opened_usec = 0;

	// Shared, under mutex.
	mutable BinaryMutex mutex;
	ConditionVariable condition;
	uint64_t position = 0; // The reader's position.
	uint64_t ring_start = 0; // [ring_start, ring_end): in the ring.
	uint64_t ring_end = 0;
	bool reset_pending = false;
	uint32_t generation = 0; // A seek outside the ring: what the thread is writing for an older one is dropped.
	bool eof = false;
	bool failed = false;
	String error;
	// Live: a reader that ran dry is woken once it can take reader_want bytes or has waited GATHER_USEC, not for every
	// datagram. reader_wait_usec: when it began to wait; 0 while it isn't waiting.
	uint64_t reader_want = 0;
	uint64_t reader_wait_usec = 0;
	double live_limit = 0.0; // Seconds.
	double arrival_rate = 0.0; // Bytes a second coming in (live), averaged.
	uint64_t rate_bytes = 0;
	uint64_t rate_since_usec = 0;
	Stats stats;

	// Reader thread only (and open()).
	Ref<StreamPeerTCP> tcp;
	Ref<StreamPeerTLS> tls;
	Ref<StreamPeer> peer;
	LocalVector<uint8_t> leftover; // Read past the headers: the start of the body.
	uint32_t leftover_at = 0;
	int64_t body_remaining = -1; // -1: until the connection closes.
	bool chunked = false;
	int64_t chunk_remaining = 0; // Chunked: bytes left in this chunk; 0: a size line comes next.
	bool body_done = false;
	int udp_socket = -1;
	int wake_fd = -1;

	// HTTP.
	bool _connect(const Url &p_url, String &r_error);
	void _disconnect();
	bool _request(uint64_t p_offset, String &r_error); // Sends the GET, reads the headers, follows redirects.
	int _peer_read(uint8_t *p_buffer, int p_size); // > 0 bytes, 0 nothing yet, -1 closed or failed.
	bool _read_line(String &r_line, double p_timeout);
	int _body_read(uint8_t *p_buffer, int p_size); // > 0 bytes, 0 nothing yet, -1 end or failure.
	bool _reconnect(uint64_t p_offset);
	// UDP.
	bool _open_udp(String &r_error);

	bool _stop_requested() const;
	void _wrote_locked(uint64_t p_bytes);
	void _wake_reader_locked(uint64_t p_now, bool p_wrote);
	void _reader_loop();
	static void _thread_func(void *p_userdata);
	int _read(uint8_t *p_buffer, int p_size);
	int64_t _seek(int64_t p_offset, int p_whence);
};
