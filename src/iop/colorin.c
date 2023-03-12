/*
    This file is part of darktable,
    Copyright (C) 2009-2023 darktable developers.

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
#include "common/imagebuf.h"
#include "common/iop_profile.h"
#include "common/colormatrices.c"
#include "common/colorspaces.h"
#include "common/colorspaces_inline_conversions.h"
#include "common/file_location.h"
#include "common/image_cache.h"
#include "common/opencl.h"
#include "control/control.h"
#include "develop/develop.h"
#include "gui/gtk.h"
#include "gui/accelerators.h"
#ifdef HAVE_OPENJPEG
#include "imageio/imageio_j2k.h"
#endif
#include "imageio/imageio_jpeg.h"
#include "imageio/imageio_png.h"
#include "imageio/imageio_tiff.h"
#ifdef HAVE_LIBAVIF
#include "imageio/imageio_avif.h"
#endif
#ifdef HAVE_LIBHEIF
#include "imageio/imageio_heif.h"
#endif
#include "develop/imageop_math.h"
#include "develop/imageop_gui.h"
#include "iop/iop_api.h"

#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <lcms2.h>

// max iccprofile file name length
// must be in synch with dt_colorspaces_color_profile_t
#define DT_IOP_COLOR_ICC_LEN 512

#define LUT_SAMPLES 0x10000

DT_MODULE_INTROSPECTION(7, dt_iop_colorin_params_t)

static void update_profile_list(dt_iop_module_t *self);

typedef enum dt_iop_color_normalize_t
{
  DT_NORMALIZE_OFF,               //$DESCRIPTION: "off"
  DT_NORMALIZE_SRGB,              //$DESCRIPTION: "sRGB"
  DT_NORMALIZE_ADOBE_RGB,         //$DESCRIPTION: "Adobe RGB (compatible)"
  DT_NORMALIZE_LINEAR_REC709_RGB, //$DESCRIPTION: "linear Rec709 RGB"
  DT_NORMALIZE_LINEAR_REC2020_RGB //$DESCRIPTION: "linear Rec2020 RGB"
} dt_iop_color_normalize_t;

typedef struct dt_iop_colorin_params_t
{
  dt_colorspaces_color_profile_type_t type; // $DEFAULT: DT_COLORSPACE_ENHANCED_MATRIX
  char filename[DT_IOP_COLOR_ICC_LEN];
  dt_iop_color_intent_t intent;       // $DEFAULT: DT_INTENT_PERCEPTUAL
  dt_iop_color_normalize_t normalize; // $DEFAULT: DT_NORMALIZE_OFF $DESCRIPTION: "gamut clipping"
  int blue_mapping;
  // working color profile
  dt_colorspaces_color_profile_type_t type_work; // $DEFAULT: DT_COLORSPACE_LIN_REC2020
  char filename_work[DT_IOP_COLOR_ICC_LEN];
} dt_iop_colorin_params_t;

typedef struct dt_iop_colorin_gui_data_t
{
  GtkWidget *profile_combobox, *clipping_combobox, *work_combobox;
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
  dt_colormatrix_t cmatrix;
  dt_colormatrix_t nmatrix;
  dt_colormatrix_t lmatrix;
  float unbounded_coeffs[3][3]; // approximation for extrapolation of shaper curves
  int blue_mapping;
  int nonlinearlut;
  dt_colorspaces_color_profile_type_t type;
  dt_colorspaces_color_profile_type_t type_work;
  char filename[DT_IOP_COLOR_ICC_LEN];
  char filename_work[DT_IOP_COLOR_ICC_LEN];
} dt_iop_colorin_data_t;


const char *name()
{
  return _("input color profile");
}

const char **description(struct dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("convert any RGB input to pipeline reference RGB\n"
                                        "using color profiles to remap RGB values"),
                                      _("mandatory"),
                                      _("linear or non-linear, RGB, scene-referred"),
                                      _("defined by profile"),
                                      _("linear, RGB, scene-referred"));
}

int default_group()
{
  return IOP_GROUP_COLOR | IOP_GROUP_TECHNICAL;
}

int flags()
{
  return IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_ONE_INSTANCE | IOP_FLAGS_CACHE_IMPORTANT_NEXT;
}

int default_colorspace(dt_iop_module_t *self,
                       dt_dev_pixelpipe_t *pipe,
                       dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

int input_colorspace(dt_iop_module_t *self,
                     dt_dev_pixelpipe_t *pipe,
                     dt_dev_pixelpipe_iop_t *piece)
{
  if(piece)
  {
    const dt_iop_colorin_data_t *const d = (dt_iop_colorin_data_t *)piece->data;
    if(d->type == DT_COLORSPACE_LAB)
      return IOP_CS_LAB;
  }
  return IOP_CS_RGB;
}

int output_colorspace(dt_iop_module_t *self,
                      dt_dev_pixelpipe_t *pipe,
                      dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_LAB;
}

static void _resolve_work_profile(dt_colorspaces_color_profile_type_t *work_type,
                                  char *work_filename)
{
  for(GList *l = darktable.color_profiles->profiles; l; l = g_list_next(l))
  {
    dt_colorspaces_color_profile_t *prof = (dt_colorspaces_color_profile_t *)l->data;
    if(prof->work_pos > -1 && *work_type == prof->type
       && (prof->type != DT_COLORSPACE_FILE
           || dt_colorspaces_is_profile_equal(prof->filename, work_filename)))
      return;
  }

  dt_print(DT_DEBUG_ALWAYS,
           "[colorin] profile `%s' not suitable for work profile."
           " it has been replaced by linear Rec2020 RGB!\n",
           dt_colorspaces_get_name(*work_type, work_filename));
  *work_type = DT_COLORSPACE_LIN_REC2020;
  work_filename[0] = '\0';
}

int legacy_params(dt_iop_module_t *self,
                  const void *const old_params,
                  const int old_version,
                  void *new_params,
                  const int new_version)
{
#define DT_IOP_COLOR_ICC_LEN_V5 100

  if(old_version == 1 && new_version == 7)
  {
    typedef struct dt_iop_colorin_params_v1_t
    {
      char iccprofile[DT_IOP_COLOR_ICC_LEN_V5];
      dt_iop_color_intent_t intent;
    } dt_iop_colorin_params_v1_t;

    const dt_iop_colorin_params_v1_t *old = (dt_iop_colorin_params_v1_t *)old_params;
    dt_iop_colorin_params_t *new = (dt_iop_colorin_params_t *)new_params;
    memset(new, 0, sizeof(*new));

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
    else if(!strcmp(old->iccprofile, "linear_rec709_rgb")
            || !strcmp(old->iccprofile, "linear_rgb"))
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
    new->type_work = DT_COLORSPACE_LIN_REC709;
    new->filename_work[0] = '\0';
    return 0;
  }
  if(old_version == 2 && new_version == 7)
  {
    typedef struct dt_iop_colorin_params_v2_t
    {
      char iccprofile[DT_IOP_COLOR_ICC_LEN_V5];
      dt_iop_color_intent_t intent;
      int normalize;
    } dt_iop_colorin_params_v2_t;

    const dt_iop_colorin_params_v2_t *old = (dt_iop_colorin_params_v2_t *)old_params;
    dt_iop_colorin_params_t *new = (dt_iop_colorin_params_t *)new_params;
    memset(new, 0, sizeof(*new));

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
    else if(!strcmp(old->iccprofile, "linear_rec709_rgb")
            || !strcmp(old->iccprofile, "linear_rgb"))
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
    new->type_work = DT_COLORSPACE_LIN_REC709;
    new->filename_work[0] = '\0';
    return 0;
  }
  if(old_version == 3 && new_version == 7)
  {
    typedef struct dt_iop_colorin_params_v3_t
    {
      char iccprofile[DT_IOP_COLOR_ICC_LEN_V5];
      dt_iop_color_intent_t intent;
      int normalize;
      int blue_mapping;
    } dt_iop_colorin_params_v3_t;

    const dt_iop_colorin_params_v3_t *old = (dt_iop_colorin_params_v3_t *)old_params;
    dt_iop_colorin_params_t *new = (dt_iop_colorin_params_t *)new_params;
    memset(new, 0, sizeof(*new));

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
    else if(!strcmp(old->iccprofile, "linear_rec709_rgb")
            || !strcmp(old->iccprofile, "linear_rgb"))
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
    new->type_work = DT_COLORSPACE_LIN_REC709;
    new->filename_work[0] = '\0';

    return 0;
  }
  if(old_version == 4 && new_version == 7)
  {
    typedef struct dt_iop_colorin_params_v4_t
    {
      dt_colorspaces_color_profile_type_t type;
      char filename[DT_IOP_COLOR_ICC_LEN_V5];
      dt_iop_color_intent_t intent;
      int normalize;
      int blue_mapping;
    } dt_iop_colorin_params_v4_t;

    const dt_iop_colorin_params_v4_t *old = (dt_iop_colorin_params_v4_t *)old_params;
    dt_iop_colorin_params_t *new = (dt_iop_colorin_params_t *)new_params;
    memset(new, 0, sizeof(*new));

    new->type = old->type;
    g_strlcpy(new->filename, old->filename, sizeof(new->filename));
    new->intent = old->intent;
    new->normalize = old->normalize;
    new->blue_mapping = old->blue_mapping;
    new->type_work = DT_COLORSPACE_LIN_REC709;
    new->filename_work[0] = '\0';

    return 0;
  }
  if(old_version == 5 && new_version == 7)
  {
    typedef struct dt_iop_colorin_params_v5_t
    {
      dt_colorspaces_color_profile_type_t type;
      char filename[DT_IOP_COLOR_ICC_LEN_V5];
      dt_iop_color_intent_t intent;
      int normalize;
      int blue_mapping;
      // working color profile
      dt_colorspaces_color_profile_type_t type_work;
      char filename_work[DT_IOP_COLOR_ICC_LEN_V5];
    } dt_iop_colorin_params_v5_t;

    const dt_iop_colorin_params_v5_t *old = (dt_iop_colorin_params_v5_t *)old_params;
    dt_iop_colorin_params_t *new = (dt_iop_colorin_params_t *)new_params;
    memset(new, 0, sizeof(*new));

    new->type = old->type;
    g_strlcpy(new->filename, old->filename, sizeof(new->filename));
    new->intent = old->intent;
    new->normalize = old->normalize;
    new->blue_mapping = old->blue_mapping;
    new->type_work = old->type_work;
    g_strlcpy(new->filename_work, old->filename_work, sizeof(new->filename_work));
    _resolve_work_profile(&new->type_work, new->filename_work);

    return 0;
  }
  if(old_version == 6 && new_version == 7)
  {
    // The structure is equal to to v7 (current) but a new version is
    // introduced to convert invalid working profile choice to the
    // default, linear Rec2020.
    typedef struct dt_iop_colorin_params_v6_t
    {
      dt_colorspaces_color_profile_type_t type;
      char filename[DT_IOP_COLOR_ICC_LEN];
      dt_iop_color_intent_t intent;
      dt_iop_color_normalize_t normalize;
      int blue_mapping;
      // working color profile
      dt_colorspaces_color_profile_type_t type_work;
      char filename_work[DT_IOP_COLOR_ICC_LEN];
    } dt_iop_colorin_params_v6_t;

    const dt_iop_colorin_params_v6_t *old = (dt_iop_colorin_params_v6_t *)old_params;
    dt_iop_colorin_params_t *new = (dt_iop_colorin_params_t *)new_params;
    memcpy(new, old, sizeof(*new));
    _resolve_work_profile(&new->type_work, new->filename_work);

    return 0;
  }
  return 1;
#undef DT_IOP_COLOR_ICC_LEN_V5
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

static void profile_changed(GtkWidget *widget, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(darktable.gui->reset) return;
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
  for(; prof; prof = g_list_next(prof))
  {
    dt_colorspaces_color_profile_t *pp = (dt_colorspaces_color_profile_t *)prof->data;
    if(pp->in_pos == pos)
    {
      p->type = pp->type;
      memcpy(p->filename, pp->filename, sizeof(p->filename));
      dt_dev_add_history_item(darktable.develop, self, TRUE);

      DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals,
                                    DT_SIGNAL_CONTROL_PROFILE_USER_CHANGED,
                                    DT_COLORSPACES_PROFILE_TYPE_INPUT);
      return;
    }
  }
  // should really never happen.
  dt_print(DT_DEBUG_ALWAYS, "[colorin] color profile %s seems to have disappeared!\n",
           dt_colorspaces_get_name(p->type, p->filename));
}

static void workicc_changed(GtkWidget *widget, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_colorin_params_t *p = (dt_iop_colorin_params_t *)self->params;
  if(darktable.gui->reset) return;

  dt_iop_request_focus(self);

  dt_colorspaces_color_profile_type_t type_work = DT_COLORSPACE_NONE;
  char filename_work[DT_IOP_COLOR_ICC_LEN];

  int pos = dt_bauhaus_combobox_get(widget);
  for(const GList *prof = darktable.color_profiles->profiles;
      prof;
      prof = g_list_next(prof))
  {
    dt_colorspaces_color_profile_t *pp = (dt_colorspaces_color_profile_t *)prof->data;
    if(pp->work_pos == pos)
    {
      type_work = pp->type;
      g_strlcpy(filename_work, pp->filename, sizeof(filename_work));
      break;
    }
  }

  if(type_work != DT_COLORSPACE_NONE)
  {
    p->type_work = type_work;
    g_strlcpy(p->filename_work, filename_work, sizeof(p->filename_work));

    const dt_iop_order_iccprofile_info_t *const work_profile =
      dt_ioppr_add_profile_info_to_list(self->dev, p->type_work,
                                        p->filename_work, DT_INTENT_PERCEPTUAL);
    if(work_profile == NULL
       || isnan(work_profile->matrix_in[0][0])
       || isnan(work_profile->matrix_out[0][0]))
    {
      dt_print(DT_DEBUG_ALWAYS, "[colorin] can't extract matrix from colorspace `%s',"
               " it will be replaced by Rec2020 RGB!\n", p->filename_work);
      dt_control_log(_("can't extract matrix from colorspace `%s'"
                       ", it will be replaced by Rec2020 RGB!"), p->filename_work);

    }
    dt_dev_add_history_item(darktable.develop, self, TRUE);

    DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals,
                                  DT_SIGNAL_CONTROL_PROFILE_USER_CHANGED,
                                  DT_COLORSPACES_PROFILE_TYPE_WORK);

    dt_dev_pixelpipe_rebuild(self->dev);
  }
  else
  {
    // should really never happen.
    dt_print(DT_DEBUG_ALWAYS, "[colorin] color profile %s seems to have disappeared!\n",
             dt_colorspaces_get_name(p->type_work, p->filename_work));
  }
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
int process_cl(struct dt_iop_module_t *self,
               dt_dev_pixelpipe_iop_t *piece,
               cl_mem dev_in,
               cl_mem dev_out,
               const dt_iop_roi_t *const roi_in,
               const dt_iop_roi_t *const roi_out)
{
  dt_iop_colorin_data_t *d = (dt_iop_colorin_data_t *)piece->data;
  dt_iop_colorin_global_data_t *gd = (dt_iop_colorin_global_data_t *)self->global_data;
  cl_mem dev_m = NULL, dev_l = NULL, dev_r = NULL;
  cl_mem dev_g = NULL, dev_b = NULL, dev_coeffs = NULL;

  int kernel;
  float cmat[9], lmat[9];

  if(d->nrgb)
  {
    kernel = gd->kernel_colorin_clipping;
    pack_3xSSE_to_3x3(d->nmatrix, cmat);
    pack_3xSSE_to_3x3(d->lmatrix, lmat);
  }
  else
  {
    kernel = gd->kernel_colorin_unbound;
    pack_3xSSE_to_3x3(d->cmatrix, cmat);
    pack_3xSSE_to_3x3(d->lmatrix, lmat);
  }

  cl_int err = DT_OPENCL_DEFAULT_ERROR;
  const int blue_mapping = d->blue_mapping
                           && dt_image_is_matrix_correction_supported(&piece->pipe->image);
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
  err = dt_opencl_enqueue_kernel_2d_args(devid, kernel, width, height,
                                         CLARG(dev_in), CLARG(dev_out),
                                         CLARG(width), CLARG(height),
                                         CLARG(dev_m), CLARG(dev_l), CLARG(dev_r),
                                         CLARG(dev_g), CLARG(dev_b),
                                         CLARG(blue_mapping), CLARG(dev_coeffs));
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
  dt_print(DT_DEBUG_OPENCL,
           "[opencl_colorin] couldn't enqueue kernel! %s\n", cl_errstr(err));
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

// legacy processing (IOP versions 1 and 2, 2014 and earlier)
static void process_cmatrix_bm(struct dt_iop_module_t *self,
                               dt_dev_pixelpipe_iop_t *piece,
                               const void *const ivoid,
                               void *const ovoid,
                               const dt_iop_roi_t *const roi_in,
                               const dt_iop_roi_t *const roi_out)
{
  const dt_iop_colorin_data_t *const d = (dt_iop_colorin_data_t *)piece->data;
  const int clipping = (d->nrgb != NULL);

  dt_colormatrix_t cmatrix;
  transpose_3xSSE(d->cmatrix, cmatrix);
  dt_colormatrix_t nmatrix;
  transpose_3xSSE(d->nmatrix, nmatrix);
  dt_colormatrix_t lmatrix;
  transpose_3xSSE(d->lmatrix, lmatrix);

  const size_t npixels = (size_t)roi_out->height * roi_out->width;
  // dt_print(DT_DEBUG_ALWAYS, "Using cmatrix codepath\n");
  // only color matrix. use our optimized fast path!
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(npixels, clipping, d, ivoid, ovoid) \
  shared(cmatrix, nmatrix, lmatrix) \
  schedule(static)
#endif
  for(int j = 0; j < npixels; j++)
  {
    const float *const restrict in = (const float *)ivoid + 4*j;
    float *const restrict out = (float *)ovoid + 4*j;
    dt_aligned_pixel_t cam;

    // memcpy(cam, buf_in, sizeof(float)*3);
    // avoid calling this for linear profiles (marked with negative
    // entries), assures unbounded color management without
    // extrapolation.
    for(int c = 0; c < 3; c++)
      cam[c] = (d->lut[c][0] >= 0.0f)
        ? ((in[c] < 1.0f)
           ? lerp_lut(d->lut[c], in[c])
           : dt_iop_eval_exp(d->unbounded_coeffs[c], in[c]))
        : in[c];
    cam[3] = 0.0f; // avoid uninitialized-variable warning

    apply_blue_mapping(cam, cam);

    if(!clipping)
    {
      dt_aligned_pixel_t _xyz;
      dt_apply_transposed_color_matrix(cam, cmatrix, _xyz);
      dt_aligned_pixel_t res;
      dt_XYZ_to_Lab(_xyz, res);
      copy_pixel_nontemporal(out, res);
    }
    else
    {
      dt_aligned_pixel_t nRGB;
      dt_apply_transposed_color_matrix(cam, nmatrix, nRGB);

      dt_aligned_pixel_t cRGB;
      for_each_channel(c)
      {
        cRGB[c] = CLAMP(nRGB[c], 0.0f, 1.0f);
      }

      dt_aligned_pixel_t XYZ;
      dt_apply_transposed_color_matrix(cRGB, lmatrix, XYZ);
      dt_aligned_pixel_t res;
      dt_XYZ_to_Lab(XYZ, res);
      copy_pixel_nontemporal(out, res);
    }
  }
  dt_omploop_sfence();
}

#if defined(__SSE2__)
// legacy processing (IOP versions 1 and 2, 2014 and earlier)
static void process_sse2_cmatrix_bm(struct dt_iop_module_t *self,
                                    dt_dev_pixelpipe_iop_t *piece,
                                    const void *const ivoid,
                                    void *const ovoid,
                                    const dt_iop_roi_t *const roi_in,
                                    const dt_iop_roi_t *const roi_out)
{
  const dt_iop_colorin_data_t *const d = (dt_iop_colorin_data_t *)piece->data;
  const int ch = piece->colors;
  const int clipping = (d->nrgb != NULL);

  // only color matrix. use our optimized fast path!
  float cmat[9], nmat[9], lmat[9];
  pack_3xSSE_to_3x3(d->cmatrix, cmat);
  pack_3xSSE_to_3x3(d->nmatrix, nmat);
  pack_3xSSE_to_3x3(d->lmatrix, lmat);
  float *in = (float *)ivoid;
  float *out = (float *)ovoid;

  const __m128 cm0 = _mm_set_ps(0.0f, cmat[6], cmat[3], cmat[0]);
  const __m128 cm1 = _mm_set_ps(0.0f, cmat[7], cmat[4], cmat[1]);
  const __m128 cm2 = _mm_set_ps(0.0f, cmat[8], cmat[5], cmat[2]);
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(ch, clipping, d, lmat, nmat, roi_in, roi_out, cm0, cm1, cm2) \
  shared(out, in) \
  schedule(static)
#endif
  for(int j = 0; j < roi_out->height; j++)
  {

    float *buf_in = in + (size_t)ch * roi_in->width * j;
    float *buf_out = out + (size_t)ch * roi_out->width * j;
    dt_aligned_pixel_t cam;

    const __m128 nm0 = _mm_set_ps(0.0f, nmat[6], nmat[3], nmat[0]);
    const __m128 nm1 = _mm_set_ps(0.0f, nmat[7], nmat[4], nmat[1]);
    const __m128 nm2 = _mm_set_ps(0.0f, nmat[8], nmat[5], nmat[2]);

    const __m128 lm0 = _mm_set_ps(0.0f, lmat[6], lmat[3], lmat[0]);
    const __m128 lm1 = _mm_set_ps(0.0f, lmat[7], lmat[4], lmat[1]);
    const __m128 lm2 = _mm_set_ps(0.0f, lmat[8], lmat[5], lmat[2]);

    for(int i = 0; i < roi_out->width; i++, buf_in += ch, buf_out += ch)
    {

      // memcpy(cam, buf_in, sizeof(float)*3);
      // avoid calling this for linear profiles (marked with negative
      // entries), assures unbounded color management without
      // extrapolation.
      for(int c = 0; c < 3; c++)
        cam[c] = (d->lut[c][0] >= 0.0f)
          ? ((buf_in[c] < 1.0f)
             ? lerp_lut(d->lut[c], buf_in[c])
             : dt_iop_eval_exp(d->unbounded_coeffs[c], buf_in[c]))
          : buf_in[c];

      apply_blue_mapping(cam, cam);

      if(!clipping)
      {
        __m128 xyz
            = _mm_add_ps(_mm_add_ps(_mm_mul_ps(cm0, _mm_set1_ps(cam[0])),
                                    _mm_mul_ps(cm1, _mm_set1_ps(cam[1]))),
                         _mm_mul_ps(cm2, _mm_set1_ps(cam[2])));
        _mm_stream_ps(buf_out, dt_XYZ_to_Lab_sse2(xyz));
      }
      else
      {
        __m128 nrgb
            = _mm_add_ps(_mm_add_ps(_mm_mul_ps(nm0, _mm_set1_ps(cam[0])),
                                    _mm_mul_ps(nm1, _mm_set1_ps(cam[1]))),
                         _mm_mul_ps(nm2, _mm_set1_ps(cam[2])));
        __m128 crgb = _mm_min_ps(_mm_max_ps(nrgb, _mm_set1_ps(0.0f)),
                                 _mm_set1_ps(1.0f));
        __m128 xyz = _mm_add_ps(_mm_add_ps
                                (_mm_mul_ps(lm0,
                                            _mm_shuffle_ps(crgb, crgb,
                                                           _MM_SHUFFLE(0, 0, 0, 0))),
                                 _mm_mul_ps(lm1,
                                            _mm_shuffle_ps(crgb, crgb,
                                                           _MM_SHUFFLE(1, 1, 1, 1)))),
                                _mm_mul_ps(lm2, _mm_shuffle_ps(crgb, crgb,
                                                               _MM_SHUFFLE(2, 2, 2, 2))));
        _mm_stream_ps(buf_out, dt_XYZ_to_Lab_sse2(xyz));
      }
    }
  }
  _mm_sfence();
}
#endif

static void _cmatrix_fastpath_simple(float *const restrict out,
                                     const float *const restrict in,
                                     size_t npixels,
                                     const dt_colormatrix_t cmatrix)
{
  const dt_aligned_pixel_t cmatrix_row0 = { cmatrix[0][0], cmatrix[1][0], cmatrix[2][0], 0.0f };
  const dt_aligned_pixel_t cmatrix_row1 = { cmatrix[0][1], cmatrix[1][1], cmatrix[2][1], 0.0f };
  const dt_aligned_pixel_t cmatrix_row2 = { cmatrix[0][2], cmatrix[1][2], cmatrix[2][2], 0.0f };

  // this function is called from inside a parallel for loop, so no need for further parallelization
  for(size_t k = 0; k < npixels; k++)
  {
    dt_aligned_pixel_t res;
    dt_RGB_to_Lab(in + 4*k, cmatrix_row0, cmatrix_row1, cmatrix_row2, res);
    copy_pixel_nontemporal(out + 4*k, res);
  }
}

#ifdef __SSE2__
static inline void _cmatrix_fastpath_clipping_sse(float *const restrict out,
                                                  const float *const restrict in,
                                                  size_t npixels,
                                                  const dt_colormatrix_t nmatrix,
                                                  const dt_colormatrix_t lmatrix)
{
  // only color matrix. use our optimized fast path!
  const __m128 nm0 = _mm_set_ps(0.0f, nmatrix[2][0], nmatrix[1][0], nmatrix[0][0]);
  const __m128 nm1 = _mm_set_ps(0.0f, nmatrix[2][1], nmatrix[1][1], nmatrix[0][1]);
  const __m128 nm2 = _mm_set_ps(0.0f, nmatrix[2][2], nmatrix[1][2], nmatrix[0][2]);

  const __m128 lm0 = _mm_set_ps(0.0f, lmatrix[2][0], lmatrix[1][0], lmatrix[0][0]);
  const __m128 lm1 = _mm_set_ps(0.0f, lmatrix[2][1], lmatrix[1][1], lmatrix[0][1]);
  const __m128 lm2 = _mm_set_ps(0.0f, lmatrix[2][2], lmatrix[1][2], lmatrix[0][2]);

  // this function is called from inside a parallel for loop, so no need for further parallelization
  for(size_t k = 0; k < npixels; k++)
  {
    __m128 input = _mm_load_ps(in + 4*k);
    // convert to gamut space
    __m128 nrgb = ((nm0 * _mm_shuffle_ps(input, input, _MM_SHUFFLE(0, 0, 0, 0))) +
                   (nm1 * _mm_shuffle_ps(input, input, _MM_SHUFFLE(1, 1, 1, 1))) +
                   (nm2 * _mm_shuffle_ps(input, input, _MM_SHUFFLE(2, 2, 2, 2))));
    // clip to gamut
    __m128 crgb = _mm_min_ps(_mm_max_ps(nrgb, _mm_set1_ps(0.0f)), _mm_set1_ps(1.0f));
    // convert to output space
    __m128 xyz = ((lm0 * _mm_shuffle_ps(crgb, crgb, _MM_SHUFFLE(0, 0, 0, 0))) +
                  (lm1 * _mm_shuffle_ps(crgb, crgb, _MM_SHUFFLE(1, 1, 1, 1))) +
                  (lm2 * _mm_shuffle_ps(crgb, crgb, _MM_SHUFFLE(2, 2, 2, 2))));
    _mm_stream_ps(out + 4*k, dt_XYZ_to_Lab_sse2(xyz));
  }
  _mm_sfence();
}
#endif

static inline void _cmatrix_fastpath_clipping(float *const restrict out,
                                              const float *const restrict in,
                                              size_t npixels,
                                              const dt_colormatrix_t nmatrix,
                                              const dt_colormatrix_t lmatrix)
{
#ifdef __SSE2__
  // we can't remove the SSE version yet, because I haven't been able to
  // convince GCC10 to put the color matrix rows into registers, unlike
  // with the SSE version.  That makes the non-SSE version quite a bit
  // slower with fewer than 32 threads.
  if(darktable.codepath.SSE2)
  {
    _cmatrix_fastpath_clipping_sse(out, in, npixels, nmatrix, lmatrix);
    return;
  }
#endif
  const dt_aligned_pixel_t nmatrix_row0 = { nmatrix[0][0], nmatrix[1][0], nmatrix[2][0], 0.0f };
  const dt_aligned_pixel_t nmatrix_row1 = { nmatrix[0][1], nmatrix[1][1], nmatrix[2][1], 0.0f };
  const dt_aligned_pixel_t nmatrix_row2 = { nmatrix[0][2], nmatrix[1][2], nmatrix[2][2], 0.0f };
  const dt_aligned_pixel_t lmatrix_row0 = { lmatrix[0][0], lmatrix[1][0], lmatrix[2][0], 0.0f };
  const dt_aligned_pixel_t lmatrix_row1 = { lmatrix[0][1], lmatrix[1][1], lmatrix[2][1], 0.0f };
  const dt_aligned_pixel_t lmatrix_row2 = { lmatrix[0][2], lmatrix[1][2], lmatrix[2][2], 0.0f };
  
  // this function is called from inside a parallel for loop, so no need for further parallelization
  for(size_t k = 0; k < npixels; k++)
  {
    dt_aligned_pixel_t nRGB;
    dt_apply_color_matrix_by_row(in + 4*k, nmatrix_row0, nmatrix_row1, nmatrix_row2, nRGB);
    dt_vector_clip(nRGB);
    dt_aligned_pixel_t res;
    dt_RGB_to_Lab(nRGB, lmatrix_row0, lmatrix_row1, lmatrix_row2, res);
    copy_pixel_nontemporal(out + 4*k, res);
  }
}

static void process_cmatrix_fastpath(struct dt_iop_module_t *self,
                                     dt_dev_pixelpipe_iop_t *piece,
                                     const void *const ivoid,
                                     void *const ovoid,
                                     const dt_iop_roi_t *const roi_in,
                                     const dt_iop_roi_t *const roi_out)
{
  const dt_iop_colorin_data_t *const d = (dt_iop_colorin_data_t *)piece->data;
  assert(piece->colors == 4);
  const int clipping = (d->nrgb != NULL);

  const size_t npixels = (size_t)roi_out->width * roi_out->height;
  const float *const restrict in = (float*)ivoid;
  float *const restrict  out = (float*)ovoid;

#ifdef _OPENMP
  // figure out the number of pixels each thread needs to process
  // round up to a multiple of 4 pixels so that each chunk starts aligned(64)
  const size_t nthreads = dt_get_num_threads();
  const size_t chunksize = 4 * (((npixels / nthreads) + 3) / 4);
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(in, out, npixels, chunksize, nthreads, d, clipping)  \
  schedule(static)
  for(size_t chunk = 0; chunk < nthreads; chunk++)
  {
    size_t start = chunksize * dt_get_thread_num();
    size_t end = MIN(start + chunksize, npixels);
    if(clipping)
      _cmatrix_fastpath_clipping(out + 4*start, in + 4*start, end-start, d->nmatrix, d->lmatrix);
    else
      _cmatrix_fastpath_simple(out + 4*start, in + 4*start, end-start, d->cmatrix);
  }
#else // no OpenMP
  if(clipping)
    _cmatrix_fastpath_clipping(out, in, npixels, d->nmatrix, d->lmatrix);
  else
    _cmatrix_fastpath_simple(out, in, npixels, d->cmatrix);
#endif
  // ensure that all nontemporal writes have been flushed to RAM before we return
  dt_omploop_sfence();
}

#ifdef __SSE2__
static void _cmatrix_proper_simple_sse(float *const restrict out,
                                       const float *const restrict in,
                                       size_t npixels,
                                       const dt_iop_colorin_data_t *const d,
                                       const dt_colormatrix_t cmatrix)
{
  const __m128 cm0 = _mm_set_ps(0.0f, d->cmatrix[2][0], d->cmatrix[1][0], d->cmatrix[0][0]);
  const __m128 cm1 = _mm_set_ps(0.0f, d->cmatrix[2][1], d->cmatrix[1][1], d->cmatrix[0][1]);
  const __m128 cm2 = _mm_set_ps(0.0f, d->cmatrix[2][2], d->cmatrix[1][2], d->cmatrix[0][2]);

  // this function is called from inside a parallel for loop, so no need for further parallelization
  for(size_t k = 0; k < npixels; k++)
  {
    dt_aligned_pixel_t cam;

    // memcpy(cam, in, sizeof(float)*3);
    // avoid calling this for linear profiles (marked with negative
    // entries), assures unbounded color management without
    // extrapolation.
    for(int c = 0; c < 3; c++)
      cam[c] = (d->lut[c][0] >= 0.0f)
        ? ((in[4*k+c] < 1.0f)
           ? lerp_lut(d->lut[c], in[4*k+c])
           : dt_iop_eval_exp(d->unbounded_coeffs[c], in[4*k+c]))
        : in[4*k+c];

    __m128 xyz = ((cm0 * _mm_set1_ps(cam[0]))
                  + (cm1 * _mm_set1_ps(cam[1]))
                  + (cm2 * _mm_set1_ps(cam[2])));
    _mm_stream_ps(out + 4*k, dt_XYZ_to_Lab_sse2(xyz));
  }
}
#endif

static void _cmatrix_proper_simple(float *const restrict out,
                                   const float *const restrict in,
                                   size_t npixels,
                                   const dt_iop_colorin_data_t *const d,
                                   const dt_colormatrix_t cmatrix)
{
#ifdef __SSE2__
  if(darktable.codepath.SSE2)
  {
    _cmatrix_proper_simple_sse(out, in, npixels, d, cmatrix);
    return;
  }
#endif
  const dt_aligned_pixel_t cmatrix_row0 = { cmatrix[0][0], cmatrix[1][0], cmatrix[2][0], 0.0f };
  const dt_aligned_pixel_t cmatrix_row1 = { cmatrix[0][1], cmatrix[1][1], cmatrix[2][1], 0.0f };
  const dt_aligned_pixel_t cmatrix_row2 = { cmatrix[0][2], cmatrix[1][2], cmatrix[2][2], 0.0f };

  // this function is called from inside a parallel for loop, so no need for further parallelization
  for(size_t k = 0; k < npixels; k++)
  {
    dt_aligned_pixel_t cam;
    // avoid calling this for linear profiles (marked with negative
    // entries), assures unbounded color management without
    // extrapolation.
    for(int c = 0; c < 3; c++)
      cam[c] = (d->lut[c][0] >= 0.0f)
        ? ((in[4*k+c] < 1.0f)
           ? lerp_lut(d->lut[c], in[4*k+c])
           : dt_iop_eval_exp(d->unbounded_coeffs[c], in[4*k+c]))
        : in[4*k+c];
    cam[3] = 0.0f; // avoid uninitialized-variable warning

    dt_aligned_pixel_t res;
    dt_RGB_to_Lab(cam, cmatrix_row0, cmatrix_row1, cmatrix_row2, res);
    copy_pixel_nontemporal(out + 4*k, res);
  }
}

#ifdef __SSE2__
static inline void _cmatrix_proper_clipping_sse(float *const restrict out,
                                                const float *const restrict in,
                                                size_t npixels,
                                                const dt_iop_colorin_data_t *const d,
                                                const dt_colormatrix_t nmatrix,
                                                const dt_colormatrix_t lmatrix)
{
  const __m128 nm0 = _mm_set_ps(0.0f, d->nmatrix[2][0], d->nmatrix[1][0], d->nmatrix[0][0]);
  const __m128 nm1 = _mm_set_ps(0.0f, d->nmatrix[2][1], d->nmatrix[1][1], d->nmatrix[0][1]);
  const __m128 nm2 = _mm_set_ps(0.0f, d->nmatrix[2][2], d->nmatrix[1][2], d->nmatrix[0][2]);
  const __m128 lm0 = _mm_set_ps(0.0f, d->lmatrix[2][0], d->lmatrix[1][0], d->lmatrix[0][0]);
  const __m128 lm1 = _mm_set_ps(0.0f, d->lmatrix[2][1], d->lmatrix[1][1], d->lmatrix[0][1]);
  const __m128 lm2 = _mm_set_ps(0.0f, d->lmatrix[2][2], d->lmatrix[1][2], d->lmatrix[0][2]);

  // this function is called from inside a parallel for loop, so no need for further parallelization
  for(size_t k = 0; k < npixels; k++)
  {
    dt_aligned_pixel_t cam;

    // memcpy(cam, in, sizeof(float)*3);
    // avoid calling this for linear profiles (marked with negative
    // entries), assures unbounded color management without
    // extrapolation.
    for(int c = 0; c < 3; c++)
      cam[c] = (d->lut[c][0] >= 0.0f)
        ? ((in[4*k+c] < 1.0f)
           ? lerp_lut(d->lut[c], in[4*k+c])
           : dt_iop_eval_exp(d->unbounded_coeffs[c], in[4*k+c]))
        : in[4*k+c];

    // convert to clipping colorspace
    __m128 nrgb = ((nm0 * _mm_set1_ps(cam[0]))
                   + (nm1 * _mm_set1_ps(cam[1]))
                   + (nm2 * _mm_set1_ps(cam[2])));
    // clip to gamut
    __m128 crgb = _mm_min_ps(_mm_max_ps(nrgb, _mm_set1_ps(0.0f)), _mm_set1_ps(1.0f));
    // convert to output colorspace
    __m128 xyz = ((lm0 * _mm_shuffle_ps(crgb, crgb, _MM_SHUFFLE(0, 0, 0, 0)))
                  + (lm1 * _mm_shuffle_ps(crgb, crgb, _MM_SHUFFLE(1, 1, 1, 1)))
                  + (lm2 * _mm_shuffle_ps(crgb, crgb, _MM_SHUFFLE(2, 2, 2, 2))));
    _mm_stream_ps(out + 4*k, dt_XYZ_to_Lab_sse2(xyz));
  }
}
#endif

static inline void _cmatrix_proper_clipping(float *const restrict out,
                                            const float *const restrict in,
                                            size_t npixels,
                                            const dt_iop_colorin_data_t *const d,
                                            const dt_colormatrix_t nmatrix,
                                            const dt_colormatrix_t lmatrix)
{
#ifdef __SSE2__
  if(darktable.codepath.SSE2)
  {
    _cmatrix_proper_clipping_sse(out, in, npixels, d, nmatrix, lmatrix);
    return;
  }
#endif
  const dt_aligned_pixel_t nmatrix_row0 = { nmatrix[0][0], nmatrix[1][0], nmatrix[2][0], 0.0f };
  const dt_aligned_pixel_t nmatrix_row1 = { nmatrix[0][1], nmatrix[1][1], nmatrix[2][1], 0.0f };
  const dt_aligned_pixel_t nmatrix_row2 = { nmatrix[0][2], nmatrix[1][2], nmatrix[2][2], 0.0f };
  const dt_aligned_pixel_t lmatrix_row0 = { lmatrix[0][0], lmatrix[1][0], lmatrix[2][0], 0.0f };
  const dt_aligned_pixel_t lmatrix_row1 = { lmatrix[0][1], lmatrix[1][1], lmatrix[2][1], 0.0f };
  const dt_aligned_pixel_t lmatrix_row2 = { lmatrix[0][2], lmatrix[1][2], lmatrix[2][2], 0.0f };
  
  // this function is called from inside a parallel for loop, so no need for further parallelization
  for(size_t k = 0; k < npixels; k++)
  {
    dt_aligned_pixel_t cam;
    // avoid calling this for linear profiles (marked with negative
    // entries), assures unbounded color management without
    // extrapolation.
    for(int c = 0; c < 3; c++)
      cam[c] = (d->lut[c][0] >= 0.0f)
        ? ((in[4*k+c] < 1.0f)
           ? lerp_lut(d->lut[c], in[4*k+c])
           : dt_iop_eval_exp(d->unbounded_coeffs[c], in[4*k+c]))
        : in[4*k+c];
    cam[3] = 0.0f; // avoid uninitialized-variable warning

    dt_aligned_pixel_t nRGB;
    dt_apply_color_matrix_by_row(cam, nmatrix_row0, nmatrix_row1, nmatrix_row2, nRGB);

    // two separate clamping operations proves more efficient than
    // either CLAMP or CLIP macros...
    for_each_channel(c)
      nRGB[c] = MAX(nRGB[c], 0.0f);
    for_each_channel(c)
      nRGB[c] = MIN(nRGB[c], 1.0f);

    dt_aligned_pixel_t res;
    dt_RGB_to_Lab(nRGB, lmatrix_row0, lmatrix_row1, lmatrix_row2, res);
    copy_pixel_nontemporal(out + 4*k, res);
  }
}

static void process_cmatrix_proper(struct dt_iop_module_t *self,
                                   dt_dev_pixelpipe_iop_t *piece,
                                   const void *const ivoid,
                                   void *const ovoid,
                                   const dt_iop_roi_t *const roi_in,
                                   const dt_iop_roi_t *const roi_out)
{
  const dt_iop_colorin_data_t *const d = (dt_iop_colorin_data_t *)piece->data;
  assert(piece->colors == 4);
  const int clipping = (d->nrgb != NULL);

  const size_t npixels = (size_t)roi_out->width * roi_out->height;
  const float *const restrict in = (float*)ivoid;
  float *const restrict  out = (float*)ovoid;

#ifdef _OPENMP
  // figure out the number of pixels each thread needs to process
  // round up to a multiple of 4 pixels so that each chunk starts aligned(64)
  const size_t nthreads = dt_get_num_threads();
  const size_t chunksize = 4 * (((npixels / nthreads) + 3) / 4);
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(in, out, npixels, chunksize, nthreads, clipping, d) \
  schedule(static)
  for(size_t chunk = 0; chunk < nthreads; chunk++)
  {
    size_t start = chunksize * dt_get_thread_num();
    size_t end = MIN(start + chunksize, npixels);
    if(clipping)
      _cmatrix_proper_clipping(out + 4*start, in + 4*start, end-start, d, d->nmatrix, d->lmatrix);
    else
      _cmatrix_proper_simple(out + 4*start, in + 4*start, end-start, d, d->cmatrix);
  }
#else
  if(clipping)
    _cmatrix_proper_clipping(out, in, npixels, d,d->nmatrix, d->lmatrix);
  else
    _cmatrix_proper_simple(out, in, npixels, d, d->cmatrix);
#endif
  // ensure that all nontemporal writes have been flushed to RAM before we return
  dt_omploop_sfence();
}

static void process_cmatrix(struct dt_iop_module_t *self,
                            dt_dev_pixelpipe_iop_t *piece,
                            const void *const ivoid,
                            void *const ovoid,
                            const dt_iop_roi_t *const roi_in,
                            const dt_iop_roi_t *const roi_out)
{
  const dt_iop_colorin_data_t *const d = (dt_iop_colorin_data_t *)piece->data;
  const int blue_mapping =
    d->blue_mapping && dt_image_is_matrix_correction_supported(&piece->pipe->image);

  if(!blue_mapping && d->nonlinearlut == 0)
  {
    process_cmatrix_fastpath(self, piece, ivoid, ovoid, roi_in, roi_out);
  }
  else if(blue_mapping)
  {
#ifdef __SSE2__
    if(darktable.codepath.SSE2)
      process_sse2_cmatrix_bm(self, piece, ivoid, ovoid, roi_in, roi_out);
    else
#endif
    process_cmatrix_bm(self, piece, ivoid, ovoid, roi_in, roi_out);
  }
  else
  {
    process_cmatrix_proper(self, piece, ivoid, ovoid, roi_in, roi_out);
  }
}

// legacy processing (IOP versions 1 and 2, 2014 and earlier)
static void process_lcms2_bm(struct dt_iop_module_t *self,
                             dt_dev_pixelpipe_iop_t *piece,
                             const void *const ivoid,
                             void *const ovoid,
                             const dt_iop_roi_t *const roi_in,
                             const dt_iop_roi_t *const roi_out)
{
  const dt_iop_colorin_data_t *const d = (dt_iop_colorin_data_t *)piece->data;
  const size_t height = roi_out->height;
  const size_t width = roi_out->width;

// use general lcms2 fallback
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(d, ivoid, ovoid, height, width) \
  schedule(static)
#endif
  for(int k = 0; k < height; k++)
  {
    const float *in = (const float *)ivoid + (size_t)4 * k * width;
    float *out = (float *)ovoid + (size_t)4 * k * width;

    for(int j = 0; j < width; j++)
    {
      apply_blue_mapping(in + 4*j, out + 4*j);
    }

    // convert to (L,a/L,b/L) to be able to change L without changing saturation.
    if(!d->nrgb)
    {
      cmsDoTransform(d->xform_cam_Lab, out, out, width);
    }
    else
    {
      cmsDoTransform(d->xform_cam_nrgb, out, out, width);

      for(int j = 0; j < width; j++)
      {
        for_each_channel(c)
        {
          out[4*j+c] = CLAMP(out[4*j+c], 0.0f, 1.0f);
        }
      }

      cmsDoTransform(d->xform_nrgb_Lab, out, out, width);
    }
  }
}

static void process_lcms2_proper(struct dt_iop_module_t *self,
                                 dt_dev_pixelpipe_iop_t *piece,
                                 const void *const ivoid,
                                 void *const ovoid,
                                 const dt_iop_roi_t *const roi_in,
                                 const dt_iop_roi_t *const roi_out)
{
  const dt_iop_colorin_data_t *const d = (dt_iop_colorin_data_t *)piece->data;
  const size_t height = roi_out->height;
  const size_t width = roi_out->width;

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(d, ivoid, ovoid, height, width) \
  schedule(static)
#endif
  for(size_t k = 0; k < height; k++)
  {
    const float *in = (const float *)ivoid + (size_t)4 * k * width;
    float *out = (float *)ovoid + (size_t)4 * k * width;

    // convert to (L,a/L,b/L) to be able to change L without changing saturation.
    if(!d->nrgb)
    {
      cmsDoTransform(d->xform_cam_Lab, in, out, width);
    }
    else
    {
      cmsDoTransform(d->xform_cam_nrgb, in, out, width);

      for(int j = 0; j < width; j++)
      {
        for_each_channel(c)
        {
          out[4*j+c] = CLAMP(out[4*j+c], 0.0f, 1.0f);
        }
      }

      cmsDoTransform(d->xform_nrgb_Lab, out, out, width);
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
  if(!dt_iop_have_required_input_format(4 /*we need full-color pixels*/, self, piece->colors,
                                         ivoid, ovoid, roi_in, roi_out))
    return;

  const dt_iop_colorin_data_t *const d = (dt_iop_colorin_data_t *)piece->data;
  const int blue_mapping =
    d->blue_mapping && dt_image_is_matrix_correction_supported(&piece->pipe->image);

  if(d->type == DT_COLORSPACE_LAB)
  {
    dt_iop_image_copy_by_size(ovoid, ivoid, roi_out->width, roi_out->height, piece->colors);
  }
  else if(!isnan(d->cmatrix[0][0]))
  {
    process_cmatrix(self, piece, ivoid, ovoid, roi_in, roi_out);
  }
  else
  {
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
}

