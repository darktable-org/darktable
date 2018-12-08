/*
    This file is part of darktable,
    copyright (c) 2012 Pierre Lamot
    copyright (c) 2013-2014 Jose Carlos Garcia Sogo

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
#include <libxml/parser.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

DT_MODULE(2)

#define GOOGLE_WS_BASE_URL "https://accounts.google.com/"
#define GOOGLE_API_BASE_URL "https://www.googleapis.com/"
#define GOOGLE_API_KEY "428088086479.apps.googleusercontent.com"
#define GOOGLE_API_SECRET "tIIL4FUs46Nc9nQWKeg3H_Hy"
#define GOOGLE_URI "urn:ietf:wg:oauth:2.0:oob"
#define GOOGLE_PICASA "https://picasaweb.google.com/"

#define GOOGLE_IMAGE_MAX_SIZE 960

#define MSGCOLOR_RED "#e07f7f"
#define MSGCOLOR_GREEN "#7fe07f"

/** Authenticate against google picasa service*/
typedef struct _buffer_t
{
  char *data;
  size_t size;
  size_t offset;
} _buffer_t;

typedef enum ComboUserModel
{
  COMBO_USER_MODEL_NAME_COL = 0,
  COMBO_USER_MODEL_TOKEN_COL,
  COMBO_USER_MODEL_REFRESH_TOKEN_COL,
  COMBO_USER_MODEL_ID_COL,
  COMBO_USER_MODEL_NB_COL
} ComboUserModel;

typedef enum ComboAlbumModel
{
  COMBO_ALBUM_MODEL_NAME_COL = 0,
  COMBO_ALBUM_MODEL_ID_COL,
  COMBO_ALBUM_MODEL_NB_COL
} ComboAlbumModel;

typedef enum PicasaAlbumPrivacyPolicy
{
  PICASA_ALBUM_PRIVACY_PUBLIC,
  PICASA_ALBUM_PRIVACY_PRIVATE,
} PicasaAlbumPrivacyPolicy;

/**
 * Represents information about an album
 */
typedef struct PicasaAlbum
{
  gchar *id;
  gchar *name;
  PicasaAlbumPrivacyPolicy privacy;
} PicasaAlbum;

static PicasaAlbum *picasa_album_init()
{
  return (PicasaAlbum *)g_malloc0(sizeof(PicasaAlbum));
}

static void picasa_album_destroy(PicasaAlbum *album)
{
  if(album == NULL) return;
  g_free(album->id);
  g_free(album->name);
  g_free(album);
}

/**
 * Represents information about an account
 */
typedef struct PicasaAccountInfo
{
  gchar *id;
  gchar *username;
  gchar *token;
  gchar *refresh_token;
} PicasaAccountInfo;

static PicasaAccountInfo *picasa_account_info_init()
{
  return (PicasaAccountInfo *)g_malloc0(sizeof(PicasaAccountInfo));
}

static void picasa_account_info_destroy(PicasaAccountInfo *account)
{
  if(account == NULL) return;
  g_free(account->id);
  g_free(account->username);
  g_free(account);
}

typedef struct PicasaContext
{
  gchar album_id[1024];
  gchar userid[1024];

  int album_permission;

  /// curl context
  CURL *curl_ctx;
  /// Json parser context
  JsonParser *json_parser;

  GString *errmsg;

  /// authorization token
  gchar *token;
  gchar *refresh_token;
} PicasaContext;

typedef struct dt_storage_picasa_gui_data_t
{
  // == ui elements ==
  GtkLabel *label_status;

  GtkComboBox *comboBox_username;
  GtkButton *button_login;

  GtkDarktableButton *dtbutton_refresh_album;
  GtkComboBox *comboBox_album;

  // == context ==
  gboolean connected;
  PicasaContext *picasa_api;
} dt_storage_picasa_gui_data_t;


static PicasaContext *picasa_api_init()
{
  PicasaContext *ctx = (PicasaContext *)g_malloc0(sizeof(PicasaContext));
  ctx->curl_ctx = curl_easy_init();
  ctx->errmsg = g_string_new("");
  ctx->json_parser = json_parser_new();
  return ctx;
}

static void picasa_api_destroy(PicasaContext *ctx)
{
  if(ctx == NULL) return;
  curl_easy_cleanup(ctx->curl_ctx);
  g_free(ctx->token);
  g_free(ctx->refresh_token);
  g_object_unref(ctx->json_parser);
  g_string_free(ctx->errmsg, TRUE);
  g_free(ctx);
}


typedef struct dt_storage_picasa_param_t
{
  gint64 hash;
  PicasaContext *picasa_ctx;
} dt_storage_picasa_param_t;


static gchar *picasa_get_user_refresh_token(PicasaContext *ctx);

//////////////////////////// curl requests related functions

/** Grow and fill _buffer_t with received data... */
static size_t _picasa_api_buffer_write_func(void *ptr, size_t size, size_t nmemb, void *stream)
{
  _buffer_t *buffer = (_buffer_t *)stream;
  char *newdata = g_malloc0(buffer->size + nmemb + 1);
  if(buffer->data != NULL) memcpy(newdata, buffer->data, buffer->size);
  memcpy(newdata + buffer->size, ptr, nmemb);
  g_free(buffer->data);
  buffer->data = newdata;
  buffer->size += nmemb;
  return nmemb;
}

