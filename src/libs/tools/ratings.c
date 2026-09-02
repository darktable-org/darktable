/*
    This file is part of darktable,
    Copyright (C) 2011-2026 darktable developers.

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
#include "common/ratings.h"
#include "common/collection.h"
#include "common/debug.h"
#include "control/control.h"
#include "dtgtk/button.h"
#include "dtgtk/rating_stars.h"
#include "gui/draw.h"
#include "gui/gtk.h"
#include "gui/accelerators.h"
#include "libs/lib.h"
#include "libs/lib_api.h"

DT_MODULE(1)

/* button press handler */
static void _lib_ratings_button_press_callback(GtkGestureSingle *gesture, int n_press,
                                                  double x, double y,
                                                  dt_lib_module_t *self);

const char *name(dt_lib_module_t *self)
{
  return _("ratings");
}

dt_view_type_flags_t views(dt_lib_module_t *self)
{
  return DT_VIEW_LIGHTTABLE | DT_VIEW_TETHERING;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_CENTER_BOTTOM_LEFT;
}

gboolean expandable(dt_lib_module_t *self)
{
  return FALSE;
}

int position(const dt_lib_module_t *self)
{
  return 1002;
}

void gui_init(dt_lib_module_t *self)
{
  self->data = NULL;
  self->widget = dtgtk_rating_stars_new(G_CALLBACK(_lib_ratings_button_press_callback), self);

  /* map each button so shortcut mode sees the hovered widget as actionable */
  GList *buttons = gtk_container_get_children(GTK_CONTAINER(self->widget));
  dt_action_t *ac = NULL;
  for(const GList *button = buttons; button; button = g_list_next(button))
    ac = dt_action_define(&darktable.control->actions_thumb, NULL, N_("rating"),
                          GTK_WIDGET(button->data), &dt_action_def_rating);
  g_list_free(buttons);

  dt_shortcut_register(ac, 0, 0, GDK_KEY_0, 0);
  dt_shortcut_register(ac, 1, 0, GDK_KEY_1, 0);
  dt_shortcut_register(ac, 2, 0, GDK_KEY_2, 0);
  dt_shortcut_register(ac, 3, 0, GDK_KEY_3, 0);
  dt_shortcut_register(ac, 4, 0, GDK_KEY_4, 0);
  dt_shortcut_register(ac, 5, 0, GDK_KEY_5, 0);
  dt_shortcut_register(ac, 6, 0, GDK_KEY_r, 0);
}

void gui_cleanup(dt_lib_module_t *self)
{
  g_free(self->data);
  self->data = NULL;
}

static void _lib_ratings_button_press_callback(GtkGestureSingle *gesture, int n_press,
                                                  double x, double y,
                                                  dt_lib_module_t *self)
{
  GtkWidget *widget = dt_gui_get_widget(gesture);
  const int rating = dtgtk_rating_stars_get_value(widget);
  if(rating < 0) return;

  GList *imgs = dt_act_on_get_images(FALSE, TRUE, FALSE);
  dt_ratings_apply_on_list(imgs, rating, TRUE);
  dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD,
                             DT_COLLECTION_PROP_RATING_RANGE, imgs);
  
  dt_control_queue_redraw_center();
}



// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
