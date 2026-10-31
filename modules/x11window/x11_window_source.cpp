/**************************************************************************/
/*  x11_window_source.cpp                                                 */
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

#include "x11_window_source.h"

#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "servers/display/display_server.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_server.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>

// Distinct buffers kept imported per window. Xwayland swaps a window between 2-3 buffers; a resize brings new ones.
static constexpr uint32_t MAX_CACHED_BUFFERS = 6;
// Frames a changed buffer may be skipped while the GPU is still writing it, before it is shown anyway.
static constexpr int MAX_DEFERRED_FRAMES = 3;

static constexpr uint16_t SHIFT_MASK = XCB_MOD_MASK_SHIFT;

/* X11WindowTexture */

void X11WindowTexture::init() {
	ERR_FAIL_COND(proxy.is_valid());
	Ref<Image> image = Image::create_empty(2, 2, false, Image::FORMAT_RGBA8);
	image->fill(Color(0, 0, 0, 1));
	placeholder = RS::get_singleton()->texture_2d_create(image);
	proxy = RS::get_singleton()->texture_proxy_create(placeholder);
	size = Size2i(2, 2);
}

void X11WindowTexture::set_source(RID p_texture, const Size2i &p_size, bool p_alpha) {
	ERR_FAIL_COND(proxy.is_null());
	RS::get_singleton()->texture_proxy_update(proxy, p_texture.is_valid() ? p_texture : placeholder);
	const Size2i new_size = p_texture.is_valid() ? p_size : Size2i(2, 2);
	alpha = p_texture.is_valid() && p_alpha;
	if (new_size != size) {
		size = new_size;
		emit_changed();
	}
}

X11WindowTexture::~X11WindowTexture() {
	if (RS::get_singleton() != nullptr) {
		if (proxy.is_valid()) {
			RS::get_singleton()->free_rid(proxy);
		}
		if (placeholder.is_valid()) {
			RS::get_singleton()->free_rid(placeholder);
		}
	}
}

/* Helpers */

template <typename T>
struct XCBReply {
	T *reply = nullptr;
	XCBReply(T *p_reply) :
			reply(p_reply) {}
	~XCBReply() { free(reply); }
	T *operator->() const { return reply; }
	explicit operator bool() const { return reply != nullptr; }
	XCBReply(const XCBReply &) = delete;
	XCBReply &operator=(const XCBReply &) = delete;
};

static int _version(int p_major, int p_minor) {
	return p_major * 100 + p_minor;
}

static String _version_string(int p_version) {
	return p_version == 0 ? String("missing") : vformat("%d.%d", p_version / 100, p_version % 100);
}

static uint32_t _unicode_to_keysym(char32_t p_char) {
	switch (p_char) {
		case '\n':
		case '\r':
			return 0xff0d; // Return
		case '\t':
			return 0xff09; // Tab
		case '\b':
			return 0xff08; // BackSpace
		case 0x1b:
			return 0xff1b; // Escape
		case 0x7f:
			return 0xffff; // Delete
		default:
			break;
	}
	if ((p_char >= 0x20 && p_char <= 0x7e) || (p_char >= 0xa0 && p_char <= 0xff)) {
		return p_char; // Latin-1 keysyms are the code points.
	}
	return 0x01000000 | p_char; // Unicode keysyms.
}

/* Render thread */

void X11WindowSource::_render_flush() {
	MutexLock lock(render_mutex);
	RenderingDevice *rd = RenderingDevice::get_singleton();
	for (RenderOp &op : render_ops) {
		switch (op.type) {
			case RenderOp::ACQUIRE:
				if (op.image.is_valid() && op.image->is_imported()) {
					op.image->acquire();
				}
				break;
			case RenderOp::RELEASE:
				if (op.image.is_valid() && op.image->is_imported()) {
					op.image->release();
				}
				break;
			case RenderOp::FREE:
				if (op.image.is_valid()) {
					if (op.image->is_imported()) {
						op.image->release();
					}
					op.image->free_image();
				}
				break;
			case RenderOp::UPLOAD:
				if (rd != nullptr && op.rd_texture.is_valid() && rd->texture_is_valid(op.rd_texture)) {
					rd->texture_update(op.rd_texture, 0, op.data);
				}
				break;
		}
	}
	render_ops.clear(); // Keeps the capacity.
}

void X11WindowSource::_render_free_images(const Array &p_images) {
	for (const Variant &v : p_images) {
		Ref<X11DmabufImage> image = v;
		if (image.is_valid()) {
			if (image->is_imported()) {
				image->release();
			}
			image->free_image();
		}
	}
}

void X11WindowSource::_queue_render_op(RenderOp::Type p_type, const Ref<X11DmabufImage> &p_image, RID p_rd_texture, const Vector<uint8_t> &p_data) {
	MutexLock lock(render_mutex);
	RenderOp op;
	op.type = p_type;
	op.image = p_image;
	op.rd_texture = p_rd_texture;
	op.data = p_data;
	render_ops.push_back(op);
}

void X11WindowSource::_kick_render_ops() {
	{
		MutexLock lock(render_mutex);
		if (render_ops.is_empty()) {
			return;
		}
	}
	RenderingServer *rs = RS::get_singleton();
	if (rs == nullptr) {
		return;
	}
	if (rs->is_on_render_thread()) {
		_render_flush();
	} else {
		rs->call_on_render_thread(render_callable);
	}
}

/* X helpers */

xcb_atom_t X11WindowSource::_atom(const String &p_name, bool p_create) {
	const xcb_atom_t *cached = atoms.getptr(p_name);
	if (cached) {
		return *cached;
	}
	ERR_FAIL_NULL_V(connection, XCB_ATOM_NONE);
	CharString name = p_name.utf8();
	XCBReply<xcb_intern_atom_reply_t> reply(xcb->xcb_intern_atom_reply(connection, xcb->xcb_intern_atom(connection, !p_create, name.length(), name.get_data()), nullptr));
	const xcb_atom_t atom = reply ? reply->atom : (xcb_atom_t)XCB_ATOM_NONE;
	if (atom != XCB_ATOM_NONE) {
		atoms[p_name] = atom;
	}
	return atom;
}

String X11WindowSource::_atom_name_cached(xcb_atom_t p_atom) const {
	for (const KeyValue<String, xcb_atom_t> &kv : atoms) {
		if (kv.value == p_atom) {
			return kv.key;
		}
	}
	return itos(p_atom);
}

void X11WindowSource::_error_event(const xcb_generic_error_t *p_error) {
	x_errors++;
	last_x_error = vformat("X error %d (request %d.%d, resource 0x%x)", p_error->error_code, p_error->major_code, p_error->minor_code, (int64_t)p_error->resource_id);
	print_verbose("X11WindowSource: " + last_x_error);
}

bool X11WindowSource::_check(xcb_void_cookie_t p_cookie, const char *p_what) {
	xcb_generic_error_t *error = xcb->xcb_request_check(connection, p_cookie);
	if (error == nullptr) {
		return true;
	}
	_error_event(error);
	last_error = vformat("%s failed: %s.", p_what, last_x_error);
	free(error);
	return false;
}

uint32_t X11WindowSource::_server_time() {
	if (server_time_ticks == 0) {
		return XCB_CURRENT_TIME;
	}
	return server_time_base + uint32_t(OS::get_singleton()->get_ticks_msec() - server_time_ticks);
}

void X11WindowSource::_sync_server_time() {
	// A zero-length append to a property of our own window: the PropertyNotify carries the server's time.
	const xcb_atom_t atom = _atom("_GODOT_X11WINDOW_TIME");
	xcb->xcb_change_property(connection, XCB_PROP_MODE_APPEND, helper_window, atom, XCB_ATOM_STRING, 8, 0, nullptr);
	xcb->xcb_flush(connection);
	const uint64_t deadline = OS::get_singleton()->get_ticks_msec() + 250;
	while (OS::get_singleton()->get_ticks_msec() < deadline && server_time_ticks == 0) {
		xcb_generic_event_t *event = xcb->xcb_poll_for_event(connection);
		if (event == nullptr) {
			OS::get_singleton()->delay_usec(1000);
			continue;
		}
		_handle_event(event);
		free(event);
	}
	if (server_time_ticks == 0) {
		print_line("X11WindowSource: no server timestamp; synthetic events use CurrentTime.");
	}
}

void X11WindowSource::_load_keyboard_mapping() {
	keysym_map.clear();
	scratch_keycode = 0;
	keysyms_per_keycode = 0;
	const int count = int(max_keycode) - int(min_keycode) + 1;
	XCBReply<xcb_get_keyboard_mapping_reply_t> reply(xcb->xcb_get_keyboard_mapping_reply(connection, xcb->xcb_get_keyboard_mapping(connection, min_keycode, count), nullptr));
	if (!reply) {
		return;
	}
	const int per_keycode = reply->keysyms_per_keycode;
	const xcb_keysym_t *syms = xcb->xcb_get_keyboard_mapping_keysyms(reply.reply);
	const int length = xcb->xcb_get_keyboard_mapping_keysyms_length(reply.reply);
	keysyms_per_keycode = per_keycode;
	// Unshifted levels first, so a keysym on two keys maps to the one that needs no Shift.
	for (int level = 0; level < MIN(per_keycode, 2); level++) {
		for (int k = 0; k < count && (k + 1) * per_keycode <= length; k++) {
			const xcb_keysym_t sym = syms[k * per_keycode + level];
			if (sym != 0 && !keysym_map.has(sym)) {
				KeyCode code;
				code.keycode = uint8_t(min_keycode + k);
				code.shift = level == 1;
				keysym_map[sym] = code;
			}
		}
	}
	// A keycode with no symbols at all, to map unknown keysyms on demand (what xdotool does).
	for (int k = count - 1; k >= 0 && (k + 1) * per_keycode <= length; k--) {
		bool empty = true;
		for (int level = 0; level < per_keycode; level++) {
			empty = empty && syms[k * per_keycode + level] == 0;
		}
		if (empty) {
			scratch_keycode = uint8_t(min_keycode + k);
			break;
		}
	}
	if (scratch_keysym != 0 && scratch_keycode != 0) {
		KeyCode code;
		code.keycode = scratch_keycode;
		keysym_map[scratch_keysym] = code;
	}

	shift_keycode = 0;
	XCBReply<xcb_get_modifier_mapping_reply_t> modifiers(xcb->xcb_get_modifier_mapping_reply(connection, xcb->xcb_get_modifier_mapping(connection), nullptr));
	if (modifiers) {
		const xcb_keycode_t *keycodes = xcb->xcb_get_modifier_mapping_keycodes(modifiers.reply);
		for (int i = 0; i < modifiers->keycodes_per_modifier; i++) {
			if (keycodes[i] != 0) { // Row 0 is Shift.
				shift_keycode = keycodes[i];
				break;
			}
		}
	}
}

