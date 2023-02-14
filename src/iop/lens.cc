/*
    This file is part of darktable,
    Copyright (C) 2019-2023 darktable developers.

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

extern "C" {

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "bauhaus/bauhaus.h"
#include "common/interpolation.h"
#include "common/file_location.h"
#include "common/imagebuf.h"
#include "common/opencl.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "develop/tiling.h"
#include "dtgtk/button.h"
#include "dtgtk/resetlabel.h"
#include "gui/accelerators.h"
#include "gui/draw.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"
#include <assert.h>
#include <ctype.h>
#include <gtk/gtk.h>
#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
}

#include <lensfun.h>

#define MAXKNOTS 16

extern "C" {

#if LF_VERSION < ((0 << 24) | (2 << 16) | (9 << 8) | 0)
#define LF_SEARCH_SORT_AND_UNIQUIFY 2
#endif

#if LF_VERSION == ((0 << 24) | (3 << 16) | (95 << 8) | 0)
#define LF_0395
#error lensfun 0.3.95 is not supported since its API is not backward compatible with lensfun stable release.
#endif

DT_MODULE_INTROSPECTION(7, dt_iop_lens_params_t)

typedef enum dt_iop_lens_method_t
{
  DT_IOP_LENS_METHOD_EMBEDDED_METADATA = 0, // $DESCRIPTION: "embedded metadata"
  DT_IOP_LENS_METHOD_LENSFUN = 1 // $DESCRIPTION: "lensfun database"
} dt_iop_lens_method_t;

typedef enum dt_iop_lens_modify_flag_t
{
  DT_IOP_LENS_MODIFY_FLAG_TCA = 1,
  DT_IOP_LENS_MODIFY_FLAG_VIGNETTING = 1 << 1,
  DT_IOP_LENS_MODIFY_FLAG_DISTORTION = 1 << 2
} dt_iop_lens_modify_flag_t;

typedef enum dt_iop_lens_modflag_t
{
  DT_IOP_LENS_MODFLAG_NONE = 0,
  DT_IOP_LENS_MODFLAG_ALL = DT_IOP_LENS_MODIFY_FLAG_DISTORTION | DT_IOP_LENS_MODIFY_FLAG_TCA | DT_IOP_LENS_MODIFY_FLAG_VIGNETTING,
  DT_IOP_LENS_MODFLAG_DIST_TCA = DT_IOP_LENS_MODIFY_FLAG_DISTORTION | DT_IOP_LENS_MODIFY_FLAG_TCA,
  DT_IOP_LENS_MODFLAG_DIST_VIGN = DT_IOP_LENS_MODIFY_FLAG_DISTORTION | DT_IOP_LENS_MODIFY_FLAG_VIGNETTING,
  DT_IOP_LENS_MODFLAG_TCA_VIGN = DT_IOP_LENS_MODIFY_FLAG_TCA | DT_IOP_LENS_MODIFY_FLAG_VIGNETTING,
  DT_IOP_LENS_MODFLAG_DIST = DT_IOP_LENS_MODIFY_FLAG_DISTORTION,
  DT_IOP_LENS_MODFLAG_TCA = DT_IOP_LENS_MODIFY_FLAG_TCA,
  DT_IOP_LENS_MODFLAG_VIGN = DT_IOP_LENS_MODIFY_FLAG_VIGNETTING,
} dt_iop_lens_modflag_t;

typedef enum dt_iop_lens_lenstype_t
{
  DT_IOP_LENS_LENSTYPE_UNKNOWN = 0,
  DT_IOP_LENS_LENSTYPE_RECTILINEAR = 1,
  DT_IOP_LENS_LENSTYPE_FISHEYE = 2,
  DT_IOP_LENS_LENSTYPE_PANORAMIC = 3,
  DT_IOP_LENS_LENSTYPE_EQUIRECTANGULAR = 4,
  DT_IOP_LENS_LENSTYPE_FISHEYE_ORTHOGRAPHIC = 5,
  DT_IOP_LENS_LENSTYPE_FISHEYE_STEREOGRAPHIC = 6,
  DT_IOP_LENS_LENSTYPE_FISHEYE_EQUISOLID = 7,
  DT_IOP_LENS_LENSTYPE_FISHEYE_THOBY = 8
} dt_iop_lens_lenstype_t;

typedef struct dt_iop_lens_params_t
{
  dt_iop_lens_method_t method; // $DEFAULT: DT_IOP_LENS_METHOD_LENSFUN $DESCRIPTION: "correction method"
  int modify_flags; // $DEFAULT: DT_IOP_LENS_MODFLAG_ALL $DESCRIPTION: "corrections"

  // NOTE: the options for lensfun and metadata correction methods should be
  // kept separate since also if similar their value have different effects.
  // additionally this could permit to switch between the methods.
  // the unique parameter in common is modify_flags

  // lensfun method parameters
  int inverse; // $MIN: 0 $MAX: 1 $DEFAULT: 0 $DESCRIPTION: "mode"
  float scale; // $MIN: 0.1 $MAX: 2.0 $DEFAULT: 1.0
  float crop;
  float focal;
  float aperture;
  float distance;
  int target_geom; // $DEFAULT: DT_IOP_LENS_LENSTYPE_RECTILINEAR $DESCRIPTION: "geometry"
  char camera[128];
  char lens[128];
  gboolean tca_override; // $DEFAULT: FALSE $DESCRIPTION: "TCA overwrite"
  float tca_r; // $MIN: 0.99 $MAX: 1.01 $DEFAULT: 1.0 $DESCRIPTION: "TCA red"
  float tca_b; // $MIN: 0.99 $MAX: 1.01 $DEFAULT: 1.0 $DESCRIPTION: "TCA blue"

  // embedded metadata method parameters
  float cor_dist_ft;  // $DEFAULT: 1 $MIN: 0 $MAX: 2 $DESCRIPTION: "distortion fine-tune"
  float cor_vig_ft;   // $DEFAULT: 1 $MIN: 0 $MAX: 2 $DESCRIPTION: "vignetting fine-tune"
  // TODO should be possible to also add tca fine tune modifications

  float cor_scale;  // $DEFAULT: 1 $MIN: 0.9 $MAX: 1.1 $DESCRIPTION: "scale fine-tune"
} dt_iop_lens_params_t;

typedef struct dt_iop_lens_gui_modifier_t
{
  char name[80];
  int pos; // position in combo box
  int modflag;
} dt_iop_lens_gui_modifier_t;

typedef struct dt_iop_lens_gui_data_t
{
  GtkWidget *lens_param_box;
  GtkWidget *cbe[3];
  GtkWidget *camera_model;
  GtkMenu *camera_menu;
  GtkWidget *lens_model;
  GtkMenu *lens_menu;
  GtkWidget *methods_selector, *methods;
  GtkWidget *modflags, *target_geom, *reverse, *tca_override, *tca_r, *tca_b, *scale;
  GtkWidget *find_lens_button;
  GtkWidget *find_camera_button;
  GtkWidget *cor_dist_ft, *cor_vig_ft, *cor_scale;
  GList *modifiers;
  GtkLabel *message;
  int corrections_done;
  gboolean lensfun_trouble;
  const lfCamera *camera;
} dt_iop_lens_gui_data_t;


typedef struct dt_iop_lens_global_data_t
{
  int kernel_lens_distort_bilinear;
  int kernel_lens_distort_bicubic;
  int kernel_lens_distort_lanczos2;
  int kernel_lens_distort_lanczos3;
  int kernel_lens_vignette;
  lfDatabase *db;
} dt_iop_lens_global_data_t;

typedef struct dt_iop_lens_data_t
{
  int method;
  int modify_flags;

  /* lensfun data */
  lfLens *lens;
  int inverse;
  float scale;
  float crop;
  float focal;
  float aperture;
  float distance;
  lfLensType target_geom;
  gboolean do_nan_checks;
  gboolean tca_override;
  lfLensCalibTCA custom_tca;

  /* embedded metadata data */
  float cor_dist_ft;
  float cor_vig_ft;
  float scale_md;
  int nc;
  float knots[MAXKNOTS], cor_rgb[3][MAXKNOTS], vig[MAXKNOTS];
} dt_iop_lens_data_t;


const char *name()
{
  return _("lens correction");
}

const char *aliases()
{
  return _("vignette|chromatic aberrations|distortion");
}

const char **description(struct dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("correct lenses optical flaws"),
                                      _("corrective"),
                                      _("linear, RGB, scene-referred"),
                                      _("geometric and reconstruction, RGB"),
                                      _("linear, RGB, scene-referred"));
}


int default_group()
{
  return IOP_GROUP_CORRECT | IOP_GROUP_TECHNICAL;
}

int operation_tags()
{
  return IOP_TAG_DISTORT;
}

int flags()
{
  return IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_TILING_FULL_ROI
    | IOP_FLAGS_UNSAFE_COPY | IOP_FLAGS_GUIDES_WIDGET;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

static gboolean _have_embedded_metadata(dt_iop_module_t *self)
{
  return self->dev->image_storage.exif_correction_type;
}

static lfLensType _lenstype_to_lensfun_lenstype(int lt)
{
  switch(lt)
  {
    case DT_IOP_LENS_LENSTYPE_RECTILINEAR:
      return LF_RECTILINEAR;
    case DT_IOP_LENS_LENSTYPE_FISHEYE:
      return LF_FISHEYE;
    case DT_IOP_LENS_LENSTYPE_PANORAMIC:
      return LF_PANORAMIC;
    case DT_IOP_LENS_LENSTYPE_EQUIRECTANGULAR:
      return LF_EQUIRECTANGULAR;
    case DT_IOP_LENS_LENSTYPE_FISHEYE_ORTHOGRAPHIC:
      return LF_FISHEYE_ORTHOGRAPHIC;
    case DT_IOP_LENS_LENSTYPE_FISHEYE_STEREOGRAPHIC:
      return LF_FISHEYE_STEREOGRAPHIC;
    case DT_IOP_LENS_LENSTYPE_FISHEYE_EQUISOLID:
      return LF_FISHEYE_EQUISOLID;
    case DT_IOP_LENS_LENSTYPE_FISHEYE_THOBY:
      return LF_FISHEYE_THOBY;
    default:
      return LF_UNKNOWN;
  }
}

static int _modflags_to_lensfun_mods(int modify_flags)
{
  int mods = LF_MODIFY_GEOMETRY | LF_MODIFY_SCALE;

  mods |= modify_flags & DT_IOP_LENS_MODIFY_FLAG_DISTORTION ? LF_MODIFY_DISTORTION : 0;
  mods |= modify_flags & DT_IOP_LENS_MODIFY_FLAG_VIGNETTING ? LF_MODIFY_VIGNETTING : 0;
  mods |= modify_flags & DT_IOP_LENS_MODIFY_FLAG_TCA        ? LF_MODIFY_TCA        : 0;

  return mods;
}

static int _modflags_from_lensfun_mods(int lf_mods)
{
  int mods = 0;

  mods |= lf_mods & LF_MODIFY_DISTORTION ? DT_IOP_LENS_MODIFY_FLAG_DISTORTION : 0;
  mods |= lf_mods & LF_MODIFY_VIGNETTING ? DT_IOP_LENS_MODIFY_FLAG_VIGNETTING : 0;
  mods |= lf_mods & LF_MODIFY_TCA        ? DT_IOP_LENS_MODIFY_FLAG_TCA        : 0;

  return mods;
}

static int _lenstype_from_lensfun_lenstype(lfLensType lt)
{
  switch(lt)
  {
    case LF_RECTILINEAR:
      return DT_IOP_LENS_LENSTYPE_RECTILINEAR;
    case LF_FISHEYE:
      return DT_IOP_LENS_LENSTYPE_FISHEYE;
    case LF_PANORAMIC:
      return DT_IOP_LENS_LENSTYPE_PANORAMIC;
    case LF_EQUIRECTANGULAR:
      return DT_IOP_LENS_LENSTYPE_EQUIRECTANGULAR;
    case LF_FISHEYE_ORTHOGRAPHIC:
      return DT_IOP_LENS_LENSTYPE_FISHEYE_ORTHOGRAPHIC;
    case LF_FISHEYE_STEREOGRAPHIC:
      return DT_IOP_LENS_LENSTYPE_FISHEYE_STEREOGRAPHIC;
    case LF_FISHEYE_EQUISOLID:
      return DT_IOP_LENS_LENSTYPE_FISHEYE_EQUISOLID;
    case LF_FISHEYE_THOBY:
      return DT_IOP_LENS_LENSTYPE_FISHEYE_THOBY;
    default:
      return DT_IOP_LENS_LENSTYPE_UNKNOWN;
  }
}

int legacy_params(
        dt_iop_module_t *self,
        const void *const old_params,
        const int old_version,
        void *new_params,
        const int new_version)
{
  if(old_version == 2 && new_version == 7)
  {
    // legacy params of version 2; version 1 comes from ancient times and seems to be forgotten by now
    typedef struct
    {
      int modify_flags;
      int inverse;
      float scale;
      float crop;
      float focal;
      float aperture;
      float distance;
      lfLensType target_geom;
      char camera[52];
      char lens[52];
      int tca_override;
      float tca_r, tca_b;
    } dt_iop_lens_params_v2_t;

    const dt_iop_lens_params_v2_t *o = (dt_iop_lens_params_v2_t *)old_params;
    dt_iop_lens_params_t *n = (dt_iop_lens_params_t *)new_params;
    dt_iop_lens_params_t *d = (dt_iop_lens_params_t *)self->default_params;

    *n = *d; // start with a fresh copy of default parameters

    n->modify_flags = _modflags_from_lensfun_mods(o->modify_flags);
    n->inverse = o->inverse;
    n->scale = o->scale;
    n->crop = o->crop;
    n->focal = o->focal;
    n->aperture = o->aperture;
    n->distance = o->distance;
    n->target_geom = _lenstype_from_lensfun_lenstype(o->target_geom);
    n->tca_override = o->tca_override;
    g_strlcpy(n->camera, o->camera, sizeof(n->camera));
    g_strlcpy(n->lens, o->lens, sizeof(n->lens));

    // old versions had R and B swapped
    n->tca_r = o->tca_b;
    n->tca_b = o->tca_r;

    // new in v6
    n->method = DT_IOP_LENS_METHOD_LENSFUN;
    n->cor_dist_ft = 1.f;
    n->cor_vig_ft = 1.f;

    // new in v7
    n->cor_scale = 0.0f;

    return 0;
  }

  if(old_version == 3 && new_version == 7)
  {
    typedef struct
    {
      int modify_flags;
      int inverse;
      float scale;
      float crop;
      float focal;
      float aperture;
      float distance;
      lfLensType target_geom;
      char camera[128];
      char lens[128];
      int tca_override;
      float tca_r, tca_b;
    } dt_iop_lens_params_v3_t;

    const dt_iop_lens_params_v3_t *o = (dt_iop_lens_params_v3_t *)old_params;
    dt_iop_lens_params_t *n = (dt_iop_lens_params_t *)new_params;
    dt_iop_lens_params_t *d = (dt_iop_lens_params_t *)self->default_params;

    *n = *d; // start with a fresh copy of default parameters

    n->modify_flags = _modflags_from_lensfun_mods(o->modify_flags);
    n->inverse = o->inverse;
    n->scale = o->scale;
    n->crop = o->crop;
    n->focal = o->focal;
    n->aperture = o->aperture;
    n->distance = o->distance;
    n->target_geom = _lenstype_from_lensfun_lenstype(o->target_geom);
    n->tca_override = o->tca_override;
    g_strlcpy(n->camera, o->camera, sizeof(n->camera));
    g_strlcpy(n->lens, o->lens, sizeof(n->lens));
    n->tca_r = o->tca_r;
    n->tca_b = o->tca_b;

    // new in v6
    n->method = DT_IOP_LENS_METHOD_LENSFUN;
    n->cor_dist_ft = 1.f;
    n->cor_vig_ft = 1.f;

    // new in v7
    n->cor_scale = 0.0f;

    return 0;
  }

  if(old_version == 4 && new_version == 7)
  {
    typedef struct
    {
      int modify_flags;
      int inverse;
      float scale;
      float crop;
      float focal;
      float aperture;
      float distance;
      lfLensType target_geom;
      char camera[128];
      char lens[128];
      int tca_override;
      float tca_r, tca_b;
      int modified;
    } dt_iop_lens_params_v4_t;

    const dt_iop_lens_params_v4_t *o = (dt_iop_lens_params_v4_t *)old_params;
    dt_iop_lens_params_t *n = (dt_iop_lens_params_t *)new_params;
    dt_iop_lens_params_t *d = (dt_iop_lens_params_t *)self->default_params;

    *n = *d; // start with a fresh copy of default parameters

    n->modify_flags = _modflags_from_lensfun_mods(o->modify_flags);
    n->inverse = o->inverse;
    n->scale = o->scale;
    n->crop = o->crop;
    n->focal = o->focal;
    n->aperture = o->aperture;
    n->distance = o->distance;
    n->target_geom = _lenstype_from_lensfun_lenstype(o->target_geom);
    n->tca_override = o->tca_override;
    g_strlcpy(n->camera, o->camera, sizeof(n->camera));
    g_strlcpy(n->lens, o->lens, sizeof(n->lens));
    n->tca_r = o->tca_r;
    n->tca_b = o->tca_b;

    // new in v6
    n->method = DT_IOP_LENS_METHOD_LENSFUN;
    n->cor_dist_ft = 1.f;
    n->cor_vig_ft = 1.f;

    // new in v7
    n->cor_scale = 0.0f;

    return o->modified == 0 ? -1 : 0;
  }

  if(old_version == 5 && new_version == 7)
  {
    typedef struct
    {
      int modify_flags;
      int inverse;
      float scale;
      float crop;
      float focal;
      float aperture;
      float distance;
      lfLensType target_geom;
      char camera[128];
      char lens[128];
      int tca_override;
      float tca_r, tca_b;
      int modified;
    } dt_iop_lens_params_v5_t;

    const dt_iop_lens_params_v5_t *o = (dt_iop_lens_params_v5_t *)old_params;
    dt_iop_lens_params_t *n = (dt_iop_lens_params_t *)new_params;
    dt_iop_lens_params_t *d = (dt_iop_lens_params_t *)self->default_params;

    *n = *d; // start with a fresh copy of default parameters

    // The unique method in previous versions was lensfun
    n->modify_flags = _modflags_from_lensfun_mods(o->modify_flags);
    n->inverse = o->inverse;
    n->scale = o->scale;
    n->crop = o->crop;
    n->focal = o->focal;
    n->aperture = o->aperture;
    n->distance = o->distance;
    n->target_geom = _lenstype_from_lensfun_lenstype(o->target_geom);
    n->tca_override = o->tca_override;
    g_strlcpy(n->camera, o->camera, sizeof(n->camera));
    g_strlcpy(n->lens, o->lens, sizeof(n->lens));
    n->tca_r = o->tca_r;
    n->tca_b = o->tca_b;

    // new in v6
    n->method = DT_IOP_LENS_METHOD_LENSFUN;
    n->cor_dist_ft = 1.f;
    n->cor_vig_ft = 1.f;

    // new in v7
    n->cor_scale = 0.0f;

    return o->modified == 0 ? -1 : 0;
  }

  if(old_version == 6 && new_version == 7)
  {
    typedef struct
    {
      dt_iop_lens_method_t method;
      int modify_flags;
      int inverse;
      float scale;
      float crop;
      float focal;
      float aperture;
      float distance;
      int target_geom;
      char camera[128];
      char lens[128];
      gboolean tca_override;
      float tca_r;
      float tca_b;
      float cor_dist_ft;
      float cor_vig_ft;
      int modified;
    } dt_iop_lens_params_v6_t;


    const dt_iop_lens_params_v6_t *o = (dt_iop_lens_params_v6_t *)old_params;
    dt_iop_lens_params_t *n = (dt_iop_lens_params_t *)new_params;
    dt_iop_lens_params_t *d = (dt_iop_lens_params_t *)self->default_params;

    *n = *d; // start with a fresh copy of default parameters

    // The unique method in previous versions was lensfun
    n->method = o->method;
    n->modify_flags = o->modify_flags;
    n->inverse = o->inverse;
    n->scale = o->scale;
    n->crop = o->crop;
    n->focal = o->focal;
    n->aperture = o->aperture;
    n->distance = o->distance;
    n->target_geom = o->target_geom;
    g_strlcpy(n->camera, o->camera, sizeof(n->camera));
    g_strlcpy(n->lens, o->lens, sizeof(n->lens));
    n->tca_override = o->tca_override;
    n->tca_r = o->tca_r;
    n->tca_b = o->tca_b;
    n->cor_dist_ft = o->cor_dist_ft;
    n->cor_vig_ft = o->cor_vig_ft;

    // new in v7
    n->cor_scale = 0.0f;

    return o->modified == 0 ? -1 : 0;
  }
  return 1;
}

/* lensfun processing start */
static lfModifier * _get_modifier(
        int *mods_done,
        int w,
        int h,
        const dt_iop_lens_data_t *d,
        int mods_filter, gboolean force_inverse)
{
  lfModifier *mod;

  const int mods = _modflags_to_lensfun_mods(d->modify_flags);
  const int mods_todo = mods & mods_filter;
  int mods_done_tmp = 0;

#ifdef LF_0395
  mod = new lfModifier(d->crop, w, h, LF_PF_F32, (force_inverse) ? !d->inverse : d->inverse);
  if(mods_todo & LF_MODIFY_DISTORTION)
    mods_done_tmp |= mod->EnableDistortionCorrection(d->lens, d->focal);
  if((mods_todo & LF_MODIFY_GEOMETRY) && (d->lens->Type != d->target_geom))
    mods_done_tmp |= mod->EnableProjectionTransform(d->lens, d->focal, d->target_geom);
  if((mods_todo & LF_MODIFY_SCALE) && (d->scale != 1.0))
    mods_done_tmp |= mod->EnableScaling(d->scale);
  if(mods_todo & LF_MODIFY_TCA)
  {
    if(d->tca_override) mods_done_tmp |= mod->EnableTCACorrection(d->custom_tca);
    else mods_done_tmp |= mod->EnableTCACorrection(d->lens, d->focal);
  }
  if(mods_todo & LF_MODIFY_VIGNETTING)
    mods_done_tmp |= mod->EnableVignettingCorrection(d->lens, d->focal, d->aperture, d->distance);
#else
  mod = new lfModifier(d->lens, d->crop, w, h);
  mods_done_tmp = mod->Initialize(d->lens, LF_PF_F32, d->focal, d->aperture, d->distance, d->scale, d->target_geom, mods_todo,
                                  (force_inverse) ? !d->inverse : d->inverse);
#endif

  if(mods_done) *mods_done = mods_done_tmp;
  return mod;
}

static float _get_autoscale_lf(
        dt_iop_module_t *self,
        dt_iop_lens_params_t *p,
        const lfCamera *camera)
{
  dt_iop_lens_global_data_t *gd = (dt_iop_lens_global_data_t *)self->global_data;
  lfDatabase *dt_iop_lensfun_db = (lfDatabase *)gd->db;
  float scale = 1.0;
  if(p->lens[0] != '\0')
  {
    dt_pthread_mutex_lock(&darktable.plugin_threadsafe);
    const lfLens **lenslist
        = dt_iop_lensfun_db->FindLenses(camera, NULL, p->lens, 0);
    if(lenslist)
    {
      const dt_image_t *img = &(self->dev->image_storage);

      // FIXME: get those from rawprepare IOP somehow !!!
      const int iwd = img->width - img->crop_x - img->crop_right,
                iht = img->height - img->crop_y - img->crop_bottom;

      // create dummy modifier
#if defined(__GNUC__) && (__GNUC__ > 7)
      const dt_iop_lens_data_t d =
        {
         .modify_flags = p->modify_flags,
         .lens         = (lfLens *)lenslist[0],
         .inverse      = p->inverse,
         .scale        = 1.0f,
         .crop         = p->crop,
         .focal        = p->focal,
         .aperture     = p->aperture,
         .distance     = p->distance,
         .target_geom  = _lenstype_to_lensfun_lenstype(p->target_geom),
         .custom_tca   = { .Model = LF_TCA_MODEL_NONE }
        };
#else
      // prior to GCC 8.x the / .custom_tca   = { .Model = ??? } / was not supported:
      //    sorry, unimplemented: non-trivial designated initializers not supported
      // ?? This code can be removed when GCC-7 is not used anymore.

      dt_iop_lens_data_t d;
      d.modify_flags     = p->modify_flags;
      d.lens             = (lfLens *)lenslist[0];
      d.inverse          = p->inverse;
      d.scale            = 1.0f;
      d.crop             = p->crop;
      d.focal            = p->focal;
      d.aperture         = p->aperture;
      d.distance         = p->distance;
      d.target_geom      = _lenstype_to_lensfun_lenstype(p->target_geom);
      d.custom_tca.Model = LF_TCA_MODEL_NONE;
#endif

      lfModifier *modifier = _get_modifier(NULL, iwd, iht, &d, LF_MODIFY_ALL, FALSE);

      scale = modifier->GetAutoScale(p->inverse);
      delete modifier;
    }
    lf_free(lenslist);
    dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);
  }
  return scale;
}

