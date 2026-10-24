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

// remote inference client for the loopback ONNX worker selected by
// DT_AI_REMOTE (numeric HOST:PORT, loopback only — SSH provides
// transport security, see dev-doc/AI_REMOTE.md). wire protocol v1

#include "backend_remote.h"
#include "common/darktable.h"

#include <errno.h>
#include <glib.h>
#include <json-glib/json-glib.h>
#include <string.h>

gboolean dt_ai_remote_enabled(void)
{
  const char *env = g_getenv("DT_AI_REMOTE");
  return env && *env;
}

#ifdef _WIN32
// the worker is a loopback service reached through SSH on the homelab
// host; Windows builds keep the DT_AI_REMOTE gate loud but unsupported.
// DT_AI_REMOTE set => every AI load fails here, never falls back locally
dt_ai_remote_model_t *dt_ai_remote_connect_load(
    const char *model_id, const char *model_file,
    const gboolean cpu,
    const dt_ai_opt_level_t opt_level,
    const dt_ai_dim_override_t *dim_overrides, const int n_overrides)
{
  (void)cpu;
  dt_print(DT_DEBUG_ALWAYS,
           "[ai remote] remote inference is not supported on this platform "
           "(DT_AI_REMOTE='%s', model '%s')",
           g_getenv("DT_AI_REMOTE") ? g_getenv("DT_AI_REMOTE") : "",
           model_id ? model_id : "?");
  return NULL;
}

int dt_ai_remote_run(dt_ai_remote_model_t *model,
                     dt_ai_tensor_t *inputs, const int num_inputs,
                     dt_ai_tensor_t *outputs, const int num_outputs)
{
  return -1;
}

void dt_ai_remote_free(dt_ai_remote_model_t *model)
{
  g_free(model);
}

#else // !_WIN32 — POSIX client

#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

// element size of a wire dtype, 0 for anything not on the wire
static int _dtype_size(const int64_t type)
{
  switch(type)
  {
    case DT_AI_FLOAT:
    case DT_AI_INT32:
      return 4;
    case DT_AI_UINT8:
    case DT_AI_INT8:
      return 1;
    case DT_AI_INT64:
      return 8;
    case DT_AI_FLOAT16:
      return 2;
    default:
      return 0;
  }
}

static gboolean _valid_dtype(const int64_t type)
{
  return _dtype_size(type) != 0;
}

// overflow-checked shape product; FALSE on non-positive or overflowing dims
static gboolean _shape_elems(const int64_t *shape, const int ndim, int64_t *out)
{
  int64_t n = 1;
  for(int i = 0; i < ndim; i++)
  {
    if(shape[i] <= 0 || n > INT64_MAX / shape[i]) return FALSE;
    n *= shape[i];
  }
  *out = n;
  return TRUE;
}

// float16 -> float32, bit-exact with the local backend's conversion
// (handles zero, denormals, infinity and NaN)
static float _half_to_float(const uint16_t h)
{
  const uint32_t sign = (h >> 15) & 1;
  const uint32_t exp = (h >> 10) & 0x1F;
  const uint32_t mant = h & 0x3FF;
  uint32_t result;

  if(exp == 0)
  {
    if(mant == 0)
    {
      result = sign << 31;
    }
    else
    {
      uint32_t m = mant;
      int e = -1;
      while(!(m & 0x400))
      {
        m <<= 1;
        e--;
      }
      m &= 0x3FF;
      result = (sign << 31) | (uint32_t)(e + 127 - 14 + 1) << 23 | (m << 13);
    }
  }
  else if(exp == 31)
  {
    result = (sign << 31) | 0x7F800000 | (mant << 13);
  }
  else
  {
    result = (sign << 31) | (exp + 127 - 15) << 23 | (mant << 13);
  }

  float f;
  memcpy(&f, &result, sizeof(f));
  return f;
}

