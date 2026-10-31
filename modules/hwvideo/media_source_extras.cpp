/**************************************************************************/
/*  media_source_extras.cpp                                               */
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

// MediaSource: chapters, stereo and spherical metadata, subtitle files next to the video, language picks and the
// subtitle read-ahead. Everything here runs on the caller's thread, outside the demux loop.

#include "media_languages.h"
#include "media_source.h"
#include "smb_client.h"

#include "core/config/project_settings.h"
#include "core/io/file_access.h"

#include <climits>
#include <cmath>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/spherical.h>
#include <libavutil/stereo3d.h>
}

static constexpr int64_t SUBTITLE_FILE_MAX_BYTES = 16 << 20;

/* Stream metadata */

void MediaSource::_read_stream_metadata(const AVStream *p_stream) {
	const AVCodecParameters *par = p_stream->codecpar;
	const AVPacketSideData *stereo_data = av_packet_side_data_get(par->coded_side_data, par->nb_coded_side_data, AV_PKT_DATA_STEREO3D);
	if (stereo_data && stereo_data->size >= sizeof(AVStereo3D)) {
		const AVStereo3D *stereo = (const AVStereo3D *)stereo_data->data;
		switch (stereo->type) {
			case AV_STEREO3D_2D:
				video.stereo_mode = "2d";
				break;
			case AV_STEREO3D_SIDEBYSIDE:
				video.stereo_mode = "sbs";
				break;
			case AV_STEREO3D_TOPBOTTOM:
				video.stereo_mode = "tb";
				break;
			case AV_STEREO3D_FRAMESEQUENCE:
				video.stereo_mode = "frame_sequence";
				break;
			case AV_STEREO3D_CHECKERBOARD:
				video.stereo_mode = "checkerboard";
				break;
			case AV_STEREO3D_SIDEBYSIDE_QUINCUNX:
				video.stereo_mode = "sbs_quincunx";
				break;
			case AV_STEREO3D_LINES:
				video.stereo_mode = "lines";
				break;
			case AV_STEREO3D_COLUMNS:
				video.stereo_mode = "columns";
				break;
			default:
				video.stereo_mode = "unspecified";
				break;
		}
		video.stereo_inverted = (stereo->flags & AV_STEREO3D_FLAG_INVERT) != 0;
		video.stereo_baseline_mm = stereo->baseline / 1000.0; // Micrometres in FFmpeg.
		if (stereo->horizontal_field_of_view.num > 0 && stereo->horizontal_field_of_view.den > 0) {
			video.stereo_fov = av_q2d(stereo->horizontal_field_of_view);
		}
	}

	const AVPacketSideData *spherical_data = av_packet_side_data_get(par->coded_side_data, par->nb_coded_side_data, AV_PKT_DATA_SPHERICAL);
	if (spherical_data && spherical_data->size >= sizeof(AVSphericalMapping)) {
		const AVSphericalMapping *mapping = (const AVSphericalMapping *)spherical_data->data;
		video.projection_yaw = mapping->yaw / 65536.0;
		video.projection_pitch = mapping->pitch / 65536.0;
		video.projection_roll = mapping->roll / 65536.0;
		switch (mapping->projection) {
			case AV_SPHERICAL_EQUIRECTANGULAR:
			case AV_SPHERICAL_EQUIRECTANGULAR_TILE: {
				// A tile is a crop of the full sphere (VR180 is the middle half of the width): its bounds say how much.
				video.projection = "equirectangular";
				if (mapping->projection == AV_SPHERICAL_EQUIRECTANGULAR_TILE) {
					const double scale = 1.0 / 4294967296.0; // 0.32 fixed point.
					video.projection_bounds[0] = mapping->bound_left * scale;
					video.projection_bounds[1] = mapping->bound_top * scale;
					video.projection_bounds[2] = mapping->bound_right * scale;
					video.projection_bounds[3] = mapping->bound_bottom * scale;
				}
				video.projection_fov_h = 360.0 * (1.0 - video.projection_bounds[0] - video.projection_bounds[2]);
				video.projection_fov_v = 180.0 * (1.0 - video.projection_bounds[1] - video.projection_bounds[3]);
			} break;
			case AV_SPHERICAL_HALF_EQUIRECTANGULAR:
				video.projection = "half_equirectangular";
				video.projection_fov_h = 180.0;
				video.projection_fov_v = 180.0;
				break;
			case AV_SPHERICAL_CUBEMAP:
				video.projection = "cubemap";
				video.projection_fov_h = 360.0;
				video.projection_fov_v = 180.0;
				break;
			case AV_SPHERICAL_FISHEYE:
				video.projection = "fisheye";
				break;
			case AV_SPHERICAL_RECTILINEAR:
				video.projection = "rectilinear";
				break;
			case AV_SPHERICAL_PARAMETRIC_IMMERSIVE:
				video.projection = "parametric_immersive";
				break;
			default:
				video.projection = "other";
				break;
		}
	}
}

