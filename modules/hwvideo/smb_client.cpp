/**************************************************************************/
/*  smb_client.cpp                                                        */
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

#include "smb_client.h"

#include "core/math/math_funcs.h"
#include "core/os/os.h"
#include "core/string/print_string.h"
#include "core/templates/hash_map.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

extern "C" {
#include <libavformat/avio.h>
#include <libavutil/error.h>
// libsmb2's headers depend on this order.
// clang-format off
#include <smb2/smb2.h>
#include <smb2/libsmb2.h>
// clang-format on

// build/libsmb2-patches/0001-signing-fast-aes-cmac.patch: which block cipher SMB 3.x signing uses on this CPU.
const char *smb3_cmac_implementation(void);
}

static double thread_cpu_now() {
	struct timespec ts;
	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static uint64_t ticks_usec() {
	return OS::get_singleton()->get_ticks_usec();
}

// libsmb2 writes to its socket with writev(): on a connection the server dropped, that raises SIGPIPE, which would end
// the process. Ignore it (if nobody handles it), as network programs do; writev() then fails with EPIPE.
static void ignore_sigpipe() {
	static SafeFlag done;
	if (done.is_set()) {
		return;
	}
	done.set();
	struct sigaction current = {};
	if (sigaction(SIGPIPE, nullptr, &current) == 0 && current.sa_handler == SIG_DFL) {
		signal(SIGPIPE, SIG_IGN);
	}
}

/* SmbCredentials */

static BinaryMutex credentials_mutex;
static HashMap<String, SmbCredentials::Entry> credentials_map;

void SmbCredentials::set(const String &p_host, const String &p_user, const String &p_password, const String &p_domain) {
	MutexLock lock(credentials_mutex);
	const String key = p_host.strip_edges().to_lower();
	if (p_user.is_empty() && p_password.is_empty()) {
		credentials_map.erase(key);
		return;
	}
	credentials_map[key] = { p_user, p_password, p_domain };
}

bool SmbCredentials::get(const String &p_host, Entry &r_entry) {
	MutexLock lock(credentials_mutex);
	const Entry *entry = credentials_map.getptr(p_host.strip_edges().to_lower());
	if (!entry) {
		return false;
	}
	r_entry = *entry;
	return true;
}

void SmbCredentials::clear() {
	MutexLock lock(credentials_mutex);
	credentials_map.clear();
}

/* SmbUrl */

bool SmbUrl::is_smb(const String &p_path) {
	return p_path.begins_with("smb://");
}

SmbUrl SmbUrl::parse(const String &p_url) {
	SmbUrl url;
	if (!is_smb(p_url)) {
		return url;
	}
	const Vector<String> parts = p_url.substr(6).split("/", false);
	if (parts.is_empty()) {
		return url;
	}
	url.host = parts[0];
	if (parts.size() > 1) {
		url.share = parts[1];
	}
	for (int i = 2; i < parts.size(); i++) {
		url.path += (i > 2 ? "/" : "") + parts[i];
	}
	url.valid = !url.host.is_empty();
	return url;
}

/* SmbSession */

void SmbSession::result_callback(struct smb2_context *p_smb2, int p_status, void *p_command_data, void *p_private) {
	Result *result = static_cast<Result *>(p_private);
	result->status = p_status;
	result->data = p_command_data;
	result->done = true;
}

SmbSession::~SmbSession() {
	close();
}

bool SmbSession::connect(const String &p_host, const String &p_share, double p_timeout, String &r_error, const SafeFlag *p_abort, int p_wake_fd) {
	close();
	ignore_sigpipe();
	host = p_host;
	share = p_share;
	timeout = p_timeout;
	context = smb2_init_context();
	if (!context) {
		r_error = "Can't create an SMB context.";
		return false;
	}
	SmbCredentials::Entry credentials;
	const bool have = SmbCredentials::get(host, credentials);
	guest = !have || credentials.user.is_empty();
	// Without credentials: the guest account (most Windows hosts refuse it).
	const CharString user = (have && !credentials.user.is_empty() ? credentials.user : String("guest")).utf8();
	const CharString password = (have ? credentials.password : String()).utf8();
	const CharString domain = (have ? credentials.domain : String()).utf8();
	smb2_set_user(context, user.get_data());
	smb2_set_password(context, password.get_data());
	if (domain.length() > 0) {
		smb2_set_domain(context, domain.get_data());
	}
	smb2_set_security_mode(context, SMB2_NEGOTIATE_SIGNING_ENABLED);
	smb2_set_timeout(context, MAX(1, (int)Math::ceil(p_timeout)));

	const CharString host_utf8 = host.utf8();
	const CharString share_utf8 = share.utf8();
	Result result;
	if (smb2_connect_share_async(context, host_utf8.get_data(), share_utf8.get_data(), user.get_data(), result_callback, &result) != 0) {
		r_error = describe_error(-EINVAL, share);
		close();
		return false;
	}
	const Wait wait = run(&result, ticks_usec() + (uint64_t)(p_timeout * 1e6), p_abort, p_wake_fd);
	if (wait != WAIT_DONE || result.status != 0) {
		r_error = describe_error(result.status, share, wait);
		close();
		return false;
	}
	return true;
}

void SmbSession::close() {
	if (context) {
		smb2_destroy_context(context);
		context = nullptr;
	}
}

String SmbSession::get_description() const {
	if (!context) {
		return String();
	}
	const uint16_t dialect = smb2_get_dialect(context);
	String text = vformat("SMB %d.%d", dialect >> 8, (dialect >> 4) & 0xf);
	if ((dialect & 0xf) != 0) {
		text += vformat(".%d", dialect & 0xf);
	}
	if (dialect >= 0x0300) {
		text += vformat(", AES-CMAC signing with %s", smb3_cmac_implementation());
	}
	return text;
}

SmbSession::Wait SmbSession::run(const Result *p_result, uint64_t p_deadline_usec, const SafeFlag *p_abort, int p_wake_fd) {
	while (!p_result->done) {
		if (p_abort && p_abort->is_set()) {
			return WAIT_ABORTED;
		}
		const uint64_t now = ticks_usec();
		if (now >= p_deadline_usec) {
			return WAIT_TIMEOUT;
		}
		const int wait_ms = (int)MIN<uint64_t>((p_deadline_usec - now) / 1000 + 1, 250);
		if (!poll_once(p_wake_fd, wait_ms)) {
			return p_result->done ? WAIT_DONE : WAIT_ERROR;
		}
	}
	return WAIT_DONE;
}

bool SmbSession::poll_once(int p_wake_fd, int p_max_wait_ms) {
	if (!context) {
		return false;
	}
	size_t count = 0;
	int connect_timeout = -1; // While connecting to one of several addresses: when to try the next one.
	const t_socket *fds = smb2_get_fds(context, &count, &connect_timeout);
	const bool connecting = connect_timeout >= 0 || count != 1;
	struct pollfd pfd[9] = {};
	int n = 0;
	const short events = (short)smb2_which_events(context);
	for (size_t i = 0; fds && i < count && n < 8; i++) {
		pfd[n].fd = fds[i];
		pfd[n].events = events;
		n++;
	}
	const int smb_count = n;
	if (p_wake_fd >= 0) {
		pfd[n].fd = p_wake_fd;
		pfd[n].events = POLLIN;
		n++;
	}
	int wait_ms = p_max_wait_ms;
	if (connect_timeout >= 0 && connect_timeout < wait_ms) {
		wait_ms = connect_timeout;
	}
	const int r = poll(pfd, n, wait_ms);
	if (r < 0) {
		return errno == EINTR;
	}
	if (p_wake_fd >= 0 && (pfd[n - 1].revents & POLLIN)) {
		uint64_t value = 0;
		if (::read(p_wake_fd, &value, sizeof(value)) < 0) {
			// Nothing to drain (EAGAIN).
		}
	}
	for (int i = 0; i < smb_count; i++) {
		if (pfd[i].revents) {
			if (smb2_service_fd(context, pfd[i].fd, pfd[i].revents) < 0) {
				return false;
			}
			if (connecting) {
				return true; // A connect may have closed the other descriptors.
			}
		}
	}
	if (r == 0 || (smb_count > 0 && !pfd[0].revents)) {
		if (connecting) {
			if (connect_timeout >= 0 && r == 0 && smb2_service_fd(context, -1, 0) < 0) {
				return false; // No next address left.
			}
		} else if (smb2_service_fd(context, fds[0], 0) < 0) { // Expires timed-out requests.
			return false;
		}
	}
	return true;
}

void SmbSession::clear_error() {
	if (context) {
		smb2_set_error(context, "");
	}
}

String SmbSession::describe_error(int p_status, const String &p_what, Wait p_wait) const {
	if (p_wait == WAIT_TIMEOUT) {
		return vformat("No answer from %s within %d s.", host, (int)timeout);
	}
	if (p_wait == WAIT_ABORTED) {
		return "Cancelled.";
	}
	const uint32_t nt = context ? (uint32_t)smb2_get_nterror(context) : 0;
	const String detail = context ? String::utf8(smb2_get_error(context)) : String();
	switch (nt) {
		case 0xC000006D: // STATUS_LOGON_FAILURE
		case 0xC000006A: // STATUS_WRONG_PASSWORD
		case 0xC0000064: // STATUS_NO_SUCH_USER
		case 0xC000006E: // STATUS_ACCOUNT_RESTRICTION
		case 0xC0000071: // STATUS_PASSWORD_EXPIRED
		case 0xC0000072: // STATUS_ACCOUNT_DISABLED
		case 0xC0000193: // STATUS_ACCOUNT_EXPIRED
		case 0xC0000234: // STATUS_ACCOUNT_LOCKED_OUT
			if (guest) {
				return vformat("Access denied: no user name and password are set for %s (set_smb_credentials() with this host name), and it refused a guest login.", host);
			}
			return vformat("Access denied: %s refused the user name or password.", host);
		case 0xC0000022: // STATUS_ACCESS_DENIED
			return vformat("Access denied: %s on %s.", p_what, host);
		case 0xC00000CC: // STATUS_BAD_NETWORK_NAME
			return vformat("%s has no share named \"%s\".", host, share);
		case 0xC000000F: // STATUS_NO_SUCH_FILE
		case 0xC0000033: // STATUS_OBJECT_NAME_INVALID
		case 0xC0000034: // STATUS_OBJECT_NAME_NOT_FOUND
		case 0xC000003A: // STATUS_OBJECT_PATH_NOT_FOUND
			return vformat("Not found: %s on %s.", p_what, host);
		case 0xC0000103: // STATUS_NOT_A_DIRECTORY
			return vformat("Not a folder: %s.", p_what);
		case 0xC00000BA: // STATUS_FILE_IS_A_DIRECTORY
			return vformat("%s is a folder.", p_what);
		default:
			break;
	}
	if (detail.contains("resolve")) {
		return vformat("Can't find the host %s: the name doesn't resolve.", host);
	}
	int error_number = p_status < 0 ? -p_status : p_status;
	const int with = detail.rfind("failed with ");
	if (error_number == 0 && with >= 0) {
		error_number = detail.substr(with + 12).to_int(); // "Socket connect failed with 111"
	}
	switch (error_number) {
		case ECONNREFUSED:
			return vformat("Can't reach %s: connection refused (is file sharing on, and port 445 open?).", host);
		case ETIMEDOUT:
			return vformat("No answer from %s.", host);
		case EHOSTUNREACH:
		case ENETUNREACH:
			return vformat("Can't reach %s: no route to the host.", host);
		case ECONNRESET:
		case EPIPE:
			return vformat("The connection to %s was lost.", host);
		default:
			break;
	}
	if (!detail.is_empty()) {
		return vformat("SMB error with %s (%s): %s", host, p_what, detail);
	}
	return vformat("SMB error with %s (%s): %s", host, p_what, String::utf8(strerror(error_number)));
}

/* SmbFile */

SmbFile::SmbFile() {
	for (Request &request : requests) {
		request.owner = this;
	}
}

SmbFile::~SmbFile() {
	close();
}

bool SmbFile::_open_handle(String &r_error, const SafeFlag *p_abort) {
	smb2_context *smb2 = session.get_context();
	const String what = url.share + "/" + url.path;
	const CharString path_utf8 = url.path.utf8();
	SmbSession::Result opened;
	session.clear_error();
	if (smb2_open_async(smb2, path_utf8.get_data(), O_RDONLY, SmbSession::result_callback, &opened) != 0) {
		r_error = session.describe_error(-EINVAL, what);
		return false;
	}
	SmbSession::Wait wait = session.run(&opened, ticks_usec() + (uint64_t)(CONNECT_TIMEOUT * 1e6), p_abort, wake_fd);
	if (wait != SmbSession::WAIT_DONE || opened.status != 0 || !opened.data) {
		r_error = session.describe_error(opened.status, what, wait);
		return false;
	}
	handle = static_cast<smb2fh *>(opened.data);
	struct smb2_stat_64 st = {};
	SmbSession::Result stated;
	if (smb2_fstat_async(smb2, handle, &st, SmbSession::result_callback, &stated) != 0) {
		r_error = session.describe_error(-EINVAL, what);
		return false;
	}
	wait = session.run(&stated, ticks_usec() + (uint64_t)(CONNECT_TIMEOUT * 1e6), p_abort, wake_fd);
	if (wait != SmbSession::WAIT_DONE || stated.status != 0) {
		r_error = session.describe_error(stated.status, what, wait);
		return false;
	}
	if (st.smb2_type == SMB2_TYPE_DIRECTORY) {
		r_error = vformat("%s is a folder.", what);
		return false;
	}
	if (size > 0 && (int64_t)st.smb2_size != size) {
		print_line(vformat("SmbFile: %s changed size on reopen (%d -> %d bytes).", what, size, (int64_t)st.smb2_size));
	}
	size = (int64_t)st.smb2_size;
	return true;
}

Error SmbFile::open(const String &p_url, uint64_t p_buffer_size, String &r_error) {
	close();
	url = SmbUrl::parse(p_url);
	if (!url.valid || url.share.is_empty() || url.path.is_empty()) {
		r_error = vformat("Not an smb://host/share/path/file URL: %s", p_url);
		return ERR_INVALID_PARAMETER;
	}
	abort_flag.clear();
	wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (wake_fd < 0) {
		r_error = vformat("eventfd failed: %s", String::utf8(strerror(errno)));
		return ERR_CANT_CREATE;
	}
	opened_usec = ticks_usec();
	size = 0;
	if (!session.connect(url.host, url.share, CONNECT_TIMEOUT, r_error) || !_open_handle(r_error, nullptr)) {
		close();
		return ERR_CANT_OPEN;
	}

	// Requests are a power of two of at most 1 MiB (and what the server allows), so they tile the ring.
	const uint32_t max_read = smb2_get_max_read_size(session.get_context());
	chunk = CHUNK_MAX;
	while (chunk > 4096 && chunk > max_read) {
		chunk >>= 1;
	}
	capacity = MAX(p_buffer_size, 4 * chunk);
	capacity = (capacity + chunk - 1) / chunk * chunk;
	keep_back = capacity / 8;
	max_ahead = capacity - keep_back;
	ring = (uint8_t *)memalloc(capacity);
	description = session.get_description() + vformat(", reads of %d KiB, %d in flight, %d MiB buffer", (int)(chunk >> 10), MAX_REQUESTS, (int)(capacity >> 20));
	{
		MutexLock lock(mutex);
		position = ring_start = ring_end = issue_offset = 0;
		ahead_limit = MIN(FIRST_AHEAD, max_ahead);
		reset_point = reset_to = 0;
		reset_pending = false;
		generation = 0;
		consumer_waiting = reader_wants_space = false;
		failed = false;
		error = String();
		stats = Stats();
		stats.size = (uint64_t)size;
		stats.buffer_size = capacity;
	}
	request_head = request_count = 0;
	restart_pending = false;
	busy_since_usec = 0;
	const String drop = OS::get_singleton()->get_environment("HWVIDEO_SMB_DROP_AFTER_MB");
	drop_after_bytes = drop.is_empty() ? 0 : (uint64_t)MAX<int64_t>(drop.to_int(), 1) << 20;
	thread.start(&SmbFile::_thread_func, this);
	return OK;
}

void SmbFile::abort() {
	abort_flag.set();
	_wake();
	MutexLock lock(mutex);
	condition.notify_all();
}

void SmbFile::close() {
	abort();
	if (thread.is_started()) {
		thread.wait_to_finish();
	}
	if (handle && session.is_connected() && request_count == 0) {
		// Polite close, briefly, when no read is in flight (else its answer would come after them). Closing the
		// connection closes the file on the server anyway.
		SmbSession::Result closed;
		if (smb2_close_async(session.get_context(), handle, SmbSession::result_callback, &closed) == 0) {
			session.run(&closed, ticks_usec() + 200000);
		}
	}
	handle = nullptr;
	session.close();
	if (ring) {
		memfree(ring);
		ring = nullptr;
	}
	capacity = 0;
	if (wake_fd >= 0) {
		::close(wake_fd);
		wake_fd = -1;
	}
}

String SmbFile::get_description() const {
	return description;
}

SmbFile::Stats SmbFile::get_stats() const {
	MutexLock lock(mutex);
	Stats s = stats;
	s.buffered = ring_end > position ? ring_end - position : 0;
	if (busy_since_usec) {
		s.busy_seconds += (double)(ticks_usec() - busy_since_usec) * 1e-6;
	}
	s.open_seconds = (double)(ticks_usec() - opened_usec) * 1e-6;
	return s;
}

bool SmbFile::has_failed() const {
	MutexLock lock(mutex);
	return failed;
}

String SmbFile::get_error() const {
	MutexLock lock(mutex);
	return error;
}

void SmbFile::_wake() {
	if (wake_fd >= 0) {
		const uint64_t one = 1;
		if (::write(wake_fd, &one, sizeof(one)) < 0) {
			// The counter is already non-zero.
		}
	}
}

void SmbFile::_read_done(struct smb2_context *p_smb2, int p_status, void *p_command_data, void *p_private) {
	Request *request = static_cast<Request *>(p_private);
	request->status = p_status;
	request->done = true;
}

void SmbFile::_thread_func(void *p_userdata) {
	static_cast<SmbFile *>(p_userdata)->_reader_loop();
}

// After a lost connection: reconnect and reopen, up to RECONNECT_TRIES times. Requests in flight are dropped.
bool SmbFile::_reconnect() {
	const String reason = session.describe_error(-ECONNRESET, url.share + "/" + url.path);
	{
		MutexLock lock(mutex);
		generation++;
		stats.errors++;
	}
	handle = nullptr;
	session.close(); // Fails what is in flight; nothing more is written into the ring.
	{
		MutexLock lock(mutex);
		request_head = request_count = 0;
		if (busy_since_usec) {
			stats.busy_seconds += (double)(ticks_usec() - busy_since_usec) * 1e-6;
			busy_since_usec = 0;
		}
	}
	String last_error = reason;
	for (int attempt = 0; attempt < RECONNECT_TRIES && !abort_flag.is_set(); attempt++) {
		const int delay_ms = 500 << attempt;
		print_line(vformat("SmbFile: %s Reconnecting to %s in %.1f s (try %d of %d).", last_error, url.host, delay_ms / 1000.0, attempt + 1, RECONNECT_TRIES));
		struct pollfd pfd = { wake_fd, POLLIN, 0 };
		const uint64_t until = ticks_usec() + (uint64_t)delay_ms * 1000;
		while (!abort_flag.is_set() && ticks_usec() < until) {
			poll(&pfd, 1, (int)MAX<int64_t>(1, (int64_t)(until - ticks_usec()) / 1000));
			uint64_t value = 0;
			if (::read(wake_fd, &value, sizeof(value)) < 0) {
				// Not woken.
			}
		}
		if (abort_flag.is_set()) {
			return false;
		}
		String connect_error;
		if (session.connect(url.host, url.share, CONNECT_TIMEOUT, connect_error, &abort_flag, wake_fd) && _open_handle(connect_error, &abort_flag)) {
			MutexLock lock(mutex);
			stats.reconnects++;
			issue_offset = ring_end;
			print_line(vformat("SmbFile: reconnected to %s; reading on at byte %d.", url.host, (int64_t)ring_end));
			return true;
		}
		handle = nullptr;
		session.close();
		last_error = connect_error;
	}
	MutexLock lock(mutex);
	failed = true;
	error = vformat("Lost the connection to %s: %s", url.host, last_error);
	condition.notify_all();
	return false;
}

void SmbFile::_reader_loop() {
	Request *to_issue[MAX_REQUESTS];
	while (!abort_flag.is_set()) {
		// 1. Plan: apply a reset once the requests of the old position are in, then fill the ring up to the limit.
		int issue_count = 0;
		{
			MutexLock lock(mutex);
			if (request_count == 0 && reset_pending) {
				reset_pending = false;
				ring_start = ring_end = issue_offset = reset_to;
				ahead_limit = MIN(FIRST_AHEAD, max_ahead);
				restart_pending = false;
			} else if (request_count == 0 && restart_pending) {
				restart_pending = false;
				issue_offset = ring_end;
			}
			if (position > ring_start + keep_back) {
				ring_start = MIN(position - keep_back, ring_end);
			}
			reader_wants_space = false;
			while (!reset_pending && !restart_pending && !failed && request_count < MAX_REQUESTS && issue_offset < (uint64_t)size) {
				const uint64_t limit = MIN(ring_start + capacity, MAX(position, ring_start) + ahead_limit);
				if (issue_offset >= limit) {
					reader_wants_space = true;
					break;
				}
				uint64_t length = MIN(chunk - issue_offset % chunk, limit - issue_offset);
				length = MIN(length, (uint64_t)size - issue_offset);
				Request &request = requests[(request_head + request_count) % MAX_REQUESTS];
				request.offset = issue_offset;
				request.length = (uint32_t)length;
				request.generation = generation;
				request.done = false;
				request.status = 0;
				request_count++;
				to_issue[issue_count++] = &request;
				issue_offset += length;
				stats.requests++;
			}
			if (request_count > 0 && busy_since_usec == 0) {
				busy_since_usec = ticks_usec();
			}
			stats.thread_cpu_seconds = thread_cpu_now();
		}

		// 2. Issue, straight into the ring.
		bool lost = false;
		for (int i = 0; i < issue_count; i++) {
			Request *request = to_issue[i];
			if (smb2_pread_async(session.get_context(), handle, ring + request->offset % capacity, request->length, request->offset, &SmbFile::_read_done, request) != 0) {
				lost = true;
				break;
			}
		}
		// 3. Wait for data, a wake (seek, space, abort), or at most 1 s when idle.
		if (!lost && !session.poll_once(wake_fd, request_count > 0 ? 250 : 1000)) {
			lost = true;
		}

		// 4. Commit what arrived, in file order.
		if (!lost) {
			MutexLock lock(mutex);
			bool advanced = false;
			while (request_count > 0) {
				Request &request = requests[request_head];
				if (!request.done) {
					break;
				}
				if (request.status < 0) {
					lost = true;
					break;
				}
				request_head = (request_head + 1) % MAX_REQUESTS;
				request_count--;
				if (request.generation != generation) {
					continue; // From before a seek.
				}
				stats.bytes_read += (uint64_t)request.status;
				if (request.offset == ring_end) {
					ring_end += (uint64_t)request.status;
					advanced = true;
				}
				if ((uint32_t)request.status < request.length && ring_end < (uint64_t)size) {
					// Short read before the end: drop what is in flight and ask again from here.
					generation++;
					restart_pending = true;
				}
			}
			if (request_count == 0 && busy_since_usec) {
				stats.busy_seconds += (double)(ticks_usec() - busy_since_usec) * 1e-6;
				busy_since_usec = 0;
			}
			if (advanced && consumer_waiting) {
				consumer_waiting = false;
				condition.notify_all();
			}
			if (drop_after_bytes && stats.bytes_read >= drop_after_bytes) {
				drop_after_bytes = 0;
				print_line(vformat("SmbFile: test hook (HWVIDEO_SMB_DROP_AFTER_MB): dropping the connection after %d MiB.", (int64_t)(stats.bytes_read >> 20)));
				shutdown(smb2_get_fd(session.get_context()), SHUT_RDWR);
			}
		}
		if (lost && !abort_flag.is_set() && !_reconnect()) {
			break;
		}
	}
	MutexLock lock(mutex);
	stats.thread_cpu_seconds = thread_cpu_now();
}

int SmbFile::_read(uint8_t *p_buffer, int p_size) {
	MutexLock lock(mutex);
	uint64_t wait_start = 0;
	for (;;) {
		if (abort_flag.is_set()) {
			return AVERROR_EXIT;
		}
		if (position >= (uint64_t)size) {
			return AVERROR_EOF;
		}
		if (!reset_pending && position >= ring_start && position < ring_end) {
			break;
		}
		if (failed) {
			return AVERROR(EIO);
		}
		if (!wait_start) {
			wait_start = ticks_usec();
			stats.waits++;
			if (!reset_pending && position != reset_point) {
				stats.stalls++;
			}
		}
		consumer_waiting = true;
		if (reader_wants_space) {
			reader_wants_space = false;
			_wake();
		}
		condition.wait(lock);
	}
	if (wait_start) {
		stats.wait_seconds += (double)(ticks_usec() - wait_start) * 1e-6;
	}
	const uint64_t count = MIN((uint64_t)p_size, ring_end - position);
	const uint64_t slot = position % capacity;
	const uint64_t first = MIN(count, capacity - slot);
	memcpy(p_buffer, ring + slot, first);
	if (count > first) {
		memcpy(p_buffer + first, ring, count - first);
	}
	position += count;
	// Sequential reading: let the readahead grow.
	if (ahead_limit < max_ahead && position - reset_point >= ahead_limit / 2) {
		ahead_limit = MIN(ahead_limit * 2, max_ahead);
	}
	// Wake the reader when it waits for space and a whole request fits now (about once per request).
	if (reader_wants_space) {
		const uint64_t start = MAX(ring_start, MIN(position > keep_back ? position - keep_back : 0, ring_end));
		const uint64_t limit = MIN(start + capacity, position + ahead_limit);
		if (limit >= issue_offset + MIN(chunk, (uint64_t)size - MIN(issue_offset, (uint64_t)size))) {
			reader_wants_space = false;
			_wake();
		}
	}
	return (int)count;
}

int64_t SmbFile::_seek(int64_t p_offset, int p_whence) {
	if (p_whence & AVSEEK_SIZE) {
		return size;
	}
	MutexLock lock(mutex);
	int64_t target;
	switch (p_whence & ~AVSEEK_FORCE) {
		case SEEK_SET:
			target = p_offset;
			break;
		case SEEK_CUR:
			target = (int64_t)position + p_offset;
			break;
		case SEEK_END:
			target = size + p_offset;
			break;
		default:
			return AVERROR(EINVAL);
	}
	if (target < 0) {
		return AVERROR(EINVAL);
	}
	position = (uint64_t)target;
	// Data that is buffered or on its way (plus one request) is waited for; anything else starts a new read there.
	const uint64_t horizon = MAX(ring_end, issue_offset) + chunk;
	if (reset_pending || position < ring_start || position > horizon) {
		reset_pending = true;
		reset_to = position;
		reset_point = position;
		generation++;
		stats.resets++;
		_wake();
	}
	return target;
}

int SmbFile::avio_read(void *p_opaque, uint8_t *p_buffer, int p_size) {
	return static_cast<SmbFile *>(p_opaque)->_read(p_buffer, p_size);
}

int64_t SmbFile::avio_seek(void *p_opaque, int64_t p_offset, int p_whence) {
	return static_cast<SmbFile *>(p_opaque)->_seek(p_offset, p_whence);
}
