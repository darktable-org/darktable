/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

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

#include "mcp/mcp_jsonrpc.h"
#include "mcp/mcp_tools.h"

#include <json-glib/json-glib.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include "win/getdelim.h" // getline() is not declared by MinGW
#endif

#define SERVER_NAME "darktable-mcp"
#define SERVER_VERSION "0.1.0"
#define PROTOCOL_VERSION "2025-06-18"

// JSON-RPC error codes
#define ERR_PARSE -32700
#define ERR_INVALID_REQUEST -32600
#define ERR_METHOD_NOT_FOUND -32601
#define ERR_INVALID_PARAMS -32602

// whether the current peer uses LSP-style Content-Length framing (else the MCP
// stdio default of one JSON object per line)
static gboolean _use_framing = FALSE;

// ---------------------------------------------------------------------------
// output
// ---------------------------------------------------------------------------

static void _send_node(FILE *out, JsonNode *root)
{
  JsonGenerator *gen = json_generator_new();
  json_generator_set_root(gen, root);
  gsize len = 0;
  gchar *s = json_generator_to_data(gen, &len);
  if(_use_framing) fprintf(out, "Content-Length: %zu\r\n\r\n", (size_t)len);
  fwrite(s, 1, len, out);
  if(!_use_framing) fputc('\n', out);
  fflush(out);
  g_free(s);
  g_object_unref(gen);
}

static JsonNode *_empty_object(void)
{
  JsonNode *n = json_node_new(JSON_NODE_OBJECT);
  json_node_take_object(n, json_object_new());
  return n;
}

// builds { jsonrpc, id, result }, takes ownership of `result`
static void _send_result(FILE *out, JsonNode *id, JsonNode *result)
{
  JsonObject *o = json_object_new();
  json_object_set_string_member(o, "jsonrpc", "2.0");
  if(id) json_object_set_member(o, "id", json_node_copy(id));
  else    json_object_set_null_member(o, "id");
  json_object_set_member(o, "result", result);
  JsonNode *root = json_node_new(JSON_NODE_OBJECT);
  json_node_take_object(root, o);
  _send_node(out, root);
  json_node_unref(root);
}

static void _send_error(FILE *out, JsonNode *id, int code, const char *msg)
{
  JsonObject *err = json_object_new();
  json_object_set_int_member(err, "code", code);
  json_object_set_string_member(err, "message", msg ? msg : "");

  JsonObject *o = json_object_new();
  json_object_set_string_member(o, "jsonrpc", "2.0");
  if(id) json_object_set_member(o, "id", json_node_copy(id));
  else    json_object_set_null_member(o, "id");
  json_object_set_object_member(o, "error", err);

  JsonNode *root = json_node_new(JSON_NODE_OBJECT);
  json_node_take_object(root, o);
  _send_node(out, root);
  json_node_unref(root);
}

// ---------------------------------------------------------------------------
// method results
// ---------------------------------------------------------------------------

static JsonNode *_initialize_result(void)
{
  JsonObject *tools = json_object_new();
  json_object_set_boolean_member(tools, "listChanged", FALSE);

  JsonObject *caps = json_object_new();
  json_object_set_object_member(caps, "tools", tools);

  JsonObject *info = json_object_new();
  json_object_set_string_member(info, "name", SERVER_NAME);
  json_object_set_string_member(info, "version", SERVER_VERSION);

  JsonObject *o = json_object_new();
  json_object_set_string_member(o, "protocolVersion", PROTOCOL_VERSION);
  json_object_set_object_member(o, "capabilities", caps);
  json_object_set_object_member(o, "serverInfo", info);

  JsonNode *n = json_node_new(JSON_NODE_OBJECT);
  json_node_take_object(n, o);
  return n;
}

static JsonNode *_tools_list_result(void)
{
  JsonObject *o = json_object_new();
  json_object_set_member(o, "tools", mcp_tools_list_node());
  JsonNode *n = json_node_new(JSON_NODE_OBJECT);
  json_node_take_object(n, o);
  return n;
}

