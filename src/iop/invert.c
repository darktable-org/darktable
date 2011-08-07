/*
    This file is part of darktable,
    copyright (c) 2011 Tobias Ellinghaus, johannes hanika.

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

const char *name()
{
  return _("invert"); // FIXME: add context to distinguish it from the accel to invert the selection in lighttable mode.
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

static int
FC(const int row, const int col, const unsigned int filters)
{
  return filters >> (((row << 1 & 14) + (col & 1)) << 1) & 3;
}

void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *ivoid, void *ovoid, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  const int filters = dt_image_flipped_filter(self->dev->image);

  if(piece->pipe->type != DT_DEV_PIXELPIPE_PREVIEW && filters && self->dev->image->bpp != 4)
  {
    // doesn't work and isn't used.
    uint16_t min = -1, max = 0, res[3] = {0,0,0};
#ifdef _OPENMP
    #pragma omp parallel for default(none) shared(roi_out, ivoid, ovoid, min, max, res) schedule(static)
#endif
    for(int j=0; j<roi_out->height; j++)
    {
      const uint16_t *in = ((uint16_t*)ivoid) + j*roi_out->width;
      uint16_t *out = ((uint16_t*)ovoid) + j*roi_out->width;
      for(int i=0; i<roi_out->width; i++,out++,in++)
      {
        *out = 65535 - *in;
        res[FC(j+roi_out->x, i+roi_out->y, filters)] = MAX(res[FC(j+roi_out->x, i+roi_out->y, filters)], *out);
        min = MIN(min, *out);
        max = MAX(max, *out);
      }
    }

    for(int k=0; k<3; k++)
      piece->pipe->processed_maximum[k] = res[k];
  }
  else if(piece->pipe->type != DT_DEV_PIXELPIPE_PREVIEW && filters && self->dev->image->bpp == 4)
  {
    // doesn't work and isn't used.
    float min, max;
    min = 650000;
    max = -650000;
#ifdef _OPENMP
    #pragma omp parallel for default(none) shared(roi_out, ivoid, ovoid, min, max) schedule(static)
#endif
    for(int j=0; j<roi_out->height; j++)
    {
      const float *in = ((float *)ivoid) + j*roi_out->width;
      float *out = ((float*)ovoid) + j*roi_out->width;
      for(int i=0; i<roi_out->width; i++,out++,in++)
      {
        min = MIN(min, *in);
        max = MAX(max, *in);
        *out = 1.0 - *in;
      }
    }
  }
  else
  {
    const int ch = piece->colors;
#ifdef _OPENMP
    #pragma omp parallel for default(none) shared(roi_out, ivoid, ovoid) schedule(static)
#endif
    for(int k=0; k<roi_out->height; k++)
    {
      const float *in = ((float*)ivoid) + ch*k*roi_out->width;
      float *out = ((float*)ovoid) + ch*k*roi_out->width;
      for (int j=0; j<roi_out->width; j++,in+=ch,out+=ch)
        for(int c=0; c<3; c++) out[c] = 1.0 - in[c];
    }
  }
}

void reload_defaults(dt_iop_module_t *module)
{
  module->params = NULL;
  module->default_params = NULL;

  // can't be switched on for raw and hdr images:
  if(module->dev->image->flags & DT_IMAGE_RAW) module->hide_enable_button = 1;
  else                                         module->hide_enable_button = 0;
}

void init(dt_iop_module_t *module)
{
  module->data = NULL;
  module->params = NULL;
  module->default_params = NULL;
  module->default_enabled = 0;
  module->priority = 20; // module order created by iop_dependencies.py, do not edit!
  module->params_size = 0;
  module->gui_data = NULL;
}

void cleanup(dt_iop_module_t *module)
{
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *params, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = NULL;
}

void cleanup_pipe  (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
}

void gui_update(dt_iop_module_t *self)
{
  if(self->dev->image->filters)
    gtk_label_set_text(GTK_LABEL(self->widget), _("look at the image. now look back at me.\nthis doesn't work for raw images."));
  else
    gtk_label_set_text(GTK_LABEL(self->widget), _("look at the image. now look back at me."));

}

void gui_init(dt_iop_module_t *self)
{
  self->gui_data = NULL;

  self->widget = gtk_label_new("");
  gtk_misc_set_alignment(GTK_MISC(self->widget), 0.0, 0.5);
}

void gui_cleanup  (dt_iop_module_t *self)
{
  self->gui_data = NULL;
}

// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