bool X11WindowSource::_keysym_to_keycode(uint32_t p_keysym, KeyCode &r_code) {
	const KeyCode *code = keysym_map.getptr(p_keysym);
	if (code) {
		r_code = *code;
		return true;
	}
	// Core keymaps often list only the lowercase letter; the uppercase one is its Shift level.
	if (p_keysym >= 'A' && p_keysym <= 'Z') {
		code = keysym_map.getptr(p_keysym + 32);
		if (code) {
			r_code = *code;
			r_code.shift = true;
			return true;
		}
	}
	if (scratch_keycode == 0 || keysyms_per_keycode <= 0) {
		return false;
	}
	// Put the keysym on the spare keycode, on every level. MappingNotify tells every client before our key event arrives.
	LocalVector<xcb_keysym_t> syms;
	syms.resize(keysyms_per_keycode);
	for (xcb_keysym_t &s : syms) {
		s = p_keysym;
	}
	if (!_check(xcb->xcb_change_keyboard_mapping_checked(connection, 1, scratch_keycode, keysyms_per_keycode, syms.ptr()), "ChangeKeyboardMapping")) {
		return false;
	}
	if (scratch_keysym != 0) {
		keysym_map.erase(scratch_keysym);
	}
	scratch_keysym = p_keysym;
	KeyCode scratch;
	scratch.keycode = scratch_keycode;
	keysym_map[p_keysym] = scratch;
	r_code = scratch;
	return true;
}

String X11WindowSource::_get_string_property(xcb_window_t p_window, xcb_atom_t p_property) {
	if (p_property == XCB_ATOM_NONE) {
		return String();
	}
	XCBReply<xcb_get_property_reply_t> reply(xcb->xcb_get_property_reply(connection, xcb->xcb_get_property(connection, 0, p_window, p_property, XCB_GET_PROPERTY_TYPE_ANY, 0, 1024), nullptr));
	if (!reply || reply->type == XCB_ATOM_NONE || reply->format != 8) {
		return String();
	}
	const char *data = (const char *)xcb->xcb_get_property_value(reply.reply);
	const int length = xcb->xcb_get_property_value_length(reply.reply);
	if (reply->type == XCB_ATOM_STRING) {
		return String::latin1(Span<char>(data, length));
	}
	return String::utf8(data, length);
}

xcb_window_t X11WindowSource::_find_client(xcb_window_t p_window, int p_depth) {
	const xcb_atom_t wm_state = _atom("WM_STATE");
	XCBReply<xcb_get_property_reply_t> state(xcb->xcb_get_property_reply(connection, xcb->xcb_get_property(connection, 0, p_window, wm_state, XCB_GET_PROPERTY_TYPE_ANY, 0, 0), nullptr));
	if (state && state->type != XCB_ATOM_NONE) {
		return p_window;
	}
	if (p_depth <= 0) {
		return 0;
	}
	XCBReply<xcb_query_tree_reply_t> tree(xcb->xcb_query_tree_reply(connection, xcb->xcb_query_tree(connection, p_window), nullptr));
	if (!tree) {
		return 0;
	}
	const xcb_window_t *children = xcb->xcb_query_tree_children(tree.reply);
	for (int i = xcb->xcb_query_tree_children_length(tree.reply) - 1; i >= 0; i--) {
		const xcb_window_t client = _find_client(children[i], p_depth - 1);
		if (client != 0) {
			return client;
		}
	}
	return 0;
}

xcb_window_t X11WindowSource::_toplevel_of(xcb_window_t p_window) {
	xcb_window_t current = p_window;
	for (int i = 0; i < 16 && current != 0; i++) {
		XCBReply<xcb_query_tree_reply_t> tree(xcb->xcb_query_tree_reply(connection, xcb->xcb_query_tree(connection, current), nullptr));
		if (!tree) {
			return 0;
		}
		if (tree->parent == root || tree->parent == 0) {
			return current;
		}
		current = tree->parent;
	}
	return current;
}

Dictionary X11WindowSource::_describe_window(xcb_window_t p_frame, xcb_window_t p_client) {
	Dictionary info;
	info["id"] = (int64_t)p_client;
	info["frame"] = (int64_t)p_frame;

	xcb_get_geometry_cookie_t geometry_cookie = xcb->xcb_get_geometry(connection, p_client);
	xcb_translate_coordinates_cookie_t position_cookie = xcb->xcb_translate_coordinates(connection, p_client, root, 0, 0);
	xcb_get_window_attributes_cookie_t attributes_cookie = xcb->xcb_get_window_attributes(connection, p_frame);
	XCBReply<xcb_get_geometry_reply_t> geometry(xcb->xcb_get_geometry_reply(connection, geometry_cookie, nullptr));
	XCBReply<xcb_translate_coordinates_reply_t> position(xcb->xcb_translate_coordinates_reply(connection, position_cookie, nullptr));
	XCBReply<xcb_get_window_attributes_reply_t> attributes(xcb->xcb_get_window_attributes_reply(connection, attributes_cookie, nullptr));
	info["width"] = geometry ? (int)geometry->width : 0;
	info["height"] = geometry ? (int)geometry->height : 0;
	info["depth"] = geometry ? (int)geometry->depth : 0;
	info["x"] = position ? (int)position->dst_x : 0;
	info["y"] = position ? (int)position->dst_y : 0;
	info["mapped"] = attributes && attributes->map_state == XCB_MAP_STATE_VIEWABLE;
	info["override_redirect"] = attributes && attributes->override_redirect;

	String title = _get_string_property(p_client, _atom("_NET_WM_NAME"));
	if (title.is_empty()) {
		title = _get_string_property(p_client, XCB_ATOM_WM_NAME);
	}
	info["title"] = title;

	String instance;
	String window_class;
	{
		XCBReply<xcb_get_property_reply_t> reply(xcb->xcb_get_property_reply(connection, xcb->xcb_get_property(connection, 0, p_client, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 0, 256), nullptr));
		if (reply && reply->format == 8) {
			const char *data = (const char *)xcb->xcb_get_property_value(reply.reply);
			const int length = xcb->xcb_get_property_value_length(reply.reply);
			const int first = strnlen(data, length);
			instance = String::utf8(data, first);
			if (first + 1 < length) {
				window_class = String::utf8(data + first + 1, strnlen(data + first + 1, length - first - 1));
			}
		}
	}
	info["instance"] = instance;
	info["class"] = window_class;

	Variant pid = get_window_property(p_client, "_NET_WM_PID");
	info["pid"] = pid.get_type() == Variant::PACKED_INT64_ARRAY && !PackedInt64Array(pid).is_empty() ? PackedInt64Array(pid)[0] : int64_t(0);
	Variant game = get_window_property(p_frame, "STEAM_GAME");
	info["steam_game"] = game.get_type() == Variant::PACKED_INT64_ARRAY && !PackedInt64Array(game).is_empty() ? PackedInt64Array(game)[0] : int64_t(0);
	return info;
}

void X11WindowSource::_update_root_position() {
	if (window == 0) {
		return;
	}
	XCBReply<xcb_translate_coordinates_reply_t> position(xcb->xcb_translate_coordinates_reply(connection, xcb->xcb_translate_coordinates(connection, window, root, 0, 0), nullptr));
	if (position) {
		root_position = Point2i(position->dst_x, position->dst_y);
	}
}

/* Connection */

