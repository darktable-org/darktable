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

// rebuilding a harvested edit (see harvest.h) into darktable structures,
// shared by every tool that reads a harvest file, so that all read the format
// through the same code: a second reader could disagree with the first. Also
// the plumbing those tools share around it: opening the edits, reusing a
// verdict for a repeat, writing the report

#include "develop/blend.h"
#include "develop/masks.h"

#include <glib.h>
#include <json-glib/json-glib.h>
#include <stdio.h>

G_BEGIN_DECLS

/** load a harvest file into a JsonParser, decompressing it when it is
    gzipped, as the copy contributors are asked to send is. The magic number
    decides, not the extension. NULL on failure, with `error` set; the caller
    owns the parser */
JsonParser *dt_masks_harvest_load(const char *path, GError **error);

/** the "edits" array of the harvest file at `json_path`, loaded into
    `*parser`, which the caller owns. NULL, with no parser to free, after
    saying why on stderr, prefixed with `[tag]` */
JsonArray *dt_masks_harvest_open_edits(const char *json_path,
                                       const char *tag,
                                       JsonParser **parser);

/** run a check over the harvest file at `json_path` and write its report, the
    one `section` writes, as a JSON object of its own to `report_path`; no
    report when that is NULL. Returns what `section` does */
gboolean dt_masks_harvest_report(const char *json_path,
                                 const char *report_path,
                                 const char *tag,
                                 gboolean (*section)(const char *json_path, FILE *rf));

/** keep a copy of the `size` bytes at `rep` in `seen` under `key`, for an
    exact repeat of the edit to reuse (see dt_masks_harvest_edit_key). `seen`
    takes `key` and frees both with g_free. A NULL `key` keeps nothing */
void dt_masks_harvest_remember(GHashTable *seen,
                               gchar *key,
                               const void *rep,
                               const size_t size);

/* in a check's loop over the edits: count edit `i` as skipped, for `why`,
   record that in the report `rf`, and go on to the next edit. Uses the loop's
   `skipped`, `rf`, `first_report` and `i`.

   A plain block, not the usual do{...}while(0): `continue` binds to the
   nearest enclosing loop, and do/while(0) is one, so the skip would fall
   through into the code it exists to avoid, and a skipped edit would be
   judged too. So never follow it with an `else`; a skip ends an iteration, so
   there is nothing for an `else` to do */
#define DT_MASKS_HARVEST_SKIP(why)                                              \
  {                                                                             \
    skipped++;                                                                  \
    if(rf)                                                                      \
    {                                                                           \
      fprintf(rf, "%s\n    {\"index\": %u, \"result\": \"skipped\","             \
                  " \"reason\": \"%s\"}", first_report ? "" : ",", i, (why));   \
      first_report = FALSE;                                                     \
    }                                                                           \
    continue;                                                                   \
  }

/** a classic edit of a harvest, as the checks that migrate it read it */
typedef struct dt_masks_harvest_edit_t
{
  dt_develop_blend_params_t bp;
  GList *forms;          // the caller owns them: free with dt_masks_free_form
  const char *operation; // NULL when the edit names none; the parser owns it
  int multi_priority;
  int blendop_version;
  int width, height;     // the harvested image's
} dt_masks_harvest_edit_t;

/** read a harvested edit into `e`. NULL on success, else why it cannot be
    replayed, with nothing to free. An edit already flexi is one of those: it
    was never migrated */
const char *dt_masks_harvest_read_classic_edit(JsonObject *edit,
                                               dt_masks_harvest_edit_t *e);

/** a key for everything a replay of one harvested edit depends on: the
    module, the blend params, the forms and the image size, but not its place
    in the harvest. A preset or a copied history stores the same mask on many
    images, and a repeat rendered the same way cannot render differently, so
    the checks replay each distinct edit once and count its repeats. Keyed on
    content, not on the configuration's shape: edits of one shape can render
    differently, so sampling by shape could miss one. The caller owns the
    string */
gchar *dt_masks_harvest_edit_key(JsonObject *edit);

/** member `k` of `o`; `dflt` if `o` is NULL or the member is absent, null or
    not a plain value */
gint64 dt_masks_harvest_obj_int(JsonObject *o, const char *k, const gint64 dflt);
float dt_masks_harvest_obj_float(JsonObject *o, const char *k, const float dflt);
const char *dt_masks_harvest_obj_str(JsonObject *o, const char *k, const char *dflt);

/** rebuild the forms of one harvested edit from its "forms" array. NULL if
    anything cannot be rebuilt, so that a malformed record is skipped rather
    than replayed as something else. The caller owns the list (free it with
    dt_masks_free_form) */
GList *dt_masks_harvest_read_forms(JsonArray *forms_arr);

/** rebuild the blend params of one harvested edit from its "blend" object.
    `p` is zeroed first, so absent members are 0 */
void dt_masks_harvest_read_blend_params(JsonObject *b,
                                        dt_develop_blend_params_t *p);

G_END_DECLS

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
