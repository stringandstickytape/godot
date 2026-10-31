/**************************************************************************/
/*  net_stream.cpp                                                        */
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

#include "net_stream.h"

#include "core/config/project_settings.h"
#include "core/crypto/crypto.h"
#include "core/io/ip.h"
#include "core/os/os.h"
#include "core/string/print_string.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <poll.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

extern "C" {
#include <libavformat/avio.h>
#include <libavutil/error.h>
}

static double thread_cpu_now() {
	struct timespec ts;
	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static uint64_t now_usec() {
	return OS::get_singleton()->get_ticks_usec();
}

bool NetStream::is_url(const String &p_path) {
	const String lower = p_path.substr(0, 8).to_lower();
	return lower.begins_with("http://") || lower.begins_with("https://") || lower.begins_with("udp://");
}

NetStream::Url NetStream::parse(const String &p_url) {
	Url u;
	const int separator = p_url.find("://");
	if (separator <= 0) {
		return u;
	}
	u.scheme = p_url.substr(0, separator).to_lower();
	const String rest = p_url.substr(separator + 3);
	const int slash = rest.find_char('/');
	String authority = slash >= 0 ? rest.substr(0, slash) : rest;
	u.path = slash >= 0 ? rest.substr(slash) : String("/");
	const int query = authority.find_char('?');
	if (query >= 0) {
		if (slash < 0) {
			u.path = "/" + authority.substr(query);
		}
		authority = authority.substr(0, query); // udp://@:1234?pkt_size=1316: options are ignored.
	}
	const int at = authority.rfind_char('@');
	if (at >= 0) {
		authority = authority.substr(at + 1); // User information (and udp's "@" for "any address").
	}
	String port;
	if (authority.begins_with("[")) {
		const int close = authority.find_char(']');
		if (close < 0) {
			return u;
		}
		u.host = authority.substr(1, close - 1);
		port = authority.substr(close + 1).trim_prefix(":");
	} else {
		const int colon = authority.rfind_char(':');
		u.host = colon >= 0 ? authority.substr(0, colon) : authority;
		port = colon >= 0 ? authority.substr(colon + 1) : String();
	}
	u.port = port.is_valid_int() ? port.to_int() : (u.scheme == "https" ? 443 : (u.scheme == "http" ? 80 : 0));
	u.valid = ((u.scheme == "http" || u.scheme == "https") && !u.host.is_empty() && u.port > 0 && u.port < 65536) || (u.scheme == "udp" && u.port > 0 && u.port < 65536);
	return u;
}

NetStream::NetStream() {}

NetStream::~NetStream() {
	close();
}

Error NetStream::open(const String &p_url, uint64_t p_buffer_size, bool p_tls_verify, String &r_error) {
	url = parse(p_url);
	if (!url.valid) {
		r_error = vformat("Not a usable network address: %s.", p_url);
		return ERR_INVALID_PARAMETER;
	}
	tls_verify = p_tls_verify;
	opened_usec = now_usec();
	capacity = MAX(p_buffer_size, (uint64_t)1 << 20);
	ring = (uint8_t *)memalloc(capacity);
	keep_back = capacity / 8;
	stats = Stats();
	stats.buffer_size = capacity;
	if (url.scheme == "udp") {
		if (!_open_udp(r_error)) {
			close();
			return ERR_CANT_CONNECT;
		}
		live = true;
		seekable = false;
		size = -1;
	} else {
		if (!_request(0, r_error)) {
			_disconnect();
			close();
			return ERR_CANT_CONNECT;
		}
		live = size < 0;
		description = vformat("%s from %s (%s, %s)", url.scheme.to_upper(), url.host, live ? String("live") : vformat("%.1f MB", size / 1e6),
				seekable ? "seekable" : "not seekable");
	}
	stats.size = size;
	opened = true;
	thread.start(_thread_func, this);
	return OK;
}

void NetStream::abort() {
	abort_flag.set();
	{
		MutexLock lock(mutex);
		condition.notify_all();
	}
	if (wake_fd >= 0) {
		const uint64_t one = 1;
		ssize_t written = write(wake_fd, &one, sizeof(one));
		(void)written;
	}
}

void NetStream::close() {
	abort();
	if (thread.is_started()) {
		thread.wait_to_finish();
	}
	_disconnect();
	if (udp_socket >= 0) {
		::close(udp_socket);
		udp_socket = -1;
	}
	if (wake_fd >= 0) {
		::close(wake_fd);
		wake_fd = -1;
	}
	if (ring) {
		memfree(ring);
		ring = nullptr;
	}
	capacity = 0;
}

NetStream::Stats NetStream::get_stats() const {
	MutexLock lock(mutex);
	Stats s = stats;
	s.buffered = ring_end > position ? ring_end - position : 0;
	s.open_seconds = opened_usec ? (now_usec() - opened_usec) / 1e6 : 0.0;
	return s;
}

bool NetStream::has_failed() const {
	MutexLock lock(mutex);
	return failed;
}

String NetStream::get_error() const {
	MutexLock lock(mutex);
	return error;
}

void NetStream::set_live_limit(double p_seconds) {
	MutexLock lock(mutex);
	live_limit = MAX(p_seconds, 0.0);
}

// mutex held: p_bytes more are in the ring. Live: measure the incoming rate, and move a reader that fell too far behind.
void NetStream::_wrote_locked(uint64_t p_bytes) {
	ring_end += p_bytes;
	stats.bytes_read += p_bytes;
	if (!live) {
		condition.notify_all();
		return;
	}
	const uint64_t now = now_usec();
	if (rate_since_usec == 0) {
		rate_since_usec = now;
	}
	rate_bytes += p_bytes;
	const double elapsed = (now - rate_since_usec) / 1e6;
	if (elapsed >= 1.0) {
		const double rate = rate_bytes / elapsed;
		arrival_rate = arrival_rate > 0.0 ? arrival_rate * 0.7 + rate * 0.3 : rate;
		rate_bytes = 0;
		rate_since_usec = now;
	}
	if (live_limit > 0.0 && arrival_rate > 0.0) {
		const uint64_t limit = MIN(MAX((uint64_t)(arrival_rate * live_limit), (uint64_t)64 << 10), capacity / 2);
		if (ring_end - position > limit) {
			const uint64_t to = ring_end - limit / 2;
			stats.overruns += to - MAX(position, ring_start);
			stats.skips++;
			position = to;
		}
	}
	_wake_reader_locked(now, true);
}

// mutex held, live: wakes a waiting reader once it has enough, or has waited long enough with something to read.
// Without p_wrote (a quiet socket), only the second.
void NetStream::_wake_reader_locked(uint64_t p_now, bool p_wrote) {
	if (reader_wait_usec == 0) {
		return;
	}
	const uint64_t ready = ring_end > position ? ring_end - position : 0;
	if ((p_wrote && ready >= reader_want) || (ready > 0 && p_now - reader_wait_usec >= GATHER_USEC)) {
		condition.notify_all();
	}
}

bool NetStream::_stop_requested() const {
	return abort_flag.is_set();
}

/* HTTP */

// The CAs HTTPS is checked against: the project's bundle override, else the system's, else Godot's default. Loaded
// once (Godot itself loads its default only when it starts a game, not for --script runs).
static Ref<X509Certificate> trusted_cas() {
	static BinaryMutex mutex;
	static Ref<X509Certificate> cas;
	static bool loaded = false;
	MutexLock lock(mutex);
	if (!loaded) {
		loaded = true;
		const String override_path = GLOBAL_GET("network/tls/certificate_bundle_override");
		Ref<X509Certificate> chain = Ref<X509Certificate>(X509Certificate::create());
		if (chain.is_valid()) {
			Error result = ERR_FILE_NOT_FOUND;
			if (!override_path.is_empty()) {
				result = chain->load(override_path);
			} else {
				const String system = OS::get_singleton()->get_system_ca_certificates();
				if (!system.is_empty()) {
					result = chain->load_from_string(system);
				}
			}
			if (result == OK) {
				cas = chain;
			}
		}
	}
	return cas; // Null: TLSOptions::client() falls back to Godot's default.
}

// Godot loads its default CA store when the main loop starts. Only code
// that runs before that (a --script _init) needs this, and then Godot's own
// later load prints a harmless "already loaded" error. Returns true the
// first time, after loading it.
static bool load_default_cas_once() {
	static BinaryMutex mutex;
	static bool tried = false;
	MutexLock lock(mutex);
	if (tried) {
		return false;
	}
	tried = true;
	Crypto::load_default_certificates(GLOBAL_GET("network/tls/certificate_bundle_override"));
	return true;
}

bool NetStream::_connect(const Url &p_url, String &r_error) {
	_disconnect();
	const IPAddress ip = IP::get_singleton()->resolve_hostname(p_url.host);
	if (!ip.is_valid()) {
		r_error = vformat("Can't find the address of %s.", p_url.host);
		return false;
	}
	tcp.instantiate();
	if (tcp->connect_to_host(ip, p_url.port) != OK) {
		r_error = vformat("Can't connect to %s port %d.", p_url.host, p_url.port);
		return false;
	}
	const uint64_t deadline = now_usec() + (uint64_t)(CONNECT_TIMEOUT * 1e6);
	for (;;) {
		tcp->poll();
		const StreamPeerTCP::Status status = tcp->get_status();
		if (status == StreamPeerTCP::STATUS_CONNECTED) {
			break;
		}
		if (status != StreamPeerTCP::STATUS_CONNECTING) {
			r_error = vformat("%s port %d refused the connection.", p_url.host, p_url.port);
			return false;
		}
		if (_stop_requested()) {
			r_error = "Stopped.";
			return false;
		}
		if (now_usec() > deadline) {
			r_error = vformat("%s port %d doesn't answer.", p_url.host, p_url.port);
			return false;
		}
		tcp->wait(NetSocket::POLL_TYPE_IN_OUT, WAIT_MS);
	}
	tcp->set_no_delay(true);
	if (p_url.scheme == "https") {
		tls = Ref<StreamPeerTLS>(StreamPeerTLS::create());
		if (tls.is_null()) {
			r_error = "HTTPS needs TLS, and this build has none.";
			return false;
		}
		// Unsafe needs a null chain: only then does mbedtls skip the
		// certificate check. It still wants the default CA store loaded,
		// which a --script run doesn't do, so load it once and try again.
		const Ref<TLSOptions> options = tls_verify ? TLSOptions::client(trusted_cas()) : TLSOptions::client_unsafe(Ref<X509Certificate>());
		Error started = tls->connect_to_stream(tcp, p_url.host, options);
		if (started == ERR_UNCONFIGURED && load_default_cas_once()) {
			tls = Ref<StreamPeerTLS>(StreamPeerTLS::create());
			started = tls->connect_to_stream(tcp, p_url.host, options);
		}
		if (started != OK) {
			r_error = vformat("TLS with %s failed to start.", p_url.host);
			return false;
		}
		for (;;) {
			tls->poll();
			const StreamPeerTLS::Status status = tls->get_status();
			if (status == StreamPeerTLS::STATUS_CONNECTED) {
				break;
			}
			if (status == StreamPeerTLS::STATUS_ERROR_HOSTNAME_MISMATCH) {
				r_error = vformat("The TLS certificate of %s is for another name.", p_url.host);
				return false;
			}
			if (status != StreamPeerTLS::STATUS_HANDSHAKING) {
				r_error = vformat("The TLS handshake with %s failed (a certificate that isn't trusted?).", p_url.host);
				return false;
			}
			if (_stop_requested() || now_usec() > deadline) {
				r_error = vformat("The TLS handshake with %s didn't finish.", p_url.host);
				return false;
			}
			tcp->wait(NetSocket::POLL_TYPE_IN, WAIT_MS);
		}
		peer = tls;
	} else {
		peer = tcp;
	}
	return true;
}

void NetStream::_disconnect() {
	if (tls.is_valid()) {
		tls->disconnect_from_stream();
		tls.unref();
	}
	if (tcp.is_valid()) {
		tcp->disconnect_from_host();
		tcp.unref();
	}
	peer.unref();
	leftover.clear();
	leftover_at = 0;
	body_remaining = -1;
	chunked = false;
	chunk_remaining = 0;
	body_done = true;
}

int NetStream::_peer_read(uint8_t *p_buffer, int p_size) {
	if (leftover_at < leftover.size()) {
		const int n = MIN(p_size, (int)(leftover.size() - leftover_at));
		memcpy(p_buffer, leftover.ptr() + leftover_at, n);
		leftover_at += n;
		return n;
	}
	if (peer.is_null()) {
		return -1;
	}
	if (tls.is_valid()) {
		tls->poll();
		if (tls->get_status() != StreamPeerTLS::STATUS_CONNECTED) {
			return -1;
		}
	}
	int got = 0;
	if (peer->get_partial_data(p_buffer, p_size, got) != OK) {
		return -1;
	}
	if (got > 0) {
		return got;
	}
	tcp->poll();
	if (tcp->get_status() != StreamPeerTCP::STATUS_CONNECTED) {
		return -1;
	}
	tcp->wait(NetSocket::POLL_TYPE_IN, WAIT_MS);
	return 0;
}

bool NetStream::_read_line(String &r_line, double p_timeout) {
	CharString bytes;
	const uint64_t deadline = now_usec() + (uint64_t)(p_timeout * 1e6);
	for (;;) {
		if (leftover_at >= leftover.size()) {
			leftover.resize(4096);
			leftover_at = 0;
			int got = 0;
			if (tls.is_valid()) {
				tls->poll();
			}
			if (peer.is_null() || peer->get_partial_data(leftover.ptr(), 4096, got) != OK) {
				leftover.clear();
				return false;
			}
			leftover.resize(got);
			if (got == 0) {
				if (_stop_requested() || now_usec() > deadline) {
					return false;
				}
				tcp->poll();
				if (tcp->get_status() != StreamPeerTCP::STATUS_CONNECTED) {
					return false;
				}
				tcp->wait(NetSocket::POLL_TYPE_IN, WAIT_MS);
				continue;
			}
		}
		const char c = (char)leftover[leftover_at++];
		if (c == '\n') {
			break;
		}
		if (c != '\r') {
			bytes += c;
		}
		if (bytes.length() > 16384) {
			return false;
		}
	}
	r_line = String::utf8(bytes.get_data());
	return true;
}

bool NetStream::_request(uint64_t p_offset, String &r_error) {
	Url target = url;
	for (int redirect = 0; redirect <= REDIRECTS_MAX; redirect++) {
		if (!_connect(target, r_error)) {
			return false;
		}
		const bool default_port = (target.scheme == "https" && target.port == 443) || (target.scheme == "http" && target.port == 80);
		const String host = target.host.contains(":") ? "[" + target.host + "]" : target.host;
		const String request = vformat("GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: GodotMediaSource/1.0\r\nAccept: */*\r\nRange: bytes=%d-\r\nConnection: close\r\n\r\n",
				target.path, default_port ? host : vformat("%s:%d", host, target.port), (int64_t)p_offset);
		const CharString bytes = request.utf8();
		if (peer->put_data((const uint8_t *)bytes.get_data(), bytes.length()) != OK) {
			r_error = vformat("Can't send the request to %s.", target.host);
			return false;
		}
		{
			MutexLock lock(mutex);
			stats.requests++;
		}
		String status_line;
		if (!_read_line(status_line, CONNECT_TIMEOUT)) {
			r_error = vformat("%s didn't answer the request.", target.host);
			return false;
		}
		const int code = status_line.get_slicec(' ', 1).to_int();
		String content_length;
		String content_range;
		String transfer_encoding;
		String location;
		String accept_ranges;
		String type;
		for (;;) {
			String line;
			if (!_read_line(line, CONNECT_TIMEOUT)) {
				r_error = vformat("%s's answer ended in the headers.", target.host);
				return false;
			}
			if (line.is_empty()) {
				break;
			}
			const int colon = line.find_char(':');
			if (colon <= 0) {
				continue;
			}
			const String key = line.substr(0, colon).strip_edges().to_lower();
			const String value = line.substr(colon + 1).strip_edges();
			if (key == "content-length") {
				content_length = value;
			} else if (key == "content-range") {
				content_range = value;
			} else if (key == "transfer-encoding") {
				transfer_encoding = value.to_lower();
			} else if (key == "location") {
				location = value;
			} else if (key == "accept-ranges") {
				accept_ranges = value.to_lower();
			} else if (key == "content-type") {
				type = value;
			}
		}
		if ((code == 301 || code == 302 || code == 303 || code == 307 || code == 308) && !location.is_empty()) {
			if (location.begins_with("//")) {
				location = target.scheme + ":" + location;
			} else if (location.begins_with("/")) {
				location = vformat("%s://%s:%d%s", target.scheme, target.host.contains(":") ? "[" + target.host + "]" : target.host, target.port, location);
			} else if (!location.contains("://")) {
				location = vformat("%s://%s:%d%s%s", target.scheme, target.host, target.port, target.path.get_base_dir().trim_suffix("/") + "/", location);
			}
			const Url next = parse(location);
			if (!next.valid || (next.scheme != "http" && next.scheme != "https")) {
				r_error = vformat("%s redirected to an address that can't be used: %s.", target.host, location);
				return false;
			}
			target = next;
			continue;
		}
		if (code == 416 && size >= 0 && (int64_t)p_offset >= size) {
			body_remaining = 0; // A seek to the end.
			chunked = false;
			body_done = false;
			return true;
		}
		if (code != 200 && code != 206) {
			const int reason = status_line.find_char(' ', status_line.find_char(' ') + 1);
			r_error = vformat("%s answered HTTP %d%s.", target.host, code, reason > 0 ? " (" + status_line.substr(reason + 1) + ")" : String());
			return false;
		}
		if (code == 200 && p_offset > 0) {
			r_error = vformat("%s can't start the file in the middle (no Range support).", target.host);
			return false;
		}
		url = target; // Later requests (seeks, reconnects) go where the redirects led.
		chunked = transfer_encoding.contains("chunked");
		chunk_remaining = 0;
		body_done = false;
		content_type = type;
		const int64_t length = !chunked && content_length.is_valid_int() ? content_length.to_int() : -1;
		body_remaining = length;
		if (!opened) {
			if (code == 206) {
				const String total = content_range.get_slicec('/', 1).strip_edges();
				size = total.is_valid_int() ? total.to_int() : -1;
				seekable = size > 0;
			} else {
				size = length;
				seekable = accept_ranges == "bytes" && length > 0;
			}
		}
		return true;
	}
	r_error = vformat("Too many redirects from %s.", url.host);
	return false;
}

int NetStream::_body_read(uint8_t *p_buffer, int p_size) {
	if (body_done) {
		return -1;
	}
	if (!chunked) {
		if (body_remaining == 0) {
			body_done = true;
			return -1;
		}
		const int want = body_remaining > 0 ? (int)MIN((int64_t)p_size, body_remaining) : p_size;
		const int got = _peer_read(p_buffer, want);
		if (got < 0) {
			body_done = true;
			return -1;
		}
		if (body_remaining > 0) {
			body_remaining -= got;
		}
		return got;
	}
	if (chunk_remaining == 0) {
		String line;
		if (!_read_line(line, 30.0)) {
			body_done = true;
			return -1;
		}
		chunk_remaining = line.get_slicec(';', 0).strip_edges().hex_to_int();
		if (chunk_remaining <= 0) {
			body_done = true; // The last chunk (trailers are not read: the connection closes).
			return -1;
		}
	}
	const int got = _peer_read(p_buffer, (int)MIN((int64_t)p_size, chunk_remaining));
	if (got < 0) {
		body_done = true;
		return -1;
	}
	chunk_remaining -= got;
	if (got > 0 && chunk_remaining == 0) {
		String crlf;
		_read_line(crlf, 30.0);
	}
	return got;
}

// A lost connection: open it again at p_offset, up to RECONNECT_TRIES times, 0.5 s to 8 s apart.
bool NetStream::_reconnect(uint64_t p_offset) {
	double delay = 0.5;
	String last_error;
	for (int i = 0; i < RECONNECT_TRIES && !_stop_requested(); i++) {
		{
			MutexLock lock(mutex);
			stats.reconnects++;
		}
		const uint64_t until = now_usec() + (uint64_t)(delay * 1e6);
		while (now_usec() < until && !_stop_requested()) {
			OS::get_singleton()->delay_usec(WAIT_MS * 1000);
		}
		if (_request(p_offset, last_error)) {
			print_line(vformat("NetStream: %s: reconnected at byte %d.", url.host, (int64_t)p_offset));
			return true;
		}
		delay = MIN(delay * 2.0, 8.0);
	}
	MutexLock lock(mutex);
	failed = true;
	error = vformat("The connection to %s was lost: %s", url.host, last_error);
	stats.errors++;
	condition.notify_all();
	return false;
}

/* UDP */

bool NetStream::_open_udp(String &r_error) {
	wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	udp_socket = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (udp_socket < 0 || wake_fd < 0) {
		r_error = vformat("Can't make a UDP socket: %s.", String::utf8(strerror(errno)));
		return false;
	}
	int one = 1;
	setsockopt(udp_socket, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	int receive_buffer = 4 << 20;
	setsockopt(udp_socket, SOL_SOCKET, SO_RCVBUF, &receive_buffer, sizeof(receive_buffer));
	struct sockaddr_in address = {};
	address.sin_family = AF_INET;
	address.sin_port = htons((uint16_t)url.port);
	address.sin_addr.s_addr = htonl(INADDR_ANY);
	struct in_addr host = {};
	bool multicast = false;
	if (!url.host.is_empty() && url.host != "0.0.0.0") {
		if (inet_pton(AF_INET, url.host.utf8().get_data(), &host) != 1) {
			r_error = vformat("udp:// needs an IPv4 address, not %s.", url.host);
			return false;
		}
		multicast = IN_MULTICAST(ntohl(host.s_addr));
		address.sin_addr = host; // A multicast group, or the local address to listen on.
	}
	if (bind(udp_socket, (struct sockaddr *)&address, sizeof(address)) != 0) {
		r_error = vformat("Can't listen on UDP %s port %d: %s.", url.host.is_empty() ? String("any address") : url.host, url.port, String::utf8(strerror(errno)));
		return false;
	}
	int joined = 0;
	if (multicast) {
		// On every IPv4 interface (the default route's alone often isn't the one the stream comes in on).
		struct ifaddrs *interfaces = nullptr;
		if (getifaddrs(&interfaces) == 0) {
			for (struct ifaddrs *i = interfaces; i; i = i->ifa_next) {
				if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET) {
					struct ip_mreq request = {};
					request.imr_multiaddr = host;
					request.imr_interface = ((struct sockaddr_in *)i->ifa_addr)->sin_addr;
					if (setsockopt(udp_socket, IPPROTO_IP, IP_ADD_MEMBERSHIP, &request, sizeof(request)) == 0) {
						joined++;
					}
				}
			}
			freeifaddrs(interfaces);
		}
		if (joined == 0) {
			r_error = vformat("Can't join the multicast group %s.", url.host);
			return false;
		}
	}
	// What the kernel granted (net.core.rmem_max caps it, often at 208 KiB, reported doubled): the burst it can hold.
	socklen_t length = sizeof(receive_buffer);
	getsockopt(udp_socket, SOL_SOCKET, SO_RCVBUF, &receive_buffer, &length);
	const String buffer_kib = vformat("%d KiB socket buffer", receive_buffer >> 10);
	description = multicast ? vformat("UDP multicast %s port %d (%d interfaces, live, %s)", url.host, url.port, joined, buffer_kib) : vformat("UDP port %d (live, %s)", url.port, buffer_kib);
	return true;
}

/* Reader thread */

void NetStream::_thread_func(void *p_userdata) {
	static_cast<NetStream *>(p_userdata)->_reader_loop();
}

void NetStream::_reader_loop() {
	const bool udp = udp_socket >= 0;
	LocalVector<uint8_t> datagram; // UDP: a datagram that doesn't fit the ring's contiguous space.
	if (udp) {
		datagram.resize(65536);
	}
	uint64_t cpu_checked_usec = 0;
	for (;;) {
		bool reset = false;
		uint64_t reset_offset = 0;
		uint32_t gen;
		uint8_t *span = nullptr;
		uint64_t span_size = 0;
		int poll_ms = 250;
		{
			MutexLock lock(mutex);
			stats.wakeups++;
			const uint64_t now = now_usec();
			if (now - cpu_checked_usec >= 100000) { // A system call: not for every datagram.
				cpu_checked_usec = now;
				stats.thread_cpu_seconds = thread_cpu_now();
			}
			if (reader_wait_usec != 0 && ring_end > position) {
				poll_ms = (int)(GATHER_USEC / 1000); // Hand what there is to the reader soon even if nothing more comes.
			}
			if (abort_flag.is_set()) {
				return;
			}
			if (reset_pending) {
				reset_pending = false;
				reset = true;
				reset_offset = ring_end;
			}
			gen = generation;
			if (!reset) {
				if (live) {
					// Make room by dropping the oldest data, even data not read yet. A whole span's worth at once: with
					// only 64 KiB free, UDP would take one datagram per wake-up.
					const uint64_t need = 256 << 10;
					if (capacity - (ring_end - ring_start) < need) {
						const uint64_t drop = need - (capacity - (ring_end - ring_start));
						if (position < ring_start + drop) {
							stats.overruns += ring_start + drop - MAX(position, ring_start);
							position = ring_start + drop;
						}
						ring_start += drop;
					}
				} else {
					if (eof) {
						condition.wait(lock); // Until a seek or close().
						continue;
					}
					const uint64_t ahead = ring_end > position ? ring_end - position : 0;
					if (ahead >= capacity - keep_back) {
						condition.wait(lock); // Full: until the reader takes some.
						continue;
					}
					const uint64_t keep_from = position > keep_back ? position - keep_back : 0;
					ring_start = CLAMP(keep_from, ring_start, ring_end);
				}
				const uint64_t at = ring_end % capacity;
				span_size = MIN(capacity - at, capacity - (ring_end - ring_start));
				span_size = MIN(span_size, (uint64_t)256 << 10);
				span = ring + at;
			}
		}
		if (reset) {
			_disconnect();
			String request_error;
			if (!_request(reset_offset, request_error) && !_reconnect(reset_offset)) {
				continue; // Failed: reads return errors now.
			}
			continue;
		}
		int got = 0;
		if (udp) {
			struct pollfd fds[2] = { { udp_socket, POLLIN, 0 }, { wake_fd, POLLIN, 0 } };
			if (poll(fds, 2, poll_ms) <= 0 || !(fds[0].revents & POLLIN)) {
				if (fds[1].revents & POLLIN) {
					uint64_t value;
					ssize_t drained = read(wake_fd, &value, sizeof(value));
					(void)drained;
				}
				MutexLock lock(mutex);
				_wake_reader_locked(now_usec(), false);
				continue;
			}
			// Every datagram waiting (up to UDP_BATCH), straight into the ring while 64 KiB still fit there contiguously.
			uint64_t filled = 0;
			int count = 0;
			bool empty = false;
			while (count < UDP_BATCH && span_size - filled >= 65536) {
				const ssize_t n = recv(udp_socket, span + filled, span_size - filled, MSG_DONTWAIT);
				if (n <= 0) {
					empty = true;
					break;
				}
				filled += (uint64_t)n;
				count++;
			}
			if (count > 0) {
				{
					MutexLock lock(mutex);
					stats.packets += count;
					_wrote_locked(filled);
				}
				if (empty && UDP_GATHER_MS > 0) {
					// The socket is empty: let the next datagrams gather in its buffer (4 MiB asked for), so there is
					// one wake-up per UDP_GATHER_MS, not one per datagram, at any rate. close() still ends it at once.
					struct pollfd wake = { wake_fd, POLLIN, 0 };
					poll(&wake, 1, UDP_GATHER_MS);
				}
				continue;
			}
			if (span_size >= 65536) {
				continue; // Nothing there after all.
			}
			// Near the ring's end: one datagram through the side buffer, in two pieces (the drop above left 64 KiB).
			const ssize_t n = recv(udp_socket, datagram.ptr(), datagram.size(), MSG_DONTWAIT);
			if (n <= 0) {
				continue;
			}
			MutexLock lock(mutex);
			stats.packets++;
			uint64_t done = 0;
			while (done < (uint64_t)n) {
				const uint64_t at = (ring_end + done) % capacity;
				const uint64_t piece = MIN((uint64_t)n - done, capacity - at);
				memcpy(ring + at, datagram.ptr() + done, piece);
				done += piece;
			}
			_wrote_locked((uint64_t)n);
			continue;
		}
		got = _body_read(span, (int)span_size);
		if (got > 0) {
			MutexLock lock(mutex);
			if (gen == generation) {
				_wrote_locked((uint64_t)got);
			}
			continue;
		}
		if (got == 0) {
			if (live) {
				MutexLock lock(mutex);
				_wake_reader_locked(now_usec(), false);
			}
			continue;
		}
		// The body ended.
		uint64_t resume = 0;
		bool lost = false;
		{
			MutexLock lock(mutex);
			if (gen != generation || reset_pending) {
				continue; // A seek is waiting.
			}
			if (live) {
				lost = true; // A live feed doesn't end: reconnect.
			} else if (size >= 0 && (int64_t)ring_end < size) {
				lost = true;
				resume = ring_end;
			} else {
				eof = true;
				condition.notify_all();
			}
		}
		if (lost) {
			print_line(vformat("NetStream: %s: the connection ended early; reconnecting.", url.host));
			_disconnect();
			_reconnect(resume);
		}
	}
}

/* AVIOContext */

int NetStream::_read(uint8_t *p_buffer, int p_size) {
	MutexLock lock(mutex);
	uint64_t wait_start = 0;
	for (;;) {
		if (abort_flag.is_set()) {
			return AVERROR_EXIT;
		}
		if (position < ring_start) {
			position = ring_start; // Live: what was not read in time is gone.
		}
		bool ready = position < ring_end;
		if (ready && live && wait_start && !failed && !eof) {
			// It ran dry: take a batch, not each datagram as it comes (the thread wakes it, _wake_reader_locked).
			ready = ring_end - position >= reader_want || now_usec() - wait_start >= GATHER_USEC;
		}
		if (ready) {
			reader_wait_usec = 0;
			const uint64_t n = MIN((uint64_t)p_size, ring_end - position);
			uint64_t done = 0;
			while (done < n) {
				const uint64_t at = (position + done) % capacity;
				const uint64_t piece = MIN(n - done, capacity - at);
				memcpy(p_buffer + done, ring + at, piece);
				done += piece;
			}
			position += n;
			if (wait_start) {
				stats.wait_seconds += (now_usec() - wait_start) / 1e6;
			}
			condition.notify_all(); // Room for the thread.
			return (int)n;
		}
		if (failed) {
			reader_wait_usec = 0;
			return AVERROR(EIO);
		}
		if (eof) {
			reader_wait_usec = 0;
			return AVERROR_EOF;
		}
		if (!wait_start) {
			wait_start = now_usec();
			stats.waits++;
		}
		reader_want = live ? MIN((uint64_t)p_size, GATHER_BYTES) : 1;
		reader_wait_usec = wait_start;
		condition.wait(lock);
	}
}

int64_t NetStream::_seek(int64_t p_offset, int p_whence) {
	MutexLock lock(mutex);
	if (p_whence & AVSEEK_SIZE) {
		return size >= 0 ? size : AVERROR(ENOSYS);
	}
	int64_t target;
	switch (p_whence & ~AVSEEK_FORCE) {
		case SEEK_SET:
			target = p_offset;
			break;
		case SEEK_CUR:
			target = (int64_t)position + p_offset;
			break;
		case SEEK_END:
			if (size < 0) {
				return AVERROR(ENOSYS);
			}
			target = size + p_offset;
			break;
		default:
			return AVERROR(EINVAL);
	}
	if (target < 0) {
		return AVERROR(EINVAL);
	}
	if ((uint64_t)target >= ring_start && (uint64_t)target <= ring_end + (seekable ? SEEK_AHEAD : 0)) {
		position = target; // In the ring, or just ahead: the thread gets there.
		return target;
	}
	if (!seekable) {
		return AVERROR(ESPIPE);
	}
	generation++;
	reset_pending = true;
	ring_start = ring_end = position = target;
	eof = false;
	stats.resets++;
	condition.notify_all();
	return target;
}

int NetStream::avio_read(void *p_opaque, uint8_t *p_buffer, int p_size) {
	return static_cast<NetStream *>(p_opaque)->_read(p_buffer, p_size);
}

int64_t NetStream::avio_seek(void *p_opaque, int64_t p_offset, int p_whence) {
	return static_cast<NetStream *>(p_opaque)->_seek(p_offset, p_whence);
}
