/*
    This file is part of darktable,
    Copyright (C) 2011-2025 darktable developers.

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

#include "common/iop_profile.h"
#include "common/opencl.h"
#include "develop/pixelpipe.h"
#include "develop/masks.h"
#include "dtgtk/button.h"
#include "dtgtk/gradientslider.h"
#include "gui/color_picker_proxy.h"
#include "common/imagebuf.h"
#include "common/gaussian.h"

#define DEVELOP_BLEND_VERSION (15)

// where the flexi masks panel lives ("plugins/darkroom/blend/masks_panel_position")
#define MASKS_PANEL_POS_EMBEDDED 0
#define MASKS_PANEL_POS_UTILITY  1
// the side is not part of the position (see dt_masks_gui_panel_side_right)
#define MASKS_PANEL_POS_CANVAS   2

G_BEGIN_DECLS

typedef enum dt_develop_blend_colorspace_t
{
  DEVELOP_BLEND_CS_NONE = 0,
  DEVELOP_BLEND_CS_RAW = 1,
  DEVELOP_BLEND_CS_LAB = 2,
  DEVELOP_BLEND_CS_RGB_DISPLAY = 3,
  DEVELOP_BLEND_CS_RGB_SCENE = 4,
} dt_develop_blend_colorspace_t;

typedef enum dt_develop_blend_mode_t
{
  DEVELOP_BLEND_DISABLED_OBSOLETE = 0x00, /* same as the new normal */
  DEVELOP_BLEND_NORMAL_OBSOLETE = 0x01, /* obsolete as it did clamping */
  DEVELOP_BLEND_LIGHTEN = 0x02,
  DEVELOP_BLEND_DARKEN = 0x03,
  DEVELOP_BLEND_MULTIPLY = 0x04,
  DEVELOP_BLEND_AVERAGE = 0x05,
  DEVELOP_BLEND_ADD = 0x06,
  DEVELOP_BLEND_SUBTRACT = 0x07,
  DEVELOP_BLEND_DIFFERENCE = 0x08, /* deprecated */
  DEVELOP_BLEND_SCREEN = 0x09,
  DEVELOP_BLEND_OVERLAY = 0x0A,
  DEVELOP_BLEND_SOFTLIGHT = 0x0B,
  DEVELOP_BLEND_HARDLIGHT = 0x0C,
  DEVELOP_BLEND_VIVIDLIGHT = 0x0D,
  DEVELOP_BLEND_LINEARLIGHT = 0x0E,
  DEVELOP_BLEND_PINLIGHT = 0x0F,
  DEVELOP_BLEND_LIGHTNESS = 0x10,
  DEVELOP_BLEND_CHROMATICITY = 0x11,
  DEVELOP_BLEND_HUE = 0x12,
  DEVELOP_BLEND_COLOR = 0x13,
  DEVELOP_BLEND_INVERSE_OBSOLETE = 0x14, /* obsolete */
  DEVELOP_BLEND_UNBOUNDED_OBSOLETE = 0x15, /* obsolete as new normal takes over */
  DEVELOP_BLEND_COLORADJUST = 0x16,
  DEVELOP_BLEND_DIFFERENCE2 = 0x17,
  DEVELOP_BLEND_NORMAL2 = 0x18,
  DEVELOP_BLEND_BOUNDED = 0x19,
  DEVELOP_BLEND_LAB_LIGHTNESS = 0x1A,
  DEVELOP_BLEND_LAB_COLOR = 0x1B,
  DEVELOP_BLEND_HSV_VALUE = 0x1C,
  DEVELOP_BLEND_HSV_COLOR = 0x1D,
  DEVELOP_BLEND_LAB_L = 0x1E,
  DEVELOP_BLEND_LAB_A = 0x1F,
  DEVELOP_BLEND_LAB_B = 0x20,
  DEVELOP_BLEND_RGB_R = 0x21,
  DEVELOP_BLEND_RGB_G = 0x22,
  DEVELOP_BLEND_RGB_B = 0x23,
  DEVELOP_BLEND_MULTIPLY_REVERSE_OBSOLETE = 0x24, /* obsoleted by MULTIPLY + REVERSE */
  DEVELOP_BLEND_SUBTRACT_INVERSE = 0x25,
  DEVELOP_BLEND_DIVIDE = 0x26,
  DEVELOP_BLEND_DIVIDE_INVERSE = 0x27,
  DEVELOP_BLEND_GEOMETRIC_MEAN = 0x28,
  DEVELOP_BLEND_HARMONIC_MEAN = 0x29,

  DEVELOP_BLEND_REVERSE = 0x80000000,
  DEVELOP_BLEND_MODE_MASK = 0xFF,
} dt_develop_blend_mode_t;

typedef enum dt_develop_mask_mode_t
{
  DEVELOP_MASK_DISABLED = 0,                                                         // off
  DEVELOP_MASK_ENABLED = 1,                                                          // uniformly
  DEVELOP_MASK_MASK = 1 << 1,                                                        // drawn mask
  DEVELOP_MASK_CONDITIONAL = 1 << 2,                                                 // parametric mask
  DEVELOP_MASK_RASTER = 1 << 3,                                                      // raster mask
  DEVELOP_MASK_MASK_CONDITIONAL = (DEVELOP_MASK_MASK | DEVELOP_MASK_CONDITIONAL),    // drawn & parametric
  DEVELOP_MASK_FLEXI = 1 << 4                                                        // flexi mask: a tree of groups
} dt_develop_mask_mode_t;

