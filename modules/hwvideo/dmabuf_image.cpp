/**************************************************************************/
/*  dmabuf_image.cpp                                                      */
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

#include "dmabuf_image.h"

#include "core/object/class_db.h"
#include "core/os/os.h"
#include "drivers/vulkan/rendering_device_driver_vulkan.h"
#include "servers/rendering/rendering_device.h"

#include <drm/drm.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_mode.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <linux/udmabuf.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

// Vulkan allows up to 4 memory planes per image (e.g. 2 data + 2 compression metadata planes for UBWC NV12).
static constexpr uint32_t MAX_MEMORY_PLANES = 4;
static constexpr VkFormat NV12_FORMAT = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
// The image format plus the formats of the plane views made by import_nv12().
static const VkFormat NV12_VIEW_FORMATS[] = { NV12_FORMAT, VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM };

Mutex DmabufImage::pending_frees_mutex;
LocalVector<DmabufImage::PendingFree> DmabufImage::pending_frees;

static uint64_t _align(uint64_t p_value, uint64_t p_alignment) {
	return (p_value + p_alignment - 1) / p_alignment * p_alignment;
}

static RenderingDeviceDriverVulkan *_get_vulkan_driver() {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	ERR_FAIL_NULL_V_MSG(rd, nullptr, "DmabufImage requires a RenderingDevice-based renderer.");
	ERR_FAIL_COND_V_MSG(rd->get_device_api_name() != "Vulkan", nullptr, "DmabufImage requires the Vulkan rendering driver.");
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

static String _modifier_to_string(uint64_t p_modifier) {
	return "0x" + String::num_uint64(p_modifier, 16);
}

PackedInt64Array DmabufImage::get_supported_nv12_modifiers() {
	PackedInt64Array modifiers;
	RenderingDeviceDriverVulkan *driver = _get_vulkan_driver();
	ERR_FAIL_NULL_V(driver, modifiers);
	ERR_FAIL_COND_V_MSG(!driver->is_device_extension_enabled(VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME), modifiers, "VK_EXT_image_drm_format_modifier is not enabled.");

	VkPhysicalDevice physical_device = (VkPhysicalDevice)RenderingDevice::get_singleton()->get_driver_resource(RD::DRIVER_RESOURCE_PHYSICAL_DEVICE);
	LocalVector<VkDrmFormatModifierPropertiesEXT> properties;
	_get_modifier_properties(physical_device, NV12_FORMAT, properties);
	for (const VkDrmFormatModifierPropertiesEXT &p : properties) {
		print_verbose(vformat("DmabufImage: NV12 modifier %s, %d memory plane(s), tiling features 0x%x.", _modifier_to_string(p.drmFormatModifier), p.drmFormatModifierPlaneCount, (int64_t)p.drmFormatModifierTilingFeatures));
		if (p.drmFormatModifierTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) {
			modifiers.push_back((int64_t)p.drmFormatModifier);
		}
	}
	return modifiers;
}

Error DmabufImage::import_nv12(int p_fd, int p_width, int p_height, int64_t p_drm_modifier, const PackedInt64Array &p_plane_offsets, const PackedInt64Array &p_plane_pitches) {
	ERR_FAIL_COND_V_MSG(is_imported(), ERR_ALREADY_IN_USE, "An image is already imported. Call free_image() first.");
	ERR_FAIL_COND_V(p_fd < 0, ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V_MSG(p_width <= 0 || p_height <= 0 || (p_width % 2) != 0 || (p_height % 2) != 0, ERR_INVALID_PARAMETER, "NV12 width and height must be positive and even.");
	ERR_FAIL_COND_V_MSG(p_plane_offsets.is_empty() || p_plane_offsets.size() > (int)MAX_MEMORY_PLANES || p_plane_offsets.size() != p_plane_pitches.size(), ERR_INVALID_PARAMETER, "Give one offset and one pitch per memory plane (1 to 4).");

	RenderingDeviceDriverVulkan *driver = _get_vulkan_driver();
	ERR_FAIL_NULL_V(driver, ERR_UNAVAILABLE);
	const char *required_extensions[] = {
		VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
		VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
		VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
	};
	for (const char *extension : required_extensions) {
		ERR_FAIL_COND_V_MSG(!driver->is_device_extension_enabled(extension), ERR_UNAVAILABLE, vformat("Device extension %s is not enabled.", extension));
	}
	ERR_FAIL_COND_V_MSG(!driver->is_sampler_ycbcr_conversion_enabled(), ERR_UNAVAILABLE, "samplerYcbcrConversion is not enabled, multi-planar images can't be created.");
	if (!driver->is_device_extension_enabled(VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME)) {
		WARN_PRINT_ONCE("VK_EXT_queue_family_foreign is not enabled, ownership transfers will use VK_QUEUE_FAMILY_EXTERNAL.");
	}

	_process_pending_frees();

	RenderingDevice *rd = RenderingDevice::get_singleton();
	VkDevice device = (VkDevice)rd->get_driver_resource(RD::DRIVER_RESOURCE_LOGICAL_DEVICE);
	VkPhysicalDevice physical_device = (VkPhysicalDevice)rd->get_driver_resource(RD::DRIVER_RESOURCE_PHYSICAL_DEVICE);
	ERR_FAIL_COND_V(device == VK_NULL_HANDLE || physical_device == VK_NULL_HANDLE, ERR_UNAVAILABLE);

	PFN_vkGetMemoryFdPropertiesKHR get_memory_fd_properties = (PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(device, "vkGetMemoryFdPropertiesKHR");
	PFN_vkGetImageDrmFormatModifierPropertiesEXT get_image_drm_format_modifier_properties = (PFN_vkGetImageDrmFormatModifierPropertiesEXT)vkGetDeviceProcAddr(device, "vkGetImageDrmFormatModifierPropertiesEXT");
	ERR_FAIL_NULL_V(get_memory_fd_properties, ERR_UNAVAILABLE);
	ERR_FAIL_NULL_V(get_image_drm_format_modifier_properties, ERR_UNAVAILABLE);

	const uint64_t modifier = (uint64_t)p_drm_modifier;
	const uint32_t memory_plane_count = p_plane_offsets.size();

	// The modifier must be supported for NV12, sampleable, and have as many memory planes as we were given.
	{
		LocalVector<VkDrmFormatModifierPropertiesEXT> properties;
		_get_modifier_properties(physical_device, NV12_FORMAT, properties);
		const VkDrmFormatModifierPropertiesEXT *modifier_properties = nullptr;
		String supported;
		for (const VkDrmFormatModifierPropertiesEXT &p : properties) {
			supported += (supported.is_empty() ? "" : ", ") + _modifier_to_string(p.drmFormatModifier);
			if (p.drmFormatModifier == modifier) {
				modifier_properties = &p;
			}
		}
		ERR_FAIL_NULL_V_MSG(modifier_properties, ERR_UNAVAILABLE, vformat("DRM format modifier %s is not supported for NV12. Supported: %s.", _modifier_to_string(modifier), supported));
		ERR_FAIL_COND_V_MSG(!(modifier_properties->drmFormatModifierTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT), ERR_UNAVAILABLE, vformat("NV12 with modifier %s can't be sampled.", _modifier_to_string(modifier)));
		ERR_FAIL_COND_V_MSG(modifier_properties->drmFormatModifierPlaneCount != memory_plane_count, ERR_INVALID_PARAMETER, vformat("Modifier %s has %d memory plane(s), but %d offset(s) were given.", _modifier_to_string(modifier), modifier_properties->drmFormatModifierPlaneCount, memory_plane_count));
	}

	// Plane views (R8 and R8G8 over the planes) need a mutable format, which must be listed with DRM format modifier tiling.
	VkImageFormatListCreateInfo format_list = {};
	format_list.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO;
	format_list.viewFormatCount = std_size(NV12_VIEW_FORMATS);
	format_list.pViewFormats = NV12_VIEW_FORMATS;

	// Check the full combination up front: it gives a clearer error than vkCreateImage().
	{
		VkPhysicalDeviceImageDrmFormatModifierInfoEXT modifier_info = {};
		modifier_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT;
		modifier_info.pNext = &format_list;
		modifier_info.drmFormatModifier = modifier;
		modifier_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

		VkPhysicalDeviceExternalImageFormatInfo external_info = {};
		external_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO;
		external_info.pNext = &modifier_info;
		external_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

		VkPhysicalDeviceImageFormatInfo2 format_info = {};
		format_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
		format_info.pNext = &external_info;
		format_info.format = NV12_FORMAT;
		format_info.type = VK_IMAGE_TYPE_2D;
		format_info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
		format_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
		format_info.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;

		VkExternalImageFormatProperties external_properties = {};
		external_properties.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES;

		VkImageFormatProperties2 image_format_properties = {};
		image_format_properties.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
		image_format_properties.pNext = &external_properties;

		VkResult err = vkGetPhysicalDeviceImageFormatProperties2(physical_device, &format_info, &image_format_properties);
		ERR_FAIL_COND_V_MSG(err != VK_SUCCESS, ERR_UNAVAILABLE, vformat("NV12 + modifier %s + mutable format (plane views) + dmabuf import is not supported (VkResult %d).", _modifier_to_string(modifier), err));
		ERR_FAIL_COND_V_MSG(!(external_properties.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT), ERR_UNAVAILABLE, "dmabufs can't be imported for this image.");
		const VkExtent3D &max_extent = image_format_properties.imageFormatProperties.maxExtent;
		ERR_FAIL_COND_V_MSG((uint32_t)p_width > max_extent.width || (uint32_t)p_height > max_extent.height, ERR_INVALID_PARAMETER, vformat("Size %dx%d is larger than the maximum %dx%d.", p_width, p_height, max_extent.width, max_extent.height));
	}

	// One image, not disjoint: every plane lives in the same memory object, at the given offsets.
	VkSubresourceLayout plane_layouts[MAX_MEMORY_PLANES] = {};
	for (uint32_t i = 0; i < memory_plane_count; i++) {
		ERR_FAIL_COND_V(p_plane_offsets[i] < 0 || p_plane_pitches[i] <= 0, ERR_INVALID_PARAMETER);
		plane_layouts[i].offset = p_plane_offsets[i];
		plane_layouts[i].rowPitch = p_plane_pitches[i];
	}

	VkImageDrmFormatModifierExplicitCreateInfoEXT modifier_create_info = {};
	modifier_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT;
	modifier_create_info.pNext = &format_list;
	modifier_create_info.drmFormatModifier = modifier;
	modifier_create_info.drmFormatModifierPlaneCount = memory_plane_count;
	modifier_create_info.pPlaneLayouts = plane_layouts;

	VkExternalMemoryImageCreateInfo external_create_info = {};
	external_create_info.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
	external_create_info.pNext = &modifier_create_info;
	external_create_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

	VkImageCreateInfo image_create_info = {};
	image_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	image_create_info.pNext = &external_create_info;
	image_create_info.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
	image_create_info.imageType = VK_IMAGE_TYPE_2D;
	image_create_info.format = NV12_FORMAT;
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
	ERR_FAIL_COND_V_MSG(err != VK_SUCCESS, ERR_CANT_CREATE, vformat("vkCreateImage failed for NV12 dmabuf (VkResult %d). VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT (%d) means the offsets or pitches don't suit the driver.", err, VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT));

	VkMemoryRequirements memory_requirements = {};
	vkGetImageMemoryRequirements(device, image, &memory_requirements);

	VkMemoryFdPropertiesKHR fd_properties = {};
	fd_properties.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR;
	err = get_memory_fd_properties(device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, p_fd, &fd_properties);
	if (err != VK_SUCCESS) {
		vkDestroyImage(device, image, nullptr);
		ERR_FAIL_V_MSG(ERR_CANT_CREATE, vformat("vkGetMemoryFdPropertiesKHR failed (VkResult %d). Is the fd a dmabuf?", err));
	}

	const uint32_t memory_type_bits = memory_requirements.memoryTypeBits & fd_properties.memoryTypeBits;
	if (memory_type_bits == 0) {
		vkDestroyImage(device, image, nullptr);
		ERR_FAIL_V_MSG(ERR_CANT_CREATE, vformat("No memory type suits both the image (0x%x) and the dmabuf (0x%x).", memory_requirements.memoryTypeBits, fd_properties.memoryTypeBits));
	}
	uint32_t memory_type_index = 0;
	while (!(memory_type_bits & (1u << memory_type_index))) {
		memory_type_index++;
	}

	// The driver refuses to import less memory than the dmabuf holds; report it here rather than as a generic import failure.
	const off_t dmabuf_size = lseek(p_fd, 0, SEEK_END);
	if (dmabuf_size >= 0 && (uint64_t)dmabuf_size < memory_requirements.size) {
		vkDestroyImage(device, image, nullptr);
		ERR_FAIL_V_MSG(ERR_INVALID_PARAMETER, vformat("The dmabuf holds %d bytes, but the image needs %d.", (int64_t)dmabuf_size, (int64_t)memory_requirements.size));
	}

	// A successful import takes ownership of the fd, so give it a duplicate: the caller keeps theirs.
	const int import_fd = fcntl(p_fd, F_DUPFD_CLOEXEC, 0);
	if (import_fd < 0) {
		vkDestroyImage(device, image, nullptr);
		ERR_FAIL_V_MSG(ERR_CANT_CREATE, vformat("Couldn't duplicate the dmabuf fd: %s.", strerror(errno)));
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
		ERR_FAIL_V_MSG(ERR_CANT_CREATE, vformat("Importing the dmabuf failed (VkResult %d).", err));
	}

	err = vkBindImageMemory(device, image, memory, 0);
	if (err != VK_SUCCESS) {
		vkFreeMemory(device, memory, nullptr);
		vkDestroyImage(device, image, nullptr);
		ERR_FAIL_V_MSG(ERR_CANT_CREATE, vformat("vkBindImageMemory failed (VkResult %d).", err));
	}

	if (OS::get_singleton()->is_stdout_verbose()) {
		VkImageDrmFormatModifierPropertiesEXT image_modifier = {};
		image_modifier.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT;
		get_image_drm_format_modifier_properties(device, image, &image_modifier);
		print_line(vformat("DmabufImage: imported %dx%d NV12, modifier %s, %d bytes, memory type %d.", p_width, p_height, _modifier_to_string(image_modifier.drmFormatModifier), (int64_t)memory_requirements.size, memory_type_index));
		for (uint32_t i = 0; i < memory_plane_count; i++) {
			VkImageSubresource subresource = {};
			subresource.aspectMask = VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT << i;
			VkSubresourceLayout layout = {};
			vkGetImageSubresourceLayout(device, image, &subresource, &layout);
			print_line(vformat("DmabufImage:   memory plane %d: offset %d, pitch %d, size %d.", i, (int64_t)layout.offset, (int64_t)layout.rowPitch, (int64_t)layout.size));
		}
	}

	return _create_planes(device, image, memory, p_width, p_height, modifier);
}

Error DmabufImage::_create_planes(VkDevice p_device, VkImage p_image, VkDeviceMemory p_memory, int p_width, int p_height, uint64_t p_drm_modifier) {
	// From here on the image is ours to free, with free_image().
	vk_device = p_device;
	vk_image = p_image;
	vk_memory = p_memory;
	size = Size2i(p_width, p_height);
	drm_modifier = p_drm_modifier;
	acquired = false;

	// One RenderingDevice texture per plane: Y as R8 at full size, interleaved CbCr as R8G8 at half size.
	RenderingDevice *rd = RenderingDevice::get_singleton();
	const RD::DataFormat plane_formats[MAX_PLANES] = { RD::DATA_FORMAT_R8_UNORM, RD::DATA_FORMAT_R8G8_UNORM };
	const Size2i plane_sizes[MAX_PLANES] = { Size2i(p_width, p_height), Size2i(p_width / 2, p_height / 2) };
	for (uint32_t i = 0; i < MAX_PLANES; i++) {
		planes[i] = rd->texture_create_from_extension(RD::TEXTURE_TYPE_2D, plane_formats[i], RD::TEXTURE_SAMPLES_1, RD::TEXTURE_USAGE_SAMPLING_BIT, (uint64_t)p_image, plane_sizes[i].width, plane_sizes[i].height, 1, 1, 1, i);
		if (planes[i].is_null()) {
			plane_count = i;
			free_image();
			ERR_FAIL_V_MSG(ERR_CANT_CREATE, vformat("Couldn't create the RenderingDevice texture for plane %d.", i));
		}
	}
	plane_count = MAX_PLANES;
	return OK;
}

Error DmabufImage::_import_host_test_pattern(int p_width, int p_height) {
	// Test path for machines without dmabufs (e.g. lavapipe): a linear NV12 image in host-visible memory, written through a
	// mapping. It goes through the same plane views, ownership transfers and shader as an imported dmabuf, but not the import.
	RenderingDeviceDriverVulkan *driver = _get_vulkan_driver();
	ERR_FAIL_NULL_V(driver, ERR_UNAVAILABLE);
	ERR_FAIL_COND_V_MSG(!driver->is_sampler_ycbcr_conversion_enabled(), ERR_UNAVAILABLE, "samplerYcbcrConversion is not enabled, multi-planar images can't be created.");

	_process_pending_frees();

	RenderingDevice *rd = RenderingDevice::get_singleton();
	VkDevice device = (VkDevice)rd->get_driver_resource(RD::DRIVER_RESOURCE_LOGICAL_DEVICE);
	VkPhysicalDevice physical_device = (VkPhysicalDevice)rd->get_driver_resource(RD::DRIVER_RESOURCE_PHYSICAL_DEVICE);

	VkImageFormatListCreateInfo format_list = {};
	format_list.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO;
	format_list.viewFormatCount = std_size(NV12_VIEW_FORMATS);
	format_list.pViewFormats = NV12_VIEW_FORMATS;

	VkImageCreateInfo image_create_info = {};
	image_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	image_create_info.pNext = &format_list;
	image_create_info.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
	image_create_info.imageType = VK_IMAGE_TYPE_2D;
	image_create_info.format = NV12_FORMAT;
	image_create_info.extent = { (uint32_t)p_width, (uint32_t)p_height, 1 };
	image_create_info.mipLevels = 1;
	image_create_info.arrayLayers = 1;
	image_create_info.samples = VK_SAMPLE_COUNT_1_BIT;
	image_create_info.tiling = VK_IMAGE_TILING_LINEAR;
	image_create_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
	image_create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	image_create_info.initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;

	VkImage image = VK_NULL_HANDLE;
	VkResult err = vkCreateImage(device, &image_create_info, nullptr, &image);
	ERR_FAIL_COND_V_MSG(err != VK_SUCCESS, ERR_CANT_CREATE, vformat("vkCreateImage failed for a linear NV12 image (VkResult %d).", err));

	VkMemoryRequirements memory_requirements = {};
	vkGetImageMemoryRequirements(device, image, &memory_requirements);
	VkPhysicalDeviceMemoryProperties memory_properties = {};
	vkGetPhysicalDeviceMemoryProperties(physical_device, &memory_properties);
	const VkMemoryPropertyFlags wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	uint32_t memory_type_index = UINT32_MAX;
	for (uint32_t i = 0; i < memory_properties.memoryTypeCount; i++) {
		if ((memory_requirements.memoryTypeBits & (1u << i)) && (memory_properties.memoryTypes[i].propertyFlags & wanted) == wanted) {
			memory_type_index = i;
			break;
		}
	}
	if (memory_type_index == UINT32_MAX) {
		vkDestroyImage(device, image, nullptr);
		ERR_FAIL_V_MSG(ERR_UNAVAILABLE, "No host-visible memory type for a linear NV12 image.");
	}

	VkMemoryAllocateInfo allocate_info = {};
	allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocate_info.allocationSize = memory_requirements.size;
	allocate_info.memoryTypeIndex = memory_type_index;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	err = vkAllocateMemory(device, &allocate_info, nullptr, &memory);
	if (err == VK_SUCCESS) {
		err = vkBindImageMemory(device, image, memory, 0);
	}
	void *data = nullptr;
	if (err == VK_SUCCESS) {
		err = vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &data);
	}
	if (err != VK_SUCCESS) {
		if (memory != VK_NULL_HANDLE) {
			vkFreeMemory(device, memory, nullptr);
		}
		vkDestroyImage(device, image, nullptr);
		ERR_FAIL_V_MSG(ERR_CANT_CREATE, vformat("Allocating or mapping host memory for the test pattern failed (VkResult %d).", err));
	}

	VkSubresourceLayout plane_layouts[MAX_PLANES] = {};
	for (uint32_t i = 0; i < MAX_PLANES; i++) {
		VkImageSubresource subresource = {};
		subresource.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT << i;
		vkGetImageSubresourceLayout(device, image, &subresource, &plane_layouts[i]);
	}
	_fill_test_pattern((uint8_t *)data + plane_layouts[0].offset, plane_layouts[0].rowPitch, (uint8_t *)data + plane_layouts[1].offset, plane_layouts[1].rowPitch, p_width, p_height);
	vkUnmapMemory(device, memory);
	print_line(vformat("DmabufImage: test pattern %dx%d in host memory (no dmabuf), planes at %d and %d, pitches %d and %d.", p_width, p_height, (int64_t)plane_layouts[0].offset, (int64_t)plane_layouts[1].offset, (int64_t)plane_layouts[0].rowPitch, (int64_t)plane_layouts[1].rowPitch));

	return _create_planes(device, image, memory, p_width, p_height, DRM_FORMAT_MOD_LINEAR);
}

void DmabufImage::_fill_test_pattern(uint8_t *p_luma, uint64_t p_luma_pitch, uint8_t *p_chroma, uint64_t p_chroma_pitch, uint32_t p_width, uint32_t p_height) {
	// Luma: dark to bright from left to right (limited range, 16-235), with a white grid every 1/8 of the size.
	const uint32_t grid_x = MAX(p_width / 8, 1u);
	const uint32_t grid_y = MAX(p_height / 8, 1u);
	for (uint32_t y = 0; y < p_height; y++) {
		uint8_t *row = p_luma + y * p_luma_pitch;
		for (uint32_t x = 0; x < p_width; x++) {
			const bool grid = (x % grid_x) < 4 || (y % grid_y) < 4;
			row[x] = grid ? 235 : uint8_t(16 + (219 * x) / (p_width - 1));
		}
	}

	// Chroma (interleaved Cb Cr, half size): Cb rises from top to bottom, Cr from left to right (16-240).
	const uint32_t chroma_width = p_width / 2;
	const uint32_t chroma_height = p_height / 2;
	for (uint32_t y = 0; y < chroma_height; y++) {
		uint8_t *row = p_chroma + y * p_chroma_pitch;
		for (uint32_t x = 0; x < chroma_width; x++) {
			row[x * 2 + 0] = uint8_t(16 + (224 * y) / (chroma_height - 1));
			row[x * 2 + 1] = uint8_t(16 + (224 * x) / (chroma_width - 1));
		}
	}
}

static void _dmabuf_sync(int p_fd, uint64_t p_flags) {
	struct dma_buf_sync sync = {};
	sync.flags = p_flags | DMA_BUF_SYNC_WRITE;
	if (ioctl(p_fd, DMA_BUF_IOCTL_SYNC, &sync) < 0) {
		print_verbose(vformat("DmabufImage: DMA_BUF_IOCTL_SYNC failed: %s.", strerror(errno)));
	}
}

Error DmabufImage::_create_test_pattern_dumb(const String &p_device, uint32_t p_width, uint32_t p_height, int &r_fd, uint64_t &r_pitch, uint64_t &r_uv_offset) {
	const int drm_fd = open(p_device.utf8().get_data(), O_RDWR | O_CLOEXEC);
	if (drm_fd < 0) {
		print_line(vformat("DmabufImage: can't open %s: %s.", p_device, strerror(errno)));
		return ERR_CANT_OPEN;
	}

	// One byte per pixel: luma rows, then chroma rows starting on a page boundary. Pitch aligned to 256 bytes, which should suit
	// any GPU's linear layout.
	const uint64_t pitch_estimate = _align(p_width, 256);
	struct drm_mode_create_dumb create = {};
	create.width = pitch_estimate;
	create.height = p_height + p_height / 2 + _align(4096, pitch_estimate) / pitch_estimate;
	create.bpp = 8;
	if (ioctl(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0) {
		print_line(vformat("DmabufImage: DRM_IOCTL_MODE_CREATE_DUMB on %s failed: %s.", p_device, strerror(errno)));
		close(drm_fd);
		return ERR_CANT_CREATE;
	}

	struct drm_mode_destroy_dumb destroy = {};
	destroy.handle = create.handle;

	const uint64_t pitch = create.pitch;
	const uint64_t uv_offset = _align(pitch * p_height, 4096);
	if (pitch < p_width || uv_offset + pitch * (p_height / 2) > create.size) {
		print_line(vformat("DmabufImage: dumb buffer is too small (pitch %d, size %d).", (int64_t)pitch, (int64_t)create.size));
		ioctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
		close(drm_fd);
		return ERR_CANT_CREATE;
	}

	struct drm_prime_handle prime = {};
	prime.handle = create.handle;
	prime.flags = DRM_CLOEXEC | DRM_RDWR;
	if (ioctl(drm_fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime) < 0) {
		print_line(vformat("DmabufImage: DRM_IOCTL_PRIME_HANDLE_TO_FD failed: %s.", strerror(errno)));
		ioctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
		close(drm_fd);
		return ERR_CANT_CREATE;
	}

	struct drm_mode_map_dumb map = {};
	map.handle = create.handle;
	void *data = MAP_FAILED;
	if (ioctl(drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &map) == 0) {
		data = mmap(nullptr, create.size, PROT_READ | PROT_WRITE, MAP_SHARED, drm_fd, map.offset);
	}
	if (data == MAP_FAILED) {
		print_line(vformat("DmabufImage: mapping the dumb buffer failed: %s.", strerror(errno)));
		close(prime.fd);
		ioctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
		close(drm_fd);
		return ERR_CANT_CREATE;
	}

	_dmabuf_sync(prime.fd, DMA_BUF_SYNC_START);
	_fill_test_pattern((uint8_t *)data, pitch, (uint8_t *)data + uv_offset, pitch, p_width, p_height);
	_dmabuf_sync(prime.fd, DMA_BUF_SYNC_END);
	munmap(data, create.size);

	// The dmabuf keeps the buffer alive.
	ioctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
	close(drm_fd);

	r_fd = prime.fd;
	r_pitch = pitch;
	r_uv_offset = uv_offset;
	return OK;
}

Error DmabufImage::_create_test_pattern_udmabuf(uint32_t p_width, uint32_t p_height, int &r_fd, uint64_t &r_pitch, uint64_t &r_uv_offset) {
	const int udmabuf_fd = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
	if (udmabuf_fd < 0) {
		print_line(vformat("DmabufImage: can't open /dev/udmabuf: %s.", strerror(errno)));
		return ERR_CANT_OPEN;
	}

	const uint64_t page_size = (uint64_t)sysconf(_SC_PAGESIZE);
	const uint64_t pitch = _align(p_width, 256);
	const uint64_t uv_offset = _align(pitch * p_height, 4096);
	const uint64_t size = _align(uv_offset + pitch * (p_height / 2), page_size);

	const int memfd = memfd_create("godot-dmabuf-test-pattern", MFD_CLOEXEC | MFD_ALLOW_SEALING);
	if (memfd < 0 || ftruncate(memfd, size) < 0 || fcntl(memfd, F_ADD_SEALS, F_SEAL_SHRINK) < 0) {
		print_line(vformat("DmabufImage: creating the memfd failed: %s.", strerror(errno)));
		if (memfd >= 0) {
			close(memfd);
		}
		close(udmabuf_fd);
		return ERR_CANT_CREATE;
	}

	struct udmabuf_create create = {};
	create.memfd = memfd;
	create.flags = UDMABUF_FLAGS_CLOEXEC;
	create.offset = 0;
	create.size = size;
	const int dmabuf_fd = ioctl(udmabuf_fd, UDMABUF_CREATE, &create);
	close(udmabuf_fd);
	if (dmabuf_fd < 0) {
		print_line(vformat("DmabufImage: UDMABUF_CREATE failed: %s.", strerror(errno)));
		close(memfd);
		return ERR_CANT_CREATE;
	}

	void *data = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, 0);
	close(memfd); // The udmabuf holds the pages.
	if (data == MAP_FAILED) {
		print_line(vformat("DmabufImage: mapping the memfd failed: %s.", strerror(errno)));
		close(dmabuf_fd);
		return ERR_CANT_CREATE;
	}

	_dmabuf_sync(dmabuf_fd, DMA_BUF_SYNC_START);
	_fill_test_pattern((uint8_t *)data, pitch, (uint8_t *)data + uv_offset, pitch, p_width, p_height);
	_dmabuf_sync(dmabuf_fd, DMA_BUF_SYNC_END);
	munmap(data, size);

	r_fd = dmabuf_fd;
	r_pitch = pitch;
	r_uv_offset = uv_offset;
	return OK;
}

Error DmabufImage::import_test_pattern(int p_width, int p_height, const String &p_device) {
	ERR_FAIL_COND_V_MSG(p_width < 16 || p_height < 16 || (p_width % 2) != 0 || (p_height % 2) != 0, ERR_INVALID_PARAMETER, "Width and height must be even and at least 16.");
	ERR_FAIL_COND_V_MSG(is_imported(), ERR_ALREADY_IN_USE, "An image is already imported. Call free_image() first.");
	if (p_device == "host") {
		return _import_host_test_pattern(p_width, p_height);
	}

	int fd = -1;
	uint64_t pitch = 0;
	uint64_t uv_offset = 0;
	const char *source = "DRM dumb buffer";
	Error err = _create_test_pattern_dumb(p_device, p_width, p_height, fd, pitch, uv_offset);
	if (err != OK) {
		source = "udmabuf";
		err = _create_test_pattern_udmabuf(p_width, p_height, fd, pitch, uv_offset);
	}
	ERR_FAIL_COND_V_MSG(err != OK, err, "Couldn't create a dmabuf for the test pattern (tried a DRM dumb buffer and /dev/udmabuf).");
	print_line(vformat("DmabufImage: test pattern %dx%d in a %s, pitch %d, chroma at offset %d.", p_width, p_height, source, (int64_t)pitch, (int64_t)uv_offset));

	PackedInt64Array offsets;
	offsets.push_back(0);
	offsets.push_back((int64_t)uv_offset);
	PackedInt64Array pitches;
	pitches.push_back((int64_t)pitch);
	pitches.push_back((int64_t)pitch);
	err = import_nv12(fd, p_width, p_height, (int64_t)DRM_FORMAT_MOD_LINEAR, offsets, pitches);

	// The image holds its own reference to the dmabuf.
	close(fd);
	return err;
}

RID DmabufImage::get_plane_rid(int p_plane) const {
	ERR_FAIL_INDEX_V(p_plane, (int)plane_count, RID());
	return planes[p_plane];
}

Error DmabufImage::acquire() {
	ERR_FAIL_COND_V_MSG(!is_imported(), ERR_UNCONFIGURED, "No image is imported.");
	if (acquired) {
		// Still ours: the producer can't have written to it since it was acquired.
		return OK;
	}

	_process_pending_frees();

	Error err = RenderingDevice::get_singleton()->texture_external_acquire(planes[0], VectorView<RID>(&planes[1], plane_count - 1));
	ERR_FAIL_COND_V(err != OK, err);
	acquired = true;
	return OK;
}

Error DmabufImage::release() {
	ERR_FAIL_COND_V_MSG(!is_imported(), ERR_UNCONFIGURED, "No image is imported.");
	if (!acquired) {
		return OK;
	}

	_process_pending_frees();

	Error err = RenderingDevice::get_singleton()->texture_external_release(planes[0], VectorView<RID>(&planes[1], plane_count - 1));
	ERR_FAIL_COND_V(err != OK, err);
	acquired = false;
	return OK;
}

void DmabufImage::free_image() {
	if (vk_image == VK_NULL_HANDLE) {
		return;
	}

	// The RenderingDevice destroys the plane views once the frames that may use them have completed. The image and its memory
	// must outlive them, so they're destroyed a frame after that.
	RenderingDevice *rd = RenderingDevice::get_singleton();
	PendingFree pending;
	pending.device = vk_device;
	pending.image = vk_image;
	pending.memory = vk_memory;
	if (rd != nullptr) {
		for (uint32_t i = 0; i < plane_count; i++) {
			if (planes[i].is_valid()) {
				rd->free_rid(planes[i]);
			}
		}
		pending.frame = rd->get_frames_drawn();
	}

	for (uint32_t i = 0; i < MAX_PLANES; i++) {
		planes[i] = RID();
	}
	vk_device = VK_NULL_HANDLE;
	vk_image = VK_NULL_HANDLE;
	vk_memory = VK_NULL_HANDLE;
	plane_count = 0;
	size = Size2i();
	drm_modifier = 0;
	acquired = false;

	if (rd == nullptr) {
		// The device is gone, and the image with it.
		return;
	}

	{
		MutexLock lock(pending_frees_mutex);
		pending_frees.push_back(pending);
	}
	_process_pending_frees();
}

// Called from every import, acquire, release and free, which a decoder does every frame. No per-frame hook into the
// RenderingServer is needed, so nothing of ours runs during engine shutdown.
void DmabufImage::_process_pending_frees() {
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

void DmabufImage::flush_pending_frees() {
	// At shutdown the RenderingDevice still holds views of these images until it's finalized, after the modules, and
	// destroys the device right after. Destroying the images first would leave it with views of destroyed images, so
	// leave them to the process exit.
	MutexLock lock(pending_frees_mutex);
	if (!pending_frees.is_empty()) {
		print_verbose(vformat("DmabufImage: leaving %d image(s) to be released at exit.", pending_frees.size()));
	}
	pending_frees.clear();
}

DmabufImage::~DmabufImage() {
	free_image();
}

void DmabufImage::_bind_methods() {
	ClassDB::bind_method(D_METHOD("import_nv12", "fd", "width", "height", "drm_modifier", "plane_offsets", "plane_pitches"), &DmabufImage::import_nv12);
	ClassDB::bind_method(D_METHOD("import_test_pattern", "width", "height", "device"), &DmabufImage::import_test_pattern, DEFVAL("/dev/dri/card0"));
	ClassDB::bind_method(D_METHOD("free_image"), &DmabufImage::free_image);

	ClassDB::bind_method(D_METHOD("acquire"), &DmabufImage::acquire);
	ClassDB::bind_method(D_METHOD("release"), &DmabufImage::release);
	ClassDB::bind_method(D_METHOD("is_acquired"), &DmabufImage::is_acquired);

	ClassDB::bind_method(D_METHOD("is_imported"), &DmabufImage::is_imported);
	ClassDB::bind_method(D_METHOD("get_plane_rid", "plane"), &DmabufImage::get_plane_rid);
	ClassDB::bind_method(D_METHOD("get_size"), &DmabufImage::get_size);
	ClassDB::bind_method(D_METHOD("get_drm_modifier"), &DmabufImage::get_drm_modifier);

	ClassDB::bind_static_method("DmabufImage", D_METHOD("get_supported_nv12_modifiers"), &DmabufImage::get_supported_nv12_modifiers);
}
