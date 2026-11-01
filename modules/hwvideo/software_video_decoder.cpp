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
#include <libswscale/swscale.h>
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

// A size limit in words: "1920x1088", "3840x2176" when it is one of those, else the pixel count.
static String _describe_pixels(int64_t p_pixels) {
	static const int sizes[][2] = { { 1920, 1088 }, { 1920, 1080 }, { 2560, 1440 }, { 3840, 2160 }, { 3840, 2176 }, { 1280, 720 } };
	for (const auto &size : sizes) {
		if ((int64_t)size[0] * size[1] == p_pixels) {
			return vformat("%dx%d", size[0], size[1]);
		}
	}
	return vformat("%d pixels", p_pixels);
}

int64_t SoftwareVideoDecoder::get_max_pixels(const MediaSource::VideoInfo &p_info, int64_t p_max_pixels) {
	if (p_info.codec != MediaSource::VIDEO_CODEC_AV1) {
		return p_max_pixels;
	}
	const double fps = p_info.fps >= 1.0 && p_info.fps <= 240.0 ? p_info.fps : 30.0;
	const int64_t av1 = MIN(AV1_MAX_PIXELS, (int64_t)(AV1_MAX_PIXEL_RATE / MAX(fps, 30.0)));
	return MAX(p_max_pixels, av1);
}

String SoftwareVideoDecoder::get_unsupported_reason(const MediaSource::VideoInfo &p_info, int64_t p_max_pixels) {
	if (!p_info.valid) {
		return "There is no video stream.";
	}
	const String name = MediaSource::get_codec_display_name(p_info);
	if (p_info.codec_id == AV_CODEC_ID_NONE) {
		return vformat("The video codec%s isn't known to FFmpeg, so it can't be decoded.", p_info.codec_tag.is_empty() ? String() : " " + p_info.codec_tag);
	}
	if (!p_info.software_decoder) {
		return vformat("%s video can't be decoded in software: this build has no decoder for it.", name);
	}
	const int64_t max_pixels = get_max_pixels(p_info, p_max_pixels);
	if ((int64_t)p_info.width * p_info.height > max_pixels) {
		if (p_info.codec == MediaSource::VIDEO_CODEC_AV1) {
			return vformat("%dx%d AV1 at %.0f fps is too much to decode in software on the Frame (up to %s at 30 fps, 2560x1440 at 60).", p_info.width, p_info.height, p_info.fps, _describe_pixels(AV1_MAX_PIXELS));
		}
		return vformat("%dx%d %s is too large to decode in software on the Frame (up to %s).", p_info.width, p_info.height, name, _describe_pixels(max_pixels));
	}
	return String();
}