/* Why do we care about being a monochrome image or not?
 The lensfun library does not have an algorithm for distortion or tca correction specialized for monochrome images,
   the builtin correction works with subtle differences for the color channels leading to some colorizing of the images.
 How is this fixed here:
   Monochrome images (from pure monochrome cameras or cameras with the color filter removed from the sensor) have
   all three rgb colors set to the same value by the demosaicer.
   Looking through lensfun code & docs the ApplySubpixelGeometryDistortion algorithm makes assumptions from given
   coeffs how far data are displaced for the different wavelengths of light.
   As green / Y channel is the most centric i took that as the canonical value instead of taking the mean.
*/

static void _process_lf(
        dt_iop_module_t *self,
        dt_dev_pixelpipe_iop_t *piece,
        const void *const ivoid,
        void *const ovoid,
        const dt_iop_roi_t *const roi_in,
        const dt_iop_roi_t *const roi_out)
{
  const dt_iop_lens_data_t *const d = (dt_iop_lens_data_t *)piece->data;

  const int ch = piece->colors;
  const int ch_width = ch * roi_in->width;
  const int mask_display = piece->pipe->mask_display;

  const unsigned int pixelformat = ch == 3 ? LF_CR_3(RED, GREEN, BLUE) : LF_CR_4(RED, GREEN, BLUE, UNKNOWN);

  if(!d->lens || !d->lens->Maker || d->crop <= 0.0f)
  {
    dt_iop_image_copy_by_size((float*)ovoid, (float*)ivoid, roi_out->width, roi_out->height, ch);
    return;
  }

  const gboolean raw_monochrome = dt_image_is_monochrome(&self->dev->image_storage);
  const int used_lf_mask = (raw_monochrome) ? LF_MODIFY_ALL & ~LF_MODIFY_TCA : LF_MODIFY_ALL;

  const float orig_w = roi_in->scale * piece->buf_in.width;
  const float orig_h = roi_in->scale * piece->buf_in.height;

  dt_pthread_mutex_lock(&darktable.plugin_threadsafe);

  int modflags;
  const lfModifier *modifier = _get_modifier(&modflags, orig_w, orig_h, d, used_lf_mask, FALSE);

  dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);

  const struct dt_interpolation *const interpolation = dt_interpolation_new(DT_INTERPOLATION_USERPREF_WARP);

  if(d->inverse)
  {
    // reverse direction (useful for renderings)
    if(modflags & (LF_MODIFY_TCA | LF_MODIFY_DISTORTION | LF_MODIFY_GEOMETRY | LF_MODIFY_SCALE))
    {
      // acquire temp memory for distorted pixel coords
      const size_t bufsize = (size_t)roi_out->width * 2 * 3;

      size_t padded_bufsize;
      float *const buf = dt_alloc_perthread_float(bufsize, &padded_bufsize);

#ifdef _OPENMP
#pragma omp parallel for default(none) \
      dt_omp_firstprivate(padded_bufsize, ch, ch_width, d, interpolation, ivoid, mask_display, ovoid, roi_in, roi_out)	\
      dt_omp_sharedconst(buf, raw_monochrome) \
      shared(modifier) \
      schedule(static)
#endif
      for(int y = 0; y < roi_out->height; y++)
      {
        float *bufptr = (float*)dt_get_perthread(buf, padded_bufsize);
        modifier->ApplySubpixelGeometryDistortion(roi_out->x, roi_out->y + y, roi_out->width, 1, bufptr);

        // reverse transform the global coords from lf to our buffer
        float *out = ((float *)ovoid) + (size_t)y * roi_out->width * ch;
        for(int x = 0; x < roi_out->width; x++, bufptr += 6, out += ch)
        {
          for(int c = 0; c < 3; c++)
          {
            if(d->do_nan_checks && (!isfinite(bufptr[c * 2]) || !isfinite(bufptr[c * 2 + 1])))
            {
              out[c] = 0.0f;
              continue;
            }

            const float *const inptr = (const float *const)ivoid + (size_t)c;
            const float pi0 = fmaxf(fminf(bufptr[c * 2] - roi_in->x, roi_in->width - 1.0f), 0.0f);
            const float pi1 = fmaxf(fminf(bufptr[c * 2 + 1] - roi_in->y, roi_in->height - 1.0f), 0.0f);
            out[c] = dt_interpolation_compute_sample(interpolation, inptr, pi0, pi1, roi_in->width,
                                                     roi_in->height, ch, ch_width);
          }

          if(raw_monochrome) out[0] = out[2] = out[1];

          if(mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK)
          {
            if(d->do_nan_checks && (!isfinite(bufptr[2]) || !isfinite(bufptr[3])))
            {
              out[3] = 0.0f;
              continue;
            }

            // take green channel distortion also for alpha channel
            const float *const inptr = (const float *const)ivoid + (size_t)3;
            const float pi0 = fmaxf(fminf(bufptr[2] - roi_in->x, roi_in->width - 1.0f), 0.0f);
            const float pi1 = fmaxf(fminf(bufptr[3] - roi_in->y, roi_in->height - 1.0f), 0.0f);
            out[3] = dt_interpolation_compute_sample(interpolation, inptr, pi0, pi1, roi_in->width,
                                                     roi_in->height, ch, ch_width);
          }
        }
      }
      dt_free_align(buf);
    }
    else
    {
      dt_iop_image_copy_by_size((float*)ovoid, (float*)ivoid, roi_out->width, roi_out->height, ch);
    }

    if(modflags & LF_MODIFY_VIGNETTING)
    {
#ifdef _OPENMP
#pragma omp parallel for default(none) \
      dt_omp_firstprivate(ch, pixelformat, roi_out, ovoid) \
      shared(modifier) \
      schedule(static)
#endif
      for(int y = 0; y < roi_out->height; y++)
      {
        /* Colour correction: vignetting */
        // actually this way row stride does not matter.
        float *out = ((float *)ovoid) + (size_t)y * roi_out->width * ch;
        modifier->ApplyColorModification(out, roi_out->x, roi_out->y + y, roi_out->width, 1,
                                         pixelformat, ch * roi_out->width);
      }
    }
  }
  else // correct distortions:
  {
    // acquire temp memory for image buffer
    const size_t bufsize = (size_t)roi_in->width * roi_in->height * ch * sizeof(float);
    void *buf = dt_alloc_align(64, bufsize);
    memcpy(buf, ivoid, bufsize);

    if(modflags & LF_MODIFY_VIGNETTING)
    {
#ifdef _OPENMP
#pragma omp parallel for default(none) \
      dt_omp_firstprivate(ch, pixelformat, roi_in) \
      shared(buf, modifier) \
      schedule(static)
#endif
      for(int y = 0; y < roi_in->height; y++)
      {
        /* Colour correction: vignetting */
        // actually this way row stride does not matter.
        float *bufptr = ((float *)buf) + (size_t)ch * roi_in->width * y;
        modifier->ApplyColorModification(bufptr, roi_in->x, roi_in->y + y, roi_in->width, 1,
                                         pixelformat, ch * roi_in->width);
      }
    }

    if(modflags & (LF_MODIFY_TCA | LF_MODIFY_DISTORTION | LF_MODIFY_GEOMETRY | LF_MODIFY_SCALE))
    {
      // acquire temp memory for distorted pixel coords
      const size_t buf2size = (size_t)roi_out->width * 2 * 3;
      size_t padded_buf2size;
      float *const buf2 = dt_alloc_perthread_float(buf2size, &padded_buf2size);

#ifdef _OPENMP
#pragma omp parallel for default(none) \
      dt_omp_firstprivate(padded_buf2size, ch, ch_width, d, interpolation, mask_display, ovoid, roi_in, roi_out) \
      dt_omp_sharedconst(buf2, raw_monochrome) \
      shared(buf, modifier) \
      schedule(static)
#endif
      for(int y = 0; y < roi_out->height; y++)
      {
        float *buf2ptr = (float*)dt_get_perthread(buf2, padded_buf2size);
        modifier->ApplySubpixelGeometryDistortion(roi_out->x, roi_out->y + y, roi_out->width,
                                                  1, buf2ptr);
        // reverse transform the global coords from lf to our buffer
        float *out = ((float *)ovoid) + (size_t)y * roi_out->width * ch;
        for(int x = 0; x < roi_out->width; x++, buf2ptr += 6, out += ch)
        {
          for(int c = 0; c < 3; c++)
          {
            if(d->do_nan_checks && (!isfinite(buf2ptr[c * 2]) || !isfinite(buf2ptr[c * 2 + 1])))
            {
              out[c] = 0.0f;
              continue;
            }

            float *bufptr = ((float *)buf) + c;
            const float pi0 = fmaxf(fminf(buf2ptr[c * 2] - roi_in->x, roi_in->width - 1.0f), 0.0f);
            const float pi1 = fmaxf(fminf(buf2ptr[c * 2 + 1] - roi_in->y, roi_in->height - 1.0f), 0.0f);
            out[c] = dt_interpolation_compute_sample(interpolation, bufptr, pi0, pi1, roi_in->width,
                                                     roi_in->height, ch, ch_width);
          }
          if(raw_monochrome) out[0] = out[2] = out[1];
          if(mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK)
          {
            if(d->do_nan_checks && (!isfinite(buf2ptr[2]) || !isfinite(buf2ptr[3])))
            {
              out[3] = 0.0f;
              continue;
            }

            // take green channel distortion also for alpha channel
            float *bufptr = ((float *)buf) + 3;
            const float pi0 = fmaxf(fminf(buf2ptr[2] - roi_in->x, roi_in->width - 1.0f), 0.0f);
            const float pi1 = fmaxf(fminf(buf2ptr[3] - roi_in->y, roi_in->height - 1.0f), 0.0f);
            out[3] = dt_interpolation_compute_sample(interpolation, bufptr, pi0, pi1, roi_in->width,
                                                     roi_in->height, ch, ch_width);
          }
        }
      }
      dt_free_align(buf2);
    }
    else
    {
      memcpy(ovoid, buf, bufsize);
    }
    dt_free_align(buf);
  }
  delete modifier;
}

