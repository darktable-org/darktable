/*
    This file is part of darktable,
    copyright (c) 2012 Pierre Lamot
    copyright (c) 2013-2014 Jose Carlos Garcia Sogo
    copyright (c) 2012-2019 Pascal Obry

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
#include "common/curl_tools.h"
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
#include <libxml/parser.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>

DT_MODULE(2)

#define GOOGLE_WS_BASE_URL "https://accounts.google.com/"
#define GOOGLE_API_BASE_URL "https://www.googleapis.com/"
#define GOOGLE_URI "urn:ietf:wg:oauth:2.0:oob"
#define GOOGLE_GPHOTO "https://photoslibrary.googleapis.com/"

// client_id and client_secret are in darktablerc. those values are
// shared and a max of 10.000 calls are allowed per day
//  "plugins/imageio/storage/gphoto/id"
//  "plugins/imageio/storage/gphoto/secret"
//
// to generate new: https://developers.google.com/photos/library/guides/get-started


#define MSGCOLOR_RED "#e07f7f"
#define MSGCOLOR_GREEN "#7fe07f"

#define gphoto_EXTRA_VERBOSE FALSE
#define GPHOTO_STORAGE "gphoto"

/** Authenticate against google gphoto service*/
typedef struct _buffer_t
{
  char *data;
  size_t size;
  size_t offset;
} _buffer_t;

typedef enum _combo_user_model_t
{
  COMBO_USER_MODEL_NAME_COL = 0,
  COMBO_USER_MODEL_TOKEN_COL,
  COMBO_USER_MODEL_REFRESH_TOKEN_COL,
  COMBO_USER_MODEL_ID_COL,
  COMBO_USER_MODEL_NB_COL
} dt_combo_user_model_t;

typedef enum _combo_album_model_t
{
  COMBO_ALBUM_MODEL_NAME_COL = 0,
  COMBO_ALBUM_MODEL_ID_COL,
  COMBO_ALBUM_MODEL_NB_COL
} dt_combo_album_model_t;

/**
 * Represents information about an album
 */
typedef struct dt_gphoto_album_t
{
  gchar *id;
  gchar *name;
  int size;
} dt_gphoto_album_t;

typedef struct _curl_args_t
{
  char name[100];
  char value[2048];
} _curl_args_t;

static dt_gphoto_album_t *gphoto_album_init()
{
  return (dt_gphoto_album_t *)g_malloc0(sizeof(dt_gphoto_album_t));
}

static void gphoto_album_destroy(dt_gphoto_album_t *album)
{
  if(album == NULL) return;
  g_free(album->id);
  g_free(album->name);
  g_free(album);
}

/**
 * Represents information about an account
 */
typedef struct _gphoto_account_info_t
{
  gchar *id;
  gchar *username;
  gchar *token;
  gchar *refresh_token;
} dt_gphoto_account_info_t;

static dt_gphoto_account_info_t *gphoto_account_info_init()
{
  return (dt_gphoto_account_info_t *)g_malloc0(sizeof(dt_gphoto_account_info_t));
}

static void gphoto_account_info_destroy(dt_gphoto_account_info_t *account)
{
  if(account == NULL) return;
  g_free(account->id);
  g_free(account->username);
  g_free(account);
}

typedef struct _gphoto_context_t
{
  gchar album_id[1024];
  gchar userid[1024];

  gchar *album_title;
  int album_permission;

  /// curl context
  CURL *curl_ctx;
  /// Json parser context
  JsonParser *json_parser;

  GString *errmsg;
  GString *response;

  /// authorization token
  gchar *token;
  gchar *refresh_token;
  gchar *google_client_id;
  gchar *google_client_secret;
} dt_gphoto_context_t;

typedef struct dt_storage_gphoto_gui_data_t
{
  // == ui elements ==
  GtkLabel *label_status;

  GtkComboBox *combo_username;
  GtkButton *button_login;

  GtkDarktableButton *dtbutton_refresh_album;
  GtkComboBox *combo_album;
  int albums_count;

  //  === album creation section ===
  GtkLabel *label_album_title;

  GtkEntry *entry_album_title;

  GtkBox *hbox_album;

  // == context ==
  gboolean connected;
  dt_gphoto_context_t *gphoto_api;
} dt_storage_gphoto_gui_data_t;


static dt_gphoto_context_t *gphoto_api_init()
{
  dt_gphoto_context_t *ctx = (dt_gphoto_context_t *)g_malloc0(sizeof(dt_gphoto_context_t));
  ctx->curl_ctx = curl_easy_init();
  ctx->errmsg = g_string_new("");
  ctx->response = g_string_new("");
  ctx->json_parser = json_parser_new();
  ctx->google_client_id = dt_conf_get_string("plugins/imageio/storage/gphoto/id");
  ctx->google_client_secret = dt_conf_get_string("plugins/imageio/storage/gphoto/secret");
  return ctx;
}

static void gphoto_api_destroy(dt_gphoto_context_t *ctx)
{
  if(ctx == NULL) return;
  curl_easy_cleanup(ctx->curl_ctx);
  g_free(ctx->token);
  g_free(ctx->refresh_token);
  g_free(ctx->album_title);
  g_free(ctx->google_client_id);
  g_free(ctx->google_client_secret);

  g_object_unref(ctx->json_parser);
  g_string_free(ctx->errmsg, TRUE);
  g_string_free(ctx->response, TRUE);
  g_free(ctx);
}


typedef struct dt_storage_gphoto_param_t
{
  gint64 hash;
} dt_storage_gphoto_param_t;


static gchar *gphoto_get_user_refresh_token(dt_gphoto_context_t *ctx);
static void ui_refresh_albums_fill(dt_gphoto_album_t *album, GtkListStore *list_store);

//////////////////////////// curl requests related functions

static size_t _gphoto_api_buffer_read_func(void *ptr, size_t size, size_t nmemb, void *stream)
{
  _buffer_t *buffer = (_buffer_t *)stream;
  size_t dsize = 0;
  if((buffer->size - buffer->offset) > nmemb)
    dsize = nmemb;
  else
    dsize = (buffer->size - buffer->offset);

  memcpy(ptr, buffer->data + buffer->offset, dsize);
  buffer->offset += dsize;
  return dsize;
}

static size_t curl_write_data_cb(void *ptr, size_t size, size_t nmemb, void *data)
{
  GString *string = (GString *)data;
  g_string_append_len(string, ptr, size * nmemb);
#if gphoto_EXTRA_VERBOSE == TRUE
  if(strlen(string->str)<1500)
  g_printf("server reply: %s\n", string->str);
#endif
  return size * nmemb;
}

