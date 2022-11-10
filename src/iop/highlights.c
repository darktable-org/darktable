/*
   This file is part of darktable,
   Copyright (C) 2010-2020 darktable developers.

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

#define MAX_NUM_SCALES 10

// Set to one to output intermediate image steps as PFM in /tmp
#define DEBUG_DUMP_PFM 0

#if DEBUG_DUMP_PFM
static void dump_PFM(const char *filename, const float* out, const uint32_t w, const uint32_t h)
{
  FILE *f = g_fopen(filename, "wb");
  fprintf(f, "PF\n%d %d\n-1.0\n", w, h);
  for(int j = h - 1 ; j >= 0 ; j--)
    for(int i = 0 ; i < w ; i++)
      for(int c = 0 ; c < 3 ; c++)
        fwrite(out + (j * w + i) * 4 + c, 1, sizeof(float), f);
  fclose(f);
}
#endif

DT_MODULE_INTROSPECTION(4, dt_iop_highlights_params_t)

/* As some of the internal algorithms use a smaller value for clipping than given by the UI
   the visualizing is wrong for those algos. It seems to be a a minor issue but sometimes significant.
   Please note, every mode defined in dt_iop_highlights_mode_t requires a value.
*/
static float highlights_clip_magics[6] = { 1.0f, 1.0f, 0.987f, 0.995f, 0.987f, 0.987f };

typedef enum dt_iop_highlights_mode_t
{
  DT_IOP_HIGHLIGHTS_CLIP = 0,    // $DESCRIPTION: "clip highlights"
  DT_IOP_HIGHLIGHTS_LCH = 1,     // $DESCRIPTION: "reconstruct in LCh"
  DT_IOP_HIGHLIGHTS_INPAINT = 2, // $DESCRIPTION: "reconstruct color"
  DT_IOP_HIGHLIGHTS_LAPLACIAN = 3, //$DESCRIPTION: "guided laplacians"
  DT_IOP_HIGHLIGHTS_SEGMENTS = 4, // $DESCRIPTION: "segmentation based"
  DT_IOP_HIGHLIGHTS_OPPOSED = 5,  // $DESCRIPTION: "inpaint opposed"
} dt_iop_highlights_mode_t;

typedef enum dt_atrous_wavelets_scales_t
{
  DT_WAVELETS_1_SCALE = 0,   // $DESCRIPTION: "4 px"
  DT_WAVELETS_2_SCALE = 1,   // $DESCRIPTION: "8 px"
  DT_WAVELETS_3_SCALE = 2,   // $DESCRIPTION: "16 px"
  DT_WAVELETS_4_SCALE = 3,   // $DESCRIPTION: "32 px"
  DT_WAVELETS_5_SCALE = 4,   // $DESCRIPTION: "64 px"
  DT_WAVELETS_6_SCALE = 5,   // $DESCRIPTION: "128 px"
  DT_WAVELETS_7_SCALE = 6,   // $DESCRIPTION: "256 px (slow)"
  DT_WAVELETS_8_SCALE = 7,   // $DESCRIPTION: "512 px (slow)"
  DT_WAVELETS_9_SCALE = 8,   // $DESCRIPTION: "1024 px (very slow)"
  DT_WAVELETS_10_SCALE = 9 ,  // $DESCRIPTION: "2048 px (insanely slow)"
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

typedef enum dt_segments_mask_t
{
  DT_SEGMENTS_MASK_OFF,
  DT_SEGMENTS_MASK_COMBINE,
  DT_SEGMENTS_MASK_CANDIDATING,
  DT_SEGMENTS_MASK_STRENGTH
} dt_segments_mask_t;

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
  int iterations; // $MIN: 1 $MAX: 64 $DEFAULT: 1 $DESCRIPTION: "iterations"
  dt_atrous_wavelets_scales_t scales; // $DEFAULT: DT_WAVELETS_6_SCALE $DESCRIPTION: "diameter of reconstruction"
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
  gboolean show_visualize;
  dt_segments_mask_t segmentation_mask_mode;
  dt_aligned_pixel_t chroma_correction;
  gboolean valid_chroma_correction;
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
  int kernel_highlights_false_color;

  int kernel_filmic_bspline_vertical;
  int kernel_filmic_bspline_horizontal;
  int kernel_filmic_wavelets_detail;
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

int legacy_params(dt_iop_module_t *self, const void *const old_params, const int old_version,
                  void *new_params, const int new_version)
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

#ifdef HAVE_OPENCL
static cl_int process_laplacian_bayer_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece,
                                         cl_mem dev_in, cl_mem dev_out, const dt_iop_roi_t *const roi_in,
                                         const dt_iop_roi_t *const roi_out, const dt_aligned_pixel_t clips);

int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_highlights_data_t *d = (dt_iop_highlights_data_t *)piece->data;
  dt_iop_highlights_gui_data_t *g = (dt_iop_highlights_gui_data_t *)self->gui_data;
  dt_iop_highlights_global_data_t *gd = (dt_iop_highlights_global_data_t *)self->global_data;

  const uint32_t filters = piece->pipe->dsc.filters;
  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;

  const gboolean fullpipe = piece->pipe->type & DT_DEV_PIXELPIPE_FULL;
  const gboolean visualizing = (g != NULL) ? g->show_visualize && fullpipe : FALSE;

  cl_int err = DT_OPENCL_DEFAULT_ERROR;
  cl_mem dev_xtrans = NULL;
  cl_mem dev_clips = NULL;
  // this works for bayer and X-Trans sensors
  if(visualizing)
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

    size_t sizes[] = { ROUNDUPDWD(width, devid), ROUNDUPDHT(height, devid), 1 };
    dt_opencl_set_kernel_args(devid, gd->kernel_highlights_false_color, 0, CLARG(dev_in), CLARG(dev_out),
      CLARG(width), CLARG(height), CLARG(roi_out->x), CLARG(roi_out->y), CLARG(filters), CLARG(dev_xtrans),
      CLARG(dev_clips));

    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_highlights_false_color, sizes);
    if(err != CL_SUCCESS) goto error;

    piece->pipe->mask_display = DT_DEV_PIXELPIPE_DISPLAY_PASSTHRU;
    piece->pipe->type |= DT_DEV_PIXELPIPE_FAST;
    dt_opencl_release_mem_object(dev_clips);
    dt_opencl_release_mem_object(dev_xtrans);
    return TRUE;
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
  else // default: if(d->mode == DT_IOP_HIGHLIGHTS_CLIP)
  {
    // raw images with clip mode (both bayer and xtrans)
    err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_highlights_1f_clip, width, height,
      CLARG(dev_in), CLARG(dev_out), CLARG(width), CLARG(height), CLARG(clip), CLARG(roi_out->x), CLARG(roi_out->y),
      CLARG(filters));
    if(err != CL_SUCCESS) goto error;
  }

  // update processed maximum
  if(d->mode !=  DT_IOP_HIGHLIGHTS_LAPLACIAN)
  {
    // The guided laplacian is the only mode that keeps signal scene-referred and doesn't clip highlights to 1
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
  dt_print(DT_DEBUG_OPENCL, "[opencl_highlights] couldn't enqueue kernel! %s\n", cl_errstr(err));
  return FALSE;
}
#endif

void tiling_callback(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
              const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out,
              struct dt_develop_tiling_t *tiling)
{
  dt_iop_highlights_data_t *d = (dt_iop_highlights_data_t *)piece->data;
  const uint32_t filters = piece->pipe->dsc.filters;

  if(d->mode == DT_IOP_HIGHLIGHTS_LAPLACIAN && filters && filters != 9u)
  {
    // Bayer CFA and guided laplacian method : prepare for wavelets decomposition.
    const float scale = fmaxf(piece->iscale / roi_in->scale, 1.f);
    const float final_radius = (float)((int)(1 << d->scales)) / scale;
    const int scales = CLAMP((int)round(log2f(final_radius)), 0, MAX_NUM_SCALES);
    const int max_filter_radius = (1 << scales);

    // Warning : in and out are single-channel in RAW mode
    // in + out + interpolated + tmp + 2 * LF + HF + mask
    if(filters) // RAW
    {
      tiling->factor = 2.f + 6.f * 4;
      tiling->factor_cl = 2.f + 6.f * 4;

      // The wavelets decomposition uses a temp buffer of size 4 × width
      tiling->maxbuf = 1.f / roi_in->height * 4.f;
    }
    else
    {
      tiling->factor = 2.f + 6.f;
      tiling->factor_cl = 2.f + 6.f;

      // The wavelets decomposition uses a temp buffer of size 4 × width
      tiling->maxbuf = 1.f / roi_in->height;
    }

    // No temp buffer on GPU
    tiling->maxbuf_cl = 1.0f;
    tiling->overhead = 0;

    // Note : if we were not doing anything iterative,
    // max_filter_radius would not need to be factored more.
    // Since we are iterating within tiles, we need more padding.
    // The clean way of doing it would be an internal tiling mechanism
    // where we restitch the tiles between each new iteration.
    tiling->overlap = max_filter_radius * 1.5f;
    tiling->xalign = 1;
    tiling->yalign = 1;

    return;
  }

  if(d->mode == DT_IOP_HIGHLIGHTS_SEGMENTS)
  {
    // even if the algorithm can't tile we want to calculate memory for pixelpipe checks and a possible warning
    const int segments = roi_out->width * roi_out->height / 4000; // segments per mpix
    if(filters == 9u)
    {
      tiling->xalign = 3;
      tiling->yalign = 3;
    }
    else
    {
      tiling->xalign = 2;
      tiling->yalign = 2;
    }
    tiling->overlap = 0;
    tiling->overhead = segments * 5 * 5 * sizeof(int); // segmentation stuff
    tiling->factor = 3.0f;
    tiling->maxbuf = 1.0f;
    return;
  }

  if(d->mode == DT_IOP_HIGHLIGHTS_OPPOSED)
  {
    if(filters == 9u)
    {
      tiling->xalign = 3;
      tiling->yalign = 3;
    }
    else
    {
      tiling->xalign = 2;
      tiling->yalign = 2;
    }
    tiling->factor = 2.0f; // in & out plus plane buffers including some border safety
    tiling->overlap = 0;
    tiling->maxbuf = 1.0f;
    tiling->overhead = 0;
    return;
  }

