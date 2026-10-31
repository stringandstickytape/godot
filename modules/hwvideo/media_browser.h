/**************************************************************************/
/*  media_browser.h                                                       */
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

#include "smb_client.h"

#include "core/object/ref_counted.h"
#include "core/variant/typed_array.h"

// Lists SMB shares and folders (and local folders) for a file browser. Every call blocks (network round trips, up to
// `timeout` seconds): call it from a worker thread. One connection per object is kept and reused while the host and
// share stay the same.
class MediaBrowser : public RefCounted {
	GDCLASS(MediaBrowser, RefCounted);

	mutable BinaryMutex mutex;
	SmbSession session;
	String last_error;
	double timeout = 8.0;
	bool include_hidden = false;

	bool _session_for(const String &p_host, const String &p_share);
	Array _list_smb(const SmbUrl &p_url);
	Array _list_local(const String &p_path);
	TypedArray<String> _list_shares(const String &p_host);

protected:
	static void _bind_methods();

public:
	static void set_smb_credentials(const String &p_host, const String &p_user, const String &p_password, const String &p_domain = String());

	TypedArray<String> list_shares(const String &p_host);
	Array list_dir(const String &p_path);
	String get_last_error() const;
	void disconnect_host();

	void set_timeout(double p_seconds);
	double get_timeout() const { return timeout; }
	void set_include_hidden(bool p_include);
	bool get_include_hidden() const { return include_hidden; }

	~MediaBrowser();
};
