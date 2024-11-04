/*
    This file is part of darktable,
    Copyright (C) 2011-2024 darktable developers.

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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "common/atomic.h"
#include "common/database.h"
#include "common/darktable.h"
#include "common/datetime.h"
#include "common/debug.h"
#include "common/file_location.h"
#include "common/iop_order.h"
#include "common/styles.h"
#include "common/history.h"
#ifdef HAVE_ICU
#include "common/sqliteicu.h"
#endif
#include "control/conf.h"
#include "control/control.h"

#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <sqlite3.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>

// NEVER change these; after these versions, NEVER update _create_*_schema(), either
// For consistency and reducing duplication / effort, after these versions, the full schema
// is created by running the upgrade steps.
#define LAST_FULL_DATABASE_VERSION_LIBRARY 55
#define LAST_FULL_DATABASE_VERSION_DATA    10
// You HAVE TO bump THESE versions whenever you add an update branches to _upgrade_*_schema_step()!
#define CURRENT_DATABASE_VERSION_LIBRARY 56
#define CURRENT_DATABASE_VERSION_DATA    10

#define USE_NESTED_TRANSACTIONS
#define MAX_NESTED_TRANSACTIONS 5
/* transaction id */
static dt_atomic_int _trxid;

typedef struct dt_database_t
{
  gboolean lock_acquired;

  /* data database filename */
  gchar *dbfilename_data, *lockfile_data;

  /* library database filename */
  gchar *dbfilename_library, *lockfile_library;

  /* ondisk DB */
  sqlite3 *handle;

  gchar *error_message, *error_dbfilename;
  int error_other_pid;
} dt_database_t;


/* migrates database from old place to new */
static void _database_migrate_to_xdg_structure();

/* delete old mipmaps files */
static void _database_delete_mipmaps_files();

#define _SQLITE3_EXEC(a, b, c, d, e)                                                                         \
  if(sqlite3_exec(a, b, c, d, e) != SQLITE_OK)                                                               \
  {                                                                                                          \
    all_ok = FALSE;                                                                                          \
    failing_query = b;                                                                                       \
    goto end;                                                                                                \
  }

int32_t dt_database_last_insert_rowid(const dt_database_t *db)
{
  return (int32_t)sqlite3_last_insert_rowid(db->handle);
}

/* migrate from the legacy db format (with the 'settings' blob) to the
   first version this system knows */
static gboolean _migrate_schema(dt_database_t *db, int version)
{
  gboolean all_ok = TRUE;
  const char *failing_query = NULL;
  sqlite3_stmt *stmt;
  sqlite3_stmt *innerstmt;

  if(version != 36) // if anyone shows up with an older db we can probably add extra code
    return FALSE;

  sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);
  // clang-format off
  // remove stuff that is either no longer needed or that got renamed
  _SQLITE3_EXEC(db->handle, "DROP TABLE IF EXISTS main.lock", NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "DROP TABLE IF EXISTS main.settings", NULL, NULL, NULL); // yes, we do this in many
                                                                                     // places. because it's really
                                                                                     // important to not miss it in
                                                                                     // any code path.
  _SQLITE3_EXEC(db->handle, "DROP INDEX IF EXISTS main.group_id_index", NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "DROP INDEX IF EXISTS main.imgid_index", NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "DROP TABLE IF EXISTS main.mipmaps", NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "DROP TABLE IF EXISTS main.mipmap_timestamps", NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "DROP TABLE IF EXISTS main.dt_migration_table", NULL, NULL, NULL);

  // using _create_library_schema() and filling that with the old data doesn't work since we always want to generate
  // version 1 tables
  ////////////////////////////// db_info
  _SQLITE3_EXEC(db->handle, "CREATE TABLE main.db_info (key VARCHAR PRIMARY KEY, value VARCHAR)",
                NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "INSERT OR REPLACE INTO main.db_info (key, value) VALUES ('version', 1)",
                NULL, NULL, NULL);
  ////////////////////////////// film_rolls
  _SQLITE3_EXEC(db->handle, "CREATE INDEX IF NOT EXISTS main.film_rolls_folder_index ON film_rolls (folder)",
                NULL, NULL, NULL);
  ////////////////////////////// images
  sqlite3_exec(db->handle, "ALTER TABLE main.images ADD COLUMN orientation INTEGER", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "ALTER TABLE main.images ADD COLUMN focus_distance REAL", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "ALTER TABLE main.images ADD COLUMN group_id INTEGER", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "ALTER TABLE main.images ADD COLUMN histogram BLOB", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "ALTER TABLE main.images ADD COLUMN lightmap BLOB", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "ALTER TABLE main.images ADD COLUMN longitude REAL", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "ALTER TABLE main.images ADD COLUMN latitude REAL", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "ALTER TABLE main.images ADD COLUMN color_matrix BLOB", NULL, NULL, NULL);
  // the colorspace as specified in some image types
  sqlite3_exec(db->handle, "ALTER TABLE main.images ADD COLUMN colorspace INTEGER", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "ALTER TABLE main.images ADD COLUMN version INTEGER", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "ALTER TABLE main.images ADD COLUMN max_version INTEGER", NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "UPDATE main.images SET orientation = -1 WHERE orientation IS NULL", NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "UPDATE main.images SET focus_distance = -1 WHERE focus_distance IS NULL",
                NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "UPDATE main.images SET group_id = id WHERE group_id IS NULL", NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "UPDATE main.images SET max_version = (SELECT COUNT(*)-1 FROM main.images i WHERE "
                            "i.filename = main.images.filename AND "
                            "i.film_id = main.images.film_id) WHERE max_version IS NULL",
                NULL, NULL, NULL);
  _SQLITE3_EXEC(
      db->handle,
      "UPDATE main.images SET version = (SELECT COUNT(*) FROM main.images i "
      "WHERE i.filename = main.images.filename AND "
      "i.film_id = main.images.film_id AND i.id < main.images.id) WHERE version IS NULL",
      NULL, NULL, NULL);
  // make sure we have AUTOINCREMENT on imgid --> move the whole thing away and recreate the table :(
  _SQLITE3_EXEC(db->handle, "ALTER TABLE main.images RENAME TO dt_migration_table", NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "DROP INDEX IF EXISTS main.images_group_id_index", NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "DROP INDEX IF EXISTS main.images_film_id_index", NULL, NULL, NULL);
  _SQLITE3_EXEC(
      db->handle,
      "CREATE TABLE main.images (id INTEGER PRIMARY KEY AUTOINCREMENT, group_id INTEGER, film_id INTEGER, "
      "width INTEGER, height INTEGER, filename VARCHAR, maker VARCHAR, model VARCHAR, "
      "lens VARCHAR, exposure REAL, aperture REAL, iso REAL, focal_length REAL, "
      "focus_distance REAL, datetime_taken CHAR(20), flags INTEGER, "
      "output_width INTEGER, output_height INTEGER, crop REAL, "
      "raw_parameters INTEGER, raw_denoise_threshold REAL, "
      "raw_auto_bright_threshold REAL, raw_black INTEGER, raw_maximum INTEGER, "
      "caption VARCHAR, description VARCHAR, license VARCHAR, sha1sum CHAR(40), "
      "orientation INTEGER, histogram BLOB, lightmap BLOB, longitude REAL, "
      "latitude REAL, color_matrix BLOB, colorspace INTEGER, version INTEGER, max_version INTEGER)",
      NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "CREATE INDEX main.images_group_id_index ON images (group_id)", NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "CREATE INDEX main.images_film_id_index ON images (film_id)", NULL, NULL, NULL);
  _SQLITE3_EXEC(
      db->handle,
      "INSERT INTO main.images (id, group_id, film_id, width, height, filename, maker, model, "
      "lens, exposure, aperture, iso, focal_length, focus_distance, datetime_taken, flags, "
      "output_width, output_height, crop, raw_parameters, raw_denoise_threshold, "
      "raw_auto_bright_threshold, raw_black, raw_maximum, caption, description, license, sha1sum, "
      "orientation, histogram, lightmap, longitude, latitude, color_matrix, colorspace, version, "
      "max_version) "
      "SELECT id, group_id, film_id, width, height, filename, maker, model, lens, exposure, aperture, iso, "
      "focal_length, focus_distance, datetime_taken, flags, output_width, output_height, crop, "
      "raw_parameters, raw_denoise_threshold, raw_auto_bright_threshold, raw_black, raw_maximum, "
      "caption, description, license, sha1sum, orientation, histogram, lightmap, longitude, "
      "latitude, color_matrix, colorspace, version, max_version FROM dt_migration_table",
      NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "DROP TABLE dt_migration_table", NULL, NULL, NULL);
  ////////////////////////////// selected_images
  // selected_images should have a primary key. add it if it's missing:
  _SQLITE3_EXEC(db->handle, "CREATE TEMPORARY TABLE dt_migration_table (imgid INTEGER)", NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "INSERT INTO dt_migration_table SELECT imgid FROM main.selected_images", NULL, NULL,
                NULL);
  _SQLITE3_EXEC(db->handle, "DROP TABLE main.selected_images", NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "CREATE TABLE main.selected_images (imgid INTEGER PRIMARY KEY)", NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "INSERT OR IGNORE INTO main.selected_images SELECT imgid FROM dt_migration_table",
                NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "DROP TABLE dt_migration_table", NULL, NULL, NULL);
  ////////////////////////////// history
  sqlite3_exec(db->handle, "ALTER TABLE main.history ADD COLUMN blendop_params BLOB", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "ALTER TABLE main.history ADD COLUMN blendop_version INTEGER", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "ALTER TABLE main.history ADD COLUMN multi_priority INTEGER", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "ALTER TABLE main.history ADD COLUMN multi_name VARCHAR(256)", NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "CREATE INDEX IF NOT EXISTS main.history_imgid_index ON history (imgid)", NULL, NULL,
                NULL);
  _SQLITE3_EXEC(db->handle, "UPDATE main.history SET blendop_version = 1 WHERE blendop_version IS NULL", NULL,
                NULL, NULL);
  _SQLITE3_EXEC(db->handle, "UPDATE main.history SET multi_priority = 0 WHERE multi_priority IS NULL", NULL, NULL,
                NULL);
  _SQLITE3_EXEC(db->handle, "UPDATE main.history SET multi_name = ' ' WHERE multi_name IS NULL", NULL, NULL, NULL);
  ////////////////////////////// mask
  _SQLITE3_EXEC(db->handle, "CREATE TABLE IF NOT EXISTS main.mask (imgid INTEGER, formid INTEGER, form INTEGER, "
                            "name VARCHAR(256), version INTEGER, "
                            "points BLOB, points_count INTEGER, source BLOB)",
                NULL, NULL, NULL);
  sqlite3_exec(db->handle, "ALTER TABLE main.mask ADD COLUMN source BLOB", NULL, NULL,
               NULL); // in case the table was there already but missed that column
  ////////////////////////////// tagged_images
  _SQLITE3_EXEC(db->handle, "CREATE INDEX IF NOT EXISTS main.tagged_images_tagid_index ON tagged_images (tagid)",
                NULL, NULL, NULL);
  ////////////////////////////// styles
  _SQLITE3_EXEC(db->handle,
                "CREATE TABLE IF NOT EXISTS main.styles (id INTEGER, name VARCHAR, description VARCHAR)", NULL,
                NULL, NULL);
  sqlite3_exec(db->handle, "ALTER TABLE main.styles ADD COLUMN id INTEGER", NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "UPDATE main.styles SET id = rowid WHERE id IS NULL", NULL, NULL, NULL);
  ////////////////////////////// style_items
  _SQLITE3_EXEC(db->handle, "CREATE TABLE IF NOT EXISTS main.style_items (styleid INTEGER, num INTEGER, module "
                            "INTEGER, operation VARCHAR(256), op_params BLOB, "
                            "enabled INTEGER, blendop_params BLOB, blendop_version INTEGER, multi_priority "
                            "INTEGER, multi_name VARCHAR(256))",
                NULL, NULL, NULL);
  sqlite3_exec(db->handle, "ALTER TABLE main.style_items ADD COLUMN blendop_params BLOB", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "ALTER TABLE main.style_items ADD COLUMN blendop_version INTEGER", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "ALTER TABLE main.style_items ADD COLUMN multi_priority INTEGER", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "ALTER TABLE main.style_items ADD COLUMN multi_name VARCHAR(256)", NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "UPDATE main.style_items SET blendop_version = 1 WHERE blendop_version IS NULL", NULL,
                NULL, NULL);
  _SQLITE3_EXEC(db->handle, "UPDATE main.style_items SET multi_priority = 0 WHERE multi_priority IS NULL", NULL,
                NULL, NULL);
  _SQLITE3_EXEC(db->handle, "UPDATE main.style_items SET multi_name = ' ' WHERE multi_name IS NULL", NULL, NULL,
                NULL);
  ////////////////////////////// color_labels
  // color_labels could have a PRIMARY KEY that we don't want
  _SQLITE3_EXEC(db->handle, "CREATE TEMPORARY TABLE dt_migration_table (imgid INTEGER, color INTEGER)", NULL,
                NULL, NULL);
  _SQLITE3_EXEC(db->handle, "INSERT INTO dt_migration_table SELECT imgid, color FROM main.color_labels", NULL,
                NULL, NULL);
  _SQLITE3_EXEC(db->handle, "DROP TABLE main.color_labels", NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "CREATE TABLE main.color_labels (imgid INTEGER, color INTEGER)", NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "CREATE UNIQUE INDEX main.color_labels_idx ON color_labels (imgid, color)", NULL, NULL,
                NULL);
  _SQLITE3_EXEC(db->handle, "INSERT OR IGNORE INTO main.color_labels SELECT imgid, color FROM dt_migration_table",
                NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "DROP TABLE dt_migration_table", NULL, NULL, NULL);
  ////////////////////////////// meta_data
  _SQLITE3_EXEC(db->handle, "CREATE TABLE IF NOT EXISTS main.meta_data (id INTEGER, key INTEGER, value VARCHAR)",
                NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "CREATE INDEX IF NOT EXISTS main.metadata_index ON meta_data (id, key)", NULL, NULL,
                NULL);
  ////////////////////////////// presets
  _SQLITE3_EXEC(db->handle, "CREATE TABLE IF NOT EXISTS main.presets (name VARCHAR, description VARCHAR, "
                            "operation VARCHAR, op_version INTEGER, op_params BLOB, "
                            "enabled INTEGER, blendop_params BLOB, blendop_version INTEGER, multi_priority "
                            "INTEGER, multi_name VARCHAR(256), "
                            "model VARCHAR, maker VARCHAR, lens VARCHAR, iso_min REAL, iso_max REAL, "
                            "exposure_min REAL, exposure_max REAL, "
                            "aperture_min REAL, aperture_max REAL, focal_length_min REAL, focal_length_max "
                            "REAL, writeprotect INTEGER, "
                            "autoapply INTEGER, filter INTEGER, def INTEGER, isldr INTEGER)",
                NULL, NULL, NULL);
  sqlite3_exec(db->handle, "ALTER TABLE main.presets ADD COLUMN op_version INTEGER", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "ALTER TABLE main.presets ADD COLUMN blendop_params BLOB", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "ALTER TABLE main.presets ADD COLUMN blendop_version INTEGER", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "ALTER TABLE main.presets ADD COLUMN multi_priority INTEGER", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "ALTER TABLE main.presets ADD COLUMN multi_name VARCHAR(256)", NULL, NULL, NULL);
  // the unique index only works if the db doesn't have any (name, operation, op_version) more than once.
  // apparently there are dbs out there which do have that. :(
  sqlite3_prepare_v2(db->handle,
                     "SELECT p.rowid, p.name, p.operation, p.op_version FROM main.presets p INNER JOIN "
                     "(SELECT * FROM (SELECT rowid, name, operation, op_version, COUNT(*) AS count "
                     "FROM main.presets GROUP BY name, operation, op_version) WHERE count > 1) s "
                     "ON p.name = s.name AND p.operation = s.operation AND p.op_version = s.op_version",
                     -1, &stmt, NULL);
  // clang-format on
  char *last_name = NULL, *last_operation = NULL;
  int last_op_version = 0;
  int i = 0;
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const int rowid = sqlite3_column_int(stmt, 0);
    const char *name = (const char *)sqlite3_column_text(stmt, 1);
    const char *operation = (const char *)sqlite3_column_text(stmt, 2);
    const int op_version = sqlite3_column_int(stmt, 3);

    // is it still the same (name, operation, op_version) triple?
    if(!last_name || strcmp(last_name, name) || !last_operation || strcmp(last_operation, operation)
       || last_op_version != op_version)
    {
      g_free(last_name);
      g_free(last_operation);
      last_name = g_strdup(name);
      last_operation = g_strdup(operation);
      last_op_version = op_version;
      i = 0;
    }

    // find the next free amended version of name
    // clang-format off
    sqlite3_prepare_v2(db->handle, "SELECT name FROM main.presets  WHERE name = ?1 || ' (' || ?2 || ')' AND "
                                   "operation = ?3 AND op_version = ?4",
                       -1, &innerstmt, NULL);
    // clang-format on
    while(1)
    {
      sqlite3_bind_text(innerstmt, 1, name, -1, SQLITE_TRANSIENT);
      sqlite3_bind_int(innerstmt, 2, i);
      sqlite3_bind_text(innerstmt, 3, operation, -1, SQLITE_TRANSIENT);
      sqlite3_bind_int(innerstmt, 4, op_version);
      if(sqlite3_step(innerstmt) != SQLITE_ROW) break;
      sqlite3_reset(innerstmt);
      sqlite3_clear_bindings(innerstmt);
      i++;
    }
    sqlite3_finalize(innerstmt);

    // rename preset
    // clang-format off
    const char *query = "UPDATE main.presets SET name = name || ' (' || ?1 || ')' WHERE rowid = ?2";
    // clang-format on
    sqlite3_prepare_v2(db->handle, query, -1, &innerstmt, NULL);
    sqlite3_bind_int(innerstmt, 1, i);
    sqlite3_bind_int(innerstmt, 2, rowid);
    if(sqlite3_step(innerstmt) != SQLITE_DONE)
    {
      all_ok = FALSE;
      failing_query = query;
      goto end;
    }
    sqlite3_finalize(innerstmt);
  }
  sqlite3_finalize(stmt);
  g_free(last_name);
  g_free(last_operation);
  // now we should be able to create the index
  // clang-format off
  _SQLITE3_EXEC(db->handle,
                "CREATE UNIQUE INDEX IF NOT EXISTS main.presets_idx ON presets (name, operation, op_version)",
                NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "UPDATE main.presets SET blendop_version = 1 WHERE blendop_version IS NULL", NULL,
                NULL, NULL);
  _SQLITE3_EXEC(db->handle, "UPDATE main.presets SET multi_priority = 0 WHERE multi_priority IS NULL", NULL, NULL,
                NULL);
  _SQLITE3_EXEC(db->handle, "UPDATE main.presets SET multi_name = ' ' WHERE multi_name IS NULL", NULL, NULL, NULL);
  // clang-format on


  // There are systems where absolute paths don't start with '/' (like Windows).
  // Since the bug which introduced absolute paths to the db was fixed before a
  // Windows build was available this shouldn't matter though.
  // clang-format off
  sqlite3_prepare_v2(db->handle, "SELECT id, filename FROM main.images WHERE filename LIKE '/%'", -1, &stmt, NULL);
  sqlite3_prepare_v2(db->handle, "UPDATE main.images SET filename = ?1 WHERE id = ?2", -1, &innerstmt, NULL);
  // clang-format on
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const int id = sqlite3_column_int(stmt, 0);
    const char *path = (const char *)sqlite3_column_text(stmt, 1);
    gchar *filename = g_path_get_basename(path);
    sqlite3_bind_text(innerstmt, 1, filename, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(innerstmt, 2, id);
    sqlite3_step(innerstmt);
    sqlite3_reset(innerstmt);
    sqlite3_clear_bindings(innerstmt);
    g_free(filename);
  }
  sqlite3_finalize(stmt);
  sqlite3_finalize(innerstmt);

  // We used to insert datetime_taken entries with '-' as date separators. Since that doesn't work well with
  // the regular ':' when parsing
  // or sorting we changed it to ':'. This takes care to change what we have as leftovers
  // clang-format off
  _SQLITE3_EXEC(
      db->handle,
      "UPDATE main.images SET datetime_taken = REPLACE(datetime_taken, '-', ':') WHERE datetime_taken LIKE '%-%'",
      NULL, NULL, NULL);
  // clang-format on

end:
  if(all_ok)
    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
  else
  {
    dt_print(DT_DEBUG_ALWAYS, "[init] failing query: `%s' sql: '%s", failing_query, sqlite3_errmsg(db->handle));
    sqlite3_exec(db->handle, "ROLLBACK TRANSACTION", NULL, NULL, NULL);
  }

  return all_ok;
}

#undef _SQLITE3_EXEC

#define TRY_EXEC(_query, _message)                                               \
  do                                                                             \
  {                                                                              \
    if(sqlite3_exec(db->handle, _query, NULL, NULL, NULL) != SQLITE_OK)          \
    {                                                                            \
      dt_print(DT_DEBUG_ALWAYS, "TRY_EXEC '%s' sql: '%s'", _message, sqlite3_errmsg(db->handle)); \
      FINALIZE;                                                                  \
      sqlite3_exec(db->handle, "ROLLBACK TRANSACTION", NULL, NULL, NULL);        \
      return version;                                                            \
    }                                                                            \
  } while(0)

#define TRY_STEP(_stmt, _expected, _message)                                     \
  do                                                                             \
  {                                                                              \
    if(sqlite3_step(_stmt) != _expected)                                         \
    {                                                                            \
      dt_print(DT_DEBUG_ALWAYS, "TRY_STEP '%s' sql: '%s'", _message, sqlite3_errmsg(db->handle)); \
      FINALIZE;                                                                  \
      sqlite3_exec(db->handle, "ROLLBACK TRANSACTION", NULL, NULL, NULL);        \
      return version;                                                            \
    }                                                                            \
  } while(0)

#define TRY_PREPARE(_stmt, _query, _message)                                     \
  do                                                                             \
  {                                                                              \
    if(sqlite3_prepare_v2(db->handle, _query, -1, &_stmt, NULL) != SQLITE_OK)    \
    {                                                                            \
      dt_print(DT_DEBUG_ALWAYS, "TRY_PREPARE '%s' sql: '%s'", _message, sqlite3_errmsg(db->handle)); \
      FINALIZE;                                                                  \
      sqlite3_exec(db->handle, "ROLLBACK TRANSACTION", NULL, NULL, NULL);        \
      return version;                                                            \
    }                                                                            \
  } while(0)

// redefine this where needed
#define FINALIZE

/* do the real migration steps, returns the version the db was converted to */
static int _upgrade_library_schema_step(dt_database_t *db, int version)
{
  sqlite3_stmt *stmt;
  int new_version = version;
  if(version == CURRENT_DATABASE_VERSION_LIBRARY)
    return version;
  else if(version == 0)
  {
    // this can't happen, we started with 1, but it's a good example how this function works
    // <do some magic to the db>
    new_version = 1; // the version we transformed the db to. this way it might be possible to roll back or
                     // add fast paths
  }
  else if(version == 1)
  {
    // 1 -> 2 added write_timestamp
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);
    // clang-format off
    TRY_EXEC("ALTER TABLE main.images ADD COLUMN write_timestamp INTEGER",
             "can't add `write_timestamp' column to database");
    TRY_EXEC("UPDATE main.images SET write_timestamp = STRFTIME('%s', 'now') WHERE write_timestamp IS NULL",
             "can't initialize `write_timestamp' with current point in time");
    // clang-format on
    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
    new_version = 2;
  }
  else if(version == 2)
  {
    // 2 -> 3 reset raw_black and raw_maximum. in theory we should change the columns from REAL to INTEGER,
    // but sqlite doesn't care about types so whatever
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);
    // clang-format off
    TRY_EXEC("UPDATE main.images SET raw_black = 0, raw_maximum = 16384",
             "can't reset raw_black and raw_maximum");
    // clang-format on
    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
    new_version = 3;
  }
  else if(version == 3)
  {
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);
    // clang-format off
    TRY_EXEC("CREATE TRIGGER insert_tag AFTER INSERT ON main.tags"
             " BEGIN"
             "   INSERT INTO tagxtag SELECT id, new.id, 0 FROM TAGS;"
             "   UPDATE tagxtag SET count = 1000000 WHERE id1=new.id AND id2=new.id;"
             " END",
             "can't create insert_tag trigger");
    TRY_EXEC("CREATE TRIGGER delete_tag BEFORE DELETE ON main.tags"
             " BEGIN"
             "   DELETE FROM tagxtag WHERE id1=old.id OR id2=old.id;"
             "   DELETE FROM tagged_images WHERE tagid=old.id;"
             " END",
             "can't create delete_tag trigger");
    TRY_EXEC("CREATE TRIGGER attach_tag AFTER INSERT ON main.tagged_images"
             " BEGIN"
             "   UPDATE tagxtag"
             "     SET count = count + 1"
             "     WHERE (id1=new.tagid AND id2 IN (SELECT tagid FROM tagged_images WHERE imgid=new.imgid))"
             "        OR (id2=new.tagid AND id1 IN (SELECT tagid FROM tagged_images WHERE imgid=new.imgid));"
             " END",
             "can't create attach_tag trigger");
    TRY_EXEC("CREATE TRIGGER detach_tag BEFORE DELETE ON main.tagged_images"
             " BEGIN"
             "   UPDATE tagxtag"
             "     SET count = count - 1"
             "     WHERE (id1=old.tagid AND id2 IN (SELECT tagid FROM tagged_images WHERE imgid=old.imgid))"
             "        OR (id2=old.tagid AND id1 IN (SELECT tagid FROM tagged_images WHERE imgid=old.imgid));"
             " END",
             "can't create detach_tag trigger");
    // clang-format on
    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
    new_version = 4;
  }
  else if(version == 4)
  {
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);
    // clang-format off
    TRY_EXEC("ALTER TABLE main.presets RENAME TO tmp_presets", "can't rename table presets");

    TRY_EXEC("CREATE TABLE main.presets (name VARCHAR, description VARCHAR, operation VARCHAR, op_params BLOB,"
             "enabled INTEGER, blendop_params BLOB, model VARCHAR, maker VARCHAR, lens VARCHAR,"
             "iso_min REAL, iso_max REAL, exposure_min REAL, exposure_max REAL, aperture_min REAL,"
             "aperture_max REAL, focal_length_min REAL, focal_length_max REAL, writeprotect INTEGER,"
             "autoapply INTEGER, filter INTEGER, def INTEGER, format INTEGER, op_version INTEGER,"
             "blendop_version INTEGER, multi_priority INTEGER, multi_name VARCHAR(256))",
             "can't create new presets table");

    TRY_EXEC("INSERT INTO main.presets (name, description, operation, op_params, enabled, blendop_params, model, "
             "maker, lens, iso_min, iso_max, exposure_min, exposure_max, aperture_min, aperture_max,"
             "focal_length_min, focal_length_max, writeprotect, autoapply, filter, def, format, op_version, "
             "blendop_version, multi_priority, multi_name) SELECT name, description, operation, op_params, "
             "enabled, blendop_params, model, maker, lens, iso_min, iso_max, exposure_min, exposure_max, "
             "aperture_min, aperture_max, focal_length_min, focal_length_max, writeprotect, autoapply, filter, "
             "def, isldr, op_version, blendop_version, multi_priority, multi_name FROM tmp_presets",
             "can't populate presets table from tmp_presets");

    TRY_EXEC("DROP TABLE tmp_presets", "can't delete table tmp_presets");
    // clang-format on
    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
    new_version = 5;
  }
  else if(version == 5)
  {
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);
    // clang-format off
    TRY_EXEC("CREATE INDEX main.images_filename_index ON images (filename)",
             "can't create index on image filename");
    // clang-format on
    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
    new_version = 6;
  }
  else if(version == 6)
  {
    // some ancient tables can have the styleid column of style_items be called style_id. fix that.
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);

    if(sqlite3_exec(db->handle, "SELECT style_id FROM main.style_items", NULL, NULL, NULL) == SQLITE_OK)
    {
      // clang-format off
      TRY_EXEC("ALTER TABLE main.style_items RENAME TO tmp_style_items",
               "can't rename table style_items");

      TRY_EXEC("CREATE TABLE main.style_items (styleid INTEGER, num INTEGER, module INTEGER, "
               "operation VARCHAR(256), op_params BLOB, enabled INTEGER, "
               "blendop_params BLOB, blendop_version INTEGER, multi_priority INTEGER, multi_name VARCHAR(256))",
               "can't create new style_items table");

      TRY_EXEC("INSERT INTO main.style_items (styleid, num, module, operation, op_params, enabled,"
               "                         blendop_params, blendop_version, multi_priority, multi_name)"
               "                  SELECT style_id, num, module, operation, op_params, enabled,"
               "                         blendop_params, blendop_version, multi_priority, multi_name"
               "                  FROM   tmp_style_items",
               "can't populate style_items table from tmp_style_items");

      TRY_EXEC("DROP TABLE tmp_style_items", "can't delete table tmp_style_items");
      // clang-format on
    }

    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
    new_version = 7;
  }
  else if(version == 7)
  {
    // make sure that we have no film rolls with a NULL folder
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);
    // clang-format off
    TRY_EXEC("ALTER TABLE main.film_rolls RENAME TO tmp_film_rolls", "can't rename table film_rolls");

    TRY_EXEC("CREATE TABLE main.film_rolls "
             "(id INTEGER PRIMARY KEY, datetime_accessed CHAR(20), "
             "folder VARCHAR(1024) NOT NULL)",
             "can't create new film_rolls table");

    TRY_EXEC("INSERT INTO main.film_rolls (id, datetime_accessed, folder) "
             "SELECT id, datetime_accessed, folder "
             "FROM   tmp_film_rolls "
             "WHERE  folder IS NOT NULL",
             "can't populate film_rolls table from tmp_film_rolls");

    TRY_EXEC("DROP TABLE tmp_film_rolls", "can't delete table tmp_film_rolls");
    // clang-format on
    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
    new_version = 8;
  }
  else if(version == 8)
  {
    // 8 -> 9 added history_end column to images
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);
    // clang-format off
    TRY_EXEC("ALTER TABLE main.images ADD COLUMN history_end INTEGER",
             "can't add `history_end' column to database");

    TRY_EXEC("UPDATE main.images SET history_end = (SELECT IFNULL(MAX(num) + 1, 0) FROM main.history "
             "WHERE imgid = id)",
             "can't initialize `history_end' with last history entry");
    // clang-format on
    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
    new_version = 9;
  }
  else if(version == 9)
  {
    // 9 -> 10 cleanup of last update :(
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);
    // clang-format off
    TRY_EXEC("UPDATE main.images SET history_end = (SELECT IFNULL(MAX(num) + 1, 0) FROM main.history "
             "WHERE imgid = id)",
             "can't set `history_end' to 0 where it was NULL");
    // clang-format on
    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
    new_version = 10;
  }
  else if(version == 10)
  {
    // 10 -> 11 added altitude column to images
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);
    // clang-format off
    TRY_EXEC("ALTER TABLE main.images ADD COLUMN altitude REAL",
             "can't add `altitude' column to database");

    TRY_EXEC("UPDATE main.images SET altitude = NULL", "can't initialize `altitude' with NULL");
    // clang-format on
    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
    new_version = 11;
  }
  else if(version == 11)
  {
    // 11 -> 12 tagxtag was removed in order to reduce database size
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);
    // clang-format off
    TRY_EXEC("DROP TRIGGER main.detach_tag", "can't drop trigger `detach_tag' from database");

    TRY_EXEC("DROP TRIGGER main.attach_tag", "can't drop trigger `attach_tag' from database");

    TRY_EXEC("DROP TRIGGER main.delete_tag", "can't drop trigger `delete_tag' from database");

    TRY_EXEC("DROP TRIGGER main.insert_tag", "can't drop trigger `insert_tag' from database");

    TRY_EXEC("DROP TABLE main.tagxtag", "can't drop table `tagxtag' from database");
    // clang-format on
    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
    new_version = 12;
  }
  else if(version == 12)
  {
    // 11 -> 12 move presets, styles and tags over to the data database
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);

    ////////////// presets
