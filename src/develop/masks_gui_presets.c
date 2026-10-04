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

// group-layout presets for the flexi masks panel: capture a mask's group
// skeleton, store it in the presets database under a fake operation name, and
// apply it back onto a module; and the built-in layouts, read from JSON, with
// their per-group notes and the default layout a mask starts with. It shares
// only the symbols in blend_gui_internal.h with the rest of the panel.

#include "develop/blend_gui_internal.h"

#include "common/darktable.h"
#include "common/debug.h"
#include "common/file_location.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "gui/gtk.h"

#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <sqlite3.h>

// ---- group-layout presets --------------------------------------------------
// a "layout" is the skeleton of a flexi mask: the mask's own group and the
// groups nested in it, each with its flexi operator, name and opacity
// -- nothing else: no shapes, no channel or raster elements. Captured and
// applied as an array of _flexi_layout_node_t in pre-order, the mask's own
// group first and every group's nested groups bottom-up after it, each naming
// its holder by index.
//
// stored in the presets table under a fake operation name no module
// registers, so that they are shared by every module's panel. Not through
// gui/presets.c, which applies a preset by overwriting a module's params; a
// layout touches only the mask's groups
#define FLEXI_GROUP_PRESET_OP "flexi_mask_groups"
#define FLEXI_GROUP_PRESET_VERSION 1

// one group of a layout. Written to the database verbatim, as one blob of
// these, so every field is fixed-size
typedef struct _flexi_layout_node_t
{
  // flexi operator bits (DT_MASKS_STATE_FLEXI_OP)
  dt_masks_state_t flexi_op;
  float opacity;
  // same width as dt_masks_point_group_t.name, the group's marker's name.
  // Unused for the mask's own group, which is always "whole mask"
  char name[128];
  // index of the holding group's node, -1 for the mask's own group
  int32_t parent;
} _flexi_layout_node_t;

// the groups nested in `list`'s groups, as nodes under `parent`, bottom-up
static void _flexi_layout_capture_nested(GArray *out,
                                         const dt_masks_form_t *list,
                                         const int32_t parent,
                                         const int depth)
{
  if(!list || depth > DT_MASKS_NESTING_MAX) return;
  for(const GList *l = list->points; l; l = g_list_next(l))
  {
    const dt_masks_point_group_t *pt = l->data;
    if(dt_masks_point_is_marker(pt)) continue;
    const dt_masks_form_t *sub = dt_masks_get_from_id(darktable.develop, pt->formid);
    if(!sub || sub == list || !(sub->type & DT_MASKS_GROUP)) continue;
    // a nested group is one group: its marker heads its list
    const dt_masks_point_group_t *marker = sub->points ? sub->points->data : NULL;
    if(!marker || !dt_masks_point_is_marker(marker)) continue;
    _flexi_layout_node_t node = { .flexi_op = marker->state & DT_MASKS_STATE_FLEXI_OP,
                                  .opacity = marker->group_opacity,
                                  .parent = parent };
    dt_strlcpy_fixed_to_fixed(node.name, sizeof(node.name), marker->name, sizeof(marker->name));
    g_array_append_val(out, node);
    _flexi_layout_capture_nested(out, sub, (int32_t)out->len - 1, depth + 1);
  }
}

// the module's group skeleton, in pre-order (see _flexi_layout_node_t).
// Caller frees the returned array
static _flexi_layout_node_t *_flexi_layout_capture(dt_iop_module_t *module, int *n_out)
{
  dt_masks_form_t *grp = dt_masks_gui_module_mask_group(module);
  GArray *out = g_array_new(FALSE, FALSE, sizeof(_flexi_layout_node_t));

  // a mask with no group form yet has the one group the panel shows for it
  const dt_masks_point_group_t *root =
    grp && grp->points && dt_masks_point_is_marker(grp->points->data) ? grp->points->data
                                                                       : NULL;
  const _flexi_layout_node_t node = {
    .flexi_op = root ? root->state & DT_MASKS_STATE_FLEXI_OP : 0,
    .opacity = root ? root->group_opacity : 1.0f,
    .parent = -1
  };
  g_array_append_val(out, node);
  _flexi_layout_capture_nested(out, grp, 0, 1);

  *n_out = out->len;
  return (_flexi_layout_node_t *)g_array_free(out, FALSE);
}