typedef enum dt_develop_mask_combine_mode_t
{
  DEVELOP_COMBINE_NORM = 0x00,
  DEVELOP_COMBINE_INV = 0x01,
  DEVELOP_COMBINE_EXCL = 0x00,
  DEVELOP_COMBINE_INCL = 0x02,
  DEVELOP_COMBINE_MASKS_POS = 0x04,
  DEVELOP_COMBINE_NORM_EXCL = (DEVELOP_COMBINE_NORM | DEVELOP_COMBINE_EXCL),
  DEVELOP_COMBINE_NORM_INCL = (DEVELOP_COMBINE_NORM | DEVELOP_COMBINE_INCL),
  DEVELOP_COMBINE_INV_EXCL = (DEVELOP_COMBINE_INV | DEVELOP_COMBINE_EXCL),
  DEVELOP_COMBINE_INV_INCL = (DEVELOP_COMBINE_INV | DEVELOP_COMBINE_INCL)
} dt_develop_mask_combine_mode_t;

typedef enum dt_develop_mask_feathering_guide_t
{
  DEVELOP_MASK_GUIDE_IN_BEFORE_BLUR = 0x01,
  DEVELOP_MASK_GUIDE_OUT_BEFORE_BLUR = 0x02,
  DEVELOP_MASK_GUIDE_IN_AFTER_BLUR = 0x05,
  DEVELOP_MASK_GUIDE_OUT_AFTER_BLUR = 0x06,
} dt_develop_mask_feathering_guide_t;

typedef enum dt_develop_blendif_channels_t
{
  DEVELOP_BLENDIF_L_in = 0,
  DEVELOP_BLENDIF_A_in = 1,
  DEVELOP_BLENDIF_B_in = 2,

  DEVELOP_BLENDIF_L_out = 4,
  DEVELOP_BLENDIF_A_out = 5,
  DEVELOP_BLENDIF_B_out = 6,

  DEVELOP_BLENDIF_GRAY_in = 0,
  DEVELOP_BLENDIF_RED_in = 1,
  DEVELOP_BLENDIF_GREEN_in = 2,
  DEVELOP_BLENDIF_BLUE_in = 3,

  DEVELOP_BLENDIF_GRAY_out = 4,
  DEVELOP_BLENDIF_RED_out = 5,
  DEVELOP_BLENDIF_GREEN_out = 6,
  DEVELOP_BLENDIF_BLUE_out = 7,

  DEVELOP_BLENDIF_C_in = 8,
  DEVELOP_BLENDIF_h_in = 9,

  DEVELOP_BLENDIF_C_out = 12,
  DEVELOP_BLENDIF_h_out = 13,

  DEVELOP_BLENDIF_H_in = 8,
  DEVELOP_BLENDIF_S_in = 9,
  DEVELOP_BLENDIF_l_in = 10,

  DEVELOP_BLENDIF_H_out = 12,
  DEVELOP_BLENDIF_S_out = 13,
  DEVELOP_BLENDIF_l_out = 14,

  DEVELOP_BLENDIF_Jz_in = 8,
  DEVELOP_BLENDIF_Cz_in = 9,
  DEVELOP_BLENDIF_hz_in = 10,

  DEVELOP_BLENDIF_Jz_out = 12,
  DEVELOP_BLENDIF_Cz_out = 13,
  DEVELOP_BLENDIF_hz_out = 14,

  DEVELOP_BLENDIF_MAX = 14,
  DEVELOP_BLENDIF_unused = 15,

  DEVELOP_BLENDIF_active = 31,

  DEVELOP_BLENDIF_SIZE = 16,

  DEVELOP_BLENDIF_Lab_MASK = 0x3377,
  DEVELOP_BLENDIF_RGB_MASK = 0x77FF,
  DEVELOP_BLENDIF_OUTPUT_MASK = 0xF0F0
} dt_develop_blendif_channels_t;


/** blend parameters current version */
typedef struct dt_develop_blend_params_t
{
  /** what kind of masking to use: off, non-mask (uniformly),
   *  hand-drawn mask and/or conditional mask or raster mask */
  uint32_t mask_mode;
  /** blending color space type */
  int32_t blend_cst;
  /** blending mode */
  uint32_t blend_mode;
  /** parameter for the blending */
  float blend_parameter;
  /** mixing opacity */
  float opacity;
  /** how masks are combined */
  uint32_t mask_combine;
  /** id of mask in current pipeline */
  dt_mask_id_t mask_id;
  /** blendif mask */
  uint32_t blendif;
  /** feathering radius */
  float feathering_radius;
  /** feathering guide */
  uint32_t feathering_guide;
  /** blur radius */
  float blur_radius;
  /** mask contrast enhancement */
  float contrast;
  /** mask brightness adjustment */
  float brightness;
  /** details threshold */
  float details;
  /** feathering parameters version */
  uint32_t feather_version;
  /** nonzero: reset, presets, styles and paste leave the mask alone (see
   *  dt_develop_blend_keep_locked_mask). It took over a reserved field, which
   *  older versions do not keep zero, so every legacy conversion clears it */
  uint32_t mask_lock;
  /** some reserved fields for future use */
  uint32_t reserved[1];
  /** blendif parameters */
  float blendif_parameters[4 * DEVELOP_BLENDIF_SIZE];
  float blendif_boost_factors[DEVELOP_BLENDIF_SIZE];
  dt_dev_operation_t raster_mask_source;
  int raster_mask_instance;
  dt_mask_id_t raster_mask_id;
  gboolean raster_mask_invert;
} dt_develop_blend_params_t;

/** point struct for a DT_MASKS_PARAMETRIC form: its own copy of the blendif
    configuration, mirroring the blendif fields of dt_develop_blend_params_t,
    so that several parametric masks can be elements of one group */
typedef struct dt_masks_point_parametric_t
{
  uint32_t blendif;                            // active channel flags (+ polarity)
  float blendif_parameters[4 * DEVELOP_BLENDIF_SIZE];
  float blendif_boost_factors[DEVELOP_BLENDIF_SIZE];
  uint32_t colorspace;                         // dt_develop_blend_colorspace_t the form was made in
  // the form edits one blendif channel, `channel`, an index into its
  // colorspace's channel table. Input and output ranges of the channel both
  // refine the mask, as in classic blendif; `in_out` only says whether the
  // output range is shown
  uint32_t channel;                            // index into the colorspace's channel table
  uint32_t in_out;                             // GUI only: 1 = show the output range too
  uint32_t disabled;                           // bit 0: input range off, bit 1: output range off
} dt_masks_point_parametric_t;

