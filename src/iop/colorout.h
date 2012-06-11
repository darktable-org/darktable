/*
		This file is part of darktable,
		copyright (c) 2009--2011 johannes hanika.

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
#ifndef DARKTABLE_IOP_COLOROUT_H
#define DARKTABLE_IOP_COLOROUT_H

#include "common/colorspaces.h"
#include "develop/imageop.h"
#include "iop/colorin.h" // common structs
#include <gtk/gtk.h>
#include <inttypes.h>

typedef struct dt_iop_colorout_global_data_t
{
  int kernel_colorout;
}
dt_iop_colorout_global_data_t;

typedef struct dt_iop_colorout_params_t
{
  char iccprofile[DT_IOP_COLOR_ICC_LEN];
  char displayprofile[DT_IOP_COLOR_ICC_LEN];

  dt_iop_color_intent_t intent;
  dt_iop_color_intent_t displayintent;

  char softproof_enabled;
  char softproofprofile[DT_IOP_COLOR_ICC_LEN];
  dt_iop_color_intent_t softproofintent; /// NOTE: Not used for now but reserved for future use
}
dt_iop_colorout_params_t;

typedef struct dt_iop_colorout_gui_data_t
{
  gboolean softproof_enabled;
  GtkWidget *cbox1, *cbox2, *cbox3, *cbox4,*cbox5;
  GList *profiles;

}
dt_iop_colorout_gui_data_t;

typedef struct dt_iop_colorout_data_t
{
  gboolean softproof_enabled;
  float lut[3][LUT_SAMPLES];
  float cmatrix[9];
  cmsHPROFILE softproof;
  cmsHPROFILE output;
  cmsHPROFILE Lab;
  cmsHTRANSFORM *xform;
  float unbounded_coeffs[3][3];       // for extrapolation of shaper curves
}
dt_iop_colorout_data_t;

#endif
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
