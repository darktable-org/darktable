/*
    This file is part of darktable,
    copyright (c) 2019 edgardo hoszowski.

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

/*
  Exposure fusion algorithm based on Tom Mertens, Jan Kautz and Frank van Reeth, “Exposure Fusion”:
  https://mericam.github.io/papers/exposure_fusion_reduced.pdf

  Exposure weight modes are based on Enfuse options:
  http://enblend.sourceforge.net/enfuse.doc/enfuse_4.2.xhtml/enfuse.html
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "bauhaus/bauhaus.h"
#include "common/colorspaces_inline_conversions.h"

DT_MODULE_INTROSPECTION(1, dt_iop_fusion_params_t)

typedef enum dt_iop_grey_projectors_t
{
  DT_PROJECTOR_AVERAGE = 0,
  DT_PROJECTOR_MIN = 1,
  DT_PROJECTOR_MAX = 2,
  DT_PROJECTOR_RGB_LUMINANCE = 3,
  DT_PROJECTOR_HSL_LIGHTNESS = 4,
  DT_PROJECTOR_LAB_LIGHTNESS = 5
} dt_iop_grey_projectors_t;

typedef struct dt_iop_fusion_params_t
{
  int num_exposures;         // number of exposure fusion steps
  float exposure_stops;      // number of stops between fusion images
  float exposure_optimum;    // Optimum brightness for exposure fusion
  float exposure_width;      // exposure weight function variance
  int grey_projector;        // rgb --> grey
  float exposure_left_cutoff;
  float exposure_right_cutoff;
} dt_iop_fusion_params_t;

typedef struct dt_iop_fusion_params_t dt_iop_fusion_data_t;

typedef struct dt_iop_fusion_gui_data_t
{
  GtkWidget *sl_num_exposures;
  GtkWidget *sl_exposure_stops;
  GtkWidget *sl_exposure_optimum;
  GtkWidget *sl_exposure_width;
  GtkWidget *cmb_grey_projector;
  GtkWidget *sl_exposure_left_cutoff;
  GtkWidget *sl_exposure_right_cutoff;
} dt_iop_fusion_gui_data_t;

const char *name()
{
  return _("exposure fusion");
}

int default_group()
{
  return IOP_GROUP_BASIC;
}

int flags()
{
  return IOP_FLAGS_SUPPORTS_BLENDING;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return iop_cs_rgb;
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *params, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  memcpy(piece->data, params, sizeof(dt_iop_fusion_params_t));
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1, sizeof(dt_iop_fusion_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_fusion_params_t *p = (dt_iop_fusion_params_t *)self->params;
  dt_iop_fusion_gui_data_t *g = (dt_iop_fusion_gui_data_t *)self->gui_data;

  dt_bauhaus_slider_set(g->sl_num_exposures, p->num_exposures);
  dt_bauhaus_slider_set(g->sl_exposure_stops, p->exposure_stops);
  dt_bauhaus_slider_set(g->sl_exposure_optimum, p->exposure_optimum);
  dt_bauhaus_slider_set(g->sl_exposure_width, p->exposure_width);
  dt_bauhaus_combobox_set(g->cmb_grey_projector, p->grey_projector);
  dt_bauhaus_slider_set(g->sl_exposure_left_cutoff, p->exposure_left_cutoff * 100.f);
  dt_bauhaus_slider_set(g->sl_exposure_right_cutoff, p->exposure_right_cutoff * 100.f);
}

void init(dt_iop_module_t *module)
{
  module->params = calloc(1, sizeof(dt_iop_fusion_params_t));
  module->default_params = calloc(1, sizeof(dt_iop_fusion_params_t));
  module->default_enabled = 0;
  module->params_size = sizeof(dt_iop_fusion_params_t);
  module->gui_data = NULL;
  dt_iop_fusion_params_t tmp = { 0 };

  tmp.num_exposures = 3;
  tmp.exposure_stops = 1.f;
  tmp.exposure_optimum = .5f;
  tmp.exposure_width = .2f;
  tmp.grey_projector = DT_PROJECTOR_AVERAGE;
  tmp.exposure_left_cutoff = 0.f;
  tmp.exposure_right_cutoff = 1.f;

  memcpy(module->params, &tmp, sizeof(dt_iop_fusion_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_fusion_params_t));
}

void cleanup(dt_iop_module_t *module)
{
  free(module->params);
  module->params = NULL;
}

static void _num_exposures_callback(GtkWidget *widget, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_fusion_params_t *p = (dt_iop_fusion_params_t *)self->params;

  p->num_exposures = dt_bauhaus_slider_get(widget);

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void _exposure_stops_callback(GtkWidget *widget, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_fusion_params_t *p = (dt_iop_fusion_params_t *)self->params;

  p->exposure_stops = dt_bauhaus_slider_get(widget);

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void _exposure_optimum_callback(GtkWidget *widget, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_fusion_params_t *p = (dt_iop_fusion_params_t *)self->params;

  p->exposure_optimum = dt_bauhaus_slider_get(widget);

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void _exposure_width_callback(GtkWidget *widget, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_fusion_params_t *p = (dt_iop_fusion_params_t *)self->params;

  p->exposure_width = dt_bauhaus_slider_get(widget);

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void _grey_projector_callback(GtkWidget *widget, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_fusion_params_t *p = (dt_iop_fusion_params_t *)self->params;

  p->grey_projector = dt_bauhaus_combobox_get(widget);

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void _exposure_left_cutoff_callback(GtkWidget *widget, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_fusion_params_t *p = (dt_iop_fusion_params_t *)self->params;

  p->exposure_left_cutoff = dt_bauhaus_slider_get(widget) / 100.f;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void _exposure_right_cutoff_callback(GtkWidget *widget, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_fusion_params_t *p = (dt_iop_fusion_params_t *)self->params;

  p->exposure_right_cutoff = dt_bauhaus_slider_get(widget) / 100.f;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_fusion_gui_data_t));
  dt_iop_fusion_gui_data_t *g = (dt_iop_fusion_gui_data_t *)self->gui_data;
  dt_iop_fusion_params_t *p = (dt_iop_fusion_params_t *)self->params;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
  dt_gui_add_help_link(self->widget, dt_get_help_url(self->op));

  g->sl_num_exposures = dt_bauhaus_slider_new_with_range(self, 2, 5, 1, p->num_exposures, 0);
  gtk_widget_set_tooltip_text(g->sl_num_exposures, _("number of different exposures to fuse"));
  dt_bauhaus_slider_set_format(g->sl_num_exposures, "%.2fEV");
  dt_bauhaus_widget_set_label(g->sl_num_exposures, NULL, _("number of exposures"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->sl_num_exposures, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(g->sl_num_exposures), "value-changed", G_CALLBACK(_num_exposures_callback), self);

  g->sl_exposure_stops = dt_bauhaus_slider_new_with_range(self, 0.0f, 3.0f, 0.100f, p->exposure_stops, 3);
  gtk_widget_set_tooltip_text(g->sl_exposure_stops, _("how many stops to shift the individual exposures apart"));
  dt_bauhaus_slider_set_format(g->sl_exposure_stops, "%.2fEV");
  dt_bauhaus_widget_set_label(g->sl_exposure_stops, NULL, _("exposure shift"));
  dt_bauhaus_slider_enable_soft_boundaries(g->sl_exposure_stops, 0.0, 5.0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->sl_exposure_stops, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(g->sl_exposure_stops), "value-changed", G_CALLBACK(_exposure_stops_callback), self);

  g->sl_exposure_optimum = dt_bauhaus_slider_new_with_range(self, 0.01f, 1.0f, 0.100f, p->exposure_optimum, 4);
  gtk_widget_set_tooltip_text(g->sl_exposure_optimum, _("optimum exposure value"));
  dt_bauhaus_widget_set_label(g->sl_exposure_optimum, NULL, _("exposure optimum"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->sl_exposure_optimum, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(g->sl_exposure_optimum), "value-changed", G_CALLBACK(_exposure_optimum_callback), self);

  g->sl_exposure_width = dt_bauhaus_slider_new_with_range(self, 0.01f, 1.0f, 0.100f, p->exposure_width, 3);
  gtk_widget_set_tooltip_text(g->sl_exposure_width, _("exposure width"));
  dt_bauhaus_widget_set_label(g->sl_exposure_width, NULL, _("exposure width"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->sl_exposure_width, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(g->sl_exposure_width), "value-changed", G_CALLBACK(_exposure_width_callback), self);

  g->cmb_grey_projector = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->cmb_grey_projector, NULL, _("grey projector"));
  dt_bauhaus_combobox_add(g->cmb_grey_projector, _("average rgb"));
  dt_bauhaus_combobox_add(g->cmb_grey_projector, _("min rgb"));
  dt_bauhaus_combobox_add(g->cmb_grey_projector, _("max rgb"));
  dt_bauhaus_combobox_add(g->cmb_grey_projector, _("rgb luminance"));
  dt_bauhaus_combobox_add(g->cmb_grey_projector, _("hsl lightness"));
  dt_bauhaus_combobox_add(g->cmb_grey_projector, _("lab lightness"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->cmb_grey_projector, TRUE, TRUE, 0);
  gtk_widget_set_tooltip_text(g->cmb_grey_projector, _("method to convert from rgb to grey scale when calculating pixels weights"));
  g_signal_connect(G_OBJECT(g->cmb_grey_projector), "value-changed", G_CALLBACK(_grey_projector_callback), self);

  g->sl_exposure_left_cutoff = dt_bauhaus_slider_new_with_range(self, 0.0f, 100.0f, 0.01f, p->exposure_left_cutoff * 100.f, 3);
  gtk_widget_set_tooltip_text(g->sl_exposure_left_cutoff, _("excludes from merging pixels that don't fall in range"));
  dt_bauhaus_slider_set_format(g->sl_exposure_left_cutoff, "%.3f%%");
  dt_bauhaus_widget_set_label(g->sl_exposure_left_cutoff, NULL, _("exposure cutoff - left"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->sl_exposure_left_cutoff, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(g->sl_exposure_left_cutoff), "value-changed", G_CALLBACK(_exposure_left_cutoff_callback), self);

  g->sl_exposure_right_cutoff = dt_bauhaus_slider_new_with_range(self, 0.0f, 100.0f, 0.01f, p->exposure_right_cutoff * 100.f, 3);
  gtk_widget_set_tooltip_text(g->sl_exposure_right_cutoff, _("excludes from merging pixels that don't fall in range"));
  dt_bauhaus_slider_set_format(g->sl_exposure_right_cutoff, "%.3f%%");
  dt_bauhaus_widget_set_label(g->sl_exposure_right_cutoff, NULL, _("exposure cutoff - right"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->sl_exposure_right_cutoff, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(g->sl_exposure_right_cutoff), "value-changed", G_CALLBACK(_exposure_right_cutoff_callback), self);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

//////////////////////////////////////
// fusion
//////////////////////////////////////

typedef struct dt_image_pyramid_t
{
  float *img;
  size_t w, h;
  int ch;
} dt_image_pyramid_t;

typedef struct dt_pyramid_t
{
  dt_image_pyramid_t *images;
  int num_levels;
} dt_pyramid_t;

static inline void _apply_exposure(const float *const img_src, const size_t wd, const size_t ht, const int ch, const float exp,
                                   float *const img_dest, const gboolean use_sse)
{
  const size_t size = wd * ht * ch;

#if defined(__SSE__)
  if(use_sse && ch == 4)
  {
    const __m128 exp4 = _mm_set1_ps(exp);
    const __m128 zero = _mm_set1_ps(0.f);
    const __m128 one = _mm_set1_ps(1.f);

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(size, ch, img_src, img_dest, exp4, zero, one) \
  schedule(static)
#endif
    for(int i = 0; i < size; i += ch)
      _mm_store_ps(img_dest + i, _mm_min_ps(_mm_max_ps(_mm_mul_ps(_mm_load_ps(img_src + i), exp4), zero), one));

    return;
  }
#endif


#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(size, exp, img_src, img_dest) \
  schedule(static)
#endif
  for(int i = 0; i < size; i++)
  {
    img_dest[i] = img_src[i] * exp;
    img_dest[i] = CLAMP(img_dest[i], 0.f, 1.f);
  }
}

static inline void _image_copy(const float *const img_src, const size_t wd, const size_t ht, const int ch,
                               float *const img_dest)
{
  const size_t size = wd * ht * ch;
  memcpy(img_dest, img_src, size * sizeof(float));
}

static inline void _images_div(const float *const img_src1, const size_t wd, const size_t ht, const int ch,
                               const float *const img_src2, float *const img_dest, const gboolean use_sse)
{
  const size_t size = wd * ht * ch;

#if defined(__SSE__)
  if(use_sse && ch == 4)
  {
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(size, ch, img_src1, img_src2, img_dest) \
  schedule(static)
#endif
    for(int i = 0; i < size; i += ch)
      _mm_store_ps(img_dest + i, _mm_div_ps(_mm_load_ps(img_src1 + i), _mm_load_ps(img_src2 + i)));

    return;
  }
#endif

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(size, img_src1, img_src2, img_dest) \
  schedule(static)
#endif
  for(int i = 0; i < size; i++)
  {
    if(img_src2[i] != 0.f) img_dest[i] = img_src1[i] / img_src2[i];
  }
}

static inline void _images_add(const float *const img_src1, const size_t wd, const size_t ht, const int ch,
                               const float *const img_src2, float *const img_dest, const gboolean use_sse)
{
  const size_t size = wd * ht * ch;

#if defined(__SSE__)
  if(use_sse && ch == 4)
  {
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(size, ch, img_src1, img_src2, img_dest) \
  schedule(static)
#endif
    for(int i = 0; i < size; i += ch)
      _mm_store_ps(img_dest + i, _mm_add_ps(_mm_load_ps(img_src1 + i), _mm_load_ps(img_src2 + i)));

    return;
  }
#endif

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(size, img_src1, img_src2, img_dest) \
  schedule(static)
#endif
  for(int i = 0; i < size; i++)
    img_dest[i] = img_src1[i] + img_src2[i];
}

static inline void _images_add_weighted(const float *const img_src1, const size_t wd, const size_t ht, const int ch,
                               const float *const img_src2, const float *const img_weight, float *const img_dest, const gboolean use_sse)
{
  const size_t size = wd * ht;

#if defined(__SSE__)
  if(use_sse && ch == 4)
  {
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(size, ch, img_src1, img_src2, img_weight, img_dest) \
  schedule(static)
#endif
    for(int i = 0; i < size; i++)
    {
      const __m128 weight = _mm_set1_ps(img_weight[i]);
      _mm_store_ps(img_dest + (i*ch), _mm_add_ps(_mm_load_ps(img_src1 + (i*ch)), _mm_mul_ps(_mm_load_ps(img_src2 + (i*ch)), weight)));
    }

    return;
  }
#endif

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(size, ch, img_src1, img_src2, img_weight, img_dest) \
  schedule(static)
#endif
  for(int i = 0; i < size; i++)
  {
    for(int c = 0; c < ch; c++)
      img_dest[i*ch + c] = img_src1[i*ch + c] + img_src2[i*ch + c] * img_weight[i];
  }
}

static inline void _image_add(const float *const img_src, const size_t wd, const size_t ht, const int ch,
                              const float val, float *const img_dest, const gboolean use_sse)
{
  const size_t size = wd * ht * ch;

#if defined(__SSE__)
  if(use_sse && ch == 4)
  {
    const __m128 val4 = _mm_set1_ps(val);

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(size, ch, img_src, img_dest, val4) \
  schedule(static)
#endif
    for(int i = 0; i < size; i += ch)
      _mm_store_ps(img_dest + i, _mm_add_ps(_mm_load_ps(img_src + i), val4));

    return;
  }
#endif

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(size, val, img_src, img_dest) \
  schedule(static)
#endif
  for(int i = 0; i < size; i++)
    img_dest[i] = img_src[i] + val;
}

static void _convolve_symmetric(const float *const img_src, const size_t wd, const size_t ht, const int ch,
                                const float *const fx, const float *const fy, float *const img_dest)
{
  const int ch1 = (ch == 4) ? 3: ch;
  float *img_tmp = dt_alloc_align(64, wd * ht * ch * sizeof(float));

  // horizontal filter
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(img_src, img_tmp, fx, ht, wd, ch, ch1) \
  schedule(static) \
  collapse(2)
#endif
  for(int i = 0; i < ht; i++) // all lines
  {
    for(int j = 2; j < wd - 2; j++)
      for(int k = 0; k < ch1; k++)
        img_tmp[(i * wd + j) * ch + k] =
            img_src[(i*wd + (j - 2)) * ch + k] * fx[0] +
            img_src[(i*wd + (j - 1)) * ch + k] * fx[1] +
            img_src[(i*wd +  j     ) * ch + k] * fx[2] +
            img_src[(i*wd + (j + 1)) * ch + k] * fx[3] +
            img_src[(i*wd + (j + 2)) * ch + k] * fx[4];
  }

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(img_src, img_tmp, fx, ht, wd, ch, ch1) \
  schedule(static)
#endif
  for(int i = 0; i < ht; i++)
  {
    // left edge
    int j = 0; // 1 0 [0 1 2 ... ]
    for(int k = 0; k < ch1; k++)
      img_tmp[(i * wd + j) * ch + k] =
          img_src[(i*wd + (j + 1)) * ch + k] * fx[0] +
          img_src[(i*wd +  j     ) * ch + k] * fx[1] +
          img_src[(i*wd +  j     ) * ch + k] * fx[2] +
          img_src[(i*wd + (j + 1)) * ch + k] * fx[3] +
          img_src[(i*wd + (j + 2)) * ch + k] * fx[4];

    j = 1; // -1 [-1 0 1 2 ... ]
    for(int k = 0; k < ch1; k++)
      img_tmp[(i * wd + j) * ch + k] =
          img_src[(i*wd + (j - 1)) * ch + k] * fx[0] +
          img_src[(i*wd + (j - 1)) * ch + k] * fx[1] +
          img_src[(i*wd +  j     ) * ch + k] * fx[2] +
          img_src[(i*wd + (j + 1)) * ch + k] * fx[3] +
          img_src[(i*wd + (j + 2)) * ch + k] * fx[4];

    // right edge
    j = wd - 2; // [ ... -2 -1 0 1] 1
    for(int k = 0; k < ch1; k++)
      img_tmp[(i * wd + j) * ch + k] =
          img_src[(i*wd + (j - 2)) * ch + k] * fx[0] +
          img_src[(i*wd + (j - 1)) * ch + k] * fx[1] +
          img_src[(i*wd +  j     ) * ch + k] * fx[2] +
          img_src[(i*wd + (j + 1)) * ch + k] * fx[3] +
          img_src[(i*wd + (j + 1)) * ch + k] * fx[4];

    j = wd - 1; // [ ... -2 -1 0] 0 -1
    for(int k = 0; k < ch1; k++)
      img_tmp[(i * wd + j) * ch + k] =
          img_src[(i*wd + (j - 2)) * ch + k] * fx[0] +
          img_src[(i*wd + (j - 1)) * ch + k] * fx[1] +
          img_src[(i*wd +  j     ) * ch + k] * fx[2] +
          img_src[(i*wd +  j     ) * ch + k] * fx[3] +
          img_src[(i*wd + (j - 1)) * ch + k] * fx[4];
  }

  // vertical filter
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(img_dest, img_tmp, fy, wd, ht, ch, ch1) \
  schedule(static) \
  collapse(2)
#endif
  for(int j = 0; j < wd; j++) // all columns
  {
    for(int i = 2; i < ht - 2; i++)
      for(int k = 0; k < ch1; k++)
        img_dest[(i * wd + j) * ch + k] =
            img_tmp[((i - 2) * wd + j) * ch + k] * fy[0] +
            img_tmp[((i - 1) * wd + j) * ch + k] * fy[1] +
            img_tmp[( i      * wd + j) * ch + k] * fy[2] +
            img_tmp[((i + 1) * wd + j) * ch + k] * fy[3] +
            img_tmp[((i + 2) * wd + j) * ch + k] * fy[4];
  }

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(img_dest, img_tmp, fy, wd, ht, ch, ch1) \
  schedule(static)
#endif
  for(int j = 0; j < wd; j++)
  {
    // top edge
    int i = 0; // 1 0 [0 1 2 ... ]
    for(int k = 0; k < ch1; k++)
      img_dest[(i * wd + j) * ch + k] =
          img_tmp[((i + 1) * wd + j) * ch + k] * fy[0] +
          img_tmp[( i      * wd + j) * ch + k] * fy[1] +
          img_tmp[( i      * wd + j) * ch + k] * fy[2] +
          img_tmp[((i + 1) * wd + j) * ch + k] * fy[3] +
          img_tmp[((i + 2) * wd + j) * ch + k] * fy[4];

    i = 1; // -1 [-1 0 1 2 ... ]
    for(int k = 0; k < ch1; k++)
      img_dest[(i * wd + j) * ch + k] =
          img_tmp[((i - 1) * wd + j) * ch + k] * fy[0] +
          img_tmp[((i - 1) * wd + j) * ch + k] * fy[1] +
          img_tmp[( i      * wd + j) * ch + k] * fy[2] +
          img_tmp[((i + 1) * wd + j) * ch + k] * fy[3] +
          img_tmp[((i + 2) * wd + j) * ch + k] * fy[4];

    // bottom edge
    i = ht - 2; // [ ... -2 -1 0 1] 1
    for(int k = 0; k < ch1; k++)
      img_dest[(i * wd + j) * ch + k] =
          img_tmp[((i - 2) * wd + j) * ch + k] * fy[0] +
          img_tmp[((i - 1) * wd + j) * ch + k] * fy[1] +
          img_tmp[( i      * wd + j) * ch + k] * fy[2] +
          img_tmp[((i + 1) * wd + j) * ch + k] * fy[3] +
          img_tmp[((i + 1) * wd + j) * ch + k] * fy[4];

    i = ht - 1; // [ ... -2 -1 0] 0 -1
    for(int k = 0; k < ch1; k++)
      img_dest[(i * wd + j) * ch + k] =
          img_tmp[((i - 2) * wd + j) * ch + k] * fy[0] +
          img_tmp[((i - 1) * wd + j) * ch + k] * fy[1] +
          img_tmp[( i      * wd + j) * ch + k] * fy[2] +
          img_tmp[( i      * wd + j) * ch + k] * fy[3] +
          img_tmp[((i - 1) * wd + j) * ch + k] * fy[4];
  }

  dt_free_align(img_tmp);
}

static void _convolve_replicate(const float *const img_src, const size_t wd, const size_t ht, const int ch,
                                const float *const fx, const float *const fy, float *const img_dest)
{
  const int ch1 = (ch == 4) ? 3: ch;
  float *img_tmp = dt_alloc_align(64, wd * ht * ch * sizeof(float));

  // horizontal filter
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(img_src, img_tmp, fx, ht, wd, ch, ch1) \
  schedule(static) \
  collapse(2)
#endif
  for(int i = 0; i < ht; i++) // all rows
  {
    for(int j = 2; j < wd - 2; j++)
      for(int k = 0; k < ch1; k++)
        img_tmp[(i * wd + j) * ch + k] =
            img_src[(i*wd + (j - 2)) * ch + k] * fx[0] +
            img_src[(i*wd + (j - 1)) * ch + k] * fx[1] +
            img_src[(i*wd +  j     ) * ch + k] * fx[2] +
            img_src[(i*wd + (j + 1)) * ch + k] * fx[3] +
            img_src[(i*wd + (j + 2)) * ch + k] * fx[4];
  }

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(img_src, img_tmp, fx, ht, wd, ch, ch1) \
  schedule(static)
#endif
  for(int i = 0; i < ht; i++)
  {
    // left edge
    int j = 0; // 0 0 [0 1 2 ... ]
    for(int k = 0; k < ch1; k++)
      img_tmp[(i * wd + j) * ch + k] =
          img_src[(i*wd +  j     ) * ch + k] * fx[0] +
          img_src[(i*wd +  j     ) * ch + k] * fx[1] +
          img_src[(i*wd +  j     ) * ch + k] * fx[2] +
          img_src[(i*wd + (j + 1)) * ch + k] * fx[3] +
          img_src[(i*wd + (j + 2)) * ch + k] * fx[4];

    j = 1; // -1 [-1 0 1 2 ... ]
    for(int k = 0; k < ch1; k++)
      img_tmp[(i * wd + j) * ch + k] =
          img_src[(i*wd + (j - 1)) * ch + k] * fx[0] +
          img_src[(i*wd + (j - 1)) * ch + k] * fx[1] +
          img_src[(i*wd +  j     ) * ch + k] * fx[2] +
          img_src[(i*wd + (j + 1)) * ch + k] * fx[3] +
          img_src[(i*wd + (j + 2)) * ch + k] * fx[4];

    // right edge
    j = wd - 2; // [ ... -2 -1 0 1] 1
    for(int k = 0; k < ch1; k++)
      img_tmp[(i * wd + j) * ch + k] =
          img_src[(i*wd + (j - 2)) * ch + k] * fx[0] +
          img_src[(i*wd + (j - 1)) * ch + k] * fx[1] +
          img_src[(i*wd +  j     ) * ch + k] * fx[2] +
          img_src[(i*wd + (j + 1)) * ch + k] * fx[3] +
          img_src[(i*wd + (j + 1)) * ch + k] * fx[4];

    j = wd - 1; // [ ... -2 -1 0] 0 0
    for(int k = 0; k < ch1; k++)
      img_tmp[(i * wd + j) * ch + k] =
          img_src[(i*wd + (j - 2)) * ch + k] * fx[0] +
          img_src[(i*wd + (j - 1)) * ch + k] * fx[1] +
          img_src[(i*wd +  j     ) * ch + k] * fx[2] +
          img_src[(i*wd +  j     ) * ch + k] * fx[3] +
          img_src[(i*wd +  j     ) * ch + k] * fx[4];
  }

  // vertical filter
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(img_dest, img_tmp, fy, wd, ht, ch, ch1) \
  schedule(static) \
  collapse(2)
#endif
  for(int j = 0; j < wd; j++) // all columns
  {
    for(int i = 2; i < ht - 2; i++)
      for(int k = 0; k < ch1; k++)
        img_dest[(i * wd + j) * ch + k] =
            img_tmp[((i - 2) * wd + j) * ch + k] * fy[0] +
            img_tmp[((i - 1) * wd + j) * ch + k] * fy[1] +
            img_tmp[( i      * wd + j) * ch + k] * fy[2] +
            img_tmp[((i + 1) * wd + j) * ch + k] * fy[3] +
            img_tmp[((i + 2) * wd + j) * ch + k] * fy[4];
  }

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(img_dest, img_tmp, fy, wd, ht, ch, ch1) \
  schedule(static)
#endif
  for(int j = 0; j < wd; j++)
  {
    // top edge
    int i = 0; // 0 0 [0 1 2 ... ]
    for(int k = 0; k < ch1; k++)
      img_dest[(i * wd + j) * ch + k] =
          img_tmp[( i      * wd + j) * ch + k] * fy[0] +
          img_tmp[( i      * wd + j) * ch + k] * fy[1] +
          img_tmp[( i      * wd + j) * ch + k] * fy[2] +
          img_tmp[((i + 1) * wd + j) * ch + k] * fy[3] +
          img_tmp[((i + 2) * wd + j) * ch + k] * fy[4];

    i = 1; // -1 [-1 0 1 2 ... ]
    for(int k = 0; k < ch1; k++)
      img_dest[(i * wd + j) * ch + k] =
          img_tmp[((i - 1) * wd + j) * ch + k] * fy[0] +
          img_tmp[((i - 1) * wd + j) * ch + k] * fy[1] +
          img_tmp[( i      * wd + j) * ch + k] * fy[2] +
          img_tmp[((i + 1) * wd + j) * ch + k] * fy[3] +
          img_tmp[((i + 2) * wd + j) * ch + k] * fy[4];

    // bottom edge
    i = ht - 2; // [ ... -2 -1 0 1] 1
    for(int k = 0; k < ch1; k++)
      img_dest[(i * wd + j) * ch + k] =
          img_tmp[((i - 2) * wd + j) * ch + k] * fy[0] +
          img_tmp[((i - 1) * wd + j) * ch + k] * fy[1] +
          img_tmp[( i      * wd + j) * ch + k] * fy[2] +
          img_tmp[((i + 1) * wd + j) * ch + k] * fy[3] +
          img_tmp[((i + 1) * wd + j) * ch + k] * fy[4];

    i = ht - 1; // [ ... -2 -1 0] 0 0
    for(int k = 0; k < ch1; k++)
      img_dest[(i * wd + j) * ch + k] =
          img_tmp[((i - 2) * wd + j) * ch + k] * fy[0] +
          img_tmp[((i - 1) * wd + j) * ch + k] * fy[1] +
          img_tmp[( i      * wd + j) * ch + k] * fy[2] +
          img_tmp[( i      * wd + j) * ch + k] * fy[3] +
          img_tmp[( i      * wd + j) * ch + k] * fy[4];
  }

  dt_free_align(img_tmp);
}

static void _alloc_image(dt_image_pyramid_t *img, const size_t wd, const size_t ht, const int ch)
{
  img->w = wd;
  img->h = ht;
  img->ch = ch;
  img->img = dt_alloc_align(64, img->w * img->h * img->ch * sizeof(float));
  memset(img->img, 0, img->w * img->h * img->ch * sizeof(float));
}

static void _free_image(dt_image_pyramid_t *img)
{
  dt_free_align(img->img);
  img->img = NULL;
}

static void _alloc_pyramid(dt_pyramid_t *pyramid, const size_t wd, const size_t ht, const int ch,
                           const int num_levels)
{
  pyramid->images = (dt_image_pyramid_t *)malloc(num_levels * sizeof(dt_image_pyramid_t));
  pyramid->num_levels = num_levels;

  size_t w = wd;
  size_t h = ht;

  for(int i = 0; i < num_levels; i++)
  {
    _alloc_image(pyramid->images + i, w, h, ch);

    h = h / 2 + (h % 2);
    w = w / 2 + (w % 2);
  }
}

static void _free_pyramid(dt_pyramid_t *pyramid)
{
  for(int i = 0; i < pyramid->num_levels; i++) _free_image(pyramid->images + i);
  free(pyramid->images);
  pyramid->images = NULL;
}

static void _downsample_image(const float *const img_src, const size_t wd, const size_t ht, const int ch,
                              const float *const filter, const size_t down_wd, const size_t down_ht, float *const img_dest)
{
  const int ch1 = (ch == 4) ? 3: ch;
  float *img_tmp = dt_alloc_align(64, wd * ht * ch * sizeof(float));

  // low pass filter
  _convolve_symmetric(img_src, wd, ht, ch, filter, filter, img_tmp);

  // decimate, using every second entry
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(img_dest, img_tmp, wd, down_ht, down_wd, ch, ch1) \
  schedule(static) \
  collapse(2)
#endif
  for(int i = 0; i < down_ht; i++)
    for(int j = 0; j < down_wd; j++)
      for(int k = 0; k < ch1; k++)
        img_dest[(i * down_wd + j) * ch + k] = img_tmp[((i * 2) * wd + (j * 2)) * ch + k];

  dt_free_align(img_tmp);
}

static void _upsample_image(const float *const img_src, const size_t wd, const size_t ht, const int ch,
                            const float *const filter, const size_t up_wd, const size_t up_ht,
                            float *const img_add_sub, float *const img_dest, const gboolean add_to_image, float *const img_wmap)
{
  const int ch1 = (ch == 4) ? 3: ch;
  const size_t pad = 1;

  // upsampled sizes = 1 pixel padding and duplicate size
  const size_t wd_upsd = (wd + 2 * pad) * 2;
  const size_t ht_upsd = (ht + 2 * pad) * 2;

  float *img_tmp = dt_alloc_align(64, wd_upsd * ht_upsd * ch * sizeof(float));
  memset(img_tmp, 0, wd_upsd * ht_upsd * ch * sizeof(float));

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(img_src, img_tmp, ht, wd, ch, ch1, pad, wd_upsd) \
  schedule(static) \
  collapse(2)
#endif
  for(int i = 0; i < ht; i++)
    for(int j = 0; j < wd; j++)
      for(int k = 0; k < ch1; k++)
        img_tmp[((2 * (i + pad)) * wd_upsd + (2 * (j + pad))) * ch + k] = 4.f * img_src[(i * wd + j) * ch + k];

  // top row
  int i = -1;
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(img_src, img_tmp, wd, ch, ch1, pad, wd_upsd, i) \
  schedule(static)
#endif
  for(int j = 0; j < wd; j++)
    for(int k = 0; k < ch1; k++)
      img_tmp[((2 * (i + pad)) * wd_upsd + (2 * (j + pad))) * ch + k] = 4.f * img_src[((i + 1) * wd + j) * ch + k];

  // bottom row
  i = ht;
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(img_src, img_tmp, wd, ch, ch1, pad, wd_upsd, i) \
  schedule(static)
#endif
  for(int j = 0; j < wd; j++)
    for(int k = 0; k < ch1; k++)
      img_tmp[((2 * (i + pad)) * wd_upsd + (2 * (j + pad))) * ch + k] = 4.f * img_src[((i - 1) * wd + j) * ch + k];

  // left edge
  int j = -1;
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(img_src, img_tmp, ht, wd, ch, ch1, pad, wd_upsd, j) \
  schedule(static)
#endif
  for(int ii = 0; ii < ht; ii++)
    for(int k = 0; k < ch1; k++)
      img_tmp[((2 * (ii + pad)) * wd_upsd + (2 * (j + pad))) * ch + k] = 4.f * img_src[((ii)*wd + (j + 1)) * ch + k];

  // right edge
  j = wd;
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(img_src, img_tmp, ht, wd, ch, ch1, pad, wd_upsd, j) \
  schedule(static)
#endif
  for(int ii = 0; ii < ht; ii++)
    for(int k = 0; k < ch1; k++)
      img_tmp[((2 * (ii + pad)) * wd_upsd + (2 * (j + pad))) * ch + k] = 4.f * img_src[((ii)*wd + (j - 1)) * ch + k];

  // corners
  for(int k = 0; k < ch1; k++)
  {
    i = -1;
    j = -1;
    img_tmp[((2 * (i + pad)) * wd_upsd + (2 * (j + pad))) * ch + k] = 4.f * img_src[((i + 1) * wd + (j + 1)) * ch + k];

    j = wd;
    img_tmp[((2 * (i + pad)) * wd_upsd + (2 * (j + pad))) * ch + k] = 4.f * img_src[((i + 1) * wd + (j - 1)) * ch + k];

    i = ht;
    j = -1;
    img_tmp[((2 * (i + pad)) * wd_upsd + (2 * (j + pad))) * ch + k] = 4.f * img_src[((i - 1) * wd + (j + 1)) * ch + k];

    j = wd;
    img_tmp[((2 * (i + pad)) * wd_upsd + (2 * (j + pad))) * ch + k] = 4.f * img_src[((i - 1) * wd + (j - 1)) * ch + k];
  }

  // blur
  _convolve_replicate(img_tmp, wd_upsd, ht_upsd, ch, filter, filter, img_tmp);

  // remove the border and copy result
  if(add_to_image)
  {
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(img_dest, img_tmp, up_ht, up_wd, ch, ch1, img_add_sub, wd_upsd) \
  schedule(static) \
  collapse(2)
#endif
    for(int ii = 0; ii < up_ht; ii++)
      for(int jj = 0; jj < up_wd; jj++)
        for(int k = 0; k < ch1; k++)
          img_dest[(ii * up_wd + jj) * ch + k] = img_add_sub[(ii * up_wd + jj) * ch + k] + img_tmp[((ii + 2) * wd_upsd + (jj + 2)) * ch + k];
  }
  else
  {
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(img_dest, img_tmp, up_ht, up_wd, ch, ch1, img_add_sub, wd_upsd, img_wmap) \
  schedule(static) \
  collapse(2)
#endif
    for(int ii = 0; ii < up_ht; ii++)
      for(int jj = 0; jj < up_wd; jj++)
        for(int k = 0; k < ch1; k++)
          img_dest[(ii * up_wd + jj) * ch + k] += (img_add_sub[(ii * up_wd + jj) * ch + k] - img_tmp[((ii + 2) * wd_upsd + (jj + 2)) * ch + k]) *
                                                    img_wmap[(ii * up_wd + jj)];
  }

  dt_free_align(img_tmp);
}

#define EXPFUSION_PYRAMID_FILTER { .0625, .25, .375, .25, .0625 }
//  const float a = 0.4;
//  const float pyramid_filter[] = { 1. / 4. - a / 2., 1. / 4., a, 1. / 4., 1. / 4. - a / 2. };

static void _build_gaussian_pyramid(const float *const img_src, const size_t wd, const size_t ht, const int ch,
                                    dt_pyramid_t *const pyramid_dest)
{
  // copy image to the finest level
  _image_copy(img_src, wd, ht, ch, pyramid_dest->images[0].img);

  const float pyramid_filter[] = EXPFUSION_PYRAMID_FILTER;

  for(int v = 1; v < pyramid_dest->num_levels; v++)
  {
    // downsample image and store into level
    _downsample_image(pyramid_dest->images[v - 1].img, pyramid_dest->images[v - 1].w, pyramid_dest->images[v - 1].h,
                      pyramid_dest->images[v - 1].ch, pyramid_filter, pyramid_dest->images[v].w, pyramid_dest->images[v].h,
                      pyramid_dest->images[v].img);
  }
}

static void _build_laplacian_pyramid(const float *const img_src, const size_t wd, const size_t ht, const int ch,
                                     dt_pyramid_t *const pyramid_wmap, dt_pyramid_t *const pyramid_dest, const gboolean use_sse)
{
  float *img_tmp2 = dt_alloc_align(64, wd * ht * ch * sizeof(float));
  float *img_tmp3 = dt_alloc_align(64, wd * ht * ch * sizeof(float));

  const float pyramid_filter[] = EXPFUSION_PYRAMID_FILTER;

   _image_copy(img_src, wd, ht, ch, img_tmp3);

  size_t tmp2_wd = wd;
  size_t tmp2_ht = ht;
  size_t tmp3_wd = wd;
  size_t tmp3_ht = ht;

  for(int v = 0; v < pyramid_dest->num_levels - 1; v++)
  {
    // downsample image img_tmp3 and store in img_tmp2
    tmp2_wd = pyramid_dest->images[v + 1].w;
    tmp2_ht = pyramid_dest->images[v + 1].h;

    _downsample_image(img_tmp3, tmp3_wd, tmp3_ht, ch, pyramid_filter, tmp2_wd, tmp2_ht, img_tmp2);

    // upsample image img_tmp2 and subtract from img_tmp3
    _upsample_image(img_tmp2, tmp2_wd, tmp2_ht, ch, pyramid_filter, pyramid_dest->images[v].w, pyramid_dest->images[v].h,
                    img_tmp3, pyramid_dest->images[v].img, FALSE, pyramid_wmap->images[v].img);

    tmp3_wd = tmp2_wd;
    tmp3_ht = tmp2_ht;

    // continue with downsampled image
    _image_copy(img_tmp2, tmp2_ht, tmp2_wd, ch, img_tmp3);
  }

  // coarsest level, residual low pass image
  _images_add_weighted(pyramid_dest->images[pyramid_dest->num_levels - 1].img, tmp3_ht, tmp3_wd, ch, img_tmp3,
                        pyramid_wmap->images[pyramid_dest->num_levels - 1].img, pyramid_dest->images[pyramid_dest->num_levels - 1].img,
                        use_sse);

  dt_free_align(img_tmp2);
  dt_free_align(img_tmp3);
}

static void _reconstruct_laplacian(const dt_pyramid_t *const pyramid, const int ch, float *const img_dest)
{
  const float pyramid_filter[] = EXPFUSION_PYRAMID_FILTER;

  _image_copy(pyramid->images[pyramid->num_levels - 1].img, pyramid->images[pyramid->num_levels - 1].w,
                               pyramid->images[pyramid->num_levels - 1].h, ch, img_dest);

  for(int v = pyramid->num_levels - 2; v >= 0; v--)
  {
    // upsample and add to current level
    _upsample_image(img_dest, pyramid->images[v + 1].w, pyramid->images[v + 1].h, ch, pyramid_filter,
                    pyramid->images[v].w, pyramid->images[v].h, pyramid->images[v].img, img_dest, TRUE, NULL);
  }
}

static inline void _rgb_to_lab(const float *const rgb, float *const lab,
                               const dt_iop_order_iccprofile_info_t *const work_profile)
{
  dt_ioppr_rgb_matrix_to_lab(rgb, lab, work_profile);
}

static inline float _grey_projector(const float *const rgb, const dt_iop_grey_projectors_t grey_projector,
                                    const dt_iop_order_iccprofile_info_t *const work_profile)
{
  float lum = 0.f;

  switch(grey_projector)
  {
    case DT_PROJECTOR_AVERAGE:
      lum = (rgb[0] + rgb[1] + rgb[2]) / 3.f;
      break;
    case DT_PROJECTOR_MIN:
      lum = fmin(fmin(rgb[0], rgb[1]), rgb[2]);
      break;
    case DT_PROJECTOR_MAX:
      lum = fmax(fmax(rgb[0], rgb[1]), rgb[2]);
      break;
    case DT_PROJECTOR_RGB_LUMINANCE:
      lum = (work_profile) ? dt_ioppr_get_rgb_matrix_luminance(rgb, work_profile) : dt_camera_rgb_luminance(rgb);
      break;
    case DT_PROJECTOR_HSL_LIGHTNESS:
      lum = (fmax(fmax(rgb[0], rgb[1]), rgb[2]) + fmin(fmin(rgb[0], rgb[1]), rgb[2])) * .5f;
      break;
    case DT_PROJECTOR_LAB_LIGHTNESS:
    {
      float lab[4] = { 0.f };
      _rgb_to_lab(rgb, lab, work_profile);
      lum = lab[0] * (1.f / 100.f);
    }
    break;
  }

  return lum;
}

static inline float _well_exposedness(const float lum, const float exposure_optimum, const float exposure_width,
                                      const float exposure_left_cutoff, const float exposure_right_cutoff)
{
  if((exposure_left_cutoff > 0.f && lum < exposure_left_cutoff) || (exposure_right_cutoff < 1.f && lum > exposure_right_cutoff))
    return 0.f;

  const float v = (lum - exposure_optimum) / exposure_width;
  const float exp = dt_fast_expf(-(v * v) * .5f);

  return exp;
}

static void _buil_weight_map(const float *const img_src, const size_t wd, const size_t ht, const int ch,
                             float *const img_map, const dt_iop_grey_projectors_t grey_projector,
                             const float exposure_optimum, const float exposure_width,
                             const float exposure_left_cutoff, const float exposure_right_cutoff,
                             const dt_iop_order_iccprofile_info_t *const work_profile)
{
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(img_src, img_map, ht, wd, ch, grey_projector, work_profile, \
  exposure_optimum, exposure_width, exposure_left_cutoff, exposure_right_cutoff) \
  schedule(static) \
  collapse(2)
#endif
  for(size_t y = 0; y < ht; y++)
  {
    for(size_t x = 0; x < wd; x++)
    {
      const float *const rgb = img_src + (y * wd * ch) + x * ch;

      const float lum = powf(_grey_projector(rgb, grey_projector, work_profile), exposure_optimum);
      img_map[y * wd + x] = _well_exposedness(lum, exposure_optimum, exposure_width, exposure_left_cutoff, exposure_right_cutoff);
    }
  }
}

static float exposure_increment(const float stops, const int e)
{
  const float white = exp2f(-stops * (float)e);
  const float scale = 1.0f / white;
  return scale;
}

static void _exposure_fusion(const float *const img_src, const size_t wd, const size_t ht, const int ch,
                             float *const img_dest, struct dt_iop_module_t *self,
                             const dt_iop_order_iccprofile_info_t *const work_profile,
                             dt_iop_fusion_data_t *const d, const gboolean use_sse)
{
  const int num_exposures = d->num_exposures;
  const int num_levels = floor(log2(MIN(wd, ht)));

  // array of images with the weight map for each different exposure image
  dt_image_pyramid_t *img_wmaps = malloc(num_exposures * sizeof(dt_image_pyramid_t));
  for(int i = 0; i < num_exposures; i++) _alloc_image(img_wmaps + i, wd, ht, 1);

  // pyramid with the sum of all weight pyramids
  // used to normalize each weight map
  dt_pyramid_t pyramid_blend = { 0 };
  _alloc_pyramid(&pyramid_blend, wd, ht, ch, num_levels);

  // pyramid for each weight map
  dt_pyramid_t pyramid_wmap = { 0 };
  _alloc_pyramid(&pyramid_wmap, wd, ht, 1, num_levels);

  // build the weight map for each exposure
  for(int n = 0; n < num_exposures; n++)
  {
    if(n > 0)
      _apply_exposure(img_src, wd, ht, ch, exposure_increment(d->exposure_stops, n), img_dest, use_sse);
    else
      _image_copy(img_src, wd, ht, ch, img_dest);

    _buil_weight_map(img_dest, wd, ht, ch, img_wmaps[n].img, d->grey_projector, d->exposure_optimum, d->exposure_width,
                      d->exposure_left_cutoff, d->exposure_right_cutoff, work_profile);
  }

  // normalize the weight maps so the sum for each pixel == 1
  // start with the first one, the sum is stored in img_tmp1
  _image_copy(img_wmaps[0].img, img_wmaps[0].w, img_wmaps[0].h, img_wmaps[0].ch, img_dest);
  // add all the rest
  for(int n = 1; n < num_exposures; n++)
    _images_add(img_dest, img_wmaps[n].w, img_wmaps[n].h, img_wmaps[n].ch, img_wmaps[n].img, img_dest, use_sse);
  // avoid division by zero
  _image_add(img_dest, img_wmaps[0].w, img_wmaps[0].h, img_wmaps[0].ch, 1.0E-12, img_dest, use_sse);
  // normalize all the maps
  for(int n = 0; n < num_exposures; n++)
    _images_div(img_wmaps[n].img, img_wmaps[n].w, img_wmaps[n].h, img_wmaps[n].ch, img_dest, img_wmaps[n].img, use_sse);

  // now create a laplacian pyramid with the weighted sum of the laplacian of each image
  // weighted with the gaussian of the weight maps
  for(int n = 0; n < num_exposures; n++)
  {
    // apply the exposure compensation to the source image (not to the first one)
    if(n > 0)
      _apply_exposure(img_src, wd, ht, ch, exposure_increment(d->exposure_stops, n), img_dest, use_sse);
    else
      _image_copy(img_src, wd, ht, ch, img_dest);

    // transform to the blend colorspace as requested by the user
    {
      int converted_cst = 0;
      dt_ioppr_transform_image_colorspace(self, img_dest, img_dest, wd, ht, iop_cs_rgb, iop_cs_Lab, &converted_cst, work_profile);
    }

    // build a gaussian pyramid for the weight map
    _build_gaussian_pyramid(img_wmaps[n].img, wd, ht, 1, &pyramid_wmap);

    // build a laplacian pyramid for the image
    _build_laplacian_pyramid(img_dest, wd, ht, ch, &pyramid_wmap, &pyramid_blend, use_sse);
  }

  // reconstruct the blended laplacian pyramid
  _reconstruct_laplacian(&pyramid_blend, ch, img_dest);

  // transforn the final image to rgb if needed
  {
    // just transform back to rgb
    int converted_cst = 0;
    dt_ioppr_transform_image_colorspace(self, img_dest, img_dest, wd, ht, iop_cs_Lab, iop_cs_rgb, &converted_cst, work_profile);
  }

  // return the alpha channel
  const size_t size = wd * ht;
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(img_dest, img_src, size, ch) \
  schedule(static)
#endif
  for(size_t i = 0; i < size; i++)
    img_dest[i * ch + 3] = img_src[i * ch + 3];

    // free resources
  _free_pyramid(&pyramid_blend);
  _free_pyramid(&pyramid_wmap);

  for(int i = 0; i < num_exposures; i++) _free_image(img_wmaps + i);
  free(img_wmaps);
}

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_fusion_data_t *const d = (dt_iop_fusion_data_t *)(piece->data);
  const dt_iop_order_iccprofile_info_t *const work_profile = dt_ioppr_get_pipe_work_profile_info(piece->pipe);

  const int ch = 4;
  const size_t width = roi_in->width, height = roi_in->height;

  const float *const in = (const float *const)ivoid;
  float *const out = (float *const)ovoid;

  _exposure_fusion(in, width, height, ch, out, self, work_profile, d, FALSE);
}

#if defined(__SSE__)
void process_sse2(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
                  void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_fusion_data_t *const d = (dt_iop_fusion_data_t *)(piece->data);
  const dt_iop_order_iccprofile_info_t *const work_profile = dt_ioppr_get_pipe_work_profile_info(piece->pipe);

  const int ch = 4;
  const size_t width = roi_in->width, height = roi_in->height;

  const float *const in = (const float *const)ivoid;
  float *const out = (float *const)ovoid;

  _exposure_fusion(in, width, height, ch, out, self, work_profile, d, TRUE);
}
#endif

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
