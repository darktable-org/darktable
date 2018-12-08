/*
    This file is part of darktable,
    copyright (c) 2012 Pierre Lamot
    copyright (c) 2015 tobias ellinghaus

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
#include "common/image.h"
#include "common/image_cache.h"
#include "common/imageio.h"
#include "common/imageio_module.h"
#include "common/metadata.h"
#include "common/pwstorage/pwstorage.h"
#include "common/tags.h"
#include "control/conf.h"
#include "control/control.h"
#include "dtgtk/button.h"
#include "gui/gtk.h"
#include "imageio/storage/imageio_storage_api.h"
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif
#include <curl/curl.h>
#include <json-glib/json-glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#ifdef HAVE_HTTP_SERVER
#include "common/http_server.h"
static const int port_pool[] = { 8123, 9123, 10123, 11123 };
static const int n_ports = sizeof(port_pool) / sizeof(port_pool[0]);
#endif

DT_MODULE(1)

#define FB_CALLBACK_ID "facebook"
#define FB_WS_BASE_URL "https://www.facebook.com/"
#define FB_GRAPH_BASE_URL "https://graph.facebook.com/v2.8/"
#define FB_API_KEY "315766121847254"

// facebook doesn't allow pictures bigger than FB_IMAGE_MAX_SIZExFB_IMAGE_MAX_SIZE px
#define FB_IMAGE_MAX_SIZE 2048

#define MSGCOLOR_RED "#e07f7f"
#define MSGCOLOR_GREEN "#7fe07f"

typedef enum ComboUserModel
{
  COMBO_USER_MODEL_NAME_COL = 0,
  COMBO_USER_MODEL_TOKEN_COL,
  COMBO_USER_MODEL_ID_COL,
  COMBO_USER_MODEL_NB_COL
} ComboUserModel;

typedef enum ComboAlbumModel
{
  COMBO_ALBUM_MODEL_NAME_COL = 0,
  COMBO_ALBUM_MODEL_ID_COL,
  COMBO_ALBUM_MODEL_NB_COL
} ComboAlbumModel;

typedef enum ComboPrivacyModel
{
  COMBO_PRIVACY_MODEL_NAME_COL = 0,
  COMBO_PRIVACY_MODEL_VAL_COL,
  COMBO_PRIVACY_MODEL_NB_COL
} ComboPrivacyModel;


/*
 * note: we don't support some kinds of privacy setting:
 *  CUSTOM : no plan to do this one for the moment
 *  NETWORKS_FRIENDS : this seems to be deprecated, it is currently impossible
 *      to create a new network (https://www.facebook.com/help/networks)
 */
typedef enum FBAlbumPrivacyPolicy
{
  FBALBUM_PRIVACY_EVERYONE,
  FBALBUM_PRIVACY_ALL_FRIENDS,
  FBALBUM_PRIVACY_NETWORKS_FRIENDS, // not implemented
  FBALBUM_PRIVACY_FRIENDS_OF_FRIENDS,
  FBALBUM_PRIVACY_SELF,
  FBALBUM_PRIVACY_CUSTOM // not implemented
} FBAlbumPrivacyPolicy;


/**
 * Represents information about an album
 */
typedef struct FBAlbum
{
  gchar *id;
  gchar *name;
  FBAlbumPrivacyPolicy privacy;
} FBAlbum;

static FBAlbum *fb_album_init()
{
  return (FBAlbum *)g_malloc0(sizeof(FBAlbum));
}

static void fb_album_destroy(FBAlbum *album)
{
  if(album == NULL) return;
  g_free(album->id);
  g_free(album->name);
  g_free(album);
}

/**
 * Represents information about an account
 */
typedef struct FBAccountInfo
{
  gchar *id;
  gchar *readablename;
  gchar *token;
} FBAccountInfo;

static FBAccountInfo *fb_account_info_init()
{
  return (FBAccountInfo *)g_malloc0(sizeof(FBAccountInfo));
}

static void fb_account_info_destroy(FBAccountInfo *account)
{
  if(account == NULL) return;
  g_free(account->id);
  g_free(account->readablename);
  g_free(account->token);
  g_free(account);
}

typedef struct FBContext
{
  /// curl context
  CURL *curl_ctx;
  /// Json parser context
  JsonParser *json_parser;

  GString *errmsg;

  /// authorization token
  gchar *token;

  gchar *album_id;
  char *album_title;
  char *album_summary;
  int album_permission;
  gboolean new_album;
} FBContext;

typedef struct dt_storage_facebook_gui_data_t
{
  // == ui elements ==
  GtkLabel *label_status;

  GtkComboBox *comboBox_username;
  GtkButton *button_login;

  GtkDarktableButton *dtbutton_refresh_album;
  GtkComboBox *comboBox_album;

  //  == album creation section ==
  GtkLabel *label_album_title;
  GtkLabel *label_album_summary;
  GtkLabel *label_album_privacy;

  GtkEntry *entry_album_title;
  GtkEntry *entry_album_summary;
  GtkComboBox *comboBox_privacy;

  GtkBox *hbox_album;

  // == context ==
  gboolean connected;
  FBContext *facebook_api;

  // == authentication dialog ==
  GtkMessageDialog *auth_dialog;
} dt_storage_facebook_gui_data_t;


static FBContext *fb_api_init()
{
  FBContext *ctx = (FBContext *)g_malloc0(sizeof(FBContext));
  ctx->curl_ctx = curl_easy_init();
  ctx->errmsg = g_string_new("");
  ctx->json_parser = json_parser_new();
  return ctx;
}


static void fb_api_destroy(FBContext *ctx)
{
  if(ctx == NULL) return;
  curl_easy_cleanup(ctx->curl_ctx);
  g_free(ctx->token);
  g_object_unref(ctx->json_parser);
  g_string_free(ctx->errmsg, TRUE);
  g_free(ctx);
}


typedef struct dt_storage_facebook_param_t
{
  gint64 hash;
  FBContext *facebook_ctx;
} dt_storage_facebook_param_t;


//////////////////////////// curl requests related functions

/**
 * extract the user token from the callback @a url
 */
static gchar *fb_extract_token_from_url(const gchar *url)
{
  g_return_val_if_fail((url != NULL), NULL);
  if(!(g_str_has_prefix(url, "http://localhost:8123/" FB_CALLBACK_ID) == TRUE)) return NULL;

  char *authtoken = NULL;

  char **urlchunks = g_strsplit_set(url, "#&=", -1);
  // starts at 1 to skip the url prefix, then values are in the form key=value
  for(int i = 1; urlchunks[i] != NULL; i += 2)
  {
    if((g_strcmp0(urlchunks[i], "access_token") == 0) && (urlchunks[i + 1] != NULL))
    {
      authtoken = g_strdup(urlchunks[i + 1]);
      break;
    }
    else if(g_strcmp0(urlchunks[i], "error") == 0)
    {
      break;
    }
    if(urlchunks[i + 1] == NULL) // this shouldn't happens but we never know...
    {
      g_printerr(_("[facebook] unexpected URL format\n"));
      break;
    }
  }
  g_strfreev(urlchunks);
  return authtoken;
}

