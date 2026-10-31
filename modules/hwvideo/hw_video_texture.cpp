/**************************************************************************/
/*  hw_video_texture.cpp                                                  */
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

#include "hw_video_texture.h"

#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_server.h"

#include <drm/drm_fourcc.h>
#include <linux/videodev2.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdlib>
#include <cstring>
#include <ctime>

#ifndef V4L2_PIX_FMT_QC10C
#define V4L2_PIX_FMT_QC10C v4l2_fourcc('Q', '1', '0', 'C')
#endif
#ifndef V4L2_PIX_FMT_P010
#define V4L2_PIX_FMT_P010 v4l2_fourcc('P', '0', '1', '0')
#endif
#ifndef V4L2_PIX_FMT_QC08C
#define V4L2_PIX_FMT_QC08C v4l2_fourcc('Q', '0', '8', 'C')
#endif
#ifndef DRM_FORMAT_MOD_QCOM_COMPRESSED
#define DRM_FORMAT_MOD_QCOM_COMPRESSED fourcc_mod_code(QCOM, 1)
#endif

/* HWVideoPlaneTexture */

void HWVideoPlaneTexture::init(int p_plane) {
	ERR_FAIL_COND(proxy.is_valid());
	// Limited-range black: Y = 16, Cb = Cr = 128. L8 and LA8 sample like the plane views (.r, and .r/.a).
	Ref<Image> image;
	if (p_plane == 0) {
		image = Image::create_empty(2, 2, false, Image::FORMAT_L8);
		image->fill(Color(16.0 / 255.0, 16.0 / 255.0, 16.0 / 255.0));
	} else {
		image = Image::create_empty(2, 2, false, Image::FORMAT_LA8);
		image->fill(Color(128.0 / 255.0, 128.0 / 255.0, 128.0 / 255.0, 128.0 / 255.0));
	}
	placeholder = RS::get_singleton()->texture_2d_create(image);
	proxy = RS::get_singleton()->texture_proxy_create(placeholder);
	size = Size2i(2, 2);
}

void HWVideoPlaneTexture::set_source(RID p_texture, const Size2i &p_size) {
	ERR_FAIL_COND(proxy.is_null());
	RS::get_singleton()->texture_proxy_update(proxy, p_texture.is_valid() ? p_texture : placeholder);
	const Size2i new_size = p_texture.is_valid() ? p_size : Size2i(2, 2);
	if (new_size != size) {
		size = new_size;
		emit_changed();
	}
}

HWVideoPlaneTexture::~HWVideoPlaneTexture() {
	if (RS::get_singleton() != nullptr) {
		if (proxy.is_valid()) {
			RS::get_singleton()->free_rid(proxy);
		}
		if (placeholder.is_valid()) {
			RS::get_singleton()->free_rid(placeholder);
		}
	}
}

/* HWVideoTexture */

// Render thread. The release of the previous frame and the acquire of the next are recorded through the RenderingDevice
// graph, before this frame's draws. Images in p_free are released if needed and freed; DmabufImage keeps the VkImage alive
// until the frames that may still use it are done.
void HWVideoTexture::_render_thread_swap(const Ref<DmabufImage> &p_release, const Ref<DmabufImage> &p_acquire, const Array &p_free) {
	if (p_release.is_valid() && p_release->is_imported()) {
		p_release->release();
	}
	if (p_acquire.is_valid() && p_acquire->is_imported()) {
		p_acquire->acquire();
	}
	for (const Variant &v : p_free) {
		Ref<DmabufImage> image = v;
		if (image.is_valid()) {
			if (image->is_imported()) {
				image->release();
			}
			image->free_image();
		}
	}
}

void HWVideoTexture::_free_buffer(Buffer &p_buffer, Array &r_free) {
	for (uint32_t p = 0; p < DmabufImage::MAX_PLANES; p++) {
		if (p_buffer.planes[p].is_valid()) {
			RS::get_singleton()->free_rid(p_buffer.planes[p]);
			p_buffer.planes[p] = RID();
		}
	}
	if (p_buffer.image.is_valid()) {
		r_free.push_back(p_buffer.image);
		p_buffer.image.unref();
	}
}

// The decoder has moved to a new CAPTURE generation (resolution change or session restart): its old buffers are orphaned
// in the driver. Free ours, except the one on screen, which stays until the next frame replaces it.
void HWVideoTexture::_release_generation(Array &r_free) {
	for (uint32_t i = 0; i < buffers.size(); i++) {
		if ((int)i == shown && !have_orphan) {
			orphan = buffers[i];
			have_orphan = true;
			continue;
		}
		_free_buffer(buffers[i], r_free);
	}
	frames_dropped += ready.size();
	ready.clear();
	buffers.clear();
	shown = -1;
	have_generation = false;
}