static size_t _picasa_api_buffer_read_func(void *ptr, size_t size, size_t nmemb, void *stream)
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
#ifdef picasa_EXTRA_VERBOSE
  g_printf("server reply: %s\n", string->str);
#endif
  return size * nmemb;
}

static JsonObject *picasa_parse_response(PicasaContext *ctx, GString *response)
{
  GError *error = NULL;
  gboolean ret = json_parser_load_from_data(ctx->json_parser, response->str, response->len, &error);
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


static void picasa_query_get_add_url_arguments(GString *key, GString *value, GString *url)
{
  g_string_append(url, "&");
  g_string_append(url, key->str);
  g_string_append(url, "=");
  g_string_append(url, value->str);
}

/**
 * perform a GET request on picasa/google api
 *
 * @note use this one to read information (user info, existing albums, ...)
 *
 * @param ctx picasa context (token field must be set)
 * @param method the method to call on the google API, the methods should not start with '/' example:
 *"me/albums"
 * @param args hashtable of the arguments to be added to the requests, must be in the form key (string) =
 *value (string)
 * @returns NULL if the request fails, or a JsonObject of the reply
 */
static JsonObject *picasa_query_get(PicasaContext *ctx, const gchar *method, GHashTable *args, gboolean picasa)
{
  g_return_val_if_fail(ctx != NULL, NULL);
  g_return_val_if_fail(ctx->token != NULL, NULL);
  // build the query
  GString *url;
  if(picasa == TRUE)
    url = g_string_new(GOOGLE_PICASA);
  else
    url = g_string_new(GOOGLE_API_BASE_URL);

  g_string_append(url, method);

  if(picasa == TRUE)
  {
    g_string_append(url, "?alt=json&access_token=");
    g_string_append(url, ctx->token);
  }
  else
  {
    g_string_append(url, "?alt=json&access_token=");
    g_string_append(url, ctx->token);
  }
  if(args != NULL) g_hash_table_foreach(args, (GHFunc)picasa_query_get_add_url_arguments, url);

  // send the request
  GString *response = g_string_new("");
  curl_easy_reset(ctx->curl_ctx);
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_URL, url->str);
#ifdef picasa_EXTRA_VERBOSE
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
  JsonObject *respobj = picasa_parse_response(ctx, response);

  g_string_free(response, TRUE);
  g_string_free(url, TRUE);
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
 * @param ctx picasa context (token field must be set)
 * @param method the method to call on the google API, the methods should not start with '/' example:
 *"me/albums"
 * @param args hashtable of the arguments to be added to the requests, might be null if none
 * @returns NULL if the request fails, or a JsonObject of the reply
 */

static JsonObject *picasa_query_post_auth(PicasaContext *ctx, const gchar *method, gchar *args)
{
  g_return_val_if_fail(ctx != NULL, NULL);

  GString *url = NULL;

  url = g_string_new(GOOGLE_WS_BASE_URL);
  g_string_append(url, method);

  // send the requests
  GString *response = g_string_new("");
  curl_easy_reset(ctx->curl_ctx);
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_URL, url->str);
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_POST, 1);
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_COPYPOSTFIELDS, args);
#ifdef picasa_EXTRA_VERBOSE
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_VERBOSE, 2);
#endif
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_SSL_VERIFYPEER, FALSE);
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_WRITEFUNCTION, curl_write_data_cb);
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_WRITEDATA, response);

  int res = curl_easy_perform(ctx->curl_ctx);
  g_string_free(url, TRUE);
  if(res != CURLE_OK) return NULL;
  // parse the response
  JsonObject *respobj = picasa_parse_response(ctx, response);
  g_string_free(response, TRUE);
  return respobj;
}

//////////////////////////// picasa api functions

/**
 * @returns TRUE if the current token is valid
 */
static gboolean picasa_test_auth_token(PicasaContext *ctx)
{
  gchar *access_token = NULL;
  access_token = picasa_get_user_refresh_token(ctx);

  if(access_token != NULL) ctx->token = access_token;

  return access_token != NULL;
}

/**
 * @return a GList of PicasaAlbums associated to the user
 */
static GList *picasa_get_album_list(PicasaContext *ctx, gboolean *ok)
{
  if(!ok) return NULL;

  *ok = TRUE;
  GList *album_list = NULL;

  JsonObject *reply = picasa_query_get(ctx, "data/feed/api/user/default", NULL, TRUE);
  if(reply == NULL) goto error;

  JsonObject *feed = json_object_get_object_member(reply, "feed");
  if(feed == NULL) goto error;

  JsonArray *jsalbums = json_object_get_array_member(feed, "entry");

  guint i;
  for(i = 0; i < json_array_get_length(jsalbums); i++)
  {
    JsonObject *obj = json_array_get_object_element(jsalbums, i);
    if(obj == NULL) continue;

    PicasaAlbum *album = picasa_album_init();
    if(album == NULL) goto error;

    JsonObject *jsid = json_object_get_object_member(obj, "gphoto$id");
    JsonObject *jstitle = json_object_get_object_member(obj, "title");

    const char *id = json_object_get_string_member(jsid, "$t");
    const char *name = json_object_get_string_member(jstitle, "$t");
    if(id == NULL || name == NULL)
    {
      picasa_album_destroy(album);
      goto error;
    }
    album->id = g_strdup(id);
    album->name = g_strdup(name);
    album_list = g_list_append(album_list, album);
  }
  return album_list;

error:
  *ok = FALSE;
  g_list_free_full(album_list, (GDestroyNotify)picasa_album_destroy);
  return NULL;
}