#ifdef HAVE_OPENCL
static int _process_cl_lf(
        struct dt_iop_module_t *self,
        dt_dev_pixelpipe_iop_t *piece,
        cl_mem dev_in, cl_mem dev_out,
        const dt_iop_roi_t *const roi_in,
        const dt_iop_roi_t *const roi_out)
{
  dt_iop_lens_data_t *d = (dt_iop_lens_data_t *)piece->data;
  dt_iop_lens_global_data_t *gd = (dt_iop_lens_global_data_t *)self->global_data;

  const gboolean raw_monochrome = dt_image_is_monochrome(&self->dev->image_storage);
  const int used_lf_mask = (raw_monochrome) ? LF_MODIFY_ALL & ~LF_MODIFY_TCA : LF_MODIFY_ALL;

  cl_mem dev_tmpbuf = NULL;
  cl_mem dev_tmp = NULL;
  cl_int err = DT_OPENCL_DEFAULT_ERROR;

  float *tmpbuf = NULL;
  lfModifier *modifier = NULL;

  const int devid = piece->pipe->devid;
  const int iwidth = roi_in->width;
  const int iheight = roi_in->height;
  const int owidth = roi_out->width;
  const int oheight = roi_out->height;
  const int roi_in_x = roi_in->x;
  const int roi_in_y = roi_in->y;
  const int width = MAX(iwidth, owidth);
  const int height = MAX(iheight, oheight);
  const int ch = piece->colors;
  const int tmpbufwidth = owidth * 2 * 3;
  const size_t tmpbuflen = d->inverse ? (size_t)oheight * owidth * 2 * 3 * sizeof(float)
                                      : MAX((size_t)oheight * owidth * 2 * 3, (size_t)iheight * iwidth * ch)
                                        * sizeof(float);
  const unsigned int pixelformat = ch == 3 ? LF_CR_3(RED, GREEN, BLUE) : LF_CR_4(RED, GREEN, BLUE, UNKNOWN);

  const float orig_w = roi_in->scale * piece->buf_in.width, orig_h = roi_in->scale * piece->buf_in.height;

  size_t origin[] = { 0, 0, 0 };
  size_t iregion[] = { (size_t)iwidth, (size_t)iheight, 1 };
  size_t oregion[] = { (size_t)owidth, (size_t)oheight, 1 };
  size_t isizes[] = { (size_t)ROUNDUPDWD(iwidth, devid), (size_t)ROUNDUPDHT(iheight, devid), 1 };
  size_t osizes[] = { (size_t)ROUNDUPDWD(owidth, devid), (size_t)ROUNDUPDHT(oheight, devid), 1 };

  int modflags;
  int ldkernel = -1;
  const struct dt_interpolation *interpolation = dt_interpolation_new(DT_INTERPOLATION_USERPREF_WARP);

  if(!d->lens || !d->lens->Maker || d->crop <= 0.0f)
  {
    err = dt_opencl_enqueue_copy_image(devid, dev_in, dev_out, origin, origin, oregion);
    if(err != CL_SUCCESS) goto error;
    return TRUE;
  }

  switch(interpolation->id)
  {
    case DT_INTERPOLATION_BILINEAR:
      ldkernel = gd->kernel_lens_distort_bilinear;
      break;
    case DT_INTERPOLATION_BICUBIC:
      ldkernel = gd->kernel_lens_distort_bicubic;
      break;
    case DT_INTERPOLATION_LANCZOS2:
      ldkernel = gd->kernel_lens_distort_lanczos2;
      break;
    case DT_INTERPOLATION_LANCZOS3:
      ldkernel = gd->kernel_lens_distort_lanczos3;
      break;
    default:
      return FALSE;
  }

  tmpbuf = (float *)dt_alloc_align(64, tmpbuflen);
  if(tmpbuf == NULL) goto error;

  dev_tmp = (cl_mem)dt_opencl_alloc_device(devid, width, height, sizeof(float) * 4);
  if(dev_tmp == NULL) goto error;

  dev_tmpbuf = (cl_mem)dt_opencl_alloc_device_buffer(devid, tmpbuflen);
  if(dev_tmpbuf == NULL) goto error;

  dt_pthread_mutex_lock(&darktable.plugin_threadsafe);
  modifier = _get_modifier(&modflags, orig_w, orig_h, d, used_lf_mask, FALSE);
  dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);

  if(d->inverse)
  {
    // reverse direction (useful for renderings)
    if(modflags & (LF_MODIFY_TCA | LF_MODIFY_DISTORTION | LF_MODIFY_GEOMETRY | LF_MODIFY_SCALE))
    {
#ifdef _OPENMP
#pragma omp parallel for default(none) \
      dt_omp_firstprivate(tmpbufwidth, roi_out) \
      dt_omp_sharedconst(raw_monochrome) \
      shared(tmpbuf, d, modifier) \
      schedule(static)
#endif
      for(int y = 0; y < roi_out->height; y++)
      {
        float *pi = tmpbuf + (size_t)y * tmpbufwidth;
        modifier->ApplySubpixelGeometryDistortion(roi_out->x, roi_out->y + y, roi_out->width, 1, pi);
      }

      /* _blocking_ memory transfer: host tmpbuf buffer -> opencl dev_tmpbuf */
      err = dt_opencl_write_buffer_to_device(devid, tmpbuf, dev_tmpbuf, 0,
                                             (size_t)owidth * oheight * 2 * 3 * sizeof(float), CL_TRUE);
      if(err != CL_SUCCESS) goto error;

      dt_opencl_set_kernel_args(devid, ldkernel, 0, CLARG(dev_in), CLARG(dev_tmp), CLARG(owidth), CLARG(oheight),
        CLARG(iwidth), CLARG(iheight), CLARG(roi_in_x), CLARG(roi_in_y), CLARG(dev_tmpbuf), CLARG((d->do_nan_checks)),
        CLARG((raw_monochrome)));
      err = dt_opencl_enqueue_kernel_2d(devid, ldkernel, osizes);
      if(err != CL_SUCCESS) goto error;
    }
    else
    {
      err = dt_opencl_enqueue_copy_image(devid, dev_in, dev_tmp, origin, origin, oregion);
      if(err != CL_SUCCESS) goto error;
    }

    if(modflags & LF_MODIFY_VIGNETTING)
    {
#ifdef _OPENMP
#pragma omp parallel for default(none) \
      dt_omp_firstprivate(ch, pixelformat, roi_out) \
      shared(tmpbuf, modifier, d) \
      schedule(static)
#endif
      for(int y = 0; y < roi_out->height; y++)
      {
        /* Colour correction: vignetting */
        // actually this way row stride does not matter.
        float *buf = tmpbuf + (size_t)y * ch * roi_out->width;
        for(int k = 0; k < ch * roi_out->width; k++) buf[k] = 0.5f;
        modifier->ApplyColorModification(buf, roi_out->x, roi_out->y + y, roi_out->width, 1,
                                         pixelformat, ch * roi_out->width);
      }

      /* _blocking_ memory transfer: host tmpbuf buffer -> opencl dev_tmpbuf */
      err = dt_opencl_write_buffer_to_device(devid, tmpbuf, dev_tmpbuf, 0,
                                             (size_t)ch * roi_out->width * roi_out->height * sizeof(float),
                                             CL_TRUE);
      if(err != CL_SUCCESS) goto error;

      dt_opencl_set_kernel_args(devid, gd->kernel_lens_vignette, 0, CLARG(dev_tmp), CLARG(dev_out), CLARG(owidth),
        CLARG(oheight), CLARG(dev_tmpbuf));
      err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_lens_vignette, osizes);
      if(err != CL_SUCCESS) goto error;
    }
    else
    {
      err = dt_opencl_enqueue_copy_image(devid, dev_tmp, dev_out, origin, origin, oregion);
      if(err != CL_SUCCESS) goto error;
    }
  }

  else // correct distortions:
  {

    if(modflags & LF_MODIFY_VIGNETTING)
    {
#ifdef _OPENMP
#pragma omp parallel for default(none) \
      dt_omp_firstprivate(ch, pixelformat, roi_in) \
      shared(tmpbuf, modifier, d) \
      schedule(static)
#endif
      for(int y = 0; y < roi_in->height; y++)
      {
        /* Colour correction: vignetting */
        // actually this way row stride does not matter.
        float *buf = tmpbuf + (size_t)y * ch * roi_in->width;
        for(int k = 0; k < ch * roi_in->width; k++) buf[k] = 0.5f;
        modifier->ApplyColorModification(buf, roi_in->x, roi_in->y + y, roi_in->width, 1,
                                         pixelformat, ch * roi_in->width);
      }

      /* _blocking_ memory transfer: host tmpbuf buffer -> opencl dev_tmpbuf */
      err = dt_opencl_write_buffer_to_device(
          devid, tmpbuf, dev_tmpbuf, 0, (size_t)ch * roi_in->width * roi_in->height * sizeof(float), CL_TRUE);
      if(err != CL_SUCCESS) goto error;

      dt_opencl_set_kernel_args(devid, gd->kernel_lens_vignette, 0, CLARG(dev_in), CLARG(dev_tmp), CLARG(iwidth),
        CLARG(iheight), CLARG(dev_tmpbuf));
      err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_lens_vignette, isizes);
      if(err != CL_SUCCESS) goto error;
    }
    else
    {
      err = dt_opencl_enqueue_copy_image(devid, dev_in, dev_tmp, origin, origin, iregion);
      if(err != CL_SUCCESS) goto error;
    }

    if(modflags & (LF_MODIFY_TCA | LF_MODIFY_DISTORTION | LF_MODIFY_GEOMETRY | LF_MODIFY_SCALE))
    {
#ifdef _OPENMP
#pragma omp parallel for default(none) \
      dt_omp_firstprivate(tmpbufwidth, roi_out) \
      dt_omp_sharedconst(raw_monochrome) \
      shared(tmpbuf, d, modifier) \
      schedule(static)
#endif
      for(int y = 0; y < roi_out->height; y++)
      {
        float *pi = tmpbuf + (size_t)y * tmpbufwidth;
        modifier->ApplySubpixelGeometryDistortion(roi_out->x, roi_out->y + y, roi_out->width, 1, pi);
      }

      /* _blocking_ memory transfer: host tmpbuf buffer -> opencl dev_tmpbuf */
      err = dt_opencl_write_buffer_to_device(devid, tmpbuf, dev_tmpbuf, 0,
                                             (size_t)owidth * oheight * 2 * 3 * sizeof(float), CL_TRUE);
      if(err != CL_SUCCESS) goto error;

      dt_opencl_set_kernel_args(devid, ldkernel, 0, CLARG(dev_tmp), CLARG(dev_out), CLARG(owidth), CLARG(oheight),
        CLARG(iwidth), CLARG(iheight), CLARG(roi_in_x), CLARG(roi_in_y), CLARG(dev_tmpbuf), CLARG((d->do_nan_checks)),
        CLARG((raw_monochrome)));
      err = dt_opencl_enqueue_kernel_2d(devid, ldkernel, osizes);
      if(err != CL_SUCCESS) goto error;
    }
    else
    {
      err = dt_opencl_enqueue_copy_image(devid, dev_tmp, dev_out, origin, origin, oregion);
      if(err != CL_SUCCESS) goto error;
    }
  }

  dt_opencl_release_mem_object(dev_tmpbuf);
  dt_opencl_release_mem_object(dev_tmp);
  if(tmpbuf != NULL) dt_free_align(tmpbuf);
  if(modifier != NULL) delete modifier;
  return TRUE;

error:
  dt_opencl_release_mem_object(dev_tmp);
  dt_opencl_release_mem_object(dev_tmpbuf);
  if(tmpbuf != NULL) dt_free_align(tmpbuf);
  if(modifier != NULL) delete modifier;
  dt_print(DT_DEBUG_OPENCL, "[opencl_lens] couldn't enqueue kernel! %s\n", cl_errstr(err));
  return FALSE;
}
#endif

static void _tiling_callback_lf(
        struct dt_iop_module_t *self,
        struct dt_dev_pixelpipe_iop_t *piece,
        const dt_iop_roi_t *roi_in,
        const dt_iop_roi_t *roi_out,
        struct dt_develop_tiling_t *tiling)
{
  tiling->factor = 4.5f; // in + out + tmp + tmpbuf
  tiling->maxbuf = 1.5f;
  tiling->overhead = 0;
  tiling->overlap = 4;
  tiling->xalign = 1;
  tiling->yalign = 1;
  return;
}

static int _distort_transform_lf(
        dt_iop_module_t *self,
        dt_dev_pixelpipe_iop_t *piece,
        float *const __restrict points,
        size_t points_count)
{
  dt_iop_lens_data_t *d = (dt_iop_lens_data_t *)piece->data;
  if(!d->lens || !d->lens->Maker || d->crop <= 0.0f) return 0;

  const float orig_w = piece->buf_in.width;
  const float orig_h = piece->buf_in.height;
  int modflags;

  const int used_lf_mask = (dt_image_is_monochrome(&self->dev->image_storage)) ? LF_MODIFY_ALL & ~LF_MODIFY_TCA : LF_MODIFY_ALL;

  const lfModifier *modifier = _get_modifier(&modflags, orig_w, orig_h, d, used_lf_mask, TRUE);
  if(modflags & (LF_MODIFY_TCA | LF_MODIFY_DISTORTION | LF_MODIFY_GEOMETRY | LF_MODIFY_SCALE))
  {

#ifdef _OPENMP
#pragma omp parallel for default(none) \
    dt_omp_firstprivate(points_count, points, modifier) \
    schedule(static) if(points_count > 100)
#endif
    for(size_t i = 0; i < points_count * 2; i += 2)
    {
      float DT_ALIGNED_ARRAY buf[6];
      modifier->ApplySubpixelGeometryDistortion(points[i], points[i + 1], 1, 1, buf);
      points[i] = buf[0];
      points[i + 1] = buf[3];
    }
  }

  delete modifier;
  return 1;
}

static int _distort_backtransform_lf(
        dt_iop_module_t *self,
        dt_dev_pixelpipe_iop_t *piece,
        float *const __restrict points,
        size_t points_count)
{
  dt_iop_lens_data_t *d = (dt_iop_lens_data_t *)piece->data;

  if(!d->lens || !d->lens->Maker || d->crop <= 0.0f) return 0;

  const int used_lf_mask = (dt_image_is_monochrome(&self->dev->image_storage)) ? LF_MODIFY_ALL & ~LF_MODIFY_TCA : LF_MODIFY_ALL;

  const float orig_w = piece->buf_in.width;
  const float orig_h = piece->buf_in.height;
  int modflags;
  const lfModifier *modifier = _get_modifier(&modflags, orig_w, orig_h, d, used_lf_mask, FALSE);

  if(modflags & (LF_MODIFY_TCA | LF_MODIFY_DISTORTION | LF_MODIFY_GEOMETRY | LF_MODIFY_SCALE))
  {

#ifdef _OPENMP
#pragma omp parallel for default(none) \
    dt_omp_firstprivate(points_count, points, modifier) \
    schedule(static) if(points_count > 100)
#endif
    for(size_t i = 0; i < points_count * 2; i += 2)
    {
      float DT_ALIGNED_ARRAY buf[6];
      modifier->ApplySubpixelGeometryDistortion(points[i], points[i + 1], 1, 1, buf);
      points[i] = buf[0];
      points[i + 1] = buf[3];
    }
  }

  delete modifier;
  return 1;
}

// TODO: Shall we keep LF_MODIFY_TCA in the modifiers?
static void _distort_mask_lf(
        struct dt_iop_module_t *self,
        struct dt_dev_pixelpipe_iop_t *piece,
        const float *const in,
        float *const out,
        const dt_iop_roi_t *const roi_in,
        const dt_iop_roi_t *const roi_out)
{
  const dt_iop_lens_data_t *const d = (dt_iop_lens_data_t *)piece->data;

  if(!d->lens || !d->lens->Maker || d->crop <= 0.0f)
  {
    dt_iop_image_copy_by_size(out, in, roi_out->width, roi_out->height, 1);
    return;
  }

  const float orig_w = roi_in->scale * piece->buf_in.width, orig_h = roi_in->scale * piece->buf_in.height;
  dt_pthread_mutex_lock(&darktable.plugin_threadsafe);
  int modflags;
  const lfModifier *modifier = _get_modifier(&modflags, orig_w, orig_h, d, /*LF_MODIFY_TCA |*/ LF_MODIFY_DISTORTION | LF_MODIFY_GEOMETRY | LF_MODIFY_SCALE, FALSE);

  dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);

  if(!(modflags & (LF_MODIFY_TCA | LF_MODIFY_DISTORTION | LF_MODIFY_GEOMETRY | LF_MODIFY_SCALE)))
  {
    dt_iop_image_copy_by_size(out, in, roi_out->width, roi_out->height, 1);
    delete modifier;
    return;
  }

  const struct dt_interpolation *const interpolation = dt_interpolation_new(DT_INTERPOLATION_USERPREF_WARP);

  // acquire temp memory for distorted pixel coords
  const size_t bufsize = (size_t)roi_out->width * 2 * 3;
  size_t padded_bufsize;
  float *const buf = dt_alloc_perthread_float(bufsize, &padded_bufsize);

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(padded_bufsize, d, in, interpolation, out, roi_in, roi_out) \
  dt_omp_sharedconst(buf) \
  shared(modifier) \
  schedule(static)
#endif
  for(int y = 0; y < roi_out->height; y++)
  {
    float *bufptr = (float*)dt_get_perthread(buf, padded_bufsize);
    modifier->ApplySubpixelGeometryDistortion(roi_out->x, roi_out->y + y, roi_out->width, 1, bufptr);

    // reverse transform the global coords from lf to our buffer
    float *_out = out + (size_t)y * roi_out->width;
    for(int x = 0; x < roi_out->width; x++, bufptr += 6, _out++)
    {
      if(d->do_nan_checks && (!isfinite(bufptr[2]) || !isfinite(bufptr[3])))
      {
        *_out = 0.0f;
        continue;
      }

      // take green channel distortion also for alpha channel
      const float pi0 = bufptr[2] - roi_in->x;
      const float pi1 = bufptr[3] - roi_in->y;
      *_out = dt_interpolation_compute_sample(interpolation, in, pi0, pi1, roi_in->width, roi_in->height, 1,
                                              roi_in->width);
    }
  }
  dt_free_align(buf);
  delete modifier;
}

