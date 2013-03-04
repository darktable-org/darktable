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
#include "common/mipmap_cache.h"
#include "common/pwstorage/pwstorage.h"
#include "control/control.h"
#include "control/jobs/control_jobs.h"
#include "gui/styles.h"
#include "gui/gtk.h"
#include "dtgtk/label.h"
#include "views/view.h"
#include <gdk/gdk.h>
#include <curl/curl.h>

// Note that the size here has nothing to do with the thumbs online
#define _THUMBNAIL_WIDTH 150
#define _THUMBNAIL_HEIGHT 150
#define _STYLES_SERVER "http://darktablestyles.sourceforge.net/"

typedef struct dt_gui_styles_upload_dialog_t
{
  int32_t beforeid, afterid;
  gchar *nameorig;
  GtkWidget *name, *username, *password, *auth_label, *save_local, *agreement;
  GtkTextBuffer *description;
} dt_gui_styles_upload_dialog_t;

size_t static _curl_parse_response(void *buffer, size_t size, size_t nmemb, void *userp)
{
    char **response_ptr =  (char**)userp;
    *response_ptr = strndup(buffer, (size_t)(size *nmemb));
    return nmemb * size;
}


gchar* _curl_send(const char* url, struct curl_httppost *formpost)
{

  CURL *curl;
  CURLcode res;
  gchar *response;
  
  struct curl_slist *headerlist = NULL;
  static const char buf[] = "Expect:";

  curl_global_init(CURL_GLOBAL_ALL);

  curl = curl_easy_init();
  headerlist = curl_slist_append(headerlist, buf);
  if(curl) {
    curl_easy_setopt(curl, CURLOPT_URL, url);      
    curl_easy_setopt(curl, CURLOPT_HTTPPOST, formpost);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, _curl_parse_response);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    res = curl_easy_perform(curl);
    /* Check for errors */
    if(res != CURLE_OK)
      return "curlcode_error";

    curl_easy_cleanup(curl);
    curl_formfree(formpost);
    curl_slist_free_all (headerlist);
    return response;
  }
  return "nocurl_error";
}

gboolean _curl_authenticate(dt_gui_styles_upload_dialog_t *sd)
{

  /* All the values to be used */
  const char *url, *name, *username, *password;
  gchar mup[512]= {0};
  sprintf( mup,"<span foreground=\"#ffffff\" ><small>%s</small></span>",_("authenticating..."));
  gtk_label_set_markup(GTK_LABEL(sd->auth_label), mup);

  url      = _STYLES_SERVER "authenticate.php";
  name     = gtk_entry_get_text ( GTK_ENTRY (sd->name));
  username = gtk_entry_get_text ( GTK_ENTRY (sd->username));
  password = gtk_entry_get_text ( GTK_ENTRY (sd->password));

  struct curl_httppost *formpost = NULL;
  struct curl_httppost *lastptr = NULL;

  /* Set credentials */
  curl_formadd(&formpost, &lastptr, CURLFORM_COPYNAME, "name",
               CURLFORM_COPYCONTENTS, name, CURLFORM_END);
               
  curl_formadd(&formpost, &lastptr, CURLFORM_COPYNAME, "username",
               CURLFORM_COPYCONTENTS, username, CURLFORM_END);

  curl_formadd(&formpost, &lastptr, CURLFORM_COPYNAME, "p",
               CURLFORM_COPYCONTENTS, password, CURLFORM_END);
  
  gchar *response = g_strdup(_curl_send(url, formpost));

  if (g_strcmp0(response, "success") == 0)
  {
    /* Add creds to pwstorage */
    GHashTable *table = g_hash_table_new(g_str_hash, g_str_equal);
    gchar* g_u = g_strdup(username);
    gchar* g_p = g_strdup(password);
    
    g_hash_table_insert(table, "username", g_u);
    g_hash_table_insert(table, "password", g_p);

    if( !dt_pwstorage_set("redmine", table) )
    {
      dt_print(DT_DEBUG_PWSTORAGE,"[redmine] cannot store username/password\n");
    }

    g_free(g_u);
    g_free(g_p);
    g_hash_table_destroy(table);

    g_free(response);
    return TRUE;
  }
  else
  {
    fprintf(stderr, "%s%s\n", _("redmine authentication failed: "), response);
    sprintf( mup,"<span foreground=\"#e07f7f\" ><small>%s</small></span>",_("authentication failed"));
    gtk_label_set_markup(GTK_LABEL(sd->auth_label), mup);
    g_free(response);
    return FALSE;
  }
}

