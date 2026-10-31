/**************************************************************************/
/*  media_languages.cpp                                                   */
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

#include "media_languages.h"

#include "core/templates/local_vector.h"

namespace {

struct Language {
	const char *iso1; // ISO 639-1.
	const char *iso2b; // ISO 639-2/B.
	const char *iso2t; // ISO 639-2/T (often the same).
	const char *name; // English name, lower case.
	const char *alias; // Another name used in file names, or nullptr.
};

const Language LANGUAGES[] = {
	{ "en", "eng", "eng", "english", nullptr },
	{ "de", "ger", "deu", "german", "deutsch" },
	{ "fr", "fre", "fra", "french", "francais" },
	{ "es", "spa", "spa", "spanish", "espanol" },
	{ "it", "ita", "ita", "italian", "italiano" },
	{ "pt", "por", "por", "portuguese", "portugues" },
	{ "nl", "dut", "nld", "dutch", "nederlands" },
	{ "sv", "swe", "swe", "swedish", "svenska" },
	{ "no", "nor", "nor", "norwegian", "norsk" },
	{ "nb", "nob", "nob", "bokmal", nullptr },
	{ "nn", "nno", "nno", "nynorsk", nullptr },
	{ "da", "dan", "dan", "danish", "dansk" },
	{ "fi", "fin", "fin", "finnish", "suomi" },
	{ "is", "ice", "isl", "icelandic", nullptr },
	{ "pl", "pol", "pol", "polish", "polski" },
	{ "cs", "cze", "ces", "czech", nullptr },
	{ "sk", "slo", "slk", "slovak", nullptr },
	{ "sl", "slv", "slv", "slovenian", "slovene" },
	{ "hu", "hun", "hun", "hungarian", "magyar" },
	{ "ro", "rum", "ron", "romanian", nullptr },
	{ "bg", "bul", "bul", "bulgarian", nullptr },
	{ "hr", "hrv", "hrv", "croatian", nullptr },
	{ "sr", "srp", "srp", "serbian", nullptr },
	{ "bs", "bos", "bos", "bosnian", nullptr },
	{ "mk", "mac", "mkd", "macedonian", nullptr },
	{ "sq", "alb", "sqi", "albanian", nullptr },
	{ "el", "gre", "ell", "greek", nullptr },
	{ "tr", "tur", "tur", "turkish", nullptr },
	{ "ru", "rus", "rus", "russian", nullptr },
	{ "uk", "ukr", "ukr", "ukrainian", nullptr },
	{ "be", "bel", "bel", "belarusian", nullptr },
	{ "lt", "lit", "lit", "lithuanian", nullptr },
	{ "lv", "lav", "lav", "latvian", nullptr },
	{ "et", "est", "est", "estonian", nullptr },
	{ "ga", "gle", "gle", "irish", nullptr },
	{ "cy", "wel", "cym", "welsh", nullptr },
	{ "eu", "baq", "eus", "basque", nullptr },
	{ "ca", "cat", "cat", "catalan", nullptr },
	{ "gl", "glg", "glg", "galician", nullptr },
	{ "he", "heb", "heb", "hebrew", nullptr },
	{ "ar", "ara", "ara", "arabic", nullptr },
	{ "fa", "per", "fas", "persian", "farsi" },
	{ "hi", "hin", "hin", "hindi", nullptr },
	{ "bn", "ben", "ben", "bengali", nullptr },
	{ "ta", "tam", "tam", "tamil", nullptr },
	{ "te", "tel", "tel", "telugu", nullptr },
	{ "ml", "mal", "mal", "malayalam", nullptr },
	{ "kn", "kan", "kan", "kannada", nullptr },
	{ "mr", "mar", "mar", "marathi", nullptr },
	{ "ur", "urd", "urd", "urdu", nullptr },
	{ "th", "tha", "tha", "thai", nullptr },
	{ "vi", "vie", "vie", "vietnamese", nullptr },
	{ "id", "ind", "ind", "indonesian", nullptr },
	{ "ms", "may", "msa", "malay", nullptr },
	{ "tl", "tgl", "tgl", "tagalog", "filipino" },
	{ "zh", "chi", "zho", "chinese", "mandarin" },
	{ "ja", "jpn", "jpn", "japanese", nullptr },
	{ "ko", "kor", "kor", "korean", nullptr },
	{ "ka", "geo", "kat", "georgian", nullptr },
	{ "hy", "arm", "hye", "armenian", nullptr },
	{ "kk", "kaz", "kaz", "kazakh", nullptr },
	{ "af", "afr", "afr", "afrikaans", nullptr },
	{ "sw", "swa", "swa", "swahili", nullptr },
	{ "la", "lat", "lat", "latin", nullptr },
};

// Other codes some containers and files use for the languages above.
const char *const EXTRA_CODES[][2] = {
	{ "cmn", "zh" },
	{ "fil", "tl" },
	{ "scc", "sr" },
	{ "scr", "hr" },
	{ "iw", "he" },
	{ "in", "id" },
	{ "pob", "pt-br" },
};

bool is_undetermined(const String &p_code) {
	return p_code.is_empty() || p_code == "und" || p_code == "zxx" || p_code == "mis" || p_code == "mul";
}

const Language *find(const String &p_lower, bool p_codes, bool p_names) {
	for (const Language &language : LANGUAGES) {
		if (p_codes && (p_lower == language.iso1 || p_lower == language.iso2b || p_lower == language.iso2t)) {
			return &language;
		}
		if (p_names && (p_lower == language.name || (language.alias && p_lower == language.alias))) {
			return &language;
		}
	}
	return nullptr;
}

// "es-419", "pt_br": the primary code and the region (empty if none).
void split_region(const String &p_lower, String &r_primary, String &r_region) {
	const int separator = MAX(p_lower.find_char('-'), p_lower.find_char('_'));
	if (separator > 0) {
		r_primary = p_lower.substr(0, separator);
		r_region = p_lower.substr(separator + 1);
	} else {
		r_primary = p_lower;
		r_region = String();
	}
}

bool is_region(const String &p_region) {
	if (p_region.length() == 2) {
		return is_ascii_alphabet_char(p_region[0]) && is_ascii_alphabet_char(p_region[1]);
	}
	return p_region.length() == 3 && is_digit(p_region[0]) && is_digit(p_region[1]) && is_digit(p_region[2]);
}

String with_region(const String &p_code, const String &p_region) {
	return p_region.is_empty() || p_code.contains_char('-') ? p_code : p_code + "-" + p_region;
}

} // namespace