/** point struct for a DT_MASKS_RASTER form: a reference to another module's
    raster mask, by that module's operation and instance and the mask's id
    within it, so that the reference survives serialization */
typedef struct dt_masks_point_raster_t
{
  dt_dev_operation_t source;                   // op of the module writing the raster mask
  int instance;                                // multi_priority of that module instance
  dt_mask_id_t id;                             // which mask of the source module
} dt_masks_point_raster_t;


typedef struct dt_blendop_cl_global_t
{
  int kernel_blendop_mask_Lab;
  int kernel_blendop_mask_RAW;
  int kernel_blendop_mask_rgb_hsl;
  int kernel_blendop_mask_rgb_jzczhz;
  int kernel_blendop_Lab;
  int kernel_blendop_RAW;
  int kernel_blendop_RAW4;
  int kernel_blendop_rgb_hsl;
  int kernel_blendop_rgb_jzczhz;
  int kernel_blendop_mask_tone_curve;
  int kernel_blendop_set_mask;
  int kernel_blendop_display_channel;
  int kernel_calc_Y0_mask;
  int kernel_calc_scharr_mask;
  int kernel_calc_blend;
} dt_blendop_cl_global_t;


typedef struct dt_iop_gui_blendif_colorstop_t
{
  float stoppoint;
  GdkRGBA color;
} dt_iop_gui_blendif_colorstop_t;

typedef struct dt_iop_gui_blendif_channel_t
{
  char *label;
  char *tooltip;
  float increment;
  int numberstops;
  const dt_iop_gui_blendif_colorstop_t *colorstops;
  gboolean boost_factor_enabled;
  float boost_factor_offset;
  dt_develop_blendif_channels_t param_channels[2];
  dt_dev_pixelpipe_display_mask_t display_channel;
  void (*scale_print)(float value, float boost_factor, char *string, int n);
  int (*altdisplay)(GtkWidget *, dt_iop_module_t *, int);
  char *name;
} dt_iop_gui_blendif_channel_t;

// the channel table of a blend colorspace (blend_gui.c)
const dt_iop_gui_blendif_channel_t *dt_develop_blendif_channels_for_csp(const int csp);

// the localized type label of a parametric form: its channel's name, or a
// generic one when its colorspace has no channel table
const char *dt_masks_parametric_type_label(const dt_masks_form_t *const form);
/** does this parametric form, held by a member inverted or not, still cover
    its channel's whole span, i.e. restrict the mask not at all? */
gboolean dt_masks_parametric_is_noop(const dt_masks_form_t *const sel,
                                     const gboolean inverted);
/** a single-channel parametric form read from storage names a channel of its
    colorspace's table, which every reader indexes unchecked: one that does
    not is reset to the first channel. TRUE if it had to be */
gboolean dt_masks_parametric_sanitize(dt_masks_form_t *const form);

// the group renderers (masks/group.c), which object.c reuses: an AI object's
// points are group points, as a group's are
int dt_masks_group_get_mask(const dt_iop_module_t *const module,
                            const dt_dev_pixelpipe_iop_t *const piece,
                            struct dt_masks_form_t *const form,
                            float **buffer,
                            int *width,
                            int *height,
                            int *posx,
                            int *posy);
int dt_masks_group_get_mask_roi(const dt_iop_module_t *const module,
                                const dt_dev_pixelpipe_iop_t *const piece,
                                struct dt_masks_form_t *const form,
                                const dt_iop_roi_t *const roi,
                                float *const buffer);
void dt_masks_group_duplicate_points(struct dt_develop_t *const dev,
                                     struct dt_masks_form_t *const base,
                                     struct dt_masks_form_t *const dest);

// drop a path's cached shrink and grow results (masks/path.c), for object.c,
// which resizes and rotates an object's paths without going through path.c
void dt_masks_path_resize_invalidate(const dt_mask_id_t formid);

typedef struct dt_iop_gui_blendif_filter_t
{
  GtkDarktableGradientSlider *slider;
  // the slider's display scale: 0 linear, 1 the channel's alternative one
  // (log or zoom), named by altmode_name. Named in the slider's tooltip, as
  // the row has no text label to carry it
  int altmode;
  const char *altmode_name;
} dt_iop_gui_blendif_filter_t;

extern const dt_introspection_type_enum_tuple_t dt_develop_blend_colorspace_names[];
extern const dt_introspection_type_enum_tuple_t dt_develop_blend_mode_names[];
extern const dt_introspection_type_enum_tuple_t dt_develop_blend_mode_flag_names[];
extern const dt_introspection_type_enum_tuple_t dt_develop_mask_mode_names[];
extern const dt_introspection_type_enum_tuple_t dt_develop_combine_masks_names[];
extern const dt_introspection_type_enum_tuple_t dt_develop_feathering_guide_names[];
extern const dt_introspection_type_enum_tuple_t dt_develop_invert_mask_names[];

#ifdef HAVE_AI
#define DEVELOP_MASKS_NB_SHAPES 6
#else
#define DEVELOP_MASKS_NB_SHAPES 5
#endif

