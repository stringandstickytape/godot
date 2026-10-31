/**************************************************************************/
/*  media_source.h                                                        */
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

#include "core/math/audio_frame.h"
#include "core/object/ref_counted.h"
#include "core/os/condition_variable.h"
#include "core/os/mutex.h"
#include "core/os/thread.h"
#include "core/templates/local_vector.h"
#include "core/templates/safe_refcount.h"
#include "core/variant/binder_common.h"
#include "core/variant/variant.h"

struct AVBSFContext;
struct AVCodecContext;
struct AVFormatContext;
struct AVFrame;
struct AVPacket;
struct SwrContext;
struct AVIOContext;

class SmbFile;

class AudioStreamMedia;

// A media file (Matroska/WebM, MP4/MOV, MPEG-TS, AVI) opened with FFmpeg's libavformat, read on a worker thread.
// Video is only demuxed: access units go, in Annex-B for H.264/HEVC, through a bounded queue to a consumer (V4L2Decoder).
// Audio of the selected track is decoded and resampled to the AudioServer mix rate (stereo float), into a ring buffer
// that AudioStreamMedia plays; its playback position is the A/V master clock.
//
// Times are media seconds: the stream timestamp minus the file's start time, so the first frame is at about 0.
// Every seek() increments a serial. Video packets carry the serial they were read under; a consumer that sees a new serial
// knows that everything before it is stale.
class MediaSource : public RefCounted {
	GDCLASS(MediaSource, RefCounted);

public:
	enum State {
		STATE_CLOSED,
		STATE_READING,
		STATE_ENDED, // The demuxer reached the end of the file; what is queued can still be consumed.
		STATE_ERROR,
	};

	enum VideoCodec {
		VIDEO_CODEC_NONE,
		VIDEO_CODEC_H264,
		VIDEO_CODEC_HEVC,
		VIDEO_CODEC_VP9,
		VIDEO_CODEC_AV1,
		VIDEO_CODEC_OTHER,
	};

	struct VideoInfo {
		bool valid = false;
		VideoCodec codec = VIDEO_CODEC_NONE;
		String codec_name;
		int stream_index = -1;
		int width = 0; // From the container / codec parameters (display size).
		int height = 0;
		double fps = 0.0;
		double sample_aspect_ratio = 1.0; // Pixel aspect (anamorphic DVD, some broadcasts); 1 if square or unknown.
		int bit_depth = 0;
		String profile;
		String pixel_format;
		AnnexBSplitter::SPSInfo sps; // H.264/HEVC: the first SPS (coded size, cropped size, bit depth).
	};

	// One access unit. Annex-B for H.264 and HEVC, with the parameter sets in band before the first picture after open()
	// and after every seek. Owned by whoever popped it until release_video_packet().
	struct VideoPacket {
		AVPacket *packet = nullptr;
		const uint8_t *data = nullptr;
		uint32_t size = 0;
		double pts = 0.0; // Media seconds.
		double dts = 0.0; // Decode order; equal to pts when the container has no DTS.
		uint32_t serial = 0;
		bool keyframe = false;
		uint32_t sps_width = 0; // Coded size of an SPS in this access unit (0: none).
		uint32_t sps_height = 0;
		// H.264/HEVC keyframes (and the first access unit after a jump): the type of the first picture NAL unit
		// (H.264: 5 IDR, 1 I/recovery point; HEVC: 19/20 IDR, 21 CRA, 16-18 BLA). -1: not looked at.
		int8_t nal_type = -1;
	};

	// A subtitle event of the selected subtitle track. Text tracks (SubRip, ASS/SSA, MP4 timed text, WebVTT) are decoded:
	// text is plain (ASS override tags removed, line breaks as \n), ass the ASS event line(s) for libass. Bitmap tracks
	// (PGS, VobSub, DVB) are passed through: data is the raw packet, to be decoded later.
	struct SubtitleEvent {
		double start = 0.0; // Media seconds.
		double end = -1.0; // -1: until the next event (bitmap tracks, or no duration in the container).
		String text;
		String ass;
		PackedByteArray data;
		bool bitmap = false;
		uint32_t serial = 0;
	};

private:
	struct AudioTrack {
		int stream_index = -1;
		String codec;
		int channels = 0;
		String channel_layout;
		int sample_rate = 0;
		int64_t bit_rate = 0;
		String language;
		String title;
		bool is_default = false;
	};

