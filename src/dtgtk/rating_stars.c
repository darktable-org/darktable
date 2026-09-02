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

#include "dtgtk/rating_stars.h"
#include "dtgtk/button.h"
#include "dtgtk/paint.h"
#include "control/control.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"

#define DT_RATING_STARS_N 6
#define DT_RATING_STARS_BUTTONS_KEY "dt-rating-stars-buttons"

/* Fill on prelight/active like the filter range icons (paint_star needs data). */
void dtgtk_cairo_paint_rating_star(cairo_t *cr,
                                   const gint x,
                                   const gint y,
                                   const gint w,
                                   const gint h,
                                   const gint flags,
                                   void *data)
{
  void *fill = NULL;
  GdkRGBA shade;

  if((flags & CPF_PRELIGHT) || (flags & CPF_ACTIVE))
  {
    if(cairo_pattern_get_rgba(cairo_get_source(cr),
                              &shade.red, &shade.green, &shade.blue, &shade.alpha)
       == CAIRO_STATUS_SUCCESS)
    {
      shade.alpha *= 0.5;
      fill = &shade;
    }
  }

  dtgtk_cairo_paint_star(cr, x, y, w, h, flags, fill);
}

static GtkWidget **_stars_buttons(GtkWidget *box)
{
  return g_object_get_data(G_OBJECT(box), DT_RATING_STARS_BUTTONS_KEY);
}

static void _stars_set_prelight(GtkWidget *box,
                                const int upto)
{
  GtkWidget **buttons = _stars_buttons(box);
  if(!buttons) return;

  for(int k = 1; k < DT_RATING_STARS_N; k++)
  {
    if(k <= upto)
      gtk_widget_set_state_flags(buttons[k], GTK_STATE_FLAG_PRELIGHT, FALSE);
    else
      gtk_widget_unset_state_flags(buttons[k], GTK_STATE_FLAG_PRELIGHT);
    gtk_widget_queue_draw(buttons[k]);
  }
}

static void _stars_enter_cb(GtkEventControllerMotion *controller,
                            const double x,
                            const double y,
                            gpointer user_data)
{
  GtkWidget *box = user_data;
  GtkWidget *widget = dt_gui_get_widget(controller);
  const int rating = dtgtk_rating_stars_get_value(widget);
  if(rating < 0) return;

  if(rating >= 1)
    darktable.control->element = rating;

  _stars_set_prelight(box, rating);
}

static void _stars_leave_cb(GtkEventControllerMotion *controller,
                            gpointer user_data)
{
  _stars_set_prelight(user_data, 0);
}

int dtgtk_rating_stars_get_value(GtkWidget *button)
{
  if(!button) return -1;
  if(!g_object_get_data(G_OBJECT(button), "dt-rating-stars-member"))
    return -1;
  return GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), DT_RATING_STARS_VALUE_KEY));
}

GtkWidget *dtgtk_rating_stars_new(GCallback pressed,
                                  gpointer data)
{
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name(box, "lib-rating-stars");
  gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(box, GTK_ALIGN_CENTER);

  GtkWidget **buttons = g_malloc0_n(DT_RATING_STARS_N, sizeof(GtkWidget *));
  g_object_set_data_full(G_OBJECT(box), DT_RATING_STARS_BUTTONS_KEY, buttons, g_free);

  static const char *tooltips[DT_RATING_STARS_N] = {
    N_("clear star rating of selected images"),
    N_("set 1-star rating for selected images"),
    N_("set 2-star rating for selected images"),
    N_("set 3-star rating for selected images"),
    N_("set 4-star rating for selected images"),
    N_("set 5-star rating for selected images"),
  };

  for(int k = 0; k < DT_RATING_STARS_N; k++)
  {
    DTGTKCairoPaintIconFunc paint =
      (k == 0) ? dtgtk_cairo_paint_unratestar : dtgtk_cairo_paint_rating_star;

    GtkWidget *button = dtgtk_button_new(paint, 0, NULL);
    buttons[k] = button;

    char name[32];
    g_snprintf(name, sizeof(name), "rating-star-%d", k);
    gtk_widget_set_name(button, name);
    dt_gui_add_class(button, "dt_rating_star");
    dt_gui_add_class(button, "dt_transparent_background");
    gtk_widget_set_tooltip_text(button, _(tooltips[k]));

    g_object_set_data(G_OBJECT(button), DT_RATING_STARS_VALUE_KEY, GINT_TO_POINTER(k));
    g_object_set_data(G_OBJECT(button), "dt-rating-stars-member", GINT_TO_POINTER(1));

    /* GtkButton: claim so class handlers do not eat the first press
     * (same gtk4-prep pattern as footer color labels). Prefer
     * dt_gui_connect_click_claim() if that helper is already on the branch.*/
    {
      GtkGesture *gesture = gtk_gesture_multi_press_new(button);
      gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(gesture),
                                                 GTK_PHASE_CAPTURE);
      dt_gui_add_controller(button, gesture);
      if(pressed)
        g_signal_connect(gesture, "pressed", G_CALLBACK(pressed), data);
      g_signal_connect(gesture, "begin", G_CALLBACK(dt_gui_gesture_claim), NULL);
      gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(gesture), 0);
      g_object_set_data(G_OBJECT(button), DT_ACTION_GESTURE_KEY, gesture);
    }

    dt_gui_connect_motion(button, NULL, _stars_enter_cb, _stars_leave_cb, box);

    gtk_box_pack_start(GTK_BOX(box), button, FALSE, FALSE, 0);
  }

  return box;
}
