/*
   This file is part of darktable,
   Copyright (C) 2010-2026 darktable developers.

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
#include "common/interpolation.h"
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
  DT_IOP_HIGHLIGHTS_OPPOSED = 5,   // $DESCRIPTION: "inpaint opposed"
  DT_IOP_HIGHLIGHTS_LCH = 1,       // $DESCRIPTION: "reconstruct in LCh"
  DT_IOP_HIGHLIGHTS_CLIP = 0,      // $DESCRIPTION: "clip highlights"
  DT_IOP_HIGHLIGHTS_SEGMENTS = 4,  // $DESCRIPTION: "segmentation based"
  DT_IOP_HIGHLIGHTS_LAPLACIAN = 3, // $DESCRIPTION: "guided laplacians"
  DT_IOP_HIGHLIGHTS_INPAINT = 2,   // $DESCRIPTION: "reconstruct color"
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
  dt_iop_highlights_mode_t mode; // $DEFAULT: DT_IOP_HIGHLIGHTS_OPPOSED $DESCRIPTION: "method"
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

const char **description(dt_iop_module_t *self)
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
  return IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_ONE_INSTANCE | IOP_FLAGS_WRITE_RASTER;
}

dt_iop_colorspace_type_t default_colorspace(dt_iop_module_t *self,
                                            dt_dev_pixelpipe_t *pipe,
                                            dt_dev_pixelpipe_iop_t *piece)
{
  // This module might possible work in RAW or RGB (e.g. for TIFF files) color space
  // depending on the input but will not change it.
  return (pipe && !dt_image_is_raw(&pipe->image)) ? IOP_CS_RGB : IOP_CS_RAW;
}

int legacy_params(dt_iop_module_t *self,
                  const void *const old_params,
                  const int old_version,
                  void **new_params,
                  int32_t *new_params_size,
                  int *new_version)
{
  typedef struct dt_iop_highlights_params_v4_t
  {
    // params of v1
    dt_iop_highlights_mode_t mode;
    float blendL;
    float blendC;
    float strength;
    // params of v2
    float clip;
    // params of v3
    float noise_level;
    int iterations;
    dt_atrous_wavelets_scales_t scales;
    float candidating;
    float combine;
    dt_recovery_mode_t recovery;
    // params of v4
    float solid_color;
  } dt_iop_highlights_params_v4_t;

  if(old_version == 1)
  {
    typedef struct dt_iop_highlights_params_v1_t
    {
      dt_iop_highlights_mode_t mode;
      float blendL, blendC, blendh;
      float strength;
    } dt_iop_highlights_params_v1_t;

    const dt_iop_highlights_params_v1_t *o = (dt_iop_highlights_params_v1_t *)old_params;
    dt_iop_highlights_params_v4_t *n = malloc(sizeof(dt_iop_highlights_params_v4_t));
    memcpy(n, o, sizeof(dt_iop_highlights_params_v1_t));

    n->clip = 1.0f;
    n->noise_level = 0.0f;
    n->candidating = 0.4f;
    n->combine = 2.f;
    n->recovery = DT_RECOVERY_MODE_OFF;
    n->iterations = 1;
    n->scales = 5;
    n->solid_color = 0.f;
    n->strength = 0.0f;

    *new_params = n;
    *new_params_size = sizeof(dt_iop_highlights_params_v4_t);
    *new_version = 4;
    return 0;
  }
  if(old_version == 2)
  {
    typedef struct dt_iop_highlights_params_v2_t
    {
      dt_iop_highlights_mode_t mode;
      float blendL, blendC, blendh;
      float strength;
      float clip;
    } dt_iop_highlights_params_v2_t;

    const dt_iop_highlights_params_v2_t *o = (dt_iop_highlights_params_v2_t *)old_params;
    dt_iop_highlights_params_v4_t *n = malloc(sizeof(dt_iop_highlights_params_v4_t));
    memcpy(n, o, sizeof(dt_iop_highlights_params_v2_t));

    n->noise_level = 0.0f;
    n->candidating = 0.4f;
    n->combine = 2.f;
    n->recovery = DT_RECOVERY_MODE_OFF;
    n->iterations = 1;
    n->scales = 5;
    n->solid_color = 0.f;
    n->strength = 0.0f;

    *new_params = n;
    *new_params_size = sizeof(dt_iop_highlights_params_v4_t);
    *new_version = 4;
    return 0;
  }
  if(old_version == 3)
  {
    typedef struct dt_iop_highlights_params_v3_t
    {
      dt_iop_highlights_mode_t mode;
      float blendL, blendC, blendh;
      float strength;
      float noise_level;
      int iterations;
      dt_atrous_wavelets_scales_t scales;
      float candidating;
      float combine;
      dt_recovery_mode_t recovery;
    } dt_iop_highlights_params_v3_t;

    const dt_iop_highlights_params_v3_t *o = (dt_iop_highlights_params_v3_t *)old_params;
    dt_iop_highlights_params_v4_t *n = malloc(sizeof(dt_iop_highlights_params_v4_t));
    memcpy(n, o, sizeof(dt_iop_highlights_params_v3_t));

    n->solid_color = 0.f;
    n->strength = 0.0f;

    *new_params = n;
    *new_params_size = sizeof(dt_iop_highlights_params_v4_t);
    *new_version = 4;
    return 0;
  }

  return 1;
}

static dt_aligned_pixel_t img_oppchroma;
static gboolean img_oppclipped = TRUE;
static dt_hash_t img_opphash = ULLONG_MAX;

#include "hlreconstruct/segmentation.c"
#include "hlreconstruct/segbased.c"
#include "hlreconstruct/opposed.c"
#include "hlreconstruct/laplacian.c"
#include "hlreconstruct/inpaint.c"
#include "hlreconstruct/lch.c"

void distort_mask(dt_iop_module_t *self,
                  dt_dev_pixelpipe_iop_t *piece,
                  const float *const in,
                  float *const out,
                  const dt_iop_roi_t *const roi_in,
                  const dt_iop_roi_t *const roi_out)
{
  if(roi_out->scale != roi_in->scale)
  {
    const dt_interpolation_t *itor = dt_interpolation_new(DT_INTERPOLATION_USERPREF_WARP);
    dt_interpolation_resample_roi_1c(itor, out, roi_out, in, roi_in);
  }
  else
    dt_iop_copy_image_roi(out, in, 1, roi_in, roi_out);
}

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

/* inpaint opposed and segmentation based algorithms want the whole image for proper calculation
   of chrominance correction and best candidates.
*/
void modify_roi_in(dt_iop_module_t *self,
                   dt_dev_pixelpipe_iop_t *piece,
                   const dt_iop_roi_t *const roi_out,
                   dt_iop_roi_t *roi_in)
{
  *roi_in = *roi_out;

  dt_iop_highlights_data_t *d = piece->data;
  const gboolean use_opposing = (d->mode == DT_IOP_HIGHLIGHTS_OPPOSED) || (d->mode == DT_IOP_HIGHLIGHTS_SEGMENTS);

  // Whenever we use opposed we have to setup the desired roi_in area
  if(!use_opposing)
    return;

  roi_in->scale = 1.0f;
  if(piece->pipe->dsc.filters == 0)
  {
    // For linear raws we will use an internal downscaler as we normally do in demosaic
    roi_in->x /= roi_out->scale;
    roi_in->y /= roi_out->scale;
    roi_in->width /= roi_out->scale;
    roi_in->height /= roi_out->scale;
  }
  else
  {
    // We require the correct (full-image-data) expansion with a defined scale for all pixelpipes for proper
    // aligning and scaling in the demosiacer
    roi_in->x = 0;
    roi_in->y = 0;
    roi_in->width = piece->buf_in.width;
    roi_in->height = piece->buf_in.height;
  }
}

