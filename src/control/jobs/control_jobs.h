/*
    This file is part of darktable,
    Copyright (C) 2010-2025 darktable developers.

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

#include "control/control.h"
#include "imageio/imageio_module.h"
#include <inttypes.h>

#ifdef HAVE_PRINT
#include "common/cups_print.h"
#endif

G_BEGIN_DECLS

void dt_control_gpx_apply(const gchar *filename, const int32_t filmid, const gchar *tz, GList *imgs);

void dt_control_datetime(const GTimeSpan offset, const char *datetime, GList *imgs);

void dt_control_write_sidecar_files(void);
void dt_control_delete_images(void);
void dt_control_delete_duplicate(const dt_imgid_t imgid);
void dt_control_duplicate_images(const gboolean virgin);
void dt_control_flip_images(const int32_t cw);
void dt_control_monochrome_images(const int32_t mode);
gboolean dt_control_remove_images(void);
void dt_control_move_images(void);
void dt_control_copy_images(void);
void dt_control_compress_history(GList *imgs);
void dt_control_discard_history(GList *imgs);
void dt_control_paste_history(GList *imgs);
void dt_control_paste_parts_history(GList *imgs);
void dt_control_apply_styles(GList *imgs, GList *styles, const gboolean duplicate);
void dt_control_set_local_copy_images(void);
void dt_control_reset_local_copy_images(void);
void dt_control_export(GList *imgid_list,
                       const int max_width,
                       const int max_height,
                       const int format_index,
                       const int storage_index,
                       const gboolean high_quality,
                       const gboolean upscale,
                       const gboolean dimensions_scale,
                       const gboolean is_scaling,
                       const double scale_factor,
                       const gboolean export_masks,
                       char *style,
                       const gboolean style_append,
                       const dt_colorspaces_color_profile_type_t icc_type,
                       const gchar *icc_filename,
                       const dt_iop_color_intent_t icc_intent,
                       const gchar *metadata_export);
void dt_control_merge_hdr(void);
void dt_control_import(GList *imgs, const char *datetime_override, const gboolean inplace);
void dt_control_refresh_exif(void);

G_END_DECLS

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
