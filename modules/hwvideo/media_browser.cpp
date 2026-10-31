/**************************************************************************/
/*  media_browser.cpp                                                     */
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

#include "media_browser.h"

#include "core/config/project_settings.h"
#include "core/object/class_db.h"
#include "core/os/os.h"

#include <dirent.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>

// libsmb2's headers depend on this order.
extern "C" {
// clang-format off
#include <smb2/smb2.h>
#include <smb2/libsmb2.h>
#include <smb2/libsmb2-raw.h>
#include <smb2/libsmb2-dcerpc.h>
#include <smb2/libsmb2-dcerpc-srvsvc.h>
// clang-format on
}

namespace {

struct Entry {
	String name;
	bool is_dir = false;
	int64_t size = 0;
	int64_t mtime = 0;
};

// Folders first, then by name in natural order, ignoring case.
struct EntryOrder {
	bool operator()(const Entry &p_a, const Entry &p_b) const {
		if (p_a.is_dir != p_b.is_dir) {
			return p_a.is_dir;
		}
		return p_a.name.naturalnocasecmp_to(p_b.name) < 0;
	}
};

Array to_array(Vector<Entry> &p_entries) {
	p_entries.sort_custom<EntryOrder>();
	Array out;
	for (const Entry &entry : p_entries) {
		Dictionary d;
		d["name"] = entry.name;
		d["is_dir"] = entry.is_dir;
		d["size"] = entry.size;
		d["mtime"] = entry.mtime;
		out.push_back(d);
	}
	return out;
}

uint64_t deadline(double p_seconds) {
	return OS::get_singleton()->get_ticks_usec() + (uint64_t)(p_seconds * 1e6);
}

} // namespace

void MediaBrowser::set_smb_credentials(const String &p_host, const String &p_user, const String &p_password, const String &p_domain) {
	SmbCredentials::set(p_host, p_user, p_password, p_domain);
}

bool MediaBrowser::_session_for(const String &p_host, const String &p_share) {
	if (session.is_connected() && session.get_host() == p_host && session.get_share() == p_share) {
		return true;
	}
	String error;
	if (!session.connect(p_host, p_share, timeout, error)) {
		last_error = error;
		return false;
	}
	return true;
}

TypedArray<String> MediaBrowser::_list_shares(const String &p_host) {
	TypedArray<String> out;
	const SmbUrl url = SmbUrl::parse(SmbUrl::is_smb(p_host) ? p_host : "smb://" + p_host);
	if (!url.valid) {
		last_error = vformat("Not a host name: %s", p_host);
		return out;
	}
	// Shares are enumerated over DCE/RPC (srvsvc) on the IPC$ share.
	for (int attempt = 0; attempt < 2; attempt++) {
		if (!_session_for(url.host, "IPC$")) {
			return out;
		}
		SmbSession::Result result;
		session.clear_error();
		SmbSession::Wait wait = SmbSession::WAIT_ERROR;
		if (smb2_share_enum_async(session.get_context(), SHARE_INFO_1, SmbSession::result_callback, &result) == 0) {
			wait = session.run(&result, deadline(timeout));
		}
		if (wait == SmbSession::WAIT_ERROR && attempt == 0) {
			session.close(); // A cached connection the server dropped: once more on a new one.
			continue;
		}
		if (wait != SmbSession::WAIT_DONE || result.status != 0 || !result.data) {
			last_error = session.describe_error(result.status, "the list of shares", wait);
			if (last_error.contains("SMB error")) {
				last_error += " Listing shares may not be allowed on this host: type the share name instead.";
			}
			session.close();
			return out;
		}
		struct srvsvc_NetrShareEnum_rep *reply = static_cast<struct srvsvc_NetrShareEnum_rep *>(result.data);
		Vector<String> names;
		const struct srvsvc_SHARE_INFO_1_CONTAINER &container = reply->ses.ShareInfo.Level1;
		for (uint32_t i = 0; container.Buffer && i < container.EntriesRead; i++) {
			const struct srvsvc_SHARE_INFO_1 &info = container.Buffer->share_info_1[i];
			const String name = info.netname.utf8 ? String::utf8(info.netname.utf8) : String();
			// Folders only: no printers, no IPC$, no hidden or administrative shares (C$, ADMIN$).
			if (name.is_empty() || (info.type & 3) != SHARE_TYPE_DISKTREE || (info.type & SHARE_TYPE_HIDDEN) || name.ends_with("$")) {
				continue;
			}
			names.push_back(name);
		}
		smb2_free_data(session.get_context(), reply);
		names.sort_custom<NaturalNoCaseComparator>();
		for (const String &name : names) {
			out.push_back(name);
		}
		return out;
	}
	return out;
}

TypedArray<String> MediaBrowser::list_shares(const String &p_host) {
	MutexLock lock(mutex);
	last_error = String();
	return _list_shares(p_host);
}

