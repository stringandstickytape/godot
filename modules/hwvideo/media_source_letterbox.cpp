/**************************************************************************/
/*  media_source_letterbox.cpp                                            */
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

// MediaSource: letterbox detection. The hardware decoder's frames are UBWC-compressed buffers the CPU can't read, so a
// few keyframes go through FFmpeg's software decoder as well, one at a time on a thread of its own at a low priority,
// without the loop filter. Each picture's black bars are found by scanning luma rows and columns in from the edges.

#include "media_source.h"

#include "core/math/math_funcs.h"
#include "core/os/os.h"

#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixdesc.h>
}

// The average luma (8-bit steps above black) a picture needs to count: below it, a dark scene.
static constexpr int LETTERBOX_MIN_MEAN = 20;

static double letterbox_thread_cpu() {
	struct timespec ts;
	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

// The picture's rectangle in one decoded frame: rows, then columns, in from each edge until one has more than 2 % of its
// samples (and at least 2) above black + 10 (in 8-bit steps). About 480 samples a row and 270 a column, so a frame costs
// well under a millisecond. False when the frame is black, or what isn't black is under a quarter of its height (a
// title or a logo on black says nothing about the bars).
static bool letterbox_scan(const AVFrame *p_frame, Rect2i &r_picture) {
	const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get((AVPixelFormat)p_frame->format);
	if (desc == nullptr || (desc->flags & (AV_PIX_FMT_FLAG_HWACCEL | AV_PIX_FMT_FLAG_RGB)) || desc->nb_components < 1) {
		return false;
	}
	const int depth = desc->comp[0].depth;
	const int shift = desc->comp[0].shift;
	const int step = desc->comp[0].step;
	const int offset = desc->comp[0].offset;
	const int plane = desc->comp[0].plane;
	const uint8_t *base = p_frame->data[plane];
	const int pitch = p_frame->linesize[plane];
	const int width = p_frame->width;
	const int height = p_frame->height;
	if (base == nullptr || width < 16 || height < 16 || depth < 8 || depth > 16) {
		return false;
	}
	const bool full_range = p_frame->color_range == AVCOL_RANGE_JPEG;
	const int threshold = ((full_range ? 0 : 16) + 10) << (depth - 8);
	const auto sample = [&](int p_x, int p_y) -> int {
		const uint8_t *at = base + (int64_t)p_y * pitch + (int64_t)p_x * step + offset;
		if (depth > 8) {
			uint16_t v;
			memcpy(&v, at, sizeof(v));
			return v >> shift;
		}
		return *at;
	};
	const int x_step = MAX(1, width / 480);
	const int y_step = MAX(1, height / 270);
	const auto row_lit = [&](int p_y) {
		int lit = 0;
		int count = 0;
		for (int x = 0; x < width; x += x_step) {
			lit += sample(x, p_y) > threshold ? 1 : 0;
			count++;
		}
		return lit >= 2 && lit * 50 > count;
	};
	int top = 0;
	while (top < height && !row_lit(top)) {
		top++;
	}
	if (top == height) {
		return false;
	}
	int bottom = height - 1;
	while (bottom > top && !row_lit(bottom)) {
		bottom--;
	}
	if (bottom - top + 1 < height / 4) {
		return false;
	}
	const auto column_lit = [&](int p_x) {
		int lit = 0;
		int count = 0;
		for (int y = top; y <= bottom; y += y_step) {
			lit += sample(p_x, y) > threshold ? 1 : 0;
			count++;
		}
		return lit >= 2 && lit * 50 > count;
	};
	int left = 0;
	while (left < width && !column_lit(left)) {
		left++;
	}
	int right = width - 1;
	while (right > left && !column_lit(right)) {
		right--;
	}
	// A dark scene's edges look like bars: only a picture that is bright enough on average counts.
	int64_t sum = 0;
	int count = 0;
	for (int y = top; y <= bottom; y += y_step) {
		for (int x = left; x <= right; x += x_step) {
			sum += sample(x, y);
			count++;
		}
	}
	const int bright_enough = ((full_range ? 0 : 16) + LETTERBOX_MIN_MEAN) << (depth - 8);
	if (count == 0 || sum < (int64_t)bright_enough * count) {
		return false;
	}
	r_picture = Rect2i(left, top, right - left + 1, bottom - top + 1);
	return true;
}

// The frames' rectangles as one: each edge as far out as any frame's, except that with 4 frames or more the outermost
// one is left out (a logo or a flash over the bars). Inner values come from dark scenes and lose. Bars under 1 % of the
// size are encoder edges, not bars.
static Rect2i letterbox_combine(const LocalVector<Rect2i> &p_frames, const Size2i &p_size) {
	const uint32_t n = p_frames.size();
	const uint32_t skip = n >= 4 ? 1 : 0;
	LocalVector<int> tops, bottoms, lefts, rights;
	for (const Rect2i &r : p_frames) {
		tops.push_back(r.position.y);
		bottoms.push_back(r.position.y + r.size.y);
		lefts.push_back(r.position.x);
		rights.push_back(r.position.x + r.size.x);
	}
	std::sort(tops.ptr(), tops.ptr() + n);
	std::sort(bottoms.ptr(), bottoms.ptr() + n);
	std::sort(lefts.ptr(), lefts.ptr() + n);
	std::sort(rights.ptr(), rights.ptr() + n);
	int top = tops[skip];
	int bottom = bottoms[n - 1 - skip];
	int left = lefts[skip];
	int right = rights[n - 1 - skip];
	if (top < p_size.y / 100) {
		top = 0;
	}
	if (p_size.y - bottom < p_size.y / 100) {
		bottom = p_size.y;
	}
	if (left < p_size.x / 100) {
		left = 0;
	}
	if (p_size.x - right < p_size.x / 100) {
		right = p_size.x;
	}
	// Bars are centred. Two that differ by more than 1.5 % of the size are dark picture on one side: none on that axis
	// (cropping too little hides nothing; cropping too much does).
	if (Math::abs(top - (p_size.y - bottom)) > MAX(8, p_size.y * 3 / 200)) {
		top = 0;
		bottom = p_size.y;
	}
	if (Math::abs(left - (p_size.x - right)) > MAX(8, p_size.x * 3 / 200)) {
		left = 0;
		right = p_size.x;
	}
	return Rect2i(left, top, right - left, bottom - top);
}

// open(), after the stream is known: a software decoder for the video, and the thread. Nothing if the build has no
// decoder for it.
void MediaSource::_letterbox_start() {
	if (!letterbox_detection || video.stream_index < 0 || format == nullptr) {
		return;
	}
	const AVCodecParameters *par = format->streams[video.stream_index]->codecpar;
	const AVCodec *decoder = avcodec_find_decoder(par->codec_id);
	MutexLock lock(letterbox.mutex);
	if (decoder == nullptr) {
		letterbox.error = vformat("This build has no software %s decoder to look for black bars with.", video.codec_name);
		return;
	}
	letterbox.codec = avcodec_alloc_context3(decoder);
	letterbox.frame = av_frame_alloc();
	if (letterbox.codec == nullptr || letterbox.frame == nullptr) {
		avcodec_free_context(&letterbox.codec);
		av_frame_free(&letterbox.frame);
		letterbox.error = "Out of memory for the letterbox decoder.";
		return;
	}
	if (_is_annexb_codec()) {
		// Annex-B with the parameter sets put in front of each keyframe (_letterbox_offer), so no extradata.
		letterbox.codec->width = par->width;
		letterbox.codec->height = par->height;
	} else {
		avcodec_parameters_to_context(letterbox.codec, par);
	}
	letterbox.codec->thread_count = 1;
	letterbox.codec->skip_loop_filter = AVDISCARD_ALL; // Faster; a little blockier, which a black-bar scan doesn't mind.
	letterbox.codec->flags2 |= AV_CODEC_FLAG2_FAST | AV_CODEC_FLAG2_SHOW_ALL; // SHOW_ALL: open-GOP I pictures come out too.
	const int r = avcodec_open2(letterbox.codec, decoder, nullptr);
	if (r < 0) {
		avcodec_free_context(&letterbox.codec);
		av_frame_free(&letterbox.frame);
		letterbox.error = vformat("The letterbox decoder didn't open: %s.", _av_error(r));
		return;
	}
	letterbox.exit = false;
	letterbox_next_pts = -1.0;
	letterbox_last_pts = -1.0;
	letterbox_wanted.set();
	Thread::Settings settings;
	settings.priority = Thread::PRIORITY_LOW;
	letterbox.thread.start(&MediaSource::_letterbox_thread_func, this, settings);
}

// close(): ends the thread (it may be decoding a keyframe: at most that long) and frees the decoder.
void MediaSource::_letterbox_stop() {
	letterbox_wanted.clear();
	if (letterbox.thread.is_started()) {
		{
			MutexLock lock(letterbox.mutex);
			letterbox.exit = true;
			letterbox.condition.notify_all();
		}
		letterbox.thread.wait_to_finish();
	}
	MutexLock lock(letterbox.mutex);
	av_packet_free(&letterbox.offered);
	avcodec_free_context(&letterbox.codec);
	av_frame_free(&letterbox.frame);
	letterbox.frames.clear();
	letterbox.frame_times.clear();
	letterbox.frame_size = Size2i();
	letterbox.scanned = 0;
	letterbox.dark = 0;
	letterbox.done = false;
	letterbox.cpu_seconds = 0.0;
	letterbox.decode_seconds_max = 0.0;
	letterbox.rect = Rect2i();
	letterbox.error = String();
}

// Demux thread: a keyframe the letterbox thread may take. One copy per LETTERBOX_INTERVAL at most (and only while the
// thread wants more); skipped while it is still decoding the last one.
void MediaSource::_letterbox_offer(const AVPacket *p_packet, double p_pts) {
	MutexLock lock(letterbox.mutex);
	if (letterbox.offered != nullptr || letterbox.done || letterbox.exit) {
		return;
	}
	const bool annexb = _is_annexb_codec();
	const LocalVector<uint8_t> &sets = inband_parameter_sets.is_empty() ? config_parameter_sets : inband_parameter_sets;
	const int prefix = annexb ? (int)sets.size() : 0;
	AVPacket *copy = av_packet_alloc();
	if (copy == nullptr || av_new_packet(copy, prefix + p_packet->size) < 0) {
		av_packet_free(&copy);
		return;
	}
	if (prefix > 0) {
		memcpy(copy->data, sets.ptr(), prefix);
	}
	memcpy(copy->data + prefix, p_packet->data, p_packet->size);
	copy->flags = AV_PKT_FLAG_KEY;
	letterbox.offered = copy;
	letterbox.offered_pts = p_pts;
	letterbox_last_pts = p_pts;
	letterbox_next_pts = p_pts + (letterbox.scanned < LETTERBOX_SLOW_AFTER ? LETTERBOX_INTERVAL : LETTERBOX_SLOW_INTERVAL);
	letterbox.condition.notify_one();
}

void MediaSource::_letterbox_thread_func(void *p_userdata) {
	static_cast<MediaSource *>(p_userdata)->_letterbox_loop();
}

void MediaSource::_letterbox_loop() {
	// Below the demux, decoder and audio threads: the scan can wait, they can't.
	setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 10);
	for (;;) {
		AVPacket *packet = nullptr;
		double pts = 0.0;
		{
			MutexLock lock(letterbox.mutex);
			while (!letterbox.exit && letterbox.offered == nullptr) {
				letterbox.condition.wait(lock);
			}
			if (letterbox.exit) {
				return;
			}
			packet = letterbox.offered;
			pts = letterbox.offered_pts;
		}
		// The keyframe alone: send it, drain the decoder, take the picture, reset for the next one.
		const uint64_t start_usec = OS::get_singleton()->get_ticks_usec();
		bool got = false;
		bool lit = false;
		Rect2i picture;
		Size2i size;
		if (avcodec_send_packet(letterbox.codec, packet) >= 0) {
			avcodec_send_packet(letterbox.codec, nullptr);
			while (avcodec_receive_frame(letterbox.codec, letterbox.frame) >= 0) {
				if (!got) {
					got = true;
					size = Size2i(letterbox.frame->width, letterbox.frame->height);
					lit = letterbox_scan(letterbox.frame, picture);
				}
				av_frame_unref(letterbox.frame);
			}
		}
		avcodec_flush_buffers(letterbox.codec);
		const double seconds = (OS::get_singleton()->get_ticks_usec() - start_usec) / 1e6;

		MutexLock lock(letterbox.mutex);
		av_packet_free(&letterbox.offered);
		letterbox.scanned++;
		letterbox.cpu_seconds = letterbox_thread_cpu();
		letterbox.decode_seconds_max = MAX(letterbox.decode_seconds_max, seconds);
		if (got && size != letterbox.frame_size) {
			// The first picture, or a new size: start over.
			letterbox.frames.clear();
			letterbox.frame_times.clear();
			letterbox.frame_size = size;
		}
		if (got && lit) {
			letterbox.frames.push_back(picture);
			letterbox.frame_times.push_back(pts);
			if (letterbox.frames.size() >= LETTERBOX_KNOWN) {
				letterbox.rect = letterbox_combine(letterbox.frames, letterbox.frame_size);
			}
		} else {
			letterbox.dark++;
		}
		if (letterbox.frames.size() >= LETTERBOX_FRAMES || letterbox.scanned >= LETTERBOX_TRIES) {
			letterbox.done = true;
			letterbox_wanted.clear();
			return; // close() frees the decoder.
		}
	}
}

