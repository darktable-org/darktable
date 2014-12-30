/*
    This file is part of darktable,
    copyright (c) 2011 henrik andersson.
    copyright (c) 2012 tobias ellinghaus.

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
#include "common/database.h"
#include "control/control.h"
#include "control/conf.h"
#include "gui/legacy_presets.h"

#include <sqlite3.h>
#include <glib.h>
#include <gio/gio.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>

// whenever _create_schema() gets changed you HAVE to bump this version and add an update path to
// _upgrade_schema_step()!
#define CURRENT_DATABASE_VERSION 8

typedef struct dt_database_t
{
  gboolean is_new_database;
  gboolean lock_acquired;

  /* database filename */
  gchar *dbfilename, *lockfile;

  /* ondisk DB */
  sqlite3 *handle;
} dt_database_t;


/* migrates database from old place to new */
static void _database_migrate_to_xdg_structure();

/* delete old mipmaps files */
static void _database_delete_mipmaps_files();

gboolean dt_database_is_new(const dt_database_t *db)
{
  return db->is_new_database;
}

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
  _SQLITE3_EXEC(db->handle, "DROP TABLE IF EXISTS lock", NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "DROP TABLE IF EXISTS settings", NULL, NULL, NULL); // yes, we do this in many
                                                                                // places. because it's really
                                                                                // important to not miss it in
                                                                                // any code path.
  _SQLITE3_EXEC(db->handle, "DROP INDEX IF EXISTS group_id_index", NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "DROP INDEX IF EXISTS imgid_index", NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "DROP TABLE IF EXISTS mipmaps", NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "DROP TABLE IF EXISTS mipmap_timestamps", NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "DROP TABLE IF EXISTS dt_migration_table", NULL, NULL, NULL);

  // using _create_schema() and filling that with the old data doesn't work since we always want to generate
  // version 1 tables
  ////////////////////////////// db_info
  _SQLITE3_EXEC(db->handle, "CREATE TABLE db_info (key VARCHAR PRIMARY KEY, value VARCHAR)", NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "INSERT OR REPLACE INTO db_info (key, value) VALUES ('version', 1)", NULL, NULL,
                NULL);
  ////////////////////////////// film_rolls
  _SQLITE3_EXEC(db->handle, "CREATE INDEX IF NOT EXISTS film_rolls_folder_index ON film_rolls (folder)", NULL,
                NULL, NULL);
  ////////////////////////////// images
  sqlite3_exec(db->handle, "ALTER TABLE images ADD COLUMN orientation INTEGER", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "ALTER TABLE images ADD COLUMN focus_distance REAL", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "ALTER TABLE images ADD COLUMN group_id INTEGER", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "ALTER TABLE images ADD COLUMN histogram BLOB", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "ALTER TABLE images ADD COLUMN lightmap BLOB", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "ALTER TABLE images ADD COLUMN longitude REAL", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "ALTER TABLE images ADD COLUMN latitude REAL", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "ALTER TABLE images ADD COLUMN color_matrix BLOB", NULL, NULL,
               NULL); // the color matrix
  sqlite3_exec(db->handle, "ALTER TABLE images ADD COLUMN colorspace INTEGER", NULL, NULL,
               NULL); // the colorspace as specified in some image types
  sqlite3_exec(db->handle, "ALTER TABLE images ADD COLUMN version INTEGER", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "ALTER TABLE images ADD COLUMN max_version INTEGER", NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "UPDATE images SET orientation = -1 WHERE orientation IS NULL", NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "UPDATE images SET focus_distance = -1 WHERE focus_distance IS NULL", NULL, NULL,
                NULL);
  _SQLITE3_EXEC(db->handle, "UPDATE images SET group_id = id WHERE group_id IS NULL", NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "UPDATE images SET max_version = (SELECT COUNT(*)-1 FROM images i WHERE "
                            "i.filename = images.filename AND "
                            "i.film_id = images.film_id) WHERE max_version IS NULL",
                NULL, NULL, NULL);
  _SQLITE3_EXEC(
      db->handle,
      "UPDATE images SET version = (SELECT COUNT(*) FROM images i WHERE i.filename = images.filename AND "
      "i.film_id = images.film_id AND i.id < images.id) WHERE version IS NULL",
      NULL, NULL, NULL);
  // make sure we have AUTOINCREMENT on imgid --> move the whole thing away and recreate the table :(
  _SQLITE3_EXEC(db->handle, "ALTER TABLE images RENAME TO dt_migration_table", NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "DROP INDEX IF EXISTS images_group_id_index", NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "DROP INDEX IF EXISTS images_film_id_index", NULL, NULL, NULL);
  _SQLITE3_EXEC(
      db->handle,
      "CREATE TABLE images (id INTEGER PRIMARY KEY AUTOINCREMENT, group_id INTEGER, film_id INTEGER, "
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
  _SQLITE3_EXEC(db->handle, "CREATE INDEX images_group_id_index ON images (group_id)", NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "CREATE INDEX images_film_id_index ON images (film_id)", NULL, NULL, NULL);
  _SQLITE3_EXEC(
      db->handle,
      "INSERT INTO images (id, group_id, film_id, width, height, filename, maker, model, "
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
  _SQLITE3_EXEC(db->handle, "INSERT INTO dt_migration_table SELECT imgid FROM selected_images", NULL, NULL,
                NULL);
  _SQLITE3_EXEC(db->handle, "DROP TABLE selected_images", NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "CREATE TABLE selected_images (imgid INTEGER PRIMARY KEY)", NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "INSERT OR IGNORE INTO selected_images SELECT imgid FROM dt_migration_table",
                NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "DROP TABLE dt_migration_table", NULL, NULL, NULL);
  ////////////////////////////// history
  sqlite3_exec(db->handle, "ALTER TABLE history ADD COLUMN blendop_params BLOB", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "ALTER TABLE history ADD COLUMN blendop_version INTEGER", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "ALTER TABLE history ADD COLUMN multi_priority INTEGER", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "ALTER TABLE history ADD COLUMN multi_name VARCHAR(256)", NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "CREATE INDEX IF NOT EXISTS history_imgid_index ON history (imgid)", NULL, NULL,
                NULL);
  _SQLITE3_EXEC(db->handle, "UPDATE history SET blendop_version = 1 WHERE blendop_version IS NULL", NULL,
                NULL, NULL);
  _SQLITE3_EXEC(db->handle, "UPDATE history SET multi_priority = 0 WHERE multi_priority IS NULL", NULL, NULL,
                NULL);
  _SQLITE3_EXEC(db->handle, "UPDATE history SET multi_name = ' ' WHERE multi_name IS NULL", NULL, NULL, NULL);
  ////////////////////////////// mask
  _SQLITE3_EXEC(db->handle, "CREATE TABLE IF NOT EXISTS mask (imgid INTEGER, formid INTEGER, form INTEGER, "
                            "name VARCHAR(256), version INTEGER, "
                            "points BLOB, points_count INTEGER, source BLOB)",
                NULL, NULL, NULL);
  sqlite3_exec(db->handle, "ALTER TABLE mask ADD COLUMN source BLOB", NULL, NULL,
               NULL); // in case the table was there already but missed that column
  ////////////////////////////// tagged_images
  _SQLITE3_EXEC(db->handle, "CREATE INDEX IF NOT EXISTS tagged_images_tagid_index ON tagged_images (tagid)",
                NULL, NULL, NULL);
  ////////////////////////////// styles
  _SQLITE3_EXEC(db->handle,
                "CREATE TABLE IF NOT EXISTS styles (id INTEGER, name VARCHAR, description VARCHAR)", NULL,
                NULL, NULL);
  sqlite3_exec(db->handle, "ALTER TABLE styles ADD COLUMN id INTEGER", NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "UPDATE styles SET id = rowid WHERE id IS NULL", NULL, NULL, NULL);
  ////////////////////////////// style_items
  _SQLITE3_EXEC(db->handle, "CREATE TABLE IF NOT EXISTS style_items (styleid INTEGER, num INTEGER, module "
                            "INTEGER, operation VARCHAR(256), op_params BLOB, "
                            "enabled INTEGER, blendop_params BLOB, blendop_version INTEGER, multi_priority "
                            "INTEGER, multi_name VARCHAR(256))",
                NULL, NULL, NULL);
  sqlite3_exec(db->handle, "ALTER TABLE style_items ADD COLUMN blendop_params BLOB", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "ALTER TABLE style_items ADD COLUMN blendop_version INTEGER", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "ALTER TABLE style_items ADD COLUMN multi_priority INTEGER", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "ALTER TABLE style_items ADD COLUMN multi_name VARCHAR(256)", NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "UPDATE style_items SET blendop_version = 1 WHERE blendop_version IS NULL", NULL,
                NULL, NULL);
  _SQLITE3_EXEC(db->handle, "UPDATE style_items SET multi_priority = 0 WHERE multi_priority IS NULL", NULL,
                NULL, NULL);
  _SQLITE3_EXEC(db->handle, "UPDATE style_items SET multi_name = ' ' WHERE multi_name IS NULL", NULL, NULL,
                NULL);
  ////////////////////////////// color_labels
  // color_labels could have a PRIMARY KEY that we don't want
  _SQLITE3_EXEC(db->handle, "CREATE TEMPORARY TABLE dt_migration_table (imgid INTEGER, color INTEGER)", NULL,
                NULL, NULL);
  _SQLITE3_EXEC(db->handle, "INSERT INTO dt_migration_table SELECT imgid, color FROM color_labels", NULL,
                NULL, NULL);
  _SQLITE3_EXEC(db->handle, "DROP TABLE color_labels", NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "CREATE TABLE color_labels (imgid INTEGER, color INTEGER)", NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "CREATE UNIQUE INDEX color_labels_idx ON color_labels (imgid, color)", NULL, NULL,
                NULL);
  _SQLITE3_EXEC(db->handle, "INSERT OR IGNORE INTO color_labels SELECT imgid, color FROM dt_migration_table",
                NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "DROP TABLE dt_migration_table", NULL, NULL, NULL);
  ////////////////////////////// meta_data
  _SQLITE3_EXEC(db->handle, "CREATE TABLE IF NOT EXISTS meta_data (id INTEGER, key INTEGER, value VARCHAR)",
                NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "CREATE INDEX IF NOT EXISTS metadata_index ON meta_data (id, key)", NULL, NULL,
                NULL);
  ////////////////////////////// presets
  _SQLITE3_EXEC(db->handle, "CREATE TABLE IF NOT EXISTS presets (name VARCHAR, description VARCHAR, "
                            "operation VARCHAR, op_version INTEGER, op_params BLOB, "
                            "enabled INTEGER, blendop_params BLOB, blendop_version INTEGER, multi_priority "
                            "INTEGER, multi_name VARCHAR(256), "
                            "model VARCHAR, maker VARCHAR, lens VARCHAR, iso_min REAL, iso_max REAL, "
                            "exposure_min REAL, exposure_max REAL, "
                            "aperture_min REAL, aperture_max REAL, focal_length_min REAL, focal_length_max "
                            "REAL, writeprotect INTEGER, "
                            "autoapply INTEGER, filter INTEGER, def INTEGER, isldr INTEGER)",
                NULL, NULL, NULL);
  sqlite3_exec(db->handle, "ALTER TABLE presets ADD COLUMN op_version INTEGER", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "ALTER TABLE presets ADD COLUMN blendop_params BLOB", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "ALTER TABLE presets ADD COLUMN blendop_version INTEGER", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "ALTER TABLE presets ADD COLUMN multi_priority INTEGER", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "ALTER TABLE presets ADD COLUMN multi_name VARCHAR(256)", NULL, NULL, NULL);
  // the unique index only works if the db doesn't have any (name, operation, op_version) more than once.
  // apparently there are dbs out there which do have that. :(
  sqlite3_prepare_v2(db->handle,
                     "SELECT p.rowid, p.name, p.operation, p.op_version FROM presets p INNER JOIN "
                     "(SELECT * FROM (SELECT rowid, name, operation, op_version, COUNT(*) AS count "
                     "FROM presets GROUP BY name, operation, op_version) WHERE count > 1) s "
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

    // find the next free ammended version of name
    sqlite3_prepare_v2(db->handle, "SELECT name FROM presets  WHERE name = ?1 || ' (' || ?2 || ')' AND "
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
    const char *query = "UPDATE presets SET name = name || ' (' || ?1 || ')' WHERE rowid = ?2";
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
                "CREATE UNIQUE INDEX IF NOT EXISTS presets_idx ON presets (name, operation, op_version)",
                NULL, NULL, NULL);
  _SQLITE3_EXEC(db->handle, "UPDATE presets SET blendop_version = 1 WHERE blendop_version IS NULL", NULL,
                NULL, NULL);
  _SQLITE3_EXEC(db->handle, "UPDATE presets SET multi_priority = 0 WHERE multi_priority IS NULL", NULL, NULL,
                NULL);
  _SQLITE3_EXEC(db->handle, "UPDATE presets SET multi_name = ' ' WHERE multi_name IS NULL", NULL, NULL, NULL);


  // There are systems where absolute paths don't start with '/' (like Windows).
  // Since the bug which introduced absolute paths to the db was fixed before a
  // Windows build was available this shouldn't matter though.
  sqlite3_prepare_v2(db->handle, "SELECT id, filename FROM images WHERE filename LIKE '/%'", -1, &stmt, NULL);
  sqlite3_prepare_v2(db->handle, "UPDATE images SET filename = ?1 WHERE id = ?2", -1, &innerstmt, NULL);
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
      "UPDATE images SET datetime_taken = REPLACE(datetime_taken, '-', ':') WHERE datetime_taken LIKE '%-%'",
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

/* do the real migration steps, returns the version the db was converted to */
static int _upgrade_schema_step(dt_database_t *db, int version)
{
  sqlite3_stmt *stmt;
  int new_version = version;
  if(version == CURRENT_DATABASE_VERSION)
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
    if(sqlite3_exec(db->handle, "ALTER TABLE images ADD COLUMN write_timestamp INTEGER", NULL, NULL, NULL)
       != SQLITE_OK)
    {
      fprintf(stderr, "[init] can't add `write_timestamp' column to database\n");
      fprintf(stderr, "[init]   %s\n", sqlite3_errmsg(db->handle));
      sqlite3_exec(db->handle, "ROLLBACK TRANSACTION", NULL, NULL, NULL);
      return version;
    }
    if(sqlite3_exec(db->handle,
                    "UPDATE images SET write_timestamp = STRFTIME('%s', 'now') WHERE write_timestamp IS NULL",
                    NULL, NULL, NULL) != SQLITE_OK)
    {
      fprintf(stderr,
              "[init] can't initialize `write_timestamp' with current point in time\n"); // let alone space
      fprintf(stderr, "[init]   %s\n", sqlite3_errmsg(db->handle));
      sqlite3_exec(db->handle, "ROLLBACK TRANSACTION", NULL, NULL, NULL);
      return version;
    }
    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
    new_version = 2;
  }
  else if(version == 2)
  {
    // 2 -> 3 reset raw_black and raw_maximum. in theory we should change the columns from REAL to INTEGER,
    // but sqlite doesn't care about types so whatever
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);
    if(sqlite3_exec(db->handle, "UPDATE images SET raw_black = 0, raw_maximum = 16384", NULL, NULL, NULL)
       != SQLITE_OK)
    {
      fprintf(stderr, "[init] can't reset raw_black and raw_maximum\n"); // let alone space
      fprintf(stderr, "[init]   %s\n", sqlite3_errmsg(db->handle));
      sqlite3_exec(db->handle, "ROLLBACK TRANSACTION", NULL, NULL, NULL);
      return version;
    }
    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
    new_version = 3;
  }
  else if(version == 3)
  {
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);

    if(sqlite3_exec(db->handle, "CREATE TRIGGER insert_tag AFTER INSERT ON tags"
                                " BEGIN"
                                "   INSERT INTO tagxtag SELECT id, new.id, 0 FROM TAGS;"
                                "   UPDATE tagxtag SET count = 1000000 WHERE id1=new.id AND id2=new.id;"
                                " END",
                    NULL, NULL, NULL) != SQLITE_OK)
    {
      fprintf(stderr, "[init] can't create insert_tag trigger\n");
      fprintf(stderr, "[init]   %s\n", sqlite3_errmsg(db->handle));
      sqlite3_exec(db->handle, "ROLLBACK TRANSACTION", NULL, NULL, NULL);
      return version;
    }
    if(sqlite3_exec(db->handle, "CREATE TRIGGER delete_tag BEFORE DELETE on tags"
                                " BEGIN"
                                "   DELETE FROM tagxtag WHERE id1=old.id OR id2=old.id;"
                                "   DELETE FROM tagged_images WHERE tagid=old.id;"
                                " END",
                    NULL, NULL, NULL) != SQLITE_OK)
    {
      fprintf(stderr, "[init] can't create delete_tag trigger\n");
      fprintf(stderr, "[init]   %s\n", sqlite3_errmsg(db->handle));
      sqlite3_exec(db->handle, "ROLLBACK TRANSACTION", NULL, NULL, NULL);
      return version;
    }
    if(sqlite3_exec(
           db->handle,
           "CREATE TRIGGER attach_tag AFTER INSERT ON tagged_images"
           " BEGIN"
           "   UPDATE tagxtag"
           "     SET count = count + 1"
           "     WHERE (id1=new.tagid AND id2 IN (SELECT tagid FROM tagged_images WHERE imgid=new.imgid))"
           "        OR (id2=new.tagid AND id1 IN (SELECT tagid FROM tagged_images WHERE imgid=new.imgid));"
           " END",
           NULL, NULL, NULL) != SQLITE_OK)
    {
      fprintf(stderr, "[init] can't create attach_tag trigger\n");
      fprintf(stderr, "[init]   %s\n", sqlite3_errmsg(db->handle));
      sqlite3_exec(db->handle, "ROLLBACK TRANSACTION", NULL, NULL, NULL);
      return version;
    }
    if(sqlite3_exec(
           db->handle,
           "CREATE TRIGGER detach_tag BEFORE DELETE ON tagged_images"
           " BEGIN"
           "   UPDATE tagxtag"
           "     SET count = count - 1"
           "     WHERE (id1=old.tagid AND id2 IN (SELECT tagid FROM tagged_images WHERE imgid=old.imgid))"
           "        OR (id2=old.tagid AND id1 IN (SELECT tagid FROM tagged_images WHERE imgid=old.imgid));"
           " END",
           NULL, NULL, NULL) != SQLITE_OK)
    {
      fprintf(stderr, "[init] can't create detach_tag trigger\n");
      fprintf(stderr, "[init]   %s\n", sqlite3_errmsg(db->handle));
      sqlite3_exec(db->handle, "ROLLBACK TRANSACTION", NULL, NULL, NULL);
      return version;
    }

    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
    new_version = 4;
  }
  else if(version == 4)
  {
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);

    if(sqlite3_exec(db->handle, "ALTER TABLE presets RENAME TO tmp_presets", NULL, NULL, NULL) != SQLITE_OK)
    {
      fprintf(stderr, "[init] can't rename table presets\n");
      fprintf(stderr, "[init]   %s\n", sqlite3_errmsg(db->handle));
      sqlite3_exec(db->handle, "ROLLBACK TRANSACTION", NULL, NULL, NULL);
      return version;
    }

    if(sqlite3_exec(
           db->handle,
           "CREATE TABLE presets (name VARCHAR, description VARCHAR, operation VARCHAR, op_params BLOB,"
           "enabled INTEGER, blendop_params BLOB, model VARCHAR, maker VARCHAR, lens VARCHAR,"
           "iso_min REAL, iso_max REAL, exposure_min REAL, exposure_max REAL, aperture_min REAL,"
           "aperture_max REAL, focal_length_min REAL, focal_length_max REAL, writeprotect INTEGER,"
           "autoapply INTEGER, filter INTEGER, def INTEGER, format INTEGER, op_version INTEGER,"
           "blendop_version INTEGER, multi_priority INTEGER, multi_name VARCHAR(256))",
           NULL, NULL, NULL) != SQLITE_OK)
    {
      fprintf(stderr, "[init] can't create new presets table\n");
      fprintf(stderr, "[init]   %s\n", sqlite3_errmsg(db->handle));
      sqlite3_exec(db->handle, "ROLLBACK TRANSACTION", NULL, NULL, NULL);
      return version;
    }

    if(sqlite3_exec(
           db->handle,
           "INSERT INTO presets (name, description, operation, op_params, enabled, blendop_params, model, "
           "maker, lens,"
           "                     iso_min, iso_max, exposure_min, exposure_max, aperture_min, aperture_max,"
           "                     focal_length_min, focal_length_max, writeprotect, autoapply, filter, def, "
           "format,"
           "                     op_version, blendop_version, multi_priority, multi_name)"
           "              SELECT name, description, operation, op_params, enabled, blendop_params, model, "
           "maker, lens,"
           "                     iso_min, iso_max, exposure_min, exposure_max, aperture_min, aperture_max,"
           "                     focal_length_min, focal_length_max, writeprotect, autoapply, filter, def, "
           "isldr,"
           "                     op_version, blendop_version, multi_priority, multi_name"
           "              FROM   tmp_presets",
           NULL, NULL, NULL) != SQLITE_OK)
    {
      fprintf(stderr, "[init] can't populate presets table from tmp_presets\n");
      fprintf(stderr, "[init]   %s\n", sqlite3_errmsg(db->handle));
      sqlite3_exec(db->handle, "ROLLBACK TRANSACTION", NULL, NULL, NULL);
      return version;
    }

    if(sqlite3_exec(db->handle, "DROP TABLE tmp_presets", NULL, NULL, NULL) != SQLITE_OK)
    {
      fprintf(stderr, "[init] can't delete table tmp_presets\n");
      fprintf(stderr, "[init]   %s\n", sqlite3_errmsg(db->handle));
      sqlite3_exec(db->handle, "ROLLBACK TRANSACTION", NULL, NULL, NULL);
      return version;
    }

    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
    new_version = 5;
  }
  else if(version == 5)
  {
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);

    if(sqlite3_exec(db->handle, "CREATE INDEX images_filename_index ON images (filename)", NULL, NULL, NULL)
       != SQLITE_OK)
    {
      fprintf(stderr, "[init] can't create index on image filename\n");
      fprintf(stderr, "[init]   %s\n", sqlite3_errmsg(db->handle));
      sqlite3_exec(db->handle, "ROLLBACK TRANSACTION", NULL, NULL, NULL);
      return version;
    }

    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
    new_version = 6;
  }
  else if(version == 6)
  {
    // some ancient tables can have the styleid column of style_items be called style_id. fix that.
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);

    if(sqlite3_exec(db->handle, "SELECT style_id FROM style_items", NULL, NULL, NULL) == SQLITE_OK)
    {
      if(sqlite3_exec(db->handle, "ALTER TABLE style_items RENAME TO tmp_style_items", NULL, NULL, NULL)
         != SQLITE_OK)
      {
        fprintf(stderr, "[init] can't rename table style_items\n");
        fprintf(stderr, "[init]   %s\n", sqlite3_errmsg(db->handle));
        sqlite3_exec(db->handle, "ROLLBACK TRANSACTION", NULL, NULL, NULL);
        return version;
      }

      if(sqlite3_exec(
             db->handle,
             "CREATE TABLE style_items (styleid INTEGER, num INTEGER, module INTEGER, "
             "operation VARCHAR(256), op_params BLOB, enabled INTEGER, "
             "blendop_params BLOB, blendop_version INTEGER, multi_priority INTEGER, multi_name VARCHAR(256))",
             NULL, NULL, NULL) != SQLITE_OK)
      {
        fprintf(stderr, "[init] can't create new style_items table\n");
        fprintf(stderr, "[init]   %s\n", sqlite3_errmsg(db->handle));
        sqlite3_exec(db->handle, "ROLLBACK TRANSACTION", NULL, NULL, NULL);
        return version;
      }

      if(sqlite3_exec(db->handle,
                      "INSERT INTO style_items (styleid, num, module, operation, op_params, enabled,"
                      "                         blendop_params, blendop_version, multi_priority, multi_name)"
                      "                  SELECT style_id, num, module, operation, op_params, enabled,"
                      "                         blendop_params, blendop_version, multi_priority, multi_name"
                      "                  FROM   tmp_style_items",
                      NULL, NULL, NULL) != SQLITE_OK)
      {
        fprintf(stderr, "[init] can't populate style_items table from tmp_style_items\n");
        fprintf(stderr, "[init]   %s\n", sqlite3_errmsg(db->handle));
        sqlite3_exec(db->handle, "ROLLBACK TRANSACTION", NULL, NULL, NULL);
        return version;
      }

      if(sqlite3_exec(db->handle, "DROP TABLE tmp_style_items", NULL, NULL, NULL) != SQLITE_OK)
      {
        fprintf(stderr, "[init] can't delete table tmp_style_items\n");
        fprintf(stderr, "[init]   %s\n", sqlite3_errmsg(db->handle));
        sqlite3_exec(db->handle, "ROLLBACK TRANSACTION", NULL, NULL, NULL);
        return version;
      }
    }

    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
    new_version = 7;
  }
  else if(version == 7)
  {
    // make sure that we have no film rolls with a NULL folder
    sqlite3_exec(db->handle, "BEGIN TRANSACTION", NULL, NULL, NULL);

    if(sqlite3_exec(db->handle, "ALTER TABLE film_rolls RENAME TO tmp_film_rolls", NULL, NULL, NULL)
       != SQLITE_OK)
    {
      fprintf(stderr, "[init] can't rename table film_rolls\n");
      fprintf(stderr, "[init]   %s\n", sqlite3_errmsg(db->handle));
      sqlite3_exec(db->handle, "ROLLBACK TRANSACTION", NULL, NULL, NULL);
      return version;
    }

    if(sqlite3_exec(db->handle, "CREATE TABLE film_rolls "
                                "(id INTEGER PRIMARY KEY, datetime_accessed CHAR(20), "
                                "folder VARCHAR(1024) NOT NULL)",
                    NULL, NULL, NULL) != SQLITE_OK)
    {
      fprintf(stderr, "[init] can't create new film_rolls table\n");
      fprintf(stderr, "[init]   %s\n", sqlite3_errmsg(db->handle));
      sqlite3_exec(db->handle, "ROLLBACK TRANSACTION", NULL, NULL, NULL);
      return version;
    }

    if(sqlite3_exec(db->handle, "INSERT INTO film_rolls (id, datetime_accessed, folder) "
                                "SELECT id, datetime_accessed, folder "
                                "FROM   tmp_film_rolls "
                                "WHERE  folder IS NOT NULL",
                    NULL, NULL, NULL) != SQLITE_OK)
    {
      fprintf(stderr, "[init] can't populate film_rolls table from tmp_film_rolls\n");
      fprintf(stderr, "[init]   %s\n", sqlite3_errmsg(db->handle));
      sqlite3_exec(db->handle, "ROLLBACK TRANSACTION", NULL, NULL, NULL);
      return version;
    }

    if(sqlite3_exec(db->handle, "DROP TABLE tmp_film_rolls", NULL, NULL, NULL) != SQLITE_OK)
    {
      fprintf(stderr, "[init] can't delete table tmp_film_rolls\n");
      fprintf(stderr, "[init]   %s\n", sqlite3_errmsg(db->handle));
      sqlite3_exec(db->handle, "ROLLBACK TRANSACTION", NULL, NULL, NULL);
      return version;
    }

    sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL);
    new_version = 8;
  } // maybe in the future, see commented out code elsewhere
    //   else if(version == XXX)
    //   {
    //     sqlite3_exec(db->handle, "ALTER TABLE film_rolls ADD COLUMN external_drive VARCHAR(1024)", NULL,
    //     NULL, NULL);
    //   }
  else
    new_version = version; // should be the fallback so that calling code sees that we are in an infinite loop

  // write the new version to db
  DT_DEBUG_SQLITE3_PREPARE_V2(
      db->handle, "INSERT OR REPLACE INTO db_info (key, value) VALUES ('version', ?1)", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, new_version);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  return new_version;
}

