/**************************************************************************/
/*  subtitle_style.cpp                                                    */
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

#include "subtitle_style.h"

#include "core/variant/array.h"
#include "core/variant/variant.h"

namespace {

// SSA's alignment (1-3 bottom, +4 top, +8 middle) to the numpad layout ASS uses.
int legacy_alignment(int p_value) {
	const int column = CLAMP(p_value & 3, 1, 3);
	if (p_value & 4) {
		return column + 6;
	}
	if (p_value & 8) {
		return column + 3;
	}
	return column;
}

bool flag_value(const String &p_value) {
	const int v = p_value.strip_edges().to_int();
	return v != 0;
}

struct State {
	String font;
	float size = 18.0f;
	Color color;
	Color outline;
	float outline_width = 2.0f;
	bool bold = false;
	bool italic = false;
	bool underline = false;
	bool strikeout = false;

	void from_style(const SubtitleStyle::Style &p_style) {
		font = p_style.font;
		size = p_style.size;
		color = p_style.primary;
		outline = p_style.outline;
		outline_width = p_style.outline_width;
		bold = p_style.bold;
		italic = p_style.italic;
		underline = p_style.underline;
		strikeout = p_style.strikeout;
	}

	bool operator==(const State &p_other) const {
		return font == p_other.font && size == p_other.size && color == p_other.color && outline == p_other.outline && outline_width == p_other.outline_width &&
				bold == p_other.bold && italic == p_other.italic && underline == p_other.underline && strikeout == p_other.strikeout;
	}

	Dictionary to_run(const String &p_text) const {
		Dictionary d;
		d["text"] = p_text;
		d["font"] = font;
		d["size"] = size;
		d["color"] = color;
		d["outline_color"] = outline;
		d["outline_width"] = outline_width;
		d["bold"] = bold;
		d["italic"] = italic;
		d["underline"] = underline;
		d["strikeout"] = strikeout;
		return d;
	}
};

const SubtitleStyle::Style &find_style(const SubtitleStyle::Header &p_header, const String &p_name) {
	static const SubtitleStyle::Style fallback;
	String name = p_name.strip_edges();
	if (name.begins_with("*")) {
		name = name.substr(1); // "*Default" in some scripts.
	}
	if (const SubtitleStyle::Style *style = p_header.styles.getptr(name)) {
		return *style;
	}
	if (const SubtitleStyle::Style *style = p_header.styles.getptr("Default")) {
		return *style;
	}
	return fallback;
}

} // namespace

Color SubtitleStyle::parse_color(const String &p_value, const Color &p_base, bool p_with_alpha) {
	String v = p_value.strip_edges().to_upper();
	int64_t number = 0;
	bool has_alpha = false;
	if (v.begins_with("&H") || v.begins_with("H")) {
		v = v.trim_prefix("&").trim_prefix("H").trim_suffix("&");
		if (v.is_empty() || !v.is_valid_hex_number(false)) {
			return p_base;
		}
		number = v.hex_to_int();
		has_alpha = v.length() > 6;
	} else if (v.is_valid_int()) {
		number = v.to_int(); // SSA styles: decimal BGR.
		has_alpha = number > 0xFFFFFF;
	} else {
		return p_base;
	}
	const uint32_t value = (uint32_t)number;
	Color color((value & 0xFF) / 255.0f, ((value >> 8) & 0xFF) / 255.0f, ((value >> 16) & 0xFF) / 255.0f, p_base.a);
	if (p_with_alpha && has_alpha) {
		color.a = 1.0f - ((value >> 24) & 0xFF) / 255.0f;
	}
	return color;
}

