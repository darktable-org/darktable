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

#include "common/utility.h"
#include "develop/blend_gui_internal.h"

#include "bauhaus/bauhaus.h"
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
// applied as an array of dt_masks_layout_node_t in pre-order, the mask's own
// group first and every group's nested groups bottom-up after it, each naming
// its holder by index.
//
// stored in the presets table under a fake operation name no module
// registers, so that they are shared by every module's panel. Not through
// gui/presets.c, which applies a preset by overwriting a module's params; a
// layout touches only the mask's groups
#define FLEXI_GROUP_PRESET_OP "flexi_mask_groups"
#define FLEXI_GROUP_PRESET_VERSION 1

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
    const dt_masks_point_group_t *marker = dt_masks_group_marker(sub);
    if(!marker) continue;
    dt_masks_layout_node_t node = { .flexi_op = marker->state & DT_MASKS_STATE_FLEXI_OP,
                                    .opacity = marker->group_opacity,
                                    .parent = parent };
    dt_strlcpy_fixed_to_fixed(node.name, sizeof(node.name), marker->name, sizeof(marker->name));
    g_array_append_val(out, node);
    _flexi_layout_capture_nested(out, sub, (int32_t)out->len - 1, depth + 1);
  }
}

// the group skeleton of group `cid` of the module's mask, in pre-order (see
// dt_masks_layout_node_t): the group itself first, as the layout's own group,
// then the groups nested in it. INVALID_MASKID, or the mask's own group, is
// the whole mask. Caller frees the returned array
static dt_masks_layout_node_t *_flexi_layout_capture(dt_iop_module_t *module,
                                                     const dt_mask_id_t cid,
                                                     int *n_out)
{
  dt_masks_form_t *grp = dt_masks_gui_module_mask_group(module);
  GArray *out = g_array_new(FALSE, FALSE, sizeof(dt_masks_layout_node_t));

  const dt_masks_point_group_t *root = dt_masks_group_marker(grp);
  const gboolean whole = !dt_is_valid_maskid(cid) || (root && cid == root->formid);
  const dt_masks_form_t *list = whole ? grp : dt_masks_model_nested_group_of(grp, cid);
  // a mask with no group form yet has the group the panel shows for it (see
  // dt_masks_gui_default_layout_root)
  dt_masks_point_group_t none = { .group_opacity = 1.0f };
  if(!grp) dt_masks_gui_default_layout_root(&none);
  const dt_masks_point_group_t *head = grp ? dt_masks_group_marker(list) : &none;
  const dt_masks_layout_node_t node = {
    .flexi_op = head ? head->state & DT_MASKS_STATE_FLEXI_OP : 0,
    .opacity = head ? head->group_opacity : 1.0f,
    .parent = -1
  };
  g_array_append_val(out, node);
  _flexi_layout_capture_nested(out, list, 0, 1);

  *n_out = out->len;
  return (dt_masks_layout_node_t *)g_array_free(out, FALSE);
}

// give the layout's own group, whose marker is `head`, the settings of the
// layout's first node `node`, and its note key `key` (NULL for none)
static void _flexi_layout_set_head(dt_masks_point_group_t *head,
                                   const dt_masks_layout_node_t *node,
                                   const gchar *key)
{
  head->state = (head->state & ~DT_MASKS_STATE_FLEXI_OP)
                | (node->flexi_op & DT_MASKS_STATE_FLEXI_OP);
  head->group_opacity = node->opacity;
  dt_strlcpy_to_fixed(head->preset_note, key ? key : "", sizeof(head->preset_note));
}

// `keys` are a built-in's; a user preset has no notes
dt_mask_id_t dt_masks_model_layout_build_at(dt_masks_form_t *grp,
                                            dt_masks_point_group_t *head,
                                            const dt_masks_layout_node_t *nodes,
                                            const int n,
                                            const gchar *const *keys,
                                            const gboolean keep_head,
                                            int *skipped)
{
  if(skipped) *skipped = 0;
  if(!head || n <= 0 || nodes[0].parent != -1) return INVALID_MASKID;
  if(!keep_head) _flexi_layout_set_head(head, &nodes[0], keys ? keys[0] : NULL);

  dt_mask_id_t *cids = g_new(dt_mask_id_t, n);
  cids[0] = head->formid;
  dt_mask_id_t first_nested = INVALID_MASKID;
  for(int i = 1; i < n; i++)
  {
    cids[i] = INVALID_MASKID;
    // pre-order: a holder always comes before what it holds
    const int32_t parent = nodes[i].parent;
    if(parent >= 0 && parent < i && dt_is_valid_maskid(cids[parent]))
      cids[i] = dt_masks_model_nest_new_group(grp, nodes[i].flexi_op, cids[parent]);
    dt_masks_point_group_t *marker = dt_masks_gui_group_point(grp, cids[i]);
    if(!marker)
    {
      if(skipped) (*skipped)++;
      continue;
    }
    marker->group_opacity = nodes[i].opacity;
    dt_strlcpy_to_fixed(marker->name, nodes[i].name, sizeof(marker->name));
    dt_strlcpy_to_fixed(marker->preset_note, keys ? keys[i] : "", sizeof(marker->preset_note));
    if(!dt_is_valid_maskid(first_nested)) first_nested = cids[i];
  }
  g_free(cids);
  return first_nested;
}

