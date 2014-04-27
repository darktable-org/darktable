/*
    This file is part of darktable,
    copyright (c) 2009--2010 johannes hanika.
    copyright (c) 2014 LebedevRI.

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
#include <xmmintrin.h>
#include "common/opencl.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "control/control.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "dtgtk/resetlabel.h"
#include "dtgtk/slider.h"
#include "bauhaus/bauhaus.h"
#include "develop/pixelpipe.h"
#include "common/histogram.h"

#define exposure2white(x)	exp2f(-(x))
#define white2exposure(x)	-dt_log2f(fmaxf(0.001, x))

DT_MODULE_INTROSPECTION(3, dt_iop_exposure_params_t)

typedef struct dt_iop_exposure_params_t
{
  float black, exposure;
  gboolean deflicker;
  float deflicker_percentile, deflicker_level;
}
dt_iop_exposure_params_t;

typedef struct dt_iop_exposure_gui_data_t
{
  GtkVBox *vbox1, *vbox2;
  GtkCheckButton *autoexp;
  GtkWidget* black;
  GtkWidget* exposure;
  GtkWidget* autoexpp;
  GtkCheckButton *deflicker;
  GtkWidget *deflicker_percentile;
  GtkWidget *deflicker_level;
}
dt_iop_exposure_gui_data_t;

typedef struct dt_iop_exposure_data_t
{
  float black, exposure;
  gboolean deflicker;
  float deflicker_percentile, deflicker_level;
}
dt_iop_exposure_data_t;

typedef struct dt_iop_exposure_global_data_t
{
  int kernel_exposure;
}
dt_iop_exposure_global_data_t;


const char *name()
{
  return _("exposure");
}

int
groups ()
{
  return IOP_GROUP_BASIC;
}

int
flags ()
{
  return IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_SUPPORTS_BLENDING;
}

void init_key_accels(dt_iop_module_so_t *self)
{
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "black"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "exposure"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "auto-exposure"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "deflicker"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "deflicker-percentile"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "deflicker-level"));
}

void connect_key_accels(dt_iop_module_t *self)
{
  dt_iop_exposure_gui_data_t* g = (dt_iop_exposure_gui_data_t*)self->gui_data;

  dt_accel_connect_slider_iop(self, "black", GTK_WIDGET(g->black));
  dt_accel_connect_slider_iop(self, "exposure", GTK_WIDGET(g->exposure));
  dt_accel_connect_slider_iop(self, "auto-exposure", GTK_WIDGET(g->autoexpp));
  dt_accel_connect_slider_iop(self, "deflicker", GTK_WIDGET(g->deflicker));
  dt_accel_connect_slider_iop(self, "deflicker-percentile", GTK_WIDGET(g->deflicker_percentile));
  dt_accel_connect_slider_iop(self, "deflicker-level", GTK_WIDGET(g->deflicker_level));
}

int
output_bpp(dt_iop_module_t *module, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return 4*sizeof(float);
}

int
legacy_params (dt_iop_module_t *self, const void *const old_params, const int old_version, void *new_params, const int new_version)
{
  if(old_version == 2 && new_version == 3)
  {
    typedef struct dt_iop_exposure_params_v2_t
    {
      float black, exposure, gain;
    }
    dt_iop_exposure_params_v2_t;

    dt_iop_exposure_params_v2_t *o = (dt_iop_exposure_params_v2_t *)old_params;
    dt_iop_exposure_params_t *n = (dt_iop_exposure_params_t *)new_params;
    dt_iop_exposure_params_t *d = (dt_iop_exposure_params_t *)self->default_params;

    *n = *d;  // start with a fresh copy of default parameters

    n->black = o->black;
    n->exposure = o->exposure;
    return 0;
  }
  return 1;
}

void init_presets (dt_iop_module_so_t *self)
{
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "begin", NULL, NULL, NULL);

  dt_gui_presets_add_generic(_("magic lantern defaults"), self->op, self->version(), &(dt_iop_exposure_params_t)
  {
    0., 0., TRUE, 50., -4.
  } , sizeof(dt_iop_exposure_params_t), 1);
  dt_gui_presets_add_generic(_("almost no clipping"), self->op, self->version(), &(dt_iop_exposure_params_t)
  {
    0., 0., TRUE, 100., -1.
  } , sizeof(dt_iop_exposure_params_t), 1);

  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "commit", NULL, NULL, NULL);
}

#ifdef HAVE_OPENCL
int
process_cl (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_exposure_data_t *d = (dt_iop_exposure_data_t *)piece->data;
  dt_iop_exposure_global_data_t *gd = (dt_iop_exposure_global_data_t *)self->data;

  cl_int err = -999;
  const float black = d->black;
  const float white = exposure2white(d->exposure);
  const float scale = 1.0/(white - black);
  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;

  size_t sizes[] = { ROUNDUPWD(width), ROUNDUPHT(height), 1};
  dt_opencl_set_kernel_arg(devid, gd->kernel_exposure, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_exposure, 1, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(devid, gd->kernel_exposure, 2, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_exposure, 3, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_exposure, 4, sizeof(float), (void *)&black);
  dt_opencl_set_kernel_arg(devid, gd->kernel_exposure, 5, sizeof(float), (void *)&scale);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_exposure, sizes);
  if(err != CL_SUCCESS) goto error;
  for(int k=0; k<3; k++) piece->pipe->processed_maximum[k] *= scale;
  return TRUE;

error:
  dt_print(DT_DEBUG_OPENCL, "[opencl_exposure] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif

/* input: 0 - 16384 (valid range: from black level to white level) */
/* output: -14 ... 0 */
static float raw_to_ev(float raw, float black_level, float white_level)
{
    float raw_max = white_level - black_level;
    float raw_ev = -log2f(raw_max) + log2f(CLAMP(raw, 0.0f, 16384.0f));

    return raw_ev;
}

