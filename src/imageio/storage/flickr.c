/*
    This file is part of darktable,
    copyright (c) 2010-2011 Jose Carlos Garcia Sogo

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

#include "dtgtk/button.h"
#include "bauhaus/bauhaus.h"
#include "gui/gtk.h"
#include "common/darktable.h"
#include "common/image.h"
#include "common/image_cache.h"
#include "common/imageio_module.h"
#include "common/imageio.h"
#include "common/tags.h"
#include "common/pwstorage/pwstorage.h"
#include "common/metadata.h"
#include "common/imageio_storage.h"
#include "control/conf.h"
#include "control/control.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <curl/curl.h>
#include <flickcurl.h>

DT_MODULE(1)

#define API_KEY "1d25b2dfcceba8c55fecb27645c968a3"
#define SHARED_SECRET "ac66b6c212be6f0c"

typedef struct _flickr_api_context_t
{
  flickcurl *fc;

  gboolean needsReauthentication;

  /** Current album used when posting images... */
  flickcurl_photoset *current_album;

  char *album_title;
  char *album_summary;
  int album_public;
  gboolean new_album;
  gboolean error_occured;

} _flickr_api_context_t;

typedef struct dt_storage_flickr_gui_data_t
{

  GtkLabel *status_label;
  GtkEntry *user_entry, *title_entry, *summary_entry;
  GtkWidget *export_tags;
  GtkBox *create_box;                               // Create album options...
  GtkWidget *permission_list, *album_list;

  char *user_token;

  /* List of albums */
  flickcurl_photoset **albums;

  /** Current Flickr context for the gui */
  _flickr_api_context_t *flickr_api;

} dt_storage_flickr_gui_data_t;


typedef struct dt_storage_flickr_params_t
{
  int64_t hash;
  _flickr_api_context_t *flickr_api;
  gboolean export_tags;
  gboolean public_perm;
  gboolean friend_perm;
  gboolean family_perm;
} dt_storage_flickr_params_t;


/** Authenticates and retrieves an initialized flickr api object */
static _flickr_api_context_t *_flickr_api_authenticate(dt_storage_flickr_gui_data_t *ui);

static flickcurl_upload_status *_flickr_api_upload_photo(dt_storage_flickr_params_t *params, char *data,
                                                         char *caption, char *description, gint imgid);

static void _flickr_api_free(_flickr_api_context_t *ctx)
{

  g_free(ctx->album_title);
  g_free(ctx->album_summary);

  if(ctx->current_album != NULL) flickcurl_free_photoset(ctx->current_album);

  flickcurl_free(ctx->fc);

  g_free(ctx);
}

static void _flickr_api_error_handler(void *data, const char *message)
{
  dt_control_log(_("flickr authentication: %s"), message);
  if(data)
  {
    _flickr_api_context_t *ctx = (_flickr_api_context_t *)data;
    ctx->error_occured = 1;
  }
}