// replaces the module's whole mask with the
// empty groups of the layout `nodes` (see dt_masks_model_layout_build_at).
// Never asks for confirmation itself; a caller that may be discarding elements
// asks first (see _flexi_layout_apply_within). `wipe` FALSE skips the reset,
// for a mask with no group form yet: there is nothing to replace, and the
// reset would also clear a whole-mask refinement, the very edit that may have
// just switched the mask on (see _blendop_mask_enable)
static void _flexi_layout_apply(dt_iop_module_t *module,
                                const dt_masks_layout_node_t *nodes,
                                const int n,
                                const gchar *const *keys,
                                const gboolean wipe)
{
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  if(wipe) dt_masks_gui_reset_mask_core(module);
  // created plain, not through dt_masks_gui_module_flexi_group, which would
  // build the default layout into it before this one
  dt_masks_form_t *grp = dt_masks_gui_module_mask_group(module);
  if(!grp && darktable.develop) grp = dt_masks_module_group_create(darktable.develop, module);
  // the reset left the mask's own group, empty: the layout's first node
  dt_masks_point_group_t *root = dt_masks_group_marker(grp);
  if(!root || n <= 0 || nodes[0].parent != -1) return;
  const dt_mask_id_t first_nested =
    dt_masks_model_layout_build_at(grp, root, nodes, n, keys, FALSE, NULL);

  // start where the next element most likely goes: the bottom nested group,
  // or the mask's own group when the layout nests none
  bd->panel_selected_formid = INVALID_MASKID;
  bd->panel_selected_group_cid =
    dt_is_valid_maskid(first_nested) ? first_nested : root->formid;
  dt_dev_add_masks_history_item(darktable.develop, module, TRUE);
  dt_masks_gui_build_list(module);
  dt_masks_gui_refresh_canvas_edit(module);
}

// how a preset goes into the selected group (see _flexi_preset_row_new)
typedef enum _flexi_preset_choice_t
{
  // the group drops what it holds and becomes the preset
  _PRESET_REPLACE = 0,
  // the preset's groups go into the group, which keeps its own settings
  _PRESET_MERGE,
  // the preset goes into the group whole, as a new group
  _PRESET_ADD
} _flexi_preset_choice_t;

// applies the layout `nodes` within group `cid` of the module's mask, the
// selected group the presets button was opened for (INVALID_MASKID for a
// mask with no group form yet), as `how` says (see _flexi_preset_choice_t).
// Replacing keeps the group's name, and for the mask's own group is the whole
// mask's reset (see _flexi_layout_apply); it asks first when it would discard
// elements. The group added is named `name`. The group replaced, merged into
// or added is selected, which also opens its note
static void _flexi_layout_apply_within(dt_iop_module_t *module,
                                       const dt_mask_id_t cid,
                                       const dt_masks_layout_node_t *nodes,
                                       const int n,
                                       const gchar *const *keys,
                                       const _flexi_preset_choice_t how,
                                       const gchar *name)
{
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  if(n <= 0 || nodes[0].parent != -1) return;
  dt_masks_form_t *mask = dt_masks_gui_module_mask_group(module);
  const dt_masks_point_group_t *root = dt_masks_group_marker(mask);
  const gboolean whole = !dt_is_valid_maskid(cid) || (root && cid == root->formid);
  if(how == _PRESET_REPLACE
     && dt_masks_model_group_has_elements(whole ? mask : dt_masks_model_nested_group_of(mask, cid))
     && !dt_gui_show_yes_no_dialog(_("apply mask layout preset?"), "",
                                   whole ? _("this replaces the group layout and removes every"
                                             " element currently in this mask. continue?")
                                         : _("this replaces the group's layout and removes every"
                                             " element currently in it. continue?")))
    return;
  if(how == _PRESET_REPLACE && whole)
  {
    _flexi_layout_apply(module, nodes, n, keys, TRUE);
    return;
  }

  // a mask with no group form yet gets one, with the default layout the
  // panel shows for it (see dt_masks_gui_module_flexi_group)
  dt_mask_id_t target = cid;
  dt_masks_form_t *grp = dt_masks_gui_module_flexi_group(module, &target);
  if(!grp) return;
  dt_masks_point_group_t *head = NULL;
  if(how == _PRESET_ADD)
  {
    head = dt_masks_gui_group_point(grp, dt_masks_model_nest_new_group(grp, nodes[0].flexi_op,
                                                                       target));
    if(head) dt_strlcpy_to_fixed(head->name, name, sizeof(head->name));
  }
  else
  {
    if(how == _PRESET_REPLACE) dt_masks_gui_empty_group_core(module, target);
    head = dt_masks_gui_group_point(grp, target);
  }
  if(!head)
  {
    // the group replaced or merged into is always there; a new one may not be
    if(how == _PRESET_ADD)
      dt_control_log(_("this group is nested too deep to add a group to it"));
    return;
  }
  int skipped = 0;
  dt_masks_model_layout_build_at(grp, head, nodes, n, keys, how == _PRESET_MERGE, &skipped);
  if(skipped)
    dt_control_log(ngettext("%d group of the preset would nest too deep and was left out",
                            "%d groups of the preset would nest too deep and were left out",
                            skipped),
                   skipped);

  bd->panel_selected_formid = INVALID_MASKID;
  bd->panel_selected_group_cid = head->formid;
  dt_dev_add_masks_history_item(darktable.develop, module, TRUE);
  dt_masks_gui_build_list(module);
  dt_masks_gui_refresh_canvas_edit(module);
}