Error X11WindowSource::open(const String &p_display) {
	close();
	String error;
	xcb = XCBLoader::get_singleton(error);
	if (xcb == nullptr) {
		last_error = error;
		return ERR_CANT_OPEN;
	}
	display_name = p_display.is_empty() ? OS::get_singleton()->get_environment("DISPLAY") : p_display;
	CharString name = display_name.utf8();
	int screen_index = 0;
	connection = xcb->xcb_connect(display_name.is_empty() ? nullptr : name.get_data(), &screen_index);
	if (connection == nullptr || xcb->xcb_connection_has_error(connection)) {
		if (connection) {
			xcb->xcb_disconnect(connection);
		}
		connection = nullptr;
		last_error = vformat("Can't connect to the X display \"%s\".", display_name);
		return ERR_CANT_CONNECT;
	}
	const xcb_setup_t *setup = xcb->xcb_get_setup(connection);
	xcb_screen_iterator_t screens = xcb->xcb_setup_roots_iterator(setup);
	if (screen_index != 0) {
		WARN_PRINT(vformat("X11WindowSource: screen %d of \"%s\"; using the first screen.", screen_index, display_name));
	}
	root = screens.data->root;
	min_keycode = setup->min_keycode;
	max_keycode = setup->max_keycode;

	// Extensions: present, then version. XFixes first (Damage relies on it).
	const xcb_query_extension_reply_t *ext = nullptr;
	if (xcb->has_library(XCBLoader::LIB_XFIXES) && (ext = xcb->xcb_get_extension_data(connection, xcb->xcb_xfixes_id)) && ext->present) {
		XCBReply<xcb_xfixes_query_version_reply_t> v(xcb->xcb_xfixes_query_version_reply(connection, xcb->xcb_xfixes_query_version(connection, 5, 0), nullptr));
		xfixes_version = v ? _version(v->major_version, v->minor_version) : 0;
		xfixes_event_base = ext->first_event;
	}
	if (xcb->has_library(XCBLoader::LIB_COMPOSITE) && (ext = xcb->xcb_get_extension_data(connection, xcb->xcb_composite_id)) && ext->present) {
		XCBReply<xcb_composite_query_version_reply_t> v(xcb->xcb_composite_query_version_reply(connection, xcb->xcb_composite_query_version(connection, 0, 4), nullptr));
		composite_version = v ? _version(v->major_version, v->minor_version) : 0;
	}
	if (xfixes_version > 0 && xcb->has_library(XCBLoader::LIB_DAMAGE) && (ext = xcb->xcb_get_extension_data(connection, xcb->xcb_damage_id)) && ext->present) {
		XCBReply<xcb_damage_query_version_reply_t> v(xcb->xcb_damage_query_version_reply(connection, xcb->xcb_damage_query_version(connection, 1, 1), nullptr));
		damage_version = v ? _version(v->major_version, v->minor_version) : 0;
		damage_event_base = ext->first_event;
	}
	if (xcb->has_library(XCBLoader::LIB_XTEST) && (ext = xcb->xcb_get_extension_data(connection, xcb->xcb_test_id)) && ext->present) {
		XCBReply<xcb_test_get_version_reply_t> v(xcb->xcb_test_get_version_reply(connection, xcb->xcb_test_get_version(connection, 2, 2), nullptr));
		xtest_version = v ? _version(v->major_version, v->minor_version) : 0;
	}
	if (xcb->has_library(XCBLoader::LIB_DRI3) && (ext = xcb->xcb_get_extension_data(connection, xcb->xcb_dri3_id)) && ext->present) {
		XCBReply<xcb_dri3_query_version_reply_t> v(xcb->xcb_dri3_query_version_reply(connection, xcb->xcb_dri3_query_version(connection, 1, 2), nullptr));
		dri3_version = v ? _version(v->major_version, v->minor_version) : 0;
	}
	if (xcb->has_library(XCBLoader::LIB_SHM) && (ext = xcb->xcb_get_extension_data(connection, xcb->xcb_shm_id)) && ext->present) {
		XCBReply<xcb_shm_query_version_reply_t> v(xcb->xcb_shm_query_version_reply(connection, xcb->xcb_shm_query_version(connection), nullptr));
		shm_version = v ? _version(v->major_version, v->minor_version) : 0;
		shm_fd_passing = shm_version >= _version(1, 2);
	}

	// An unmapped input-only window of our own, for server timestamps.
	helper_window = xcb->xcb_generate_id(connection);
	const uint32_t helper_mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
	xcb->xcb_create_window(connection, 0, helper_window, root, -1, -1, 1, 1, 0, XCB_WINDOW_CLASS_INPUT_ONLY, XCB_COPY_FROM_PARENT, XCB_CW_EVENT_MASK, &helper_mask);
	_sync_server_time();
	_load_keyboard_mapping();

	print_line(vformat("X11WindowSource: connected to \"%s\": Composite %s, DAMAGE %s, XFixes %s, XTEST %s, DRI3 %s, MIT-SHM %s%s.", display_name,
			_version_string(composite_version), _version_string(damage_version), _version_string(xfixes_version), _version_string(xtest_version),
			_version_string(dri3_version), _version_string(shm_version), shm_fd_passing ? " (fd passing)" : ""));
	return OK;
}

void X11WindowSource::close() {
	stop_capture();
	if (connection == nullptr) {
		return;
	}
	gamescope_release_focus();
	if (scratch_keysym != 0 && scratch_keycode != 0 && keysyms_per_keycode > 0) {
		LocalVector<xcb_keysym_t> syms;
		syms.resize(keysyms_per_keycode);
		for (xcb_keysym_t &s : syms) {
			s = 0;
		}
		xcb->xcb_change_keyboard_mapping_checked(connection, 1, scratch_keycode, keysyms_per_keycode, syms.ptr());
		scratch_keysym = 0;
	}
	_free_shm();
	if (helper_window != 0) {
		xcb->xcb_destroy_window(connection, helper_window);
		helper_window = 0;
	}
	xcb->xcb_flush(connection);
	xcb->xcb_disconnect(connection);
	connection = nullptr;
	atoms.clear();
	keysym_map.clear();
	server_time_ticks = 0;
	composite_version = damage_version = xfixes_version = xtest_version = dri3_version = shm_version = 0;
	shm_fd_passing = false;
}

Dictionary X11WindowSource::get_extensions() const {
	Dictionary result;
	result["display"] = display_name;
	result["connected"] = connection != nullptr;
	result["composite"] = _version_string(composite_version);
	result["damage"] = _version_string(damage_version);
	result["xfixes"] = _version_string(xfixes_version);
	result["xtest"] = _version_string(xtest_version);
	result["dri3"] = _version_string(dri3_version);
	result["shm"] = _version_string(shm_version);
	result["shm_fd_passing"] = shm_fd_passing;
	return result;
}

TypedArray<Dictionary> X11WindowSource::list_windows(bool p_include_unmapped) {
	TypedArray<Dictionary> result;
	ERR_FAIL_NULL_V_MSG(connection, result, "Call open() first.");
	XCBReply<xcb_query_tree_reply_t> tree(xcb->xcb_query_tree_reply(connection, xcb->xcb_query_tree(connection, root), nullptr));
	if (!tree) {
		return result;
	}
	const xcb_window_t *children = xcb->xcb_query_tree_children(tree.reply);
	const int count = xcb->xcb_query_tree_children_length(tree.reply);
	LocalVector<xcb_get_window_attributes_cookie_t> cookies;
	cookies.resize(count);
	for (int i = 0; i < count; i++) {
		cookies[i] = xcb->xcb_get_window_attributes(connection, children[i]);
	}
	LocalVector<xcb_window_t> frames;
	for (int i = 0; i < count; i++) {
		XCBReply<xcb_get_window_attributes_reply_t> attributes(xcb->xcb_get_window_attributes_reply(connection, cookies[i], nullptr));
		if (!attributes || attributes->_class != XCB_WINDOW_CLASS_INPUT_OUTPUT) {
			continue;
		}
		if (attributes->map_state != XCB_MAP_STATE_VIEWABLE && !p_include_unmapped) {
			continue;
		}
		frames.push_back(children[i]);
	}
	for (xcb_window_t frame : frames) {
		if (frame == helper_window) {
			continue;
		}
		xcb_window_t client = _find_client(frame, 2);
		if (client == 0) {
			client = frame; // No WM_STATE (no window manager, or an override-redirect window).
		}
		Dictionary info = _describe_window(frame, client);
		if (int(info["width"]) <= 1 && int(info["height"]) <= 1) {
			continue;
		}
		result.push_back(info);
	}
	return result;
}

Dictionary X11WindowSource::get_window_info(int64_t p_window) {
	ERR_FAIL_NULL_V_MSG(connection, Dictionary(), "Call open() first.");
	const xcb_window_t toplevel = _toplevel_of((xcb_window_t)p_window);
	if (toplevel == 0) {
		return Dictionary();
	}
	return _describe_window(toplevel, (xcb_window_t)p_window);
}

/* Capture */

Error X11WindowSource::capture(int64_t p_window) {
	ERR_FAIL_NULL_V_MSG(connection, ERR_UNCONFIGURED, "Call open() first.");
	stop_capture();
	const xcb_window_t target = (xcb_window_t)p_window;

	xcb_get_window_attributes_cookie_t attributes_cookie = xcb->xcb_get_window_attributes(connection, target);
	xcb_get_geometry_cookie_t geometry_cookie = xcb->xcb_get_geometry(connection, target);
	XCBReply<xcb_get_window_attributes_reply_t> attributes(xcb->xcb_get_window_attributes_reply(connection, attributes_cookie, nullptr));
	XCBReply<xcb_get_geometry_reply_t> geometry(xcb->xcb_get_geometry_reply(connection, geometry_cookie, nullptr));
	if (!attributes || !geometry) {
		last_error = vformat("Window 0x%x doesn't exist.", p_window);
		return ERR_DOES_NOT_EXIST;
	}
	if (attributes->_class != XCB_WINDOW_CLASS_INPUT_OUTPUT) {
		last_error = "An input-only window has no picture.";
		return ERR_INVALID_PARAMETER;
	}

	window = target;
	depth = geometry->depth;
	size = Size2i(geometry->width, geometry->height);
	mapped = attributes->map_state == XCB_MAP_STATE_VIEWABLE;

	const uint32_t event_mask = XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_PROPERTY_CHANGE;
	if (!_check(xcb->xcb_change_window_attributes_checked(connection, window, XCB_CW_EVENT_MASK, &event_mask), "Selecting the window's events")) {
		window = 0;
		return ERR_CANT_ACQUIRE_RESOURCE;
	}
	// Our own (automatic) redirect keeps the window off-screen-rendered, and its pixmap valid, even without a compositor.
	// Another client's manual redirect (a compositing manager, Xwayland's own) coexists with it.
	if (composite_version >= _version(0, 2)) {
		redirected = _check(xcb->xcb_composite_redirect_window_checked(connection, window, XCB_COMPOSITE_REDIRECT_AUTOMATIC), "RedirectWindow");
	}
	if (damage_version > 0) {
		damage = xcb->xcb_generate_id(connection);
		if (!_check(xcb->xcb_damage_create_checked(connection, damage, window, XCB_DAMAGE_REPORT_LEVEL_NON_EMPTY), "DamageCreate")) {
			damage = 0;
		}
	}
	if (damage == 0) {
		WARN_PRINT("X11WindowSource: no DAMAGE: the window is read every frame.");
	}
	_update_root_position();

	dmabuf_error = String();
	if (preferred_mode == CAPTURE_DMABUF && dri3_version > 0 && composite_version >= _version(0, 2)) {
		mode = CAPTURE_DMABUF;
	} else {
		if (preferred_mode == CAPTURE_DMABUF) {
			dmabuf_error = dri3_version == 0 ? "No DRI3 extension." : "No Composite 0.2.";
		}
		mode = (shm_version > 0 && preferred_mode != CAPTURE_GET_IMAGE) ? CAPTURE_SHM : CAPTURE_GET_IMAGE;
	}
	dirty = true;
	deferred_frames = 0;
	pointer_window = 0;
	button_state = 0;
	scroll_remainder = Vector2();
	xcb->xcb_flush(connection);

	Dictionary info = _describe_window(_toplevel_of(window), window);
	print_line(vformat("X11WindowSource: capturing 0x%x \"%s\" (%s), %dx%d, depth %d, %s; %s.", (int64_t)window, info["title"], info["class"], size.width, size.height, depth,
			mode == CAPTURE_DMABUF ? "dmabuf (zero copy)" : (mode == CAPTURE_SHM ? "MIT-SHM copy" : "GetImage copy"), mapped ? "mapped" : "not mapped yet"));
	return OK;
}