/**
 * @see https://developers.google.com/picasa-web/docs/2.0/developers_guide_protocol#PostPhotos
 * @return the id of the uploaded photo
 */
static const gchar *picasa_upload_photo_to_album(PicasaContext *ctx, gchar *albumid, gchar *fname,
                                                 gchar *title, gchar *summary, const int imgid)
{
  _buffer_t buffer = { 0 };
  gchar *photo_id = NULL;

  char uri[4096] = { 0 };

  // Open the temp file and read image to memory
  GMappedFile *imgfile = g_mapped_file_new(fname, FALSE, NULL);
  const int size = g_mapped_file_get_length(imgfile);
  gchar *data = g_mapped_file_get_contents(imgfile);

  gchar *entry = g_markup_printf_escaped("<entry xmlns='http://www.w3.org/2005/Atom'>\n"
                                         "<title>%s</title>\n"
                                         "<summary>%s</summary>\n"
                                         "<category scheme=\"http://schemas.google.com/g/2005#kind\"\n"
                                         " term=\"http://schemas.google.com/photos/2007#photo\"/>"
                                         "</entry>",
                                         title, summary);

  gchar *authHeader = NULL;
  authHeader = dt_util_dstrcat(authHeader, "Authorization: OAuth %s", ctx->token);

  // Hack for nonform multipart post...
  gchar mpart1[4096] = { 0 };
  gchar *mpart_format = "\nMedia multipart posting\n--END_OF_PART\nContent-Type: "
                        "application/atom+xml\n\n%s\n--END_OF_PART\nContent-Type: image/jpeg\n\n";
  snprintf(mpart1, sizeof(mpart1), mpart_format, entry);
  g_free(entry);

  const int mpart1size = strlen(mpart1);
  const int postdata_length = mpart1size + size + strlen("\n--END_OF_PART--");
  gchar *postdata = g_malloc(postdata_length);
  memcpy(postdata, mpart1, mpart1size);
  memcpy(postdata + mpart1size, data, size);
  memcpy(postdata + mpart1size + size, "\n--END_OF_PART--", strlen("\n--END_OF_PART--"));

  g_mapped_file_unref(imgfile);

  struct curl_slist *headers = NULL;
  headers = curl_slist_append(headers, "Content-Type: multipart/related; boundary=\"END_OF_PART\"");
  headers = curl_slist_append(headers, "MIME-version: 1.0");
  headers = curl_slist_append(headers, "GData-Version: 3");
  headers = curl_slist_append(headers, authHeader);

  snprintf(uri, sizeof(uri), "https://picasaweb.google.com/data/feed/api/user/default/albumid/%s", albumid);
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_URL, uri);
#ifdef picasa_EXTRA_VERBOSE
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_VERBOSE, 2);
#endif
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_UPLOAD, 0); // A post request !
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_POST, 1);
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_FOLLOWLOCATION, 1);
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_POSTFIELDS, postdata);
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_POSTFIELDSIZE, postdata_length);
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_WRITEFUNCTION, _picasa_api_buffer_write_func);
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_WRITEDATA, &buffer);

  int res = curl_easy_perform(ctx->curl_ctx);

  curl_slist_free_all(headers);

  if(res != CURLE_OK)
  {
    fprintf(stderr, "[picasa] error uploading photo: %s\n", curl_easy_strerror(res));
    return NULL;
  }

#ifdef picasa_EXTRA_VERBOSE
  printf("Uploading: %s\n", buffer.data);
