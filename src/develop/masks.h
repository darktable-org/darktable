/*
    This file is part of darktable,
    Copyright (C) 2013-2026 darktable developers.

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

#include "common/darktable.h"
#include "common/opencl.h"
#include "develop/pixelpipe.h"
#include "dtgtk/button.h"
#include "dtgtk/gradientslider.h"
#include "gui/gtk.h"

#include <assert.h>

#define DEVELOP_MASKS_VERSION (7)

G_BEGIN_DECLS

/**forms types */
typedef enum dt_masks_type_t
{
  DT_MASKS_NONE = 0, // keep first
  DT_MASKS_CIRCLE = 1 << 0,
  DT_MASKS_PATH = 1 << 1,
  DT_MASKS_GROUP = 1 << 2,
  DT_MASKS_CLONE = 1 << 3,
  DT_MASKS_GRADIENT = 1 << 4,
  DT_MASKS_ELLIPSE = 1 << 5,
  DT_MASKS_BRUSH = 1 << 6,
  DT_MASKS_NON_CLONE = 1 << 7,
  // defined without HAVE_AI too, as code over every form type tests it. Only
  // object.c, which creates such forms, is built with AI alone
  DT_MASKS_OBJECT = 1 << 8,
  DT_MASKS_PARAMETRIC = 1 << 9, // a parametric (blendif) mask as an element of a group
  DT_MASKS_RASTER = 1 << 10,    // another module's raster mask as an element of a group
} dt_masks_type_t;

/**masts states */
typedef enum dt_masks_state_t
{
  DT_MASKS_STATE_NONE = 0,
  DT_MASKS_STATE_USE = 1 << 0,
  DT_MASKS_STATE_SHOW = 1 << 1,
  DT_MASKS_STATE_INVERSE = 1 << 2,
  DT_MASKS_STATE_UNION = 1 << 3,
  DT_MASKS_STATE_INTERSECTION = 1 << 4,
  DT_MASKS_STATE_DIFFERENCE = 1 << 5,
  DT_MASKS_STATE_EXCLUSION = 1 << 6,
  DT_MASKS_STATE_SUM = 1 << 7,
  // a hidden member is skipped by the group renderer: what solo sets on every
  // member it does not keep
  DT_MASKS_STATE_HIDDEN = 1 << 8,
  // flexi group operators: how a group folds its members, in list order. They
  // are set on the group's marker and are mutually exclusive; none set is
  // maximum, the default. See DT_MASKS_STATE_FLEXI_OP
  //   screen: the soft union a + b - ab, smoothing feathered overlaps
  DT_MASKS_STATE_FLEXI_SCREEN = 1 << 9,
  //   minimum: min
  DT_MASKS_STATE_FLEXI_MINIMUM = 1 << 12,
  // bypass (on a group's marker): the group contributes nothing, as if it were
  // not there. A modifier: the group keeps its operator
  DT_MASKS_STATE_OP_DISABLE = 1 << 14,
  DT_MASKS_STATE_OP_BYPASS = DT_MASKS_STATE_OP_DISABLE,
  //   product: the per-pixel product. Not minimum's min(): the two agree only
  //   for hard 0/1 membership
  DT_MASKS_STATE_FLEXI_PRODUCT = 1 << 15,
  // invert output (on a group's marker): flips the group's folded mask after
  // its refinement and before its opacity. Not DT_MASKS_STATE_INVERSE, which
  // flips one member's mask before it is folded in: for more than one member
  // the two differ
  DT_MASKS_STATE_OP_INVERT = 1 << 16,
  // disabled (element-level): the group fold skips this element
  DT_MASKS_STATE_DISABLE = 1 << 17,
  // a flexi group's own record in its group's point list: it refers to no
  // form (its formid is an id of its own, in the same id space as the forms)
  // and holds the group's settings once, followed by the group's members
  DT_MASKS_STATE_GROUP_MARKER = 1 << 18,
  //   sum: min(1, a + b), the clamp classic's sum applies after every shape.
  //   A clamp at 1 is absorbing for non-negative terms, so the fold order
  //   does not matter
  DT_MASKS_STATE_FLEXI_SUM = 1 << 19,
  //   difference: the first visible member is the base and every later one is
  //   subtracted from it in turn, as classic's difference does
  DT_MASKS_STATE_FLEXI_DIFFERENCE = 1 << 20,
  //   exclusion: classic's exclusion combiner, member by member in list
  //   order. It is not associative, so the order is part of it
  DT_MASKS_STATE_FLEXI_EXCLUSION = 1 << 21,
  // a classic member's operator: exactly one of these is set on every member
  // of a classic group but the bottom one
  DT_MASKS_STATE_OP_COMBINE = DT_MASKS_STATE_UNION
                            | DT_MASKS_STATE_INTERSECTION
                            | DT_MASKS_STATE_DIFFERENCE
                            | DT_MASKS_STATE_SUM
                            | DT_MASKS_STATE_EXCLUSION,
  DT_MASKS_STATE_OP = DT_MASKS_STATE_OP_COMBINE
                    | DT_MASKS_STATE_OP_DISABLE
                    | DT_MASKS_STATE_OP_INVERT,
  // a flexi group's operator
  DT_MASKS_STATE_FLEXI_OP = DT_MASKS_STATE_FLEXI_SCREEN
                        | DT_MASKS_STATE_FLEXI_MINIMUM
                        | DT_MASKS_STATE_FLEXI_PRODUCT
                        | DT_MASKS_STATE_FLEXI_SUM
                        | DT_MASKS_STATE_FLEXI_DIFFERENCE
                        | DT_MASKS_STATE_FLEXI_EXCLUSION
} dt_masks_state_t;

// one `state` word carries independent roles, a classic member's operator
// and a group's bypass/invert (DT_MASKS_STATE_OP), a flexi group's operator
// (DT_MASKS_STATE_FLEXI_OP) and the per-element flags. Their bits must never
// overlap: a collision switches on an unrelated feature, and bits stored in
// blobs and XMP cannot be reassigned later
G_STATIC_ASSERT((DT_MASKS_STATE_OP & DT_MASKS_STATE_FLEXI_OP) == 0);
G_STATIC_ASSERT((DT_MASKS_STATE_OP_COMBINE
                 & (DT_MASKS_STATE_OP_DISABLE | DT_MASKS_STATE_OP_INVERT)) == 0);
G_STATIC_ASSERT((DT_MASKS_STATE_GROUP_MARKER
                 & (DT_MASKS_STATE_OP | DT_MASKS_STATE_FLEXI_OP)) == 0);

// a classic member's effective operator. dt_masks_group_add_form() gives a
// group's first shape no combine bit, which there means union onto nothing
static inline dt_masks_state_t dt_masks_eff_group_op(const int state)
{
  // cast: masks.h is included from C++ too (common/exif.cc), where the masked
  // int does not convert back to the enum on its own
  const dt_masks_state_t op = (dt_masks_state_t)(state & DT_MASKS_STATE_OP);
  // only a combining bit counts: bypass and invert output modify an operator
  // and never stand in for one
  return (op & DT_MASKS_STATE_OP_COMBINE)
             ? op
             : (dt_masks_state_t)(op | DT_MASKS_STATE_UNION);
}

typedef enum dt_masks_property_t
{
  DT_MASKS_PROPERTY_OPACITY,
  DT_MASKS_PROPERTY_SIZE,
  DT_MASKS_PROPERTY_HARDNESS,
  DT_MASKS_PROPERTY_FEATHER,
  DT_MASKS_PROPERTY_ROTATION,
  DT_MASKS_PROPERTY_CURVATURE,
  DT_MASKS_PROPERTY_COMPRESSION,
  DT_MASKS_PROPERTY_CLEANUP,
  DT_MASKS_PROPERTY_SMOOTHING,
  DT_MASKS_PROPERTY_REFINE,
  DT_MASKS_PROPERTY_LAST
} dt_masks_property_t;

typedef enum dt_masks_points_states_t
{
  DT_MASKS_POINT_STATE_NORMAL = 1,
  DT_MASKS_POINT_STATE_USER = 2
} dt_masks_points_states_t;