void tiling_callback(dt_iop_module_t *self,
                     dt_dev_pixelpipe_iop_t *piece,
                     const dt_iop_roi_t *roi_in,
                     const dt_iop_roi_t *roi_out,
                     dt_develop_tiling_t *tiling)
{
  dt_iop_highlights_data_t *d = piece->data;
  const uint32_t filters = piece->pipe->dsc.filters;

  const gboolean is_bayer = filters && (filters != 9u);
  const gboolean is_xtrans = filters && (filters == 9u);

  tiling->xalign = 1;
  tiling->yalign = 1;
  tiling->overlap = 0;
  tiling->factor = 2.0f;
  tiling->factor_cl = 2.0f;
  tiling->maxbuf = 1.0f;
  tiling->maxbuf_cl = 1.0f;
  tiling->overhead = 0;
  tiling->overlap = 0;

  dt_develop_blend_params_t *const bldata = piece->blendop_data;
  if(bldata && dt_iop_piece_is_raster_mask_used(piece, BLEND_RASTER_ID))
  {
    tiling->factor += 0.5f;
    tiling->factor_cl += 0.5f;
  }

  if(d->mode == DT_IOP_HIGHLIGHTS_LAPLACIAN && is_bayer)
  {
    // Bayer CFA and guided laplacian method : prepare for wavelets decomposition.
    const float scale = fmaxf(DS_FACTOR * piece->iscale / roi_in->scale, 1.f);
    const float final_radius = (float)((int)(1 << d->scales)) / scale;
    const int scales = CLAMP((int)ceilf(log2f(final_radius)), 1, MAX_NUM_SCALES);
    const int max_filter_radius = (1 << scales);

    tiling->factor += 2.f * 4 + 6.f * 4 / (DS_FACTOR * DS_FACTOR);
    tiling->factor_cl += 3.f * 4 + 5.f * 4 / (DS_FACTOR * DS_FACTOR);

    // The wavelets decomposition uses a temp buffer of size 4 × ds_width
    tiling->maxbuf = 1.f / roi_in->height * dt_get_num_threads() * 4.f / DS_FACTOR;

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
    tiling->factor += 1.0f;
    return;
  }

  if(d->mode == DT_IOP_HIGHLIGHTS_OPPOSED)
  {
    tiling->factor += 0.5f; // enough for in&output buffers plus masks
    tiling->factor_cl += 0.5f; // enough for in&output buffers plus masks
    return;
  }

  if(d->mode == DT_IOP_HIGHLIGHTS_LCH)
  {
    tiling->overlap = is_xtrans ? 2 : 1;
  }
}

