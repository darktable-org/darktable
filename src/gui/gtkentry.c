/*
    This file is part of darktable,
    Copyright (C) 2012-2020 darktable developers.

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
#include "gtkentry.h"
#include "common/darktable.h"

/**
 * Called when the user selects an entry from the autocomplete list.
 *
 * @param[in] widget
 * @param[in] model     Data structure containing autocomplete strings.
 * @param[in] iter      Pointer into data structure.
 * @param[in] user_data unused here
 *
 * @return Currently always true
 */
static gboolean on_match_select(GtkEntryCompletion *widget, GtkTreeModel *model, GtkTreeIter *iter,
                                gpointer user_data)
{

  const gchar *varname;
  GtkEditable *e = (GtkEditable *)gtk_entry_completion_get_entry(widget);
  gchar *s = gtk_editable_get_chars(e, 0, -1);
  gint cur_pos = gtk_editable_get_position(e);
  gint p = cur_pos;

  GValue value = {
    0,
  };
  gtk_tree_model_get_value(model, iter, COMPL_VARNAME, &value);
  varname = g_value_get_string(&value);

  for(p = cur_pos; p - 2 > 0; p--)
  {
    if(strncmp(s + p - 2, "$(", 2) == 0)
    {
      break;
    }
  }

  size_t text_len = strlen(varname) + 2;
  gchar *addtext = (gchar *)g_malloc(text_len);
  snprintf(addtext, text_len, "%s)", varname);

  gtk_editable_delete_text(e, p, cur_pos);
  gtk_editable_insert_text(e, addtext, -1, &p);
  gtk_editable_set_position(e, p);
  g_value_unset(&value);
  g_free(addtext);
  g_free(s);
  return TRUE;
}

/**
 * Case insensitive substring search for a completion match.
 *
 * Based on the default matching function in GtkEntryCompletion.
 *
 * This function is called once for each iter in the GtkEntryCompletion's
 * list of completion entries (model).
 *
 * @param completion Completion object to apply this function on
 * @param key        Complete string from the GtkEntry.
 * @param iter       Item in list of autocomplete database to compare key against.
 * @param user_data  Unused.
 */
static gboolean on_match_func(GtkEntryCompletion *completion, const gchar *key, GtkTreeIter *iter,
                              gpointer user_data)
{
  gboolean ret = FALSE;

  GtkEditable *e = (GtkEditable *)gtk_entry_completion_get_entry(completion);
  gint cur_pos = gtk_editable_get_position(e); /* returns 1..* */
  gint var_start = 0;
  gboolean var_present = FALSE;

  for(gint p = cur_pos; p >= 0; p--)
  {
    gchar *ss = gtk_editable_get_chars(e, p, cur_pos);
    if(strncmp(ss, "$(", 2) == 0)
    {
      var_start = p + 2;
      var_present = TRUE;
      g_free(ss);
      break;
    }
    g_free(ss);
  }

  if(var_present)
  {
    gchar *varname = gtk_editable_get_chars(e, var_start, cur_pos);

    GtkTreeModel *model = gtk_entry_completion_get_model(completion);
    gchar *item = NULL;
    gtk_tree_model_get(model, iter, COMPL_VARNAME, &item, -1);

    if(item != NULL)
    {
      // Do utf8-safe case insensitive string compare.
      // Shamelessly stolen from GtkEntryCompletion.
      gchar *normalized_string = g_utf8_normalize(item, -1, G_NORMALIZE_ALL);

      if(normalized_string != NULL)
      {
        gchar *case_normalized_string = g_utf8_casefold(normalized_string, -1);

        if(!g_ascii_strncasecmp(varname, case_normalized_string, strlen(varname))) ret = TRUE;

        g_free(case_normalized_string);
      }
      g_free(normalized_string);
    }
    g_free(varname);
    g_free(item);
  }

  return ret;
}

/**
 * This function initializes entry with an autocomplete table
 * specified by compl_list. To set the default darktable variables,
 * use dt_gtkentry_get_default_path_compl_list().
 *
 * @param[in] entry GtkEntry
 * @param[in] compl_list A {NULL,NULL} terminated array containing
 *                       {variable,description} for each available
 *                       completion text.
 */