static JsonObject *gphoto_parse_response(dt_gphoto_context_t *ctx, GString *response)
{
  GError *error = NULL;
  gboolean ret = json_parser_load_from_data(ctx->json_parser, response->str, response->len, &error);

  if(ret)
  {
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
  else // not a json response, can be the upload-token
  {
    ctx->response = g_string_new(response->str);
    return NULL;
  }
}

static GList *_gphoto_query_add_arguments(GList *args, const char *name, const char *value)
{
  _curl_args_t *arg = malloc(sizeof(_curl_args_t));
  g_strlcpy(arg->name, name, sizeof(arg->name));
  g_strlcpy(arg->value, value, sizeof(arg->value));
  return g_list_append(args, arg);
}

/**
 * perform a GET request on google photo api
 * @returns NULL if the request fails, or a JsonObject of the reply
 */
static JsonObject *gphoto_query_get(dt_gphoto_context_t *ctx, const gchar *url, GList *args)
{
  g_return_val_if_fail(ctx != NULL, NULL);
  g_return_val_if_fail(ctx->token != NULL, NULL);

  // send the request
  GString *response = g_string_new("");

  dt_curl_init(ctx->curl_ctx, strstr(url,"v1/albums")==NULL?gphoto_EXTRA_VERBOSE:FALSE);

  args = _gphoto_query_add_arguments(args, "alt", "json");
  args = _gphoto_query_add_arguments(args, "access_token", ctx->token);

  GString *gurl = g_string_new(url);

  GList *a = args;

  while (a)
  {
    _curl_args_t *ca = (_curl_args_t *)a->data;
    if(a==args)
      g_string_append(gurl, "?");
    else
      g_string_append(gurl, "&");
    g_string_append(gurl, ca->name);
    g_string_append(gurl, "=");
    g_string_append(gurl, ca->value);

    a = g_list_next(a);
  }

  curl_easy_setopt(ctx->curl_ctx, CURLOPT_URL, gurl->str);
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_WRITEFUNCTION, curl_write_data_cb);
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_WRITEDATA, response);

  int res = curl_easy_perform(ctx->curl_ctx);

  g_list_free(args);
  g_string_free(gurl, TRUE);

  if(res != CURLE_OK)
  {
    g_string_free(response, TRUE);
    return NULL;
  }
  // parse the response
  JsonObject *respobj = gphoto_parse_response(ctx, response);

  g_string_free(response, TRUE);

  return respobj;
}

/**
 * perform a POST request on google photo api
 * @returns NULL if the request fails, or a JsonObject of the reply
 */
static JsonObject *gphoto_query_post(dt_gphoto_context_t *ctx, const gchar *url,
                                     struct curl_slist *headers, GList *args, gchar *body, int size)
{
  g_return_val_if_fail(ctx != NULL, NULL);
  g_return_val_if_fail(ctx->token != NULL, NULL);

  // send the request
  GString *response = g_string_new("");

  gchar *auth = dt_util_dstrcat(NULL, "Authorization: Bearer %s", ctx->token);
  struct curl_slist *h = curl_slist_append(headers, auth);

  _buffer_t writebuffer;
  writebuffer.data = (void *)body;
  writebuffer.size = size;
  writebuffer.offset = 0;

  dt_curl_init(ctx->curl_ctx, TRUE);

  curl_easy_setopt(ctx->curl_ctx, CURLOPT_HTTPHEADER, h);

  curl_easy_setopt(ctx->curl_ctx, CURLOPT_URL, url);
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_POST, 1);
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_WRITEFUNCTION, curl_write_data_cb);
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_WRITEDATA, response);
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_READFUNCTION, _gphoto_api_buffer_read_func);
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_READDATA, &writebuffer);

  int res = curl_easy_perform(ctx->curl_ctx);

  curl_slist_free_all(h);
  g_list_free(args);

  if(res != CURLE_OK)
  {
    g_string_free(response, TRUE);
    return NULL;
  }
  // parse the response
  JsonObject *respobj = gphoto_parse_response(ctx, response);

  g_string_free(response, TRUE);

  return respobj;
}

typedef struct
{
  struct curl_httppost *formpost;
  struct curl_httppost *lastptr;
} HttppostFormList;

/**
 * perform a POST request on google api to get the auth token
 *
 * @param ctx gphoto context (token field must be set)
 * @param method the method to call on the google API, the methods should not start with '/' example:
 *"me/albums"
 * @param args hashtable of the arguments to be added to the requests, might be null if none
 * @returns NULL if the request fails, or a JsonObject of the reply
 */

static JsonObject *gphoto_query_post_auth(dt_gphoto_context_t *ctx, const gchar *url, gchar *args)
{
  g_return_val_if_fail(ctx != NULL, NULL);

  // send the requests
  GString *response = g_string_new("");

  dt_curl_init(ctx->curl_ctx, gphoto_EXTRA_VERBOSE);

  curl_easy_setopt(ctx->curl_ctx, CURLOPT_URL, url);
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_POST, 1);
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_COPYPOSTFIELDS, args);
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_WRITEFUNCTION, curl_write_data_cb);
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_WRITEDATA, response);

  int res = curl_easy_perform(ctx->curl_ctx);
  if(res != CURLE_OK) return NULL;
  // parse the response
  JsonObject *respobj = gphoto_parse_response(ctx, response);
  g_string_free(response, TRUE);
  return respobj;
}

//////////////////////////// gphoto api functions

/**
 * @returns TRUE if the current token is valid
 */
static gboolean gphoto_test_auth_token(dt_gphoto_context_t *ctx)
{
  gchar *access_token = NULL;
  access_token = gphoto_get_user_refresh_token(ctx);

  if(access_token != NULL) ctx->token = access_token;

  return access_token != NULL;
}

static dt_gphoto_album_t *_json_new_album(JsonObject *obj)
{
  // handle on writable albums (in Google Photo only albums created by the API are writeable via the API)

  if(json_object_has_member(obj, "isWriteable"))
    if(json_object_get_boolean_member(obj, "isWriteable"))
    {
      dt_gphoto_album_t *album = gphoto_album_init();
      if(album == NULL) goto error;

      const char *id = json_object_get_string_member(obj, "id");
      const char *name = json_object_get_string_member(obj, "title");
      const int size = json_object_has_member(obj, "mediaItemsCount")
        ? json_object_get_int_member(obj, "mediaItemsCount")
        : 0;

      if(id == NULL || name == NULL)
      {
        gphoto_album_destroy(album);
        goto error;
      }

      album->id = g_strdup(id);
      album->name = g_strdup(name);
      album->size = size;

      return album;
    }

 error:
  return NULL;
}

