/**************************************************************************/
/*  binaural_renderer.cpp                                                 */
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

#include "binaural_renderer.h"

#include "core/object/class_db.h"
#include "core/os/os.h"

namespace {

constexpr float SPEED_OF_SOUND = 343.0f;
constexpr float HEAD_RADIUS = 0.0875f;
constexpr int MAX_ORDER = 3;

// Real spherical harmonics, SN3D, ACN order, up to order 3. AmbiX axes: x front, y left, z up; p unit length.
void sh_sn3d(int p_order, float x, float y, float z, float *r) {
	r[0] = 1.0f;
	if (p_order < 1) {
		return;
	}
	r[1] = y;
	r[2] = z;
	r[3] = x;
	if (p_order < 2) {
		return;
	}
	const float s3 = Math::sqrt(3.0f);
	r[4] = s3 * x * y;
	r[5] = s3 * y * z;
	r[6] = 0.5f * (3.0f * z * z - 1.0f);
	r[7] = s3 * x * z;
	r[8] = 0.5f * s3 * (x * x - y * y);
	if (p_order < 3) {
		return;
	}
	const float a = Math::sqrt(5.0f / 8.0f);
	const float b = Math::sqrt(15.0f);
	const float c = Math::sqrt(3.0f / 8.0f);
	r[9] = a * y * (3.0f * x * x - y * y);
	r[10] = b * x * y * z;
	r[11] = c * y * (5.0f * z * z - 1.0f);
	r[12] = 0.5f * z * (5.0f * z * z - 3.0f);
	r[13] = c * x * (5.0f * z * z - 1.0f);
	r[14] = 0.5f * b * z * (x * x - y * y);
	r[15] = a * x * (x * x - 3.0f * y * y);
}

// Virtual speakers for an ambisonic order, AmbiX axes, with their quadrature weights (sum 1): a cube (a spherical
// 3-design) for order 1, an icosahedron (5-design) for 2, Lebedev's 26 points (exact to degree 7) for 3.
void speaker_grid(int p_order, LocalVector<Vector3> &r_dirs, LocalVector<float> &r_weights) {
	r_dirs.clear();
	r_weights.clear();
	if (p_order <= 1) {
		for (int i = 0; i < 8; i++) {
			r_dirs.push_back(Vector3(i & 1 ? 1 : -1, i & 2 ? 1 : -1, i & 4 ? 1 : -1).normalized());
			r_weights.push_back(1.0f / 8.0f);
		}
	} else if (p_order == 2) {
		const float g = (1.0f + Math::sqrt(5.0f)) * 0.5f;
		for (int s1 = -1; s1 <= 1; s1 += 2) {
			for (int s2 = -1; s2 <= 1; s2 += 2) {
				r_dirs.push_back(Vector3(0, s1, s2 * g).normalized());
				r_dirs.push_back(Vector3(s1, s2 * g, 0).normalized());
				r_dirs.push_back(Vector3(s2 * g, 0, s1).normalized());
			}
		}
		for (int i = 0; i < 12; i++) {
			r_weights.push_back(1.0f / 12.0f);
		}
	} else {
		for (int axis = 0; axis < 3; axis++) {
			for (int s = -1; s <= 1; s += 2) {
				Vector3 v;
				v[axis] = (float)s;
				r_dirs.push_back(v);
				r_weights.push_back(1.0f / 21.0f);
			}
		}
		for (int axis = 0; axis < 3; axis++) {
			for (int s1 = -1; s1 <= 1; s1 += 2) {
				for (int s2 = -1; s2 <= 1; s2 += 2) {
					Vector3 v;
					v[(axis + 1) % 3] = (float)s1;
					v[(axis + 2) % 3] = (float)s2;
					r_dirs.push_back(v.normalized());
					r_weights.push_back(4.0f / 105.0f);
				}
			}
		}
		for (int i = 0; i < 8; i++) {
			r_dirs.push_back(Vector3(i & 1 ? 1 : -1, i & 2 ? 1 : -1, i & 4 ? 1 : -1).normalized());
			r_weights.push_back(9.0f / 280.0f);
		}
	}
}

// max-rE weight of degree p_n for order p_order: P_n(cos(137.9 deg / (order + 1.51))).
float max_re(int p_n, int p_order) {
	const float x = Math::cos(Math::deg_to_rad(137.9f / (p_order + 1.51f)));
	float p0 = 1.0f;
	float p1 = x;
	if (p_n == 0) {
		return p0;
	}
	for (int n = 2; n <= p_n; n++) {
		const float p2 = ((2 * n - 1) * x * p1 - (n - 1) * p0) / n;
		p0 = p1;
		p1 = p2;
	}
	return p1;
}

inline Vector3 ambix_to_godot(const Vector3 &v) {
	return Vector3(-v.y, v.z, -v.x);
}

inline Vector3 godot_to_ambix(const Vector3 &v) {
	return Vector3(-v.z, -v.x, v.y);
}

inline float cubic(const float *p, float t) {
	return p[1] + 0.5f * t * (p[2] - p[0] + t * (2.0f * p[0] - 5.0f * p[1] + 4.0f * p[2] - p[3] + t * (3.0f * (p[1] - p[2]) + p[3] - p[0])));
}

// y[i] = sum over k of h[k] x[i - k]; x has TAPS - 1 samples of history before it.
inline void convolve(const float *__restrict p_x, const float *__restrict p_h, float *__restrict r_y, int p_frames) {
	for (int i = 0; i < p_frames; i++) {
		r_y[i] = 0.0f;
	}
	for (int k = 0; k < BinauralRenderer::TAPS; k++) {
		const float hk = p_h[k];
		const float *__restrict xk = p_x - k;
		for (int i = 0; i < p_frames; i++) {
			r_y[i] += hk * xk[i];
		}
	}
}

} // namespace

