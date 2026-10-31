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
struct DmabufYUVFormat;

class DmabufImage : public RefCounted {
	GDCLASS(DmabufImage, RefCounted);

public:
	enum {
		MAX_PLANES = 2,
	};

private:
	struct PendingFree {
		VkDevice device = VK_NULL_HANDLE;
		VkImage images[MAX_PLANES] = {};
		VkDeviceMemory memories[MAX_PLANES] = {};
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

	static void _process_pending_frees();
	static Error _create_test_pattern_dumb(const String &p_device, uint32_t p_width, uint32_t p_height, int &r_fd, uint64_t &r_pitch, uint64_t &r_uv_offset);
	static Error _create_test_pattern_udmabuf(uint32_t p_width, uint32_t p_height, int &r_fd, uint64_t &r_pitch, uint64_t &r_uv_offset);
	static void _fill_test_pattern(uint8_t *p_luma, uint64_t p_luma_pitch, uint8_t *p_chroma, uint64_t p_chroma_pitch, uint32_t p_width, uint32_t p_height, uint32_t p_bytes);

	Error _create_planes(VkDevice p_device, const VkImage *p_images, const VkDeviceMemory *p_memories, uint32_t p_image_count, int p_width, int p_height, uint64_t p_drm_modifier, const RD::DataFormat *p_plane_formats);
	Error _import_yuv420(const DmabufYUVFormat &p_yuv, int p_fd, int p_width, int p_height, int64_t p_drm_modifier, const PackedInt64Array &p_plane_offsets, const PackedInt64Array &p_plane_pitches);
	Error _import_host_test_pattern(int p_width, int p_height, bool p_split, bool p_p010);
	Error _transfer(bool p_acquire);
	Error _import_planes_bind(int p_fd, RD::DataFormat p_format, int64_t p_drm_modifier, const PackedVector2Array &p_sizes, const PackedInt64Array &p_offsets, const PackedInt64Array &p_pitches, bool p_mutable);

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