Error HWVideoTexture::_import_generation(uint32_t p_generation) {
	if (p_generation == failed_generation) {
		return ERR_UNAVAILABLE;
	}
	V4L2Decoder::CaptureLayout layout;
	Error err = decoder->get_capture_layout(p_generation, layout);
	if (err != OK) {
		return err;
	}

	Array to_free;
	_release_generation(to_free);

	const bool q10c = layout.fourcc == V4L2_PIX_FMT_QC10C;
	const bool p010 = layout.fourcc == V4L2_PIX_FMT_P010;
	if (layout.fourcc != V4L2_PIX_FMT_NV12 && layout.fourcc != V4L2_PIX_FMT_QC08C && !q10c && !p010) {
		for (int fd : layout.fds) {
			::close(fd);
		}
		failed_generation = p_generation;
		last_error = "Only NV12, Q08C (UBWC NV12), P010 and, experimentally, Q10C CAPTURE buffers can be imported.";
		ERR_PRINT("HWVideoTexture: " + last_error);
		RS::get_singleton()->call_on_render_thread(callable_mp_static(&HWVideoTexture::_render_thread_swap).bind(Ref<DmabufImage>(), Ref<DmabufImage>(), to_free));
		return ERR_UNAVAILABLE;
	}

	// One import per buffer and CAPTURE generation, never per frame: per-frame dmabuf imports can oops the 6.18 kernel.
	// Q08C: two memory planes, each starting at its UBWC metadata (DmabufImage imports it as two images, see there).
	const uint64_t modifier = layout.ubwc ? DRM_FORMAT_MOD_QCOM_COMPRESSED : DRM_FORMAT_MOD_LINEAR;
	PackedInt64Array offsets;
	offsets.push_back(0);
	offsets.push_back((int64_t)layout.chroma_offset);
	PackedInt64Array pitches;
	pitches.push_back(layout.pitch);
	pitches.push_back(layout.pitch);

	// Q10C (experiment): each plane as a UBWC image of a 32-bit format, W/3 texels wide (three 10-bit samples per texel);
	// the shader unpacks them (see Packing). Turnip has no TP10 format, but the A750 can read UBWC data as another format.
	Packing new_packing = p010 ? PACKING_P010 : PACKING_NV12;
	Size2i new_plane_sizes[DmabufImage::MAX_PLANES] = { layout.size, layout.size / 2 };
	RD::DataFormat q10c_format = RD::DATA_FORMAT_A2B10G10R10_UNORM_PACK32;
	bool q10c_mutable = false;
	if (q10c) {
		new_packing = q10c_view.begins_with("rgba8") ? PACKING_TP10_RGBA8 : PACKING_TP10_RGB10A2;
		q10c_format = new_packing == PACKING_TP10_RGBA8 ? RD::DATA_FORMAT_R8G8B8A8_UNORM : RD::DATA_FORMAT_A2B10G10R10_UNORM_PACK32;
		q10c_mutable = q10c_view.ends_with("_mutable");
		new_plane_sizes[0] = Size2i((layout.size.width + 2) / 3, layout.size.height);
		new_plane_sizes[1] = Size2i((layout.size.width + 2) / 3, (layout.size.height + 1) / 2);
	}
	const uint64_t q10c_offsets[DmabufImage::MAX_PLANES] = { 0, layout.chroma_offset };
	const uint64_t q10c_pitches[DmabufImage::MAX_PLANES] = { layout.pitch, layout.pitch };

	buffers.resize(layout.fds.size());
	for (uint32_t i = 0; i < layout.fds.size(); i++) {
		Buffer &buffer = buffers[i];
		buffer = Buffer();
		if (err == OK) {
			buffer.image.instantiate();
			if (q10c) {
				err = buffer.image->import_planes(layout.fds[i], q10c_format, (int64_t)modifier, new_plane_sizes, q10c_offsets, q10c_pitches, DmabufImage::MAX_PLANES, q10c_mutable);
			} else if (p010) {
				err = buffer.image->import_p010(layout.fds[i], layout.size.width, layout.size.height, (int64_t)modifier, offsets, pitches);
			} else {
				err = buffer.image->import_nv12(layout.fds[i], layout.size.width, layout.size.height, (int64_t)modifier, offsets, pitches);
			}
			if (err == OK) {
				for (uint32_t p = 0; p < DmabufImage::MAX_PLANES; p++) {
					buffer.planes[p] = RS::get_singleton()->texture_rd_create(buffer.image->get_plane_rid(p));
				}
			}
		}
		::close(layout.fds[i]); // The image holds its own reference.
	}
	if (err != OK) {
		for (Buffer &buffer : buffers) {
			_free_buffer(buffer, to_free);
		}
		buffers.clear();
		failed_generation = p_generation;
		last_error = vformat("Importing the decoder's CAPTURE buffers (generation %d) failed: %s.", p_generation, error_names[err]);
		ERR_PRINT("HWVideoTexture: " + last_error);
		ubwc_import_failed = layout.ubwc && !q10c && source.is_valid();
	} else {
		generation = p_generation;
		have_generation = true;
		decoded_size = layout.size;
		visible_rect = layout.visible;
		packing = new_packing;
		plane_sizes[0] = new_plane_sizes[0];
		plane_sizes[1] = new_plane_sizes[1];
		const String format_name = q10c ? vformat("Q10C (UBWC TP10, planes as %s%s)", new_packing == PACKING_TP10_RGBA8 ? "RGBA8" : "RGB10A2", q10c_mutable ? ", mutable" : "") : (layout.ubwc ? String("Q08C (UBWC)") : String(p010 ? "P010" : "NV12"));
		print_line(vformat("HWVideoTexture: imported %d decoder buffers (generation %d): %s %dx%d, pitch %d, %s at %d, visible %dx%d+%d+%d, %d Vulkan image(s) per buffer.", buffers.size(), p_generation,
				format_name, layout.size.width, layout.size.height, layout.pitch, layout.ubwc ? "UV metadata" : "chroma", layout.chroma_offset,
				visible_rect.size.x, visible_rect.size.y, visible_rect.position.x, visible_rect.position.y, buffers.is_empty() ? 0 : buffers[0].image->get_image_count()));
	}
	if (!to_free.is_empty()) {
		RS::get_singleton()->call_on_render_thread(callable_mp_static(&HWVideoTexture::_render_thread_swap).bind(Ref<DmabufImage>(), Ref<DmabufImage>(), to_free));
	}
	if (err == OK) {
		emit_signal(SNAME("format_changed"));
	}
	return err;
}

void HWVideoTexture::set_q10c_view(const String &p_view) {
	ERR_FAIL_COND_MSG(p_view != "rgb10a2" && p_view != "rgb10a2_mutable" && p_view != "rgba8" && p_view != "rgba8_mutable", "q10c_view is rgb10a2, rgb10a2_mutable, rgba8 or rgba8_mutable.");
	q10c_view = p_view;
}

void HWVideoTexture::_show(const ReadyFrame &p_frame, uint64_t p_frames_drawn) {
	Ref<DmabufImage> release;
	Array to_free;
	if (shown >= 0) {
		Buffer &previous = buffers[shown];
		previous.status = BUFFER_RETIRING;
		previous.release_frame = p_frames_drawn;
		release = previous.image;
	}

	Buffer &buffer = buffers[p_frame.buffer];
	buffer.status = BUFFER_SHOWN;
	shown = (int)p_frame.buffer;
	frame_time = p_frame.time;
	frames_shown++;
	plane_textures[0]->set_source(buffer.planes[0], plane_sizes[0]);
	plane_textures[1]->set_source(buffer.planes[1], plane_sizes[1]);

	if (have_orphan) {
		// Nothing points at it any more.
		_free_buffer(orphan, to_free);
		have_orphan = false;
	}
	RS::get_singleton()->call_on_render_thread(callable_mp_static(&HWVideoTexture::_render_thread_swap).bind(release, buffer.image, to_free));
}

void HWVideoTexture::_return_ready_buffer(const ReadyFrame &p_frame) {
	if (software) {
		software->return_slot(p_frame.buffer);
		return;
	}
	buffers[p_frame.buffer].status = BUFFER_DECODER;
	decoder->return_buffer(generation, p_frame.buffer);
}

void HWVideoTexture::_discard_ready(uint32_t p_count, bool p_skipped) {
	for (uint32_t k = 0; k < p_count; k++) {
		_return_ready_buffer(ready[k]);
	}
	if (p_skipped) {
		frames_skipped += p_count;
	} else {
		frames_dropped += p_count;
	}
	for (uint32_t k = 0; k < p_count; k++) {
		ready.remove_at(0); // Fewer than a handful of entries.
	}
}

void HWVideoTexture::_return_all_ready() {
	for (const ReadyFrame &ready_frame : ready) {
		_return_ready_buffer(ready_frame);
	}
	frames_stale += ready.size();
	ready.clear();
}

// A seek on the source (by us, the audio player or anyone): frames ready now are from before it. True if there was one.
bool HWVideoTexture::_check_seek() {
	if (source.is_null()) {
		return false;
	}
	const uint32_t source_serial = source->get_serial();
	if (source_serial == serial) {
		return false;
	}
	serial = source_serial;
	clock = source->get_seek_target();
	seek_target = clock;
	settling = true;
	_return_all_ready();
	return true;
}

