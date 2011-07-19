/*
    This file is part of darktable,
    copyright (c) 2011 Rostyslav Pidgornyi

    and the initial plugin `stuck pixels' was
    copyright (c) 2011 bruce guenter

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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "control/control.h"
#include "develop/imageop.h"
#include "dtgtk/slider.h"
#include "dtgtk/resetlabel.h"
#include "gui/gtk.h"
#include <gtk/gtk.h>
#include <stdlib.h>

DT_MODULE(1)

typedef struct dt_iop_hotpixels_params_t
{
  float strength;
  float threshold;
  gboolean markfixed;
  gboolean permissive;
}
dt_iop_hotpixels_params_t;

typedef struct dt_iop_hotpixels_gui_data_t
{
  GtkDarktableSlider *threshold;
  GtkDarktableSlider *strength;
  GtkToggleButton *markfixed;
  GtkToggleButton *permissive;
  GtkLabel *message;
}
dt_iop_hotpixels_gui_data_t;

typedef struct dt_iop_hotpixels_data_t
{
  uint32_t filters;
  float threshold;
  float multiplier;
  gboolean permissive;
  gboolean markfixed;
}
dt_iop_hotpixels_data_t;

const char *name()
{
  return _("hot pixels");
}

int
groups ()
{
  return IOP_GROUP_CORRECT;
}

void init_key_accels()
{
  dtgtk_slider_init_accel(darktable.control->accels_darkroom,"<Darktable>/darkroom/plugins/hotpixels/threshold");
  dtgtk_slider_init_accel(darktable.control->accels_darkroom,"<Darktable>/darkroom/plugins/hotpixels/strength");
}
int
output_bpp(dt_iop_module_t *module, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return sizeof(float);
}

/* Detect hot sensor pixels based on the 4 surrounding sites. Pixels
 * having 3 or 4 (depending on permissive setting) surrounding pixels that 
 * than value*multiplier are considered "hot", and are replaced by the maximum of
 * the neighbour pixels. The permissive variant allows for
 * correcting pairs of hot pixels in adjacent sites. Replacement using
 * the maximum produces fewer artifacts when inadvertently replacing
 * non-hot pixels. */
void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *i, void *o, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_hotpixels_gui_data_t *g = (dt_iop_hotpixels_gui_data_t *)self->gui_data;
  const dt_iop_hotpixels_data_t *data = (dt_iop_hotpixels_data_t *)piece->data;
  const float threshold = data->threshold;
  const float multiplier = data->multiplier;
  const int width = roi_out->width;
  const int widthx2 = width*2;
  const gboolean markfixed = data->markfixed;
  const int min_neighbours = data->permissive ? 3 : 4;

  // The loop should output only a few pixels, so just copy everything first
  memcpy(o, i, roi_out->width*roi_out->height*sizeof(float));

  int fixed = 0;
#ifdef _OPENMP
  #pragma omp parallel for default(none) shared(roi_out, i, o) reduction(+:fixed) schedule(static)
#endif
  for (int row=2; row<roi_out->height-2; row++)
  {
    const float *in = (float*)i + width*row+2;
    float *out = (float*)o + width*row+2;
    for (int col=2; col<width-1; col++, in++, out++)
    {
      float mid= *in * multiplier;
      if (*in > threshold)
      {
        int count=0;
        float maxin=0.0;
        float other;
#define TESTONE(OFFSET)				\
	other=in[OFFSET];			\
	if (mid > other)			\
	{					\
	  count++;				\
	  if (other > maxin) maxin = other;	\
	}
        TESTONE(-2);
        TESTONE(-widthx2);
        TESTONE(+2);
        TESTONE(+widthx2);
#undef TESTONE
        if (count >= min_neighbours)
        {
          *out = maxin;
          fixed++;
          if (markfixed)
          {
            for (int i=-2; i>=-10 && i>=-col; i-=2)
              out[i] = *in;
            for (int i=2; i<=10 && i<width-col; i+=2)
              out[i] = *in;
          }
        }
      }
    }
  }
  if (g != NULL)
  {
    // lock gui thread mutex, if we are not called from gui thread (very likely)
    int needlock = !pthread_equal(pthread_self(),darktable.control->gui_thread);
    if(needlock) gdk_threads_enter();
    char buf[256];
    snprintf(buf, sizeof buf, _("fixed %d pixels"), fixed);
    gtk_label_set_text(g->message, buf);
    if(needlock) gdk_threads_leave();
  }
}

void init(dt_iop_module_t *module)
{
  module->data = NULL;
  module->params = malloc(sizeof(dt_iop_hotpixels_params_t));
  module->default_params = malloc(sizeof(dt_iop_hotpixels_params_t));
  module->default_enabled = 0;
  module->priority = 66; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_hotpixels_params_t);
  module->gui_data = NULL;
  const dt_iop_hotpixels_params_t tmp =
  {
    0.5, 0.01, FALSE, FALSE
  };

  memcpy(module->params, &tmp, sizeof(dt_iop_hotpixels_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_hotpixels_params_t));
}

void cleanup(dt_iop_module_t *module)
{
  free(module->gui_data);
  module->gui_data = NULL;
  free(module->params);
  module->params = NULL;
  free(module->data);
  module->data = NULL;
}