void MediaSource::_read_chapters() {
	chapters.clear();
	for (unsigned i = 0; i < format->nb_chapters; i++) {
		const AVChapter *source_chapter = format->chapters[i];
		Chapter chapter;
		const double unit = av_q2d(source_chapter->time_base);
		chapter.start = source_chapter->start * unit - start_time;
		chapter.end = source_chapter->end * unit - start_time;
		const AVDictionaryEntry *title = av_dict_get(source_chapter->metadata, "title", nullptr, 0);
		chapter.title = title ? String::utf8(title->value) : String();
		chapters.push_back(chapter);
	}
	struct ByStart {
		bool operator()(const Chapter &p_a, const Chapter &p_b) const { return p_a.start < p_b.start; }
	};
	chapters.sort_custom<ByStart>();
	// Some files give no end (or end = start): until the next chapter, or the end of the file.
	for (uint32_t i = 0; i < chapters.size(); i++) {
		if (chapters[i].end <= chapters[i].start) {
			chapters[i].end = i + 1 < chapters.size() ? chapters[i + 1].start : MAX(duration, chapters[i].start);
		}
	}
}

Array MediaSource::get_chapters() const {
	Array result;
	for (uint32_t i = 0; i < chapters.size(); i++) {
		Dictionary d;
		d["index"] = i;
		d["start"] = chapters[i].start;
		d["end"] = chapters[i].end;
		d["title"] = chapters[i].title;
		result.push_back(d);
	}
	return result;
}

int MediaSource::get_chapter_at(double p_seconds) const {
	int found = -1;
	for (uint32_t i = 0; i < chapters.size(); i++) {
		if (chapters[i].start <= p_seconds + 1e-3) {
			found = (int)i;
		}
	}
	if (found >= 0 && p_seconds >= chapters[found].end && found + 1 == (int)chapters.size()) {
		return -1; // Past the end of the last chapter.
	}
	return found;
}

/* Bitrate */

static uint32_t bitrate_slot(int64_t p_second, uint32_t p_count) {
	const int64_t slot = p_second % (int64_t)p_count;
	return (uint32_t)(slot < 0 ? slot + p_count : slot);
}

void MediaSource::_count_video_bytes_locked(double p_pts, uint32_t p_bytes) {
	const int64_t second = (int64_t)Math::floor(p_pts);
	BitrateSecond &entry = bitrate_seconds[bitrate_slot(second, BITRATE_SECONDS)];
	if (entry.second != second || entry.serial != serial) {
		entry.second = second;
		entry.serial = serial;
		entry.bytes = 0;
	}
	entry.bytes += p_bytes;
}

