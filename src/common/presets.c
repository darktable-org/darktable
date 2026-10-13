/*
    This file is part of darktable,
    Copyright (C) 2019-2026 darktable developers.

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

#include "common/presets.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/exif.h"
#include "common/utility.h"
#include "develop/blend.h"
#include "develop/imageop.h"
#include "libs/lib.h"
#include <glib-2.0/gio/gmenu.h>
#include <libxml/encoding.h>
#include <libxml/xmlwriter.h>
#include <libxml/parser.h>
#include <libxml/xpath.h>
#include <libxml/xpathInternals.h>

#include <float.h>
#include <glib.h>
#include <inttypes.h>
#include <math.h>
#include <sqlite3.h>

static char *_preset_encode(sqlite3_stmt *stmt,
                            const int row)
{
  const int32_t len = sqlite3_column_bytes(stmt, row);
  char *vparams = dt_exif_xmp_encode
    ((const unsigned char *)sqlite3_column_blob(stmt, row), len, NULL);
  return vparams;
}

void dt_presets_save_to_file(const int rowid,
                             const char *preset_name,
                             const char *filedir)
{
  sqlite3_stmt *stmt;

  // generate filename based on name of preset
  // convert all characters to underscore which are not allowed in filenames
  gchar *presetname = g_strdup(preset_name);
  gchar *filename = g_strdup_printf("%s/%s.dtpreset", filedir,
                                    g_strdelimit(presetname, "/<>:\"\\|*?[]", '_'));

  g_free(presetname);

  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2
    (dt_database_get(darktable.db),
     "SELECT op_params, blendop_params, name, description, operation,"
     "   autoapply, model, maker, lens, iso_min, iso_max, exposure_min,"
     "   exposure_max, aperture_min, aperture_max, focal_length_min,"
     "   focal_length_max, op_version, blendop_version, enabled,"
     "   multi_priority, multi_name, filter, def, format, multi_name_hand_edited"
     " FROM data.presets"
     " WHERE rowid = ?1",
     -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, rowid);

  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const gchar *name = (gchar *)sqlite3_column_text(stmt, 2);
    const gchar *description = (gchar *)sqlite3_column_text(stmt, 3);
    const gchar *operation = (gchar *)sqlite3_column_text(stmt, 4);
    const int autoapply = sqlite3_column_int(stmt, 5);
    const gchar *model = (gchar *)sqlite3_column_text(stmt, 6);
    const gchar *maker = (gchar *)sqlite3_column_text(stmt, 7);
    const gchar *lens = (gchar *)sqlite3_column_text(stmt, 8);
    const double iso_min = sqlite3_column_double(stmt, 9);
    const double iso_max = sqlite3_column_double(stmt, 10);
    const double exposure_min = sqlite3_column_double(stmt, 11);
    const double exposure_max = sqlite3_column_double(stmt, 12);
    const double aperture_min = sqlite3_column_double(stmt, 13);
    const double aperture_max = sqlite3_column_double(stmt, 14);
    const int focal_length_min = sqlite3_column_double(stmt, 15);
    const int focal_length_max = sqlite3_column_double(stmt, 16);
    const int op_version = sqlite3_column_int(stmt, 17);
    const int blendop_version = sqlite3_column_int(stmt, 18);
    const int enabled = sqlite3_column_int(stmt, 19);
    const int multi_priority = sqlite3_column_int(stmt, 20);
    const gchar *multi_name = (gchar *)sqlite3_column_text(stmt, 21);
    const int filter = sqlite3_column_double(stmt, 22);
    const int def = sqlite3_column_double(stmt, 23);
    const int format = sqlite3_column_double(stmt, 24);
    const int multi_name_hand_edited = sqlite3_column_double(stmt, 25);

    int rc = 0;

    xmlTextWriterPtr writer = xmlNewTextWriterFilename(filename, 0);

    if(writer == NULL)
    {
      dt_print(DT_DEBUG_ALWAYS,
               "[dt_presets_save_to_file] Error creating the xml writer\n, path: %s",
               filename);
      g_free(filename);
      return;
    }

    rc = xmlTextWriterStartDocument(writer, NULL, "UTF-8", NULL);
    if(rc < 0)
    {
      dt_print(DT_DEBUG_ALWAYS,
               "[dt_presets_save_to_file]: Error on encoding setting");
      g_free(filename);
      return;
    }

    xmlTextWriterStartElement(writer, BAD_CAST "darktable_preset");
    xmlTextWriterWriteAttribute(writer, BAD_CAST "version", BAD_CAST "1.0");

    xmlTextWriterStartElement(writer, BAD_CAST "preset");
    xmlTextWriterWriteFormatElement(writer, BAD_CAST "name", "%s", name);
    xmlTextWriterWriteFormatElement(writer, BAD_CAST "description", "%s", description);
    xmlTextWriterWriteFormatElement(writer, BAD_CAST "operation", "%s", operation);
    xmlTextWriterWriteFormatElement(writer, BAD_CAST "op_params", "%s",
                                    _preset_encode(stmt, 0));
    xmlTextWriterWriteFormatElement(writer, BAD_CAST "op_version", "%d", op_version);
    xmlTextWriterWriteFormatElement(writer, BAD_CAST "enabled", "%d", enabled);
    xmlTextWriterWriteFormatElement(writer, BAD_CAST "autoapply", "%d", autoapply);
    xmlTextWriterWriteFormatElement(writer, BAD_CAST "model", "%s", model);
    xmlTextWriterWriteFormatElement(writer, BAD_CAST "maker", "%s", maker);
    xmlTextWriterWriteFormatElement(writer, BAD_CAST "lens", "%s", lens);
    xmlTextWriterWriteFormatElement(writer, BAD_CAST "iso_min", "%f", iso_min);
    xmlTextWriterWriteFormatElement(writer, BAD_CAST "iso_max", "%f", iso_max);
    xmlTextWriterWriteFormatElement(writer, BAD_CAST "exposure_min", "%f", exposure_min);
    xmlTextWriterWriteFormatElement(writer, BAD_CAST "exposure_max", "%f", exposure_max);
    xmlTextWriterWriteFormatElement(writer, BAD_CAST "aperture_min", "%f", aperture_min);
    xmlTextWriterWriteFormatElement(writer, BAD_CAST "aperture_max", "%f", aperture_max);
    xmlTextWriterWriteFormatElement(writer, BAD_CAST "focal_length_min", "%d",
                                    focal_length_min);
    xmlTextWriterWriteFormatElement(writer, BAD_CAST "focal_length_max", "%d",
                                    focal_length_max);
    xmlTextWriterWriteFormatElement(writer, BAD_CAST "blendop_params", "%s",
                                    _preset_encode(stmt, 1));
    xmlTextWriterWriteFormatElement(writer, BAD_CAST "blendop_version", "%d",
                                    blendop_version);
    xmlTextWriterWriteFormatElement(writer, BAD_CAST "multi_priority", "%d",
                                    multi_priority);
    xmlTextWriterWriteFormatElement(writer, BAD_CAST "multi_name", "%s", multi_name);
    xmlTextWriterWriteFormatElement(writer, BAD_CAST "multi_name_hand_edited", "%d",
                                    multi_name_hand_edited);
    xmlTextWriterWriteFormatElement(writer, BAD_CAST "filter", "%d", filter);
    xmlTextWriterWriteFormatElement(writer, BAD_CAST "def", "%d", def);
    xmlTextWriterWriteFormatElement(writer, BAD_CAST "format", "%d", format);
    xmlTextWriterEndElement(writer);

    sqlite3_finalize(stmt);
    xmlTextWriterEndDocument(writer);
    xmlFreeTextWriter(writer);
  }
  g_free(filename);
}

static gchar *get_preset_element(xmlDocPtr doc, gchar *name)
{
  xmlXPathContextPtr xpathCtx = xmlXPathNewContext(doc);
  char xpath[128] = { 0 };
  snprintf(xpath, sizeof(xpath), "//%s", name);
  gchar *result = NULL;

  xmlXPathObjectPtr xpathObj =
    xmlXPathEvalExpression((const xmlChar *)xpath, xpathCtx);

  if(xpathObj)
  {
    const xmlNodeSetPtr xnodes = xpathObj->nodesetval;
    if(xnodes->nodeTab)
    {
      const xmlNodePtr xnode = xnodes->nodeTab[0];
      xmlChar *value = xmlNodeListGetString(doc, xnode->xmlChildrenNode, 1);

      if(value)
        result = g_strdup((gchar *)value);
      else
        result = g_strdup("");

      xmlFree(value);
    }
    xmlXPathFreeObject(xpathObj);
  }

  xmlXPathFreeContext(xpathCtx);
  return result;
}

static int get_preset_element_int(xmlDocPtr doc, gchar *name)
{
  gchar *value = get_preset_element(doc, name);
  const int result = value ? atoi(value) : 0;
  g_free(value);
  return result;
}

static double get_preset_element_double(xmlDocPtr doc, gchar *name)
{
  gchar *value = get_preset_element(doc, name);
  const double result = value ? atof(value) : 0.0;
  g_free(value);
  return result;
}

gboolean dt_presets_import_from_file(const char *preset_path)
{
  xmlDocPtr doc = xmlParseFile(preset_path);
  if(!doc)
    return FALSE;

  xmlNodePtr root = xmlDocGetRootElement(doc);
  if(!root || xmlStrcmp(root->name, BAD_CAST "darktable_preset") != 0)
  {
    xmlFreeDoc(doc);
    return FALSE;
  }

  gchar *name = get_preset_element(doc, "name");
  gchar *description = get_preset_element(doc, "description");
  gchar *operation = get_preset_element(doc, "operation");
  const int autoapply = get_preset_element_int(doc, "autoapply");
  gchar *model = get_preset_element(doc, "model");
  gchar *maker = get_preset_element(doc, "maker");
  gchar *lens = get_preset_element(doc, "lens");
  const double iso_min = get_preset_element_double(doc, "iso_min");
  const double iso_max = get_preset_element_double(doc, "iso_max");
  const double exposure_min = get_preset_element_double(doc, "exposure_min");
  const double exposure_max = get_preset_element_double(doc, "exposure_max");
  const double aperture_min = get_preset_element_double(doc, "aperture_min");
  const double aperture_max = get_preset_element_double(doc, "aperture_max");
  const int focal_length_min = get_preset_element_int(doc, "focal_length_min");
  const int focal_length_max = get_preset_element_int(doc, "focal_length_max");
  gchar *op_params = get_preset_element(doc, "op_params");
  const int op_version = get_preset_element_int(doc, "op_version");
  gchar *blendop_params = get_preset_element(doc, "blendop_params");
  const int blendop_version = get_preset_element_int(doc, "blendop_version");
  const int enabled = get_preset_element_int(doc, "enabled");
  const int multi_priority = get_preset_element_int(doc, "multi_priority");
  gchar *multi_name = get_preset_element(doc, "multi_name");
  const int multi_name_hand_edited = get_preset_element_int(doc, "multi_name_hand_edited");
  const int filter = get_preset_element_int(doc, "filter");
  const int def = get_preset_element_int(doc, "def");
  const int format = get_preset_element_int(doc, "format");
  xmlFreeDoc(doc);

  int blendop_params_len = 0;
  const unsigned char *blendop_params_blob = dt_exif_xmp_decode
    (blendop_params, strlen(blendop_params), &blendop_params_len);

  int op_params_len = 0;
  const unsigned char *op_params_blob = dt_exif_xmp_decode
    (op_params, strlen(op_params), &op_params_len);

  sqlite3_stmt *stmt;
  int result = 0;

  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
     "INSERT OR REPLACE"
     "  INTO data.presets"
     "    (name, description, operation, autoapply,"
     "     model, maker, lens, iso_min, iso_max, exposure_min, exposure_max,"
     "     aperture_min, aperture_max, focal_length_min, focal_length_max,"
     "     op_params, op_version, blendop_params, blendop_version, enabled,"
     "     multi_priority, multi_name, filter, def, format, multi_name_hand_edited,"
     "     writeprotect)"
     "  VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, "
     "          ?15, ?16, ?17, ?18, ?19, ?20, ?21, ?22, ?23, ?24, ?25, ?26, 0)",
     -1, &stmt, NULL);
  // clang-format on

  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, name, strlen(name), SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, description, strlen(description), SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, operation, strlen(operation), SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 4, autoapply);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 5, model, strlen(model), SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 6, maker, strlen(maker), SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 7, lens, strlen(lens), SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 8, iso_min);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 9, iso_max);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 10, exposure_min);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 11, exposure_max);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 12, aperture_min);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 13, aperture_max);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 14, focal_length_min);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 15, focal_length_max);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 16, op_params_blob, op_params_len, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 17, op_version);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 18, blendop_params_blob, blendop_params_len,
                             SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 19, blendop_version);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 20, enabled);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 21, multi_priority);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 22, multi_name, strlen(multi_name), SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 23, filter);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 24, def);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 25, format);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 26, multi_name_hand_edited);

  result = (sqlite3_step(stmt) == SQLITE_DONE);

  sqlite3_finalize(stmt);

  g_free(name);
  g_free(description);
  g_free(operation);
  g_free(model);
  g_free(maker);
  g_free(lens);
  g_free(op_params);
  g_free(blendop_params);
  g_free(multi_name);

  return result;
}

gboolean dt_presets_module_can_autoapply(const gchar *operation)
{
  for(const GList *lib_modules = darktable.lib->plugins;
      lib_modules;
      lib_modules = g_list_next(lib_modules))
  {
    dt_lib_module_t *lib_module = lib_modules->data;
    if(!strcmp(lib_module->plugin_name, operation))
    {
      return dt_lib_presets_can_autoapply(lib_module);
    }
  }
  return TRUE;
}

char *dt_presets_get_filter(const dt_image_t *image)
{
  const gboolean is_raw = dt_image_is_rawprepare_supported(image);
  const gboolean has_matrix = dt_image_is_matrix_correction_supported(image);

  const gint raw = is_raw ? FOR_RAW : FOR_LDR;
  const gint mat = has_matrix ? FOR_MATRIX : 0xFFFF;
  const gint exl = dt_image_monochrome_flags(image) ? FOR_NOT_MONO : FOR_NOT_COLOR;
  const gint hdr = dt_image_is_hdr(image) ? FOR_HDR : 0xFFFF;

  // The rules for matching are:
  // R1. Match presets with RAW or LDR or MATRIX flag. If the picture has no matrix we
  //     ignore the preset MATRIX flag.
  // R2. If R1 matches, then we want to macth also HDR presets if image is HDR.
  // R3. If R1 matches, we want either presets for color or monochome images or both.

  return g_strdup_printf
    ("format = 0"
     " OR ((format&%d == %d OR format&%d == %d)"
     "     AND format&%d != 0"
     "     AND ~format&%d != 0)",
     raw, raw,
     mat, mat,
     hdr,
     exl);
}

gboolean dt_presets_blend_params_match(const dt_develop_blend_params_t *bp,
                                       const void *blob,
                                       const int size)
{
  if(size <= 0 || !blob) return TRUE;
  if(!dt_masks_blend_blob_has_tail(size))
    return !memcmp(bp, blob, MIN((size_t)size, sizeof(dt_develop_blend_params_t)));

  dt_develop_blend_params_t stored;
  memcpy(&stored, blob, sizeof(stored));
  stored.mask_id = bp->mask_id;
  return !memcmp(bp, &stored, sizeof(stored));
}

gboolean dt_presets_module_blend_match(const dt_iop_module_t *module,
                                       const void *blob,
                                       const int size)
{
  if(!dt_presets_blend_params_match(module->blend_params, blob, size)) return FALSE;
  if(size <= 0 || !blob || !dt_masks_blend_blob_has_tail(size)) return TRUE;
  return dt_masks_blend_blob_forms_match(module->blend_params,
                                         module->dev ? module->dev->forms : NULL,
                                         blob, size);
}

gchar *dt_get_active_preset_name(dt_iop_module_t *module,
                                 gboolean *writeprotect)
{
  sqlite3_stmt *stmt;
  // if we sort by writeprotect DESC then in case user copied the
  // writeprotected preset then the preset name returned will be
  // writeprotected and thus not deletable sorting ASC prefers user
  // created presets.
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(
    dt_database_get(darktable.db),
     "SELECT name, op_params, blendop_params, enabled, writeprotect"
     " FROM data.presets"
     " WHERE operation=?1 AND op_version=?2"
     " ORDER BY writeprotect ASC, LOWER(name), rowid",
     -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, module->op, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, module->version());

  gchar *name = NULL;
  *writeprotect = FALSE;

  // collect all presets for op from db
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const void *op_params = (void *)sqlite3_column_blob(stmt, 1);
    const int32_t op_params_size = sqlite3_column_bytes(stmt, 1);
    const void *blendop_params = (void *)sqlite3_column_blob(stmt, 2);
    const int32_t bl_params_size = sqlite3_column_bytes(stmt, 2);
    const int enabled = sqlite3_column_int(stmt, 3);

    if(((op_params_size == 0
         && !memcmp(module->default_params, module->params, module->params_size))
        || ((op_params_size > 0
             && !memcmp(module->params, op_params,
                        MIN(op_params_size, module->params_size)))))
       && dt_presets_module_blend_match(module, blendop_params, bl_params_size)
       && module->enabled == enabled)
    {
      name = g_strdup((char *)sqlite3_column_text(stmt, 0));
      *writeprotect = sqlite3_column_int(stmt, 4);
      break;
    }
  }
  sqlite3_finalize(stmt);
  return name;
}

char *dt_presets_get_module_label(const char *module_name,
                                  const void *params,
                                  const uint32_t param_size,
                                  const gboolean is_default_params,
                                  const void *blend_params,
                                  const uint32_t blend_params_size)
{
  const gboolean auto_module = dt_conf_get_bool("darkroom/ui/auto_module_name_update");

  if(!auto_module)
    return NULL;

  sqlite3_stmt *stmt;

  // clang-format off
  // a preset carrying the forms of its mask is longer than the blend params,
  // and is compared in C (dt_presets_blend_params_match())
  char *query = g_strdup_printf("SELECT name, multi_name, blendop_params"
                                " FROM data.presets"
                                " WHERE operation = ?1"
                                "   AND (op_params = ?2"
                                "        %s)"
                                "   AND (blendop_params = ?3"
                                "        OR LENGTH(blendop_params) > ?4)",
                                is_default_params ? "OR op_params IS NULL" : "");

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);

  // clang-format on
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, module_name, strlen(module_name), SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 2, params, param_size, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 3, blend_params, blend_params_size, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 4, (int)sizeof(dt_develop_blend_params_t));

  char *result = NULL;

  // returns the preset's multi_name if defined otherwise the preset
  // name is returned.
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const void *stored = sqlite3_column_blob(stmt, 2);
    const int stored_size = sqlite3_column_bytes(stmt, 2);
    if(dt_masks_blend_blob_has_tail(stored_size)
       && (blend_params_size != sizeof(dt_develop_blend_params_t)
           || !dt_presets_blend_params_match(blend_params, stored, stored_size)))
      continue;

    const char *name = (const char *)sqlite3_column_text(stmt, 0);
    const char *multi_name = (const char *)sqlite3_column_text(stmt, 1);
    if(multi_name && (strlen(multi_name) == 0 || multi_name[0] != ' '))
      result = dt_presets_get_multi_name(name, multi_name, FALSE);
    break;
  }
  g_free(query);
  sqlite3_finalize(stmt);

  return result;
}

char *dt_presets_get_multi_name(const char *name,
                                const char *multi_name,
                                const gboolean localize)
{
  const gboolean auto_module = dt_conf_get_bool("darkroom/ui/auto_module_name_update");

  // in auto-update mode     : use either the multi_name if defined otherwise the name
  // in non auto-update mode : use only the multi_name if defined
  if(auto_module)
    return strlen(multi_name) > 0
      ? g_strdup(multi_name)
      : (localize ? dt_util_localize_segmented_name(name, FALSE) : g_strdup(name));
  else
    return g_strdup(strlen(multi_name) > 0 ? multi_name : "");
}

void dt_insert_preset_in_menu_hierarchy(const char *name,
                                        const char *action,
                                        GSList **menu_path,
                                        GMenu *mainmenu,
                                        GMenu **submenu,
                                        gchar ***prev_split,
                                        gboolean isdefault,
                                        gboolean writeprotect)
{
  gchar *local_name =
    writeprotect ?
    dt_util_localize_segmented_name(name, FALSE)
    : g_strdup(name);

  gchar **split = g_strsplit(local_name, "|", -1);
  gchar **s = split;
  gchar **p = *prev_split;
  GSList *mpath = *menu_path;
  GMenuItem *mi;
  g_free(local_name);

  for(; p && *(p+1) && *(s+1) && !g_strcmp0(*s, *p); p++, s++)
    ;

  for(; p && *(p+1); p++)
  {
    mpath = g_slist_delete_link(mpath, mpath); // pop
    *submenu = mpath ? mpath->data : mainmenu;
  }

  for(; *(s+1); s++)
  {
    GMenu *sm = g_menu_new();
    gchar *label_unm = NULL;
    label_unm = dt_str_unmnemonic(*s);
    GMenuItem *smi = g_menu_item_new_submenu(label_unm, G_MENU_MODEL(sm));
    g_free(label_unm);

    g_menu_append_item(*submenu, smi);
    g_object_unref(smi);
    *submenu = sm;
    mpath = g_slist_prepend(mpath, sm); // push
  }

  *menu_path = mpath;
  g_strfreev(*prev_split);
  *prev_split = split;

  if(isdefault)
  {
    gchar *label = g_strdup_printf("%s %s", *s, _("(default)"));
    gchar *label_unm = dt_str_unmnemonic(label);
    mi = g_menu_item_new(label_unm, action);
    g_menu_append_item(*submenu, mi);
    g_object_unref(mi);
    g_free(label);
    g_free(label_unm);
  }
  else
  {
    gchar *label_unm = dt_str_unmnemonic(*s);
    mi = g_menu_item_new(label_unm, action);
    g_menu_append_item(*submenu, mi);
    g_object_unref(mi);
    g_free(label_unm);
  }
}

gboolean dt_presets_exists(const char *name,
                           const char *operation,
                           const int op_version)
{
  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get(darktable.db),
      "SELECT name"
      " FROM data.presets"
      " WHERE name = ?1 AND operation=?2 AND op_version=?3"
      " LIMIT 1",
      -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, name, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, operation, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, op_version);
  const gboolean exists = sqlite3_step(stmt) == SQLITE_ROW;
  sqlite3_finalize(stmt);
  return exists;
}

gboolean dt_presets_get_operation(const int rowid,
                                  char **operation,
                                  int *op_version)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT operation, op_version"
                              " FROM data.presets"
                              " WHERE rowid = ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, rowid);
  const gboolean found = sqlite3_step(stmt) == SQLITE_ROW;
  if(found)
  {
    *operation = g_strdup((const char *)sqlite3_column_text(stmt, 0));
    *op_version = sqlite3_column_int(stmt, 1);
  }
  sqlite3_finalize(stmt);
  return found;
}

void dt_presets_delete_unprotected(const int rowid)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "DELETE FROM data.presets"
                              " WHERE rowid=?1 AND writeprotect=0", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, rowid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

gboolean dt_presets_get_details(const char *name,
                                const char *operation,
                                const int op_version,
                                dt_presets_details_t *d)
{
  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2
    (dt_database_get(darktable.db),
     "SELECT rowid, description, model, maker, lens, iso_min, iso_max, "
     "       exposure_min, exposure_max, aperture_min, aperture_max, focal_length_min, "
     "       focal_length_max, autoapply, filter, format, op_params"
     " FROM data.presets"
     " WHERE name = ?1 AND operation = ?2 AND op_version = ?3",
     -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, name, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, operation, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, op_version);
  const gboolean found = sqlite3_step(stmt) == SQLITE_ROW;
  if(found)
  {
    d->rowid = sqlite3_column_int(stmt, 0);
    d->description = g_strdup((const char *)sqlite3_column_text(stmt, 1));
    d->model = g_strdup((const char *)sqlite3_column_text(stmt, 2));
    d->maker = g_strdup((const char *)sqlite3_column_text(stmt, 3));
    d->lens = g_strdup((const char *)sqlite3_column_text(stmt, 4));
    d->iso_min = g_strdup((const char *)sqlite3_column_text(stmt, 5));
    d->iso_max = g_strdup((const char *)sqlite3_column_text(stmt, 6));
    d->exposure_min = sqlite3_column_double(stmt, 7);
    d->exposure_max = sqlite3_column_double(stmt, 8);
    d->aperture_min = sqlite3_column_double(stmt, 9);
    d->aperture_max = sqlite3_column_double(stmt, 10);
    d->focal_length_min = sqlite3_column_double(stmt, 11);
    d->focal_length_max = sqlite3_column_double(stmt, 12);
    d->autoapply = sqlite3_column_int(stmt, 13);
    d->filter = sqlite3_column_int(stmt, 14);
    d->format = sqlite3_column_int(stmt, 15);
    d->op_params_size = sqlite3_column_bytes(stmt, 16);
  }
  sqlite3_finalize(stmt);
  return found;
}

void dt_presets_details_clear(dt_presets_details_t *d)
{
  g_free(d->description);
  g_free(d->model);
  g_free(d->maker);
  g_free(d->lens);
  g_free(d->iso_min);
  g_free(d->iso_max);
  memset(d, 0, sizeof(*d));
}

void dt_presets_write(const int rowid, const dt_presets_write_t *p)
{
  gchar *query = NULL;
  if(rowid >= 0)
  {
    // clang-format off
    query = g_strdup_printf
      ("UPDATE data.presets "
       "SET"
       "  name=?1, description=?2,"
       "  model=?3, maker=?4, lens=?5, iso_min=?6, iso_max=?7, exposure_min=?8,"
       "  exposure_max=?9, aperture_min=?10,"
       "  aperture_max=?11, focal_length_min=?12, focal_length_max=?13, autoapply=?14,"
       "  filter=?15, format=?16 %s"
       " WHERE rowid=%d",
       p->iop
         ? ", op_params=?19, enabled=?20, blendop_params=?21, blendop_version=?22,"
           "  multi_name=?23, multi_name_hand_edited=?24"
         : "",
       rowid);
    // clang-format on
  }
  else
  {
    // clang-format off
    query = g_strdup_printf
      ("INSERT INTO data.presets"
       " (name, description, "
       "  model, maker, lens, iso_min, iso_max, exposure_min, exposure_max, aperture_min,"
       "  aperture_max, focal_length_min, focal_length_max, autoapply,"
       "  filter, format, def, writeprotect, operation, op_version, op_params, enabled,"
       "  blendop_params, blendop_version,"
       "  multi_priority, multi_name, multi_name_hand_edited) "
       "VALUES"
       " (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15, ?16,"
       "   0, 0, ?17, ?18, ?19, ?20, ?21, ?22, 0, ?23, ?24)");
    // clang-format on
  }

  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  g_free(query);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, p->name, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, p->description, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, p->model, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 4, p->maker, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 5, p->lens, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 6, p->iso_min);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 7, p->iso_max);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 8, p->exposure_min);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 9, p->exposure_max);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 10, p->aperture_min);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 11, p->aperture_max);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 12, p->focal_length_min);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 13, p->focal_length_max);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 14, p->autoapply);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 15, p->filter);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 16, p->format);

  if(p->iop)
  {
    DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 19, p->op_params, p->op_params_size, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 20, p->enabled);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 23, p->multi_name, -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 24, p->multi_name_hand_edited);
  }

  if(p->iop || rowid < 0)
  {
    DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 21, p->blendop_params, p->blendop_params_size,
                               SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 22, p->blendop_version);
  }

  if(rowid < 0)
  {
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 17, p->operation, -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 18, p->op_version);
  }

  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

void dt_presets_update_params(const char *operation,
                              const int op_version,
                              const char *name,
                              const void *op_params,
                              const int op_params_size,
                              const int enabled,
                              const void *blendop_params,
                              const int blendop_params_size,
                              const int blendop_version)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "UPDATE data.presets"
                              " SET op_version=?2, op_params=?3, enabled=?4, "
                              "     blendop_params=?5, blendop_version=?6"
                              " WHERE name=?7 AND operation=?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, operation, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, op_version);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 3, op_params, op_params_size, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 4, enabled);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 5, blendop_params, blendop_params_size, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 6, blendop_version);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 7, name, -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

static void *_blob_copy(sqlite3_stmt *stmt, const int col, int *size)
{
  const void *blob = sqlite3_column_blob(stmt, col);
  *size = sqlite3_column_bytes(stmt, col);
  if(!blob) return NULL;
  // one zero byte more: some modules store a string without its terminator
  char *copy = g_malloc0(*size + 1);
  memcpy(copy, blob, *size);
  return copy;
}

gboolean dt_presets_get_params(const char *operation,
                               const int op_version,
                               const char *name,
                               dt_presets_params_t *p)
{
  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(
     dt_database_get(darktable.db),
     "SELECT op_params, enabled, blendop_params, blendop_version, writeprotect,"
     "       multi_name, multi_name_hand_edited, autoapply"
     " FROM data.presets"
     " WHERE operation = ?1 AND op_version = ?2 AND name = ?3",
     -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, operation, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, op_version);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, name, -1, SQLITE_TRANSIENT);
  const gboolean found = sqlite3_step(stmt) == SQLITE_ROW;
  if(found)
  {
    p->op_params = _blob_copy(stmt, 0, &p->op_params_size);
    p->enabled = sqlite3_column_int(stmt, 1);
    p->blendop_params = _blob_copy(stmt, 2, &p->blendop_params_size);
    p->blendop_version = sqlite3_column_int(stmt, 3);
    p->writeprotect = sqlite3_column_int(stmt, 4);
    p->multi_name = g_strdup((const char *)sqlite3_column_text(stmt, 5));
    p->multi_name_hand_edited = sqlite3_column_int(stmt, 6);
    p->autoapply = sqlite3_column_int(stmt, 7);
  }
  sqlite3_finalize(stmt);
  return found;
}

void dt_presets_params_clear(dt_presets_params_t *p)
{
  g_free(p->name);
  g_free(p->op_params);
  g_free(p->blendop_params);
  g_free(p->multi_name);
  memset(p, 0, sizeof(*p));
}

char *dt_presets_adjacent_name(const char *operation,
                               const int op_version,
                               const char *name,
                               const int direction)
{
  char *found = NULL;
  sqlite3_stmt *stmt;
  // clang-format off
  gchar *query = g_strdup_printf("SELECT name"
                                 " FROM data.presets"
                                 " WHERE operation=?1 AND op_version=?2 AND"
                                 "       (?3='' OR LOWER(name) %s LOWER(?3))"
                                 " ORDER BY writeprotect %s, LOWER(name) %s"
                                 " LIMIT ?4",
                                 direction < 0 ? "<" : ">",
                                 direction < 0 ? "ASC" : "DESC",
                                 direction < 0 ? "DESC" : "ASC");
  // clang-format on
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, operation, -1, SQLITE_STATIC);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, op_version);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, name ? name : "", -1, SQLITE_STATIC);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 4, abs(direction));
  g_free(query);

  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    g_free(found);
    found = g_strdup((gchar *)sqlite3_column_text(stmt, 0));
  }
  sqlite3_finalize(stmt);
  return found;
}

gboolean dt_presets_find_autoapply(const char *operation,
                                   const int op_version,
                                   const dt_image_t *image,
                                   const gboolean is_display_referred,
                                   const char *workflow_preset,
                                   dt_presets_params_t *p)
{
  char *format_filter = dt_presets_get_filter(image);

  // Take the first auto-applied preset (autoapply = 1) and select first the
  // user's presets if they exist (writeprotect = 0) by ordering on writeprotect.
  // Also make sure we pick the last user's defined preset (ORDER BY rowid DESC).
  char query[2024];
  // clang-format off
  snprintf(query, sizeof(query),
     "SELECT name, op_params, blendop_params"
     " FROM data.presets"
     " WHERE operation = ?1"
     "        AND ((autoapply=1"
     "           AND ((?2 LIKE model AND ?3 LIKE maker) OR (?4 LIKE model AND ?5 LIKE maker))"
     "           AND ?6 LIKE lens AND ?7 BETWEEN iso_min AND iso_max"
     "           AND ?8 BETWEEN exposure_min AND exposure_max"
     "           AND ?9 BETWEEN aperture_min AND aperture_max"
     "           AND ?10 BETWEEN focal_length_min AND focal_length_max"
     "           AND (%s)"
     "           AND operation NOT IN"
     "               ('ioporder', 'metadata', 'export', 'tagging', 'collect', '%s'))"
     "  OR (name = ?13)) AND op_version = ?14"
     " ORDER BY writeprotect ASC, rowid DESC",
     format_filter,
     is_display_referred?"":"basecurve");
  // clang-format on

  g_free(format_filter);

  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, operation, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, image->exif_model, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, image->exif_maker, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 4, image->camera_alias, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 5, image->camera_maker, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 6, image->exif_lens, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 7, fmaxf(0.0f,
                                              fminf(FLT_MAX, image->exif_iso)));
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 8, fmaxf(0.0f,
                                              fminf(1000000, image->exif_exposure)));
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 9, fmaxf(0.0f,
                                              fminf(1000000, image->exif_aperture)));
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 10, fmaxf(0.0f,
                                               fminf(1000000, image->exif_focal_length)));
  // 0: dontcare, 1: ldr, 2: raw plus monochrome & color
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 13, workflow_preset, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 14, op_version);

  const gboolean found = sqlite3_step(stmt) == SQLITE_ROW;
  if(found)
  {
    p->name = g_strdup((const char *)sqlite3_column_text(stmt, 0));
    p->op_params = _blob_copy(stmt, 1, &p->op_params_size);
    p->blendop_params = _blob_copy(stmt, 2, &p->blendop_params_size);
  }
  sqlite3_finalize(stmt);
  return found;
}

GList *dt_presets_list_names(const char *operation,
                             const gboolean writeprotect_first)
{
  GList *names = NULL;
  // clang-format off
  gchar *query = g_strdup_printf("SELECT name, writeprotect"
                                 " FROM data.presets"
                                 " WHERE operation=?1"
                                 " ORDER BY writeprotect %s, LOWER(name), rowid",
                                 writeprotect_first ? "DESC" : "ASC"
                                );
  // clang-format on
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, operation, -1, SQLITE_TRANSIENT);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    dt_presets_name_t *n = g_new(dt_presets_name_t, 1);
    n->name = g_strdup((const char *)sqlite3_column_text(stmt, 0));
    n->writeprotect = sqlite3_column_int(stmt, 1);
    names = g_list_prepend(names, n);
  }
  sqlite3_finalize(stmt);
  g_free(query);
  return g_list_reverse(names);
}

void dt_presets_name_free(gpointer data)
{
  dt_presets_name_t *n = data;
  g_free(n->name);
  g_free(n);
}

GList *dt_presets_version_names(const char *operation,
                                const int op_version,
                                const gboolean builtin_first)
{
  GList *names = NULL;
  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              builtin_first
                              ? "SELECT name"
                                " FROM data.presets"
                                " WHERE operation=?1 AND op_version=?2"
                                " ORDER BY writeprotect DESC, name, rowid"
                              : "SELECT name"
                                " FROM data.presets"
                                " WHERE operation=?1"
                                " AND op_version = ?2"
                                " ORDER BY name",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, operation, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, op_version);
  while(sqlite3_step(stmt) == SQLITE_ROW)
    names = g_list_prepend(names, g_strdup((const char *)sqlite3_column_text(stmt, 0)));
  sqlite3_finalize(stmt);
  return g_list_reverse(names);
}

GList *dt_presets_list_params(const char *operation,
                              const int op_version,
                              const gboolean user_only)
{
  GList *list = NULL;
  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              user_only
                              ? "SELECT name, op_params"
                                " FROM data.presets"
                                " WHERE operation = ?1 AND op_version = ?2 AND writeprotect = 0"
                              : "SELECT name, op_params"
                                " FROM data.presets"
                                " WHERE operation = ?1 AND op_version = ?2",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, operation, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, op_version);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    dt_presets_params_t *p = g_new0(dt_presets_params_t, 1);
    p->name = g_strdup((const char *)sqlite3_column_text(stmt, 0));
    p->op_params = _blob_copy(stmt, 1, &p->op_params_size);
    list = g_list_prepend(list, p);
  }
  sqlite3_finalize(stmt);
  return g_list_reverse(list);
}

void dt_presets_params_free(gpointer data)
{
  dt_presets_params_clear(data);
  g_free(data);
}

void dt_presets_delete_version(const char *operation, const int op_version)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "DELETE FROM data.presets"
                              " WHERE operation = ?1 AND op_version = ?2",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, operation, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, op_version);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

void dt_presets_rename(const char *operation,
                       const int op_version,
                       const char *name,
                       const char *new_name)
{
  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "UPDATE data.presets"
                              " SET name=?1"
                              " WHERE name=?2 AND operation = ?3 AND op_version = ?4",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, new_name, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, name, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, operation, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 4, op_version);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

char *dt_presets_find_lib_autoapply(const char *operation,
                                    const int op_version,
                                    const dt_image_t *image)
{
  char *format_filter = dt_presets_get_filter(image);

  char query[1024];
  // clang-format off
  snprintf(query, sizeof(query),
           "SELECT name"
           " FROM data.presets"
           " WHERE operation=?11"
           "       AND op_version=?1"
           "       AND autoapply=1"
           "       AND ((?2 LIKE model AND ?3 LIKE maker) OR (?4 LIKE model AND ?5 LIKE maker))"
           "       AND ?6 LIKE lens AND ?7 BETWEEN iso_min AND iso_max"
           "       AND ?8 BETWEEN exposure_min AND exposure_max"
           "       AND ?9 BETWEEN aperture_min AND aperture_max"
           "       AND ?10 BETWEEN focal_length_min AND focal_length_max"
           "       AND (%s)"
           " ORDER BY writeprotect DESC, name DESC"
           " LIMIT 1",
           format_filter);
  // clang-format on

  g_free(format_filter);

  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, op_version);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, image->exif_model, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, image->exif_maker, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 4, image->camera_alias, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 5, image->camera_maker, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 6, image->exif_lens, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 7, fmaxf(0.0f, fminf(FLT_MAX, image->exif_iso)));
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 8, fmaxf(0.0f, fminf(1000000, image->exif_exposure)));
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 9, fmaxf(0.0f, fminf(1000000, image->exif_aperture)));
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 10, fmaxf(0.0f, fminf(1000000, image->exif_focal_length)));
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 11, operation, -1, SQLITE_TRANSIENT);

  char *name = NULL;
  if(sqlite3_step(stmt) == SQLITE_ROW)
    name = g_strdup((const char *)sqlite3_column_text(stmt, 0));
  sqlite3_finalize(stmt);
  return name;
}

GList *dt_presets_list_stored(const char *operation)
{
  GList *list = NULL;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get(darktable.db),
      "SELECT name, op_version, op_params, blendop_version, blendop_params, rowid"
      " FROM data.presets"
      " WHERE operation = ?1",
      -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, operation, -1, SQLITE_TRANSIENT);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    dt_presets_params_t *p = g_new0(dt_presets_params_t, 1);
    p->name = g_strdup((const char *)sqlite3_column_text(stmt, 0));
    p->op_version = sqlite3_column_int(stmt, 1);
    p->op_params = _blob_copy(stmt, 2, &p->op_params_size);
    p->blendop_version = sqlite3_column_int(stmt, 3);
    p->blendop_params = _blob_copy(stmt, 4, &p->blendop_params_size);
    p->rowid = sqlite3_column_int(stmt, 5);
    list = g_list_prepend(list, p);
  }
  sqlite3_finalize(stmt);
  return g_list_reverse(list);
}

GList *dt_presets_names_by_rowid(const char *operation)
{
  GList *names = NULL;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT name"
                              " FROM data.presets"
                              " WHERE operation=?1"
                              " ORDER BY writeprotect DESC, rowid",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, operation, -1, SQLITE_TRANSIENT);
  while(sqlite3_step(stmt) == SQLITE_ROW)
    names = g_list_prepend(names, g_strdup((const char *)sqlite3_column_text(stmt, 0)));
  sqlite3_finalize(stmt);
  return g_list_reverse(names);
}

void dt_presets_set_op_version(const char *operation,
                               const char *name,
                               const int op_version)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2
    (dt_database_get(darktable.db),
     "UPDATE data.presets"
     " SET op_version=?1"
     " WHERE operation=?2 AND name=?3", -1,
     &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, op_version);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, operation, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, name, -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

void dt_presets_set_op_params(const char *operation,
                              const char *name,
                              const int op_version,
                              const void *op_params,
                              const int op_params_size)
{
  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "UPDATE data.presets"
                              " SET op_version=?1, op_params=?2"
                              " WHERE operation=?3 AND name=?4",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, op_version);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 2, op_params, op_params_size, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, operation, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 4, name, -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

void dt_presets_set_blendop_params(const char *operation,
                                   const char *name,
                                   const int blendop_version,
                                   const void *blendop_params,
                                   const int blendop_params_size)
{
  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "UPDATE data.presets"
                              " SET blendop_version=?1, blendop_params=?2"
                              " WHERE operation=?3 AND name=?4",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, blendop_version);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 2, blendop_params, blendop_params_size,
                             SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, operation, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 4, name, -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

void dt_presets_delete_builtin(const char *operation)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2
    (dt_database_get(darktable.db),
     "DELETE FROM data.presets"
     " WHERE writeprotect = 1"
     "   AND operation = ?1",
     -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, operation, -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

void dt_presets_insert_lib_preset(const char *name,
                                  const char *operation,
                                  const int op_version,
                                  const void *op_params,
                                  const int op_params_size)
{
  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get(darktable.db),
      "INSERT INTO data.presets (name, description, operation, op_version, op_params,"
      "  blendop_params, blendop_version, enabled, model, maker, lens,"
      "  iso_min, iso_max, exposure_min, exposure_max, aperture_min, aperture_max,"
      "  focal_length_min, focal_length_max, writeprotect, "
      "  autoapply, filter, def, format)"
      " VALUES (?1, '', ?2, ?3, ?4, NULL, 0, 1, '%', "
      "         '%', '%', 0, 340282346638528859812000000000000000000, 0, 100000000,"
      "          0, 100000000, 0, 1000, 0, 0, 0, 0, 0)",
      -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, name, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, operation, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, op_version);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 4, op_params, op_params_size, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

gboolean dt_presets_get_params_any_version(const char *operation,
                                           const char *name,
                                           void **op_params,
                                           int *op_params_size)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get(darktable.db),
      "SELECT op_params"
      " FROM data.presets"
      " WHERE operation=?2"
      " AND name = ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, name, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, operation, -1, SQLITE_TRANSIENT);
  const gboolean found = sqlite3_step(stmt) == SQLITE_ROW;
  if(found) *op_params = _blob_copy(stmt, 0, op_params_size);
  sqlite3_finalize(stmt);
  return found;
}

void dt_presets_set_op_params_at(const int rowid,
                                 const void *op_params,
                                 const int op_params_size)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "UPDATE data.presets"
                              " SET op_params=?1"
                              " WHERE rowid=?2",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 1, op_params, op_params_size, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, rowid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

void dt_presets_delete_at(const int rowid)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "DELETE FROM data.presets WHERE rowid=?1", -1,
                              &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, rowid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

void dt_presets_update_mml(const char *name,
                           const char *op,
                           const int32_t version,
                           const char *maker,
                           const char *model,
                           const char *lens)
{
  sqlite3_stmt *stmt;
  // clang-format off¨
  DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get(darktable.db),
      "UPDATE data.presets"
      " SET maker='%' || ?1 || '%', model=?2, lens=?3"
      " WHERE operation=?4 AND op_version=?5 AND name=?6", -1,
      &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, maker, -1, SQLITE_TRANSIENT);
  if(*model)
  {
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, model, -1, SQLITE_TRANSIENT);
  }
  else
  {
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, "%", -1, SQLITE_TRANSIENT);
  }
  if(*lens)
  {
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, lens, -1, SQLITE_TRANSIENT);
  }
  else
  {
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, "%", -1, SQLITE_TRANSIENT);
  }
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 4, op, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 5, version);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 6, name, -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

void dt_presets_update_iso(const char *name,
                           const char *op,
                           const int32_t version,
                           const float min,
                           const float max)
{
  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get(darktable.db),
      "UPDATE data.presets"
      " SET iso_min=?1, iso_max=?2"
      " WHERE operation=?3 AND op_version=?4 AND name=?5", -1, &stmt,
      NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 1, min);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 2, max);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, op, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 4, version);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 5, name, -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

void dt_presets_update_av(const char *name,
                          const char *op,
                          const int32_t version,
                          const float min,
                          const float max)
{
  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get(darktable.db),
      "UPDATE data.presets"
      " SET aperture_min=?1, aperture_max=?2"
      " WHERE operation=?3 AND op_version=?4 AND name=?5",
      -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 1, min);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 2, max);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, op, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 4, version);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 5, name, -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

void dt_presets_update_tv(const char *name,
                          const char *op,
                          const int32_t version,
                          const float min,
                          const float max)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get(darktable.db),
      "UPDATE data.presets"
      " SET exposure_min=?1, exposure_max=?2"
      " WHERE operation=?3 AND op_version=?4 AND name=?5",
      -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 1, min);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 2, max);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, op, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 4, version);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 5, name, -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

void dt_presets_update_fl(const char *name,
                          const char *op,
                          const int32_t version,
                          const float min,
                          const float max)
{
  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "UPDATE data.presets"
                              " SET focal_length_min=?1, focal_length_max=?2"
                              " WHERE operation=?3 AND op_version=?4 AND name=?5",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 1, min);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 2, max);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, op, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 4, version);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 5, name, -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

void dt_presets_update_format(const char *name,
                              const char *op,
                              const int32_t version,
                              const int flag)
{
  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "UPDATE data.presets"
                              " SET format=?1"
                              " WHERE operation=?2 AND op_version=?3 AND name=?4",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, flag);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, op, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, version);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 4, name, -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

void dt_presets_update_autoapply(const char *name,
                                 const char *op,
                                 const int32_t version,
                                 const gboolean autoapply)
{
  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get(darktable.db),
      "UPDATE data.presets"
      " SET autoapply=?1"
      " WHERE operation=?2 AND op_version=?3 AND name=?4", -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, autoapply);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, op, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, version);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 4, name, -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

void dt_presets_update_filter(const char *name,
                              const char *op,
                              const int32_t version,
                              const int filter)
{
  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "UPDATE data.presets"
                              " SET filter=?1"
                              " WHERE operation=?2 AND op_version=?3 AND name=?4",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, filter);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, op, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, version);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 4, name, -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

void dt_presets_add_builtin(const char *name,
                            const char *op,
                            const int32_t version,
                            const void *params,
                            const int32_t params_size,
                            const void *blend_params,
                            const gboolean enabled)
{
  sqlite3_stmt *stmt;

  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get(darktable.db),
      "INSERT OR REPLACE"
      " INTO data.presets (name, description, operation, op_version, op_params, enabled,"
      "                    blendop_params, blendop_version, multi_priority, multi_name,"
      "                    model, maker, lens, iso_min, iso_max, exposure_min, exposure_max,"
      "                    aperture_min, aperture_max, focal_length_min, focal_length_max,"
      "                    writeprotect, autoapply, filter, def, format, multi_name_hand_edited)"
      " VALUES (?1, '', ?2, ?3, ?4, ?5, ?6, ?7, 0, '', '%', '%', '%', 0,"
      "         340282346638528859812000000000000000000, 0, 10000000, 0, 100000000, 0,"
      "         ?8, 1, 0, 0, 0, 0, 0)",
      -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, name, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, op, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, version);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 4, params, params_size, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 5, enabled);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 6, blend_params, sizeof(dt_develop_blend_params_t),
                             SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 7, dt_develop_blend_version());
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 8, MAX_FOCAL_LEN);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

void dt_presets_delete_all_builtin(void)
{
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db),
                        "DELETE FROM data.presets WHERE writeprotect = 1", NULL,
                        NULL, NULL);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
