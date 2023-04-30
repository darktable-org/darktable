/*
   This file is part of darktable,
   Copyright (C) 2010-2023 darktable developers.

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
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "bauhaus/bauhaus.h"
#include "common/box_filters.h"
#include "common/bspline.h"
#include "common/opencl.h"
#include "common/imagebuf.h"
#include "common/fast_guided_filter.h"
#include "common/distance_transform.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "develop/imageop_gui.h"
#include "develop/noise_generator.h"
#include "develop/tiling.h"
#include "develop/masks.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"

#include <gtk/gtk.h>
#include <inttypes.h>

// Downsampling factor for guided-laplacian
#define DS_FACTOR 4
#define MAX_NUM_SCALES 12

DT_MODULE_INTROSPECTION(4, dt_iop_highlights_params_t)

/* As some of the internal algorithms use a smaller value for clipping than given by the UI
   the visualizing is wrong for those algos. It seems to be a a minor issue but sometimes significant.
   Please note, every mode defined in dt_iop_highlights_mode_t requires a value.
*/
static float highlights_clip_magics[6] = { 1.0f, 1.0f, 0.987f, 0.995f, 0.987f, 0.987f };

typedef enum dt_iop_highlights_mode_t
{
  DT_IOP_HIGHLIGHTS_OPPOSED = 5,  // $DESCRIPTION: "inpaint opposed"
  DT_IOP_HIGHLIGHTS_LCH = 1,     // $DESCRIPTION: "reconstruct in LCh"
  DT_IOP_HIGHLIGHTS_CLIP = 0,    // $DESCRIPTION: "clip highlights"
  DT_IOP_HIGHLIGHTS_SEGMENTS = 4, // $DESCRIPTION: "segmentation based"
  DT_IOP_HIGHLIGHTS_LAPLACIAN = 3, //$DESCRIPTION: "guided laplacians"
  DT_IOP_HIGHLIGHTS_INPAINT = 2, // $DESCRIPTION: "reconstruct color"
} dt_iop_highlights_mode_t;

typedef enum dt_atrous_wavelets_scales_t
{
  WAVELETS_1_SCALE = 0,   // $DESCRIPTION: "2 px"
  WAVELETS_2_SCALE = 1,   // $DESCRIPTION: "4 px"
  WAVELETS_3_SCALE = 2,   // $DESCRIPTION: "8 px"
  WAVELETS_4_SCALE = 3,   // $DESCRIPTION: "16 px"
  WAVELETS_5_SCALE = 4,   // $DESCRIPTION: "32 px"
  WAVELETS_6_SCALE = 5,   // $DESCRIPTION: "64 px"
  WAVELETS_7_SCALE = 6,   // $DESCRIPTION: "128 px (slow)"
  WAVELETS_8_SCALE = 7,   // $DESCRIPTION: "256 px (slow)"
  WAVELETS_9_SCALE = 8,   // $DESCRIPTION: "512 px (very slow)"
  WAVELETS_10_SCALE = 9,  // $DESCRIPTION: "1024 px (very slow)"
  WAVELETS_11_SCALE = 10, // $DESCRIPTION: "2048 px (insanely slow)"
  WAVELETS_12_SCALE = 11, // $DESCRIPTION: "4096 px (insanely slow)"
} dt_atrous_wavelets_scales_t;

typedef enum dt_recovery_mode_t
{
  DT_RECOVERY_MODE_OFF = 0,    // $DESCRIPTION: "off"
  DT_RECOVERY_MODE_ADAPT = 5,  // $DESCRIPTION: "generic"
  DT_RECOVERY_MODE_ADAPTF = 6, // $DESCRIPTION: "flat generic"
  DT_RECOVERY_MODE_SMALL = 1,  // $DESCRIPTION: "small segments"
  DT_RECOVERY_MODE_LARGE = 2,  // $DESCRIPTION: "large segments"
  DT_RECOVERY_MODE_SMALLF = 3, // $DESCRIPTION: "flat small segments"
  DT_RECOVERY_MODE_LARGEF = 4, // $DESCRIPTION: "flat large segments"
} dt_recovery_mode_t;
#define NUM_RECOVERY_MODES 7

typedef enum dt_highlights_mask_t
{
  DT_HIGHLIGHTS_MASK_OFF,
  DT_HIGHLIGHTS_MASK_COMBINE,
  DT_HIGHLIGHTS_MASK_CANDIDATING,
  DT_HIGHLIGHTS_MASK_STRENGTH,
  DT_HIGHLIGHTS_MASK_CLIPPED
} dt_highlights_mask_t;

typedef struct dt_iop_highlights_params_t
{
  // params of v1
  dt_iop_highlights_mode_t mode; // $DEFAULT: DT_IOP_HIGHLIGHTS_CLIP $DESCRIPTION: "method"
  float blendL; // unused $DEFAULT: 1.0
  float blendC; // unused $DEFAULT: 0.0
  float strength; // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "strength"
  // params of v2
  float clip; // $MIN: 0.0 $MAX: 2.0 $DEFAULT: 1.0 $DESCRIPTION: "clipping threshold"
  // params of v3
  float noise_level; // $MIN: 0. $MAX: 0.5 $DEFAULT: 0.00 $DESCRIPTION: "noise level"
  int iterations; // $MIN: 1 $MAX: 256 $DEFAULT: 30 $DESCRIPTION: "iterations"
  dt_atrous_wavelets_scales_t scales; // $DEFAULT: WAVELETS_7_SCALE $DESCRIPTION: "diameter of reconstruction"
  float candidating; // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.4 $DESCRIPTION: "candidating"
  float combine;     // $MIN: 0.0 $MAX: 8.0 $DEFAULT: 2.0 $DESCRIPTION: "combine"
  dt_recovery_mode_t recovery; // $DEFAULT: DT_RECOVERY_MODE_OFF $DESCRIPTION: "rebuild"
  // params of v4
  float solid_color; // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "inpaint a flat color"
} dt_iop_highlights_params_t;

typedef struct dt_iop_highlights_gui_data_t
{
  GtkWidget *clip;
  GtkWidget *mode;
  GtkWidget *noise_level;
  GtkWidget *iterations;
  GtkWidget *scales;
  GtkWidget *solid_color;
  GtkWidget *candidating;
  GtkWidget *combine;
  GtkWidget *recovery;
  GtkWidget *strength;
  dt_highlights_mask_t hlr_mask_mode;
} dt_iop_highlights_gui_data_t;

typedef dt_iop_highlights_params_t dt_iop_highlights_data_t;

typedef struct dt_iop_highlights_global_data_t
{
  int kernel_highlights_1f_clip;
  int kernel_highlights_1f_lch_bayer;
  int kernel_highlights_1f_lch_xtrans;
  int kernel_highlights_4f_clip;
  int kernel_highlights_bilinear_and_mask;
  int kernel_highlights_remosaic_and_replace;
  int kernel_highlights_guide_laplacians;
  int kernel_highlights_diffuse_color;
  int kernel_highlights_box_blur;

  int kernel_highlights_opposed;
  int kernel_highlights_initmask;
  int kernel_highlights_dilatemask;
  int kernel_highlights_chroma;

  int kernel_highlights_false_color;

  int kernel_filmic_bspline_vertical;
  int kernel_filmic_bspline_horizontal;
  int kernel_filmic_wavelets_detail;

  int kernel_interpolate_bilinear;
} dt_iop_highlights_global_data_t;


