/**************************************************************************/
/*  binaural_hrtf.cpp                                                     */
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

#include "binaural_hrtf.h"

#include "core/io/file_access.h"
#include "core/math/math_funcs.h"

#include "thirdparty/libmysofa/src/hrtf/mysofa.h"

#include <complex>

namespace {

constexpr int PACK_VERSION = 1;
constexpr int FADE = 16; // Half-Hann fade at the end of each aligned response.
constexpr int EQ_FFT = 512;
constexpr int EQ_TAPS = 65; // Linear-phase diffuse-field equaliser, delay (EQ_TAPS - 1) / 2.

void fft(std::complex<double> *p_data, int p_size, bool p_inverse) {
	for (int i = 1, j = 0; i < p_size; i++) {
		int bit = p_size >> 1;
		for (; j & bit; bit >>= 1) {
			j ^= bit;
		}
		j ^= bit;
		if (i < j) {
			std::swap(p_data[i], p_data[j]);
		}
	}
	for (int len = 2; len <= p_size; len <<= 1) {
		const double angle = 2.0 * Math::PI / len * (p_inverse ? 1.0 : -1.0);
		const std::complex<double> step(Math::cos(angle), Math::sin(angle));
		for (int i = 0; i < p_size; i += len) {
			std::complex<double> w(1.0, 0.0);
			for (int k = 0; k < len / 2; k++) {
				const std::complex<double> u = p_data[i + k];
				const std::complex<double> v = p_data[i + k + len / 2] * w;
				p_data[i + k] = u + v;
				p_data[i + k + len / 2] = u - v;
				w *= step;
			}
		}
	}
	if (p_inverse) {
		for (int i = 0; i < p_size; i++) {
			p_data[i] /= (double)p_size;
		}
	}
}

// Catmull-Rom between p[1] and p[2].
inline float cubic(const float *p, float t) {
	return p[1] + 0.5f * t * (p[2] - p[0] + t * (2.0f * p[0] - 5.0f * p[1] + 4.0f * p[2] - p[3] + t * (3.0f * (p[1] - p[2]) + p[3] - p[0])));
}

// Sample p_t of p_data (p_size long, zero outside), Catmull-Rom.
float sample_at(const float *p_data, int p_size, double p_t) {
	const int i = (int)Math::floor(p_t);
	const float t = (float)(p_t - i);
	float p[4];
	for (int k = 0; k < 4; k++) {
		const int at = i - 1 + k;
		p[k] = (at >= 0 && at < p_size) ? p_data[at] : 0.0f;
	}
	return cubic(p, t);
}

String attribute(MYSOFA_HRTF *p_hrtf, const char *p_name) {
	const char *value = mysofa_getAttribute(p_hrtf->attributes, (char *)p_name);
	return value ? String::utf8(value).strip_edges() : String();
}

} // namespace

Vector3 BinauralHrtfData::grid_direction(int p_el, int p_az) {
	const float el = Math::deg_to_rad(EL_MIN + EL_STEP * p_el);
	const float az = Math::deg_to_rad(AZ_STEP * p_az);
	return Vector3(Math::sin(az) * Math::cos(el), Math::sin(el), -Math::cos(az) * Math::cos(el));
}

void BinauralHrtfData::lookup(float p_azimuth, float p_elevation, float *r_left, float *r_right, float &r_delay_left, float &r_delay_right) const {
	const float fe = (CLAMP(p_elevation, EL_MIN, 90.0f) - EL_MIN) / EL_STEP;
	const int e0 = MIN((int)fe, EL - 1);
	const int e1 = MIN(e0 + 1, EL - 1);
	const float te = fe - e0;
	float fa = Math::fposmod(p_azimuth, 360.0f) / AZ_STEP;
	const int a0 = MIN((int)fa, AZ - 1);
	const int a1 = (a0 + 1) % AZ;
	const float ta = fa - a0;
	const int points[4] = { index(e0, a0), index(e0, a1), index(e1, a0), index(e1, a1) };
	const float weights[4] = { (1.0f - te) * (1.0f - ta), (1.0f - te) * ta, te * (1.0f - ta), te * ta };
	for (int n = 0; n < TAPS; n++) {
		r_left[n] = 0.0f;
		r_right[n] = 0.0f;
	}
	r_delay_left = 0.0f;
	r_delay_right = 0.0f;
	for (int p = 0; p < 4; p++) {
		const float w = weights[p];
		if (w <= 0.0f) {
			continue;
		}
		const float *l = ir.ptr() + ((uint64_t)points[p] * 2) * TAPS;
		const float *r = l + TAPS;
		for (int n = 0; n < TAPS; n++) {
			r_left[n] += w * l[n];
			r_right[n] += w * r[n];
		}
		r_delay_left += w * delay[points[p] * 2];
		r_delay_right += w * delay[points[p] * 2 + 1];
	}
}