static int compute_correction(dt_iop_module_t *self, float *correction)
{
  dt_iop_exposure_params_t *p = (dt_iop_exposure_params_t *)self->params;

  if(self->histogram == NULL) return 1;

  float total = 0;
  for(int i=0; i < self->histogram_params.bins_count; i++)
  {
    total += self->histogram[4*i];
    total += self->histogram[4*i+1];
    total += self->histogram[4*i+2];
  }

  float thr = (total * p->deflicker_percentile / 100) - 2; // 50% => median; allow up to 2 stuck pixels
  float n = 0;
  float raw = -1;

  for(int i=0; i < self->histogram_params.bins_count; i++)
  {
    n += self->histogram[4*i];
    n += self->histogram[4*i+1];
    n += self->histogram[4*i+2];

    if (n >= thr)
    {
      raw = i;
      break;
    }
  }

  float ev = raw_to_ev(raw, self->dev->image_storage.raw_black_level + p->black, self->dev->image_storage.raw_white_point);

  *correction = p->deflicker_level - ev;

  return 0;
}

void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *i, void *o, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_exposure_data_t *d = (dt_iop_exposure_data_t *)piece->data;
  const float black = d->black;
  const float white = exposure2white(d->exposure);
  const int ch = piece->colors;
  const float scale = 1.0/(white - black);
  const __m128 blackv = _mm_set1_ps(black);
  const __m128 scalev = _mm_set1_ps(scale);
#ifdef _OPENMP
  #pragma omp parallel for default(none) shared(roi_out,i,o) schedule(static)
#endif
  for(int k=0; k<roi_out->height; k++)
  {
    const float *in = ((float *)i) + (size_t)ch*k*roi_out->width;
    float *out = ((float *)o) + (size_t)ch*k*roi_out->width;
    for (int j=0; j<roi_out->width; j++,in+=4,out+=4)
      _mm_store_ps(out, (_mm_load_ps(in)-blackv)*scalev);
  }

  if(piece->pipe->mask_display)
    dt_iop_alpha_copy(i, o, roi_out->width, roi_out->height);

  for(int k=0; k<3; k++) piece->pipe->processed_maximum[k] *= scale;
}

void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_exposure_params_t *p = (dt_iop_exposure_params_t *)p1;
  dt_iop_exposure_data_t *d = (dt_iop_exposure_data_t *)piece->data;
  d->black = p->black;
  d->exposure = p->exposure;
  d->deflicker = p->deflicker;
  d->deflicker_percentile = p->deflicker_percentile;
  d->deflicker_level = p->deflicker_level;
}

