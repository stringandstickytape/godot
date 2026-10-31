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

// The fallback for what the hardware path can't show (10-bit without P010, 4:2:2 H.264/HEVC, H.264 High 10, a second VP9
// session, VP9 after a decoder fault), at 1080p and below:
// FFmpeg's software decoders (with NEON and frame threads) on a thread of their own, fed from a MediaSource. Every frame
// is converted to 8-bit NV12 in one of a fixed pool of slots (no allocations in steady state), which HWVideoTexture
// uploads to two textures, so the same NV12 shader shows it. 10-bit is rounded to 8 bits; HDR (PQ) isn't tone-mapped.
class SoftwareVideoDecoder {
public:
	static constexpr uint32_t SLOT_COUNT = 8;
	// The largest frame decoded in software by default: 1920x1088.
	static constexpr int64_t DEFAULT_MAX_PIXELS = 1920 * 1088;

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
	double thread_cpu_seconds = 0.0;
	double convert_seconds = 0.0;
	double last_time = 0.0;
	double fps = 30.0;

	static void _thread_func(void *p_user);
	void _run();
	bool _free_slot_available();
	void _wait(int p_timeout_ms);
	bool _output_frame();
	void _fail(const String &p_error);

public:
	// Empty if this stream can be decoded here (codec, bit depth, chroma, size up to p_max_pixels); else why not.
	static String get_unsupported_reason(const MediaSource::VideoInfo &p_info, int64_t p_max_pixels);

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
