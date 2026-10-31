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
#include "media_source.h"

#include "core/object/ref_counted.h"
#include "core/os/mutex.h"
#include "core/os/thread.h"
#include "core/templates/local_vector.h"
#include "core/templates/safe_refcount.h"
#include "core/variant/binder_common.h"
#include "core/variant/variant.h"

// A V4L2 stateful memory-to-memory decoder session (the Steam Frame's Qualcomm iris decoder, /dev/video-dec0), fed from a
// raw Annex-B H.264/HEVC file (open()) or from a MediaSource's access units (open_source(); H.264, HEVC or VP9), on a
// worker thread. A port of the session logic of spikes/01-decode/v4l2dec.c.
//
// VP9: one frame per OUTPUT buffer: a superframe (a hidden alt-ref frame and a shown one in one container packet) is split
// with FFmpeg's vp9_superframe_split first, as other players on the device do. No Annex-B, no parameter sets; after a seek
// the source starts at a keyframe, which resets every reference, so the session just goes on (no end-of-sequence NAL
// unit, no flush). Two VP9 sessions at once crash the iris firmware (SteamOS #2865; device run 2026-10-31), so at most one
// V4L2Decoder holds a VP9 session at a time (is_vp9_session_live()); a second open_source() of VP9 fails with ERR_BUSY.
// A VP9 session also stops itself, with state error, on decoder faults (DQBUF failures, a run of ERROR buffers, no
// frame for the input fed); HWVideoTexture then goes on in software, and hardware VP9 stays off for the rest of the run.
//
// One long-lived session: OUTPUT (bitstream) buffers are mmapped and fed one access unit each, paced by a clock that the
// consumer sets (set_clock()), so the queue rate the driver measures stays near the playback rate. CAPTURE buffers are
// allocated after SOURCE_CHANGE and exported once each (VIDIOC_EXPBUF); the fds are kept until the CAPTURE queue is torn
// down. A decoded buffer stays dequeued, owned by the consumer, until it hands it back with return_buffer().
class V4L2Decoder : public RefCounted {
	GDCLASS(V4L2Decoder, RefCounted);

public:
	// What goes into the bitstream in front of the first access unit after a seek on a MediaSource (not at a session
	// start): an end-of-sequence NAL unit (H.264: end of sequence, type 10; HEVC: EOS_NUT, type 36), so the decoder
	// treats the keyframe as a random access point. That matters for open GOPs: after an HEVC CRA or an H.264
	// recovery-point I picture, the leading pictures reference pictures from before the seek; after EOS the decoder
	// drops them (HEVC: NoRaslOutputFlag) instead of decoding them from the wrong references. It also makes the decoder
	// output the pictures still held for reordering.
	enum SeekEOS {
		SEEK_EOS_OFF,
		SEEK_EOS_PREPEND, // In the same OUTPUT buffer, before the access unit.
		SEEK_EOS_SEPARATE, // In an OUTPUT buffer of its own.
	};

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
		uint32_t serial = 0; // File: the seek_to_start() count when the access unit was fed. Source: the source's seek serial.
		double time = 0.0; // Presentation time in seconds. File: display-order index / fps. Source: the access unit's PTS.
	};

	// Layout of the CAPTURE buffers of one generation. Only single-buffer NV12, QC08C and QC10C are described.
	// The uncompressed header of a VP9 frame (spec section 6.2), of the first keyframe in a packet (or of its first frame).
	struct VP9FrameInfo {
		bool valid = false;
		bool keyframe = false;
		bool show_existing_frame = false;
		int profile = 0;
		int bit_depth = 8;
		int color_space = 0;
		bool full_range = false;
		int subsampling_x = 1;
		int subsampling_y = 1;
		uint32_t width = 0; // Keyframes only.
		uint32_t height = 0;
		uint32_t render_width = 0;
		uint32_t render_height = 0;
		int frames = 1; // Frames in the packet (a superframe has more than one).
	};

	struct CaptureLayout {
		uint32_t generation = 0;
		uint32_t fourcc = 0;
		Size2i size; // G_FMT width and height. The height can include the driver's padding (e.g. 1088 for 1080).
		uint32_t pitch = 0; // Bytes per row of pixels (QC10C: of 32-bit words holding 3 samples each).
		// NV12: where the chroma rows start. QC08C, QC10C: where the UV metadata starts (the second plane of the layout).
		uint64_t chroma_offset = 0;
		bool ubwc = false; // QC08C, QC10C: DRM_FORMAT_MOD_QCOM_COMPRESSED.
		// QC08C, QC10C: where the pixels start (the metadata of each plane is in front of them: Y at 0, UV at chroma_offset),
		// and the metadata pitches (one byte per tile).
		uint64_t y_data_offset = 0;
		uint64_t uv_data_offset = 0;
		uint32_t y_meta_pitch = 0;
		uint32_t uv_meta_pitch = 0;
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
	// VP9 fault detection (the firmware crash of SteamOS #2865 shows as DQBUF EINVAL, then nothing).
	static constexpr uint32_t VP9_DQBUF_FAILURES = 3; // Failed DQBUFs in a row (not EAGAIN/EPIPE).
	static constexpr uint32_t VP9_ERROR_RUN = 8; // Buffers with V4L2_BUF_FLAG_ERROR in a row, without a good frame.
	static constexpr uint32_t VP9_UNANSWERED_AUS = 3; // The no-frame timer starts when this many are fed since the last frame.
	static constexpr double VP9_NO_FRAME_SECONDS = 4.0;

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
		double pts = 0.0; // Source mode.
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
		uint64_t eos_inserted = 0;
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
		double input_time = 0.0; // Source mode: PTS of the last access unit fed.
		double thread_cpu_seconds = 0.0; // CPU time of the worker thread.
		uint64_t vp9_packets = 0; // VP9 container packets fed.
		uint64_t vp9_superframes = 0; // Of those, superframes split into frames (vp9_superframe_split).
		uint64_t vp9_split_frames = 0; // The frames they gave, each in an OUTPUT buffer of its own.
		uint64_t vp9_hidden_frames = 0; // Of those, frames not shown (alt-ref).
	};

	// Settings, read by the worker thread; change them only while closed (lead_time and verbose can change any time).
	String device_path = "/dev/video-dec0";
	String capture_format = "Q08C"; // UBWC NV12: same picture as NV12, less bandwidth (device A/B, 2026-10-31).
	int64_t capture_chroma_offset = 0; // > 0: use this chroma offset for NV12/P010 instead of computing it.
	int output_buffer_count = 8;
	int capture_buffer_count = 12;
	int output_buffer_size = 0;
	double lead_time = 0.25;
	double reconfig_rate = 25.0;
	int timeout_ms = 5000;
	bool loop = false;
	bool verbose = false;
	bool experimental_10bit = false; // Decode 10-bit streams to Q10C for HWVideoTexture's experimental import.
	bool q10c_ycbcr = false; // 10-bit without P010 goes to Q10C, for HWVideoTexture's import through a YCbCr sampler.
	SeekEOS seek_eos = SEEK_EOS_SEPARATE; // iris rejects the NAL unit in the keyframe's buffer (1 decoder error per seek).

	// The stream, read-only while the worker runs. With a MediaSource (source valid) there is no file mapping: access
	// units are popped from the source, and their PTS become the frame times.
	Ref<MediaSource> source;
	String file_path;
	const uint8_t *file_data = nullptr;
	uint64_t file_size = 0;
	AnnexBSplitter::Codec codec = AnnexBSplitter::CODEC_HEVC;
	bool vp9 = false; // Source mode only: VP9 packets (codec is unused then).
	uint32_t codec_fourcc = 0;
	uint32_t capture_fourcc = 0;
	double fps = 30.0;
	AnnexBSplitter::SPSInfo stream_sps;

	Thread thread;
	int wake_fd = -1;
	SafeFlag exit_requested;
	SafeFlag worker_finished; // The worker has closed the device and returned (close() won't block then).

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
	bool format_error = false; // The error is a CAPTURE format the player can't show (e.g. no P010 for 10-bit VP9).
	bool decoder_fault = false; // The error is the decoder's (DQBUF failures, ERROR buffers, no frames, HANG).

	static Mutex device_close_mutex;
	static uint64_t device_close_usec;
	static Mutex p010_mutex;
	static int p010_state; // -1 not looked yet, 0 no, 1 yes.
	static String p010_formats;
	// The one VP9 session (see the class comment), and why hardware VP9 is off for the rest of the run ("" if it isn't).
	static Mutex vp9_mutex;
	static const V4L2Decoder *vp9_owner;
	static String vp9_disabled_reason;

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
	bool data_offset_logged = false;
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
	double queue_pts[QUEUE_RING] = {}; // Source mode: PTS of the access unit queued as seq.
	MediaSource::VideoPacket held; // Source mode: the next access unit, popped but not fed yet (pacing).
	bool have_held = false;
	double input_time = 0.0;
	double epoch_start_pts = 0.0; // Source mode: PTS of the first access unit of the current epoch.
	bool eos_pending = false; // Source mode: put an end-of-sequence NAL unit before the next access unit (seek_eos).
	Size2i last_sps_size;
	double t_reconfig_start = 0.0;
	double t_last_seen = 0.0;
	double t_eos = 0.0;
	double last_progress = 0.0;
	IoctlError ioctl_errors[32];
	uint32_t ioctl_error_count = 0;
	uint32_t vp9_bit_depth = 0; // From the uncompressed header of the latest VP9 keyframe fed (0: none yet).
	uint32_t dqbuf_failures = 0; // In a row.
	int dqbuf_errno = 0; // Of the last one.
	uint32_t error_run = 0; // ERROR buffers since the last good frame.
	uint32_t fed_since_frame = 0;
	double t_unanswered = 0.0; // When the VP9_UNANSWERED_AUS-th access unit since the last frame was fed (0: not now).
	// VP9 superframes: FFmpeg's vp9_superframe_split (null if FFmpeg lacks it: superframes then go in whole), the frames of
	// `held` still to feed, and the next one.
	struct AVBSFContext *vp9_split = nullptr;
	bool vp9_split_tried = false;
	bool vp9_held_split = false; // `held` went through the filter (its frames are in vp9_parts, or it is one frame).
	LocalVector<struct AVPacket *> vp9_parts;
	uint32_t vp9_part = 0;

	static double _now();
	static String _fourcc_to_string(uint32_t p_fourcc);
	static Error _map_file(const String &p_path, const uint8_t *&r_data, uint64_t &r_size);
	static bool _parse_codec(const String &p_codec, AnnexBSplitter::Codec &r_codec);

	void _log(const String &p_message) const;
	void _log_verbose(const String &p_message) const;
	void _fail(const String &p_message);
	void _fail_decoder(const String &p_message); // _fail() for a decoder fault (decoder_fault).
	bool _check_vp9_faults(double p_now);
	void _release_vp9();
	void _set_state(State p_state);
	void _wake();
	int _ioctl(int p_fd, unsigned long p_request, void *p_arg, const char *p_name, int p_quiet_1 = 0, int p_quiet_2 = 0);

	Error _open_device();
	void _close_device(const char *p_why);
	Error _setup_output();
	Error _subscribe_events();
	Error _start_session();
	Error _restart_session(const AnnexBSplitter::AccessUnit &p_resume, uint64_t p_media_index, const char *p_why);
	Error _restart_session_source(const char *p_why);
	Error _wait_for_source_input();
	Error _start_worker();

	uint64_t _timestamp_for_seq(uint64_t p_seq) const;
	uint64_t _seq_for_timestamp(uint64_t p_usec) const;
	double _queued_rate() const;
	int _free_output_buffer() const;
	int _qbuf_output(uint32_t p_index, uint64_t p_bytes, bool p_counts_as_frame = true);
	int _queue_vp9_packet(uint32_t p_index);
	void _split_vp9_packet();
	void _drop_vp9_parts();
	void _log_vp9_split_summary();
	int _queue_access_unit(uint32_t p_index, const AnnexBSplitter::AccessUnit &p_au);
	bool _hold_source_packet();
	int _queue_source_packet(uint32_t p_index);
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
	static Dictionary probe_file(const String &p_path, const String &p_codec, int p_max_access_units = 0, int p_list_count = 6);
	// The VP9 uncompressed header of the first keyframe in a packet (else of its first frame). False if it isn't VP9.
	static bool parse_vp9_packet(const uint8_t *p_data, uint64_t p_size, VP9FrameInfo &r_info);
	// The same for scripts and tests (a packet from MediaSource.read_video_packet()); {} if it isn't VP9.
	static Dictionary probe_vp9_packet(const PackedByteArray &p_packet);
	// The frames of a VP9 packet as they go to the decoder (FFmpeg's vp9_superframe_split): [{"data", "hidden"}], one entry
	// for a plain frame, none if the filter is missing or refuses the packet.
	static Array split_vp9_packet(const PackedByteArray &p_packet);

	Error open(const String &p_path, const String &p_codec, double p_fps);
	// Decodes the video stream of an open MediaSource (H.264, HEVC or VP9). Seeks are done on the source
	// (MediaSource.seek()); the decoder follows its serial. loop is ignored. VP9 while another V4L2Decoder has a VP9
	// session: ERR_BUSY (see the class comment).
	Error open_source(const Ref<MediaSource> &p_source);
	Ref<MediaSource> get_media_source() const { return source; }
	void close();
	bool is_open() const { return thread.is_started(); }
	bool is_worker_finished() const { return worker_finished.is_set(); }

	// Main thread: feeding stops once the access units fed reach the clock + lead_time.
	void set_clock(double p_seconds);
	double get_clock() const;
	// Main thread: drops what is queued and feeds from the first access unit again (new serial). Returns the serial.
	// With a MediaSource: MediaSource.seek(0).
	int seek_to_start();

	// Main thread (C++): the frame handoff.
	bool pop_frame(Frame &r_frame);
	void return_buffer(uint32_t p_generation, uint32_t p_buffer);
	Error get_capture_layout(uint32_t p_generation, CaptureLayout &r_layout) const;

	State get_state() const;
	String get_last_error() const;
	// With state error: the error is a CAPTURE format the player can't show / a decoder fault (see the members).
	bool is_format_error() const;
	bool is_decoder_fault() const;
	Dictionary get_stats() const;
	Dictionary get_stream_info() const;
	double get_fps() const { return fps; }
	bool is_vp9() const { return vp9 && source.is_valid(); }
	// Whether a V4L2Decoder holds the VP9 session now (open_source() of VP9 until close()).
	static bool is_vp9_session_live();
	// Why hardware VP9 is off for the rest of the run (a VP9 session had a decoder fault); "" if it isn't.
	static String get_vp9_disabled_reason();

	void set_device_path(const String &p_path);
	String get_device_path() const { return device_path; }
	void set_capture_format(const String &p_fourcc);
	void set_capture_chroma_offset(int64_t p_offset);
	int64_t get_capture_chroma_offset() const { return capture_chroma_offset; }
	// Whether the decoder offers linear P010 CAPTURE (a patched iris module): looks once per process (opens the device at
	// idle, ENUM_FMT on CAPTURE with an HEVC OUTPUT format, closes it), then answers from the cache. HWVIDEO_P010=1 or 0
	// overrides. Also tells MediaSource (hw_decodable for 10-bit).
	static bool is_p010_available(const String &p_device_path = "/dev/video-dec0");
	static String get_idle_capture_formats() { return p010_formats; }
	static void _note_p010_offered(bool p_offered, const String &p_formats);
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
	void set_experimental_10bit(bool p_enabled);
	bool is_experimental_10bit() const { return experimental_10bit; }
	// 10-bit streams without P010 (or with HWVIDEO_P010=0) decode to Q10C, for HWVideoTexture's YCbCr import. HWVideoTexture
	// sets it at open_source() when the Vulkan driver has a Q10C candidate (DmabufImage.get_q10c_candidates()).
	void set_q10c_ycbcr(bool p_enabled);
	bool is_q10c_ycbcr() const { return q10c_ycbcr; }
	// HWVIDEO_P010=0: P010 isn't used even when the driver offers it (so Q10C is, on a patched module too).
	static bool is_p010_forced_off();
	bool is_verbose() const { return verbose; }
	void set_seek_eos(SeekEOS p_mode);
	SeekEOS get_seek_eos() const { return seek_eos; }

	~V4L2Decoder();
};

VARIANT_ENUM_CAST(V4L2Decoder::State);
VARIANT_ENUM_CAST(V4L2Decoder::SeekEOS);