#undef FINALIZE
#define FINALIZE                                                                   \
    do                                                                             \
    {                                                                              \
      sqlite3_finalize(stmt);                                                      \
      sqlite3_finalize(select_stmt);                                               \
      sqlite3_finalize(count_clashes_stmt);                                        \
      sqlite3_finalize(update_name_stmt);                                          \
      sqlite3_finalize(insert_stmt);                                               \
      sqlite3_finalize(delete_stmt);                                               \
    } while(0)

    stmt = NULL;
    sqlite3_stmt *insert_stmt = NULL, *delete_stmt = NULL, *select_stmt = NULL, *count_clashes_stmt = NULL,
    *update_name_stmt = NULL;
    // clang-format off
    // remove presets that are already in data.
    // we can't use a NATURAL JOIN here as that fails when columns have NULL values. :-(
    TRY_EXEC("DELETE FROM main.presets WHERE rowid IN (SELECT p1.rowid FROM main.presets p1 "
             "JOIN data.presets p2 ON "
                 "p1.name IS p2.name AND "
                 "p1.description IS p2.description AND "
                 "p1.operation IS p2.operation AND "
                 "p1.op_version IS p2.op_version AND "
                 "p1.op_params IS p2.op_params AND "
                 "p1.enabled IS p2.enabled AND "
                 "p1.blendop_params IS p2.blendop_params AND "
                 "p1.blendop_version IS p2.blendop_version AND "
                 "p1.multi_priority IS p2.multi_priority AND "
                 "p1.multi_name IS p2.multi_name AND "
                 "p1.model IS p2.model AND "
                 "p1.maker IS p2.maker AND "
                 "p1.lens IS p2.lens AND "
                 "p1.iso_min IS p2.iso_min AND "
                 "p1.iso_max IS p2.iso_max AND "
                 "p1.exposure_min IS p2.exposure_min AND "
                 "p1.exposure_max IS p2.exposure_max AND "
                 "p1.aperture_min IS p2.aperture_min AND "
                 "p1.aperture_max IS p2.aperture_max AND "
                 "p1.focal_length_min IS p2.focal_length_min AND "
                 "p1.focal_length_max IS p2.focal_length_max AND "
                 "p1.writeprotect IS p2.writeprotect AND "
                 "p1.autoapply IS p2.autoapply AND "
                 "p1.filter IS p2.filter AND "
                 "p1.def IS p2.def AND "
                 "p1.format IS p2.format "
             "WHERE p1.writeprotect = 0)",
             "can't delete already migrated presets from database");

    // find all presets that are clashing with something else in presets. that can happen as we introduced an
    // index on presets in data which wasn't in place in library.
    TRY_PREPARE(select_stmt, "SELECT p.rowid, r FROM main.presets AS p, (SELECT rowid AS r, name, operation, "
                             "op_version FROM main.presets GROUP BY name, operation, op_version HAVING "
                             "COUNT(*) > 1) USING (name, operation, op_version) WHERE p.rowid != r",
                "can't prepare selecting presets with same name, operation, op_version from database");

    // see if an updated preset name still causes problems
    TRY_PREPARE(count_clashes_stmt, "SELECT COUNT(*) FROM main.presets AS p, (SELECT name, operation, op_version "
                                    "FROM main.presets WHERE rowid = ?1) AS i ON p.name = i.name || \" #\" || ?2 "
                                    "AND p.operation = i.operation AND p.op_version = i.op_version",
                "can't prepare selection of preset count by name from database");

    // update the preset name for good
    TRY_PREPARE(update_name_stmt, "UPDATE main.presets SET name = name || \" #\" || ?1 WHERE rowid = ?2",
                "can't prepare updating of preset name in database");

    // find all presets that would be clashing with something in data
    TRY_PREPARE(stmt, "SELECT p1.rowid FROM main.presets p1 INNER JOIN data.presets p2 "
                      "USING (name, operation, op_version) WHERE p1.writeprotect = 0",
                "can't access table `presets' in database");

    // ... and move them over with a new name
    TRY_PREPARE(insert_stmt, "INSERT OR FAIL INTO data.presets (name, description, operation, op_version, "
                             "op_params, enabled, blendop_params, blendop_version, multi_priority, multi_name, "
                             "model, maker, lens, iso_min, iso_max, exposure_min, exposure_max, aperture_min, "
                             "aperture_max, focal_length_min, focal_length_max, writeprotect, autoapply, filter, "
                             "def, format) "
                             "SELECT name || \" #\" || ?1, description, operation, op_version, op_params, "
                             "enabled, blendop_params, blendop_version, multi_priority, multi_name, model, maker, "
                             "lens, iso_min, iso_max, exposure_min, exposure_max, aperture_min, aperture_max, "
                             "focal_length_min, focal_length_max, writeprotect, autoapply, filter, def, format "
                             "FROM main.presets p1 WHERE p1.rowid = ?2",
                "can't prepare insertion statement");

    TRY_PREPARE(delete_stmt, "DELETE FROM main.presets WHERE rowid = ?1", "can't prepare deletion statement");
    // clang-format on
    // first rename presets with (name, operation, op_version) not being unique
    while(sqlite3_step(select_stmt) == SQLITE_ROW)
    {
      const int own_rowid = sqlite3_column_int(select_stmt, 0);
      const int other_rowid = sqlite3_column_int(select_stmt, 1);
      int preset_version = 0;

      do
      {
        preset_version++;
        sqlite3_reset(count_clashes_stmt);
        sqlite3_clear_bindings(count_clashes_stmt);
        sqlite3_bind_int(count_clashes_stmt, 1, other_rowid);
        sqlite3_bind_int(count_clashes_stmt, 2, preset_version);
      }
      while(sqlite3_step(count_clashes_stmt) == SQLITE_ROW && sqlite3_column_int(count_clashes_stmt, 0) > 0);

      sqlite3_bind_int(update_name_stmt, 1, preset_version);
      sqlite3_bind_int(update_name_stmt, 2, own_rowid);
      TRY_STEP(update_name_stmt, SQLITE_DONE, "can't rename preset in database");
      sqlite3_reset(update_name_stmt);
      sqlite3_reset(update_name_stmt);
    }

    // now rename to avoid clashes with data.presets
    while(sqlite3_step(stmt) == SQLITE_ROW)
    {
      int preset_version = 0;
      const int rowid = sqlite3_column_int(stmt, 0);

      do
      {
        preset_version++;
        sqlite3_reset(insert_stmt);
        sqlite3_clear_bindings(insert_stmt);
        sqlite3_bind_int(insert_stmt, 1, preset_version);
        sqlite3_bind_int(insert_stmt, 2, rowid);
      } while(sqlite3_step(insert_stmt) != SQLITE_DONE);

      sqlite3_reset(delete_stmt);
      sqlite3_clear_bindings(delete_stmt);
      sqlite3_bind_int(delete_stmt, 1, rowid);
      TRY_STEP(delete_stmt, SQLITE_DONE, "can't delete preset from database");
    }
    // clang-format off
    // all that is left in presets should be those that can be moved over without any further concerns
    TRY_EXEC("INSERT OR FAIL INTO data.presets SELECT name, description, operation, "
             "op_version, op_params, enabled, blendop_params, blendop_version, "
             "multi_priority, multi_name, model, maker, lens, iso_min, iso_max, "
             "exposure_min, exposure_max, aperture_min, aperture_max, "
             "focal_length_min, focal_length_max, writeprotect, autoapply, filter, "
             "def, format FROM main.presets WHERE writeprotect = 0",
             "can't copy presets to the data database");
    // ... delete them on the old side
    TRY_EXEC("DELETE FROM main.presets WHERE writeprotect = 0",
             "can't copy presets to the data database");
    // clang-format on

    FINALIZE;
#undef FINALIZE

    ////////////// styles