static float *_provide_raster_mask(const dt_iop_roi_t *const roi_in,
                                   const dt_iop_roi_t *const roi_out,
                                   const float *in,
                                   const float clip,
                                   dt_dev_pixelpipe_iop_t *piece)
{
  const size_t opix = (size_t)roi_out->width * roi_out->height;
  float *out = dt_alloc_align_float(opix);
  float *tmp = dt_alloc_align_float(opix);
  if(!out || !tmp)
  {
    dt_free_align(tmp);
    dt_free_align(out);
    return NULL;
  }

  const uint32_t filters = piece->filters;
  const float clips[4] = { clip * piece->pipe->dsc.processed_maximum[0],
                           clip * piece->pipe->dsc.processed_maximum[1],
                           clip * piece->pipe->dsc.processed_maximum[2], clip };

  if(filters == 0)  // sraw
  {
    DT_OMP_FOR()
    for(int row = 0; row < roi_out->height; row++)
    {
      for(int col = 0; col < roi_out->width; col++)
      {
        const size_t ox = (size_t)row * roi_out->width + col;
        const size_t ix = ox * 4;
        float mval = 0.0f;
        for(int c = 0; c < 3; c++)
        {
          const float ref = MAX(0.5, 0.95f * clips[c]);
          mval = MAX(mval, (in[ix+c] - ref) / ref);
        }
        tmp[ox] = MAX(0.0f, mval);
      }
    }
  }
  else
  {
    const uint8_t(*const xtrans)[6] = piece->xtrans;
    DT_OMP_FOR()
    for(int row = 0; row < roi_out->height; row++)
    {
      for(int col = 0; col < roi_out->width; col++)
      {
        const size_t ox = (size_t)row * roi_out->width + col;
        const int irow = row + roi_out->y - roi_in->y;
        const int icol = col + roi_out->x - roi_in->x;
        const int c = fcol(irow, icol, filters, xtrans);
        const float ref = MAX(0.5, 0.95f * clips[c]);
        tmp[ox] = MAX(0.0f, (in[ox] - ref) / ref);
      }
    }
  }
  dt_gaussian_fast_blur(tmp, out, roi_out->width, roi_out->height, 1.0f, 0.0f, 1.0f, 1);
  dt_free_align(tmp);
  return out;
}

#ifdef HAVE_OPENCL
int process_cl(dt_iop_module_t *self,
               dt_dev_pixelpipe_iop_t *piece,
               cl_mem dev_in,
               cl_mem dev_out,
               const dt_iop_roi_t *const roi_in,
               const dt_iop_roi_t *const roi_out)
{
  dt_dev_pixelpipe_t *pipe = piece->pipe;
  dt_iop_highlights_data_t *d = piece->data;
  dt_iop_highlights_gui_data_t *g = self->gui_data;
  dt_iop_highlights_global_data_t *gd = self->global_data;

  const uint32_t filters = piece->filters;
  const int devid = pipe->devid;

  if(pipe->mask_display == DT_DEV_PIXELPIPE_DISPLAY_PASSTHRU)
  {
    if(!filters)
      return dt_iop_clip_and_zoom_roi_cl(devid, dev_out, dev_in, roi_out, roi_in);
    else
    {
      size_t iorigin[] = { roi_out->x, roi_out->y, 0 };
      size_t oorigin[] = { 0, 0, 0 };
      size_t region[] = { roi_out->width, roi_out->height, 1 };
      return dt_opencl_enqueue_copy_image(devid, dev_in, dev_out, iorigin, oorigin, region);
    }
  }

  const gboolean fullpipe = pipe->type & DT_DEV_PIXELPIPE_FULL;
  const dt_iop_highlights_mode_t dmode =  d->mode;

  gboolean announce = dt_iop_piece_is_raster_mask_used(piece, BLEND_RASTER_ID);

  cl_int err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
  cl_mem dev_xtrans = NULL;
  cl_mem dev_clips = NULL;

  if(g && fullpipe)
  {
    if(g->hlr_mask_mode != DT_HIGHLIGHTS_MASK_OFF)
    {
      pipe->mask_display = DT_DEV_PIXELPIPE_DISPLAY_PASSTHRU;
      if(g->hlr_mask_mode == DT_HIGHLIGHTS_MASK_CLIPPED)
      {
        const float mclip = d->clip * highlights_clip_magics[d->mode];
        const float *c = pipe->dsc.temperature.coeffs;
        float clips[4] = { mclip * (c[RED]   <= 0.0f ? 1.0f : c[RED]),
                           mclip * (c[GREEN] <= 0.0f ? 1.0f : c[GREEN]),
                           mclip * (c[BLUE]  <= 0.0f ? 1.0f : c[BLUE]),
                           mclip * (c[GREEN] <= 0.0f ? 1.0f : c[GREEN]) };

        dev_clips = dt_opencl_copy_host_to_device_constant(devid, 4 * sizeof(float), clips);
        dev_xtrans = dt_opencl_copy_host_to_device_constant(devid, sizeof(piece->xtrans), piece->xtrans);
        if(!dev_clips || !dev_xtrans) goto finish;

        const int dy = roi_out->y - roi_in->y;
        const int dx = roi_out->x - roi_in->x;

        err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_highlights_false_color, roi_out->width, roi_out->height,
          CLARG(dev_in), CLARG(dev_out),
          CLARG(roi_out->width), CLARG(roi_out->height),
          CLARG(roi_in->width), CLARG(roi_in->height),
          CLARG(dx), CLARG(dy),
          CLARG(filters), CLARG(dev_xtrans),
          CLARG(dev_clips));
        announce = FALSE;
        goto finish;
      }
    }
  }

  const float clip = d->clip * dt_iop_get_processed_minimum(piece);

  if(!filters)
  {
    // non-raw images use dedicated kernel which just clips
    err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_highlights_4f_clip, roi_in->width, roi_in->height,
      CLARG(dev_in), CLARG(dev_out),
      CLARG(roi_in->width), CLARG(roi_in->height),
      CLARG(dmode), CLARG(clip));
  }
  else if(dmode == DT_IOP_HIGHLIGHTS_OPPOSED)
  {
    err = process_opposed_cl(self, piece, dev_in, dev_out, roi_in, roi_out);
  }
  else if(dmode == DT_IOP_HIGHLIGHTS_LCH && filters != 9u)
  {
    // bayer sensor raws with LCH mode
    err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_highlights_1f_lch_bayer, roi_in->width, roi_in->height,
      CLARG(dev_in), CLARG(dev_out),
      CLARG(roi_in->width), CLARG(roi_in->height),
      CLARG(clip), CLARG(filters));
  }
  else if(dmode == DT_IOP_HIGHLIGHTS_LCH && filters == 9u)
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

    dev_xtrans = dt_opencl_copy_host_to_device_constant(devid, sizeof(piece->xtrans), piece->xtrans);
    if(dev_xtrans == NULL) goto finish;

    size_t sizes[] = { ROUNDUP(roi_in->width, blocksizex), ROUNDUP(roi_in->height, blocksizey), 1 };
    size_t local[] = { blocksizex, blocksizey, 1 };
    dt_opencl_set_kernel_args(devid, gd->kernel_highlights_1f_lch_xtrans, 0,
      CLARG(dev_in), CLARG(dev_out),
      CLARG(roi_in->width), CLARG(roi_in->height),
      CLARG(clip), CLARG(dev_xtrans),
      CLLOCAL(sizeof(float) * (blocksizex + 4) * (blocksizey + 4)));
    err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_highlights_1f_lch_xtrans, sizes, local);
  }
  else if(dmode == DT_IOP_HIGHLIGHTS_LAPLACIAN)
  {
    const float clipper = d->clip * highlights_clip_magics[DT_IOP_HIGHLIGHTS_LAPLACIAN];
    const dt_aligned_pixel_t clips = { clipper * pipe->dsc.processed_maximum[0],
                                       clipper * pipe->dsc.processed_maximum[1],
                                       clipper * pipe->dsc.processed_maximum[2], clip };
    err = process_laplacian_bayer_cl(self, piece, dev_in, dev_out, roi_in, roi_out, clips);
  }
  else // (dmode == DT_IOP_HIGHLIGHTS_CLIP)
  {
    const dt_dev_chroma_t *chr = &self->dev->chroma;
    dt_aligned_pixel_t clips = { clip, clip, clip, clip};
    if(chr->late_correction)
    for_each_channel(c)
      clips[c] *= chr->as_shot[c] / chr->D65coeffs[c];
    dev_clips = dt_opencl_copy_host_to_device_constant(devid, 4 * sizeof(float), clips);
    dev_xtrans = dt_opencl_copy_host_to_device_constant(devid, sizeof(piece->xtrans), piece->xtrans);
    if(!dev_clips || !dev_xtrans) goto finish;

    // raw images with clip mode (both bayer and xtrans)
    const int dy = roi_out->y - roi_in->y;
    const int dx = roi_out->x - roi_in->x;
    err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_highlights_1f_clip, roi_out->width, roi_out->height,
      CLARG(dev_in), CLARG(dev_out),
      CLARG(roi_in->width), CLARG(roi_in->height),
      CLARG(roi_out->width), CLARG(roi_out->height),
      CLARG(dev_clips), CLARG(dx), CLARG(dy),
      CLARG(filters), CLARG(dev_xtrans));
  }
  if(err != CL_SUCCESS) goto finish;

  float *mask = NULL;
  if(announce)
  {
    const size_t ch = filters ? 1 : 4;
    float *cpdata = dt_alloc_align_float(ch * roi_out->width * roi_out->height);
    if(cpdata)
    {
      if(dt_opencl_copy_device_to_host(devid, cpdata, dev_out, roi_out->width, roi_out->height, ch * sizeof(float)) == CL_SUCCESS)
        mask = _provide_raster_mask(roi_in, roi_out, cpdata, d->clip, piece);
      dt_free_align(cpdata);
    }
  }
  if(mask)  dt_iop_piece_set_raster(piece, mask, roi_in, roi_out);
  else      dt_iop_piece_clear_raster(piece, NULL);

  // update processed maximum
  if((err == CL_SUCCESS) && (d->mode != DT_IOP_HIGHLIGHTS_LAPLACIAN) && (d->mode != DT_IOP_HIGHLIGHTS_OPPOSED))
  {
    // The guided laplacian and opposed are the modes that keeps signal scene-referred and don't clip highlights to 1
    // For the other modes, we need to notify the pipeline that white point has changed
    const float m = dt_iop_get_processed_maximum(piece);
    for_three_channels(k) pipe->dsc.processed_maximum[k] = m;
  }

  finish:
  if(err != CL_SUCCESS) dt_iop_piece_clear_raster(piece, NULL);

  dt_opencl_release_mem_object(dev_clips);
  dt_opencl_release_mem_object(dev_xtrans);
  return err;
}
#endif

