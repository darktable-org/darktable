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

#include <lcms.h>

/** create the ICC virtual profile for srgb space. */
cmsHPROFILE dt_colorspaces_create_srgb_profile(void);

/** create the ICC virtual profile for linear rgb space. */
cmsHPROFILE dt_colorspaces_create_linear_rgb_profile(void);

/** create the ICC virtual profile for adobe rgb space. */
cmsHPROFILE dt_colorspaces_create_adobergb_profile(void);

/** create a ICC virtual profile for XYZ. */
cmsHPROFILE dt_colorspaces_create_xyz_profile(void);

/** create a ICC virtual profile from the shipped presets in darktable. */
cmsHPROFILE dt_colorspaces_create_darktable_profile(const char *makermodel);

/** get the icc profile this image would be exported with. */
cmsHPROFILE dt_colorspaces_create_output_profile(const int imgid);

#endif