#endif

  long result;
  curl_easy_getinfo(ctx->curl_ctx, CURLINFO_RESPONSE_CODE, &result);

  // If we want to add tags let's do...
  if(result == 201 && imgid > 0)
  {
    // Image was created , fine.. and result have the fully created photo xml entry..
    // Let's perform an update of the photos keywords with tags passed along to this function..
    // and use picasa photo update api to add keywords to the photo...

    // Build the keywords content string
    GList *keywords_list = dt_tag_get_list(imgid);
    gchar *keywords = dt_util_glist_to_str(",", keywords_list);

    xmlDocPtr doc;
    xmlNodePtr entryNode;
    // Parse xml document
    if((doc = xmlParseDoc((xmlChar *)buffer.data)) == NULL) return 0;
    entryNode = xmlDocGetRootElement(doc);
    if(!xmlStrcmp(entryNode->name, (const xmlChar *)"entry"))
    {
      // Let's get the gd:etag attribute of entry...
      // For now, we force update using "If-Match: *"
      /*
        if( !xmlHasProp(entryNode, (const xmlChar*)"gd:etag") ) return 0;
        xmlChar *etag = xmlGetProp(entryNode,(const xmlChar*)"gd:etag");
      */

      gchar *updateUri = NULL;
      xmlNodePtr entryChilds = entryNode->xmlChildrenNode;
      if(entryChilds != NULL)
      {
        do
        {
          if((!xmlStrcmp(entryChilds->name, (const xmlChar *)"id")))
          {
            // Get the photo id used in uri for update
            xmlChar *id = xmlNodeListGetString(doc, entryChilds->xmlChildrenNode, 1);
            if(xmlStrncmp(id, (const xmlChar *)"http://", 7)) photo_id = g_strdup((const char *)id);
            xmlFree(id);
          }
          else if((!xmlStrcmp(entryChilds->name, (const xmlChar *)"group")))
          {
            // Got the media:group entry lets find the child media:keywords
            xmlNodePtr mediaChilds = entryChilds->xmlChildrenNode;
            if(mediaChilds != NULL)
            {
              do
              {
                // Got the keywords tag, let's set the tags
                if((!xmlStrcmp(mediaChilds->name, (const xmlChar *)"keywords")))
                  xmlNodeSetContent(mediaChilds, (xmlChar *)keywords);
              } while((mediaChilds = mediaChilds->next) != NULL);
            }
          }
          else if((!xmlStrcmp(entryChilds->name, (const xmlChar *)"link")))
          {
            xmlChar *rel = xmlGetProp(entryChilds, (const xmlChar *)"rel");
            if(!xmlStrcmp(rel, (const xmlChar *)"edit"))
            {
              updateUri = (char *)xmlGetProp(entryChilds, (const xmlChar *)"href");
            }
            xmlFree(rel);
          }
        } while((entryChilds = entryChilds->next) != NULL);
      }

      // Let's update the photo
      headers = NULL;
      headers = curl_slist_append(headers, "Content-Type: application/atom+xml");
      headers = curl_slist_append(headers, "If-Match: *");
      headers = curl_slist_append(headers, "GData-Version: 3");
      headers = curl_slist_append(headers, authHeader);

      _buffer_t response = { 0 };

      // Setup data to send..
      _buffer_t writebuffer;
      int datasize;
      xmlDocDumpMemory(doc, (xmlChar **)&writebuffer.data, &datasize);
      writebuffer.size = datasize;
      writebuffer.offset = 0;

      curl_easy_setopt(ctx->curl_ctx, CURLOPT_URL, updateUri);
#ifdef picasa_iEXTRA_VERBOSE
      curl_easy_setopt(ctx->curl_ctx, CURLOPT_VERBOSE, 2);
#endif
      curl_easy_setopt(ctx->curl_ctx, CURLOPT_HTTPHEADER, headers);
      curl_easy_setopt(ctx->curl_ctx, CURLOPT_UPLOAD, 1); // A put request
      curl_easy_setopt(ctx->curl_ctx, CURLOPT_READDATA, &writebuffer);
      curl_easy_setopt(ctx->curl_ctx, CURLOPT_INFILESIZE, writebuffer.size);
      curl_easy_setopt(ctx->curl_ctx, CURLOPT_READFUNCTION, _picasa_api_buffer_read_func);
      curl_easy_setopt(ctx->curl_ctx, CURLOPT_WRITEFUNCTION, _picasa_api_buffer_write_func);
      curl_easy_setopt(ctx->curl_ctx, CURLOPT_WRITEDATA, &response);
      res = curl_easy_perform(ctx->curl_ctx);

      if(res != CURLE_OK)
        fprintf(stderr, "[picasa] error updating photo: %s\n", curl_easy_strerror(res));

#ifdef picasa_EXTRA_VERBOSE
      printf("Uploading: %s\n", response.data);
#endif

      xmlFree(updateUri);
      xmlFree(writebuffer.data);
      g_free(response.data);

      curl_slist_free_all(headers);
    }

    xmlFreeDoc(doc);
    g_free(keywords);
    g_list_free_full(keywords_list, g_free);
  }
  return photo_id;
}

/**
 * @see https://developers.google.com/accounts/docs/OAuth2InstalledApp#callinganapi
 * @return basic information about the account
 */