  tiling->factor = 2.0f;  // in + out
  tiling->maxbuf = 1.0f;
  tiling->overhead = 0;

  if(filters == 9u)
  {
    // xtrans
    tiling->xalign = 6;
    tiling->yalign = 6;
    tiling->overlap = (d->mode == DT_IOP_HIGHLIGHTS_LCH) ? 2 : 0;
  }
  else if(filters)
  {
    // bayer
    tiling->xalign = 2;
    tiling->yalign = 2;
    tiling->overlap = (d->mode == DT_IOP_HIGHLIGHTS_LCH) ? 1 : 0;
  }
  else
  {
    // non-raw
    tiling->xalign = 1;
    tiling->yalign = 1;
    tiling->overlap = 0;
  }
}

/* interpolate value for a pixel, ideal via ratio to nearby pixel */
static inline float interp_pix_xtrans(const int ratio_next,
                                      const ssize_t offset_next,
                                      const float clip0, const float clip_next,
                                      const float *const in,
                                      const float *const ratios)
{
  assert(ratio_next != 0);
  // it's OK to exceed clipping of current pixel's color based on a
  // neighbor -- that is the purpose of interpolating highlight
  // colors
  const float clip_val = fmaxf(clip0, clip_next);
  if(in[offset_next] >= clip_next - 1e-5f)
  {
    // next pixel is also clipped
    return clip_val;
  }
  else
  {
    // set this pixel in ratio to the next
    assert(ratio_next != 0);
    if(ratio_next > 0)
      return fminf(in[offset_next] / ratios[ratio_next], clip_val);
    else
      return fminf(in[offset_next] * ratios[-ratio_next], clip_val);
  }
}

static inline void interpolate_color_xtrans(const void *const ivoid, void *const ovoid,
                                            const dt_iop_roi_t *const roi_in,
                                            const dt_iop_roi_t *const roi_out,
                                            int dim, int dir, int other,
                                            const float *const clip,
                                            const uint8_t (*const xtrans)[6],
                                            const int pass)
{
  // In Bayer each row/col has only green/red or green/blue
  // transitions, hence can reconstruct color by single ratio per
  // row. In x-trans there can be transitions between arbitrary colors
  // in a row/col (and 2x2 green blocks which provide no color
  // transition information). Hence calculate multiple color ratios
  // for each row/col.

  // Lookup for color ratios, e.g. red -> blue is roff[0][2] and blue
  // -> red is roff[2][0]. Returned value is an index into ratios. If
  // negative, then need to invert the ratio. Identity color
  // transitions aren't used.
  const int roff[3][3] = {{ 0, -1, -2},
                          { 1,  0, -3},
                          { 2,  3,  0}};
  // record ratios of color transitions 0:unused, 1:RG, 2:RB, and 3:GB
  dt_aligned_pixel_t ratios = {1.0f, 1.0f, 1.0f, 1.0f};

  // passes are 0:+x, 1:-x, 2:+y, 3:-y
  // dims are 0:traverse a row, 1:traverse a column
  // dir is 1:left to right, -1: right to left
  int i = (dim == 0) ? 0 : other;
  int j = (dim == 0) ? other : 0;
  const ssize_t offs = (ssize_t)(dim ? roi_out->width : 1) * ((dir < 0) ? -1 : 1);
  const ssize_t offl = offs - (dim ? 1 : roi_out->width);
  const ssize_t offr = offs + (dim ? 1 : roi_out->width);
  int beg, end;
  if(dir == 1)
  {
    beg = 0;
    end = (dim == 0) ? roi_out->width : roi_out->height;
  }
  else
  {
    beg = ((dim == 0) ? roi_out->width : roi_out->height) - 1;
    end = -1;
  }

  float *in, *out;
  if(dim == 1)
  {
    out = (float *)ovoid + (size_t)i + (size_t)beg * roi_out->width;
    in = (float *)ivoid + (size_t)i + (size_t)beg * roi_in->width;
  }
  else
  {
    out = (float *)ovoid + (size_t)beg + (size_t)j * roi_out->width;
    in = (float *)ivoid + (size_t)beg + (size_t)j * roi_in->width;
  }

  for(int k = beg; k != end; k += dir)
  {
    if(dim == 1)
      j = k;
    else
      i = k;

    const uint8_t f0 = FCxtrans(j, i, roi_in, xtrans);
    const uint8_t f1 = FCxtrans(dim ? (j + dir) : j, dim ? i : (i + dir), roi_in, xtrans);
    const uint8_t fl = FCxtrans(dim ? (j + dir) : (j - 1), dim ? (i - 1) : (i + dir), roi_in, xtrans);
    const uint8_t fr = FCxtrans(dim ? (j + dir) : (j + 1), dim ? (i + 1) : (i + dir), roi_in, xtrans);
    const float clip0 = clip[f0];
    const float clip1 = clip[f1];
    const float clipl = clip[fl];
    const float clipr = clip[fr];
    const float clip_max = fmaxf(fmaxf(clip[0], clip[1]), clip[2]);

    if(i == 0 || i == roi_out->width - 1 || j == 0 || j == roi_out->height - 1)
    {
      if(pass == 3) out[0] = fminf(clip_max, in[0]);
    }
    else
    {
      // ratio to next pixel if this & next are unclamped and not in
      // 2x2 green block
      if((f0 != f1) &&
          (in[0] < clip0 && in[0] > 1e-5f) &&
          (in[offs] < clip1 && in[offs] > 1e-5f))
      {
        const int r = roff[f0][f1];
        assert(r != 0);
        if(r > 0)
          ratios[r] = (3.f * ratios[r] + (in[offs] / in[0])) / 4.f;
        else
          ratios[-r] = (3.f * ratios[-r] + (in[0] / in[offs])) / 4.f;
      }

      if(in[0] >= clip0 - 1e-5f)
      {
        // interplate color for clipped pixel
        float add;
        if(f0 != f1)
          // next pixel is different color
          add =
            interp_pix_xtrans(roff[f0][f1], offs, clip0, clip1, in, ratios);
        else
          // at start of 2x2 green block, look diagonally
          add = (fl != f0) ?
            interp_pix_xtrans(roff[f0][fl], offl, clip0, clipl, in, ratios) :
            interp_pix_xtrans(roff[f0][fr], offr, clip0, clipr, in, ratios);

        if(pass == 0)
          out[0] = add;
        else if(pass == 3)
          out[0] = fminf(clip_max, (out[0] + add) / 4.0f);
        else
          out[0] += add;
      }
      else
      {
        // pixel is not clipped
        if(pass == 3) out[0] = in[0];
      }
    }
    out += offs;
    in += offs;
  }
}

static inline void interpolate_color(const void *const ivoid, void *const ovoid,
                                     const dt_iop_roi_t *const roi_out, int dim, int dir, int other,
                                     const float *clip, const uint32_t filters, const int pass)
{
  float ratio = 1.0f;
  float *in, *out;

  int i = 0, j = 0;
  if(dim == 0)
    j = other;
  else
    i = other;
  ssize_t offs = dim ? roi_out->width : 1;
  if(dir < 0) offs = -offs;
  int beg, end;
  if(dim == 0 && dir == 1)
  {
    beg = 0;
    end = roi_out->width;
  }
  else if(dim == 0 && dir == -1)
  {
    beg = roi_out->width - 1;
    end = -1;
  }
  else if(dim == 1 && dir == 1)
  {
    beg = 0;
    end = roi_out->height;
  }
  else if(dim == 1 && dir == -1)
  {
    beg = roi_out->height - 1;
    end = -1;
  }
  else
    return;

  if(dim == 1)
  {
    out = (float *)ovoid + i + (size_t)beg * roi_out->width;
    in = (float *)ivoid + i + (size_t)beg * roi_out->width;
  }
  else
  {
    out = (float *)ovoid + beg + (size_t)j * roi_out->width;
    in = (float *)ivoid + beg + (size_t)j * roi_out->width;
  }
  for(int k = beg; k != end; k += dir)
  {
    if(dim == 1)
      j = k;
    else
      i = k;
    const float clip0 = clip[FC(j, i, filters)];
    const float clip1 = clip[FC(dim ? (j + 1) : j, dim ? i : (i + 1), filters)];
    if(i == 0 || i == roi_out->width - 1 || j == 0 || j == roi_out->height - 1)
    {
      if(pass == 3) out[0] = in[0];
    }
    else
    {
      if(in[0] < clip0 && in[0] > 1e-5f)
      { // both are not clipped
        if(in[offs] < clip1 && in[offs] > 1e-5f)
        { // update ratio, exponential decay. ratio = in[odd]/in[even]
          if(k & 1)
            ratio = (3.0f * ratio + in[0] / in[offs]) / 4.0f;
          else
            ratio = (3.0f * ratio + in[offs] / in[0]) / 4.0f;
        }
      }

      if(in[0] >= clip0 - 1e-5f)
      { // in[0] is clipped, restore it as in[1] adjusted according to ratio
        float add = 0.0f;
        if(in[offs] >= clip1 - 1e-5f)
          add = fmaxf(clip0, clip1);
        else if(k & 1)
          add = in[offs] * ratio;
        else
          add = in[offs] / ratio;

        if(pass == 0)
          out[0] = add;
        else if(pass == 3)
          out[0] = (out[0] + add) / 4.0f;
        else
          out[0] += add;
      }
      else
      {
        if(pass == 3) out[0] = in[0];
      }
    }
    out += offs;
    in += offs;
  }
}

/*
 * these 2 constants were computed using following Sage code:
 *
 * sqrt3 = sqrt(3)
 * sqrt12 = sqrt(12) # 2*sqrt(3)
 *
 * print 'sqrt3 = ', sqrt3, ' ~= ', RealField(128)(sqrt3)
 * print 'sqrt12 = ', sqrt12, ' ~= ', RealField(128)(sqrt12)
 */
#define SQRT3 1.7320508075688772935274463415058723669L
#define SQRT12 3.4641016151377545870548926830117447339L // 2*SQRT3

