/**************************************************************************/
/*  v4l2_decoder.cpp                                                      */
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

#include "v4l2_decoder.h"

#include "core/config/project_settings.h"
#include "core/object/class_db.h"
#include "core/os/os.h"

#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <ctime>

#ifndef V4L2_PIX_FMT_QC08C
#define V4L2_PIX_FMT_QC08C v4l2_fourcc('Q', '0', '8', 'C')
#endif
#ifndef V4L2_PIX_FMT_QC10C
#define V4L2_PIX_FMT_QC10C v4l2_fourcc('Q', '1', '0', 'C')
#endif

static constexpr uint32_t OUTPUT_TYPE = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
static constexpr uint32_t CAPTURE_TYPE = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
static constexpr int POLL_SLICE_MS = 100;
// Fast open/close cycles can crash the decoder firmware (0x5000003), so device opens are spaced at least this far apart.
static constexpr uint64_t DEVICE_REOPEN_USEC = 1000000;

Mutex V4L2Decoder::device_close_mutex;
uint64_t V4L2Decoder::device_close_usec = 0;

#define IOCTL(m_request, m_arg) _ioctl(fd, (m_request), (m_arg), #m_request)
#define IOCTL_QUIET(m_request, m_arg, m_quiet_1, m_quiet_2) _ioctl(fd, (m_request), (m_arg), #m_request, (m_quiet_1), (m_quiet_2))