PackedFloat32Array MediaSource::get_video_bitrate_history(int p_seconds, double p_at) const {
	PackedFloat32Array history;
	const int count = CLAMP(p_seconds, 0, (int)BITRATE_SECONDS / 2);
	history.resize(count);
	float *out = history.ptrw();
	const int64_t last = (int64_t)Math::floor(p_at) - 1; // The last whole second before p_at.
	MutexLock lock(mutex);
	for (int i = 0; i < count; i++) {
		const int64_t second = last - (count - 1) + i;
		const BitrateSecond &entry = bitrate_seconds[bitrate_slot(second, BITRATE_SECONDS)];
		out[i] = entry.second == second && entry.serial == serial ? (float)(entry.bytes * 8 / 1000.0) : -1.0f;
	}
	return history;
}

/* Subtitle read-ahead */

uint32_t MediaSource::_subtitle_video_target() const {
	const double fps = video.fps > 0.0 ? video.fps : 30.0;
	const uint32_t wanted = (uint32_t)(MAX(video_queue_seconds, subtitle_read_ahead + 0.5) * fps);
	return CLAMP(wanted, MAX(video_queue_target, VIDEO_QUEUE_MIN), VIDEO_QUEUE_MAX - 8);
}

void MediaSource::set_subtitle_read_ahead(double p_seconds) {
	MutexLock lock(mutex);
	subtitle_read_ahead = CLAMP(p_seconds, 0.0, 30.0);
	subtitle_video_target = _subtitle_video_target();
	condition.notify_one();
}

double MediaSource::get_subtitle_read_ahead() const {
	MutexLock lock(mutex);
	return subtitle_read_ahead;
}

/* Subtitle files */

bool MediaSource::_decode_text_subtitle(AVCodecContext *p_codec, AVPacket *p_packet, double p_start, double p_duration, SubtitleEvent &r_event, bool &r_error) {
	r_error = false;
	AVSubtitle subtitle = {};
	int got = 0;
	if (avcodec_decode_subtitle2(p_codec, &subtitle, &got, p_packet) < 0) {
		r_error = true;
		return false;
	}
	if (!got) {
		return false;
	}
	r_event = SubtitleEvent();
	r_event.start = p_start + subtitle.start_display_time / 1000.0;
	if (p_duration > 0.0) {
		r_event.end = p_start + p_duration;
	} else if (subtitle.end_display_time > subtitle.start_display_time && subtitle.end_display_time != UINT32_MAX) {
		r_event.end = p_start + subtitle.end_display_time / 1000.0;
	}
	for (unsigned i = 0; i < subtitle.num_rects; i++) {
		const AVSubtitleRect *rect = subtitle.rects[i];
		String line;
		if (rect->ass) {
			line = String::utf8(rect->ass);
		} else if (rect->text) {
			line = String::utf8(rect->text);
		}
		if (line.is_empty()) {
			continue;
		}
		if (!r_event.ass.is_empty()) {
			r_event.ass += "\n";
			r_event.text += "\n";
		}
		r_event.ass += line;
		r_event.text += rect->ass ? _ass_to_text(line) : line;
	}
	avsubtitle_free(&subtitle);
	return !r_event.ass.is_empty();
}

static bool is_valid_utf8(const uint8_t *p_data, int64_t p_size) {
	int64_t i = 0;
	while (i < p_size) {
		const uint8_t c = p_data[i];
		int extra = 0;
		if (c < 0x80) {
			extra = 0;
		} else if ((c & 0xE0) == 0xC0 && c >= 0xC2) {
			extra = 1;
		} else if ((c & 0xF0) == 0xE0) {
			extra = 2;
		} else if ((c & 0xF8) == 0xF0 && c <= 0xF4) {
			extra = 3;
		} else {
			return false;
		}
		if (i + extra >= p_size && extra > 0) {
			return false; // Cut off at the end.
		}
		for (int k = 1; k <= extra; k++) {
			if ((p_data[i + k] & 0xC0) != 0x80) {
				return false;
			}
		}
		i += extra + 1;
	}
	return true;
}

