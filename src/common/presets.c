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
#include "common/file_location.h"
#include "develop/blend.h"
#include "develop/imageop.h"
#include "libs/lib.h"

#include <glib-2.0/gio/gmenu.h>
#include <glib-2.0/gio/gmenumodel.h>
#include <libxml/encoding.h>
#include <libxml/xmlwriter.h>
#include <libxml/parser.h>
#include <libxml/xpath.h>
#include <libxml/xpathInternals.h>

#include <glib.h>
#include <inttypes.h>
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
       && !memcmp(module->blend_params, blendop_params,
                  MIN(bl_params_size, sizeof(dt_develop_blend_params_t)))
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
  char *query = g_strdup_printf("SELECT name, multi_name"
                                " FROM data.presets"
                                " WHERE operation = ?1"
                                "   AND (op_params = ?2"
                                "        %s)"
                                "   AND blendop_params = ?3",
                                is_default_params ? "OR op_params IS NULL" : "");

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);

  // clang-format on
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, module_name, strlen(module_name), SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 2, params, param_size, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 3, blend_params, blend_params_size, SQLITE_TRANSIENT);

  char *result = NULL;

  // returns the preset's multi_name if defined otherwise the preset
  // name is returned.
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const char *name = (const char *)sqlite3_column_text(stmt, 0);
    const char *multi_name = (const char *)sqlite3_column_text(stmt, 1);
    if(multi_name && (strlen(multi_name) == 0 || multi_name[0] != ' '))
      result = dt_presets_get_multi_name(name, multi_name, FALSE);
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

static void _menu_shell_insert_sorted(GtkWidget *menu_shell,
                                      GtkWidget *item,
                                      const gchar *name)
{
  GList *items = gtk_container_get_children(GTK_CONTAINER(menu_shell));
  int num = g_list_length(items);
  for(GList *i = g_list_last(items); i; i = i->prev, num--)
    if(g_utf8_collate(gtk_menu_item_get_label(i->data), name) < 0) break;
  gtk_menu_shell_insert(GTK_MENU_SHELL(menu_shell), item, num);
  g_list_free(items);
}

static void _menu_shell_insert_sorted2(GMenu *menu,
                                       GMenuItem *item,
                                       const gchar *name)
{
  GMenuModel *model = G_MENU_MODEL(menu);

  int num = g_menu_model_get_n_items(model);
  gboolean found = FALSE;
  int i;
  for(i = 0; i < num; i++)
  {
    gchar *item_label = NULL;
    GVariant *attr = g_menu_model_get_item_attribute_value(model, i, "label", G_VARIANT_TYPE_STRING);
    if(attr)
    {
      item_label = g_variant_dup_string(attr, NULL);
      g_variant_unref(attr);
      if(g_utf8_collate(item_label, name) > 0) found = TRUE;
      g_free(item_label);
      if(found) break;
    }
  }

  g_menu_insert_item(menu, i, item);
}

GtkWidget *dt_insert_preset_in_menu_hierarchy(const char *name,
                                              GSList **menu_path,
                                              GtkWidget *mainmenu,
                                              GtkWidget **submenu,
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
  GtkWidget *mi;
  g_free(local_name);
  for(; p && *(p+1) && *(s+1) && !g_strcmp0(*s, *p); p++, s++)
    ;
  for(; p && *(p+1); p++)
  {
    mpath = g_slist_delete_link(mpath, mpath); // pop
    *submenu = mpath ? gtk_menu_item_get_submenu(mpath->data) : mainmenu;
  }
  for(; *(s+1); s++)
  {
    GtkWidget *sm = gtk_menu_item_new_with_label(*s);
    mpath = g_slist_prepend(mpath, sm); // push

    _menu_shell_insert_sorted(*submenu, sm, *s);
    *submenu = gtk_menu_new();
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(sm), *submenu);
  }
  *menu_path = mpath;
  g_strfreev(*prev_split);
  *prev_split = split;
  if(isdefault)
  {
    gchar *label = g_strdup_printf("%s %s", *s, _("(default)"));
    mi = gtk_check_menu_item_new_with_label(label);
    _menu_shell_insert_sorted(*submenu, mi, label);
    g_free(label);
  }
  else
  {
    mi = gtk_check_menu_item_new_with_label(*s);
    _menu_shell_insert_sorted(*submenu, mi, *s);
  }
  dt_gui_add_class(mi, "dt_transparent_background");
  return mi;
}

GtkWidget *dt_insert_preset_in_menu_hierarchy2(const char *name,
                                               const char *action,
                                               GSList **menu_path,
                                               GtkWidget *mainmenu,
                                               GtkWidget **submenu,
                                               GSList **menu_path2,
                                               GMenu *mainmenu2,
                                               GMenu **submenu2,
                                               gchar ***prev_split,
                                               gboolean isdefault)
{
  gchar *local_name = dt_util_localize_segmented_name(name, FALSE);
  gchar **split = g_strsplit(local_name, "|", -1);
  gchar **s = split;
  gchar **p = *prev_split;
  GSList *mpath = *menu_path;
  GSList *mpath2 = *menu_path2;
  GtkWidget *mi;
  GMenuItem *mi2;
  g_free(local_name);
  for(; p && *(p+1) && *(s+1) && !g_strcmp0(*s, *p); p++, s++)
    ;
  for(; p && *(p+1); p++)
  {
    mpath = g_slist_delete_link(mpath, mpath); // pop
    *submenu = mpath ? gtk_menu_item_get_submenu(mpath->data) : mainmenu;

    mpath2 = g_slist_delete_link(mpath2, mpath2); // pop
    *submenu2 = mpath2 ? mpath->data : mainmenu2;

  }
  for(; *(s+1); s++)
  {
    GtkWidget *sm = gtk_menu_item_new_with_label(*s);
    mpath = g_slist_prepend(mpath, sm); // push

    _menu_shell_insert_sorted(*submenu, sm, *s);
    *submenu = gtk_menu_new();
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(sm), *submenu);

    GMenu *sm2 = g_menu_new();
    GMenuItem *smi = g_menu_item_new_submenu(*s, G_MENU_MODEL(sm2));
    _menu_shell_insert_sorted2(*submenu2, smi, *s);
    *submenu2 = sm2;
    mpath2 = g_slist_prepend(mpath2, sm2); // push

  }
  *menu_path = mpath;
  *menu_path2 = mpath2;
  g_strfreev(*prev_split);
  *prev_split = split;
  if(isdefault)
  {
    gchar *label = g_strdup_printf("%s %s", *s, _("(default)"));
    mi = gtk_check_menu_item_new_with_label(label);
    _menu_shell_insert_sorted(*submenu, mi, label);

    mi2 = g_menu_item_new(label, action);
    _menu_shell_insert_sorted2(*submenu2, mi2, label);

    g_free(label);
  }
  else
{
    mi = gtk_check_menu_item_new_with_label(*s);
    _menu_shell_insert_sorted(*submenu, mi, *s);

    mi2 = g_menu_item_new(*s, action);
    _menu_shell_insert_sorted2(*submenu2, mi2, *s);

  }
  dt_gui_add_class(mi, "dt_transparent_background");
  return mi;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
