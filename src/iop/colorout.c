/*
    This file is part of darktable,
    Copyright (C) 2009-2026 darktable developers.

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
#include "common/colorspaces.h"
#include "common/hdr-transfer.h"
#include "common/colorspaces_inline_conversions.h"
#include "common/dttypes.h"
#include "common/imagebuf.h"
#include "common/iop_profile.h"
#include "common/opencl.h"
#include "common/utility.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"

#include <gdk/gdkkeysyms.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

// max iccprofile file name length
// must be in synch with dt_colorspaces_color_profile_t
#define DT_IOP_COLOR_ICC_LEN 512
#define LUT_SAMPLES 0x10000
// grid of the LittleCMS transform baked for the GPU, see _bake_clut()
#define CLUT_LEVEL 65

DT_MODULE_INTROSPECTION(5, dt_iop_colorout_params_t)

typedef struct dt_iop_colorout_data_t
{
  dt_colorspaces_color_profile_type_t type;
  dt_colorspaces_color_mode_t mode;
  int hdr_transfer;
  float lut[3][LUT_SAMPLES];
  dt_colormatrix_t cmatrix;
  cmsHTRANSFORM *xform;
  float *clut;       // xform baked into a CLUT_LEVEL^3 LUT for the GPU, built on first use
  float unbounded_coeffs[3][3]; // for extrapolation of shaper curves
  // a soft proof renders as an export to the proof profile and clips where
  // an integer export clips (clip). the gamut check marks the colors that
  // clip changes: from their linear values, within gamut_tolerance of
  // their magnitude and gamut_black, on the matrix path and for PQ and HLG,
  // where gamut_scale takes the linear value at which each channel's
  // encoding reaches 1 to 1; elsewhere by littlecms' gamut check
  // (gamut_xform)
  gboolean clip;
  float gamut_scale[4];
  float gamut_tolerance;
  float gamut_black;
  // what the gamut check paints: device cyan of the window, rendered as
  // the pixels are (see _gamut_mark())
  float gamut_mark[4];
  cmsContext gamut_context;
  cmsHTRANSFORM gamut_xform;
  // an ink proof, which no frame holds: Lab to the proof's values
  // (proof_xform), which xform takes on to the window's output
  int proof_channels;
  cmsHTRANSFORM proof_xform;
} dt_iop_colorout_data_t;

typedef struct dt_iop_colorout_global_data_t
{
  int kernel_colorout;
  int kernel_colorout_clut;
} dt_iop_colorout_global_data_t;

typedef struct dt_iop_colorout_params_t
{
  dt_colorspaces_color_profile_type_t type; // $DEFAULT: DT_COLORSPACE_SRGB
  char filename[DT_IOP_COLOR_ICC_LEN];
  dt_iop_color_intent_t intent; // $DEFAULT: DT_INTENT_PERCEPTUAL
} dt_iop_colorout_params_t;

typedef struct dt_iop_colorout_gui_data_t
{
  GtkWidget *output_intent, *output_profile;
} dt_iop_colorout_gui_data_t;


const char *name()
{
  return _("output color profile");
}


const char **description(dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("convert pipeline reference RGB to any display RGB\n"
                                        "using color profiles to remap RGB values"),
                                      _("mandatory"),
                                      _("linear or non-linear, Lab, display-referred"),
                                      _("defined by profile"),
                                      _("non-linear, RGB or Lab, display-referred"));
}


int default_group()
{
  return IOP_GROUP_COLOR | IOP_GROUP_TECHNICAL;
}

int flags()
{
  return IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_ONE_INSTANCE | IOP_FLAGS_WRITE_PIPECACHE_IN;
}

dt_iop_colorspace_type_t default_colorspace(dt_iop_module_t *self,
                                            dt_dev_pixelpipe_t *pipe,
                                            dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_LAB;
}

dt_iop_colorspace_type_t input_colorspace(dt_iop_module_t *self,
                                          dt_dev_pixelpipe_t *pipe,
                                          dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_LAB;
}

dt_iop_colorspace_type_t output_colorspace(dt_iop_module_t *self,
                                           dt_dev_pixelpipe_t *pipe,
                                           dt_dev_pixelpipe_iop_t *piece)
{
  int cst = IOP_CS_RGB;
  if(piece)
  {
    const dt_iop_colorout_data_t *const d = piece->data;
    if(d->type == DT_COLORSPACE_LAB) cst = IOP_CS_LAB;
  }
  else
  {
    dt_iop_colorout_params_t *p = self->params;
    if(p->type == DT_COLORSPACE_LAB) cst = IOP_CS_LAB;
  }
  return cst;
}

int legacy_params(dt_iop_module_t *self,
                  const void *const old_params,
                  const int old_version,
                  void **new_params,
                  int32_t *new_params_size,
                  int *new_version)
{
  typedef struct dt_iop_colorout_params_v5_t
  {
    dt_colorspaces_color_profile_type_t type; // $DEFAULT: DT_COLORSPACE_SRGB
    char filename[DT_IOP_COLOR_ICC_LEN];
    dt_iop_color_intent_t intent; // $DEFAULT: DT_INTENT_PERCEPTUAL
  } dt_iop_colorout_params_v5_t;

#define DT_IOP_COLOR_ICC_LEN_V4 100
  /*  if(old_version == 1 && new_version == 2)
  {
    dt_iop_colorout_params_t *o = (dt_iop_colorout_params_t *)old_params;
    dt_iop_colorout_params_t *n = (dt_iop_colorout_params_t *)new_params;
    memcpy(n,o,sizeof(dt_iop_colorout_params_t));
    n->seq = 0;
    return 0;
    }*/
  if(old_version == 2 || old_version == 3)
  {
    typedef struct dt_iop_colorout_params_v3_t
    {
      char iccprofile[DT_IOP_COLOR_ICC_LEN_V4];
      char displayprofile[DT_IOP_COLOR_ICC_LEN_V4];
      dt_iop_color_intent_t intent;
      dt_iop_color_intent_t displayintent;
      char softproof_enabled;
      char softproofprofile[DT_IOP_COLOR_ICC_LEN_V4];
      dt_iop_color_intent_t softproofintent;
    } dt_iop_colorout_params_v3_t;


    const dt_iop_colorout_params_v3_t *o = (dt_iop_colorout_params_v3_t *)old_params;
    dt_iop_colorout_params_v5_t *n = malloc(sizeof(dt_iop_colorout_params_v5_t));
    memset(n, 0, sizeof(dt_iop_colorout_params_v5_t));

    if(!strcmp(o->iccprofile, "sRGB"))
      n->type = DT_COLORSPACE_SRGB;
    else if(!strcmp(o->iccprofile, "linear_rec709_rgb")
            || !strcmp(o->iccprofile, "linear_rgb"))
      n->type = DT_COLORSPACE_LIN_REC709;
    else if(!strcmp(o->iccprofile, "linear_rec2020_rgb"))
      n->type = DT_COLORSPACE_LIN_REC2020;
    else if(!strcmp(o->iccprofile, "adobergb"))
      n->type = DT_COLORSPACE_ADOBERGB;
    else if(!strcmp(o->iccprofile, "X profile"))
      n->type = DT_COLORSPACE_DISPLAY;
    else
    {
      n->type = DT_COLORSPACE_FILE;
      dt_strlcpy_fixed_to_fixed(n->filename, sizeof(n->filename),
                                o->iccprofile, sizeof(o->iccprofile));
    }

    n->intent = o->intent;

    *new_params = n;
    *new_params_size = sizeof(dt_iop_colorout_params_v5_t);
    *new_version = 5;
    return 0;
  }
  if(old_version == 4)
  {
    typedef struct dt_iop_colorout_params_v4_t
    {
      dt_colorspaces_color_profile_type_t type;
      char filename[DT_IOP_COLOR_ICC_LEN_V4];
      dt_iop_color_intent_t intent;
    } dt_iop_colorout_params_v4_t;


    const dt_iop_colorout_params_v4_t *o = (dt_iop_colorout_params_v4_t *)old_params;
    dt_iop_colorout_params_v5_t *n = malloc(sizeof(dt_iop_colorout_params_v5_t));
    memset(n, 0, sizeof(dt_iop_colorout_params_v5_t));

    n->type = o->type;
    dt_strlcpy_fixed_to_fixed(n->filename, sizeof(n->filename),
                              o->filename, sizeof(o->filename));
    n->intent = o->intent;

    *new_params = n;
    *new_params_size = sizeof(dt_iop_colorout_params_v5_t);
    *new_version = 5;
    return 0;
  }

  return 1;
