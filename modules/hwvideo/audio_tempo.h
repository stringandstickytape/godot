/**************************************************************************/
/*  audio_tempo.h                                                         */
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

#include "core/math/audio_frame.h"

#include <cstdint>

// Plays stereo audio faster or slower without changing its pitch (WSOLA: waveform-similarity overlap-add). Each step
// makes SEGMENT frames of output: the continuation of the previous segment cross-faded into the input segment near the
// ideal position (input_position) whose start looks most like that continuation, searched within SEARCH frames. The
// input position then moves on by speed x SEGMENT. The search compares a mono, decimated signal: about 2 % of the work of
// a full search, a few million multiply-adds a second. Fixed buffers, no allocation.
class AudioTempo {
public:
	static constexpr uint32_t SEGMENT = 1024; // About 23 ms at 44.1 kHz: the output step and the cross-fade.
	static constexpr uint32_t SEARCH = 512; // How far the input segment may move to match.
	static constexpr uint32_t WINDOW = 2 * SEARCH + 2 * SEGMENT; // Input frames one step looks at.

	// Starts over at input frame p_position (no cross-fade with what came before).
	void reset(double p_position);
	// The input window of the next step starts at this frame (may be negative right after reset: those frames are 0).
	int64_t window_start() const;
	// The next step needs input up to (not including) this frame.
	int64_t window_end() const { return window_start() + WINDOW; }
	// One step. p_window: WINDOW frames from window_start(). Writes SEGMENT frames to p_out.
	void step(const AudioFrame *p_window, AudioFrame *p_out, double p_speed);
	double get_input_position() const { return input_position; }
	// The input frame of the last output frame handed out (what is being heard, before the output latency).
	double get_heard_position() const;

	// The input window of a step, copied out of the caller's ring.
	AudioFrame window[WINDOW];
	// Output made by a step but not mixed yet (when the mixer asks for less than SEGMENT).
	AudioFrame pending[SEGMENT];
	uint32_t pending_count = 0;
	uint32_t pending_offset = 0;

	AudioTempo();

private:
	double input_position = 0.0;
	AudioFrame tail[SEGMENT]; // The continuation of the last segment, faded out in the next step.
	bool tail_valid = false;
	double last_step_speed = 0.0; // 0: no step since reset().
	float fade_in[SEGMENT];

	int _search(const AudioFrame *p_window) const;
};
