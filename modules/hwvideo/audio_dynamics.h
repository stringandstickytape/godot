/**************************************************************************/
/*  audio_dynamics.h                                                      */
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

#include <cstdint>

// Night mode for decoded audio: loud passages are turned down and quiet ones up, so dialogue and explosions end up
// closer in level. Three stages, with the channels linked (one gain for all):
// - A leveller: the film's typical level (a gated average of the short-term level in dB over about 5 s) is moved
//   towards TARGET_DB, by at most max_boost_db up and 6 dB down.
// - A compressor: whatever is louder than that typical level is reduced by the ratio, and whatever is more than 6 dB
//   quieter (but above GATE_DB) is lifted by half as much. The total lift is at most 1.5 x max_boost_db.
// - A limiter: no sample above CEILING_DB (instant attack, 80 ms release). Also on by itself, for dialogue boost.
// The level and gain are worked out once per BLOCK frames and the gain is ramped across the block: a few multiply-adds
// per sample, two logs and one exp per block. In place, no allocation, no latency.
class AudioDynamics {
public:
	static constexpr uint32_t BLOCK = 32;
	static constexpr float TARGET_DB = -24.0f;
	static constexpr float CEILING_DB = -1.0f;
	static constexpr float GATE_DB = -55.0f; // Blocks quieter than this don't move the typical level.

	// p_strength 0-1: ratio 1 + 4 x strength, leveller boost up to 12 dB x strength. p_compress false: limiter only.
	void configure(bool p_compress, float p_strength, bool p_limit, int p_rate);
	void reset();
	bool is_active() const { return compress || limit; }
	// Interleaved float samples, p_channels per frame. r_gains (optional, p_frames long): the gain each frame got, to
	// apply the same to other channels of the same audio.
	void process(float *p_samples, uint32_t p_frames, int p_channels, float *r_gains = nullptr);

	// The gain of the last block: leveller and compressor (dB), and the limiter's reduction (dB, 0 or negative).
	float get_gain_db() const { return last_gain_db; }
	float get_limiter_db() const { return last_limiter_db; }
	float get_typical_level_db() const { return typical_db; }

private:
	bool compress = false;
	bool limit = false;
	float ratio = 1.0f;
	float max_boost_db = 0.0f;
	float attack = 0.0f; // One-pole coefficients per block.
	float release = 0.0f;
	float typical_coefficient = 0.0f;
	float limiter_release = 0.0f; // Per frame.
	float ceiling = 1.0f;

	float fast_power = 0.0f; // Short-term power, attack 10 ms, release 150 ms.
	float typical_db = TARGET_DB; // Gated average of the short-term level.
	bool typical_valid = false;
	float gain = 1.0f; // Gain at the end of the last block (linear).
	float limiter_gain = 1.0f;
	float last_gain_db = 0.0f;
	float last_limiter_db = 0.0f;

	float _target_gain() const;
};