void HWVideoTexture::update(double p_delta, double p_master_clock) {
	if (software) {
		_update_software(p_delta, p_master_clock);
		_check_next();
		return;
	}
	if (decoder.is_null() || !decoder->is_open()) {
		return;
	}
	if (decoder->is_vp9() && software_fallback && decoder->get_state() == V4L2Decoder::STATE_ERROR && decoder->is_worker_finished()) {
		// A VP9 decoder fault (or 10-bit VP9 without P010): the worker has closed the device (so close() doesn't wait on
		// a stuck ioctl here); go on in software.
		String why;
		if (decoder->is_format_error()) {
			const MediaSource::VideoInfo &info = source->get_video_stream_info();
			why = MediaSource::get_hw_unsupported_reason(info, decoder->is_experimental_10bit() || V4L2Decoder::is_p010_available(decoder->get_device_path()));
		}
		if (why.is_empty()) {
			why = "The hardware decoder failed on VP9 (" + decoder->get_last_error() + ").";
		}
		_switch_to_software(why);
		_check_next();
		return;
	}
	_check_seek();
	if (playing) {
		// Trick play (keyframes only, no audio): the source sets the pace.
		const double trick = source.is_valid() ? source->get_trick_play_speed() : 0.0;
		clock = trick > 0.0 ? clock + p_delta * trick : (p_master_clock >= 0.0 ? p_master_clock : clock + p_delta * playback_speed);
	}
	decoder->set_clock(clock);

	RenderingDevice *rd = RenderingDevice::get_singleton();
	const uint64_t frames_drawn = rd ? rd->get_frames_drawn() : 0;
	const uint64_t frame_delay = rd ? rd->get_frame_delay() : 0;

	// A released buffer goes back to the decoder once the frame that recorded the release has completed on the GPU. One
	// frame of margin covers a render thread that runs a frame behind this one.
	for (uint32_t i = 0; i < buffers.size(); i++) {
		Buffer &buffer = buffers[i];
		if (buffer.status == BUFFER_RETIRING && frames_drawn >= buffer.release_frame + frame_delay + 2) {
			buffer.status = BUFFER_DECODER;
			decoder->return_buffer(generation, i);
		}
	}

	V4L2Decoder::Frame frame;
	while (decoder->pop_frame(frame)) {
		if (frame.serial != serial) {
			// Fed before the last seek.
			frames_stale++;
			decoder->return_buffer(frame.generation, frame.buffer);
			continue;
		}
		if ((!have_generation || frame.generation != generation) && _import_generation(frame.generation) != OK) {
			decoder->return_buffer(frame.generation, frame.buffer);
			continue;
		}
		if (frame.buffer >= buffers.size() || buffers[frame.buffer].status != BUFFER_DECODER) {
			ERR_PRINT(vformat("HWVideoTexture: unexpected buffer %d from the decoder.", frame.buffer));
			continue;
		}
		buffers[frame.buffer].status = BUFFER_READY;
		ReadyFrame ready_frame;
		ready_frame.buffer = frame.buffer;
		ready_frame.time = frame.time;
		ready.push_back(ready_frame);
	}
	if (ubwc_import_failed) {
		_fall_back_to_nv12();
		return;
	}

	ReadyFrame show;
	if (_select_due(decoder->get_state() == V4L2Decoder::STATE_ENDED, show)) {
		_show(show, frames_drawn);
	}
	_check_next();
}

/* The next item (#44) */

// Points the plane textures at the frame on screen (the placeholder if none).
void HWVideoTexture::_point_planes() {
	for (uint32_t p = 0; p < DmabufImage::MAX_PLANES; p++) {
		RID rid;
		if (software) {
			rid = software_shown >= 0 ? software_rs[p] : RID();
		} else if (shown >= 0) {
			rid = buffers[shown].planes[p];
		} else if (have_orphan) {
			rid = orphan.planes[p];
		}
		plane_textures[p]->set_source(rid, rid.is_valid() ? plane_sizes[p] : Size2i());
	}
}

void HWVideoTexture::swap_planes_with(const Ref<HWVideoTexture> &p_other) {
	ERR_FAIL_COND(p_other.is_null() || p_other.ptr() == this);
	for (uint32_t p = 0; p < DmabufImage::MAX_PLANES; p++) {
		SWAP(plane_textures[p], p_other->plane_textures[p]);
	}
	_point_planes();
	p_other->_point_planes();
	emit_signal(SNAME("format_changed"));
	p_other->emit_signal(SNAME("format_changed"));
}

void HWVideoTexture::set_next(const Ref<HWVideoTexture> &p_next) {
	ERR_FAIL_COND_MSG(p_next.ptr() == this, "HWVideoTexture: an item can't be its own next.");
	next = p_next;
	next_set_usec = OS::get_singleton()->get_ticks_usec();
	next_preroll_seconds = -1.0;
}

bool HWVideoTexture::is_finished() const {
	const bool input_ended = software ? software->is_ended() : (decoder.is_valid() && decoder->is_open() && decoder->get_state() == V4L2Decoder::STATE_ENDED);
	if (!input_ended || !ready.is_empty() || !has_frame() || frame_time < 0.0) {
		return false;
	}
	if (source.is_valid() && source->has_audio() && source->is_audio_finished()) {
		return true; // The audio clock stops at the end of the audio.
	}
	const double fps = source.is_valid() ? source->get_video_stream_info().fps : 0.0;
	return clock >= frame_time + (fps > 0.0 ? 1.0 / fps : 0.0) - 1e-4;
}

// After this frame's update(): preroll the next item; at the end of this one, hand it the plane textures.
void HWVideoTexture::_check_next() {
	if (next.is_null()) {
		return;
	}
	next->update(0.0, -1.0); // Not playing: it decodes and shows its first frame, then waits (its lead time).
	if (next_preroll_seconds < 0.0 && next->has_frame()) {
		next_preroll_seconds = (OS::get_singleton()->get_ticks_usec() - next_set_usec) / 1e6;
	}
	if (!next->has_frame() || !is_finished()) {
		return;
	}
	const Ref<HWVideoTexture> incoming = next;
	next.unref();
	swap_planes_with(incoming);
	incoming->play();
	emit_signal(SNAME("next_started"), incoming);
}

// Q08C (UBWC) buffers that don't import: reopen the decoder with linear NV12 and carry on from the same time. Once: the
// decoder keeps NV12 from then on.
void HWVideoTexture::_fall_back_to_nv12() {
	ubwc_import_failed = false;
	const Ref<MediaSource> resume_source = source;
	const double resume_time = clock;
	const bool was_playing = playing;
	print_line(vformat("HWVideoTexture: Q08C (UBWC) import failed; reopening the decoder with NV12 at %.3f s.", resume_time));
	close();
	decoder->set_capture_format("NV12");
	if (open_source(resume_source) != OK) {
		return; // last_error says why.
	}
	resume_source->seek(resume_time);
	playing = was_playing;
}

// Picks the frame to show now from `ready`: the newest that is due; the ones before it are late and dropped (or, while
// settling after a seek, skipped). False if there is nothing to show yet.
bool HWVideoTexture::_select_due(bool p_input_ended, ReadyFrame &r_show) {
	int due = -1;
	bool later = false;
	for (uint32_t k = 0; k < ready.size(); k++) {
		if (ready[k].time > clock + 1e-4) {
			later = true;
			break;
		}
		due = (int)k;
	}
	if (settling) {
		if (due < 0) {
			if (!later) {
				return false;
			}
			due = 0; // The first frame is after the target (a stream starting at 0.083 s, a seek into a gap): show it.
		} else if (!later && ready[due].time < clock - 1e-3 && !p_input_ended) {
			// Not known yet whether this is the frame for the target: keep it, skip the ones before it.
			_discard_ready((uint32_t)due, true);
			return false;
		}
		_discard_ready((uint32_t)due, true);
		settling = false;
	} else {
		if (due < 0) {
			return false;
		}
		_discard_ready((uint32_t)due, false);
	}
	r_show = ready[0];
	ready.remove_at(0);
	return true;
}

/* Software path */

