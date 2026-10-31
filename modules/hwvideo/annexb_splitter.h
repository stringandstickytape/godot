/**************************************************************************/
/*  annexb_splitter.h                                                     */
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

// Splits a raw Annex-B H.264 or HEVC elementary stream into access units, one at a time, so a long file needs no scan up
// front. Same heuristic as spikes/01-decode/v4l2dec.c (--parse-only): a new access unit starts at the first of these NAL
// units that follows a VCL NAL unit of the current one:
//   HEVC: AUD (35) on any layer; on nuh_layer_id 0 only: VPS/SPS/PPS (32-34), prefix SEI (39), 41-44, 48-55, or a VCL NAL
//         unit (0-31) with first_slice_segment_in_pic_flag set. Layer-1 (MV-HEVC) slices stay with their layer-0 picture.
//   H.264: AUD (9), SEI (6), SPS (7), PPS (8), 15-18, or a slice (1, 2, 5) with first_mb_in_slice = 0. SVC/MVC streams are
//         not split correctly, and PAFF field pairs become two access units.
// An access unit spans from the zero_byte of its first start code to the start of the next access unit.
class AnnexBSplitter {
public:
	enum Codec {
		CODEC_H264,
		CODEC_HEVC,
	};

	struct SPSInfo {
		uint32_t width = 0; // Coded size.
		uint32_t height = 0;
		uint32_t display_width = 0; // After the conformance window / frame cropping.
		uint32_t display_height = 0;
		uint32_t bit_depth = 0;
		uint32_t chroma_format = 0;
		bool valid = false;
	};

	struct AccessUnit {
		uint64_t offset = 0;
		uint64_t size = 0;
		// Coded size of an SPS inside this access unit (0 if none). Marks where a session restart can resume.
		uint32_t sps_width = 0;
		uint32_t sps_height = 0;
	};

private:
	const uint8_t *data = nullptr;
	uint64_t data_size = 0;
	Codec codec = CODEC_HEVC;

	uint64_t start_code = 0; // Offset of the next start code to classify.
	uint64_t au_start = UINT64_MAX;
	bool seen_vcl = false;
	uint32_t sps_width = 0;
	uint32_t sps_height = 0;
	SPSInfo first_sps;

	static uint64_t _find_start_code(const uint8_t *p_data, uint64_t p_size, uint64_t p_from);
	void _classify(const uint8_t *p_nal, uint64_t p_size, bool &r_new_au, bool &r_vcl, SPSInfo &r_sps) const;

public:
	static void parse_hevc_sps(const uint8_t *p_nal, uint64_t p_size, SPSInfo &r_sps);
	static void parse_h264_sps(const uint8_t *p_nal, uint64_t p_size, SPSInfo &r_sps);

	void init(const uint8_t *p_data, uint64_t p_size, Codec p_codec);
	// Continues at p_offset, which must be the offset of an access unit returned earlier (0 for the start).
	void seek(uint64_t p_offset);
	// Returns false at the end of the data.
	bool next(AccessUnit &r_au);

	// The first SPS seen so far (by next()).
	const SPSInfo &get_first_sps() const { return first_sps; }
	Codec get_codec() const { return codec; }
	const uint8_t *get_data() const { return data; }
	uint64_t get_data_size() const { return data_size; }
};
