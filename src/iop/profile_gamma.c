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
#include "develop/develop.h"
#include "control/control.h"
#include "bauhaus/bauhaus.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/simple_gui.h"

DT_MODULE(1)

const char *name()
{
  return _("unbreak input profile");
}

int
groups ()
{
  return IOP_GROUP_COLOR;
}

int flags ()
{
  return IOP_FLAGS_ONE_INSTANCE;
}

void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *i, void *o, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  float *d = (float *)piece->data;
  const float gamma  = d[0];
  const float linear = d[1];

  float table[0x10000];
  float a, b, c, g;
  if(gamma == 1.0)
  {
    for(int k=0; k<0x10000; k++) table[k] = 1.0*k/0x10000;
  }
  else
  {
    if(linear == 0.0)
    {
      for(int k=0; k<0x10000; k++)
        table[k] = powf(1.00*k/0x10000, gamma);
    }
    else
    {
      if(linear<1.0)
      {
        g = gamma*(1.0-linear)/(1.0-gamma*linear);
        a = 1.0/(1.0+linear*(g-1));
        b = linear*(g-1)*a;
        c = powf(a*linear+b, g)/linear;
      }
      else
      {
        a = b = g = 0.0;
        c = 1.0;
      }
      for(int k=0; k<0x10000; k++)
      {
        float tmp;
        if (k<0x10000*linear) tmp = c*k/0x10000;
        else tmp = powf(a*k/0x10000+b, g);
        table[k] = tmp;
      }
    }
  }

  float *in = (float *)i;
  float *out = (float *)o;
  const int ch = piece->colors;
  for(size_t k=0; k<(size_t)roi_out->width*roi_out->height; k++)
  {
    out[0] = table[CLAMP((int)(in[0]*0x10000ul), 0, 0xffff)];
    out[1] = table[CLAMP((int)(in[1]*0x10000ul), 0, 0xffff)];
    out[2] = table[CLAMP((int)(in[2]*0x10000ul), 0, 0xffff)];
    in += ch;
    out += ch;
  }

  if(piece->pipe->mask_display)
    dt_iop_alpha_copy(i, o, roi_out->width, roi_out->height);
}

void init(dt_iop_module_t *module)
{
  module->priority = 315; // module order created by iop_dependencies.py, do not edit!
}

dt_gui_simple_t* gui_init_simple(dt_iop_module_so_t *self)
{
  static dt_gui_simple_t gui =
  {
    0, // not used currently
    {
      /** linear slider */
      {
        .slider = {
          DT_SIMPLE_GUI_SLIDER,
          "linear",
          N_("linear"),
          N_("linear part"),
          NULL,
          0.0, 1.0, 0.0001, 0.1,
          4,
          NULL,
          NULL
        }
      },
      /** gamma slider */
      {
        .slider = {
          DT_SIMPLE_GUI_SLIDER,
          "gamma",
          N_("gamma"),
          N_("gamma exponential factor"),
          NULL,
          0.0, 1.0, 0.0001, 0.45,
          4,
          NULL,
          NULL
        }
      },
      /** the last element has to be of type DT_SIMPLE_GUI_NONE */
      {.common = {DT_SIMPLE_GUI_NONE, NULL, NULL, NULL}}
    }
  };

  return &gui;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
