/**************************************************************************/
/*  x11_window_source.h                                                   */
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

#include "x11_dmabuf_image.h"
#include "xcb_loader.h"

#include "core/os/mutex.h"
#include "core/templates/hash_map.h"
#include "core/variant/typed_array.h"
#include "scene/resources/texture.h"

// The captured window's picture as a Texture2D: its RenderingServer RID is a proxy pointed at whichever buffer is current
// (a dmabuf import, or the copy texture of the SHM fallback), so a material set up once keeps working across resizes.
class X11WindowTexture : public Texture2D {
	GDCLASS(X11WindowTexture, Texture2D);

	RID placeholder;
	RID proxy;
	Size2i size;
	bool alpha = false;

protected:
	static void _bind_methods() {}

public:
	void init();
	// An invalid RID shows the placeholder (opaque black).
	void set_source(RID p_texture, const Size2i &p_size, bool p_alpha);

	virtual int get_width() const override { return size.width; }
	virtual int get_height() const override { return size.height; }
	virtual RID get_rid() const override { return proxy; }
	virtual bool has_alpha() const override { return alpha; }

	~X11WindowTexture();
};

// Shows an X11 window of the desktop as a texture, zero-copy where the X server can export the window's pixmap as a dmabuf
// (Composite + DRI3), and forwards pointer, wheel and key input to it.
//
// Per window: Composite NameWindowPixmap -> DRI3 BuffersFromPixmap (dmabuf fds + DRM modifier) -> one VkImage per distinct
// buffer, imported once and cached by the dmabuf's inode (Xwayland may swap the window between a few buffers). DAMAGE
// says when the content changed: an idle window costs one non-blocking read per update(), a changed one a buffer lookup
// (two round trips) and a release/acquire of the image through the render graph. If the dmabuf path is unavailable,
// MIT-SHM GetImage copies the window into a texture instead (CPU copy, reported in get_stats()).
//
// Call update() once per frame from the main thread. Single-threaded rendering (the default thread model) is assumed,
// as for HWVideoTexture.
class X11WindowSource : public RefCounted {
	GDCLASS(X11WindowSource, RefCounted);

public:
	enum CaptureMode {
		CAPTURE_NONE,
		CAPTURE_DMABUF, // Zero copy: the window's pixmap imported as a Vulkan image.
		CAPTURE_SHM, // MIT-SHM GetImage into shared memory, then an upload.
		CAPTURE_GET_IMAGE, // Plain GetImage through the socket, then an upload.
	};

	enum InputMethod {
		INPUT_SEND_EVENT, // SendEvent to the captured window (synthetic events; no focus or stacking needed).
		INPUT_XTEST, // XTEST fake input: real events, delivered by the X server to the window under the pointer / with focus.
	};

private:
	struct CachedBuffer {
		Ref<X11DmabufImage> image;
		RID texture; // RenderingServer texture over the image's view.
		ino_t inode = 0;
		int fd = -1; // Kept for the readiness poll.
		Size2i size;
		uint64_t last_used = 0;
	};

	struct RenderOp {
		enum Type {
			ACQUIRE,
			RELEASE,
			FREE, // Release if needed, then free.
			UPLOAD, // Copy path: texture_update of upload_texture.
		};
		Type type = ACQUIRE;
		Ref<X11DmabufImage> image;
		RID rd_texture;
		Vector<uint8_t> data;
	};

	struct KeyCode {
		uint8_t keycode = 0;
		bool shift = false;
	};

	XCBLoader *xcb = nullptr;
	xcb_connection_t *connection = nullptr;
	xcb_window_t root = 0;
	xcb_window_t helper_window = 0; // Unmapped InputOnly window, for server timestamps.
	String display_name;

	// Extensions: version (major * 100 + minor), 0 when missing.
	int composite_version = 0;
	int damage_version = 0;
	int xfixes_version = 0;
	int xtest_version = 0;
	int dri3_version = 0;
	int shm_version = 0;
	bool shm_fd_passing = false;
	uint8_t damage_event_base = 0;
	uint8_t xfixes_event_base = 0;

	HashMap<String, xcb_atom_t> atoms;

	// Server time: the server's millisecond clock at a local tick, for event timestamps.
	uint32_t server_time_base = 0;
	uint64_t server_time_ticks = 0;

