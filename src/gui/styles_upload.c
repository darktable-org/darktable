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
#include "common/history.h"
#include "common/file_location.h"
#include "common/imageio.h"
#include "common/imageio_module.h"
#include "control/control.h"
#include "gui/styles.h"
#include "gui/gtk.h"
#include "dtgtk/label.h"
#include "views/view.h"
#include <gdk/gdk.h>
#include <curl/curl.h>

typedef struct dt_gui_styles_upload_dialog_t
{
  int32_t beforeid, afterid;
  gchar *nameorig;
  GtkWidget *name, *username, *password, *agreement;
  GtkTextBuffer *description;
} dt_gui_styles_upload_dialog_t;

void _temp_export(int32_t id, const char* dir, const char* filename, int width, int height)
{
  /* this function is more or less copied from src/cli/main.c, some adjustments may be needed */
  int size = 0, dat_size = 0;
  dt_imageio_module_format_t *format;
  dt_imageio_module_storage_t *storage;
  dt_imageio_module_data_t *sdata, *fdata;
  char *path;
  path = g_strconcat(dir, "/", filename, NULL);

  storage = dt_imageio_get_storage_by_name("disk"); // only exporting to disk makes sense
  if(storage == NULL)
  {
    fprintf(stderr, "%s\n", _("cannot find disk storage module. please check your installation, something seems to be broken."));
    return;
  }

  sdata = storage->get_params(storage, &size);
  if(sdata == NULL)
  {
    fprintf(stderr, "%s\n", _("failed to get parameters from storage module, aborting export ..."));
    return;
  }

  // and now for the really ugly hacks. don't tell your children about this one or they won't sleep at night any longer ...
  g_strlcpy((char*)sdata, path, DT_MAX_PATH_LEN);
  // all is good now, the last line didn't happen.

  format = dt_imageio_get_format_by_name("jpeg");
  if(format == NULL)
  {
    fprintf(stderr, _("failed to get format jpeg"));
    fprintf(stderr, "\n");
    return;
  }

  fdata = format->get_params(format, &dat_size);
  if(fdata == NULL)
  {
    fprintf(stderr, "%s\n", _("failed to get parameters from format module, aborting export ..."));
    return;
  }

  uint32_t w,h,fw,fh,sw,sh;
  fw=fh=sw=sh=0;
  storage->dimension(storage, &sw, &sh);
  format->dimension(format, &fw, &fh);

  if( sw==0 || fw==0) w=sw>fw?sw:fw;
  else w=sw<fw?sw:fw;

  if( sh==0 || fh==0) h=sh>fh?sh:fh;
  else h=sh<fh?sh:fh;

  fdata->max_width  = width;
  fdata->max_height = height;
  fdata->max_width = (w!=0 && fdata->max_width >w)?w:fdata->max_width;
  fdata->max_height = (h!=0 && fdata->max_height >h)?h:fdata->max_height;
  fdata->style[0] = '\0';

  //TODO: add a callback to set the bpp without going through the config

  storage->store(sdata, id, format, fdata, 1, 1, FALSE);

  // cleanup time
  if(storage->finalize_store) storage->finalize_store(storage, sdata);
  storage->free_params(storage, sdata);
  format->free_params(format, fdata);

}

