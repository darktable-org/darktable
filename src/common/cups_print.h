/*
    This file is part of darktable,
    Copyright (C) 2014-2020 darktable developers.

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

#pragma once

#include <common/colorspaces.h>

#define MAX_NAME 128

typedef enum dt_alignment_t {
  ALIGNMENT_TOP_LEFT,
  ALIGNMENT_TOP,
  ALIGNMENT_TOP_RIGHT,
  ALIGNMENT_LEFT,
  ALIGNMENT_CENTER,
  ALIGNMENT_RIGHT,
  ALIGNMENT_BOTTOM_LEFT,
  ALIGNMENT_BOTTOM,
  ALIGNMENT_BOTTOM_RIGHT
} dt_alignment_t;

typedef struct dt_paper_info_t
{
  char name[MAX_NAME], common_name[MAX_NAME];
  double width, height;
} dt_paper_info_t;

typedef struct dt_medium_info_t
{
  char name[MAX_NAME], common_name[MAX_NAME];
} dt_medium_info_t;

typedef struct dt_page_setup_t
{
  gboolean landscape;
  double margin_top, margin_bottom, margin_left, margin_right;
} dt_page_setup_t;

typedef struct dt_printer_info_t
{
  char name[MAX_NAME];
  int resolution;
  double hw_margin_top, hw_margin_bottom, hw_margin_left, hw_margin_right;
  dt_iop_color_intent_t intent;
  char profile[256];
  gboolean is_turboprint;
} dt_printer_info_t;

typedef struct dt_print_info_t
{
  dt_printer_info_t printer;
  dt_page_setup_t page;
  dt_paper_info_t paper;
  dt_medium_info_t medium;
} dt_print_info_t;

// Asynchronous printer discovery, cb will be called for each printer found
void dt_printers_discovery(void (*cb)(dt_printer_info_t *pr, void *user_data), void *user_data);
void dt_printers_abort_discovery(void);

// initialize the pinfo structure
void dt_init_print_info(dt_print_info_t *pinfo);

// get printer information for the given printer name
void dt_get_printer_info(const char *printer_name, dt_printer_info_t *pinfo);

// get all available papers for the given printer
GList *dt_get_papers(const dt_printer_info_t *printer);

// get paper information for the given paper name
dt_paper_info_t *dt_get_paper(GList *papers, const char *name);

// get all available media type for the given printer
GList *dt_get_media_type(const dt_printer_info_t *printer);

// get paper information for the given paper name
dt_medium_info_t *dt_get_medium(GList *media, const char *name);

// print filename using the printer and the page size and setup
void dt_print_file(const int32_t imgid, const char *filename, const char *job_title, const dt_print_info_t *pinfo);

// given the page settings (media size and border) and the printer (hardware margins) returns the
// page and printable area layout in the area_width and area_height (the area that dt allocate
// for the central display).
//  - the page area (px, py, pwidth, pheight)
//  - the printable area (ax, ay, awidth and aheight), the area without the borders
// there is no unit, every returned values are based on the area size.
void dt_get_print_layout(const dt_print_info_t *prt,
                         const int32_t area_width, const int32_t area_height,
                         float *px, float *py, float *pwidth, float *pheight,
                         float *ax, float *ay, float *awidth, float *aheight,
                         gboolean *borderless);

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

