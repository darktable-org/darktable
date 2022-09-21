/*
    This file is part of darktable,
    Copyright (C) 2018-2021 darktable developers.

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

#include "bauhaus/bauhaus.h"
#include "common/darktable.h"
#include "common/file_location.h"
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
#include "gui/accelerators.h"
#include "imageio/storage/imageio_storage_api.h"
#include <curl/curl.h>
#include <json-glib/json-glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <inttypes.h>

DT_MODULE(1)

#define piwigo_EXTRA_VERBOSE FALSE

#define MAX_ALBUM_NAME_SIZE 100

typedef struct _piwigo_api_context_t
{
  /// curl context
  CURL *curl_ctx;
  JsonParser *json_parser;
  JsonObject *response;
  gboolean authenticated;
  gchar *cookie_file;
  gchar *url;

  gchar *server;
  gchar *username;
  gchar *password;
  gchar *pwg_token;
  gboolean error_occured;
} _piwigo_api_context_t;

typedef struct _piwigo_album_t
{
  int64_t id;
  char name[MAX_ALBUM_NAME_SIZE];
  char label[MAX_ALBUM_NAME_SIZE];
  int64_t size;
} _piwigo_album_t;

typedef struct _piwigo_account_t
{
  gchar *server;
  gchar *username;
  gchar *password;
} _piwigo_account_t;

typedef struct dt_storage_piwigo_gui_data_t
{
  GtkLabel *status_label;
  GtkEntry *server_entry;
  GtkEntry *user_entry, *pwd_entry, *new_album_entry;
  GtkBox *create_box;                               // Create album options...
  GtkWidget *permission_list;
  GtkWidget *album_list, *parent_album_list;
  GtkWidget *account_list;

  GList *albums;
  GList *accounts;

  /** Current Piwigo context for the gui */
  _piwigo_api_context_t *api;
} dt_storage_piwigo_gui_data_t;

typedef struct _curl_args_t
{
  char name[100];
  char value[512];
} _curl_args_t;

typedef struct dt_storage_piwigo_params_t
{
  _piwigo_api_context_t *api;
  int64_t album_id;
  int64_t parent_album_id;
  char *album;
  gboolean new_album;
  int privacy;
  gboolean export_tags; // deprecated - let here not to change params size. to be removed on next version change
  gchar *tags;
} dt_storage_piwigo_params_t;

/* low-level routine doing the HTTP POST request */
static void _piwigo_api_post(_piwigo_api_context_t *ctx, GList *args, char *filename, gboolean isauth);

static size_t curl_write_data_cb(void *ptr, size_t size, size_t nmemb, void *data)
{
  GString *string = (GString *)data;
  g_string_append_len(string, ptr, size * nmemb);
#if piwigo_EXTRA_VERBOSE == TRUE
  g_printf("server reply: %s\n", string->str);
#endif
  return size * nmemb;
}

static GList *_piwigo_query_add_arguments(GList *args, const char *name, const char *value)
{
  _curl_args_t *arg = malloc(sizeof(_curl_args_t));
  g_strlcpy(arg->name, name, sizeof(arg->name));
  g_strlcpy(arg->value, value, sizeof(arg->value));
  return g_list_append(args, arg);
}

static _piwigo_api_context_t *_piwigo_ctx_init(void)
{
  _piwigo_api_context_t *ctx = malloc(sizeof(struct _piwigo_api_context_t));

  ctx->curl_ctx = curl_easy_init();
  ctx->json_parser = json_parser_new();
  ctx->authenticated = FALSE;
  ctx->url = NULL;
  ctx->cookie_file = NULL;
  ctx->error_occured = FALSE;
  ctx->pwg_token = NULL;
  return ctx;
}

static void _piwigo_ctx_destroy(_piwigo_api_context_t **ctx)
{
  if(*ctx)
  {
    curl_easy_cleanup((*ctx)->curl_ctx);
    if((*ctx)->cookie_file) g_unlink((*ctx)->cookie_file);
    g_object_unref((*ctx)->json_parser);
    g_free((*ctx)->cookie_file);
    g_free((*ctx)->url);
    g_free((*ctx)->server);
    g_free((*ctx)->username);
    g_free((*ctx)->password);
    g_free((*ctx)->pwg_token);
    free(*ctx);
    *ctx = NULL;
  }
}

static void _piwigo_free_account(void *data)
{
  _piwigo_account_t *account = (_piwigo_account_t *)data;
  g_free(account->server);
  g_free(account->username);
  g_free(account->password);
}