// parse numeric "HOST:PORT" (HOST may be bracketed IPv6), refuse
// anything that is not a loopback address. returns fd or -1
static int _connect_endpoint(const char *spec)
{
  const char *colon = strrchr(spec, ':');
  if(!colon || colon == spec || !colon[1])
  {
    dt_print(DT_DEBUG_AI, "[ai remote] DT_AI_REMOTE must be HOST:PORT, got '%s'", spec);
    return -1;
  }
  char *host = g_strndup(spec, colon - spec);
  const size_t hl = strlen(host);
  if(hl >= 2 && host[0] == '[' && host[hl - 1] == ']')
  {
    memmove(host, host + 1, hl - 2);
    host[hl - 2] = '\0';
  }

  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;
  struct addrinfo *res = NULL;
  const int rc = getaddrinfo(host, colon + 1, &hints, &res);
  g_free(host);
  if(rc != 0 || !res)
  {
    dt_print(DT_DEBUG_AI,
             "[ai remote] DT_AI_REMOTE must be a numeric loopback address, got '%s'",
             spec);
    return -1;
  }

  int fd = -1;
  for(struct addrinfo *ai = res; ai && fd < 0; ai = ai->ai_next)
  {
    gboolean loopback = FALSE;
    if(ai->ai_family == AF_INET)
    {
      const struct sockaddr_in *sin = (const struct sockaddr_in *)ai->ai_addr;
      loopback = (ntohl(sin->sin_addr.s_addr) >> 24) == 127;
    }
    else if(ai->ai_family == AF_INET6)
    {
      const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *)ai->ai_addr;
      loopback = IN6_IS_ADDR_LOOPBACK(&sin6->sin6_addr);
    }
    if(!loopback)
    {
      dt_print(DT_DEBUG_AI, "[ai remote] refusing non-loopback address '%s'", spec);
      continue;
    }

    fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if(fd < 0) continue;

    const struct timeval tv = { DT_AI_REMOTE_TIMEOUT_S, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    const int one = 1;
#ifdef SO_NOSIGPIPE
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
#ifdef TCP_NODELAY
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#endif

    if(connect(fd, ai->ai_addr, ai->ai_addrlen) != 0)
    {
      const int err = errno;
      close(fd);
      fd = -1;
      dt_print(DT_DEBUG_AI, "[ai remote] connect to %s failed: %s", spec, g_strerror(err));
    }
  }
  freeaddrinfo(res);
  return fd;
}

// a dead connection cannot be recovered mid-frame: close it and let
// every later call on this model fail
static void _drop_connection(dt_ai_remote_model_t *m)
{
  if(m->fd >= 0) close(m->fd);
  m->fd = -1;
}

static gboolean _send_all(const int fd, const void *buf, const size_t len)
{
  const char *p = (const char *)buf;
  size_t off = 0;
  while(off < len)
  {
    const ssize_t n = send(fd, p + off, len - off, MSG_NOSIGNAL);
    if(n < 0)
    {
      if(errno == EINTR) continue;
      return FALSE;
    }
    off += (size_t)n;
  }
  return TRUE;
}

static gboolean _recv_all(const int fd, void *buf, const size_t len)
{
  char *p = (char *)buf;
  size_t off = 0;
  while(off < len)
  {
    const ssize_t n = recv(fd, p + off, len - off, 0);
    if(n == 0) return FALSE;
    if(n < 0)
    {
      if(errno == EINTR) continue;
      return FALSE;
    }
    off += (size_t)n;
  }
  return TRUE;
}

// one network-endian uint32 length prefix + JSON body; the prefix is
// bounds-checked before any payload is read
static gboolean _send_frame(const int fd, const char *json, const size_t len)
{
  if(len == 0 || len > DT_AI_REMOTE_MAX_JSON) return FALSE;
  const guint32 hdr = GUINT32_TO_BE((guint32)len);
  return _send_all(fd, &hdr, sizeof(hdr)) && _send_all(fd, json, len);
}

static gchar *_recv_frame(const int fd)
{
  guint32 hdr = 0;
  if(!_recv_all(fd, &hdr, sizeof(hdr))) return NULL;
  const guint32 len = GUINT32_FROM_BE(hdr);
  if(len == 0 || len > DT_AI_REMOTE_MAX_JSON) return NULL;
  gchar *buf = g_malloc(len + 1);
  if(!_recv_all(fd, buf, len))
  {
    g_free(buf);
    return NULL;
  }
  buf[len] = '\0';
  return buf;
}