	struct SubtitleTrack {
		int stream_index = -1;
		String codec;
		bool text = false; // Decoded to text here.
		bool bitmap = false; // Passed through as packets.
		String language;
		String title;
		bool is_default = false;
		bool forced = false;
	};

	struct AudioMarker {
		uint64_t frame = 0; // Absolute ring frame index.
		double time = 0.0; // Media time of that frame.
	};

	struct Stats {
		uint64_t packets_read = 0;
		uint64_t bytes_read = 0;
		uint64_t video_packets = 0;
		uint64_t video_bytes = 0;
		uint64_t video_skipped = 0; // Before the first keyframe after open/seek.
		uint64_t video_parameter_sets_added = 0;
		uint64_t audio_packets = 0;
		uint64_t audio_frames_out = 0; // Resampled frames written to the ring.
		uint64_t audio_decode_errors = 0;
		uint64_t audio_dropped_frames = 0; // Before a seek target.
		uint64_t audio_silence_frames = 0; // Gap filled after a seek.
		uint64_t read_errors = 0;
		uint64_t seek_retries = 0; // Seeks moved further back because the first keyframe found was after the target (TS).
		uint64_t subtitle_events = 0;
		uint64_t subtitle_decode_errors = 0;
		double thread_cpu_seconds = 0.0; // CPU time of the demux thread (CLOCK_THREAD_CPUTIME_ID).
	};

	static constexpr uint32_t VIDEO_QUEUE_MAX = 512;
	static constexpr uint64_t VIDEO_QUEUE_MAX_BYTES = 96ull << 20;
	static constexpr uint32_t VIDEO_QUEUE_MIN = 8;
	static constexpr uint32_t AUDIO_MARKERS_MAX = 32;
	static constexpr uint32_t AUDIO_FIFO_MAX = 8192; // Compressed audio packets (TrueHD: 1200 per second).
	static constexpr uint64_t AUDIO_FIFO_MAX_BYTES = 32ull << 20;
	static constexpr double AUDIO_RING_SECONDS = 4.0;
	static constexpr uint32_t SUBTITLE_QUEUE_MAX = 512;
	static constexpr int SEEK_RETRIES_MAX = 6;

	// Settings: change them before open().
	double video_queue_seconds = 1.0;
	double audio_queue_seconds = 1.0;
	bool downmix_normalize = false;
	Dictionary audio_decoder_options; // FFmpeg decoder AVOptions, e.g. {"core_only": "1"} for DTS without its extensions.

	// Set up by open(), read-only while the thread runs.
	String path;
	AVFormatContext *format = nullptr;
	String format_name;
	double start_time = 0.0;
	double duration = 0.0;
	int64_t bit_rate = 0;
	VideoInfo video;
	LocalVector<AudioTrack> audio_tracks;
	LocalVector<SubtitleTrack> subtitle_tracks;
	LocalVector<uint8_t> config_parameter_sets; // Annex-B VPS/SPS/PPS from the codec configuration (avcC/hvcC).
	int output_rate = 48000;
	uint64_t ring_capacity = 0;

	// smb://host/share/path: the file is read by an SmbFile (readahead thread) through a custom AVIOContext.
	SmbFile *smb_file = nullptr;
	AVIOContext *avio = nullptr;
	int64_t network_buffer_size = 16 << 20;
	int network_requests = 4;

	Thread thread;

	// Shared, under mutex. Never held while reading the file or decoding.
	mutable BinaryMutex mutex;
	ConditionVariable condition;
	bool exit_requested = false;
	State state = STATE_CLOSED;
	String last_error;
	uint32_t serial = 0;
	bool seek_pending = false;
	double seek_target = 0.0;
	int audio_track = -1;
	int subtitle_track = -1;
	LocalVector<SubtitleEvent> subtitle_queue;
	String subtitle_header;
	int wake_fd = -1;
	VideoPacket video_queue[VIDEO_QUEUE_MAX];
	uint32_t video_head = 0;
	uint32_t video_count = 0;
	uint64_t video_queued_bytes = 0;
	LocalVector<AVPacket *> packet_pool;
	bool demux_eof = false;
	double last_video_pts = -1.0;
	uint32_t video_queue_target = VIDEO_QUEUE_MIN;
	Stats stats; // Published copy of dstats.
	uint64_t seek_count = 0;
	SafeFlag demux_idle; // Set by the thread, with mutex held, from just before it checks for work until it wakes.
	SafeNumeric<uint32_t> audio_space_wanted; // Frames of decoded audio waiting for room in the ring (0: none).