typedef enum dt_masks_gradient_states_t
{
  DT_MASKS_GRADIENT_STATE_LINEAR = 1,
  DT_MASKS_GRADIENT_STATE_SIGMOIDAL = 2
} dt_masks_gradient_states_t;

typedef enum dt_masks_edit_mode_t
{
  DT_MASKS_EDIT_OFF = 0,
  DT_MASKS_EDIT_FULL = 1,
  DT_MASKS_EDIT_RESTRICTED = 2
} dt_masks_edit_mode_t;

typedef enum dt_masks_pressure_sensitivity_t
{
  DT_MASKS_PRESSURE_OFF = 0,
  DT_MASKS_PRESSURE_HARDNESS_REL = 1,
  DT_MASKS_PRESSURE_HARDNESS_ABS = 2,
  DT_MASKS_PRESSURE_OPACITY_REL = 3,
  DT_MASKS_PRESSURE_OPACITY_ABS = 4,
  DT_MASKS_PRESSURE_BRUSHSIZE_REL = 5
} dt_masks_pressure_sensitivity_t;

typedef enum dt_masks_ellipse_flags_t
{
  DT_MASKS_ELLIPSE_EQUIDISTANT = 0,
  DT_MASKS_ELLIPSE_PROPORTIONAL = 1
} dt_masks_ellipse_flags_t;

typedef enum dt_masks_source_pos_type_t
{
  DT_MASKS_SOURCE_POS_RELATIVE = 0,
  DT_MASKS_SOURCE_POS_RELATIVE_TEMP = 1,
  DT_MASKS_SOURCE_POS_ABSOLUTE = 2
} dt_masks_source_pos_type_t;

/* selected Bézier control point for path*/
typedef enum dt_masks_path_ctrl_t
{
  DT_MASKS_PATH_CRTL_NONE = 0,
  DT_MASKS_PATH_CTRL1 = 1,
  DT_MASKS_PATH_CTRL2 = 2

} dt_masks_path_ctrl_t;

/* restrictions on moving Bézier control points */
typedef enum dt_masks_path_edit_mode_t
{
  DT_MASKS_BEZIER_NONE = 0,        // preserve angle & scale
  DT_MASKS_BEZIER_SINGLE = 1,      // no restriction
  DT_MASKS_BEZIER_SYMMETRIC = 2,   // force full symmetry
  DT_MASKS_BEZIER_SING_SYMM = 3    // SINGLE && SYMMETRIC => force angle symmetry only
} dt_masks_path_edit_mode_t;

/** structure used to store 1 point for a circle */
typedef struct dt_masks_point_circle_t
{
  float center[2];
  float radius;
  float border;
} dt_masks_point_circle_t;

/** structure used to store 1 point for an ellipse */
typedef struct dt_masks_point_ellipse_t
{
  float center[2];
  float radius[2];
  float rotation;
  float border;
  dt_masks_ellipse_flags_t flags;
} dt_masks_point_ellipse_t;

#ifdef HAVE_AI
/** structure used to store 1 point for an object (AI segmentation) form */
typedef struct dt_masks_point_object_t
{
  float anchor[2]; // click position (normalized image coords)
  int label;       // 1 = foreground, 0 = background
} dt_masks_point_object_t;
#endif

/** structure used to store 1 point for a path form */
typedef struct dt_masks_point_path_t
{
  float corner[2];
  float ctrl1[2];
  float ctrl2[2];
  float border[2];
  dt_masks_points_states_t state;
} dt_masks_point_path_t;

/** structure used to store 1 point for a brush form */
typedef struct dt_masks_point_brush_t
{
  float corner[2];
  float ctrl1[2];
  float ctrl2[2];
  float border[2];
  float density;
  float hardness;
  dt_masks_points_states_t state;
} dt_masks_point_brush_t;

/** structure used to store anchor for a gradient */
typedef struct dt_masks_point_gradient_t
{
  float anchor[2];
  float rotation;
  float compression;
  float steepness;
  float curvature;
  dt_masks_gradient_states_t state;
} dt_masks_point_gradient_t;

/** which mask a refinement applies to */
typedef enum dt_masks_refine_scope_t
{
  DT_MASKS_REFINE_OFF = 0,     // no refinement
  DT_MASKS_REFINE_ELEMENT = 1, // a member's own mask, before it is folded in
  DT_MASKS_REFINE_GROUP = 2,   // on a group's marker: the group's folded mask
} dt_masks_refine_scope_t;

/** an optional mask refinement (since masks v7). The fields mirror the
    refinement controls of dt_develop_blend_params_t, which refine the
    module's finished mask */
typedef struct dt_masks_refinement_t
{
  int32_t enabled;           // dt_masks_refine_scope_t
  float details;             // detail-mask threshold, [-1..1]
  float feathering_radius;   // guided-filter radius, [0..]
  uint32_t feathering_guide; // dt_develop_mask_feathering_guide_t
  float blur_radius;         // gaussian blur radius, [0..]
  float contrast;            // mask contrast, [-1..1]
  float brightness;          // mask brightness, [-1..1]
} dt_masks_refinement_t;

/** structure used to store all forms's id for a group.
    The fields after opacity were added in masks v7. A point stored before
    stops where they start (see dt_masks_point_stride) and is read with them
    zero-filled, which is neutral for all but group_opacity */
typedef struct dt_masks_point_group_t
{
  dt_mask_id_t formid;
  dt_mask_id_t parentid;
  int state;
  float opacity;
  dt_masks_refinement_t refinement; // zero-filled = no refinement
  // a user-given group name, on a group's marker. Empty = none
  char name[128];
  // a group's own opacity, on its marker, multiplying its folded mask on top
  // of its members' own opacities. 1.0 is neutral, so the v6 to v7 step sets
  // it rather than leaving it zero-filled
  float group_opacity;
  // a group made from a built-in group layout preset: "<preset id>/<group id>",
  // the key of the notes the panel shows for it. On a group's marker; empty =
  // not from a preset. No pixel depends on it
  char preset_note[64];
} dt_masks_point_group_t;

// Is `pt` a group's marker rather than a member? A marker's formid resolves to
// no form, so code that looks a member's form up and skips what does not
// resolve is already safe. What has to ask is code that counts members, copies
// them, or keys something on a member's formid as if it were a form's
static inline gboolean dt_masks_point_is_marker(const dt_masks_point_group_t *pt)
{
  return (pt->state & DT_MASKS_STATE_GROUP_MARKER) != 0;
}

// how deep a walk follows groups nested in groups, and so how deep the panel
// lets groups nest. A deeper tree is malformed or cyclic, and a walk stops
// there instead of recursing until the stack is gone
#define DT_MASKS_NESTING_MAX 8