/* upgrade db from 'version' to CURRENT_DATABASE_VERSION . don't touch this function but
 * _upgrade_schema_step() instead. */
static gboolean _upgrade_schema(dt_database_t *db, int version)
{
  while(version < CURRENT_DATABASE_VERSION)
  {
    int new_version = _upgrade_schema_step(db, version);
    if(new_version == version)
      return FALSE; // we don't know how to upgrade this db. probably a bug in _upgrade_schema_step
    else
      version = new_version;
  }
  return TRUE;
}

/* create the current database schema and set the version in db_info accordingly */
static void _create_schema(dt_database_t *db)
{
  sqlite3_stmt *stmt;
  ////////////////////////////// db_info
  DT_DEBUG_SQLITE3_EXEC(db->handle, "CREATE TABLE db_info (key VARCHAR PRIMARY KEY, value VARCHAR)", NULL,
                        NULL, NULL);
  DT_DEBUG_SQLITE3_PREPARE_V2(
      db->handle, "INSERT OR REPLACE INTO db_info (key, value) VALUES ('version', ?1)", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, CURRENT_DATABASE_VERSION);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  ////////////////////////////// film_rolls
  DT_DEBUG_SQLITE3_EXEC(db->handle,
                        "CREATE TABLE film_rolls "
                        "(id INTEGER PRIMARY KEY, datetime_accessed CHAR(20), "
                        //                        "folder VARCHAR(1024), external_drive VARCHAR(1024))", //
                        //                        FIXME: make sure to bump CURRENT_DATABASE_VERSION and add a
                        //                        case to _upgrade_schema_step when adding this!
                        "folder VARCHAR(1024) NOT NULL)",
                        NULL, NULL, NULL);
  DT_DEBUG_SQLITE3_EXEC(db->handle, "CREATE INDEX film_rolls_folder_index ON film_rolls (folder)", NULL, NULL,
                        NULL);
  ////////////////////////////// images
  DT_DEBUG_SQLITE3_EXEC(
      db->handle,
      "CREATE TABLE images (id INTEGER PRIMARY KEY AUTOINCREMENT, group_id INTEGER, film_id INTEGER, "
      "width INTEGER, height INTEGER, filename VARCHAR, maker VARCHAR, model VARCHAR, "
      "lens VARCHAR, exposure REAL, aperture REAL, iso REAL, focal_length REAL, "
      "focus_distance REAL, datetime_taken CHAR(20), flags INTEGER, "
      "output_width INTEGER, output_height INTEGER, crop REAL, "
      "raw_parameters INTEGER, raw_denoise_threshold REAL, "
      "raw_auto_bright_threshold REAL, raw_black INTEGER, raw_maximum INTEGER, "
      "caption VARCHAR, description VARCHAR, license VARCHAR, sha1sum CHAR(40), "
      "orientation INTEGER, histogram BLOB, lightmap BLOB, longitude REAL, "
      "latitude REAL, color_matrix BLOB, colorspace INTEGER, version INTEGER, max_version INTEGER, "
      "write_timestamp INTEGER)",
      NULL, NULL, NULL);
  DT_DEBUG_SQLITE3_EXEC(db->handle, "CREATE INDEX images_group_id_index ON images (group_id)", NULL, NULL,
                        NULL);
  DT_DEBUG_SQLITE3_EXEC(db->handle, "CREATE INDEX images_film_id_index ON images (film_id)", NULL, NULL, NULL);
  DT_DEBUG_SQLITE3_EXEC(db->handle, "CREATE INDEX images_filename_index ON images (filename)", NULL, NULL,
                        NULL);
  ////////////////////////////// selected_images
  DT_DEBUG_SQLITE3_EXEC(db->handle, "CREATE TABLE selected_images (imgid INTEGER PRIMARY KEY)", NULL, NULL,
                        NULL);
  ////////////////////////////// history
  DT_DEBUG_SQLITE3_EXEC(
      db->handle,
      "CREATE TABLE history (imgid INTEGER, num INTEGER, module INTEGER, "
      "operation VARCHAR(256), op_params BLOB, enabled INTEGER, "
      "blendop_params BLOB, blendop_version INTEGER, multi_priority INTEGER, multi_name VARCHAR(256))",
      NULL, NULL, NULL);
  DT_DEBUG_SQLITE3_EXEC(db->handle, "CREATE INDEX history_imgid_index ON history (imgid)", NULL, NULL, NULL);
  ////////////////////////////// mask
  DT_DEBUG_SQLITE3_EXEC(db->handle,
                        "CREATE TABLE mask (imgid INTEGER, formid INTEGER, form INTEGER, name VARCHAR(256), "
                        "version INTEGER, points BLOB, points_count INTEGER, source BLOB)",
                        NULL, NULL, NULL);
  ////////////////////////////// tags
  DT_DEBUG_SQLITE3_EXEC(db->handle, "CREATE TABLE tags (id INTEGER PRIMARY KEY, name VARCHAR, icon BLOB, "
                                    "description VARCHAR, flags INTEGER)",
                        NULL, NULL, NULL);
  ////////////////////////////// tagged_images
  DT_DEBUG_SQLITE3_EXEC(db->handle, "CREATE TABLE tagged_images (imgid INTEGER, tagid INTEGER, "
                                    "PRIMARY KEY (imgid, tagid))",
                        NULL, NULL, NULL);
  DT_DEBUG_SQLITE3_EXEC(db->handle, "CREATE INDEX tagged_images_tagid_index ON tagged_images (tagid)", NULL,
                        NULL, NULL);
  ////////////////////////////// tagxtag
  DT_DEBUG_SQLITE3_EXEC(db->handle, "CREATE TABLE tagxtag (id1 INTEGER, id2 INTEGER, count INTEGER, "
                                    "PRIMARY KEY (id1, id2))",
                        NULL, NULL, NULL);
  DT_DEBUG_SQLITE3_EXEC(db->handle, "CREATE TRIGGER insert_tag AFTER INSERT ON tags"
                                    " BEGIN"
                                    "   INSERT INTO tagxtag SELECT id, new.id, 0 FROM TAGS;"
                                    "   UPDATE tagxtag SET count = 1000000 WHERE id1=new.id AND id2=new.id;"
                                    " END",
                        NULL, NULL, NULL);
  DT_DEBUG_SQLITE3_EXEC(db->handle, "CREATE TRIGGER delete_tag BEFORE DELETE on tags"
                                    " BEGIN"
                                    "   DELETE FROM tagxtag WHERE id1=old.id OR id2=old.id;"
                                    "   DELETE FROM tagged_images WHERE tagid=old.id;"
                                    " END",
                        NULL, NULL, NULL);
  DT_DEBUG_SQLITE3_EXEC(
      db->handle,
      "CREATE TRIGGER attach_tag AFTER INSERT ON tagged_images"
      " BEGIN"
      "   UPDATE tagxtag"
      "     SET count = count + 1"
      "     WHERE (id1=new.tagid AND id2 IN (SELECT tagid FROM tagged_images WHERE imgid=new.imgid))"
      "        OR (id2=new.tagid AND id1 IN (SELECT tagid FROM tagged_images WHERE imgid=new.imgid));"
      " END",
      NULL, NULL, NULL);
  DT_DEBUG_SQLITE3_EXEC(
      db->handle,
      "CREATE TRIGGER detach_tag BEFORE DELETE ON tagged_images"
      " BEGIN"
      "   UPDATE tagxtag"
      "     SET count = count - 1"
      "     WHERE (id1=old.tagid AND id2 IN (SELECT tagid FROM tagged_images WHERE imgid=old.imgid))"
      "        OR (id2=old.tagid AND id1 IN (SELECT tagid FROM tagged_images WHERE imgid=old.imgid));"
      " END",
      NULL, NULL, NULL);
  ////////////////////////////// styles
  DT_DEBUG_SQLITE3_EXEC(db->handle, "CREATE TABLE styles (id INTEGER, name VARCHAR, description VARCHAR)",
                        NULL, NULL, NULL);
  ////////////////////////////// style_items
  DT_DEBUG_SQLITE3_EXEC(
      db->handle,
      "CREATE TABLE style_items (styleid INTEGER, num INTEGER, module INTEGER, "
      "operation VARCHAR(256), op_params BLOB, enabled INTEGER, "
      "blendop_params BLOB, blendop_version INTEGER, multi_priority INTEGER, multi_name VARCHAR(256))",
      NULL, NULL, NULL);
  ////////////////////////////// color_labels
  DT_DEBUG_SQLITE3_EXEC(db->handle, "CREATE TABLE color_labels (imgid INTEGER, color INTEGER)", NULL, NULL,
                        NULL);
  DT_DEBUG_SQLITE3_EXEC(db->handle, "CREATE UNIQUE INDEX color_labels_idx ON color_labels (imgid, color)",
                        NULL, NULL, NULL);
  ////////////////////////////// meta_data
  DT_DEBUG_SQLITE3_EXEC(db->handle, "CREATE TABLE meta_data (id INTEGER, key INTEGER, value VARCHAR)", NULL,
                        NULL, NULL);
  DT_DEBUG_SQLITE3_EXEC(db->handle, "CREATE INDEX metadata_index ON meta_data (id, key)", NULL, NULL, NULL);
  ////////////////////////////// presets
  DT_DEBUG_SQLITE3_EXEC(db->handle, "CREATE TABLE presets (name VARCHAR, description VARCHAR, operation "
                                    "VARCHAR, op_version INTEGER, op_params BLOB, "
                                    "enabled INTEGER, blendop_params BLOB, blendop_version INTEGER, "
                                    "multi_priority INTEGER, multi_name VARCHAR(256), "
                                    "model VARCHAR, maker VARCHAR, lens VARCHAR, iso_min REAL, iso_max REAL, "
                                    "exposure_min REAL, exposure_max REAL, "
                                    "aperture_min REAL, aperture_max REAL, focal_length_min REAL, "
                                    "focal_length_max REAL, writeprotect INTEGER, "
                                    "autoapply INTEGER, filter INTEGER, def INTEGER, format INTEGER)",
                        NULL, NULL, NULL);
  DT_DEBUG_SQLITE3_EXEC(db->handle, "CREATE UNIQUE INDEX presets_idx ON presets(name, operation, op_version)",
                        NULL, NULL, NULL);
}