static _flickr_api_context_t *_flickr_api_authenticate(dt_storage_flickr_gui_data_t *ui)
{
  char *perms = NULL, *frob;
  gchar *token;
  char *flickr_user_token = NULL;
  gint result;
  _flickr_api_context_t *ctx = (_flickr_api_context_t *)g_malloc0(sizeof(_flickr_api_context_t));

  flickcurl_init();
  ctx->fc = flickcurl_new();
  flickcurl_set_api_key(ctx->fc, API_KEY);
  flickcurl_set_shared_secret(ctx->fc, SHARED_SECRET);
  flickcurl_set_error_handler(ctx->fc, _flickr_api_error_handler, ctx);

  if(!ui->user_token)
  {
    // Retrieve stored auth_key
    // TODO: We should be able to store token for different users
    GHashTable *table = dt_pwstorage_get("flickr");
    gchar *_username = g_strdup(g_hash_table_lookup(table, "username"));
    gchar *_user_token = g_strdup(g_hash_table_lookup(table, "token"));
    g_hash_table_destroy(table);

    if(_username)
    {
      if(!strcmp(_username, gtk_entry_get_text(ui->user_entry)))
      {
        flickr_user_token = g_strdup(_user_token);
        perms = flickcurl_auth_checkToken(ctx->fc, flickr_user_token);
      }
      g_free(_username);
    }
    g_free(_user_token);
  }
  else
  {
    flickr_user_token = ui->user_token;
    perms = flickcurl_auth_checkToken(ctx->fc, ui->user_token);
  }


  if(perms)
  {
    ui->user_token = flickr_user_token;
    flickcurl_set_auth_token(ctx->fc, flickr_user_token);
    return ctx;
  }
  else if(!ctx->error_occured)
  {
    frob = flickcurl_auth_getFrob(ctx->fc);
    GError *error = NULL;
    char *sign = g_strdup_printf("%sapi_key%sfrob%spermswrite", SHARED_SECRET, API_KEY, frob);
    char *sign_md5 = g_compute_checksum_for_string(G_CHECKSUM_MD5, sign, strlen(sign));
    gchar auth_url[250];
    snprintf(auth_url, sizeof(auth_url),
             "https://flickr.com/services/auth/?api_key=%s&perms=write&frob=%s&api_sig=%s", API_KEY, frob,
             sign_md5);

    if(!gtk_show_uri(gdk_screen_get_default(), auth_url, gtk_get_current_event_time(), &error))
    {
      fprintf(stderr, "[flickr] error opening browser: %s\n", error->message);
      g_error_free(error);
    }

    g_free(sign);
    g_free(sign_md5);

    // Hold here to let the user interact
    // Show a dialog.
    gchar *text1, *text2;
    text1 = g_strdup(
        _("step 1: a new window or tab of your browser should have been loaded. you have to login into your "
          "flickr account there and authorize darktable to upload photos before continuing."));
    text2 = g_strdup(_("step 2: click the OK button once you are done."));

    GtkWidget *window = dt_ui_main_window(darktable.gui->ui);
    GtkWidget *flickr_auth_dialog
        = gtk_message_dialog_new(GTK_WINDOW(window), GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_INFO,
                                 GTK_BUTTONS_OK_CANCEL, _("flickr authentication"));
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(flickr_auth_dialog), "%s\n\n%s", text1, text2);

    result = gtk_dialog_run(GTK_DIALOG(flickr_auth_dialog));

    gtk_widget_destroy(flickr_auth_dialog);

    g_free(text1);
    g_free(text2);

    switch(result)
    {
      case GTK_RESPONSE_OK:
        token = flickcurl_auth_getToken(ctx->fc, frob);
        g_free(frob);
        // TODO: Handle timeouts errors
        if(token)
        {
          flickr_user_token = g_strdup(token);
        }
        else
        {
          g_free(token);
          _flickr_api_free(ctx);
          return NULL;
        }
        ui->user_token = g_strdup(flickr_user_token);
        flickcurl_set_auth_token(ctx->fc, flickr_user_token);

        /* Add creds to pwstorage */
        GHashTable *table = g_hash_table_new(g_str_hash, g_str_equal);
        gchar *username = (gchar *)gtk_entry_get_text(ui->user_entry);

        g_hash_table_insert(table, "username", username);
        g_hash_table_insert(table, "token", flickr_user_token);

        if(!dt_pwstorage_set("flickr", table))
        {
          dt_print(DT_DEBUG_PWSTORAGE, "[flickr] cannot store username/token\n");
        }

        g_free(flickr_user_token);
        g_hash_table_destroy(table);

        return ctx;
        break;

      default:
        dt_print(DT_DEBUG_PWSTORAGE, "[flickr] user cancelled the login process\n");
        return NULL;
    }
  }

  free(perms);

  return NULL;
}


static flickcurl_upload_status *_flickr_api_upload_photo(dt_storage_flickr_params_t *p, char *fname,
                                                         char *caption, char *description, gint imgid)
{

  flickcurl_upload_params *params = g_malloc0(sizeof(flickcurl_upload_params));
  flickcurl_upload_status *status;

  params->safety_level = 1; // Defaults to safe photos
  params->content_type = 1; // Defaults to photo (we don't support video!)

  params->title = caption;
  params->description = description;

  if(imgid)
  {
    GList *tags_list = dt_tag_get_list(imgid);
    params->tags = dt_util_glist_to_str(",", tags_list);
    g_list_free_full(tags_list, g_free);
  }
  params->photo_file = fname; // fname should be the URI of temp file

  params->is_public = (int)p->public_perm;
  params->is_friend = (int)p->friend_perm;
  params->is_family = (int)p->family_perm;

  status = flickcurl_photos_upload_params(p->flickr_api->fc, params);
  if(!status)
  {
    fprintf(stderr, "[flickr] Something went wrong when uploading");
    g_free((gchar *)params->tags);
    g_free(params);
    return NULL;
  }
  g_free((gchar *)params->tags);
  g_free(params);
  return status;
}