void commit_params(struct dt_iop_module_t *self,
                   dt_iop_params_t *p1,
                   dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  const dt_iop_colorin_params_t *p = (dt_iop_colorin_params_t *)p1;
  dt_iop_colorin_data_t *d = (dt_iop_colorin_data_t *)piece->data;

  d->type = p->type;
  d->type_work = p->type_work;
  g_strlcpy(d->filename, p->filename, sizeof(d->filename));
  g_strlcpy(d->filename_work, p->filename_work, sizeof(d->filename_work));

  const cmsHPROFILE Lab =
    dt_colorspaces_get_profile(DT_COLORSPACE_LAB, "", DT_PROFILE_DIRECTION_ANY)->profile;

  // only clean up when it's a type that we created here
  if(d->input && d->clear_input) dt_colorspaces_cleanup_profile(d->input);
  d->input = NULL;
  d->clear_input = 0;
  d->nrgb = NULL;

  d->blue_mapping = p->blue_mapping;

  switch(p->normalize)
  {
    case DT_NORMALIZE_SRGB:
      d->nrgb = dt_colorspaces_get_profile(DT_COLORSPACE_SRGB, "",
                                           DT_PROFILE_DIRECTION_IN)->profile;
      break;
    case DT_NORMALIZE_ADOBE_RGB:
      d->nrgb = dt_colorspaces_get_profile(DT_COLORSPACE_ADOBERGB, "",
                                           DT_PROFILE_DIRECTION_IN)->profile;
      break;
    case DT_NORMALIZE_LINEAR_REC709_RGB:
      d->nrgb = dt_colorspaces_get_profile(DT_COLORSPACE_LIN_REC709, "",
                                           DT_PROFILE_DIRECTION_IN)->profile;
      break;
    case DT_NORMALIZE_LINEAR_REC2020_RGB:
      d->nrgb = dt_colorspaces_get_profile(DT_COLORSPACE_LIN_REC2020, "",
                                           DT_PROFILE_DIRECTION_IN)->profile;
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

  d->cmatrix[0][0] = d->nmatrix[0][0] = d->lmatrix[0][0] = NAN;
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
    const dt_image_t *cimg = dt_image_cache_get(darktable.image_cache, pipe->image.id, 'r');
    if(isnan(cimg->d65_color_matrix[0]))
      type = DT_COLORSPACE_STANDARD_MATRIX;
    else
    {
      d->input = dt_colorspaces_create_xyzimatrix_profile
        ((float(*)[3])cimg->d65_color_matrix);
      d->clear_input = 1;
    }
    dt_image_cache_read_release(darktable.image_cache, cimg);
  }
  if(type == DT_COLORSPACE_STANDARD_MATRIX)
  {
    if(isnan(pipe->image.adobe_XYZ_to_CAM[0][0]))
    {
      if(dt_image_is_matrix_correction_supported(&pipe->image))
      {
        dt_print(DT_DEBUG_ALWAYS, "[colorin] `%s' color matrix not found!\n",
                 pipe->image.camera_makermodel);
        dt_control_log(_("`%s' color matrix not found!"), pipe->image.camera_makermodel);
      }
      type = DT_COLORSPACE_LIN_REC709;
    }
    else
    {
      d->input = dt_colorspaces_create_xyzimatrix_profile
        ((float(*)[3])pipe->image.adobe_XYZ_to_CAM);
      d->clear_input = 1;
    }
  }

  if(!d->input)
  {
    const dt_colorspaces_color_profile_t *profile =
      dt_colorspaces_get_profile(type, p->filename, DT_PROFILE_DIRECTION_IN);
    if(profile) d->input = profile->profile;
  }

  if(!d->input && type != DT_COLORSPACE_SRGB)
  {
    // use linear_rec709_rgb as fallback for missing non-sRGB profiles:
    d->input = dt_colorspaces_get_profile(DT_COLORSPACE_LIN_REC709, "",
                                          DT_PROFILE_DIRECTION_IN)->profile;
    d->clear_input = 0;
  }

  // final resort: sRGB
  if(!d->input)
  {
    d->input = dt_colorspaces_get_profile(DT_COLORSPACE_SRGB, "",
                                          DT_PROFILE_DIRECTION_IN)->profile;
    d->clear_input = 0;
  }

  // should never happen, but catch that case to avoid a crash
  if(!d->input)
  {
    dt_print(DT_DEBUG_ALWAYS, "[colorin] input profile could not be generated!\n");
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
      // FIXME: even though this is allowed/works,
      // dt_ioppr_generate_profile_info still complains about these
      // profiles
      input_format = TYPE_XYZA_FLT;
      break;
    default:
      dt_print(DT_DEBUG_ALWAYS, "[colorin] input profile color space `%c%c%c%c' not supported\n",
               (char)(input_color_space>>24),
               (char)(input_color_space>>16),
               (char)(input_color_space>>8),
               (char)(input_color_space));
      input_format = TYPE_RGBA_FLT; // this will fail later,
                                    // triggering the linear rec709
                                    // fallback
  }

  // prepare transformation matrix or lcms2 transforms as fallback
  if(d->nrgb)
  {
    // user wants us to clip to a given RGB profile
    if(dt_colorspaces_get_matrix_from_input_profile
       (d->input, d->cmatrix, d->lut[0], d->lut[1], d->lut[2],
        LUT_SAMPLES))
    {
      piece->process_cl_ready = 0;
      d->cmatrix[0][0] = NAN;
      d->xform_cam_Lab = cmsCreateTransform(d->input, input_format, Lab,
                                            TYPE_LabA_FLT, p->intent, 0);
      d->xform_cam_nrgb = cmsCreateTransform(d->input, input_format, d->nrgb,
                                             TYPE_RGBA_FLT, p->intent, 0);
      d->xform_nrgb_Lab = cmsCreateTransform(d->nrgb, TYPE_RGBA_FLT, Lab,
                                             TYPE_LabA_FLT, p->intent, 0);
    }
    else
    {
      float lutr[1], lutg[1], lutb[1];
      dt_colormatrix_t omat;
      dt_colorspaces_get_matrix_from_output_profile(d->nrgb, omat, lutr, lutg, lutb, 1);
      dt_colormatrix_mul(d->nmatrix, omat, d->cmatrix);
      dt_colorspaces_get_matrix_from_input_profile(d->nrgb, d->lmatrix,
                                                   lutr, lutg, lutb, 1);
    }
  }
  else
  {
    // default mode: unbound processing
    if(dt_colorspaces_get_matrix_from_input_profile(d->input, d->cmatrix,
                                                    d->lut[0], d->lut[1], d->lut[2],
                                                    LUT_SAMPLES))
    {
      piece->process_cl_ready = 0;
      d->cmatrix[0][0] = NAN;
      d->xform_cam_Lab = cmsCreateTransform(d->input, input_format, Lab,
                                            TYPE_LabA_FLT, p->intent, 0);
    }
  }

  // we might have failed generating the clipping transformations, check that:
  if(d->nrgb && ((!d->xform_cam_nrgb && isnan(d->nmatrix[0][0]))
                 || (!d->xform_nrgb_Lab && isnan(d->lmatrix[0][0]))))
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

  // user selected a non-supported input profile, check that:
  if(!d->xform_cam_Lab && isnan(d->cmatrix[0][0]))
  {
    if(p->type == DT_COLORSPACE_FILE)
      dt_print(DT_DEBUG_ALWAYS, "[colorin] unsupported input profile `%s' has"
               " been replaced by linear Rec709 RGB!\n", p->filename);
    else
      dt_print(DT_DEBUG_ALWAYS, "[colorin] unsupported input profile has been"
               " replaced by linear Rec709 RGB!\n");
    dt_control_log(_("unsupported input profile has been replaced by linear Rec709 RGB!"));
    if(d->input && d->clear_input) dt_colorspaces_cleanup_profile(d->input);
    d->nrgb = NULL;
    d->input = dt_colorspaces_get_profile(DT_COLORSPACE_LIN_REC709, "",
                                          DT_PROFILE_DIRECTION_IN)->profile;
    d->clear_input = 0;
    if(dt_colorspaces_get_matrix_from_input_profile(d->input, d->cmatrix,
                                                    d->lut[0], d->lut[1], d->lut[2],
                                                    LUT_SAMPLES))
    {
      piece->process_cl_ready = 0;
      d->cmatrix[0][0] = NAN;
      d->xform_cam_Lab = cmsCreateTransform(d->input, TYPE_RGBA_FLT, Lab,
                                            TYPE_LabA_FLT, p->intent, 0);
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
      const float y[4] = { lerp_lut(d->lut[k], x[0]),
                           lerp_lut(d->lut[k], x[1]),
                           lerp_lut(d->lut[k], x[2]),
                           lerp_lut(d->lut[k], x[3]) };
      dt_iop_estimate_exp(x, y, 4, d->unbounded_coeffs[k]);
    }
    else
      d->unbounded_coeffs[k][0] = -1.0f;
  }

  // commit color profiles to pipeline
  dt_ioppr_set_pipe_work_profile_info(self->dev, piece->pipe, d->type_work,
                                      d->filename_work, DT_INTENT_PERCEPTUAL);
  dt_ioppr_set_pipe_input_profile_info(self->dev, piece->pipe, d->type,
                                       d->filename, p->intent, d->cmatrix);
}