static PicasaAccountInfo *picasa_get_account_info(PicasaContext *ctx)
{
  JsonObject *obj = picasa_query_get(ctx, "oauth2/v1/userinfo", NULL, FALSE);
  g_return_val_if_fail((obj != NULL), NULL);
  /* Using the email instead of the username as it is unique */
  /* To change it to use the username, change "email" by "name" */
  const gchar *user_name = json_object_get_string_member(obj, "given_name");
  const gchar *email = json_object_get_string_member(obj, "email");
  const gchar *user_id = json_object_get_string_member(obj, "id");
  g_return_val_if_fail(user_name != NULL && user_id != NULL, NULL);

  gchar *name = NULL;
  name = dt_util_dstrcat(name, "%s - %s", user_name, email);

  PicasaAccountInfo *accountinfo = picasa_account_info_init();
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

static gchar *picasa_get_user_refresh_token(PicasaContext *ctx)
{
  gchar *refresh_token = NULL;
  JsonObject *reply;
  gchar *params = NULL;

  params = dt_util_dstrcat(params, "refresh_token=%s&client_id=" GOOGLE_API_KEY
                                   "&client_secret=" GOOGLE_API_SECRET "&grant_type=refresh_token",
                           ctx->refresh_token);

  reply = picasa_query_post_auth(ctx, "o/oauth2/token", params);

  refresh_token = g_strdup(json_object_get_string_member(reply, "access_token"));

  g_free(params);

  return refresh_token;
}

/**
 * @see https://developers.google.com/accounts/docs/OAuth2InstalledApp
 * @returns NULL if the user cancels the operation or a valid token
 */
static int picasa_get_user_auth_token(dt_storage_picasa_gui_data_t *ui)
{
  ///////////// open the authentication url in a browser
  GError *error = NULL;
  if(!gtk_show_uri(
         gdk_screen_get_default(), GOOGLE_WS_BASE_URL
         "o/oauth2/auth?"
         "client_id=" GOOGLE_API_KEY "&redirect_uri=urn:ietf:wg:oauth:2.0:oob"
         "&scope=https://picasaweb.google.com/data/ https://www.googleapis.com/auth/userinfo.profile"
         " https://www.googleapis.com/auth/userinfo.email"
         "&response_type=code",
         gtk_get_current_event_time(), &error))
  {
    fprintf(stderr, "[picasa] error opening browser: %s\n", error->message);
    g_error_free(error);
  }

  ////////////// build & show the validation dialog
  const gchar *text1 = _("step 1: a new window or tab of your browser should have been "
                         "loaded. you have to login into your google+ account there "
                         "and authorize darktable to upload photos before continuing.");
  const gchar *text2 = _("step 2: paste the verification code shown to you in the browser "
                         "and click the OK button once you are done.");

  GtkWidget *window = dt_ui_main_window(darktable.gui->ui);
  GtkDialog *picasa_auth_dialog = GTK_DIALOG(
      gtk_message_dialog_new(GTK_WINDOW(window), GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_QUESTION,
                             GTK_BUTTONS_OK_CANCEL, _("google+ authentication")));
#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(GTK_WIDGET(picasa_auth_dialog));
#endif
  gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(picasa_auth_dialog), "%s\n\n%s", text1, text2);

  GtkWidget *entry = gtk_entry_new();
  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(gtk_label_new(_("verification code:"))), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(entry), TRUE, TRUE, 0);

  GtkWidget *picasaauthdialog_vbox
      = gtk_message_dialog_get_message_area(GTK_MESSAGE_DIALOG(picasa_auth_dialog));
  gtk_box_pack_end(GTK_BOX(picasaauthdialog_vbox), hbox, TRUE, TRUE, 0);

  gtk_widget_show_all(GTK_WIDGET(picasa_auth_dialog));

  ////////////// wait for the user to enter the verification code
  gint result;
  gchar *token = NULL;
  const char *replycode;
  while(TRUE)
  {
    result = gtk_dialog_run(GTK_DIALOG(picasa_auth_dialog));
    if(result == GTK_RESPONSE_CANCEL) break;
    replycode = gtk_entry_get_text(GTK_ENTRY(entry));
    if(replycode == NULL || g_strcmp0(replycode, "") == 0)
    {
      gtk_message_dialog_format_secondary_markup(GTK_MESSAGE_DIALOG(picasa_auth_dialog),
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
  gtk_widget_destroy(GTK_WIDGET(picasa_auth_dialog));

  if(result == GTK_RESPONSE_CANCEL)
    return 1;

  // Interchange now the authorization_code for an access_token and refresh_token
  JsonObject *reply;

  gchar *params = NULL;
  params = dt_util_dstrcat(params, "code=%s&client_id=" GOOGLE_API_KEY "&client_secret=" GOOGLE_API_SECRET
                                   "&redirect_uri=" GOOGLE_URI "&grant_type=authorization_code",
                           token);

  g_free(token);

  reply = picasa_query_post_auth(ui->picasa_api, "o/oauth2/token", params);

  gchar *access_token = g_strdup(json_object_get_string_member(reply, "access_token"));

  gchar *refresh_token = g_strdup(json_object_get_string_member(reply, "refresh_token"));

  ui->picasa_api->token = access_token;
  ui->picasa_api->refresh_token = refresh_token;

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
    PicasaAccountInfo *info = picasa_account_info_init();
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
 * @return a GSList of saved PicasaAccountInfo
 */
static GSList *load_account_info()
{
  GSList *accountlist = NULL;

  GHashTable *table = dt_pwstorage_get("picasa2");
  g_hash_table_foreach(table, (GHFunc)load_account_info_fill, &accountlist);
  g_hash_table_destroy(table);
  return accountlist;
}

static void save_account_info(dt_storage_picasa_gui_data_t *ui, PicasaAccountInfo *accountinfo)
{
  PicasaContext *ctx = ui->picasa_api;
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

  GHashTable *table = dt_pwstorage_get("picasa2");
  g_hash_table_insert(table, g_strdup(accountinfo->id), data);
  dt_pwstorage_set("picasa2", table);

  g_hash_table_destroy(table);
}

static void remove_account_info(const gchar *accountid)
{
  GHashTable *table = dt_pwstorage_get("picasa2");
  g_hash_table_remove(table, accountid);
  dt_pwstorage_set("picasa2", table);
  g_hash_table_destroy(table);
}

static void ui_refresh_users_fill(PicasaAccountInfo *value, gpointer dataptr)
{
  GtkListStore *liststore = GTK_LIST_STORE(dataptr);
  GtkTreeIter iter;
  gtk_list_store_append(liststore, &iter);
  gtk_list_store_set(liststore, &iter, COMBO_USER_MODEL_NAME_COL, value->username, COMBO_USER_MODEL_TOKEN_COL,
                     value->token, COMBO_USER_MODEL_REFRESH_TOKEN_COL, value->refresh_token,
                     COMBO_USER_MODEL_ID_COL, value->id, -1);
}

static void ui_refresh_users(dt_storage_picasa_gui_data_t *ui)
{
  GSList *accountlist = load_account_info();
  GtkListStore *list_store = GTK_LIST_STORE(gtk_combo_box_get_model(ui->comboBox_username));
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
  gtk_combo_box_set_active(ui->comboBox_username, 0);

  g_slist_free_full(accountlist, (GDestroyNotify)picasa_account_info_destroy);
  gtk_combo_box_set_row_separator_func(ui->comboBox_username, combobox_separator, ui->comboBox_username, NULL);
}

static void ui_refresh_albums_fill(PicasaAlbum *album, GtkListStore *list_store)
{
  GtkTreeIter iter;
  gtk_list_store_append(list_store, &iter);
  gtk_list_store_set(list_store, &iter, COMBO_ALBUM_MODEL_NAME_COL, album->name, COMBO_ALBUM_MODEL_ID_COL,
                     album->id, -1);
}

static void ui_refresh_albums(dt_storage_picasa_gui_data_t *ui)
{
  gboolean getlistok;
  GList *albumList = picasa_get_album_list(ui->picasa_api, &getlistok);
  if(!getlistok)
  {
    dt_control_log(_("unable to retrieve the album list"));
    goto cleanup;
  }

  GtkListStore *model_album = GTK_LIST_STORE(gtk_combo_box_get_model(ui->comboBox_album));
  GtkTreeIter iter;
  gtk_list_store_clear(model_album);
  gtk_list_store_append(model_album, &iter);
  gtk_list_store_set(model_album, &iter, COMBO_ALBUM_MODEL_NAME_COL, _("drop box"),
                     COMBO_ALBUM_MODEL_ID_COL, NULL, -1);
  if(albumList != NULL)
  {
    gtk_list_store_append(model_album, &iter);
    gtk_list_store_set(model_album, &iter, COMBO_ALBUM_MODEL_NAME_COL, "", COMBO_ALBUM_MODEL_ID_COL, NULL,
                       -1); // separator
  }
  g_list_foreach(albumList, (GFunc)ui_refresh_albums_fill, model_album);

  if(albumList != NULL) gtk_combo_box_set_active(ui->comboBox_album, 2);
  // FIXME: get the albumid and set it in the PicasaCtx
  else
    gtk_combo_box_set_active(ui->comboBox_album, 0);

  gtk_widget_show_all(GTK_WIDGET(ui->comboBox_album));
  g_list_free_full(albumList, (GDestroyNotify)picasa_album_destroy);

cleanup:
  return;
}

static void ui_combo_username_changed(GtkComboBox *combo, struct dt_storage_picasa_gui_data_t *ui)
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

  ui->picasa_api->token = g_strdup(token);
  ui->picasa_api->refresh_token = g_strdup(refresh_token);
  g_snprintf(ui->picasa_api->userid, sizeof(ui->picasa_api->userid), "%s", userid);

  if(ui->picasa_api->token != NULL && picasa_test_auth_token(ui->picasa_api))
  {
    ui->connected = TRUE;
    gtk_button_set_label(ui->button_login, _("logout"));
    ui_refresh_albums(ui);
    gtk_widget_set_sensitive(GTK_WIDGET(ui->comboBox_album), TRUE);
  }
  else
  {
    gtk_button_set_label(ui->button_login, _("login"));
    g_free(ui->picasa_api->token);
    g_free(ui->picasa_api->refresh_token);
    ui->picasa_api->token = ui->picasa_api->refresh_token = NULL;
    gtk_widget_set_sensitive(GTK_WIDGET(ui->comboBox_album), FALSE);
    GtkListStore *model_album = GTK_LIST_STORE(gtk_combo_box_get_model(ui->comboBox_album));
    gtk_list_store_clear(model_album);
  }
}

static void ui_combo_album_changed(GtkComboBox *combo, gpointer data)
{
  GtkTreeIter iter;
  gchar *albumid = NULL;
  if(gtk_combo_box_get_active_iter(combo, &iter))
  {
    GtkTreeModel *model = gtk_combo_box_get_model(combo);
    gtk_tree_model_get(model, &iter, COMBO_ALBUM_MODEL_ID_COL, &albumid, -1); // get the album id
  }
}


static gboolean ui_authenticate(dt_storage_picasa_gui_data_t *ui)
{
  if(ui->picasa_api == NULL)
  {
    ui->picasa_api = picasa_api_init();
  }

  PicasaContext *ctx = ui->picasa_api;
  gboolean mustsaveaccount = FALSE;

  gchar *uiselectedaccounttoken = NULL;
  gchar *uiselectedaccountrefreshtoken = NULL;
  gchar *uiselecteduserid = NULL;
  GtkTreeIter iter;
  gtk_combo_box_get_active_iter(ui->comboBox_username, &iter);
  GtkTreeModel *accountModel = gtk_combo_box_get_model(ui->comboBox_username);
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
  if(ctx->token != NULL && !picasa_test_auth_token(ctx))
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
    ret = picasa_get_user_auth_token(ui); // ask user to log in
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
      PicasaAccountInfo *accountinfo = picasa_get_account_info(ui->picasa_api);
      g_return_val_if_fail(accountinfo != NULL, FALSE);
      save_account_info(ui, accountinfo);

      // add account to user list and select it
      GtkListStore *model = GTK_LIST_STORE(gtk_combo_box_get_model(ui->comboBox_username));
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
      g_signal_handlers_block_matched(ui->comboBox_username, G_SIGNAL_MATCH_FUNC, 0, 0, NULL,
                                      ui_combo_username_changed, NULL);
      gtk_combo_box_set_active_iter(ui->comboBox_username, &iter);
      g_signal_handlers_unblock_matched(ui->comboBox_username, G_SIGNAL_MATCH_FUNC, 0, 0, NULL,
                                        ui_combo_username_changed, NULL);

      picasa_account_info_destroy(accountinfo);
    }
    return TRUE;
  }
}