static void process_clip(dt_iop_module_t *self,
                         dt_dev_pixelpipe_iop_t *piece,
                         const void *const ivoid,
                         void *const ovoid,
                         const dt_iop_roi_t *const roi_in,
                         const dt_iop_roi_t *const roi_out,
                         const float clip)
{
  const float *const in = (const float *const)ivoid;
  float *const out = (float *const)ovoid;

  const uint32_t filters = piece->filters;
  const int ch = filters ? 1 : 4;
  if(ch == 4)
  {
    const size_t msize = (size_t)roi_out->width * roi_out->height * ch;
    DT_OMP_FOR()
    for(size_t k = 0; k < msize; k++)
      out[k] = fminf(clip, in[k]);
  }
  else
  {
    const uint8_t(*const xtrans)[6] = piece->xtrans;
    const dt_dev_chroma_t *chr = &self->dev->chroma;
    dt_aligned_pixel_t clips = { clip, clip, clip, clip};
    if(chr->late_correction)
    {
      for_each_channel(c) clips[c] *= chr->as_shot[c] / chr->D65coeffs[c];
    }
    for(int row = 0; row < roi_out->height; row++)
    {
      for(int col = 0; col < roi_out->width; col++)
      {
        const size_t ox = (size_t)row * roi_out->width + col;
        const int irow = row + roi_out->y - roi_in->y;
        const int icol = col + roi_out->x - roi_in->x;
        const size_t ix = (size_t)irow * roi_in->width + icol;

        if((icol >= 0) && (irow >= 0) && (irow < roi_in->height) && (icol < roi_in->width))
        {
          const int c = fcol(irow, icol, filters, xtrans);
          out[ox] = fminf(in[ix], clips[c]);
        }
        else
          out[ox] = 0.0f;
      }
    }
  }
}