void init_pipe(struct dt_iop_module_t *self,
               dt_dev_pixelpipe_t *pipe,
               dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_colorin_data_t));
  dt_iop_colorin_data_t *d = (dt_iop_colorin_data_t *)piece->data;
  d->input = NULL;
  d->nrgb = NULL;
  d->xform_cam_Lab = NULL;
  d->xform_cam_nrgb = NULL;
  d->xform_nrgb_Lab = NULL;
}

void cleanup_pipe(struct dt_iop_module_t *self,
                  dt_dev_pixelpipe_t *pipe,
                  dt_dev_pixelpipe_iop_t *piece)
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
  dt_iop_colorin_gui_data_t *g = (dt_iop_colorin_gui_data_t *)self->gui_data;
  dt_iop_colorin_params_t *p = (dt_iop_colorin_params_t *)self->params;

  dt_bauhaus_combobox_set(g->clipping_combobox, p->normalize);

  // working profile
  int idx = -1;
  for(const GList *prof = darktable.color_profiles->profiles;
      prof;
      prof = g_list_next(prof))
  {
    dt_colorspaces_color_profile_t *pp = (dt_colorspaces_color_profile_t *)prof->data;
    if(pp->work_pos > -1
       && pp->type == p->type_work
       && (pp->type != DT_COLORSPACE_FILE
           || dt_colorspaces_is_profile_equal(pp->filename, p->filename_work)))
    {
      idx = pp->work_pos;
      break;
    }
  }

  if(idx < 0)
  {
    idx = 0;
    dt_print(DT_DEBUG_ALWAYS, "[colorin] could not find requested working profile `%s'!\n",
             dt_colorspaces_get_name(p->type_work, p->filename_work));
  }
  dt_bauhaus_combobox_set(g->work_combobox, idx);

  for(const GList *prof = g->image_profiles; prof; prof = g_list_next(prof))
  {
    dt_colorspaces_color_profile_t *pp = (dt_colorspaces_color_profile_t *)prof->data;
    if(pp->type == p->type
       && (pp->type != DT_COLORSPACE_FILE
           || dt_colorspaces_is_profile_equal(pp->filename, p->filename)))
    {
      dt_bauhaus_combobox_set(g->profile_combobox, pp->in_pos);
      return;
    }
  }

  for(const GList *prof = darktable.color_profiles->profiles;
      prof;
      prof = g_list_next(prof))
  {
    dt_colorspaces_color_profile_t *pp = (dt_colorspaces_color_profile_t *)prof->data;
    if(pp->in_pos > -1
       && pp->type == p->type
       && (pp->type != DT_COLORSPACE_FILE
           || dt_colorspaces_is_profile_equal(pp->filename, p->filename)))
    {
      dt_bauhaus_combobox_set(g->profile_combobox, pp->in_pos + g->n_image_profiles);
      return;
    }
  }
  dt_bauhaus_combobox_set(g->profile_combobox, 0);

  if(p->type != DT_COLORSPACE_ENHANCED_MATRIX)
    dt_print(DT_DEBUG_ALWAYS, "[colorin] could not find requested profile `%s'!\n",
             dt_colorspaces_get_name(p->type, p->filename));
}