/** blend gui data */
typedef struct dt_iop_gui_blend_data_t
{
  gboolean blendif_support;
  gboolean blend_inited;
  gboolean masks_support;
  gboolean masks_inited;

  dt_develop_blend_colorspace_t csp;
  dt_iop_module_t *module;

  GtkWidget *iopw;
  GtkBox *blend_box;
  GtkBox *refine_box;
  // switches the mask on (DEVELOP_MASK_ENABLED | DEVELOP_MASK_FLEXI) or off,
  // in the "blend mask" header (see _blendop_mask_enable_toggled)
  GtkWidget *mask_enable_toggle;
  // the module name in the "blend mask" header
  GtkWidget *masks_blend_header_label;
  // the arrow at the start of the "blend mask" header that folds the panel
  // body away, shown only in the embedded position: the hosts fold themselves
  GtkWidget *flexi_inline_collapse_btn;
  // the "blend mask" header and everything below it: what moves into the
  // host the masks panel position names (dt_iop_gui_blend_masks_panel_relocate)
  GtkBox *relocatable_box;
  // everything in relocatable_box below the header, folded away in the
  // embedded position while the header stays as the way back. Only this
  // wrapper is hidden, so its children keep their own visibility
  GtkBox *masks_panel_body;
  GtkBox *masks_box;

  GtkWidget *showmask;
  // locks the mask against reset, presets, styles, paste and editing (see
  // dt_develop_blend_params_t's mask_lock). Left of showmask in the header
  GtkWidget *mask_lock_btn;
  GtkWidget *soloedit_mode;
  GtkWidget *masks_combine_combo;
  GtkWidget *blend_modes_combo;
  GtkWidget *blend_modes_blend_order;
  GtkWidget *blend_mode_parameter_slider;
  GtkWidget *opacity_slider;
  GtkWidget *blend_opacity_lowop_badge;
  GtkWidget *masks_feathering_guide_combo;
  GtkWidget *feathering_radius_slider;
  GtkWidget *blur_radius_slider;
  GtkWidget *contrast_slider;
  GtkWidget *brightness_slider;

  dt_develop_blend_colorspace_t blend_modes_csp;

  // the channel preview under the pointer: the parametric range slider or
  // "add channel" button it is over, the channel display bits it stands for,
  // and whether the preview is on
  GtkWidget *hovered_channel_widget;
  dt_dev_pixelpipe_display_mask_t hovered_channel_display;
  gboolean hover_preview_active;
  // the pointer has to rest on a channel button before the preview starts
  // (_preview_on_hover_enter). Remove it on every teardown path, or it fires
  // into freed blend_data
  guint preview_dwell_timer;
  dt_dev_pixelpipe_display_mask_t save_for_leave;
  GtkWidget *details_slider;

  // opens the menu that links or copies elements of other modules' masks, or
  // adds or uses another module's whole mask (_masks_import_btn_pressed)
  GtkWidget *masks_import_btn;
  GtkWidget *masks_shapes[DEVELOP_MASKS_NB_SHAPES];
  int masks_type[DEVELOP_MASKS_NB_SHAPES];
  GtkWidget *masks_edit;
  dt_masks_edit_mode_t masks_shown;
  // the edit mode folding the panel away turned off, for unfolding it to
  // restore (dt_iop_gui_blend_masks_panel_collapsed). DT_MASKS_EDIT_OFF:
  // nothing to restore
  dt_masks_edit_mode_t masks_shown_stash;

  // one button per channel of the module's blend colorspace, in masks_toolbar:
  // a click adds a single-channel parametric form for that channel, hovering
  // previews its mask. masks_param_channels_inner holds the buttons, rebuilt
  // only when the colorspace changes from param_channels_csp
  GtkWidget *masks_param_channels_box;
  GtkWidget *masks_param_channels_inner;
  int param_channels_csp;
  // the "add group" button and its wrapper in masks_toolbar. Its menu picks an
  // operator, and nests a new empty group with it in the target group,
  // selected so that the next shape drawn joins it (_stage_new_group)
  GtkWidget *masks_new_op;
  GtkWidget *masks_new_op_box;
  // the operator the "add group above selected group" shortcut uses: the one
  // last picked in the add-group menu, not one taken from the selection
  int masks_new_group_op;
  // the toolbar of every "add to the mask" action, above masks_list_box:
  // add group, the shapes, the parametric channels and import, plus the group
  // layout presets at the top right. It wraps onto two or three rows when the
  // panel is narrow. A height-for-width container (masks_gui_toolbar.c) lays
  // it out in GTK's own measure and allocate passes: do not move widgets
  // between rows by hand, which races GTK's layout. Every button is inserted
  // once (the channel buttons once the colorspace is known)
  GtkWidget *masks_toolbar;
  // the "blend mask" section header. Reading order:
  //
  //   expander | title | <space> | lock | show_mask_overlay | edit run | toggle
  //
  // (the expander moves to the far right when the canvas panel is docked on
  // the right, see _masks_header_apply_side)
  GtkWidget *masks_blend_header;
  // lock + show_mask_overlay + edit run + toggle, packed END
  GtkWidget *masks_right_cluster;
  // the edit run on the panel header: edit on canvas and solo edit between two
  // fixed gaps (see _pack_header_edit_run). Whole-mask canvas controls, so on
  // the header wherever the panel is hosted, never on the mask's own group
  GtkWidget *masks_header_edit_box;
  // the mask list: each group's header, its element rows indented under it
  // (_pack_group_elements), reorderable by drag and drop
  GtkBox *masks_list_box;
  // masks_toolbar and masks_list_box on one ground, so the add buttons read as
  // part of the list. Not the list box itself: dt_masks_gui_build_list wipes that
  GtkWidget *masks_list_area;
  // formid -> its row (the "mask-row" row_vbox), so that hover sync, selection
  // and row refreshes need no walk of the widget tree on every pointer motion.
  // Filled by _make_shape_row, emptied by dt_masks_gui_build_list; the widget
  // tree owns the rows
  GHashTable *masks_row_map;
  // what the last dt_masks_gui_build_list built from (dt_masks_gui_list_signature):
  // an unchanged signature would build the same list, so the rebuild is
  // skipped. DT_INVALID_HASH: never built
  dt_hash_t masks_list_sig;
  // the smoothing and cleanup sliders of the AI object being created, built
  // once by the pending row and kept while it is created, so that a value
  // change does not rebuild them mid-drag. NULL otherwise. Canvas scrolling
  // updates them (dt_iop_gui_blend_sync_pending_ai_sliders)
  GtkWidget *pending_ai_smoothing_slider;
  GtkWidget *pending_ai_cleanup_slider;
  float pending_ai_smoothing_last;
  float pending_ai_cleanup_last;
  // the area the module's parametric pickers last set a range from: with
  // "reuse the last picked area" on, arming any of them samples it again
  // (see _param_row_master_picker_pressed). param_pick_box_set is FALSE until
  // the first pick
  dt_pickerbox_t param_pick_box;
  gboolean param_pick_box_set;
  // the selected row's element; INVALID = none
  dt_mask_id_t panel_selected_formid;
  // the selected group, by its marker's id: where the next shape drawn lands
  // and what the refinement acts on. With no other group selected the mask's
  // own is (_select_mask_group_if_none), so it is never INVALID while the mask
  // has a group
  dt_mask_id_t panel_selected_group_cid;
  // the element under the cursor on the canvas, as its row stands for it
  // (dt_masks_model_panel_formid_for). The selection panel shows it in place
  // of the selection, which it leaves alone; INVALID = none
  dt_mask_id_t canvas_hovered_formid;
  // where dt_masks_group_insert_point puts the next element, while
  // insert_active: after the point insert_after_fid, the group's top member,
  // or its marker for an empty group
  gboolean insert_active;
  dt_mask_id_t insert_after_fid;
  // cluster key -> expanded (gboolean), for the clusters of adjacent shapes
  // of one kind within a group, a purely visual grouping, across list
  // rebuilds. Created on first use
  GHashTable *masks_cluster_expanded;
  // the soloed element, the others hidden; INVALID when none. Solo alone sets
  // DT_MASKS_STATE_HIDDEN, so ending it clears every hidden bit
  dt_mask_id_t solo_formid;
  // the soloed group's key; 0 when none (keys are >= 16)
  guint solo_group_key;
  // solo edit: only these outlines can be edited on the canvas, while the mask
  // still renders whole. An element, or a group's marker for all its members
  // (_soloedit_formids); INVALID when none
  dt_mask_id_t soloedit_formid;

  // the mask refinement sliders act on the selection
  // (_flexi_refine_follow_selection): the whole mask, a group or an element.
  // masks_refine_scope_kind/_formid name it; masks_refine_updating keeps a
  // slider update from committing back
  GtkWidget *masks_refine_reset_btn;  // resets the refinement of the active scope
  int masks_refine_scope_kind;
  dt_mask_id_t masks_refine_scope_formid;
  gboolean masks_refine_updating;
  // the next release on a row or group header only selects, never deselects:
  // the second press of a double-click that stepped into an AI object, or a
  // group drag ending in a release. Cleared on a plain press
  gboolean masks_skip_group_select_release;
  // set while _auto_expand_selected_row toggles row expanders itself: toggling
  // an expander selects its row, which would re-enter it without end. Not
  // DT_ENTER_GUI_UPDATE, which makes _props_row_toggled skip the update the
  // toggle still needs
  gboolean masks_suppress_toggle_select;
  // "auto-expand selected": the last selected element that has an expander
  // (dt_masks_model_row_is_expandable), apart from panel_selected_formid, so
  // that selecting one with nothing to expand leaves the open one open.
  // NO_MASKID: none yet
  dt_mask_id_t masks_last_expanded_elem;
  // the same for groups (_auto_expand_selected_group), which open their
  // members. Selecting an element selects its group too, so both open
  dt_mask_id_t masks_last_expanded_group;
  // a group the user just folded with its own chevron, for the next
  // _auto_expand_selected_group not to reopen: the same click selects it.
  // INVALID_MASKID when none
  dt_mask_id_t masks_group_collapse_click;
  // a press on a row turned into a drag (_row_drag_begin), which selects what
  // it settles on (_masks_drag_end). A release still reaching the row must
  // not run the click's select or toggle, which would deselect it again. On
  // macOS a drag source can start for a plain click
  gboolean masks_row_click_handled;
  // the AI object the canvas was stepped into when a row's last click began:
  // that click's own release can step out of it (deselecting the object
  // selects its group), and a double-click must still step out, not back in
  dt_mask_id_t masks_row_click_entered;
  // suppresses dt_masks_gui_build_list while set: dt_masks_form_remove()
  // rebuilds the list for every shape it removes, so a caller removing several
  // sets this around its loop and rebuilds once afterwards
  gboolean masks_rebuild_suppressed;
  // a deferred rebuild is queued (_queue_masks_list_rebuild), so that the
  // requests of one gesture make one rebuild
  gboolean masks_rebuild_pending;
  // its idle source, for dt_iop_gui_cleanup_blending to remove: run after the
  // module is gone, it would touch destroyed widgets. 0 when none
  guint masks_rebuild_idle_id;
  // the selection panel under the list: the selection's icon and name
  // (masks_selection_icon_box, masks_selection_name_label, see
  // _refine_update_header), then the properties and the refinement, which act
  // on it
  GtkWidget *masks_selection_area;
  GtkWidget *masks_selection_icon_box;
  GtkWidget *masks_selection_name_label;
  // the refinement section's title; its tooltip says why the section is
  // inactive, when it is (see _update_refine_sensitivity)
  GtkWidget *masks_refine_section_label;
  GtkWidget *masks_refine_expander;
  GtkWidget *masks_refine_bypass_btn;
  GtkWidget *masks_refine_toggle_btn;
  GtkBox *masks_refine_sliders_box;
  // "element properties in subpanel": the selected element's or group's
  // properties, or the creation controls of a shape being drawn, in a
  // collapsible of their own above the refinements (see _props_panel_sync).
  // props_panel_box is the wrapped section, props_panel_content the box the
  // editor goes in, props_panel_formid the element or group
  // (props_panel_is_group) it holds the editor of, and pending_props_box the
  // creation controls a pending row built for it
  GtkBox *props_panel_box;
  GtkWidget *props_panel_expander;
  GtkWidget *props_panel_content;
  GtkWidget *props_panel_toggle_btn;
  dt_mask_id_t props_panel_formid;
  gboolean props_panel_is_group;
  GtkWidget *pending_props_box;
  // "mask consumers": the modules downstream reading this module's raster
  // mask, one row each, in a collapsible below the refinements (see
  // _consumers_sync). consumers_sig hashes the rows shown, so an unchanged
  // list is not rebuilt
  GtkBox *consumers_box;
  GtkWidget *consumers_expander;
  GtkWidget *consumers_content;
  GtkWidget *consumers_toggle_btn;
  dt_hash_t consumers_sig;
  // the refinements the user previews switched off, never stored, keyed by
  // dt_masks_refine_key_*() below. Changed on the GTK thread under `lock`,
  // which commit_params takes to snapshot it (dt_masks_refine_bypass_commit);
  // the renderer reads only the snapshot
  GHashTable *masks_refine_bypassed;
  // row/group key -> expanded (see _remember_expanded): element rows by form
  // id, groups by their marker's id, nested group rows by their form id.
  // Created on first use
  GHashTable *masks_props_expanded;
  // group cid -> the page its preset note shows (see _make_group_note), so a
  // list rebuild keeps it. Created on first use
  GHashTable *masks_note_page;
  // group cid -> its preset note switched on (1) or off (2) with the info
  // icon by its name; wins over masks_notes_all_open and the selection (see
  // _group_note_is_open). Created on first use
  GHashTable *masks_note_open;
  // a preset was just applied: every note is open until another group is
  // selected
  gboolean masks_notes_all_open;
  // group cid -> its displayed number (see _group_ordinal_any)
  GHashTable *group_ordinals;

  dt_pthread_mutex_t lock;
} dt_iop_gui_blend_data_t;

