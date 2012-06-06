/*
    This file is part of darktable,
    copyright (c) 2010 - 2011 henrik andersson.

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
#include "views/capture.h"
#include "common/darktable.h"
#include "common/camera_control.h"
#include "control/jobs.h"
#include "control/control.h"
#include "control/conf.h"
#include "libs/lib.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "dtgtk/label.h"
#include <gdk/gdkkeysyms.h>
#include "dtgtk/button.h"

DT_MODULE(1)

typedef struct dt_lib_live_view_t
{
  GtkWidget *live_view, *rotate_ccw, *rotate_cw;
}
dt_lib_live_view_t;



const char*
name ()
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


void
gui_reset (dt_lib_module_t *self)
{
}

int
position ()
{
  return 998;
}

void init_key_accels(dt_lib_module_t *self)
{
  dt_accel_register_lib(self, NC_("accel", "toggle live view"), GDK_v, 0);
  dt_accel_register_lib(self, NC_("accel", "rotate 90 degrees ccw"), 0, 0);
  dt_accel_register_lib(self, NC_("accel", "rotate 90 degrees cw"), 0, 0);
}

void connect_key_accels(dt_lib_module_t *self)
{
  dt_lib_live_view_t *lib = (dt_lib_live_view_t*)self->data;

  dt_accel_connect_button_lib(self, "toggle live view", GTK_WIDGET(lib->live_view));
  dt_accel_connect_button_lib(self, "rotate 90 degrees ccw", GTK_WIDGET(lib->rotate_ccw));
  dt_accel_connect_button_lib(self, "rotate 90 degrees cw", GTK_WIDGET(lib->rotate_cw));
}

static void _rotate_ccw(GtkWidget *widget, gpointer user_data)
{
  dt_camera_t *cam = (dt_camera_t*)darktable.camctl->active_camera;
  cam->live_view_rotation = (cam->live_view_rotation + 1) % 4; // 0 -> 1 -> 2 -> 3 -> 0 -> ...
}

static void _rotate_cw(GtkWidget *widget, gpointer user_data)
{
  dt_camera_t *cam = (dt_camera_t*)darktable.camctl->active_camera;
  cam->live_view_rotation = (cam->live_view_rotation + 3) % 4; // 0 -> 3 -> 2 -> 1 -> 0 -> ...
}

// Congratulations to Simon for being the first one recognizing live view in a screen shot ^^
static void _toggle_live_view_clicked(GtkWidget *widget, gpointer user_data)
{
  if(gtk_toggle_button_get_active( GTK_TOGGLE_BUTTON(widget) ) == TRUE)
  {
    if(dt_camctl_camera_start_live_view(darktable.camctl) == FALSE)
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widget), FALSE);
  }
  else
  {
    dt_camctl_camera_stop_live_view(darktable.camctl);
  }
}

void
gui_init (dt_lib_module_t *self)
{
  self->data = malloc(sizeof(dt_lib_live_view_t));
  memset(self->data,0,sizeof(dt_lib_live_view_t));

  // Setup lib data
  dt_lib_live_view_t *lib=self->data;

  // Setup gui
  self->widget = gtk_hbox_new(FALSE, 5);

  lib->live_view  = dtgtk_togglebutton_new(dtgtk_cairo_paint_eye, CPF_STYLE_FLAT|CPF_DO_NOT_USE_BORDER);
  lib->rotate_ccw = dtgtk_button_new(dtgtk_cairo_paint_refresh, CPF_STYLE_FLAT|CPF_DO_NOT_USE_BORDER);
  lib->rotate_cw  = dtgtk_button_new(dtgtk_cairo_paint_refresh, CPF_STYLE_FLAT|CPF_DO_NOT_USE_BORDER|1);

  gtk_box_pack_start(GTK_BOX(self->widget), lib->live_view, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), lib->rotate_ccw, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), lib->rotate_cw, TRUE, TRUE, 0);

  g_object_set(G_OBJECT( lib->live_view), "tooltip-text", _("toggle live view"), (char *)NULL);
  g_object_set(G_OBJECT( lib->rotate_ccw), "tooltip-text", _("rotate 90 degrees ccw"), (char *)NULL);
  g_object_set(G_OBJECT( lib->rotate_cw), "tooltip-text", _("rotate 90 degrees cw"), (char *)NULL);

  g_signal_connect(G_OBJECT(lib->live_view), "clicked", G_CALLBACK(_toggle_live_view_clicked), lib);
  g_signal_connect(G_OBJECT(lib->rotate_ccw), "clicked", G_CALLBACK(_rotate_ccw), lib);
  g_signal_connect(G_OBJECT(lib->rotate_cw), "clicked", G_CALLBACK(_rotate_cw), lib);
}

void
gui_cleanup (dt_lib_module_t *self)
{
}

// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
