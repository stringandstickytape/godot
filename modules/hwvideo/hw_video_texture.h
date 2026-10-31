/**************************************************************************/
/*  hw_video_texture.h                                                    */
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

#include "dmabuf_image.h"
#include "software_video_decoder.h"
#include "v4l2_decoder.h"

#include "scene/resources/texture.h"

// One plane of the current video frame, as a Texture2D that a ShaderMaterial can use. Its RenderingServer RID is a proxy
// that is pointed at the plane of whichever decoder buffer is shown, so the material never has to change.
class HWVideoPlaneTexture : public Texture2D {
	GDCLASS(HWVideoPlaneTexture, Texture2D);

	RID placeholder;
	RID proxy;
	Size2i size;

protected:
	static void _bind_methods() {}

public:
	void init(int p_plane);
	// An invalid RID shows the placeholder (black for plane 0, neutral chroma for plane 1).
	void set_source(RID p_texture, const Size2i &p_size);

	virtual int get_width() const override { return size.width; }
	virtual int get_height() const override { return size.height; }
	virtual RID get_rid() const override { return proxy; }
	virtual bool has_alpha() const override { return false; }

	~HWVideoPlaneTexture();
};

// Plays a V4L2Decoder's output with no copy: every CAPTURE buffer is imported once as a DmabufImage (per CAPTURE setup),
// the frame due at the clock is shown through two plane textures, and buffers go back to the decoder only once the GPU is
// done with them. Call update() once per frame from the main thread. The input is a raw Annex-B file (open()) or a
// MediaSource (open_source()); with a source, seeks go through it and are picked up here by its serial.
class HWVideoTexture : public RefCounted {
	GDCLASS(HWVideoTexture, RefCounted);

public:
	// How the plane textures hold the samples.
	enum Packing {
		PACKING_NV12, // Y as R8 (luminance), interleaved CbCr as R8G8 at half size (NV12 and Q08C).
		PACKING_TP10_RGBA8, // Q10C: each texel is a 32-bit word of three 10-bit samples, as R8G8B8A8 (bytes, little-endian).
		PACKING_TP10_RGB10A2, // Q10C: the same words as A2B10G10R10: r, g, b are the three samples.
	};

private:
	enum BufferStatus {
		BUFFER_DECODER, // Queued in the decoder (or never handed out).
		BUFFER_READY, // Decoded, waiting for its time.
		BUFFER_SHOWN, // Acquired by the GPU; the plane textures point at it.
		BUFFER_RETIRING, // Released; back to the decoder once the frames that sampled it are done.
	};

	struct Buffer {
		Ref<DmabufImage> image;
		RID planes[DmabufImage::MAX_PLANES]; // RenderingServer textures of the image's plane views.
		BufferStatus status = BUFFER_DECODER;
		uint64_t release_frame = 0;
	};

	struct ReadyFrame {
		uint32_t buffer = 0; // Hardware: the CAPTURE buffer. Software: the decoder's slot.
		double time = 0.0;
		uint32_t generation = 0; // Software: the frame size/range generation.
	};

	struct RetiringSlot {
		uint32_t slot = 0;
		uint64_t release_frame = 0;
	};

	Ref<V4L2Decoder> decoder;
	Ref<MediaSource> source;
	Ref<HWVideoPlaneTexture> plane_textures[DmabufImage::MAX_PLANES];
	LocalVector<Buffer> buffers;
	LocalVector<ReadyFrame> ready;
	uint32_t generation = 0;
	bool have_generation = false;
	uint32_t failed_generation = UINT32_MAX;
	int shown = -1;
	// The shown buffer of an earlier CAPTURE generation: freed once the next frame is shown.
	Buffer orphan;
	bool have_orphan = false;
	Size2i decoded_size;
	Rect2i visible_rect;
	Size2i plane_sizes[DmabufImage::MAX_PLANES];
	Packing packing = PACKING_NV12;
	// Q10C import experiment: "rgb10a2", "rgb10a2_mutable", "rgba8" or "rgba8_mutable" (see q10c_view in the docs).
	String q10c_view = "rgb10a2_mutable";

