/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

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

#include "catalog_fixture.h"
#include "common/database.h"
#include "common/debug.h"
#include "common/film.h"
#include "common/image.h"

#include <glib/gstdio.h>

static const char *_images[] = { "mire1.cr2", "hlrecovery.arw", "bayer4.crw", NULL };
static gchar *_tmpdir = NULL;

int dt_test_catalog_setup(void **state)
{
  _tmpdir = g_dir_make_tmp("dt-catalog-test-XXXXXX", NULL);
  if(!_tmpdir) return -1;

  // the build tree layout, passed by CMake: the test binary is not next to
  // darktable's data and modules
  char *argv[] = { "darktable-test",
                   "--library", ":memory:",
                   "--configdir", _tmpdir,
                   "--cachedir", _tmpdir,
                   "--datadir", DT_TEST_DATADIR,
                   "--moduledir", DT_TEST_MODULEDIR,
                   "--disable-opencl",
                   "--conf", "write_sidecar_files=never",
                   NULL };
  const int argc = sizeof(argv) / sizeof(argv[0]) - 1;
  if(dt_init(argc, argv, FALSE, FALSE, NULL)) return -1;

  dt_film_t film;
  dt_film_init(&film);
  const dt_filmid_t filmid = dt_film_new(&film, DT_TEST_IMAGES);
  dt_film_cleanup(&film);
  if(!dt_is_valid_filmid(filmid)) return -1;

  for(int i = 0; _images[i]; i++)
  {
    gchar *path = g_build_filename(DT_TEST_IMAGES, _images[i], NULL);
    const dt_imgid_t imgid = dt_image_import(filmid, path, TRUE, FALSE);
    g_free(path);
    if(!dt_is_valid_imgid(imgid)) return -1;
  }
  return 0;
}

int dt_test_catalog_teardown(void **state)
{
  dt_cleanup();
  if(_tmpdir)
  {
    // configdir and cachedir hold darktablerc, data.db and mipmap folders
    gchar *cmd = g_strdup_printf("rm -rf '%s'", _tmpdir);
    const int rc = system(cmd);
    g_free(cmd);
    g_free(_tmpdir);
    _tmpdir = NULL;
    return rc;
  }
  return 0;
}

dt_imgid_t dt_test_catalog_image(const char *filename)
{
  dt_imgid_t imgid = NO_IMGID;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT id FROM main.images WHERE filename = ?1 AND version = 0",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, filename, -1, SQLITE_TRANSIENT);
  if(sqlite3_step(stmt) == SQLITE_ROW)
    imgid = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  return imgid;
}
