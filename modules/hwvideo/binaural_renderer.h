/**************************************************************************/
/*  binaural_renderer.h                                                   */
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

#include "binaural_hrtf.h"

#include "core/math/audio_frame.h"
#include "core/math/transform_3d.h"
#include "core/object/ref_counted.h"
#include "core/os/mutex.h"
#include "core/templates/local_vector.h"
#include "core/variant/array.h"
#include "core/variant/binder_common.h"
#include "core/variant/dictionary.h"
#include "core/variant/variant.h"

// Binaural rendering of a film's channels (or an ambisonic sound field) as virtual speakers in a room, for headphones
// and the headset's speakers. Every speaker is heard through the HRTF of its direction from the head, so it comes from
// in front, behind, above or below, outside the head; head tracking turns the directions every SUB frames.
//
// Per speaker and ear: a fractional delay line (the interaural delay, ramped across each sub-block) and a TAPS-long
// FIR convolution with the bilinearly interpolated, delay-free HRIR; when the direction changes, the old and new
// filters' outputs are crossfaded across the sub-block, so head turns make no clicks. No FFT, no block: no latency.
// Ambisonics (AmbiX: ACN order, SN3D, orders 1-3; higher orders are cut to 3) are decoded to world-fixed virtual
// speakers (a cube, an icosahedron, a 26-point Lebedev grid) with max-rE weights, and those are rendered the same way,
// so the sound field turns with the picture and stays put when the head turns.
// The room: first-order reflections off the six walls of a box (image sources, delayed and panned by a spherical-head
// model of time and level differences, low-passed) and a late reverb (an 8-line feedback delay network sized to the
// room, with its decay time), both scaled by the room's wet amount. They put the speakers at a distance, out of the
// head. Then the output EQ (the output profile's bands and the concha gain) and the gain.
//
// Thread use: the setters (any thread) leave the new state for process(), which takes it at its next call (the audio
// thread). The listener pose is interpolated across each process() call from the last pose to the newest.
class BinauralRenderer : public RefCounted {
	GDCLASS(BinauralRenderer, RefCounted);

public:
	enum Kind {
		KIND_POINT, // Through the HRTF from its position.
		KIND_LFE, // To both ears, no direction.
		KIND_HEAD_LEFT, // Head-locked (non-diegetic) left.
		KIND_HEAD_RIGHT,
	};

	static constexpr int TAPS = BinauralHrtfData::TAPS;
	static constexpr int SUB = 64; // Frames per head-tracking update.
	static constexpr int MAX_SOURCES = 48;
	static constexpr int MAX_INPUTS = 32;
	static constexpr int MAX_EQ = 8;
	static constexpr int REFLECTIONS = 6;
	static constexpr int REFL_LEN = 8192; // 170 ms of reflection delay at 48 kHz.
	static constexpr int FDN_LINES = 8;
	static constexpr int FDN_LEN = 8192;
	static constexpr float BASE_DELAY = 2.0f; // Samples, so the cubic delay never reads ahead.
	static constexpr int HIST = 132; // Input history kept: MAX_RUNTIME_DELAY + BASE_DELAY + the cubic's reach.
	static constexpr float MAX_RUNTIME_DELAY = 124.0f; // Samples (the HRTF's delays, scaled to the rate and the ITD tuning).

private:
	struct SourceConfig {
		int input = -1;
		Kind kind = KIND_POINT;
		Vector3 position; // World. at_infinity: a direction.
		float gain = 1.0f;
		bool at_infinity = false;
		bool reflections = true;
		int ambisonic = -1; // A virtual speaker of the ambisonic decode (its row in the decode matrix).
	};

	struct Room {
		bool enabled = false;
		Vector3 min;
		Vector3 max;
		float reflect[6] = {}; // -X +X -Y +Y -Z +Z walls (left, right, floor, ceiling, front, back).
		float rt60 = 0.5f;
		float wet = 0.0f; // Early and late together, 0-2.
		float late = 0.5f; // Late reverb level against the direct sound at wet 1.
	};

	struct EqBand {
		int type = 0; // 0 peak, 1 low shelf, 2 high shelf.
		float freq = 1000.0f;
		float gain_db = 0.0f;
		float q = 0.707f;
	};

	struct Biquad {
		float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
		float z1[2] = {}, z2[2] = {};
	};

	struct Config {
		LocalVector<SourceConfig> sources;
		int ambisonic_order = 0;
		int ambisonic_input = 0;
		Basis ambisonic_frame;
		float ambisonic_gain = 1.0f;
		Room room;
		LocalVector<EqBand> eq;
		float itd_scale = 1.0f;
		float concha_db = 0.0f;
		float rate = (float)BinauralHrtfData::RATE; // The output's sample rate.
		float gain = 1.0f;
		Ref<BinauralHrtfData> hrtf; // Tuned (the pinna warp applied).
	};