/** structure used to store pointers to the functions implementing operations on a mask shape */
/** plus a few per-class descriptive data items */
typedef struct dt_masks_functions_t
{
  int point_struct_size;   // sizeof(struct dt_masks_point_*_t)
  void (*sanitize_config)(dt_masks_type_t type_flags);
  GSList *(*setup_mouse_actions)(const struct dt_masks_form_t *const form);
  void (*set_form_name)(struct dt_masks_form_t *const form, const size_t nb);
  void (*set_hint_message)(const struct dt_masks_form_gui_t *const gui,
                           const struct dt_masks_form_t *const form,
                           const int opacity,
                           char *const __restrict__ msgbuf,
                           const size_t msgbuf_len);
  void (*modify_property)(struct dt_masks_form_t *const form,
                          dt_masks_property_t prop,
                          const float old_val,
                          const float new_val,
                          float *sum,
                          int *count,
                          float *min,
                          float *max);
  // grow/shrink (outset/inset) a shape to a signed absolute amount in the given
  // unit (use_percent: TRUE = % of shape size, FALSE = image pixels), measured
  // from a baseline captured the first time the shape is resized. Positive grows,
  // negative shrinks, 0 restores the baseline. Results are cached per offset, so
  // re-requesting a value is lossless. Returns TRUE if a usable shape resulted.
  // Currently only implemented by path masks.
  gboolean (*resize)(struct dt_masks_form_t *const form,
                     const int amount,
                     const gboolean use_percent);
  // report the resize offset currently applied to the shape, in the requested
  // unit, so a UI control can mirror it. Returns FALSE (amount 0) if no resize is
  // active. Currently only implemented by path masks.
  gboolean (*resize_get)(struct dt_masks_form_t *const form,
                         const gboolean use_percent,
                         float *amount);
  void (*duplicate_points)(dt_develop_t *const dev,
                           struct dt_masks_form_t *base,
                           struct dt_masks_form_t *dest);
  void (*initial_source_pos)(const float iwd,
                             const float iht,
                             float *x,
                             float *y);
  void (*get_distance)(const float x,
                       const float y,
                       const float as,
                       struct dt_masks_form_gui_t *gui,
                       const int index,
                       const int num_points,
                       gboolean *inside,
                       gboolean *inside_border,
                       int *near,
                       gboolean *inside_source,
                       float *dist);
  int (*get_points)(dt_develop_t *dev,
                    const float x,
                    const float y,
                    const float radius_a,
                    const float radius_b,
                    const float rotation,
                    float **points,
                    int *points_count);
  int (*get_points_border)(dt_develop_t *dev,
                           struct dt_masks_form_t *form,
                           float **points,
                           int *points_count,
                           float **border,
                           int *border_count,
                           const int source,
                           const dt_iop_module_t *const module);
  int (*get_mask)(const dt_iop_module_t *const module,
                  const dt_dev_pixelpipe_iop_t *const piece,
                  struct dt_masks_form_t *const form,
                  float **buffer,
                  int *width,
                  int *height,
                  int *posx,
                  int *posy);
  int (*get_mask_roi)(const dt_iop_module_t *const fmodule,
                      const dt_dev_pixelpipe_iop_t *const piece,
                      struct dt_masks_form_t *const form,
                      const dt_iop_roi_t *roi,
                      float *buffer);
  int (*get_area)(const dt_iop_module_t *const module,
                  const dt_dev_pixelpipe_iop_t *const piece,
                  struct dt_masks_form_t *const form,
                  int *width,
                  int *height,
                  int *posx,
                  int *posy);
  int (*get_source_area)(dt_iop_module_t *module,
                         dt_dev_pixelpipe_iop_t *piece,
                         struct dt_masks_form_t *form,
                         int *width,
                         int *height,
                         int *posx,
                         int *posy);
  int (*mouse_moved)(dt_iop_module_t *module,
                     float pzx,
                     float pzy,
                     const double pressure,
                     const int which,
                     const float zoom_scale,
                     struct dt_masks_form_t *form,
                     const dt_imgid_t parentid,
                     struct dt_masks_form_gui_t *gui,
                     const int index);
  int (*mouse_scrolled)(dt_iop_module_t *module,
                        float pzx,
                        float pzy,
                        const gboolean up,
                        uint32_t state,
                        struct dt_masks_form_t *form,
                        const dt_imgid_t parentid,
                        struct dt_masks_form_gui_t *gui,
                        const int index);
  int (*button_pressed)(dt_iop_module_t *module,
                        float pzx,
                        float pzy,
                        const double pressure,
                        const int which,
                        const int type,
                        const uint32_t state,
                        struct dt_masks_form_t *form,
                        const dt_imgid_t parentid,
                        struct dt_masks_form_gui_t *gui,
                        const int index);
  int (*button_released)(dt_iop_module_t *module,
                         float pzx,
                         float pzy,
                         const int which,
                         const uint32_t state,
                         struct dt_masks_form_t *form,
                         const dt_imgid_t parentid,
                         struct dt_masks_form_gui_t *gui,
                         const int index);
  void (*post_expose)(cairo_t *cr,
                      const float zoom_scale,
                      struct dt_masks_form_gui_t *gui,
                      const int index,
                      const int num_points);
} dt_masks_functions_t;

/** structure used to define a form */
typedef struct dt_masks_form_t
{
  GList *points; // list of point structures
  dt_masks_type_t type;
  const dt_masks_functions_t *functions;

  // position of the source (used only for clone). [0]=dx, [1]=dy, [2]=angle
  float source[3];
  // name of the form
  char name[128];
  // id used to store the form
  dt_mask_id_t formid;
  // version of the form
  int version;
} dt_masks_form_t;

typedef struct dt_masks_form_gui_points_t
{
  float *points;
  int points_count;
  float *border;
  int border_count;
  float *source;
  int source_count;
  gboolean clockwise;
} dt_masks_form_gui_points_t;

/** structure for dynamic buffers */
typedef struct dt_masks_dynbuf_t
{
  float *buffer;
  char tag[128];
  size_t pos;
  size_t size;
} dt_masks_dynbuf_t;

typedef struct dt_masks_intbuf_t
{
  int *buffer;
  char tag[128];
  size_t pos;
  size_t size;
} dt_masks_intbuf_t;


/** structure used to display a form */
typedef struct dt_masks_form_gui_t
{
  // points used to draw the form
  GList *points; // list of dt_masks_form_gui_points_t

  // points used to sample mouse moves
  dt_masks_dynbuf_t *guipoints, *guipoints_payload;
  int guipoints_count;

  // values for mouse positions, etc...
  float posx, posy, dx, dy, scrollx, scrolly, posx_source, posy_source;
  // TRUE if mouse has leaved the center window
  gboolean form_selected;
  gboolean border_selected;
  gboolean source_selected;
  gboolean source_rotating;
  gboolean counter_rotate_source;
  // joint rotation grabbed from the source shape: the mouse circles the source,
  // so its angular sweep must be measured about the source centroid (not the
  // destination centroid) to keep the rotation gain symmetric with grabbing the
  // target. The applied angle is identical either way; only the pivot used to
  // read the mouse motion differs.
  gboolean rotate_about_source;
  gboolean pivot_selected;
  gboolean select_only_border;
  dt_masks_edit_mode_t edit_mode;
  int point_selected;
  int point_edited;
  int feather_selected;
  dt_masks_path_ctrl_t bezier_ctrl; // For paths, this selects a Bézier control point.
  int seg_selected;
  int point_border_selected;
  int source_pos_type;

  gboolean form_dragging;
  gboolean source_dragging;
  gboolean form_rotating;
  gboolean border_toggling;
  gboolean gradient_toggling;
  int point_dragging;
  int feather_dragging;
  int seg_dragging;
  int point_border_dragging;

  dt_masks_path_edit_mode_t bezier_mode;  // Bézier editing with shift or ctrl
  float bezier_ctrl_angle;  // angle between ctrl1 and ctrl2
  float bezier_ctrl_scale;  // length of ctrl2 relative to ctrl1

  int group_edited;
  int group_selected;

  guint show_all_feathers;

  gboolean creation;
  gboolean creation_continuous;
  gboolean creation_closing_form;
  dt_iop_module_t *creation_module;
  dt_iop_module_t *creation_continuous_module;

  dt_masks_pressure_sensitivity_t pressure_sensitivity;

  // ids
  dt_mask_id_t formid;
  dt_hash_t pipe_hash;

  // masks panel and canvas feedback, set by blend_gui.c:
  // panel_hover_formids: shapes highlighted on the canvas because their row,
  //   or the header of a cluster holding them, is hovered. NULL = none
  // panel_selected_formid: the panel's selection, highlighted on the canvas
  //   while nothing is hovered
  // canvas_hover_formid: the shape under the cursor, whose row the panel
  //   highlights; kept to skip repeated updates
  // solo_formids: shapes soloed or solo-edited in the panel, highlighted
  //   whatever is hovered (_sync_solo_canvas_highlight in blend_gui.c).
  //   NULL = none
  GList *panel_hover_formids;
  dt_mask_id_t panel_selected_formid;
  dt_mask_id_t canvas_hover_formid;
  GList *solo_formids;
  // entered_object: the AI object the user stepped into on the canvas
  //   (double-click on it). Its paths then act one by one instead of as the
  //   single unit the object otherwise is. INVALID_MASKID = none. It survives
  //   dt_masks_clear_form_gui, which runs after every edit, and is reset by
  //   a click outside the object or when the module loses focus
  dt_mask_id_t entered_object;

  // opaque per-type data (e.g. segmentation context for object masks)
  void *scratchpad;
  void (*scratchpad_cleanup)(struct dt_masks_form_gui_t *gui);
} dt_masks_form_gui_t;