BinauralRenderer::BinauralRenderer() {
	fdn.resize(FDN_LINES * FDN_LEN);
	predelay.resize(FDN_LEN);
	for (float &v : fdn) {
		v = 0.0f;
	}
	for (float &v : predelay) {
		v = 0.0f;
	}
	_setup_eq();
}

BinauralRenderer::~BinauralRenderer() {
	for (SourceState *s : states) {
		memdelete(s);
	}
}

bool BinauralRenderer::is_ready() const {
	MutexLock lock(mutex);
	return pending.hrtf.is_valid() && pending.hrtf->is_valid();
}

Error BinauralRenderer::load_hrtf(const String &p_path) {
	Ref<BinauralHrtfData> data;
	data.instantiate();
	String error;
	const Error err = data->load(p_path, error);
	if (err != OK) {
		last_error = error;
		return err;
	}
	last_error = String();
	Ref<BinauralHrtfData> tuned = data->tuned(warp, rate_wanted);
	MutexLock lock(mutex);
	base_hrtf = data;
	pending.hrtf = tuned;
	pending_serial++;
	return OK;
}

Error BinauralRenderer::save_hrtf(const String &p_path, const String &p_name, const String &p_credit) {
	Ref<BinauralHrtfData> data;
	{
		MutexLock lock(mutex);
		data = base_hrtf;
	}
	ERR_FAIL_COND_V_MSG(data.is_null() || !data->is_valid(), ERR_UNCONFIGURED, "BinauralRenderer: no HRTF loaded.");
	Ref<BinauralHrtfData> out = data->tuned(1.0f);
	if (!p_name.is_empty()) {
		out->name = p_name;
	}
	if (!p_credit.is_empty()) {
		out->credit = p_credit;
	}
	return out->save_pack(p_path);
}

Dictionary BinauralRenderer::get_hrtf_info() const {
	Ref<BinauralHrtfData> data;
	{
		MutexLock lock(mutex);
		data = base_hrtf;
	}
	Dictionary d;
	d["loaded"] = data.is_valid() && data->is_valid();
	if (data.is_valid()) {
		d["name"] = data->name;
		d["credit"] = data->credit;
		d["source"] = data->source;
		d["measured"] = data->measured;
	}
	d["grid"] = vformat("%d x %d directions, %d taps at %d Hz", BinauralHrtfData::AZ, BinauralHrtfData::EL, TAPS, BinauralHrtfData::RATE);
	return d;
}

void BinauralRenderer::set_sources(const Array &p_sources) {
	LocalVector<SourceConfig> list;
	for (int i = 0; i < p_sources.size() && (int)list.size() < MAX_SOURCES; i++) {
		const Dictionary d = p_sources[i];
		SourceConfig s;
		s.input = d.get("input", -1);
		const Variant kind = d.get("kind", "point");
		if (kind.get_type() == Variant::INT) {
			s.kind = (Kind)CLAMP((int)kind, 0, (int)KIND_HEAD_RIGHT);
		} else {
			const String k = kind;
			s.kind = k == "lfe" ? KIND_LFE : k == "head_left" ? KIND_HEAD_LEFT
					: k == "head_right"                     ? KIND_HEAD_RIGHT
															: KIND_POINT;
		}
		s.position = d.get("position", Vector3(0, 0, -2));
		s.gain = d.get("gain", 1.0);
		s.at_infinity = d.get("at_infinity", false);
		s.reflections = d.get("reflections", true);
		list.push_back(s);
	}
	MutexLock lock(mutex);
	pending.sources = list;
	pending_serial++;
}

void BinauralRenderer::set_ambisonics(int p_order, int p_first_input, const Basis &p_frame, float p_gain) {
	MutexLock lock(mutex);
	pending.ambisonic_order = CLAMP(p_order, 0, 31);
	pending.ambisonic_input = MAX(0, p_first_input);
	pending.ambisonic_frame = p_frame.orthonormalized();
	pending.ambisonic_gain = p_gain;
	pending_serial++;
}

void BinauralRenderer::set_listener(const Transform3D &p_pose) {
	MutexLock lock(mutex);
	pending_pose = Transform3D(p_pose.basis.orthonormalized(), p_pose.origin);
	pending_pose_usec = OS::get_singleton()->get_ticks_usec();
	pending_pose_serial++;
}

void BinauralRenderer::set_room(const Dictionary &p_room) {
	Room r;
	r.enabled = p_room.get("enabled", p_room.has("min") && p_room.has("max"));
	r.min = p_room.get("min", Vector3(-3, 0, -3));
	r.max = p_room.get("max", Vector3(3, 3, 3));
	const Variant reflect = p_room.get("reflect", 0.5);
	for (int w = 0; w < 6; w++) {
		if (reflect.get_type() == Variant::ARRAY || reflect.get_type() == Variant::PACKED_FLOAT32_ARRAY || reflect.get_type() == Variant::PACKED_FLOAT64_ARRAY) {
			const Array a = reflect;
			r.reflect[w] = w < a.size() ? CLAMP((float)a[w], 0.0f, 1.0f) : 0.0f;
		} else {
			r.reflect[w] = CLAMP((float)reflect, 0.0f, 1.0f);
		}
	}
	r.rt60 = CLAMP((float)p_room.get("rt60", 0.5), 0.05f, 6.0f);
	r.wet = CLAMP((float)p_room.get("wet", 0.0), 0.0f, 2.0f);
	r.late = CLAMP((float)p_room.get("late", 0.5), 0.0f, 2.0f);
	MutexLock lock(mutex);
	pending.room = r;
	pending_serial++;
}

