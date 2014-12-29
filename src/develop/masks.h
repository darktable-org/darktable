/*
    This file is part of darktable,
    copyright (c) 2012 aldric renaudin.

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

#ifndef DT_DEVELOP_MASKS_H
#define DT_DEVELOP_MASKS_H

#include "dtgtk/button.h"
#include "dtgtk/gradientslider.h"
#include "develop/pixelpipe.h"
#include "common/opencl.h"

#define DEVELOP_MASKS_VERSION (2)

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
  DT_MASKS_BRUSH = 1 << 6
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
  DT_MASKS_STATE_EXCLUSION = 1 << 6
} dt_masks_state_t;

typedef enum dt_masks_points_states_t
{
  DT_MASKS_POINT_STATE_NORMAL = 1,
  DT_MASKS_POINT_STATE_USER = 2
} dt_masks_points_states_t;

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
} dt_masks_point_ellipse_t;

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
} dt_masks_point_gradient_t;

/** structure used to store all forms's id for a group */
typedef struct dt_masks_point_group_t
{
  int formid;
  int parentid;
  int state;
  float opacity;
} dt_masks_point_group_t;

/** structure used to define a form */
typedef struct dt_masks_form_t
{
  GList *points; // list of point structures
  dt_masks_type_t type;

  // position of the source (used only for clone)
  float source[2];
  // name of the form
  char name[128];
  // id used to store the form
  int formid;
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

/** structure used to display a form */
typedef struct dt_masks_form_gui_t
{
  // points used to draw the form
  GList *points; // list of dt_masks_form_gui_points_t

  // points used to sample mouse moves
  float *guipoints, *guipoints_payload;
  int guipoints_count;

  // values for mouse positions, etc...
  float posx, posy, dx, dy, scrollx, scrolly;
  gboolean form_selected;
  gboolean border_selected;
  gboolean source_selected;
  gboolean pivot_selected;
  dt_masks_edit_mode_t edit_mode;
  int point_selected;
  int point_edited;
  int feather_selected;
  int seg_selected;
  int point_border_selected;

  gboolean form_dragging;
  gboolean source_dragging;
  gboolean form_rotating;
  int point_dragging;
  int feather_dragging;
  int seg_dragging;
  int point_border_dragging;

  int group_edited;
  int group_selected;


  gboolean creation;
  gboolean creation_closing_form;
  dt_iop_module_t *creation_module;

  dt_masks_pressure_sensitivity_t pressure_sensitivity;

  // ids
  int formid;
  uint64_t pipe_hash;
} dt_masks_form_gui_t;

/** get points in real space with respect of distortion dx and dy are used to eventually move the center of
 * the circle */
int dt_masks_get_points_border(dt_develop_t *dev, dt_masks_form_t *form, float **points, int *points_count,
                               float **border, int *border_count, int source);

/** get the rectangle which include the form and his border */
int dt_masks_get_area(dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece, dt_masks_form_t *form,
                      int *width, int *height, int *posx, int *posy);
int dt_masks_get_source_area(dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece, dt_masks_form_t *form,
                             int *width, int *height, int *posx, int *posy);
/** get the transparency mask of the form and his border */
int dt_masks_get_mask(dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece, dt_masks_form_t *form,
                      float **buffer, int *width, int *height, int *posx, int *posy);
int dt_masks_get_mask_roi(dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece, dt_masks_form_t *form,
                          const dt_iop_roi_t *roi, float *buffer);
int dt_masks_group_render(dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece, dt_masks_form_t *form,
                          float **buffer, int *roi, float scale);
int dt_masks_group_render_roi(dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece, dt_masks_form_t *form,
                              const dt_iop_roi_t *roi, float *buffer);

// returns current masks version
int dt_masks_version(void);

// update masks from older versions
int dt_masks_legacy_params(dt_develop_t *dev, void *params, const int old_version, const int new_version);
/*
 * TODO:
 *
 * int
 * dt_masks_legacy_params(
 *   dt_develop_t *dev,
 *   const void *const old_params, const int old_version,
 *   void *new_params,             const int new_version);
 */

/** we create a completely new form. */
dt_masks_form_t *dt_masks_create(dt_masks_type_t type);
/** retrieve a form with is id */
dt_masks_form_t *dt_masks_get_from_id(dt_develop_t *dev, int id);

/** read the forms from the db */
void dt_masks_read_forms(dt_develop_t *dev);
/** write the forms into the db */
void dt_masks_write_form(dt_masks_form_t *form, dt_develop_t *dev);
void dt_masks_write_forms(dt_develop_t *dev);
void dt_masks_free_form(dt_masks_form_t *form);
void dt_masks_update_image(dt_develop_t *dev);
void dt_masks_cleanup_unused(dt_develop_t *dev);

/** function used to manipulate forms for masks */
void dt_masks_init_form_gui(dt_develop_t *dev);
void dt_masks_change_form_gui(dt_masks_form_t *newform);
void dt_masks_clear_form_gui(dt_develop_t *dev);
void dt_masks_reset_form_gui(void);
void dt_masks_reset_show_masks_icons(void);

int dt_masks_events_mouse_moved(struct dt_iop_module_t *module, double x, double y, double pressure,
                                int which);
int dt_masks_events_button_released(struct dt_iop_module_t *module, double x, double y, int which,
                                    uint32_t state);
int dt_masks_events_button_pressed(struct dt_iop_module_t *module, double x, double y, double pressure,
                                   int which, int type, uint32_t state);
int dt_masks_events_mouse_scrolled(struct dt_iop_module_t *module, double x, double y, int up, uint32_t state);
void dt_masks_events_post_expose(struct dt_iop_module_t *module, cairo_t *cr, int32_t width, int32_t height,
                                 int32_t pointerx, int32_t pointery);

/** functions used to manipulate gui datas */
void dt_masks_gui_form_create(dt_masks_form_t *form, dt_masks_form_gui_t *gui, int index);
void dt_masks_gui_form_remove(dt_masks_form_t *form, dt_masks_form_gui_t *gui, int index);
void dt_masks_gui_form_test_create(dt_masks_form_t *form, dt_masks_form_gui_t *gui);
void dt_masks_gui_form_save_creation(struct dt_iop_module_t *module, dt_masks_form_t *form,
                                     dt_masks_form_gui_t *gui);
void dt_masks_group_ungroup(dt_masks_form_t *dest_grp, dt_masks_form_t *grp);

void dt_masks_iop_edit_toggle_callback(GtkToggleButton *togglebutton, struct dt_iop_module_t *module);
void dt_masks_iop_value_changed_callback(GtkWidget *widget, struct dt_iop_module_t *module);
void dt_masks_set_edit_mode(struct dt_iop_module_t *module, dt_masks_edit_mode_t value);
void dt_masks_iop_update(struct dt_iop_module_t *module);
void dt_masks_iop_combo_populate(struct dt_iop_module_t **m);
void dt_masks_iop_use_same_as(struct dt_iop_module_t *module, struct dt_iop_module_t *src);
int dt_masks_group_get_hash_buffer_length(dt_masks_form_t *form);
char *dt_masks_group_get_hash_buffer(dt_masks_form_t *form, char *str);

void dt_masks_form_remove(struct dt_iop_module_t *module, dt_masks_form_t *grp, dt_masks_form_t *form);
void dt_masks_form_change_opacity(dt_masks_form_t *form, int parentid, int up);
void dt_masks_form_move(dt_masks_form_t *grp, int formid, int up);
int dt_masks_form_duplicate(dt_develop_t *dev, int formid);

/** utils functions */
int dt_masks_point_in_form_exact(float x, float y, float *points, int points_start, int points_count);
int dt_masks_point_in_form_near(float x, float y, float *points, int points_start, int points_count, float distance, int *near);
#endif
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
