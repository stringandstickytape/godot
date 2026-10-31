/**************************************************************************/
/*  dmabuf_image.h                                                        */
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

#include "core/object/ref_counted.h"
#include "core/os/mutex.h"
#include "core/templates/local_vector.h"
#include "core/templates/vector.h"
#include "core/variant/variant.h"

#include "drivers/vulkan/godot_vulkan.h"
#include "servers/rendering/rendering_device.h"

// A dmabuf (e.g. a decoded video frame) imported once as a multi-planar VkImage, with one RenderingDevice texture per plane.
// The memory is shared, not copied. The producer and the GPU take turns: acquire() after the producer has written a frame,
// sample the planes, release() before handing the buffer back to the producer.
//
// When the driver can't make plane views of both formats on one image without giving up the image's layout (Turnip keeps
// UBWC on a mutable NV12 image only if its format list has a single format), the dmabuf is imported twice, as two VkImages
// with the same layout: one for the R8 view of the luma plane, one for the R8G8 view of the chroma plane ("split").
//
// YCbCr import (import_ycbcr(), for Q10C: UBWC-compressed TP10, which has no plane view format): the dmabuf is one
// multi-planar image read only through a sampler with a YCbCr conversion, the way the driver decompresses it. A compute
// pass (recorded by acquire(), in the RenderingDevice graph) writes it into two textures the caller owns, an R16 luma and
// an R16G16 chroma texture holding the codes as P010 does (in the high bits), so the shaders read it like P010.
struct DmabufYUVFormat;
struct DmabufYCbCrPipeline;

class DmabufImage : public RefCounted {
	GDCLASS(DmabufImage, RefCounted);

public:
	enum {
		MAX_PLANES = 2,
	};

private:
	// A YCbCr import (see above): its view, descriptor set, and what the driver callback that records the pass needs.
	struct YCbCrState {
		DmabufYCbCrPipeline *pipeline = nullptr;
		VkImage image = VK_NULL_HANDLE;
		VkImageView view = VK_NULL_HANDLE;
		VkDescriptorPool pool = VK_NULL_HANDLE;
		VkDescriptorSet set = VK_NULL_HANDLE;
		uint32_t width = 0;
		uint32_t height = 0;
		bool external = true; // An imported dmabuf (foreign queue family, GENERAL layout between uses).
		bool preinitialized = false; // A host test pattern not sampled yet (PREINITIALIZED layout).
		uint32_t foreign_queue_family = 0;
		RID out[MAX_PLANES];
		uint64_t conversions = 0;
	};

	struct PendingFree {
		VkDevice device = VK_NULL_HANDLE;
		VkImage images[MAX_PLANES] = {};
		VkDeviceMemory memories[MAX_PLANES] = {};
		VkImageView view = VK_NULL_HANDLE;
		VkDescriptorPool pool = VK_NULL_HANDLE;
		YCbCrState *ycbcr = nullptr;
		uint64_t frame = 0;
	};

	static Mutex pending_frees_mutex;
	static LocalVector<PendingFree> pending_frees;

	VkDevice vk_device = VK_NULL_HANDLE;
	// One image for all planes, or one per plane when split.
	VkImage vk_images[MAX_PLANES] = {};
	VkDeviceMemory vk_memories[MAX_PLANES] = {};
	uint32_t image_count = 0;
	RID planes[MAX_PLANES];
	uint32_t plane_count = 0;
	Size2i size;
	uint64_t drm_modifier = 0;
	bool acquired = false;
	YCbCrState *ycbcr = nullptr; // A YCbCr import: planes[] are the caller's out textures (not ours to free).
	String ycbcr_description;

	static void _process_pending_frees();
	static Error _create_test_pattern_dumb(const String &p_device, uint32_t p_width, uint32_t p_height, int &r_fd, uint64_t &r_pitch, uint64_t &r_uv_offset);
	static Error _create_test_pattern_udmabuf(uint32_t p_width, uint32_t p_height, int &r_fd, uint64_t &r_pitch, uint64_t &r_uv_offset);
	static void _fill_test_pattern(uint8_t *p_luma, uint64_t p_luma_pitch, uint8_t *p_chroma, uint64_t p_chroma_pitch, uint32_t p_width, uint32_t p_height, uint32_t p_bytes);

	Error _create_planes(VkDevice p_device, const VkImage *p_images, const VkDeviceMemory *p_memories, uint32_t p_image_count, int p_width, int p_height, uint64_t p_drm_modifier, const RD::DataFormat *p_plane_formats);
	Error _import_yuv420(const DmabufYUVFormat &p_yuv, int p_fd, int p_width, int p_height, int64_t p_drm_modifier, const PackedInt64Array &p_plane_offsets, const PackedInt64Array &p_plane_pitches);
	Error _import_host_test_pattern(int p_width, int p_height, bool p_split, bool p_p010);
	Error _transfer(bool p_acquire);
	Error _import_planes_bind(int p_fd, RD::DataFormat p_format, int64_t p_drm_modifier, const PackedVector2Array &p_sizes, const PackedInt64Array &p_offsets, const PackedInt64Array &p_pitches, bool p_mutable);
	Error _setup_ycbcr(VkDevice p_device, VkImage p_image, VkDeviceMemory p_memory, uint32_t p_vk_format, uint64_t p_features, int p_width, int p_height, uint64_t p_modifier, bool p_external, RID p_out_y, RID p_out_cbcr);
	static void _record_ycbcr(RenderingDeviceDriver *p_driver, RDD::CommandBufferID p_command_buffer, void *p_userdata);
	Error _import_ycbcr_bind(int p_fd, int p_vk_format, int64_t p_modifier, int p_width, int p_height, const PackedInt64Array &p_offsets, const PackedInt64Array &p_pitches, RID p_out_y, RID p_out_cbcr);
	static Array _get_q10c_candidates_bind();

protected:
	static void _bind_methods();

public:
	static void flush_pending_frees();

