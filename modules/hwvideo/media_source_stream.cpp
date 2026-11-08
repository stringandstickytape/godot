/**************************************************************************/
/*  media_source_stream.cpp                                               */
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

// MediaSource: pushed access units (a PC window's picture, sent over the network as single access units with a time).
// Nothing is demuxed: the app parses its own framing and hands each access unit over, and they go into the same video
// queue as a film's, so V4L2Decoder (hardware, as for films) and SoftwareVideoDecoder (a desktop) decode them unchanged
// and HWVideoTexture shows them at their time against the clock the app passes to update().

#include "media_source.h"

#include "core/object/class_db.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/pixfmt.h>
}

Error MediaSource::open_stream(const String &p_codec, int p_width, int p_height, int p_bit_depth) {
	close();
	const String codec = p_codec.to_lower();
	const bool hevc = codec == "hevc" || codec == "h265";
	const bool h264 = codec == "h264" || codec == "avc";
	ERR_FAIL_COND_V_MSG(!hevc && !h264, ERR_INVALID_PARAMETER, vformat("MediaSource: a stream is H.264 or HEVC, not \"%s\".", p_codec));
	// 0 x 0: the size comes from the first SPS (and so does the bit depth then).
	ERR_FAIL_COND_V_MSG(p_width < 0 || p_height < 0 || p_width > 8192 || p_height > 8192 || (p_width == 0) != (p_height == 0), ERR_INVALID_PARAMETER, vformat("MediaSource: a stream of %dx%d pixels?", p_width, p_height));
	ERR_FAIL_COND_V_MSG(p_bit_depth != 8 && p_bit_depth != 10, ERR_INVALID_PARAMETER, "MediaSource: a stream is 8 or 10 bits.");
	const AVCodecID id = hevc ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264;
	const bool ten_bit = p_bit_depth > 8;

	pushed_parameters = avcodec_parameters_alloc();
	ERR_FAIL_NULL_V(pushed_parameters, ERR_OUT_OF_MEMORY);
	pushed_parameters->codec_type = AVMEDIA_TYPE_VIDEO;
	pushed_parameters->codec_id = id;
	pushed_parameters->width = p_width;
	pushed_parameters->height = p_height;
	pushed_parameters->format = ten_bit ? AV_PIX_FMT_YUV420P10LE : AV_PIX_FMT_YUV420P;

	video = VideoInfo();
	video.valid = true;
	video.codec = hevc ? VIDEO_CODEC_HEVC : VIDEO_CODEC_H264;
	video.codec_name = hevc ? "hevc" : "h264";
	const AVCodecDescriptor *descriptor = avcodec_descriptor_get(id);
	video.codec_long_name = descriptor && descriptor->long_name ? String::utf8(descriptor->long_name) : video.codec_name;
	video.codec_id = (int)id;
	video.software_decoder = avcodec_find_decoder(id) != nullptr;
	video.stream_index = 0;
	video.width = p_width;
	video.height = p_height;
	video.fps = 0.0; // Variable: a frame when the window changes.
	video.bit_depth = p_bit_depth;
	video.profile = hevc ? (ten_bit ? "Main 10" : "Main") : "High";
	video.pixel_format = ten_bit ? "yuv420p10le" : "yuv420p";
	// What the PC's encoders write for a desktop picture: BT.709, limited range.
	video.color_primaries = "bt709";
	video.color_transfer = "bt709";
	video.color_matrix = "bt709";
	video.color_range = "tv";
	video.field_order = "progressive";

	path = "stream:" + video.codec_name;
	format_name = "stream";
	start_time = 0.0;
	duration = 0.0;
	bit_rate = 0;
	audio_track = -1;
	{
		MutexLock lock(mutex);
		state = STATE_READING;
		last_error = String();
		serial = 0;
		seek_pending = false;
		seek_target = 0.0;
		demux_eof = false;
		audio_blocks_video = false;
		last_video_pts = -1.0;
		stats = Stats();
		seek_count = 0;
		video_head = video_count = 0;
		video_queued_bytes = 0;
		for (BitrateSecond &entry : bitrate_seconds) {
			entry = BitrateSecond();
		}
		pushed_need_keyframe = true;
		pushed_keyframe_wanted = false;
		pushed_packets = 0;
		pushed_dropped = 0;
		pushed = true;
	}
	{
		MutexLock audio_lock(audio_mutex);
		audio_eof = true;
	}
	return OK;
}