static char *_flickr_api_create_photoset(_flickr_api_context_t *ctx, const char *photo_id)
{
  char *photoset;
  const char *title = ctx->album_title;
  const char *summary = ctx->album_summary;

  photoset = flickcurl_photosets_create(ctx->fc, title, summary, photo_id, NULL);
  if(!photoset) fprintf(stderr, "[flickr] Something went wrong when creating gallery %s", title);
  return photoset;
}

const char *name(const struct dt_imageio_module_storage_t *self)
{
  return _("flickr webalbum");
}

/** Set status connection text */
static void set_status(dt_storage_flickr_gui_data_t *ui, gchar *message, gchar *color)
{
  if(!color) color = "#ffffff";
  gchar mup[512] = { 0 };
  snprintf(mup, sizeof(mup), "<span foreground=\"%s\" ><small>%s</small></span>", color, message);
  gtk_label_set_markup(ui->status_label, mup);
}

static void flickr_entry_changed(GtkEntry *entry, gpointer data)
{
  dt_storage_flickr_gui_data_t *ui = (dt_storage_flickr_gui_data_t *)data;

  if(ui->flickr_api != NULL)
  {
    ui->flickr_api->needsReauthentication = TRUE;
    g_free(ui->user_token);
    ui->user_token = NULL;
    set_status(ui, _("not authenticated"), "#e07f7f");
    gtk_widget_set_sensitive(GTK_WIDGET(ui->album_list), FALSE);
  }
}

static flickcurl_photoset **_flickr_api_photosets(_flickr_api_context_t *ctx, const char *user)
{
  flickcurl_photoset **photoset;
  //  char *nsid;

  // TODO: Support both userid and email. As more services uses email as username
  //      users can confuse the needed id to be introduced in the user field.
  //  nsid = flickcurl_people_findByEmail(ctx->fc, "@");

  //  no need to specify nsid at all
  //  nsid = flickcurl_people_findByUsername(ctx->fc, user);

  // "If none is specified, the calling user is assumed (or NULL) "
  // (c) http://librdf.org/flickcurl/api/flickcurl-section-photoset.html#flickcurl-photosets-getList
  photoset = flickcurl_photosets_getList(ctx->fc, NULL);

  return photoset;
}

/** Refresh albums */
static void refresh_albums(dt_storage_flickr_gui_data_t *ui)
{
  int i;
  gtk_widget_set_sensitive(GTK_WIDGET(ui->album_list), FALSE);

  if(ui->flickr_api == NULL || ui->flickr_api->needsReauthentication == TRUE)
  {
    if(ui->flickr_api != NULL) _flickr_api_free(ui->flickr_api);
    ui->flickr_api = _flickr_api_authenticate(ui);
    if(ui->flickr_api != NULL)
    {
      set_status(ui, _("authenticated"), "#7fe07f");
    }
    else
    {
      set_status(ui, _("not authenticated"), "#e07f7f");
      gtk_widget_set_sensitive(GTK_WIDGET(ui->album_list), FALSE);
      return;
    }
  }

  // First clear the cobobox except first 2 items (none / create new album)
  dt_bauhaus_combobox_clear(ui->album_list);

  ui->albums = _flickr_api_photosets(ui->flickr_api, gtk_entry_get_text(ui->user_entry));
  if(ui->albums)
  {

    // Add standard action
    dt_bauhaus_combobox_add(ui->album_list, _("without album"));
    dt_bauhaus_combobox_add(ui->album_list, _("create new album"));
//     dt_bauhaus_combobox_add(ui->album_list, ""); // Separator // FIXME: bauhaus doesn't support separators

    // Then add albums from list...
    for(i = 0; ui->albums[i]; i++)
    {
      char data[512] = { 0 };
      snprintf(data, sizeof(data), "%s (%i)", ui->albums[i]->title, ui->albums[i]->photos_count);
      dt_bauhaus_combobox_add(ui->album_list, data);
    }
    dt_bauhaus_combobox_set(ui->album_list, 2);
    gtk_widget_hide(GTK_WIDGET(ui->create_box)); // Hide create album box...
  }
  else
  {
    // Failed to parse feed of album...
    // Lets notify somehow...
    dt_bauhaus_combobox_set(ui->album_list, 0);
  }
  gtk_widget_set_sensitive(GTK_WIDGET(ui->album_list), TRUE);
}