double V4L2Decoder::_now() {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

String V4L2Decoder::_fourcc_to_string(uint32_t p_fourcc) {
	String s;
	for (int i = 0; i < 4; i++) {
		const char32_t c = (p_fourcc >> (8 * i)) & 0xff;
		s += (c >= 32 && c < 127) ? c : U'.';
	}
	return s;
}

bool V4L2Decoder::_parse_codec(const String &p_codec, AnnexBSplitter::Codec &r_codec) {
	const String codec_name = p_codec.to_lower();
	if (codec_name == "h264" || codec_name == "avc") {
		r_codec = AnnexBSplitter::CODEC_H264;
		return true;
	}
	if (codec_name == "hevc" || codec_name == "h265") {
		r_codec = AnnexBSplitter::CODEC_HEVC;
		return true;
	}
	return false;
}

Error V4L2Decoder::_map_file(const String &p_path, const uint8_t *&r_data, uint64_t &r_size) {
	const String path = ProjectSettings::get_singleton() ? ProjectSettings::get_singleton()->globalize_path(p_path) : p_path;
	const int file_fd = ::open(path.utf8().get_data(), O_RDONLY | O_CLOEXEC);
	ERR_FAIL_COND_V_MSG(file_fd < 0, ERR_FILE_CANT_OPEN, vformat("V4L2Decoder: can't open %s: %s.", path, strerror(errno)));
	struct stat st;
	if (fstat(file_fd, &st) != 0 || st.st_size <= 0) {
		::close(file_fd);
		ERR_FAIL_V_MSG(ERR_FILE_CANT_READ, vformat("V4L2Decoder: %s is empty or unreadable.", path));
	}
	void *map = mmap(nullptr, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, file_fd, 0);
	::close(file_fd);
	ERR_FAIL_COND_V_MSG(map == MAP_FAILED, ERR_FILE_CANT_READ, vformat("V4L2Decoder: mmap of %s failed: %s.", path, strerror(errno)));
	madvise(map, (size_t)st.st_size, MADV_SEQUENTIAL);
	r_data = (const uint8_t *)map;
	r_size = (uint64_t)st.st_size;
	return OK;
}

void V4L2Decoder::_unmap_file() {
	if (file_data) {
		munmap((void *)file_data, file_size);
		file_data = nullptr;
		file_size = 0;
	}
}

void V4L2Decoder::_log(const String &p_message) const {
	print_line("V4L2Decoder: " + p_message);
}

void V4L2Decoder::_log_verbose(const String &p_message) const {
	if (verbose || OS::get_singleton()->is_stdout_verbose()) {
		print_line("V4L2Decoder: " + p_message);
	}
}

void V4L2Decoder::_fail(const String &p_message) {
	{
		MutexLock lock(mutex);
		state = STATE_ERROR;
		last_error = p_message;
	}
	ERR_PRINT("V4L2Decoder: " + p_message);
}

void V4L2Decoder::_set_state(State p_state) {
	MutexLock lock(mutex);
	if (state != STATE_ERROR) {
		state = p_state;
	}
}

void V4L2Decoder::_wake() {
	if (wake_fd >= 0) {
		const uint64_t one = 1;
		ssize_t written = write(wake_fd, &one, sizeof(one));
		(void)written;
	}
}

// Every ioctl goes through here: retries on EINTR, records and prints failures (the first few of each kind). p_quiet_1 and
// p_quiet_2 are errnos that are part of normal flow (EAGAIN, EPIPE, ...) and are returned silently.
int V4L2Decoder::_ioctl(int p_fd, unsigned long p_request, void *p_arg, const char *p_name, int p_quiet_1, int p_quiet_2) {
	int r;
	do {
		r = ioctl(p_fd, p_request, p_arg);
	} while (r < 0 && errno == EINTR);
	if (r < 0) {
		const int e = errno;
		if (e != p_quiet_1 && e != p_quiet_2) {
			IoctlError *entry = nullptr;
			for (uint32_t i = 0; i < ioctl_error_count; i++) {
				if (ioctl_errors[i].error == e && strcmp(ioctl_errors[i].name, p_name) == 0) {
					entry = &ioctl_errors[i];
					break;
				}
			}
			if (entry == nullptr && ioctl_error_count < std_size(ioctl_errors)) {
				entry = &ioctl_errors[ioctl_error_count++];
				entry->name = p_name;
				entry->error = e;
			}
			const uint32_t count = entry ? ++entry->count : 1;
			if (count < 5) {
				_log(vformat("%s failed: %s (errno %d).", p_name, strerror(e), e));
			} else if (count == 5) {
				_log(vformat("(further %s errno %d failures not printed)", p_name, e));
			}
		}
		errno = e;
	}
	return r;
}

/* Device and OUTPUT queue */

Error V4L2Decoder::_open_device() {
	{
		uint64_t wait_usec = 0;
		{
			MutexLock lock(device_close_mutex);
			const uint64_t now = OS::get_singleton()->get_ticks_usec();
			if (device_close_usec != 0 && now < device_close_usec + DEVICE_REOPEN_USEC) {
				wait_usec = device_close_usec + DEVICE_REOPEN_USEC - now;
			}
		}
		if (wait_usec > 0) {
			_log_verbose(vformat("waiting %d ms since the last close before reopening the device.", wait_usec / 1000));
			OS::get_singleton()->delay_usec(wait_usec);
		}
	}

	fd = ::open(device_path.utf8().get_data(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) {
		const int e = errno;
		_fail(vformat("Can't open the decoder %s: %s. V4L2Decoder needs a V4L2 stateful decoder, e.g. the Steam Frame's (Qualcomm iris, /dev/video-dec0).%s", device_path, strerror(e),
				e == EACCES ? " Check the device permissions and the user's groups." : ""));
		return ERR_CANT_OPEN;
	}

	struct v4l2_capability capability = {};
	if (IOCTL(VIDIOC_QUERYCAP, &capability) < 0) {
		_fail(vformat("VIDIOC_QUERYCAP on %s failed: %s. Is it a V4L2 device?", device_path, strerror(errno)));
		_close_device("QUERYCAP failed");
		return ERR_CANT_OPEN;
	}
	const uint32_t caps = (capability.capabilities & V4L2_CAP_DEVICE_CAPS) ? capability.device_caps : capability.capabilities;
	_log(vformat("opened %s: driver '%s', card '%s', device_caps 0x%x.", device_path, String::utf8((const char *)capability.driver), String::utf8((const char *)capability.card), caps));
	if (!(caps & V4L2_CAP_VIDEO_M2M_MPLANE) || !(caps & V4L2_CAP_STREAMING)) {
		_fail(vformat("%s is not a streaming multi-planar memory-to-memory device.", device_path));
		_close_device("not a decoder");
		return ERR_CANT_OPEN;
	}
	return OK;
}

// STREAMOFF both queues, free every buffer, close the decoder fd. Order matters: STREAMOFF, then drop the fds, then REQBUFS(0).
void V4L2Decoder::_close_device(const char *p_why) {
	if (fd < 0) {
		return;
	}
	_teardown_capture(p_why);
	if (output_streaming) {
		int type = OUTPUT_TYPE;
		const int r = IOCTL(VIDIOC_STREAMOFF, &type);
		_log_verbose(vformat("STREAMOFF OUTPUT: %s.", r ? strerror(errno) : "ok"));
		output_streaming = false;
	}
	if (output_count) {
		for (uint32_t i = 0; i < output_count; i++) {
			if (output[i].map) {
				munmap(output[i].map, output[i].length);
			}
			output[i] = OutputBuffer();
		}
		struct v4l2_requestbuffers request = {};
		request.count = 0;
		request.type = OUTPUT_TYPE;
		request.memory = V4L2_MEMORY_MMAP;
		IOCTL(VIDIOC_REQBUFS, &request);
		output_count = 0;
	}
	output_in_driver = 0;
	::close(fd);
	fd = -1;
	{
		MutexLock lock(device_close_mutex);
		device_close_usec = OS::get_singleton()->get_ticks_usec();
	}
	_log(vformat("decoder closed (%s).", p_why));
}

Error V4L2Decoder::_setup_output() {
	// OUTPUT buffers hold one access unit each. The largest one isn't known without a scan of the whole file, so size them
	// from the coded size; access units that don't fit are skipped and logged.
	uint64_t want = output_buffer_size > 0 ? (uint64_t)output_buffer_size : CLAMP((uint64_t)output_size.width * output_size.height * 3 / 4, (uint64_t)1 << 20, (uint64_t)16 << 20);
	want = (want + 4095) & ~(uint64_t)4095;

	struct v4l2_format format = {};
	format.type = OUTPUT_TYPE;
	format.fmt.pix_mp.pixelformat = codec_fourcc;
	format.fmt.pix_mp.width = output_size.width;
	format.fmt.pix_mp.height = output_size.height;
	format.fmt.pix_mp.num_planes = 1;
	format.fmt.pix_mp.plane_fmt[0].sizeimage = (uint32_t)want;
	if (IOCTL(VIDIOC_S_FMT, &format) < 0) {
		_fail(vformat("S_FMT OUTPUT %s %dx%d failed: %s.", _fourcc_to_string(codec_fourcc), output_size.width, output_size.height, strerror(errno)));
		return FAILED;
	}
	if (format.fmt.pix_mp.pixelformat != codec_fourcc) {
		_fail(vformat("The driver replaced the coded format %s with %s: codec not supported.", _fourcc_to_string(codec_fourcc), _fourcc_to_string(format.fmt.pix_mp.pixelformat)));
		return ERR_UNAVAILABLE;
	}
	output_sizeimage = format.fmt.pix_mp.plane_fmt[0].sizeimage;
	_log(vformat("S_FMT OUTPUT %s %dx%d: sizeimage %d (asked %d).", _fourcc_to_string(codec_fourcc), output_size.width, output_size.height, output_sizeimage, want));

	struct v4l2_requestbuffers request = {};
	request.count = output_buffer_count;
	request.type = OUTPUT_TYPE;
	request.memory = V4L2_MEMORY_MMAP;
	if (IOCTL(VIDIOC_REQBUFS, &request) < 0 || request.count == 0) {
		_fail(vformat("REQBUFS OUTPUT failed: %s.", request.count == 0 ? "0 buffers" : strerror(errno)));
		return FAILED;
	}
	output_count = MIN(request.count, MAX_OUTPUT_BUFFERS);
	for (uint32_t i = 0; i < output_count; i++) {
		struct v4l2_plane planes[VIDEO_MAX_PLANES] = {};
		struct v4l2_buffer buffer = {};
		buffer.index = i;
		buffer.type = OUTPUT_TYPE;
		buffer.memory = V4L2_MEMORY_MMAP;
		buffer.m.planes = planes;
		buffer.length = VIDEO_MAX_PLANES;
		if (IOCTL(VIDIOC_QUERYBUF, &buffer) < 0) {
			_fail("QUERYBUF OUTPUT failed.");
			return FAILED;
		}
		void *map = mmap(nullptr, planes[0].length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, planes[0].m.mem_offset);
		if (map == MAP_FAILED) {
			_fail(vformat("mmap of OUTPUT buffer %d failed: %s.", i, strerror(errno)));
			return FAILED;
		}
		output[i].map = map;
		output[i].length = planes[0].length;
		output[i].queued = false;
	}
	_log_verbose(vformat("OUTPUT: %d MMAP buffers of %d bytes.", output_count, output[0].length));
	return OK;
}

Error V4L2Decoder::_subscribe_events() {
	struct v4l2_event_subscription subscription = {};
	subscription.type = V4L2_EVENT_SOURCE_CHANGE;
	if (IOCTL(VIDIOC_SUBSCRIBE_EVENT, &subscription) < 0) {
		_fail("Subscribing to V4L2_EVENT_SOURCE_CHANGE failed.");
		return FAILED;
	}
	subscription = {};
	subscription.type = V4L2_EVENT_EOS;
	if (IOCTL(VIDIOC_SUBSCRIBE_EVENT, &subscription) < 0) {
		_log_verbose("EOS event not available; relying on V4L2_BUF_FLAG_LAST.");
	}
	return OK;
}

// Open the decoder (if needed), set up OUTPUT, queue the first access units, STREAMON OUTPUT.
Error V4L2Decoder::_start_session() {
	seq_at_open = seq;
	Error err = OK;
	if (fd < 0) {
		err = _open_device();
		if (err != OK) {
			return err;
		}
	}
	err = _setup_output();
	if (err == OK) {
		err = _subscribe_events();
	}
	if (err != OK) {
		return err;
	}
	if (_queue_input() < 0) {
		_fail("Queueing the first access units failed.");
		return FAILED;
	}
	if (source.is_valid() && _wait_for_source_input() != OK) {
		return FAILED;
	}
	int type = OUTPUT_TYPE;
	if (IOCTL(VIDIOC_STREAMON, &type) < 0) {
		_fail(vformat("STREAMON OUTPUT failed: %s.", strerror(errno)));
		return FAILED;
	}
	output_streaming = true;
	_set_state(STATE_STARTING);
	_log_verbose(vformat("OUTPUT streaming, %d access units queued; waiting for SOURCE_CHANGE.", output_in_driver));
	return OK;
}

// Closes the whole session, reopens the device and resumes at p_resume.
Error V4L2Decoder::_restart_session(const AnnexBSplitter::AccessUnit &p_resume, uint64_t p_media_index, const char *p_why) {
	_log(vformat("session restart (%s): closing and reopening the device, resuming at access unit %d (offset %d).", p_why, p_media_index, p_resume.offset));
	_close_device("session restart");
	splitter.seek(p_resume.offset);
	media_index = p_media_index;
	if (p_resume.sps_width) {
		output_size = Size2i(p_resume.sps_width, p_resume.sps_height);
	}
	need_setup = false;
	pending_reconfig = false;
	got_last = false;
	input_done = false;
	stop_sent = false;
	stop_failed = false;
	ended = false;
	eos_events = 0;
	_begin_epoch(serial, media_index);
	wstats.restarts++;
	return _start_session();
}

// Source mode: closes the whole session, reopens the device and feeds from the source's queue (after a seek).
Error V4L2Decoder::_restart_session_source(const char *p_why) {
	_log(vformat("session restart (%s): closing and reopening the device.", p_why));
	_close_device("session restart");
	if (last_sps_size.width > 0) {
		output_size = last_sps_size;
	}
	need_setup = false;
	pending_reconfig = false;
	got_last = false;
	input_done = false;
	stop_sent = false;
	stop_failed = false;
	ended = false;
	eos_events = 0;
	wstats.restarts++;
	return _start_session();
}

// Source mode: the session starts with access units in the driver. The source may still be reading (just opened, or
// seeking), so wait for them.
Error V4L2Decoder::_wait_for_source_input() {
	const double deadline = _now() + MAX(timeout_ms / 1000.0, 10.0);
	while (output_in_driver == 0 && !input_done && !exit_requested.is_set()) {
		if (_now() > deadline) {
			_fail(vformat("No video access unit from the MediaSource in %.0f s (source state %d: %s).", MAX(timeout_ms / 1000.0, 10.0), (int)source->get_state(), source->get_last_error()));
			return FAILED;
		}
		struct pollfd wait_fd = {};
		wait_fd.fd = wake_fd;
		wait_fd.events = POLLIN;
		if (poll(&wait_fd, 1, POLL_SLICE_MS) > 0) {
			uint64_t value;
			const ssize_t r = read(wake_fd, &value, sizeof(value));
			(void)r;
		}
		if (_queue_input() < 0) {
			_fail("Queueing the first access units failed.");
			return FAILED;
		}
	}
	return OK;
}

/* Input */

uint64_t V4L2Decoder::_timestamp_for_seq(uint64_t p_seq) const {
	// Real timestamps (seq / fps): the driver may estimate the frame rate from them.
	return (uint64_t)((double)p_seq * 1e6 / fps + 0.5);
}

uint64_t V4L2Decoder::_seq_for_timestamp(uint64_t p_usec) const {
	return (uint64_t)((double)p_usec * fps / 1e6 + 0.5);
}

double V4L2Decoder::_queued_rate() const {
	if (seq - seq_at_open <= RATE_WINDOW) {
		return 0.0;
	}
	const double span = queue_time[(seq - 1) % QUEUE_RING] - queue_time[(seq - 1 - RATE_WINDOW) % QUEUE_RING];
	return span > 0.0 ? RATE_WINDOW / span : 0.0;
}

int V4L2Decoder::_free_output_buffer() const {
	for (uint32_t i = 0; i < output_count; i++) {
		if (!output[i].queued) {
			return (int)i;
		}
	}
	return -1;
}

// Copies the access unit into OUTPUT buffer p_index and queues it. 1: queued, 0: skipped (too large), -1: error.
int V4L2Decoder::_queue_access_unit(uint32_t p_index, const AnnexBSplitter::AccessUnit &p_au) {
	recent[recent_count % RECENT_AUS].au = p_au;
	recent[recent_count % RECENT_AUS].media_index = media_index;
	recent_count++;
	media_index++;

	if (p_au.size > output[p_index].length) {
		_log(vformat("access unit %d (%d bytes) doesn't fit an OUTPUT buffer (%d bytes); skipped. Set output_buffer_size.", media_index - 1, p_au.size, output[p_index].length));
		wstats.aus_skipped++;
		return 0;
	}
	memcpy(output[p_index].map, file_data + p_au.offset, p_au.size);
	return _qbuf_output(p_index, p_au.size);
}

// Queues OUTPUT buffer p_index, holding p_bytes of bitstream, under the next seq. 1: queued, -1: error.
int V4L2Decoder::_qbuf_output(uint32_t p_index, uint64_t p_bytes) {
	struct v4l2_plane planes[1] = {};
	planes[0].bytesused = (uint32_t)p_bytes;
	planes[0].length = (uint32_t)output[p_index].length;
	struct v4l2_buffer buffer = {};
	buffer.index = p_index;
	buffer.type = OUTPUT_TYPE;
	buffer.memory = V4L2_MEMORY_MMAP;
	buffer.m.planes = planes;
	buffer.length = 1;
	const uint64_t usec = _timestamp_for_seq(seq);
	buffer.timestamp.tv_sec = (time_t)(usec / 1000000);
	buffer.timestamp.tv_usec = (suseconds_t)(usec % 1000000);
	if (IOCTL(VIDIOC_QBUF, &buffer) < 0) {
		return -1;
	}
	queue_seq[seq % QUEUE_RING] = seq;
	queue_time[seq % QUEUE_RING] = _now();
	output[p_index].queued = true;
	output_in_driver++;
	seq++;
	wstats.aus_queued++;
	return 1;
}

// Source mode: makes `held` the next access unit to feed. Access units read before the source's latest seek are dropped;
// the first one after a seek starts a new epoch under its serial. False if none is queued yet, or the input is done.
bool V4L2Decoder::_hold_source_packet() {
	for (;;) {
		if (!have_held) {
			if (!source->pop_video_packet(held)) {
				if (source->is_video_input_done(serial)) {
					input_done = true;
					_log(vformat("end of the input after %d access units.", media_index));
				}
				return false;
			}
			have_held = true;
		}
		if (held.serial != source->get_serial()) {
			source->release_video_packet(held);
			have_held = false;
			continue;
		}
		if (held.serial != serial) {
			serial = held.serial;
			media_index = 0;
			_begin_epoch(serial, 0);
			// Only mid-session: a session starts with a clean decoder anyway.
			eos_pending = seek_eos != SEEK_EOS_OFF && seq > seq_at_open;
			static const char *nal_names_h264[] = { "?", "I (non-IDR)", "?", "?", "?", "IDR" };
			String nal = "?";
			if (held.nal_type >= 0) {
				if (codec == AnnexBSplitter::CODEC_H264) {
					nal = held.nal_type <= 5 ? String(nal_names_h264[held.nal_type]) : itos(held.nal_type);
				} else {
					nal = held.nal_type == 19 || held.nal_type == 20 ? "IDR" : held.nal_type == 21 ? "CRA" : (held.nal_type >= 16 && held.nal_type <= 18) ? "BLA" : itos(held.nal_type);
				}
			}
			_log(vformat("seek (serial %d): first access unit at %.3f s, %s%s, %s.", serial, held.pts, held.keyframe ? "keyframe " : "", nal,
					held.sps_width ? " with SPS" : "", eos_pending ? (seek_eos == SEEK_EOS_PREPEND ? "end-of-sequence NAL unit in front" : "end-of-sequence NAL unit in its own buffer") : "no end-of-sequence NAL unit"));
		}
		return true;
	}
}

// Source mode: copies `held` into OUTPUT buffer p_index and queues it, with its PTS kept for the frame it decodes to.
// With a pending end-of-sequence NAL unit (seek_eos), that goes in front, or alone into this buffer (`held` stays).
int V4L2Decoder::_queue_source_packet(uint32_t p_index) {
	static const uint8_t h264_eos[] = { 0, 0, 0, 1, 0x0a }; // nal_unit_type 10, end of sequence.
	static const uint8_t hevc_eos[] = { 0, 0, 0, 1, 0x48, 0x01 }; // nal_unit_type 36 (EOS_NUT), layer 0, TemporalId 0.
	const uint8_t *eos = codec == AnnexBSplitter::CODEC_H264 ? h264_eos : hevc_eos;
	const uint32_t eos_size = codec == AnnexBSplitter::CODEC_H264 ? sizeof(h264_eos) : sizeof(hevc_eos);
	if (eos_pending && seek_eos == SEEK_EOS_SEPARATE) {
		eos_pending = false;
		memcpy(output[p_index].map, eos, eos_size);
		queue_pts[seq % QUEUE_RING] = held.pts;
		wstats.eos_inserted++;
		return _qbuf_output(p_index, eos_size);
	}
	const uint32_t prefix = eos_pending ? eos_size : 0;
	eos_pending = false;

	RecentAU &recent_au = recent[recent_count % RECENT_AUS];
	recent_au.au = AnnexBSplitter::AccessUnit();
	recent_au.au.size = held.size;
	recent_au.au.sps_width = held.sps_width;
	recent_au.au.sps_height = held.sps_height;
	recent_au.media_index = media_index;
	recent_au.pts = held.pts;
	recent_count++;
	if (media_index == 0) {
		epoch_start_pts = held.pts;
	}
	media_index++;
	input_time = held.pts;
	if (held.sps_width) {
		last_sps_size = Size2i(held.sps_width, held.sps_height);
	}
	int r = 0;
	if (held.size + prefix > output[p_index].length) {
		_log(vformat("access unit at %.3f s (%d bytes) doesn't fit an OUTPUT buffer (%d bytes); skipped. Set output_buffer_size.", held.pts, held.size, output[p_index].length));
		wstats.aus_skipped++;
	} else {
		if (prefix) {
			memcpy(output[p_index].map, eos, prefix);
			wstats.eos_inserted++;
		}
		memcpy((uint8_t *)output[p_index].map + prefix, held.data, held.size);
		queue_pts[seq % QUEUE_RING] = held.pts;
		r = _qbuf_output(p_index, held.size + prefix);
	}
	source->release_video_packet(held);
	have_held = false;
	return r;
}

// Feeds access units while the input is no more than lead_time ahead of the clock. Until the epoch's first frame is out the
// feed is unpaced (SOURCE_CHANGE and B-frame reordering need a few access units), but at most all OUTPUT buffers but one
// are in the driver: during a resolution change the driver keeps what it has, and _rate_reset() needs a free buffer.
int V4L2Decoder::_queue_input() {
	int queued = 0;
	while (!input_done && !pending_reconfig && !exit_requested.is_set()) {
		if (output_count > 1 && output_in_driver + 1 >= output_count) {
			break;
		}
		if (source.is_valid()) {
			if (!_hold_source_packet()) {
				break;
			}
			// Paced like the file path: access unit k of an epoch is due at the epoch's start + k / fps (decode order runs at
			// the frame rate), and never later than its own PTS. Not by DTS: Matroska has none (FFmpeg sets it to the PTS,
			// which jumps around with B-frames).
			const double due = MIN(epoch_start_pts + (double)media_index / fps, held.pts);
			if (epochs[epochs.size() - 1].frames > 0 && due > worker_clock + lead_time) {
				break;
			}
			const int index = _free_output_buffer();
			if (index < 0) {
				break;
			}
			const int r = _queue_source_packet((uint32_t)index);
			if (r < 0) {
				return -1;
			}
			queued += r;
			continue;
		}
		const Epoch &epoch = epochs[epochs.size() - 1];
		if (epoch.frames > 0 && (double)media_index / fps > worker_clock + lead_time) {
			break;
		}
		const int index = _free_output_buffer();
		if (index < 0) {
			break;
		}
		AnnexBSplitter::AccessUnit au;
		if (!splitter.next(au)) {
			if (!loop || media_index == epoch.base_index) {
				input_done = true;
				_log(vformat("end of the stream after %d access units.", media_index));
				break;
			}
			splitter.seek(0);
			wstats.loops++;
			_begin_epoch(serial, media_index);
			continue;
		}
		const int r = _queue_access_unit((uint32_t)index, au);
		if (r < 0) {
			return -1;
		}
		queued += r;
	}
	return queued;
}

void V4L2Decoder::_begin_epoch(uint32_t p_serial, uint64_t p_base_index) {
	Epoch epoch;
	epoch.first_seq = seq;
	epoch.serial = p_serial;
	epoch.base_index = p_base_index;
	if (epochs.size() >= MAX_EPOCHS) {
		epochs.remove_at(0);
	}
	epochs.push_back(epoch);
}

V4L2Decoder::Epoch *V4L2Decoder::_find_epoch(uint64_t p_seq) {
	for (int64_t i = (int64_t)epochs.size() - 1; i >= 0; i--) {
		if (epochs[i].first_seq <= p_seq) {
			return &epochs[i];
		}
	}
	return epochs.is_empty() ? nullptr : &epochs[0];
}

// The fork's admission check (check_core_mbps_mbpf() in iris_helpers.c) runs on every REQBUFS and both STREAMONs, and fails
// with ENOMEM when mbpf x fps > 7833600, where fps is at least the wall-clock rate of the last 30 OUTPUT QBUFs. Before a
// CAPTURE re-setup, pause until the last 30 intervals span 30 / reconfig_rate seconds, then queue one access unit: that one
// long interval brings the driver's estimate down. A no-op when the feed was paced below reconfig_rate.
void V4L2Decoder::_rate_reset() {
	if (reconfig_rate <= 0.0 || seq - seq_at_open < RATE_WINDOW) {
		return;
	}
	const double before = _queued_rate();
	const double target = queue_time[(seq - RATE_WINDOW) % QUEUE_RING] + RATE_WINDOW / reconfig_rate;
	double now = _now();
	if (now >= target) {
		_log_verbose(vformat("rate reset: queued-rate estimate %.1f fps, no pause needed.", before));
		return;
	}
	const int index = _free_output_buffer();
	if (index < 0) {
		_log(vformat("rate reset: no free OUTPUT buffer (estimate %.1f fps); REQBUFS may fail with ENOMEM.", before));
		return;
	}
	AnnexBSplitter::AccessUnit au;
	if (source.is_valid() ? !_hold_source_packet() : !splitter.next(au)) {
		_log(vformat("rate reset: no access unit left to queue (estimate %.1f fps).", before));
		return;
	}
	_log(vformat("rate reset: queued-rate estimate %.1f fps; pausing %d ms, then queueing one access unit.", before, (int)((target - now) * 1e3)));
	while (!exit_requested.is_set() && (now = _now()) < target) {
		OS::get_singleton()->delay_usec((uint64_t)(MIN(target - now, 0.05) * 1e6) + 1);
	}
	if ((source.is_valid() ? _queue_source_packet((uint32_t)index) : _queue_access_unit((uint32_t)index, au)) == 1) {
		_log_verbose(vformat("rate reset: estimate now %.1f fps.", _queued_rate()));
	}
}

int V4L2Decoder::_dequeue_output() {
	int count = 0;
	for (;;) {
		struct v4l2_plane planes[VIDEO_MAX_PLANES] = {};
		struct v4l2_buffer buffer = {};
		buffer.type = OUTPUT_TYPE;
		buffer.memory = V4L2_MEMORY_MMAP;
		buffer.m.planes = planes;
		buffer.length = VIDEO_MAX_PLANES;
		if (IOCTL_QUIET(VIDIOC_DQBUF, &buffer, EAGAIN, 0) < 0) {
			break;
		}
		if (buffer.index < output_count && output[buffer.index].queued) {
			output[buffer.index].queued = false;
			output_in_driver--;
		}
		if (buffer.flags & V4L2_BUF_FLAG_ERROR) {
			wstats.output_errors++;
		}
		count++;
	}
	return count;
}

/* CAPTURE queue */

Error V4L2Decoder::_queue_capture(uint32_t p_index) {
	struct v4l2_plane planes[VIDEO_MAX_PLANES] = {};
	struct v4l2_buffer buffer = {};
	buffer.index = p_index;
	buffer.type = CAPTURE_TYPE;
	buffer.memory = V4L2_MEMORY_MMAP;
	buffer.m.planes = planes;
	buffer.length = 1;
	if (IOCTL(VIDIOC_QBUF, &buffer) < 0) {
		return FAILED;
	}
	capture[p_index].queued = true;
	capture_in_driver++;
	return OK;
}

void V4L2Decoder::_explain_enomem(const char *p_what) {
	if (errno != ENOMEM) {
		return;
	}
	const double rate = _queued_rate();
	const double mbpf = (double)((output_size.width + 15) / 16) * (double)((output_size.height + 15) / 16);
	_log(vformat("%s ENOMEM: most likely the driver's admission check, not memory: mbpf %d x max(30, queued rate ~%d fps) = %d vs MAX_MBPS 7833600 (dmesg: 'current session not supported(-12)').",
			p_what, (int64_t)mbpf, (int64_t)rate, (int64_t)(mbpf * MAX(rate, 30.0))));
}

// Only after SOURCE_CHANGE: the CAPTURE format is valid only from then on. The driver can DMA a full frame into too-small
// buffers (SMMU fault), so buffers are allocated from the format reported then, and reallocated on every later change.
Error V4L2Decoder::_setup_capture() {
	const double t_start = _now();
	struct v4l2_format format = {};
	format.type = CAPTURE_TYPE;
	if (IOCTL(VIDIOC_G_FMT, &format) < 0) {
		_fail("G_FMT CAPTURE failed.");
		return FAILED;
	}
	_log_verbose(vformat("G_FMT CAPTURE after SOURCE_CHANGE: %s %dx%d, %d plane(s).", _fourcc_to_string(format.fmt.pix_mp.pixelformat), format.fmt.pix_mp.width, format.fmt.pix_mp.height, format.fmt.pix_mp.num_planes));
	if (capture_setups == 0 && (verbose || OS::get_singleton()->is_stdout_verbose())) {
		for (uint32_t i = 0;; i++) {
			struct v4l2_fmtdesc description = {};
			description.index = i;
			description.type = CAPTURE_TYPE;
			if (IOCTL_QUIET(VIDIOC_ENUM_FMT, &description, EINVAL, 0) < 0) {
				break;
			}
			_log(vformat("  CAPTURE format %d: %s '%s'.", i, _fourcc_to_string(description.pixelformat), String::utf8((const char *)description.description)));
		}
	}

	if (capture_fourcc && format.fmt.pix_mp.pixelformat != capture_fourcc) {
		struct v4l2_format want = format;
		want.fmt.pix_mp.pixelformat = capture_fourcc;
		IOCTL(VIDIOC_S_FMT, &want);
		format = {};
		format.type = CAPTURE_TYPE;
		if (IOCTL(VIDIOC_G_FMT, &format) < 0) {
			_fail("G_FMT CAPTURE failed.");
			return FAILED;
		}
		if (format.fmt.pix_mp.pixelformat != capture_fourcc) {
			_log(vformat("WARNING: asked for CAPTURE %s, the driver chose %s.", _fourcc_to_string(capture_fourcc), _fourcc_to_string(format.fmt.pix_mp.pixelformat)));
		}
	}
	const struct v4l2_pix_format_mplane &pf = format.fmt.pix_mp;
	if (pf.num_planes != 1) {
		_fail(vformat("CAPTURE format %s has %d planes; only single-plane formats (NV12 with chroma after luma) are supported.", _fourcc_to_string(pf.pixelformat), pf.num_planes));
		return ERR_UNAVAILABLE;
	}
	if (capture_setups == 0 && stream_sps.valid && (pf.width < stream_sps.display_width || pf.height < stream_sps.display_height || pf.plane_fmt[0].sizeimage < stream_sps.display_width * stream_sps.display_height)) {
		_fail(vformat("CAPTURE format %dx%d (sizeimage %d) is smaller than the stream's %dx%d; refusing to allocate (risk of an SMMU fault). Still the placeholder?",
				pf.width, pf.height, pf.plane_fmt[0].sizeimage, stream_sps.display_width, stream_sps.display_height));
		return FAILED;
	}
	if (stream_sps.bit_depth > 8 && pf.pixelformat != V4L2_PIX_FMT_QC10C) {
		_log(vformat("NOTE: %d-bit stream decoded to %s; the driver is known to write only the low 8 bits (garbage). Q10C is the only correct 10-bit output.", stream_sps.bit_depth, _fourcc_to_string(pf.pixelformat)));
	}

	Rect2i visible;
	struct v4l2_selection selection = {};
	selection.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	selection.target = V4L2_SEL_TGT_COMPOSE;
	bool visible_ok = IOCTL_QUIET(VIDIOC_G_SELECTION, &selection, EINVAL, ENOTTY) == 0;
	if (!visible_ok) {
		selection.target = V4L2_SEL_TGT_CROP;
		visible_ok = IOCTL_QUIET(VIDIOC_G_SELECTION, &selection, EINVAL, ENOTTY) == 0;
	}
	if (visible_ok && selection.r.width > 0 && selection.r.height > 0) {
		visible = Rect2i(selection.r.left, selection.r.top, selection.r.width, selection.r.height);
	} else if (stream_sps.valid) {
		visible = Rect2i(0, 0, MIN(stream_sps.display_width, pf.width), MIN(stream_sps.display_height, pf.height));
	} else {
		visible = Rect2i(0, 0, pf.width, pf.height);
	}

	struct v4l2_control control = {};
	control.id = V4L2_CID_MIN_BUFFERS_FOR_CAPTURE;
	capture_min = IOCTL_QUIET(VIDIOC_G_CTRL, &control, EINVAL, 0) == 0 ? (uint32_t)control.value : 0;
	const uint32_t count = CLAMP(MAX((uint32_t)capture_buffer_count, capture_min + 2), 2u, MAX_CAPTURE_BUFFERS);

	struct v4l2_requestbuffers request = {};
	request.count = count;
	request.type = CAPTURE_TYPE;
	request.memory = V4L2_MEMORY_MMAP;
	if (IOCTL(VIDIOC_REQBUFS, &request) < 0 || request.count == 0) {
		_explain_enomem("REQBUFS CAPTURE");
		_fail(vformat("REQBUFS CAPTURE failed: %s.", request.count == 0 ? "0 buffers" : strerror(errno)));
		return FAILED;
	}
	capture_count = MIN(request.count, MAX_CAPTURE_BUFFERS);
	generation++;

	for (uint32_t i = 0; i < capture_count; i++) {
		struct v4l2_exportbuffer export_buffer = {};
		export_buffer.type = CAPTURE_TYPE;
		export_buffer.index = i;
		export_buffer.plane = 0;
		export_buffer.flags = O_RDWR | O_CLOEXEC;
		if (IOCTL(VIDIOC_EXPBUF, &export_buffer) < 0) {
			_fail(vformat("VIDIOC_EXPBUF on CAPTURE buffer %d failed: %s.", i, strerror(errno)));
			return FAILED;
		}
		capture[i].fd = export_buffer.fd;
		capture[i].queued = false;
		_log_verbose(vformat("  CAPTURE[%d]: dmabuf fd %d, %d bytes.", i, export_buffer.fd, (int64_t)lseek(export_buffer.fd, 0, SEEK_END)));
	}

	// NV12 in one plane: luma rows, then interleaved chroma rows. The luma plane is padded to a multiple of 32 lines.
	const uint32_t pitch = pf.plane_fmt[0].bytesperline;
	uint64_t chroma_offset = (uint64_t)pitch * ((pf.height + 31) & ~31u);
	if (chroma_offset + (uint64_t)pitch * (pf.height / 2) > pf.plane_fmt[0].sizeimage) {
		chroma_offset = (uint64_t)pitch * pf.height;
	}
	{
		MutexLock lock(mutex);
		layout.generation = generation;
		layout.fourcc = pf.pixelformat;
		layout.size = Size2i(pf.width, pf.height);
		layout.pitch = pitch;
		layout.chroma_offset = chroma_offset;
		layout.buffer_size = pf.plane_fmt[0].sizeimage;
		layout.visible = visible;
		layout.fds.clear();
		for (uint32_t i = 0; i < capture_count; i++) {
			layout.fds.push_back(capture[i].fd);
		}
		layout_valid = true;
	}

	for (uint32_t i = 0; i < capture_count; i++) {
		if (_queue_capture(i) != OK) {
			_fail("QBUF CAPTURE failed.");
			return FAILED;
		}
	}
	int type = CAPTURE_TYPE;
	if (IOCTL(VIDIOC_STREAMON, &type) < 0) {
		_explain_enomem("STREAMON CAPTURE");
		_fail(vformat("STREAMON CAPTURE failed: %s.", strerror(errno)));
		return FAILED;
	}
	capture_streaming = true;
	capture_configured = true;
	capture_setups++;
	_log(vformat("CAPTURE generation %d: %s %dx%d, pitch %d, chroma at %d, sizeimage %d, visible %dx%d+%d+%d, %d buffers (asked %d, MIN_BUFFERS_FOR_CAPTURE %d); setup took %.1f ms.",
			generation, _fourcc_to_string(pf.pixelformat), pf.width, pf.height, pitch, chroma_offset, pf.plane_fmt[0].sizeimage, visible.size.x, visible.size.y, visible.position.x, visible.position.y,
			capture_count, count, capture_min, (_now() - t_start) * 1e3));
	return OK;
}

// The consumer may still hold (and sample) buffers of this generation. Its imported images keep their dmabufs alive; the
// queue only orphans them (vb2 frees a buffer when the last dmabuf reference goes).
void V4L2Decoder::_teardown_capture(const char *p_why) {
	if (!capture_streaming && !capture_count) {
		capture_configured = false;
		return;
	}
	_log_verbose(vformat("CAPTURE teardown (%s).", p_why));
	if (capture_streaming) {
		int type = CAPTURE_TYPE;
		const int r = IOCTL(VIDIOC_STREAMOFF, &type);
		_log_verbose(vformat("  STREAMOFF CAPTURE: %s.", r ? strerror(errno) : "ok"));
		capture_streaming = false;
	}
	{
		MutexLock lock(mutex);
		layout_valid = false;
		layout.fds.clear();
	}
	for (uint32_t i = 0; i < capture_count; i++) {
		if (capture[i].fd >= 0) {
			::close(capture[i].fd);
		}
		capture[i] = CaptureBuffer();
	}
	capture_in_driver = 0;
	if (capture_count) {
		struct v4l2_requestbuffers request = {};
		request.count = 0;
		request.type = CAPTURE_TYPE;
		request.memory = V4L2_MEMORY_MMAP;
		const int r = IOCTL(VIDIOC_REQBUFS, &request);
		_log_verbose(vformat("  REQBUFS(CAPTURE, 0) for %d buffer(s): %s.", capture_count, r ? strerror(errno) : "ok"));
		capture_count = 0;
	}
	capture_configured = false;
}

void V4L2Decoder::_handle_events() {
	for (;;) {
		struct v4l2_event event = {};
		if (IOCTL_QUIET(VIDIOC_DQEVENT, &event, ENOENT, 0) < 0) {
			return;
		}
		last_progress = _now();
		if (event.type == V4L2_EVENT_SOURCE_CHANGE) {
			wstats.source_changes++;
			_log(vformat("SOURCE_CHANGE #%d (changes 0x%x).", wstats.source_changes, event.u.src_change.changes));
			if (!capture_configured) {
				need_setup = true;
			} else {
				pending_reconfig = true;
				t_reconfig_start = last_progress;
				_set_state(STATE_RECONFIGURING);
				_log("resolution change: draining CAPTURE until the LAST buffer before reallocating (input paused).");
			}
		} else if (event.type == V4L2_EVENT_EOS) {
			eos_events++;
			t_eos = last_progress;
			_log_verbose("EOS event.");
		}
		if (event.pending == 0) {
			return;
		}
	}
}

int V4L2Decoder::_dequeue_capture() {
	int count = 0;
	LocalVector<Frame> frames;
	while (!got_last) {
		struct v4l2_plane planes[VIDEO_MAX_PLANES] = {};
		struct v4l2_buffer buffer = {};
		buffer.type = CAPTURE_TYPE;
		buffer.memory = V4L2_MEMORY_MMAP;
		buffer.m.planes = planes;
		buffer.length = VIDEO_MAX_PLANES;
		if (IOCTL_QUIET(VIDIOC_DQBUF, &buffer, EAGAIN, EPIPE) < 0) {
			if (errno == EPIPE) {
				_log_verbose("DQBUF CAPTURE: EPIPE (the LAST buffer was already dequeued).");
				got_last = true;
				t_last_seen = _now();
			}
			break;
		}
		const double now = _now();
		count++;
		if (buffer.index >= capture_count) {
			continue;
		}
		if (capture[buffer.index].queued) {
			capture[buffer.index].queued = false;
			capture_in_driver--;
		}
		const bool last = buffer.flags & V4L2_BUF_FLAG_LAST;
		const uint64_t usec = (uint64_t)buffer.timestamp.tv_sec * 1000000u + (uint64_t)buffer.timestamp.tv_usec;
		const uint64_t frame_seq = _seq_for_timestamp(usec);
		bool keep = false;
		if (buffer.flags & V4L2_BUF_FLAG_ERROR) {
			wstats.frames_error++;
			Epoch *epoch = _find_epoch(frame_seq);
			if (epoch) {
				epoch->frames++; // It still takes a display slot.
			}
		} else if (planes[0].bytesused > 0) {
			wstats.frames_decoded++;
			Epoch *epoch = _find_epoch(frame_seq);
			Frame frame;
			frame.generation = generation;
			frame.buffer = buffer.index;
			if (epoch) {
				frame.serial = epoch->serial;
				const uint32_t slot = frame_seq % QUEUE_RING;
				if (source.is_valid() && queue_seq[slot] == frame_seq) {
					// The driver copies the OUTPUT timestamp to the picture decoded from it, so this is that access unit's PTS.
					frame.time = queue_pts[slot];
				} else {
					frame.time = (double)(epoch->base_index + epoch->frames) / fps;
				}
				epoch->frames++;
			}
			frames.push_back(frame);
			keep = true;
			if (queue_seq[frame_seq % QUEUE_RING] == frame_seq && frame_seq < seq) {
				const double latency = now - queue_time[frame_seq % QUEUE_RING];
				wstats.latency_sum += latency;
				wstats.latency_max = MAX(wstats.latency_max, latency);
				wstats.latency_count++;
			}
		}
		if (last) {
			_log_verbose(vformat("CAPTURE buffer with V4L2_BUF_FLAG_LAST (bytesused %d).", planes[0].bytesused));
			got_last = true;
			t_last_seen = now;
			break; // An empty LAST buffer stays dequeued.
		}
		if (!keep && _queue_capture(buffer.index) != OK) {
			return -1;
		}
	}
	if (!frames.is_empty()) {
		MutexLock lock(mutex);
		for (const Frame &frame : frames) {
			frames_ready.push_back(frame);
		}
	}
	return count;
}

void V4L2Decoder::_send_stop() {
	struct v4l2_decoder_cmd command = {};
	command.cmd = V4L2_DEC_CMD_STOP;
	if (IOCTL(VIDIOC_DECODER_CMD, &command) == 0) {
		stop_sent = true;
		_log_verbose("V4L2_DEC_CMD_STOP sent; draining.");
	} else {
		stop_failed = true;
		_log("V4L2_DEC_CMD_STOP failed; will finish when the decoder goes idle.");
	}
}

/* Worker loop */

void V4L2Decoder::_process_inbox() {
	LocalVector<uint64_t> returned;
	uint32_t want_serial;
	{
		MutexLock lock(mutex);
		returned = buffers_returned;
		buffers_returned.clear();
		want_serial = requested_serial;
		worker_clock = clock;
	}
	for (uint64_t r : returned) {
		const uint32_t buffer_generation = (uint32_t)(r >> 32);
		const uint32_t index = (uint32_t)(r & 0xffffffffu);
		// Buffers of an earlier generation were orphaned at teardown; after LAST the queue is drained.
		if (buffer_generation == generation && capture_streaming && !got_last && index < capture_count && !capture[index].queued) {
			_queue_capture(index);
		}
	}
	if (source.is_valid()) {
		// A seek on the source: its access units carry the new serial. If the decoder was drained to the end, the session
		// has to start again; otherwise it just continues with the keyframe (and its parameter sets) the source delivers.
		if (source->get_serial() != serial && !pending_reconfig && !need_setup) {
			if (stop_sent || stop_failed || ended) {
				if (_restart_session_source("seek after the end") != OK) {
					_fail("Restarting the session for the seek failed.");
				}
			} else {
				input_done = false;
			}
		}
	} else if (want_serial != serial && !pending_reconfig && !need_setup) {
		_seek_to_start(want_serial);
	}
}

void V4L2Decoder::_seek_to_start(uint32_t p_serial) {
	serial = p_serial;
	if (stop_sent || ended) {
		// The decoder was drained to its end: start a new session.
		AnnexBSplitter::AccessUnit first;
		first.sps_width = stream_sps.width;
		first.sps_height = stream_sps.height;
		if (_restart_session(first, 0, "seek to start after the end") != OK) {
			_fail("Restarting the session for the seek failed.");
		}
		return;
	}
	// No STREAMOFF: the first access unit carries the SPS and an IDR picture, so the decoder just continues with it. What is
	// still queued decodes under the old serial, and the consumer drops it.
	splitter.seek(0);
	media_index = 0;
	input_done = false;
	_begin_epoch(serial, 0);
	_log_verbose(vformat("seek to start (serial %d).", serial));
}

Error V4L2Decoder::_restart_after_reconfig() {
	// Input was paused at SOURCE_CHANGE, so the access unit with the new SPS is among the last few fed.
	const RecentAU *found = nullptr;
	const RecentAU *any_sps = nullptr;
	for (uint64_t k = 1; k <= RECENT_AUS && k <= recent_count; k++) {
		const RecentAU &r = recent[(recent_count - k) % RECENT_AUS];
		if (!r.au.sps_width) {
			continue;
		}
		if (!any_sps) {
			any_sps = &r;
		}
		if ((int)r.au.sps_width != output_size.width || (int)r.au.sps_height != output_size.height) {
			found = &r;
			break;
		}
	}
	if (!found) {
		found = any_sps;
	}
	if (!found) {
		_fail("Session restart: no SPS among the last access units fed.");
		return FAILED;
	}
	const RecentAU resume = *found;
	wstats.reconfig_restart++;
	if (source.is_valid()) {
		// The access units fed since the new SPS are gone from the source's queue: read them again from its keyframe. This
		// is a seek on the source (audio included), back by the few access units in flight.
		source->seek(resume.pts);
		return _restart_session_source("resolution change");
	}
	return _restart_session(resume.au, resume.media_index, "resolution change");
}

void V4L2Decoder::_publish_stats() {
	wstats.output_in_driver = output_in_driver;
	wstats.capture_in_driver = capture_in_driver;
	wstats.capture_count = capture_count;
	wstats.queued_rate = _queued_rate();
	wstats.media_index = media_index;
	wstats.input_time = input_time;
	{
		struct timespec ts;
		clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
		wstats.thread_cpu_seconds = (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
	}
	MutexLock lock(mutex);
	stats = wstats;
}

void V4L2Decoder::_run() {
	const double timeout = timeout_ms / 1000.0;
	last_progress = _now();
	while (!exit_requested.is_set()) {
		double now = _now();
		_process_inbox();
		if (get_state() == STATE_ERROR) {
			break;
		}
		if (need_setup) {
			need_setup = false;
			_rate_reset(); // A no-op unless >= 30 access units were fed before SOURCE_CHANGE.
			if (_setup_capture() != OK) {
				break;
			}
			_set_state(STATE_DECODING);
			last_progress = _now();
		}
		if (got_last && !stop_sent && !pending_reconfig && now - t_last_seen > timeout / 4) {
			_log("LAST buffer without SOURCE_CHANGE or STOP; treating it as a resolution change.");
			pending_reconfig = true;
			t_reconfig_start = now;
			_set_state(STATE_RECONFIGURING);
		}
		if (pending_reconfig && (got_last || now - t_reconfig_start > timeout / 2)) {
			if (!got_last) {
				_log(vformat("no LAST buffer %.1f s after SOURCE_CHANGE; reallocating anyway.", timeout / 2));
			}
			_rate_reset();
			_teardown_capture("resolution change");
			pending_reconfig = false;
			got_last = false;
			if (_setup_capture() == OK) {
				wstats.reconfig_in_place++;
				_log(vformat("reconfig path: IN-PLACE, resolution change handled in %.1f ms.", (_now() - t_reconfig_start) * 1e3));
				_set_state(STATE_DECODING);
			} else {
				{
					// The in-place setup failed (most likely the admission check): reopen the device instead.
					MutexLock lock(mutex);
					state = STATE_RECONFIGURING;
					last_error = String();
				}
				if (_restart_after_reconfig() != OK) {
					break;
				}
			}
			last_progress = _now();
		}

		int progress = 0;
		const int queued = _queue_input();
		if (queued < 0) {
			_fail("Queueing an access unit failed.");
			break;
		}
		progress += queued;
		if (input_done && capture_streaming && !pending_reconfig && !stop_sent && !stop_failed) {
			_send_stop();
		}
		if (!ended && ((stop_sent && got_last) || (stop_sent && eos_events && now - t_eos > 1.0) || (stop_failed && output_in_driver == 0 && now - last_progress > 1.0))) {
			ended = true;
			_set_state(STATE_ENDED);
			_log(vformat("end of the stream drained: %d frames decoded.", wstats.frames_decoded));
		}

		// Poll the device only while it has work: with nothing queued on either side, the m2m poll returns POLLERR at once.
		const bool device_busy = output_in_driver > 0 || (capture_streaming && !got_last && capture_in_driver > 0) || (output_streaming && !capture_configured);
		struct pollfd fds[2] = {};
		fds[0].fd = wake_fd;
		fds[0].events = POLLIN;
		nfds_t fd_count = 1;
		if (device_busy && !ended) {
			fds[1].fd = fd;
			fds[1].events = POLLPRI;
			if (output_in_driver) {
				fds[1].events |= POLLOUT;
			}
			if (capture_streaming && !got_last) {
				fds[1].events |= POLLIN;
			}
			fd_count = 2;
		}
		const int poll_result = poll(fds, fd_count, POLL_SLICE_MS);
		if (poll_result < 0 && errno != EINTR) {
			_fail(vformat("poll failed: %s.", strerror(errno)));
			break;
		}
		if (fds[0].revents & POLLIN) {
			uint64_t value;
			ssize_t r = read(wake_fd, &value, sizeof(value));
			(void)r;
		}
		if (fd_count == 2 || !capture_configured) {
			// Events are also polled for directly until CAPTURE exists, in case POLLPRI doesn't come before it streams.
			if ((fds[1].revents & POLLPRI) || !capture_configured || poll_result == 0) {
				const uint64_t before = wstats.source_changes + eos_events;
				_handle_events();
				progress += (int)(wstats.source_changes + eos_events - before);
			}
			progress += _dequeue_output();
			if (capture_streaming) {
				const int dequeued = _dequeue_capture();
				if (dequeued < 0) {
					_fail("Re-queueing a CAPTURE buffer failed.");
					break;
				}
				progress += dequeued;
			}
		}
		now = _now();
		if (progress) {
			last_progress = now;
		} else if (fd_count == 2 && (fds[1].revents & POLLERR)) {
			OS::get_singleton()->delay_usec(1000); // Something we couldn't consume.
		}
		_publish_stats();

		// Waiting on the decoder: it holds input and has somewhere to put the output, or a drain is in progress.
		const bool waiting = (output_in_driver > 0 && (capture_in_driver > 0 || !capture_configured)) || (stop_sent && !ended) || pending_reconfig;
		if (waiting && now - last_progress > timeout) {
			_fail(vformat("HANG: the decoder made no progress for %.1f s (%d OUTPUT and %d CAPTURE buffers in the driver, stop sent %s, reconfig %s). Firmware hang? Check 'sudo dmesg | grep -iE \"iris|0x5000003\"'. Closing the session; don't SIGKILL a process stuck in a decoder ioctl.",
					now - last_progress, output_in_driver, capture_in_driver, stop_sent ? "yes" : "no", pending_reconfig ? "yes" : "no"));
			break;
		}
	}
}

void V4L2Decoder::_thread_func(void *p_userdata) {
	V4L2Decoder *self = static_cast<V4L2Decoder *>(p_userdata);
	if (self->_start_session() == OK) {
		self->_run();
	}
	self->_close_device("session end");
	self->_publish_stats();
}

/* Public API */

Error V4L2Decoder::open(const String &p_path, const String &p_codec, double p_fps) {
	ERR_FAIL_COND_V_MSG(is_open(), ERR_ALREADY_IN_USE, "V4L2Decoder: already open. Call close() first.");
	ERR_FAIL_COND_V_MSG(!_parse_codec(p_codec, codec), ERR_INVALID_PARAMETER, vformat("V4L2Decoder: unknown codec '%s' (use h264 or hevc).", p_codec));
	ERR_FAIL_COND_V_MSG(p_fps <= 0.0 || p_fps > 1000.0, ERR_INVALID_PARAMETER, "V4L2Decoder: fps must be in (0, 1000].");
	fps = p_fps;
	source.unref();

	{
		MutexLock lock(mutex);
		state = STATE_CLOSED;
		last_error = String();
	}
	Error err = _map_file(p_path, file_data, file_size);
	if (err != OK) {
		MutexLock lock(mutex);
		state = STATE_ERROR;
		last_error = vformat("Can't read %s.", p_path);
		return err;
	}
	file_path = p_path;

	// The coded size for S_FMT(OUTPUT) comes from the first SPS, which is normally in the first access unit.
	splitter.init(file_data, file_size, codec);
	AnnexBSplitter::AccessUnit au;
	for (int i = 0; i < 300 && !splitter.get_first_sps().valid && splitter.next(au); i++) {
	}
	stream_sps = splitter.get_first_sps();
	splitter.seek(0);
	if (!stream_sps.valid) {
		_unmap_file();
		_fail(vformat("No %s SPS in the first 300 access units of %s. Is it a raw Annex-B elementary stream? (ffmpeg -i in.mp4 -c:v copy -bsf:v %s_mp4toannexb out.%s)", codec == AnnexBSplitter::CODEC_H264 ? "H.264" : "HEVC", p_path,
				codec == AnnexBSplitter::CODEC_H264 ? "h264" : "hevc", codec == AnnexBSplitter::CODEC_H264 ? "h264" : "h265"));
		return ERR_FILE_UNRECOGNIZED;
	}
	_log(vformat("%s: %d bytes, %s %dx%d (display %dx%d), %d-bit, %.3f fps.", p_path, file_size, codec == AnnexBSplitter::CODEC_H264 ? "H.264" : "HEVC", stream_sps.width, stream_sps.height,
			stream_sps.display_width, stream_sps.display_height, stream_sps.bit_depth, fps));

	err = _start_worker();
	if (err != OK) {
		_unmap_file();
	}
	return err;
}

Error V4L2Decoder::open_source(const Ref<MediaSource> &p_source) {
	ERR_FAIL_COND_V_MSG(is_open(), ERR_ALREADY_IN_USE, "V4L2Decoder: already open. Call close() first.");
	ERR_FAIL_COND_V_MSG(p_source.is_null() || !p_source->is_open(), ERR_INVALID_PARAMETER, "V4L2Decoder: the MediaSource isn't open.");
	{
		MutexLock lock(mutex);
		state = STATE_CLOSED;
		last_error = String();
	}
	const MediaSource::VideoInfo &info = p_source->get_video_stream_info();
	const String path = p_source->get_path();
	if (!info.valid) {
		_fail(vformat("%s has no video stream.", path));
		return ERR_INVALID_DATA;
	}
	if (info.codec != MediaSource::VIDEO_CODEC_H264 && info.codec != MediaSource::VIDEO_CODEC_HEVC) {
		_fail(vformat("%s: %s video has no hardware decode path here (H.264 and HEVC only: VP9 crashes the decoder firmware, AV1 isn't exposed).", path, info.codec_name));
		return ERR_UNAVAILABLE;
	}
	if (!info.sps.valid) {
		_fail(vformat("%s: no SPS found for the %s stream, so the coded size is unknown.", path, info.codec_name));
		return ERR_FILE_UNRECOGNIZED;
	}
	codec = info.codec == MediaSource::VIDEO_CODEC_H264 ? AnnexBSplitter::CODEC_H264 : AnnexBSplitter::CODEC_HEVC;
	fps = info.fps >= 1.0 && info.fps <= 240.0 ? info.fps : 30.0;
	stream_sps = info.sps;
	file_path = path;
	source = p_source;
	if (loop) {
		_log("loop is ignored with a MediaSource; seek to 0 at the end instead.");
	}
	_log(vformat("%s: %s %dx%d (display %dx%d), %d-bit, %.3f fps, from a MediaSource (frame times from PTS).", path, codec == AnnexBSplitter::CODEC_H264 ? "H.264" : "HEVC", stream_sps.width, stream_sps.height,
			stream_sps.display_width, stream_sps.display_height, stream_sps.bit_depth, fps));
	const Error err = _start_worker();
	if (err != OK) {
		source->set_video_wake_fd(-1);
		source.unref();
	}
	return err;
}

// Resets the worker state, opens the device (so a missing decoder fails here, synchronously) and starts the worker.
Error V4L2Decoder::_start_worker() {
	codec_fourcc = codec == AnnexBSplitter::CODEC_H264 ? V4L2_PIX_FMT_H264 : V4L2_PIX_FMT_HEVC;
	const CharString capture_name = capture_format.utf8();
	capture_fourcc = capture_name.length() == 4 ? v4l2_fourcc(capture_name[0], capture_name[1], capture_name[2], capture_name[3]) : 0;

	// Reset the worker state.
	wstats = Stats();
	stats = Stats();
	output_size = Size2i(stream_sps.width, stream_sps.height);
	output_count = output_in_driver = 0;
	capture_count = capture_in_driver = capture_setups = 0;
	generation = 0;
	serial = source.is_valid() ? source->get_serial() : 0;
	requested_serial = serial;
	clock = 0.0;
	worker_clock = 0.0;
	output_streaming = capture_streaming = capture_configured = false;
	need_setup = pending_reconfig = got_last = input_done = stop_sent = stop_failed = ended = false;
	seq = seq_at_open = media_index = 0;
	eos_events = 0;
	recent_count = 0;
	ioctl_error_count = 0;
	epochs.clear();
	_begin_epoch(serial, 0);
	have_held = false;
	eos_pending = false;
	input_time = 0.0;
	last_sps_size = Size2i(stream_sps.width, stream_sps.height);
	frames_ready.clear();
	buffers_returned.clear();
	layout_valid = false;

	Error err = _open_device();
	if (err != OK) {
		return err;
	}
	wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (wake_fd < 0) {
		_close_device("eventfd failed");
		_fail(vformat("eventfd failed: %s.", strerror(errno)));
		return ERR_CANT_CREATE;
	}
	if (source.is_valid()) {
		source->set_video_wake_fd(wake_fd);
	}
	_set_state(STATE_STARTING);
	exit_requested.clear();
	thread.start(&V4L2Decoder::_thread_func, this);
	return OK;
}

void V4L2Decoder::close() {
	if (thread.is_started()) {
		// The worker finishes its current poll slice, then STREAMOFFs and closes. Ioctls are never interrupted.
		exit_requested.set();
		_wake();
		thread.wait_to_finish();
	}
	_close_device("close");
	if (source.is_valid()) {
		source->set_video_wake_fd(-1); // Before the eventfd is closed: the source writes to it under its lock.
		if (have_held) {
			source->release_video_packet(held);
			have_held = false;
		}
		source.unref();
	}
	if (wake_fd >= 0) {
		::close(wake_fd);
		wake_fd = -1;
	}
	_unmap_file();
	MutexLock lock(mutex);
	frames_ready.clear();
	buffers_returned.clear();
	layout_valid = false;
	layout.fds.clear();
	state = STATE_CLOSED;
}

void V4L2Decoder::set_clock(double p_seconds) {
	{
		MutexLock lock(mutex);
		if (p_seconds == clock) {
			return;
		}
		clock = p_seconds;
	}
	_wake();
}

double V4L2Decoder::get_clock() const {
	MutexLock lock(mutex);
	return clock;
}

int V4L2Decoder::seek_to_start() {
	if (source.is_valid()) {
		return source->seek(0.0);
	}
	int new_serial;
	{
		MutexLock lock(mutex);
		requested_serial++;
		new_serial = (int)requested_serial;
		clock = 0.0;
		// Frames not yet taken are stale now: hand their buffers straight back.
		for (const Frame &frame : frames_ready) {
			buffers_returned.push_back(((uint64_t)frame.generation << 32) | frame.buffer);
		}
		frames_ready.clear();
	}
	_wake();
	return new_serial;
}

bool V4L2Decoder::pop_frame(Frame &r_frame) {
	MutexLock lock(mutex);
	if (frames_ready.is_empty()) {
		return false;
	}
	r_frame = frames_ready[0];
	frames_ready.remove_at(0);
	return true;
}

void V4L2Decoder::return_buffer(uint32_t p_generation, uint32_t p_buffer) {
	{
		MutexLock lock(mutex);
		buffers_returned.push_back(((uint64_t)p_generation << 32) | p_buffer);
	}
	_wake();
}

Error V4L2Decoder::get_capture_layout(uint32_t p_generation, CaptureLayout &r_layout) const {
	MutexLock lock(mutex);
	ERR_FAIL_COND_V_MSG(!layout_valid || layout.generation != p_generation, ERR_UNAVAILABLE, vformat("V4L2Decoder: CAPTURE generation %d is gone.", p_generation));
	r_layout.generation = layout.generation;
	r_layout.fourcc = layout.fourcc;
	r_layout.size = layout.size;
	r_layout.pitch = layout.pitch;
	r_layout.chroma_offset = layout.chroma_offset;
	r_layout.buffer_size = layout.buffer_size;
	r_layout.visible = layout.visible;
	r_layout.fds.clear();
	for (int buffer_fd : layout.fds) {
		const int duplicate = fcntl(buffer_fd, F_DUPFD_CLOEXEC, 0);
		if (duplicate < 0) {
			for (int d : r_layout.fds) {
				::close(d);
			}
			r_layout.fds.clear();
			ERR_FAIL_V_MSG(ERR_CANT_CREATE, vformat("V4L2Decoder: duplicating a CAPTURE dmabuf fd failed: %s.", strerror(errno)));
		}
		r_layout.fds.push_back(duplicate);
	}
	return OK;
}

V4L2Decoder::State V4L2Decoder::get_state() const {
	MutexLock lock(mutex);
	return state;
}

String V4L2Decoder::get_last_error() const {
	MutexLock lock(mutex);
	return last_error;
}

Dictionary V4L2Decoder::get_stats() const {
	static const char *state_names[] = { "closed", "starting", "decoding", "reconfiguring", "ended", "error" };
	MutexLock lock(mutex);
	Dictionary d;
	d["state"] = (int)state;
	d["state_name"] = state_names[state];
	d["frames_decoded"] = stats.frames_decoded;
	d["frames_error"] = stats.frames_error;
	d["frames_waiting"] = frames_ready.size();
	d["aus_queued"] = stats.aus_queued;
	d["aus_skipped"] = stats.aus_skipped;
	d["output_errors"] = stats.output_errors;
	d["source_changes"] = stats.source_changes;
	d["loops"] = stats.loops;
	d["eos_inserted"] = stats.eos_inserted;
	d["reconfig_in_place"] = stats.reconfig_in_place;
	d["reconfig_restart"] = stats.reconfig_restart;
	d["restarts"] = stats.restarts;
	d["output_in_driver"] = stats.output_in_driver;
	d["capture_in_driver"] = stats.capture_in_driver;
	d["capture_buffers"] = stats.capture_count;
	d["queued_rate"] = stats.queued_rate;
	d["latency_avg_ms"] = stats.latency_count ? stats.latency_sum / (double)stats.latency_count * 1e3 : 0.0;
	d["latency_max_ms"] = stats.latency_max * 1e3;
	d["thread_cpu_seconds"] = stats.thread_cpu_seconds;
	d["input_position"] = source.is_valid() ? stats.input_time : (double)stats.media_index / fps;
	d["last_error"] = last_error;
	return d;
}

Dictionary V4L2Decoder::get_stream_info() const {
	Dictionary d;
	d["path"] = file_path;
	d["media_source"] = source.is_valid();
	d["codec"] = codec == AnnexBSplitter::CODEC_H264 ? "h264" : "hevc";
	d["fps"] = fps;
	d["width"] = stream_sps.width;
	d["height"] = stream_sps.height;
	d["display_width"] = stream_sps.display_width;
	d["display_height"] = stream_sps.display_height;
	d["bit_depth"] = stream_sps.bit_depth;
	return d;
}

Dictionary V4L2Decoder::probe_file(const String &p_path, const String &p_codec, int p_max_access_units, int p_list_count) {
	Dictionary d;
	AnnexBSplitter::Codec probe_codec;
	ERR_FAIL_COND_V_MSG(!_parse_codec(p_codec, probe_codec), d, vformat("V4L2Decoder: unknown codec '%s' (use h264 or hevc).", p_codec));
	const uint8_t *data = nullptr;
	uint64_t size = 0;
	if (_map_file(p_path, data, size) != OK) {
		return d;
	}
	const double t_start = _now();
	AnnexBSplitter splitter;
	splitter.init(data, size, probe_codec);
	AnnexBSplitter::AccessUnit au;
	uint64_t count = 0;
	uint64_t total = 0;
	uint64_t largest = 0;
	uint32_t last_width = 0;
	uint32_t last_height = 0;
	Array first;
	Array size_changes;
	while ((p_max_access_units <= 0 || count < (uint64_t)p_max_access_units) && splitter.next(au)) {
		if (count < (uint64_t)MAX(p_list_count, 0)) {
			first.push_back(Vector2i((int)au.offset, (int)au.size));
		}
		if (au.sps_width && (au.sps_width != last_width || au.sps_height != last_height)) {
			size_changes.push_back(Vector3i((int)count, (int)au.sps_width, (int)au.sps_height));
			last_width = au.sps_width;
			last_height = au.sps_height;
		}
		total += au.size;
		largest = MAX(largest, au.size);
		count++;
	}
	const AnnexBSplitter::SPSInfo &sps = splitter.get_first_sps();
	d["file_size"] = size;
	d["access_units"] = count;
	d["max_au_bytes"] = largest;
	d["avg_au_bytes"] = count ? (double)total / (double)count : 0.0;
	d["width"] = sps.width;
	d["height"] = sps.height;
	d["display_width"] = sps.display_width;
	d["display_height"] = sps.display_height;
	d["bit_depth"] = sps.bit_depth;
	d["chroma_format"] = sps.chroma_format;
	d["sps_size_changes"] = size_changes;
	d["first_access_units"] = first;
	d["parse_seconds"] = _now() - t_start;
	munmap((void *)data, size);
	return d;
}

/* Settings */

#define SETTING_GUARD() ERR_FAIL_COND_MSG(is_open(), "V4L2Decoder: change the settings before open().")

void V4L2Decoder::set_device_path(const String &p_path) {
	SETTING_GUARD();
	device_path = p_path;
}

void V4L2Decoder::set_capture_format(const String &p_fourcc) {
	SETTING_GUARD();
	ERR_FAIL_COND_MSG(!p_fourcc.is_empty() && p_fourcc.length() != 4, "V4L2Decoder: the CAPTURE format is a fourcc (e.g. NV12), or empty for the driver's choice.");
	capture_format = p_fourcc;
}

void V4L2Decoder::set_output_buffer_count(int p_count) {
	SETTING_GUARD();
	output_buffer_count = CLAMP(p_count, 2, (int)MAX_OUTPUT_BUFFERS);
}

void V4L2Decoder::set_capture_buffer_count(int p_count) {
	SETTING_GUARD();
	capture_buffer_count = CLAMP(p_count, 2, (int)MAX_CAPTURE_BUFFERS);
}

void V4L2Decoder::set_output_buffer_size(int p_size) {
	SETTING_GUARD();
	output_buffer_size = MAX(p_size, 0);
}

void V4L2Decoder::set_lead_time(double p_seconds) {
	lead_time = MAX(p_seconds, 0.0);
}

void V4L2Decoder::set_reconfig_rate(double p_fps) {
	SETTING_GUARD();
	reconfig_rate = MAX(p_fps, 0.0);
}

void V4L2Decoder::set_timeout_ms(int p_ms) {
	SETTING_GUARD();
	timeout_ms = MAX(p_ms, 100);
}

void V4L2Decoder::set_loop(bool p_loop) {
	SETTING_GUARD();
	loop = p_loop;
}

void V4L2Decoder::set_verbose(bool p_verbose) {
	verbose = p_verbose;
}

void V4L2Decoder::set_seek_eos(SeekEOS p_mode) {
	seek_eos = p_mode; // Read by the worker at the next seek.
}

V4L2Decoder::~V4L2Decoder() {
	close();
}

void V4L2Decoder::_bind_methods() {
	ClassDB::bind_static_method("V4L2Decoder", D_METHOD("probe_file", "path", "codec", "max_access_units", "list_count"), &V4L2Decoder::probe_file, DEFVAL(0), DEFVAL(6));

	ClassDB::bind_method(D_METHOD("open", "path", "codec", "fps"), &V4L2Decoder::open);
	ClassDB::bind_method(D_METHOD("open_source", "source"), &V4L2Decoder::open_source);
	ClassDB::bind_method(D_METHOD("get_media_source"), &V4L2Decoder::get_media_source);
	ClassDB::bind_method(D_METHOD("close"), &V4L2Decoder::close);
	ClassDB::bind_method(D_METHOD("is_open"), &V4L2Decoder::is_open);
	ClassDB::bind_method(D_METHOD("set_clock", "seconds"), &V4L2Decoder::set_clock);
	ClassDB::bind_method(D_METHOD("get_clock"), &V4L2Decoder::get_clock);
	ClassDB::bind_method(D_METHOD("seek_to_start"), &V4L2Decoder::seek_to_start);
	ClassDB::bind_method(D_METHOD("get_state"), &V4L2Decoder::get_state);
	ClassDB::bind_method(D_METHOD("get_last_error"), &V4L2Decoder::get_last_error);
	ClassDB::bind_method(D_METHOD("get_stats"), &V4L2Decoder::get_stats);
	ClassDB::bind_method(D_METHOD("get_stream_info"), &V4L2Decoder::get_stream_info);

	ClassDB::bind_method(D_METHOD("set_device_path", "path"), &V4L2Decoder::set_device_path);
	ClassDB::bind_method(D_METHOD("get_device_path"), &V4L2Decoder::get_device_path);
	ClassDB::bind_method(D_METHOD("set_capture_format", "fourcc"), &V4L2Decoder::set_capture_format);
	ClassDB::bind_method(D_METHOD("get_capture_format"), &V4L2Decoder::get_capture_format);
	ClassDB::bind_method(D_METHOD("set_output_buffer_count", "count"), &V4L2Decoder::set_output_buffer_count);
	ClassDB::bind_method(D_METHOD("get_output_buffer_count"), &V4L2Decoder::get_output_buffer_count);
	ClassDB::bind_method(D_METHOD("set_capture_buffer_count", "count"), &V4L2Decoder::set_capture_buffer_count);
	ClassDB::bind_method(D_METHOD("get_capture_buffer_count"), &V4L2Decoder::get_capture_buffer_count);
	ClassDB::bind_method(D_METHOD("set_output_buffer_size", "bytes"), &V4L2Decoder::set_output_buffer_size);
	ClassDB::bind_method(D_METHOD("get_output_buffer_size"), &V4L2Decoder::get_output_buffer_size);
	ClassDB::bind_method(D_METHOD("set_lead_time", "seconds"), &V4L2Decoder::set_lead_time);
	ClassDB::bind_method(D_METHOD("get_lead_time"), &V4L2Decoder::get_lead_time);
	ClassDB::bind_method(D_METHOD("set_reconfig_rate", "fps"), &V4L2Decoder::set_reconfig_rate);
	ClassDB::bind_method(D_METHOD("get_reconfig_rate"), &V4L2Decoder::get_reconfig_rate);
	ClassDB::bind_method(D_METHOD("set_timeout_ms", "ms"), &V4L2Decoder::set_timeout_ms);
	ClassDB::bind_method(D_METHOD("get_timeout_ms"), &V4L2Decoder::get_timeout_ms);
	ClassDB::bind_method(D_METHOD("set_loop", "loop"), &V4L2Decoder::set_loop);
	ClassDB::bind_method(D_METHOD("get_loop"), &V4L2Decoder::get_loop);
	ClassDB::bind_method(D_METHOD("set_verbose", "verbose"), &V4L2Decoder::set_verbose);
	ClassDB::bind_method(D_METHOD("is_verbose"), &V4L2Decoder::is_verbose);
	ClassDB::bind_method(D_METHOD("set_seek_eos", "mode"), &V4L2Decoder::set_seek_eos);
	ClassDB::bind_method(D_METHOD("get_seek_eos"), &V4L2Decoder::get_seek_eos);

	ADD_PROPERTY(PropertyInfo(Variant::STRING, "device_path"), "set_device_path", "get_device_path");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "capture_format"), "set_capture_format", "get_capture_format");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "output_buffer_count"), "set_output_buffer_count", "get_output_buffer_count");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "capture_buffer_count"), "set_capture_buffer_count", "get_capture_buffer_count");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "output_buffer_size"), "set_output_buffer_size", "get_output_buffer_size");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "lead_time"), "set_lead_time", "get_lead_time");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "reconfig_rate"), "set_reconfig_rate", "get_reconfig_rate");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "timeout_ms"), "set_timeout_ms", "get_timeout_ms");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "loop"), "set_loop", "get_loop");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "verbose"), "set_verbose", "is_verbose");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "seek_eos", PROPERTY_HINT_ENUM, "Off,Prepend,Separate"), "set_seek_eos", "get_seek_eos");

	BIND_ENUM_CONSTANT(STATE_CLOSED);
	BIND_ENUM_CONSTANT(STATE_STARTING);
	BIND_ENUM_CONSTANT(STATE_DECODING);
	BIND_ENUM_CONSTANT(STATE_RECONFIGURING);
	BIND_ENUM_CONSTANT(STATE_ENDED);
	BIND_ENUM_CONSTANT(STATE_ERROR);

	BIND_ENUM_CONSTANT(SEEK_EOS_OFF);
	BIND_ENUM_CONSTANT(SEEK_EOS_PREPEND);
	BIND_ENUM_CONSTANT(SEEK_EOS_SEPARATE);
}