void _curl_upload_style(const char* dir, dt_gui_styles_upload_dialog_t *sd)
{

  /* All the values to be used */
  const char *upload_url, *name, *style, *image_sb, *image_sa, *image_bb, *image_ba, *username, *password;
  upload_url = "http://darktablestyles.sourceforge.net/upload.php";
  name       = gtk_entry_get_text ( GTK_ENTRY (sd->name));
  style      = g_strconcat(dir, "/", name, ".dtstyle", NULL);
  image_sb   = g_strconcat(dir, "/small-before.jpg", NULL);
  image_sa   = g_strconcat(dir, "/small-after.jpg", NULL);
  image_bb   = g_strconcat(dir, "/big-before.jpg", NULL);
  image_ba   = g_strconcat(dir, "/big-after.jpg", NULL);
  username   = gtk_entry_get_text ( GTK_ENTRY (sd->username));
  password   = gtk_entry_get_text ( GTK_ENTRY (sd->password));

  CURL *curl;
  CURLcode res;

  struct curl_httppost *formpost = NULL;
  struct curl_httppost *lastptr = NULL;
  struct curl_slist *headerlist = NULL;
  static const char buf[] = "Expect:";

  curl_global_init(CURL_GLOBAL_ALL);

  /* Set all files to send */
  curl_formadd(&formpost, &lastptr, CURLFORM_COPYNAME, "file_data",
               CURLFORM_FILE, style, CURLFORM_END);

  curl_formadd(&formpost, &lastptr, CURLFORM_COPYNAME, "small_before",
               CURLFORM_FILE, image_sb, CURLFORM_END);

  curl_formadd(&formpost, &lastptr, CURLFORM_COPYNAME, "small_after",
               CURLFORM_FILE, image_sa, CURLFORM_END);

  curl_formadd(&formpost, &lastptr, CURLFORM_COPYNAME, "big_before",
               CURLFORM_FILE, image_bb, CURLFORM_END);

  curl_formadd(&formpost, &lastptr, CURLFORM_COPYNAME, "big_after",
               CURLFORM_FILE, image_ba, CURLFORM_END);

  /* Set credentials */
  curl_formadd(&formpost, &lastptr, CURLFORM_COPYNAME, "username",
               CURLFORM_COPYCONTENTS, username, CURLFORM_END);

  curl_formadd(&formpost, &lastptr, CURLFORM_COPYNAME, "p",
               CURLFORM_COPYCONTENTS, password, CURLFORM_END);
		
  curl = curl_easy_init();
  headerlist = curl_slist_append(headerlist, buf);
  if(curl) {
    curl_easy_setopt(curl, CURLOPT_URL, upload_url);      
    curl_easy_setopt(curl, CURLOPT_HTTPPOST, formpost);
    res = curl_easy_perform(curl);
    /* Check for errors */
    if(res != CURLE_OK)
      fprintf(stderr, "curl_easy_perform() failed: %s\n",
              curl_easy_strerror(res));

    curl_easy_cleanup(curl);
    curl_formfree(formpost);
    curl_slist_free_all (headerlist);
  }
}

static void _gui_styles_upload_response(GtkDialog *dialog, gint response_id, dt_gui_styles_upload_dialog_t *sd)
{
  if (response_id == GTK_RESPONSE_ACCEPT)
  {
    /* return if not all required settings are set */
    if (strlen(gtk_entry_get_text ( GTK_ENTRY (sd->name))) == 0
      || strlen(gtk_entry_get_text ( GTK_ENTRY (sd->username))) == 0
      || strlen(gtk_entry_get_text ( GTK_ENTRY (sd->password))) == 0
      || !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(sd->agreement)) )
    {
      return;
    }
    char *name, *description, *dir = "/tmp";
    const char *newname;
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(sd->description, &start, &end);
    description = gtk_text_buffer_get_text (sd->description, &start, &end, FALSE);
    newname = gtk_entry_get_text ( GTK_ENTRY (sd->name));
    name = sd->nameorig;
    // not sure how to get platform specific tmp dir...
    //dt_loc_init_tmp_dir(dir);
    dt_styles_update (name, newname, description, NULL);
    dt_styles_save_to_file(newname, dir, TRUE);
    _temp_export(sd->beforeid, dir, "small-before", 200, 150);
    _temp_export(sd->afterid, dir, "small-after", 200, 150);
    _temp_export(sd->beforeid, dir, "big-before", 800, 600);
    _temp_export(sd->afterid, dir, "big-after", 800, 600);
    _curl_upload_style(dir, sd);
    unlink(g_strconcat(dir, "/small-before.jpg", NULL));
    unlink(g_strconcat(dir, "/small-after.jpg", NULL));
    unlink(g_strconcat(dir, "/big-before.jpg", NULL));
    unlink(g_strconcat(dir, "/big-after.jpg", NULL));
    unlink(g_strconcat(dir, "/", newname , ".dtstyle", NULL));
  }

  gtk_widget_destroy(GTK_WIDGET(dialog));
  dt_image_remove(sd->beforeid);
  dt_image_remove(sd->afterid);
  g_free(sd->nameorig);
  g_free(sd);
}