/** special value to indicate an invalid or uninitialized coordinate */
/** (replaces former use of NAN and isnan() by the most negative float) **/
#define DT_INVALID_COORDINATE (-FLT_MAX)

/** the shape-specific function tables */
extern const dt_masks_functions_t dt_masks_functions_circle;
extern const dt_masks_functions_t dt_masks_functions_ellipse;
extern const dt_masks_functions_t dt_masks_functions_brush;
extern const dt_masks_functions_t dt_masks_functions_path;
extern const dt_masks_functions_t dt_masks_functions_gradient;
extern const dt_masks_functions_t dt_masks_functions_group;
extern const dt_masks_functions_t dt_masks_functions_parametric;
extern const dt_masks_functions_t dt_masks_functions_raster;
/** the darkroom module a raster form reads its mask from, or NULL when it is
    gone. Resolved by operation and instance, as the form stores them */
struct dt_iop_module_t *dt_masks_raster_source(const dt_masks_form_t *form);
/** TRUE when this raster form cannot obtain a mask, so it renders all zero:
    its source module is gone, unnamed, switched off, or writes no raster mask.
    The group fold then skips its inversion, and the panel badges its row.
    FALSE for anything that is not a raster form.

    `piece` may be NULL outside a pipe (the panel). Inside one, pass it: in an
    export pipe only the source's piece knows whether it is on.

    A mask merely missing from the source's table this pass does not count:
    the next render brings it, and a badge would flicker */
gboolean dt_masks_raster_is_unresolved(const dt_iop_module_t *module,
                                       const dt_dev_pixelpipe_iop_t *piece,
                                       const dt_masks_form_t *form);
#ifdef HAVE_AI
extern const dt_masks_functions_t dt_masks_functions_object;
/** check if AI object mask model is downloaded and AI is enabled */
gboolean dt_masks_object_available(void);
/** apply a smoothing or cleanup change to the AI object being created, from
    the panel's pending-row sliders. Does nothing if none is being created */
void dt_masks_object_creation_apply_property(const dt_masks_property_t prop,
                                              const float old_val,
                                              const float new_val);
/** the smoothing, cleanup and edge refinement of the AI object being created,
    for the pending-row controls to show. Any output may be NULL. FALSE, with
    the outputs untouched, if none is being created */
gboolean dt_masks_object_creation_get_preview_params(float *smoothing,
                                                     int *cleanup,
                                                     gboolean *refine);
#endif

/** init dt_masks_form_gui_t struct with default values */
void dt_masks_init_form_gui(dt_masks_form_gui_t *gui);

/** get points in real space with respect of distortion dx and dy are
 * used to eventually move the center of the circle */
int dt_masks_get_points_border(dt_develop_t *dev,
                               dt_masks_form_t *form,
                               float **points,
                               int *points_count,
                               float **border,
                               int *border_count,
                               const int source,
                               const dt_iop_module_t *module);

/** get the rectangle which include the form and his border */
int dt_masks_get_area(const dt_iop_module_t *module,
                      const dt_dev_pixelpipe_iop_t *piece,
                      dt_masks_form_t *form,
                      int *width,
                      int *height,
                      int *posx,
                      int *posy);
int dt_masks_get_source_area(dt_iop_module_t *module,
                             dt_dev_pixelpipe_iop_t *piece,
                             dt_masks_form_t *form,
                             int *width,
                             int *height,
                             int *posx,
                             int *posy);
/** get the transparency mask of the form and his border */
static inline int dt_masks_get_mask(const dt_iop_module_t *const module,
                                    const dt_dev_pixelpipe_iop_t *const piece,
                                    dt_masks_form_t *const form,
                                    float **buffer,
                                    int *width,
                                    int *height,
                                    int *posx,
                                    int *posy)
{
  return (form->functions && form->functions->get_mask)
    ? form->functions->get_mask(module, piece, form, buffer, width, height, posx, posy)
    : 0;
}

static inline int dt_masks_get_mask_roi(const dt_iop_module_t *const module,
                                        const dt_dev_pixelpipe_iop_t *const piece,
                                        dt_masks_form_t *const form,
                                        const dt_iop_roi_t *roi,
                                        float *buffer)
{
  return (form->functions && form->functions->get_mask_roi)
    ? form->functions->get_mask_roi(module, piece, form, roi, buffer)
    : 0;
}

int dt_masks_group_render(dt_iop_module_t *module,
                          dt_dev_pixelpipe_iop_t *piece,
                          dt_masks_form_t *form,
                          float **buffer,
                          int *roi,
                          const float scale);
int dt_masks_group_render_roi(dt_iop_module_t *module,
                              dt_dev_pixelpipe_iop_t *piece,
                              dt_masks_form_t *form,
                              const dt_iop_roi_t *roi,
                              float *buffer);

// returns current masks version
int dt_masks_version(void);

// update masks from older versions
/** bytes one stored point of a `type` form takes in a blob written at masks
    `version`, for a current point struct of `point_size` bytes */
size_t dt_masks_point_stride(const dt_masks_type_t type,
                             const int version,
                             const size_t point_size);
int dt_masks_legacy_params(dt_develop_t *dev,
                           void *params,
                           const int old_version,
                           const int new_version);
/*
 * TODO:
 *
 * int
 * dt_masks_legacy_params(
 *   dt_develop_t *dev,
 *   const void *const old_params, const int old_version,
 *   void *new_params,             const int new_version);
 */

/** convert a module's classic mask_mode in place into a flexi mask (see
    migrate_legacy.c). Called from dt_develop_blend_legacy_params_ext().

    `history_num` is the main.history row `bp` will be written back under, or
    negative when there is none (style and preset conversion). With one, a
    conversion that creates forms is deferred to
    dt_masks_finish_flexi_migrations(), which knows the history position the
    forms must be written under: mask_mode turns flexi at once, mask_id then.

    It cannot fail: migration is one way. */
void dt_masks_migrate_classic_to_flexi(struct dt_iop_module_t *module,
                                       struct dt_develop_blend_params_t *bp,
                                       const int history_num);

/** create the forms of every migration dt_masks_migrate_classic_to_flexi()
    deferred, writing them under the masks_history position
    dt_masks_read_masks_history() reads as current. Call it once
    dev->history_end is read from the database and before
    dt_masks_read_masks_history() */
void dt_masks_finish_flexi_migrations(dt_develop_t *dev);

/** convert to flexi groups every classic group a migration kept
    (dev->pending_flexi_group_splits), in the live tree and every history
    snapshot, for the caller to write back. Call it after
    dt_masks_read_masks_history(), which replaces dev->forms */
void dt_masks_normalize_flexi_groups(dt_develop_t *dev);

/** we create a completely new form. */
dt_masks_form_t *dt_masks_create(dt_masks_type_t type);
/** we create a completely new form and add it to darktable.develop->allforms. */
dt_masks_form_t *dt_masks_create_ext(dt_masks_type_t type);
/** replace dev->forms with forms */
void dt_masks_replace_current_forms(dt_develop_t *dev, GList *forms);
/** returns a form with formid == id from a list of forms */
dt_masks_form_t *dt_masks_get_from_id_ext(GList *forms, dt_mask_id_t id);
/** returns a form with formid == id from dev->forms */
dt_masks_form_t *dt_masks_get_from_id(const dt_develop_t *dev, dt_mask_id_t id);
/** check if a form is used by a given module (directly or as a child of its group) */
gboolean dt_masks_is_in_module(dt_mask_id_t maskid, const struct dt_iop_module_t *module);
/** register forms into the mask manager, recording them in `module`'s history
    item (the mask manager's when NULL) */
