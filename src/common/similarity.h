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

#define DT_SIMILARITY_HISTOGRAM_BUCKETS 4
typedef struct dt_similarity_histogram_t
{
	float rgbl[DT_SIMILARITY_HISTOGRAM_BUCKETS][4];
}dt_similarity_histogram_t;
	

/** \brief stores the histogram with the imgid to database
	\note a histogram is generated in a DT_SIMILARITY_HISTOGRAM_BUCKETSx4 float array.
	\see dt_dev_pixelpipe_process_rec()
*/
void dt_similarity_store_histogram(uint32_t imgid, const dt_similarity_histogram_t *histogram);
void dt_similarity_match_image(uint32_t imgid);

#endif