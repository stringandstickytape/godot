/**************************************************************************/
/*  annexb_splitter.cpp                                                   */
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

#include "annexb_splitter.h"

namespace {

// Exp-Golomb bit reader over an RBSP (emulation prevention bytes already removed).
struct BitReader {
	const uint8_t *data = nullptr;
	uint64_t size = 0;
	uint64_t bit = 0;
	bool error = false;

	uint32_t read(int p_bits) {
		uint32_t value = 0;
		while (p_bits-- > 0) {
			if (bit >= size * 8) {
				error = true;
				return 0;
			}
			value = (value << 1) | ((data[bit >> 3] >> (7 - (bit & 7))) & 1u);
			bit++;
		}
		return value;
	}

	void skip(uint64_t p_bits) {
		bit += p_bits;
		if (bit > size * 8) {
			error = true;
		}
	}

	uint32_t read_ue() {
		int leading_zeros = 0;
		while (!read(1)) {
			if (error || ++leading_zeros > 31) {
				error = true;
				return 0;
			}
		}
		return leading_zeros ? ((1u << leading_zeros) - 1u + read(leading_zeros)) : 0;
	}

	int32_t read_se() {
		const uint32_t k = read_ue();
		return (k & 1) ? (int32_t)((k + 1) / 2) : -(int32_t)(k / 2);
	}
};

// Copies at most p_capacity bytes of p_src to r_dst without the emulation prevention bytes (00 00 03).
uint64_t unescape(const uint8_t *p_src, uint64_t p_size, uint8_t *r_dst, uint64_t p_capacity) {
	uint64_t out = 0;
	int zeros = 0;
	for (uint64_t i = 0; i < p_size && out < p_capacity; i++) {
		if (zeros >= 2 && p_src[i] == 3) {
			zeros = 0;
			continue;
		}
		r_dst[out++] = p_src[i];
		zeros = p_src[i] == 0 ? zeros + 1 : 0;
	}
	return out;
}

void h264_skip_scaling_list(BitReader &r_reader, int p_size) {
	int last = 8;
	int next = 8;
	for (int j = 0; j < p_size; j++) {
		if (next) {
			next = (last + r_reader.read_se() + 256) % 256;
		}
		last = next ? next : last;
	}
}

} // namespace

void AnnexBSplitter::parse_hevc_sps(const uint8_t *p_nal, uint64_t p_size, SPSInfo &r_sps) {
	uint8_t buffer[512];
	if (p_size < 3) {
		return;
	}
	BitReader b;
	b.data = buffer;
	b.size = unescape(p_nal + 2, p_size - 2, buffer, sizeof(buffer));
	b.skip(4); // sps_video_parameter_set_id
	const uint32_t max_sub_layers = b.read(3);
	b.skip(1); // sps_temporal_id_nesting_flag
	b.skip(88 + 8); // General profile, tier and flags, general_level_idc.
	bool sub_layer_profile[8] = {};
	bool sub_layer_level[8] = {};
	for (uint32_t i = 0; i < max_sub_layers; i++) {
		sub_layer_profile[i] = b.read(1);
		sub_layer_level[i] = b.read(1);
	}
	if (max_sub_layers > 0) {
		for (uint32_t i = max_sub_layers; i < 8; i++) {
			b.skip(2);
		}
	}
	for (uint32_t i = 0; i < max_sub_layers; i++) {
		if (sub_layer_profile[i]) {
			b.skip(88);
		}
		if (sub_layer_level[i]) {
			b.skip(8);
		}
	}
	b.read_ue(); // sps_seq_parameter_set_id
	const uint32_t chroma = b.read_ue();
	if (chroma == 3) {
		b.skip(1); // separate_colour_plane_flag
	}
	const uint32_t width = b.read_ue();
	const uint32_t height = b.read_ue();
	uint32_t crop_left = 0, crop_right = 0, crop_top = 0, crop_bottom = 0;
	if (b.read(1)) {
		crop_left = b.read_ue();
		crop_right = b.read_ue();
		crop_top = b.read_ue();
		crop_bottom = b.read_ue();
	}
	const uint32_t bit_depth = b.read_ue() + 8;
	if (b.error || width == 0 || height == 0 || width > 16384 || height > 16384) {
		return;
	}
	const uint32_t sub_width = (chroma == 1 || chroma == 2) ? 2 : 1;
	const uint32_t sub_height = chroma == 1 ? 2 : 1;
	r_sps.width = width;
	r_sps.height = height;
	r_sps.display_width = width - sub_width * (crop_left + crop_right);
	r_sps.display_height = height - sub_height * (crop_top + crop_bottom);
	r_sps.bit_depth = bit_depth;
	r_sps.chroma_format = chroma;
	r_sps.valid = true;
}