static void flickr_album_changed(GtkComboBox *cb, gpointer data)
{
  dt_storage_flickr_gui_data_t *ui = (dt_storage_flickr_gui_data_t *)data;
  const gchar *value = dt_bauhaus_combobox_get_text(ui->album_list);
  if(value != NULL && strcmp(value, _("create new album")) == 0)
  {
    gtk_widget_set_no_show_all(GTK_WIDGET(ui->create_box), FALSE);
    gtk_widget_show_all(GTK_WIDGET(ui->create_box));
  }
  else
    gtk_widget_hide(GTK_WIDGET(ui->create_box));
}

// Refresh button pressed...
static void flickr_button1_clicked(GtkButton *button, gpointer data)
{
  dt_storage_flickr_gui_data_t *ui = (dt_storage_flickr_gui_data_t *)data;
  refresh_albums(ui);
}

/*
static gboolean
focus_in(GtkWidget *widget, GdkEventFocus *event, gpointer user_data)
{
  dt_control_tab_shortcut_off(darktable.control);
  return FALSE;
}

static gboolean
focus_out(GtkWidget *widget, GdkEventFocus *event, gpointer user_data)
{
  dt_control_tab_shortcut_on(darktable.control);
  return FALSE;
}
*/

void gui_init(dt_imageio_module_storage_t *self)
{
  self->gui_data = (dt_storage_flickr_gui_data_t *)g_malloc0(sizeof(dt_storage_flickr_gui_data_t));
  dt_storage_flickr_gui_data_t *ui = self->gui_data;
  self->widget = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(self->widget), DT_PIXEL_APPLY_DPI(5));
  gtk_grid_set_column_spacing(GTK_GRID(self->widget), DT_PIXEL_APPLY_DPI(10));
  int line = 0;

  GHashTable *table = dt_pwstorage_get("flickr");
  gchar *_username = g_strdup(g_hash_table_lookup(table, "username"));
  g_hash_table_destroy(table);

  GtkWidget *hbox, *label, *button;


  label = gtk_label_new(_("flickr user"));
  g_object_set(G_OBJECT(label), "xalign", 0.0, NULL);
  gtk_grid_attach(GTK_GRID(self->widget), label, 0, line++, 1, 1);

  hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_PIXEL_APPLY_DPI(8));

  ui->user_entry = GTK_ENTRY(gtk_entry_new());
  gtk_widget_set_hexpand(GTK_WIDGET(ui->user_entry), TRUE);
  dt_gui_key_accel_block_on_focus_connect(GTK_WIDGET(ui->user_entry));
  gtk_entry_set_text(ui->user_entry, _username == NULL ? "" : _username);
  g_signal_connect(G_OBJECT(ui->user_entry), "changed", G_CALLBACK(flickr_entry_changed), (gpointer)ui);
  gtk_entry_set_width_chars(GTK_ENTRY(ui->user_entry), 0);

  button = gtk_button_new_with_label(_("login"));
  g_object_set(G_OBJECT(button), "tooltip-text", _("flickr login"), (char *)NULL);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(flickr_button1_clicked), (gpointer)ui);
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(ui->user_entry), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(hbox), button, FALSE, FALSE, 0);

  gtk_grid_attach_next_to(GTK_GRID(self->widget), hbox, label, GTK_POS_RIGHT, 1, 1);


  ui->status_label = GTK_LABEL(gtk_label_new(NULL));
  gtk_widget_set_halign(GTK_WIDGET(ui->status_label), GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(self->widget), GTK_WIDGET(ui->status_label), 1, line++, 1, 1);


  ui->export_tags = dt_bauhaus_combobox_new(NULL);
  dt_bauhaus_widget_set_label(ui->export_tags, NULL, _("export tags"));
  dt_bauhaus_combobox_add(ui->export_tags, _("yes"));
  dt_bauhaus_combobox_add(ui->export_tags, _("no"));
  dt_bauhaus_combobox_set(ui->export_tags, 0);
  gtk_widget_set_hexpand(ui->export_tags, TRUE);
  gtk_grid_attach(GTK_GRID(self->widget), ui->export_tags, 0, line++, 2, 1);


  ui->permission_list = dt_bauhaus_combobox_new(NULL);
  dt_bauhaus_widget_set_label(ui->permission_list, NULL, _("visible to"));
  dt_bauhaus_combobox_add(ui->permission_list, _("you"));
  dt_bauhaus_combobox_add(ui->permission_list, _("friends"));
  dt_bauhaus_combobox_add(ui->permission_list, _("family"));
  dt_bauhaus_combobox_add(ui->permission_list, _("friends + family"));
  dt_bauhaus_combobox_add(ui->permission_list, _("everyone"));
  dt_bauhaus_combobox_set(ui->permission_list, 0); // Set default permission to private
  gtk_grid_attach(GTK_GRID(self->widget), GTK_WIDGET(ui->permission_list), 0, line++, 2, 1);


  hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_PIXEL_APPLY_DPI(5));

  ui->album_list = dt_bauhaus_combobox_new(NULL); // Available albums
  dt_bauhaus_widget_set_label(ui->album_list, NULL, _("photosets"));
  g_signal_connect(G_OBJECT(ui->album_list), "value-changed", G_CALLBACK(flickr_album_changed), (gpointer)ui);
  gtk_widget_set_sensitive(ui->album_list, FALSE);
  gtk_box_pack_start(GTK_BOX(hbox), ui->album_list, TRUE, TRUE, 0);

  button = dtgtk_button_new(dtgtk_cairo_paint_refresh, CPF_DO_NOT_USE_BORDER);
  g_object_set(G_OBJECT(button), "tooltip-text", _("refresh album list"), (char *)NULL);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(flickr_button1_clicked), (gpointer)ui);
  gtk_box_pack_start(GTK_BOX(hbox), button, FALSE, FALSE, 0);

  gtk_grid_attach(GTK_GRID(self->widget), hbox, 0, line++, 2, 1);


  // the box that gets shown when a new album is to be created
  ui->create_box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_PIXEL_APPLY_DPI(5)));
  gtk_widget_set_no_show_all(GTK_WIDGET(ui->create_box), TRUE);
  gtk_grid_attach(GTK_GRID(self->widget), GTK_WIDGET(ui->create_box), 0, line++, 2, 1);


  hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_PIXEL_APPLY_DPI(10));

  label = gtk_label_new(_("title"));
  g_object_set(G_OBJECT(label), "xalign", 0.0, NULL);
  gtk_box_pack_start(GTK_BOX(hbox), label, FALSE, FALSE, 0);

  ui->title_entry = GTK_ENTRY(gtk_entry_new()); // Album title
  dt_gui_key_accel_block_on_focus_connect(GTK_WIDGET(ui->title_entry));
  gtk_entry_set_text(ui->title_entry, _("my new photoset"));
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(ui->title_entry), TRUE, TRUE, 0);
  gtk_entry_set_width_chars(GTK_ENTRY(ui->title_entry), 0);

  gtk_box_pack_start(ui->create_box, hbox, FALSE, FALSE, 0);


  hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_PIXEL_APPLY_DPI(10));

  label = gtk_label_new(_("summary"));
  g_object_set(G_OBJECT(label), "xalign", 0.0, NULL);
  gtk_box_pack_start(GTK_BOX(hbox), label, FALSE, FALSE, 0);

  ui->summary_entry = GTK_ENTRY(gtk_entry_new()); // Album summary
  dt_gui_key_accel_block_on_focus_connect(GTK_WIDGET(ui->summary_entry));
  gtk_entry_set_text(ui->summary_entry, _("exported from darktable"));
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(ui->summary_entry), TRUE, TRUE, 0);
  gtk_entry_set_width_chars(GTK_ENTRY(ui->summary_entry), 0);

  gtk_box_pack_start(ui->create_box, hbox, TRUE, TRUE, 0);


  set_status(ui, _("click login button to start"), "#ffffff");

  /**
  don't populate the combo on startup, save 3 second

  // If username and password is stored, let's populate the combo
  if( _username && _password )
  {
    ui->user_token = _password;
    refresh_albums(ui);
  }
  */

  g_free(_username);
  dt_bauhaus_combobox_set(ui->album_list, 0);
}

