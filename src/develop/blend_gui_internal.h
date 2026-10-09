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

// the seam between the flexi masks panel's files: blend_gui.c,
// masks_gui_presets.c, masks_gui_toolbar.c and masks_gui_panel_host.c. Not
// public API: what an IOP or the pipe calls goes in blend.h.
//
// Keep it small: a symbol is here only while a caller in another panel file,
// or in the panel's model tests, needs it; otherwise it is static

#include "develop/blend.h"
#include "develop/masks.h"

G_BEGIN_DECLS

/** the group a newly added element lands in, by its marker id: the selected
    one, or else the mask's own. INVALID_MASKID for a mask that has no group
    form yet (see dt_masks_gui_module_flexi_group) */
dt_mask_id_t dt_masks_gui_resolve_add_target(dt_iop_module_t *module);

// ---------------------------------------------------------------------------
// blend_gui.c -> masks_gui_presets.c
// ---------------------------------------------------------------------------

// a group's operator: how it folds its own members together, in list order
// (dev-doc/masks_data_model.md). Each name is the operation, then its
// family: the three unions and the two intersections agree on solid shapes
// and differ only in how partial opacities combine. Order matches the menu.
// `short_name`, the operation alone, names a group that has no name of its
// own and the operator in the group layout presets file. `formula` ends the
// tooltip: the fold of the mask so far `a` with the next member `b` (group.c
// dt_masks_combine_*). It is not translated: a formula reads the same in
// every language
typedef struct dt_masks_flexi_op_t
{
  dt_masks_state_t bit; // 0 = maximum (no flexi operator bit)
  DTGTKCairoPaintIconFunc paint;
  const char *name;
  const char *short_name;
  const char *tooltip;
  const char *formula;
} dt_masks_flexi_op_t;

#define DT_MASKS_FLEXI_OPS_COUNT 7
extern const dt_masks_flexi_op_t dt_masks_flexi_ops[DT_MASKS_FLEXI_OPS_COUNT];

/** what the refinement controls target (dt_iop_gui_blend_data_t's
    masks_refine_scope_kind); see the scope comment in blend_gui.c */
enum
{
  REFINE_SCOPE_GLOBAL = 0,
  REFINE_SCOPE_ELEMENT,
  REFINE_SCOPE_GROUP
};

/** the module's mask group, or NULL if it has none / it is not a group */
dt_masks_form_t *dt_masks_gui_module_mask_group(dt_iop_module_t *module);
/** the module's flexi group, created with its first group if it has no group
    form yet. *cid, when given and invalid, becomes that first group's id */
dt_masks_form_t *dt_masks_gui_module_flexi_group(dt_iop_module_t *module, dt_mask_id_t *cid);
/** the point for `id` within `grp` -- a member, or a group's marker -- or NULL */
dt_masks_point_group_t *dt_masks_gui_group_point(dt_masks_form_t *grp, const dt_mask_id_t id);
/** remove every shape and reset the panel's scratch state (no confirmation) */
void dt_masks_gui_reset_mask_core(dt_iop_module_t *module);
/** destroy and rebuild the panel's row tree */
void dt_masks_gui_build_list(dt_iop_module_t *module);
/** re-arm on-canvas editing after the mask changed underneath it */
void dt_masks_gui_refresh_canvas_edit(dt_iop_module_t *module);

// ---------------------------------------------------------------------------
// blend_gui.c -> the flexi panel model tests
// (src/tests/unittests/masks/test_flexi_model.c)
// ---------------------------------------------------------------------------
//
// the panel's group model: functions over the mask's tree of groups, without
// GTK or widget state. A group's list holds its marker (see
// DT_MASKS_STATE_GROUP_MARKER) first, then its members. Every change the
// panel makes to the mask's structure goes through these, so that it can be
// tested without a display. Keep them stable: the model tests depend on them

/** the member ids of the group `id` is in (`id` a member or the group's
    marker), top-first. Caller frees the list. */