#undef DT_IOP_COLOR_ICC_LEN_V4
}

void init_global(dt_iop_module_so_t *self)
{
  const int program = 2; // basic.cl, from programs.conf
  dt_iop_colorout_global_data_t *gd = malloc(sizeof(dt_iop_colorout_global_data_t));
  self->data = gd;
  gd->kernel_colorout = dt_opencl_create_kernel(program, "colorout");
  gd->kernel_colorout_clut = dt_opencl_create_kernel(program, "colorout_clut");
}

void cleanup_global(dt_iop_module_so_t *self)
{
  dt_iop_colorout_global_data_t *gd = self->data;
  dt_opencl_free_kernel(gd->kernel_colorout);
  dt_opencl_free_kernel(gd->kernel_colorout_clut);
  free(self->data);
  self->data = NULL;
}

static void _intent_changed(GtkWidget *widget, dt_iop_module_t *self)
{
  DT_GUARD_GUI_UPDATE();
  dt_iop_colorout_params_t *p = self->params;
  p->intent = (dt_iop_color_intent_t)dt_bauhaus_combobox_get(widget);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void _output_profile_changed(GtkWidget *widget, dt_iop_module_t *self)
{
  DT_GUARD_GUI_UPDATE();
  dt_iop_colorout_params_t *p = self->params;
  int pos = dt_bauhaus_combobox_get(widget);

  for(GList *profiles = darktable.color_profiles->profiles; profiles; profiles = g_list_next(profiles))
  {
    dt_colorspaces_color_profile_t *pp = profiles->data;
    if(pp->out_pos == pos)
    {
      p->type = pp->type;
      dt_strlcpy_to_fixed(p->filename, pp->filename, sizeof(p->filename));
      dt_dev_add_history_item(darktable.develop, self, TRUE);

      DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_CONTROL_PROFILE_USER_CHANGED, DT_COLORSPACES_PROFILE_TYPE_EXPORT);
      return;
    }
  }

  dt_print(DT_DEBUG_ALWAYS,
           "[colorout] color profile %s seems to have disappeared!",
           dt_colorspaces_get_name(p->type, p->filename));
}

static float _lerp_u_lut(const float *const lut, const float v)
{
  // TODO: check if optimization is worthwhile!
  const float ft = CLAMPS(v * (LUT_SAMPLES - 1), 0, LUT_SAMPLES - 1);
  const int t = ft < LUT_SAMPLES - 2 ? ft : LUT_SAMPLES - 2;
  const float f = ft - t;
  const float l1 = lut[t];
  const float l2 = lut[t + 1];
  return l1 * (1.0f - f) + l2 * f;
}

// call only if sure that v<1.0
static float _lerp_lut(const float *const lut, const float v)
{
  const float z = MAX(v,0.0f);  // clip away negatives
  const float ft = z * (LUT_SAMPLES - 1);
  // because v<1.0, ft must be less than (LUT_SAMPLES-1), so truncating
  // will set t <= (LUT_SAMPLES-2) and thus we don't need to clamp it
  // to avoid an array overrun
  const int t = (int)ft;
  const float f = ft - t;
  const float l1 = lut[t];
  const float l2 = lut[t + 1];
  return l1 * (1.0f - f) + l2 * f;
}

// sample xform on a CLUT_LEVEL^3 grid over the Lab range LittleCMS encodes
// floats in (L 0..100, a and b -128..127), the first channel varying fastest
// as in the lut3d module.
static float *_bake_clut(cmsHTRANSFORM xform)
{
  const size_t level = CLUT_LEVEL;
  const size_t nodes = level * level * level;
  float *const lab = dt_alloc_align_float(4 * nodes);
  float *const rgb = dt_alloc_align_float(4 * nodes);
  float *const clut = dt_alloc_align_float(3 * nodes);
  if(!lab || !rgb || !clut)
  {
    dt_free_align(lab);
    dt_free_align(rgb);
    dt_free_align(clut);
    return NULL;
  }

  DT_OMP_FOR()
  for(size_t k = 0; k < nodes; k++)
  {
    lab[4 * k + 0] = 100.0f * (float)(k % level) / (float)(level - 1);
    lab[4 * k + 1] = 255.0f * (float)((k / level) % level) / (float)(level - 1) - 128.0f;
    lab[4 * k + 2] = 255.0f * (float)(k / (level * level)) / (float)(level - 1) - 128.0f;
    lab[4 * k + 3] = 0.0f;
  }

  // the same chunking as _transform_lcms()
  const size_t chunksize = dt_cacheline_chunks(nodes, dt_get_num_threads());
  DT_OMP_FOR()
  for(size_t chunkstart = 0; chunkstart < nodes; chunkstart += chunksize)
    cmsDoTransform(xform, lab + 4 * chunkstart, rgb + 4 * chunkstart,
                   MIN(chunkstart + chunksize, nodes) - chunkstart);

  DT_OMP_FOR()
  for(size_t k = 0; k < nodes; k++)
    for(int c = 0; c < 3; c++) clut[3 * k + c] = rgb[4 * k + c];

  dt_free_align(lab);
  dt_free_align(rgb);
  return clut;
}

#ifdef HAVE_OPENCL
// the LittleCMS path as a 3D LUT
static int _process_cl_clut(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in,
                            cl_mem dev_out, const int width, const int height)
{
  dt_iop_colorout_data_t *d = piece->data;
  const dt_iop_colorout_global_data_t *gd = self->global_data;
  const int devid = piece->pipe->devid;

  if(!d->clut) d->clut = _bake_clut(d->xform);
  if(!d->clut) return CL_MEM_OBJECT_ALLOCATION_FAILURE;

  const int level = CLUT_LEVEL;
  const size_t size = sizeof(float) * 3 * level * level * level;
  cl_int err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
  cl_mem dev_clut = dt_opencl_copy_host_to_device_constant(devid, size, d->clut);
  if(dev_clut)
    err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_colorout_clut, width, height,
      CLARG(dev_in), CLARG(dev_out), CLARG(width), CLARG(height), CLARG(dev_clut), CLARG(level),
      CLARG(d->hdr_transfer), CLARG(d->clip));

  dt_opencl_release_mem_object(dev_clut);
  return err;
}

