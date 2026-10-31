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
#include "net_stream.h"
#include "smb_client.h"

#include "core/config/project_settings.h"
#include "core/io/file_access.h"
#include "core/os/os.h"

#include <climits>
#include <cmath>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/dovi_meta.h>
#include <libavutil/mastering_display_metadata.h>
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

// A bitmap subtitle packet (PGS, VobSub, DVB, XSUB) as RGBA images. The palette entries are 0xAARRGGBB.
bool MediaSource::_decode_bitmap_subtitle(AVPacket *p_packet, double p_start, double p_duration, SubtitleEvent &r_event, bool &r_error) {
	r_error = false;
	AVSubtitle subtitle = {};
	int got = 0;
	if (avcodec_decode_subtitle2(subtitle_codec, &subtitle, &got, p_packet) < 0) {
		r_error = true;
		return false;
	}
	if (!got) {
		return false;
	}
	r_event = SubtitleEvent();
	r_event.bitmap = true;
	r_event.start = p_start + subtitle.start_display_time / 1000.0;
	if (subtitle.end_display_time > subtitle.start_display_time && subtitle.end_display_time != UINT32_MAX) {
		r_event.end = p_start + subtitle.end_display_time / 1000.0;
	} else if (p_duration > 0.0) {
		r_event.end = p_start + p_duration;
	}
	r_event.canvas = Size2i(subtitle_codec->width, subtitle_codec->height);
	for (unsigned i = 0; i < subtitle.num_rects; i++) {
		const AVSubtitleRect *rect = subtitle.rects[i];
		if (rect->type != SUBTITLE_BITMAP || rect->w <= 0 || rect->h <= 0 || !rect->data[0] || !rect->data[1]) {
			continue;
		}
		PackedByteArray rgba;
		rgba.resize((int64_t)rect->w * rect->h * 4);
		uint8_t *out = rgba.ptrw();
		const uint32_t *palette = (const uint32_t *)rect->data[1];
		const int colors = rect->nb_colors > 0 ? MIN(rect->nb_colors, 256) : 256;
		for (int y = 0; y < rect->h; y++) {
			const uint8_t *row = rect->data[0] + (int64_t)y * rect->linesize[0];
			for (int x = 0; x < rect->w; x++) {
				const uint32_t p = row[x] < colors ? palette[row[x]] : 0;
				out[0] = (p >> 16) & 0xFF;
				out[1] = (p >> 8) & 0xFF;
				out[2] = p & 0xFF;
				out[3] = p >> 24;
				out += 4;
			}
		}
		SubtitleBitmap bitmap;
		bitmap.rect = Rect2i(rect->x, rect->y, rect->w, rect->h);
		bitmap.image = Image::create_from_data(rect->w, rect->h, false, Image::FORMAT_RGBA8, rgba);
		bitmap.forced = (rect->flags & AV_SUBTITLE_FLAG_FORCED) != 0;
		r_event.bitmaps.push_back(bitmap);
	}
	avsubtitle_free(&subtitle);
	return true; // No pictures: the screen is cleared from this time on.
}