static size_t curl_write_data_cb(void *ptr, size_t size, size_t nmemb, void *data)
{
  GString *string = (GString *)data;
  g_string_append_len(string, ptr, size * nmemb);
#ifdef FACEBOOK_EXTRA_VERBOSE
  g_printf("server reply: %s\n", string->str);
#endif
  return size * nmemb;
}

static JsonObject *fb_parse_response(FBContext *ctx, GString *response)
{
  GError *error;
  const gboolean ret = json_parser_load_from_data(ctx->json_parser, response->str, response->len, &error);
  g_return_val_if_fail((ret), NULL);

  JsonNode *root = json_parser_get_root(ctx->json_parser);
  // we should always have a dict
  g_return_val_if_fail((json_node_get_node_type(root) == JSON_NODE_OBJECT), NULL);

  JsonObject *rootdict = json_node_get_object(root);
  if(json_object_has_member(rootdict, "error"))
  {
    JsonObject *errorstruct = json_object_get_object_member(rootdict, "error");
    g_return_val_if_fail((errorstruct != NULL), NULL);
    const gchar *errormessage = json_object_get_string_member(errorstruct, "message");
    g_return_val_if_fail((errormessage != NULL), NULL);
    g_string_assign(ctx->errmsg, errormessage);
    return NULL;
  }

  return rootdict;
}


static void fb_query_get_add_url_arguments(const gchar *key, const gchar *value, GString *url)
{
  g_string_append(url, "&");
  g_string_append(url, key);
  g_string_append(url, "=");
  g_string_append(url, value);
}

/**
 * perform a GET request on facebook graph api
 *
 * @note use this one to read information (user info, existing albums, ...)
 *
 * @param ctx facebook context (token field must be set)
 * @param method the method to call on the facebook graph API, the methods should not start with '/' example:
 *"me/albums"
 * @param args hashtable of the arguments to be added to the requests, must be in the form key (string) =
 *value (string)
 * @returns NULL if the request fails, or a JsonObject of the reply
 */
static JsonObject *fb_query_get(FBContext *ctx, const gchar *method, GHashTable *args)
{
  g_return_val_if_fail(ctx != NULL, NULL);
  g_return_val_if_fail(ctx->token != NULL, NULL);
  // build the query
  GString *url = g_string_new(FB_GRAPH_BASE_URL);
  g_string_append(url, method);
  g_string_append(url, "?access_token=");
  g_string_append(url, ctx->token);
  if(args != NULL) g_hash_table_foreach(args, (GHFunc)fb_query_get_add_url_arguments, url);

  // send the request
  GString *response = g_string_new("");
  curl_easy_reset(ctx->curl_ctx);
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_URL, url->str);
#ifdef FACEBOOK_EXTRA_VERBOSE
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_VERBOSE, 2);
#endif
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_WRITEFUNCTION, curl_write_data_cb);
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_SSL_VERIFYPEER, FALSE);
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_WRITEDATA, response);
  int res = curl_easy_perform(ctx->curl_ctx);

  if(res != CURLE_OK)
  {
    g_string_free(url, TRUE);
    g_string_free(response, TRUE);
    return NULL;
  }

  // parse the response
  JsonObject *respobj = fb_parse_response(ctx, response);

  g_string_free(response, TRUE);
  g_string_free(url, TRUE);
  return respobj;
}

typedef struct
{
  struct curl_httppost *formpost;
  struct curl_httppost *lastptr;
} HttppostFormList;

static void fb_query_post_add_form_arguments(const gchar *key, const gchar *value, HttppostFormList *formlist)
{
  curl_formadd(&(formlist->formpost), &(formlist->lastptr), CURLFORM_COPYNAME, key, CURLFORM_COPYCONTENTS,
               value, CURLFORM_END);
}

static void fb_query_post_add_file_arguments(const gchar *key, const gchar *value, HttppostFormList *formlist)
{
  curl_formadd(&(formlist->formpost), &(formlist->lastptr), CURLFORM_COPYNAME, key, CURLFORM_COPYCONTENTS,
               value, CURLFORM_END);

  curl_formadd(&(formlist->formpost), &(formlist->lastptr), CURLFORM_COPYNAME, key, CURLFORM_FILE, value,
               CURLFORM_END);
}

/**
 * perform a POST request on facebook graph api
 *
 * @note use this one to create object (album, photos, ...)
 *
 * @param ctx facebook context (token field must be set)
 * @param method the method to call on the facebook graph API, the methods should not start with '/' example:
 *"me/albums"
 * @param args hashtable of the arguments to be added to the requests, might be null if none
 * @param files hashtable of the files to be send with the requests, might be null if none
 * @returns NULL if the request fails, or a JsonObject of the reply
 */
static JsonObject *fb_query_post(FBContext *ctx, const gchar *method, GHashTable *args, GHashTable *files)
{
  g_return_val_if_fail(ctx != NULL, NULL);
  g_return_val_if_fail(ctx->token != NULL, NULL);

  GString *url = g_string_new(FB_GRAPH_BASE_URL);
  g_string_append(url, method);

  HttppostFormList formlist;
  formlist.formpost = NULL;
  formlist.lastptr = NULL;

  curl_formadd(&(formlist.formpost), &(formlist.lastptr), CURLFORM_COPYNAME, "access_token",
               CURLFORM_COPYCONTENTS, ctx->token, CURLFORM_END);
  if(args != NULL) g_hash_table_foreach(args, (GHFunc)fb_query_post_add_form_arguments, &formlist);

  if(files != NULL) g_hash_table_foreach(files, (GHFunc)fb_query_post_add_file_arguments, &formlist);

  // send the requests
  GString *response = g_string_new("");
  curl_easy_reset(ctx->curl_ctx);
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_URL, url->str);
#ifdef FACEBOOK_EXTRA_VERBOSE
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_VERBOSE, 2);
#endif
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_HTTPPOST, formlist.formpost);
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_SSL_VERIFYPEER, FALSE);
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_WRITEFUNCTION, curl_write_data_cb);
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_WRITEDATA, response);
  int res = curl_easy_perform(ctx->curl_ctx);
  curl_formfree(formlist.formpost);
  g_string_free(url, TRUE);
  if(res != CURLE_OK) return NULL;
  // parse the response
  JsonObject *respobj = fb_parse_response(ctx, response);
  g_string_free(response, TRUE);
  return respobj;
}

//////////////////////////// facebook api functions

/**
 * @returns TRUE if the current token is valid
 */
static gboolean fb_test_auth_token(FBContext *ctx)
{
  JsonObject *obj = fb_query_get(ctx, "me", NULL);
  return obj != NULL;
}

/**
 * @return a GList of FBAlbums associated to the user
 */