static void process_lch_bayer(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
                              void *const ovoid, const dt_iop_roi_t *const roi_in,
                              const dt_iop_roi_t *const roi_out, const float clip)
{
  const uint32_t filters = piece->pipe->dsc.filters;

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(clip, filters, ivoid, ovoid, roi_out) \
  schedule(static) collapse(2)
#endif
  for(int j = 0; j < roi_out->height; j++)
  {
    for(int i = 0; i < roi_out->width; i++)
    {
      float *const out = (float *)ovoid + (size_t)roi_out->width * j + i;
      const float *const in = (float *)ivoid + (size_t)roi_out->width * j + i;

      if(i == roi_out->width - 1 || j == roi_out->height - 1)
      {
        // fast path for border
        out[0] = MIN(clip, in[0]);
      }
      else
      {
        int clipped = 0;

        // sample 1 bayer block. thus we will have 2 green values.
        float R = 0.0f, Gmin = FLT_MAX, Gmax = -FLT_MAX, B = 0.0f;
        for(int jj = 0; jj <= 1; jj++)
        {
          for(int ii = 0; ii <= 1; ii++)
          {
            const float val = in[(size_t)jj * roi_out->width + ii];

            clipped = (clipped || (val > clip));

            const int c = FC(j + jj + roi_out->y, i + ii + roi_out->x, filters);
            switch(c)
            {
              case 0:
                R = val;
                break;
              case 1:
                Gmin = MIN(Gmin, val);
                Gmax = MAX(Gmax, val);
                break;
              case 2:
                B = val;
                break;
            }
          }
        }

        if(clipped)
        {
          const float Ro = MIN(R, clip);
          const float Go = MIN(Gmin, clip);
          const float Bo = MIN(B, clip);

          const float L = (R + Gmax + B) / 3.0f;

          float C = SQRT3 * (R - Gmax);
          float H = 2.0f * B - Gmax - R;

          const float Co = SQRT3 * (Ro - Go);
          const float Ho = 2.0f * Bo - Go - Ro;

          if(R != Gmax && Gmax != B)
          {
            const float ratio = sqrtf((Co * Co + Ho * Ho) / (C * C + H * H));
            C *= ratio;
            H *= ratio;
          }

          dt_aligned_pixel_t RGB = { 0.0f, 0.0f, 0.0f };

          /*
           * backtransform proof, sage:
           *
           * R,G,B,L,C,H = var('R,G,B,L,C,H')
           * solve([L==(R+G+B)/3, C==sqrt(3)*(R-G), H==2*B-G-R], R, G, B)
           *
           * result:
           * [[R == 1/6*sqrt(3)*C - 1/6*H + L, G == -1/6*sqrt(3)*C - 1/6*H + L, B == 1/3*H + L]]
           */
          RGB[0] = L - H / 6.0f + C / SQRT12;
          RGB[1] = L - H / 6.0f - C / SQRT12;
          RGB[2] = L + H / 3.0f;

          out[0] = RGB[FC(j + roi_out->y, i + roi_out->x, filters)];
        }
        else
        {
          out[0] = in[0];
        }
      }
    }
  }
}

static void process_lch_xtrans(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
                               void *const ovoid, const dt_iop_roi_t *const roi_in,
                               const dt_iop_roi_t *const roi_out, const float clip)
{
  const uint8_t(*const xtrans)[6] = (const uint8_t(*const)[6])piece->pipe->dsc.xtrans;

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(clip, ivoid, ovoid, roi_in, roi_out, xtrans) \
  schedule(static)
#endif
  for(int j = 0; j < roi_out->height; j++)
  {
    float *out = (float *)ovoid + (size_t)roi_out->width * j;
    float *in = (float *)ivoid + (size_t)roi_in->width * j;

    // bit vector used as ring buffer to remember clipping of current
    // and last two columns, checking current pixel and its vertical
    // neighbors
    int cl = 0;

    for(int i = 0; i < roi_out->width; i++)
    {
      // update clipping ring buffer
      cl = (cl << 1) & 6;
      if(j >= 2 && j <= roi_out->height - 3)
      {
        cl |= (in[-roi_in->width] > clip) | (in[0] > clip) | (in[roi_in->width] > clip);
      }

      if(i < 2 || i > roi_out->width - 3 || j < 2 || j > roi_out->height - 3)
      {
        // fast path for border
        out[0] = MIN(clip, in[0]);
      }
      else
      {
        // if current pixel is clipped, always reconstruct
        int clipped = (in[0] > clip);
        if(!clipped)
        {
          clipped = cl;
          if(clipped)
          {
            // If the ring buffer can't show we are in an obviously
            // unclipped region, this is the slow case: check if there
            // is any 3x3 block touching the current pixel which has
            // no clipping, as then don't need to reconstruct the
            // current pixel. This avoids zippering in edge
            // transitions from clipped to unclipped areas. The
            // X-Trans sensor seems prone to this, unlike Bayer, due
            // to its irregular pattern.
            for(int offset_j = -2; offset_j <= 0; offset_j++)
            {
              for(int offset_i = -2; offset_i <= 0; offset_i++)
              {
                if(clipped)
                {
                  clipped = 0;
                  for(int jj = offset_j; jj <= offset_j + 2; jj++)
                  {
                    for(int ii = offset_i; ii <= offset_i + 2; ii++)
                    {
                      const float val = in[(ssize_t)jj * roi_in->width + ii];
                      clipped = (clipped || (val > clip));
                    }
                  }
                }
              }
            }
          }
        }

        if(clipped)
        {
          dt_aligned_pixel_t mean = { 0.0f, 0.0f, 0.0f };
          dt_aligned_pixel_t RGBmax = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
          int cnt[3] = { 0, 0, 0 };

          for(int jj = -1; jj <= 1; jj++)
          {
            for(int ii = -1; ii <= 1; ii++)
            {
              const float val = in[(ssize_t)jj * roi_in->width + ii];
              const int c = FCxtrans(j+jj, i+ii, roi_in, xtrans);
              mean[c] += val;
              cnt[c]++;
              RGBmax[c] = MAX(RGBmax[c], val);
            }
          }

          const float Ro = MIN(mean[0]/cnt[0], clip);
          const float Go = MIN(mean[1]/cnt[1], clip);
          const float Bo = MIN(mean[2]/cnt[2], clip);

          const float R = RGBmax[0];
          const float G = RGBmax[1];
          const float B = RGBmax[2];

          const float L = (R + G + B) / 3.0f;

          float C = SQRT3 * (R - G);
          float H = 2.0f * B - G - R;

          const float Co = SQRT3 * (Ro - Go);
          const float Ho = 2.0f * Bo - Go - Ro;

          if(R != G && G != B)
          {
            const float ratio = sqrtf((Co * Co + Ho * Ho) / (C * C + H * H));
            C *= ratio;
            H *= ratio;
          }

          dt_aligned_pixel_t RGB = { 0.0f, 0.0f, 0.0f };

          RGB[0] = L - H / 6.0f + C / SQRT12;
          RGB[1] = L - H / 6.0f - C / SQRT12;
          RGB[2] = L + H / 3.0f;

          out[0] = RGB[FCxtrans(j, i, roi_out, xtrans)];
        }
        else
          out[0] = in[0];
      }
      out++;
      in++;
    }
  }
}

#undef SQRT3
#undef SQRT12

static void _interpolate_and_mask(const float *const restrict input,
                                  float *const restrict interpolated,
                                  float *const restrict clipping_mask,
                                  const dt_aligned_pixel_t clips,
                                  const dt_aligned_pixel_t wb,
                                  const uint32_t filters,
                                  const size_t width, const size_t height)
{
  // Bilinear interpolation
  #ifdef _OPENMP
  #pragma omp parallel for default(none) \
    dt_omp_firstprivate(width, height, clips, filters)  \
    dt_omp_sharedconst(input, interpolated, clipping_mask) \
    shared(wb) \
    schedule(static)
  #endif
  for(size_t i = 0; i < height; i++)
    for(size_t j = 0; j < width; j++)
    {
      const size_t c = FC(i, j, filters);
      const size_t i_center = i * width;
      const float center = input[i_center + j];

      float R = 0.f;
      float G = 0.f;
      float B = 0.f;

      int R_clipped = 0;
      int G_clipped = 0;
      int B_clipped = 0;

      if(i == 0 || j == 0 || i == height - 1 || j == width - 1)
      {
        // We are on the image edges. We don't need to demosaic,
        // just set R = G = B = center and record clipping.
        // This will introduce a marginal error close to edges, mostly irrelevant
        // because we are dealing with local averages anyway, later on.
        // Also we remosaic the image at the end, so only the relevant channel gets picked.
        // Finally, it's unlikely that the borders of the image get clipped due to vignetting.
        R = G = B = center;
        R_clipped = G_clipped = B_clipped = (center > clips[c]);
      }
      else
      {
        const size_t i_prev = (i - 1) * width;
        const size_t i_next = (i + 1) * width;
        const size_t j_prev = (j - 1);
        const size_t j_next = (j + 1);

        const float north = input[i_prev + j];
        const float south = input[i_next + j];
        const float west = input[i_center + j_prev];
        const float east = input[i_center + j_next];

        const float north_east = input[i_prev + j_next];
        const float north_west = input[i_prev + j_prev];
        const float south_east = input[i_next + j_next];
        const float south_west = input[i_next + j_prev];

        if(c == GREEN) // green pixel
        {
          G = center;
          G_clipped = (center > clips[GREEN]);
        }
        else // non-green pixel
        {
          // interpolate inside an X/Y cross
          G = (north + south + east + west) / 4.f;
          G_clipped = (north > clips[GREEN] || south > clips[GREEN] || east > clips[GREEN] || west > clips[GREEN]);
        }

        if(c == RED ) // red pixel
        {
          R = center;
          R_clipped = (center > clips[RED]);
        }
        else // non-red pixel
        {
          if(FC(i - 1, j, filters) == RED && FC(i + 1, j, filters) == RED)
          {
            // we are on a red column, so interpolate column-wise
            R = (north + south) / 2.f;
            R_clipped = (north > clips[RED] || south > clips[RED]);
          }
          else if(FC(i, j - 1, filters) == RED && FC(i, j + 1, filters) == RED)
          {
            // we are on a red row, so interpolate row-wise
            R = (west + east) / 2.f;
            R_clipped = (west > clips[RED] || east > clips[RED]);
          }
          else
          {
            // we are on a blue row, so interpolate inside a square
            R = (north_west + north_east + south_east + south_west) / 4.f;
            R_clipped = (north_west > clips[RED] || north_east > clips[RED] || south_west > clips[RED]
                          || south_east > clips[RED]);
          }
        }

        if(c == BLUE ) // blue pixel
        {
          B = center;
          B_clipped = (center > clips[BLUE]);
        }
        else // non-blue pixel
        {
          if(FC(i - 1, j, filters) == BLUE && FC(i + 1, j, filters) == BLUE)
          {
            // we are on a blue column, so interpolate column-wise
            B = (north + south) / 2.f;
            B_clipped = (north > clips[BLUE] || south > clips[BLUE]);
          }
          else if(FC(i, j - 1, filters) == BLUE && FC(i, j + 1, filters) == BLUE)
          {
            // we are on a red row, so interpolate row-wise
            B = (west + east) / 2.f;
            B_clipped = (west > clips[BLUE] || east > clips[BLUE]);
          }
          else
          {
            // we are on a red row, so interpolate inside a square
            B = (north_west + north_east + south_east + south_west) / 4.f;

            B_clipped = (north_west > clips[BLUE] || north_east > clips[BLUE] || south_west > clips[BLUE]
                        || south_east > clips[BLUE]);
          }
        }
      }

      dt_aligned_pixel_t RGB = { R, G, B, sqrtf(sqf(R) + sqf(G) + sqf(B)) };
      dt_aligned_pixel_t clipped = { R_clipped, G_clipped, B_clipped, (R_clipped || G_clipped || B_clipped) };

      for_each_channel(k, aligned(RGB, interpolated, clipping_mask, clipped, wb))
      {
        const size_t idx = (i * width + j) * 4 + k;
        interpolated[idx] = fmaxf(RGB[k] / wb[k], 0.f);
        clipping_mask[idx] = clipped[k];
      }
    }
}