#define FINALIZE                                                                   \
    do                                                                             \
    {                                                                              \
      sqlite3_finalize(stmt);                                                      \
      sqlite3_finalize(insert_stmt);                                               \
      sqlite3_finalize(select_stmt);                                               \
      sqlite3_finalize(delete_stmt);                                               \
      sqlite3_finalize(update_name_stmt);                                          \
      sqlite3_finalize(select_new_stmt);                                           \
      sqlite3_finalize(copy_style_items_stmt);                                     \
      sqlite3_finalize(delete_style_items_stmt);                                   \
    } while(0)

    stmt = NULL;
    select_stmt = NULL;
    update_name_stmt = NULL;
    insert_stmt = NULL;
    delete_stmt = NULL;
    sqlite3_stmt *select_new_stmt = NULL, *copy_style_items_stmt = NULL, *delete_style_items_stmt = NULL;
    // clang-format off
    TRY_PREPARE(stmt, "SELECT id, name FROM main.styles", "can't prepare style selection from database");
    TRY_PREPARE(select_stmt, "SELECT rowid FROM data.styles WHERE name = ?1 LIMIT 1",
                "can't prepare style item selection from database");
    TRY_PREPARE(update_name_stmt, "UPDATE main.styles SET name = ?1 WHERE id = ?2",
                "can't prepare style name update");
    TRY_PREPARE(insert_stmt, "INSERT INTO data.styles (id, name, description) "
                             "SELECT (SELECT COALESCE(MAX(id),0)+1 FROM data.styles), name, description "
                             "FROM main.styles where id = ?1",
                "can't prepare style insertion for database");
    TRY_PREPARE(delete_stmt, "DELETE FROM main.styles WHERE id = ?1",
                "can't prepare style deletion for database");
    TRY_PREPARE(select_new_stmt, "SELECT id FROM data.styles WHERE rowid = ?1",
                "can't prepare style selection from data database");
    TRY_PREPARE(copy_style_items_stmt, "INSERT INTO data.style_items "
                                       "(styleid, num, module, operation, op_params, enabled, blendop_params, "
                                       "blendop_version, multi_priority, multi_name) "
                                       "SELECT ?1, num, module, operation, op_params, enabled, blendop_params, "
                                       "blendop_version, multi_priority, multi_name FROM main.style_items "
                                       "WHERE styleid = ?2",
                "can't prepare style item copy into data database");
    TRY_PREPARE(delete_style_items_stmt, "DELETE FROM main.style_items WHERE styleid = ?1",
                "can't prepare style item deletion for database");
    // clang-format on
    while(sqlite3_step(stmt) == SQLITE_ROW)
    {
      const int id = sqlite3_column_int(stmt, 0);
      const char *name = (const char *)sqlite3_column_text(stmt, 1);

      // find a unique name of the style for data.styles
      sqlite3_bind_text(select_stmt, 1, name, -1, SQLITE_TRANSIENT);
      if(sqlite3_step(select_stmt) == SQLITE_ROW)
      {
        // we need to append a version
        int style_version = 0;
        char *new_name = NULL;
        do
        {
          style_version++;
          g_free(new_name);
          new_name = g_strdup_printf("%s #%d", name, style_version);
          sqlite3_reset(select_stmt);
          sqlite3_clear_bindings(select_stmt);
          sqlite3_bind_text(select_stmt, 1, new_name, -1, SQLITE_TRANSIENT);
        } while(sqlite3_step(select_stmt) == SQLITE_ROW);

        // update the name in the old place
        sqlite3_bind_text(update_name_stmt, 1, new_name, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(update_name_stmt, 2, id);
        TRY_STEP(update_name_stmt, SQLITE_DONE, "can't update name of style in database");
        sqlite3_reset(update_name_stmt);
        sqlite3_clear_bindings(update_name_stmt);
        g_free(new_name);
      }

      // move the style to data.styles and get the rowid
      sqlite3_bind_int(insert_stmt, 1, id);
      TRY_STEP(insert_stmt, SQLITE_DONE, "can't insert style into data database");
      sqlite3_int64 last_rowid = sqlite3_last_insert_rowid(db->handle);

      // delete style from styles
      sqlite3_bind_int(delete_stmt, 1, id);
      TRY_STEP(delete_stmt, SQLITE_DONE, "can't delete style from database");

      sqlite3_bind_int(select_new_stmt, 1, last_rowid);
      TRY_STEP(select_new_stmt, SQLITE_ROW, "can't select new style from data database");
      const int new_id = sqlite3_column_int(select_new_stmt, 0);

      // now that we have the style over in data.styles and the new id we can just copy over all style items
      sqlite3_bind_int(copy_style_items_stmt, 1, new_id);
      sqlite3_bind_int(copy_style_items_stmt, 2, id);
      TRY_STEP(copy_style_items_stmt, SQLITE_DONE, "can't copy style items into data database");

      // delete the style items from the old table
      sqlite3_bind_int(delete_style_items_stmt, 1, id);
      TRY_STEP(delete_style_items_stmt, SQLITE_DONE, "can't delete style items from database");

      // cleanup for the next round
      sqlite3_reset(insert_stmt);
      sqlite3_clear_bindings(insert_stmt);
      sqlite3_reset(select_stmt);
      sqlite3_clear_bindings(select_stmt);
      sqlite3_reset(delete_stmt);
      sqlite3_clear_bindings(delete_stmt);
      sqlite3_reset(select_new_stmt);
      sqlite3_clear_bindings(select_new_stmt);
      sqlite3_reset(copy_style_items_stmt);
      sqlite3_clear_bindings(copy_style_items_stmt);
      sqlite3_reset(delete_style_items_stmt);
      sqlite3_clear_bindings(delete_style_items_stmt);
    }
    FINALIZE;
#undef FINALIZE

    ////////////// tags
#define FINALIZE
    // clang-format off
    // tags
    TRY_EXEC("INSERT OR IGNORE INTO data.tags (name, icon, description, flags) "
             "SELECT name, icon, description, flags FROM main.tags",
             "can't prepare insertion of used tags into data database");

    // tagged images
    // we need a temp table to update tagged_images due to its primary key
    TRY_EXEC("CREATE TEMPORARY TABLE tagged_images_tmp (imgid INTEGER, tagid INTEGER)",
             "can't create temporary table for updating `tagged_images'");

    TRY_EXEC("INSERT INTO tagged_images_tmp (imgid, tagid) "
             "SELECT imgid, (SELECT t2.id FROM main.tags t1, data.tags t2 USING (name) WHERE t1.id = tagid) "
             "FROM main.tagged_images",
             "can't insert into `tagged_images_tmp'");

    TRY_EXEC("DELETE FROM main.tagged_images", "can't delete tagged images in database");

    TRY_EXEC("INSERT OR IGNORE INTO main.tagged_images (imgid, tagid) SELECT imgid, tagid FROM tagged_images_tmp",
             "can't copy updated values back to `tagged_images'");

    TRY_EXEC("DROP TABLE tagged_images_tmp", "can't drop table `tagged_images_tmp' from database");

    ////////////// cleanup - drop the indexes and tags
    TRY_EXEC("DROP INDEX IF EXISTS main.presets_idx", "can't drop index `presets_idx' from database");
    TRY_EXEC("DROP TABLE main.presets", "can't drop table `presets' from database");
    TRY_EXEC("DROP TABLE main.style_items", "can't drop table `style_items' from database");
    TRY_EXEC("DROP TABLE main.styles", "can't drop table `styles' from database");
    TRY_EXEC("DROP TABLE main.tags", "can't drop table `tags' from database");
    // clang-format on
    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
    new_version = 13;
  } else if(version == 13)
  {
    // 12 -> 13 bring back the used tag names to library.db so people can use it independently of data.db
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);
    // clang-format off
    TRY_EXEC("CREATE TABLE main.used_tags (id INTEGER, name VARCHAR NOT NULL)",
             "can't create `used_tags` table");

    TRY_EXEC("CREATE INDEX main.used_tags_idx ON used_tags (id, name)",
             "can't create index on table `used_tags' in database");

    TRY_EXEC("INSERT INTO main.used_tags (id, name) SELECT t.id, t.name FROM data.tags AS t, main.tagged_images "
             "AS i ON t.id = i.tagid GROUP BY t.id",
             "can't insert used tags into `used_tags` table in database");
    // clang-format on
    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
    new_version = 14;
  }
  else if(version == 14)
  {
    // 13 -> fix the index on used_tags to be a UNIQUE index :-/
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);
    // clang-format off
    TRY_EXEC("DELETE FROM main.used_tags WHERE rowid NOT IN (SELECT rowid FROM used_tags GROUP BY id)",
             "can't delete duplicated entries from `used_tags' in database");

    TRY_EXEC("DROP INDEX main.used_tags_idx", "can't drop index `used_tags_idx' from database");

    TRY_EXEC("CREATE UNIQUE INDEX main.used_tags_idx ON used_tags (id, name)",
             "can't create index `used_tags_idx' in database");

    TRY_EXEC("DELETE FROM main.tagged_images WHERE tagid IS NULL",
             "can't delete NULL entries from `tagged_images' in database");

    TRY_EXEC("DELETE FROM main.used_tags WHERE id NOT IN (SELECT DISTINCT tagid FROM main.tagged_images)",
             "can't delete unused tags from `used_tags' in database");
    // clang-format on
    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
    new_version = 15;
  }
  else if(version == 15)
  {
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);
    ////////////////////////////// custom image order
    // clang-format off
    TRY_EXEC("ALTER TABLE main.images ADD COLUMN position INTEGER",
             "can't add `position' column to images table in database");
    TRY_EXEC("CREATE INDEX main.image_position_index ON images (position)",
             "can't create index for custom image order table");

    // Set the initial image sequence. The image id - the sequence images were imported -
    // defines the initial order of images.
    //
    // An int64 is used for the position index. The upper 31 bits define the initial order.
    // The lower 32bit provide space to reorder images.
    //
    // see: dt_collection_move_before()
    //
    TRY_EXEC("UPDATE main.images SET position = id << 32",
             "can't update positions custom image order table");
    // clang-format on
    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
    new_version = 16;
  }
  else if(version == 16)
  {
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);
    ////////////////////////////// final image aspect ratio
    // clang-format off
    TRY_EXEC("ALTER TABLE main.images ADD COLUMN aspect_ratio REAL",
             "can't add `aspect_ratio' column to images table in database");
    TRY_EXEC("UPDATE main.images SET aspect_ratio = 0.0",
             "can't update aspect_ratio in database");
    // clang-format on
    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
    new_version = 17;
  }
  else if(version == 17)
  {
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);

    ////////////////////////////// masks history
    // clang-format off
    TRY_EXEC("CREATE TABLE main.masks_history (imgid INTEGER, num INTEGER, formid INTEGER, form INTEGER, name VARCHAR(256), "
             "version INTEGER, points BLOB, points_count INTEGER, source BLOB)",
             "can't create `masks_history` table");

    TRY_EXEC("CREATE INDEX main.masks_history_imgid_index ON masks_history (imgid)",
             "can't create index `masks_history_imgid_index' in database");

    // to speed up the mask look-up, and makes the following UPDATE instantaneous whereas it could takes hours
    TRY_EXEC("CREATE INDEX main.mask_imgid_index ON mask (imgid);",
             "can't create index `mask_imgid_index' in database");

    // create a mask manager entry on history for all images containing all forms
    // make room for mask manager history entry
    TRY_EXEC("UPDATE main.history SET num=num+1 WHERE imgid IN (SELECT imgid FROM main.mask WHERE main.mask.imgid=main.history.imgid)",
             "can't update `num' with num+1");

    // update history end
    TRY_EXEC("UPDATE main.images SET history_end = history_end+1 WHERE id IN (SELECT imgid FROM main.mask WHERE main.mask.imgid=main.images.id)",
             "can't update `history_end' with history_end+1");

    // copy all masks into history
    TRY_EXEC("INSERT INTO main.masks_history (imgid, num, formid, form, name, version, points, points_count, source) SELECT "
             "imgid, 0, formid, form, name, version, points, points_count, source FROM main.mask",
             "can't insert into masks_history");

    // create a mask manager entry for each image that has masks
    TRY_EXEC("INSERT INTO main.history (imgid, num, operation, op_params, module, enabled, "
             "blendop_params, blendop_version, multi_priority, multi_name) "
             "SELECT DISTINCT imgid, 0, 'mask_manager', NULL, 1, 0, NULL, 0, 0, '' FROM main.mask "
             "GROUP BY imgid",
             "can't insert mask manager into history");

    TRY_EXEC("DROP TABLE main.mask", "[init] can't drop table `mask' from database");

    ////////////////////////////// custom iop order
    GList *prior_v1 = dt_ioppr_get_iop_order_list_version(DT_IOP_ORDER_LEGACY);

    TRY_EXEC("ALTER TABLE main.images ADD COLUMN iop_order_version INTEGER",
             "can't add `iop_order_version' column to images table in database");

    TRY_EXEC("UPDATE main.images SET iop_order_version = 0",
             "can't update iop_order_version in database");

    TRY_EXEC("UPDATE main.images SET iop_order_version = 1 WHERE "
             "EXISTS(SELECT * FROM main.history WHERE main.history.imgid = main.images.id)",
             "can't update iop_order_version in database");

    TRY_EXEC("ALTER TABLE main.history ADD COLUMN iop_order REAL",
             "can't add `iop_order' column to history table in database");

    // create a temp table with the previous priorities
    TRY_EXEC("CREATE TEMPORARY TABLE iop_order_tmp (iop_order REAL, operation VARCHAR(256))",
             "can't create temporary table for updating `main.history'");
    // clang-format on
    // fill temp table with all operations up to this release
    // it will be used to create the pipe and update the iop_order on history
    for(GList *priorities = prior_v1; priorities; priorities = g_list_next(priorities))
    {
      dt_iop_order_entry_t *prior = priorities->data;

      sqlite3_prepare_v2(
          db->handle,
          "INSERT INTO iop_order_tmp (iop_order, operation) VALUES (?1, ?2)",
          -1, &stmt, NULL);
      sqlite3_bind_double(stmt, 1, prior->o.iop_order_f);
      sqlite3_bind_text(stmt, 2, prior->operation, -1, SQLITE_TRANSIENT);
      TRY_STEP(stmt, SQLITE_DONE, "can't insert default value in iop_order_tmp");
      sqlite3_finalize(stmt);
    }
    g_list_free_full(prior_v1, free);

    // create the order of the pipe
    // iop_order is by default the module priority
    // if there's multi-instances we add the multi_priority
    // multi_priority is in reverse order in this version,
    // so we assume that is always less than 1000 and reverse it
    // it is possible that multi_priority = 0 don't appear in history
    // so just in case 1 / 1000 to every instance
    // clang-format off
    TRY_EXEC("UPDATE main.history SET iop_order = ((("
        "SELECT MAX(multi_priority) FROM main.history hist1 WHERE hist1.imgid = main.history.imgid AND hist1.operation = main.history.operation "
             ") + 1. - multi_priority) / 1000.) + "
             "IFNULL((SELECT iop_order FROM iop_order_tmp WHERE iop_order_tmp.operation = "
             "main.history.operation), -999999.) ",
             "can't update iop_order in history table");

    // check if there's any entry in history that was not updated
    sqlite3_stmt *sel_stmt;
    TRY_PREPARE(sel_stmt, "SELECT DISTINCT operation FROM main.history WHERE iop_order <= 0 OR iop_order IS NULL",
                "can't prepare selecting history iop_order");
    // clang-format on
    while(sqlite3_step(sel_stmt) == SQLITE_ROW)
    {
      const char *op_name = (const char *)sqlite3_column_text(sel_stmt, 0);
      dt_print(DT_DEBUG_ALWAYS, "[init] operation %s with no iop_order while upgrading database", op_name);
    }
    sqlite3_finalize(sel_stmt);
    // clang-format off
    TRY_EXEC("DROP TABLE iop_order_tmp", "can't drop table `iop_order_tmp' from database");
    // clang-format on
    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
    new_version = 18;
  }
  // maybe in the future, see commented out code elsewhere
  //   else if(version == XXX)
  //   {
  //     sqlite3_exec(db->handle, "ALTER TABLE film_rolls ADD COLUMN external_drive VARCHAR(1024)", NULL,
  //     NULL, NULL);
  //   }
  else if(version == 18)
  {
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);
    // clang-format off
    TRY_EXEC("UPDATE images SET orientation=-2 WHERE orientation=1;",
             "can't update images orientation 1 from database");

    TRY_EXEC("UPDATE images SET orientation=1 WHERE orientation=2;",
             "can't update images orientation 2 from database");

    TRY_EXEC("UPDATE images SET orientation=-6 WHERE orientation=5;",
             "can't update images orientation 5 from database");

    TRY_EXEC("UPDATE images SET orientation=5 WHERE orientation=6;",
             "can't update images orientation 6 from database");

    TRY_EXEC("UPDATE images SET orientation=2 WHERE orientation=-2;",
             "can't update images orientation -1 from database");

    TRY_EXEC("UPDATE images SET orientation=6 WHERE orientation=-6;",
             "can't update images orientation -6 from database");
    // clang-format on
    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
    new_version = 19;
  }
  else if(version == 19)
  {
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);
    // clang-format off
    // create a temp table to invert all multi_priority
    TRY_EXEC("CREATE TEMPORARY TABLE m_prio (id INTEGER, operation VARCHAR(256), prio INTEGER)",
             "can't create temporary table for updating `history and style_items'");

    TRY_EXEC("CREATE INDEX m_prio_id_index ON m_prio (id)",
             "can't create temporary index for updating `history and style_items'");
    TRY_EXEC("CREATE INDEX m_prio_op_index ON m_prio (operation)",
             "can't create temporary index for updating `history and style_items'");

    TRY_EXEC("INSERT INTO m_prio SELECT imgid, operation, MAX(multi_priority)"
             " FROM main.history GROUP BY imgid, operation",
             "can't populate m_prio");

    TRY_EXEC("UPDATE main.history SET multi_priority = "
             "(SELECT prio FROM m_prio "
             " WHERE main.history.operation = operation AND main.history.imgid = id) - main.history.multi_priority",
             "can't update multi_priority for history");

    TRY_EXEC("DROP TABLE m_prio", "[init] can't drop table `m_prio' from database");
    // clang-format on
    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
    new_version = 20;
  }
  else if(version == 20)
  {
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);
    // clang-format off
    TRY_EXEC("DROP INDEX IF EXISTS main.used_tags_idx", "can't drop index `used_tags_idx' from database");
    TRY_EXEC("DROP TABLE used_tags", "can't delete table used_tags");
    // clang-format on
    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);

    new_version = 21;
  }
  else if(version == 21)
  {
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);
    // create a temp table to invert all multi_priority
    // clang-format off
    TRY_EXEC("CREATE TABLE module_order (imgid INTEGER PRIMARY KEY, version INTEGER, iop_list VARCHAR)",
             "can't create module_order table'");

    // for all images:
    sqlite3_stmt *mig_stmt;
    TRY_PREPARE(mig_stmt, "SELECT imgid, operation, multi_priority, iop_order, mi.iop_order_version"
                          " FROM main.history AS hi, main.images AS mi"
                          " WHERE hi.imgid = mi.id"
                          " GROUP BY imgid, operation, multi_priority"
                          " ORDER BY imgid, iop_order",
                "can't prepare selecting history for iop_order migration (v21)");
    // clang-format on
    GList *item_list = NULL;
    dt_imgid_t current_imgid = NO_IMGID;
    int current_order_version = -1;

    gboolean has_row = (sqlite3_step(mig_stmt) == SQLITE_ROW);

    while(has_row)
    {
      const dt_imgid_t imgid = sqlite3_column_int(mig_stmt, 0);
      char operation[20] = { 0 };
      g_strlcpy(operation, (const char *)sqlite3_column_text(mig_stmt, 1), sizeof(operation));
      const int multi_priority = sqlite3_column_int(mig_stmt, 2);
      const double iop_order = sqlite3_column_double(mig_stmt, 3);
      const int iop_order_version = sqlite3_column_int(mig_stmt, 4);

      has_row = (sqlite3_step(mig_stmt) == SQLITE_ROW);

      // a new image, let's initialize the iop_order_version
      if(imgid != current_imgid || !has_row)
      {
        // new image, let's handle it
        if(item_list != NULL)
        {
          // we keep legacy, everything else is migrated to v3.0
          const dt_iop_order_t new_order_version = current_order_version == 2 ? DT_IOP_ORDER_LEGACY : DT_IOP_ORDER_V30;

          GList *iop_order_list = dt_ioppr_get_iop_order_list_version(new_order_version);

          // merge entries into iop_order_list

          // first remove all item_list iop from the iop_order_list

          GList *e = item_list;
          GList *n = NULL;
          dt_iop_order_entry_t *n_entry = NULL;

          while(e)
          {
            dt_iop_order_entry_t *e_entry = e->data;

            GList *s = iop_order_list;
            while(s && strcmp(((dt_iop_order_entry_t *)s->data)->operation, e_entry->operation))
            {
              s = g_list_next(s);
            }
            if(s)
            {
              iop_order_list = g_list_delete_link(iop_order_list, s);
            }

            // skip all multiple instances
            n = e;
            do
            {
              n = g_list_next(n);
              if(!n) break;
              n_entry = (dt_iop_order_entry_t *)n->data;
            } while(!strcmp(n_entry->operation, e_entry->operation));
            e = n;
          }

          // then add all item_list into iop_order_list

          for(e = item_list; e; e = g_list_next(e))
          {
            dt_iop_order_entry_t *e_entry = e->data;
            iop_order_list = g_list_prepend(iop_order_list, e_entry);
          }

          // and finally reorder the full list based on the iop-order

          iop_order_list = g_list_sort(iop_order_list, dt_sort_iop_list_by_order_f);

          const dt_iop_order_t kind = dt_ioppr_get_iop_order_list_kind(iop_order_list);

          // check if we have some multi-instances

          gboolean has_multiple_instances = FALSE;
          GList *l = iop_order_list;

          while(l)
          {
            GList *next = g_list_next(l);
            if(next
               && (strcmp(((dt_iop_order_entry_t *)(l->data))->operation,
                          ((dt_iop_order_entry_t *)(next->data))->operation) == 0))
            {
              has_multiple_instances = TRUE;
              break;
            }
            l = next;
          }

          // write iop_order_list and/or version into module_order

          sqlite3_stmt *ins_stmt = NULL;
          if(kind == DT_IOP_ORDER_CUSTOM || has_multiple_instances)
          {
            char *iop_list_txt = dt_ioppr_serialize_text_iop_order_list(iop_order_list);

            sqlite3_prepare_v2(db->handle,
                               "INSERT INTO module_order VALUES (?1, ?2, ?3)", -1,
                               &ins_stmt, NULL);
            sqlite3_bind_int(ins_stmt, 1, current_imgid);
            sqlite3_bind_int(ins_stmt, 2, kind);
            sqlite3_bind_text(ins_stmt, 3, iop_list_txt, -1, SQLITE_TRANSIENT);
            TRY_STEP(ins_stmt, SQLITE_DONE, "can't insert into module_order (custom order)");
            sqlite3_finalize(ins_stmt);

            g_free(iop_list_txt);
          }
          else
          {
            sqlite3_prepare_v2(db->handle,
                               "INSERT INTO module_order VALUES (?1, ?2, NULL)", -1,
                               &ins_stmt, NULL);
            sqlite3_bind_int(ins_stmt, 1, current_imgid);
            sqlite3_bind_int(ins_stmt, 2, kind);
            TRY_STEP(ins_stmt, SQLITE_DONE, "can't insert into module_order (standard order)");
            sqlite3_finalize(ins_stmt);
          }

          g_list_free(item_list);
          g_list_free_full(iop_order_list, free);

          item_list = NULL;
        }

        current_imgid = imgid;
        current_order_version = iop_order_version;
      }

      dt_iop_order_entry_t *item = malloc(sizeof(dt_iop_order_entry_t));
      memcpy(item->operation, operation, sizeof(item->operation));
      item->instance = multi_priority;
      item->o.iop_order_f = iop_order; // used to order the enties only
      item_list = g_list_append(item_list, item);
    }
    sqlite3_finalize(mig_stmt);

    // remove iop_order from history table
    // clang-format off
    TRY_EXEC("CREATE TABLE h (imgid INTEGER, num INTEGER, module INTEGER, "
             "operation VARCHAR(256), op_params BLOB, enabled INTEGER, "
             "blendop_params BLOB, blendop_version INTEGER, multi_priority INTEGER, multi_name VARCHAR(256))",
             "can't create module_order table");
    TRY_EXEC("CREATE INDEX h_imgid_index ON h (imgid)",
             "[init] can't create index h_imgid_index");
    TRY_EXEC("INSERT INTO h SELECT imgid, num, module, operation, op_params, enabled, "
             "blendop_params, blendop_version, multi_priority, multi_name FROM main.history",
             "can't create module_order table");
    TRY_EXEC("DROP TABLE history",
             "can't drop table history");
    TRY_EXEC("ALTER TABLE h RENAME TO history",
             "can't rename h to history");
    TRY_EXEC("DROP INDEX h_imgid_index",
             "can't drop index h_imgid_index");
    TRY_EXEC("CREATE INDEX main.history_imgid_index ON history (imgid)",
             "can't create index images_imgid_index");

    // remove iop_order_version from images

    TRY_EXEC("CREATE TABLE i (id INTEGER PRIMARY KEY AUTOINCREMENT, group_id INTEGER, film_id INTEGER, "
             "width INTEGER, height INTEGER, filename VARCHAR, maker VARCHAR, model VARCHAR, "
             "lens VARCHAR, exposure REAL, aperture REAL, iso REAL, focal_length REAL, "
             "focus_distance REAL, datetime_taken CHAR(20), flags INTEGER, "
             "output_width INTEGER, output_height INTEGER, crop REAL, "
             "raw_parameters INTEGER, raw_denoise_threshold REAL, "
             "raw_auto_bright_threshold REAL, raw_black INTEGER, raw_maximum INTEGER, "
             "caption VARCHAR, description VARCHAR, license VARCHAR, sha1sum CHAR(40), "
             "orientation INTEGER, histogram BLOB, lightmap BLOB, longitude REAL, "
             "latitude REAL, altitude REAL, color_matrix BLOB, colorspace INTEGER, version INTEGER, "
             "max_version INTEGER, write_timestamp INTEGER, history_end INTEGER, position INTEGER, aspect_ratio REAL)",
             "[init] can't create table i");
    TRY_EXEC("INSERT INTO i SELECT id, group_id, film_id, width, height, filename, maker, model,"
             " lens, exposure, aperture, iso, focal_length, focus_distance, datetime_taken, flags,"
             " output_width, output_height, crop, raw_parameters, raw_denoise_threshold,"
             " raw_auto_bright_threshold, raw_black, raw_maximum, caption, description, license, sha1sum,"
             " orientation, histogram, lightmap, longitude, latitude, altitude, color_matrix, colorspace, version,"
             " max_version, write_timestamp, history_end, position, aspect_ratio "
             "FROM images",
             "can't populate table h");
    TRY_EXEC("DROP TABLE images",
             "can't drop table images");
    TRY_EXEC("ALTER TABLE i RENAME TO images",
             "can't rename i to images");
    // clang-format on
    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);

    new_version = 22;
  }
  else if(version == 22)
  {
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);
    // clang-format off
    TRY_EXEC("CREATE INDEX IF NOT EXISTS main.images_group_id_index ON images (group_id)",
             "can't create group_id index on image");
    TRY_EXEC("CREATE INDEX IF NOT EXISTS  main.images_film_id_index ON images (film_id)",
             "can't create film_id index on image");
    TRY_EXEC("CREATE INDEX IF NOT EXISTS main.images_filename_index ON images (filename)",
             "can't create filename index on image");
    TRY_EXEC("CREATE INDEX IF NOT EXISTS main.image_position_index ON images (position)",
             "can't create position index on image");

    TRY_EXEC("CREATE INDEX IF NOT EXISTS main.film_rolls_folder_index ON film_rolls (folder)",
             "can't create folder index on film_rolls");
    // clang-format on
    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);

    new_version = 23;
  }
  else if(version == 23)
  {
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);
    // clang-format off
    TRY_EXEC("CREATE TABLE main.history_hash (imgid INTEGER PRIMARY KEY, "
             "basic_hash BLOB, auto_hash BLOB, current_hash BLOB)",
             "can't create table history_hash");
    // clang-format on

    // use the former dt_image_altered() to initialise the history_hash table
    // insert an history_hash entry for all images which have an history
    // note that images without history don't get hash and are considered as basic
    sqlite3_stmt *h_stmt;
    const gboolean basecurve_auto_apply = dt_is_display_referred();
    // clang-format off
    char *query = g_strdup_printf(
                            "SELECT id, CASE WHEN imgid IS NULL THEN 0 ELSE 1 END as altered "
                            // first, images which are both in images and history (avoids history orphans)
                            "FROM (SELECT DISTINCT id FROM main.images JOIN main.history ON imgid = id) "
                            "LEFT JOIN (SELECT DISTINCT imgid FROM main.images JOIN main.history ON imgid = id "
                            "           WHERE num < history_end AND enabled = 1"
                            "             AND operation NOT IN ('flip', 'dither', 'highlights', 'rawprepare', "
                            "             'colorin', 'colorout', 'gamma', 'demosaic', 'temperature'%s)) "
                            "ON imgid = id",
                             basecurve_auto_apply ? ", 'basecurve'" : "");
    // clang-format on
    TRY_PREPARE(h_stmt, query,
                "can't prepare selecting history for history_hash migration");
    while(sqlite3_step(h_stmt) == SQLITE_ROW)
    {
      const dt_imgid_t imgid= sqlite3_column_int(h_stmt, 0);
      const int32_t altered = sqlite3_column_int(h_stmt, 1);

      guint8 *hash = NULL;
      GChecksum *checksum = g_checksum_new(G_CHECKSUM_MD5);
      gsize hash_len = 0;

      // get history
      sqlite3_stmt *h2_stmt;
      // clang-format off
      sqlite3_prepare_v2(db->handle,
                         "SELECT operation, op_params, blendop_params"
                         " FROM main.history"
                         " WHERE imgid = ?1 AND enabled = 1"
                         " ORDER BY num",
                         -1, &h2_stmt, NULL);
      // clang-format on
      sqlite3_bind_int(h2_stmt, 1, imgid);
      while(sqlite3_step(h2_stmt) == SQLITE_ROW)
      {
        // operation
        char *buf = (char *)sqlite3_column_text(h2_stmt, 0);
        if(buf) g_checksum_update(checksum, (const guchar *)buf, -1);
        // op_params
        buf = (char *)sqlite3_column_blob(h2_stmt, 1);
        int params_len = sqlite3_column_bytes(h2_stmt, 1);
        if(buf) g_checksum_update(checksum, (const guchar *)buf, params_len);
        // blendop_params
        buf = (char *)sqlite3_column_blob(h2_stmt, 2);
        params_len = sqlite3_column_bytes(h2_stmt, 2);
        if(buf) g_checksum_update(checksum, (const guchar *)buf, params_len);
      }
      sqlite3_finalize(h2_stmt);

      // get module order
      h2_stmt = NULL;
      // clang-format off
      sqlite3_prepare_v2(db->handle,
                         "SELECT version, iop_list"
                         " FROM main.module_order"
                         " WHERE imgid = ?1",
                         -1, &h2_stmt, NULL);
      // clang-format on
      sqlite3_bind_int(h2_stmt, 1, imgid);
      if(sqlite3_step(h2_stmt) == SQLITE_ROW)
      {
        const int version_h = sqlite3_column_int(h2_stmt, 0);
        g_checksum_update(checksum, (const guchar *)&version_h, sizeof(version_h));
        if(version_h == DT_IOP_ORDER_CUSTOM)
        {
          // iop_list
          const char *buf = (char *)sqlite3_column_text(h2_stmt, 1);
          if(buf) g_checksum_update(checksum, (const guchar *)buf, -1);
        }
      }
      sqlite3_finalize(h2_stmt);

      const gsize checksum_len = g_checksum_type_get_length(G_CHECKSUM_MD5);
      hash = g_malloc(checksum_len);
      hash_len = checksum_len;
      g_checksum_get_digest(checksum, hash, &hash_len);

      g_checksum_free(checksum);
      // insert the hash for that image
      h2_stmt = NULL;
      // clang-format off
      sqlite3_prepare_v2(db->handle,
                         "INSERT INTO main.history_hash"
                         " VALUES (?1, ?2, NULL, ?3)",
                         -1, &h2_stmt, NULL);
      // clang-format on
      sqlite3_bind_int(h2_stmt, 1, imgid);
      sqlite3_bind_blob(h2_stmt, 2, altered ? NULL : hash, altered ? 0 : hash_len, SQLITE_TRANSIENT);
      sqlite3_bind_blob(h2_stmt, 3, hash, hash_len, SQLITE_TRANSIENT);
      TRY_STEP(h2_stmt, SQLITE_DONE, "can't insert into history_hash");
      sqlite3_finalize(h2_stmt);
      g_free(hash);
    }
    sqlite3_finalize(h_stmt);
    g_free(query);

    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);

    new_version = 24;
  }
  else if(version == 24)
  {
    // clang-format off
    TRY_EXEC("ALTER TABLE main.history_hash ADD COLUMN mipmap_hash BLOB",
             "can't add `mipmap_hash' column to history_hash table in database");
    // clang-format on

    new_version = 25;
  }
  else if(version == 25)
  {
    // clang-format of
    TRY_EXEC("ALTER TABLE main.images ADD COLUMN exposure_bias REAL",
             "can't add `exposure_bias' column to images table in database");
    // clang-format on

    new_version = 26;
  }
  else if(version == 26)
  {
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);
    // clang-format off
    TRY_EXEC("CREATE TABLE main.new_film_rolls "
             "(id INTEGER PRIMARY KEY, "
             "access_timestamp INTEGER, "
             "folder VARCHAR(1024) NOT NULL)",
             "can't create new_film_rolls table");

    TRY_EXEC("INSERT INTO main.new_film_rolls"
             "(id, access_timestamp, folder) "
             "SELECT id, "
             "strftime('%s', replace(substr(datetime_accessed, 1, 10), ':', '-') || substr(datetime_accessed, 11), 'utc'), "
             "folder "
             "FROM film_rolls "
             "WHERE folder IS NOT NULL",
             "can't populate new_film_rolls table from film_rolls");

    TRY_EXEC("DROP TABLE film_rolls",
             "can't delete table film_rolls");

    TRY_EXEC("ALTER TABLE main.new_film_rolls RENAME TO film_rolls",
             "can't rename table new_film_rolls to film_rolls");

    TRY_EXEC("CREATE INDEX main.film_rolls_folder_index ON film_rolls (folder)",
             "can't create index `film_rolls_folder_index' on table `film_rolls'");
    // clang-format on
    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
    new_version = 27;
  }
  else if(version == 27)
  {
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);
    // clang-format off
    TRY_EXEC("ALTER TABLE main.images ADD COLUMN import_timestamp INTEGER DEFAULT -1",
             "can't add `import_timestamp' column to images table in database");
    TRY_EXEC("ALTER TABLE main.images ADD COLUMN change_timestamp INTEGER DEFAULT -1",
             "can't add `change_timestamp' column to images table in database");
    TRY_EXEC("ALTER TABLE main.images ADD COLUMN export_timestamp INTEGER DEFAULT -1",
             "can't add `export_timestamp' column to images table in database");
    TRY_EXEC("ALTER TABLE main.images ADD COLUMN print_timestamp INTEGER DEFAULT -1",
             "can't add `print_timestamp' column to images table in database");

    TRY_EXEC("UPDATE main.images SET import_timestamp = (SELECT access_timestamp "
               "FROM main.film_rolls WHERE film_rolls.id = images.film_id)",
             "can't populate import_timestamp column from film_rolls.access_timestamp.");

    TRY_EXEC("UPDATE main.images SET change_timestamp = images.write_timestamp "
               "WHERE images.write_timestamp IS NOT NULL "
                 "AND images.id = (SELECT imgid FROM tagged_images "
                   "JOIN data.tags ON tags.id = tagged_images.tagid "
                     "WHERE data.tags.name = 'darktable|changed')",
             "can't populate change_timestamp column from images.write_timestamp.");
    // clang-format on
    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
    new_version = 28;
  }
  else if(version == 28)
  {
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);
    // clang-format off
    // clear flag DT_IMAGE_REJECTED (was not used)
    TRY_EXEC("UPDATE main.images SET flags = (flags & ~8)",
             "can't clear rejected flags");

    // add DT_IMAGE_REJECTED and clear rating for all images being rejected
    TRY_EXEC("UPDATE main.images SET flags = (flags | 8) & ~7 WHERE (flags & 7) = 6",
             "can't set rejected flags");
    // clang-format on
    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
    new_version = 29;
  }
  else if(version == 29)
  {
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);
    // clang-format off
    // add position in tagged_images table
    TRY_EXEC("ALTER TABLE main.tagged_images ADD COLUMN position INTEGER",
             "can't add `position' column to tagged_images table in database");

    TRY_EXEC("CREATE INDEX IF NOT EXISTS main.tagged_images_imgid_index ON tagged_images (imgid)",
             "can't create image index on tagged_images");
    TRY_EXEC("CREATE INDEX IF NOT EXISTS main.tagged_images_position_index ON tagged_images (position)",
             "can't create position index on tagged_images");
    TRY_EXEC("UPDATE main.tagged_images SET position = (tagid + imgid) << 32",
             "can't populate position on tagged_images");

    // remove caption and description fields from images table

    TRY_EXEC("CREATE TABLE main.i (id INTEGER PRIMARY KEY AUTOINCREMENT, group_id INTEGER, film_id INTEGER, "
             "width INTEGER, height INTEGER, filename VARCHAR, maker VARCHAR, model VARCHAR, "
             "lens VARCHAR, exposure REAL, aperture REAL, iso REAL, focal_length REAL, "
             "focus_distance REAL, datetime_taken CHAR(20), flags INTEGER, "
             "output_width INTEGER, output_height INTEGER, crop REAL, "
             "raw_parameters INTEGER, raw_denoise_threshold REAL, "
             "raw_auto_bright_threshold REAL, raw_black INTEGER, raw_maximum INTEGER, "
             "license VARCHAR, sha1sum CHAR(40), "
             "orientation INTEGER, histogram BLOB, lightmap BLOB, longitude REAL, "
             "latitude REAL, altitude REAL, color_matrix BLOB, colorspace INTEGER, version INTEGER, "
             "max_version INTEGER, write_timestamp INTEGER, history_end INTEGER, position INTEGER, "
             "aspect_ratio REAL, exposure_bias REAL, "
             "import_timestamp INTEGER DEFAULT -1, change_timestamp INTEGER DEFAULT -1, "
             "export_timestamp INTEGER DEFAULT -1, print_timestamp INTEGER DEFAULT -1)",
             "can't create table i");

    TRY_EXEC("INSERT INTO main.i SELECT id, group_id, film_id, width, height, filename, maker, model,"
             " lens, exposure, aperture, iso, focal_length, focus_distance, datetime_taken, flags,"
             " output_width, output_height, crop, raw_parameters, raw_denoise_threshold,"
             " raw_auto_bright_threshold, raw_black, raw_maximum, license, sha1sum,"
             " orientation, histogram, lightmap, longitude, latitude, altitude, color_matrix, colorspace, version,"
             " max_version, write_timestamp, history_end, position, aspect_ratio, exposure_bias,"
             " import_timestamp, change_timestamp, export_timestamp, print_timestamp "
             "FROM main.images",
             "can't populate table i");
    TRY_EXEC("DROP TABLE main.images",
             "can't drop table images");
    TRY_EXEC("ALTER TABLE main.i RENAME TO images",
             "can't rename i to images");

    TRY_EXEC("CREATE INDEX main.images_group_id_index ON images (group_id)",
          "can't create group_id index on images table");
    TRY_EXEC("CREATE INDEX main.images_film_id_index ON images (film_id)",
          "can't create film_id index on images table");
    TRY_EXEC("CREATE INDEX main.images_filename_index ON images (filename)",
          "can't create filename index on images table");
    TRY_EXEC("CREATE INDEX main.image_position_index ON images (position)",
          "can't create position index on images table");
    // clang-format on
    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
    new_version = 30;
  }
  else if(version == 30)
  {
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);
    // clang-format off
    // add second columns to speed up sorting
    TRY_EXEC("DROP INDEX IF EXISTS `history_imgid_index`",
        "can't drop history_imgid_index");
    TRY_EXEC("CREATE INDEX `history_imgid_index` ON `history` ( `imgid`, `operation` )",
        "can't recreate history_imgid_index");

    TRY_EXEC("DROP INDEX IF EXISTS `images_filename_index`",
        "can't drop images_filename_index");
    TRY_EXEC("CREATE INDEX `images_filename_index` ON `images` ( `filename`, `version` )",
        "can't recreate images_filename_index");

    TRY_EXEC("DROP INDEX IF EXISTS `images_film_id_index`",
        "[init] can't drop images_film_id_index");
    TRY_EXEC("CREATE INDEX `images_film_id_index` ON `images` ( `film_id`, `filename` )",
        "can't recreate images_film_id_index");

    TRY_EXEC("DROP INDEX IF EXISTS `images_group_id_index`",
        "can't drop images_group_id_index");
    TRY_EXEC("CREATE INDEX `images_group_id_index` ON `images` ( `group_id`, `id` )",
        "can't recreate images_group_id_index");

    TRY_EXEC("DROP INDEX IF EXISTS `masks_history_imgid_index`",
        "can't drop masks_history_imgid_index");
    TRY_EXEC("CREATE INDEX `masks_history_imgid_index` ON `masks_history` ( `imgid`, `num` )",
        "can't recreate masks_history_imgid_index");

    // map refinement: avoid full table scan
    TRY_EXEC("CREATE INDEX `images_latlong_index` ON `images` ( `latitude` DESC, `longitude` DESC )",
        "can't create images_latlong_index");
    // clang-format on
    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
    new_version = 31;
  }
  else if(version == 31)
  {
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);
    // clang-format off
    // remove duplicates
    TRY_EXEC("DELETE FROM main.meta_data WHERE rowid NOT IN (SELECT MIN(rowid) "
             "FROM main.meta_data GROUP BY id, key)",
             "can't remove duplicates from meta_data");

    // recreate the index with UNIQUE option
    TRY_EXEC("DROP INDEX IF EXISTS metadata_index",
             "can't drop metadata_index");
    TRY_EXEC("CREATE UNIQUE INDEX main.metadata_index ON meta_data (id, key)",
             "can't create metadata_index");
    // clang-format on
    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
    new_version = 32;
  }
  else if(version == 32)
  {
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);
    // clang-format off
    // add foreign keys for database consistency. ON UPDATE CASCADE since you never know
    // if a future version will change image_id
    // Unfortunately sqlite does not support adding foreign keys to existing tables
    // so we have to rename the existing tables, recreate them and copy back the old values
    // images first
    // needs to delete orphaned entries
    TRY_EXEC("ALTER TABLE `images` RENAME TO `images_old`",
        "can't rename images");

    TRY_EXEC("CREATE TABLE `images` (id INTEGER PRIMARY KEY AUTOINCREMENT, group_id INTEGER, film_id INTEGER, "
      "width INTEGER, height INTEGER, filename VARCHAR, maker VARCHAR, model VARCHAR, lens VARCHAR, "
      "exposure REAL, aperture REAL, iso REAL, focal_length REAL, focus_distance REAL, datetime_taken CHAR(20), "
      "flags INTEGER, output_width INTEGER, output_height INTEGER, crop REAL, "
      "raw_parameters INTEGER, raw_denoise_threshold REAL, raw_auto_bright_threshold REAL, "
      "raw_black INTEGER, raw_maximum INTEGER, license VARCHAR, sha1sum CHAR(40), "
      "orientation INTEGER, histogram BLOB, lightmap BLOB, longitude REAL, latitude REAL, altitude REAL, "
      "color_matrix BLOB, colorspace INTEGER, version INTEGER, max_version INTEGER, write_timestamp INTEGER, "
      "history_end INTEGER, position INTEGER, aspect_ratio REAL, exposure_bias REAL, "
      "import_timestamp INTEGER DEFAULT -1, change_timestamp INTEGER DEFAULT -1, export_timestamp INTEGER DEFAULT -1, print_timestamp INTEGER DEFAULT -1, "
      "FOREIGN KEY(film_id) REFERENCES film_rolls(id) ON DELETE CASCADE ON UPDATE CASCADE, "
      "FOREIGN KEY(group_id) REFERENCES images(id) ON DELETE RESTRICT ON UPDATE CASCADE)",
        "can't create new images table");

    // corner case: database inconsistency with images having invalid film id
    TRY_EXEC("DELETE FROM `images_old` WHERE film_id NOT IN (SELECT id FROM `film_rolls`)",
        "can't delete images with invalid film id");

    TRY_EXEC("UPDATE `images_old` SET group_id=id WHERE group_id NOT IN (SELECT id from `images_old`)",
        "can't fix invalid group ids");

    TRY_EXEC("INSERT INTO `images` SELECT * FROM `images_old`",
        "can't copy back from images_old");

    // pita: need to recreate index
    TRY_EXEC("DROP INDEX IF EXISTS `image_position_index`",
        "can't drop image_position_index");
    TRY_EXEC("CREATE INDEX `image_position_index` ON `images` (position)",
        "can't add image_position_index");

    // second columns
    TRY_EXEC("DROP INDEX IF EXISTS `images_filename_index`",
        "can't drop images_filename_index");
    TRY_EXEC("CREATE INDEX `images_filename_index` ON `images` ( `filename`, `version` )",
        "can't recreate images_filename_index");

    TRY_EXEC("DROP INDEX IF EXISTS `images_film_id_index`",
        "can't drop images_film_id_index");
    TRY_EXEC("CREATE INDEX `images_film_id_index` ON `images` ( `film_id`, `filename` )",
        "can't recreate images_film_id_index");

    TRY_EXEC("DROP INDEX IF EXISTS `images_group_id_index`",
        "can't drop images_group_id_index");
    TRY_EXEC("CREATE INDEX `images_group_id_index` ON `images` ( `group_id`, `id` )",
        "can't recreate images_group_id_index");

    TRY_EXEC("DROP INDEX IF EXISTS `images_latlong_index`",
        "can't drop images_latlong_index");
    TRY_EXEC("CREATE INDEX `images_latlong_index` ON `images` ( latitude DESC, longitude DESC )",
        "can't add images_latlong_index");

    TRY_EXEC("DROP TABLE `images_old`",
        "can't drop table images_old");

    // history
    TRY_EXEC("ALTER TABLE `history` RENAME TO `history_old`",
        "can't rename history");

    TRY_EXEC("CREATE TABLE `history` (imgid INTEGER, num INTEGER, module INTEGER, "
      "operation VARCHAR(256), op_params BLOB, enabled INTEGER, blendop_params BLOB, blendop_version INTEGER, "
      "multi_priority INTEGER, multi_name VARCHAR(256), "
      "FOREIGN KEY(imgid) REFERENCES images(id) ON DELETE CASCADE ON UPDATE CASCADE)",
        "can't create new history table");

    TRY_EXEC("DELETE FROM `history_old` WHERE imgid NOT IN (SELECT id FROM `images`)",
        "can't delete orphaned history elements");

    TRY_EXEC("INSERT INTO history SELECT * FROM history_old",
         "can't copy back from history_old");

    TRY_EXEC("DROP INDEX IF EXISTS `history_imgid_index`",
        "can't drop history_imgid_index");
    TRY_EXEC("CREATE INDEX `history_imgid_op_index` ON `history` ( `imgid`, `operation` )",
        "can't recreate history_imgid_index");
    TRY_EXEC("CREATE INDEX `history_imgid_num_index` ON `history` ( `imgid`, `num` DESC )",
        "can't recreate history_imgid_index");

    TRY_EXEC("DROP TABLE `history_old`",
        "can't drop table history_old");

    // history hash
    TRY_EXEC("ALTER TABLE `history_hash` RENAME TO `history_hash_old`",
         "can't rename history_hash");

    TRY_EXEC("CREATE TABLE `history_hash` (imgid INTEGER PRIMARY KEY, basic_hash BLOB, auto_hash BLOB, current_hash BLOB, "
      "mipmap_hash BLOB, FOREIGN KEY(imgid) REFERENCES images(id) ON DELETE CASCADE ON UPDATE CASCADE)",
        "can't create new history_hash table");

    TRY_EXEC("DELETE FROM `history_hash_old` WHERE imgid NOT IN (SELECT id FROM `images`)",
        "can't delete orphaned history_hash elements");

    TRY_EXEC("INSERT INTO `history_hash` SELECT * FROM `history_hash_old`",
         "can't copy back from history_hash_old");

    TRY_EXEC("DROP TABLE `history_hash_old`",
        "can't drop table history_hash_old");

    // tagged images
    TRY_EXEC("ALTER TABLE `tagged_images` RENAME TO `tagged_images_old`",
         "can't rename tagged_images");

    TRY_EXEC("CREATE TABLE `tagged_images` (imgid integer, tagid integer, position INTEGER, "
        "primary key(imgid, tagid), FOREIGN KEY(imgid) REFERENCES images(id) ON DELETE CASCADE ON UPDATE CASCADE)",
        "can't create new tagged_images table");

    TRY_EXEC("DELETE FROM `tagged_images_old` WHERE imgid NOT IN (SELECT id FROM `images`)",
        "can't delete orphaned tagged_images elements");

    TRY_EXEC("INSERT INTO `tagged_images` SELECT * FROM `tagged_images_old`",
         "can't copy back from tagged_images_old");

    // old indices
    TRY_EXEC("DROP INDEX IF EXISTS tagged_images_imgid_index",
        "can't drop tagged_images_imgid_index");
    TRY_EXEC("DROP INDEX IF EXISTS tagged_images_position_index",
        "can't drop tagged_images_position_index");
    TRY_EXEC("CREATE INDEX tagged_images_position_index ON tagged_images (position)",
        "can't add index tagged_images_position_index");
    TRY_EXEC("DROP INDEX IF EXISTS tagged_images_tagid_index",
        "can't drop tagged_images_tagid_index");
    TRY_EXEC("CREATE INDEX tagged_images_tagid_index ON tagged_images (tagid)",
        "can't add index tagged_images_tagid_index");

    TRY_EXEC("DROP TABLE `tagged_images_old`",
        "can't drop table tagged_images_old");

    // masks history
    TRY_EXEC("ALTER TABLE `masks_history` RENAME TO `masks_history_old`",
         "can't rename masks_history");

    TRY_EXEC("CREATE TABLE masks_history (imgid INTEGER, num INTEGER, formid INTEGER, form INTEGER, "
        "name VARCHAR(256), version INTEGER, points BLOB, points_count INTEGER, source BLOB, "
        "FOREIGN KEY(imgid) REFERENCES images(id) ON DELETE CASCADE ON UPDATE CASCADE)",
        "can't create new masks_history table");

    TRY_EXEC("DELETE FROM `masks_history_old` WHERE imgid NOT IN (SELECT id FROM `images`)",
        "can't delete orphaned masks_history elements");

    TRY_EXEC("INSERT INTO `masks_history` SELECT * FROM `masks_history_old`",
         "can't copy back from masks_history");

    TRY_EXEC("DROP INDEX IF EXISTS `masks_history_imgid_index`",
        "can't drop masks_history_imgid_index");
    TRY_EXEC("CREATE INDEX `masks_history_imgid_index` ON `masks_history` ( imgid, num )",
        "can't recreate masks_history_imgid_index");

    TRY_EXEC("DROP TABLE masks_history_old",
        "can't drop table masks_history_old");

    // color labels
    TRY_EXEC("ALTER TABLE `color_labels` RENAME TO `color_labels_old`",
         "can't rename color_labels");
    TRY_EXEC("CREATE TABLE `color_labels` (imgid INTEGER, color INTEGER, "
      "FOREIGN KEY(imgid) REFERENCES images(id) ON DELETE CASCADE ON UPDATE CASCADE)",
        "can't create new color_labels table");

    TRY_EXEC("DELETE FROM `color_labels_old` WHERE imgid NOT IN (SELECT id FROM `images`)",
        "can't delete orphaned color_labels elements");

    TRY_EXEC("INSERT INTO `color_labels` SELECT * FROM `color_labels_old`",
         "can't copy back from color_labels");

    TRY_EXEC("DROP TABLE color_labels_old",
        "can't drop table color_labels_old");

    TRY_EXEC("CREATE UNIQUE INDEX `color_labels_idx` ON `color_labels` (imgid, color)",
        "can't recreate color_labels_idx");

    // meta data
    TRY_EXEC("ALTER TABLE `meta_data` RENAME TO `meta_data_old`",
         "can't rename meta_data");
    TRY_EXEC("CREATE TABLE `meta_data` (id integer, key integer, value varchar, "
      "FOREIGN KEY(id) REFERENCES images(id) ON DELETE CASCADE ON UPDATE CASCADE)",
        "can't create new meta_data table");

    TRY_EXEC("DELETE FROM `meta_data_old` WHERE id NOT IN (SELECT id FROM `images`)",
      "can't delete orphaned meta_data elements");

    TRY_EXEC("INSERT INTO `meta_data` SELECT * FROM `meta_data_old`",
         "can't copy back from meta_data");

    TRY_EXEC("DROP TABLE meta_data_old",
        "can't drop table meta_data_old");

    TRY_EXEC("CREATE UNIQUE INDEX `metadata_index` ON `meta_data` (id, key, value)",
         "can't recreate metadata_index");

    // selected images
    TRY_EXEC("ALTER TABLE `selected_images` RENAME TO `selected_images_old`",
         "can't rename selected_images");
    TRY_EXEC("CREATE TABLE `selected_images` (imgid INTEGER PRIMARY KEY, "
      "FOREIGN KEY(imgid) REFERENCES images(id) ON DELETE CASCADE ON UPDATE CASCADE)",
        "can't create new selected_images table");

    TRY_EXEC("DELETE FROM `selected_images_old` WHERE imgid NOT IN (SELECT id FROM `images`)",
      "can't delete orphaned selected_images elements");

    TRY_EXEC("INSERT INTO `selected_images` SELECT * FROM `selected_images_old`",
         "can't copy back selected_images meta_data");

    TRY_EXEC("DROP TABLE selected_images_old",
        "can't drop table selected_images_old");

    // module order
    TRY_EXEC("ALTER TABLE `module_order` RENAME TO `module_order_old`",
         "can't rename module_order");
    TRY_EXEC("CREATE TABLE `module_order` (imgid INTEGER PRIMARY KEY, version INTEGER, iop_list VARCHAR, "
        "FOREIGN KEY(imgid) REFERENCES images(id) ON DELETE CASCADE ON UPDATE CASCADE)",
        "can't create new module_order table");

    TRY_EXEC("DELETE FROM `module_order_old` WHERE imgid NOT IN (SELECT id FROM `images`)",
        "can't delete orphaned module_order elements");

    TRY_EXEC("INSERT INTO `module_order` SELECT * FROM `module_order_old`",
         "can't copy back module_order meta_data");

    TRY_EXEC("DROP TABLE module_order_old",
        "can't drop table module_order_old");
    // clang-format on
    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
    new_version = 33;
  }
  else if(version == 33)
  {
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);

    TRY_EXEC("CREATE INDEX IF NOT EXISTS main.images_datetime_taken_nc ON images (datetime_taken COLLATE NOCASE)",
             "can't create images_datetime_taken");
    TRY_EXEC("CREATE INDEX IF NOT EXISTS main.metadata_index_key ON meta_data (key)",
             "can't create metadata_index_key");

    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
    new_version = 34;
  }
  else if(version == 34)
  {
    sqlite3_exec(db->handle, "PRAGMA foreign_keys = OFF", NULL, NULL, NULL);
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);

    TRY_EXEC("CREATE TABLE main.images_new (id INTEGER PRIMARY KEY AUTOINCREMENT, group_id INTEGER, film_id INTEGER, "
        "width INTEGER, height INTEGER, filename VARCHAR, maker VARCHAR, model VARCHAR, "
        "lens VARCHAR, exposure REAL, aperture REAL, iso REAL, focal_length REAL, "
        "focus_distance REAL, datetime_taken INTEGER, flags INTEGER, "
        "output_width INTEGER, output_height INTEGER, crop REAL, "
        "raw_parameters INTEGER, raw_denoise_threshold REAL, "
        "raw_auto_bright_threshold REAL, raw_black INTEGER, raw_maximum INTEGER, "
        "license VARCHAR, sha1sum CHAR(40), "
        "orientation INTEGER, histogram BLOB, lightmap BLOB, longitude REAL, "
        "latitude REAL, altitude REAL, color_matrix BLOB, colorspace INTEGER, version INTEGER, "
        "max_version INTEGER, write_timestamp INTEGER, history_end INTEGER, position INTEGER, "
        "aspect_ratio REAL, exposure_bias REAL, "
        "import_timestamp INTEGER, change_timestamp INTEGER, "
        "export_timestamp INTEGER, print_timestamp INTEGER, "
        "FOREIGN KEY(film_id) REFERENCES film_rolls(id) ON DELETE CASCADE ON UPDATE CASCADE, "
        "FOREIGN KEY(group_id) REFERENCES images(id) ON DELETE RESTRICT ON UPDATE CASCADE)",
        "can't create new images table");

    TRY_EXEC("INSERT INTO `images_new` SELECT "
        "id, group_id, film_id, width, height, filename, maker, model, "
        "lens, exposure, aperture, iso, focal_length, focus_distance, NULL AS datetime_taken, flags, "
        "output_width, output_height, crop, raw_parameters, raw_denoise_threshold, raw_auto_bright_threshold, raw_black, raw_maximum, "
        "license, sha1sum, orientation, histogram, lightmap, longitude, latitude, altitude, color_matrix, colorspace, version, "
        "max_version, write_timestamp, history_end, position, aspect_ratio, exposure_bias, "
        "NULL AS import_timestamp, NULL AS change_timestamp, NULL AS export_timestamp, NULL AS print_timestamp "
        "FROM `images`",
        "can't copy back from images");

    TRY_PREPARE(stmt, "SELECT id,"
                      " CASE WHEN datetime_taken = '' THEN NULL ELSE datetime_taken END,"
                      " CASE WHEN import_timestamp = -1 THEN NULL ELSE import_timestamp END,"
                      " CASE WHEN change_timestamp = -1 THEN NULL ELSE change_timestamp END,"
                      " CASE WHEN export_timestamp = -1 THEN NULL ELSE export_timestamp END,"
                      " CASE WHEN print_timestamp = -1 THEN NULL ELSE print_timestamp END "
                      "FROM `images`",
                "can't get datetime from images");
    while(sqlite3_step(stmt) == SQLITE_ROW)
    {
      sqlite3_stmt *stmt2;
      sqlite3_prepare_v2(db->handle,
                         "UPDATE `images_new` SET"
                         " (datetime_taken, import_timestamp,"
                         "  change_timestamp, export_timestamp, print_timestamp) = "
                         " (?2, ?3, ?4, ?5, ?6) WHERE id = ?1",
                         -1, &stmt2, NULL);
      sqlite3_bind_int(stmt2, 1, sqlite3_column_int(stmt, 0));
      if(sqlite3_column_type(stmt, 1) != SQLITE_NULL)
      {
        GDateTime *gdt = dt_datetime_exif_to_gdatetime((const char *)sqlite3_column_text(stmt, 1), darktable.utc_tz);
        if(gdt)
        {
          sqlite3_bind_int64(stmt2, 2, dt_datetime_gdatetime_to_gtimespan(gdt));
          g_date_time_unref(gdt);
        }
      }
      for(int i = 0; i < 4; i++)
      {
        if(sqlite3_column_type(stmt, i + 2) != SQLITE_NULL)
        {
          GDateTime *gdt = g_date_time_new_from_unix_utc(sqlite3_column_int(stmt, i + 2));
          if(gdt)
          {
            sqlite3_bind_int64(stmt2, i + 3, dt_datetime_gdatetime_to_gtimespan(gdt));
            g_date_time_unref(gdt);
          }
        }
      }
      TRY_STEP(stmt2, SQLITE_DONE, "can't update datetimes into images_new table");
      sqlite3_finalize(stmt2);
    }
    sqlite3_finalize(stmt);

    TRY_EXEC("DROP TABLE `images`", "can't drop images table");
    // that's the way to keep the other tables foreign keys references valid
    TRY_EXEC("ALTER TABLE `images_new` RENAME TO `images`", "can't rename images_new table to images");

    // pita: need to recreate indexes
    TRY_EXEC("CREATE INDEX `image_position_index` ON `images` (position)",
        "can't add image_position_index");
    TRY_EXEC("CREATE INDEX `images_filename_index` ON `images` ( `filename`, `version` )",
        "can't recreate images_filename_index");
    TRY_EXEC("CREATE INDEX `images_film_id_index` ON `images` ( `film_id`, `filename` )",
        "can't recreate images_film_id_index");
    TRY_EXEC("CREATE INDEX `images_group_id_index` ON `images` ( `group_id`, `id` )",
        "can't recreate images_group_id_index");
    TRY_EXEC("CREATE INDEX `images_latlong_index` ON `images` ( latitude DESC, longitude DESC )",
        "can't add images_latlong_index");
    TRY_EXEC("CREATE INDEX `images_datetime_taken` ON images (datetime_taken)",
        "can't create images_datetime_taken");

    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
    sqlite3_exec(db->handle, "PRAGMA foreign_keys = ON", NULL, NULL, NULL);
    new_version = 35;
  }
  else if(version == 35)
  {
    TRY_EXEC("CREATE TABLE main.images_new (id INTEGER, filename VARCHAR, flags INTEGER)",
             "can't create new images table");

    gchar *query = g_strdup_printf("INSERT INTO `images_new` "
                                   "SELECT id, filename, flags"
                                   " FROM images"
                                   " WHERE (flags & %d == 0)",
                                   DT_IMAGE_RAW | DT_IMAGE_LDR | DT_IMAGE_HDR);
    TRY_EXEC(query, "can't copy back from images");

    TRY_PREPARE(stmt, "SELECT id, filename, flags FROM `images_new`",
                "can't prepare selecting images flags");

    while(sqlite3_step(stmt) == SQLITE_ROW)
    {
      sqlite3_stmt *stmt2;
      sqlite3_prepare_v2(db->handle,
                         "UPDATE `images` SET"
                         " (flags) = "
                         " (?2) WHERE id = ?1",
                         -1, &stmt2, NULL);
      sqlite3_bind_int(stmt2, 1, sqlite3_column_int(stmt, 0));

      dt_image_flags_t flags = sqlite3_column_int(stmt, 2);
      gchar *ext = g_strrstr((const char *)sqlite3_column_text(stmt, 1), ".");
      flags |= dt_imageio_get_type_from_extension(ext);
      sqlite3_bind_int(stmt2, 2, flags);

      TRY_STEP(stmt2, SQLITE_DONE, "can't update flags");
      sqlite3_finalize(stmt2);
    }
    sqlite3_finalize(stmt);

    TRY_EXEC("DROP TABLE `images_new`", "can't drop temp images table");
    new_version = 36;
  }
  else if(version == 36)
  {
    TRY_EXEC("CREATE INDEX IF NOT EXISTS `metadata_index_value` ON meta_data (value)",
             "can't create metadata_index_value");
    new_version = 37;
  }
  else if(version == 37)
  {
    TRY_EXEC("ALTER TABLE main.history ADD COLUMN multi_name_hand_edited INTEGER default 0",
             "can't add multi_name_hand_edited column");
    TRY_EXEC("UPDATE main.history SET multi_name_hand_edited = 1 WHERE multi_name != ''",
             "can't set multi_name_hand_edited column");
    new_version = 38;
  }
  else if(version == 38)
  {
    sqlite3_exec(db->handle, "PRAGMA foreign_keys = OFF", NULL, NULL, NULL);
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);

    // create new tables
    TRY_EXEC("CREATE TABLE main.makers"
             " (id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "  name VARCHAR)",
             "can't create makers table");
    TRY_EXEC("CREATE TABLE main.models"
             " (id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "  name VARCHAR)",
             "can't create models table");
    TRY_EXEC("CREATE TABLE main.lens"
             " (id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "  name VARCHAR)",
             "can't create lens table");
    TRY_EXEC("CREATE TABLE cameras"
             " (id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "  name VARCHAR,"
             "  alias VARCHAR)",
             "can't create cameras table");

    // create new indexes
    TRY_EXEC("CREATE INDEX makers_name ON makers (name)",
             "can't create makers_name");
    TRY_EXEC("CREATE INDEX model_name ON models (name)",
             "can't create model_name");
    TRY_EXEC("CREATE INDEX lens_name ON lens (name)",
             "can't create lens_name");
    TRY_EXEC("CREATE INDEX camera_name ON cameras (name)",
             "can't create camera_name");

    // populate new tables
    TRY_EXEC("INSERT INTO main.makers (name)"
             " SELECT DISTINCT maker FROM main.images",
             "can't populate makers table");
    TRY_EXEC("INSERT INTO main.models (name)"
             " SELECT DISTINCT model FROM main.images",
             "can't populate models table");
    TRY_EXEC("INSERT INTO main.lens (name)"
             " SELECT DISTINCT lens FROM main.images",
             "can't populate lens table");

    // add new columns for main.images
    TRY_EXEC("ALTER TABLE main.images ADD COLUMN maker_id INTEGER default 0",
             "can't add maker_id column");
    TRY_EXEC("ALTER TABLE main.images ADD COLUMN model_id INTEGER default 0",
             "can't add model_id column");
    TRY_EXEC("ALTER TABLE main.images ADD COLUMN lens_id INTEGER default 0",
             "can't add lens_id column");
    TRY_EXEC("ALTER TABLE main.images ADD COLUMN camera_id INTEGER default 0",
             "can't add camera_id column");

    // update main images columns
    TRY_EXEC("UPDATE main.images"
             " SET maker_id = (SELECT id FROM main.makers WHERE name = maker)",
             "can't populate maker_id column");
    TRY_EXEC("UPDATE main.images"
             " SET model_id = (SELECT id FROM main.models WHERE name = model)",
             "can't populate model_id column");
    TRY_EXEC("UPDATE main.images"
             " SET lens_id = (SELECT id FROM main.lens WHERE name = lens)",
             "can't populate lens_id column");

