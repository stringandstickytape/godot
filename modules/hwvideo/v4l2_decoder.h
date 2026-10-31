/**************************************************************************/
/*  v4l2_decoder.h                                                        */
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

#include "annexb_splitter.h"

#include "core/object/ref_counted.h"
#include "core/os/mutex.h"
#include "core/os/thread.h"
#include "core/templates/local_vector.h"
#include "core/templates/safe_refcount.h"
#include "core/variant/binder_common.h"
#include "core/variant/variant.h"

// A V4L2 stateful memory-to-memory decoder session (the Steam Frame's Qualcomm iris decoder, /dev/video-dec0), fed from a
// raw Annex-B H.264/HEVC file, on a worker thread. A port of the session logic of spikes/01-decode/v4l2dec.c.
//
// One long-lived session: OUTPUT (bitstream) buffers are mmapped and fed one access unit each, paced by a clock that the
// consumer sets (set_clock()), so the queue rate the driver measures stays near the playback rate. CAPTURE buffers are
// allocated after SOURCE_CHANGE and exported once each (VIDIOC_EXPBUF); the fds are kept until the CAPTURE queue is torn
// down. A decoded buffer stays dequeued, owned by the consumer, until it hands it back with return_buffer().
class V4L2Decoder : public RefCounted {
	GDCLASS(V4L2Decoder, RefCounted);

public:
	enum State {
		STATE_CLOSED,
		STATE_STARTING, // Device open and fed, waiting for the first SOURCE_CHANGE.
		STATE_DECODING,
		STATE_RECONFIGURING, // Resolution change: draining, reallocating CAPTURE, or restarting the session.
		STATE_ENDED, // End of the stream drained (loop off). seek_to_start() starts again.
		STATE_ERROR,
	};

	// A decoded picture. Its CAPTURE buffer is the consumer's until return_buffer().
	struct Frame {
		uint32_t generation = 0; // The CAPTURE setup the buffer belongs to. Buffer indices are only valid within one.
		uint32_t buffer = 0;
		uint32_t serial = 0; // The seek_to_start() count when the frame's access unit was fed.
		double time = 0.0; // Presentation time in seconds: display-order index / fps.
	};

	// Layout of the CAPTURE buffers of one generation. Only single-plane formats (NV12) are described.
	struct CaptureLayout {
		uint32_t generation = 0;
		uint32_t fourcc = 0;
		Size2i size; // G_FMT width and height. The height can include the driver's padding (e.g. 1088 for 1080).
		uint32_t pitch = 0;
		uint64_t chroma_offset = 0;
		uint64_t buffer_size = 0;
		Rect2i visible;
		LocalVector<int> fds; // One per buffer. get_capture_layout() returns duplicates, which the caller closes.
	};

private:
	static constexpr uint32_t MAX_OUTPUT_BUFFERS = 32;
	static constexpr uint32_t MAX_CAPTURE_BUFFERS = 64;
	// The fork's QUEUED_RATE averages the last 30 intervals between OUTPUT QBUFs.
	static constexpr uint32_t RATE_WINDOW = 30;
	static constexpr uint32_t QUEUE_RING = 1024;
	static constexpr uint32_t RECENT_AUS = 64;
	static constexpr uint32_t MAX_EPOCHS = 8;

	struct OutputBuffer {
		void *map = nullptr;
		uint64_t length = 0;
		bool queued = false;
	};

	struct CaptureBuffer {
		int fd = -1; // VIDIOC_EXPBUF.
		bool queued = false; // In the driver.
	};

	// A run of access units fed in order since a jump (start, seek, loop or session restart). Frames are matched to their
	// epoch by timestamp, and numbered in output (display) order within it.
	struct Epoch {
		uint64_t first_seq = 0;
		uint32_t serial = 0;
		uint64_t base_index = 0;
		uint64_t frames = 0;
	};

	struct RecentAU {
		AnnexBSplitter::AccessUnit au;
		uint64_t media_index = 0;
	};