	struct SourceState {
		SourceConfig config;
		float hist[HIST + SUB] = {};
		float xd[2][TAPS - 1 + SUB] = {};
		float h[2][TAPS] = {};
		float delay[2] = {};
		bool have_filter = false;
		Vector3 direction; // Head-relative, of the filter in h.
		LocalVector<float> refl; // REFL_LEN, the input for the reflections.
		uint32_t refl_pos = 0;
		float refl_delay[REFLECTIONS][2] = {};
		float refl_gain[REFLECTIONS][2] = {};
		bool refl_valid = false;
	};

	mutable BinaryMutex mutex; // pending, pending_serial, pending_pose, stats.
	Config pending;
	uint32_t pending_serial = 1;
	Transform3D pending_pose;
	Ref<BinauralHrtfData> base_hrtf; // As loaded.
	float warp = 1.0f;
	float rate_wanted = (float)BinauralHrtfData::RATE; // set_sample_rate().
	uint64_t stats_frames = 0;
	uint64_t stats_usec = 0;
	uint64_t stats_calls = 0;
	uint64_t stats_max_usec = 0;
	uint32_t stats_max_frames = 0;
	bool reset_requested = false;

	// process() only.
	Config config;
	uint32_t config_serial = 0;
	Transform3D pose;
	bool have_pose = false;
	LocalVector<SourceState *> states;
	LocalVector<float> decode; // [virtual speaker][ambisonic channel]
	int decode_channels = 0;
	float in_block[MAX_SOURCES][SUB] = {};
	float filter_new[2][TAPS] = {};
	float y_old[SUB] = {};
	float y_new[SUB] = {};
	float out_l[SUB] = {};
	float out_r[SUB] = {};
	float refl_l[SUB] = {};
	float refl_r[SUB] = {};
	float send[SUB] = {};
	float refl_lp[2] = {};
	// Late reverb.
	LocalVector<float> fdn; // FDN_LINES x FDN_LEN
	uint32_t fdn_pos = 0;
	int fdn_len[FDN_LINES] = {};
	float fdn_gain[FDN_LINES] = {};
	float fdn_damp[FDN_LINES] = {};
	float fdn_lp[FDN_LINES] = {};
	float fdn_norm = 0.0f;
	LocalVector<float> predelay;
	uint32_t predelay_pos = 0;
	int predelay_len = 0;
	Room fdn_room;
	bool fdn_ready = false;
	float fdn_rate = 0.0f;
	Biquad eq_filters[MAX_EQ + 1];
	int eq_count = 0;

	void _apply_config();
	void _setup_reverb();
	void _setup_eq();
	void _reset_state();
	void _render_sub(const float *p_in, int p_stride, int p_channels, int p_frames, const Transform3D &p_pose, float *r_l, float *r_r);
	void _reflections(SourceState &p_state, const Transform3D &p_pose, int p_frames);
	void _reverb(int p_frames);

	static void _biquad_set(Biquad &r_b, int p_type, float p_freq, float p_gain_db, float p_q, float p_rate);

protected:
	static void _bind_methods();

public:
	// The audio thread: p_frames frames of p_channels inputs, p_stride floats apart per frame, to stereo. r_out may not
	// alias p_in.
	void process(const float *p_in, int p_stride, int p_channels, int p_frames, AudioFrame *r_out);
	bool is_ready() const;

	Error load_hrtf(const String &p_path);
	Error save_hrtf(const String &p_path, const String &p_name, const String &p_credit);
	Dictionary get_hrtf_info() const;
	String last_error;
	String get_last_error() const { return last_error; }

	void set_sources(const Array &p_sources);
	void set_ambisonics(int p_order, int p_first_input, const Basis &p_frame, float p_gain);
	void set_listener(const Transform3D &p_pose);
	void set_room(const Dictionary &p_room);
	void set_eq(const Array &p_bands);
	void set_tuning(float p_itd_scale, float p_pinna_warp, float p_concha_db);
	Dictionary get_tuning() const;
	void set_gain(float p_gain);
	// The output's mix rate (MediaSource sets it from the AudioServer); the HRTF is resampled to it.
	void set_sample_rate(float p_rate);
	float get_sample_rate() const;
	void reset();

	// For scripts and tests: p_input interleaved (p_channels per frame) to interleaved stereo.
	PackedFloat32Array render(const PackedFloat32Array &p_input, int p_channels);
	Dictionary lookup_hrir(float p_azimuth, float p_elevation) const;
	static PackedFloat32Array ambisonic_encode(const Vector3 &p_direction, int p_order);
	static PackedVector3Array virtual_speakers(int p_order);
	Dictionary get_stats() const;
	void reset_stats();

	BinauralRenderer();
	~BinauralRenderer();
};

VARIANT_ENUM_CAST(BinauralRenderer::Kind);
