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
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_server.h"

#include <drm/drm_fourcc.h>
#include <linux/videodev2.h>
#include <unistd.h>

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

	if (layout.fourcc != V4L2_PIX_FMT_NV12) {
		for (int fd : layout.fds) {
			::close(fd);
		}
		failed_generation = p_generation;
		last_error = "Only NV12 CAPTURE buffers can be imported for now (UBWC Q08C needs its plane layout).";
		ERR_PRINT("HWVideoTexture: " + last_error);
		RS::get_singleton()->call_on_render_thread(callable_mp_static(&HWVideoTexture::_render_thread_swap).bind(Ref<DmabufImage>(), Ref<DmabufImage>(), to_free));
		return ERR_UNAVAILABLE;
	}

	// One import per buffer and CAPTURE generation, never per frame: per-frame dmabuf imports can oops the 6.18 kernel.
	PackedInt64Array offsets;
	offsets.push_back(0);
	offsets.push_back((int64_t)layout.chroma_offset);
	PackedInt64Array pitches;
	pitches.push_back(layout.pitch);
	pitches.push_back(layout.pitch);
	buffers.resize(layout.fds.size());
	for (uint32_t i = 0; i < layout.fds.size(); i++) {
		Buffer &buffer = buffers[i];
		buffer = Buffer();
		if (err == OK) {
			buffer.image.instantiate();
			err = buffer.image->import_nv12(layout.fds[i], layout.size.width, layout.size.height, (int64_t)DRM_FORMAT_MOD_LINEAR, offsets, pitches);
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
	} else {
		generation = p_generation;
		have_generation = true;
		decoded_size = layout.size;
		visible_rect = layout.visible;
		print_line(vformat("HWVideoTexture: imported %d decoder buffers (generation %d): NV12 %dx%d, pitch %d, chroma at %d, visible %dx%d+%d+%d.", buffers.size(), p_generation,
				layout.size.width, layout.size.height, layout.pitch, layout.chroma_offset, visible_rect.size.x, visible_rect.size.y, visible_rect.position.x, visible_rect.position.y));
	}
	if (!to_free.is_empty()) {
		RS::get_singleton()->call_on_render_thread(callable_mp_static(&HWVideoTexture::_render_thread_swap).bind(Ref<DmabufImage>(), Ref<DmabufImage>(), to_free));
	}
	if (err == OK) {
		emit_signal(SNAME("format_changed"));
	}
	return err;
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
	plane_textures[0]->set_source(buffer.planes[0], decoded_size);
	plane_textures[1]->set_source(buffer.planes[1], decoded_size / 2);

	if (have_orphan) {
		// Nothing points at it any more.
		_free_buffer(orphan, to_free);
		have_orphan = false;
	}
	RS::get_singleton()->call_on_render_thread(callable_mp_static(&HWVideoTexture::_render_thread_swap).bind(release, buffer.image, to_free));
}