// Subtitle files come as UTF-8 (with or without a BOM), UTF-16 with a BOM, or in an 8-bit Windows code page. Anything
// that isn't valid UTF-8 is read as Windows-1252 (Latin-1 with the 0x80-0x9F punctuation), the common case for western
// European files; other code pages come out wrong but readable in their ASCII parts.
String MediaSource::_subtitle_file_to_utf8(const PackedByteArray &p_bytes, String &r_charset) {
	const uint8_t *data = p_bytes.ptr();
	const int64_t size = p_bytes.size();
	String text;
	if (size >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) {
		r_charset = "UTF-8";
		if (is_valid_utf8(data + 3, size - 3)) {
			text.append_utf8((const char *)data + 3, (int)(size - 3));
			return text;
		}
		r_charset = "Windows-1252 (after a UTF-8 BOM)";
	} else if (size >= 2 && ((data[0] == 0xFF && data[1] == 0xFE) || (data[0] == 0xFE && data[1] == 0xFF))) {
		const bool little = data[0] == 0xFF;
		r_charset = little ? "UTF-16LE" : "UTF-16BE";
		LocalVector<char16_t> units;
		units.resize((uint32_t)((size - 2) / 2));
		for (uint32_t i = 0; i < units.size(); i++) {
			const uint8_t a = data[2 + 2 * i];
			const uint8_t b = data[3 + 2 * i];
			units[i] = little ? (char16_t)(a | (b << 8)) : (char16_t)((a << 8) | b);
		}
		text.append_utf16(units.ptr(), (int)units.size());
		return text;
	} else if (is_valid_utf8(data, size)) {
		r_charset = "UTF-8";
		text.append_utf8((const char *)data, (int)size);
		return text;
	} else {
		r_charset = "Windows-1252";
	}
	static const char32_t CP1252_80_9F[32] = {
		0x20AC, 0xFFFD, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0xFFFD, 0x017D, 0xFFFD,
		0xFFFD, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0xFFFD, 0x017E, 0x0178
	};
	LocalVector<char32_t> chars;
	chars.resize((uint32_t)size);
	for (int64_t i = 0; i < size; i++) {
		const uint8_t c = data[i];
		chars[(uint32_t)i] = (c >= 0x80 && c < 0xA0) ? CP1252_80_9F[c - 0x80] : (char32_t)c;
	}
	return String::utf32(Span<char32_t>(chars.ptr(), chars.size()));
}

Error MediaSource::_read_whole_file(const String &p_path, int64_t p_max_bytes, PackedByteArray &r_bytes, String &r_error) {
	if (SmbUrl::is_smb(p_path)) {
		SmbFile file;
		if (file.open(p_path, 4 << 20, 2, r_error) != OK) {
			return ERR_FILE_CANT_OPEN;
		}
		const int64_t size = file.get_size();
		if (size > p_max_bytes) {
			file.close();
			r_error = vformat("%s is %d bytes; subtitle files of up to %d bytes are read.", p_path, size, p_max_bytes);
			return ERR_FILE_CORRUPT;
		}
		r_bytes.resize(size);
		int64_t done = 0;
		while (done < size) {
			const int n = SmbFile::avio_read(&file, r_bytes.ptrw() + done, (int)MIN(size - done, (int64_t)(1 << 20)));
			if (n <= 0) {
				file.close();
				r_error = vformat("Reading %s failed after %d of %d bytes.", p_path, done, size);
				return ERR_FILE_CANT_READ;
			}
			done += n;
		}
		file.close();
		return OK;
	}
	const String global_path = ProjectSettings::get_singleton() ? ProjectSettings::get_singleton()->globalize_path(p_path) : p_path;
	Ref<FileAccess> file = FileAccess::open(global_path, FileAccess::READ);
	if (file.is_null()) {
		r_error = vformat("Can't open %s.", global_path);
		return ERR_FILE_CANT_OPEN;
	}
	const int64_t size = (int64_t)file->get_length();
	if (size > p_max_bytes) {
		r_error = vformat("%s is %d bytes; subtitle files of up to %d bytes are read.", global_path, size, p_max_bytes);
		return ERR_FILE_CORRUPT;
	}
	r_bytes.resize(size);
	if ((int64_t)file->get_buffer(r_bytes.ptrw(), size) != size) {
		r_error = vformat("Reading %s failed.", global_path);
		return ERR_FILE_CANT_READ;
	}
	return OK;
}

