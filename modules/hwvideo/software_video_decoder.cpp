/**************************************************************************/
/*  software_video_decoder.cpp                                            */
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

#include "software_video_decoder.h"

#include "core/os/os.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/pixdesc.h>
}

#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <cerrno>
#include <ctime>

static double _thread_cpu_now() {
	struct timespec ts;
	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static double _process_cpu_now() {
	struct timespec ts;
	clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static double _now() {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static String _av_error(int p_error) {
	char text[AV_ERROR_MAX_STRING_SIZE] = {};
	av_strerror(p_error, text, sizeof(text));
	return String::utf8(text);
}

String SoftwareVideoDecoder::get_unsupported_reason(const MediaSource::VideoInfo &p_info, int64_t p_max_pixels) {
	if (!p_info.valid) {
		return "There is no video stream.";
	}
	if (p_info.codec != MediaSource::VIDEO_CODEC_H264 && p_info.codec != MediaSource::VIDEO_CODEC_HEVC && p_info.codec != MediaSource::VIDEO_CODEC_VP9) {
		return vformat("%s video can't be decoded in software here (H.264, HEVC and VP9 only).", p_info.codec_name.to_upper());
	}
	if ((int64_t)p_info.width * p_info.height > p_max_pixels) {
		return vformat("%dx%d is too large to decode in software on the Frame (up to %d pixels, about 1080p).", p_info.width, p_info.height, p_max_pixels);
	}
	const int bit_depth = MAX(p_info.bit_depth, (int)p_info.sps.bit_depth);
	if (bit_depth > 10) {
		return vformat("%d-bit video can't be decoded here (8 and 10 bits only).", bit_depth);
	}
	return String();
}

Error SoftwareVideoDecoder::open(const Ref<MediaSource> &p_source, int p_threads, String &r_error) {
	close();
	ERR_FAIL_COND_V(p_source.is_null() || !p_source->is_open(), ERR_INVALID_PARAMETER);
	const MediaSource::VideoInfo &info = p_source->get_video_stream_info();
	const AVCodecID codec_id = info.codec == MediaSource::VIDEO_CODEC_H264 ? AV_CODEC_ID_H264 : (info.codec == MediaSource::VIDEO_CODEC_HEVC ? AV_CODEC_ID_HEVC : AV_CODEC_ID_VP9);
	const AVCodec *codec = avcodec_find_decoder(codec_id);
	if (codec == nullptr) {
		r_error = vformat("This build has no software %s decoder.", info.codec_name);
		return ERR_UNAVAILABLE;
	}
	context = avcodec_alloc_context3(codec);
	frame = av_frame_alloc();
	packet = av_packet_alloc();
	if (context == nullptr || frame == nullptr || packet == nullptr) {
		close();
		r_error = "Out of memory for the software decoder.";
		return ERR_OUT_OF_MEMORY;
	}
	// H.264 and HEVC arrive as Annex-B with the parameter sets in band (MediaSource converts them), so no extradata.
	context->width = info.width;
	context->height = info.height;
	context->pkt_timebase = AVRational{ 1, 1000000 };
	context->thread_count = CLAMP(p_threads, 1, 16);
	context->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
	const int err = avcodec_open2(context, codec, nullptr);
	if (err < 0) {
		r_error = vformat("Opening the software %s decoder failed: %s.", info.codec_name, _av_error(err));
		close();
		return ERR_CANT_OPEN;
	}

	source = p_source;
	fps = info.fps >= 1.0 && info.fps <= 240.0 ? info.fps : 30.0;
	serial = source->get_serial();
	{
		MutexLock lock(mutex);
		for (Slot &slot : slots) {
			slot.free = true;
		}
		ready.clear();
		generation = 0;
		frame_size = Size2i();
		ended = failed = false;
		error = String();
		frames_decoded = decode_errors = packets_fed = 0;
		thread_cpu_seconds = convert_seconds = 0.0;
	}
	codec_description = vformat("software %s (FFmpeg %s, %d threads)", info.codec_name, codec->name, context->thread_count);
	wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	source->set_video_wake_fd(wake_fd);
	stop.clear();
	thread.start(&SoftwareVideoDecoder::_thread_func, this);
	print_line(vformat("SoftwareVideoDecoder: %s %dx%d %d-bit, %.3f fps: %s.", info.codec_name, info.width, info.height, MAX(info.bit_depth, (int)info.sps.bit_depth), fps, codec_description));
	return OK;
}

void SoftwareVideoDecoder::close() {
	if (thread.is_started()) {
		stop.set();
		if (wake_fd >= 0) {
			const uint64_t one = 1;
			[[maybe_unused]] ssize_t written = write(wake_fd, &one, sizeof(one));
		}
		thread.wait_to_finish();
	}
	if (source.is_valid()) {
		source->set_video_wake_fd(-1); // Before the eventfd is closed: the source writes to it under its lock.
		source.unref();
	}
	if (wake_fd >= 0) {
		::close(wake_fd);
		wake_fd = -1;
	}
	if (context != nullptr) {
		avcodec_free_context(&context);
	}
	if (frame != nullptr) {
		av_frame_free(&frame);
	}
	if (packet != nullptr) {
		av_packet_free(&packet);
	}
	MutexLock lock(mutex);
	ready.clear();
	for (Slot &slot : slots) {
		slot.free = true;
	}
}

void SoftwareVideoDecoder::_thread_func(void *p_user) {
	static_cast<SoftwareVideoDecoder *>(p_user)->_run();
}

bool SoftwareVideoDecoder::_free_slot_available() {
	MutexLock lock(mutex);
	for (const Slot &slot : slots) {
		if (slot.free) {
			return true;
		}
	}
	return false;
}

// Blocks until the source has packets, a slot is returned, close() is called, or the timeout passes.
void SoftwareVideoDecoder::_wait(int p_timeout_ms) {
	struct pollfd fd = { wake_fd, POLLIN, 0 };
	if (poll(&fd, 1, p_timeout_ms) > 0) {
		uint64_t value = 0;
		[[maybe_unused]] ssize_t got = read(wake_fd, &value, sizeof(value));
	}
}

void SoftwareVideoDecoder::_fail(const String &p_error) {
	MutexLock lock(mutex);
	failed = true;
	error = p_error;
	ERR_PRINT("SoftwareVideoDecoder: " + p_error);
}

void SoftwareVideoDecoder::_run() {
	const double cpu_start = _thread_cpu_now();
	MediaSource::VideoPacket held;
	bool have_held = false;
	bool flushed = false; // The end of the input was sent (a null packet) for this serial.
	uint32_t feed_serial = serial;

	while (!stop.is_set()) {
		// Take every decoded frame there is room for.
		bool need_input = false;
		while (!stop.is_set() && _free_slot_available()) {
			const int got = avcodec_receive_frame(context, frame);
			if (got == 0) {
				_output_frame();
				av_frame_unref(frame);
				continue;
			}
			if (got == AVERROR(EAGAIN)) {
				need_input = true;
			} else if (got == AVERROR_EOF) {
				MutexLock lock(mutex);
				ended = true;
			} else {
				_fail(vformat("Decoding failed: %s.", _av_error(got)));
				stop.set();
			}
			break;
		}
		{
			MutexLock lock(mutex);
			thread_cpu_seconds = _thread_cpu_now() - cpu_start;
		}
		if (stop.is_set()) {
			break;
		}
		if (!need_input) {
			if (!is_ended()) {
				_wait(100); // Every slot is held by the consumer: wait for one to come back.
				continue;
			}
			if (source->get_serial() == feed_serial) {
				_wait(100); // Drained to the end: wait for a seek.
				continue;
			}
			// A seek after the end: feed the packets after it (the first one flushes the decoder).
		}

		if (!have_held) {
			if (!source->pop_video_packet(held)) {
				if (!flushed && source->is_video_input_done(feed_serial)) {
					avcodec_send_packet(context, nullptr); // Drain: the last frames come out, then AVERROR_EOF.
					flushed = true;
					continue;
				}
				_wait(100);
				continue;
			}
			have_held = true;
		}
		if (held.serial != source->get_serial()) {
			source->release_video_packet(held); // Read before a seek that has happened since.
			have_held = false;
			continue;
		}
		if (held.serial != feed_serial) {
			// The first packet after a seek: drop what the decoder holds from before it.
			avcodec_flush_buffers(context);
			feed_serial = held.serial;
			flushed = false;
			MutexLock lock(mutex);
			serial = feed_serial;
			ended = false;
		}

		// The source's AVPacket is refcounted: send it as it is when its data wasn't rewritten (no copy).
		const bool rewritten = held.packet == nullptr || held.data != held.packet->data || (int)held.size != held.packet->size;
		AVPacket *send = held.packet;
		if (rewritten) {
			av_packet_unref(packet);
			if (av_new_packet(packet, (int)held.size) < 0) {
				_fail("Out of memory for a packet.");
				break;
			}
			memcpy(packet->data, held.data, held.size);
			if (held.keyframe) {
				packet->flags |= AV_PKT_FLAG_KEY;
			}
			send = packet;
		}
		send->pts = (int64_t)llround(held.pts * 1e6);
		send->dts = (int64_t)llround(held.dts * 1e6);
		const int sent = avcodec_send_packet(context, send);
		if (rewritten) {
			av_packet_unref(packet);
		}
		if (sent == AVERROR(EAGAIN)) {
			continue; // Frames must be taken first; the packet stays held.
		}
		source->release_video_packet(held);
		have_held = false;
		MutexLock lock(mutex);
		packets_fed++;
		if (sent < 0 && sent != AVERROR_EOF) {
			decode_errors++; // Corrupt data: the decoder conceals and goes on.
		}
	}
	if (have_held) {
		source->release_video_packet(held);
	}
}

// Converts the decoded frame to 8-bit NV12 in a free slot and queues it.
bool SoftwareVideoDecoder::_output_frame() {
	const double t_start = _now();
	const int format = frame->format;
	const bool ten_bit = format == AV_PIX_FMT_YUV420P10LE;
	if (format != AV_PIX_FMT_YUV420P && format != AV_PIX_FMT_YUVJ420P && !ten_bit) {
		const char *name = av_get_pix_fmt_name((AVPixelFormat)format);
		_fail(vformat("The decoder gives %s pictures; only 4:2:0 at 8 or 10 bits can be shown.", name ? name : "unknown"));
		stop.set();
		return false;
	}
	const int width = frame->width & ~1;
	const int height = frame->height & ~1;
	const bool range = format == AV_PIX_FMT_YUVJ420P || frame->color_range == AVCOL_RANGE_JPEG;

	uint32_t index = SLOT_COUNT;
	{
		MutexLock lock(mutex);
		for (uint32_t i = 0; i < SLOT_COUNT; i++) {
			if (slots[i].free) {
				index = i;
				break;
			}
		}
		if (index == SLOT_COUNT) {
			return false; // Can't happen: the caller checked.
		}
		slots[index].free = false;
		if (Size2i(width, height) != frame_size || range != full_range) {
			frame_size = Size2i(width, height);
			full_range = range;
			generation++;
		}
	}

	// The slot is ours until it is queued. Its Vectors are only reallocated when the size changes.
	Slot &slot = slots[index];
	const int64_t y_size = (int64_t)width * height;
	const int64_t uv_size = (int64_t)width * (height / 2);
	if (slot.y.size() != y_size) {
		slot.y.resize(y_size);
	}
	if (slot.uv.size() != uv_size) {
		slot.uv.resize(uv_size);
	}
	slot.size = Size2i(width, height);
	uint8_t *y_out = slot.y.ptrw();
	uint8_t *uv_out = slot.uv.ptrw();
	const int chroma_width = width / 2;
	if (ten_bit) {
		for (int row = 0; row < height; row++) {
			const uint16_t *in = (const uint16_t *)(frame->data[0] + (int64_t)row * frame->linesize[0]);
			uint8_t *out = y_out + (int64_t)row * width;
			for (int x = 0; x < width; x++) {
				out[x] = (uint8_t)MIN((in[x] + 2) >> 2, 255);
			}
		}
		for (int row = 0; row < height / 2; row++) {
			const uint16_t *u = (const uint16_t *)(frame->data[1] + (int64_t)row * frame->linesize[1]);
			const uint16_t *v = (const uint16_t *)(frame->data[2] + (int64_t)row * frame->linesize[2]);
			uint8_t *out = uv_out + (int64_t)row * width;
			for (int x = 0; x < chroma_width; x++) {
				out[2 * x] = (uint8_t)MIN((u[x] + 2) >> 2, 255);
				out[2 * x + 1] = (uint8_t)MIN((v[x] + 2) >> 2, 255);
			}
		}
	} else {
		for (int row = 0; row < height; row++) {
			memcpy(y_out + (int64_t)row * width, frame->data[0] + (int64_t)row * frame->linesize[0], width);
		}
		for (int row = 0; row < height / 2; row++) {
			const uint8_t *u = frame->data[1] + (int64_t)row * frame->linesize[1];
			const uint8_t *v = frame->data[2] + (int64_t)row * frame->linesize[2];
			uint8_t *out = uv_out + (int64_t)row * width;
			for (int x = 0; x < chroma_width; x++) {
				out[2 * x] = u[x];
				out[2 * x + 1] = v[x];
			}
		}
	}

	double time = 0.0;
	const int64_t pts = frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp : frame->pts;
	MutexLock lock(mutex);
	time = pts != AV_NOPTS_VALUE ? pts * 1e-6 : last_time + 1.0 / fps;
	last_time = time;
	Frame out;
	out.slot = index;
	out.serial = serial;
	out.generation = generation;
	out.time = time;
	// Presentation order: frame threading returns frames in order; keep it sorted anyway (a handful of entries).
	uint32_t at = ready.size();
	while (at > 0 && ready[at - 1].time > time) {
		at--;
	}
	ready.insert(at, out);
	frames_decoded++;
	convert_seconds += _now() - t_start;
	return true;
}

bool SoftwareVideoDecoder::pop_frame(Frame &r_frame) {
	MutexLock lock(mutex);
	if (ready.is_empty()) {
		return false;
	}
	r_frame = ready[0];
	ready.remove_at(0);
	return true;
}

void SoftwareVideoDecoder::return_slot(uint32_t p_slot) {
	ERR_FAIL_UNSIGNED_INDEX(p_slot, SLOT_COUNT);
	{
		MutexLock lock(mutex);
		slots[p_slot].free = true;
	}
	if (wake_fd >= 0) {
		const uint64_t one = 1;
		[[maybe_unused]] ssize_t written = write(wake_fd, &one, sizeof(one));
	}
}

void SoftwareVideoDecoder::return_all_ready() {
	{
		MutexLock lock(mutex);
		for (const Frame &f : ready) {
			slots[f.slot].free = true;
		}
		ready.clear();
	}
	if (wake_fd >= 0) {
		const uint64_t one = 1;
		[[maybe_unused]] ssize_t written = write(wake_fd, &one, sizeof(one));
	}
}

bool SoftwareVideoDecoder::is_full_range() const {
	MutexLock lock(mutex);
	return full_range;
}

bool SoftwareVideoDecoder::is_ended() const {
	MutexLock lock(mutex);
	return ended;
}

bool SoftwareVideoDecoder::has_failed() const {
	MutexLock lock(mutex);
	return failed;
}

String SoftwareVideoDecoder::get_error() const {
	MutexLock lock(mutex);
	return error;
}

Dictionary SoftwareVideoDecoder::get_stats() const {
	Dictionary d;
	MutexLock lock(mutex);
	d["software_frames_decoded"] = frames_decoded;
	d["software_decode_errors"] = decode_errors;
	d["software_packets_fed"] = packets_fed;
	d["software_thread_cpu_seconds"] = thread_cpu_seconds;
	d["software_convert_seconds"] = convert_seconds;
	d["process_cpu_seconds"] = _process_cpu_now();
	d["software_threads"] = context ? context->thread_count : 0;
	d["frames_waiting"] = ready.size();
	uint32_t in_use = 0;
	for (const Slot &slot : slots) {
		in_use += slot.free ? 0 : 1;
	}
	d["software_slots_in_use"] = in_use;
	return d;
}

SoftwareVideoDecoder::~SoftwareVideoDecoder() {
	close();
}