#if 0
    // drop old columns (only supported starting with 3.35)
    TRY_EXEC("ALTER TABLE main.images DROP COLUMN maker",
             "can't drop maker column");
    TRY_EXEC("ALTER TABLE main.images DROP COLUMN model",
             "can't drop model column");
    TRY_EXEC("ALTER TABLE main.images DROP COLUMN lens",
             "can't drop lens column");
#endif

    TRY_EXEC("CREATE TABLE images_new (id INTEGER PRIMARY KEY AUTOINCREMENT, group_id INTEGER, "
             "film_id INTEGER, "
             "width INTEGER, height INTEGER, filename VARCHAR, "
             "maker_id INTEGER, model_id INTEGER, lens_id INTEGER, camera_id INTEGER,"
             "exposure REAL, aperture REAL, iso REAL, focal_length REAL, "
             "focus_distance REAL, datetime_taken INTEGER, flags INTEGER, "
             "output_width INTEGER, output_height INTEGER, crop REAL, "
             "raw_parameters INTEGER, raw_denoise_threshold REAL, "
             "raw_auto_bright_threshold REAL, raw_black INTEGER, raw_maximum INTEGER, "
             "license VARCHAR, sha1sum CHAR(40), "
             "orientation INTEGER, histogram BLOB, lightmap BLOB, longitude REAL, "
             "latitude REAL, altitude REAL, color_matrix BLOB, colorspace INTEGER, version INTEGER, "
             "max_version INTEGER, write_timestamp INTEGER, history_end INTEGER, position INTEGER, "
             "aspect_ratio REAL, exposure_bias REAL, "
             "import_timestamp INTEGER DEFAULT -1, change_timestamp INTEGER DEFAULT -1, "
             "export_timestamp INTEGER DEFAULT -1, print_timestamp INTEGER DEFAULT -1, "
             "FOREIGN KEY(maker_id) REFERENCES makers(id) ON DELETE CASCADE ON UPDATE CASCADE, "
             "FOREIGN KEY(model_id) REFERENCES models(id) ON DELETE CASCADE ON UPDATE CASCADE, "
             "FOREIGN KEY(lens_id) REFERENCES lens(id) ON DELETE CASCADE ON UPDATE CASCADE, "
             "FOREIGN KEY(camera_id) REFERENCES cameras(id) ON DELETE CASCADE ON UPDATE CASCADE, "
             "FOREIGN KEY(film_id) REFERENCES film_rolls(id) ON DELETE CASCADE ON UPDATE CASCADE, "
             "FOREIGN KEY(group_id) REFERENCES images(id) ON DELETE RESTRICT ON UPDATE CASCADE)",
             "can't create new table images");

    TRY_EXEC("INSERT INTO images_new"
             " SELECT id, group_id, film_id, width, height, filename,"
             "        maker_id, model_id, lens_id, camera_id,"
             "        exposure, aperture, iso, focal_length,"
             "        focus_distance, datetime_taken, flags,"
             "        output_width, output_height, crop,"
             "        raw_parameters, raw_denoise_threshold,"
             "        raw_auto_bright_threshold, raw_black, raw_maximum,"
             "        license, sha1sum,"
             "        orientation, histogram, lightmap, longitude,"
             "        latitude, altitude, color_matrix, colorspace, version,"
             "        max_version, write_timestamp, history_end, position,"
             "        aspect_ratio, exposure_bias,"
             "        import_timestamp, change_timestamp, export_timestamp, print_timestamp"
             "  FROM images",
             "can't populate new images table");

    TRY_EXEC("DROP TABLE images",
             "can't drop table images_old");

    TRY_EXEC("ALTER TABLE images_new RENAME TO images",
             "can't rename images");

    // recreate the indexes
    TRY_EXEC("CREATE INDEX image_position_index ON images (position)",
             "can't add image_position_index");
    TRY_EXEC("CREATE INDEX images_filename_index ON images (filename, version)",
             "can't recreate images_filename_index");
    TRY_EXEC("CREATE INDEX images_film_id_index ON images (film_id, filename)",
             "can't recreate images_film_id_index");
    TRY_EXEC("CREATE INDEX images_group_id_index ON images (group_id, id)",
             "can't recreate images_group_id_index");
    TRY_EXEC("CREATE INDEX images_latlong_index ON images (latitude DESC, longitude DESC)",
             "can't add images_latlong_index");
    TRY_EXEC("CREATE INDEX images_datetime_taken ON images (datetime_taken)",
             "can't create images_datetime_taken");

    // Some triggers to remove possible dangling refs in makers/models/lens/cameras
    TRY_EXEC("CREATE TRIGGER remove_makers AFTER DELETE ON images"
             " BEGIN"
             "  DELETE FROM makers"
             "    WHERE id = OLD.maker_id"
             "      AND NOT EXISTS (SELECT 1 FROM images WHERE maker_id = OLD.maker_id);"
             " END",
             "can't create trigger remove_makers");

    TRY_EXEC("CREATE TRIGGER remove_models AFTER DELETE ON images"
             " BEGIN"
             "  DELETE FROM models"
             "    WHERE id = OLD.model_id"
             "      AND NOT EXISTS (SELECT 1 FROM images WHERE model_id = OLD.model_id);"
             " END",
             "can't create trigger remove_models");

    TRY_EXEC("CREATE TRIGGER remove_lens AFTER DELETE ON images"
             " BEGIN"
             "  DELETE FROM lens"
             "    WHERE id = OLD.lens_id"
             "      AND NOT EXISTS (SELECT 1 FROM images WHERE lens_id = OLD.lens_id);"
             " END",
             "can't create trigger remove_lens");

    TRY_EXEC("CREATE TRIGGER remove_cameras AFTER DELETE ON images"
             " BEGIN"
             "  DELETE FROM cameras"
             "    WHERE id = OLD.camera_id"
             "      AND NOT EXISTS (SELECT 1 FROM images WHERE camera_id = OLD.camera_id);"
             " END",
             "can't create trigger remove_cameras");

    // NOTE: datetime_taken is in nano-second since "0001-01-01 00:00:00"
    TRY_EXEC
      ("CREATE VIEW v_images AS"
       " SELECT mi.id AS id, mk.name AS maker, md.name AS model, ln.name AS lens,"
       "        cm.name AS normalized_camera, cm.alias AS camera_alias,"
       "        exposure, aperture, iso,"
       "        datetime(datetime_taken/1000000"
       "                 + unixepoch('0001-01-01 00:00:00'), 'unixepoch') AS datetime,"
       "        fr.folder AS folders, filename"
       " FROM images AS mi,"
       "      makers AS mk, models AS md, lens AS ln, cameras AS cm, film_rolls AS fr"
       " WHERE mi.maker_id = mk.id"
       "   AND mi.model_id = md.id"
       "   AND mi.lens_id = ln.id"
       "   AND mi.camera_id = cm.id"
       "   AND mi.film_id = fr.id"
       " ORDER BY normalized_camera, folders",
       "can't create view v_images");

    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
    sqlite3_exec(db->handle, "PRAGMA foreign_keys = ON", NULL, NULL, NULL);

    new_version = 39;
  }
  else if(version == 39)
  {
    sqlite3_exec(db->handle, "PRAGMA foreign_keys = OFF", NULL, NULL, NULL);
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);

    TRY_EXEC("DROP TABLE cameras",
             "can't drop cameras table");

    TRY_EXEC("CREATE TABLE cameras"
             " (id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "  maker VARCHAR, model VARCHAR,"
             "  alias VARCHAR)",
             "can't create cameras table");

    TRY_EXEC("CREATE UNIQUE INDEX camera_name ON cameras (maker, model, alias)",
             "can't create camera_name");

    // NOTE: datetime_taken is in nano-second since "0001-01-01 00:00:00"
    TRY_EXEC("DROP VIEW v_images",
             "can't drop v_images view");

    TRY_EXEC
      ("CREATE VIEW v_images AS"
       " SELECT mi.id AS id, mk.name AS maker, md.name AS model, ln.name AS lens,"
       "        cm.maker || ' ' || cm.model AS normalized_camera, "
       "        cm.alias AS camera_alias,"
       "        exposure, aperture, iso,"
       "        datetime(datetime_taken/1000000"
       "                 + unixepoch('0001-01-01 00:00:00'), 'unixepoch') AS datetime,"
       "        fr.folder AS folders, filename"
       " FROM images AS mi,"
       "      makers AS mk, models AS md, lens AS ln, cameras AS cm, film_rolls AS fr"
       " WHERE mi.maker_id = mk.id"
       "   AND mi.model_id = md.id"
       "   AND mi.lens_id = ln.id"
       "   AND mi.camera_id = cm.id"
       "   AND mi.film_id = fr.id"
       " ORDER BY normalized_camera, folders",
       "can't create view v_images");

    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
    sqlite3_exec(db->handle, "PRAGMA foreign_keys = ON", NULL, NULL, NULL);

    new_version = 40;
  }
  else if(version == 40)
  {
    TRY_EXEC("ALTER TABLE main.history_hash ADD COLUMN fullthumb_hash BLOB default NULL",
             "can't add fullthumb_hash column");
    new_version = 41;
  }
  else if(version == 41)
  {
    sqlite3_exec(db->handle, "PRAGMA foreign_keys = OFF", NULL, NULL, NULL);
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);

    TRY_EXEC("CREATE TABLE images_new (id INTEGER PRIMARY KEY AUTOINCREMENT, group_id INTEGER, "
             "film_id INTEGER, "
             "width INTEGER, height INTEGER, filename VARCHAR, "
             "maker_id INTEGER, model_id INTEGER, lens_id INTEGER, camera_id INTEGER,"
             "exposure REAL, aperture REAL, iso REAL, focal_length REAL, "
             "focus_distance REAL, datetime_taken INTEGER, flags INTEGER, "
             "output_width INTEGER, output_height INTEGER, crop REAL, "
             "raw_parameters INTEGER, raw_black INTEGER, raw_maximum INTEGER, "
             "orientation INTEGER, longitude REAL, "
             "latitude REAL, altitude REAL, color_matrix BLOB, colorspace INTEGER, version INTEGER, "
             "max_version INTEGER, write_timestamp INTEGER, history_end INTEGER, position INTEGER, "
             "aspect_ratio REAL, exposure_bias REAL, "
             "import_timestamp INTEGER DEFAULT -1, change_timestamp INTEGER DEFAULT -1, "
             "export_timestamp INTEGER DEFAULT -1, print_timestamp INTEGER DEFAULT -1, "
             "FOREIGN KEY(maker_id) REFERENCES makers(id) ON DELETE CASCADE ON UPDATE CASCADE, "
             "FOREIGN KEY(model_id) REFERENCES models(id) ON DELETE CASCADE ON UPDATE CASCADE, "
             "FOREIGN KEY(lens_id) REFERENCES lens(id) ON DELETE CASCADE ON UPDATE CASCADE, "
             "FOREIGN KEY(camera_id) REFERENCES cameras(id) ON DELETE CASCADE ON UPDATE CASCADE, "
             "FOREIGN KEY(film_id) REFERENCES film_rolls(id) ON DELETE CASCADE ON UPDATE CASCADE, "
             "FOREIGN KEY(group_id) REFERENCES images(id) ON DELETE RESTRICT ON UPDATE CASCADE)",
             "can't create new table images");

    TRY_EXEC("INSERT INTO images_new"
             " SELECT id, group_id, film_id, width, height, filename,"
             "        maker_id, model_id, lens_id, camera_id,"
             "        exposure, aperture, iso, focal_length,"
             "        focus_distance, datetime_taken, flags,"
             "        output_width, output_height, crop,"
             "        raw_parameters, raw_black, raw_maximum,"
             "        orientation, longitude,"
             "        latitude, altitude, color_matrix, colorspace, version,"
             "        max_version, write_timestamp, history_end, position,"
             "        aspect_ratio, exposure_bias,"
             "        import_timestamp, change_timestamp, export_timestamp, print_timestamp"
             "  FROM images",
             "can't populate new images table");

    // NOTE: datetime_taken is in nano-second since "0001-01-01 00:00:00"
    TRY_EXEC("DROP VIEW v_images",
             "can't drop v_images view");

    TRY_EXEC("DROP TABLE images",
             "can't drop table images_old");

    TRY_EXEC("ALTER TABLE images_new RENAME TO images",
             "can't rename images");

    TRY_EXEC
      ("CREATE VIEW v_images AS"
       " SELECT mi.id AS id, mk.name AS maker, md.name AS model, ln.name AS lens,"
       "        cm.maker || ' ' || cm.model AS normalized_camera, "
       "        cm.alias AS camera_alias,"
       "        exposure, aperture, iso,"
       "        datetime(datetime_taken/1000000"
       "                 + unixepoch('0001-01-01 00:00:00'), 'unixepoch') AS datetime,"
       "        fr.folder AS folders, filename"
       " FROM images AS mi,"
       "      makers AS mk, models AS md, lens AS ln, cameras AS cm, film_rolls AS fr"
       " WHERE mi.maker_id = mk.id"
       "   AND mi.model_id = md.id"
       "   AND mi.lens_id = ln.id"
       "   AND mi.camera_id = cm.id"
       "   AND mi.film_id = fr.id"
       " ORDER BY normalized_camera, folders",
       "can't create view v_images");

    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
    sqlite3_exec(db->handle, "PRAGMA foreign_keys = ON", NULL, NULL, NULL);

    new_version = 42;
  }
  else if(version == 42)
  {
    TRY_EXEC("ALTER TABLE main.history_hash ADD COLUMN fullthumb_maxmip INTEGER default 0",
             "can't add fullthumb_maxmip column");
    new_version = 43;
  }
  else if(version == 43)
  {
    // add back triggers and indices removed during last images changes.

    // recreate the indexes
    TRY_EXEC("CREATE INDEX image_position_index ON images (position)",
             "can't add image_position_index");
    TRY_EXEC("CREATE INDEX images_filename_index ON images (filename, version)",
             "can't recreate images_filename_index");
    TRY_EXEC("CREATE INDEX images_film_id_index ON images (film_id, filename)",
             "can't recreate images_film_id_index");
    TRY_EXEC("CREATE INDEX images_group_id_index ON images (group_id, id)",
             "can't recreate images_group_id_index");
    TRY_EXEC("CREATE INDEX images_latlong_index ON images (latitude DESC, longitude DESC)",
             "can't add images_latlong_index");
    TRY_EXEC("CREATE INDEX images_datetime_taken ON images (datetime_taken)",
             "can't create images_datetime_taken");

    // Some triggers to remove possible dangling refs in makers/models/lens/cameras
    TRY_EXEC("CREATE TRIGGER remove_makers AFTER DELETE ON images"
             " BEGIN"
             "  DELETE FROM makers"
             "    WHERE id = OLD.maker_id"
             "      AND NOT EXISTS (SELECT 1 FROM images WHERE maker_id = OLD.maker_id);"
             " END",
             "can't create trigger remove_makers");

    TRY_EXEC("CREATE TRIGGER remove_models AFTER DELETE ON images"
             " BEGIN"
             "  DELETE FROM models"
             "    WHERE id = OLD.model_id"
             "      AND NOT EXISTS (SELECT 1 FROM images WHERE model_id = OLD.model_id);"
             " END",
             "can't create trigger remove_models");

    TRY_EXEC("CREATE TRIGGER remove_lens AFTER DELETE ON images"
             " BEGIN"
             "  DELETE FROM lens"
             "    WHERE id = OLD.lens_id"
             "      AND NOT EXISTS (SELECT 1 FROM images WHERE lens_id = OLD.lens_id);"
             " END",
             "can't create trigger remove_lens");

    TRY_EXEC("CREATE TRIGGER remove_cameras AFTER DELETE ON images"
             " BEGIN"
             "  DELETE FROM cameras"
             "    WHERE id = OLD.camera_id"
             "      AND NOT EXISTS (SELECT 1 FROM images WHERE camera_id = OLD.camera_id);"
             " END",
             "can't create trigger remove_cameras");

    new_version = 44;
  }
  else if(version == 44)
  {
    sqlite3_exec(db->handle, "PRAGMA foreign_keys = OFF", NULL, NULL, NULL);
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);

    // As we cannot rename a table if we have FOREIGN KEY or CASCADE
    // we do a workaround by creating a tmp table and populate data twice.

    TRY_EXEC("CREATE TABLE tmp_history_hash"
                " (imgid INTEGER PRIMARY KEY,"
                "  basic_hash BLOB, auto_hash BLOB, current_hash BLOB, mipmap_hash BLOB,"
                "  FOREIGN KEY(imgid) REFERENCES images(id) ON UPDATE CASCADE ON DELETE CASCADE)",
              "can't create table tmp_history_hash");

    TRY_EXEC("INSERT INTO tmp_history_hash"
                " SELECT imgid, basic_hash, auto_hash, current_hash, mipmap_hash"
                " FROM history_hash",
              "can't populate table tmp_history_hash");

    TRY_EXEC("DROP TABLE history_hash",
              "can't drop table history_hash");

    TRY_EXEC("CREATE TABLE history_hash"
                " (imgid INTEGER PRIMARY KEY,"
                "  basic_hash BLOB, auto_hash BLOB, current_hash BLOB, mipmap_hash BLOB,"
                "  FOREIGN KEY(imgid) REFERENCES images(id) ON UPDATE CASCADE ON DELETE CASCADE)",
              "can't create new table history_hash");

    TRY_EXEC("INSERT INTO history_hash"
                " SELECT imgid, basic_hash, auto_hash, current_hash, mipmap_hash"
                " FROM tmp_history_hash",
              "can't populate table history_hash");

    TRY_EXEC("DROP TABLE tmp_history_hash",
              "can't drop table tmp_history_hash");

    TRY_EXEC("ALTER TABLE images ADD COLUMN thumb_timestamp INTEGER default -1",
             "can't add fullthumb_hash column");

    TRY_EXEC("ALTER TABLE images ADD COLUMN thumb_maxmip INTEGER default 0",
             "can't add fullthumb_maxmip column");

    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
    sqlite3_exec(db->handle, "PRAGMA foreign_keys = ON", NULL, NULL, NULL);

    new_version = 45;
  }
  else if(version == 45)
  {
    TRY_EXEC("DROP TABLE IF EXISTS legacy_presets",
              "can't drop legacy_presets");

    new_version = 46;
  }
  else if(version == 46)
  {
    TRY_EXEC("CREATE TABLE harmony_guide"
             " (imgid INTEGER PRIMARY KEY,"
             "  type INTEGER, rotation INTEGER, width INTEGER,"
             "  FOREIGN KEY(imgid) REFERENCES images(id)"
             "    ON UPDATE CASCADE ON DELETE CASCADE)",
             "can't create table harmony_guide");

    new_version = 47;
  }
  else if(version == 47)
  {
    TRY_EXEC("CREATE TABLE overlay"
             " (imgid INTEGER, overlay_id INTEGER,"
             "  PRIMARY KEY (imgid, overlay_id),"
             "  FOREIGN KEY(imgid) REFERENCES images(id)"
             "    ON UPDATE CASCADE ON DELETE CASCADE)",
             "can't create table overlay");

    new_version = 48;
  }
  else if(version == 48)
  {
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);

    TRY_EXEC("CREATE TABLE tmp_selected_images (imgid INTEGER PRIMARY KEY)",
             "can't create table tmp_selected_images");

    TRY_EXEC("INSERT INTO tmp_selected_images"
             " SELECT imgid FROM selected_images",
              "can't populate table tmp_selected_images");

    TRY_EXEC("DROP TABLE selected_images",
              "can't drop selected_images");

    TRY_EXEC("CREATE TABLE selected_images (num INTEGER PRIMARY KEY AUTOINCREMENT,"
             "                              imgid INTEGER,"
             "  FOREIGN KEY(imgid) REFERENCES images(id)"
             "    ON UPDATE CASCADE ON DELETE CASCADE)",
             "can't create table selected_images");

    TRY_EXEC("CREATE UNIQUE INDEX selected_images_ni"
             " ON selected_images (num, imgid)",
             "can't create index selected_images_ni");

    TRY_EXEC("INSERT INTO selected_images (imgid)"
             " SELECT imgid FROM tmp_selected_images",
              "can't populate table selected_images");

    TRY_EXEC("DROP TABLE tmp_selected_images",
              "can't drop tmp_selected_images");

    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
    new_version = 49;
  }
  else if(version == 49)
  {
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);

    TRY_EXEC("DROP INDEX selected_images_ni",
             "can't drop index selected_images_ni");

    TRY_EXEC("CREATE UNIQUE INDEX selected_images_ni"
             " ON selected_images (imgid)",
             "can't create index selected_images_ni");

    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
    new_version = 50;
  }
  else if(version == 50)
  {
    sqlite3_exec(db->handle, "PRAGMA foreign_keys = OFF", NULL, NULL, NULL);
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);

    TRY_EXEC("CREATE TABLE whitebalance"
             " (id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "  name VARCHAR)",
              "can't create table whitebalance");
    TRY_EXEC("CREATE UNIQUE INDEX whitebalance_name ON whitebalance (name)",
             "can't create index `whitebalance_name' on table `whitebalance'");

    TRY_EXEC("CREATE TABLE flash"
             " (id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "  name VARCHAR)",
              "can't create table flash");
    TRY_EXEC("CREATE UNIQUE INDEX flash_name ON flash (name)",
             "can't create index `flash_name' on table `flash'");

    TRY_EXEC("CREATE TABLE exposure_program"
             " (id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "  name VARCHAR)",
              "can't create table exposure_program");
    TRY_EXEC("CREATE UNIQUE INDEX exposure_program_name ON exposure_program (name)",
             "can't create index `exposure_program_name' on table `exposure_program'");

    TRY_EXEC("CREATE TABLE metering_mode"
             " (id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "  name VARCHAR)",
              "can't create table metering_mode");
    TRY_EXEC("CREATE UNIQUE INDEX metering_mode_name ON metering_mode (name)",
             "can't create index `metering_mode_name' on table `metering_mode'");

    TRY_EXEC("CREATE TABLE images_new"
             " (id INTEGER PRIMARY KEY AUTOINCREMENT, group_id INTEGER, film_id INTEGER,"
             "  width INTEGER, height INTEGER, filename VARCHAR,"
             "  maker_id INTEGER, model_id INTEGER, lens_id INTEGER, camera_id INTEGER,"
             "  exposure REAL, aperture REAL, iso REAL, focal_length REAL,"
             "  focus_distance REAL, datetime_taken INTEGER, flags INTEGER,"
             "  output_width INTEGER, output_height INTEGER, crop REAL,"
             "  raw_parameters INTEGER, raw_black INTEGER, raw_maximum INTEGER,"
             "  orientation INTEGER, longitude REAL,"
             "  latitude REAL, altitude REAL, color_matrix BLOB, colorspace INTEGER,"
             "  version INTEGER, max_version INTEGER, write_timestamp INTEGER,"
             "  history_end INTEGER, position INTEGER, aspect_ratio REAL, exposure_bias REAL,"
             "  import_timestamp INTEGER DEFAULT -1, change_timestamp INTEGER DEFAULT -1, "
             "  export_timestamp INTEGER DEFAULT -1, print_timestamp INTEGER DEFAULT -1, "
             "  thumb_timestamp INTEGER DEFAULT -1, thumb_maxmip INTEGER DEFAULT 0, "
             "  whitebalance_id INTEGER, flash_id INTEGER, "
             "  exposure_program_id INTEGER, metering_mode_id INTEGER, "
             "FOREIGN KEY(maker_id) REFERENCES makers(id) ON DELETE CASCADE ON UPDATE CASCADE, "
             "FOREIGN KEY(model_id) REFERENCES models(id) ON DELETE CASCADE ON UPDATE CASCADE, "
             "FOREIGN KEY(lens_id) REFERENCES lens(id) ON DELETE CASCADE ON UPDATE CASCADE, "
             "FOREIGN KEY(camera_id) REFERENCES cameras(id) ON DELETE CASCADE ON UPDATE CASCADE, "
             "FOREIGN KEY(film_id) REFERENCES film_rolls(id) ON DELETE CASCADE ON UPDATE CASCADE, "
             "FOREIGN KEY(group_id) REFERENCES images(id) ON DELETE RESTRICT ON UPDATE CASCADE, "
             "FOREIGN KEY(whitebalance_id) REFERENCES whitebalance(id) ON DELETE CASCADE ON UPDATE CASCADE, "
             "FOREIGN KEY(flash_id) REFERENCES flash(id) ON DELETE CASCADE ON UPDATE CASCADE, "
             "FOREIGN KEY(exposure_program_id) REFERENCES exposure_program(id) ON DELETE CASCADE ON UPDATE CASCADE, "
             "FOREIGN KEY(metering_mode_id) REFERENCES metering_mode(id) ON DELETE CASCADE ON UPDATE CASCADE)",
             "can't create new table images");

    TRY_EXEC("INSERT INTO images_new"
             " SELECT id, group_id, film_id, width, height, filename,"
             "        maker_id, model_id, lens_id, camera_id,"
             "        exposure, aperture, iso, focal_length,"
             "        focus_distance, datetime_taken, flags,"
             "        output_width, output_height, crop,"
             "        raw_parameters, raw_black, raw_maximum,"
             "        orientation, longitude,"
             "        latitude, altitude, color_matrix, colorspace, version,"
             "        max_version, write_timestamp, history_end, position,"
             "        aspect_ratio, exposure_bias,"
             "        import_timestamp, change_timestamp, export_timestamp, print_timestamp,"
             "        thumb_timestamp, thumb_maxmip,"
             "        NULL, NULL, NULL, NULL"
             "  FROM images",
             "can't populate new images table");

    // NOTE: datetime_taken is in nano-second since "0001-01-01 00:00:00"
    TRY_EXEC("DROP VIEW v_images",
             "can't drop v_images view");

    TRY_EXEC("DROP TABLE images",
             "can't drop table images");

    TRY_EXEC("ALTER TABLE images_new RENAME TO images",
             "can't rename images");

    TRY_EXEC
      ("CREATE VIEW v_images AS"
       " SELECT mi.id AS id, mk.name AS maker, md.name AS model, ln.name AS lens,"
       "        cm.maker || ' ' || cm.model AS normalized_camera, "
       "        cm.alias AS camera_alias,"
       "        exposure, aperture, iso,"
       "        datetime(datetime_taken/1000000"
       "                 + unixepoch('0001-01-01 00:00:00'), 'unixepoch') AS datetime,"
       "        fr.folder AS folders, filename"
       " FROM images AS mi,"
       "      makers AS mk, models AS md, lens AS ln, cameras AS cm, film_rolls AS fr"
       " WHERE mi.maker_id = mk.id"
       "   AND mi.model_id = md.id"
       "   AND mi.lens_id = ln.id"
       "   AND mi.camera_id = cm.id"
       "   AND mi.film_id = fr.id"
       " ORDER BY normalized_camera, folders",
       "can't create view v_images");

    // recreate the indexesx
    TRY_EXEC("CREATE INDEX image_position_index ON images (position)",
             "can't add image_position_index");
    TRY_EXEC("CREATE INDEX images_filename_index ON images (filename, version)",
             "can't recreate images_filename_index");
    TRY_EXEC("CREATE INDEX images_film_id_index ON images (film_id, filename)",
             "can't recreate images_film_id_index");
    TRY_EXEC("CREATE INDEX images_group_id_index ON images (group_id, id)",
             "can't recreate images_group_id_index");
    TRY_EXEC("CREATE INDEX images_latlong_index ON images (latitude DESC, longitude DESC)",
             "can't add images_latlong_index");
    TRY_EXEC("CREATE INDEX images_datetime_taken ON images (datetime_taken)",
             "can't create images_datetime_taken");

    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
    sqlite3_exec(db->handle, "PRAGMA foreign_keys = ON", NULL, NULL, NULL);

    new_version = 51;
  }
  else if(version == 51 || version == 52)
  {
    /* the code to create the DB schema from scratch had a temporary version that set the version number as 52
       but did not create the index correctly (only this migration code was updated at the time), so
       let's repeat the migration steps if we are potentially on that broken interim version 52 */

    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);

    // As the selected_images table might have non-unique data the UNIQUE INDEX could fail,
    // we avoid this by recreating both the table & index.
    // minor downside: selection is lost while updating database sheme.
    TRY_EXEC("DROP TABLE selected_images",
             "can't drop selected_images");

    TRY_EXEC("CREATE TABLE selected_images (num INTEGER PRIMARY KEY AUTOINCREMENT, imgid INTEGER)",
             "can't create selected_images");

    TRY_EXEC("CREATE UNIQUE INDEX selected_images_ni"
             " ON selected_images (imgid)",
             "can't create index selected_images_ni");

    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
    /* even if we were at version 51, the step is the same for 51 -> 52 and 52 -> 53 (see above), so jump straight to 53 */
    new_version = 53;
  }
  else if(version == 53)
  {
    sqlite3_exec(db->handle, "PRAGMA foreign_keys = OFF", NULL, NULL, NULL);
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);

    // NOTE: datetime_taken is in nano-second since "0001-01-01 00:00:00"
    TRY_EXEC("DROP VIEW v_images",
             "can't drop v_images view");

    TRY_EXEC
      ("CREATE VIEW v_images AS"
       " SELECT mi.id AS id, mk.name AS maker, md.name AS model, ln.name AS lens,"
       "        cm.maker || ' ' || cm.model AS normalized_camera, "
       "        cm.alias AS camera_alias,"
       "        exposure, aperture, iso,"
       "        datetime(datetime_taken/1000000"
       "                 + unixepoch('0001-01-01 00:00:00'), 'unixepoch') AS datetime,"
       "        fr.folder AS folders, filename"
       " FROM images AS mi,"
       "      makers AS mk, models AS md, lens AS ln, cameras AS cm, film_rolls AS fr"
       " WHERE mi.maker_id = mk.id"
       "   AND mi.model_id = md.id"
       "   AND mi.lens_id = ln.id"
       "   AND mi.camera_id = cm.id"
       "   AND mi.film_id = fr.id"
       " ORDER BY normalized_camera, folders",
       "can't create view v_images");

    // meta data
    TRY_EXEC("ALTER TABLE `meta_data` RENAME TO `meta_data_old`",
             "can't rename meta_data");
    TRY_EXEC("CREATE TABLE `meta_data` (id integer, key integer, value varchar, "
             "FOREIGN KEY(id) REFERENCES images(id) ON DELETE CASCADE ON UPDATE CASCADE)",
             "can't create new meta_data table");

    TRY_EXEC("DELETE FROM `meta_data_old` WHERE id NOT IN (SELECT id FROM `images`)",
             "can't delete orphaned meta_data elements");

    TRY_EXEC("INSERT INTO `meta_data` SELECT * FROM `meta_data_old`",
             "can't copy back from meta_data");

    TRY_EXEC("DROP TABLE meta_data_old",
             "can't drop table meta_data_old");

    TRY_EXEC("CREATE UNIQUE INDEX `metadata_index` ON `meta_data` (id, key, value)",
             "can't recreate metadata_index");

    TRY_EXEC("CREATE INDEX main.metadata_index_key ON meta_data (key)",
             "can't recreate metadata_index");

    TRY_EXEC("CREATE INDEX main.metadata_index_value ON meta_data (value)",
             "can't create metadata_index_value");

    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
    sqlite3_exec(db->handle, "PRAGMA foreign_keys = ON", NULL, NULL, NULL);

    new_version = 54;
  }
  else if(version == 54)
  {
    // NOTE: datetime_taken is in nano-second since "0001-01-01 00:00:00"
    TRY_EXEC("DROP VIEW v_images",
             "can't drop v_images view");

    TRY_EXEC
      ("CREATE VIEW v_images AS"
       " SELECT mi.id AS id, mk.name AS maker, md.name AS model, ln.name AS lens,"
       "        cm.maker || ' ' || cm.model AS normalized_camera, "
       "        cm.alias AS camera_alias,"
       "        exposure, aperture, iso,"
       "        datetime(datetime_taken/1000000"
       "                 + unixepoch('0001-01-01 00:00:00'), 'unixepoch') AS datetime,"
       "        fr.folder AS folders, filename"
       " FROM images AS mi,"
       "      makers AS mk, models AS md, lens AS ln, cameras AS cm, film_rolls AS fr"
       " WHERE mi.maker_id = mk.id"
       "   AND mi.model_id = md.id"
       "   AND mi.lens_id = ln.id"
       "   AND mi.camera_id = cm.id"
       "   AND mi.film_id = fr.id"
       " ORDER BY normalized_camera, folders",
       "can't create view v_images");

    new_version = 55;
  }
  else if(version == 55)
  {
    sqlite3_exec(db->handle, "PRAGMA foreign_keys = OFF", NULL, NULL, NULL);
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);

    TRY_EXEC("CREATE TABLE overlay_new("
             " imgid INTEGER, overlay_id INTEGER,"
             " PRIMARY KEY (imgid, overlay_id),"
             " FOREIGN KEY(imgid) REFERENCES images(id) ON UPDATE CASCADE ON DELETE CASCADE,"
             " FOREIGN KEY(overlay_id) REFERENCES images(id) ON UPDATE CASCADE ON DELETE RESTRICT"
             ")",
             "can't create migration target table 'overlay_new'");

    TRY_EXEC("INSERT INTO overlay_new"
             " SELECT imgid, overlay_id"
             " FROM overlay",
             "can't populate migration target table 'overlay_new'");

    TRY_EXEC("CREATE INDEX main.overlay_overlay_id_index ON overlay_new (overlay_id)",
             "can't create index on 'overlay_id'");

    TRY_EXEC("DROP TABLE overlay",
             "can't drop old table 'overlay'");

    TRY_EXEC("ALTER TABLE overlay_new RENAME TO overlay",
             "can't rename table 'overlay_new' to 'overlay'");

    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
    sqlite3_exec(db->handle, "PRAGMA foreign_keys = ON", NULL, NULL, NULL);
    new_version = 56;
  }
  else
    new_version = version; // should be the fallback so that calling code sees that we are in an infinite loop

  // write the new version to db
  sqlite3_prepare_v2(db->handle,
                     "INSERT OR REPLACE"
                     " INTO main.db_info (key, value)"
                     " VALUES ('version', ?1)", -1, &stmt,
                     NULL);
  sqlite3_bind_int(stmt, 1, new_version);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  return new_version;
}