static void ui_login_clicked(GtkButton *button, gpointer data)
{
  dt_storage_picasa_gui_data_t *ui = (dt_storage_picasa_gui_data_t *)data;
  gtk_widget_set_sensitive(GTK_WIDGET(ui->comboBox_album), FALSE);
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
    if(ui->connected == TRUE && ui->picasa_api->token != NULL)
    {
      GtkTreeModel *model = gtk_combo_box_get_model(ui->comboBox_username);
      GtkTreeIter iter;
      gtk_combo_box_get_active_iter(ui->comboBox_username, &iter);
      gchar *userid;
      gtk_tree_model_get(model, &iter, COMBO_USER_MODEL_ID_COL, &userid, -1);
      remove_account_info(userid);
      gtk_button_set_label(ui->button_login, _("login"));
      ui_refresh_users(ui);
      ui->connected = FALSE;
    }
  }
  gtk_widget_set_sensitive(GTK_WIDGET(ui->comboBox_album), TRUE);
}



////////////////////////// darktable library interface

/* plugin name */
const char *name(const struct dt_imageio_module_storage_t *self)
{
  return _("google+ photos");
}

/* construct widget above */
void gui_init(struct dt_imageio_module_storage_t *self)
{
  self->gui_data = g_malloc0(sizeof(dt_storage_picasa_gui_data_t));
  dt_storage_picasa_gui_data_t *ui = self->gui_data;
  ui->picasa_api = picasa_api_init();

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  // create entries
  GtkListStore *model_username
      = gtk_list_store_new(COMBO_USER_MODEL_NB_COL, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                           G_TYPE_STRING); // text, token, refresh_token, id
  ui->comboBox_username = GTK_COMBO_BOX(gtk_combo_box_new_with_model(GTK_TREE_MODEL(model_username)));
  GtkCellRenderer *p_cell = gtk_cell_renderer_text_new();
  g_object_set(G_OBJECT(p_cell), "ellipsize", PANGO_ELLIPSIZE_MIDDLE, "ellipsize-set", TRUE, "width-chars", 35,
               (gchar *)0);
  gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(ui->comboBox_username), p_cell, FALSE);
  gtk_cell_layout_set_attributes(GTK_CELL_LAYOUT(ui->comboBox_username), p_cell, "text", 0, NULL);

  dt_gui_key_accel_block_on_focus_connect(GTK_WIDGET(ui->comboBox_username));

  // retrieve saved accounts
  ui_refresh_users(ui);

  //////// album list /////////
  GtkWidget *albumlist = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  GtkListStore *model_album
      = gtk_list_store_new(COMBO_ALBUM_MODEL_NB_COL, G_TYPE_STRING, G_TYPE_STRING); // name, id
  ui->comboBox_album = GTK_COMBO_BOX(gtk_combo_box_new_with_model(GTK_TREE_MODEL(model_album)));
  p_cell = gtk_cell_renderer_text_new();
  g_object_set(G_OBJECT(p_cell), "ellipsize", PANGO_ELLIPSIZE_MIDDLE, "ellipsize-set", TRUE, "width-chars", 35,
               (gchar *)0);
  gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(ui->comboBox_album), p_cell, FALSE);
  gtk_cell_layout_set_attributes(GTK_CELL_LAYOUT(ui->comboBox_album), p_cell, "text", 0, NULL);

  gtk_widget_set_sensitive(GTK_WIDGET(ui->comboBox_album), FALSE);
  gtk_combo_box_set_row_separator_func(ui->comboBox_album, combobox_separator, ui->comboBox_album, NULL);
  gtk_box_pack_start(GTK_BOX(albumlist), GTK_WIDGET(ui->comboBox_album), TRUE, TRUE, 0);

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

  // connect buttons to signals
  g_signal_connect(G_OBJECT(ui->button_login), "clicked", G_CALLBACK(ui_login_clicked), (gpointer)ui);
  g_signal_connect(G_OBJECT(ui->comboBox_username), "changed", G_CALLBACK(ui_combo_username_changed),
                   (gpointer)ui);
  g_signal_connect(G_OBJECT(ui->comboBox_album), "changed", G_CALLBACK(ui_combo_album_changed), (gpointer)ui);

  g_object_unref(model_username);
  g_object_unref(model_album);
}