	struct IoctlError {
		const char *name = nullptr;
		int error = 0;
		uint32_t count = 0;
	};

	struct Stats {
		uint64_t aus_queued = 0;
		uint64_t aus_skipped = 0;
		uint64_t frames_decoded = 0;
		uint64_t frames_error = 0;
		uint64_t output_errors = 0;
		uint64_t source_changes = 0;
		uint64_t loops = 0;
		uint32_t reconfig_in_place = 0;
		uint32_t reconfig_restart = 0;
		uint32_t restarts = 0;
		uint32_t output_in_driver = 0;
		uint32_t capture_in_driver = 0;
		uint32_t capture_count = 0;
		double queued_rate = 0.0;
		double latency_sum = 0.0;
		double latency_max = 0.0;
		uint64_t latency_count = 0;
		uint64_t media_index = 0;
	};

	// Settings, read by the worker thread; change them only while closed (lead_time and verbose can change any time).
	String device_path = "/dev/video-dec0";
	String capture_format = "NV12";
	int output_buffer_count = 8;
	int capture_buffer_count = 12;
	int output_buffer_size = 0;
	double lead_time = 0.25;
	double reconfig_rate = 25.0;
	int timeout_ms = 5000;
	bool loop = false;
	bool verbose = false;

	// The stream, read-only while the worker runs.
	String file_path;
	const uint8_t *file_data = nullptr;
	uint64_t file_size = 0;
	AnnexBSplitter::Codec codec = AnnexBSplitter::CODEC_HEVC;
	uint32_t codec_fourcc = 0;
	uint32_t capture_fourcc = 0;
	double fps = 30.0;
	AnnexBSplitter::SPSInfo stream_sps;

	Thread thread;
	int wake_fd = -1;
	SafeFlag exit_requested;

	// Shared with the main thread.
	mutable Mutex mutex;
	State state = STATE_CLOSED;
	String last_error;
	double clock = 0.0;
	uint32_t requested_serial = 0;
	LocalVector<Frame> frames_ready;
	LocalVector<uint64_t> buffers_returned; // generation << 32 | buffer index.
	CaptureLayout layout; // fds owned by the worker.
	bool layout_valid = false;
	Stats stats;

	static Mutex device_close_mutex;
	static uint64_t device_close_usec;

	// Worker thread only.
	int fd = -1;
	AnnexBSplitter splitter;
	Stats wstats;
	OutputBuffer output[MAX_OUTPUT_BUFFERS];
	uint32_t output_count = 0;
	uint32_t output_in_driver = 0;
	uint32_t output_sizeimage = 0;
	Size2i output_size;
	CaptureBuffer capture[MAX_CAPTURE_BUFFERS];
	uint32_t capture_count = 0;
	uint32_t capture_in_driver = 0;
	uint32_t capture_min = 0;
	uint32_t capture_setups = 0;
	uint32_t generation = 0;
	uint32_t serial = 0;
	double worker_clock = 0.0;
	bool output_streaming = false;
	bool capture_streaming = false;
	bool capture_configured = false;
	bool need_setup = false;
	bool pending_reconfig = false;
	bool got_last = false;
	bool input_done = false;
	bool stop_sent = false;
	bool stop_failed = false;
	bool ended = false;
	uint64_t seq = 0;
	uint64_t seq_at_open = 0;
	uint64_t media_index = 0;
	uint64_t eos_events = 0;
	LocalVector<Epoch> epochs;
	RecentAU recent[RECENT_AUS];
	uint64_t recent_count = 0;
	double queue_time[QUEUE_RING] = {};
	uint64_t queue_seq[QUEUE_RING] = {};
	double t_reconfig_start = 0.0;
	double t_last_seen = 0.0;
	double t_eos = 0.0;
	double last_progress = 0.0;
	IoctlError ioctl_errors[32];
	uint32_t ioctl_error_count = 0;

