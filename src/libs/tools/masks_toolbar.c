/*
    This file is part of darktable,
    Copyright (C) 2021 darktable developers.

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
#include "common/debug.h"
#include "common/image_cache.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/masks/masks.h"
#include "develop/masks.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include "libs/lib_api.h"

#include <gdk/gdkkeysyms.h>
#include <gtk/gtk.h>
#include <stdlib.h>
#include <ctype.h>

#include <glib.h>

DT_MODULE(1)


typedef struct dt_lib_masks_toolbar_t
{
  GtkWidget *opacity;
  GtkWidget *pressure;
  GtkWidget *smoothing;
  GtkWidget *hardness;
  dt_masks_form_gui_t *gui;
  dt_masks_form_t *form;
  int have_gui;
  int have_form;
} dt_lib_masks_toolbar_t;


const char *name(dt_lib_module_t *self)
{
  return _("masks toolbar");
}

const char **views(dt_lib_module_t *self)
{
  static const char *v[] = {"darkroom", NULL};
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return  DT_UI_CONTAINER_PANEL_CENTER_TOP_LEFT;
}

int expandable(dt_lib_module_t *self)
{
  return 0;
}

int position()
{
  return 10;
}

void refresh_masks_cache(dt_lib_masks_toolbar_t *d)
{
  if(darktable.develop->form_gui)
  {
    memcpy(d->gui, darktable.develop->form_gui, sizeof(dt_masks_form_gui_t));
    d->have_gui = TRUE;
  }
  else
  {
    d->have_gui = FALSE;
  }
  if(darktable.develop->form_visible)
  {
    memcpy(d->form, darktable.develop->form_visible, sizeof(dt_masks_form_t));
    d->have_form = TRUE;
  }
  else
  {
    d->have_form = FALSE;
  }
}

int button_pressed(struct dt_lib_module_t *self, double x, double y, double pressure, int which, int type,
                   uint32_t state)
{
  dt_lib_masks_toolbar_t *d = (dt_lib_masks_toolbar_t *)self->data;

  if(which == 1)
  {
    // On click, cache the current masks forms stack that is under the mouse
    // because it gets voided as soon as mouse leaves a form.
    // This is a trick over the mask behaviour that uses the "on mouse hover" logic (with mouse scroll actions and key modifiers)
    // but does not allow to permanently select a form on mouse click.
    // To work with Wacom tablets and pens, or just with toolbars, we need permanent selections because
    // selection and setting are asynchronous.
    refresh_masks_cache(d);
  }

  return 0; // we don't capture mouse events, only use them to cache data
}

void gui_post_expose(struct dt_lib_module_t *self, cairo_t *cr, int32_t width, int32_t height,
                     int32_t pointerx, int32_t pointery)
{
  dt_lib_masks_toolbar_t *d = (dt_lib_masks_toolbar_t *)self->data;

  // Update toolbar depending on current mask features
  if(d->have_gui && d->have_form && d->form->points)
  {
    const dt_masks_point_group_t *fpt = (dt_masks_point_group_t *)g_list_nth_data(d->form->points, d->gui->group_edited);
    const dt_masks_form_t *selected = (fpt) ? dt_masks_get_from_id(darktable.develop, fpt->formid) : d->form;

    if(selected->functions && (selected->functions->supported_features & DT_MASK_SUPPORT_OPACITY))
    {
      // update opacity slider
      const float opacity = get_mask_opacity(d->gui, d->form);
      dt_bauhaus_slider_set(d->opacity, opacity);
      gtk_widget_show(d->opacity);
    }
    else
      gtk_widget_hide(d->opacity);


    if(selected->functions && (selected->functions->supported_features & DT_MASK_SUPPORT_HARDNESS))
    {
      // update hardness
      const float hardness = get_mask_hardness(d->gui, d->form);
      dt_bauhaus_slider_set(d->hardness, hardness);
      gtk_widget_show(d->hardness);
    }
    else
      gtk_widget_hide(d->hardness);
  }
  else
  {
    // hide all
    gtk_widget_hide(d->opacity);
    gtk_widget_hide(d->hardness);
  }
}


void _opacity_changed(GtkWidget *range, gpointer user_data)
{
  dt_lib_masks_toolbar_t *d = (dt_lib_masks_toolbar_t *)user_data;
  set_mask_opacity(d->gui, d->form, dt_bauhaus_slider_get(range));
  dt_dev_add_masks_history_item(darktable.develop, darktable.develop->gui_module, TRUE);
  dt_masks_update_image(darktable.develop);
}

void _hardness_changed(GtkWidget *range, gpointer user_data)
{
  dt_lib_masks_toolbar_t *d = (dt_lib_masks_toolbar_t *)user_data;
  set_mask_hardness(d->gui, d->form, dt_bauhaus_slider_get(range));
  dt_dev_add_masks_history_item(darktable.develop, darktable.develop->gui_module, TRUE);
  dt_masks_gui_form_remove(d->form, d->gui, 0);
  dt_masks_gui_form_create(d->form, d->gui, 0, darktable.develop->gui_module);
}

void gui_init(dt_lib_module_t *self)
{
  /* initialize ui widgets */
  dt_lib_masks_toolbar_t *d = (dt_lib_masks_toolbar_t *)g_malloc0(sizeof(dt_lib_masks_toolbar_t));
  self->data = (void *)d;

  d->gui = g_malloc0(sizeof(dt_masks_form_gui_t));
  d->form = g_malloc0(sizeof(dt_masks_form_t));
  d->have_form = FALSE;
  d->have_gui = FALSE;

  self->widget = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

  d->opacity = dt_bauhaus_slider_new_with_range(NULL, 0., 1., 0.01, 1., 2);
  dt_bauhaus_widget_set_label(d->opacity, NULL, N_("opacity"));
  dt_bauhaus_slider_set_factor(d->opacity, 100.);
  dt_bauhaus_slider_set_format(d->opacity, "%.0f %%");
  gtk_widget_set_size_request(d->opacity, DT_PIXEL_APPLY_DPI(150), -1);
  gtk_box_pack_start(GTK_BOX(self->widget), d->opacity, 0, 0, 0);
  g_signal_connect(G_OBJECT(d->opacity), "value-changed", G_CALLBACK(_opacity_changed), (gpointer)self->data);
  gtk_widget_hide(d->opacity);

  d->hardness = dt_bauhaus_slider_new_with_range(NULL, 0., 1., 0.01, 1., 2);
  dt_bauhaus_widget_set_label(d->hardness, NULL, N_("hardness"));
  dt_bauhaus_slider_set_factor(d->hardness, 100.);
  dt_bauhaus_slider_set_format(d->hardness, "%.0f %%");
  gtk_widget_set_size_request(d->hardness, DT_PIXEL_APPLY_DPI(150), -1);
  gtk_box_pack_start(GTK_BOX(self->widget), d->hardness, 0, 0, 0);
  g_signal_connect(G_OBJECT(d->hardness), "value-changed", G_CALLBACK(_hardness_changed), (gpointer)self->data);
  gtk_widget_hide(d->hardness);

/*
  d->pressure = dt_bauhaus_combobox_new(NULL);
  dt_bauhaus_widget_set_label(d->pressure, NULL, N_("pressure"));
  dt_bauhaus_combobox_add(d->pressure, _("off"));
  dt_bauhaus_combobox_add(d->pressure, _("hardness (absolute)"));
  dt_bauhaus_combobox_add(d->pressure, _("hardness (relative)"));
  dt_bauhaus_combobox_add(d->pressure, _("opacity (absolute)"));
  dt_bauhaus_combobox_add(d->pressure, _("opacity (relative)"));
  dt_bauhaus_combobox_add(d->pressure, _("brush size (relative)"));
  gtk_box_pack_start(GTK_BOX(self->widget), d->pressure, 0, 0, 0);
  gchar *psens = dt_conf_get_string("pressure_sensitivity");
  dt_bauhaus_combobox_set_from_text(d->pressure, psens);
  g_free(psens);
  */
}

void gui_cleanup(dt_lib_module_t *self)
{
  dt_lib_masks_toolbar_t *d = (dt_lib_masks_toolbar_t *)self->data;
  g_free(d->gui);
  g_free(d->form);
  g_free(self->data);
  self->data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