// Render thread: copies the frame's planes into the textures (through the RenderingDevice's staging buffers, before this
// frame's draws). The Vectors are the decoder's slot data, held by reference; the slot isn't written again until the
// frames after this one have completed.
void HWVideoTexture::_render_thread_upload(RID p_y, RID p_uv, const Vector<uint8_t> &p_y_data, const Vector<uint8_t> &p_uv_data) {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (rd == nullptr || !rd->texture_is_valid(p_y) || !rd->texture_is_valid(p_uv)) {
		return;
	}
	rd->texture_update(p_y, 0, p_y_data);
	rd->texture_update(p_uv, 0, p_uv_data);
}

void HWVideoTexture::_free_software_textures() {
	for (uint32_t p = 0; p < DmabufImage::MAX_PLANES; p++) {
		if (software_rs[p].is_valid() && RS::get_singleton() != nullptr) {
			RS::get_singleton()->free_rid(software_rs[p]);
		}
		software_rs[p] = RID();
		RenderingDevice *rd = RenderingDevice::get_singleton();
		if (software_rd[p].is_valid() && rd != nullptr) {
			rd->free_rid(software_rd[p]); // Destroyed once the frames that may use it are done.
		}
		software_rd[p] = RID();
	}
	software_generation = UINT32_MAX;
}

void HWVideoTexture::_show_software(const ReadyFrame &p_frame, uint64_t p_frames_drawn) {
	const SoftwareVideoDecoder::Slot &slot = software->get_slot(p_frame.buffer);
	bool format_changed = false;
	if (p_frame.generation != software_generation || software_rd[0].is_null()) {
		// First frame, or a new size: textures for it.
		for (uint32_t p = 0; p < DmabufImage::MAX_PLANES; p++) {
			plane_textures[p]->set_source(RID(), Size2i());
		}
		_free_software_textures();
		RenderingDevice *rd = RenderingDevice::get_singleton();
		ERR_FAIL_NULL(rd);
		const Size2i sizes[DmabufImage::MAX_PLANES] = { slot.size, slot.size / 2 };
		const RD::DataFormat formats[DmabufImage::MAX_PLANES] = { RD::DATA_FORMAT_R8_UNORM, RD::DATA_FORMAT_R8G8_UNORM };
		for (uint32_t p = 0; p < DmabufImage::MAX_PLANES; p++) {
			RD::TextureFormat format;
			format.format = formats[p];
			format.width = sizes[p].width;
			format.height = sizes[p].height;
			format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_UPDATE_BIT;
			software_rd[p] = rd->texture_create(format, RD::TextureView());
			ERR_FAIL_COND_MSG(software_rd[p].is_null(), "HWVideoTexture: couldn't create the software path's textures.");
			software_rs[p] = RS::get_singleton()->texture_rd_create(software_rd[p]);
		}
		software_generation = p_frame.generation;
		decoded_size = slot.size;
		visible_rect = Rect2i(Point2i(), slot.size); // FFmpeg's frames are already cropped.
		plane_sizes[0] = sizes[0];
		plane_sizes[1] = sizes[1];
		packing = PACKING_NV12;
		format_changed = true;
		print_line(vformat("HWVideoTexture: software frames %dx%d (%s range).", slot.size.width, slot.size.height, software->is_full_range() ? "full" : "limited"));
	}

	if (software_shown >= 0) {
		RetiringSlot retiring;
		retiring.slot = (uint32_t)software_shown;
		retiring.release_frame = p_frames_drawn;
		software_retiring.push_back(retiring);
	}
	software_shown = (int)p_frame.buffer;
	frame_time = p_frame.time;
	frames_shown++;
	RS::get_singleton()->call_on_render_thread(callable_mp_static(&HWVideoTexture::_render_thread_upload).bind(software_rd[0], software_rd[1], slot.y, slot.uv));
	plane_textures[0]->set_source(software_rs[0], plane_sizes[0]);
	plane_textures[1]->set_source(software_rs[1], plane_sizes[1]);
	if (format_changed) {
		emit_signal(SNAME("format_changed"));
	}
}

void HWVideoTexture::_update_software(double p_delta, double p_master_clock) {
	if (_check_seek()) {
		software->return_all_ready();
	}
	if (playing) {
		// Trick play (keyframes only, no audio): the source sets the pace.
		const double trick = source.is_valid() ? source->get_trick_play_speed() : 0.0;
		clock = trick > 0.0 ? clock + p_delta * trick : (p_master_clock >= 0.0 ? p_master_clock : clock + p_delta * playback_speed);
	}

	RenderingDevice *rd = RenderingDevice::get_singleton();
	const uint64_t frames_drawn = rd ? rd->get_frames_drawn() : 0;
	const uint64_t frame_delay = rd ? rd->get_frame_delay() : 0;
	// A slot shown before goes back once the frame that uploaded from it has completed (as for the hardware path).
	for (uint32_t i = 0; i < software_retiring.size(); i++) {
		if (frames_drawn >= software_retiring[i].release_frame + frame_delay + 2) {
			software->return_slot(software_retiring[i].slot);
			software_retiring.remove_at_unordered(i);
			i--;
		}
	}

	SoftwareVideoDecoder::Frame frame;
	while (software->pop_frame(frame)) {
		if (frame.serial != serial) {
			frames_stale++; // Decoded before the last seek.
			software->return_slot(frame.slot);
			continue;
		}
		ReadyFrame ready_frame;
		ready_frame.buffer = frame.slot;
		ready_frame.time = frame.time;
		ready_frame.generation = frame.generation;
		ready.push_back(ready_frame);
	}
	if (software->has_failed() && last_error.is_empty()) {
		last_error = software->get_error();
	}

	ReadyFrame show;
	if (_select_due(software->is_ended(), show)) {
		_show_software(show, frames_drawn);
	}
}

Dictionary HWVideoTexture::_get_software_stats() const {
	// The keys of V4L2Decoder.get_stats() that apply, so that code reading those works on both paths.
	Dictionary d = software->get_stats();
	const bool ended = software->is_ended() && ready.is_empty();
	const int state = software->has_failed() ? V4L2Decoder::STATE_ERROR : (ended ? V4L2Decoder::STATE_ENDED : V4L2Decoder::STATE_DECODING);
	static const char *state_names[] = { "closed", "starting", "decoding", "reconfiguring", "ended", "error" };
	d["state"] = state;
	d["state_name"] = state_names[state];
	d["frames_decoded"] = d["software_frames_decoded"];
	d["frames_error"] = d["software_decode_errors"];
	d["aus_queued"] = d["software_packets_fed"];
	d["thread_cpu_seconds"] = d["software_thread_cpu_seconds"];
	d["aus_skipped"] = 0;
	d["output_errors"] = 0;
	d["source_changes"] = 0;
	d["loops"] = 0;
	d["eos_inserted"] = 0;
	d["reconfig_in_place"] = 0;
	d["reconfig_restart"] = 0;
	d["restarts"] = 0;
	d["output_in_driver"] = 0;
	d["capture_in_driver"] = d["software_slots_in_use"];
	d["capture_buffers"] = SoftwareVideoDecoder::SLOT_COUNT;
	d["queued_rate"] = 0.0;
	d["latency_avg_ms"] = 0.0;
	d["latency_max_ms"] = 0.0;
	d["media_source"] = true;
	d["software_reason"] = software_reason;
	d["software_description"] = software->get_description();
	const String error = software->get_error();
	if (!error.is_empty()) {
		d["last_error"] = error;
	}
	return d;
}

String HWVideoTexture::get_decode_path() const {
	if (software) {
		return "software";
	}
	return decoder.is_valid() && decoder->is_open() ? "hardware" : "";
}