GList *dt_masks_model_group_members(dt_masks_form_t *grp, const dt_mask_id_t id);
/** the id of the group `fid` is in -- its marker's -- or INVALID_MASKID */
dt_mask_id_t dt_masks_gui_group_cid_of_form(dt_masks_form_t *grp, const dt_mask_id_t fid);
/** a new empty nested group on top of the members of group `cid`, folding
    its members with the flexi operator `flexi_op`. Returns that group's
    id, or INVALID_MASKID where groups may not nest any deeper */
dt_mask_id_t dt_masks_model_nest_new_group(dt_masks_form_t *grp,
                                           const dt_masks_state_t flexi_op,
                                           const dt_mask_id_t cid);
/** "compose": put the element or group of point `pt` (a member, or a group's
    marker) into a new group folding with `flexi_op`, where it was, on top of
    it a new empty group to fill. The mask's own group cannot move, so its
    members and settings go into a new group instead, and it keeps nothing
    but `flexi_op`. Returns the empty group's id, or INVALID_MASKID where
    groups may not nest any deeper */
dt_mask_id_t dt_masks_model_compose(dt_masks_form_t *grp,
                                    const dt_masks_point_group_t *pt,
                                    const dt_masks_state_t flexi_op);
/** the reverse for the mask's own group: when it applies nothing and holds
    one plain nested group, that group's members and settings become its own.
    `whole` is the module's refinement, which takes the group's. FALSE where
    that would not render the same */
gboolean dt_masks_model_hoist_sole_group(dt_masks_form_t *grp, dt_masks_refinement_t *whole);
/** remove the members of group `cid`, keeping the group. Returns their ids,
    which the caller frees */
GList *dt_masks_model_empty_group(dt_masks_form_t *grp, const dt_mask_id_t cid);

// what the gestures do, apart from their GTK handlers so that the tests run
// the same code. They change the mask structure and the panel's selection,
// not history, the pipe or the widgets: the handler commits (see
// dt_masks_model_drop_element_onto_element in blend_gui.c)

/** move element `src` into `dst`'s group, landing above or below it */
gboolean dt_masks_model_drop_element_onto_element(dt_iop_module_t *module,
                                                  dt_masks_form_t *grp,
                                                  const dt_mask_id_t src,
                                                  const dt_mask_id_t dst,
                                                  const gboolean above);
/** the same for the references `sp` and `dp` themselves, where the mask holds
    either shape twice and the form id alone names the first reference */
gboolean dt_masks_model_drop_point_onto_point(dt_iop_module_t *module,
                                              dt_masks_form_t *grp,
                                              const dt_masks_point_group_t *sp,
                                              const dt_masks_point_group_t *dp,
                                              const gboolean above);

/** a panel selection: an element, and the group it sits in. Either may be
    INVALID_MASKID -- an element is never selected without its group, but a
    group is routinely selected on its own. */
typedef struct dt_masks_panel_sel_t
{
  dt_mask_id_t formid;
  dt_mask_id_t group_cid;
} dt_masks_panel_sel_t;

/** the selection a click on element `id` produces */
dt_masks_panel_sel_t dt_masks_model_click_element(const dt_iop_gui_blend_data_t *bd,
                                                  dt_masks_form_t *grp,
                                                  const dt_mask_id_t id);
/** the selection a click on group `cid` produces */
dt_masks_panel_sel_t dt_masks_model_click_group(const dt_iop_gui_blend_data_t *bd,
                                                const dt_mask_id_t cid);

/** move element `src` into the group `dst` is in (`dst` a member or the
    group's marker), landing on top of it */
gboolean dt_masks_model_drop_element_onto_group(dt_iop_module_t *module,
                                                dt_masks_form_t *grp,
                                                const dt_mask_id_t src,
                                                const dt_mask_id_t dst);