static void _piwigo_load_account(dt_storage_piwigo_gui_data_t *ui)
{
  if(!ui->accounts)
  {
    g_list_free_full(ui->accounts, _piwigo_free_account);
    ui->accounts = NULL;
  }

  GHashTable *table = dt_pwstorage_get("piwigo");
  GHashTableIter iter;
  gpointer key, value;

  g_hash_table_iter_init (&iter, table);

  while(g_hash_table_iter_next (&iter, &key, &value))
  {
    if(key && value)
    {
      gchar *data = (gchar *)value;
      JsonParser *parser = json_parser_new();
      json_parser_load_from_data(parser, data, strlen(data), NULL);
      JsonNode *root = json_parser_get_root(parser);

      if(root)
      {
        JsonObject *obj = json_node_get_object(root);
        _piwigo_account_t *account = malloc(sizeof(_piwigo_account_t));

        account->server =  g_strdup(json_object_get_string_member(obj, "server"));
        account->username =  g_strdup(json_object_get_string_member(obj, "username"));
        account->password =  g_strdup(json_object_get_string_member(obj, "password"));

        if(account->server && strlen(account->server)>0)
          ui->accounts = g_list_append(ui->accounts, account);
        else
          free(account); // we didn't add account to list, freeing it
      }

      g_object_unref(parser);
    }
  }

  g_hash_table_destroy(table);
}

static _piwigo_account_t *_piwigo_get_account(dt_storage_piwigo_gui_data_t *ui, const gchar *server)
{
  if(!server) return NULL;

  for(const GList *a = ui->accounts; a; a = g_list_next(a))
  {
    _piwigo_account_t *account = (_piwigo_account_t *)a->data;;
    if(account->server && !strcmp(server, account->server)) return account;
  }

  return NULL;
}

static void _piwigo_set_account(dt_storage_piwigo_gui_data_t *ui)
{
  /// serialize data;
  JsonBuilder *builder = json_builder_new();
  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "server");
  json_builder_add_string_value(builder, gtk_entry_get_text(ui->server_entry));
  json_builder_set_member_name(builder, "username");
  json_builder_add_string_value(builder, gtk_entry_get_text(ui->user_entry));
  json_builder_set_member_name(builder, "password");
  json_builder_add_string_value(builder, gtk_entry_get_text(ui->pwd_entry));

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

  GHashTable *table = dt_pwstorage_get("piwigo");
  g_hash_table_insert(table, g_strdup(gtk_entry_get_text(ui->server_entry)), data);
  dt_pwstorage_set("piwigo", table);
  g_hash_table_destroy(table);
}

/** Set status connection text */
static void _piwigo_set_status(dt_storage_piwigo_gui_data_t *ui, gchar *message, gchar *color)
{
  if(!color) color = "#ffffff";
  gchar mup[512] = { 0 };
  snprintf(mup, sizeof(mup), "<span foreground=\"%s\" ><small>%s</small></span>", color, message);
  gtk_label_set_markup(ui->status_label, mup);
  gtk_widget_set_tooltip_markup(GTK_WIDGET(ui->status_label), mup);
}

static int _piwigo_api_post_internal(_piwigo_api_context_t *ctx, GList *args, char *filename, gboolean isauth)
{
  curl_mime *form = NULL;

  GString *url = g_string_new(ctx->url);

  // send the requests
  GString *response = g_string_new("");

  dt_curl_init(ctx->curl_ctx, piwigo_EXTRA_VERBOSE);

  curl_easy_setopt(ctx->curl_ctx, CURLOPT_URL, url->str);
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_POST, 1);
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_WRITEFUNCTION, curl_write_data_cb);
  curl_easy_setopt(ctx->curl_ctx, CURLOPT_WRITEDATA, response);

  if(isauth)
  {
    /* construct a temporary file name */
    char cookie_fmt[PATH_MAX] = { 0 };
    dt_loc_get_tmp_dir(cookie_fmt, sizeof(cookie_fmt));
    g_strlcat(cookie_fmt, "/cookies.%.4lf.txt", sizeof(cookie_fmt));

    ctx->cookie_file = g_strdup_printf(cookie_fmt, dt_get_wtime());

    // not that this is safe as the cookie file is written only when the curl context is finalized.
    // At this stage we unlink the file.
    curl_easy_setopt(ctx->curl_ctx, CURLOPT_COOKIEJAR, ctx->cookie_file);
  }
  else
  {
    curl_easy_setopt(ctx->curl_ctx, CURLOPT_COOKIEFILE, ctx->cookie_file);
  }

  if(filename)
  {
    curl_mimepart *field = NULL;

    form = curl_mime_init(ctx->curl_ctx);

    for(const GList *a = args; a; a = g_list_next(a))
    {
      _curl_args_t *ca = (_curl_args_t *)a->data;
      field = curl_mime_addpart(form);
      curl_mime_name(field, ca->name);
      curl_mime_data(field, ca->value, CURL_ZERO_TERMINATED);
    }

    field = curl_mime_addpart(form);
    curl_mime_name(field, "image");
    curl_mime_filedata(field, filename);

    curl_easy_setopt(ctx->curl_ctx, CURLOPT_MIMEPOST, form);
  }
  else
  {
    GString *gargs = g_string_new("");

    for(const GList *a = args; a; a = g_list_next(a))
    {
      _curl_args_t *ca = (_curl_args_t *)a->data;
      if(a!=args) g_string_append(gargs, "&");
      g_string_append(gargs, ca->name);
      g_string_append(gargs, "=");
      g_string_append(gargs, ca->value);
    }

    curl_easy_setopt(ctx->curl_ctx, CURLOPT_COPYPOSTFIELDS, gargs->str);
    g_string_free(gargs, TRUE);
  }

  const int res = curl_easy_perform(ctx->curl_ctx);