static void process_visualize(dt_dev_pixelpipe_iop_t *piece,
                              const void *const ivoid,
                              void *const ovoid,
                              const dt_iop_roi_t *const roi_in,
                              const dt_iop_roi_t *const roi_out,
                              dt_iop_highlights_data_t *d)
{
  const uint8_t(*const xtrans)[6] = piece->xtrans;
  const uint32_t filters = piece->filters;
  const float *const in = (const float *const)ivoid;
  float *const out = (float *const)ovoid;

  const float mclip = d->clip * highlights_clip_magics[d->mode];
  const float *cf = piece->pipe->dsc.temperature.coeffs;
  const float clips[4] = { mclip * (cf[RED]   <= 0.0f ? 1.0f : cf[RED]),
                           mclip * (cf[GREEN] <= 0.0f ? 1.0f : cf[GREEN]),
                           mclip * (cf[BLUE]  <= 0.0f ? 1.0f : cf[BLUE]),
                           mclip * (cf[GREEN] <= 0.0f ? 1.0f : cf[GREEN]) };

  if(filters == 0)
  {
    const size_t npixels = roi_out->width * (size_t)roi_out->height;
    DT_OMP_FOR()
    for(size_t k = 0; k < 4*npixels; k += 4)
    {
      for_each_channel(c)
        out[k+c] = (in[k+c] < clips[c]) ? 0.2f * in[k+c] : 1.0f;
      out[k+3] = 0.0f;
    }
  }
  else
  {
    DT_OMP_FOR()
    for(int row = 0; row < roi_out->height; row++)
    {
      for(int col = 0; col < roi_out->width; col++)
      {
        const size_t ox = (size_t)row * roi_out->width + col;
        const int irow = row + roi_out->y - roi_in->y;
        const int icol = col + roi_out->x - roi_in->x;
        const size_t ix = (size_t)irow * roi_in->width + icol;

        if((icol >= 0) && (irow >= 0) && (irow < roi_in->height) && (icol < roi_in->width))
        {
          const int c = fcol(irow, icol, filters, xtrans);
          const float ival = in[ix];
          out[ox] = (ival < clips[c]) ? 0.2f * ival : 1.0f;
        }
        else
          out[ox] = 0.0f;
      }
    }
  }
}

