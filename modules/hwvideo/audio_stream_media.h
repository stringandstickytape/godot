/**************************************************************************/
/*  audio_stream_media.h                                                  */
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

#include "servers/audio/audio_stream.h"

// The decoded audio of a MediaSource as an AudioStream, for an AudioStreamPlayer. The playback pulls from the source's
// ring buffer (stereo, already at the AudioServer mix rate) and reports the media time of the last sample mixed, so
// get_playback_position() (plus AudioServer.get_time_since_last_mix() - AudioServer.get_output_latency()) is the A/V
// master clock. Play it from one player at a time: playbacks share the source's buffer.
class AudioStreamMedia : public AudioStream {
	GDCLASS(AudioStreamMedia, AudioStream);

	Ref<MediaSource> source;

protected:
	static void _bind_methods();

public:
	void set_media_source(const Ref<MediaSource> &p_source);
	Ref<MediaSource> get_media_source() const { return source; }

	virtual Ref<AudioStreamPlayback> instantiate_playback() override;
	virtual String get_stream_name() const override;
	virtual double get_length() const override;
	virtual bool is_monophonic() const override { return true; }
};

class AudioStreamPlaybackMedia : public AudioStreamPlayback {
	GDCLASS(AudioStreamPlaybackMedia, AudioStreamPlayback);
	friend class AudioStreamMedia;

	Ref<MediaSource> source;
	bool active = false;

protected:
	static void _bind_methods() {}

public:
	// p_from_pos > 0 seeks the source (and with it the video) unless it is already within 50 ms of it; 0 plays from
	// where the source is.
	virtual void start(double p_from_pos = 0.0) override;
	virtual void stop() override;
	virtual bool is_playing() const override;
	virtual int get_loop_count() const override { return 0; }
	virtual double get_playback_position() const override;
	virtual void seek(double p_time) override;
	// Ignores p_rate_scale (pitch_scale): the audio is the clock.
	virtual int mix(AudioFrame *p_buffer, float p_rate_scale, int p_frames) override;
};