// replaces the module's whole mask -- elements and groups alike -- with the
// empty groups of the layout `nodes`, each given its note key from `keys`
// (a built-in's; NULL for a user preset, which has no notes). Never asks for
// confirmation itself; callers that might be discarding elements confirm
// first (see _flexi_preset_apply_confirmed).
static void _flexi_layout_apply(dt_iop_module_t *module,
                                const _flexi_layout_node_t *nodes,
                                const int n,
                                const gchar *const *keys)
{
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  dt_masks_gui_reset_mask_core(module);
  dt_mask_id_t root_cid = INVALID_MASKID;
  dt_masks_form_t *grp = dt_masks_gui_module_flexi_group(module, &root_cid);
  if(!grp || n <= 0 || nodes[0].parent != -1) return;
  // the reset left the mask's own group, empty: the layout's first node
  dt_masks_point_group_t *root = grp->points ? grp->points->data : NULL;
  if(!root || !dt_masks_point_is_marker(root)) return;
  root->state = (root->state & ~DT_MASKS_STATE_FLEXI_OP)
                | (nodes[0].flexi_op & DT_MASKS_STATE_FLEXI_OP);
  root->group_opacity = nodes[0].opacity;
  dt_strlcpy_to_fixed(root->preset_note, keys ? keys[0] : "", sizeof(root->preset_note));

  dt_mask_id_t *cids = g_new(dt_mask_id_t, n);
  cids[0] = root->formid;
  dt_mask_id_t first_nested = INVALID_MASKID;
  for(int i = 1; i < n; i++)
  {
    cids[i] = INVALID_MASKID;
    // pre-order: a holder always comes before what it holds
    const int32_t parent = nodes[i].parent;
    if(parent < 0 || parent >= i || !dt_is_valid_maskid(cids[parent])) continue;
    cids[i] = dt_masks_model_nest_new_group(grp, nodes[i].flexi_op, cids[parent]);
    dt_masks_point_group_t *marker = dt_masks_gui_group_point(grp, cids[i]);
    if(!marker) continue;
    marker->group_opacity = nodes[i].opacity;
    dt_strlcpy_to_fixed(marker->name, nodes[i].name, sizeof(marker->name));
    dt_strlcpy_to_fixed(marker->preset_note, keys ? keys[i] : "", sizeof(marker->preset_note));
    if(!dt_is_valid_maskid(first_nested)) first_nested = cids[i];
  }
  g_free(cids);

  // start where the next element most likely goes: the bottom nested group,
  // or the mask's own group when the layout nests none
  bd->panel_selected_formid = INVALID_MASKID;
  bd->panel_selected_group_cid =
    dt_is_valid_maskid(first_nested) ? first_nested : root->formid;
  // every note opens, to show what the new layout is for (see
  // _group_note_is_open); switches set on the old groups are gone with them
  bd->masks_notes_all_open = TRUE;
  if(bd->masks_note_open) g_hash_table_remove_all(bd->masks_note_open);
  dt_dev_add_masks_history_item(darktable.develop, module, TRUE);
  dt_masks_gui_build_list(module);
  dt_masks_gui_refresh_canvas_edit(module);
}

// does `list` hold, at any depth, an element an applied layout would discard?
// Its nested groups themselves are not: the layout replaces those anyway
static gboolean _flexi_list_has_content(const dt_masks_form_t *list, const int depth)
{
  if(!list || depth > DT_MASKS_NESTING_MAX) return FALSE;
  for(const GList *l = list->points; l; l = g_list_next(l))
  {
    const dt_masks_point_group_t *pt = l->data;
    if(dt_masks_point_is_marker(pt)) continue;
    const dt_masks_form_t *f = dt_masks_get_from_id(darktable.develop, pt->formid);
    if(!f || f == list || !(f->type & DT_MASKS_GROUP)) return TRUE;
    if(_flexi_list_has_content(f, depth + 1)) return TRUE;
  }
  return FALSE;
}

static gboolean _flexi_layout_has_content(dt_iop_module_t *module)
{
  return _flexi_list_has_content(dt_masks_gui_module_mask_group(module), 0);
}

// reads back every user-saved layout preset's name + node array. Caller frees
// with _flexi_preset_list_free.
typedef struct _flexi_preset_t
{
  gchar *name;
  _flexi_layout_node_t *nodes;
  int n;
} _flexi_preset_t;

static GList *_flexi_preset_list_load(void)
{
  GList *out = NULL;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT name, op_params FROM data.presets"
                              " WHERE operation = ?1 AND writeprotect = 0"
                              "   AND op_version = ?2"
                              " ORDER BY name",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, FLEXI_GROUP_PRESET_OP, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, FLEXI_GROUP_PRESET_VERSION);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const int n = sqlite3_column_bytes(stmt, 1) / (int)sizeof(_flexi_layout_node_t);
    if(n <= 0) continue;
    _flexi_preset_t *p = calloc(1, sizeof(_flexi_preset_t));
    p->name = g_strdup((const gchar *)sqlite3_column_text(stmt, 0));
    p->nodes = calloc(n, sizeof(_flexi_layout_node_t));
    memcpy(p->nodes, sqlite3_column_blob(stmt, 1), n * sizeof(_flexi_layout_node_t));
    p->n = n;
    out = g_list_append(out, p);
  }
  sqlite3_finalize(stmt);
  return out;
}

static void _flexi_preset_free(gpointer data)
{
  _flexi_preset_t *p = data;
  g_free(p->name);
  free(p->nodes);
  free(p);
}

static void _flexi_preset_list_free(GList *presets)
{
  g_list_free_full(presets, _flexi_preset_free);
}

// the preset called `name` in the list `presets`, or NULL
static const _flexi_preset_t *_flexi_preset_find(GList *presets, const gchar *name)
{
  for(GList *p = presets; p && name; p = g_list_next(p))
    if(!g_strcmp0(((_flexi_preset_t *)p->data)->name, name)) return p->data;
  return NULL;
}

// is the layout `nodes` what a mask is anyway before it has a group form: its
// one group, a union, at full opacity? Then applying it would only add a
// history item
static gboolean _flexi_layout_is_plain(const _flexi_layout_node_t *nodes, const int n)
{
  return n == 1 && !nodes[0].flexi_op && nodes[0].opacity == 1.0f;
}

