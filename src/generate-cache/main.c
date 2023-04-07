/*
    This file is part of darktable,
    Copyright (C) 2015-2022 darktable developers.

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

#include <glib.h>    // for g_mkdir_with_parents, _
#include <gtk/gtk.h> // for gtk_init_check
#include <libintl.h> // for bind_textdomain_codeset, etc
#include <limits.h>  // for PATH_MAX
#include <sqlite3.h> // for sqlite3_column_int, etc
#include <stddef.h>  // for size_t
#include <stdint.h>  // for uint32_t
#include <stdio.h>   // for fprintf, stderr, snprintf, NULL, etc
#include <stdlib.h>  // for exit, EXIT_FAILURE
#include <string.h>  // for strcmp
#include <unistd.h>  // for access, R_OK

#include "common/darktable.h"    // for darktable, darktable_t, dt_cleanup, etc
#include "common/database.h"     // for dt_database_get
#include "common/debug.h"        // for DT_DEBUG_SQLITE3_PREPARE_V2
#include "common/mipmap_cache.h" // for dt_mipmap_size_t, etc
#include "common/file_location.h"
#include "common/history.h"      // for dt_history_hash_set_mipmap
#include "config.h"              // for GETTEXT_PACKAGE, etc
#include "control/conf.h"        // for dt_conf_get_bool

#ifdef __APPLE__
#include "osx/osx.h"
#endif

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

static int generate_thumbnail_cache(const dt_mipmap_size_t min_mip, const dt_mipmap_size_t max_mip, const int32_t min_imgid, const int32_t max_imgid)
{
  fprintf(stderr, _("creating cache directories\n"));
  for(dt_mipmap_size_t k = min_mip; k <= max_mip; k++)
  {
    char dirname[PATH_MAX] = { 0 };
    snprintf(dirname, sizeof(dirname), "%s.d/%d", darktable.mipmap_cache->cachedir, k);

    fprintf(stderr, _("creating cache directory '%s'\n"), dirname);
    if(g_mkdir_with_parents(dirname, 0750))
    {
      fprintf(stderr, _("could not create directory '%s'!\n"), dirname);
      return 1;
    }
  }

  // some progress counter
  sqlite3_stmt *stmt;
  size_t image_count = 0, counter = 0;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT COUNT(*) FROM main.images WHERE id >= ?1 AND id <= ?2", -1, &stmt, 0);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, min_imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, max_imgid);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    image_count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
  }
  else
  {
    return 1;
  }

  if(!image_count)
  {
    fprintf(stderr, _("warning: no images are matching the requested image id range\n"));
    if(min_imgid > max_imgid)
    {
      fprintf(stderr, _("warning: did you want to swap these boundaries?\n"));
    }
  }

  // go through all images:
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT id, filename FROM main.images WHERE id >= ?1 AND id <= ?2", -1, &stmt, 0);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, min_imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, max_imgid);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const dt_imgid_t imgid = sqlite3_column_int(stmt, 0);
    const char *imgfilename = (const char*)sqlite3_column_text(stmt, 1);

    counter++;
    fprintf(stderr, "image %zu/%zu (%.02f%%) (id:%d, file=%s)\n", counter, image_count, 100.0 * counter / (float)image_count, imgid, imgfilename);

    for(int k = max_mip; k >= min_mip && k >= 0; k--)
    {
      char filename[PATH_MAX] = { 0 };
      snprintf(filename, sizeof(filename), "%s.d/%d/%d.jpg", darktable.mipmap_cache->cachedir, k, imgid);

      // if a valid thumbnail file is already on disc - do nothing
      if(dt_util_test_image_file(filename)) continue;

      // else, generate thumbnail and store in mipmap cache.
      dt_mipmap_buffer_t buf;
      dt_mipmap_cache_get(darktable.mipmap_cache, &buf, imgid, k, DT_MIPMAP_BLOCKING, 'r');
      dt_mipmap_cache_release(darktable.mipmap_cache, &buf);
    }

    // and immediately write thumbs to disc and remove from mipmap cache.
    dt_mimap_cache_evict(darktable.mipmap_cache, imgid);
    // thumbnail in sync with image
    dt_history_hash_set_mipmap(imgid);
  }

  sqlite3_finalize(stmt);
  fprintf(stderr, "done\n");

  return 0;
}

static void usage(const char *progname)
{
  fprintf(stderr,
          "usage: %s [-h, --help; --version]\n"
          "  [--min-mip <0-8> (default = 0)] [-m, --max-mip <0-8> (default = 2)]\n"
          "  [--min-imgid <N>] [--max-imgid <N>]\n"
          "  [--core <darktable options>]\n"
          "\n"
          "When multiple mipmap sizes are requested, the biggest one is computed\n"
          "while the rest are quickly downsampled.\n"
          "\n"
          "The --min-imgid and --max-imgid specify the range of internal image ID\n"
          "numbers to work on.\n",
          progname);
}

int main(int argc, char *arg[])
{
#ifdef __APPLE__
  dt_osx_prepare_environment();
#endif

  // get valid locale dir
  dt_loc_init(NULL, NULL, NULL, NULL, NULL, NULL);
  char localedir[PATH_MAX] = { 0 };
  dt_loc_get_localedir(localedir, sizeof(localedir));
  bindtextdomain(GETTEXT_PACKAGE, localedir);

  bind_textdomain_codeset(GETTEXT_PACKAGE, "UTF-8");
  textdomain(GETTEXT_PACKAGE);

  gtk_init_check(&argc, &arg);

  // parse command line arguments
  dt_mipmap_size_t min_mip = DT_MIPMAP_0;
  dt_mipmap_size_t max_mip = DT_MIPMAP_2;
  int32_t min_imgid = NO_IMGID;
  int32_t max_imgid = INT32_MAX;

  int k;
  for(k = 1; k < argc; k++)
  {
    if(!strcmp(arg[k], "-h") || !strcmp(arg[k], "--help"))
    {
      usage(arg[0]);
      exit(EXIT_FAILURE);
    }
    else if(!strcmp(arg[k], "--version"))
    {
      printf("this is darktable-generate-cache %s\ncopyright (c) 2014 johannes hanika; 2015 LebedevRI\n",
        darktable_package_version);
      exit(EXIT_FAILURE);
    }
    else if((!strcmp(arg[k], "-m") || !strcmp(arg[k], "--max-mip")) && argc > k + 1)
    {
      k++;
      max_mip = (dt_mipmap_size_t)MIN(MAX(atoi(arg[k]), DT_MIPMAP_0), DT_MIPMAP_8);
    }
    else if(!strcmp(arg[k], "--min-mip") && argc > k + 1)
    {
      k++;
      min_mip = (dt_mipmap_size_t)MIN(MAX(atoi(arg[k]), DT_MIPMAP_0), DT_MIPMAP_8);
    }
    else if(!strcmp(arg[k], "--min-imgid") && argc > k + 1)
    {
      k++;
      min_imgid = (int32_t)MIN(MAX(atoi(arg[k]), 0), INT32_MAX);
    }
    else if(!strcmp(arg[k], "--max-imgid") && argc > k + 1)
    {
      k++;
      max_imgid = (int32_t)MIN(MAX(atoi(arg[k]), 0), INT32_MAX);
    }
    else if(!strcmp(arg[k], "--core"))
    {
      // everything from here on should be passed to the core
      k++;
      break;
    }
  }

  int m_argc = 0;
  char **m_arg = malloc(sizeof(char *) * (3 + argc - k + 1));
  m_arg[m_argc++] = "darktable-generate-cache";
  m_arg[m_argc++] = "--conf";
  m_arg[m_argc++] = "write_sidecar_files=never";
  for(; k < argc; k++) m_arg[m_argc++] = arg[k];
  m_arg[m_argc] = NULL;

  // init dt without gui:
  if(dt_init(m_argc, m_arg, FALSE, TRUE, NULL))
  {
    free(m_arg);
    exit(EXIT_FAILURE);
  }

  if(!dt_conf_get_bool("cache_disk_backend"))
  {
    fprintf(stderr, _("warning: disk backend for thumbnail cache is disabled (cache_disk_backend)\nif you want "
                      "to pre-generate thumbnails and for darktable to use them, you need to enable disk backend "
                      "for thumbnail cache\nno thumbnails to be generated, done.\n"));
    dt_cleanup();
    free(m_arg);
    exit(EXIT_FAILURE);
  }

  if(max_mip == 8 && !dt_conf_get_bool("cache_disk_backend_full"))
  {
    fprintf(stderr,
            _("warning: disk backend for full preview cache is disabled (cache_disk_backend_full)\nif you want "
              "to pre-generate full previews and for darktable to use them, you need to enable disk backend "
              "for full preview cache\nno full previews to be generated, done.\n"));
    dt_cleanup();
    free(m_arg);
    exit(EXIT_FAILURE);
  }

  if(min_mip > max_mip)
  {
    fprintf(stderr, _("error: ensure that min_mip <= max_mip\n"));
    free(m_arg);
    exit(EXIT_FAILURE);
  }

  fprintf(stderr, _("creating complete lighttable thumbnail cache\n"));

  if(generate_thumbnail_cache(min_mip, max_mip, min_imgid, max_imgid))
  {
    free(m_arg);
    exit(EXIT_FAILURE);
  }

  dt_cleanup();

  free(m_arg);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