	static double _now();
	static String _fourcc_to_string(uint32_t p_fourcc);
	static Error _map_file(const String &p_path, const uint8_t *&r_data, uint64_t &r_size);
	static bool _parse_codec(const String &p_codec, AnnexBSplitter::Codec &r_codec);

	void _log(const String &p_message) const;
	void _log_verbose(const String &p_message) const;
	void _fail(const String &p_message);
	void _set_state(State p_state);
	void _wake();
	int _ioctl(int p_fd, unsigned long p_request, void *p_arg, const char *p_name, int p_quiet_1 = 0, int p_quiet_2 = 0);

	Error _open_device();
	void _close_device(const char *p_why);
	Error _setup_output();
	Error _subscribe_events();
	Error _start_session();
	Error _restart_session(const AnnexBSplitter::AccessUnit &p_resume, uint64_t p_media_index, const char *p_why);

	uint64_t _timestamp_for_seq(uint64_t p_seq) const;
	uint64_t _seq_for_timestamp(uint64_t p_usec) const;
	double _queued_rate() const;
	int _free_output_buffer() const;
	int _queue_access_unit(uint32_t p_index, const AnnexBSplitter::AccessUnit &p_au);
	int _queue_input();
	void _begin_epoch(uint32_t p_serial, uint64_t p_base_index);
	Epoch *_find_epoch(uint64_t p_seq);
	void _rate_reset();
	int _dequeue_output();

	Error _queue_capture(uint32_t p_index);
	Error _setup_capture();
	void _teardown_capture(const char *p_why);
	void _explain_enomem(const char *p_what);
	void _handle_events();
	int _dequeue_capture();
	void _send_stop();
	void _process_inbox();
	void _seek_to_start(uint32_t p_serial);
	Error _restart_after_reconfig();
	void _publish_stats();
	void _run();
	static void _thread_func(void *p_userdata);

	void _unmap_file();

protected:
	static void _bind_methods();

public:
	static Dictionary probe_file(const String &p_path, const String &p_codec);

	Error open(const String &p_path, const String &p_codec, double p_fps);
	void close();
	bool is_open() const { return thread.is_started(); }

	// Main thread: feeding stops once the access units fed reach the clock + lead_time.
	void set_clock(double p_seconds);
	double get_clock() const;
	// Main thread: drops what is queued and feeds from the first access unit again (new serial). Returns the serial.
	int seek_to_start();

	// Main thread (C++): the frame handoff.
	bool pop_frame(Frame &r_frame);
	void return_buffer(uint32_t p_generation, uint32_t p_buffer);
	Error get_capture_layout(uint32_t p_generation, CaptureLayout &r_layout) const;

	State get_state() const;
	String get_last_error() const;
	Dictionary get_stats() const;
	Dictionary get_stream_info() const;
	double get_fps() const { return fps; }

	void set_device_path(const String &p_path);
	String get_device_path() const { return device_path; }
	void set_capture_format(const String &p_fourcc);
	String get_capture_format() const { return capture_format; }
	void set_output_buffer_count(int p_count);
	int get_output_buffer_count() const { return output_buffer_count; }
	void set_capture_buffer_count(int p_count);
	int get_capture_buffer_count() const { return capture_buffer_count; }
	void set_output_buffer_size(int p_size);
	int get_output_buffer_size() const { return output_buffer_size; }
	void set_lead_time(double p_seconds);
	double get_lead_time() const { return lead_time; }
	void set_reconfig_rate(double p_fps);
	double get_reconfig_rate() const { return reconfig_rate; }
	void set_timeout_ms(int p_ms);
	int get_timeout_ms() const { return timeout_ms; }
	void set_loop(bool p_loop);
	bool get_loop() const { return loop; }
	void set_verbose(bool p_verbose);
	bool is_verbose() const { return verbose; }

	~V4L2Decoder();
};

VARIANT_ENUM_CAST(V4L2Decoder::State);