void init_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_exposure_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_exposure_gui_data_t *g = (dt_iop_exposure_gui_data_t *)self->gui_data;
  dt_iop_exposure_params_t *p = (dt_iop_exposure_params_t *)self->params;

  dt_bauhaus_slider_set(g->black, p->black);
  dt_bauhaus_slider_set_soft(g->exposure, p->exposure);

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->autoexp), FALSE);
  dt_bauhaus_slider_set(g->autoexpp, 0.01);
  gtk_widget_set_sensitive(GTK_WIDGET(g->autoexpp), FALSE);

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->deflicker), p->deflicker);
  dt_bauhaus_slider_set(g->deflicker_percentile, p->deflicker_percentile);
  gtk_widget_set_sensitive(GTK_WIDGET(g->deflicker_percentile), p->deflicker);
  dt_bauhaus_slider_set(g->deflicker_level, p->deflicker_level);
  gtk_widget_set_sensitive(GTK_WIDGET(g->deflicker_level), p->deflicker);

  self->request_color_pick = 0;
}

void gui_focus(struct dt_iop_module_t *self, gboolean in)
{
  // switch off auto exposure when we lose focus (switching images etc)
  dt_iop_exposure_gui_data_t *g = (dt_iop_exposure_gui_data_t *)self->gui_data;

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->autoexp), FALSE);
  dt_bauhaus_slider_set(g->autoexpp, 0.01);
  gtk_widget_set_sensitive(GTK_WIDGET(g->autoexpp), FALSE);
}

void init(dt_iop_module_t *module)
{
  module->params = malloc(sizeof(dt_iop_exposure_params_t));
  module->default_params = malloc(sizeof(dt_iop_exposure_params_t));
  module->default_enabled = 0;
  module->request_histogram |=  (DT_REQUEST_ON); //FIXME: only when deflicker is enabled maybe?
  module->histogram_params.bins_count = 16384; // we neeed really maximally reliable histogrem
  module->priority = 175; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_exposure_params_t);
  module->gui_data = NULL;
}

void reload_defaults(dt_iop_module_t *module)
{
  dt_iop_exposure_params_t tmp = (dt_iop_exposure_params_t)
  {
    0., 0., FALSE, 100., -1.
  };

  tmp.black = 0.0f;
  tmp.exposure = 0.0f;
  tmp.deflicker = FALSE;
  tmp.deflicker_percentile = 100.0f;
  tmp.deflicker_level = -1.0f;

  memcpy(module->params, &tmp, sizeof(dt_iop_exposure_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_exposure_params_t));
}

void init_global(dt_iop_module_so_t *module)
{
  const int program = 2; // from programs.conf: basic.cl
  dt_iop_exposure_global_data_t *gd = (dt_iop_exposure_global_data_t *)malloc(sizeof(dt_iop_exposure_global_data_t));
  module->data = gd;
  gd->kernel_exposure = dt_opencl_create_kernel(program, "exposure");
}

void cleanup(dt_iop_module_t *module)
{
  free(module->gui_data);
  module->gui_data = NULL;
  free(module->params);
  module->params = NULL;
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_exposure_global_data_t *gd = (dt_iop_exposure_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_exposure);
  free(module->data);
  module->data = NULL;
}

static void exposure_set_black(struct dt_iop_module_t *self, const float black);

static void
autoexp_disable(dt_iop_module_t *self)
{
  if (self->request_color_pick <= 0) return;

  dt_iop_exposure_gui_data_t *g = (dt_iop_exposure_gui_data_t *)self->gui_data;

  gulong signal_id = g_signal_lookup("toggled", GTK_TYPE_CHECK_BUTTON);
  gulong handler_id = g_signal_handler_find(G_OBJECT(g->autoexp),
                                            G_SIGNAL_MATCH_ID,
                                            signal_id,
                                            0, NULL, NULL, NULL);

  g_signal_handler_block(G_OBJECT (g->autoexp), handler_id);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->autoexp), FALSE);
  g_signal_handler_unblock(G_OBJECT (g->autoexp), handler_id);

  gtk_widget_set_sensitive(GTK_WIDGET(g->autoexpp), FALSE);

  self->request_color_pick = 0;
}

