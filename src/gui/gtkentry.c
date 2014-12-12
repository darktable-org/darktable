/*
    This file is part of darktable,
    copyright (c) 2009--2012 christian tellefsen

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
#include "gtkentry.h"

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
  gchar *end;
  gint del_end_pos = -1;

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

  end = s + cur_pos;

  if(end)
  {
    del_end_pos = end - s + 1;
  }
  else
  {
    del_end_pos = cur_pos;
  }

  size_t text_len = strlen(varname) + 2;
  gchar *addtext = (gchar *)g_malloc(text_len);
  snprintf(addtext, text_len, "%s)", varname);

  gtk_editable_delete_text(e, p, del_end_pos);
  gtk_editable_insert_text(e, addtext, -1, &p);
  gtk_editable_set_position(e, p);
  g_value_unset(&value);
  g_free(addtext);
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
  gchar *item = NULL;
  gchar *normalized_string;
  gchar *case_normalized_string;
  gboolean ret = FALSE;
  GtkTreeModel *model = gtk_entry_completion_get_model(completion);

  GtkEditable *e = (GtkEditable *)gtk_entry_completion_get_entry(completion);
  gint cur_pos = gtk_editable_get_position(e); /* returns 1..* */
  gint p = cur_pos;
  gint var_start;
  gboolean var_present = FALSE;

  for(p = cur_pos; p >= 0; p--)
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

    gtk_tree_model_get(model, iter, COMPL_VARNAME, &item, -1);

    if(item != NULL)
    {
      // Do utf8-safe case insensitive string compare.
      // Shamelessly stolen from GtkEntryCompletion.
      normalized_string = g_utf8_normalize(item, -1, G_NORMALIZE_ALL);

      if(normalized_string != NULL)
      {
        case_normalized_string = g_utf8_casefold(normalized_string, -1);

        if(!g_ascii_strncasecmp(varname, case_normalized_string, strlen(varname))) ret = TRUE;

        g_free(case_normalized_string);
      }
      g_free(normalized_string);
    }
    g_free(varname);
  }
  g_free(item);

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
      = { { "ROLL_NAME", N_("$(ROLL_NAME) - roll of the input image") },
          { "FILE_FOLDER", N_("$(FILE_FOLDER) - folder containing the input image") },
          { "FILE_NAME", N_("$(FILE_NAME) - basename of the input image") },
          { "FILE_EXTENSION", N_("$(FILE_EXTENSION) - extension of the input image") },
          { "VERSION", N_("$(VERSION) - duplicate version") },
          { "SEQUENCE", N_("$(SEQUENCE) - sequence number") },
          { "YEAR", N_("$(YEAR) - year") },
          { "MONTH", N_("$(MONTH) - month") },
          { "DAY", N_("$(DAY) - day") },
          { "HOUR", N_("$(HOUR) - hour") },
          { "MINUTE", N_("$(MINUTE) - minute") },
          { "SECOND", N_("$(SECOND) - second") },
          { "EXIF_YEAR", N_("$(EXIF_YEAR) - EXIF year") },
          { "EXIF_MONTH", N_("$(EXIF_MONTH) - EXIF month") },
          { "EXIF_DAY", N_("$(EXIF_DAY) - EXIF day") },
          { "EXIF_HOUR", N_("$(EXIF_HOUR) - EXIF hour") },
          { "EXIF_MINUTE", N_("$(EXIF_MINUTE) - EXIF minute") },
          { "EXIF_SECOND", N_("$(EXIF_SECOND) - EXIF second") },
          { "EXIF_ISO", N_("$(EXIF_ISO) - ISO value") },
          { "STARS", N_("$(STARS) - star rating") },
          { "LABELS", N_("$(LABELS) - colorlabels") },
          { "PICTURES_FOLDER", N_("$(PICTURES_FOLDER) - pictures folder") },
          { "HOME", N_("$(HOME) - home folder") },
          { "DESKTOP", N_("$(DESKTOP) - desktop folder") },
          { "TITLE", N_("$(TITLE) - title from metadata") },
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
  const gchar *lines[array_len + 2];
  const gchar **l = lines;
  *l++ = header;

  for(dt_gtkentry_completion_spec const *p = compl_list; p->description != NULL; p++, l++)
    *l = _(p->description);

  *l = NULL;

  return g_strjoinv("\n", (gchar **)lines);
}


// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