// FIXME: update the gui when we add/remove the eprofile or ematrix
void reload_defaults(dt_iop_module_t *module)
{
  module->default_enabled = 1;
  module->hide_enable_button = 1;

  dt_iop_colorin_params_t *d = module->default_params;

  dt_colorspaces_color_profile_type_t color_profile = DT_COLORSPACE_NONE;

  // some file formats like jpeg can have an embedded color profile
  // currently we only support jpeg, j2k, tiff, png, avif, and heif
  dt_image_t *img = dt_image_cache_get(darktable.image_cache,
                                       module->dev->image_storage.id, 'w');

  if(!img->profile)
  {
    // the image has not a profile inited
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
        color_profile = (img->profile_size > 0)
          ? DT_COLORSPACE_EMBEDDED_ICC
          : DT_COLORSPACE_NONE;
      }
    }
    else if(!strcmp(ext, "pfm"))
    {
      // PFM have no embedded color profile nor ICC tag, we can't know the color space
      // but we can assume the are linear since it's a floating point format
      color_profile = DT_COLORSPACE_LIN_REC709;
    }
#ifdef HAVE_OPENJPEG
    else if(!strcmp(ext, "jp2")
            || !strcmp(ext, "j2k")
            || !strcmp(ext, "j2c")
            || !strcmp(ext, "jpc"))
    {
      img->profile_size = dt_imageio_j2k_read_profile(filename, &img->profile);
      color_profile = (img->profile_size > 0)
        ? DT_COLORSPACE_EMBEDDED_ICC
        : DT_COLORSPACE_NONE;
    }