const char *name()
{
  return _("highlight reconstruction");
}

const char **description(struct dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("avoid magenta highlights and try to recover highlights colors"),
                                      _("corrective"),
                                      _("linear, raw, scene-referred"),
                                      _("reconstruction, raw"),
                                      _("linear, raw, scene-referred"));
}

int default_group()
{
  return IOP_GROUP_BASIC | IOP_GROUP_TECHNICAL;
}

int flags()
{
  return IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_ONE_INSTANCE;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RAW;
}

int legacy_params(dt_iop_module_t *self,
                  const void *const old_params,
                  const int old_version,
                  void *new_params,
                  const int new_version)
{
  if(old_version == 1 && new_version == 4)
  {
    /*
      params of v2 :
        float clip
      + params of v3
      + params of v4
    */
    memcpy(new_params, old_params, sizeof(dt_iop_highlights_params_t) - 5 * sizeof(float) - 2 * sizeof(int) - sizeof(dt_atrous_wavelets_scales_t));
    dt_iop_highlights_params_t *n = (dt_iop_highlights_params_t *)new_params;
    n->clip = 1.0f;
    n->noise_level = 0.0f;
    n->candidating = 0.4f;
    n->combine = 2.f;
    n->recovery = DT_RECOVERY_MODE_OFF;
    n->iterations = 1;
    n->scales = 5;
    n->solid_color = 0.f;
    n->strength = 0.0f;
    return 0;
  }
  if(old_version == 2 && new_version == 4)
  {
    /*
      params of v3 :
        float noise_level;
        int iterations;
        dt_atrous_wavelets_scales_t scales;
        float candidating;
        float combine;
        int recovery;
      + params of v4
    */
    memcpy(new_params, old_params, sizeof(dt_iop_highlights_params_t) - 4 * sizeof(float) - 2 * sizeof(int) - sizeof(dt_atrous_wavelets_scales_t));
    dt_iop_highlights_params_t *n = (dt_iop_highlights_params_t *)new_params;
    n->noise_level = 0.0f;
    n->candidating = 0.4f;
    n->combine = 2.f;
    n->recovery = DT_RECOVERY_MODE_OFF;
    n->iterations = 1;
    n->scales = 5;
    n->solid_color = 0.f;
    n->strength = 0.0f;
    return 0;
  }
  if(old_version == 3 && new_version == 4)
  {
    /*
      params of v4 :
        float solid_color;
    */
    memcpy(new_params, old_params, sizeof(dt_iop_highlights_params_t) - sizeof(float));
    dt_iop_highlights_params_t *n = (dt_iop_highlights_params_t *)new_params;
    n->solid_color = 0.f;
    n->strength = 0.0f;
    return 0;
  }

  return 1;
}

static dt_aligned_pixel_t img_oppchroma;
static uint64_t img_opphash = 0;

#include "hlreconstruct/segmentation.c"
#include "hlreconstruct/segbased.c"
#include "hlreconstruct/opposed.c"
#include "hlreconstruct/laplacian.c"
#include "hlreconstruct/inpaint.c"
#include "hlreconstruct/lch.c"

/* inpaint opposed and segmentation based algorithms want the whole image for proper calculation
   of chrominance correction and best candidates so we change both rois.
*/
void modify_roi_out(dt_iop_module_t *self,
                    dt_dev_pixelpipe_iop_t *piece,
                    dt_iop_roi_t *roi_out,
                    const dt_iop_roi_t *const roi_in)
{
  *roi_out = *roi_in;
  // it can never hurt to make sure
  roi_out->x = MAX(0, roi_in->x);
  roi_out->y = MAX(0, roi_in->y);
}

void modify_roi_in(dt_iop_module_t *self,
                   dt_dev_pixelpipe_iop_t *piece,
                   const dt_iop_roi_t *const roi_out,
                   dt_iop_roi_t *roi_in)
{
  *roi_in = *roi_out;

  dt_iop_highlights_data_t *d = (dt_iop_highlights_data_t *)piece->data;
  const gboolean use_opposing = (d->mode == DT_IOP_HIGHLIGHTS_OPPOSED) || (d->mode == DT_IOP_HIGHLIGHTS_SEGMENTS);
  /* When do we need to expand the roi to maximum of the full input data?
     1. Certainly not if any other than opposed or the segmentation based algo is used.
   */
  if(!use_opposing)
    return;

  /*
    2. Certainly not for linear raws as they miss the automatic downscaler provided by the demosaicer stage, so
       the expanding to full image data does not work as we do a downscaling very early in
       the pixelpipe. So - no quality achieved but really bad performance.
       See #12998 and #12993 for a lengthy discussion
  */
  if(piece->pipe->dsc.filters == 0)
    return;

  /* We require the correct (full-image-data) expansion with a defined scale for all pixelpipes for proper
     aligning and scaling in the demosiacer
  */
  roi_in->x = 0;
  roi_in->y = 0;
  roi_in->width = piece->buf_in.width;
  roi_in->height = piece->buf_in.height;
  roi_in->scale = 1.0f;
}

void tiling_callback(struct dt_iop_module_t *self,
                     struct dt_dev_pixelpipe_iop_t *piece,
                     const dt_iop_roi_t *roi_in,
                     const dt_iop_roi_t *roi_out,
                     struct dt_develop_tiling_t *tiling)
{
  dt_iop_highlights_data_t *d = (dt_iop_highlights_data_t *)piece->data;
  const uint32_t filters = piece->pipe->dsc.filters;

  const gboolean is_bayer = filters && (filters != 9u);
  const gboolean is_xtrans = filters && (filters == 9u);

  tiling->xalign = is_xtrans ? 3 : 2;
  tiling->yalign = is_xtrans ? 3 : 2;
  tiling->overlap = 0;

  if(d->mode == DT_IOP_HIGHLIGHTS_LAPLACIAN && is_bayer)
  {
    // Bayer CFA and guided laplacian method : prepare for wavelets decomposition.
    const float scale = fmaxf(DS_FACTOR * piece->iscale / roi_in->scale, 1.f);
    const float final_radius = (float)((int)(1 << d->scales)) / scale;
    const int scales = CLAMP((int)ceilf(log2f(final_radius)), 1, MAX_NUM_SCALES);
    const int max_filter_radius = (1 << scales);

    tiling->factor = 2.f + 2.f * 4 + 6.f * 4 / (DS_FACTOR * DS_FACTOR);
    tiling->factor_cl =  2.f + 3.f * 4 + 5.f * 4 / (DS_FACTOR * DS_FACTOR);

    // The wavelets decomposition uses a temp buffer of size 4 × ds_width
    tiling->maxbuf = 1.f / roi_in->height * dt_get_num_threads() * 4.f / DS_FACTOR;

    // No temp buffer on GPU
    tiling->maxbuf_cl = 1.0f;
    tiling->overhead = 0;

    // Note : if we were not doing anything iterative,
    // max_filter_radius would not need to be factored more.
    // Since we are iterating within tiles, we need more padding.
    // The clean way of doing it would be an internal tiling mechanism
    // where we restitch the tiles between each new iteration.
    tiling->overlap = max_filter_radius * 1.5f / DS_FACTOR;
    return;
  }