int process_cl(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_colorout_data_t *d = piece->data;
  dt_iop_colorout_global_data_t *gd = self->global_data;
  cl_mem dev_m = NULL, dev_r = NULL, dev_g = NULL, dev_b = NULL, dev_coeffs = NULL;

  cl_int err = DT_OPENCL_DEFAULT_ERROR;
  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;

  if(d->type == DT_COLORSPACE_LAB)
  {
    const size_t region[2] = { roi_in->width, roi_in->height };
    err = dt_opencl_enqueue_copy_image(devid, dev_in, dev_out, CLIMG_ORIGIN, CLIMG_ORIGIN, region);
    if(err != CL_SUCCESS) goto error;
    return CL_SUCCESS;
  }

  if(!dt_is_valid_colormatrix(d->cmatrix[0][0]))
    return _process_cl_clut(self, piece, dev_in, dev_out, width, height);

  const int gamutcheck = d->mode == DT_PROFILE_GAMUTCHECK;

  float cmatrix[9];
  pack_3xSSE_to_3x3(d->cmatrix, cmatrix);
  dev_m = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * 9, cmatrix);
  if(dev_m == NULL) goto error;
  dev_r = dt_opencl_copy_host_to_image(devid, d->lut[0], 256, 256, sizeof(float));
  if(dev_r == NULL) goto error;
  dev_g = dt_opencl_copy_host_to_image(devid, d->lut[1], 256, 256, sizeof(float));
  if(dev_g == NULL) goto error;
  dev_b = dt_opencl_copy_host_to_image(devid, d->lut[2], 256, 256, sizeof(float));
  if(dev_b == NULL) goto error;
  dev_coeffs
      = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * 3 * 3, (float *)d->unbounded_coeffs);
  if(dev_coeffs == NULL) goto error;
  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_colorout, width, height,
    CLARG(dev_in), CLARG(dev_out), CLARG(width), CLARG(height), CLARG(dev_m), CLARG(dev_r), CLARG(dev_g),
    CLARG(dev_b), CLARG(dev_coeffs), CLARG(d->hdr_transfer), CLARG(d->clip), CLARG(gamutcheck),
    CLARRAY(4, d->gamut_scale), CLARG(d->gamut_tolerance), CLARG(d->gamut_black),
    CLARRAY(4, d->gamut_mark));

error:
  dt_opencl_release_mem_object(dev_m);
  dt_opencl_release_mem_object(dev_r);
  dt_opencl_release_mem_object(dev_g);
  dt_opencl_release_mem_object(dev_b);
  dt_opencl_release_mem_object(dev_coeffs);
  return err;
}
#endif

static void _process_fastpath_apply_tonecurves(const dt_iop_colorout_data_t *const d,
                                              float *const restrict out,
                                              const size_t npixels)
{
  if(dt_is_valid_colormatrix(d->cmatrix[0][0]))
  {
    // out is already converted to RGB from Lab.

    // do we have any lut to apply, or is this a linear profile?
    if((d->lut[0][0] >= 0.0f) && (d->lut[1][0] >= 0.0f) && (d->lut[2][0] >= 0.0f))
    { // apply profile
      DT_OMP_FOR()
      for(size_t k = 0; k < 4 * npixels; k += 4)
      {
        for(int c = 0; c < 3; c++)
        {
          out[k + c] = (out[k + c] < 1.0f) ? _lerp_lut(d->lut[c], out[k + c])
                                           : dt_iop_eval_exp(d->unbounded_coeffs[c], out[k + c]);
        }
      }
    }
    else if((d->lut[0][0] >= 0.0f) || (d->lut[1][0] >= 0.0f) || (d->lut[2][0] >= 0.0f))
    { // apply profile
      DT_OMP_FOR()
      for(size_t k = 0; k < 4 * npixels; k += 4)
      {
        for(int c = 0; c < 3; c++)
        {
          if(d->lut[c][0] >= 0.0f)
          {
            out[k + c] = (out[k + c] < 1.0f) ? _lerp_lut(d->lut[c], out[k + c])
                                             : dt_iop_eval_exp(d->unbounded_coeffs[c], out[k + c]);
          }
        }
      }
    }
  }
}

static void _transform_cmatrix_linear(const dt_iop_colorout_data_t *const d,
                               float *restrict out,
                               const float *restrict in,
                               const size_t npixels)
{
  dt_colormatrix_t cmatrix;
  transpose_3xSSE(d->cmatrix, cmatrix);
  dt_aligned_pixel_t cmatrix_0, cmatrix_1, cmatrix_2;
  copy_pixel(cmatrix_0,cmatrix[0]);
  copy_pixel(cmatrix_1,cmatrix[1]);
  copy_pixel(cmatrix_2,cmatrix[2]);
  DT_OMP_FOR()
  for(size_t k = 0; k < npixels; k++)
  {
    // oddly, calling dt_Lab_to_linearRGB instead of doing the
    // matrix multiplication here is about 10% slower even though it
    // generates virtually the same instructions and fewer memory
    // loads.
    dt_aligned_pixel_t XYZ;
    dt_Lab_to_XYZ(in + 4*k, XYZ);
    dt_aligned_pixel_t rgb;
    for_each_channel(r)
      rgb[r] = cmatrix_0[r] * XYZ[0] + cmatrix_1[r] * XYZ[1] + cmatrix_2[r] * XYZ[2];
    copy_pixel_nontemporal(out + 4*k, rgb);
  }
  dt_omploop_sfence();
}

static void _transform_cmatrix_tonecurve(const dt_iop_colorout_data_t *const d,
                                         float *restrict out,
                                         const float *restrict in,
                                         const size_t npixels)
{
  dt_colormatrix_t cmatrix;
  transpose_3xSSE(d->cmatrix, cmatrix);
  dt_aligned_pixel_t cmatrix_0, cmatrix_1, cmatrix_2;
  copy_pixel(cmatrix_0,cmatrix[0]);
  copy_pixel(cmatrix_1,cmatrix[1]);
  copy_pixel(cmatrix_2,cmatrix[2]);
  const float *const lut = &d->lut[0][0];
  const float *coeffs = &d->unbounded_coeffs[0][0];
  DT_OMP_FOR()
  for(size_t k = 0; k < npixels; k++)
  {
    dt_aligned_pixel_t rgb; // using an aligned temporary variable lets the compiler optimize away interm. writes
    dt_Lab_to_linearRGB(in + 4*k, cmatrix_0, cmatrix_1, cmatrix_2, rgb);
    if(lut[0] >= 0.0f)
    {
      rgb[0] = (rgb[0] < 1.0f) ? _lerp_lut(lut, rgb[0])
        : dt_iop_eval_exp(coeffs, rgb[0]);
    }
    if(lut[LUT_SAMPLES] >= 0.0f)
    {
      rgb[1] = (rgb[1] < 1.0f) ? _lerp_lut(lut+LUT_SAMPLES, rgb[1])
        : dt_iop_eval_exp(coeffs+3, rgb[1]);
    }
    if(lut[2*LUT_SAMPLES] >= 0.0f)
    {
      rgb[2] = (rgb[2] < 1.0f) ? _lerp_lut(lut+2*LUT_SAMPLES, rgb[2])
        : dt_iop_eval_exp(coeffs+6, rgb[2]);
    }
    copy_pixel_nontemporal(out + 4*k, rgb);
  }
  dt_omploop_sfence();
}

static int _transform_cmatrix(const dt_iop_colorout_data_t *const d,
                               float *restrict out,
                               const float *restrict in,
                               const size_t npixels)
{
  const gboolean is_linear = (d->lut[0][0] < 0.0f) || (d->lut[1][0] < 0.0f) || (d->lut[2][0] < 0.0f);
//  const gboolean all_nonlin = (d->lut[0][0] >= 0.0f) || (d->lut[1][0] >= 0.0f) || (d->lut[2][0] >= 0.0f);
  if(is_linear || 1) //TODO: integrate tonecurve in same pass as color matrix without major speed penalty
  {
    _transform_cmatrix_linear(d, out, in, npixels);
  }
  else
  {
    _transform_cmatrix_tonecurve(d, out, in, npixels);
  }
  return is_linear != 0; // not done if nonlinear, need to apply tonecurve
}

