/*
    This file is part of darktable,
    copyright (c) 2009--2011 johannes hanika.
    copyright (c) 2012 tobias ellinghaus.
    copyright (c) 2014 henrik andersson.

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
#include "bauhaus/bauhaus.h"
#include "common/darktable.h"
#include "common/camera_control.h"
#include "common/image_cache.h"
#include "common/mipmap_cache.h"
#include "control/jobs.h"
#include "control/control.h"
#include "control/conf.h"
#include "libs/lib.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/draw.h"
#include "gui/guides.h"
#include <gdk/gdkkeysyms.h>
#include "dtgtk/button.h"

typedef enum dt_lib_liveview_guide_t
{
  GUIDE_NONE = 0,
  GUIDE_GRID,
  GUIDE_THIRD,
  GUIDE_DIAGONAL,
  GUIDE_TRIANGL,
  GUIDE_GOLDEN
} dt_lib_liveview_guide_t;

typedef enum dt_lib_liveview_overlay_t
{
  OVERLAY_NONE = 0,
  OVERLAY_SELECTED,
  OVERLAY_ID
} dt_lib_liveview_overlay_t;

#define HANDLE_SIZE 0.02

static const cairo_operator_t _overlay_modes[] = {
  CAIRO_OPERATOR_OVER, CAIRO_OPERATOR_XOR, CAIRO_OPERATOR_ADD, CAIRO_OPERATOR_SATURATE
#if(CAIRO_VERSION >= CAIRO_VERSION_ENCODE(1, 10, 0))
  ,
  CAIRO_OPERATOR_MULTIPLY, CAIRO_OPERATOR_SCREEN, CAIRO_OPERATOR_OVERLAY, CAIRO_OPERATOR_DARKEN,
  CAIRO_OPERATOR_LIGHTEN, CAIRO_OPERATOR_COLOR_DODGE, CAIRO_OPERATOR_COLOR_BURN, CAIRO_OPERATOR_HARD_LIGHT,
  CAIRO_OPERATOR_SOFT_LIGHT, CAIRO_OPERATOR_DIFFERENCE, CAIRO_OPERATOR_EXCLUSION, CAIRO_OPERATOR_HSL_HUE,
  CAIRO_OPERATOR_HSL_SATURATION, CAIRO_OPERATOR_HSL_COLOR, CAIRO_OPERATOR_HSL_LUMINOSITY
#endif
};

DT_MODULE(1)

typedef struct dt_lib_live_view_t
{
  int imgid;
  int splitline_rotation;
  double overlay_x0, overlay_x1, overlay_y0, overlay_y1;
  double splitline_x, splitline_y; // 0..1
  gboolean splitline_dragging;

  GtkWidget *live_view, *live_view_zoom, *rotate_ccw, *rotate_cw, *flip;
  GtkWidget *focus_out_small, *focus_out_big, *focus_in_small, *focus_in_big;
  GtkWidget *guide_selector, *flip_guides, *golden_extras;
  GtkWidget *overlay, *overlay_id_box, *overlay_id, *overlay_mode, *overlay_splitline;
} dt_lib_live_view_t;

static void guides_presets_changed(GtkWidget *combo, dt_lib_live_view_t *lib)
{
  int which = dt_bauhaus_combobox_get(combo);
  if(which == GUIDE_TRIANGL || which == GUIDE_GOLDEN)
    gtk_widget_set_visible(GTK_WIDGET(lib->flip_guides), TRUE);
  else
    gtk_widget_set_visible(GTK_WIDGET(lib->flip_guides), FALSE);

  if(which == GUIDE_GOLDEN)
    gtk_widget_set_visible(GTK_WIDGET(lib->golden_extras), TRUE);
  else
    gtk_widget_set_visible(GTK_WIDGET(lib->golden_extras), FALSE);
}

static void overlay_changed(GtkWidget *combo, dt_lib_live_view_t *lib)
{
  int which = dt_bauhaus_combobox_get(combo);
  if(which == OVERLAY_NONE)
  {
    gtk_widget_set_visible(GTK_WIDGET(lib->overlay_mode), FALSE);
    gtk_widget_set_visible(GTK_WIDGET(lib->overlay_splitline), FALSE);
  }
  else
  {
    gtk_widget_set_visible(GTK_WIDGET(lib->overlay_mode), TRUE);
    gtk_widget_set_visible(GTK_WIDGET(lib->overlay_splitline), TRUE);
  }

  if(which == OVERLAY_ID)
    gtk_widget_set_visible(GTK_WIDGET(lib->overlay_id_box), TRUE);
  else
    gtk_widget_set_visible(GTK_WIDGET(lib->overlay_id_box), FALSE);
}


const char *name()
{
  return _("live view");
}

uint32_t views()
{
  return DT_VIEW_TETHERING;
}

uint32_t container()
{
  return DT_UI_CONTAINER_PANEL_RIGHT_CENTER;
}


void gui_reset(dt_lib_module_t *self)
{
}

int position()
{
  return 998;
}

void init_key_accels(dt_lib_module_t *self)
{
  dt_accel_register_lib(self, NC_("accel", "toggle live view"), GDK_KEY_v, 0);
  dt_accel_register_lib(self, NC_("accel", "rotate 90 degrees CCW"), 0, 0);
  dt_accel_register_lib(self, NC_("accel", "rotate 90 degrees CW"), 0, 0);
  dt_accel_register_lib(self, NC_("accel", "flip horizontally"), 0, 0);
  dt_accel_register_lib(self, NC_("accel", "move focus point in (big steps)"), 0, 0);
  dt_accel_register_lib(self, NC_("accel", "move focus point in (small steps)"), 0, 0);
  dt_accel_register_lib(self, NC_("accel", "move focus point out (small steps)"), 0, 0);
  dt_accel_register_lib(self, NC_("accel", "move focus point out (big steps)"), 0, 0);
}

void connect_key_accels(dt_lib_module_t *self)
{
  dt_lib_live_view_t *lib = (dt_lib_live_view_t *)self->data;

  dt_accel_connect_button_lib(self, "toggle live view", GTK_WIDGET(lib->live_view));
  dt_accel_connect_button_lib(self, "rotate 90 degrees CCW", GTK_WIDGET(lib->rotate_ccw));
  dt_accel_connect_button_lib(self, "rotate 90 degrees CW", GTK_WIDGET(lib->rotate_cw));
  dt_accel_connect_button_lib(self, "flip horizontally", GTK_WIDGET(lib->flip));
  dt_accel_connect_button_lib(self, "move focus point in (big steps)", GTK_WIDGET(lib->focus_in_big));
  dt_accel_connect_button_lib(self, "move focus point in (small steps)", GTK_WIDGET(lib->focus_in_small));
  dt_accel_connect_button_lib(self, "move focus point out (small steps)", GTK_WIDGET(lib->focus_out_small));
  dt_accel_connect_button_lib(self, "move focus point out (big steps)", GTK_WIDGET(lib->focus_out_big));
}

static void _rotate_ccw(GtkWidget *widget, gpointer user_data)
{
  dt_camera_t *cam = (dt_camera_t *)darktable.camctl->active_camera;
  cam->live_view_rotation = (cam->live_view_rotation + 1) % 4; // 0 -> 1 -> 2 -> 3 -> 0 -> ...
}

static void _rotate_cw(GtkWidget *widget, gpointer user_data)
{
  dt_camera_t *cam = (dt_camera_t *)darktable.camctl->active_camera;
  cam->live_view_rotation = (cam->live_view_rotation + 3) % 4; // 0 -> 3 -> 2 -> 1 -> 0 -> ...
}

// Congratulations to Simon for being the first one recognizing live view in a screen shot ^^
static void _toggle_live_view_clicked(GtkWidget *widget, gpointer user_data)
{
  if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget)) == TRUE)
  {
    if(dt_camctl_camera_start_live_view(darktable.camctl) == FALSE)
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widget), FALSE);
  }
  else
  {
    dt_camctl_camera_stop_live_view(darktable.camctl);
  }
}

// TODO: using a toggle button would be better, but this setting can also be changed by right clicking on the
// canvas (src/views/capture.c).
//       maybe using a signal would work? i have no idea.
static void _zoom_live_view_clicked(GtkWidget *widget, gpointer user_data)
{
  dt_camera_t *cam = (dt_camera_t *)darktable.camctl->active_camera;
  if(cam->is_live_viewing)
  {
    cam->live_view_zoom = !cam->live_view_zoom;
    if(cam->live_view_zoom == TRUE)
      dt_camctl_camera_set_property_string(darktable.camctl, NULL, "eoszoom", "5");
    else
      dt_camctl_camera_set_property_string(darktable.camctl, NULL, "eoszoom", "1");
  }
}

static const gchar *focus_array[] = { "Near 3", "Near 2", "Near 1", "Far 1", "Far 2", "Far 3" };
static void _focus_button_clicked(GtkWidget *widget, gpointer user_data)
{
  int focus = GPOINTER_TO_INT(user_data);
  if(focus >= 0 && focus <= 5)
    dt_camctl_camera_set_property_string(darktable.camctl, NULL, "manualfocusdrive",
                                         g_dgettext("libgphoto2-2", focus_array[focus]));
}

static void _toggle_flip_clicked(GtkWidget *widget, gpointer user_data)
{
  dt_camera_t *cam = (dt_camera_t *)darktable.camctl->active_camera;
  cam->live_view_flip = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));
}

static void _overlay_id_changed(GtkWidget *widget, gpointer user_data)
{
  dt_lib_live_view_t *lib = (dt_lib_live_view_t *)user_data;
  lib->imgid = gtk_spin_button_get_value(GTK_SPIN_BUTTON(widget));
  dt_conf_set_int("plugins/lighttable/live_view/overlay_imgid", lib->imgid);
}

static void _overlay_mode_changed(GtkWidget *combo, gpointer user_data)
{
  dt_conf_set_int("plugins/lighttable/live_view/overlay_mode", dt_bauhaus_combobox_get(combo));
}

static void _overlay_splitline_changed(GtkWidget *combo, gpointer user_data)
{
  dt_conf_set_int("plugins/lighttable/live_view/splitline", dt_bauhaus_combobox_get(combo));
}

void gui_init(dt_lib_module_t *self)
{
  self->data = calloc(1, sizeof(dt_lib_live_view_t));

  // Setup lib data
  dt_lib_live_view_t *lib = self->data;
  lib->splitline_x = lib->splitline_y = 0.5;

  // Setup gui
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
  GtkWidget *box;

  box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
  gtk_box_pack_start(GTK_BOX(self->widget), box, TRUE, TRUE, 0);
  lib->live_view = dtgtk_togglebutton_new(dtgtk_cairo_paint_eye, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER);
  lib->live_view_zoom = dtgtk_button_new(
      dtgtk_cairo_paint_zoom, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER); // TODO: see _zoom_live_view_clicked
  lib->rotate_ccw = dtgtk_button_new(dtgtk_cairo_paint_refresh, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER);
  lib->rotate_cw = dtgtk_button_new(dtgtk_cairo_paint_refresh,
                                    CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER | CPF_DIRECTION_UP);
  lib->flip = dtgtk_togglebutton_new(dtgtk_cairo_paint_flip,
                                     CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER | CPF_DIRECTION_UP);

  gtk_box_pack_start(GTK_BOX(box), lib->live_view, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(box), lib->live_view_zoom, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(box), lib->rotate_ccw, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(box), lib->rotate_cw, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(box), lib->flip, TRUE, TRUE, 0);

  g_object_set(G_OBJECT(lib->live_view), "tooltip-text", _("toggle live view"), (char *)NULL);
  g_object_set(G_OBJECT(lib->live_view_zoom), "tooltip-text", _("zoom live view"), (char *)NULL);
  g_object_set(G_OBJECT(lib->rotate_ccw), "tooltip-text", _("rotate 90 degrees ccw"), (char *)NULL);
  g_object_set(G_OBJECT(lib->rotate_cw), "tooltip-text", _("rotate 90 degrees cw"), (char *)NULL);
  g_object_set(G_OBJECT(lib->flip), "tooltip-text", _("flip live view horizontally"), (char *)NULL);

  g_signal_connect(G_OBJECT(lib->live_view), "clicked", G_CALLBACK(_toggle_live_view_clicked), lib);
  g_signal_connect(G_OBJECT(lib->live_view_zoom), "clicked", G_CALLBACK(_zoom_live_view_clicked), lib);
  g_signal_connect(G_OBJECT(lib->rotate_ccw), "clicked", G_CALLBACK(_rotate_ccw), lib);
  g_signal_connect(G_OBJECT(lib->rotate_cw), "clicked", G_CALLBACK(_rotate_cw), lib);
  g_signal_connect(G_OBJECT(lib->flip), "clicked", G_CALLBACK(_toggle_flip_clicked), lib);

  // focus buttons
  box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
  gtk_box_pack_start(GTK_BOX(self->widget), box, TRUE, TRUE, 0);
  lib->focus_in_big = dtgtk_button_new(dtgtk_cairo_paint_solid_triangle,
                                       CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER | CPF_DIRECTION_LEFT);
  lib->focus_in_small
      = dtgtk_button_new(dtgtk_cairo_paint_arrow, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER
                                                  | CPF_DIRECTION_LEFT); // TODO icon not centered
  lib->focus_out_small = dtgtk_button_new(dtgtk_cairo_paint_arrow, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER
                                                                   | CPF_DIRECTION_RIGHT); // TODO same here
  lib->focus_out_big = dtgtk_button_new(dtgtk_cairo_paint_solid_triangle,
                                        CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER | CPF_DIRECTION_RIGHT);

  gtk_box_pack_start(GTK_BOX(box), lib->focus_in_big, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(box), lib->focus_in_small, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(box), lib->focus_out_small, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(box), lib->focus_out_big, TRUE, TRUE, 0);

  g_object_set(G_OBJECT(lib->focus_in_big), "tooltip-text", _("move focus point in (big steps)"),
               (char *)NULL);
  g_object_set(G_OBJECT(lib->focus_in_small), "tooltip-text", _("move focus point in (small steps)"),
               (char *)NULL);
  g_object_set(G_OBJECT(lib->focus_out_small), "tooltip-text", _("move focus point out (small steps)"),
               (char *)NULL);
  g_object_set(G_OBJECT(lib->focus_out_big), "tooltip-text", _("move focus point out (big steps)"),
               (char *)NULL);

  // 1 and 4 would be medium steps, not in ui right now ...
  g_signal_connect(G_OBJECT(lib->focus_in_big), "clicked", G_CALLBACK(_focus_button_clicked),
                   GINT_TO_POINTER(0));
  g_signal_connect(G_OBJECT(lib->focus_in_small), "clicked", G_CALLBACK(_focus_button_clicked),
                   GINT_TO_POINTER(2));
  g_signal_connect(G_OBJECT(lib->focus_out_small), "clicked", G_CALLBACK(_focus_button_clicked),
                   GINT_TO_POINTER(3));
  g_signal_connect(G_OBJECT(lib->focus_out_big), "clicked", G_CALLBACK(_focus_button_clicked),
                   GINT_TO_POINTER(5));

  // Guides
  lib->guide_selector = dt_bauhaus_combobox_new(NULL);
  dt_bauhaus_widget_set_label(lib->guide_selector, NULL, _("guides"));
  dt_bauhaus_combobox_add(lib->guide_selector, _("none"));
  dt_bauhaus_combobox_add(lib->guide_selector, _("grid"));
  dt_bauhaus_combobox_add(lib->guide_selector, _("rules of thirds"));
  dt_bauhaus_combobox_add(lib->guide_selector, _("diagonal method"));
  dt_bauhaus_combobox_add(lib->guide_selector, _("harmonious triangles"));
  dt_bauhaus_combobox_add(lib->guide_selector, _("golden mean"));
  g_object_set(G_OBJECT(lib->guide_selector), "tooltip-text",
               _("display guide lines to help compose your photograph"), (char *)NULL);
  g_signal_connect(G_OBJECT(lib->guide_selector), "value-changed", G_CALLBACK(guides_presets_changed), lib);
  gtk_box_pack_start(GTK_BOX(self->widget), lib->guide_selector, TRUE, TRUE, 0);

  lib->flip_guides = dt_bauhaus_combobox_new(NULL);
  dt_bauhaus_widget_set_label(lib->flip_guides, NULL, _("flip"));
  dt_bauhaus_combobox_add(lib->flip_guides, _("none"));
  dt_bauhaus_combobox_add(lib->flip_guides, _("horizontally"));
  dt_bauhaus_combobox_add(lib->flip_guides, _("vertically"));
  dt_bauhaus_combobox_add(lib->flip_guides, _("both"));
  g_object_set(G_OBJECT(lib->flip_guides), "tooltip-text", _("flip guides"), (char *)NULL);
  gtk_box_pack_start(GTK_BOX(self->widget), lib->flip_guides, TRUE, TRUE, 0);

  lib->golden_extras = dt_bauhaus_combobox_new(NULL);
  dt_bauhaus_widget_set_label(lib->golden_extras, NULL, _("extra"));
  dt_bauhaus_combobox_add(lib->golden_extras, _("golden sections"));
  dt_bauhaus_combobox_add(lib->golden_extras, _("golden spiral sections"));
  dt_bauhaus_combobox_add(lib->golden_extras, _("golden spiral"));
  dt_bauhaus_combobox_add(lib->golden_extras, _("all"));
  g_object_set(G_OBJECT(lib->golden_extras), "tooltip-text", _("show some extra guides"), (char *)NULL);
  gtk_box_pack_start(GTK_BOX(self->widget), lib->golden_extras, TRUE, TRUE, 0);

  lib->overlay = dt_bauhaus_combobox_new(NULL);
  dt_bauhaus_widget_set_label(lib->overlay, NULL, _("overlay"));
  dt_bauhaus_combobox_add(lib->overlay, _("none"));
  dt_bauhaus_combobox_add(lib->overlay, _("selected image"));
  dt_bauhaus_combobox_add(lib->overlay, _("id"));
  g_object_set(G_OBJECT(lib->overlay), "tooltip-text", _("overlay another image over the live view"),
               (char *)NULL);
  g_signal_connect(G_OBJECT(lib->overlay), "value-changed", G_CALLBACK(overlay_changed), lib);
  gtk_box_pack_start(GTK_BOX(self->widget), lib->overlay, TRUE, TRUE, 0);

  lib->overlay_id_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
  GtkWidget *label = gtk_label_new(_("image id"));
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  lib->overlay_id = gtk_spin_button_new_with_range(0, 1000000000, 1);
  gtk_spin_button_set_digits(GTK_SPIN_BUTTON(lib->overlay_id), 0);
  g_object_set(G_OBJECT(lib->overlay_id), "tooltip-text", _("enter image id of the overlay manually"),
               (char *)NULL);
  g_signal_connect(G_OBJECT(lib->overlay_id), "value-changed", G_CALLBACK(_overlay_id_changed), lib);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(lib->overlay_id),
                            dt_conf_get_int("plugins/lighttable/live_view/overlay_imgid"));
  gtk_box_pack_start(GTK_BOX(lib->overlay_id_box), label, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(lib->overlay_id_box), lib->overlay_id, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), lib->overlay_id_box, TRUE, TRUE, 0);
  gtk_widget_show(lib->overlay_id);
  gtk_widget_show(label);

  lib->overlay_mode = dt_bauhaus_combobox_new(NULL);
  dt_bauhaus_widget_set_label(lib->overlay_mode, NULL, _("overlay mode"));
  dt_bauhaus_combobox_add(lib->overlay_mode, C_("blendmode", "normal"));
  dt_bauhaus_combobox_add(lib->overlay_mode, C_("blendmode", "xor"));
  dt_bauhaus_combobox_add(lib->overlay_mode, C_("blendmode", "add"));
  dt_bauhaus_combobox_add(lib->overlay_mode, C_("blendmode", "saturate"));
#if(CAIRO_VERSION >= CAIRO_VERSION_ENCODE(1, 10, 0))
  dt_bauhaus_combobox_add(lib->overlay_mode, C_("blendmode", "multiply"));
  dt_bauhaus_combobox_add(lib->overlay_mode, C_("blendmode", "screen"));
  dt_bauhaus_combobox_add(lib->overlay_mode, C_("blendmode", "overlay"));
  dt_bauhaus_combobox_add(lib->overlay_mode, C_("blendmode", "darken"));
  dt_bauhaus_combobox_add(lib->overlay_mode, C_("blendmode", "lighten"));
  dt_bauhaus_combobox_add(lib->overlay_mode, C_("blendmode", "color dodge"));
  dt_bauhaus_combobox_add(lib->overlay_mode, C_("blendmode", "color burn"));
  dt_bauhaus_combobox_add(lib->overlay_mode, C_("blendmode", "hard light"));
  dt_bauhaus_combobox_add(lib->overlay_mode, C_("blendmode", "soft light"));
  dt_bauhaus_combobox_add(lib->overlay_mode, C_("blendmode", "difference"));
  dt_bauhaus_combobox_add(lib->overlay_mode, C_("blendmode", "exclusion"));
  dt_bauhaus_combobox_add(lib->overlay_mode, C_("blendmode", "HSL hue"));
  dt_bauhaus_combobox_add(lib->overlay_mode, C_("blendmode", "HSL saturation"));
  dt_bauhaus_combobox_add(lib->overlay_mode, C_("blendmode", "HSL color"));
  dt_bauhaus_combobox_add(lib->overlay_mode, C_("blendmode", "HSL luminosity"));
#endif
  g_object_set(G_OBJECT(lib->overlay_mode), "tooltip-text", _("mode of the overlay"), (char *)NULL);
  dt_bauhaus_combobox_set(lib->overlay_mode, dt_conf_get_int("plugins/lighttable/live_view/overlay_mode"));
  g_signal_connect(G_OBJECT(lib->overlay_mode), "value-changed", G_CALLBACK(_overlay_mode_changed), lib);
  gtk_box_pack_start(GTK_BOX(self->widget), lib->overlay_mode, TRUE, TRUE, 0);

  lib->overlay_splitline = dt_bauhaus_combobox_new(NULL);
  dt_bauhaus_widget_set_label(lib->overlay_splitline, NULL, _("split line"));
  dt_bauhaus_combobox_add(lib->overlay_splitline, _("off"));
  dt_bauhaus_combobox_add(lib->overlay_splitline, _("on"));
  g_object_set(G_OBJECT(lib->overlay_splitline), "tooltip-text", _("only draw part of the overlay"),
               (char *)NULL);
  dt_bauhaus_combobox_set(lib->overlay_splitline, dt_conf_get_int("plugins/lighttable/live_view/splitline"));
  g_signal_connect(G_OBJECT(lib->overlay_splitline), "value-changed", G_CALLBACK(_overlay_splitline_changed),
                   lib);
  gtk_box_pack_start(GTK_BOX(self->widget), lib->overlay_splitline, TRUE, TRUE, 0);

  gtk_widget_set_visible(GTK_WIDGET(lib->flip_guides), FALSE);
  gtk_widget_set_visible(GTK_WIDGET(lib->golden_extras), FALSE);
  gtk_widget_set_visible(GTK_WIDGET(lib->overlay_mode), FALSE);
  gtk_widget_set_visible(GTK_WIDGET(lib->overlay_id_box), FALSE);
  gtk_widget_set_visible(GTK_WIDGET(lib->overlay_splitline), FALSE);

  gtk_widget_set_no_show_all(GTK_WIDGET(lib->flip_guides), TRUE);
  gtk_widget_set_no_show_all(GTK_WIDGET(lib->golden_extras), TRUE);
  gtk_widget_set_no_show_all(GTK_WIDGET(lib->overlay_mode), TRUE);
  gtk_widget_set_no_show_all(GTK_WIDGET(lib->overlay_id_box), TRUE);
  gtk_widget_set_no_show_all(GTK_WIDGET(lib->overlay_splitline), TRUE);

  // disable buttons that won't work with this camera
  // TODO: initialize tethering mode outside of libs/camera.s so we can use darktable.camctl->active_camera
  // here
  const dt_camera_t *cam = darktable.camctl->active_camera;
  if(cam == NULL) cam = darktable.camctl->wanted_camera;
  if(cam != NULL && cam->can_live_view_advanced == FALSE)
  {
    gtk_widget_set_sensitive(lib->live_view_zoom, FALSE);
    gtk_widget_set_sensitive(lib->focus_in_big, FALSE);
    gtk_widget_set_sensitive(lib->focus_in_small, FALSE);
    gtk_widget_set_sensitive(lib->focus_out_big, FALSE);
    gtk_widget_set_sensitive(lib->focus_out_small, FALSE);
  }
}

void gui_cleanup(dt_lib_module_t *self)
{
}

// TODO: find out where the zoom window is and draw overlay + grid accordingly
#define MARGIN 20
#define BAR_HEIGHT 18 /* see libs/camera.c */
void gui_post_expose(dt_lib_module_t *self, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx,
                     int32_t pointery)
{
  dt_camera_t *cam = (dt_camera_t *)darktable.camctl->active_camera;
  dt_lib_live_view_t *lib = self->data;

  if(cam->is_live_viewing == FALSE || cam->live_view_zoom == TRUE) return;

  dt_pthread_mutex_lock(&cam->live_view_pixbuf_mutex);
  if(GDK_IS_PIXBUF(cam->live_view_pixbuf) == FALSE)
  {
    dt_pthread_mutex_unlock(&cam->live_view_pixbuf_mutex);
    return;
  }
  double w = width - (MARGIN * 2.0f);
  double h = height - (MARGIN * 2.0f) - BAR_HEIGHT;
  gint pw = gdk_pixbuf_get_width(cam->live_view_pixbuf);
  gint ph = gdk_pixbuf_get_height(cam->live_view_pixbuf);
  lib->overlay_x0 = lib->overlay_x1 = lib->overlay_y0 = lib->overlay_y1 = 0.0;

  gboolean use_splitline = (dt_bauhaus_combobox_get(lib->overlay_splitline) == 1);

  // OVERLAY
  int imgid = 0;
  switch(dt_bauhaus_combobox_get(lib->overlay))
  {
    case OVERLAY_SELECTED:
      imgid = dt_view_tethering_get_selected_imgid(darktable.view_manager);
      break;
    case OVERLAY_ID:
      imgid = lib->imgid;
      break;
  }
  if(imgid > 0)
  {
    cairo_save(cr);
    const dt_image_t *img = dt_image_cache_testget(darktable.image_cache, imgid, 'r');
    // if the user points at this image, we really want it:
    if(!img) img = dt_image_cache_get(darktable.image_cache, imgid, 'r');

    int zoom = 1;
    float imgwd = 0.90f;
    if(zoom == 1)
    {
      imgwd = .97f;
    }

    dt_mipmap_buffer_t buf;
    dt_mipmap_size_t mip = dt_mipmap_cache_get_matching_size(darktable.mipmap_cache, imgwd * w, imgwd * h);
    dt_mipmap_cache_get(darktable.mipmap_cache, &buf, imgid, mip, 0, 'r');

    float scale = 1.0;
    cairo_surface_t *surface = NULL;
    if(buf.buf)
    {
      const int32_t stride = cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, buf.width);
      surface = cairo_image_surface_create_for_data(buf.buf, CAIRO_FORMAT_RGB24, buf.width,
                                                    buf.height, stride);
      if(zoom == 1)
      {
        scale = fminf(fminf(w, pw) / (float)buf.width, fminf(h, ph) / (float)buf.height);
      }
      else
        scale = fminf(w * imgwd / (float)buf.width, h * imgwd / (float)buf.height);
    }

    // draw centered and fitted:
    cairo_translate(cr, width / 2.0, (height + BAR_HEIGHT) / 2.0f);
    cairo_scale(cr, scale, scale);

    if(buf.buf)
    {
      cairo_translate(cr, -.5f * buf.width, -.5f * buf.height);

      if(use_splitline)
      {
        double x0, y0, x1, y1;
        switch(lib->splitline_rotation)
        {
          case 0:
            x0 = 0.0;
            y0 = 0.0;
            x1 = buf.width * lib->splitline_x;
            y1 = buf.height;
            break;
          case 1:
            x0 = 0.0;
            y0 = 0.0;
            x1 = buf.width;
            y1 = buf.height * lib->splitline_y;
            break;
          case 2:
            x0 = buf.width * lib->splitline_x;
            y0 = 0.0;
            x1 = buf.width;
            y1 = buf.height;
            break;
          case 3:
            x0 = 0.0;
            y0 = buf.height * lib->splitline_y;
            x1 = buf.width;
            y1 = buf.height;
            break;
          default:
            fprintf(stderr, "OMFG, the world will collapse, this shouldn't be reachable!\n");
            return;
        }

        cairo_rectangle(cr, x0, y0, x1, y1);
        cairo_clip(cr);
      }

      cairo_set_source_surface(cr, surface, 0, 0);
      // set filter no nearest:
      // in skull mode, we want to see big pixels.
      // in 1 iir mode for the right mip, we want to see exactly what the pipe gave us, 1:1 pixel for pixel.
      // in between, filtering just makes stuff go unsharp.
      if((buf.width <= 8 && buf.height <= 8) || fabsf(scale - 1.0f) < 0.01f)
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
      cairo_rectangle(cr, 0, 0, buf.width, buf.height);
      int overlay_modes_index = dt_bauhaus_combobox_get(lib->overlay_mode);
      if(overlay_modes_index >= 0)
      {
        cairo_operator_t mode = _overlay_modes[overlay_modes_index];
        cairo_set_operator(cr, mode);
      }
      cairo_fill(cr);
      cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
      cairo_surface_destroy(surface);
    }
    cairo_restore(cr);
    if(buf.buf) dt_mipmap_cache_release(darktable.mipmap_cache, &buf);
    if(img) dt_image_cache_read_release(darktable.image_cache, img);

    // ON CANVAS CONTROLS
    if(use_splitline)
    {
      float scale = fminf(1.0, fminf(w / pw, h / ph));

      // image coordinates
      lib->overlay_x0 = 0.5 * (width - pw * scale);
      lib->overlay_y0 = 0.5 * (height - ph * scale + BAR_HEIGHT);
      lib->overlay_x1 = lib->overlay_x0 + pw * scale;
      lib->overlay_y1 = lib->overlay_y0 + ph * scale;

      // splitline position to absolute coords:
      double sl_x = lib->overlay_x0 + lib->splitline_x * pw * scale;
      double sl_y = lib->overlay_y0 + lib->splitline_y * ph * scale;

      int x0 = sl_x, y0 = 0.0, x1 = x0, y1 = height;
      if(lib->splitline_rotation % 2 != 0)
      {
        x0 = 0.0;
        y0 = sl_y;
        x1 = width;
        y1 = y0;
      }
      gboolean mouse_over_control = (lib->splitline_rotation % 2 == 0) ? (fabs(sl_x - pointerx) < 5)
                                                                       : (fabs(sl_y - pointery) < 5);
      cairo_save(cr);
      cairo_set_source_rgb(cr, .7, .7, .7);
      cairo_set_line_width(cr, (mouse_over_control ? 2.0 : 0.5));

      cairo_move_to(cr, x0, y0);
      cairo_line_to(cr, x1, y1);
      cairo_stroke(cr);

      /* if mouse over control lets draw center rotate control, hide if split is dragged */
      if(!lib->splitline_dragging && mouse_over_control)
      {
        cairo_set_line_width(cr, 0.5);
        double s = width * HANDLE_SIZE;
        dtgtk_cairo_paint_refresh(cr, sl_x - (s * 0.5), sl_y - (s * 0.5), s, s, 1);
      }

      cairo_restore(cr);
    }
  }

  // GUIDES
  if(cam->live_view_rotation % 2 == 1)
  {
    gint tmp = pw;
    pw = ph;
    ph = tmp;
  }
  float scale = 1.0;
  //   if(cam->live_view_zoom == FALSE)
  //   {
  if(pw > w) scale = w / pw;
  if(ph > h) scale = fminf(scale, h / ph);
  //   }
  double sw = scale * pw;
  double sh = scale * ph;

  // draw guides
  double left = (width - scale * pw) * 0.5;
  double right = left + scale * pw;
  double top = (height + BAR_HEIGHT - scale * ph) * 0.5;
  double bottom = top + scale * ph;

  double dashes = 5.0;

  cairo_save(cr);
  cairo_set_dash(cr, &dashes, 1, 0);

  int guide_flip = dt_bauhaus_combobox_get(lib->flip_guides);
  int which = dt_bauhaus_combobox_get(lib->guide_selector);
  switch(which)
  {
    case GUIDE_GRID:
      dt_guides_draw_simple_grid(cr, left, top, right, bottom, 1.0);
      break;

    case GUIDE_DIAGONAL:
      dt_guides_draw_diagonal_method(cr, left, top, sw, sh);
      cairo_stroke(cr);
      cairo_set_dash(cr, &dashes, 0, 0);
      cairo_set_source_rgba(cr, .3, .3, .3, .8);
      dt_guides_draw_diagonal_method(cr, left, top, sw, sh);
      cairo_stroke(cr);
      break;
    case GUIDE_THIRD:
      dt_guides_draw_rules_of_thirds(cr, left, top, right, bottom, sw / 3.0, sh / 3.0);
      cairo_stroke(cr);
      cairo_set_dash(cr, &dashes, 0, 0);
      cairo_set_source_rgba(cr, .3, .3, .3, .8);
      dt_guides_draw_rules_of_thirds(cr, left, top, right, bottom, sw / 3.0, sh / 3.0);
      cairo_stroke(cr);
      break;
    case GUIDE_TRIANGL:
    {
      int dst = (int)((sh * cos(atan(sw / sh)) / (cos(atan(sh / sw)))));
      // Move coordinates to local center selection.
      cairo_translate(cr, ((right - left) / 2 + left), ((bottom - top) / 2 + top));

      // Flip horizontal.
      if(guide_flip & 1) cairo_scale(cr, -1, 1);
      // Flip vertical.
      if(guide_flip & 2) cairo_scale(cr, 1, -1);

      dt_guides_draw_harmonious_triangles(cr, left, top, right, bottom, dst);
      cairo_stroke(cr);
      // p.setPen(QPen(d->guideColor, d->guideSize, Qt::DotLine));
      cairo_set_dash(cr, &dashes, 0, 0);
      cairo_set_source_rgba(cr, .3, .3, .3, .8);
      dt_guides_draw_harmonious_triangles(cr, left, top, right, bottom, dst);
      cairo_stroke(cr);
    }
    break;
    case GUIDE_GOLDEN:
    {
      // Move coordinates to local center selection.
      cairo_translate(cr, ((right - left) / 2 + left), ((bottom - top) / 2 + top));

      // Flip horizontal.
      if(guide_flip & 1) cairo_scale(cr, -1, 1);
      // Flip vertical.
      if(guide_flip & 2) cairo_scale(cr, 1, -1);

      float w = sw;
      float h = sh;

      // lengths for the golden mean and half the sizes of the region:
      float w_g = w * INVPHI;
      float h_g = h * INVPHI;
      float w_2 = w / 2;
      float h_2 = h / 2;

      dt_QRect_t R1, R2, R3, R4, R5, R6, R7;
      dt_guides_q_rect(&R1, -w_2, -h_2, w_g, h);

      // w - 2*w_2 corrects for one-pixel difference
      // so that R2.right() is really at the right end of the region
      dt_guides_q_rect(&R2, w_g - w_2, h_2 - h_g, w - w_g + 1 - (w - 2 * w_2), h_g);
      dt_guides_q_rect(&R3, w_2 - R2.width * INVPHI, -h_2, R2.width * INVPHI, h - R2.height);
      dt_guides_q_rect(&R4, R2.left, R1.top, R3.left - R2.left, R3.height * INVPHI);
      dt_guides_q_rect(&R5, R4.left, R4.bottom, R4.width * INVPHI, R3.height - R4.height);
      dt_guides_q_rect(&R6, R5.left + R5.width, R5.bottom - R5.height * INVPHI, R3.left - R5.right,
                       R5.height * INVPHI);
      dt_guides_q_rect(&R7, R6.right - R6.width * INVPHI, R4.bottom, R6.width * INVPHI, R5.height - R6.height);

      const int extras = dt_bauhaus_combobox_get(lib->golden_extras);
      dt_guides_draw_golden_mean(cr, &R1, &R2, &R3, &R4, &R5, &R6, &R7, extras == 0 || extras == 3, 0,
                                 extras == 1 || extras == 3, extras == 2 || extras == 3);
      cairo_stroke(cr);

      cairo_set_dash(cr, &dashes, 0, 0);
      cairo_set_source_rgba(cr, .3, .3, .3, .8);
      dt_guides_draw_golden_mean(cr, &R1, &R2, &R3, &R4, &R5, &R6, &R7, extras == 0 || extras == 3, 0,
                                 extras == 1 || extras == 3, extras == 2 || extras == 3);
      cairo_stroke(cr);
    }
    break;
  }
  cairo_restore(cr);
  dt_pthread_mutex_unlock(&cam->live_view_pixbuf_mutex);
}

