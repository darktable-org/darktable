/*
    This file is part of darktable,
    copyright (c) 2011 henrik andersson.
    copyright (c) 2012-2016 tobias ellinghaus.

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

#include "common/database.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "control/conf.h"
#include "control/control.h"
#include "gui/legacy_presets.h"

#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <sqlite3.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>

// whenever _create_*_schema() gets changed you HAVE to bump this version and add an update path to
// _upgrade_*_schema_step()!
#define CURRENT_DATABASE_VERSION_LIBRARY 17
#define CURRENT_DATABASE_VERSION_DATA 1

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

/* migrate from the legacy db format (with the 'settings' blob) to the first version this system knows */
static gboolean _migrate_schema(dt_database_t *db, int version)
{
  gboolean all_ok = TRUE;
  const char *failing_query = NULL;
  sqlite3_stmt *stmt;
  sqlite3_stmt *innerstmt;

  if(version != 36) // if anyone shows up with an older db we can probably add extra code
    return FALSE;

  sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);

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
  char *last_name = NULL, *last_operation = NULL;
  int last_op_version = 0;
  int i = 0;
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    int rowid = sqlite3_column_int(stmt, 0);
    const char *name = (const char *)sqlite3_column_text(stmt, 1);
    const char *operation = (const char *)sqlite3_column_text(stmt, 2);
    int op_version = sqlite3_column_int(stmt, 3);

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
    sqlite3_prepare_v2(db->handle, "SELECT name FROM main.presets  WHERE name = ?1 || ' (' || ?2 || ')' AND "
                                   "operation = ?3 AND op_version = ?4",
                       -1, &innerstmt, NULL);
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
    const char *query = "UPDATE main.presets SET name = name || ' (' || ?1 || ')' WHERE rowid = ?2";
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
  _SQLITE3_EXEC(db->handle,
                "CREATE UNIQUE INDEX IF NOT EXISTS main.presets_idx ON presets (name, operation, op_version)",
                NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "UPDATE main.presets SET blendop_version = 1 WHERE blendop_version IS NULL", NULL,
                NULL, NULL);
  _SQLITE3_EXEC(db->handle, "UPDATE main.presets SET multi_priority = 0 WHERE multi_priority IS NULL", NULL, NULL,
                NULL);
  _SQLITE3_EXEC(db->handle, "UPDATE main.presets SET multi_name = ' ' WHERE multi_name IS NULL", NULL, NULL, NULL);


  // There are systems where absolute paths don't start with '/' (like Windows).
  // Since the bug which introduced absolute paths to the db was fixed before a
  // Windows build was available this shouldn't matter though.
  sqlite3_prepare_v2(db->handle, "SELECT id, filename FROM main.images WHERE filename LIKE '/%'", -1, &stmt, NULL);
  sqlite3_prepare_v2(db->handle, "UPDATE main.images SET filename = ?1 WHERE id = ?2", -1, &innerstmt, NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    int id = sqlite3_column_int(stmt, 0);
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
  _SQLITE3_EXEC(
      db->handle,
      "UPDATE main.images SET datetime_taken = REPLACE(datetime_taken, '-', ':') WHERE datetime_taken LIKE '%-%'",
      NULL, NULL, NULL);

end:
  if(all_ok)
    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
  else
  {
    fprintf(stderr, "[init] failing query: `%s'\n", failing_query);
    fprintf(stderr, "[init]   %s\n", sqlite3_errmsg(db->handle));
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
      fprintf(stderr, _message);                                                 \
      fprintf(stderr, "[init]   %s\n", sqlite3_errmsg(db->handle));              \
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
      fprintf(stderr, _message);                                                 \
      fprintf(stderr, "[init]   %s\n", sqlite3_errmsg(db->handle));              \
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
      fprintf(stderr, _message);                                                 \
      fprintf(stderr, "[init]   %s\n", sqlite3_errmsg(db->handle));              \
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
    TRY_EXEC("ALTER TABLE main.images ADD COLUMN write_timestamp INTEGER",
             "[init] can't add `write_timestamp' column to database\n");
    TRY_EXEC("UPDATE main.images SET write_timestamp = STRFTIME('%s', 'now') WHERE write_timestamp IS NULL",
             "[init] can't initialize `write_timestamp' with current point in time\n");
    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
    new_version = 2;
  }
  else if(version == 2)
  {
    // 2 -> 3 reset raw_black and raw_maximum. in theory we should change the columns from REAL to INTEGER,
    // but sqlite doesn't care about types so whatever
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);
    TRY_EXEC("UPDATE main.images SET raw_black = 0, raw_maximum = 16384",
             "[init] can't reset raw_black and raw_maximum\n");
    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
    new_version = 3;
  }
  else if(version == 3)
  {
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);

    TRY_EXEC("CREATE TRIGGER insert_tag AFTER INSERT ON main.tags"
             " BEGIN"
             "   INSERT INTO tagxtag SELECT id, new.id, 0 FROM TAGS;"
             "   UPDATE tagxtag SET count = 1000000 WHERE id1=new.id AND id2=new.id;"
             " END",
             "[init] can't create insert_tag trigger\n");
    TRY_EXEC("CREATE TRIGGER delete_tag BEFORE DELETE ON main.tags"
             " BEGIN"
             "   DELETE FROM tagxtag WHERE id1=old.id OR id2=old.id;"
             "   DELETE FROM tagged_images WHERE tagid=old.id;"
             " END",
             "[init] can't create delete_tag trigger\n");
    TRY_EXEC("CREATE TRIGGER attach_tag AFTER INSERT ON main.tagged_images"
             " BEGIN"
             "   UPDATE tagxtag"
             "     SET count = count + 1"
             "     WHERE (id1=new.tagid AND id2 IN (SELECT tagid FROM tagged_images WHERE imgid=new.imgid))"
             "        OR (id2=new.tagid AND id1 IN (SELECT tagid FROM tagged_images WHERE imgid=new.imgid));"
             " END",
             "[init] can't create attach_tag trigger\n");
    TRY_EXEC("CREATE TRIGGER detach_tag BEFORE DELETE ON main.tagged_images"
             " BEGIN"
             "   UPDATE tagxtag"
             "     SET count = count - 1"
             "     WHERE (id1=old.tagid AND id2 IN (SELECT tagid FROM tagged_images WHERE imgid=old.imgid))"
             "        OR (id2=old.tagid AND id1 IN (SELECT tagid FROM tagged_images WHERE imgid=old.imgid));"
             " END",
             "[init] can't create detach_tag trigger\n");

    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
    new_version = 4;
  }
  else if(version == 4)
  {
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);

    TRY_EXEC("ALTER TABLE main.presets RENAME TO tmp_presets",  "[init] can't rename table presets\n");

    TRY_EXEC("CREATE TABLE main.presets (name VARCHAR, description VARCHAR, operation VARCHAR, op_params BLOB,"
             "enabled INTEGER, blendop_params BLOB, model VARCHAR, maker VARCHAR, lens VARCHAR,"
             "iso_min REAL, iso_max REAL, exposure_min REAL, exposure_max REAL, aperture_min REAL,"
             "aperture_max REAL, focal_length_min REAL, focal_length_max REAL, writeprotect INTEGER,"
             "autoapply INTEGER, filter INTEGER, def INTEGER, format INTEGER, op_version INTEGER,"
             "blendop_version INTEGER, multi_priority INTEGER, multi_name VARCHAR(256))",
             "[init] can't create new presets table\n");

    TRY_EXEC("INSERT INTO main.presets (name, description, operation, op_params, enabled, blendop_params, model, "
             "maker, lens, iso_min, iso_max, exposure_min, exposure_max, aperture_min, aperture_max,"
             "focal_length_min, focal_length_max, writeprotect, autoapply, filter, def, format, op_version, "
             "blendop_version, multi_priority, multi_name) SELECT name, description, operation, op_params, "
             "enabled, blendop_params, model, maker, lens, iso_min, iso_max, exposure_min, exposure_max, "
             "aperture_min, aperture_max, focal_length_min, focal_length_max, writeprotect, autoapply, filter, "
             "def, isldr, op_version, blendop_version, multi_priority, multi_name FROM tmp_presets",
             "[init] can't populate presets table from tmp_presets\n");

    TRY_EXEC("DROP TABLE tmp_presets", "[init] can't delete table tmp_presets\n");

    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
    new_version = 5;
  }
  else if(version == 5)
  {
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);

    TRY_EXEC("CREATE INDEX main.images_filename_index ON images (filename)",
             "[init] can't create index on image filename\n");

    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
    new_version = 6;
  }
  else if(version == 6)
  {
    // some ancient tables can have the styleid column of style_items be called style_id. fix that.
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);

    if(sqlite3_exec(db->handle, "SELECT style_id FROM main.style_items", NULL, NULL, NULL) == SQLITE_OK)
    {
      TRY_EXEC("ALTER TABLE main.style_items RENAME TO tmp_style_items",
               "[init] can't rename table style_items\n");

      TRY_EXEC("CREATE TABLE main.style_items (styleid INTEGER, num INTEGER, module INTEGER, "
               "operation VARCHAR(256), op_params BLOB, enabled INTEGER, "
               "blendop_params BLOB, blendop_version INTEGER, multi_priority INTEGER, multi_name VARCHAR(256))",
               "[init] can't create new style_items table\n");

      TRY_EXEC("INSERT INTO main.style_items (styleid, num, module, operation, op_params, enabled,"
               "                         blendop_params, blendop_version, multi_priority, multi_name)"
               "                  SELECT style_id, num, module, operation, op_params, enabled,"
               "                         blendop_params, blendop_version, multi_priority, multi_name"
               "                  FROM   tmp_style_items",
               "[init] can't populate style_items table from tmp_style_items\n");

      TRY_EXEC("DROP TABLE tmp_style_items", "[init] can't delete table tmp_style_items\n");
    }

    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
    new_version = 7;
  }
  else if(version == 7)
  {
    // make sure that we have no film rolls with a NULL folder
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);

    TRY_EXEC("ALTER TABLE main.film_rolls RENAME TO tmp_film_rolls", "[init] can't rename table film_rolls\n");

    TRY_EXEC("CREATE TABLE main.film_rolls "
             "(id INTEGER PRIMARY KEY, datetime_accessed CHAR(20), "
             "folder VARCHAR(1024) NOT NULL)",
             "[init] can't create new film_rolls table\n");

    TRY_EXEC("INSERT INTO main.film_rolls (id, datetime_accessed, folder) "
             "SELECT id, datetime_accessed, folder "
             "FROM   tmp_film_rolls "
             "WHERE  folder IS NOT NULL",
             "[init] can't populate film_rolls table from tmp_film_rolls\n");

    TRY_EXEC("DROP TABLE tmp_film_rolls", "[init] can't delete table tmp_film_rolls\n");

    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
    new_version = 8;
  }
  else if(version == 8)
  {
    // 8 -> 9 added history_end column to images
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);

    TRY_EXEC("ALTER TABLE main.images ADD COLUMN history_end INTEGER",
             "[init] can't add `history_end' column to database\n");

    TRY_EXEC("UPDATE main.images SET history_end = (SELECT IFNULL(MAX(num) + 1, 0) FROM main.history "
             "WHERE imgid = id)", "[init] can't initialize `history_end' with last history entry\n");

    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
    new_version = 9;
  }
  else if(version == 9)
  {
    // 9 -> 10 cleanup of last update :(
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);

    TRY_EXEC("UPDATE main.images SET history_end = (SELECT IFNULL(MAX(num) + 1, 0) FROM main.history "
             "WHERE imgid = id)", "[init] can't set `history_end' to 0 where it was NULL\n");

    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
    new_version = 10;
  }
  else if(version == 10)
  {
    // 10 -> 11 added altitude column to images
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);

    TRY_EXEC("ALTER TABLE main.images ADD COLUMN altitude REAL",
             "[init] can't add `altitude' column to database\n");

    TRY_EXEC("UPDATE main.images SET altitude = NULL", "[init] can't initialize `altitude' with NULL\n");

    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
    new_version = 11;
  }
  else if(version == 11)
  {
    // 11 -> 12 tagxtag was removed in order to reduce database size
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);

    TRY_EXEC("DROP TRIGGER main.detach_tag", "[init] can't drop trigger `detach_tag' from database\n");

    TRY_EXEC("DROP TRIGGER main.attach_tag", "[init] can't drop trigger `attach_tag' from database\n");

    TRY_EXEC("DROP TRIGGER main.delete_tag", "[init] can't drop trigger `delete_tag' from database\n");

    TRY_EXEC("DROP TRIGGER main.insert_tag", "[init] can't drop trigger `insert_tag' from database\n");

    TRY_EXEC("DROP TABLE main.tagxtag", "[init] can't drop table `tagxtag' from database\n");

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
             "[init] can't delete already migrated presets from database\n");

    // find all presets that are clashing with something else in presets. that can happen as we introduced an
    // index on presets in data which wasn't in place in library.
    TRY_PREPARE(select_stmt, "SELECT p.rowid, r FROM main.presets AS p, (SELECT rowid AS r, name, operation, "
                             "op_version FROM main.presets GROUP BY name, operation, op_version HAVING "
                             "COUNT(*) > 1) USING (name, operation, op_version) WHERE p.rowid != r",
                "[init] can't prepare selecting presets with same name, operation, op_version from database\n");

    // see if an updated preset name still causes problems
    TRY_PREPARE(count_clashes_stmt, "SELECT COUNT(*) FROM main.presets AS p, (SELECT name, operation, op_version "
                                    "FROM main.presets WHERE rowid = ?1) AS i ON p.name = i.name || \" #\" || ?2 "
                                    "AND p.operation = i.operation AND p.op_version = i.op_version",
                "[init] can't prepare selection of preset count by name from database\n");

    // update the preset name for good
    TRY_PREPARE(update_name_stmt, "UPDATE main.presets SET name = name || \" #\" || ?1 WHERE rowid = ?2",
                "[init] can't prepare updating of preset name in database\n");

    // find all presets that would be clashing with something in data
    TRY_PREPARE(stmt, "SELECT p1.rowid FROM main.presets p1 INNER JOIN data.presets p2 "
                      "USING (name, operation, op_version) WHERE p1.writeprotect = 0",
                "[init] can't access table `presets' in database\n");

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
                "[init] can't prepare insertion statement\n");

    TRY_PREPARE(delete_stmt, "DELETE FROM main.presets WHERE rowid = ?1", "[init] can't prepare deletion statement\n");

    // first rename presets with (name, operation, op_version) not being unique
    while(sqlite3_step(select_stmt) == SQLITE_ROW)
    {
      int own_rowid = sqlite3_column_int(select_stmt, 0);
      int other_rowid = sqlite3_column_int(select_stmt, 1);
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
      TRY_STEP(update_name_stmt, SQLITE_DONE, "[init] can't rename preset in database\n");
      sqlite3_reset(update_name_stmt);
      sqlite3_reset(update_name_stmt);
    }

    // now rename to avoid clashes with data.presets
    while(sqlite3_step(stmt) == SQLITE_ROW)
    {
      int preset_version = 0;
      int rowid = sqlite3_column_int(stmt, 0);

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
      TRY_STEP(delete_stmt, SQLITE_DONE, "[init] can't delete preset from database\n");
    }

    // all that is left in presets should be those that can be moved over without any further concerns
    TRY_EXEC("INSERT OR FAIL INTO data.presets SELECT name, description, operation, "
             "op_version, op_params, enabled, blendop_params, blendop_version, "
             "multi_priority, multi_name, model, maker, lens, iso_min, iso_max, "
             "exposure_min, exposure_max, aperture_min, aperture_max, "
             "focal_length_min, focal_length_max, writeprotect, autoapply, filter, "
             "def, format FROM main.presets WHERE writeprotect = 0",
             "[init] can't copy presets to the data database\n");
    // ... delete them on the old side
    TRY_EXEC("DELETE FROM main.presets WHERE writeprotect = 0",
             "[init] can't copy presets to the data database\n");

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

    TRY_PREPARE(stmt, "SELECT id, name FROM main.styles", "[init] can't prepare style selection from database\n");
    TRY_PREPARE(select_stmt, "SELECT rowid FROM data.styles WHERE name = ?1 LIMIT 1",
                "[init] can't prepare style item selection from database\n");
    TRY_PREPARE(update_name_stmt, "UPDATE main.styles SET name = ?1 WHERE id = ?2",
                "[init] can't prepare style name update\n");
    TRY_PREPARE(insert_stmt, "INSERT INTO data.styles (id, name, description) "
                             "SELECT (SELECT COALESCE(MAX(id),0)+1 FROM data.styles), name, description "
                             "FROM main.styles where id = ?1",
                "[init] can't prepare style insertion for database\n");
    TRY_PREPARE(delete_stmt, "DELETE FROM main.styles WHERE id = ?1",
                "[init] can't prepare style deletion for database\n");
    TRY_PREPARE(select_new_stmt, "SELECT id FROM data.styles WHERE rowid = ?1",
                "[init] can't prepare style selection from data database\n");
    TRY_PREPARE(copy_style_items_stmt, "INSERT INTO data.style_items "
                                       "(styleid, num, module, operation, op_params, enabled, blendop_params, "
                                       "blendop_version, multi_priority, multi_name) "
                                       "SELECT ?1, num, module, operation, op_params, enabled, blendop_params, "
                                       "blendop_version, multi_priority, multi_name FROM main.style_items "
                                       "WHERE styleid = ?2",
                "[init] can't prepare style item copy into data database\n");
    TRY_PREPARE(delete_style_items_stmt, "DELETE FROM main.style_items WHERE styleid = ?1",
                "[init] can't prepare style item deletion for database\n");

    while(sqlite3_step(stmt) == SQLITE_ROW)
    {
      int id = sqlite3_column_int(stmt, 0);
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
        TRY_STEP(update_name_stmt, SQLITE_DONE, "[init] can't update name of style in database\n");
        sqlite3_reset(update_name_stmt);
        sqlite3_clear_bindings(update_name_stmt);
        g_free(new_name);
      }

      // move the style to data.styles and get the rowid
      sqlite3_bind_int(insert_stmt, 1, id);
      TRY_STEP(insert_stmt, SQLITE_DONE, "[init] can't insert style into data database\n");
      sqlite3_int64 last_rowid = sqlite3_last_insert_rowid(db->handle);

      // delete style from styles
      sqlite3_bind_int(delete_stmt, 1, id);
      TRY_STEP(delete_stmt, SQLITE_DONE, "[init] can't delete style from database\n");

      sqlite3_bind_int(select_new_stmt, 1, last_rowid);
      TRY_STEP(select_new_stmt, SQLITE_ROW, "[init] can't select new style from data database\n");
      int new_id = sqlite3_column_int(select_new_stmt, 0);

      // now that we have the style over in data.styles and the new id we can just copy over all style items
      sqlite3_bind_int(copy_style_items_stmt, 1, new_id);
      sqlite3_bind_int(copy_style_items_stmt, 2, id);
      TRY_STEP(copy_style_items_stmt, SQLITE_DONE, "[init] can't copy style items into data database\n");

      // delete the style items from the old table
      sqlite3_bind_int(delete_style_items_stmt, 1, id);
      TRY_STEP(delete_style_items_stmt, SQLITE_DONE, "[init] can't delete style items from database\n");

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

    // tags
    TRY_EXEC("INSERT OR IGNORE INTO data.tags (name, icon, description, flags) "
             "SELECT name, icon, description, flags FROM main.tags",
             "[init] can't prepare insertion of used tags into data database\n");

    // tagged images
    // we need a temp table to update tagged_images due to its primary key
    TRY_EXEC("CREATE TEMPORARY TABLE tagged_images_tmp (imgid INTEGER, tagid INTEGER)",
             "[init] can't create temporary table for updating `tagged_images'\n");

    TRY_EXEC("INSERT INTO tagged_images_tmp (imgid, tagid) "
             "SELECT imgid, (SELECT t2.id FROM main.tags t1, data.tags t2 USING (name) WHERE t1.id = tagid) "
             "FROM main.tagged_images", "[init] can't insert into `tagged_images_tmp'\n");

    TRY_EXEC("DELETE FROM main.tagged_images", "[init] can't delete tagged images in database\n");

    TRY_EXEC("INSERT OR IGNORE INTO main.tagged_images (imgid, tagid) SELECT imgid, tagid FROM tagged_images_tmp",
             "[init] can't copy updated values back to `tagged_images'\n");

    TRY_EXEC("DROP TABLE tagged_images_tmp", "[init] can't drop table `tagged_images_tmp' from database\n");

    ////////////// cleanup - drop the indexes and tags
    TRY_EXEC("DROP INDEX IF EXISTS main.presets_idx", "[init] can't drop index `presets_idx' from database\n");
    TRY_EXEC("DROP TABLE main.presets", "[init] can't drop table `presets' from database\n");
    TRY_EXEC("DROP TABLE main.style_items", "[init] can't drop table `style_items' from database\n");
    TRY_EXEC("DROP TABLE main.styles", "[init] can't drop table `styles' from database\n");
    TRY_EXEC("DROP TABLE main.tags", "[init] can't drop table `tags' from database\n");

    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
    new_version = 13;
  } else if(version == 13)
  {
    // 12 -> 13 bring back the used tag names to library.db so people can use it independently of data.db
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);

    TRY_EXEC("CREATE TABLE main.used_tags (id INTEGER, name VARCHAR NOT NULL)",
             "[init] can't create `used_tags` table\n");

    TRY_EXEC("CREATE INDEX main.used_tags_idx ON used_tags (id, name)",
             "[init] can't create index on table `used_tags' in database\n");

    TRY_EXEC("INSERT INTO main.used_tags (id, name) SELECT t.id, t.name FROM data.tags AS t, main.tagged_images "
             "AS i ON t.id = i.tagid GROUP BY t.id",
             "[init] can't insert used tags into `used_tags` table in database\n");

    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
    new_version = 14;
  }
  else if(version == 14)
  {
    // 13 -> fix the index on used_tags to be a UNIQUE index :-/
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);

    TRY_EXEC("DELETE FROM main.used_tags WHERE rowid NOT IN (SELECT rowid FROM used_tags GROUP BY id)",
             "[init] can't delete duplicated entries from `used_tags' in database\n");

    TRY_EXEC("DROP INDEX main.used_tags_idx", "[init] can't drop index `used_tags_idx' from database\n");

    TRY_EXEC("CREATE UNIQUE INDEX main.used_tags_idx ON used_tags (id, name)",
             "[init] can't create index `used_tags_idx' in database\n");

    TRY_EXEC("DELETE FROM main.tagged_images WHERE tagid IS NULL",
             "[init] can't delete NULL entries from `tagged_images' in database");

    TRY_EXEC("DELETE FROM main.used_tags WHERE id NOT IN (SELECT DISTINCT tagid FROM main.tagged_images)",
             "[init] can't delete unused tags from `used_tags' in database\n");

    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
    new_version = 15;
  }
  else if(version == 15)
  {
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);
    ////////////////////////////// custom image order
    TRY_EXEC("ALTER TABLE main.images ADD COLUMN position INTEGER",
             "[init] can't add `position' column to images table in database\n");
    TRY_EXEC("CREATE INDEX main.image_position_index ON images (position)",
             "[init] can't create index for custom image order table\n");

    // Set the initial image sequence. The image id - the sequece images were imported -
    // defines the initial order of images.
    //
    // An int64 is used for the position index. The upper 31 bits define the initial order.
    // The lower 32bit provide space to reorder images.
    //
    // see: dt_collection_move_before()
    //
    TRY_EXEC("UPDATE main.images SET position = id << 32",
             "[init] can't update positions custom image order table\n");

    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
    new_version = 16;
  }
  else if(version == 16)
  {
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);
    ////////////////////////////// final image aspect ratio
    TRY_EXEC("ALTER TABLE main.images ADD COLUMN aspect_ratio REAL",
             "[init] can't add `aspect_ratio' column to images table in database\n");
    TRY_EXEC("UPDATE main.images SET aspect_ratio = 0.0",
             "[init] can't update aspect_ratio in database\n");

    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
    new_version = 17;
  }
  // maybe in the future, see commented out code elsewhere
  //   else if(version == XXX)
  //   {
  //     sqlite3_exec(db->handle, "ALTER TABLE film_rolls ADD COLUMN external_drive VARCHAR(1024)", NULL,
  //     NULL, NULL);
  //   }
  else
    new_version = version; // should be the fallback so that calling code sees that we are in an infinite loop

  // write the new version to db
  sqlite3_prepare_v2(db->handle, "INSERT OR REPLACE INTO main.db_info (key, value) VALUES ('version', ?1)", -1, &stmt,
                     NULL);
  sqlite3_bind_int(stmt, 1, new_version);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  return new_version;
}