namespace {

struct MemoryReader {
	const uint8_t *data = nullptr;
	int64_t size = 0;
	int64_t position = 0;
};

int memory_read(void *p_opaque, uint8_t *p_buffer, int p_size) {
	MemoryReader *reader = (MemoryReader *)p_opaque;
	const int64_t n = MIN((int64_t)p_size, reader->size - reader->position);
	if (n <= 0) {
		return AVERROR_EOF;
	}
	memcpy(p_buffer, reader->data + reader->position, n);
	reader->position += n;
	return (int)n;
}

int64_t memory_seek(void *p_opaque, int64_t p_offset, int p_whence) {
	MemoryReader *reader = (MemoryReader *)p_opaque;
	if (p_whence & AVSEEK_SIZE) {
		return reader->size;
	}
	int64_t target = p_offset;
	switch (p_whence & ~AVSEEK_FORCE) {
		case SEEK_SET:
			break;
		case SEEK_CUR:
			target += reader->position;
			break;
		case SEEK_END:
			target += reader->size;
			break;
		default:
			return AVERROR(EINVAL);
	}
	if (target < 0 || target > reader->size) {
		return AVERROR(EINVAL);
	}
	reader->position = target;
	return target;
}

// Frees what add_external_subtitle() allocated, whichever way it leaves.
struct SubtitleFileContext {
	AVIOContext *avio = nullptr;
	AVFormatContext *format = nullptr;
	AVCodecContext *codec = nullptr;
	AVPacket *packet = nullptr;

	~SubtitleFileContext() {
		av_packet_free(&packet);
		avcodec_free_context(&codec);
		if (format) {
			avformat_close_input(&format);
		}
		if (avio) {
			av_freep(&avio->buffer);
			avio_context_free(&avio);
		}
	}
};

} // namespace

