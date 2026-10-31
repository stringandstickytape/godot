/**************************************************************************/
/*  audio_stream_media.cpp                                                */
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

#include "audio_stream_media.h"

#include "core/object/class_db.h"

/* AudioStreamMedia */

void AudioStreamMedia::set_media_source(const Ref<MediaSource> &p_source) {
	source = p_source;
}

Ref<AudioStreamPlayback> AudioStreamMedia::instantiate_playback() {
	Ref<AudioStreamPlaybackMedia> playback;
	ERR_FAIL_COND_V_MSG(source.is_null(), playback, "AudioStreamMedia: no MediaSource.");
	playback.instantiate();
	playback->source = source;
	return playback;
}

String AudioStreamMedia::get_stream_name() const {
	return source.is_valid() ? source->get_path().get_file() : String("AudioStreamMedia");
}

double AudioStreamMedia::get_length() const {
	return source.is_valid() ? source->get_duration() : 0.0;
}

void AudioStreamMedia::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_media_source", "source"), &AudioStreamMedia::set_media_source);
	ClassDB::bind_method(D_METHOD("get_media_source"), &AudioStreamMedia::get_media_source);
}

/* AudioStreamPlaybackMedia */

void AudioStreamPlaybackMedia::start(double p_from_pos) {
	active = true;
	if (p_from_pos > 0.0 && source.is_valid() && source->is_open()) {
		source->seek(p_from_pos);
	}
}

void AudioStreamPlaybackMedia::stop() {
	active = false;
}

bool AudioStreamPlaybackMedia::is_playing() const {
	return active && source.is_valid() && !source->is_audio_finished();
}

double AudioStreamPlaybackMedia::get_playback_position() const {
	return source.is_valid() ? source->get_audio_position() : 0.0;
}

void AudioStreamPlaybackMedia::seek(double p_time) {
	if (source.is_valid() && source->is_open()) {
		source->seek(p_time);
	}
}

int AudioStreamPlaybackMedia::mix(AudioFrame *p_buffer, float p_rate_scale, int p_frames) {
	if (!active || source.is_null()) {
		for (int i = 0; i < p_frames; i++) {
			p_buffer[i] = AudioFrame(0, 0);
		}
		return p_frames;
	}
	source->mix_audio(p_buffer, p_frames);
	return p_frames;
}
