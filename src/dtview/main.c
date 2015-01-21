/*
    This file is part of darktable,
    copyright (c) 2011 johannes hanika.

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

#include "common/darktable.h"
#include "common/debug.h"
#include "common/collection.h"
#include "common/points.h"
#include "common/image.h"
#include "common/image_cache.h"
#include "common/imageio.h"
#include "common/imageio_module.h"
#include "control/conf.h"

#include <GL/gl.h>
#include <GL/glu.h>
#include <SDL/SDL.h>
#include <stdlib.h>
double drand48(void);
void srand48(long int);
#include <sys/time.h>
#include <unistd.h>
#include <inttypes.h>

int running;
int width, height;
uint32_t random_state;
int32_t repeat;
int use_random;
float *pixels;
uint32_t scramble = 0;

int init(int argc, char *arg[])
{
  const SDL_VideoInfo *info = NULL;
  int bpp = 0;
  int flags = 0;

  struct timeval time;
  gettimeofday(&time, NULL);
  scramble = time.tv_sec + time.tv_usec;

  if(SDL_Init(SDL_INIT_VIDEO) < 0)
  {
    fprintf(stderr, "[%s] video initialization failed: %s\n", arg[0], SDL_GetError());
    exit(1);
  }

  info = SDL_GetVideoInfo();

  if(info == NULL)
  {
    fprintf(stderr, "[%s] video info failed: %s\n", arg[0], SDL_GetError());
    exit(1);
  }

  width = info->current_w;
  height = info->current_h;
  pixels = (float *)malloc(sizeof(float) * 4 * width * height);
  for(int k = 0; k < width * height * 4; k++) pixels[k] = 1.0f;

  bpp = 32;

  SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 16);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

  flags = SDL_OPENGL | SDL_FULLSCREEN;

  if(SDL_SetVideoMode(width, height, bpp, flags) == 0)
  {
    fprintf(stderr, "[%s] video mode set failed: %s\n", arg[0], SDL_GetError());
    return 0;
  }
  SDL_WM_SetCaption("darktable image viewer", NULL);

  atexit(&SDL_Quit);

  // hide mouse cursor
  SDL_ShowCursor(0);

  GLuint texID = 0;
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_BLEND);
  glColor3f(1.0f, 1.0f, 1.0f);
  glGenTextures(1, &texID);
  glBindTexture(GL_TEXTURE_2D, texID);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_FLOAT, NULL);

  glEnable(GL_TEXTURE_2D);
  glBindTexture(GL_TEXTURE_2D, texID);

  glMatrixMode(GL_PROJECTION);
  glPushMatrix();
  glLoadIdentity();
  gluOrtho2D(0, 1, 1, 0);
  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();

  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  return 1;
}

static void dtv_shutdown()
{
  // close all dt related stuff.
  dt_cleanup();
}

static void handle_event(const SDL_Event *event)
{
  switch(event->type)
  {
    case SDL_KEYDOWN:
    {
      const SDLKey keysym = event->key.keysym.sym;
      if(keysym == SDLK_ESCAPE) running = 0;
      break;
    }
    default:
      break;
  }
}

static void pump_events()
{
  SDL_Event event;
  while(SDL_PollEvent(&event)) handle_event(&event);
}

static void update(const int frame)
{
  // copy over the buffer, so we can blend smoothly.
  glReadBuffer(GL_FRONT);
  glDrawBuffer(GL_BACK);
  glCopyPixels(0, 0, width, height, GL_COLOR);
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_FLOAT, pixels);
  if(frame < 18)
  {
    // glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_BLEND);
  }
  else
  {
    glDisable(GL_BLEND);
  }
  glBegin(GL_QUADS);
  glTexCoord2d(0.0f, 1.0f);
  glVertex2f(0.0f, 1.0f);
  glTexCoord2d(0.0f, 0.0f);
  glVertex2f(0.0f, 0.0f);
  glTexCoord2d(1.0f, 0.0f);
  glVertex2f(1.0f, 0.0f);
  glTexCoord2d(1.0f, 1.0f);
  glVertex2f(1.0f, 1.0f);
  glEnd();
  // display the back buffer
  SDL_GL_SwapBuffers();
}

static int bpp(dt_imageio_module_data_t *data)
{
  return 32;
}

static int levels(dt_imageio_module_data_t *data)
{
  return IMAGEIO_RGB | IMAGEIO_FLOAT;
}

static const char *mime(dt_imageio_module_data_t *data)
{
  return "memory";
}

static int write_image(dt_imageio_module_data_t *data, const char *filename, const void *in, void *exif,
                       int exif_len, int imgid, int num, int total)
{
  const int offx = (width - data->width) / 2;
  const int offy = (height - data->height) / 2;
  float *out = pixels + (offy * width + offx) * 4;
  const float *rd = in;
  memset(pixels, 0, 4 * sizeof(float) * width * height);
  const float alpha = 0.2f;
  for(int i = 3; i < 4 * width * height; i += 4) pixels[i] = 0.2f;
  for(int j = 0; j < MIN(data->height, height); j++)
  {
    for(int i = 0; i < MIN(data->width, width); i++)
    {
      for(int c = 0; c < 3; c++) out[4 * i + c] = rd[4 * i + c];
      out[4 * i + 3] = alpha;
    }
    out += 4 * width;
    rd += 4 * data->width;
  }

  return 0;
}

uint32_t next_random()
{
  uint32_t i = random_state++;
  // van der corput for 32 bits. this guarantees every number will appear exactly once
  i = ((i & 0x0000ffff) << 16) | (i >> 16);
  i = ((i & 0x00ff00ff) << 8) | ((i & 0xff00ff00) >> 8);
  i = ((i & 0x0f0f0f0f) << 4) | ((i & 0xf0f0f0f0) >> 4);
  i = ((i & 0x33333333) << 2) | ((i & 0xcccccccc) >> 2);
  i = ((i & 0x55555555) << 1) | ((i & 0xaaaaaaaa) >> 1);
  return i ^ scramble;
}

static int process_next_image()
{
  static int counter = 0;
  dt_imageio_module_format_t buf;
  dt_imageio_module_data_t dat;
  buf.mime = mime;
  buf.levels = levels;
  buf.bpp = bpp;
  buf.write_image = write_image;
  dat.max_width = width;
  dat.max_height = height;
  dat.style[0] = '\0';

  // get random image id from sql
  int32_t id = 0;
  const uint32_t cnt = dt_collection_get_count(darktable.collection);
  // enumerated all images?
  if(++counter >= cnt) return 1;
  uint32_t ran = counter - 1;
  if(use_random)
  {
    // get random number up to next power of two greater than cnt:
    const uint32_t zeros = __builtin_clz(cnt);
    // pull radical inverses only in our desired range:
    do
      ran = next_random() >> zeros;
    while(ran >= cnt);
  }
  const int32_t rand = ran % cnt;
  const gchar *query = dt_collection_get_query(darktable.collection);
  if(!query) return 1;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, rand);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, rand + 1);
  if(sqlite3_step(stmt) == SQLITE_ROW) id = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);

  if(id)
  {
    dt_imageio_export(id, "unused", &buf, &dat, TRUE, FALSE, NULL, NULL, 1, 1);
  }
  return 0;
}

int main(int argc, char *arg[])
{
  gtk_init(&argc, &arg);
  repeat = random_state = use_random = 0;
  for(int k = 1; k < argc; k++)
  {
    if(!strcmp(arg[k], "--random"))
      use_random = 1;
    else if(!strcmp(arg[k], "--repeat"))
      repeat = -1;
    else if(!strcmp(arg[k], "-h") || !strcmp(arg[k], "--help"))
    {
      fprintf(stderr, "usage: %s [--random] [--repeat]\n", arg[0]);
      exit(0);
    }
  }
  // init dt without gui:
  if(dt_init(argc, arg, 0, NULL)) exit(1);
  // use system color profile, if we can:
  gchar *oldprofile = dt_conf_get_string("plugins/lighttable/export/iccprofile");
  const gchar *overprofile = "X profile";
  dt_conf_set_string("plugins/lighttable/export/iccprofile", overprofile);
  running = init(argc, arg);
  srand48(SDL_GetTicks());
  if(use_random) random_state = drand48() * INT_MAX;
  if(repeat < 0) repeat = random_state;
  while(running)
  {
    pump_events();
    if(!running) break;
    if(process_next_image())
    {
      if(repeat >= 0)
      {
        // start over
        random_state = repeat;
        continue;
      }
      break;
    }
    for(int k = 0; k <= 18; k++)
    {
      update(k);

      struct timespec time = { 0, 10000000L };
      nanosleep(&time, NULL);
    }
    for(int k = 0; k < 100; k++)
    {
      pump_events();
      if(!running) break;

      struct timespec time = { 0, 35000000L };
      nanosleep(&time, NULL);
    }
  }
  if(oldprofile)
  {
    dt_conf_set_string("plugins/lighttable/export/iccprofile", oldprofile);
    g_free(oldprofile);
  }
  dtv_shutdown();
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
