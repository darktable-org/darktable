/*
    This file is part of darktable,
    copyright (c) 2010 Jose Carlos Garcia Sogo

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
#include "dtgtk/label.h"
#include "gui/gtk.h"
#include "common/darktable.h"
#include "common/image.h"
#include "common/image_cache.h"
#include "common/imageio_module.h"
#include "common/imageio.h"
#include "common/tags.h"
#include "common/pwstorage/pwstorage.h"
#include "common/metadata.h"
#include "control/conf.h"
#include "control/control.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <curl/curl.h>
#include <flickcurl.h>
#include <libxml/parser.h>
#include <libxml/xpath.h>

DT_MODULE(1)

#define API_KEY "1d25b2dfcceba8c55fecb27645c968a3"
#define SHARED_SECRET "ac66b6c212be6f0c"

typedef struct _flickr_api_context_t
{
  flickcurl *fc;

  gboolean needsReauthentication;

  /** Current album used when posting images... */
  flickcurl_photoset  *current_album;

  char *album_title;
  char *album_summary;
  int album_public;
  gboolean new_album;
  gboolean error_occured;

} _flickr_api_context_t;

typedef struct dt_storage_flickr_gui_data_t
{

  GtkLabel *label1,*label2,*label3, *label4,*label5,*label6,*label7;                           // username, password, albums, status, albumtitle, albumsummary, albumrights
  GtkEntry *entry1,*entry2,*entry3,*entry4;                          // username, password, albumtitle,albumsummary
  GtkComboBox *comboBox1;                                 // album box
  GtkCheckButton *checkButton1,*checkButton2;                         // public album, export tags
  GtkDarktableButton *dtbutton1;                        // refresh albums
  GtkBox *hbox1;                                                    // Create album options...

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
  gboolean public_image;
} dt_storage_flickr_params_t;


/** Authenticates and retreives an initialized flickr api object */
_flickr_api_context_t static *_flickr_api_authenticate(dt_storage_flickr_gui_data_t *ui);

flickcurl_upload_status static *_flickr_api_upload_photo(dt_storage_flickr_params_t *params, char *data, char *caption, char *description,GList * tags );

void static _flickr_api_free( _flickr_api_context_t *ctx )
{

  g_free( ctx->album_title );
  g_free( ctx->album_summary );

  if (ctx->current_album != NULL)
    flickcurl_free_photoset (ctx->current_album);

  flickcurl_free (ctx->fc);

  g_free( ctx );
}

static void _flickr_api_error_handler(void *data, const char *message)
{
  dt_control_log(_("flickr authentication: %s"), message);
  if (data)
  {
    _flickr_api_context_t *ctx = (_flickr_api_context_t *)data;
    ctx->error_occured = 1;
  }
}