void gui_cleanup(dt_imageio_module_storage_t *self)
{
  dt_storage_flickr_gui_data_t *ui = self->gui_data;
  dt_gui_key_accel_block_on_focus_disconnect(GTK_WIDGET(ui->user_entry));
  dt_gui_key_accel_block_on_focus_disconnect(GTK_WIDGET(ui->title_entry));
  dt_gui_key_accel_block_on_focus_disconnect(GTK_WIDGET(ui->summary_entry));
  g_free(self->gui_data);
}

void gui_reset(dt_imageio_module_storage_t *self)
{
}

int store(dt_imageio_module_storage_t *self, dt_imageio_module_data_t *sdata, const int imgid,
          dt_imageio_module_format_t *format, dt_imageio_module_data_t *fdata, const int num, const int total,
          const gboolean high_quality)
{
  gint result = 0;
  dt_storage_flickr_params_t *p = (dt_storage_flickr_params_t *)sdata;
  flickcurl_upload_status *photo_status;
  gint tags = 0;

  const char *ext = format->extension(fdata);

  // Let's upload image...

  /* construct a temporary file name */
  char fname[PATH_MAX] = { 0 };
  dt_loc_get_tmp_dir(fname, sizeof(fname));
  g_strlcat(fname, "/darktable.XXXXXX.", sizeof(fname));
  g_strlcat(fname, ext, sizeof(fname));

  char *caption = NULL;
  char *description = NULL;


  gint fd = g_mkstemp(fname);
  fprintf(stderr, "tempfile: %s\n", fname);
  if(fd == -1)
  {
    dt_control_log("failed to create temporary image for flickr export");
    return 1;
  }
  close(fd);
  const dt_image_t *img = dt_image_cache_get(darktable.image_cache, imgid, 'r');

  // If title is not existing, then use the filename without extension. If not, then use title instead
  GList *title = dt_metadata_get(img->id, "Xmp.dc.title", NULL);
  if(title != NULL)
  {
    caption = g_strdup(title->data);
    g_list_free_full(title, &g_free);
  }
  else
  {
    caption = g_path_get_basename(img->filename);
    (g_strrstr(caption, "."))[0] = '\0'; // chop extension...
  }

  GList *desc = dt_metadata_get(img->id, "Xmp.dc.description", NULL);
  if(desc != NULL)
  {
    description = desc->data;
  }
  dt_image_cache_read_release(darktable.image_cache, img);

  if(dt_imageio_export(imgid, fname, format, fdata, high_quality, FALSE, self, sdata, num, total) != 0)
  {
    fprintf(stderr, "[imageio_storage_flickr] could not export to file: `%s'!\n", fname);
    dt_control_log(_("could not export to file `%s'!"), fname);
    result = 1;
    goto cleanup;
  }

#ifdef _OPENMP
#pragma omp critical
#endif
  {
    // TODO: Check if this could be done in threads, so we enhance export time by using
    //      upload time for one image to export another image to disk.
    // Upload image
    // Do we export tags?
    if(p->export_tags == TRUE) tags = imgid;
    photo_status = _flickr_api_upload_photo(p, fname, caption, description, tags);
  }

  if(!photo_status)
  {
    fprintf(stderr, "[imageio_storage_flickr] could not upload to flickr!\n");
    dt_control_log(_("could not upload to flickr!"));
    result = 1;
    goto cleanup;
  }

  //  int fail = 0;
  // A photoset is only created if we have an album title set
  if(p->flickr_api->current_album == NULL && p->flickr_api->new_album == TRUE)
  {
    char *photoset_id;
    photoset_id = _flickr_api_create_photoset(p->flickr_api, photo_status->photoid);

    if(photoset_id == NULL)
    {
      dt_control_log("failed to create flickr album");
      //      fail = 1;
    }
    else
    {
      //      p->flickr_api->new_album = FALSE;
      p->flickr_api->current_album = flickcurl_photosets_getInfo(p->flickr_api->fc, photoset_id);
    }
  }

  //  if(fail) return 1;
  // TODO: What to do if photoset creation fails?

  // Add to gallery, if needed
  if(p->flickr_api->current_album != NULL && p->flickr_api->new_album != TRUE)
  {
    flickcurl_photosets_addPhoto(p->flickr_api->fc, p->flickr_api->current_album->id, photo_status->photoid);
    // TODO: Check for errors adding photo to gallery
  }
  else
  {
    if(p->flickr_api->current_album != NULL && p->flickr_api->new_album == TRUE)
    {
      p->flickr_api->new_album = FALSE;
    }
  }

cleanup:

  // And remove from filesystem..
  unlink(fname);
  g_free(caption);
  if(desc) g_list_free_full(desc, &g_free);

  if(!result)
  {
    // this makes sense only if the export was successful
    dt_control_log(_("%d/%d exported to flickr webalbum"), num, total);
  }
  return result;
}