/** the same for the reference `sp` itself (see dt_masks_model_drop_point_onto_point) */
gboolean dt_masks_model_drop_point_onto_group(dt_iop_module_t *module,
                                              dt_masks_form_t *grp,
                                              const dt_masks_point_group_t *sp,
                                              const dt_mask_id_t dst);
/** drop every member whose form is gone from dev->forms; how many went */
int dt_masks_model_prune_dangling_members(dt_masks_form_t *grp);
/** move a whole same-kind cluster onto an element row or a group header */
gboolean dt_masks_gui_cluster_move(dt_iop_module_t *module,
                                   GList *member_ids,
                                   const dt_mask_id_t dst,
                                   const gboolean dst_is_group,
                                   const gboolean above);
/** the nested group form whose marker is `cid`, or NULL: for the mask's own
    group, and for what is no group's marker */
dt_masks_form_t *dt_masks_model_nested_group_of(dt_masks_form_t *grp, const dt_mask_id_t cid);
/** a group dropped on group `dst_cid`: right above or below it, or with
    `inside` on top of its members. A group moves as the member its holder has
    for it, so the mask's own group never moves, and nothing lands beside it.
    FALSE where it may not go */
gboolean dt_masks_model_move_group(dt_iop_module_t *module,
                                   const dt_mask_id_t src_cid,
                                   const dt_mask_id_t dst_cid,
                                   const gboolean above,
                                   const gboolean inside);

/** what a solo-family toggle leaves for its caller to do to the canvas edit
    scope. The model half never touches the canvas itself. */
typedef enum dt_masks_solo_canvas_t
{
  DT_MASKS_SOLO_CANVAS_NONE = 0, // nothing to do
  DT_MASKS_SOLO_CANVAS_FULL,     // restore whole-group editing
  DT_MASKS_SOLO_CANVAS_ONE,      // narrow editing to bd->soloedit_formid
} dt_masks_solo_canvas_t;

/* solo, group solo and solo edit exclude each other, and at most one element
   or one group is soloed: each of these three cancels the other two */
dt_masks_solo_canvas_t dt_masks_model_toggle_solo_form(dt_iop_module_t *module,
                                                       dt_masks_form_t *grp,
                                                       const dt_mask_id_t id);
dt_masks_solo_canvas_t dt_masks_model_toggle_solo_group(dt_iop_module_t *module,
                                                        dt_masks_form_t *grp,
                                                        const guint key,
                                                        GList *members);
dt_masks_solo_canvas_t dt_masks_model_toggle_soloedit(dt_iop_module_t *module,
                                                      dt_masks_form_t *grp,
                                                      const dt_mask_id_t id);

/** does an element row of this kind carry an expander chevron of its own?
    Every element has at least its opacity slider to show. `props_subpanel` is
    "element properties in subpanel", which leaves only a parametric row's
    in/out chevron. */
gboolean dt_masks_model_row_is_expandable(const dt_masks_type_t type,
                                          const gboolean props_subpanel);

/** which element "auto-expand selected" keeps open: the selection if it can be
    expanded at all, else whatever was expanded last. Resolves the form's kind
    through darktable.develop. */
dt_mask_id_t dt_masks_model_auto_expand_anchor(const dt_iop_gui_blend_data_t *bd);

/** what a real click on an element row's chevron does to "auto-expand
    selected": the row to collapse (INVALID_MASKID for none) and the element
    the option considers open afterwards. */
typedef struct dt_masks_chevron_click_t
{
  dt_mask_id_t collapse;
  dt_mask_id_t last_expanded;
} dt_masks_chevron_click_t;

dt_masks_chevron_click_t dt_masks_model_element_chevron_click(const dt_iop_gui_blend_data_t *bd,
                                                              const dt_mask_id_t id,
                                                              const gboolean expanded,
                                                              const gboolean auto_expand);
/** the same, one level up: which group it keeps open. Every group can be
    expanded, so this needs no kind test. */