// reads back every user-saved layout preset's name + node array. Caller frees
// with _flexi_preset_list_free.
typedef struct _flexi_preset_t
{
  gchar *name;
  dt_masks_layout_node_t *nodes;
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
    const int n = sqlite3_column_bytes(stmt, 1) / (int)sizeof(dt_masks_layout_node_t);
    if(n <= 0) continue;
    _flexi_preset_t *p = calloc(1, sizeof(_flexi_preset_t));
    p->name = g_strdup((const gchar *)sqlite3_column_text(stmt, 0));
    p->nodes = calloc(n, sizeof(dt_masks_layout_node_t));
    memcpy(p->nodes, sqlite3_column_blob(stmt, 1), n * sizeof(dt_masks_layout_node_t));
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

// the user presets as last read, owned here. The default layout is looked up
// on every list signature (see _default_layout), too often to query the
// database each time. Every change made here forgets them, and the presets
// popover reads them afresh, for one made elsewhere
static GList *_user_presets = NULL;
static gboolean _user_presets_read = FALSE;

// the resolved default layout's first node and note key, cached to avoid
// re-parsing on every signature. _cached_default_head is FALSE when the default
// names no preset, which leaves a caller's group untouched
static dt_masks_layout_node_t _cached_default_node;
static gchar _cached_default_key[64];
static gboolean _cached_default_head = FALSE;
static gboolean _cached_default_root_valid = FALSE;
static gchar *_cached_default_preset = NULL;

static void _default_layout_cache_invalidate(void)
{
  _cached_default_root_valid = FALSE;
}

static GList *_flexi_user_presets(void)
{
  if(!_user_presets_read)
  {
    _user_presets = _flexi_preset_list_load();
    _user_presets_read = TRUE;
  }
  return _user_presets;
}

static void _flexi_user_presets_forget(void)
{
  _default_layout_cache_invalidate();
  _flexi_preset_list_free(_user_presets);
  _user_presets = NULL;
  _user_presets_read = FALSE;
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
static gboolean _flexi_layout_is_plain(const dt_masks_layout_node_t *nodes, const int n)
{
  return n == 1 && !nodes[0].flexi_op && nodes[0].opacity == 1.0f;
}

static void
_flexi_preset_save_to_db(const gchar *name, const dt_masks_layout_node_t *nodes, int n)
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
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 4, nodes, (int)(n * sizeof(dt_masks_layout_node_t)),
                             SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  _flexi_user_presets_forget();
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
  _flexi_user_presets_forget();
}

// what a way of applying a preset does, to the group or to the whole mask
static const gchar *_flexi_preset_explain(const _flexi_preset_choice_t how, const gboolean group)
{
  switch(how)
  {
    case _PRESET_REPLACE:
      return group ? _("the group drops every member it holds and becomes the"
                       " preset, keeping its name")
                   : _("the mask starts over as the preset, every member in it"
                       " removed");
    case _PRESET_MERGE:
      return group ? _("the preset's groups go into the group, next to what it holds; the"
                       " group keeps its own operator and opacity")
                   : _("the preset's groups go into the mask, next to what it holds; the"
                       " mask keeps its own operator and opacity");
    default:
      return group ? _("the preset goes into the group whole, as a new group next to what"
                       " it holds")
                   : _("the preset goes into the mask whole, as a new group next to what"
                       " it holds");
  }
}

// the tooltip of a way of applying a preset: its name and what it does
static gchar *_flexi_preset_how_tip(const _flexi_preset_choice_t how, const gboolean group)
{
  const gchar *title = how == _PRESET_REPLACE ? _("replace")
                       : how == _PRESET_MERGE ? _("merge")
                                              : _("add as subgroup");
  return g_strdup_printf("%s\n%s", title, _flexi_preset_explain(how, group));
}

// the way the preset is applied is picked in the presets popover, by the button
// clicked. The group it goes into is the one the presets button was opened for
// (see _masks_presets_pressed in blend_gui.c)
static void _flexi_preset_apply_picked(GtkWidget *anchor,
                                       const dt_masks_layout_node_t *nodes,
                                       const int n,
                                       const gchar *const *keys,
                                       const gchar *name,
                                       const _flexi_preset_choice_t how)
{
  dt_iop_module_t *module = g_object_get_data(G_OBJECT(anchor), "module");
  if(!module) return;
  const dt_mask_id_t cid = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(anchor), "presets-cid"));
  _flexi_layout_apply_within(module, cid, nodes, n, keys, how, name);
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
  GArray *nodes;      // dt_masks_layout_node_t, pre-order (see there); names untranslated
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

  dt_masks_layout_node_t node = { .opacity = 1.0f, .parent = parent };
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
  _default_layout_cache_invalidate();
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
    b->nodes = g_array_new(FALSE, TRUE, sizeof(dt_masks_layout_node_t));
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
static dt_masks_layout_node_t *_flexi_builtin_nodes(const _flexi_builtin_t *b)
{
  dt_masks_layout_node_t *nodes = g_new(dt_masks_layout_node_t, b->nodes->len);
  memcpy(nodes, b->nodes->data, b->nodes->len * sizeof(dt_masks_layout_node_t));
  for(guint i = 0; i < b->nodes->len; i++)
  {
    // gettext hands back its argument when there is no translation, and
    // dt_strlcpy_to_fixed clears the destination before copying
    const char *tr = nodes[i].name[0] ? _(nodes[i].name) : nodes[i].name;
    if(tr != nodes[i].name) dt_strlcpy_to_fixed(nodes[i].name, tr, sizeof(nodes[i].name));
  }
  return nodes;
}