void dt_gtkentry_setup_completion(GtkEntry *entry, const dt_gtkentry_completion_spec *compl_list)
{
  GtkEntryCompletion *completion = gtk_entry_completion_new();
  GtkListStore *model = gtk_list_store_new(3, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
  GtkTreeIter iter;

  gtk_entry_completion_set_text_column(completion, COMPL_DESCRIPTION);
  gtk_entry_set_completion(entry, completion);
  g_signal_connect(G_OBJECT(completion), "match-selected", G_CALLBACK(on_match_select), NULL);

  /* Populate the completion database. */
  for(const dt_gtkentry_completion_spec *l = compl_list; l && l->varname; l++)
  {
    gtk_list_store_append(model, &iter);
    gtk_list_store_set(model, &iter, COMPL_VARNAME, l->varname, COMPL_DESCRIPTION, _(l->description), -1);
  }
  gtk_entry_completion_set_model(completion, GTK_TREE_MODEL(model));
  gtk_entry_completion_set_match_func(completion, on_match_func, NULL, NULL);
  g_object_unref(model);
}

/**
 * The default set of image metadata of interest for use in image paths.
 */
const dt_gtkentry_completion_spec *dt_gtkentry_get_default_path_compl_list()
{
  static dt_gtkentry_completion_spec default_path_compl_list[]
      = { { "ROLL.NAME", N_("$(ROLL.NAME) - roll of the input image") },
          { "FILE.FOLDER", N_("$(FILE.FOLDER) - folder containing the input image") },
          { "FILE.NAME", N_("$(FILE.NAME) - basename of the input image") },
          { "FILE.EXTENSION", N_("$(FILE.EXTENSION) - extension of the input image") },
          { "VERSION", N_("$(VERSION) - duplicate version") },
          { "VERSION.IF_MULTI", N_("$(VERSION.IF_MULTI) - same as $(VERSION) but null string if only one version exists") },
          { "VERSION.NAME", N_("$(VERSION.NAME) - version name from metadata") },
          { "JOBCODE", N_("$(JOBCODE) - job code for import") },
          { "SEQUENCE", N_("$(SEQUENCE) - sequence number") },
          { "WIDTH.MAX", N_("$(WIDTH.MAX) - maximum image export width") },
          { "WIDTH.SENSOR", N_("$(WIDTH.SENSOR) - image sensor width") },
          { "WIDTH.RAW", N_("$(WIDTH.RAW) - RAW image width") },
          { "WIDTH.CROP", N_("$(WIDTH.CROP) - image width after crop") },
          { "WIDTH.EXPORT", N_("$(WIDTH.EXPORT) - exported image width") },
          { "HEIGHT.MAX", N_("$(HEIGHT.MAX) - maximum image export height") },
          { "HEIGHT.SENSOR", N_("$(HEIGHT.SENSOR) - image sensor height") },
          { "HEIGHT.RAW", N_("$(HEIGHT.RAW) - RAW image height") },
          { "HEIGHT.CROP", N_("$(HEIGHT.CROP) - image height after crop") },
          { "HEIGHT.EXPORT", N_("$(HEIGHT.EXPORT) - exported image height") },
          { "YEAR", N_("$(YEAR) - year") },
          { "YEAR.SHORT", N_("$(YEAR.SHORT) - year without century") },
          { "MONTH", N_("$(MONTH) - month") },
          { "MONTH.SHORT", N_("$(MONTH.SHORT) - abbreviated month name according to the current locale") },
          { "MONTH.LONG", N_("$(MONTH.LONG) - full month name according to the current locale") },
          { "DAY", N_("$(DAY) - day") },
          { "HOUR", N_("$(HOUR) - hour") },
          { "HOUR.AMPM", N_("$(HOUR.AMPM) - hour, 12-hour clock") },
          { "MINUTE", N_("$(MINUTE) - minute") },
          { "SECOND", N_("$(SECOND) - second") },
          { "MSEC", N_("$(MSEC) - millisecond") },
          { "EXIF.YEAR", N_("$(EXIF.YEAR) - EXIF year") },
          { "EXIF.YEAR.SHORT", N_("$(EXIF.YEAR.SHORT) - EXIF year without century") },
          { "EXIF.MONTH", N_("$(EXIF.MONTH) - EXIF month") },
          { "EXIF.MONTH.SHORT", N_("$(EXIF.MONTH.SHORT) - abbreviated EXIF month name according to the current locale") },
          { "EXIF.MONTH.LONG", N_("$(EXIF.MONTH.LONG) - full EXIF month name according to the current locale") },
          { "EXIF.DAY", N_("$(EXIF.DAY) - EXIF day") },
          { "EXIF.HOUR", N_("$(EXIF.HOUR) - EXIF hour") },
          { "EXIF.HOUR.AMPM", N_("$(EXIF.HOUR.AMPM) - EXIF hour, 12-hour clock") },
          { "EXIF.MINUTE", N_("$(EXIF.MINUTE) - EXIF minute") },
          { "EXIF.SECOND", N_("$(EXIF.SECOND) - EXIF second") },
          { "EXIF.MSEC", N_("$(EXIF.MSEC) - EXIF millisecond") },
          { "EXIF.ISO", N_("$(EXIF.ISO) - ISO value") },
          { "EXIF.EXPOSURE", N_("$(EXIF.EXPOSURE) - EXIF exposure") },
          { "EXIF.EXPOSURE.BIAS", N_("$(EXIF.EXPOSURE.BIAS) - EXIF exposure bias") },
          { "EXIF.APERTURE", N_("$(EXIF.APERTURE) - EXIF aperture") },
          { "EXIF.FOCAL.LENGTH", N_("$(EXIF.FOCAL.LENGTH) - EXIF focal length") },
          { "EXIF.FOCUS.DISTANCE", N_("$(EXIF.FOCUS.DISTANCE) - EXIF focal distance") },
          { "EXIF.MAKER", N_("$(EXIF.MAKER) - camera maker") },
          { "EXIF.MODEL", N_("$(EXIF.MODEL) - camera model") },
          { "EXIF.LENS", N_("$(EXIF.LENS) - lens") },
          { "LONGITUDE", N_("$(LONGITUDE) - longitude") },
          { "LATITUDE", N_("$(LATITUDE) - latitude") },
          { "ELEVATION", N_("$(ELEVATION) - elevation") },
          { "STARS", N_("$(STARS) - star rating as number (-1 for rejected)") },
          { "RATING.ICONS", N_("$(RATING.ICONS) - star/reject rating in icon form") },
          { "LABELS", N_("$(LABELS) - color labels as text") },
          { "LABELS.ICONS", N_("$(LABELS.ICONS) - color labels as icons") },
          { "ID", N_("$(ID) - image ID") },
          { "TITLE", N_("$(TITLE) - title from metadata") },
          { "DESCRIPTION", N_("$(DESCRIPTION) - description from metadata") },
          { "CREATOR", N_("$(CREATOR) - creator from metadata") },
          { "PUBLISHER", N_("$(PUBLISHER) - publisher from metadata") },
          { "RIGHTS", N_("$(RIGHTS) - rights from metadata") },
          { "USERNAME", N_("$(USERNAME) - login name") },
          { "FOLDER.PICTURES", N_("$(FOLDER.PICTURES) - pictures folder") },
          { "FOLDER.HOME", N_("$(FOLDER.HOME) - home folder") },
          { "FOLDER.DESKTOP", N_("$(FOLDER.DESKTOP) - desktop folder") },
          { "OPENCL.ACTIVATED", N_("$(OPENCL.ACTIVATED) - whether OpenCL is activated") },
          { "CATEGORY", N_("$(CATEGORY0(category)) - subtag of level 0 in hierarchical tags") },
          { "TAGS", N_("$(TAGS) - tags as set in metadata settings") },
          { "DARKTABLE.NAME", N_("$(DARKTABLE.NAME) - darktable name") },
          { "DARKTABLE.VERSION", N_("$(DARKTABLE.VERSION) - current darktable version") },
          { NULL, NULL } };

  return default_path_compl_list;
}

/**
 * Builds the tooltip text for a GtkEntry. Uses the same datatype as
 * used for initializing the auto completion table above.
 *
 * @return g_malloc()'ed string. Must be free'd by the caller.
 */
gchar *dt_gtkentry_build_completion_tooltip_text(const gchar *header,
                                                 const dt_gtkentry_completion_spec *compl_list)
{
  size_t array_len = 0;
  for(dt_gtkentry_completion_spec const *p = compl_list; p->description != NULL; p++) array_len++;
  const gchar **lines = malloc(sizeof(gchar *) * (array_len + 2));
  const gchar **l = lines;
  *l++ = header;

  for(dt_gtkentry_completion_spec const *p = compl_list; p->description != NULL; p++, l++)
    *l = _(p->description);

  *l = NULL;

  gchar *ret = g_strjoinv("\n", (gchar **)lines);

  free(lines);

  return ret;
}


// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