dt_mask_id_t dt_masks_model_auto_expand_group_anchor(const dt_iop_gui_blend_data_t *bd);

/** has the user touched this channel's input (0) / output (1) sub-range? */
gboolean dt_masks_gui_param_channel_is_used(const dt_masks_point_parametric_t *p,
                                            const dt_iop_gui_blendif_channel_t *channel,
                                            const int in_out);

/** which controls a parametric row shows */
typedef struct dt_masks_param_vis_t
{
  gboolean input;
  gboolean output;
  gboolean boost;
  gboolean bypass;
  /** the full opacity slider leading the expanded controls */
  gboolean opacity;
} dt_masks_param_vis_t;

dt_masks_param_vis_t dt_masks_model_param_row_visibility(const gboolean expanded,
                                                         const gboolean in_used,
                                                         const gboolean out_used,
                                                         const gboolean boost_enabled,
                                                         const gboolean props_subpanel);

/** state transition decisions for the mask panel and corner icon */
typedef struct dt_masks_panel_state_t
{
  gboolean want_hosted;
  gboolean panel_collapsed;
  gboolean corner_icon_visible;
  gboolean corner_icon_active;
} dt_masks_panel_state_t;

dt_masks_panel_state_t dt_masks_model_panel_state(const int pos,
                                                  const gboolean is_focused,
                                                  const gboolean has_masking,
                                                  const gboolean is_expanded,
                                                  const gboolean mask_active,
                                                  const gboolean panel_pref_collapsed);

gboolean dt_masks_model_pin_should_expand_iop(const gboolean is_expanded,
                                              const gboolean is_collapsed);

char *dt_masks_model_panel_header_markup(const char *module_name,
                                        const char *instance_name,
                                        const gboolean is_hosted);

/** the shape solo-edit mode should be isolating, given the panel's selection,
    or INVALID_MASKID if the mode must stand down */
dt_mask_id_t dt_masks_model_soloedit_target(dt_iop_gui_blend_data_t *bd);
/** what the "element properties in subpanel" section shows, and whose */
typedef struct dt_masks_props_target_t
{
  /** the selected element, or else the selected group; INVALID_MASKID when
      it has nothing to show there */
  dt_mask_id_t id;
  gboolean is_group;
  /** a drawn shape's size, feather and the rest */
  gboolean shape;
  /** a full opacity slider: always, for anything selected */
  gboolean opacity;
  /** a parametric channel's boost factor, for a channel that has one */
  gboolean boost;
} dt_masks_props_target_t;

dt_masks_props_target_t dt_masks_model_props_panel_target(const dt_iop_gui_blend_data_t *bd);

/** the collapsible sections of the mask panel below the list. Each keeps one
    folded state, the same for every module and target */
typedef enum dt_masks_section_t
{
  DT_MASKS_SECTION_REFINE,
  DT_MASKS_SECTION_PROPS,
  DT_MASKS_SECTION_CONSUMERS,
  DT_MASKS_SECTION_COUNT
} dt_masks_section_t;

/** whether a section shows unfolded: its saved state, except that the
    properties section opens while it holds the creation controls of a shape
    being drawn, without that being saved */
gboolean dt_masks_model_section_expanded(const dt_masks_section_t section, const gboolean drawing);
/** save a section's folded state, as a click on its toggle does */
void dt_masks_model_section_save(const dt_masks_section_t section, const gboolean expanded);

/** geometry for dt_masks_model_whisker_popup_rect, all in root (screen) coordinates */
typedef struct dt_masks_whisker_geom_t
{
  GdkRectangle anchor;   // the widget the popup belongs to
  gint center_x;         // where the popup wants to be centered
  GdkRectangle workarea; // the monitor's usable area
  gint panel_x;          // horizontal bounds the popup is held within: the
  gint panel_w;          //   host panel, or the work area outside one
  gint size;             // the popup is square
  gint gap;              // clearance kept between the popup and the anchor
} dt_masks_whisker_geom_t;

