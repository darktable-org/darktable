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

#include <glib.h>
#include <stdint.h>

/* Locating and, on request, fetching the spektrafilm spectral data pack.
 *
 * A pack is a directory holding pack.json, spectra_lut.f32 and a profiles
 * subdirectory of per-stock JSON, which is what sf_pack_load() consumes. Two
 * places can hold one:
 *
 *   <user data>/darktable/spektrafilm/                  hand-installed, user-managed
 *   <user data>/darktable/spektrafilm/packs/<lut_hash>/ downloaded, one per spectral table
 *
 * The top level always wins when it can satisfy the request, so a user who
 * exports a pack themselves with tools/spektrafilm_export_data.py never has a
 * download silently override it. Downloads only ever write inside packs/,
 * which is what keeps the two apart while leaving both under one directory
 * shared by every darktable instance on the machine.
 *
 * Packs are identified by the 32-bit hash of the spectral upsampling table they
 * carry (sf_pack_lut_hash / the lut_hash recorded in every edit's params).
 * Different spektrafilm releases revise that table and each revision renders
 * differently, so the hash -- not a version string -- is the thing an edit needs
 * matched. An editable dev install reports whatever pyproject.toml happens to
 * say, so two materially different checkouts can claim the same version.
 *
 * Nothing here uses git. The remote is a plain HTTPS file tree served out of a
 * git forge, read with libcurl, which is already a hard darktable dependency.
 */

/* ------------------------------------------------------------------ setup -- */

void sf_fetch_init(void);
void sf_fetch_cleanup(void);

/* -------------------------------------------------------------- resolving -- */

/* Pick the pack directory to hand to sf_pack_load().
 *
 * Purely local: stats a few files and reads at most 288 bytes of each candidate
 * LUT header. It never opens a socket and never blocks on one, so it is safe to
 * call from the pixelpipe.
 *
 * wanted_lut_hash is the hash the edit recorded, or 0 for "no preference"
 * (a new edit, or one made before that field existed). With 0 the config
 * directory is taken whatever table it carries; with a specific hash the config
 * directory is only taken if it matches, then the download cache is searched.
 *
 * Returns TRUE and fills dst when some pack is usable. When it returns FALSE
 * there is nothing installed at all, and the caller should offer a download.
 *
 * out_exact, when non-NULL, reports whether the returned directory actually
 * carries wanted_lut_hash. FALSE there means a pack was found but it is the
 * wrong one -- render with it anyway rather than showing the user a black
 * frame, and let the existing mismatch warning explain the difference. */
gboolean sf_fetch_resolve_pack_dir(uint32_t wanted_lut_hash,
                                   char *dst,
                                   size_t dstsz,
                                   gboolean *out_exact);

/* TRUE when a pack carrying this exact table is already on disk. */
gboolean sf_fetch_have_lut_hash(uint32_t lut_hash);

/* Bumped every time a download changes what is on disk.
 *
 * A caller that caches a loaded pack cannot detect a new one by watching the
 * resolved directory alone: a download can land in a directory that was
 * already probed and found wanting, leaving the path identical and the cached
 * failure in place. Comparing this counter instead catches that case, which is
 * precisely the one the download exists to resolve. Starts at 0 and only ever
 * increases. Safe to call from any thread. */
guint sf_fetch_generation(void);

/* ------------------------------------------------------------ downloading -- */

typedef enum sf_fetch_state_t
{
  SF_FETCH_IDLE = 0,
  SF_FETCH_RUNNING,
  SF_FETCH_DONE,
  SF_FETCH_FAILED,
} sf_fetch_state_t;

/* Start a background download.
 *
 * wanted_lut_hash selects the manifest entry to install; 0 asks for whichever
 * entry the manifest marks as default. Returns FALSE without doing anything if
 * a fetch is already running, if downloads are disabled in preferences, or if
 * the configured repository is malformed.
 *
 * The work happens on its own thread. On success the pack lands under
 * <cache>/spektrafilm/packs/<lut_hash>/ and the developed pixelpipe is
 * reprocessed so the new data takes effect without the user reopening the
 * image. Call from the GUI thread. */
gboolean sf_fetch_start(uint32_t wanted_lut_hash);

/* Ask a running fetch to stop. Returns once the flag is set, not once the
 * thread has finished; the partially downloaded files are discarded. */
void sf_fetch_cancel(void);

/* Current state, plus a short human-readable message and 0..1 progress.
 * msg and progress may be NULL. Safe to call from any thread. */
sf_fetch_state_t sf_fetch_status(char *msg,
                                 size_t msgsz,
                                 double *progress);

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