// the same for a built-in, which also has to come without notes
static gboolean _flexi_builtin_is_plain(const _flexi_builtin_t *b)
{
  return _flexi_layout_is_plain((const dt_masks_layout_node_t *)b->nodes->data, b->nodes->len)
         && !dt_masks_gui_preset_notes(g_ptr_array_index(b->keys, 0));
}

static void _flexi_preset_save_clicked(GtkWidget *button, gpointer user_data)
{
  dt_iop_module_t *module = g_object_get_data(G_OBJECT(user_data), "module");
  if(darktable.gui->active_popover_menu)
    gtk_popover_popdown(GTK_POPOVER(darktable.gui->active_popover_menu));
  if(!module) return;
  // read before the dialog, which runs a nested main loop without being modal:
  // the presets button can be opened for another group while it waits
  const dt_mask_id_t cid = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(user_data), "presets-cid"));
  char *name = dt_gui_show_standalone_string_dialog(
    _("save mask layout preset"),
    _("enter a name for this preset\n"
      "(only the layout of the selected group and the groups in it is saved,\n"
      "not the shapes/channels inside them):"),
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
    dt_masks_layout_node_t *nodes = _flexi_layout_capture(module, cid, &n);
    if(n > 0) _flexi_preset_save_to_db(name, nodes, n);
    free(nodes);
  }
  g_free(name);
}

// a row of the presets popover: a preset and the ways to apply it
typedef struct _flexi_preset_row_t
{
  GtkWidget *anchor; // the presets button, see _flexi_preset_apply_picked
  gchar *id;         // a built-in's id, or a user preset's name
  gboolean builtin;
} _flexi_preset_row_t;

static void _flexi_preset_row_free(gpointer data)
{
  _flexi_preset_row_t *row = data;
  g_free(row->id);
  g_free(row);
}

static void _flexi_preset_popdown(void)
{
  if(darktable.gui->active_popover_menu)
    gtk_popover_popdown(GTK_POPOVER(darktable.gui->active_popover_menu));
}