static void
_flexi_preset_save_to_db(const gchar *name, const _flexi_layout_node_t *nodes, int n)
{
  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
      "INSERT OR REPLACE INTO data.presets"
      " (name, description, operation, op_version, op_params, enabled,"
      "  blendop_params, blendop_version, multi_priority, multi_name,"
      "  model, maker, lens, iso_min, iso_max, exposure_min, exposure_max,"
      "  aperture_min, aperture_max, focal_length_min, focal_length_max,"
      "  writeprotect, autoapply, filter, def, format, multi_name_hand_edited)"
      " VALUES (?1, '', ?2, ?3, ?4, 1, NULL, 0, 0, '', '%', '%', '%', 0, 0, 0, 0,"
      "         0, 0, 0, 0, 0, 0, 0, 0, 0, 0)",
      -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, name, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, FLEXI_GROUP_PRESET_OP, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, FLEXI_GROUP_PRESET_VERSION);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 4, nodes, (int)(n * sizeof(_flexi_layout_node_t)),
                             SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

static void _flexi_preset_delete_from_db(const gchar *name)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(
    dt_database_get(darktable.db),
    "DELETE FROM data.presets WHERE operation = ?1 AND name = ?2 AND writeprotect = 0",
    -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, FLEXI_GROUP_PRESET_OP, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, name, -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

// applying a preset discards any real shapes currently in the mask (it only
// ever restores the group skeleton) -- confirm first, same as the plain reset
// button, whenever there is anything to lose.
static void _flexi_preset_apply_confirmed(dt_iop_module_t *module,
                                          const _flexi_layout_node_t *nodes,
                                          const int n,
                                          const gchar *const *keys)
{
  if(_flexi_layout_has_content(module)
     && !dt_gui_show_yes_no_dialog(
       _("apply mask layout preset?"), "",
       _("this replaces the group layout and removes every element "
         "currently in this mask. continue?")))
    return;
  _flexi_layout_apply(module, nodes, n, keys);
}


// ---- built-in layouts ------------------------------------------------------
// listed above the user's own presets, and their names are reserved so a user
// preset cannot shadow one. Read from masks_group_presets.json: the copy in
// the user's config directory when there is one, which is how they are
// iterated on without a rebuild, else the installed one. The file is read
// again whenever it changes, so an edit shows the next time a menu opens or
// the list is rebuilt. Its strings are extracted for translation at build
// time (tools/generate_masks_presets_strings.sh) and translated here with _().
//
// The file's format. At the top level, "version" (1) and "presets", the list
// of presets in the order the menus show them. A preset has:
//   "id"           required and stable: the default preset setting, other
//                  presets and the note keys of the groups it made refer to it
//   "name"         required, shown in the menus; a user preset cannot take it
//   "description"  optional, the menu item's tooltip
//   "mask"         required, the mask's own group
// A preset is a tree of groups, and a group has, all optional:
//   "operator"     how it combines its elements, by the short name of a
//                  dt_masks_flexi_ops entry ("maximum", "screen", "sum",
//                  "minimum", "product", "difference", "exclusion"); maximum
//                  when left out
//   "opacity"      from 0 to 1, 1 when left out
//   "name"         the group's name in the panel
//   "notes"        how to use the group: a list of strings, one page each, of
//                  Pango markup (so "&" is written "&amp;")
//   "groups"       the groups nested in it, listed top-first as the panel
//                  shows them, at most DT_MASKS_NESTING_MAX levels deep
//   "preset"       the id of another preset, whose mask the group is, whole;
//                  any member the group sets takes the place of that preset's
//                  own (see _flexi_group_chain)
//   "id"           names the group's notes within its preset, in place of its
//                  name or its place in the tree (see the note key below)
// The groups a preset makes keep their note key "<preset id>/<group id, name
// or place>", which must fit dt_masks_point_group_t.preset_note, so the notes
// are looked up in the file each time the group is shown, and edited notes
// show on groups made before the edit.
//
// The string extractor reads the file line by line, not as JSON: each "name"
// and "description" stays on one line, and each page of notes on its own
// line of a "notes" list opened at the end of its line.
#define FLEXI_GROUP_PRESET_FILE "masks_group_presets.json"
#define FLEXI_USER_PRESET_PREFIX "user:"

typedef struct _flexi_builtin_t
{
  gchar *id;          // stable, the note keys and the default preset refer to it
  gchar *name;        // untranslated
  gchar *description; // untranslated, may be NULL
  GArray *nodes;      // _flexi_layout_node_t, pre-order (see there); names untranslated
  GPtrArray *keys;    // note key per node, whether the file has notes for it or not
} _flexi_builtin_t;

static struct
{
  GPtrArray *presets; // _flexi_builtin_t
  GHashTable *notes;  // note key -> GPtrArray of untranslated pages
  gchar *path;
  gint64 mtime;
} _builtins;

static void _flexi_builtin_free(gpointer data)
{
  _flexi_builtin_t *b = data;
  g_free(b->id);
  g_free(b->name);
  g_free(b->description);
  g_array_free(b->nodes, TRUE);
  g_ptr_array_free(b->keys, TRUE);
  g_free(b);
}

static const gchar *_json_string(JsonObject *o, const char *member)
{
  JsonNode *n = o ? json_object_get_member(o, member) : NULL;
  return n && JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == G_TYPE_STRING
           ? json_node_get_string(n)
           : NULL;
}

// a group can be another preset, inserted whole: {"preset": "<id>"}, with any
// member it sets itself ("name", "operator", "notes", "groups"...) taking the
// place of that preset's own. It resolves to a chain of objects, the group as
// written first, then the mask of each preset it names in turn, and each
// member is read from the first of them that has it. Each object keeps the
// preset it was written in, and its place there, which the note keys and the
// paths of nested groups are made from: notes and nested groups taken from a
// preset keep that preset's keys, so its notes show wherever it is reused
typedef struct
{
  JsonObject *obj;
  const gchar *preset; // the preset the object is written in
  gchar *path;         // its place there ("mask", "mask.1"...)
} _flexi_group_src_t;

#define FLEXI_PRESET_CHAIN_MAX 8

static void _flexi_chain_free(_flexi_group_src_t *chain, const int n)
{
  for(int i = 0; i < n; i++) g_free(chain[i].path);
}

// resolves `g` into `chain` (see above); how many objects, 0 on an error
static int _flexi_group_chain(_flexi_builtin_t *b,
                              JsonObject *g,
                              const gchar *preset,
                              const gchar *path,
                              GHashTable *by_id,
                              _flexi_group_src_t *chain)
{
  int n = 0;
  chain[n++] = (_flexi_group_src_t){ g, preset, g_strdup(path) };
  const gchar *ref;
  while((ref = _json_string(chain[n - 1].obj, "preset")))
  {
    JsonObject *p = g_hash_table_lookup(by_id, ref);
    JsonObject *mask = p && json_object_has_member(p, "mask")
                         ? json_object_get_object_member(p, "mask")
                         : NULL;
    if(!mask || n == FLEXI_PRESET_CHAIN_MAX)
    {
      dt_print(DT_DEBUG_ALWAYS, "[masks presets] '%s': group %s: %s preset '%s'", b->id,
               path, mask ? "too many levels of, or a loop in," : "no such", ref);
      _flexi_chain_free(chain, n);
      return 0;
    }
    chain[n++] = (_flexi_group_src_t){ mask, ref, g_strdup("mask") };
  }
  return n;
}

// the first object of `chain` that has `member`, NULL if none has
static const _flexi_group_src_t *_flexi_chain_find(const _flexi_group_src_t *chain,
                                                   const int n,
                                                   const char *member)
{
  for(int i = 0; i < n; i++)
    if(json_object_has_member(chain[i].obj, member)) return &chain[i];
  return NULL;
}

// one group and, bottom-up after it, the groups nested in it. `preset` and
// `path` say where `g` is written. FALSE, with the reason logged, on anything
// the file should not hold
static gboolean _flexi_builtin_parse_group(_flexi_builtin_t *b,
                                           JsonObject *g,
                                           const gchar *preset,
                                           const gchar *path,
                                           const int32_t parent,
                                           const int depth,
                                           GHashTable *notes,
                                           GHashTable *by_id)
{
  if(!g || depth > DT_MASKS_NESTING_MAX)
  {
    dt_print(DT_DEBUG_ALWAYS, "[masks presets] '%s': group %s missing or nested too deep",
             b->id, path);
    return FALSE;
  }
  _flexi_group_src_t chain[FLEXI_PRESET_CHAIN_MAX];
  const int nc = _flexi_group_chain(b, g, preset, path, by_id, chain);
  if(!nc) return FALSE;
  gboolean ok = FALSE;
  GPtrArray *pages = g_ptr_array_new_with_free_func(g_free);
  gchar *key = NULL;

  _flexi_layout_node_t node = { .opacity = 1.0f, .parent = parent };
  const _flexi_group_src_t *src = _flexi_chain_find(chain, nc, "operator");
  const gchar *op = src ? _json_string(src->obj, "operator") : NULL;
  gboolean known = !op;
  // an operator is keyed by its short name, untranslated
  for(int i = 0; op && i < DT_MASKS_FLEXI_OPS_COUNT; i++)
    if(!g_strcmp0(op, dt_masks_flexi_ops[i].short_name))
    {
      node.flexi_op = dt_masks_flexi_ops[i].bit;
      known = TRUE;
    }
  if(!known)
  {
    dt_print(DT_DEBUG_ALWAYS, "[masks presets] '%s': group %s has unknown operator '%s'",
             b->id, path, op);
    goto done;
  }
  if((src = _flexi_chain_find(chain, nc, "opacity")))
    node.opacity = CLAMPF(json_object_get_double_member(src->obj, "opacity"), 0.0f, 1.0f);
  src = _flexi_chain_find(chain, nc, "name");
  const gchar *name = src ? _json_string(src->obj, "name") : NULL;
  if(name) dt_strlcpy_to_fixed(node.name, name, sizeof(node.name));

  // the notes, and the key they go by: that of the object they are written
  // in, or with none, of the group as its innermost preset defines it. Within
  // its preset an object is named by its id, or failing that its name, or its
  // place
  const _flexi_group_src_t *note_src = _flexi_chain_find(chain, nc, "notes");
  const _flexi_group_src_t *key_src = note_src ? note_src : &chain[nc - 1];
  const gchar *kid = _json_string(key_src->obj, "id");
  const gchar *kname = _json_string(key_src->obj, "name");
  key = g_strdup_printf("%s/%s", key_src->preset, kid ? kid : kname ? kname : key_src->path);
  JsonNode *notes_node = note_src ? json_object_get_member(note_src->obj, "notes") : NULL;
  const gboolean notes_ok = !notes_node || JSON_NODE_HOLDS_ARRAY(notes_node);
  JsonArray *notes_arr = notes_ok && notes_node ? json_node_get_array(notes_node) : NULL;
  const guint n_pages = notes_arr ? json_array_get_length(notes_arr) : 0;
  gboolean pages_ok = notes_ok;
  for(guint i = 0; pages_ok && i < n_pages; i++)
  {
    JsonNode *page = json_array_get_element(notes_arr, i);
    pages_ok = JSON_NODE_HOLDS_VALUE(page) && json_node_get_value_type(page) == G_TYPE_STRING;
    if(!pages_ok) break;
    const gchar *text = json_node_get_string(page);
    // pages are Pango markup. A broken one still loads, and the panel shows it
    // as plain text (see _group_note_label); say so here, where it is written
    GError *error = NULL;
    if(!pango_parse_markup(text, -1, 0, NULL, NULL, NULL, &error))
    {
      dt_print(DT_DEBUG_ALWAYS, "[masks presets] '%s': group %s, note page %u: %s",
               key_src->preset, key_src->path, i + 1, error->message);
      g_clear_error(&error);
    }
    g_ptr_array_add(pages, g_strdup(text));
  }
  if(!pages_ok)
  {
    dt_print(DT_DEBUG_ALWAYS, "[masks presets] '%s': group %s: \"notes\" must be a list"
             " of strings", key_src->preset, key_src->path);
    goto done;
  }
  if(strlen(key) >= sizeof(((dt_masks_point_group_t *)0)->preset_note))
  {
    dt_print(DT_DEBUG_ALWAYS, "[masks presets] note key '%s' is too long", key);
    goto done;
  }
  if(pages->len)
  {
    g_hash_table_insert(notes, g_strdup(key), pages);
    pages = NULL;
  }
  g_array_append_val(b->nodes, node);
  // every group keeps its key, notes or not, so notes added to the file later
  // show on the groups the preset has already made
  g_ptr_array_add(b->keys, key);
  key = NULL;

  // listed top-first, created bottom-up: each new group lands on top of the
  // ones before it (see dt_masks_model_nest_new_group)
  const int32_t self = (int32_t)b->nodes->len - 1;
  const _flexi_group_src_t *sub_src = _flexi_chain_find(chain, nc, "groups");
  JsonArray *subs = sub_src ? json_object_get_array_member(sub_src->obj, "groups") : NULL;
  const int n = subs ? (int)json_array_get_length(subs) : 0;
  ok = TRUE;
  for(int i = n - 1; ok && i >= 0; i--)
  {
    gchar *sub_path = g_strdup_printf("%s.%d", sub_src->path, i);
    ok = _flexi_builtin_parse_group(b, json_array_get_object_element(subs, i),
                                    sub_src->preset, sub_path, self, depth + 1, notes, by_id);
    g_free(sub_path);
  }

done:
  if(pages) g_ptr_array_unref(pages);
  g_free(key);
  _flexi_chain_free(chain, nc);
  return ok;
}

static gchar *_flexi_builtins_path(void)
{
  char dir[PATH_MAX] = { 0 };
  dt_loc_get_user_config_dir(dir, sizeof(dir));
  gchar *path = g_build_filename(dir, FLEXI_GROUP_PRESET_FILE, NULL);
  if(g_file_test(path, G_FILE_TEST_EXISTS)) return path;
  g_free(path);
  dt_loc_get_datadir(dir, sizeof(dir));
  return g_build_filename(dir, FLEXI_GROUP_PRESET_FILE, NULL);
}

// the built-ins, (re)read when the file is new or has changed. A file that
// does not parse leaves no built-ins, and says why on the console
static GPtrArray *_flexi_builtins_get(void)
{
  gchar *path = _flexi_builtins_path();
  GStatBuf st;
  const gint64 mtime = g_stat(path, &st) == 0 ? (gint64)st.st_mtime : -1;
  if(_builtins.presets && !g_strcmp0(path, _builtins.path) && mtime == _builtins.mtime)
  {
    g_free(path);
    return _builtins.presets;
  }

  if(_builtins.presets) g_ptr_array_free(_builtins.presets, TRUE);
  if(_builtins.notes) g_hash_table_destroy(_builtins.notes);
  g_free(_builtins.path);
  _builtins.presets = g_ptr_array_new_with_free_func(_flexi_builtin_free);
  _builtins.notes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                          (GDestroyNotify)g_ptr_array_unref);
  _builtins.path = path;
  _builtins.mtime = mtime;

  GError *error = NULL;
  JsonParser *parser = json_parser_new();
  if(!json_parser_load_from_file(parser, path, &error))
  {
    dt_print(DT_DEBUG_ALWAYS, "[masks presets] cannot read '%s': %s", path,
             error ? error->message : "unknown error");
    g_clear_error(&error);
    g_object_unref(parser);
    return _builtins.presets;
  }

  JsonNode *root = json_parser_get_root(parser);
  JsonObject *top = root && JSON_NODE_HOLDS_OBJECT(root) ? json_node_get_object(root) : NULL;
  JsonArray *list = top && json_object_has_member(top, "presets")
                      ? json_object_get_array_member(top, "presets")
                      : NULL;
  const int n = list ? (int)json_array_get_length(list) : 0;
  // what a group's "preset" member can name
  GHashTable *by_id = g_hash_table_new(g_str_hash, g_str_equal);
  for(int i = 0; i < n; i++)
  {
    JsonObject *p = json_array_get_object_element(list, i);
    const gchar *id = _json_string(p, "id");
    if(id) g_hash_table_insert(by_id, (gpointer)id, p);
  }
  for(int i = 0; i < n; i++)
  {
    JsonObject *p = json_array_get_object_element(list, i);
    const gchar *id = _json_string(p, "id");
    const gchar *name = _json_string(p, "name");
    if(!id || !name || !json_object_has_member(p, "mask"))
    {
      dt_print(DT_DEBUG_ALWAYS, "[masks presets] '%s': preset %d needs an id, a name and"
               " a mask", path, i);
      continue;
    }
    _flexi_builtin_t *b = g_new0(_flexi_builtin_t, 1);
    b->id = g_strdup(id);
    b->name = g_strdup(name);
    b->description = g_strdup(_json_string(p, "description"));
    b->nodes = g_array_new(FALSE, TRUE, sizeof(_flexi_layout_node_t));
    b->keys = g_ptr_array_new_with_free_func(g_free);
    // a preset's notes only count if all of it parses
    GHashTable *notes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                              (GDestroyNotify)g_ptr_array_unref);
    if(_flexi_builtin_parse_group(b, json_object_get_object_member(p, "mask"), b->id, "mask",
                                  -1, 0, notes, by_id))
    {
      GHashTableIter it;
      gpointer k, v;
      g_hash_table_iter_init(&it, notes);
      while(g_hash_table_iter_next(&it, &k, &v))
      {
        g_hash_table_iter_steal(&it);
        g_hash_table_insert(_builtins.notes, k, v);
      }
      g_ptr_array_add(_builtins.presets, b);
    }
    else
      _flexi_builtin_free(b);
    g_hash_table_destroy(notes);
  }
  g_hash_table_destroy(by_id);
  g_object_unref(parser);
  dt_print(DT_DEBUG_PARAMS, "[masks presets] %u built-in presets from '%s'",
           _builtins.presets->len, path);
  return _builtins.presets;
}