_flickr_api_context_t static *_flickr_api_authenticate(dt_storage_flickr_gui_data_t *ui)
{
  char *perms = NULL, *frob;
  gchar *token;
  char *flickr_user_token = NULL;
  gint result;
  _flickr_api_context_t *ctx = (_flickr_api_context_t *)g_malloc(sizeof(_flickr_api_context_t));
  memset(ctx,0,sizeof(_flickr_api_context_t));

  flickcurl_init ();
  ctx->fc = flickcurl_new ();
  flickcurl_set_api_key (ctx->fc, API_KEY);
  flickcurl_set_shared_secret (ctx->fc, SHARED_SECRET);
  flickcurl_set_error_handler(ctx->fc, _flickr_api_error_handler, ctx);

  if (!ui->user_token)
  {
    // Retrieve stored auth_key
    // TODO: We should be able to store token for different users
    GHashTable* table = dt_pwstorage_get("flickr");
    gchar * _username = g_strdup( g_hash_table_lookup(table, "username"));
    gchar *_user_token = g_strdup( g_hash_table_lookup(table, "token"));
    g_hash_table_destroy(table);

    if (_username)
    {
      if (!strcmp(_username,gtk_entry_get_text(ui->entry1)))
      {
        flickr_user_token = g_strdup(_user_token);
        perms = flickcurl_auth_checkToken(ctx->fc, flickr_user_token);
      }
      g_free (_username);
    }
    if (_user_token)
      g_free (_user_token);
  }
  else
  {
    flickr_user_token = ui->user_token;
    perms = flickcurl_auth_checkToken(ctx->fc, ui->user_token);
  }


  if (perms)
  {
    ui->user_token = flickr_user_token;
    flickcurl_set_auth_token(ctx->fc, flickr_user_token);
    return ctx;

  }
  else if (!ctx->error_occured)
  {
    frob = flickcurl_auth_getFrob(ctx->fc);
    GError *error = NULL;
    char *sign = g_strdup_printf ("%sapi_key%sfrob%spermswrite", SHARED_SECRET, API_KEY, frob);
    char *sign_md5 = g_compute_checksum_for_string (G_CHECKSUM_MD5, sign, strlen (sign));
    gchar auth_url[250];
    sprintf(auth_url,"http://flickr.com/services/auth/?api_key=%s&perms=write&frob=%s&api_sig=%s", API_KEY, frob, sign_md5);

    gtk_show_uri (gdk_screen_get_default(), auth_url, gtk_get_current_event_time (), &error);

    g_free(sign);
    g_free(sign_md5);

    // Hold here to let the user interact
    // Show a dialog.
    gchar *text1, *text2;
    text1 = g_strdup(_("step 1: a new window or tab of your browser should have been loaded. you have to login into your flickr account there and authorize darktable to upload photos before continuing."));
    text2 = g_strdup(_("step 2: click the ok button once you are done."));

    GtkWidget *window = darktable.gui->widgets.main_window;
    GtkWidget *flickr_auth_dialog = gtk_message_dialog_new (GTK_WINDOW (window),
                                    GTK_DIALOG_DESTROY_WITH_PARENT,
                                    GTK_MESSAGE_INFO,
                                    GTK_BUTTONS_OK_CANCEL,
                                    _("flickr authentication"));
    gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (flickr_auth_dialog),
        "%s\n\n%s", text1, text2);

    result = gtk_dialog_run (GTK_DIALOG (flickr_auth_dialog));

    gtk_widget_destroy(flickr_auth_dialog);

    g_free (text1);
    g_free (text2);

    switch (result)
    {
      case GTK_RESPONSE_OK:
        token = flickcurl_auth_getToken(ctx->fc, frob);
        g_free(frob);
        // TODO: Handle timeouts errors
        if (token)
        {
          flickr_user_token = g_strdup (token);
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
        gchar* username = g_strdup(gtk_entry_get_text(ui->entry1));

        g_hash_table_insert(table, "username", username);
        g_hash_table_insert(table, "token", flickr_user_token);

        if( !dt_pwstorage_set("flickr", table) )
        {
          dt_print(DT_DEBUG_PWSTORAGE,"[flickr] cannot store username/token\n");
        }

        g_free(flickr_user_token);
        g_hash_table_destroy(table);

        return ctx;
        break;

      default:
        dt_print(DT_DEBUG_PWSTORAGE,"[flickr] user cancelled the login process\n");
        return NULL;
    }


  }

  if (perms)
  {
    free(perms);
  }
  return NULL;
}