#if piwigo_EXTRA_VERBOSE == TRUE
  g_printf("curl_easy_perform status %d\n", res);
#endif

  if(filename) curl_mime_free(form);

  g_string_free(url, TRUE);

  ctx->response = NULL;

  if(res == CURLE_OK)
  {
    GError *error = NULL;
    gboolean ret = json_parser_load_from_data(ctx->json_parser, response->str, response->len, &error);
    if(!ret) goto cleanup;
    JsonNode *root = json_parser_get_root(ctx->json_parser);
    // we should always have a dict
    if(json_node_get_node_type(root) != JSON_NODE_OBJECT) goto cleanup;
    ctx->response = json_node_get_object(root);
    const char *status = json_object_get_string_member(ctx->response, "stat");
    ctx->error_occured = (status && (strcmp(status,"fail")==0));
  }
  else
    ctx->error_occured = TRUE;

 cleanup:
  g_string_free(response, TRUE);
  return res;
}

static void _piwigo_api_authenticate(_piwigo_api_context_t *ctx)
{
  GList *args = NULL;

  args = _piwigo_query_add_arguments(args, "method", "pwg.session.login");
  args = _piwigo_query_add_arguments(args, "username", ctx->username);
  args = _piwigo_query_add_arguments(args, "password", ctx->password);
  if(!strcmp(ctx->server, "piwigo.com"))
    ctx->url = g_strdup_printf("https://%s.piwigo.com/ws.php?format=json", ctx->username);
  else if(strstr(ctx->server, "http") == ctx->server)
    ctx->url = g_strdup_printf("%s/ws.php?format=json", ctx->server);
  else
    ctx->url = g_strdup_printf("https://%s/ws.php?format=json", ctx->server);

  _piwigo_api_post(ctx, args, NULL, TRUE);

  g_list_free(args);

  //  getStatus to retrieve the pwd_token

  args = NULL;

  args = _piwigo_query_add_arguments(args, "method", "pwg.session.getStatus");

  _piwigo_api_post(ctx, args, NULL, TRUE);

  if(ctx->response && !ctx->error_occured)
  {
    JsonObject *result = json_node_get_object(json_object_get_member(ctx->response, "result"));
    const gchar *pwg_token = json_object_get_string_member(result, "pwg_token");
    ctx->pwg_token = g_strdup(pwg_token);
  }

  g_list_free(args);
}

static void _piwigo_api_post(_piwigo_api_context_t *ctx, GList *args, char *filename, gboolean isauth)
{
  int res = _piwigo_api_post_internal(ctx, args, filename, isauth);

  if(res == CURLE_COULDNT_CONNECT || res == CURLE_SSL_CONNECT_ERROR)
  {
#if piwigo_EXTRA_VERBOSE == TRUE
    g_printf("curl post error (%d), try authentication again\n", res);
#endif

    //  recreate a new CURL connection
    curl_easy_cleanup(ctx->curl_ctx);
    ctx->curl_ctx = curl_easy_init();
    ctx->authenticated = FALSE;

    if(!isauth)
    {
      // an error during the curl post command, could be an authentication issue, try to authenticate again
      // but only if this is not an authentication post, otherwise this will happen below anyway.
      _piwigo_api_authenticate(ctx);
    }

    // authentication ok, send again
    if(ctx->response && !ctx->error_occured)
    {
      ctx->authenticated = TRUE;
#if piwigo_EXTRA_VERBOSE == TRUE
      g_printf("authenticated again, retry\n");
#endif
      res = _piwigo_api_post_internal(ctx, args, filename, isauth);
#if piwigo_EXTRA_VERBOSE == TRUE
      g_printf("second post exit with status %d\n", res);
#endif
    }
    else
    {
#if piwigo_EXTRA_VERBOSE == TRUE
      g_printf("failed second authentication\n");
#endif
    }
  }
}

static void _piwigo_authenticate(dt_storage_piwigo_gui_data_t *ui)
{
  if(!ui->api) ui->api = _piwigo_ctx_init();

  ui->api->server = g_strdup(gtk_entry_get_text(ui->server_entry));
  ui->api->username = g_uri_escape_string(gtk_entry_get_text(ui->user_entry), NULL, FALSE);
  ui->api->password = g_uri_escape_string(gtk_entry_get_text(ui->pwd_entry), NULL, FALSE);

  _piwigo_api_authenticate(ui->api);

  ui->api->authenticated = FALSE;

  if(ui->api->response && !ui->api->error_occured)
  {
    ui->api->authenticated = TRUE;
    gtk_widget_set_sensitive(GTK_WIDGET(ui->album_list), ui->api->authenticated);

    if(ui->api->authenticated)
    {
      _piwigo_set_status(ui, _("authenticated"), "#7fe07f");
      dt_conf_set_string("plugins/imageio/storage/export/piwigo/server", ui->api->server);
      _piwigo_set_account(ui);
    }
    else
    {
      const gchar *errormessage = json_object_get_string_member(ui->api->response, "message");
      fprintf(stderr, "[imageio_storage_piwigo] could not authenticate: `%s'!\n", errormessage);
      _piwigo_set_status(ui, _("not authenticated"), "#e07f7f");
      _piwigo_ctx_destroy(&ui->api);
    }
  }
  else
  {
    _piwigo_set_status(ui, _("not authenticated, cannot reach server"), "#e07f7f");
    _piwigo_ctx_destroy(&ui->api);
  }
}

