/*
    This file is part of darktable,
    copyright (c) 2014-2015 pascal obry.

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

#ifndef DT_CUPS_PRINT_H
#define DT_CUPS_PRINT_H

#include <common/colorspaces.h>

#define MAX_NAME 128

typedef enum dt_alignment_t
{
  top_left, top, top_right,
  left, center, right,
  bottom_left, bottom, bottom_right
} dt_alignment_t;

typedef struct dt_paper_info_t
{
  char name[MAX_NAME], common_name[MAX_NAME];
  double width, height;
} dt_paper_info_t;

typedef struct dt_page_setup_t
{
  gboolean landscape;
  dt_alignment_t alignment;
  double margin_top, margin_bottom, margin_left, margin_right;
} dt_page_setup_t;

typedef struct dt_printer_info_t
{
  char name[MAX_NAME];
  int resolution;
  double hw_margin_top, hw_margin_bottom, hw_margin_left, hw_margin_right;
  dt_iop_color_intent_t intent;
  char profile[256];
} dt_printer_info_t;

typedef struct dt_print_info_t
{
  dt_printer_info_t printer;
  dt_page_setup_t page;
  dt_paper_info_t paper;
} dt_print_info_t;

// Asynchronous printer discovery, cb will be called for each printer found
void dt_printers_discovery(void (*cb)(dt_printer_info_t *pr, void *user_data), void *user_data);
void dt_printers_abort_discovery(void);

// initialize the pinfo structure
void dt_init_print_info(dt_print_info_t *pinfo);

// get printer information for the given printer name
dt_printer_info_t *dt_get_printer_info(const char *printer_name);

// get all available papers for the given printer
GList *dt_get_papers(const char *printer_name);

// get paper information for the given paper name
dt_paper_info_t *dt_get_paper(GList *papers, const char *name);

// print filename using the printer and the page size and setup
void dt_print_file(const int32_t imgid, const char *filename, const dt_print_info_t *pinfo);

// given the page settings (media size and border) and the printer (hardware margins) returns the page, image
// and printable area layout in the area_width and area_height (the area that dt allocate for the central display).
//  - the page area (px, py, pwidth, pheight)
//  - the printable area (ax, ay, awidth and aheight), the area without the borders
//  - the image position (ix, iy, iwidth, iheight)
// there is no unit, every returned values are based on the area size.
void dt_get_print_layout(const int32_t imgid, const dt_print_info_t *prt, const int32_t area_width, const int32_t area_height,
                         int32_t *iwpix, int32_t *ihpix,
                         int32_t *px,    int32_t *py,    int32_t *pwidth, int32_t *pheight,
                         int32_t *ax,    int32_t *ay,    int32_t *awidth, int32_t *aheight,
                         int32_t *ix,    int32_t *iy,    int32_t *iwidth, int32_t *iheight);
#endif

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
