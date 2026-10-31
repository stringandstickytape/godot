/**************************************************************************/
/*  x11_dmabuf_image.cpp                                                  */
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

#include "x11_dmabuf_image.h"

#include "core/object/class_db.h"
#include "core/os/os.h"
#include "drivers/vulkan/rendering_device_driver_vulkan.h"
#include "servers/rendering/rendering_device.h"

#include <drm/drm_fourcc.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

static constexpr uint32_t MAX_MEMORY_PLANES = 4;

Mutex X11DmabufImage::pending_frees_mutex;
LocalVector<X11DmabufImage::PendingFree> X11DmabufImage::pending_frees;

static String _modifier_to_string(uint64_t p_modifier) {
	return "0x" + String::num_uint64(p_modifier, 16);
}

static bool _fourcc_formats(uint32_t p_fourcc, VkFormat &r_vk_format, RD::DataFormat &r_rd_format) {
	switch (p_fourcc) {
		case DRM_FORMAT_XRGB8888:
		case DRM_FORMAT_ARGB8888:
			// Little endian B, G, R, X/A bytes.
			r_vk_format = VK_FORMAT_B8G8R8A8_UNORM;
			r_rd_format = RD::DATA_FORMAT_B8G8R8A8_UNORM;
			return true;
		case DRM_FORMAT_XRGB2101010:
		case DRM_FORMAT_ARGB2101010:
			r_vk_format = VK_FORMAT_A2R10G10B10_UNORM_PACK32;
			r_rd_format = RD::DATA_FORMAT_A2R10G10B10_UNORM_PACK32;
			return true;
		default:
			return false;
	}
}

uint32_t X11DmabufImage::fourcc_for_depth(int p_depth, int p_bpp) {
	if (p_bpp != 32) {
		return 0;
	}
	switch (p_depth) {
		case 24:
			return DRM_FORMAT_XRGB8888;
		case 32:
			return DRM_FORMAT_ARGB8888;
		case 30:
			return DRM_FORMAT_XRGB2101010;
		default:
			return 0;
	}
}

bool X11DmabufImage::is_supported_fourcc(uint32_t p_fourcc) {
	VkFormat vk_format;
	RD::DataFormat rd_format;
	return _fourcc_formats(p_fourcc, vk_format, rd_format);
}

bool X11DmabufImage::fourcc_has_alpha(uint32_t p_fourcc) {
	return p_fourcc == DRM_FORMAT_ARGB8888 || p_fourcc == DRM_FORMAT_ARGB2101010;
}

String X11DmabufImage::fourcc_to_string(uint32_t p_fourcc) {
	char s[5] = { char(p_fourcc & 0xff), char((p_fourcc >> 8) & 0xff), char((p_fourcc >> 16) & 0xff), char((p_fourcc >> 24) & 0xff), 0 };
	return String(s);
}

static RenderingDeviceDriverVulkan *_get_vulkan_driver(String &r_error) {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (rd == nullptr || rd->get_device_api_name() != "Vulkan") {
		r_error = "The dmabuf path needs the Vulkan RenderingDevice.";
		return nullptr;
	}
	return static_cast<RenderingDeviceDriverVulkan *>(rd->get_device_driver());
}

static void _get_modifier_properties(VkPhysicalDevice p_physical_device, VkFormat p_format, LocalVector<VkDrmFormatModifierPropertiesEXT> &r_properties) {
	VkDrmFormatModifierPropertiesListEXT modifier_list = {};
	modifier_list.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT;
	VkFormatProperties2 format_properties = {};
	format_properties.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
	format_properties.pNext = &modifier_list;
	vkGetPhysicalDeviceFormatProperties2(p_physical_device, p_format, &format_properties);
	r_properties.resize(modifier_list.drmFormatModifierCount);
	modifier_list.pDrmFormatModifierProperties = r_properties.ptr();
	vkGetPhysicalDeviceFormatProperties2(p_physical_device, p_format, &format_properties);
	r_properties.resize(modifier_list.drmFormatModifierCount);
}