	// Software path, for what the hardware path can't show (open_source() picks it). Null on the hardware path.
	SoftwareVideoDecoder *software = nullptr;
	String software_reason; // Why the hardware path wasn't used.
	bool software_fallback = true;
	int software_threads = 4;
	int64_t software_max_pixels = SoftwareVideoDecoder::DEFAULT_MAX_PIXELS;
	RID software_rd[DmabufImage::MAX_PLANES]; // The shown frame is uploaded to these (Y R8, CbCr R8G8).
	RID software_rs[DmabufImage::MAX_PLANES];
	uint32_t software_generation = UINT32_MAX;
	int software_shown = -1; // Slot of the frame on screen.
	LocalVector<RetiringSlot> software_retiring; // Shown before; back to the decoder once the GPU upload is done.
	uint32_t serial = 0;
	double clock = 0.0;
	double frame_time = -1.0;
	bool playing = false;
	// After open or a seek: frames before the target are decoded only to reach it. Nothing is shown until the frame for
	// the target is known (the newest at or before it, once a later one has arrived), so a seek doesn't flash the frames
	// between the keyframe and the target.
	bool settling = false;
	double seek_target = 0.0;
	uint64_t frames_shown = 0;
	uint64_t frames_dropped = 0;
	uint64_t frames_stale = 0;
	uint64_t frames_skipped = 0;
	String last_error;

	static void _render_thread_swap(const Ref<DmabufImage> &p_release, const Ref<DmabufImage> &p_acquire, const Array &p_free);

	Error _import_generation(uint32_t p_generation);
	void _release_generation(Array &r_free);
	void _free_buffer(Buffer &p_buffer, Array &r_free);
	void _show(const ReadyFrame &p_frame, uint64_t p_frames_drawn);
	void _discard_ready(uint32_t p_count, bool p_skipped);
	void _return_all_ready();
	void _return_ready_buffer(const ReadyFrame &p_frame);
	bool _select_due(bool p_input_ended, ReadyFrame &r_show);
	bool _check_seek();
	void _update_software(double p_delta, double p_master_clock);
	void _show_software(const ReadyFrame &p_frame, uint64_t p_frames_drawn);
	void _free_software_textures();
	Dictionary _get_software_stats() const;
	static void _render_thread_upload(RID p_y, RID p_uv, const Vector<uint8_t> &p_y_data, const Vector<uint8_t> &p_uv_data);

protected:
	static void _bind_methods();

public:
	Error open(const String &p_path, const String &p_codec, double p_fps);
	// Plays the video stream of an open MediaSource. Play its get_audio_stream() in an AudioStreamPlayer and pass that
	// player's clock to update().
	Error open_source(const Ref<MediaSource> &p_source);
	void close();

	// Advances the clock by p_delta while playing, or sets it to p_master_clock when that is >= 0 (e.g. the audio
	// position), then shows the newest frame whose time is <= the clock. Older frames are dropped.
	void update(double p_delta, double p_master_clock = -1.0);

	void play();
	void pause();
	bool is_playing() const { return playing; }
	void seek_to_start();
	// With a MediaSource: MediaSource.seek() (audio and video). With a raw file only 0 is possible.
	Error seek(double p_seconds);
	double get_position() const { return clock; }
	double get_frame_time() const { return frame_time; }
	bool has_frame() const { return shown >= 0 || have_orphan || software_shown >= 0; }

	Ref<V4L2Decoder> get_decoder() const { return decoder; }
	Ref<MediaSource> get_media_source() const { return source; }
	Ref<Texture2D> get_plane_texture(int p_plane) const;
	RID get_plane_rd_rid(int p_plane) const;
	Size2i get_decoded_size() const { return decoded_size; }
	Rect2i get_visible_rect() const { return visible_rect; }
	Rect2 get_visible_uv_rect() const;
	Packing get_packing() const { return packing; }
	// "hardware" (V4L2 + dmabuf import), "software" (SoftwareVideoDecoder), or "" when closed.
	String get_decode_path() const;
	String get_software_reason() const { return software_reason; }
	void set_software_fallback(bool p_enabled) { software_fallback = p_enabled; }
	bool is_software_fallback() const { return software_fallback; }
	void set_software_threads(int p_threads) { software_threads = CLAMP(p_threads, 1, 16); }
	int get_software_threads() const { return software_threads; }
	void set_software_max_pixels(int64_t p_pixels) { software_max_pixels = MAX(p_pixels, (int64_t)64 * 64); }
	int64_t get_software_max_pixels() const { return software_max_pixels; }
	void set_q10c_view(const String &p_view);
	String get_q10c_view() const { return q10c_view; }
	String get_last_error() const { return last_error; }
	Dictionary get_stats() const;

	HWVideoTexture();
	~HWVideoTexture();
};

VARIANT_ENUM_CAST(HWVideoTexture::Packing);