void BinauralRenderer::set_eq(const Array &p_bands) {
	LocalVector<EqBand> bands;
	for (int i = 0; i < p_bands.size() && (int)bands.size() < MAX_EQ; i++) {
		const Dictionary d = p_bands[i];
		EqBand b;
		const String type = d.get("type", "peak");
		b.type = type == "low_shelf" ? 1 : type == "high_shelf" ? 2
																: 0;
		b.freq = CLAMP((float)d.get("freq", 1000.0), 20.0f, 20000.0f);
		b.gain_db = CLAMP((float)d.get("gain_db", 0.0), -24.0f, 24.0f);
		b.q = CLAMP((float)d.get("q", 0.707), 0.1f, 20.0f);
		bands.push_back(b);
	}
	MutexLock lock(mutex);
	pending.eq = bands;
	pending_serial++;
}

void BinauralRenderer::set_tuning(float p_itd_scale, float p_pinna_warp, float p_concha_db) {
	const float new_warp = CLAMP(p_pinna_warp, 0.8f, 1.25f);
	Ref<BinauralHrtfData> base;
	bool rebuild = false;
	{
		MutexLock lock(mutex);
		rebuild = !Math::is_equal_approx(new_warp, warp);
		base = base_hrtf;
	}
	Ref<BinauralHrtfData> tuned;
	if (rebuild && base.is_valid()) {
		tuned = base->tuned(new_warp, rate_wanted);
	}
	MutexLock lock(mutex);
	warp = new_warp;
	if (tuned.is_valid()) {
		pending.hrtf = tuned;
	}
	pending.itd_scale = CLAMP(p_itd_scale, 0.5f, 1.5f);
	pending.concha_db = CLAMP(p_concha_db, -12.0f, 12.0f);
	pending_serial++;
}

Dictionary BinauralRenderer::get_tuning() const {
	MutexLock lock(mutex);
	Dictionary d;
	d["itd_scale"] = pending.itd_scale;
	d["pinna_warp"] = warp;
	d["concha_db"] = pending.concha_db;
	return d;
}

void BinauralRenderer::set_gain(float p_gain) {
	MutexLock lock(mutex);
	pending.gain = MAX(0.0f, p_gain);
	pending_serial++;
}

void BinauralRenderer::set_sample_rate(float p_rate) {
	const float r = CLAMP(p_rate, 8000.0f, 192000.0f);
	Ref<BinauralHrtfData> base;
	float w = 1.0f;
	{
		MutexLock lock(mutex);
		if (Math::is_equal_approx(r, rate_wanted)) {
			return;
		}
		rate_wanted = r;
		base = base_hrtf;
		w = warp;
	}
	Ref<BinauralHrtfData> tuned;
	if (base.is_valid()) {
		tuned = base->tuned(w, r);
	}
	MutexLock lock(mutex);
	if (tuned.is_valid()) {
		pending.hrtf = tuned;
	}
	pending.rate = r;
	pending_serial++;
}

float BinauralRenderer::get_sample_rate() const {
	MutexLock lock(mutex);
	return rate_wanted;
}

void BinauralRenderer::reset() {
	MutexLock lock(mutex);
	reset_requested = true;
}

void BinauralRenderer::_reset_state() {
	have_pose = false;
	for (SourceState *s : states) {
		SourceConfig c = s->config;
		LocalVector<float> refl;
		refl = s->refl;
		*s = SourceState();
		s->config = c;
		s->refl = refl;
		for (float &v : s->refl) {
			v = 0.0f;
		}
	}
	for (float &v : fdn) {
		v = 0.0f;
	}
	for (float &v : predelay) {
		v = 0.0f;
	}
	for (int j = 0; j < FDN_LINES; j++) {
		fdn_lp[j] = 0.0f;
	}
	refl_lp[0] = refl_lp[1] = 0.0f;
	for (Biquad &b : eq_filters) {
		b.z1[0] = b.z1[1] = b.z2[0] = b.z2[1] = 0.0f;
	}
}