  if(d->mode == DT_IOP_HIGHLIGHTS_SEGMENTS)
  {
    // even if the algorithm can't tile we want to calculate memory for pixelpipe checks and a possible warning
    const int segments = roi_out->width * roi_out->height / 4000; // segments per mpix
    tiling->overhead = segments * 5 * 5 * sizeof(int); // segmentation stuff
    tiling->factor = 3.0f;
    tiling->maxbuf = 1.0f;
    return;
  }

  if(d->mode == DT_IOP_HIGHLIGHTS_OPPOSED)
  {
    tiling->factor = 2.5f; // enough for in&output buffers plus masks
    tiling->maxbuf = 1.0f;
    tiling->overhead = 0;
    return;
  }

  tiling->factor = 2.0f;  // in + out
  tiling->maxbuf = 1.0f;
  tiling->overhead = 0;

  if(d->mode == DT_IOP_HIGHLIGHTS_LCH)
  {
    tiling->xalign = is_xtrans ? 6 : 2;
    tiling->yalign = is_xtrans ? 6 : 2;
    tiling->overlap = is_xtrans ? 2 : 1;
  }
}

#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self,
               dt_dev_pixelpipe_iop_t *piece,
               cl_mem dev_in,
               cl_mem dev_out,
               const dt_iop_roi_t *const roi_in,
               const dt_iop_roi_t *const roi_out)
{
  dt_iop_highlights_data_t *d = (dt_iop_highlights_data_t *)piece->data;
  dt_iop_highlights_gui_data_t *g = (dt_iop_highlights_gui_data_t *)self->gui_data;
  dt_iop_highlights_global_data_t *gd = (dt_iop_highlights_global_data_t *)self->global_data;

  const uint32_t filters = piece->pipe->dsc.filters;
  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;

  const gboolean fullpipe = piece->pipe->type & DT_DEV_PIXELPIPE_FULL;

  cl_int err = DT_OPENCL_DEFAULT_ERROR;
  cl_mem dev_xtrans = NULL;
  cl_mem dev_clips = NULL;

  if(g && fullpipe)
  {
    if(g->hlr_mask_mode != DT_HIGHLIGHTS_MASK_OFF)
    {
      piece->pipe->mask_display = DT_DEV_PIXELPIPE_DISPLAY_PASSTHRU;
      if(g->hlr_mask_mode == DT_HIGHLIGHTS_MASK_CLIPPED)
      {
        const float mclip = d->clip * highlights_clip_magics[d->mode];
        const float *c = piece->pipe->dsc.temperature.coeffs;
        float clips[4] = { mclip * (c[RED]   <= 0.0f ? 1.0f : c[RED]),
                           mclip * (c[GREEN] <= 0.0f ? 1.0f : c[GREEN]),
                           mclip * (c[BLUE]  <= 0.0f ? 1.0f : c[BLUE]),
                           mclip * (c[GREEN] <= 0.0f ? 1.0f : c[GREEN]) };

        dev_clips = dt_opencl_copy_host_to_device_constant(devid, 4 * sizeof(float), clips);
        if(dev_clips == NULL) goto error;

        dev_xtrans = dt_opencl_copy_host_to_device_constant(devid, sizeof(piece->pipe->dsc.xtrans), piece->pipe->dsc.xtrans);
        if(dev_xtrans == NULL) goto error;

        size_t sizes[] = { ROUNDUPDWD(roi_out->width, devid), ROUNDUPDHT(roi_out->height, devid), 1 };
        dt_opencl_set_kernel_args(devid, gd->kernel_highlights_false_color, 0, CLARG(dev_in), CLARG(dev_out),
          CLARG(roi_out->width), CLARG(roi_out->height), CLARG(roi_in->width), CLARG(roi_in->height),
          CLARG(roi_out->x), CLARG(roi_out->y), CLARG(filters), CLARG(dev_xtrans),
          CLARG(dev_clips));

        err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_highlights_false_color, sizes);
        if(err != CL_SUCCESS) goto error;

        dt_opencl_release_mem_object(dev_clips);
        dt_opencl_release_mem_object(dev_xtrans);
        return TRUE;
      }
    }
  }

  const float clip = d->clip
                     * fminf(piece->pipe->dsc.processed_maximum[0],
                             fminf(piece->pipe->dsc.processed_maximum[1], piece->pipe->dsc.processed_maximum[2]));

  if(!filters)
  {
    // non-raw images use dedicated kernel which just clips
    err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_highlights_4f_clip, width, height,
      CLARG(dev_in), CLARG(dev_out), CLARG(width), CLARG(height), CLARG(d->mode), CLARG(clip));
    if(err != CL_SUCCESS) goto error;
  }
  else if(d->mode == DT_IOP_HIGHLIGHTS_OPPOSED)
  {
    err = process_opposed_cl(self, piece, dev_in, dev_out, roi_in, roi_out);
    if(err != CL_SUCCESS) goto error;
  }
  else if(d->mode == DT_IOP_HIGHLIGHTS_LCH && filters != 9u)
  {
    // bayer sensor raws with LCH mode
    err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_highlights_1f_lch_bayer, width, height,
      CLARG(dev_in), CLARG(dev_out), CLARG(width), CLARG(height), CLARG(clip), CLARG(roi_out->x), CLARG(roi_out->y),
      CLARG(filters));
    if(err != CL_SUCCESS) goto error;
  }
  else if(d->mode == DT_IOP_HIGHLIGHTS_LCH && filters == 9u)
  {
    // xtrans sensor raws with LCH mode
    int blocksizex, blocksizey;

    dt_opencl_local_buffer_t locopt
      = (dt_opencl_local_buffer_t){ .xoffset = 2 * 2, .xfactor = 1, .yoffset = 2 * 2, .yfactor = 1,
                                    .cellsize = sizeof(float), .overhead = 0,
                                    .sizex = 1 << 8, .sizey = 1 << 8 };

    if(dt_opencl_local_buffer_opt(devid, gd->kernel_highlights_1f_lch_xtrans, &locopt))
    {
      blocksizex = locopt.sizex;
      blocksizey = locopt.sizey;
    }
    else
      blocksizex = blocksizey = 1;

    dev_xtrans
        = dt_opencl_copy_host_to_device_constant(devid, sizeof(piece->pipe->dsc.xtrans), piece->pipe->dsc.xtrans);
    if(dev_xtrans == NULL) goto error;

    size_t sizes[] = { ROUNDUP(width, blocksizex), ROUNDUP(height, blocksizey), 1 };
    size_t local[] = { blocksizex, blocksizey, 1 };
    dt_opencl_set_kernel_args(devid, gd->kernel_highlights_1f_lch_xtrans, 0, CLARG(dev_in), CLARG(dev_out),
      CLARG(width), CLARG(height), CLARG(clip), CLARG(roi_out->x), CLARG(roi_out->y), CLARG(dev_xtrans),
      CLLOCAL(sizeof(float) * (blocksizex + 4) * (blocksizey + 4)));

    err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_highlights_1f_lch_xtrans, sizes, local);
    if(err != CL_SUCCESS) goto error;
  }
  else if(d->mode == DT_IOP_HIGHLIGHTS_LAPLACIAN)
  {
    const dt_aligned_pixel_t clips = {  0.995f * d->clip * piece->pipe->dsc.processed_maximum[0],
                                        0.995f * d->clip * piece->pipe->dsc.processed_maximum[1],
                                        0.995f * d->clip * piece->pipe->dsc.processed_maximum[2], clip };
    err = process_laplacian_bayer_cl(self, piece, dev_in, dev_out, roi_in, roi_out, clips);
    if(err != CL_SUCCESS) goto error;
  }
  else // (d->mode == DT_IOP_HIGHLIGHTS_CLIP)
  {
    // raw images with clip mode (both bayer and xtrans)
    err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_highlights_1f_clip, width, height,
      CLARG(dev_in), CLARG(dev_out), CLARG(width), CLARG(height), CLARG(clip), CLARG(roi_out->x), CLARG(roi_out->y),
      CLARG(filters));
    if(err != CL_SUCCESS) goto error;
  }

  // update processed maximum
  if((d->mode != DT_IOP_HIGHLIGHTS_LAPLACIAN) && (d->mode != DT_IOP_HIGHLIGHTS_OPPOSED))
  {
    // The guided laplacian and opposed are the modes that keeps signal scene-referred and don't clip highlights to 1
    // For the other modes, we need to notify the pipeline that white point has changed
    const float m = fmaxf(fmaxf(piece->pipe->dsc.processed_maximum[0], piece->pipe->dsc.processed_maximum[1]),
                          piece->pipe->dsc.processed_maximum[2]);
    for(int k = 0; k < 3; k++) piece->pipe->dsc.processed_maximum[k] = m;
  }

  dt_opencl_release_mem_object(dev_xtrans);
  return TRUE;

  error:
  dt_opencl_release_mem_object(dev_clips);
  dt_opencl_release_mem_object(dev_xtrans);
  dt_print(DT_DEBUG_OPENCL, "[opencl_highlights] [%s] had error %s\n", dt_dev_pixelpipe_type_to_str(piece->pipe->type), cl_errstr(err));
  return FALSE;
}
#endif

