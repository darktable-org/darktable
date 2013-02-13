/*
    This file is part of darktable,
    copyright (c) 2013 robert rosman

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
#include "common/darktable.h"
#include "common/styles.h"
//#include "common/history.h"
//#include "develop/imageop.h"
//#include "control/control.h"
#include "gui/styles.h"
#include "gui/gtk.h"
#include "dtgtk/label.h"

typedef struct dt_gui_styles_upload_dialog_t
{
  int32_t imgid, duplicateid;
  gchar *nameorig;
  GtkWidget *name, *username, *password, *agreement;
  GtkTextBuffer *description;
} dt_gui_styles_upload_dialog_t;

static void _gui_styles_upload_response(GtkDialog *dialog, gint response_id, dt_gui_styles_upload_dialog_t *sd)
{
  if (response_id == GTK_RESPONSE_ACCEPT)
  {
    /* check if all required settings are set */
    if (strlen(gtk_entry_get_text ( GTK_ENTRY (sd->name))) == 0
      || strlen(gtk_entry_get_text ( GTK_ENTRY (sd->username))) == 0
      || strlen(gtk_entry_get_text ( GTK_ENTRY (sd->password))) == 0
      || !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(sd->agreement)) )
    {
      return;
    }
    char* description;
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(sd->description, &start, &end);
    description = gtk_text_buffer_get_text (sd->description, &start, &end, FALSE);
    // save changes
    // export style
    // export images
    // _curl_upload_style(sd);
    // delete exported images
    puts (description);
  }

  gtk_widget_destroy(GTK_WIDGET(dialog));
  g_free(sd->nameorig);
  g_free(sd);
}