/* destroy resources */
void gui_cleanup(struct dt_imageio_module_storage_t *self)
{
  dt_storage_picasa_gui_data_t *ui = self->gui_data;
  dt_gui_key_accel_block_on_focus_disconnect(GTK_WIDGET(ui->comboBox_username));
  if(ui->picasa_api != NULL) picasa_api_destroy(ui->picasa_api);
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
  gint result = 1;
  PicasaContext *ctx = (PicasaContext *)sdata;

  const char *ext = format->extension(fdata);
  char fname[PATH_MAX] = { 0 };
  dt_loc_get_tmp_dir(fname, sizeof(fname));
  g_strlcat(fname, "/darktable.XXXXXX.", sizeof(fname));
  g_strlcat(fname, ext, sizeof(fname));

  gint fd = g_mkstemp(fname);
  if(fd == -1)
  {
    dt_control_log("failed to create temporary image for google+ export");
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
    g_printerr("[picasa] could not export to file: `%s'!\n", fname);
    dt_control_log(_("could not export to file `%s'!"), fname);
    result = 0;
    goto cleanup;
  }

  const char *photoid = picasa_upload_photo_to_album(ctx, ctx->album_id, fname, title, summary, imgid);
  if(photoid == NULL)
  {
    dt_control_log(_("unable to export photo to google+ album"));
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
    dt_control_log(ngettext("%d/%d exported to google+ album", "%d/%d exported to google+ album", num), num, total);
  }
  return 0;
}

