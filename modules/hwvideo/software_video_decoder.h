/**************************************************************************/
/*  software_video_decoder.h                                              */
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

#include "media_source.h"

#include "core/os/mutex.h"
#include "core/os/thread.h"
#include "core/templates/local_vector.h"
#include "core/templates/safe_refcount.h"
#include "core/variant/variant.h"

struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;

// Everything the hardware decoder doesn't take: every codec FFmpeg decodes (MPEG-4 part 2 / DivX / Xvid, MS-MPEG4,
// MPEG-1/2, H.263, WMV/VC-1, RealVideo, FLV, Theora, VP8, MJPEG, ... and AV1 through dav1d), and what the hardware path
// can't show (10-bit without P010, 4:2:2 H.264/HEVC, H.264 High 10, a second VP9 session, VP9 after a decoder fault),
// up to a size per codec (get_max_pixels()).
// FFmpeg's software decoders (with NEON and frame threads) on a thread of their own, fed from a MediaSource. Every frame
// is converted to 8-bit NV12 in one of a fixed pool of slots (no allocations in steady state), which HWVideoTexture
// uploads to two textures, so the same NV12 shader shows it. 4:2:0 at 8 bits is copied; 10-bit 4:2:0 is rounded to 8
// bits; anything else (palette, RGB, gray, 4:1:0, 4:1:1, 4:2:2, 4:4:4, other depths, a full-range picture in a stream
// described as limited) goes through swscale, to the matrix and range the stream's video info gives the shader. HDR (PQ)
// isn't tone-mapped here (the shader does it).
class SoftwareVideoDecoder {
public:
	static constexpr uint32_t SLOT_COUNT = 8;
	// The largest frame decoded in software by default: 1920x1088.
	static constexpr int64_t DEFAULT_MAX_PIXELS = 1920 * 1088;
	// AV1 (dav1d, the fastest of the software decoders, NEON, many threads) may go further: pixels per second up to 4K at
	// 30 fps (2560x1440 at 60), never less than the general limit. See docs/decisions.md D15.
	static constexpr int64_t AV1_MAX_PIXELS = 3840 * 2176;
	static constexpr double AV1_MAX_PIXEL_RATE = 3840.0 * 2160.0 * 30.0;

	struct Frame {
		uint32_t slot = 0;
		uint32_t serial = 0;
		uint32_t generation = 0; // Bumped when the frame size or range changes.
		double time = 0.0;
	};

	struct Slot {
		Vector<uint8_t> y; // width x height bytes.
		Vector<uint8_t> uv; // (width / 2) x (height / 2) Cb Cr pairs.
		Size2i size;
		bool free = true;
	};

private:
	Ref<MediaSource> source;
	AVCodecContext *context = nullptr;
	AVFrame *frame = nullptr;
	AVPacket *packet = nullptr;
	// swscale (decode thread only): its context is rebuilt when the input format or size changes.
	SwsContext *sws = nullptr;
	int sws_format = -1;
	Size2i sws_size;
	bool sws_source_full = false;
	int target_matrix = 0; // SWS_CS_*: the Y'CbCr matrix the shader will use (the video info's, as the app reads it).
	bool target_full_range = false; // The video info says "pc": the shader expands nothing.
	int last_format_id = -1; // Decode thread.
	static constexpr uint32_t ERRORS_IN_ROW_MAX = 100; // Decoding errors in a row before the decoder gives up.
	uint32_t errors_in_row = 0; // Decode thread.
	bool hide_until_key = false; // Decode thread: a guessed keyframe was fed; hide pictures until one is a keyframe.
	uint32_t hidden_since_guess = 0;
	String last_format; // The decoded pixel format, and how it is converted (stats, under the mutex).
	String conversion;
	Thread thread;
	SafeFlag stop;
	int wake_fd = -1; // The source writes to it when packets arrive; we write to it when a slot is returned or on close.

	mutable Mutex mutex;
	Slot slots[SLOT_COUNT];
	LocalVector<Frame> ready; // Decoded, in presentation order, not yet popped.
	uint32_t generation = 0;
	Size2i frame_size;
	bool full_range = false;
	bool ended = false;
	bool failed = false;
	String error;
	String codec_description;
	uint32_t serial = 0;

	// Stats (written by the decode thread under the mutex).
	uint64_t frames_decoded = 0;
	uint64_t decode_errors = 0;
	uint64_t packets_fed = 0;
	uint64_t frames_interlaced = 0; // Pictures the decoder flagged as interlaced (two fields)...
	uint64_t frames_top_first = 0; // ...with the top field first.
	uint64_t frames_converted = 0; // Through swscale.
	uint64_t frames_guessed_time = 0; // No timestamp at all: the last one plus a frame.
	uint64_t frames_hidden = 0; // After a guessed keyframe, until a real one.
	double thread_cpu_seconds = 0.0;
	double convert_seconds = 0.0;
	double last_time = 0.0;
	double fps = 30.0;

	static void _thread_func(void *p_user);
	void _run();
	bool _free_slot_available();
	void _wait(int p_timeout_ms);
	bool _output_frame();
	bool _convert(Slot &r_slot, int p_width, int p_height);
	void _fail(const String &p_error);

public:
	// Empty if this stream can be decoded here (a decoder in this build, size up to get_max_pixels()); else why not.
	static String get_unsupported_reason(const MediaSource::VideoInfo &p_info, int64_t p_max_pixels);
	// The largest frame (pixels) decoded in software for this stream: p_max_pixels (HWVideoTexture.software_max_pixels,
	// 1920x1088 by default) for every codec, more for AV1 (see AV1_MAX_PIXEL_RATE).
	static int64_t get_max_pixels(const MediaSource::VideoInfo &p_info, int64_t p_max_pixels);

	Error open(const Ref<MediaSource> &p_source, int p_threads, String &r_error);
	void close();
	bool is_open() const { return context != nullptr; }

	// Called from the consumer's thread.
	bool pop_frame(Frame &r_frame);
	// The slot's data may be written again; the consumer must be done with it (and with any GPU upload from it).
	void return_slot(uint32_t p_slot);
	// Discards the frames decoded but not popped yet (after a seek), returning their slots.
	void return_all_ready();
	const Slot &get_slot(uint32_t p_slot) const { return slots[p_slot]; }
	bool is_full_range() const;
	bool is_ended() const;
	bool has_failed() const;
	String get_error() const;
	String get_description() const { return codec_description; }
	Dictionary get_stats() const;

	~SoftwareVideoDecoder();
};
