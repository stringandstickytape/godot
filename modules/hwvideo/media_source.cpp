/**************************************************************************/
/*  media_source.cpp                                                      */
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

#include "media_source.h"
#include "software_video_decoder.h"

#include "audio_stream_media.h"
#include "audio_tempo.h"
#include "net_stream.h"
#include "smb_client.h"

#include "core/config/project_settings.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "servers/audio/audio_server.h"

#include <unistd.h>
#include <cmath>
#include <cstring>
#include <ctime>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
}

static constexpr double SQRT_HALF = 0.70710678118654752440;
static constexpr int AVIO_BUFFER_SIZE = 256 << 10; // smb:// files: FFmpeg's read buffer in front of the SmbFile ring.

static double thread_cpu_seconds() {
	struct timespec ts;
	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* Helpers */

String MediaSource::_av_error(int p_error) {
	char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
	av_strerror(p_error, buffer, sizeof(buffer));
	return String::utf8(buffer);
}

// Walks the NAL units of an Annex-B access unit up to its first picture (VCL) NAL unit, so only the few bytes in front of
// the slice data are looked at. Returns true if there is an SPS; r_sps gets the first one. r_parameter_sets, if given,
// gets the VPS/SPS/PPS NAL units found, each behind a 4-byte start code.
bool MediaSource::_scan_access_unit(const uint8_t *p_data, uint64_t p_size, bool p_hevc, AnnexBSplitter::SPSInfo &r_sps, LocalVector<uint8_t> *r_parameter_sets, int *r_nal_type) {
	if (r_parameter_sets) {
		r_parameter_sets->clear();
	}
	const auto find_start_code = [p_data, p_size](uint64_t p_from) -> uint64_t {
		for (uint64_t k = p_from; k + 2 < p_size; k++) {
			if (p_data[k + 2] > 1) {
				k += 2; // Can't be part of a start code that begins at k, k + 1 or k + 2.
			} else if (p_data[k] == 0 && p_data[k + 1] == 0 && p_data[k + 2] == 1) {
				return k;
			}
		}
		return p_size;
	};
	bool has_sps = false;
	uint64_t start_code = find_start_code(0);
	while (start_code < p_size) {
		const uint64_t nal = start_code + 3;
		if (nal >= p_size) {
			break;
		}
		const uint8_t header = p_data[nal];
		bool vcl;
		bool parameter_set;
		bool sps;
		const uint32_t type = p_hevc ? (header >> 1) & 0x3f : header & 0x1f;
		if (p_hevc) {
			vcl = type < 32;
			parameter_set = type >= 32 && type <= 34;
			sps = type == 33;
		} else {
			vcl = type >= 1 && type <= 5;
			parameter_set = type == 7 || type == 8;
			sps = type == 7;
		}
		if (vcl) {
			if (r_nal_type) {
				*r_nal_type = (int)type;
			}
			break;
		}
		const uint64_t next = find_start_code(nal);
		uint64_t end = next;
		while (end > nal && p_data[end - 1] == 0) {
			end--; // Trailing zeros, and the zero_byte of a 4-byte start code.
		}
		if (end > nal) {
			if (sps && !has_sps) {
				has_sps = true;
				if (p_hevc) {
					AnnexBSplitter::parse_hevc_sps(p_data + nal, end - nal, r_sps);
				} else {
					AnnexBSplitter::parse_h264_sps(p_data + nal, end - nal, r_sps);
				}
			}
			if (parameter_set && r_parameter_sets) {
				static const uint8_t four_byte_start_code[4] = { 0, 0, 0, 1 };
				for (uint8_t b : four_byte_start_code) {
					r_parameter_sets->push_back(b);
				}
				for (uint64_t k = nal; k < end; k++) {
					r_parameter_sets->push_back(p_data[k]);
				}
			}
		}
		start_code = next;
	}
	return has_sps;
}

// The codec configuration of MP4/Matroska (avcC or hvcC) holds the parameter sets with 16-bit lengths; Annex-B
// configurations (some Matroska and TS muxers) are copied as they are.
void MediaSource::_config_to_annexb(const uint8_t *p_data, int p_size, bool p_hevc, LocalVector<uint8_t> &r_out) {
	r_out.clear();
	if (p_data == nullptr || p_size < 4) {
		return;
	}
	if ((p_data[0] == 0 && p_data[1] == 0 && p_data[2] == 1) || (p_data[0] == 0 && p_data[1] == 0 && p_data[2] == 0 && p_data[3] == 1)) {
		for (int i = 0; i < p_size; i++) {
			r_out.push_back(p_data[i]);
		}
		return;
	}
	if (p_data[0] != 1) {
		return;
	}
	int pos = 0;
	const auto append_nal = [&](int p_length) -> bool {
		if (p_length <= 0 || pos + p_length > p_size) {
			return false;
		}
		r_out.push_back(0);
		r_out.push_back(0);
		r_out.push_back(0);
		r_out.push_back(1);
		for (int i = 0; i < p_length; i++) {
			r_out.push_back(p_data[pos + i]);
		}
		pos += p_length;
		return true;
	};
	const auto read16 = [&](int &r_value) -> bool {
		if (pos + 2 > p_size) {
			return false;
		}
		r_value = (p_data[pos] << 8) | p_data[pos + 1];
		pos += 2;
		return true;
	};
	if (p_hevc) {
		// HEVCDecoderConfigurationRecord: 22 bytes, numOfArrays, then (type, numNalus, (length, NAL unit)*)*.
		if (p_size < 23) {
			return;
		}
		pos = 22;
		const int arrays = p_data[pos++];
		for (int a = 0; a < arrays && pos < p_size; a++) {
			pos++; // array_completeness, NAL unit type.
			int count = 0;
			if (!read16(count)) {
				return;
			}
			for (int i = 0; i < count; i++) {
				int length = 0;
				if (!read16(length) || !append_nal(length)) {
					return;
				}
			}
		}
	} else {
		// AVCDecoderConfigurationRecord: 5 bytes, numOfSequenceParameterSets & 0x1f, SPSs, numOfPictureParameterSets, PPSs.
		if (p_size < 7) {
			return;
		}
		pos = 5;
		for (int list = 0; list < 2 && pos < p_size; list++) {
			const int count = list == 0 ? (p_data[pos++] & 0x1f) : p_data[pos++];
			for (int i = 0; i < count; i++) {
				int length = 0;
				if (!read16(length) || !append_nal(length)) {
					return;
				}
			}
		}
	}
}

bool MediaSource::_is_text_subtitle(int p_codec_id) {
	switch (p_codec_id) {
		case AV_CODEC_ID_SUBRIP:
		case AV_CODEC_ID_ASS:
		case AV_CODEC_ID_SSA:
		case AV_CODEC_ID_MOV_TEXT:
		case AV_CODEC_ID_WEBVTT:
		case AV_CODEC_ID_TEXT:
			return true;
		default:
			return false;
	}
}

bool MediaSource::_is_bitmap_subtitle(int p_codec_id) {
	switch (p_codec_id) {
		case AV_CODEC_ID_HDMV_PGS_SUBTITLE:
		case AV_CODEC_ID_DVD_SUBTITLE:
		case AV_CODEC_ID_DVB_SUBTITLE:
		case AV_CODEC_ID_XSUB:
			return true;
		default:
			return false;
	}
}

// An ASS event as FFmpeg's decoders give it ("ReadOrder,Layer,Style,Name,MarginL,MarginR,MarginV,Effect,Text") to plain
// text: the Text field without override blocks ({...}), \N and \n as line breaks, \h as a space.
String MediaSource::_ass_to_text(const String &p_ass) {
	int commas = 0;
	int start = 0;
	for (int i = 0; i < p_ass.length() && commas < 8; i++) {
		if (p_ass[i] == ',') {
			commas++;
			start = i + 1;
		}
	}
	String out;
	bool in_override = false;
	for (int i = start; i < p_ass.length(); i++) {
		const char32_t c = p_ass[i];
		if (in_override) {
			in_override = c != '}';
			continue;
		}
		if (c == '{') {
			in_override = true;
		} else if (c == '\\' && i + 1 < p_ass.length() && (p_ass[i + 1] == 'N' || p_ass[i + 1] == 'n')) {
			out += "\n";
			i++;
		} else if (c == '\\' && i + 1 < p_ass.length() && p_ass[i + 1] == 'h') {
			out += " ";
			i++;
		} else if (c != '\r' && c != '\n') {
			out += c;
		}
	}
	return out.strip_edges();
}

void MediaSource::_set_error(const String &p_message) {
	{
		MutexLock lock(mutex);
		state = STATE_ERROR;
		last_error = p_message;
	}
	ERR_PRINT("MediaSource: " + p_message);
}

double MediaSource::_media_time(int64_t p_ts, int p_stream) const {
	return (double)p_ts * av_q2d(format->streams[p_stream]->time_base) - start_time;
}

/* Packet pool: AVPacket structs are reused; FFmpeg allocates the packet data. */

AVPacket *MediaSource::_packet_get() {
	{
		MutexLock lock(mutex);
		if (!packet_pool.is_empty()) {
			AVPacket *packet = packet_pool[packet_pool.size() - 1];
			packet_pool.resize(packet_pool.size() - 1);
			return packet;
		}
	}
	return av_packet_alloc();
}

void MediaSource::_packet_put(AVPacket *p_packet) {
	if (p_packet == nullptr) {
		return;
	}
	av_packet_unref(p_packet);
	MutexLock lock(mutex);
	packet_pool.push_back(p_packet);
}

void MediaSource::_flush_video_queue_locked() {
	for (uint32_t i = 0; i < video_count; i++) {
		VideoPacket &entry = video_queue[(video_head + i) % VIDEO_QUEUE_MAX];
		if (entry.packet) {
			av_packet_unref(entry.packet);
			packet_pool.push_back(entry.packet);
		}
		entry = VideoPacket();
	}
	video_head = 0;
	video_count = 0;
	video_queued_bytes = 0;
}

void MediaSource::_wake_consumer_locked() {
	if (wake_fd >= 0) {
		const uint64_t one = 1;
		const ssize_t written = write(wake_fd, &one, sizeof(one));
		(void)written;
	}
}

uint32_t MediaSource::_video_queue_target() const {
	return MAX(VIDEO_QUEUE_MIN, (uint32_t)(video_queue_seconds * (video.fps > 0.0 ? video.fps : 30.0)));
}

// Demux thread, mutex held. Read on while the video queue or the decoded audio is short of its target (ffplay's rule: a
// stream that has enough doesn't stop the others), unless the video queue is at its hard limit.
bool MediaSource::_needs_input_locked() const {
	if (video_count + 4 >= VIDEO_QUEUE_MAX || video_queued_bytes >= VIDEO_QUEUE_MAX_BYTES || audio_fifo_count + 1 >= AUDIO_FIFO_MAX || audio_fifo_bytes >= AUDIO_FIFO_MAX_BYTES) {
		return false;
	}
	if (demux_trick > 0.0) {
		return video_count < TRICK_QUEUE; // Keyframes only, no audio.
	}
	const uint32_t target = current_subtitle_stream >= 0 ? MAX(video_queue_target, subtitle_video_target) : video_queue_target;
	const bool video_enough = video.stream_index < 0 || video_count >= target;
	bool audio_enough = true;
	if (current_audio_stream >= 0 && audio_fifo_count == 0 && scratch_frames == 0) {
		MutexLock audio_lock(audio_mutex);
		audio_enough = audio_eof || (double)(ring_write - ring_read) >= audio_target_seconds * output_rate;
	}
	return !(video_enough && audio_enough);
}

/* Audio */

void MediaSource::_apply_discard() {
	for (unsigned i = 0; i < format->nb_streams; i++) {
		if (demux_trick > 0.0) {
			// Trick play: the video's keyframes only (demuxers that know skip the rest without reading it).
			format->streams[i]->discard = (int)i == video.stream_index ? AVDISCARD_NONKEY : AVDISCARD_ALL;
			continue;
		}
		const bool used = (int)i == video.stream_index || (int)i == current_audio_stream || (int)i == current_subtitle_stream || (int)i == current_secondary_stream;
		format->streams[i]->discard = used ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
	}
}

void MediaSource::_audio_fifo_push(AVPacket *p_packet) {
	if (audio_fifo.size() < AUDIO_FIFO_MAX) {
		audio_fifo.resize(AUDIO_FIFO_MAX);
	}
	audio_fifo[(audio_fifo_head + audio_fifo_count) % AUDIO_FIFO_MAX] = p_packet;
	audio_fifo_count++;
	audio_fifo_bytes += (uint64_t)p_packet->size;
}

AVPacket *MediaSource::_audio_fifo_pop() {
	if (audio_fifo_count == 0) {
		return nullptr;
	}
	AVPacket *packet = audio_fifo[audio_fifo_head];
	audio_fifo[audio_fifo_head] = nullptr;
	audio_fifo_head = (audio_fifo_head + 1) % AUDIO_FIFO_MAX;
	audio_fifo_count--;
	audio_fifo_bytes -= (uint64_t)packet->size;
	return packet;
}

void MediaSource::_audio_fifo_clear() {
	while (audio_fifo_count > 0) {
		_packet_put(_audio_fifo_pop());
	}
	audio_fifo_head = 0;
	audio_fifo_bytes = 0;
}

void MediaSource::_close_audio_codec() {
	_audio_fifo_clear();
	avcodec_free_context(&audio_codec);
	dstats.decoder_drc = false;
	dstats.dialogue_boost_applies = false;
	swr_free(&swr);
	swr_in_format = -1;
	swr_in_rate = 0;
	swr_in_channels = 0;
	swr_in_mask = 0;
	scratch_frames = 0;
	scratch_offset = 0;
}

void MediaSource::_open_audio_track(int p_track) {
	_close_audio_codec();
	current_audio_track = p_track;
	current_audio_stream = -1;
	if (p_track >= 0 && p_track < (int)audio_tracks.size()) {
		const int index = audio_tracks[p_track].stream_index;
		const AVStream *stream = format->streams[index];
		const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
		String problem;
		if (codec == nullptr) {
			problem = vformat("no decoder for %s in this FFmpeg build", avcodec_get_name(stream->codecpar->codec_id));
		} else {
			audio_codec = avcodec_alloc_context3(codec);
			int r = audio_codec ? avcodec_parameters_to_context(audio_codec, stream->codecpar) : AVERROR(ENOMEM);
			if (r >= 0) {
				audio_codec->pkt_timebase = stream->time_base;
				audio_codec->thread_count = 1;
				AVDictionary *options = nullptr;
				for (const KeyValue<Variant, Variant> &kv : audio_decoder_options) {
					av_dict_set(&options, String(kv.key).utf8().get_data(), String(kv.value).utf8().get_data(), 0);
				}
				r = avcodec_open2(audio_codec, codec, &options);
				// Options the decoder doesn't have stay in the dictionary: other codecs' options are fine to pass.
				av_dict_free(&options);
			}
			if (r < 0) {
				problem = vformat("opening the %s decoder failed: %s", codec->name, _av_error(r));
				avcodec_free_context(&audio_codec);
			}
		}
		if (problem.is_empty()) {
			current_audio_stream = index;
			dstats.decoder_drc = codec->id == AV_CODEC_ID_AC3 || codec->id == AV_CODEC_ID_EAC3;
			_apply_audio_decoder_settings();
			dynamics.reset(); // Another track: its typical level is learnt again.
			print_line(vformat("MediaSource: audio track %d: %s decoder%s.", p_track, codec->name, audio_decoder_options.is_empty() ? String() : vformat(" with options %s", Variant(audio_decoder_options))));
		} else {
			// Not fatal: the video plays on, without audio (AudioStreamMedia finishes at once).
			MutexLock lock(mutex);
			last_error = vformat("Audio track %d: %s.", p_track, problem);
			ERR_PRINT("MediaSource: " + last_error);
		}
	}
	_apply_discard();
	if (current_audio_stream < 0) {
		MutexLock audio_lock(audio_mutex);
		audio_eof = true;
	}
}

// FFmpeg's AC-3 / E-AC-3 decoder clears its whole context when flushed, its options too (drc_scale, heavy_compr,
// target_level go back to 0): they are set again after open, after every seek, and when night mode changes. Without
// night mode drc_scale is 0 (the full dynamic range, as mpv plays it); in night mode it is the strength. An app's own
// "drc_scale" in audio_decoder_options wins.
void MediaSource::_apply_audio_decoder_settings() {
	if (audio_codec == nullptr || !dstats.decoder_drc) {
		return;
	}
	for (const KeyValue<Variant, Variant> &kv : audio_decoder_options) {
		av_opt_set(audio_codec, String(kv.key).utf8().get_data(), String(kv.value).utf8().get_data(), AV_OPT_SEARCH_CHILDREN);
	}
	if (!audio_decoder_options.has("drc_scale")) {
		av_opt_set_double(audio_codec, "drc_scale", dsp_night_mode ? (double)dsp_strength : 0.0, AV_OPT_SEARCH_CHILDREN);
	}
}

// Demux thread: night mode or dialogue boost changed.
void MediaSource::_update_audio_dsp() {
	_apply_audio_decoder_settings();
	const bool was_active = dynamics.is_active();
	dynamics.configure(dsp_night_mode, dsp_strength, dsp_dialogue_boost > 0.0f, output_rate);
	if (!was_active) {
		dynamics.reset();
	}
	// Dialogue boost: _configure_swr() builds the resampler again with the new centre level at the next frame.
	print_line(vformat("MediaSource: night mode %s (strength %.2f, decoder DRC %s), dialogue boost %.1f dB.", dsp_night_mode ? "on" : "off", dsp_strength,
			dstats.decoder_drc ? "yes" : "no", dsp_dialogue_boost));
}

bool MediaSource::_configure_swr(const AVFrame *p_frame) {
	const int channels = p_frame->ch_layout.nb_channels;
	const uint64_t mask = p_frame->ch_layout.order == AV_CHANNEL_ORDER_NATIVE ? p_frame->ch_layout.u.mask : 0;
	// Downmix to stereo (ITU-R BS.775): centre at -3 dB, plus the dialogue boost. With discrete channels swr makes 7.1 and
	// _downmix_discrete() the stereo, with the boost.
	const double center_level = discrete_channels ? SQRT_HALF : SQRT_HALF * Math::db_to_linear((double)dsp_dialogue_boost);
	if (swr && swr_in_format == p_frame->format && swr_in_rate == p_frame->sample_rate && swr_in_channels == channels && swr_in_mask == mask && swr_center_level == center_level) {
		return true;
	}
	swr_free(&swr);
	if (channels <= 0 || p_frame->sample_rate <= 0) {
		return false;
	}
	AVChannelLayout in_layout = {};
	if (p_frame->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC) {
		av_channel_layout_default(&in_layout, channels);
	} else {
		av_channel_layout_copy(&in_layout, &p_frame->ch_layout);
	}
	AVChannelLayout out_layout = {};
	if (discrete_channels) {
		// Other layouts' channels land on these slots (exact matches 1:1; wide and back centre channels are mixed in).
		const AVChannelLayout layout_7_1 = AV_CHANNEL_LAYOUT_7POINT1;
		av_channel_layout_copy(&out_layout, &layout_7_1);
	} else {
		av_channel_layout_default(&out_layout, 2);
	}
	int r = swr_alloc_set_opts2(&swr, &out_layout, AV_SAMPLE_FMT_FLT, output_rate, &in_layout, (AVSampleFormat)p_frame->format, p_frame->sample_rate, 0, nullptr);
	if (r >= 0) {
		// Downmix to stereo (ITU-R BS.775): centre and surrounds at -3 dB, LFE dropped. Not normalised by default (like
		// mpv), so dialogue keeps its level; downmix_normalize scales the matrix so no sum can exceed full scale.
		// Mono (a centre channel alone) is spread at -3 dB whatever the centre level: the boost needs front left and right.
		av_opt_set_double(swr, "center_mix_level", center_level, 0);
		av_opt_set_double(swr, "surround_mix_level", SQRT_HALF, 0);
		av_opt_set_double(swr, "lfe_mix_level", 0.0, 0);
		if (downmix_normalize) {
			av_opt_set_double(swr, "rematrix_maxval", 1.0, 0);
		}
		r = swr_init(swr);
	}
	char in_name[128] = {};
	av_channel_layout_describe(&in_layout, in_name, sizeof(in_name));
	dstats.dialogue_boost_applies = av_channel_layout_index_from_channel(&in_layout, AV_CHAN_FRONT_CENTER) >= 0 &&
			av_channel_layout_index_from_channel(&in_layout, AV_CHAN_FRONT_LEFT) >= 0 && av_channel_layout_index_from_channel(&in_layout, AV_CHAN_FRONT_RIGHT) >= 0;
	static const AVChannel slots[DISCRETE_CHANNELS] = { AV_CHAN_FRONT_LEFT, AV_CHAN_FRONT_RIGHT, AV_CHAN_FRONT_CENTER, AV_CHAN_LOW_FREQUENCY,
		AV_CHAN_BACK_LEFT, AV_CHAN_BACK_RIGHT, AV_CHAN_SIDE_LEFT, AV_CHAN_SIDE_RIGHT };
	dstats.audio_channel_mask = 0;
	for (int i = 0; i < DISCRETE_CHANNELS; i++) {
		if (av_channel_layout_index_from_channel(&in_layout, slots[i]) >= 0) {
			dstats.audio_channel_mask |= 1u << i;
		}
	}
	av_channel_layout_uninit(&in_layout);
	av_channel_layout_uninit(&out_layout);
	if (r < 0) {
		swr_free(&swr);
		MutexLock lock(mutex);
		last_error = vformat("Can't resample %s %d Hz audio: %s.", in_name, p_frame->sample_rate, _av_error(r));
		ERR_PRINT("MediaSource: " + last_error);
		return false;
	}
	swr_in_format = p_frame->format;
	swr_in_rate = p_frame->sample_rate;
	swr_in_channels = channels;
	swr_in_mask = mask;
	swr_center_level = center_level;
	const String description = vformat("MediaSource: audio %s, %s, %d Hz -> %s float, %d Hz%s.", av_get_sample_fmt_name((AVSampleFormat)p_frame->format), in_name, p_frame->sample_rate,
			discrete_channels ? "7.1 and stereo" : "stereo", output_rate, downmix_normalize ? " (normalised downmix)" : "");
	if (description != swr_description) {
		swr_description = description;
		print_line(description);
	}
	return true;
}

// discrete_channels: the stereo mix of 7.1 frames, as swr makes it without discrete channels (ITU-R BS.775: centre and
// surrounds at -3 dB, LFE dropped; mono spread at -3 dB). The dialogue boost raises the centre channel itself, so the
// channel streams get it too.
void MediaSource::_downmix_discrete(float *p_channels, float *r_stereo, uint32_t p_frames) {
	const uint32_t mask = dstats.audio_channel_mask;
	const float s = (float)SQRT_HALF;
	if (dstats.dialogue_boost_applies && dsp_dialogue_boost > 0.0f) {
		const float boost = (float)Math::db_to_linear((double)dsp_dialogue_boost);
		for (uint32_t f = 0; f < p_frames; f++) {
			p_channels[(uint64_t)f * DISCRETE_CHANNELS + 2] *= boost;
		}
	}
	float scale = 1.0f;
	if (downmix_normalize) {
		// As swr's rematrix_maxval 1: the largest sum of one output's coefficients is 1.
		const float row = ((mask & 1) ? 1.0f : 0.0f) + ((mask & 4) ? s : 0.0f) + ((mask & 16) ? s : 0.0f) + ((mask & 64) ? s : 0.0f);
		scale = row > 1.0f ? 1.0f / row : 1.0f;
	}
	const float front = scale;
	const float centre = s * scale;
	const float surround = s * scale;
	for (uint32_t f = 0; f < p_frames; f++) {
		const float *c = p_channels + (uint64_t)f * DISCRETE_CHANNELS; // FL FR FC LFE BL BR SL SR
		const float middle = centre * c[2];
		r_stereo[f * 2] = front * c[0] + middle + surround * (c[4] + c[6]);
		r_stereo[f * 2 + 1] = front * c[1] + middle + surround * (c[5] + c[7]);
	}
}

// p_frame null: flush what the resampler still holds (end of the file).
void MediaSource::_resample_frame(const AVFrame *p_frame) {
	if (p_frame && !_configure_swr(p_frame)) {
		return;
	}
	if (swr == nullptr) {
		return;
	}
	const int in_samples = p_frame ? p_frame->nb_samples : 0;
	const int max_out = swr_get_out_samples(swr, in_samples);
	if (max_out <= 0) {
		return;
	}
	double time = audio_next_time;
	if (p_frame) {
		const int64_t ts = p_frame->best_effort_timestamp != AV_NOPTS_VALUE ? p_frame->best_effort_timestamp : p_frame->pts;
		if (ts != AV_NOPTS_VALUE) {
			// The resampler's buffered input comes out first.
			time = _media_time(ts, current_audio_stream) - (double)swr_get_delay(swr, output_rate) / output_rate;
		}
	}
	if (time < -1e8) {
		time = 0.0;
	}
	if (scratch_frames == 0) {
		scratch_offset = 0;
	}
	const uint64_t frames_needed = (uint64_t)scratch_offset + scratch_frames + (uint32_t)max_out;
	if (scratch.size() < frames_needed * 2) {
		scratch.resize(frames_needed * 2); // Grows to the largest packet once; no allocation in steady state.
	}
	float *stereo = scratch.ptr() + (uint64_t)(scratch_offset + scratch_frames) * 2;
	float *channels = nullptr;
	if (discrete_channels) {
		if (channel_scratch.size() < frames_needed * DISCRETE_CHANNELS) {
			channel_scratch.resize(frames_needed * DISCRETE_CHANNELS);
		}
		if (gain_trace.size() < (uint32_t)max_out) {
			gain_trace.resize(max_out);
		}
		channels = channel_scratch.ptr() + (uint64_t)(scratch_offset + scratch_frames) * DISCRETE_CHANNELS;
	}
	uint8_t *out = (uint8_t *)(channels ? channels : stereo);
	const int converted = swr_convert(swr, &out, max_out, p_frame ? (const uint8_t **)p_frame->extended_data : nullptr, in_samples);
	if (converted <= 0) {
		return;
	}
	if (channels) {
		_downmix_discrete(channels, stereo, (uint32_t)converted);
	}
	if (dynamics.is_active()) {
		dynamics.process(stereo, (uint32_t)converted, 2, channels ? gain_trace.ptr() : nullptr);
		if (channels) {
			// The channels get the gain the stereo mix got.
			for (int f = 0; f < converted; f++) {
				float *frame = channels + (uint64_t)f * DISCRETE_CHANNELS;
				const float g = gain_trace[f];
				for (int c = 0; c < DISCRETE_CHANNELS; c++) {
					frame[c] *= g;
				}
			}
		}
		dstats.audio_gain_db = dynamics.get_gain_db();
		dstats.audio_limiter_db = dynamics.get_limiter_db();
		dstats.audio_typical_level_db = dynamics.get_typical_level_db();
	}
	if (scratch_frames == 0) {
		scratch_time = time;
		scratch_serial = demux_serial;
	}
	scratch_frames += (uint32_t)converted;
	audio_next_time = time + (double)converted / output_rate;
	dstats.audio_frames_out += (uint64_t)converted;
}

// p_packet null: drain the decoder (end of the file).
void MediaSource::_decode_audio(const AVPacket *p_packet) {
	if (audio_codec == nullptr) {
		return;
	}
	int r = avcodec_send_packet(audio_codec, p_packet);
	if (r < 0 && r != AVERROR_EOF) {
		dstats.audio_decode_errors++;
		return;
	}
	while ((r = avcodec_receive_frame(audio_codec, audio_frame)) >= 0) {
		_resample_frame(audio_frame);
		av_frame_unref(audio_frame);
	}
	if (r != AVERROR(EAGAIN) && r != AVERROR_EOF) {
		dstats.audio_decode_errors++;
	}
	if (p_packet == nullptr) {
		_resample_frame(nullptr);
	}
	_flush_scratch();
}

// audio_mutex held. p_samples null writes silence.
void MediaSource::_write_ring_locked(const float *p_samples, const float *p_channels, uint32_t p_frames, double p_time) {
	if (p_frames == 0) {
		return;
	}
	// A marker wherever the time doesn't continue from what is in the ring (start, seek, gap in the stream).
	bool continuous = false;
	if (marker_count > 0) {
		const AudioMarker &last = markers[marker_count - 1];
		const double expected = last.time + (double)(ring_write - last.frame) / output_rate;
		continuous = Math::abs(p_time - expected) < 0.002;
	}
	if (!continuous) {
		if (marker_count == AUDIO_MARKERS_MAX) {
			for (uint32_t i = 1; i < marker_count; i++) {
				markers[i - 1] = markers[i];
			}
			marker_count--;
		}
		markers[marker_count].frame = ring_write;
		markers[marker_count].time = p_time;
		marker_count++;
	}
	const bool channels = !channel_ring.is_empty();
	uint32_t done = 0;
	while (done < p_frames) {
		const uint64_t at = (ring_write + done) % ring_capacity;
		const uint32_t run = (uint32_t)MIN((uint64_t)(p_frames - done), ring_capacity - at);
		if (p_samples) {
			memcpy((void *)&ring[at], p_samples + (uint64_t)done * 2, (size_t)run * sizeof(AudioFrame));
		} else {
			memset((void *)&ring[at], 0, (size_t)run * sizeof(AudioFrame));
		}
		if (channels) {
			float *destination = channel_ring.ptr() + at * DISCRETE_CHANNELS;
			if (p_channels) {
				memcpy((void *)destination, p_channels + (uint64_t)done * DISCRETE_CHANNELS, (size_t)run * DISCRETE_CHANNELS * sizeof(float));
			} else {
				memset((void *)destination, 0, (size_t)run * DISCRETE_CHANNELS * sizeof(float));
			}
		}
		done += run;
	}
	ring_write += p_frames;
}

// Moves decoded audio into the ring, as much as fits. After a seek, audio before the target is dropped and a gap up to
// the first decoded sample (< 1 s) is filled with silence, so the audio clock starts exactly at the target.
void MediaSource::_flush_scratch() {
	if (scratch_frames == 0) {
		return;
	}
	const double rate = output_rate;
	uint32_t silence = 0;
	double silence_time = 0.0;
	if (audio_trim_until >= 0.0) {
		const double end = scratch_time + scratch_frames / rate;
		if (end <= audio_trim_until) {
			dstats.audio_dropped_frames += scratch_frames;
			scratch_frames = 0;
			if (eof_drained) {
				MutexLock audio_lock(audio_mutex);
				if (audio_serial == scratch_serial) {
					audio_eof = true; // A seek to the very end: nothing left to play.
				}
			}
			return;
		}
		if (scratch_time < audio_trim_until) {
			const uint32_t skip = (uint32_t)MIN((double)scratch_frames, Math::round((audio_trim_until - scratch_time) * rate));
			scratch_offset += skip;
			scratch_frames -= skip;
			scratch_time += skip / rate;
			dstats.audio_dropped_frames += skip;
		} else if (scratch_time - audio_trim_until < 1.0) {
			silence = (uint32_t)Math::round((scratch_time - audio_trim_until) * rate);
			silence_time = audio_trim_until;
		}
		audio_trim_until = -1.0;
	}
	MutexLock audio_lock(audio_mutex);
	if (audio_serial != scratch_serial) {
		scratch_frames = 0; // Decoded before a seek.
		return;
	}
	if (silence > 0 && ring_capacity - (ring_write - ring_read) >= silence) {
		_write_ring_locked(nullptr, nullptr, silence, silence_time);
		dstats.audio_silence_frames += silence;
	}
	const uint32_t room = (uint32_t)MIN((uint64_t)scratch_frames, ring_capacity - (ring_write - ring_read));
	if (room > 0) {
		_write_ring_locked(scratch.ptr() + (uint64_t)scratch_offset * 2, channel_ring.is_empty() ? nullptr : channel_scratch.ptr() + (uint64_t)scratch_offset * DISCRETE_CHANNELS, room, scratch_time);
		scratch_offset += room;
		scratch_frames -= room;
		scratch_time += room / rate;
	}
	if (scratch_frames == 0 && eof_drained) {
		audio_eof = true;
	}
}

void MediaSource::mix_audio(AudioFrame *p_buffer, int p_frames) {
	_mix(0, p_buffer, p_frames);
}

void MediaSource::mix_audio_channel(AudioFrame *p_buffer, int p_frames, int p_channel) {
	if (p_channel < 0 || p_channel >= DISCRETE_CHANNELS) {
		memset((void *)p_buffer, 0, (size_t)MAX(p_frames, 0) * sizeof(AudioFrame));
		return;
	}
	_mix(1 + p_channel, p_buffer, p_frames);
}

// audio_mutex held, speed 1. Where reader p_reader (0: the stereo mix, 1 + n: channel n) reads next, and how many frames
// it gets. Without discrete channels the stereo mix is the only reader and moves ring_read. With them, every reader in
// one mix step of the audio server reads the same frames: a step starts when a reader comes back for more (it has read
// in this step already), which also commits what the step read (ring_read = step_end). r_first: this reader started the
// step (it counts the frames and moves the clock).
uint32_t MediaSource::_take_frames_locked(int p_reader, uint32_t p_frames, uint64_t &r_from, bool &r_first) {
	if (channel_ring.is_empty()) {
		r_from = ring_read;
		r_first = true;
		const uint32_t got = (uint32_t)MIN((uint64_t)p_frames, ring_write - ring_read);
		ring_read += got;
		return got;
	}
	r_first = false;
	if (step_serial != audio_serial || reader_step[p_reader] == mix_step) {
		if (step_serial == audio_serial) {
			ring_read = CLAMP(step_end, ring_read, ring_write);
		}
		step_serial = audio_serial;
		mix_step++;
		step_start = ring_read;
		step_end = ring_read;
		r_first = true;
	}
	reader_step[p_reader] = mix_step;
	r_from = step_start;
	const uint32_t got = (uint32_t)MIN((uint64_t)p_frames, ring_write - step_start);
	step_end = MAX(step_end, step_start + got);
	return got;
}

void MediaSource::_mix(int p_reader, AudioFrame *p_buffer, int p_frames) {
	if (p_frames <= 0) {
		return;
	}
	uint32_t got = 0;
	bool wake = false;
	{
		MutexLock audio_lock(audio_mutex);
		bool counts = true; // The reader that starts a mix step counts frames, underruns and moves the clock.
		if (live_preroll_frames > 0 && !live_playing && (audio_eof || ring_write - ring_read >= live_preroll_frames)) {
			live_playing = true;
		}
		const bool live_hold = live_preroll_frames > 0 && !live_playing; // Every reader holds: nothing is taken.
		if (ring_capacity > 0 && !live_hold) {
			if (playback_speed != 1.0 && tempo != nullptr) {
				// The time stretch is for the stereo mix only: channel streams are silent at other speeds.
				if (p_reader == 0) {
					got = _mix_tempo_locked(p_buffer, (uint32_t)p_frames);
					step_serial = UINT32_MAX; // ring_read moved: the next step starts from it.
				} else {
					counts = false;
				}
			} else {
				if (tempo_active) {
					// Back to 1: go on from the last frame heard (what the time stretch made but didn't hand out is dropped).
					tempo_active = false;
					if (tempo_serial == audio_serial) {
						ring_read = CLAMP((uint64_t)MAX(tempo->get_heard_position(), 0.0), ring_read, ring_write);
					}
					step_serial = UINT32_MAX;
				}
				uint64_t from = 0;
				got = _take_frames_locked(p_reader, (uint32_t)p_frames, from, counts);
				if (p_reader == 0) {
					uint32_t done = 0;
					while (done < got) {
						const uint64_t at = (from + done) % ring_capacity;
						const uint32_t run = (uint32_t)MIN((uint64_t)(got - done), ring_capacity - at);
						memcpy((void *)(p_buffer + done), (const void *)&ring[at], (size_t)run * sizeof(AudioFrame));
						done += run;
					}
				} else if (!channel_ring.is_empty()) {
					const float *channel = channel_ring.ptr() + (p_reader - 1);
					for (uint32_t i = 0; i < got; i++) {
						const float v = channel[((from + i) % ring_capacity) * DISCRETE_CHANNELS];
						p_buffer[i] = AudioFrame(v, v);
					}
				} else {
					got = 0; // No discrete channels: a channel stream is silent.
					counts = false;
				}
			}
			if (counts) {
				audio_frames_mixed += got;
				if (got > 0) {
					audio_mixed_since_seek = true;
				}
				while (marker_count > 1 && markers[1].frame <= ring_read) {
					for (uint32_t i = 1; i < marker_count; i++) {
						markers[i - 1] = markers[i];
					}
					marker_count--;
				}
				// The frame being heard: the time stretch's input position is ahead of ring_read (which keeps what its
				// next step may still look at); with discrete channels the end of this mix step.
				uint64_t heard = ring_read;
				if (tempo_active) {
					heard = CLAMP((uint64_t)MAX(tempo->get_heard_position(), 0.0), ring_read, ring_write);
				} else if (!channel_ring.is_empty() && step_serial == audio_serial) {
					heard = step_end;
				}
				uint32_t m = 0;
				while (m + 1 < marker_count && markers[m + 1].frame <= heard) {
					m++;
				}
				if (marker_count > 0 && heard >= markers[m].frame) {
					audio_position = markers[m].time + (double)(heard - markers[m].frame) / output_rate;
				}
			}
		}
		if (!counts) {
			// Another reader of this step keeps the counts.
		} else if (got < (uint32_t)p_frames && !audio_eof) {
			live_playing = false; // Live: buffer live_latency again before going on.
			if (!audio_in_underrun) {
				if (audio_mixed_since_seek) {
					audio_underruns++;
				} else {
					audio_seek_waits++;
				}
				audio_in_underrun = true;
			}
		} else {
			audio_in_underrun = false;
		}
		const uint64_t fill = ring_write - ring_read;
		const uint32_t wanted = audio_space_wanted.get();
		wake = (wanted > 0 && ring_capacity - fill >= wanted) || (double)fill < audio_target_seconds * 0.75 * output_rate;
	}
	if (got < (uint32_t)p_frames) {
		memset((void *)(p_buffer + got), 0, (size_t)(p_frames - got) * sizeof(AudioFrame));
	}
	if (wake && demux_idle.is_set()) {
		MutexLock lock(mutex);
		condition.notify_one();
	}
}

// audio_mutex held. Fills p_buffer through the time stretch; returns the frames made (fewer on an underrun).
uint32_t MediaSource::_mix_tempo_locked(AudioFrame *p_buffer, uint32_t p_frames) {
	if (!tempo_active || tempo_serial != audio_serial) {
		tempo->reset((double)ring_read);
		tempo_serial = audio_serial;
		tempo_active = true;
	}
	uint32_t done = 0;
	while (done < p_frames) {
		if (tempo->pending_count > 0) {
			const uint32_t n = MIN(tempo->pending_count, p_frames - done);
			memcpy((void *)(p_buffer + done), (const void *)(tempo->pending + tempo->pending_offset), (size_t)n * sizeof(AudioFrame));
			tempo->pending_offset += n;
			tempo->pending_count -= n;
			done += n;
			continue;
		}
		const int64_t start = tempo->window_start();
		const int64_t end = tempo->window_end();
		if (end > (int64_t)ring_write) {
			if (audio_eof) {
				ring_read = ring_write; // The last few milliseconds are dropped: the stream is finished.
			}
			break; // Underrun, or the end.
		}
		// The window, out of the ring; frames before ring_read (just after a reset) are silence.
		int64_t frame = start;
		while (frame < end) {
			AudioFrame *destination = tempo->window + (frame - start);
			if (frame < (int64_t)ring_read) {
				const int64_t n = MIN(end, (int64_t)ring_read) - frame;
				memset((void *)destination, 0, (size_t)n * sizeof(AudioFrame));
				frame += n;
				continue;
			}
			const uint64_t at = (uint64_t)frame % ring_capacity;
			const int64_t n = MIN(end - frame, (int64_t)(ring_capacity - at));
			memcpy((void *)destination, (const void *)&ring[at], (size_t)n * sizeof(AudioFrame));
			frame += n;
		}
		if (p_frames - done >= AudioTempo::SEGMENT) {
			tempo->step(tempo->window, p_buffer + done, playback_speed);
			done += AudioTempo::SEGMENT;
		} else {
			tempo->step(tempo->window, tempo->pending, playback_speed);
			tempo->pending_count = AudioTempo::SEGMENT;
			tempo->pending_offset = 0;
		}
		// What comes before the next window isn't needed any more.
		const int64_t keep = tempo->window_start();
		if (keep > (int64_t)ring_read) {
			ring_read = MIN((uint64_t)keep, ring_write);
		}
	}
	return done;
}

void MediaSource::set_playback_speed(double p_speed) {
	const double speed = CLAMP(p_speed, 0.5, 2.0);
	AudioTempo *created = nullptr;
	if (speed != 1.0) {
		bool needed;
		{
			MutexLock audio_lock(audio_mutex);
			needed = tempo == nullptr;
		}
		if (needed) {
			created = memnew(AudioTempo); // Outside the lock the mixer takes.
		}
	}
	MutexLock audio_lock(audio_mutex);
	if (created != nullptr && tempo == nullptr) {
		tempo = created;
		created = nullptr;
	}
	playback_speed = tempo != nullptr ? speed : 1.0;
	if (created != nullptr) {
		memdelete(created); // Another thread made one meanwhile.
	}
}

double MediaSource::get_playback_speed() const {
	MutexLock audio_lock(audio_mutex);
	return playback_speed;
}

double MediaSource::get_audio_position() const {
	MutexLock audio_lock(audio_mutex);
	return audio_position;
}

bool MediaSource::is_audio_finished() const {
	MutexLock audio_lock(audio_mutex);
	return audio_eof && ring_write == ring_read;
}

/* Subtitles */

// Demux thread: the second text track of dual subtitles (-1: none).
void MediaSource::_open_secondary_subtitle_track(int p_track) {
	avcodec_free_context(&secondary_codec);
	current_secondary_track = p_track;
	current_secondary_stream = -1;
	String header;
	if (p_track >= 0 && p_track < (int)subtitle_tracks.size() && subtitle_tracks[p_track].text) {
		const SubtitleTrack &track = subtitle_tracks[p_track];
		const AVStream *stream = format->streams[track.stream_index];
		const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
		secondary_codec = codec ? avcodec_alloc_context3(codec) : nullptr;
		int r = secondary_codec ? avcodec_parameters_to_context(secondary_codec, stream->codecpar) : AVERROR_DECODER_NOT_FOUND;
		if (r >= 0) {
			secondary_codec->pkt_timebase = stream->time_base;
			r = avcodec_open2(secondary_codec, codec, nullptr);
		}
		if (r < 0) {
			avcodec_free_context(&secondary_codec);
			MutexLock lock(mutex);
			last_error = vformat("Subtitle track %d (%s): can't open a decoder: %s.", p_track, track.codec, _av_error(r));
			ERR_PRINT("MediaSource: " + last_error);
		} else {
			current_secondary_stream = track.stream_index;
			if (secondary_codec->subtitle_header && secondary_codec->subtitle_header_size > 0) {
				header = String::utf8((const char *)secondary_codec->subtitle_header, secondary_codec->subtitle_header_size);
			}
		}
	}
	_apply_discard();
	MutexLock lock(mutex);
	secondary_subtitle_header = header;
}

void MediaSource::_handle_secondary_subtitle_packet() {
	const int index = current_secondary_stream;
	const int64_t ts = read_packet->pts != AV_NOPTS_VALUE ? read_packet->pts : read_packet->dts;
	if (ts == AV_NOPTS_VALUE || secondary_codec == nullptr) {
		return;
	}
	const double start = _media_time(ts, index);
	const double packet_duration = read_packet->duration > 0 ? read_packet->duration * av_q2d(format->streams[index]->time_base) : -1.0;
	SubtitleEvent event;
	bool error = false;
	if (_decode_text_subtitle(secondary_codec, read_packet, start, packet_duration, event, error)) {
		event.track = current_secondary_track;
		_push_subtitle(event);
	} else if (error) {
		dstats.subtitle_decode_errors++;
	}
}

void MediaSource::_open_subtitle_track(int p_track) {
	avcodec_free_context(&subtitle_codec);
	current_subtitle_track = p_track;
	current_subtitle_stream = -1;
	String header;
	if (p_track >= 0 && p_track < (int)subtitle_tracks.size()) {
		const SubtitleTrack &track = subtitle_tracks[p_track];
		const AVStream *stream = format->streams[track.stream_index];
		if (track.text || track.bitmap_decoded) {
			const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
			subtitle_codec = codec ? avcodec_alloc_context3(codec) : nullptr;
			int r = subtitle_codec ? avcodec_parameters_to_context(subtitle_codec, stream->codecpar) : AVERROR_DECODER_NOT_FOUND;
			if (r >= 0) {
				subtitle_codec->pkt_timebase = stream->time_base;
				r = avcodec_open2(subtitle_codec, codec, nullptr);
			}
			if (r < 0) {
				avcodec_free_context(&subtitle_codec);
				MutexLock lock(mutex);
				last_error = vformat("Subtitle track %d (%s): can't open a decoder: %s.", p_track, track.codec, _av_error(r));
				ERR_PRINT("MediaSource: " + last_error);
			} else {
				current_subtitle_stream = track.stream_index;
				if (track.text && subtitle_codec->subtitle_header && subtitle_codec->subtitle_header_size > 0) {
					header = String::utf8((const char *)subtitle_codec->subtitle_header, subtitle_codec->subtitle_header_size);
				}
			}
		} else {
			current_subtitle_stream = track.stream_index; // Bitmap or other: packets passed through.
		}
	}
	_apply_discard();
	if (p_track < (int)subtitle_tracks.size()) {
		MutexLock lock(mutex);
		subtitle_header = header;
	}
}

void MediaSource::_push_subtitle(SubtitleEvent &p_event) {
	p_event.serial = demux_serial;
	dstats.subtitle_events++;
	MutexLock lock(mutex);
	if (p_event.serial != serial) {
		return;
	}
	if (p_event.track >= 0 && p_event.track == current_secondary_track) {
		if (secondary_subtitle_track != current_secondary_track) {
			return;
		}
	} else if (subtitle_track != current_subtitle_track) {
		return;
	}
	if (p_event.track < 0) {
		p_event.track = current_subtitle_track;
	}
	if (subtitle_queue.size() >= SUBTITLE_QUEUE_MAX) {
		subtitle_queue.remove_at(0); // Nobody reads them: keep the newest.
	}
	subtitle_queue.push_back(p_event);
}

void MediaSource::_handle_subtitle_packet() {
	const int index = current_subtitle_stream;
	const AVRational time_base = format->streams[index]->time_base;
	const int64_t ts = read_packet->pts != AV_NOPTS_VALUE ? read_packet->pts : read_packet->dts;
	if (ts == AV_NOPTS_VALUE) {
		return;
	}
	const double start = _media_time(ts, index);
	const double packet_duration = read_packet->duration > 0 ? read_packet->duration * av_q2d(time_base) : -1.0;
	if (subtitle_codec == nullptr) {
		SubtitleEvent event;
		event.start = start;
		event.end = packet_duration > 0.0 ? start + packet_duration : -1.0;
		event.bitmap = true;
		event.data.resize(read_packet->size);
		memcpy(event.data.ptrw(), read_packet->data, read_packet->size);
		_push_subtitle(event);
		return;
	}
	SubtitleEvent event;
	bool error = false;
	const bool bitmap = current_subtitle_track >= 0 && current_subtitle_track < (int)subtitle_tracks.size() && subtitle_tracks[current_subtitle_track].bitmap;
	const bool decoded = bitmap ? _decode_bitmap_subtitle(read_packet, start, packet_duration, event, error) : _decode_text_subtitle(subtitle_codec, read_packet, start, packet_duration, event, error);
	if (decoded) {
		_push_subtitle(event);
	} else if (error) {
		dstats.subtitle_decode_errors++;
	}
}

/* Video */

void MediaSource::_deliver_video(AVPacket *p_packet) {
	const int index = video.stream_index;
	const AVRational time_base = format->streams[index]->time_base;
	VideoPacket out;
	out.pts = p_packet->pts != AV_NOPTS_VALUE ? _media_time(p_packet->pts, index) : (p_packet->dts != AV_NOPTS_VALUE ? _media_time(p_packet->dts, index) : next_video_pts);
	out.dts = p_packet->dts != AV_NOPTS_VALUE ? _media_time(p_packet->dts, index) : out.pts;
	const double frame_duration = p_packet->duration > 0 ? p_packet->duration * av_q2d(time_base) : (video.fps > 0.0 ? 1.0 / video.fps : 0.0);
	next_video_pts = MAX(out.pts, out.dts) + frame_duration;
	out.keyframe = (p_packet->flags & AV_PKT_FLAG_KEY) != 0;

	if (pending_reseek >= 0.0) {
		_packet_put(p_packet); // A seek further back is about to happen.
		return;
	}
	if (need_keyframe) {
		if (!out.keyframe) {
			dstats.video_skipped++;
			_packet_put(p_packet);
			return;
		}
		if (seek_wanted >= 0.0 && out.pts > seek_wanted + 0.001 && seek_position > 0.0 && seek_retries < SEEK_RETRIES_MAX) {
			// Containers without a keyframe index (MPEG-TS) are seeked by timestamp: reading starts at the target, and the
			// first keyframe is after it. Go back further (2, 4, 8... s) until the keyframe is at or before the target.
			seek_retries++;
			pending_reseek = MAX(seek_wanted - (double)(1 << seek_retries), 0.0);
			dstats.seek_retries++;
			dstats.video_skipped++;
			_packet_put(p_packet);
			return;
		}
		need_keyframe = false;
		seek_wanted = -1.0;
	}
	if (demux_trick > 0.0) {
		// Trick play: keyframes only, at most one per TRICK_WALL_INTERVAL of wall time at this speed. With an index the
		// next one is reached by a jump (_trick_step, after this packet); without (MPEG-TS) by reading on.
		if (!out.keyframe || (trick_next_pts >= 0.0 && out.pts < trick_next_pts)) {
			dstats.video_skipped++;
			_packet_put(p_packet);
			return;
		}
		trick_next_pts = out.pts + demux_trick * TRICK_WALL_INTERVAL;
		const AVStream *stream = format->streams[index];
		if (format_name != "mpegts" && avformat_index_get_entries_count(stream) > 1) {
			trick_step_to = trick_next_pts;
		}
	}

	if (_is_annexb_codec() && (out.keyframe || need_parameter_sets)) {
		const bool hevc = video.codec == VIDEO_CODEC_HEVC;
		AnnexBSplitter::SPSInfo sps;
		int nal_type = -1;
		const bool has_sps = _scan_access_unit(p_packet->data, (uint64_t)p_packet->size, hevc, sps, &scan_parameter_sets, &nal_type);
		out.nal_type = (int8_t)nal_type;
		if (hdr_sei_pending && out.keyframe && !thread.is_started()) {
			_parse_hdr_sei(p_packet->data, (uint64_t)p_packet->size, hevc);
			hdr_sei_pending = false;
		}
		if (has_sps) {
			out.sps_width = sps.width;
			out.sps_height = sps.height;
			inband_parameter_sets = scan_parameter_sets;
			if (!video.sps.valid && !thread.is_started() && sps.valid) {
				video.sps = sps;
				video.bit_depth = (int)sps.bit_depth;
			}
		} else if (need_parameter_sets) {
			// The first access unit after open or a seek must carry the parameter sets: a decoder (re)starts with it. The
			// mp4toannexb filters add them to IDR pictures, not to other keyframes (recovery points, CRA).
			const LocalVector<uint8_t> &sets = inband_parameter_sets.is_empty() ? config_parameter_sets : inband_parameter_sets;
			if (!sets.is_empty()) {
				AVPacket *with_sets = _packet_get();
				if (with_sets && av_new_packet(with_sets, (int)sets.size() + p_packet->size) == 0) {
					memcpy(with_sets->data, sets.ptr(), sets.size());
					memcpy(with_sets->data + sets.size(), p_packet->data, p_packet->size);
					av_packet_copy_props(with_sets, p_packet);
					_packet_put(p_packet);
					p_packet = with_sets;
					dstats.video_parameter_sets_added++;
					if (_scan_access_unit(p_packet->data, (uint64_t)p_packet->size, hevc, sps, nullptr)) {
						out.sps_width = sps.width;
						out.sps_height = sps.height;
					}
				} else {
					_packet_put(with_sets);
				}
			}
		}
	}
	need_parameter_sets = false;
	if (letterbox_wanted.is_set() && out.keyframe && (out.pts >= letterbox_next_pts || out.pts < letterbox_last_pts - 1.0)) {
		_letterbox_offer(p_packet, out.pts); // The second case: a seek back.
	}

	out.packet = p_packet;
	out.data = p_packet->data;
	out.size = (uint32_t)p_packet->size;
	out.serial = demux_serial;
	dstats.video_packets++;
	dstats.video_bytes += out.size;

	MutexLock lock(mutex);
	if (out.serial != serial || video_count >= VIDEO_QUEUE_MAX) {
		// Read before a seek (or, never expected, past the hard limit).
		av_packet_unref(p_packet);
		packet_pool.push_back(p_packet);
		return;
	}
	video_queue[(video_head + video_count) % VIDEO_QUEUE_MAX] = out;
	video_count++;
	video_queued_bytes += out.size;
	_count_video_bytes_locked(out.pts, out.size);
	if (video_count == 1) {
		_wake_consumer_locked();
	}
}

void MediaSource::_receive_from_bsf() {
	for (;;) {
		AVPacket *packet = _packet_get();
		if (packet == nullptr) {
			return;
		}
		if (av_bsf_receive_packet(bsf, packet) < 0) {
			_packet_put(packet);
			return;
		}
		_deliver_video(packet);
	}
}

void MediaSource::_handle_video_packet() {
	if (bsf) {
		// The filter takes the reference; the Annex-B output is the one copy of the access unit before the decoder's.
		if (av_bsf_send_packet(bsf, read_packet) < 0) {
			av_packet_unref(read_packet);
			dstats.video_skipped++;
			return;
		}
		_receive_from_bsf();
	} else {
		AVPacket *packet = _packet_get();
		if (packet == nullptr) {
			return;
		}
		av_packet_move_ref(packet, read_packet);
		_deliver_video(packet);
	}
}

/* Demux thread */

bool MediaSource::_read_packet() {
	const int r = av_read_frame(format, read_packet);
	if (r < 0) {
		if (r == AVERROR_EOF || (format->pb && avio_feof(format->pb))) {
			eof_reached = true;
		} else if (r == AVERROR(EAGAIN)) {
			OS::get_singleton()->delay_usec(1000);
		} else if (smb_file && smb_file->has_failed()) {
			dstats.read_errors++;
			_set_error(smb_file->get_error());
			eof_reached = true;
		} else if (net_stream && net_stream->has_failed()) {
			dstats.read_errors++;
			_set_error(net_stream->get_error());
			eof_reached = true;
		} else {
			dstats.read_errors++;
			if (++read_errors_in_row >= 64) {
				_set_error(vformat("Reading %s failed: %s.", path, _av_error(r)));
				eof_reached = true;
			}
		}
		return false;
	}
	read_errors_in_row = 0;
	dstats.packets_read++;
	dstats.bytes_read += (uint64_t)read_packet->size;
	if (read_packet->stream_index == video.stream_index) {
		_handle_video_packet();
	} else if (demux_trick > 0.0) {
		// Trick play: no audio or subtitles (most demuxers drop them already, AVDISCARD_ALL).
	} else if (read_packet->stream_index == current_audio_stream) {
		dstats.audio_packets++;
		if (scratch_frames > 0 || audio_fifo_count > 0 || _holding_audio()) {
			// The ring is full, or the seek's keyframe isn't found yet: keep the packet compressed and read on.
			AVPacket *queued = _packet_get();
			if (queued) {
				av_packet_move_ref(queued, read_packet);
				_audio_fifo_push(queued);
			}
		} else {
			_decode_audio(read_packet);
		}
	} else if (read_packet->stream_index == current_subtitle_stream) {
		_handle_subtitle_packet();
	} else if (read_packet->stream_index == current_secondary_stream) {
		_handle_secondary_subtitle_packet();
	}
	av_packet_unref(read_packet);
	if (pending_reseek >= 0.0) {
		const double position = pending_reseek;
		pending_reseek = -1.0;
		_seek_file(position);
	}
	if (trick_step_to >= 0.0) {
		const double to = trick_step_to;
		trick_step_to = -1.0;
		_trick_step(to);
	}
	return true;
}

// Trick play with an index: jump to the first keyframe at or after p_to, so what lies between isn't read. Past the last
// keyframe: the end.
void MediaSource::_trick_step(double p_to) {
	const int stream = video.stream_index;
	const double seconds_per_tick = av_q2d(format->streams[stream]->time_base);
	const int64_t ts = (int64_t)Math::round((p_to + start_time) / seconds_per_tick);
	if (avformat_seek_file(format, stream, ts, ts, INT64_MAX, 0) < 0) {
		eof_reached = true;
		return;
	}
	if (bsf) {
		av_bsf_flush(bsf);
	}
	need_parameter_sets = true; // A keyframe that isn't an IDR gets the parameter sets put in front.
	dstats.trick_steps++;
}

// After a seek, audio waits (compressed) until the video's keyframe is found: if the file has to be seeked again further
// back, what was read meanwhile would come out of order.
bool MediaSource::_holding_audio() const {
	return need_keyframe && video.stream_index >= 0 && !eof_reached && audio_fifo_count + 16 < AUDIO_FIFO_MAX;
}

void MediaSource::_drain_at_eof() {
	if (bsf) {
		av_bsf_send_packet(bsf, nullptr);
		_receive_from_bsf();
	}
	eof_drained = true;
	_decode_audio(nullptr);
	if (audio_codec == nullptr || scratch_frames == 0) {
		MutexLock audio_lock(audio_mutex);
		if (audio_serial == demux_serial) {
			audio_eof = true;
		}
	}
	MutexLock lock(mutex);
	if (serial == demux_serial) {
		demux_eof = true;
		if (state == STATE_READING) {
			state = STATE_ENDED;
		}
		_wake_consumer_locked();
	}
}

// Positions the file at the keyframe at or before p_position (where the container has an index; else by timestamp) and
// flushes everything read or decoded before.
void MediaSource::_seek_file(double p_position) {
	seek_position = p_position;
	const int stream = video.stream_index >= 0 ? video.stream_index : current_audio_stream;
	int r;
	if (p_position <= 0.0 && format_name == "mpegts") {
		// MPEG-TS has no index: a timestamp seek to the start can land just after the first keyframe's start (its PES is
		// large), and the first keyframe found is then the next one. The start of the file is byte 0.
		r = av_seek_frame(format, -1, 0, AVSEEK_FLAG_BYTE);
	} else if (stream >= 0) {
		// The keyframe at or before the target (max_ts = target).
		const double seconds_per_tick = av_q2d(format->streams[stream]->time_base);
		const int64_t ts = (int64_t)Math::round((p_position + start_time) / seconds_per_tick);
		r = avformat_seek_file(format, stream, INT64_MIN, ts, ts, 0);
		if (r < 0) {
			r = av_seek_frame(format, stream, ts, AVSEEK_FLAG_BACKWARD);
		}
	} else {
		const int64_t ts = (int64_t)Math::round((p_position + start_time) * AV_TIME_BASE);
		r = avformat_seek_file(format, -1, INT64_MIN, ts, ts, 0);
	}
	if (r < 0) {
		MutexLock lock(mutex);
		last_error = vformat("Seeking to %.3f s failed: %s.", p_position, _av_error(r));
		ERR_PRINT("MediaSource: " + last_error);
	}
	if (bsf) {
		av_bsf_flush(bsf);
	}
	if (audio_codec) {
		avcodec_flush_buffers(audio_codec);
		_apply_audio_decoder_settings(); // The AC-3 decoder's flush cleared them.
	}
	if (subtitle_codec) {
		avcodec_flush_buffers(subtitle_codec);
	}
	_audio_fifo_clear();
	swr_free(&swr); // Its delay line holds audio from before the seek; reconfigured with the next frame.
	swr_in_format = -1;
	scratch_frames = 0;
	scratch_offset = 0;
	need_keyframe = true;
	need_parameter_sets = true;
	eof_reached = false;
	eof_drained = false;
	read_errors_in_row = 0;
	pending_reseek = -1.0;
}

void MediaSource::_do_seek(double p_target, double p_trick) {
	if (p_trick != demux_trick) {
		demux_trick = p_trick;
		_apply_discard();
	}
	trick_next_pts = -1.0;
	trick_step_to = -1.0;
	seek_wanted = p_target;
	seek_retries = 0;
	_seek_file(p_target);
	audio_trim_until = p_target;
	audio_next_time = p_target;
	next_video_pts = p_target;
	if (current_audio_stream < 0 || demux_trick > 0.0) {
		MutexLock audio_lock(audio_mutex);
		audio_eof = true;
	}
}

void MediaSource::_thread_loop() {
	for (;;) {
		bool do_seek = false;
		double target = 0.0;
		double trick = 0.0;
		int track;
		int sub_track;
		int sub_track2;
		bool dsp_changed = false;
		{
			MutexLock lock(mutex);
			if (exit_requested) {
				return;
			}
			if (audio_dsp_serial != dsp_serial) {
				dsp_serial = audio_dsp_serial;
				dsp_night_mode = night_mode;
				dsp_strength = (float)night_mode_strength;
				dsp_dialogue_boost = (float)dialogue_boost;
				dsp_changed = true;
			}
			if (seek_pending) {
				do_seek = true;
				seek_pending = false;
				target = seek_target;
				trick = trick_speed;
				demux_serial = serial;
			}
			track = audio_track;
			sub_track = subtitle_track;
			sub_track2 = secondary_subtitle_track;
			dstats.thread_cpu_seconds = thread_cpu_seconds();
			stats = dstats;
		}
		if (track != current_audio_track) {
			_open_audio_track(track);
		}
		if (dsp_changed) {
			_update_audio_dsp();
		}
		if (sub_track2 != current_secondary_track) {
			_open_secondary_subtitle_track(sub_track2);
		}
		if (sub_track != current_subtitle_track) {
			_open_subtitle_track(sub_track);
		}
		if (do_seek) {
			_do_seek(target, trick);
		}

		bool progress = false;
		// Audio: decoded audio into the ring; the next queued packet once the last one is in.
		if (scratch_frames > 0) {
			const uint32_t before = scratch_frames;
			_flush_scratch();
			progress = scratch_frames != before;
		}
		if (scratch_frames == 0 && audio_fifo_count > 0 && !_holding_audio()) {
			AVPacket *packet = _audio_fifo_pop();
			_decode_audio(packet);
			_packet_put(packet);
			progress = true;
		}
		// Read while a queue is short (audio read while the ring is full waits compressed in audio_fifo).
		if (!eof_reached) {
			bool want;
			{
				MutexLock lock(mutex);
				want = !seek_pending && state != STATE_ERROR && _needs_input_locked();
			}
			if (want) {
				_read_packet();
				progress = true;
			}
		} else if (!eof_drained && scratch_frames == 0 && audio_fifo_count == 0) {
			_drain_at_eof();
			progress = true;
		}
		if (progress) {
			continue;
		}

		// Nothing to do: wait for the consumers (video pops, audio mixes), a seek, a track change or close().
		audio_space_wanted.set(scratch_frames);
		MutexLock lock(mutex);
		demux_idle.set();
		bool work = exit_requested || seek_pending || audio_track != current_audio_track || subtitle_track != current_subtitle_track || secondary_subtitle_track != current_secondary_track;
		if (!work && state != STATE_ERROR) {
			if (scratch_frames > 0) {
				MutexLock audio_lock(audio_mutex);
				work = audio_serial != scratch_serial || ring_capacity - (ring_write - ring_read) >= scratch_frames;
			} else {
				work = audio_fifo_count > 0 && !_holding_audio();
			}
			if (!work) {
				work = eof_reached ? (!eof_drained && scratch_frames == 0 && audio_fifo_count == 0) : _needs_input_locked();
			}
		}
		if (!work) {
			dstats.thread_cpu_seconds = thread_cpu_seconds();
			stats = dstats;
			condition.wait(lock);
		}
		demux_idle.clear();
		audio_space_wanted.set(0);
	}
}

void MediaSource::_thread_func(void *p_userdata) {
	static_cast<MediaSource *>(p_userdata)->_thread_loop();
}

/* Public API */

Error MediaSource::open(const String &p_path) {
	close();
	{
		MutexLock lock(mutex);
		state = STATE_CLOSED;
		last_error = String();
		serial = 0;
		seek_pending = false;
		seek_target = 0.0;
		exit_requested = false;
		demux_eof = false;
		last_video_pts = -1.0;
		stats = Stats();
		seek_count = 0;
		video_head = video_count = 0;
		video_queued_bytes = 0;
		for (BitrateSecond &entry : bitrate_seconds) {
			entry = BitrateSecond();
		}
	}
	dstats = Stats();
	dsp_serial = UINT32_MAX; // Night mode and dialogue boost are applied again by the new thread.
	path = p_path;
	const bool smb = SmbUrl::is_smb(p_path);
	const bool web = NetStream::is_url(p_path);
	const bool network = smb || web;
	const String global_path = network || !ProjectSettings::get_singleton() ? p_path : ProjectSettings::get_singleton()->globalize_path(p_path);

	AVFormatContext *context = nullptr;
	if (network) {
		// Blocking: connect, log in, open (up to about 8 s each when the host doesn't answer).
		String error;
		void *opaque;
		int (*read_function)(void *, uint8_t *, int);
		int64_t (*seek_function)(void *, int64_t, int);
		if (smb) {
			smb_file = memnew(SmbFile);
			if (smb_file->open(p_path, (uint64_t)network_buffer_size, (uint32_t)network_requests, error) != OK) {
				_close_network();
				_set_error(error);
				return ERR_FILE_CANT_OPEN;
			}
			print_line(vformat("MediaSource: %s: %.2f GB over %s.", p_path, (double)smb_file->get_size() / 1e9, smb_file->get_description()));
			opaque = smb_file;
			read_function = &SmbFile::avio_read;
			seek_function = &SmbFile::avio_seek;
		} else {
			net_stream = memnew(NetStream);
			if (net_stream->open(p_path, (uint64_t)network_buffer_size, network_tls_verify, error) != OK) {
				_close_network();
				_set_error(error);
				return ERR_FILE_CANT_OPEN;
			}
			print_line(vformat("MediaSource: %s: %s.", p_path, net_stream->get_description()));
			opaque = net_stream;
			read_function = &NetStream::avio_read;
			seek_function = &NetStream::avio_seek;
		}
		uint8_t *buffer = (uint8_t *)av_malloc(AVIO_BUFFER_SIZE);
		avio = buffer ? avio_alloc_context(buffer, AVIO_BUFFER_SIZE, 0, opaque, read_function, nullptr, seek_function) : nullptr;
		if (avio && net_stream && !net_stream->is_seekable()) {
			avio->seekable = 0; // Live: the demuxers read on and don't try to seek.
		}
		context = avio ? avformat_alloc_context() : nullptr;
		if (!context) {
			if (!avio) {
				av_free(buffer);
			}
			_close_network();
			_set_error("Out of memory.");
			return ERR_OUT_OF_MEMORY;
		}
		context->pb = avio;
		context->flags |= AVFMT_FLAG_CUSTOM_IO;
		if (net_stream && net_stream->is_live()) {
			// Live: probe briefly (what is probed is played, so the probe is the start-up delay).
			context->probesize = 2 << 20;
			context->max_analyze_duration = (int64_t)(MAX(live_latency, 0.3) * AV_TIME_BASE);
		}
	}
	int r = avformat_open_input(&context, global_path.utf8().get_data(), nullptr, nullptr);
	if (r < 0) {
		// avformat_open_input() freed the context; a custom AVIOContext stays ours.
		_close_network();
		_set_error(vformat("Can't open %s: %s.", global_path, _av_error(r)));
		return ERR_FILE_CANT_OPEN;
	}
	format = context;
	r = avformat_find_stream_info(format, nullptr);
	if (r < 0) {
		_set_error(vformat("Can't read the streams of %s: %s.", global_path, _av_error(r)));
		_close_internal();
		return ERR_FILE_UNRECOGNIZED;
	}
	if (net_stream && net_stream->is_live()) {
		// Bound the delay: a reader more than 2 x live_latency behind the newest data skips ahead.
		net_stream->set_live_limit(live_latency * 2.0);
	}
	format_name = format->iformat && format->iformat->name ? String::utf8(format->iformat->name) : String();
	start_time = format->start_time != AV_NOPTS_VALUE ? (double)format->start_time / AV_TIME_BASE : 0.0;
	duration = format->duration != AV_NOPTS_VALUE ? (double)format->duration / AV_TIME_BASE : 0.0;
	bit_rate = format->bit_rate;
	_read_chapters();

	// Cover art (music files, Matroska image attachments) comes as a video stream with one packet: never the video.
	int best_video = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
	if (best_video >= 0 && (format->streams[best_video]->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
		best_video = -1;
		for (unsigned i = 0; i < format->nb_streams && best_video < 0; i++) {
			if (format->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && !(format->streams[i]->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
				best_video = (int)i;
			}
		}
	}
	for (unsigned i = 0; i < format->nb_streams; i++) {
		const AVStream *stream = format->streams[i];
		if ((stream->disposition & AV_DISPOSITION_ATTACHED_PIC) && stream->attached_pic.size > 0) {
			AttachedPicture picture;
			picture.codec = String::utf8(avcodec_get_name(stream->codecpar->codec_id));
			const AVDictionaryEntry *comment = av_dict_get(stream->metadata, "comment", nullptr, 0);
			const AVDictionaryEntry *title = av_dict_get(stream->metadata, "title", nullptr, 0);
			const AVDictionaryEntry *filename = av_dict_get(stream->metadata, "filename", nullptr, 0);
			picture.title = comment ? String::utf8(comment->value) : (title ? String::utf8(title->value) : (filename ? String::utf8(filename->value) : String()));
			picture.data.resize(stream->attached_pic.size);
			memcpy(picture.data.ptrw(), stream->attached_pic.data, stream->attached_pic.size);
			pictures.push_back(picture);
		}
	}
	const int best_audio = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, best_video >= 0 ? best_video : -1, nullptr, 0);
	int default_track = -1;
	for (unsigned i = 0; i < format->nb_streams; i++) {
		const AVStream *stream = format->streams[i];
		const AVCodecParameters *par = stream->codecpar;
		const AVDictionaryEntry *language = av_dict_get(stream->metadata, "language", nullptr, 0);
		const AVDictionaryEntry *title = av_dict_get(stream->metadata, "title", nullptr, 0);
		if (par->codec_type == AVMEDIA_TYPE_AUDIO) {
			AudioTrack track;
			track.stream_index = (int)i;
			track.codec = String::utf8(avcodec_get_name(par->codec_id));
			track.channels = par->ch_layout.nb_channels;
			char layout[128] = {};
			av_channel_layout_describe(&par->ch_layout, layout, sizeof(layout));
			track.channel_layout = String::utf8(layout);
			track.sample_rate = par->sample_rate;
			track.bit_rate = par->bit_rate;
			track.language = language ? String::utf8(language->value) : String();
			track.title = title ? String::utf8(title->value) : String();
			track.is_default = (stream->disposition & AV_DISPOSITION_DEFAULT) != 0;
			const String lower_title = track.title.to_lower();
			track.commentary = (stream->disposition & AV_DISPOSITION_COMMENT) != 0 || lower_title.contains("commentary");
			track.visual_impaired = (stream->disposition & AV_DISPOSITION_VISUAL_IMPAIRED) != 0 || lower_title.contains("description") || lower_title.contains("descriptive");
			if ((int)i == best_audio) {
				default_track = (int)audio_tracks.size();
			}
			audio_tracks.push_back(track);
		} else if (par->codec_type == AVMEDIA_TYPE_SUBTITLE) {
			SubtitleTrack track;
			track.stream_index = (int)i;
			track.codec = String::utf8(avcodec_get_name(par->codec_id));
			track.text = _is_text_subtitle(par->codec_id) && avcodec_find_decoder(par->codec_id) != nullptr;
			track.bitmap = _is_bitmap_subtitle(par->codec_id);
			track.bitmap_decoded = track.bitmap && avcodec_find_decoder(par->codec_id) != nullptr;
			track.language = language ? String::utf8(language->value) : String();
			track.title = title ? String::utf8(title->value) : String();
			track.is_default = (stream->disposition & AV_DISPOSITION_DEFAULT) != 0;
			const String lower_title = track.title.to_lower();
			track.forced = (stream->disposition & AV_DISPOSITION_FORCED) != 0 || lower_title.contains("forced");
			track.hearing_impaired = (stream->disposition & AV_DISPOSITION_HEARING_IMPAIRED) != 0 || lower_title.contains("sdh") || lower_title.contains("hearing");
			subtitle_tracks.push_back(track);
		}
	}

	if (best_video >= 0) {
		const AVStream *stream = format->streams[best_video];
		const AVCodecParameters *par = stream->codecpar;
		video.valid = true;
		video.stream_index = best_video;
		video.codec_name = String::utf8(avcodec_get_name(par->codec_id));
		switch (par->codec_id) {
			case AV_CODEC_ID_H264:
				video.codec = VIDEO_CODEC_H264;
				break;
			case AV_CODEC_ID_HEVC:
				video.codec = VIDEO_CODEC_HEVC;
				break;
			case AV_CODEC_ID_VP9:
				video.codec = VIDEO_CODEC_VP9;
				break;
			case AV_CODEC_ID_AV1:
				video.codec = VIDEO_CODEC_AV1;
				break;
			default:
				video.codec = VIDEO_CODEC_OTHER;
				break;
		}
		video.width = par->width;
		video.height = par->height;
		_read_stream_metadata(stream);
		_read_hdr_side_data(stream);
		const AVRational rate = stream->avg_frame_rate.num > 0 && stream->avg_frame_rate.den > 0 ? stream->avg_frame_rate : stream->r_frame_rate;
		video.fps = rate.num > 0 && rate.den > 0 ? av_q2d(rate) : 0.0;
		const AVRational sar = av_guess_sample_aspect_ratio(format, format->streams[best_video], nullptr);
		video.sample_aspect_ratio = sar.num > 0 && sar.den > 0 ? av_q2d(sar) : 1.0;
		const AVPixFmtDescriptor *pixel = av_pix_fmt_desc_get((AVPixelFormat)par->format);
		video.pixel_format = pixel ? String::utf8(pixel->name) : String();
		const char *primaries = par->color_primaries != AVCOL_PRI_UNSPECIFIED ? av_color_primaries_name(par->color_primaries) : nullptr;
		const char *transfer = par->color_trc != AVCOL_TRC_UNSPECIFIED ? av_color_transfer_name(par->color_trc) : nullptr;
		const char *matrix = par->color_space != AVCOL_SPC_UNSPECIFIED ? av_color_space_name(par->color_space) : nullptr;
		const char *range = par->color_range != AVCOL_RANGE_UNSPECIFIED ? av_color_range_name(par->color_range) : nullptr;
		video.color_primaries = primaries ? String::utf8(primaries) : String();
		video.color_transfer = transfer ? String::utf8(transfer) : String();
		video.color_matrix = matrix ? String::utf8(matrix) : String();
		video.color_range = range ? String::utf8(range) : String();
		static const char *field_orders[] = { "", "progressive", "tt", "bb", "tb", "bt" }; // AVFieldOrder.
		video.field_order = par->field_order >= AV_FIELD_UNKNOWN && par->field_order <= AV_FIELD_BT ? String(field_orders[par->field_order]) : String();
		video.bit_depth = pixel ? pixel->comp[0].depth : par->bits_per_raw_sample;
		const char *profile = avcodec_profile_name(par->codec_id, par->profile);
		video.profile = profile ? String::utf8(profile) : String();
		if (_is_annexb_codec()) {
			const bool hevc = video.codec == VIDEO_CODEC_HEVC;
			_config_to_annexb(par->extradata, par->extradata_size, hevc, config_parameter_sets);
			if (!config_parameter_sets.is_empty()) {
				AnnexBSplitter::SPSInfo sps;
				if (_scan_access_unit(config_parameter_sets.ptr(), config_parameter_sets.size(), hevc, sps, nullptr) && sps.valid) {
					video.sps = sps;
					video.bit_depth = (int)sps.bit_depth;
				}
			}
			if (video.pixel_format.is_empty() && video.sps.valid) {
				// libavformat sets the pixel format only when it can decode; without video decoders, say what the SPS says.
				static const char *chroma_names[] = { "gray", "yuv420p", "yuv422p", "yuv444p" };
				video.pixel_format = chroma_names[MIN(video.sps.chroma_format, 3u)];
				if (video.sps.bit_depth > 8) {
					video.pixel_format += itos(video.sps.bit_depth) + "le";
				}
			}
			if (par->extradata_size > 0 && par->extradata[0] == 1) {
				// Length-prefixed NAL units (avcC/hvcC): convert to Annex-B. The filter also puts the parameter sets in
				// front of every IDR picture.
				const AVBitStreamFilter *filter = av_bsf_get_by_name(hevc ? "hevc_mp4toannexb" : "h264_mp4toannexb");
				r = filter ? av_bsf_alloc(filter, &bsf) : AVERROR_BSF_NOT_FOUND;
				if (r >= 0) {
					r = avcodec_parameters_copy(bsf->par_in, par);
				}
				if (r >= 0) {
					bsf->time_base_in = stream->time_base;
					r = av_bsf_init(bsf);
				}
				if (r < 0) {
					_set_error(vformat("Can't set up the Annex-B conversion of the %s stream: %s.", video.codec_name, _av_error(r)));
					_close_internal();
					return ERR_CANT_CREATE;
				}
			}
		}
	}
	video_queue_target = _video_queue_target();
	{
		MutexLock lock(mutex);
		subtitle_video_target = _subtitle_video_target();
	}

	output_rate = AudioServer::get_singleton() ? (int)AudioServer::get_singleton()->get_mix_rate() : 48000;
	if (output_rate <= 0) {
		output_rate = 48000;
	}
	ring_capacity = (uint64_t)(output_rate * AUDIO_RING_SECONDS);
	ring.resize(ring_capacity);
	{
		MutexLock audio_lock(audio_mutex);
		if (discrete_channels) {
			channel_ring.resize(ring_capacity * DISCRETE_CHANNELS);
		} else {
			channel_ring.clear();
		}
		step_serial = UINT32_MAX;
		ring_read = ring_write = 0;
		marker_count = 0;
		audio_position = 0.0;
		audio_serial = 0;
		audio_eof = false;
		audio_in_underrun = false;
		audio_mixed_since_seek = false;
		live_preroll_frames = is_live() ? (uint64_t)(live_latency * output_rate) : 0;
		live_playing = false;
		// The thread must read past the pre-roll, or the hold would never end.
		audio_target_seconds = is_live() ? MAX(audio_queue_seconds, live_latency + 0.25) : audio_queue_seconds;
		audio_underruns = 0;
		audio_seek_waits = 0;
		audio_frames_mixed = 0;
	}
	read_packet = av_packet_alloc();
	audio_frame = av_frame_alloc();
	demux_serial = 0;
	need_keyframe = true;
	need_parameter_sets = true;
	seek_wanted = -1.0;
	seek_position = 0.0;
	seek_retries = 0;
	pending_reseek = -1.0;
	eof_reached = eof_drained = false;
	read_errors_in_row = 0;
	inband_parameter_sets.clear();
	next_video_pts = 0.0;
	audio_trim_until = -1.0;
	audio_next_time = 0.0;
	scratch_frames = scratch_offset = 0;
	audio_space_wanted.set(0);
	demux_idle.clear();

	{
		MutexLock lock(mutex);
		audio_track = default_track;
		subtitle_track = -1;
		secondary_subtitle_track = -1;
		secondary_subtitle_header = String();
		subtitle_queue.clear();
		subtitle_header = String();
	}
	_open_audio_track(default_track);
	current_subtitle_track = -1;
	current_subtitle_stream = -1;
	current_secondary_track = -1;
	current_secondary_stream = -1;

	// Without a codec configuration (MPEG-TS), the coded size comes from the first SPS in the stream: read up to it now
	// (what is read is queued as usual).
	if (_is_annexb_codec() && !video.sps.valid) {
		for (int i = 0; i < 4000 && !video.sps.valid && !eof_reached && get_state() != STATE_ERROR; i++) {
			_read_packet();
		}
		if (!video.sps.valid) {
			print_line(vformat("MediaSource: no %s SPS found near the start of %s.", video.codec_name, global_path));
		}
	}
	// HDR video whose container lacks the mastering display or light level metadata (most rips): read up to the first
	// keyframe now and take them from its SEI (what is read is queued as usual).
	hdr_sei_pending = _is_annexb_codec() && (video.color_transfer == "smpte2084" || video.color_transfer == "arib-std-b67") &&
			!(video.mastering_luminance && video.light_level);
	if (hdr_sei_pending) {
		for (int i = 0; i < 4000 && hdr_sei_pending && !eof_reached && get_state() != STATE_ERROR; i++) {
			_read_packet();
		}
		hdr_sei_pending = false;
		print_line(vformat("MediaSource: HDR metadata %s: mastering display %s, MaxCLL %d, MaxFALL %d%s%s.", video.hdr_metadata_source.is_empty() ? String("none") : video.hdr_metadata_source,
				video.mastering_luminance ? vformat("%.4f-%.0f nits", video.mastering_min_nits, video.mastering_max_nits) : String("unknown"), video.max_cll, video.max_fall,
				video.hdr10plus ? ", HDR10+" : "", video.dolby_vision_profile >= 0 ? vformat(", Dolby Vision profile %d", video.dolby_vision_profile) : String()));
	}

	if (get_state() == STATE_ERROR) {
		_close_internal();
		return FAILED;
	}
	{
		MutexLock lock(mutex);
		state = STATE_READING;
	}

	String summary = vformat("MediaSource: %s: %s, %.1f s", global_path, format_name, duration);
	if (video.valid) {
		summary += vformat(", video %s %dx%d %.3f fps %d-bit", video.codec_name, video.width, video.height, video.fps, video.bit_depth);
		if (video.sps.valid) {
			summary += vformat(" (SPS %dx%d)", video.sps.width, video.sps.height);
		}
	}
	if (video.valid && (!video.stereo_mode.is_empty() || !video.projection.is_empty())) {
		summary += vformat(" (stereo %s%s, projection %s)", video.stereo_mode.is_empty() ? String("not given") : video.stereo_mode, video.stereo_inverted ? " inverted" : "",
				video.projection.is_empty() ? String("flat") : vformat("%s %.0fx%.0f", video.projection, video.projection_fov_h, video.projection_fov_v));
	}
	if (!chapters.is_empty()) {
		summary += vformat(", %d chapters", chapters.size());
	}
	if (default_track >= 0) {
		const AudioTrack &track = audio_tracks[default_track];
		summary += vformat(", audio track %d of %d: %s %s %d Hz", default_track, audio_tracks.size(), track.codec, track.channel_layout, track.sample_rate);
	}
	summary += vformat(", %d subtitle track(s).", subtitle_tracks.size());
	print_line(summary);

	_letterbox_start();
	thread.start(&MediaSource::_thread_func, this);
	return OK;
}

void MediaSource::_close_network() {
	if (avio) {
		av_freep(&avio->buffer);
		avio_context_free(&avio);
	}
	if (smb_file) {
		smb_file->close();
		memdelete(smb_file);
		smb_file = nullptr;
	}
	if (net_stream) {
		net_stream->close();
		memdelete(net_stream);
		net_stream = nullptr;
	}
}

void MediaSource::_close_internal() {
	lines_generation.increment();
	if (smb_file) {
		smb_file->abort(); // The demux thread may wait for network data.
	}
	if (net_stream) {
		net_stream->abort();
	}
	if (thread.is_started()) {
		{
			MutexLock lock(mutex);
			exit_requested = true;
			condition.notify_all();
		}
		thread.wait_to_finish();
	}
	_letterbox_stop(); // After the demux thread: nothing is offered any more.
	demux_trick = 0.0;
	trick_next_pts = -1.0;
	trick_step_to = -1.0;
	{
		MutexLock lock(mutex);
		trick_speed = 0.0;
	}
	_close_audio_codec();
	avcodec_free_context(&subtitle_codec);
	current_subtitle_track = -1;
	current_subtitle_stream = -1;
	avcodec_free_context(&secondary_codec);
	current_secondary_track = -1;
	current_secondary_stream = -1;
	av_bsf_free(&bsf);
	av_packet_free(&read_packet);
	av_frame_free(&audio_frame);
	{
		MutexLock lock(mutex);
		_flush_video_queue_locked();
		demux_eof = false;
	}
	if (format) {
		avformat_close_input(&format);
	}
	_close_network();
	video = VideoInfo();
	swr_description = String();
	audio_tracks.clear();
	{
		MutexLock lock(mutex);
		subtitle_tracks.clear(); // get_subtitle_lines() reads it from any thread.
	}
	chapters.clear();
	pictures.clear();
	config_parameter_sets.clear();
	inband_parameter_sets.clear();
	current_audio_track = -1;
	current_audio_stream = -1;
	{
		MutexLock audio_lock(audio_mutex);
		ring.clear();
		channel_ring.clear();
		ring_capacity = 0;
		ring_read = ring_write = 0;
		marker_count = 0;
		audio_eof = true;
	}
}

void MediaSource::close() {
	_close_internal();
	MutexLock lock(mutex);
	if (state != STATE_ERROR) {
		state = STATE_CLOSED;
	}
	audio_track = -1;
	subtitle_track = -1;
	secondary_subtitle_track = -1;
	subtitle_queue.clear();
	subtitle_header = String();
	external_subtitles.clear();
	external_cursor = 0;
}

String MediaSource::get_hw_unsupported_reason(const VideoInfo &p_info, bool p_allow_10bit) {
	if (!p_info.valid) {
		return "There is no video stream.";
	}
	switch (p_info.codec) {
		case VIDEO_CODEC_H264:
		case VIDEO_CODEC_HEVC:
			break;
		case VIDEO_CODEC_VP9:
			// Policy, not a driver refusal: the device is never opened for VP9 (docs/research-device.md, SteamOS #2865).
			return "VP9 video isn't played in hardware: on this SteamOS kernel the video decoder's firmware crashes on VP9 (from the second VP9 session after boot), which can restart the headset.";
		case VIDEO_CODEC_AV1:
			return "AV1 video can't be decoded in hardware: this SteamOS kernel doesn't expose the AV1 decoder.";
		default:
			return vformat("%s video can't be decoded in hardware (the Frame decodes H.264 and HEVC).", p_info.codec_name.to_upper());
	}
	const int bit_depth = MAX(p_info.bit_depth, (int)p_info.sps.bit_depth);
	if (bit_depth > 8 && !p_allow_10bit) {
		return vformat("%d-bit video is not supported yet on this kernel: the decoder writes it only in a compressed 10-bit format (Q10C) that the player can't display yet.", bit_depth);
	}
	if (bit_depth > 10) {
		return vformat("%d-bit video can't be decoded in hardware (8 and 10 bits only).", bit_depth);
	}
	if (p_info.sps.valid && p_info.sps.chroma_format != 1) {
		return vformat("%s video can't be decoded in hardware (4:2:0 only).", p_info.sps.chroma_format == 2 ? "4:2:2" : (p_info.sps.chroma_format == 3 ? "4:4:4" : "Monochrome"));
	}
	return String();
}

Dictionary MediaSource::get_video_info() const {
	Dictionary d;
	if (!video.valid) {
		return d;
	}
	static const char *codec_names[] = { "", "h264", "hevc", "vp9", "av1", "other" };
	d["stream_index"] = video.stream_index;
	d["codec"] = video.codec == VIDEO_CODEC_OTHER ? video.codec_name : String(codec_names[video.codec]);
	d["codec_name"] = video.codec_name;
	d["width"] = video.width;
	d["height"] = video.height;
	d["fps"] = video.fps;
	d["sample_aspect_ratio"] = video.sample_aspect_ratio;
	d["display_aspect"] = video.height > 0 ? video.width * video.sample_aspect_ratio / video.height : 0.0;
	d["bit_depth"] = video.bit_depth;
	d["profile"] = video.profile;
	d["pixel_format"] = video.pixel_format;
	d["color_primaries"] = video.color_primaries;
	d["color_transfer"] = video.color_transfer;
	d["color_matrix"] = video.color_matrix;
	d["color_range"] = video.color_range;
	d["field_order"] = video.field_order;
	// What the shader needs: the matrix (BT.2020 when the stream says so, else BT.709, the usual for HD and up), the
	// range, and whether the transfer is HDR (PQ or HLG: needs tone mapping).
	d["matrix_bt2020"] = video.color_matrix.begins_with("bt2020");
	d["full_range"] = video.color_range == "pc";
	d["hdr"] = video.color_transfer == "smpte2084" || video.color_transfer == "arib-std-b67";
	// HDR metadata (get_tone_mapping() takes this dictionary).
	d["hdr_transfer"] = video.color_transfer == "smpte2084" ? "pq" : (video.color_transfer == "arib-std-b67" ? "hlg" : "");
	d["max_cll"] = video.max_cll;
	d["max_fall"] = video.max_fall;
	PackedVector2Array mastering;
	if (video.mastering_primaries) {
		for (int i = 0; i < 4; i++) {
			mastering.push_back(Vector2(video.mastering_display[i * 2], video.mastering_display[i * 2 + 1]));
		}
	}
	d["mastering_display_primaries"] = mastering;
	d["mastering_display_luminance"] = video.mastering_luminance ? Vector2(video.mastering_min_nits, video.mastering_max_nits) : Vector2();
	d["hdr_metadata_source"] = video.hdr_metadata_source;
	d["dolby_vision_profile"] = video.dolby_vision_profile;
	d["hdr10plus"] = video.hdr10plus;
	// Stereo and spherical metadata of the container ("" when it says nothing: guess from the name, as before).
	d["stereo_mode"] = video.stereo_mode;
	d["stereo_inverted"] = video.stereo_inverted;
	d["stereo_baseline_mm"] = video.stereo_baseline_mm;
	d["stereo_fov"] = video.stereo_fov;
	d["projection"] = video.projection;
	d["projection_fov"] = Vector2(video.projection_fov_h, video.projection_fov_v);
	d["projection_yaw_pitch_roll"] = Vector3(video.projection_yaw, video.projection_pitch, video.projection_roll);
	d["projection_bounds"] = Vector4(video.projection_bounds[0], video.projection_bounds[1], video.projection_bounds[2], video.projection_bounds[3]);
	// Matroska also keeps its StereoMode as a stream tag ("left_right", "top_bottom", "block_lr", ...).
	const AVDictionaryEntry *stereo_tag = format && video.stream_index >= 0 ? av_dict_get(format->streams[video.stream_index]->metadata, "stereo_mode", nullptr, 0) : nullptr;
	d["stereo_mode_tag"] = stereo_tag ? String::utf8(stereo_tag->value) : String();
	d["is_h264"] = video.codec == VIDEO_CODEC_H264;
	d["is_hevc"] = video.codec == VIDEO_CODEC_HEVC;
	d["is_vp9"] = video.codec == VIDEO_CODEC_VP9;
	d["is_av1"] = video.codec == VIDEO_CODEC_AV1;
	// What the Steam Frame decodes in hardware to a format we can import (spike 01): H.264 and HEVC, 8-bit, 4:2:0.
	const String hw_reason = get_hw_unsupported_reason(video, hw_10bit_available.is_set());
	d["hw_decodable"] = hw_reason.is_empty();
	d["hw_unsupported_reason"] = hw_reason;
	// HWVideoTexture.open_source() decodes the rest in software up to about 1080p (its software_max_pixels can change that).
	const String sw_reason = hw_reason.is_empty() ? String() : SoftwareVideoDecoder::get_unsupported_reason(video, SoftwareVideoDecoder::DEFAULT_MAX_PIXELS);
	d["software_decodable"] = !hw_reason.is_empty() && sw_reason.is_empty();
	d["playable"] = hw_reason.is_empty() || sw_reason.is_empty();
	d["unplayable_reason"] = hw_reason.is_empty() || sw_reason.is_empty() ? String() : hw_reason + " " + sw_reason;
	d["annexb_filter"] = bsf != nullptr;
	if (video.sps.valid) {
		d["coded_width"] = video.sps.width;
		d["coded_height"] = video.sps.height;
		d["display_width"] = video.sps.display_width;
		d["display_height"] = video.sps.display_height;
		d["chroma_format"] = video.sps.chroma_format;
	}
	return d;
}

Array MediaSource::get_audio_tracks() const {
	Array tracks;
	for (uint32_t i = 0; i < audio_tracks.size(); i++) {
		const AudioTrack &track = audio_tracks[i];
		Dictionary d;
		d["index"] = i;
		d["stream_index"] = track.stream_index;
		d["codec"] = track.codec;
		d["channels"] = track.channels;
		d["channel_layout"] = track.channel_layout;
		d["sample_rate"] = track.sample_rate;
		d["bit_rate"] = track.bit_rate;
		d["language"] = track.language;
		d["language_code"] = normalize_language(track.language);
		d["title"] = track.title;
		d["default"] = track.is_default;
		d["commentary"] = track.commentary;
		d["visual_impaired"] = track.visual_impaired;
		tracks.push_back(d);
	}
	return tracks;
}

Array MediaSource::get_subtitle_tracks() const {
	Array tracks;
	for (uint32_t i = 0; i < subtitle_tracks.size(); i++) {
		tracks.push_back(_subtitle_track_to_dictionary(subtitle_tracks[i], (int)i, false));
	}
	MutexLock lock(mutex);
	for (uint32_t i = 0; i < external_subtitles.size(); i++) {
		tracks.push_back(_subtitle_track_to_dictionary(external_subtitles[i].track, (int)(subtitle_tracks.size() + i), true));
	}
	return tracks;
}

Dictionary MediaSource::get_info() const {
	Dictionary d;
	d["path"] = path;
	d["format"] = format_name;
	d["duration"] = duration;
	d["start_time"] = start_time;
	d["bit_rate"] = bit_rate;
	d["video"] = get_video_info();
	d["audio_tracks"] = get_audio_tracks();
	d["subtitle_tracks"] = get_subtitle_tracks();
	d["chapters"] = get_chapters();
	d["tags"] = get_tags();
	d["live"] = is_live();
	d["seekable"] = is_seekable();
	d["network_description"] = net_stream ? net_stream->get_description() : String();
	d["attached_pictures"] = (int)pictures.size();
	d["audio_track"] = get_audio_track();
	d["subtitle_track"] = get_subtitle_track();
	d["output_rate"] = output_rate;
	return d;
}

Error MediaSource::select_audio_track(int p_track) {
	ERR_FAIL_COND_V_MSG(!is_open(), ERR_UNCONFIGURED, "MediaSource: not open.");
	ERR_FAIL_COND_V_MSG(p_track < -1 || p_track >= (int)audio_tracks.size(), ERR_INVALID_PARAMETER, vformat("MediaSource: no audio track %d (%d tracks).", p_track, audio_tracks.size()));
	const double position = get_position();
	{
		MutexLock lock(mutex);
		if (audio_track == p_track) {
			return OK;
		}
		audio_track = p_track;
	}
	// The demux thread is ahead of the playback position by the queues: read again from here with the new track.
	seek(position);
	return OK;
}

int MediaSource::get_audio_track() const {
	MutexLock lock(mutex);
	return audio_track;
}

Error MediaSource::select_subtitle_track(int p_track) {
	ERR_FAIL_COND_V_MSG(!is_open(), ERR_UNCONFIGURED, "MediaSource: not open.");
	MutexLock lock(mutex);
	const int count = (int)(subtitle_tracks.size() + external_subtitles.size());
	ERR_FAIL_COND_V_MSG(p_track < -1 || p_track >= count, ERR_INVALID_PARAMETER, vformat("MediaSource: no subtitle track %d (%d tracks).", p_track, count));
	if (subtitle_track != p_track) {
		subtitle_track = p_track;
		subtitle_queue.clear();
		const int external = p_track - (int)subtitle_tracks.size();
		subtitle_header = external >= 0 ? external_subtitles[external].header : String();
		external_cursor = 0;
		external_serial = serial;
		condition.notify_one();
	}
	return OK;
}

int MediaSource::get_subtitle_track() const {
	MutexLock lock(mutex);
	return subtitle_track;
}

Error MediaSource::select_secondary_subtitle_track(int p_track) {
	ERR_FAIL_COND_V_MSG(!is_open(), ERR_UNCONFIGURED, "MediaSource: not open.");
	ERR_FAIL_COND_V_MSG(p_track < -1 || p_track >= (int)subtitle_tracks.size(), ERR_INVALID_PARAMETER,
			vformat("MediaSource: the second subtitle track must be one of the file's own (0 to %d), not %d.", (int)subtitle_tracks.size() - 1, p_track));
	ERR_FAIL_COND_V_MSG(p_track >= 0 && !subtitle_tracks[p_track].text, ERR_INVALID_PARAMETER, vformat("MediaSource: subtitle track %d is pictures; the second track must be text.", p_track));
	MutexLock lock(mutex);
	if (secondary_subtitle_track != p_track) {
		secondary_subtitle_track = p_track;
		secondary_subtitle_header = String();
		// Like the first track: events read before now aren't delivered (no seek).
		condition.notify_one();
	}
	return OK;
}

int MediaSource::get_secondary_subtitle_track() const {
	MutexLock lock(mutex);
	return secondary_subtitle_track;
}

Dictionary MediaSource::read_subtitle_event() {
	Dictionary d;
	SubtitleEvent event;
	{
		MutexLock lock(mutex);
		const int external = subtitle_track - (int)subtitle_tracks.size();
		if (subtitle_track >= 0 && external >= 0 && !subtitle_queue.is_empty()) {
			// The file's second track (dual subtitles) alongside a subtitle file.
			event = subtitle_queue[0];
			subtitle_queue.remove_at(0);
		} else if (subtitle_track >= 0 && external >= 0) {
			// A subtitle file: everything at once; after a seek, again from the events still showing at the target.
			const LocalVector<SubtitleEvent> &events = external_subtitles[external].events;
			if (external_serial != serial) {
				external_serial = serial;
				external_cursor = 0;
				// Events without an end show until the next one: count them as 10 s long here.
				while (external_cursor < events.size() && (events[external_cursor].end >= 0.0 ? events[external_cursor].end : events[external_cursor].start + 10.0) < seek_target) {
					external_cursor++;
				}
			}
			if (external_cursor >= events.size()) {
				return d;
			}
			event = events[external_cursor++];
			event.serial = serial;
			event.track = subtitle_track;
		} else {
			if (subtitle_queue.is_empty()) {
				return d;
			}
			event = subtitle_queue[0];
			subtitle_queue.remove_at(0);
		}
	}
	d["start"] = event.start;
	d["end"] = event.end;
	d["text"] = event.text;
	d["ass"] = event.ass;
	d["bitmap"] = event.bitmap;
	if (event.bitmap) {
		if (!event.data.is_empty()) {
			d["data"] = event.data;
		} else {
			Array images;
			bool forced = false;
			for (const SubtitleBitmap &bitmap : event.bitmaps) {
				Dictionary image;
				image["rect"] = bitmap.rect;
				image["image"] = bitmap.image;
				image["forced"] = bitmap.forced;
				images.push_back(image);
				forced = forced || bitmap.forced;
			}
			d["images"] = images;
			d["forced"] = forced;
			d["canvas_size"] = event.canvas.width > 0 && event.canvas.height > 0 ? Vector2i(event.canvas) : Vector2i(video.width, video.height);
		}
	} else if (!event.ass.is_empty()) {
		String header;
		bool second = false;
		{
			MutexLock lock(mutex);
			second = event.track >= 0 && event.track == secondary_subtitle_track && event.track != subtitle_track;
			header = second ? secondary_subtitle_header : subtitle_header;
		}
		String &source = second ? styled_header2_source : styled_header_source;
		SubtitleStyle::Header &parsed = second ? styled_header2 : styled_header;
		if (header != source) {
			source = header;
			parsed = SubtitleStyle::parse_header(header);
		}
		// One event may hold several lines (several rects): each is styled on its own.
		Array styled;
		const Vector<String> lines = event.ass.split("\n");
		for (const String &line : lines) {
			styled.push_back(SubtitleStyle::parse_event(line, parsed));
		}
		d["styled"] = styled;
	}
	d["serial"] = event.serial;
	d["track"] = event.track;
	return d;
}

String MediaSource::get_subtitle_header() const {
	MutexLock lock(mutex);
	return subtitle_header;
}

bool MediaSource::is_live() const {
	return net_stream != nullptr && net_stream->is_live();
}

bool MediaSource::is_seekable() const {
	return is_open() && !is_live() && !(net_stream != nullptr && !net_stream->is_seekable());
}

int MediaSource::seek(double p_seconds) {
	ERR_FAIL_COND_V_MSG(!is_open(), -1, "MediaSource: not open.");
	if (!is_seekable()) {
		WARN_PRINT_ONCE("MediaSource: a live stream (or a server without Range support) can't seek.");
		MutexLock lock(mutex);
		return (int)serial;
	}
	double target = MAX(p_seconds, 0.0);
	if (duration > 0.0) {
		target = MIN(target, duration);
	}
	MutexLock lock(mutex);
	serial++;
	seek_pending = true;
	seek_target = target;
	demux_eof = false;
	seek_count++;
	if (state == STATE_ENDED) {
		state = STATE_READING;
	}
	_flush_video_queue_locked();
	subtitle_queue.clear();
	last_video_pts = -1.0;
	{
		MutexLock audio_lock(audio_mutex);
		ring_read = ring_write = 0;
		marker_count = 0;
		audio_position = target;
		audio_serial = serial;
		audio_eof = audio_track < 0 || trick_speed > 0.0; // Trick play has no audio.
		audio_in_underrun = false;
		audio_mixed_since_seek = false;
		live_playing = false;
	}
	condition.notify_one();
	_wake_consumer_locked();
	return (int)serial;
}

int MediaSource::trick_play(double p_speed, double p_from) {
	ERR_FAIL_COND_V_MSG(!is_open(), -1, "MediaSource: not open.");
	const double speed = p_speed >= TRICK_SPEED_MIN ? MIN(p_speed, TRICK_SPEED_MAX) : 0.0;
	if (speed > 0.0 && video.stream_index < 0) {
		_set_error("Trick play needs a video stream.");
		return (int)get_serial();
	}
	{
		MutexLock lock(mutex);
		trick_speed = speed; // The demux thread takes it with the seek.
	}
	return seek(p_from);
}

double MediaSource::get_trick_play_speed() const {
	MutexLock lock(mutex);
	return trick_speed;
}

uint32_t MediaSource::get_serial() const {
	MutexLock lock(mutex);
	return serial;
}

double MediaSource::get_seek_target() const {
	MutexLock lock(mutex);
	return seek_target;
}

double MediaSource::get_position() const {
	if (get_audio_track() >= 0) {
		return get_audio_position();
	}
	MutexLock lock(mutex);
	return last_video_pts >= 0.0 ? last_video_pts : seek_target;
}

bool MediaSource::is_eof() const {
	MutexLock lock(mutex);
	return demux_eof;
}

MediaSource::State MediaSource::get_state() const {
	MutexLock lock(mutex);
	return state;
}

String MediaSource::get_last_error() const {
	MutexLock lock(mutex);
	return last_error;
}

Dictionary MediaSource::get_stats() const {
	static const char *state_names[] = { "closed", "reading", "ended", "error" };
	Dictionary d;
	// The video bit rate at the playback position (the last 3 known whole seconds); left out while unknown.
	const PackedFloat32Array recent = get_video_bitrate_history(4, get_position());
	double kbps_sum = 0.0;
	int known = 0;
	for (int i = recent.size() - 1; i >= 0 && known < 3; i--) {
		if (recent[i] >= 0.0f) {
			kbps_sum += recent[i];
			known++;
		}
	}
	if (known > 0) {
		d["bitrate_bps"] = (int64_t)(kbps_sum / known * 1000.0);
	}
	MutexLock lock(mutex);
	d["state"] = (int)state;
	d["state_name"] = state_names[state];
	d["serial"] = serial;
	d["eof"] = demux_eof;
	d["packets_read"] = stats.packets_read;
	d["bytes_read"] = stats.bytes_read;
	d["video_packets"] = stats.video_packets;
	d["video_bytes"] = stats.video_bytes;
	d["video_skipped"] = stats.video_skipped;
	d["video_parameter_sets_added"] = stats.video_parameter_sets_added;
	d["video_queue"] = video_count;
	d["video_queue_bytes"] = video_queued_bytes;
	d["audio_packets"] = stats.audio_packets;
	d["audio_frames_out"] = stats.audio_frames_out;
	d["audio_decode_errors"] = stats.audio_decode_errors;
	d["audio_dropped_frames"] = stats.audio_dropped_frames;
	d["audio_silence_frames"] = stats.audio_silence_frames;
	d["read_errors"] = stats.read_errors;
	d["seek_retries"] = stats.seek_retries;
	d["trick_steps"] = stats.trick_steps;
	d["trick_play_speed"] = trick_speed;
	d["external_clock"] = external_seconds >= 0.0;
	d["external_clock_error_ms"] = external_error * 1000.0; // Audio position minus the external clock, at the last read.
	d["external_clock_ratio"] = external_ratio;
	d["subtitle_events"] = stats.subtitle_events;
	d["subtitle_decode_errors"] = stats.subtitle_decode_errors;
	d["subtitle_queue"] = subtitle_queue.size();
	d["thread_cpu_seconds"] = stats.thread_cpu_seconds;
	d["seeks"] = seek_count;
	d["last_error"] = last_error;
	d["network"] = smb_file != nullptr || net_stream != nullptr;
	d["live"] = net_stream != nullptr && net_stream->is_live();
	if (net_stream) {
		const NetStream::Stats n = net_stream->get_stats();
		d["net_buffer_bytes"] = n.buffer_size;
		d["net_buffered_bytes"] = n.buffered;
		d["net_bytes_read"] = n.bytes_read;
		d["net_requests"] = n.requests;
		d["net_resets"] = n.resets;
		d["net_waits"] = n.waits;
		d["net_wait_seconds"] = n.wait_seconds;
		d["net_reconnects"] = n.reconnects;
		d["net_errors"] = n.errors;
		d["net_packets"] = n.packets;
		d["net_overruns"] = n.overruns;
		d["net_skips"] = n.skips;
		d["net_wakeups"] = n.wakeups;
		d["net_average_mbps"] = n.open_seconds > 0.0 ? (double)n.bytes_read * 8.0 / n.open_seconds / 1e6 : 0.0;
		d["net_thread_cpu_seconds"] = n.thread_cpu_seconds;
	}
	if (smb_file) {
		const SmbFile::Stats n = smb_file->get_stats();
		d["net_buffer_bytes"] = n.buffer_size;
		d["net_buffered_bytes"] = n.buffered;
		d["net_bytes_read"] = n.bytes_read;
		d["net_requests"] = n.requests;
		d["net_resets"] = n.resets;
		d["net_waits"] = n.waits;
		d["net_seek_waits"] = n.seek_waits;
		d["net_stalls"] = n.stalls;
		d["net_stall_seconds"] = n.stall_seconds;
		d["net_wait_seconds"] = n.wait_seconds;
		d["net_refills"] = n.refills;
		d["net_refill_seconds"] = n.refill_seconds;
		d["net_refill_mbps"] = n.refill_seconds > 0.0 ? (double)n.refill_bytes * 8.0 / n.refill_seconds / 1e6 : 0.0;
		d["net_reconnects"] = n.reconnects;
		d["net_errors"] = n.errors;
		d["net_read_mbps"] = n.busy_seconds > 0.0 ? (double)n.bytes_read * 8.0 / n.busy_seconds / 1e6 : 0.0;
		d["net_average_mbps"] = n.open_seconds > 0.0 ? (double)n.bytes_read * 8.0 / n.open_seconds / 1e6 : 0.0;
		d["net_busy_seconds"] = n.busy_seconds;
		d["net_thread_cpu_seconds"] = n.thread_cpu_seconds;
	}
	{
		MutexLock audio_lock(audio_mutex);
		d["audio_buffered"] = output_rate > 0 ? (double)(ring_write - ring_read) / output_rate : 0.0;
		d["audio_position"] = audio_position;
		d["audio_underruns"] = audio_underruns;
		d["audio_seek_waits"] = audio_seek_waits;
		d["audio_frames_mixed"] = audio_frames_mixed;
		d["audio_finished"] = audio_eof && ring_write == ring_read;
	}
	// mutex is still held (since the counters above).
	d["night_mode"] = night_mode;
	d["night_mode_decoder_drc"] = stats.decoder_drc;
	d["dialogue_boost_applies"] = stats.dialogue_boost_applies;
	d["audio_gain_db"] = stats.audio_gain_db;
	d["audio_limiter_db"] = stats.audio_limiter_db;
	d["audio_typical_level_db"] = stats.audio_typical_level_db;
	return d;
}

Ref<AudioStreamMedia> MediaSource::get_audio_stream() {
	Ref<AudioStreamMedia> stream;
	stream.instantiate();
	stream->set_media_source(this);
	return stream;
}

Dictionary MediaSource::read_video_packet() {
	Dictionary d;
	VideoPacket packet;
	if (!pop_video_packet(packet)) {
		return d;
	}
	PackedByteArray data;
	data.resize(packet.size);
	memcpy(data.ptrw(), packet.data, packet.size);
	d["data"] = data;
	d["pts"] = packet.pts;
	d["dts"] = packet.dts;
	d["keyframe"] = packet.keyframe;
	d["serial"] = packet.serial;
	d["sps_width"] = packet.sps_width;
	d["sps_height"] = packet.sps_height;
	d["nal_type"] = packet.nal_type;
	static const char *h264_names[] = { "", "i", "", "", "", "idr" };
	String random_access;
	if (packet.nal_type >= 0 && video.codec == VIDEO_CODEC_H264 && packet.nal_type <= 5) {
		random_access = h264_names[packet.nal_type];
	} else if (packet.nal_type >= 0 && video.codec == VIDEO_CODEC_HEVC) {
		random_access = packet.nal_type == 19 || packet.nal_type == 20 ? "idr" : packet.nal_type == 21 ? "cra" : (packet.nal_type >= 16 && packet.nal_type <= 18) ? "bla" : "";
	}
	d["random_access"] = random_access;
	release_video_packet(packet);
	return d;
}

bool MediaSource::pop_video_packet(VideoPacket &r_packet) {
	MutexLock lock(mutex);
	if (video_count == 0) {
		return false;
	}
	VideoPacket &entry = video_queue[video_head];
	r_packet = entry;
	entry = VideoPacket();
	video_head = (video_head + 1) % VIDEO_QUEUE_MAX;
	video_count--;
	video_queued_bytes -= r_packet.size;
	last_video_pts = r_packet.pts;
	if (demux_idle.is_set() && video_count < video_queue_target * 3 / 4) {
		condition.notify_one();
	}
	return true;
}

void MediaSource::release_video_packet(VideoPacket &r_packet) {
	if (r_packet.packet) {
		_packet_put(r_packet.packet);
	}
	r_packet = VideoPacket();
}

void MediaSource::set_video_wake_fd(int p_fd) {
	MutexLock lock(mutex);
	wake_fd = p_fd;
}

bool MediaSource::is_video_input_done(uint32_t p_serial) const {
	MutexLock lock(mutex);
	if (state == STATE_ERROR || state == STATE_CLOSED) {
		return video_count == 0;
	}
	return demux_eof && video_count == 0 && !seek_pending && p_serial == serial;
}

#define SETTING_GUARD() ERR_FAIL_COND_MSG(is_open(), "MediaSource: change the settings before open().")

void MediaSource::set_video_queue_seconds(double p_seconds) {
	SETTING_GUARD();
	video_queue_seconds = CLAMP(p_seconds, 0.1, 10.0);
}

void MediaSource::set_audio_queue_seconds(double p_seconds) {
	SETTING_GUARD();
	audio_queue_seconds = CLAMP(p_seconds, 0.1, AUDIO_RING_SECONDS - 1.0);
}

void MediaSource::set_discrete_channels(bool p_enabled) {
	SETTING_GUARD();
	discrete_channels = p_enabled;
}

Ref<AudioStreamMedia> MediaSource::get_audio_channel_stream(int p_channel) {
	Ref<AudioStreamMedia> stream;
	ERR_FAIL_INDEX_V_MSG(p_channel, DISCRETE_CHANNELS, stream, "MediaSource: channels are 0-7 (FL FR FC LFE BL BR SL SR).");
	if (!discrete_channels) {
		WARN_PRINT("MediaSource: get_audio_channel_stream() without discrete_channels: the stream is silent.");
	}
	stream.instantiate();
	stream->set_media_source(this);
	stream->set_channel(p_channel);
	return stream;
}

Array MediaSource::get_audio_channels() const {
	static const char *names[DISCRETE_CHANNELS] = { "FL", "FR", "FC", "LFE", "BL", "BR", "SL", "SR" };
	static const char *labels[DISCRETE_CHANNELS] = { "front left", "front right", "front centre", "low frequency", "back left", "back right", "side left", "side right" };
	uint32_t mask;
	{
		MutexLock lock(mutex);
		mask = stats.audio_channel_mask;
	}
	Array channels;
	for (int i = 0; i < DISCRETE_CHANNELS; i++) {
		Dictionary d;
		d["index"] = i;
		d["name"] = names[i];
		d["label"] = labels[i];
		d["present"] = (mask & (1u << i)) != 0;
		channels.push_back(d);
	}
	return channels;
}

void MediaSource::set_night_mode(bool p_enabled) {
	MutexLock lock(mutex);
	night_mode = p_enabled;
	audio_dsp_serial++;
	condition.notify_one();
}

bool MediaSource::is_night_mode() const {
	MutexLock lock(mutex);
	return night_mode;
}

void MediaSource::set_night_mode_strength(double p_strength) {
	MutexLock lock(mutex);
	night_mode_strength = CLAMP(p_strength, 0.0, 1.0);
	audio_dsp_serial++;
	condition.notify_one();
}

double MediaSource::get_night_mode_strength() const {
	MutexLock lock(mutex);
	return night_mode_strength;
}

void MediaSource::set_dialogue_boost(double p_db) {
	MutexLock lock(mutex);
	dialogue_boost = CLAMP(p_db, 0.0, 12.0);
	audio_dsp_serial++;
	condition.notify_one();
}

double MediaSource::get_dialogue_boost() const {
	MutexLock lock(mutex);
	return dialogue_boost;
}

void MediaSource::set_downmix_normalize(bool p_normalize) {
	SETTING_GUARD();
	downmix_normalize = p_normalize;
}

void MediaSource::set_smb_credentials(const String &p_host, const String &p_user, const String &p_password, const String &p_domain) {
	SmbCredentials::set(p_host, p_user, p_password, p_domain);
}

void MediaSource::set_network_buffer_size(int64_t p_bytes) {
	network_buffer_size = CLAMP(p_bytes, (int64_t)4 << 20, (int64_t)256 << 20); // Applies at the next open().
}

void MediaSource::set_network_requests(int p_requests) {
	network_requests = CLAMP(p_requests, 1, 16); // Applies at the next open().
}

void MediaSource::set_audio_decoder_options(const Dictionary &p_options) {
	SETTING_GUARD();
	audio_decoder_options = p_options.duplicate();
}

MediaSource::~MediaSource() {
	_close_internal();
	if (tempo) {
		memdelete(tempo);
		tempo = nullptr;
	}
	for (AVPacket *packet : packet_pool) {
		av_packet_free(&packet);
	}
	packet_pool.clear();
}

void MediaSource::_bind_methods() {
	ClassDB::bind_method(D_METHOD("open", "path"), &MediaSource::open);
	ClassDB::bind_method(D_METHOD("close"), &MediaSource::close);
	ClassDB::bind_method(D_METHOD("is_open"), &MediaSource::is_open);
	ClassDB::bind_method(D_METHOD("get_path"), &MediaSource::get_path);
	ClassDB::bind_method(D_METHOD("get_info"), &MediaSource::get_info);
	ClassDB::bind_method(D_METHOD("get_video_info"), &MediaSource::get_video_info);
	ClassDB::bind_method(D_METHOD("get_audio_tracks"), &MediaSource::get_audio_tracks);
	ClassDB::bind_method(D_METHOD("get_subtitle_tracks"), &MediaSource::get_subtitle_tracks);
	ClassDB::bind_method(D_METHOD("select_audio_track", "track"), &MediaSource::select_audio_track);
	ClassDB::bind_method(D_METHOD("get_audio_track"), &MediaSource::get_audio_track);
	ClassDB::bind_method(D_METHOD("select_subtitle_track", "track"), &MediaSource::select_subtitle_track);
	ClassDB::bind_method(D_METHOD("get_subtitle_track"), &MediaSource::get_subtitle_track);
	ClassDB::bind_method(D_METHOD("read_subtitle_event"), &MediaSource::read_subtitle_event);
	ClassDB::bind_method(D_METHOD("get_subtitle_header"), &MediaSource::get_subtitle_header);
	ClassDB::bind_method(D_METHOD("seek", "seconds"), &MediaSource::seek);
	ClassDB::bind_method(D_METHOD("trick_play", "speed", "from"), &MediaSource::trick_play);
	ClassDB::bind_method(D_METHOD("get_trick_play_speed"), &MediaSource::get_trick_play_speed);
	ClassDB::bind_method(D_METHOD("get_serial"), &MediaSource::get_serial);
	ClassDB::bind_method(D_METHOD("get_seek_target"), &MediaSource::get_seek_target);
	ClassDB::bind_method(D_METHOD("get_duration"), &MediaSource::get_duration);
	ClassDB::bind_method(D_METHOD("get_start_time"), &MediaSource::get_start_time);
	ClassDB::bind_method(D_METHOD("get_position"), &MediaSource::get_position);
	ClassDB::bind_method(D_METHOD("get_audio_position"), &MediaSource::get_audio_position);
	ClassDB::bind_method(D_METHOD("is_eof"), &MediaSource::is_eof);
	ClassDB::bind_method(D_METHOD("get_state"), &MediaSource::get_state);
	ClassDB::bind_method(D_METHOD("get_last_error"), &MediaSource::get_last_error);
	ClassDB::bind_method(D_METHOD("get_stats"), &MediaSource::get_stats);
	ClassDB::bind_method(D_METHOD("get_audio_stream"), &MediaSource::get_audio_stream);
	ClassDB::bind_method(D_METHOD("get_output_rate"), &MediaSource::get_output_rate);
	ClassDB::bind_method(D_METHOD("read_video_packet"), &MediaSource::read_video_packet);
	ClassDB::bind_method(D_METHOD("get_chapters"), &MediaSource::get_chapters);
	ClassDB::bind_method(D_METHOD("get_tags"), &MediaSource::get_tags);
	ClassDB::bind_method(D_METHOD("get_attached_pictures"), &MediaSource::get_attached_pictures);
	ClassDB::bind_method(D_METHOD("get_album_art"), &MediaSource::get_album_art);
	ClassDB::bind_static_method("MediaSource", D_METHOD("parse_ass_event", "line", "header"), &MediaSource::parse_ass_event);
	ClassDB::bind_static_method("MediaSource", D_METHOD("get_tone_mapping", "video_info", "target_nits"), &MediaSource::get_tone_mapping, DEFVAL(203.0));
	ClassDB::bind_static_method("MediaSource", D_METHOD("tone_map_color", "rgb", "tone_mapping"), &MediaSource::tone_map_color);
	ClassDB::bind_method(D_METHOD("set_playback_speed", "speed"), &MediaSource::set_playback_speed);
	ClassDB::bind_method(D_METHOD("get_playback_speed"), &MediaSource::get_playback_speed);
	ClassDB::bind_method(D_METHOD("get_video_bitrate_history", "seconds", "at"), &MediaSource::get_video_bitrate_history);
	ClassDB::bind_method(D_METHOD("get_bit_rate"), &MediaSource::get_bit_rate);
	ClassDB::bind_method(D_METHOD("get_chapter_at", "seconds"), &MediaSource::get_chapter_at);
	ClassDB::bind_method(D_METHOD("add_external_subtitle", "path", "language", "title"), &MediaSource::add_external_subtitle, DEFVAL(String()), DEFVAL(String()));
	ClassDB::bind_method(D_METHOD("get_external_subtitle_count"), &MediaSource::get_external_subtitle_count);
	ClassDB::bind_method(D_METHOD("get_subtitle_lines", "track"), &MediaSource::get_subtitle_lines);
	ClassDB::bind_method(D_METHOD("read_audio_frames", "count"), &MediaSource::read_audio_frames);
	ClassDB::bind_method(D_METHOD("set_external_clock", "seconds", "ticks_usec"), &MediaSource::set_external_clock);
	ClassDB::bind_method(D_METHOD("select_secondary_subtitle_track", "track"), &MediaSource::select_secondary_subtitle_track);
	ClassDB::bind_method(D_METHOD("get_secondary_subtitle_track"), &MediaSource::get_secondary_subtitle_track);
	ClassDB::bind_method(D_METHOD("set_subtitle_read_ahead", "seconds"), &MediaSource::set_subtitle_read_ahead);
	ClassDB::bind_method(D_METHOD("get_subtitle_read_ahead"), &MediaSource::get_subtitle_read_ahead);
	ClassDB::bind_static_method("MediaSource", D_METHOD("normalize_language", "code"), &MediaSource::normalize_language);
	ClassDB::bind_static_method("MediaSource", D_METHOD("language_name", "code"), &MediaSource::language_name);
	ClassDB::bind_method(D_METHOD("pick_audio_track", "languages"), &MediaSource::pick_audio_track);
	ClassDB::bind_method(D_METHOD("pick_subtitle_track", "languages", "forced_only"), &MediaSource::pick_subtitle_track, DEFVAL(false));

	ClassDB::bind_method(D_METHOD("set_video_queue_seconds", "seconds"), &MediaSource::set_video_queue_seconds);
	ClassDB::bind_method(D_METHOD("get_video_queue_seconds"), &MediaSource::get_video_queue_seconds);
	ClassDB::bind_method(D_METHOD("set_audio_queue_seconds", "seconds"), &MediaSource::set_audio_queue_seconds);
	ClassDB::bind_method(D_METHOD("get_audio_queue_seconds"), &MediaSource::get_audio_queue_seconds);
	ClassDB::bind_method(D_METHOD("set_network_tls_verify", "verify"), &MediaSource::set_network_tls_verify);
	ClassDB::bind_method(D_METHOD("is_network_tls_verify"), &MediaSource::is_network_tls_verify);
	ClassDB::bind_method(D_METHOD("set_live_latency", "seconds"), &MediaSource::set_live_latency);
	ClassDB::bind_method(D_METHOD("get_live_latency"), &MediaSource::get_live_latency);
	ClassDB::bind_method(D_METHOD("is_live"), &MediaSource::is_live);
	ClassDB::bind_method(D_METHOD("is_seekable"), &MediaSource::is_seekable);
	ClassDB::bind_method(D_METHOD("set_discrete_channels", "enabled"), &MediaSource::set_discrete_channels);
	ClassDB::bind_method(D_METHOD("is_discrete_channels"), &MediaSource::is_discrete_channels);
	ClassDB::bind_method(D_METHOD("get_audio_channel_stream", "channel"), &MediaSource::get_audio_channel_stream);
	ClassDB::bind_method(D_METHOD("get_audio_channels"), &MediaSource::get_audio_channels);
	ClassDB::bind_method(D_METHOD("set_night_mode", "enabled"), &MediaSource::set_night_mode);
	ClassDB::bind_method(D_METHOD("is_night_mode"), &MediaSource::is_night_mode);
	ClassDB::bind_method(D_METHOD("set_night_mode_strength", "strength"), &MediaSource::set_night_mode_strength);
	ClassDB::bind_method(D_METHOD("get_night_mode_strength"), &MediaSource::get_night_mode_strength);
	ClassDB::bind_method(D_METHOD("set_dialogue_boost", "db"), &MediaSource::set_dialogue_boost);
	ClassDB::bind_method(D_METHOD("get_dialogue_boost"), &MediaSource::get_dialogue_boost);
	ClassDB::bind_method(D_METHOD("set_letterbox_detection", "enabled"), &MediaSource::set_letterbox_detection);
	ClassDB::bind_method(D_METHOD("is_letterbox_detection"), &MediaSource::is_letterbox_detection);
	ClassDB::bind_method(D_METHOD("get_letterbox"), &MediaSource::get_letterbox);
	ClassDB::bind_method(D_METHOD("set_downmix_normalize", "normalize"), &MediaSource::set_downmix_normalize);
	ClassDB::bind_method(D_METHOD("is_downmix_normalize"), &MediaSource::is_downmix_normalize);
	ClassDB::bind_static_method("MediaSource", D_METHOD("set_smb_credentials", "host", "user", "password", "domain"), &MediaSource::set_smb_credentials, DEFVAL(String()));
	ClassDB::bind_method(D_METHOD("set_network_buffer_size", "bytes"), &MediaSource::set_network_buffer_size);
	ClassDB::bind_method(D_METHOD("get_network_buffer_size"), &MediaSource::get_network_buffer_size);
	ClassDB::bind_method(D_METHOD("set_network_requests", "requests"), &MediaSource::set_network_requests);
	ClassDB::bind_method(D_METHOD("get_network_requests"), &MediaSource::get_network_requests);
	ClassDB::bind_method(D_METHOD("set_audio_decoder_options", "options"), &MediaSource::set_audio_decoder_options);
	ClassDB::bind_method(D_METHOD("get_audio_decoder_options"), &MediaSource::get_audio_decoder_options);

	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "video_queue_seconds"), "set_video_queue_seconds", "get_video_queue_seconds");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "audio_queue_seconds"), "set_audio_queue_seconds", "get_audio_queue_seconds");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "downmix_normalize"), "set_downmix_normalize", "is_downmix_normalize");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "letterbox_detection"), "set_letterbox_detection", "is_letterbox_detection");
	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "audio_decoder_options"), "set_audio_decoder_options", "get_audio_decoder_options");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "playback_speed", PROPERTY_HINT_RANGE, "0.5,2,0.05"), "set_playback_speed", "get_playback_speed");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "discrete_channels"), "set_discrete_channels", "is_discrete_channels");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "network_tls_verify"), "set_network_tls_verify", "is_network_tls_verify");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "live_latency", PROPERTY_HINT_RANGE, "0.1,2.5,0.1,suffix:s"), "set_live_latency", "get_live_latency");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "night_mode"), "set_night_mode", "is_night_mode");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "night_mode_strength", PROPERTY_HINT_RANGE, "0,1,0.05"), "set_night_mode_strength", "get_night_mode_strength");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "dialogue_boost", PROPERTY_HINT_RANGE, "0,12,0.5,suffix:dB"), "set_dialogue_boost", "get_dialogue_boost");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "subtitle_read_ahead", PROPERTY_HINT_RANGE, "0,30,0.1,suffix:s"), "set_subtitle_read_ahead", "get_subtitle_read_ahead");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "network_buffer_size", PROPERTY_HINT_RANGE, "4194304,268435456,1048576,suffix:B"), "set_network_buffer_size", "get_network_buffer_size");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "network_requests", PROPERTY_HINT_RANGE, "1,16,1"), "set_network_requests", "get_network_requests");

	BIND_ENUM_CONSTANT(STATE_CLOSED);
	BIND_ENUM_CONSTANT(STATE_READING);
	BIND_ENUM_CONSTANT(STATE_ENDED);
	BIND_ENUM_CONSTANT(STATE_ERROR);
}