	// Capture.
	xcb_window_t window = 0;
	xcb_damage_damage_t damage = 0;
	bool redirected = false;
	bool mapped = false;
	bool window_destroyed = false;
	bool dirty = false;
	int deferred_frames = 0;
	Size2i size;
	Point2i root_position;
	int depth = 0;
	CaptureMode mode = CAPTURE_NONE;
	CaptureMode preferred_mode = CAPTURE_DMABUF;
	Ref<X11WindowTexture> texture;

	// Dmabuf path.
	LocalVector<CachedBuffer> buffers;
	int current_buffer = -1;
	uint32_t fourcc = 0;
	uint64_t modifier = 0;
	String dmabuf_error;

	// Copy paths.
	uint32_t shm_segment = 0;
	int shm_fd = -1;
	void *shm_data = nullptr;
	size_t shm_size = 0;
	int shm_id = -1; // SysV segment when fd passing isn't available.
	Vector<uint8_t> upload_data;
	RID upload_rd_texture;
	RID upload_texture;
	Size2i upload_size;

	// Render thread.
	Mutex render_mutex;
	LocalVector<RenderOp> render_ops;
	Callable render_callable;

	// Input.
	InputMethod input_method = INPUT_SEND_EVENT;
	uint8_t min_keycode = 8;
	uint8_t max_keycode = 255;
	HashMap<uint32_t, KeyCode> keysym_map;
	uint8_t shift_keycode = 0;
	uint8_t scratch_keycode = 0;
	uint32_t scratch_keysym = 0;
	int keysyms_per_keycode = 0;
	uint16_t modifier_state = 0;
	uint16_t button_state = 0;
	Point2i pointer;
	xcb_window_t pointer_window = 0;
	Vector2 scroll_remainder;

	// Stats.
	uint64_t frame_counter = 0;
	uint64_t damage_events = 0;
	uint64_t frames_updated = 0;
	uint64_t frames_deferred = 0;
	uint64_t imports = 0;
	uint64_t import_failures = 0;
	uint64_t buffer_switches = 0;
	uint64_t copies = 0;
	uint64_t bytes_copied = 0;
	uint64_t x_errors = 0;
	uint64_t input_events = 0;
	uint64_t fetch_usec_total = 0;
	uint64_t fetch_usec_max = 0;
	String last_x_error;
	String last_error;
	bool own_baselayer = false;

	static void _render_free_images(const Array &p_images);

	void _render_flush();
	void _queue_render_op(RenderOp::Type p_type, const Ref<X11DmabufImage> &p_image, RID p_rd_texture = RID(), const Vector<uint8_t> &p_data = Vector<uint8_t>());
	void _kick_render_ops();

	xcb_atom_t _atom(const String &p_name, bool p_create = true);
	String _atom_name_cached(xcb_atom_t p_atom) const;
	bool _check(xcb_void_cookie_t p_cookie, const char *p_what);
	void _error_event(const xcb_generic_error_t *p_error);
	uint32_t _server_time();
	void _sync_server_time();
	void _load_keyboard_mapping();
	bool _keysym_to_keycode(uint32_t p_keysym, KeyCode &r_code);

	Dictionary _describe_window(xcb_window_t p_frame, xcb_window_t p_client);
	String _get_string_property(xcb_window_t p_window, xcb_atom_t p_property);
	xcb_window_t _find_client(xcb_window_t p_window, int p_depth);
	xcb_window_t _toplevel_of(xcb_window_t p_window);
	void _update_root_position();

	void _handle_event(xcb_generic_event_t *p_event);
	Error _fetch_dmabuf(bool &r_ready);
	Error _fetch_copy();
	Error _ensure_shm(size_t p_size);
	void _free_shm();
	void _free_buffers();
	void _free_upload_texture();
	void _evict_buffers();

	xcb_window_t _pointer_target(int p_x, int p_y, int &r_x, int &r_y);
	void _send_pointer_event(uint8_t p_type, uint8_t p_detail);
	void _send_crossing(xcb_window_t p_window, bool p_enter, int p_x, int p_y);

protected:
	static void _bind_methods();

public:
	Error open(const String &p_display = String());
	void close();
	bool is_open() const { return connection != nullptr; }
	Dictionary get_extensions() const;