void process(dt_iop_module_t *self,
             dt_dev_pixelpipe_iop_t *piece,
             const void *const ivoid,
             void *const ovoid,
             const dt_iop_roi_t *const roi_in,
             const dt_iop_roi_t *const roi_out)
{
  dt_dev_pixelpipe_t *pipe = piece->pipe;
  const uint32_t filters = piece->filters;
  dt_iop_highlights_data_t *d = piece->data;
  dt_iop_highlights_gui_data_t *g = self->gui_data;

  if(pipe->mask_display == DT_DEV_PIXELPIPE_DISPLAY_PASSTHRU)
  {
    if(!filters)
      dt_iop_clip_and_zoom_roi((float *)ovoid, (float *)ivoid, roi_out, roi_in);
    else
      dt_iop_copy_image_roi((float *)ovoid, (float *)ivoid, 1, roi_in, roi_out);
    return;
  }

  const gboolean fullpipe = pipe->type & DT_DEV_PIXELPIPE_FULL;
  const gboolean fastmode = pipe->type & DT_DEV_PIXELPIPE_FAST;
  const dt_iop_highlights_mode_t dmode = fastmode && (d->mode == DT_IOP_HIGHLIGHTS_SEGMENTS)
                                        ? DT_IOP_HIGHLIGHTS_OPPOSED
                                        : d->mode;
  const gboolean scaled = filters == 0 && dmode != DT_IOP_HIGHLIGHTS_CLIP;

  float *out = scaled ? dt_alloc_align_float((size_t)roi_in->width * roi_in->height * 4) : NULL;
  const gboolean announce = dt_iop_piece_is_raster_mask_used(piece, BLEND_RASTER_ID);

  if(!out && scaled)
  {
    dt_iop_clip_and_zoom_roi((float *)ovoid, (float *)ivoid, roi_out, roi_in);
    dt_print_pipe(DT_DEBUG_ALWAYS,
          "bypass highlights", pipe, self, DT_DEVICE_CPU, roi_in, roi_out,
          "can't allocate temp buffer");
    dt_iop_piece_clear_raster(piece, NULL);
    return;
  }

  if(g && fullpipe)
  {
    if(g->hlr_mask_mode != DT_HIGHLIGHTS_MASK_OFF)
    {
      pipe->mask_display = DT_DEV_PIXELPIPE_DISPLAY_PASSTHRU;
      if(g->hlr_mask_mode == DT_HIGHLIGHTS_MASK_CLIPPED)
      {
        if(scaled)
        {
          process_visualize(piece, ivoid, out, roi_in, roi_in, d);
          dt_iop_clip_and_zoom_roi((float *)ovoid, out, roi_out, roi_in);
          dt_free_align(out);
        }
        else
          process_visualize(piece, ivoid, ovoid, roi_in, roi_out, d);

        dt_iop_piece_clear_raster(piece, NULL);
        return;
      }
    }
  }

  /* While rendering thumnbnails we look for an acceptable lower quality */
  gboolean high_quality = TRUE;
  if(pipe->type & DT_DEV_PIXELPIPE_THUMBNAIL)
  {
    const dt_mipmap_size_t level = dt_mipmap_cache_get_matching_size(pipe->final_width, pipe->final_height);
    const char *min = dt_conf_get_string_const("plugins/lighttable/thumbnail_hq_min_level");
    const dt_mipmap_size_t min_s = dt_mipmap_cache_get_min_mip_from_pref(min);
    high_quality = (level >= min_s);
  }

  const float clip = d->clip * dt_iop_get_processed_minimum(piece);

  if(filters == 0)
  {
    if(dmode == DT_IOP_HIGHLIGHTS_CLIP)
    {
      process_clip(self, piece, ivoid, ovoid, roi_in, roi_out, clip);
      const float m = dt_iop_get_processed_minimum(piece);
      for_three_channels(k) pipe->dsc.processed_maximum[k] = m;
    }
    else
    {
      _process_linear_opposed(self, piece, ivoid, out, roi_in, high_quality);
      dt_iop_clip_and_zoom_roi((float *)ovoid, out, roi_out, roi_in);
      dt_free_align(out);
    }

    float *mask = announce ? _provide_raster_mask(roi_in, roi_out, (float *)ovoid, d->clip, piece) : NULL;
    if(mask)  dt_iop_piece_set_raster(piece, mask, roi_in, roi_out);
    else      dt_iop_piece_clear_raster(piece, NULL);

    return;
  }

  switch(dmode)
  {
    case DT_IOP_HIGHLIGHTS_INPAINT: // a1ex's (magiclantern) idea of color inpainting:
    {
      const float clipper = d->clip * highlights_clip_magics[DT_IOP_HIGHLIGHTS_INPAINT];
      const float clips[4] = { clipper * pipe->dsc.processed_maximum[0],
                               clipper * pipe->dsc.processed_maximum[1],
                               clipper * pipe->dsc.processed_maximum[2], clip };

      if(filters == 9u)
      {
        const uint8_t(*const xtrans)[6] = piece->xtrans;
        DT_OMP_FOR()
        for(int j = 0; j < roi_out->height; j++)
        {
          interpolate_color_xtrans(ivoid, ovoid, roi_in, roi_out, 0, 1, j, clips, xtrans, 0);
          interpolate_color_xtrans(ivoid, ovoid, roi_in, roi_out, 0, -1, j, clips, xtrans, 1);
        }
        DT_OMP_FOR()
        for(int i = 0; i < roi_out->width; i++)
        {
          interpolate_color_xtrans(ivoid, ovoid, roi_in, roi_out, 1, 1, i, clips, xtrans, 2);
          interpolate_color_xtrans(ivoid, ovoid, roi_in, roi_out, 1, -1, i, clips, xtrans, 3);
        }
      }
      else
      {
        DT_OMP_FOR()
        for(int j = 0; j < roi_out->height; j++)
        {
          interpolate_color(ivoid, ovoid, roi_out, 0, 1, j, clips, filters, 0);
          interpolate_color(ivoid, ovoid, roi_out, 0, -1, j, clips, filters, 1);
        }

// up/down directions
        DT_OMP_FOR()
        for(int i = 0; i < roi_out->width; i++)
        {
          interpolate_color(ivoid, ovoid, roi_out, 1, 1, i, clips, filters, 2);
          interpolate_color(ivoid, ovoid, roi_out, 1, -1, i, clips, filters, 3);
        }
      }
      break;
    }

    case DT_IOP_HIGHLIGHTS_LCH:
    {
      if(filters == 9u)
        process_lch_xtrans(self, piece, ivoid, ovoid, roi_in, roi_out, clip);
      else
        process_lch_bayer(self, piece, ivoid, ovoid, roi_in, roi_out, clip);
      break;
    }

    case DT_IOP_HIGHLIGHTS_SEGMENTS:
    {
      const dt_highlights_mask_t vmode = ((g != NULL) && fullpipe && (g->hlr_mask_mode != DT_HIGHLIGHTS_MASK_CLIPPED)) ? g->hlr_mask_mode : DT_HIGHLIGHTS_MASK_OFF;

      float *tmp = _process_opposed(self, piece, ivoid, ovoid, roi_in, roi_out, TRUE, TRUE);
      if(tmp)
        _process_segmentation(piece, ivoid, ovoid, roi_in, roi_out, d, vmode, tmp);
      dt_free_align(tmp);
      break;
    }

    case DT_IOP_HIGHLIGHTS_CLIP:
    {
      process_clip(self, piece, ivoid, ovoid, roi_in, roi_out, clip);
      break;
    }

    case DT_IOP_HIGHLIGHTS_LAPLACIAN:
    {
      const float clipper = d->clip * highlights_clip_magics[DT_IOP_HIGHLIGHTS_LAPLACIAN];
      const dt_aligned_pixel_t clips = { clipper * pipe->dsc.processed_maximum[0],
                                         clipper * pipe->dsc.processed_maximum[1],
                                         clipper * pipe->dsc.processed_maximum[2], clip };
      process_laplacian_bayer(self, piece, ivoid, ovoid, roi_in, roi_out, clips);
      break;
    }

    default:
    {
      _process_opposed(self, piece, ivoid, ovoid, roi_in, roi_out, FALSE, high_quality);
      break;
    }
  }

  float *mask = announce ? _provide_raster_mask(roi_in, roi_out, (float *)ovoid, d->clip, piece) : NULL;
  if(mask)  dt_iop_piece_set_raster(piece, mask, roi_in, roi_out);
  else      dt_iop_piece_clear_raster(piece, NULL);

  // update processed maximum
  if((dmode != DT_IOP_HIGHLIGHTS_LAPLACIAN) && (dmode != DT_IOP_HIGHLIGHTS_SEGMENTS) && (dmode != DT_IOP_HIGHLIGHTS_OPPOSED))
  {
    // The guided laplacian, inpaint opposed and segmentation modes keep signal scene-referred and don't clip highlights to 1
    // For the other modes, we need to notify the pipeline that white point has changed
    const float m = dt_iop_get_processed_maximum(piece);
    for_three_channels(k) pipe->dsc.processed_maximum[k] = m;
  }
}