int button_released(struct dt_lib_module_t *self, double x, double y, int which, uint32_t state)
{
  dt_lib_live_view_t *d = (dt_lib_live_view_t *)self->data;
  if(d->splitline_dragging == TRUE)
  {
    d->splitline_dragging = FALSE;
    return 1;
  }
  return 0;
}

int button_pressed(struct dt_lib_module_t *self, double x, double y, double pressure, int which, int type,
                   uint32_t state)
{
  dt_lib_live_view_t *lib = (dt_lib_live_view_t *)self->data;
  int result = 0;

  int imgid = 0;
  switch(dt_bauhaus_combobox_get(lib->overlay))
  {
    case OVERLAY_SELECTED:
      imgid = dt_view_tethering_get_selected_imgid(darktable.view_manager);
      break;
    case OVERLAY_ID:
      imgid = lib->imgid;
      break;
  }

  if(imgid > 0 && dt_bauhaus_combobox_get(lib->overlay_splitline))
  {
    const double width = lib->overlay_x1 - lib->overlay_x0;
    const double height = lib->overlay_y1 - lib->overlay_y0;

    // splitline position to absolute coords:
    double sl_x = lib->overlay_x0 + lib->splitline_x * width;
    double sl_y = lib->overlay_y0 + lib->splitline_y * height;

    gboolean mouse_over_control = (lib->splitline_rotation % 2 == 0) ? (fabs(sl_x - x) < 5)
                                                                     : (fabs(sl_y - y) < 5);

    /* do the split rotating */
    if(which == 1 && fabs(sl_x - x) < 7 && fabs(sl_y - y) < 7)
    {
      /* let's rotate */
      lib->splitline_rotation = (lib->splitline_rotation + 1) % 4;

      dt_control_queue_redraw_center();
      result = 1;
    }
    /* do the dragging !? */
    else if(which == 1 && mouse_over_control)
    {
      lib->splitline_dragging = TRUE;
      dt_control_queue_redraw_center();
      result = 1;
    }
  }
  return result;
}

int mouse_moved(dt_lib_module_t *self, double x, double y, double pressure, int which)
{
  dt_lib_live_view_t *lib = (dt_lib_live_view_t *)self->data;
  int result = 0;

  if(lib->splitline_dragging)
  {
    const double width = lib->overlay_x1 - lib->overlay_x0;
    const double height = lib->overlay_y1 - lib->overlay_y0;

    // absolute coords to splitline position:
    lib->splitline_x = CLAMPS((x - lib->overlay_x0) / width, 0.0, 1.0);
    lib->splitline_y = CLAMPS((y - lib->overlay_y0) / height, 0.0, 1.0);

    result = 1;
  }

  return result;
}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