/**
 * @return a GList of dt_gphoto_album_ts associated to the user
 */
static GList *gphoto_get_album_list(dt_gphoto_context_t *ctx, gboolean *ok)
{
  if(!ok) return NULL;

  *ok = TRUE;
  GList *album_list = NULL;

  GList *args = NULL;

//  args = _gphoto_query_add_arguments(args, "pageSize", "50"); // max for list albums

  JsonObject *reply = gphoto_query_get(ctx, GOOGLE_GPHOTO "v1/albums", NULL);
  if(reply == NULL) goto error;

  do
  {
    JsonArray *jsalbums = json_object_get_array_member(reply, "albums");

    for(gint i = 0; i < json_array_get_length(jsalbums); i++)
    {
      JsonObject *obj = json_array_get_object_element(jsalbums, i);
      if(obj == NULL) continue;

      dt_gphoto_album_t *album = _json_new_album(obj);
      if(album)
        album_list = g_list_append(album_list, album);
    }

    args = NULL;

//    args = _gphoto_query_add_arguments(args, "pageSize", "50"); // max for list albums

    if(json_object_has_member(reply, "nextPageToken"))
      args = _gphoto_query_add_arguments(args, "pageToken", json_object_get_string_member(reply, "nextPageToken"));
    else
      break;

    reply = gphoto_query_get(ctx, GOOGLE_GPHOTO "v1/albums", args);
    if(reply == NULL) goto error;
  } while(true);

  return album_list;

error:
  *ok = FALSE;
  g_list_free_full(album_list, (GDestroyNotify)gphoto_album_destroy);
  return NULL;
}

static void ui_reset_albums_creation(struct dt_storage_gphoto_gui_data_t *ui)
{
  gtk_entry_set_text(ui->entry_album_title, "");
  gtk_widget_hide(GTK_WIDGET(ui->hbox_album));
}

/**
 * @see https://developers.google.com/photos/library/guides/create-albums
 * @return the id of the newly reacted
 */
static const gchar *gphoto_create_album(dt_storage_gphoto_gui_data_t *ui, dt_gphoto_context_t *ctx, gchar *name)
{
  struct curl_slist *headers = NULL;

  gchar *jbody = dt_util_dstrcat(NULL, "{ \"album\": { \"title\": \"%s\"} }", name);

  headers = curl_slist_append(headers, "Content-type: application/json");

  JsonObject *response = gphoto_query_post(ctx, GOOGLE_GPHOTO "v1/albums", headers, NULL, jbody, strlen(jbody));

  // add new album into the list
  dt_gphoto_album_t *album = _json_new_album(response);
  GtkListStore *model_album = GTK_LIST_STORE(gtk_combo_box_get_model(ui->combo_album));

  if(album)
  {
    ui_refresh_albums_fill(album, model_album);
    gtk_combo_box_set_active(ui->combo_album, ui->albums_count);
    ui->albums_count++;
    ui_reset_albums_creation(ui);
  }

  g_free(jbody);

  return album?album->id:NULL;
}

/**
 *  step.1: https://developers.google.com/photos/library/guides/upload-media#uploading-bytes
 *  step.2: https://developers.google.com/photos/library/guides/upload-media#creating-media-item
 */
static const gchar *gphoto_upload_photo_to_album(dt_gphoto_context_t *ctx, gchar *albumid, gchar *fname,
                                                 gchar *title, gchar *summary, const int imgid)
{
  // step.1 : upload raw data

  gchar *basename = g_path_get_basename(fname);

  JsonObject *response = NULL;
  gchar *photo_id = NULL;
  gchar *upload_token = NULL;

  gchar *filename = dt_util_dstrcat(NULL, "X-Goog-Upload-File-Name: %s", basename);
  g_free(basename);

  struct curl_slist *headers = curl_slist_append(NULL, "Content-type: application/octet-stream");
  headers = curl_slist_append(headers, filename);
  headers = curl_slist_append(headers, "X-Goog-Upload-Protocol: raw");

  // Open the temp file and read image to memory
  GMappedFile *imgfile = g_mapped_file_new(fname, FALSE, NULL);
  const int size = g_mapped_file_get_length(imgfile);
  gchar *data = g_mapped_file_get_contents(imgfile);

  response = gphoto_query_post(ctx, GOOGLE_GPHOTO "v1/uploads", headers, NULL, data, size);

  if(!response)
  {
    // all good, the body is the upload-token
    upload_token = g_strdup(ctx->response->str);
  }
  else
    return NULL;

  // step.2 : add raw data into an album

  headers = NULL;

  headers = curl_slist_append(headers, "Content-type: application/json");

  gchar *jbody = dt_util_dstrcat(NULL, "{ \"albumId\": \"%s\", "
                                         "\"newMediaItems\": [ "
                                           "{ \"description\": \"%s\", "
                                           "  \"simpleMediaItem\": { \"uploadToken\": \"%s\"} "
                                           "} ] }", albumid, summary, upload_token);

  response = gphoto_query_post(ctx, GOOGLE_GPHOTO "v1/mediaItems:batchCreate", headers, NULL, jbody, strlen(jbody));

  g_free(jbody);

  // check that the upload was correct and return the photo_id

  if(response)
  {
    if(json_object_has_member(response, "newMediaItemResults"))
    {
      JsonArray *results = json_object_get_array_member(response, "newMediaItemResults");
      // get first element, we have uploaded a single pciture
      JsonObject *root = json_array_get_object_element(results, 0);
      JsonObject *o = root;
      if(json_object_has_member(o, "status"))
      {
        o = json_node_get_object(json_object_get_member(o, "status"));
        if(json_object_has_member(o, "message"))
        {
          if(g_strcmp0(json_object_get_string_member(o, "message"), "OK"))
            return NULL;
        }
        else
          return NULL;
      }
      else
        return NULL;

      if(json_object_has_member(root, "mediaItem"))
      {
        o = json_node_get_object(json_object_get_member(root, "mediaItem"));
        if(json_object_has_member(o, "id"))
          photo_id = g_strdup(json_object_get_string_member(o, "id"));
      }
    }
  }

  return photo_id;
}

/**
 * @see https://developers.google.com/accounts/docs/OAuth2InstalledApp#callinganapi
 * @return basic information about the account
 */