static GList *fb_get_album_list(FBContext *ctx, gboolean *ok)
{
  if(!ok) return NULL;

  *ok = TRUE;
  GList *album_list = NULL;

  GHashTable *args = g_hash_table_new((GHashFunc)g_str_hash, (GEqualFunc)g_str_equal);
  g_hash_table_insert(args, "fields", "id,name,can_upload");
  JsonObject *reply = fb_query_get(ctx, "me/albums", args);
  g_hash_table_destroy(args);

  if(reply == NULL) goto error;

  JsonArray *jsalbums = json_object_get_array_member(reply, "data");
  if(jsalbums == NULL) goto error;

  guint i;
  for(i = 0; i < json_array_get_length(jsalbums); i++)
  {
    JsonObject *obj = json_array_get_object_element(jsalbums, i);
    if(obj == NULL) continue;

    JsonNode *canupload_node = json_object_get_member(obj, "can_upload");
    if(canupload_node == NULL || !json_node_get_boolean(canupload_node)) continue;

    FBAlbum *album = fb_album_init();
    if(album == NULL) goto error;

    const char *id = json_object_get_string_member(obj, "id");
    const char *name = json_object_get_string_member(obj, "name");
    if(id == NULL || name == NULL)
    {
      fb_album_destroy(album);
      goto error;
    }
    album->id = g_strdup(id);
    album->name = g_strdup(name);
    album_list = g_list_append(album_list, album);
  }
  return album_list;

error:
  *ok = FALSE;
  g_list_free_full(album_list, (GDestroyNotify)fb_album_destroy);
  return NULL;
}

/**
 * @see https://developers.facebook.com/docs/reference/api/user/
 * @return the id of the newly reacted
 */
static const gchar *fb_create_album(FBContext *ctx, gchar *name, gchar *summary, FBAlbumPrivacyPolicy privacy)
{
  GHashTable *args = g_hash_table_new((GHashFunc)g_str_hash, (GEqualFunc)g_str_equal);
  g_hash_table_insert(args, "name", name);
  if(summary != NULL) g_hash_table_insert(args, "message", summary);
  switch(privacy)
  {
    case FBALBUM_PRIVACY_EVERYONE:
      g_hash_table_insert(args, "privacy", "{\"value\":\"EVERYONE\"}");
      break;
    case FBALBUM_PRIVACY_ALL_FRIENDS:
      g_hash_table_insert(args, "privacy", "{\"value\":\"ALL_FRIENDS\"}");
      break;
    case FBALBUM_PRIVACY_FRIENDS_OF_FRIENDS:
      g_hash_table_insert(args, "privacy", "{\"value\":\"FRIENDS_OF_FRIENDS\"}");
      break;
    case FBALBUM_PRIVACY_SELF:
      g_hash_table_insert(args, "privacy", "{\"value\":\"SELF\"}");
      break;
    default:
      goto error;
      break;
  }
  JsonObject *ref = fb_query_post(ctx, "me/albums", args, NULL);
  if(ref == NULL) goto error;
  g_hash_table_destroy(args);
  return json_object_get_string_member(ref, "id");

error:
  g_hash_table_destroy(args);
  return NULL;
}

/**
 * @see https://developers.facebook.com/docs/reference/api/album/
 * @return the id of the uploaded photo
 */
static const gchar *fb_upload_photo_to_album(FBContext *ctx, gchar *albumid, gchar *fpath, gchar *description)
{
  GString *method = g_string_new(albumid);
  g_string_append(method, "/photos");

  GHashTable *files = g_hash_table_new((GHashFunc)g_str_hash, (GEqualFunc)g_str_equal);
  g_hash_table_insert(files, "source", fpath);

  GHashTable *args = NULL;
  if(description != NULL)
  {
    args = g_hash_table_new((GHashFunc)g_str_hash, (GEqualFunc)g_str_equal);
    g_hash_table_insert(args, "message", description);
  }

  JsonObject *ref = fb_query_post(ctx, method->str, args, files);
  g_string_free(method, TRUE);

  g_hash_table_destroy(files);
  if(args != NULL)
  {
    g_hash_table_destroy(args);
  }
  if(ref == NULL) return NULL;
  return json_object_get_string_member(ref, "id");
}

/**
 * @see https://developers.facebook.com/docs/reference/api/user/
 * @return basic information about the account
 */
static FBAccountInfo *fb_get_account_info(FBContext *ctx)
{
  JsonObject *obj = fb_query_get(ctx, "me", NULL);
  g_return_val_if_fail((obj != NULL), NULL);
  const gchar *readablename = json_object_get_string_member(obj, "name");
  const gchar *user_id = json_object_get_string_member(obj, "id");
  g_return_val_if_fail(readablename != NULL && user_id != NULL, NULL);
  FBAccountInfo *accountinfo = fb_account_info_init();
  accountinfo->id = g_strdup(user_id);
  accountinfo->readablename = g_strdup(readablename);
  accountinfo->token = g_strdup(ctx->token);
  return accountinfo;
}


///////////////////////////////// UI functions

static gboolean combobox_separator(GtkTreeModel *model, GtkTreeIter *iter, gpointer data)
{
  GValue value = {
    0,
  };
  gtk_tree_model_get_value(model, iter, 0, &value);
  gchar *v = NULL;
  if(G_VALUE_HOLDS_STRING(&value))
  {
    if((v = (gchar *)g_value_get_string(&value)) != NULL && *v == '\0') return TRUE;
  }
  g_value_unset(&value);
  return FALSE;
}

/**
 * @see https://developers.facebook.com/docs/authentication/
 * @returns NULL if the user cancels the operation or a valid token
 */
static gboolean _open_browser(const char *callback_url)
{
  GError *error = NULL;
  char *url = g_strdup_printf(FB_WS_BASE_URL "dialog/oauth?"
                                             "client_id=" FB_API_KEY "&redirect_uri=%s"
                                             "&scope=user_photos,publish_actions"
                                             "&response_type=token",
                              callback_url);
  if(!gtk_show_uri(gdk_screen_get_default(), url, gtk_get_current_event_time(), &error))
  {
    fprintf(stderr, "[facebook] error opening browser: %s\n", error->message);
    g_error_free(error);
    g_free(url);
    return FALSE;
  }
  g_free(url);
  return TRUE;
}