static const _flexi_builtin_t *_flexi_builtin_by_id(const gchar *id)
{
  GPtrArray *all = _flexi_builtins_get();
  for(guint i = 0; id && i < all->len; i++)
  {
    const _flexi_builtin_t *b = g_ptr_array_index(all, i);
    if(!g_strcmp0(b->id, id)) return b;
  }
  return NULL;
}

GPtrArray *dt_masks_gui_preset_notes(const char *key)
{
  if(!key || !*key) return NULL;
  _flexi_builtins_get();
  return g_hash_table_lookup(_builtins.notes, key);
}

gboolean dt_masks_gui_preset_notes_shown(void)
{
  return dt_conf_get_bool("plugins/darkroom/masks/show_preset_notes");
}

// a built-in, as the node array the apply path takes, its group names
// translated. Caller frees
static _flexi_layout_node_t *_flexi_builtin_nodes(const _flexi_builtin_t *b)
{
  _flexi_layout_node_t *nodes = g_new(_flexi_layout_node_t, b->nodes->len);
  memcpy(nodes, b->nodes->data, b->nodes->len * sizeof(_flexi_layout_node_t));
  for(guint i = 0; i < b->nodes->len; i++)
  {
    // gettext hands back its argument when there is no translation, and
    // dt_strlcpy_to_fixed clears the destination before copying
    const char *tr = nodes[i].name[0] ? _(nodes[i].name) : nodes[i].name;
    if(tr != nodes[i].name) dt_strlcpy_to_fixed(nodes[i].name, tr, sizeof(nodes[i].name));
  }
  return nodes;
}