	// Toplevel windows, bottom to top: id (the client window), frame (its toplevel ancestor, == id without a reparenting
	// window manager), title, class, instance, pid, x, y, width, height, depth, mapped, override_redirect, steam_game.
	TypedArray<Dictionary> list_windows(bool p_include_unmapped = false);
	Dictionary get_window_info(int64_t p_window);

	Error capture(int64_t p_window);
	void stop_capture();
	// Processes X events and refreshes the texture if the window changed. Returns true when the texture got new content.
	bool update();

	void set_preferred_mode(CaptureMode p_mode) { preferred_mode = p_mode; }
	CaptureMode get_preferred_mode() const { return preferred_mode; }
	CaptureMode get_capture_mode() const { return mode; }
	int64_t get_window() const { return window; }
	bool is_capturing() const { return window != 0; }
	bool is_window_mapped() const { return mapped; }
	Ref<Texture2D> get_texture() const;
	Size2i get_size() const { return size; }
	bool has_alpha() const { return depth == 32; }
	Dictionary get_stats() const;
	String get_last_error() const { return last_error; }

	// Input, in window pixels from the top-left corner of the captured window.
	void set_input_method(InputMethod p_method) { input_method = p_method; }
	InputMethod get_input_method() const { return input_method; }
	void send_pointer(double p_x, double p_y);
	// X buttons: 1 left, 2 middle, 3 right, 8 back, 9 forward.
	void send_button(int p_button, bool p_pressed);
	// In wheel notches (buttons 4-7); fractions accumulate. Positive dy scrolls down, positive dx right.
	void send_scroll(double p_dx, double p_dy);
	void send_key(int64_t p_keysym, bool p_pressed);
	void send_unicode(int64_t p_codepoint, bool p_pressed);
	void send_text(const String &p_text);
	// A Godot Key (e.g. from an InputEventKey's keycode), mapped to an X keysym.
	void send_godot_key(Key p_key, bool p_pressed);
	// A synthetic FocusIn to the captured window (some toolkits ignore keys while they think they are inactive).
	void send_focus_in();

	// Window management and focus (see docs: gamescope picks the shown window itself).
	int64_t get_own_window() const;
	int64_t get_toplevel(int64_t p_window);
	Error set_input_focus(int64_t p_window);
	Error raise_window(int64_t p_window);
	Error remap_window(int64_t p_window);
	// Gamescope (Steam-controlled focus): gives the window a STEAM_GAME id if it has none and names it in the root's
	// GAMESCOPECTRL_BASELAYER_WINDOW. gamescope_release_focus() removes that property again if we set it.
	Error gamescope_focus(int64_t p_window, int64_t p_app_id = 0);
	Error gamescope_release_focus();
	// Root and focus state: GAMESCOPE_FOCUSED_WINDOW/APP, GAMESCOPE_FOCUSABLE_WINDOWS, GAMESCOPECTRL_BASELAYER_*,
	// _NET_ACTIVE_WINDOW, the X input focus, and our own window.
	Dictionary get_focus_info();
	// Input passthrough: an empty input shape lets pointer events fall through the window (e.g. our own, so XTEST clicks
	// reach the captured window under it). Needs XFixes 2.
	Error set_input_passthrough(int64_t p_window, bool p_enabled);

	// Generic property access, for experiments from scripts. 32-bit types (CARDINAL, WINDOW, ATOM) come back as a
	// PackedInt64Array, strings as a String; missing properties as null.
	Variant get_window_property(int64_t p_window, const String &p_name);
	Error set_window_property(int64_t p_window, const String &p_name, const String &p_type, const PackedInt64Array &p_values);
	Error delete_window_property(int64_t p_window, const String &p_name);
	Error send_client_message(int64_t p_window, const String &p_type, const PackedInt64Array &p_data, int64_t p_destination = 0);
	// Asks the window to close (ICCCM WM_DELETE_WINDOW), as a window manager's close button does.
	Error close_window(int64_t p_window);
	int64_t get_atom(const String &p_name);

	X11WindowSource();
	~X11WindowSource();
};

VARIANT_ENUM_CAST(X11WindowSource::CaptureMode);
VARIANT_ENUM_CAST(X11WindowSource::InputMethod);
