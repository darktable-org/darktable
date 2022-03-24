/*
    This file is part of darktable,
    Copyright (C) 2016-2020 darktable developers.

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

#include "develop/format.h"
#include "develop/imageop.h"

size_t dt_iop_buffer_dsc_to_bpp(const struct dt_iop_buffer_dsc_t *dsc)
{
  size_t bpp = dsc->channels;

  switch(dsc->datatype)
  {
    case TYPE_FLOAT:
      bpp *= sizeof(float);
      break;
    case TYPE_UINT16:
      bpp *= sizeof(uint16_t);
      break;
    default:
      dt_unreachable_codepath();
      break;
  }

  return bpp;
}

void default_input_format(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece,
                          dt_iop_buffer_dsc_t *dsc)
{
  dsc->channels = 4;
  dsc->datatype = TYPE_FLOAT;
  dsc->cst = self->input_colorspace(self, pipe, piece);

  if(dsc->cst != IOP_CS_RAW) return;

  if(dt_image_is_raw(&pipe->image)) dsc->channels = 1;

  if(dt_ioppr_get_iop_order(pipe->iop_order_list, self->op, self->multi_priority)
     > dt_ioppr_get_iop_order(pipe->iop_order_list, "rawprepare", 0)) return;

  if(piece->pipe->dsc.filters)
    dsc->datatype = TYPE_UINT16;
}

void default_output_format(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece,
                           dt_iop_buffer_dsc_t *dsc)
{
  dsc->channels = 4;
  dsc->datatype = TYPE_FLOAT;
  dsc->cst = self->output_colorspace(self, pipe, piece);

  if(dsc->cst != IOP_CS_RAW) return;

  if(dt_image_is_raw(&pipe->image)) dsc->channels = 1;

  if(dt_ioppr_get_iop_order(pipe->iop_order_list, self->op, self->multi_priority)
     >= dt_ioppr_get_iop_order(pipe->iop_order_list, "rawprepare", 0)) return;

  if(piece->pipe->dsc.filters)
    dsc->datatype = TYPE_UINT16;
}

int default_input_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe,
                     dt_dev_pixelpipe_iop_t *piece)
{
  return self->default_colorspace(self, pipe, piece);
}

int default_output_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe,
                      dt_dev_pixelpipe_iop_t *piece)
{
  return self->default_colorspace(self, pipe, piece);
}

int default_blend_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe,
                      dt_dev_pixelpipe_iop_t *piece)
{
  return self->default_colorspace(self, pipe, piece);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