void X11WindowSource::stop_capture() {
	if (window == 0) {
		return;
	}
	if (connection != nullptr && !xcb->xcb_connection_has_error(connection)) {
		if (damage != 0) {
			xcb->xcb_damage_destroy(connection, damage);
		}
		if (redirected) {
			xcb->xcb_composite_unredirect_window(connection, window, XCB_COMPOSITE_REDIRECT_AUTOMATIC);
		}
		if (!window_destroyed) {
			const uint32_t no_events = 0;
			xcb->xcb_change_window_attributes(connection, window, XCB_CW_EVENT_MASK, &no_events);
		}
		xcb->xcb_flush(connection);
	}
	if (texture.is_valid()) {
		texture->set_source(RID(), Size2i(), false);
	}
	_free_buffers();
	_free_upload_texture();
	window = 0;
	damage = 0;
	redirected = false;
	mapped = false;
	window_destroyed = false;
	dirty = false;
	mode = CAPTURE_NONE;
	size = Size2i();
	depth = 0;
	pointer_window = 0;
	button_state = 0;
}

void X11WindowSource::_handle_event(xcb_generic_event_t *p_event) {
	const uint8_t type = p_event->response_type & 0x7f;
	if (type == 0) {
		_error_event((const xcb_generic_error_t *)p_event);
		return;
	}
	if (damage_version > 0 && type == damage_event_base + XCB_DAMAGE_NOTIFY) {
		const xcb_damage_notify_event_t *event = (const xcb_damage_notify_event_t *)p_event;
		if (event->damage == damage && damage != 0) {
			dirty = true;
			damage_events++;
		}
		return;
	}
	switch (type) {
		case XCB_CONFIGURE_NOTIFY: {
			const xcb_configure_notify_event_t *event = (const xcb_configure_notify_event_t *)p_event;
			if (event->window == window && window != 0) {
				const Size2i new_size(event->width, event->height);
				if (new_size != size) {
					size = new_size;
					dirty = true;
				}
				_update_root_position();
			}
		} break;
		case XCB_MAP_NOTIFY: {
			const xcb_map_notify_event_t *event = (const xcb_map_notify_event_t *)p_event;
			if (event->window == window && window != 0) {
				mapped = true;
				dirty = true;
				_update_root_position();
				emit_signal(SNAME("mapped_changed"), true);
			}
		} break;
		case XCB_UNMAP_NOTIFY: {
			const xcb_unmap_notify_event_t *event = (const xcb_unmap_notify_event_t *)p_event;
			if (event->window == window && window != 0) {
				mapped = false;
				emit_signal(SNAME("mapped_changed"), false);
			}
		} break;
		case XCB_DESTROY_NOTIFY: {
			const xcb_destroy_notify_event_t *event = (const xcb_destroy_notify_event_t *)p_event;
			if (event->window == window && window != 0) {
				print_line(vformat("X11WindowSource: window 0x%x was destroyed.", (int64_t)window));
				last_error = "The window was closed.";
				redirected = false;
				damage = 0; // Destroyed with the window.
				window_destroyed = true;
				stop_capture();
				emit_signal(SNAME("window_closed"));
			}
		} break;
		case XCB_PROPERTY_NOTIFY: {
			const xcb_property_notify_event_t *event = (const xcb_property_notify_event_t *)p_event;
			if (event->window == helper_window) {
				server_time_base = event->time;
				server_time_ticks = MAX(OS::get_singleton()->get_ticks_msec(), (uint64_t)1);
			} else if (event->window == window && window != 0) {
				const xcb_atom_t *net_wm_name = atoms.getptr("_NET_WM_NAME");
				if (event->atom == XCB_ATOM_WM_NAME || (net_wm_name && event->atom == *net_wm_name)) {
					emit_signal(SNAME("title_changed"));
				}
			}
		} break;
		case XCB_MAPPING_NOTIFY: {
			const xcb_mapping_notify_event_t *event = (const xcb_mapping_notify_event_t *)p_event;
			if (event->request == XCB_MAPPING_KEYBOARD || event->request == XCB_MAPPING_MODIFIER) {
				_load_keyboard_mapping();
			}
		} break;
		default:
			break;
	}
}

bool X11WindowSource::update() {
	if (connection == nullptr) {
		return false;
	}
	frame_counter++;
	X11DmabufImage::process_pending_frees();

	const Size2i size_before = size;
	xcb_generic_event_t *event;
	while (connection != nullptr && (event = xcb->xcb_poll_for_event(connection)) != nullptr) {
		_handle_event(event);
		free(event);
	}
	if (connection == nullptr) {
		return false;
	}
	if (xcb->xcb_connection_has_error(connection)) {
		last_error = "The X connection was lost.";
		ERR_PRINT("X11WindowSource: " + last_error);
		const bool had_window = window != 0;
		stop_capture();
		close();
		if (had_window) {
			emit_signal(SNAME("window_closed"));
		}
		return false;
	}
	if (window == 0 || !mapped) {
		return false;
	}
	if (damage == 0) {
		dirty = true;
	}
	if (!dirty) {
		return false;
	}
	// Subtract before reading, so anything drawn after this point raises a new notification.
	if (damage != 0) {
		xcb->xcb_damage_subtract(connection, damage, XCB_NONE, XCB_NONE);
	}

	const uint64_t start = OS::get_singleton()->get_ticks_usec();
	bool changed = false;
	bool failed = false;
	if (mode == CAPTURE_DMABUF) {
		bool ready = true;
		Error err = _fetch_dmabuf(ready);
		if (err != OK) {
			dmabuf_error = last_error;
			WARN_PRINT("X11WindowSource: the dmabuf path failed (" + dmabuf_error + "); copying the window instead.");
			_free_buffers();
			mode = shm_version > 0 ? CAPTURE_SHM : CAPTURE_GET_IMAGE;
			err = _fetch_copy();
		}
		if (err != OK) {
			failed = true;
		} else if (mode == CAPTURE_DMABUF && !ready) {
			// Still being written: keep showing the previous buffer, look again next frame.
			frames_deferred++;
			deferred_frames++;
		} else {
			changed = true;
		}
	} else {
		failed = _fetch_copy() != OK;
		changed = !failed;
	}
	if (changed) {
		dirty = false;
		deferred_frames = 0;
		frames_updated++;
	} else if (failed) {
		// E.g. the window was unmapped meanwhile; the next damage tries again.
		dirty = false;
	}
	_kick_render_ops();
	xcb->xcb_flush(connection);

	const uint64_t elapsed = OS::get_singleton()->get_ticks_usec() - start;
	fetch_usec_total += elapsed;
	fetch_usec_max = MAX(fetch_usec_max, elapsed);
	if (size != size_before && window != 0) {
		emit_signal(SNAME("size_changed"), size);
	}
	return changed;
}