void HWVideoTexture::update(double p_delta, double p_master_clock) {
	if (decoder.is_null() || !decoder->is_open()) {
		return;
	}
	if (playing) {
		clock = p_master_clock >= 0.0 ? p_master_clock : clock + p_delta;
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

	// Show the newest frame that is due; the ones before it are late and dropped.
	int due = -1;
	for (uint32_t k = 0; k < ready.size(); k++) {
		if (ready[k].time > clock + 1e-4) {
			break;
		}
		due = (int)k;
	}
	if (due < 0) {
		return;
	}
	for (int k = 0; k < due; k++) {
		buffers[ready[k].buffer].status = BUFFER_DECODER;
		decoder->return_buffer(generation, ready[k].buffer);
		frames_dropped++;
	}
	const ReadyFrame show = ready[due];
	ready.remove_at(0); // Fewer than a handful of entries.
	for (int k = 0; k < due; k++) {
		ready.remove_at(0);
	}
	_show(show, frames_drawn);
}

Error HWVideoTexture::open(const String &p_path, const String &p_codec, double p_fps) {
	close();
	last_error = String();
	failed_generation = UINT32_MAX;
	clock = 0.0;
	frame_time = -1.0;
	serial = 0;
	playing = false;
	frames_shown = frames_dropped = frames_stale = 0;
	const Error err = decoder->open(p_path, p_codec, p_fps);
	if (err != OK) {
		last_error = decoder->get_last_error();
	}
	return err;
}

void HWVideoTexture::close() {
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
	buffers.clear();
	ready.clear();
	shown = -1;
	have_orphan = false;
	have_generation = false;
	decoded_size = Size2i();
	visible_rect = Rect2i();
	playing = false;
	decoder->close();
}

void HWVideoTexture::play() {
	playing = true;
}

void HWVideoTexture::pause() {
	playing = false;
}

void HWVideoTexture::seek_to_start() {
	serial = (uint32_t)decoder->seek_to_start();
	clock = 0.0;
	for (const ReadyFrame &ready_frame : ready) {
		buffers[ready_frame.buffer].status = BUFFER_DECODER;
		decoder->return_buffer(generation, ready_frame.buffer);
	}
	frames_stale += ready.size();
	ready.clear();
}

Ref<Texture2D> HWVideoTexture::get_plane_texture(int p_plane) const {
	ERR_FAIL_INDEX_V(p_plane, (int)DmabufImage::MAX_PLANES, Ref<Texture2D>());
	return plane_textures[p_plane];
}

RID HWVideoTexture::get_plane_rd_rid(int p_plane) const {
	ERR_FAIL_INDEX_V(p_plane, (int)DmabufImage::MAX_PLANES, RID());
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
	Dictionary d = decoder->get_stats();
	d["frames_shown"] = frames_shown;
	d["frames_dropped"] = frames_dropped;
	d["frames_stale"] = frames_stale;
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
	ClassDB::bind_method(D_METHOD("open", "path", "codec", "fps"), &HWVideoTexture::open);
	ClassDB::bind_method(D_METHOD("close"), &HWVideoTexture::close);
	ClassDB::bind_method(D_METHOD("update", "delta", "master_clock"), &HWVideoTexture::update, DEFVAL(-1.0));
	ClassDB::bind_method(D_METHOD("play"), &HWVideoTexture::play);
	ClassDB::bind_method(D_METHOD("pause"), &HWVideoTexture::pause);
	ClassDB::bind_method(D_METHOD("is_playing"), &HWVideoTexture::is_playing);
	ClassDB::bind_method(D_METHOD("seek_to_start"), &HWVideoTexture::seek_to_start);
	ClassDB::bind_method(D_METHOD("get_position"), &HWVideoTexture::get_position);
	ClassDB::bind_method(D_METHOD("get_frame_time"), &HWVideoTexture::get_frame_time);
	ClassDB::bind_method(D_METHOD("has_frame"), &HWVideoTexture::has_frame);
	ClassDB::bind_method(D_METHOD("get_decoder"), &HWVideoTexture::get_decoder);
	ClassDB::bind_method(D_METHOD("get_plane_texture", "plane"), &HWVideoTexture::get_plane_texture);
	ClassDB::bind_method(D_METHOD("get_plane_rd_rid", "plane"), &HWVideoTexture::get_plane_rd_rid);
	ClassDB::bind_method(D_METHOD("get_decoded_size"), &HWVideoTexture::get_decoded_size);
	ClassDB::bind_method(D_METHOD("get_visible_rect"), &HWVideoTexture::get_visible_rect);
	ClassDB::bind_method(D_METHOD("get_visible_uv_rect"), &HWVideoTexture::get_visible_uv_rect);
	ClassDB::bind_method(D_METHOD("get_last_error"), &HWVideoTexture::get_last_error);
	ClassDB::bind_method(D_METHOD("get_stats"), &HWVideoTexture::get_stats);

	ADD_SIGNAL(MethodInfo("format_changed"));
}