static void _flexi_preset_option_clicked(GtkWidget *button, gpointer data)
{
  const _flexi_preset_row_t *row = data;
  const _flexi_preset_choice_t how =
    GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "how"));
  _flexi_preset_popdown();
  if(row->builtin)
  {
    const _flexi_builtin_t *b = _flexi_builtin_by_id(row->id);
    if(!b) return;
    dt_masks_layout_node_t *nodes = _flexi_builtin_nodes(b);
    _flexi_preset_apply_picked(row->anchor, nodes, b->nodes->len,
                               (const gchar **)b->keys->pdata, _(b->name), how);
    g_free(nodes);
    return;
  }
  const _flexi_preset_t *preset = _flexi_preset_find(_flexi_user_presets(), row->id);
  if(preset)
    _flexi_preset_apply_picked(row->anchor, preset->nodes, preset->n, NULL, preset->name, how);
}

static void _flexi_preset_delete_clicked(GtkWidget *button, gpointer data)
{
  const _flexi_preset_row_t *row = data;
  _flexi_preset_popdown();
  if(dt_gui_show_yes_no_dialog(_("delete preset?"), "",
                               _("do you really want to delete the mask layout "
                                 "preset `%s'?"),
                               row->id))
    _flexi_preset_delete_from_db(row->id);
}

// an icon button of a preset's row. One that does not apply to the preset
// keeps its room, invisible and inert, so that the buttons of a kind line up
// down the rows
static GtkWidget *_flexi_preset_row_button(GtkWidget *box,
                                           DTGTKCairoPaintIconFunc paint,
                                           const gchar *tip,
                                           GCallback callback,
                                           gpointer row)
{
  GtkWidget *button = dtgtk_button_new(paint, 0, NULL);
  if(tip)
  {
    gtk_widget_set_tooltip_text(button, tip);
    g_signal_connect(button, "clicked", G_CALLBACK(callback), row);
  }
  else
  {
    gtk_widget_set_opacity(button, 0.0);
    gtk_widget_set_sensitive(button, FALSE);
    gtk_widget_set_can_focus(button, FALSE);
  }
  gtk_box_pack_start(GTK_BOX(box), button, FALSE, FALSE, 0);
  return button;
}

// the groups of a layout as a tree, one line per group: its operator, its name
// when it has one and its opacity when below full. The first line is the
// layout's own group, the rest hang from it. The nested groups of a group are
// listed top first, as the panel does, which is the reverse of their order in
// the nodes. `prefix` is what the lines below this group start with
static void _flexi_layout_outline_add(GString *out,
                                      const dt_masks_layout_node_t *nodes,
                                      const int n,
                                      const int i,
                                      const gchar *prefix)
{
  const dt_masks_flexi_op_t *op = &dt_masks_flexi_ops[0];
  for(int k = 1; k < DT_MASKS_FLEXI_OPS_COUNT; k++)
    if(nodes[i].flexi_op & dt_masks_flexi_ops[k].bit) op = &dt_masks_flexi_ops[k];
  g_string_append(out, _(op->short_name));
  if(nodes[i].parent >= 0 && nodes[i].name[0])
    g_string_append_printf(out, " (%s)", nodes[i].name);
  if(nodes[i].opacity < 1.0f)
    g_string_append_printf(out, ", %d%%", (int)roundf(nodes[i].opacity * 100.0f));

  int last = -1;
  for(int j = n - 1; j > i; j--)
    if(nodes[j].parent == i) last = j;
  for(int j = n - 1; j > i; j--)
  {
    if(nodes[j].parent != i) continue;
    // the last group listed closes the branch
    const gboolean closes = j == last;
    g_string_append_printf(out, "\n%s%s", prefix, closes ? "\u2514\u2500 " : "\u251c\u2500 ");
    gchar *below = g_strdup_printf("%s%s", prefix, closes ? "   " : "\u2502  ");
    _flexi_layout_outline_add(out, nodes, n, j, below);
    g_free(below);
  }
}

