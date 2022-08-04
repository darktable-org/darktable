/*
    This file is part of darktable,
    Copyright (C) 2012-2021 darktable developers.

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
#include "common/camera_control.h"
#include "common/darktable.h"
#include "common/image_cache.h"
#include "common/mipmap_cache.h"
#include "control/conf.h"
#include "control/control.h"
#include "control/jobs.h"
#include "dtgtk/button.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/guides.h"
#include "libs/lib.h"
#include "libs/lib_api.h"
#include <gdk/gdkkeysyms.h>

typedef enum dt_lib_live_view_focus_control_t
{
  DT_FOCUS_NEAR = 0,
  DT_FOCUS_NEARER = 2,
  DT_FOCUS_FAR = 4,
  DT_FOCUS_FARTHER = 6
} dt_lib_live_view_focus_control_t;

typedef enum dt_lib_live_view_flip_t
{
  FLAG_FLIP_NONE = 0,
  FLAG_FLIP_HORIZONTAL = 1<<0,
  FLAG_FLIP_VERTICAL = 1<<1,
  FLAG_FLIP_BOTH = FLAG_FLIP_HORIZONTAL|FLAG_FLIP_VERTICAL
} dt_lib_live_view_flip_t;

typedef enum dt_lib_live_view_overlay_t
{
  OVERLAY_NONE = 0,
  OVERLAY_SELECTED,
  OVERLAY_ID
} dt_lib_live_view_overlay_t;

#define HANDLE_SIZE 0.02

static const cairo_operator_t _overlay_modes[] = {
  CAIRO_OPERATOR_OVER, CAIRO_OPERATOR_XOR, CAIRO_OPERATOR_ADD, CAIRO_OPERATOR_SATURATE,
  CAIRO_OPERATOR_MULTIPLY, CAIRO_OPERATOR_SCREEN, CAIRO_OPERATOR_OVERLAY, CAIRO_OPERATOR_DARKEN,
  CAIRO_OPERATOR_LIGHTEN, CAIRO_OPERATOR_COLOR_DODGE, CAIRO_OPERATOR_COLOR_BURN, CAIRO_OPERATOR_HARD_LIGHT,
  CAIRO_OPERATOR_SOFT_LIGHT, CAIRO_OPERATOR_DIFFERENCE, CAIRO_OPERATOR_EXCLUSION, CAIRO_OPERATOR_HSL_HUE,
  CAIRO_OPERATOR_HSL_SATURATION, CAIRO_OPERATOR_HSL_COLOR, CAIRO_OPERATOR_HSL_LUMINOSITY
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
  GtkWidget *auto_focus, *focus_out_small, *focus_out_big, *focus_in_small, *focus_in_big;

  GtkWidget *overlay, *overlay_id_box, *overlay_id, *overlay_mode, *overlay_splitline;
} dt_lib_live_view_t;

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


const char *name(dt_lib_module_t *self)
{
  return _("Live view");
}

const char **views(dt_lib_module_t *self)
{
  static const char *v[] = {"tethering", NULL};
  return v;
}

uint32_t container(dt_lib_module_t *self)
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

static void _auto_focus_button_clicked(GtkWidget *widget, gpointer user_data)
{
  const char *property = "autofocusdrive";
  CameraWidgetType property_type;
  if(dt_camctl_camera_get_property_type(darktable.camctl, NULL, property, &property_type))
  {
    dt_print(DT_DEBUG_CAMCTL, "[camera control] unable to get property type for %s\n", property);
  }
  else
  {
    if(property_type == GP_WIDGET_TOGGLE)
    {
      dt_camctl_camera_set_property_toggle(darktable.camctl, NULL, property);
    }
    else
    {
      // TODO evaluate if this is the right thing to do in default scenario
      dt_print(DT_DEBUG_CAMCTL, "[camera control] unable to set %s for property type %d\n", property, property_type);
    }
  }
}

static void _focus_button_clicked(GtkWidget *widget, gpointer user_data)
{
  int focus = GPOINTER_TO_INT(user_data);
  CameraWidgetType property_type;
  if(dt_camctl_camera_get_property_type(darktable.camctl, NULL, "manualfocusdrive", &property_type))
  {
    // default to avoid breaking backwards compatibility
    // note that this might not work on non-Canon EOS cameras
    dt_camctl_camera_set_property_choice(darktable.camctl, NULL, "manualfocusdrive", focus);
  }
  else
  {
    // we need to check the property type here because of a peculiar difference between the property type that gphoto2
    // supports for Canon EOS and Nikon systems. In particular, if you have a Canon, expect a TOGGLE or RADIO.
    // If you have a Nikon, expect a RANGE.
    switch(property_type)
    {
      case GP_WIDGET_RANGE:
      {
        float focus_amount;
        switch(focus)
        {
          case DT_FOCUS_NEARER:
            focus_amount = 250;
            break;
          case DT_FOCUS_NEAR:
            focus_amount = 50;
            break;
          case DT_FOCUS_FAR:
            focus_amount = -50;
            break;
          case DT_FOCUS_FARTHER:
            focus_amount = -250;
            break;
          default:
            focus_amount = 0;
        }
        dt_camctl_camera_set_property_float(darktable.camctl, NULL, "manualfocusdrive", focus_amount);
        break;
      }
      case GP_WIDGET_TOGGLE | GP_WIDGET_RADIO:
        dt_camctl_camera_set_property_choice(darktable.camctl, NULL, "manualfocusdrive", focus);
        break;
      default:
        // TODO evaluate if this is the right thing to do in default scenario
        dt_print(DT_DEBUG_CAMCTL, "[camera control] unable to set manualfocusdrive for property type %d", property_type);
        break;
    }
  }
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
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), box, TRUE, TRUE, 0);

  GtkWidget *button;
#define NEW_BUTTON(type, paint, direction, callback, data, action)           \
  button = dtgtk_##type##button_new(paint, direction, NULL);                 \
  gtk_widget_set_tooltip_text(button, action);                               \
  gtk_box_pack_start(GTK_BOX(box), button, TRUE, TRUE, 0);                   \
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(callback), data); \
  dt_action_define(DT_ACTION(self), NULL, action, button, *(#type)?&dt_action_def_toggle:&dt_action_def_button);

  lib->live_view = NEW_BUTTON(toggle, dtgtk_cairo_paint_eye, 0, _toggle_live_view_clicked, lib, N_("Toggle live view"));
  dt_shortcut_register(dt_action_section(DT_ACTION(self), "toggle live view"), 0, 0, GDK_KEY_v, 0);
  lib->live_view_zoom = NEW_BUTTON(, dtgtk_cairo_paint_zoom, 0, _zoom_live_view_clicked, lib, N_("Zoom live view")); // TODO: see _zoom_live_view_clicked
  dt_shortcut_register(dt_action_section(DT_ACTION(self), "zoom live view"), 0, 0, GDK_KEY_w, 0);
  lib->rotate_ccw = NEW_BUTTON(, dtgtk_cairo_paint_refresh, 0, _rotate_ccw, lib, N_("Rotate 90 degrees ccw"));
  lib->rotate_cw = NEW_BUTTON(,dtgtk_cairo_paint_refresh, CPF_DIRECTION_UP, _rotate_cw, lib, N_("Rotate 90 degrees cw"));
  lib->flip = NEW_BUTTON(toggle, dtgtk_cairo_paint_flip, CPF_DIRECTION_UP, _toggle_flip_clicked, lib, N_("Flip live view horizontally"));

  // focus buttons
  box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), box, TRUE, TRUE, 0);

  lib->focus_in_big = NEW_BUTTON(,dtgtk_cairo_paint_solid_triangle, CPF_DIRECTION_LEFT, _focus_button_clicked, GINT_TO_POINTER(DT_FOCUS_NEARER), N_("Move focus point in (big steps)"));
  lib->focus_in_small = NEW_BUTTON(,dtgtk_cairo_paint_arrow, CPF_DIRECTION_LEFT, _focus_button_clicked, GINT_TO_POINTER(DT_FOCUS_NEAR), N_("Move focus point in (small steps)"));// TODO icon not centered
  lib->auto_focus = NEW_BUTTON(,dtgtk_cairo_paint_lock, 0, _auto_focus_button_clicked, GINT_TO_POINTER(1), N_("Run autofocus"));
  lib->focus_out_small = NEW_BUTTON(,dtgtk_cairo_paint_arrow, CPF_DIRECTION_RIGHT, _focus_button_clicked, GINT_TO_POINTER(DT_FOCUS_FAR), N_("Move focus point out (small steps)")); // TODO same here
  lib->focus_out_big = NEW_BUTTON(,dtgtk_cairo_paint_solid_triangle, CPF_DIRECTION_RIGHT, _focus_button_clicked, GINT_TO_POINTER(DT_FOCUS_FARTHER), N_("Move focus point out (big steps)"));
#undef NEW_BUTTON

  lib->overlay = dt_bauhaus_combobox_new_action(DT_ACTION(self));

  dt_bauhaus_widget_set_label(lib->overlay, NULL, N_("Overlay"));
  dt_bauhaus_combobox_add(lib->overlay, _("None"));
  dt_bauhaus_combobox_add(lib->overlay, _("Selected image"));
  dt_bauhaus_combobox_add(lib->overlay, _("Id"));
  gtk_widget_set_tooltip_text(lib->overlay, _("Overlay another image over the live view"));
  g_signal_connect(G_OBJECT(lib->overlay), "value-changed", G_CALLBACK(overlay_changed), lib);
  gtk_box_pack_start(GTK_BOX(self->widget), lib->overlay, TRUE, TRUE, 0);

  lib->overlay_id_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *label = gtk_label_new(_("Image id"));
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  lib->overlay_id = gtk_spin_button_new_with_range(0, 1000000000, 1);
  gtk_spin_button_set_digits(GTK_SPIN_BUTTON(lib->overlay_id), 0);
  gtk_widget_set_tooltip_text(lib->overlay_id, _("Enter image id of the overlay manually"));
  g_signal_connect(G_OBJECT(lib->overlay_id), "value-changed", G_CALLBACK(_overlay_id_changed), lib);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(lib->overlay_id),
                            dt_conf_get_int("plugins/lighttable/live_view/overlay_imgid"));
  gtk_box_pack_start(GTK_BOX(lib->overlay_id_box), label, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(lib->overlay_id_box), lib->overlay_id, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), lib->overlay_id_box, TRUE, TRUE, 0);
  gtk_widget_show(lib->overlay_id);
  gtk_widget_show(label);

  lib->overlay_mode = dt_bauhaus_combobox_new_action(DT_ACTION(self));
  dt_bauhaus_widget_set_label(lib->overlay_mode, NULL, N_("Overlay mode"));
  dt_bauhaus_combobox_add(lib->overlay_mode, C_("blendmode", "Normal"));
  dt_bauhaus_combobox_add(lib->overlay_mode, C_("blendmode", "Xor"));
  dt_bauhaus_combobox_add(lib->overlay_mode, C_("blendmode", "Add"));
  dt_bauhaus_combobox_add(lib->overlay_mode, C_("blendmode", "Saturate"));
  dt_bauhaus_combobox_add(lib->overlay_mode, C_("blendmode", "Multiply"));
  dt_bauhaus_combobox_add(lib->overlay_mode, C_("blendmode", "Screen"));
  dt_bauhaus_combobox_add(lib->overlay_mode, C_("blendmode", "Overlay"));
  dt_bauhaus_combobox_add(lib->overlay_mode, C_("blendmode", "Darken"));
  dt_bauhaus_combobox_add(lib->overlay_mode, C_("blendmode", "Lighten"));
  dt_bauhaus_combobox_add(lib->overlay_mode, C_("blendmode", "Color dodge"));
  dt_bauhaus_combobox_add(lib->overlay_mode, C_("blendmode", "Color burn"));
  dt_bauhaus_combobox_add(lib->overlay_mode, C_("blendmode", "Hard light"));
  dt_bauhaus_combobox_add(lib->overlay_mode, C_("blendmode", "Soft light"));
  dt_bauhaus_combobox_add(lib->overlay_mode, C_("blendmode", "Difference"));
  dt_bauhaus_combobox_add(lib->overlay_mode, C_("blendmode", "Exclusion"));
  dt_bauhaus_combobox_add(lib->overlay_mode, C_("blendmode", "HSL hue"));
  dt_bauhaus_combobox_add(lib->overlay_mode, C_("blendmode", "HSL saturation"));
  dt_bauhaus_combobox_add(lib->overlay_mode, C_("blendmode", "HSL color"));
  dt_bauhaus_combobox_add(lib->overlay_mode, C_("blendmode", "HSL luminosity"));
  gtk_widget_set_tooltip_text(lib->overlay_mode, _("Mode of the overlay"));
  dt_bauhaus_combobox_set(lib->overlay_mode, dt_conf_get_int("plugins/lighttable/live_view/overlay_mode"));
  g_signal_connect(G_OBJECT(lib->overlay_mode), "value-changed", G_CALLBACK(_overlay_mode_changed), lib);
  gtk_box_pack_start(GTK_BOX(self->widget), lib->overlay_mode, TRUE, TRUE, 0);

  lib->overlay_splitline = dt_bauhaus_combobox_new_action(DT_ACTION(self));
  dt_bauhaus_widget_set_label(lib->overlay_splitline, NULL, N_("Split line"));
  dt_bauhaus_combobox_add(lib->overlay_splitline, _("Off"));
  dt_bauhaus_combobox_add(lib->overlay_splitline, _("On"));
  gtk_widget_set_tooltip_text(lib->overlay_splitline, _("Only draw part of the overlay"));
  dt_bauhaus_combobox_set(lib->overlay_splitline, dt_conf_get_int("plugins/lighttable/live_view/splitline"));
  g_signal_connect(G_OBJECT(lib->overlay_splitline), "value-changed", G_CALLBACK(_overlay_splitline_changed),
                   lib);
  gtk_box_pack_start(GTK_BOX(self->widget), lib->overlay_splitline, TRUE, TRUE, 0);

  gtk_widget_set_visible(GTK_WIDGET(lib->overlay_mode), FALSE);
  gtk_widget_set_visible(GTK_WIDGET(lib->overlay_id_box), FALSE);
  gtk_widget_set_visible(GTK_WIDGET(lib->overlay_splitline), FALSE);

  gtk_widget_set_no_show_all(GTK_WIDGET(lib->overlay_mode), TRUE);
  gtk_widget_set_no_show_all(GTK_WIDGET(lib->overlay_id_box), TRUE);
  gtk_widget_set_no_show_all(GTK_WIDGET(lib->overlay_splitline), TRUE);
}

void gui_cleanup(dt_lib_module_t *self)
{
  // dt_lib_live_view_t *lib = self->data;

  // g_list_free(lib->guides_widgets_list);
  // INTENTIONAL. it's supposed to be leaky until lua is fixed.

  free(self->data);
  self->data = NULL;
}

void view_enter(struct dt_lib_module_t *self,struct dt_view_t *old_view,struct dt_view_t *new_view)
{
  // disable buttons that won't work with this camera
  // TODO: initialize tethering mode outside of libs/camera.s so we can use darktable.camctl->active_camera
  // here
  const dt_lib_live_view_t *lib = self->data;
  const dt_camera_t *cam = darktable.camctl->active_camera;
  if(cam == NULL) cam = darktable.camctl->wanted_camera;

  const gboolean sensitive = cam && cam->can_live_view_advanced;

  gtk_widget_set_sensitive(lib->live_view_zoom, sensitive);
  gtk_widget_set_sensitive(lib->focus_in_big, sensitive);
  gtk_widget_set_sensitive(lib->focus_in_small, sensitive);
  gtk_widget_set_sensitive(lib->focus_out_big, sensitive);
  gtk_widget_set_sensitive(lib->focus_out_small, sensitive);
}

void view_leave(struct dt_lib_module_t *self, struct dt_view_t *old_view, struct dt_view_t *new_view)
{
  const dt_lib_live_view_t *lib = self->data;

  // there's no code to automatically restart live view when entering
  // the view, and besides the user may not want to jump right back
  // into live view if they've been out of tethering view doing other
  // things
  if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(lib->live_view)) == TRUE)
  {
    dt_camctl_camera_stop_live_view(darktable.camctl);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(lib->live_view), FALSE);
  }
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

  dt_pthread_mutex_lock(&cam->live_view_buffer_mutex);
  if(!cam->live_view_buffer)
  {
    dt_pthread_mutex_unlock(&cam->live_view_buffer_mutex);
    return;
  }
  const double w = width - (MARGIN * 2.0f);
  const double h = height - (MARGIN * 2.0f) - BAR_HEIGHT;
  gint pw = cam->live_view_width;
  gint ph = cam->live_view_height;
  lib->overlay_x0 = lib->overlay_x1 = lib->overlay_y0 = lib->overlay_y1 = 0.0;

  const gboolean use_splitline = (dt_bauhaus_combobox_get(lib->overlay_splitline) == 1);

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

    const float imgwd = 0.97f;
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
      scale = fminf(fminf(w, pw) / (float)buf.width, fminf(h, ph) / (float)buf.height);
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
            dt_pthread_mutex_unlock(&cam->live_view_buffer_mutex);
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
      const int overlay_modes_index = dt_bauhaus_combobox_get(lib->overlay_mode);
      if(overlay_modes_index >= 0)
      {
        const cairo_operator_t mode = _overlay_modes[overlay_modes_index];
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
      scale = fminf(1.0, fminf(w / pw, h / ph));

      // image coordinates
      lib->overlay_x0 = 0.5 * (width - pw * scale);
      lib->overlay_y0 = 0.5 * (height - ph * scale + BAR_HEIGHT);
      lib->overlay_x1 = lib->overlay_x0 + pw * scale;
      lib->overlay_y1 = lib->overlay_y0 + ph * scale;

      // splitline position to absolute coords:
      const double sl_x = lib->overlay_x0 + lib->splitline_x * pw * scale;
      const double sl_y = lib->overlay_y0 + lib->splitline_y * ph * scale;

      int x0 = sl_x, y0 = 0.0, x1 = x0, y1 = height;
      if(lib->splitline_rotation % 2 != 0)
      {
        x0 = 0.0;
        y0 = sl_y;
        x1 = width;
        y1 = y0;
      }
      const gboolean mouse_over_control = (lib->splitline_rotation % 2 == 0)
        ? (fabs(sl_x - pointerx) < 5)
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
        const double s = width * HANDLE_SIZE;
        dtgtk_cairo_paint_refresh(cr, sl_x - (s * 0.5), sl_y - (s * 0.5), s, s, 1, NULL);
      }

      cairo_restore(cr);
    }
  }

  // GUIDES
  float scale;
  if(cam->live_view_rotation % 2 == 0)
    scale = fminf(w / pw, h / ph);
  else
  {
    const gint tmp = pw;
    pw = ph;
    ph = tmp;

    scale = fminf(w / ph, h / pw);
  }

  // ensure some sanity on the scale factor
  scale = fminf(10.0, scale);

  const double sw = scale * pw;
  const double sh = scale * ph;

  // draw guides
  const double left = (width - sw) * 0.5;
  const double top = (height + BAR_HEIGHT - sh) * 0.5;

  dt_guides_draw(cr, left, top, sw, sh, 1.0);

  dt_pthread_mutex_unlock(&cam->live_view_buffer_mutex);
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
    const double sl_x = lib->overlay_x0 + lib->splitline_x * width;
    const double sl_y = lib->overlay_y0 + lib->splitline_y * height;

    const gboolean mouse_over_control = (lib->splitline_rotation % 2 == 0)
      ? (fabs(sl_x - x) < 5)
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
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