static dt_gphoto_account_info_t *gphoto_get_account_info(dt_gphoto_context_t *ctx)
{
  JsonObject *obj = gphoto_query_get(ctx, GOOGLE_API_BASE_URL "oauth2/v1/userinfo", NULL);
  g_return_val_if_fail((obj != NULL), NULL);
  /* Using the email instead of the username as it is unique */
  /* To change it to use the username, change "email" by "name" */
  const gchar *user_name = json_object_get_string_member(obj, "given_name");
  const gchar *email = json_object_get_string_member(obj, "email");
  const gchar *user_id = json_object_get_string_member(obj, "id");
  g_return_val_if_fail(user_name != NULL && user_id != NULL, NULL);

  gchar *name = NULL;
  name = dt_util_dstrcat(name, "%s - %s", user_name, email);

  dt_gphoto_account_info_t *accountinfo = gphoto_account_info_init();
  accountinfo->id = g_strdup(user_id);
  accountinfo->username = g_strdup(name);
  accountinfo->token = g_strdup(ctx->token);
  accountinfo->refresh_token = g_strdup(ctx->refresh_token);

  g_snprintf(ctx->userid, sizeof(ctx->userid), "%s", user_id);
  g_free(name);
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

static gchar *gphoto_get_user_refresh_token(dt_gphoto_context_t *ctx)
{
  gchar *refresh_token = NULL;
  JsonObject *reply;
  gchar *params = NULL;

  params = dt_util_dstrcat(params, "refresh_token=%s&client_id=%s&client_secret=%s&grant_type=refresh_token",
                           ctx->refresh_token, ctx->google_client_id, ctx->google_client_secret);

  reply = gphoto_query_post_auth(ctx, GOOGLE_API_BASE_URL "oauth2/v4/token", params);

  refresh_token = g_strdup(json_object_get_string_member(reply, "access_token"));

  g_free(params);

  return refresh_token;
}

/**
 * @see https://developers.google.com/accounts/docs/OAuth2InstalledApp
 * @returns NULL if the user cancels the operation or a valid token
 */
static int gphoto_get_user_auth_token(dt_storage_gphoto_gui_data_t *ui)
{
  ///////////// open the authentication url in a browser
  GError *error = NULL;
  gchar *params = NULL;
  params = dt_util_dstrcat(params,
                           GOOGLE_WS_BASE_URL
                           "o/oauth2/v2/auth?"
                           "client_id=%s&redirect_uri=urn:ietf:wg:oauth:2.0:oob"
                           "&scope=" GOOGLE_API_BASE_URL "auth/photoslibrary "
                           GOOGLE_API_BASE_URL "auth/userinfo.profile "
                           GOOGLE_API_BASE_URL "auth/userinfo.email"
                           "&response_type=code&access_type=offline",
                           ui->gphoto_api->google_client_id);

  if(!gtk_show_uri(gdk_screen_get_default(), params, gtk_get_current_event_time(), &error))
  {
    fprintf(stderr, "[gphoto] error opening browser: %s\n", error->message);
    g_error_free(error);
  }

  ////////////// build & show the validation dialog
  const gchar *text1 = _("step 1: a new window or tab of your browser should have been "
                         "loaded. you have to login into your google account there "
                         "and authorize darktable to upload photos before continuing.");
  const gchar *text2 = _("step 2: paste the verification code shown to you in the browser "
                         "and click the OK button once you are done.");

  GtkWidget *window = dt_ui_main_window(darktable.gui->ui);
  GtkDialog *gphoto_auth_dialog = GTK_DIALOG(
      gtk_message_dialog_new(GTK_WINDOW(window), GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_QUESTION,
                             GTK_BUTTONS_OK_CANCEL, _("google authentication")));
#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(GTK_WIDGET(gphoto_auth_dialog));
#endif
  gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(gphoto_auth_dialog), "%s\n\n%s", text1, text2);

  GtkWidget *entry = gtk_entry_new();
  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(gtk_label_new(_("verification code:"))), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(entry), TRUE, TRUE, 0);

  GtkWidget *gphotoauthdialog_vbox
      = gtk_message_dialog_get_message_area(GTK_MESSAGE_DIALOG(gphoto_auth_dialog));
  gtk_box_pack_end(GTK_BOX(gphotoauthdialog_vbox), hbox, TRUE, TRUE, 0);

  gtk_widget_show_all(GTK_WIDGET(gphoto_auth_dialog));

  ////////////// wait for the user to enter the verification code
  gint result;
  gchar *token = NULL;
  const char *replycode;
  while(TRUE)
  {
    result = gtk_dialog_run(GTK_DIALOG(gphoto_auth_dialog));
    if(result == GTK_RESPONSE_CANCEL) break;
    replycode = gtk_entry_get_text(GTK_ENTRY(entry));
    if(replycode == NULL || g_strcmp0(replycode, "") == 0)
    {
      gtk_message_dialog_format_secondary_markup(GTK_MESSAGE_DIALOG(gphoto_auth_dialog),
                                                 "%s\n\n%s\n\n<span foreground=\"" MSGCOLOR_RED
                                                 "\" ><small>%s</small></span>",
                                                 text1, text2, _("please enter the verification code"));
      continue;
    }
    else
    {
      token = g_strdup(replycode);
      break;
    }
  }
  gtk_widget_destroy(GTK_WIDGET(gphoto_auth_dialog));
  g_free(params);

  if(result == GTK_RESPONSE_CANCEL)
    return 1;

  // Interchange now the authorization_code for an access_token and refresh_token
  JsonObject *reply;

  params = NULL;
  params = dt_util_dstrcat(params, "code=%s&client_id=%s&client_secret=%s"
                           "&redirect_uri=" GOOGLE_URI "&grant_type=authorization_code",
                           token, ui->gphoto_api->google_client_id, ui->gphoto_api->google_client_secret);

  g_free(token);

  reply = gphoto_query_post_auth(ui->gphoto_api, GOOGLE_WS_BASE_URL "o/oauth2/token", params);

  gchar *access_token = g_strdup(json_object_get_string_member(reply, "access_token"));

  gchar *refresh_token = g_strdup(json_object_get_string_member(reply, "refresh_token"));

  ui->gphoto_api->token = access_token;
  ui->gphoto_api->refresh_token = refresh_token;

  g_free(params);

  return 0; // FIXME
}


static void load_account_info_fill(gchar *key, gchar *value, GSList **accountlist)
{
  JsonParser *parser = json_parser_new();
  json_parser_load_from_data(parser, value, strlen(value), NULL);
  JsonNode *root = json_parser_get_root(parser);

  // defensive check, root can be null while parsing the account info
  if(root)
  {
    JsonObject *obj = json_node_get_object(root);
    dt_gphoto_account_info_t *info = gphoto_account_info_init();
    info->id = g_strdup(key);
    info->token = g_strdup(json_object_get_string_member(obj, "token"));
    info->username = g_strdup(json_object_get_string_member(obj, "username"));
    info->id = g_strdup(json_object_get_string_member(obj, "userid"));
    info->refresh_token = g_strdup(json_object_get_string_member(obj, "refresh_token"));
    *accountlist = g_slist_prepend(*accountlist, info);
  }
  g_object_unref(parser);
}