Error BinauralHrtfData::load(const String &p_path, String &r_error) {
	if (p_path.get_extension().to_lower() == "sofa") {
		return load_sofa(p_path, r_error);
	}
	return load_pack(p_path, r_error);
}

Error BinauralHrtfData::load_pack(const String &p_path, String &r_error) {
	Error err = OK;
	Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::READ, &err);
	if (f.is_null()) {
		r_error = "can't open " + p_path;
		return err != OK ? err : ERR_CANT_OPEN;
	}
	uint8_t magic[4] = {};
	f->get_buffer(magic, 4);
	if (memcmp(magic, "FHRT", 4) != 0 || f->get_32() != PACK_VERSION) {
		r_error = "not an HRTF pack (version 1)";
		return ERR_FILE_UNRECOGNIZED;
	}
	const uint32_t pack_rate = f->get_32();
	const uint32_t taps = f->get_32();
	const uint32_t az = f->get_32();
	const uint32_t el = f->get_32();
	const float el_min = f->get_float();
	const float el_step = f->get_float();
	const float az_step = f->get_float();
	const float scale = f->get_float();
	if (pack_rate != RATE || taps != TAPS || az != AZ || el != EL || el_min != EL_MIN || el_step != EL_STEP || az_step != AZ_STEP) {
		r_error = vformat("a pack for another grid (%d Hz, %d taps, %d x %d)", pack_rate, taps, az, el);
		return ERR_FILE_UNRECOGNIZED;
	}
	const uint32_t name_len = f->get_32();
	if (name_len > 4096) {
		r_error = "corrupt pack";
		return ERR_FILE_CORRUPT;
	}
	Vector<uint8_t> text;
	text.resize(name_len);
	f->get_buffer(text.ptrw(), name_len);
	name = String::utf8((const char *)text.ptr(), name_len);
	const uint32_t credit_len = f->get_32();
	if (credit_len > 65536) {
		r_error = "corrupt pack";
		return ERR_FILE_CORRUPT;
	}
	text.resize(credit_len);
	f->get_buffer(text.ptrw(), credit_len);
	credit = String::utf8((const char *)text.ptr(), credit_len);
	delay.resize(POINTS * 2);
	for (uint32_t i = 0; i < delay.size(); i++) {
		delay[i] = f->get_float();
	}
	ir.resize(POINTS * 2 * TAPS);
	Vector<uint8_t> raw;
	raw.resize(ir.size() * 2);
	if (f->get_buffer(raw.ptrw(), raw.size()) != (uint64_t)raw.size()) {
		ir.clear();
		r_error = "truncated pack";
		return ERR_FILE_CORRUPT;
	}
	const uint8_t *p = raw.ptr();
	for (uint32_t i = 0; i < ir.size(); i++) {
		const int16_t v = (int16_t)(p[i * 2] | (p[i * 2 + 1] << 8));
		ir[i] = v * scale / 32767.0f;
	}
	source = p_path;
	measured = 0;
	warp = 1.0f;
	return OK;
}

Error BinauralHrtfData::save_pack(const String &p_path) const {
	ERR_FAIL_COND_V(!is_valid(), ERR_UNCONFIGURED);
	Error err = OK;
	Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::WRITE, &err);
	if (f.is_null()) {
		return err != OK ? err : ERR_CANT_CREATE;
	}
	float peak = 0.0f;
	for (uint32_t i = 0; i < ir.size(); i++) {
		peak = MAX(peak, Math::abs(ir[i]));
	}
	const float scale = peak > 0.0f ? peak : 1.0f;
	f->store_buffer((const uint8_t *)"FHRT", 4);
	f->store_32(PACK_VERSION);
	f->store_32(RATE);
	f->store_32(TAPS);
	f->store_32(AZ);
	f->store_32(EL);
	f->store_float(EL_MIN);
	f->store_float(EL_STEP);
	f->store_float(AZ_STEP);
	f->store_float(scale);
	const CharString n = name.utf8();
	f->store_32(n.length());
	f->store_buffer((const uint8_t *)n.get_data(), n.length());
	const CharString c = credit.utf8();
	f->store_32(c.length());
	f->store_buffer((const uint8_t *)c.get_data(), c.length());
	for (uint32_t i = 0; i < delay.size(); i++) {
		f->store_float(delay[i]);
	}
	Vector<uint8_t> raw;
	raw.resize(ir.size() * 2);
	uint8_t *p = raw.ptrw();
	for (uint32_t i = 0; i < ir.size(); i++) {
		const int16_t v = (int16_t)CLAMP(Math::round(ir[i] / scale * 32767.0f), -32767.0f, 32767.0f);
		p[i * 2] = (uint8_t)(v & 0xff);
		p[i * 2 + 1] = (uint8_t)((uint16_t)v >> 8);
	}
	f->store_buffer(raw.ptr(), raw.size());
	return OK;
}