// receive one frame and validate it as a JSON object. on success
// *parser owns the tree (g_object_unref when done); NULL = dead or
// malformed (caller drops the connection)
static JsonObject *_recv_object(const int fd, JsonParser **parser)
{
  *parser = NULL;
  gchar *frame = _recv_frame(fd);
  if(!frame) return NULL;

  JsonParser *p = json_parser_new();
  JsonObject *obj = NULL;
  if(json_parser_load_from_data(p, frame, -1, NULL))
  {
    JsonNode *root = json_parser_get_root(p);
    if(root && JSON_NODE_HOLDS_OBJECT(root)) obj = json_node_get_object(root);
  }
  g_free(frame);
  if(!obj)
  {
    dt_print(DT_DEBUG_AI, "[ai remote] malformed response frame — dropping connection");
    g_object_unref(p);
  }
  else
  {
    *parser = p;
  }
  return obj;
}

static const char *_error_string(JsonObject *obj)
{
  JsonNode *n = json_object_get_member(obj, "error");
  if(n && JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == G_TYPE_STRING)
    return json_node_get_string(n);
  return "unspecified worker error";
}

// 1 = ok, 0 = worker refused (logged; server closes), -1 = malformed frame
static int _check_ok(JsonObject *obj)
{
  JsonNode *n = json_object_get_member(obj, "ok");
  if(!n || !JSON_NODE_HOLDS_VALUE(n) || json_node_get_value_type(n) != G_TYPE_BOOLEAN)
    return -1;
  if(json_node_get_boolean(n)) return 1;
  dt_print(DT_DEBUG_AI, "[ai remote] worker refused: %s", _error_string(obj));
  return 0;
}

// parse one descriptor element. -1 dims are accepted only as metadata /
// allocation requests; a "bytes" member must exactly equal the shape
// product times the dtype size. 0 ok, -1 invalid
static int _parse_io_element(JsonObject *e, dt_ai_remote_io_t *io,
                             const gboolean allow_dynamic)
{
  memset(io, 0, sizeof(*io));

  JsonNode *tn = json_object_get_member(e, "type");
  if(!tn || !JSON_NODE_HOLDS_VALUE(tn)) return -1;
  const int64_t type = json_node_get_int(tn);
  if(!_valid_dtype(type)) return -1;

  JsonNode *sn = json_object_get_member(e, "shape");
  if(!sn || !JSON_NODE_HOLDS_ARRAY(sn)) return -1;
  JsonArray *shape = json_node_get_array(sn);
  const guint ndim = json_array_get_length(shape);
  // rank 0 = scalar tensor (e.g. an int64 token count)
  if(ndim > DT_AI_REMOTE_MAX_RANK) return -1;

  for(guint d = 0; d < ndim; d++)
  {
    JsonNode *dn = json_array_get_element(shape, d);
    if(!dn || !JSON_NODE_HOLDS_VALUE(dn)) return -1;
    const int64_t v = json_node_get_int(dn);
    if(v == -1 && allow_dynamic)
      io->shape[d] = -1;
    else if(v > 0)
      io->shape[d] = v;
    else
      return -1;
  }
  io->type = (dt_ai_dtype_t)type;
  io->ndim = (int)ndim;

  io->name = NULL;
  JsonNode *nn = json_object_get_member(e, "name");
  if(nn && JSON_NODE_HOLDS_VALUE(nn) && json_node_get_value_type(nn) == G_TYPE_STRING)
    io->name = g_strdup(json_node_get_string(nn));

  io->bytes = 0;
  JsonNode *bn = json_object_get_member(e, "bytes");
  if(bn && JSON_NODE_HOLDS_VALUE(bn))
  {
    const int64_t bytes = json_node_get_int(bn);
    const size_t esize = (size_t)_dtype_size(type);
    int64_t elems = 0;
    if(bytes <= 0 || esize == 0 || !_shape_elems(io->shape, io->ndim, &elems)
       || (guint64)elems > (guint64)(INT64_MAX / (int64_t)esize)
       || bytes != elems * (int64_t)esize)
      return -1;
    io->bytes = bytes;
  }

  return 0;
}

// load-metadata "inputs"/"outputs": names required, dynamic dims
// allowed, no bytes. returns count or -1
static int _parse_meta_array(JsonArray *arr, dt_ai_remote_io_t *out, const int max)
{
  const guint n = json_array_get_length(arr);
  if(n < 1 || n > (guint)max) return -1;
  for(guint i = 0; i < n; i++)
  {
    JsonNode *en = json_array_get_element(arr, i);
    if(!en || !JSON_NODE_HOLDS_OBJECT(en)) return -1;
    if(_parse_io_element(json_node_get_object(en), &out[i], TRUE) != 0) return -1;
    if(!out[i].name) return -1;
  }
  return (int)n;
}

