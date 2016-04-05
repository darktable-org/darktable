/*
    This file is part of darktable,
    copyright (c) 2016 johannes hanika.

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
#include "bauhaus/bauhaus.h"
#include "control/control.h"
#include "common/colorspaces.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "develop/tiling.h"
#include "dtgtk/drawingarea.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "iop/iop_api.h"
#include "iop/svd.h"
#include "libs/colorpicker.h"
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <gtk/gtk.h>
#include <inttypes.h>

DT_MODULE_INTROSPECTION(1, dt_iop_colorchecker_params_t)

static const int colorchecker_patches = 24;
static const char* colorchecker_name[] =
{
  N_("dark skin :A0"),
  N_("light skin :A1"),
  N_("blue sky :A2"),
  N_("foliage :A3"),
  N_("blue flower :A4"),
  N_("bluish green :A5"),
  N_("orange :B0"),
  N_("purple red :B1"),
  N_("moderate red :B2"),
  N_("purple :B3"),
  N_("yellow green :B4"),
  N_("orange yellow :B5"),
  N_("blue :C0"),
  N_("green :C1"),
  N_("red :C2"),
  N_("yellow :C3"),
  N_("magenta :C4"),
  N_("cyan :C5"),
  N_("white :D0"),
  N_("neutral 8 :D1"),
  N_("neutral 65 :D2"),
  N_("neutral 5 :D3"),
  N_("neutral 35 :D4"),
  N_("black :D5"),
};
static const float colorchecker_Lab[] =
{
39.19, 13.76,  14.29, // dark skin
65.18, 19.00,  17.32, // light skin
49.46, -4.23, -22.95, // blue sky
42.85,-13.33,  22.12, // foliage
55.18,  9.44, -24.94, // blue flower
70.36,-32.77,  -0.04, // bluish green  
62.92, 35.49,  57.10, // orange
40.75, 11.41, -46.03, // purple red
52.10, 48.11,  16.89, // moderate red  
30.67, 21.19, -20.81, // purple
73.08,-23.55,  56.97, // yellow green  
72.43, 17.48,  68.20, // orange yellow 
30.97, 12.67, -46.30, // blue
56.43,-40.66,  31.94, // green
43.40, 50.68,  28.84, // red
82.45,  2.41,  80.25, // yellow
51.98, 50.68, -14.84, // magenta
51.02,-27.63, -28.03, // cyan
95.97, -0.40,   1.24, // white
81.10, -0.83,  -0.43, // neutral 8
66.81, -1.08,  -0.70, // neutral 65
50.98, -0.19,  -0.30, // neutral 5
35.72, -0.69,  -1.11, // neutral 35
21.46,  0.06,  -0.95, // black
};

typedef struct dt_iop_colorchecker_params_t
{
  float target_L[24];
  float target_a[24];
  float target_b[24];
} dt_iop_colorchecker_params_t;

typedef struct dt_iop_colorchecker_gui_data_t
{
  GtkWidget *area, *combobox_patch, *scale_L, *scale_a, *scale_b, *scale_C;
  int patch, drawn_patch;
  cmsHTRANSFORM xform;
} dt_iop_colorchecker_gui_data_t;

typedef struct dt_iop_colorchecker_data_t
{
  float coeff_L[28];
  float coeff_a[28];
  float coeff_b[28];
} dt_iop_colorchecker_data_t;

const char *name()
{
  return _("color checker lut");
}

int groups()
{
  return IOP_GROUP_COLOR;
}

int flags()
{
  return IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING;
}

void init_presets(dt_iop_module_so_t *self)
{ }

// thinplate spline kernel \phi(r)
static inline float kernel(const float r)
{
  return r*r*logf(MAX(1e-8f,r));
}

static inline float distance(const float *x, const float *y)
{
  return sqrtf(
      (x[0]-y[0])*(x[0]-y[0])+
      (x[1]-y[1])*(x[1]-y[1])+
      (x[2]-y[2])*(x[2]-y[2]));
}

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_colorchecker_data_t *const data = (dt_iop_colorchecker_data_t *)piece->data;
  const int ch = piece->colors;
  // TODO: work on blurred input for smoother transition? see monochrome?
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static) collapse(2)
#endif
  for(int j=0;j<roi_out->height;j++)
  {
    for(int i=0;i<roi_out->width;i++)
    {
      const float *in = ((float *)ivoid) + (size_t)ch * (j * roi_in->width + i);
      float *out = ((float *)ovoid) + (size_t)ch * (j * roi_in->width + i);
      out[0] = data->coeff_L[24];
      out[1] = data->coeff_a[24];
      out[2] = data->coeff_b[24];
      // polynomial part:
      out[0] += data->coeff_L[25] * in[0] + data->coeff_L[26] * in[1] + data->coeff_L[27] * in[2];
      out[1] += data->coeff_a[25] * in[0] + data->coeff_a[26] * in[1] + data->coeff_a[27] * in[2];
      out[2] += data->coeff_b[25] * in[0] + data->coeff_b[26] * in[1] + data->coeff_b[27] * in[2];
      for(int k=0;k<24;k++)
      { // rbf from thin plate spline
        const float phi = kernel(distance(in, colorchecker_Lab + 3*k));
        out[0] += data->coeff_L[k] * phi;
        out[1] += data->coeff_a[k] * phi;
        out[2] += data->coeff_b[k] * phi;
      }
    }
  }
  if(piece->pipe->mask_display) dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_colorchecker_params_t *p = (dt_iop_colorchecker_params_t *)p1;
  dt_iop_colorchecker_data_t *d = (dt_iop_colorchecker_data_t *)piece->data;

#define N 24 // number of patches
  // solve equation system to fit thin plate splines to our data
  double A[(N+4)*(N+4)];
  double target[N+4] = {0.0};

  // find coeffs for three channels separately:
  for(int ch=0;ch<3;ch++)
  {
    if(ch==0) for(int k=0;k<N;k++) target[k] = p->target_L[k];
    if(ch==1) for(int k=0;k<N;k++) target[k] = p->target_a[k];
    if(ch==2) for(int k=0;k<N;k++) target[k] = p->target_b[k];
    // following JP's great siggraph course on scattered data interpolation,
    // construct system matrix A such that:
    // A c = f
    //
    // | R   P | |c| = |f|
    // | P^t 0 | |d|   |0|
    //
    // to interpolate values f_i with the radial basis function system matrix R and a polynomial term P.
    // P is a 3D linear polynomial a + b x + c y + d z
    //
    int wd = N+4;
    // radial basis function part R
    for(int j=0;j<N;j++)
      for(int i=j;i<N;i++)
        A[j*wd+i] = A[i*wd+j] = kernel(distance(colorchecker_Lab+3*i,colorchecker_Lab+3*j));

    // polynomial part P: constant + 3x linear
    for(int i=0;i<N;i++) A[i*wd+N+0] = A[(N+0)*wd+i] = 1.0f;
    for(int i=0;i<N;i++) A[i*wd+N+1] = A[(N+1)*wd+i] = colorchecker_Lab[3*i+0];
    for(int i=0;i<N;i++) A[i*wd+N+2] = A[(N+2)*wd+i] = colorchecker_Lab[3*i+1];
    for(int i=0;i<N;i++) A[i*wd+N+3] = A[(N+3)*wd+i] = colorchecker_Lab[3*i+2];

    for(int j=N;j<wd;j++) for(int i=N;i<wd;i++) A[j*wd+i] = 0.0f;

    // coefficient vector:
    double c[N+4] = {0.0f};

    // svd to solve for c:
    // A * c = offsets
    // A = u w v => A-1 = v^t 1/w u^t
    // regularisation epsilon:
    const float eps = 0.001f;
    double w[N+4], v[(N+4)*(N+4)], tmp[N+4] = {0.0f};
    dsvd(A, N+4, N+4, w, v);

    for(int j=0;j<wd;j++)
      for(int i=0;i<wd;i++)
        tmp[j] += A[i*wd+j] * target[i];
    for(int i=0;i<wd;i++)
      tmp[i] *= w[i] / ((w[i] + eps)*(w[i] + eps));
    for(int j=0;j<wd;j++)
      for(int i=0;i<wd;i++)
        c[j] += v[j*wd+i] * tmp[i];
    if(ch==0) for(int i=0;i<N+4;i++) d->coeff_L[i] = c[i];
    if(ch==1) for(int i=0;i<N+4;i++) d->coeff_a[i] = c[i];
    if(ch==2) for(int i=0;i<N+4;i++) d->coeff_b[i] = c[i];
  }
  // for(int i=0;i<N+4;i++) fprintf(stderr, "coeff L[%d] = %f\n", i, d->coeff_L[i]);
  // for(int i=0;i<N+4;i++) fprintf(stderr, "coeff a[%d] = %f\n", i, d->coeff_a[i]);
  // for(int i=0;i<N+4;i++) fprintf(stderr, "coeff b[%d] = %f\n", i, d->coeff_b[i]);
#undef N
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_colorchecker_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_module_t *module = (dt_iop_module_t *)self;
  dt_iop_colorchecker_gui_data_t *g = (dt_iop_colorchecker_gui_data_t *)self->gui_data;
  dt_iop_colorchecker_params_t *p = (dt_iop_colorchecker_params_t *)module->params;
  if(g->patch >= 24 || g->patch < 0) return;
  dt_bauhaus_slider_set(g->scale_L, p->target_L[g->patch] - colorchecker_Lab[3*g->patch+0]);
  dt_bauhaus_slider_set(g->scale_a, p->target_a[g->patch] - colorchecker_Lab[3*g->patch+1]);
  dt_bauhaus_slider_set(g->scale_b, p->target_b[g->patch] - colorchecker_Lab[3*g->patch+2]);
  const float Cin = sqrtf(
      colorchecker_Lab[3*g->patch+1]*colorchecker_Lab[3*g->patch+1]+
      colorchecker_Lab[3*g->patch+2]*colorchecker_Lab[3*g->patch+2]);
  const float Cout = sqrtf(
      p->target_a[g->patch]*p->target_a[g->patch]+
      p->target_b[g->patch]*p->target_b[g->patch]);
  dt_bauhaus_slider_set(g->scale_C, Cout-Cin);
}

void init(dt_iop_module_t *module)
{
  module->params = calloc(1, sizeof(dt_iop_colorchecker_params_t));
  module->default_params = calloc(1, sizeof(dt_iop_colorchecker_params_t));
  module->default_enabled = 0;
  module->priority = 384; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_colorchecker_params_t);
  module->gui_data = NULL;
  dt_iop_colorchecker_params_t tmp;
  for(int k=0;k<24;k++) tmp.target_L[k] = colorchecker_Lab[3*k+0];
  for(int k=0;k<24;k++) tmp.target_a[k] = colorchecker_Lab[3*k+1];
  for(int k=0;k<24;k++) tmp.target_b[k] = colorchecker_Lab[3*k+2];
  memcpy(module->params, &tmp, sizeof(dt_iop_colorchecker_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_colorchecker_params_t));
}

void cleanup(dt_iop_module_t *module)
{
  free(module->params);
  module->params = NULL;
}

static void picker_callback(GtkWidget *button, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(darktable.gui->reset) return;

  if(self->request_color_pick == DT_REQUEST_COLORPICK_OFF)
    self->request_color_pick = DT_REQUEST_COLORPICK_MODULE;
  else
    self->request_color_pick = DT_REQUEST_COLORPICK_OFF;

  dt_iop_request_focus(self);

  if(self->request_color_pick != DT_REQUEST_COLORPICK_OFF)
    dt_dev_reprocess_all(self->dev);
  else
    dt_control_queue_redraw();

  if(self->off) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->off), 1);
}

static void target_L_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_colorchecker_params_t *p = (dt_iop_colorchecker_params_t *)self->params;
  dt_iop_colorchecker_gui_data_t *g = (dt_iop_colorchecker_gui_data_t *)self->gui_data;
  p->target_L[g->patch] = CLAMP(colorchecker_Lab[3*g->patch + 0] + dt_bauhaus_slider_get(slider), 0.0, 100.0);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void target_a_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_colorchecker_params_t *p = (dt_iop_colorchecker_params_t *)self->params;
  dt_iop_colorchecker_gui_data_t *g = (dt_iop_colorchecker_gui_data_t *)self->gui_data;
  p->target_a[g->patch] = CLAMP(colorchecker_Lab[3*g->patch + 1] + dt_bauhaus_slider_get(slider), -128.0, 128.0);
  const float Cin = sqrtf(
      colorchecker_Lab[3*g->patch+1]*colorchecker_Lab[3*g->patch+1]+
      colorchecker_Lab[3*g->patch+2]*colorchecker_Lab[3*g->patch+2]);
  const float Cout = sqrtf(
      p->target_a[g->patch]*p->target_a[g->patch]+
      p->target_b[g->patch]*p->target_b[g->patch]);
  const int reset = darktable.gui->reset;
  darktable.gui->reset = 1; // avoid history item
  dt_bauhaus_slider_set(g->scale_C, Cout-Cin);
  darktable.gui->reset = reset;
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void target_b_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_colorchecker_params_t *p = (dt_iop_colorchecker_params_t *)self->params;
  dt_iop_colorchecker_gui_data_t *g = (dt_iop_colorchecker_gui_data_t *)self->gui_data;
  p->target_b[g->patch] = CLAMP(colorchecker_Lab[3*g->patch + 2] + dt_bauhaus_slider_get(slider), -128.0, 128.0);
  const float Cin = sqrtf(
      colorchecker_Lab[3*g->patch+1]*colorchecker_Lab[3*g->patch+1]+
      colorchecker_Lab[3*g->patch+2]*colorchecker_Lab[3*g->patch+2]);
  const float Cout = sqrtf(
      p->target_a[g->patch]*p->target_a[g->patch]+
      p->target_b[g->patch]*p->target_b[g->patch]);
  const int reset = darktable.gui->reset;
  darktable.gui->reset = 1; // avoid history item
  dt_bauhaus_slider_set(g->scale_C, Cout-Cin);
  darktable.gui->reset = reset;
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void target_C_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_colorchecker_params_t *p = (dt_iop_colorchecker_params_t *)self->params;
  dt_iop_colorchecker_gui_data_t *g = (dt_iop_colorchecker_gui_data_t *)self->gui_data;
  const float Cin = sqrtf(
      colorchecker_Lab[3*g->patch+1]*colorchecker_Lab[3*g->patch+1]+
      colorchecker_Lab[3*g->patch+2]*colorchecker_Lab[3*g->patch+2]);
  const float Cout = sqrtf(
      p->target_a[g->patch]*p->target_a[g->patch]+
      p->target_b[g->patch]*p->target_b[g->patch]);
  const float Cnew = CLAMP(Cin + dt_bauhaus_slider_get(slider), 0.01, 128.0);
  p->target_a[g->patch] = CLAMP(p->target_a[g->patch]*Cnew/Cout, -128.0, 128.0);
  p->target_b[g->patch] = CLAMP(p->target_b[g->patch]*Cnew/Cout, -128.0, 128.0);
  const int reset = darktable.gui->reset;
  darktable.gui->reset = 1; // avoid history item
  dt_bauhaus_slider_set(g->scale_a, p->target_a[g->patch] - colorchecker_Lab[3*g->patch+1]);
  dt_bauhaus_slider_set(g->scale_b, p->target_b[g->patch] - colorchecker_Lab[3*g->patch+2]);
  darktable.gui->reset = reset;
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void patch_callback(GtkWidget *combo, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_colorchecker_gui_data_t *g = (dt_iop_colorchecker_gui_data_t *)self->gui_data;
  g->patch = dt_bauhaus_combobox_get(combo);
  // switch off colour picker, it'll interfere with other changes of the patch:
  self->request_color_pick = DT_REQUEST_COLORPICK_OFF;
  if(g->patch != g->drawn_patch) gtk_widget_queue_draw(g->area);
  self->gui_update(self);
}

static gboolean checker_draw(GtkWidget *widget, cairo_t *crf, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_colorchecker_gui_data_t *g = (dt_iop_colorchecker_gui_data_t *)self->gui_data;
  dt_iop_colorchecker_params_t *p = (dt_iop_colorchecker_params_t *)self->params;

  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  int width = allocation.width, height = allocation.height;
  cairo_surface_t *cst = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);
  // clear bg
  cairo_set_source_rgb(cr, .2, .2, .2);
  cairo_paint(cr);

  const float *picked_mean = self->picked_color;
  int besti = 0, bestj = 0;
  cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);
  const int cells_x = 6, cells_y = 4;
  for(int j = 0; j < cells_y; j++)
  {
    for(int i = 0; i < cells_x; i++)
    {
      double rgb[3] = { 0.5, 0.5, 0.5 }; // Lab: rgb grey converted to Lab
      cmsCIELab Lab;
      const int patch = i + j*cells_x;
      Lab.L = colorchecker_Lab[3*patch];
      Lab.a = colorchecker_Lab[3*patch+1];
      Lab.b = colorchecker_Lab[3*patch+2];
      if((picked_mean[0] - Lab.L)*(picked_mean[0] - Lab.L) +
         (picked_mean[1] - Lab.a)*(picked_mean[1] - Lab.a) +
         (picked_mean[2] - Lab.b)*(picked_mean[2] - Lab.b) <
         (picked_mean[0] - colorchecker_Lab[3*(6*bestj+besti)+0])*
         (picked_mean[0] - colorchecker_Lab[3*(6*bestj+besti)+0])+
         (picked_mean[1] - colorchecker_Lab[3*(6*bestj+besti)+1])*
         (picked_mean[1] - colorchecker_Lab[3*(6*bestj+besti)+1])+
         (picked_mean[2] - colorchecker_Lab[3*(6*bestj+besti)+2])*
         (picked_mean[2] - colorchecker_Lab[3*(6*bestj+besti)+2]))
      {
        besti = i;
        bestj = j;
      }
      cmsDoTransform(g->xform, &Lab, rgb, 1);
      cairo_set_source_rgb(cr, rgb[0], rgb[1], rgb[2]);
      cairo_rectangle(cr, width * i / (float)cells_x, height * j / (float)cells_y,
          width / (float)cells_x - DT_PIXEL_APPLY_DPI(1),
          height / (float)cells_y - DT_PIXEL_APPLY_DPI(1));
      cairo_fill(cr);
      if(fabsf(p->target_L[patch] - colorchecker_Lab[3*patch+0]) > 1e-6f ||
         fabsf(p->target_a[patch] - colorchecker_Lab[3*patch+1]) > 1e-6f ||
         fabsf(p->target_b[patch] - colorchecker_Lab[3*patch+2]) > 1e-6f)
      {
        cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(2.));
        cairo_set_source_rgb(cr, 0.8, 0.8, 0.8);
        cairo_rectangle(cr,
            width * i / (float)cells_x + DT_PIXEL_APPLY_DPI(1),
            height * j / (float)cells_y + DT_PIXEL_APPLY_DPI(1),
            width / (float)cells_x - DT_PIXEL_APPLY_DPI(3),
            height / (float)cells_y - DT_PIXEL_APPLY_DPI(3));
        cairo_stroke(cr);
        cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.));
        cairo_set_source_rgb(cr, 0.2, 0.2, 0.2);
        cairo_rectangle(cr,
            width * i / (float)cells_x + DT_PIXEL_APPLY_DPI(2),
            height * j / (float)cells_y + DT_PIXEL_APPLY_DPI(2),
            width / (float)cells_x - DT_PIXEL_APPLY_DPI(5),
            height / (float)cells_y - DT_PIXEL_APPLY_DPI(5));
        cairo_stroke(cr);
      }
    }
  }

  // highlight patch that is closest to picked colour,
  // or the one selected in the combobox.
  if(self->request_color_pick == DT_REQUEST_COLORPICK_OFF)
  {
    int i = dt_bauhaus_combobox_get(g->combobox_patch);
    besti = i % cells_x;
    bestj = i / cells_x;
    g->drawn_patch = cells_x * bestj + besti;
  }
  else
  {
    // freshly picked, also select it in gui:
    int pick = self->request_color_pick;
    g->drawn_patch = cells_x * bestj + besti;
    dt_bauhaus_combobox_set(g->combobox_patch, cells_x * bestj + besti);
    self->request_color_pick = pick; // restore, the combobox will kill it
  }
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(2.));
  cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
  cairo_rectangle(cr,
      width * besti / (float)cells_x + DT_PIXEL_APPLY_DPI(5),
      height * bestj / (float)cells_y + DT_PIXEL_APPLY_DPI(5),
      width / (float)cells_x - DT_PIXEL_APPLY_DPI(11),
      height / (float)cells_y - DT_PIXEL_APPLY_DPI(11));
  cairo_stroke(cr);

  cairo_destroy(cr);
  cairo_set_source_surface(crf, cst, 0, 0);
  cairo_paint(crf);
  cairo_surface_destroy(cst);
  return TRUE;
}

static gboolean checker_motion_notify(GtkWidget *widget, GdkEventMotion *event,
    gpointer user_data)
{
  // highlight?
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_colorchecker_gui_data_t *g = (dt_iop_colorchecker_gui_data_t *)self->gui_data;
  // dt_iop_colorchecker_params_t *p = (dt_iop_colorchecker_params_t *)self->params;
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  int width = allocation.width, height = allocation.height;
  const float mouse_x = CLAMP(event->x, 0, width);
  const float mouse_y = CLAMP(event->y, 0, height);
  const float mx = mouse_x * 6.0f / (float)width;
  const float my = mouse_y * 4.0f / (float)height;
  int patch = CLAMP((int)mx + 6*(int)my, 0, 23);
  char tooltip[1024];
  snprintf(tooltip, sizeof(tooltip), _("select patch `%s' (altered patches are marked with an outline)"), _(colorchecker_name[patch]));
  gtk_widget_set_tooltip_text(g->area, tooltip);
  return TRUE;
}

static gboolean checker_button_press(GtkWidget *widget, GdkEventButton *event,
                                                    gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_colorchecker_gui_data_t *g = (dt_iop_colorchecker_gui_data_t *)self->gui_data;
  dt_iop_colorchecker_params_t *p = (dt_iop_colorchecker_params_t *)self->params;
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  int width = allocation.width, height = allocation.height;
  const float mouse_x = CLAMP(event->x, 0, width);
  const float mouse_y = CLAMP(event->y, 0, height);
  const float mx = mouse_x * 6.0f / (float)width;
  const float my = mouse_y * 4.0f / (float)height;
  int patch = CLAMP((int)mx + 6*(int)my, 0, 23);
  dt_bauhaus_combobox_set(g->combobox_patch, patch);
  if(event->button == 1 && event->type == GDK_2BUTTON_PRESS)
  { // reset on double click
    p->target_L[patch] = colorchecker_Lab[3*patch+0];
    p->target_a[patch] = colorchecker_Lab[3*patch+1];
    p->target_b[patch] = colorchecker_Lab[3*patch+2];
    dt_dev_add_history_item(darktable.develop, self, TRUE);
    self->gui_update(self);
  }
  return TRUE;
}

static gboolean checker_leave_notify(GtkWidget *widget, GdkEventCrossing *event,
                                                    gpointer user_data)
{
  return FALSE; // ?
}

void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_colorchecker_gui_data_t));
  dt_iop_colorchecker_gui_data_t *g = (dt_iop_colorchecker_gui_data_t *)self->gui_data;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  // custom 24-patch widget in addition to combo box
  g->area = dtgtk_drawing_area_new_with_aspect_ratio(4.0/6.0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->area, TRUE, TRUE, 0);

  gtk_widget_add_events(GTK_WIDGET(g->area), GDK_POINTER_MOTION_MASK | GDK_POINTER_MOTION_HINT_MASK
                                             | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK
                                             | GDK_LEAVE_NOTIFY_MASK | GDK_SCROLL_MASK);
  g_signal_connect(G_OBJECT(g->area), "draw", G_CALLBACK(checker_draw), self);
  g_signal_connect(G_OBJECT(g->area), "button-press-event", G_CALLBACK(checker_button_press), self);
  g_signal_connect(G_OBJECT(g->area), "motion-notify-event", G_CALLBACK(checker_motion_notify), self);
  g_signal_connect(G_OBJECT(g->area), "leave-notify-event", G_CALLBACK(checker_leave_notify), self);

  g->patch = 0;
  g->drawn_patch = -1;
  g->combobox_patch = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->combobox_patch, NULL, _("patch"));
  gtk_widget_set_tooltip_text(g->combobox_patch, _("color checker patch"));
  for(int k=0;k<24;k++)
    dt_bauhaus_combobox_add(g->combobox_patch, _(colorchecker_name[k]));
  self->request_color_pick = DT_REQUEST_COLORPICK_OFF;
  dt_bauhaus_widget_set_quad_paint(g->combobox_patch, dtgtk_cairo_paint_colorpicker, CPF_ACTIVE);

  g->scale_L = dt_bauhaus_slider_new_with_range(self, -100.0, 100.0, 1.0, 0.0f, 2);
  gtk_widget_set_tooltip_text(g->scale_L, _("lightness offset"));
  dt_bauhaus_widget_set_label(g->scale_L, NULL, _("lightness"));

  g->scale_a = dt_bauhaus_slider_new_with_range(self, -256.0, 256.0, 1.0, 0.0f, 2);
  gtk_widget_set_tooltip_text(g->scale_a, _("chroma offset green/red"));
  dt_bauhaus_widget_set_label(g->scale_a, NULL, _("green/red"));
  dt_bauhaus_slider_set_stop(g->scale_a, 0.0, 0.0, 1.0, 0.2);
  dt_bauhaus_slider_set_stop(g->scale_a, 0.5, 1.0, 1.0, 1.0);
  dt_bauhaus_slider_set_stop(g->scale_a, 1.0, 1.0, 0.0, 0.2);

  g->scale_b = dt_bauhaus_slider_new_with_range(self, -256.0, 256.0, 1.0, 0.0f, 2);
  gtk_widget_set_tooltip_text(g->scale_b, _("chroma offset blue/yellow"));
  dt_bauhaus_widget_set_label(g->scale_b, NULL, _("blue/yellow"));
  dt_bauhaus_slider_set_stop(g->scale_b, 0.0, 0.0, 0.0, 1.0);
  dt_bauhaus_slider_set_stop(g->scale_b, 0.5, 1.0, 1.0, 1.0);
  dt_bauhaus_slider_set_stop(g->scale_b, 1.0, 1.0, 1.0, 0.0);

  g->scale_C = dt_bauhaus_slider_new_with_range(self, -128.0, 128.0, 1.0f, 0.0f, 2);
  gtk_widget_set_tooltip_text(g->scale_C, _("saturation offset"));
  dt_bauhaus_widget_set_label(g->scale_C, NULL, _("saturation"));

  gtk_box_pack_start(GTK_BOX(self->widget), g->combobox_patch, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->scale_L, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->scale_a, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->scale_b, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->scale_C, TRUE, TRUE, 0);

  g_signal_connect(G_OBJECT(g->combobox_patch), "value-changed", G_CALLBACK(patch_callback), self);
  g_signal_connect(G_OBJECT(g->combobox_patch), "quad-pressed", G_CALLBACK(picker_callback), self);
  g_signal_connect(G_OBJECT(g->scale_L), "value-changed", G_CALLBACK(target_L_callback), self);
  g_signal_connect(G_OBJECT(g->scale_a), "value-changed", G_CALLBACK(target_a_callback), self);
  g_signal_connect(G_OBJECT(g->scale_b), "value-changed", G_CALLBACK(target_b_callback), self);
  g_signal_connect(G_OBJECT(g->scale_C), "value-changed", G_CALLBACK(target_C_callback), self);

  cmsHPROFILE hsRGB = dt_colorspaces_get_profile(DT_COLORSPACE_SRGB, "", DT_PROFILE_DIRECTION_IN)->profile;
  cmsHPROFILE hLab = dt_colorspaces_get_profile(DT_COLORSPACE_LAB, "", DT_PROFILE_DIRECTION_ANY)->profile;
  g->xform = cmsCreateTransform(hLab, TYPE_Lab_DBL, hsRGB, TYPE_RGB_DBL, INTENT_PERCEPTUAL,
                                0); // cmsFLAGS_NOTPRECALC);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  dt_iop_colorchecker_gui_data_t *g = (dt_iop_colorchecker_gui_data_t *)self->gui_data;
  cmsDeleteTransform(g->xform);
  free(self->gui_data);
  self->gui_data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