static void _piwigo_entry_changed(GtkEntry *entry, gpointer data)
{
  dt_storage_piwigo_gui_data_t *ui = (dt_storage_piwigo_gui_data_t *)data;

  _piwigo_set_status(ui, _("not authenticated"), "#e07f7f");
    gtk_widget_set_sensitive(GTK_WIDGET(ui->album_list), FALSE);

  if(ui->api) _piwigo_ctx_destroy(&ui->api);
}

static void _piwigo_server_entry_changed(GtkEntry *entry, gpointer data)
{
  dt_storage_piwigo_gui_data_t *ui = (dt_storage_piwigo_gui_data_t *)data;

  if(ui->api)
  {
    _piwigo_set_status(ui, _("not authenticated"), "#e07f7f");
    _piwigo_ctx_destroy(&ui->api);
    gtk_widget_set_sensitive(GTK_WIDGET(ui->album_list), FALSE);
  }
}

static void _piwigo_account_changed(GtkComboBox *cb, gpointer data)
{
  dt_storage_piwigo_gui_data_t *ui = (dt_storage_piwigo_gui_data_t *)data;
  const gchar *value = dt_bauhaus_combobox_get_text(ui->account_list);
  const _piwigo_account_t *account = _piwigo_get_account(ui, value);

  if(account)
  {
    gtk_entry_set_text(ui->server_entry, account->server);
    gtk_entry_set_text(ui->user_entry, account->username);
    gtk_entry_set_text(ui->pwd_entry, account->password);
  }
}

static void _piwigo_album_changed(GtkComboBox *cb, gpointer data)
{
  dt_storage_piwigo_gui_data_t *ui = (dt_storage_piwigo_gui_data_t *)data;
  const gchar *value = dt_bauhaus_combobox_get_text(ui->album_list);

  // early return if the combo is not yet populated
  if(value == NULL) return;

  if(strcmp(value, _("create new album")) == 0)
  {
    gtk_widget_set_no_show_all(GTK_WIDGET(ui->create_box), FALSE);
    gtk_widget_show_all(GTK_WIDGET(ui->create_box));
  }
  else
  {
    gtk_widget_hide(GTK_WIDGET(ui->create_box));

    // As the album name has spaces as prefix (for indentation) and a
    // count of entries in parenthesis as suffix, we need to do some clean-up.
    gchar *v = g_strstrip(g_strdup(value));
    gchar *p = v + strlen(v) - 1;
    if(*p == ')')
    {
      while(p != v && *p != '(') p--;
      if(*p == '(')
      {
        p--;
        if(p >= v) *p = '\0';
      }
    }
    dt_conf_set_string("storage/piwigo/last_album", v);
    g_free(v);
  }
}

/** Refresh albums */
static void _piwigo_refresh_albums(dt_storage_piwigo_gui_data_t *ui, const gchar *select_album)
{
  gtk_widget_set_sensitive(GTK_WIDGET(ui->album_list), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(ui->parent_album_list), FALSE);

  if(ui->api == NULL || ui->api->authenticated == FALSE)
  {
    _piwigo_authenticate(ui);
    if(ui->api == NULL || !ui->api->authenticated) return;
  }

  gchar *to_select;
  int index = 0;

  // get the new album name, it will be checked in the
  if(select_album == NULL)
  {
    to_select = g_strdup(dt_bauhaus_combobox_get_text(ui->album_list));
    if(to_select)
    {
      // cut the count of picture in album to get the name only
      gchar *p = to_select;
      while(*p)
      {
        if(*p == ' ' && *(p+1) == '(')
        {
          *p = '\0';
          break;
        }
        p++;
      }
    }
  }
  else
    to_select = g_strdup(select_album);

  // First clear the combobox except first 2 items (none / create new album)
  dt_bauhaus_combobox_clear(ui->album_list);
  dt_bauhaus_combobox_clear(ui->parent_album_list);
  g_list_free(ui->albums);
  ui->albums = NULL;

  GList *args = NULL;

  args = _piwigo_query_add_arguments(args, "method", "pwg.categories.getList");
  args = _piwigo_query_add_arguments(args, "cat_id", "0");
  args = _piwigo_query_add_arguments(args, "recursive", "true");

  _piwigo_api_post(ui->api, args, NULL, FALSE);

  g_list_free(args);

  if(ui->api->response && !ui->api->error_occured)
  {
    dt_bauhaus_combobox_add(ui->album_list, _("create new album"));
    dt_bauhaus_combobox_add(ui->parent_album_list, _("---"));

    JsonObject *result = json_node_get_object(json_object_get_member(ui->api->response, "result"));
    JsonArray *albums = json_object_get_array_member(result, "categories");

    if(json_array_get_length(albums)>0 && index==0) index = 1;
    if(index > json_array_get_length(albums) - 1) index = json_array_get_length(albums) - 1;

    for(int i = 0; i < json_array_get_length(albums); i++)
    {
      char data[MAX_ALBUM_NAME_SIZE] = { 0 };
      JsonObject *album = json_array_get_object_element(albums, i);

      _piwigo_album_t *new_album = g_malloc0(sizeof(struct _piwigo_album_t));

      g_strlcpy(new_album->name, json_object_get_string_member(album, "name"), sizeof(new_album->name));
      new_album->id = json_object_get_int_member(album, "id");
      new_album->size = json_object_get_int_member(album, "nb_images");
      const int isroot = json_object_get_null_member(album, "id_uppercat");
      int indent = 0;

      if(!isroot)
      {
        // Ids of parent albums coma separated
        const char *hierarchy = json_object_get_string_member(album, "uppercats");
        char const *p = hierarchy;
        while(*p++) if(*p == ',') indent++;
      }

      snprintf(data, sizeof(data), "%*c%s (%"PRId64")", indent * 3, ' ', new_album->name, new_album->size);

      if(to_select && !strcmp(new_album->name, to_select)) index = i + 1;

      g_strlcpy(new_album->label, data, sizeof(new_album->label));

      ui->albums = g_list_append(ui->albums, new_album);

      dt_bauhaus_combobox_add_aligned(ui->album_list, data, DT_BAUHAUS_COMBOBOX_ALIGN_LEFT);
      dt_bauhaus_combobox_add_aligned(ui->parent_album_list, data, DT_BAUHAUS_COMBOBOX_ALIGN_LEFT);
    }
  }
  else
    dt_control_log(_("cannot refresh albums"));

  g_free(to_select);

  gtk_widget_set_sensitive(GTK_WIDGET(ui->album_list), TRUE);
  gtk_widget_set_sensitive(GTK_WIDGET(ui->parent_album_list), TRUE);
  dt_bauhaus_combobox_set(ui->album_list, index);
  dt_bauhaus_combobox_set(ui->parent_album_list, 0);
}