static void _modify_roi_in_lf(
        struct dt_iop_module_t *self,
        struct dt_dev_pixelpipe_iop_t *piece,
        const dt_iop_roi_t *const roi_out,
        dt_iop_roi_t *roi_in)
{
  dt_iop_lens_data_t *d = (dt_iop_lens_data_t *)piece->data;
  *roi_in = *roi_out;
  // inverse transform with given params

  if(!d->lens || !d->lens->Maker || d->crop <= 0.0f) return;

  const float orig_w = roi_in->scale * piece->buf_in.width, orig_h = roi_in->scale * piece->buf_in.height;
  int modflags;
  const lfModifier *modifier = _get_modifier(&modflags, orig_w, orig_h, d, LF_MODIFY_ALL, FALSE);

  if(modflags & (LF_MODIFY_TCA | LF_MODIFY_DISTORTION | LF_MODIFY_GEOMETRY | LF_MODIFY_SCALE))
  {
    const int xoff = roi_in->x;
    const int yoff = roi_in->y;
    const int width = roi_in->width;
    const int height = roi_in->height;
    const int awidth = abs(width);
    const int aheight = abs(height);
    const int xstep = (width < 0) ? -1 : 1;
    const int ystep = (height < 0) ? -1 : 1;

    float xm = FLT_MAX, xM = -FLT_MAX, ym = FLT_MAX, yM = -FLT_MAX;
    const size_t nbpoints = 2 * awidth + 2 * aheight;

    float *const buf = (float *)dt_alloc_align(64, sizeof(float) * nbpoints * 2 * 3);

#ifdef _OPENMP
#pragma omp parallel default(none) \
    dt_omp_firstprivate(aheight, awidth, buf, height, nbpoints, width, xoff, \
                        xstep, yoff, ystep) \
    shared(modifier) reduction(min : xm, ym) reduction(max : xM, yM)
#endif
    {
#ifdef _OPENMP
#pragma omp for schedule(static)
#endif
      for(int i = 0; i < awidth; i++)
        modifier->ApplySubpixelGeometryDistortion(xoff + i * xstep, yoff, 1, 1, buf + 6 * i);

#ifdef _OPENMP
#pragma omp for schedule(static)
#endif
      for(int i = 0; i < awidth; i++)
        modifier->ApplySubpixelGeometryDistortion(xoff + i * xstep, yoff + (height - 1), 1, 1, buf + 6 * (awidth + i));

#ifdef _OPENMP
#pragma omp for schedule(static)
#endif
      for(int j = 0; j < aheight; j++)
        modifier->ApplySubpixelGeometryDistortion(xoff, yoff + j * ystep, 1, 1, buf + 6 * (2 * awidth + j));

#ifdef _OPENMP
#pragma omp for schedule(static)
#endif
      for(int j = 0; j < aheight; j++)
        modifier->ApplySubpixelGeometryDistortion(xoff + (width - 1), yoff + j * ystep, 1, 1, buf + 6 * (2 * awidth + aheight + j));

#ifdef _OPENMP
#pragma omp barrier
#endif

#ifdef _OPENMP
#pragma omp for schedule(static)
#endif
      for(size_t k = 0; k < nbpoints; k++)
      {
        // iterate over RGB channels x and y coordinates
        for(size_t c = 0; c < 6; c+=2)
        {
          const float x = buf[6 * k + c];
          const float y = buf[6 * k + c + 1];
          xm = isnan(x) ? xm : MIN(xm, x);
          xM = isnan(x) ? xM : MAX(xM, x);
          ym = isnan(y) ? ym : MIN(ym, y);
          yM = isnan(y) ? yM : MAX(yM, y);
        }
      }
    }

    dt_free_align(buf);

    // LensFun can return NAN coords, so we need to handle them carefully.
    if(!isfinite(xm) || !(0 <= xm && xm < orig_w)) xm = 0;
    if(!isfinite(xM) || !(1 <= xM && xM < orig_w)) xM = orig_w;
    if(!isfinite(ym) || !(0 <= ym && ym < orig_h)) ym = 0;
    if(!isfinite(yM) || !(1 <= yM && yM < orig_h)) yM = orig_h;

    const struct dt_interpolation *interpolation = dt_interpolation_new(DT_INTERPOLATION_USERPREF_WARP);
    roi_in->x = fmaxf(0.0f, xm - interpolation->width);
    roi_in->y = fmaxf(0.0f, ym - interpolation->width);
    roi_in->width = fminf(orig_w - roi_in->x, xM - roi_in->x + interpolation->width);
    roi_in->height = fminf(orig_h - roi_in->y, yM - roi_in->y + interpolation->width);

    // sanity check.
    roi_in->x = CLAMP(roi_in->x, 0, (int)floorf(orig_w));
    roi_in->y = CLAMP(roi_in->y, 0, (int)floorf(orig_h));
    roi_in->width = CLAMP(roi_in->width, 1, (int)ceilf(orig_w) - roi_in->x);
    roi_in->height = CLAMP(roi_in->height, 1, (int)ceilf(orig_h) - roi_in->y);
  }
  delete modifier;
}

static void _commit_params_lf(
        struct dt_iop_module_t *self,
        dt_iop_lens_params_t *p,
        dt_dev_pixelpipe_t *pipe,
        dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_lens_data_t *d = (dt_iop_lens_data_t *)piece->data;
  dt_iop_lens_gui_data_t *g = (dt_iop_lens_gui_data_t *)self->gui_data;

  dt_iop_lens_global_data_t *gd = (dt_iop_lens_global_data_t *)self->global_data;
  lfDatabase *dt_iop_lensfun_db = (lfDatabase *)gd->db;
  const lfCamera *camera = NULL;
  const lfCamera **cam = NULL;
  if(d->lens)
  {
    delete d->lens;
    d->lens = NULL;
  }
  d->lens = new lfLens;

  if(p->camera[0])
  {
    dt_pthread_mutex_lock(&darktable.plugin_threadsafe);
    cam = dt_iop_lensfun_db->FindCamerasExt(NULL, p->camera, 0);
    if(cam)
    {
      camera = cam[0];
      d->crop = cam[0]->CropFactor;
    }
    dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);
  }
  if(p->lens[0])
  {
    dt_pthread_mutex_lock(&darktable.plugin_threadsafe);
    const lfLens **lens
        = dt_iop_lensfun_db->FindLenses(camera, NULL, p->lens, 0);
    dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);
    if(lens)
    {
      *d->lens = *lens[0];
      if(p->tca_override)
      {
#ifdef LF_0395
        const dt_image_t *img = &(self->dev->image_storage);

        d->custom_tca =
          {
           .Model     = LF_TCA_MODEL_LINEAR,
           .Focal     = p->focal,
           .Terms     = { p->tca_r, p->tca_b },
           .CalibAttr = {
                         .CenterX = 0.0f,
                         .CenterY = 0.0f,
                         .CropFactor = d->crop,
                         .AspectRatio = (float)img->width / (float)img->height
                         }
          };
#else
        // add manual d->lens stuff:
        lfLensCalibTCA tca = { LF_TCA_MODEL_NONE };
        tca.Focal = 0;
        tca.Model = LF_TCA_MODEL_LINEAR;
        tca.Terms[0] = p->tca_r;
        tca.Terms[1] = p->tca_b;
        if(d->lens->CalibTCA)
          while(d->lens->CalibTCA[0]) d->lens->RemoveCalibTCA(0);
        d->lens->AddCalibTCA(&tca);
#endif
      }
      lf_free(lens);
    }
  }
  lf_free(cam);
  d->inverse = p->inverse;
  d->scale = p->scale;
  d->focal = p->focal;
  d->aperture = p->aperture;
  d->distance = p->distance;
  d->target_geom = _lenstype_to_lensfun_lenstype(p->target_geom);
  d->do_nan_checks = TRUE;
  d->tca_override = p->tca_override;

  /*
   * there are certain situations when LensFun can return NAN coordinated.
   * most common case would be when the FOV is increased.
   */
  if(d->target_geom == LF_RECTILINEAR)
  {
    d->do_nan_checks = FALSE;
  }
  else if(d->target_geom == d->lens->Type)
  {
    d->do_nan_checks = FALSE;
  }

  /* calculate which corrections will be applied by lensfun */
  if(self->dev->gui_attached && g && (piece->pipe->type & DT_DEV_PIXELPIPE_PREVIEW))
  {
    const gboolean raw_monochrome = dt_image_is_monochrome(&self->dev->image_storage);
    const int used_lf_mask = (raw_monochrome) ? LF_MODIFY_ALL & ~LF_MODIFY_TCA : LF_MODIFY_ALL;

    dt_pthread_mutex_lock(&darktable.plugin_threadsafe);

    int modflags;
    /* we use the modifier only to get which corrections will be applied, we have
     * to provide a size that won't be used so we use the image size */
    _get_modifier(&modflags, self->dev->image_storage.width, self->dev->image_storage.height, d, used_lf_mask,
                  FALSE);

    dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);

    dt_iop_gui_enter_critical_section(self);
    g->corrections_done = _modflags_from_lensfun_mods(modflags);
    dt_iop_gui_leave_critical_section(self);
  }
}
/* lensfun processing end*/

/* embedded metadata processing start */

/* This code is based on the algorithm developed by Freddie Witherden <freddie@witherden.org>
 * in pull request
 * https://github.com/darktable-org/darktable/pull/7092 */

#ifdef __GNUC__
  #pragma GCC push_options
  #pragma GCC optimize ("fast-math", "fp-contract=fast", "finite-math-only", "no-math-errno")
#endif

static inline float _interpolate_linear_spline(const float *xi, const float *yi, int ni, float x)
{
  if(x < xi[0])
    return yi[0];

  for(int i = 1; i < ni; i++)
  {
    if(x >= xi[i - 1] && x <= xi[i])
    {
      const float dydx = (yi[i] - yi[i - 1]) / (xi[i] - xi[i - 1]);

      return yi[i - 1] + (x - xi[i - 1]) * dydx;
    }
  }

  return yi[ni - 1];
}

static int _init_coeffs_md(
        const dt_image_t *img,
        const dt_iop_lens_params_t *p,
        const float scale,
        float knots[MAXKNOTS],
        float cor_rgb[3][MAXKNOTS],
        float vig[MAXKNOTS])
{
  const dt_image_correction_data_t *cd = &img->exif_correction_data;

  if(img->exif_correction_type == CORRECTION_TYPE_SONY)
  {
    int nc = cd->sony.nc;
    for(int i = 0; i < nc; i++)
    {
      knots[i] = (float) (i + 0.5) / (nc - 1);

      if(cor_rgb && p->modify_flags & DT_IOP_LENS_MODIFY_FLAG_DISTORTION)
        cor_rgb[0][i] = cor_rgb[1][i] = cor_rgb[2][i] = (p->cor_dist_ft * cd->sony.distortion[i] * powf(2, -14) + 1) * scale;
      else if(cor_rgb)
        cor_rgb[0][i] = cor_rgb[1][i] = cor_rgb[2][i] = scale;

      if(cor_rgb && p->modify_flags & DT_IOP_LENS_MODIFY_FLAG_TCA)
      {
        cor_rgb[0][i] *= cd->sony.ca_r[i] * powf(2, -21) + 1;
        cor_rgb[2][i] *= cd->sony.ca_b[i] * powf(2, -21) + 1;
      }

      if(vig && p->modify_flags & DT_IOP_LENS_MODIFY_FLAG_VIGNETTING)
        vig[i] = powf(2, 0.5f - powf(2, p->cor_vig_ft * cd->sony.vignetting[i] * powf(2, -13)  - 1));
      else if(vig)
        vig[i] = 1;
    }

    return nc;
  }
  else if(img->exif_correction_type == CORRECTION_TYPE_FUJI)
  {
    int nc = cd->fuji.nc;
    for(int i = 0; i < nc; i++)
    {
      knots[i] = cd->fuji.cropf * cd->fuji.knots[i];

      if(cor_rgb && p->modify_flags & DT_IOP_LENS_MODIFY_FLAG_DISTORTION)
        cor_rgb[0][i] = cor_rgb[1][i] = cor_rgb[2][i] = (p->cor_dist_ft * cd->fuji.distortion[i] / 100 + 1) * scale;
      else if(cor_rgb)
        cor_rgb[0][i] = cor_rgb[1][i] = cor_rgb[2][i] = scale;

      if(cor_rgb && p->modify_flags & DT_IOP_LENS_MODIFY_FLAG_TCA)
      {
        cor_rgb[0][i] *= cd->fuji.ca_r[i] + 1;
        cor_rgb[2][i] *= cd->fuji.ca_b[i] + 1;
      }

      if(vig && p->modify_flags & DT_IOP_LENS_MODIFY_FLAG_VIGNETTING)
        vig[i] = 1 - p->cor_vig_ft * (1 - cd->fuji.vignetting[i] / 100);
      else if(vig)
        vig[i] = 1;
    }

    return nc;
  }

  else if(img->exif_correction_type == CORRECTION_TYPE_DNG)
  {
    const int nc = MAXKNOTS;
    for(int i = 0; i < nc; i++)
    {
      const float r = (float) i / (float) (nc);
      knots[i] = r;
      if(cor_rgb) cor_rgb[0][i] = cor_rgb[1][i] = cor_rgb[2][i] = 1.0f;
      if(vig)     vig[i] = 1.0f;

      const float pw2 = powf(r, 2.0f), pw4 = powf(r, 4.0f), pw6 = powf(r, 6.0f);
      if(cor_rgb && cd->dng.has_warp && p->modify_flags & (DT_IOP_LENS_MODIFY_FLAG_DISTORTION | DT_IOP_LENS_MODIFY_FLAG_TCA))
      {
        // Convert the polynomial to a spline by evaluating it at each knot
        for(int c = 0; c < cd->dng.planes; c++)
        {
          const float r_cor = cd->dng.cwarp[c][0] + cd->dng.cwarp[c][1]*pw2 + cd->dng.cwarp[c][2]*pw4 + cd->dng.cwarp[c][3]*pw6;
          cor_rgb[c][i] = (p->cor_dist_ft * (r_cor - 1.0f) + 1.0f) * scale;
        }

        if(cd->dng.planes == 1)
          cor_rgb[2][i] = cor_rgb[1][i] = cor_rgb[0][i];
      }

      if(vig && cd->dng.has_vignette && (p->modify_flags & DT_IOP_LENS_MODIFY_FLAG_VIGNETTING))
      {
        const float dvig = cd->dng.cvig[0]*pw2 + cd->dng.cvig[1]*pw4 + cd->dng.cvig[2]*pw6
                         + cd->dng.cvig[3]*powf(r, 8.0f) + cd->dng.cvig[4]*powf(r, 10.0f);
        // Pixel value is to be divided by (1 + dvig) to correct vignetting
        // Scale dvig according to fine-tune: 0 for no correction, 1 for
        // correction specified by metadata, and 2 to double the correction.
        // Store the square root since _process_md will square the value
        vig[i] = sqrtf(1.0f / (1.0f + p->cor_vig_ft * dvig));
      }
    }
    return nc;
  }

  return 0;
}

static float _get_autoscale_md(dt_iop_module_t *self, dt_iop_lens_params_t *p)
{
  const dt_image_t *img = &(self->dev->image_storage);
  if(img->exif_correction_type == CORRECTION_TYPE_DNG)
    return 1.0f;

  const float tested = 200.0f;

  float knots[MAXKNOTS], cor_rgb[3][MAXKNOTS];
  // Default the scale to one for the benefit of init_coeffs

  const int nc = _init_coeffs_md(img, p, 1.0f, knots, cor_rgb, NULL);
  // Compute the new scale
  float scale = 0.0f;
  for(float i = 0.0f; i < tested; i++)
  {
    for(int j = 0; j < 3; j++)
      scale = fmaxf(scale, _interpolate_linear_spline(knots, cor_rgb[j], nc, 0.5f + 0.5f * i / (tested - 1.0f)));
  }
  return scale;
}

static void _autoscale_pressed_md(GtkWidget *button, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_lens_gui_data_t *g = (dt_iop_lens_gui_data_t *)self->gui_data;
  dt_iop_lens_params_t *p = (dt_iop_lens_params_t *)self->params;

  const float scale = _get_autoscale_md(self, p);
  dt_bauhaus_slider_set(g->cor_scale, scale);
}

static int _check_corrections_md(dt_iop_lens_data_t *d)
{
  gboolean has_vignette = FALSE;
  gboolean has_distort = FALSE;
  gboolean has_tca = FALSE;

  for(int i = 0; i < d->nc; i++)
  {
    if(!(feqf(d->vig[i], 1.0f, 1e-7)))
       has_vignette |= TRUE;
    for(int c = 0; c < 3; c++)
    {
      if(!(feqf(d->cor_rgb[c][i], 1.0f, 1e-7)))
         has_distort |= TRUE;
    }
    if((d->cor_rgb[0][i] != d->cor_rgb[1][i])
       || (d->cor_rgb[0][i] != d->cor_rgb[2][i])
       || (d->cor_rgb[1][i] != d->cor_rgb[2][i]))
      has_tca |= TRUE;
  }

  return (((d->modify_flags & DT_IOP_LENS_MODIFY_FLAG_TCA) && has_tca) ? DT_IOP_LENS_MODIFY_FLAG_TCA : 0)
       | (((d->modify_flags & DT_IOP_LENS_MODIFY_FLAG_VIGNETTING) && has_vignette) ? DT_IOP_LENS_MODIFY_FLAG_VIGNETTING : 0)
       | (((d->modify_flags & DT_IOP_LENS_MODIFY_FLAG_DISTORTION) && has_distort) ? DT_IOP_LENS_MODIFY_FLAG_DISTORTION : 0);
}

static void _commit_params_md(
        dt_iop_module_t *self,
        dt_iop_lens_params_t *p,
        dt_dev_pixelpipe_t *pipe,
        dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_lens_data_t *d = (dt_iop_lens_data_t *)piece->data;
  dt_iop_lens_gui_data_t *g = (dt_iop_lens_gui_data_t *)self->gui_data;

  const dt_image_t *img = &self->dev->image_storage;

  d->nc = 0;

  if(!_have_embedded_metadata(self))
    return;

  d->cor_dist_ft = p->cor_dist_ft;
  d->cor_vig_ft = p->cor_vig_ft;

  d->scale_md = p->cor_scale;
  if((d->scale_md < 0.9f) || (d->scale_md > 1.1f)) // enforce an autoscale if unproper data
    d->scale_md = _get_autoscale_md(self, p);
  d->nc = _init_coeffs_md(img, p, 1.0f / d->scale_md, d->knots, d->cor_rgb, d->vig);

  if(self->dev->gui_attached && g && (piece->pipe->type & DT_DEV_PIXELPIPE_PREVIEW))
  {
    dt_iop_gui_enter_critical_section(self);
    g->corrections_done = _check_corrections_md(d);
    dt_iop_gui_leave_critical_section(self);
  }
}