/**
 * @return a GSList of saved dt_gphoto_account_info_t
 */
static GSList *load_account_info()
{
  GSList *accountlist = NULL;

  GHashTable *table = dt_pwstorage_get(GPHOTO_STORAGE);
  g_hash_table_foreach(table, (GHFunc)load_account_info_fill, &accountlist);
  g_hash_table_destroy(table);
  return accountlist;
}

static void save_account_info(dt_storage_gphoto_gui_data_t *ui, dt_gphoto_account_info_t *accountinfo)
{
  dt_gphoto_context_t *ctx = ui->gphoto_api;
  g_return_if_fail(ctx != NULL);

  /// serialize data;
  JsonBuilder *builder = json_builder_new();
  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "username");
  json_builder_add_string_value(builder, accountinfo->username);
  json_builder_set_member_name(builder, "userid");
  json_builder_add_string_value(builder, accountinfo->id);
  json_builder_set_member_name(builder, "token");
  json_builder_add_string_value(builder, accountinfo->token);
  json_builder_set_member_name(builder, "refresh_token");
  json_builder_add_string_value(builder, accountinfo->refresh_token);

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

  GHashTable *table = dt_pwstorage_get(GPHOTO_STORAGE);
  g_hash_table_insert(table, g_strdup(accountinfo->id), data);
  dt_pwstorage_set(GPHOTO_STORAGE, table);

  g_hash_table_destroy(table);
}

static void remove_account_info(const gchar *accountid)
{
  GHashTable *table = dt_pwstorage_get(GPHOTO_STORAGE);
  g_hash_table_remove(table, accountid);
  dt_pwstorage_set(GPHOTO_STORAGE, table);
  g_hash_table_destroy(table);
}

static void ui_refresh_users_fill(dt_gphoto_account_info_t *value, gpointer dataptr)
{
  GtkListStore *liststore = GTK_LIST_STORE(dataptr);
  GtkTreeIter iter;
  gtk_list_store_append(liststore, &iter);
  gtk_list_store_set(liststore, &iter, COMBO_USER_MODEL_NAME_COL, value->username, COMBO_USER_MODEL_TOKEN_COL,
                     value->token, COMBO_USER_MODEL_REFRESH_TOKEN_COL, value->refresh_token,
                     COMBO_USER_MODEL_ID_COL, value->id, -1);
}

static void ui_refresh_users(dt_storage_gphoto_gui_data_t *ui)
{
  GSList *accountlist = load_account_info();
  GtkListStore *list_store = GTK_LIST_STORE(gtk_combo_box_get_model(ui->combo_username));
  GtkTreeIter iter;

  gtk_list_store_clear(list_store);
  gtk_list_store_append(list_store, &iter);

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
  }

  g_slist_foreach(accountlist, (GFunc)ui_refresh_users_fill, list_store);
  gtk_combo_box_set_active(ui->combo_username, 0);

  g_slist_free_full(accountlist, (GDestroyNotify)gphoto_account_info_destroy);
  gtk_combo_box_set_row_separator_func(ui->combo_username, combobox_separator, ui->combo_username, NULL);
}

static void ui_refresh_albums_fill(dt_gphoto_album_t *album, GtkListStore *list_store)
{
  GtkTreeIter iter;
  gtk_list_store_append(list_store, &iter);
  gtk_list_store_set(list_store, &iter, COMBO_ALBUM_MODEL_NAME_COL, album->name, COMBO_ALBUM_MODEL_ID_COL,
                     album->id, -1);
}

static void ui_refresh_albums(dt_storage_gphoto_gui_data_t *ui)
{
  gboolean getlistok;
  GList *albumList = gphoto_get_album_list(ui->gphoto_api, &getlistok);
  if(!getlistok)
  {
    dt_control_log(_("unable to retrieve the album list"));
    goto cleanup;
  }

  const int current_index = gtk_combo_box_get_active(ui->combo_album);

  GtkListStore *model_album = GTK_LIST_STORE(gtk_combo_box_get_model(ui->combo_album));
  GtkTreeIter iter;
  gtk_list_store_clear(model_album);
  gtk_list_store_append(model_album, &iter);
  gtk_list_store_set(model_album, &iter, COMBO_ALBUM_MODEL_NAME_COL, _("create new album"),
                     COMBO_ALBUM_MODEL_ID_COL, NULL, -1);
  ui->albums_count = 1;

  if(albumList != NULL)
  {
    gtk_list_store_append(model_album, &iter);
    gtk_list_store_set(model_album, &iter, COMBO_ALBUM_MODEL_NAME_COL, "", COMBO_ALBUM_MODEL_ID_COL, NULL,
                       -1); // separator
    ui->albums_count += g_list_length(albumList);
  }

  g_list_foreach(albumList, (GFunc)ui_refresh_albums_fill, model_album);

  gtk_widget_show_all(GTK_WIDGET(ui->combo_album));

  if (albumList != NULL && current_index > 0)
  {
    gtk_combo_box_set_active(ui->combo_album, current_index);
    gtk_widget_set_no_show_all(GTK_WIDGET(ui->hbox_album), TRUE);
    gtk_widget_hide(GTK_WIDGET(ui->hbox_album));
  }
  else
  {
    gtk_combo_box_set_active(ui->combo_album, 0);
    gtk_widget_set_no_show_all(GTK_WIDGET(ui->hbox_album), FALSE);
    gtk_widget_show_all(GTK_WIDGET(ui->hbox_album));
  }

  g_list_free_full(albumList, (GDestroyNotify)gphoto_album_destroy);
cleanup:
  return;
}