static gboolean _piwigo_api_create_new_album(dt_storage_piwigo_params_t *p)
{
  GList *args = NULL;

  args = _piwigo_query_add_arguments(args, "method", "pwg.categories.add");
  args = _piwigo_query_add_arguments(args, "name", p->album);
  if(p->parent_album_id != 0)
  {
    char pid[100];
    snprintf(pid, sizeof(pid), "%"PRId64, p->parent_album_id);
    args = _piwigo_query_add_arguments(args, "parent", pid);
  }
  args = _piwigo_query_add_arguments(args, "status", p->privacy==0?"public":"private");

  _piwigo_api_post(p->api, args, NULL, FALSE);

  g_list_free(args);

  if(!p->api->response || p->api->error_occured)
  {
    return FALSE;
  }
  else
  {
    JsonObject *result = json_node_get_object(json_object_get_member(p->api->response, "result"));
    // set new album id in parameter
    p->album_id = json_object_get_int_member(result, "id");
  }

  return TRUE;
}

static gboolean _piwigo_api_upload_photo(dt_storage_piwigo_params_t *p, gchar *fname,
                                         gchar *author, gchar *caption, gchar *description)
{
  GList *args = NULL;
  char cat[10];
  char privacy[10];

  // upload picture

  snprintf(cat, sizeof(cat), "%"PRId64, p->album_id);
  snprintf(privacy, sizeof(privacy), "%d", p->privacy);

  args = _piwigo_query_add_arguments(args, "method", "pwg.images.addSimple");
  args = _piwigo_query_add_arguments(args, "image", fname);
  args = _piwigo_query_add_arguments(args, "category", cat);
  args = _piwigo_query_add_arguments(args, "level", privacy);

  if(caption && strlen(caption)>0)
    args = _piwigo_query_add_arguments(args, "name", caption);

  if(author && strlen(author)>0)
    args = _piwigo_query_add_arguments(args, "author", author);

  if(description && strlen(description)>0)
    args = _piwigo_query_add_arguments(args, "comment", description);

  if(p->tags && strlen(p->tags)>0)
    args = _piwigo_query_add_arguments(args, "tags", p->tags);

  _piwigo_api_post(p->api, args, fname, FALSE);

  g_list_free(args);

  return !p->api->error_occured;
}

// Login button pressed...
static void _piwigo_login_clicked(GtkButton *button, gpointer data)
{
  dt_storage_piwigo_gui_data_t *ui = (dt_storage_piwigo_gui_data_t *)data;
  _piwigo_ctx_destroy(&ui->api);

  gchar *last_album = dt_conf_get_string("storage/piwigo/last_album");
  _piwigo_refresh_albums(ui, last_album);
  dt_conf_set_string("storage/piwigo/last_album", last_album);
  g_free(last_album);
}

// Refresh button pressed...
static void _piwigo_refresh_clicked(GtkButton *button, gpointer data)
{
  dt_storage_piwigo_gui_data_t *ui = (dt_storage_piwigo_gui_data_t *)data;

  gchar *last_album = dt_conf_get_string("storage/piwigo/last_album");
  _piwigo_refresh_albums(ui, NULL);
  dt_conf_set_string("storage/piwigo/last_album", last_album);
  g_free(last_album);
}