void AnnexBSplitter::parse_h264_sps(const uint8_t *p_nal, uint64_t p_size, SPSInfo &r_sps) {
	uint8_t buffer[512];
	if (p_size < 4) {
		return;
	}
	BitReader b;
	b.data = buffer;
	b.size = unescape(p_nal + 1, p_size - 1, buffer, sizeof(buffer));
	const uint32_t profile = b.read(8);
	b.skip(16); // Constraint flags, level_idc.
	b.read_ue(); // seq_parameter_set_id
	uint32_t chroma = 1;
	uint32_t bit_depth = 8;
	if (profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 || profile == 83 || profile == 86 ||
			profile == 118 || profile == 128 || profile == 138 || profile == 139 || profile == 134 || profile == 135) {
		chroma = b.read_ue();
		if (chroma == 3) {
			b.skip(1);
		}
		bit_depth = b.read_ue() + 8;
		b.read_ue(); // bit_depth_chroma_minus8
		b.skip(1); // qpprime_y_zero_transform_bypass_flag
		if (b.read(1)) { // seq_scaling_matrix_present_flag
			for (int i = 0; i < (chroma == 3 ? 12 : 8) && !b.error; i++) {
				if (b.read(1)) {
					h264_skip_scaling_list(b, i < 6 ? 16 : 64);
				}
			}
		}
	}
	b.read_ue(); // log2_max_frame_num_minus4
	const uint32_t poc_type = b.read_ue();
	if (poc_type == 0) {
		b.read_ue();
	} else if (poc_type == 1) {
		b.skip(1);
		b.read_se();
		b.read_se();
		const uint32_t count = b.read_ue();
		for (uint32_t i = 0; i < count && i < 256 && !b.error; i++) {
			b.read_se();
		}
	}
	b.read_ue(); // max_num_ref_frames
	b.skip(1); // gaps_in_frame_num_value_allowed_flag
	const uint32_t width_mbs = b.read_ue() + 1;
	const uint32_t height_map_units = b.read_ue() + 1;
	const uint32_t frame_mbs_only = b.read(1);
	if (!frame_mbs_only) {
		b.skip(1);
	}
	b.skip(1); // direct_8x8_inference_flag
	uint32_t crop_left = 0, crop_right = 0, crop_top = 0, crop_bottom = 0;
	if (b.read(1)) {
		crop_left = b.read_ue();
		crop_right = b.read_ue();
		crop_top = b.read_ue();
		crop_bottom = b.read_ue();
	}
	if (b.error || width_mbs > 1024 || height_map_units > 1024) {
		return;
	}
	const uint32_t width = width_mbs * 16;
	const uint32_t height = (2 - frame_mbs_only) * height_map_units * 16;
	const uint32_t crop_unit_x = (chroma == 1 || chroma == 2) ? 2 : 1;
	const uint32_t crop_unit_y = (chroma == 1 ? 2 : 1) * (2 - frame_mbs_only);
	r_sps.width = width;
	r_sps.height = height;
	r_sps.display_width = width - crop_unit_x * (crop_left + crop_right);
	r_sps.display_height = height - crop_unit_y * (crop_top + crop_bottom);
	r_sps.bit_depth = bit_depth;
	r_sps.chroma_format = chroma;
	r_sps.valid = true;
}