static void ui_combo_username_changed(GtkComboBox *combo, struct dt_storage_gphoto_gui_data_t *ui)
{
  GtkTreeIter iter;
  gchar *token = NULL;
  gchar *refresh_token = NULL;
  gchar *userid = NULL;
  if(!gtk_combo_box_get_active_iter(combo, &iter)) return; // ie: list is empty while clearing the combo
  GtkTreeModel *model = gtk_combo_box_get_model(combo);
  gtk_tree_model_get(model, &iter, COMBO_USER_MODEL_TOKEN_COL, &token, -1); // get the selected token
  gtk_tree_model_get(model, &iter, COMBO_USER_MODEL_REFRESH_TOKEN_COL, &refresh_token, -1);
  gtk_tree_model_get(model, &iter, COMBO_USER_MODEL_ID_COL, &userid, -1);

  ui->gphoto_api->token = g_strdup(token);
  ui->gphoto_api->refresh_token = g_strdup(refresh_token);
  g_snprintf(ui->gphoto_api->userid, sizeof(ui->gphoto_api->userid), "%s", userid);

  if(ui->gphoto_api->token != NULL && gphoto_test_auth_token(ui->gphoto_api))
  {
    ui->connected = TRUE;
    gtk_button_set_label(ui->button_login, _("logout"));
    ui_refresh_albums(ui);
    gtk_widget_set_sensitive(GTK_WIDGET(ui->combo_album), TRUE);
  }
  else
  {
    gtk_button_set_label(ui->button_login, _("login"));
    g_free(ui->gphoto_api->token);
    g_free(ui->gphoto_api->refresh_token);
    ui->gphoto_api->token = ui->gphoto_api->refresh_token = NULL;
    gtk_widget_set_sensitive(GTK_WIDGET(ui->combo_album), FALSE);
    GtkListStore *model_album = GTK_LIST_STORE(gtk_combo_box_get_model(ui->combo_album));
    gtk_list_store_clear(model_album);
  }
}

static void ui_combo_album_changed(GtkComboBox *combo, gpointer data)
{
  dt_storage_gphoto_gui_data_t *ui = (dt_storage_gphoto_gui_data_t *)data;

  const int index = gtk_combo_box_get_active(ui->combo_album);

  if(index == 0)
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

static gboolean ui_authenticate(dt_storage_gphoto_gui_data_t *ui)
{
  if(ui->gphoto_api == NULL)
  {
    ui->gphoto_api = gphoto_api_init();
  }

  dt_gphoto_context_t *ctx = ui->gphoto_api;
  gboolean mustsaveaccount = FALSE;

  gchar *uiselectedaccounttoken = NULL;
  gchar *uiselectedaccountrefreshtoken = NULL;
  gchar *uiselecteduserid = NULL;
  GtkTreeIter iter;
  gtk_combo_box_get_active_iter(ui->combo_username, &iter);
  GtkTreeModel *accountModel = gtk_combo_box_get_model(ui->combo_username);
  gtk_tree_model_get(accountModel, &iter, COMBO_USER_MODEL_TOKEN_COL, &uiselectedaccounttoken, -1);
  gtk_tree_model_get(accountModel, &iter, COMBO_USER_MODEL_REFRESH_TOKEN_COL, &uiselectedaccountrefreshtoken,
                     -1);
  gtk_tree_model_get(accountModel, &iter, COMBO_USER_MODEL_ID_COL, &uiselecteduserid, -1);

  if(ctx->token != NULL)
  {
    g_free(ctx->token);
    g_free(ctx->refresh_token);
    ctx->userid[0] = 0;
    ctx->token = ctx->refresh_token = NULL;
  }

  if(uiselectedaccounttoken != NULL)
  {
    ctx->token = g_strdup(uiselectedaccounttoken);
    ctx->refresh_token = g_strdup(uiselectedaccountrefreshtoken);
    g_snprintf(ctx->userid, sizeof(ctx->userid), "%s", uiselecteduserid);
  }
  // check selected token if we already have one
  if(ctx->token != NULL && !gphoto_test_auth_token(ctx))
  {
    g_free(ctx->token);
    g_free(ctx->refresh_token);
    ctx->userid[0] = 0;
    ctx->token = ctx->refresh_token = NULL;
  }

  int ret = 0;
  if(ctx->token == NULL)
  {
    mustsaveaccount = TRUE;
    ret = gphoto_get_user_auth_token(ui); // ask user to log in
  }

  if(ctx->token == NULL || ctx->refresh_token == NULL || ret != 0)
  {
    return FALSE;
  }
  else
  {
    if(mustsaveaccount)
    {
      // Get first the refresh token
      dt_gphoto_account_info_t *accountinfo = gphoto_get_account_info(ui->gphoto_api);
      g_return_val_if_fail(accountinfo != NULL, FALSE);
      save_account_info(ui, accountinfo);

      // add account to user list and select it
      GtkListStore *model = GTK_LIST_STORE(gtk_combo_box_get_model(ui->combo_username));
      gboolean r;
      gchar *uid;

      gboolean updated = FALSE;

      for(r = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(model), &iter); r == TRUE;
          r = gtk_tree_model_iter_next(GTK_TREE_MODEL(model), &iter))
      {
        gtk_tree_model_get(GTK_TREE_MODEL(model), &iter, COMBO_USER_MODEL_ID_COL, &uid, -1);

        if(g_strcmp0(uid, accountinfo->id) == 0)
        {
          gtk_list_store_set(model, &iter, COMBO_USER_MODEL_NAME_COL, accountinfo->username,
                             COMBO_USER_MODEL_TOKEN_COL, accountinfo->token,
                             COMBO_USER_MODEL_REFRESH_TOKEN_COL, accountinfo->refresh_token, -1);
          updated = TRUE;
          break;
        }
      }

      if(!updated)
      {
        gtk_list_store_append(model, &iter);
        gtk_list_store_set(model, &iter, COMBO_USER_MODEL_NAME_COL, accountinfo->username,
                           COMBO_USER_MODEL_TOKEN_COL, accountinfo->token, COMBO_USER_MODEL_REFRESH_TOKEN_COL,
                           accountinfo->refresh_token, COMBO_USER_MODEL_ID_COL, accountinfo->id, -1);
      }
      g_signal_handlers_block_matched(ui->combo_username, G_SIGNAL_MATCH_FUNC, 0, 0, NULL,
                                      ui_combo_username_changed, NULL);
      gtk_combo_box_set_active_iter(ui->combo_username, &iter);
      g_signal_handlers_unblock_matched(ui->combo_username, G_SIGNAL_MATCH_FUNC, 0, 0, NULL,
                                        ui_combo_username_changed, NULL);

      gphoto_account_info_destroy(accountinfo);
    }
    return TRUE;
  }
}