const char *name(const struct dt_imageio_module_storage_t *self)
{
  return _("piwigo");
}

void gui_init(dt_imageio_module_storage_t *self)
{
  self->gui_data = (dt_storage_piwigo_gui_data_t *)g_malloc0(sizeof(dt_storage_piwigo_gui_data_t));
  dt_storage_piwigo_gui_data_t *ui = self->gui_data;

  ui->albums = NULL;
  ui->accounts = NULL;
  ui->api = NULL;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  _piwigo_load_account(ui);

  gchar *server = dt_conf_get_string("plugins/imageio/storage/export/piwigo/server");

  // look for last server information
  _piwigo_account_t *last_account = _piwigo_get_account(ui, server);

  GtkWidget *hbox, *label, *button;

  // account
  ui->account_list = dt_bauhaus_combobox_new_action(DT_ACTION(self));
  dt_bauhaus_widget_set_label(ui->account_list, NULL, N_("accounts"));
  int account_index = -1, index=0;
  for(const GList *a = ui->accounts; a; a = g_list_next(a))
  {
    _piwigo_account_t *account = (_piwigo_account_t *)a->data;
    dt_bauhaus_combobox_add(ui->account_list, account->server);
    if(!strcmp(account->server, server)) account_index = index;
    index++;
  }
  gtk_widget_set_hexpand(ui->account_list, TRUE);
  g_signal_connect(G_OBJECT(ui->account_list), "value-changed", G_CALLBACK(_piwigo_account_changed), (gpointer)ui);
  gtk_box_pack_start(GTK_BOX(self->widget), ui->account_list, FALSE, FALSE, 0);

  // server
  hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  ui->server_entry = GTK_ENTRY(dt_action_entry_new(DT_ACTION(self), N_("server"), G_CALLBACK(_piwigo_server_entry_changed), ui,
                                                   _("the server name\ndefault protocol is https\nspecify http:// if non secure server"),
                                                   last_account?last_account->server:"piwigo.com"));
  gtk_widget_set_hexpand(GTK_WIDGET(ui->server_entry), TRUE);
  gtk_box_pack_start(GTK_BOX(hbox), dt_ui_label_new(_("server")), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(ui->server_entry), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hbox), TRUE, TRUE, 0);
  g_free(server);

  // login
  hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  ui->user_entry = GTK_ENTRY(dt_action_entry_new(DT_ACTION(self), N_("user"), G_CALLBACK(_piwigo_entry_changed), ui,
                                                   _("the server name\ndefault protocol is https\nspecify http:// if non secure server"),
                                                   last_account?last_account->username:""));
  gtk_widget_set_hexpand(GTK_WIDGET(ui->user_entry), TRUE);
  gtk_box_pack_start(GTK_BOX(hbox), dt_ui_label_new(_("user")), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(ui->user_entry), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hbox), TRUE, TRUE, 0);

  // password
  hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  ui->pwd_entry = GTK_ENTRY(dt_action_entry_new(DT_ACTION(self), N_("password"), G_CALLBACK(_piwigo_entry_changed), ui, NULL,
                                                   last_account?last_account->password:""));
  gtk_entry_set_visibility(GTK_ENTRY(ui->pwd_entry), FALSE);
  gtk_widget_set_hexpand(GTK_WIDGET(ui->pwd_entry), TRUE);
  gtk_box_pack_start(GTK_BOX(hbox), dt_ui_label_new(_("password")), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(ui->pwd_entry), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hbox), TRUE, TRUE, 0);

  // login button
  button = gtk_button_new_with_label(_("login"));
  gtk_widget_set_tooltip_text(button, _("piwigo login"));
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(_piwigo_login_clicked), (gpointer)ui);
  gtk_box_pack_start(GTK_BOX(self->widget), button, FALSE, FALSE, 0);

  // status area
  ui->status_label = GTK_LABEL(gtk_label_new(NULL));
  gtk_label_set_ellipsize(ui->status_label, PANGO_ELLIPSIZE_END);
  gtk_widget_set_halign(GTK_WIDGET(ui->status_label), GTK_ALIGN_START);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(ui->status_label), FALSE, FALSE, 0);

  // select account
  if(account_index != -1) dt_bauhaus_combobox_set(ui->account_list, account_index);

  // permissions list
  DT_BAUHAUS_COMBOBOX_NEW_FULL(ui->permission_list, self, NULL, N_("visible to"), NULL,
                               0, NULL, NULL,
                               N_("everyone"),
                               N_("contacts"),
                               N_("friends"),
                               N_("family"),
                               N_("you"));
  gtk_box_pack_start(GTK_BOX(self->widget), ui->permission_list, FALSE, FALSE, 0);

  // album list
  hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

  ui->album_list = dt_bauhaus_combobox_new_action(DT_ACTION(self)); // Available albums
  dt_bauhaus_widget_set_label(ui->album_list, NULL, N_("album"));
  g_signal_connect(G_OBJECT(ui->album_list), "value-changed", G_CALLBACK(_piwigo_album_changed), (gpointer)ui);
  gtk_widget_set_sensitive(ui->album_list, FALSE);
  gtk_box_pack_start(GTK_BOX(hbox), ui->album_list, TRUE, TRUE, 0);

  button = dtgtk_button_new(dtgtk_cairo_paint_refresh, CPF_NONE, NULL);
  gtk_widget_set_tooltip_text(button, _("refresh album list"));
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(_piwigo_refresh_clicked), (gpointer)ui);
  gtk_box_pack_start(GTK_BOX(hbox), button, FALSE, FALSE, 0);

  gtk_box_pack_start(GTK_BOX(self->widget), hbox, FALSE, FALSE, 0);

  // new album
  ui->create_box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
  gtk_widget_set_no_show_all(GTK_WIDGET(ui->create_box), TRUE);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(ui->create_box), FALSE, FALSE, 0);

  hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

  label = gtk_label_new(_("title"));
  g_object_set(G_OBJECT(label), "xalign", 0.0, (gchar *)0);
  gtk_box_pack_start(GTK_BOX(hbox), label, FALSE, FALSE, 0);

  ui->new_album_entry = GTK_ENTRY(gtk_entry_new()); // Album title
  gtk_entry_set_text(ui->new_album_entry, _("new album"));
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(ui->new_album_entry), TRUE, TRUE, 0);
  gtk_entry_set_width_chars(GTK_ENTRY(ui->new_album_entry), 0);

  gtk_box_pack_start(ui->create_box, hbox, FALSE, FALSE, 0);

  // parent album list
  ui->parent_album_list = dt_bauhaus_combobox_new_action(DT_ACTION(self)); // Available albums
  dt_bauhaus_widget_set_label(ui->parent_album_list, NULL, N_("parent album"));
  gtk_widget_set_sensitive(ui->parent_album_list, TRUE);
  gtk_box_pack_start(ui->create_box, ui->parent_album_list, TRUE, TRUE, 0);

  _piwigo_set_status(ui, _("click login button to start"), "#ffffff");
}