static gboolean _finalize_store(gpointer user_data)
{
  dt_storage_picasa_gui_data_t *ui = (dt_storage_picasa_gui_data_t *)user_data;
  ui_refresh_albums(ui);

  return FALSE;
}

void finalize_store(struct dt_imageio_module_storage_t *self, dt_imageio_module_data_t *data)
{
  g_main_context_invoke(NULL, _finalize_store, self->gui_data);
}


size_t params_size(dt_imageio_module_storage_t *self)
{
  return sizeof(PicasaContext) - 8 * sizeof(void *);
}

void init(dt_imageio_module_storage_t *self)
{
}
void *get_params(struct dt_imageio_module_storage_t *self)
{
  dt_storage_picasa_gui_data_t *ui = (dt_storage_picasa_gui_data_t *)self->gui_data;
  if(!ui) return NULL; // gui not initialized, CLI mode
  if(ui->picasa_api == NULL || ui->picasa_api->token == NULL)
  {
    return NULL;
  }
  PicasaContext *p = (PicasaContext *)g_malloc0(sizeof(PicasaContext));

  p->curl_ctx = ui->picasa_api->curl_ctx;
  p->json_parser = ui->picasa_api->json_parser;
  p->errmsg = ui->picasa_api->errmsg;
  p->token = g_strdup(ui->picasa_api->token);
  p->refresh_token = g_strdup(ui->picasa_api->refresh_token);

  int index = gtk_combo_box_get_active(ui->comboBox_album);
  if(index < 0)
  {
    picasa_api_destroy(p);
    return NULL;
  }
  else if(index == 0)
  {
    g_snprintf(p->album_id, sizeof(p->album_id), "default");

    /* Hardcode the album as private, to avoid problems with the old Picasa interface */
    p->album_permission = 1;
  }
  else
  {
    GtkTreeModel *model = gtk_combo_box_get_model(ui->comboBox_album);
    GtkTreeIter iter;
    gchar *albumid = NULL;
    gtk_combo_box_get_active_iter(ui->comboBox_album, &iter);
    gtk_tree_model_get(model, &iter, COMBO_ALBUM_MODEL_ID_COL, &albumid, -1);
    g_snprintf(p->album_id, sizeof(p->album_id), "%s", albumid);
  }

  g_snprintf(p->userid, sizeof(p->userid), "%s", ui->picasa_api->userid);

  // recreate a new context for further usages
  ui->picasa_api = picasa_api_init();
  ui->picasa_api->token = g_strdup(p->token);
  ui->picasa_api->refresh_token = g_strdup(p->refresh_token);
  g_snprintf(ui->picasa_api->userid, sizeof(ui->picasa_api->userid), "%s", p->userid);

  return p;
}


void free_params(struct dt_imageio_module_storage_t *self, dt_imageio_module_data_t *data)
{
  if(!data) return;
  PicasaContext *ctx = (PicasaContext *)data;
  picasa_api_destroy(ctx);
}

int set_params(struct dt_imageio_module_storage_t *self, const void *params, const int size)
{
  if(size != self->params_size(self)) return 1;

  PicasaContext *d = (PicasaContext *)params;
  dt_storage_picasa_gui_data_t *g = (dt_storage_picasa_gui_data_t *)self->gui_data;

  g_snprintf(g->picasa_api->album_id, sizeof(g->picasa_api->album_id), "%s", d->album_id);
  g_snprintf(g->picasa_api->userid, sizeof(g->picasa_api->userid), "%s", d->userid);

  GtkListStore *model = GTK_LIST_STORE(gtk_combo_box_get_model(g->comboBox_username));
  GtkTreeIter iter;
  gboolean r;
  gchar *uid = NULL;
  gchar *albumid = NULL;

  for(r = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(model), &iter); r == TRUE;
      r = gtk_tree_model_iter_next(GTK_TREE_MODEL(model), &iter))
  {
    gtk_tree_model_get(GTK_TREE_MODEL(model), &iter, COMBO_USER_MODEL_ID_COL, &uid, -1);
    if(g_strcmp0(uid, g->picasa_api->userid) == 0)
    {
      gtk_combo_box_set_active_iter(g->comboBox_username, &iter);
      break;
    }
  }
  g_free(uid);

  model = GTK_LIST_STORE(gtk_combo_box_get_model(g->comboBox_album));
  for(r = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(model), &iter); r == TRUE;
      r = gtk_tree_model_iter_next(GTK_TREE_MODEL(model), &iter))
  {
    gtk_tree_model_get(GTK_TREE_MODEL(model), &iter, COMBO_ALBUM_MODEL_ID_COL, &albumid, -1);
    if(g_strcmp0(albumid, g->picasa_api->album_id) == 0)
    {
      gtk_combo_box_set_active_iter(g->comboBox_album, &iter);
      break;
    }
  }
  g_free(albumid);

  return 0;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
