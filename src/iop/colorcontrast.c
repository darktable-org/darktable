/* -*- Mode: c; c-basic-offset: 2; -*- */
/*
    This file is part of darktable,
    copyright (c) 2009--2010 johannes hanika
    copyright (c) 2011 Sergey Astanin

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
#include "control/control.h"
#include "dtgtk/slider.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include <gtk/gtk.h>
#include <stdlib.h>
#include <assert.h>
#include <xmmintrin.h>

DT_MODULE(1)

typedef struct dt_iop_colorcontrast_params_t
{
  // these are stored in db.
  // make sure everything is in here does not
  // depend on temporary memory (pointers etc)
  // stored in self->params and self->default_params
  // also, since this is stored in db, you should keep changes to this struct
  // to a minimum. if you have to change this struct, it will break
  // users data bases, and you should increment the version
  // of DT_MODULE(VERSION) above!
  float a_steepness;
  float a_offset;
  float b_steepness;
  float b_offset;
}
dt_iop_colorcontrast_params_t;

typedef struct dt_iop_colorcontrast_gui_data_t
{
  // whatever you need to make your gui happy.
  // stored in self->gui_data
  GtkVBox *vbox;
  GtkDarktableSlider *a_scale; // this is needed by gui_update
  GtkDarktableSlider *b_scale;
}
dt_iop_colorcontrast_gui_data_t;

typedef struct dt_iop_colorcontrast_data_t
{
  // this is stored in the pixelpipeline after a commit (not the db),
  // you can do some precomputation and get this data in process().
  // stored in piece->data
  float a_steepness;
  float a_offset;
  float b_steepness;
  float b_offset;
}
dt_iop_colorcontrast_data_t;

typedef struct dt_iop_colorcontrast_global_data_t
{
  // this is optionally stored in self->global_data
  // and can be used to alloc globally needed stuff
  // which is needed in gui mode and during processing.
}
dt_iop_colorcontrast_global_data_t;

// this returns a translatable name
const char *name()
{
  // make sure you put all your translatable strings into _() !
  return _("color contrast");
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
  return IOP_GROUP_COLOR;
}

void init_key_accels(dt_iop_module_so_t *self)
{
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "green vs magenta"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "blue vs yellow"));
}

void connect_key_accels(dt_iop_module_t *self)
{
  dt_iop_colorcontrast_gui_data_t *g =
      (dt_iop_colorcontrast_gui_data_t*)self->gui_data;

  dt_accel_connect_slider_iop(self, "green vs magenta",
                              GTK_WIDGET(g->a_scale));
  dt_accel_connect_slider_iop(self, "blue vs yellow",
                              GTK_WIDGET(g->b_scale));
}

/** modify regions of interest (optional, per pixel ops don't need this) */
// void modify_roi_out(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, dt_iop_roi_t *roi_out, const dt_iop_roi_t *roi_in);
// void modify_roi_in(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, const dt_iop_roi_t *roi_out, dt_iop_roi_t *roi_in);

/** process, all real work is done here. */
void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *i, void *o, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  // this is called for preview and full pipe separately, each with its own pixelpipe piece.
  assert(dt_iop_module_colorspace(self) == iop_cs_Lab);
  // get our data struct:
  dt_iop_colorcontrast_params_t *d = (dt_iop_colorcontrast_params_t *)piece->data;
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

    const __m128 scale = _mm_set_ps(0.0f,d->b_steepness,d->a_steepness,1.0f);
    const __m128 offset = _mm_set_ps(0.0f,d->b_offset,d->a_offset,0.0f);
    const __m128 min = _mm_set_ps(0.0f,-128.0f,-128.0f, -INFINITY);
    const __m128 max = _mm_set_ps(0.0f, 128.0f, 128.0f,  INFINITY);


    for(int i=0; i<roi_out->width; i++)
    {
      _mm_stream_ps(out,_mm_min_ps(max,_mm_max_ps(min,_mm_add_ps(offset,_mm_mul_ps(scale,_mm_load_ps(in))))));
      in+=ch;
      out+=ch;
    }
  }
  _mm_sfence();
}

/** optional: if this exists, it will be called to init new defaults if a new image is loaded from film strip mode. */
void reload_defaults(dt_iop_module_t *module)
{
  // change default_enabled depending on type of image, or set new default_params even.
  // if this callback exists, it has to write default_params and default_enabled.
  dt_iop_colorcontrast_params_t tmp = (dt_iop_colorcontrast_params_t)
  {
    1.0, 0.0, 1.0, 0.0
  };
  memcpy(module->params, &tmp, sizeof(dt_iop_colorcontrast_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_colorcontrast_params_t));
  module->default_enabled = 0;
}