// keys into the refinement bypass set and its snapshot: the whole mask, an
// element's refinement or a group's. A group key has the top bit set, which
// no mask id has
#define DT_MASKS_REFINE_KEY_GROUP_FLAG (0x80000000U)
#define DT_MASKS_REFINE_KEY_GLOBAL     (0U)

static inline guint32 dt_masks_refine_key_element(const dt_mask_id_t id)
{
  return (guint32)id;
}

static inline guint32 dt_masks_refine_key_group(const dt_mask_id_t cid)
{
  return (guint32)cid | DT_MASKS_REFINE_KEY_GROUP_FLAG;
}

/** copy the module's refinement bypass set into the piece, under bd->lock,
    from commit_params on a pipe worker. Without a GUI (export, CLI,
    thumbnails) the snapshot is empty: bypass is a preview only */
void dt_masks_refine_bypass_commit(const dt_iop_module_t *const module,
                                   dt_dev_pixelpipe_iop_t *const piece);

/** release a snapshot's key array */
void dt_masks_refine_bypass_cleanup(dt_dev_refine_bypass_t *const bypass);

/** is `key` (see dt_masks_refine_key_*) bypassed in this snapshot? */
gboolean dt_masks_refine_bypass_lookup(const dt_dev_refine_bypass_t *const bypass,
                                       const guint32 key);

