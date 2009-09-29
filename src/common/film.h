#ifndef DT_FILM_H
#define DT_FILM_H

#include <inttypes.h>
#include <glib.h>
#include <pthread.h>

/**
 * film roll.
 * this is one directory of images on disk.
 * also manages the preview image cache.
 */
typedef struct dt_film_t
{
  int32_t id;
  char dirname[512];
  pthread_mutex_t images_mutex;
  GDir *dir;
  int32_t num_images, last_loaded, last_exported;
}
dt_film_t;

void dt_film_init(dt_film_t *film);
void dt_film_cleanup(dt_film_t *film);
/** open film with given id. */
int dt_film_open(dt_film_t *film, const int32_t id);
/** open num-th most recently used film. */
int dt_film_open_recent(dt_film_t *film, const int32_t num);
/** import new film and all images in this directory (non-recursive, existing films/images are respected). */
int dt_film_import(dt_film_t *film, const char *dirname);
/** helper for import threads. */
void dt_film_import1(dt_film_t *film);

#endif