static void _flexi_builtin_apply(dt_iop_module_t *module, const _flexi_builtin_t *b)
{
  _flexi_layout_node_t *nodes = _flexi_builtin_nodes(b);
  _flexi_preset_apply_confirmed(module, nodes, b->nodes->len, (const gchar **)b->keys->pdata);
  g_free(nodes);
}

// the same for a built-in, which also has to come without notes
static gboolean _flexi_builtin_is_plain(const _flexi_builtin_t *b)
{
  return _flexi_layout_is_plain((const _flexi_layout_node_t *)b->nodes->data, b->nodes->len)
         && !dt_masks_gui_preset_notes(g_ptr_array_index(b->keys, 0));
}

static void _flexi_preset_save_action(GSimpleAction *action,
                                      GVariant *parameter,
                                      gpointer user_data)
{
  dt_iop_module_t *module = (dt_iop_module_t *)user_data;
  if(darktable.gui->active_popover_menu)
    gtk_popover_popdown(GTK_POPOVER(darktable.gui->active_popover_menu));
  char *name = dt_gui_show_standalone_string_dialog(
    _("save mask layout preset"),
    _("enter a name for this preset\n"
      "(only the group layout is saved, not the shapes/channels inside it):"),
    _("preset name"), _("cancel"), _("save"));
  if(!name) return;
  gboolean reserved = FALSE;
  GPtrArray *builtins = _flexi_builtins_get();
  for(guint i = 0; i < builtins->len; i++)
    if(!strcmp(name, _(((_flexi_builtin_t *)g_ptr_array_index(builtins, i))->name)))
      reserved = TRUE;
  if(!*name)
    dt_control_log(_("please give the preset a name"));
  else if(reserved)
    dt_control_log(_("`%s' is a reserved preset name, please pick another one"), name);
  else
  {
    int n = 0;
    _flexi_layout_node_t *nodes = _flexi_layout_capture(module, &n);
    if(n > 0) _flexi_preset_save_to_db(name, nodes, n);
    free(nodes);
  }
  g_free(name);
}