void BinauralRenderer::_apply_config() {
	// The film's own speakers, then the ambisonic decode's virtual speakers.
	LocalVector<SourceConfig> all;
	all = config.sources;
	decode.clear();
	decode_channels = 0;
	if (config.ambisonic_order > 0) {
		const int order = MIN(config.ambisonic_order, MAX_ORDER);
		decode_channels = (order + 1) * (order + 1);
		LocalVector<Vector3> dirs;
		LocalVector<float> weights;
		speaker_grid(order, dirs, weights);
		float sh[16];
		for (uint32_t s = 0; s < dirs.size() && (int)all.size() < MAX_SOURCES; s++) {
			sh_sn3d(order, dirs[s].x, dirs[s].y, dirs[s].z, sh);
			for (int n = 0; n <= order; n++) {
				const float k = weights[s] * max_re(n, order) * (2 * n + 1) * config.ambisonic_gain;
				for (int m = -n; m <= n; m++) {
					decode.push_back(k * sh[n * n + n + m]);
				}
			}
			SourceConfig v;
			v.kind = KIND_POINT;
			v.position = config.ambisonic_frame.xform(ambix_to_godot(dirs[s]));
			v.at_infinity = true;
			v.reflections = false;
			v.ambisonic = (int)s;
			all.push_back(v);
		}
	}
	while (states.size() > all.size()) {
		memdelete(states[states.size() - 1]);
		states.resize(states.size() - 1);
	}
	for (uint32_t i = 0; i < all.size(); i++) {
		if (i >= states.size()) {
			SourceState *s = memnew(SourceState);
			s->refl.resize(REFL_LEN);
			for (float &v : s->refl) {
				v = 0.0f;
			}
			states.push_back(s);
		}
		SourceState &s = *states[i];
		const SourceConfig &c = all[i];
		if (s.config.input != c.input || s.config.kind != c.kind || s.config.ambisonic != c.ambisonic) {
			// Another signal in this slot: its history goes.
			LocalVector<float> refl;
			refl = s.refl;
			s = SourceState();
			s.refl = refl;
			for (float &v : s.refl) {
				v = 0.0f;
			}
		}
		if (s.config.at_infinity != c.at_infinity) {
			s.refl_valid = false;
		}
		s.config = c;
	}
	const Room &r = config.room;
	if (!fdn_ready || config.rate != fdn_rate || r.min != fdn_room.min || r.max != fdn_room.max || r.rt60 != fdn_room.rt60 || r.late != fdn_room.late || r.wet != fdn_room.wet) {
		_setup_reverb();
	}
	_setup_eq();
}

void BinauralRenderer::_setup_reverb() {
	const Room &r = config.room;
	const Vector3 size = (r.max - r.min).abs().clamp(Vector3(1, 1, 1), Vector3(100, 100, 100));
	const float volume = size.x * size.y * size.z;
	const float surface = 2.0f * (size.x * size.y + size.y * size.z + size.z * size.x);
	const float free_path = 4.0f * volume / surface; // Metres between reflections on average.
	const float base = CLAMP(free_path / SPEED_OF_SOUND * config.rate, 240.0f, 2600.0f);
	static const float ratios[FDN_LINES] = { 1.0f, 1.137f, 1.283f, 1.437f, 1.571f, 1.733f, 1.879f, 2.053f };
	float mean_square = 0.0f;
	for (int j = 0; j < FDN_LINES; j++) {
		fdn_len[j] = CLAMP((int)(base * ratios[j]) | 1, 101, FDN_LEN - 1);
		fdn_gain[j] = Math::pow(10.0f, -3.0f * fdn_len[j] / (r.rt60 * config.rate));
		fdn_damp[j] = 0.25f;
		mean_square += fdn_gain[j] * fdn_gain[j];
	}
	mean_square /= FDN_LINES;
	fdn_norm = Math::sqrt(MAX(0.0f, 1.0f - mean_square)) * r.late * r.wet;
	predelay_len = CLAMP((int)base, 64, FDN_LEN - 1);
	fdn_room = r;
	fdn_rate = config.rate;
	fdn_ready = true;
}

void BinauralRenderer::_biquad_set(Biquad &r_b, int p_type, float p_freq, float p_gain_db, float p_q, float p_rate) {
	// RBJ cookbook.
	const float a = Math::pow(10.0f, p_gain_db / 40.0f);
	const float w = 2.0f * Math::PI * p_freq / p_rate;
	const float cw = Math::cos(w);
	const float alpha = Math::sin(w) / (2.0f * p_q);
	float b0, b1, b2, a0, a1, a2;
	if (p_type == 1 || p_type == 2) {
		const float sa = 2.0f * Math::sqrt(a) * alpha;
		const float s = p_type == 1 ? 1.0f : -1.0f; // Low shelf, high shelf.
		b0 = a * ((a + 1) - s * (a - 1) * cw + sa);
		b1 = s * 2 * a * ((a - 1) - s * (a + 1) * cw);
		b2 = a * ((a + 1) - s * (a - 1) * cw - sa);
		a0 = (a + 1) + s * (a - 1) * cw + sa;
		a1 = -s * 2 * ((a - 1) + s * (a + 1) * cw);
		a2 = (a + 1) + s * (a - 1) * cw - sa;
	} else {
		b0 = 1 + alpha * a;
		b1 = -2 * cw;
		b2 = 1 - alpha * a;
		a0 = 1 + alpha / a;
		a1 = -2 * cw;
		a2 = 1 - alpha / a;
	}
	r_b.b0 = b0 / a0;
	r_b.b1 = b1 / a0;
	r_b.b2 = b2 / a0;
	r_b.a1 = a1 / a0;
	r_b.a2 = a2 / a0;
}

void BinauralRenderer::_setup_eq() {
	eq_count = 0;
	for (const EqBand &b : config.eq) {
		if (eq_count >= MAX_EQ) {
			break;
		}
		_biquad_set(eq_filters[eq_count++], b.type, MIN(b.freq, config.rate * 0.45f), b.gain_db, b.q, config.rate);
	}
	if (!Math::is_zero_approx(config.concha_db)) {
		// The concha's first resonance, around 4.5 kHz, about an octave wide.
		_biquad_set(eq_filters[eq_count++], 0, 4500.0f, config.concha_db, 1.4f, config.rate);
	}
}