Dictionary MediaSource::parse_ass_event(const String &p_line, const String &p_header) {
	return SubtitleStyle::parse_event(p_line, SubtitleStyle::parse_header(p_header));
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
			r_error = vformat("%s is %d bytes; files of up to %d bytes are read.", p_path, size, p_max_bytes);
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
		r_error = vformat("%s is %d bytes; files of up to %d bytes are read.", global_path, size, p_max_bytes);
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
	d["bitmap_decoded"] = p_track.bitmap_decoded;
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

/* HDR */

// Container side data: Matroska's Colour element and MP4's mdcv/clli boxes, and Dolby Vision's configuration record.
void MediaSource::_read_hdr_side_data(const AVStream *p_stream) {
	const AVCodecParameters *par = p_stream->codecpar;
	const AVPacketSideData *data = av_packet_side_data_get(par->coded_side_data, par->nb_coded_side_data, AV_PKT_DATA_MASTERING_DISPLAY_METADATA);
	if (data && data->size >= (size_t)sizeof(AVMasteringDisplayMetadata)) {
		const AVMasteringDisplayMetadata *m = (const AVMasteringDisplayMetadata *)data->data;
		if (m->has_primaries) {
			video.mastering_primaries = true;
			for (int i = 0; i < 3; i++) {
				video.mastering_display[i * 2] = av_q2d(m->display_primaries[i][0]);
				video.mastering_display[i * 2 + 1] = av_q2d(m->display_primaries[i][1]);
			}
			video.mastering_display[6] = av_q2d(m->white_point[0]);
			video.mastering_display[7] = av_q2d(m->white_point[1]);
		}
		if (m->has_luminance) {
			video.mastering_luminance = true;
			video.mastering_min_nits = av_q2d(m->min_luminance);
			video.mastering_max_nits = av_q2d(m->max_luminance);
		}
		video.hdr_metadata_source = "container";
	}
	data = av_packet_side_data_get(par->coded_side_data, par->nb_coded_side_data, AV_PKT_DATA_CONTENT_LIGHT_LEVEL);
	if (data && data->size >= (size_t)sizeof(AVContentLightMetadata)) {
		const AVContentLightMetadata *c = (const AVContentLightMetadata *)data->data;
		video.light_level = true;
		video.max_cll = (int)c->MaxCLL;
		video.max_fall = (int)c->MaxFALL;
		video.hdr_metadata_source = "container";
	}
	data = av_packet_side_data_get(par->coded_side_data, par->nb_coded_side_data, AV_PKT_DATA_DOVI_CONF);
	if (data && data->size >= (size_t)sizeof(AVDOVIDecoderConfigurationRecord)) {
		video.dolby_vision_profile = ((const AVDOVIDecoderConfigurationRecord *)data->data)->dv_profile;
	}
}

// The HDR SEI messages of an Annex-B access unit (H.264 or HEVC), before its first picture: mastering display colour
// volume (137), content light level (144), and HDR10+ (ITU-T T.35, 4). Fills only what the container didn't give.
void MediaSource::_parse_hdr_sei(const uint8_t *p_data, uint64_t p_size, bool p_hevc) {
	const auto find_start_code = [p_data, p_size](uint64_t p_from) -> uint64_t {
		for (uint64_t k = p_from; k + 2 < p_size; k++) {
			if (p_data[k + 2] > 1) {
				k += 2;
			} else if (p_data[k] == 0 && p_data[k + 1] == 0 && p_data[k + 2] == 1) {
				return k;
			}
		}
		return p_size;
	};
	const bool had_mastering = video.mastering_primaries || video.mastering_luminance;
	const bool had_light = video.light_level;
	bool found = false;
	uint8_t rbsp[1024]; // SEI before a picture is small; longer messages (HDR10+ curves) are only looked at in front.
	uint64_t start_code = find_start_code(0);
	while (start_code < p_size) {
		const uint64_t nal = start_code + 3;
		if (nal >= p_size) {
			break;
		}
		const uint32_t type = p_hevc ? (p_data[nal] >> 1) & 0x3f : p_data[nal] & 0x1f;
		const bool vcl = p_hevc ? type < 32 : (type >= 1 && type <= 5);
		if (vcl) {
			break;
		}
		const uint64_t next = find_start_code(nal);
		const bool sei = p_hevc ? type == 39 : type == 6;
		if (sei) {
			// Unescape (00 00 03 -> 00 00) past the NAL header.
			uint32_t n = 0;
			int zeros = 0;
			for (uint64_t k = nal + (p_hevc ? 2 : 1); k < next && n < sizeof(rbsp); k++) {
				const uint8_t b = p_data[k];
				if (zeros >= 2 && b == 3) {
					zeros = 0;
					continue;
				}
				zeros = b == 0 ? zeros + 1 : 0;
				rbsp[n++] = b;
			}
			uint32_t at = 0;
			while (at + 2 <= n && rbsp[at] != 0x80) {
				uint32_t payload_type = 0;
				while (at < n && rbsp[at] == 0xff) {
					payload_type += 255;
					at++;
				}
				payload_type += at < n ? rbsp[at++] : 0;
				uint32_t payload_size = 0;
				while (at < n && rbsp[at] == 0xff) {
					payload_size += 255;
					at++;
				}
				payload_size += at < n ? rbsp[at++] : 0;
				const uint8_t *p = rbsp + at;
				const uint32_t available = at <= n ? n - at : 0;
				const auto u16 = [p](int i) { return (uint32_t)((p[i] << 8) | p[i + 1]); };
				const auto u32 = [p](int i) { return ((uint32_t)p[i] << 24) | ((uint32_t)p[i + 1] << 16) | ((uint32_t)p[i + 2] << 8) | p[i + 3]; };
				if (payload_type == 137 && payload_size >= 24 && available >= 24 && !had_mastering) {
					// Green, blue, red in the SEI; units of 0.00002 and 0.0001 cd/m2.
					static const int slot[3] = { 1, 2, 0 }; // SEI index -> R, G, B slot.
					for (int i = 0; i < 3; i++) {
						video.mastering_display[slot[i] * 2] = u16(i * 4) * 0.00002;
						video.mastering_display[slot[i] * 2 + 1] = u16(i * 4 + 2) * 0.00002;
					}
					video.mastering_display[6] = u16(12) * 0.00002;
					video.mastering_display[7] = u16(14) * 0.00002;
					video.mastering_max_nits = u32(16) * 0.0001;
					video.mastering_min_nits = u32(20) * 0.0001;
					video.mastering_primaries = true;
					video.mastering_luminance = true;
					found = true;
				} else if (payload_type == 144 && payload_size >= 4 && available >= 4 && !had_light) {
					video.max_cll = (int)u16(0);
					video.max_fall = (int)u16(2);
					video.light_level = true;
					found = true;
				} else if (payload_type == 4 && payload_size >= 6 && available >= 6) {
					// ITU-T T.35: United States (0xB5), Samsung (0x003C), oriented code 1, application 4 = HDR10+.
					if (p[0] == 0xb5 && u16(1) == 0x003c && u16(3) == 0x0001 && p[5] == 4) {
						video.hdr10plus = true;
					}
				}
				at += payload_size;
			}
		}
		start_code = next;
	}
	if (found) {
		video.hdr_metadata_source = video.hdr_metadata_source.is_empty() ? String("sei") : video.hdr_metadata_source + "+sei";
	}
}

namespace {

// Linear RGB with these primaries and white point (x, y of R, G, B, white) to CIE XYZ.
Basis rgb_to_xyz(const double *p_xy) {
	Vector3 c[3];
	for (int i = 0; i < 3; i++) {
		const double x = p_xy[i * 2];
		const double y = p_xy[i * 2 + 1];
		c[i] = Vector3(x / y, 1.0, (1.0 - x - y) / y);
	}
	const Basis m(c[0].x, c[1].x, c[2].x, c[0].y, c[1].y, c[2].y, c[0].z, c[1].z, c[2].z);
	const double wx = p_xy[6];
	const double wy = p_xy[7];
	const Vector3 s = m.inverse().xform(Vector3(wx / wy, 1.0, (1.0 - wx - wy) / wy));
	return m * Basis::from_scale(s);
}

const double BT709_XY[8] = { 0.640, 0.330, 0.300, 0.600, 0.150, 0.060, 0.3127, 0.3290 };
const double BT2020_XY[8] = { 0.708, 0.292, 0.170, 0.797, 0.131, 0.046, 0.3127, 0.3290 };
const double P3_D65_XY[8] = { 0.680, 0.320, 0.265, 0.690, 0.150, 0.060, 0.3127, 0.3290 };

// SMPTE ST 2084.
constexpr double PQ_M1 = 2610.0 / 16384.0;
constexpr double PQ_M2 = 2523.0 / 4096.0 * 128.0;
constexpr double PQ_C1 = 3424.0 / 4096.0;
constexpr double PQ_C2 = 2413.0 / 4096.0 * 32.0;
constexpr double PQ_C3 = 2392.0 / 4096.0 * 32.0;
// BT.2100 HLG.
constexpr double HLG_A = 0.17883277;
constexpr double HLG_B = 0.28466892; // 1 - 4a
constexpr double HLG_C = 0.55991073; // 0.5 - a ln(4a)

double pq_to_nits(double p_e) {
	const double e = Math::pow(CLAMP(p_e, 0.0, 1.0), 1.0 / PQ_M2);
	return 10000.0 * Math::pow(MAX(e - PQ_C1, 0.0) / (PQ_C2 - PQ_C3 * e), 1.0 / PQ_M1);
}

double nits_to_pq(double p_nits) {
	const double y = Math::pow(CLAMP(p_nits / 10000.0, 0.0, 1.0), PQ_M1);
	return Math::pow((PQ_C1 + PQ_C2 * y) / (1.0 + PQ_C3 * y), PQ_M2);
}

double hlg_to_scene(double p_e) {
	const double e = CLAMP(p_e, 0.0, 1.0);
	return e <= 0.5 ? e * e / 3.0 : (Math::exp((e - HLG_C) / HLG_A) + HLG_B) / 12.0;
}

} // namespace

Dictionary MediaSource::get_tone_mapping(const Dictionary &p_video_info, double p_target_nits) {
	Dictionary d;
	const String transfer = p_video_info.get("color_transfer", String());
	const String primaries = p_video_info.get("color_primaries", String());
	const String kind = transfer == "smpte2084" ? "pq" : (transfer == "arib-std-b67" ? "hlg" : "sdr");
	d["transfer"] = kind;
	// Linear light in the video's primaries to linear BT.709 (out-of-gamut colours come out negative: clamp them).
	const double *source = primaries == "bt2020" ? BT2020_XY : (primaries == "smpte432" ? P3_D65_XY : nullptr);
	d["gamut_matrix"] = source ? rgb_to_xyz(BT709_XY).inverse() * rgb_to_xyz(source) : Basis();
	d["primaries_bt709"] = source == nullptr;
	d["pq_m1"] = PQ_M1;
	d["pq_m2"] = PQ_M2;
	d["pq_c1"] = PQ_C1;
	d["pq_c2"] = PQ_C2;
	d["pq_c3"] = PQ_C3;
	d["hlg_a"] = HLG_A;
	d["hlg_b"] = HLG_B;
	d["hlg_c"] = HLG_C;
	// The brightest the video gets: MaxCLL, else the mastering display's peak, else 1000 nits (the usual grade); never
	// above the mastering display's peak. HLG is shown as on a 1000 nit display (system gamma 1.2), then mapped like PQ.
	const double target = CLAMP(p_target_nits, 48.0, 10000.0);
	double peak = 1000.0;
	double black = 0.0;
	if (kind == "pq") {
		const int max_cll = p_video_info.get("max_cll", 0);
		const Vector2 mastering = p_video_info.get("mastering_display_luminance", Vector2());
		if (max_cll > 0) {
			peak = max_cll;
		} else if (mastering.y > 0.0f) {
			peak = mastering.y;
		}
		if (mastering.y > 0.0f) {
			peak = MIN(peak, (double)mastering.y);
			black = mastering.x;
		}
		peak = MAX(peak, 100.0);
	}
	d["hlg_system_gamma"] = 1.2;
	d["hlg_peak_nits"] = 1000.0;
	d["source_peak_nits"] = peak;
	d["source_black_nits"] = black;
	d["target_peak_nits"] = target;
	// BT.2390 EETF, on PQ values normalised to the source's range: E = (pq - source_min_pq) / (source_max_pq -
	// source_min_pq). Below knee_start E is kept; above, a Hermite spline bends it to max_luminance (the target peak) at
	// E = 1. tone_mapping false: the source fits the target, nothing to compress.
	const double source_min_pq = nits_to_pq(black);
	const double source_max_pq = nits_to_pq(peak);
	const double max_luminance = CLAMP((nits_to_pq(target) - source_min_pq) / (source_max_pq - source_min_pq), 0.0, 1.0);
	d["source_min_pq"] = source_min_pq;
	d["source_max_pq"] = source_max_pq;
	d["max_luminance"] = max_luminance;
	d["knee_start"] = 1.5 * max_luminance - 0.5;
	d["tone_mapping"] = kind != "sdr" && peak > target;
	return d;
}

Color MediaSource::tone_map_color(const Color &p_rgb, const Dictionary &p_tone_mapping) {
	const String kind = p_tone_mapping.get("transfer", String("sdr"));
	const double target = p_tone_mapping.get("target_peak_nits", 203.0);
	Vector3 nits;
	if (kind == "pq") {
		nits = Vector3(pq_to_nits(p_rgb.r), pq_to_nits(p_rgb.g), pq_to_nits(p_rgb.b));
	} else if (kind == "hlg") {
		// Scene light, then the OOTF of a 1000 nit display: Y^(gamma - 1), Y the BT.2020 luminance.
		const Vector3 scene(hlg_to_scene(p_rgb.r), hlg_to_scene(p_rgb.g), hlg_to_scene(p_rgb.b));
		const double y = 0.2627 * scene.x + 0.6780 * scene.y + 0.0593 * scene.z;
		const double gamma = p_tone_mapping.get("hlg_system_gamma", 1.2);
		nits = scene * (1000.0 * Math::pow(MAX(y, 0.0), gamma - 1.0));
	} else {
		// SDR: BT.1886 (gamma 2.4) at the target's peak; only the gamut matrix applies.
		nits = Vector3(Math::pow(MAX((double)p_rgb.r, 0.0), 2.4), Math::pow(MAX((double)p_rgb.g, 0.0), 2.4), Math::pow(MAX((double)p_rgb.b, 0.0), 2.4)) * target;
	}
	const Basis gamut = p_tone_mapping.get("gamut_matrix", Basis());
	nits = gamut.xform(nits);
	nits = Vector3(MAX(nits.x, (real_t)0.0), MAX(nits.y, (real_t)0.0), MAX(nits.z, (real_t)0.0));
	// The EETF on the brightest channel, applied as a ratio to all three (keeps the hue).
	const double brightest = MAX(nits.x, MAX(nits.y, nits.z));
	if (bool(p_tone_mapping.get("tone_mapping", false)) && brightest > 0.0) {
		const double min_pq = p_tone_mapping["source_min_pq"];
		const double max_pq = p_tone_mapping["source_max_pq"];
		const double max_luminance = p_tone_mapping["max_luminance"];
		const double knee = p_tone_mapping["knee_start"];
		double e = CLAMP((nits_to_pq(brightest) - min_pq) / (max_pq - min_pq), 0.0, 1.0);
		if (e > knee) {
			const double t = (e - knee) / (1.0 - knee);
			const double t2 = t * t;
			const double t3 = t2 * t;
			e = (2.0 * t3 - 3.0 * t2 + 1.0) * knee + (t3 - 2.0 * t2 + t) * (1.0 - knee) + (-2.0 * t3 + 3.0 * t2) * max_luminance;
		}
		nits *= pq_to_nits(e * (max_pq - min_pq) + min_pq) / brightest;
	}
	nits /= target;
	return Color(nits.x, nits.y, nits.z, p_rgb.a);
}

/* Music files */

Dictionary MediaSource::get_tags() const {
	Dictionary tags;
	if (format == nullptr) {
		return tags;
	}
	// The container's tags, then the audio stream's (Ogg keeps its Vorbis comments there); keys in lower case.
	const auto add = [&tags](const AVDictionary *p_metadata) {
		const AVDictionaryEntry *entry = nullptr;
		while ((entry = av_dict_iterate(p_metadata, entry))) {
			const String key = String::utf8(entry->key).to_lower();
			if (!tags.has(key) && key != "handler_name" && key != "vendor_id" && key != "encoder" && key != "language" && key != "major_brand" && key != "minor_version" &&
					key != "compatible_brands") {
				tags[key] = String::utf8(entry->value);
			}
		}
	};
	add(format->metadata);
	if (!audio_tracks.is_empty()) {
		add(format->streams[audio_tracks[0].stream_index]->metadata);
	}
	return tags;
}

static Ref<Image> decode_picture(const PackedByteArray &p_data, const String &p_codec) {
	Ref<Image> image;
	image.instantiate();
	const uint8_t *b = p_data.ptr();
	const int64_t n = p_data.size();
	Error error = ERR_FILE_UNRECOGNIZED;
	if (p_codec == "png" || (n > 4 && b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G')) {
		error = image->load_png_from_buffer(p_data);
	} else if (p_codec == "mjpeg" || (n > 3 && b[0] == 0xff && b[1] == 0xd8 && b[2] == 0xff)) {
		error = image->load_jpg_from_buffer(p_data);
	} else if (p_codec == "webp" || (n > 12 && b[0] == 'R' && b[1] == 'I' && b[2] == 'F' && b[3] == 'F' && b[8] == 'W' && b[9] == 'E')) {
		error = image->load_webp_from_buffer(p_data);
	} else if (p_codec == "bmp" || (n > 2 && b[0] == 'B' && b[1] == 'M')) {
		error = image->load_bmp_from_buffer(p_data);
	}
	return error == OK ? image : Ref<Image>();
}

Array MediaSource::get_attached_pictures() const {
	Array result;
	for (const AttachedPicture &picture : pictures) {
		Dictionary d;
		d["codec"] = picture.codec;
		d["title"] = picture.title;
		d["data"] = picture.data;
		const Ref<Image> image = decode_picture(picture.data, picture.codec);
		d["image"] = image;
		d["size"] = image.is_valid() ? image->get_size() : Vector2i();
		result.push_back(d);
	}
	return result;
}

// The front cover if one is marked so, else the first picture that decodes.
Ref<Image> MediaSource::get_album_art() const {
	int first = -1;
	for (uint32_t i = 0; i < pictures.size(); i++) {
		const String title = pictures[i].title.to_lower();
		if (title.contains("front") || title.begins_with("cover")) {
			first = (int)i;
			break;
		}
	}
	if (first >= 0) {
		const Ref<Image> image = decode_picture(pictures[first].data, pictures[first].codec);
		if (image.is_valid()) {
			return image;
		}
	}
	for (const AttachedPicture &picture : pictures) {
		const Ref<Image> image = decode_picture(picture.data, picture.codec);
		if (image.is_valid()) {
			return image;
		}
	}
	return Ref<Image>();
}

/* Audio without a player */

void MediaSource::set_external_clock(double p_seconds, int64_t p_ticks_usec) {
	MutexLock lock(mutex);
	external_seconds = p_seconds;
	external_ticks_usec = p_ticks_usec;
}

PackedVector2Array MediaSource::read_audio_frames(int p_count) {
	PackedVector2Array out;
	if (p_count <= 0 || !is_open()) {
		return out;
	}
	double clock = -1.0;
	{
		MutexLock lock(mutex);
		if (pull_serial != serial) {
			// After a seek: no frame from before it in the interpolation.
			pull_serial = serial;
			pull_phase = 0.0;
			pull_last = AudioFrame(0, 0);
		}
		if (external_seconds >= 0.0) {
			clock = external_seconds + (double)((int64_t)OS::get_singleton()->get_ticks_usec() - external_ticks_usec) / 1e6;
		}
	}
	double ratio = 1.0;
	if (clock >= 0.0) {
		// Ahead of the clock: take input slower; behind: faster.
		external_error = get_audio_position() - clock;
		ratio = 1.0 - CLAMP(external_error * EXTERNAL_GAIN, -EXTERNAL_MAX_SLEW, EXTERNAL_MAX_SLEW);
	} else {
		external_error = 0.0;
	}
	external_ratio = ratio;
	// Output frame i is input position pull_phase + i x ratio, interpolated; input 0 is the last frame of the call before.
	const double end = pull_phase + p_count * ratio;
	const int need = (int)Math::floor(end);
	if (pull_scratch.size() < (uint32_t)need + 1) {
		pull_scratch.resize(need + 1);
	}
	pull_scratch[0] = pull_last;
	if (need > 0) {
		mix_audio(pull_scratch.ptr() + 1, need);
	}
	out.resize(p_count);
	Vector2 *w = out.ptrw();
	for (int i = 0; i < p_count; i++) {
		const double position = pull_phase + i * ratio;
		const int index = MIN((int)position, need);
		const float t = (float)(position - index);
		const AudioFrame &a = pull_scratch[index];
		const AudioFrame &b = pull_scratch[MIN(index + 1, need)];
		w[i] = Vector2(a.left + (b.left - a.left) * t, a.right + (b.right - a.right) * t);
	}
	pull_last = pull_scratch[need];
	pull_phase = end - need;
	return out;
}

/* Whole subtitle tracks */

Error MediaSource::_read_subtitle_stream(const String &p_path, int p_stream, double p_start_time, uint32_t p_generation, LocalVector<SubtitleEvent> &r_events, String &r_error) const {
	const bool smb = SmbUrl::is_smb(p_path);
	const bool web = NetStream::is_url(p_path);
	SmbFile *side_smb = nullptr;
	NetStream *side_net = nullptr;
	AVIOContext *side_avio = nullptr;
	AVFormatContext *context = nullptr;
	AVCodecContext *codec = nullptr;
	AVPacket *packet = nullptr;
	Error result = OK;
	const auto fail = [&](Error p_error, const String &p_message) {
		result = p_error;
		r_error = p_message;
	};

	if (smb || web) {
		void *opaque;
		int (*read_function)(void *, uint8_t *, int);
		int64_t (*seek_function)(void *, int64_t, int);
		if (smb) {
			side_smb = memnew(SmbFile);
			if (side_smb->open(p_path, 8 << 20, 4, r_error) != OK) {
				result = ERR_FILE_CANT_OPEN;
			}
			opaque = side_smb;
			read_function = &SmbFile::avio_read;
			seek_function = &SmbFile::avio_seek;
		} else {
			side_net = memnew(NetStream);
			if (side_net->open(p_path, 16 << 20, network_tls_verify, r_error) != OK) {
				result = ERR_FILE_CANT_OPEN;
			} else if (side_net->is_live()) {
				fail(ERR_UNAVAILABLE, "A live stream has no whole subtitle track to read.");
			}
			opaque = side_net;
			read_function = &NetStream::avio_read;
			seek_function = &NetStream::avio_seek;
		}
		if (result == OK) {
			uint8_t *buffer = (uint8_t *)av_malloc(256 << 10);
			side_avio = buffer ? avio_alloc_context(buffer, 256 << 10, 0, opaque, read_function, nullptr, seek_function) : nullptr;
			context = side_avio ? avformat_alloc_context() : nullptr;
			if (context == nullptr) {
				if (side_avio == nullptr) {
					av_free(buffer);
				}
				fail(ERR_OUT_OF_MEMORY, "Out of memory.");
			} else {
				context->pb = side_avio;
				context->flags |= AVFMT_FLAG_CUSTOM_IO;
			}
		}
	}
	const String global_path = smb || web || !ProjectSettings::get_singleton() ? p_path : ProjectSettings::get_singleton()->globalize_path(p_path);
	if (result == OK) {
		const int r = avformat_open_input(&context, global_path.utf8().get_data(), nullptr, nullptr);
		if (r < 0) {
			context = nullptr; // Freed by avformat_open_input().
			fail(ERR_FILE_CANT_OPEN, vformat("Can't open %s: %s.", global_path, _av_error(r)));
		} else if ((int)context->nb_streams <= p_stream && avformat_find_stream_info(context, nullptr) < 0) {
			fail(ERR_FILE_UNRECOGNIZED, vformat("Can't read the streams of %s.", global_path));
		} else if ((int)context->nb_streams <= p_stream) {
			fail(ERR_FILE_UNRECOGNIZED, vformat("%s has no stream %d.", global_path, p_stream));
		}
	}
	if (result == OK) {
		for (unsigned i = 0; i < context->nb_streams; i++) {
			context->streams[i]->discard = (int)i == p_stream ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
		}
		const AVStream *stream = context->streams[p_stream];
		const AVCodec *decoder = avcodec_find_decoder(stream->codecpar->codec_id);
		codec = decoder ? avcodec_alloc_context3(decoder) : nullptr;
		packet = av_packet_alloc();
		if (codec == nullptr || packet == nullptr || avcodec_parameters_to_context(codec, stream->codecpar) < 0) {
			fail(ERR_UNAVAILABLE, "No decoder for this subtitle track.");
		} else {
			codec->pkt_timebase = stream->time_base;
			if (avcodec_open2(codec, decoder, nullptr) < 0) {
				fail(ERR_UNAVAILABLE, "The subtitle decoder didn't open.");
			}
		}
	}
	if (result == OK) {
		const AVRational time_base = context->streams[p_stream]->time_base;
		while (av_read_frame(context, packet) >= 0) {
			if (lines_generation.get() != p_generation) {
				av_packet_unref(packet);
				fail(ERR_SKIP, "Stopped: the file was closed.");
				break;
			}
			if (packet->stream_index == p_stream) {
				const int64_t ts = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
				if (ts != AV_NOPTS_VALUE) {
					const double start = ts * av_q2d(time_base) - p_start_time;
					const double packet_duration = packet->duration > 0 ? packet->duration * av_q2d(time_base) : -1.0;
					SubtitleEvent event;
					bool error = false;
					if (_decode_text_subtitle(codec, packet, start, packet_duration, event, error)) {
						r_events.push_back(event);
					}
				}
			}
			av_packet_unref(packet);
		}
	}
	av_packet_free(&packet);
	avcodec_free_context(&codec);
	if (context) {
		avformat_close_input(&context);
	}
	if (side_avio) {
		av_freep(&side_avio->buffer);
		avio_context_free(&side_avio);
	}
	if (side_smb) {
		side_smb->close();
		memdelete(side_smb);
	}
	if (side_net) {
		side_net->close();
		memdelete(side_net);
	}
	return result;
}

Array MediaSource::get_subtitle_lines(int p_track) {
	Array lines;
	ERR_FAIL_COND_V_MSG(!is_open(), lines, "MediaSource: not open.");
	LocalVector<SubtitleEvent> events;
	String error;
	// Copied under the mutex: close() may run meanwhile (it clears the tracks under it); the side context needs
	// nothing else of this source.
	String file;
	int stream = -1;
	double start = 0.0;
	uint32_t generation = 0;
	{
		MutexLock lock(mutex);
		generation = lines_generation.get();
		if (p_track >= 0 && p_track < (int)subtitle_tracks.size()) {
			const SubtitleTrack &track = subtitle_tracks[p_track];
			if (!track.text) {
				error = vformat("Subtitle track %d is pictures (%s), not text.", p_track, track.codec);
			} else {
				file = path;
				stream = track.stream_index;
				start = start_time;
			}
		} else {
			const int external = p_track - (int)subtitle_tracks.size();
			if (p_track < 0 || external >= (int)external_subtitles.size()) {
				error = vformat("There is no subtitle track %d.", p_track);
			} else {
				events = external_subtitles[external].events;
			}
		}
	}
	if (stream >= 0) {
		_read_subtitle_stream(file, stream, start, generation, events, error);
	}
	if (!error.is_empty()) {
		MutexLock lock(mutex);
		last_error = error;
		WARN_PRINT("MediaSource: " + error);
		return lines;
	}
	struct ByStart {
		bool operator()(const SubtitleEvent &p_a, const SubtitleEvent &p_b) const { return p_a.start < p_b.start; }
	};
	events.sort_custom<ByStart>();
	for (uint32_t i = 0; i < events.size(); i++) {
		const SubtitleEvent &event = events[i];
		double end = event.end;
		if (end < event.start) {
			end = i + 1 < events.size() ? events[i + 1].start : event.start + 5.0;
		}
		Dictionary d;
		d["start"] = event.start;
		d["end"] = end;
		d["text"] = event.text;
		d["ass"] = event.ass;
		lines.push_back(d);
	}
	return lines;
}