static void process_clip(dt_dev_pixelpipe_iop_t *piece,
                         const void *const ivoid,
                         void *const ovoid,
                         const dt_iop_roi_t *const roi_in,
                         const dt_iop_roi_t *const roi_out,
                         const float clip)
{
  const float *const in = (const float *const)ivoid;
  float *const out = (float *const)ovoid;

  const int ch = (piece->pipe->dsc.filters) ? piece->colors : 1;
  const size_t msize = (size_t)roi_out->width * roi_out->height * ch; 
#ifdef _OPENMP
#pragma omp parallel for SIMD() default(none) \
    dt_omp_firstprivate(clip, in, out, msize) \
    schedule(static)
#endif
  for(size_t k = 0; k < msize; k++)
    out[k] = fminf(clip, in[k]);
}

static void process_visualize(dt_dev_pixelpipe_iop_t *piece,
                              const void *const ivoid,
                              void *const ovoid,
                              const dt_iop_roi_t *const roi_in,
                              const dt_iop_roi_t *const roi_out,
                              dt_iop_highlights_data_t *data)
{
  const uint8_t(*const xtrans)[6] = (const uint8_t(*const)[6])piece->pipe->dsc.xtrans;
  const uint32_t filters = piece->pipe->dsc.filters;
  const gboolean is_xtrans = (filters == 9u);
  const gboolean is_linear = (filters == 0);
  const float *const in = (const float *const)ivoid;
  float *const out = (float *const)ovoid;

  const float mclip = data->clip * highlights_clip_magics[data->mode];
  const float *cf = piece->pipe->dsc.temperature.coeffs;
  const float clips[4] = { mclip * (cf[RED]   <= 0.0f ? 1.0f : cf[RED]),
                           mclip * (cf[GREEN] <= 0.0f ? 1.0f : cf[GREEN]),
                           mclip * (cf[BLUE]  <= 0.0f ? 1.0f : cf[BLUE]),
                           mclip * (cf[GREEN] <= 0.0f ? 1.0f : cf[GREEN]) };

  if(is_linear)
  {
    const size_t npixels = roi_out->width * (size_t)roi_out->height;
#ifdef _OPENMP
#pragma omp parallel for default(none) \
    dt_omp_firstprivate(in, out, clips, npixels) \
    schedule(static)
#endif
    for(size_t k = 0; k < 4*npixels; k += 4)
    {
      for_each_channel(c)
        out[k+c] = (in[k+c] < clips[c]) ? 0.2f * in[k+c] : 1.0f;
      out[k+3] = 0.0f;
    }
  }
  else
  {
#ifdef _OPENMP
  #pragma omp parallel for default(none) \
  dt_omp_firstprivate(in, out, clips, roi_in, roi_out, filters, xtrans, is_xtrans) \
  schedule(static)
#endif
    for(int row = 0; row < roi_out->height; row++)
    {
      for(int col = 0; col < roi_out->width; col++)
      {
        const size_t ox = (size_t)row * roi_out->width + col;
        const int irow = row + roi_out->y;
        const int icol = col + roi_out->x;
        const size_t ix = (size_t)irow * roi_in->width + icol;
        if((icol >= 0) && (irow >= 0) && (irow < roi_in->height) && (icol < roi_in->width))
        {
          const int c = is_xtrans ? FCxtrans(irow, icol, roi_in, xtrans) : FC(irow, icol, filters);
          const float ival = in[ix];
          out[ox] = (ival < clips[c]) ? 0.2f * ival : 1.0f;
        }
        else
          out[ox] = 0.0f;
      }
    }
  }
}