void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *params, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_hotpixels_params_t *p = (dt_iop_hotpixels_params_t *)params;
  dt_iop_hotpixels_data_t *d = (dt_iop_hotpixels_data_t *)piece->data;
  d->filters = dt_image_flipped_filter(self->dev->image);
  d->multiplier = p->strength/2.0;
  d->threshold = p->threshold;
  d->permissive = p->permissive;
  d->markfixed = p->markfixed && (pipe->type != DT_DEV_PIXELPIPE_EXPORT);
  if (!d->filters || pipe->type == DT_DEV_PIXELPIPE_PREVIEW || p->strength == 0.0)
    piece->enabled = 0;
}

void init_pipe     (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_hotpixels_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe  (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
}

static void
strength_callback(GtkRange *range, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_hotpixels_gui_data_t *g = (dt_iop_hotpixels_gui_data_t *)self->gui_data;
  dt_iop_hotpixels_params_t *p = (dt_iop_hotpixels_params_t *)self->params;
  p->strength = dtgtk_slider_get_value(g->strength);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void
threshold_callback(GtkRange *range, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_hotpixels_gui_data_t *g = (dt_iop_hotpixels_gui_data_t *)self->gui_data;
  dt_iop_hotpixels_params_t *p = (dt_iop_hotpixels_params_t *)self->params;
  p->threshold = dtgtk_slider_get_value(g->threshold);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void
markfixed_callback(GtkRange *range, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_hotpixels_gui_data_t *g = (dt_iop_hotpixels_gui_data_t *)self->gui_data;
  dt_iop_hotpixels_params_t *p = (dt_iop_hotpixels_params_t *)self->params;
  p->markfixed = gtk_toggle_button_get_active(g->markfixed);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void
permissive_callback(GtkRange *range, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_hotpixels_gui_data_t *g = (dt_iop_hotpixels_gui_data_t *)self->gui_data;
  dt_iop_hotpixels_params_t *p = (dt_iop_hotpixels_params_t *)self->params;
  p->permissive = gtk_toggle_button_get_active(g->permissive);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

void gui_update    (dt_iop_module_t *self)
{
  dt_iop_hotpixels_gui_data_t *g = (dt_iop_hotpixels_gui_data_t *)self->gui_data;
  dt_iop_hotpixels_params_t *p = (dt_iop_hotpixels_params_t *)self->params;
  dtgtk_slider_set_value(g->strength, p->strength);
  dtgtk_slider_set_value(g->threshold, p->threshold);
  gtk_toggle_button_set_active(g->markfixed, p->markfixed);
  gtk_toggle_button_set_active(g->permissive, p->permissive);
}

void gui_init     (dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_hotpixels_gui_data_t));
  dt_iop_hotpixels_gui_data_t *g = (dt_iop_hotpixels_gui_data_t*)self->gui_data;
  dt_iop_hotpixels_params_t *p = (dt_iop_hotpixels_params_t*)self->params;

  self->widget = GTK_WIDGET(gtk_hbox_new(FALSE, 0));
  GtkVBox *vbox = GTK_VBOX(gtk_vbox_new(FALSE, DT_GUI_IOP_MODULE_CONTROL_SPACING));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(vbox), TRUE, TRUE, 5);

  g->threshold = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR, 0.0, 1.0, 0.005, p->threshold, 4));
  dtgtk_slider_set_label(g->threshold,_("threshold"));
  g_object_set(G_OBJECT(g->threshold), "tooltip-text", _("lower threshold for hot pixel"), NULL);
  dtgtk_slider_set_format_type(DTGTK_SLIDER(g->threshold),DARKTABLE_SLIDER_FORMAT_FLOAT);
  gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(g->threshold), TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT (g->threshold), "value-changed", G_CALLBACK (threshold_callback), self);
  dtgtk_slider_set_accel(g->threshold,darktable.control->accels_darkroom,"<Darktable>/darkroom/plugins/hotpixels/threshold");

  g->strength = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR, 0.0, 1.0, 0.01, p->strength, 4));
  dtgtk_slider_set_label(g->strength,_("strength"));
  g_object_set(G_OBJECT(g->strength), "tooltip-text", _("strength of hot pixel correction"), NULL);
  dtgtk_slider_set_format_type(DTGTK_SLIDER(g->strength),DARKTABLE_SLIDER_FORMAT_FLOAT);
  gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(g->strength), TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT (g->strength), "value-changed", G_CALLBACK (strength_callback), self);
  dtgtk_slider_set_accel(g->strength,darktable.control->accels_darkroom,"<Darktable>/darkroom/plugins/hotpixels/strength");

  g->permissive  = GTK_TOGGLE_BUTTON(gtk_check_button_new_with_label(_("detect by 3 neighbours")));
  gtk_toggle_button_set_active(g->permissive, p->permissive);
  gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(g->permissive), TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(g->permissive), "toggled", G_CALLBACK(permissive_callback), self);

  GtkHBox *hbox1 = GTK_HBOX(gtk_hbox_new(FALSE, 0));

  g->markfixed  = GTK_TOGGLE_BUTTON(gtk_check_button_new_with_label(_("mark fixed pixels")));
  gtk_toggle_button_set_active(g->markfixed, p->markfixed);
  gtk_box_pack_start(GTK_BOX(hbox1), GTK_WIDGET(g->markfixed), TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(g->markfixed), "toggled", G_CALLBACK(markfixed_callback), self);

  g->message = GTK_LABEL(gtk_label_new ("")); // This gets filled in by process
  gtk_box_pack_start(GTK_BOX(hbox1), GTK_WIDGET(g->message), TRUE, TRUE, 0);

  gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(hbox1), TRUE, TRUE, 0);
}

void gui_cleanup  (dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