static void ui_login_clicked(GtkButton *button, gpointer data)
{
  dt_storage_gphoto_gui_data_t *ui = (dt_storage_gphoto_gui_data_t *)data;
  gtk_widget_set_sensitive(GTK_WIDGET(ui->combo_album), FALSE);
  if(ui->connected == FALSE)
  {
    if(ui_authenticate(ui))
    {
      ui_refresh_albums(ui);
      ui->connected = TRUE;
      gtk_button_set_label(ui->button_login, _("logout"));
    }
    else
    {
      gtk_button_set_label(ui->button_login, _("login"));
    }
  }
  else // disconnect user
  {
    if(ui->connected == TRUE && ui->gphoto_api->token != NULL)
    {
      GtkTreeModel *model = gtk_combo_box_get_model(ui->combo_username);
      GtkTreeIter iter;
      gtk_combo_box_get_active_iter(ui->combo_username, &iter);
      gchar *userid;
      gtk_tree_model_get(model, &iter, COMBO_USER_MODEL_ID_COL, &userid, -1);
      remove_account_info(userid);
      gtk_button_set_label(ui->button_login, _("login"));
      ui_refresh_users(ui);
      ui->connected = FALSE;
    }
  }
  gtk_widget_set_sensitive(GTK_WIDGET(ui->combo_album), TRUE);
}



////////////////////////// darktable library interface

/* plugin name */
const char *name(const struct dt_imageio_module_storage_t *self)
{
  return _("google photos");
}

/* construct widget above */
void gui_init(struct dt_imageio_module_storage_t *self)
{
  self->gui_data = g_malloc0(sizeof(dt_storage_gphoto_gui_data_t));
  dt_storage_gphoto_gui_data_t *ui = self->gui_data;
  ui->gphoto_api = gphoto_api_init();

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  // create labels
  ui->label_album_title = GTK_LABEL(gtk_label_new(_("title")));
  ui->label_status = GTK_LABEL(gtk_label_new(NULL));

  gtk_widget_set_halign(GTK_WIDGET(ui->label_album_title), GTK_ALIGN_START);

  // create entries
  GtkListStore *model_username
      = gtk_list_store_new(COMBO_USER_MODEL_NB_COL, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                           G_TYPE_STRING); // text, token, refresh_token, id
  ui->combo_username = GTK_COMBO_BOX(gtk_combo_box_new_with_model(GTK_TREE_MODEL(model_username)));
  GtkCellRenderer *p_cell = gtk_cell_renderer_text_new();
  g_object_set(G_OBJECT(p_cell), "ellipsize", PANGO_ELLIPSIZE_MIDDLE, "ellipsize-set", TRUE, "width-chars", 35,
               (gchar *)0);
  gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(ui->combo_username), p_cell, FALSE);
  gtk_cell_layout_set_attributes(GTK_CELL_LAYOUT(ui->combo_username), p_cell, "text", 0, NULL);

  ui->entry_album_title = GTK_ENTRY(gtk_entry_new());

  dt_gui_key_accel_block_on_focus_connect(GTK_WIDGET(ui->combo_username));
  dt_gui_key_accel_block_on_focus_connect(GTK_WIDGET(ui->entry_album_title));

  // retrieve saved accounts
  ui_refresh_users(ui);

  //////// album list /////////
  GtkWidget *albumlist = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  GtkListStore *model_album
      = gtk_list_store_new(COMBO_ALBUM_MODEL_NB_COL, G_TYPE_STRING, G_TYPE_STRING); // name, id
  ui->combo_album = GTK_COMBO_BOX(gtk_combo_box_new_with_model(GTK_TREE_MODEL(model_album)));
  p_cell = gtk_cell_renderer_text_new();
  g_object_set(G_OBJECT(p_cell), "ellipsize", PANGO_ELLIPSIZE_MIDDLE, "ellipsize-set", TRUE, "width-chars", 35,
               (gchar *)0);
  gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(ui->combo_album), p_cell, FALSE);
  gtk_cell_layout_set_attributes(GTK_CELL_LAYOUT(ui->combo_album), p_cell, "text", 0, NULL);

  gtk_widget_set_sensitive(GTK_WIDGET(ui->combo_album), FALSE);
  gtk_combo_box_set_row_separator_func(ui->combo_album, combobox_separator, ui->combo_album, NULL);
  gtk_box_pack_start(GTK_BOX(albumlist), GTK_WIDGET(ui->combo_album), TRUE, TRUE, 0);

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
  gtk_box_pack_start(GTK_BOX(vbox_auth_fields), GTK_WIDGET(ui->combo_username), TRUE, FALSE, 2);

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

  // connect buttons to signals
  g_signal_connect(G_OBJECT(ui->button_login), "clicked", G_CALLBACK(ui_login_clicked), (gpointer)ui);
  g_signal_connect(G_OBJECT(ui->combo_username), "changed", G_CALLBACK(ui_combo_username_changed),
                   (gpointer)ui);
  g_signal_connect(G_OBJECT(ui->combo_album), "changed", G_CALLBACK(ui_combo_album_changed), (gpointer)ui);

  g_object_unref(model_username);
  g_object_unref(model_album);
}

/* destroy resources */
void gui_cleanup(struct dt_imageio_module_storage_t *self)
{
  dt_storage_gphoto_gui_data_t *ui = self->gui_data;
  dt_gui_key_accel_block_on_focus_disconnect(GTK_WIDGET(ui->combo_username));
  dt_gui_key_accel_block_on_focus_disconnect(GTK_WIDGET(ui->entry_album_title));

  if(ui->gphoto_api != NULL) gphoto_api_destroy(ui->gphoto_api);
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
  if(strcmp(format->mime(NULL), "image/jpeg") == 0) return 1;
  return 0;
}

