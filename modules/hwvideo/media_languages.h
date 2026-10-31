/**************************************************************************/
/*  media_languages.h                                                     */
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

#include "core/string/ustring.h"

// Language codes of tracks and subtitle files. Containers use ISO 639-2 (Matroska: the B codes such as "ger", MP4: the T
// codes such as "deu"), file names use ISO 639-1 ("de"), 639-2 or English names ("German"), sometimes with a region
// ("pt-BR", "es-419"). Everything is normalized to the ISO 639-1 code where there is one ("de"), lower case, with the
// region kept after a hyphen ("pt-br").
namespace MediaLanguages {

// "ger", "deu", "de", "DE", "German", "de_DE" -> "de" / "de-de". "und", "zxx", "mis", "mul" and "" -> "". Unknown codes are
// returned lower case.
String normalize(const String &p_code);
// True if p_token (one word of a file name) names a language (a known code or English name). r_code: normalized.
// Two- and three-letter codes count only when p_allow_codes (file names: "en", "eng"); names always count.
bool parse_token(const String &p_token, bool p_allow_codes, String &r_code);
// The English name of a normalized code ("de" -> "German"), or the code itself.
String name(const String &p_code);
// Whether a track's language satisfies a preference: same language, and the same region if the preference has one.
bool matches(const String &p_track_code, const String &p_preferred_code);

// What a subtitle file's name says, next to a video ("Film.2019.en.forced.srt" next to "Film.2019.mkv"; "2_English.srt"
// in a Subs folder).
struct SubtitleFileName {
	bool subtitle = false; // .srt, .ass, .ssa or .vtt.
	bool matches_video = false; // Its name starts with the video's name (without the extension).
	String language; // Normalized; empty if the name says none.
	bool forced = false; // "forced", "foreign".
	bool hearing_impaired = false; // "sdh", "cc", "hi" (after a language), "hearing impaired".
	String format; // "srt", "ass", "vtt".
};
SubtitleFileName parse_subtitle_file_name(const String &p_video_file, const String &p_subtitle_file);
bool is_subtitle_extension(const String &p_extension);
bool is_video_extension(const String &p_extension);

} // namespace MediaLanguages
