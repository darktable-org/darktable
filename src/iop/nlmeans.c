/*
    This file is part of darktable,
    Copyright (C) 2011-2023 darktable developers.

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
#include "bauhaus/bauhaus.h"
#include "common/nlmeans_core.h"
#include "common/opencl.h"
#include "control/control.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "develop/imageop_gui.h"
#include "develop/tiling.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"
#include <gtk/gtk.h>
#include <stdlib.h>

// which version of the non-local means code should be used?  0=old (this file), 1=new (src/common/nlmeans_core.c)
#define USE_NEW_IMPL_CL 0

// number of intermediate buffers used by OpenCL code path.  Needs to match value in src/common/nlmeans_core.c
//   to correctly compute tiling
#define NUM_BUCKETS 4

// this is the version of the modules parameters,
// and includes version information about compile-time dt
DT_MODULE_INTROSPECTION(2, dt_iop_nlmeans_params_t)

typedef struct dt_iop_nlmeans_params_t
{
  // these are stored in db.
  float radius;   // $MIN: 0.0 $MAX: 10.0 $DEFAULT: 2.0 $DESCRIPTION: "patch size"
  float strength; // $MIN: 0.0 $MAX: 100000.0 $DEFAULT: 50.0
  float luma;     // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.5
  float chroma;   // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 1.0
} dt_iop_nlmeans_params_t;

typedef struct dt_iop_nlmeans_gui_data_t
{
  GtkWidget *radius;
  GtkWidget *strength;
  GtkWidget *luma;
  GtkWidget *chroma;
} dt_iop_nlmeans_gui_data_t;

typedef dt_iop_nlmeans_params_t dt_iop_nlmeans_data_t;

typedef struct dt_iop_nlmeans_global_data_t
{
  int kernel_nlmeans_init;
  int kernel_nlmeans_dist;
  int kernel_nlmeans_horiz;
  int kernel_nlmeans_vert;
  int kernel_nlmeans_accu;
  int kernel_nlmeans_finish;
} dt_iop_nlmeans_global_data_t;


const char *name()
{
  return _("astrophoto denoise");
}

const char *aliases()
{
  return _("denoise (non-local means)");
}

const char **description(dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("apply a poisson noise removal best suited for astrophotography"),
                                      _("corrective"),
                                      _("non-linear, Lab, display-referred"),
                                      _("non-linear, Lab"),
                                      _("non-linear, Lab, display-referred"));
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
  typedef struct dt_iop_nlmeans_params_v2_t
  {
    float radius;
    float strength;
    float luma;
    float chroma;
  } dt_iop_nlmeans_params_v2_t;

  if(old_version == 1)
  {
    typedef struct dt_iop_nlmeans_params_v1_t
    {
      float luma;
      float chroma;
    } dt_iop_nlmeans_params_v1_t;

    const dt_iop_nlmeans_params_v1_t *o = (dt_iop_nlmeans_params_v1_t *)old_params;
    dt_iop_nlmeans_params_v2_t *n = malloc(sizeof(dt_iop_nlmeans_params_v2_t));

    n->luma = o->luma;
    n->chroma = o->chroma;
    n->strength = 100.0f;
    n->radius = 3;

    *new_params = n;
    *new_params_size = sizeof(dt_iop_nlmeans_params_v2_t);
    *new_version = 2;
    return 0;
  }
  return 1;
}

int default_group()
{
  return IOP_GROUP_CORRECT | IOP_GROUP_TECHNICAL;
}

int flags()
{
  return IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING;
}

#if defined(HAVE_OPENCL) && !USE_NEW_IMPL_CL
static int bucket_next(unsigned int *state, unsigned int max)
{
  unsigned int current = *state;
  unsigned int next = (current >= max - 1 ? 0 : current + 1);

  *state = next;

  return next;
}

int process_cl(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_nlmeans_params_t *d = piece->data;
  dt_iop_nlmeans_global_data_t *gd = self->global_data;
#if USE_NEW_IMPL_CL
  const int width = roi_in->width;
  const int height = roi_in->height;

  const float scale = fminf(roi_in->scale, 2.0f) / fmaxf(piece->iscale, 1.0f);
  const int P = ceilf(d->radius * scale); // pixel filter size
  const int K = ceilf(7 * scale);         // nbhood
  const float sharpness = 3000.0f / (1.0f + d->strength);

  // adjust to Lab, make L more important
  const float max_L = 120.0f, max_C = 512.0f;
  const float nL = 1.0f / max_L, nC = 1.0f / max_C;
  const float norm2[4] = { nL, nC }; //luma and chroma scaling factors

  // allocate a buffer to receive the denoised image
  const int devid = piece->pipe->devid;
  cl_mem dev_U2 = dt_opencl_alloc_device_buffer(devid, sizeof(float) * 4 * width * height);
  if(dev_U2 == NULL)
     return DT_OPENCL_SYSMEM_ALLOCATION;

  const dt_nlmeans_param_t params =
  {
    .scattering = 0,
    .scale = scale,
    .luma = d->luma,
    .chroma = d->chroma,
    .center_weight = -1,
    .sharpness = sharpness,
    .patch_radius = P,
    .search_radius = K,
    .decimate = 0,
    .norm = norm2,
    .pipetype = piece->pipe->type,
    .kernel_init = gd->kernel_nlmeans_init,
    .kernel_dist = gd->kernel_nlmeans_dist,
    .kernel_horiz = gd->kernel_nlmeans_horiz,
    .kernel_vert = gd->kernel_nlmeans_vert,
    .kernel_accu = gd->kernel_nlmeans_accu
  };
  cl_int err = nlmeans_denoise_cl(&params, devid, dev_in, dev_U2, roi_in);
  if(err == CL_SUCCESS)
  {
    // normalize and blend
    const float weight[4] = { d->luma, d->chroma, d->chroma, 1.0f };
    err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_nlmeans_finish, width, height,
      CLARG(dev_in), CLARG(dev_U2), CLARG(dev_out), CLARG(width), CLARG(height), CLARG(weight));
  }
  // clean up and check whether all kernels ran successfully
  dt_opencl_release_mem_object(dev_U2);
  return err;

#else // old code
  const int width = roi_in->width;
  const int height = roi_in->height;

  cl_int err = DT_OPENCL_DEFAULT_ERROR;

  const float scale = fminf(roi_in->scale, 2.0f) / fmaxf(piece->iscale, 1.0f);
  const int P = ceilf(d->radius * scale); // pixel filter size
  const int K = ceilf(7 * scale);         // nbhood
  const float sharpness = 3000.0f / (1.0f + d->strength);

  // adjust to Lab, make L more important
  const float max_L = 120.0f, max_C = 512.0f;
  const float nL = 1.0f / max_L, nC = 1.0f / max_C;
  const float nL2 = nL * nL, nC2 = nC * nC;
  const dt_aligned_pixel_t weight = { d->luma, d->chroma, d->chroma, 1.0f };

  const int devid = piece->pipe->devid;
  cl_mem dev_U2 = dt_opencl_alloc_device_buffer(devid, sizeof(float) * 4 * width * height);
  if(dev_U2 == NULL) goto error;

  cl_mem buckets[NUM_BUCKETS] = { NULL };
  unsigned int state = 0;
  for(int k = 0; k < NUM_BUCKETS; k++)
  {
    buckets[k] = dt_opencl_alloc_device_buffer(devid, sizeof(float) * width * height);
    if(buckets[k] == NULL) goto error;
  }

  int hblocksize;
  dt_opencl_local_buffer_t hlocopt
    = (dt_opencl_local_buffer_t){ .xoffset = 2 * P, .xfactor = 1, .yoffset = 0, .yfactor = 1,
                                  .cellsize = sizeof(float), .overhead = 0,
                                  .sizex = 1 << 16, .sizey = 1 };

  if(dt_opencl_local_buffer_opt(devid, gd->kernel_nlmeans_horiz, &hlocopt))
    hblocksize = hlocopt.sizex;
  else
    hblocksize = 1;

  int vblocksize;
  dt_opencl_local_buffer_t vlocopt
    = (dt_opencl_local_buffer_t){ .xoffset = 1, .xfactor = 1, .yoffset = 2 * P, .yfactor = 1,
                                  .cellsize = sizeof(float), .overhead = 0,
                                  .sizex = 1, .sizey = 1 << 16 };

  if(dt_opencl_local_buffer_opt(devid, gd->kernel_nlmeans_vert, &vlocopt))
    vblocksize = vlocopt.sizey;
  else
    vblocksize = 1;

  size_t sizes[] = { ROUNDUPDWD(width, devid), ROUNDUPDHT(height, devid), 1 };
  size_t sizesl[3];
  size_t local[3];

  dt_opencl_set_kernel_args(devid, gd->kernel_nlmeans_init, 0, CLARG(dev_U2), CLARG(width), CLARG(height));
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_nlmeans_init, sizes);
  if(err != CL_SUCCESS) goto error;


  const size_t bwidth = ROUNDUP(width, hblocksize);
  const size_t bheight = ROUNDUP(height, vblocksize);

  for(int j = -K; j <= 0; j++)
    for(int i = -K; i <= K; i++)
    {
      int q[2] = { i, j };

      cl_mem dev_U4 = buckets[bucket_next(&state, NUM_BUCKETS)];
      dt_opencl_set_kernel_args(devid, gd->kernel_nlmeans_dist, 0, CLARG(dev_in), CLARG(dev_U4), CLARG(width),
        CLARG(height), CLARG(q), CLARG(nL2), CLARG(nC2));
      err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_nlmeans_dist, sizes);
      if(err != CL_SUCCESS) goto error;

      sizesl[0] = bwidth;
      sizesl[1] = ROUNDUPDHT(height, devid);
      sizesl[2] = 1;
      local[0] = hblocksize;
      local[1] = 1;
      local[2] = 1;
      cl_mem dev_U4_t = buckets[bucket_next(&state, NUM_BUCKETS)];
      dt_opencl_set_kernel_args(devid, gd->kernel_nlmeans_horiz, 0, CLARG(dev_U4), CLARG(dev_U4_t), CLARG(width),
        CLARG(height), CLARG(q), CLARG(P), CLLOCAL((hblocksize + 2 * P) * sizeof(float)));
      err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_nlmeans_horiz, sizesl, local);
      if(err != CL_SUCCESS) goto error;


      sizesl[0] = ROUNDUPDWD(width, devid);
      sizesl[1] = bheight;
      sizesl[2] = 1;
      local[0] = 1;
      local[1] = vblocksize;
      local[2] = 1;
      cl_mem dev_U4_tt = buckets[bucket_next(&state, NUM_BUCKETS)];
      dt_opencl_set_kernel_args(devid, gd->kernel_nlmeans_vert, 0, CLARG(dev_U4_t), CLARG(dev_U4_tt),
        CLARG(width), CLARG(height), CLARG(q), CLARG(P), CLARG(sharpness), CLLOCAL((vblocksize + 2 * P) * sizeof(float)));
      err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_nlmeans_vert, sizesl, local);
      if(err != CL_SUCCESS) goto error;


      dt_opencl_set_kernel_args(devid, gd->kernel_nlmeans_accu, 0, CLARG(dev_in), CLARG(dev_U2), CLARG(dev_U4_tt),
        CLARG(width), CLARG(height), CLARG(q));
      err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_nlmeans_accu, sizes);
      if(err != CL_SUCCESS) goto error;

      dt_opencl_finish_sync_pipe(devid, piece->pipe->type);

      // indirectly give gpu some air to breathe (and to do display related stuff)
      dt_iop_nap(dt_opencl_micro_nap(devid));
    }

  // normalize and blend
  dt_opencl_set_kernel_args(devid, gd->kernel_nlmeans_finish, 0, CLARG(dev_in), CLARG(dev_U2), CLARG(dev_out),
    CLARG(width), CLARG(height), CLARG(weight));
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_nlmeans_finish, sizes);

error:
  dt_opencl_release_mem_object(dev_U2);
  for(int k = 0; k < NUM_BUCKETS; k++)
     dt_opencl_release_mem_object(buckets[k]);

  return err;
#endif /* USE_NEW_IMPL_CL */
}
#endif


