/*
    This file is part of darktable,
    Copyright (C) 2011-2024 darktable developers.

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

#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif

DT_MODULE(1)

typedef struct dt_lib_colorlabels_t
{
  char *tooltips[6];
  GtkWidget *buttons[6];
  GtkWidget *floating_window;
  gint colorlabel;
} dt_lib_colorlabels_t;

/* callback when a colorlabel button is clicked */
static void _lib_colorlabels_button_clicked_callback(GtkWidget *w,
                                                     GdkEventButton *event,
                                                     dt_lib_module_t *self);

gint _get_colorlabel(dt_lib_module_t *self, GtkWidget *w)
{
  dt_lib_colorlabels_t *d = self->data;
  for(int k = 0; k < 6; k++)
  {
    if(d->buttons[k] == w)
      return k;
  }
  return -1;
}

const char *name(dt_lib_module_t *self)
{
  return _("colorlabels");
}

dt_view_type_flags_t views(dt_lib_module_t *self)
{
  return DT_VIEW_LIGHTTABLE | DT_VIEW_TETHERING;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_CENTER_BOTTOM_LEFT;
}

int expandable(dt_lib_module_t *self)
{
  return 0;
}

int position(const dt_lib_module_t *self)
{
  return 1001;
}

static gboolean _lib_colorlabels_enter_notify_callback(GtkWidget *widget,
                                                       GdkEventCrossing *event,
                                                       dt_lib_module_t *self)
{
  const gint colorlabel = _get_colorlabel(self, widget);

  darktable.control->element = (colorlabel + 1) % 6;
  return FALSE;
}

static char *_get_tooltip_for(const int coloridx)
{
  char confname[128];
  snprintf(confname, sizeof(confname), "colorlabel/%s", dt_colorlabels_name[coloridx]);

  const gchar *text = dt_conf_get_string_const(confname);

  char *tooltip =
    text[0]
    ? g_markup_printf_escaped(_("toggle color label of selected images\n"
                                "<i>%s</i>"),
                              text)
    : g_strdup(_("toggle color label of selected images"));

  return tooltip;
}

void gui_init(dt_lib_module_t *self)
{
  /* initialize ui widgets */
  dt_lib_colorlabels_t *d = g_malloc0(sizeof(dt_lib_colorlabels_t));
  self->data = (void *)d;

  /* create buttons box */
  self->widget = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

  dt_action_t *ac;

  for(int k = 0; k < 6; k++)
  {
    GtkWidget *button =
      dtgtk_button_new(dtgtk_cairo_paint_label, (k | 8 | CPF_LABEL_PURPLE), NULL);
    d->buttons[k] = button;
    dt_gui_add_class(d->buttons[k], "dt_no_hover");
    dt_gui_add_class(d->buttons[k], "dt_dimmed");
    // Only the first 5 buttons are color label togglers, the last one clears all labels
    char *tooltip = k < 5
                    ? _get_tooltip_for(k)
                    : g_strdup(_("clear color labels of selected images"));
    gtk_widget_set_tooltip_markup(button, tooltip);
    g_free(tooltip);
    gtk_box_pack_start(GTK_BOX(self->widget), button, TRUE, TRUE, 0);
    g_signal_connect(G_OBJECT(button), "button-press-event",
                     G_CALLBACK(_lib_colorlabels_button_clicked_callback),
                     self);
    g_signal_connect(G_OBJECT(button), "enter-notify-event",
                     G_CALLBACK(_lib_colorlabels_enter_notify_callback),
                     self);
    ac = dt_action_define(&darktable.control->actions_thumb, NULL,
                          N_("color label"), button, &dt_action_def_color_label);
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

#define FLOATING_ENTRY_WIDTH DT_PIXEL_APPLY_DPI(150)

static gboolean _lib_colorlabels_key_press(GtkWidget *entry,
                                           GdkEventKey *event,
                                           dt_lib_module_t *self)
{
  dt_lib_colorlabels_t *d = self->data;

  switch(event->keyval)
  {
    case GDK_KEY_Escape:
      gtk_widget_destroy(d->floating_window);
      gtk_window_present(GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)));
      return TRUE;
    case GDK_KEY_Tab:
      return TRUE;
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
    {
      char confname[128];
      const gchar *label = gtk_entry_get_text(GTK_ENTRY(entry));
      snprintf(confname, sizeof(confname), "colorlabel/%s",
               dt_colorlabels_name[d->colorlabel]);
      dt_conf_set_string(confname, label);

      char *tooltip = _get_tooltip_for(d->colorlabel);
      gtk_widget_set_tooltip_markup(d->buttons[d->colorlabel], tooltip);
      g_free(tooltip);

      gtk_widget_destroy(d->floating_window);
      gtk_window_present(GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)));
      return TRUE;
    }
  }
  return FALSE; /* event not handled */
}

