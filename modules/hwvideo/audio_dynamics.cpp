/**************************************************************************/
/*  audio_dynamics.cpp                                                    */
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

#include "audio_dynamics.h"

#include "core/math/math_funcs.h"

static inline float power_to_db(float p_power) {
	return 4.3429448f * Math::log(p_power + 1e-12f); // 10 log10.
}

static float one_pole(double p_seconds, double p_step_rate) {
	return (float)(1.0 - Math::exp(-1.0 / (p_seconds * p_step_rate)));
}

void AudioDynamics::configure(bool p_compress, float p_strength, bool p_limit, int p_rate) {
	const float strength = CLAMP(p_strength, 0.0f, 1.0f);
	compress = p_compress && strength > 0.0f;
	limit = p_limit || compress;
	ratio = 1.0f + 4.0f * strength;
	max_boost_db = 12.0f * strength;
	const double rate = MAX(p_rate, 8000);
	const double blocks = rate / BLOCK;
	attack = one_pole(0.010, blocks);
	release = one_pole(0.150, blocks);
	typical_coefficient = one_pole(5.0, blocks);
	limiter_release = one_pole(0.080, rate);
	ceiling = (float)Math::db_to_linear(CEILING_DB);
}

void AudioDynamics::reset() {
	fast_power = 0.0f;
	typical_db = TARGET_DB;
	typical_valid = false;
	gain = 1.0f;
	limiter_gain = 1.0f;
	last_gain_db = 0.0f;
	last_limiter_db = 0.0f;
}

// After the envelopes have taken the block in.
float AudioDynamics::_target_gain() const {
	if (!compress) {
		return 1.0f;
	}
	const float level_db = power_to_db(fast_power);
	const float level_gain_db = CLAMP(TARGET_DB - typical_db, -6.0f, max_boost_db);
	// Above the typical level: reduced by the ratio, with a 6 dB soft knee.
	const float over = level_db - typical_db;
	const float slope = 1.0f - 1.0f / ratio;
	float reduction_db = 0.0f;
	if (over >= 3.0f) {
		reduction_db = over * slope;
	} else if (over > -3.0f) {
		reduction_db = slope * (over + 3.0f) * (over + 3.0f) / 12.0f;
	}
	// More than 6 dB below the typical level (and above the gate, so silence and hiss stay down): turned up, half as
	// much as the ratio would turn it down.
	float lift_db = 0.0f;
	const float under = typical_db - 6.0f - level_db;
	if (under > 0.0f && level_db > GATE_DB) {
		lift_db = MIN(under * slope * 0.5f, max_boost_db);
	}
	const float total_db = MIN(level_gain_db - reduction_db + lift_db, 1.5f * max_boost_db);
	return Math::db_to_linear(total_db);
}

void AudioDynamics::process(float *p_samples, uint32_t p_frames, int p_channels, float *r_gains) {
	if (!is_active() || p_channels <= 0) {
		return;
	}
	float lowest_limiter = 1.0f;
	uint32_t done = 0;
	while (done < p_frames) {
		const uint32_t n = MIN(BLOCK, p_frames - done);
		float *block = p_samples + (uint64_t)done * p_channels;
		const uint32_t samples = n * (uint32_t)p_channels;
		float target = gain;
		if (compress) {
			float power = 0.0f;
			for (uint32_t i = 0; i < samples; i++) {
				power += block[i] * block[i];
			}
			power /= (float)samples;
			fast_power += (power > fast_power ? attack : release) * (power - fast_power);
			const float level_db = power_to_db(fast_power);
			if (level_db > GATE_DB) {
				if (!typical_valid) {
					typical_db = level_db;
					typical_valid = true;
				} else {
					typical_db += typical_coefficient * (level_db - typical_db);
				}
			}
			target = _target_gain();
		}
		// Ramp from the last block's gain, then limit.
		const float step = (target - gain) / (float)n;
		float g = gain;
		for (uint32_t f = 0; f < n; f++) {
			g += step;
			float *frame = block + (uint64_t)f * p_channels;
			float peak = 0.0f;
			for (int c = 0; c < p_channels; c++) {
				frame[c] *= g;
				peak = MAX(peak, Math::abs(frame[c]));
			}
			if (limit) {
				if (peak * limiter_gain > ceiling) {
					limiter_gain = ceiling / peak;
				} else {
					limiter_gain += (1.0f - limiter_gain) * limiter_release;
				}
				if (limiter_gain < 0.9999f) {
					for (int c = 0; c < p_channels; c++) {
						frame[c] *= limiter_gain;
					}
				}
				lowest_limiter = MIN(lowest_limiter, limiter_gain);
			}
			if (r_gains) {
				r_gains[done + f] = limit && limiter_gain < 0.9999f ? g * limiter_gain : g;
			}
		}
		gain = target;
		done += n;
	}
	last_gain_db = (float)Math::linear_to_db(MAX(gain, 1e-6f));
	last_limiter_db = (float)Math::linear_to_db(lowest_limiter);
}