static void _expose_thumbnail(GtkWidget *widget, cairo_t *cr,
    gpointer user_data)
{
  int32_t imgid = *(int32_t*)user_data;
  GdkWindow *window = gtk_widget_get_window(widget);
  int size = 150;
  gtk_widget_set_size_request (widget, size, size);
  cr = gdk_cairo_create(window);
  dt_view_image_over_t * image_over = (dt_view_image_over_t *)DT_VIEW_REJECT;
  dt_view_image_expose(image_over, imgid, cr, size, size, 6, 0, 0, FALSE);
  cairo_destroy(cr);
  /* well the images aren't really exposed for me, unless I add the following
   * command, but that would create an infinite loop of redrawing the thumbnails,
   * so I'm not really sure how to solve it, but I'm sure we'll figure out :)
  dt_control_queue_redraw_widget(widget);
  */
}

void _gui_init (dt_gui_styles_upload_dialog_t *sd)
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
  GtkBox *thumbnails = GTK_BOX(gtk_vbox_new(FALSE, 5));
  gtk_box_pack_start (hbox,GTK_WIDGET(settings),TRUE,TRUE,0);
  gtk_box_pack_start (hbox,GTK_WIDGET(thumbnails),FALSE,FALSE,0);

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
  g_object_set (sd->username, "tooltip-text", _("your username at www.darktable.org/redmine"), (char *)NULL);
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


  /* set thumbnails */

  label = dtgtk_label_new(_("before"), DARKTABLE_LABEL_TAB | DARKTABLE_LABEL_ALIGN_RIGHT);
  gtk_box_pack_start (thumbnails,GTK_WIDGET(label),FALSE,FALSE,0);
  GtkWidget *before = gtk_drawing_area_new();
  g_signal_connect(G_OBJECT(before), "expose-event",
      G_CALLBACK(_expose_thumbnail), (gpointer)&sd->beforeid);
  gtk_box_pack_start (thumbnails,GTK_WIDGET(before),FALSE,FALSE,0);
   
  label = dtgtk_label_new(_("after"), DARKTABLE_LABEL_TAB | DARKTABLE_LABEL_ALIGN_RIGHT);
  gtk_box_pack_start (thumbnails,GTK_WIDGET(label),FALSE,FALSE,0);
  GtkWidget *after = gtk_drawing_area_new();
  g_signal_connect(G_OBJECT(after), "expose-event",
      G_CALLBACK(_expose_thumbnail), (gpointer)&sd->afterid);
  gtk_box_pack_start (thumbnails,GTK_WIDGET(after),FALSE,FALSE,0);

  /* set response and draw dialog */
  g_signal_connect (dialog, "response", G_CALLBACK (_gui_styles_upload_response), sd);
  gtk_widget_show_all (GTK_WIDGET (dialog));
  gtk_dialog_run(GTK_DIALOG(dialog));

}

void dt_gui_styles_upload (const char *name,int imgid)
{
  dt_gui_styles_upload_dialog_t *sd=(dt_gui_styles_upload_dialog_t *)g_malloc (sizeof (dt_gui_styles_upload_dialog_t));
  sd->nameorig = g_strdup(name);

  /* create beforeimage */
  int duplicateid = dt_image_duplicate (imgid);
  if(duplicateid != -1) dt_history_copy_and_paste_on_image(imgid, duplicateid, FALSE,NULL);
  dt_styles_remove_from_image(name, duplicateid);
  sd->beforeid = duplicateid;

  /* create afterimage */
  duplicateid = dt_image_duplicate (imgid);
  if(duplicateid != -1) dt_history_copy_and_paste_on_image(imgid, duplicateid, FALSE,NULL);
  dt_styles_apply_to_image(name, FALSE, duplicateid);
  sd->afterid = duplicateid;

  _gui_init(sd);
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