static gboolean _lib_colorlabels_destroy(GtkWidget *widget,
                                         GdkEvent *event,
                                         dt_lib_module_t *self)
{
  dt_lib_colorlabels_t *d = self->data;

  gtk_widget_destroy(d->floating_window);
  return FALSE;
}

static void _lib_colorlabels_edit(dt_lib_module_t *self,
                                  GdkEventButton *event)
{
  dt_lib_colorlabels_t *d = self->data;

  GtkWidget *window = dt_ui_main_window(darktable.gui->ui);

  const gint x = event->x_root;
  const gint y = event->y_root - DT_PIXEL_APPLY_DPI(50);

  d->floating_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(d->floating_window);
#endif
  /* stackoverflow.com/questions/1925568/how-to-give-keyboard-focus-to-a-pop-up-gtk-window */
  gtk_widget_set_can_focus(d->floating_window, TRUE);
  gtk_window_set_decorated(GTK_WINDOW(d->floating_window), FALSE);
  gtk_window_set_type_hint(GTK_WINDOW(d->floating_window), GDK_WINDOW_TYPE_HINT_POPUP_MENU);
  gtk_window_set_transient_for(GTK_WINDOW(d->floating_window), GTK_WINDOW(window));
  gtk_widget_set_opacity(d->floating_window, 0.8);
  gtk_window_move(GTK_WINDOW(d->floating_window), x, y);

  GtkWidget *entry = gtk_entry_new();
  gtk_widget_set_size_request(entry, FLOATING_ENTRY_WIDTH, -1);
  gtk_widget_add_events(entry, GDK_FOCUS_CHANGE_MASK);

  gtk_editable_select_region(GTK_EDITABLE(entry), 0, -1);
  gtk_container_add(GTK_CONTAINER(d->floating_window), entry);
  g_signal_connect(entry, "focus-out-event",
                   G_CALLBACK(_lib_colorlabels_destroy), self);
  g_signal_connect(entry, "key-press-event",
                   G_CALLBACK(_lib_colorlabels_key_press), self);
  gtk_widget_set_tooltip_text(entry,
                              _("enter a description of how you use this color label"));

  gtk_widget_show_all(d->floating_window);
  gtk_widget_grab_focus(entry);
  gtk_window_present(GTK_WINDOW(d->floating_window));
}

static void _lib_colorlabels_button_clicked_callback(GtkWidget *w,
                                                     GdkEventButton *event,
                                                     dt_lib_module_t *self)
{
  dt_lib_colorlabels_t *d = self->data;

  const gint colorlabel = _get_colorlabel(self, w);

  if(event->type == GDK_BUTTON_PRESS
     && event->button == 3
     && colorlabel != 5)  // The button to reset colorlabels needs no description
  {
    d->colorlabel = colorlabel;
    _lib_colorlabels_edit(self, event);
  }
  else
  {
    GList *imgs = dt_act_on_get_images(FALSE, TRUE, FALSE);
    dt_colorlabels_toggle_label_on_list(imgs, colorlabel, TRUE);
    dt_collection_update_query(darktable.collection,
                               DT_COLLECTION_CHANGE_RELOAD, DT_COLLECTION_PROP_COLORLABEL,
                               imgs);
  }
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