void dt_masks_register_forms(dt_develop_t *dev,
                             struct dt_iop_module_t *module,
                             GList *forms);

/** read the forms from the db */
void dt_masks_read_masks_history(dt_develop_t *dev, const dt_imgid_t imgid);
/** write the forms into the db */
void dt_masks_write_masks_history_item(const dt_imgid_t imgid,
                                       const int num,
                                       const dt_masks_form_t *form);
void dt_masks_free_form(dt_masks_form_t *form);
void dt_masks_cleanup_unused(dt_develop_t *dev);
/** drop from every forms snapshot in history_list the forms nothing reads */
void dt_masks_cleanup_unused_from_list(GList *history_list);
/** drop every AI object none of whose paths is in `forms`, from `forms` and
    from every group in it. Returns how many were dropped */
int dt_masks_prune_empty_objects(GList **forms);

/** function used to manipulate forms for masks */
void dt_masks_change_form_gui(dt_masks_form_t *newform);
void dt_masks_clear_form_gui(const dt_develop_t *dev);
void dt_masks_reset_form_gui(void);
void dt_masks_reset_show_masks_icons(void);
gboolean dt_masks_cancel_creation(void);

gboolean dt_masks_events_mouse_moved(struct dt_iop_module_t *module,
                                     const float x,
                                     const float y,
                                     const double pressure,
                                     const int which,
                                     const float zoom_scale);
gboolean dt_masks_events_button_released(struct dt_iop_module_t *module,
                                         const float x,
                                         const float y,
                                         const int which,
                                         const uint32_t state,
                                         const float zoom_scale);
gboolean dt_masks_events_button_pressed(struct dt_iop_module_t *module,
                                        const float x,
                                        const float y,
                                        const double pressure,
                                        const int which,
                                        const int type,
                                        const uint32_t state);
gboolean dt_masks_events_mouse_scrolled(struct dt_iop_module_t *module,
                                        const float x,
                                        const float y,
                                        const gboolean up,
                                        const uint32_t state);
// Return TRUE if scrolling over the center view should adjust the visible
// mask (size/border/opacity) instead of zoom/pan. Returns FALSE while drawing
// a path, since path creation has no scroll-adjustable parameter.
gboolean dt_masks_scroll_over_mask(void);
void dt_masks_events_post_expose(const struct dt_iop_module_t *module,
                                 cairo_t *cr,
                                 const int32_t width,
                                 const int32_t height,
                                 const float pointerx,
                                 const float pointery,
                                 const float zoom_scale);
gboolean dt_masks_events_mouse_leave(struct dt_iop_module_t *module);
gboolean dt_masks_events_mouse_enter(struct dt_iop_module_t *module);

/** functions used to manipulate gui data */
void dt_masks_gui_form_create(dt_masks_form_t *form,
                              dt_masks_form_gui_t *gui,
                              const int index,
                              const struct dt_iop_module_t *module);
void dt_masks_gui_form_remove(dt_masks_form_t *form,
                              dt_masks_form_gui_t *gui,
                              const int index);
// Constrain a drag target (in preview/processed-pipe pixel coords, wd/ht =
// processed image size) so it stays within the image expanded by
// DT_MASKS_MOVE_MARGIN. Used when translating a whole form / its anchor / clone
// source so the dragged control point stays within the image or reasonably
// close, instead of being movable to an arbitrary distance where the shape
// would be lost.
void dt_masks_clamp_move_pts(float *pts, const float wd, const float ht);
void dt_masks_gui_form_test_create(dt_masks_form_t *form,
                                   dt_masks_form_gui_t *gui,
                                   const struct dt_iop_module_t *module);
void dt_masks_gui_form_save_creation(dt_develop_t *dev,
                                     struct dt_iop_module_t *module,
                                     dt_masks_form_t *form,
                                     dt_masks_form_gui_t *gui);
// add a registered form to the module's mask group where the panel's insert
// hint says the next element goes (_recompute_insert_hint in blend_gui.c).
// dt_masks_gui_form_save_creation() ends with it, and code that registers its
// own forms (object.c) calls it, so a new element lands in the same place
// whatever created it
void dt_masks_group_insert_member(dt_develop_t *dev,
                                  struct dt_iop_module_t *module,
                                  dt_masks_form_t *form,
                                  dt_masks_form_gui_t *gui);
/** a new flexi mask group for `module`, holding nothing but its marker,
    registered in dev->forms and made its mask. Records no history */
dt_masks_form_t *dt_masks_module_group_create(dt_develop_t *dev,
                                              struct dt_iop_module_t *module);
/** dt_masks_group_insert_member()'s placement, recording no history and
    touching no selection: for a caller adding several elements and committing
    once. Returns the new point */
dt_masks_point_group_t *dt_masks_group_insert_point(dt_develop_t *dev,
                                                    struct dt_iop_module_t *module,
                                                    dt_masks_form_t *form);
// give `form` the next free "<type label> #<n>" name, as
// dt_masks_gui_form_save_creation() names a new shape, for code that creates
// forms without the GUI (migrate_legacy.c)
void dt_masks_assign_unique_name(dt_develop_t *dev, dt_masks_form_t *form);
/** Solo: clear `bits` on the members named by `formids` and set them on every
 * other member of `grp`. A nested group holding a named point keeps its own
 * member clear and is isolated the same way, at any depth; one that is named
 * is cleared whole. Passing formids == NULL clears `bits` on every member at
 * every depth (i.e. "solo off"). A nested group another module's mask also
 * holds is isolated for that mask too, since the two share its points. */
void dt_masks_group_isolate_state(dt_masks_form_t *grp,
                                  GList *formids,
                                  const dt_masks_state_t bits);
void dt_masks_group_ungroup(dt_masks_form_t *dest_grp, dt_masks_form_t *grp);
void dt_masks_group_update_name(dt_iop_module_t *module);
/** a fresh id for a group marker, used by no form and no marker in `forms` */
dt_mask_id_t dt_masks_new_marker_id(GList *forms);
/** a new group marker for `grp`, folding with the flexi operator `flexi_op`
    (0 = maximum), not yet in any list */
dt_masks_point_group_t *dt_masks_marker_new(GList *forms,
                                            const dt_masks_form_t *grp,
                                            const dt_masks_state_t flexi_op);
/** append a copy of group marker `marker` to `dest` under a fresh id */
dt_masks_point_group_t *dt_masks_group_copy_marker(GList *forms,
                                                   dt_masks_form_t *dest,
                                                   const dt_masks_point_group_t *marker);
/** convert a classic group, and the classic groups nested in it, into flexi
    groups, each holding one marker and folding its members in order with one
    operator (dev-doc/masks_data_model.md). Where classic's operator
    changes along a list, what comes before becomes the first member of a
    new group; the last one keeps `grp`'s id. The members become plain
    elements, keeping their own opacity and inversion: the classic fold
    cannot read the result. A nested group converted here is replaced by its
    own members wherever that renders the same mask; a flexi-authored one is
    left as it is. A nested group the mask still holds twice gets a copy for
    each reference past the first, so a group has one parent within a mask.
    `roots` holds the id of every group a module renders as its mask, or is
    NULL: a nested group one of them names is shared, so the settings of the
    reference to it stay on that reference. TRUE if anything changed */
gboolean dt_masks_group_mark_classic_runs(GList **forms,
                                          dt_masks_form_t *grp,
                                          GHashTable *roots);
/** make the tree of flexi group `grp` shallower where that renders the same
    mask: empty nested groups go, a nested group folding with its holder's
    operator is replaced by its members, a group applying nothing to its one
    member by that member, and so on. A group with a name, or settings that
    change its result, stays, and one another module renders as its mask
    keeps its own settings. TRUE if anything changed */
gboolean dt_masks_group_simplify(GList *forms, dt_masks_form_t *grp);
/** the list node of point `id` of the mask `root`, a member or a marker, at
    any depth. `*owner`, when given, is set to the group form whose list holds
    it. NULL if none does */