	Error import_nv12(int p_fd, int p_width, int p_height, int64_t p_drm_modifier, const PackedInt64Array &p_plane_offsets, const PackedInt64Array &p_plane_pitches);
	// P010 (16-bit samples, 10 valid bits at the top) as G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16; the plane textures are R16 and R16G16.
	Error import_p010(int p_fd, int p_width, int p_height, int64_t p_drm_modifier, const PackedInt64Array &p_plane_offsets, const PackedInt64Array &p_plane_pitches);
	Error import_test_pattern(int p_width, int p_height, const String &p_device = "/dev/dri/card0");
	// Imports each plane of a dmabuf as a single-plane image of p_format, one memory plane each (e.g. Q10C: each plane is laid
	// out like a UBWC image of a 32-bit format). Plane i is the whole of image i. Experimental; see Q10C in HWVideoTexture.
	Error import_planes(int p_fd, RD::DataFormat p_format, int64_t p_drm_modifier, const Size2i *p_sizes, const uint64_t *p_offsets, const uint64_t *p_pitches, uint32_t p_count, bool p_mutable);
	void free_image();

	// A YCbCr candidate for Q10C (DRM format modifier and plane layout of a 2-plane 4:2:0 VkFormat), see get_q10c_candidates().
	struct YCbCrCandidate {
		uint32_t format = 0; // VkFormat.
		uint64_t modifier = 0;
		uint32_t memory_planes = 0;
		// Where the memory planes start in iris's Q10C buffer (Y metadata, Y, UV metadata, UV): "meta" (2 planes, each at its UBWC
		// metadata: Turnip's QCOM_COMPRESSED convention), "data" (2 planes, each at its pixels), "4plane" (Y, UV, Y metadata,
		// UV metadata) or "1plane".
		String layout;
		String source; // Why it is a candidate ("listed by the driver", "HWVIDEO_Q10C_MODIFIER", ...).
		String describe() const;
	};
	// The (format, modifier, layout) combinations to try for Q10C, best first: HWVIDEO_Q10C_FORMAT, HWVIDEO_Q10C_MODIFIER and
	// HWVIDEO_Q10C_LAYOUT override; otherwise every non-linear modifier the driver lists for a 10-bit 2-plane 4:2:0 format
	// (G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16 by default) that it says it can import from a dmabuf and sample. Empty with
	// HWVIDEO_Q10C=0, without the Vulkan features, or when nothing fits (get_q10c_report() says why). Looked once per process.
	static Vector<YCbCrCandidate> get_q10c_candidates();
	static String get_q10c_report();
	// Whether 10-bit can go to Q10C through the YCbCr import: there is a candidate and it hasn't failed in this run.
	static bool is_q10c_available();
	// After a Q10C failure (no candidate imported, or the picture check said garbage): off for the rest of the run.
	static void disable_q10c(const String &p_reason);
	static String get_q10c_disabled_reason();
	// The candidate that imported (an index into get_q10c_candidates()), -1 before the first import.
	static int get_q10c_chosen();
	static void set_q10c_chosen(int p_index);
	// Imports a 2-plane 4:2:0 dmabuf as one image of p_vk_format with the explicit modifier and memory planes, for the YCbCr pass
	// into p_out_y (R16, full size) and p_out_cbcr (R16G16, half size; both need storage use). get_plane_rid() returns those.
	Error import_ycbcr(int p_fd, uint32_t p_vk_format, uint64_t p_modifier, int p_width, int p_height, const uint64_t *p_offsets, const uint64_t *p_pitches, uint32_t p_count, RID p_out_y, RID p_out_cbcr);
	// The same pass on a linear host-memory test pattern (G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16, or G8_B8R8 with p_bits 8), for
	// drivers without dmabufs (lavapipe): the output must equal the P010 form of the pattern.
	Error import_ycbcr_test_pattern(int p_width, int p_height, RID p_out_y, RID p_out_cbcr, int p_bits = 10);
	bool is_ycbcr() const { return ycbcr != nullptr; }
	String get_ycbcr_description() const { return ycbcr_description; }
	uint64_t get_ycbcr_conversions() const { return ycbcr ? ycbcr->conversions : 0; }

	// For a YCbCr import, acquire() records the pass into the out textures (render thread) and release() does nothing.
	Error acquire();
	Error release();
	bool is_acquired() const { return acquired; }

	bool is_imported() const { return vk_images[0] != VK_NULL_HANDLE; }
	// 1, or 2 when the planes are views of two images over the same memory (see above).
	int get_image_count() const { return image_count; }
	RID get_plane_rid(int p_plane) const;
	Size2i get_size() const { return size; }
	int64_t get_drm_modifier() const { return (int64_t)drm_modifier; }

	static PackedInt64Array get_supported_nv12_modifiers();

	~DmabufImage();
};
