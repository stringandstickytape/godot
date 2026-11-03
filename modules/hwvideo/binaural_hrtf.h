/**************************************************************************/
/*  binaural_hrtf.h                                                       */
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
#include "core/string/ustring.h"
#include "core/templates/local_vector.h"

// A head-related transfer function set on a regular grid of directions, as BinauralRenderer uses it: for each grid
// direction and ear an aligned impulse response (TAPS samples at 48 kHz, the onset PRE samples in) and the delay that
// was taken out of it (samples, the smallest of the set 0). Interpolation between grid points is bilinear on both;
// separating the delay from the response keeps the blend free of comb filtering.
//
// Directions: azimuth in degrees from the front, positive to the right (clockwise seen from above); elevation in
// degrees, positive up. Grid: AZ azimuths AZ_STEP apart from 0, EL elevations EL_STEP apart from EL_MIN to 90.
//
// Sources: a pack (the compact file below, what the app bundles) or a SOFA file (AES69, SimpleFreeFieldHRIR, read
// with libmysofa), which is resampled to 48 kHz if needed, diffuse-field equalised, aligned and put on the grid
// (inverse-distance weights of the three nearest measured directions).
//
// Pack: little-endian. "FHRT", u32 version (1), u32 rate, u32 taps, u32 az, u32 el, f32 el_min, f32 el_step,
// f32 az_step, f32 scale, u32 + UTF-8 name, u32 + UTF-8 credit, f32 delays [el][az][2], s16 taps [el][az][2][taps]
// (value x scale / 32767).
class BinauralHrtfData : public RefCounted {
	GDCLASS(BinauralHrtfData, RefCounted);

public:
	static constexpr int RATE = 48000;
	static constexpr int TAPS = 128;
	static constexpr int AZ = 48;
	static constexpr int EL = 14;
	static constexpr float AZ_STEP = 7.5f;
	static constexpr float EL_MIN = -40.0f;
	static constexpr float EL_STEP = 10.0f;
	static constexpr int PRE = 4; // Samples kept before the onset.
	static constexpr float MAX_DELAY = 56.0f; // Samples; the interaural delay is at most about 40.
	static constexpr int POINTS = AZ * EL;

	String name;
	String credit; // Who made it and under what licence, as the pack or the SOFA file says.
	String source; // The path it was loaded from.
	int measured = 0; // Directions in the SOFA file it came from (0 for a pack).
	float warp = 1.0f; // make_tuned(): the pinna warp it was made with.
	LocalVector<float> ir; // [EL][AZ][2][TAPS]
	LocalVector<float> delay; // [EL][AZ][2]

	bool is_valid() const { return ir.size() == (uint32_t)POINTS * 2 * TAPS && delay.size() == (uint32_t)POINTS * 2; }
	static int index(int p_el, int p_az) { return p_el * AZ + p_az; }
	static Vector3 grid_direction(int p_el, int p_az); // Godot axes: -Z front, +X right, +Y up.

	// Bilinear: r_left / r_right TAPS long. Delays in samples.
	void lookup(float p_azimuth, float p_elevation, float *r_left, float *r_right, float &r_delay_left, float &r_delay_right) const;

	Error load(const String &p_path, String &r_error); // A pack or (by extension) a .sofa file.
	Error load_pack(const String &p_path, String &r_error);
	Error load_sofa(const String &p_path, String &r_error);
	Error save_pack(const String &p_path) const;
	// A copy with the pinna warp: each response stretched in time by 1 / p_warp, so its spectral features (the pinna
	// notches above all) move up by p_warp (0.8-1.25), each ear's energy kept; and resampled to p_rate (the output's
	// mix rate, when it isn't RATE: the delays scaled, the responses read at RATE / p_rate steps).
	Ref<BinauralHrtfData> tuned(float p_warp, float p_rate = (float)RATE) const;
	float rate = (float)RATE; // tuned(): the rate it was made for.
};