static void _transform_lcms(const dt_iop_colorout_data_t *const d,
                            float *restrict out,
                            const float *restrict in,
                            const size_t npixels)
{
  // figure out the number of pixels each thread needs to process,
  // rounded up to a multiple of the CPU's cache line size
  const size_t nthreads = dt_get_num_threads();
  const size_t chunksize = dt_cacheline_chunks(npixels, nthreads);
  DT_OMP_FOR()
  for(size_t chunkstart = 0; chunkstart < npixels; chunkstart += chunksize)
  {
    size_t count = MIN(chunkstart + chunksize, npixels) - chunkstart;
    cmsDoTransform(d->xform, in + 4*chunkstart, out + 4*chunkstart, count);
  }
}

// pixels per block of the littlecms gamut check, whose buffers are on the stack
#define PROOF_BLOCK 256

// a soft proof's export clips the linear value v: below 0, or above the
// range its encoding stores, by more than the tolerance. the range: up
// to 1 / gamut_scale for an SDR proof, light up to the peak for PQ and
// scene light up to 1 for HLG, what dt_hdr_encode() clamps
static inline gboolean _out_of_gamut(const float *const v,
                                     const dt_iop_colorout_data_t *const d)
{
  float scale[3] = { d->gamut_scale[0], d->gamut_scale[1], d->gamut_scale[2] };
  if(d->hdr_transfer == DT_HDR_PQ)
  {
    for(int c = 0; c < 3; c++) scale[c] = DT_HDR_REFERENCE_WHITE_NITS / DT_HDR_PQ_PEAK_NITS;
  }
  else if(d->hdr_transfer != DT_HDR_NONE)
  {
    const float positive[3] = { MAX(v[0], 0.0f), MAX(v[1], 0.0f), MAX(v[2], 0.0f) };
    const float hlg = dt_hdr_hlg_scene_scale(dt_hdr_hlg_luminance(positive, d->hdr_transfer));
    for(int c = 0; c < 3; c++) scale[c] = hlg;
  }
  const float tolerance = d->gamut_tolerance;
  const float below = -tolerance * MAX(MAX(MAX(v[0], v[1]), v[2]), 0.0f) - d->gamut_black;
  for(int c = 0; c < 3; c++)
    if(v[c] < below || v[c] * scale[c] > 1.0f + tolerance) return TRUE;
  return FALSE;
}

// marks the colors of a block of Lab pixels that are out of the proof's
// gamut, by littlecms' gamut check
static void _gamut_alarm(const dt_iop_colorout_data_t *const d,
                         const float *const in,
                         uint8_t *const marked,
                         const size_t count)
{
  // gamut_xform gives its alarm codes, zero, for a color out of the
  // proof's gamut: Lab 0, -128, -128, the corner of the encoding, which
  // an in-gamut color only comes back as through an identity Lab proof,
  // as the hidden Lab output is, for an input that encodes there
  cmsUInt16Number lab[3 * PROOF_BLOCK] = { 0 }, gamut[3 * PROOF_BLOCK];
  for(size_t j = 0; j < count; j++)
  {
    const float *const l = in + 4 * j;
    const cmsCIELab ref = { l[0], l[1], l[2] };
    cmsFloat2LabEncoded(lab + 3 * j, &ref);
  }
  cmsDoTransform(d->gamut_xform, lab, gamut, count);
  for(size_t j = 0; j < count; j++)
  {
    const cmsUInt16Number *const g = gamut + 3 * j;
    marked[j] = !(g[0] | g[1] | g[2]);
  }
}

// the gamut check's marks of npixels pixels, from the Lab input through
// gamut_xform or else from the linear values the transform wrote; NULL
// without memory, which leaves the proof unmarked
static uint8_t *_gamut_marks(const dt_iop_colorout_data_t *const d,
                             const float *const lab,
                             const float *const linear,
                             const size_t npixels)
{
  uint8_t *const marked = dt_alloc_aligned(npixels);
  if(!marked) return NULL;
  if(d->gamut_xform)
  {
    DT_OMP_FOR()
    for(size_t start = 0; start < npixels; start += PROOF_BLOCK)
      _gamut_alarm(d, lab + 4 * start, marked + start, MIN(start + PROOF_BLOCK, npixels) - start);
  }
  else
  {
    DT_OMP_FOR()
    for(size_t k = 0; k < npixels; k++)
      marked[k] = _out_of_gamut(linear + 4 * k, d);
  }
  return marked;
}

static void _paint_gamut_marks(const dt_iop_colorout_data_t *const d,
                               float *const out,
                               const uint8_t *const marked,
                               const size_t npixels)
{
  const float *const mark = d->gamut_mark;
  DT_OMP_FOR()
  for(size_t k = 0; k < npixels; k++)
    if(marked[k])
      for(int c = 0; c < 3; c++) out[4 * k + c] = mark[c];
}

// an ink proof shows a deliverable darktable does not export, and a frame
// holds RGB: Lab to the proof's values, clipped to what its integer
// encoding holds, and from there to the window's output. the proper place
// for it is a soft proof in the print module. marked, if not NULL, takes
// the gamut check's marks
static void _transform_ink_proof(const dt_iop_colorout_data_t *const d,
                                 float *restrict out,
                                 const float *restrict in,
                                 const size_t npixels,
                                 uint8_t *const marked)
{
  const int nch = d->proof_channels;
  DT_OMP_FOR()
  for(size_t start = 0; start < npixels; start += PROOF_BLOCK)
  {
    const size_t count = MIN(start + PROOF_BLOCK, npixels) - start;
    float DT_ALIGNED_ARRAY proof[cmsMAXCHANNELS * PROOF_BLOCK];

    cmsDoTransform(d->proof_xform, in + 4 * start, proof, count);
    if(marked) _gamut_alarm(d, in + 4 * start, marked + start, count);
    for(size_t j = 0; j < count * nch; j++) proof[j] = CLIP(proof[j]);
    cmsDoTransform(d->xform, proof, out + 4 * start, count);
  }
}

// what an export writes after its transform: the HDR transfer function,
// and the clip of an integer file. dt_hdr_encode() already clamps the
// signal
static void _encode(const dt_iop_colorout_data_t *const d,
                    float *const restrict out,
                    const size_t npixels)
{
  if(d->hdr_transfer)
  {
    DT_OMP_FOR()
    for(size_t k = 0; k < npixels; k++)
      dt_hdr_encode(out + 4*k, d->hdr_transfer);
  }
  if(d->clip)
  {
    DT_OMP_FOR()
    for(size_t k = 0; k < npixels; k++)
      for(int c = 0; c < 3; c++) out[4 * k + c] = CLIP(out[4 * k + c]);
  }
}