void gui_cleanup(dt_imageio_module_storage_t *self)
{
  g_free(self->gui_data);
}

void gui_reset(dt_imageio_module_storage_t *self)
{
}

static gboolean _finalize_store(gpointer user_data)
{
  dt_storage_piwigo_gui_data_t *g = (dt_storage_piwigo_gui_data_t *)user_data;

  // notify that uploads are completed to empty the lounge

  if(!g->api->error_occured)
  {
    GList *args = NULL;

    args = _piwigo_query_add_arguments(args, "method", "pwg.images.uploadCompleted");
    args = _piwigo_query_add_arguments(args, "pwg_token", g->api->pwg_token);

    _piwigo_api_post(g->api, args, NULL, FALSE);

    g_list_free(args);
  }

  _piwigo_refresh_albums(g, dt_bauhaus_combobox_get_text(g->album_list));

  return FALSE;
}

void finalize_store(struct dt_imageio_module_storage_t *self, dt_imageio_module_data_t *data)
{
  g_main_context_invoke(NULL, _finalize_store, self->gui_data);
}

int store(dt_imageio_module_storage_t *self, dt_imageio_module_data_t *sdata, const int imgid,
          dt_imageio_module_format_t *format, dt_imageio_module_data_t *fdata, const int num, const int total,
          const gboolean high_quality, const gboolean upscale, const gboolean export_masks,
          dt_colorspaces_color_profile_type_t icc_type, const gchar *icc_filename, dt_iop_color_intent_t icc_intent,
          dt_export_metadata_t *metadata)
{
  dt_storage_piwigo_gui_data_t *ui = self->gui_data;

  gint result = 0;

  const char *ext = format->extension(fdata);

  // Let's upload image...

  /* construct a temporary file name */
  char fname[PATH_MAX] = { 0 };
  dt_loc_get_tmp_dir(fname, sizeof(fname));
  g_strlcat(fname, "/darktable.XXXXXX.", sizeof(fname));
  g_strlcat(fname, ext, sizeof(fname));

  char *caption = NULL;
  char *description = NULL;
  char *author = NULL;

  gint fd = g_mkstemp(fname);
  if(fd == -1)
  {
    dt_control_log("failed to create temporary image for piwigo export");
    fprintf(stderr, "failed to create tempfile: %s\n", fname);
    return 1;
  }
  close(fd);

  if((metadata->flags & DT_META_METADATA) && !(metadata->flags & DT_META_CALCULATED))
  {
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
      gchar *dot = g_strrstr(caption, ".");
      if(dot) dot[0] = '\0'; // chop extension...
    }

    GList *desc = dt_metadata_get(img->id, "Xmp.dc.description", NULL);
    if(desc != NULL)
    {
      description = g_strdup(desc->data);
      g_list_free_full(desc, &g_free);
    }
    dt_image_cache_read_release(darktable.image_cache, img);

    GList *auth = dt_metadata_get(img->id, "Xmp.dc.creator", NULL);
    if(auth != NULL)
    {
      author = g_strdup(auth->data);
      g_list_free_full(auth, &g_free);
    }
  }
  if(dt_imageio_export(imgid, fname, format, fdata, high_quality, upscale, TRUE, export_masks, icc_type, icc_filename,
                       icc_intent, self, sdata, num, total, metadata) != 0)
  {
    fprintf(stderr, "[imageio_storage_piwigo] could not export to file: `%s'!\n", fname);
    dt_control_log(_("could not export to file `%s'!"), fname);
    result = 1;
    goto cleanup;
  }
  dt_pthread_mutex_lock(&darktable.plugin_threadsafe);
  {
    gboolean status = TRUE;
    dt_storage_piwigo_params_t *p = (dt_storage_piwigo_params_t *)sdata;

    if(metadata->flags & DT_META_TAG)
    {
      GList *tags_list = dt_tag_get_list_export(imgid, metadata->flags);
      p->tags = dt_util_glist_to_str(",", tags_list);
      g_list_free_full(tags_list, g_free);
    }

    if(p->new_album)
    {
      status = _piwigo_api_create_new_album(p);
      if(!status) dt_control_log(_("cannot create a new piwigo album!"));
    }

    if(status)
    {
      status = _piwigo_api_upload_photo(p, fname, author, caption, description);
      if(!status)
      {
        fprintf(stderr, "[imageio_storage_piwigo] could not upload to piwigo!\n");
        dt_control_log(_("could not upload to piwigo!"));
        result = 1;
      }
      else if(p->new_album)
      {
        // we do not want to create more albums when multiple upload
        p->new_album = FALSE;
        _piwigo_refresh_albums(ui, p->album);
      }
    }
    if(p->tags)
    {
      g_free(p->tags);
      p->tags = NULL;
    }
  }
  dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);