void process(struct dt_iop_module_t *self,
             dt_dev_pixelpipe_iop_t *piece,
             const void *const ivoid,
             void *const ovoid,
             const dt_iop_roi_t *const roi_in,
             const dt_iop_roi_t *const roi_out)
{
  const uint32_t filters = piece->pipe->dsc.filters;
  dt_iop_highlights_data_t *data = (dt_iop_highlights_data_t *)piece->data;
  dt_iop_highlights_gui_data_t *g = (dt_iop_highlights_gui_data_t *)self->gui_data;

  const gboolean fullpipe = piece->pipe->type & DT_DEV_PIXELPIPE_FULL;
  if(g && fullpipe)
  {
    if(g->hlr_mask_mode != DT_HIGHLIGHTS_MASK_OFF)
    {
      piece->pipe->mask_display = DT_DEV_PIXELPIPE_DISPLAY_PASSTHRU;
      if(g->hlr_mask_mode == DT_HIGHLIGHTS_MASK_CLIPPED)
      {
        process_visualize(piece, ivoid, ovoid, roi_in, roi_out, data);
        return;
      }
    }
  }

  /* While rendering thumnbnails we look for an acceptable lower quality */
  gboolean high_quality = TRUE;
  if(piece->pipe->type & DT_DEV_PIXELPIPE_THUMBNAIL)
  {
    const int level = dt_mipmap_cache_get_matching_size(darktable.mipmap_cache, piece->pipe->final_width, piece->pipe->final_height);
    const char *min = dt_conf_get_string_const("plugins/lighttable/thumbnail_hq_min_level");
    const dt_mipmap_size_t min_s = dt_mipmap_cache_get_min_mip_from_pref(min);
    high_quality = (level >= min_s);
  }

  const float clip
      = data->clip * fminf(piece->pipe->dsc.processed_maximum[0],
                           fminf(piece->pipe->dsc.processed_maximum[1], piece->pipe->dsc.processed_maximum[2]));

  if(filters == 0)
  {
    if(data->mode == DT_IOP_HIGHLIGHTS_CLIP)
    {
      process_clip(piece, ivoid, ovoid, roi_in, roi_out, clip);
      for(int k=0;k<3;k++)
        piece->pipe->dsc.processed_maximum[k]
          = fminf(piece->pipe->dsc.processed_maximum[0],
                  fminf(piece->pipe->dsc.processed_maximum[1], piece->pipe->dsc.processed_maximum[2]));
    }
    else
    {
      _process_linear_opposed(self, piece, ivoid, ovoid, roi_in, roi_out, high_quality);
    }
    return;
  }

  switch(data->mode)
  {
    case DT_IOP_HIGHLIGHTS_INPAINT: // a1ex's (magiclantern) idea of color inpainting:
    {
      const float clips[4] = { 0.987 * data->clip * piece->pipe->dsc.processed_maximum[0],
                               0.987 * data->clip * piece->pipe->dsc.processed_maximum[1],
                               0.987 * data->clip * piece->pipe->dsc.processed_maximum[2], clip };

      if(filters == 9u)
      {
        const uint8_t(*const xtrans)[6] = (const uint8_t(*const)[6])piece->pipe->dsc.xtrans;
#ifdef _OPENMP
#pragma omp parallel for default(none) \
        dt_omp_firstprivate(clips, filters, ivoid, ovoid, roi_in, roi_out, xtrans) \
        schedule(static)
#endif
        for(int j = 0; j < roi_out->height; j++)
        {
          interpolate_color_xtrans(ivoid, ovoid, roi_in, roi_out, 0, 1, j, clips, xtrans, 0);
          interpolate_color_xtrans(ivoid, ovoid, roi_in, roi_out, 0, -1, j, clips, xtrans, 1);
        }
#ifdef _OPENMP
#pragma omp parallel for default(none) \
        dt_omp_firstprivate(clips, filters, ivoid, ovoid, roi_in, roi_out, xtrans) \
        schedule(static)
#endif
        for(int i = 0; i < roi_out->width; i++)
        {
          interpolate_color_xtrans(ivoid, ovoid, roi_in, roi_out, 1, 1, i, clips, xtrans, 2);
          interpolate_color_xtrans(ivoid, ovoid, roi_in, roi_out, 1, -1, i, clips, xtrans, 3);
        }
      }
      else
      {
#ifdef _OPENMP
#pragma omp parallel for default(none) \
        dt_omp_firstprivate(clips, filters, ivoid, ovoid, roi_out) \
        shared(data, piece) \
        schedule(static)
#endif
        for(int j = 0; j < roi_out->height; j++)
        {
          interpolate_color(ivoid, ovoid, roi_out, 0, 1, j, clips, filters, 0);
          interpolate_color(ivoid, ovoid, roi_out, 0, -1, j, clips, filters, 1);
        }

// up/down directions
#ifdef _OPENMP
#pragma omp parallel for default(none) \
        dt_omp_firstprivate(clips, filters, ivoid, ovoid, roi_out) \
        shared(data, piece) \
        schedule(static)
#endif
        for(int i = 0; i < roi_out->width; i++)
        {
          interpolate_color(ivoid, ovoid, roi_out, 1, 1, i, clips, filters, 2);
          interpolate_color(ivoid, ovoid, roi_out, 1, -1, i, clips, filters, 3);
        }
      }
      break;
    }
    case DT_IOP_HIGHLIGHTS_LCH:
      if(filters == 9u)
        process_lch_xtrans(self, piece, ivoid, ovoid, roi_in, roi_out, clip);
      else
        process_lch_bayer(self, piece, ivoid, ovoid, roi_in, roi_out, clip);
      break;

    case DT_IOP_HIGHLIGHTS_SEGMENTS:
    {
      const dt_highlights_mask_t vmode = ((g != NULL) && fullpipe && (g->hlr_mask_mode != DT_HIGHLIGHTS_MASK_CLIPPED)) ? g->hlr_mask_mode : DT_HIGHLIGHTS_MASK_OFF;

      float *tmp = _process_opposed(self, piece, ivoid, ovoid, roi_in, roi_out, TRUE, TRUE);
      if(tmp)
        _process_segmentation(piece, ivoid, ovoid, roi_in, roi_out, data, vmode, tmp);
      dt_free_align(tmp);
      break;
    }

    case DT_IOP_HIGHLIGHTS_CLIP:
    {
      process_clip(piece, ivoid, ovoid, roi_in, roi_out, clip);
      break;
    }

    case DT_IOP_HIGHLIGHTS_LAPLACIAN:
    {
      const dt_aligned_pixel_t clips = { 0.995f * data->clip * piece->pipe->dsc.processed_maximum[0],
                                         0.995f * data->clip * piece->pipe->dsc.processed_maximum[1],
                                         0.995f * data->clip * piece->pipe->dsc.processed_maximum[2], clip };
      process_laplacian_bayer(self, piece, ivoid, ovoid, roi_in, roi_out, clips);
      break;
    }

    default:
    case DT_IOP_HIGHLIGHTS_OPPOSED:
    {
      float *tmp = _process_opposed(self, piece, ivoid, ovoid, roi_in, roi_out, FALSE, high_quality);
      dt_free_align(tmp);
      break;
    }
  }

  // update processed maximum
  if((data->mode != DT_IOP_HIGHLIGHTS_LAPLACIAN) && (data->mode != DT_IOP_HIGHLIGHTS_SEGMENTS) && (data->mode != DT_IOP_HIGHLIGHTS_OPPOSED))
  {
    // The guided laplacian, inpaint opposed and segmentation modes keep signal scene-referred and don't clip highlights to 1
    // For the other modes, we need to notify the pipeline that white point has changed
    const float m = fmaxf(fmaxf(piece->pipe->dsc.processed_maximum[0], piece->pipe->dsc.processed_maximum[1]),
                          piece->pipe->dsc.processed_maximum[2]);
    for(int k = 0; k < 3; k++) piece->pipe->dsc.processed_maximum[k] = m;
  }
}