static void _sanitize_db(dt_database_t *db)
{
  sqlite3_stmt *stmt, *innerstmt;

  /* first let's get rid of non-utf8 tags. */
  sqlite3_prepare_v2(db->handle, "SELECT id, name FROM tags", -1, &stmt, NULL);
  sqlite3_prepare_v2(db->handle, "UPDATE tags SET name = ?1 WHERE id = ?2", -1, &innerstmt, NULL);
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

dt_database_t *dt_database_init(char *alternative)
{
  /* migrate default database location to new default */
  _database_migrate_to_xdg_structure();

  /* delete old mipmaps files */
  _database_delete_mipmaps_files();

  /* lets construct the db filename  */
  gchar *dbname = NULL;
  gchar dbfilename[PATH_MAX] = { 0 };
  gchar datadir[PATH_MAX] = { 0 };

  dt_loc_get_user_config_dir(datadir, sizeof(datadir));

  if(alternative == NULL)
  {
    dbname = dt_conf_get_string("database");
    if(!dbname)
      snprintf(dbfilename, sizeof(dbfilename), "%s/library.db", datadir);
    else if(dbname[0] != '/')
      snprintf(dbfilename, sizeof(dbfilename), "%s/%s", datadir, dbname);
    else
      snprintf(dbfilename, sizeof(dbfilename), "%s", dbname);
  }
  else
  {
    snprintf(dbfilename, sizeof(dbfilename), "%s", alternative);

    GFile *galternative = g_file_new_for_path(alternative);
    dbname = g_file_get_basename(galternative);
    g_object_unref(galternative);
  }

  /* create database */
  dt_database_t *db = (dt_database_t *)g_malloc0(sizeof(dt_database_t));
  db->dbfilename = g_strdup(dbfilename);
  db->is_new_database = FALSE;
  db->lock_acquired = FALSE;

/* having more than one instance of darktable using the same database is a bad idea */
/* try to get a lock for the database */
#ifdef __WIN32__
  db->lock_acquired = TRUE;
#else
  mode_t old_mode;
  int fd = 0, lock_tries = 0;
  if(!strcmp(dbfilename, ":memory:"))
  {
    db->lock_acquired = TRUE;
  }
  else
  {
    db->lockfile = g_strconcat(dbfilename, ".lock", NULL);
  lock_again:
    lock_tries++;
    old_mode = umask(0);
    fd = open(db->lockfile, O_RDWR | O_CREAT | O_EXCL, 0666);
    umask(old_mode);

    if(fd != -1) // the lockfile was successfully created - write our PID into it
    {
      gchar *pid = g_strdup_printf("%d", getpid());
      if(write(fd, pid, strlen(pid) + 1) > -1) db->lock_acquired = TRUE;
      close(fd);
    }
    else // the lockfile already exists - see if it's a stale one left over from a crashed instance
    {
      char buf[64];
      memset(buf, 0, sizeof(buf));
      fd = open(db->lockfile, O_RDWR | O_CREAT, 0666);
      if(fd != -1)
      {
        if(read(fd, buf, sizeof(buf) - 1) > -1)
        {
          int other_pid = atoi(buf);
          if((kill(other_pid, 0) == -1) && errno == ESRCH)
          {
            // the other process seems to no longer exist. unlink the .lock file and try again
            unlink(db->lockfile);
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
          }
        }
        else
        {
          fprintf(stderr, "[init] the database lock file seems to be empty\n");
        }
        close(fd);
      }
      else
      {
        fprintf(stderr, "[init] error opening the database lock file for reading\n");
      }
    }
  }
#endif

  if(!db->lock_acquired)
  {
    fprintf(stderr, "[init] database is locked, probably another process is already using it\n");
    g_free(dbname);
    return db;
  }

  /* test if databasefile is available */
  if(!g_file_test(dbfilename, G_FILE_TEST_IS_REGULAR)) db->is_new_database = TRUE;

  /* opening / creating database */
  if(sqlite3_open(db->dbfilename, &db->handle))
  {
    fprintf(stderr, "[init] could not find database ");
    if(dbname)
      fprintf(stderr, "`%s'!\n", dbname);
    else
      fprintf(stderr, "\n");
    fprintf(stderr, "[init] maybe your %s/darktablerc is corrupt?\n", datadir);
    dt_loc_get_datadir(dbfilename, sizeof(dbfilename));
    fprintf(stderr, "[init] try `cp %s/darktablerc %s/darktablerc'\n", dbfilename, datadir);
    sqlite3_close(db->handle);
    g_free(dbname);
    g_free(db->lockfile);
    g_free(db);
    return NULL;
  }

  /* attach a memory database to db connection for use with temporary tables
     used during instance life time, which is discarded on exit.
  */
  sqlite3_exec(db->handle, "attach database ':memory:' as memory", NULL, NULL, NULL);

  sqlite3_exec(db->handle, "PRAGMA synchronous = OFF", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "PRAGMA journal_mode = MEMORY", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "PRAGMA page_size = 32768", NULL, NULL, NULL);

  /* now that we got a functional database that is locked for us we can make sure that the schema is set up */
  // does the db contain the new 'db_info' table?
  sqlite3_stmt *stmt;
  int rc = sqlite3_prepare_v2(db->handle, "select value from db_info where key = 'version'", -1, &stmt, NULL);
  if(rc == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW)
  {
    // compare the version of the db with what is current for this executable
    const int db_version = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    if(db_version < CURRENT_DATABASE_VERSION)
    {
      // older: upgrade
      if(!_upgrade_schema(db, db_version))
      {
        // we couldn't upgrade the db for some reason. bail out.
        fprintf(stderr, "[init] database `%s' couldn't be upgraded from version %d to %d. aborting\n", dbname,
                db_version, CURRENT_DATABASE_VERSION);
        dt_database_destroy(db);
        db = NULL;
        goto error;
      }
    }
    else if(db_version > CURRENT_DATABASE_VERSION)
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
  else
  {
    // does it contain the legacy 'settings' table?
    sqlite3_finalize(stmt);
    rc = sqlite3_prepare_v2(db->handle, "select settings from settings", -1, &stmt, NULL);
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
      if(!_upgrade_schema(db, 1)) // ... and upgrade it
      {
        // we couldn't upgrade the db for some reason. bail out.
        fprintf(stderr, "[init] database `%s' couldn't be upgraded from version 1 to %d. aborting\n", dbname,
                CURRENT_DATABASE_VERSION);
        dt_database_destroy(db);
        db = NULL;
        goto error;
      }
    }
    else
    {
      sqlite3_finalize(stmt);
      _create_schema(db); // a brand new db it seems
    }
  }

  // create the in-memory tables
  // temporary stuff for some ops, need this for some reason with newer sqlite3:
  DT_DEBUG_SQLITE3_EXEC(db->handle, "CREATE TABLE memory.color_labels_temp (imgid INTEGER PRIMARY KEY)", NULL,
                        NULL, NULL);
  DT_DEBUG_SQLITE3_EXEC(
      db->handle,
      "CREATE TABLE memory.collected_images (rowid INTEGER PRIMARY KEY AUTOINCREMENT, imgid INTEGER)", NULL,
      NULL, NULL);
  DT_DEBUG_SQLITE3_EXEC(db->handle, "CREATE TABLE memory.tmp_selection (imgid INTEGER)", NULL, NULL, NULL);
  DT_DEBUG_SQLITE3_EXEC(db->handle, "CREATE TABLE memory.tagq (tmpid INTEGER PRIMARY KEY, id INTEGER)", NULL,
                        NULL, NULL);
  DT_DEBUG_SQLITE3_EXEC(db->handle, "CREATE TABLE memory.taglist "
                                    "(tmpid INTEGER PRIMARY KEY, id INTEGER UNIQUE ON CONFLICT REPLACE, "
                                    "count INTEGER)",
                        NULL, NULL, NULL);
  DT_DEBUG_SQLITE3_EXEC(
      db->handle,
      "CREATE TABLE memory.history (imgid INTEGER, num INTEGER, module INTEGER, "
      "operation VARCHAR(256) UNIQUE ON CONFLICT REPLACE, op_params BLOB, enabled INTEGER, "
      "blendop_params BLOB, blendop_version INTEGER, multi_priority INTEGER, multi_name VARCHAR(256))",
      NULL, NULL, NULL);
  DT_DEBUG_SQLITE3_EXEC(
      db->handle,
      "CREATE TABLE MEMORY.style_items (styleid INTEGER, num INTEGER, module INTEGER, "
      "operation VARCHAR(256), op_params BLOB, enabled INTEGER, "
      "blendop_params BLOB, blendop_version INTEGER, multi_priority INTEGER, multi_name VARCHAR(256))",
      NULL, NULL, NULL);

  // create a table legacy_presets with all the presets from pre-auto-apply-cleanup darktable.
  dt_legacy_presets_create(db);

  // drop table settings -- we don't want old versions of dt to drop our tables
  sqlite3_exec(db->handle, "drop table settings", NULL, NULL, NULL);

  // take care of potential bad data in the db.
  _sanitize_db(db);

error:
  g_free(dbname);

  return db;
}

void dt_database_destroy(const dt_database_t *db)
{
  sqlite3_close(db->handle);
  unlink(db->lockfile);
  g_free(db->lockfile);
  g_free((dt_database_t *)db);
}

sqlite3 *dt_database_get(const dt_database_t *db)
{
  return db->handle;
}

const gchar *dt_database_get_path(const struct dt_database_t *db)
{
  return db->dbfilename;
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
    unlink(mipmapfilename);

    snprintf(mipmapfilename, sizeof(mipmapfilename), "%s/mipmaps.fallback", cachedir);

    if(access(mipmapfilename, F_OK) != -1) unlink(mipmapfilename);
  }
}

gboolean dt_database_get_lock_acquired(const dt_database_t *db)
{
  return db->lock_acquired;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
