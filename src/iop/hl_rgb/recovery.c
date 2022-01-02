/*
    This file is part of darktable,
    Copyright (C) 2022 darktable developers.

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


static void process_recovery(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece,
                                    const void *const restrict ivoid, void *const restrict ovoid,
                                    const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_copy_image_roi(ovoid, ivoid, 4, roi_in, roi_out, TRUE);
}