void BinauralRenderer::_reflections(SourceState &p_state, const Transform3D &p_pose, int p_frames) {
	const Room &room = config.room;
	const SourceConfig &c = p_state.config;
	const Vector3 head = p_pose.origin;
	const float direct = MAX(0.3f, c.position.distance_to(head));
	float target_delay[REFLECTIONS][2];
	float target_gain[REFLECTIONS][2];
	for (int w = 0; w < REFLECTIONS; w++) {
		const int axis = w / 2;
		const float wall = (w & 1) ? room.max[axis] : room.min[axis];
		Vector3 image = c.position;
		image[axis] = 2.0f * wall - c.position[axis];
		const Vector3 rel = image - head;
		const float distance = MAX(direct, rel.length());
		const float extra = CLAMP((distance - direct) / SPEED_OF_SOUND * config.rate, 1.0f, (float)(REFL_LEN - SUB - 64));
		const Vector3 local = p_pose.basis.xform_inv(rel) / MAX(distance, 0.001f);
		const float lateral = CLAMP(local.x, -1.0f, 1.0f); // Sine of the angle to the right.
		const float itd = HEAD_RADIUS / SPEED_OF_SOUND * (Math::asin(lateral) + lateral) * config.rate;
		const float g = room.reflect[w] * room.wet * direct / distance;
		target_delay[w][0] = MAX(1.0f, extra + 0.5f * itd);
		target_delay[w][1] = MAX(1.0f, extra - 0.5f * itd);
		target_gain[w][0] = g * (1.0f - 0.35f * lateral);
		target_gain[w][1] = g * (1.0f + 0.35f * lateral);
	}
	if (!p_state.refl_valid) {
		memcpy(p_state.refl_delay, target_delay, sizeof(target_delay));
		memcpy(p_state.refl_gain, target_gain, sizeof(target_gain));
		p_state.refl_valid = true;
	}
	const uint32_t mask = REFL_LEN - 1;
	float *ring = p_state.refl.ptr();
	const float inv = 1.0f / p_frames;
	for (int w = 0; w < REFLECTIONS; w++) {
		for (int e = 0; e < 2; e++) {
			const float d0 = p_state.refl_delay[w][e];
			const float g0 = p_state.refl_gain[w][e];
			const float dd = (target_delay[w][e] - d0) * inv;
			const float dg = (target_gain[w][e] - g0) * inv;
			if (g0 == 0.0f && dg == 0.0f) {
				continue;
			}
			float *out = e == 0 ? refl_l : refl_r;
			for (int i = 0; i < p_frames; i++) {
				const float d = d0 + dd * (i + 1);
				const float pos = (float)(p_state.refl_pos + i) - d;
				const float fl = Math::floor(pos);
				const float t = pos - fl;
				const uint32_t at = (uint32_t)(int64_t)fl;
				const float v = ring[at & mask] + t * (ring[(at + 1) & mask] - ring[at & mask]);
				out[i] += (g0 + dg * (i + 1)) * v;
			}
			p_state.refl_delay[w][e] = target_delay[w][e];
			p_state.refl_gain[w][e] = target_gain[w][e];
		}
	}
}

void BinauralRenderer::_reverb(int p_frames) {
	const uint32_t mask = FDN_LEN - 1;
	static const float sign_l[FDN_LINES] = { 1, -1, 1, -1, 1, -1, 1, -1 };
	static const float sign_r[FDN_LINES] = { 1, 1, -1, -1, 1, 1, -1, -1 };
	static const float sign_in[FDN_LINES] = { 1, 1, 1, 1, -1, -1, -1, -1 };
	const float in_scale = 1.0f / Math::sqrt((float)FDN_LINES);
	float *lines = fdn.ptr();
	float *pre = predelay.ptr();
	for (int i = 0; i < p_frames; i++) {
		const float in = pre[(predelay_pos - predelay_len) & mask] * in_scale;
		pre[predelay_pos & mask] = send[i];
		predelay_pos++;
		float o[FDN_LINES];
		float sum = 0.0f;
		for (int j = 0; j < FDN_LINES; j++) {
			const float v = lines[j * FDN_LEN + ((fdn_pos - fdn_len[j]) & mask)];
			fdn_lp[j] = v + fdn_damp[j] * (fdn_lp[j] - v);
			o[j] = fdn_lp[j] * fdn_gain[j];
			sum += o[j];
		}
		const float m = sum * (2.0f / FDN_LINES);
		float l = 0.0f;
		float r = 0.0f;
		for (int j = 0; j < FDN_LINES; j++) {
			lines[j * FDN_LEN + (fdn_pos & mask)] = o[j] - m + sign_in[j] * in;
			l += sign_l[j] * o[j];
			r += sign_r[j] * o[j];
		}
		fdn_pos++;
		out_l[i] += l * in_scale * fdn_norm;
		out_r[i] += r * in_scale * fdn_norm;
	}
}