static void _tiling_callback_md(
        struct dt_iop_module_t *self,
        struct dt_dev_pixelpipe_iop_t *piece,
        const dt_iop_roi_t *roi_in,
        const dt_iop_roi_t *roi_out,
        struct dt_develop_tiling_t *tiling)
{
  tiling->factor = 4.5f; // in + out + tmp + tmpbuf
  tiling->maxbuf = 1.5f;
  tiling->overhead = 0;
  tiling->overlap = 4;
  tiling->xalign = 1;
  tiling->yalign = 1;
}

static int _distort_transform_md(
        dt_iop_module_t *self,
        dt_dev_pixelpipe_iop_t *piece,
        float *points,
        size_t points_count)
{
  dt_iop_lens_data_t *d = (dt_iop_lens_data_t *)piece->data;

  if(!d->nc || d->modify_flags == DT_IOP_LENS_MODFLAG_NONE)
    return 0;

  const float w2 = 0.5f * piece->buf_in.width;
  const float h2 = 0.5f * piece->buf_in.height;
  const float r = 1 / sqrtf(w2*w2 + h2*h2);

  for(size_t i = 0; i < 2*points_count; i += 2)
  {
    float p1 = points[i];
    float p2 = points[i + 1];

    for(int k = 0; k < 10; k++)
    {
      const float cx = p1 - w2;
      const float cy = p2 - h2;
      const float dr = _interpolate_linear_spline(d->knots, d->cor_rgb[1], d->nc, r*sqrtf(cx*cx + cy*cy));

      const float dist1 = points[i] - (dr*cx + w2), dist2 = points[i + 1] - (dr*cy + h2);
      if(fabs(dist1) < .5f && fabs(dist2) < .5f)
        break;

      p1 += dist1;
      p2 += dist2;
    }

    points[i] = p1;
    points[i + 1] = p2;
  }

  return 1;
}

static int _distort_backtransform_md(
        dt_iop_module_t *self,
        dt_dev_pixelpipe_iop_t *piece,
        float *points,
        size_t points_count)
{
  dt_iop_lens_data_t *d = (dt_iop_lens_data_t *)piece->data;

  if(!d->nc || d->modify_flags == DT_IOP_LENS_MODFLAG_NONE)
    return 0;

  const float w2 = 0.5f * piece->buf_in.width;
  const float h2 = 0.5f * piece->buf_in.height;
  const float r = 1.0f / sqrtf(w2*w2 + h2*h2);

  for(size_t i = 0; i < 2*points_count; i += 2)
  {
    const float cx = points[i] - w2;
    const float cy = points[i + 1] - h2;
    const float dr = _interpolate_linear_spline(d->knots, d->cor_rgb[1], d->nc, r*sqrtf(cx*cx + cy*cy));

    points[i] = dr*cx + w2;
    points[i + 1] = dr*cy + h2;
  }

  return 1;
}

static void _distort_mask_md(
        struct dt_iop_module_t *self,
        struct dt_dev_pixelpipe_iop_t *piece,
        const float *const in,
        float *const out,
        const dt_iop_roi_t *const roi_in,
        const dt_iop_roi_t *const roi_out)
{
  dt_iop_lens_data_t *d = (dt_iop_lens_data_t *)piece->data;

  if(!d->nc || d->modify_flags == DT_IOP_LENS_MODFLAG_NONE)
    return dt_iop_image_copy_by_size(out, in, roi_out->width, roi_out->height, 1);

  const float w2 = 0.5f * roi_in->scale * piece->buf_in.width;
  const float h2 = 0.5f * roi_in->scale * piece->buf_in.height;
  const float r = 1.0f / sqrtf(w2*w2 + h2*h2);

  const struct dt_interpolation *interpolation = dt_interpolation_new(DT_INTERPOLATION_USERPREF_WARP);

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(roi_in, roi_out, d, in, out, interpolation) \
  dt_omp_sharedconst(w2, h2, r) \
  schedule(static) collapse(2)
#endif
  for(int y = 0; y < roi_out->height; y++)
  {
    for(int x = 0; x < roi_out->width; x++)
    {
      const float cx = roi_out->x + x - w2;
      const float cy = roi_out->y + y - h2;
      const float dr = _interpolate_linear_spline(d->knots, d->cor_rgb[1], d->nc, r*sqrtf(cx*cx + cy*cy));
      const float xs = dr*cx + w2 - roi_in->x;
      const float ys = dr*cy + h2 - roi_in->y;
      out[y * roi_out->width + x] = dt_interpolation_compute_sample(interpolation, in, xs, ys, roi_in->width,
                                              roi_in->height, 1, roi_in->width);
    }
  }
}

static void _process_md(
        struct dt_iop_module_t *self,
        dt_dev_pixelpipe_iop_t *piece,
        const void *const ivoid,
        void *const ovoid,
        const dt_iop_roi_t *const roi_in,
        const dt_iop_roi_t *const roi_out)
{
  dt_iop_lens_data_t *d = (dt_iop_lens_data_t *)piece->data;

  if(!d->nc || d->modify_flags == DT_IOP_LENS_MODFLAG_NONE)
    return dt_iop_copy_image_roi((float *)ovoid, (float *)ivoid, 4, roi_in, roi_out, TRUE);

  const float w2 = 0.5f * roi_in->scale * piece->buf_in.width;
  const float h2 = 0.5f * roi_in->scale * piece->buf_in.height;
  const float r = 1.0f / sqrtf(w2*w2 + h2*h2);

  const struct dt_interpolation *interpolation = dt_interpolation_new(DT_INTERPOLATION_USERPREF_WARP);

  // Allocate temporary storage
  const size_t bufsize = (size_t) roi_in->width * roi_in->height * 4;
  float *buf = dt_alloc_align_float(bufsize);
  dt_iop_image_copy(buf, (float*)ivoid, bufsize);

  // Correct vignetting
  if(d->modify_flags & DT_IOP_LENS_MODIFY_FLAG_VIGNETTING)
  {
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(roi_in, buf, d) \
  dt_omp_sharedconst(w2, h2, r) \
  schedule(static) collapse(2)
#endif
    for(int y = 0; y < roi_in->height; y++)
    {
      for(int x = 0; x < roi_in->width; x++)
      {
        const size_t idx = 4 * (y * roi_in->width + x);
        const float cx = roi_in->x + x - w2;
        const float cy = roi_in->y + y - h2;
        const float sf = _interpolate_linear_spline(d->knots, d->vig, d->nc, r*sqrtf(cx*cx + cy*cy));

        for_each_channel(c)
          buf[idx + c] /= sf*sf;
      }
    }
  }

  float *out = ((float *) ovoid);
  // Correct distortion and/or chromatic aberration

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(roi_in, roi_out, buf, d, out, interpolation) \
  dt_omp_sharedconst(w2, h2, r) \
  schedule(static) collapse(2)
#endif
  for(int y = 0; y < roi_out->height; y++)
  {
    for(int x = 0; x < roi_out->width; x++)
    {
      const size_t odx = 4 * (y * roi_out->width + x);
      const float cx = roi_out->x + x - w2;
      const float cy = roi_out->y + y - h2;

      const float radius = r*sqrtf(cx*cx + cy*cy);
      for_each_channel(c)
      {
        const float dr = _interpolate_linear_spline(d->knots, d->cor_rgb[c], d->nc, radius);
        const float xs = dr*cx + w2 - roi_in->x;
        const float ys = dr*cy + h2 - roi_in->y;
        out[odx+c] = dt_interpolation_compute_sample(interpolation, buf + c, xs, ys, roi_in->width,
                                                 roi_in->height, 4, 4*roi_in->width);
      }
    }
  }

  dt_free_align(buf);
}

static void _modify_roi_in_md(
        struct dt_iop_module_t *self,
        struct dt_dev_pixelpipe_iop_t *piece,
        const dt_iop_roi_t *const roi_out,
        dt_iop_roi_t *roi_in)
{
  dt_iop_lens_data_t *d = (dt_iop_lens_data_t *)piece->data;

  *roi_in = *roi_out;

  if(!d->nc || d->modify_flags==DT_IOP_LENS_MODFLAG_NONE)
    return;

  const float orig_w = roi_in->scale * piece->buf_in.width;
  const float orig_h = roi_in->scale * piece->buf_in.height;
  const float w2 = 0.5f * orig_w;
  const float h2 = 0.5f * orig_h;
  const float r = 1.0f / sqrtf(w2*w2 + h2*h2);

  const int xoff = roi_in->x;
  const int yoff = roi_in->y;
  const int width = roi_in->width, height = roi_in->height;
  const float cxs[] = { xoff - w2, xoff + (width - 1) - w2 };
  const float cys[] = { yoff - h2, yoff + (height - 1) - h2 };

  float xm = FLT_MAX;
  float xM = -FLT_MAX;
  float ym = FLT_MAX;
  float yM = -FLT_MAX;

  // Sweep along the top and bottom rows of the ROI
  for(int i = 0; i < width; i++)
  {
    const float cx = xoff + i - w2;
    for(int j = 0; j < 2; j++)
    {
      const float cy = cys[j];
      for_each_channel(c)
      {
        const float dr = _interpolate_linear_spline(d->knots, d->cor_rgb[c], d->nc, r*sqrtf(cx*cx + cy*cy));
        const float xs = dr*cx + w2;
        const float ys = dr*cy + h2;
        xm = fminf(xm, xs);
        xM = fmaxf(xM, xs);
        ym = fminf(ym, ys);
        yM = fmaxf(yM, ys);
      }
    }
  }

  // Sweep along the left and right columns of the ROI
  for(int j = 0; j < height; j++)
  {
    const float cy = yoff + j - h2;
    for(int i = 0; i < 2; i++)
    {
      const float cx = cxs[i];
      for_each_channel(c)
      {
        const float dr = _interpolate_linear_spline(d->knots, d->cor_rgb[c], d->nc, r*sqrtf(cx*cx + cy*cy));
        const float xs = dr*cx + w2;
        const float ys = dr*cy + h2;
        xm = fminf(xm, xs);
        xM = fmaxf(xM, xs);
        ym = fminf(ym, ys);
        yM = fmaxf(yM, ys);
      }
    }
  }

  const struct dt_interpolation *interpolation = dt_interpolation_new(DT_INTERPOLATION_USERPREF_WARP);
  roi_in->x = (int)fmaxf(0.0f, xm - interpolation->width);
  roi_in->y = (int)fmaxf(0.0f, ym - interpolation->width);
  roi_in->width = (int)fminf(orig_w - roi_in->x, xM - roi_in->x + interpolation->width);
  roi_in->height = (int)fminf(orig_h - roi_in->y, yM - roi_in->y + interpolation->width);
}

#ifdef __GNUC__
  #pragma GCC pop_options
#endif

/* embedded metadata processing end */

void process(
        dt_iop_module_t *self,
        dt_dev_pixelpipe_iop_t *piece,
        const void *const ivoid,
        void *const ovoid,
        const dt_iop_roi_t *const roi_in,
        const dt_iop_roi_t *const roi_out)
{
  dt_iop_lens_data_t *d = (dt_iop_lens_data_t *)piece->data;

  if(d->method == DT_IOP_LENS_METHOD_LENSFUN)
  {
    _process_lf(self, piece, ivoid, ovoid, roi_in, roi_out);
  }
  else
  {
    _process_md(self, piece, ivoid, ovoid, roi_in, roi_out);
  }
}


#ifdef HAVE_OPENCL
int process_cl(
        struct dt_iop_module_t *self,
        dt_dev_pixelpipe_iop_t *piece,
        cl_mem dev_in,
        cl_mem dev_out,
        const dt_iop_roi_t *const roi_in,
        const dt_iop_roi_t *const roi_out)
{
  // process_cl is called only for lensfun method
  return _process_cl_lf(self, piece, dev_in, dev_out, roi_in, roi_out);
}
#endif

void tiling_callback(
        struct dt_iop_module_t *self,
        struct dt_dev_pixelpipe_iop_t *piece,
        const dt_iop_roi_t *roi_in,
        const dt_iop_roi_t *roi_out,
        struct dt_develop_tiling_t *tiling)
{
  dt_iop_lens_data_t *d = (dt_iop_lens_data_t *)piece->data;

  if(d->method == DT_IOP_LENS_METHOD_LENSFUN)
  {
    _tiling_callback_lf(self, piece, roi_in, roi_out, tiling);
  }
  else
  {
    _tiling_callback_md(self, piece, roi_in, roi_out, tiling);
  }
}

int distort_transform(
        dt_iop_module_t *self,
        dt_dev_pixelpipe_iop_t *piece,
        float *const __restrict points,
        size_t points_count)
{
  dt_iop_lens_data_t *d = (dt_iop_lens_data_t *)piece->data;

  if(d->method == DT_IOP_LENS_METHOD_LENSFUN)
  {
    return _distort_transform_lf(self, piece, points, points_count);
  }
  else
  {
    return _distort_transform_md(self, piece, points, points_count);
  }
}

int distort_backtransform(
        dt_iop_module_t *self,
        dt_dev_pixelpipe_iop_t *piece,
        float *const __restrict points,
        size_t points_count)
{
  dt_iop_lens_data_t *d = (dt_iop_lens_data_t *)piece->data;

  if(d->method == DT_IOP_LENS_METHOD_LENSFUN)
  {
    return _distort_backtransform_lf(self, piece, points, points_count);
  }
  else
  {
    return _distort_backtransform_md(self, piece, points, points_count);
  }
}

void distort_mask(
        struct dt_iop_module_t *self,
        struct dt_dev_pixelpipe_iop_t *piece,
        const float *const in,
        float *const out,
        const dt_iop_roi_t *const roi_in,
        const dt_iop_roi_t *const roi_out)
{
  dt_iop_lens_data_t *d = (dt_iop_lens_data_t *)piece->data;

  if(d->method == DT_IOP_LENS_METHOD_LENSFUN)
  {
    _distort_mask_lf(self, piece, in, out, roi_in, roi_out);
  }
  else
  {
    _distort_mask_md(self, piece, in, out, roi_in, roi_out);
  }
}

void modify_roi_in(
        struct dt_iop_module_t *self,
        struct dt_dev_pixelpipe_iop_t *piece,
        const dt_iop_roi_t *const roi_out,
        dt_iop_roi_t *roi_in)
{
  dt_iop_lens_data_t *d = (dt_iop_lens_data_t *)piece->data;

  if(d->method == DT_IOP_LENS_METHOD_LENSFUN)
  {
    _modify_roi_in_lf(self, piece, roi_out, roi_in);
  }
  else
  {
    _modify_roi_in_md(self, piece, roi_out, roi_in);
  }
}

// _get_method returns the method to use based on the provided preferred method and available methods.
const dt_iop_lens_method_t _get_method(
        struct dt_iop_module_t *self,
        dt_iop_lens_method_t method)
{
  // currently we have only two methods. If new methods will be added a default
  // order of fallback methods should be defined to keeps reproducibilty

  // prefer provided method if available
  if(method == DT_IOP_LENS_METHOD_EMBEDDED_METADATA && !_have_embedded_metadata(self))
  {
    // fallback to lensfun method
    method = DT_IOP_LENS_METHOD_LENSFUN;
  }

  return method;
}

void commit_params(
        struct dt_iop_module_t *self,
        dt_iop_params_t *p1,
        dt_dev_pixelpipe_t *pipe,
        dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_lens_params_t *p = (dt_iop_lens_params_t *)p1;
  dt_iop_lens_data_t *d = (dt_iop_lens_data_t *)piece->data;
  // check ?  p->method = _get_method(self, method);

  d->method = p->method;
  d->modify_flags = p->modify_flags;
  if(dt_image_is_monochrome(&self->dev->image_storage)) d->modify_flags &= ~DT_IOP_LENS_MODIFY_FLAG_TCA;

  // no OpenCL for LENS_METHOD_EMBEDDED_METADATA
  piece->process_cl_ready = (d->method == DT_IOP_LENS_METHOD_EMBEDDED_METADATA) ? 0 : 1;

  if(d->method == DT_IOP_LENS_METHOD_LENSFUN)
  {
    _commit_params_lf(self, p, pipe, piece);
  }
  else
  {
    _commit_params_md(self, p, pipe, piece);
  }
}

void init_pipe(
        struct dt_iop_module_t *self,
        dt_dev_pixelpipe_t *pipe,
        dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1, sizeof(dt_iop_lens_data_t));
}

void cleanup_pipe(
        struct dt_iop_module_t *self,
        dt_dev_pixelpipe_t *pipe,
        dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_lens_data_t *d = (dt_iop_lens_data_t *)piece->data;

  if(d->lens)
  {
    delete d->lens;
    d->lens = NULL;
  }

  free(piece->data);
  piece->data = NULL;
}