/** where a parametric slider's precise-entry popup goes */
GdkRectangle dt_masks_model_whisker_popup_rect(const dt_masks_whisker_geom_t *g);

/** a parametric channel's [0,1] slider fraction as the number the user reads
    and types, and back. Round-tripping has to be exact: it is how a typed
    value reaches the node, and the hue span it produces has to be exactly 360
    for bauhaus to treat the popup as an angle (see _is_full_circle). */
float dt_masks_gui_param_row_slider_precise_display(const dt_iop_gui_blendif_channel_t *channel,
                                                    const float boost_factor,
                                                    const float frac);
float dt_masks_gui_param_row_slider_precise_parse(const dt_iop_gui_blendif_channel_t *channel,
                                                  const float boost_factor,
                                                  const float typed);

/** every module whose own mask uses form `fid`: more than one means the form
    is linked. Caller frees the list. */
GList *dt_masks_model_form_users(const dt_mask_id_t fid);
/** the shapes and AI objects in `src`'s mask, nested groups' included,
    bottom-up. Caller frees. */
GList *dt_masks_model_module_shapes(dt_iop_module_t *src);
/** add `fids` (bottom-up) where the insertion hint points, linked or, with
    `copy`, as independent copies. Each keeps the opacity, invert state and
    refinement it has in `src`'s mask (NULL: defaults); forms the mask already
    uses are skipped. Returns the ids added, bottom-up. Records no history. */
GList *dt_masks_model_import_forms(dt_iop_module_t *module,
                                   dt_iop_module_t *src,
                                   GList *fids,
                                   const gboolean copy);
/** replace linked form `fid` in `module`'s mask with its own copy, for the
    reference `pt`, carrying the panel's references over: a mask can hold the
    same shape twice, and each reference unlinks on its own (NULL pt: the
    first one the mask holds). Returns the copy's id. Records no history. */
dt_mask_id_t dt_masks_model_unlink_form_point(dt_iop_module_t *module,
                                              const dt_mask_id_t fid,
                                              dt_masks_point_group_t *pt);
/** the name a row shows: the form's own without its type prefix, or for a
    raster element named by its type alone, its source's current name */
gchar *dt_masks_gui_form_display_name(const dt_masks_form_t *form);
/** rename from the row's entry, which edits the part after the type prefix;
    an empty name sets a raster element to follow its source. TRUE if changed */
gboolean dt_masks_model_rename_form(dt_masks_form_t *form, const char *txt);
/** a shared element: another module's mask uses it too. Never a raster one */
gboolean dt_masks_model_form_is_linked(const dt_masks_form_t *form);
/** point the refinement scope at what the panel selection names: the
    element, its group, a staged group, or the whole mask */
void dt_masks_model_refine_scope_from_selection(dt_iop_module_t *module);
/** forget a refinement scope whose element or group left the mask, with the
    selections naming it. TRUE when the scope must be derived again */
gboolean dt_masks_model_refine_scope_prune(dt_iop_module_t *module);
/** the row a canvas selection names: a path of an AI object maps to the object */
dt_mask_id_t dt_masks_model_panel_formid_for(dt_iop_module_t *module, const dt_mask_id_t formid);
/** everything the element list is built from; an unchanged one skips the rebuild */
dt_hash_t dt_masks_gui_list_signature(dt_iop_module_t *module);

/* group numbering: a group's number is an identity, which must survive the
   group emptying and refilling */
/** the numbering series of a group's flexi operator in `state`: 0 for the
    unions, 1 for the intersections, 2 for difference, 3 for exclusion */
int dt_masks_gui_flexi_op_index_for_state(const int state);
/** the highest number a live group of series `mode` holds, 0 if none: a new
    group takes the next one */
int dt_masks_gui_group_ord_max_for_flexi_op(dt_iop_module_t *module, const int mode);
/** the number of group `cid` in its series, from 1, assigned on first ask and
    kept while the group exists */