static gchar *facebook_get_user_auth_token_from_url(dt_storage_facebook_gui_data_t *ui)
{
  ///////////// open the authentication url in a browser. just use some port, we won't listen anyway
  if(!_open_browser("http://localhost:8123/" FB_CALLBACK_ID)) return NULL;

  ////////////// build & show the validation dialog
  gchar *text1 = _("step 1: a new window or tab of your browser should have been "
                   "loaded. you have to login into your facebook account there "
                   "and authorize darktable to upload photos before continuing.");
  gchar *text2 = _("step 2: paste your browser URL and click the OK button once "
                   "you are done.");

  GtkWidget *window = dt_ui_main_window(darktable.gui->ui);
  GtkDialog *fb_auth_dialog = GTK_DIALOG(
      gtk_message_dialog_new(GTK_WINDOW(window), GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_QUESTION,
                             GTK_BUTTONS_OK_CANCEL, _("facebook authentication")));
#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(GTK_WIDGET(fb_auth_dialog));
#endif
  gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(fb_auth_dialog), "%s\n\n%s", text1, text2);

  GtkWidget *entry = gtk_entry_new();
  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(gtk_label_new(_("URL:"))), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(entry), TRUE, TRUE, 0);

  GtkWidget *fbauthdialog_vbox = gtk_message_dialog_get_message_area(GTK_MESSAGE_DIALOG(fb_auth_dialog));
  gtk_box_pack_end(GTK_BOX(fbauthdialog_vbox), hbox, TRUE, TRUE, 0);

  gtk_widget_show_all(GTK_WIDGET(fb_auth_dialog));

  ////////////// wait for the user to enter the validation URL
  gint result;
  gchar *token = NULL;
  const char *replyurl;
  while(TRUE)
  {
    result = gtk_dialog_run(GTK_DIALOG(fb_auth_dialog));
    if(result == GTK_RESPONSE_CANCEL) break;
    replyurl = gtk_entry_get_text(GTK_ENTRY(entry));
    if(replyurl == NULL || g_strcmp0(replyurl, "") == 0)
    {
      gtk_message_dialog_format_secondary_markup(GTK_MESSAGE_DIALOG(fb_auth_dialog),
                                                 "%s\n\n%s\n\n<span foreground=\"" MSGCOLOR_RED
                                                 "\" ><small>%s</small></span>",
                                                 text1, text2, _("please enter the validation URL"));
      continue;
    }
    token = fb_extract_token_from_url(replyurl);
    if(token != NULL) // we have a valid token
      break;
    else
      gtk_message_dialog_format_secondary_markup(
          GTK_MESSAGE_DIALOG(fb_auth_dialog),
          "%s\n\n%s%s\n\n<span foreground=\"" MSGCOLOR_RED "\"><small>%s</small></span>", text1, text2,
          _("the given URL is not valid, it should look like: "),
          FB_WS_BASE_URL "connect/login_success.html?...");
  }
  gtk_widget_destroy(GTK_WIDGET(fb_auth_dialog));

  return token;
}

#ifdef HAVE_HTTP_SERVER
static void ui_authenticate_finish(dt_storage_facebook_gui_data_t *ui, gboolean mustsaveaccount);

static gboolean _server_callback(GHashTable *query, gpointer user_data)
{
  dt_storage_facebook_gui_data_t *ui = (dt_storage_facebook_gui_data_t *)user_data;

  const char *access_token = g_hash_table_lookup(query, "access_token");

  if(access_token)
  {
    // we got what we wanted
    dt_print(DT_DEBUG_CONTROL, "[facebook] got access_token `%s' from facebook redirect\n", access_token);

    // close the dialog
    gtk_widget_destroy(GTK_WIDGET(ui->auth_dialog));
    ui->auth_dialog = NULL;

    FBContext *ctx = ui->facebook_api;
    ctx->token = g_strdup(access_token);

    ui_authenticate_finish(ui, TRUE);

    dt_control_log(_("authentication successful"));
    return TRUE;
  }

  dt_control_log(_("authentication failed"));

  return FALSE;
}


static gboolean facebook_get_user_auth_token_from_server(dt_storage_facebook_gui_data_t *ui)
{
  // create a dialog telling the user to login in the browser
  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);

  GtkWidget *dialog = gtk_message_dialog_new(
      GTK_WINDOW(win), GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_INFO, GTK_BUTTONS_CANCEL,
      _("a new window or tab of your browser should have been "
        "loaded. you have to login into your facebook account there "
        "and authorize darktable to upload photos before continuing."));
#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(dialog);
#endif

  gtk_window_set_title(GTK_WINDOW(dialog), _("facebook authentication"));

  ui->auth_dialog = GTK_MESSAGE_DIALOG(dialog);

  // create an http server
  dt_http_server_t *server = dt_http_server_create(port_pool, n_ports, "facebook", _server_callback, ui);
  if(!server)
  {
    gtk_widget_destroy(dialog);
    return FALSE;
  }

  // open the browser
  if(!_open_browser(server->url))
  {
    gtk_widget_destroy(dialog);
    dt_http_server_kill(server);
    return FALSE;
  }

  // show the window
  if(gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_CANCEL)
  {
    // if cancel button is clicked -> kill the server
    dt_http_server_kill(server);
    gtk_widget_destroy(dialog);
  }

  return TRUE;
}
#endif // HAVE_HTTP_SERVER

static void load_account_info_fill(gchar *key, gchar *value, GSList **accountlist)
{
  JsonParser *parser = json_parser_new();
  json_parser_load_from_data(parser, value, strlen(value), NULL);
  JsonNode *root = json_parser_get_root(parser);

  // root can be null while parsing the account info
  if(root)
  {
    JsonObject *obj = json_node_get_object(root);
    FBAccountInfo *info = fb_account_info_init();
    info->id = g_strdup(key);
    info->token = g_strdup(json_object_get_string_member(obj, "token"));
    info->readablename = g_strdup(json_object_get_string_member(obj, "name"));
    *accountlist = g_slist_prepend(*accountlist, info);
  }
  g_object_unref(parser);
}

/**
 * @return a GSList of saved FBAccountInfo
 */
static GSList *load_account_info()
{
  GSList *accountlist = NULL;

  GHashTable *table = dt_pwstorage_get("facebook");
  g_hash_table_foreach(table, (GHFunc)load_account_info_fill, &accountlist);
  g_hash_table_destroy(table);
  return accountlist;
}

static void save_account_info(dt_storage_facebook_gui_data_t *ui, FBAccountInfo *accountinfo)
{
  FBContext *ctx = ui->facebook_api;
  g_return_if_fail(ctx != NULL);

  /// serialize data;
  JsonBuilder *builder = json_builder_new();
  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "name");
  json_builder_add_string_value(builder, accountinfo->readablename);
  json_builder_set_member_name(builder, "token");
  json_builder_add_string_value(builder, accountinfo->token);
  json_builder_end_object(builder);

  JsonNode *node = json_builder_get_root(builder);
  JsonGenerator *generator = json_generator_new();
  json_generator_set_root(generator, node);
#if JSON_CHECK_VERSION(0, 14, 0)
  json_generator_set_pretty(generator, FALSE);
#endif
  gchar *data = json_generator_to_data(generator, NULL);

  json_node_free(node);
  g_object_unref(generator);
  g_object_unref(builder);

  GHashTable *table = dt_pwstorage_get("facebook");
  g_hash_table_insert(table, g_strdup(accountinfo->id), data);
  dt_pwstorage_set("facebook", table);

  g_hash_table_destroy(table);
}

static void remove_account_info(const gchar *accountid)
{
  GHashTable *table = dt_pwstorage_get("facebook");
  g_hash_table_remove(table, accountid);
  dt_pwstorage_set("facebook", table);
  g_hash_table_destroy(table);
}