#endif
    // the ldr test just checks for magics in the file header
    else if((!strcmp(ext, "tif") || !strcmp(ext, "tiff")) && dt_imageio_is_ldr(filename))
    {
      img->profile_size = dt_imageio_tiff_read_profile(filename, &img->profile);
      color_profile = (img->profile_size > 0)
        ? DT_COLORSPACE_EMBEDDED_ICC
        : DT_COLORSPACE_NONE;
    }
    else if(!strcmp(ext, "png"))
    {
      dt_colorspaces_cicp_t cicp;
      img->profile_size = dt_imageio_png_read_profile(filename, &img->profile, &cicp);
      /* PNG spec says try the cICP chunk first, but rather than
       * ignoring, we also try any ICC profile present if CICP combo
       * is unsupported */
      color_profile = dt_colorspaces_cicp_to_type(&cicp, filename);
      if(color_profile == DT_COLORSPACE_NONE)
        color_profile = (img->profile_size > 0)
          ? DT_COLORSPACE_EMBEDDED_ICC
          : DT_COLORSPACE_NONE;
    }
#ifdef HAVE_LIBAVIF
    else if(!strcmp(ext, "avif"))
    {
      dt_colorspaces_cicp_t cicp;
      img->profile_size = dt_imageio_avif_read_profile(filename, &img->profile, &cicp);
      /* AVIF spec gives priority to ICC profile over CICP; only one
       * valid kind is returned above anyway */
      color_profile
          = (img->profile_size > 0)
        ? DT_COLORSPACE_EMBEDDED_ICC
        : dt_colorspaces_cicp_to_type(&cicp, filename);
    }
