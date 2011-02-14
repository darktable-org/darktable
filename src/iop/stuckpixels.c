/*
    This file is part of darktable,
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
#include "develop/imageop.h"
#include "dtgtk/slider.h"
#include "dtgtk/resetlabel.h"
#include "gui/gtk.h"
#include <gtk/gtk.h>
#include <stdlib.h>

DT_MODULE(1)

typedef struct dt_iop_stuckpixels_params_t
{
  float strength;
  gboolean markfixed;
}
dt_iop_stuckpixels_params_t;

typedef struct dt_iop_stuckpixels_gui_data_t
{
  GtkDarktableSlider *strength;
  GtkToggleButton *markfixed;
  GtkLabel *message;
}
dt_iop_stuckpixels_gui_data_t;

typedef struct dt_iop_stuckpixels_data_t
{
  uint32_t filters;
  float threshold;
  gboolean markfixed;
}
dt_iop_stuckpixels_data_t;

const char *name()
{
  return _("stuck pixels");
}

int
groups ()
{
  return IOP_GROUP_BASIC;
}

int
output_bpp(dt_iop_module_t *module, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return sizeof(float);
}

/* Detect stuck sensor pixels based on the 4 surrounding sites. Pixels
 * having 3 or 4 surrounding pixels that are more than a threshold
 * smaller are considered "stuck", and are replaced by the maximum of
 * the smaller pixels. The test for 3 or 4 smaller pixels allows for
 * correcting pairs of hot pixels in adjacent sites. Replacement using
 * the maximum produces fewer artifacts when inadvertently replacing
 * non-stuck pixels. */
void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *i, void *o, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_stuckpixels_gui_data_t *g = (dt_iop_stuckpixels_gui_data_t *)self->gui_data;
  const dt_iop_stuckpixels_data_t *data = (dt_iop_stuckpixels_data_t *)piece->data;
  const float threshold = data->threshold;
  const int width = roi_out->width;
  const int widthx2 = width*2;
  const gboolean markfixed = data->markfixed;

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
      float mid=*in-threshold;
      if (mid > 0)
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
	if (count >= 3)
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
    char buf[256];
    snprintf(buf, sizeof buf, _("fixed %d pixels"), fixed);
    gtk_label_set_text(g->message, buf);
  }
}

void init(dt_iop_module_t *module)
{
  module->data = NULL;
  module->params = malloc(sizeof(dt_iop_stuckpixels_params_t));
  module->default_params = malloc(sizeof(dt_iop_stuckpixels_params_t));
  module->default_enabled = 0;
  module->priority = 160;
  module->params_size = sizeof(dt_iop_stuckpixels_params_t);
  module->gui_data = NULL;
  const dt_iop_stuckpixels_params_t tmp =
  {
    0.5, FALSE
  };

  memcpy(module->params, &tmp, sizeof(dt_iop_stuckpixels_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_stuckpixels_params_t));
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
  dt_iop_stuckpixels_params_t *p = (dt_iop_stuckpixels_params_t *)params;
  dt_iop_stuckpixels_data_t *d = (dt_iop_stuckpixels_data_t *)piece->data;
  d->filters = dt_image_flipped_filter(self->dev->image);
  d->threshold = 1.0/(p->strength+1.0);
  d->markfixed = p->markfixed && (pipe->type != DT_DEV_PIXELPIPE_EXPORT);
  if (!d->filters || pipe->type == DT_DEV_PIXELPIPE_PREVIEW || p->strength == 0.0)
    piece->enabled = 0;
}

void init_pipe     (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_stuckpixels_data_t));
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
  dt_iop_stuckpixels_gui_data_t *g = (dt_iop_stuckpixels_gui_data_t *)self->gui_data;
  dt_iop_stuckpixels_params_t *p = (dt_iop_stuckpixels_params_t *)self->params;
  p->strength = dtgtk_slider_get_value(g->strength);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void
markfixed_callback(GtkRange *range, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_stuckpixels_gui_data_t *g = (dt_iop_stuckpixels_gui_data_t *)self->gui_data;
  dt_iop_stuckpixels_params_t *p = (dt_iop_stuckpixels_params_t *)self->params;
  p->markfixed = gtk_toggle_button_get_active(g->markfixed);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

void gui_update    (dt_iop_module_t *self)
{
  dt_iop_stuckpixels_gui_data_t *g = (dt_iop_stuckpixels_gui_data_t *)self->gui_data;
  dt_iop_stuckpixels_params_t *p = (dt_iop_stuckpixels_params_t *)self->params;
  dtgtk_slider_set_value(g->strength, p->strength);
  gtk_toggle_button_set_active(g->markfixed, p->markfixed);
}

void gui_init     (dt_iop_module_t *self)
{
  GtkWidget *widget;

  self->gui_data = malloc(sizeof(dt_iop_stuckpixels_gui_data_t));
  dt_iop_stuckpixels_gui_data_t *g = (dt_iop_stuckpixels_gui_data_t*)self->gui_data;
  dt_iop_stuckpixels_params_t *p = (dt_iop_stuckpixels_params_t*)self->params;

  self->widget = GTK_WIDGET(gtk_hbox_new(FALSE, 0));
  GtkVBox *vbox1 = GTK_VBOX(gtk_vbox_new(FALSE, DT_GUI_IOP_MODULE_CONTROL_SPACING));
  GtkVBox *vbox2 = GTK_VBOX(gtk_vbox_new(FALSE, DT_GUI_IOP_MODULE_CONTROL_SPACING));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(vbox1), FALSE, FALSE, 5);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(vbox2), TRUE, TRUE, 5);

  widget = dtgtk_reset_label_new (_("strength"), self, &p->strength, sizeof p->strength);
  gtk_box_pack_start(GTK_BOX(vbox1), GTK_WIDGET(widget), TRUE, TRUE, 0);

  g->strength = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR, 0.0, 10.0, 0.01, p->strength, 4));
  gtk_box_pack_start(GTK_BOX(vbox2), GTK_WIDGET(g->strength), TRUE, TRUE, 0);
  gtk_object_set(GTK_OBJECT(g->strength), "tooltip-text", _("strength of stuck pixel correction threshold"), NULL);
  dtgtk_slider_set_format_type(DTGTK_SLIDER(g->strength),DARKTABLE_SLIDER_FORMAT_FLOAT);
  g_signal_connect(G_OBJECT (g->strength), "value-changed", G_CALLBACK (strength_callback), self);

  widget = gtk_label_new ("");	// Need a spacer so the labels align properly
  gtk_box_pack_start(GTK_BOX(vbox1), GTK_WIDGET(widget), TRUE, TRUE, 0);

  g->markfixed  = GTK_TOGGLE_BUTTON(gtk_check_button_new_with_label(_("mark fixed pixels")));
  gtk_toggle_button_set_active(g->markfixed, p->markfixed);
  gtk_box_pack_start(GTK_BOX(vbox2), GTK_WIDGET(g->markfixed), TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(g->markfixed), "toggled", G_CALLBACK(markfixed_callback), self);

  widget = gtk_label_new (""); // Need a spacer so the labels align properly
  gtk_box_pack_start(GTK_BOX(vbox1), GTK_WIDGET(widget), TRUE, TRUE, 0);
  g->message = GTK_LABEL(gtk_label_new ("")); // This gets filled in by process
  gtk_box_pack_start(GTK_BOX(vbox2), GTK_WIDGET(g->message), TRUE, TRUE, 0);
}

void gui_cleanup  (dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}