cleanup:

  // And remove from filesystem..
  g_unlink(fname);
  g_free(caption);
  g_free(description);
  g_free(author);

  if(!result)
  {
    // this makes sense only if the export was successful
    dt_control_log(ngettext("%d/%d exported to piwigo webalbum", "%d/%d exported to piwigo webalbum", num),
                   num, total);
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

static uint64_t _piwigo_album_id(const gchar *name, GList *albums)
{
  uint64_t id = 0;

  for(const GList *a = albums; a; a = g_list_next(a))
  {
    _piwigo_album_t *album = (_piwigo_album_t *)a->data;
    if(!strcmp(name, album->label))
    {
      id = album->id;
      break;
    }
  }

  return id;
}

void *get_params(dt_imageio_module_storage_t *self)
{
  dt_storage_piwigo_gui_data_t *ui = (dt_storage_piwigo_gui_data_t *)self->gui_data;
  if(!ui) return NULL; // gui not initialized, CLI mode
  dt_storage_piwigo_params_t *p = (dt_storage_piwigo_params_t *)g_malloc0(sizeof(dt_storage_piwigo_params_t));

  if(!p) return NULL;

  // fill d from controls in ui
  if(ui->api && ui->api->authenticated == TRUE)
  {
    // create a new context for the import. set username/password to be able to connect.
    p->api = _piwigo_ctx_init();
    p->api->authenticated = FALSE;
    p->api->server = g_strdup(ui->api->server);
    p->api->username = g_strdup(ui->api->username);
    p->api->password = g_strdup(ui->api->password);

    _piwigo_api_authenticate(p->api);

    int index = dt_bauhaus_combobox_get(ui->album_list);

    p->album_id = 0;
    p->tags = NULL;

    switch(dt_bauhaus_combobox_get(ui->permission_list))
    {
      case 0:                // everyone
        p->privacy = 0;
        break;
      case 1:                // contacts
        p->privacy = 1;
        break;
      case 2:                // friends
        p->privacy = 2;
        break;
      case 3:                // family
        p->privacy = 4;
        break;
      default:               // you / admin
        p->privacy = 8;
    }

    if(index >= 0)
    {
      switch(index)
      {
        case 0: // Create album
          p->parent_album_id = _piwigo_album_id(dt_bauhaus_combobox_get_text(ui->parent_album_list), ui->albums);
          p->album = g_strdup(gtk_entry_get_text(ui->new_album_entry));
          p->new_album = TRUE;
          break;

        default:
          p->album = g_strdup(dt_bauhaus_combobox_get_text(ui->album_list));
          p->new_album = FALSE;

          if(p->album == NULL)
          {
            // Something went wrong...
            fprintf(stderr, "Something went wrong.. album index %d = NULL\n", index - 2);
            goto cleanup;
          }

          p->album_id = _piwigo_album_id(p->album, ui->albums);

          if(!p->album_id)
          {
            fprintf(stderr, "[imageio_storage_piwigo] cannot find album `%s'!\n", p->album);
            goto cleanup;
          }

          break;
      }
    }
    else
      goto cleanup;
  }
  else
    goto cleanup;

  return p;

 cleanup:
    g_free(p);
    return NULL;
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
  dt_storage_piwigo_params_t *p = (dt_storage_piwigo_params_t *)params;

  if(p)
  {
    g_free(p->album);
    g_free(p->tags);
    _piwigo_ctx_destroy(&p->api);
    free(p);
  }
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

