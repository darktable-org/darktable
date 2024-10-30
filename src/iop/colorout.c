/*
    This file is part of darktable,
    Copyright (C) 2009-2024 darktable developers.

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
#include "common/colorspaces.h"
#include "common/colorspaces_inline_conversions.h"
#include "common/dttypes.h"
#include "common/imagebuf.h"
#include "common/iop_profile.h"
#include "common/opencl.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"

#include <assert.h>
#include <gdk/gdkkeysyms.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

// max iccprofile file name length
// must be in synch with dt_colorspaces_color_profile_t
#define DT_IOP_COLOR_ICC_LEN 512
#define LUT_SAMPLES 0x10000

DT_MODULE_INTROSPECTION(5, dt_iop_colorout_params_t)

typedef struct dt_iop_colorout_data_t
{
  dt_colorspaces_color_profile_type_t type;
  dt_colorspaces_color_mode_t mode;
  float lut[3][LUT_SAMPLES];
  dt_colormatrix_t cmatrix;
  cmsHTRANSFORM *xform;
  float unbounded_coeffs[3][3]; // for extrapolation of shaper curves
} dt_iop_colorout_data_t;

typedef struct dt_iop_colorout_global_data_t
{
  int kernel_colorout;
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
  return IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_ONE_INSTANCE;
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
      g_strlcpy(n->filename, o->iccprofile, sizeof(n->filename));
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
    g_strlcpy(n->filename, o->filename, sizeof(n->filename));
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
}

void cleanup_global(dt_iop_module_so_t *self)
{
  dt_iop_colorout_global_data_t *gd = self->data;
  dt_opencl_free_kernel(gd->kernel_colorout);
  free(self->data);
  self->data = NULL;
}

static void intent_changed(GtkWidget *widget, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_colorout_params_t *p = self->params;
  p->intent = (dt_iop_color_intent_t)dt_bauhaus_combobox_get(widget);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void output_profile_changed(GtkWidget *widget, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_colorout_params_t *p = self->params;
  int pos = dt_bauhaus_combobox_get(widget);

  for(GList *profiles = darktable.color_profiles->profiles; profiles; profiles = g_list_next(profiles))
  {
    dt_colorspaces_color_profile_t *pp = profiles->data;
    if(pp->out_pos == pos)
    {
      p->type = pp->type;
      g_strlcpy(p->filename, pp->filename, sizeof(p->filename));
      dt_dev_add_history_item(darktable.develop, self, TRUE);

      DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_CONTROL_PROFILE_USER_CHANGED, DT_COLORSPACES_PROFILE_TYPE_EXPORT);
      return;
    }
  }

  dt_print(DT_DEBUG_ALWAYS,
           "[colorout] color profile %s seems to have disappeared!",
           dt_colorspaces_get_name(p->type, p->filename));
}

static void _signal_profile_changed(gpointer instance, gpointer user_data)
{
  dt_develop_t *dev = (dt_develop_t *)user_data;
  if(!dev->gui_attached || dev->gui_leaving) return;
  dt_dev_reprocess_center(dev);
}

#if 1
static float lerp_lut(const float *const lut, const float v)
{
  // TODO: check if optimization is worthwhile!
  const float ft = CLAMPS(v * (LUT_SAMPLES - 1), 0, LUT_SAMPLES - 1);
  const int t = ft < LUT_SAMPLES - 2 ? ft : LUT_SAMPLES - 2;
  const float f = ft - t;
  const float l1 = lut[t];
  const float l2 = lut[t + 1];
  return l1 * (1.0f - f) + l2 * f;
}
#endif

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

#ifdef HAVE_OPENCL
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
    size_t origin[] = { 0, 0, 0 };
    size_t region[] = { roi_in->width, roi_in->height, 1 };
    err = dt_opencl_enqueue_copy_image(devid, dev_in, dev_out, origin, origin, region);
    if(err != CL_SUCCESS) goto error;
    return CL_SUCCESS;
  }


  float cmatrix[9];
  pack_3xSSE_to_3x3(d->cmatrix, cmatrix);
  dev_m = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * 9, cmatrix);
  if(dev_m == NULL) goto error;
  dev_r = dt_opencl_copy_host_to_device(devid, d->lut[0], 256, 256, sizeof(float));
  if(dev_r == NULL) goto error;
  dev_g = dt_opencl_copy_host_to_device(devid, d->lut[1], 256, 256, sizeof(float));
  if(dev_g == NULL) goto error;
  dev_b = dt_opencl_copy_host_to_device(devid, d->lut[2], 256, 256, sizeof(float));
  if(dev_b == NULL) goto error;
  dev_coeffs
      = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * 3 * 3, (float *)d->unbounded_coeffs);
  if(dev_coeffs == NULL) goto error;
  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_colorout, width, height,
    CLARG(dev_in), CLARG(dev_out), CLARG(width), CLARG(height), CLARG(dev_m), CLARG(dev_r), CLARG(dev_g),
    CLARG(dev_b), CLARG(dev_coeffs));

error:
  dt_opencl_release_mem_object(dev_m);
  dt_opencl_release_mem_object(dev_r);
  dt_opencl_release_mem_object(dev_g);
  dt_opencl_release_mem_object(dev_b);
  dt_opencl_release_mem_object(dev_coeffs);
  return err;
}
#endif

static void process_fastpath_apply_tonecurves(dt_iop_module_t *self,
                                              dt_dev_pixelpipe_iop_t *piece,
                                              void *const ovoid,
                                              const dt_iop_roi_t *const roi_out)
{
  const dt_iop_colorout_data_t *const d = piece->data;

  if(dt_is_valid_colormatrix(d->cmatrix[0][0]))
  {
    const size_t npixels = (size_t)roi_out->width * roi_out->height;
    float *const restrict out = (float *const)ovoid;
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
  const int gamutcheck = (d->mode == DT_PROFILE_GAMUTCHECK);
  // figure out the number of pixels each thread needs to process,
  // rounded up to a multiple of the CPU's cache line size
  const size_t nthreads = dt_get_num_threads();
  const size_t chunksize = dt_cacheline_chunks(npixels, nthreads);
  DT_OMP_FOR()
  for(size_t chunkstart = 0; chunkstart < npixels; chunkstart += chunksize)
  {
    size_t count = MIN(chunkstart + chunksize, npixels) - chunkstart;
    float *const outp = out + 4 * chunkstart;

    cmsDoTransform(d->xform, in + 4*chunkstart, outp, count);

    if(gamutcheck)
    {
      static const dt_aligned_pixel_t cyan = { 0.0f, 1.0f, 1.0f, 0.0f };
      for(int j = 0; j < count; j++)
      {
        if(outp[4*j+0] < 0.0f || outp[4*j+1] < 0.0f || outp[4*j+2] < 0.0f)
        {
          copy_pixel_nontemporal(outp + 4*j, cyan);
        }
      }
    }
  }
  dt_omploop_sfence();
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

  if(d->type == DT_COLORSPACE_LAB)
  {
    dt_iop_image_copy_by_size(ovoid, ivoid, roi_out->width, roi_out->height, piece->colors);
  }
  else if(dt_is_valid_colormatrix(d->cmatrix[0][0]))
  {
    if (!_transform_cmatrix(d, out, (float*)ivoid, npixels))
      process_fastpath_apply_tonecurves(self, piece, ovoid, roi_out);
  }
  else
  {
    _transform_lcms(d, out, (float*)ivoid, npixels);
  }
}

void commit_params(dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_colorout_params_t *p = (dt_iop_colorout_params_t *)p1;
  dt_iop_colorout_data_t *d = piece->data;

  d->type = p->type;

  const gboolean force_lcms2 = dt_conf_get_bool("plugins/lighttable/export/force_lcms2");

  dt_colorspaces_color_profile_type_t out_type = DT_COLORSPACE_SRGB;
  gchar *out_filename = NULL;
  dt_iop_color_intent_t out_intent = DT_INTENT_PERCEPTUAL;

  const cmsHPROFILE Lab = dt_colorspaces_get_profile(DT_COLORSPACE_LAB, "", DT_PROFILE_DIRECTION_ANY)->profile;

  cmsHPROFILE output = NULL;
  cmsHPROFILE softproof = NULL;
  cmsUInt32Number output_format = TYPE_RGBA_FLT;

  d->mode = (pipe->type & DT_DEV_PIXELPIPE_FULL) ? darktable.color_profiles->mode : DT_PROFILE_NORMAL;

  if(d->xform)
  {
    cmsDeleteTransform(d->xform);
    d->xform = NULL;
  }
  dt_mark_colormatrix_invalid(&d->cmatrix[0][0]);
  d->lut[0][0] = -1.0f;
  d->lut[1][0] = -1.0f;
  d->lut[2][0] = -1.0f;
  piece->process_cl_ready = TRUE;

  /* if we are exporting then check and set usage of override profile */
  if(pipe->type & DT_DEV_PIXELPIPE_EXPORT)
  {
    if(pipe->icc_type != DT_COLORSPACE_NONE)
    {
      p->type = pipe->icc_type;
      g_strlcpy(p->filename, pipe->icc_filename, sizeof(p->filename));
    }
    if((unsigned int)pipe->icc_intent < DT_INTENT_LAST) p->intent = pipe->icc_intent;

    out_type = p->type;
    out_filename = p->filename;
    out_intent = p->intent;
  }
  else if(pipe->type & DT_DEV_PIXELPIPE_THUMBNAIL)
  {
    out_type = dt_mipmap_cache_get_colorspace();
    out_filename = (out_type == DT_COLORSPACE_DISPLAY ? darktable.color_profiles->display_filename : "");
    out_intent = darktable.color_profiles->display_intent;
  }
  else if(pipe->type & DT_DEV_PIXELPIPE_PREVIEW2)
  {
    /* preview2 is only used in second darkroom window, using display2 profile as output */
    out_type = darktable.color_profiles->display2_type;
    out_filename = darktable.color_profiles->display2_filename;
    out_intent = darktable.color_profiles->display2_intent;
  }
  else
  {
    /* we are not exporting, using display profile as output */
    out_type = darktable.color_profiles->display_type;
    out_filename = darktable.color_profiles->display_filename;
    out_intent = darktable.color_profiles->display_intent;
  }

  // when the output type is Lab then process is a nop, so we can avoid creating a transform
  // and the subsequent error messages
  d->type = out_type;
  if(out_type == DT_COLORSPACE_LAB)
    return;

  /*
   * Setup transform flags
   */
  uint32_t transformFlags = 0;

  /* creating output profile */
  if(out_type == DT_COLORSPACE_DISPLAY || out_type == DT_COLORSPACE_DISPLAY2)
    pthread_rwlock_rdlock(&darktable.color_profiles->xprofile_lock);

  const dt_colorspaces_color_profile_t *out_profile
      = dt_colorspaces_get_profile(out_type, out_filename,
                                   DT_PROFILE_DIRECTION_OUT
                                   | DT_PROFILE_DIRECTION_DISPLAY
                                   | DT_PROFILE_DIRECTION_DISPLAY2);
  if(out_profile)
  {
    output = out_profile->profile;
    if(out_type == DT_COLORSPACE_XYZ) output_format = TYPE_XYZA_FLT;
  }
  else
  {
    output = dt_colorspaces_get_profile(DT_COLORSPACE_SRGB, "",
                                        DT_PROFILE_DIRECTION_OUT
                                        | DT_PROFILE_DIRECTION_DISPLAY
                                        | DT_PROFILE_DIRECTION_DISPLAY2)
                 ->profile;
    dt_control_log(_("missing output profile has been replaced by sRGB!"));
    dt_print(DT_DEBUG_ALWAYS, "missing output profile `%s' has been replaced by sRGB!",
             dt_colorspaces_get_name(out_type, out_filename));
  }

  /* creating softproof profile if softproof is enabled */
  if((d->mode != DT_PROFILE_NORMAL) && (pipe->type & DT_DEV_PIXELPIPE_FULL))
  {
    const dt_colorspaces_color_profile_t *prof = dt_colorspaces_get_profile
      (darktable.color_profiles->softproof_type,
       darktable.color_profiles->softproof_filename,
       DT_PROFILE_DIRECTION_OUT | DT_PROFILE_DIRECTION_DISPLAY | DT_PROFILE_DIRECTION_DISPLAY2);

    if(prof)
      softproof = prof->profile;
    else
    {
      softproof = dt_colorspaces_get_profile(DT_COLORSPACE_SRGB, "",
                                             DT_PROFILE_DIRECTION_OUT
                                             | DT_PROFILE_DIRECTION_DISPLAY
                                             | DT_PROFILE_DIRECTION_DISPLAY2)
                      ->profile;
      dt_control_log(_("missing softproof profile has been replaced by sRGB!"));
      dt_print(DT_DEBUG_ALWAYS, "missing softproof profile `%s' has been replaced by sRGB!",
               dt_colorspaces_get_name(darktable.color_profiles->softproof_type,
                                       darktable.color_profiles->softproof_filename));
    }

    // some of our internal profiles are what lcms considers ideal profiles as they have a parametric TRC so
    // taking a roundtrip through those profiles during softproofing has no effect. as a workaround we have to
    // make lcms quantisize those gamma tables to get the desired effect.
    // in case that fails we don't enable softproofing.
    softproof = dt_colorspaces_make_temporary_profile(softproof);
    if(softproof)
    {
      /* TODO: the use of bpc should be userconfigurable either from module or preference pane */
      /* softproof flag and black point compensation */
      transformFlags |= cmsFLAGS_SOFTPROOFING | cmsFLAGS_NOCACHE | cmsFLAGS_BLACKPOINTCOMPENSATION;

      if(d->mode == DT_PROFILE_GAMUTCHECK) transformFlags |= cmsFLAGS_GAMUTCHECK;
    }
  }

  /*
   * NOTE: theoretically, we should be passing
   * UsedDirection = LCMS_USED_AS_PROOF  into
   * dt_colorspaces_get_matrix_from_output_profile() so that
   * dt_colorspaces_get_matrix_from_profile() knows it, but since we do not try
   * to use our matrix codepath when softproof is enabled, this seemed redundant.
   */

  /* get matrix from profile, if softproofing or high quality exporting always go xform codepath */
  if(d->mode != DT_PROFILE_NORMAL || force_lcms2
     || dt_colorspaces_get_matrix_from_output_profile(output, d->cmatrix,
                                                      d->lut[0], d->lut[1], d->lut[2], LUT_SAMPLES))
  {
    dt_mark_colormatrix_invalid(&d->cmatrix[0][0]);
    piece->process_cl_ready = FALSE;
    d->xform = cmsCreateProofingTransform(Lab, TYPE_LabA_FLT, output, output_format, softproof,
                                          out_intent, INTENT_RELATIVE_COLORIMETRIC, transformFlags);
  }

  // user selected a non-supported output profile, check that:
  if(!d->xform && !dt_is_valid_colormatrix(d->cmatrix[0][0]))
  {
    dt_control_log(_("unsupported output profile has been replaced by sRGB!"));
    dt_print(DT_DEBUG_ALWAYS,
             "unsupported output profile `%s' has been replaced by sRGB!",
             out_profile->name);
    output = dt_colorspaces_get_profile(DT_COLORSPACE_SRGB, "", DT_PROFILE_DIRECTION_OUT)->profile;

    if(d->mode != DT_PROFILE_NORMAL
       || dt_colorspaces_get_matrix_from_output_profile(output, d->cmatrix,
                                                        d->lut[0], d->lut[1], d->lut[2], LUT_SAMPLES))
    {
      dt_mark_colormatrix_invalid(&d->cmatrix[0][0]);
      piece->process_cl_ready = FALSE;

      d->xform = cmsCreateProofingTransform(Lab, TYPE_LabA_FLT, output, output_format, softproof,
                                            out_intent, INTENT_RELATIVE_COLORIMETRIC, transformFlags);
    }
  }

  if(out_type == DT_COLORSPACE_DISPLAY || out_type == DT_COLORSPACE_DISPLAY2)
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
      const float y[4] = { lerp_lut(d->lut[k], x[0]),
                           lerp_lut(d->lut[k], x[1]),
                           lerp_lut(d->lut[k], x[2]),
                           lerp_lut(d->lut[k], x[3]) };
      dt_iop_estimate_exp(x, y, 4, d->unbounded_coeffs[k]);
    }
    else
      d->unbounded_coeffs[k][0] = -1.0f;
  }

  // softproof is never the original but always a copy that went through dt_colorspaces_make_temporary_profile()
  dt_colorspaces_cleanup_profile(softproof);

  dt_ioppr_set_pipe_output_profile_info(self->dev, piece->pipe, d->type, out_filename, p->intent);
}