String X11DmabufImage::describe_modifiers(uint32_t p_fourcc) {
	String error;
	VkFormat vk_format;
	RD::DataFormat rd_format;
	if (!_fourcc_formats(p_fourcc, vk_format, rd_format) || _get_vulkan_driver(error) == nullptr) {
		return String();
	}
	VkPhysicalDevice physical_device = (VkPhysicalDevice)RenderingDevice::get_singleton()->get_driver_resource(RD::DRIVER_RESOURCE_PHYSICAL_DEVICE);
	LocalVector<VkDrmFormatModifierPropertiesEXT> properties;
	_get_modifier_properties(physical_device, vk_format, properties);
	String result;
	for (const VkDrmFormatModifierPropertiesEXT &p : properties) {
		result += vformat("%s%s (%d plane%s%s)", result.is_empty() ? "" : ", ", _modifier_to_string(p.drmFormatModifier), p.drmFormatModifierPlaneCount,
				p.drmFormatModifierPlaneCount == 1 ? "" : "s", (p.drmFormatModifierTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) ? "" : ", not sampleable");
	}
	return result;
}

Error X11DmabufImage::import(int p_fd, int p_width, int p_height, uint32_t p_fourcc, uint64_t p_modifier, uint32_t p_plane_count, const uint32_t *p_offsets, const uint32_t *p_pitches, String &r_error) {
	ERR_FAIL_COND_V(is_imported(), ERR_ALREADY_IN_USE);
	ERR_FAIL_COND_V(p_fd < 0 || p_width <= 0 || p_height <= 0, ERR_INVALID_PARAMETER);

	VkFormat vk_format;
	RD::DataFormat rd_format;
	if (!_fourcc_formats(p_fourcc, vk_format, rd_format)) {
		r_error = vformat("Format %s can't be imported.", fourcc_to_string(p_fourcc));
		return ERR_UNAVAILABLE;
	}
	if (p_modifier == DRM_FORMAT_MOD_INVALID) {
		r_error = "The buffer has no explicit DRM format modifier (implicit layout), which can't be imported.";
		return ERR_UNAVAILABLE;
	}
	if (p_plane_count == 0 || p_plane_count > MAX_MEMORY_PLANES) {
		r_error = vformat("%d memory planes.", p_plane_count);
		return ERR_INVALID_PARAMETER;
	}

	RenderingDeviceDriverVulkan *driver = _get_vulkan_driver(r_error);
	if (driver == nullptr) {
		return ERR_UNAVAILABLE;
	}
	const char *required_extensions[] = {
		VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
		VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
		VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
	};
	for (const char *extension : required_extensions) {
		if (!driver->is_device_extension_enabled(extension)) {
			r_error = vformat("Device extension %s is not enabled.", extension);
			return ERR_UNAVAILABLE;
		}
	}

	process_pending_frees();

	RenderingDevice *rd = RenderingDevice::get_singleton();
	VkDevice device = (VkDevice)rd->get_driver_resource(RD::DRIVER_RESOURCE_LOGICAL_DEVICE);
	VkPhysicalDevice physical_device = (VkPhysicalDevice)rd->get_driver_resource(RD::DRIVER_RESOURCE_PHYSICAL_DEVICE);
	ERR_FAIL_COND_V(device == VK_NULL_HANDLE || physical_device == VK_NULL_HANDLE, ERR_UNAVAILABLE);
	PFN_vkGetMemoryFdPropertiesKHR get_memory_fd_properties = (PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(device, "vkGetMemoryFdPropertiesKHR");
	ERR_FAIL_NULL_V(get_memory_fd_properties, ERR_UNAVAILABLE);

	// The modifier must be sampleable for the format and have as many memory planes as the X server gave.
	{
		LocalVector<VkDrmFormatModifierPropertiesEXT> properties;
		_get_modifier_properties(physical_device, vk_format, properties);
		const VkDrmFormatModifierPropertiesEXT *modifier_properties = nullptr;
		for (const VkDrmFormatModifierPropertiesEXT &p : properties) {
			if (p.drmFormatModifier == p_modifier) {
				modifier_properties = &p;
			}
		}
		if (modifier_properties == nullptr) {
			r_error = vformat("Modifier %s is not supported for %s. Supported: %s.", _modifier_to_string(p_modifier), fourcc_to_string(p_fourcc), describe_modifiers(p_fourcc));
			return ERR_UNAVAILABLE;
		}
		if (!(modifier_properties->drmFormatModifierTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
			r_error = vformat("%s with modifier %s can't be sampled.", fourcc_to_string(p_fourcc), _modifier_to_string(p_modifier));
			return ERR_UNAVAILABLE;
		}
		if (modifier_properties->drmFormatModifierPlaneCount != p_plane_count) {
			r_error = vformat("Modifier %s has %d memory plane(s), the X server gave %d.", _modifier_to_string(p_modifier), modifier_properties->drmFormatModifierPlaneCount, p_plane_count);
			return ERR_INVALID_PARAMETER;
		}
	}

	{
		VkPhysicalDeviceImageDrmFormatModifierInfoEXT modifier_info = {};
		modifier_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT;
		modifier_info.drmFormatModifier = p_modifier;
		modifier_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

		VkPhysicalDeviceExternalImageFormatInfo external_info = {};
		external_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO;
		external_info.pNext = &modifier_info;
		external_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

		VkPhysicalDeviceImageFormatInfo2 format_info = {};
		format_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
		format_info.pNext = &external_info;
		format_info.format = vk_format;
		format_info.type = VK_IMAGE_TYPE_2D;
		format_info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
		format_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT;

		VkExternalImageFormatProperties external_properties = {};
		external_properties.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES;
		VkImageFormatProperties2 image_format_properties = {};
		image_format_properties.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
		image_format_properties.pNext = &external_properties;

		VkResult err = vkGetPhysicalDeviceImageFormatProperties2(physical_device, &format_info, &image_format_properties);
		if (err != VK_SUCCESS) {
			r_error = vformat("%s + modifier %s + dmabuf import is not supported (VkResult %d).", fourcc_to_string(p_fourcc), _modifier_to_string(p_modifier), err);
			return ERR_UNAVAILABLE;
		}
		if (!(external_properties.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT)) {
			r_error = "dmabufs can't be imported for this image.";
			return ERR_UNAVAILABLE;
		}
		const VkExtent3D &max_extent = image_format_properties.imageFormatProperties.maxExtent;
		if ((uint32_t)p_width > max_extent.width || (uint32_t)p_height > max_extent.height) {
			r_error = vformat("Size %dx%d is larger than the maximum %dx%d.", p_width, p_height, max_extent.width, max_extent.height);
			return ERR_INVALID_PARAMETER;
		}
	}

	VkSubresourceLayout plane_layouts[MAX_MEMORY_PLANES] = {};
	for (uint32_t i = 0; i < p_plane_count; i++) {
		plane_layouts[i].offset = p_offsets[i];
		plane_layouts[i].rowPitch = p_pitches[i];
	}

	VkImageDrmFormatModifierExplicitCreateInfoEXT modifier_create_info = {};
	modifier_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT;
	modifier_create_info.drmFormatModifier = p_modifier;
	modifier_create_info.drmFormatModifierPlaneCount = p_plane_count;
	modifier_create_info.pPlaneLayouts = plane_layouts;

	VkExternalMemoryImageCreateInfo external_create_info = {};
	external_create_info.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
	external_create_info.pNext = &modifier_create_info;
	external_create_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

	VkImageCreateInfo image_create_info = {};
	image_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	image_create_info.pNext = &external_create_info;
	image_create_info.imageType = VK_IMAGE_TYPE_2D;
	image_create_info.format = vk_format;
	image_create_info.extent = { (uint32_t)p_width, (uint32_t)p_height, 1 };
	image_create_info.mipLevels = 1;
	image_create_info.arrayLayers = 1;
	image_create_info.samples = VK_SAMPLE_COUNT_1_BIT;
	image_create_info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
	image_create_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
	image_create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	image_create_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

	VkImage image = VK_NULL_HANDLE;
	VkResult err = vkCreateImage(device, &image_create_info, nullptr, &image);
	if (err != VK_SUCCESS) {
		r_error = vformat("vkCreateImage failed (VkResult %d; %d means the offsets or pitches don't suit the driver).", err, VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT);
		return ERR_CANT_CREATE;
	}

	VkMemoryRequirements memory_requirements = {};
	vkGetImageMemoryRequirements(device, image, &memory_requirements);

	VkMemoryFdPropertiesKHR fd_properties = {};
	fd_properties.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR;
	err = get_memory_fd_properties(device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, p_fd, &fd_properties);
	if (err != VK_SUCCESS) {
		vkDestroyImage(device, image, nullptr);
		r_error = vformat("vkGetMemoryFdPropertiesKHR failed (VkResult %d).", err);
		return ERR_CANT_CREATE;
	}
	const uint32_t memory_type_bits = memory_requirements.memoryTypeBits & fd_properties.memoryTypeBits;
	if (memory_type_bits == 0) {
		vkDestroyImage(device, image, nullptr);
		r_error = vformat("No memory type suits both the image (0x%x) and the dmabuf (0x%x).", memory_requirements.memoryTypeBits, fd_properties.memoryTypeBits);
		return ERR_CANT_CREATE;
	}
	uint32_t memory_type_index = 0;
	while (!(memory_type_bits & (1u << memory_type_index))) {
		memory_type_index++;
	}

	const off_t dmabuf_size = lseek(p_fd, 0, SEEK_END);
	if (dmabuf_size >= 0 && (uint64_t)dmabuf_size < memory_requirements.size) {
		vkDestroyImage(device, image, nullptr);
		r_error = vformat("The dmabuf holds %d bytes, the image needs %d.", (int64_t)dmabuf_size, (int64_t)memory_requirements.size);
		return ERR_INVALID_PARAMETER;
	}

	// A successful import takes ownership of the fd: give it a duplicate.
	const int import_fd = fcntl(p_fd, F_DUPFD_CLOEXEC, 0);
	if (import_fd < 0) {
		vkDestroyImage(device, image, nullptr);
		r_error = vformat("Couldn't duplicate the dmabuf fd: %s.", strerror(errno));
		return ERR_CANT_CREATE;
	}

	VkMemoryDedicatedAllocateInfo dedicated_info = {};
	dedicated_info.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
	dedicated_info.image = image;

	VkImportMemoryFdInfoKHR import_info = {};
	import_info.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
	import_info.pNext = &dedicated_info;
	import_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
	import_info.fd = import_fd;

	VkMemoryAllocateInfo allocate_info = {};
	allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocate_info.pNext = &import_info;
	allocate_info.allocationSize = memory_requirements.size;
	allocate_info.memoryTypeIndex = memory_type_index;

	VkDeviceMemory memory = VK_NULL_HANDLE;
	err = vkAllocateMemory(device, &allocate_info, nullptr, &memory);
	if (err != VK_SUCCESS) {
		close(import_fd);
		vkDestroyImage(device, image, nullptr);
		r_error = vformat("Importing the dmabuf failed (VkResult %d).", err);
		return ERR_CANT_CREATE;
	}
	err = vkBindImageMemory(device, image, memory, 0);
	if (err != VK_SUCCESS) {
		vkFreeMemory(device, memory, nullptr);
		vkDestroyImage(device, image, nullptr);
		r_error = vformat("vkBindImageMemory failed (VkResult %d).", err);
		return ERR_CANT_CREATE;
	}

	vk_device = device;
	vk_image = image;
	vk_memory = memory;
	size = Size2i(p_width, p_height);
	drm_modifier = p_modifier;
	acquired = false;

	base_texture = rd->texture_create_from_extension(RD::TEXTURE_TYPE_2D, rd_format, RD::TEXTURE_SAMPLES_1, RD::TEXTURE_USAGE_SAMPLING_BIT, (uint64_t)image, p_width, p_height, 1, 1, 1, -1);
	if (base_texture.is_null()) {
		free_image();
		r_error = "Couldn't create the RenderingDevice texture.";
		return ERR_CANT_CREATE;
	}
	view_texture = base_texture;
	if (!fourcc_has_alpha(p_fourcc)) {
		// The X byte of XRGB is undefined: sample it as opaque.
		RD::TextureView view;
		view.swizzle_a = RD::TEXTURE_SWIZZLE_ONE;
		view_texture = rd->texture_create_shared(view, base_texture);
		if (view_texture.is_null()) {
			free_image();
			r_error = "Couldn't create the opaque view of the texture.";
			return ERR_CANT_CREATE;
		}
	}
	print_verbose(vformat("X11WindowSource: imported a %dx%d %s dmabuf, modifier %s, %d plane(s), pitch %d, %d bytes, memory type %d.", p_width, p_height,
			fourcc_to_string(p_fourcc), _modifier_to_string(p_modifier), p_plane_count, p_pitches[0], (int64_t)memory_requirements.size, memory_type_index));
	return OK;
}

Error X11DmabufImage::acquire() {
	ERR_FAIL_COND_V(!is_imported(), ERR_UNCONFIGURED);
	if (acquired) {
		return OK;
	}
	process_pending_frees();
	Error err = RenderingDevice::get_singleton()->texture_external_acquire(base_texture);
	ERR_FAIL_COND_V(err != OK, err);
	acquired = true;
	return OK;
}

Error X11DmabufImage::release() {
	ERR_FAIL_COND_V(!is_imported(), ERR_UNCONFIGURED);
	if (!acquired) {
		return OK;
	}
	Error err = RenderingDevice::get_singleton()->texture_external_release(base_texture);
	ERR_FAIL_COND_V(err != OK, err);
	acquired = false;
	return OK;
}

void X11DmabufImage::free_image() {
	if (vk_image == VK_NULL_HANDLE) {
		return;
	}
	// The views are destroyed by the RenderingDevice once the frames that may use them are done; the image and its memory
	// a frame after that.
	RenderingDevice *rd = RenderingDevice::get_singleton();
	PendingFree pending;
	pending.device = vk_device;
	pending.image = vk_image;
	pending.memory = vk_memory;
	if (rd != nullptr) {
		if (view_texture.is_valid() && view_texture != base_texture) {
			rd->free_rid(view_texture);
		}
		if (base_texture.is_valid()) {
			rd->free_rid(base_texture);
		}
		pending.frame = rd->get_frames_drawn();
	}
	base_texture = RID();
	view_texture = RID();
	vk_device = VK_NULL_HANDLE;
	vk_image = VK_NULL_HANDLE;
	vk_memory = VK_NULL_HANDLE;
	size = Size2i();
	acquired = false;
	if (rd == nullptr) {
		return;
	}
	{
		MutexLock lock(pending_frees_mutex);
		pending_frees.push_back(pending);
	}
	process_pending_frees();
}

void X11DmabufImage::process_pending_frees() {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (rd == nullptr) {
		return;
	}
	const uint64_t frames_drawn = rd->get_frames_drawn();
	const uint64_t frame_delay = rd->get_frame_delay();
	MutexLock lock(pending_frees_mutex);
	for (uint32_t i = 0; i < pending_frees.size(); i++) {
		const PendingFree &pending = pending_frees[i];
		if (frames_drawn < pending.frame + frame_delay + 1) {
			continue;
		}
		vkDestroyImage(pending.device, pending.image, nullptr);
		vkFreeMemory(pending.device, pending.memory, nullptr);
		pending_frees.remove_at_unordered(i);
		i--;
	}
}

void X11DmabufImage::flush_pending_frees() {
	// At shutdown the RenderingDevice still holds views of these images until it is finalized (after the modules): leave
	// them to the process exit, as DmabufImage does.
	MutexLock lock(pending_frees_mutex);
	pending_frees.clear();
}

X11DmabufImage::~X11DmabufImage() {
	free_image();
}