	// The decoded audio, under audio_mutex (the only lock the mixer takes on its path; it takes mutex only to wake the
	// thread, and never while holding audio_mutex).
	mutable BinaryMutex audio_mutex;
	LocalVector<AudioFrame> ring;
	uint64_t ring_read = 0;
	uint64_t ring_write = 0;
	AudioMarker markers[AUDIO_MARKERS_MAX];
	uint32_t marker_count = 0;
	double audio_position = 0.0;
	uint32_t audio_serial = 0;
	bool audio_eof = false;
	bool audio_in_underrun = false;
	bool audio_mixed_since_seek = false; // An empty ring before the first frame after open/seek is a wait, not an underrun.
	uint64_t audio_underruns = 0;
	uint64_t audio_seek_waits = 0;
	uint64_t audio_frames_mixed = 0;

	// Demux thread only (and open(), before the thread starts).
	Stats dstats;
	AVPacket *read_packet = nullptr;
	AVBSFContext *bsf = nullptr;
	AVCodecContext *audio_codec = nullptr;
	AVFrame *audio_frame = nullptr;
	SwrContext *swr = nullptr;
	int swr_in_format = -1;
	int swr_in_rate = 0;
	int swr_in_channels = 0;
	uint64_t swr_in_mask = 0;
	String swr_description; // Logged when it changes (not after every seek).
	int current_audio_track = -1;
	int current_audio_stream = -1;
	AVCodecContext *subtitle_codec = nullptr;
	int current_subtitle_track = -1;
	int current_subtitle_stream = -1;
	double seek_wanted = -1.0; // The target of the last seek until its keyframe is found.
	double seek_position = 0.0; // Where the file was last seeked to (earlier than seek_wanted after a retry).
	int seek_retries = 0;
	double pending_reseek = -1.0;
	uint32_t demux_serial = 0;
	bool need_keyframe = true;
	bool need_parameter_sets = true;
	bool eof_reached = false;
	bool eof_drained = false;
	uint32_t read_errors_in_row = 0;
	LocalVector<uint8_t> inband_parameter_sets; // The latest in-band VPS/SPS/PPS.
	LocalVector<uint8_t> scan_parameter_sets;
	double next_video_pts = 0.0;
	double audio_trim_until = -1.0;
	double audio_next_time = -1.0;
	// Compressed audio packets read while the ring is full (audio not played yet, or ahead of the video in the file), so the
	// video can still be read. Decoded in order once the ring has room.
	LocalVector<AVPacket *> audio_fifo;
	uint32_t audio_fifo_head = 0;
	uint32_t audio_fifo_count = 0;
	uint64_t audio_fifo_bytes = 0;
	LocalVector<float> scratch; // Interleaved stereo.
	uint32_t scratch_frames = 0;
	uint32_t scratch_offset = 0;
	double scratch_time = 0.0;
	uint32_t scratch_serial = 0;

	static String _av_error(int p_error);
	static bool _scan_access_unit(const uint8_t *p_data, uint64_t p_size, bool p_hevc, AnnexBSplitter::SPSInfo &r_sps, LocalVector<uint8_t> *r_parameter_sets, int *r_nal_type = nullptr);
	static String _ass_to_text(const String &p_ass);
	static bool _is_text_subtitle(int p_codec_id);
	static bool _is_bitmap_subtitle(int p_codec_id);
	static void _config_to_annexb(const uint8_t *p_data, int p_size, bool p_hevc, LocalVector<uint8_t> &r_out);

	void _set_error(const String &p_message);
	void _close_network();
	double _media_time(int64_t p_ts, int p_stream) const;
	bool _is_annexb_codec() const { return video.codec == VIDEO_CODEC_H264 || video.codec == VIDEO_CODEC_HEVC; }

	uint32_t _video_queue_target() const;
	AVPacket *_packet_get();
	void _packet_put(AVPacket *p_packet);
	void _flush_video_queue_locked();
	void _wake_consumer_locked();
	bool _needs_input_locked() const;

	void _apply_discard();
	void _open_audio_track(int p_track);
	void _close_audio_codec();
	bool _configure_swr(const AVFrame *p_frame);
	void _decode_audio(const AVPacket *p_packet);
	void _audio_fifo_push(AVPacket *p_packet);
	AVPacket *_audio_fifo_pop();
	void _audio_fifo_clear();
	void _resample_frame(const AVFrame *p_frame);
	void _flush_scratch();
	void _write_ring_locked(const float *p_samples, uint32_t p_frames, double p_time);