void BinauralRenderer::_render_sub(const float *p_in, int p_stride, int p_channels, int p_frames, const Transform3D &p_pose, float *r_l, float *r_r) {
	const int n = p_frames;
	for (int i = 0; i < n; i++) {
		out_l[i] = out_r[i] = refl_l[i] = refl_r[i] = send[i] = 0.0f;
	}
	const BinauralHrtfData *hrtf = config.hrtf.ptr();
	const bool room = config.room.enabled && config.room.wet > 0.0f;
	for (uint32_t s = 0; s < states.size(); s++) {
		SourceState &st = *states[s];
		const SourceConfig &c = st.config;
		float *x = in_block[s];
		if (c.ambisonic >= 0) {
			const float *row = decode.ptr() + (uint64_t)c.ambisonic * decode_channels;
			const int count = MIN(decode_channels, p_channels - config.ambisonic_input);
			for (int i = 0; i < n; i++) {
				const float *frame = p_in + (uint64_t)i * p_stride + config.ambisonic_input;
				float v = 0.0f;
				for (int k = 0; k < count; k++) {
					v += row[k] * frame[k];
				}
				x[i] = v;
			}
		} else if (c.input >= 0 && c.input < p_channels) {
			for (int i = 0; i < n; i++) {
				x[i] = p_in[(uint64_t)i * p_stride + c.input] * c.gain;
			}
		} else {
			for (int i = 0; i < n; i++) {
				x[i] = 0.0f;
			}
		}
		if (c.kind == KIND_LFE || c.kind == KIND_HEAD_LEFT || c.kind == KIND_HEAD_RIGHT) {
			for (int i = 0; i < n; i++) {
				if (c.kind != KIND_HEAD_RIGHT) {
					out_l[i] += x[i];
				}
				if (c.kind != KIND_HEAD_LEFT) {
					out_r[i] += x[i];
				}
			}
			continue;
		}
		// The direction from the head, in the head's axes.
		Vector3 local = p_pose.basis.xform_inv(c.at_infinity ? c.position : c.position - p_pose.origin);
		if (local.length_squared() < 1e-8f) {
			local = Vector3(0, 0, -1);
		}
		local.normalize();
		const bool changed = !st.have_filter || local.distance_squared_to(st.direction) > 1e-7f;
		float target[2] = { st.delay[0], st.delay[1] };
		if (changed) {
			const float azimuth = Math::rad_to_deg(Math::atan2(local.x, -local.z));
			const float elevation = Math::rad_to_deg(Math::asin(CLAMP(local.y, -1.0f, 1.0f)));
			hrtf->lookup(azimuth, elevation, filter_new[0], filter_new[1], target[0], target[1]);
			const float mid = 0.5f * (target[0] + target[1]);
			for (int e = 0; e < 2; e++) {
				target[e] = CLAMP(mid + (target[e] - mid) * config.itd_scale, 0.0f, MAX_RUNTIME_DELAY);
			}
			if (!st.have_filter) {
				st.delay[0] = target[0];
				st.delay[1] = target[1];
			}
		}
		memcpy(st.hist + HIST, x, sizeof(float) * n);
		const float inv = 1.0f / n;
		for (int e = 0; e < 2; e++) {
			float *xd = st.xd[e] + TAPS - 1;
			const float d0 = st.delay[e];
			const float dd = (target[e] - d0) * inv;
			for (int i = 0; i < n; i++) {
				const float pos = (float)(HIST + i) - (d0 + dd * (i + 1) + BASE_DELAY);
				const int ip = (int)pos;
				xd[i] = cubic(st.hist + ip - 1, pos - ip);
			}
			float *out = e == 0 ? out_l : out_r;
			if (changed && st.have_filter) {
				convolve(xd, st.h[e], y_old, n);
				convolve(xd, filter_new[e], y_new, n);
				for (int i = 0; i < n; i++) {
					const float t = (i + 1) * inv;
					out[i] += y_old[i] + t * (y_new[i] - y_old[i]);
				}
			} else {
				convolve(xd, changed ? filter_new[e] : st.h[e], y_old, n);
				for (int i = 0; i < n; i++) {
					out[i] += y_old[i];
				}
			}
			st.delay[e] = target[e];
			memmove(st.xd[e], st.xd[e] + n, sizeof(float) * (TAPS - 1));
		}
		if (changed) {
			memcpy(st.h, filter_new, sizeof(filter_new));
			st.direction = local;
			st.have_filter = true;
		}
		memmove(st.hist, st.hist + n, sizeof(float) * HIST);
		if (room) {
			if (c.reflections && !c.at_infinity) {
				float *ring = st.refl.ptr();
				for (int i = 0; i < n; i++) {
					ring[(st.refl_pos + i) & (REFL_LEN - 1)] = x[i];
				}
				_reflections(st, p_pose, n);
				st.refl_pos += n;
			}
			for (int i = 0; i < n; i++) {
				send[i] += x[i];
			}
		}
	}
	if (room) {
		// Walls take the highs off: one pole at about 5 kHz.
		const float a = 0.52f;
		for (int i = 0; i < n; i++) {
			refl_lp[0] = refl_l[i] + a * (refl_lp[0] - refl_l[i]);
			refl_lp[1] = refl_r[i] + a * (refl_lp[1] - refl_r[i]);
			out_l[i] += refl_lp[0];
			out_r[i] += refl_lp[1];
		}
		_reverb(n);
	}
	for (int f = 0; f < eq_count; f++) {
		Biquad &b = eq_filters[f];
		for (int e = 0; e < 2; e++) {
			float *v = e == 0 ? out_l : out_r;
			float z1 = b.z1[e];
			float z2 = b.z2[e];
			for (int i = 0; i < n; i++) {
				const float in = v[i];
				const float out = b.b0 * in + z1;
				z1 = b.b1 * in - b.a1 * out + z2;
				z2 = b.b2 * in - b.a2 * out;
				v[i] = out;
			}
			b.z1[e] = z1;
			b.z2[e] = z2;
		}
	}
	const float g = config.gain;
	for (int i = 0; i < n; i++) {
		r_l[i] = out_l[i] * g;
		r_r[i] = out_r[i] * g;
	}
}