/* do the real migration steps, returns the version the db was converted to */
static int _upgrade_data_schema_step(dt_database_t *db, int version)
{
  sqlite3_stmt *stmt;
  int new_version = version;
  if(version == CURRENT_DATABASE_VERSION_DATA)
    return version;
  else if(version == 0)
  {
    // this can't happen, we started with 1, but it's a good example how this function works
    // <do some magic to the db>
    new_version = 1; // the version we transformed the db to. this way it might be possible to roll back or
    // add fast paths
  }
  else if(version == 1)
  {
    // clang-format off
    // style_items:
    //    NO TRY_EXEC has the column could be there before version 1 (master build)
    //    TRY_EXEC("ALTER TABLE data.style_items ADD COLUMN iop_order REAL",
    //             "can't add `iop_order' column to style_items table in database");
    sqlite3_exec(db->handle, "ALTER TABLE data.style_items ADD COLUMN iop_order REAL", NULL, NULL, NULL);
    // clang-format on
    sqlite3_stmt *sel_stmt = NULL;
    GList *prior_v1 = dt_ioppr_get_iop_order_list_version(DT_IOP_ORDER_LEGACY);
    // clang-format off
    // create a temp table with the previous priorities
    TRY_EXEC("CREATE TEMPORARY TABLE iop_order_tmp (iop_order REAL, operation VARCHAR(256))",
             "can't create temporary table for updating `data.style_items'");
    // clang-format on
    // fill temp table with all operations up to this release
    // it will be used to create the pipe and update the iop_order on history
    for(GList *priorities = prior_v1; priorities; priorities = g_list_next(priorities))
    {
      dt_iop_order_entry_t *prior = priorities->data;

      sqlite3_prepare_v2(
          db->handle,
          "INSERT INTO iop_order_tmp (iop_order, operation) VALUES (?1, ?2)",
          -1, &stmt, NULL);
      sqlite3_bind_double(stmt, 1, prior->o.iop_order_f);
      sqlite3_bind_text(stmt, 2, prior->operation, -1, SQLITE_TRANSIENT);
      TRY_STEP(stmt, SQLITE_DONE, "can't insert default value in iop_order_tmp");
      sqlite3_finalize(stmt);
    }
    g_list_free_full(prior_v1, free);

    // do the same as for history
    // clang-format off
    TRY_EXEC("UPDATE data.style_items SET iop_order = ((("
        "SELECT MAX(multi_priority) FROM data.style_items style1 WHERE style1.styleid = data.style_items.styleid AND style1.operation = data.style_items.operation "
             ") + 1. - multi_priority) / 1000.) + "
             "IFNULL((SELECT iop_order FROM iop_order_tmp WHERE iop_order_tmp.operation = "
             "data.style_items.operation), -999999.) ",
             "can't update iop_order in style_items table");

    TRY_PREPARE(sel_stmt, "SELECT DISTINCT operation FROM data.style_items WHERE iop_order <= 0 OR iop_order IS NULL",
                "can't prepare selecting style_items iop_order");
    // clang-format on
    while(sqlite3_step(sel_stmt) == SQLITE_ROW)
    {
      const char *op_name = (const char *)sqlite3_column_text(sel_stmt, 0);
      dt_print(DT_DEBUG_ALWAYS, "operation %s with no iop_order while upgrading style_items in database", op_name);
    }
    sqlite3_finalize(sel_stmt);
    // clang-format off
    TRY_EXEC("DROP TABLE iop_order_tmp", "can't drop table `iop_order_tmp' from database");
    // clang-format on
    new_version = 2;
  }
  else if(version == 2)
  {
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);

    //    With sqlite above or equal to 3.25.0 RENAME COLUMN can be used instead of the following code
    //    TRY_EXEC("ALTER TABLE data.tags RENAME COLUMN description TO synonyms;",
    //             "can't change tags column name from description to synonyms");
    // clang-format off
    TRY_EXEC("ALTER TABLE data.tags RENAME TO tmp_tags",  "can't rename table tags");

    TRY_EXEC("CREATE TABLE data.tags (id INTEGER PRIMARY KEY, name VARCHAR, "
             "synonyms VARCHAR, flags INTEGER)",
             "can't create new tags table");

    TRY_EXEC("INSERT INTO data.tags (id, name, synonyms, flags) SELECT id, name, description, flags "
             "FROM tmp_tags",
             "can't populate tags table from tmp_tags");

    TRY_EXEC("DROP TABLE tmp_tags", "can't delete table tmp_tags");

    TRY_EXEC("CREATE UNIQUE INDEX data.tags_name_idx ON tags (name)",
             "can't create tags_name_idx on tags table");
    // clang-format on
    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);

    new_version = 3;
  }
  else if(version == 3)
  {
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);
    // clang-format off
    // create a temp table to invert all multi_priority
    TRY_EXEC("CREATE TEMPORARY TABLE m_prio (id INTEGER, operation VARCHAR(256), prio INTEGER)",
             "can't create temporary table for updating `history and style_items'");

    TRY_EXEC("INSERT INTO m_prio SELECT styleid, operation, MAX(multi_priority)"
             " FROM data.style_items GROUP BY styleid, operation",
             "can't populate m_prio");

    // update multi_priority for style items and history
    TRY_EXEC("UPDATE data.style_items SET multi_priority = "
             "(SELECT prio FROM m_prio "
             " WHERE data.style_items.operation = operation AND data.style_items.styleid = id)"
             " - data.style_items.multi_priority",
             "can't update multi_priority for style_items");

    TRY_EXEC("DROP TABLE m_prio", "can't drop table `m_prio' from database");
    // clang-format on
    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);

    new_version = 4;
  }
  else if(version == 4)
  {
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);
    // clang-format off
    // remove iop_order from style_item table
    TRY_EXEC("ALTER TABLE data.style_items RENAME TO s",
             "can't rename style_items to s");
    TRY_EXEC("CREATE TABLE data.style_items (styleid INTEGER, num INTEGER, module INTEGER, "
             "operation VARCHAR(256), op_params BLOB, enabled INTEGER, "
             "blendop_params BLOB, blendop_version INTEGER, multi_priority INTEGER, multi_name VARCHAR(256))",
             "can't create style_items table'");
    TRY_EXEC("INSERT INTO data.style_items SELECT styleid, num, module, operation, op_params, enabled, "
             " blendop_params, blendop_version, multi_priority, multi_name "
             "FROM s",
             "can't populate style_items table'");
    TRY_EXEC("DROP TABLE s",
             "can't drop table s'");
    // clang-format on
    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);

    new_version = 5;
  }
  else if(version == 5)
  {
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);
    // clang-format of
    // make style.id a PRIMARY KEY and add iop_list
    TRY_EXEC("ALTER TABLE data.styles RENAME TO s",
             "can't rename styles to s");
    TRY_EXEC("CREATE TABLE data.styles (id INTEGER PRIMARY KEY, name VARCHAR, description VARCHAR, iop_list VARCHAR)",
             "can't create styles table");
    TRY_EXEC("INSERT INTO data.styles SELECT id, name, description, NULL FROM s",
             "can't populate styles table");
    TRY_EXEC("DROP TABLE s",
             "can't drop table s");

    TRY_EXEC("CREATE INDEX IF NOT EXISTS data.styles_name_index ON styles (name)",
             "can't create styles_nmae_index");

    // make style_items.styleid index

    TRY_EXEC("CREATE INDEX IF NOT EXISTS data.style_items_styleid_index ON style_items (styleid)",
             "can't create style_items_styleid_index");
    // clang-format on
    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);

    new_version = 6;
  }
  else if(version == 6)
  {
    // clang-format off
    TRY_EXEC("CREATE TABLE data.locations "
             "(tagid INTEGER PRIMARY KEY, type INTEGER, longitude REAL, latitude REAL, "
             "delta1 REAL, delta2 REAL, FOREIGN KEY(tagid) REFERENCES tags(id))",
             "can't create new locations table");
    // clang-format on
    new_version = 7;
  }
  else if(version == 7)
  {
    // clang-format off
    TRY_EXEC("ALTER TABLE data.locations ADD COLUMN ratio FLOAT DEFAULT 1",
             "can't add column `ratio' column to locations table");
    // clang-format on
    new_version = 8;
  }
  else if(version == 8)
  {
    // clang-format off
    TRY_EXEC("ALTER TABLE data.locations ADD COLUMN polygons BLOB",
             "can't add column `polygons' column to locations table");
    // clang-format on
    new_version = 9;
  }
  else if(version == 9)
  {
    TRY_EXEC("ALTER TABLE data.style_items ADD COLUMN multi_name_hand_edited INTEGER default 0",
             "can't add multi_name_hand_edited column");
    TRY_EXEC("UPDATE data.style_items SET multi_name_hand_edited = 1 WHERE multi_name != ''",
             "can't set multi_name_hand_edited column");

    TRY_EXEC("ALTER TABLE data.presets ADD COLUMN multi_name_hand_edited INTEGER default 0",
             "can't add multi_name_hand_edited column");
    TRY_EXEC("UPDATE data.presets SET multi_name_hand_edited = 1 WHERE multi_name != ''",
             "can't set multi_name_hand_edited column");

    new_version = 10;
  }
  else
    new_version = version; // should be the fallback so that calling code sees that we are in an infinite loop

  // write the new version to db
  // clang-format off¨
  sqlite3_prepare_v2(db->handle,
                     "INSERT OR REPLACE"
                     " INTO data.db_info (key, value)"
                     " VALUES ('version', ?1)", -1, &stmt,
                     NULL);
  // clang-format on
  sqlite3_bind_int(stmt, 1, new_version);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  return new_version;
}

#undef FINALIZE

#undef TRY_EXEC
#undef TRY_STEP
#undef TRY_PREPARE

/* upgrade library db from 'version' to CURRENT_DATABASE_VERSION_LIBRARY. don't touch this function but
 * _upgrade_library_schema_step() instead. */
static gboolean _upgrade_library_schema(dt_database_t *db, int version)
{
  while(version < CURRENT_DATABASE_VERSION_LIBRARY)
  {
    const int new_version = _upgrade_library_schema_step(db, version);
    if(new_version == version)
      return FALSE; // we don't know how to upgrade this db. probably
                    // a bug in _upgrade_library_schema_step
    else
      version = new_version;
  }
  return TRUE;
}

/* upgrade data db from 'version' to CURRENT_DATABASE_VERSION_DATA. don't touch this function but
 * _upgrade_data_schema_step() instead. */
static gboolean _upgrade_data_schema(dt_database_t *db, int version)
{
  while(version < CURRENT_DATABASE_VERSION_DATA)
  {
    const int new_version = _upgrade_data_schema_step(db, version);
    if(new_version == version)
      return FALSE; // we don't know how to upgrade this db. probably a bug in _upgrade_data_schema_step
    else
      version = new_version;
  }
  return TRUE;
}

/* create the 'library' database schema according to the state from 27 October 2024, and set the version in db_info accordingly.
 * from now on, do NOT update this function; simply add further modifications to _upgrade_library_schema for consistency. */