// a user preset's delete button (a built-in cannot be deleted, and has none),
// then the preset's name, which adds it as a subgroup (the way that loses
// nothing), then a button per way of applying it, with its own icon: replace,
// merge and add as subgroup. A preset with no groups inside (`n` <= 1) has
// nothing to merge
static GtkWidget *_flexi_preset_row_new(GtkWidget *anchor,
                                        const gchar *id,
                                        const gboolean builtin,
                                        const gchar *label,
                                        const gchar *description,
                                        const dt_masks_layout_node_t *nodes,
                                        const int n)
{
  _flexi_preset_row_t *row = g_new0(_flexi_preset_row_t, 1);
  row->anchor = anchor;
  row->id = g_strdup(id);
  row->builtin = builtin;
  const gboolean group = g_object_get_data(G_OBJECT(anchor), "presets-group") != NULL;

  GtkWidget *box = dt_gui_hbox();
  if(!builtin)
  {
    GtkWidget *del = _flexi_preset_row_button(box, dtgtk_cairo_paint_remove,
                                              _("delete this preset"),
                                              G_CALLBACK(_flexi_preset_delete_clicked), row);
    dt_gui_add_class(del, "dt_masks_preset_delete");
  }

  GtkWidget *name = gtk_model_button_new();
  // a model button reads an underscore of its text as a mnemonic
  gchar *label_unm = dt_str_unmnemonic(label);
  g_object_set(name, "text", label_unm, NULL);
  g_free(label_unm);
  gtk_widget_set_hexpand(name, TRUE);
  dt_gui_add_class(name, "dt_masks_preset_name");
  g_object_set_data(G_OBJECT(name), "how", GINT_TO_POINTER(_PRESET_ADD));
  g_signal_connect(name, "clicked", G_CALLBACK(_flexi_preset_option_clicked), row);
  GString *layout = g_string_new(NULL);
  _flexi_layout_outline_add(layout, nodes, n, 0, "");
  gchar *name_tip = description
    ? g_strdup_printf("%s\n\n%s\n%s", description, _("group layout:"), layout->str)
    : g_strdup_printf("%s\n%s", _("group layout:"), layout->str);
  gtk_widget_set_tooltip_text(name, name_tip);
  g_string_free(layout, TRUE);
  g_free(name_tip);
  gtk_box_pack_start(GTK_BOX(box), name, TRUE, TRUE, 0);

  const struct
  {
    _flexi_preset_choice_t how;
    DTGTKCairoPaintIconFunc paint;
    gboolean shown;
  } ways[] = { { _PRESET_REPLACE, dtgtk_cairo_paint_refresh, TRUE },
               { _PRESET_MERGE, dtgtk_cairo_paint_union, n > 1 },
               { _PRESET_ADD, dtgtk_cairo_paint_square_plus, TRUE } };
  for(size_t i = 0; i < G_N_ELEMENTS(ways); i++)
  {
    gchar *tip = ways[i].shown ? _flexi_preset_how_tip(ways[i].how, group) : NULL;
    GtkWidget *button = _flexi_preset_row_button(box, ways[i].paint, tip,
                                                 G_CALLBACK(_flexi_preset_option_clicked), row);
    g_object_set_data(G_OBJECT(button), "how", GINT_TO_POINTER(ways[i].how));
    g_free(tip);
  }
  g_object_set_data_full(G_OBJECT(box), "row", row, _flexi_preset_row_free);
  return box;
}

// a section title between two rules, as the context menus draw theirs (a
// GMenu section label; its widgets are styled by popover.menu in darktable.css)
static GtkWidget *_flexi_preset_section_label(const gchar *text)
{
  GtkWidget *label = gtk_label_new(text);
  gtk_style_context_add_class(gtk_widget_get_style_context(label), "separator");
  GtkWidget *box = dt_gui_hbox();
  gtk_box_pack_start(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), TRUE, TRUE, 0);
  return box;
}

GtkWidget *dt_masks_gui_menu_section(const gchar *text)
{
  return _flexi_preset_section_label(text);
}

static void _flexi_preset_popover_destroyed(GtkWidget *popover, gpointer anchor)
{
  if(darktable.gui && darktable.gui->active_popover_menu == popover)
    darktable.gui->active_popover_menu = NULL;
  g_object_set_data(G_OBJECT(anchor), "presets-popover", NULL);
}

