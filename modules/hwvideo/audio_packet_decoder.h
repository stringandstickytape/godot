/**************************************************************************/
/*  audio_packet_decoder.h                                                */
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

#include "core/object/ref_counted.h"
#include "core/variant/variant.h"

struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwrContext;

// Compressed audio frames that arrive one by one (a PC's sound: AAC-LC with its ADTS header) decoded with FFmpeg and
// resampled to stereo float at the output rate (the AudioServer's mix rate by default), for an AudioStreamGenerator the
// app fills at the frames' times. No container and no clock: each decode() returns what that packet gave.
class AudioPacketDecoder : public RefCounted {
	GDCLASS(AudioPacketDecoder, RefCounted);

	AVCodecContext *context = nullptr;
	AVPacket *packet = nullptr;
	AVFrame *frame = nullptr;
	SwrContext *swr = nullptr;
	int swr_format = -1;
	int swr_rate = 0;
	int swr_channels = 0;
	int output_rate = 48000;
	int sample_rate = 0;
	int channels = 0;
	String codec_name;
	String last_error;
	uint64_t packets = 0;
	uint64_t frames_out = 0;
	uint64_t errors = 0;

	bool _configure_swr();
	void _append(PackedVector2Array &r_out);

protected:
	static void _bind_methods();

public:
	// p_codec: FFmpeg's decoder name ("aac"; any audio decoder of the build that needs no extradata). p_output_rate 0: the
	// AudioServer's mix rate.
	Error open(const String &p_codec = "aac", int p_output_rate = 0);
	void close();
	bool is_open() const { return context != nullptr; }
	// One packet: the stereo frames it decoded to at get_output_rate() (none for a damaged packet, which is counted).
	PackedVector2Array decode(const PackedByteArray &p_packet);
	// Drops what the decoder and the resampler hold (after a gap, so old sound doesn't come out first).
	void flush();
	int get_output_rate() const { return output_rate; }
	// Of the stream, once a packet has decoded (0 before).
	int get_sample_rate() const { return sample_rate; }
	int get_channels() const { return channels; }
	String get_last_error() const { return last_error; }
	Dictionary get_stats() const;

	~AudioPacketDecoder();
};
