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

#define DEVELOP_BLEND_MASK_FLAG			0x7f
#define DEVELOP_BLEND_DISABLED			0x00
#define DEVELOP_BLEND_NORMAL				0x01
#define DEVELOP_BLEND_LIGHTEN				0x02
#define DEVELOP_BLEND_DARKEN				0x03
#define DEVELOP_BLEND_MULTIPLY				0x04
#define DEVELOP_BLEND_AVERAGE				0x05

typedef struct dt_develop_blend_params_t {
	/** blending mode */
	unsigned char mode;
	/** mixing opacity */
	float opacity;
	/** id of mask in current pipeline */
	unsigned int mask_id;
} dt_develop_blend_params_t;

#define DT_DEVELOP_BLEND_WITH_MASK(p) ((p->mode&DEVELOP_BLEND_MASK_FLAG)?1:0)

/** apply blend */
void dt_develop_blend_process (struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, void *i, void *o, const struct dt_iop_roi_t *roi_in, const struct dt_iop_roi_t *roi_out);

#endif