/** hash of a snapshot, for mask cache invalidation */
dt_hash_t dt_masks_refine_bypass_hash(const dt_dev_refine_bypass_t *const bypass);


/** global init of blendops */
dt_blendop_cl_global_t *dt_develop_blend_init_cl_global(void);
/** global cleanup of blendops */
void dt_develop_blend_free_cl_global(dt_blendop_cl_global_t *b);

/** apply blend */
void dt_develop_blend_process(dt_iop_module_t *self,
                              dt_dev_pixelpipe_iop_t *piece,
                              const void *const i,
                              void *const o,
                              const dt_iop_roi_t *const roi_in,
                              const dt_iop_roi_t *const roi_out);

/** get blend version */
int dt_develop_blend_version(void);

/** returns the default blend color space for the given module */
dt_develop_blend_colorspace_t
dt_develop_blend_default_module_blend_colorspace(dt_iop_module_t *module);

/** initializes the default blend parameters for the given color space in blend_params */
void dt_develop_blend_init_blend_parameters(dt_develop_blend_params_t *blend_params,
                                            const dt_develop_blend_colorspace_t cst);

/** the mask of these blend params is locked */
static inline gboolean dt_develop_blend_mask_locked(const dt_develop_blend_params_t *const p)
{
  return p && p->mask_lock != 0;
}

/** copies the mask part of `locked` (everything but blend mode, blend
 *  parameter and opacity) over `incoming`, when `locked` is locked. Otherwise
 *  only clears the lock `incoming` brings along: the params being replaced
 *  decide whether the result is locked, never the incoming ones */
void dt_develop_blend_keep_locked_mask(dt_develop_blend_params_t *incoming,
                                       const dt_develop_blend_params_t *const locked);

/** initializes the default blendif parameters for the given color space in blend_params */
void dt_develop_blend_init_blendif_parameters(dt_develop_blend_params_t *blend_params,
                                              const dt_develop_blend_colorspace_t cst);

/** returns the color space for the given module */
dt_iop_colorspace_type_t
dt_develop_blend_colorspace(const dt_dev_pixelpipe_iop_t *const piece,
                            const dt_iop_colorspace_type_t cst);

/** update blendop params to current version */
gboolean dt_develop_blend_legacy_params(dt_iop_module_t *module,
                                        const void *const old_params,
                                        const int old_version,
                                        void *new_params,
                                        const int new_version,
                                        const int length);
/** dt_develop_blend_legacy_params(), plus the main.history `num` the row will
    be written back under, negative when there is none (a style item or a
    preset). With one, dt_masks_migrate_classic_to_flexi() writes the forms it
    creates into main.masks_history, which dt_masks_read_masks_history()
    reloads dev->forms from. dt_dev_read_history_ext() is the caller with one */
gboolean dt_develop_blend_legacy_params_ext(dt_iop_module_t *module,
                                            const void *const old_params,
                                            const int old_version,
                                            void *new_params,
                                            const int new_version,
                                            const int length,
                                            const int history_num);
gboolean dt_develop_blend_legacy_params_from_so(dt_iop_module_so_t *module_so,
                                                const void *const old_params,
                                                const int old_version,
                                                void *new_params,
                                                const int new_version,
                                                const int length);