static void _create_library_schema(dt_database_t *db)
{
  sqlite3_stmt *stmt;
  ////////////////////////////// db_info
  // clang-format off
  sqlite3_exec
    (db->handle, "CREATE TABLE main.db_info (key VARCHAR PRIMARY KEY, value VARCHAR)",
     NULL, NULL, NULL);
  sqlite3_prepare_v2
    (db->handle, "INSERT OR REPLACE INTO main.db_info (key, value) VALUES ('version', ?1)",
     -1, &stmt, NULL);
  // clang-format on
  sqlite3_bind_int(stmt, 1, LAST_FULL_DATABASE_VERSION_LIBRARY);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  ////////////////////////////// film_rolls
  // clang-format off
  sqlite3_exec(db->handle,
               "CREATE TABLE main.film_rolls "
               "(id INTEGER PRIMARY KEY, access_timestamp INTEGER, "
               //                        "folder VARCHAR(1024), external_drive VARCHAR(1024))", //
               //                        FIXME: make sure to bump CURRENT_DATABASE_VERSION_LIBRARY and add a
               //                        case to _upgrade_library_schema_step when adding this!
               "folder VARCHAR(1024) NOT NULL)",
               NULL, NULL, NULL);
  sqlite3_exec(db->handle, "CREATE INDEX main.film_rolls_folder_index ON film_rolls (folder)", NULL, NULL, NULL);
  ////////////////////////////// maker
  sqlite3_exec(db->handle,
               "CREATE TABLE main.makers"
               " (id INTEGER PRIMARY KEY AUTOINCREMENT,"
               "  name VARCHAR)",
               NULL, NULL, NULL);
  sqlite3_exec
    (db->handle,
     "CREATE INDEX makers_name ON makers (name)",
     NULL, NULL, NULL);
  ////////////////////////////// model
  sqlite3_exec(db->handle,
               "CREATE TABLE main.models"
               " (id INTEGER PRIMARY KEY AUTOINCREMENT,"
               "  name VARCHAR)",
               NULL, NULL, NULL);
  sqlite3_exec
    (db->handle,
     "CREATE INDEX models_name ON models (name)",
     NULL, NULL, NULL);
  ////////////////////////////// lens
  sqlite3_exec(db->handle,
               "CREATE TABLE main.lens"
               " (id INTEGER PRIMARY KEY AUTOINCREMENT,"
               "  name VARCHAR)",
               NULL, NULL, NULL);
  sqlite3_exec
    (db->handle,
     "CREATE INDEX lens_name ON lens (name)",
     NULL, NULL, NULL);
  ////////////////////////////// cameras
  sqlite3_exec(db->handle,
               "CREATE TABLE cameras"
               " (id INTEGER PRIMARY KEY AUTOINCREMENT,"
               "  maker VARCHAR, model VARCHAR,"
               "  alias VARCHAR)",
               NULL, NULL, NULL);
  sqlite3_exec
    (db->handle,
     "CREATE UNIQUE INDEX cameras_name ON cameras (maker, model, alias)",
     NULL, NULL, NULL);
  ////////////////////////////// white balance
  sqlite3_exec(db->handle,
               "CREATE TABLE whitebalance"
               " (id INTEGER PRIMARY KEY AUTOINCREMENT,"
               "  name VARCHAR)",
               NULL, NULL, NULL);
  sqlite3_exec
    (db->handle,
     "CREATE UNIQUE INDEX whitebalance_name ON whitebalance (name)",
     NULL, NULL, NULL);
  ////////////////////////////// flash
  sqlite3_exec(db->handle,
               "CREATE TABLE flash"
               " (id INTEGER PRIMARY KEY AUTOINCREMENT,"
               "  name VARCHAR)",
               NULL, NULL, NULL);
  sqlite3_exec
    (db->handle,
     "CREATE UNIQUE INDEX flash_name ON flash (name)",
     NULL, NULL, NULL);
  ////////////////////////////// exposure program
  sqlite3_exec(db->handle,
               "CREATE TABLE exposure_program"
               " (id INTEGER PRIMARY KEY AUTOINCREMENT,"
               "  name VARCHAR)",
               NULL, NULL, NULL);
  sqlite3_exec
    (db->handle,
     "CREATE UNIQUE INDEX exposure_program_name ON exposure_program (name)",
     NULL, NULL, NULL);
  ////////////////////////////// metering mode
  sqlite3_exec(db->handle,
               "CREATE TABLE metering_mode"
               " (id INTEGER PRIMARY KEY AUTOINCREMENT,"
               "  name VARCHAR)",
               NULL, NULL, NULL);
  sqlite3_exec
    (db->handle,
     "CREATE UNIQUE INDEX metering_mode_name ON metering_mode (name)",
     NULL, NULL, NULL);
  ////////////////////////////// images
  sqlite3_exec(
      db->handle,
      "CREATE TABLE main.images"
      " (id INTEGER PRIMARY KEY AUTOINCREMENT, group_id INTEGER, film_id INTEGER,"
      "  width INTEGER, height INTEGER, filename VARCHAR,"
      "  maker_id INTEGER, model_id INTEGER, lens_id INTEGER, camera_id INTEGER,"
      "  exposure REAL, aperture REAL, iso REAL, focal_length REAL,"
      "  focus_distance REAL, datetime_taken INTEGER, flags INTEGER,"
      "  output_width INTEGER, output_height INTEGER, crop REAL,"
      "  raw_parameters INTEGER, raw_black INTEGER, raw_maximum INTEGER,"
      "  orientation INTEGER, longitude REAL,"
      "  latitude REAL, altitude REAL, color_matrix BLOB, colorspace INTEGER,"
      "  version INTEGER, max_version INTEGER, write_timestamp INTEGER,"
      "  history_end INTEGER, position INTEGER, aspect_ratio REAL, exposure_bias REAL,"
      "  import_timestamp INTEGER DEFAULT -1, change_timestamp INTEGER DEFAULT -1, "
      "  export_timestamp INTEGER DEFAULT -1, print_timestamp INTEGER DEFAULT -1, "
      "  thumb_timestamp INTEGER DEFAULT -1, thumb_maxmip INTEGER DEFAULT 0, "
      "  whitebalance_id INTEGER, flash_id INTEGER, "
      "  exposure_program_id INTEGER, metering_mode_id INTEGER, "
      "FOREIGN KEY(maker_id) REFERENCES makers(id) ON DELETE CASCADE ON UPDATE CASCADE, "
      "FOREIGN KEY(model_id) REFERENCES models(id) ON DELETE CASCADE ON UPDATE CASCADE, "
      "FOREIGN KEY(lens_id) REFERENCES lens(id) ON DELETE CASCADE ON UPDATE CASCADE, "
      "FOREIGN KEY(camera_id) REFERENCES cameras(id) ON DELETE CASCADE ON UPDATE CASCADE, "
      "FOREIGN KEY(film_id) REFERENCES film_rolls(id) ON DELETE CASCADE ON UPDATE CASCADE, "
      "FOREIGN KEY(group_id) REFERENCES images(id) ON DELETE RESTRICT ON UPDATE CASCADE, "
      "FOREIGN KEY(whitebalance_id) REFERENCES whitebalance(id) ON DELETE CASCADE ON UPDATE CASCADE, "
      "FOREIGN KEY(flash_id) REFERENCES flash(id) ON DELETE CASCADE ON UPDATE CASCADE, "
      "FOREIGN KEY(exposure_program_id) REFERENCES exposure_program(id) ON DELETE CASCADE ON UPDATE CASCADE, "
      "FOREIGN KEY(metering_mode_id) REFERENCES metering_mode(id) ON DELETE CASCADE ON UPDATE CASCADE)",
      NULL, NULL, NULL);
  sqlite3_exec(db->handle,
               "CREATE INDEX main.images_group_id_index ON images (group_id, id)",
               NULL, NULL, NULL);
  sqlite3_exec(db->handle,
               "CREATE INDEX main.images_film_id_index ON images (film_id, filename)",
               NULL, NULL, NULL);
  sqlite3_exec(db->handle,
               "CREATE INDEX main.images_filename_index ON images (filename, version)",
               NULL, NULL, NULL);
  sqlite3_exec(db->handle,
               "CREATE INDEX main.image_position_index ON images (position)",
               NULL, NULL, NULL);
  sqlite3_exec(db->handle,
               "CREATE INDEX main.images_datetime_taken_nc ON images (datetime_taken)",
               NULL, NULL, NULL);

  ////////////////////////////// selected_images
  sqlite3_exec(db->handle,
               "CREATE TABLE main.selected_images (num INTEGER PRIMARY KEY AUTOINCREMENT,"
               "                                   imgid INTEGER)",
               NULL, NULL, NULL);

  sqlite3_exec(db->handle,
               "CREATE UNIQUE INDEX main.selected_images_ni"
               " ON selected_images (imgid)",
               NULL, NULL, NULL);
  ////////////////////////////// history
  sqlite3_exec(
      db->handle,
      "CREATE TABLE main.history (imgid INTEGER, num INTEGER, module INTEGER, "
      "operation VARCHAR(256), op_params BLOB, enabled INTEGER, "
      "blendop_params BLOB, blendop_version INTEGER, multi_priority INTEGER, multi_name VARCHAR(256), multi_name_hand_edited INTEGER, "
      "FOREIGN KEY(imgid) REFERENCES images(id) ON UPDATE CASCADE ON DELETE CASCADE)",
      NULL, NULL, NULL);
  sqlite3_exec(db->handle, "CREATE INDEX main.history_imgid_op_index ON history (imgid, operation)", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "CREATE INDEX main.history_imgid_num_index ON history (imgid, num DESC)", NULL, NULL, NULL);
  ////////////////////////////// masks history
  sqlite3_exec(db->handle,
               "CREATE TABLE main.masks_history (imgid INTEGER, num INTEGER, formid INTEGER, form INTEGER, name VARCHAR(256), "
               "version INTEGER, points BLOB, points_count INTEGER, source BLOB, "
                "FOREIGN KEY(imgid) REFERENCES images(id) ON UPDATE CASCADE ON DELETE CASCADE)",
                 NULL, NULL, NULL);

  sqlite3_exec(db->handle,
      "CREATE INDEX main.masks_history_imgid_index ON masks_history (imgid, num)",
      NULL, NULL, NULL);

  sqlite3_exec(db->handle, "CREATE INDEX main.images_latlong_index ON images (latitude DESC, longitude DESC)",
      NULL, NULL, NULL);

  ////////////////////////////// tagged_images
  sqlite3_exec(db->handle, "CREATE TABLE main.tagged_images (imgid INTEGER, tagid INTEGER, position INTEGER, "
                           "PRIMARY KEY (imgid, tagid),"
                           "FOREIGN KEY(imgid) REFERENCES images(id) ON UPDATE CASCADE ON DELETE CASCADE)", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "CREATE INDEX main.tagged_images_tagid_index ON tagged_images (tagid)", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "CREATE INDEX main.tagged_images_position_index ON tagged_images (position)", NULL, NULL, NULL);
  ////////////////////////////// color_labels
  sqlite3_exec(db->handle, "CREATE TABLE main.color_labels (imgid INTEGER, color INTEGER)", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "CREATE UNIQUE INDEX main.color_labels_idx ON color_labels (imgid, color)", NULL, NULL,
               NULL);
  ////////////////////////////// meta_data
  sqlite3_exec(db->handle, "CREATE TABLE main.meta_data (id INTEGER, key INTEGER, value VARCHAR, "
                           "FOREIGN KEY(id) REFERENCES images(id) ON DELETE CASCADE ON UPDATE CASCADE)", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "CREATE UNIQUE INDEX main.metadata_index ON meta_data (id, key, value)", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "CREATE INDEX main.metadata_index_key ON meta_data (key)", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "CREATE INDEX main.metadata_index_value ON meta_data (value)", NULL, NULL, NULL);

  sqlite3_exec(db->handle, "CREATE TABLE main.module_order (imgid INTEGER PRIMARY KEY, version INTEGER, iop_list VARCHAR)",
               NULL, NULL, NULL);
  sqlite3_exec
    (db->handle, "CREATE TABLE main.history_hash"
     " (imgid INTEGER PRIMARY KEY,"
     "  basic_hash BLOB, auto_hash BLOB, current_hash BLOB,"
     "  mipmap_hash BLOB,"
     "  FOREIGN KEY(imgid) REFERENCES images(id) ON UPDATE CASCADE ON DELETE CASCADE)",
     NULL, NULL, NULL);

  // v34
  sqlite3_exec(db->handle, "CREATE INDEX main.images_datetime_taken_nc ON images (datetime_taken COLLATE NOCASE)",
               NULL, NULL, NULL);
  sqlite3_exec(db->handle, "CREATE INDEX main.metadata_index_key ON meta_data (key)", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "CREATE INDEX main.metadata_index_value ON meta_data (value)", NULL, NULL, NULL);

  sqlite3_exec
    (db->handle,
     "CREATE TABLE harmony_guide"
     " (imgid INTEGER PRIMARY KEY,"
     "  type INTEGER, rotation INTEGER, width INTEGER,"
     "  FOREIGN KEY(imgid) REFERENCES images(id)"
     "    ON UPDATE CASCADE ON DELETE CASCADE)",
     NULL, NULL, NULL);

  sqlite3_exec
    (db->handle,
     "CREATE TABLE overlay"
     " (imgid INTEGER, overlay_id INTEGER,"
     "  PRIMARY KEY (imgid, overlay_id),"
     "  FOREIGN KEY(imgid) REFERENCES images(id)"
     "    ON UPDATE CASCADE ON DELETE CASCADE)",
     NULL, NULL, NULL);

  // Some triggers to remove possible dangling refs in makers/models/lens/cameras
  sqlite3_exec
    (db->handle,
     "CREATE TRIGGER remove_makers AFTER DELETE ON images"
     " BEGIN"
     "  DELETE FROM makers"
     "    WHERE id = OLD.maker_id"
     "      AND NOT EXISTS (SELECT 1 FROM images WHERE maker_id = OLD.maker_id);"
     " END",
     NULL, NULL, NULL);

  sqlite3_exec
    (db->handle,
     "CREATE TRIGGER remove_models AFTER DELETE ON images"
     " BEGIN"
     "  DELETE FROM models"
     "    WHERE id = OLD.model_id"
     "      AND NOT EXISTS (SELECT 1 FROM images WHERE model_id = OLD.model_id);"
     " END",
     NULL, NULL, NULL);

  sqlite3_exec
    (db->handle,
     "CREATE TRIGGER remove_lens AFTER DELETE ON images"
     " BEGIN"
     "  DELETE FROM lens"
     "    WHERE id = OLD.lens_id"
     "      AND NOT EXISTS (SELECT 1 FROM images WHERE lens_id = OLD.lens_id);"
     " END",
     NULL, NULL, NULL);

  sqlite3_exec
    (db->handle,
     "CREATE TRIGGER remove_cameras AFTER DELETE ON images"
     " BEGIN"
     "  DELETE FROM cameras"
     "    WHERE id = OLD.camera_id"
     "      AND NOT EXISTS (SELECT 1 FROM images WHERE camera_id = OLD.camera_id);"
     " END",
     NULL, NULL, NULL);

  // Finally some views to ease walking the data

  // NOTE: datetime_taken is in nano-second since "0001-01-01 00:00:00"
  sqlite3_exec
    (db->handle,
    "CREATE VIEW v_images AS"
    " SELECT mi.id AS id, mk.name AS maker, md.name AS model, ln.name AS lens,"
    "        cm.maker || ' ' || cm.model AS normalized_camera, "
    "        cm.alias AS camera_alias,"
    "        exposure, aperture, iso,"
    "        datetime(datetime_taken/1000000"
    "                 + unixepoch('0001-01-01 00:00:00'), 'unixepoch') AS datetime,"
    "        fr.folder AS folders, filename"
    " FROM images AS mi,"
    "      makers AS mk, models AS md, lens AS ln, cameras AS cm, film_rolls AS fr"
    " WHERE mi.maker_id = mk.id"
    "   AND mi.model_id = md.id"
    "   AND mi.lens_id = ln.id"
    "   AND mi.camera_id = cm.id"
    "   AND mi.film_id = fr.id"
    " ORDER BY normalized_camera, folders",
     NULL, NULL, NULL);
  // clang-format on

  // continue by executing the upgrade steps
  _upgrade_library_schema(db, LAST_FULL_DATABASE_VERSION_LIBRARY);
}

/* create the 'data' database schema according to the state from 27 October 2024, and set the version in db_info accordingly.
 * from now on, do NOT update this function; simply add further modifications to _upgrade_data_schema for consistency. */
static void _create_data_schema(dt_database_t *db)
{
  sqlite3_stmt *stmt;
  // clang-format off
  ////////////////////////////// db_info
  sqlite3_exec
    (db->handle, "CREATE TABLE data.db_info (key VARCHAR PRIMARY KEY, value VARCHAR)",
     NULL, NULL, NULL);
  sqlite3_prepare_v2
    (db->handle, "INSERT OR REPLACE INTO data.db_info (key, value) VALUES ('version', ?1)",
     -1, &stmt, NULL);
  sqlite3_bind_int(stmt, 1, LAST_FULL_DATABASE_VERSION_DATA);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  ////////////////////////////// tags
  sqlite3_exec(db->handle, "CREATE TABLE data.tags (id INTEGER PRIMARY KEY, name VARCHAR, "
                           "synonyms VARCHAR, flags INTEGER)", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "CREATE UNIQUE INDEX data.tags_name_idx ON tags (name)", NULL, NULL, NULL);
  ////////////////////////////// styles
  sqlite3_exec(db->handle, "CREATE TABLE data.styles (id INTEGER PRIMARY KEY, name VARCHAR, description VARCHAR, iop_list VARCHAR)",
                        NULL, NULL, NULL);
  sqlite3_exec(db->handle, "CREATE INDEX data.styles_name_index ON styles (name)", NULL, NULL, NULL);
  ////////////////////////////// style_items
  sqlite3_exec(
      db->handle,
      "CREATE TABLE data.style_items (styleid INTEGER, num INTEGER, module INTEGER,"
      "                               operation VARCHAR(256), op_params BLOB, enabled INTEGER,"
      "                               blendop_params BLOB, blendop_version INTEGER,"
      "                               multi_priority INTEGER, multi_name VARCHAR(256),"
      "                               multi_name_hand_edited INTEGER)",
      NULL, NULL, NULL);
  sqlite3_exec(
      db->handle,
      "CREATE INDEX IF NOT EXISTS data.style_items_styleid_index ON style_items (styleid)",
      NULL, NULL, NULL);
  ////////////////////////////// presets
  sqlite3_exec(db->handle, "CREATE TABLE data.presets (name VARCHAR, description VARCHAR, operation "
                           "VARCHAR, op_version INTEGER, op_params BLOB, "
                           "enabled INTEGER, blendop_params BLOB, blendop_version INTEGER, "
                           "multi_priority INTEGER, multi_name VARCHAR(256), "
                           "multi_name_hand_edited INTEGER, "
                           "model VARCHAR, maker VARCHAR, lens VARCHAR, iso_min REAL, iso_max REAL, "
                           "exposure_min REAL, exposure_max REAL, "
                           "aperture_min REAL, aperture_max REAL, focal_length_min REAL, "
                           "focal_length_max REAL, writeprotect INTEGER, "
                           "autoapply INTEGER, filter INTEGER, def INTEGER, format INTEGER)",
               NULL, NULL, NULL);
  sqlite3_exec(db->handle, "CREATE UNIQUE INDEX data.presets_idx ON presets (name, operation, op_version)",
               NULL, NULL, NULL);
  ////////////////////////////// (map) locations
  sqlite3_exec(db->handle, "CREATE TABLE data.locations (tagid INTEGER PRIMARY KEY, "
               "type INTEGER, longitude REAL, latitude REAL, delta1 REAL, delta2 REAL, ratio FLOAT, polygons BLOB, "
               "FOREIGN KEY(tagid) REFERENCES tags(id))", NULL, NULL, NULL);
  // clang-format on

  /////////////////////////////////////////////////////////////////
  // DO NOT ADD ANYMORE CODE HERE, LET THE UPGRADE STEP DO THE JOB:
  // continue by executing the upgrade steps
  _upgrade_data_schema(db, LAST_FULL_DATABASE_VERSION_DATA);
}

// create the in-memory tables
// temporary stuff for some ops, need this for some reason with newer sqlite3:
static void _create_memory_schema(dt_database_t *db)
{
  // clang-format off
  sqlite3_exec(db->handle, "CREATE TABLE memory.color_labels_temp (imgid INTEGER PRIMARY KEY)", NULL, NULL, NULL);
  sqlite3_exec(
      db->handle,
      "CREATE TABLE memory.collected_images (rowid INTEGER PRIMARY KEY AUTOINCREMENT, imgid INTEGER)", NULL,
      NULL, NULL);
  sqlite3_exec(db->handle, "CREATE TABLE memory.tmp_selection (imgid INTEGER PRIMARY KEY)", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "CREATE TABLE memory.taglist "
                           "(tmpid INTEGER PRIMARY KEY, id INTEGER UNIQUE ON CONFLICT IGNORE, "
                           "count INTEGER DEFAULT 0, count2 INTEGER DEFAULT 0)",
               NULL, NULL, NULL);
  sqlite3_exec(db->handle, "CREATE TABLE memory.similar_tags (tagid INTEGER PRIMARY KEY)", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "CREATE TABLE memory.darktable_tags (tagid INTEGER PRIMARY KEY)", NULL, NULL, NULL);
  sqlite3_exec(
      db->handle,
      "CREATE TABLE memory.history (imgid INTEGER, num INTEGER, module INTEGER, "
      "operation VARCHAR(256), op_params BLOB, enabled INTEGER, "
      "blendop_params BLOB, blendop_version INTEGER, multi_priority INTEGER, multi_name VARCHAR(256), multi_name_hand_edited INTEGER, CONSTRAINT opprio UNIQUE (operation, multi_priority))",
      NULL, NULL, NULL);
  sqlite3_exec(
      db->handle,
      "CREATE TABLE memory.snapshot_history (id INTEGER, imgid INTEGER, num INTEGER, module INTEGER, "
      "operation VARCHAR(256), op_params BLOB, enabled INTEGER, "
      "blendop_params BLOB, blendop_version INTEGER, multi_priority INTEGER, multi_name VARCHAR(256), multi_name_hand_edited INTEGER)",
      NULL, NULL, NULL);
  sqlite3_exec(
      db->handle,
      "CREATE TABLE memory.snapshot_masks_history (id INTEGER, imgid INTEGER, num INTEGER, formid INTEGER,"
      " form INTEGER, name VARCHAR(256), version INTEGER, points BLOB, points_count INTEGER, source BLOB)",
      NULL, NULL, NULL);
  sqlite3_exec(
      db->handle,
      "CREATE TABLE memory.snapshot_module_order (id INTEGER, imgid INTEGER, version INTEGER, iop_list VARCHAR)",
      NULL, NULL, NULL);
  sqlite3_exec(db->handle,
      "CREATE TABLE memory.darktable_iop_names (operation VARCHAR(256) PRIMARY KEY, name VARCHAR(256))",
      NULL, NULL, NULL);
  sqlite3_exec(db->handle,
      "CREATE TABLE memory.film_folder (id INTEGER PRIMARY KEY, status INTEGER)",
      NULL, NULL, NULL);
  // clang-format on
}

static void _sanitize_db(dt_database_t *db)
{
  sqlite3_stmt *stmt, *innerstmt;
  // clang-format off
  /* first let's get rid of non-utf8 tags. */
  sqlite3_prepare_v2(db->handle, "SELECT id, name FROM data.tags", -1, &stmt, NULL);
  sqlite3_prepare_v2(db->handle, "UPDATE data.tags SET name = ?1 WHERE id = ?2", -1, &innerstmt, NULL);
  // clang-format on
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const int id = sqlite3_column_int(stmt, 0);
    const char *tag = (const char *)sqlite3_column_text(stmt, 1);

    if(tag && !g_utf8_validate(tag, -1, NULL))
    {
      gchar *new_tag = dt_util_foo_to_utf8(tag);
      dt_print(DT_DEBUG_ALWAYS,
               "[init]: tag `%s' is not valid utf8, replacing it with `%s'", tag, new_tag);
      sqlite3_bind_text(innerstmt, 1, new_tag, -1, SQLITE_TRANSIENT);
      sqlite3_bind_int(innerstmt, 2, id);
      sqlite3_step(innerstmt);
      sqlite3_reset(innerstmt);
      sqlite3_clear_bindings(innerstmt);
      g_free(new_tag);
    }
  }
  sqlite3_finalize(stmt);
  sqlite3_finalize(innerstmt);
  // clang-format off
  // make sure film_roll folders don't end in "/", that will result in empty entries in the collect module
  sqlite3_exec(db->handle,
               "UPDATE main.film_rolls SET folder = substr(folder, 1, length(folder) - 1) WHERE folder LIKE '%/'",
               NULL, NULL, NULL);
  // clang-format on
}

// in library we keep the names of the tags used in tagged_images. however, using that table at runtime results
// in some overhead not necessary so instead we just use the used_tags table to update tagged_images on startup
#define TRY_EXEC(_query, _message)                                                 \
  do                                                                               \
  {                                                                                \
    if(sqlite3_exec(db->handle, _query, NULL, NULL, NULL) != SQLITE_OK)            \
    {                                                                              \
      dt_print(DT_DEBUG_ALWAYS, "TRY_EXEC: %s, sql: %s", _message, sqlite3_errmsg(db->handle)); \
      FINALIZE;                                                                    \
      sqlite3_exec(db->handle, "ROLLBACK TRANSACTION", NULL, NULL, NULL);          \
      return FALSE;                                                                \
    }                                                                              \
  } while(0)

#define TRY_STEP(_stmt, _expected, _message)                                       \
  do                                                                               \
  {                                                                                \
    if(sqlite3_step(_stmt) != _expected)                                           \
    {                                                                              \
      dt_print(DT_DEBUG_ALWAYS, "TRY_STEP: %s, sql: %s", _message, sqlite3_errmsg(db->handle)); \
      FINALIZE;                                                                    \
      sqlite3_exec(db->handle, "ROLLBACK TRANSACTION", NULL, NULL, NULL);          \
      return FALSE;                                                                \
    }                                                                              \
  } while(0)

#define TRY_PREPARE(_stmt, _query, _message)                                       \
  do                                                                               \
  {                                                                                \
    if(sqlite3_prepare_v2(db->handle, _query, -1, &_stmt, NULL) != SQLITE_OK)      \
    {                                                                              \
      dt_print(DT_DEBUG_ALWAYS, "TRY_PREPARE: %s, sql: %s", _message, sqlite3_errmsg(db->handle)); \
      FINALIZE;                                                                    \
      sqlite3_exec(db->handle, "ROLLBACK TRANSACTION", NULL, NULL, NULL);          \
      return FALSE;                                                                \
    }                                                                              \
  } while(0)

#define FINALIZE                                                                   \
  do                                                                               \
  {                                                                                \
    sqlite3_finalize(stmt); stmt = NULL; /* NULL so that finalize becomes a NOP */ \
  } while(0)

#undef TRY_EXEC
#undef TRY_STEP
#undef TRY_PREPARE
#undef FINALIZE

void dt_database_show_error(const dt_database_t *db)
{
  if(!db->lock_acquired)
  {
    char lck_pathname[1024];
    snprintf(lck_pathname, sizeof(lck_pathname), "%s.lock", db->error_dbfilename);
    char *lck_dirname = g_strdup(lck_pathname);
    char *last_dirsep_position = g_strrstr(lck_dirname, G_DIR_SEPARATOR_S);
    if(last_dirsep_position)
      *last_dirsep_position = '\0';  // make lck_dirname contain only the directory name

    // clang-format off
    char *label_text = g_markup_printf_escaped(
        _("\n"
          "  Sorry, darktable could not be started (database is locked)\n"
          "\n"
          "  How to solve this problem?\n"
          "\n"
          "  1 - If another darktable instance is already open, \n"
          "      click cancel and either use that instance or close it before attempting to rerun darktable \n"
          "      (process ID <i><b>%d</b></i> created the database locks)\n"
          "\n"
          "  2 - If you closed darktable within the past few minutes, it may still be running in the background \n"
          "      to export images, update sidecar files, or perform database maintenance. Try again once \n"
          "      this processing finishes.\n"
          "\n"
          "  3 - If you are not confident in your ability to correctly deal with processes in the OS, \n"
          "      it would be safer to restart the session or reboot your computer after some time (few minutes). \n"
          "      This will close all running programs and hopefully close the databases correctly. \n"
          "\n"
          "  4 - If you have done this or are certain that no other instances of darktable are running, \n"
          "      this probably means that the last instance was ended abnormally. \n"
          "      Click on the \"delete database lock files\" button to delete the files <i>data.db.lock</i> and <i>library.db.lock</i>. \n"
          "\n\n"
          "      <i><u>Caution!</u> Do not delete these files without first undertaking the above checks, \n"
          "      otherwise you risk generating serious inconsistencies in your database.</i>\n"),
      db->error_other_pid);
    // clang-format on

    gboolean delete_lockfiles = dt_gui_show_standalone_yes_no_dialog(_("error starting darktable"),
                                        label_text, _("_cancel"), _("_delete database lock files"));

    if(delete_lockfiles)
    {
      gboolean really_delete_lockfiles =
        dt_gui_show_standalone_yes_no_dialog
        (_("are you sure?"),
         _("\ndo you really want to delete the lock files?\n"), _("_no"), _("_yes"));
      if(really_delete_lockfiles)
      {
        int status = 0;

        char *lck_filename = g_strconcat(lck_dirname, "/data.db.lock", NULL);
        if(g_access(lck_filename, F_OK) != -1)
          status += remove(lck_filename);
        g_free(lck_filename);

        lck_filename = g_strconcat(lck_dirname, "/library.db.lock", NULL);
        if(g_access(lck_filename, F_OK) != -1)
          status += remove(lck_filename);
        g_free(lck_filename);

        if(status==0)
          dt_gui_show_standalone_yes_no_dialog(_("done"),
                                        _("\nsuccessfully deleted the lock files.\nyou can now restart darktable\n"),
                                        _("_ok"), NULL);
        else
          dt_gui_show_standalone_yes_no_dialog
            (_("error"), g_markup_printf_escaped(
              _("\nat least one file could not be deleted.\n"
                "you may try to manually delete the files <i>data.db.lock</i> and <i>library.db.lock</i>\n"
                "in folder <a href=\"file:///%s\">%s</a>.\n"), lck_dirname, lck_dirname),
             _("_ok"), NULL);
      }
    }

    g_free(lck_dirname);
    g_free(label_text);
  }

  g_free(db->error_message);
  g_free(db->error_dbfilename);
  ((dt_database_t *)db)->error_other_pid = 0;
  ((dt_database_t *)db)->error_message = NULL;
  ((dt_database_t *)db)->error_dbfilename = NULL;
}

static gboolean pid_is_alive(int pid)
{
  gboolean pid_is_alive;

#ifdef _WIN32
  pid_is_alive = FALSE;
  HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
  if(h)
  {
    wchar_t wfilename[MAX_PATH];
    long unsigned int n_filename = sizeof(wfilename);
    int ret = QueryFullProcessImageNameW(h, 0, wfilename, &n_filename);
    char *filename = g_utf16_to_utf8(wfilename, -1, NULL, NULL, NULL);
    if(ret && n_filename > 0 && filename && g_str_has_suffix(filename, "darktable.exe"))
      pid_is_alive = TRUE;
    g_free(filename);
    CloseHandle(h);
  }
#else
  pid_is_alive = !((kill(pid, 0) == -1) && errno == ESRCH);

#ifdef __linux__
  // If this is Linux, we can query /proc to see if the pid is
  // actually a darktable instance.
  if(pid_is_alive)
  {
    gchar *contents;
    gsize length;
    gchar filename[64];
    snprintf(filename, sizeof(filename), "/proc/%d/cmdline", pid);

    if(g_file_get_contents("", &contents, &length, NULL))
    {
      if(strstr(contents, "darktable") == NULL)
      {
        pid_is_alive = FALSE;
      }
      g_free(contents);
    }
  }
#endif

#endif

  return pid_is_alive;
}

static gboolean _lock_single_database(dt_database_t *db, const char *dbfilename, char **lockfile)
{
  gboolean lock_acquired = FALSE;
  mode_t old_mode;
  int lock_tries = 0;
  gchar *pid = g_strdup_printf("%d", getpid());

  if(!strcmp(dbfilename, ":memory:"))
  {
    lock_acquired = TRUE;
  }
  else
  {
    *lockfile = g_strconcat(dbfilename, ".lock", NULL);
lock_again:
    lock_tries++;
    old_mode = umask(0);
    int fd = g_open(*lockfile, O_RDWR | O_CREAT | O_EXCL, 0666);
    umask(old_mode);

    if(fd != -1) // the lockfile was successfully created - write our PID into it
    {
      if(write(fd, pid, strlen(pid) + 1) > -1) lock_acquired = TRUE;
      close(fd);
    }
    else // the lockfile already exists - see if it's a stale one left over from a crashed instance
    {
      char buf[64];
      memset(buf, 0, sizeof(buf));
      fd = g_open(*lockfile, O_RDWR | O_CREAT, 0666);
      if(fd != -1)
      {
        int foo;
        if((foo = read(fd, buf, sizeof(buf) - 1)) > 0)
        {
          db->error_other_pid = atoi(buf);
          if(!pid_is_alive(db->error_other_pid))
          {
            // the other process seems to no longer exist. unlink the .lock file and try again
            g_unlink(*lockfile);
            if(lock_tries < 5)
            {
              close(fd);
              goto lock_again;
            }
          }
          else
          {
            dt_print(DT_DEBUG_ALWAYS,
                     "[init] the database lock file contains a pid that seems to be alive in your system: %d",
                     db->error_other_pid);
            db->error_message
              = g_strdup_printf(_("the database lock file contains a pid that seems to be alive in your system: %d"),
                                db->error_other_pid);
          }
        }
        else
        {
          dt_print(DT_DEBUG_ALWAYS, "[init] the database lock file seems to be empty");
          db->error_message = g_strdup_printf(_("the database lock file seems to be empty"));
        }
        close(fd);
      }
      else
      {
        int err = errno;
        dt_print(DT_DEBUG_ALWAYS,
                 "[init] error opening the database lock file for reading: %s",
                 strerror(err));
        db->error_message = g_strdup_printf(_("error opening the database lock file for reading: %s"), strerror(err));
      }
    }
  }

  g_free(pid);

  if(db->error_message)
    db->error_dbfilename = g_strdup(dbfilename);

  return lock_acquired;
}

static gboolean _lock_databases(dt_database_t *db)
{
  if(!_lock_single_database(db, db->dbfilename_data, &db->lockfile_data))
    return FALSE;
  if(!_lock_single_database(db, db->dbfilename_library, &db->lockfile_library))
  {
    // unlock data.db to not leave a stale lock file around
    g_unlink(db->lockfile_data);
    return FALSE;
  }
  return TRUE;
}

static gboolean _upgrade_camera_table(const dt_database_t *db)
{
  gboolean res = TRUE;
  sqlite3_stmt *stmt;
  sqlite3_stmt *innerstmt;

  sqlite3_prepare_v2(db->handle,
                     "SELECT mi.id, mk.name, md.name"
                     " FROM main.images AS mi, main.makers AS mk, main.models AS md"
                     " WHERE mi.maker_id = mk.id"
                     "   AND mi.model_id = md.id",
                     -1, &stmt, NULL);

  sqlite3_prepare_v2(db->handle,
                     "UPDATE main.images SET camera_id = ?1 WHERE id = ?2",
                     -1, &innerstmt, NULL);

  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const dt_imgid_t imgid = sqlite3_column_int(stmt, 0);
    const char *maker = (const char *)sqlite3_column_text(stmt, 1);
    const char *model = (const char *)sqlite3_column_text(stmt, 2);

    const int32_t camera_id = dt_image_get_camera_id(maker, model);

    sqlite3_bind_int(innerstmt, 1, camera_id);
    sqlite3_bind_int(innerstmt, 2, imgid);
    sqlite3_step(innerstmt);
    sqlite3_reset(innerstmt);
    sqlite3_clear_bindings(innerstmt);
  }

  sqlite3_finalize(stmt);
  sqlite3_finalize(innerstmt);

  return res;
}

static void _too_new_db_version(const gchar *dbname, const gboolean has_gui)
{
  if(!has_gui)
    exit(1);

  char *label_text = g_markup_printf_escaped
    (_("the database schema version of\n"
       "\n"
       "<span style='italic'>%s</span>\n"
       "\n"
       "is too new for this build of darktable "
       "(this means the database was created or upgraded by a newer darktable version)\n"),
       dbname);
  dt_gui_show_standalone_yes_no_dialog(_("darktable - too new db version"), label_text,
                                         _("_quit darktable"), NULL);
  g_free(label_text);
}

static void _ask_for_upgrade(const gchar *dbname, const gboolean has_gui)
{
  // if there's no gui just leave
  if(!has_gui)
  {
    dt_print(DT_DEBUG_ALWAYS, "[init] database `%s' is out-of-date. aborting", dbname);
    exit(1);
  }

  // the database has to be upgraded, let's ask user

  char *label_text = g_markup_printf_escaped(_("the database schema has to be upgraded for\n"
                                               "\n"
                                               "<span style='italic'>%s</span>\n"
                                               "\nthis might take a long time in case of a large database\n\n"
                                               "do you want to proceed or quit now to do a backup\n"),
                                               dbname);

  gboolean shall_we_update_the_db =
    dt_gui_show_standalone_yes_no_dialog(_("darktable - schema migration"), label_text,
                                         _("_close darktable"), _("_upgrade database"));

  g_free(label_text);

  // if no upgrade, we exit now, nothing we can do more
  if(!shall_we_update_the_db)
  {
    dt_print(DT_DEBUG_ALWAYS, "[init] we shall not update the database, aborting");
    exit(1);
  }
}

