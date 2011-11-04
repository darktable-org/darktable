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

#include <sqlite3.h>
#include <glib.h>
#include <gio/gio.h>

#include "common/debug.h"
#include "common/database.h"
#include "control/control.h"
#include "control/conf.h"

typedef struct dt_database_t
{
  gboolean is_new_database;

  /* database filename */
  gchar *dbfilename;

  /* ondisk DB */
  sqlite3 *ondisk_handle;

  /* working copy */
  sqlite3 *working_handle;
} dt_database_t;


/* migrates database from old place to new */
static void _database_migrate_to_xdg_structure();

/* copies a db into another */
static int _database_backup(sqlite3 *dst, sqlite3 *src);

gboolean dt_database_is_new(const dt_database_t *db)
{
  return db->is_new_database;
}

dt_database_t *dt_database_init(char *alternative)
{
  /* migrate default database location to new default */
  _database_migrate_to_xdg_structure();

  /* lets construct the db filename  */
  gchar * dbname = NULL;
  gchar dbfilename[1024] = {0};
  gchar datadir[1024] = {0};

  dt_util_get_user_config_dir(datadir, 1024);

  if ( alternative == NULL )
  {
    dbname = dt_conf_get_string ("database");
    if(!dbname)               snprintf(dbfilename, 1024, "%s/library.db", datadir);
    else if(dbname[0] != '/') snprintf(dbfilename, 1024, "%s/%s", datadir, dbname);
    else                      snprintf(dbfilename, 1024, "%s", dbname);
  }
  else
  {
    snprintf(dbfilename, 1024, "%s", alternative);
    dbname = g_file_get_basename (g_file_new_for_path(alternative));
  }

  /* create database */
  dt_database_t *db = (dt_database_t *)g_malloc(sizeof(dt_database_t));
  memset(db,0,sizeof(dt_database_t));
  db->dbfilename = g_strdup(dbfilename);
  db->is_new_database = FALSE;

  /* test if databasefile is available */
  if(!g_file_test(dbfilename, G_FILE_TEST_IS_REGULAR)) 
    db->is_new_database = TRUE;

  /* opening / creating database */
  if(sqlite3_open(db->dbfilename, &db->ondisk_handle))
  {
    fprintf(stderr, "[init] could not find database ");
    if(dbname) fprintf(stderr, "`%s'!\n", dbname);
    else       fprintf(stderr, "\n");
#ifndef HAVE_GCONF
    fprintf(stderr, "[init] maybe your %s/darktablerc is corrupt?\n",datadir);
    dt_util_get_datadir(dbfilename, 512);
    fprintf(stderr, "[init] try `cp %s/darktablerc %s/darktablerc'\n", dbfilename,datadir);
#else
    fprintf(stderr, "[init] check your /apps/darktable/database gconf entry!\n");
#endif
    if (dbname != NULL) g_free(dbname);
    return NULL;
  }

  if (sqlite3_open(":memory:", &db->working_handle))
  {
    db->working_handle = db->ondisk_handle;
  }
  else
  {
    if (_database_backup(db->working_handle, db->ondisk_handle))
    {
      sqlite3_close(db->working_handle);
      db->working_handle = db->ondisk_handle;
    }
  }

  if (db->working_handle == db->ondisk_handle)
  {
    fprintf(stderr, "[init] could not create an in memory db for faster collection processing. expect some slow io on your hdd\n");
  }

  /* attach a memory database to db connection for use with temporary tables
     used during instance life time, which is discarded on exit.
  */
  sqlite3_exec(db->working_handle, "attach database ':memory:' as memory",NULL,NULL,NULL);
  
  return db;
}

void dt_database_destroy(const dt_database_t *db)
{
  if (db->working_handle != db->ondisk_handle)
  {
    if (_database_backup(db->ondisk_handle, db->working_handle))
    {
      fprintf(stderr, "[close] could not write back in memory db to disk. your session work is only in the xmp, back them up!");
    }
    sqlite3_close(db->working_handle);
  }
  sqlite3_close(db->ondisk_handle);
  g_free((dt_database_t *)db);
}

sqlite3 *dt_database_get(const dt_database_t *db)
{
  return db->working_handle;
}


static void _database_migrate_to_xdg_structure()
{
  gchar dbfilename[2048]= {0};
  gchar *conf_db = dt_conf_get_string("database");
  
  gchar datadir[1024] = {0};
  dt_util_get_datadir(datadir, 1024);
  
  if (conf_db && conf_db[0] != '/')
  {
    char *homedir = getenv ("HOME");
    snprintf (dbfilename,2048,"%s/%s", homedir, conf_db);
    if (g_file_test (dbfilename, G_FILE_TEST_EXISTS))
    {
      fprintf(stderr, "[init] moving database into new XDG directory structure\n");
      char destdbname[2048]= {0};
      snprintf(destdbname,2048,"%s/%s",datadir,"library.db");
      if(!g_file_test (destdbname,G_FILE_TEST_EXISTS))
      {
	rename(dbfilename,destdbname);
	dt_conf_set_string("database","library.db");
      }
    }
    g_free(conf_db);
  }
}

static int _database_backup(sqlite3 *dst, sqlite3 *src)
{
  sqlite3_backup *backup;

  backup = sqlite3_backup_init(dst, "main", src, "main");
  if (!backup)
  {
    return -1;
  }

  int ret = sqlite3_backup_step(backup, -1);
  ret = !(SQLITE_DONE == ret);

  sqlite3_backup_finish(backup);

  return ret;
}