static void _flexi_preset_builtin_action(GSimpleAction *action,
                                         GVariant *parameter,
                                         gpointer user_data)
{
  dt_iop_module_t *module = (dt_iop_module_t *)user_data;
  const gchar *id = g_variant_get_string(parameter, NULL);
  if(darktable.gui->active_popover_menu)
    gtk_popover_popdown(GTK_POPOVER(darktable.gui->active_popover_menu));
  const _flexi_builtin_t *b = _flexi_builtin_by_id(id);
  if(b) _flexi_builtin_apply(module, b);
}

static void _flexi_preset_user_action(GSimpleAction *action,
                                      GVariant *parameter,
                                      gpointer user_data)
{
  dt_iop_module_t *module = (dt_iop_module_t *)user_data;
  const gchar *name = g_variant_get_string(parameter, NULL);
  if(darktable.gui->active_popover_menu)
    gtk_popover_popdown(GTK_POPOVER(darktable.gui->active_popover_menu));
  GList *user_presets = _flexi_preset_list_load();
  const _flexi_preset_t *preset = _flexi_preset_find(user_presets, name);
  if(preset) _flexi_preset_apply_confirmed(module, preset->nodes, preset->n, NULL);
  _flexi_preset_list_free(user_presets);
}

static void _flexi_preset_delete_action(GSimpleAction *action,
                                        GVariant *parameter,
                                        gpointer user_data)
{
  const gchar *name = g_variant_get_string(parameter, NULL);
  if(darktable.gui->active_popover_menu)
    gtk_popover_popdown(GTK_POPOVER(darktable.gui->active_popover_menu));
  if(name && dt_gui_show_yes_no_dialog(_("delete preset?"), "",
                                       _("do you really want to delete the mask layout "
                                         "preset `%s'?"),
                                       name))
  {
    _flexi_preset_delete_from_db(name);
  }
}