static void _remosaic_and_replace(const float *const restrict interpolated,
                                  float *const restrict output,
                                  const dt_aligned_pixel_t wb,
                                  const uint32_t filters,
                                  const size_t width, const size_t height)
{
  // Take RGB ratios and norm, reconstruct RGB and remosaic the image
  #ifdef _OPENMP
  #pragma omp parallel for default(none) \
    dt_omp_firstprivate(width, height, filters)  \
    dt_omp_sharedconst(output, interpolated) \
    shared(wb) \
    schedule(static)
  #endif
  for(size_t i = 0; i < height; i++)
    for(size_t j = 0; j < width; j++)
    {
      const size_t c = FC(i, j, filters);
      const size_t idx = i * width + j;
      const size_t index = idx * 4;
      output[idx] = fmaxf(interpolated[index + c] * wb[c], 0.f);
    }
}

typedef enum diffuse_reconstruct_variant_t
{
  DIFFUSE_RECONSTRUCT_RGB = 0,
  DIFFUSE_RECONSTRUCT_CHROMA
} diffuse_reconstruct_variant_t;


enum wavelets_scale_t
{
  ANY_SCALE   = 1 << 0, // any wavelets scale   : reconstruct += HF
  FIRST_SCALE = 1 << 1, // first wavelets scale : reconstruct = 0
  LAST_SCALE  = 1 << 2, // last wavelets scale  : reconstruct += residual
};


static uint8_t scale_type(const int s, const int scales)
{
  uint8_t scale = ANY_SCALE;
  if(s == 0) scale |= FIRST_SCALE;
  if(s == scales - 1) scale |= LAST_SCALE;
  return scale;
}


static inline void guide_laplacians(const float *const restrict high_freq, const float *const restrict low_freq,
                                    const float *const restrict clipping_mask,
                                    float *const restrict output,
                                    const size_t width, const size_t height, const int mult,
                                    const float noise_level, const int salt,
                                    const uint8_t scale, const float radius_sq)
{
  float *const restrict out = DT_IS_ALIGNED(output);
  const float *const restrict LF = DT_IS_ALIGNED(low_freq);
  const float *const restrict HF = DT_IS_ALIGNED(high_freq);

#ifdef _OPENMP
#pragma omp parallel for default(none)                                                                            \
    dt_omp_firstprivate(out, clipping_mask, HF, LF, height, width, mult, noise_level, salt, scale, radius_sq) \
    schedule(static)
#endif
  for(size_t row = 0; row < height; ++row)
  {
    // interleave the order in which we process the rows so that we minimize cache misses
    const int i = dwt_interleave_rows(row, height, mult);
    // compute the 'above' and 'below' coordinates, clamping them to the image, once for the entire row
    const size_t i_neighbours[3]
      = { MAX((int)(i - mult), (int)0) * width,            // x - mult
          i * width,                                       // x
          MIN((int)(i + mult), (int)height - 1) * width }; // x + mult
    for(int j = 0; j < width; ++j)
    {
      const size_t idx = (i * width + j);
      const size_t index = idx * 4;

      // fetch the clipping mask opacity : opaque (alpha = 100 %) where clipped
      const float alpha = clipping_mask[index + ALPHA];
      const float alpha_comp = 1.f - clipping_mask[index + ALPHA];

      dt_aligned_pixel_t high_frequency = { HF[index + 0], HF[index + 1], HF[index + 2], HF[index + 3] };

      if(alpha > 0.f) // reconstruct
      {
        // non-local neighbours coordinates
        const size_t j_neighbours[3]
          = { MAX((int)(j - mult), (int)0),           // y - mult
              j,                                      // y
              MIN((int)(j + mult), (int)width - 1) }; // y + mult

        // fetch non-local pixels and store them locally and contiguously
        dt_aligned_pixel_t neighbour_pixel_HF[9];
        for_four_channels(c, aligned(neighbour_pixel_HF, HF))
        {
          neighbour_pixel_HF[3 * 0 + 0][c] = HF[4 * (i_neighbours[0] + j_neighbours[0]) + c];
          neighbour_pixel_HF[3 * 0 + 1][c] = HF[4 * (i_neighbours[0] + j_neighbours[1]) + c];
          neighbour_pixel_HF[3 * 0 + 2][c] = HF[4 * (i_neighbours[0] + j_neighbours[2]) + c];

          neighbour_pixel_HF[3 * 1 + 0][c] = HF[4 * (i_neighbours[1] + j_neighbours[0]) + c];
          neighbour_pixel_HF[3 * 1 + 1][c] = HF[4 * (i_neighbours[1] + j_neighbours[1]) + c];
          neighbour_pixel_HF[3 * 1 + 2][c] = HF[4 * (i_neighbours[1] + j_neighbours[2]) + c];

          neighbour_pixel_HF[3 * 2 + 0][c] = HF[4 * (i_neighbours[2] + j_neighbours[0]) + c];
          neighbour_pixel_HF[3 * 2 + 1][c] = HF[4 * (i_neighbours[2] + j_neighbours[1]) + c];
          neighbour_pixel_HF[3 * 2 + 2][c] = HF[4 * (i_neighbours[2] + j_neighbours[2]) + c];
        }

        // Compute the linear fit of the laplacian of chromaticity against the laplacian of the norm
        // that is the chromaticity filter guided by the norm

        // Get the local average per channel
        dt_aligned_pixel_t means_HF = { 0.f, 0.f, 0.f, 0.f };
        for(size_t k = 0; k < 9; k++)
          for_each_channel(c, aligned(neighbour_pixel_HF, means_HF : 64))
          {
            means_HF[c] += neighbour_pixel_HF[k][c] / 9.f;
          }

        // Get the local variance per channel
        dt_aligned_pixel_t variance_HF = { 0.f, 0.f, 0.f, 0.f };
        for(size_t k = 0; k < 9; k++)
          for_each_channel(c, aligned(variance_HF, neighbour_pixel_HF, means_HF))
          {
            variance_HF[c] += sqf(neighbour_pixel_HF[k][c] - means_HF[c]) / 9.f;
          }

        // Find the channel most likely to contain details = max( variance(HF) )
        size_t guiding_channel_HF = ALPHA;
        float guiding_value_HF = 0.f;
        for(size_t c = 0; c < 3; ++c)
        {
          if(variance_HF[c] > guiding_value_HF)
          {
            guiding_value_HF = variance_HF[c];
            guiding_channel_HF = c;
          }
        }

        // Compute the linear regression channel = f(guide)
        dt_aligned_pixel_t covariance_HF = { 0.f, 0.f, 0.f, 0.f };
        for(size_t k = 0; k < 9; k++)
          for_each_channel(c, aligned(variance_HF, covariance_HF, neighbour_pixel_HF, means_HF))
          {
            covariance_HF[c] += (neighbour_pixel_HF[k][c] - means_HF[c])
                                * (neighbour_pixel_HF[k][guiding_channel_HF] - means_HF[guiding_channel_HF]) / 9.f;
          }

        const float scale_multiplier = 1.f / radius_sq;
        const dt_aligned_pixel_t alpha_ch = { clipping_mask[index + RED], clipping_mask[index + GREEN], clipping_mask[index + BLUE], clipping_mask[index + ALPHA] };

        dt_aligned_pixel_t a_HF, b_HF;
        for_each_channel(c, aligned(out, neighbour_pixel_HF, a_HF, b_HF, covariance_HF, variance_HF, means_HF, high_frequency, alpha_ch))
        {
          // Get a and b s.t. y = a * x + b, y = test data, x = guide
          a_HF[c] = fmaxf(covariance_HF[c] / (variance_HF[guiding_channel_HF]), 0.f);
          b_HF[c] = means_HF[c] - a_HF[c] * means_HF[guiding_channel_HF];

          high_frequency[c] = alpha_ch[c] * scale_multiplier * (a_HF[c] * high_frequency[guiding_channel_HF] + b_HF[c])
                            + (1.f - alpha_ch[c] * scale_multiplier) * high_frequency[c];
        }
      }

      if((scale & FIRST_SCALE))
      {
        // out is not inited yet
        for_each_channel(c, aligned(out, high_frequency : 64))
          out[index + c] = high_frequency[c];
      }
      else
      {
        // just accumulate HF
        for_each_channel(c, aligned(out, high_frequency : 64))
          out[index + c] += high_frequency[c];
      }

      if((scale & LAST_SCALE))
      {
        // add the residual and clamp
        for_each_channel(c, aligned(out, LF, high_frequency : 64))
          out[index + c] = fmaxf(out[index + c] + LF[index + c], 0.f);
      }

      // Last step of RGB reconstruct : add noise
      if((scale & LAST_SCALE) && salt && alpha > 0.f)
      {
        // Init random number generator
        uint32_t DT_ALIGNED_ARRAY state[4] = { splitmix32(j + 1), splitmix32((j + 1) * (i + 3)), splitmix32(1337), splitmix32(666) };
        xoshiro128plus(state);
        xoshiro128plus(state);
        xoshiro128plus(state);
        xoshiro128plus(state);

        dt_aligned_pixel_t noise = { 0.f };
        dt_aligned_pixel_t sigma = { 0.20f };
        const int DT_ALIGNED_ARRAY flip[4] = { TRUE, FALSE, TRUE, FALSE };

        for_each_channel(c,aligned(out, sigma)) sigma[c] = out[index + c] * noise_level;

        // create statistical noise
        dt_noise_generator_simd(DT_NOISE_POISSONIAN, out + index, sigma, flip, state, noise);

        // Save the noisy interpolated image
        for_each_channel(c,aligned(out, noise: 64))
        {
          // Ensure the noise only brightens the image, since it's clipped
          noise[c] = out[index + c] + fabsf(noise[c] - out[index + c]);
          out[index + c] = fmaxf(alpha * noise[c] + alpha_comp * out[index + c], 0.f);
        }
      }

      if((scale & LAST_SCALE))
      {
        // Break the RGB channels into ratios/norm for the next step of reconstruction
        const float norm = fmaxf(sqrtf(sqf(out[index + RED]) + sqf(out[index + GREEN]) + sqf(out[index + BLUE])), 1e-6f);
        for_each_channel(c, aligned(out : 64)) out[index + c] /= norm;
        out[index + ALPHA] = norm;
      }
    }
  }
}

