/*
    This file is part of darktable,
    Copyright (C) 2024 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/
#pragma once

#include <glib.h>

G_BEGIN_DECLS

// Initialize: spawn dt-p2p-daemon if a passphrase is configured.
// Call once from dt_init() after prefs are loaded.
void dt_p2p_init(void);

// Tear down: send SIGTERM to daemon and close socket.
void dt_p2p_cleanup(void);

// Push an XMP file to all peers sharing the same passphrase.
// raw_path  – canonical path to the raw file (e.g. /mnt/photos/IMG_1234.CR2)
// xmp_path  – path to the .xmp sidecar to read and broadcast
void dt_p2p_push_xmp(const char *raw_path, const char *xmp_path);

// Ask peers for a proxy sidecar for raw_path.
// The daemon writes the file to raw_path + ".proxy.avif" when found.
// Async: returns immediately; caller can poll for the file.
void dt_p2p_fetch_proxy(const char *raw_path);

// Tell the daemon that a proxy sidecar for raw_path is now available locally.
// The daemon will include it in the manifest so peers can download it.
void dt_p2p_announce_proxy(const char *raw_path);

// Returns TRUE if the P2P daemon is running and connected.
gboolean dt_p2p_is_running(void);

// Tear down the running daemon and restart it with the current darktablerc
// settings.  Blocks for up to ~2 s while the new daemon binds its socket.
// Must be called from the GTK main thread.
void dt_p2p_restart(void);

G_END_DECLS