void init_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1, sizeof(dt_iop_colorout_data_t));
  dt_iop_colorout_data_t *d = piece->data;
  d->xform = NULL;
}

void cleanup_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_colorout_data_t *d = piece->data;
  if(d->xform)
  {
    cmsDeleteTransform(d->xform);
    d->xform = NULL;
  }

  free(piece->data);
  piece->data = NULL;
}

void gui_update(dt_iop_module_t *self)
{
  dt_iop_colorout_gui_data_t *g = self->gui_data;
  dt_iop_colorout_params_t *p = self->params;

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

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  DT_BAUHAUS_COMBOBOX_NEW_FULL(g->output_intent, self, NULL, N_("output intent"),
                               _("rendering intent"),
                               0, intent_changed, self,
                               N_("perceptual"),
                               N_("relative colorimetric"),
                               NC_("rendering intent", "saturation"),
                               N_("absolute colorimetric"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->output_intent, TRUE, TRUE, 0);

  if(!force_lcms2)
  {
    gtk_widget_set_no_show_all(g->output_intent, TRUE);
    gtk_widget_set_visible(g->output_intent, FALSE);
  }

  g->output_profile = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->output_profile, NULL, N_("export profile"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->output_profile, TRUE, TRUE, 0);
  for(GList *l = darktable.color_profiles->profiles; l; l = g_list_next(l))
  {
    dt_colorspaces_color_profile_t *prof = l->data;
    if(prof->out_pos > -1) dt_bauhaus_combobox_add(g->output_profile, prof->name);
  }

  char *tooltip = dt_ioppr_get_location_tooltip("out", _("export ICC profiles"));
  gtk_widget_set_tooltip_markup(g->output_profile, tooltip);
  g_free(tooltip);

  g_signal_connect(G_OBJECT(g->output_profile), "value-changed",
                   G_CALLBACK(output_profile_changed), (gpointer)self);

  // reload the profiles when the display or softproof profile changed!
  DT_CONTROL_SIGNAL_CONNECT(DT_SIGNAL_CONTROL_PROFILE_CHANGED, _signal_profile_changed, self->dev);
  // update the gui when the preferences changed (i.e. show intent when using lcms2)
  DT_CONTROL_SIGNAL_CONNECT(DT_SIGNAL_PREFERENCES_CHANGE, _preference_changed, self);
}

void gui_cleanup(dt_iop_module_t *self)
{
  DT_CONTROL_SIGNAL_DISCONNECT(_signal_profile_changed, self->dev);
  DT_CONTROL_SIGNAL_DISCONNECT(_preference_changed, self);

  IOP_GUI_FREE;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