SubtitleStyle::Header SubtitleStyle::parse_header(const String &p_header) {
	Header header;
	enum Section {
		NONE,
		INFO,
		STYLES_ASS,
		STYLES_SSA,
		OTHER,
	} section = NONE;
	Vector<String> format;
	int play_x = 0;
	int play_y = 0;
	const Vector<String> lines = p_header.split("\n");
	for (String line : lines) {
		line = line.strip_edges();
		if (line.is_empty() || line.begins_with(";")) {
			continue;
		}
		if (line.begins_with("[")) {
			const String lower = line.to_lower();
			section = lower == "[script info]" ? INFO : (lower == "[v4+ styles]" ? STYLES_ASS : (lower == "[v4 styles]" ? STYLES_SSA : OTHER));
			format.clear();
			continue;
		}
		const int colon = line.find_char(':');
		if (colon < 0) {
			continue;
		}
		const String key = line.substr(0, colon).strip_edges();
		const String value = line.substr(colon + 1).strip_edges();
		if (section == INFO) {
			if (key == "PlayResX") {
				play_x = value.to_int();
			} else if (key == "PlayResY") {
				play_y = value.to_int();
			}
		} else if (section == STYLES_ASS || section == STYLES_SSA) {
			if (key == "Format") {
				format = value.split(",");
				for (int i = 0; i < format.size(); i++) {
					format.write[i] = format[i].strip_edges().to_lower();
				}
			} else if (key == "Style" && !format.is_empty()) {
				const Vector<String> values = value.split(",", true, format.size() - 1);
				Style style;
				for (int i = 0; i < format.size() && i < values.size(); i++) {
					const String &field = format[i];
					const String v = values[i].strip_edges();
					if (field == "name") {
						style.name = v;
					} else if (field == "fontname") {
						style.font = v;
					} else if (field == "fontsize") {
						style.size = v.to_float();
					} else if (field == "primarycolour" || field == "primarycolor") {
						style.primary = parse_color(v, style.primary, true);
					} else if (field == "outlinecolour" || field == "outlinecolor" || field == "tertiarycolour") {
						style.outline = parse_color(v, style.outline, true);
					} else if (field == "backcolour" || field == "backcolor") {
						style.back = parse_color(v, style.back, true);
					} else if (field == "bold") {
						style.bold = flag_value(v);
					} else if (field == "italic") {
						style.italic = flag_value(v);
					} else if (field == "underline") {
						style.underline = flag_value(v);
					} else if (field == "strikeout") {
						style.strikeout = flag_value(v);
					} else if (field == "alignment") {
						const int a = v.to_int();
						style.alignment = section == STYLES_SSA ? legacy_alignment(a) : CLAMP(a, 1, 9);
					} else if (field == "outline") {
						style.outline_width = v.to_float();
					} else if (field == "marginl") {
						style.margin_l = v.to_int();
					} else if (field == "marginr") {
						style.margin_r = v.to_int();
					} else if (field == "marginv") {
						style.margin_v = v.to_int();
					}
				}
				header.styles[style.name] = style;
			}
		}
	}
	// One of the two given: the other as for a 4:3 script (libass does the same).
	if (play_x > 0 && play_y <= 0) {
		play_y = play_x == 1280 ? 1024 : play_x * 3 / 4;
	} else if (play_y > 0 && play_x <= 0) {
		play_x = play_y == 1024 ? 1280 : play_y * 4 / 3;
	}
	if (play_x > 0 && play_y > 0) {
		header.play_res = Vector2i(play_x, play_y);
	}
	return header;
}

