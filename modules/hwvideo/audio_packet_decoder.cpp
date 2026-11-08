/**************************************************************************/
/*  audio_packet_decoder.cpp                                              */
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

#include "audio_packet_decoder.h"

#include "core/object/class_db.h"
#include "servers/audio/audio_server.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

Error AudioPacketDecoder::open(const String &p_codec, int p_output_rate) {
	close();
	last_error = String();
	const AVCodec *codec = avcodec_find_decoder_by_name(p_codec.utf8().get_data());
	if (codec == nullptr || codec->type != AVMEDIA_TYPE_AUDIO) {
		last_error = vformat("This build has no %s audio decoder.", p_codec);
		return ERR_UNAVAILABLE;
	}
	context = avcodec_alloc_context3(codec);
	packet = av_packet_alloc();
	frame = av_frame_alloc();
	if (context == nullptr || packet == nullptr || frame == nullptr) {
		close();
		last_error = "Out of memory for the audio decoder.";
		return ERR_OUT_OF_MEMORY;
	}
	const int r = avcodec_open2(context, codec, nullptr);
	if (r < 0) {
		close();
		last_error = vformat("Opening the %s decoder failed (%d).", p_codec, r);
		return ERR_CANT_OPEN;
	}
	output_rate = p_output_rate > 0 ? p_output_rate : (AudioServer::get_singleton() ? (int)AudioServer::get_singleton()->get_mix_rate() : 48000);
	codec_name = p_codec;
	sample_rate = channels = 0;
	packets = frames_out = errors = 0;
	return OK;
}

void AudioPacketDecoder::close() {
	if (context != nullptr) {
		avcodec_free_context(&context);
	}
	if (packet != nullptr) {
		av_packet_free(&packet);
	}
	if (frame != nullptr) {
		av_frame_free(&frame);
	}
	if (swr != nullptr) {
		swr_free(&swr);
	}
	swr_format = -1;
	swr_rate = swr_channels = 0;
}

// The resampler for the frame just decoded: its format, rate and channels to stereo float at output_rate.
bool AudioPacketDecoder::_configure_swr() {
	const int in_channels = frame->ch_layout.nb_channels;
	if (swr != nullptr && swr_format == frame->format && swr_rate == frame->sample_rate && swr_channels == in_channels) {
		return true;
	}
	if (swr != nullptr) {
		swr_free(&swr);
	}
	AVChannelLayout in_layout = {};
	if (frame->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC) {
		av_channel_layout_default(&in_layout, in_channels);
	} else {
		av_channel_layout_copy(&in_layout, &frame->ch_layout);
	}
	AVChannelLayout out_layout = AV_CHANNEL_LAYOUT_STEREO;
	int r = swr_alloc_set_opts2(&swr, &out_layout, AV_SAMPLE_FMT_FLT, output_rate, &in_layout, (AVSampleFormat)frame->format, frame->sample_rate, 0, nullptr);
	av_channel_layout_uninit(&in_layout);
	if (r >= 0) {
		r = swr_init(swr);
	}
	if (r < 0) {
		if (swr != nullptr) {
			swr_free(&swr);
		}
		last_error = vformat("Can't resample %d Hz, %d channels to %d Hz stereo.", frame->sample_rate, in_channels, output_rate);
		return false;
	}
	swr_format = frame->format;
	swr_rate = frame->sample_rate;
	swr_channels = in_channels;
	sample_rate = frame->sample_rate;
	channels = in_channels;
	return true;
}

void AudioPacketDecoder::_append(PackedVector2Array &r_out) {
	if (!_configure_swr()) {
		errors++;
		return;
	}
	const int max_out = swr_get_out_samples(swr, frame->nb_samples);
	if (max_out <= 0) {
		return;
	}
	const int64_t at = r_out.size();
	r_out.resize(at + max_out);
	// Vector2 is two floats: interleaved stereo float is exactly its layout.
	uint8_t *out = reinterpret_cast<uint8_t *>(r_out.ptrw() + at);
	const int converted = swr_convert(swr, &out, max_out, (const uint8_t **)frame->extended_data, frame->nb_samples);
	r_out.resize(at + MAX(converted, 0));
	frames_out += MAX(converted, 0);
}

PackedVector2Array AudioPacketDecoder::decode(const PackedByteArray &p_packet) {
	PackedVector2Array out;
	ERR_FAIL_NULL_V_MSG(context, out, "AudioPacketDecoder: not open.");
	if (p_packet.is_empty()) {
		return out;
	}
	packets++;
	av_packet_unref(packet);
	if (av_new_packet(packet, p_packet.size()) < 0) {
		errors++;
		return out;
	}
	memcpy(packet->data, p_packet.ptr(), p_packet.size());
	if (avcodec_send_packet(context, packet) < 0) {
		errors++; // Damaged: the decoder goes on with the next.
		return out;
	}
	while (avcodec_receive_frame(context, frame) == 0) {
		_append(out);
		av_frame_unref(frame);
	}
	return out;
}

void AudioPacketDecoder::flush() {
	if (context != nullptr) {
		avcodec_flush_buffers(context);
	}
	if (swr != nullptr) {
		swr_free(&swr); // Made again for the next frame, without the samples it held back.
		swr_format = -1;
	}
}

Dictionary AudioPacketDecoder::get_stats() const {
	Dictionary d;
	d["codec"] = codec_name;
	d["open"] = is_open();
	d["output_rate"] = output_rate;
	d["sample_rate"] = sample_rate;
	d["channels"] = channels;
	d["packets"] = packets;
	d["frames_out"] = frames_out;
	d["errors"] = errors;
	return d;
}

void AudioPacketDecoder::_bind_methods() {
	ClassDB::bind_method(D_METHOD("open", "codec", "output_rate"), &AudioPacketDecoder::open, DEFVAL("aac"), DEFVAL(0));
	ClassDB::bind_method(D_METHOD("close"), &AudioPacketDecoder::close);
	ClassDB::bind_method(D_METHOD("is_open"), &AudioPacketDecoder::is_open);
	ClassDB::bind_method(D_METHOD("decode", "packet"), &AudioPacketDecoder::decode);
	ClassDB::bind_method(D_METHOD("flush"), &AudioPacketDecoder::flush);
	ClassDB::bind_method(D_METHOD("get_output_rate"), &AudioPacketDecoder::get_output_rate);
	ClassDB::bind_method(D_METHOD("get_sample_rate"), &AudioPacketDecoder::get_sample_rate);
	ClassDB::bind_method(D_METHOD("get_channels"), &AudioPacketDecoder::get_channels);
	ClassDB::bind_method(D_METHOD("get_last_error"), &AudioPacketDecoder::get_last_error);
	ClassDB::bind_method(D_METHOD("get_stats"), &AudioPacketDecoder::get_stats);
}

AudioPacketDecoder::~AudioPacketDecoder() {
	close();
}
