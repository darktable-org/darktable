/*
 *    This file is part of darktable,
 *    Copyright (C) 2015-2021 darktable developers.
 *
 *    darktable is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 *
 *    darktable is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 *  this is a simple PDF writer, capable of creating multi page PDFs with embedded images.
 *  it is NOT meant to be a full fledged PDF library, and shall never turn into something like that!
 *  see the main() function in pdf.c to see an example how to use this.
 */

#pragma once

#include <glib.h>
#include <glib/gi18n.h>
#include <inttypes.h>
#include <stdio.h>

// clang-format off

#define dt_pdf_inch_to_point(inch)      ((inch) * 72.0)
#define dt_pdf_point_to_inch(pt)        ((pt) / 72.0)
#define dt_pdf_mm_to_point(mm)          dt_pdf_inch_to_point((mm) / 25.4)
#define dt_pdf_point_to_mm(pt)          dt_pdf_point_to_inch((pt) * 25.4)
#define dt_pdf_point_to_pixel(pt, dpi)  (dt_pdf_point_to_inch(pt) * (dpi))
#define dt_pdf_pixel_to_point(px, dpi)  (dt_pdf_inch_to_point((px) / (dpi)))

typedef enum dt_pdf_stream_encoder_t
{
  DT_PDF_STREAM_ENCODER_ASCII_HEX = 0,  // inflate size by 2 -- big & fast
  DT_PDF_STREAM_ENCODER_FLATE     = 1   // use zlib to compress -- small & slow
} dt_pdf_stream_encoder_t;

typedef struct dt_pdf_t
{
  FILE                    *fd;
  int                      next_id;
  int                      next_image;
  size_t                   bytes_written;
  float                    page_width, page_height, dpi;
  dt_pdf_stream_encoder_t  default_encoder;

  char                    *title;

  size_t                  *offsets;
  int                      n_offsets;
} dt_pdf_t;

typedef struct dt_pdf_image_t
{
  int       object_id;
  int       name_id;
  size_t    size;
  size_t    width, height;
  float     bb_x, bb_y, bb_width, bb_height;

  gboolean  rotate_to_fit;

  gboolean  outline_mode; // set to 1 to only draw a box instead of the image
  gboolean  show_bb; // set to 1 to draw the bounding box. useful for debugging
} dt_pdf_image_t;

typedef struct dt_pdf_page_t
{
  int     object_id;
  size_t  size;
} dt_pdf_page_t;

static const struct
{
  const char  *name;
  const float  factor;
} dt_pdf_units[] =
{
  {N_("mm"),   dt_pdf_mm_to_point(1.0)},
  {N_("cm"),   dt_pdf_mm_to_point(10.0)},
  {N_("inch"), dt_pdf_inch_to_point(1.0)},
  {N_("\""),   dt_pdf_inch_to_point(1.0)},
  {NULL,       0.0}
};

static const int dt_pdf_units_n = sizeof(dt_pdf_units) / sizeof(dt_pdf_units[0]);

static const struct
{
  const char  *name;
  const float  width;
  const float  height;
} dt_pdf_paper_sizes[] =
{
  {N_("A4"),     dt_pdf_mm_to_point(210),   dt_pdf_mm_to_point(297)},
  {N_("A3"),     dt_pdf_mm_to_point(297),   dt_pdf_mm_to_point(420)},
  {N_("Letter"), dt_pdf_inch_to_point(8.5), dt_pdf_inch_to_point(11.0)},
  {N_("Legal"),  dt_pdf_inch_to_point(8.5), dt_pdf_inch_to_point(14.0)},
  {NULL,         0.0,                       0.0}
};

// clang-format on

static const int dt_pdf_paper_sizes_n = sizeof(dt_pdf_paper_sizes) / sizeof(dt_pdf_paper_sizes[0]) - 1;

// construction of the pdf
dt_pdf_t *dt_pdf_start(const char *filename, float width, float height, float dpi, dt_pdf_stream_encoder_t default_encoder);
int dt_pdf_add_icc(dt_pdf_t *pdf, const char *filename);
int dt_pdf_add_icc_from_data(dt_pdf_t *pdf, const unsigned char *data, size_t size);
dt_pdf_image_t *dt_pdf_add_image(dt_pdf_t *pdf, const unsigned char *image, int width, int height, int bpp, int icc_id, float border);
dt_pdf_page_t *dt_pdf_add_page(dt_pdf_t *pdf, dt_pdf_image_t **images, int n_images);
void dt_pdf_finish(dt_pdf_t *pdf, dt_pdf_page_t **pages, int n_pages);

// general helpers
int dt_pdf_parse_length(const char *str, float *length);
int dt_pdf_parse_paper_size(const char *str, float *width, float *height);

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