// run-response "outputs": concrete shapes and a checked bytes field
// on every element. returns count or -1
static int _parse_tensor_array(JsonArray *arr, dt_ai_remote_io_t *out, const int max)
{
  const guint n = json_array_get_length(arr);
  if(n < 1 || n > (guint)max) return -1;
  for(guint i = 0; i < n; i++)
  {
    JsonNode *en = json_array_get_element(arr, i);
    if(!en || !JSON_NODE_HOLDS_OBJECT(en)) return -1;
    if(_parse_io_element(json_node_get_object(en), &out[i], FALSE) != 0) return -1;
    if(out[i].bytes <= 0) return -1;
  }
  return (int)n;
}

static gchar *_json_to_string(JsonBuilder *b)
{
  JsonNode *root = json_builder_get_root(b);
  JsonGenerator *gen = json_generator_new();
  json_generator_set_root(gen, root);
  gchar *str = json_generator_to_data(gen, NULL);
  g_object_unref(gen);
  json_node_free(root);
  g_object_unref(b);
  return str;
}

static gchar *_build_load_request(const char *model_id, const char *model_file,
                                  const gboolean cpu,
                                  const int opt_level,
                                  const dt_ai_dim_override_t *dim_overrides,
                                  const int n_overrides)
{
  JsonBuilder *b = json_builder_new();
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "v");
  json_builder_add_int_value(b, 1);
  json_builder_set_member_name(b, "op");
  json_builder_add_string_value(b, "load");
  json_builder_set_member_name(b, "model");
  json_builder_add_string_value(b, model_id);
  json_builder_set_member_name(b, "file");
  json_builder_add_string_value(b, model_file);
  json_builder_set_member_name(b, "opt");
  json_builder_add_int_value(b, opt_level);
  json_builder_set_member_name(b, "cpu");
  json_builder_add_boolean_value(b, cpu);
  json_builder_set_member_name(b, "overrides");
  json_builder_begin_array(b);
  for(int i = 0; i < n_overrides; i++)
  {
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "name");
    json_builder_add_string_value(b, dim_overrides[i].name);
    json_builder_set_member_name(b, "value");
    json_builder_add_int_value(b, dim_overrides[i].value);
    json_builder_end_object(b);
  }
  json_builder_end_array(b);
  json_builder_end_object(b);
  return _json_to_string(b);
}

// caller dtype may differ from the model dtype only inside the
// float32<->float16 pair (matches the local backend; no integer casts)
static gboolean _dtype_compatible(const dt_ai_dtype_t caller, const dt_ai_dtype_t model)
{
  return caller == model || (caller == DT_AI_FLOAT && model == DT_AI_FLOAT16);
}

// build the run request for the caller's tensors. inputs must be fully
// concrete — we read exactly shape product x dtype bytes from them.
// outputs with data == NULL go out as allocation requests (bytes 0,
// -1 dims kept, rank preserved). NULL on any invalid caller tensor
static gchar *_build_run_request(dt_ai_tensor_t *inputs, const int num_inputs,
                                 dt_ai_tensor_t *outputs, const int num_outputs)
{
  JsonBuilder *b = json_builder_new();
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "v");
  json_builder_add_int_value(b, 1);
  json_builder_set_member_name(b, "op");
  json_builder_add_string_value(b, "run");

  guint64 total = 0;
  json_builder_set_member_name(b, "inputs");
  json_builder_begin_array(b);
  for(int i = 0; i < num_inputs; i++)
  {
    int64_t elems = 0;
    if(!_valid_dtype(inputs[i].type) || !inputs[i].shape
       || inputs[i].ndim < 0 || inputs[i].ndim > DT_AI_REMOTE_MAX_RANK
       || !_shape_elems(inputs[i].shape, inputs[i].ndim, &elems))
      goto invalid;
    const int64_t bytes = elems * _dtype_size(inputs[i].type);
    total += (guint64)bytes;
    if(bytes <= 0 || total > DT_AI_REMOTE_MAX_BYTES) goto invalid;

    json_builder_begin_object(b);
    json_builder_set_member_name(b, "type");
    json_builder_add_int_value(b, inputs[i].type);
    json_builder_set_member_name(b, "shape");
    json_builder_begin_array(b);
    for(int d = 0; d < inputs[i].ndim; d++)
      json_builder_add_int_value(b, inputs[i].shape[d]);
    json_builder_end_array(b);
    json_builder_set_member_name(b, "bytes");
    json_builder_add_int_value(b, bytes);
    json_builder_end_object(b);
  }
  json_builder_end_array(b);

  json_builder_set_member_name(b, "outputs");
  json_builder_begin_array(b);
  for(int i = 0; i < num_outputs; i++)
  {
    if(!_valid_dtype(outputs[i].type) || !outputs[i].shape
       || outputs[i].ndim < 0 || outputs[i].ndim > DT_AI_REMOTE_MAX_RANK)
      goto invalid;
    int64_t elems = 0;
    const gboolean allocated = outputs[i].data != NULL
      && _shape_elems(outputs[i].shape, outputs[i].ndim, &elems);
    const int64_t bytes = allocated ? elems * _dtype_size(outputs[i].type) : 0;
    total += (guint64)bytes;
    if(bytes < 0 || total > DT_AI_REMOTE_MAX_BYTES) goto invalid;

    json_builder_begin_object(b);
    json_builder_set_member_name(b, "type");
    json_builder_add_int_value(b, outputs[i].type);
    json_builder_set_member_name(b, "shape");
    json_builder_begin_array(b);
    for(int d = 0; d < outputs[i].ndim; d++)
      json_builder_add_int_value(b, outputs[i].shape[d]);
    json_builder_end_array(b);
    json_builder_set_member_name(b, "bytes");
    json_builder_add_int_value(b, bytes);
    json_builder_end_object(b);
  }
  json_builder_end_array(b);
  json_builder_end_object(b);
  return _json_to_string(b);