void dt_database_backup(const char *filename)
{
  char *version = g_strdup(darktable_package_version);
  int k = 0;
  // get plain version (no commit id)
  while(version[k])
  {
    if((version[k] < '0' || version[k] > '9') && (version[k] != '.'))
    {
      version[k] = '\0';
      break;
    }
    k++;
  }

  gchar *backup = g_strdup_printf("%s-pre-%s", filename, version);

  GError *gerror = NULL;
  if(!g_file_test(backup, G_FILE_TEST_EXISTS))
  {
    GFile *src = g_file_new_for_path(filename);
    GFile *dest = g_file_new_for_path(backup);
    gboolean copy_status = TRUE;
    if(g_file_test(filename, G_FILE_TEST_EXISTS))
    {
      copy_status = g_file_copy(src, dest, G_FILE_COPY_NONE, NULL, NULL, NULL, &gerror);
      if(copy_status) copy_status = g_chmod(backup, S_IRUSR) == 0;
    }
    else
    {
      // there is nothing to backup, create an empty file to prevent further backup attempts
      int fd = g_open(backup, O_CREAT, S_IRUSR);
      if(fd < 0 || !g_close(fd, &gerror)) copy_status = FALSE;
    }
    if(!copy_status) dt_print(DT_DEBUG_ALWAYS, "[backup failed] %s -> %s", filename, backup);

    g_object_unref(src);
    g_object_unref(dest);
  }

  g_free(version);
  g_free(backup);
}

int _get_pragma_int_val(sqlite3 *db, const char* pragma)
{
  gchar* query= g_strdup_printf("PRAGMA %s", pragma);
  int val = -1;
  sqlite3_stmt *stmt;
  const int rc = sqlite3_prepare_v2(db, query,-1, &stmt, NULL);
  if(rc == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW)
  {
    val = sqlite3_column_int(stmt, 0);
  }
  sqlite3_finalize(stmt);
  g_free(query);

  return val;
}

gchar* _get_pragma_string_val(sqlite3 *db, const char* pragma)
{
  gchar* query= g_strdup_printf("PRAGMA %s", pragma);
  sqlite3_stmt *stmt;
  gchar* val = NULL;
  const int rc = sqlite3_prepare_v2(db, query,-1, &stmt, NULL);
  if(rc == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW)
  {
    val = g_strdup((const char *)sqlite3_column_text(stmt, 0));
    while(sqlite3_step(stmt) == SQLITE_ROW)
    {
      gchar* cur_val = g_strdup((const char *)sqlite3_column_text(stmt, 0));
      gchar* tmp_val = g_strdup(val);
      g_free(val);
      val = g_strconcat(tmp_val, "\n", cur_val, NULL);
      g_free(cur_val);
      g_free(tmp_val);
    }
  }
  sqlite3_finalize(stmt);
  g_free(query);
  return val;
}

dt_database_t *dt_database_init(const char *alternative, const gboolean load_data, const gboolean has_gui)
{
  /*  set the threading mode to Serialized */
  sqlite3_config(SQLITE_CONFIG_SERIALIZED);

  sqlite3_initialize();

start:
  if(alternative == NULL)
  {
    /* migrate default database location to new default */
    _database_migrate_to_xdg_structure();
  }

  /* delete old mipmaps files */
  _database_delete_mipmaps_files();

  /* lets construct the db filename  */
  gchar *dbname = NULL;
  gchar dbfilename_library[PATH_MAX] = { 0 };
  gchar datadir[PATH_MAX] = { 0 };

  dt_loc_get_user_config_dir(datadir, sizeof(datadir));

  if(alternative == NULL)
  {
    dbname = dt_conf_get_string("database");
    if(!dbname)
      snprintf(dbfilename_library, sizeof(dbfilename_library), "%s%slibrary.db", datadir, G_DIR_SEPARATOR_S);
    else if(!strcmp(dbname, ":memory:"))
      g_strlcpy(dbfilename_library, dbname, sizeof(dbfilename_library));
    else if(dbname[0] != '/')
      snprintf(dbfilename_library, sizeof(dbfilename_library), "%s%s%s", datadir, G_DIR_SEPARATOR_S, dbname);
    else
      g_strlcpy(dbfilename_library, dbname, sizeof(dbfilename_library));
  }
  else
  {
    g_strlcpy(dbfilename_library, alternative, sizeof(dbfilename_library));

    GFile *galternative = g_file_new_for_path(alternative);
    dbname = g_file_get_basename(galternative);
    g_object_unref(galternative);
  }

  /* we also need a 2nd db with permanent data like presets, styles and tags */
  char dbfilename_data[PATH_MAX] = { 0 };
  if(load_data)
    snprintf(dbfilename_data, sizeof(dbfilename_data), "%s%sdata.db", datadir, G_DIR_SEPARATOR_S);
  else
    snprintf(dbfilename_data, sizeof(dbfilename_data), ":memory:");

  /* create database */
  dt_database_t *db = g_malloc0(sizeof(dt_database_t));
  db->dbfilename_data = g_strdup(dbfilename_data);
  db->dbfilename_library = g_strdup(dbfilename_library);

  dt_atomic_set_int(&_trxid, 0);

  /* make sure the folder exists. this might not be the case for new databases */
  /* also check if a database backup is needed */
  if(g_strcmp0(dbfilename_data, ":memory:"))
  {
    char *data_path = g_path_get_dirname(dbfilename_data);
    g_mkdir_with_parents(data_path, 0750);
    g_free(data_path);
    dt_database_backup(dbfilename_data);
  }
  if(g_strcmp0(dbfilename_library, ":memory:"))
  {
    char *library_path = g_path_get_dirname(dbfilename_library);
    g_mkdir_with_parents(library_path, 0750);
    g_free(library_path);
    dt_database_backup(dbfilename_library);
  }

  dt_print(DT_DEBUG_SQL, "[init sql] library: %s, data: %s",
           dbfilename_library, dbfilename_data);

  /* having more than one instance of darktable using the same database is a bad idea */
  /* try to get locks for the databases */
  db->lock_acquired = _lock_databases(db);

  if(!db->lock_acquired)
  {
    dt_print(DT_DEBUG_ALWAYS,
             "[init] database is locked, probably another process is already using it");
    g_free(dbname);
    return db;
  }


  /* opening / creating database */
  if(sqlite3_open(db->dbfilename_library, &db->handle))
  {
    dt_print(DT_DEBUG_ALWAYS, "[init] could not find database %s%s%s",
                              dbname ? " `" : "", dbname ? dbname : "", dbname ? "'!" : "");
    dt_print(DT_DEBUG_ALWAYS, "[init] maybe your %s/darktablerc is corrupt?", datadir);
    dt_loc_get_datadir(dbfilename_library, sizeof(dbfilename_library));
    dt_print(DT_DEBUG_ALWAYS, "[init] try `cp %s/darktablerc %s/darktablerc'",
             dbfilename_library, datadir);
    sqlite3_close(db->handle);
    g_free(dbname);
    g_free(db->lockfile_data);
    g_free(db->dbfilename_data);
    g_free(db->lockfile_library);
    g_free(db->dbfilename_library);
    g_free(db);
    return NULL;
  }

  /* attach a memory database to db connection for use with temporary tables
     used during instance life time, which is discarded on exit.
  */
  sqlite3_exec(db->handle, "attach database ':memory:' as memory", NULL, NULL, NULL);

  // attach the data database which contains presets, styles, tags and similar things not tied to single images
  sqlite3_stmt *stmt;
  gboolean have_data_db = load_data && g_file_test(dbfilename_data, G_FILE_TEST_EXISTS);
  int rc = sqlite3_prepare_v2(db->handle, "ATTACH DATABASE ?1 AS data", -1, &stmt, NULL);
  sqlite3_bind_text(stmt, 1, dbfilename_data, -1, SQLITE_TRANSIENT);
  if(rc != SQLITE_OK || sqlite3_step(stmt) != SQLITE_DONE)
  {
    sqlite3_finalize(stmt);
    dt_print(DT_DEBUG_ALWAYS, "[init] database `%s' couldn't be opened. aborting",
             dbfilename_data);
    dt_database_destroy(db);
    db = NULL;
    goto error;
  }
  sqlite3_finalize(stmt);

  // some sqlite3 config
  sqlite3_exec(db->handle, "PRAGMA synchronous = OFF", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "PRAGMA journal_mode = MEMORY", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "PRAGMA page_size = 32768", NULL, NULL, NULL);

  // WARNING: the foreign_keys pragma must not be used, the integrity of the
  // database rely on it.
  sqlite3_exec(db->handle, "PRAGMA foreign_keys = ON", NULL, NULL, NULL);

  /* now that we got functional databases that are locked for us we can make sure that the schema is set up */

  // first we update the data database to the latest version so that we can potentially move data from the library
  // over when updating that one
  if(!have_data_db)
  {
    _create_data_schema(db); // a brand new db it seems
  }
  else
  {
    gchar* data_status = _get_pragma_string_val(db->handle, "data.quick_check");
    rc = sqlite3_prepare_v2(db->handle,
                            "SELECT value FROM data.db_info WHERE key = 'version'",
                            -1, &stmt, NULL);
    if(!g_strcmp0(data_status, "ok") && rc == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW)
    {
      g_free(data_status); // status is OK and we don't need to care :)
      // compare the version of the db with what is current for this executable
      const int db_version = sqlite3_column_int(stmt, 0);
      sqlite3_finalize(stmt);
      if(db_version < CURRENT_DATABASE_VERSION_DATA)
      {
        _ask_for_upgrade(dbfilename_data, has_gui);

        // older: upgrade
        if(!_upgrade_data_schema(db, db_version))
        {
          // we couldn't upgrade the db for some reason. bail out.
          dt_print(DT_DEBUG_ALWAYS,
                   "[init] database `%s' couldn't be upgraded from version %d to %d. aborting",
                   dbfilename_data, db_version, CURRENT_DATABASE_VERSION_DATA);
          dt_database_destroy(db);
          db = NULL;
          goto error;
        }

        // upgrade was successful, time for some housekeeping
        sqlite3_exec(db->handle, "VACUUM data", NULL, NULL, NULL);
        sqlite3_exec(db->handle, "ANALYZE data", NULL, NULL, NULL);

      }
      else if(db_version > CURRENT_DATABASE_VERSION_DATA)
      {
        // newer: bail out
        _too_new_db_version(dbfilename_data, has_gui);
        dt_print(DT_DEBUG_ALWAYS,
                 "[init] database version of `%s' is too new for this build of darktable. aborting",
                 dbfilename_data);
        dt_database_destroy(db);
        db = NULL;
        goto error;
      }
      // else: the current version, do nothing
    }
    else
    {
      // oh, bad situation. the database is corrupt and can't be read!
      // we inform the user here and let him decide what to do: exit or delete and try again.

      gchar* quick_check_text = NULL;
      if(g_strcmp0(data_status, "ok")) // data_status is not ok
      {
        quick_check_text = g_strdup_printf(_("quick_check said:\n"
                                            "%s \n"), data_status);
      }
      else
      {
        quick_check_text = g_strdup(""); // a trick;
      }

      gchar *data_snap = dt_database_get_most_recent_snap(dbfilename_data);

      GtkWidget *dialog;
      GtkDialogFlags dflags = GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT;

      const char* label_options = NULL;

      if(data_snap)
      {
        dialog = gtk_dialog_new_with_buttons(_("darktable - error opening database"),
                                            NULL,
                                            dflags,
                                            _("_close darktable"), GTK_RESPONSE_CLOSE,
                                            _("_attempt restore"), GTK_RESPONSE_ACCEPT,
                                            _("_delete database"), GTK_RESPONSE_REJECT,
                                            NULL);
        gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
        label_options = _("do you want to close darktable now to manually restore\n"
                          "the database from a backup, attempt an automatic restore\n"
                          "from the most recent snapshot or delete the corrupted database\n"
                          "and start with a new one?");
      }
      else
      {
        dialog = gtk_dialog_new_with_buttons(_("darktable - error opening database"),
                                            NULL,
                                            dflags,
                                            _("_close darktable"), GTK_RESPONSE_CLOSE,
                                            _("_delete database"), GTK_RESPONSE_REJECT,
                                            NULL);
        gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_CLOSE);
        label_options = _("do you want to close darktable now to manually restore\n"
                          "the database from a backup or delete the corrupted database\n"
                          "and start with a new one?");
      }



      char *label_text = g_markup_printf_escaped(_("an error has occurred while trying to open the database from\n"
                                                   "\n"
                                                   "<span style='italic'>%s</span>\n"
                                                   "\n"
                                                   "it seems that the database is corrupted.\n"
                                                   "%s"
                                                   "%s"),
                                                 dbfilename_data, quick_check_text, label_options);

      g_free(quick_check_text);
      g_free(data_status);

      GtkWidget *content_area = gtk_dialog_get_content_area(GTK_DIALOG (dialog));
      GtkWidget *label = gtk_label_new(NULL);
      gtk_label_set_markup(GTK_LABEL(label), label_text);
      g_free(label_text);
      gtk_container_add(GTK_CONTAINER (content_area), label);

      gtk_widget_show_all(content_area);

      const int resp = gtk_dialog_run(GTK_DIALOG(dialog));

      gtk_widget_destroy(dialog);

      dt_database_destroy(db);
      db = NULL;

      if(resp != GTK_RESPONSE_ACCEPT && resp != GTK_RESPONSE_REJECT)
      {
        dt_print(DT_DEBUG_ALWAYS,
                 "[init] database `%s' is corrupt and can't be opened! either replace it from a backup or "
                 "delete the file so that darktable can create a new one the next time. aborting",
                 dbfilename_data);
        g_free(data_snap);
        goto error;
      }

      //here were sure that response is either accept (restore from snap) or reject (just delete the damaged db)
      dt_print(DT_DEBUG_ALWAYS, "[init] deleting `%s' on user request: %s",
        dbfilename_data, g_unlink(dbfilename_data) == 0 ? "ok" : "failed" );

      if(resp == GTK_RESPONSE_ACCEPT && data_snap)
      {
        GError *gerror = NULL;
        if(!g_file_test(dbfilename_data, G_FILE_TEST_EXISTS))
        {
          GFile *src = g_file_new_for_path(data_snap);
          GFile *dest = g_file_new_for_path(dbfilename_data);
          gboolean copy_status = TRUE;
          if(g_file_test(data_snap, G_FILE_TEST_EXISTS))
          {
            copy_status = g_file_copy(src, dest, G_FILE_COPY_NONE, NULL, NULL, NULL, &gerror);
            if(copy_status) copy_status = g_chmod(dbfilename_data, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH) == 0;
          }
          else
          {
            // there is nothing to restore, create an empty file
            const int fd = g_open(dbfilename_data, O_CREAT, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
            if(fd < 0 || !g_close(fd, &gerror)) copy_status = FALSE;
          }
          dt_print(DT_DEBUG_ALWAYS, "[init] restoring `%s' from `%s' :%s",
                 dbfilename_data, data_snap, copy_status ? "success!" : "failed!");
          g_object_unref(src);
          g_object_unref(dest);
        }
      }
      g_free(data_snap);
      g_free(dbname);
      goto start;
    }
  }

  gchar* libdb_status = _get_pragma_string_val(db->handle, "main.quick_check");
  // next we are looking at the library database
  // does the db contain the new 'db_info' table?
  rc = sqlite3_prepare_v2(db->handle,
                          "SELECT value FROM main.db_info WHERE key = 'version'",
                          -1, &stmt, NULL);
  if(!g_strcmp0(libdb_status, "ok") && rc == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW)
  {
    g_free(libdb_status);//it's ok :)

    // compare the version of the db with what is current for this executable
    const int db_version = sqlite3_column_int(stmt, 0);

    sqlite3_finalize(stmt);
    if(db_version < CURRENT_DATABASE_VERSION_LIBRARY)
    {
      _ask_for_upgrade(dbfilename_library, has_gui);

      // older: upgrade
      if(!_upgrade_library_schema(db, db_version))
      {
        // we couldn't upgrade the db for some reason. bail out.
        dt_print(DT_DEBUG_ALWAYS,
                 "[init] database `%s' couldn't be upgraded from version %d to %d. aborting",
                 dbname, db_version, CURRENT_DATABASE_VERSION_LIBRARY);
        dt_database_destroy(db);
        db = NULL;
        goto error;
      }

      // upgrade was successful, time for some housekeeping
      sqlite3_exec(db->handle, "VACUUM main", NULL, NULL, NULL);
      sqlite3_exec(db->handle, "ANALYZE main", NULL, NULL, NULL);
    }
    else if(db_version > CURRENT_DATABASE_VERSION_LIBRARY)
    {
      // newer: bail out. it's better than what we did before: delete everything
      _too_new_db_version(dbfilename_library, has_gui);
      dt_print(DT_DEBUG_ALWAYS,
               "[init] database version of `%s' is too new for this build of darktable. aborting",
               dbname);
      dt_database_destroy(db);
      db = NULL;
      goto error;
    }
    // else: the current version, do nothing
  }
  else if(g_strcmp0(libdb_status, "ok") || rc == SQLITE_CORRUPT || rc == SQLITE_NOTADB)
  {
    // oh, bad situation. the database is corrupt and can't be read!
    // we inform the user here and let him decide what to do: exit or delete and try again.

    gchar* quick_check_text = NULL;
    if(g_strcmp0(libdb_status, "ok")) // data_status is not ok
    {
      quick_check_text = g_strdup_printf(_("quick_check said:\n"
                                          "%s \n"), libdb_status);
    }
    else
    {
      quick_check_text = g_strdup(""); // a trick;
    }

    gchar *data_snap = dt_database_get_most_recent_snap(dbfilename_library);

    GtkWidget *dialog;
    GtkDialogFlags dflags = GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT;

    const char* label_options = NULL;

    if(data_snap)
    {
      dialog = gtk_dialog_new_with_buttons(_("darktable - error opening database"),
                                          NULL,
                                          dflags,
                                          _("_close darktable"), GTK_RESPONSE_CLOSE,
                                          _("_attempt restore"), GTK_RESPONSE_ACCEPT,
                                          _("_delete database"), GTK_RESPONSE_REJECT,
                                          NULL);
      gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
      label_options = _("do you want to close darktable now to manually restore\n"
                        "the database from a backup, attempt an automatic restore\n"
                        "from the most recent snapshot or delete the corrupted database\n"
                        "and start with a new one?");
    }
    else
    {
      dialog = gtk_dialog_new_with_buttons(_("darktable - error opening database"),
                                          NULL,
                                          dflags,
                                          _("_close darktable"), GTK_RESPONSE_CLOSE,
                                          _("_delete database"), GTK_RESPONSE_REJECT,
                                          NULL);
      gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_CLOSE);
      label_options = _("do you want to close darktable now to manually restore\n"
                        "the database from a backup or delete the corrupted database\n"
                        "and start with a new one?");
    }

    char *label_text = g_markup_printf_escaped(_("an error has occurred while trying to open the database from\n"
                                                 "\n"
                                                 "<span style='italic'>%s</span>\n"
                                                 "\n"
                                                 "it seems that the database is corrupted.\n"
                                                 "%s"
                                                 "%s"),
                                               dbfilename_data, quick_check_text, label_options);

    g_free(quick_check_text);
    g_free(libdb_status);

    GtkWidget *content_area = gtk_dialog_get_content_area(GTK_DIALOG (dialog));
    GtkWidget *label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(label), label_text);
    g_free(label_text);
    gtk_container_add(GTK_CONTAINER (content_area), label);

    gtk_widget_show_all(content_area);

    const int resp = gtk_dialog_run(GTK_DIALOG(dialog));

    gtk_widget_destroy(dialog);

    dt_database_destroy(db);
    db = NULL;

    if(resp != GTK_RESPONSE_ACCEPT && resp != GTK_RESPONSE_REJECT)
    {
      dt_print(DT_DEBUG_ALWAYS,
               "[init] database `%s' is corrupt and can't be opened! either replace it from a backup or "
               "delete the file so that darktable can create a new one the next time. aborting",
               dbfilename_library);
      g_free(data_snap);
      goto error;
    }

    //here were sure that response is either accept (restore from snap) or reject (just delete the damaged db)

    dt_print(DT_DEBUG_ALWAYS, "[init] deleting `%s' on user request ...%s",
      dbfilename_library, g_unlink(dbfilename_library) == 0 ? "OK" : "failed");

    if(resp == GTK_RESPONSE_ACCEPT && data_snap)
    {
      GError *gerror = NULL;
      if(!g_file_test(dbfilename_library, G_FILE_TEST_EXISTS))
      {
        GFile *src = g_file_new_for_path(data_snap);
        GFile *dest = g_file_new_for_path(dbfilename_library);
        gboolean copy_status = TRUE;
        if(g_file_test(data_snap, G_FILE_TEST_EXISTS))
        {
          copy_status = g_file_copy(src, dest, G_FILE_COPY_NONE, NULL, NULL, NULL, &gerror);
          if(copy_status) copy_status = g_chmod(dbfilename_library, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH) == 0;
        }
        else
        {
          // there is nothing to restore, create an empty file to prevent further backup attempts
          const int fd = g_open(dbfilename_library, O_CREAT, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
          if(fd < 0 || !g_close(fd, &gerror)) copy_status = FALSE;
        }
        dt_print(DT_DEBUG_ALWAYS, "[init] restoring `%s' from `%s'... %s",
               dbfilename_library, data_snap, copy_status ? "success" : "failed");
        g_object_unref(src);
        g_object_unref(dest);
      }
    }
    g_free(data_snap);
    g_free(dbname);
    goto start;
  }
  else
  {
    // does it contain the legacy 'settings' table?
    sqlite3_finalize(stmt);
    rc = sqlite3_prepare_v2(db->handle, "SELECT settings FROM main.settings", -1, &stmt, NULL);
    if(rc == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW)
    {
      // the old blob had the version as an int in the first place
      const void *set = sqlite3_column_blob(stmt, 0);
      const int db_version = *(int *)set;
      sqlite3_finalize(stmt);
      if(!_migrate_schema(db, db_version)) // bring the legacy layout to the first one known
                                           // to our upgrade path ...
      {
        // we couldn't migrate the db for some reason. bail out.
        dt_print(DT_DEBUG_ALWAYS,
                 "[init] database `%s' couldn't be migrated from the legacy version %d. aborting",
                 dbname, db_version);
        dt_database_destroy(db);
        db = NULL;
        goto error;
      }
      if(!_upgrade_library_schema(db, 1)) // ... and upgrade it
      {
        // we couldn't upgrade the db for some reason. bail out.
        dt_print(DT_DEBUG_ALWAYS,
                 "[init] database `%s' couldn't be upgraded from version 1 to %d. aborting",
                 dbname, CURRENT_DATABASE_VERSION_LIBRARY);
        dt_database_destroy(db);
        db = NULL;
        goto error;
      }
    }
    else
    {
      sqlite3_finalize(stmt);
      _create_library_schema(db); // a brand new db it seems
    }
  }

  // create the in-memory tables
  _create_memory_schema(db);

  // drop table settings -- we don't want old versions of dt to drop our tables
  sqlite3_exec(db->handle, "DROP TABLE main.settings", NULL, NULL, NULL);

  // take care of potential bad data in the db.
  _sanitize_db(db);

#ifdef HAVE_ICU
  // check if sqlite is already icu enabled
  // if not enabled expected error: no such function:icu_load_collation
  rc = sqlite3_prepare_v2(db->handle,
                          "SELECT icu_load_collation('en_US', 'english')",
                          -1, &stmt, NULL);
  sqlite3_finalize(stmt);

  if(rc != SQLITE_OK)
  {
    rc = sqlite3IcuInit(db->handle);
    if(rc != SQLITE_OK)
      dt_print(DT_DEBUG_ALWAYS, "[sqlite] init icu extension error %d", rc);
  }
#endif

error:
  g_free(dbname);

  return db;
}

void dt_upgrade_maker_model(const dt_database_t *db)
{
  sqlite3_stmt *stmt;

  // check if updating the camera table is needed (done for each new darktable version)

  sqlite3_prepare_v2(db->handle,
                     "SELECT value"
                     " FROM main.db_info"
                     " WHERE key = 'dt_version'",
                     -1, &stmt, NULL);
  char *dt_version = NULL;
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    dt_version = (char *)sqlite3_column_text(stmt, 0);
  }

  if(!dt_version || strcmp(dt_version, darktable_package_version))
  {
    _upgrade_camera_table(db);

    sqlite3_finalize(stmt);

    sqlite3_prepare_v2(db->handle,
                       "INSERT OR REPLACE"
                       " INTO main.db_info (key, value)"
                       " VALUES ('dt_version', ?1)",
                       -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, darktable_package_version, -1, SQLITE_TRANSIENT);
    if(sqlite3_step(stmt) != SQLITE_DONE)
    {
      dt_print(DT_DEBUG_ALWAYS, "[init] can't insert/update new dt_version");
    }
  }

  sqlite3_finalize(stmt);
}

void dt_database_destroy(const dt_database_t *db)
{
  sqlite3_close(db->handle);
  if(db->lockfile_data)
  {
    g_unlink(db->lockfile_data);
    g_free(db->lockfile_data);
  }
  if(db->lockfile_library)
  {
    g_unlink(db->lockfile_library);
    g_free(db->lockfile_library);
  }
  g_free(db->dbfilename_data);
  g_free(db->dbfilename_library);
  g_free((dt_database_t *)db);

  sqlite3_shutdown();
}

sqlite3 *dt_database_get(const dt_database_t *db)
{
  return db ? db->handle : NULL;
}

const gchar *dt_database_get_path(const struct dt_database_t *db)
{
  return db->dbfilename_library;
}

static void _database_migrate_to_xdg_structure()
{
  gchar *conf_db = dt_conf_get_string("database");

  gchar datadir[PATH_MAX] = { 0 };
  dt_loc_get_datadir(datadir, sizeof(datadir));

  if(conf_db && conf_db[0] != '/')
  {
    const char *homedir = getenv("HOME");
    gchar *dbfilename = g_strdup_printf("%s/%s", homedir, conf_db);
    if(g_file_test(dbfilename, G_FILE_TEST_EXISTS))
    {
      gchar *destdbname = g_strdup_printf("%s/%s", datadir, "library.db");
      if(!g_file_test(destdbname, G_FILE_TEST_EXISTS))
      {
        dt_print(DT_DEBUG_ALWAYS, "[init] moving database into new XDG directory structure");
        rename(dbfilename, destdbname);
        dt_conf_set_string("database", "library.db");
      }
      g_free(destdbname);
    }
    g_free(dbfilename);
  }

  g_free(conf_db);
}

/* delete old mipmaps files */
static void _database_delete_mipmaps_files()
{
  /* This migration is intended to be run only from 0.9.x to new cache in 1.0 */

  // Directory
  char cachedir[PATH_MAX] = { 0 }, mipmapfilename[PATH_MAX] = { 0 };
  dt_loc_get_user_cache_dir(cachedir, sizeof(cachedir));

  snprintf(mipmapfilename, sizeof(mipmapfilename), "%s/mipmaps", cachedir);

  if(g_access(mipmapfilename, F_OK) != -1)
  {
    dt_print(DT_DEBUG_ALWAYS, "[mipmap_cache] dropping old version file: %s", mipmapfilename);
    g_unlink(mipmapfilename);

    snprintf(mipmapfilename, sizeof(mipmapfilename), "%s/mipmaps.fallback", cachedir);

    if(g_access(mipmapfilename, F_OK) != -1) g_unlink(mipmapfilename);
  }
}

gboolean dt_database_get_lock_acquired(const dt_database_t *db)
{
  return db->lock_acquired;
}

void dt_database_cleanup_busy_statements(const struct dt_database_t *db)
{
  sqlite3_stmt *stmt = NULL;
  while( (stmt = sqlite3_next_stmt(db->handle, NULL)) != NULL)
  {
    const char* sql = sqlite3_sql(stmt);
    if(sqlite3_stmt_busy(stmt))
    {
      dt_print(DT_DEBUG_SQL,
               "[db busy stmt] non-finalized nor stepped through statement: '%s'",sql);
      sqlite3_reset(stmt);
    }
    else {
      dt_print(DT_DEBUG_SQL, "[db busy stmt] non-finalized statement: '%s'",sql);
    }
    sqlite3_finalize(stmt);
  }
}

#define ERRCHECK {if(err!=NULL) {dt_print(DT_DEBUG_SQL, "[db maintenance] maintenance error: '%s'",err); sqlite3_free(err); err=NULL;}}
void dt_database_perform_maintenance(const struct dt_database_t *db)
{
  char* err = NULL;

  const int main_pre_free_count = _get_pragma_int_val(db->handle, "main.freelist_count");
  const int main_page_size = _get_pragma_int_val(db->handle, "main.page_size");
  const int data_pre_free_count = _get_pragma_int_val(db->handle, "data.freelist_count");
  const int data_page_size = _get_pragma_int_val(db->handle, "data.page_size");

  const guint64 calc_pre_size = (main_pre_free_count*main_page_size) + (data_pre_free_count*data_page_size);

  if(calc_pre_size == 0)
  {
    dt_print(DT_DEBUG_SQL,
             "[db maintenance] maintenance deemed unnecessary, performing only analyze");
    DT_DEBUG_SQLITE3_EXEC(db->handle, "ANALYZE data", NULL, NULL, &err);
    ERRCHECK
    DT_DEBUG_SQLITE3_EXEC(db->handle, "ANALYZE main", NULL, NULL, &err);
    ERRCHECK
    DT_DEBUG_SQLITE3_EXEC(db->handle, "ANALYZE", NULL, NULL, &err);
    ERRCHECK
    return;
  }

  DT_DEBUG_SQLITE3_EXEC(db->handle, "VACUUM data", NULL, NULL, &err);
  ERRCHECK
  DT_DEBUG_SQLITE3_EXEC(db->handle, "VACUUM main", NULL, NULL, &err);
  ERRCHECK
  DT_DEBUG_SQLITE3_EXEC(db->handle, "ANALYZE data", NULL, NULL, &err);
  ERRCHECK
  DT_DEBUG_SQLITE3_EXEC(db->handle, "ANALYZE main", NULL, NULL, &err);
  ERRCHECK

  // for some reason this is needed in some cases
  // in case above performed vacuum+analyze properly, this is noop.
  DT_DEBUG_SQLITE3_EXEC(db->handle, "VACUUM", NULL, NULL, &err);
  ERRCHECK
  DT_DEBUG_SQLITE3_EXEC(db->handle, "ANALYZE", NULL, NULL, &err);
  ERRCHECK

  const int main_post_free_count = _get_pragma_int_val(db->handle, "main.freelist_count");
  const int data_post_free_count = _get_pragma_int_val(db->handle, "data.freelist_count");

  const guint64 calc_post_size = (main_post_free_count*main_page_size) + (data_post_free_count*data_page_size);
  const gint64 bytes_freed = calc_pre_size - calc_post_size;

  dt_print(DT_DEBUG_SQL,
           "[db maintenance] maintenance done, %" G_GINT64_FORMAT " bytes freed",
           bytes_freed);

  if(calc_post_size >= calc_pre_size)
  {
    dt_print(DT_DEBUG_SQL,
             "[db maintenance] maintenance problem. if no errors logged, it should work fine next time");
  }
}
#undef ERRCHECK

static inline gboolean _is_mem_db(const struct dt_database_t *db)
{
  return !g_strcmp0(db->dbfilename_data, ":memory:") || !g_strcmp0(db->dbfilename_library, ":memory:");
}

