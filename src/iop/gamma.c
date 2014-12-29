/*
    This file is part of darktable,
    copyright (c) 2009--2010 johannes hanika.

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
#include <stdlib.h>
#include <math.h>
#include <assert.h>
#include <string.h>
#ifdef HAVE_GEGL
#include <gegl.h>
#endif
#include "develop/develop.h"
#include "control/control.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"

DT_MODULE_INTROSPECTION(1, dt_iop_gamma_params_t)


typedef struct dt_iop_gamma_params_t
{
  float gamma, linear;
} dt_iop_gamma_params_t;


typedef struct dt_iop_gamma_data_t
{
  uint8_t table[0x10000];
} dt_iop_gamma_data_t;


const char *name()
{
  return C_("modulename", "gamma");
}

int groups()
{
  return IOP_GROUP_COLOR;
}

int flags()
{
  return IOP_FLAGS_HIDDEN | IOP_FLAGS_ONE_INSTANCE;
}

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *i, void *o,
             const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_gamma_data_t *d = (dt_iop_gamma_data_t *)piece->data;
  const int ch = piece->colors;

  if(piece->pipe->mask_display)
  {
    const float yellow[3] = { 1.0f, 1.0f, 0.0f };
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(roi_out, o, i, d) schedule(static)
#endif
    for(int k = 0; k < roi_out->height; k++)
    {
      const float *in = ((float *)i) + (size_t)ch * k * roi_out->width;
      uint8_t *out = ((uint8_t *)o) + (size_t)ch * k * roi_out->width;
      for(int j = 0; j < roi_out->width; j++, in += ch, out += ch)
      {
        float gray = 0.3f * in[0] + 0.59f * in[1] + 0.11f * in[2];
        float alpha = in[3];
        for(int c = 0; c < 3; c++)
        {
          float value = gray * (1.0f - alpha) + yellow[c] * alpha;
          out[2 - c] = d->table[(uint16_t)CLAMP((int)(0xfffful * value), 0, 0xffff)];
        }
      }
    }
  }
  else
  {
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(roi_out, o, i, d) schedule(static)
#endif
    for(int k = 0; k < roi_out->height; k++)
    {
      const float *in = ((float *)i) + (size_t)ch * k * roi_out->width;
      uint8_t *out = ((uint8_t *)o) + (size_t)ch * k * roi_out->width;
      for(int j = 0; j < roi_out->width; j++, in += ch, out += ch)
      {
        for(int c = 0; c < 3; c++) out[2 - c] = d->table[(uint16_t)CLAMP((int)(0xfffful * in[c]), 0, 0xffff)];
      }
    }
  }
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_gamma_params_t *p = (dt_iop_gamma_params_t *)p1;
#ifdef HAVE_GEGL
  // pull in new params to gegl
  gegl_node_set(piece->input, "linear_value", p->linear, "gamma_value", p->gamma, NULL);
// gegl_node_set(piece->input, "value", p->gamma, NULL);
#else
  // build gamma table in pipeline piece from committed params:
  dt_iop_gamma_data_t *d = (dt_iop_gamma_data_t *)piece->data;
  float a, b, c, g;
  if(p->linear < 1.0)
  {
    g = p->gamma * (1.0 - p->linear) / (1.0 - p->gamma * p->linear);
    a = 1.0 / (1.0 + p->linear * (g - 1));
    b = p->linear * (g - 1) * a;
    c = powf(a * p->linear + b, g) / p->linear;
  }
  else
  {
    a = b = g = 0.0;
    c = 1.0;
  }
  for(int k = 0; k < 0x10000; k++)
  {
    int32_t tmp;
    if(k < 0x10000 * p->linear)
      tmp = MIN(c * k, 0xFFFF);
    else
    {
      const float _t = powf(a * k / 0x10000 + b, g) * 0x10000;
      tmp = MIN(_t, 0xFFFF);
    }
    d->table[k] = tmp >> 8;
  }
#endif
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
#ifdef HAVE_GEGL
  // create part of the gegl pipeline
  piece->data = NULL;
  dt_iop_gamma_params_t *default_params = (dt_iop_gamma_params_t *)self->default_params;
  piece->input = piece->output
      = gegl_node_new_child(pipe->gegl, "operation", "gegl:dt-gamma", "linear_value", default_params->linear,
                            "gamma_value", default_params->gamma, NULL);
// piece->input = piece->output = gegl_node_new_child(pipe->gegl, "operation", "gegl:gamma", "value",
// default_params->gamma, NULL);
#else
  piece->data = malloc(sizeof(dt_iop_gamma_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
#endif
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
#ifdef HAVE_GEGL
  // clean up everything again.
  (void)gegl_node_remove_child(pipe->gegl, piece->input);
// no free necessary, no data is alloc'ed
#else
  free(piece->data);
  piece->data = NULL;
#endif
}

void init(dt_iop_module_t *module)
{
  // module->data = malloc(sizeof(dt_iop_gamma_data_t));
  module->params = malloc(sizeof(dt_iop_gamma_params_t));
  module->default_params = malloc(sizeof(dt_iop_gamma_params_t));
  module->params_size = sizeof(dt_iop_gamma_params_t);
  module->gui_data = NULL;
  module->priority = 1000; // module order created by iop_dependencies.py, do not edit!
  module->hide_enable_button = 1;
  module->default_enabled = 1;
  dt_iop_gamma_params_t tmp = (dt_iop_gamma_params_t){ 1.0, 1.0 };
  memcpy(module->params, &tmp, sizeof(dt_iop_gamma_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_gamma_params_t));
}

void cleanup(dt_iop_module_t *module)
{
  free(module->gui_data);
  module->gui_data = NULL;
  free(module->params);
  module->params = NULL;
}


// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
