/**************************************************************************/
/*  audio_tempo.cpp                                                       */
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

#include "audio_tempo.h"

#include "core/math/math_funcs.h"

AudioTempo::AudioTempo() {
	// Raised-cosine fades: fade_in + fade_out = 1 at every frame.
	for (uint32_t i = 0; i < SEGMENT; i++) {
		const double x = (i + 0.5) / SEGMENT;
		fade_in[i] = (float)(0.5 - 0.5 * Math::cos(Math::PI * x));
	}
}

void AudioTempo::reset(double p_position) {
	input_position = p_position;
	tail_valid = false;
	last_step_speed = 0.0;
	pending_count = 0;
	pending_offset = 0;
}

// Output frame i of a step is input frame (step start + i): the frames not handed out yet are still pending.
double AudioTempo::get_heard_position() const {
	if (last_step_speed == 0.0) {
		return input_position;
	}
	return input_position - last_step_speed * SEGMENT + (SEGMENT - pending_count);
}

int64_t AudioTempo::window_start() const {
	return (int64_t)Math::floor(input_position) - (int64_t)SEARCH;
}

// The offset (-SEARCH..SEARCH) of the input segment whose start matches the tail best: normalized cross-correlation of
// the mono signals, first every 8 offsets on every 4th frame, then every offset near the best on every 2nd frame.
int AudioTempo::_search(const AudioFrame *p_window) const {
	auto score = [&](int p_offset, uint32_t p_stride) {
		const AudioFrame *segment = p_window + SEARCH + p_offset;
		float dot = 0.0f;
		float energy = 1e-9f;
		for (uint32_t i = 0; i < SEGMENT; i += p_stride) {
			const float a = tail[i].left + tail[i].right;
			const float b = segment[i].left + segment[i].right;
			dot += a * b;
			energy += b * b;
		}
		return dot / Math::sqrt(energy);
	};
	int best = 0;
	float best_score = -1e30f;
	for (int offset = -(int)SEARCH; offset <= (int)SEARCH; offset += 8) {
		const float s = score(offset, 4);
		if (s > best_score) {
			best_score = s;
			best = offset;
		}
	}
	const int coarse = best;
	best_score = -1e30f;
	for (int offset = MAX(coarse - 7, -(int)SEARCH); offset <= MIN(coarse + 7, (int)SEARCH); offset++) {
		const float s = score(offset, 2);
		if (s > best_score) {
			best_score = s;
			best = offset;
		}
	}
	return best;
}

void AudioTempo::step(const AudioFrame *p_window, AudioFrame *p_out, double p_speed) {
	const int offset = tail_valid ? _search(p_window) : 0;
	const AudioFrame *segment = p_window + SEARCH + offset;
	if (tail_valid) {
		for (uint32_t i = 0; i < SEGMENT; i++) {
			const float in = fade_in[i];
			const float out = 1.0f - in;
			p_out[i] = AudioFrame(tail[i].left * out + segment[i].left * in, tail[i].right * out + segment[i].right * in);
		}
	} else {
		for (uint32_t i = 0; i < SEGMENT; i++) {
			p_out[i] = segment[i];
		}
	}
	for (uint32_t i = 0; i < SEGMENT; i++) {
		tail[i] = segment[SEGMENT + i];
	}
	tail_valid = true;
	last_step_speed = p_speed;
	input_position += p_speed * SEGMENT;
}
