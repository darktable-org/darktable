/*
    This file is part of darktable,
    Copyright (C) 2012-2023 darktable developers.

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
#include "bauhaus/bauhaus.h"
#include "common/bilateral.h"
#include "common/bilateralcl.h"
#include "common/imagebuf.h"
#include "common/locallaplacian.h"
#include "common/locallaplaciancl.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "develop/imageop_gui.h"
#include "develop/tiling.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "iop/iop_api.h"

#include <gtk/gtk.h>
#include <stdlib.h>
#include "gui/accelerators.h"

// this is the version of the modules parameters,
// and includes version information about compile-time dt
DT_MODULE_INTROSPECTION(3, dt_iop_bilat_params_t)

typedef enum dt_iop_bilat_mode_t
{
  s_mode_bilateral = 0,       // $DESCRIPTION: "bilateral grid"
  s_mode_local_laplacian = 1, // $DESCRIPTION: "local laplacian filter"
}
dt_iop_bilat_mode_t;

typedef struct dt_iop_bilat_params_t
{
  dt_iop_bilat_mode_t mode; // $DEFAULT: 1
  float sigma_r; // $MIN: 0.0 $MAX: 100.0 $DEFAULT: 0.5 highlights 100 & range
  float sigma_s; // $MIN: 0.0 $MAX: 100.0 $DEFAULT: 0.5 shadows 100 & spatial 1 100 50
  float detail;  // $MIN: -1.0 $MAX: 4.0 $DEFAULT: 0.25
  float midtone; // $MIN: 0.001 $MAX: 1.0 $DEFAULT: 0.5 $DESCRIPTION: "midtone range"
} dt_iop_bilat_params_t;

typedef dt_iop_bilat_params_t dt_iop_bilat_data_t;

typedef struct dt_iop_bilat_gui_data_t
{
  GtkWidget *highlights;
  GtkWidget *shadows;
  GtkWidget *midtone;
  GtkWidget *spatial;
  GtkWidget *range;
  GtkWidget *detail;
  GtkWidget *mode;
} dt_iop_bilat_gui_data_t;

// this returns a translatable name
const char *name()
{
  return _("local contrast");
}

const char *aliases()
{
  return _("clarity");
}

const char **description(dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("manipulate local and global contrast separately"),
                                      _("creative"),
                                      _("non-linear, Lab, display-referred"),
                                      _("non-linear, Lab"),
                                      _("non-linear, Lab, display-referred"));
}

// some additional flags (self explanatory i think):
int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING;
}

// where does it appear in the gui?
int default_group()
{
  return IOP_GROUP_TONE | IOP_GROUP_EFFECTS;
}

dt_iop_colorspace_type_t default_colorspace(dt_iop_module_t *self,
                                            dt_dev_pixelpipe_t *pipe,
                                            dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_LAB;
}

int legacy_params(dt_iop_module_t *self,
                  const void *const old_params,
                  const int old_version,
                  void **new_params,
                  int32_t *new_params_size,
                  int *new_version)
{
  typedef struct dt_iop_bilat_params_v3_t
  {
    dt_iop_bilat_mode_t mode;
    float sigma_r;
    float sigma_s;
    float detail;
    float midtone;
  } dt_iop_bilat_params_v3_t;

  if(old_version == 1)
  {
    typedef struct dt_iop_bilat_params_v1_t
    {
      float sigma_r;
      float sigma_s;
      float detail;
    } dt_iop_bilat_params_v1_t;

    const dt_iop_bilat_params_v1_t *o = old_params;
    dt_iop_bilat_params_v3_t *n = malloc(sizeof(dt_iop_bilat_params_v3_t));
    n->detail  = o->detail;
    n->sigma_r = o->sigma_r;
    n->sigma_s = o->sigma_s;
    n->midtone = 0.2f;
    n->mode    = s_mode_bilateral;

    *new_params = n;
    *new_params_size = sizeof(dt_iop_bilat_params_v3_t);
    *new_version = 3;
    return 0;
  }

  if(old_version == 2)
  {
    typedef struct dt_iop_bilat_params_v2_t
    {
      uint32_t mode;
      float sigma_r;
      float sigma_s;
      float detail;
    } dt_iop_bilat_params_v2_t;

    const dt_iop_bilat_params_v2_t *o = old_params;
    dt_iop_bilat_params_v3_t *n = malloc(sizeof(dt_iop_bilat_params_v3_t));
    n->detail  = o->detail;
    n->sigma_r = o->sigma_r;
    n->sigma_s = o->sigma_s;
    n->midtone = 0.2f;
    n->mode    = o->mode;

    *new_params = n;
    *new_params_size = sizeof(dt_iop_bilat_params_v3_t);
    *new_version = 3;
    return 0;
  }
  return 1;
}

void init_presets(dt_iop_module_so_t *self)
{
  dt_iop_bilat_params_t p;
  memset(&p, 0, sizeof(p));

  p.mode = s_mode_local_laplacian;
  p.sigma_r = 0.f;
  p.sigma_s = 0.f;
  p.detail = 0.33f;
  p.midtone = 0.5f;

  dt_gui_presets_add_generic(_("clarity"), self->op,
                             self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_SCENE);

  p.mode = s_mode_local_laplacian;
  p.sigma_r = 0.f;
  p.sigma_s = 0.f;
  p.detail = 1.f;
  p.midtone = 0.25f;

  dt_gui_presets_add_generic(_("HDR local tone-mapping"), self->op,
                             self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_SCENE);
}


#ifdef HAVE_OPENCL
int process_cl(dt_iop_module_t *self,
               dt_dev_pixelpipe_iop_t *piece,
               cl_mem dev_in,
               cl_mem dev_out,
               const dt_iop_roi_t *const roi_in,
               const dt_iop_roi_t *const roi_out)
{
  dt_iop_bilat_data_t *d = piece->data;

  cl_int err = DT_OPENCL_PROCESS_CL;
  if(d->mode == s_mode_bilateral)
  {
    // the total scale is composed of scale before input to the pipeline (iscale),
    // and the scale of the roi.
    const float scale = fmaxf(piece->iscale / roi_in->scale, 1.f);
    const float sigma_r = d->sigma_r; // does not depend on scale
    const float sigma_s = d->sigma_s / scale;

    dt_bilateral_cl_t *b
      = dt_bilateral_init_cl(piece->pipe->devid, roi_in->width, roi_in->height,
                             sigma_s, sigma_r);
    if(!b) goto error;
    err = dt_bilateral_splat_cl(b, dev_in);
    if(err != CL_SUCCESS) goto error;
    err = dt_bilateral_blur_cl(b);
    if(err != CL_SUCCESS) goto error;
    err = dt_bilateral_slice_cl(b, dev_in, dev_out, d->detail);
error:
    dt_bilateral_free_cl(b);
    return err;
  }
  else // mode == s_mode_local_laplacian
  {
    dt_local_laplacian_cl_t *b =
      dt_local_laplacian_init_cl(piece->pipe->devid, roi_in->width, roi_in->height,
        d->midtone, d->sigma_s, d->sigma_r, d->detail);
    if(!b) goto error_ll;
    err = dt_local_laplacian_cl(b, dev_in, dev_out);
error_ll:
    if(b) dt_local_laplacian_free_cl(b);
    return err;
  }
}
#endif


void tiling_callback(dt_iop_module_t *self,
                     dt_dev_pixelpipe_iop_t *piece,
                     const dt_iop_roi_t *roi_in,
                     const dt_iop_roi_t *roi_out,
                     dt_develop_tiling_t *tiling)
{
  dt_iop_bilat_data_t *d = piece->data;
  // the total scale is composed of scale before input to the pipeline (iscale),
  // and the scale of the roi.

  if(d->mode == s_mode_bilateral)
  {
    // used to adjuste blur level depending on size. Don't amplify noise if magnified > 100%
    const float scale = fmaxf(piece->iscale / roi_in->scale, 1.f);
    const float sigma_r = d->sigma_r;
    const float sigma_s = d->sigma_s / scale;

    const int width = roi_in->width;
    const int height = roi_in->height;
    const int channels = piece->colors;

    const size_t basebuffer = sizeof(float) * channels * width * height;

    tiling->factor = 2.0f + (float)dt_bilateral_memory_use(width, height, sigma_s, sigma_r) / basebuffer;
    tiling->maxbuf
        = fmax(1.0f, (float)dt_bilateral_singlebuffer_size(width, height, sigma_s, sigma_r) / basebuffer);
    tiling->overhead = 0;
    tiling->overlap = ceilf(4 * sigma_s);
    tiling->xalign = 1;
    tiling->yalign = 1;
  }
  else  // mode == s_mode_local_laplacian
  {
    const int width = roi_in->width;
    const int height = roi_in->height;
    const int channels = piece->colors;

    const size_t basebuffer = sizeof(float) * channels * width * height;
    const int rad = MIN(roi_in->width, ceilf(256 * roi_in->scale / piece->iscale));

    tiling->factor = 2.0f + (float)local_laplacian_memory_use(width, height) / basebuffer;
    tiling->maxbuf
        = fmax(1.0f, (float)local_laplacian_singlebuffer_size(width, height) / basebuffer);
    tiling->overhead = 0;
    tiling->overlap = rad;
    tiling->xalign = 1;
    tiling->yalign = 1;
  }
}

void commit_params(dt_iop_module_t *self,
                   dt_iop_params_t *p1,
                   dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_bilat_params_t *p = (dt_iop_bilat_params_t *)p1;
  dt_iop_bilat_data_t *d = piece->data;
  *d = *p;

#ifdef HAVE_OPENCL
  if(d->mode == s_mode_bilateral)
    piece->process_cl_ready =
      (piece->process_cl_ready && !dt_opencl_avoid_atomics(pipe->devid));
#endif
  if(d->mode == s_mode_local_laplacian)
    piece->process_tiling_ready = FALSE; // can't deal with tiles, sorry.
}


void init_pipe(dt_iop_module_t *self,
               dt_dev_pixelpipe_t *pipe,
               dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1, sizeof(dt_iop_bilat_data_t));
}


void cleanup_pipe(dt_iop_module_t *self,
                  dt_dev_pixelpipe_t *pipe,
                  dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}


void process(dt_iop_module_t *self,
             dt_dev_pixelpipe_iop_t *piece,
             const void *const i, void *const o,
             const dt_iop_roi_t *const roi_in,
             const dt_iop_roi_t *const roi_out)
{
  // this is called for preview and full pipe separately, each with its own pixelpipe piece.
  // get our data struct:
  dt_iop_bilat_data_t *d = piece->data;
  // the total scale is composed of scale before input to the pipeline (iscale),
  // and the scale of the roi.
  // used to adjuste blur level depending on size. Don't amplify noise if magnified > 100%
  const float scale = fmaxf(piece->iscale / roi_in->scale, 1.f);
  const float sigma_r = d->sigma_r; // does not depend on scale
  const float sigma_s = d->sigma_s / scale;

  if(d->mode == s_mode_bilateral)
  {
    dt_bilateral_t *b = dt_bilateral_init(roi_in->width, roi_in->height, sigma_s, sigma_r);
    if(b)
    {
      dt_bilateral_splat(b, (float *)i);
      dt_bilateral_blur(b);
      dt_bilateral_slice(b, (float *)i, (float *)o, d->detail);
      dt_bilateral_free(b);
    }
    else
    {
      // dt_bilateral_init will have spit out an error message.  Now just copy the input to output
      dt_iop_image_copy_by_size(o, i, roi_out->width, roi_out->height, piece->colors);
    }
  }
  else // s_mode_local_laplacian
  {
    local_laplacian(i, o, roi_in->width, roi_in->height,
                    d->midtone, d->sigma_s, d->sigma_r, d->detail, 0);
  }
}

void gui_changed(dt_iop_module_t *self, GtkWidget *w, void *previous)
{
  dt_iop_bilat_gui_data_t *g = self->gui_data;
  dt_iop_bilat_params_t *p = self->params;
  if(w == g->highlights || w == g->shadows || w == g->midtone)
  {
    dt_bauhaus_combobox_set(g->mode, s_mode_local_laplacian);
  }
  else if(w == g->range || w == g->spatial)
  {
    dt_bauhaus_combobox_set(g->mode, s_mode_bilateral);
  }
  else if(w == g->mode)
  {
    if(p->mode == s_mode_local_laplacian)
    {
      p->sigma_r = dt_bauhaus_slider_get(g->highlights);
      p->sigma_s = dt_bauhaus_slider_get(g->shadows);
    }
    else
    {
      p->sigma_r = dt_bauhaus_slider_get(g->range);
      p->sigma_s = dt_bauhaus_slider_get(g->spatial);
    }
  }

  if(!w || w == g->mode)
  {
    gtk_widget_set_visible(g->highlights, p->mode == s_mode_local_laplacian);
    gtk_widget_set_visible(g->shadows, p->mode == s_mode_local_laplacian);
    gtk_widget_set_visible(g->midtone, p->mode == s_mode_local_laplacian);
    gtk_widget_set_visible(g->range, p->mode != s_mode_local_laplacian);
    gtk_widget_set_visible(g->spatial, p->mode != s_mode_local_laplacian);
  }
}

void gui_update(dt_iop_module_t *self)
{
  dt_iop_bilat_gui_data_t *g = self->gui_data;
  dt_iop_bilat_params_t *p = self->params;

  if(p->mode == s_mode_local_laplacian)
  {
    dt_bauhaus_slider_set(g->highlights, p->sigma_r);
    dt_bauhaus_slider_set(g->shadows, p->sigma_s);
    dt_bauhaus_slider_set(g->midtone, p->midtone);
    dt_bauhaus_slider_set(g->range, 20.0f);
    dt_bauhaus_slider_set(g->spatial, 50.0f);
  }
  else
  {
    dt_bauhaus_slider_set(g->range, p->sigma_r);
    dt_bauhaus_slider_set(g->spatial, p->sigma_s);
    dt_bauhaus_slider_set(g->midtone, p->midtone);
    dt_bauhaus_slider_set(g->highlights, 0.5f);
    dt_bauhaus_slider_set(g->shadows, 0.5f);
  }

  gui_changed(self, NULL, NULL);
}

void gui_init(dt_iop_module_t *self)
{
  // init the slider (more sophisticated layouts are possible with gtk tables and boxes):
  dt_iop_bilat_gui_data_t *g = IOP_GUI_ALLOC(bilat);

  g->mode = dt_bauhaus_combobox_from_params(self, N_("mode"));
  gtk_widget_set_tooltip_text
    (g->mode,
     _("the filter used for local contrast enhancement."
       " bilateral is faster but can lead to artifacts around"
       " edges for extreme settings."));

  g->detail = dt_bauhaus_slider_from_params(self, N_("detail"));
  dt_bauhaus_slider_set_offset(g->detail, 100);
  dt_bauhaus_slider_set_format(g->detail, "%");
  gtk_widget_set_tooltip_text(g->detail, _("changes the local contrast"));

  ++darktable.bauhaus->skip_accel;
  g->spatial = dt_bauhaus_slider_from_params(self, "sigma_s");
  g->range = dt_bauhaus_slider_from_params(self, "sigma_r");
  g->highlights = dt_bauhaus_slider_from_params(self, "sigma_r");
  g->shadows = dt_bauhaus_slider_from_params(self, "sigma_s");
  --darktable.bauhaus->skip_accel;

  dt_bauhaus_slider_set_hard_min(g->spatial, 3.0);
  dt_bauhaus_slider_set_default(g->spatial, 50.0);
  dt_bauhaus_slider_set_digits(g->spatial, 0);
  dt_bauhaus_widget_set_label(g->spatial, NULL, N_("coarseness"));
  gtk_widget_set_tooltip_text
    (g->spatial,
     _("feature size of local details (spatial sigma of bilateral filter)"));

  dt_bauhaus_slider_set_hard_min(g->range, 1.0);
  dt_bauhaus_slider_set_default(g->range, 20.0);
  dt_bauhaus_slider_set_digits(g->range, 0);
  dt_bauhaus_widget_set_label(g->range, NULL, N_("contrast"));
  gtk_widget_set_tooltip_text
    (g->range,
     _("L difference to detect edges (range sigma of bilateral filter)"));

  dt_bauhaus_widget_set_label(g->highlights, NULL, N_("highlights"));
  dt_bauhaus_slider_set_hard_max(g->highlights, 2.0);
  dt_bauhaus_slider_set_format(g->highlights, "%");
  gtk_widget_set_tooltip_text(g->highlights,
                              _("changes the local contrast of highlights"));

  dt_bauhaus_widget_set_label(g->shadows, NULL, N_("shadows"));
  dt_bauhaus_slider_set_hard_max(g->shadows, 2.0);
  dt_bauhaus_slider_set_format(g->shadows, "%");
  gtk_widget_set_tooltip_text(g->shadows,
                              _("changes the local contrast of shadows"));

  g->midtone = dt_bauhaus_slider_from_params(self, "midtone");
  dt_bauhaus_slider_set_digits(g->midtone, 3);
  gtk_widget_set_tooltip_text
    (g->midtone,
     _("defines what counts as mid-tones. lower for better dynamic range compression"
       " (reduce shadow and highlight contrast),"
       " increase for more powerful local contrast"));

  // work around multi-instance issue which calls show all a fair bit:
  g_object_set(G_OBJECT(g->highlights), "no-show-all", TRUE, NULL);
  g_object_set(G_OBJECT(g->shadows), "no-show-all", TRUE, NULL);
  g_object_set(G_OBJECT(g->midtone), "no-show-all", TRUE, NULL);
  g_object_set(G_OBJECT(g->range), "no-show-all", TRUE, NULL);
  g_object_set(G_OBJECT(g->spatial), "no-show-all", TRUE, NULL);

}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