flickcurl_upload_status static *_flickr_api_upload_photo( dt_storage_flickr_params_t *p, char *fname, char *caption, char *description, GList * tags )
{

  flickcurl_upload_params *params = g_malloc(sizeof(flickcurl_upload_params));
  flickcurl_upload_status *status;

  memset(params,0,sizeof(flickcurl_upload_params));
  params->safety_level = 1; //Defaults to safe photos
  params->content_type = 1; //Defaults to photo (we don't support video!)

  params->title = caption;
  params->description = description;

  //TODO: handle tags with two or more words
  if (tags)
  {

    gchar **array;
    int i, length;

    length = g_list_length(tags);
    if (length > 1)
    {
      array = g_malloc(sizeof(gchar*)*(length+1));

      array[0] = "";

      for (i=1; i<length; i++)
      {
        dt_tag_t *t = g_list_nth_data(tags,i);
        if (t)
        {
          array[i] = g_strdup (t->tag);
        }

      }
      array[length] = NULL;
      params->tags = g_strjoinv(" ",array);
//TODO      g_strfreev(array);
    }
  }
  params->photo_file = fname; //fname should be the URI of temp file

  params->is_public = (int) p->public_image;

  status = flickcurl_photos_upload_params(p->flickr_api->fc, params);
  if (!status)
  {
    fprintf (stderr,"[flickr] Something went wrong when uploading");
    g_free (params);
    return NULL;
  }
  g_free(params);
  return status;
}


char static *_flickr_api_create_photoset(_flickr_api_context_t *ctx, const char *photo_id )
{
  char *photoset;
  const char *title = ctx->album_title;
  const char *summary = ctx->album_summary;

  photoset = flickcurl_photosets_create (ctx->fc, title, summary, photo_id, NULL);
  if (!photoset)
    fprintf(stderr,"[flickr] Something went wrong when creating gallery %s", title);
  return photoset;
}

const char*
name ()
{
  return _("flickr webalbum");
}

/** Set status connection text */
void static set_status(dt_storage_flickr_gui_data_t *ui, gchar *message, gchar *color)
{
  if( !color ) color="#ffffff";
  gchar mup[512]= {0};
  sprintf( mup,"<span foreground=\"%s\" ><small>%s</small></span>",color,message);
  gtk_label_set_markup(ui->label4, mup);
}

void static flickr_entry_changed(GtkEntry *entry, gpointer data)
{
  dt_storage_flickr_gui_data_t *ui=(dt_storage_flickr_gui_data_t *)data;

  if( ui->flickr_api != NULL)
  {
    ui->flickr_api->needsReauthentication=TRUE;
    if (ui->user_token)
    {
      g_free(ui->user_token);
      ui->user_token = NULL;
    }
    set_status(ui,_("not authenticated"), "#e07f7f");
    gtk_widget_set_sensitive(GTK_WIDGET( ui->comboBox1 ) ,FALSE);
  }
}

flickcurl_photoset static **_flickr_api_photosets( _flickr_api_context_t *ctx, const char *user)
{
  flickcurl_photoset **photoset;
//  char *nsid;

//TODO: Support both userid and email. As more services uses email as username
//      users can confise the needed id to be introduced in the user field.
//  nsid = flickcurl_people_findByEmail(ctx->fc, "@");

//  no need to specify nsid at all
//  nsid = flickcurl_people_findByUsername(ctx->fc, user);

// "If none is specified, the calling user is assumed (or NULL) "
// (c) http://librdf.org/flickcurl/api/flickcurl-section-photoset.html#flickcurl-photosets-getList
  photoset = flickcurl_photosets_getList(ctx->fc, NULL);

  return photoset;
}