Transform3D BinauralRenderer::_toward(const Transform3D &p_from, const Transform3D &p_to, float p_weight) {
	return p_from.is_equal_approx(p_to) ? p_to : p_from.interpolate_with(p_to, p_weight);
}

void BinauralRenderer::process(const float *p_in, int p_stride, int p_channels, int p_frames, AudioFrame *r_out) {
	if (p_frames <= 0) {
		return;
	}
	const uint64_t start = OS::get_singleton()->get_ticks_usec();
	Transform3D target;
	uint64_t target_usec = 0;
	uint64_t target_serial = 0;
	bool changed = false;
	bool reset_now = false;
	{
		MutexLock lock(mutex);
		if (pending_serial != config_serial) {
			config = pending;
			config_serial = pending_serial;
			changed = true;
		}
		target = pending_pose;
		target_usec = pending_pose_usec;
		target_serial = pending_pose_serial;
		reset_now = reset_requested;
		reset_requested = false;
	}
	if (changed) {
		_apply_config();
	}
	if (reset_now) {
		_reset_state();
	}
	if (config.hrtf.is_null() || !config.hrtf->is_valid()) {
		memset((void *)r_out, 0, sizeof(AudioFrame) * p_frames);
		return;
	}
	if (!have_pose) {
		pose = target;
		last_pose = target;
		last_pose_usec = target_usec;
		pose_serial = target_serial;
		turn_rate = 0.0f;
		move_rate = Vector3();
		since_pose = 0.0f;
		have_pose = true;
	} else if (target_serial != pose_serial) {
		// A new pose: the head's motion since the one before.
		const float dt = (target_usec - last_pose_usec) * 1e-6f;
		turn_rate = 0.0f;
		move_rate = Vector3();
		if (dt > 0.002f && dt < 0.1f) {
			Quaternion q = (target.basis.get_rotation_quaternion() * last_pose.basis.get_rotation_quaternion().inverse()).normalized();
			if (q.w < 0.0f) {
				q = -q;
			}
			const float angle = q.get_angle();
			if (angle > 1e-5f) {
				turn_axis = q.get_axis().normalized();
				turn_rate = angle / dt;
			}
			move_rate = (target.origin - last_pose.origin) / dt;
		}
		last_pose = target;
		last_pose_usec = target_usec;
		pose_serial = target_serial;
		since_pose = 0.0f;
	}
	float l[SUB];
	float r[SUB];
	for (int done = 0; done < p_frames;) {
		const int n = MIN(SUB, p_frames - done);
		// Poses come once a game frame, and the audio callbacks in bursts: one can get no new pose or several. Followed
		// directly, the interaural delay would move in jerks, heard as a pulsing pitch when the head turns. So the head
		// is carried on at its last speed between poses, and a one-pole stage takes out the steps when a pose comes.
		since_pose += (float)n / config.rate;
		const float ahead = MIN(since_pose, POSE_AHEAD);
		Transform3D predicted = last_pose;
		if (turn_rate > 0.0f) {
			predicted.basis = Basis(Quaternion(turn_axis, turn_rate * ahead)) * last_pose.basis;
		}
		predicted.origin += move_rate * ahead;
		pose = _toward(pose, predicted, 1.0f - Math::exp(-(float)n / (config.rate * POSE_TAU)));
		_render_sub(p_in + (uint64_t)done * p_stride, p_stride, p_channels, n, pose, l, r);
		for (int i = 0; i < n; i++) {
			r_out[done + i] = AudioFrame(l[i], r[i]);
		}
		done += n;
	}
	const uint64_t spent = OS::get_singleton()->get_ticks_usec() - start;
	MutexLock lock(mutex);
	stats_frames += p_frames;
	stats_usec += spent;
	stats_calls++;
	if (spent > stats_max_usec) {
		stats_max_usec = spent;
		stats_max_frames = p_frames;
	}
}

PackedFloat32Array BinauralRenderer::render(const PackedFloat32Array &p_input, int p_channels) {
	PackedFloat32Array out;
	ERR_FAIL_COND_V(p_channels <= 0, out);
	const int frames = p_input.size() / p_channels;
	out.resize(frames * 2);
	AudioFrame buffer[512];
	const float *in = p_input.ptr();
	float *o = out.ptrw();
	for (int done = 0; done < frames;) {
		const int n = MIN(512, frames - done);
		process(in + (uint64_t)done * p_channels, p_channels, p_channels, n, buffer);
		for (int i = 0; i < n; i++) {
			o[(done + i) * 2] = buffer[i].left;
			o[(done + i) * 2 + 1] = buffer[i].right;
		}
		done += n;
	}
	return out;
}

Dictionary BinauralRenderer::lookup_hrir(float p_azimuth, float p_elevation) const {
	Ref<BinauralHrtfData> data;
	float itd = 1.0f;
	{
		MutexLock lock(mutex);
		data = pending.hrtf;
		itd = pending.itd_scale;
	}
	Dictionary d;
	ERR_FAIL_COND_V(data.is_null() || !data->is_valid(), d);
	PackedFloat32Array left;
	PackedFloat32Array right;
	left.resize(TAPS);
	right.resize(TAPS);
	float dl = 0.0f;
	float dr = 0.0f;
	data->lookup(p_azimuth, p_elevation, left.ptrw(), right.ptrw(), dl, dr);
	const float mid = 0.5f * (dl + dr);
	d["left"] = left;
	d["right"] = right;
	d["delay_left"] = mid + (dl - mid) * itd;
	d["delay_right"] = mid + (dr - mid) * itd;
	return d;
}