void commit_params(dt_iop_module_t *self,
                   dt_iop_params_t *p1,
                   dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_highlights_params_t *p = (dt_iop_highlights_params_t *)p1;
  dt_iop_highlights_data_t *d = piece->data;

  memcpy(d, p, sizeof(*p));

  const dt_image_t *img = &piece->pipe->image;
  const uint32_t filters = img->buf_dsc.filters;
  const gboolean rawprep = dt_image_is_rawprepare_supported(img);
  const gboolean linear = (filters == 0);

  // for non-raws always use clip
  if(!rawprep)
    d->mode = DT_IOP_HIGHLIGHTS_CLIP;

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

  dt_iop_highlights_gui_data_t *g = self->gui_data;
  if(g && (g->hlr_mask_mode == DT_HIGHLIGHTS_MASK_CLIPPED) && linear && fullpipe)
    piece->process_cl_ready = FALSE;
}

void init_global(dt_iop_module_so_t *self)
{
  const int program = 2; // basic.cl, from programs.conf
  dt_iop_highlights_global_data_t *gd = malloc(sizeof(dt_iop_highlights_global_data_t));
  self->data = gd;
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

void cleanup_global(dt_iop_module_so_t *self)
{
  dt_iop_highlights_global_data_t *gd = self->data;
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
  dt_opencl_free_kernel(gd->kernel_interpolate_bilinear);

  dt_opencl_free_kernel(gd->kernel_filmic_bspline_vertical);
  dt_opencl_free_kernel(gd->kernel_filmic_bspline_horizontal);
  dt_opencl_free_kernel(gd->kernel_filmic_wavelets_detail);

  free(self->data);
  self->data = NULL;
}

void init_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_highlights_data_t));
}

void cleanup_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

static void _set_quads(dt_iop_highlights_gui_data_t *g, GtkWidget *w)
{
  g->hlr_mask_mode = w && dt_bauhaus_widget_get_quad_active(w)
                   ? w == g->clip     ? DT_HIGHLIGHTS_MASK_CLIPPED
                   : w == g->combine  ? DT_HIGHLIGHTS_MASK_COMBINE
                   : w == g->strength ? DT_HIGHLIGHTS_MASK_STRENGTH
                   : DT_HIGHLIGHTS_MASK_CANDIDATING
                   : DT_HIGHLIGHTS_MASK_OFF;
  if(w != g->clip       ) dt_bauhaus_widget_set_quad_active(g->clip,        FALSE);
  if(w != g->candidating) dt_bauhaus_widget_set_quad_active(g->candidating, FALSE);
  if(w != g->combine    ) dt_bauhaus_widget_set_quad_active(g->combine,     FALSE);
  if(w != g->strength   ) dt_bauhaus_widget_set_quad_active(g->strength,    FALSE);
}

void gui_changed(dt_iop_module_t *self, GtkWidget *w, void *previous)
{
  dt_iop_highlights_gui_data_t *g = self->gui_data;
  dt_iop_highlights_params_t *p = self->params;

  const dt_image_t *img = &self->dev->image_storage;
  const uint32_t filters = img->buf_dsc.filters;
  const gboolean bayer = (filters != 0) && (filters != 9u);
  const gboolean rawprep = dt_image_is_rawprepare_supported(img);

  /* Sanitize mode if wrongfully
   - copied as part of the history of another pic or by preset / style or
   - by old edits that allowed opposed for non-raws
  */
  if(!rawprep)
  {
    p->mode = DT_IOP_HIGHLIGHTS_CLIP;
    dt_bauhaus_combobox_set_from_value(g->mode, p->mode);
    // not reported visually as so common and not relevant
  }

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
  const gboolean use_segmentation = p->mode == DT_IOP_HIGHLIGHTS_SEGMENTS;
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
    _set_quads(g, NULL);
  }
}

void gui_update(dt_iop_module_t *self)
{
  dt_iop_highlights_gui_data_t *g = self->gui_data;
  const dt_image_t *img = &self->dev->image_storage;
  const gboolean monochrome = dt_image_is_monochrome(img);
  // enable this per default if raw or sraw if not real monochrome
  self->default_enabled = dt_image_is_rawprepare_supported(img) && !monochrome;
  self->hide_enable_button = monochrome;
  gtk_stack_set_visible_child_name(GTK_STACK(self->widget), !monochrome ? "default" : "notapplicable");
  _set_quads(g, NULL);

  gui_changed(self, NULL, NULL);
}