#endif
#ifdef HAVE_LIBHEIF
    else if(!strcmp(ext, "heif")
         || !strcmp(ext, "heic")
         || !strcmp(ext, "hif")
  #ifndef HAVE_LIBAVIF
         || !strcmp(ext, "avif")
  #endif
         )
    {
      dt_colorspaces_cicp_t cicp;
      img->profile_size = dt_imageio_heif_read_profile(filename, &img->profile, &cicp);
      /* HEIF spec gives priority to ICC profile over CICP; only one valid kind is returned above anyway */
      color_profile
          = (img->profile_size > 0)
        ? DT_COLORSPACE_EMBEDDED_ICC
        : dt_colorspaces_cicp_to_type(&cicp, filename);
    }
#endif
    g_free(ext);
  }
  else
  {
    // there is an inited embedded profile
    color_profile = DT_COLORSPACE_EMBEDDED_ICC;
  }


  if(color_profile != DT_COLORSPACE_NONE)
    d->type = color_profile;
  else if(img->flags & DT_IMAGE_4BAYER) // 4Bayer images have been pre-converted to rec2020
    d->type = DT_COLORSPACE_LIN_REC2020;
  else if(dt_image_is_monochrome(img))
    d->type = DT_COLORSPACE_LIN_REC709;
  else if(img->colorspace == DT_IMAGE_COLORSPACE_SRGB)
    d->type = DT_COLORSPACE_SRGB;
  else if(img->colorspace == DT_IMAGE_COLORSPACE_ADOBE_RGB)
    d->type = DT_COLORSPACE_ADOBERGB;
  else if(dt_image_is_ldr(img))
    d->type = DT_COLORSPACE_SRGB;
  else if(!isnan(img->d65_color_matrix[0])) // image is DNG, EXR, or RGBE
    d->type = DT_COLORSPACE_EMBEDDED_MATRIX;
  else if(dt_image_is_matrix_correction_supported(img)) // image is raw
    d->type = DT_COLORSPACE_STANDARD_MATRIX;
  else if(dt_image_is_hdr(img)) // image is 32 bit float, most likely
                                // linear space, best guess is Rec709
    d->type = DT_COLORSPACE_LIN_REC709;
  else // no ICC tag nor colorprofile was found - ICC spec says untagged files are sRGB
    d->type = DT_COLORSPACE_SRGB;

  dt_image_cache_write_release(darktable.image_cache, img, DT_IMAGE_CACHE_RELAXED);

  update_profile_list(module);
}

