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

// A dmabuf (e.g. a decoded video frame) imported once as a multi-planar VkImage, with one RenderingDevice texture per plane.
// The memory is shared, not copied. The producer and the GPU take turns: acquire() after the producer has written a frame,
// sample the planes, release() before handing the buffer back to the producer.
class DmabufImage : public RefCounted {
	GDCLASS(DmabufImage, RefCounted);

public:
	enum {
		MAX_PLANES = 2,
	};

private:
	struct PendingFree {
		VkDevice device = VK_NULL_HANDLE;
		VkImage image = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		uint64_t frame = 0;
	};

	static Mutex pending_frees_mutex;
	static LocalVector<PendingFree> pending_frees;

	VkDevice vk_device = VK_NULL_HANDLE;
	VkImage vk_image = VK_NULL_HANDLE;
	VkDeviceMemory vk_memory = VK_NULL_HANDLE;
	RID planes[MAX_PLANES];
	uint32_t plane_count = 0;
	Size2i size;
	uint64_t drm_modifier = 0;
	bool acquired = false;

	static void _process_pending_frees();
	static Error _create_test_pattern_dumb(const String &p_device, uint32_t p_width, uint32_t p_height, int &r_fd, uint64_t &r_pitch, uint64_t &r_uv_offset);
	static Error _create_test_pattern_udmabuf(uint32_t p_width, uint32_t p_height, int &r_fd, uint64_t &r_pitch, uint64_t &r_uv_offset);
	static void _fill_test_pattern(uint8_t *p_luma, uint64_t p_luma_pitch, uint8_t *p_chroma, uint64_t p_chroma_pitch, uint32_t p_width, uint32_t p_height);

	Error _create_planes(VkDevice p_device, VkImage p_image, VkDeviceMemory p_memory, int p_width, int p_height, uint64_t p_drm_modifier);
	Error _import_host_test_pattern(int p_width, int p_height);

protected:
	static void _bind_methods();

public:
	static void flush_pending_frees();

	Error import_nv12(int p_fd, int p_width, int p_height, int64_t p_drm_modifier, const PackedInt64Array &p_plane_offsets, const PackedInt64Array &p_plane_pitches);
	Error import_test_pattern(int p_width, int p_height, const String &p_device = "/dev/dri/card0");
	void free_image();

	Error acquire();
	Error release();
	bool is_acquired() const { return acquired; }

	bool is_imported() const { return vk_image != VK_NULL_HANDLE; }
	RID get_plane_rid(int p_plane) const;
	Size2i get_size() const { return size; }
	int64_t get_drm_modifier() const { return (int64_t)drm_modifier; }

	static PackedInt64Array get_supported_nv12_modifiers();

	~DmabufImage();
};