static void ui_refresh_users_fill(FBAccountInfo *value, gpointer dataptr)
{
  GtkListStore *liststore = GTK_LIST_STORE(dataptr);
  GtkTreeIter iter;
  gtk_list_store_append(liststore, &iter);
  gtk_list_store_set(liststore, &iter, COMBO_USER_MODEL_NAME_COL, value->readablename, COMBO_USER_MODEL_TOKEN_COL,
                     value->token, COMBO_USER_MODEL_ID_COL, value->id, -1);
}

static void ui_refresh_users(dt_storage_facebook_gui_data_t *ui)
{
  GSList *accountlist = load_account_info();
  GtkListStore *list_store = GTK_LIST_STORE(gtk_combo_box_get_model(ui->comboBox_username));
  GtkTreeIter iter;

  gtk_list_store_clear(list_store);
  gtk_list_store_append(list_store, &iter);

  int active_account = 0;
  if(g_slist_length(accountlist) == 0)
  {
    gtk_list_store_set(list_store, &iter, COMBO_USER_MODEL_NAME_COL, _("new account"),
                       COMBO_USER_MODEL_TOKEN_COL, NULL, COMBO_USER_MODEL_ID_COL, NULL, -1);
  }
  else
  {
    gtk_list_store_set(list_store, &iter, COMBO_USER_MODEL_NAME_COL, _("other account"),
                       COMBO_USER_MODEL_TOKEN_COL, NULL, COMBO_USER_MODEL_ID_COL, NULL, -1);
    gtk_list_store_append(list_store, &iter);
    gtk_list_store_set(list_store, &iter, COMBO_USER_MODEL_NAME_COL, "", COMBO_USER_MODEL_TOKEN_COL, NULL,
                       COMBO_USER_MODEL_ID_COL, NULL, -1); // separator
    active_account = 2;
  }

  g_slist_foreach(accountlist, (GFunc)ui_refresh_users_fill, list_store);
  gtk_combo_box_set_active(ui->comboBox_username, active_account);

  g_slist_free_full(accountlist, (GDestroyNotify)fb_account_info_destroy);
  gtk_combo_box_set_row_separator_func(ui->comboBox_username, combobox_separator, ui->comboBox_username, NULL);
}

static void ui_refresh_albums_fill(FBAlbum *album, GtkListStore *list_store)
{
  GtkTreeIter iter;
  gtk_list_store_append(list_store, &iter);
  gtk_list_store_set(list_store, &iter, COMBO_ALBUM_MODEL_NAME_COL, album->name, COMBO_ALBUM_MODEL_ID_COL,
                     album->id, -1);
}

static void ui_refresh_albums(dt_storage_facebook_gui_data_t *ui)
{
  gboolean getlistok;
  GList *albumList = fb_get_album_list(ui->facebook_api, &getlistok);
  if(!getlistok)
  {
    dt_control_log(_("unable to retrieve the album list"));
    goto cleanup;
  }

  GtkListStore *model_album = GTK_LIST_STORE(gtk_combo_box_get_model(ui->comboBox_album));
  GtkTreeIter iter;
  gtk_list_store_clear(model_album);
  gtk_list_store_append(model_album, &iter);
  gtk_list_store_set(model_album, &iter, COMBO_ALBUM_MODEL_NAME_COL, _("create new album"),
                     COMBO_ALBUM_MODEL_ID_COL, NULL, -1);
  if(albumList != NULL)
  {
    gtk_list_store_append(model_album, &iter);
    gtk_list_store_set(model_album, &iter, COMBO_ALBUM_MODEL_NAME_COL, "", COMBO_ALBUM_MODEL_ID_COL, NULL,
                       -1); // separator
  }
  g_list_foreach(albumList, (GFunc)ui_refresh_albums_fill, model_album);

  if(albumList != NULL)
    gtk_combo_box_set_active(ui->comboBox_album, 2);
  else
    gtk_combo_box_set_active(ui->comboBox_album, 0);

  gtk_widget_show_all(GTK_WIDGET(ui->comboBox_album));
  g_list_free_full(albumList, (GDestroyNotify)fb_album_destroy);

cleanup:
  return;
}

static void ui_authenticate_finish(dt_storage_facebook_gui_data_t *ui, gboolean mustsaveaccount)
{
  FBContext *ctx = ui->facebook_api;

  if(ctx->token == NULL) goto error;

  if(mustsaveaccount)
  {
    FBAccountInfo *accountinfo = fb_get_account_info(ui->facebook_api);
    if(accountinfo == NULL) goto error;
    save_account_info(ui, accountinfo);

    // add account to user list and select it
    GtkListStore *model = GTK_LIST_STORE(gtk_combo_box_get_model(ui->comboBox_username));
    GtkTreeIter iter;
    gboolean r;
    gchar *uid;

    gboolean updated = FALSE;

    for(r = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(model), &iter); r == TRUE;
        r = gtk_tree_model_iter_next(GTK_TREE_MODEL(model), &iter))
    {
      gtk_tree_model_get(GTK_TREE_MODEL(model), &iter, COMBO_USER_MODEL_ID_COL, &uid, -1);

      if(g_strcmp0(uid, accountinfo->id) == 0)
      {
        gtk_list_store_set(model, &iter, COMBO_USER_MODEL_NAME_COL, accountinfo->readablename,
                           COMBO_USER_MODEL_TOKEN_COL, accountinfo->token, -1);
        updated = TRUE;
        break;
      }
    }

    if(!updated)
    {
      gtk_list_store_append(model, &iter);
      gtk_list_store_set(model, &iter, COMBO_USER_MODEL_NAME_COL, accountinfo->readablename,
                         COMBO_USER_MODEL_TOKEN_COL, accountinfo->token, COMBO_USER_MODEL_ID_COL,
                         accountinfo->id, -1);
    }
    gtk_combo_box_set_active_iter(ui->comboBox_username, &iter);
    // we have to re-set the current token here since ui_combo_username_changed is called
    // on gtk_combo_box_set_active_iter (and thus is resetting the active token)
    ctx->token = g_strdup(accountinfo->token);
    fb_account_info_destroy(accountinfo);
  }

  ui_refresh_albums(ui);
  ui->connected = TRUE;
  gtk_button_set_label(ui->button_login, _("logout"));
  gtk_widget_set_sensitive(GTK_WIDGET(ui->comboBox_album), TRUE);

  return;

error:
  gtk_button_set_label(ui->button_login, _("login"));
  gtk_widget_set_sensitive(GTK_WIDGET(ui->comboBox_album), FALSE);
}

