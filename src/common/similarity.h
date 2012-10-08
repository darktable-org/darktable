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
#ifndef DT_SIMILARITY_H
#define DT_SIMILARITY_H

#include <inttypes.h>

typedef struct dt_similarity_t
{
  float histogram_weight;
  float lightmap_weight;
  float redmap_weight;
  float greenmap_weight;
  float bluemap_weight;
} dt_similarity_t;

#define DT_SIMILARITY_HISTOGRAM_BUCKETS 8
typedef struct dt_similarity_histogram_t
{
  float rgbl[DT_SIMILARITY_HISTOGRAM_BUCKETS][4];
} dt_similarity_histogram_t;

#define DT_SIMILARITY_LIGHTMAP_SIZE 10
typedef struct dt_similarity_imagemap_t
{
  uint8_t pixels[DT_SIMILARITY_LIGHTMAP_SIZE*DT_SIMILARITY_LIGHTMAP_SIZE*4];
} dt_similarity_lightmap_t;

void dt_similarity_image_dirty(uint32_t imgid);

/** \brief stores the histogram with the imgid to database
	\note a histogram is generated in a DT_SIMILARITY_HISTOGRAM_BUCKETSx4 float array.
	\see dt_dev_pixelpipe_process_rec()
*/
void dt_similarity_histogram_store(uint32_t imgid, const dt_similarity_histogram_t *histogram);
/** marks histogram data for imgid as dirty and will be regenerated at next indexing. */
void dt_similarity_histogram_dirty(uint32_t imgid);

void dt_similarity_lightmap_store(uint32_t imgid, const dt_similarity_lightmap_t *lightmap);
void dt_similarity_lightmap_dirty(uint32_t imgid);

void dt_similarity_match_image(uint32_t imgid, dt_similarity_t *data);

#endif
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