/* this actually does the work */
int store(dt_imageio_module_storage_t *self, struct dt_imageio_module_data_t *sdata, const int imgid,
          dt_imageio_module_format_t *format, dt_imageio_module_data_t *fdata, const int num, const int total,
          const gboolean high_quality, const gboolean upscale, dt_colorspaces_color_profile_type_t icc_type,
          const gchar *icc_filename, dt_iop_color_intent_t icc_intent)
{
  dt_storage_gphoto_gui_data_t *ui = self->gui_data;

  gint result = 1;
  dt_gphoto_context_t *ctx = (dt_gphoto_context_t *)sdata;

  const char *ext = format->extension(fdata);
  char fname[PATH_MAX] = { 0 };
  dt_loc_get_tmp_dir(fname, sizeof(fname));
  g_strlcat(fname, "/darktable.XXXXXX.", sizeof(fname));
  g_strlcat(fname, ext, sizeof(fname));

  gint fd = g_mkstemp(fname);
  if(fd == -1)
  {
    dt_control_log("failed to create temporary image for google photos export");
    return 1;
  }
  close(fd);

  // get metadata
  const dt_image_t *img = dt_image_cache_get(darktable.image_cache, imgid, 'r');
  char *title = NULL;
  char *summary = NULL;
  GList *meta_title = NULL;

  title = g_path_get_basename(img->filename);
  (g_strrstr(title, "."))[0] = '\0'; // Chop extension...

  meta_title = dt_metadata_get(img->id, "Xmp.dc.title", NULL);
  summary = meta_title != NULL ? meta_title->data : "";

  dt_image_cache_read_release(darktable.image_cache, img);

  if(dt_imageio_export(imgid, fname, format, fdata, high_quality, upscale, FALSE, icc_type, icc_filename, icc_intent,
                       self, sdata, num, total) != 0)
  {
    g_printerr("[gphoto] could not export to file: `%s'!\n", fname);
    dt_control_log(_("could not export to file `%s'!"), fname);
    result = 0;
    goto cleanup;
  }

  if(!*(ctx->album_id))
  {
    if(ctx->album_title == NULL)
    {
      dt_control_log(_("unable to create album, no title provided"));
      result = 0;
      goto cleanup;
    }
    const gchar *album_id  = gphoto_create_album(ui, ctx, ctx->album_title);
    if(album_id == NULL)
    {
      dt_control_log(_("unable to create album"));
      result = 0;
      goto cleanup;
    }
    g_snprintf(ctx->album_id, sizeof(ctx->album_id), "%s", album_id);
  }

  const char *photoid = gphoto_upload_photo_to_album(ctx, ctx->album_id, fname, title, summary, imgid);
  if(photoid == NULL)
  {
    dt_control_log(_("unable to export to google photos album"));
    result = 0;
    goto cleanup;
  }

cleanup:
  g_unlink(fname);
  g_free(title);
  g_list_free_full(meta_title, &g_free);

  if(result)
  {
    // this makes sense only if the export was successful
    dt_control_log(ngettext("%d/%d exported to google photos album", "%d/%d exported to google photos album", num), num, total);
  }
  return 0;
}

static gboolean _finalize_store(gpointer user_data)
{
  dt_storage_gphoto_gui_data_t *ui = (dt_storage_gphoto_gui_data_t *)user_data;
  ui_reset_albums_creation(ui);

  return FALSE;
}

void finalize_store(struct dt_imageio_module_storage_t *self, dt_imageio_module_data_t *data)
{
  g_main_context_invoke(NULL, _finalize_store, self->gui_data);
}

size_t params_size(dt_imageio_module_storage_t *self)
{
  return sizeof(dt_gphoto_context_t) - 8 * sizeof(void *);
}

void init(dt_imageio_module_storage_t *self)
{
}
void *get_params(struct dt_imageio_module_storage_t *self)
{
  dt_storage_gphoto_gui_data_t *ui = (dt_storage_gphoto_gui_data_t *)self->gui_data;
  if(!ui) return NULL; // gui not initialized, CLI mode
  if(ui->gphoto_api == NULL || ui->gphoto_api->token == NULL)
  {
    return NULL;
  }
  dt_gphoto_context_t *p = (dt_gphoto_context_t *)g_malloc0(sizeof(dt_gphoto_context_t));

  p->curl_ctx = ui->gphoto_api->curl_ctx;
  p->json_parser = ui->gphoto_api->json_parser;
  p->errmsg = ui->gphoto_api->errmsg;
  p->token = g_strdup(ui->gphoto_api->token);
  p->refresh_token = g_strdup(ui->gphoto_api->refresh_token);

  int index = gtk_combo_box_get_active(ui->combo_album);
  if(index < 0)
  {
    gphoto_api_destroy(p);
    return NULL;
  }
  else if(index == 0)
  {
    p->album_id[0] = 0;
    p->album_title = g_strdup(gtk_entry_get_text(ui->entry_album_title));

    /* Hardcode the album as private, to avoid problems with the old Gphoto interface */
    p->album_permission = 1;
  }
  else
  {
    GtkTreeModel *model = gtk_combo_box_get_model(ui->combo_album);
    GtkTreeIter iter;
    gchar *albumid = NULL;
    gtk_combo_box_get_active_iter(ui->combo_album, &iter);
    gtk_tree_model_get(model, &iter, COMBO_ALBUM_MODEL_ID_COL, &albumid, -1);
    g_snprintf(p->album_id, sizeof(p->album_id), "%s", albumid);
  }

  g_snprintf(p->userid, sizeof(p->userid), "%s", ui->gphoto_api->userid);

  // recreate a new context for further usages
  ui->gphoto_api = gphoto_api_init();
  ui->gphoto_api->token = g_strdup(p->token);
  ui->gphoto_api->refresh_token = g_strdup(p->refresh_token);
  g_snprintf(ui->gphoto_api->userid, sizeof(ui->gphoto_api->userid), "%s", p->userid);

  return p;
}

void free_params(struct dt_imageio_module_storage_t *self, dt_imageio_module_data_t *data)
{
  if(!data) return;
  dt_gphoto_context_t *ctx = (dt_gphoto_context_t *)data;
  gphoto_api_destroy(ctx);
}

int set_params(struct dt_imageio_module_storage_t *self, const void *params, const int size)
{
  if(size != self->params_size(self)) return 1;

  dt_gphoto_context_t *d = (dt_gphoto_context_t *)params;
  dt_storage_gphoto_gui_data_t *g = (dt_storage_gphoto_gui_data_t *)self->gui_data;

  g_snprintf(g->gphoto_api->album_id, sizeof(g->gphoto_api->album_id), "%s", d->album_id);
  g_snprintf(g->gphoto_api->userid, sizeof(g->gphoto_api->userid), "%s", d->userid);

  GtkListStore *model = GTK_LIST_STORE(gtk_combo_box_get_model(g->combo_username));
  GtkTreeIter iter;
  gboolean r;
  gchar *uid = NULL;
  gchar *albumid = NULL;

  for(r = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(model), &iter); r == TRUE;
      r = gtk_tree_model_iter_next(GTK_TREE_MODEL(model), &iter))
  {
    gtk_tree_model_get(GTK_TREE_MODEL(model), &iter, COMBO_USER_MODEL_ID_COL, &uid, -1);
    if(g_strcmp0(uid, g->gphoto_api->userid) == 0)
    {
      gtk_combo_box_set_active_iter(g->combo_username, &iter);
      break;
    }
  }
  g_free(uid);

  model = GTK_LIST_STORE(gtk_combo_box_get_model(g->combo_album));
  for(r = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(model), &iter); r == TRUE;
      r = gtk_tree_model_iter_next(GTK_TREE_MODEL(model), &iter))
  {
    gtk_tree_model_get(GTK_TREE_MODEL(model), &iter, COMBO_ALBUM_MODEL_ID_COL, &albumid, -1);
    if(g_strcmp0(albumid, g->gphoto_api->album_id) == 0)
    {
      gtk_combo_box_set_active_iter(g->combo_album, &iter);
      break;
    }
  }
  g_free(albumid);

  return 0;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
