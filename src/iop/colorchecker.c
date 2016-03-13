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
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "develop/tiling.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "iop/iop_api.h"
#include "iop/svd.h"
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
39.19, 13.76,  14.29,
65.18, 19.00,  17.32,
49.46, -4.23, -22.95,
42.85,-13.33,  22.12,
55.18,  9.44, -24.94,
70.36,-32.77,  -0.04,
62.92, 35.49,  57.10,
40.75, 11.41, -46.03,
52.10, 48.11,  16.89,
30.67, 21.19, -20.81,
73.08,-23.55,  56.97,
72.43, 17.48,  68.20,
30.97, 12.67, -46.30,
56.43,-40.66,  31.94,
43.40, 50.68,  28.84,
82.45,  2.41,  80.25,
51.98, 50.68, -14.84,
51.02,-27.63, -28.03,
95.97, -0.40,   1.24,
81.10, -0.83,  -0.43,
66.81, -1.08,  -0.70,
50.98, -0.19,  -0.30,
35.72, -0.69,  -1.11,
21.46,  0.06,  -0.95,
};

typedef struct dt_iop_colorchecker_params_t
{
  float target_L[24];
  float target_a[24];
  float target_b[24];
} dt_iop_colorchecker_params_t;

typedef struct dt_iop_colorchecker_gui_data_t
{
  GtkWidget *combobox_patch, *scale_L, *scale_a, *scale_b;
  int patch;
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
  dt_bauhaus_slider_set(g->scale_L, p->target_L[g->patch] - colorchecker_Lab[3*g->patch+0]);
  dt_bauhaus_slider_set(g->scale_a, p->target_a[g->patch] - colorchecker_Lab[3*g->patch+1]);
  dt_bauhaus_slider_set(g->scale_b, p->target_b[g->patch] - colorchecker_Lab[3*g->patch+2]);
}

void init(dt_iop_module_t *module)
{
  module->params = calloc(1, sizeof(dt_iop_colorchecker_params_t));
  module->default_params = calloc(1, sizeof(dt_iop_colorchecker_params_t));
  module->default_enabled = 0;
  module->priority = 365; // module order created by iop_dependencies.py, do not edit!
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
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void target_b_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_colorchecker_params_t *p = (dt_iop_colorchecker_params_t *)self->params;
  dt_iop_colorchecker_gui_data_t *g = (dt_iop_colorchecker_gui_data_t *)self->gui_data;
  p->target_b[g->patch] = CLAMP(colorchecker_Lab[3*g->patch + 2] + dt_bauhaus_slider_get(slider), -128.0, 128.0);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void patch_callback(GtkWidget *combo, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_colorchecker_gui_data_t *g = (dt_iop_colorchecker_gui_data_t *)self->gui_data;
  g->patch = dt_bauhaus_combobox_get(combo);
  self->gui_update(self);
}

void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_colorchecker_gui_data_t));
  dt_iop_colorchecker_gui_data_t *g = (dt_iop_colorchecker_gui_data_t *)self->gui_data;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  // TODO: draw custom 24-patch widget instead of combo box
  g->combobox_patch = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->combobox_patch, NULL, _("patch"));
  gtk_widget_set_tooltip_text(g->combobox_patch, _("color checker patch"));
  for(int k=0;k<24;k++)
    dt_bauhaus_combobox_add(g->combobox_patch, colorchecker_name[k]);

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

  gtk_box_pack_start(GTK_BOX(self->widget), g->combobox_patch, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->scale_L, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->scale_a, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->scale_b, TRUE, TRUE, 0);

  g_signal_connect(G_OBJECT(g->combobox_patch), "value-changed", G_CALLBACK(patch_callback), (gpointer)self);
  g_signal_connect(G_OBJECT(g->scale_L), "value-changed", G_CALLBACK(target_L_callback), self);
  g_signal_connect(G_OBJECT(g->scale_a), "value-changed", G_CALLBACK(target_a_callback), self);
  g_signal_connect(G_OBJECT(g->scale_b), "value-changed", G_CALLBACK(target_b_callback), self);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