String HWVideoTexture::get_decode_path_text() const {
	const String path = get_decode_path();
	if (path != "hardware") {
		return path;
	}
	const bool vp9 = decoder->is_vp9();
	const bool ten_bit = have_generation && packing != PACKING_NV12;
	if (vp9) {
		return ten_bit ? "hardware (VP9, 10-bit)" : "hardware (VP9)";
	}
	return ten_bit ? "hardware (10-bit)" : "hardware";
}

Error HWVideoTexture::open(const String &p_path, const String &p_codec, double p_fps) {
	close();
	last_error = String();
	failed_generation = UINT32_MAX;
	clock = 0.0;
	frame_time = -1.0;
	serial = 0;
	playing = false;
	settling = true;
	seek_target = 0.0;
	frames_shown = frames_dropped = frames_stale = frames_skipped = 0;
	const Error err = decoder->open(p_path, p_codec, p_fps);
	if (err != OK) {
		last_error = decoder->get_last_error();
	}
	return err;
}

Error HWVideoTexture::open_source(const Ref<MediaSource> &p_source) {
	close();
	ERR_FAIL_COND_V_MSG(p_source.is_null() || !p_source->is_open(), ERR_INVALID_PARAMETER, "HWVideoTexture: the MediaSource isn't open.");
	last_error = String();
	failed_generation = UINT32_MAX;
	frame_time = -1.0;
	playing = false;
	frames_shown = frames_dropped = frames_stale = frames_skipped = 0;
	source = p_source;
	serial = source->get_serial();
	clock = source->get_seek_target();
	seek_target = clock;
	settling = true;
	software_reason = String();

	// What the hardware path can't show (10-bit without P010, 4:2:2, H.264 High 10) is decoded in software, up to about
	// 1080p; so is VP9 when hardware VP9 is off, or another texture holds the one hardware VP9 session.
	const MediaSource::VideoInfo &info = p_source->get_video_stream_info();
	const bool ten_bit = MAX(info.bit_depth, (int)info.sps.bit_depth) > 8;
	const bool allow_10bit = decoder->is_experimental_10bit() || (ten_bit && V4L2Decoder::is_p010_available(decoder->get_device_path()));
	String hw_reason = MediaSource::get_hw_unsupported_reason(info, allow_10bit);
	bool forced = false; // Software by choice: what software can't decode still goes to the hardware.
	bool never_hardware = false; // VP9 policy: what software can't decode fails instead.
	if (!pending_software_reason.is_empty()) {
		// _switch_to_software(): the hardware session has just failed.
		hw_reason = pending_software_reason;
		pending_software_reason = String();
		never_hardware = true;
	} else if (hw_reason.is_empty()) {
		// A machine without the decoder (a desktop) decodes in software too; HWVIDEO_FORCE_SW=1 does it anywhere (A/B).
		struct stat device;
		if (OS::get_singleton()->get_environment("HWVIDEO_FORCE_SW") == "1") {
			hw_reason = "Software decode forced (HWVIDEO_FORCE_SW=1).";
			forced = true;
			// What software can't take goes to the hardware then, but VP9 only within its rules.
			never_hardware = info.codec == MediaSource::VIDEO_CODEC_VP9 && !_vp9_policy_reason().is_empty();
		} else if (stat(decoder->get_device_path().utf8().get_data(), &device) != 0) {
			hw_reason = vformat("There is no hardware video decoder on this machine (no %s).", decoder->get_device_path());
		} else if (info.codec == MediaSource::VIDEO_CODEC_VP9) {
			hw_reason = _vp9_policy_reason();
			never_hardware = !hw_reason.is_empty();
		}
	}
	const String sw_reason = hw_reason.is_empty() ? String() : SoftwareVideoDecoder::get_unsupported_reason(info, software_max_pixels);
	if (never_hardware && (!software_fallback || !sw_reason.is_empty())) {
		last_error = software_fallback ? hw_reason + " " + sw_reason : hw_reason + " (software_fallback is off)";
		print_line("HWVideoTexture: can't play VP9: " + last_error);
		source.unref();
		return ERR_UNAVAILABLE;
	}
	if (!hw_reason.is_empty() && software_fallback && !(forced && !sw_reason.is_empty())) {
		if (!sw_reason.is_empty()) {
			last_error = hw_reason + " " + sw_reason;
			source.unref();
			return ERR_UNAVAILABLE;
		}
		return _open_software(p_source, hw_reason);
	}

	const Error err = decoder->open_source(p_source);
	if (err == ERR_BUSY && info.codec == MediaSource::VIDEO_CODEC_VP9 && software_fallback) {
		// Opened elsewhere since the check above (another thread): the one-session rule holds, this one goes to software.
		const String busy_reason = decoder->get_last_error();
		const String busy_sw_reason = SoftwareVideoDecoder::get_unsupported_reason(info, software_max_pixels);
		if (!busy_sw_reason.is_empty()) {
			last_error = busy_reason + " " + busy_sw_reason;
			source.unref();
			return ERR_UNAVAILABLE;
		}
		return _open_software(p_source, busy_reason);
	}
	if (err != OK) {
		last_error = decoder->get_last_error();
		source.unref();
	} else if (info.codec == MediaSource::VIDEO_CODEC_VP9) {
		print_line(vformat("HWVideoTexture: hardware decode (VP9 %s, %d-bit): the one hardware VP9 session of this process.", info.profile, MAX(info.bit_depth, 8)));
	}
	return err;
}

// Why this VP9 stream doesn't go to the hardware now ("" if it does): hw_vp9 / HWVIDEO_HW_VP9, a decoder fault earlier
// in the run, or the one hardware VP9 session is taken (two at once crash the iris firmware, SteamOS #2865).
String HWVideoTexture::_vp9_policy_reason() const {
	const String env = OS::get_singleton()->get_environment("HWVIDEO_HW_VP9");
	if (env == "0") {
		return "Hardware VP9 decode is off (HWVIDEO_HW_VP9=0).";
	}
	if (env != "1" && !hw_vp9) {
		return "Hardware VP9 decode is off (the hw_vp9 setting).";
	}
	const String disabled = V4L2Decoder::get_vp9_disabled_reason();
	if (!disabled.is_empty()) {
		return disabled;
	}
	if (V4L2Decoder::is_vp9_session_live()) {
		return "Another video is decoding VP9 in hardware: two VP9 sessions at once crash the decoder firmware (SteamOS #2865), so this one decodes in software.";
	}
	return String();
}

Error HWVideoTexture::_open_software(const Ref<MediaSource> &p_source, const String &p_reason) {
	software = memnew(SoftwareVideoDecoder);
	String error;
	const Error err = software->open(p_source, software_threads, error);
	if (err != OK) {
		memdelete(software);
		software = nullptr;
		last_error = error;
		source.unref();
		return err;
	}
	software_reason = p_reason;
	print_line(vformat("HWVideoTexture: software decode (%s): %s", software->get_description(), p_reason));
	return OK;
}

