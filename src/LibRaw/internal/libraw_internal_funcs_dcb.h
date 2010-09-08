/*
    This file is part of darktable,
    copyright (c) 2010 Andrey Kaminsky.

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

   This file is generated from Jacek Gozdz's (cuniek@kft.umcs.lublin.pl) dcb_demosaicing.c
   Look into original file (or probably http://www.linuxphoto.org/html/algorithms.html)
   for copyright information.

*/

#ifndef _LIBRAW_INTERNAL_FUNCS_DCB_H
#define _LIBRAW_INTERNAL_FUNCS_DCB_H
    void        dcb_pp();
    void        dcb_copy_to_buffer(float (*image2)[3]);
    void        dcb_restore_from_buffer(float (*image2)[3]);
    void        dcb_color();
    void        dcb_color_full();
    void        dcb_map();
    void        dcb_correction();
    void        dcb_correction2();
    void        dcb_refinement();
    void        rgb_to_lch(double (*image3)[3]);
    void        lch_to_rgb(double (*image3)[3]);
    void        fbdd_correction();
    void        fbdd_correction2(double (*image3)[3]);
    void        fbdd_green();
    void  	dcb_ver(float (*image3)[3]);
    void 	dcb_hor(float (*image2)[3]);
    void 	dcb_color2(float (*image2)[3]);
    void 	dcb_color3(float (*image3)[3]);
    void 	dcb_decide(float (*image2)[3], float (*image3)[3]);
    void 	dcb_nyquist();
#endif