#undef FINALIZE

#undef TRY_EXEC
#undef TRY_STEP
#undef TRY_PREPARE

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
  else
    new_version = version; // should be the fallback so that calling code sees that we are in an infinite loop

  // write the new version to db
  sqlite3_prepare_v2(db->handle, "INSERT OR REPLACE INTO data.db_info (key, value) VALUES ('version', ?1)", -1, &stmt,
                     NULL);
  sqlite3_bind_int(stmt, 1, new_version);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  return new_version;
}

/* upgrade library db from 'version' to CURRENT_DATABASE_VERSION_LIBRARY. don't touch this function but
 * _upgrade_library_schema_step() instead. */
static gboolean _upgrade_library_schema(dt_database_t *db, int version)
{
  while(version < CURRENT_DATABASE_VERSION_LIBRARY)
  {
    int new_version = _upgrade_library_schema_step(db, version);
    if(new_version == version)
      return FALSE; // we don't know how to upgrade this db. probably a bug in _upgrade_library_schema_step
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
    int new_version = _upgrade_data_schema_step(db, version);
    if(new_version == version)
      return FALSE; // we don't know how to upgrade this db. probably a bug in _upgrade_data_schema_step
    else
      version = new_version;
  }
  return TRUE;
}

/* create the current database schema and set the version in db_info accordingly */
static void _create_library_schema(dt_database_t *db)
{
  sqlite3_stmt *stmt;
  ////////////////////////////// db_info
  sqlite3_exec(db->handle, "CREATE TABLE main.db_info (key VARCHAR PRIMARY KEY, value VARCHAR)", NULL,
               NULL, NULL);
  sqlite3_prepare_v2(
      db->handle, "INSERT OR REPLACE INTO main.db_info (key, value) VALUES ('version', ?1)", -1, &stmt, NULL);
  sqlite3_bind_int(stmt, 1, CURRENT_DATABASE_VERSION_LIBRARY);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  ////////////////////////////// film_rolls
  sqlite3_exec(db->handle,
               "CREATE TABLE main.film_rolls "
               "(id INTEGER PRIMARY KEY, datetime_accessed CHAR(20), "
               //                        "folder VARCHAR(1024), external_drive VARCHAR(1024))", //
               //                        FIXME: make sure to bump CURRENT_DATABASE_VERSION_LIBRARY and add a
               //                        case to _upgrade_library_schema_step when adding this!
               "folder VARCHAR(1024) NOT NULL)",
               NULL, NULL, NULL);
  sqlite3_exec(db->handle, "CREATE INDEX main.film_rolls_folder_index ON film_rolls (folder)", NULL, NULL, NULL);
  ////////////////////////////// images
  sqlite3_exec(
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
      "latitude REAL, altitude REAL, color_matrix BLOB, colorspace INTEGER, version INTEGER, "
      "max_version INTEGER, write_timestamp INTEGER, history_end INTEGER, position INTEGER, aspect_ratio REAL)",
      NULL, NULL, NULL);
  sqlite3_exec(db->handle, "CREATE INDEX main.images_group_id_index ON images (group_id)", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "CREATE INDEX main.images_film_id_index ON images (film_id)", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "CREATE INDEX main.images_filename_index ON images (filename)", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "CREATE INDEX main.image_position_index ON images (position)", NULL, NULL, NULL);

  ////////////////////////////// selected_images
  sqlite3_exec(db->handle, "CREATE TABLE main.selected_images (imgid INTEGER PRIMARY KEY)", NULL, NULL, NULL);
  ////////////////////////////// history
  sqlite3_exec(
      db->handle,
      "CREATE TABLE main.history (imgid INTEGER, num INTEGER, module INTEGER, "
      "operation VARCHAR(256), op_params BLOB, enabled INTEGER, "
      "blendop_params BLOB, blendop_version INTEGER, multi_priority INTEGER, multi_name VARCHAR(256))",
      NULL, NULL, NULL);
  sqlite3_exec(db->handle, "CREATE INDEX main.history_imgid_index ON history (imgid)", NULL, NULL, NULL);
  ////////////////////////////// mask
  sqlite3_exec(db->handle,
               "CREATE TABLE main.mask (imgid INTEGER, formid INTEGER, form INTEGER, name VARCHAR(256), "
               "version INTEGER, points BLOB, points_count INTEGER, source BLOB)",
               NULL, NULL, NULL);
  ////////////////////////////// tagged_images
  sqlite3_exec(db->handle, "CREATE TABLE main.tagged_images (imgid INTEGER, tagid INTEGER, "
                           "PRIMARY KEY (imgid, tagid))", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "CREATE INDEX main.tagged_images_tagid_index ON tagged_images (tagid)", NULL, NULL, NULL);
  ////////////////////////////// used_tags
  sqlite3_exec(db->handle, "CREATE TABLE main.used_tags (id INTEGER, name VARCHAR NOT NULL)", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "CREATE UNIQUE INDEX main.used_tags_idx ON used_tags (id, name)", NULL, NULL, NULL);
  ////////////////////////////// color_labels
  sqlite3_exec(db->handle, "CREATE TABLE main.color_labels (imgid INTEGER, color INTEGER)", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "CREATE UNIQUE INDEX main.color_labels_idx ON color_labels (imgid, color)", NULL, NULL,
               NULL);
  ////////////////////////////// meta_data
  sqlite3_exec(db->handle, "CREATE TABLE main.meta_data (id INTEGER, key INTEGER, value VARCHAR)", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "CREATE INDEX main.metadata_index ON meta_data (id, key)", NULL, NULL, NULL);
}

