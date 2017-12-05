/*
    This file is part of darktable,
    copyright (c) 2016 Roman Lebedev.

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

static int _iop_module_rawprepare = 0, _iop_module_demosaic = 0;
static inline void _get_iop_priorities(const dt_iop_module_t *module)
{
  if(_iop_module_rawprepare && _iop_module_demosaic) return;

  GList *iop = module->dev->iop;
  while(iop)
  {
    dt_iop_module_t *m = (dt_iop_module_t *)iop->data;

    if(!strcmp(m->op, "rawprepare")) _iop_module_rawprepare = m->priority;
    if(!strcmp(m->op, "demosaic")) _iop_module_demosaic = m->priority;

    if(_iop_module_rawprepare && _iop_module_demosaic) break;

    iop = g_list_next(iop);
  }
}

void default_input_format(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece,
                          dt_iop_buffer_dsc_t *dsc)
{
  _get_iop_priorities(self);

  dsc->channels = 4;
  dsc->datatype = TYPE_FLOAT;

  if(self->priority > _iop_module_demosaic) return;

  if(pipe->image.flags & DT_IMAGE_RAW) dsc->channels = 1;

  if(self->priority > _iop_module_rawprepare) return;

  if(piece->pipe->dsc.filters)
    dsc->datatype = TYPE_UINT16;
}

void default_output_format(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece,
                           dt_iop_buffer_dsc_t *dsc)
{
  _get_iop_priorities(self);

  dsc->channels = 4;
  dsc->datatype = TYPE_FLOAT;

  if(self->priority >= _iop_module_demosaic) return;

  if(pipe->image.flags & DT_IMAGE_RAW) dsc->channels = 1;

  if(self->priority >= _iop_module_rawprepare) return;

  if(piece->pipe->dsc.filters)
    dsc->datatype = TYPE_UINT16;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