// the popover of the selection's presets button: the group-layout presets,
// each with the buttons that apply it (see _flexi_preset_row_new), and saving
// the selected group's layout as a preset. The buttons act on the group the
// presets button was opened for, which it holds (see _masks_presets_pressed in
// blend_gui.c)
GtkWidget *dt_masks_gui_presets_popover(GtkWidget *anchor, dt_iop_module_t *module)
{
  g_object_set_data(G_OBJECT(anchor), "module", module);
  // the previous one is closed, as no click can reach the button before it is
  GtkWidget *old = g_object_get_data(G_OBJECT(anchor), "presets-popover");
  if(old) gtk_widget_destroy(old);

  GtkWidget *popover = gtk_popover_new(anchor);
  // the look of the context menus (see popover.menu in darktable.css)
  gtk_style_context_add_class(gtk_widget_get_style_context(popover), "menu");
  g_object_set_data(G_OBJECT(anchor), "presets-popover", popover);
  g_signal_connect(popover, "destroy", G_CALLBACK(_flexi_preset_popover_destroyed), anchor);

  GtkWidget *box = dt_gui_vbox(_flexi_preset_section_label(_("built-in presets")));
  dt_gui_add_class(box, "dt_masks_presets_menu");
  GPtrArray *builtins = _flexi_builtins_get();
  for(guint i = 0; i < builtins->len; i++)
  {
    const _flexi_builtin_t *b = g_ptr_array_index(builtins, i);
    dt_gui_box_add(box, _flexi_preset_row_new(anchor, b->id, TRUE, _(b->name),
                                              b->description ? _(b->description) : NULL,
                                              (const dt_masks_layout_node_t *)b->nodes->data,
                                              b->nodes->len));
  }

  // read afresh, for presets changed elsewhere
  _flexi_user_presets_forget();
  GList *user_presets = _flexi_user_presets();
  if(user_presets) dt_gui_box_add(box, _flexi_preset_section_label(_("user defined presets")));
  for(GList *p = user_presets; p; p = g_list_next(p))
  {
    const _flexi_preset_t *preset = p->data;
    dt_gui_box_add(box, _flexi_preset_row_new(anchor, preset->name, FALSE, preset->name,
                                              NULL, preset->nodes, preset->n));
  }

  GtkWidget *save = gtk_model_button_new();
  g_object_set(save, "text",
               g_object_get_data(G_OBJECT(anchor), "presets-group")
                 ? _("save this group's layout as preset...")
                 : _("save the whole mask's layout as preset..."),
               NULL);
  g_signal_connect(save, "clicked", G_CALLBACK(_flexi_preset_save_clicked), anchor);
  dt_gui_box_add(box, _flexi_preset_section_label(_("create new")), save);

  // the rows of many user presets, or a short window, must scroll rather than
  // clip: as the stock menus' pages do (see the popover wrapping in gui/gtk.c),
  // the budget is the toplevel, which a popover cannot leave
  GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
                                 GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
  gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(scroll), TRUE);
  GtkAllocation toplevel;
  gtk_widget_get_allocation(gtk_widget_get_toplevel(anchor), &toplevel);
  if(toplevel.height > 1)
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), toplevel.height);
  gtk_container_add(GTK_CONTAINER(scroll), box);
  gtk_container_add(GTK_CONTAINER(popover), scroll);
  gtk_widget_show_all(scroll);
  return popover;
}

// ---- default preset ---------------------------------------------------------
// the layout a mask starts with: a built-in's id, or a user preset's name
// behind FLEXI_USER_PRESET_PREFIX
#define FLEXI_DEFAULT_PRESET_CONF "plugins/darkroom/masks/default_group_preset"

// the default layout as nodes, with its note keys (a built-in's, owned by the
// preset cache; NULL for a user preset) and whether it is plain. NULL when the
// default names no preset. Caller frees the nodes
static dt_masks_layout_node_t *_default_layout(int *n,
                                               const gchar *const **keys,
                                               gboolean *plain)
{
  dt_masks_layout_node_t *nodes = NULL;
  *n = 0;
  *keys = NULL;
  *plain = TRUE;
  gchar *def = dt_conf_get_string(FLEXI_DEFAULT_PRESET_CONF);
  if(g_str_has_prefix(def, FLEXI_USER_PRESET_PREFIX))
  {
    const gchar *name = def + strlen(FLEXI_USER_PRESET_PREFIX);
    const _flexi_preset_t *preset = _flexi_preset_find(_flexi_user_presets(), name);
    if(preset)
    {
      nodes = g_new(dt_masks_layout_node_t, preset->n);
      memcpy(nodes, preset->nodes, preset->n * sizeof(dt_masks_layout_node_t));
      *n = preset->n;
      *plain = _flexi_layout_is_plain(preset->nodes, preset->n);
    }
  }
  else
  {
    const _flexi_builtin_t *b = _flexi_builtin_by_id(def);
    if(b)
    {
      nodes = _flexi_builtin_nodes(b);
      *n = b->nodes->len;
      *keys = (const gchar *const *)b->keys->pdata;
      *plain = _flexi_builtin_is_plain(b);
    }
  }
  g_free(def);
  return nodes;
}

void dt_masks_gui_apply_default_preset(dt_iop_module_t *module, const gboolean wipe)
{
  if(!wipe && dt_masks_gui_module_mask_group(module)) return;
  // starting a mask, a plain layout is skipped: it is what the mask is anyway
  // before it has a group form, and applying it would only add a history item
  int n;
  const gchar *const *keys;
  gboolean is_plain;
  dt_masks_layout_node_t *nodes = _default_layout(&n, &keys, &is_plain);
  const gboolean applied = nodes && (wipe || !is_plain);
  if(applied) _flexi_layout_apply(module, nodes, n, keys, wipe);
  g_free(nodes);

  // a default that names no preset any more resets to the plain layout
  if(!applied && wipe)
  {
    const dt_masks_layout_node_t plain = { .opacity = 1.0f, .parent = -1 };
    _flexi_layout_apply(module, &plain, 1, NULL, TRUE);
  }
}

