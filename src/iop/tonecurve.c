/*
    This file is part of darktable,
    copyright (c) 2009--2012 johannes hanika.

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
#include "iop/tonecurve.h"
#include "gui/presets.h"
#include "develop/develop.h"
#include "control/control.h"
#include "gui/gtk.h"
#include "common/opencl.h"

#define DT_GUI_CURVE_EDITOR_INSET 5
#define DT_GUI_CURVE_INFL .3f

#define ROUNDUP(a, n)		((a) % (n) == 0 ? (a) : ((a) / (n) + 1) * (n))

DT_MODULE(3)

const char *name()
{
  return _("tone curve");
}


int
groups ()
{
  return IOP_GROUP_TONE;
}

int
flags ()
{
  return IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING;
}

int
legacy_params (dt_iop_module_t *self, const void *const old_params, const int old_version, void *new_params, const int new_version)
{
  if(old_version == 1 && new_version == 3)
  {
    dt_iop_tonecurve_1_params_t *o = (dt_iop_tonecurve_1_params_t *)old_params;
    dt_iop_tonecurve_params_t *n = (dt_iop_tonecurve_params_t *)new_params;
    dt_iop_tonecurve_params_t *d = (dt_iop_tonecurve_params_t *)self->default_params;

    *n = *d;  // start with a fresh copy of default parameters
    for (int k=0; k<6; k++) n->tonecurve[ch_L][k].x = o->tonecurve_x[k];
    for (int k=0; k<6; k++) n->tonecurve[ch_L][k].y = o->tonecurve_y[k];
    n->tonecurve_nodes[ch_L] = 6;
    n->tonecurve_type[ch_L] = CUBIC_SPLINE;
    n->tonecurve_autoscale_ab = 1;
    n->tonecurve_preset = o->tonecurve_preset;
    return 0;
  }
  return 1;
}

#ifdef HAVE_OPENCL
int
process_cl (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_tonecurve_data_t *d = (dt_iop_tonecurve_data_t *)piece->data;
  dt_iop_tonecurve_global_data_t *gd = (dt_iop_tonecurve_global_data_t *)self->data;
  cl_mem dev_L, dev_a, dev_b = NULL;
  cl_mem dev_coeffs = NULL;
  cl_int err = -999;

  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;
  const int autoscale_ab = d->autoscale_ab;

  size_t sizes[] = { ROUNDUP(width, 4), ROUNDUP(height, 4), 1};
  dev_L = dt_opencl_copy_host_to_device(devid, d->table[ch_L], 256, 256, sizeof(float));
  dev_a = dt_opencl_copy_host_to_device(devid, d->table[ch_a], 256, 256, sizeof(float));
  dev_b = dt_opencl_copy_host_to_device(devid, d->table[ch_b], 256, 256, sizeof(float));
  if (dev_L == NULL || dev_a == NULL || dev_b == NULL) goto error;

  dev_coeffs = dt_opencl_copy_host_to_device_constant(devid, sizeof(float)*2, d->unbounded_coeffs);
  if (dev_coeffs == NULL) goto error;
  dt_opencl_set_kernel_arg(devid, gd->kernel_tonecurve, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_tonecurve, 1, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(devid, gd->kernel_tonecurve, 2, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_tonecurve, 3, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_tonecurve, 4, sizeof(cl_mem), (void *)&dev_L);
  dt_opencl_set_kernel_arg(devid, gd->kernel_tonecurve, 5, sizeof(cl_mem), (void *)&dev_a);
  dt_opencl_set_kernel_arg(devid, gd->kernel_tonecurve, 6, sizeof(cl_mem), (void *)&dev_b);
  dt_opencl_set_kernel_arg(devid, gd->kernel_tonecurve, 7, sizeof(int), (void *)&autoscale_ab);
  dt_opencl_set_kernel_arg(devid, gd->kernel_tonecurve, 8, sizeof(cl_mem), (void *)&dev_coeffs);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_tonecurve, sizes);

  if(err != CL_SUCCESS) goto error;
  dt_opencl_release_mem_object(dev_L);
  dt_opencl_release_mem_object(dev_a);
  dt_opencl_release_mem_object(dev_b); 
  dt_opencl_release_mem_object(dev_coeffs);
  return TRUE;

error:
  if (dev_L != NULL) dt_opencl_release_mem_object(dev_L);
  if (dev_a != NULL) dt_opencl_release_mem_object(dev_a);
  if (dev_b != NULL) dt_opencl_release_mem_object(dev_b);
  if (dev_coeffs != NULL) dt_opencl_release_mem_object(dev_coeffs);
  dt_print(DT_DEBUG_OPENCL, "[opencl_tonecurve] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif

void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *i, void *o, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  const int ch = piece->colors;
  dt_iop_tonecurve_data_t *d = (dt_iop_tonecurve_data_t *)(piece->data);
#ifdef _OPENMP
  #pragma omp parallel for default(none) shared(roi_out, i, o, d) schedule(static)
#endif
  for(int k=0; k<roi_out->height; k++)
  {
    float *in = ((float *)i) + k*ch*roi_out->width;
    float *out = ((float *)o) + k*ch*roi_out->width;

    const float low_approximation = d->table[0][(int)(0.01f * 0xfffful)];

    for (int j=0; j<roi_out->width; j++,in+=ch,out+=ch)
    {
      const float L_in = in[0]/100.0f;

      out[0] = (L_in < 1.0f) ? d->table[ch_L][CLAMP((int)(L_in*0xfffful), 0, 0xffff)] :
        dt_iop_eval_exp(d->unbounded_coeffs, L_in);

      if (d->autoscale_ab == 0)
      {
        const float a_in = (in[1] + 128.0f) / 256.0f;
        const float b_in = (in[2] + 128.0f) / 256.0f;
        out[1] = d->table[ch_a][CLAMP((int)(a_in*0xfffful), 0, 0xffff)];
        out[2] = d->table[ch_b][CLAMP((int)(b_in*0xfffful), 0, 0xffff)];
      }
      // in Lab: correct compressed Luminance for saturation:
      else if(L_in > 0.01f)
      {
        out[1] = in[1] * out[0]/in[0];
        out[2] = in[2] * out[0]/in[0];
      }
      else
      {
        out[0] = in[0] * low_approximation;
        out[1] = in[1] * low_approximation;
        out[2] = in[2] * low_approximation;
      }
    }
  }
}

void init_presets (dt_iop_module_so_t *self)
{
  dt_iop_tonecurve_params_t p;
  p.tonecurve_nodes[ch_L] = 6;
  p.tonecurve_nodes[ch_a] = 7;
  p.tonecurve_nodes[ch_b] = 7;
  p.tonecurve_type[ch_L] = CUBIC_SPLINE;
  p.tonecurve_type[ch_a] = CUBIC_SPLINE;
  p.tonecurve_type[ch_b] = CUBIC_SPLINE;
  p.tonecurve_preset = 0;
  p.tonecurve_autoscale_ab = 1;

  float linear_L[6] = {0.0, 0.08, 0.4, 0.6, 0.92, 1.0};
  float linear_ab[7] = {0.0, 0.08, 0.3, 0.5, 0.7, 0.92, 1.0};

  // linear a, b curves for presets
  for(int k=0; k<7; k++) p.tonecurve[ch_a][k].x = linear_ab[k];
  for(int k=0; k<7; k++) p.tonecurve[ch_a][k].y = linear_ab[k];
  for(int k=0; k<7; k++) p.tonecurve[ch_b][k].x = linear_ab[k];
  for(int k=0; k<7; k++) p.tonecurve[ch_b][k].y = linear_ab[k];


  // More useful low contrast curve (based on Samsung NX -2 Contrast)
  p.tonecurve[ch_L][0].x = 0.000000;
  p.tonecurve[ch_L][1].x = 0.003862;
  p.tonecurve[ch_L][2].x = 0.076613;
  p.tonecurve[ch_L][3].x = 0.169355;
  p.tonecurve[ch_L][4].x = 0.774194;
  p.tonecurve[ch_L][5].x = 1.000000;
  p.tonecurve[ch_L][0].y = 0.000000;
  p.tonecurve[ch_L][1].y = 0.007782;
  p.tonecurve[ch_L][2].y = 0.156182;
  p.tonecurve[ch_L][3].y = 0.290352;
  p.tonecurve[ch_L][4].y = 0.773852;
  p.tonecurve[ch_L][5].y = 1.000000;
  dt_gui_presets_add_generic(_("low contrast"), self->op, self->version(), &p, sizeof(p), 1);



  for(int k=0; k<6; k++) p.tonecurve[ch_L][k].x = linear_L[k];
  for(int k=0; k<6; k++) p.tonecurve[ch_L][k].y = linear_L[k];
  dt_gui_presets_add_generic(_("linear"), self->op, self->version(), &p, sizeof(p), 1);

  for(int k=0; k<6; k++) p.tonecurve[ch_L][k].x = linear_L[k];
  for(int k=0; k<6; k++) p.tonecurve[ch_L][k].y = linear_L[k];
  p.tonecurve[ch_L][1].y -= 0.03;
  p.tonecurve[ch_L][4].y += 0.03;
  p.tonecurve[ch_L][2].y -= 0.03;
  p.tonecurve[ch_L][3].y += 0.03;
  for(int k=1; k<5; k++) p.tonecurve[ch_L][k].x = powf(p.tonecurve[ch_L][k].x, 2.2f);
  for(int k=1; k<5; k++) p.tonecurve[ch_L][k].y = powf(p.tonecurve[ch_L][k].y, 2.2f);
  dt_gui_presets_add_generic(_("med contrast"), self->op, self->version(), &p, sizeof(p), 1);

  for(int k=0; k<6; k++) p.tonecurve[ch_L][k].x = linear_L[k];
  for(int k=0; k<6; k++) p.tonecurve[ch_L][k].y = linear_L[k];
  p.tonecurve[ch_L][1].y -= 0.06;
  p.tonecurve[ch_L][4].y += 0.06;
  p.tonecurve[ch_L][2].y -= 0.10;
  p.tonecurve[ch_L][3].y += 0.10;
  for(int k=1; k<5; k++) p.tonecurve[ch_L][k].x = powf(p.tonecurve[ch_L][k].x, 2.2f);
  for(int k=1; k<5; k++) p.tonecurve[ch_L][k].y = powf(p.tonecurve[ch_L][k].y, 2.2f);
  dt_gui_presets_add_generic(_("high contrast"), self->op, self->version(), &p, sizeof(p), 1);
}

void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  // pull in new params to gegl
  dt_iop_tonecurve_data_t *d = (dt_iop_tonecurve_data_t *)(piece->data);
  dt_iop_tonecurve_params_t *p = (dt_iop_tonecurve_params_t *)p1;
  for(int ch=0; ch<ch_max; ch++)
  {
    // take care of possible change of curve type or number of nodes (not yet implemented in UI)
    if(d->curve_type[ch] != p->tonecurve_type[ch] || d->curve_nodes[ch] != p->tonecurve_nodes[ch])
    {
      dt_draw_curve_destroy(d->curve[ch]);
      d->curve[ch] = dt_draw_curve_new(0.0, 1.0, p->tonecurve_type[ch]);
      d->curve_nodes[ch] = p->tonecurve_nodes[ch];
      d->curve_type[ch] = p->tonecurve_type[ch];
      for(int k=0; k<p->tonecurve_nodes[ch]; k++)
	(void)dt_draw_curve_add_point(d->curve[ch], p->tonecurve[ch][k].x, p->tonecurve[ch][k].y);
    }
    else
    {
      for(int k=0; k<p->tonecurve_nodes[ch]; k++)
        dt_draw_curve_set_point(d->curve[ch], k, p->tonecurve[ch][k].x, p->tonecurve[ch][k].y);
    }
    dt_draw_curve_calc_values(d->curve[ch], 0.0f, 1.0f, 0x10000, NULL, d->table[ch]);
  }
  for(int k=0; k<0x10000; k++) d->table[ch_L][k] *= 100.0f;
  for(int k=0; k<0x10000; k++) d->table[ch_a][k] = d->table[ch_a][k]*256.0f - 128.0f;
  for(int k=0; k<0x10000; k++) d->table[ch_b][k] = d->table[ch_b][k]*256.0f - 128.0f;

  d->autoscale_ab = p->tonecurve_autoscale_ab;

  // now the extrapolation stuff (for L curve only):
  const float x[4] = {0.7f, 0.8f, 0.9f, 1.0f};
  const float y[4] = {d->table[ch_L][CLAMP((int)(x[0]*0x10000ul), 0, 0xffff)],
                      d->table[ch_L][CLAMP((int)(x[1]*0x10000ul), 0, 0xffff)],
                      d->table[ch_L][CLAMP((int)(x[2]*0x10000ul), 0, 0xffff)],
                      d->table[ch_L][CLAMP((int)(x[3]*0x10000ul), 0, 0xffff)]};
  dt_iop_estimate_exp(x, y, 4, d->unbounded_coeffs);
}

void init_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  // create part of the gegl pipeline
  dt_iop_tonecurve_data_t *d = (dt_iop_tonecurve_data_t *)malloc(sizeof(dt_iop_tonecurve_data_t));
  dt_iop_tonecurve_params_t *default_params = (dt_iop_tonecurve_params_t *)self->default_params;
  piece->data = (void *)d;
  d->autoscale_ab = 1;
  for(int ch=0; ch<ch_max; ch++)
  {
    d->curve[ch] = dt_draw_curve_new(0.0, 1.0, default_params->tonecurve_type[ch]);
    d->curve_nodes[ch] = default_params->tonecurve_nodes[ch];
    d->curve_type[ch] = default_params->tonecurve_type[ch];
    for(int k=0; k<default_params->tonecurve_nodes[ch]; k++)
	(void)dt_draw_curve_add_point(d->curve[ch], default_params->tonecurve[ch][k].x, default_params->tonecurve[ch][k].y);
  }
  for(int k=0; k<0x10000; k++) d->table[ch_L][k] = 100.0f*k/0x10000; // identity for L
  for(int k=0; k<0x10000; k++) d->table[ch_a][k] = 256.0f*k/0x10000 - 128.0f; // identity for a
  for(int k=0; k<0x10000; k++) d->table[ch_b][k] = 256.0f*k/0x10000 - 128.0f; // identity for b
}

void cleanup_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  // clean up everything again.
  dt_iop_tonecurve_data_t *d = (dt_iop_tonecurve_data_t *)(piece->data);
  for (int ch=0; ch<ch_max; ch++) dt_draw_curve_destroy(d->curve[ch]);
  free(piece->data);
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_tonecurve_gui_data_t *g = (dt_iop_tonecurve_gui_data_t *)self->gui_data;
  dt_iop_tonecurve_params_t *p = (dt_iop_tonecurve_params_t *)self->params;
  gtk_toggle_button_set_active(g->autoscale_ab, p->tonecurve_autoscale_ab);
  // that's all, gui curve is read directly from params during expose event.
  gtk_widget_queue_draw(self->widget);
}


void init(dt_iop_module_t *module)
{
  module->params = malloc(sizeof(dt_iop_tonecurve_params_t));
  module->default_params = malloc(sizeof(dt_iop_tonecurve_params_t));
  module->default_enabled = 0;
  module->priority = 581; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_tonecurve_params_t);
  module->gui_data = NULL;
  dt_iop_tonecurve_params_t tmp = (dt_iop_tonecurve_params_t)
  {
    {
      {							// three curves (L, a, b) with a number of nodes
        {0.0, 0.0}, {0.08, 0.08}, {0.4, 0.4}, {0.6, 0.6}, {0.92, 0.92}, {1.0, 1.0}
      },
      {
        {0.0, 0.0}, {0.08, 0.08}, {0.3, 0.3}, {0.5, 0.5}, {0.7, 0.7}, {0.92, 0.92}, {1.0, 1.0}
      },
      {
        {0.0, 0.0}, {0.08, 0.08}, {0.3, 0.3}, {0.5, 0.5}, {0.7, 0.7}, {0.92, 0.92}, {1.0, 1.0}
      }
    },
    { 6, 7, 7 },					// number of nodes per curve
    { CUBIC_SPLINE, CUBIC_SPLINE, CUBIC_SPLINE }, 	// curve types
    1,							// autoscale_ab
    0
  };
  memcpy(module->params, &tmp, sizeof(dt_iop_tonecurve_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_tonecurve_params_t));
}

void init_global(dt_iop_module_so_t *module)
{
  const int program = 2; // basic.cl, from programs.conf
  dt_iop_tonecurve_global_data_t *gd = (dt_iop_tonecurve_global_data_t *)malloc(sizeof(dt_iop_tonecurve_global_data_t));
  module->data = gd;
  gd->kernel_tonecurve = dt_opencl_create_kernel(program, "tonecurve");
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_tonecurve_global_data_t *gd = (dt_iop_tonecurve_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_tonecurve);
  free(module->data);
  module->data = NULL;
}

void cleanup(dt_iop_module_t *module)
{
  free(module->gui_data);
  module->gui_data = NULL;
  free(module->params);
  module->params = NULL;
}

static void
autoscale_ab_callback(GtkRange *range, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_tonecurve_gui_data_t *g = (dt_iop_tonecurve_gui_data_t *)self->gui_data;
  dt_iop_tonecurve_params_t *p = (dt_iop_tonecurve_params_t *)self->params;
  p->tonecurve_autoscale_ab = gtk_toggle_button_get_active(g->autoscale_ab);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void
tab_switch(GtkNotebook *notebook, GtkNotebookPage *page, guint page_num, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_tonecurve_gui_data_t *c = (dt_iop_tonecurve_gui_data_t *)self->gui_data;
  if(self->dt->gui->reset) return;
  c->channel = (tonecurve_channel_t)page_num;
  gtk_widget_queue_draw(self->widget);
}


void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_tonecurve_gui_data_t));
  dt_iop_tonecurve_gui_data_t *c = (dt_iop_tonecurve_gui_data_t *)self->gui_data;
  dt_iop_tonecurve_params_t *p = (dt_iop_tonecurve_params_t *)self->params;

  for (int ch=0; ch<ch_max; ch++)
  {
    c->minmax_curve[ch] = dt_draw_curve_new(0.0, 1.0, p->tonecurve_type[ch]);
    c->minmax_curve_nodes[ch] = p->tonecurve_nodes[ch];
    c->minmax_curve_type[ch] = p->tonecurve_type[ch];
    for(int k=0; k<p->tonecurve_nodes[ch]; k++) (void)dt_draw_curve_add_point(c->minmax_curve[ch], p->tonecurve[ch][k].x, p->tonecurve[ch][k].y);
  }

  c->channel = ch_L;
  c->mouse_x = c->mouse_y = -1.0;
  c->selected = -1;
  c->selected_offset = c->selected_y = c->selected_min = c->selected_max = 0.0;
  c->dragging = 0;
  c->x_move = -1;

  self->widget = GTK_WIDGET(gtk_vbox_new(FALSE, 0));

  // tabs
  GtkWidget *hbox = gtk_hbox_new(FALSE, 0);
  c->channel_tabs = GTK_NOTEBOOK(gtk_notebook_new());

  gtk_notebook_append_page(GTK_NOTEBOOK(c->channel_tabs), GTK_WIDGET(gtk_hbox_new(FALSE,0)), gtk_label_new(_("  L  ")));
  g_object_set(G_OBJECT(gtk_notebook_get_tab_label(c->channel_tabs, gtk_notebook_get_nth_page(c->channel_tabs, -1))), "tooltip-text", _("tonecurve for L channel"), NULL);
  gtk_notebook_append_page(GTK_NOTEBOOK(c->channel_tabs), GTK_WIDGET(gtk_hbox_new(FALSE,0)), gtk_label_new(_("  a  ")));
  g_object_set(G_OBJECT(gtk_notebook_get_tab_label(c->channel_tabs, gtk_notebook_get_nth_page(c->channel_tabs, -1))), "tooltip-text", _("tonecurve for a channel"), NULL);
  gtk_notebook_append_page(GTK_NOTEBOOK(c->channel_tabs), GTK_WIDGET(gtk_hbox_new(FALSE,0)), gtk_label_new(_("  b  ")));
  g_object_set(G_OBJECT(gtk_notebook_get_tab_label(c->channel_tabs, gtk_notebook_get_nth_page(c->channel_tabs, -1))), "tooltip-text", _("tonecurve for b channel"), NULL);

  gtk_widget_show_all(GTK_WIDGET(gtk_notebook_get_nth_page(c->channel_tabs, c->channel)));
  gtk_notebook_set_current_page(GTK_NOTEBOOK(c->channel_tabs), c->channel);

  g_object_set(G_OBJECT(c->channel_tabs), "homogeneous", TRUE, (char *)NULL);

  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(c->channel_tabs), TRUE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), hbox, FALSE, FALSE, 0);

  g_signal_connect(G_OBJECT(c->channel_tabs), "switch_page",
                   G_CALLBACK (tab_switch), self);


  c->area = GTK_DRAWING_AREA(gtk_drawing_area_new());
  GtkWidget *asp = gtk_aspect_frame_new(NULL, 0.5, 0.5, 1.0, TRUE);
  gtk_box_pack_start(GTK_BOX(self->widget), asp, TRUE, TRUE, 0);
  gtk_container_add(GTK_CONTAINER(asp), GTK_WIDGET(c->area));
  // gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(c->area), TRUE, TRUE, 0);
  gtk_drawing_area_size(c->area, 258, 258);
  g_object_set (GTK_OBJECT(c->area), "tooltip-text", _("double click to reset curve"), (char *)NULL);

  c->sizegroup = GTK_SIZE_GROUP(gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL));
  gtk_size_group_add_widget(c->sizegroup, GTK_WIDGET(c->area));
  gtk_size_group_add_widget(c->sizegroup, GTK_WIDGET(c->channel_tabs));

  gtk_widget_add_events(GTK_WIDGET(c->area), GDK_POINTER_MOTION_MASK | GDK_POINTER_MOTION_HINT_MASK | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_LEAVE_NOTIFY_MASK);
  g_signal_connect (G_OBJECT (c->area), "expose-event",
                    G_CALLBACK (dt_iop_tonecurve_expose), self);
  g_signal_connect (G_OBJECT (c->area), "button-press-event",
                    G_CALLBACK (dt_iop_tonecurve_button_press), self);
  g_signal_connect (G_OBJECT (c->area), "button-release-event",
                    G_CALLBACK (dt_iop_tonecurve_button_release), self);
  g_signal_connect (G_OBJECT (c->area), "motion-notify-event",
                    G_CALLBACK (dt_iop_tonecurve_motion_notify), self);
  g_signal_connect (G_OBJECT (c->area), "leave-notify-event",
                    G_CALLBACK (dt_iop_tonecurve_leave_notify), self);

  c->autoscale_ab  = GTK_TOGGLE_BUTTON(gtk_check_button_new_with_label(_("autoscale Lab chroma values according to L")));
  gtk_toggle_button_set_active(c->autoscale_ab, p->tonecurve_autoscale_ab);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(c->autoscale_ab), TRUE, TRUE, 5);
  g_object_set (GTK_OBJECT(c->autoscale_ab), "tooltip-text", _("if checked a and b curves have no effect and are\nnot displayed. chroma values (a and b) of each pixel\nare then adjusted based on L curve data."), (char *)NULL);
  g_signal_connect(G_OBJECT(c->autoscale_ab), "toggled", G_CALLBACK(autoscale_ab_callback), self);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  dt_iop_tonecurve_gui_data_t *c = (dt_iop_tonecurve_gui_data_t *)self->gui_data;
  // this one we need to unref manually. not so the initially unowned widgets.
  g_object_unref(c->sizegroup);
  dt_draw_curve_destroy(c->minmax_curve[ch_L]);
  dt_draw_curve_destroy(c->minmax_curve[ch_a]);
  dt_draw_curve_destroy(c->minmax_curve[ch_b]);
  free(self->gui_data);
  self->gui_data = NULL;
}


static gboolean dt_iop_tonecurve_leave_notify(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_tonecurve_gui_data_t *c = (dt_iop_tonecurve_gui_data_t *)self->gui_data;
  c->mouse_x = c->mouse_y = -1.0;
  gtk_widget_queue_draw(widget);
  return TRUE;
}

static gboolean dt_iop_tonecurve_expose(GtkWidget *widget, GdkEventExpose *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_tonecurve_gui_data_t *c = (dt_iop_tonecurve_gui_data_t *)self->gui_data;
  dt_iop_tonecurve_params_t *p = (dt_iop_tonecurve_params_t *)self->params;

  const float color_labels_left[3][3] = { { 0.3f, 0.3f,  0.3f  },
                                          { 0.0f, 0.34f, 0.27f },
                                          { 0.0f, 0.27f, 0.58f }};

  const float color_labels_right[3][3] = {{ 0.3f, 0.3f, 0.3f   },
                                          { 0.53f, 0.08f, 0.28f},
                                          { 0.81f, 0.66f, 0.0f }};

  int ch = c->channel;
  int nodes = p->tonecurve_nodes[ch];
  dt_iop_tonecurve_node_t *tonecurve = p->tonecurve[ch];
  int autoscale_ab = p->tonecurve_autoscale_ab;
  dt_draw_curve_t *minmax_curve = c->minmax_curve[ch];

  for(int k=0; k<nodes; k++) dt_draw_curve_set_point(minmax_curve, k, tonecurve[k].x, tonecurve[k].y);
  const int inset = DT_GUI_CURVE_EDITOR_INSET;
  int width = widget->allocation.width, height = widget->allocation.height;
  cairo_surface_t *cst = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);
  // clear bg
  cairo_set_source_rgb (cr, .2, .2, .2);
  cairo_paint(cr);

  cairo_translate(cr, inset, inset);
  width -= 2*inset;
  height -= 2*inset;

#if 0
  // draw shadow around
  float alpha = 1.0f;
  for(int k=0; k<inset; k++)
  {
    cairo_rectangle(cr, -k, -k, width + 2*k, height + 2*k);
    cairo_set_source_rgba(cr, 0, 0, 0, alpha);
    alpha *= 0.6f;
    cairo_fill(cr);
  }
#else
  cairo_set_line_width(cr, 1.0);
  cairo_set_source_rgb (cr, .1, .1, .1);
  cairo_rectangle(cr, 0, 0, width, height);
  cairo_stroke(cr);
#endif

  cairo_set_source_rgb (cr, .3, .3, .3);
  cairo_rectangle(cr, 0, 0, width, height);
  cairo_fill(cr);

  // draw color labels
  cairo_set_source_rgb (cr, color_labels_left[ch][0], color_labels_left[ch][1], color_labels_left[ch][2]);
  cairo_rectangle(cr, 0, 7*width/8, width/8, height/8);
  cairo_fill(cr);

  cairo_set_source_rgb (cr, color_labels_right[ch][0], color_labels_right[ch][1], color_labels_right[ch][2]);
  cairo_rectangle(cr, 7*width/8, 0, width/8, height/8);
  cairo_fill(cr);

  if(c->mouse_y > 0 || c->dragging)
  {
    float oldx1, oldy1;
    oldx1 = tonecurve[c->selected].x;
    oldy1 = tonecurve[c->selected].y;

    if(c->selected == 0) dt_draw_curve_set_point(minmax_curve, 1, tonecurve[1].x, fmaxf(c->selected_min, tonecurve[1].y));
    if(c->selected == 2) dt_draw_curve_set_point(minmax_curve, 1, tonecurve[1].x, fminf(c->selected_min, fmaxf(0.0, tonecurve[1].y + DT_GUI_CURVE_INFL*(c->selected_min - oldy1))));
    if(c->selected == nodes-3) dt_draw_curve_set_point(minmax_curve, nodes-2, tonecurve[nodes-2].x, fmaxf(c->selected_min, fminf(1.0, tonecurve[nodes-2].y + DT_GUI_CURVE_INFL*(c->selected_min - oldy1))));
    if(c->selected == nodes-1) dt_draw_curve_set_point(minmax_curve, nodes-2, tonecurve[nodes-2].x, fminf(c->selected_min, tonecurve[nodes-2].y));
    dt_draw_curve_set_point(minmax_curve, c->selected, oldx1, c->selected_min);
    dt_draw_curve_calc_values(minmax_curve, 0.0, 1.0, DT_IOP_TONECURVE_RES, c->draw_min_xs, c->draw_min_ys);

    if(c->selected == 0) dt_draw_curve_set_point(minmax_curve, 1, tonecurve[1].x, fmaxf(c->selected_max, tonecurve[1].y));
    if(c->selected == 2) dt_draw_curve_set_point(minmax_curve, 1, tonecurve[1].x, fminf(c->selected_max, fmaxf(0.0, tonecurve[1].y + DT_GUI_CURVE_INFL*(c->selected_max - oldy1))));
    if(c->selected == nodes-3) dt_draw_curve_set_point(minmax_curve, nodes-2, tonecurve[nodes-2].x, fmaxf(c->selected_max, fminf(1.0, tonecurve[nodes-2].y + DT_GUI_CURVE_INFL*(c->selected_max - oldy1))));
    if(c->selected == nodes-1) dt_draw_curve_set_point(minmax_curve, nodes-2, tonecurve[nodes-2].x, fminf(c->selected_max, tonecurve[nodes-2].y));
    dt_draw_curve_set_point  (minmax_curve, c->selected, oldx1, c->selected_max);
    dt_draw_curve_calc_values(minmax_curve, 0.0, 1.0, DT_IOP_TONECURVE_RES, c->draw_max_xs, c->draw_max_ys);

  }
  for(int k=0; k<nodes; k++) dt_draw_curve_set_point(minmax_curve, k, tonecurve[k].x, tonecurve[k].y);
  dt_draw_curve_calc_values(minmax_curve, 0.0, 1.0, DT_IOP_TONECURVE_RES, c->draw_xs, c->draw_ys);

  // draw grid
  cairo_set_line_width(cr, .4);
  cairo_set_source_rgb (cr, .1, .1, .1);
  dt_draw_grid(cr, 4, 0, 0, width, height);

  // if autoscale_ab is on: do not display a and b curves
  if (autoscale_ab && ch != ch_L) goto finally;

  // draw x positions
  cairo_set_line_width(cr, 1.);
  cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
  const float arrw = 7.0f;
  for(int k=1; k<nodes-1; k++)
  {
    cairo_move_to(cr, width*tonecurve[k].x, height+inset-1);
    cairo_rel_line_to(cr, -arrw*.5f, 0);
    cairo_rel_line_to(cr, arrw*.5f, -arrw);
    cairo_rel_line_to(cr, arrw*.5f, arrw);
    cairo_close_path(cr);
    if(c->x_move == k) cairo_fill(cr);
    else               cairo_stroke(cr);
  }

  // draw selected cursor
  cairo_set_line_width(cr, 1.);
  cairo_translate(cr, 0, height);

  // draw lum h istogram in background
  dt_develop_t *dev = darktable.develop;
  float *hist, hist_max;
  hist = dev->histogram_pre_tonecurve;
  hist_max = dev->histogram_pre_tonecurve_max;
  if(hist_max > 0 && ch == ch_L)
  {
    cairo_save(cr);
    cairo_scale(cr, width/63.0, -(height-5)/(float)hist_max);
    cairo_set_source_rgba(cr, .2, .2, .2, 0.5);
    dt_draw_histogram_8(cr, hist, 3);
    cairo_restore(cr);
  }

  if(c->mouse_y > 0 || c->dragging)
  {
    // draw min/max, if selected
    cairo_set_source_rgba(cr, .6, .6, .6, .5);
    cairo_move_to(cr, 0, - height*c->draw_min_ys[0]);
    for(int k=1; k<DT_IOP_TONECURVE_RES; k++)   cairo_line_to(cr, k*width/(DT_IOP_TONECURVE_RES-1.0), - height*c->draw_min_ys[k]);
    cairo_line_to(cr, width, - height*c->draw_min_ys[DT_IOP_TONECURVE_RES-1]);
    cairo_line_to(cr, width, - height*c->draw_max_ys[DT_IOP_TONECURVE_RES-1]);
    for(int k=DT_IOP_TONECURVE_RES-2; k>=0; k--) cairo_line_to(cr, k*width/(DT_IOP_TONECURVE_RES-1.0), - height*c->draw_max_ys[k]);
    cairo_close_path(cr);
    cairo_fill(cr);
    // draw mouse focus circle
    cairo_set_source_rgb(cr, .9, .9, .9);
    const float pos = MAX(0, (DT_IOP_TONECURVE_RES-1) * c->mouse_x/(float)width - 1);
    int k = (int)pos;
    const float f = k - pos;
    if(k >= DT_IOP_TONECURVE_RES-1) k = DT_IOP_TONECURVE_RES - 2;
    float ht = -height*(f*c->draw_ys[k] + (1-f)*c->draw_ys[k+1]);
    cairo_arc(cr, c->mouse_x, ht, 4, 0, 2.*M_PI);
    cairo_stroke(cr);
  }

  // draw curve
  cairo_set_line_width(cr, 2.);
  cairo_set_source_rgb(cr, .9, .9, .9);
  // cairo_set_line_cap  (cr, CAIRO_LINE_CAP_SQUARE);
  cairo_move_to(cr, 0, -height*c->draw_ys[0]);
  for(int k=1; k<DT_IOP_TONECURVE_RES; k++) cairo_line_to(cr, k*width/(DT_IOP_TONECURVE_RES-1.0), - height*c->draw_ys[k]);
  cairo_stroke(cr);

finally:
  cairo_destroy(cr);
  cairo_t *cr_pixmap = gdk_cairo_create(gtk_widget_get_window(widget));
  cairo_set_source_surface (cr_pixmap, cst, 0, 0);
  cairo_paint(cr_pixmap);
  cairo_destroy(cr_pixmap);
  cairo_surface_destroy(cst);
  return TRUE;
}

static gboolean dt_iop_tonecurve_motion_notify(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_tonecurve_gui_data_t *c = (dt_iop_tonecurve_gui_data_t *)self->gui_data;
  dt_iop_tonecurve_params_t *p = (dt_iop_tonecurve_params_t *)self->params;


  const float selected_min_left[3] = { 0.0f, 0.0f, 0.0f };
  const float selected_max_left[3] = { 0.2f, 0.5f, 0.5f };
  const float selected_min_right[3] = { 0.8f, 0.5f, 0.5f };
  const float selected_max_right[3] = { 1.0f, 1.0f, 1.0f };

  int ch = c->channel;
  int nodes = p->tonecurve_nodes[ch];
  dt_iop_tonecurve_node_t *tonecurve = p->tonecurve[ch];
  int autoscale_ab = p->tonecurve_autoscale_ab;

  // if autoscale_ab is on: do not modify a and b curves
  if (autoscale_ab && ch != ch_L) goto finally;

  const int inset = DT_GUI_CURVE_EDITOR_INSET;
  int height = widget->allocation.height - 2*inset, width = widget->allocation.width - 2*inset;
  if(!c->dragging) c->mouse_x = CLAMP(event->x - inset, 0, width);
  c->mouse_y = CLAMP(event->y - inset, 0, height);

  if(c->dragging)
  {
    if(c->x_move >= 0)
    {
      const float mx = CLAMP(event->x - inset, 0, width)/(float)width;
      if(c->x_move > 0 && c->x_move < nodes-1)
      {
        const float minx = tonecurve[c->x_move-1].x + 0.001f;
        const float maxx = tonecurve[c->x_move+1].x - 0.001f;
        tonecurve[c->x_move].x = fminf(maxx, fmaxf(minx, mx));
      }
    }
    else
    {
      float f = c->selected_y - (c->mouse_y-c->selected_offset)/height;
      f = fmaxf(c->selected_min, fminf(c->selected_max, f));
      if(c->selected == 0) tonecurve[1].y = fmaxf(f, tonecurve[1].y);
      if(c->selected == 2) tonecurve[1].y = fminf(f, fmaxf(0.0, tonecurve[1].y + DT_GUI_CURVE_INFL*(f - tonecurve[2].y)));
      if(c->selected == nodes-3) tonecurve[nodes-2].y = fmaxf(f, fminf(1.0, tonecurve[nodes-2].y + DT_GUI_CURVE_INFL*(f - tonecurve[nodes-3].y)));
      if(c->selected == nodes-1) tonecurve[nodes-2].y = fminf(f, tonecurve[nodes-2].y);
      tonecurve[c->selected].y = f;
    }
    dt_dev_add_history_item(darktable.develop, self, TRUE);
  }
  else
  {
    if(event->y > height)
    {
      c->x_move = 0;
      const float mx = CLAMP(event->x - inset, 0, width)/(float)width;
      float dist = fabsf(tonecurve[0].x - mx);
      for(int k=1; k<nodes; k++)
      {
        float d2 = fabsf(tonecurve[k].x - mx);
        if(d2 < dist)
        {
          c->x_move = k;
          dist = d2;
        }
      }
    }
    else
    {
      c->x_move = -1;
    }
    float pos = (event->x - inset)/width;
    float min = 100.0;
    int nearest = 0;
    for(int k=0; k<nodes; k++)
    {
      float dist = (pos - tonecurve[k].x);
      dist *= dist;
      if(dist < min)
      {
        min = dist;
        nearest = k;
      }
    }
    c->selected = nearest;
    c->selected_y = tonecurve[c->selected].y;
    c->selected_offset = c->mouse_y;
    const float f = 0.8f;
    if(c->selected == 0)
    {
      c->selected_min = selected_min_left[ch];
      c->selected_max = selected_max_left[ch];
    }
    else if(c->selected == nodes-1)
    {
      c->selected_min = selected_min_right[ch];
      c->selected_max = selected_max_right[ch];
    }
    else
    {
      c->selected_min = fmaxf(c->selected_y - 0.2f, (1.-f)*c->selected_y + f*tonecurve[c->selected-1].y);
      c->selected_max = fminf(c->selected_y + 0.2f, (1.-f)*c->selected_y + f*tonecurve[c->selected+1].y);
    }
    if (ch == 0)
    {
      if(c->selected == 1) c->selected_max *= 0.7;
      if(c->selected == nodes-2) c->selected_min = 1.0 - 0.7*(1.0 - c->selected_min);
    }
  }
finally:
  gtk_widget_queue_draw(widget);
  return TRUE;
}

static gboolean dt_iop_tonecurve_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;

  if(event->button == 1 && event->type == GDK_2BUTTON_PRESS)
  {
    // reset current curve
    dt_iop_tonecurve_params_t *p = (dt_iop_tonecurve_params_t *)self->params;
    dt_iop_tonecurve_params_t *d = (dt_iop_tonecurve_params_t *)self->factory_params;
    dt_iop_tonecurve_gui_data_t *c = (dt_iop_tonecurve_gui_data_t *)self->gui_data;

    int ch = c->channel;
    int nodes = p->tonecurve_nodes[ch];
    int autoscale_ab = p->tonecurve_autoscale_ab;

    // if autoscale_ab is on: allow only reset of L curve
    if (!(autoscale_ab && ch != ch_L))
    {
      for(int k=0; k<nodes; k++)
      {
          p->tonecurve[ch][k].x = d->tonecurve[ch][k].x;
          p->tonecurve[ch][k].y = d->tonecurve[ch][k].y;
      }
      dt_dev_add_history_item(darktable.develop, self, TRUE);
      gtk_widget_queue_draw(self->widget);
    }
  }
  // set active point
  else if(event->button == 1)
  {
    dt_iop_module_t *self = (dt_iop_module_t *)user_data;
    dt_iop_tonecurve_gui_data_t *c = (dt_iop_tonecurve_gui_data_t *)self->gui_data;
    c->dragging = 1;
    return TRUE;
  }
  return FALSE;
}

static gboolean dt_iop_tonecurve_button_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  if(event->button == 1)
  {
    dt_iop_module_t *self = (dt_iop_module_t *)user_data;
    dt_iop_tonecurve_gui_data_t *c = (dt_iop_tonecurve_gui_data_t *)self->gui_data;
    c->dragging = 0;
    return TRUE;
  }
  return FALSE;
}