// Lab to the output's values, as an export writes them. marked, if not
// NULL, takes the gamut check's marks, NULL without memory or for a Lab
// output. FALSE without a transform
static gboolean _to_output(const dt_iop_colorout_data_t *const d,
                           float *const restrict out,
                           const float *const restrict in,
                           const size_t npixels,
                           uint8_t **const marked)
{
  if(marked) *marked = NULL;
  if(d->type == DT_COLORSPACE_LAB)
  {
    dt_iop_image_copy(out, in, 4 * npixels);
  }
  else if(dt_is_valid_colormatrix(d->cmatrix[0][0]))
  {
    const gboolean linear = _transform_cmatrix(d, out, in, npixels);
    if(marked) *marked = _gamut_marks(d, in, out, npixels);
    if(!linear) _process_fastpath_apply_tonecurves(d, out, npixels);
  }
  else if(d->proof_xform)
  {
    if(marked) *marked = dt_alloc_aligned(npixels);
    _transform_ink_proof(d, out, in, npixels, marked ? *marked : NULL);
  }
  else if(d->xform)
  {
    _transform_lcms(d, out, in, npixels);
    if(marked) *marked = _gamut_marks(d, in, out, npixels);
  }
  else
    return FALSE;
  _encode(d, out, npixels);
  return TRUE;
}

void process(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  if(!dt_iop_have_required_input_format(4 /*we need full-color pixels*/, self, piece->colors,
                                         ivoid, ovoid, roi_in, roi_out))
    return;
  const dt_iop_colorout_data_t *const d = piece->data;
  const size_t width = roi_out->width;
  const size_t height = roi_out->height;
  const size_t npixels = width * height;
  float *const restrict out = (float *)ovoid;
  const float *const restrict in = (const float *)ivoid;
  const gboolean gamutcheck = d->mode == DT_PROFILE_GAMUTCHECK;
  uint8_t *marked = NULL;

  if(!_to_output(d, out, in, npixels, gamutcheck ? &marked : NULL))
  {
    // an export without a transform (commit_params() leaves colorout out
    // of a presented pipe instead): stopping the pipe leaves it no output,
    // and the export fails
    dt_dev_pixelpipe_set_shutdown(piece->pipe, DT_DEV_PIXELPIPE_STOP_PIECE);
    return;
  }
  if(marked)
  {
    _paint_gamut_marks(d, out, marked, npixels);
    dt_free_align(marked);
  }
}

// littlecms takes Lab to an RGB matrix profile as the matrix path does,
// for intent and black point compensation (cmscnvrt.c, 2.16). every
// intent but absolute colorimetric reads the same curves and matrix
// (_cmsReadOutputLUT(), with no table for the matrix path to ignore), and
// the compensation, which littlecms forces for perceptual and saturation
// into a v4 profile, changes nothing where the profile's black point as
// littlecms detects it is the Lab profile's: 0, as an abstract profile's
static gboolean _matrix_like_lcms(const cmsHPROFILE profile,
                                  const dt_iop_color_intent_t intent,
                                  const gboolean bpc)
{
  if(intent == DT_INTENT_ABSOLUTE_COLORIMETRIC) return FALSE;
  const gboolean forced = cmsGetEncodedICCversion(profile) >= 0x4000000
    && (intent == DT_INTENT_PERCEPTUAL || intent == DT_INTENT_SATURATION);
  if(!bpc && !forced) return TRUE;
  cmsCIEXYZ black;
  cmsDetectDestinationBlackPoint(&black, profile, intent, 0);
  return black.X == 0.0 && black.Y == 0.0 && black.Z == 0.0;
}

// the matrix path an export takes, unless "always use LittleCMS 2" asks
// for littlecms; a soft proof takes it only where it reproduces littlecms
// for the proof's intent and black point compensation
static gboolean _matrix_path(dt_iop_colorout_data_t *d,
                             const cmsHPROFILE output,
                             const gboolean proof,
                             const dt_iop_color_intent_t intent,
                             const gboolean bpc,
                             const gboolean force_lcms2)
{
  if(force_lcms2 || (proof && !_matrix_like_lcms(output, intent, bpc))) return FALSE;
  return !dt_colorspaces_get_matrix_from_output_profile(output, d->cmatrix,
                                                        d->lut[0], d->lut[1], d->lut[2], LUT_SAMPLES);
}

// 1 over the linear value at which channel c of the matrix path's encoding
// exceeds 1: the curve's table up to 1, its extrapolation above
static float _encoding_scale(const dt_iop_colorout_data_t *const d, const int c)
{
  const float *const lut = d->lut[c];
  if(lut[0] < 0.0f) return 1.0f;
  if(lut[0] > 1.0f) return INFINITY;
  if(lut[LUT_SAMPLES - 1] > 1.0f)
  {
    int lo = 0, hi = LUT_SAMPLES - 1;
    while(hi - lo > 1)
    {
      const int mid = (lo + hi) / 2;
      if(lut[mid] > 1.0f)
        hi = mid;
      else
        lo = mid;
    }
    const float f = (1.0f - lut[lo]) / (lut[hi] - lut[lo]);
    return (LUT_SAMPLES - 1) / (lo + f);
  }
  // dt_iop_eval_exp(): a[1] * (x * a[0])^a[2]
  const float *const a = d->unbounded_coeffs[c];
  if(!(a[0] > 0.0f && a[1] > 0.0f && a[2] > 0.0f)) return 0.0f;
  return 1.0f / MAX(1.0f, powf(1.0f / a[1], 1.0f / a[2]) / a[0]);
}

static void _delete_transforms(dt_iop_colorout_data_t *d)
{
  if(d->xform) cmsDeleteTransform(d->xform);
  if(d->proof_xform) cmsDeleteTransform(d->proof_xform);
  if(d->gamut_xform) cmsDeleteTransform(d->gamut_xform);
  if(d->gamut_context) cmsDeleteContext(d->gamut_context);
  dt_free_align(d->clut);
  d->xform = NULL;
  d->proof_xform = NULL;
  d->gamut_xform = NULL;
  d->gamut_context = NULL;
  d->clut = NULL;
}

// Lab to output, with the intent and black point compensation of the
// export or the soft proof. an ink proof goes to its values first (see
// _transform_ink_proof()), and from there to output relative colorimetric.
// alarm adds littlecms' gamut check against the proof
static void _create_transforms(dt_iop_colorout_data_t *d,
                               const cmsHPROFILE Lab,
                               const cmsHPROFILE output,
                               const cmsUInt32Number output_format,
                               const cmsHPROFILE ink,
                               const dt_iop_color_intent_t intent,
                               const gboolean bpc,
                               const gboolean alarm)
{
  const uint32_t flags = bpc ? cmsFLAGS_BLACKPOINTCOMPENSATION : 0;
  if(ink)
  {
    // the ink's values in littlecms' own encoding (PT_ANY), where [0, 1]
    // is what an integer encoding holds
    const cmsUInt32Number proof_format =
      FLOAT_SH(1) | CHANNELS_SH(cmsChannelsOfColorSpace(cmsGetColorSpace(ink))) | BYTES_SH(4);
    d->proof_channels = T_CHANNELS(proof_format);
    d->proof_xform = cmsCreateTransform(Lab, TYPE_LabA_FLT, ink, proof_format, intent,
                                        cmsFLAGS_NOCACHE | flags);
    d->xform = cmsCreateTransform(ink, proof_format, output, output_format,
                                  INTENT_RELATIVE_COLORIMETRIC, cmsFLAGS_NOCACHE);
  }
  else
    d->xform = cmsCreateTransform(Lab, TYPE_LabA_FLT, output, output_format, intent, flags);

  // littlecms' own gamut check, from its proofing transform: it rates a
  // LUT profile, a printer's, by a relative colorimetric round trip, which
  // a round trip with the proof's intent does not reproduce under a
  // perceptual table. it samples that rating on a grid, the only measure
  // there. on 16 bits every littlecms version writes the context's alarm
  // codes in place of such a color, where a float transform writes -1 in
  // 2.16 and the codes in 2.19
  if(alarm)
  {
    static const cmsUInt16Number codes[cmsMAXCHANNELS] = { 0 };
    d->gamut_context = cmsCreateContext(NULL, NULL);
    if(d->gamut_context)
    {
      cmsSetAlarmCodesTHR(d->gamut_context, codes);
      d->gamut_xform = cmsCreateProofingTransformTHR
        (d->gamut_context, Lab, TYPE_Lab_16, Lab, TYPE_Lab_16, ink ? ink : output, intent,
         INTENT_RELATIVE_COLORIMETRIC, cmsFLAGS_NOCACHE | flags | cmsFLAGS_GAMUTCHECK);
    }
  }

  if(!d->xform || (ink && !d->proof_xform) || (alarm && !d->gamut_xform))
    _delete_transforms(d);
}