/** color blending utility functions */

#define DEVELOP_BLENDIF_PARAMETER_ITEMS 6

/** initializes the parameter array (of size
 * DEVELOP_BLENDIF_PARAMETER_ITEMS * DEVELOP_BLENDIF_SIZE) */
void dt_develop_blendif_process_parameters(float *const parameters,
                                           const dt_develop_blend_params_t *const params);

/**
 * Set up a profile adapted to the blending.
 *
 * darktable built-in color profiles are chroma-adjusted such that
 * they define a [D65 RGB -> D50 XYZ] transform, which is expected by
 * CIE Lab and the ICC pipeline. Since JzAzBz expects an XYZ vector
 * adjusted for D65, we apply a Bradford transform on the profile
 * primaries to output D65 XYZ. The updated primaries are stored in
 * matrix_out. This is valid only in the context of blending with
 * JzAzBz color space. The resulting XYZ is used only to define masks
 * and not re-injected into the pipeline.
 *
 * The initialized profile may only be used to convert from RGB to XYZ.
 */
gboolean dt_develop_blendif_init_masking_profile(dt_dev_pixelpipe_iop_t *piece,
                                                 dt_iop_order_iccprofile_info_t *blending_profile,
                                                 const dt_develop_blend_colorspace_t cst);

/** refine one element's mask (details, feathering, blur, contrast and
 * brightness) in the group renderer, before it is combined. Does nothing when
 * the refinement is off. The feathering guide comes from the piece's
 * blend_refine_* context */
void dt_develop_blend_refine_form_mask(struct dt_iop_module_t *self,
                                       dt_dev_pixelpipe_iop_t *piece,
                                       float *const mask,
                                       const dt_iop_roi_t *const roi,
                                       const dt_masks_refinement_t *const r);

/** color blending mask generation functions.

    `d` is the blend configuration to evaluate. It is not always the piece's
    own: a parametric form (masks/parametric.c) evaluates its own blendif
    settings. The piece still provides the channel count and the mask display
    state. The module's own mask passes piece->blendop_data */

void dt_develop_blendif_raw_make_mask(dt_dev_pixelpipe_iop_t *piece,
                                      const dt_develop_blend_params_t *const d,
                                      const float *const a,
                                      const float *const b,
                                      const dt_iop_roi_t *const roi_in,
                                      const dt_iop_roi_t *const roi_out,
                                      float *const mask);

void dt_develop_blendif_lab_make_mask(dt_dev_pixelpipe_iop_t *piece,
                                      const dt_develop_blend_params_t *const d,
                                      const float *const a,
                                      const float *const b,
                                      const dt_iop_roi_t *const roi_in,
                                      const dt_iop_roi_t *const roi_out,
                                      float *const mask);

void dt_develop_blendif_rgb_hsl_make_mask(dt_dev_pixelpipe_iop_t *piece,
                                          const dt_develop_blend_params_t *const d,
                                          const float *const a,
                                          const float *const b,
                                          const dt_iop_roi_t *const roi_in,
                                          const dt_iop_roi_t *const roi_out,
                                          float *const mask);

void dt_develop_blendif_rgb_jzczhz_make_mask(dt_dev_pixelpipe_iop_t *piece,
                                             const dt_develop_blend_params_t *const d,
                                             const float *const a,
                                             const float *const b,
                                             const dt_iop_roi_t *const roi_in,
                                             const dt_iop_roi_t *const roi_out,
                                             float *const mask);

/** color blending operators */

void dt_develop_blendif_raw_blend(dt_dev_pixelpipe_iop_t *piece,
                                  const float *const a,
                                  float *const b,
                                  const dt_iop_roi_t *const roi_in,
                                  const dt_iop_roi_t *const roi_out,
                                  const float *const mask,
                                  const dt_dev_pixelpipe_display_mask_t request_mask_display);

void dt_develop_blendif_lab_blend(dt_dev_pixelpipe_iop_t *piece,
                                  const float *const a,
                                  float *const b,
                                  const dt_iop_roi_t *const roi_in,
                                  const dt_iop_roi_t *const roi_out,
                                  const float *const mask,
                                  const dt_dev_pixelpipe_display_mask_t request_mask_display);

void dt_develop_blendif_rgb_hsl_blend(dt_dev_pixelpipe_iop_t *piece,
                                      const float *const a,
                                      float *const b,
                                      const dt_iop_roi_t *const roi_in,
                                      const dt_iop_roi_t *const roi_out,
                                      const float *const mask,
                                      const dt_dev_pixelpipe_display_mask_t request_mask_display);

void dt_develop_blendif_rgb_jzczhz_blend(dt_dev_pixelpipe_iop_t *piece,
                                         const float *const a,
                                         float *const b,
                                         const dt_iop_roi_t *const roi_in,
                                         const dt_iop_roi_t *const roi_out,
                                         const float *const mask,
                                         const dt_dev_pixelpipe_display_mask_t request_mask_display);


/** May the blend render mask_id's group as this module's blend mask?

    FALSE only for an IOP_FLAGS_NO_MASKS module (retouch, spots) with a
    classic drawn mask: such a module uses its own forms in process(), and
    rendering mask_id's group would paint them. A flexi group is never those
    forms and always renders. Exported for the masks tests: getting this wrong
    silently drops the parametric mask of such modules */
gboolean dt_blend_may_render_group(struct dt_iop_module_t *self,
                                   const dt_develop_mask_mode_t mask_mode);

