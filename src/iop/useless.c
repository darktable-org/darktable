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
// our includes go first:
#include "develop/imageop.h"
#include "dtgtk/slider.h"
#include "gui/gtk.h"

#include <gtk/gtk.h>
#include <stdlib.h>

// this is the version of the modules parameters,
// and includes version information about compile-time dt
DT_MODULE(1)

// TODO: some build system to support dt-less compilation and translation!

typedef struct dt_iop_useless_params_t
{
  // these are stored in db.
  // make sure everything is in here does not
  // depend on temporary memory (pointers etc)
  // stored in self->params and self->default_params
  // also, since this is stored in db, you should keep changes to this struct
  // to a minimum. if you have to change this struct, it will break
  // users data bases, and you should increment the version
  // of DT_MODULE(VERSION) above!
  int checker_scale;
}
dt_iop_useless_params_t;

typedef struct dt_iop_useless_gui_data_t
{
  // whatever you need to make your gui happy.
  // stored in self->gui_data
  GtkDarktableSlider *scale; // this is needed by gui_update
}
dt_iop_useless_gui_data_t;

typedef struct dt_iop_useless_data_t
{
  // this is stored in the pixelpipeline after a commit (not the db),
  // you can do some precomputation and get this data in process().
  // stored in piece->data
  int checker_scale; // in our case, no precomputation or
  // anything is possible, so this is just a copy.
}
dt_iop_useless_data_t;

typedef struct dt_iop_useless_global_data_t
{
  // this is optionally stored in self->global_data
  // and can be used to alloc globally needed stuff
  // which is needed in gui mode and during processing.

  // we don't need it for this example (as for most dt plugins)
}
dt_iop_useless_global_data_t;

// this returns a translatable name
const char *name()
{
  // make sure you put all your translatable strings into _() !
  return _("silly example");
}

// some additional flags (self explanatory i think):
int
flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING;
}

// where does it appear in the gui?
int
groups ()
{
  return IOP_GROUP_BASIC;
}

void init_key_accels()
{
  dtgtk_slider_init_accel(darktable.control->accels_darkroom,"<Darktable>/darkroom/modules/useless/useless");
}
// implement this, if you have esoteric output bytes per pixel. default is 4*float
/*
int
output_bpp(dt_iop_module_t *module, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  if(pipe->type != DT_DEV_PIXELPIPE_PREVIEW && module->dev->image->filters) return sizeof(float);
  else return 4*sizeof(float);
}
*/


/** modify regions of interest (optional, per pixel ops don't need this) */
// void modify_roi_out(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, dt_iop_roi_t *roi_out, const dt_iop_roi_t *roi_in);
// void modify_roi_in(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, const dt_iop_roi_t *roi_out, dt_iop_roi_t *roi_in);

/** process, all real work is done here. */
void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *i, void *o, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  // this is called for preview and full pipe separately, each with its own pixelpipe piece.
  // get our data struct:
  dt_iop_useless_params_t *d = (dt_iop_useless_params_t *)piece->data;
  // the total scale is composed of scale before input to the pipeline (iscale),
  // and the scale of the roi.
  const float scale = piece->iscale/roi_in->scale;
  // how many colors in our buffer?
  const int ch = piece->colors;
  // iterate over all output pixels (same coordinates as input)
#ifdef _OPENMP
  // optional: parallelize it!
  #pragma omp parallel for default(none) schedule(static) shared(i,o,roi_in,roi_out,d)
#endif
  for(int j=0; j<roi_out->height; j++)
  {
    float *in  = ((float *)i) + ch*roi_in->width *j;
    float *out = ((float *)o) + ch*roi_out->width*j;
    for(int i=0; i<roi_out->width; i++)
    {
      // calculate world space coordinates:
      int wi = (roi_in->x + i) * scale, wj = (roi_in->y + j) * scale;
      if((wi/d->checker_scale+wj/d->checker_scale)&1) for(int c=0; c<3; c++) out[c] = 0;
      else                                            for(int c=0; c<3; c++) out[c] = in[c];
      in += ch;
      out += ch;
    }
  }
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
  module->data = NULL; //malloc(sizeof(dt_iop_useless_global_data_t));
  module->params = malloc(sizeof(dt_iop_useless_params_t));
  module->default_params = malloc(sizeof(dt_iop_useless_params_t));
  // our module is disabled by default
  // by default:
  module->default_enabled = 0;
  // order has to be changed by editing the dependencies in tools/iop_dependencies.py
  module->priority = 901; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_useless_params_t);
  module->gui_data = NULL;
  // init defaults:
  dt_iop_useless_params_t tmp = (dt_iop_useless_params_t)
  {
    50
  };

  memcpy(module->params, &tmp, sizeof(dt_iop_useless_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_useless_params_t));
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

/** commit is the synch point between core and gui, so it copies params to pipe data. */
void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *params, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_useless_params_t *p = (dt_iop_useless_params_t *)params;
  dt_iop_useless_data_t *d = (dt_iop_useless_data_t *)piece->data;
  d->checker_scale = p->checker_scale;
}

void init_pipe     (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_useless_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe  (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
}

/** put your local callbacks here, be sure to make them static so they won't be visible outside this file! */
static void
slider_callback(GtkRange *range, dt_iop_module_t *self)
{
  // this is important to avoid cycles!
  if(darktable.gui->reset) return;
  dt_iop_useless_gui_data_t *g = (dt_iop_useless_gui_data_t *)self->gui_data;
  dt_iop_useless_params_t *p = (dt_iop_useless_params_t *)self->params;
  p->checker_scale = dtgtk_slider_get_value(g->scale);
  // let core know of the changes
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

/** gui callbacks, these are needed. */
void gui_update    (dt_iop_module_t *self)
{
  // let gui slider match current parameters:
  dt_iop_useless_gui_data_t *g = (dt_iop_useless_gui_data_t *)self->gui_data;
  dt_iop_useless_params_t *p = (dt_iop_useless_params_t *)self->params;
  dtgtk_slider_set_value(g->scale, p->checker_scale);
}

void gui_init     (dt_iop_module_t *self)
{
  // init the slider (more sophisticated layouts are possible with gtk tables and boxes):
  self->gui_data = malloc(sizeof(dt_iop_useless_gui_data_t));
  dt_iop_useless_gui_data_t *g = (dt_iop_useless_gui_data_t *)self->gui_data;
  g->scale = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_VALUE, 1, 100, 1, 50, 0));
  dtgtk_slider_set_accel(g->scale,darktable.control->accels_darkroom,"<Darktable>/darkroom/modules/useless/useless");
  self->widget = GTK_WIDGET(g->scale);
  g_signal_connect (G_OBJECT (g->scale), "value-changed",
                    G_CALLBACK (slider_callback), self);
}

void gui_cleanup  (dt_iop_module_t *self)
{
  // nothing else necessary, gtk will clean up the slider.
  free(self->gui_data);
  self->gui_data = NULL;
}

/** additional, optional callbacks to capture darkroom center events. */
// void gui_post_expose(dt_iop_module_t *self, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx, int32_t pointery);
// int mouse_moved(dt_iop_module_t *self, double x, double y, int which);
// int button_pressed(dt_iop_module_t *self, double x, double y, int which, int type, uint32_t state);
// int button_released(struct dt_iop_module_t *self, double x, double y, int which, uint32_t state);
// int scrolled(dt_iop_module_t *self, double x, double y, int up, uint32_t state);

// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
