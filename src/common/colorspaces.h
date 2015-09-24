/*
    This file is part of darktable,
    copyright (c) 2009--2010 johannes hanika.

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
#ifndef DT_COLORSPACES_H
#define DT_COLORSPACES_H

#include "common/darktable.h"
#include <lcms2.h>

// constants fit to the ones from lcms.h:
typedef enum dt_iop_color_intent_t
{
  DT_INTENT_PERCEPTUAL = INTENT_PERCEPTUAL,                       // 0
  DT_INTENT_RELATIVE_COLORIMETRIC = INTENT_RELATIVE_COLORIMETRIC, // 1
  DT_INTENT_SATURATION = INTENT_SATURATION,                       // 2
  DT_INTENT_ABSOLUTE_COLORIMETRIC = INTENT_ABSOLUTE_COLORIMETRIC, // 3
  DT_INTENT_LAST
} dt_iop_color_intent_t;

typedef enum dt_colorspaces_color_profile_type_t
{
  DT_COLORSPACE_NONE = -1,
  DT_COLORSPACE_FILE = 0,
  DT_COLORSPACE_SRGB = 1,
  DT_COLORSPACE_ADOBERGB = 2,
  DT_COLORSPACE_LIN_REC709 = 3,
  DT_COLORSPACE_LIN_REC2020 = 4,
  DT_COLORSPACE_XYZ = 5,
  DT_COLORSPACE_LAB = 6,
  DT_COLORSPACE_INFRARED = 7,
  DT_COLORSPACE_DISPLAY = 8,
  DT_COLORSPACE_EMBEDDED_ICC = 9,
  DT_COLORSPACE_EMBEDDED_MATRIX = 10,
  DT_COLORSPACE_STANDARD_MATRIX = 11,
  DT_COLORSPACE_ENHANCED_MATRIX = 12,
  DT_COLORSPACE_VENDOR_MATRIX = 13,
  DT_COLORSPACE_ALTERNATE_MATRIX = 14,
  DT_COLORSPACE_LAST = 15
} dt_colorspaces_color_profile_type_t;

typedef enum dt_colorspaces_color_mode_t
{
  DT_PROFILE_NORMAL = 0,
  DT_PROFILE_SOFTPROOF,
  DT_PROFILE_GAMUTCHECK
} dt_colorspaces_color_mode_t;

typedef enum dt_colorspaces_profile_direction_t
{
  DT_PROFILE_DIRECTION_IN = 1 << 0,
  DT_PROFILE_DIRECTION_OUT = 1 << 1,
  DT_PROFILE_DIRECTION_DISPLAY = 1 << 2,
  DT_PROFILE_DIRECTION_ANY = DT_PROFILE_DIRECTION_IN | DT_PROFILE_DIRECTION_OUT | DT_PROFILE_DIRECTION_DISPLAY
} dt_colorspaces_profile_direction_t;

typedef struct dt_colorspaces_t
{
  GList *profiles;

  // xatom color profile:
  pthread_rwlock_t xprofile_lock;
  gchar *colord_profile_file;
  uint8_t *xprofile_data;
  int xprofile_size;

  // the current set of selected profiles
  dt_colorspaces_color_profile_type_t display_type;
  dt_colorspaces_color_profile_type_t softproof_type;
  char display_filename[512];
  char softproof_filename[512];
  dt_iop_color_intent_t display_intent;
  dt_iop_color_intent_t softproof_intent;

  dt_colorspaces_color_mode_t mode;

  cmsHTRANSFORM transform_srgb_to_display, transform_adobe_rgb_to_display;

} dt_colorspaces_t;

typedef struct dt_colorspaces_color_profile_t
{
  dt_colorspaces_color_profile_type_t type; // filename is only used for type DT_COLORSPACE_FILE
  char filename[512];                       // icc file name
  char name[512];                           // product name, displayed in GUI
  cmsHPROFILE profile;                      // the actual profile
  int in_pos;                               // position in input combo box, -1 if not applicable
  int out_pos;                              // position in output combo box, -1 if not applicable
  int display_pos;                          // position in display combo box, -1 if not applicable
} dt_colorspaces_color_profile_t;

int mat3inv_float(float *const dst, const float *const src);
int mat3inv_double(double *const dst, const double *const src);
int mat3inv(float *const dst, const float *const src);

/** populate the global color profile lists */
dt_colorspaces_t *dt_colorspaces_init();
/** cleanup on shutdown */
void dt_colorspaces_cleanup(dt_colorspaces_t *self);

/** create the lab profile. */
cmsHPROFILE dt_colorspaces_create_lab_profile();