static void update_profile_list(dt_iop_module_t *self)
{
  dt_iop_colorin_gui_data_t *g = (dt_iop_colorin_gui_data_t *)self->gui_data;

  if(!g) return;

  // clear and refill the image profile list
  g_list_free_full(g->image_profiles, free);
  g->image_profiles = NULL;
  g->n_image_profiles = 0;

  int pos = -1;
  // some file formats like jpeg can have an embedded color profile
  // currently we only support jpeg, j2k, tiff and png
  const dt_image_t *cimg =
    dt_image_cache_get(darktable.image_cache, self->dev->image_storage.id, 'r');
  if(cimg->profile)
  {
    dt_colorspaces_color_profile_t *prof
        = (dt_colorspaces_color_profile_t *)calloc(1, sizeof(dt_colorspaces_color_profile_t));
    g_strlcpy(prof->name, dt_colorspaces_get_name(DT_COLORSPACE_EMBEDDED_ICC, ""),
              sizeof(prof->name));
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
    g_strlcpy(prof->name, dt_colorspaces_get_name(DT_COLORSPACE_EMBEDDED_MATRIX, ""),
              sizeof(prof->name));
    prof->type = DT_COLORSPACE_EMBEDDED_MATRIX;
    g->image_profiles = g_list_append(g->image_profiles, prof);
    prof->in_pos = ++pos;
  }

  if(!isnan(self->dev->image_storage.adobe_XYZ_to_CAM[0][0])
     && !(self->dev->image_storage.flags & DT_IMAGE_4BAYER))
  {
    dt_colorspaces_color_profile_t *prof
        = (dt_colorspaces_color_profile_t *)calloc(1, sizeof(dt_colorspaces_color_profile_t));
    g_strlcpy(prof->name, dt_colorspaces_get_name(DT_COLORSPACE_STANDARD_MATRIX, ""),
              sizeof(prof->name));
    prof->type = DT_COLORSPACE_STANDARD_MATRIX;
    g->image_profiles = g_list_append(g->image_profiles, prof);
    prof->in_pos = ++pos;
  }

  // darktable built-in, if applicable
  for(int k = 0; k < dt_profiled_colormatrix_cnt; k++)
  {
    if(!strcasecmp(self->dev->image_storage.camera_makermodel,
                   dt_profiled_colormatrices[k].makermodel))
    {
      dt_colorspaces_color_profile_t *prof
          = (dt_colorspaces_color_profile_t *)calloc(1, sizeof(dt_colorspaces_color_profile_t));
      g_strlcpy(prof->name, dt_colorspaces_get_name(DT_COLORSPACE_ENHANCED_MATRIX, ""),
                sizeof(prof->name));
      prof->type = DT_COLORSPACE_ENHANCED_MATRIX;
      g->image_profiles = g_list_append(g->image_profiles, prof);
      prof->in_pos = ++pos;
      break;
    }
  }

  // darktable vendor matrix, if applicable
  for(int k = 0; k < dt_vendor_colormatrix_cnt; k++)
  {
    if(!strcmp(self->dev->image_storage.camera_makermodel,
               dt_vendor_colormatrices[k].makermodel))
    {
      dt_colorspaces_color_profile_t *prof
          = (dt_colorspaces_color_profile_t *)calloc(1, sizeof(dt_colorspaces_color_profile_t));
      g_strlcpy(prof->name, dt_colorspaces_get_name(DT_COLORSPACE_VENDOR_MATRIX, ""),
                sizeof(prof->name));
      prof->type = DT_COLORSPACE_VENDOR_MATRIX;
      g->image_profiles = g_list_append(g->image_profiles, prof);
      prof->in_pos = ++pos;
      break;
    }
  }

  // darktable alternate matrix, if applicable
  for(int k = 0; k < dt_alternate_colormatrix_cnt; k++)
  {
    if(!strcmp(self->dev->image_storage.camera_makermodel,
               dt_alternate_colormatrices[k].makermodel))
    {
      dt_colorspaces_color_profile_t *prof
          = (dt_colorspaces_color_profile_t *)calloc(1, sizeof(dt_colorspaces_color_profile_t));
      g_strlcpy(prof->name, dt_colorspaces_get_name(DT_COLORSPACE_ALTERNATE_MATRIX, ""),
                sizeof(prof->name));
      prof->type = DT_COLORSPACE_ALTERNATE_MATRIX;
      g->image_profiles = g_list_append(g->image_profiles, prof);
      prof->in_pos = ++pos;
      break;
    }
  }

  g->n_image_profiles = pos + 1;

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

  // working profile
  dt_bauhaus_combobox_clear(g->work_combobox);

  for(GList *l = darktable.color_profiles->profiles; l; l = g_list_next(l))
  {
    dt_colorspaces_color_profile_t *prof = (dt_colorspaces_color_profile_t *)l->data;
    if(prof->work_pos > -1) dt_bauhaus_combobox_add(g->work_combobox, prof->name);
  }
}