void
_gui_init (dt_gui_styles_upload_dialog_t *sd)
{
  /* create the dialog */
  GtkWidget *window = dt_ui_main_window(darktable.gui->ui);
  GtkDialog *dialog = GTK_DIALOG (gtk_dialog_new_with_buttons(_("upload style"),
                                  GTK_WINDOW(window),
                                  GTK_DIALOG_DESTROY_WITH_PARENT,
                                  _("cancel"),
                                  GTK_RESPONSE_REJECT,
                                  _("upload"),
                                  GTK_RESPONSE_ACCEPT,
                                  NULL));

  /* create layout */
  GtkContainer *content_area = GTK_CONTAINER (gtk_dialog_get_content_area (GTK_DIALOG (dialog)));
  GtkWidget *alignment = gtk_alignment_new (0.5, 0.5, 1.0, 1.0);
  gtk_alignment_set_padding (GTK_ALIGNMENT(alignment), 5, 5, 5, 5);
  gtk_container_add (content_area, alignment);
  GtkBox *hbox = GTK_BOX(gtk_hbox_new(FALSE, 5));
  gtk_container_add (GTK_CONTAINER(alignment), GTK_WIDGET(hbox));
  GtkWidget *settings = gtk_table_new(8, 2, FALSE);
  gtk_table_set_row_spacings(GTK_TABLE(settings), 5);
  GtkBox *previews = GTK_BOX(gtk_vbox_new(FALSE, 5));
  gtk_box_pack_start (hbox,GTK_WIDGET(settings),TRUE,TRUE,0);
  gtk_box_pack_start (hbox,GTK_WIDGET(previews),TRUE,TRUE,0);

  /* add fields to settings */
  GtkWidget *label;

  label = dtgtk_label_new(_("general options"), DARKTABLE_LABEL_TAB | DARKTABLE_LABEL_ALIGN_RIGHT);
  gtk_table_attach(GTK_TABLE(settings), GTK_WIDGET(label), 0, 2, 0, 1, GTK_EXPAND|GTK_FILL, 0, 0, 0);

  label = gtk_label_new(_("style name"));
  gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
  gtk_table_attach(GTK_TABLE(settings), GTK_WIDGET(label), 0, 1, 1, 2, 0, 0, 0, 0);
  sd->name = gtk_entry_new();
  gtk_entry_set_text(GTK_ENTRY(sd->name), sd->nameorig);
  g_object_set (sd->name, "tooltip-text", _("enter a name for the style"), (char *)NULL);
  gtk_table_attach(GTK_TABLE(settings), GTK_WIDGET(sd->name), 1, 2, 1, 2, GTK_EXPAND|GTK_FILL, 0, 0, 0);

  label = gtk_label_new("username");
  gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
  gtk_table_attach(GTK_TABLE(settings), GTK_WIDGET(label), 0, 1, 2, 3, 0, 0, 0, 0);
  sd->username = gtk_entry_new();
  g_object_set (sd->username, "tooltip-text", _("your username at darktable.org/redmine"), (char *)NULL);
  gtk_table_attach(GTK_TABLE(settings), GTK_WIDGET(sd->username), 1, 2, 2, 3, GTK_EXPAND|GTK_FILL, 0, 0, 0);

  label = gtk_label_new("password");
  gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
  gtk_table_attach(GTK_TABLE(settings), GTK_WIDGET(label), 0, 1, 3, 4, 0, 0, 0, 0);
  sd->password = gtk_entry_new();
  g_object_set (sd->password, "tooltip-text", _("your password"), (char *)NULL);
  gtk_entry_set_visibility (GTK_ENTRY(sd->password), FALSE);
  gtk_table_attach(GTK_TABLE(settings), GTK_WIDGET(sd->password), 1, 2, 3, 4, GTK_EXPAND|GTK_FILL, 0, 0, 0);

  label = dtgtk_label_new(_("description"), DARKTABLE_LABEL_TAB | DARKTABLE_LABEL_ALIGN_RIGHT);
  gtk_table_set_row_spacing(GTK_TABLE(settings), 4, 20);
  gtk_table_attach(GTK_TABLE(settings), GTK_WIDGET(label), 0, 2, 5, 6, GTK_EXPAND|GTK_FILL, 0, 0, 0);

  GtkWidget* description;
  description = gtk_text_view_new();
  g_object_set (description, "tooltip-text", _("enter a description for the style"), (char *)NULL);
  sd->description = gtk_text_view_get_buffer (GTK_TEXT_VIEW (description));
  gchar *olddesc = dt_styles_get_description (sd->nameorig);
  gtk_text_buffer_set_text (sd->description, olddesc, -1);
  gtk_text_view_set_wrap_mode (GTK_TEXT_VIEW(description), GTK_WRAP_CHAR);
  GtkWidget* scrolledwindow = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW(scrolledwindow), GTK_POLICY_NEVER, GTK_POLICY_ALWAYS);
  gtk_container_add(GTK_CONTAINER(scrolledwindow), description);
  gtk_table_attach(GTK_TABLE(settings), GTK_WIDGET(scrolledwindow), 0, 2, 6, 7, GTK_EXPAND|GTK_FILL, GTK_EXPAND|GTK_FILL, 0, 0);

  sd->agreement = gtk_check_button_new_with_label(_("I accept the user agreement"));
  g_object_set (sd->agreement, "tooltip-text", _("you must accept the user agreement to upload style"), (char *)NULL);
  gtk_table_attach(GTK_TABLE(settings), GTK_WIDGET(sd->agreement), 0, 2, 7, 8, GTK_EXPAND|GTK_FILL, 0, 0, 0);


  /* fill previews 

  label = dtgtk_label_new(_("before"), DARKTABLE_LABEL_TAB | DARKTABLE_LABEL_ALIGN_RIGHT);
  gtk_box_pack_start (previews,GTK_WIDGET(label),FALSE,FALSE,0);
  * 
  label = dtgtk_label_new(_("after"), DARKTABLE_LABEL_TAB | DARKTABLE_LABEL_ALIGN_RIGHT);
  gtk_box_pack_start (previews,GTK_WIDGET(label),FALSE,FALSE,0);
*/

  g_signal_connect (dialog, "response", G_CALLBACK (_gui_styles_upload_response), sd);
  gtk_widget_show_all (GTK_WIDGET (dialog));
  gtk_dialog_run(GTK_DIALOG(dialog));

}

void
dt_gui_styles_upload (const char *name,int imgid)
{
  dt_gui_styles_upload_dialog_t *sd=(dt_gui_styles_upload_dialog_t *)g_malloc (sizeof (dt_gui_styles_upload_dialog_t));
  sd->nameorig = g_strdup(name);
  sd->imgid = imgid;
  // duplicate image
  // apply style to duplicate
  // set sd->imgafterid
  _gui_init(sd);
  // delete duplicate
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
