/**************************************************************************/
/*  x11_dmabuf_image.h                                                    */
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

#include "drivers/vulkan/godot_vulkan.h"

// A single-plane RGB dmabuf (an X pixmap exported through DRI3) imported once as a VkImage, with a RenderingDevice
// texture over it. Same mechanism as hwvideo's DmabufImage (explicit DRM format modifier, dedicated memory import,
// FOREIGN <-> graphics ownership transfers through the render graph), for packed RGB formats instead of NV12.
class X11DmabufImage : public RefCounted {
	GDCLASS(X11DmabufImage, RefCounted);

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
	RID base_texture; // Over the whole image; ownership transfers go through it.
	RID view_texture; // What is sampled: the base texture, or a view of it with alpha forced to 1.
	Size2i size;
	uint64_t drm_modifier = 0;
	bool acquired = false;

protected:
	static void _bind_methods() {}

public:
	// DRM fourcc values of the formats X servers export (little endian): 'XR24', 'AR24', 'XR30', 'AR30'.
	static uint32_t fourcc_for_depth(int p_depth, int p_bpp);
	static bool is_supported_fourcc(uint32_t p_fourcc);
	static bool fourcc_has_alpha(uint32_t p_fourcc);
	static String fourcc_to_string(uint32_t p_fourcc);
	// The modifiers the device can sample for the format, e.g. for logs.
	static String describe_modifiers(uint32_t p_fourcc);

	// p_fd stays the caller's. One offset and pitch per memory plane of the modifier.
	Error import(int p_fd, int p_width, int p_height, uint32_t p_fourcc, uint64_t p_modifier, uint32_t p_plane_count, const uint32_t *p_offsets, const uint32_t *p_pitches, String &r_error);
	void free_image();

	// Render thread (RenderingServer::call_on_render_thread()).
	Error acquire();
	Error release();
	bool is_acquired() const { return acquired; }

	bool is_imported() const { return vk_image != VK_NULL_HANDLE; }
	RID get_texture() const { return view_texture; }
	Size2i get_size() const { return size; }
	uint64_t get_drm_modifier() const { return drm_modifier; }

	static void process_pending_frees();
	static void flush_pending_frees();

	~X11DmabufImage();
};