void tiling_callback(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece,
                     const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out,
                     dt_develop_tiling_t *tiling)
{
  dt_iop_nlmeans_params_t *d = piece->data;
  const int P = ceilf(d->radius * fmin(roi_in->scale, 2.0f) / fmax(piece->iscale, 1.0f)); // pixel filter size
  const int K = ceilf(7 * fmin(roi_in->scale, 2.0f) / fmax(piece->iscale, 1.0f));         // nbhood

  tiling->factor = 2.0f + 1.0f + 0.25 * NUM_BUCKETS; // in + out + tmp
  tiling->maxbuf = 1.0f;
  tiling->overhead = 0;
  tiling->overlap = P + K;
  tiling->xalign = 1;
  tiling->yalign = 1;
  return;
}

void process(
        dt_iop_module_t *self,
        dt_dev_pixelpipe_iop_t *piece,
        const void *const ivoid,
        void *const ovoid,
        const dt_iop_roi_t *const roi_in,
        const dt_iop_roi_t *const roi_out)
{
  // this is called for preview and full pipe separately, each with its own pixelpipe piece.
  // get our data struct:
  const dt_iop_nlmeans_params_t *const d = piece->data;
  if(!dt_iop_have_required_input_format(4 /*we need full-color pixels*/, piece->module, piece->colors,
                                         ivoid, ovoid, roi_in, roi_out))
    return; // image has been copied through to output and module's trouble flag has been updated

  // adjust to zoom size:
  const float scale = fmin(roi_in->scale, 2.0f) / fmax(piece->iscale, 1.0f);
  const int P = ceilf(d->radius * scale); // pixel filter size
  const int K = ceilf(7 * scale);         // nbhood
  const float sharpness = 3000.0f / (1.0f + d->strength);

  // adjust to Lab, make L more important
  float max_L = 120.0f, max_C = 512.0f;
  float nL = 1.0f / max_L, nC = 1.0f / max_C;
  const dt_aligned_pixel_t norm2 = { nL * nL, nC * nC, nC * nC, 1.0f };

  // faster but less accurate processing by skipping half the patches on previews and thumbnails
  int decimate = (piece->pipe->type & (DT_DEV_PIXELPIPE_PREVIEW | DT_DEV_PIXELPIPE_PREVIEW2 | DT_DEV_PIXELPIPE_THUMBNAIL));

  const dt_nlmeans_param_t params = { .scattering = 0,
                                      .scale = scale,
                                      .luma = d->luma,
                                      .chroma = d->chroma,
                                      .center_weight = -1,
                                      .sharpness = sharpness,
                                      .patch_radius = P,
                                      .search_radius = K,
                                      .decimate = decimate,
                                      .norm = norm2 };

  nlmeans_denoise(ivoid, ovoid, roi_in, roi_out, &params);
}