int dt_masks_gui_group_ordinal_of_cid(dt_iop_module_t *module, const dt_mask_id_t cid);
/** forget the numbers of groups that no longer exist, so that an emptied
    series restarts at 1 */
void dt_masks_gui_prune_group_ordinals(dt_iop_module_t *module);
/** clear the group solo when the soloed group no longer exists */
void dt_masks_gui_prune_stale_solo(dt_iop_module_t *module);

// ---------------------------------------------------------------------------
// masks_gui_presets.c -> blend_gui.c
// ---------------------------------------------------------------------------

/** append the "group layout presets" section to an existing menu */
void dt_masks_gui_add_presets_menu(GMenu *menu, GtkWidget *anchor, dt_iop_module_t *module);
/** a preset group's note pages, untranslated, from its
    dt_masks_point_group_t.preset_note key; NULL when the key names none. Owned
    by the preset cache: valid until the presets file is next read */
GPtrArray *dt_masks_gui_preset_notes(const char *key);
/** the "show preset notes" option */
gboolean dt_masks_gui_preset_notes_shown(void);
/** give a mask that has no group form yet the default group layout */
void dt_masks_gui_apply_default_preset(dt_iop_module_t *module);
/** append the "default group layout" section to the panel options popover */
void dt_masks_gui_add_default_preset_box(GtkWidget *box);

// ---------------------------------------------------------------------------
// masks_gui_toolbar.c -> blend_gui.c
// ---------------------------------------------------------------------------

/** the add-buttons toolbar: add group, shapes, parametric channels and
    import, centered on one line when they fit and on two or three rows
    otherwise, presets at the top right. gap is an empty widget whose width
    spaces them */
GtkWidget *dt_masks_gui_toolbar_new(GtkWidget *group,
                                    GtkWidget *shapes,
                                    GtkWidget *channels,
                                    GtkWidget *import,
                                    GtkWidget *presets,
                                    GtkWidget *gap);

// ---------------------------------------------------------------------------
// blend_gui.c -> masks_gui_panel_host.c
// ---------------------------------------------------------------------------

/** a section title in the panel options popover; `tip` may be NULL */
void dt_masks_gui_pref_section(GtkWidget *box, const gchar *title, const gchar *tip);
/** re-home a widget into a new parent (no-op if already there), keeping its
    shown state */
void dt_masks_gui_reparent_into(GtkWidget *w, GtkWidget *parent,
                                const gboolean at_end, const gboolean expand);


// ---------------------------------------------------------------------------
// masks_gui_panel_host.c -> blend_gui.c
// ---------------------------------------------------------------------------

/** the utility lib's header with no module hosted: titled for no module, its
    arrow and title insensitive */
void dt_masks_gui_utility_header_unhosted(struct dt_lib_module_t *host);
/** move this module's panel content back into its own expander */
void dt_masks_gui_flexi_release(dt_iop_module_t *module);
/** "collapse" button shown in the "blend mask" header */
void dt_masks_gui_flexi_inline_collapse_clicked(GtkWidget *w, gpointer user_data);
/** record whether the masking panel should be folded away. Shared by all three
    positions, and only ever *applied* by dt_iop_gui_blend_masks_panel_relocate/-release, so a
    caller states the intent and every position carries it out the same way */
void dt_masks_gui_panel_set_collapsed_pref(const gboolean collapsed);
// the position preference (MASKS_PANEL_POS_*)
int dt_masks_gui_panel_position(void);
// which edge the canvas-side panel is docked against, and where pinning it
// records that -- not part of the position choice (see MASKS_PANEL_POS_CANVAS)
gboolean dt_masks_gui_panel_side_right(void);
/** append the "blend mask panel position" section to an existing menu */
void dt_masks_gui_add_panel_position_box(GtkWidget *box, dt_iop_module_t *module);

G_END_DECLS

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