// the gamut check's mark is a UI tone, as gamma's views are: device
// (0, 1, 1) of the window, rendered to the output as the pixels are, so a
// proof narrower than the device clips it. an ink proof's frame holds the
// window's output, so the mark goes there, not through the ink. output is
// the profile xform converts to
static void _gamut_mark(dt_iop_colorout_data_t *d,
                        const dt_dev_pixelpipe_t *pipe,
                        const cmsHPROFILE Lab,
                        const cmsHPROFILE output,
                        const cmsUInt32Number output_format)
{
  const dt_color_encoding_t *const device = &pipe->output.device;
  if(!device->profile) return;
  cmsHTRANSFORM to_lab = cmsCreateTransform(device->profile, TYPE_RGBA_FLT, Lab, TYPE_LabA_FLT,
                                            device->intent, cmsFLAGS_NOCACHE);
  if(!to_lab) return;
  const dt_aligned_pixel_t cyan = { 0.0f, 1.0f, 1.0f, 0.0f };
  dt_aligned_pixel_t lab = { 0.0f }, mark = { 0.0f };
  cmsDoTransform(to_lab, cyan, lab, 1);
  cmsDeleteTransform(to_lab);

  if(d->proof_xform)
  {
    cmsHTRANSFORM to_output = cmsCreateTransform(Lab, TYPE_LabA_FLT, output, output_format,
                                                 INTENT_RELATIVE_COLORIMETRIC, cmsFLAGS_NOCACHE);
    if(!to_output) return;
    cmsDoTransform(to_output, lab, mark, 1);
    cmsDeleteTransform(to_output);
    _encode(d, mark, 1);
  }
  else if(!_to_output(d, mark, lab, 1, NULL))
    return;
  for(int c = 0; c < 3; c++) d->gamut_mark[c] = mark[c];
}

// colorout filtered out: the pixels stay in the working RGB, which the run
// resolves as its output
static void _leave_out(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece)
{
  piece->enabled = FALSE;
  const dt_iop_order_iccprofile_info_t *const work =
    dt_ioppr_get_pipe_work_profile_info(piece->pipe);
  if(work)
    dt_ioppr_set_pipe_output_profile_info(self->dev, piece->pipe, work->type,
                                          work->filename, work->profile, work->intent);
}