namespace MediaLanguages {

String normalize(const String &p_code) {
	const String lower = p_code.strip_edges().to_lower();
	if (is_undetermined(lower)) {
		return String();
	}
	String primary;
	String region;
	split_region(lower, primary, region);
	for (const auto &extra : EXTRA_CODES) {
		if (primary == extra[0]) {
			return with_region(extra[1], region);
		}
	}
	const Language *language = find(primary, true, true);
	if (language == nullptr) {
		return lower;
	}
	return with_region(language->iso1, region);
}

bool parse_token(const String &p_token, bool p_allow_codes, String &r_code) {
	const String lower = p_token.strip_edges().to_lower();
	if (lower.is_empty()) {
		return false;
	}
	String primary;
	String region;
	split_region(lower, primary, region);
	if (!region.is_empty() && !is_region(region)) {
		return false;
	}
	const Language *language = find(primary, p_allow_codes, true);
	if (language == nullptr) {
		if (!p_allow_codes) {
			return false;
		}
		for (const auto &extra : EXTRA_CODES) {
			if (primary == extra[0]) {
				r_code = with_region(extra[1], region);
				return true;
			}
		}
		return false;
	}
	r_code = with_region(language->iso1, region);
	return true;
}

String name(const String &p_code) {
	String primary;
	String region;
	split_region(p_code.to_lower(), primary, region);
	const Language *language = find(primary, true, false);
	if (language == nullptr) {
		return p_code;
	}
	const String english = String(language->name).capitalize();
	return region.is_empty() ? english : english + " (" + region.to_upper() + ")";
}

bool matches(const String &p_track_code, const String &p_preferred_code) {
	if (p_track_code.is_empty() || p_preferred_code.is_empty()) {
		return false;
	}
	String track_primary;
	String track_region;
	split_region(p_track_code, track_primary, track_region);
	String preferred_primary;
	String preferred_region;
	split_region(p_preferred_code, preferred_primary, preferred_region);
	if (track_primary != preferred_primary) {
		// Norwegian: "no" covers both written forms.
		if (!((track_primary == "nb" || track_primary == "nn") && preferred_primary == "no")) {
			return false;
		}
	}
	return preferred_region.is_empty() || track_region == preferred_region;
}

bool is_subtitle_extension(const String &p_extension) {
	const String extension = p_extension.to_lower();
	return extension == "srt" || extension == "ass" || extension == "ssa" || extension == "vtt";
}

bool is_video_extension(const String &p_extension) {
	static const char *const EXTENSIONS[] = { "mkv", "mp4", "m4v", "mov", "avi", "ts", "m2ts", "mts", "webm", "wmv", "mpg", "mpeg" };
	const String extension = p_extension.to_lower();
	for (const char *candidate : EXTENSIONS) {
		if (extension == candidate) {
			return true;
		}
	}
	return false;
}

SubtitleFileName parse_subtitle_file_name(const String &p_video_file, const String &p_subtitle_file) {
	SubtitleFileName result;
	const String extension = p_subtitle_file.get_extension().to_lower();
	result.subtitle = is_subtitle_extension(extension);
	result.format = extension == "ssa" ? String("ass") : extension;
	const String base = p_subtitle_file.get_basename();
	const String video_base = p_video_file.get_basename();
	String rest = base;
	if (!video_base.is_empty() && base.to_lower().begins_with(video_base.to_lower())) {
		result.matches_video = true;
		rest = base.substr(video_base.length());
	}

	// Words split on . , space and brackets. A word that isn't a language as a whole ("pt-BR" is) is split again on - and _
	// ("2_English", "English-SDH").
	LocalVector<String> words;
	String current;
	for (int i = 0; i < rest.length(); i++) {
		const char32_t c = rest[i];
		if (c == '.' || c == ' ' || c == ',' || c == '(' || c == ')' || c == '[' || c == ']') {
			if (!current.is_empty()) {
				words.push_back(current);
			}
			current = String();
		} else {
			current += c;
		}
	}
	if (!current.is_empty()) {
		words.push_back(current);
	}
	LocalVector<String> languages; // In file-name order.
	for (const String &word : words) {
		String code;
		if (parse_token(word, true, code)) {
			languages.push_back(code);
			continue;
		}
		const Vector<String> parts = word.replace_char('_', '-').split("-", false);
		for (const String &part : parts) {
			const String lower = part.to_lower();
			if (lower == "forced" || lower == "foreign") {
				result.forced = true;
			} else if (lower == "sdh" || lower == "cc" || lower == "hearing" || lower == "deaf") {
				result.hearing_impaired = true;
			} else if (parse_token(part, true, code)) {
				languages.push_back(code);
			}
		}
	}
	// "en.hi": "hi" after a language means hearing impaired, not Hindi. Otherwise the last language named wins (tags
	// come at the end; a title word that happens to be a code comes earlier).
	if (languages.size() >= 2 && languages[languages.size() - 1] == "hi") {
		result.hearing_impaired = true;
		languages.resize(languages.size() - 1);
	}
	if (!languages.is_empty()) {
		result.language = languages[languages.size() - 1];
	}
	return result;
}

} // namespace MediaLanguages