/** gui related stuff */
void dt_iop_gui_init_blending(GtkWidget *iopw, dt_iop_module_t *module);
void dt_iop_gui_update_blending(dt_iop_module_t *module);
void dt_iop_gui_update_masks(dt_iop_module_t *module);
// select the row of the shape selected on the canvas. Does nothing without a
// mask list
void dt_iop_gui_blend_masks_select_form(dt_iop_module_t *module, dt_mask_id_t formid);
// highlight the row of the shape hovered on the canvas, or its cluster's
// header while the cluster is folded, and show the shape in the selected
// element's panel in place of the selection. INVALID_MASKID clears both
void dt_iop_gui_blend_masks_hover_form(dt_iop_module_t *module, dt_mask_id_t formid);
// the canvas stepped into or out of an AI object: the panel lists the paths
// of the one stepped into
void dt_iop_gui_blend_masks_entered_object_changed(dt_iop_module_t *module);
// a click on empty canvas: nothing is selected in the panel any more
void dt_iop_gui_blend_masks_clear_selection(dt_iop_module_t *module);
void dt_iop_gui_cleanup_blending(dt_iop_module_t *module);
void dt_iop_gui_blending_lose_focus(dt_iop_module_t *module);
// the counterpart, when a module gains focus: moves the masks panel into its
// host, and carries the mask display and the canvas edit mode over from the
// module that lost focus
void dt_iop_gui_blending_gain_focus(dt_iop_module_t *module);
// move the masks panel out of its host and back into this module's expander.
// Call it before the module's widgets are destroyed: while hosted, the panel
// is parented in the host and would outlive its module. Does nothing if this
// module's panel is not hosted
void dt_iop_gui_blend_masks_panel_release(dt_iop_module_t *module);
// the forms were just replaced (undo, history jump, style paste, snapshot,
// history compression): drop the empty-group placeholders, which exist in the
// GUI only and would duplicate a group the reload brought back
void dt_iop_gui_blend_forms_reloaded(dt_iop_module_t *module);
// update the mask rows' warning badges in place. A badge can depend on another
// module (a raster element's source switched off or removed), so a signal
// handler calls this for every module; it returns at once without a list
void dt_iop_gui_blend_refresh_mask_badges(dt_iop_module_t *module);
// opens the blending options popover (blend colorspace, masks panel position,
// options) for the focused module, else the hosted one, else with the
// panel-wide sections only. A right-click on the toolbar's masks panel button
// opens it
void dt_iop_gui_blend_masks_options_popup(GtkButton *button, gpointer user_data);
// switch the module's mask on, flexi and empty if it has none yet; does
// nothing if it is on or the module does not blend. The folded panel's corner
// icon in gtk.c uses it, so that a click shows a live editor, not an inert one
void dt_iop_gui_blend_mask_enable(dt_iop_module_t *module);
// locks or unlocks the module's mask, as a history item. The module header's
// lock indicator unlocks through this
void dt_iop_gui_blend_set_mask_lock(dt_iop_module_t *module, const gboolean lock);
// the masks panel folded away (TRUE) or came back, wherever it is. Folding
// turns canvas editing and any armed shape tool off, keeping the edit mode in
// bd->masks_shown_stash for unfolding to restore: no editing overlay stays
// live without its panel. Called by all three ways to fold: the canvas
// panel (dt_ui_flexi_panel_set_collapsed in gtk.c), the utility module
// (masks_flexi_host.c) and the embedded header's arrow
void dt_iop_gui_blend_masks_panel_collapsed(const gboolean collapsed);
// the utility module hosting the panel was expanded or folded: records the
// user's change as the panel's fold preference, then follows it as above, as
// the other two positions do
void dt_iop_gui_blend_masks_panel_host_expanded(const gboolean expanded);
// moves the masks panel to its host and updates the header labels and tooltips
void dt_iop_gui_blend_masks_panel_relocate(dt_iop_module_t *module);
// show or hide the focused module's masks panel, wherever it is. The panel's
// arrow, the shortcut and the toolbar button all come here, so they cannot
// drift apart. Does nothing unless the focused module has a masks panel
void dt_iop_gui_blend_masks_panel_toggle(void);
// open the focused module's mask panel where it is hosted, if it is folded
void dt_iop_gui_blend_masks_panel_show(void);
// show the panel's state on the darkroom toolbar button: pressed while the
// panel shows, insensitive when the focused module has no masks panel
void dt_iop_gui_blend_masks_panel_sync_toolbox(void);
// show the smoothing and cleanup of the AI object being created in the
// pending-row sliders, after object.c changed them on a canvas scroll,
// without rebuilding the list mid-drag
void dt_iop_gui_blend_sync_pending_ai_sliders(dt_iop_module_t *module);
// the module's shapes, or the defaults of the shape being drawn, changed on
// the canvas: show the new values in the panel's controls without rebuilding
// it. Called from dt_dev_masks_list_change
void dt_iop_gui_blend_masks_changed(dt_iop_module_t *module);

// remove one element from this module's mask, exactly as the panel's delete
// does: only this module's use of it goes, and an emptied group stays in
// place. The canvas's delete gestures come here too (see
// dt_masks_remove_shape), so both behave the same
void dt_iop_gui_blend_delete_element(dt_iop_module_t *module, const dt_mask_id_t id);
/** the module's name changed: refresh the mask panels whose raster elements
    are named after it */
void dt_iop_gui_blend_module_renamed(dt_iop_module_t *module);

// shape creation ended for this module: queues the rebuild that drops the
// panel's pending row. Called from dt_masks_change_form_gui, which every way
// of canceling goes through, a click outside the canvas included
void dt_iop_gui_blend_masks_creation_ended(dt_iop_module_t *module);

gboolean blend_color_picker_apply(dt_iop_module_t *module,
                                  GtkWidget *picker,
                                  dt_dev_pixelpipe_t *pipe);

#ifdef HAVE_OPENCL
/** apply blend for opencl modules*/
gboolean dt_develop_blend_process_cl(dt_iop_module_t *self,
                                     dt_dev_pixelpipe_iop_t *piece,
                                     cl_mem dev_in,
                                     cl_mem dev_out,
                                     const dt_iop_roi_t *roi_in,
                                     const dt_iop_roi_t *roi_out);
#endif

#define _BLEND_FUNC_PROTO(align, uni) DT_OMP_DECLARE_SIMD(aligned align uniform uni) static void

G_END_DECLS

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