Dictionary SubtitleStyle::parse_event(const String &p_line, const Header &p_header) {
	// The first 8 fields; the text is the rest (it may contain commas).
	Vector<String> fields;
	int start = 0;
	for (int i = 0; i < p_line.length() && fields.size() < 8; i++) {
		if (p_line[i] == ',') {
			fields.push_back(p_line.substr(start, i - start));
			start = i + 1;
		}
	}
	const String text = fields.size() == 8 ? p_line.substr(start) : p_line;
	const Style &style = find_style(p_header, fields.size() > 2 ? fields[2] : String());

	int margin_l = style.margin_l;
	int margin_r = style.margin_r;
	int margin_v = style.margin_v;
	if (fields.size() == 8) {
		margin_l = fields[4].to_int() != 0 ? fields[4].to_int() : margin_l;
		margin_r = fields[5].to_int() != 0 ? fields[5].to_int() : margin_r;
		margin_v = fields[6].to_int() != 0 ? fields[6].to_int() : margin_v;
	}
	int alignment = style.alignment;
	bool alignment_set = false;
	Vector2 position(-1, -1);

	State state;
	state.from_style(style);
	State run_state = state;
	String run;
	String plain;
	Array runs;
	auto flush = [&]() {
		if (!run.is_empty()) {
			runs.push_back(run_state.to_run(run));
			run = String();
		}
		run_state = state;
	};

	for (int i = 0; i < text.length(); i++) {
		const char32_t c = text[i];
		if (c == '{') {
			const int close = text.find_char('}', i + 1);
			if (close < 0) {
				break;
			}
			const Vector<String> tags = text.substr(i + 1, close - i - 1).split("\\", false);
			for (const String &raw : tags) {
				const String tag = raw.strip_edges();
				if (tag.is_empty()) {
					continue;
				}
				if (tag.begins_with("an") && tag.length() >= 3 && is_digit(tag[2])) {
					if (!alignment_set) {
						alignment = CLAMP(tag.substr(2).to_int(), 1, 9);
						alignment_set = true;
					}
				} else if (tag.begins_with("pos(")) {
					const Vector<String> xy = tag.substr(4).trim_suffix(")").split(",");
					if (xy.size() >= 2) {
						position = Vector2(xy[0].to_float(), xy[1].to_float());
					}
				// A tag without a value goes back to the style's value: FFmpeg closes SubRip's <font> with {\c}, {\fs}, {\fn}.
				} else if (tag == "fs") {
					state.size = style.size;
				} else if (tag.begins_with("fs") && (is_digit(tag[2]) || tag[2] == '.')) {
					state.size = tag.substr(2).to_float();
				} else if (tag.begins_with("fn")) {
					state.font = tag.length() > 2 ? tag.substr(2) : style.font;
				} else if (tag == "bord") {
					state.outline_width = style.outline_width;
				} else if (tag.begins_with("bord") && (is_digit(tag[4]) || tag[4] == '.')) {
					state.outline_width = tag.substr(4).to_float();
				} else if (tag == "c" || tag == "1c") {
					state.color = Color(style.primary.r, style.primary.g, style.primary.b, state.color.a);
				} else if (tag.begins_with("1c&") || tag.begins_with("c&")) {
					state.color = parse_color(tag.substr(tag[0] == '1' ? 2 : 1), state.color, false);
				} else if (tag == "3c") {
					state.outline = Color(style.outline.r, style.outline.g, style.outline.b, state.outline.a);
				} else if (tag.begins_with("3c&")) {
					state.outline = parse_color(tag.substr(2), state.outline, false);
				} else if (tag.begins_with("1a") || tag.begins_with("alpha")) {
					const String value = tag.substr(tag.begins_with("1a") ? 2 : 5).trim_prefix("&").trim_prefix("H").trim_suffix("&");
					if (value.is_valid_hex_number(false)) {
						state.color.a = 1.0f - (value.hex_to_int() & 0xFF) / 255.0f;
					}
				} else if (tag == "r") {
					state.from_style(style);
				} else if (tag.begins_with("r") && !tag.begins_with("rnd")) {
					state.from_style(find_style(p_header, tag.substr(1)));
				} else if (tag[0] == 'b' && (tag.length() == 1 || is_digit(tag[1]))) {
					const int weight = tag.length() == 1 ? (style.bold ? 1 : 0) : tag.substr(1).to_int();
					state.bold = weight == 1 || weight >= 600;
				} else if (tag[0] == 'i' && (tag.length() == 1 || is_digit(tag[1]))) {
					state.italic = tag.length() == 1 ? style.italic : tag.substr(1).to_int() != 0;
				} else if (tag[0] == 'u' && (tag.length() == 1 || is_digit(tag[1]))) {
					state.underline = tag.length() == 1 ? style.underline : tag.substr(1).to_int() != 0;
				} else if (tag[0] == 's' && (tag.length() == 1 || is_digit(tag[1]))) {
					state.strikeout = tag.length() == 1 ? style.strikeout : tag.substr(1).to_int() != 0;
				} else if (tag[0] == 'a' && tag.length() >= 2 && is_digit(tag[1])) {
					if (!alignment_set) {
						alignment = legacy_alignment(tag.substr(1).to_int());
						alignment_set = true;
					}
				}
				// Everything else (karaoke, \t, \clip, \p drawings, \blur, \fr, \move, \fad, ...) is dropped.
			}
			if (!(state == run_state)) {
				flush();
			}
			i = close;
			continue;
		}
		if (c == '\\' && i + 1 < text.length() && (text[i + 1] == 'N' || text[i + 1] == 'n' || text[i + 1] == 'h')) {
			const String add = text[i + 1] == 'h' ? String::chr(0xA0) : String("\n");
			run += add;
			plain += text[i + 1] == 'h' ? String(" ") : add;
			i++;
			continue;
		}
		if (c == '\r' || c == '\n') {
			continue;
		}
		if (!(state == run_state)) {
			flush();
		}
		run += c;
		plain += c;
	}
	flush();

	Dictionary d;
	d["runs"] = runs;
	d["text"] = plain.strip_edges();
	d["alignment"] = alignment;
	d["position"] = position;
	d["margin_l"] = margin_l;
	d["margin_r"] = margin_r;
	d["margin_v"] = margin_v;
	d["play_res"] = p_header.play_res;
	d["style"] = style.name;
	d["layer"] = fields.size() > 1 ? fields[1].to_int() : 0;
	d["back_color"] = style.back;
	return d;
}