/** Refresh albums */
void static refresh_albums(dt_storage_flickr_gui_data_t *ui)
{
  int i;
  gtk_widget_set_sensitive( GTK_WIDGET(ui->comboBox1), FALSE);

  if (ui->flickr_api == NULL || ui->flickr_api->needsReauthentication == TRUE)
  {
    if (ui->flickr_api != NULL) _flickr_api_free (ui->flickr_api);
    ui->flickr_api = _flickr_api_authenticate(ui);
    if (ui->flickr_api != NULL)
    {
      set_status(ui,_("authenticated"), "#7fe07f");
    }
    else
    {
      set_status(ui,_("not authenticated"), "#e07f7f");
      gtk_widget_set_sensitive(GTK_WIDGET( ui->comboBox1 ) ,FALSE);
      return;
    }
  }

  // First clear the model of data except first item (Create new album)
  GtkTreeModel *model=gtk_combo_box_get_model(ui->comboBox1);
  gtk_list_store_clear (GTK_LIST_STORE(model));

  ui->albums = _flickr_api_photosets(ui->flickr_api, gtk_entry_get_text(ui->entry1));
  if( ui->albums )
  {

    // Add standard action
    gtk_combo_box_append_text( ui->comboBox1, _("without album") );
    gtk_combo_box_append_text( ui->comboBox1, _("create new album") );
    gtk_combo_box_append_text( ui->comboBox1, "" );// Separator

    // Then add albums from list...
    for(i=0; ui->albums[i]; i++)
    {
      char data[512]= {0};
      sprintf(data,"%s (%i)", ui->albums[i]->title, ui->albums[i]->photos_count);
      gtk_combo_box_append_text( ui->comboBox1, g_strdup(data));
    }
    gtk_combo_box_set_active( ui->comboBox1, 3);
    gtk_widget_hide( GTK_WIDGET(ui->hbox1) ); // Hide create album box...
  }
  else
  {
    // Failed to parse feed of album...
    // Lets notify somehow...
    gtk_combo_box_set_active( ui->comboBox1, 0);
  }
  gtk_widget_set_sensitive( GTK_WIDGET(ui->comboBox1), TRUE);

}


void static flickr_album_changed(GtkComboBox *cb,gpointer data)
{
  dt_storage_flickr_gui_data_t * ui=(dt_storage_flickr_gui_data_t *)data;
  gchar *value=gtk_combo_box_get_active_text(ui->comboBox1);
  if( value!=NULL && strcmp( value, _("create new album") ) == 0 )
  {
    gtk_widget_set_no_show_all(GTK_WIDGET(ui->hbox1), FALSE);
    gtk_widget_show_all(GTK_WIDGET(ui->hbox1));
  }
  else
    gtk_widget_hide(GTK_WIDGET(ui->hbox1));
}

gboolean static combobox_separator(GtkTreeModel *model,GtkTreeIter *iter,gpointer data)
{
  GValue value = { 0, };
  gtk_tree_model_get_value(model,iter,0,&value);
  gchar *v=NULL;
  if (G_VALUE_HOLDS_STRING (&value))
  {
    if( (v=(gchar *)g_value_get_string (&value))!=NULL && strlen(v) == 0 ) return TRUE;
  }
  return FALSE;
}

