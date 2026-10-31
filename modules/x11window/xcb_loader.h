/**************************************************************************/
/*  xcb_loader.h                                                          */
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

// libxcb and its extension libraries, loaded at run time (dlopen) so the engine binary gets no new library dependencies
// and still runs where they are missing. The headers are the generated libxcb 1.15 ones in thirdparty/xcb/ (MIT); the
// functions used here are ABI-stable across libxcb releases.

#include "thirdparty/xcb/composite.h"
#include "thirdparty/xcb/damage.h"
#include "thirdparty/xcb/dri3.h"
#include "thirdparty/xcb/shm.h"
#include "thirdparty/xcb/xcb.h"
#include "thirdparty/xcb/xproto.h"
#include "thirdparty/xcb/xfixes.h"
#include "thirdparty/xcb/xtest.h"

#include "core/string/ustring.h"

// F(library, symbol). The library index refers to XCBLoader::LIB_*.
#define XCB_CORE_FUNCTIONS(F)                       \
	F(LIB_XCB, xcb_connect)                         \
	F(LIB_XCB, xcb_disconnect)                      \
	F(LIB_XCB, xcb_connection_has_error)            \
	F(LIB_XCB, xcb_get_setup)                       \
	F(LIB_XCB, xcb_setup_roots_iterator)            \
	F(LIB_XCB, xcb_flush)                           \
	F(LIB_XCB, xcb_generate_id)                     \
	F(LIB_XCB, xcb_poll_for_event)                  \
	F(LIB_XCB, xcb_get_extension_data)              \
	F(LIB_XCB, xcb_request_check)                   \
	F(LIB_XCB, xcb_intern_atom)                     \
	F(LIB_XCB, xcb_intern_atom_reply)               \
	F(LIB_XCB, xcb_query_tree)                      \
	F(LIB_XCB, xcb_query_tree_reply)                \
	F(LIB_XCB, xcb_query_tree_children)             \
	F(LIB_XCB, xcb_query_tree_children_length)      \
	F(LIB_XCB, xcb_get_window_attributes)           \
	F(LIB_XCB, xcb_get_window_attributes_reply)     \
	F(LIB_XCB, xcb_get_geometry)                    \
	F(LIB_XCB, xcb_get_geometry_reply)              \
	F(LIB_XCB, xcb_get_property)                    \
	F(LIB_XCB, xcb_get_property_reply)              \
	F(LIB_XCB, xcb_get_property_value)              \
	F(LIB_XCB, xcb_get_property_value_length)       \
	F(LIB_XCB, xcb_change_property)                 \
	F(LIB_XCB, xcb_change_property_checked)         \
	F(LIB_XCB, xcb_set_input_focus_checked)         \
	F(LIB_XCB, xcb_delete_property)                 \
	F(LIB_XCB, xcb_translate_coordinates)           \
	F(LIB_XCB, xcb_translate_coordinates_reply)     \
	F(LIB_XCB, xcb_change_window_attributes)        \
	F(LIB_XCB, xcb_change_window_attributes_checked) \
	F(LIB_XCB, xcb_send_event)                      \
	F(LIB_XCB, xcb_configure_window)                \
	F(LIB_XCB, xcb_set_input_focus)                 \
	F(LIB_XCB, xcb_get_input_focus)                 \
	F(LIB_XCB, xcb_get_input_focus_reply)           \
	F(LIB_XCB, xcb_map_window)                      \
	F(LIB_XCB, xcb_unmap_window)                    \
	F(LIB_XCB, xcb_create_window)                   \
	F(LIB_XCB, xcb_destroy_window)                  \
	F(LIB_XCB, xcb_free_pixmap)                     \
	F(LIB_XCB, xcb_get_image)                       \
	F(LIB_XCB, xcb_get_image_reply)                 \
	F(LIB_XCB, xcb_get_image_data)                  \
	F(LIB_XCB, xcb_get_image_data_length)           \
	F(LIB_XCB, xcb_get_keyboard_mapping)            \
	F(LIB_XCB, xcb_get_keyboard_mapping_reply)      \
	F(LIB_XCB, xcb_get_keyboard_mapping_keysyms)    \
	F(LIB_XCB, xcb_get_keyboard_mapping_keysyms_length) \
	F(LIB_XCB, xcb_change_keyboard_mapping_checked) \
	F(LIB_XCB, xcb_get_modifier_mapping)            \
	F(LIB_XCB, xcb_get_modifier_mapping_reply)      \
	F(LIB_XCB, xcb_get_modifier_mapping_keycodes)