void init_global(dt_iop_module_so_t *self)
{
  const int program = 5; // nlmeans.cl, from programs.conf
  dt_iop_nlmeans_global_data_t *gd = malloc(sizeof(dt_iop_nlmeans_global_data_t));
  self->data = gd;
  gd->kernel_nlmeans_init = dt_opencl_create_kernel(program, "nlmeans_init");
  gd->kernel_nlmeans_dist = dt_opencl_create_kernel(program, "nlmeans_dist");
  gd->kernel_nlmeans_horiz = dt_opencl_create_kernel(program, "nlmeans_horiz");
  gd->kernel_nlmeans_vert = dt_opencl_create_kernel(program, "nlmeans_vert");
  gd->kernel_nlmeans_accu = dt_opencl_create_kernel(program, "nlmeans_accu");
  gd->kernel_nlmeans_finish = dt_opencl_create_kernel(program, "nlmeans_finish");
}

void cleanup_global(dt_iop_module_so_t *self)
{
  dt_iop_nlmeans_global_data_t *gd = self->data;
  dt_opencl_free_kernel(gd->kernel_nlmeans_init);
  dt_opencl_free_kernel(gd->kernel_nlmeans_dist);
  dt_opencl_free_kernel(gd->kernel_nlmeans_horiz);
  dt_opencl_free_kernel(gd->kernel_nlmeans_vert);
  dt_opencl_free_kernel(gd->kernel_nlmeans_accu);
  dt_opencl_free_kernel(gd->kernel_nlmeans_finish);
  free(self->data);
  self->data = NULL;
}