// Refresh button pressed...
void static flickr_button1_clicked(GtkButton *button,gpointer data)
{
  dt_storage_flickr_gui_data_t * ui=(dt_storage_flickr_gui_data_t *)data;
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

void
gui_init (dt_imageio_module_storage_t *self)
{
  self->gui_data = (dt_storage_flickr_gui_data_t *)g_malloc(sizeof(dt_storage_flickr_gui_data_t));
  memset(self->gui_data,0,sizeof(dt_storage_flickr_gui_data_t));
  dt_storage_flickr_gui_data_t *ui= self->gui_data;
  self->widget = gtk_vbox_new(TRUE, 0);

  GtkWidget *hbox1=gtk_hbox_new(FALSE,5);
  GtkWidget *vbox1=gtk_vbox_new(FALSE,0);
  GtkWidget *vbox2=gtk_vbox_new(FALSE,0);

  ui->label1 = GTK_LABEL(  gtk_label_new( _("flickr user") ) );
//  ui->label2 = GTK_LABEL(  gtk_label_new( _("F_password") ) );
  ui->label3 = GTK_LABEL(  gtk_label_new( _("photosets") ) );
  ui->label4 = GTK_LABEL(  gtk_label_new( NULL ) );
  ui->label5 = GTK_LABEL(  gtk_label_new( _("title") ) );
  ui->label6 = GTK_LABEL(  gtk_label_new( _("summary") ) );
//  ui->label7 = GTK_LABEL(  gtk_label_new( _("F_rights") ) );
  gtk_misc_set_alignment(GTK_MISC(ui->label1), 0.0, 0.5);
//  gtk_misc_set_alignment(GTK_MISC(ui->label2), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(ui->label3), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(ui->label5), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(ui->label6), 0.0, 0.5);
//  gtk_misc_set_alignment(GTK_MISC(ui->label7), 0.0, 0.5);

  ui->entry1 = GTK_ENTRY( gtk_entry_new() );
//  ui->entry2 = GTK_ENTRY( gtk_entry_new() );
  ui->entry3 = GTK_ENTRY( gtk_entry_new() );  // Album title
  ui->entry4 = GTK_ENTRY( gtk_entry_new() );  // Album summary

  dt_gui_key_accel_block_on_focus (GTK_WIDGET (ui->entry1));
//  dt_gui_key_accel_block_on_focus (GTK_WIDGET (ui->entry2));
  dt_gui_key_accel_block_on_focus (GTK_WIDGET (ui->entry3));
  dt_gui_key_accel_block_on_focus (GTK_WIDGET (ui->entry4));

  /*
    gtk_widget_add_events(GTK_WIDGET(ui->entry1), GDK_FOCUS_CHANGE_MASK);
    g_signal_connect (G_OBJECT (ui->entry1), "focus-in-event",  G_CALLBACK(focus_in),  NULL);
    g_signal_connect (G_OBJECT (ui->entry1), "focus-out-event", G_CALLBACK(focus_out), NULL);

    gtk_widget_add_events(GTK_WIDGET(ui->entry2), GDK_FOCUS_CHANGE_MASK);
    g_signal_connect (G_OBJECT (ui->entry2), "focus-in-event",  G_CALLBACK(focus_in),  NULL);
    g_signal_connect (G_OBJECT (ui->entry2), "focus-out-event", G_CALLBACK(focus_out), NULL);
    gtk_widget_add_events(GTK_WIDGET(ui->entry3), GDK_FOCUS_CHANGE_MASK);
    g_signal_connect (G_OBJECT (ui->entry3), "focus-in-event",  G_CALLBACK(focus_in),  NULL);
    g_signal_connect (G_OBJECT (ui->entry3), "focus-out-event", G_CALLBACK(focus_out), NULL);
    gtk_widget_add_events(GTK_WIDGET(ui->entry4), GDK_FOCUS_CHANGE_MASK);
    g_signal_connect (G_OBJECT (ui->entry4), "focus-in-event",  G_CALLBACK(focus_in),  NULL);
    g_signal_connect (G_OBJECT (ui->entry4), "focus-out-event", G_CALLBACK(focus_out), NULL);
  */
  GHashTable* table = dt_pwstorage_get("flickr");
  gchar* _username = g_strdup( g_hash_table_lookup(table, "username"));
  //gchar* _password = g_strdup( g_hash_table_lookup(table, "token"));
  g_hash_table_destroy(table);
  gtk_entry_set_text( ui->entry1,  _username == NULL?"":_username );
//  gtk_entry_set_text( ui->entry2,  _password == NULL?"":_password );
  gtk_entry_set_text( ui->entry3, _("my new photoset") );
  gtk_entry_set_text( ui->entry4, _("exported from darktable") );

//  gtk_entry_set_visibility(ui->entry2, FALSE);

  GtkWidget *albumlist=gtk_hbox_new(FALSE,0);
  ui->comboBox1=GTK_COMBO_BOX( gtk_combo_box_new_text()); // Available albums

  GList *renderers = gtk_cell_layout_get_cells(GTK_CELL_LAYOUT(ui->comboBox1));
  GList *it = renderers;
  while(it)
  {
    GtkCellRendererText *tr = GTK_CELL_RENDERER_TEXT(it->data);
    g_object_set(G_OBJECT(tr), "ellipsize", PANGO_ELLIPSIZE_MIDDLE, (char *)NULL);
    it = g_list_next(it);
  }
  g_list_free(renderers);

  ui->dtbutton1 = DTGTK_BUTTON( dtgtk_button_new(dtgtk_cairo_paint_refresh,0) );
  g_object_set(G_OBJECT(ui->dtbutton1), "tooltip-text", _("refresh album list"), (char *)NULL);
  gtk_widget_set_sensitive( GTK_WIDGET(ui->comboBox1), FALSE);
  gtk_combo_box_set_row_separator_func(ui->comboBox1,combobox_separator,ui->comboBox1,NULL);
  gtk_box_pack_start(GTK_BOX(albumlist), GTK_WIDGET(ui->comboBox1), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(albumlist), GTK_WIDGET(ui->dtbutton1), FALSE, FALSE, 0);

  ui->checkButton1 = GTK_CHECK_BUTTON( gtk_check_button_new_with_label(_("public images")) );
  ui->checkButton2 = GTK_CHECK_BUTTON( gtk_check_button_new_with_label(_("export tags")) );
//  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON( ui->checkButton1 ),TRUE);
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON( ui->checkButton2 ),TRUE);
  // Auth
  gtk_box_pack_start(GTK_BOX(hbox1), vbox1, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(hbox1), vbox2, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), hbox1, TRUE, FALSE, 5);
  gtk_box_pack_start(GTK_BOX( vbox1 ), GTK_WIDGET( ui->label1 ), TRUE, TRUE, 0);