static void ui_authenticate(dt_storage_facebook_gui_data_t *ui)
{
  if(ui->facebook_api == NULL)
  {
    ui->facebook_api = fb_api_init();
  }

  FBContext *ctx = ui->facebook_api;
  gboolean mustsaveaccount = FALSE;

  gchar *uiselectedaccounttoken = NULL;
  GtkTreeIter iter;
  gtk_combo_box_get_active_iter(ui->comboBox_username, &iter);
  GtkTreeModel *accountModel = gtk_combo_box_get_model(ui->comboBox_username);
  gtk_tree_model_get(accountModel, &iter, 1, &uiselectedaccounttoken, -1);

  gtk_button_set_label(ui->button_login, _("login"));
  gtk_widget_set_sensitive(GTK_WIDGET(ui->comboBox_album), FALSE);

  g_free(ctx->token);
  ctx->token = g_strdup(uiselectedaccounttoken);
  // check selected token if we already have one
  if(ctx->token != NULL && !fb_test_auth_token(ctx))
  {
    g_free(ctx->token);
    ctx->token = NULL;
  }

  if(ctx->token == NULL)
  {
    mustsaveaccount = TRUE;

#ifdef HAVE_HTTP_SERVER
    // try to get the token from the callback URL
    if(facebook_get_user_auth_token_from_server(ui)) return;
#endif

    // if we reached this point we either have no http server support
    // or couldn't start it (no free port, ...)
    ctx->token = facebook_get_user_auth_token_from_url(ui); // ask user to log in
  }

  ui_authenticate_finish(ui, mustsaveaccount);
}


static void ui_login_clicked(GtkButton *button, gpointer data)
{
  dt_storage_facebook_gui_data_t *ui = (dt_storage_facebook_gui_data_t *)data;
  if(ui->connected == FALSE)
  {
    ui_authenticate(ui);
  }
  else // disconnect user
  {
    if(ui->facebook_api->token != NULL)
    {
      GtkTreeModel *model = gtk_combo_box_get_model(ui->comboBox_username);
      GtkTreeIter iter;
      gtk_combo_box_get_active_iter(ui->comboBox_username, &iter);
      gchar *userid;
      gtk_tree_model_get(model, &iter, COMBO_USER_MODEL_ID_COL, &userid, -1);
      remove_account_info(userid);
      gtk_button_set_label(ui->button_login, _("login"));
      gtk_widget_set_sensitive(GTK_WIDGET(ui->comboBox_album), FALSE);
      ui_refresh_users(ui);
      ui->connected = FALSE;
    }
  }
}


static void ui_reset_albums_creation(struct dt_storage_facebook_gui_data_t *ui)
{
  GtkListStore *model_album = GTK_LIST_STORE(gtk_combo_box_get_model(ui->comboBox_album));
  gtk_list_store_clear(model_album);
  gtk_entry_set_text(ui->entry_album_summary, "");
  gtk_entry_set_text(ui->entry_album_title, "");
  gtk_widget_hide(GTK_WIDGET(ui->hbox_album));
}

static void ui_combo_username_changed(GtkComboBox *combo, struct dt_storage_facebook_gui_data_t *ui)
{
  GtkTreeIter iter;
  gchar *token = NULL;
  if(!gtk_combo_box_get_active_iter(combo, &iter)) return; // ie: list is empty while clearing the combo
  GtkTreeModel *model = gtk_combo_box_get_model(combo);
  gtk_tree_model_get(model, &iter, 1, &token, -1); // get the selected token
  ui->connected = FALSE;
  gtk_button_set_label(ui->button_login, _("login"));
  g_free(ui->facebook_api->token);
  ui->facebook_api->token = NULL;
  ui_reset_albums_creation(ui);
}

static void ui_combo_album_changed(GtkComboBox *combo, gpointer data)
{
  dt_storage_facebook_gui_data_t *ui = (dt_storage_facebook_gui_data_t *)data;

  GtkTreeIter iter;
  gchar *albumid = NULL;
  if(gtk_combo_box_get_active_iter(combo, &iter))
  {
    GtkTreeModel *model = gtk_combo_box_get_model(combo);
    gtk_tree_model_get(model, &iter, 1, &albumid, -1); // get the album id
  }

  if(albumid == NULL)
  {
    gtk_widget_set_no_show_all(GTK_WIDGET(ui->hbox_album), FALSE);
    gtk_widget_show_all(GTK_WIDGET(ui->hbox_album));
  }
  else
  {
    gtk_widget_set_no_show_all(GTK_WIDGET(ui->hbox_album), TRUE);
    gtk_widget_hide(GTK_WIDGET(ui->hbox_album));
  }
}

////////////////////////// darktable library interface

/* plugin name */
const char *name(const struct dt_imageio_module_storage_t *self)
{
  return _("facebook webalbum");
}

int recommended_dimension(struct dt_imageio_module_storage_t *self, dt_imageio_module_data_t *data,
                          uint32_t *width, uint32_t *height)
{
  *width = FB_IMAGE_MAX_SIZE;
  *height = FB_IMAGE_MAX_SIZE;
  return 1;
}