/* create the current database schema and set the version in db_info accordingly */
static void _create_data_schema(dt_database_t *db)
{
  sqlite3_stmt *stmt;
  ////////////////////////////// db_info
  sqlite3_exec(db->handle, "CREATE TABLE data.db_info (key VARCHAR PRIMARY KEY, value VARCHAR)", NULL,
               NULL, NULL);
  sqlite3_prepare_v2(
        db->handle, "INSERT OR REPLACE INTO data.db_info (key, value) VALUES ('version', ?1)", -1, &stmt, NULL);
  sqlite3_bind_int(stmt, 1, CURRENT_DATABASE_VERSION_DATA);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  ////////////////////////////// tags
  sqlite3_exec(db->handle, "CREATE TABLE data.tags (id INTEGER PRIMARY KEY, name VARCHAR, icon BLOB, "
                           "description VARCHAR, flags INTEGER)", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "CREATE UNIQUE INDEX data.tags_name_idx ON tags (name)", NULL, NULL, NULL);
  ////////////////////////////// styles
  sqlite3_exec(db->handle, "CREATE TABLE data.styles (id INTEGER, name VARCHAR, description VARCHAR)",
                        NULL, NULL, NULL);
  ////////////////////////////// style_items
  sqlite3_exec(
      db->handle,
      "CREATE TABLE data.style_items (styleid INTEGER, num INTEGER, module INTEGER, "
      "operation VARCHAR(256), op_params BLOB, enabled INTEGER, "
      "blendop_params BLOB, blendop_version INTEGER, multi_priority INTEGER, multi_name VARCHAR(256))",
      NULL, NULL, NULL);
  ////////////////////////////// presets
  sqlite3_exec(db->handle, "CREATE TABLE data.presets (name VARCHAR, description VARCHAR, operation "
                           "VARCHAR, op_version INTEGER, op_params BLOB, "
                           "enabled INTEGER, blendop_params BLOB, blendop_version INTEGER, "
                           "multi_priority INTEGER, multi_name VARCHAR(256), "
                           "model VARCHAR, maker VARCHAR, lens VARCHAR, iso_min REAL, iso_max REAL, "
                           "exposure_min REAL, exposure_max REAL, "
                           "aperture_min REAL, aperture_max REAL, focal_length_min REAL, "
                           "focal_length_max REAL, writeprotect INTEGER, "
                           "autoapply INTEGER, filter INTEGER, def INTEGER, format INTEGER)",
               NULL, NULL, NULL);
  sqlite3_exec(db->handle, "CREATE UNIQUE INDEX data.presets_idx ON presets (name, operation, op_version)",
               NULL, NULL, NULL);
}