static inline void heat_PDE_diffusion(const float *const restrict high_freq, const float *const restrict low_freq,
                                      const float *const restrict clipping_mask,
                                      float *const restrict output, const size_t width, const size_t height,
                                      const int mult, const uint8_t scale, const float first_order_factor)
{
  // Simultaneous inpainting for image structure and texture using anisotropic heat transfer model
  // https://www.researchgate.net/publication/220663968
  // modified as follow :
  //  * apply it in a multi-scale wavelet setup : we basically solve it twice, on the wavelets LF and HF layers.
  //  * replace the manual texture direction/distance selection by an automatic detection similar to the structure one,
  //  * generalize the framework for isotropic diffusion and anisotropic weighted on the isophote direction
  //  * add a variance regularization to better avoid edges.
  // The sharpness setting mimics the contrast equalizer effect by simply multiplying the HF by some gain.

  float *const restrict out = DT_IS_ALIGNED(output);
  const float *const restrict LF = DT_IS_ALIGNED(low_freq);
  const float *const restrict HF = DT_IS_ALIGNED(high_freq);

#ifdef _OPENMP
#pragma omp parallel for default(none)                                                                            \
    dt_omp_firstprivate(out, clipping_mask, HF, LF, height, width, mult, scale, first_order_factor) \
    schedule(static)
#endif
  for(size_t row = 0; row < height; ++row)
  {
    // interleave the order in which we process the rows so that we minimize cache misses
    const size_t i = dwt_interleave_rows(row, height, mult);
    // compute the 'above' and 'below' coordinates, clamping them to the image, once for the entire row
    const size_t i_neighbours[3]
      = { MAX((int)(i - mult), (int)0) * width,            // x - mult
          i * width,                                       // x
          MIN((int)(i + mult), (int)height - 1) * width }; // x + mult
    for(size_t j = 0; j < width; ++j)
    {
      const size_t idx = (i * width + j);
      const size_t index = idx * 4;

      // fetch the clipping mask opacity : opaque (alpha = 100 %) where clipped
      const dt_aligned_pixel_t alpha = { clipping_mask[index + RED],
                                         clipping_mask[index + GREEN],
                                         clipping_mask[index + BLUE],
                                         clipping_mask[index + ALPHA] };

      dt_aligned_pixel_t high_frequency = { HF[index + 0], HF[index + 1], HF[index + 2], HF[index + 3] };

      if(alpha[ALPHA] > 0.f)  // reconstruct
      {
        // non-local neighbours coordinates
        const size_t j_neighbours[3]
          = { MAX((int)(j - mult), (int)0),           // y - mult
              j,                                      // y
              MIN((int)(j + mult), (int)width - 1) }; // y + mult

        // fetch non-local pixels and store them locally and contiguously
        dt_aligned_pixel_t neighbour_pixel_HF[9];
        for_four_channels(c, aligned(neighbour_pixel_HF, HF: 16))
        {
          neighbour_pixel_HF[3 * 0 + 0][c] = HF[4 * (i_neighbours[0] + j_neighbours[0]) + c];
          neighbour_pixel_HF[3 * 0 + 1][c] = HF[4 * (i_neighbours[0] + j_neighbours[1]) + c];
          neighbour_pixel_HF[3 * 0 + 2][c] = HF[4 * (i_neighbours[0] + j_neighbours[2]) + c];

          neighbour_pixel_HF[3 * 1 + 0][c] = HF[4 * (i_neighbours[1] + j_neighbours[0]) + c];
          neighbour_pixel_HF[3 * 1 + 1][c] = HF[4 * (i_neighbours[1] + j_neighbours[1]) + c];
          neighbour_pixel_HF[3 * 1 + 2][c] = HF[4 * (i_neighbours[1] + j_neighbours[2]) + c];

          neighbour_pixel_HF[3 * 2 + 0][c] = HF[4 * (i_neighbours[2] + j_neighbours[0]) + c];
          neighbour_pixel_HF[3 * 2 + 1][c] = HF[4 * (i_neighbours[2] + j_neighbours[1]) + c];
          neighbour_pixel_HF[3 * 2 + 2][c] = HF[4 * (i_neighbours[2] + j_neighbours[2]) + c];
        }

        // Compute the laplacian in the direction parallel to the steepest gradient on the norm
        float DT_ALIGNED_ARRAY anisotropic_kernel_isophote[9] = { 0.25f, 0.5f, 0.25f, 0.5f, -3.f, 0.5f, 0.25f, 0.5f, 0.25f };

        // Convolve the filter to get the laplacian
        dt_aligned_pixel_t laplacian_HF = { 0.f, 0.f, 0.f, 0.f };
        for(int k = 0; k < 9; k++)
        {
          for_each_channel(c, aligned(laplacian_HF, neighbour_pixel_HF:16) aligned(anisotropic_kernel_isophote: 64))
            laplacian_HF[c] += neighbour_pixel_HF[k][c] * anisotropic_kernel_isophote[k];
        }

        // Diffuse
        const dt_aligned_pixel_t multipliers_HF = { 1.f / B_SPLINE_TO_LAPLACIAN, 1.f / B_SPLINE_TO_LAPLACIAN, 1.f / B_SPLINE_TO_LAPLACIAN, 0.f };
        for_each_channel(c, aligned(high_frequency, multipliers_HF, laplacian_HF, alpha))
          high_frequency[c] += alpha[c] * multipliers_HF[c] * (laplacian_HF[c] - first_order_factor * high_frequency[c]);
      }

      if((scale & FIRST_SCALE))
      {
        // out is not inited yet
        for_each_channel(c, aligned(out, high_frequency : 64))
          out[index + c] = high_frequency[c];
      }
      else
      {
        // just accumulate HF
        for_each_channel(c, aligned(out, high_frequency : 64))
          out[index + c] += high_frequency[c];
      }

      if((scale & LAST_SCALE))
      {
        // add the residual and clamp
        for_each_channel(c, aligned(out, LF, high_frequency : 64))
          out[index + c] = fmaxf(out[index + c] + LF[index + c], 0.f);

        // renormalize ratios
        if(alpha[ALPHA] > 0.f)
        {
          const float norm = sqrtf(sqf(out[index + RED]) + sqf(out[index + GREEN]) + sqf(out[index + BLUE]));
          for_each_channel(c, aligned(out, LF, high_frequency : 64))
            out[index + c] /= (c != ALPHA && norm > 1e-4f) ? norm : 1.f;
        }

        // Last scale : reconstruct RGB from ratios and norm - norm stays in the 4th channel
        // we need it to evaluate the gradient
        for_four_channels(c, aligned(out))
          out[index + c] = (c == ALPHA) ? out[index + ALPHA] : out[index + c] * out[index + ALPHA];
      }
    }
  }
}

static inline gint wavelets_process(const float *const restrict in, float
                                    *const restrict reconstructed,
                                    const float *const restrict clipping_mask,
                                    const size_t width, const size_t height,
                                    const int scales,
                                    float *const restrict HF,
                                    float *const restrict LF_odd,
                                    float *const restrict LF_even,
                                    const diffuse_reconstruct_variant_t variant,
                                    const float noise_level,
                                    const int salt, const float first_order_factor)
{
  gint success = TRUE;

  // À trous decimated wavelet decompose
  // there is a paper from a guy we know that explains it : https://jo.dreggn.org/home/2010_atrous.pdf
  // the wavelets decomposition here is the same as the equalizer/atrous module,

  // allocate a one-row temporary buffer for the decomposition
  size_t padded_size;
  float *const DT_ALIGNED_ARRAY tempbuf = dt_alloc_perthread_float(4 * width, &padded_size); //TODO: alloc in caller
  for(int s = 0; s < scales; ++s)
  {
    //fprintf(stderr, "CPU Wavelet decompose : scale %i\n", s);
    const int mult = 1 << s;

    const float *restrict buffer_in;
    float *restrict buffer_out;

    if(s == 0)
    {
      buffer_in = in;
      buffer_out = LF_odd;
    }
    else if(s % 2 != 0)
    {
      buffer_in = LF_odd;
      buffer_out = LF_even;
    }
    else
    {
      buffer_in = LF_even;
      buffer_out = LF_odd;
    }

    decompose_2D_Bspline(buffer_in, HF, buffer_out, width, height, mult, tempbuf, padded_size);

    uint8_t current_scale_type = scale_type(s, scales);
    const float radius = sqf(equivalent_sigma_at_step(B_SPLINE_SIGMA, s));

    if(variant == DIFFUSE_RECONSTRUCT_RGB)
      guide_laplacians(HF, buffer_out, clipping_mask, reconstructed, width, height, mult, noise_level, salt, current_scale_type, radius);
    else
      heat_PDE_diffusion(HF, buffer_out, clipping_mask, reconstructed, width, height, mult, current_scale_type, first_order_factor);

#if DEBUG_DUMP_PFM
    char name[64];
    sprintf(name, "/tmp/scale-input-%i.pfm", s);
    dump_PFM(name, buffer_in, width, height);

    sprintf(name, "/tmp/scale-blur-%i.pfm", s);
    dump_PFM(name, buffer_out, width, height);
#endif
  }
  dt_free_align(tempbuf);

  return success;
}