void reload_defaults(dt_iop_module_t *self)
{
  // we might be called from presets update infrastructure => there is no image
  if(!self->dev || !dt_is_valid_imgid(self->dev->image_storage.id)) return;

  const dt_image_t *img = &self->dev->image_storage;
  const gboolean monochrome = dt_image_is_monochrome(img);
  const uint32_t filters = img->buf_dsc.filters;
  const gboolean rawprep = dt_image_is_rawprepare_supported(img);
  const gboolean sraw = rawprep && (filters == 0);
  const gboolean xtrans = rawprep && (filters == 9u);

  // enable this per default if raw or sraw if not real monochrome
  self->default_enabled = rawprep && !monochrome;
  self->hide_enable_button = monochrome;

  if(self->widget)
    gtk_stack_set_visible_child_name(GTK_STACK(self->widget), !monochrome ? "default" : "notapplicable");

  dt_iop_highlights_params_t *d = self->default_params;
  dt_iop_highlights_gui_data_t *g = self->gui_data;
  if(g)
  {
    // rebuild the complete menu depending on sensor type and possibly active but obsolete mode
    dt_bauhaus_combobox_clear(g->mode);

    dt_introspection_type_enum_tuple_t *values = self->so->get_f("mode")->Enum.values;

    if(!rawprep)
    {
      dt_bauhaus_combobox_add_introspection(g->mode, NULL, values, DT_IOP_HIGHLIGHTS_CLIP,
                                                                   DT_IOP_HIGHLIGHTS_OPPOSED);
      // As we only have clip available we remove all other options
      for(int i = 0; i < 6; i++) dt_bauhaus_combobox_remove_at(g->mode, 1);
    }
    else if(sraw)
    {
      dt_bauhaus_combobox_add_introspection(g->mode, NULL, values, DT_IOP_HIGHLIGHTS_OPPOSED,
                                                                   DT_IOP_HIGHLIGHTS_OPPOSED);
      dt_bauhaus_combobox_add_introspection(g->mode, NULL, values, DT_IOP_HIGHLIGHTS_CLIP,
                                                                   DT_IOP_HIGHLIGHTS_CLIP);
    }
    else
    {
      dt_bauhaus_combobox_add_introspection(g->mode, NULL, values, DT_IOP_HIGHLIGHTS_OPPOSED,
                                                                   xtrans
                                                                   ? DT_IOP_HIGHLIGHTS_SEGMENTS
                                                                   : DT_IOP_HIGHLIGHTS_LAPLACIAN);
    }
    _set_quads(g, NULL);
  }
  d->clip = MIN(d->clip, img->linear_response_limit);
  d->mode = rawprep ? DT_IOP_HIGHLIGHTS_OPPOSED : DT_IOP_HIGHLIGHTS_CLIP;
}

static void _quad_callback(GtkWidget *quad, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_highlights_gui_data_t *g = self->gui_data;
  _set_quads(g, quad);
  dt_dev_reprocess_center(self->dev);
}

void gui_focus(dt_iop_module_t *self, gboolean in)
{
  dt_iop_highlights_gui_data_t *g = self->gui_data;
  if(!in)
  {
    const gboolean was_visualize = (g->hlr_mask_mode != DT_HIGHLIGHTS_MASK_OFF);
    _set_quads(g, NULL);
    if(was_visualize) dt_dev_reprocess_center(self->dev);
  }
}

void gui_init(dt_iop_module_t *self)
{
  dt_iop_highlights_gui_data_t *g = IOP_GUI_ALLOC(highlights);
  GtkWidget *box_raw = self->widget = dt_gui_vbox();

  g->mode = dt_bauhaus_combobox_from_params(self, "mode");
  gtk_widget_set_tooltip_text(g->mode, _("highlight reconstruction method"));

  g->clip = dt_bauhaus_slider_from_params(self, "clip");
  dt_bauhaus_slider_set_digits(g->clip, 3);
  gtk_widget_set_tooltip_text(g->clip,
                              _("manually adjust the clipping threshold mostly used against magenta highlights.\n"
                                "you might use this for tuning 'laplacian', 'inpaint opposed' or 'segmentation' modes,\n"
                                "especially if camera white point is incorrect."));
  dt_bauhaus_widget_set_quad(g->clip, self, dtgtk_cairo_paint_showmask, TRUE, _quad_callback,
    _("visualize clipped highlights in a false color representation.\n"
      "the effective clipping level also depends on the reconstruction method."));

  g->combine = dt_bauhaus_slider_from_params(self, "combine");
  dt_bauhaus_slider_set_digits(g->combine, 0);
  gtk_widget_set_tooltip_text(g->combine, _("combine closely related clipped segments by morphological operations.\n"
                                            "this often leads to improved color reconstruction for tiny segments before dark background."));
  dt_bauhaus_widget_set_quad(g->combine, self, dtgtk_cairo_paint_showmask, TRUE, _quad_callback,
    _("visualize the combined segments in a false color representation."));

  g->candidating = dt_bauhaus_slider_from_params(self, "candidating");
  gtk_widget_set_tooltip_text(g->candidating, _("select inpainting after segmentation analysis.\n"
                                                "increase to favor candidates found in segmentation analysis, decrease for opposed means inpainting."));
  dt_bauhaus_slider_set_format(g->candidating, "%");
  dt_bauhaus_slider_set_digits(g->candidating, 0);
  dt_bauhaus_widget_set_quad(g->candidating, self, dtgtk_cairo_paint_showmask, TRUE, _quad_callback,
    _("visualize segments that are considered to have a good candidate in a false color representation."));

  g->recovery = dt_bauhaus_combobox_from_params(self, "recovery");
  gtk_widget_set_tooltip_text(g->recovery, _("approximate lost data in regions with all photosites clipped, the effect depends on segment size and border gradients.\n"
                                             "choose a mode tuned for segment size or the generic mode that tries to find best settings for every segment.\n"
                                             "small means areas with a diameter less than 25 pixels, large is best for greater than 100.\n"
                                             "the flat modes ignore narrow unclipped structures (like powerlines) to keep highlights rebuilt and avoid gradients."));

  g->strength = dt_bauhaus_slider_from_params(self, "strength");
  gtk_widget_set_tooltip_text(g->strength, _("set strength of rebuilding in regions with all photosites clipped."));
  dt_bauhaus_slider_set_format(g->strength, "%");
  dt_bauhaus_slider_set_digits(g->strength, 0);
  dt_bauhaus_widget_set_quad(g->strength, self, dtgtk_cairo_paint_showmask, TRUE, _quad_callback,
    _("show the effect that is added to already reconstructed data."));

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
  gtk_widget_set_tooltip_text(notapplicable, _("this module does not work with monochrome RAW files"));

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