void init_global(dt_iop_module_so_t *module)
{
  const int program = 2; // basic.cl, from programs.conf
  dt_iop_lens_global_data_t *gd
      = (dt_iop_lens_global_data_t *)calloc(1, sizeof(dt_iop_lens_global_data_t));
  module->data = gd;
  gd->kernel_lens_distort_bilinear = dt_opencl_create_kernel(program, "lens_distort_bilinear");
  gd->kernel_lens_distort_bicubic = dt_opencl_create_kernel(program, "lens_distort_bicubic");
  gd->kernel_lens_distort_lanczos2 = dt_opencl_create_kernel(program, "lens_distort_lanczos2");
  gd->kernel_lens_distort_lanczos3 = dt_opencl_create_kernel(program, "lens_distort_lanczos3");
  gd->kernel_lens_vignette = dt_opencl_create_kernel(program, "lens_vignette");

  lfDatabase *dt_iop_lensfun_db = new lfDatabase;
  gd->db = (lfDatabase *)dt_iop_lensfun_db;

#if defined(__MACH__) || defined(__APPLE__)
#else
  if(dt_iop_lensfun_db->Load() != LF_NO_ERROR)
#endif
  {
    char datadir[PATH_MAX] = { 0 };
    dt_loc_get_datadir(datadir, sizeof(datadir));

    // get parent directory
    GFile *file = g_file_parse_name(datadir);
    gchar *path = g_file_get_path(g_file_get_parent(file));
    g_object_unref(file);
#ifdef LF_MAX_DATABASE_VERSION
    gchar *sysdbpath = g_build_filename(path, "lensfun", "version_" STR(LF_MAX_DATABASE_VERSION), (char *)NULL);
#endif

#ifdef LF_0395
    const long userdbts = dt_iop_lensfun_db->ReadTimestamp(dt_iop_lensfun_db->UserUpdatesLocation);
    const long sysdbts = dt_iop_lensfun_db->ReadTimestamp(sysdbpath);
    const char *dbpath = userdbts > sysdbts ? dt_iop_lensfun_db->UserUpdatesLocation : sysdbpath;
    if(dt_iop_lensfun_db->Load(dbpath) != LF_NO_ERROR)
      dt_print(DT_DEBUG_ALWAYS, "[iop_lens]: could not load lensfun database in `%s'!\n", dbpath);
    else
      dt_iop_lensfun_db->Load(dt_iop_lensfun_db->UserLocation);
#else
    // code for older lensfun preserved as-is
#ifdef LF_MAX_DATABASE_VERSION
    g_free(dt_iop_lensfun_db->HomeDataDir);
    dt_iop_lensfun_db->HomeDataDir = g_strdup(sysdbpath);
    if(dt_iop_lensfun_db->Load() != LF_NO_ERROR)
    {
      dt_print(DT_DEBUG_ALWAYS, "[iop_lens]: could not load lensfun database in `%s'!\n", sysdbpath);
#endif
      g_free(dt_iop_lensfun_db->HomeDataDir);
      dt_iop_lensfun_db->HomeDataDir = g_build_filename(path, "lensfun", (char *)NULL);
      if(dt_iop_lensfun_db->Load() != LF_NO_ERROR)
        dt_print(DT_DEBUG_ALWAYS, "[iop_lens]: could not load lensfun database in `%s'!\n", dt_iop_lensfun_db->HomeDataDir);
#ifdef LF_MAX_DATABASE_VERSION
    }
#endif
#endif

#ifdef LF_MAX_DATABASE_VERSION
    g_free(sysdbpath);
#endif
    g_free(path);
  }
}

static char *_lens_sanitize(const char *orig_lens)
{
  const char *found_or = strstr(orig_lens, " or ");
  const char *found_parenthesis = strstr(orig_lens, " (");

  if(found_or || found_parenthesis)
  {
    const size_t pos_or = (size_t)(found_or - orig_lens);
    const size_t pos_parenthesis = (size_t)(found_parenthesis - orig_lens);
    const size_t pos = pos_or < pos_parenthesis ? pos_or : pos_parenthesis;

    if(pos > 0)
    {
      char *new_lens = (char *)malloc(pos + 1);

      strncpy(new_lens, orig_lens, pos);
      new_lens[pos] = '\0';

      return new_lens;
    }
    else
    {
      char *new_lens = strdup(orig_lens);
      return new_lens;
    }
  }
  else
  {
    char *new_lens = strdup(orig_lens);
    return new_lens;
  }
}

void reload_defaults(dt_iop_module_t *module)
{
  char *new_lens;
  const dt_image_t *img = &module->dev->image_storage;

  // reload image specific stuff
  // get all we can from exif:
  dt_iop_lens_params_t *d = (dt_iop_lens_params_t *)module->default_params;

  new_lens = _lens_sanitize(img->exif_lens);
  g_strlcpy(d->lens, new_lens, sizeof(d->lens));
  free(new_lens);
  g_strlcpy(d->camera, img->exif_model, sizeof(d->camera));
  d->crop = img->exif_crop;
  d->aperture = img->exif_aperture;
  d->focal = img->exif_focal_length;
  d->scale = 1.0;
  d->modify_flags = DT_IOP_LENS_MODFLAG_ALL;

  // if we did not find focus_distance in EXIF, lets default to 1000
  d->distance = img->exif_focus_distance == 0.0f ? 1000.0f : img->exif_focus_distance;
  d->target_geom = DT_IOP_LENS_LENSTYPE_RECTILINEAR;

  if(dt_image_is_monochrome(img))
    d->modify_flags &= ~DT_IOP_LENS_MODIFY_FLAG_TCA;

  // init crop from lensfun db:
  char model[100]; // truncate often complex descriptions.
  g_strlcpy(model, img->exif_model, sizeof(model));
  for(char cnt = 0, *c = model; c < model + 100 && *c != '\0'; c++)
    if(*c == ' ')
      if(++cnt == 2) *c = '\0';
  if(img->exif_maker[0] || model[0])
  {
    dt_iop_lens_global_data_t *gd = (dt_iop_lens_global_data_t *)module->global_data;

    // just to be sure
    if(!gd || !gd->db) return;

    dt_pthread_mutex_lock(&darktable.plugin_threadsafe);
    const lfCamera **cam = gd->db->FindCamerasExt(img->exif_maker, img->exif_model, 0);
    dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);
    if(cam)
    {
      dt_pthread_mutex_lock(&darktable.plugin_threadsafe);
      const lfLens **lens = gd->db->FindLenses(cam[0], NULL, d->lens, 0);
      dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);

      if(!lens && islower(cam[0]->Mount[0]))
      {
        /*
         * This is a fixed-lens camera, and LF returned no lens.
         * (reasons: lens is "(65535)" or lens is correct lens name,
         *  but LF have it as "fixed lens")
         *
         * Let's unset lens name and re-run lens query
         */
        g_strlcpy(d->lens, "", sizeof(d->lens));

        dt_pthread_mutex_lock(&darktable.plugin_threadsafe);
        lens = gd->db->FindLenses(cam[0], NULL, d->lens, 0);
        dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);
      }

      if(lens)
      {
        int lens_i = 0;

        /*
         * Current SVN lensfun lets you test for a fixed-lens camera by looking
         * at the zeroth character in the mount's name:
         * If it is a lower case letter, it is a fixed-lens camera.
         */
        if(!d->lens[0] && islower(cam[0]->Mount[0]))
        {
          /*
           * no lens info in EXIF, and this is fixed-lens camera,
           * let's find shortest lens model in the list of possible lenses
           */
          size_t min_model_len = SIZE_MAX;
          for(int i = 0; lens[i]; i++)
          {
            if(strlen(lens[i]->Model) < min_model_len)
            {
              min_model_len = strlen(lens[i]->Model);
              lens_i = i;
            }
          }

          // and set lens to it
          g_strlcpy(d->lens, lens[lens_i]->Model, sizeof(d->lens));
        }

        d->target_geom = _lenstype_from_lensfun_lenstype(lens[lens_i]->Type);
        lf_free(lens);
      }

      d->crop = cam[0]->CropFactor;
      d->scale = _get_autoscale_lf(module, d, cam[0]);

      lf_free(cam);
    }
  }

  d->method = DT_IOP_LENS_METHOD_LENSFUN;
  if(_have_embedded_metadata(module))
  {
    // prefer embedded metadata if available
    d->method = DT_IOP_LENS_METHOD_EMBEDDED_METADATA;
    d->cor_scale = _get_autoscale_md(module, d);
  }

  dt_iop_lens_gui_data_t *g = (dt_iop_lens_gui_data_t *)module->gui_data;
  if(g)
  {
    // rebuild methods selector combbox with only available methods
    const int menu_size = dt_bauhaus_combobox_length(g->methods_selector);
    for(int i = 0; i < menu_size; i++) dt_bauhaus_combobox_remove_at(g->methods_selector, 0);

    if(_have_embedded_metadata(module))
      dt_bauhaus_combobox_add_full(g->methods_selector, _("embedded metadata"), DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT,
                                   GINT_TO_POINTER(DT_IOP_LENS_METHOD_EMBEDDED_METADATA), NULL, TRUE);

    dt_bauhaus_combobox_add_full(g->methods_selector, _("lensfun"), DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT,
                                 GINT_TO_POINTER(DT_IOP_LENS_METHOD_LENSFUN), NULL, TRUE);

    // if we have a gui -> reset corrections_done message
    dt_iop_gui_enter_critical_section(module);
    g->corrections_done = -1;
    dt_iop_gui_leave_critical_section(module);
    gtk_label_set_text(g->message, "");
  }
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_lens_global_data_t *gd = (dt_iop_lens_global_data_t *)module->data;

  lfDatabase *dt_iop_lensfun_db = (lfDatabase *)gd->db;
  delete dt_iop_lensfun_db;

  dt_opencl_free_kernel(gd->kernel_lens_distort_bilinear);
  dt_opencl_free_kernel(gd->kernel_lens_distort_bicubic);
  dt_opencl_free_kernel(gd->kernel_lens_distort_lanczos2);
  dt_opencl_free_kernel(gd->kernel_lens_distort_lanczos3);
  dt_opencl_free_kernel(gd->kernel_lens_vignette);
  free(module->data);
  module->data = NULL;
}

/* lensfun gui start */

/// ############################################################
/// gui stuff: inspired by ufraws lensfun tab:

/* simple function to compute the floating-point precision
   which is enough for "normal use". The criteria is to have
   about 3 leading digits after the initial zeros.  */
static int _precision(double x, double adj)
{
  x *= adj;

  if(x == 0) return 1;
  if(x < 1.0)
    if(x < 0.1)
      if(x < 0.01)
        return 5;
      else
        return 4;
    else
      return 3;
  else if(x < 100.0)
    if(x < 10.0)
      return 2;
    else
      return 1;
  else
    return 0;
}

/* -- ufraw ptr array functions -- */

static int _ptr_array_insert_sorted(
        GPtrArray *array,
        const void *item,
        GCompareFunc compare)
{
  int length = array->len;
  g_ptr_array_set_size(array, length + 1);
  const void **root = (const void **)array->pdata;

  int m = 0, l = 0, r = length - 1;

  // Skip trailing NULL, if any
  if(l <= r && !root[r]) r--;

  while(l <= r)
  {
    m = (l + r) / 2;
    int cmp = compare(root[m], item);

    if(cmp == 0)
    {
      ++m;
      goto done;
    }
    else if(cmp < 0)
      l = m + 1;
    else
      r = m - 1;
  }
  if(r == m) m++;

done:
  memmove(root + m + 1, root + m, sizeof(void *) * (length - m));
  root[m] = item;
  return m;
}

static int _ptr_array_find_sorted(
        const GPtrArray *array,
        const void *item,
        GCompareFunc compare)
{
  int length = array->len;
  void **root = array->pdata;

  int l = 0, r = length - 1;
  int m = 0, cmp = 0;

  if(!length) return -1;

  // Skip trailing NULL, if any
  if(!root[r]) r--;

  while(l <= r)
  {
    m = (l + r) / 2;
    cmp = compare(root[m], item);

    if(cmp == 0)
      return m;
    else if(cmp < 0)
      l = m + 1;
    else
      r = m - 1;
  }

  return -1;
}

static void _ptr_array_insert_index(
        GPtrArray *array,
        const void *item,
        int index)
{
  const void **root;
  int length = array->len;
  g_ptr_array_set_size(array, length + 1);
  root = (const void **)array->pdata;
  memmove(root + index + 1, root + index, sizeof(void *) * (length - index));
  root[index] = item;
}

/* -- end ufraw ptr array functions -- */

/* -- camera -- */

static void _camera_set(dt_iop_module_t *self, const lfCamera *cam)
{
  dt_iop_lens_gui_data_t *g = (dt_iop_lens_gui_data_t *)self->gui_data;
  dt_iop_lens_params_t *p = (dt_iop_lens_params_t *)self->params;
  gchar *fm;
  const char *maker, *model, *variant;
  char _variant[100];

  if(!cam)
  {
    gtk_label_set_text(GTK_LABEL(gtk_bin_get_child(GTK_BIN(g->camera_model))), "");
    gtk_widget_set_tooltip_text(GTK_WIDGET(g->camera_model), "");
    return;
  }

  g_strlcpy(p->camera, cam->Model, sizeof(p->camera));
  p->crop = cam->CropFactor;
  g->camera = cam;

  maker = lf_mlstr_get(cam->Maker);
  model = lf_mlstr_get(cam->Model);
  variant = lf_mlstr_get(cam->Variant);

  if(model)
  {
    if(maker)
      fm = g_strdup_printf("%s, %s", maker, model);
    else
      fm = g_strdup_printf("%s", model);
    gtk_label_set_text(GTK_LABEL(gtk_bin_get_child(GTK_BIN(g->camera_model))), fm);
    g_free(fm);
  }

  if(variant)
    snprintf(_variant, sizeof(_variant), " (%s)", variant);
  else
    _variant[0] = 0;

  fm = g_strdup_printf(_("maker:\t\t%s\n"
                         "model:\t\t%s%s\n"
                         "mount:\t\t%s\n"
                         "crop factor:\t%.1f"),
                       maker, model, _variant, cam->Mount, cam->CropFactor);
  gtk_widget_set_tooltip_text(GTK_WIDGET(g->camera_model), fm);
  g_free(fm);
}

static void _camera_menu_select(GtkMenuItem *menuitem, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  _camera_set(self, (lfCamera *)g_object_get_data(G_OBJECT(menuitem), "lfCamera"));
  if(darktable.gui->reset) return;
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void camera_menu_fill(dt_iop_module_t *self, const lfCamera *const *camlist)
{
  dt_iop_lens_gui_data_t *g = (dt_iop_lens_gui_data_t *)self->gui_data;
  unsigned i;
  GPtrArray *makers, *submenus;

  if(g->camera_menu)
  {
    gtk_widget_destroy(GTK_WIDGET(g->camera_menu));
    g->camera_menu = NULL;
  }

  /* Count all existing camera makers and create a sorted list */
  makers = g_ptr_array_new();
  submenus = g_ptr_array_new();
  for(i = 0; camlist[i]; i++)
  {
    GtkWidget *submenu, *item;
    const char *m = lf_mlstr_get(camlist[i]->Maker);
    int idx = _ptr_array_find_sorted(makers, m, (GCompareFunc)g_utf8_collate);
    if(idx < 0)
    {
      /* No such maker yet, insert it into the array */
      idx = _ptr_array_insert_sorted(makers, m, (GCompareFunc)g_utf8_collate);
      /* Create a submenu for cameras by this maker */
      submenu = gtk_menu_new();
      _ptr_array_insert_index(submenus, submenu, idx);
    }

    submenu = (GtkWidget *)g_ptr_array_index(submenus, idx);
    /* Append current camera name to the submenu */
    m = lf_mlstr_get(camlist[i]->Model);
    if(!camlist[i]->Variant)
      item = gtk_menu_item_new_with_label(m);
    else
    {
      gchar *fm = g_strdup_printf("%s (%s)", m, camlist[i]->Variant);
      item = gtk_menu_item_new_with_label(fm);
      g_free(fm);
    }
    gtk_widget_show(item);
    g_object_set_data(G_OBJECT(item), "lfCamera", (void *)camlist[i]);
    g_signal_connect(G_OBJECT(item), "activate", G_CALLBACK(_camera_menu_select), self);
    gtk_menu_shell_append(GTK_MENU_SHELL(submenu), item);
  }

  g->camera_menu = GTK_MENU(gtk_menu_new());
  for(i = 0; i < makers->len; i++)
  {
    GtkWidget *item = (GtkWidget *)gtk_menu_item_new_with_label((const gchar *)g_ptr_array_index(makers, i));
    gtk_widget_show(item);
    gtk_menu_shell_append(GTK_MENU_SHELL(g->camera_menu), item);
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), (GtkWidget *)g_ptr_array_index(submenus, i));
  }

  g_ptr_array_free(submenus, TRUE);
  g_ptr_array_free(makers, TRUE);
}

static void _parse_model(const char *txt, char *model, size_t sz_model)
{
  while(txt[0] && isspace(txt[0])) txt++;
  size_t len = strlen(txt);
  if(len > sz_model - 1) len = sz_model - 1;
  memcpy(model, txt, len);
  model[len] = 0;
}

static void _camera_menusearch_clicked(GtkWidget *button, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_lens_global_data_t *gd = (dt_iop_lens_global_data_t *)self->global_data;
  lfDatabase *dt_iop_lensfun_db = (lfDatabase *)gd->db;
  dt_iop_lens_gui_data_t *g = (dt_iop_lens_gui_data_t *)self->gui_data;

  (void)button;

  const lfCamera *const *camlist;
  dt_pthread_mutex_lock(&darktable.plugin_threadsafe);
  camlist = dt_iop_lensfun_db->GetCameras();
  dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);
  if(!camlist) return;
  camera_menu_fill(self, camlist);

  dt_gui_menu_popup(GTK_MENU(g->camera_menu), button, GDK_GRAVITY_SOUTH, GDK_GRAVITY_NORTH);
}

static void _camera_autosearch_clicked(GtkWidget *button, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_lens_global_data_t *gd = (dt_iop_lens_global_data_t *)self->global_data;
  lfDatabase *dt_iop_lensfun_db = (lfDatabase *)gd->db;
  dt_iop_lens_gui_data_t *g = (dt_iop_lens_gui_data_t *)self->gui_data;
  char make[200], model[200];
  const gchar *txt = (const gchar *)((dt_iop_lens_params_t *)self->default_params)->camera;

  (void)button;

  if(txt[0] == '\0')
  {
    const lfCamera *const *camlist;
    dt_pthread_mutex_lock(&darktable.plugin_threadsafe);
    camlist = dt_iop_lensfun_db->GetCameras();
    dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);
    if(!camlist) return;
    camera_menu_fill(self, camlist);
  }
  else
  {
    _parse_model(txt, model, sizeof(model));
    dt_pthread_mutex_lock(&darktable.plugin_threadsafe);
    const lfCamera **camlist = dt_iop_lensfun_db->FindCamerasExt(make, model, 0);
    dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);
    if(!camlist) return;
    camera_menu_fill(self, camlist);
    lf_free(camlist);
  }

  dt_gui_menu_popup(GTK_MENU(g->camera_menu), button, GDK_GRAVITY_SOUTH_EAST, GDK_GRAVITY_NORTH_EAST);
}