void commit_params(dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_colorout_params_t *p = (dt_iop_colorout_params_t *)p1;
  dt_iop_colorout_data_t *d = piece->data;

  // stored params need not terminate the file name
  p->filename[sizeof(p->filename) - 1] = '\0';

  d->type = p->type;

  // to be used in pixel-pipe cache
  dt_ioppr_set_pipe_export_profile_info(self->dev, piece->pipe, p->type, p->filename, p->intent);

  const gboolean force_lcms2 = dt_conf_get_bool("plugins/lighttable/export/force_lcms2");

  dt_colorspaces_color_profile_type_t out_type = DT_COLORSPACE_SRGB;
  gchar *out_filename = NULL;
  dt_iop_color_intent_t out_intent = DT_INTENT_PERCEPTUAL;
  gboolean presented = FALSE;
  // the pipe renders to its window's output
  gboolean window = FALSE;

  const cmsHPROFILE Lab = dt_colorspaces_get_profile(DT_COLORSPACE_LAB, "", DT_PROFILE_DIRECTION_ANY)->profile;

  cmsHPROFILE output = NULL;
  cmsHPROFILE ink = NULL;
  cmsHPROFILE hdr_linear = NULL;
  cmsUInt32Number output_format = TYPE_RGBA_FLT;

  // the proof settings, read once: the darkroom changes them from the GUI
  // thread, and one commit builds from one set
  const dt_colorspaces_t *const cp = darktable.color_profiles;
  const dt_colorspaces_color_mode_t mode = cp->mode;
  const dt_colorspaces_color_profile_type_t proof_type = cp->softproof_type;
  char proof_filename[sizeof(cp->softproof_filename)];
  g_strlcpy(proof_filename, cp->softproof_filename, sizeof(proof_filename));
  const dt_iop_color_intent_t proof_intent = cp->softproof_intent;
  const gboolean proof_bpc = cp->softproof_bpc;

  // the full pipe shows the proof. the preview pipe's proof feeds the
  // scopes, the color pickers and navigation, which read the proof's
  // values, so it leaves out the gamut check's marks
  d->mode = dt_pipe_is_full(pipe) ? mode
          : dt_pipe_is_preview(pipe) && mode != DT_PROFILE_NORMAL ? DT_PROFILE_SOFTPROOF
          : DT_PROFILE_NORMAL;
  const gboolean proof = d->mode != DT_PROFILE_NORMAL;
  const gboolean gamutcheck = d->mode == DT_PROFILE_GAMUTCHECK;

  _delete_transforms(d);
  dt_mark_colormatrix_invalid(&d->cmatrix[0][0]);
  d->lut[0][0] = -1.0f;
  d->lut[1][0] = -1.0f;
  d->lut[2][0] = -1.0f;
  d->clip = FALSE;
  for(int c = 0; c < 4; c++) d->gamut_scale[c] = 1.0f;
  d->gamut_tolerance = 0.0f;
  d->gamut_black = 0.0f;
  // the output's own cyan where no conversion of the device's can be built
  d->gamut_mark[0] = 0.0f;
  d->gamut_mark[1] = d->gamut_mark[2] = 1.0f;
  d->gamut_mark[3] = 0.0f;
  piece->process_cl_ready = TRUE;

  // a failure records nothing: no profile describes what process() then writes
  dt_color_encoding_t *const encoding = &pipe->colorout_encoding;
  memset(encoding, 0, sizeof(*encoding));
  encoding->type = DT_COLORSPACE_NONE;

  /* if we are exporting then check and set usage of override profile */
  if(dt_pipe_is_export(pipe))
  {
    if(pipe->icc_type != DT_COLORSPACE_NONE)
    {
      p->type = pipe->icc_type;
      dt_strlcpy_to_fixed(p->filename, pipe->icc_filename, sizeof(p->filename));
    }
    if((unsigned int)pipe->icc_intent < DT_INTENT_LAST) p->intent = pipe->icc_intent;

    out_type = p->type;
    out_filename = p->filename;
    out_intent = p->intent;
  }
  else if(dt_pipe_is_thumb(pipe))
  {
    // with the intent of the main window's device encoding, which the
    // registry's DISPLAY entry holds
    dt_display_output_t main_window;
    dt_colorspaces_get_display_output(DT_COLORSPACE_DISPLAY, &main_window);
    out_type = dt_mipmap_cache_get_colorspace();
    out_filename = "";
    out_intent = main_window.device.intent;
  }
  else
  {
    /* we are not exporting, the pipe is presented */
    presented = TRUE;
    // a soft proof renders as an export to the proof profile, so its frame
    // is in that profile, and the presenter shows it as any other. a frame
    // holds RGB, so an ink proof goes on to the window's output
    if(proof)
    {
      const dt_colorspaces_color_profile_t *prof = dt_colorspaces_get_profile
        (proof_type, proof_filename,
         DT_PROFILE_DIRECTION_OUT | DT_PROFILE_DIRECTION_DISPLAY | DT_PROFILE_DIRECTION_DISPLAY2);
      if(prof && cmsGetColorSpace(prof->profile) != cmsSigRgbData) ink = prof->profile;
    }
    if(proof && !ink)
    {
      out_type = proof_type;
      out_filename = proof_filename;
      out_intent = proof_intent;
      d->clip = TRUE;
    }
    else
    {
      out_type = pipe->output.image.type;
      out_filename = pipe->output.image.filename;
      out_intent = pipe->output.image.intent;
      window = TRUE;
    }
  }

  // a pipe without a display encoding is not presented: a dt_dev_image()
  // sink of type DT_COLORSPACE_NONE filters colorout out and keeps the
  // pixels in the working RGB. the run resolves its output from the final
  // working profile; modules after colorout that read it in commit_params()
  // get this one
  if(out_type == DT_COLORSPACE_NONE)
  {
    _leave_out(self, piece);
    return;
  }

  // the profile the transform converts to, once the fallbacks below have
  // replaced a missing or unsupported one
  dt_colorspaces_color_profile_type_t built_type = out_type;
  const char *built_filename = out_filename;

  // when the output type is Lab then process is a nop, so we can avoid creating a transform
  // and the subsequent error messages but still have to publish the profile_info
  d->type = out_type;
  d->hdr_transfer = dt_colorspaces_hdr_transfer(out_type);
  if(out_type == DT_COLORSPACE_LAB)
  {
    dt_ioppr_set_pipe_output_profile_info(self->dev, piece->pipe, d->type, out_filename, Lab,
                                          p->intent);
    encoding->type = DT_COLORSPACE_LAB;
    g_strlcpy(encoding->filename, out_filename, sizeof(encoding->filename));
    encoding->profile = Lab;
    encoding->intent = out_intent;
    encoding->headroom = pipe->output_headroom;
    return;
  }

  /* creating output profile */
  // a presented pipe renders to the profile its run took with its output,
  // which a new screen profile may have replaced in the registry since
  const gboolean lookup_display = !window
    && (out_type == DT_COLORSPACE_DISPLAY || out_type == DT_COLORSPACE_DISPLAY2);
  if(lookup_display)
    pthread_rwlock_rdlock(&darktable.color_profiles->xprofile_lock);

  const dt_colorspaces_color_profile_t *out_profile
      = window ? NULL
               : dt_colorspaces_get_profile(out_type, out_filename,
                                            DT_PROFILE_DIRECTION_OUT
                                            | DT_PROFILE_DIRECTION_DISPLAY
                                            | DT_PROFILE_DIRECTION_DISPLAY2);
  output = window ? pipe->output.image.profile : out_profile ? out_profile->profile : NULL;
  const cmsHPROFILE out_content = output;
  if(output)
  {
    if(out_type == DT_COLORSPACE_XYZ) output_format = TYPE_XYZA_FLT;
  }
  else
  {
    output = dt_colorspaces_get_profile(DT_COLORSPACE_SRGB, "",
                                        DT_PROFILE_DIRECTION_OUT
                                        | DT_PROFILE_DIRECTION_DISPLAY
                                        | DT_PROFILE_DIRECTION_DISPLAY2)
                 ->profile;
    built_type = DT_COLORSPACE_SRGB;
    built_filename = "";
    if(d->clip)
    {
      dt_control_log(_("missing softproof profile has been replaced by sRGB!"));
      dt_print(DT_DEBUG_ALWAYS, "missing softproof profile `%s' has been replaced by sRGB!",
               dt_colorspaces_get_name(out_type, out_filename));
    }
    else
    {
      dt_control_log(_("missing output profile has been replaced by sRGB!"));
      dt_print(DT_DEBUG_ALWAYS, "missing output profile `%s' has been replaced by sRGB!",
               dt_colorspaces_get_name(out_type, out_filename));
    }
  }

  cmsHPROFILE built_profile = output;

  if(d->hdr_transfer)
  {
    hdr_linear = dt_colorspaces_linearize_profile(output);
    // nothing to build a transform on
    if(!hdr_linear) goto done;
    output = hdr_linear;
  }

  // a soft proof has its own intent and black point compensation. an
  // export asks for none, which littlecms still applies for perceptual and
  // saturation into a v4 profile
  const dt_iop_color_intent_t intent = proof ? proof_intent : out_intent;
  const gboolean bpc = proof && proof_bpc;
  // the export of a PQ or HLG proof clips in dt_hdr_encode(), past what
  // littlecms' gamut check sees: its linear values tell
  const gboolean alarm = gamutcheck && (ink || !d->hdr_transfer);

  if(ink || !_matrix_path(d, output, proof, intent, bpc, force_lcms2))
  {
    dt_mark_colormatrix_invalid(&d->cmatrix[0][0]);
    piece->process_cl_ready = FALSE;
    _create_transforms(d, Lab, output, output_format, ink, intent, bpc, alarm);
  }

  // user selected a non-supported output profile, check that:
  if(!d->xform && !dt_is_valid_colormatrix(d->cmatrix[0][0]))
  {
    dt_control_log(_("unsupported output profile has been replaced by sRGB!"));
    dt_print(DT_DEBUG_ALWAYS,
             "unsupported output profile `%s' has been replaced by sRGB!",
             out_profile ? out_profile->name : dt_colorspaces_get_name(out_type, out_filename));
    output = dt_colorspaces_get_profile(DT_COLORSPACE_SRGB, "", DT_PROFILE_DIRECTION_OUT)->profile;
    built_type = DT_COLORSPACE_SRGB;
    built_filename = "";
    built_profile = output;

    if(ink || !_matrix_path(d, output, proof, intent, bpc, force_lcms2))
    {
      dt_mark_colormatrix_invalid(&d->cmatrix[0][0]);
      piece->process_cl_ready = FALSE;

      _create_transforms(d, Lab, output, output_format, ink, intent, bpc, alarm);
    }
  }

  // the GPU runs xform as a 3D LUT over Lab L 0..100 (_bake_clut()), only
  // where that domain covers the input: not for a PQ or HLG output, which
  // keeps light above reference white. "always use LittleCMS 2" asks for
  // the exact transform, which only the CPU has, an ink proof clips between
  // two transforms, and the gamut check off the matrix path runs on the CPU
  if(d->xform && !d->proof_xform && !force_lcms2 && !gamutcheck && !d->hdr_transfer)
    piece->process_cl_ready = TRUE;

  // the gamut check on linear values: a matrix path's, or littlecms' for
  // PQ and HLG. profiles store their primaries as s15Fixed16 numbers,
  // rounded by up to half a 2^-16 step: a primary the input and the proof
  // share comes out up to 2^-16 times a row of |M^-1| off the range,
  // relative to the color's magnitude. black comes out of the float Lab
  // conversions in and out (dt_XYZ_to_Lab(), dt_Lab_to_XYZ()) up to a
  // float step of 16 each off zero in 116 f(Y) - 16, 2^-18 together,
  // which is 2^-18 / kappa in XYZ, times a row of |M^-1|: no gamut limit
  // either
  if(gamutcheck && !d->gamut_xform)
  {
    dt_colormatrix_t to_rgb;
    const gboolean matrix = dt_is_valid_colormatrix(d->cmatrix[0][0]);
    if(matrix) memcpy(to_rgb, d->cmatrix, sizeof(to_rgb));
    if(matrix || !dt_colorspaces_get_matrix_from_output_profile(output, to_rgb, NULL, NULL, NULL, 0))
    {
      float rows = 0.0f;
      for(int k = 0; k < 3; k++)
        rows = MAX(rows, fabsf(to_rgb[k][0]) + fabsf(to_rgb[k][1]) + fabsf(to_rgb[k][2]));
      d->gamut_tolerance = rows / 65536.0f;
      d->gamut_black = rows * (27.0f / 24389.0f) / 262144.0f;
    }
  }

done:
  if(lookup_display)
    pthread_rwlock_unlock(&darktable.color_profiles->xprofile_lock);

  // now try to initialize unbounded mode:
  // we do extrapolation for input values above 1.0f.
  // unfortunately we can only do this if we got the computation
  // in our hands, i.e. for the fast builtin-dt-matrix-profile path.
  for(int k = 0; k < 3; k++)
  {
    // omit luts marked as linear (negative as marker)
    if(d->lut[k][0] >= 0.0f)
    {
      const float x[4] = { 0.7f, 0.8f, 0.9f, 1.0f };
      const float y[4] = { _lerp_u_lut(d->lut[k], x[0]),
                           _lerp_u_lut(d->lut[k], x[1]),
                           _lerp_u_lut(d->lut[k], x[2]),
                           _lerp_u_lut(d->lut[k], x[3]) };
      dt_iop_estimate_exp(x, y, 4, d->unbounded_coeffs[k]);
    }
    else
      d->unbounded_coeffs[k][0] = -1.0f;
  }

  if(gamutcheck && dt_is_valid_colormatrix(d->cmatrix[0][0]) && !d->hdr_transfer)
    for(int c = 0; c < 3; c++) d->gamut_scale[c] = _encoding_scale(d, c);

  if(gamutcheck && (d->xform || dt_is_valid_colormatrix(d->cmatrix[0][0])))
    _gamut_mark(d, pipe, Lab, output, output_format);

  dt_colorspaces_cleanup_profile(hdr_linear);

  // with no transform process() has nothing to convert with. a presented
  // pipe's frame says what its pixels are; an export's file would claim
  // the requested profile, so process() fails it
  const gboolean built = d->xform || dt_is_valid_colormatrix(d->cmatrix[0][0]);
  if(!built)
  {
    dt_print(DT_DEBUG_ALWAYS, "[colorout] no transform to `%s', %s",
             dt_colorspaces_get_name(out_type, out_filename),
             presented ? "output left in the working RGB" : "the export fails");
    if(presented)
    {
      _leave_out(self, piece);
      return;
    }
    piece->process_cl_ready = FALSE;
  }

  dt_ioppr_set_pipe_output_profile_info(self->dev, piece->pipe, d->type, out_filename,
                                        out_content, p->intent);
  // render the proof for its export's headroom and let the screen clip, as
  // Resolve's viewer shows the deliverable's transform (18.5 manual, ch. 9-10)
  if(proof)
    piece->pipe->output_headroom =
      dt_ioppr_profile_headroom(ink ? proof_type : built_type);

  // process() encodes for out_type's HDR transfer function, which no
  // profile describes once sRGB has replaced out_type's profile
  if(built && !(d->hdr_transfer && built_type != out_type))
  {
    encoding->type = built_type;
    g_strlcpy(encoding->filename, built_filename, sizeof(encoding->filename));
    encoding->profile = built_profile;
    encoding->intent = out_intent;
    encoding->headroom = pipe->output_headroom;
  }
}

