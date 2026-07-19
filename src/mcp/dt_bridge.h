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

// the only translation unit that calls into libdarktable. everything here is
// plain C (+ json-glib for value marshalling) so the transport and tool layers
// never touch darktable internals directly

#pragma once

#include <glib.h>
#include <stdint.h>

// the *_json helpers return a newly-allocated string (free with g_free), or
// NULL on error with *err set to a g_malloc'd message

/** JSON array of [{ operation, version, have_introspection, doc_url }] */
char *dt_bridge_list_modules_json(void);

/** JSON description of a module's parameter layout (fields, ranges, doc_url) */
char *dt_bridge_module_schema_json(const char *op, char **err);

/** decode a hex op_params blob into a JSON { field: value } object */
char *dt_bridge_decode_params_json(const char *op, const char *blob_hex, char **err);

/** seed a module's defaults, apply `fields` (a JsonObject*, void* to keep this
    header light), and return a hex op_params blob */
char *dt_bridge_encode_params_hex(const char *op, void *fields_jsonobject, char **err);

/** render a raw (by path or imgid) through the base pipeline plus an optional
    module `stack` (JsonArray*), on a throwaway duplicate so the source is
    untouched; on success hands back a g_malloc'd PNG in png_out / png_len */
gboolean dt_bridge_render_png(const char *path, int imgid_in, int width, int height,
                              void *stack_jsonarray, gboolean disable_tone_mappers,
                              int history_end, uint8_t **png_out, size_t *png_len,
                              char **err);

/** like render, but returns per-channel statistics as JSON instead of a PNG */
char *dt_bridge_image_stats_json(const char *path, int imgid_in, int width, int height,
                                 void *stack_jsonarray, gboolean disable_tone_mappers,
                                 int history_end, char **err);

// --- library (catalog) tools ---

/** JSON array of { imgid, path } for images in the current library */
char *dt_bridge_list_images_json(int limit);

/** JSON array of the decoded edit-history stack of `imgid` */
char *dt_bridge_get_history_json(int imgid, char **err);

/** JSON array of saved styles ({ name, description }) */
char *dt_bridge_list_styles_json(void);

gboolean dt_bridge_apply_style(const char *name, int imgid, gboolean overwrite,
                               char **err);
gboolean dt_bridge_save_style(const char *name, const char *description,
                              int imgid, char **err);
gboolean dt_bridge_import_style(const char *path, char **err);

/** develop (with optional stack) and write a PNG to out_path */
gboolean dt_bridge_export_png(const char *in_path, int imgid_in, int width, int height,
                              void *stack_jsonarray, gboolean disable_tone_mappers,
                              int history_end, const char *out_path, char **err);