static void process_laplacian_bayer(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece,
                                    const void *const restrict ivoid, void *const restrict ovoid,
                                    const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out,
                                    const dt_aligned_pixel_t clips)
{
  dt_iop_highlights_data_t *data = (dt_iop_highlights_data_t *)piece->data;

  const uint32_t filters = piece->pipe->dsc.filters;
  dt_aligned_pixel_t wb = { 1.f, 1.f, 1.f, 1.f };
  if(piece->pipe->dsc.temperature.coeffs[0] != 0.f)
  {
    wb[0] = piece->pipe->dsc.temperature.coeffs[0];
    wb[1] = piece->pipe->dsc.temperature.coeffs[1];
    wb[2] = piece->pipe->dsc.temperature.coeffs[2];
  }

  const size_t height = roi_in->height;
  const size_t width = roi_in->width;
  const size_t size = roi_in->width * roi_in->height;

  float *const restrict interpolated = dt_alloc_align_float(size * 4);  // [R, G, B, norm] for each pixel
  float *const restrict clipping_mask = dt_alloc_align_float(size * 4); // [R, G, B, norm] for each pixel

  float *const restrict temp = dt_alloc_align_float(size * 4);

  // temp buffer for blurs. We will need to cycle between them for memory efficiency
  float *const restrict LF_odd = dt_alloc_align_float(size * 4);
  float *const restrict LF_even = dt_alloc_align_float(size * 4);

  const float scale = fmaxf(piece->iscale / roi_in->scale, 1.f);
  const float final_radius = (float)((int)(1 << data->scales)) / scale;
  const int scales = CLAMP((int)round(log2f(final_radius)), 0, MAX_NUM_SCALES);

  const float noise_level = data->noise_level / scale;

  // wavelets scales buffers
  float *restrict HF = dt_alloc_align_float(width * height * 4);

  const float *const restrict input = (const float *const restrict)ivoid;
  float *const restrict output = (float *const restrict)ovoid;

  _interpolate_and_mask(input, interpolated, clipping_mask, clips, wb, filters, width, height);
  dt_box_mean(clipping_mask, height, width, 4, 2, 1);

  for(int i = 0; i < data->iterations; i++)
  {
    const int salt = (i == data->iterations - 1); // add noise on the last iteration only
    wavelets_process(interpolated, temp, clipping_mask, width, height, scales, HF, LF_odd,
                     LF_even, DIFFUSE_RECONSTRUCT_RGB, noise_level, salt, data->solid_color);
    wavelets_process(temp, interpolated, clipping_mask, width, height, scales, HF, LF_odd,
                    LF_even, DIFFUSE_RECONSTRUCT_CHROMA, noise_level, salt, data->solid_color);
  }

  _remosaic_and_replace(interpolated, output, wb, filters, width, height);

#if DEBUG_DUMP_PFM
  dump_PFM("/tmp/interpolated.pfm", interpolated, width, height);
  dump_PFM("/tmp/clipping_mask.pfm", clipping_mask, width, height);
#endif

  dt_free_align(interpolated);
  dt_free_align(clipping_mask);
  dt_free_align(temp);
  dt_free_align(LF_even);
  dt_free_align(LF_odd);
  dt_free_align(HF);
}

#ifdef HAVE_OPENCL
static inline cl_int wavelets_process_cl(const int devid,
                                         cl_mem in, cl_mem reconstructed,
                                         cl_mem clipping_mask,
                                         const size_t sizes[3], const int width, const int height,
                                         dt_iop_highlights_global_data_t *const gd,
                                         const int scales,
                                         cl_mem HF,
                                         cl_mem LF_odd,
                                         cl_mem LF_even,
                                         const diffuse_reconstruct_variant_t variant,
                                         const float noise_level,
                                         const int salt, const float solid_color)
{
  cl_int err = DT_OPENCL_DEFAULT_ERROR;

  // À trous wavelet decompose
  // there is a paper from a guy we know that explains it : https://jo.dreggn.org/home/2010_atrous.pdf
  // the wavelets decomposition here is the same as the equalizer/atrous module,
  for(int s = 0; s < scales; ++s)
  {
    //fprintf(stderr, "GPU Wavelet decompose : scale %i\n", s);
    const int mult = 1 << s;

    cl_mem buffer_in;
    cl_mem buffer_out;

    if(s == 0)
    {
      buffer_in = in;
      buffer_out = LF_odd;
    }
    else if(s % 2 != 0)
    {
      buffer_in = LF_odd;
      buffer_out = LF_even;
    }
    else
    {
      buffer_in = LF_even;
      buffer_out = LF_odd;
    }

    // Compute wavelets low-frequency scales
    dt_opencl_set_kernel_args(devid, gd->kernel_filmic_bspline_horizontal, 0, CLARG(buffer_in), CLARG(HF),
      CLARG(width), CLARG(height), CLARG(mult));
    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_filmic_bspline_horizontal, sizes);
    if(err != CL_SUCCESS) return err;

    dt_opencl_set_kernel_args(devid, gd->kernel_filmic_bspline_vertical, 0, CLARG(HF), CLARG(buffer_out),
      CLARG(width), CLARG(height), CLARG(mult));
    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_filmic_bspline_vertical, sizes);
    if(err != CL_SUCCESS) return err;

    // Compute wavelets high-frequency scales and backup the maximum of texture over the RGB channels
    // Note : HF = detail - LF
    dt_opencl_set_kernel_args(devid, gd->kernel_filmic_wavelets_detail, 0, CLARG(buffer_in), CLARG(buffer_out),
      CLARG(HF), CLARG(width), CLARG(height));
    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_filmic_wavelets_detail, sizes);
    if(err != CL_SUCCESS) return err;

    uint8_t current_scale_type = scale_type(s, scales);
    const float radius = sqf(equivalent_sigma_at_step(B_SPLINE_SIGMA, s));

    // Compute wavelets low-frequency scales
    if(variant == DIFFUSE_RECONSTRUCT_RGB)
    {
      dt_opencl_set_kernel_args(devid, gd->kernel_highlights_guide_laplacians, 0, CLARG(HF), CLARG(buffer_out),
        CLARG(clipping_mask), CLARG(reconstructed), CLARG(reconstructed), CLARG(width), CLARG(height),
        CLARG(mult), CLARG(noise_level), CLARG(salt), CLARG(current_scale_type), CLARG(radius));
      err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_highlights_guide_laplacians, sizes);
      if(err != CL_SUCCESS) return err;
    }
    else // DIFFUSE_RECONSTRUCT_CHROMA
    {
      dt_opencl_set_kernel_args(devid, gd->kernel_highlights_diffuse_color, 0, CLARG(HF), CLARG(buffer_out),
        CLARG(clipping_mask), CLARG(reconstructed), CLARG(reconstructed), CLARG(width), CLARG(height),
        CLARG(mult), CLARG(current_scale_type), CLARG(solid_color));
      err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_highlights_diffuse_color, sizes);
      if(err != CL_SUCCESS) return err;
    }
  }

  return err;
}

static cl_int process_laplacian_bayer_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece,
                                         cl_mem dev_in, cl_mem dev_out,
                                         const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out,
                                         const dt_aligned_pixel_t clips)
{
  dt_iop_highlights_data_t *data = (dt_iop_highlights_data_t *)piece->data;
  dt_iop_highlights_global_data_t *gd = (dt_iop_highlights_global_data_t *)self->global_data;

  cl_int err = DT_OPENCL_DEFAULT_ERROR;

  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;

  size_t sizes[] = { ROUNDUPDWD(width, devid), ROUNDUPDHT(height, devid), 1 };

  const uint32_t filters = piece->pipe->dsc.filters;

  dt_aligned_pixel_t wb = { 1.f, 1.f, 1.f, 1.f };
  if(piece->pipe->dsc.temperature.coeffs[0] != 0.f)
  {
    wb[0] = piece->pipe->dsc.temperature.coeffs[0];
    wb[1] = piece->pipe->dsc.temperature.coeffs[1];
    wb[2] = piece->pipe->dsc.temperature.coeffs[2];
  }

  cl_mem interpolated = dt_opencl_alloc_device(devid, sizes[0], sizes[1], sizeof(float) * 4);  // [R, G, B, norm] for each pixel
  cl_mem clipping_mask = dt_opencl_alloc_device(devid, sizes[0], sizes[1], sizeof(float) * 4); // [R, G, B, norm] for each pixel

  cl_mem temp = dt_opencl_alloc_device(devid, sizes[0], sizes[1], sizeof(float) * 4);

  // temp buffer for blurs. We will need to cycle between them for memory efficiency
  cl_mem LF_odd = dt_opencl_alloc_device(devid, sizes[0], sizes[1], sizeof(float) * 4);
  cl_mem LF_even = dt_opencl_alloc_device(devid, sizes[0], sizes[1], sizeof(float) * 4);

  const float scale = fmaxf(piece->iscale / roi_in->scale, 1.f);
  const float final_radius = (float)((int)(1 << data->scales)) / scale;
  const int scales = CLAMP((int)round(log2f(final_radius)), 0, MAX_NUM_SCALES);

  //fprintf(stderr, "scales : %i\n", scales);

  const float noise_level = data->noise_level / scale;

  // wavelets scales buffers
  cl_mem HF = dt_opencl_alloc_device(devid, sizes[0], sizes[1], sizeof(float) * 4);

  cl_mem clips_cl = dt_opencl_copy_host_to_device_constant(devid, 4 * sizeof(float), (float*)clips);
  cl_mem wb_cl = dt_opencl_copy_host_to_device_constant(devid, 4 * sizeof(float), (float*)wb);

  dt_opencl_set_kernel_args(devid, gd->kernel_highlights_bilinear_and_mask, 0, CLARG(dev_in), CLARG(interpolated),
    CLARG(temp), CLARG(clips_cl), CLARG(wb_cl), CLARG(filters), CLARG(roi_out->width), CLARG(roi_out->height));
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_highlights_bilinear_and_mask, sizes);
  dt_opencl_release_mem_object(clips_cl);
  if(err != CL_SUCCESS) goto error;

  dt_opencl_set_kernel_args(devid, gd->kernel_highlights_box_blur, 0, CLARG(temp), CLARG(clipping_mask),
    CLARG(roi_out->width), CLARG(roi_out->height));
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_highlights_box_blur, sizes);
  if(err != CL_SUCCESS) goto error;

  for(int i = 0; i < data->iterations; i++)
  {
    const int salt = (i == data->iterations - 1); // add noise on the last iteration only
    err = wavelets_process_cl(devid, interpolated, temp, clipping_mask, sizes, width, height, gd, scales, HF,
                              LF_odd, LF_even, DIFFUSE_RECONSTRUCT_RGB, noise_level, salt, data->solid_color);
    if(err != CL_SUCCESS) goto error;

    err = wavelets_process_cl(devid, temp, interpolated, clipping_mask, sizes, width, height, gd, scales, HF,
                              LF_odd, LF_even, DIFFUSE_RECONSTRUCT_CHROMA, noise_level, salt, data->solid_color);
    if(err != CL_SUCCESS) goto error;
  }

  dt_opencl_set_kernel_args(devid, gd->kernel_highlights_remosaic_and_replace, 0, CLARG(interpolated),
    CLARG(dev_out), CLARG(wb_cl), CLARG(filters), CLARG(roi_out->width), CLARG(roi_out->height));
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_highlights_remosaic_and_replace, sizes);
  if(err != CL_SUCCESS) goto error;

  // cleanup and exit on success
  if(wb_cl) dt_opencl_release_mem_object(wb_cl);
  if(interpolated) dt_opencl_release_mem_object(interpolated);
  if(clipping_mask) dt_opencl_release_mem_object(clipping_mask);
  if(temp) dt_opencl_release_mem_object(temp);
  if(LF_even) dt_opencl_release_mem_object(LF_even);
  if(LF_odd) dt_opencl_release_mem_object(LF_odd);
  if(HF) dt_opencl_release_mem_object(HF);
  return err;

