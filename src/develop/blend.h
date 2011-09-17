/*
    This file is part of darktable,
    copyright (c) 2011 henrik andersson.

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

#ifndef DT_DEVELOP_BLEND_H
#define DT_DEVELOP_BLEND_H

#include "develop/pixelpipe.h"
#include "common/opencl.h"

#define DEVELOP_BLEND_MASK_FLAG				0x80
#define DEVELOP_BLEND_DISABLED				0x00
#define DEVELOP_BLEND_NORMAL				0x01
#define DEVELOP_BLEND_LIGHTEN				0x02
#define DEVELOP_BLEND_DARKEN				0x03
#define DEVELOP_BLEND_MULTIPLY				0x04
#define DEVELOP_BLEND_AVERAGE				0x05
#define DEVELOP_BLEND_ADD					0x06
#define DEVELOP_BLEND_SUBSTRACT				0x07
#define DEVELOP_BLEND_DIFFERENCE				0x08
#define DEVELOP_BLEND_SCREEN				0x09
#define DEVELOP_BLEND_OVERLAY				0x0A
#define DEVELOP_BLEND_SOFTLIGHT			0x0B
#define DEVELOP_BLEND_HARDLIGHT			0x0C
#define DEVELOP_BLEND_VIVIDLIGHT			0x0D
#define DEVELOP_BLEND_LINEARLIGHT			0x0E
#define DEVELOP_BLEND_PINLIGHT				0x0F
#define DEVELOP_BLEND_LIGHTNESS				0x10
#define DEVELOP_BLEND_CHROMA				0x11
#define DEVELOP_BLEND_HUE				0x12
#define DEVELOP_BLEND_COLOR				0x13

typedef struct dt_develop_blend_params_t
{
  /** blending mode */
  uint32_t mode;
  /** mixing opacity */
  float opacity;
  /** id of mask in current pipeline */
  uint32_t mask_id;
} dt_develop_blend_params_t;


typedef struct dt_blendop_t
{
  int kernel_blendop_Lab;
  int kernel_blendop_RAW;
  int kernel_blendop_rgb;
}
dt_blendop_t;


#define DT_DEVELOP_BLEND_WITH_MASK(p) ((p->mode&DEVELOP_BLEND_MASK_FLAG)?1:0)

/** global init of blendops */
void dt_develop_blend_init(dt_blendop_t *gd);

/** apply blend */
void dt_develop_blend_process (struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, void *i, void *o, const struct dt_iop_roi_t *roi_in, const struct dt_iop_roi_t *roi_out);

#ifdef HAVE_OPENCL
/** apply blend for opencl modules*/
int dt_develop_blend_process_cl (struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out, const struct dt_iop_roi_t *roi_in, const struct dt_iop_roi_t *roi_out);
#endif

#endif

