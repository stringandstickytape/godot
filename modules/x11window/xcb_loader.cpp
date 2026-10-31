/**************************************************************************/
/*  xcb_loader.cpp                                                        */
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

#include "xcb_loader.h"

#include "core/os/mutex.h"
#include "core/string/print_string.h"
#include "core/variant/variant.h"

#include <dlfcn.h>

static const char *library_names[XCBLoader::LIB_MAX] = {
	"libxcb.so.1",
	"libxcb-composite.so.0",
	"libxcb-damage.so.0",
	"libxcb-xfixes.so.0",
	"libxcb-xtest.so.0",
	"libxcb-dri3.so.0",
	"libxcb-shm.so.0",
};

Error XCBLoader::load(String &r_error) {
	for (int i = 0; i < LIB_MAX; i++) {
		handles[i] = dlopen(library_names[i], RTLD_NOW | RTLD_LOCAL);
		library_loaded[i] = handles[i] != nullptr;
		if (!handles[i]) {
			print_verbose(vformat("X11WindowSource: can't load %s: %s", library_names[i], dlerror()));
		}
	}
	if (!library_loaded[LIB_XCB]) {
		r_error = "libxcb.so.1 can't be loaded.";
		return ERR_CANT_OPEN;
	}

	// A missing symbol marks its whole library as unavailable.
#define XCB_LOAD_SYMBOL(m_lib, m_name)                                                                \
	if (library_loaded[m_lib]) {                                                                      \
		void *symbol = dlsym(handles[m_lib], #m_name);                                                \
		if (symbol == nullptr) {                                                                      \
			print_line(vformat("X11WindowSource: %s has no %s.", library_names[m_lib], #m_name));     \
			library_loaded[m_lib] = false;                                                            \
		}                                                                                             \
		m_name = reinterpret_cast<decltype(m_name)>(symbol);                                          \
	}
	XCB_CORE_FUNCTIONS(XCB_LOAD_SYMBOL)
	XCB_EXTENSION_FUNCTIONS(XCB_LOAD_SYMBOL)
	XCB_EXTENSION_IDS(XCB_LOAD_SYMBOL)
#undef XCB_LOAD_SYMBOL

	if (!library_loaded[LIB_XCB]) {
		r_error = "libxcb.so.1 lacks a needed function.";
		return ERR_CANT_OPEN;
	}
	// Damage events and regions come with XFixes; Composite's header depends on it too.
	if (!library_loaded[LIB_XFIXES]) {
		library_loaded[LIB_DAMAGE] = false;
	}
	return OK;
}

XCBLoader *XCBLoader::get_singleton(String &r_error) {
	// A plain static, not memnew: it lives until the process exits (the libraries stay loaded).
	static Mutex mutex;
	static XCBLoader instance;
	static bool tried = false;
	static bool loaded = false;
	static String load_error;

	MutexLock lock(mutex);
	if (!tried) {
		tried = true;
		loaded = instance.load(load_error) == OK;
	}
	r_error = load_error;
	return loaded ? &instance : nullptr;
}