Error X11WindowSource::_fetch_dmabuf(bool &r_ready) {
	r_ready = true;
	const xcb_pixmap_t pixmap = xcb->xcb_generate_id(connection);
	if (!_check(xcb->xcb_composite_name_window_pixmap_checked(connection, window, pixmap), "NameWindowPixmap")) {
		return ERR_CANT_ACQUIRE_RESOURCE;
	}

	int fds[4] = { -1, -1, -1, -1 };
	uint32_t strides[4] = {};
	uint32_t offsets[4] = {};
	int fd_count = 0;
	uint64_t buffer_modifier = 0x00ffffffffffffffULL; // DRM_FORMAT_MOD_INVALID
	int width = 0;
	int height = 0;
	int buffer_depth = 0;
	int bpp = 0;
	xcb_generic_error_t *error = nullptr;
	if (dri3_version >= _version(1, 2)) {
		XCBReply<xcb_dri3_buffers_from_pixmap_reply_t> reply(xcb->xcb_dri3_buffers_from_pixmap_reply(connection, xcb->xcb_dri3_buffers_from_pixmap(connection, pixmap), &error));
		if (reply) {
			const int *reply_fds = xcb->xcb_dri3_buffers_from_pixmap_reply_fds(connection, reply.reply);
			const uint32_t *reply_strides = xcb->xcb_dri3_buffers_from_pixmap_strides(reply.reply);
			const uint32_t *reply_offsets = xcb->xcb_dri3_buffers_from_pixmap_offsets(reply.reply);
			fd_count = reply->nfd;
			for (int i = 0; i < fd_count; i++) {
				if (i < 4) {
					fds[i] = reply_fds[i];
					strides[i] = reply_strides[i];
					offsets[i] = reply_offsets[i];
				} else {
					::close(reply_fds[i]);
				}
			}
			fd_count = MIN(fd_count, 4);
			buffer_modifier = reply->modifier;
			width = reply->width;
			height = reply->height;
			buffer_depth = reply->depth;
			bpp = reply->bpp;
		}
	} else {
		XCBReply<xcb_dri3_buffer_from_pixmap_reply_t> reply(xcb->xcb_dri3_buffer_from_pixmap_reply(connection, xcb->xcb_dri3_buffer_from_pixmap(connection, pixmap), &error));
		if (reply) {
			const int *reply_fds = xcb->xcb_dri3_buffer_from_pixmap_reply_fds(connection, reply.reply);
			fd_count = 1;
			fds[0] = reply_fds[0];
			strides[0] = reply->stride;
			width = reply->width;
			height = reply->height;
			buffer_depth = reply->depth;
			bpp = reply->bpp;
		}
	}
	xcb->xcb_free_pixmap(connection, pixmap);
	if (error != nullptr) {
		_error_event(error);
		free(error);
		last_error = "DRI3 BuffersFromPixmap failed: " + last_x_error + ".";
		return ERR_CANT_ACQUIRE_RESOURCE;
	}
	if (fd_count <= 0) {
		last_error = "DRI3 returned no buffer.";
		return ERR_CANT_ACQUIRE_RESOURCE;
	}

	struct stat st = {};
	if (fstat(fds[0], &st) != 0) {
		last_error = vformat("fstat on the dmabuf failed: %s.", strerror(errno));
		for (int i = 0; i < fd_count; i++) {
			::close(fds[i]);
		}
		return ERR_CANT_ACQUIRE_RESOURCE;
	}
	for (int i = 1; i < fd_count; i++) {
		struct stat plane_st = {};
		if (fstat(fds[i], &plane_st) != 0 || plane_st.st_ino != st.st_ino) {
			last_error = "The buffer's planes are in different dmabufs (not supported).";
			for (int j = 0; j < fd_count; j++) {
				::close(fds[j]);
			}
			return ERR_UNAVAILABLE;
		}
	}

	// The same buffer exports as the same dmabuf (same inode): import each one only once.
	const Size2i buffer_size(width, height);
	int index = -1;
	for (uint32_t i = 0; i < buffers.size(); i++) {
		if (buffers[i].inode == st.st_ino && buffers[i].size == buffer_size) {
			index = (int)i;
			break;
		}
	}
	if (index >= 0) {
		for (int i = 0; i < fd_count; i++) {
			::close(fds[i]);
		}
	} else {
		const uint32_t buffer_fourcc = X11DmabufImage::fourcc_for_depth(buffer_depth, bpp);
		String import_error;
		Ref<X11DmabufImage> image;
		image.instantiate();
		Error err = buffer_fourcc == 0 ? ERR_UNAVAILABLE : image->import(fds[0], width, height, buffer_fourcc, buffer_modifier, fd_count, offsets, strides, import_error);
		for (int i = 1; i < fd_count; i++) {
			::close(fds[i]);
		}
		if (err != OK) {
			::close(fds[0]);
			import_failures++;
			last_error = buffer_fourcc == 0 ? vformat("Depth %d at %d bpp can't be imported.", buffer_depth, bpp) : import_error;
			return err;
		}
		_evict_buffers();
		CachedBuffer buffer;
		buffer.image = image;
		buffer.texture = RS::get_singleton()->texture_rd_create(image->get_texture());
		buffer.inode = st.st_ino;
		buffer.fd = fds[0];
		buffer.size = buffer_size;
		buffers.push_back(buffer);
		index = (int)buffers.size() - 1;
		imports++;
		if (fourcc != buffer_fourcc || modifier != buffer_modifier || imports == 1) {
			print_line(vformat("X11WindowSource: importing the window's buffers: %dx%d %s, modifier 0x%s, %d plane(s), pitch %d, offset %d.", width, height,
					X11DmabufImage::fourcc_to_string(buffer_fourcc), String::num_uint64(buffer_modifier, 16), fd_count, strides[0], offsets[0]));
		}
		fourcc = buffer_fourcc;
		modifier = buffer_modifier;
	}

	CachedBuffer &buffer = buffers[index];
	buffer.last_used = frame_counter;

	// Implicit sync: a dmabuf polls readable once its pending writes (the X server's rendering) are done.
	if (deferred_frames < MAX_DEFERRED_FRAMES) {
		struct pollfd pfd = {};
		pfd.fd = buffer.fd;
		pfd.events = POLLIN;
		if (poll(&pfd, 1, 0) == 0) {
			r_ready = false;
			return OK;
		}
	}

	if (index != current_buffer) {
		if (current_buffer >= 0) {
			_queue_render_op(RenderOp::RELEASE, buffers[current_buffer].image);
		}
		_queue_render_op(RenderOp::ACQUIRE, buffer.image);
		current_buffer = index;
		buffer_switches++;
		texture->set_source(buffer.texture, buffer.size, X11DmabufImage::fourcc_has_alpha(fourcc));
		size = buffer.size;
	} else {
		// Same buffer, new content: hand it back and take it again, so the new writes are visible.
		_queue_render_op(RenderOp::RELEASE, buffer.image);
		_queue_render_op(RenderOp::ACQUIRE, buffer.image);
	}
	return OK;
}

void X11WindowSource::_evict_buffers() {
	while (buffers.size() >= MAX_CACHED_BUFFERS) {
		int oldest = -1;
		for (uint32_t i = 0; i < buffers.size(); i++) {
			if ((int)i != current_buffer && (oldest < 0 || buffers[i].last_used < buffers[oldest].last_used)) {
				oldest = (int)i;
			}
		}
		if (oldest < 0) {
			return;
		}
		CachedBuffer &buffer = buffers[oldest];
		RS::get_singleton()->free_rid(buffer.texture);
		::close(buffer.fd);
		_queue_render_op(RenderOp::FREE, buffer.image);
		const int last = (int)buffers.size() - 1;
		buffers.remove_at_unordered(oldest);
		if (current_buffer == last) {
			current_buffer = oldest;
		}
	}
}

void X11WindowSource::_free_buffers() {
	_kick_render_ops();
	if (buffers.is_empty()) {
		current_buffer = -1;
		return;
	}
	Array images;
	for (CachedBuffer &buffer : buffers) {
		if (buffer.texture.is_valid()) {
			RS::get_singleton()->free_rid(buffer.texture);
		}
		if (buffer.fd >= 0) {
			::close(buffer.fd);
		}
		images.push_back(buffer.image);
	}
	buffers.clear();
	current_buffer = -1;
	RS::get_singleton()->call_on_render_thread(callable_mp_static(&X11WindowSource::_render_free_images).bind(images));
}

