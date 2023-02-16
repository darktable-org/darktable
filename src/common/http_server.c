/*
 *    This file is part of darktable,
 *    Copyright (C) 2015-2020 darktable developers.
 *
 *    darktable is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 *
 *    darktable is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <glib/gi18n.h>
#include <stdio.h>
#include <stdlib.h>

#include "common/darktable.h"
#include "common/http_server.h"

typedef struct _connection_t
{
  const char *id;
  dt_http_server_t *server;
  dt_http_server_callback callback;
  gpointer user_data;
} _connection_t;

static const char reply[]
    = "<!DOCTYPE html>\n"
      "<html>\n"
      "<head>\n"
      "<meta http-equiv=\"Content-Type\" content=\"text/html; charset=utf-8\">\n"
      "<title>%s</title>\n"
      "<style>\n"
      "html {\n"
      "  background-color: #575656;\n"
      "  font-family: \"Lucida Grande\",Verdana,\"Bitstream Vera Sans\",Arial,sans-serif;\n"
      "  font-size: 12px;\n"
      "  padding: 50px 100px 50px 100px;\n"
      "}\n"
      "#content {\n"
      "  background-color: #cfcece;\n"
      "  border: 1px solid #000;\n"
      "  padding: 0px 40px 40px 40px;\n"
      "}\n"
      "</style>\n"
      "<script>\n"
      "  if(window.location.hash && %d) {\n"
      "    var hash = window.location.hash.substring(1);\n"
      "    window.location.search = hash;\n"
      "  }\n"
      "</script>\n"
      "</head>\n"
      "<body><div id=\"content\">\n"
      "<div style=\"font-size: 42pt; font-weight: bold; color: white; text-align: right;\">%s</div>\n"
      "%s\n"
      "</div>\n"
      "</body>\n"
      "</html>";

static void _request_finished_callback(SoupServer *server, SoupServerMessage *message, gpointer user_data)
{
  dt_http_server_kill((dt_http_server_t *)user_data);
}

// this is always in the gui thread
static void _new_connection(SoupServer *server, SoupServerMessage *msg, const char *path, GHashTable *query,
                            gpointer user_data)
{
  _connection_t *params = (_connection_t *)user_data;
  gboolean res = TRUE;

  if(soup_server_message_get_method(msg) != SOUP_METHOD_GET)
  {
    soup_server_message_set_status(msg, SOUP_STATUS_NOT_IMPLEMENTED, NULL);
    goto end;
  }

  char *page_title = g_strdup_printf(_("darktable » %s"), params->id);
  const char *title = _(params->id);
  const char *body = _("<h1>Sorry,</h1><p>something went wrong. Please try again.</p>");

  res = params->callback(query, params->user_data);

  if(res)
    body = _("<h1>Thank you,</h1><p>everything should have worked, you can <b>close</b> your browser now and "
             "<b>go back</b> to darktable.</p>");


  char *resp_body = g_strdup_printf(reply, page_title, res ? 0 : 1, title, body);
  size_t resp_length = strlen(resp_body);
  g_free(page_title);

  soup_server_message_set_status(msg, SOUP_STATUS_OK, NULL);
  soup_server_message_set_response(msg, "text/html", SOUP_MEMORY_TAKE, resp_body, resp_length);

end:
  if(res)
  {
    dt_http_server_t *http_server = params->server;
    soup_server_remove_handler(server, path);
    g_signal_connect(G_OBJECT(server), "request-finished", G_CALLBACK(_request_finished_callback), http_server);
  }
}

dt_http_server_t *dt_http_server_create(const int *ports, const int n_ports, const char *id,
                                        const dt_http_server_callback callback, gpointer user_data)
{
  SoupServer *httpserver = NULL;
  int port = 0;

  dt_print(DT_DEBUG_CONTROL, "[http server] using libsoup3 api\n");

  httpserver = soup_server_new("server-header", "darktable internal server", NULL);
  if(httpserver == NULL)
  {
    fprintf(stderr, "error: couldn't create libsoup httpserver\n");
    return NULL;
  }

  for(int i = 0; i < n_ports; i++)
  {
    port = ports[i];

    if(soup_server_listen_local(httpserver, port, 0, NULL)) break;

    port = 0;
  }
  if(port == 0)
  {
    fprintf(stderr, "error: can't bind to any port from our pool\n");
    return NULL;
  }

  dt_http_server_t *server = (dt_http_server_t *)malloc(sizeof(dt_http_server_t));
  server->server = httpserver;

  _connection_t *params = (_connection_t *)malloc(sizeof(_connection_t));
  params->id = id;
  params->server = server;
  params->callback = callback;
  params->user_data = user_data;

  char *path = g_strdup_printf("/%s", id);
  server->url = g_strdup_printf("http://localhost:%d/%s", port, id);

  soup_server_add_handler(httpserver, path, _new_connection, params, free);

  g_free(path);

  dt_print(DT_DEBUG_CONTROL, "[http server] listening on %s\n", server->url);

  return server;
}

void dt_http_server_kill(dt_http_server_t *server)
{
  if(server->server)
  {
    soup_server_disconnect(server->server);
    g_object_unref(server->server);
    server->server = NULL;
  }
  g_free(server->url);
  server->url = NULL;
  free(server);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

