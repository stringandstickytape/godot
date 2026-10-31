/**************************************************************************/
/*  subtitle_style.h                                                      */
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

#include "core/math/color.h"
#include "core/math/vector2i.h"
#include "core/string/ustring.h"
#include "core/templates/hash_map.h"
#include "core/variant/dictionary.h"

// What an app without libass can draw of an ASS/SSA event (FFmpeg gives every text track as ASS: SubRip's <b>, <i> and
// <font color> become override tags): runs of text with bold, italic, underline, strike-out, colour and size, the
// alignment (numpad 1-9), an explicit position, the margins, and the script resolution those are in. Everything else
// (karaoke, transforms, clips, drawings, blur, rotation) is dropped.
class SubtitleStyle {
public:
	struct Style {
		String name = "Default";
		String font = "Arial";
		float size = 18.0f;
		Color primary = Color(1, 1, 1, 1);
		Color outline = Color(0, 0, 0, 1);
		Color back = Color(0, 0, 0, 0.5f);
		float outline_width = 2.0f;
		bool bold = false;
		bool italic = false;
		bool underline = false;
		bool strikeout = false;
		int alignment = 2; // Numpad layout: 1-3 bottom, 4-6 middle, 7-9 top.
		int margin_l = 10;
		int margin_r = 10;
		int margin_v = 10;
	};

	struct Header {
		Vector2i play_res = Vector2i(384, 288); // ASS's default when the script doesn't say.
		HashMap<String, Style> styles;
	};

	static Header parse_header(const String &p_header);
	// p_line: an event as FFmpeg's decoders give it: "ReadOrder,Layer,Style,Name,MarginL,MarginR,MarginV,Effect,Text".
	static Dictionary parse_event(const String &p_line, const Header &p_header);
	// "&HAABBGGRR&" (or decimal) to a Color; ASS alpha 0 is opaque.
	static Color parse_color(const String &p_value, const Color &p_base, bool p_with_alpha);
};
