/*
    This file is part of darktable,
    copyright (c) 2009--2010 johannes hanika.

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
#ifndef DT_FILM_H
#define DT_FILM_H

#include "common/darktable.h"
#include "common/dtpthread.h"

#include <inttypes.h>
#include <glib.h>

/**
 * film roll.
 * this is one directory of images on disk.
 * also manages the preview image cache.
 */
typedef struct dt_film_t
{
  int32_t id;
  char dirname[512];
  dt_pthread_mutex_t images_mutex;
  GDir *dir;
  int32_t num_images, last_loaded;
  int32_t ref;
} dt_film_t;

void dt_film_init(dt_film_t *film);
void dt_film_cleanup(dt_film_t *film);
/** open film with given id. */
int dt_film_open(const int32_t id);
/** open film with given id. */
int dt_film_open2(dt_film_t *film);

/** open num-th most recently used film. */
int dt_film_open_recent(const int32_t num);
/** import new film and all images in this directory as a background task(non-recursive, existing films/images
 * are respected). */
int dt_film_import(const char *dirname);
/** helper for import threads. */
void dt_film_import1(dt_film_t *film);
/** constructs the lighttable/query setting for this film, respecting stars and filters. */
void dt_film_set_query(const int32_t id);
/** removes this film and all its images from db. */
void dt_film_remove(const int id);
/** checks if film is empty */
int dt_film_is_empty(const int id);
/** Creating a new filmroll */
int dt_film_new(dt_film_t *film, const char *directory);
/** removes all empty film rolls. */
void dt_film_remove_empty();

#endif
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