/* -- end camera -- */

static void _lens_comboentry_focal_update(GtkWidget *widget, dt_iop_module_t *self)
{
  dt_iop_lens_params_t *p = (dt_iop_lens_params_t *)self->params;
  const char *text = dt_bauhaus_combobox_get_text(widget);
  if(text) (void)sscanf(text, "%f", &p->focal);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void _lens_comboentry_aperture_update(GtkWidget *widget, dt_iop_module_t *self)
{
  dt_iop_lens_params_t *p = (dt_iop_lens_params_t *)self->params;
  const char *text = dt_bauhaus_combobox_get_text(widget);
  if(text) (void)sscanf(text, "%f", &p->aperture);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void _lens_comboentry_distance_update(GtkWidget *widget, dt_iop_module_t *self)
{
  dt_iop_lens_params_t *p = (dt_iop_lens_params_t *)self->params;
  const char *text = dt_bauhaus_combobox_get_text(widget);
  if(text) (void)sscanf(text, "%f", &p->distance);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void _delete_children(GtkWidget *widget, gpointer data)
{
  (void)data;
  gtk_widget_destroy(widget);
}

static void _lens_set(dt_iop_module_t *self, const lfLens *lens)
{
  dt_iop_lens_gui_data_t *g = (dt_iop_lens_gui_data_t *)self->gui_data;
  dt_iop_lens_params_t *p = (dt_iop_lens_params_t *)self->params;

  gchar *fm;
  const char *maker, *model;
  unsigned i;
  gdouble focal_values[]
      = { -INFINITY, 4.5, 8,   10,  12,  14,  15,  16,  17,  18,  20,  24,  28,   30,      31,  35,
          38,        40,  43,  45,  50,  55,  60,  70,  75,  77,  80,  85,  90,   100,     105, 110,
          120,       135, 150, 200, 210, 240, 250, 300, 400, 500, 600, 800, 1000, INFINITY };
  gdouble aperture_values[]
      = { -INFINITY, 0.7, 0.8, 0.9, 1, 1.1, 1.2, 1.4, 1.8, 2,  2.2, 2.5, 2.8, 3.2, 3.4, 4,  4.5, 5.0,
          5.6,       6.3, 7.1, 8,   9, 10,  11,  13,  14,  16, 18,  20,  22,  25,  29,  32, 38,  INFINITY };

  if(!lens)
  {
    g->lensfun_trouble = TRUE;
    return;
  }
  else
  {
    // no longer in trouble
    g->lensfun_trouble = FALSE;
  }

  maker = lf_mlstr_get(lens->Maker);
  model = lf_mlstr_get(lens->Model);

  g_strlcpy(p->lens, lens->Model, sizeof(p->lens));

  if(model)
  {
    if(maker)
      fm = g_strdup_printf("%s, %s", maker, model);
    else
      fm = g_strdup_printf("%s", model);
    gtk_label_set_text(GTK_LABEL(gtk_bin_get_child(GTK_BIN(g->lens_model))), fm);
    g_free(fm);
  }

  char focal[100], aperture[100], mounts[200];

  if(lens->MinFocal < lens->MaxFocal)
    snprintf(focal, sizeof(focal), "%g-%gmm", lens->MinFocal, lens->MaxFocal);
  else
    snprintf(focal, sizeof(focal), "%gmm", lens->MinFocal);
  if(lens->MinAperture < lens->MaxAperture)
    snprintf(aperture, sizeof(aperture), "%g-%g", lens->MinAperture, lens->MaxAperture);
  else
    snprintf(aperture, sizeof(aperture), "%g", lens->MinAperture);

  mounts[0] = 0;
#ifdef LF_0395
  const char* const* mount_names = lens->GetMountNames();
  i = 0;
  while (mount_names && *mount_names) {
    if(i > 0) g_strlcat(mounts, ", ", sizeof(mounts));
    g_strlcat(mounts, *mount_names, sizeof(mounts));
    i++;
    mount_names++;
  }
#else
  if(lens->Mounts)
    for(i = 0; lens->Mounts[i]; i++)
    {
      if(i > 0) g_strlcat(mounts, ", ", sizeof(mounts));
      g_strlcat(mounts, lens->Mounts[i], sizeof(mounts));
    }
#endif
  fm = g_strdup_printf(_("maker:\t\t%s\n"
                         "model:\t\t%s\n"
                         "focal range:\t%s\n"
                         "aperture:\t%s\n"
                         "crop factor:\t%.1f\n"
                         "type:\t\t%s\n"
                         "mounts:\t%s"),
                       maker ? maker : "?", model ? model : "?", focal, aperture,
#ifdef LF_0395
                       g->camera->CropFactor,
#else
                       lens->CropFactor,
#endif
                       lfLens::GetLensTypeDesc(lens->Type, NULL), mounts);

  gtk_widget_set_tooltip_text(GTK_WIDGET(g->lens_model), fm);
  g_free(fm);

  /* Create the focal/aperture/distance combo boxes */
  gtk_container_foreach(GTK_CONTAINER(g->lens_param_box), _delete_children, NULL);

  int ffi = 1, fli = -1;
  for(i = 1; i < sizeof(focal_values) / sizeof(gdouble) - 1; i++)
  {
    if(focal_values[i] < lens->MinFocal) ffi = i + 1;
    if(focal_values[i] > lens->MaxFocal && fli == -1) fli = i;
  }
  if(focal_values[ffi] > lens->MinFocal)
  {
    focal_values[ffi - 1] = lens->MinFocal;
    ffi--;
  }
  if(lens->MaxFocal == 0 || fli < 0) fli = sizeof(focal_values) / sizeof(gdouble) - 2;
  if(focal_values[fli + 1] < lens->MaxFocal)
  {
    focal_values[fli + 1] = lens->MaxFocal;
    ffi++;
  }
  if(fli < ffi) fli = ffi + 1;

  GtkWidget *w;
  char txt[30];

  // focal length
  w = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(w, NULL, N_("mm"));
  gtk_widget_set_tooltip_text(w, _("focal length (mm)"));
  snprintf(txt, sizeof(txt), "%.*f", _precision(p->focal, 10.0), p->focal);
  dt_bauhaus_combobox_add(w, txt);
  for(int k = 0; k < fli - ffi; k++)
  {
    snprintf(txt, sizeof(txt), "%.*f", _precision(focal_values[ffi + k], 10.0), focal_values[ffi + k]);
    dt_bauhaus_combobox_add(w, txt);
  }
  g_signal_connect(G_OBJECT(w), "value-changed", G_CALLBACK(_lens_comboentry_focal_update), self);
  gtk_box_pack_start(GTK_BOX(g->lens_param_box), w, TRUE, TRUE, 0);
  dt_bauhaus_combobox_set_editable(w, 1);
  g->cbe[0] = w;

  // f-stop
  ffi = 1, fli = sizeof(aperture_values) / sizeof(gdouble) - 1;
  for(i = 1; i < sizeof(aperture_values) / sizeof(gdouble) - 1; i++)
    if(aperture_values[i] < lens->MinAperture) ffi = i + 1;
  if(aperture_values[ffi] > lens->MinAperture)
  {
    aperture_values[ffi - 1] = lens->MinAperture;
    ffi--;
  }

  w = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(w, NULL, N_("f/"));
  gtk_widget_set_tooltip_text(w, _("f-number (aperture)"));
  snprintf(txt, sizeof(txt), "%.*f", _precision(p->aperture, 10.0), p->aperture);
  dt_bauhaus_combobox_add(w, txt);
  for(int k = 0; k < fli - ffi; k++)
  {
    snprintf(txt, sizeof(txt), "%.*f", _precision(aperture_values[ffi + k], 10.0), aperture_values[ffi + k]);
    dt_bauhaus_combobox_add(w, txt);
  }
  g_signal_connect(G_OBJECT(w), "value-changed", G_CALLBACK(_lens_comboentry_aperture_update), self);
  gtk_box_pack_start(GTK_BOX(g->lens_param_box), w, TRUE, TRUE, 0);
  dt_bauhaus_combobox_set_editable(w, 1);
  g->cbe[1] = w;

  w = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(w, NULL, N_("d"));
  gtk_widget_set_tooltip_text(w, _("distance to subject"));
  snprintf(txt, sizeof(txt), "%.*f", _precision(p->distance, 10.0), p->distance);
  dt_bauhaus_combobox_add(w, txt);
  float val = 0.25f;
  for(int k = 0; k < 25; k++)
  {
    if(val > 1000.0f) val = 1000.0f;
    snprintf(txt, sizeof(txt), "%.*f", _precision(val, 10.0), val);
    dt_bauhaus_combobox_add(w, txt);
    if(val >= 1000.0f) break;
    val *= sqrtf(2.0f);
  }
  g_signal_connect(G_OBJECT(w), "value-changed", G_CALLBACK(_lens_comboentry_distance_update), self);
  gtk_box_pack_start(GTK_BOX(g->lens_param_box), w, TRUE, TRUE, 0);
  dt_bauhaus_combobox_set_editable(w, 1);
  g->cbe[2] = w;

  gtk_widget_show_all(g->lens_param_box);
}

static void _lens_menu_select(GtkMenuItem *menuitem, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_lens_gui_data_t *g = (dt_iop_lens_gui_data_t *)self->gui_data;
  dt_iop_lens_params_t *p = (dt_iop_lens_params_t *)self->params;
  _lens_set(self, (lfLens *)g_object_get_data(G_OBJECT(menuitem), "lfLens"));
  if(darktable.gui->reset) return;

  const float scale = _get_autoscale_lf(self, p, g->camera);
  dt_bauhaus_slider_set(g->scale, scale);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void _lens_menu_fill(dt_iop_module_t *self, const lfLens *const *lenslist)
{
  dt_iop_lens_gui_data_t *g = (dt_iop_lens_gui_data_t *)self->gui_data;
  unsigned i;
  GPtrArray *makers, *submenus;

  if(g->lens_menu)
  {
    gtk_widget_destroy(GTK_WIDGET(g->lens_menu));
    g->lens_menu = NULL;
  }

  /* Count all existing lens makers and create a sorted list */
  makers = g_ptr_array_new();
  submenus = g_ptr_array_new();
  for(i = 0; lenslist[i]; i++)
  {
    GtkWidget *submenu, *item;
    const char *m = lf_mlstr_get(lenslist[i]->Maker);
    int idx = _ptr_array_find_sorted(makers, m, (GCompareFunc)g_utf8_collate);
    if(idx < 0)
    {
      /* No such maker yet, insert it into the array */
      idx = _ptr_array_insert_sorted(makers, m, (GCompareFunc)g_utf8_collate);
      /* Create a submenu for lenses by this maker */
      submenu = gtk_menu_new();
      _ptr_array_insert_index(submenus, submenu, idx);
    }

    submenu = (GtkWidget *)g_ptr_array_index(submenus, idx);
    /* Append current lens name to the submenu */
    item = gtk_menu_item_new_with_label(lf_mlstr_get(lenslist[i]->Model));
    gtk_widget_show(item);
    g_object_set_data(G_OBJECT(item), "lfLens", (void *)lenslist[i]);
    g_signal_connect(G_OBJECT(item), "activate", G_CALLBACK(_lens_menu_select), self);
    gtk_menu_shell_append(GTK_MENU_SHELL(submenu), item);
  }

  g->lens_menu = GTK_MENU(gtk_menu_new());
  for(i = 0; i < makers->len; i++)
  {
    GtkWidget *item = gtk_menu_item_new_with_label((const gchar *)g_ptr_array_index(makers, i));
    gtk_widget_show(item);
    gtk_menu_shell_append(GTK_MENU_SHELL(g->lens_menu), item);
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), (GtkWidget *)g_ptr_array_index(submenus, i));
  }

  g_ptr_array_free(submenus, TRUE);
  g_ptr_array_free(makers, TRUE);
}

static void _lens_menusearch_clicked(GtkWidget *button, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_lens_global_data_t *gd = (dt_iop_lens_global_data_t *)self->global_data;
  lfDatabase *dt_iop_lensfun_db = (lfDatabase *)gd->db;
  dt_iop_lens_gui_data_t *g = (dt_iop_lens_gui_data_t *)self->gui_data;
  const lfLens **lenslist;

  (void)button;

  dt_pthread_mutex_lock(&darktable.plugin_threadsafe);
  lenslist = dt_iop_lensfun_db->FindLenses(g->camera, NULL, NULL, LF_SEARCH_SORT_AND_UNIQUIFY);
  dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);

  if(!lenslist) return;
  _lens_menu_fill(self, lenslist);
  lf_free(lenslist);

  dt_gui_menu_popup(GTK_MENU(g->lens_menu), button, GDK_GRAVITY_SOUTH, GDK_GRAVITY_NORTH);
}

static void _lens_autosearch_clicked(GtkWidget *button, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_lens_global_data_t *gd = (dt_iop_lens_global_data_t *)self->global_data;
  lfDatabase *dt_iop_lensfun_db = (lfDatabase *)gd->db;
  dt_iop_lens_gui_data_t *g = (dt_iop_lens_gui_data_t *)self->gui_data;
  const lfLens **lenslist;
  char model[200];
  const gchar *txt = ((dt_iop_lens_params_t *)self->default_params)->lens;

  (void)button;

  _parse_model(txt, model, sizeof(model));
  dt_pthread_mutex_lock(&darktable.plugin_threadsafe);
  lenslist = dt_iop_lensfun_db->FindLenses(g->camera, NULL,
                                           model[0] ? model : NULL, LF_SEARCH_SORT_AND_UNIQUIFY);
  dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);
  if(!lenslist) return;
  _lens_menu_fill(self, lenslist);
  lf_free(lenslist);

  dt_gui_menu_popup(GTK_MENU(g->lens_menu), button, GDK_GRAVITY_SOUTH_EAST, GDK_GRAVITY_NORTH_EAST);
}

/* -- end lens -- */

static void _autoscale_pressed_lf(GtkWidget *button, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_lens_gui_data_t *g = (dt_iop_lens_gui_data_t *)self->gui_data;
  dt_iop_lens_params_t *p = (dt_iop_lens_params_t *)self->params;

  const float scale = _get_autoscale_lf(self, p, g->camera);
  dt_bauhaus_slider_set(g->scale, scale);
}

static void _target_geometry_changed(GtkWidget *widget, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_lens_params_t *p = (dt_iop_lens_params_t *)self->params;

  const int pos = dt_bauhaus_combobox_get(widget);
  p->target_geom = (pos + DT_IOP_LENS_LENSTYPE_UNKNOWN + 1);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}
/* -- lensfun gui end -- */

static void _display_errors(struct dt_iop_module_t *self)
{
  dt_iop_lens_gui_data_t *g = (dt_iop_lens_gui_data_t *)self->gui_data;
  dt_iop_lens_params_t *p = (dt_iop_lens_params_t *)self->params;

  if(g->lensfun_trouble
     && self->enabled
     && p->method == DT_IOP_LENS_METHOD_LENSFUN)
  {
    dt_iop_set_module_trouble_message(self, _("camera/lens not found"),
                                      _("please select your lens manually\n"
                                        "you might also want to check if your lensfun database is up-to-date\n"
                                        "by running lensfun_update_data"),
                                      "camera/lens not found");
  }
  else
  {
    dt_iop_set_module_trouble_message(self, NULL, NULL, NULL);
  }

  gtk_widget_queue_draw(self->widget);
}

static void _modflags_changed(GtkWidget *widget, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(darktable.gui->reset) return;

  dt_iop_lens_params_t *p = (dt_iop_lens_params_t *)self->params;
  dt_iop_lens_gui_data_t *g = (dt_iop_lens_gui_data_t *)self->gui_data;

  const int pos = dt_bauhaus_combobox_get(widget);
  for(GList *modifiers = g->modifiers;  modifiers; modifiers = g_list_next(modifiers))
  {
    dt_iop_lens_gui_modifier_t *mm = (dt_iop_lens_gui_modifier_t *)modifiers->data;
    if(mm->pos == pos)
    {
      p->modify_flags = mm->modflag;
      dt_dev_add_history_item(darktable.develop, self, TRUE);
      break;
    }
  }
}

void gui_changed(dt_iop_module_t *self, GtkWidget *w, void *previous)
{
  dt_iop_lens_params_t *p = (dt_iop_lens_params_t *)self->params;
  dt_iop_lens_gui_data_t *g = (dt_iop_lens_gui_data_t *)self->gui_data;

  // enable methods selector combobox only if more than 1 methods are available
  gtk_widget_set_sensitive(g->methods_selector, _have_embedded_metadata(self) > 0);

  if(p->method == DT_IOP_LENS_METHOD_LENSFUN)
  {
    gtk_stack_set_visible_child_name(GTK_STACK(g->methods), "lensfun");

    gtk_widget_set_sensitive(GTK_WIDGET(g->modflags), !g->lensfun_trouble);
    gtk_widget_set_sensitive(GTK_WIDGET(g->target_geom), !g->lensfun_trouble);
    gtk_widget_set_sensitive(GTK_WIDGET(g->scale), !g->lensfun_trouble);
    gtk_widget_set_sensitive(GTK_WIDGET(g->reverse), !g->lensfun_trouble);
    gtk_widget_set_sensitive(GTK_WIDGET(g->tca_r), !g->lensfun_trouble);
    gtk_widget_set_sensitive(GTK_WIDGET(g->tca_b), !g->lensfun_trouble);
    gtk_widget_set_sensitive(GTK_WIDGET(g->message), !g->lensfun_trouble);

    const gboolean raw_monochrome = dt_image_is_monochrome(&self->dev->image_storage);
    gtk_widget_set_visible(g->tca_override, !raw_monochrome);

    // show tca sliders only if tca_overwrite is set
    gtk_widget_set_visible(g->tca_r, p->tca_override && !raw_monochrome);
    gtk_widget_set_visible(g->tca_b, p->tca_override && !raw_monochrome);

  }
  else
  {
    gtk_stack_set_visible_child_name(GTK_STACK(g->methods), "metadata");

    const dt_image_t *img = &self->dev->image_storage;
    const dt_image_correction_data_t *cd = &img->exif_correction_data;

    const gboolean has_warp = (img->exif_correction_type == CORRECTION_TYPE_DNG) ? cd->dng.has_warp : TRUE;
    const gboolean has_vign = (img->exif_correction_type == CORRECTION_TYPE_DNG) ? cd->dng.has_vignette : TRUE;

    gtk_widget_set_visible(g->cor_dist_ft, has_warp);
    gtk_widget_set_visible(g->cor_vig_ft, has_vign);
    gtk_widget_set_visible(g->cor_scale, has_warp);

    gtk_widget_set_sensitive(GTK_WIDGET(g->modflags), TRUE);
    gtk_widget_set_sensitive(GTK_WIDGET(g->message), TRUE);
  }

  _display_errors(self);
}

