/*
    This file is part of darktable,
    Copyright (C) 2011-2021 darktable developers.

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
#include "common/colorlabels.h"
#include "common/collection.h"
#include "control/control.h"
#include "dtgtk/button.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include "libs/lib_api.h"

DT_MODULE(1)

typedef struct dt_lib_colorlabels_t
{
  char *tooltips[6];
  GtkWidget *buttons[6];
} dt_lib_colorlabels_t;

/* callback when a colorlabel button is clicked */
static void _lib_colorlabels_button_clicked_callback(GtkWidget *w, gpointer user_data);

const char *name(dt_lib_module_t *self)
{
  return _("colorlabels");
}

const char **views(dt_lib_module_t *self)
{
  static const char *v[] = {"lighttable", "tethering", NULL};
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_CENTER_BOTTOM_LEFT;
}

int expandable(dt_lib_module_t *self)
{
  return 0;
}

int position()
{
  return 1001;
}

static gboolean _lib_colorlabels_enter_notify_callback(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  darktable.control->element = (GPOINTER_TO_INT(user_data) + 1) % 6;
  return FALSE;
}

void gui_init(dt_lib_module_t *self)
{
  /* initialize ui widgets */
  dt_lib_colorlabels_t *d = (dt_lib_colorlabels_t *)g_malloc0(sizeof(dt_lib_colorlabels_t));
  self->data = (void *)d;

  /* create buttons */
  self->widget = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *button;
  dt_action_t *ac;
  for(int k = 0; k < 6; k++)
  {
    button = dtgtk_button_new(dtgtk_cairo_paint_label, (k | 8 | CPF_LABEL_PURPLE), NULL);
    d->buttons[k] = button;
    dt_gui_add_class(d->buttons[k], "dt_no_hover");
    dt_gui_add_class(d->buttons[k], "dt_dimmed");
    gtk_widget_set_tooltip_text(button, _("toggle color label of selected images"));
    gtk_box_pack_start(GTK_BOX(self->widget), button, TRUE, TRUE, 0);
    g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(_lib_colorlabels_button_clicked_callback),
                     GINT_TO_POINTER(k));
    g_signal_connect(G_OBJECT(button), "enter-notify-event", G_CALLBACK(_lib_colorlabels_enter_notify_callback),
                     GINT_TO_POINTER(k));
    ac = dt_action_define(&darktable.control->actions_thumb, NULL, N_("color label"), button, &dt_action_def_color_label);
  }

  dt_shortcut_register(ac, 1, 0, GDK_KEY_F1, 0);
  dt_shortcut_register(ac, 2, 0, GDK_KEY_F2, 0);
  dt_shortcut_register(ac, 3, 0, GDK_KEY_F3, 0);
  dt_shortcut_register(ac, 4, 0, GDK_KEY_F4, 0);
  dt_shortcut_register(ac, 5, 0, GDK_KEY_F5, 0);

  gtk_widget_set_name(self->widget, "lib-label-colors");
}

void gui_cleanup(dt_lib_module_t *self)
{
  g_free(self->data);
  self->data = NULL;
}

static void _lib_colorlabels_button_clicked_callback(GtkWidget *w, gpointer user_data)
{
  GList *imgs = dt_act_on_get_images(FALSE, TRUE, FALSE);
  dt_colorlabels_toggle_label_on_list(imgs, GPOINTER_TO_INT(user_data), TRUE);
  dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, DT_COLLECTION_PROP_COLORLABEL,
                             imgs);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