GList *dt_masks_group_find_node(GList *forms,
                                dt_masks_form_t *root,
                                const dt_mask_id_t id,
                                dt_masks_form_t **owner);
/** the first raster element in the mask `grp`, nested groups included, that
    reads a mask of `source`: mask `id`, or any of its masks with `any_id`.
    NULL if there is none */
const struct dt_masks_point_raster_t *dt_masks_group_find_raster_of(GList *forms,
                                                                    const dt_masks_form_t *grp,
                                                                    const dt_iop_module_t *source,
                                                                    const dt_mask_id_t id,
                                                                    const gboolean any_id);
dt_masks_point_group_t *dt_masks_group_add_form(dt_masks_form_t *grp,
                                                const dt_masks_form_t *form);

dt_masks_edit_mode_t dt_masks_get_edit_mode(void);
void dt_masks_set_edit_mode(struct dt_iop_module_t *module,
                            const dt_masks_edit_mode_t value);
void dt_masks_set_edit_mode_single_form(struct dt_iop_module_t *module,
                                        const dt_mask_id_t formid,
                                        const dt_masks_edit_mode_t value);
// restrict canvas editing to `formids` (solo edit): only their outlines and
// handles can be edited, while the whole mask still renders
void dt_masks_set_edit_mode_forms(struct dt_iop_module_t *module,
                                  GList *formids,
                                  const dt_masks_edit_mode_t value);
void dt_masks_iop_update(struct dt_iop_module_t *module);
void dt_masks_iop_use_same_as(struct dt_iop_module_t *module,
                              struct dt_iop_module_t *src);
dt_hash_t dt_masks_group_hash(dt_hash_t hash, dt_masks_form_t *form);
// the same, resolving group members against an explicit form list instead of
// darktable.develop's. Use this wherever the caller knows which list is
// actually being rendered -- a pipe's own copy, say -- since a member that
// cannot be resolved contributes nothing to the hash at all.
dt_hash_t dt_masks_group_hash_ext(dt_hash_t hash,
                                  dt_masks_form_t *form,
                                  GList *forms);

void dt_masks_form_remove(struct dt_iop_module_t *module,
                          dt_masks_form_t *grp,
                          dt_masks_form_t *form);
/** delete a shape, from the canvas or the panel alike: for an element of the
    module's flexi mask, the same as the panel's delete (see
    dt_iop_gui_blend_delete_element), otherwise removal from `parentid`.
    `whole` is a gesture on the shape as a whole, which for a path of an AI
    object takes the object with it unless the user stepped into the object
    (dt_masks_form_gui_t.entered_object); otherwise just the path goes */
void dt_masks_remove_shape(struct dt_iop_module_t *module,
                           dt_masks_form_t *form,
                           dt_mask_id_t parentid,
                           const gboolean whole);
/** what dt_masks_remove_shape takes away, with `form` and `parentid` moved to
    that target: a path out of its AI object, an element of the module's flexi
    mask (the panel's delete), or anything else out of its parent */
typedef enum dt_masks_remove_target_t
{
  DT_MASKS_REMOVE_PATH = 0,
  DT_MASKS_REMOVE_ELEMENT = 1,
  DT_MASKS_REMOVE_FROM_PARENT = 2
} dt_masks_remove_target_t;
dt_masks_remove_target_t dt_masks_remove_shape_target(const struct dt_iop_module_t *module,
                                                      dt_masks_form_t **form,
                                                      dt_mask_id_t *parentid,
                                                      const gboolean whole);
/** a canvas press in the flat edit group: a double-click on a path of an AI
    object steps into the object, any other primary click outside it steps out.
    `hit_object` is the object of the path under the pointer, if any. A step
    either way redraws the canvas and updates `module`'s mask panel. TRUE when
    the press was the step in and must do nothing else */
gboolean dt_masks_gui_step_object(dt_iop_module_t *module,
                                  dt_masks_form_gui_t *gui,
                                  const dt_mask_id_t hit_object,
                                  const gboolean primary,
                                  const gboolean double_click);
/** the AI object a flat edit group's point moves with as one unit, NULL when
    it is no object's path or the user stepped into its object */
dt_masks_form_t *dt_masks_bundle_of(const dt_masks_point_group_t *fpt);
/** the center of the AI object `object`, the mean of all its paths' corner
    points: close enough for the nested outline and holes a segmentation
    produces. FALSE, with `cx` and `cy` untouched, for an object with no point */
gboolean dt_masks_object_center(const dt_masks_form_t *object, double *cx, double *cy);
/** add every element of `src_grp` to `grp`, keeping its operator and opacity:
    shapes are shared, parametric channels copied. No history item */
void dt_masks_group_add_members_of(dt_masks_form_t *grp, const dt_masks_form_t *src_grp);
/** change the opacity of `form` in its parent `parentid`, a group or an AI
    object, by `amount`, clamped to [0, 1]. Returns the opacity it now has, or
    0 when it has none there to change: it is a group, or no member */
float dt_masks_form_change_opacity(struct dt_iop_module_t *module,
                                   dt_masks_form_t *form,
                                   const dt_mask_id_t parentid,
                                   const float amount);
void dt_masks_form_move(dt_masks_form_t *grp,
                        const dt_mask_id_t formid,
                        const gboolean up);
int dt_masks_form_duplicate(dt_develop_t *dev,
                            const dt_mask_id_t formid);
/** an independent copy of a form under a new id, keeping its name, registered
    in dev->forms; members included for an AI object. Records no history */
dt_mask_id_t dt_masks_form_copy(dt_develop_t *dev, const dt_mask_id_t formid);
/* returns a duplicate tof form, including the formid */
dt_masks_form_t *dt_masks_dup_masks_form(const dt_masks_form_t *form);
/* duplicate the list of forms, replace item in the list with form with the same formid */
GList *dt_masks_dup_forms_deep(GList *forms, dt_masks_form_t *form);

/** utils functions */
gboolean dt_masks_point_in_form_exact(const float x,
                                      const float y,
                                      const float *points,
                                      const int points_start,
                                      const int points_count);
gboolean dt_masks_point_in_form_near(const float x,
                                     const float y,
                                     const float *points,
                                     const int points_start,
                                     const int points_count,
                                     const float distance,
                                     int *near);
float dt_masks_drag_factor(dt_masks_form_gui_t *gui,
                           const int index,
                           const int k,
                           const gboolean border);

float dt_masks_change_size(const gboolean up,
                           const float value,
                           const float min,
                           const float max);

float dt_masks_change_rotation(const gboolean up,
                               const float value,
                               const gboolean is_degree);

/** allow to select a shape inside an iop */
void dt_masks_select_form(struct dt_iop_module_t *module,
                          const dt_masks_form_t *sel);

/** utils for selecting the source of a clone mask while creating it */
void dt_masks_draw_clone_source_pos(cairo_t *cr,
                                    const float zoom_scale,
                                    const float x,
                                    const float y);
void dt_masks_set_source_pos_initial_state(dt_masks_form_gui_t *gui,
                                           const uint32_t state,
                                           const float pzx,
                                           const float pzy);
void dt_masks_set_source_pos_initial_value(dt_masks_form_gui_t *gui,
                                           const int mask_type,
                                           dt_masks_form_t *form,
                                           const float pzx,
                                           const float pzy);
void dt_masks_calculate_source_pos_value(const dt_masks_form_gui_t *gui,
                                         const int mask_type,
                                         const float initial_xpos,
                                         const float initial_ypos,
                                         const float xpos,
                                         const float ypos,
                                         float *px,
                                         float *py,
                                         const int adding);

/** detail mask support */
float *dt_masks_calc_scharr_mask(struct dt_dev_pixelpipe_t *pipe,
                                 float *src,
                                 const int width,
                                 const int height,
                                 const gboolean rawmode);
float *dt_masks_calc_detail_mask(struct dt_dev_pixelpipe_iop_t *piece,
                                 const float threshold,
                                 const gboolean detail);
void dt_masks_calc_detail_blend(float *const src,
                                float *out,
                                const size_t msize,
                                const float threshold,
                                const gboolean detail);


