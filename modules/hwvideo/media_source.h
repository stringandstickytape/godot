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
#include "audio_dynamics.h"
#include "subtitle_style.h"

#include "core/io/image.h"
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
struct AVStream;
struct SwrContext;
struct AVIOContext;

class SmbFile;

class AudioStreamMedia;
class AudioTempo;

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
		// Colour description from the container / codec parameters (FFmpeg's names; empty if unspecified).
		String color_primaries; // "bt709", "bt2020", ...
		String color_transfer; // "bt709", "smpte2084" (PQ), "arib-std-b67" (HLG), ...
		String color_matrix; // "bt709", "bt2020nc", "bt470bg" / "smpte170m" (BT.601), ...
		String color_range; // "tv" (limited) or "pc" (full).
		// Stereo 3D and spherical metadata from the container (Matroska StereoMode and Projection, MP4 st3d and sv3d).
		String stereo_mode; // "" (the container says nothing), "2d", "sbs", "tb", "frame_sequence", "checkerboard", ...
		bool stereo_inverted = false; // The right eye's picture comes first (left half, or top).
		double stereo_baseline_mm = 0.0; // Distance between the cameras, if given.
		double stereo_fov = 0.0; // Horizontal field of view in degrees, if given.
		String projection; // "" (flat: the container says nothing), "equirectangular", "half_equirectangular", "fisheye", ...
		double projection_fov_h = 0.0; // Degrees covered (equirectangular: from the crop bounds); 0 if unknown.
		double projection_fov_v = 0.0;
		double projection_yaw = 0.0; // Degrees.
		double projection_pitch = 0.0;
		double projection_roll = 0.0;
		double projection_bounds[4] = {}; // Equirectangular crop: left, top, right, bottom, as fractions of the full sphere.
		AnnexBSplitter::SPSInfo sps; // H.264/HEVC: the first SPS (coded size, cropped size, bit depth).
		// HDR10 static metadata (SMPTE ST 2086, CTA-861.3): from the container, or else from the SEI of the first keyframe
		// (most rips carry it only there).
		bool mastering_primaries = false;
		double mastering_display[8] = {}; // CIE 1931 x, y of red, green, blue and the white point.
		bool mastering_luminance = false;
		double mastering_min_nits = 0.0;
		double mastering_max_nits = 0.0;
		bool light_level = false;
		int max_cll = 0; // Brightest pixel of the film in nits (0: not given).
		int max_fall = 0; // Brightest frame average in nits (0: not given).
		String hdr_metadata_source; // "container", "sei", "container+sei" or "".
		int dolby_vision_profile = -1; // From the container's configuration record; -1: none.
		bool hdr10plus = false; // HDR10+ dynamic metadata in the first keyframe's SEI.
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
	// One picture of a bitmap subtitle event, in the subtitle canvas's pixels.
	struct SubtitleBitmap {
		Rect2i rect;
		Ref<Image> image; // RGBA8, straight alpha.
		bool forced = false;
	};

	struct SubtitleEvent {
		double start = 0.0; // Media seconds.
		double end = -1.0; // -1: until the next event (bitmap tracks, or no duration in the container).
		String text;
		String ass;
		PackedByteArray data; // Bitmap tracks without a decoder: the raw packet.
		bool bitmap = false;
		LocalVector<SubtitleBitmap> bitmaps; // Decoded bitmap tracks; none: the screen is cleared.
		Size2i canvas; // The size the bitmaps' rects refer to.
		uint32_t serial = 0;
	};