int MediaSource::add_external_subtitle(const String &p_path, const String &p_language, const String &p_title) {
	ERR_FAIL_COND_V_MSG(!is_open(), -1, "MediaSource: open the video before adding subtitle files.");
	auto fail = [this](const String &p_message) {
		{
			MutexLock lock(mutex);
			last_error = p_message;
		}
		WARN_PRINT("MediaSource: " + p_message);
		return -1;
	};

	PackedByteArray bytes;
	String error;
	if (_read_whole_file(p_path, SUBTITLE_FILE_MAX_BYTES, bytes, error) != OK) {
		return fail(error);
	}
	String charset;
	const CharString utf8 = _subtitle_file_to_utf8(bytes, charset).utf8();

	const String extension = p_path.get_extension().to_lower();
	const char *demuxer_name = extension == "srt" ? "srt" : (extension == "ass" || extension == "ssa" ? "ass" : (extension == "vtt" ? "webvtt" : nullptr));
	const AVInputFormat *input_format = demuxer_name ? av_find_input_format(demuxer_name) : nullptr;

	MemoryReader reader;
	reader.data = (const uint8_t *)utf8.get_data();
	reader.size = utf8.length();
	SubtitleFileContext context;
	uint8_t *buffer = (uint8_t *)av_malloc(64 << 10);
	context.avio = buffer ? avio_alloc_context(buffer, 64 << 10, 0, &reader, &memory_read, nullptr, &memory_seek) : nullptr;
	if (!context.avio) {
		av_free(buffer);
		return fail("Out of memory.");
	}
	context.format = avformat_alloc_context();
	if (!context.format) {
		return fail("Out of memory.");
	}
	context.format->pb = context.avio;
	context.format->flags |= AVFMT_FLAG_CUSTOM_IO;
	int r = avformat_open_input(&context.format, p_path.get_file().utf8().get_data(), input_format, nullptr);
	if (r < 0) {
		context.format = nullptr; // Freed by avformat_open_input().
		return fail(vformat("%s isn't a subtitle file that can be read (SubRip, ASS/SSA, WebVTT): %s.", p_path, _av_error(r)));
	}
	int stream_index = -1;
	for (unsigned i = 0; i < context.format->nb_streams; i++) {
		if (context.format->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE) {
			stream_index = (int)i;
			break;
		}
	}
	if (stream_index < 0) {
		return fail(vformat("%s has no subtitle stream.", p_path));
	}
	const AVStream *stream = context.format->streams[stream_index];
	const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
	if (!codec || !_is_text_subtitle(stream->codecpar->codec_id)) {
		return fail(vformat("%s: no decoder for %s subtitles.", p_path, avcodec_get_name(stream->codecpar->codec_id)));
	}
	context.codec = avcodec_alloc_context3(codec);
	r = context.codec ? avcodec_parameters_to_context(context.codec, stream->codecpar) : AVERROR(ENOMEM);
	if (r >= 0) {
		context.codec->pkt_timebase = stream->time_base;
		r = avcodec_open2(context.codec, codec, nullptr);
	}
	if (r < 0) {
		return fail(vformat("%s: can't open the %s decoder: %s.", p_path, codec->name, _av_error(r)));
	}

	ExternalSubtitle external;
	if (context.codec->subtitle_header && context.codec->subtitle_header_size > 0) {
		external.header = String::utf8((const char *)context.codec->subtitle_header, context.codec->subtitle_header_size);
	}
	context.packet = av_packet_alloc();
	uint32_t decode_errors = 0;
	// The file's times are playback times: they line up with the video's media seconds as they are.
	while (context.packet && av_read_frame(context.format, context.packet) >= 0) {
		if (context.packet->stream_index == stream_index) {
			const int64_t ts = context.packet->pts != AV_NOPTS_VALUE ? context.packet->pts : context.packet->dts;
			if (ts != AV_NOPTS_VALUE) {
				const double start = ts * av_q2d(stream->time_base);
				const double length = context.packet->duration > 0 ? context.packet->duration * av_q2d(stream->time_base) : -1.0;
				SubtitleEvent event;
				bool decode_error = false;
				if (_decode_text_subtitle(context.codec, context.packet, start, length, event, decode_error)) {
					external.events.push_back(event);
				} else if (decode_error) {
					decode_errors++;
				}
			}
		}
		av_packet_unref(context.packet);
	}
	struct ByStart {
		bool operator()(const SubtitleEvent &p_a, const SubtitleEvent &p_b) const { return p_a.start < p_b.start; }
	};
	external.events.sort_custom<ByStart>();

	const MediaLanguages::SubtitleFileName name = MediaLanguages::parse_subtitle_file_name(path.get_file(), p_path.get_file());
	SubtitleTrack &track = external.track;
	track.stream_index = -1;
	track.codec = String::utf8(avcodec_get_name(stream->codecpar->codec_id)); // As for the file's own tracks.
	track.text = true;
	track.language = p_language.is_empty() ? name.language : MediaLanguages::normalize(p_language);
	track.title = p_title.is_empty() ? p_path.get_file() : p_title;
	track.forced = name.forced;
	track.hearing_impaired = name.hearing_impaired;
	track.path = p_path;
	const uint32_t event_count = external.events.size();

	int index;
	{
		MutexLock lock(mutex);
		index = (int)(subtitle_tracks.size() + external_subtitles.size());
		external_subtitles.push_back(external);
	}
	print_line(vformat("MediaSource: subtitle file %s (%s, %s): %d events%s, track %d, language \"%s\"%s%s.", p_path, track.codec, charset, event_count,
			decode_errors ? vformat(", %d decode errors", decode_errors) : String(), index, track.language, track.forced ? ", forced" : "", track.hearing_impaired ? ", SDH" : ""));
	return index;
}