/* construct widget above */
void gui_init(struct dt_imageio_module_storage_t *self)
{
  self->gui_data = g_malloc0(sizeof(dt_storage_facebook_gui_data_t));
  dt_storage_facebook_gui_data_t *ui = self->gui_data;
  ui->facebook_api = fb_api_init();

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  // create labels
  ui->label_album_title = GTK_LABEL(gtk_label_new(_("title")));
  ui->label_album_summary = GTK_LABEL(gtk_label_new(_("summary")));
  ui->label_album_privacy = GTK_LABEL(gtk_label_new(_("privacy")));
  ui->label_status = GTK_LABEL(gtk_label_new(NULL));

  gtk_widget_set_halign(GTK_WIDGET(ui->label_album_title), GTK_ALIGN_START);
  gtk_widget_set_halign(GTK_WIDGET(ui->label_album_summary), GTK_ALIGN_START);
  gtk_widget_set_halign(GTK_WIDGET(ui->label_album_privacy), GTK_ALIGN_START);

  // create entries
  GtkListStore *model_username = gtk_list_store_new(COMBO_USER_MODEL_NB_COL, G_TYPE_STRING, G_TYPE_STRING,
                                                    G_TYPE_STRING); // text, token, id
  ui->comboBox_username = GTK_COMBO_BOX(gtk_combo_box_new_with_model(GTK_TREE_MODEL(model_username)));
  GtkCellRenderer *p_cell = gtk_cell_renderer_text_new();
  gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(ui->comboBox_username), p_cell, FALSE);
  gtk_cell_layout_set_attributes(GTK_CELL_LAYOUT(ui->comboBox_username), p_cell, "text", 0, NULL);

  ui->entry_album_title = GTK_ENTRY(gtk_entry_new());
  ui->entry_album_summary = GTK_ENTRY(gtk_entry_new());

  dt_gui_key_accel_block_on_focus_connect(GTK_WIDGET(ui->comboBox_username));
  dt_gui_key_accel_block_on_focus_connect(GTK_WIDGET(ui->entry_album_title));
  dt_gui_key_accel_block_on_focus_connect(GTK_WIDGET(ui->entry_album_summary));

  gtk_entry_set_width_chars(GTK_ENTRY(ui->entry_album_title), 0);
  gtk_entry_set_width_chars(GTK_ENTRY(ui->entry_album_summary), 0);


  // retrieve saved accounts
  ui_refresh_users(ui);

  //////// album list /////////
  GtkWidget *albumlist = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  GtkListStore *model_album
      = gtk_list_store_new(COMBO_ALBUM_MODEL_NB_COL, G_TYPE_STRING, G_TYPE_STRING); // name, id
  ui->comboBox_album = GTK_COMBO_BOX(gtk_combo_box_new_with_model(GTK_TREE_MODEL(model_album)));
  p_cell = gtk_cell_renderer_text_new();
  gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(ui->comboBox_album), p_cell, FALSE);
  gtk_cell_layout_set_attributes(GTK_CELL_LAYOUT(ui->comboBox_album), p_cell, "text", 0, NULL);

  gtk_widget_set_sensitive(GTK_WIDGET(ui->comboBox_album), FALSE);
  gtk_combo_box_set_row_separator_func(ui->comboBox_album, combobox_separator, ui->comboBox_album, NULL);
  gtk_box_pack_start(GTK_BOX(albumlist), GTK_WIDGET(ui->comboBox_album), TRUE, TRUE, 0);

  ui->comboBox_privacy = GTK_COMBO_BOX(gtk_combo_box_text_new());
  GtkListStore *list_store = gtk_list_store_new(COMBO_ALBUM_MODEL_NB_COL, G_TYPE_STRING, G_TYPE_INT);
  GtkTreeIter iter;
  gtk_list_store_append(list_store, &iter);
  gtk_list_store_set(list_store, &iter, COMBO_PRIVACY_MODEL_NAME_COL, _("only me"),
                     COMBO_PRIVACY_MODEL_VAL_COL, FBALBUM_PRIVACY_SELF, -1);
  gtk_list_store_append(list_store, &iter);
  gtk_list_store_set(list_store, &iter, COMBO_PRIVACY_MODEL_NAME_COL, _("friends"),
                     COMBO_PRIVACY_MODEL_VAL_COL, FBALBUM_PRIVACY_ALL_FRIENDS, -1);
  gtk_list_store_append(list_store, &iter);
  gtk_list_store_set(list_store, &iter, COMBO_PRIVACY_MODEL_NAME_COL, _("public"),
                     COMBO_PRIVACY_MODEL_VAL_COL, FBALBUM_PRIVACY_EVERYONE, -1);
  gtk_list_store_append(list_store, &iter);
  gtk_list_store_set(list_store, &iter, COMBO_PRIVACY_MODEL_NAME_COL, _("friends of friends"),
                     COMBO_PRIVACY_MODEL_VAL_COL, FBALBUM_PRIVACY_FRIENDS_OF_FRIENDS, -1);

  gtk_combo_box_set_model(ui->comboBox_privacy, GTK_TREE_MODEL(list_store));

  gtk_combo_box_set_active(GTK_COMBO_BOX(ui->comboBox_privacy), 1); // Set default permission to private
  ui->button_login = GTK_BUTTON(gtk_button_new_with_label(_("login")));
  ui->connected = FALSE;

  // pack the ui
  ////the auth box
  GtkWidget *hbox_auth = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
  GtkWidget *vbox_auth_labels = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *vbox_auth_fields = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_pack_start(GTK_BOX(hbox_auth), vbox_auth_labels, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(hbox_auth), vbox_auth_fields, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hbox_auth), TRUE, FALSE, 2);
  gtk_box_pack_start(GTK_BOX(vbox_auth_fields), GTK_WIDGET(ui->comboBox_username), TRUE, FALSE, 2);

  gtk_box_pack_start(GTK_BOX(vbox_auth_labels), GTK_WIDGET(gtk_label_new("")), TRUE, TRUE, 2);
  gtk_box_pack_start(GTK_BOX(vbox_auth_fields), GTK_WIDGET(ui->button_login), TRUE, FALSE, 2);

  gtk_box_pack_start(GTK_BOX(vbox_auth_fields), GTK_WIDGET(albumlist), TRUE, FALSE, 2);

  ////the album creation box
  ui->hbox_album = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5));
  gtk_widget_set_no_show_all(GTK_WIDGET(ui->hbox_album), TRUE); // hide it by default
  GtkWidget *vbox_album_labels = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *vbox_album_fields = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(ui->hbox_album), TRUE, FALSE, 5);
  gtk_box_pack_start(GTK_BOX(ui->hbox_album), vbox_album_labels, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(ui->hbox_album), vbox_album_fields, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(vbox_album_labels), GTK_WIDGET(ui->label_album_title), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(vbox_album_fields), GTK_WIDGET(ui->entry_album_title), TRUE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(vbox_album_labels), GTK_WIDGET(ui->label_album_summary), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(vbox_album_fields), GTK_WIDGET(ui->entry_album_summary), TRUE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(vbox_album_labels), GTK_WIDGET(ui->label_album_privacy), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(vbox_album_fields), GTK_WIDGET(ui->comboBox_privacy), TRUE, FALSE, 0);

  // connect buttons to signals
  g_signal_connect(G_OBJECT(ui->button_login), "clicked", G_CALLBACK(ui_login_clicked), (gpointer)ui);
  g_signal_connect(G_OBJECT(ui->comboBox_username), "changed", G_CALLBACK(ui_combo_username_changed),
                   (gpointer)ui);
  g_signal_connect(G_OBJECT(ui->comboBox_album), "changed", G_CALLBACK(ui_combo_album_changed), (gpointer)ui);

  g_object_unref(model_username);
  g_object_unref(model_album);
  g_object_unref(list_store);
}

/* destroy resources */
void gui_cleanup(struct dt_imageio_module_storage_t *self)
{
  dt_storage_facebook_gui_data_t *ui = self->gui_data;
  dt_gui_key_accel_block_on_focus_disconnect(GTK_WIDGET(ui->comboBox_username));
  dt_gui_key_accel_block_on_focus_disconnect(GTK_WIDGET(ui->entry_album_title));
  dt_gui_key_accel_block_on_focus_disconnect(GTK_WIDGET(ui->entry_album_summary));
  if(ui->facebook_api != NULL) fb_api_destroy(ui->facebook_api);
  g_free(self->gui_data);
}

/* reset options to defaults */
void gui_reset(struct dt_imageio_module_storage_t *self)
{
  // TODO?
}

/* try and see if this format is supported? */
int supported(struct dt_imageio_module_storage_t *self, struct dt_imageio_module_format_t *format)
{
  if(strcmp(format->mime(NULL), "image/jpeg") == 0)
    return 1;
  else if(strcmp(format->mime(NULL), "image/png") == 0)
    return 1;
  return 0;
}