/** return the list of possible mouse actions */
GSList *dt_masks_mouse_actions(const dt_masks_form_t *form);

void dt_group_events_post_expose(cairo_t *cr,
                                 const float zoom_scale,
                                 dt_masks_form_t *form,
                                 dt_masks_form_gui_t *gui);


/******************************************************
 * code for dynamic handling of intermediate buffers
 * buffer for floats
 */
static inline gboolean _dt_masks_dynbuf_growto(dt_masks_dynbuf_t *a,
                                               const size_t newsize)
{
  float *newbuf = dt_alloc_align_float(newsize);
  if (!newbuf)
  {
    // not much we can do here except emit an error message
    dt_print(DT_DEBUG_ALWAYS,
             "critical: out of memory for dynbuf '%s' with size request %zu!",
             a->tag, newsize);
    return FALSE;
  }
  if (a->buffer)
  {
    memcpy(newbuf, a->buffer, a->size * sizeof(float));
    dt_print(DT_DEBUG_MASKS, "[masks dynbuf '%s'] grows to size %lu (is %p, was %p)",
             a->tag,
             (unsigned long)a->size, newbuf, a->buffer);
    dt_free_align(a->buffer);
  }
  a->size = newsize;
  a->buffer = newbuf;
  return TRUE;
}

static inline
dt_masks_dynbuf_t *dt_masks_dynbuf_init(const size_t size, const char *tag)
{
  assert(size > 0);
  dt_masks_dynbuf_t *a = (dt_masks_dynbuf_t *)calloc(1, sizeof(dt_masks_dynbuf_t));

  if(a != NULL)
  {
    g_strlcpy(a->tag, tag, sizeof(a->tag)); //only for debugging purposes
    a->pos = 0;
    if(_dt_masks_dynbuf_growto(a, size))
      dt_print(DT_DEBUG_MASKS, "[masks dynbuf '%s'] with initial size %lu (is %p)",
               a->tag,
               (unsigned long)a->size, a->buffer);
    if(a->buffer == NULL)
    {
      free(a);
      a = NULL;
    }
  }
  return a;
}

static inline
void dt_masks_dynbuf_add(dt_masks_dynbuf_t *a, const float value)
{
  assert(a != NULL);
  assert(a->pos <= a->size);
  if(__builtin_expect(a->pos == a->size, 0))
  {
    if (a->size == 0 || !_dt_masks_dynbuf_growto(a, 2 * a->size))
      return;
  }
  a->buffer[a->pos++] = value;
}

static inline
void dt_masks_dynbuf_add_2(dt_masks_dynbuf_t *a, const float value1, const float value2)
{
  assert(a != NULL);
  assert(a->pos <= a->size);
  if(__builtin_expect(a->pos + 2 >= a->size, 0))
  {
    if (a->size == 0 || !_dt_masks_dynbuf_growto(a, 2 * (a->size+1)))
      return;
  }
  a->buffer[a->pos++] = value1;
  a->buffer[a->pos++] = value2;
}

// Return a pointer to N floats past the current end of the dynbuf's
// contents, marking them as already in use.  The caller should then
// fill in the reserved elements using the returned pointer.
static inline
float *dt_masks_dynbuf_reserve_n(dt_masks_dynbuf_t *a, const int n)
{
  assert(a != NULL);
  assert(a->pos <= a->size);
  if(__builtin_expect(a->pos + n >= a->size, 0))
  {
    if(a->size == 0) return NULL;
    size_t newsize = a->size;
    while(a->pos + n >= newsize) newsize *= 2;
    if (!_dt_masks_dynbuf_growto(a, newsize))
    {
      return NULL;
    }
  }
  // get the current end of the (possibly reallocated) buffer, then
  // mark the next N items as in-use
  float *reserved = a->buffer + a->pos;
  a->pos += n;
  return reserved;
}

static inline
void dt_masks_dynbuf_add_zeros(dt_masks_dynbuf_t *a, const int n)
{
  assert(a != NULL);
  assert(a->pos <= a->size);
  if(__builtin_expect(a->pos + n >= a->size, 0))
  {
    if(a->size == 0) return;
    size_t newsize = a->size;
    while(a->pos + n >= newsize) newsize *= 2;
    if (!_dt_masks_dynbuf_growto(a, newsize))
    {
      return;
    }
  }
  // now that we've ensured a sufficiently large buffer add N zeros to
  // the end of the existing data
  memset(a->buffer + a->pos, 0, n * sizeof(float));
  a->pos += n;
}


static inline
float dt_masks_dynbuf_get(dt_masks_dynbuf_t *a, const int offset)
{
  assert(a != NULL);
  // offset: must be negative distance relative to end of buffer
  assert(offset < 0);
  assert((long)a->pos + offset >= 0);
  return (a->buffer[a->pos + offset]);
}

static inline
float dt_masks_dynbuf_get_absolute(dt_masks_dynbuf_t *a, const int position)
{
  assert(a != NULL);
  assert(position >= 0);
  assert((long)a->pos > position);
  return (a->buffer[position]);
}

static inline
void dt_masks_dynbuf_set(dt_masks_dynbuf_t *a, const int offset, const float value)
{
  assert(a != NULL);
  // offset: must be negative distance relative to end of buffer
  assert(offset < 0);
  assert((long)a->pos + offset >= 0);
  a->buffer[a->pos + offset] = value;
}

static inline
void dt_masks_dynbuf_set_absolute(dt_masks_dynbuf_t *a, const int position, const float value)
{
  assert(a != NULL);
  assert(position >= 0);
  assert((long)a->pos > position);
  a->buffer[position] = value;
}

static inline
float *dt_masks_dynbuf_buffer(dt_masks_dynbuf_t *a)
{
  assert(a != NULL);
  return a->buffer;
}

static inline
size_t dt_masks_dynbuf_position(dt_masks_dynbuf_t *a)
{
  assert(a != NULL);
  return a->pos;
}

static inline
void dt_masks_dynbuf_reset_position(dt_masks_dynbuf_t *a, const size_t newpos)
{
  assert(a != NULL);
  assert(newpos <= a->pos);
  a->pos = newpos;
}

static inline
void dt_masks_dynbuf_reset(dt_masks_dynbuf_t *a)
{
  assert(a != NULL);
  a->pos = 0;
}

static inline
float *dt_masks_dynbuf_harvest(dt_masks_dynbuf_t *a)
{
  // take out data buffer and make dynamic buffer obsolete
  if(a == NULL) return NULL;
  float *r = a->buffer;
  a->buffer = NULL;
  a->pos = a->size = 0;
  return r;
}

static inline
void dt_masks_dynbuf_free(dt_masks_dynbuf_t *a)
{
  if(a == NULL) return;
  dt_print(DT_DEBUG_MASKS, "[masks dynbuf '%s'] freed (was %p)", a->tag,
          a->buffer);
  dt_free_align(a->buffer);
  free(a);
}

// Dump buffer to file for debugging.
static inline
void dt_masks_dynbuf_debug_print(dt_masks_dynbuf_t *a, gboolean to_stdout)
{
  if(a == NULL) return;
  if (to_stdout)
  {
    printf("'%s' buffer: ", a->tag);
    for (size_t i = 0; i < a->pos; i += 2)
    {
      printf("(%f %f), ", a->buffer[i], a->buffer[i+1]);
    }
    printf("\n");
  }
  else
  {
    FILE *f;
    char filename[255] = { 0 };
    sprintf(filename, "debug-%" PRIdMAX "-%s", (intmax_t)time(NULL), a->tag);
    f = g_fopen(filename, "w");
    for (size_t i = 0; i < a->pos; i += 2)
    {
      fprintf(f, "%f %f\n", a->buffer[i], a->buffer[i+1]);
    }
    fclose(f);
  }
}

/******************************************************
 * code for dynamic handling of intermediate buffers
 * buffer for ints
 */