/** create the ICC virtual profile for srgb space. */
cmsHPROFILE dt_colorspaces_create_srgb_profile(void);
cmsHPROFILE dt_colorspaces_create_srgb_profile_v4(void);

/** create the ICC virtual profile for linear rec709 rgb space. */
cmsHPROFILE dt_colorspaces_create_linear_rec709_rgb_profile(void);

/** create the ICC virtual profile for linear rec2020 rgb space. */
cmsHPROFILE dt_colorspaces_create_linear_rec2020_rgb_profile(void);

/** create the ICC virtual profile for linear infrared bgr space. */
cmsHPROFILE dt_colorspaces_create_linear_infrared_profile(void);

/** create the ICC virtual profile for adobe rgb space. */
cmsHPROFILE dt_colorspaces_create_adobergb_profile(void);

/** create a ICC virtual profile for XYZ. */
cmsHPROFILE dt_colorspaces_create_xyz_profile(void);

/** create a RGB matrix from a color matrix from dcraw. */
void dt_colorspaces_create_cmatrix(float cmatrix[4][3], float mat[3][3]);

/** create a profile from a camera->xyz matrix. */
cmsHPROFILE dt_colorspaces_create_xyzmatrix_profile(float cam_xyz[3][3]);

/** create a profile from a xyz->camera matrix. */
cmsHPROFILE dt_colorspaces_create_xyzimatrix_profile(float cam_xyz[3][3]);

/** create a ICC virtual profile from the shipped presets in darktable. */
cmsHPROFILE dt_colorspaces_create_darktable_profile(const char *makermodel);

/** create a ICC virtual profile from the shipped vendor matrices in darktable. */
cmsHPROFILE dt_colorspaces_create_vendor_profile(const char *makermodel);

/** create a ICC virtual profile from the shipped alternate matrices in darktable. */
cmsHPROFILE dt_colorspaces_create_alternate_profile(const char *makermodel);

/** just get the associated transformation matrix, for manual application. */
int dt_colorspaces_get_darktable_matrix(const char *makermodel, float *matrix);

/** return the output profile, taking export override into account */
const dt_colorspaces_color_profile_t *dt_colorspaces_get_output_profile(const int imgid);

/** free the resources of a profile created with the functions above. */
void dt_colorspaces_cleanup_profile(cmsHPROFILE p);

/** uses D50 white point. */
void dt_XYZ_to_Lab(const float *XYZ, float *Lab);

/** uses D50 white point. */
void dt_Lab_to_XYZ(const float *Lab, float *XYZ);

/** extracts tonecurves and color matrix prof to XYZ from a given input profile, returns 0 on success (curves
 * and matrix are inverted for input) */
int dt_colorspaces_get_matrix_from_input_profile(cmsHPROFILE prof, float *matrix, float *lutr, float *lutg,
                                                 float *lutb, const int lutsize, const int intent);

/** extracts tonecurves and color matrix prof to XYZ from a given output profile, returns 0 on success. */
int dt_colorspaces_get_matrix_from_output_profile(cmsHPROFILE prof, float *matrix, float *lutr, float *lutg,
                                                  float *lutb, const int lutsize, const int intent);

/** searches for the given profile name in the user config dir ~/.config/darktable/color/<inout> and
 * /usr/share/darktable/.. */
int dt_colorspaces_find_profile(char *filename, size_t filename_len, const char *profile, const char *inout);

/** wrapper to get the name from a color profile. this tries to handle character encodings. */
void dt_colorspaces_get_profile_name(cmsHPROFILE p, const char *language, const char *country, char *name,
                                     size_t len);

/** get a nice printable name. */
const char *dt_colorspaces_get_name(dt_colorspaces_color_profile_type_t type, const char *filename);

/** common functions to change between colorspaces, used in iop modules */
void rgb2hsl(const float rgb[3], float *h, float *s, float *l);
void hsl2rgb(float rgb[3], float h, float s, float l);

/** trigger updating the display profile from the system settings (x atom, colord, ...) */
void dt_colorspaces_set_display_profile();
/** get the profile described by type & filename.
 *  this doesn't support image specifics like embedded profiles or camera matrices */
const dt_colorspaces_color_profile_t *
dt_colorspaces_get_profile(dt_colorspaces_color_profile_type_t type, const char *filename,
                           dt_colorspaces_profile_direction_t direction);

/** update the display transforms of srgb and adobergb to the display profile.
 * make sure that darktable.color_profiles->xprofile_lock is held when calling this! */
void dt_colorspaces_update_display_transforms();

#endif
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
