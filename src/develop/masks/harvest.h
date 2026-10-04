/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

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

// Mask harvesting: export every mask configuration in a library to a JSON
// file, so that the migration to flexi masks can be checked against real
// edits, not only against invented ones.
//
// `darktable --harvest-masks out.json` reads the library --library and
// --configdir name. `darktable --harvest-masks-xmp DIR out.json` reads the XMP
// sidecars under DIR instead, for photographers who keep their edits there and
// treat the library as a rebuildable index: a library harvest would miss their
// masks. Both write the same format and keep both promises below.
//
// 1. It is strictly read-only on the user's library. A normal startup opens
//    the library read-write, locks it, and may upgrade its schema, so
//    harvesting does not use darktable's database handle at all: it runs
//    before dt_database_init(), opens its own read-only connection, and exits
//    without the rest of startup. The sidecar harvest parses the XML itself:
//    dt_exif_xmp_read() needs the whole startup.
//
// 2. Nothing in the output identifies the user, their files or their
//    subjects: users are asked to send this file. It is plain JSON with every
//    value in a named field, no base64 or blobs, so that anyone can read what
//    it holds: numbers and module names. Free text hides in three places, all
//    stripped:
//
//      - file names and film roll paths: never queried
//      - masks_history.name, which the user can set to anything: dropped, the
//        reader makes a name from the type
//      - dt_masks_point_group_t.name, a group name inside the points blob,
//        which is why the blobs are decoded rather than copied
//
//    An image is reduced to its width and height: masks use normalized
//    coordinates, and the verifier renders them on a generated probe (see
//    probe_image.h). Image ids are renumbered.

#include <glib.h>

G_BEGIN_DECLS

/** harvest every history entry with a mask in the library at `library_path`
    into a JSON file at `output_path`. It never writes to, locks or upgrades
    the library. Progress and a summary go to stdout, so that the user can see
    what was collected before deciding to share it. TRUE on success */
gboolean dt_masks_harvest_library(const char *library_path,
                                  const char *output_path);

/** the same from the XMP sidecars found under `dir`, recursively */
gboolean dt_masks_harvest_xmp_dir(const char *dir, const char *output_path);

G_END_DECLS

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