#define XCB_EXTENSION_FUNCTIONS(F)                     \
	F(LIB_COMPOSITE, xcb_composite_query_version)       \
	F(LIB_COMPOSITE, xcb_composite_query_version_reply) \
	F(LIB_COMPOSITE, xcb_composite_redirect_window_checked) \
	F(LIB_COMPOSITE, xcb_composite_unredirect_window)   \
	F(LIB_COMPOSITE, xcb_composite_name_window_pixmap_checked) \
	F(LIB_DAMAGE, xcb_damage_query_version)             \
	F(LIB_DAMAGE, xcb_damage_query_version_reply)       \
	F(LIB_DAMAGE, xcb_damage_create_checked)            \
	F(LIB_DAMAGE, xcb_damage_destroy)                   \
	F(LIB_DAMAGE, xcb_damage_subtract)                  \
	F(LIB_XFIXES, xcb_xfixes_query_version)             \
	F(LIB_XFIXES, xcb_xfixes_query_version_reply)       \
	F(LIB_XFIXES, xcb_xfixes_create_region)             \
	F(LIB_XFIXES, xcb_xfixes_destroy_region)            \
	F(LIB_XFIXES, xcb_xfixes_set_window_shape_region_checked) \
	F(LIB_XTEST, xcb_test_get_version)                  \
	F(LIB_XTEST, xcb_test_get_version_reply)            \
	F(LIB_XTEST, xcb_test_fake_input)                   \
	F(LIB_DRI3, xcb_dri3_query_version)                 \
	F(LIB_DRI3, xcb_dri3_query_version_reply)           \
	F(LIB_DRI3, xcb_dri3_buffers_from_pixmap)           \
	F(LIB_DRI3, xcb_dri3_buffers_from_pixmap_reply)     \
	F(LIB_DRI3, xcb_dri3_buffers_from_pixmap_reply_fds) \
	F(LIB_DRI3, xcb_dri3_buffers_from_pixmap_strides)   \
	F(LIB_DRI3, xcb_dri3_buffers_from_pixmap_offsets)   \
	F(LIB_DRI3, xcb_dri3_buffer_from_pixmap)            \
	F(LIB_DRI3, xcb_dri3_buffer_from_pixmap_reply)      \
	F(LIB_DRI3, xcb_dri3_buffer_from_pixmap_reply_fds)  \
	F(LIB_SHM, xcb_shm_query_version)                   \
	F(LIB_SHM, xcb_shm_query_version_reply)             \
	F(LIB_SHM, xcb_shm_attach_checked)                  \
	F(LIB_SHM, xcb_shm_attach_fd_checked)               \
	F(LIB_SHM, xcb_shm_detach)                          \
	F(LIB_SHM, xcb_shm_get_image)                       \
	F(LIB_SHM, xcb_shm_get_image_reply)

#define XCB_EXTENSION_IDS(F)             \
	F(LIB_COMPOSITE, xcb_composite_id) \
	F(LIB_DAMAGE, xcb_damage_id)       \
	F(LIB_XFIXES, xcb_xfixes_id)       \
	F(LIB_XTEST, xcb_test_id)          \
	F(LIB_DRI3, xcb_dri3_id)           \
	F(LIB_SHM, xcb_shm_id)

struct XCBLoader {
	enum Library {
		LIB_XCB,
		LIB_COMPOSITE,
		LIB_DAMAGE,
		LIB_XFIXES,
		LIB_XTEST,
		LIB_DRI3,
		LIB_SHM,
		LIB_MAX,
	};

	void *handles[LIB_MAX] = {};
	bool library_loaded[LIB_MAX] = {};

#define XCB_DECLARE_FUNCTION(m_lib, m_name) decltype(&::m_name) m_name = nullptr;
	XCB_CORE_FUNCTIONS(XCB_DECLARE_FUNCTION)
	XCB_EXTENSION_FUNCTIONS(XCB_DECLARE_FUNCTION)
#undef XCB_DECLARE_FUNCTION

#define XCB_DECLARE_ID(m_lib, m_name) xcb_extension_t *m_name = nullptr;
	XCB_EXTENSION_IDS(XCB_DECLARE_ID)
#undef XCB_DECLARE_ID

	// Loads every library that exists. Fails only if libxcb itself is missing; an extension library that is missing (or
	// lacks a symbol) leaves has_library() false for it.
	Error load(String &r_error);
	bool has_library(Library p_library) const { return library_loaded[p_library]; }

	// Shared by all X11WindowSource instances, loaded on first use, never unloaded.
	static XCBLoader *get_singleton(String &r_error);
};