static void
deflicker_disable(dt_iop_module_t *self)
{
  dt_iop_exposure_gui_data_t *g = (dt_iop_exposure_gui_data_t *)self->gui_data;
  dt_iop_exposure_params_t *p = (dt_iop_exposure_params_t *)self->params;

  gulong signal_id = g_signal_lookup("toggled", GTK_TYPE_CHECK_BUTTON);
  gulong handler_id = g_signal_handler_find(G_OBJECT(g->deflicker),
                                      G_SIGNAL_MATCH_ID,
                                      signal_id,
                                      0, NULL, NULL, NULL);

  g_signal_handler_block(G_OBJECT (g->deflicker), handler_id);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->deflicker), FALSE);
  g_signal_handler_unblock(G_OBJECT (g->deflicker), handler_id);

  gtk_widget_set_sensitive(GTK_WIDGET(g->deflicker_percentile), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(g->deflicker_level), FALSE);

  p->deflicker = FALSE;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void exposure_set_white(struct dt_iop_module_t *self, const float white)
{
  dt_iop_exposure_params_t *p = (dt_iop_exposure_params_t *)self->params;

  float exposure = white2exposure(white);
  if (p->exposure == exposure) return;

  p->exposure = exposure;
  if (p->black >= white) exposure_set_black(self, white-0.01);

  dt_iop_exposure_gui_data_t *g = (dt_iop_exposure_gui_data_t *)self->gui_data;

  darktable.gui->reset = 1;
  dt_bauhaus_slider_set_soft(g->exposure, p->exposure);
  darktable.gui->reset = 0;
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void dt_iop_exposure_set_white(struct dt_iop_module_t *self, const float white)
{
  autoexp_disable(self);
  deflicker_disable(self);
  exposure_set_white(self, white);
}

static float dt_iop_exposure_get_white(struct dt_iop_module_t *self)
{
  dt_iop_exposure_params_t *p = (dt_iop_exposure_params_t *)self->params;
  return exposure2white(p->exposure);
}

static void exposure_set_black(struct dt_iop_module_t *self, const float black)
{
  dt_iop_exposure_params_t *p = (dt_iop_exposure_params_t *)self->params;

  float b = black;
  if (p->black == b) return;

  p->black = b;
  if (p->black >= exposure2white(p->exposure))
  {
    exposure_set_white(self, p->black+0.01);
  }

  dt_iop_exposure_gui_data_t *g = (dt_iop_exposure_gui_data_t *)self->gui_data;
  darktable.gui->reset = 1;
  dt_bauhaus_slider_set(g->black, p->black);
  darktable.gui->reset = 0;
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void dt_iop_exposure_set_black(struct dt_iop_module_t *self, const float black)
{
  autoexp_disable(self);
  exposure_set_black(self, black);
}

static float dt_iop_exposure_get_black(struct dt_iop_module_t *self)
{
  dt_iop_exposure_params_t *p = (dt_iop_exposure_params_t *)self->params;
  return p->black;
}

static void
autoexp_callback (GtkToggleButton *button, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_exposure_gui_data_t *g = (dt_iop_exposure_gui_data_t *)self->gui_data;
  if(self->dt->gui->reset) return;

  deflicker_disable(self);

  self->request_color_pick = gtk_toggle_button_get_active(button);

  dt_iop_request_focus(self);

  if (self->request_color_pick > 0)
  {
    dt_lib_colorpicker_set_area(darktable.lib, 0.99);
    dt_dev_reprocess_all(self->dev);
  }
  else
    dt_control_queue_redraw();

  gtk_widget_set_sensitive(GTK_WIDGET(g->autoexpp), gtk_toggle_button_get_active(button));

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void
autoexpp_callback (GtkWidget* slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  if(self->request_color_pick <= 0 || self->picked_color_max[0] < 0.0f) return;

  dt_iop_exposure_gui_data_t *g = (dt_iop_exposure_gui_data_t *)self->gui_data;
  const float white = fmaxf(fmaxf(self->picked_color_max[0], self->picked_color_max[1]), self->picked_color_max[2])
                      * (1.0-dt_bauhaus_slider_get(g->autoexpp));
  exposure_set_white(self, white);
}

static void
deflicker_process (dt_iop_module_t *self)
{
  if(!(self->dev->image_storage.flags & DT_IMAGE_RAW)) return;

  dt_iop_exposure_params_t *p = (dt_iop_exposure_params_t *)self->params;
  float correction;

  if(p->deflicker && !compute_correction(self, &correction))
    exposure_set_white(self, exposure2white(correction));
}

static void
deflicker_params_callback (GtkWidget* slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;

  if(!(self->dev->image_storage.flags & DT_IMAGE_RAW)) return;

  dt_iop_exposure_gui_data_t *g = (dt_iop_exposure_gui_data_t *)self->gui_data;
  dt_iop_exposure_params_t *p = (dt_iop_exposure_params_t *)self->params;

  p->deflicker = TRUE;
  p->deflicker_percentile = dt_bauhaus_slider_get(g->deflicker_percentile);
  p->deflicker_level = dt_bauhaus_slider_get(g->deflicker_level);

  gtk_widget_set_sensitive(GTK_WIDGET(g->deflicker_percentile), p->deflicker);
  gtk_widget_set_sensitive(GTK_WIDGET(g->deflicker_level), p->deflicker);

  deflicker_process (self);
}

static void
deflicker_callback (GtkToggleButton *button, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;

  if(self->dt->gui->reset) return;

  dt_iop_exposure_gui_data_t *g = (dt_iop_exposure_gui_data_t *)self->gui_data;
  dt_iop_exposure_params_t *p = (dt_iop_exposure_params_t *)self->params;

  if(!(self->dev->image_storage.flags & DT_IMAGE_RAW))
  {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->deflicker), FALSE);
    return;
  }

  autoexp_disable(self);

  p->deflicker = gtk_toggle_button_get_active(button);

  gtk_widget_set_sensitive(GTK_WIDGET(g->deflicker_percentile), p->deflicker);
  gtk_widget_set_sensitive(GTK_WIDGET(g->deflicker_level), p->deflicker);

  if(p->deflicker) //deflicker has been turend on
  {
    deflicker_params_callback(NULL, user_data);
  }

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void
exposure_callback (GtkWidget* slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;

  autoexp_disable(self);
  deflicker_disable(self);

  const float exposure = dt_bauhaus_slider_get(slider);
  dt_iop_exposure_set_white(self, exposure2white(exposure));
}

static void
black_callback (GtkWidget* slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;

  const float black = dt_bauhaus_slider_get(slider);
  dt_iop_exposure_set_black(self, black);
}

static gboolean
expose (GtkWidget *widget, GdkEventExpose *event, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return FALSE;

  // Needed if deflicker is part of auto-applied preset
  deflicker_process(self);

  if(self->request_color_pick <= 0) return FALSE;

  if(self->picked_color_max[0] < 0.0f) return FALSE;

  dt_iop_exposure_gui_data_t *g = (dt_iop_exposure_gui_data_t *)self->gui_data;

  const float white = fmaxf(fmaxf(self->picked_color_max[0], self->picked_color_max[1]), self->picked_color_max[2])
                      * (1.0-dt_bauhaus_slider_get(g->autoexpp));
  const float black = fminf(fminf(self->picked_color_min[0], self->picked_color_min[1]), self->picked_color_min[2]);

  exposure_set_white(self, white);
  exposure_set_black(self, black);
  return FALSE;
}

void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_exposure_gui_data_t));
  dt_iop_exposure_gui_data_t *g = (dt_iop_exposure_gui_data_t *)self->gui_data;
  dt_iop_exposure_params_t *p = (dt_iop_exposure_params_t *)self->params;

  /* register hooks with current dev so that  histogram
     can interact with this module.
   */
  darktable.develop->proxy.exposure.module = self;
  darktable.develop->proxy.exposure.set_white = dt_iop_exposure_set_white;
  darktable.develop->proxy.exposure.get_white = dt_iop_exposure_get_white;
  darktable.develop->proxy.exposure.set_black = dt_iop_exposure_set_black;
  darktable.develop->proxy.exposure.get_black = dt_iop_exposure_get_black;

  self->request_color_pick = 0;

  self->widget = GTK_WIDGET(gtk_vbox_new(FALSE, DT_BAUHAUS_SPACE));

  g->black = dt_bauhaus_slider_new_with_range(self, -0.1, 0.1, .001, p->black, 4);
  g_object_set(G_OBJECT(g->black), "tooltip-text", _("adjust the black level"), (char *)NULL);
  dt_bauhaus_slider_set_format(g->black,"%.4f");
  dt_bauhaus_widget_set_label(g->black, NULL, _("black"));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->black), TRUE, TRUE, 0);

  g->exposure = dt_bauhaus_slider_new_with_range(self, -3.0, 3.0, .02, p->exposure, 3);
  g_object_set(G_OBJECT(g->exposure), "tooltip-text", _("adjust the exposure correction"), (char *)NULL);
  dt_bauhaus_slider_set_format(g->exposure,"%.2fEV");
  dt_bauhaus_widget_set_label(g->exposure, NULL, _("exposure"));
  dt_bauhaus_slider_enable_soft_boundaries(g->exposure, -18.0, 18.0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->exposure), TRUE, TRUE, 0);

  g->autoexp  = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("auto")));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->autoexp), FALSE);
  g->autoexpp = dt_bauhaus_slider_new_with_range(self, 0.0, 0.2, .001, 0.01,3);
  g_object_set(G_OBJECT(g->autoexpp), "tooltip-text", _("percentage of bright values clipped out"), (char *)NULL);
  gtk_widget_set_sensitive(GTK_WIDGET(g->autoexpp), TRUE);

  GtkHBox *hbox = GTK_HBOX(gtk_hbox_new(FALSE, 0));
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(g->autoexp), FALSE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(g->autoexpp), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hbox), TRUE, TRUE, 0);

  g->deflicker = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("deflicker")));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->deflicker), p->deflicker);

  g->deflicker_percentile = dt_bauhaus_slider_new_with_range(self, 0, 100, .01, p->deflicker_percentile, 3);
  g_object_set(G_OBJECT(g->deflicker_percentile), "tooltip-text", _("percentile"), (char *)NULL);
  dt_bauhaus_slider_set_format(g->deflicker_percentile,"%.2f%%");
  dt_bauhaus_widget_set_label(g->deflicker_percentile, NULL, _("percentile"));
  gtk_widget_set_sensitive(GTK_WIDGET(g->deflicker_percentile), p->deflicker);

  g->deflicker_level = dt_bauhaus_slider_new_with_range(self, -18.0, 18.0, .01, p->deflicker_level, 3);
  g_object_set(G_OBJECT(g->deflicker_level), "tooltip-text", _("target level"), (char *)NULL);
  dt_bauhaus_slider_set_format(g->deflicker_level,"%.2fEV");
  dt_bauhaus_widget_set_label(g->deflicker_level, NULL, _("target level"));
  gtk_widget_set_sensitive(GTK_WIDGET(g->deflicker_level), p->deflicker);

  GtkVBox *vbox = GTK_VBOX(gtk_vbox_new(FALSE, DT_BAUHAUS_SPACE));
  gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(g->deflicker_percentile), FALSE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(g->deflicker_level), TRUE, TRUE, 0);

  GtkHBox *hbox2 = GTK_HBOX(gtk_hbox_new(FALSE, 0));
  gtk_box_pack_start(GTK_BOX(hbox2), GTK_WIDGET(g->deflicker), FALSE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(hbox2), GTK_WIDGET(vbox), TRUE, TRUE, 0);

  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hbox2), TRUE, TRUE, 0);

  darktable.gui->reset = 1;
  self->gui_update(self);
  darktable.gui->reset = 0;

  g_signal_connect (G_OBJECT (g->black), "value-changed",
                    G_CALLBACK (black_callback), self);
  g_signal_connect (G_OBJECT (g->exposure), "value-changed",
                    G_CALLBACK (exposure_callback), self);
  g_signal_connect (G_OBJECT (g->autoexpp), "value-changed",
                    G_CALLBACK (autoexpp_callback), self);
  g_signal_connect (G_OBJECT (g->autoexp), "toggled",
                    G_CALLBACK (autoexp_callback), self);
  g_signal_connect (G_OBJECT (g->deflicker), "toggled",
                    G_CALLBACK (deflicker_callback), self);
  g_signal_connect (G_OBJECT (g->deflicker_percentile), "value-changed",
                    G_CALLBACK (deflicker_params_callback), self);
  g_signal_connect (G_OBJECT (g->deflicker_level), "value-changed",
                    G_CALLBACK (deflicker_params_callback), self);
  g_signal_connect (G_OBJECT(self->widget), "expose-event",
                    G_CALLBACK(expose), self);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