gboolean dt_database_maybe_maintenance(const struct dt_database_t *db)
{
  if(_is_mem_db(db))
    return FALSE;

  // checking free pages
  const int main_free_count = _get_pragma_int_val(db->handle, "main.freelist_count");
  const int main_page_count = _get_pragma_int_val(db->handle, "main.page_count");
  const int main_page_size = _get_pragma_int_val(db->handle, "main.page_size");

  const int data_free_count = _get_pragma_int_val(db->handle, "data.freelist_count");
  const int data_page_count = _get_pragma_int_val(db->handle, "data.page_count");
  const int data_page_size = _get_pragma_int_val(db->handle, "data.page_size");

  dt_print(DT_DEBUG_SQL,
           "[db maintenance] main: [%d/%d pages], data: [%d/%d pages]",
           main_free_count, main_page_count, data_free_count, data_page_count);

  if(main_page_count <= 0 || data_page_count <= 0)
  {
    //something's wrong with PRAGMA page_size returns. early bail.
    dt_print(DT_DEBUG_SQL,
             "[db maintenance] page_count <= 0 : main.page_count: %d, data.page_count: %d",
             main_page_count, data_page_count);
    return FALSE;
  }

  // we don't need fine-grained percentages, so let's do ints
  const int main_free_percentage = (main_free_count * 100 ) / main_page_count;
  const int data_free_percentage = (data_free_count * 100 ) / data_page_count;

  const int freepage_ratio = dt_conf_get_int("database/maintenance_freepage_ratio");

  if((main_free_percentage >= freepage_ratio)
     || (data_free_percentage >= freepage_ratio))
  {
    const guint64 calc_size = (main_free_count*main_page_size) + (data_free_count*data_page_size);
    dt_print(DT_DEBUG_SQL,
             "[db maintenance] maintenance, %" G_GUINT64_FORMAT " bytes to free",
             calc_size);
    return TRUE;
  }

  return FALSE;
}

void dt_database_optimize(const struct dt_database_t *db)
{
  if(_is_mem_db(db))
    return;
  // optimize should in most cases be no-op and have no noticeable downsides
  // this should be ran on every exit
  // see: https://www.sqlite.org/pragma.html#pragma_optimize
  DT_DEBUG_SQLITE3_EXEC(db->handle, "PRAGMA optimize", NULL, NULL, NULL);
}

static void _print_backup_progress(int remaining, int total)
{
  // TODO if we have closing splashpage - this can be used to advance progressbar :)
  dt_print(DT_DEBUG_SQL, "[db backup] %d out of %d done", total - remaining, total);
}

static int _backup_db(
  sqlite3 *src_db,               /* Database handle to back up */
  const char *src_db_name,       /* Database name to back up */
  const char *dest_filename,      /* Name of file to back up to */
  void(*xProgress)(int, int)  /* Progress function to invoke */
)
{
  sqlite3 *dest_db;             /* Database connection opened on zFilename */

  /* Open the database file identified by zFilename. */
  int rc = sqlite3_open(dest_filename, &dest_db);

  if(rc == SQLITE_OK)
  {
    /* Open the sqlite3_backup object used to accomplish the transfer */
    sqlite3_backup *sb_dest = sqlite3_backup_init(dest_db, "main", src_db, src_db_name);
    if(sb_dest)
    {
      dt_print(DT_DEBUG_SQL, "[db backup] %s to %s", src_db_name, dest_filename);
      gchar *pragma = g_strdup_printf("%s.page_count", src_db_name);
      const int spc = _get_pragma_int_val(src_db, pragma);
      g_free(pragma);
      const int pc = MIN(spc, MAX(5,spc/100));
      do
      {
        rc = sqlite3_backup_step(sb_dest, pc);
        if(xProgress)
          xProgress(
            sqlite3_backup_remaining(sb_dest),
            sqlite3_backup_pagecount(sb_dest)
          );
        if( rc==SQLITE_OK || rc==SQLITE_BUSY || rc==SQLITE_LOCKED )
        {
          sqlite3_sleep(25);
        }
      }
      while( rc==SQLITE_OK || rc==SQLITE_BUSY || rc==SQLITE_LOCKED );

      /* Release resources allocated by backup_init(). */
      (void)sqlite3_backup_finish(sb_dest);
    }
    rc = sqlite3_errcode(dest_db);
  }
  /* Close the database connection opened on database file zFilename
  ** and return the result of this function. */
  (void)sqlite3_close(dest_db);
  return rc;
}

gboolean dt_database_snapshot(const struct dt_database_t *db)
{
  // backing up memory db is pointelss
  if(_is_mem_db(db))
    return FALSE;
  GDateTime *date_now = g_date_time_new_now_local();
  gchar *date_suffix = g_date_time_format(date_now, "%Y%m%d%H%M%S");
  g_date_time_unref(date_now);

  const char *file_pattern = "%s-snp-%s";
  const char *temp_pattern = "%s-tmp-%s";

  gchar *lib_backup_file = g_strdup_printf(file_pattern, db->dbfilename_library, date_suffix);
  gchar *lib_tmpbackup_file = g_strdup_printf(temp_pattern, db->dbfilename_library, date_suffix);

  int rc = _backup_db(db->handle, "main", lib_tmpbackup_file, _print_backup_progress);
  if(!(rc==SQLITE_OK))
  {
    g_unlink(lib_tmpbackup_file);
    g_free(lib_tmpbackup_file);
    g_free(lib_backup_file);
    g_free(date_suffix);
    return FALSE;
  }
  g_rename(lib_tmpbackup_file, lib_backup_file);
  g_chmod(lib_backup_file, S_IRUSR);
  g_free(lib_tmpbackup_file);
  g_free(lib_backup_file);

  gchar *dat_backup_file = g_strdup_printf(file_pattern, db->dbfilename_data, date_suffix);
  gchar *dat_tmpbackup_file = g_strdup_printf(temp_pattern, db->dbfilename_data, date_suffix);

  g_free(date_suffix);

  rc = _backup_db(db->handle, "data", dat_tmpbackup_file, _print_backup_progress);
  if(!(rc==SQLITE_OK))
  {
    g_unlink(dat_tmpbackup_file);
    g_free(dat_tmpbackup_file);
    g_free(dat_backup_file);
    return FALSE;
  }
  g_rename(dat_tmpbackup_file, dat_backup_file);
  g_chmod(dat_backup_file, S_IRUSR);
  g_free(dat_tmpbackup_file);
  g_free(dat_backup_file);

  return TRUE;
}

gboolean dt_database_maybe_snapshot(const struct dt_database_t *db)
{
  if(_is_mem_db(db))
    return FALSE;

  const char *config = dt_conf_get_string_const("database/create_snapshot");
  if(!g_strcmp0(config, "never"))
  {
    // early bail out on "never"
    dt_print(DT_DEBUG_SQL,
             "[db backup] please consider enabling database snapshots");
    return FALSE;
  }
  if(!g_strcmp0(config, "on close"))
  {
    // early bail out on "on close"
    dt_print(DT_DEBUG_SQL, "[db backup] performing unconditional snapshot");
    return TRUE;
  }

  GTimeSpan span_from_last_snap_required;

  if(!g_strcmp0(config, "once a day"))
  {
    span_from_last_snap_required = G_TIME_SPAN_DAY;
  }
  else if(!g_strcmp0(config, "once a week"))
  {
    span_from_last_snap_required = G_TIME_SPAN_DAY * 7;
  }
  else if(!g_strcmp0(config, "once a month"))
  {
    //average month ;)
    span_from_last_snap_required = G_TIME_SPAN_DAY * 30;
  }
  else
  {
    // early bail out on "invalid value"
    dt_print(DT_DEBUG_SQL,
             "[db backup] invalid timespan requirement expecting never/on close/once a [day/week/month], got %s",
             config);
    return TRUE;
  }

  //we're in trouble zone - we have to determine when was the last snapshot done (including version upgrade snapshot) :/
  //this could be easy if we wrote date of last successful backup to config, but that's not really an option since
  //backup may done as last db operation, way after config file is closed. Plus we might be mixing dates of backups for
  //various library.db

  dt_print(DT_DEBUG_SQL, "[db backup] checking snapshots existence");
  GFile *library = g_file_parse_name(db->dbfilename_library);
  GFile *parent = g_file_get_parent(library);

  if(parent == NULL)
  {
    dt_print(DT_DEBUG_SQL, "[db backup] couldn't get library parent!");
    g_object_unref(library);
    return FALSE;
  }

  GError *error = NULL;
  GFileEnumerator *library_dir_files = g_file_enumerate_children(parent, G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_TIME_MODIFIED, G_FILE_QUERY_INFO_NONE, NULL, &error);

  if(library_dir_files == NULL)
  {
    dt_print(DT_DEBUG_SQL, "[db backup] couldn't enumerate library parent: %s", error->message);
    g_object_unref(parent);
    g_object_unref(library);
    g_error_free(error);
    return FALSE;
  }

  gchar *lib_basename = g_file_get_basename(library);
  g_object_unref(library);

  gchar *lib_snap_format = g_strdup_printf("%s-snp-", lib_basename);
  gchar *lib_backup_format = g_strdup_printf("%s-pre-", lib_basename);
  g_free(lib_basename);

  GFileInfo *info = NULL;
  guint64 last_snap = 0;

  while((info = g_file_enumerator_next_file(library_dir_files, NULL, &error)))
  {
    const char* fname = g_file_info_get_name(info);
    if(g_str_has_prefix(fname, lib_snap_format) || g_str_has_prefix(fname, lib_backup_format))
    {
      dt_print(DT_DEBUG_SQL, "[db backup] found file: %s", fname);
      if(last_snap == 0)
      {
        last_snap = g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
        g_object_unref(info);
        continue;
      }
      const guint64 try_snap = g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
      if(try_snap > last_snap)
      {
        last_snap = try_snap;
      }
    }
    g_object_unref(info);
  }
  g_object_unref(parent);
  g_free(lib_snap_format);
  g_free(lib_backup_format);

  if(error)
  {
    dt_print(DT_DEBUG_SQL, "[db backup] problem enumerating library parent: %s", error->message);
    g_file_enumerator_close(library_dir_files, NULL, NULL);
    g_object_unref(library_dir_files);
    g_error_free(error);
    return FALSE;
  }

  g_file_enumerator_close(library_dir_files, NULL, NULL);
  g_object_unref(library_dir_files);

  GDateTime *date_now = g_date_time_new_now_local();

  // Even if last_snap is 0 (didn't found any snaps) it produces proper date - unix epoch
  GDateTime *date_last_snap = g_date_time_new_from_unix_local(last_snap);

  gchar *now_txt = g_date_time_format(date_now, "%Y%m%d%H%M%S");
  gchar *ls_txt = g_date_time_format(date_last_snap, "%Y%m%d%H%M%S");
  dt_print(DT_DEBUG_SQL, "[db backup] last snap: %s; curr date: %s", ls_txt, now_txt);
  g_free(now_txt);
  g_free(ls_txt);

  GTimeSpan span_from_last_snap = g_date_time_difference(date_now, date_last_snap);

  g_date_time_unref(date_now);
  g_date_time_unref(date_last_snap);

  return span_from_last_snap > span_from_last_snap_required;
}

/* Parse integers in the form d (week days), dd (hours etc), ddd (ordinal days) or dddd (years) */
static gboolean _get_iso8601_int (const gchar *text, gsize length, gint *value)
{
  gsize i;
  guint v = 0;

  if(length < 1 || length > 4)
    return FALSE;

  for(i = 0; i < length; i++)
  {
    const gchar c = text[i];
    if(c < '0' || c > '9')
      return FALSE;
    v = v * 10 + (c - '0');
  }

  *value = v;
  return TRUE;
}

static gint _db_snap_sort(gconstpointer a, gconstpointer b, gpointer user_data)
{
  const gchar* e1 = (gchar *) a;
  const gchar* e2 = (gchar *) b;

  //we assume that both end with date in
  //"%Y%m%d%H%M%S" format

  gchar *datepos1 = g_strrstr(e1, "-snp-");
  gchar *datepos2 = g_strrstr(e2, "-snp-");
  if(!datepos1 || !datepos2)
    return 0;

  datepos1 +=5; //skip "-snp-"
  datepos2 +=5; //skip "-snp-"

  int year,month,day,hour,minute,second;

  if(!_get_iso8601_int(datepos1,    4, &year)   ||
     !_get_iso8601_int(datepos1+4,  2, &month)  ||
     !_get_iso8601_int(datepos1+6,  2, &day)    ||
     !_get_iso8601_int(datepos1+8,  2, &hour)   ||
     !_get_iso8601_int(datepos1+10, 2, &minute) ||
     !_get_iso8601_int(datepos1+12, 2, &second)
    )
  {
    return 0;
  }

  GDateTime *d1=g_date_time_new_local(year, month, day, hour, minute, second);

  if(!_get_iso8601_int(datepos2,    4, &year)   ||
     !_get_iso8601_int(datepos2+4,  2, &month)  ||
     !_get_iso8601_int(datepos2+6,  2, &day)    ||
     !_get_iso8601_int(datepos2+8,  2, &hour)   ||
     !_get_iso8601_int(datepos2+10, 2, &minute) ||
     !_get_iso8601_int(datepos2+12, 2, &second)
    )
  {
    g_date_time_unref(d1);
    return 0;
  }

  GDateTime *d2=g_date_time_new_local(year, month, day, hour, minute, second);

  const gint ret = g_date_time_compare(d1, d2);

  g_date_time_unref(d1);
  g_date_time_unref(d2);

  return ret;
}

char **dt_database_snaps_to_remove(const struct dt_database_t *db)
{
  if(_is_mem_db(db))
    return NULL;

  const int keep_snaps = dt_conf_get_int("database/keep_snapshots");

  if(keep_snaps < 0)
    return NULL;

  dt_print(DT_DEBUG_SQL, "[db backup] checking snapshots existence");
  GFile *lib_file = g_file_parse_name(db->dbfilename_library);
  GFile *lib_parent = g_file_get_parent(lib_file);

  if(lib_parent == NULL)
  {
    dt_print(DT_DEBUG_SQL, "[db backup] couldn't get library parent!");
    g_object_unref(lib_file);
    return NULL;
  }

  GFile *dat_file = g_file_parse_name(db->dbfilename_data);
  GFile *dat_parent = g_file_get_parent(dat_file);

  if(dat_parent == NULL)
  {
    dt_print(DT_DEBUG_SQL, "[db backup] couldn't get data parent!");
    g_object_unref(dat_file);
    g_object_unref(lib_file);
    g_object_unref(lib_parent);
  }

  gchar *lib_basename = g_file_get_basename(lib_file);
  g_object_unref(lib_file);
  gchar *lib_snap_format = g_strdup_printf("%s-snp-", lib_basename);
  gchar *lib_tmp_format = g_strdup_printf("%s-tmp-", lib_basename);
  g_free(lib_basename);

  gchar *dat_basename = g_file_get_basename(dat_file);
  g_object_unref(dat_file);
  gchar *dat_snap_format = g_strdup_printf("%s-snp-", dat_basename);
  gchar *dat_tmp_format = g_strdup_printf("%s-tmp-", dat_basename);
  g_free(dat_basename);

  GQueue *lib_snaps = g_queue_new();
  GQueue *dat_snaps = g_queue_new();
  GQueue *tmplib_snaps = g_queue_new();
  GQueue *tmpdat_snaps = g_queue_new();

  if(g_file_equal(lib_parent, dat_parent))
  {
    //slight optimization if library and data are in same dir, we only have to scan one
    GError *error = NULL;
    GFileEnumerator *library_dir_files = g_file_enumerate_children(lib_parent, G_FILE_ATTRIBUTE_STANDARD_NAME, G_FILE_QUERY_INFO_NONE, NULL, &error);

    if(library_dir_files == NULL)
    {
      dt_print(DT_DEBUG_SQL,
               "[db backup] couldn't enumerate library parent: %s",
               error->message);
      g_object_unref(lib_parent);
      g_object_unref(dat_parent);
      g_free(lib_snap_format);
      g_free(dat_snap_format);
      g_free(lib_tmp_format);
      g_free(dat_tmp_format);
      g_queue_free(lib_snaps);
      g_queue_free(dat_snaps);
      g_queue_free(tmplib_snaps);
      g_queue_free(tmpdat_snaps);
      g_error_free(error);
      return NULL;
    }

    GFileInfo *info = NULL;

    while((info = g_file_enumerator_next_file(library_dir_files, NULL, &error)))
    {
      const char* fname = g_file_info_get_name(info);
      if(g_str_has_prefix(fname, lib_snap_format))
      {
        dt_print(DT_DEBUG_SQL, "[db backup] found file: %s", fname);
        g_queue_insert_sorted(lib_snaps, g_strdup(fname), _db_snap_sort, NULL);
      }
      else if(g_str_has_prefix(fname, dat_snap_format))
      {
        dt_print(DT_DEBUG_SQL, "[db backup] found file: %s", fname);
        g_queue_insert_sorted(dat_snaps, g_strdup(fname), _db_snap_sort, NULL);
      }
      else if(g_str_has_prefix(fname, lib_tmp_format) || g_str_has_prefix(fname, dat_tmp_format))
      {
        //we insert into single queue, since it's just dependent on parent
        g_queue_push_head(tmplib_snaps, g_strdup(fname));
      }
      g_object_unref(info);
    }
    g_free(lib_snap_format);
    g_free(dat_snap_format);

    if(error)
    {
      dt_print(DT_DEBUG_SQL, "[db backup] problem enumerating library parent: %s", error->message);
      g_object_unref(lib_parent);
      g_object_unref(dat_parent);
      g_free(lib_tmp_format);
      g_free(dat_tmp_format);
      g_queue_free_full(lib_snaps, g_free);
      g_queue_free_full(dat_snaps, g_free);
      g_queue_free_full(tmplib_snaps, g_free);
      g_queue_free_full(tmpdat_snaps, g_free);
      g_file_enumerator_close(library_dir_files, NULL, NULL);
      g_object_unref(library_dir_files);
      g_error_free(error);
      return NULL;
    }
    g_file_enumerator_close(library_dir_files, NULL, NULL);
    g_object_unref(library_dir_files);
  }
  else
  {
    //well... fun.

    GError *error = NULL;
    GFileEnumerator *library_dir_files = g_file_enumerate_children(lib_parent, G_FILE_ATTRIBUTE_STANDARD_NAME, G_FILE_QUERY_INFO_NONE, NULL, &error);
    if(library_dir_files == NULL)
    {
      dt_print(DT_DEBUG_SQL,
               "[db backup] couldn't enumerate library parent: %s",
               error->message);
      g_object_unref(lib_parent);
      g_object_unref(dat_parent);
      g_free(lib_snap_format);
      g_free(dat_snap_format);
      g_free(lib_tmp_format);
      g_free(dat_tmp_format);
      g_error_free(error);
      g_queue_free(lib_snaps);
      g_queue_free(dat_snaps);
      g_queue_free(tmplib_snaps);
      g_queue_free(tmpdat_snaps);
      return NULL;
    }

    GFileEnumerator *data_dir_files = g_file_enumerate_children(dat_parent, G_FILE_ATTRIBUTE_STANDARD_NAME, G_FILE_QUERY_INFO_NONE, NULL, &error);
    if(data_dir_files == NULL)
    {
      dt_print(DT_DEBUG_SQL,
               "[db backup] couldn't enumerate data parent: %s",
               error->message);
      g_object_unref(lib_parent);
      g_object_unref(dat_parent);
      g_free(lib_snap_format);
      g_free(dat_snap_format);
      g_free(lib_tmp_format);
      g_free(dat_tmp_format);
      g_file_enumerator_close(library_dir_files, NULL, NULL);
      g_object_unref(library_dir_files);
      g_error_free(error);
      g_queue_free(lib_snaps);
      g_queue_free(dat_snaps);
      g_queue_free(tmplib_snaps);
      g_queue_free(tmpdat_snaps);
      return NULL;
    }

    GFileInfo *info = NULL;

    while((info = g_file_enumerator_next_file(library_dir_files, NULL, &error)))
    {
      const char* fname = g_file_info_get_name(info);
      if(g_str_has_prefix(fname, lib_snap_format))
      {
        dt_print(DT_DEBUG_SQL, "[db backup] found file: %s", fname);
        g_queue_insert_sorted(lib_snaps, g_strdup(fname), _db_snap_sort, NULL);
      }
      else if(g_str_has_prefix(fname, lib_tmp_format) || g_str_has_prefix(fname, dat_tmp_format))
      {
        // we remove all incomplete snaps matching pattern in BOTH dirs
        g_queue_push_head(tmplib_snaps, g_strdup(fname));
      }
      g_object_unref(info);
    }
    g_free(lib_snap_format);

    if(error)
    {
      dt_print(DT_DEBUG_SQL,
               "[db backup] problem enumerating library parent: %s",
               error->message);
      g_object_unref(lib_parent);
      g_object_unref(dat_parent);
      g_free(lib_tmp_format);
      g_free(dat_tmp_format);
      g_queue_free_full(lib_snaps, g_free);
      g_queue_free(dat_snaps);
      g_queue_free_full(tmplib_snaps, g_free);
      g_queue_free(tmpdat_snaps);
      g_file_enumerator_close(library_dir_files, NULL, NULL);
      g_object_unref(library_dir_files);
      g_file_enumerator_close(data_dir_files, NULL, NULL);
      g_object_unref(data_dir_files);
      g_error_free(error);
      return NULL;
    }
    g_file_enumerator_close(library_dir_files, NULL, NULL);
    g_object_unref(library_dir_files);

    while((info = g_file_enumerator_next_file(data_dir_files, NULL, &error)))
    {
      const char* fname = g_file_info_get_name(info);
      if(g_str_has_prefix(fname, dat_snap_format))
      {
        dt_print(DT_DEBUG_SQL, "[db backup] found file: `%s'", fname);
        g_queue_insert_sorted(dat_snaps, g_strdup(fname), _db_snap_sort, NULL);
      }
      else if(g_str_has_prefix(fname, lib_tmp_format) || g_str_has_prefix(fname, dat_tmp_format))
      {
        //we add to queue both matches - it just depends on parent
        g_queue_push_head(tmpdat_snaps, g_strdup(fname));
      }
      g_object_unref(info);
    }
    g_free(dat_snap_format);
    g_free(lib_tmp_format);
    g_free(dat_tmp_format);

    if(error)
    {
      dt_print(DT_DEBUG_SQL,
               "[db backup] problem enumerating data parent: %s",
               error->message);
      g_object_unref(lib_parent);
      g_object_unref(dat_parent);
      g_queue_free_full(lib_snaps, g_free);
      g_queue_free_full(dat_snaps, g_free);
      g_queue_free_full(tmplib_snaps, g_free);
      g_queue_free_full(tmpdat_snaps, g_free);
      g_file_enumerator_close(data_dir_files, NULL, NULL);
      g_object_unref(data_dir_files);
      g_error_free(error);
      return NULL;
    }

    g_file_enumerator_close(data_dir_files, NULL, NULL);
    g_object_unref(data_dir_files);
  }

  // here we have list of snaps sorted in date order, now we have to
  // create from that list of snaps to be deleted and return that :D

  GPtrArray *ret = g_ptr_array_new();

  gchar *lib_parent_path = g_file_get_path(lib_parent);
  g_object_unref(lib_parent);

  while(g_queue_get_length(lib_snaps) > keep_snaps)
  {
    gchar *head = g_queue_pop_head(lib_snaps);
    g_ptr_array_add(ret, g_strconcat(lib_parent_path, G_DIR_SEPARATOR_S, head, NULL));
    g_free(head);
  }
  while(!g_queue_is_empty(tmplib_snaps))
  {
    gchar *head = g_queue_pop_head(tmplib_snaps);
    g_ptr_array_add(ret, g_strconcat(lib_parent_path, G_DIR_SEPARATOR_S, head, NULL));
    g_free(head);
  }
  g_free(lib_parent_path);
  g_queue_free_full(lib_snaps, g_free);
  g_queue_free_full(tmplib_snaps, g_free); // should be totally freed, but eh - this won't make doublefree

  gchar *dat_parent_path = g_file_get_path(dat_parent);
  g_object_unref(dat_parent);

  while(g_queue_get_length(dat_snaps) > keep_snaps)
  {
    gchar *head = g_queue_pop_head(dat_snaps);
    g_ptr_array_add(ret, g_strconcat(dat_parent_path, G_DIR_SEPARATOR_S, head, NULL));
    g_free(head);
  }
  while(!g_queue_is_empty(tmpdat_snaps))
  {
    gchar *head = g_queue_pop_head(tmpdat_snaps);
    g_ptr_array_add(ret, g_strconcat(dat_parent_path, G_DIR_SEPARATOR_S, head, NULL));
    g_free(head);
  }
  g_free(dat_parent_path);
  g_queue_free_full(dat_snaps, g_free);
  g_queue_free_full(tmpdat_snaps, g_free);

  g_ptr_array_add (ret, NULL);

  return (char**)g_ptr_array_free(ret, FALSE);
}

gchar *dt_database_get_most_recent_snap(const char* db_filename)
{
  if(!g_strcmp0(db_filename, ":memory:"))
    return NULL;

  dt_print(DT_DEBUG_SQL, "[db backup] checking snapshots existence");
  GFile *db_file = g_file_parse_name(db_filename);
  GFile *parent = g_file_get_parent(db_file);

  if(parent == NULL)
  {
    dt_print(DT_DEBUG_SQL, "[db backup] couldn't get database parent!");
    g_object_unref(db_file);
    return NULL;
  }

  GError *error = NULL;
  GFileEnumerator *db_dir_files = g_file_enumerate_children(parent, G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_TIME_MODIFIED, G_FILE_QUERY_INFO_NONE, NULL, &error);

  if(db_dir_files == NULL)
  {
    dt_print(DT_DEBUG_SQL,
             "[db backup] couldn't enumerate database parent: %s",
             error->message);
    g_object_unref(parent);
    g_object_unref(db_file);
    g_error_free(error);
    return NULL;
  }

  gchar *db_basename = g_file_get_basename(db_file);
  g_object_unref(db_file);

  gchar *db_snap_format = g_strdup_printf("%s-snp-", db_basename);
  gchar *db_backup_format = g_strdup_printf("%s-pre-", db_basename);
  g_free(db_basename);

  GFileInfo *info = NULL;
  guint64 last_snap = 0;
  gchar *last_snap_name = NULL;

  while((info = g_file_enumerator_next_file(db_dir_files, NULL, &error)))
  {
    const char* fname = g_file_info_get_name(info);
    if(g_str_has_prefix(fname, db_snap_format) || g_str_has_prefix(fname, db_backup_format))
    {
      dt_print(DT_DEBUG_SQL, "[db backup] found file: `%s'", fname);
      if(last_snap == 0)
      {
        last_snap = g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
        last_snap_name = g_strdup(fname);
        g_object_unref(info);
        continue;
      }
      guint64 try_snap = g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
      if(try_snap > last_snap)
      {
        last_snap = try_snap;
        g_free(last_snap_name);
        last_snap_name = g_strdup(fname);
      }
    }
    g_object_unref(info);
  }
  g_free(db_snap_format);
  g_free(db_backup_format);

  if(error)
  {
    dt_print(DT_DEBUG_SQL,
             "[db backup] problem enumerating database parent: %s",
             error->message);
    g_file_enumerator_close(db_dir_files, NULL, NULL);
    g_object_unref(db_dir_files);
    g_error_free(error);
    g_free(last_snap_name);
    return NULL;
  }

  g_file_enumerator_close(db_dir_files, NULL, NULL);
  g_object_unref(db_dir_files);

  if(!last_snap_name)
  {
    g_object_unref(parent);
    return NULL;
  }

  gchar *parent_path = g_file_get_path(parent);
  g_object_unref(parent);

  gchar *ret = g_strconcat(parent_path, G_DIR_SEPARATOR_S, last_snap_name, NULL);
  g_free(parent_path);
  g_free(last_snap_name);

  return ret;
}

// Nested transactions support
//
// NOTE: the nested support is actually not activated (see || TRUE below). This current
//       implementation is just a refactoring of the previous code using:
//          - dt_database_start_transaction()
//          - dt_database_release_transaction()
//          - dt_database_rollback_transaction()
//
//       With this refactoring we can count and check for nested transaction and unmatched
//       transaction routines. And it has been done to help further implementation for
//       proper threading and nested transaction support.
//
void dt_database_start_transaction(const struct dt_database_t *db)
{
  const int trxid = dt_atomic_add_int(&_trxid, 1);

  // if top level a simple unamed transaction is used BEGIN / COMMIT / ROLLBACK
  // otherwise we use a savepoint (named transaction).

  if(trxid == 0)
  {
    // In theads application it may be safer to use an IMMEDIATE transaction:
    // "BEGIN IMMEDIATE TRANSACTION"
    DT_DEBUG_SQLITE3_EXEC(dt_database_get(db), "BEGIN TRANSACTION", NULL, NULL, NULL);
  }
  else
#ifdef USE_NESTED_TRANSACTIONS
  {
    char SQLTRX[32] = { 0 };
    g_snprintf(SQLTRX, sizeof(SQLTRX), "SAVEPOINT trx%d", trxid);
    DT_DEBUG_SQLITE3_EXEC(dt_database_get(db), SQLTRX, NULL, NULL, NULL);
  }
#else
  {
    dt_print(DT_DEBUG_ALWAYS,
             "[dt_database_start_transaction] nested transaction detected (%d)",
             trxid);
  }
#endif

  if(trxid > MAX_NESTED_TRANSACTIONS)
    dt_print(DT_DEBUG_ALWAYS,
             "[dt_database_start_transaction] more than %d nested transaction",
             MAX_NESTED_TRANSACTIONS);
}

void dt_database_release_transaction(const struct dt_database_t *db)
{
  const int trxid = dt_atomic_sub_int(&_trxid, 1);

  if(trxid <= 0)
    dt_print(DT_DEBUG_ALWAYS,
             "[dt_database_release_transaction] COMMIT outside a transaction");

  if(trxid == 1)
  {
    DT_DEBUG_SQLITE3_EXEC(dt_database_get(db), "COMMIT TRANSACTION", NULL, NULL, NULL);
  }
  else
#ifdef USE_NESTED_TRANSACTIONS
  {
    char SQLTRX[64] = { 0 };
    g_snprintf(SQLTRX, sizeof(SQLTRX), "RELEASE SAVEPOINT trx%d", trxid - 1);
    DT_DEBUG_SQLITE3_EXEC(dt_database_get(db), SQLTRX, NULL, NULL, NULL);
  }
#else
  {
    dt_print(DT_DEBUG_ALWAYS,
             "[dt_database_end_transaction] nested transaction detected (%d)",
             trxid);
  }
#endif
}

void dt_database_rollback_transaction(const struct dt_database_t *db)
{
  const int trxid = dt_atomic_sub_int(&_trxid, 1);

  if(trxid <= 0)
    dt_print(DT_DEBUG_ALWAYS,
             "[dt_database_rollback_transaction] ROLLBACK outside a transaction");

  if(trxid == 1)
  {
    DT_DEBUG_SQLITE3_EXEC(dt_database_get(db), "ROLLBACK TRANSACTION", NULL, NULL, NULL);
  }
  else
#ifdef USE_NESTED_TRANSACTIONS
  {
    char SQLTRX[64] = { 0 };
    g_snprintf(SQLTRX, sizeof(SQLTRX), "ROLLBACK TRANSACTION TO SAVEPOINT trx%d", trxid - 1);
    DT_DEBUG_SQLITE3_EXEC(dt_database_get(db), SQLTRX, NULL, NULL, NULL);
  }
#else
  {
    dt_print(DT_DEBUG_ALWAYS,
             "[dt_database_rollback_transaction] nested transaction detected (%d)",
             trxid);
  }
#endif
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