static void _handle_tools_call(FILE *out, JsonNode *id, JsonObject *req)
{
  JsonObject *params = NULL;
  if(json_object_has_member(req, "params"))
  {
    JsonNode *pn = json_object_get_member(req, "params");
    if(JSON_NODE_HOLDS_OBJECT(pn)) params = json_node_get_object(pn);
  }
  const char *name = params && json_object_has_member(params, "name")
                       ? json_object_get_string_member(params, "name") : NULL;
  if(!name) { _send_error(out, id, ERR_INVALID_PARAMS, "missing tool name"); return; }

  JsonObject *args = NULL;
  if(params && json_object_has_member(params, "arguments"))
  {
    JsonNode *an = json_object_get_member(params, "arguments");
    if(JSON_NODE_HOLDS_OBJECT(an)) args = json_node_get_object(an);
  }

  gboolean found = FALSE;
  JsonNode *result = mcp_tools_call_node(name, args, &found);
  if(!found)
  {
    if(result) json_node_unref(result);
    _send_error(out, id, ERR_METHOD_NOT_FOUND, "unknown tool");
    return;
  }
  _send_result(out, id, result);
}

// ---------------------------------------------------------------------------
// read loop
// ---------------------------------------------------------------------------

static void _rstrip(char *s, ssize_t *n)
{
  while(*n > 0 && (s[*n - 1] == '\n' || s[*n - 1] == '\r')) s[--(*n)] = '\0';
}

void mcp_jsonrpc_loop(FILE *in, FILE *out)
{
  char *line = NULL;
  size_t cap = 0;
  ssize_t n;
  gboolean done = FALSE;

  while(!done && (n = getline(&line, &cap, in)) != -1)
  {
    _rstrip(line, &n);
    if(n == 0) continue; // blank line between messages

    char *msg = line;
    char *framed_msg = NULL;

    if(g_ascii_strncasecmp(line, "Content-Length:", 15) == 0)
    {
      _use_framing = TRUE;
      const long clen = strtol(line + 15, NULL, 10);
      // consume any remaining header lines up to the blank separator
      while((n = getline(&line, &cap, in)) != -1)
      {
        _rstrip(line, &n);
        if(n == 0) break;
      }
      if(clen <= 0) continue;
      framed_msg = g_malloc(clen + 1);
      const size_t got = fread(framed_msg, 1, clen, in);
      framed_msg[got] = '\0';
      msg = framed_msg;
    }

    JsonParser *parser = json_parser_new();
    GError *e = NULL;
    if(!json_parser_load_from_data(parser, msg, -1, &e))
    {
      _send_error(out, NULL, ERR_PARSE, e ? e->message : "parse error");
      if(e) g_error_free(e);
      g_object_unref(parser);
      g_free(framed_msg);
      continue;
    }

    JsonNode *root = json_parser_get_root(parser);
    if(!root || !JSON_NODE_HOLDS_OBJECT(root))
    {
      _send_error(out, NULL, ERR_INVALID_REQUEST, "not a JSON-RPC object");
      g_object_unref(parser);
      g_free(framed_msg);
      continue;
    }

    JsonObject *req = json_node_get_object(root);
    const char *method = json_object_has_member(req, "method")
                           ? json_object_get_string_member(req, "method") : NULL;
    JsonNode *id = json_object_has_member(req, "id")
                     ? json_object_get_member(req, "id") : NULL;

    if(!method)
    {
      if(id) _send_error(out, id, ERR_INVALID_REQUEST, "missing method");
    }
    else if(!strcmp(method, "initialize"))
      _send_result(out, id, _initialize_result());
    else if(!strcmp(method, "tools/list"))
      _send_result(out, id, _tools_list_result());
    else if(!strcmp(method, "tools/call"))
      _handle_tools_call(out, id, req);
    else if(!strcmp(method, "ping"))
      _send_result(out, id, _empty_object());
    else if(g_str_has_prefix(method, "notifications/"))
      { /* notifications get no response */ }
    else if(!strcmp(method, "shutdown"))
    {
      _send_result(out, id, _empty_object());
      done = TRUE;
    }
    else if(id)
      _send_error(out, id, ERR_METHOD_NOT_FOUND, "method not found");

    g_object_unref(parser);
    g_free(framed_msg);
  }

  g_free(line);
}
