
#ifndef DARKTABLE_LIBRARY_H
#define DARKTABLE_LIBRARY_H

#include "common/darktable.h"
#include "common/image.h"
#include <inttypes.h>
#include <gtk/gtk.h>
#include <pthread.h>

#define DT_LIBRARY_MAX_ZOOM 13

void dt_image_expose(dt_image_t *image, int32_t i, cairo_t *cr, int32_t width, int32_t height, int32_t zoom, int32_t px, int32_t py);

/**
 * film roll.
 * this is one directory of images on disk.
 * also manages the preview image cache.
 */
typedef struct dt_film_roll_t
{
  int32_t id;
  char dirname[512];
  pthread_mutex_t images_mutex;
  GDir *dir;
  int32_t num_images, last_loaded, last_exported;
}
dt_film_roll_t;

struct dt_library_t;
void dt_film_roll_init(dt_film_roll_t *film);
void dt_film_roll_cleanup(dt_film_roll_t *film);
/** open film with given id. */
int dt_film_roll_open(dt_film_roll_t *film, const int32_t id);
/** open num-th most recently used film. */
int dt_film_roll_open_recent(dt_film_roll_t *film, const int num);
/** import new film and all images in this directory (non-recursive, existing films/images are respected). */
int dt_film_roll_import(dt_film_roll_t *film, const char *dirname);
/** helper for import threads. */
void dt_film_import1(dt_film_roll_t *film);

typedef enum dt_library_image_over_t
{
  DT_LIB_DESERT = 0,
  DT_LIB_STAR_1 = 1,
  DT_LIB_STAR_2 = 2,
  DT_LIB_STAR_3 = 3,
  DT_LIB_STAR_4 = 4
}
dt_library_image_over_t;

/**
 * this organises the whole library:
 * previously imported film rolls..
 */
typedef struct dt_library_t
{
  dt_film_roll_t *film;
  // tmp mouse vars:
  float select_offset_x, select_offset_y;
  int32_t last_selected_id;
  int button;
  uint32_t modifiers;
  dt_library_image_over_t image_over;
}
dt_library_t;

void dt_library_init(dt_library_t *lib);
void dt_library_cleanup(dt_library_t *lib);
// event handling:
void dt_library_expose(dt_library_t *lib, cairo_t *cr, int width, int height, int32_t pointerx, int32_t pointery);
void dt_library_button_pressed(dt_library_t *lib, double x, double y, int which, uint32_t state);
void dt_library_button_released(dt_library_t *lib, double x, double y, int which, uint32_t state);
void dt_library_mouse_moved(dt_library_t *lib, double x, double y, int which);
void dt_library_mouse_leave(dt_library_t *lib);


#endif