//  gtk_box_pack_start(GTK_BOX( vbox1 ), GTK_WIDGET( ui->label2 ), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX( vbox1 ), GTK_WIDGET( gtk_label_new("")), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX( vbox1 ), GTK_WIDGET( gtk_label_new("")), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX( vbox1 ), GTK_WIDGET( ui->label3 ), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX( vbox2 ), GTK_WIDGET( ui->entry1 ), TRUE, FALSE, 0);
//  gtk_box_pack_start(GTK_BOX( vbox2 ), GTK_WIDGET( ui->entry2 ), TRUE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX( vbox2 ), GTK_WIDGET( ui->label4 ), TRUE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX( vbox2 ), GTK_WIDGET( ui->checkButton1 ), TRUE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX( vbox2 ), GTK_WIDGET( ui->checkButton2 ), TRUE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX( vbox2 ), GTK_WIDGET( albumlist ), TRUE, FALSE, 0);


  // Create Album
  ui->hbox1=GTK_BOX(gtk_hbox_new(FALSE,5));
  gtk_widget_set_no_show_all(GTK_WIDGET(ui->hbox1), TRUE);
  vbox1=gtk_vbox_new(FALSE,0);
  vbox2=gtk_vbox_new(FALSE,0);

  gtk_box_pack_start(GTK_BOX(ui->hbox1), vbox1, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(ui->hbox1), vbox2, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(ui->hbox1), TRUE, FALSE, 5);
  gtk_box_pack_start(GTK_BOX( vbox1 ), GTK_WIDGET( ui->label5 ), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX( vbox1 ), GTK_WIDGET( ui->label6 ), TRUE, TRUE, 0);
//  gtk_box_pack_start(GTK_BOX( vbox1 ), GTK_WIDGET( ui->label7 ), TRUE, TRUE, 0);

  gtk_box_pack_start(GTK_BOX( vbox2 ), GTK_WIDGET( ui->entry3 ), TRUE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX( vbox2 ), GTK_WIDGET( ui->entry4 ), TRUE, FALSE, 0);

  // Setup signals
  // add signal on realize and hide gtk_widget_hide(GTK_WIDGET(ui->hbox1));

  g_signal_connect(G_OBJECT(ui->dtbutton1), "clicked", G_CALLBACK(flickr_button1_clicked), (gpointer)ui);
  g_signal_connect(G_OBJECT(ui->entry1), "changed", G_CALLBACK(flickr_entry_changed), (gpointer)ui);