// create the in-memory tables
// temporary stuff for some ops, need this for some reason with newer sqlite3:
static void _create_memory_schema(dt_database_t *db)
{
  sqlite3_exec(db->handle, "CREATE TABLE memory.color_labels_temp (imgid INTEGER PRIMARY KEY)", NULL, NULL, NULL);
  sqlite3_exec(
      db->handle,
      "CREATE TABLE memory.collected_images (rowid INTEGER PRIMARY KEY AUTOINCREMENT, imgid INTEGER)", NULL,
      NULL, NULL);
  sqlite3_exec(db->handle, "CREATE TABLE memory.tmp_selection (imgid INTEGER)", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "CREATE TABLE memory.tagq (tmpid INTEGER PRIMARY KEY, id INTEGER)", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "CREATE TABLE memory.taglist "
                           "(tmpid INTEGER PRIMARY KEY, id INTEGER UNIQUE ON CONFLICT IGNORE, count INTEGER)",
               NULL, NULL, NULL);
  sqlite3_exec(db->handle, "CREATE TABLE memory.similar_tags (tagid INTEGER)", NULL, NULL, NULL);
  sqlite3_exec(
      db->handle,
      "CREATE TABLE memory.history (imgid INTEGER, num INTEGER, module INTEGER, "
      "operation VARCHAR(256) UNIQUE ON CONFLICT REPLACE, op_params BLOB, enabled INTEGER, "
      "blendop_params BLOB, blendop_version INTEGER, multi_priority INTEGER, multi_name VARCHAR(256))",
      NULL, NULL, NULL);
  sqlite3_exec(
      db->handle,
      "CREATE TABLE memory.style_items (styleid INTEGER, num INTEGER, module INTEGER, "
      "operation VARCHAR(256), op_params BLOB, enabled INTEGER, "
      "blendop_params BLOB, blendop_version INTEGER, multi_priority INTEGER, multi_name VARCHAR(256))",
      NULL, NULL, NULL);
}