Error BinauralHrtfData::load_sofa(const String &p_path, String &r_error) {
	Error ferr = OK;
	const Vector<uint8_t> bytes = FileAccess::get_file_as_bytes(p_path, &ferr);
	if (bytes.is_empty()) {
		r_error = "can't read " + p_path;
		return ferr != OK ? ferr : ERR_CANT_OPEN;
	}
	int err = 0;
	MYSOFA_HRTF *h = mysofa_load_data((const char *)bytes.ptr(), bytes.size(), &err);
	if (h == nullptr) {
		r_error = vformat("not a readable SOFA file (libmysofa error %d)", err);
		return ERR_FILE_CORRUPT;
	}
	err = mysofa_check(h);
	if (err != MYSOFA_OK) {
		mysofa_free(h);
		r_error = vformat("not a SimpleFreeFieldHRIR SOFA file (libmysofa error %d)", err);
		return ERR_FILE_UNRECOGNIZED;
	}
	if (h->R != 2 || h->N < 8 || h->M < 6) {
		mysofa_free(h);
		r_error = "a SOFA file without two ears or with too few directions";
		return ERR_FILE_UNRECOGNIZED;
	}
	if (Math::abs(h->DataSamplingRate.values[0] - (float)RATE) > 0.5f) {
		err = mysofa_resample(h, (float)RATE);
		if (err != MYSOFA_OK) {
			mysofa_free(h);
			r_error = vformat("can't resample to 48 kHz (libmysofa error %d)", err);
			return ERR_CANT_CREATE;
		}
	}
	mysofa_tocartesian(h);
	const int M = (int)h->M;
	const int N = (int)h->N;
	const float *data = h->DataIR.values;

	// Measured directions in Godot axes (SOFA: +X front, +Y left, +Z up), relative to the listener.
	LocalVector<Vector3> dirs;
	dirs.resize(M);
	Vector3 listener;
	if (h->ListenerPosition.elements >= 3) {
		listener = Vector3(h->ListenerPosition.values[0], h->ListenerPosition.values[1], h->ListenerPosition.values[2]);
	}
	for (int m = 0; m < M; m++) {
		const float *s = h->SourcePosition.values + m * 3;
		const Vector3 v = Vector3(s[0], s[1], s[2]) - listener;
		dirs[m] = Vector3(-v.y, v.z, -v.x).normalized();
	}

	// Diffuse-field equaliser: the inverse of the average power spectrum (all directions, both ears), smoothed over a
	// third of an octave, held below 100 Hz and above 16 kHz, limited to -18 / +12 dB of its value at 1 kHz.
	LocalVector<double> power;
	power.resize(EQ_FFT / 2 + 1);
	for (double &v : power) {
		v = 0.0;
	}
	LocalVector<std::complex<double>> buf;
	buf.resize(EQ_FFT);
	for (int m = 0; m < M; m++) {
		for (int r = 0; r < 2; r++) {
			const float *x = data + ((uint64_t)m * 2 + r) * N;
			for (int i = 0; i < EQ_FFT; i++) {
				buf[i] = std::complex<double>(i < N ? x[i] : 0.0, 0.0);
			}
			fft(buf.ptr(), EQ_FFT, false);
			for (int k = 0; k <= EQ_FFT / 2; k++) {
				power[k] += std::norm(buf[k]);
			}
		}
	}
	const double bin_hz = (double)RATE / EQ_FFT;
	LocalVector<double> gain;
	gain.resize(EQ_FFT / 2 + 1);
	for (int k = 0; k <= EQ_FFT / 2; k++) {
		const double f = CLAMP(k * bin_hz, 100.0, 16000.0);
		const int lo = MAX(0, (int)Math::floor(f / 1.122 / bin_hz));
		const int hi = MIN(EQ_FFT / 2, (int)Math::ceil(f * 1.122 / bin_hz));
		double sum = 0.0;
		for (int j = lo; j <= hi; j++) {
			sum += power[j];
		}
		const double avg = sum / (hi - lo + 1) / (2.0 * M);
		gain[k] = avg > 1e-20 ? 1.0 / Math::sqrt(avg) : 1.0;
	}
	const double reference = gain[(int)Math::round(1000.0 / bin_hz)];
	for (double &g : gain) {
		g = CLAMP(g, reference * 0.125, reference * 4.0);
	}
	float eq[EQ_TAPS];
	const int centre = (EQ_TAPS - 1) / 2;
	for (int n = 0; n < EQ_TAPS; n++) {
		double sum = 0.0;
		for (int k = 0; k < EQ_FFT; k++) {
			const int kk = k <= EQ_FFT / 2 ? k : EQ_FFT - k;
			sum += gain[kk] * Math::cos(2.0 * Math::PI * k * (n - centre) / EQ_FFT);
		}
		const double window = 0.5 - 0.5 * Math::cos(2.0 * Math::PI * (n + 1) / (EQ_TAPS + 1));
		eq[n] = (float)(sum / EQ_FFT * window);
	}

	// Equalise, find each onset (the first sample within 14 dB of the peak, one sample back), keep TAPS from PRE
	// before it.
	const int length = N + EQ_TAPS - 1;
	LocalVector<float> aligned;
	aligned.resize((uint64_t)M * 2 * TAPS);
	LocalVector<float> onset;
	onset.resize(M * 2);
	LocalVector<float> work;
	work.resize(length);
	for (int m = 0; m < M; m++) {
		for (int r = 0; r < 2; r++) {
			const float *x = data + ((uint64_t)m * 2 + r) * N;
			float peak = 0.0f;
			for (int n = 0; n < length; n++) {
				float s = 0.0f;
				for (int k = MAX(0, n - N + 1); k <= MIN(n, EQ_TAPS - 1); k++) {
					s += eq[k] * x[n - k];
				}
				work[n] = s;
				peak = MAX(peak, Math::abs(s));
			}
			int first = 0;
			while (first < length && Math::abs(work[first]) < 0.2f * peak) {
				first++;
			}
			first = MAX(0, first - 1);
			float extra = 0.0f; // SOFA delays (Data.Delay) beside the responses.
			if (h->DataDelay.elements == (unsigned)M * 2) {
				extra = h->DataDelay.values[m * 2 + r];
			} else if (h->DataDelay.elements == 2) {
				extra = h->DataDelay.values[r];
			}
			onset[m * 2 + r] = (float)first + extra;
			float *out = aligned.ptr() + ((uint64_t)m * 2 + r) * TAPS;
			for (int n = 0; n < TAPS; n++) {
				const int at = first - PRE + n;
				float v = (at >= 0 && at < length) ? work[at] : 0.0f;
				if (n >= TAPS - FADE) {
					v *= 0.5f + 0.5f * Math::cos(Math::PI * (n - (TAPS - FADE) + 1) / (FADE + 1));
				}
				out[n] = v;
			}
		}
	}
	const String title = attribute(h, "Title");
	const String database = attribute(h, "DatabaseName");
	const String subject = attribute(h, "ListenerShortName");
	const String licence = attribute(h, "License");
	const String organisation = attribute(h, "Organization");
	const String authors = attribute(h, "AuthorContact");
	mysofa_free(h);

	// The grid: the nearest measured direction if within a degree, else inverse-square-distance weights of the three
	// nearest.
	ir.resize(POINTS * 2 * TAPS);
	delay.resize(POINTS * 2);
	for (int e = 0; e < EL; e++) {
		for (int a = 0; a < AZ; a++) {
			const Vector3 g = grid_direction(e, a);
			int best[3] = { -1, -1, -1 };
			float best_dot[3] = { -2.0f, -2.0f, -2.0f };
			for (int m = 0; m < M; m++) {
				const float d = g.dot(dirs[m]);
				for (int k = 0; k < 3; k++) {
					if (d > best_dot[k]) {
						for (int j = 2; j > k; j--) {
							best[j] = best[j - 1];
							best_dot[j] = best_dot[j - 1];
						}
						best[k] = m;
						best_dot[k] = d;
						break;
					}
				}
			}
			float weights[3] = {};
			const float angle0 = Math::acos(CLAMP(best_dot[0], -1.0f, 1.0f));
			if (angle0 < Math::deg_to_rad(1.0f)) {
				weights[0] = 1.0f;
			} else {
				float sum = 0.0f;
				for (int k = 0; k < 3; k++) {
					if (best[k] < 0) {
						continue;
					}
					const float angle = Math::acos(CLAMP(best_dot[k], -1.0f, 1.0f));
					weights[k] = 1.0f / (angle * angle);
					sum += weights[k];
				}
				for (float &w : weights) {
					w /= sum;
				}
			}
			const int p = index(e, a);
			for (int r = 0; r < 2; r++) {
				float *out = ir.ptr() + ((uint64_t)p * 2 + r) * TAPS;
				float d = 0.0f;
				for (int n = 0; n < TAPS; n++) {
					out[n] = 0.0f;
				}
				for (int k = 0; k < 3; k++) {
					if (weights[k] <= 0.0f) {
						continue;
					}
					const float *in = aligned.ptr() + ((uint64_t)best[k] * 2 + r) * TAPS;
					for (int n = 0; n < TAPS; n++) {
						out[n] += weights[k] * in[n];
					}
					d += weights[k] * onset[best[k] * 2 + r];
				}
				delay[p * 2 + r] = d;
			}
		}
	}
	float smallest = 1e9f;
	for (float d : delay) {
		smallest = MIN(smallest, d);
	}
	for (float &d : delay) {
		d = CLAMP(d - smallest, 0.0f, MAX_DELAY);
	}
	// Unit energy over the sphere (each grid point weighted by the area it stands for), both ears together.
	double energy = 0.0;
	double area = 0.0;
	for (int e = 0; e < EL; e++) {
		const double w = Math::cos(Math::deg_to_rad(EL_MIN + EL_STEP * e)) + 0.02;
		for (int a = 0; a < AZ; a++) {
			const float *x = ir.ptr() + (uint64_t)index(e, a) * 2 * TAPS;
			double s = 0.0;
			for (int n = 0; n < TAPS * 2; n++) {
				s += (double)x[n] * x[n];
			}
			energy += w * s * 0.5;
			area += w;
		}
	}
	const float norm = energy > 0.0 ? (float)(1.0 / Math::sqrt(energy / area)) : 1.0f;
	for (float &v : ir) {
		v *= norm;
	}

	name = !title.is_empty() ? title : p_path.get_file().get_basename();
	if (!database.is_empty() && !subject.is_empty() && !name.contains(subject)) {
		name = database + " " + subject;
	}
	PackedStringArray parts;
	for (const String &s : { title, database, organisation, authors, licence.is_empty() ? String() : "Licence: " + licence }) {
		if (!s.is_empty() && !parts.has(s)) {
			parts.push_back(s);
		}
	}
	credit = String("; ").join(parts);
	source = p_path;
	measured = M;
	warp = 1.0f;
	return OK;
}