/* this actually does the work */
int store(dt_imageio_module_storage_t *self, struct dt_imageio_module_data_t *sdata, const int imgid,
          dt_imageio_module_format_t *format, dt_imageio_module_data_t *fdata, const int num, const int total,
          const gboolean high_quality, const gboolean upscale, dt_colorspaces_color_profile_type_t icc_type,
          const gchar *icc_filename, dt_iop_color_intent_t icc_intent)
{
  gint result = 1;
  dt_storage_facebook_param_t *p = (dt_storage_facebook_param_t *)sdata;

  const char *ext = format->extension(fdata);
  char fname[PATH_MAX] = { 0 };
  dt_loc_get_tmp_dir(fname, sizeof(fname));
  g_strlcat(fname, "/darktable.XXXXXX.", sizeof(fname));
  g_strlcat(fname, ext, sizeof(fname));

  gint fd = g_mkstemp(fname);
  if(fd == -1)
  {
    dt_control_log("failed to create temporary image for facebook export");
    return 1;
  }
  close(fd);

  // get metadata
  const dt_image_t *img = dt_image_cache_get(darktable.image_cache, imgid, 'r');
  char *caption = NULL;
  GList *caption_list = NULL;

  caption_list = dt_metadata_get(img->id, "Xmp.dc.title", NULL);
  if(caption_list != NULL)
  {
    caption = g_strdup(caption_list->data);
    g_list_free_full(caption_list, &g_free);
  }
  else
  {
    caption_list = dt_metadata_get(img->id, "Xmp.dc.description", NULL);
    if(caption_list != NULL)
    {
      caption = g_strdup(caption_list->data);
      g_list_free_full(caption_list, &g_free);
    }
  }
  dt_image_cache_read_release(darktable.image_cache, img);

  // facebook doesn't allow pictures bigger than FB_IMAGE_MAX_SIZExFB_IMAGE_MAX_SIZE px
  if(fdata->max_height == 0 || fdata->max_height > FB_IMAGE_MAX_SIZE) fdata->max_height = FB_IMAGE_MAX_SIZE;
  if(fdata->max_width == 0 || fdata->max_width > FB_IMAGE_MAX_SIZE) fdata->max_width = FB_IMAGE_MAX_SIZE;

  if(dt_imageio_export(imgid, fname, format, fdata, high_quality, upscale, FALSE, icc_type, icc_filename, icc_intent,
                       self, sdata, num, total)
     != 0)
  {
    g_printerr("[facebook] could not export to file: `%s'!\n", fname);
    dt_control_log(_("could not export to file `%s'!"), fname);
    result = 0;
    goto cleanup;
  }

  if(p->facebook_ctx->album_id == NULL)
  {
    if(p->facebook_ctx->album_title == NULL || strlen(p->facebook_ctx->album_title) == 0)
    {
      dt_control_log(_("unable to create album, no title provided"));
      result = 0;
      goto cleanup;
    }
    const gchar *album_id
        = fb_create_album(p->facebook_ctx, p->facebook_ctx->album_title, p->facebook_ctx->album_summary,
                          p->facebook_ctx->album_permission);
    if(album_id == NULL)
    {
      dt_control_log(_("unable to create album"));
      result = 0;
      goto cleanup;
    }
    p->facebook_ctx->album_id = g_strdup(album_id);
  }

  const char *photoid = fb_upload_photo_to_album(p->facebook_ctx, p->facebook_ctx->album_id, fname, caption);
  if(photoid == NULL)
  {
    dt_control_log(_("unable to export photo to webalbum"));
    result = 0;
    goto cleanup;
  }

cleanup:
  g_unlink(fname);
  g_free(caption);

  if(result)
  {
    // this makes sense only if the export was successful
    dt_control_log(ngettext("%d/%d exported to facebook webalbum", "%d/%d exported to facebook webalbum", num),
                   num, total);
  }
  return 0;
}

static gboolean _finalize_store(gpointer user_data)
{
  dt_storage_facebook_gui_data_t *ui = (dt_storage_facebook_gui_data_t *)user_data;
  ui_reset_albums_creation(ui);
  ui_refresh_albums(ui);

  return FALSE;
}

void finalize_store(struct dt_imageio_module_storage_t *self, dt_imageio_module_data_t *data)
{
  g_main_context_invoke(NULL, _finalize_store, self->gui_data);
}

size_t params_size(dt_imageio_module_storage_t *self)
{
  return sizeof(gint64);
}


void init(dt_imageio_module_storage_t *self)
{
}
void *get_params(struct dt_imageio_module_storage_t *self)
{
  dt_storage_facebook_gui_data_t *ui = (dt_storage_facebook_gui_data_t *)self->gui_data;
  if(!ui) return NULL; // gui not initialized, CLI mode
  if(ui->facebook_api == NULL || ui->facebook_api->token == NULL)
  {
    return NULL;
  }
  dt_storage_facebook_param_t *p
      = (dt_storage_facebook_param_t *)g_malloc0(sizeof(dt_storage_facebook_param_t));
  p->hash = 1;
  p->facebook_ctx = ui->facebook_api;
  int index = gtk_combo_box_get_active(ui->comboBox_album);
  if(index < 0)
  {
    g_free(p);
    return NULL;
  }
  else if(index == 0)
  {
    p->facebook_ctx->album_id = NULL;
    p->facebook_ctx->album_title = g_strdup(gtk_entry_get_text(ui->entry_album_title));
    p->facebook_ctx->album_summary = g_strdup(gtk_entry_get_text(ui->entry_album_summary));
    GtkTreeModel *model = gtk_combo_box_get_model(ui->comboBox_privacy);
    GtkTreeIter iter;
    int permission = -1;
    gtk_combo_box_get_active_iter(ui->comboBox_privacy, &iter);
    gtk_tree_model_get(model, &iter, COMBO_PRIVACY_MODEL_VAL_COL, &permission, -1);
    p->facebook_ctx->album_permission = permission;
  }
  else
  {
    GtkTreeModel *model = gtk_combo_box_get_model(ui->comboBox_album);
    GtkTreeIter iter;
    gchar *albumid = NULL;
    gtk_combo_box_get_active_iter(ui->comboBox_album, &iter);
    gtk_tree_model_get(model, &iter, COMBO_ALBUM_MODEL_ID_COL, &albumid, -1);
    p->facebook_ctx->album_id = g_strdup(albumid);
  }

  // recreate a new context for further usages
  ui->facebook_api = fb_api_init();
  ui->facebook_api->token = g_strdup(p->facebook_ctx->token);
  return p;
}


void free_params(struct dt_imageio_module_storage_t *self, dt_imageio_module_data_t *data)
{
  if(!data) return;
  dt_storage_facebook_param_t *p = (dt_storage_facebook_param_t *)data;
  fb_api_destroy(p->facebook_ctx);
  g_free(p);
}

int set_params(struct dt_imageio_module_storage_t *self, const void *params, const int size)
{
  if(size != self->params_size(self)) return 1;
  // gui stuff not updated, as sensitive user data is not stored in the preset.
  // TODO: store name/hash in kwallet/etc module and get encrypted stuff from there!
  return 0;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