// Closes the decoder (hardware or software), opens the software path on the same source and seeks it to where playback
// is, keeping play/pause and the next item. The picture is black until the first software frame is up.
Error HWVideoTexture::_switch_to_software(const String &p_reason) {
	ERR_FAIL_COND_V_MSG(source.is_null(), ERR_UNCONFIGURED, "HWVideoTexture: switching to software decode needs a MediaSource.");
	const Ref<MediaSource> resume_source = source;
	const double resume_time = clock;
	const bool was_playing = playing;
	const Ref<HWVideoTexture> keep_next = next;
	const uint64_t keep_next_usec = next_set_usec;
	const double keep_next_preroll = next_preroll_seconds;
	print_line(vformat("HWVideoTexture: switching to software decode at %.3f s: %s", resume_time, p_reason));
	close();
	// Seek first: the new decoder then starts at the keyframe before resume_time (open_source() takes the serial and the
	// target), instead of being fed the rest of a GOP it has no references for.
	resume_source->seek(resume_time);
	pending_software_reason = p_reason;
	const Error err = open_source(resume_source);
	pending_software_reason = String();
	next = keep_next;
	next_set_usec = keep_next_usec;
	next_preroll_seconds = keep_next_preroll;
	if (err != OK) {
		ERR_PRINT("HWVideoTexture: the software path couldn't take over: " + last_error);
		return err;
	}
	playing = was_playing;
	return OK;
}

Error HWVideoTexture::switch_to_software(const String &p_reason) {
	return _switch_to_software(p_reason.is_empty() ? String("Switched to software decode.") : p_reason);
}

void HWVideoTexture::close() {
	next.unref(); // Still open: the caller's to close.
	if (RS::get_singleton() != nullptr) {
		Array to_free;
		for (uint32_t p = 0; p < DmabufImage::MAX_PLANES; p++) {
			plane_textures[p]->set_source(RID(), Size2i());
		}
		for (Buffer &buffer : buffers) {
			_free_buffer(buffer, to_free);
		}
		if (have_orphan) {
			_free_buffer(orphan, to_free);
		}
		if (!to_free.is_empty()) {
			RS::get_singleton()->call_on_render_thread(callable_mp_static(&HWVideoTexture::_render_thread_swap).bind(Ref<DmabufImage>(), Ref<DmabufImage>(), to_free));
		}
	}
	if (software) {
		memdelete(software); // Stops its thread.
		software = nullptr;
	}
	_free_software_textures();
	software_retiring.clear();
	software_shown = -1;
	buffers.clear();
	ready.clear();
	shown = -1;
	have_orphan = false;
	have_generation = false;
	decoded_size = Size2i();
	packing = PACKING_NV12;
	visible_rect = Rect2i();
	playing = false;
	decoder->close();
	source.unref();
}

void HWVideoTexture::play() {
	playing = true;
}

void HWVideoTexture::pause() {
	playing = false;
}

void HWVideoTexture::seek_to_start() {
	if (source.is_valid()) {
		source->seek(0.0); // Picked up by update().
		return;
	}
	serial = (uint32_t)decoder->seek_to_start();
	clock = 0.0;
	seek_target = 0.0;
	settling = true;
	_return_all_ready();
}

Error HWVideoTexture::seek(double p_seconds) {
	if (source.is_valid()) {
		source->seek(p_seconds);
		return OK;
	}
	ERR_FAIL_COND_V_MSG(p_seconds != 0.0, ERR_UNAVAILABLE, "HWVideoTexture: a raw Annex-B file can only seek to 0; open it through a MediaSource to seek anywhere.");
	seek_to_start();
	return OK;
}

Ref<Texture2D> HWVideoTexture::get_plane_texture(int p_plane) const {
	ERR_FAIL_INDEX_V(p_plane, (int)DmabufImage::MAX_PLANES, Ref<Texture2D>());
	return plane_textures[p_plane];
}

RID HWVideoTexture::get_plane_rd_rid(int p_plane) const {
	ERR_FAIL_INDEX_V(p_plane, (int)DmabufImage::MAX_PLANES, RID());
	if (software) {
		return software_shown >= 0 ? software_rd[p_plane] : RID();
	}
	if (shown >= 0) {
		return buffers[shown].image->get_plane_rid(p_plane);
	}
	if (have_orphan) {
		return orphan.image->get_plane_rid(p_plane);
	}
	return RID();
}

Rect2 HWVideoTexture::get_visible_uv_rect() const {
	if (decoded_size.width <= 0 || decoded_size.height <= 0) {
		return Rect2(0, 0, 1, 1);
	}
	const Vector2 size = decoded_size;
	return Rect2(Vector2(visible_rect.position) / size, Vector2(visible_rect.size) / size);
}

Dictionary HWVideoTexture::get_stats() const {
	Dictionary d = software ? _get_software_stats() : decoder->get_stats();
	if (!software && !decoder->is_open() && !last_error.is_empty()) {
		// Opening failed, or the software path couldn't take over from a failed hardware session.
		d["state"] = V4L2Decoder::STATE_ERROR;
		d["state_name"] = "error";
	}
	d["decode_path"] = get_decode_path();
	d["decode_path_text"] = get_decode_path_text();
	// CPU time of the whole process (all threads, FFmpeg's decoder threads included), to compare the two paths.
	struct timespec cpu;
	clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cpu);
	d["process_cpu_seconds"] = cpu.tv_sec + cpu.tv_nsec * 1e-9;
	d["frames_shown"] = frames_shown;
	d["next_preroll_seconds"] = next_preroll_seconds;
	d["frames_dropped"] = frames_dropped;
	d["frames_stale"] = frames_stale;
	d["frames_skipped"] = frames_skipped;
	d["settling"] = settling;
	d["seek_target"] = seek_target;
	d["frames_ready"] = ready.size();
	d["position"] = clock;
	d["frame_time"] = frame_time;
	d["playing"] = playing;
	d["generation"] = have_generation ? (int64_t)generation : -1;
	uint32_t retiring = 0;
	for (const Buffer &buffer : buffers) {
		retiring += buffer.status == BUFFER_RETIRING ? 1 : 0;
	}
	d["buffers_retiring"] = retiring;
	if (!last_error.is_empty()) {
		d["last_error"] = last_error;
	}
	return d;
}

// Reads a small sysfs file (no allocation beyond the result). False if it can't be read.
static bool read_sysfs(const String &p_path, char *r_buffer, int p_size) {
	const int fd = ::open(p_path.utf8().get_data(), O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		return false;
	}
	const ssize_t n = ::read(fd, r_buffer, p_size - 1);
	::close(fd);
	if (n <= 0) {
		return false;
	}
	size_t length = (size_t)n;
	r_buffer[length] = 0;
	while (length > 0 && (r_buffer[length - 1] == '\n' || r_buffer[length - 1] == ' ')) {
		r_buffer[--length] = 0;
	}
	return true;
}

static bool read_sysfs_int(const String &p_path, int64_t &r_value) {
	char buffer[64];
	if (!read_sysfs(p_path, buffer, sizeof(buffer))) {
		return false;
	}
	char *end = nullptr;
	r_value = strtoll(buffer, &end, 10);
	return end != buffer;
}