bool MediaSource::push_video_packet(const PackedByteArray &p_data, int64_t p_time_usec, bool p_keyframe) {
	ERR_FAIL_COND_V_MSG(!pushed, false, "MediaSource: push_video_packet() needs open_stream().");
	const uint32_t size = (uint32_t)p_data.size();
	if (size == 0) {
		return false;
	}
	{
		MutexLock lock(mutex);
		pushed_packets++;
		if (state != STATE_READING || (pushed_need_keyframe && !p_keyframe)) {
			pushed_dropped++;
			return false;
		}
		if (video_count >= STREAM_QUEUE_MAX || video_count + 1 >= VIDEO_QUEUE_MAX || video_queued_bytes + size > VIDEO_QUEUE_MAX_BYTES) {
			// The decoder is behind. What is queued stays; this and everything up to the next keyframe go, since the decoder
			// would have no reference for them.
			pushed_dropped++;
			pushed_need_keyframe = true;
			pushed_keyframe_wanted = true;
			return false;
		}
	}

	VideoPacket entry;
	AnnexBSplitter::SPSInfo sps;
	int nal_type = -1;
	const bool has_sps = _scan_access_unit(p_data.ptr(), size, video.codec == VIDEO_CODEC_HEVC, sps, nullptr, &nal_type);
	AVPacket *packet = _packet_get();
	if (packet == nullptr || av_new_packet(packet, (int)size) < 0) {
		_packet_put(packet);
		MutexLock lock(mutex);
		pushed_dropped++;
		return false;
	}
	memcpy(packet->data, p_data.ptr(), size);
	packet->pts = packet->dts = p_time_usec;
	if (p_keyframe) {
		packet->flags |= AV_PKT_FLAG_KEY;
	}
	entry.packet = packet;
	entry.data = packet->data;
	entry.size = size;
	entry.pts = entry.dts = (double)p_time_usec / 1e6;
	entry.pts_valid = entry.dts_valid = true;
	entry.keyframe = p_keyframe;
	entry.nal_type = (int8_t)nal_type;
	if (has_sps) {
		entry.sps_width = sps.width;
		entry.sps_height = sps.height;
	}

	MutexLock lock(mutex);
	if (has_sps && !video.sps.valid && wake_fd < 0) {
		// The first SPS, before a decoder reads the video info (one opens only after the first keyframe). A stream opened
		// without a size takes its size and bit depth from it.
		video.sps = sps;
		if (video.width == 0 && sps.valid) {
			video.width = (int)sps.display_width;
			video.height = (int)sps.display_height;
			video.bit_depth = MAX((int)sps.bit_depth, 8);
			const bool ten_bit = video.bit_depth > 8;
			if (video.codec == VIDEO_CODEC_HEVC) {
				video.profile = ten_bit ? "Main 10" : "Main";
			}
			video.pixel_format = ten_bit ? "yuv420p10le" : "yuv420p";
			if (pushed_parameters != nullptr) {
				pushed_parameters->width = video.width;
				pushed_parameters->height = video.height;
				pushed_parameters->format = ten_bit ? AV_PIX_FMT_YUV420P10LE : AV_PIX_FMT_YUV420P;
			}
		}
	}
	if (p_keyframe) {
		pushed_need_keyframe = false;
		pushed_keyframe_wanted = false;
	}
	entry.serial = serial;
	video_queue[(video_head + video_count) % VIDEO_QUEUE_MAX] = entry;
	video_count++;
	video_queued_bytes += size;
	stats.packets_read++;
	stats.bytes_read += size;
	stats.video_packets++;
	stats.video_bytes += size;
	_count_video_bytes_locked(entry.pts, size);
	_wake_consumer_locked();
	return true;
}

void MediaSource::end_stream() {
	ERR_FAIL_COND_MSG(!pushed, "MediaSource: end_stream() needs open_stream().");
	MutexLock lock(mutex);
	demux_eof = true;
	if (state == STATE_READING) {
		state = STATE_ENDED;
	}
	_wake_consumer_locked();
}

bool MediaSource::is_keyframe_wanted() const {
	MutexLock lock(mutex);
	return pushed && pushed_keyframe_wanted;
}

// A seek on a stream (V4L2Decoder after a resolution change, HWVideoTexture switching to software): there is nothing to
// read again, so what is queued goes and the decoder starts at the next keyframe the sender gives.
int MediaSource::_seek_stream(double p_seconds) {
	MutexLock lock(mutex);
	serial++;
	seek_target = MAX(p_seconds, 0.0);
	seek_count++;
	_flush_video_queue_locked();
	last_video_pts = -1.0;
	pushed_need_keyframe = true;
	pushed_keyframe_wanted = true;
	if (state == STATE_ENDED && !demux_eof) {
		state = STATE_READING;
	}
	_wake_consumer_locked();
	return (int)serial;
}

void MediaSource::_stream_stats_locked(Dictionary &r_stats) const {
	r_stats["stream"] = true;
	r_stats["stream_packets_pushed"] = pushed_packets;
	r_stats["stream_packets_dropped"] = pushed_dropped;
	r_stats["stream_keyframe_wanted"] = pushed_keyframe_wanted;
}

// _close_internal(): the stream's own parts (the queue itself is flushed there).
void MediaSource::_close_stream() {
	if (pushed_parameters != nullptr) {
		avcodec_parameters_free(&pushed_parameters);
	}
	MutexLock lock(mutex);
	pushed = false;
	pushed_need_keyframe = true;
	pushed_keyframe_wanted = false;
}

void MediaSource::_bind_stream_methods() {
	ClassDB::bind_method(D_METHOD("open_stream", "codec", "width", "height", "bit_depth"), &MediaSource::open_stream, DEFVAL(8));
	ClassDB::bind_method(D_METHOD("push_video_packet", "data", "time_usec", "keyframe"), &MediaSource::push_video_packet);
	ClassDB::bind_method(D_METHOD("end_stream"), &MediaSource::end_stream);
	ClassDB::bind_method(D_METHOD("is_stream"), &MediaSource::is_stream);
	ClassDB::bind_method(D_METHOD("is_keyframe_wanted"), &MediaSource::is_keyframe_wanted);
}