invalid:
  g_object_unref(b);
  return NULL;
}

dt_ai_remote_model_t *dt_ai_remote_connect_load(
    const char *model_id, const char *model_file,
    const gboolean cpu,
    const dt_ai_opt_level_t opt_level,
    const dt_ai_dim_override_t *dim_overrides, const int n_overrides)
{
  const char *spec = g_getenv("DT_AI_REMOTE");
  if(!spec || !*spec || !model_id || !*model_id) return NULL;

  // basename guards on everything the request carries; the worker
  // re-checks both against its models root
  if(strchr(model_id, '/') || strstr(model_id, ".."))
  {
    dt_print(DT_DEBUG_AI, "[ai remote] model id must be a directory basename: '%s'",
             model_id);
    return NULL;
  }
  const char *file = model_file ? model_file : "model.onnx";
  if(strchr(file, '/') || strstr(file, "..") || !g_str_has_suffix(file, ".onnx"))
  {
    dt_print(DT_DEBUG_AI, "[ai remote] model file must be a basename '*.onnx': '%s'",
             file);
    return NULL;
  }
  if(n_overrides < 0 || n_overrides > DT_AI_REMOTE_MAX_OVERRIDES)
  {
    dt_print(DT_DEBUG_AI, "[ai remote] too many dim overrides (%d)", n_overrides);
    return NULL;
  }
  for(int i = 0; i < n_overrides; i++)
  {
    if(!dim_overrides || !dim_overrides[i].name || !*dim_overrides[i].name
       || dim_overrides[i].value <= 0)
    {
      dt_print(DT_DEBUG_AI, "[ai remote] invalid dim override #%d", i);
      return NULL;
    }
  }

  const gint64 t0 = g_get_monotonic_time();
  const int fd = _connect_endpoint(spec);
  if(fd < 0) return NULL;

  dt_ai_remote_model_t *m = NULL;
  JsonParser *parser = NULL;
  JsonObject *obj = NULL;
  dt_ai_remote_io_t inputs[DT_AI_REMOTE_MAX_IO] = { { 0 } };
  dt_ai_remote_io_t outputs[DT_AI_REMOTE_MAX_IO] = { { 0 } };
  int n_in = 0, n_out = 0;

  gchar *request = _build_load_request(model_id, file, cpu, (int)opt_level,
                                       dim_overrides, n_overrides);
  const gboolean sent = request && _send_frame(fd, request, strlen(request));
  g_free(request);

  if(sent && (obj = _recv_object(fd, &parser)) && _check_ok(obj) == 1)
  {
    JsonNode *in_node = json_object_get_member(obj, "inputs");
    JsonNode *out_node = json_object_get_member(obj, "outputs");
    if(in_node && JSON_NODE_HOLDS_ARRAY(in_node)
       && out_node && JSON_NODE_HOLDS_ARRAY(out_node))
    {
      const int ni = _parse_meta_array(json_node_get_array(in_node),
                                       inputs, DT_AI_REMOTE_MAX_IO);
      const int no = (ni > 0) ? _parse_meta_array(json_node_get_array(out_node),
                                                  outputs, DT_AI_REMOTE_MAX_IO) : -1;
      if(ni > 0 && no > 0)
      {
        n_in = ni;
        n_out = no;
        m = g_new0(dt_ai_remote_model_t, 1);
        m->fd = fd;
        g_mutex_init(&m->lock);
        m->n_inputs = n_in;
        m->n_outputs = n_out;
        memcpy(m->inputs, inputs, sizeof(*inputs) * (size_t)n_in);
        memcpy(m->outputs, outputs, sizeof(*outputs) * (size_t)n_out);
      }
    }
  }

  if(m)
  {
    JsonNode *pn = json_object_get_member(obj, "provider");
    const char *provider = (pn && JSON_NODE_HOLDS_VALUE(pn)
                            && json_node_get_value_type(pn) == G_TYPE_STRING)
                               ? json_node_get_string(pn) : "unknown";
    dt_print(DT_DEBUG_AI,
             "[ai remote] loaded %s/%s on %s (%d in, %d out) in %.3fs",
             model_id, file, provider, n_in, n_out,
             (g_get_monotonic_time() - t0) / 1e6);
  }
  else
  {
    close(fd);
  }

  // names of the transferred prefix now belong to m
  for(int i = m ? n_in : 0; i < DT_AI_REMOTE_MAX_IO; i++) g_free(inputs[i].name);
  for(int i = m ? n_out : 0; i < DT_AI_REMOTE_MAX_IO; i++) g_free(outputs[i].name);
  if(parser) g_object_unref(parser);
  return m;
}