static void _sanitize_db(dt_database_t *db)
{
  sqlite3_stmt *stmt, *innerstmt;

  /* first let's get rid of non-utf8 tags. */
  sqlite3_prepare_v2(db->handle, "SELECT id, name FROM data.tags", -1, &stmt, NULL);
  sqlite3_prepare_v2(db->handle, "UPDATE data.tags SET name = ?1 WHERE id = ?2", -1, &innerstmt, NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    int id = sqlite3_column_int(stmt, 0);
    const char *tag = (const char *)sqlite3_column_text(stmt, 1);

    if(!g_utf8_validate(tag, -1, NULL))
    {
      gchar *new_tag = dt_util_foo_to_utf8(tag);
      fprintf(stderr, "[init]: tag `%s' is not valid utf8, replacing it with `%s'\n", tag, new_tag);
      if(tag)
      {
        sqlite3_bind_text(innerstmt, 1, new_tag, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(innerstmt, 2, id);
        sqlite3_step(innerstmt);
        sqlite3_reset(innerstmt);
        sqlite3_clear_bindings(innerstmt);
        g_free(new_tag);
      }
    }
  }
  sqlite3_finalize(stmt);
  sqlite3_finalize(innerstmt);
}

// in library we keep the names of the tags used in tagged_images. however, using that table at runtime results
// in some overhead not necessary so instead we just use the used_tags table to update tagged_images on startup
#define TRY_EXEC(_query, _message)                                                 \
  do                                                                               \
  {                                                                                \
    if(sqlite3_exec(db->handle, _query, NULL, NULL, NULL) != SQLITE_OK)            \
    {                                                                              \
      fprintf(stderr, _message);                                                   \
      fprintf(stderr, "[init]   %s\n", sqlite3_errmsg(db->handle));                \
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
      fprintf(stderr, _message);                                                   \
      fprintf(stderr, "[init]   %s\n", sqlite3_errmsg(db->handle));                \
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
      fprintf(stderr, _message);                                                   \
      fprintf(stderr, "[init]   %s\n", sqlite3_errmsg(db->handle));                \
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

static gboolean _synchronize_tags(dt_database_t *db)
{
  sqlite3_stmt *stmt = NULL;

  sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);

  // create temporary tables -- that has to be done outside the if() as the db is locked inside
  TRY_EXEC("CREATE TEMPORARY TABLE temp_used_tags (id INTEGER, name VARCHAR)",
           "[synchronize tags] can't create temporary table for used tags\n");
  TRY_EXEC("CREATE TEMPORARY TABLE temp_tagged_images (imgid INTEGER, tagid INTEGER)",
           "[synchronize tags] can't create temporary table for tagged images\n");

  // are the two databases in sync already?
  TRY_PREPARE(stmt, "SELECT COUNT(*) FROM main.used_tags AS u LEFT JOIN data.tags AS t USING (id, name) "
                    "WHERE u.id IS NULL OR t.id IS NULL",
              "[synchronize tags] can't prepare querying the number of tags that need to be synced\n");

  TRY_STEP(stmt, SQLITE_ROW, "[synchronize tags] can't query the number of tags that need to be synced\n");
  if(sqlite3_column_int(stmt, 0) > 0)
  {
    // insert tags that are only present in main.used_tags into data.tags
    TRY_EXEC("INSERT OR IGNORE INTO data.tags (name) SELECT name FROM main.used_tags",
             "[synchronize tags] can't import new tags from the library\n");

    // insert id, name for the tags in main.used_tags according to data.tags
    TRY_EXEC("INSERT INTO temp_used_tags (id, name) SELECT t.id, t.name FROM main.used_tags, data.tags "
             "AS t USING (name)", "[synchronize tags] can't collect used tags into temporary table\n");

    // insert updated valued into temp_tagged_images
    // FIXME: slowish!
    TRY_EXEC("INSERT INTO temp_tagged_images (imgid, tagid) SELECT imgid, new_id FROM main.tagged_images, "
             "(SELECT u.id AS old_id, tu.id AS new_id, name FROM used_tags AS u, temp_used_tags AS tu "
             "USING (name)) ON old_id = tagid",
             "[synchronize tags] can't insert updated image tagging into temporary table\n");

    // clear table to not get in conflict with indices
    TRY_EXEC("DELETE FROM main.tagged_images", "[synchronize tags] can't clear table `tagged_images'\n");
    TRY_EXEC("DELETE FROM main.used_tags", "[synchronize tags] can't clear table `used_tags'\n");

    // copy back to main.tagged_images
    // FIXME: slow with huge db! dropping the index first and adding it back in the end speeds it up a little
    TRY_EXEC("INSERT INTO main.tagged_images (imgid, tagid) SELECT imgid, tagid FROM temp_tagged_images",
             "[synchronize tags] can't update table `tagged_images`\n");

    // copy back to main.used_tags
    TRY_EXEC("INSERT INTO main.used_tags (id, name) SELECT id, name FROM temp_used_tags",
             "[synchronize tags] can't update table `used_tags'\n");
  }

  FINALIZE; // we need to finalize before dropping the tables due to locking issues!

  // drop temporary tables
  TRY_EXEC("DROP TABLE temp_tagged_images", "[synchronize tags] can't drop temporary table for tagged_images\n");
  TRY_EXEC("DROP TABLE temp_used_tags", "[synchronize tags] can't drop temporary table for used_tags\n");

  sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);

  return TRUE;
}

#undef TRY_EXEC
#undef TRY_STEP
#undef TRY_PREPARE
#undef FINALIZE

void dt_database_show_error(const dt_database_t *db)
{
  if(!db->lock_acquired)
  {
    char *label_text = g_markup_printf_escaped(_("an error has occurred while trying to open the database from\n"
                                                  "\n"
                                                  "<span style=\"italic\">%s</span>\n"
                                                  "\n"
                                                  "%s\n"),
                                                db->error_dbfilename, db->error_message ? db->error_message : "");

    dt_gui_show_standalone_yes_no_dialog(_("darktable - error locking database"), label_text, _("close darktable"),
                                         /*_("try again")*/NULL);

    g_free(label_text);
  }

  g_free(db->error_message);
  g_free(db->error_dbfilename);
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
  int fd = 0, lock_tries = 0;
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
    fd = g_open(*lockfile, O_RDWR | O_CREAT | O_EXCL, 0666);
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
          int other_pid = atoi(buf);
          if(!pid_is_alive(other_pid))
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
            fprintf(
              stderr,
              "[init] the database lock file contains a pid that seems to be alive in your system: %d\n",
              other_pid);
            db->error_message = g_strdup_printf(_("the database lock file contains a pid that seems to be alive in your system: %d"), other_pid);
          }
        }
        else
        {
          fprintf(stderr, "[init] the database lock file seems to be empty\n");
          db->error_message = g_strdup_printf(_("the database lock file seems to be empty"));
        }
        close(fd);
      }
      else
      {
        int err = errno;
        fprintf(stderr, "[init] error opening the database lock file for reading: %s\n", strerror(err));
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

dt_database_t *dt_database_init(const char *alternative, const gboolean load_data)
{
  /*  set the threading mode to Serialized */
  sqlite3_config(SQLITE_CONFIG_SERIALIZED);

  sqlite3_initialize();

start:
  /* migrate default database location to new default */
  _database_migrate_to_xdg_structure();

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
      snprintf(dbfilename_library, sizeof(dbfilename_library), "%s/library.db", datadir);
    else if(!strcmp(dbname, ":memory:"))
      snprintf(dbfilename_library, sizeof(dbfilename_library), "%s", dbname);
    else if(dbname[0] != '/')
      snprintf(dbfilename_library, sizeof(dbfilename_library), "%s/%s", datadir, dbname);
    else
      snprintf(dbfilename_library, sizeof(dbfilename_library), "%s", dbname);
  }
  else
  {
    snprintf(dbfilename_library, sizeof(dbfilename_library), "%s", alternative);

    GFile *galternative = g_file_new_for_path(alternative);
    dbname = g_file_get_basename(galternative);
    g_object_unref(galternative);
  }

  /* we also need a 2nd db with permanent data like presets, styles and tags */
  char dbfilename_data[PATH_MAX] = { 0 };
  if(load_data)
    snprintf(dbfilename_data, sizeof(dbfilename_data), "%s/data.db", datadir);
  else
    snprintf(dbfilename_data, sizeof(dbfilename_data), ":memory:");

  /* create database */
  dt_database_t *db = (dt_database_t *)g_malloc0(sizeof(dt_database_t));
  db->dbfilename_data = g_strdup(dbfilename_data);
  db->dbfilename_library = g_strdup(dbfilename_library);

  /* make sure the folder exists. this might not be the case for new databases */
  char *data_path = g_path_get_dirname(db->dbfilename_data);
  char *library_path = g_path_get_dirname(db->dbfilename_library);
  g_mkdir_with_parents(data_path, 0750);
  g_mkdir_with_parents(library_path, 0750);
  g_free(data_path);
  g_free(library_path);

  /* having more than one instance of darktable using the same database is a bad idea */
  /* try to get locks for the databases */
  db->lock_acquired = _lock_databases(db);

  if(!db->lock_acquired)
  {
    fprintf(stderr, "[init] database is locked, probably another process is already using it\n");
    g_free(dbname);
    return db;
  }


  /* opening / creating database */
  if(sqlite3_open(db->dbfilename_library, &db->handle))
  {
    fprintf(stderr, "[init] could not find database ");
    if(dbname)
      fprintf(stderr, "`%s'!\n", dbname);
    else
      fprintf(stderr, "\n");
    fprintf(stderr, "[init] maybe your %s/darktablerc is corrupt?\n", datadir);
    dt_loc_get_datadir(dbfilename_library, sizeof(dbfilename_library));
    fprintf(stderr, "[init] try `cp %s/darktablerc %s/darktablerc'\n", dbfilename_library, datadir);
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
    fprintf(stderr, "[init] database `%s' couldn't be opened. aborting\n", dbfilename_data);
    dt_database_destroy(db);
    db = NULL;
    goto error;
  }
  sqlite3_finalize(stmt);

  // some sqlite3 config
  sqlite3_exec(db->handle, "PRAGMA synchronous = OFF", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "PRAGMA journal_mode = MEMORY", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "PRAGMA page_size = 32768", NULL, NULL, NULL);

  /* now that we got functional databases that are locked for us we can make sure that the schema is set up */

  // first we update the data database to the latest version so that we can potentially move data from the library
  // over when updating that one
  if(!have_data_db)
  {
    _create_data_schema(db); // a brand new db it seems
  }
  else
  {
    rc = sqlite3_prepare_v2(db->handle, "select value from data.db_info where key = 'version'", -1, &stmt, NULL);
    if(rc == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW)
    {
      // compare the version of the db with what is current for this executable
      const int db_version = sqlite3_column_int(stmt, 0);
      sqlite3_finalize(stmt);
      if(db_version < CURRENT_DATABASE_VERSION_DATA)
      {
        // older: upgrade
        if(!_upgrade_data_schema(db, db_version))
        {
          // we couldn't upgrade the db for some reason. bail out.
          fprintf(stderr, "[init] database `%s' couldn't be upgraded from version %d to %d. aborting\n",
                  dbfilename_data, db_version, CURRENT_DATABASE_VERSION_DATA);
          dt_database_destroy(db);
          db = NULL;
          goto error;
        }
      }
      else if(db_version > CURRENT_DATABASE_VERSION_DATA)
      {
        // newer: bail out
        fprintf(stderr, "[init] database version of `%s' is too new for this build of darktable. aborting\n",
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

      char *label_text = g_markup_printf_escaped(_("an error has occurred while trying to open the database from\n"
                                                   "\n"
                                                   "<span style=\"italic\">%s</span>\n"
                                                   "\n"
                                                   "it seems that the database is corrupt.\n"
                                                   "do you want to close darktable now to manually restore\n"
                                                   "the database from a backup or start with a new one?"),
                                                 dbfilename_data);

      gboolean shall_we_delete_the_db =
          dt_gui_show_standalone_yes_no_dialog(_("darktable - error opening database"), label_text,
                                               _("close darktable"), _("delete database"));

      g_free(label_text);

      dt_database_destroy(db);
      db = NULL;

      if(shall_we_delete_the_db)
      {
        fprintf(stderr, "[init] deleting `%s' on user request", dbfilename_data);

        if(g_unlink(dbfilename_data) == 0)
          fprintf(stderr, " ... ok\n");
        else
          fprintf(stderr, " ... failed\n");

        goto start;
      }
      else
      {
        fprintf(stderr, "[init] database `%s' is corrupt and can't be opened! either replace it from a backup or "
        "delete the file so that darktable can create a new one the next time. aborting\n", dbfilename_data);
        goto error;
      }
    }
  }

  // next we are looking at the library database
  // does the db contain the new 'db_info' table?
  rc = sqlite3_prepare_v2(db->handle, "select value from main.db_info where key = 'version'", -1, &stmt, NULL);
  if(rc == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW)
  {
    // compare the version of the db with what is current for this executable
    const int db_version = sqlite3_column_int(stmt, 0);

    sqlite3_finalize(stmt);
    if(db_version < CURRENT_DATABASE_VERSION_LIBRARY)
    {
      // older: upgrade
      if(!_upgrade_library_schema(db, db_version))
      {
        // we couldn't upgrade the db for some reason. bail out.
        fprintf(stderr, "[init] database `%s' couldn't be upgraded from version %d to %d. aborting\n", dbname,
                db_version, CURRENT_DATABASE_VERSION_LIBRARY);
        dt_database_destroy(db);
        db = NULL;
        goto error;
      }
    }
    else if(db_version > CURRENT_DATABASE_VERSION_LIBRARY)
    {
      // newer: bail out. it's better than what we did before: delete everything
      fprintf(stderr, "[init] database version of `%s' is too new for this build of darktable. aborting\n",
              dbname);
      dt_database_destroy(db);
      db = NULL;
      goto error;
    }
    // else: the current version, do nothing
  }
  else if(rc == SQLITE_CORRUPT || rc == SQLITE_NOTADB)
  {
    // oh, bad situation. the database is corrupt and can't be read!
    // we inform the user here and let him decide what to do: exit or delete and try again.

    char *label_text = g_markup_printf_escaped(_("an error has occurred while trying to open the database from\n"
                                                  "\n"
                                                  "<span style=\"italic\">%s</span>\n"
                                                  "\n"
                                                  "it seems that the database is corrupt.\n"
                                                  "do you want to close darktable now to manually restore\n"
                                                  "the database from a backup or start with a new one?"),
                                               dbfilename_library);

    gboolean shall_we_delete_the_db =
        dt_gui_show_standalone_yes_no_dialog(_("darktable - error opening database"), label_text,
                                              _("close darktable"), _("delete database"));

    g_free(label_text);

    dt_database_destroy(db);
    db = NULL;

    if(shall_we_delete_the_db)
    {
      fprintf(stderr, "[init] deleting `%s' on user request", dbfilename_library);

      if(g_unlink(dbfilename_library) == 0)
        fprintf(stderr, " ... ok\n");
      else
        fprintf(stderr, " ... failed\n");

      goto start;
    }
    else
    {
      fprintf(stderr, "[init] database `%s' is corrupt and can't be opened! either replace it from a backup or "
                      "delete the file so that darktable can create a new one the next time. aborting\n", dbname);
      goto error;
    }
  }
  else
  {
    // does it contain the legacy 'settings' table?
    sqlite3_finalize(stmt);
    rc = sqlite3_prepare_v2(db->handle, "select settings from main.settings", -1, &stmt, NULL);
    if(rc == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW)
    {
      // the old blob had the version as an int in the first place
      const void *set = sqlite3_column_blob(stmt, 0);
      const int db_version = *(int *)set;
      sqlite3_finalize(stmt);
      if(!_migrate_schema(db, db_version)) // bring the legacy layout to the first one known to our upgrade
                                           // path ...
      {
        // we couldn't migrate the db for some reason. bail out.
        fprintf(stderr, "[init] database `%s' couldn't be migrated from the legacy version %d. aborting\n",
                dbname, db_version);
        dt_database_destroy(db);
        db = NULL;
        goto error;
      }
      if(!_upgrade_library_schema(db, 1)) // ... and upgrade it
      {
        // we couldn't upgrade the db for some reason. bail out.
        fprintf(stderr, "[init] database `%s' couldn't be upgraded from version 1 to %d. aborting\n", dbname,
                CURRENT_DATABASE_VERSION_LIBRARY);
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

  // create a table legacy_presets with all the presets from pre-auto-apply-cleanup darktable.
  dt_legacy_presets_create(db);

  // drop table settings -- we don't want old versions of dt to drop our tables
  sqlite3_exec(db->handle, "drop table main.settings", NULL, NULL, NULL);

  // take care of potential bad data in the db.
  _sanitize_db(db);

  // make sure that the tag ids in the library match the ones in data
  if(!_synchronize_tags(db))
  {
    fprintf(stderr, "[init] couldn't synchronize tags between library and data. aborting\n");
    dt_database_destroy(db);
    db = NULL;
    goto error;
  }

error:
  g_free(dbname);

  return db;
}

void dt_database_destroy(const dt_database_t *db)
{
  sqlite3_close(db->handle);
  if (db->lockfile_data)
  {
    g_unlink(db->lockfile_data);
    g_free(db->lockfile_data);
  }
  if (db->lockfile_library)
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
  gchar dbfilename[PATH_MAX] = { 0 };
  gchar *conf_db = dt_conf_get_string("database");

  gchar datadir[PATH_MAX] = { 0 };
  dt_loc_get_datadir(datadir, sizeof(datadir));

  if(conf_db && conf_db[0] != '/')
  {
    char *homedir = getenv("HOME");
    snprintf(dbfilename, sizeof(dbfilename), "%s/%s", homedir, conf_db);
    if(g_file_test(dbfilename, G_FILE_TEST_EXISTS))
    {
      fprintf(stderr, "[init] moving database into new XDG directory structure\n");
      char destdbname[PATH_MAX] = { 0 };
      snprintf(destdbname, sizeof(dbfilename), "%s/%s", datadir, "library.db");
      if(!g_file_test(destdbname, G_FILE_TEST_EXISTS))
      {
        rename(dbfilename, destdbname);
        dt_conf_set_string("database", "library.db");
      }
    }
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

  if(access(mipmapfilename, F_OK) != -1)
  {
    fprintf(stderr, "[mipmap_cache] dropping old version file: %s\n", mipmapfilename);
    g_unlink(mipmapfilename);

    snprintf(mipmapfilename, sizeof(mipmapfilename), "%s/mipmaps.fallback", cachedir);

    if(access(mipmapfilename, F_OK) != -1) g_unlink(mipmapfilename);
  }
}

gboolean dt_database_get_lock_acquired(const dt_database_t *db)
{
  return db->lock_acquired;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