int MediaSource::get_external_subtitle_count() const {
	MutexLock lock(mutex);
	return (int)external_subtitles.size();
}

Dictionary MediaSource::_subtitle_track_to_dictionary(const SubtitleTrack &p_track, int p_index, bool p_external) {
	Dictionary d;
	d["index"] = p_index;
	d["stream_index"] = p_track.stream_index;
	d["codec"] = p_track.codec;
	d["text"] = p_track.text;
	d["bitmap"] = p_track.bitmap;
	d["language"] = p_track.language;
	d["language_code"] = MediaLanguages::normalize(p_track.language);
	d["title"] = p_track.title;
	d["default"] = p_track.is_default;
	d["forced"] = p_track.forced;
	d["hearing_impaired"] = p_track.hearing_impaired;
	d["external"] = p_external;
	if (p_external) {
		d["path"] = p_track.path;
	}
	return d;
}

/* Language memory */

String MediaSource::normalize_language(const String &p_code) {
	return MediaLanguages::normalize(p_code);
}

String MediaSource::language_name(const String &p_code) {
	return MediaLanguages::name(MediaLanguages::normalize(p_code));
}

static int preference_rank(const String &p_language, const PackedStringArray &p_languages) {
	const String code = MediaLanguages::normalize(p_language);
	for (int i = 0; i < p_languages.size(); i++) {
		if (MediaLanguages::matches(code, MediaLanguages::normalize(p_languages[i]))) {
			return i;
		}
	}
	return -1;
}

int MediaSource::pick_audio_track(const PackedStringArray &p_languages) const {
	// Best: the earliest preferred language; then not a commentary, not audio description, the default flag, more channels.
	int best = -1;
	int64_t best_score = INT64_MAX;
	for (uint32_t i = 0; i < audio_tracks.size(); i++) {
		const AudioTrack &track = audio_tracks[i];
		const int rank = preference_rank(track.language, p_languages);
		if (rank < 0) {
			continue;
		}
		const int64_t score = (int64_t)rank * 100000 + (track.commentary ? 10000 : 0) + (track.visual_impaired ? 1000 : 0) + (track.is_default ? 0 : 100) + (99 - MIN(track.channels, 99));
		if (score < best_score) {
			best_score = score;
			best = (int)i;
		}
	}
	return best;
}

int MediaSource::pick_subtitle_track(const PackedStringArray &p_languages, bool p_forced_only) const {
	// Best: the earliest preferred language; then text before bitmap (only text is drawn so far), full before forced
	// (unless p_forced_only), without before with hearing-impaired notes, the default flag, the file's own before a file.
	int best = -1;
	int64_t best_score = INT64_MAX;
	auto consider = [&](const SubtitleTrack &p_track, int p_index, bool p_external) {
		if (p_forced_only && !p_track.forced) {
			return;
		}
		const int rank = preference_rank(p_track.language, p_languages);
		if (rank < 0) {
			return;
		}
		const int64_t score = (int64_t)rank * 100000 + (p_track.text ? 0 : 10000) + (p_track.forced && !p_forced_only ? 1000 : 0) + (p_track.hearing_impaired ? 100 : 0) + (p_track.is_default ? 0 : 10) + (p_external ? 1 : 0);
		if (score < best_score) {
			best_score = score;
			best = p_index;
		}
	};
	for (uint32_t i = 0; i < subtitle_tracks.size(); i++) {
		consider(subtitle_tracks[i], (int)i, false);
	}
	MutexLock lock(mutex);
	for (uint32_t i = 0; i < external_subtitles.size(); i++) {
		consider(external_subtitles[i].track, (int)(subtitle_tracks.size() + i), true);
	}
	return best;
}