Array MediaBrowser::_list_smb(const SmbUrl &p_url) {
	const String what = p_url.share + (p_url.path.is_empty() ? String() : "/" + p_url.path);
	const CharString path = p_url.path.utf8();
	for (int attempt = 0; attempt < 2; attempt++) {
		if (!_session_for(p_url.host, p_url.share)) {
			return Array();
		}
		smb2_context *smb2 = session.get_context();
		SmbSession::Result result;
		session.clear_error();
		SmbSession::Wait wait = SmbSession::WAIT_ERROR;
		if (smb2_opendir_async(smb2, path.get_data(), SmbSession::result_callback, &result) == 0) {
			wait = session.run(&result, deadline(timeout));
		}
		if (attempt == 0 && (wait == SmbSession::WAIT_ERROR || (wait == SmbSession::WAIT_DONE && result.status < 0 && smb2_get_nterror(smb2) == 0))) {
			session.close(); // A cached connection the server dropped: once more on a new one.
			continue;
		}
		if (wait != SmbSession::WAIT_DONE || result.status != 0 || !result.data) {
			last_error = session.describe_error(result.status, what, wait);
			if (wait != SmbSession::WAIT_DONE) {
				session.close();
			}
			return Array();
		}
		struct smb2dir *dir = static_cast<struct smb2dir *>(result.data);
		Vector<Entry> entries;
		struct smb2dirent *ent;
		while ((ent = smb2_readdir(smb2, dir)) != nullptr) {
			Entry entry;
			entry.name = String::utf8(ent->name);
			if (entry.name == "." || entry.name == ".." || (!include_hidden && entry.name.begins_with("."))) {
				continue;
			}
			entry.is_dir = ent->st.smb2_type == SMB2_TYPE_DIRECTORY;
			entry.size = entry.is_dir ? 0 : (int64_t)ent->st.smb2_size;
			entry.mtime = (int64_t)ent->st.smb2_mtime;
			entries.push_back(entry);
		}
		smb2_closedir(smb2, dir);
		return to_array(entries);
	}
	return Array();
}

Array MediaBrowser::_list_local(const String &p_path) {
	const String path = ProjectSettings::get_singleton() ? ProjectSettings::get_singleton()->globalize_path(p_path) : p_path;
	DIR *dir = opendir(path.utf8().get_data());
	if (!dir) {
		const int error_number = errno;
		switch (error_number) {
			case ENOENT:
				last_error = vformat("Not found: %s.", path);
				break;
			case EACCES:
				last_error = vformat("Access denied: %s.", path);
				break;
			case ENOTDIR:
				last_error = vformat("Not a folder: %s.", path);
				break;
			default:
				last_error = vformat("Can't open %s: %s.", path, String::utf8(strerror(error_number)));
				break;
		}
		return Array();
	}
	Vector<Entry> entries;
	struct dirent *ent;
	while ((ent = readdir(dir)) != nullptr) {
		Entry entry;
		entry.name = String::utf8(ent->d_name);
		if (entry.name == "." || entry.name == ".." || (!include_hidden && entry.name.begins_with("."))) {
			continue;
		}
		struct stat st = {};
		if (fstatat(dirfd(dir), ent->d_name, &st, 0) != 0) {
			continue; // A broken link.
		}
		entry.is_dir = S_ISDIR(st.st_mode);
		entry.size = entry.is_dir ? 0 : (int64_t)st.st_size;
		entry.mtime = (int64_t)st.st_mtime;
		entries.push_back(entry);
	}
	closedir(dir);
	return to_array(entries);
}

Array MediaBrowser::list_dir(const String &p_path) {
	MutexLock lock(mutex);
	last_error = String();
	if (!SmbUrl::is_smb(p_path)) {
		return _list_local(p_path);
	}
	const SmbUrl url = SmbUrl::parse(p_path);
	if (!url.valid) {
		last_error = vformat("Not an smb://host/share/folder path: %s", p_path);
		return Array();
	}
	if (url.share.is_empty()) {
		// smb://host: its shares, as folders.
		const TypedArray<String> shares = _list_shares(url.host);
		Vector<Entry> entries;
		for (int i = 0; i < shares.size(); i++) {
			Entry entry;
			entry.name = shares[i];
			entry.is_dir = true;
			entries.push_back(entry);
		}
		return to_array(entries);
	}
	return _list_smb(url);
}

String MediaBrowser::get_last_error() const {
	MutexLock lock(mutex);
	return last_error;
}

void MediaBrowser::disconnect_host() {
	MutexLock lock(mutex);
	session.close();
}

void MediaBrowser::set_timeout(double p_seconds) {
	MutexLock lock(mutex);
	timeout = CLAMP(p_seconds, 1.0, 120.0);
}

void MediaBrowser::set_include_hidden(bool p_include) {
	MutexLock lock(mutex);
	include_hidden = p_include;
}

MediaBrowser::~MediaBrowser() {
	session.close();
}

void MediaBrowser::_bind_methods() {
	ClassDB::bind_static_method("MediaBrowser", D_METHOD("set_smb_credentials", "host", "user", "password", "domain"), &MediaBrowser::set_smb_credentials, DEFVAL(String()));
	ClassDB::bind_method(D_METHOD("list_shares", "host"), &MediaBrowser::list_shares);
	ClassDB::bind_method(D_METHOD("list_dir", "path"), &MediaBrowser::list_dir);
	ClassDB::bind_method(D_METHOD("get_last_error"), &MediaBrowser::get_last_error);
	ClassDB::bind_method(D_METHOD("disconnect_host"), &MediaBrowser::disconnect_host);
	ClassDB::bind_method(D_METHOD("set_timeout", "seconds"), &MediaBrowser::set_timeout);
	ClassDB::bind_method(D_METHOD("get_timeout"), &MediaBrowser::get_timeout);
	ClassDB::bind_method(D_METHOD("set_include_hidden", "include"), &MediaBrowser::set_include_hidden);
	ClassDB::bind_method(D_METHOD("get_include_hidden"), &MediaBrowser::get_include_hidden);

	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "timeout", PROPERTY_HINT_RANGE, "1,120,0.5,suffix:s"), "set_timeout", "get_timeout");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "include_hidden"), "set_include_hidden", "get_include_hidden");
}
