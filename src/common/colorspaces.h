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

/** create the lab profile. */
cmsHPROFILE dt_colorspaces_create_lab_profile();

/** create the ICC virtual profile for srgb space. */
cmsHPROFILE dt_colorspaces_create_srgb_profile(void);

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

/** create a profile from a color matrix from dcraw. */
cmsHPROFILE dt_colorspaces_create_cmatrix_profile(float cmatrix[3][4]);

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

/** get the name of the icc profile this image would be exported with. */
char *dt_colorspaces_get_output_profile_name(const int imgid);

/** get the icc profile this image would be exported with. */
cmsHPROFILE dt_colorspaces_create_output_profile(const int imgid);

/** free the resources of a profile created with the functions above. */
void dt_colorspaces_cleanup_profile(cmsHPROFILE p);

/** uses D50 white point. */
void dt_XYZ_to_Lab(const float *XYZ, float *Lab);

/** uses D50 white point. */
void dt_Lab_to_XYZ(const float *Lab, float *XYZ);

/** extracts tonecurves and color matrix prof to XYZ from a given input profile, returns 0 on success (curves
 * and matrix are inverted for input) */
int dt_colorspaces_get_matrix_from_input_profile(cmsHPROFILE prof, float *matrix, float *lutr, float *lutg,
                                                 float *lutb, const int lutsize);

/** extracts tonecurves and color matrix prof to XYZ from a given output profile, returns 0 on success. */
int dt_colorspaces_get_matrix_from_output_profile(cmsHPROFILE prof, float *matrix, float *lutr, float *lutg,
                                                  float *lutb, const int lutsize);

/** get normalized exif name. */
void dt_colorspaces_get_makermodel(char *makermodel, size_t makermodel_len, const char *const maker,
                                   const char *const model);
void dt_colorspaces_get_makermodel_split(char *makermodel, size_t makermodel_len, char **modelo,
                                         const char *const maker, const char *const model);

/** searches for the given profile name in the user config dir ~/.config/darktable/color/<inout> and
 * /usr/share/darktable/.. */
int dt_colorspaces_find_profile(char *filename, size_t filename_len, const char *profile, const char *inout);

/** wrapper to get the name from a color profile. this tries to handle character encodings. */
void dt_colorspaces_get_profile_name(cmsHPROFILE p, const char *language, const char *country, char *name,
                                     size_t len);

/** common functions to change between colorspaces, used in iop modules */
void rgb2hsl(const float rgb[3], float *h, float *s, float *l);
void hsl2rgb(float rgb[3], float h, float s, float l);

#endif
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