private:
	static inline SafeFlag hw_10bit_available{ false }; // Set from the decoder thread too.
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
		bool commentary = false;
		bool visual_impaired = false; // Audio description.
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
		bool hearing_impaired = false;
		bool bitmap_decoded = false; // Bitmap track with a decoder: events carry images.
		String path; // External files: where it was loaded from.
	};

	struct Chapter {
		double start = 0.0; // Media seconds.
		double end = 0.0;
		String title;
	};

	// A subtitle file loaded next to the video: all its events, in memory (add_external_subtitle()).
	struct ExternalSubtitle {
		SubtitleTrack track;
		LocalVector<SubtitleEvent> events; // Sorted by start.
		String header;
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
		bool decoder_drc = false; // AC-3 / E-AC-3: the decoder applies the stream's own DRC in night mode.
		bool dialogue_boost_applies = false; // The audio has a centre channel beside front left and right.
		float audio_gain_db = 0.0f; // Night mode: leveller and compressor gain of the last block.
		float audio_limiter_db = 0.0f; // Limiter reduction in the last buffer (0 or negative).
		float audio_typical_level_db = 0.0f; // Night mode: the typical level the compressor works from.
		uint32_t audio_channel_mask = 0; // Which of the DISCRETE_CHANNELS slots the audio has (bit n: slot n).
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
	static constexpr uint32_t BITRATE_SECONDS = 128; // Seconds of video bytes kept (behind and ahead of the read position).
	static constexpr int DISCRETE_CHANNELS = 8; // 7.1 in FFmpeg's order: FL FR FC LFE BL BR SL SR.

	struct BitrateSecond {
		int64_t second = INT64_MIN; // Media second (floor of the PTS).
		uint32_t serial = 0; // Counted under this serial; others are stale.
		uint64_t bytes = 0;
	};

	// Settings: change them before open().
	double video_queue_seconds = 1.0;
	double audio_queue_seconds = 1.0;
	bool downmix_normalize = false;
	bool discrete_channels = false;
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
	LocalVector<Chapter> chapters;
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
	// Night mode and dialogue boost: change at any time; the thread picks them up when audio_dsp_serial changes.
	bool night_mode = false;
	double night_mode_strength = 1.0;
	double dialogue_boost = 0.0; // dB on the centre channel before the downmix.
	uint32_t audio_dsp_serial = 0;
	LocalVector<SubtitleEvent> subtitle_queue;
	String subtitle_header;
	LocalVector<ExternalSubtitle> external_subtitles; // Tracks subtitle_tracks.size() and up.
	uint32_t external_cursor = 0; // The next event of the selected external track to hand out.
	// read_subtitle_event() (consumer thread): the parsed header of the selected text track.
	String styled_header_source;
	SubtitleStyle::Header styled_header;
	uint32_t external_serial = 0; // The serial it was handed out under.
	double subtitle_read_ahead = 0.0;
	uint32_t subtitle_video_target = 0; // Video queue target while a subtitle stream is read (covers the read-ahead).
	BitrateSecond bitrate_seconds[BITRATE_SECONDS];
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
	// Playback speed (audio_mutex): the ring is read faster or slower through a time stretch that keeps the pitch.
	double playback_speed = 1.0;
	AudioTempo *tempo = nullptr; // Made the first time the speed isn't 1.
	bool tempo_active = false;
	uint32_t tempo_serial = 0;
	// discrete_channels (audio_mutex): the channels of the frames in the ring, DISCRETE_CHANNELS per frame. Readers: 0 is
	// the stereo mix, 1 + n channel n. All readers in one mix step of the audio server read the same frames, from
	// step_start; a step ends when a reader comes back for more, and then ring_read moves to step_end.
	LocalVector<float> channel_ring;
	uint32_t mix_step = 1;
	uint64_t step_start = 0;
	uint64_t step_end = 0;
	uint32_t step_serial = UINT32_MAX;
	uint32_t reader_step[1 + DISCRETE_CHANNELS] = {};

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
	double swr_center_level = 0.0; // The centre mix level swr was built with (dialogue boost).
	// Night mode and dialogue boost as the thread applies them.
	uint32_t dsp_serial = UINT32_MAX;
	bool dsp_night_mode = false;
	float dsp_strength = 1.0f;
	float dsp_dialogue_boost = 0.0f;
	AudioDynamics dynamics;
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
	bool hdr_sei_pending = false; // open(): HDR video without full metadata in the container; read the first keyframe's SEI.
	double audio_trim_until = -1.0;
	double audio_next_time = -1.0;
	// Compressed audio packets read while the ring is full (audio not played yet, or ahead of the video in the file), so the
	// video can still be read. Decoded in order once the ring has room.
	LocalVector<AVPacket *> audio_fifo;
	uint32_t audio_fifo_head = 0;
	uint32_t audio_fifo_count = 0;
	uint64_t audio_fifo_bytes = 0;
	LocalVector<float> scratch; // Interleaved stereo.
	LocalVector<float> channel_scratch; // discrete_channels: the same frames as 7.1, DISCRETE_CHANNELS per frame.
	LocalVector<float> gain_trace; // discrete_channels: the gain night mode gave each stereo frame, for the channels.
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
	// One decoded text subtitle packet as an event (p_start, p_duration: the packet's media time and duration, -1 if
	// none). False if it decoded to nothing; r_error set if the decoder failed.
	static bool _decode_text_subtitle(AVCodecContext *p_codec, AVPacket *p_packet, double p_start, double p_duration, SubtitleEvent &r_event, bool &r_error);
	bool _decode_bitmap_subtitle(AVPacket *p_packet, double p_start, double p_duration, SubtitleEvent &r_event, bool &r_error);
	static String _subtitle_file_to_utf8(const PackedByteArray &p_bytes, String &r_charset);
	static Error _read_whole_file(const String &p_path, int64_t p_max_bytes, PackedByteArray &r_bytes, String &r_error);
	void _read_stream_metadata(const AVStream *p_stream);
	void _read_hdr_side_data(const AVStream *p_stream);
	void _parse_hdr_sei(const uint8_t *p_data, uint64_t p_size, bool p_hevc);
	void _read_chapters();
	uint32_t _subtitle_video_target() const;
	void _count_video_bytes_locked(double p_pts, uint32_t p_bytes);
	static Dictionary _subtitle_track_to_dictionary(const SubtitleTrack &p_track, int p_index, bool p_external);

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
	void _apply_audio_decoder_settings();
	void _update_audio_dsp();
	void _decode_audio(const AVPacket *p_packet);
	void _audio_fifo_push(AVPacket *p_packet);
	AVPacket *_audio_fifo_pop();
	void _audio_fifo_clear();
	void _resample_frame(const AVFrame *p_frame);
	void _flush_scratch();
	uint32_t _mix_tempo_locked(AudioFrame *p_buffer, uint32_t p_frames);
	void _write_ring_locked(const float *p_samples, const float *p_channels, uint32_t p_frames, double p_time);
	void _downmix_discrete(float *p_channels, float *r_stereo, uint32_t p_frames);
	uint32_t _take_frames_locked(int p_reader, uint32_t p_frames, uint64_t &r_from, bool &r_first);
	void _mix(int p_reader, AudioFrame *p_buffer, int p_frames);

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

	// Chapters of the file: [{index, start, end, title}] in media seconds; empty if it has none.
	Array get_chapters() const;
	// The chapter playing at p_seconds, or -1 (before the first, or none).
	int get_chapter_at(double p_seconds) const;

	// Loads a subtitle file (SubRip, ASS/SSA, WebVTT; local or smb://) as one more subtitle track, after the file's own,
	// and returns its index (-1 on failure, get_last_error() says why). Blocking (reads the whole file). The language and
	// title default to what the file name says (MediaBrowser.parse_subtitle_name()). Selected like any other track; its
	// events are all handed out by read_subtitle_event() at once, again from the seek target after a seek.
	int add_external_subtitle(const String &p_path, const String &p_language = String(), const String &p_title = String());
	int get_external_subtitle_count() const;
	// Subtitle events of a file's own tracks come with the packets, about video_queue_seconds ahead of the playback. A
	// negative subtitle delay shows them earlier than that: set this to the largest such delay (seconds) and the video is
	// read that far ahead while a subtitle track is selected (within the queue's limits: 512 packets, 96 MiB).
	void set_subtitle_read_ahead(double p_seconds);
	double get_subtitle_read_ahead() const;

	// The parts of an ASS event line an app can draw without libass (SubtitleStyle): runs of styled text, alignment,
	// position, margins, script resolution. p_header: get_subtitle_header().
	static Dictionary parse_ass_event(const String &p_line, const String &p_header);

	// HDR to SDR: what a shader needs to show PQ or HLG video on an SDR display of p_target_nits peak (see the class
	// reference): the gamut matrix to BT.709, the PQ and HLG constants, and the BT.2390 EETF parameters.
	static Dictionary get_tone_mapping(const Dictionary &p_video_info, double p_target_nits = 203.0);
	// The same on the CPU, for one R'G'B' value (non-linear, as decoded): linear BT.709 RGB, 1.0 = the target peak.
	// For tests and to check a shader against; far too slow for pictures.
	static Color tone_map_color(const Color &p_rgb, const Dictionary &p_tone_mapping);

	// Language memory: "ger", "deu", "de" and "German" -> "de" (see MediaLanguages), and the track that fits a list of
	// preferred languages best (-1: none of them).
	static String normalize_language(const String &p_code);
	static String language_name(const String &p_code);
	int pick_audio_track(const PackedStringArray &p_languages) const;
	// p_forced_only: only forced tracks (the foreign parts of a film in the audio's language).
	int pick_subtitle_track(const PackedStringArray &p_languages, bool p_forced_only = false) const;

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
	// Video bitrate in kbit/s for each of the p_seconds whole media seconds before p_at (oldest first), from the sizes of
	// the packets read; -1 where unknown (not read since the last seek, or too long ago). For a graph in an overlay.
	PackedFloat32Array get_video_bitrate_history(int p_seconds, double p_at) const;
	int64_t get_bit_rate() const { return bit_rate; }

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
	// Whether the hardware path can show 10-bit (the decoder offers P010). V4L2Decoder sets it once it has looked;
	// get_video_info()'s hw_decodable uses it.
	static void set_hw_10bit_available(bool p_available) { hw_10bit_available.set_to(p_available); }
	static bool is_hw_10bit_available() { return hw_10bit_available.is_set(); }
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

	// 0.5 to 2: the audio plays faster or slower with its pitch kept (AudioTempo), and since the audio is the master
	// clock, the video follows. Any time, any thread; takes effect at the next mix. Without an audio track, set
	// HWVideoTexture.playback_speed instead.
	void set_playback_speed(double p_speed);
	double get_playback_speed() const;

	void set_video_queue_seconds(double p_seconds);
	double get_video_queue_seconds() const { return video_queue_seconds; }
	void set_audio_queue_seconds(double p_seconds);
	double get_audio_queue_seconds() const { return audio_queue_seconds; }
	// Night mode and dialogue boost: any time, also while playing (heard after the audio queue, about 1 s).
	void set_night_mode(bool p_enabled);
	bool is_night_mode() const;
	void set_night_mode_strength(double p_strength);
	double get_night_mode_strength() const;
	void set_dialogue_boost(double p_db);
	double get_dialogue_boost() const;

	// Discrete channels (set before open()): the audio is also kept as 7.1, one AudioStream per channel.
	void set_discrete_channels(bool p_enabled);
	bool is_discrete_channels() const { return discrete_channels; }
	Ref<AudioStreamMedia> get_audio_channel_stream(int p_channel);
	Array get_audio_channels() const;
	// AudioStreamPlaybackMedia of a channel stream: p_channel 0-7, as mono (left = right).
	void mix_audio_channel(AudioFrame *p_buffer, int p_frames, int p_channel);

	void set_downmix_normalize(bool p_normalize);
	bool is_downmix_normalize() const { return downmix_normalize; }
	void set_audio_decoder_options(const Dictionary &p_options);
	Dictionary get_audio_decoder_options() const { return audio_decoder_options; }

	~MediaSource();
};

VARIANT_ENUM_CAST(MediaSource::State);