Error X11WindowSource::_ensure_shm(size_t p_size) {
	if (shm_segment != 0 && shm_size >= p_size) {
		return OK;
	}
	_free_shm();
	if (shm_fd_passing) {
		const int fd = memfd_create("godot-x11window", MFD_CLOEXEC);
		if (fd >= 0 && ftruncate(fd, p_size) == 0) {
			void *data = mmap(nullptr, p_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
			if (data != MAP_FAILED) {
				const uint32_t segment = xcb->xcb_generate_id(connection);
				// xcb closes the fd it sends: give it a duplicate.
				const int send_fd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
				if (send_fd >= 0 && _check(xcb->xcb_shm_attach_fd_checked(connection, segment, send_fd, 0), "ShmAttachFd")) {
					shm_fd = fd;
					shm_data = data;
					shm_size = p_size;
					shm_segment = segment;
					return OK;
				}
				munmap(data, p_size);
			}
		}
		if (fd >= 0) {
			::close(fd);
		}
	}
	// System V shared memory, for servers without fd passing (or where it failed).
	const int id = shmget(IPC_PRIVATE, p_size, IPC_CREAT | 0600);
	if (id >= 0) {
		void *data = shmat(id, nullptr, 0);
		if (data != (void *)-1) {
			const uint32_t segment = xcb->xcb_generate_id(connection);
			if (_check(xcb->xcb_shm_attach_checked(connection, segment, id, 0), "ShmAttach")) {
				shmctl(id, IPC_RMID, nullptr); // Freed once both sides detach.
				shm_id = id;
				shm_data = data;
				shm_size = p_size;
				shm_segment = segment;
				return OK;
			}
			shmdt(data);
		}
		shmctl(id, IPC_RMID, nullptr);
	}
	return ERR_CANT_CREATE;
}

void X11WindowSource::_free_shm() {
	if (shm_segment != 0 && connection != nullptr) {
		xcb->xcb_shm_detach(connection, shm_segment);
	}
	if (shm_data != nullptr) {
		if (shm_id >= 0) {
			shmdt(shm_data);
		} else {
			munmap(shm_data, shm_size);
		}
	}
	if (shm_fd >= 0) {
		::close(shm_fd);
	}
	shm_segment = 0;
	shm_data = nullptr;
	shm_size = 0;
	shm_fd = -1;
	shm_id = -1;
}

void X11WindowSource::_free_upload_texture() {
	_kick_render_ops();
	if (upload_texture.is_valid()) {
		RS::get_singleton()->free_rid(upload_texture);
		upload_texture = RID();
	}
	if (upload_rd_texture.is_valid()) {
		RenderingDevice *rd = RenderingDevice::get_singleton();
		if (rd != nullptr) {
			rd->free_rid(upload_rd_texture);
		}
		upload_rd_texture = RID();
	}
	upload_size = Size2i();
}

Error X11WindowSource::_fetch_copy() {
	if (size.width <= 0 || size.height <= 0) {
		return ERR_UNCONFIGURED;
	}
	const size_t bytes = size_t(size.width) * size.height * 4;
	if (mode == CAPTURE_SHM && _ensure_shm(bytes) != OK) {
		WARN_PRINT("X11WindowSource: MIT-SHM is unavailable (" + last_error + "); using GetImage.");
		mode = CAPTURE_GET_IMAGE;
	}

	upload_data.resize(bytes); // Allocates only when the size changes.
	xcb_generic_error_t *error = nullptr;
	int image_depth = 0;
	if (mode == CAPTURE_SHM) {
		XCBReply<xcb_shm_get_image_reply_t> reply(xcb->xcb_shm_get_image_reply(connection,
				xcb->xcb_shm_get_image(connection, window, 0, 0, size.width, size.height, ~0u, XCB_IMAGE_FORMAT_Z_PIXMAP, shm_segment, 0), &error));
		if (!reply || reply->size < bytes) {
			if (error) {
				_error_event(error);
				free(error);
			}
			last_error = reply ? vformat("ShmGetImage returned %d bytes, %d expected (not 32 bits per pixel?).", reply->size, (int64_t)bytes) : "ShmGetImage failed: " + last_x_error + ".";
			return ERR_CANT_ACQUIRE_RESOURCE;
		}
		image_depth = reply->depth;
		memcpy(upload_data.ptrw(), shm_data, bytes);
	} else {
		XCBReply<xcb_get_image_reply_t> reply(xcb->xcb_get_image_reply(connection, xcb->xcb_get_image(connection, XCB_IMAGE_FORMAT_Z_PIXMAP, window, 0, 0, size.width, size.height, ~0u), &error));
		if (!reply || (size_t)xcb->xcb_get_image_data_length(reply.reply) < bytes) {
			if (error) {
				_error_event(error);
				free(error);
			}
			last_error = reply ? "GetImage returned fewer bytes than expected (not 32 bits per pixel?)." : "GetImage failed: " + last_x_error + ".";
			return ERR_CANT_ACQUIRE_RESOURCE;
		}
		image_depth = reply->depth;
		memcpy(upload_data.ptrw(), xcb->xcb_get_image_data(reply.reply), bytes);
	}
	copies++;
	bytes_copied += bytes;

	if (upload_size != size || upload_rd_texture.is_null()) {
		_free_upload_texture();
		RenderingDevice *rd = RenderingDevice::get_singleton();
		ERR_FAIL_NULL_V(rd, ERR_UNAVAILABLE);
		RD::TextureFormat format;
		format.format = RD::DATA_FORMAT_B8G8R8A8_UNORM;
		format.width = size.width;
		format.height = size.height;
		format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_UPDATE_BIT;
		RD::TextureView view;
		if (image_depth != 32) {
			view.swizzle_a = RD::TEXTURE_SWIZZLE_ONE;
		}
		upload_rd_texture = rd->texture_create(format, view);
		ERR_FAIL_COND_V(upload_rd_texture.is_null(), ERR_CANT_CREATE);
		upload_texture = RS::get_singleton()->texture_rd_create(upload_rd_texture);
		upload_size = size;
		texture->set_source(upload_texture, size, image_depth == 32);
	}
	_queue_render_op(RenderOp::UPLOAD, Ref<X11DmabufImage>(), upload_rd_texture, upload_data);
	return OK;
}

Ref<Texture2D> X11WindowSource::get_texture() const {
	return texture;
}

Dictionary X11WindowSource::get_stats() const {
	Dictionary stats;
	static const char *mode_names[] = { "none", "dmabuf", "shm", "get_image" };
	stats["mode"] = mode_names[mode];
	stats["zero_copy"] = mode == CAPTURE_DMABUF;
	stats["window"] = (int64_t)window;
	stats["mapped"] = mapped;
	stats["size"] = size;
	stats["depth"] = depth;
	stats["format"] = fourcc ? X11DmabufImage::fourcc_to_string(fourcc) : String();
	stats["modifier"] = "0x" + String::num_uint64(modifier, 16);
	stats["damage_events"] = damage_events;
	stats["frames_updated"] = frames_updated;
	stats["frames_deferred"] = frames_deferred;
	stats["imports"] = imports;
	stats["import_failures"] = import_failures;
	stats["buffers_cached"] = buffers.size();
	stats["buffer_switches"] = buffer_switches;
	stats["copies"] = copies;
	stats["bytes_copied"] = bytes_copied;
	stats["fetch_ms_avg"] = frames_updated + frames_deferred > 0 ? double(fetch_usec_total) / 1000.0 / double(frames_updated + frames_deferred) : 0.0;
	stats["fetch_ms_max"] = double(fetch_usec_max) / 1000.0;
	stats["x_errors"] = x_errors;
	stats["last_x_error"] = last_x_error;
	stats["input_events"] = input_events;
	stats["dmabuf_error"] = dmabuf_error;
	stats["last_error"] = last_error;
	return stats;
}

/* Input */

static uint16_t _modifier_for_keysym(uint32_t p_keysym) {
	switch (p_keysym) {
		case 0xffe1: // Shift_L
		case 0xffe2: // Shift_R
			return XCB_MOD_MASK_SHIFT;
		case 0xffe3: // Control_L
		case 0xffe4: // Control_R
			return XCB_MOD_MASK_CONTROL;
		case 0xffe7: // Meta_L
		case 0xffe8: // Meta_R
		case 0xffe9: // Alt_L
		case 0xffea: // Alt_R
			return XCB_MOD_MASK_1;
		case 0xffeb: // Super_L
		case 0xffec: // Super_R
			return XCB_MOD_MASK_4;
		default:
			return 0;
	}
}

void X11WindowSource::_send_crossing(xcb_window_t p_window, bool p_enter, int p_x, int p_y) {
	xcb_enter_notify_event_t event = {};
	event.response_type = p_enter ? XCB_ENTER_NOTIFY : XCB_LEAVE_NOTIFY;
	event.detail = XCB_NOTIFY_DETAIL_NONLINEAR;
	event.time = _server_time();
	event.root = root;
	event.event = p_window;
	event.child = XCB_NONE;
	event.root_x = root_position.x + p_x;
	event.root_y = root_position.y + p_y;
	event.event_x = p_x;
	event.event_y = p_y;
	event.state = modifier_state | button_state;
	event.mode = XCB_NOTIFY_MODE_NORMAL;
	event.same_screen_focus = 3; // same_screen and focus.
	xcb->xcb_send_event(connection, 0, p_window, 0, (const char *)&event);
}

void X11WindowSource::_send_pointer_event(uint8_t p_type, uint8_t p_detail) {
	// An empty event mask sends the event to the client that created the window, whatever it selected (a toolkit that
	// reads XInput2 events may select no core pointer events at all).
	xcb_button_press_event_t event = {};
	event.response_type = p_type;
	event.detail = p_detail;
	event.time = _server_time();
	event.root = root;
	event.event = window;
	event.child = XCB_NONE;
	event.root_x = root_position.x + pointer.x;
	event.root_y = root_position.y + pointer.y;
	event.event_x = pointer.x;
	event.event_y = pointer.y;
	event.state = modifier_state | button_state;
	event.same_screen = 1;
	xcb->xcb_send_event(connection, 0, window, 0, (const char *)&event);
}

void X11WindowSource::send_pointer(double p_x, double p_y) {
	if (connection == nullptr || window == 0) {
		return;
	}
	pointer = Point2i(CLAMP((int)Math::round(p_x), 0, MAX(size.width - 1, 0)), CLAMP((int)Math::round(p_y), 0, MAX(size.height - 1, 0)));
	input_events++;
	if (input_method == INPUT_XTEST && xtest_version > 0) {
		xcb->xcb_test_fake_input(connection, XCB_MOTION_NOTIFY, 0, XCB_CURRENT_TIME, root, root_position.x + pointer.x, root_position.y + pointer.y, 0);
	} else {
		if (pointer_window != window) {
			_send_crossing(window, true, pointer.x, pointer.y);
			pointer_window = window;
		}
		_send_pointer_event(XCB_MOTION_NOTIFY, XCB_MOTION_NORMAL);
	}
	xcb->xcb_flush(connection);
}

void X11WindowSource::send_button(int p_button, bool p_pressed) {
	if (connection == nullptr || window == 0 || p_button < 1 || p_button > 255) {
		return;
	}
	input_events++;
	if (input_method == INPUT_XTEST && xtest_version > 0) {
		xcb->xcb_test_fake_input(connection, p_pressed ? XCB_BUTTON_PRESS : XCB_BUTTON_RELEASE, p_button, XCB_CURRENT_TIME, XCB_NONE, 0, 0, 0);
	} else {
		if (pointer_window != window) {
			_send_crossing(window, true, pointer.x, pointer.y);
			pointer_window = window;
		}
		_send_pointer_event(p_pressed ? XCB_BUTTON_PRESS : XCB_BUTTON_RELEASE, p_button);
	}
	// The state of an event is the state before it.
	if (p_button <= 5) {
		const uint16_t mask = uint16_t(XCB_BUTTON_MASK_1 << (p_button - 1));
		button_state = p_pressed ? (button_state | mask) : (button_state & ~mask);
	}
	xcb->xcb_flush(connection);
}

void X11WindowSource::send_scroll(double p_dx, double p_dy) {
	scroll_remainder += Vector2(p_dx, p_dy);
	for (int i = 0; i < 20 && Math::abs(scroll_remainder.y) >= 1.0; i++) {
		const int button = scroll_remainder.y > 0 ? 5 : 4;
		send_button(button, true);
		send_button(button, false);
		scroll_remainder.y -= SIGN(scroll_remainder.y);
	}
	for (int i = 0; i < 20 && Math::abs(scroll_remainder.x) >= 1.0; i++) {
		const int button = scroll_remainder.x > 0 ? 7 : 6;
		send_button(button, true);
		send_button(button, false);
		scroll_remainder.x -= SIGN(scroll_remainder.x);
	}
	// Don't let a big leftover build up from one direction.
	scroll_remainder = scroll_remainder.clampf(-1.0, 1.0);
}

void X11WindowSource::send_key(int64_t p_keysym, bool p_pressed) {
	if (connection == nullptr || window == 0 || p_keysym <= 0) {
		return;
	}
	KeyCode code;
	if (!_keysym_to_keycode((uint32_t)p_keysym, code)) {
		print_verbose(vformat("X11WindowSource: no keycode for keysym 0x%x.", p_keysym));
		return;
	}
	input_events++;
	if (input_method == INPUT_XTEST && xtest_version > 0) {
		const bool add_shift = code.shift && p_pressed && !(modifier_state & SHIFT_MASK) && shift_keycode != 0;
		if (add_shift) {
			xcb->xcb_test_fake_input(connection, XCB_KEY_PRESS, shift_keycode, XCB_CURRENT_TIME, XCB_NONE, 0, 0, 0);
		}
		xcb->xcb_test_fake_input(connection, p_pressed ? XCB_KEY_PRESS : XCB_KEY_RELEASE, code.keycode, XCB_CURRENT_TIME, XCB_NONE, 0, 0, 0);
		if (add_shift) {
			xcb->xcb_test_fake_input(connection, XCB_KEY_RELEASE, shift_keycode, XCB_CURRENT_TIME, XCB_NONE, 0, 0, 0);
		}
	} else {
		xcb_key_press_event_t event = {};
		event.response_type = p_pressed ? XCB_KEY_PRESS : XCB_KEY_RELEASE;
		event.detail = code.keycode;
		event.time = _server_time();
		event.root = root;
		event.event = window;
		event.child = XCB_NONE;
		event.root_x = root_position.x + pointer.x;
		event.root_y = root_position.y + pointer.y;
		event.event_x = pointer.x;
		event.event_y = pointer.y;
		event.state = modifier_state | button_state | (code.shift ? SHIFT_MASK : 0);
		event.same_screen = 1;
		xcb->xcb_send_event(connection, 0, window, 0, (const char *)&event);
	}
	const uint16_t modifier_mask = _modifier_for_keysym((uint32_t)p_keysym);
	if (modifier_mask) {
		modifier_state = p_pressed ? (modifier_state | modifier_mask) : (modifier_state & ~modifier_mask);
	}
	xcb->xcb_flush(connection);
}

void X11WindowSource::send_unicode(int64_t p_codepoint, bool p_pressed) {
	if (p_codepoint <= 0 || p_codepoint > 0x10ffff) {
		return;
	}
	send_key(_unicode_to_keysym((char32_t)p_codepoint), p_pressed);
}

void X11WindowSource::send_text(const String &p_text) {
	for (int i = 0; i < p_text.length(); i++) {
		send_unicode(p_text[i], true);
		send_unicode(p_text[i], false);
	}
}

void X11WindowSource::send_godot_key(Key p_key, bool p_pressed) {
	struct Mapping {
		Key key;
		uint32_t keysym;
	};
	static const Mapping mappings[] = {
		{ Key::ESCAPE, 0xff1b },
		{ Key::TAB, 0xff09 },
		{ Key::BACKTAB, 0xfe20 },
		{ Key::BACKSPACE, 0xff08 },
		{ Key::ENTER, 0xff0d },
		{ Key::KP_ENTER, 0xff8d },
		{ Key::INSERT, 0xff63 },
		{ Key::KEY_DELETE, 0xffff },
		{ Key::PAUSE, 0xff13 },
		{ Key::PRINT, 0xff61 },
		{ Key::HOME, 0xff50 },
		{ Key::END, 0xff57 },
		{ Key::LEFT, 0xff51 },
		{ Key::UP, 0xff52 },
		{ Key::RIGHT, 0xff53 },
		{ Key::DOWN, 0xff54 },
		{ Key::PAGEUP, 0xff55 },
		{ Key::PAGEDOWN, 0xff56 },
		{ Key::SHIFT, 0xffe1 },
		{ Key::CTRL, 0xffe3 },
		{ Key::META, 0xffeb },
		{ Key::ALT, 0xffe9 },
		{ Key::CAPSLOCK, 0xffe5 },
		{ Key::MENU, 0xff67 },
		{ Key::F1, 0xffbe },
		{ Key::F2, 0xffbf },
		{ Key::F3, 0xffc0 },
		{ Key::F4, 0xffc1 },
		{ Key::F5, 0xffc2 },
		{ Key::F6, 0xffc3 },
		{ Key::F7, 0xffc4 },
		{ Key::F8, 0xffc5 },
		{ Key::F9, 0xffc6 },
		{ Key::F10, 0xffc7 },
		{ Key::F11, 0xffc8 },
		{ Key::F12, 0xffc9 },
		{ Key::BACK, 0x1008ff26 }, // XF86Back
		{ Key::FORWARD, 0x1008ff27 }, // XF86Forward
		{ Key::REFRESH, 0x1008ff29 }, // XF86Reload
		{ Key::MEDIAPLAY, 0x1008ff14 }, // XF86AudioPlay
		{ Key::VOLUMEUP, 0x1008ff13 },
		{ Key::VOLUMEDOWN, 0x1008ff11 },
		{ Key::VOLUMEMUTE, 0x1008ff12 },
	};
	const Key key = p_key & KeyModifierMask::CODE_MASK;
	for (const Mapping &m : mappings) {
		if (m.key == key) {
			send_key(m.keysym, p_pressed);
			return;
		}
	}
	const uint32_t code = (uint32_t)key;
	if (code >= 'A' && code <= 'Z') {
		send_key(code + 32, p_pressed); // Godot names letters by their uppercase character; the key is the lowercase keysym.
	} else if (code >= 0x20 && code < (uint32_t)Key::SPECIAL) {
		send_unicode(code, p_pressed);
	}
}

void X11WindowSource::send_focus_in() {
	if (connection == nullptr || window == 0) {
		return;
	}
	xcb_focus_in_event_t event = {};
	event.response_type = XCB_FOCUS_IN;
	event.detail = XCB_NOTIFY_DETAIL_NONLINEAR;
	event.event = window;
	event.mode = XCB_NOTIFY_MODE_NORMAL;
	xcb->xcb_send_event(connection, 0, window, 0, (const char *)&event);
	xcb->xcb_flush(connection);
}

/* Window management */

int64_t X11WindowSource::get_own_window() const {
	DisplayServer *ds = DisplayServer::get_singleton();
	if (ds == nullptr || ds->get_name() != "X11") {
		return 0;
	}
	return ds->window_get_native_handle(DisplayServerEnums::WINDOW_HANDLE, DisplayServerEnums::MAIN_WINDOW_ID);
}

int64_t X11WindowSource::get_toplevel(int64_t p_window) {
	ERR_FAIL_NULL_V_MSG(connection, 0, "Call open() first.");
	return _toplevel_of((xcb_window_t)p_window);
}

Error X11WindowSource::set_input_focus(int64_t p_window) {
	ERR_FAIL_NULL_V_MSG(connection, ERR_UNCONFIGURED, "Call open() first.");
	// X keyboard focus only (what XTEST key events follow). gamescope sets it again whenever its own focus choice changes.
	return _check(xcb->xcb_set_input_focus_checked(connection, XCB_INPUT_FOCUS_PARENT, (xcb_window_t)p_window, XCB_CURRENT_TIME), "SetInputFocus") ? OK : ERR_CANT_ACQUIRE_RESOURCE;
}

Error X11WindowSource::raise_window(int64_t p_window) {
	ERR_FAIL_NULL_V_MSG(connection, ERR_UNCONFIGURED, "Call open() first.");
	const xcb_window_t toplevel = _toplevel_of((xcb_window_t)p_window);
	if (toplevel == 0) {
		return ERR_DOES_NOT_EXIST;
	}
	// A window manager (gamescope is one) gets this as a ConfigureRequest; the client message is the EWMH way to ask it.
	const uint32_t stack_mode = XCB_STACK_MODE_ABOVE;
	xcb->xcb_configure_window(connection, toplevel, XCB_CONFIG_WINDOW_STACK_MODE, &stack_mode);
	PackedInt64Array data;
	data.push_back(2); // Source: a pager (direct user action).
	data.push_back(_server_time());
	data.push_back(0);
	return send_client_message(toplevel, "_NET_ACTIVE_WINDOW", data);
}

Error X11WindowSource::remap_window(int64_t p_window) {
	ERR_FAIL_NULL_V_MSG(connection, ERR_UNCONFIGURED, "Call open() first.");
	const xcb_window_t toplevel = _toplevel_of((xcb_window_t)p_window);
	if (toplevel == 0) {
		return ERR_DOES_NOT_EXIST;
	}
	xcb->xcb_unmap_window(connection, toplevel);
	xcb->xcb_map_window(connection, toplevel);
	xcb->xcb_flush(connection);
	return OK;
}

Error X11WindowSource::gamescope_focus(int64_t p_window, int64_t p_app_id) {
	ERR_FAIL_NULL_V_MSG(connection, ERR_UNCONFIGURED, "Call open() first.");
	const xcb_window_t toplevel = _toplevel_of((xcb_window_t)p_window);
	if (toplevel == 0) {
		return ERR_DOES_NOT_EXIST;
	}
	// With Steam-controlled focus gamescope only considers windows that have an app id.
	Variant game = get_window_property(toplevel, "STEAM_GAME");
	if (p_app_id != 0 || game.get_type() != Variant::PACKED_INT64_ARRAY || PackedInt64Array(game).is_empty() || PackedInt64Array(game)[0] == 0) {
		PackedInt64Array app_id;
		app_id.push_back(p_app_id != 0 ? p_app_id : int64_t(toplevel));
		Error err = set_window_property(toplevel, "STEAM_GAME", "CARDINAL", app_id);
		if (err != OK) {
			return err;
		}
	}
	PackedInt64Array target;
	target.push_back(toplevel);
	Error err = set_window_property(root, "GAMESCOPECTRL_BASELAYER_WINDOW", "CARDINAL", target);
	own_baselayer = err == OK;
	return err;
}

Error X11WindowSource::gamescope_release_focus() {
	if (connection == nullptr || !own_baselayer) {
		return OK;
	}
	own_baselayer = false;
	return delete_window_property(root, "GAMESCOPECTRL_BASELAYER_WINDOW");
}

Dictionary X11WindowSource::get_focus_info() {
	Dictionary info;
	ERR_FAIL_NULL_V_MSG(connection, info, "Call open() first.");
	static const char *root_properties[] = {
		"GAMESCOPE_FOCUSED_WINDOW",
		"GAMESCOPE_FOCUSED_APP",
		"GAMESCOPE_FOCUSED_APP_GFX",
		"GAMESCOPE_FOCUSABLE_WINDOWS",
		"GAMESCOPE_FOCUSABLE_APPS",
		"GAMESCOPECTRL_BASELAYER_WINDOW",
		"GAMESCOPECTRL_BASELAYER_APPID",
		"_NET_ACTIVE_WINDOW",
	};
	for (const char *name : root_properties) {
		info[name] = get_window_property(root, name);
	}
	XCBReply<xcb_get_input_focus_reply_t> focus(xcb->xcb_get_input_focus_reply(connection, xcb->xcb_get_input_focus(connection), nullptr));
	info["x_input_focus"] = focus ? (int64_t)focus->focus : int64_t(0);
	const int64_t own = get_own_window();
	info["own_window"] = own;
	info["own_toplevel"] = own != 0 ? get_toplevel(own) : int64_t(0);
	info["captured_window"] = (int64_t)window;
	info["root"] = (int64_t)root;
	return info;
}

Error X11WindowSource::set_input_passthrough(int64_t p_window, bool p_enabled) {
	ERR_FAIL_NULL_V_MSG(connection, ERR_UNCONFIGURED, "Call open() first.");
	if (xfixes_version < _version(2, 0)) {
		last_error = "Input shapes need XFixes 2.";
		return ERR_UNAVAILABLE;
	}
	xcb_xfixes_region_t region = XCB_NONE;
	if (p_enabled) {
		region = xcb->xcb_generate_id(connection);
		xcb->xcb_xfixes_create_region(connection, region, 0, nullptr);
	}
	const bool ok = _check(xcb->xcb_xfixes_set_window_shape_region_checked(connection, (xcb_window_t)p_window, XCB_SHAPE_SK_INPUT, 0, 0, region), "SetWindowShapeRegion");
	if (region != XCB_NONE) {
		xcb->xcb_xfixes_destroy_region(connection, region);
	}
	xcb->xcb_flush(connection);
	return ok ? OK : ERR_CANT_ACQUIRE_RESOURCE;
}

Variant X11WindowSource::get_window_property(int64_t p_window, const String &p_name) {
	ERR_FAIL_NULL_V_MSG(connection, Variant(), "Call open() first.");
	const xcb_atom_t atom = _atom(p_name, false);
	if (atom == XCB_ATOM_NONE) {
		return Variant();
	}
	XCBReply<xcb_get_property_reply_t> reply(xcb->xcb_get_property_reply(connection, xcb->xcb_get_property(connection, 0, (xcb_window_t)p_window, atom, XCB_GET_PROPERTY_TYPE_ANY, 0, 65536), nullptr));
	if (!reply || reply->type == XCB_ATOM_NONE) {
		return Variant();
	}
	const void *data = xcb->xcb_get_property_value(reply.reply);
	const int length = xcb->xcb_get_property_value_length(reply.reply);
	if (reply->format == 8) {
		return String::utf8((const char *)data, length);
	}
	PackedInt64Array values;
	if (reply->format == 32) {
		const uint32_t *v = (const uint32_t *)data;
		for (int i = 0; i < length / 4; i++) {
			values.push_back(v[i]);
		}
	} else if (reply->format == 16) {
		const uint16_t *v = (const uint16_t *)data;
		for (int i = 0; i < length / 2; i++) {
			values.push_back(v[i]);
		}
	}
	return values;
}

Error X11WindowSource::set_window_property(int64_t p_window, const String &p_name, const String &p_type, const PackedInt64Array &p_values) {
	ERR_FAIL_NULL_V_MSG(connection, ERR_UNCONFIGURED, "Call open() first.");
	const xcb_atom_t atom = _atom(p_name);
	xcb_atom_t type = XCB_ATOM_CARDINAL;
	if (p_type == "WINDOW") {
		type = XCB_ATOM_WINDOW;
	} else if (p_type == "ATOM") {
		type = XCB_ATOM_ATOM;
	} else if (p_type != "CARDINAL") {
		type = _atom(p_type);
	}
	LocalVector<uint32_t> data;
	data.resize(p_values.size());
	for (int i = 0; i < p_values.size(); i++) {
		data[i] = (uint32_t)p_values[i];
	}
	return _check(xcb->xcb_change_property_checked(connection, XCB_PROP_MODE_REPLACE, (xcb_window_t)p_window, atom, type, 32, data.size(), data.ptr()), "ChangeProperty") ? OK : ERR_CANT_ACQUIRE_RESOURCE;
}

Error X11WindowSource::delete_window_property(int64_t p_window, const String &p_name) {
	ERR_FAIL_NULL_V_MSG(connection, ERR_UNCONFIGURED, "Call open() first.");
	const xcb_atom_t atom = _atom(p_name, false);
	if (atom == XCB_ATOM_NONE) {
		return OK;
	}
	xcb->xcb_delete_property(connection, (xcb_window_t)p_window, atom);
	xcb->xcb_flush(connection);
	return OK;
}

Error X11WindowSource::send_client_message(int64_t p_window, const String &p_type, const PackedInt64Array &p_data, int64_t p_destination) {
	ERR_FAIL_NULL_V_MSG(connection, ERR_UNCONFIGURED, "Call open() first.");
	xcb_client_message_event_t event = {};
	event.response_type = XCB_CLIENT_MESSAGE;
	event.format = 32;
	event.window = (xcb_window_t)p_window;
	event.type = _atom(p_type);
	for (int i = 0; i < MIN(p_data.size(), 5); i++) {
		event.data.data32[i] = (uint32_t)p_data[i];
	}
	const xcb_window_t destination = p_destination != 0 ? (xcb_window_t)p_destination : root;
	const uint32_t mask = destination == root ? (XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY) : 0;
	xcb->xcb_send_event(connection, 0, destination, mask, (const char *)&event);
	xcb->xcb_flush(connection);
	return OK;
}

/* Object */

void X11WindowSource::_bind_methods() {
	ClassDB::bind_method(D_METHOD("open", "display"), &X11WindowSource::open, DEFVAL(String()));
	ClassDB::bind_method(D_METHOD("close"), &X11WindowSource::close);
	ClassDB::bind_method(D_METHOD("is_open"), &X11WindowSource::is_open);
	ClassDB::bind_method(D_METHOD("get_extensions"), &X11WindowSource::get_extensions);
	ClassDB::bind_method(D_METHOD("list_windows", "include_unmapped"), &X11WindowSource::list_windows, DEFVAL(false));
	ClassDB::bind_method(D_METHOD("get_window_info", "window"), &X11WindowSource::get_window_info);

	ClassDB::bind_method(D_METHOD("capture", "window"), &X11WindowSource::capture);
	ClassDB::bind_method(D_METHOD("stop_capture"), &X11WindowSource::stop_capture);
	ClassDB::bind_method(D_METHOD("update"), &X11WindowSource::update);
	ClassDB::bind_method(D_METHOD("set_preferred_mode", "mode"), &X11WindowSource::set_preferred_mode);
	ClassDB::bind_method(D_METHOD("get_preferred_mode"), &X11WindowSource::get_preferred_mode);
	ClassDB::bind_method(D_METHOD("get_capture_mode"), &X11WindowSource::get_capture_mode);
	ClassDB::bind_method(D_METHOD("get_window"), &X11WindowSource::get_window);
	ClassDB::bind_method(D_METHOD("is_capturing"), &X11WindowSource::is_capturing);
	ClassDB::bind_method(D_METHOD("is_window_mapped"), &X11WindowSource::is_window_mapped);
	ClassDB::bind_method(D_METHOD("get_texture"), &X11WindowSource::get_texture);
	ClassDB::bind_method(D_METHOD("get_size"), &X11WindowSource::get_size);
	ClassDB::bind_method(D_METHOD("has_alpha"), &X11WindowSource::has_alpha);
	ClassDB::bind_method(D_METHOD("get_stats"), &X11WindowSource::get_stats);
	ClassDB::bind_method(D_METHOD("get_last_error"), &X11WindowSource::get_last_error);

	ClassDB::bind_method(D_METHOD("set_input_method", "method"), &X11WindowSource::set_input_method);
	ClassDB::bind_method(D_METHOD("get_input_method"), &X11WindowSource::get_input_method);
	ClassDB::bind_method(D_METHOD("send_pointer", "x", "y"), &X11WindowSource::send_pointer);
	ClassDB::bind_method(D_METHOD("send_button", "button", "pressed"), &X11WindowSource::send_button);
	ClassDB::bind_method(D_METHOD("send_scroll", "dx", "dy"), &X11WindowSource::send_scroll);
	ClassDB::bind_method(D_METHOD("send_key", "keysym", "pressed"), &X11WindowSource::send_key);
	ClassDB::bind_method(D_METHOD("send_unicode", "codepoint", "pressed"), &X11WindowSource::send_unicode);
	ClassDB::bind_method(D_METHOD("send_text", "text"), &X11WindowSource::send_text);
	ClassDB::bind_method(D_METHOD("send_godot_key", "key", "pressed"), &X11WindowSource::send_godot_key);
	ClassDB::bind_method(D_METHOD("send_focus_in"), &X11WindowSource::send_focus_in);

	ClassDB::bind_method(D_METHOD("get_own_window"), &X11WindowSource::get_own_window);
	ClassDB::bind_method(D_METHOD("get_toplevel", "window"), &X11WindowSource::get_toplevel);
	ClassDB::bind_method(D_METHOD("set_input_focus", "window"), &X11WindowSource::set_input_focus);
	ClassDB::bind_method(D_METHOD("raise_window", "window"), &X11WindowSource::raise_window);
	ClassDB::bind_method(D_METHOD("remap_window", "window"), &X11WindowSource::remap_window);
	ClassDB::bind_method(D_METHOD("gamescope_focus", "window", "app_id"), &X11WindowSource::gamescope_focus, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("gamescope_release_focus"), &X11WindowSource::gamescope_release_focus);
	ClassDB::bind_method(D_METHOD("get_focus_info"), &X11WindowSource::get_focus_info);
	ClassDB::bind_method(D_METHOD("set_input_passthrough", "window", "enabled"), &X11WindowSource::set_input_passthrough);

	ClassDB::bind_method(D_METHOD("get_window_property", "window", "name"), &X11WindowSource::get_window_property);
	ClassDB::bind_method(D_METHOD("set_window_property", "window", "name", "type", "values"), &X11WindowSource::set_window_property);
	ClassDB::bind_method(D_METHOD("delete_window_property", "window", "name"), &X11WindowSource::delete_window_property);
	ClassDB::bind_method(D_METHOD("send_client_message", "window", "type", "data", "destination"), &X11WindowSource::send_client_message, DEFVAL(0));

	ADD_SIGNAL(MethodInfo("window_closed"));
	ADD_SIGNAL(MethodInfo("mapped_changed", PropertyInfo(Variant::BOOL, "mapped")));
	ADD_SIGNAL(MethodInfo("size_changed", PropertyInfo(Variant::VECTOR2I, "size")));
	ADD_SIGNAL(MethodInfo("title_changed"));

	BIND_ENUM_CONSTANT(CAPTURE_NONE);
	BIND_ENUM_CONSTANT(CAPTURE_DMABUF);
	BIND_ENUM_CONSTANT(CAPTURE_SHM);
	BIND_ENUM_CONSTANT(CAPTURE_GET_IMAGE);
	BIND_ENUM_CONSTANT(INPUT_SEND_EVENT);
	BIND_ENUM_CONSTANT(INPUT_XTEST);
}

X11WindowSource::X11WindowSource() {
	texture.instantiate();
	texture->init();
	render_callable = callable_mp(this, &X11WindowSource::_render_flush);
	render_ops.reserve(8);
}

X11WindowSource::~X11WindowSource() {
	close();
}