error:
  if(wb_cl) dt_opencl_release_mem_object(wb_cl);
  if(interpolated) dt_opencl_release_mem_object(interpolated);
  if(clipping_mask) dt_opencl_release_mem_object(clipping_mask);
  if(temp) dt_opencl_release_mem_object(temp);
  if(LF_even) dt_opencl_release_mem_object(LF_even);
  if(LF_odd) dt_opencl_release_mem_object(LF_odd);
  if(HF) dt_opencl_release_mem_object(HF);

  dt_print(DT_DEBUG_OPENCL, "[opencl_highlights] couldn't enqueue kernel! %s\n", cl_errstr(err));
  return err;
}
#endif

static void process_clip(dt_dev_pixelpipe_iop_t *piece, const void *const ivoid, void *const ovoid,
                         const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out,
                         const float clip)
{
  const float *const in = (const float *const)ivoid;
  float *const out = (float *const)ovoid;

  if(piece->pipe->dsc.filters)
  { // raw mosaic
#ifdef _OPENMP
#pragma omp parallel for SIMD() default(none) \
    dt_omp_firstprivate(clip, in, out, roi_out) \
    schedule(static)
#endif
    for(size_t k = 0; k < (size_t)roi_out->width * roi_out->height; k++)
    {
      out[k] = MIN(clip, in[k]);
    }
  }
  else
  {
    const int ch = piece->colors;

#ifdef _OPENMP
#pragma omp parallel for SIMD() default(none) \
    dt_omp_firstprivate(ch, clip, in, out, roi_out) \
    schedule(static)
#endif
    for(size_t k = 0; k < (size_t)ch * roi_out->width * roi_out->height; k++)
    {
      out[k] = MIN(clip, in[k]);
    }
  }
}

static void process_visualize(dt_dev_pixelpipe_iop_t *piece, const void *const ivoid, void *const ovoid,
                         const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out,
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
    dt_omp_firstprivate(in, out, clips) \
    dt_omp_sharedconst(npixels) \
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
  dt_omp_firstprivate(in, out, clips, roi_in) \
  dt_omp_sharedconst(filters, xtrans, is_xtrans) \
  schedule(static)
#endif
    for(size_t row = 0; row < roi_in->height; row++)
    {
      for(size_t col = 0, i = row * roi_in->width; col < roi_in->width; col++, i++)
      {
        const int c = is_xtrans ? FCxtrans(row, col, roi_in, xtrans) : FC(row, col, filters);
        const float ival = in[i];
        out[i] = (ival < clips[c]) ? 0.2f * ival : 1.0f;
      }
    }
  }
}

#include "iop/hlrecovery_v2.c"
#include "iop/opposed.c"

/* inpaint opposed and segmentation based algorithms want the whole image for proper calculation
   of chrominance correction and best candidates so we change both rois.
*/
void modify_roi_out(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, dt_iop_roi_t *roi_out,
                    const dt_iop_roi_t *const roi_in)
{
  *roi_out = *roi_in;
  // it can never hurt to make sure
  roi_out->x = MAX(0, roi_in->x);
  roi_out->y = MAX(0, roi_in->y);
}

void modify_roi_in(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const dt_iop_roi_t *const roi_out,
                   dt_iop_roi_t *roi_in)
{
  *roi_in = *roi_out;

  dt_iop_highlights_data_t *d = (dt_iop_highlights_data_t *)piece->data;
  dt_iop_highlights_gui_data_t *g = (dt_iop_highlights_gui_data_t *)self->gui_data;
  const gboolean fullpipe = piece->pipe->type & DT_DEV_PIXELPIPE_FULL;
  const gboolean visualizing = (g != NULL) ? g->show_visualize && fullpipe : FALSE;
  const gboolean use_opposing = (d->mode == DT_IOP_HIGHLIGHTS_OPPOSED) || (d->mode == DT_IOP_HIGHLIGHTS_SEGMENTS);
  
  if(visualizing || !use_opposing)
    return;

  roi_in->x = 0;
  roi_in->y = 0;
  roi_in->width = piece->buf_in.width;
  roi_in->height = piece->buf_in.height;
}

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const uint32_t filters = piece->pipe->dsc.filters;
  dt_iop_highlights_data_t *data = (dt_iop_highlights_data_t *)piece->data;
  dt_iop_highlights_gui_data_t *g = (dt_iop_highlights_gui_data_t *)self->gui_data;

  const gboolean fullpipe = piece->pipe->type & DT_DEV_PIXELPIPE_FULL;
  const gboolean visualizing = (g != NULL) ? g->show_visualize && fullpipe : FALSE;

  if(visualizing)
  {
    process_visualize(piece, ivoid, ovoid, roi_in, roi_out, data);
    piece->pipe->mask_display = DT_DEV_PIXELPIPE_DISPLAY_PASSTHRU;
    piece->pipe->type |= DT_DEV_PIXELPIPE_FAST;
    return;
  }

  /* Some of the HLR algorithms can be pretty slow, while rendering thumnbnails we looks for an acceptable
     lower quality like we do in demosaic and can tune the reconstruction code. So far only used by
     opposed and segmentations algos as they make use of the full image data instead of ROI 
  */
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
      _process_linear_opposed(self, piece, ivoid, ovoid, roi_in, roi_out, data, high_quality);
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
        dt_omp_firstprivate(clips, filters, ivoid, ovoid, roi_in, roi_out, \
                            xtrans) \
        schedule(static)
#endif
        for(int j = 0; j < roi_out->height; j++)
        {
          interpolate_color_xtrans(ivoid, ovoid, roi_in, roi_out, 0, 1, j, clips, xtrans, 0);
          interpolate_color_xtrans(ivoid, ovoid, roi_in, roi_out, 0, -1, j, clips, xtrans, 1);
        }