void commit_params(struct dt_iop_module_t *self,
                   dt_iop_params_t *p1,
                   dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_highlights_params_t *p = (dt_iop_highlights_params_t *)p1;
  dt_iop_highlights_data_t *d = (dt_iop_highlights_data_t *)piece->data;

  memcpy(d, p, sizeof(*p));

  const gboolean linear = (piece->pipe->dsc.filters == 0);

  /* no OpenCLfor
     1. DT_IOP_HIGHLIGHTS_INPAINT and DT_IOP_HIGHLIGHTS_SEGMENTS
     2. DT_IOP_HIGHLIGHTS_OPPOSED on linear raws
     FIXME the opposed preprocessing might be added as OpenCL too
  */
  const gboolean opplinear = (d->mode == DT_IOP_HIGHLIGHTS_OPPOSED) && linear;

  piece->process_cl_ready = ((d->mode == DT_IOP_HIGHLIGHTS_INPAINT) || (d->mode == DT_IOP_HIGHLIGHTS_SEGMENTS) || opplinear) ? FALSE : TRUE;

  if((d->mode == DT_IOP_HIGHLIGHTS_SEGMENTS) || (d->mode == DT_IOP_HIGHLIGHTS_OPPOSED))
    piece->process_tiling_ready = FALSE;

  const gboolean fullpipe = piece->pipe->type & DT_DEV_PIXELPIPE_FULL;

  dt_iop_highlights_gui_data_t *g = (dt_iop_highlights_gui_data_t *)self->gui_data;
  if(g)
  {
    // the clipped visualizer for linears is not implemented in cl
    if((g->hlr_mask_mode == DT_HIGHLIGHTS_MASK_CLIPPED) && linear && fullpipe)
      piece->process_cl_ready = FALSE;
  }
}

void init_global(dt_iop_module_so_t *module)
{
  const int program = 2; // basic.cl, from programs.conf
  dt_iop_highlights_global_data_t *gd
      = (dt_iop_highlights_global_data_t *)malloc(sizeof(dt_iop_highlights_global_data_t));
  module->data = gd;
  gd->kernel_highlights_1f_clip = dt_opencl_create_kernel(program, "highlights_1f_clip");
  gd->kernel_highlights_1f_lch_bayer = dt_opencl_create_kernel(program, "highlights_1f_lch_bayer");
  gd->kernel_highlights_1f_lch_xtrans = dt_opencl_create_kernel(program, "highlights_1f_lch_xtrans");
  gd->kernel_highlights_4f_clip = dt_opencl_create_kernel(program, "highlights_4f_clip");
  gd->kernel_highlights_bilinear_and_mask = dt_opencl_create_kernel(program, "interpolate_and_mask");
  gd->kernel_highlights_remosaic_and_replace = dt_opencl_create_kernel(program, "remosaic_and_replace");
  gd->kernel_highlights_box_blur = dt_opencl_create_kernel(program, "box_blur_5x5");
  gd->kernel_highlights_guide_laplacians = dt_opencl_create_kernel(program, "guide_laplacians");
  gd->kernel_highlights_diffuse_color = dt_opencl_create_kernel(program, "diffuse_color");

  gd->kernel_highlights_opposed = dt_opencl_create_kernel(program, "highlights_opposed");
  gd->kernel_highlights_initmask = dt_opencl_create_kernel(program, "highlights_initmask");
  gd->kernel_highlights_dilatemask = dt_opencl_create_kernel(program, "highlights_dilatemask");
  gd->kernel_highlights_chroma = dt_opencl_create_kernel(program, "highlights_chroma");

  gd->kernel_highlights_false_color = dt_opencl_create_kernel(program, "highlights_false_color");
  gd->kernel_interpolate_bilinear = dt_opencl_create_kernel(program, "interpolate_bilinear");

  const int wavelets = 35; // bspline.cl, from programs.conf
  gd->kernel_filmic_bspline_horizontal = dt_opencl_create_kernel(wavelets, "blur_2D_Bspline_horizontal");
  gd->kernel_filmic_bspline_vertical = dt_opencl_create_kernel(wavelets, "blur_2D_Bspline_vertical");
  gd->kernel_filmic_wavelets_detail = dt_opencl_create_kernel(wavelets, "wavelets_detail_level");
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_highlights_global_data_t *gd = (dt_iop_highlights_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_highlights_4f_clip);
  dt_opencl_free_kernel(gd->kernel_highlights_1f_lch_bayer);
  dt_opencl_free_kernel(gd->kernel_highlights_1f_lch_xtrans);
  dt_opencl_free_kernel(gd->kernel_highlights_1f_clip);
  dt_opencl_free_kernel(gd->kernel_highlights_bilinear_and_mask);
  dt_opencl_free_kernel(gd->kernel_highlights_remosaic_and_replace);
  dt_opencl_free_kernel(gd->kernel_highlights_box_blur);
  dt_opencl_free_kernel(gd->kernel_highlights_guide_laplacians);
  dt_opencl_free_kernel(gd->kernel_highlights_diffuse_color);

  dt_opencl_free_kernel(gd->kernel_highlights_opposed);
  dt_opencl_free_kernel(gd->kernel_highlights_initmask);
  dt_opencl_free_kernel(gd->kernel_highlights_dilatemask);
  dt_opencl_free_kernel(gd->kernel_highlights_chroma);

  dt_opencl_free_kernel(gd->kernel_highlights_false_color);

  dt_opencl_free_kernel(gd->kernel_filmic_bspline_vertical);
  dt_opencl_free_kernel(gd->kernel_filmic_bspline_horizontal);
  dt_opencl_free_kernel(gd->kernel_filmic_wavelets_detail);

  dt_opencl_free_kernel(gd->kernel_interpolate_bilinear);

  free(module->data);
  module->data = NULL;
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_highlights_data_t));
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void gui_changed(dt_iop_module_t *self, GtkWidget *w, void *previous)
{
  dt_iop_highlights_gui_data_t *g = (dt_iop_highlights_gui_data_t *)self->gui_data;
  dt_iop_highlights_params_t *p = (dt_iop_highlights_params_t *)self->params;

  const uint32_t filters = self->dev->image_storage.buf_dsc.filters;
  const gboolean bayer = (filters != 0) && (filters != 9u);

  // Sanitize mode if wrongfully copied as part of the history of another pic or by preset / style
  if((!bayer && (p->mode == DT_IOP_HIGHLIGHTS_LAPLACIAN))
    || ((filters == 0) && (p->mode == DT_IOP_HIGHLIGHTS_LCH
        || p->mode == DT_IOP_HIGHLIGHTS_INPAINT
        || p->mode == DT_IOP_HIGHLIGHTS_SEGMENTS)))
  {
    p->mode = DT_IOP_HIGHLIGHTS_OPPOSED;
    dt_bauhaus_combobox_set_from_value(g->mode, p->mode);
    dt_control_log(_("highlights: mode not available for this type of image. falling back to inpaint opposed."));
  }

  const gboolean use_laplacian = bayer && p->mode == DT_IOP_HIGHLIGHTS_LAPLACIAN;
  const gboolean use_segmentation = (p->mode == DT_IOP_HIGHLIGHTS_SEGMENTS);
  const gboolean use_recovery = use_segmentation && (p->recovery != DT_RECOVERY_MODE_OFF);

  gtk_widget_set_visible(g->noise_level, use_laplacian || use_recovery);
  gtk_widget_set_visible(g->iterations, use_laplacian);
  gtk_widget_set_visible(g->scales, use_laplacian);
  gtk_widget_set_visible(g->solid_color, use_laplacian);

  gtk_widget_set_visible(g->candidating, use_segmentation);
  gtk_widget_set_visible(g->combine, use_segmentation);
  gtk_widget_set_visible(g->recovery, use_segmentation);
  gtk_widget_set_visible(g->strength, use_recovery);
  dt_bauhaus_widget_set_quad_visibility(g->strength, use_recovery);

  // The special case for strength button active needs further care here
  if((use_segmentation && (p->recovery == DT_RECOVERY_MODE_OFF)) && (g->hlr_mask_mode == DT_HIGHLIGHTS_MASK_STRENGTH))
  {
    dt_bauhaus_widget_set_quad_active(g->strength, FALSE);
    g->hlr_mask_mode = DT_HIGHLIGHTS_MASK_OFF;
  }

  if(w == g->mode)
  {
    dt_bauhaus_widget_set_quad_active(g->clip, FALSE);
    dt_bauhaus_widget_set_quad_active(g->candidating, FALSE);
    dt_bauhaus_widget_set_quad_active(g->combine, FALSE);
    dt_bauhaus_widget_set_quad_active(g->strength, FALSE);
    g->hlr_mask_mode = DT_HIGHLIGHTS_MASK_OFF;
  }
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_highlights_gui_data_t *g = (dt_iop_highlights_gui_data_t *)self->gui_data;
  const gboolean monochrome = dt_image_is_monochrome(&self->dev->image_storage);
  // enable this per default if raw or sraw if not real monochrome
  self->default_enabled = dt_image_is_rawprepare_supported(&self->dev->image_storage) && !monochrome;
  self->hide_enable_button = !(self->default_enabled);
  gtk_stack_set_visible_child_name(GTK_STACK(self->widget), self->default_enabled ? "default" : "notapplicable");
  dt_bauhaus_widget_set_quad_active(g->clip, FALSE);
  dt_bauhaus_widget_set_quad_active(g->candidating, FALSE);
  dt_bauhaus_widget_set_quad_active(g->combine, FALSE);
  dt_bauhaus_widget_set_quad_active(g->strength, FALSE);
  g->hlr_mask_mode = DT_HIGHLIGHTS_MASK_OFF;

  gui_changed(self, NULL, NULL);
}

