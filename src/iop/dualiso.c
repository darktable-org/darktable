/*
    This file is part of darktable,
    copyright (c) 2014 johannes hanika.

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
#include "bauhaus/bauhaus.h"
#include "gui/gtk.h"

#include <gtk/gtk.h>
#include <stdlib.h>

// this is the version of the modules parameters,
// and includes version information about compile-time dt
DT_MODULE_INTROSPECTION(1, dt_iop_dualiso_params_t)

typedef struct dt_iop_dualiso_params_t
{
  int dual_iso;
}
dt_iop_dualiso_params_t;

typedef struct dt_iop_dualiso_gui_data_t
{
}
dt_iop_dualiso_gui_data_t;

typedef struct dt_iop_dualiso_global_data_t
{
}
dt_iop_dualiso_global_data_t;

const char *name()
{
  return _("dual iso");
}

int
flags()
{
  return 0;
}

// where does it appear in the gui?
int
groups()
{
  return IOP_GROUP_BASIC;
}

int
output_bpp(dt_iop_module_t *module, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  if(!dt_dev_pixelpipe_uses_downsampled_input(pipe) &&
     (pipe->image.flags & DT_IMAGE_RAW)) return sizeof(float);
  else return 4*sizeof(float);
}

// we're not scaling here (bayer input), so just crop borders
void modify_roi_out(
    struct dt_iop_module_t *self,
    struct dt_dev_pixelpipe_iop_t *piece,
    dt_iop_roi_t *roi_out,
    const dt_iop_roi_t *roi_in)
{
  *roi_out = *roi_in;
  roi_out->x = roi_out->y = 0;
  roi_out->width -= piece->pipe->image.black_offset_x;
  roi_out->height -= piece->pipe->image.black_offset_y;
}

void modify_roi_in(
    struct dt_iop_module_t *self,
    struct dt_dev_pixelpipe_iop_t *piece,
    const dt_iop_roi_t *roi_out,
    dt_iop_roi_t *roi_in)
{
  // TODO: double check that shit for zoomed in roi (i.e. roi_out->x > 0)
  *roi_in = *roi_out;
  roi_in->width += piece->pipe->image.black_offset_x;
  roi_in->height += piece->pipe->image.black_offset_y;
}

/** process, all real work is done here. */
void process(
    struct dt_iop_module_t *self,
    dt_dev_pixelpipe_iop_t *piece,
    void *i, void *o,
    const dt_iop_roi_t *roi_in,
    const dt_iop_roi_t *roi_out)
{
  // dt_iop_dualiso_params_t *d = (dt_iop_dualiso_params_t *)piece->data;
  assert((pipe->image.flags & DT_IMAGE_RAW) &&
      !dt_dev_pixelpipe_uses_downsampled_input(pipe));
  const int ox = piece->pipe->image.black_offset_x;
  const int oy = piece->pipe->image.black_offset_y;

  const float black = piece->pipe->image.raw_black_level;
  const float white = piece->pipe->image.raw_white_point;

  // fprintf(stderr, "roi in %d %d %d %d\n", roi_in->x, roi_in->y, roi_in->width, roi_in->height);
  // fprintf(stderr, "roi out %d %d %d %d\n", roi_out->x, roi_out->y, roi_out->width, roi_out->height);
  if(dt_dev_pixelpipe_uses_downsampled_input(piece->pipe))
  { // pre-downsampled buffer that needs black/white scaling
#ifdef _OPENMP
    #pragma omp parallel for default(none) shared(i,o,roi_in,roi_out) schedule(static)
#endif
    for(int j=0; j<roi_out->height; j++)
    {
      const __m128 w = _mm_set1_ps(1.0f/white);
      const __m128 b = _mm_set1_ps(black);
      const __m128 *in  = ((__m128*)i) + ((size_t)roi_in->width*(j+oy) + ox);
      float *out = ((float*)o) + 4*(size_t)roi_out->width*j;

      // process aligned pixels with SSE
      for(int i=0 ; i < roi_out->width; i++,out+=4,in++)
        _mm_stream_ps(out, _mm_max_ps(_mm_setzero_ps(), _mm_mul_ps(_mm_sub_ps(in[0], b), w)));
    }
    _mm_sfence();
  }
  else
  { // raw mosaic
#ifdef _OPENMP
  #pragma omp parallel for default(none) schedule(static) shared(i,o,roi_in,roi_out)
#endif
    for(int j=0; j<roi_out->height; j++)
    {
      uint16_t *in  = ((uint16_t *)i) + ((size_t)roi_in->width*(j+oy) + ox);
      uint16_t *out = ((uint16_t *)o) + (size_t)roi_out->width*j;
      for(int i=0;i<roi_out->width;i++,out++,in++)
        // out[0] = CLAMP(((int32_t)in[0] - black)/white, 0, 0xffff);
        out[0] = CLAMP(((int32_t)in[0], 0, 0xffff);
    }
  }
}

void commit_params(
    struct dt_iop_module_t *self,
    dt_iop_params_t *params,
    dt_dev_pixelpipe_t *pipe,
    dt_dev_pixelpipe_iop_t *piece)
{
  memcpy(piece->data, params, sizeof(dt_iop_dualiso_params_t));
  if(!(pipe->image.flags & DT_IMAGE_RAW))// || dt_dev_pixelpipe_uses_downsampled_input(pipe))
    piece->enabled = 0;
}

/** optional: if this exists, it will be called to init new defaults if a new image is loaded from film strip mode. */
void reload_defaults(dt_iop_module_t *module)
{
  // change default_enabled depending on type of image, or set new default_params even.

  // if this callback exists, it has to write default_params and default_enabled.
}

/** init, cleanup, commit to pipeline */
void init(dt_iop_module_t *module)
{
  // we don't need global data:
  module->data = NULL; //malloc(sizeof(dt_iop_dualiso_global_data_t));
  module->params = malloc(sizeof(dt_iop_dualiso_params_t));
  module->default_params = malloc(sizeof(dt_iop_dualiso_params_t));
  // enabled by default, need to crop borders
  module->default_enabled = 1;
  // order has to be changed by editing the dependencies in tools/iop_dependencies.py
  module->priority = 10; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_dualiso_params_t);
  module->gui_data = NULL;
  // init defaults:
  dt_iop_dualiso_params_t tmp = (dt_iop_dualiso_params_t)
  {
    0
  };

  memcpy(module->params, &tmp, sizeof(dt_iop_dualiso_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_dualiso_params_t));
}

void cleanup(dt_iop_module_t *module)
{
  free(module->gui_data);
  module->gui_data = NULL; // just to be sure
  free(module->params);
  module->params = NULL;
  free(module->data); // just to be sure
  module->data = NULL;
}

/** gui callbacks, these are needed. */
void gui_update(dt_iop_module_t *self)
{
}

void gui_init(dt_iop_module_t *self)
{
  // init the slider (more sophisticated layouts are possible with gtk tables and boxes):
  self->gui_data = malloc(sizeof(dt_iop_dualiso_gui_data_t));
  // dt_iop_dualiso_gui_data_t *g = (dt_iop_dualiso_gui_data_t *)self->gui_data;
  self->widget = gtk_vbox_new(TRUE, DT_BAUHAUS_SPACE);
}

void gui_cleanup(dt_iop_module_t *self)
{
  // nothing else necessary, gtk will clean up the slider.
  free(self->gui_data);
  self->gui_data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