int dt_ai_remote_run(dt_ai_remote_model_t *m,
                     dt_ai_tensor_t *inputs, const int num_inputs,
                     dt_ai_tensor_t *outputs, const int num_outputs)
{
  if(!m || m->fd < 0 || !inputs || !outputs) return -1;
  if(num_inputs != m->n_inputs || num_outputs != m->n_outputs)
  {
    dt_print(DT_DEBUG_AI, "[ai remote] IO count mismatch: expected %d/%d, got %d/%d",
             m->n_inputs, m->n_outputs, num_inputs, num_outputs);
    return -2;
  }

  // fail fast on dtypes the worker must not implicitly cast
  for(int i = 0; i < num_inputs; i++)
    if(!_dtype_compatible(inputs[i].type, m->inputs[i].type))
    {
      dt_print(DT_DEBUG_AI, "[ai remote] input[%d] is type %d, model wants %d",
               i, inputs[i].type, m->inputs[i].type);
      return -4;
    }
  for(int i = 0; i < num_outputs; i++)
    if(!_dtype_compatible(outputs[i].type, m->outputs[i].type))
    {
      dt_print(DT_DEBUG_AI, "[ai remote] output[%d] is type %d, model delivers %d",
               i, outputs[i].type, m->outputs[i].type);
      return -4;
    }

  gchar *request = _build_run_request(inputs, num_inputs, outputs, num_outputs);
  if(!request) return -4;

  const gint64 t0 = g_get_monotonic_time();
  int ret = 0;

  g_mutex_lock(&m->lock);

  // from here every failure invalidates the connection: a frame or
  // payload may be half-consumed, so the socket is dropped and the
  // model context goes dead
  if(!_send_frame(m->fd, request, strlen(request)))
    ret = -3;

  for(int i = 0; i < num_inputs && ret == 0; i++)
  {
    int64_t elems = 0;
    if(!_shape_elems(inputs[i].shape, inputs[i].ndim, &elems)
       || !_send_all(m->fd, inputs[i].data,
                     (size_t)(elems * _dtype_size(inputs[i].type))))
      ret = -3;
  }

  JsonParser *parser = NULL;
  JsonObject *obj = NULL;
  dt_ai_remote_io_t resp[DT_AI_REMOTE_MAX_IO] = { { 0 } };
  gboolean allocated[DT_AI_REMOTE_MAX_IO] = { FALSE };

  if(ret == 0)
  {
    obj = _recv_object(m->fd, &parser);
    if(!obj || _check_ok(obj) != 1)
      ret = -3;
  }

  if(ret == 0)
  {
    JsonNode *on = json_object_get_member(obj, "outputs");
    if(!on || !JSON_NODE_HOLDS_ARRAY(on)
       || _parse_tensor_array(json_node_get_array(on), resp, num_outputs)
            != num_outputs)
    {
      dt_print(DT_DEBUG_AI, "[ai remote] malformed run response — dropping connection");
      ret = -3;
    }
  }

  // validate every descriptor against the caller's buffers BEFORE
  // reading any payload byte, then copy exactly what was validated
  guint64 total_bytes = 0;
  for(int i = 0; i < num_outputs && ret == 0; i++)
  {
    const dt_ai_remote_io_t *r = &resp[i];
    int64_t elems = 0;
    gboolean bad = !_dtype_compatible(outputs[i].type, r->type)
      || r->ndim > outputs[i].ndim
      || !_shape_elems(r->shape, r->ndim, &elems)
      || (guint64)r->bytes > DT_AI_REMOTE_MAX_BYTES
      || (total_bytes += (guint64)r->bytes) > DT_AI_REMOTE_MAX_BYTES;

    if(!bad && outputs[i].data != NULL)
    {
      // a caller-provided buffer must have a resolvable shape large
      // enough for the whole payload; otherwise its size is unknown
      // and nothing may be written to it
      int64_t capacity = 0;
      bad = !_shape_elems(outputs[i].shape, outputs[i].ndim, &capacity)
        || elems > capacity;
    }
    if(bad)
    {
      dt_print(DT_DEBUG_AI,
               "[ai remote] output[%d] descriptor violates the caller buffer", i);
      ret = -3;
      break;
    }
  }

  for(int i = 0; i < num_outputs && ret == 0; i++)
  {
    const dt_ai_remote_io_t *r = &resp[i];
    int64_t elems = 0;
    _shape_elems(r->shape, r->ndim, &elems); // validated above
    const guint64 want
      = (guint64)elems * (guint64)_dtype_size(outputs[i].type);

    void *dst = outputs[i].data;
    const gboolean allocated_here = dst == NULL;
    if(allocated_here)
    {
      allocated[i] = TRUE;
      if(want > SIZE_MAX || !(dst = g_try_malloc((size_t)want)))
      {
        ret = -3;
        break;
      }
    }

    if(r->type == DT_AI_FLOAT16 && outputs[i].type == DT_AI_FLOAT)
    {
      uint16_t *tmp = g_malloc((size_t)r->bytes);
      if(!_recv_all(m->fd, tmp, (size_t)r->bytes))
      {
        g_free(tmp);
        if(allocated_here) g_free(dst);
        ret = -3;
        break;
      }
      float *f = (float *)dst;
      for(int64_t k = 0; k < elems; k++) f[k] = _half_to_float(tmp[k]);
      g_free(tmp);
    }
    else if(!_recv_all(m->fd, dst, (size_t)r->bytes))
    {
      if(allocated_here) g_free(dst);
      ret = -3;
      break;
    }

    outputs[i].data = dst;
  }

  if(ret != 0)
  {
    for(int i = 0; i < num_outputs; i++)
      if(allocated[i]) g_clear_pointer(&outputs[i].data, g_free);
    _drop_connection(m);
  }

  if(ret == 0)
  {
    // success only: publish the resolved shapes to the caller
    for(int i = 0; i < num_outputs; i++)
    {
      memcpy(outputs[i].shape, resp[i].shape, (size_t)resp[i].ndim * sizeof(int64_t));
      outputs[i].ndim = resp[i].ndim;
    }
    dt_print(DT_DEBUG_AI, "[ai remote] run ok: %d in, %d out, %.3fs",
             num_inputs, num_outputs, (g_get_monotonic_time() - t0) / 1e6);
  }

  if(parser) g_object_unref(parser);
  g_mutex_unlock(&m->lock);
  g_free(request);
  return ret;
}

void dt_ai_remote_free(dt_ai_remote_model_t *m)
{
  if(!m) return;
  g_mutex_lock(&m->lock);
  if(m->fd >= 0) close(m->fd);
  m->fd = -1;
  g_mutex_unlock(&m->lock);
  for(int i = 0; i < DT_AI_REMOTE_MAX_IO; i++)
  {
    g_free(m->inputs[i].name);
    g_free(m->outputs[i].name);
  }
  g_mutex_clear(&m->lock);
  g_free(m);
}

#endif // _WIN32

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