void dt_masks_gui_default_layout_build(dt_masks_form_t *grp)
{
  int n;
  const gchar *const *keys;
  gboolean is_plain;
  dt_masks_layout_node_t *nodes = _default_layout(&n, &keys, &is_plain);
  if(nodes && !is_plain)
    dt_masks_model_layout_build_at(grp, dt_masks_group_marker(grp), nodes, n, keys, FALSE, NULL);
  g_free(nodes);
}

void dt_masks_gui_default_layout_root(dt_masks_point_group_t *root)
{
  if(!root) return;
  gchar *def = dt_conf_get_string(FLEXI_DEFAULT_PRESET_CONF);
  if(!_cached_default_root_valid || g_strcmp0(def, _cached_default_preset) != 0)
  {
    g_free(_cached_default_preset);
    _cached_default_preset = def;
    int n;
    const gchar *const *keys;
    gboolean is_plain;
    dt_masks_layout_node_t *nodes = _default_layout(&n, &keys, &is_plain);
    _cached_default_head = nodes && n > 0;
    if(_cached_default_head)
    {
      _cached_default_node = nodes[0];
      dt_strlcpy_to_fixed(_cached_default_key, keys && keys[0] ? keys[0] : "",
                          sizeof(_cached_default_key));
    }
    g_free(nodes);
    _cached_default_root_valid = TRUE;
  }
  else
  {
    g_free(def);
  }
  // patch the caller's group rather than replacing it: its formid and marker
  // bit are its own, and so are all its fields when the default names no preset
  if(_cached_default_head)
    _flexi_layout_set_head(root, &_cached_default_node, _cached_default_key);
}

#define _DEFAULT_PRESET_TIP                                              \
  _("the group layout preset a module's mask starts with\n"              \
    "when it is switched on for the first time")

// a built-in's description has no entry of its own to sit on: the combo box
// tip adds it while that entry is the selected one
static void _masks_default_preset_tip(GtkWidget *combo, const gchar *id)
{
  const _flexi_builtin_t *b = _flexi_builtin_by_id(id);
  gchar *tip = b && b->description
                 ? g_strdup_printf("%s\n\n%s", _DEFAULT_PRESET_TIP, _(b->description))
                 : g_strdup(_DEFAULT_PRESET_TIP);
  gtk_widget_set_tooltip_text(combo, tip);
  g_free(tip);
}

static void _masks_default_preset_changed(GtkWidget *combo, gpointer user_data)
{
  DT_GUARD_GUI_UPDATE();
  const gchar *id = dt_bauhaus_combobox_get_data(combo);
  if(!id) return;
  _default_layout_cache_invalidate();
  dt_conf_set_string(FLEXI_DEFAULT_PRESET_CONF, id);
  _masks_default_preset_tip(combo, id);
}

static void _masks_default_preset_entry(GtkWidget *combo,
                                        const gchar *label,
                                        gchar *value,
                                        const gchar *current)
{
  dt_bauhaus_combobox_add_full(combo, label, DT_BAUHAUS_COMBOBOX_ALIGN_LEFT, value, g_free, TRUE);
  if(!g_strcmp0(value, current))
    dt_bauhaus_combobox_set(combo, dt_bauhaus_combobox_length(combo) - 1);
}

void dt_masks_gui_add_default_preset_box(GtkWidget *box)
{
  gchar *current = dt_conf_get_string(FLEXI_DEFAULT_PRESET_CONF);
  GtkWidget *combo = dt_masks_gui_pref_combo(box, _("default layout for new masks"),
                                             _DEFAULT_PRESET_TIP);

  // state first, handler after, as the panel position combo box does
  DT_ENTER_GUI_UPDATE();
  GPtrArray *builtins = _flexi_builtins_get();
  for(guint i = 0; i < builtins->len; i++)
  {
    const _flexi_builtin_t *b = g_ptr_array_index(builtins, i);
    _masks_default_preset_entry(combo, _(b->name), g_strdup(b->id), current);
  }
  GList *user_presets = _flexi_preset_list_load();
  for(GList *p = user_presets; p; p = g_list_next(p))
  {
    const _flexi_preset_t *preset = p->data;
    _masks_default_preset_entry(
      combo, preset->name, g_strconcat(FLEXI_USER_PRESET_PREFIX, preset->name, NULL), current);
  }
  _flexi_preset_list_free(user_presets);
  DT_LEAVE_GUI_UPDATE();

  _masks_default_preset_tip(combo, current);
  g_signal_connect(G_OBJECT(combo), "value-changed", G_CALLBACK(_masks_default_preset_changed),
                   NULL);
  g_free(current);
}
#undef _DEFAULT_PRESET_TIP

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
