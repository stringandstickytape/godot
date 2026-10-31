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
// Split import: image i has only the view format of plane i.
static const VkFormat SPLIT_VIEW_FORMATS[DmabufImage::MAX_PLANES] = { VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM };

// The 2-plane 4:2:0 formats: the image format, its plane view formats (one image, or one per plane when split) and the
// RenderingDevice formats of the plane textures.
struct DmabufYUVFormat {
	const char *name;
	VkFormat format;
	VkFormat views[3];
	VkFormat split_views[DmabufImage::MAX_PLANES];
	RD::DataFormat plane_formats[DmabufImage::MAX_PLANES];
	uint32_t bytes_per_sample;
};
static const DmabufYUVFormat YUV_NV12 = { "NV12", NV12_FORMAT, { NV12_FORMAT, VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM },
	{ VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM }, { RD::DATA_FORMAT_R8_UNORM, RD::DATA_FORMAT_R8G8_UNORM }, 1 };
// P010: 16-bit samples, the 10 bits at the top. The plane views are plain 16-bit unorm (the same size class as R10X6).
static const DmabufYUVFormat YUV_P010 = { "P010", VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16,
	{ VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16, VK_FORMAT_R16_UNORM, VK_FORMAT_R16G16_UNORM },
	{ VK_FORMAT_R16_UNORM, VK_FORMAT_R16G16_UNORM }, { RD::DATA_FORMAT_R16_UNORM, RD::DATA_FORMAT_R16G16_UNORM }, 2 };

#ifndef DRM_FORMAT_MOD_QCOM_COMPRESSED
#define DRM_FORMAT_MOD_QCOM_COMPRESSED fourcc_mod_code(QCOM, 1)
#endif

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

// Can a sampled image of this format and modifier be imported from a dmabuf? With view formats, it's mutable to them.
static VkResult _query_import(VkPhysicalDevice p_physical_device, VkFormat p_format, uint64_t p_modifier, const VkFormat *p_view_formats, uint32_t p_view_format_count, VkExtent3D &r_max_extent, bool &r_importable) {
	VkImageFormatListCreateInfo format_list = {};
	format_list.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO;
	format_list.viewFormatCount = p_view_format_count;
	format_list.pViewFormats = p_view_formats;

	VkPhysicalDeviceImageDrmFormatModifierInfoEXT modifier_info = {};
	modifier_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT;
	modifier_info.pNext = p_view_format_count ? &format_list : nullptr;
	modifier_info.drmFormatModifier = p_modifier;
	modifier_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	VkPhysicalDeviceExternalImageFormatInfo external_info = {};
	external_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO;
	external_info.pNext = &modifier_info;
	external_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

	VkPhysicalDeviceImageFormatInfo2 format_info = {};
	format_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
	format_info.pNext = &external_info;
	format_info.format = p_format;
	format_info.type = VK_IMAGE_TYPE_2D;
	format_info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
	format_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
	format_info.flags = p_view_format_count ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0;

	VkExternalImageFormatProperties external_properties = {};
	external_properties.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES;

	VkImageFormatProperties2 image_format_properties = {};
	image_format_properties.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
	image_format_properties.pNext = &external_properties;

	const VkResult err = vkGetPhysicalDeviceImageFormatProperties2(p_physical_device, &format_info, &image_format_properties);
	r_max_extent = image_format_properties.imageFormatProperties.maxExtent;
	r_importable = external_properties.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT;
	return err;
}