// appends a "presets" section (group-layout presets) directly to `menu` --
// the menu of the toolbar's presets button (see _masks_presets_pressed)
void dt_masks_gui_add_presets_menu(GMenu *menu, GtkWidget *anchor, dt_iop_module_t *module)
{
  GActionGroup *action_group = gtk_widget_get_action_group(anchor, "masks_presets");
  if(action_group == NULL)
  {
    GActionEntry action_entries[] =
    {
      { "builtin", _flexi_preset_builtin_action, "s", NULL },
      { "user",    _flexi_preset_user_action,    "s", NULL },
      { "delete",  _flexi_preset_delete_action,  "s", NULL },
      { "save",    _flexi_preset_save_action,    NULL, NULL },
    };
    action_group = G_ACTION_GROUP(g_simple_action_group_new());
    g_action_map_add_action_entries(G_ACTION_MAP(action_group), action_entries,
                                    G_N_ELEMENTS(action_entries), module);
    gtk_widget_insert_action_group(anchor, "masks_presets", action_group);
    g_object_unref(action_group);
  }

  GMenu *sec_builtins = g_menu_new();
  GPtrArray *builtins = _flexi_builtins_get();
  for(guint i = 0; i < builtins->len; i++)
  {
    const _flexi_builtin_t *b = g_ptr_array_index(builtins, i);
    GMenuItem *item = g_menu_item_new(_(b->name), NULL);
    g_menu_item_set_action_and_target_value(item, "masks_presets.builtin",
                                            g_variant_new_string(b->id));
    if(b->description) g_menu_item_set_attribute(item, "tooltip", "s", _(b->description));
    g_menu_append_item(sec_builtins, item);
    g_object_unref(item);
  }
  g_menu_append_section(menu, _("group layout presets"), G_MENU_MODEL(sec_builtins));
  g_object_unref(sec_builtins);

  GList *user_presets = _flexi_preset_list_load();
  if(user_presets)
  {
    GMenu *sec_user = g_menu_new();
    GMenu *sub_delete = g_menu_new();
    for(GList *p = user_presets; p; p = g_list_next(p))
    {
      _flexi_preset_t *preset = p->data;
      GMenuItem *item = g_menu_item_new(preset->name, NULL);
      g_menu_item_set_action_and_target_value(item, "masks_presets.user", g_variant_new_string(preset->name));
      g_menu_append_item(sec_user, item);
      g_object_unref(item);

      GMenuItem *del = g_menu_item_new(preset->name, NULL);
      g_menu_item_set_action_and_target_value(del, "masks_presets.delete", g_variant_new_string(preset->name));
      g_menu_append_item(sub_delete, del);
      g_object_unref(del);
    }
    g_menu_append_submenu(sec_user, _("delete preset"), G_MENU_MODEL(sub_delete));
    g_object_unref(sub_delete);

    g_menu_append_section(menu, NULL, G_MENU_MODEL(sec_user));
    g_object_unref(sec_user);
    _flexi_preset_list_free(user_presets);
  }

  GMenu *sec_save = g_menu_new();
  g_menu_append(sec_save, _("save current layout as preset..."), "masks_presets.save");
  g_menu_append_section(menu, NULL, G_MENU_MODEL(sec_save));
  g_object_unref(sec_save);
}

