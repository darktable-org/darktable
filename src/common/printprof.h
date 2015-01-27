/*
    This file is part of darktable,
    copyright (c) 2014 pascal obry

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

#ifndef __PRINTPROF_H__
#define __PRINTPROF_H__

#include <inttypes.h>
#include <stddef.h>

int dt_apply_printer_profile(int imgid, void **in, uint32_t width, uint32_t height, int bpp, const char *profile, int intent);
// this routines takes as input an image of 8 or 16 bpp but always return a 8 bpp result. It is indeed better to
// apply the profile to a 16bit input but we do not need this for printing.

#endif // __PRINTPROF_H__

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