void gui_init(struct dt_iop_module_t *self)
{
  // pthread_mutex_lock(&darktable.plugin_threadsafe);
  dt_iop_colorin_gui_data_t *g = IOP_GUI_ALLOC(colorin);

  g->image_profiles = NULL;

  char datadir[PATH_MAX] = { 0 };
  char confdir[PATH_MAX] = { 0 };
  dt_loc_get_datadir(datadir, sizeof(datadir));
  dt_loc_get_user_config_dir(confdir, sizeof(confdir));

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  g->profile_combobox = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->profile_combobox, NULL, N_("input profile"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->profile_combobox, TRUE, TRUE, 0);

  g->work_combobox = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->work_combobox, NULL, N_("working profile"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->work_combobox, TRUE, TRUE, 0);

  dt_bauhaus_combobox_set(g->profile_combobox, 0);
  {
    char *system_profile_dir = g_build_filename(datadir, "color", "in", NULL);
    char *user_profile_dir = g_build_filename(confdir, "color", "in", NULL);
    char *tooltip = g_strdup_printf(_("ICC profiles in %s or %s"),
                                    user_profile_dir, system_profile_dir);
    gtk_widget_set_tooltip_text(g->profile_combobox, tooltip);
    g_free(system_profile_dir);
    g_free(user_profile_dir);
    g_free(tooltip);
  }

  dt_bauhaus_combobox_set(g->work_combobox, 0);
  {
    char *system_profile_dir = g_build_filename(datadir, "color", "out", NULL);
    char *user_profile_dir = g_build_filename(confdir, "color", "out", NULL);
    char *tooltip = g_strdup_printf(_("ICC profiles in %s or %s"),
                                    user_profile_dir, system_profile_dir);
    gtk_widget_set_tooltip_text(g->work_combobox, tooltip);
    g_free(system_profile_dir);
    g_free(user_profile_dir);
    g_free(tooltip);
  }

  g_signal_connect(G_OBJECT(g->profile_combobox), "value-changed",
                   G_CALLBACK(profile_changed), (gpointer)self);
  g_signal_connect(G_OBJECT(g->work_combobox), "value-changed",
                   G_CALLBACK(workicc_changed), (gpointer)self);

  g->clipping_combobox = dt_bauhaus_combobox_from_params(self, "normalize");
  gtk_widget_set_tooltip_text(g->clipping_combobox,
                              _("confine Lab values to gamut of RGB color space"));
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  dt_iop_colorin_gui_data_t *g = (dt_iop_colorin_gui_data_t *)self->gui_data;
  while(g->image_profiles)
  {
    g_free(g->image_profiles->data);
    g->image_profiles = g_list_delete_link(g->image_profiles, g->image_profiles);
  }

  IOP_GUI_FREE;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