static gboolean _gui_styles_upload_response(GtkDialog *dialog, gint response_id, dt_gui_styles_upload_dialog_t *sd)
{
  if (response_id == GTK_RESPONSE_ACCEPT)
  {
    /* return if not all required settings are set */
    if (strlen(gtk_entry_get_text ( GTK_ENTRY (sd->name))) == 0
      || strlen(gtk_entry_get_text ( GTK_ENTRY (sd->username))) == 0
      || strlen(gtk_entry_get_text ( GTK_ENTRY (sd->password))) == 0
      || !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(sd->agreement))
      || !_curl_authenticate(sd) )
    {
      return FALSE;
    }

    /* extract values from dialog and destroy it */
    gchar *nameorig, *name, *username, *password, *description, *url;
    GtkTextIter start, end;
    gboolean save_local;
    nameorig = sd->nameorig;
    name     = g_strdup(gtk_entry_get_text ( GTK_ENTRY (sd->name)));
    username = g_strdup(gtk_entry_get_text ( GTK_ENTRY (sd->username)));
    password = g_strdup(gtk_entry_get_text ( GTK_ENTRY (sd->password)));
    gtk_text_buffer_get_bounds(sd->description, &start, &end);
    description = gtk_text_buffer_get_text (sd->description, &start, &end, FALSE);
    save_local  = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(sd->save_local));
    url = _STYLES_SERVER "upload.php";
    gtk_widget_destroy(GTK_WIDGET(dialog));
    
    dt_control_upload_style(sd->beforeid, sd->afterid, nameorig, name, username, password, description, url);
    
    if (save_local)
      dt_styles_update (nameorig, name, description, NULL);

  }
  else
  {
    dt_image_remove(sd->beforeid);
    dt_image_remove(sd->afterid);
    gtk_widget_destroy(GTK_WIDGET(dialog));  
  }

  g_free(sd);
  return TRUE;
}

static gboolean _redraw_thumbnail(gpointer user_data)
{
  GtkWidget *widget = (GtkWidget*)user_data;
  dt_control_queue_redraw_widget(widget);
  return FALSE;
}

static void _expose_thumbnail(GtkWidget *widget, cairo_t *cr,
    gpointer user_data)
{
  int32_t imgid = *(int32_t*)user_data;
  GdkWindow *window = gtk_widget_get_window(widget);
  cr = gdk_cairo_create(window);
  
  // make sure the mipmap exists
  dt_mipmap_size_t mip = dt_mipmap_cache_get_matching_size(darktable.mipmap_cache, _THUMBNAIL_WIDTH, _THUMBNAIL_HEIGHT);
  dt_mipmap_buffer_t buf;
  dt_mipmap_cache_read_get(darktable.mipmap_cache, &buf, imgid, mip, DT_MIPMAP_BEST_EFFORT);
  if (buf.buf)
  {
    dt_view_image_over_t * image_over = (dt_view_image_over_t *)DT_VIEW_REJECT;
    dt_view_image_expose(image_over, imgid, cr, _THUMBNAIL_WIDTH, _THUMBNAIL_HEIGHT, 6, 0, 0, FALSE);
  }
  else
  {
    // try to redraw thumbnail after 500ms if mipmap isn't present
    g_timeout_add(500, _redraw_thumbnail, widget);
  }
  dt_mipmap_cache_read_release(darktable.mipmap_cache, &buf);
  cairo_destroy(cr);
}