//  g_signal_connect(G_OBJECT(ui->entry2), "changed", G_CALLBACK(flickr_entry_changed), (gpointer)ui);
  g_signal_connect(G_OBJECT(ui->comboBox1), "changed", G_CALLBACK(flickr_album_changed), (gpointer)ui);

  /**
 dont' populate the combo on startup, save 3 second

  // If username and password is stored, let's populate the combo
  if( _username && _password )
  {
    ui->user_token = _password;
    refresh_albums(ui);
  }
  */

  if( _username )
    g_free (_username);
  gtk_combo_box_set_active( ui->comboBox1, 0);
}

void
gui_cleanup (dt_imageio_module_storage_t *self)
{
}

void
gui_reset (dt_imageio_module_storage_t *self)
{
}

int
store (dt_imageio_module_data_t *sdata, const int imgid, dt_imageio_module_format_t *format, dt_imageio_module_data_t *fdata, const int num, const int total)
{
  int result=1;
  dt_storage_flickr_params_t *p=(dt_storage_flickr_params_t *)sdata;
  flickcurl_upload_status *photo_status;


  const char *ext = format->extension(fdata);

  // Let's upload image...

  /* construct a temporary file name */
  char fname[4096]= {0};
  dt_get_user_local_dir (fname,4096);
  g_strlcat (fname,"/tmp",4096);
  g_mkdir_with_parents(fname,0700);
  g_strlcat (fname,"/darktable.XXXXXX.",4096);
  g_strlcat(fname,ext,4096);

  char *caption = NULL;
  char *description = NULL;
  GList *tags = NULL;

  // Fetch the attached tags of image id if exported..
  if( p->export_tags == TRUE )
    dt_tag_get_attached(imgid,&tags);

  gint fd=g_mkstemp(fname);
  fprintf(stderr,"tempfile: %s\n",fname);
  if(fd==-1)
  {
    dt_control_log("failed to create temporary image for flickr export");
    return 1;
  }
  close(fd);
  dt_image_t *img = dt_image_cache_get(imgid, 'r');
  caption = g_path_get_basename( img->filename );

  (g_strrstr(caption,"."))[0]='\0'; // Shop extension...

  GList *desc = dt_metadata_get(img->id, "Xmp.dc.description", NULL);
  if(desc != NULL)
  {
    description = desc->data;
  }

  dt_imageio_export(img, fname, format, fdata);
  dt_image_cache_release(img, 'r');

#ifdef _OPENMP
  #pragma omp critical
#endif
//TODO: Check if this could be done in threads, so we enhace export time by using
//      upload time for one image to export another image to disk.
  // Upload image
  photo_status = _flickr_api_upload_photo( p, fname, caption, description, tags );
  if( !photo_status )
  {
    result=0;
    goto cleanup;
  }

//  int fail = 0;
  // A photoset is only created if we have an album title set
  if( p->flickr_api->current_album == NULL && p->flickr_api->new_album == TRUE)
  {
    char *photoset_id;
    photoset_id = _flickr_api_create_photoset(p->flickr_api, photo_status->photoid);

    if( photoset_id == NULL)
    {
      dt_control_log("failed to create flickr album");
//      fail = 1;
    }
    else
    {
//      p->flickr_api->new_album = FALSE;
      p->flickr_api->current_album = flickcurl_photosets_getInfo(p->flickr_api->fc,photoset_id);
    }
  }

//  if(fail) return 1;
// TODO: What to do if photset creation fails?

  // Add to gallery, if needed
  if (p->flickr_api->current_album != NULL && p->flickr_api->new_album != TRUE)
  {
    flickcurl_photosets_addPhoto (p->flickr_api->fc, p->flickr_api->current_album->id, photo_status->photoid);
    // TODO: Check for errors adding photo to gallery
  }
  else
  {
    if (p->flickr_api->current_album != NULL && p->flickr_api->new_album == TRUE)
    {
      p->flickr_api->new_album = FALSE;
    }
  }

cleanup:

  // And remove from filesystem..
  unlink( fname );
  g_free( caption );
  if(desc)
  {
    g_free(desc->data);
    g_list_free(desc);
  }

  if (result)
  {
    //this makes sense only if the export was successful
    dt_control_log(_("%d/%d exported to flickr webalbum"), num, total );
  }
  return result;
}