// Creates an image with an explicit layout and binds it to a new import of the dmabuf (dedicated, of a duplicate fd: the
// caller keeps theirs). With view formats, the image is mutable to them.
static Error _create_imported_image(VkDevice p_device, PFN_vkGetMemoryFdPropertiesKHR p_get_memory_fd_properties, int p_fd, VkFormat p_format, int p_width, int p_height, uint64_t p_modifier,
		const VkSubresourceLayout *p_plane_layouts, uint32_t p_memory_plane_count, const VkFormat *p_view_formats, uint32_t p_view_format_count,
		VkImage &r_image, VkDeviceMemory &r_memory, uint64_t &r_size, uint32_t &r_memory_type) {
	VkImageFormatListCreateInfo format_list = {};
	format_list.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO;
	format_list.viewFormatCount = p_view_format_count;
	format_list.pViewFormats = p_view_formats;

	VkImageDrmFormatModifierExplicitCreateInfoEXT modifier_create_info = {};
	modifier_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT;
	modifier_create_info.pNext = p_view_format_count ? &format_list : nullptr;
	modifier_create_info.drmFormatModifier = p_modifier;
	modifier_create_info.drmFormatModifierPlaneCount = p_memory_plane_count;
	modifier_create_info.pPlaneLayouts = p_plane_layouts;

	VkExternalMemoryImageCreateInfo external_create_info = {};
	external_create_info.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
	external_create_info.pNext = &modifier_create_info;
	external_create_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

	VkImageCreateInfo image_create_info = {};
	image_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	image_create_info.pNext = &external_create_info;
	image_create_info.flags = p_view_format_count ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0;
	image_create_info.imageType = VK_IMAGE_TYPE_2D;
	image_create_info.format = p_format;
	image_create_info.extent = { (uint32_t)p_width, (uint32_t)p_height, 1 };
	image_create_info.mipLevels = 1;
	image_create_info.arrayLayers = 1;
	image_create_info.samples = VK_SAMPLE_COUNT_1_BIT;
	image_create_info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
	image_create_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
	image_create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	image_create_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

	VkImage image = VK_NULL_HANDLE;
	VkResult err = vkCreateImage(p_device, &image_create_info, nullptr, &image);
	ERR_FAIL_COND_V_MSG(err != VK_SUCCESS, ERR_CANT_CREATE, vformat("vkCreateImage failed for a dmabuf image (VkResult %d). VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT (%d) means the offsets or pitches don't suit the driver.", err, VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT));

	VkMemoryRequirements memory_requirements = {};
	vkGetImageMemoryRequirements(p_device, image, &memory_requirements);

	VkMemoryFdPropertiesKHR fd_properties = {};
	fd_properties.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR;
	err = p_get_memory_fd_properties(p_device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, p_fd, &fd_properties);
	if (err != VK_SUCCESS) {
		vkDestroyImage(p_device, image, nullptr);
		ERR_FAIL_V_MSG(ERR_CANT_CREATE, vformat("vkGetMemoryFdPropertiesKHR failed (VkResult %d). Is the fd a dmabuf?", err));
	}

	const uint32_t memory_type_bits = memory_requirements.memoryTypeBits & fd_properties.memoryTypeBits;
	if (memory_type_bits == 0) {
		vkDestroyImage(p_device, image, nullptr);
		ERR_FAIL_V_MSG(ERR_CANT_CREATE, vformat("No memory type suits both the image (0x%x) and the dmabuf (0x%x).", memory_requirements.memoryTypeBits, fd_properties.memoryTypeBits));
	}
	uint32_t memory_type_index = 0;
	while (!(memory_type_bits & (1u << memory_type_index))) {
		memory_type_index++;
	}

	// The driver refuses to import less memory than the dmabuf holds; report it here rather than as a generic import failure.
	const off_t dmabuf_size = lseek(p_fd, 0, SEEK_END);
	if (dmabuf_size >= 0 && (uint64_t)dmabuf_size < memory_requirements.size) {
		vkDestroyImage(p_device, image, nullptr);
		ERR_FAIL_V_MSG(ERR_INVALID_PARAMETER, vformat("The dmabuf holds %d bytes, but the image needs %d.", (int64_t)dmabuf_size, (int64_t)memory_requirements.size));
	}

	// A successful import takes ownership of the fd, so give it a duplicate: the caller keeps theirs.
	const int import_fd = fcntl(p_fd, F_DUPFD_CLOEXEC, 0);
	if (import_fd < 0) {
		vkDestroyImage(p_device, image, nullptr);
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
	err = vkAllocateMemory(p_device, &allocate_info, nullptr, &memory);
	if (err != VK_SUCCESS) {
		close(import_fd);
		vkDestroyImage(p_device, image, nullptr);
		ERR_FAIL_V_MSG(ERR_CANT_CREATE, vformat("Importing the dmabuf failed (VkResult %d).", err));
	}

	err = vkBindImageMemory(p_device, image, memory, 0);
	if (err != VK_SUCCESS) {
		vkFreeMemory(p_device, memory, nullptr);
		vkDestroyImage(p_device, image, nullptr);
		ERR_FAIL_V_MSG(ERR_CANT_CREATE, vformat("vkBindImageMemory failed (VkResult %d).", err));
	}

	r_image = image;
	r_memory = memory;
	r_size = memory_requirements.size;
	r_memory_type = memory_type_index;
	return OK;
}

Error DmabufImage::import_nv12(int p_fd, int p_width, int p_height, int64_t p_drm_modifier, const PackedInt64Array &p_plane_offsets, const PackedInt64Array &p_plane_pitches) {
	return _import_yuv420(YUV_NV12, p_fd, p_width, p_height, p_drm_modifier, p_plane_offsets, p_plane_pitches);
}

Error DmabufImage::import_p010(int p_fd, int p_width, int p_height, int64_t p_drm_modifier, const PackedInt64Array &p_plane_offsets, const PackedInt64Array &p_plane_pitches) {
	return _import_yuv420(YUV_P010, p_fd, p_width, p_height, p_drm_modifier, p_plane_offsets, p_plane_pitches);
}

Error DmabufImage::_import_yuv420(const DmabufYUVFormat &p_yuv, int p_fd, int p_width, int p_height, int64_t p_drm_modifier, const PackedInt64Array &p_plane_offsets, const PackedInt64Array &p_plane_pitches) {
	ERR_FAIL_COND_V_MSG(is_imported(), ERR_ALREADY_IN_USE, "An image is already imported. Call free_image() first.");
	ERR_FAIL_COND_V(p_fd < 0, ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V_MSG(p_width <= 0 || p_height <= 0 || (p_width % 2) != 0 || (p_height % 2) != 0, ERR_INVALID_PARAMETER, vformat("%s width and height must be positive and even.", p_yuv.name));
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
		_get_modifier_properties(physical_device, p_yuv.format, properties);
		const VkDrmFormatModifierPropertiesEXT *modifier_properties = nullptr;
		String supported;
		for (const VkDrmFormatModifierPropertiesEXT &p : properties) {
			supported += (supported.is_empty() ? "" : ", ") + _modifier_to_string(p.drmFormatModifier);
			if (p.drmFormatModifier == modifier) {
				modifier_properties = &p;
			}
		}
		ERR_FAIL_NULL_V_MSG(modifier_properties, ERR_UNAVAILABLE, vformat("DRM format modifier %s is not supported for %s. Supported: %s.", _modifier_to_string(modifier), p_yuv.name, supported));
		ERR_FAIL_COND_V_MSG(!(modifier_properties->drmFormatModifierTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT), ERR_UNAVAILABLE, vformat("%s with modifier %s can't be sampled.", p_yuv.name, _modifier_to_string(modifier)));
		ERR_FAIL_COND_V_MSG(modifier_properties->drmFormatModifierPlaneCount != memory_plane_count, ERR_INVALID_PARAMETER, vformat("Modifier %s has %d memory plane(s), but %d offset(s) were given.", _modifier_to_string(modifier), modifier_properties->drmFormatModifierPlaneCount, memory_plane_count));
	}

	// Plane views (R8 and R8G8 over the planes) need a mutable format, which must be listed with DRM format modifier tiling.
	// Check the full combination up front: it gives a clearer error than vkCreateImage().
	VkExtent3D max_extent = {};
	bool importable = false;
	uint32_t count = 1;
	VkResult err = _query_import(physical_device, p_yuv.format, modifier, p_yuv.views, std_size(p_yuv.views), max_extent, importable);
	if (err != VK_SUCCESS && modifier != DRM_FORMAT_MOD_LINEAR) {
		// Turnip refuses a UBWC NV12 image with views of formats that compress differently (R8 and R8G8): it would have to
		// fall back to linear, which an explicit modifier doesn't allow. One image per plane view keeps the layout.
		VkExtent3D max_extent_uv = {};
		bool importable_uv = false;
		const VkResult err_y = _query_import(physical_device, p_yuv.format, modifier, &p_yuv.split_views[0], 1, max_extent, importable);
		const VkResult err_uv = _query_import(physical_device, p_yuv.format, modifier, &p_yuv.split_views[1], 1, max_extent_uv, importable_uv);
		if (err_y == VK_SUCCESS && err_uv == VK_SUCCESS) {
			print_verbose(vformat("DmabufImage: %s + modifier %s can't have both plane views on one image (VkResult %d); importing it as two images.", p_yuv.name, _modifier_to_string(modifier), err));
			err = VK_SUCCESS;
			count = 2;
			importable = importable && importable_uv;
			max_extent.width = MIN(max_extent.width, max_extent_uv.width);
			max_extent.height = MIN(max_extent.height, max_extent_uv.height);
		} else {
			print_verbose(vformat("DmabufImage: one image per plane view is refused too (VkResult %d and %d).", err_y, err_uv));
		}
	}
	ERR_FAIL_COND_V_MSG(err != VK_SUCCESS, ERR_UNAVAILABLE, vformat("%s + modifier %s + mutable format (plane views) + dmabuf import is not supported (VkResult %d).", p_yuv.name, _modifier_to_string(modifier), err));
	ERR_FAIL_COND_V_MSG(!importable, ERR_UNAVAILABLE, "dmabufs can't be imported for this image.");
	ERR_FAIL_COND_V_MSG((uint32_t)p_width > max_extent.width || (uint32_t)p_height > max_extent.height, ERR_INVALID_PARAMETER, vformat("Size %dx%d is larger than the maximum %dx%d.", p_width, p_height, max_extent.width, max_extent.height));

	// Not disjoint: every plane lives in the same memory object, at the given offsets.
	VkSubresourceLayout plane_layouts[MAX_MEMORY_PLANES] = {};
	for (uint32_t i = 0; i < memory_plane_count; i++) {
		ERR_FAIL_COND_V(p_plane_offsets[i] < 0 || p_plane_pitches[i] <= 0, ERR_INVALID_PARAMETER);
		plane_layouts[i].offset = p_plane_offsets[i];
		plane_layouts[i].rowPitch = p_plane_pitches[i];
	}

	// A split import imports the dmabuf once per image; the kernel and the driver share the buffer object.
	VkImage images[MAX_PLANES] = {};
	VkDeviceMemory memories[MAX_PLANES] = {};
	for (uint32_t i = 0; i < count; i++) {
		uint64_t memory_size = 0;
		uint32_t memory_type = 0;
		const VkFormat *view_formats = count == 1 ? p_yuv.views : &p_yuv.split_views[i];
		const uint32_t view_format_count = count == 1 ? std_size(p_yuv.views) : 1;
		const Error import_err = _create_imported_image(device, get_memory_fd_properties, p_fd, p_yuv.format, p_width, p_height, modifier, plane_layouts, memory_plane_count, view_formats, view_format_count, images[i], memories[i], memory_size, memory_type);
		if (import_err != OK) {
			for (uint32_t j = 0; j < i; j++) {
				vkFreeMemory(device, memories[j], nullptr);
				vkDestroyImage(device, images[j], nullptr);
			}
			return import_err;
		}

		if (OS::get_singleton()->is_stdout_verbose()) {
			VkImageDrmFormatModifierPropertiesEXT image_modifier = {};
			image_modifier.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT;
			get_image_drm_format_modifier_properties(device, images[i], &image_modifier);
			print_line(vformat("DmabufImage: imported %dx%d %s (image %d of %d), modifier %s, %d bytes, memory type %d.", p_width, p_height, p_yuv.name, i + 1, count, _modifier_to_string(image_modifier.drmFormatModifier), (int64_t)memory_size, memory_type));
			for (uint32_t p = 0; p < memory_plane_count; p++) {
				VkImageSubresource subresource = {};
				subresource.aspectMask = VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT << p;
				VkSubresourceLayout layout = {};
				vkGetImageSubresourceLayout(device, images[i], &subresource, &layout);
				print_line(vformat("DmabufImage:   memory plane %d: offset %d, pitch %d, size %d.", p, (int64_t)layout.offset, (int64_t)layout.rowPitch, (int64_t)layout.size));
			}
		}
	}

	return _create_planes(device, images, memories, count, p_width, p_height, modifier, p_yuv.plane_formats);
}

Error DmabufImage::_create_planes(VkDevice p_device, const VkImage *p_images, const VkDeviceMemory *p_memories, uint32_t p_image_count, int p_width, int p_height, uint64_t p_drm_modifier, const RD::DataFormat *p_plane_formats) {
	// From here on the images are ours to free, with free_image().
	vk_device = p_device;
	for (uint32_t i = 0; i < p_image_count; i++) {
		vk_images[i] = p_images[i];
		vk_memories[i] = p_memories[i];
	}
	image_count = p_image_count;
	size = Size2i(p_width, p_height);
	drm_modifier = p_drm_modifier;
	acquired = false;

	// One RenderingDevice texture per plane: Y at full size, interleaved CbCr at half size (R8 and R8G8 for NV12, R16 and
	// R16G16 for P010). Split: plane i of image i.
	RenderingDevice *rd = RenderingDevice::get_singleton();
	const RD::DataFormat *plane_formats = p_plane_formats;
	const Size2i plane_sizes[MAX_PLANES] = { Size2i(p_width, p_height), Size2i(p_width / 2, p_height / 2) };
	for (uint32_t i = 0; i < MAX_PLANES; i++) {
		const VkImage image = p_image_count == 1 ? p_images[0] : p_images[i];
		planes[i] = rd->texture_create_from_extension(RD::TEXTURE_TYPE_2D, plane_formats[i], RD::TEXTURE_SAMPLES_1, RD::TEXTURE_USAGE_SAMPLING_BIT, (uint64_t)image, plane_sizes[i].width, plane_sizes[i].height, 1, 1, 1, i);
		if (planes[i].is_null()) {
			plane_count = i;
			free_image();
			ERR_FAIL_V_MSG(ERR_CANT_CREATE, vformat("Couldn't create the RenderingDevice texture for plane %d.", i));
		}
	}
	plane_count = MAX_PLANES;
	return OK;
}

static VkFormat _packed_vk_format(RD::DataFormat p_format) {
	switch (p_format) {
		case RD::DATA_FORMAT_R8G8B8A8_UNORM:
			return VK_FORMAT_R8G8B8A8_UNORM;
		case RD::DATA_FORMAT_A2B10G10R10_UNORM_PACK32:
			return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
		case RD::DATA_FORMAT_R16G16_UNORM:
			return VK_FORMAT_R16G16_UNORM;
		case RD::DATA_FORMAT_R32_UINT:
			return VK_FORMAT_R32_UINT;
		default:
			return VK_FORMAT_UNDEFINED;
	}
}

Error DmabufImage::import_planes(int p_fd, RD::DataFormat p_format, int64_t p_drm_modifier, const Size2i *p_sizes, const uint64_t *p_offsets, const uint64_t *p_pitches, uint32_t p_count, bool p_mutable) {
	ERR_FAIL_COND_V_MSG(is_imported(), ERR_ALREADY_IN_USE, "An image is already imported. Call free_image() first.");
	ERR_FAIL_COND_V(p_fd < 0, ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V(p_count < 1 || p_count > MAX_PLANES, ERR_INVALID_PARAMETER);
	const VkFormat vk_format = _packed_vk_format(p_format);
	ERR_FAIL_COND_V_MSG(vk_format == VK_FORMAT_UNDEFINED, ERR_INVALID_PARAMETER, "import_planes() takes R8G8B8A8_UNORM, A2B10G10R10_UNORM_PACK32, R16G16_UNORM or R32_UINT.");

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

	_process_pending_frees();

	RenderingDevice *rd = RenderingDevice::get_singleton();
	VkDevice device = (VkDevice)rd->get_driver_resource(RD::DRIVER_RESOURCE_LOGICAL_DEVICE);
	VkPhysicalDevice physical_device = (VkPhysicalDevice)rd->get_driver_resource(RD::DRIVER_RESOURCE_PHYSICAL_DEVICE);
	ERR_FAIL_COND_V(device == VK_NULL_HANDLE || physical_device == VK_NULL_HANDLE, ERR_UNAVAILABLE);
	PFN_vkGetMemoryFdPropertiesKHR get_memory_fd_properties = (PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(device, "vkGetMemoryFdPropertiesKHR");
	ERR_FAIL_NULL_V(get_memory_fd_properties, ERR_UNAVAILABLE);

	const uint64_t modifier = (uint64_t)p_drm_modifier;
	{
		LocalVector<VkDrmFormatModifierPropertiesEXT> properties;
		_get_modifier_properties(physical_device, vk_format, properties);
		const VkDrmFormatModifierPropertiesEXT *modifier_properties = nullptr;
		for (const VkDrmFormatModifierPropertiesEXT &p : properties) {
			if (p.drmFormatModifier == modifier) {
				modifier_properties = &p;
			}
		}
		ERR_FAIL_NULL_V_MSG(modifier_properties, ERR_UNAVAILABLE, vformat("DRM format modifier %s is not supported for VkFormat %d.", _modifier_to_string(modifier), vk_format));
		ERR_FAIL_COND_V_MSG(!(modifier_properties->drmFormatModifierTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT), ERR_UNAVAILABLE, vformat("VkFormat %d with modifier %s can't be sampled.", vk_format, _modifier_to_string(modifier)));
		ERR_FAIL_COND_V_MSG(modifier_properties->drmFormatModifierPlaneCount != 1, ERR_UNAVAILABLE, vformat("Modifier %s has %d memory planes for VkFormat %d; import_planes() needs 1.", _modifier_to_string(modifier), modifier_properties->drmFormatModifierPlaneCount, vk_format));
	}

	// Mutable to a format of another UBWC class: on A750 and later Turnip then sets the descriptors' MUTABLEEN bit, which lets
	// the texture unit read UBWC data written in another format. Turnip's format query refuses such a list with the UBWC
	// modifier (it doesn't consider that bit), so the query is only reported for it, not enforced.
	const VkFormat view_formats[2] = { vk_format, vk_format == VK_FORMAT_R32_UINT ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_R32_UINT };
	const uint32_t view_format_count = p_mutable ? 2 : 0;
	{
		VkExtent3D max_extent = {};
		bool importable = false;
		const VkResult err = _query_import(physical_device, vk_format, modifier, view_formats, view_format_count, max_extent, importable);
		if (err != VK_SUCCESS || !importable) {
			ERR_FAIL_COND_V_MSG(!p_mutable, ERR_UNAVAILABLE, vformat("VkFormat %d + modifier %s + dmabuf import is not supported (VkResult %d, importable %s).", vk_format, _modifier_to_string(modifier), err, importable ? "yes" : "no"));
			print_line(vformat("DmabufImage: the driver reports VkFormat %d + modifier %s, mutable to VkFormat %d, as unsupported (VkResult %d); creating it anyway (experiment).", vk_format, _modifier_to_string(modifier), view_formats[1], err));
		}
	}

	VkImage images[MAX_PLANES] = {};
	VkDeviceMemory memories[MAX_PLANES] = {};
	for (uint32_t i = 0; i < p_count; i++) {
		VkSubresourceLayout layout = {};
		layout.offset = p_offsets[i];
		layout.rowPitch = p_pitches[i];
		uint64_t memory_size = 0;
		uint32_t memory_type = 0;
		const Error import_err = _create_imported_image(device, get_memory_fd_properties, p_fd, vk_format, p_sizes[i].width, p_sizes[i].height, modifier, &layout, 1, view_formats, view_format_count, images[i], memories[i], memory_size, memory_type);
		if (import_err != OK) {
			for (uint32_t j = 0; j < i; j++) {
				vkFreeMemory(device, memories[j], nullptr);
				vkDestroyImage(device, images[j], nullptr);
			}
			return import_err;
		}
		if (OS::get_singleton()->is_stdout_verbose()) {
			VkImageSubresource subresource = {};
			subresource.aspectMask = VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT;
			VkSubresourceLayout actual = {};
			vkGetImageSubresourceLayout(device, images[i], &subresource, &actual);
			print_line(vformat("DmabufImage: plane %d imported as %dx%d VkFormat %d%s, modifier %s: offset %d, pitch %d, size %d (image needs %d bytes).", i, p_sizes[i].width, p_sizes[i].height, vk_format,
					p_mutable ? " (mutable)" : "", _modifier_to_string(modifier), (int64_t)actual.offset, (int64_t)actual.rowPitch, (int64_t)actual.size, (int64_t)memory_size));
		}
	}

	// From here on the images are ours to free, with free_image(). Plane i is the whole of image i.
	vk_device = device;
	for (uint32_t i = 0; i < p_count; i++) {
		vk_images[i] = images[i];
		vk_memories[i] = memories[i];
	}
	image_count = p_count;
	size = p_sizes[0];
	drm_modifier = modifier;
	acquired = false;
	for (uint32_t i = 0; i < p_count; i++) {
		planes[i] = rd->texture_create_from_extension(RD::TEXTURE_TYPE_2D, p_format, RD::TEXTURE_SAMPLES_1, RD::TEXTURE_USAGE_SAMPLING_BIT, (uint64_t)images[i], p_sizes[i].width, p_sizes[i].height, 1, 1, 1, -1);
		if (planes[i].is_null()) {
			plane_count = i;
			free_image();
			ERR_FAIL_V_MSG(ERR_CANT_CREATE, vformat("Couldn't create the RenderingDevice texture for plane %d.", i));
		}
	}
	plane_count = p_count;
	return OK;
}

Error DmabufImage::_import_planes_bind(int p_fd, RD::DataFormat p_format, int64_t p_drm_modifier, const PackedVector2Array &p_sizes, const PackedInt64Array &p_offsets, const PackedInt64Array &p_pitches, bool p_mutable) {
	const int count = p_sizes.size();
	ERR_FAIL_COND_V_MSG(count < 1 || count > MAX_PLANES || p_offsets.size() != count || p_pitches.size() != count, ERR_INVALID_PARAMETER, "Give one size, offset and pitch per plane (1 or 2).");
	Size2i sizes[MAX_PLANES];
	uint64_t offsets[MAX_PLANES];
	uint64_t pitches[MAX_PLANES];
	for (int i = 0; i < count; i++) {
		ERR_FAIL_COND_V(p_offsets[i] < 0 || p_pitches[i] <= 0 || p_sizes[i].x < 1 || p_sizes[i].y < 1, ERR_INVALID_PARAMETER);
		sizes[i] = Size2i(p_sizes[i]);
		offsets[i] = p_offsets[i];
		pitches[i] = p_pitches[i];
	}
	return import_planes(p_fd, p_format, p_drm_modifier, sizes, offsets, pitches, count, p_mutable);
}

Error DmabufImage::_import_host_test_pattern(int p_width, int p_height, bool p_split, bool p_p010) {
	const DmabufYUVFormat &yuv = p_p010 ? YUV_P010 : YUV_NV12;
	// Test path for machines without dmabufs (e.g. lavapipe): a linear NV12 image in host-visible memory, written through a
	// mapping. It goes through the same plane views, ownership transfers and shader as an imported dmabuf, but not the import.
	// Split: two images bound to the same memory, as a split dmabuf import makes them.
	RenderingDeviceDriverVulkan *driver = _get_vulkan_driver();
	ERR_FAIL_NULL_V(driver, ERR_UNAVAILABLE);
	ERR_FAIL_COND_V_MSG(!driver->is_sampler_ycbcr_conversion_enabled(), ERR_UNAVAILABLE, "samplerYcbcrConversion is not enabled, multi-planar images can't be created.");

	_process_pending_frees();

	RenderingDevice *rd = RenderingDevice::get_singleton();
	VkDevice device = (VkDevice)rd->get_driver_resource(RD::DRIVER_RESOURCE_LOGICAL_DEVICE);
	VkPhysicalDevice physical_device = (VkPhysicalDevice)rd->get_driver_resource(RD::DRIVER_RESOURCE_PHYSICAL_DEVICE);

	const uint32_t count = p_split ? 2 : 1;
	VkImage images[MAX_PLANES] = {};
	VkResult err = VK_SUCCESS;
	for (uint32_t i = 0; i < count && err == VK_SUCCESS; i++) {
		VkImageFormatListCreateInfo format_list = {};
		format_list.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO;
		format_list.viewFormatCount = p_split ? 1 : std_size(yuv.views);
		format_list.pViewFormats = p_split ? &yuv.split_views[i] : yuv.views;

		VkImageCreateInfo image_create_info = {};
		image_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		image_create_info.pNext = &format_list;
		image_create_info.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | (p_split ? VK_IMAGE_CREATE_ALIAS_BIT : 0);
		image_create_info.imageType = VK_IMAGE_TYPE_2D;
		image_create_info.format = yuv.format;
		image_create_info.extent = { (uint32_t)p_width, (uint32_t)p_height, 1 };
		image_create_info.mipLevels = 1;
		image_create_info.arrayLayers = 1;
		image_create_info.samples = VK_SAMPLE_COUNT_1_BIT;
		image_create_info.tiling = VK_IMAGE_TILING_LINEAR;
		image_create_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
		image_create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		image_create_info.initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
		err = vkCreateImage(device, &image_create_info, nullptr, &images[i]);
	}
	if (err != VK_SUCCESS) {
		for (uint32_t i = 0; i < count; i++) {
			if (images[i] != VK_NULL_HANDLE) {
				vkDestroyImage(device, images[i], nullptr);
			}
		}
		ERR_FAIL_V_MSG(ERR_CANT_CREATE, vformat("vkCreateImage failed for a linear %s image (VkResult %d).", yuv.name, err));
	}

	// One allocation that suits every image.
	VkMemoryRequirements memory_requirements = {};
	vkGetImageMemoryRequirements(device, images[0], &memory_requirements);
	for (uint32_t i = 1; i < count; i++) {
		VkMemoryRequirements other = {};
		vkGetImageMemoryRequirements(device, images[i], &other);
		memory_requirements.size = MAX(memory_requirements.size, other.size);
		memory_requirements.memoryTypeBits &= other.memoryTypeBits;
	}
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
		for (uint32_t i = 0; i < count; i++) {
			vkDestroyImage(device, images[i], nullptr);
		}
		ERR_FAIL_V_MSG(ERR_UNAVAILABLE, "No host-visible memory type for a linear NV12 image.");
	}

	VkMemoryAllocateInfo allocate_info = {};
	allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocate_info.allocationSize = memory_requirements.size;
	allocate_info.memoryTypeIndex = memory_type_index;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	err = vkAllocateMemory(device, &allocate_info, nullptr, &memory);
	for (uint32_t i = 0; i < count && err == VK_SUCCESS; i++) {
		err = vkBindImageMemory(device, images[i], memory, 0);
	}
	void *data = nullptr;
	if (err == VK_SUCCESS) {
		err = vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &data);
	}
	if (err != VK_SUCCESS) {
		if (memory != VK_NULL_HANDLE) {
			vkFreeMemory(device, memory, nullptr);
		}
		for (uint32_t i = 0; i < count; i++) {
			vkDestroyImage(device, images[i], nullptr);
		}
		ERR_FAIL_V_MSG(ERR_CANT_CREATE, vformat("Allocating or mapping host memory for the test pattern failed (VkResult %d).", err));
	}

	VkSubresourceLayout plane_layouts[MAX_PLANES] = {};
	for (uint32_t i = 0; i < MAX_PLANES; i++) {
		VkImageSubresource subresource = {};
		subresource.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT << i;
		vkGetImageSubresourceLayout(device, images[0], &subresource, &plane_layouts[i]);
		if (p_split) {
			VkSubresourceLayout other = {};
			vkGetImageSubresourceLayout(device, images[1], &subresource, &other);
			if (other.offset != plane_layouts[i].offset || other.rowPitch != plane_layouts[i].rowPitch) {
				WARN_PRINT(vformat("DmabufImage: the two images of the split test pattern lay out plane %d differently.", i));
			}
		}
	}
	_fill_test_pattern((uint8_t *)data + plane_layouts[0].offset, plane_layouts[0].rowPitch, (uint8_t *)data + plane_layouts[1].offset, plane_layouts[1].rowPitch, p_width, p_height, yuv.bytes_per_sample);
	vkUnmapMemory(device, memory);
	print_line(vformat("DmabufImage: %s test pattern %dx%d in host memory (no dmabuf, %d image(s)), planes at %d and %d, pitches %d and %d.", yuv.name, p_width, p_height, count, (int64_t)plane_layouts[0].offset, (int64_t)plane_layouts[1].offset, (int64_t)plane_layouts[0].rowPitch, (int64_t)plane_layouts[1].rowPitch));

	// The memory is freed with the first image; a second image only aliases it.
	VkDeviceMemory memories[MAX_PLANES] = { memory, VK_NULL_HANDLE };
	return _create_planes(device, images, memories, count, p_width, p_height, DRM_FORMAT_MOD_LINEAR, yuv.plane_formats);
}

static void _put_sample(uint8_t *p_row, uint32_t p_index, uint8_t p_value, uint32_t p_bytes) {
	if (p_bytes == 2) {
		((uint16_t *)p_row)[p_index] = (uint16_t)(p_value << 8); // P010: the 10-bit code (v << 2) in the high bits.
	} else {
		p_row[p_index] = p_value;
	}
}

void DmabufImage::_fill_test_pattern(uint8_t *p_luma, uint64_t p_luma_pitch, uint8_t *p_chroma, uint64_t p_chroma_pitch, uint32_t p_width, uint32_t p_height, uint32_t p_bytes) {
	// Luma: dark to bright from left to right (limited range, 16-235), with a white grid every 1/8 of the size.
	const uint32_t grid_x = MAX(p_width / 8, 1u);
	const uint32_t grid_y = MAX(p_height / 8, 1u);
	for (uint32_t y = 0; y < p_height; y++) {
		uint8_t *row = p_luma + y * p_luma_pitch;
		for (uint32_t x = 0; x < p_width; x++) {
			const bool grid = (x % grid_x) < 4 || (y % grid_y) < 4;
			_put_sample(row, x, grid ? 235 : uint8_t(16 + (219 * x) / (p_width - 1)), p_bytes);
		}
	}

	// Chroma (interleaved Cb Cr, half size): Cb rises from top to bottom, Cr from left to right (16-240).
	const uint32_t chroma_width = p_width / 2;
	const uint32_t chroma_height = p_height / 2;
	for (uint32_t y = 0; y < chroma_height; y++) {
		uint8_t *row = p_chroma + y * p_chroma_pitch;
		for (uint32_t x = 0; x < chroma_width; x++) {
			_put_sample(row, x * 2 + 0, uint8_t(16 + (224 * y) / (chroma_height - 1)), p_bytes);
			_put_sample(row, x * 2 + 1, uint8_t(16 + (224 * x) / (chroma_width - 1)), p_bytes);
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
	_fill_test_pattern((uint8_t *)data, pitch, (uint8_t *)data + uv_offset, pitch, p_width, p_height, 1);
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
	_fill_test_pattern((uint8_t *)data, pitch, (uint8_t *)data + uv_offset, pitch, p_width, p_height, 1);
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
	if (p_device.begins_with("host")) {
		// "host", "host-split", "host-p010", "host-p010-split".
		return _import_host_test_pattern(p_width, p_height, p_device.ends_with("split"), p_device.contains("p010"));
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

/* YCbCr import: Q10C through a sampler with a YCbCr conversion */

// The pass: reads the image through the YCbCr sampler (RGB_IDENTITY, full range: the raw codes, G = Y, B = Cb, R = Cr) and
// writes them as P010 does (the code in the high bits of 16), so every shader reads the result like a P010 frame. Luma at
// its texel centres, chroma at the centre of each 2x2 luma block (the centre of its chroma texel for MIDPOINT, inside it
// for COSITED_EVEN), all NEAREST: the codes come out unchanged. One invocation per chroma sample.
// code = floor(sample * scale + bias): for 10 and 12 bits in a 16-bit container that is right whether the driver
// normalises by 2^bits - 1 (the spec) or reads the container as 16-bit unorm (Mesa maps R10X6 to R16).
static const char *YCBCR_SHADER = R"(#version 450
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform sampler2D picture;
layout(set = 0, binding = 1, r16) uniform restrict writeonly image2D out_y;
layout(set = 0, binding = 2, rg16) uniform restrict writeonly image2D out_cbcr;
layout(push_constant) uniform Params {
	uvec2 size;
	float scale;
	float bias;
	float out_scale;
} params;
float code(float v) {
	return floor(clamp(v, 0.0, 1.0) * params.scale + params.bias);
}
void main() {
	const uvec2 c = gl_GlobalInvocationID.xy;
	const uvec2 half_size = params.size / 2u;
	if (c.x >= half_size.x || c.y >= half_size.y) {
		return;
	}
	const vec2 inv = 1.0 / vec2(params.size);
	for (uint dy = 0u; dy < 2u; dy++) {
		for (uint dx = 0u; dx < 2u; dx++) {
			const uvec2 p = c * 2u + uvec2(dx, dy);
			const float y = textureLod(picture, (vec2(p) + 0.5) * inv, 0.0).g;
			imageStore(out_y, ivec2(p), vec4(code(y) * params.out_scale, 0.0, 0.0, 1.0));
		}
	}
	const vec4 s = textureLod(picture, (vec2(c * 2u) + 1.0) * inv, 0.0);
	imageStore(out_cbcr, ivec2(c), vec4(code(s.b) * params.out_scale, code(s.r) * params.out_scale, 0.0, 1.0));
}
)";

// Shared by every image of one format (the immutable sampler is part of the descriptor set layout). Kept until exit, like
// the RenderingDevice's own pipelines.
struct DmabufYCbCrPipeline {
	VkDevice device = VK_NULL_HANDLE;
	VkFormat format = VK_FORMAT_UNDEFINED;
	bool midpoint = true;
	uint32_t sampler_descriptors = 1;
	VkSamplerYcbcrConversion conversion = VK_NULL_HANDLE;
	VkSampler sampler = VK_NULL_HANDLE;
	VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
	VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
	VkPipeline pipeline = VK_NULL_HANDLE;
	float scale = 1023.0f;
	float bias = 0.5f;
	float out_scale = 64.0f / 65535.0f;
};

struct YCbCrPushConstants {
	uint32_t width;
	uint32_t height;
	float scale;
	float bias;
	float out_scale;
};

static Mutex ycbcr_mutex;
static LocalVector<DmabufYCbCrPipeline *> ycbcr_pipelines;
static Vector<uint8_t> ycbcr_spirv;

static Mutex q10c_mutex;
static bool q10c_looked = false;
static LocalVector<DmabufImage::YCbCrCandidate> q10c_candidates;
static String q10c_report;

static int _ycbcr_bits(VkFormat p_format) {
	switch (p_format) {
		case VK_FORMAT_G8_B8R8_2PLANE_420_UNORM:
			return 8;
		case VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16:
			return 12;
		case VK_FORMAT_G16_B16R16_2PLANE_420_UNORM:
			return 16;
		default:
			return 10; // G10X6, and an unknown (vendor) format: Q10C's samples are 10-bit.
	}
}

static String _vk_format_name(uint32_t p_format) {
	switch (p_format) {
		case VK_FORMAT_G8_B8R8_2PLANE_420_UNORM:
			return "G8_B8R8_2PLANE_420_UNORM";
		case VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16:
			return "G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16";
		case VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16:
			return "G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16";
		case VK_FORMAT_G16_B16R16_2PLANE_420_UNORM:
			return "G16_B16R16_2PLANE_420_UNORM";
		default:
			return "VkFormat " + itos(p_format);
	}
}

String DmabufImage::YCbCrCandidate::describe() const {
	return vformat("%s (%d), modifier %s, %d memory plane(s) at the %s", _vk_format_name(format), format, _modifier_to_string(modifier), memory_planes, layout);
}

// combinedImageSamplerDescriptorCount for the format and tiling (a multi-planar YCbCr image can need more than one).
static uint32_t _ycbcr_descriptor_count(VkPhysicalDevice p_physical_device, VkFormat p_format, uint64_t p_modifier, bool p_linear) {
	VkPhysicalDeviceImageDrmFormatModifierInfoEXT modifier_info = {};
	modifier_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT;
	modifier_info.drmFormatModifier = p_modifier;
	modifier_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	VkPhysicalDeviceImageFormatInfo2 format_info = {};
	format_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
	format_info.pNext = p_linear ? nullptr : &modifier_info;
	format_info.format = p_format;
	format_info.type = VK_IMAGE_TYPE_2D;
	format_info.tiling = p_linear ? VK_IMAGE_TILING_LINEAR : VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
	format_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
	VkSamplerYcbcrConversionImageFormatProperties ycbcr_properties = {};
	ycbcr_properties.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_IMAGE_FORMAT_PROPERTIES;
	VkImageFormatProperties2 properties = {};
	properties.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
	properties.pNext = &ycbcr_properties;
	if (vkGetPhysicalDeviceImageFormatProperties2(p_physical_device, &format_info, &properties) != VK_SUCCESS) {
		return 1;
	}
	return MAX(ycbcr_properties.combinedImageSamplerDescriptorCount, 1u);
}

static void _destroy_ycbcr_pipeline(DmabufYCbCrPipeline *p_pipeline) {
	VkDevice device = p_pipeline->device;
	if (p_pipeline->pipeline != VK_NULL_HANDLE) {
		vkDestroyPipeline(device, p_pipeline->pipeline, nullptr);
	}
	if (p_pipeline->pipeline_layout != VK_NULL_HANDLE) {
		vkDestroyPipelineLayout(device, p_pipeline->pipeline_layout, nullptr);
	}
	if (p_pipeline->set_layout != VK_NULL_HANDLE) {
		vkDestroyDescriptorSetLayout(device, p_pipeline->set_layout, nullptr);
	}
	if (p_pipeline->sampler != VK_NULL_HANDLE) {
		vkDestroySampler(device, p_pipeline->sampler, nullptr);
	}
	if (p_pipeline->conversion != VK_NULL_HANDLE) {
		vkDestroySamplerYcbcrConversion(device, p_pipeline->conversion, nullptr);
	}
	memdelete(p_pipeline);
}

static DmabufYCbCrPipeline *_get_ycbcr_pipeline(VkDevice p_device, VkFormat p_format, bool p_midpoint, uint32_t p_descriptors, String &r_error) {
	MutexLock lock(ycbcr_mutex);
	for (DmabufYCbCrPipeline *pipeline : ycbcr_pipelines) {
		if (pipeline->device == p_device && pipeline->format == p_format && pipeline->midpoint == p_midpoint && pipeline->sampler_descriptors == p_descriptors) {
			return pipeline;
		}
	}
	if (vkCreateSamplerYcbcrConversion == nullptr) {
		r_error = "vkCreateSamplerYcbcrConversion isn't loaded (Vulkan 1.1 is needed).";
		return nullptr;
	}
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (ycbcr_spirv.is_empty()) {
		String compile_error;
		ycbcr_spirv = rd->shader_compile_spirv_from_source(RD::SHADER_STAGE_COMPUTE, YCBCR_SHADER, RD::SHADER_LANGUAGE_GLSL, &compile_error, false);
		if (ycbcr_spirv.is_empty()) {
			r_error = "Compiling the YCbCr pass failed: " + compile_error;
			return nullptr;
		}
	}

	DmabufYCbCrPipeline *pipeline = memnew(DmabufYCbCrPipeline);
	pipeline->device = p_device;
	pipeline->format = p_format;
	pipeline->midpoint = p_midpoint;
	pipeline->sampler_descriptors = p_descriptors;
	const int bits = _ycbcr_bits(p_format);
	pipeline->scale = (float)((1u << bits) - 1u);
	pipeline->bias = 0.5f;
	if (bits > 8 && bits < 16) {
		pipeline->scale = 65535.0f / (float)(1u << (16 - bits));
		pipeline->bias = 0.0005f;
	}
	pipeline->out_scale = (float)(1u << (16 - bits)) / 65535.0f;

	VkSamplerYcbcrConversionCreateInfo conversion_info = {};
	conversion_info.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO;
	conversion_info.format = p_format;
	// RGB_IDENTITY: no range expansion and no matrix (YCBCR_IDENTITY with ITU_FULL would still subtract the chroma midpoint).
	// The app's shader keeps doing the matrix, the range, HDR and BT.2020, as for P010.
	conversion_info.ycbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_RGB_IDENTITY;
	conversion_info.ycbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_FULL;
	conversion_info.components = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY };
	conversion_info.xChromaOffset = p_midpoint ? VK_CHROMA_LOCATION_MIDPOINT : VK_CHROMA_LOCATION_COSITED_EVEN;
	conversion_info.yChromaOffset = conversion_info.xChromaOffset;
	conversion_info.chromaFilter = VK_FILTER_NEAREST;
	VkResult err = vkCreateSamplerYcbcrConversion(p_device, &conversion_info, nullptr, &pipeline->conversion);

	VkSamplerYcbcrConversionInfo conversion_link = {};
	conversion_link.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO;
	conversion_link.conversion = pipeline->conversion;
	if (err == VK_SUCCESS) {
		VkSamplerCreateInfo sampler_info = {};
		sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
		sampler_info.pNext = &conversion_link;
		sampler_info.magFilter = VK_FILTER_NEAREST;
		sampler_info.minFilter = VK_FILTER_NEAREST;
		sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
		sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		sampler_info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
		err = vkCreateSampler(p_device, &sampler_info, nullptr, &pipeline->sampler);
	}
	if (err == VK_SUCCESS) {
		VkDescriptorSetLayoutBinding bindings[3] = {};
		bindings[0].binding = 0;
		bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		bindings[0].descriptorCount = 1;
		bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
		bindings[0].pImmutableSamplers = &pipeline->sampler; // A YCbCr conversion sampler must be immutable.
		for (uint32_t i = 1; i < 3; i++) {
			bindings[i].binding = i;
			bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
			bindings[i].descriptorCount = 1;
			bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
		}
		VkDescriptorSetLayoutCreateInfo set_layout_info = {};
		set_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
		set_layout_info.bindingCount = 3;
		set_layout_info.pBindings = bindings;
		err = vkCreateDescriptorSetLayout(p_device, &set_layout_info, nullptr, &pipeline->set_layout);
	}
	if (err == VK_SUCCESS) {
		VkPushConstantRange push_range = { VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(YCbCrPushConstants) };
		VkPipelineLayoutCreateInfo layout_info = {};
		layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		layout_info.setLayoutCount = 1;
		layout_info.pSetLayouts = &pipeline->set_layout;
		layout_info.pushConstantRangeCount = 1;
		layout_info.pPushConstantRanges = &push_range;
		err = vkCreatePipelineLayout(p_device, &layout_info, nullptr, &pipeline->pipeline_layout);
	}
	if (err == VK_SUCCESS) {
		VkShaderModuleCreateInfo module_info = {};
		module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		module_info.codeSize = ycbcr_spirv.size();
		module_info.pCode = (const uint32_t *)ycbcr_spirv.ptr();
		VkShaderModule module = VK_NULL_HANDLE;
		err = vkCreateShaderModule(p_device, &module_info, nullptr, &module);
		if (err == VK_SUCCESS) {
			VkComputePipelineCreateInfo pipeline_info = {};
			pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
			pipeline_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
			pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
			pipeline_info.stage.module = module;
			pipeline_info.stage.pName = "main";
			pipeline_info.layout = pipeline->pipeline_layout;
			err = vkCreateComputePipelines(p_device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline->pipeline);
			vkDestroyShaderModule(p_device, module, nullptr);
		}
	}
	if (err != VK_SUCCESS) {
		r_error = vformat("Creating the YCbCr conversion, sampler or pass for %s failed (VkResult %d).", _vk_format_name(p_format), err);
		_destroy_ycbcr_pipeline(pipeline);
		return nullptr;
	}
	print_verbose(vformat("DmabufImage: YCbCr pass for %s (chroma %s, %d combined image sampler descriptor(s)).", _vk_format_name(p_format), p_midpoint ? "midpoint" : "cosited", p_descriptors));
	ycbcr_pipelines.push_back(pipeline);
	return pipeline;
}

Error DmabufImage::_setup_ycbcr(VkDevice p_device, VkImage p_image, VkDeviceMemory p_memory, uint32_t p_vk_format, uint64_t p_features, int p_width, int p_height, uint64_t p_modifier, bool p_external, RID p_out_y, RID p_out_cbcr) {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	VkPhysicalDevice physical_device = (VkPhysicalDevice)rd->get_driver_resource(RD::DRIVER_RESOURCE_PHYSICAL_DEVICE);
	const VkFormat format = (VkFormat)p_vk_format;
	const bool midpoint = p_features & VK_FORMAT_FEATURE_MIDPOINT_CHROMA_SAMPLES_BIT;
	if (!midpoint && !(p_features & VK_FORMAT_FEATURE_COSITED_CHROMA_SAMPLES_BIT)) {
		print_line(vformat("DmabufImage: %s reports neither midpoint nor cosited chroma samples (features 0x%x); assuming midpoint.", _vk_format_name(p_vk_format), (int64_t)p_features));
	}
	const uint32_t descriptors = _ycbcr_descriptor_count(physical_device, format, p_modifier, !p_external);
	String error;
	DmabufYCbCrPipeline *pipeline = _get_ycbcr_pipeline(p_device, format, midpoint || !(p_features & VK_FORMAT_FEATURE_COSITED_CHROMA_SAMPLES_BIT), descriptors, error);

	YCbCrState *state = memnew(YCbCrState);
	state->pipeline = pipeline;
	state->image = p_image;
	state->width = p_width;
	state->height = p_height;
	state->external = p_external;
	state->preinitialized = !p_external;
	RenderingDeviceDriverVulkan *driver = _get_vulkan_driver();
	state->foreign_queue_family = driver->is_device_extension_enabled(VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME) ? VK_QUEUE_FAMILY_FOREIGN_EXT : VK_QUEUE_FAMILY_EXTERNAL;
	state->out[0] = p_out_y;
	state->out[1] = p_out_cbcr;

	VkResult err = VK_ERROR_INITIALIZATION_FAILED;
	VkImageView out_views[MAX_PLANES] = {};
	if (pipeline != nullptr) {
		VkSamplerYcbcrConversionInfo conversion_link = {};
		conversion_link.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO;
		conversion_link.conversion = pipeline->conversion;
		VkImageViewCreateInfo view_info = {};
		view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		view_info.pNext = &conversion_link;
		view_info.image = p_image;
		view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
		view_info.format = format;
		view_info.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		err = vkCreateImageView(p_device, &view_info, nullptr, &state->view);
		if (err != VK_SUCCESS) {
			error = vformat("vkCreateImageView with the YCbCr conversion failed (VkResult %d).", err);
		}
		for (uint32_t i = 0; i < MAX_PLANES && err == VK_SUCCESS; i++) {
			out_views[i] = rd->texture_is_valid(state->out[i]) ? (VkImageView)rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE_VIEW, state->out[i]) : VK_NULL_HANDLE;
			if (out_views[i] == VK_NULL_HANDLE) {
				err = VK_ERROR_INITIALIZATION_FAILED;
				error = "The YCbCr pass's out textures aren't valid RenderingDevice textures.";
			}
		}
	}
	if (err == VK_SUCCESS) {
		VkDescriptorPoolSize pool_sizes[2] = { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, descriptors }, { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 } };
		VkDescriptorPoolCreateInfo pool_info = {};
		pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
		pool_info.maxSets = 1;
		pool_info.poolSizeCount = 2;
		pool_info.pPoolSizes = pool_sizes;
		err = vkCreateDescriptorPool(p_device, &pool_info, nullptr, &state->pool);
		if (err == VK_SUCCESS) {
			VkDescriptorSetAllocateInfo set_info = {};
			set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
			set_info.descriptorPool = state->pool;
			set_info.descriptorSetCount = 1;
			set_info.pSetLayouts = &pipeline->set_layout;
			err = vkAllocateDescriptorSets(p_device, &set_info, &state->set);
		}
		if (err != VK_SUCCESS) {
			error = vformat("Allocating the YCbCr pass's descriptor set failed (VkResult %d).", err);
		}
	}
	if (err != VK_SUCCESS) {
		if (state->pool != VK_NULL_HANDLE) {
			vkDestroyDescriptorPool(p_device, state->pool, nullptr);
		}
		if (state->view != VK_NULL_HANDLE) {
			vkDestroyImageView(p_device, state->view, nullptr);
		}
		memdelete(state);
		vkFreeMemory(p_device, p_memory, nullptr);
		vkDestroyImage(p_device, p_image, nullptr);
		ERR_FAIL_V_MSG(ERR_CANT_CREATE, "DmabufImage: " + error);
	}
	VkDescriptorImageInfo image_infos[3] = {};
	image_infos[0].imageView = state->view;
	image_infos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	for (uint32_t i = 0; i < MAX_PLANES; i++) {
		image_infos[1 + i].imageView = out_views[i];
		image_infos[1 + i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
	}
	VkWriteDescriptorSet writes[3] = {};
	for (uint32_t i = 0; i < 3; i++) {
		writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[i].dstSet = state->set;
		writes[i].dstBinding = i;
		writes[i].descriptorCount = 1;
		writes[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		writes[i].pImageInfo = &image_infos[i];
	}
	vkUpdateDescriptorSets(p_device, 3, writes, 0, nullptr);

	// From here on the image is ours to free, with free_image(). The planes are the caller's textures.
	vk_device = p_device;
	vk_images[0] = p_image;
	vk_memories[0] = p_memory;
	image_count = 1;
	size = Size2i(p_width, p_height);
	drm_modifier = p_modifier;
	acquired = false;
	ycbcr = state;
	planes[0] = p_out_y;
	planes[1] = p_out_cbcr;
	plane_count = MAX_PLANES;
	ycbcr_description = vformat("%s, modifier %s, chroma %s", _vk_format_name(p_vk_format), _modifier_to_string(p_modifier), pipeline->midpoint ? "midpoint" : "cosited");
	return OK;
}

// Driver callback (render thread, while the RenderingDevice graph records this frame's command buffer): the dmabuf comes from
// the foreign queue family (the decoder wrote it), the pass writes the out textures, and the dmabuf goes back.
void DmabufImage::_record_ycbcr(RenderingDeviceDriver *p_driver, RDD::CommandBufferID p_command_buffer, void *p_userdata) {
	YCbCrState *state = (YCbCrState *)p_userdata;
	const VkCommandBuffer command_buffer = RenderingDeviceDriverVulkan::command_buffer_get_vk(p_command_buffer);
	const uint32_t queue_family = RenderingDeviceDriverVulkan::command_buffer_get_queue_family(p_command_buffer);
	const DmabufYCbCrPipeline *pipeline = state->pipeline;

	VkImageMemoryBarrier barrier = {};
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.image = state->image;
	barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	barrier.srcAccessMask = state->external ? 0 : VK_ACCESS_HOST_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	barrier.oldLayout = state->preinitialized ? VK_IMAGE_LAYOUT_PREINITIALIZED : VK_IMAGE_LAYOUT_GENERAL;
	barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	barrier.srcQueueFamilyIndex = state->external ? state->foreign_queue_family : VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = state->external ? queue_family : VK_QUEUE_FAMILY_IGNORED;
	state->preinitialized = false;
	vkCmdPipelineBarrier(command_buffer, state->external ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

	vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline->pipeline);
	vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline->pipeline_layout, 0, 1, &state->set, 0, nullptr);
	const YCbCrPushConstants push = { state->width, state->height, pipeline->scale, pipeline->bias, pipeline->out_scale };
	vkCmdPushConstants(command_buffer, pipeline->pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
	vkCmdDispatch(command_buffer, (state->width / 2 + 7) / 8, (state->height / 2 + 7) / 8, 1);

	// Back to the producer once the pass has read it. The decoder must not write it until this submission has completed (the
	// caller's re-queue rule, as for the other imports).
	barrier.srcAccessMask = 0;
	barrier.dstAccessMask = 0;
	barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
	barrier.srcQueueFamilyIndex = state->external ? queue_family : VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = state->external ? state->foreign_queue_family : VK_QUEUE_FAMILY_IGNORED;
	vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

Error DmabufImage::import_ycbcr(int p_fd, uint32_t p_vk_format, uint64_t p_modifier, int p_width, int p_height, const uint64_t *p_offsets, const uint64_t *p_pitches, uint32_t p_count, RID p_out_y, RID p_out_cbcr) {
	ERR_FAIL_COND_V_MSG(is_imported(), ERR_ALREADY_IN_USE, "An image is already imported. Call free_image() first.");
	ERR_FAIL_COND_V(p_fd < 0, ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V(p_count < 1 || p_count > MAX_MEMORY_PLANES, ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V_MSG(p_width <= 0 || p_height <= 0 || (p_width % 2) != 0 || (p_height % 2) != 0, ERR_INVALID_PARAMETER, "YCbCr import: width and height must be positive and even.");

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
	ERR_FAIL_COND_V_MSG(!driver->is_sampler_ycbcr_conversion_enabled(), ERR_UNAVAILABLE, "samplerYcbcrConversion is not enabled.");

	_process_pending_frees();

	RenderingDevice *rd = RenderingDevice::get_singleton();
	VkDevice device = (VkDevice)rd->get_driver_resource(RD::DRIVER_RESOURCE_LOGICAL_DEVICE);
	VkPhysicalDevice physical_device = (VkPhysicalDevice)rd->get_driver_resource(RD::DRIVER_RESOURCE_PHYSICAL_DEVICE);
	PFN_vkGetMemoryFdPropertiesKHR get_memory_fd_properties = (PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(device, "vkGetMemoryFdPropertiesKHR");
	ERR_FAIL_NULL_V(get_memory_fd_properties, ERR_UNAVAILABLE);

	const VkFormat format = (VkFormat)p_vk_format;
	uint64_t features = 0;
	{
		LocalVector<VkDrmFormatModifierPropertiesEXT> properties;
		_get_modifier_properties(physical_device, format, properties);
		bool listed = false;
		for (const VkDrmFormatModifierPropertiesEXT &p : properties) {
			if (p.drmFormatModifier == p_modifier) {
				listed = true;
				features = p.drmFormatModifierTilingFeatures;
				ERR_FAIL_COND_V_MSG(p.drmFormatModifierPlaneCount != p_count, ERR_INVALID_PARAMETER, vformat("%s with modifier %s has %d memory plane(s), but %d were given.", _vk_format_name(p_vk_format), _modifier_to_string(p_modifier), p.drmFormatModifierPlaneCount, p_count));
			}
		}
		if (!listed) {
			VkFormatProperties format_properties = {};
			vkGetPhysicalDeviceFormatProperties(physical_device, format, &format_properties);
			features = format_properties.optimalTilingFeatures;
			print_line(vformat("DmabufImage: %s doesn't list modifier %s; trying it anyway (an override).", _vk_format_name(p_vk_format), _modifier_to_string(p_modifier)));
		}
	}
	VkExtent3D max_extent = {};
	bool importable = false;
	const VkResult query = _query_import(physical_device, format, p_modifier, nullptr, 0, max_extent, importable);
	ERR_FAIL_COND_V_MSG(query != VK_SUCCESS || !importable, ERR_UNAVAILABLE, vformat("%s + modifier %s + dmabuf import is not supported (VkResult %d, importable %s).", _vk_format_name(p_vk_format), _modifier_to_string(p_modifier), query, importable ? "yes" : "no"));
	ERR_FAIL_COND_V_MSG((uint32_t)p_width > max_extent.width || (uint32_t)p_height > max_extent.height, ERR_INVALID_PARAMETER, vformat("Size %dx%d is larger than the maximum %dx%d.", p_width, p_height, max_extent.width, max_extent.height));

	VkSubresourceLayout plane_layouts[MAX_MEMORY_PLANES] = {};
	for (uint32_t i = 0; i < p_count; i++) {
		plane_layouts[i].offset = p_offsets[i];
		plane_layouts[i].rowPitch = p_pitches[i];
	}
	VkImage image = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	uint64_t memory_size = 0;
	uint32_t memory_type = 0;
	const Error err = _create_imported_image(device, get_memory_fd_properties, p_fd, format, p_width, p_height, p_modifier, plane_layouts, p_count, nullptr, 0, image, memory, memory_size, memory_type);
	if (err != OK) {
		return err;
	}
	if (OS::get_singleton()->is_stdout_verbose()) {
		print_line(vformat("DmabufImage: imported %dx%d %s for the YCbCr pass, modifier %s, %d bytes, memory type %d.", p_width, p_height, _vk_format_name(p_vk_format), _modifier_to_string(p_modifier), (int64_t)memory_size, memory_type));
		for (uint32_t p = 0; p < p_count; p++) {
			VkImageSubresource subresource = {};
			subresource.aspectMask = VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT << p;
			VkSubresourceLayout layout = {};
			vkGetImageSubresourceLayout(device, image, &subresource, &layout);
			print_line(vformat("DmabufImage:   memory plane %d: offset %d, pitch %d, size %d.", p, (int64_t)layout.offset, (int64_t)layout.rowPitch, (int64_t)layout.size));
		}
	}
	return _setup_ycbcr(device, image, memory, p_vk_format, features, p_width, p_height, p_modifier, true, p_out_y, p_out_cbcr);
}

Error DmabufImage::_import_ycbcr_bind(int p_fd, int p_vk_format, int64_t p_modifier, int p_width, int p_height, const PackedInt64Array &p_offsets, const PackedInt64Array &p_pitches, RID p_out_y, RID p_out_cbcr) {
	const int count = p_offsets.size();
	ERR_FAIL_COND_V_MSG(count < 1 || count > (int)MAX_MEMORY_PLANES || p_pitches.size() != count, ERR_INVALID_PARAMETER, "Give one offset and one pitch per memory plane (1 to 4).");
	uint64_t offsets[MAX_MEMORY_PLANES];
	uint64_t pitches[MAX_MEMORY_PLANES];
	for (int i = 0; i < count; i++) {
		ERR_FAIL_COND_V(p_offsets[i] < 0 || p_pitches[i] <= 0, ERR_INVALID_PARAMETER);
		offsets[i] = p_offsets[i];
		pitches[i] = p_pitches[i];
	}
	return import_ycbcr(p_fd, (uint32_t)p_vk_format, (uint64_t)p_modifier, p_width, p_height, offsets, pitches, count, p_out_y, p_out_cbcr);
}

Error DmabufImage::import_ycbcr_test_pattern(int p_width, int p_height, RID p_out_y, RID p_out_cbcr, int p_bits) {
	ERR_FAIL_COND_V_MSG(is_imported(), ERR_ALREADY_IN_USE, "An image is already imported. Call free_image() first.");
	ERR_FAIL_COND_V(p_width <= 0 || p_height <= 0 || (p_width % 2) != 0 || (p_height % 2) != 0, ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V_MSG(p_bits != 8 && p_bits != 10, ERR_INVALID_PARAMETER, "p_bits is 8 or 10.");
	RenderingDeviceDriverVulkan *driver = _get_vulkan_driver();
	ERR_FAIL_NULL_V(driver, ERR_UNAVAILABLE);
	ERR_FAIL_COND_V_MSG(!driver->is_sampler_ycbcr_conversion_enabled(), ERR_UNAVAILABLE, "samplerYcbcrConversion is not enabled.");
	_process_pending_frees();

	RenderingDevice *rd = RenderingDevice::get_singleton();
	VkDevice device = (VkDevice)rd->get_driver_resource(RD::DRIVER_RESOURCE_LOGICAL_DEVICE);
	VkPhysicalDevice physical_device = (VkPhysicalDevice)rd->get_driver_resource(RD::DRIVER_RESOURCE_PHYSICAL_DEVICE);
	const VkFormat format = p_bits == 8 ? VK_FORMAT_G8_B8R8_2PLANE_420_UNORM : VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16;
	VkFormatProperties format_properties = {};
	vkGetPhysicalDeviceFormatProperties(physical_device, format, &format_properties);
	ERR_FAIL_COND_V_MSG(!(format_properties.linearTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT), ERR_UNAVAILABLE, vformat("%s can't be sampled with linear tiling here.", _vk_format_name(format)));

	VkImageCreateInfo image_info = {};
	image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	image_info.imageType = VK_IMAGE_TYPE_2D;
	image_info.format = format;
	image_info.extent = { (uint32_t)p_width, (uint32_t)p_height, 1 };
	image_info.mipLevels = 1;
	image_info.arrayLayers = 1;
	image_info.samples = VK_SAMPLE_COUNT_1_BIT;
	image_info.tiling = VK_IMAGE_TILING_LINEAR;
	image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
	image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	image_info.initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
	VkImage image = VK_NULL_HANDLE;
	VkResult err = vkCreateImage(device, &image_info, nullptr, &image);
	ERR_FAIL_COND_V_MSG(err != VK_SUCCESS, ERR_CANT_CREATE, vformat("vkCreateImage failed for a linear %s image (VkResult %d).", _vk_format_name(format), err));

	VkMemoryRequirements requirements = {};
	vkGetImageMemoryRequirements(device, image, &requirements);
	VkPhysicalDeviceMemoryProperties memory_properties = {};
	vkGetPhysicalDeviceMemoryProperties(physical_device, &memory_properties);
	const VkMemoryPropertyFlags wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	uint32_t memory_type = UINT32_MAX;
	for (uint32_t i = 0; i < memory_properties.memoryTypeCount; i++) {
		if ((requirements.memoryTypeBits & (1u << i)) && (memory_properties.memoryTypes[i].propertyFlags & wanted) == wanted) {
			memory_type = i;
			break;
		}
	}
	VkDeviceMemory memory = VK_NULL_HANDLE;
	if (memory_type != UINT32_MAX) {
		VkMemoryAllocateInfo allocate_info = {};
		allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		allocate_info.allocationSize = requirements.size;
		allocate_info.memoryTypeIndex = memory_type;
		err = vkAllocateMemory(device, &allocate_info, nullptr, &memory);
	}
	void *data = nullptr;
	if (memory != VK_NULL_HANDLE && err == VK_SUCCESS) {
		err = vkBindImageMemory(device, image, memory, 0);
		if (err == VK_SUCCESS) {
			err = vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &data);
		}
	}
	if (data == nullptr) {
		if (memory != VK_NULL_HANDLE) {
			vkFreeMemory(device, memory, nullptr);
		}
		vkDestroyImage(device, image, nullptr);
		ERR_FAIL_V_MSG(ERR_CANT_CREATE, "No host-visible memory for the YCbCr test pattern.");
	}
	VkSubresourceLayout plane_layouts[MAX_PLANES] = {};
	for (uint32_t i = 0; i < MAX_PLANES; i++) {
		VkImageSubresource subresource = {};
		subresource.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT << i;
		vkGetImageSubresourceLayout(device, image, &subresource, &plane_layouts[i]);
	}
	_fill_test_pattern((uint8_t *)data + plane_layouts[0].offset, plane_layouts[0].rowPitch, (uint8_t *)data + plane_layouts[1].offset, plane_layouts[1].rowPitch, p_width, p_height, p_bits > 8 ? 2 : 1);
	vkUnmapMemory(device, memory);
	print_line(vformat("DmabufImage: %s test pattern %dx%d in host memory for the YCbCr pass.", _vk_format_name(format), p_width, p_height));
	return _setup_ycbcr(device, image, memory, format, format_properties.linearTilingFeatures, p_width, p_height, DRM_FORMAT_MOD_LINEAR, false, p_out_y, p_out_cbcr);
}

static uint64_t _parse_hex(const String &p_text, bool &r_ok) {
	String text = p_text.strip_edges().to_lower();
	if (text.begins_with("0x")) {
		text = text.substr(2);
	}
	r_ok = !text.is_empty() && text.is_valid_hex_number(false);
	return r_ok ? (uint64_t)text.hex_to_int() : 0;
}

static Vector<DmabufImage::YCbCrCandidate> _q10c_candidates_copy() {
	Vector<DmabufImage::YCbCrCandidate> result;
	for (const DmabufImage::YCbCrCandidate &candidate : q10c_candidates) {
		result.push_back(candidate);
	}
	return result;
}

Vector<DmabufImage::YCbCrCandidate> DmabufImage::get_q10c_candidates() {
	MutexLock lock(q10c_mutex);
	if (q10c_looked) {
		return _q10c_candidates_copy();
	}
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (rd == nullptr || rd->get_device_api_name() != "Vulkan") {
		q10c_report = "No Vulkan RenderingDevice.";
		return Vector<YCbCrCandidate>();
	}
	q10c_looked = true;
	q10c_candidates.clear();
	const String enabled = OS::get_singleton()->get_environment("HWVIDEO_Q10C");
	if (enabled == "0") {
		q10c_report = "off (HWVIDEO_Q10C=0)";
		return _q10c_candidates_copy();
	}
	RenderingDeviceDriverVulkan *driver = static_cast<RenderingDeviceDriverVulkan *>(rd->get_device_driver());
	const char *required_extensions[] = {
		VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
		VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
		VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
	};
	for (const char *extension : required_extensions) {
		if (!driver->is_device_extension_enabled(extension)) {
			q10c_report = vformat("the Vulkan device has no %s", extension);
			return _q10c_candidates_copy();
		}
	}
	if (!driver->is_sampler_ycbcr_conversion_enabled()) {
		q10c_report = "the Vulkan device has no samplerYcbcrConversion";
		return _q10c_candidates_copy();
	}
	VkPhysicalDevice physical_device = (VkPhysicalDevice)rd->get_driver_resource(RD::DRIVER_RESOURCE_PHYSICAL_DEVICE);

	// Formats: HWVIDEO_Q10C_FORMAT (a VkFormat number, or G10X6, G12X4, G16), else the 10-bit 2-plane 4:2:0 format.
	LocalVector<uint32_t> formats;
	const String format_env = OS::get_singleton()->get_environment("HWVIDEO_Q10C_FORMAT").strip_edges().to_upper();
	if (format_env.is_valid_int()) {
		formats.push_back((uint32_t)format_env.to_int());
	} else if (format_env.begins_with("G16")) {
		formats.push_back(VK_FORMAT_G16_B16R16_2PLANE_420_UNORM);
	} else if (format_env.begins_with("G12")) {
		formats.push_back(VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16);
	} else {
		formats.push_back(VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16);
	}
	bool modifier_forced = false;
	const String modifier_env = OS::get_singleton()->get_environment("HWVIDEO_Q10C_MODIFIER");
	const uint64_t forced_modifier = modifier_env.is_empty() ? 0 : _parse_hex(modifier_env, modifier_forced);
	const String layout_env = OS::get_singleton()->get_environment("HWVIDEO_Q10C_LAYOUT").strip_edges().to_lower();
	// Preference among the QCOM modifiers a driver may list: Android's downstream TP10 UBWC (COMPRESSED|DX|TIGHT = 7) and its
	// relatives before plain COMPRESSED (1), which upstream Turnip lays out as P010 (16-bit samples), not TP10.
	static const uint64_t preference[] = { 7, 5, 4, 3, 6, 2, 1 };

	String report;
	for (uint32_t format : formats) {
		LocalVector<VkDrmFormatModifierPropertiesEXT> properties;
		_get_modifier_properties(physical_device, (VkFormat)format, properties);
		String listed;
		LocalVector<VkDrmFormatModifierPropertiesEXT> tries;
		for (const VkDrmFormatModifierPropertiesEXT &p : properties) {
			listed += (listed.is_empty() ? "" : ", ") + vformat("%s (%d plane(s))", _modifier_to_string(p.drmFormatModifier), p.drmFormatModifierPlaneCount);
			if (!modifier_forced && p.drmFormatModifier != DRM_FORMAT_MOD_LINEAR && (p.drmFormatModifierTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
				tries.push_back(p);
			}
		}
		if (modifier_forced) {
			VkDrmFormatModifierPropertiesEXT forced = {};
			forced.drmFormatModifier = forced_modifier;
			forced.drmFormatModifierPlaneCount = 2;
			for (const VkDrmFormatModifierPropertiesEXT &p : properties) {
				if (p.drmFormatModifier == forced_modifier) {
					forced = p;
				}
			}
			tries.push_back(forced);
		}
		const auto rank = [](uint64_t p_modifier) {
			if ((p_modifier >> 56) == DRM_FORMAT_MOD_VENDOR_QCOM) {
				for (uint32_t i = 0; i < std_size(preference); i++) {
					if ((p_modifier & 0x00ffffffffffffffull) == preference[i]) {
						return (int)i;
					}
				}
			}
			return (int)std_size(preference);
		};
		for (uint32_t i = 1; i < tries.size(); i++) { // A handful of entries.
			for (uint32_t j = i; j > 0 && rank(tries[j].drmFormatModifier) < rank(tries[j - 1].drmFormatModifier); j--) {
				SWAP(tries[j], tries[j - 1]);
			}
		}
		report += vformat("%s lists %s.", _vk_format_name(format), listed.is_empty() ? String("no modifiers") : listed);
		for (const VkDrmFormatModifierPropertiesEXT &p : tries) {
			VkExtent3D max_extent = {};
			bool importable = false;
			const VkResult err = _query_import(physical_device, (VkFormat)format, p.drmFormatModifier, nullptr, 0, max_extent, importable);
			if (err != VK_SUCCESS || !importable) {
				report += vformat(" %s refused for a dmabuf import (VkResult %d).", _modifier_to_string(p.drmFormatModifier), err);
				continue;
			}
			YCbCrCandidate candidate;
			candidate.format = format;
			candidate.modifier = p.drmFormatModifier;
			candidate.memory_planes = p.drmFormatModifierPlaneCount;
			if (layout_env == "meta" || layout_env == "data" || layout_env == "4plane" || layout_env == "1plane") {
				candidate.layout = layout_env;
				candidate.memory_planes = layout_env == "4plane" ? 4 : (layout_env == "1plane" ? 1 : 2);
			} else {
				candidate.layout = candidate.memory_planes == 4 ? "4plane" : (candidate.memory_planes == 1 ? "1plane" : "meta");
			}
			candidate.source = modifier_forced ? "HWVIDEO_Q10C_MODIFIER" : "listed by the driver";
			q10c_candidates.push_back(candidate);
		}
	}
	String tried;
	for (const YCbCrCandidate &candidate : q10c_candidates) {
		tried += (tried.is_empty() ? "" : "; ") + candidate.describe();
	}
	q10c_report = report + (q10c_candidates.is_empty() ? String(" No candidate.") : " Candidates: " + tried + ".");
	print_line("DmabufImage: Q10C through a YCbCr sampler: " + q10c_report);
	return _q10c_candidates_copy();
}

String DmabufImage::get_q10c_report() {
	MutexLock lock(q10c_mutex);
	return q10c_report;
}

static String q10c_disabled_reason;
static int q10c_chosen = -1;

bool DmabufImage::is_q10c_available() {
	if (!get_q10c_candidates().size()) {
		return false;
	}
	MutexLock lock(q10c_mutex);
	return q10c_disabled_reason.is_empty();
}

void DmabufImage::disable_q10c(const String &p_reason) {
	MutexLock lock(q10c_mutex);
	if (q10c_disabled_reason.is_empty()) {
		q10c_disabled_reason = p_reason;
		print_line("DmabufImage: Q10C through a YCbCr sampler is off for the rest of the run: " + p_reason);
	}
}

String DmabufImage::get_q10c_disabled_reason() {
	MutexLock lock(q10c_mutex);
	return q10c_disabled_reason;
}

int DmabufImage::get_q10c_chosen() {
	MutexLock lock(q10c_mutex);
	return q10c_chosen;
}

void DmabufImage::set_q10c_chosen(int p_index) {
	MutexLock lock(q10c_mutex);
	q10c_chosen = p_index;
}

Array DmabufImage::_get_q10c_candidates_bind() {
	Array result;
	for (const YCbCrCandidate &candidate : get_q10c_candidates()) {
		Dictionary d;
		d["format"] = candidate.format;
		d["format_name"] = _vk_format_name(candidate.format);
		d["modifier"] = (int64_t)candidate.modifier;
		d["memory_planes"] = candidate.memory_planes;
		d["layout"] = candidate.layout;
		d["source"] = candidate.source;
		d["description"] = candidate.describe();
		result.push_back(d);
	}
	return result;
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

	if (ycbcr) {
		// The pass, recorded in the graph: the out textures are written (the graph orders it before this frame's draws that
		// sample them, and after the earlier frames' reads), the dmabuf is taken from and given back to the producer inside it.
		RenderingDevice *rd = RenderingDevice::get_singleton();
		RD::CallbackResource resources[MAX_PLANES];
		for (uint32_t i = 0; i < MAX_PLANES; i++) {
			resources[i].rid = ycbcr->out[i];
			resources[i].type = RD::CALLBACK_RESOURCE_TYPE_TEXTURE;
			resources[i].usage = RD::CALLBACK_RESOURCE_USAGE_STORAGE_IMAGE_READ_WRITE;
		}
		const Error err = rd->driver_callback_add(&DmabufImage::_record_ycbcr, ycbcr, VectorView<RD::CallbackResource>(resources, MAX_PLANES));
		ERR_FAIL_COND_V(err != OK, err);
		ycbcr->conversions++;
		acquired = true;
		return OK;
	}

	Error err = _transfer(true);
	ERR_FAIL_COND_V(err != OK, err);
	acquired = true;
	return OK;
}

// One ownership transfer per VkImage: all planes at once, or each plane of a split import on its own.
Error DmabufImage::_transfer(bool p_acquire) {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (image_count == 1) {
		const VectorView<RID> aliases(&planes[1], plane_count - 1);
		return p_acquire ? rd->texture_external_acquire(planes[0], aliases) : rd->texture_external_release(planes[0], aliases);
	}
	for (uint32_t i = 0; i < plane_count; i++) {
		const Error err = p_acquire ? rd->texture_external_acquire(planes[i]) : rd->texture_external_release(planes[i]);
		ERR_FAIL_COND_V(err != OK, err);
	}
	return OK;
}

Error DmabufImage::release() {
	ERR_FAIL_COND_V_MSG(!is_imported(), ERR_UNCONFIGURED, "No image is imported.");
	if (!acquired) {
		return OK;
	}

	_process_pending_frees();

	if (ycbcr) {
		acquired = false; // The pass gave the dmabuf back already.
		return OK;
	}

	Error err = _transfer(false);
	ERR_FAIL_COND_V(err != OK, err);
	acquired = false;
	return OK;
}

void DmabufImage::free_image() {
	if (vk_images[0] == VK_NULL_HANDLE) {
		return;
	}

	// The RenderingDevice destroys the plane views once the frames that may use them have completed. The images and their
	// memory must outlive them, so they're destroyed a frame after that.
	RenderingDevice *rd = RenderingDevice::get_singleton();
	PendingFree pending;
	pending.device = vk_device;
	for (uint32_t i = 0; i < MAX_PLANES; i++) {
		pending.images[i] = vk_images[i];
		pending.memories[i] = vk_memories[i];
	}
	if (ycbcr) {
		// The planes are the caller's out textures. The view, descriptor set and the callback's state go with the image.
		pending.view = ycbcr->view;
		pending.pool = ycbcr->pool;
		pending.ycbcr = ycbcr;
		ycbcr = nullptr;
		ycbcr_description = String();
	} else if (rd != nullptr) {
		for (uint32_t i = 0; i < plane_count; i++) {
			if (planes[i].is_valid()) {
				rd->free_rid(planes[i]);
			}
		}
	}
	if (rd != nullptr) {
		pending.frame = rd->get_frames_drawn();
	}

	for (uint32_t i = 0; i < MAX_PLANES; i++) {
		planes[i] = RID();
	}
	vk_device = VK_NULL_HANDLE;
	for (uint32_t i = 0; i < MAX_PLANES; i++) {
		vk_images[i] = VK_NULL_HANDLE;
		vk_memories[i] = VK_NULL_HANDLE;
	}
	image_count = 0;
	plane_count = 0;
	size = Size2i();
	drm_modifier = 0;
	acquired = false;

	if (rd == nullptr) {
		// The device is gone, and the image with it.
		if (pending.ycbcr) {
			memdelete(pending.ycbcr);
		}
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
		if (pending.view != VK_NULL_HANDLE) {
			vkDestroyImageView(pending.device, pending.view, nullptr);
		}
		if (pending.pool != VK_NULL_HANDLE) {
			vkDestroyDescriptorPool(pending.device, pending.pool, nullptr);
		}
		if (pending.ycbcr) {
			memdelete(pending.ycbcr);
		}
		for (uint32_t j = 0; j < MAX_PLANES; j++) {
			if (pending.images[j] != VK_NULL_HANDLE) {
				vkDestroyImage(pending.device, pending.images[j], nullptr);
			}
		}
		for (uint32_t j = 0; j < MAX_PLANES; j++) {
			if (pending.memories[j] != VK_NULL_HANDLE) {
				vkFreeMemory(pending.device, pending.memories[j], nullptr);
			}
		}
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
	ClassDB::bind_method(D_METHOD("import_p010", "fd", "width", "height", "drm_modifier", "plane_offsets", "plane_pitches"), &DmabufImage::import_p010);
	ClassDB::bind_method(D_METHOD("import_planes", "fd", "format", "drm_modifier", "sizes", "offsets", "pitches", "mutable"), &DmabufImage::_import_planes_bind, DEFVAL(false));
	ClassDB::bind_method(D_METHOD("import_test_pattern", "width", "height", "device"), &DmabufImage::import_test_pattern, DEFVAL("/dev/dri/card0"));
	ClassDB::bind_method(D_METHOD("import_ycbcr", "fd", "vk_format", "drm_modifier", "width", "height", "offsets", "pitches", "out_y", "out_cbcr"), &DmabufImage::_import_ycbcr_bind);
	ClassDB::bind_method(D_METHOD("import_ycbcr_test_pattern", "width", "height", "out_y", "out_cbcr", "bits"), &DmabufImage::import_ycbcr_test_pattern, DEFVAL(10));
	ClassDB::bind_method(D_METHOD("is_ycbcr"), &DmabufImage::is_ycbcr);
	ClassDB::bind_method(D_METHOD("get_ycbcr_description"), &DmabufImage::get_ycbcr_description);
	ClassDB::bind_static_method("DmabufImage", D_METHOD("get_q10c_candidates"), &DmabufImage::_get_q10c_candidates_bind);
	ClassDB::bind_static_method("DmabufImage", D_METHOD("get_q10c_report"), &DmabufImage::get_q10c_report);
	ClassDB::bind_static_method("DmabufImage", D_METHOD("is_q10c_available"), &DmabufImage::is_q10c_available);
	ClassDB::bind_static_method("DmabufImage", D_METHOD("get_q10c_disabled_reason"), &DmabufImage::get_q10c_disabled_reason);
	ClassDB::bind_method(D_METHOD("free_image"), &DmabufImage::free_image);

	ClassDB::bind_method(D_METHOD("acquire"), &DmabufImage::acquire);
	ClassDB::bind_method(D_METHOD("release"), &DmabufImage::release);
	ClassDB::bind_method(D_METHOD("is_acquired"), &DmabufImage::is_acquired);

	ClassDB::bind_method(D_METHOD("is_imported"), &DmabufImage::is_imported);
	ClassDB::bind_method(D_METHOD("get_image_count"), &DmabufImage::get_image_count);
	ClassDB::bind_method(D_METHOD("get_plane_rid", "plane"), &DmabufImage::get_plane_rid);
	ClassDB::bind_method(D_METHOD("get_size"), &DmabufImage::get_size);
	ClassDB::bind_method(D_METHOD("get_drm_modifier"), &DmabufImage::get_drm_modifier);

	ClassDB::bind_static_method("DmabufImage", D_METHOD("get_supported_nv12_modifiers"), &DmabufImage::get_supported_nv12_modifiers);
}