void gui_reset(struct dt_iop_module_t *self)
{
  dt_iop_highlights_params_t *p = (dt_iop_highlights_params_t *)self->params;
  dt_iop_highlights_params_t *d = (dt_iop_highlights_params_t *)self->default_params;

  d->mode = DT_IOP_HIGHLIGHTS_OPPOSED;
  p->mode = DT_IOP_HIGHLIGHTS_OPPOSED;
}

void reload_defaults(dt_iop_module_t *self)
{
  // we might be called from presets update infrastructure => there is no image
  if(!self->dev || !dt_is_valid_imgid(self->dev->image_storage.id)) return;

  const gboolean monochrome = dt_image_is_monochrome(&self->dev->image_storage);
  // enable this per default if raw or sraw if not true monochrome
  self->default_enabled = dt_image_is_rawprepare_supported(&self->dev->image_storage) && !monochrome;
  self->hide_enable_button = !(self->default_enabled);

  dt_iop_highlights_params_t *d = (dt_iop_highlights_params_t *)self->default_params;

  if(!dt_image_altered(self->dev->image_storage.id))
    d->mode = DT_IOP_HIGHLIGHTS_OPPOSED;

  if(self->widget)
    gtk_stack_set_visible_child_name(GTK_STACK(self->widget), self->default_enabled ? "default" : "notapplicable");

  dt_iop_highlights_gui_data_t *g = (dt_iop_highlights_gui_data_t *)self->gui_data;
  if(g)
  {
    // rebuild the complete menu depending on sensor type and possibly active but obsolete mode
    const uint32_t filters = self->dev->image_storage.buf_dsc.filters;
    dt_bauhaus_combobox_clear(g->mode);

    dt_introspection_type_enum_tuple_t *values = self->so->get_f("mode")->Enum.values;
    if(filters == 0)
    {
      dt_bauhaus_combobox_add_introspection(g->mode, NULL, values, DT_IOP_HIGHLIGHTS_OPPOSED,
                                                                   DT_IOP_HIGHLIGHTS_OPPOSED);
      dt_bauhaus_combobox_add_introspection(g->mode, NULL, values, DT_IOP_HIGHLIGHTS_CLIP,
                                                                   DT_IOP_HIGHLIGHTS_CLIP);
    }
    else
    {
      dt_bauhaus_combobox_add_introspection(g->mode, NULL, values, DT_IOP_HIGHLIGHTS_OPPOSED,
                                                                   filters == 9u
                                                                   ? DT_IOP_HIGHLIGHTS_SEGMENTS
                                                                   : DT_IOP_HIGHLIGHTS_LAPLACIAN);
    }
    dt_bauhaus_widget_set_quad_active(g->clip, FALSE);
    dt_bauhaus_widget_set_quad_active(g->candidating, FALSE);
    dt_bauhaus_widget_set_quad_active(g->combine, FALSE);
    dt_bauhaus_widget_set_quad_active(g->strength, FALSE);
    g->hlr_mask_mode = DT_HIGHLIGHTS_MASK_OFF;
  }
}

static void _visualize_callback(GtkWidget *quad, gpointer user_data)
{
  if(darktable.gui->reset) return;
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_highlights_gui_data_t *g = (dt_iop_highlights_gui_data_t *)self->gui_data;
  dt_bauhaus_widget_set_quad_active(g->candidating, FALSE);
  dt_bauhaus_widget_set_quad_active(g->combine, FALSE);
  dt_bauhaus_widget_set_quad_active(g->strength, FALSE);
  g->hlr_mask_mode = (dt_bauhaus_widget_get_quad_active(quad)) ? DT_HIGHLIGHTS_MASK_CLIPPED : DT_HIGHLIGHTS_MASK_OFF;
  dt_dev_reprocess_center(self->dev);
}

static void _candidating_callback(GtkWidget *quad, gpointer user_data)
{
  if(darktable.gui->reset) return;
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_highlights_gui_data_t *g = (dt_iop_highlights_gui_data_t *)self->gui_data;
  g->hlr_mask_mode = (dt_bauhaus_widget_get_quad_active(quad)) ? DT_HIGHLIGHTS_MASK_CANDIDATING : DT_HIGHLIGHTS_MASK_OFF;
  dt_bauhaus_widget_set_quad_active(g->clip, FALSE);
  dt_bauhaus_widget_set_quad_active(g->combine, FALSE);
  dt_bauhaus_widget_set_quad_active(g->strength, FALSE);
  dt_dev_reprocess_center(self->dev);
}

static void _combine_callback(GtkWidget *quad, gpointer user_data)
{
  if(darktable.gui->reset) return;
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_highlights_gui_data_t *g = (dt_iop_highlights_gui_data_t *)self->gui_data;
  g->hlr_mask_mode = (dt_bauhaus_widget_get_quad_active(quad)) ? DT_HIGHLIGHTS_MASK_COMBINE : DT_HIGHLIGHTS_MASK_OFF;
  dt_bauhaus_widget_set_quad_active(g->clip, FALSE);
  dt_bauhaus_widget_set_quad_active(g->candidating, FALSE);
  dt_bauhaus_widget_set_quad_active(g->strength, FALSE);
  dt_dev_reprocess_center(self->dev);
}