void init_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1, sizeof(dt_iop_colorout_data_t));
  dt_iop_colorout_data_t *d = piece->data;
  d->xform = NULL;
}

void cleanup_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  _delete_transforms(piece->data);

  free(piece->data);
  piece->data = NULL;
}

void gui_update(dt_iop_module_t *self)
{
  dt_iop_colorout_gui_data_t *g = self->gui_data;
  dt_iop_colorout_params_t *p = self->params;

  // the GUI handlers read the stored file name as a C string
  p->filename[sizeof(p->filename) - 1] = '\0';

  dt_bauhaus_combobox_set(g->output_intent, (int)p->intent);

  for(GList *iter = darktable.color_profiles->profiles; iter; iter = g_list_next(iter))
  {
    dt_colorspaces_color_profile_t *pp = iter->data;
    if(pp->out_pos > -1 &&
       p->type == pp->type && (p->type != DT_COLORSPACE_FILE || !strcmp(p->filename, pp->filename)))
    {
      dt_bauhaus_combobox_set(g->output_profile, pp->out_pos);
      return;
    }
  }

  dt_bauhaus_combobox_set(g->output_profile, 0);
  dt_print(DT_DEBUG_ALWAYS,
           "[colorout] could not find requested profile `%s'!",
           dt_colorspaces_get_name(p->type, p->filename));
}

void init(dt_iop_module_t *self)
{
  dt_iop_default_init(self);

  self->hide_enable_button = TRUE;
  self->default_enabled = TRUE;
}

static void _preference_changed(gpointer instance, dt_iop_module_t *self)
{
  dt_iop_colorout_gui_data_t *g = self->gui_data;

  const gboolean force_lcms2 = dt_conf_get_bool("plugins/lighttable/export/force_lcms2");
  if(force_lcms2)
  {
    gtk_widget_set_no_show_all(g->output_intent, FALSE);
    gtk_widget_set_visible(g->output_intent, TRUE);
  }
  else
  {
    gtk_widget_set_no_show_all(g->output_intent, TRUE);
    gtk_widget_set_visible(g->output_intent, FALSE);
  }
}

void gui_init(dt_iop_module_t *self)
{
  const gboolean force_lcms2 = dt_conf_get_bool("plugins/lighttable/export/force_lcms2");

  dt_iop_colorout_gui_data_t *g = IOP_GUI_ALLOC(colorout);

  DT_BAUHAUS_COMBOBOX_NEW_FULL(g->output_intent, self, NULL, N_("output intent"),
                               _("rendering intent"),
                               0, _intent_changed, self,
                               N_("perceptual"),
                               N_("relative colorimetric"),
                               NC_("rendering intent", "saturation"),
                               N_("absolute colorimetric"));

  if(!force_lcms2)
  {
    gtk_widget_set_no_show_all(g->output_intent, TRUE);
    gtk_widget_set_visible(g->output_intent, FALSE);
  }

  g->output_profile = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->output_profile, NULL, N_("export profile"));
  for(GList *l = darktable.color_profiles->profiles; l; l = g_list_next(l))
  {
    dt_colorspaces_color_profile_t *prof = l->data;
    if(prof->out_pos > -1) dt_bauhaus_combobox_add(g->output_profile, prof->name);
  }

  char *tooltip = dt_ioppr_get_location_tooltip("out", _("export ICC profiles"));
  gtk_widget_set_tooltip_markup(g->output_profile, tooltip);
  g_free(tooltip);

  self->widget = dt_gui_vbox(g->output_intent, g->output_profile);

  g_signal_connect(G_OBJECT(g->output_profile), "value-changed",
                   G_CALLBACK(_output_profile_changed), (gpointer)self);

  // update the gui when the preferences changed (i.e. show intent when using lcms2)
  DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_PREFERENCES_CHANGE, _preference_changed);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