Ref<BinauralHrtfData> BinauralHrtfData::tuned(float p_warp, float p_rate) const {
	Ref<BinauralHrtfData> out;
	out.instantiate();
	out->name = name;
	out->credit = credit;
	out->source = source;
	out->measured = measured;
	out->delay = delay;
	out->ir.resize(ir.size());
	out->warp = CLAMP(p_warp, 0.8f, 1.25f);
	out->rate = CLAMP(p_rate, 8000.0f, 192000.0f);
	const double step = (double)out->warp * RATE / out->rate; // Input samples per output sample.
	if (!Math::is_equal_approx(out->rate, (float)RATE)) {
		for (float &d : out->delay) {
			d *= out->rate / RATE;
		}
	}
	if (Math::is_equal_approx((float)step, 1.0f)) {
		out->ir = ir;
		return out;
	}
	for (int i = 0; i < POINTS * 2; i++) {
		const float *in = ir.ptr() + (uint64_t)i * TAPS;
		float *o = out->ir.ptr() + (uint64_t)i * TAPS;
		double before = 0.0;
		double after = 0.0;
		for (int n = 0; n < TAPS; n++) {
			before += (double)in[n] * in[n];
			o[n] = sample_at(in, TAPS, PRE + (n - PRE) * step);
			after += (double)o[n] * o[n];
		}
		const float k = after > 1e-20 ? (float)Math::sqrt(before / after) : 1.0f;
		for (int n = 0; n < TAPS; n++) {
			o[n] *= k;
		}
	}
	return out;
}