Error SoftwareVideoDecoder::open(const Ref<MediaSource> &p_source, int p_threads, String &r_error) {
	close();
	ERR_FAIL_COND_V(p_source.is_null() || !p_source->is_open(), ERR_INVALID_PARAMETER);
	const MediaSource::VideoInfo &info = p_source->get_video_stream_info();
	const AVCodecParameters *par = p_source->get_video_codec_parameters();
	// AV1: dav1d (FFmpeg's own av1 decoder only drives hardware decoders).
	const AVCodec *codec = info.codec == MediaSource::VIDEO_CODEC_AV1 ? avcodec_find_decoder_by_name("libdav1d") : avcodec_find_decoder((AVCodecID)info.codec_id);
	if (codec == nullptr || par == nullptr) {
		r_error = vformat("This build has no software %s decoder.", MediaSource::get_codec_display_name(info));
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
	if (info.codec == MediaSource::VIDEO_CODEC_H264 || info.codec == MediaSource::VIDEO_CODEC_HEVC) {
		// H.264 and HEVC arrive as Annex-B with the parameter sets in band (MediaSource converts them), so no extradata.
		context->width = info.width;
		context->height = info.height;
	} else {
		// The rest need what the container says: extradata (MPEG-4 VOL headers, WMV and VC-1 sequence headers, RealVideo,
		// Theora and VP3 headers, Cinepak, ...), the codec tag (MS-MPEG4 versions, DivX quirks), the size, bits per sample.
		const int r = avcodec_parameters_to_context(context, par);
		if (r < 0) {
			r_error = vformat("The %s codec parameters can't be used: %s.", info.codec_name, _av_error(r));
			close();
			return ERR_CANT_OPEN;
		}
	}
	context->pkt_timebase = AVRational{ 1, 1000000 };
	// Frame threads (each thread decodes a picture of its own: a few frames more latency, the CPU's cores used) where the
	// decoder has them, else slice threads; dav1d takes the count for its own scheduler.
	context->thread_count = CLAMP(p_threads, 1, 16);
	context->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
	const int err = avcodec_open2(context, codec, nullptr);
	if (err < 0) {
		r_error = vformat("Opening the software %s decoder failed: %s.", info.codec_name, _av_error(err));
		close();
		return ERR_CANT_OPEN;
	}
	// The matrix and range the shader will use, from the video info as the app reads it (scripts/play/hdr_tone_map.gd):
	// pictures that need converting are converted to these.
	const String matrix = info.color_matrix.to_lower();
	const bool hdr = info.color_transfer == "smpte2084" || info.color_transfer == "arib-std-b67";
	const bool bt2020_primaries = info.color_primaries == "bt2020" || (info.color_primaries.is_empty() && hdr);
	if (matrix.begins_with("bt2020")) {
		target_matrix = SWS_CS_BT2020;
	} else if (matrix == "bt470bg" || matrix == "smpte170m" || matrix == "bt601") {
		target_matrix = SWS_CS_ITU601;
	} else if (matrix.is_empty() || matrix == "unknown" || matrix == "unspecified") {
		target_matrix = bt2020_primaries ? SWS_CS_BT2020 : (info.height > 0 && info.height <= 576 ? SWS_CS_ITU601 : SWS_CS_ITU709);
	} else {
		target_matrix = SWS_CS_ITU709;
	}
	target_full_range = info.color_range == "pc";
	sws_format = -1;
	errors_in_row = 0;
	hide_until_key = false;
	hidden_since_guess = 0;
	last_format_id = -1;
	last_format = String();
	conversion = String();

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
		frames_interlaced = frames_top_first = frames_converted = frames_guessed_time = frames_hidden = 0;
		thread_cpu_seconds = convert_seconds = 0.0;
		full_range = target_full_range;
	}
	const bool frame_threads = (codec->capabilities & AV_CODEC_CAP_FRAME_THREADS) && (context->active_thread_type & FF_THREAD_FRAME);
	codec_description = vformat("software %s (FFmpeg %s, %d %s threads)", info.codec_name, codec->name, context->thread_count,
			frame_threads ? "frame" : ((context->active_thread_type & FF_THREAD_SLICE) ? "slice" : "own"));
	wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	source->set_video_wake_fd(wake_fd);
	stop.clear();
	thread.start(&SoftwareVideoDecoder::_thread_func, this);
	print_line(vformat("SoftwareVideoDecoder: %s %dx%d %s %d-bit, %.3f fps, SAR %.4f%s: %s.", MediaSource::get_codec_display_name(info), info.width, info.height, info.pixel_format,
			MAX(info.bit_depth, (int)info.sps.bit_depth), fps, info.sample_aspect_ratio, info.field_order.is_empty() ? String() : ", fields " + info.field_order, codec_description));
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
	if (sws != nullptr) {
		sws_freeContext(sws);
		sws = nullptr;
	}
	sws_format = -1;
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
	// After open or a seek the decoder starts at the keyframe before the target and everything before the target is
	// thrown away: pictures nothing refers to (most B-frames) aren't decoded until then. HWVIDEO_SW_SEEK_NONREF=0 turns
	// that off (A/B).
	const bool skip_nonref = OS::get_singleton()->get_environment("HWVIDEO_SW_SEEK_NONREF") != "0";
	double skip_until = skip_nonref ? source->get_seek_target() - 2.0 / MAX(fps, 1.0) : -1.0;

	while (!stop.is_set()) {
		// Take every decoded frame there is room for.
		bool need_input = false;
		while (!stop.is_set() && _free_slot_available()) {
			const int got = avcodec_receive_frame(context, frame);
			if (got == 0) {
				errors_in_row = 0;
				_output_frame();
				av_frame_unref(frame);
				continue;
			}
			if (got == AVERROR(EAGAIN)) {
				need_input = true;
				errors_in_row = 0;
			} else if (got == AVERROR_EOF) {
				MutexLock lock(mutex);
				ended = true;
			} else if (++errors_in_row < ERRORS_IN_ROW_MAX) {
				// A damaged picture (old files often have some; frame threads report it here): the decoder conceals it and
				// goes on with the next.
				MutexLock lock(mutex);
				decode_errors++;
				continue;
			} else {
				_fail(vformat("Decoding failed %d times in a row: %s.", errors_in_row, _av_error(got)));
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
			hide_until_key = false;
			skip_until = skip_nonref ? source->get_seek_target() - 2.0 / MAX(fps, 1.0) : -1.0;
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
		if (held.keyframe_guessed) {
			// Maybe not a keyframe after all (MediaSource couldn't tell): what is decoded from it is hidden until a picture
			// is one.
			hide_until_key = true;
			hidden_since_guess = 0;
		}
		const AVDiscard discard = held.pts < skip_until ? AVDISCARD_NONREF : AVDISCARD_DEFAULT;
		if (context->skip_frame != discard) {
			context->skip_frame = discard;
		}
		// Only the timestamps the container gave: where it gave a DTS but no PTS (AVI, some MPEG-PS frames), the decoder
		// works out the presentation time from the DTS (best_effort_timestamp), which is right after B-frames reorder;
		// MediaSource's guess (the DTS itself) would be in decode order. With neither (raw streams), the guess it is.
		send->pts = held.pts_valid || !held.dts_valid ? (int64_t)llround(held.pts * 1e6) : AV_NOPTS_VALUE;
		send->dts = held.dts_valid ? (int64_t)llround(held.dts * 1e6) : AV_NOPTS_VALUE;
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

// Any other picture format to NV12 at the stream's matrix and range, through swscale (decode thread). False on failure.
bool SoftwareVideoDecoder::_convert(Slot &r_slot, int p_width, int p_height) {
	const AVPixelFormat format = (AVPixelFormat)frame->format;
	const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(format);
	const bool rgb = desc != nullptr && (desc->flags & (AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_PAL)) != 0;
	const bool source_full = rgb || format == AV_PIX_FMT_YUVJ420P || format == AV_PIX_FMT_YUVJ422P || format == AV_PIX_FMT_YUVJ444P ||
			format == AV_PIX_FMT_YUVJ440P || format == AV_PIX_FMT_YUVJ411P || frame->color_range == AVCOL_RANGE_JPEG;
	if (sws == nullptr || sws_format != (int)format || sws_size != Size2i(p_width, p_height) || sws_source_full != source_full) {
		if (sws != nullptr) {
			sws_freeContext(sws);
			sws = nullptr;
		}
		sws_format = -1;
		const char *name = av_get_pix_fmt_name(format);
		if (!sws_isSupportedInput(format)) {
			_fail(vformat("The decoder gives %s pictures, which can't be converted for display.", name ? name : "unknown"));
			return false;
		}
		// Same size in and out: only chroma is resampled (and, for RGB and palette pictures, converted with the matrix).
		sws = sws_getContext(p_width, p_height, format, p_width, p_height, AV_PIX_FMT_NV12, SWS_BILINEAR, nullptr, nullptr, nullptr);
		if (sws == nullptr) {
			_fail(vformat("Converting %s pictures (%dx%d) for display isn't possible.", name ? name : "unknown", p_width, p_height));
			return false;
		}
		const int *table = sws_getCoefficients(target_matrix);
		sws_setColorspaceDetails(sws, table, source_full ? 1 : 0, table, target_full_range ? 1 : 0, 0, 1 << 16, 1 << 16);
		sws_format = (int)format;
		sws_size = Size2i(p_width, p_height);
		sws_source_full = source_full;
		static const char *matrices[] = { "", "BT.709", "", "", "", "BT.601", "BT.601", "SMPTE 240M", "", "BT.2020" };
		const String conversion_text = vformat("swscale %s%s to NV12 (%s, %s range)", name ? name : "?", source_full && !rgb ? " full range" : "",
				target_matrix >= 0 && target_matrix < 10 ? String(matrices[target_matrix]) : String("?"), target_full_range ? "full" : "limited");
		print_line("SoftwareVideoDecoder: " + conversion_text + ".");
		MutexLock lock(mutex);
		conversion = conversion_text;
	}
	uint8_t *dst[4] = { r_slot.y.ptrw(), r_slot.uv.ptrw(), nullptr, nullptr };
	const int dst_stride[4] = { p_width, p_width, 0, 0 };
	const int rows = sws_scale(sws, frame->data, frame->linesize, 0, p_height, dst, dst_stride);
	if (rows <= 0) {
		_fail("Converting a picture for display failed.");
		return false;
	}
	return true;
}

// Converts the decoded frame to 8-bit NV12 in a free slot and queues it.
bool SoftwareVideoDecoder::_output_frame() {
	const double t_start = _now();
	const int format = frame->format;
	// Odd sizes lose their last column or row (NV12's chroma is a 2x2 block); FFmpeg's frames are already cropped.
	const int width = frame->width & ~1;
	const int height = frame->height & ~1;
	if (width <= 0 || height <= 0 || format < 0) {
		return false; // A picture with nothing in it (a skipped frame of some old codecs).
	}
	if (hide_until_key) {
		const bool key = (frame->flags & AV_FRAME_FLAG_KEY) || frame->pict_type == AV_PICTURE_TYPE_I;
		if (!key && hidden_since_guess < (uint32_t)MAX(fps * 3.0, 1.0)) {
			hidden_since_guess++;
			MutexLock lock(mutex);
			frames_hidden++;
			return false;
		}
		hide_until_key = false; // A keyframe, or 3 s without one: show what there is.
	}
	const bool frame_full = format == AV_PIX_FMT_YUVJ420P || frame->color_range == AVCOL_RANGE_JPEG;
	// 4:2:0 at 8 bits (most of everything) is copied, 10-bit 4:2:0 rounded; both only when the range is the one the
	// shader uses. The rest goes through swscale.
	const bool copy_8bit = (format == AV_PIX_FMT_YUV420P || format == AV_PIX_FMT_YUVJ420P) && frame_full == target_full_range;
	const bool ten_bit = format == AV_PIX_FMT_YUV420P10LE && frame_full == target_full_range;
	if (frame->flags & AV_FRAME_FLAG_INTERLACED) {
		MutexLock lock(mutex);
		frames_interlaced++;
		if (frame->flags & AV_FRAME_FLAG_TOP_FIELD_FIRST) {
			frames_top_first++;
		}
	}
	if (format != last_format_id) {
		// A new picture format (the first frame, or a change mid-stream): what it is and how it is shown (stats).
		last_format_id = format;
		const char *name = av_get_pix_fmt_name((AVPixelFormat)format);
		MutexLock lock(mutex);
		last_format = name ? String(name) : String("?");
		if (copy_8bit || ten_bit) {
			conversion = vformat("%s %s", last_format, copy_8bit ? "copied" : "rounded to 8 bits");
		}
	}
	const bool range = target_full_range;

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
	if (!copy_8bit && !ten_bit) {
		if (!_convert(slot, width, height)) {
			MutexLock lock(mutex);
			slot.free = true;
			stop.set();
			return false;
		}
		MutexLock lock(mutex);
		frames_converted++;
	}
	uint8_t *y_out = slot.y.ptrw();
	uint8_t *uv_out = slot.uv.ptrw();
	const int chroma_width = width / 2;
	if (!copy_8bit && !ten_bit) {
		// Converted above.
	} else if (ten_bit) {
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
	if (pts == AV_NOPTS_VALUE) {
		frames_guessed_time++;
	}
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
	d["software_pixel_format"] = last_format; // As decoded.
	d["software_conversion"] = conversion; // "yuv420p copied", "swscale pal8 to NV12 (BT.601, limited range)", ...
	d["software_frames_converted"] = frames_converted;
	d["software_frames_interlaced"] = frames_interlaced;
	// The decoder's own view of interlacing (the pictures it flagged), in FFmpeg's field order names; "" before a frame.
	d["software_field_order"] = frames_decoded == 0 ? String() : (frames_interlaced * 2 < frames_decoded ? String("progressive") : (frames_top_first * 2 >= frames_interlaced ? String("tt") : String("bb")));
	d["software_frames_guessed_time"] = frames_guessed_time;
	d["software_frames_hidden"] = frames_hidden;
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