PackedFloat32Array BinauralRenderer::ambisonic_encode(const Vector3 &p_direction, int p_order) {
	PackedFloat32Array out;
	const int order = CLAMP(p_order, 0, MAX_ORDER);
	out.resize((order + 1) * (order + 1));
	const Vector3 v = godot_to_ambix(p_direction.normalized());
	float sh[16];
	sh_sn3d(order, v.x, v.y, v.z, sh);
	for (int i = 0; i < out.size(); i++) {
		out.set(i, sh[i]);
	}
	return out;
}

PackedVector3Array BinauralRenderer::virtual_speakers(int p_order) {
	LocalVector<Vector3> dirs;
	LocalVector<float> weights;
	speaker_grid(CLAMP(p_order, 1, MAX_ORDER), dirs, weights);
	PackedVector3Array out;
	for (const Vector3 &d : dirs) {
		out.push_back(ambix_to_godot(d));
	}
	return out;
}

Dictionary BinauralRenderer::get_stats() const {
	MutexLock lock(mutex);
	Dictionary d;
	d["frames"] = (int64_t)stats_frames;
	d["calls"] = (int64_t)stats_calls;
	d["usec"] = (int64_t)stats_usec;
	// CPU time per second of audio, as a share of one core.
	d["load"] = stats_frames > 0 ? (double)stats_usec / 1e6 / ((double)stats_frames / pending.rate) : 0.0;
	d["max_usec"] = (int64_t)stats_max_usec;
	d["max_frames"] = (int64_t)stats_max_frames;
	d["sources"] = (int64_t)(pending.sources.size() + (pending.ambisonic_order > 0 ? virtual_speakers(MIN(pending.ambisonic_order, MAX_ORDER)).size() : 0));
	d["latency_frames"] = 0;
	return d;
}

void BinauralRenderer::reset_stats() {
	MutexLock lock(mutex);
	stats_frames = 0;
	stats_usec = 0;
	stats_calls = 0;
	stats_max_usec = 0;
	stats_max_frames = 0;
}

void BinauralRenderer::_bind_methods() {
	ClassDB::bind_method(D_METHOD("is_ready"), &BinauralRenderer::is_ready);
	ClassDB::bind_method(D_METHOD("load_hrtf", "path"), &BinauralRenderer::load_hrtf);
	ClassDB::bind_method(D_METHOD("save_hrtf", "path", "name", "credit"), &BinauralRenderer::save_hrtf, DEFVAL(""), DEFVAL(""));
	ClassDB::bind_method(D_METHOD("get_hrtf_info"), &BinauralRenderer::get_hrtf_info);
	ClassDB::bind_method(D_METHOD("get_last_error"), &BinauralRenderer::get_last_error);
	ClassDB::bind_method(D_METHOD("set_sources", "sources"), &BinauralRenderer::set_sources);
	ClassDB::bind_method(D_METHOD("set_ambisonics", "order", "first_input", "frame", "gain"), &BinauralRenderer::set_ambisonics, DEFVAL(Basis()), DEFVAL(1.0));
	ClassDB::bind_method(D_METHOD("set_listener", "pose"), &BinauralRenderer::set_listener);
	ClassDB::bind_method(D_METHOD("set_room", "room"), &BinauralRenderer::set_room);
	ClassDB::bind_method(D_METHOD("set_eq", "bands"), &BinauralRenderer::set_eq);
	ClassDB::bind_method(D_METHOD("set_tuning", "itd_scale", "pinna_warp", "concha_db"), &BinauralRenderer::set_tuning);
	ClassDB::bind_method(D_METHOD("get_tuning"), &BinauralRenderer::get_tuning);
	ClassDB::bind_method(D_METHOD("set_gain", "gain"), &BinauralRenderer::set_gain);
	ClassDB::bind_method(D_METHOD("set_sample_rate", "rate"), &BinauralRenderer::set_sample_rate);
	ClassDB::bind_method(D_METHOD("get_sample_rate"), &BinauralRenderer::get_sample_rate);
	ClassDB::bind_method(D_METHOD("reset"), &BinauralRenderer::reset);
	ClassDB::bind_method(D_METHOD("render", "input", "channels"), &BinauralRenderer::render);
	ClassDB::bind_method(D_METHOD("lookup_hrir", "azimuth", "elevation"), &BinauralRenderer::lookup_hrir);
	ClassDB::bind_static_method("BinauralRenderer", D_METHOD("ambisonic_encode", "direction", "order"), &BinauralRenderer::ambisonic_encode);
	ClassDB::bind_static_method("BinauralRenderer", D_METHOD("virtual_speakers", "order"), &BinauralRenderer::virtual_speakers);
	ClassDB::bind_method(D_METHOD("get_stats"), &BinauralRenderer::get_stats);
	ClassDB::bind_method(D_METHOD("reset_stats"), &BinauralRenderer::reset_stats);
	BIND_ENUM_CONSTANT(KIND_POINT);
	BIND_ENUM_CONSTANT(KIND_LFE);
	BIND_ENUM_CONSTANT(KIND_HEAD_LEFT);
	BIND_ENUM_CONSTANT(KIND_HEAD_RIGHT);
}