#ifdef _OPENMP
#pragma omp parallel for default(none) \
        dt_omp_firstprivate(clips, filters, ivoid, ovoid, roi_in, roi_out, \
                            xtrans) \
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
      const dt_segments_mask_t vmode = ((g != NULL) && fullpipe) ? g->segmentation_mask_mode : DT_SEGMENTS_MASK_OFF;

      float *tmp = _process_opposed(self, piece, ivoid, ovoid, roi_in, roi_out, data, TRUE, TRUE);
      if(tmp)
        _process_segmentation(piece, ivoid, ovoid, roi_in, roi_out, data, vmode, tmp);
      dt_free_align(tmp);

      if(vmode != DT_SEGMENTS_MASK_OFF)
      {
        piece->pipe->mask_display = DT_DEV_PIXELPIPE_DISPLAY_PASSTHRU;
        piece->pipe->type |= DT_DEV_PIXELPIPE_FAST;
      }
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
      float *tmp = _process_opposed(self, piece, ivoid, ovoid, roi_in, roi_out, data, FALSE, high_quality);
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

  if(piece->pipe->mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK) dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_highlights_params_t *p = (dt_iop_highlights_params_t *)p1;
  dt_iop_highlights_data_t *d = (dt_iop_highlights_data_t *)piece->data;

  memcpy(d, p, sizeof(*p));

  // no OpenCL for DT_IOP_HIGHLIGHTS_INPAINT or DT_IOP_HIGHLIGHTS_SEGMENTS and DT_IOP_HIGHLIGHTS_OPPOSED
  piece->process_cl_ready = ((d->mode == DT_IOP_HIGHLIGHTS_INPAINT) || (d->mode == DT_IOP_HIGHLIGHTS_SEGMENTS) || (d->mode == DT_IOP_HIGHLIGHTS_OPPOSED)) ? 0 : 1;
  if(d->mode == DT_IOP_HIGHLIGHTS_SEGMENTS) piece->process_tiling_ready = 0;

  dt_iop_highlights_gui_data_t *g = (dt_iop_highlights_gui_data_t *)self->gui_data;
  if(g)
  {
    const gboolean linear = piece->pipe->dsc.filters == 0;
    const gboolean fullpipe = piece->pipe->type & DT_DEV_PIXELPIPE_FULL;
    if(g->show_visualize && linear && fullpipe)
      piece->process_cl_ready = FALSE;
  }
  // check for heavy computing here to give an iop cache hint
  const gboolean heavy = (((d->mode == DT_IOP_HIGHLIGHTS_LAPLACIAN) && ((d->iterations * 1<<(2+d->scales)) >= 256))
                          || (d->mode == DT_IOP_HIGHLIGHTS_SEGMENTS)
                          || (d->mode == DT_IOP_HIGHLIGHTS_OPPOSED));
  self->cache_next_important = heavy;
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
  gd->kernel_highlights_false_color = dt_opencl_create_kernel(program, "highlights_false_color");

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
  dt_opencl_free_kernel(gd->kernel_highlights_false_color);

  dt_opencl_free_kernel(gd->kernel_filmic_bspline_vertical);
  dt_opencl_free_kernel(gd->kernel_filmic_bspline_horizontal);
  dt_opencl_free_kernel(gd->kernel_filmic_wavelets_detail);
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
  if((use_segmentation && (p->recovery == DT_RECOVERY_MODE_OFF)) && (g->segmentation_mask_mode == DT_SEGMENTS_MASK_STRENGTH))
  {
    dt_bauhaus_widget_set_quad_active(g->strength, FALSE);
    g->segmentation_mask_mode = DT_SEGMENTS_MASK_OFF;
  }

  if(w == g->clip)
    g->valid_chroma_correction = FALSE;
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_highlights_gui_data_t *g = (dt_iop_highlights_gui_data_t *)self->gui_data;
  const gboolean monochrome = dt_image_is_monochrome(&self->dev->image_storage);
  // enable this per default if raw or sraw if not real monochrome
  self->default_enabled = dt_image_is_rawprepare_supported(&self->dev->image_storage) && !monochrome;
  self->hide_enable_button = monochrome;
  gtk_stack_set_visible_child_name(GTK_STACK(self->widget), self->default_enabled ? "default" : "monochrome");
  dt_bauhaus_widget_set_quad_active(g->clip, FALSE);
  g->show_visualize = FALSE;
  dt_bauhaus_widget_set_quad_active(g->candidating, FALSE);
  dt_bauhaus_widget_set_quad_active(g->combine, FALSE);
  dt_bauhaus_widget_set_quad_active(g->strength, FALSE);
  g->segmentation_mask_mode = DT_SEGMENTS_MASK_OFF;

  const int menu_size = dt_bauhaus_combobox_length(g->mode);
  const uint32_t filters = self->dev->image_storage.buf_dsc.filters;
  const gboolean bayer = (filters != 0) && (filters != 9u);

  const gboolean basic = ((filters == 9u && menu_size == 4) || (bayer && menu_size == 5));
  dt_iop_highlights_params_t *p = (dt_iop_highlights_params_t *)self->params;
  if(p->mode == DT_IOP_HIGHLIGHTS_INPAINT && basic)
     dt_bauhaus_combobox_add_full(g->mode, _("reconstruct color"), DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT,
                                      GINT_TO_POINTER(DT_IOP_HIGHLIGHTS_INPAINT), NULL, TRUE);
  g->valid_chroma_correction = FALSE;

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
  if(!self->dev || self->dev->image_storage.id == -1) return;

  const gboolean monochrome = dt_image_is_monochrome(&self->dev->image_storage);
  // enable this per default if raw or sraw if not true monochrome
  self->default_enabled = dt_image_is_rawprepare_supported(&self->dev->image_storage) && !monochrome;
  self->hide_enable_button = monochrome;

  dt_iop_highlights_params_t *d = (dt_iop_highlights_params_t *)self->default_params;

  if(!dt_image_altered(self->dev->image_storage.id))
    d->mode = DT_IOP_HIGHLIGHTS_OPPOSED;

  if(self->widget)
    gtk_stack_set_visible_child_name(GTK_STACK(self->widget), self->default_enabled ? "default" : "monochrome");

  dt_iop_highlights_gui_data_t *g = (dt_iop_highlights_gui_data_t *)self->gui_data;
  if(g)
  {
    // rebuild the complete menu depending on sensor type and possibly active but obsolete mode
    const uint32_t filters = self->dev->image_storage.buf_dsc.filters;
    const int menu_size = dt_bauhaus_combobox_length(g->mode);
    for(int i = 0; i < menu_size; i++)
      dt_bauhaus_combobox_remove_at(g->mode, 0);

    dt_bauhaus_combobox_add_full(g->mode, _("inpaint opposed"), DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT,
                                      GINT_TO_POINTER(DT_IOP_HIGHLIGHTS_OPPOSED), NULL, TRUE);

    if(filters == 0)
      dt_bauhaus_combobox_add_full(g->mode, _("clip highlights"), DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT,
                                      GINT_TO_POINTER(DT_IOP_HIGHLIGHTS_CLIP), NULL, TRUE);
    else
    {
      dt_bauhaus_combobox_add_full(g->mode, _("reconstruct in LCh"), DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT,
                                      GINT_TO_POINTER(DT_IOP_HIGHLIGHTS_LCH), NULL, TRUE);
      dt_bauhaus_combobox_add_full(g->mode, _("clip highlights"), DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT,
                                      GINT_TO_POINTER(DT_IOP_HIGHLIGHTS_CLIP), NULL, TRUE);
      dt_bauhaus_combobox_add_full(g->mode, _("segmentation based"), DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT,
                                      GINT_TO_POINTER(DT_IOP_HIGHLIGHTS_SEGMENTS), NULL, TRUE);
      if((filters != 0) && (filters != 9u))
        dt_bauhaus_combobox_add_full(g->mode, _("guided laplacians"), DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT,
                                      GINT_TO_POINTER(DT_IOP_HIGHLIGHTS_LAPLACIAN), NULL, TRUE);

      dt_iop_highlights_params_t *p = (dt_iop_highlights_params_t *)self->params;
      if(p->mode == DT_IOP_HIGHLIGHTS_INPAINT)
        dt_bauhaus_combobox_add_full(g->mode, _("reconstruct color"), DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT,
                                      GINT_TO_POINTER(DT_IOP_HIGHLIGHTS_INPAINT), NULL, TRUE);
    }
    g->valid_chroma_correction = FALSE;
  }
}

static void _visualize_callback(GtkWidget *quad, gpointer user_data)
{
  if(darktable.gui->reset) return;
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_highlights_gui_data_t *g = (dt_iop_highlights_gui_data_t *)self->gui_data;
  g->show_visualize = dt_bauhaus_widget_get_quad_active(quad);
  dt_bauhaus_widget_set_quad_active(g->candidating, FALSE);
  dt_bauhaus_widget_set_quad_active(g->combine, FALSE);
  dt_bauhaus_widget_set_quad_active(g->strength, FALSE);
  g->segmentation_mask_mode = DT_SEGMENTS_MASK_OFF;
  dt_dev_reprocess_center(self->dev);
}

static void _candidating_callback(GtkWidget *quad, gpointer user_data)
{
  if(darktable.gui->reset) return;
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_highlights_gui_data_t *g = (dt_iop_highlights_gui_data_t *)self->gui_data;
  g->segmentation_mask_mode = (dt_bauhaus_widget_get_quad_active(quad)) ? DT_SEGMENTS_MASK_CANDIDATING : DT_SEGMENTS_MASK_OFF;
  dt_bauhaus_widget_set_quad_active(g->clip, FALSE);
  dt_bauhaus_widget_set_quad_active(g->combine, FALSE);
  dt_bauhaus_widget_set_quad_active(g->strength, FALSE);
  g->show_visualize = FALSE;
  dt_dev_reprocess_center(self->dev);
}

static void _combine_callback(GtkWidget *quad, gpointer user_data)
{
  if(darktable.gui->reset) return;
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_highlights_gui_data_t *g = (dt_iop_highlights_gui_data_t *)self->gui_data;
  g->segmentation_mask_mode = (dt_bauhaus_widget_get_quad_active(quad)) ? DT_SEGMENTS_MASK_COMBINE : DT_SEGMENTS_MASK_OFF;
  dt_bauhaus_widget_set_quad_active(g->clip, FALSE);
  dt_bauhaus_widget_set_quad_active(g->candidating, FALSE);
  dt_bauhaus_widget_set_quad_active(g->strength, FALSE);
  g->show_visualize = FALSE;
  dt_dev_reprocess_center(self->dev);
}

static void _strength_callback(GtkWidget *quad, gpointer user_data)
{
  if(darktable.gui->reset) return;
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_highlights_gui_data_t *g = (dt_iop_highlights_gui_data_t *)self->gui_data;
  g->segmentation_mask_mode = (dt_bauhaus_widget_get_quad_active(quad)) ? DT_SEGMENTS_MASK_STRENGTH : DT_SEGMENTS_MASK_OFF;
  dt_bauhaus_widget_set_quad_active(g->clip, FALSE);
  dt_bauhaus_widget_set_quad_active(g->combine, FALSE);
  dt_bauhaus_widget_set_quad_active(g->candidating, FALSE);
  g->show_visualize = FALSE;
  dt_dev_reprocess_center(self->dev);
}

void gui_focus(struct dt_iop_module_t *self, gboolean in)
{
  dt_iop_highlights_gui_data_t *g = (dt_iop_highlights_gui_data_t *)self->gui_data;
  if(!in)
  {
    const gboolean was_visualize = g->show_visualize || g->segmentation_mask_mode;
    dt_bauhaus_widget_set_quad_active(g->clip, FALSE);
    dt_bauhaus_widget_set_quad_active(g->candidating, FALSE);
    dt_bauhaus_widget_set_quad_active(g->combine, FALSE);
    dt_bauhaus_widget_set_quad_active(g->strength, FALSE);
    g->show_visualize = FALSE;
    g->segmentation_mask_mode = DT_SEGMENTS_MASK_OFF;
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

  GtkWidget *monochromes = dt_ui_label_new(_("not applicable"));
  gtk_widget_set_tooltip_text(monochromes, _("no highlights reconstruction for monochrome images"));

  // start building top level widget
  self->widget = gtk_stack_new();
  gtk_stack_set_homogeneous(GTK_STACK(self->widget), FALSE);
  gtk_stack_add_named(GTK_STACK(self->widget), monochromes, "monochrome");
  gtk_stack_add_named(GTK_STACK(self->widget), box_raw, "default");
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