// ---- default preset ---------------------------------------------------------
// the layout a mask starts with: a built-in's id, or a user preset's name
// behind FLEXI_USER_PRESET_PREFIX
#define FLEXI_DEFAULT_PRESET_CONF "plugins/darkroom/masks/default_group_preset"

void dt_masks_gui_apply_default_preset(dt_iop_module_t *module)
{
  gchar *def = dt_conf_get_string(FLEXI_DEFAULT_PRESET_CONF);
  if(g_str_has_prefix(def, FLEXI_USER_PRESET_PREFIX))
  {
    const gchar *name = def + strlen(FLEXI_USER_PRESET_PREFIX);
    GList *user_presets = _flexi_preset_list_load();
    const _flexi_preset_t *preset = _flexi_preset_find(user_presets, name);
    if(preset && !_flexi_layout_is_plain(preset->nodes, preset->n))
      _flexi_layout_apply(module, preset->nodes, preset->n, NULL);
    _flexi_preset_list_free(user_presets);
  }
  else
  {
    const _flexi_builtin_t *b = _flexi_builtin_by_id(def);
    if(b && !_flexi_builtin_is_plain(b))
    {
      _flexi_layout_node_t *nodes = _flexi_builtin_nodes(b);
      _flexi_layout_apply(module, nodes, b->nodes->len, (const gchar **)b->keys->pdata);
      g_free(nodes);
    }
  }
  g_free(def);
}

static void _masks_default_preset_toggled(GtkToggleButton *radio, gpointer user_data)
{
  DT_GUARD_GUI_UPDATE();
  if(!gtk_toggle_button_get_active(radio)) return;
  dt_conf_set_string(FLEXI_DEFAULT_PRESET_CONF, g_object_get_data(G_OBJECT(radio), "preset"));
}

static GtkWidget *_masks_default_preset_radio(GtkWidget *box,
                                              GtkWidget *group,
                                              const gchar *label,
                                              const gchar *tooltip,
                                              gchar *value,
                                              const gchar *current)
{
  GtkWidget *radio = gtk_radio_button_new_with_label_from_widget(
    group ? GTK_RADIO_BUTTON(group) : NULL, label);
  if(tooltip) gtk_widget_set_tooltip_text(radio, tooltip);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(radio), !g_strcmp0(value, current));
  g_object_set_data_full(G_OBJECT(radio), "preset", value, g_free);
  dt_gui_box_add(box, radio);
  return radio;
}

void dt_masks_gui_add_default_preset_box(GtkWidget *box)
{
  dt_masks_gui_pref_section(box, _("default group layout"),
                            _("the group layout preset a module's mask starts with\n"
                              "when it is switched on for the first time"));

  gchar *current = dt_conf_get_string(FLEXI_DEFAULT_PRESET_CONF);
  GtkWidget *group = NULL;
  GList *radios = NULL;

  // states first, handlers after, as the panel position radios do
  DT_ENTER_GUI_UPDATE();
  GPtrArray *builtins = _flexi_builtins_get();
  for(guint i = 0; i < builtins->len; i++)
  {
    const _flexi_builtin_t *b = g_ptr_array_index(builtins, i);
    group = _masks_default_preset_radio(box, group, _(b->name),
                                        b->description ? _(b->description) : NULL,
                                        g_strdup(b->id), current);
    radios = g_list_prepend(radios, group);
  }
  GList *user_presets = _flexi_preset_list_load();
  for(GList *p = user_presets; p; p = g_list_next(p))
  {
    const _flexi_preset_t *preset = p->data;
    group = _masks_default_preset_radio(
      box, group, preset->name, NULL,
      g_strconcat(FLEXI_USER_PRESET_PREFIX, preset->name, NULL), current);
    radios = g_list_prepend(radios, group);
  }
  _flexi_preset_list_free(user_presets);
  DT_LEAVE_GUI_UPDATE();

  for(GList *r = radios; r; r = g_list_next(r))
    g_signal_connect(G_OBJECT(r->data), "toggled", G_CALLBACK(_masks_default_preset_toggled),
                     NULL);
  g_list_free(radios);
  g_free(current);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