uint64_t AnnexBSplitter::_find_start_code(const uint8_t *p_data, uint64_t p_size, uint64_t p_from) {
	uint64_t i = p_from;
	while (i + 3 <= p_size) {
		if (p_data[i + 2] > 1) {
			i += 3;
		} else if (p_data[i + 2] == 1 && p_data[i + 1] == 0 && p_data[i] == 0) {
			return i;
		} else {
			i++;
		}
	}
	return p_size;
}

void AnnexBSplitter::_classify(const uint8_t *p_nal, uint64_t p_size, bool &r_new_au, bool &r_vcl, SPSInfo &r_sps) const {
	r_new_au = false;
	r_vcl = false;
	r_sps.valid = false;
	if (codec == CODEC_HEVC) {
		if (p_size < 2) {
			return;
		}
		const uint32_t type = (p_nal[0] >> 1) & 0x3f;
		const uint32_t layer = ((p_nal[0] & 1u) << 5) | (p_nal[1] >> 3);
		r_vcl = type <= 31;
		if (type == 35) {
			r_new_au = true;
		} else if (layer == 0 && ((type >= 32 && type <= 34) || type == 39 || (type >= 41 && type <= 44) || (type >= 48 && type <= 55))) {
			r_new_au = true;
		} else if (layer == 0 && type <= 31 && p_size >= 3 && (p_nal[2] & 0x80)) {
			r_new_au = true;
		}
		if (type == 33 && layer == 0) {
			parse_hevc_sps(p_nal, p_size, r_sps);
		}
	} else {
		if (p_size < 1) {
			return;
		}
		const uint32_t type = p_nal[0] & 0x1f;
		r_vcl = type >= 1 && type <= 5;
		if (type == 9 || type == 6 || type == 7 || type == 8 || (type >= 15 && type <= 18)) {
			r_new_au = true;
		} else if ((type == 1 || type == 2 || type == 5) && p_size >= 2 && (p_nal[1] & 0x80)) {
			r_new_au = true;
		}
		if (type == 7) {
			parse_h264_sps(p_nal, p_size, r_sps);
		}
	}
}

void AnnexBSplitter::init(const uint8_t *p_data, uint64_t p_size, Codec p_codec) {
	data = p_data;
	data_size = p_size;
	codec = p_codec;
	first_sps = SPSInfo();
	seek(0);
}

void AnnexBSplitter::seek(uint64_t p_offset) {
	// An access unit offset is its first start code, or the zero_byte just before it.
	start_code = _find_start_code(data, data_size, p_offset);
	au_start = UINT64_MAX;
	seen_vcl = false;
	sps_width = 0;
	sps_height = 0;
}

bool AnnexBSplitter::next(AccessUnit &r_au) {
	while (start_code < data_size) {
		const uint64_t nal = start_code + 3;
		const uint64_t start = (start_code > 0 && data[start_code - 1] == 0) ? start_code - 1 : start_code;
		const uint64_t next_start_code = _find_start_code(data, data_size, nal);
		bool new_au = false;
		bool vcl = false;
		SPSInfo sps;
		_classify(data + nal, next_start_code - nal, new_au, vcl, sps);

		bool emit = false;
		if (au_start == UINT64_MAX) {
			au_start = start;
		} else if (seen_vcl && new_au) {
			r_au.offset = au_start;
			r_au.size = start - au_start;
			r_au.sps_width = sps_width;
			r_au.sps_height = sps_height;
			emit = true;
			au_start = start;
			seen_vcl = false;
			sps_width = 0;
			sps_height = 0;
		}
		if (sps.valid) {
			sps_width = sps.width;
			sps_height = sps.height;
			if (!first_sps.valid) {
				first_sps = sps;
			}
		}
		if (vcl) {
			seen_vcl = true;
		}
		start_code = next_start_code;
		if (emit) {
			return true;
		}
	}
	if (au_start != UINT64_MAX && seen_vcl) {
		r_au.offset = au_start;
		r_au.size = data_size - au_start;
		r_au.sps_width = sps_width;
		r_au.sps_height = sps_height;
		au_start = UINT64_MAX;
		seen_vcl = false;
		return true;
	}
	return false;
}