/** init, cleanup, commit to pipeline */
void init(dt_iop_module_t *module)
{
  // we don't need global data:
  module->data = NULL; //malloc(sizeof(dt_iop_colorcontrast_global_data_t));
  module->params = malloc(sizeof(dt_iop_colorcontrast_params_t));
  module->default_params = malloc(sizeof(dt_iop_colorcontrast_params_t));
  // our module is disabled by default
  module->default_enabled = 0;
  // we are pretty late in the pipe:
  module->priority = 784; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_colorcontrast_params_t);
  module->gui_data = NULL;
  // init defaults:
  dt_iop_colorcontrast_params_t tmp = (dt_iop_colorcontrast_params_t)
  {
    1.0, 0.0, 1.0, 0.0
  };
  memcpy(module->params, &tmp, sizeof(dt_iop_colorcontrast_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_colorcontrast_params_t));
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
  dt_iop_colorcontrast_params_t *p = (dt_iop_colorcontrast_params_t *)params;
  dt_iop_colorcontrast_data_t *d = (dt_iop_colorcontrast_data_t *)piece->data;
  d->a_steepness = p->a_steepness;
  d->a_offset = p->a_offset;
  d->b_steepness = p->b_steepness;
  d->b_offset = p->b_offset;
}

void init_pipe     (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_colorcontrast_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe  (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
}

/** put your local callbacks here, be sure to make them static so they won't be visible outside this file! */
static void
a_slider_callback(GtkRange *range, dt_iop_module_t *self)
{
  // this is important to avoid cycles!
  if(darktable.gui->reset) return;
  dt_iop_colorcontrast_gui_data_t *g = (dt_iop_colorcontrast_gui_data_t *)self->gui_data;
  dt_iop_colorcontrast_params_t *p = (dt_iop_colorcontrast_params_t *)self->params;
  p->a_steepness = dtgtk_slider_get_value(g->a_scale);
  // let core know of the changes
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void
b_slider_callback(GtkRange *range, dt_iop_module_t *self)
{
  // this is important to avoid cycles!
  if(darktable.gui->reset) return;
  dt_iop_colorcontrast_gui_data_t *g = (dt_iop_colorcontrast_gui_data_t *)self->gui_data;
  dt_iop_colorcontrast_params_t *p = (dt_iop_colorcontrast_params_t *)self->params;
  p->b_steepness = dtgtk_slider_get_value(g->b_scale);
  // let core know of the changes
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

/** gui callbacks, these are needed. */
void gui_update    (dt_iop_module_t *self)
{
  // let gui slider match current parameters:
  dt_iop_colorcontrast_gui_data_t *g = (dt_iop_colorcontrast_gui_data_t *)self->gui_data;
  dt_iop_colorcontrast_params_t *p = (dt_iop_colorcontrast_params_t *)self->params;
  dtgtk_slider_set_value(g->a_scale, p->a_steepness);
  dtgtk_slider_set_value(g->b_scale, p->b_steepness);
}

void gui_init     (dt_iop_module_t *self)
{
  // init the slider (more sophisticated layouts are possible with gtk tables and boxes):
  self->gui_data = malloc(sizeof(dt_iop_colorcontrast_gui_data_t));
  dt_iop_colorcontrast_gui_data_t *g = (dt_iop_colorcontrast_gui_data_t *)self->gui_data;
  dt_iop_colorcontrast_params_t *p = (dt_iop_colorcontrast_params_t *)self->params;
  g->a_scale = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR, 0.0, 5.0, 0.01, p->a_steepness, 2));
  dtgtk_slider_set_label(g->a_scale,_("green vs magenta"));
  g->b_scale = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR, 0.0, 5.0, 0.01, p->b_steepness, 2));
  dtgtk_slider_set_label(g->b_scale,_("blue vs yellow"));
  
  self->widget = GTK_WIDGET(gtk_hbox_new(FALSE, 0));
  g->vbox = GTK_VBOX(gtk_vbox_new(FALSE, DT_GUI_IOP_MODULE_CONTROL_SPACING));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->vbox), TRUE, TRUE, 5);
  gtk_box_pack_start(GTK_BOX(g->vbox), GTK_WIDGET(g->a_scale), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox), GTK_WIDGET(g->b_scale), TRUE, TRUE, 0);
  g_object_set(G_OBJECT(g->a_scale), "tooltip-text",
	       _("steepness of the a* curve in Lab"), (char *)NULL);
  g_object_set(G_OBJECT(g->b_scale), "tooltip-text",
	       _("steepness of the b* curve in Lab"), (char *)NULL);


  g_signal_connect(G_OBJECT(g->a_scale), "value-changed",
		   G_CALLBACK(a_slider_callback), self);
  g_signal_connect(G_OBJECT(g->b_scale), "value-changed",
		   G_CALLBACK(b_slider_callback), self);
}

void gui_cleanup  (dt_iop_module_t *self)
{
  // nothing else necessary, gtk will clean up the sliders.
  free(self->gui_data);
  self->gui_data = NULL;
}

/** additional, optional callbacks to capture darkroom center events. */
// void gui_post_expose(dt_iop_module_t *self, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx, int32_t pointery);
// int mouse_moved(dt_iop_module_t *self, double x, double y, int which);
// int button_pressed(dt_iop_module_t *self, double x, double y, int which, int type, uint32_t state);
// int button_released(struct dt_iop_module_t *self, double x, double y, int which, uint32_t state);
// int scrolled(dt_iop_module_t *self, double x, double y, int up, uint32_t state);

// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