static inline gboolean _dt_masks_intbuf_growto(dt_masks_intbuf_t *a,
                                               const size_t newsize)
{
  int *newbuf = dt_alloc_align_int(newsize);
  if (!newbuf)
  {
    // not much we can do here except emit an error message
    dt_print(DT_DEBUG_ALWAYS,
             "critical: out of memory for intbuf '%s' with size request %zu!",
             a->tag, newsize);
    return FALSE;
  }
  if (a->buffer)
  {
    memcpy(newbuf, a->buffer, a->size * sizeof(int));
    dt_print(DT_DEBUG_MASKS, "[masks intbuf '%s'] grows to size %lu (is %p, was %p)",
             a->tag,
             (unsigned long)a->size, newbuf, a->buffer);
    dt_free_align(a->buffer);
  }
  a->size = newsize;
  a->buffer = newbuf;
  return TRUE;
}


static inline
dt_masks_intbuf_t *dt_masks_intbuf_init(const size_t size, const char *tag)
{
  assert(size > 0);
  dt_masks_intbuf_t *a = (dt_masks_intbuf_t *)calloc(1, sizeof(dt_masks_intbuf_t));

  if(a != NULL)
  {
    g_strlcpy(a->tag, tag, sizeof(a->tag)); //only for debugging purposes
    a->pos = 0;
    if(_dt_masks_intbuf_growto(a, size))
      dt_print(DT_DEBUG_MASKS, "[masks intbuf '%s'] with initial size %lu (is %p)",
               a->tag,
               (unsigned long)a->size, a->buffer);
    if(a->buffer == NULL)
    {
      free(a);
      a = NULL;
    }
  }
  return a;
}


static inline
void dt_masks_intbuf_add_2(dt_masks_intbuf_t *a, const int value1, const int value2)
{
  assert(a != NULL);
  assert(a->pos <= a->size);
  if(__builtin_expect(a->pos + 2 >= a->size, 0))
  {
    if (a->size == 0 || !_dt_masks_intbuf_growto(a, 2 * (a->size+1)))
      return;
  }
  a->buffer[a->pos++] = value1;
  a->buffer[a->pos++] = value2;
}

static inline
size_t dt_masks_intbuf_position(dt_masks_intbuf_t *a)
{
  assert(a != NULL);
  return a->pos;
}

static inline
void dt_masks_intbuf_free(dt_masks_intbuf_t *a)
{
  if(a == NULL) return;
  dt_print(DT_DEBUG_MASKS, "[masks intbuf '%s'] freed (was %p)", a->tag,
          a->buffer);
  dt_free_align(a->buffer);
  free(a);
}

// Dump buffer to file for debugging.
/*
static inline
void dt_masks_intnbuf_debug_print(dt_masks_intbuf_t *a)
{
  if(a == NULL) return;
  FILE *f;
  char filename[255] = { 0 };
  sprintf(filename, "debug-%ld-%s", time(NULL), a->tag);
  f = g_fopen(filename, "w");
  for (size_t i = 0; i < a->pos; i += 2)
  {
    fprintf(f, "%d %d\n", a->buffer[i], a->buffer[i+1]);
  }
  fclose(f);
}
*/

/* End of dynamic buffer code
 ******************************************************/

static inline
int dt_masks_roundup(const int num, const int mult)
{
  const int rem = num % mult;

  return (rem == 0) ? num : num + mult - rem;
}

#define DT_MASKS_CONF(type, shape, param) \
  (type & (DT_MASKS_CLONE | DT_MASKS_NON_CLONE) \
   ? "plugins/darkroom/spots/" #shape "_" #param \
   : "plugins/darkroom/masks/" #shape "/" #param)

void dt_masks_draw_anchor(cairo_t *cr,
                          const gboolean selected,
                          const float zoom_scale,
                          const float x,
                          const float y);

/* draw the small control point for selected anchor in path & brush */
void dt_masks_draw_ctrl(cairo_t *cr,
                        const float x,
                        const float y,
                        const float zoom_scale,
                        const gboolean selected);

/* find the closest to point (px, py) in points array.
   nb_ctrl is the number of points (control points) to
   skip at the start of points.
*/
void dt_masks_closest_point(const int count,
                            const int nb_ctrl,
                            const float *points,
                            const float px,
                            const float py,
                            float *x,
                            float *y);

/* Rotate the control points of a path/brush outline in screen space and project
   them back to normalized image coordinates. `gpt_points` is the gui display
   buffer (interleaved x,y) whose first `nb*3` pairs are the control points,
   stored per node as ctrl1, corner, ctrl2; `points_count` is its number of
   (x,y) pairs. Each control point is rotated by (cos_a, sin_a) around the screen
   pivot (cx, cy), back-transformed through the pipe in a single batch, and
   written to `out` (normalized, same interleaving, nb*6 floats). Shared by the
   path and brush rotate gestures. */
void dt_masks_rotate_ctrl_points(dt_develop_t *dev,
                                 const float *const gpt_points,
                                 const int points_count,
                                 const int nb,
                                 const float cx,
                                 const float cy,
                                 const float cos_a,
                                 const float sin_a,
                                 const float iwidth,
                                 const float iheight,
                                 float *const out);

/* draw a line from -> to with an arrow at the end.
   if touch_dest is true then the arrow will be at the
   (to_x, to_y) location, otherwise a small space will
   be left.
*/
void dt_masks_draw_arrow(cairo_t *cr,
                         const float from_x,
                         const float from_y,
                         const float to_x,
                         const float to_y,
                         const float zoom_scale,
                         const gboolean touch_dest);

/* stroke the arrow on cr depending on selection */
void dt_masks_stroke_arrow(cairo_t *cr,
                           const dt_masks_form_gui_t *gui,
                           const int group,
                           const float zoom_scale);

/* set line width for the mask drawing depending on the status
   border, source & selected
*/
void dt_masks_line_stroke(cairo_t *cr,
                          const gboolean border,
                          const gboolean source,
                          const gboolean selected,
                          const float zoom_scale);

void dt_masks_stroke_polyline(cairo_t *cr,
                              const float *const pts,
                              const int from,
                              const int to,
                              const gboolean close_path,
                              const gboolean border,
                              const gboolean source,
                              const gboolean selected,
                              const float zoom_scale);

static inline float dt_masks_sensitive_dist(const float zoom_scale)
{
  return DT_PIXEL_APPLY_DPI(7) / zoom_scale;
}

static inline void dt_masks_get_image_size(float *width,
                                           float *height,
                                           float *iwidth,
                                           float *iheight)
{
  // iwidth/iheight must match preview->iwidth/iheight (= pipe->iwidth/iheight used
  // by _path_get_pts_border to scale corner coordinates before distort_transform).
  // width/height must match preview->processed_width/height, which is what both
  // dt_dev_get_preview_size() and dt_view_paint_surface FALLBACK use as canvas size.
  const dt_develop_t *dev = darktable.develop;
  const dt_dev_pixelpipe_t *preview = dev->preview_pipe;
  const float iscale = preview->iscale > 0.f ? preview->iscale : 1.f;

  // Use preview pipe's actual processed dimensions, not full.pipe/iscale.
  // The two differ by up to 1 pixel due to independent integer truncations
  // in each pipeline (e.g. after crop), causing a systematic mask overlay shift.
  // dt_dev_get_preview_size() uses the same value, so both are consistent.
  if(preview->processed_width > 0)
  {
    if(width  ) *width   = preview->processed_width;
    if(height ) *height  = preview->processed_height;
  }
  else if(dev->full.pipe && dev->full.pipe->processed_width > 0)
  {
    if(width  ) *width   = dev->full.pipe->processed_width  / iscale;
    if(height ) *height  = dev->full.pipe->processed_height / iscale;
  }
  else
  {
    if(width  ) *width   = preview->backbuf_width;
    if(height ) *height  = preview->backbuf_height;
  }

  // iwidth/iheight must equal pipe->iwidth/iheight (the pipeline input dimensions
  // used to scale corners in _path_get_pts_border / other mask get_points_border
  // functions), so that backtransform(corner * pipe->iwidth) / iwidth = corner.
  if(iwidth ) *iwidth  = preview->iwidth;
  if(iheight) *iheight = preview->iheight;

}

G_END_DECLS

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