void*
get_params(dt_imageio_module_storage_t *self, int *size)
{
  // have to return the size of the struct to store (i.e. without all the variable pointers at the end)
  // TODO: if a hash to encrypted data is stored here, return only this size and store it at the beginning of the struct!
  *size = sizeof(int64_t);
  dt_storage_flickr_gui_data_t *ui =(dt_storage_flickr_gui_data_t *)self->gui_data;
  dt_storage_flickr_params_t *d = (dt_storage_flickr_params_t *)g_malloc(sizeof(dt_storage_flickr_params_t));
  if(!d) return NULL;
  memset(d,0,sizeof(dt_storage_flickr_params_t));
  d->hash = 1;

  // fill d from controls in ui
  if( ui->flickr_api && ui->flickr_api->needsReauthentication == FALSE)
  {
    // We are authenticated and off to actually export images..
    d->flickr_api = ui->flickr_api;
    int index = gtk_combo_box_get_active(ui->comboBox1);
    if( index >= 0 )
    {
      switch(index)
      {
        case 0: // No album
          d->flickr_api->current_album = NULL;
          break;
        case 1: // Create new album
          d->flickr_api->current_album = NULL;
          d->flickr_api->album_title = g_strdup( gtk_entry_get_text( ui->entry3 ) );
          d->flickr_api->album_summary = g_strdup( gtk_entry_get_text( ui->entry4) );
          d->flickr_api->new_album = TRUE;
          break;
        default:
          // use existing album
          d->flickr_api->current_album = flickcurl_photosets_getInfo(d->flickr_api->fc,ui->albums[index-3]->id);
          if( d->flickr_api->current_album == NULL )
          {
            // Something went wrong...
            fprintf(stderr,"Something went wrong.. album index %d = NULL\n",index-3 );
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

    d->public_image = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ui->checkButton1));
    d->export_tags = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ui->checkButton2));

    // Let UI forget about this api context and recreate a new one for further usage...
    ui->flickr_api = _flickr_api_authenticate(ui);
    if (ui->flickr_api)
    {
      set_status(ui,_("authenticated"), "#7fe07f");
    }
    else
    {
      set_status(ui,_("not authenticated"), "#e07f7f");
      gtk_widget_set_sensitive(GTK_WIDGET( ui->comboBox1 ) ,FALSE);
    }
  }
  else
  {
    set_status(ui,_("not authenticated"), "#e07f7f");
    gtk_widget_set_sensitive(GTK_WIDGET( ui->comboBox1 ) ,FALSE);
    dt_control_log(_("Flickr account not authenticated"));
    g_free(d);
    return NULL;
  }
  return d;
}

int
set_params(dt_imageio_module_format_t *self, void *params, int size)
{
  if(size != sizeof(int64_t)) return 1;
  // gui stuff not updated, as sensitive user data is not stored in the preset.
  // TODO: store name/hash in kwallet/etc module and get encrypted stuff from there!
  return 0;
}

int supported(dt_imageio_module_storage_t *storage, dt_imageio_module_format_t *format)
{
  if( strcmp(format->mime(NULL) ,"image/jpeg") ==  0 ) return 1;
  else if( strcmp(format->mime(NULL) ,"image/png") ==  0 ) return 1;

  return 0;
}

void
free_params(dt_imageio_module_storage_t *self, void *params)
{
  dt_storage_flickr_params_t *d = (dt_storage_flickr_params_t *)params;

  _flickr_api_free(  d->flickr_api ); //TODO

  free(params);
}

// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