	void _open_subtitle_track(int p_track);
	void _handle_subtitle_packet();
	void _push_subtitle(SubtitleEvent &p_event);
	bool _holding_audio() const;
	void _seek_file(double p_position);

	void _deliver_video(AVPacket *p_packet);
	void _receive_from_bsf();
	void _handle_video_packet();
	bool _read_packet();
	void _drain_at_eof();
	void _do_seek(double p_target);
	void _thread_loop();
	static void _thread_func(void *p_userdata);
	void _close_internal();

protected:
	static void _bind_methods();

public:
	Error open(const String &p_path);
	void close();
	bool is_open() const { return format != nullptr; }

	String get_path() const { return path; }
	Dictionary get_info() const;
	Dictionary get_video_info() const;
	Array get_audio_tracks() const;
	Array get_subtitle_tracks() const;
	Error select_audio_track(int p_track);
	int get_audio_track() const;
	// Subtitles (-1: none). Selecting doesn't seek: events whose packets were already read (about the queue length
	// ahead of the playback position) aren't delivered.
	Error select_subtitle_track(int p_track);
	int get_subtitle_track() const;
	// The next event of the selected subtitle track (start, end, text, ass, bitmap, data, serial), or an empty Dictionary.
	// Events come in file order, ahead of the playback position; keep them until their time.
	Dictionary read_subtitle_event();
	// The ASS header (styles) of the selected text track, for libass ([Script Info], [V4+ Styles], [Events]).
	String get_subtitle_header() const;

	// Any thread. Drops everything queued and reads again from the keyframe at or before p_seconds; decoded audio before
	// p_seconds is dropped. Returns the new serial.
	int seek(double p_seconds);
	uint32_t get_serial() const;
	double get_seek_target() const;

	double get_duration() const { return duration; }
	double get_start_time() const { return start_time; }
	double get_position() const;
	bool is_eof() const;
	State get_state() const;
	String get_last_error() const;

	static void set_smb_credentials(const String &p_host, const String &p_user, const String &p_password, const String &p_domain = String());
	void set_network_buffer_size(int64_t p_bytes);
	int64_t get_network_buffer_size() const { return network_buffer_size; }
	void set_network_requests(int p_requests);
	int get_network_requests() const { return network_requests; }
	Dictionary get_stats() const;

	Ref<AudioStreamMedia> get_audio_stream();
	int get_output_rate() const { return output_rate; }

	// Script/test access to the video queue: the next access unit as a Dictionary (data, pts, dts, keyframe, serial), or
	// an empty Dictionary if none is queued yet. Don't mix with a V4L2Decoder reading the same source.
	Dictionary read_video_packet();

	// C++ consumer API (V4L2Decoder).
	const VideoInfo &get_video_stream_info() const { return video; }
	// Why the Frame can't decode and show this video in hardware, as a sentence for the user; empty if it can.
	// p_allow_10bit: V4L2Decoder.experimental_10bit (Q10C capture) is on.
	static String get_hw_unsupported_reason(const VideoInfo &p_info, bool p_allow_10bit = false);
	bool pop_video_packet(VideoPacket &r_packet);
	void release_video_packet(VideoPacket &r_packet);
	// The demux thread writes 1 to this eventfd when the video queue becomes non-empty, at the end and on seek. -1: none.
	void set_video_wake_fd(int p_fd);
	// True once the file is read to the end, everything of p_serial was popped and no seek is pending.
	bool is_video_input_done(uint32_t p_serial) const;

	// Audio (AudioStreamPlaybackMedia, audio thread). Always fills p_frames (silence when nothing is decoded yet).
	void mix_audio(AudioFrame *p_buffer, int p_frames);
	double get_audio_position() const;
	bool is_audio_finished() const;
	bool has_audio() const { return audio_track >= 0; }

	void set_video_queue_seconds(double p_seconds);
	double get_video_queue_seconds() const { return video_queue_seconds; }
	void set_audio_queue_seconds(double p_seconds);
	double get_audio_queue_seconds() const { return audio_queue_seconds; }
	void set_downmix_normalize(bool p_normalize);
	bool is_downmix_normalize() const { return downmix_normalize; }
	void set_audio_decoder_options(const Dictionary &p_options);
	Dictionary get_audio_decoder_options() const { return audio_decoder_options; }

	~MediaSource();
};

VARIANT_ENUM_CAST(MediaSource::State);