/** commit is the synch point between core and gui, so it copies params to pipe data. */
void commit_params(dt_iop_module_t *self, dt_iop_params_t *params, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_nlmeans_params_t *p = (dt_iop_nlmeans_params_t *)params;
  dt_iop_nlmeans_data_t *d = piece->data;
  memcpy(d, p, sizeof(*d));
  d->luma = MAX(0.0001f, p->luma);
  d->chroma = MAX(0.0001f, p->chroma);
}

void init_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_nlmeans_data_t));
}

void cleanup_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void gui_init(dt_iop_module_t *self)
{
  dt_iop_nlmeans_gui_data_t *g = IOP_GUI_ALLOC(nlmeans);

  g->radius = dt_bauhaus_slider_from_params(self, "radius");
  dt_bauhaus_slider_set_soft_max(g->radius, 4.0f);
  dt_bauhaus_slider_set_digits(g->radius, 0);
  gtk_widget_set_tooltip_text(g->radius, _("radius of the patches to match"));
  g->strength = dt_bauhaus_slider_from_params(self, N_("strength"));
  dt_bauhaus_slider_set_soft_max(g->strength, 100.0f);
  dt_bauhaus_slider_set_digits(g->strength, 0);
  dt_bauhaus_slider_set_format(g->strength, "%");
  gtk_widget_set_tooltip_text(g->strength, _("strength of the effect"));
  g->luma = dt_bauhaus_slider_from_params(self, N_("luma"));
  dt_bauhaus_slider_set_format(g->luma, "%");
  gtk_widget_set_tooltip_text(g->luma, _("how much to smooth brightness"));
  g->chroma = dt_bauhaus_slider_from_params(self, N_("chroma"));
  dt_bauhaus_slider_set_format(g->chroma, "%");
  gtk_widget_set_tooltip_text(g->chroma, _("how much to smooth colors"));
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