size_t params_size(dt_imageio_module_storage_t *self)
{
  return sizeof(int64_t);
}

void init(dt_imageio_module_storage_t *self)
{
}

void *get_params(dt_imageio_module_storage_t *self)
{
  // have to return the size of the struct to store (i.e. without all the variable pointers at the end)
  // TODO: if a hash to encrypted data is stored here, return only this size and store it at the beginning of
  // the struct!
  dt_storage_flickr_gui_data_t *ui = (dt_storage_flickr_gui_data_t *)self->gui_data;
  dt_storage_flickr_params_t *d = (dt_storage_flickr_params_t *)g_malloc0(sizeof(dt_storage_flickr_params_t));
  if(!d) return NULL;
  if(!ui) return NULL; // gui not initialized, CLI mode
  d->hash = 1;

  // fill d from controls in ui
  if(ui->flickr_api && ui->flickr_api->needsReauthentication == FALSE)
  {
    // We are authenticated and off to actually export images..
    d->flickr_api = ui->flickr_api;
    int index = dt_bauhaus_combobox_get(ui->album_list);
    if(index >= 0)
    {
      switch(index)
      {
        case 0: // No album
          d->flickr_api->current_album = NULL;
          break;
        case 1: // Create new album
          d->flickr_api->current_album = NULL;
          d->flickr_api->album_title = g_strdup(gtk_entry_get_text(ui->title_entry));
          d->flickr_api->album_summary = g_strdup(gtk_entry_get_text(ui->summary_entry));
          d->flickr_api->new_album = TRUE;
          break;
        default:
          // use existing album
          d->flickr_api->current_album
              = flickcurl_photosets_getInfo(d->flickr_api->fc, ui->albums[index - 2]->id);
          if(d->flickr_api->current_album == NULL)
          {
            // Something went wrong...
            fprintf(stderr, "Something went wrong.. album index %d = NULL\n", index - 2);
            g_free(d);
            return NULL;
          }
          break;
      }
    }
    else
    {
      g_free(d);
      return NULL;
    }

    d->export_tags = (dt_bauhaus_combobox_get(ui->export_tags) == 0);

    /* Handle the permissions */
    int perm_index = (int)dt_bauhaus_combobox_get(ui->permission_list);
    switch(perm_index)
    {
      case 0: // Private
        d->public_perm = 0;
        d->friend_perm = 0;
        d->family_perm = 0;
        break;
      case 1: // Friends
        d->public_perm = 0;
        d->friend_perm = 1;
        d->family_perm = 0;
        break;
      case 2: // Family
        d->public_perm = 0;
        d->friend_perm = 0;
        d->family_perm = 1;
        break;
      case 3: // Friend + Family
        d->public_perm = 0;
        d->friend_perm = 1;
        d->family_perm = 1;
        break;
      case 4: // Public
        d->public_perm = 1;
        d->friend_perm = 0;
        d->family_perm = 0;
        break;
    }

    // Let UI forget about this api context and recreate a new one for further usage...
    ui->flickr_api = _flickr_api_authenticate(ui);
    if(ui->flickr_api)
    {
      set_status(ui, _("authenticated"), "#7fe07f");
    }
    else
    {
      set_status(ui, _("not authenticated"), "#e07f7f");
      gtk_widget_set_sensitive(GTK_WIDGET(ui->album_list), FALSE);
    }
  }
  else
  {
    set_status(ui, _("not authenticated"), "#e07f7f");
    gtk_widget_set_sensitive(GTK_WIDGET(ui->album_list), FALSE);
    g_free(d);
    return NULL;
  }
  return d;
}

int set_params(dt_imageio_module_storage_t *self, const void *params, const int size)
{
  if(size != self->params_size(self)) return 1;
  // gui stuff not updated, as sensitive user data is not stored in the preset.
  // TODO: store name/hash in kwallet/etc module and get encrypted stuff from there!
  return 0;
}

int supported(dt_imageio_module_storage_t *storage, dt_imageio_module_format_t *format)
{
  if(strcmp(format->mime(NULL), "image/jpeg") == 0)
    return 1;
  else if(strcmp(format->mime(NULL), "image/png") == 0)
    return 1;

  return 0;
}

void free_params(dt_imageio_module_storage_t *self, dt_imageio_module_data_t *params)
{
  dt_storage_flickr_params_t *d = (dt_storage_flickr_params_t *)params;

  _flickr_api_free(d->flickr_api); // TODO

  free(params);
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
