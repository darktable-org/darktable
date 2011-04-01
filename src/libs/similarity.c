/*
    This file is part of darktable,
    copyright (c) 2011 johannes hanika.

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
  return _("similar image(s)");
}

uint32_t views()
{
  return DT_LIGHTTABLE_VIEW;
}

void gui_reset (dt_lib_module_t *self)
{
}

int position ()
{
  return 850;
}


static void _histogram_weight_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_similarity_t *data = ( dt_similarity_t *)user_data;
  data->histogram_weight = dtgtk_slider_get_value(slider);
  dt_control_match_similar(data);
}

static void _lightmap_weight_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_similarity_t *data = ( dt_similarity_t *)user_data;
  data->lightmap_weight = dtgtk_slider_get_value(slider);
  dt_control_match_similar(data);
}

void gui_init (dt_lib_module_t *self)
{
  dt_similarity_t *data = g_malloc(sizeof(dt_similarity_t));
  self->data = data;

  self->widget = gtk_vbox_new(TRUE, 5);

  GtkDarktableSlider *slider = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 100.0, 2, 100.0, 2));
  dtgtk_slider_set_label(slider,_("histogram score weight"));
  dtgtk_slider_set_unit(slider,"%");
  g_signal_connect(G_OBJECT(slider), "value-changed", G_CALLBACK(_histogram_weight_callback),data);
  g_object_set(G_OBJECT(slider), "tooltip-text", _("set the score weight of histogram matching"), (char *)NULL);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(slider), TRUE, TRUE, 0);
 
  slider = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 100.0, 2, 100.0, 2));
  dtgtk_slider_set_label(slider,_("lightness weight"));
  dtgtk_slider_set_unit(slider,"%");
  g_signal_connect(G_OBJECT(slider), "value-changed", G_CALLBACK(_lightmap_weight_callback),data);
  g_object_set(G_OBJECT(slider), "tooltip-text", _("set the score weight of lightness matching"), (char *)NULL);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(slider), TRUE, TRUE, 0);
 
}

void gui_cleanup (dt_lib_module_t *self)
{
  free(self->data);
  self->data = NULL;
}