static void _have_corrections_done(gpointer instance, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_lens_gui_data_t *g = (dt_iop_lens_gui_data_t *)self->gui_data;
  if(darktable.gui->reset) return;

  dt_iop_gui_enter_critical_section(self);
  const int corrections_done = g->corrections_done;
  dt_iop_gui_leave_critical_section(self);

  const char empty_message[] = "";
  char *message = (char *)empty_message;
  for(GList *modifiers = g->modifiers; modifiers && self->enabled; modifiers = g_list_next(modifiers))
  {
    dt_iop_lens_gui_modifier_t *mm = (dt_iop_lens_gui_modifier_t *)modifiers->data;
    if(mm->modflag == corrections_done)
    {
      message = mm->name;
      break;
    }
  }

  ++darktable.gui->reset;
  gtk_label_set_text(g->message, message);
  gtk_widget_set_tooltip_text(GTK_WIDGET(g->message), message);
  --darktable.gui->reset;
}

static void _develop_ui_pipe_finished_callback(gpointer instance, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  _display_errors(self);
}

void gui_init(struct dt_iop_module_t *self)
{
  dt_iop_lens_gui_data_t *g = IOP_GUI_ALLOC(lens);

  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_DEVELOP_UI_PIPE_FINISHED,
                            G_CALLBACK(_develop_ui_pipe_finished_callback), self);

  g->modifiers = NULL;
  g->camera = NULL;
  g->camera_menu = NULL;
  g->lens_menu = NULL;

  dt_iop_gui_enter_critical_section(self); // not actually needed, we're the only one with a ref to this instance
  g->corrections_done = -1;
  dt_iop_gui_leave_critical_section(self);

  // initialize modflags options
  int pos = -1;
  dt_iop_lens_gui_modifier_t *modifier;
  modifier = (dt_iop_lens_gui_modifier_t *)g_malloc0(sizeof(dt_iop_lens_gui_modifier_t));
  dt_utf8_strlcpy(modifier->name, _("none"), sizeof(modifier->name));
  g->modifiers = g_list_append(g->modifiers, modifier);
  modifier->modflag = DT_IOP_LENS_MODFLAG_NONE;
  modifier->pos = ++pos;

  modifier = (dt_iop_lens_gui_modifier_t *)g_malloc0(sizeof(dt_iop_lens_gui_modifier_t));
  dt_utf8_strlcpy(modifier->name, _("all"), sizeof(modifier->name));
  g->modifiers = g_list_append(g->modifiers, modifier);
  modifier->modflag = DT_IOP_LENS_MODFLAG_ALL;
  modifier->pos = ++pos;

  modifier = (dt_iop_lens_gui_modifier_t *)g_malloc0(sizeof(dt_iop_lens_gui_modifier_t));
  dt_utf8_strlcpy(modifier->name, _("distortion & TCA"), sizeof(modifier->name));
  g->modifiers = g_list_append(g->modifiers, modifier);
  modifier->modflag = DT_IOP_LENS_MODFLAG_DIST_TCA;
  modifier->pos = ++pos;

  modifier = (dt_iop_lens_gui_modifier_t *)g_malloc0(sizeof(dt_iop_lens_gui_modifier_t));
  dt_utf8_strlcpy(modifier->name, _("distortion & vignetting"), sizeof(modifier->name));
  g->modifiers = g_list_append(g->modifiers, modifier);
  modifier->modflag = DT_IOP_LENS_MODFLAG_DIST_VIGN;
  modifier->pos = ++pos;

  modifier = (dt_iop_lens_gui_modifier_t *)g_malloc0(sizeof(dt_iop_lens_gui_modifier_t));
  dt_utf8_strlcpy(modifier->name, _("TCA & vignetting"), sizeof(modifier->name));
  g->modifiers = g_list_append(g->modifiers, modifier);
  modifier->modflag = DT_IOP_LENS_MODFLAG_TCA_VIGN;
  modifier->pos = ++pos;

  modifier = (dt_iop_lens_gui_modifier_t *)g_malloc0(sizeof(dt_iop_lens_gui_modifier_t));
  dt_utf8_strlcpy(modifier->name, _("only distortion"), sizeof(modifier->name));
  g->modifiers = g_list_append(g->modifiers, modifier);
  modifier->modflag = DT_IOP_LENS_MODFLAG_DIST;
  modifier->pos = ++pos;

  modifier = (dt_iop_lens_gui_modifier_t *)g_malloc0(sizeof(dt_iop_lens_gui_modifier_t));
  dt_utf8_strlcpy(modifier->name, _("only TCA"), sizeof(modifier->name));
  g->modifiers = g_list_append(g->modifiers, modifier);
  modifier->modflag = DT_IOP_LENS_MODFLAG_TCA;
  modifier->pos = ++pos;

  modifier = (dt_iop_lens_gui_modifier_t *)g_malloc0(sizeof(dt_iop_lens_gui_modifier_t));
  dt_utf8_strlcpy(modifier->name, _("only vignetting"), sizeof(modifier->name));
  g->modifiers = g_list_append(g->modifiers, modifier);
  modifier->modflag = DT_IOP_LENS_MODFLAG_VIGN;
  modifier->pos = ++pos;

  /* lensfun widget */
  // _from_params methods assign widgets to self->widget, so temporarily set self->widget to our widget
  GtkWidget *box_lf = self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  // camera selector
  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  g->camera_model = dt_iop_button_new(self, N_("camera model"),
                                      G_CALLBACK(_camera_menusearch_clicked),
                                      FALSE, 0, (GdkModifierType)0,
                                      NULL, 0, hbox);
  g->find_camera_button = dt_iop_button_new(self, N_("find camera"),
                                            G_CALLBACK(_camera_autosearch_clicked),
                                            FALSE, 0, (GdkModifierType)0,
                                            dtgtk_cairo_paint_solid_arrow, CPF_DIRECTION_DOWN, NULL);
  dt_gui_add_class(g->find_camera_button, "dt_big_btn_canvas");
  gtk_box_pack_start(GTK_BOX(hbox), g->find_camera_button, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(box_lf), hbox, TRUE, TRUE, 0);

  // lens selector
  hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  g->lens_model = dt_iop_button_new(self, N_("lens model"),
                                    G_CALLBACK(_lens_menusearch_clicked),
                                    FALSE, 0, (GdkModifierType)0,
                                    NULL, 0, hbox);
  g->find_lens_button = dt_iop_button_new(self, N_("find lens"),
                                          G_CALLBACK(_lens_autosearch_clicked),
                                          FALSE, 0, (GdkModifierType)0,
                                          dtgtk_cairo_paint_solid_arrow, CPF_DIRECTION_DOWN, NULL);
  dt_gui_add_class(g->find_lens_button, "dt_big_btn_canvas");
  gtk_box_pack_start(GTK_BOX(hbox), g->find_lens_button, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(box_lf), hbox, TRUE, TRUE, 0);

  // lens properties
  g->lens_param_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_start(GTK_BOX(box_lf), g->lens_param_box, TRUE, TRUE, 0);

#if 0
  // if unambiguous info is there, use it.
  if(self->dev->image_storage.exif_lens[0] != '\0')
  {
    char make [200], model [200];
    const gchar *txt = gtk_entry_get_text(GTK_ENTRY(g->lens_model));
    parse_maker_model (txt, make, sizeof (make), model, sizeof (model));
    dt_pthread_mutex_lock(&darktable.plugin_threadsafe);
    const lfLens **lenslist = lf_db_find_lenses_hd (dt_iop_lensfun_db, g->camera,
                              make [0] ? make : NULL,
                              model [0] ? model : NULL, 0);
    if(lenslist) lens_set (self, lenslist[0]);
    lf_free (lenslist);
    dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);
  }
#endif

  // target geometry
  g->target_geom = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->target_geom, NULL, N_("geometry"));
  gtk_box_pack_start(GTK_BOX(box_lf), g->target_geom, TRUE, TRUE, 0);
  gtk_widget_set_tooltip_text(g->target_geom, _("target geometry"));
  dt_bauhaus_combobox_add(g->target_geom, _("rectilinear"));
  dt_bauhaus_combobox_add(g->target_geom, _("fish-eye"));
  dt_bauhaus_combobox_add(g->target_geom, _("panoramic"));
  dt_bauhaus_combobox_add(g->target_geom, _("equirectangular"));
#if LF_VERSION >= ((0 << 24) | (2 << 16) | (6 << 8) | 0)
  dt_bauhaus_combobox_add(g->target_geom, _("orthographic"));
  dt_bauhaus_combobox_add(g->target_geom, _("stereographic"));
  dt_bauhaus_combobox_add(g->target_geom, _("equisolid angle"));
  dt_bauhaus_combobox_add(g->target_geom, _("thoby fish-eye"));
#endif
  g_signal_connect(G_OBJECT(g->target_geom), "value-changed", G_CALLBACK(_target_geometry_changed),
                   (gpointer)self);

  // scale
  g->scale = dt_bauhaus_slider_from_params(self, N_("scale"));
  dt_bauhaus_slider_set_digits(g->scale, 3);
  dt_bauhaus_widget_set_quad_paint(g->scale, dtgtk_cairo_paint_refresh, 0, NULL);
  g_signal_connect(G_OBJECT(g->scale), "quad-pressed", G_CALLBACK(_autoscale_pressed_lf), self);
  gtk_widget_set_tooltip_text(g->scale, _("auto scale"));

  // reverse direction
  g->reverse = dt_bauhaus_combobox_from_params(self, "inverse");
  dt_bauhaus_combobox_add(g->reverse, _("correct"));
  dt_bauhaus_combobox_add(g->reverse, _("distort"));
  gtk_widget_set_tooltip_text(g->reverse, _("correct distortions or apply them"));

  g->tca_override = dt_bauhaus_toggle_from_params(self, "tca_override");

  // override linear tca (if not 1.0):
  g->tca_r = dt_bauhaus_slider_from_params(self, "tca_r");
  dt_bauhaus_slider_set_digits(g->tca_r, 5);
  gtk_widget_set_tooltip_text(g->tca_r, _("Transversal Chromatic Aberration red"));

  g->tca_b = dt_bauhaus_slider_from_params(self, "tca_b");
  dt_bauhaus_slider_set_digits(g->tca_b, 5);
  gtk_widget_set_tooltip_text(g->tca_b, _("Transversal Chromatic Aberration blue"));

  /* embedded metadata widgets */
  GtkWidget *box_md = self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  g->cor_dist_ft = dt_bauhaus_slider_from_params(self, "cor_dist_ft");
  dt_bauhaus_slider_set_digits(g->cor_dist_ft, 3);
  gtk_widget_set_tooltip_text(g->cor_dist_ft, _("tune the warp and chromatic aberration correction"));

  g->cor_vig_ft = dt_bauhaus_slider_from_params(self, "cor_vig_ft");
  dt_bauhaus_slider_set_digits(g->cor_vig_ft, 3);
  gtk_widget_set_tooltip_text(g->cor_vig_ft, _("tune the vignette correction"));

  g->cor_scale = dt_bauhaus_slider_from_params(self, "cor_scale");
  dt_bauhaus_slider_set_digits(g->cor_scale, 4);
  dt_bauhaus_widget_set_quad_paint(g->cor_scale, dtgtk_cairo_paint_refresh, 0, NULL);
  g_signal_connect(G_OBJECT(g->cor_scale), "quad-pressed", G_CALLBACK(_autoscale_pressed_md), self);
  gtk_widget_set_tooltip_text(g->cor_scale, _("override automatic scale"));

  // main widget
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
  gtk_widget_set_name(self->widget, "lens-module");

  // selector for correction method
  g->methods_selector = dt_bauhaus_combobox_from_params(self, "method");

  // selector for correction type (modflags): one or more out of distortion, TCA, vignetting
  g->modflags = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->modflags, NULL, N_("corrections"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->modflags, TRUE, TRUE, 0);
  gtk_widget_set_tooltip_text(g->modflags, _("which corrections to apply"));

  GList *l = g->modifiers;
  while(l)
  {
    modifier = (dt_iop_lens_gui_modifier_t *)l->data;
    dt_bauhaus_combobox_add(g->modflags, modifier->name);
    l = g_list_next(l);
  }
  dt_bauhaus_combobox_set(g->modflags, 0);
  g_signal_connect(G_OBJECT(g->modflags), "value-changed",
                   G_CALLBACK(_modflags_changed), (gpointer)self);

  g->methods = gtk_stack_new();
  gtk_stack_set_homogeneous(GTK_STACK(g->methods), FALSE);
  gtk_box_pack_start(GTK_BOX(self->widget), g->methods, TRUE, TRUE, 0);

  gtk_stack_add_named(GTK_STACK(g->methods), box_lf, "lensfun");
  gtk_stack_add_named(GTK_STACK(g->methods), box_md, "metadata");

  // message box to inform user what corrections have been done. this
  // is useful as depending on lensfuns profile only some of the lens
  // flaws can be corrected
  GtkBox *hbox1 = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0));
  GtkWidget *label = gtk_label_new(_("corrections done: "));
  gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_MIDDLE);
  gtk_widget_set_tooltip_text(label, _("which corrections have actually been done"));
  gtk_box_pack_start(GTK_BOX(hbox1), label, FALSE, FALSE, 0);
  g->message = GTK_LABEL(gtk_label_new("")); // This gets filled in by process
  gtk_label_set_ellipsize(GTK_LABEL(g->message), PANGO_ELLIPSIZE_MIDDLE);
  gtk_box_pack_start(GTK_BOX(hbox1), GTK_WIDGET(g->message), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hbox1), TRUE, TRUE, 0);

  /* add signal handler for preview pipe finish to update message on corrections done */
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_DEVELOP_PREVIEW_PIPE_FINISHED,
                            G_CALLBACK(_have_corrections_done), self);
}

void gui_focus(struct dt_iop_module_t *self, gboolean in)
{
  _display_errors(self);
}

void gui_update(struct dt_iop_module_t *self)
{
  // let gui elements reflect params
  dt_iop_lens_gui_data_t *g = (dt_iop_lens_gui_data_t *)self->gui_data;
  dt_iop_lens_params_t *p = (dt_iop_lens_params_t *)self->params;

  const int modflag = p->modify_flags;
  for(GList *modifiers = g->modifiers; modifiers; modifiers = g_list_next(modifiers))
  {
    dt_iop_lens_gui_modifier_t *mm = (dt_iop_lens_gui_modifier_t *)modifiers->data;
    if(mm->modflag == modflag)
    {
      dt_bauhaus_combobox_set(g->modflags, mm->pos);
      break;
    }
  }

  dt_iop_lens_global_data_t *gd = (dt_iop_lens_global_data_t *)self->global_data;
  lfDatabase *dt_iop_lensfun_db = (lfDatabase *)gd->db;

  // these are the wrong (untranslated) strings in general but that's
  // ok, they will be overwritten further down
  gtk_label_set_text(GTK_LABEL(gtk_bin_get_child(GTK_BIN(g->camera_model))), p->camera);
  gtk_label_set_text(GTK_LABEL(gtk_bin_get_child(GTK_BIN(g->lens_model))), p->lens);
  gtk_widget_set_tooltip_text(g->camera_model, "");
  gtk_widget_set_tooltip_text(g->lens_model, "");

  dt_bauhaus_combobox_set(g->target_geom, p->target_geom - LF_UNKNOWN - 1);
  dt_bauhaus_combobox_set(g->reverse, p->inverse);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->tca_override), p->tca_override);

  const lfCamera **cam = NULL;
  g->camera = NULL;
  if(p->camera[0])
  {
    dt_pthread_mutex_lock(&darktable.plugin_threadsafe);
    cam = dt_iop_lensfun_db->FindCamerasExt(NULL, p->camera, 0);
    dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);
    if(cam)
      _camera_set(self, cam[0]);
    else
      _camera_set(self, NULL);
  }

  if(g->camera && p->lens[0])
  {
    char model[200];
    _parse_model(p->lens, model, sizeof(model));
    dt_pthread_mutex_lock(&darktable.plugin_threadsafe);
    const lfLens **lenslist = dt_iop_lensfun_db->FindLenses(g->camera, NULL,
                                                            model[0] ? model : NULL, 0);
    if(lenslist)
      _lens_set(self, lenslist[0]);
    else
      _lens_set(self, NULL);
    lf_free(lenslist);
    dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);
  }
  else
  {
    dt_pthread_mutex_lock(&darktable.plugin_threadsafe);
    _lens_set(self, NULL);
    dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);
  }


  gui_changed(self, NULL, NULL);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  dt_iop_lens_gui_data_t *g = (dt_iop_lens_gui_data_t *)self->gui_data;

  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_have_corrections_done), self);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals,
                                     G_CALLBACK(_develop_ui_pipe_finished_callback), self);

  while(g->modifiers)
  {
    g_free(g->modifiers->data);
    g->modifiers = g_list_delete_link(g->modifiers, g->modifiers);
  }

  IOP_GUI_FREE;
}

}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
