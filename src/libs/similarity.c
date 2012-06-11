/*
    This file is part of darktable,
    copyright (c) 2011 henrik andersson.

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
#include "common/collection.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/similarity.h"
#include "control/control.h"
#include "control/jobs/control_jobs.h"
#include "control/conf.h"
#include "libs/lib.h"
#include "gui/gtk.h"
#include "dtgtk/slider.h"
#include <gdk/gdkkeysyms.h>

DT_MODULE(1)

const char*
name ()
{
  return _("similar images");
}

uint32_t views()
{
  return DT_VIEW_LIGHTTABLE;
}

uint32_t container()
{
  return DT_UI_CONTAINER_PANEL_LEFT_CENTER;
}

void gui_reset (dt_lib_module_t *self)
{
}

int position ()
{
  return 850;
}


static void _button_callback (GtkWidget *w, gpointer user_data)
{
  dt_similarity_t *data = ( dt_similarity_t *)user_data;
  dt_control_match_similar(data);
}

static void _histogram_weight_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_similarity_t *data = ( dt_similarity_t *)user_data;
  data->histogram_weight = dtgtk_slider_get_value(slider)/100.0;
  dt_conf_set_float("plugins/lighttable/similarity/histogram_weight", data->histogram_weight);
}

static void _lightmap_weight_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_similarity_t *data = ( dt_similarity_t *)user_data;
  data->lightmap_weight = dtgtk_slider_get_value(slider)/100.0;
  dt_conf_set_float("plugins/lighttable/similarity/lightmap_weight", data->lightmap_weight);
}

/* for now, equally weight r,g,b map scoring might be useful
    having this individually controlled.
*/
static void _rgb_weight_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_similarity_t *data = ( dt_similarity_t *)user_data;
  data->redmap_weight = data->greenmap_weight = data->bluemap_weight = dtgtk_slider_get_value(slider)/100.0;
  dt_conf_set_float("plugins/lighttable/similarity/rmap_weight", data->redmap_weight);
  dt_conf_set_float("plugins/lighttable/similarity/gmap_weight", data->greenmap_weight);
  dt_conf_set_float("plugins/lighttable/similarity/bmap_weight", data->bluemap_weight);
}

void gui_init (dt_lib_module_t *self)
{
  dt_similarity_t *d = g_malloc(sizeof(dt_similarity_t));
  memset(d,0,sizeof(dt_similarity_t));
  self->data = d;
  d->histogram_weight = dt_conf_get_float("plugins/lighttable/similarity/histogram_weight");
  d->lightmap_weight = dt_conf_get_float("plugins/lighttable/similarity/lightmap_weight");
  
  /* for now, just take red and set all color maps weighting from that */
  d->redmap_weight = d->greenmap_weight = d->bluemap_weight = dt_conf_get_float("plugins/lighttable/similarity/rmap_weight");
  self->widget = gtk_vbox_new(TRUE, 5);

  /* add the histogram slider */
  GtkDarktableSlider *slider = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 100.0, 2, d->histogram_weight*100, 2));
  dtgtk_slider_set_label(slider,_("histogram weight"));
  dtgtk_slider_set_unit(slider,"%");
  g_signal_connect(G_OBJECT(slider), "value-changed", G_CALLBACK(_histogram_weight_callback),d);
  g_object_set(G_OBJECT(slider), "tooltip-text", _("set the score weight of histogram matching"), (char *)NULL);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(slider), TRUE, TRUE, 0);
 
  /* add the lightmap slider */
  slider = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 100.0, 2, d->lightmap_weight*100, 2));
  dtgtk_slider_set_label(slider,_("light map weight"));
  dtgtk_slider_set_unit(slider,"%");
  g_signal_connect(G_OBJECT(slider), "value-changed", G_CALLBACK(_lightmap_weight_callback),d);
  g_object_set(G_OBJECT(slider), "tooltip-text", _("set the score weight of light map matching"), (char *)NULL);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(slider), TRUE, TRUE, 0);
  
  /* add the rgbmap weighting slider */
  slider = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 100.0, 2, d->redmap_weight*100, 2));
  dtgtk_slider_set_label(slider,_("color map weight"));
  dtgtk_slider_set_unit(slider,"%");
  g_signal_connect(G_OBJECT(slider), "value-changed", G_CALLBACK(_rgb_weight_callback),d);
  g_object_set(G_OBJECT(slider), "tooltip-text", _("set the score weight of color map matching"), (char *)NULL);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(slider), TRUE, TRUE, 0);
 
  GtkWidget *button = gtk_button_new_with_label(_("view similar"));
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(_button_callback),d);
  g_object_set(G_OBJECT(button), "tooltip-text", _("match images with selected image and views the result"), (char *)NULL);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(button), TRUE, TRUE, 0);
  
}


void gui_cleanup (dt_lib_module_t *self)
{
  free(self->data);
  self->data = NULL;
}

// These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