static void _strength_callback(GtkWidget *quad, gpointer user_data)
{
  if(darktable.gui->reset) return;
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_highlights_gui_data_t *g = (dt_iop_highlights_gui_data_t *)self->gui_data;
  g->hlr_mask_mode = (dt_bauhaus_widget_get_quad_active(quad)) ? DT_HIGHLIGHTS_MASK_STRENGTH : DT_HIGHLIGHTS_MASK_OFF;
  dt_bauhaus_widget_set_quad_active(g->clip, FALSE);
  dt_bauhaus_widget_set_quad_active(g->combine, FALSE);
  dt_bauhaus_widget_set_quad_active(g->candidating, FALSE);
  dt_dev_reprocess_center(self->dev);
}

void gui_focus(struct dt_iop_module_t *self, gboolean in)
{
  dt_iop_highlights_gui_data_t *g = (dt_iop_highlights_gui_data_t *)self->gui_data;
  if(!in)
  {
    const gboolean was_visualize = (g->hlr_mask_mode != DT_HIGHLIGHTS_MASK_OFF);
    dt_bauhaus_widget_set_quad_active(g->clip, FALSE);
    dt_bauhaus_widget_set_quad_active(g->candidating, FALSE);
    dt_bauhaus_widget_set_quad_active(g->combine, FALSE);
    dt_bauhaus_widget_set_quad_active(g->strength, FALSE);
    g->hlr_mask_mode = DT_HIGHLIGHTS_MASK_OFF;
    if(was_visualize) dt_dev_reprocess_center(self->dev);
  }
}

void gui_init(struct dt_iop_module_t *self)
{
  dt_iop_highlights_gui_data_t *g = IOP_GUI_ALLOC(highlights);
  GtkWidget *box_raw = self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  g->mode = dt_bauhaus_combobox_from_params(self, "mode");
  gtk_widget_set_tooltip_text(g->mode, _("highlight reconstruction method"));

  g->clip = dt_bauhaus_slider_from_params(self, "clip");
  dt_bauhaus_slider_set_digits(g->clip, 3);
  gtk_widget_set_tooltip_text(g->clip,
                              _("manually adjust the clipping threshold mostly used against "
                                "magenta highlights\nthe mask icon shows the clipped areas.\n"
                                "you might use this for tuning 'laplacian', 'inpaint opposed' or 'segmentation' modes,\n"
                                "especially if camera white point is incorrect."));
  dt_bauhaus_widget_set_quad_paint(g->clip, dtgtk_cairo_paint_showmask, 0, NULL);
  dt_bauhaus_widget_set_quad_toggle(g->clip, TRUE);
  dt_bauhaus_widget_set_quad_active(g->clip, FALSE);
  g_signal_connect(G_OBJECT(g->clip), "quad-pressed", G_CALLBACK(_visualize_callback), self);

  g->combine = dt_bauhaus_slider_from_params(self, "combine");
  dt_bauhaus_slider_set_digits(g->combine, 0);
  gtk_widget_set_tooltip_text(g->combine, _("combine closely related clipped segments by morphological operations.\n"
                                            "the mask button shows the exact positions of resulting segment borders."));
  dt_bauhaus_widget_set_quad_paint(g->combine, dtgtk_cairo_paint_showmask, 0, NULL);
  dt_bauhaus_widget_set_quad_toggle(g->combine, TRUE);
  dt_bauhaus_widget_set_quad_active(g->combine, FALSE);
  g_signal_connect(G_OBJECT(g->combine), "quad-pressed", G_CALLBACK(_combine_callback), self);

  g->candidating = dt_bauhaus_slider_from_params(self, "candidating");
  gtk_widget_set_tooltip_text(g->candidating, _("select inpainting after segmentation analysis.\n"
                                                "increase to favour candidates found in segmentation analysis, decrease for opposed means inpainting.\n"
                                                "the mask button shows segments that are considered to have a good candidate."));
  dt_bauhaus_slider_set_format(g->candidating, "%");
  dt_bauhaus_slider_set_digits(g->candidating, 0);
  dt_bauhaus_widget_set_quad_paint(g->candidating, dtgtk_cairo_paint_showmask, 0, NULL);
  dt_bauhaus_widget_set_quad_toggle(g->candidating, TRUE);
  dt_bauhaus_widget_set_quad_active(g->candidating, FALSE);
  g_signal_connect(G_OBJECT(g->candidating), "quad-pressed", G_CALLBACK(_candidating_callback), self);

  g->recovery = dt_bauhaus_combobox_from_params(self, "recovery");
  gtk_widget_set_tooltip_text(g->recovery, _("approximate lost data in regions with all photosites clipped, the effect depends on segment size and border gradients.\n"
                                             "choose a mode tuned for segment size or the generic mode that tries to find best settings for every segment.\n"
                                             "small means areas with a diameter less than 25 pixels, large is best for greater than 100.\n"
                                             "the flat modes ignore narrow unclipped structures (like powerlines) to keep highlights rebuilt and avoid gradients."));

  g->strength = dt_bauhaus_slider_from_params(self, "strength");
  gtk_widget_set_tooltip_text(g->strength, _("set strength of rebuilding in regions with all photosites clipped.\n"
                                             "the mask buttons shows the effect that is added to already reconstructed data."));
  dt_bauhaus_slider_set_format(g->strength, "%");
  dt_bauhaus_slider_set_digits(g->strength, 0);
  dt_bauhaus_widget_set_quad_paint(g->strength, dtgtk_cairo_paint_showmask, 0, NULL);
  dt_bauhaus_widget_set_quad_toggle(g->strength, TRUE);
  dt_bauhaus_widget_set_quad_active(g->strength, FALSE);
  g_signal_connect(G_OBJECT(g->strength), "quad-pressed", G_CALLBACK(_strength_callback), self);

  g->noise_level = dt_bauhaus_slider_from_params(self, "noise_level");
  gtk_widget_set_tooltip_text(g->noise_level, _("add noise to visually blend the reconstructed areas\n"
                                                "into the rest of the noisy image. useful at high ISO."));

  g->iterations = dt_bauhaus_slider_from_params(self, "iterations");
  gtk_widget_set_tooltip_text(g->iterations, _("increase if magenta highlights don't get fully corrected\n"
                                               "each new iteration brings a performance penalty."));

  g->solid_color = dt_bauhaus_slider_from_params(self, "solid_color");
  dt_bauhaus_slider_set_format(g->solid_color, "%");
  gtk_widget_set_tooltip_text(g->solid_color, _("increase if magenta highlights don't get fully corrected.\n"
                                                "this may produce non-smooth boundaries between valid and clipped regions."));

  g->scales = dt_bauhaus_combobox_from_params(self, "scales");
  gtk_widget_set_tooltip_text(g->scales, _("increase to correct larger clipped areas.\n"
                                           "large values bring huge performance penalties"));

  GtkWidget *notapplicable = dt_ui_label_new(_("not applicable"));
  gtk_widget_set_tooltip_text(notapplicable, _("this module only works with non-monochrome RAW and sRAW"));

  // start building top level widget
  self->widget = gtk_stack_new();
  gtk_stack_set_homogeneous(GTK_STACK(self->widget), FALSE);
  gtk_stack_add_named(GTK_STACK(self->widget), notapplicable, "notapplicable");
  gtk_stack_add_named(GTK_STACK(self->widget), box_raw, "default");
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