void _gui_init (dt_gui_styles_upload_dialog_t *sd)
{
  /* create the dialog */
  GtkWidget *window = dt_ui_main_window(darktable.gui->ui);
  GtkDialog *dialog = GTK_DIALOG (gtk_dialog_new_with_buttons(_("upload style"),
                                  GTK_WINDOW(window),
                                  GTK_DIALOG_DESTROY_WITH_PARENT,
                                  _("cancel"), GTK_RESPONSE_REJECT,
                                  _("upload"), GTK_RESPONSE_ACCEPT,
                                  NULL));
  gtk_widget_set_name(GTK_WIDGET(dialog), "style-upload-dialog");
  
  /* create layout */
  GtkContainer *content_area = GTK_CONTAINER (gtk_dialog_get_content_area (GTK_DIALOG (dialog)));
  GtkWidget *alignment = gtk_alignment_new (0.5, 0.5, 1.0, 1.0);
  gtk_alignment_set_padding (GTK_ALIGNMENT(alignment), 5, 5, 5, 5);
  gtk_container_add (content_area, alignment);
  GtkBox *hbox = GTK_BOX(gtk_hbox_new(FALSE, 5));
  gtk_container_add (GTK_CONTAINER(alignment), GTK_WIDGET(hbox));
  GtkWidget *settings = gtk_table_new(10, 2, FALSE);
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

  GHashTable* table = dt_pwstorage_get("redmine");
  gchar* _username = g_strdup( g_hash_table_lookup(table, "username"));
  gchar* _password = g_strdup( g_hash_table_lookup(table, "password"));
  g_hash_table_destroy(table);
  
  label = gtk_label_new(_("user"));
  gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
  gtk_table_attach(GTK_TABLE(settings), GTK_WIDGET(label), 0, 1, 2, 3, 0, 0, 0, 0);
  sd->username = gtk_entry_new();
  g_object_set (sd->username, "tooltip-text", _("your username at www.darktable.org/redmine"), (char *)NULL);
  gtk_entry_set_text(GTK_ENTRY(sd->username),  _username == NULL?"":_username);
  gtk_table_attach(GTK_TABLE(settings), GTK_WIDGET(sd->username), 1, 2, 2, 3, GTK_EXPAND|GTK_FILL, 0, 0, 0);

  label = gtk_label_new(_("password"));
  gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
  gtk_table_attach(GTK_TABLE(settings), GTK_WIDGET(label), 0, 1, 3, 4, 0, 0, 0, 0);
  sd->password = gtk_entry_new();
  g_object_set (sd->password, "tooltip-text", _("your password"), (char *)NULL);
  gtk_entry_set_visibility (GTK_ENTRY(sd->password), FALSE);
  gtk_entry_set_text(GTK_ENTRY(sd->password),  _password == NULL?"":_password);
  gtk_table_attach(GTK_TABLE(settings), GTK_WIDGET(sd->password), 1, 2, 3, 4, GTK_EXPAND|GTK_FILL, 0, 0, 0);

  sd->auth_label = gtk_label_new(NULL);
  gtk_table_attach(GTK_TABLE(settings), GTK_WIDGET(sd->auth_label), 1, 2, 4, 5, GTK_EXPAND|GTK_FILL, 0, 0, 0);

  label = dtgtk_label_new(_("description"), DARKTABLE_LABEL_TAB | DARKTABLE_LABEL_ALIGN_RIGHT);
  gtk_table_set_row_spacing(GTK_TABLE(settings), 5, 20);
  gtk_table_attach(GTK_TABLE(settings), GTK_WIDGET(label), 0, 2, 6, 7, GTK_EXPAND|GTK_FILL, 0, 0, 0);

  GtkWidget* description;
  description = gtk_text_view_new();
  g_object_set (description, "tooltip-text", _("enter a description for the style"), (char *)NULL);
  sd->description = gtk_text_view_get_buffer (GTK_TEXT_VIEW (description));
  gchar *olddesc = dt_styles_get_description (sd->nameorig);
  gtk_text_buffer_set_text (sd->description, olddesc, -1);
  gtk_text_view_set_wrap_mode (GTK_TEXT_VIEW(description), GTK_WRAP_WORD);
  GtkWidget* scrolledwindow = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW(scrolledwindow), GTK_POLICY_NEVER, GTK_POLICY_ALWAYS);
  gtk_container_add(GTK_CONTAINER(scrolledwindow), description);
  gtk_table_attach(GTK_TABLE(settings), GTK_WIDGET(scrolledwindow), 0, 2, 7, 8, GTK_EXPAND|GTK_FILL, GTK_EXPAND|GTK_FILL, 0, 0);

  sd->save_local = gtk_check_button_new_with_label(_("save changes locally"));
  g_object_set (sd->save_local, "tooltip-text", _("do you want to save changes in name and description locally too?"), (char *)NULL);
  gtk_table_attach(GTK_TABLE(settings), GTK_WIDGET(sd->save_local), 0, 2, 8, 9, GTK_EXPAND|GTK_FILL, 0, 0, 0);

  sd->agreement = gtk_check_button_new_with_label(_("I accept the user agreement"));
  g_object_set (sd->agreement, "tooltip-text", _("you must accept the user agreement to upload style"), (char *)NULL);
  gtk_table_attach(GTK_TABLE(settings), GTK_WIDGET(sd->agreement), 0, 2, 9, 10, GTK_EXPAND|GTK_FILL, 0, 0, 0);


  /* set thumbnails */

  label = dtgtk_label_new(_("before"), DARKTABLE_LABEL_TAB | DARKTABLE_LABEL_ALIGN_RIGHT);
  gtk_box_pack_start (thumbnails,GTK_WIDGET(label),FALSE,FALSE,0);
  GtkWidget *before = gtk_drawing_area_new();
  gtk_widget_set_size_request (before, _THUMBNAIL_WIDTH, _THUMBNAIL_HEIGHT);
  g_signal_connect(G_OBJECT(before), "expose-event",
      G_CALLBACK(_expose_thumbnail), (gpointer)&sd->beforeid);
  gtk_box_pack_start (thumbnails,GTK_WIDGET(before),FALSE,FALSE,0);
   
  label = dtgtk_label_new(_("after"), DARKTABLE_LABEL_TAB | DARKTABLE_LABEL_ALIGN_RIGHT);
  gtk_box_pack_start (thumbnails,GTK_WIDGET(label),FALSE,FALSE,0);
  GtkWidget *after = gtk_drawing_area_new();
  gtk_widget_set_size_request (after, _THUMBNAIL_WIDTH, _THUMBNAIL_HEIGHT);
  g_signal_connect(G_OBJECT(after), "expose-event",
      G_CALLBACK(_expose_thumbnail), (gpointer)&sd->afterid);
  gtk_box_pack_start (thumbnails,GTK_WIDGET(after),FALSE,FALSE,0);

  g_free(_username);
  g_free(_password);

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
