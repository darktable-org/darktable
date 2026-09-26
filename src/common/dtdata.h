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

#include "common/image.h"
#include <glib.h>
#include <stdint.h>

G_BEGIN_DECLS

/* the .dtdata sidecar: a flat zip next to the .xmp, same name with the
   extension swapped, holding one PNG per raster entry. entries are named
   "<kind>-<sha1 of the file>.png" and never modified; history rows point
   at them through dt_dtdata_ref_t. see
   https://github.com/darktable-org/darktable/issues/22226 */

#define DT_DTDATA_EXT ".dtdata"
#define DT_DTDATA_ENTRY_LEN 64
#define DT_DTDATA_PRODUCER_LEN 64

typedef enum dt_dtdata_kind_t
{
  DT_DTDATA_KIND_MASK = 0,   // alpha mask, 8-bit gray
  DT_DTDATA_KIND_DEPTH = 1,  // depth map, 16-bit gray
  DT_DTDATA_KIND_BRUSH = 2,  // painted layer, 8-bit gray
  DT_DTDATA_KIND_PATCH = 3,  // inpainted pixels
} dt_dtdata_kind_t;

typedef enum dt_dtdata_origin_t
{
  DT_DTDATA_ORIGIN_REGENERABLE = 0,    // a model made it from the image
  DT_DTDATA_ORIGIN_AUTHORITATIVE = 1,  // a person made it, cannot be recomputed
} dt_dtdata_origin_t;

/* serialized as-is inside module params and mask blobs: fixed layout,
   no pointers, no padding */
typedef struct dt_dtdata_ref_t
{
  char entry[DT_DTDATA_ENTRY_LEN];  // "<kind>-<sha1>.png", empty = no reference
  int32_t kind;                     // dt_dtdata_kind_t
  int32_t origin;                   // dt_dtdata_origin_t
  int32_t width;
  int32_t height;
  int32_t bpc;                      // 8 or 16
  char producer[DT_DTDATA_PRODUCER_LEN];  // model id and version for regenerable entries
} dt_dtdata_ref_t;

/** FALSE when sidecar writing is set to "never": the feature is then
    unavailable rather than falling back to the database */
gboolean dt_dtdata_enabled(void);

/** path of the sidecar for an image, next to its xmp */
void dt_dtdata_path(const dt_imgid_t imgid, char *path, const size_t len);

/** the same for an image path that already carries the duplicate suffix,
    for callers that move or copy files without going through the db */
void dt_dtdata_path_for_image(const char *versioned_image_path,
                              char *path,
                              const size_t len);

/** store a [0,1] mask as a gray PNG entry. bpc is 8 or 16. fills *ref on
    success. an identical entry already in the file is reused, not
    duplicated */
gboolean dt_dtdata_write_gray(const dt_imgid_t imgid,
                              const dt_dtdata_kind_t kind,
                              const dt_dtdata_origin_t origin,
                              const char *producer,
                              const float *mask,
                              const int width,
                              const int height,
                              const int bpc,
                              dt_dtdata_ref_t *ref);

/** read an entry back as a [0,1] float mask, caller frees with
    dt_free_align(). NULL if the file or entry is missing or its checksum
    does not match its name */
float *dt_dtdata_read_gray(const dt_imgid_t imgid,
                           const dt_dtdata_ref_t *ref,
                           int *width,
                           int *height);

/** entry names in the image's sidecar, without "version". a GList of
    strings, free with g_list_free_full(list, g_free) */
GList *dt_dtdata_list_entries(const dt_imgid_t imgid);

/** the kind an entry name announces, or -1 if it is not one we know */
int dt_dtdata_entry_kind(const char *entry);

/** copy every entry of one image's sidecar into another's, keeping what
    the destination already has. used when history travels between
    images, since references are copied verbatim with it. a missing
    source is not an error */
gboolean dt_dtdata_merge(const dt_imgid_t src_imgid, const dt_imgid_t dst_imgid);

/** a producer's params reader for the sweep: 1 and *entry filled when
    the blob references an entry, 0 when it does not, -1 when the blob
    cannot be read (a version this build does not know). kinds is the
    bit set of entry kinds the producer writes, 1 << dt_dtdata_kind_t */
typedef int (*dt_dtdata_scan_fn)(const void *params,
                                 const size_t size,
                                 const int version,
                                 char *entry,
                                 const size_t len);
void dt_dtdata_register_scanner(const char *op,
                                const uint32_t kinds,
                                dt_dtdata_scan_fn fn);

/** drop every entry of a registered kind that no row of the image's
    stored history references, deleting the file when nothing is left.
    call it only once the history is final: after the darkroom wrote it
    on leave, or from a lighttable action without undo. an unreadable
    params blob leaves the file alone */
void dt_dtdata_sweep(const dt_imgid_t imgid);

/** every sidecar of an image file and its duplicates, "<name>.<ext>.dtdata"
    and "<name>_<digits>.<ext>.dtdata" beside it, whether or not an xmp
    exists. a GList of paths, free with g_list_free_full(list, g_free) */
GList *dt_dtdata_find_all(const char *image_path);

/* file-level primitives, on explicit sidecar paths. the imgid functions
   above resolve the path and call these; tests call them directly */
gboolean dt_dtdata_file_write_gray(const char *path,
                                   const dt_dtdata_kind_t kind,
                                   const dt_dtdata_origin_t origin,
                                   const char *producer,
                                   const float *mask,
                                   const int width,
                                   const int height,
                                   const int bpc,
                                   dt_dtdata_ref_t *ref);
float *dt_dtdata_file_read_gray(const char *path,
                                const dt_dtdata_ref_t *ref,
                                int *width,
                                int *height);
GList *dt_dtdata_file_list_entries(const char *path);
gboolean dt_dtdata_file_merge(const char *src_path, const char *dst_path);
gboolean dt_dtdata_file_sweep(const char *path,
                              const GList *keep,
                              const uint32_t kinds);

G_END_DECLS

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