namespace {

struct SystemPaths {
	bool scanned = false;
	String gpu_devfreq; // /sys/class/devfreq/<the GPU>.
	struct Zone {
		String temp_path;
		String type;
	};
	LocalVector<Zone> zones;
	String battery; // /sys/class/power_supply/<the battery>.
};

Mutex system_paths_mutex;
SystemPaths system_paths;

void scan_system_paths() {
	if (system_paths.scanned) {
		return;
	}
	system_paths.scanned = true;
	char buffer[128];
	// HWVIDEO_SYSFS_ROOT: a fake tree for tests (root/sys/class/...).
	const String root = OS::get_singleton()->get_environment("HWVIDEO_SYSFS_ROOT");
	const String devfreq = root + "/sys/class/devfreq";
	const String thermal = root + "/sys/class/thermal";
	const String power = root + "/sys/class/power_supply";
	if (DIR *dir = opendir(devfreq.utf8().get_data())) {
		while (struct dirent *entry = readdir(dir)) {
			const String name = String::utf8(entry->d_name);
			const String lower = name.to_lower();
			if (system_paths.gpu_devfreq.is_empty() && (lower.contains("gpu") || lower.contains("kgsl") || lower.contains("3d00000"))) {
				system_paths.gpu_devfreq = devfreq + "/" + name;
			}
		}
		closedir(dir);
	}
	if (DIR *dir = opendir(thermal.utf8().get_data())) {
		while (struct dirent *entry = readdir(dir)) {
			const String name = String::utf8(entry->d_name);
			if (!name.begins_with("thermal_zone")) {
				continue;
			}
			SystemPaths::Zone zone;
			zone.temp_path = thermal + "/" + name + "/temp";
			zone.type = read_sysfs(thermal + "/" + name + "/type", buffer, sizeof(buffer)) ? String::utf8(buffer) : name;
			system_paths.zones.push_back(zone);
		}
		closedir(dir);
	}
	if (DIR *dir = opendir(power.utf8().get_data())) {
		while (struct dirent *entry = readdir(dir)) {
			const String name = String::utf8(entry->d_name);
			if (name.begins_with(".")) {
				continue;
			}
			if (read_sysfs(power + "/" + name + "/type", buffer, sizeof(buffer)) && String::utf8(buffer) == "Battery") {
				system_paths.battery = power + "/" + name;
				break;
			}
		}
		closedir(dir);
	}
}

} // namespace

Dictionary HWVideoTexture::get_system_stats() {
	MutexLock lock(system_paths_mutex);
	scan_system_paths();
	Dictionary d;
	int64_t value = 0;
	if (!system_paths.gpu_devfreq.is_empty()) {
		d["gpu_devfreq"] = system_paths.gpu_devfreq;
		if (read_sysfs_int(system_paths.gpu_devfreq + "/cur_freq", value)) {
			d["gpu_mhz"] = value / 1e6;
		}
		if (read_sysfs_int(system_paths.gpu_devfreq + "/max_freq", value)) {
			d["gpu_max_mhz"] = value / 1e6;
		}
	}
	double hottest = -1000.0;
	double cpu = -1000.0;
	double gpu = -1000.0;
	String hottest_zone;
	for (const SystemPaths::Zone &zone : system_paths.zones) {
		if (!read_sysfs_int(zone.temp_path, value)) {
			continue;
		}
		const double celsius = value / 1000.0;
		const String lower = zone.type.to_lower();
		if (celsius > hottest) {
			hottest = celsius;
			hottest_zone = zone.type;
		}
		if (lower.contains("cpu")) {
			cpu = MAX(cpu, celsius);
		}
		if (lower.contains("gpu")) {
			gpu = MAX(gpu, celsius);
		}
	}
	if (hottest > -1000.0) {
		d["temperature_max_c"] = hottest;
		d["temperature_max_zone"] = hottest_zone;
	}
	if (cpu > -1000.0) {
		d["cpu_temperature_c"] = cpu;
	}
	if (gpu > -1000.0) {
		d["gpu_temperature_c"] = gpu;
	}
	if (!system_paths.battery.is_empty()) {
		int64_t current = 0;
		int64_t voltage = 0;
		if (read_sysfs_int(system_paths.battery + "/power_now", value)) {
			d["battery_power_w"] = Math::abs(value) / 1e6; // Microwatts.
		} else if (read_sysfs_int(system_paths.battery + "/current_now", current) && read_sysfs_int(system_paths.battery + "/voltage_now", voltage)) {
			d["battery_power_w"] = Math::abs((double)current * (double)voltage) / 1e12; // Microamps times microvolts.
		}
		if (read_sysfs_int(system_paths.battery + "/capacity", value)) {
			d["battery_percent"] = value;
		}
		char status[32];
		if (read_sysfs(system_paths.battery + "/status", status, sizeof(status))) {
			d["battery_status"] = String::utf8(status);
		}
	}
	return d;
}

Dictionary HWVideoTexture::get_overlay_stats() {
	const Dictionary stats = get_stats();
	Dictionary d;
	d["decode_path"] = stats["decode_path"];
	d["decode_path_text"] = stats["decode_path_text"];
	d["position"] = clock;
	d["frame_size"] = decoded_size;
	d["packing"] = packing;
	d["frames_shown"] = frames_shown;
	d["frames_dropped"] = frames_dropped;
	d["frames_ready"] = ready.size(); // Decoded, waiting for their time.
	d["decoder_input_queue"] = stats.get("output_in_driver", 0); // Access units in the decoder (hardware).
	d["decoder_capture_free"] = stats.get("capture_in_driver", 0); // Empty picture buffers the decoder can fill.
	d["decoder_latency_ms"] = stats.get("latency_avg_ms", 0.0);
	d["decode_errors"] = stats.get("frames_error", 0);

	if (source.is_valid()) {
		const MediaSource::VideoInfo &info = source->get_video_stream_info();
		d["codec"] = info.codec_name;
		d["video_size"] = Size2i(info.width, info.height);
		d["fps"] = info.fps;
		d["bit_depth"] = info.bit_depth;
		const PackedFloat32Array history = source->get_video_bitrate_history(60, clock);
		d["video_kbps_history"] = history; // The last 60 s, oldest first; -1 unknown.
		// Now: the last 3 known whole seconds.
		double sum = 0.0;
		int known = 0;
		for (int i = history.size() - 1; i >= 0 && known < 3; i--) {
			if (history[i] >= 0.0f) {
				sum += history[i];
				known++;
			}
		}
		d["video_kbps"] = known > 0 ? sum / known : -1.0;
		d["file_kbps"] = source->get_bit_rate() / 1000.0; // The container's overall rate (all streams); 0 if unknown.
		const Dictionary source_stats = source->get_stats();
		const int64_t queued = source_stats.get("video_queue", 0);
		d["demux_video_queue"] = queued;
		d["demux_video_queue_seconds"] = info.fps > 0.0 ? queued / info.fps : 0.0;
		d["demux_video_queue_mb"] = (int64_t)source_stats.get("video_queue_bytes", 0) / 1e6;
		d["audio_buffered_seconds"] = source_stats.get("audio_buffered", 0.0);
		d["audio_underruns"] = source_stats.get("audio_underruns", 0);
		if (bool(source_stats.get("network", false))) {
			const double buffered_bits = (int64_t)source_stats.get("net_buffered_bytes", 0) * 8.0;
			const double file_bps = (double)source->get_bit_rate();
			d["network_buffered_mb"] = buffered_bits / 8e6;
			d["network_buffered_seconds"] = file_bps > 0.0 ? buffered_bits / file_bps : -1.0;
			d["network_read_mbps"] = source_stats.get("net_read_mbps", 0.0);
			d["network_stalls"] = source_stats.get("net_stalls", 0);
		}
	}

	// Rates since the previous call.
	const uint64_t now = OS::get_singleton()->get_ticks_usec();
	const double cpu_seconds = stats["process_cpu_seconds"];
	const int64_t decoded = stats.get("frames_decoded", 0);
	if (overlay_usec != 0 && now > overlay_usec) {
		const double seconds = (now - overlay_usec) / 1e6;
		d["interval_seconds"] = seconds;
		d["process_cpu_percent"] = 100.0 * (cpu_seconds - overlay_cpu_seconds) / seconds; // Of one core.
		d["decode_fps"] = (decoded - overlay_decoded) / seconds;
		d["shown_fps"] = ((int64_t)frames_shown - overlay_shown) / seconds;
		d["dropped_per_second"] = ((int64_t)frames_dropped - overlay_dropped) / seconds;
	}
	overlay_usec = now;
	overlay_cpu_seconds = cpu_seconds;
	overlay_decoded = decoded;
	overlay_shown = (int64_t)frames_shown;
	overlay_dropped = (int64_t)frames_dropped;

	d.merge(get_system_stats());
	return d;
}

