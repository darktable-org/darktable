/*
    This file is part of darktable,
    copyright (c) 2009--2011 johannes hanika.
    copyright (c) 2011 henrik andersson

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
#include "common/colormatrices.c"
#include "common/colorspaces.h"
#include "common/colorspaces_inline_conversions.h"
#include "common/image_cache.h"
#include "common/opencl.h"
#include "control/control.h"
#include "develop/develop.h"
#include "gui/gtk.h"
#ifdef HAVE_OPENJPEG
#include "common/imageio_j2k.h"
#endif
#include "common/imageio_jpeg.h"
#include "common/imageio_png.h"
#include "common/imageio_tiff.h"
#include "develop/imageop_math.h"
#include "iop/iop_api.h"
#include "common/iop_group.h"

#include "external/adobe_coeff.c"
#if defined(__SSE__)
#include <xmmintrin.h>
#endif
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <lcms2.h>

// max iccprofile file name length
#define DT_IOP_COLOR_ICC_LEN 100

#define LUT_SAMPLES 0x10000

DT_MODULE_INTROSPECTION(4, dt_iop_colorin_params_t)

static void update_profile_list(dt_iop_module_t *self);

typedef enum dt_iop_color_normalize_t
{
  DT_NORMALIZE_OFF,
  DT_NORMALIZE_SRGB,
  DT_NORMALIZE_ADOBE_RGB,
  DT_NORMALIZE_LINEAR_REC709_RGB,
  DT_NORMALIZE_LINEAR_REC2020_RGB
} dt_iop_color_normalize_t;

typedef struct dt_iop_colorin_params_t
{
  dt_colorspaces_color_profile_type_t type;
  char filename[DT_IOP_COLOR_ICC_LEN];
  dt_iop_color_intent_t intent;
  int normalize;
  int blue_mapping;
} dt_iop_colorin_params_t;

typedef struct dt_iop_colorin_gui_data_t
{
  GtkWidget *profile_combobox, *clipping_combobox;
  GList *image_profiles;
  int n_image_profiles;
} dt_iop_colorin_gui_data_t;

typedef struct dt_iop_colorin_global_data_t
{
  int kernel_colorin_unbound;
  int kernel_colorin_clipping;
} dt_iop_colorin_global_data_t;

typedef struct dt_iop_colorin_data_t
{
  int clear_input;
  cmsHPROFILE input;
  cmsHPROFILE nrgb;
  cmsHTRANSFORM *xform_cam_Lab;
  cmsHTRANSFORM *xform_cam_nrgb;
  cmsHTRANSFORM *xform_nrgb_Lab;
  float lut[3][LUT_SAMPLES];
  float cmatrix[9];
  float nmatrix[9];
  float lmatrix[9];
  float unbounded_coeffs[3][3]; // approximation for extrapolation of shaper curves
  int blue_mapping;
  int nonlinearlut;
  dt_colorspaces_color_profile_type_t type;
} dt_iop_colorin_data_t;


const char *name()
{
  return _("input color profile");
}

int groups()
{
  return dt_iop_get_group("input color profile", IOP_GROUP_COLOR);
}

int flags()
{
  return IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_ONE_INSTANCE;
}

int legacy_params(dt_iop_module_t *self, const void *const old_params, const int old_version,
                  void *new_params, const int new_version)
{
  if(old_version == 1 && new_version == 4)
  {
    typedef struct dt_iop_colorin_params_v1_t
    {
      char iccprofile[DT_IOP_COLOR_ICC_LEN];
      dt_iop_color_intent_t intent;
    } dt_iop_colorin_params_v1_t;

    const dt_iop_colorin_params_v1_t *old = (dt_iop_colorin_params_v1_t *)old_params;
    dt_iop_colorin_params_t *new = (dt_iop_colorin_params_t *)new_params;
    memset(new_params, 0, sizeof(*new_params));

    if(!strcmp(old->iccprofile, "eprofile"))
      new->type = DT_COLORSPACE_EMBEDDED_ICC;
    else if(!strcmp(old->iccprofile, "ematrix"))
      new->type = DT_COLORSPACE_EMBEDDED_MATRIX;
    else if(!strcmp(old->iccprofile, "cmatrix"))
      new->type = DT_COLORSPACE_STANDARD_MATRIX;
    else if(!strcmp(old->iccprofile, "darktable"))
      new->type = DT_COLORSPACE_ENHANCED_MATRIX;
    else if(!strcmp(old->iccprofile, "vendor"))
      new->type = DT_COLORSPACE_VENDOR_MATRIX;
    else if(!strcmp(old->iccprofile, "alternate"))
      new->type = DT_COLORSPACE_ALTERNATE_MATRIX;
    else if(!strcmp(old->iccprofile, "sRGB"))
      new->type = DT_COLORSPACE_SRGB;
    else if(!strcmp(old->iccprofile, "adobergb"))
      new->type = DT_COLORSPACE_ADOBERGB;
    else if(!strcmp(old->iccprofile, "linear_rec709_rgb") || !strcmp(old->iccprofile, "linear_rgb"))
      new->type = DT_COLORSPACE_LIN_REC709;
    else if(!strcmp(old->iccprofile, "linear_rec2020_rgb"))
      new->type = DT_COLORSPACE_LIN_REC2020;
    else if(!strcmp(old->iccprofile, "infrared"))
      new->type = DT_COLORSPACE_INFRARED;
    else if(!strcmp(old->iccprofile, "XYZ"))
      new->type = DT_COLORSPACE_XYZ;
    else if(!strcmp(old->iccprofile, "Lab"))
      new->type = DT_COLORSPACE_LAB;
    else
    {
      new->type = DT_COLORSPACE_FILE;
      g_strlcpy(new->filename, old->iccprofile, sizeof(new->filename));
    }

    new->intent = old->intent;
    new->normalize = 0;
    new->blue_mapping = 1;
    return 0;
  }
  if(old_version == 2 && new_version == 4)
  {
    typedef struct dt_iop_colorin_params_v2_t
    {
      char iccprofile[DT_IOP_COLOR_ICC_LEN];
      dt_iop_color_intent_t intent;
      int normalize;
    } dt_iop_colorin_params_v2_t;

    const dt_iop_colorin_params_v2_t *old = (dt_iop_colorin_params_v2_t *)old_params;
    dt_iop_colorin_params_t *new = (dt_iop_colorin_params_t *)new_params;
    memset(new_params, 0, sizeof(*new_params));

    if(!strcmp(old->iccprofile, "eprofile"))
      new->type = DT_COLORSPACE_EMBEDDED_ICC;
    else if(!strcmp(old->iccprofile, "ematrix"))
      new->type = DT_COLORSPACE_EMBEDDED_MATRIX;
    else if(!strcmp(old->iccprofile, "cmatrix"))
      new->type = DT_COLORSPACE_STANDARD_MATRIX;
    else if(!strcmp(old->iccprofile, "darktable"))
      new->type = DT_COLORSPACE_ENHANCED_MATRIX;
    else if(!strcmp(old->iccprofile, "vendor"))
      new->type = DT_COLORSPACE_VENDOR_MATRIX;
    else if(!strcmp(old->iccprofile, "alternate"))
      new->type = DT_COLORSPACE_ALTERNATE_MATRIX;
    else if(!strcmp(old->iccprofile, "sRGB"))
      new->type = DT_COLORSPACE_SRGB;
    else if(!strcmp(old->iccprofile, "adobergb"))
      new->type = DT_COLORSPACE_ADOBERGB;
    else if(!strcmp(old->iccprofile, "linear_rec709_rgb") || !strcmp(old->iccprofile, "linear_rgb"))
      new->type = DT_COLORSPACE_LIN_REC709;
    else if(!strcmp(old->iccprofile, "linear_rec2020_rgb"))
      new->type = DT_COLORSPACE_LIN_REC2020;
    else if(!strcmp(old->iccprofile, "infrared"))
      new->type = DT_COLORSPACE_INFRARED;
    else if(!strcmp(old->iccprofile, "XYZ"))
      new->type = DT_COLORSPACE_XYZ;
    else if(!strcmp(old->iccprofile, "Lab"))
      new->type = DT_COLORSPACE_LAB;
    else
    {
      new->type = DT_COLORSPACE_FILE;
      g_strlcpy(new->filename, old->iccprofile, sizeof(new->filename));
    }

    new->intent = old->intent;
    new->normalize = old->normalize;
    new->blue_mapping = 1;
    return 0;
  }
  if(old_version == 3 && new_version == 4)
  {
    typedef struct dt_iop_colorin_params_v3_t
    {
      char iccprofile[DT_IOP_COLOR_ICC_LEN];
      dt_iop_color_intent_t intent;
      int normalize;
      int blue_mapping;
    } dt_iop_colorin_params_v3_t;

    const dt_iop_colorin_params_v3_t *old = (dt_iop_colorin_params_v3_t *)old_params;
    dt_iop_colorin_params_t *new = (dt_iop_colorin_params_t *)new_params;
    memset(new_params, 0, sizeof(*new_params));

    if(!strcmp(old->iccprofile, "eprofile"))
      new->type = DT_COLORSPACE_EMBEDDED_ICC;
    else if(!strcmp(old->iccprofile, "ematrix"))
      new->type = DT_COLORSPACE_EMBEDDED_MATRIX;
    else if(!strcmp(old->iccprofile, "cmatrix"))
      new->type = DT_COLORSPACE_STANDARD_MATRIX;
    else if(!strcmp(old->iccprofile, "darktable"))
      new->type = DT_COLORSPACE_ENHANCED_MATRIX;
    else if(!strcmp(old->iccprofile, "vendor"))
      new->type = DT_COLORSPACE_VENDOR_MATRIX;
    else if(!strcmp(old->iccprofile, "alternate"))
      new->type = DT_COLORSPACE_ALTERNATE_MATRIX;
    else if(!strcmp(old->iccprofile, "sRGB"))
      new->type = DT_COLORSPACE_SRGB;
    else if(!strcmp(old->iccprofile, "adobergb"))
      new->type = DT_COLORSPACE_ADOBERGB;
    else if(!strcmp(old->iccprofile, "linear_rec709_rgb") || !strcmp(old->iccprofile, "linear_rgb"))
      new->type = DT_COLORSPACE_LIN_REC709;
    else if(!strcmp(old->iccprofile, "linear_rec2020_rgb"))
      new->type = DT_COLORSPACE_LIN_REC2020;
    else if(!strcmp(old->iccprofile, "infrared"))
      new->type = DT_COLORSPACE_INFRARED;
    else if(!strcmp(old->iccprofile, "XYZ"))
      new->type = DT_COLORSPACE_XYZ;
    else if(!strcmp(old->iccprofile, "Lab"))
      new->type = DT_COLORSPACE_LAB;
    else
    {
      new->type = DT_COLORSPACE_FILE;
      g_strlcpy(new->filename, old->iccprofile, sizeof(new->filename));
    }

    new->intent = old->intent;
    new->normalize = old->normalize;
    new->blue_mapping = old->blue_mapping;

    return 0;
  }
  return 1;
}


void init_global(dt_iop_module_so_t *module)
{
  const int program = 2; // basic.cl, from programs.conf
  dt_iop_colorin_global_data_t *gd
      = (dt_iop_colorin_global_data_t *)malloc(sizeof(dt_iop_colorin_global_data_t));
  module->data = gd;
  gd->kernel_colorin_unbound = dt_opencl_create_kernel(program, "colorin_unbound");
  gd->kernel_colorin_clipping = dt_opencl_create_kernel(program, "colorin_clipping");
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_colorin_global_data_t *gd = (dt_iop_colorin_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_colorin_unbound);
  dt_opencl_free_kernel(gd->kernel_colorin_clipping);
  free(module->data);
  module->data = NULL;
}

#if 0
static void intent_changed (GtkWidget *widget, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_colorin_params_t *p = (dt_iop_colorin_params_t *)self->params;
  p->intent = (dt_iop_color_intent_t)dt_bauhaus_combobox_get(widget);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}
#endif

static void profile_changed(GtkWidget *widget, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_request_focus(self);
  dt_iop_colorin_params_t *p = (dt_iop_colorin_params_t *)self->params;
  dt_iop_colorin_gui_data_t *g = (dt_iop_colorin_gui_data_t *)self->gui_data;
  int pos = dt_bauhaus_combobox_get(widget);
  GList *prof;
  if(pos < g->n_image_profiles)
    prof = g->image_profiles;
  else
  {
    prof = darktable.color_profiles->profiles;
    pos -= g->n_image_profiles;
  }
  while(prof)
  {
    dt_colorspaces_color_profile_t *pp = (dt_colorspaces_color_profile_t *)prof->data;
    if(pp->in_pos == pos)
    {
      p->type = pp->type;
      memcpy(p->filename, pp->filename, sizeof(p->filename));
      dt_dev_add_history_item(darktable.develop, self, TRUE);
      return;
    }
    prof = g_list_next(prof);
  }
  // should really never happen.
  fprintf(stderr, "[colorin] color profile %s seems to have disappeared!\n", dt_colorspaces_get_name(p->type, p->filename));
}


static void normalize_changed(GtkWidget *widget, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_colorin_params_t *p = (dt_iop_colorin_params_t *)self->params;
  p->normalize = dt_bauhaus_combobox_get(widget);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}


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

#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_colorin_data_t *d = (dt_iop_colorin_data_t *)piece->data;
  dt_iop_colorin_global_data_t *gd = (dt_iop_colorin_global_data_t *)self->data;
  cl_mem dev_m = NULL, dev_l = NULL, dev_r = NULL, dev_g = NULL, dev_b = NULL, dev_coeffs = NULL;

  int kernel;
  float *cmat, *lmat;

  if(d->nrgb)
  {
    kernel = gd->kernel_colorin_clipping;
    cmat = d->nmatrix;
    lmat = d->lmatrix;
  }
  else
  {
    kernel = gd->kernel_colorin_unbound;
    cmat = d->cmatrix;
    lmat = d->lmatrix;
  }

  cl_int err = -999;
  const int blue_mapping = d->blue_mapping && piece->pipe->image.flags & DT_IMAGE_RAW;
  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;

  if(d->type == DT_COLORSPACE_LAB)
  {
    size_t origin[] = { 0, 0, 0 };
    size_t region[] = { roi_in->width, roi_in->height, 1 };
    err = dt_opencl_enqueue_copy_image(devid, dev_in, dev_out, origin, origin, region);
    if(err != CL_SUCCESS) goto error;
    return TRUE;
  }

  size_t sizes[] = { ROUNDUPWD(width), ROUNDUPHT(height), 1 };
  dev_m = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * 9, cmat);
  if(dev_m == NULL) goto error;
  dev_l = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * 9, lmat);
  if(dev_l == NULL) goto error;
  dev_r = dt_opencl_copy_host_to_device(devid, d->lut[0], 256, 256, sizeof(float));
  if(dev_r == NULL) goto error;
  dev_g = dt_opencl_copy_host_to_device(devid, d->lut[1], 256, 256, sizeof(float));
  if(dev_g == NULL) goto error;
  dev_b = dt_opencl_copy_host_to_device(devid, d->lut[2], 256, 256, sizeof(float));
  if(dev_b == NULL) goto error;
  dev_coeffs
      = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * 3 * 3, (float *)d->unbounded_coeffs);
  if(dev_coeffs == NULL) goto error;
  dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(cl_mem), (void *)&dev_m);
  dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(cl_mem), (void *)&dev_l);
  dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(cl_mem), (void *)&dev_r);
  dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(cl_mem), (void *)&dev_g);
  dt_opencl_set_kernel_arg(devid, kernel, 8, sizeof(cl_mem), (void *)&dev_b);
  dt_opencl_set_kernel_arg(devid, kernel, 9, sizeof(cl_int), (void *)&blue_mapping);
  dt_opencl_set_kernel_arg(devid, kernel, 10, sizeof(cl_mem), (void *)&dev_coeffs);
  err = dt_opencl_enqueue_kernel_2d(devid, kernel, sizes);
  if(err != CL_SUCCESS) goto error;
  dt_opencl_release_mem_object(dev_m);
  dt_opencl_release_mem_object(dev_l);
  dt_opencl_release_mem_object(dev_r);
  dt_opencl_release_mem_object(dev_g);
  dt_opencl_release_mem_object(dev_b);
  dt_opencl_release_mem_object(dev_coeffs);
  return TRUE;

error:
  dt_opencl_release_mem_object(dev_m);
  dt_opencl_release_mem_object(dev_l);
  dt_opencl_release_mem_object(dev_r);
  dt_opencl_release_mem_object(dev_g);
  dt_opencl_release_mem_object(dev_b);
  dt_opencl_release_mem_object(dev_coeffs);
  dt_print(DT_DEBUG_OPENCL, "[opencl_colorin] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif

static inline void apply_blue_mapping(const float *const in, float *const out)
{
  out[0] = in[0];
  out[1] = in[1];
  out[2] = in[2];

  const float YY = out[0] + out[1] + out[2];
  if(YY > 0.0f)
  {
    const float zz = out[2] / YY;
    const float bound_z = 0.5f, bound_Y = 0.5f;
    const float amount = 0.11f;
    if(zz > bound_z)
    {
      const float t = (zz - bound_z) / (1.0f - bound_z) * fminf(1.0, YY / bound_Y);
      out[1] += t * amount;
      out[2] -= t * amount;
    }
  }
}

static void process_cmatrix_bm(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece,
                               const void *const ivoid, void *const ovoid, const dt_iop_roi_t *const roi_in,
                               const dt_iop_roi_t *const roi_out)
{
  const dt_iop_colorin_data_t *const d = (dt_iop_colorin_data_t *)piece->data;
  const int ch = piece->colors;
  const int clipping = (d->nrgb != NULL);

    // fprintf(stderr, "Using cmatrix codepath\n");
    // only color matrix. use our optimized fast path!
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static)
#endif
  for(int j = 0; j < roi_out->height; j++)
  {
    const float *in = (const float *)ivoid + (size_t)ch * j * roi_out->width;
    float *out = (float *)ovoid + (size_t)ch * j * roi_out->width;
    float cam[3];

    for(int i = 0; i < roi_out->width; i++, in += ch, out += ch)
    {
      // memcpy(cam, buf_in, sizeof(float)*3);
      // avoid calling this for linear profiles (marked with negative entries), assures unbounded
      // color management without extrapolation.
      for(int c = 0; c < 3; c++)
        cam[c] = (d->lut[c][0] >= 0.0f) ? ((in[c] < 1.0f) ? lerp_lut(d->lut[c], in[c])
                                                          : dt_iop_eval_exp(d->unbounded_coeffs[c], in[c]))
                                        : in[c];

      apply_blue_mapping(cam, cam);

      if(!clipping)
      {
        float _xyz[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

        for(int c = 0; c < 3; c++)
        {
          _xyz[c] = 0.0f;
          for(int k = 0; k < 3; k++)
          {
            _xyz[c] += d->cmatrix[3 * c + k] * cam[k];
          }
        }

        dt_XYZ_to_Lab(_xyz, out);
      }
      else
      {
        float nRGB[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        for(int c = 0; c < 3; c++)
        {
          nRGB[c] = 0.0f;
          for(int k = 0; k < 3; k++)
          {
            nRGB[c] += d->nmatrix[3 * c + k] * cam[k];
          }
        }

        float cRGB[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        for(int c = 0; c < 3; c++)
        {
          cRGB[c] = CLAMP(nRGB[c], 0.0f, 1.0f);
        }

        float XYZ[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        for(int c = 0; c < 3; c++)
        {
          XYZ[c] = 0.0f;
          for(int k = 0; k < 3; k++)
          {
            XYZ[c] += d->lmatrix[3 * c + k] * cRGB[k];
          }
        }

        dt_XYZ_to_Lab(XYZ, out);
      }
    }
  }
}

static void process_cmatrix_fastpath_simple(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece,
                                            const void *const ivoid, void *const ovoid,
                                            const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_colorin_data_t *const d = (dt_iop_colorin_data_t *)piece->data;
  const int ch = piece->colors;

// fprintf(stderr, "Using cmatrix codepath\n");
// only color matrix. use our optimized fast path!
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static)
#endif
  for(int k = 0; k < (size_t)roi_out->width * roi_out->height; k++)
  {
    float *in = (float *)ivoid + (size_t)ch * k;
    float *out = (float *)ovoid + (size_t)ch * k;

    float _xyz[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

    for(int c = 0; c < 3; c++)
    {
      _xyz[c] = 0.0f;
      for(int i = 0; i < 3; i++)
      {
        _xyz[c] += d->cmatrix[3 * c + i] * in[i];
      }
    }

    dt_XYZ_to_Lab(_xyz, out);
  }
}

static void process_cmatrix_fastpath_clipping(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece,
                                              const void *const ivoid, void *const ovoid,
                                              const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_colorin_data_t *const d = (dt_iop_colorin_data_t *)piece->data;
  const int ch = piece->colors;

// fprintf(stderr, "Using cmatrix codepath\n");
// only color matrix. use our optimized fast path!
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static)
#endif
  for(int k = 0; k < (size_t)roi_out->width * roi_out->height; k++)
  {
    float *in = (float *)ivoid + (size_t)ch * k;
    float *out = (float *)ovoid + (size_t)ch * k;

    float nRGB[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    for(int c = 0; c < 3; c++)
    {
      nRGB[c] = 0.0f;
      for(int i = 0; i < 3; i++)
      {
        nRGB[c] += d->nmatrix[3 * c + i] * in[i];
      }
    }

    float cRGB[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    for(int c = 0; c < 3; c++)
    {
      cRGB[c] = CLAMP(nRGB[c], 0.0f, 1.0f);
    }

    float XYZ[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    for(int c = 0; c < 3; c++)
    {
      XYZ[c] = 0.0f;
      for(int i = 0; i < 3; i++)
      {
        XYZ[c] += d->lmatrix[3 * c + i] * cRGB[i];
      }
    }

    dt_XYZ_to_Lab(XYZ, out);
  }
}

static void process_cmatrix_fastpath(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece,
                                     const void *const ivoid, void *const ovoid, const dt_iop_roi_t *const roi_in,
                                     const dt_iop_roi_t *const roi_out)
{
  const dt_iop_colorin_data_t *const d = (dt_iop_colorin_data_t *)piece->data;
  const int clipping = (d->nrgb != NULL);

  if(!clipping)
  {
    process_cmatrix_fastpath_simple(self, piece, ivoid, ovoid, roi_in, roi_out);
  }
  else
  {
    process_cmatrix_fastpath_clipping(self, piece, ivoid, ovoid, roi_in, roi_out);
  }
}

static void process_cmatrix_proper(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece,
                                   const void *const ivoid, void *const ovoid, const dt_iop_roi_t *const roi_in,
                                   const dt_iop_roi_t *const roi_out)
{
  const dt_iop_colorin_data_t *const d = (dt_iop_colorin_data_t *)piece->data;
  const int ch = piece->colors;
  const int clipping = (d->nrgb != NULL);

// fprintf(stderr, "Using cmatrix codepath\n");
// only color matrix. use our optimized fast path!
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static)
#endif
  for(int j = 0; j < roi_out->height; j++)
  {
    const float *in = (const float *)ivoid + (size_t)ch * j * roi_out->width;
    float *out = (float *)ovoid + (size_t)ch * j * roi_out->width;
    float cam[3];

    for(int i = 0; i < roi_out->width; i++, in += ch, out += ch)
    {
      // memcpy(cam, buf_in, sizeof(float)*3);
      // avoid calling this for linear profiles (marked with negative entries), assures unbounded
      // color management without extrapolation.
      for(int c = 0; c < 3; c++)
        cam[c] = (d->lut[c][0] >= 0.0f) ? ((in[c] < 1.0f) ? lerp_lut(d->lut[c], in[c])
                                                          : dt_iop_eval_exp(d->unbounded_coeffs[c], in[c]))
                                        : in[c];

      if(!clipping)
      {
        float _xyz[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

        for(int c = 0; c < 3; c++)
        {
          _xyz[c] = 0.0f;
          for(int k = 0; k < 3; k++)
          {
            _xyz[c] += d->cmatrix[3 * c + k] * cam[k];
          }
        }

        dt_XYZ_to_Lab(_xyz, out);
      }
      else
      {
        float nRGB[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        for(int c = 0; c < 3; c++)
        {
          nRGB[c] = 0.0f;
          for(int k = 0; k < 3; k++)
          {
            nRGB[c] += d->nmatrix[3 * c + k] * cam[k];
          }
        }

        float cRGB[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        for(int c = 0; c < 3; c++)
        {
          cRGB[c] = CLAMP(nRGB[c], 0.0f, 1.0f);
        }

        float XYZ[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        for(int c = 0; c < 3; c++)
        {
          XYZ[c] = 0.0f;
          for(int k = 0; k < 3; k++)
          {
            XYZ[c] += d->lmatrix[3 * c + k] * cRGB[k];
          }
        }

        dt_XYZ_to_Lab(XYZ, out);
      }
    }
  }
}

static void process_cmatrix(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
                            void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_colorin_data_t *const d = (dt_iop_colorin_data_t *)piece->data;
  const int blue_mapping = d->blue_mapping && piece->pipe->image.flags & DT_IMAGE_RAW;

  if(!blue_mapping && d->nonlinearlut == 0)
  {
    process_cmatrix_fastpath(self, piece, ivoid, ovoid, roi_in, roi_out);
  }
  else if(blue_mapping)
  {
    process_cmatrix_bm(self, piece, ivoid, ovoid, roi_in, roi_out);
  }
  else
  {
    process_cmatrix_proper(self, piece, ivoid, ovoid, roi_in, roi_out);
  }
}

static void process_lcms2_bm(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
                             void *const ovoid, const dt_iop_roi_t *const roi_in,
                             const dt_iop_roi_t *const roi_out)
{
  const dt_iop_colorin_data_t *const d = (dt_iop_colorin_data_t *)piece->data;
  const int ch = piece->colors;

// use general lcms2 fallback
#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none)
#endif
  for(int k = 0; k < roi_out->height; k++)
  {
    const float *in = (const float *)ivoid + (size_t)ch * k * roi_out->width;
    float *out = (float *)ovoid + (size_t)ch * k * roi_out->width;

    float *camptr = (float *)out;
    for(int j = 0; j < roi_out->width; j++, in += 4, camptr += 4)
    {
      apply_blue_mapping(in, camptr);
    }

    // convert to (L,a/L,b/L) to be able to change L without changing saturation.
    if(!d->nrgb)
    {
      cmsDoTransform(d->xform_cam_Lab, out, out, roi_out->width);
    }
    else
    {
      cmsDoTransform(d->xform_cam_nrgb, out, out, roi_out->width);

      float *rgbptr = (float *)out;
      for(int j = 0; j < roi_out->width; j++, rgbptr += 4)
      {
        for(int c = 0; c < 3; c++)
        {
          rgbptr[c] = CLAMP(rgbptr[c], 0.0f, 1.0f);
        }
      }

      cmsDoTransform(d->xform_nrgb_Lab, out, out, roi_out->width);
    }
  }
}

static void process_lcms2_proper(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece,
                                 const void *const ivoid, void *const ovoid, const dt_iop_roi_t *const roi_in,
                                 const dt_iop_roi_t *const roi_out)
{
  const dt_iop_colorin_data_t *const d = (dt_iop_colorin_data_t *)piece->data;
  const int ch = piece->colors;

// use general lcms2 fallback
#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none)
#endif
  for(int k = 0; k < roi_out->height; k++)
  {
    const float *in = (const float *)ivoid + (size_t)ch * k * roi_out->width;
    float *out = (float *)ovoid + (size_t)ch * k * roi_out->width;

    // convert to (L,a/L,b/L) to be able to change L without changing saturation.
    if(!d->nrgb)
    {
      cmsDoTransform(d->xform_cam_Lab, in, out, roi_out->width);
    }
    else
    {
      cmsDoTransform(d->xform_cam_nrgb, in, out, roi_out->width);

      float *rgbptr = (float *)out;
      for(int j = 0; j < roi_out->width; j++, rgbptr += 4)
      {
        for(int c = 0; c < 3; c++)
        {
          rgbptr[c] = CLAMP(rgbptr[c], 0.0f, 1.0f);
        }
      }

      cmsDoTransform(d->xform_nrgb_Lab, out, out, roi_out->width);
    }
  }
}

static void process_lcms2(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
                          void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_colorin_data_t *const d = (dt_iop_colorin_data_t *)piece->data;
  const int blue_mapping = d->blue_mapping && piece->pipe->image.flags & DT_IMAGE_RAW;

  // use general lcms2 fallback
  if(blue_mapping)
  {
    process_lcms2_bm(self, piece, ivoid, ovoid, roi_in, roi_out);
  }
  else
  {
    process_lcms2_proper(self, piece, ivoid, ovoid, roi_in, roi_out);
  }
}

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_colorin_data_t *const d = (dt_iop_colorin_data_t *)piece->data;

  if(d->type == DT_COLORSPACE_LAB)
  {
    memcpy(ovoid, ivoid, sizeof(float) * 4 * roi_out->width * roi_out->height);
  }
  else if(!isnan(d->cmatrix[0]))
  {
    process_cmatrix(self, piece, ivoid, ovoid, roi_in, roi_out);
  }
  else
  {
    process_lcms2(self, piece, ivoid, ovoid, roi_in, roi_out);
  }

  if(piece->pipe->mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK) dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);
}

#if defined(__SSE2__)
static void process_sse2_cmatrix_bm(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece,
                                    const void *const ivoid, void *const ovoid, const dt_iop_roi_t *const roi_in,
                                    const dt_iop_roi_t *const roi_out)
{
  const dt_iop_colorin_data_t *const d = (dt_iop_colorin_data_t *)piece->data;
  const int ch = piece->colors;
  const int clipping = (d->nrgb != NULL);

  // only color matrix. use our optimized fast path!
  const float *const cmat = d->cmatrix;
  const float *const nmat = d->nmatrix;
  const float *const lmat = d->lmatrix;
  float *in = (float *)ivoid;
  float *out = (float *)ovoid;
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(out, in) schedule(static)
#endif
  for(int j = 0; j < roi_out->height; j++)
  {

    float *buf_in = in + (size_t)ch * roi_in->width * j;
    float *buf_out = out + (size_t)ch * roi_out->width * j;
    float cam[3];
    const __m128 cm0 = _mm_set_ps(0.0f, cmat[6], cmat[3], cmat[0]);
    const __m128 cm1 = _mm_set_ps(0.0f, cmat[7], cmat[4], cmat[1]);
    const __m128 cm2 = _mm_set_ps(0.0f, cmat[8], cmat[5], cmat[2]);

    const __m128 nm0 = _mm_set_ps(0.0f, nmat[6], nmat[3], nmat[0]);
    const __m128 nm1 = _mm_set_ps(0.0f, nmat[7], nmat[4], nmat[1]);
    const __m128 nm2 = _mm_set_ps(0.0f, nmat[8], nmat[5], nmat[2]);

    const __m128 lm0 = _mm_set_ps(0.0f, lmat[6], lmat[3], lmat[0]);
    const __m128 lm1 = _mm_set_ps(0.0f, lmat[7], lmat[4], lmat[1]);
    const __m128 lm2 = _mm_set_ps(0.0f, lmat[8], lmat[5], lmat[2]);

    for(int i = 0; i < roi_out->width; i++, buf_in += ch, buf_out += ch)
    {

      // memcpy(cam, buf_in, sizeof(float)*3);
      // avoid calling this for linear profiles (marked with negative entries), assures unbounded
      // color management without extrapolation.
      for(int c = 0; c < 3; c++)
        cam[c] = (d->lut[c][0] >= 0.0f) ? ((buf_in[c] < 1.0f) ? lerp_lut(d->lut[c], buf_in[c])
                                                              : dt_iop_eval_exp(d->unbounded_coeffs[c], buf_in[c]))
                                        : buf_in[c];

      apply_blue_mapping(cam, cam);

      if(!clipping)
      {
        __m128 xyz
            = _mm_add_ps(_mm_add_ps(_mm_mul_ps(cm0, _mm_set1_ps(cam[0])), _mm_mul_ps(cm1, _mm_set1_ps(cam[1]))),
                         _mm_mul_ps(cm2, _mm_set1_ps(cam[2])));
        _mm_stream_ps(buf_out, dt_XYZ_to_Lab_sse2(xyz));
      }
      else
      {
        __m128 nrgb
            = _mm_add_ps(_mm_add_ps(_mm_mul_ps(nm0, _mm_set1_ps(cam[0])), _mm_mul_ps(nm1, _mm_set1_ps(cam[1]))),
                         _mm_mul_ps(nm2, _mm_set1_ps(cam[2])));
        __m128 crgb = _mm_min_ps(_mm_max_ps(nrgb, _mm_set1_ps(0.0f)), _mm_set1_ps(1.0f));
        __m128 xyz = _mm_add_ps(_mm_add_ps(_mm_mul_ps(lm0, _mm_shuffle_ps(crgb, crgb, _MM_SHUFFLE(0, 0, 0, 0))),
                                           _mm_mul_ps(lm1, _mm_shuffle_ps(crgb, crgb, _MM_SHUFFLE(1, 1, 1, 1)))),
                                _mm_mul_ps(lm2, _mm_shuffle_ps(crgb, crgb, _MM_SHUFFLE(2, 2, 2, 2))));
        _mm_stream_ps(buf_out, dt_XYZ_to_Lab_sse2(xyz));
      }
    }
  }
  _mm_sfence();
}

static void process_sse2_cmatrix_fastpath_simple(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece,
                                                 const void *const ivoid, void *const ovoid,
                                                 const dt_iop_roi_t *const roi_in,
                                                 const dt_iop_roi_t *const roi_out)
{
  const dt_iop_colorin_data_t *const d = (dt_iop_colorin_data_t *)piece->data;
  const int ch = piece->colors;

  // only color matrix. use our optimized fast path!
  const float *const cmat = d->cmatrix;

  const __m128 cm0 = _mm_set_ps(0.0f, cmat[6], cmat[3], cmat[0]);
  const __m128 cm1 = _mm_set_ps(0.0f, cmat[7], cmat[4], cmat[1]);
  const __m128 cm2 = _mm_set_ps(0.0f, cmat[8], cmat[5], cmat[2]);

#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static)
#endif
  for(int k = 0; k < (size_t)roi_out->width * roi_out->height; k++)
  {
    float *in = (float *)ivoid + (size_t)ch * k;
    float *out = (float *)ovoid + (size_t)ch * k;

    __m128 input = _mm_load_ps(in);

    __m128 xyz = _mm_add_ps(_mm_add_ps(_mm_mul_ps(cm0, _mm_shuffle_ps(input, input, _MM_SHUFFLE(0, 0, 0, 0))),
                                       _mm_mul_ps(cm1, _mm_shuffle_ps(input, input, _MM_SHUFFLE(1, 1, 1, 1)))),
                            _mm_mul_ps(cm2, _mm_shuffle_ps(input, input, _MM_SHUFFLE(2, 2, 2, 2))));
    _mm_stream_ps(out, dt_XYZ_to_Lab_sse2(xyz));
  }
  _mm_sfence();
}

static void process_sse2_cmatrix_fastpath_clipping(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece,
                                                   const void *const ivoid, void *const ovoid,
                                                   const dt_iop_roi_t *const roi_in,
                                                   const dt_iop_roi_t *const roi_out)
{
  const dt_iop_colorin_data_t *const d = (dt_iop_colorin_data_t *)piece->data;
  const int ch = piece->colors;

  // only color matrix. use our optimized fast path!
  const float *const nmat = d->nmatrix;
  const float *const lmat = d->lmatrix;

  const __m128 nm0 = _mm_set_ps(0.0f, nmat[6], nmat[3], nmat[0]);
  const __m128 nm1 = _mm_set_ps(0.0f, nmat[7], nmat[4], nmat[1]);
  const __m128 nm2 = _mm_set_ps(0.0f, nmat[8], nmat[5], nmat[2]);

  const __m128 lm0 = _mm_set_ps(0.0f, lmat[6], lmat[3], lmat[0]);
  const __m128 lm1 = _mm_set_ps(0.0f, lmat[7], lmat[4], lmat[1]);
  const __m128 lm2 = _mm_set_ps(0.0f, lmat[8], lmat[5], lmat[2]);

#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static)
#endif
  for(int k = 0; k < (size_t)roi_out->width * roi_out->height; k++)
  {
    float *in = (float *)ivoid + (size_t)ch * k;
    float *out = (float *)ovoid + (size_t)ch * k;

    __m128 input = _mm_load_ps(in);

    __m128 nrgb = _mm_add_ps(_mm_add_ps(_mm_mul_ps(nm0, _mm_shuffle_ps(input, input, _MM_SHUFFLE(0, 0, 0, 0))),
                                        _mm_mul_ps(nm1, _mm_shuffle_ps(input, input, _MM_SHUFFLE(1, 1, 1, 1)))),
                             _mm_mul_ps(nm2, _mm_shuffle_ps(input, input, _MM_SHUFFLE(2, 2, 2, 2))));
    __m128 crgb = _mm_min_ps(_mm_max_ps(nrgb, _mm_set1_ps(0.0f)), _mm_set1_ps(1.0f));
    __m128 xyz = _mm_add_ps(_mm_add_ps(_mm_mul_ps(lm0, _mm_shuffle_ps(crgb, crgb, _MM_SHUFFLE(0, 0, 0, 0))),
                                       _mm_mul_ps(lm1, _mm_shuffle_ps(crgb, crgb, _MM_SHUFFLE(1, 1, 1, 1)))),
                            _mm_mul_ps(lm2, _mm_shuffle_ps(crgb, crgb, _MM_SHUFFLE(2, 2, 2, 2))));
    _mm_stream_ps(out, dt_XYZ_to_Lab_sse2(xyz));
  }
  _mm_sfence();
}

static void process_sse2_cmatrix_fastpath(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece,
                                          const void *const ivoid, void *const ovoid,
                                          const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_colorin_data_t *const d = (dt_iop_colorin_data_t *)piece->data;
  const int clipping = (d->nrgb != NULL);

  if(!clipping)
  {
    process_sse2_cmatrix_fastpath_simple(self, piece, ivoid, ovoid, roi_in, roi_out);
  }
  else
  {
    process_sse2_cmatrix_fastpath_clipping(self, piece, ivoid, ovoid, roi_in, roi_out);
  }
}

static void process_sse2_cmatrix_proper(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece,
                                        const void *const ivoid, void *const ovoid,
                                        const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_colorin_data_t *const d = (dt_iop_colorin_data_t *)piece->data;
  const int ch = piece->colors;
  const int clipping = (d->nrgb != NULL);

  // only color matrix. use our optimized fast path!
  const float *const cmat = d->cmatrix;
  const float *const nmat = d->nmatrix;
  const float *const lmat = d->lmatrix;
  float *in = (float *)ivoid;
  float *out = (float *)ovoid;
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(out, in) schedule(static)
#endif
  for(int j = 0; j < roi_out->height; j++)
  {

    float *buf_in = in + (size_t)ch * roi_in->width * j;
    float *buf_out = out + (size_t)ch * roi_out->width * j;
    float cam[3];
    const __m128 cm0 = _mm_set_ps(0.0f, cmat[6], cmat[3], cmat[0]);
    const __m128 cm1 = _mm_set_ps(0.0f, cmat[7], cmat[4], cmat[1]);
    const __m128 cm2 = _mm_set_ps(0.0f, cmat[8], cmat[5], cmat[2]);

    const __m128 nm0 = _mm_set_ps(0.0f, nmat[6], nmat[3], nmat[0]);
    const __m128 nm1 = _mm_set_ps(0.0f, nmat[7], nmat[4], nmat[1]);
    const __m128 nm2 = _mm_set_ps(0.0f, nmat[8], nmat[5], nmat[2]);

    const __m128 lm0 = _mm_set_ps(0.0f, lmat[6], lmat[3], lmat[0]);
    const __m128 lm1 = _mm_set_ps(0.0f, lmat[7], lmat[4], lmat[1]);
    const __m128 lm2 = _mm_set_ps(0.0f, lmat[8], lmat[5], lmat[2]);

    for(int i = 0; i < roi_out->width; i++, buf_in += ch, buf_out += ch)
    {

      // memcpy(cam, buf_in, sizeof(float)*3);
      // avoid calling this for linear profiles (marked with negative entries), assures unbounded
      // color management without extrapolation.
      for(int c = 0; c < 3; c++)
        cam[c] = (d->lut[c][0] >= 0.0f) ? ((buf_in[c] < 1.0f) ? lerp_lut(d->lut[c], buf_in[c])
                                                              : dt_iop_eval_exp(d->unbounded_coeffs[c], buf_in[c]))
                                        : buf_in[c];

      if(!clipping)
      {
        __m128 xyz
            = _mm_add_ps(_mm_add_ps(_mm_mul_ps(cm0, _mm_set1_ps(cam[0])), _mm_mul_ps(cm1, _mm_set1_ps(cam[1]))),
                         _mm_mul_ps(cm2, _mm_set1_ps(cam[2])));
        _mm_stream_ps(buf_out, dt_XYZ_to_Lab_sse2(xyz));
      }
      else
      {
        __m128 nrgb
            = _mm_add_ps(_mm_add_ps(_mm_mul_ps(nm0, _mm_set1_ps(cam[0])), _mm_mul_ps(nm1, _mm_set1_ps(cam[1]))),
                         _mm_mul_ps(nm2, _mm_set1_ps(cam[2])));
        __m128 crgb = _mm_min_ps(_mm_max_ps(nrgb, _mm_set1_ps(0.0f)), _mm_set1_ps(1.0f));
        __m128 xyz = _mm_add_ps(_mm_add_ps(_mm_mul_ps(lm0, _mm_shuffle_ps(crgb, crgb, _MM_SHUFFLE(0, 0, 0, 0))),
                                           _mm_mul_ps(lm1, _mm_shuffle_ps(crgb, crgb, _MM_SHUFFLE(1, 1, 1, 1)))),
                                _mm_mul_ps(lm2, _mm_shuffle_ps(crgb, crgb, _MM_SHUFFLE(2, 2, 2, 2))));
        _mm_stream_ps(buf_out, dt_XYZ_to_Lab_sse2(xyz));
      }
    }
  }
  _mm_sfence();
}

static void process_sse2_cmatrix(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece,
                                 const void *const ivoid, void *const ovoid, const dt_iop_roi_t *const roi_in,
                                 const dt_iop_roi_t *const roi_out)
{
  const dt_iop_colorin_data_t *const d = (dt_iop_colorin_data_t *)piece->data;
  const int blue_mapping = d->blue_mapping && piece->pipe->image.flags & DT_IMAGE_RAW;

  if(!blue_mapping && d->nonlinearlut == 0)
  {
    process_sse2_cmatrix_fastpath(self, piece, ivoid, ovoid, roi_in, roi_out);
  }
  else if(blue_mapping)
  {
    process_sse2_cmatrix_bm(self, piece, ivoid, ovoid, roi_in, roi_out);
  }
  else
  {
    process_sse2_cmatrix_proper(self, piece, ivoid, ovoid, roi_in, roi_out);
  }
}

static void process_sse2_lcms2_bm(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece,
                                  const void *const ivoid, void *const ovoid, const dt_iop_roi_t *const roi_in,
                                  const dt_iop_roi_t *const roi_out)
{
  const dt_iop_colorin_data_t *const d = (dt_iop_colorin_data_t *)piece->data;
  const int ch = piece->colors;

// use general lcms2 fallback
#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none)
#endif
  for(int k = 0; k < roi_out->height; k++)
  {
    const float *in = ((float *)ivoid) + (size_t)ch * k * roi_out->width;
    float *out = ((float *)ovoid) + (size_t)ch * k * roi_out->width;

    float *camptr = (float *)out;
    for(int j = 0; j < roi_out->width; j++, in += 4, camptr += 4)
    {
      apply_blue_mapping(in, camptr);
    }

    // convert to (L,a/L,b/L) to be able to change L without changing saturation.
    if(!d->nrgb)
    {
      cmsDoTransform(d->xform_cam_Lab, out, out, roi_out->width);
    }
    else
    {
      cmsDoTransform(d->xform_cam_nrgb, out, out, roi_out->width);

      float *rgbptr = (float *)out;
      for(int j = 0; j < roi_out->width; j++, rgbptr += 4)
      {
        const __m128 min = _mm_setzero_ps();
        const __m128 max = _mm_set1_ps(1.0f);
        const __m128 val = _mm_load_ps(rgbptr);
        const __m128 result = _mm_max_ps(_mm_min_ps(val, max), min);
        _mm_store_ps(rgbptr, result);
      }
      _mm_sfence();

      cmsDoTransform(d->xform_nrgb_Lab, out, out, roi_out->width);
    }
  }
}


static void process_sse2_lcms2_proper(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece,
                                      const void *const ivoid, void *const ovoid, const dt_iop_roi_t *const roi_in,
                                      const dt_iop_roi_t *const roi_out)
{
  const dt_iop_colorin_data_t *const d = (dt_iop_colorin_data_t *)piece->data;
  const int ch = piece->colors;

// use general lcms2 fallback
#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none)
#endif
  for(int k = 0; k < roi_out->height; k++)
  {
    const float *in = ((float *)ivoid) + (size_t)ch * k * roi_out->width;
    float *out = ((float *)ovoid) + (size_t)ch * k * roi_out->width;

    // convert to (L,a/L,b/L) to be able to change L without changing saturation.
    if(!d->nrgb)
    {
      cmsDoTransform(d->xform_cam_Lab, in, out, roi_out->width);
    }
    else
    {
      cmsDoTransform(d->xform_cam_nrgb, in, out, roi_out->width);

      float *rgbptr = (float *)out;
      for(int j = 0; j < roi_out->width; j++, rgbptr += 4)
      {
        const __m128 min = _mm_setzero_ps();
        const __m128 max = _mm_set1_ps(1.0f);
        const __m128 val = _mm_load_ps(rgbptr);
        const __m128 result = _mm_max_ps(_mm_min_ps(val, max), min);
        _mm_store_ps(rgbptr, result);
      }
      _mm_sfence();

      cmsDoTransform(d->xform_nrgb_Lab, out, out, roi_out->width);
    }
  }
}

static void process_sse2_lcms2(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece,
                               const void *const ivoid, void *const ovoid, const dt_iop_roi_t *const roi_in,
                               const dt_iop_roi_t *const roi_out)
{
  const dt_iop_colorin_data_t *const d = (dt_iop_colorin_data_t *)piece->data;
  const int blue_mapping = d->blue_mapping && piece->pipe->image.flags & DT_IMAGE_RAW;

  // use general lcms2 fallback
  if(blue_mapping)
  {
    process_sse2_lcms2_bm(self, piece, ivoid, ovoid, roi_in, roi_out);
  }
  else
  {
    process_sse2_lcms2_proper(self, piece, ivoid, ovoid, roi_in, roi_out);
  }
}

void process_sse2(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
                  void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_colorin_data_t *const d = (dt_iop_colorin_data_t *)piece->data;

  if(d->type == DT_COLORSPACE_LAB)
  {
    memcpy(ovoid, ivoid, sizeof(float) * 4 * roi_out->width * roi_out->height);
  }
  else if(!isnan(d->cmatrix[0]))
  {
    process_sse2_cmatrix(self, piece, ivoid, ovoid, roi_in, roi_out);
  }
  else
  {
    process_sse2_lcms2(self, piece, ivoid, ovoid, roi_in, roi_out);
  }

  if(piece->pipe->mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK) dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);
}
#endif

static void mat3mul(float *dst, const float *const m1, const float *const m2)
{
  for(int k = 0; k < 3; k++)
  {
    for(int i = 0; i < 3; i++)
    {
      float x = 0.0f;
      for(int j = 0; j < 3; j++) x += m1[3 * k + j] * m2[3 * j + i];
      dst[3 * k + i] = x;
    }
  }
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  const dt_iop_colorin_params_t *p = (dt_iop_colorin_params_t *)p1;
  dt_iop_colorin_data_t *d = (dt_iop_colorin_data_t *)piece->data;

  d->type = p->type;
  const cmsHPROFILE Lab = dt_colorspaces_get_profile(DT_COLORSPACE_LAB, "", DT_PROFILE_DIRECTION_ANY)->profile;

  // only clean up when it's a type that we created here
  if(d->input && d->clear_input) dt_colorspaces_cleanup_profile(d->input);
  d->input = NULL;
  d->clear_input = 0;
  d->nrgb = NULL;

  d->blue_mapping = p->blue_mapping;

  switch(p->normalize)
  {
    case DT_NORMALIZE_SRGB:
      d->nrgb = dt_colorspaces_get_profile(DT_COLORSPACE_SRGB, "", DT_PROFILE_DIRECTION_IN)->profile;
      break;
    case DT_NORMALIZE_ADOBE_RGB:
      d->nrgb = dt_colorspaces_get_profile(DT_COLORSPACE_ADOBERGB, "", DT_PROFILE_DIRECTION_IN)->profile;
      break;
    case DT_NORMALIZE_LINEAR_REC709_RGB:
      d->nrgb = dt_colorspaces_get_profile(DT_COLORSPACE_LIN_REC709, "", DT_PROFILE_DIRECTION_IN)->profile;
      break;
    case DT_NORMALIZE_LINEAR_REC2020_RGB:
      d->nrgb = dt_colorspaces_get_profile(DT_COLORSPACE_LIN_REC2020, "", DT_PROFILE_DIRECTION_IN)->profile;
      break;
    case DT_NORMALIZE_OFF:
    default:
      d->nrgb = NULL;
  }

  if(d->xform_cam_Lab)
  {
    cmsDeleteTransform(d->xform_cam_Lab);
    d->xform_cam_Lab = NULL;
  }
  if(d->xform_cam_nrgb)
  {
    cmsDeleteTransform(d->xform_cam_nrgb);
    d->xform_cam_nrgb = NULL;
  }
  if(d->xform_nrgb_Lab)
  {
    cmsDeleteTransform(d->xform_nrgb_Lab);
    d->xform_nrgb_Lab = NULL;
  }

  d->cmatrix[0] = d->nmatrix[0] = d->lmatrix[0] = NAN;
  d->lut[0][0] = -1.0f;
  d->lut[1][0] = -1.0f;
  d->lut[2][0] = -1.0f;
  d->nonlinearlut = 0;
  piece->process_cl_ready = 1;
  char datadir[PATH_MAX] = { 0 };
  dt_loc_get_datadir(datadir, sizeof(datadir));

  dt_colorspaces_color_profile_type_t type = p->type;
  if(type == DT_COLORSPACE_LAB)
  {
    piece->enabled = 0;
    return;
  }
  piece->enabled = 1;

  if(type == DT_COLORSPACE_ENHANCED_MATRIX)
  {
    d->input = dt_colorspaces_create_darktable_profile(pipe->image.camera_makermodel);
    if(!d->input) type = DT_COLORSPACE_EMBEDDED_ICC;
    else d->clear_input = 1;
  }
  if(type == DT_COLORSPACE_VENDOR_MATRIX)
  {
    d->input = dt_colorspaces_create_vendor_profile(pipe->image.camera_makermodel);
    if(!d->input) type = DT_COLORSPACE_EMBEDDED_ICC;
    else d->clear_input = 1;
  }
  if(type == DT_COLORSPACE_ALTERNATE_MATRIX)
  {
    d->input = dt_colorspaces_create_alternate_profile(pipe->image.camera_makermodel);
    if(!d->input) type = DT_COLORSPACE_EMBEDDED_ICC;
    else d->clear_input = 1;
  }
  if(type == DT_COLORSPACE_EMBEDDED_ICC)
  {
    // embedded color profile
    const dt_image_t *cimg = dt_image_cache_get(darktable.image_cache, pipe->image.id, 'r');
    if(cimg == NULL || cimg->profile == NULL)
      type = DT_COLORSPACE_EMBEDDED_MATRIX;
    else
    {
      d->input = dt_colorspaces_get_rgb_profile_from_mem(cimg->profile, cimg->profile_size);
      d->clear_input = 1;
    }
    dt_image_cache_read_release(darktable.image_cache, cimg);
  }
  if(type == DT_COLORSPACE_EMBEDDED_MATRIX)
  {
    // embedded matrix, hopefully D65
    if(isnan(pipe->image.d65_color_matrix[0]))
      type = DT_COLORSPACE_STANDARD_MATRIX;
    else
    {
      d->input = dt_colorspaces_create_xyzimatrix_profile((float(*)[3])pipe->image.d65_color_matrix);
      d->clear_input = 1;
    }
  }
  if(type == DT_COLORSPACE_STANDARD_MATRIX)
  {
    // color matrix
    float cam_xyz[12];
    cam_xyz[0] = NAN;

    // Use the legacy name if it has been set to honor the partial matching matrices of low-end Canons
    if (pipe->image.camera_legacy_makermodel[0])
      dt_dcraw_adobe_coeff(pipe->image.camera_legacy_makermodel, (float(*)[12])cam_xyz);
    else
      dt_dcraw_adobe_coeff(pipe->image.camera_makermodel, (float(*)[12])cam_xyz);

    if(isnan(cam_xyz[0]))
    {
      if(dt_image_is_raw(&pipe->image) && !dt_image_is_monochrome(&pipe->image))
      {
        fprintf(stderr, "[colorin] `%s' color matrix not found!\n", pipe->image.camera_makermodel);
        dt_control_log(_("`%s' color matrix not found!"), pipe->image.camera_makermodel);
      }
      type = DT_COLORSPACE_LIN_REC709;
    }
    else
    {
      d->input = dt_colorspaces_create_xyzimatrix_profile((float(*)[3])cam_xyz);
      d->clear_input = 1;
    }
  }

  if(!d->input)
  {
    const dt_colorspaces_color_profile_t *profile = dt_colorspaces_get_profile(type, p->filename, DT_PROFILE_DIRECTION_IN);
    if(profile) d->input = profile->profile;
  }

  if(!d->input && type != DT_COLORSPACE_SRGB)
  {
    // use linear_rec709_rgb as fallback for missing non-sRGB profiles:
    d->input = dt_colorspaces_get_profile(DT_COLORSPACE_LIN_REC709, "", DT_PROFILE_DIRECTION_IN)->profile;
    d->clear_input = 0;
  }

  // final resort: sRGB
  if(!d->input)
  {
    d->input = dt_colorspaces_get_profile(DT_COLORSPACE_SRGB, "", DT_PROFILE_DIRECTION_IN)->profile;
    d->clear_input = 0;
  }

  // should never happen, but catch that case to avoid a crash
  if(!d->input)
  {
    dt_control_log(_("input profile could not be generated!"));
    piece->enabled = 0;
    return;
  }

  cmsColorSpaceSignature input_color_space = cmsGetColorSpace(d->input);
  cmsUInt32Number input_format;
  switch(input_color_space)
  {
    case cmsSigRgbData:
      input_format = TYPE_RGBA_FLT;
      break;
    case cmsSigXYZData:
      input_format = TYPE_XYZA_FLT;
      break;
    default:
      // fprintf("%.*s", 4, input_color_space) doesn't work, it prints the string backwards :(
      fprintf(stderr, "[colorin] input profile color space `%c%c%c%c' not supported\n",
              (char)(input_color_space>>24),
              (char)(input_color_space>>16),
              (char)(input_color_space>>8),
              (char)(input_color_space));
      input_format = TYPE_RGBA_FLT; // this will fail later, triggering the linear rec709 fallback
  }

  // prepare transformation matrix or lcms2 transforms as fallback
  if(d->nrgb)
  {
    // user wants us to clip to a given RGB profile
    if(dt_colorspaces_get_matrix_from_input_profile(d->input, d->cmatrix, d->lut[0], d->lut[1], d->lut[2],
                                                    LUT_SAMPLES, p->intent))
    {
      piece->process_cl_ready = 0;
      d->cmatrix[0] = NAN;
      d->xform_cam_Lab = cmsCreateTransform(d->input, input_format, Lab, TYPE_LabA_FLT, p->intent, 0);
      d->xform_cam_nrgb = cmsCreateTransform(d->input, input_format, d->nrgb, TYPE_RGBA_FLT, p->intent, 0);
      d->xform_nrgb_Lab = cmsCreateTransform(d->nrgb, TYPE_RGBA_FLT, Lab, TYPE_LabA_FLT, p->intent, 0);
    }
    else
    {
      float lutr[1], lutg[1], lutb[1];
      float omat[9];
      dt_colorspaces_get_matrix_from_output_profile(d->nrgb, omat, lutr, lutg, lutb, 1, p->intent);
      mat3mul(d->nmatrix, omat, d->cmatrix);
      dt_colorspaces_get_matrix_from_input_profile(d->nrgb, d->lmatrix, lutr, lutg, lutb, 1, p->intent);
    }
  }
  else
  {
    // default mode: unbound processing
    if(dt_colorspaces_get_matrix_from_input_profile(d->input, d->cmatrix, d->lut[0], d->lut[1], d->lut[2],
                                                    LUT_SAMPLES, p->intent))
    {
      piece->process_cl_ready = 0;
      d->cmatrix[0] = NAN;
      d->xform_cam_Lab = cmsCreateTransform(d->input, input_format, Lab, TYPE_LabA_FLT, p->intent, 0);
    }
  }

  // we might have failed generating the clipping transformations, check that:
  if(d->nrgb && ((!d->xform_cam_nrgb && isnan(d->nmatrix[0])) || (!d->xform_nrgb_Lab && isnan(d->lmatrix[0]))))
  {
    if(d->xform_cam_nrgb)
    {
      cmsDeleteTransform(d->xform_cam_nrgb);
      d->xform_cam_nrgb = NULL;
    }
    if(d->xform_nrgb_Lab)
    {
      cmsDeleteTransform(d->xform_nrgb_Lab);
      d->xform_nrgb_Lab = NULL;
    }
    d->nrgb = NULL;
  }

  // user selected a non-supported output profile, check that:
  if(!d->xform_cam_Lab && isnan(d->cmatrix[0]))
  {
    dt_control_log(_("unsupported input profile has been replaced by linear Rec709 RGB!"));
    if(d->input && d->clear_input) dt_colorspaces_cleanup_profile(d->input);
    d->nrgb = NULL;
    d->input = dt_colorspaces_get_profile(DT_COLORSPACE_LIN_REC709, "", DT_PROFILE_DIRECTION_IN)->profile;
    d->clear_input = 0;
    if(dt_colorspaces_get_matrix_from_input_profile(d->input, d->cmatrix, d->lut[0], d->lut[1], d->lut[2],
                                                    LUT_SAMPLES, p->intent))
    {
      piece->process_cl_ready = 0;
      d->cmatrix[0] = NAN;
      d->xform_cam_Lab = cmsCreateTransform(d->input, TYPE_RGBA_FLT, Lab, TYPE_LabA_FLT, p->intent, 0);
    }
  }

  d->nonlinearlut = 0;

  // now try to initialize unbounded mode:
  // we do a extrapolation for input values above 1.0f.
  // unfortunately we can only do this if we got the computation
  // in our hands, i.e. for the fast builtin-dt-matrix-profile path.
  for(int k = 0; k < 3; k++)
  {
    // omit luts marked as linear (negative as marker)
    if(d->lut[k][0] >= 0.0f)
    {
      d->nonlinearlut++;

      const float x[4] = { 0.7f, 0.8f, 0.9f, 1.0f };
      const float y[4] = { lerp_lut(d->lut[k], x[0]), lerp_lut(d->lut[k], x[1]), lerp_lut(d->lut[k], x[2]),
                           lerp_lut(d->lut[k], x[3]) };
      dt_iop_estimate_exp(x, y, 4, d->unbounded_coeffs[k]);
    }
    else
      d->unbounded_coeffs[k][0] = -1.0f;
  }
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_colorin_data_t));
  dt_iop_colorin_data_t *d = (dt_iop_colorin_data_t *)piece->data;
  d->input = NULL;
  d->nrgb = NULL;
  d->xform_cam_Lab = NULL;
  d->xform_cam_nrgb = NULL;
  d->xform_nrgb_Lab = NULL;
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_colorin_data_t *d = (dt_iop_colorin_data_t *)piece->data;
  if(d->input && d->clear_input) dt_colorspaces_cleanup_profile(d->input);
  if(d->xform_cam_Lab)
  {
    cmsDeleteTransform(d->xform_cam_Lab);
    d->xform_cam_Lab = NULL;
  }
  if(d->xform_cam_nrgb)
  {
    cmsDeleteTransform(d->xform_cam_nrgb);
    d->xform_cam_nrgb = NULL;
  }
  if(d->xform_nrgb_Lab)
  {
    cmsDeleteTransform(d->xform_nrgb_Lab);
    d->xform_nrgb_Lab = NULL;
  }

  free(piece->data);
  piece->data = NULL;
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_module_t *module = (dt_iop_module_t *)self;
  dt_iop_colorin_gui_data_t *g = (dt_iop_colorin_gui_data_t *)self->gui_data;
  dt_iop_colorin_params_t *p = (dt_iop_colorin_params_t *)module->params;
  dt_bauhaus_combobox_set(g->clipping_combobox, p->normalize);

  update_profile_list(self);

  // TODO: merge this into update_profile_list()
  GList *prof = g->image_profiles;
  while(prof)
  {
    dt_colorspaces_color_profile_t *pp = (dt_colorspaces_color_profile_t *)prof->data;
    if(pp->type == p->type && (pp->type != DT_COLORSPACE_FILE || !strcmp(pp->filename, p->filename)))
    {
      dt_bauhaus_combobox_set(g->profile_combobox, pp->in_pos);
      return;
    }
    prof = g_list_next(prof);
  }
  prof = darktable.color_profiles->profiles;
  while(prof)
  {
    dt_colorspaces_color_profile_t *pp = (dt_colorspaces_color_profile_t *)prof->data;
    if(pp->in_pos > -1 &&
       pp->type == p->type && (pp->type != DT_COLORSPACE_FILE || !strcmp(pp->filename, p->filename)))
    {
      dt_bauhaus_combobox_set(g->profile_combobox, pp->in_pos + g->n_image_profiles);
      return;
    }
    prof = g_list_next(prof);
  }
  dt_bauhaus_combobox_set(g->profile_combobox, 0);

  if(p->type != DT_COLORSPACE_ENHANCED_MATRIX)
    fprintf(stderr, "[colorin] could not find requested profile `%s'!\n", dt_colorspaces_get_name(p->type, p->filename));
}

// FIXME: update the gui when we add/remove the eprofile or ematrix
void reload_defaults(dt_iop_module_t *module)
{
  dt_iop_colorin_params_t tmp = (dt_iop_colorin_params_t){ .type = DT_COLORSPACE_ENHANCED_MATRIX,
                                                           .filename = "",
                                                           .intent = DT_INTENT_PERCEPTUAL,
                                                           .normalize = DT_NORMALIZE_OFF,
                                                           .blue_mapping = 0 };

  // we might be called from presets update infrastructure => there is no image
  if(!module->dev) goto end;

  gboolean use_eprofile = FALSE;
  // some file formats like jpeg can have an embedded color profile
  // currently we only support jpeg, j2k, tiff and png
  dt_image_t *img = dt_image_cache_get(darktable.image_cache, module->dev->image_storage.id, 'w');
  if(!img->profile)
  {
    char filename[PATH_MAX] = { 0 };
    gboolean from_cache = TRUE;
    dt_image_full_path(img->id, filename, sizeof(filename), &from_cache);
    const gchar *cc = filename + strlen(filename);
    for(; *cc != '.' && cc > filename; cc--)
      ;
    gchar *ext = g_ascii_strdown(cc + 1, -1);
    if(!strcmp(ext, "jpg") || !strcmp(ext, "jpeg"))
    {
      dt_imageio_jpeg_t jpg;
      if(!dt_imageio_jpeg_read_header(filename, &jpg))
      {
        img->profile_size = dt_imageio_jpeg_read_profile(&jpg, &img->profile);
        use_eprofile = (img->profile_size > 0);
      }
    }
#ifdef HAVE_OPENJPEG
    else if(!strcmp(ext, "jp2") || !strcmp(ext, "j2k") || !strcmp(ext, "j2c") || !strcmp(ext, "jpc"))
    {
      img->profile_size = dt_imageio_j2k_read_profile(filename, &img->profile);
      use_eprofile = (img->profile_size > 0);
    }
#endif
    else if((!strcmp(ext, "tif") || !strcmp(ext, "tiff")) && dt_imageio_is_ldr(filename))
    {
      img->profile_size = dt_imageio_tiff_read_profile(filename, &img->profile);
      use_eprofile = (img->profile_size > 0);
    }
    else if(!strcmp(ext, "png"))
    {
      img->profile_size = dt_imageio_png_read_profile(filename, &img->profile);
      use_eprofile = (img->profile_size > 0);
    }
    g_free(ext);
  }
  else
    use_eprofile = TRUE; // the image has a profile assigned

  if(img->flags & DT_IMAGE_4BAYER) // 4Bayer images have been pre-converted to rec2020
    tmp.type = DT_COLORSPACE_LIN_REC709;
  else if(use_eprofile)
    tmp.type = DT_COLORSPACE_EMBEDDED_ICC;
  else if(module->dev->image_storage.colorspace == DT_IMAGE_COLORSPACE_SRGB)
    tmp.type = DT_COLORSPACE_SRGB;
  else if(module->dev->image_storage.colorspace == DT_IMAGE_COLORSPACE_ADOBE_RGB)
    tmp.type = DT_COLORSPACE_ADOBERGB;
  else if(dt_image_is_ldr(&module->dev->image_storage))
    tmp.type = DT_COLORSPACE_SRGB;
  else if(!isnan(module->dev->image_storage.d65_color_matrix[0]))
    tmp.type = DT_COLORSPACE_EMBEDDED_MATRIX;

  dt_image_cache_write_release(darktable.image_cache, img, DT_IMAGE_CACHE_RELAXED);

end:
  memcpy(module->params, &tmp, sizeof(dt_iop_colorin_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_colorin_params_t));
}

void init(dt_iop_module_t *module)
{
  // module->data = malloc(sizeof(dt_iop_colorin_data_t));
  module->params = calloc(1, sizeof(dt_iop_colorin_params_t));
  module->default_params = calloc(1, sizeof(dt_iop_colorin_params_t));
  module->params_size = sizeof(dt_iop_colorin_params_t);
  module->gui_data = NULL;
  module->priority = 371; // module order created by iop_dependencies.py, do not edit!
  module->hide_enable_button = 1;
  module->default_enabled = 1;
}

void cleanup(dt_iop_module_t *module)
{
  free(module->params);
  module->params = NULL;
}

static void update_profile_list(dt_iop_module_t *self)
{
  dt_iop_colorin_gui_data_t *g = (dt_iop_colorin_gui_data_t *)self->gui_data;

  // clear and refill the image profile list
  g_list_free_full(g->image_profiles, free);
  g->image_profiles = NULL;
  g->n_image_profiles = 0;

  int pos = -1;
  // some file formats like jpeg can have an embedded color profile
  // currently we only support jpeg, j2k, tiff and png
  const dt_image_t *cimg = dt_image_cache_get(darktable.image_cache, self->dev->image_storage.id, 'r');
  if(cimg->profile)
  {
    dt_colorspaces_color_profile_t *prof
        = (dt_colorspaces_color_profile_t *)calloc(1, sizeof(dt_colorspaces_color_profile_t));
    g_strlcpy(prof->name, dt_colorspaces_get_name(DT_COLORSPACE_EMBEDDED_ICC, ""), sizeof(prof->name));
    prof->type = DT_COLORSPACE_EMBEDDED_ICC;
    g->image_profiles = g_list_append(g->image_profiles, prof);
    prof->in_pos = ++pos;
  }
  dt_image_cache_read_release(darktable.image_cache, cimg);
  // use the matrix embedded in some DNGs and EXRs
  if(!isnan(self->dev->image_storage.d65_color_matrix[0]))
  {
    dt_colorspaces_color_profile_t *prof
        = (dt_colorspaces_color_profile_t *)calloc(1, sizeof(dt_colorspaces_color_profile_t));
    g_strlcpy(prof->name, dt_colorspaces_get_name(DT_COLORSPACE_EMBEDDED_MATRIX, ""), sizeof(prof->name));
    prof->type = DT_COLORSPACE_EMBEDDED_MATRIX;
    g->image_profiles = g_list_append(g->image_profiles, prof);
    prof->in_pos = ++pos;
  }
  // get color matrix from raw image:
  float cam_xyz[12];
  cam_xyz[0] = NAN;

  // Use the legacy name if it has been set to honor the partial matching matrices of low-end Canons
  if (self->dev->image_storage.camera_legacy_makermodel[0])
    dt_dcraw_adobe_coeff(self->dev->image_storage.camera_legacy_makermodel, (float(*)[12])cam_xyz);
  else
    dt_dcraw_adobe_coeff(self->dev->image_storage.camera_makermodel, (float(*)[12])cam_xyz);

  if(!isnan(cam_xyz[0]) && !(self->dev->image_storage.flags & DT_IMAGE_4BAYER))
  {
    dt_colorspaces_color_profile_t *prof
        = (dt_colorspaces_color_profile_t *)calloc(1, sizeof(dt_colorspaces_color_profile_t));
    g_strlcpy(prof->name, dt_colorspaces_get_name(DT_COLORSPACE_STANDARD_MATRIX, ""), sizeof(prof->name));
    prof->type = DT_COLORSPACE_STANDARD_MATRIX;
    g->image_profiles = g_list_append(g->image_profiles, prof);
    prof->in_pos = ++pos;
  }

  // darktable built-in, if applicable
  for(int k = 0; k < dt_profiled_colormatrix_cnt; k++)
  {
    if(!strcasecmp(self->dev->image_storage.camera_makermodel, dt_profiled_colormatrices[k].makermodel))
    {
      dt_colorspaces_color_profile_t *prof
          = (dt_colorspaces_color_profile_t *)calloc(1, sizeof(dt_colorspaces_color_profile_t));
      g_strlcpy(prof->name, dt_colorspaces_get_name(DT_COLORSPACE_ENHANCED_MATRIX, ""), sizeof(prof->name));
      prof->type = DT_COLORSPACE_ENHANCED_MATRIX;
      g->image_profiles = g_list_append(g->image_profiles, prof);
      prof->in_pos = ++pos;
      break;
    }
  }

  // darktable vendor matrix, if applicable
  for(int k = 0; k < dt_vendor_colormatrix_cnt; k++)
  {
    if(!strcmp(self->dev->image_storage.camera_makermodel, dt_vendor_colormatrices[k].makermodel))
    {
      dt_colorspaces_color_profile_t *prof
          = (dt_colorspaces_color_profile_t *)calloc(1, sizeof(dt_colorspaces_color_profile_t));
      g_strlcpy(prof->name, dt_colorspaces_get_name(DT_COLORSPACE_VENDOR_MATRIX, ""), sizeof(prof->name));
      prof->type = DT_COLORSPACE_VENDOR_MATRIX;
      g->image_profiles = g_list_append(g->image_profiles, prof);
      prof->in_pos = ++pos;
      break;
    }
  }

  // darktable alternate matrix, if applicable
  for(int k = 0; k < dt_alternate_colormatrix_cnt; k++)
  {
    if(!strcmp(self->dev->image_storage.camera_makermodel, dt_alternate_colormatrices[k].makermodel))
    {
      dt_colorspaces_color_profile_t *prof
          = (dt_colorspaces_color_profile_t *)calloc(1, sizeof(dt_colorspaces_color_profile_t));
      g_strlcpy(prof->name, dt_colorspaces_get_name(DT_COLORSPACE_ALTERNATE_MATRIX, ""), sizeof(prof->name));
      prof->type = DT_COLORSPACE_ALTERNATE_MATRIX;
      g->image_profiles = g_list_append(g->image_profiles, prof);
      prof->in_pos = ++pos;
      break;
    }
  }

  g->n_image_profiles = pos + 1;
  g->image_profiles = g_list_first(g->image_profiles);

  // update the gui
  dt_bauhaus_combobox_clear(g->profile_combobox);

  for(GList *l = g->image_profiles; l; l = g_list_next(l))
  {
    dt_colorspaces_color_profile_t *prof = (dt_colorspaces_color_profile_t *)l->data;
    dt_bauhaus_combobox_add(g->profile_combobox, prof->name);
  }
  for(GList *l = darktable.color_profiles->profiles; l; l = g_list_next(l))
  {
    dt_colorspaces_color_profile_t *prof = (dt_colorspaces_color_profile_t *)l->data;
    if(prof->in_pos > -1) dt_bauhaus_combobox_add(g->profile_combobox, prof->name);
  }
}

void gui_init(struct dt_iop_module_t *self)
{
  // pthread_mutex_lock(&darktable.plugin_threadsafe);
  self->gui_data = malloc(sizeof(dt_iop_colorin_gui_data_t));
  dt_iop_colorin_gui_data_t *g = (dt_iop_colorin_gui_data_t *)self->gui_data;

  g->image_profiles = NULL;

  char datadir[PATH_MAX] = { 0 };
  char confdir[PATH_MAX] = { 0 };
  dt_loc_get_datadir(datadir, sizeof(datadir));
  dt_loc_get_user_config_dir(confdir, sizeof(confdir));

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
  dt_gui_add_help_link(self->widget, dt_get_help_url(self->op));
  g->profile_combobox = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->profile_combobox, NULL, _("profile"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->profile_combobox, TRUE, TRUE, 0);

  // now generate the list of profiles applicable to the current image and update the list
  update_profile_list(self);

  dt_bauhaus_combobox_set(g->profile_combobox, 0);

  char *system_profile_dir = g_build_filename(datadir, "color", "in", NULL);
  char *user_profile_dir = g_build_filename(confdir, "color", "in", NULL);
  char *tooltip = g_strdup_printf(_("ICC profiles in %s or %s"), user_profile_dir, system_profile_dir);
  gtk_widget_set_tooltip_text(g->profile_combobox, tooltip);
  g_free(system_profile_dir);
  g_free(user_profile_dir);
  g_free(tooltip);

  g_signal_connect(G_OBJECT(g->profile_combobox), "value-changed", G_CALLBACK(profile_changed), (gpointer)self);

  g->clipping_combobox = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->clipping_combobox, NULL, _("gamut clipping"));

  dt_bauhaus_combobox_add(g->clipping_combobox, _("off"));
  dt_bauhaus_combobox_add(g->clipping_combobox, _("sRGB"));
  dt_bauhaus_combobox_add(g->clipping_combobox, _("Adobe RGB (compatible)"));
  dt_bauhaus_combobox_add(g->clipping_combobox, _("linear Rec709 RGB"));
  dt_bauhaus_combobox_add(g->clipping_combobox, _("linear Rec2020 RGB"));

  gtk_widget_set_tooltip_text(g->clipping_combobox, _("confine Lab values to gamut of RGB color space"));

  gtk_box_pack_start(GTK_BOX(self->widget), g->clipping_combobox, TRUE, TRUE, 0);

  g_signal_connect(G_OBJECT(g->clipping_combobox), "value-changed", G_CALLBACK(normalize_changed), (gpointer)self);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  dt_iop_colorin_gui_data_t *g = (dt_iop_colorin_gui_data_t *)self->gui_data;
  while(g->image_profiles)
  {
    g_free(g->image_profiles->data);
    g->image_profiles = g_list_delete_link(g->image_profiles, g->image_profiles);
  }
  free(self->gui_data);
  self->gui_data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