Dictionary MediaSource::get_letterbox() const {
	Dictionary d;
	MutexLock lock(letterbox.mutex);
	const bool known = letterbox.frames.size() >= LETTERBOX_KNOWN;
	const Rect2i full(Point2i(), letterbox.frame_size);
	const Rect2i rect = known ? letterbox.rect : full;
	d["enabled"] = letterbox_detection;
	d["known"] = known;
	d["done"] = letterbox.done;
	d["frame_size"] = Vector2i(letterbox.frame_size);
	d["rect"] = rect;
	d["uv_rect"] = letterbox.frame_size.x > 0 && letterbox.frame_size.y > 0 ? Rect2(Vector2(rect.position) / Vector2(letterbox.frame_size), Vector2(rect.size) / Vector2(letterbox.frame_size)) : Rect2(0, 0, 1, 1);
	d["bars"] = known && rect != full;
	d["picture_aspect"] = known && rect.size.y > 0 ? rect.size.x * video.sample_aspect_ratio / rect.size.y : 0.0;
	d["frames"] = letterbox.frames.size();
	Array times;
	for (double t : letterbox.frame_times) {
		times.push_back(t);
	}
	d["frame_times"] = times;
	d["scanned"] = letterbox.scanned;
	d["dark"] = letterbox.dark;
	d["cpu_seconds"] = letterbox.cpu_seconds;
	d["decode_seconds_max"] = letterbox.decode_seconds_max;
	d["error"] = letterbox.error;
	return d;
}