HWVideoTexture::HWVideoTexture() {
	decoder.instantiate();
	for (uint32_t p = 0; p < DmabufImage::MAX_PLANES; p++) {
		Ref<HWVideoPlaneTexture> texture;
		texture.instantiate();
		texture->init(p);
		plane_textures[p] = texture;
	}
}

HWVideoTexture::~HWVideoTexture() {
	close();
}

void HWVideoTexture::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_overlay_stats"), &HWVideoTexture::get_overlay_stats);
	ClassDB::bind_method(D_METHOD("set_playback_speed", "speed"), &HWVideoTexture::set_playback_speed);
	ClassDB::bind_method(D_METHOD("get_playback_speed"), &HWVideoTexture::get_playback_speed);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "playback_speed", PROPERTY_HINT_RANGE, "0.25,4,0.05"), "set_playback_speed", "get_playback_speed");
	ClassDB::bind_static_method("HWVideoTexture", D_METHOD("get_system_stats"), &HWVideoTexture::get_system_stats);
	ClassDB::bind_method(D_METHOD("open", "path", "codec", "fps"), &HWVideoTexture::open);
	ClassDB::bind_method(D_METHOD("open_source", "source"), &HWVideoTexture::open_source);
	ClassDB::bind_method(D_METHOD("seek", "seconds"), &HWVideoTexture::seek);
	ClassDB::bind_method(D_METHOD("get_media_source"), &HWVideoTexture::get_media_source);
	ClassDB::bind_method(D_METHOD("close"), &HWVideoTexture::close);
	ClassDB::bind_method(D_METHOD("update", "delta", "master_clock"), &HWVideoTexture::update, DEFVAL(-1.0));
	ClassDB::bind_method(D_METHOD("play"), &HWVideoTexture::play);
	ClassDB::bind_method(D_METHOD("pause"), &HWVideoTexture::pause);
	ClassDB::bind_method(D_METHOD("is_playing"), &HWVideoTexture::is_playing);
	ClassDB::bind_method(D_METHOD("seek_to_start"), &HWVideoTexture::seek_to_start);
	ClassDB::bind_method(D_METHOD("get_position"), &HWVideoTexture::get_position);
	ClassDB::bind_method(D_METHOD("get_frame_time"), &HWVideoTexture::get_frame_time);
	ClassDB::bind_method(D_METHOD("has_frame"), &HWVideoTexture::has_frame);
	ClassDB::bind_method(D_METHOD("set_next", "next"), &HWVideoTexture::set_next);
	ClassDB::bind_method(D_METHOD("get_next"), &HWVideoTexture::get_next);
	ClassDB::bind_method(D_METHOD("is_finished"), &HWVideoTexture::is_finished);
	ClassDB::bind_method(D_METHOD("swap_planes_with", "other"), &HWVideoTexture::swap_planes_with);
	ClassDB::bind_method(D_METHOD("get_decoder"), &HWVideoTexture::get_decoder);
	ClassDB::bind_method(D_METHOD("get_plane_texture", "plane"), &HWVideoTexture::get_plane_texture);
	ClassDB::bind_method(D_METHOD("get_plane_rd_rid", "plane"), &HWVideoTexture::get_plane_rd_rid);
	ClassDB::bind_method(D_METHOD("get_decoded_size"), &HWVideoTexture::get_decoded_size);
	ClassDB::bind_method(D_METHOD("get_visible_rect"), &HWVideoTexture::get_visible_rect);
	ClassDB::bind_method(D_METHOD("get_visible_uv_rect"), &HWVideoTexture::get_visible_uv_rect);
	ClassDB::bind_method(D_METHOD("get_packing"), &HWVideoTexture::get_packing);
	ClassDB::bind_method(D_METHOD("get_decode_path"), &HWVideoTexture::get_decode_path);
	ClassDB::bind_method(D_METHOD("get_decode_path_text"), &HWVideoTexture::get_decode_path_text);
	ClassDB::bind_method(D_METHOD("get_software_reason"), &HWVideoTexture::get_software_reason);
	ClassDB::bind_method(D_METHOD("set_hw_vp9", "enabled"), &HWVideoTexture::set_hw_vp9);
	ClassDB::bind_method(D_METHOD("is_hw_vp9"), &HWVideoTexture::is_hw_vp9);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "hw_vp9"), "set_hw_vp9", "is_hw_vp9");
	ClassDB::bind_method(D_METHOD("switch_to_software", "reason"), &HWVideoTexture::switch_to_software, DEFVAL(""));
	ClassDB::bind_method(D_METHOD("set_software_fallback", "enabled"), &HWVideoTexture::set_software_fallback);
	ClassDB::bind_method(D_METHOD("is_software_fallback"), &HWVideoTexture::is_software_fallback);
	ClassDB::bind_method(D_METHOD("set_software_threads", "threads"), &HWVideoTexture::set_software_threads);
	ClassDB::bind_method(D_METHOD("get_software_threads"), &HWVideoTexture::get_software_threads);
	ClassDB::bind_method(D_METHOD("set_software_max_pixels", "pixels"), &HWVideoTexture::set_software_max_pixels);
	ClassDB::bind_method(D_METHOD("get_software_max_pixels"), &HWVideoTexture::get_software_max_pixels);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "software_fallback"), "set_software_fallback", "is_software_fallback");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "software_threads", PROPERTY_HINT_RANGE, "1,16"), "set_software_threads", "get_software_threads");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "software_max_pixels"), "set_software_max_pixels", "get_software_max_pixels");
	ClassDB::bind_method(D_METHOD("set_q10c_view", "view"), &HWVideoTexture::set_q10c_view);
	ClassDB::bind_method(D_METHOD("get_q10c_view"), &HWVideoTexture::get_q10c_view);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "q10c_view", PROPERTY_HINT_ENUM, "rgb10a2,rgb10a2_mutable,rgba8,rgba8_mutable"), "set_q10c_view", "get_q10c_view");
	BIND_ENUM_CONSTANT(PACKING_NV12);
	BIND_ENUM_CONSTANT(PACKING_TP10_RGBA8);
	BIND_ENUM_CONSTANT(PACKING_TP10_RGB10A2);
	BIND_ENUM_CONSTANT(PACKING_P010);
	ClassDB::bind_method(D_METHOD("get_last_error"), &HWVideoTexture::get_last_error);
	ClassDB::bind_method(D_METHOD("get_stats"), &HWVideoTexture::get_stats);

	ADD_SIGNAL(MethodInfo("format_changed"));
	ADD_SIGNAL(MethodInfo("next_started", PropertyInfo(Variant::OBJECT, "next", PROPERTY_HINT_RESOURCE_TYPE, "HWVideoTexture")));
}
