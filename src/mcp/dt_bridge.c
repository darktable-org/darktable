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

#include "mcp/dt_bridge.h"

#include "common/colorspaces.h"
#include "common/darktable.h"
#include "common/database.h"
#include "common/film.h"
#include "common/image.h"
#include "common/introspection.h"
#include "common/styles.h"
#include "common/usermanual_url.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "imageio/imageio_common.h"
#include "imageio/imageio_module.h"

#include <cairo/cairo.h>
#include <json-glib/json-glib.h>
#include <sqlite3.h>
#include <string.h>

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------

static void _seterr(char **err, const char *fmt, ...)
{
  if(!err) return;
  va_list ap;
  va_start(ap, fmt);
  *err = g_strdup_vprintf(fmt, ap);
  va_end(ap);
}

static uint8_t *_hex_to_bytes(const char *hex, size_t *outlen)
{
  if(!hex) return NULL;
  const size_t n = strlen(hex);
  if(n % 2) return NULL;
  const size_t bl = n / 2;
  uint8_t *b = g_malloc(bl ? bl : 1);
  for(size_t i = 0; i < bl; i++)
  {
    const int hi = g_ascii_xdigit_value(hex[2 * i]);
    const int lo = g_ascii_xdigit_value(hex[2 * i + 1]);
    if(hi < 0 || lo < 0)
    {
      g_free(b);
      return NULL;
    }
    b[i] = (uint8_t)((hi << 4) | lo);
  }
  *outlen = bl;
  return b;
}

static char *_bytes_to_hex(const uint8_t *b, size_t n)
{
  char *s = g_malloc(2 * n + 1);
  static const char hexd[] = "0123456789abcdef";
  for(size_t i = 0; i < n; i++)
  {
    s[2 * i] = hexd[b[i] >> 4];
    s[2 * i + 1] = hexd[b[i] & 0xf];
  }
  s[2 * n] = '\0';
  return s;
}

static dt_iop_module_so_t *_find_so(const char *op)
{
  if(!op) return NULL;
  return dt_iop_get_module_so(op);
}

// full usermanual URL for a module op, or NULL (caller frees)
static char *_doc_url(const char *op)
{
  const char *topic = dt_get_help_url(op); // pointer into a static table; do not free
  return topic ? dt_get_manual_url(topic) : NULL;
}

static void _add_doc_url(JsonBuilder *b, const char *op)
{
  char *url = _doc_url(op);
  if(url)
  {
    json_builder_set_member_name(b, "doc_url");
    json_builder_add_string_value(b, url);
    g_free(url);
  }
}

static const char *_type_name(dt_introspection_type_t t)
{
  switch(t)
  {
    case DT_INTROSPECTION_TYPE_FLOAT:  return "float";
    case DT_INTROSPECTION_TYPE_DOUBLE: return "double";
    case DT_INTROSPECTION_TYPE_INT:    return "int";
    case DT_INTROSPECTION_TYPE_UINT:   return "uint";
    case DT_INTROSPECTION_TYPE_INT8:   return "int8";
    case DT_INTROSPECTION_TYPE_UINT8:  return "uint8";
    case DT_INTROSPECTION_TYPE_SHORT:  return "short";
    case DT_INTROSPECTION_TYPE_USHORT: return "ushort";
    case DT_INTROSPECTION_TYPE_BOOL:   return "bool";
    case DT_INTROSPECTION_TYPE_ENUM:   return "enum";
    default:                           return "other";
  }
}

// is this a flat scalar leaf we can read/write generically?
static gboolean _is_scalar(dt_introspection_type_t t)
{
  switch(t)
  {
    case DT_INTROSPECTION_TYPE_FLOAT:
    case DT_INTROSPECTION_TYPE_DOUBLE:
    case DT_INTROSPECTION_TYPE_INT:
    case DT_INTROSPECTION_TYPE_UINT:
    case DT_INTROSPECTION_TYPE_INT8:
    case DT_INTROSPECTION_TYPE_UINT8:
    case DT_INTROSPECTION_TYPE_SHORT:
    case DT_INTROSPECTION_TYPE_USHORT:
    case DT_INTROSPECTION_TYPE_BOOL:
    case DT_INTROSPECTION_TYPE_ENUM:
      return TRUE;
    default:
      return FALSE;
  }
}

// read the scalar at p (of introspection type t) and add it to the builder
static void _add_value(JsonBuilder *b, dt_introspection_field_t *f, const void *p)
{
  switch(f->header.type)
  {
    case DT_INTROSPECTION_TYPE_FLOAT:
      json_builder_add_double_value(b, *(const float *)p);
      break;
    case DT_INTROSPECTION_TYPE_DOUBLE:
      json_builder_add_double_value(b, *(const double *)p);
      break;
    case DT_INTROSPECTION_TYPE_INT:
      json_builder_add_int_value(b, *(const int *)p);
      break;
    case DT_INTROSPECTION_TYPE_UINT:
      json_builder_add_int_value(b, *(const unsigned int *)p);
      break;
    case DT_INTROSPECTION_TYPE_INT8:
      json_builder_add_int_value(b, *(const int8_t *)p);
      break;
    case DT_INTROSPECTION_TYPE_UINT8:
      json_builder_add_int_value(b, *(const uint8_t *)p);
      break;
    case DT_INTROSPECTION_TYPE_SHORT:
      json_builder_add_int_value(b, *(const short *)p);
      break;
    case DT_INTROSPECTION_TYPE_USHORT:
      json_builder_add_int_value(b, *(const unsigned short *)p);
      break;
    case DT_INTROSPECTION_TYPE_BOOL:
      json_builder_add_boolean_value(b, (*(const gboolean *)p) != 0);
      break;
    case DT_INTROSPECTION_TYPE_ENUM:
    {
      const int v = *(const int *)p;
      const char *name = NULL;
      for(dt_introspection_type_enum_tuple_t *e = f->Enum.values; e && e->name; e++)
        if(e->value == v) { name = e->name; break; }
      if(name) json_builder_add_string_value(b, name);
      else     json_builder_add_int_value(b, v);
      break;
    }
    default:
      json_builder_add_null_value(b);
      break;
  }
}

// write default value of a scalar field into blob at p
static void _write_default(dt_introspection_field_t *f, void *p)
{
  switch(f->header.type)
  {
    case DT_INTROSPECTION_TYPE_FLOAT:  *(float *)p = f->Float.Default; break;
    case DT_INTROSPECTION_TYPE_DOUBLE: *(double *)p = f->Double.Default; break;
    case DT_INTROSPECTION_TYPE_INT:    *(int *)p = f->Int.Default; break;
    case DT_INTROSPECTION_TYPE_UINT:   *(unsigned int *)p = f->UInt.Default; break;
    case DT_INTROSPECTION_TYPE_INT8:   *(int8_t *)p = f->Int8.Default; break;
    case DT_INTROSPECTION_TYPE_UINT8:  *(uint8_t *)p = f->UInt8.Default; break;
    case DT_INTROSPECTION_TYPE_SHORT:  *(short *)p = f->Short.Default; break;
    case DT_INTROSPECTION_TYPE_USHORT: *(unsigned short *)p = f->UShort.Default; break;
    case DT_INTROSPECTION_TYPE_BOOL:   *(gboolean *)p = f->Bool.Default; break;
    case DT_INTROSPECTION_TYPE_ENUM:   *(int *)p = f->Enum.Default; break;
    default: break;
  }
}

static void _write_num(dt_introspection_field_t *f, void *p, double num)
{
  switch(f->header.type)
  {
    case DT_INTROSPECTION_TYPE_FLOAT:  *(float *)p = (float)num; break;
    case DT_INTROSPECTION_TYPE_DOUBLE: *(double *)p = num; break;
    case DT_INTROSPECTION_TYPE_INT:    *(int *)p = (int)num; break;
    case DT_INTROSPECTION_TYPE_UINT:   *(unsigned int *)p = (unsigned int)num; break;
    case DT_INTROSPECTION_TYPE_INT8:   *(int8_t *)p = (int8_t)num; break;
    case DT_INTROSPECTION_TYPE_UINT8:  *(uint8_t *)p = (uint8_t)num; break;
    case DT_INTROSPECTION_TYPE_SHORT:  *(short *)p = (short)num; break;
    case DT_INTROSPECTION_TYPE_USHORT: *(unsigned short *)p = (unsigned short)num; break;
    case DT_INTROSPECTION_TYPE_BOOL:   *(gboolean *)p = (num != 0.0); break;
    case DT_INTROSPECTION_TYPE_ENUM:   *(int *)p = (int)num; break;
    default: break;
  }
}

static char *_builder_to_string(JsonBuilder *b)
{
  JsonNode *root = json_builder_get_root(b);
  JsonGenerator *gen = json_generator_new();
  json_generator_set_root(gen, root);
  gchar *out = json_generator_to_data(gen, NULL);
  g_object_unref(gen);
  json_node_unref(root);
  return out;
}

// ---------------------------------------------------------------------------
// public API
// ---------------------------------------------------------------------------

char *dt_bridge_list_modules_json(void)
{
  JsonBuilder *b = json_builder_new();
  json_builder_begin_array(b);
  for(GList *m = darktable.iop; m; m = g_list_next(m))
  {
    dt_iop_module_so_t *so = (dt_iop_module_so_t *)m->data;
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "operation");
    json_builder_add_string_value(b, so->op);
    json_builder_set_member_name(b, "version");
    json_builder_add_int_value(b, so->version ? so->version() : -1);
    json_builder_set_member_name(b, "have_introspection");
    json_builder_add_boolean_value(b, so->have_introspection);
    _add_doc_url(b, so->op);
    json_builder_end_object(b);
  }
  json_builder_end_array(b);
  char *out = _builder_to_string(b);
  g_object_unref(b);
  return out;
}

char *dt_bridge_module_schema_json(const char *op, char **err)
{
  dt_iop_module_so_t *so = _find_so(op);
  if(!so)
  {
    _seterr(err, "unknown module operation '%s'", op ? op : "(null)");
    return NULL;
  }
  if(!so->have_introspection || !so->get_introspection || !so->get_introspection_linear)
  {
    _seterr(err, "module '%s' has no introspection", op);
    return NULL;
  }

  dt_introspection_t *intro = so->get_introspection();
  dt_introspection_field_t *lin = so->get_introspection_linear();

  JsonBuilder *b = json_builder_new();
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "operation");
  json_builder_add_string_value(b, so->op);
  json_builder_set_member_name(b, "params_version");
  json_builder_add_int_value(b, intro->params_version);
  json_builder_set_member_name(b, "params_size");
  json_builder_add_int_value(b, (gint64)intro->size);
  _add_doc_url(b, so->op);
  json_builder_set_member_name(b, "fields");
  json_builder_begin_array(b);

  for(dt_introspection_field_t *f = lin;
      f && f->header.type != DT_INTROSPECTION_TYPE_NONE; f++)
  {
    if(!_is_scalar(f->header.type)) continue;  // skip root struct / arrays / unions
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "name");
    json_builder_add_string_value(b, f->header.field_name);
    json_builder_set_member_name(b, "type");
    json_builder_add_string_value(b, _type_name(f->header.type));
    json_builder_set_member_name(b, "offset");
    json_builder_add_int_value(b, (gint64)f->header.offset);
    switch(f->header.type)
    {
      case DT_INTROSPECTION_TYPE_FLOAT:
        json_builder_set_member_name(b, "min");
        json_builder_add_double_value(b, f->Float.Min);
        json_builder_set_member_name(b, "max");
        json_builder_add_double_value(b, f->Float.Max);
        json_builder_set_member_name(b, "default");
        json_builder_add_double_value(b, f->Float.Default);
        break;
      case DT_INTROSPECTION_TYPE_INT:
        json_builder_set_member_name(b, "min");
        json_builder_add_int_value(b, f->Int.Min);
        json_builder_set_member_name(b, "max");
        json_builder_add_int_value(b, f->Int.Max);
        json_builder_set_member_name(b, "default");
        json_builder_add_int_value(b, f->Int.Default);
        break;
      case DT_INTROSPECTION_TYPE_UINT:
        json_builder_set_member_name(b, "min");
        json_builder_add_int_value(b, f->UInt.Min);
        json_builder_set_member_name(b, "max");
        json_builder_add_int_value(b, f->UInt.Max);
        json_builder_set_member_name(b, "default");
        json_builder_add_int_value(b, f->UInt.Default);
        break;
      case DT_INTROSPECTION_TYPE_BOOL:
        json_builder_set_member_name(b, "default");
        json_builder_add_boolean_value(b, f->Bool.Default != 0);
        break;
      case DT_INTROSPECTION_TYPE_ENUM:
        json_builder_set_member_name(b, "default");
        json_builder_add_int_value(b, f->Enum.Default);
        json_builder_set_member_name(b, "values");
        json_builder_begin_array(b);
        for(dt_introspection_type_enum_tuple_t *e = f->Enum.values; e && e->name; e++)
        {
          json_builder_begin_object(b);
          json_builder_set_member_name(b, "name");
          json_builder_add_string_value(b, e->name);
          json_builder_set_member_name(b, "value");
          json_builder_add_int_value(b, e->value);
          json_builder_end_object(b);
        }
        json_builder_end_array(b);
        break;
      default: break;
    }
    json_builder_end_object(b);
  }

  json_builder_end_array(b);
  json_builder_end_object(b);
  char *out = _builder_to_string(b);
  g_object_unref(b);
  return out;
}

// write a { field: value, ... } object for all scalar fields of `blob`
static void _write_fields_object(dt_iop_module_so_t *so, const void *blob,
                                 JsonBuilder *b)
{
  dt_introspection_field_t *lin = so->get_introspection_linear();
  json_builder_begin_object(b);
  for(dt_introspection_field_t *f = lin;
      f && f->header.type != DT_INTROSPECTION_TYPE_NONE; f++)
  {
    if(!_is_scalar(f->header.type)) continue;
    void *p = so->get_p((void *)blob, f->header.name);
    if(!p) continue;
    json_builder_set_member_name(b, f->header.field_name);
    _add_value(b, f, p);
  }
  json_builder_end_object(b);
}

char *dt_bridge_decode_params_json(const char *op, const char *blob_hex, char **err)
{
  dt_iop_module_so_t *so = _find_so(op);
  if(!so)
  {
    _seterr(err, "unknown module operation '%s'", op ? op : "(null)");
    return NULL;
  }
  if(!so->have_introspection || !so->get_introspection || !so->get_p)
  {
    _seterr(err, "module '%s' has no introspection", op);
    return NULL;
  }

  size_t blen = 0;
  uint8_t *blob = _hex_to_bytes(blob_hex, &blen);
  if(!blob) { _seterr(err, "invalid hex blob"); return NULL; }

  dt_introspection_t *intro = so->get_introspection();
  if(blen != intro->size)
  {
    _seterr(err, "blob size %zu != module '%s' params size %zu"
                 " (version mismatch? pass the current version)",
            blen, op, intro->size);
    g_free(blob);
    return NULL;
  }

  JsonBuilder *b = json_builder_new();
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "operation");
  json_builder_add_string_value(b, so->op);
  json_builder_set_member_name(b, "version");
  json_builder_add_int_value(b, intro->params_version);
  json_builder_set_member_name(b, "fields");
  _write_fields_object(so, blob, b);
  json_builder_end_object(b);

  char *out = _builder_to_string(b);
  g_object_unref(b);
  g_free(blob);
  return out;
}

// allocate a params blob for `so`, seed it with each scalar field's default,
// then apply the supplied JSON `fields` (may be NULL); returns a g_malloc'd blob
// of *size bytes, or NULL on error with *err set
static uint8_t *_seed_and_apply(dt_iop_module_so_t *so, JsonObject *fields,
                                size_t *size, char **err)
{
  dt_introspection_t *intro = so->get_introspection();
  dt_introspection_field_t *lin = so->get_introspection_linear();

  // array/struct params (e.g. curve nodes) can't be rebuilt from scalar leaves
  // alone, so require a full blob_hex; the root struct (size == whole) is skipped
  for(dt_introspection_field_t *f = lin;
      f && f->header.type != DT_INTROSPECTION_TYPE_NONE; f++)
    if(!_is_scalar(f->header.type) && f->header.size != intro->size)
    {
      _seterr(err, "module '%s' has non-scalar parameters; pass a full blob_hex"
                   " instead of fields", so->op);
      return NULL;
    }

  uint8_t *blob = g_malloc0(intro->size);
  for(dt_introspection_field_t *f = lin;
      f && f->header.type != DT_INTROSPECTION_TYPE_NONE; f++)
  {
    if(!_is_scalar(f->header.type)) continue;
    void *p = so->get_p(blob, f->header.name);
    if(p) _write_default(f, p);
  }

  if(fields)
  {
    GList *members = json_object_get_members(fields);
    for(GList *it = members; it; it = g_list_next(it))
    {
      const char *name = (const char *)it->data;
      dt_introspection_field_t *f = so->get_f(name);
      if(!f || !_is_scalar(f->header.type))
      {
        _seterr(err, "unknown or non-scalar field '%s' for module '%s'", name, so->op);
        g_list_free(members);
        g_free(blob);
        return NULL;
      }
      void *p = so->get_p(blob, f->header.name);
      if(!p) continue;
      JsonNode *node = json_object_get_member(fields, name);
      if(f->header.type == DT_INTROSPECTION_TYPE_ENUM
         && json_node_get_value_type(node) == G_TYPE_STRING)
      {
        const char *sym = json_node_get_string(node);
        int val = 0;
        gboolean found = FALSE;
        for(dt_introspection_type_enum_tuple_t *e = f->Enum.values; e && e->name; e++)
          if(!g_strcmp0(e->name, sym)) { val = e->value; found = TRUE; break; }
        if(!found)
        {
          _seterr(err, "unknown enum value '%s' for field '%s'", sym, name);
          g_list_free(members);
          g_free(blob);
          return NULL;
        }
        *(int *)p = val;
      }
      else
      {
        _write_num(f, p, json_node_get_double(node));
      }
    }
    g_list_free(members);
  }

  *size = intro->size;
  return blob;
}

char *dt_bridge_encode_params_hex(const char *op, void *fields_jsonobject, char **err)
{
  dt_iop_module_so_t *so = _find_so(op);
  if(!so)
  {
    _seterr(err, "unknown module operation '%s'", op ? op : "(null)");
    return NULL;
  }
  if(!so->have_introspection || !so->get_introspection || !so->get_p || !so->get_f)
  {
    _seterr(err, "module '%s' has no introspection", op);
    return NULL;
  }

  size_t size = 0;
  uint8_t *blob = _seed_and_apply(so, (JsonObject *)fields_jsonobject, &size, err);
  if(!blob) return NULL;
  char *hex = _bytes_to_hex(blob, size);
  g_free(blob);
  return hex;
}

// ---------------------------------------------------------------------------
// import + render
// ---------------------------------------------------------------------------

static GHashTable *_path_cache = NULL; // path -> imgid (GINT_TO_POINTER)

static dt_imgid_t _import(const char *path, char **err)
{
  if(!path) { _seterr(err, "no input path or imgid provided"); return NO_IMGID; }
  if(!_path_cache)
    _path_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  gpointer v = NULL;
  if(g_hash_table_lookup_extended(_path_cache, path, NULL, &v))
    return (dt_imgid_t)GPOINTER_TO_INT(v);
  gchar *dir = g_path_get_dirname(path);
  dt_film_t film;
  const dt_filmid_t filmid = dt_film_new(&film, dir);
  g_free(dir);
  const dt_imgid_t id = dt_image_import(filmid, path, TRUE, FALSE);
  if(dt_is_valid_imgid(id))
    g_hash_table_insert(_path_cache, g_strdup(path), GINT_TO_POINTER(id));
  else
    _seterr(err, "could not import '%s'", path);
  return id;
}

// apply one stack entry to a module instance in dev and snapshot it to history
static gboolean _apply_entry(dt_develop_t *dev, JsonObject *entry, char **err)
{
  const char *op = json_object_has_member(entry, "operation")
                     ? json_object_get_string_member(entry, "operation") : NULL;
  if(!op) { _seterr(err, "stack entry missing 'operation'"); return FALSE; }
  const int multi_priority = json_object_has_member(entry, "multi_priority")
                     ? (int)json_object_get_int_member(entry, "multi_priority") : 0;
  const gboolean enabled = json_object_has_member(entry, "enabled")
                     ? json_object_get_boolean_member(entry, "enabled") : TRUE;

  dt_iop_module_t *mod = dt_iop_get_module_by_op_priority(dev->iop, op, multi_priority);
  if(!mod)
  {
    _seterr(err, "module '%s' (priority %d) not found in pipe", op, multi_priority);
    return FALSE;
  }

  size_t size = 0;
  uint8_t *blob = NULL;
  if(json_object_has_member(entry, "blob_hex"))
  {
    blob = _hex_to_bytes(json_object_get_string_member(entry, "blob_hex"), &size);
    if(!blob) { _seterr(err, "invalid blob_hex for '%s'", op); return FALSE; }
  }
  else
  {
    JsonObject *fields = NULL;
    if(json_object_has_member(entry, "params"))
    {
      JsonNode *fn = json_object_get_member(entry, "params");
      if(JSON_NODE_HOLDS_OBJECT(fn)) fields = json_node_get_object(fn);
    }
    if(!mod->so->have_introspection)
    {
      _seterr(err, "module '%s' has no introspection for 'params'", op);
      return FALSE;
    }
    blob = _seed_and_apply(mod->so, fields, &size, err);
    if(!blob) return FALSE;
  }

  if(size != (size_t)mod->params_size)
  {
    _seterr(err, "blob size %zu != module '%s' params size %d",
            size, op, mod->params_size);
    g_free(blob);
    return FALSE;
  }
  memcpy(mod->params, blob, size);
  g_free(blob);
  mod->enabled = enabled;
  dt_dev_add_history_item_ext(dev, mod, enabled, TRUE);
  return TRUE;
}

// in-memory 8-bit RGB export "format" module; we render through this instead of
// dt_imageio_preview() because that helper builds its surface via a GUI-only
// cairo wrapper (dereferences darktable.gui->ppd) and crashes headless
typedef struct
{
  dt_imageio_module_data_t head;
  int bpp;
  uint8_t *buf;
  uint32_t width, height;
} _mcp_fmt_t;

static int _mcp_write(dt_imageio_module_data_t *data, const char *filename,
                      const void *in,
                      const dt_colorspaces_color_profile_type_t over_type,
                      const char *over_filename, void *exif, const int exif_len,
                      const dt_imgid_t imgid, const int num, const int total,
                      dt_dev_pixelpipe_t *pipe, const gboolean export_masks)
{
  _mcp_fmt_t *d = (_mcp_fmt_t *)data;
  if(!in) return 1;
  memcpy(d->buf, in, sizeof(uint32_t) * (size_t)data->width * data->height);
  d->width = data->width;
  d->height = data->height;
  return 0;
}
static int _mcp_bpp(dt_imageio_module_data_t *d)
{
  return 8;
}
static int _mcp_levels(dt_imageio_module_data_t *d)
{
  return IMAGEIO_RGB | IMAGEIO_INT8;
}
static const char *_mcp_mime(dt_imageio_module_data_t *d)
{
  return "memory";
}
static void _free_cb(void *p) { g_free(p); }

// render the committed history of `imgid` to a plain cairo RGB24 surface that
// owns its pixel buffer (freed when the surface is destroyed)
static cairo_surface_t *_render_to_surface(dt_imgid_t imgid, int w, int h,
                                           int history_end, char **err)
{
  if(w <= 0) w = 1024;
  if(h <= 0) h = 1024;

  dt_imageio_module_format_t fmt;
  memset(&fmt, 0, sizeof(fmt));
  fmt.mime = _mcp_mime;
  fmt.levels = _mcp_levels;
  fmt.bpp = _mcp_bpp;
  fmt.write_image = _mcp_write;

  _mcp_fmt_t dat;
  memset(&dat, 0, sizeof(dat));
  dat.head.max_width = w;
  dat.head.max_height = h;
  dat.head.width = w;
  dat.head.height = h;
  dat.head.style_append = TRUE;
  dat.bpp = 8;
  dat.buf = g_malloc0(sizeof(uint32_t) * (size_t)w * h);

  // NB: dt_imageio_export_with_flags returns FALSE on success
  const gboolean failed = dt_imageio_export_with_flags(
      imgid, "memory", &fmt, (dt_imageio_module_data_t *)&dat,
      TRUE, TRUE, FALSE /*hq*/, TRUE /*upscale*/, FALSE /*is_scaling*/, 1.0,
      FALSE /*thumbnail*/, NULL /*filter*/, FALSE /*copy_meta*/, FALSE /*export_masks*/,
      DT_COLORSPACE_DISPLAY, NULL, DT_INTENT_LAST, NULL, NULL,
      1, 1, NULL /*metadata*/, history_end);

  if(failed || dat.width == 0 || dat.height == 0)
  {
    g_free(dat.buf);
    _seterr(err, "render failed (invalid image or pipeline)");
    return NULL;
  }

  const int stride = cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, dat.width);
  cairo_surface_t *surf =
    cairo_image_surface_create_for_data(dat.buf, CAIRO_FORMAT_RGB24, dat.width,
                                        dat.height, stride);
  if(cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS)
  {
    cairo_surface_destroy(surf);
    g_free(dat.buf);
    _seterr(err, "cairo surface creation failed");
    return NULL;
  }
  static const cairo_user_data_key_t key;
  cairo_surface_set_user_data(surf, &key, dat.buf, _free_cb);
  return surf;
}

// import/resolve, apply the optional edit stack on a throwaway duplicate, and
// render to a cairo surface; sets *dup_out to the duplicate imgid (or NO_IMGID)
// which the caller must dt_image_remove() after using the surface
static cairo_surface_t *_render_surface(const char *path, int imgid_in, int width,
                                        int height, JsonArray *stack,
                                        gboolean disable_tone_mappers, int history_end,
                                        dt_imgid_t *dup_out, char **err)
{
  *dup_out = NO_IMGID;
  const dt_imgid_t base = (imgid_in > 0) ? (dt_imgid_t)imgid_in : _import(path, err);
  if(!dt_is_valid_imgid(base)) return NULL;

  const guint n_stack = stack ? json_array_get_length(stack) : 0;
  const gboolean has_edits = disable_tone_mappers || n_stack > 0;
  dt_imgid_t work = base;

  if(has_edits)
  {
    const dt_imgid_t dup = dt_image_duplicate(base);
    if(!dt_is_valid_imgid(dup))
    {
      _seterr(err, "could not duplicate image for editing");
      return NULL;
    }
    *dup_out = dup;
    work = dup;

    dt_develop_t dev;
    dt_dev_init(&dev, FALSE);
    dt_dev_load_image(&dev, work);

    if(disable_tone_mappers)
    {
      const char *tms[] = { "sigmoid", "filmicrgb", "basecurve", NULL };
      for(int i = 0; tms[i]; i++)
      {
        dt_iop_module_t *m = dt_iop_get_module_by_op_priority(dev.iop, tms[i], 0);
        if(m && m->enabled)
        {
          m->enabled = FALSE;
          dt_dev_add_history_item_ext(&dev, m, FALSE, TRUE);
        }
      }
    }

    for(guint i = 0; i < n_stack; i++)
    {
      JsonNode *en = json_array_get_element(stack, i);
      if(!JSON_NODE_HOLDS_OBJECT(en)
         || !_apply_entry(&dev, json_node_get_object(en), err))
      {
        if(JSON_NODE_HOLDS_OBJECT(en) == FALSE)
          _seterr(err, "stack[%u] is not an object", i);
        dt_dev_cleanup(&dev);
        return NULL;
      }
    }

    dt_dev_write_history_ext(&dev, work);
    dt_dev_cleanup(&dev);
  }

  return _render_to_surface(work, width, height, history_end, err);
}

static cairo_status_t _png_writer(void *closure, const unsigned char *data,
                                  unsigned int length)
{
  g_byte_array_append((GByteArray *)closure, data, length);
  return CAIRO_STATUS_SUCCESS;
}

gboolean dt_bridge_render_png(const char *path, int imgid_in, int width, int height,
                              void *stack_jsonarray, gboolean disable_tone_mappers,
                              int history_end, uint8_t **png_out, size_t *png_len,
                              char **err)
{
  dt_imgid_t dup = NO_IMGID;
  cairo_surface_t *surf = _render_surface(path, imgid_in, width, height,
                                          (JsonArray *)stack_jsonarray,
                                          disable_tone_mappers,
                                          history_end, &dup, err);
  gboolean ok = FALSE;
  if(surf)
  {
    GByteArray *buf = g_byte_array_new();
    if(cairo_surface_write_to_png_stream(surf, _png_writer, buf) == CAIRO_STATUS_SUCCESS)
    {
      *png_len = buf->len;
      *png_out = g_byte_array_free(buf, FALSE); // hand raw bytes to caller
      ok = TRUE;
    }
    else { g_byte_array_free(buf, TRUE); _seterr(err, "PNG encoding failed"); }
    cairo_surface_destroy(surf);
  }
  if(dt_is_valid_imgid(dup)) dt_image_remove(dup);
  return ok;
}

char *dt_bridge_image_stats_json(const char *path, int imgid_in, int width, int height,
                                 void *stack_jsonarray, gboolean disable_tone_mappers,
                                 int history_end, char **err)
{
  dt_imgid_t dup = NO_IMGID;
  cairo_surface_t *surf = _render_surface(path, imgid_in, width > 0 ? width : 512,
                                          height > 0 ? height : 512,
                                          (JsonArray *)stack_jsonarray,
                                          disable_tone_mappers, history_end, &dup, err);
  if(!surf) return NULL;

  cairo_surface_flush(surf);
  const int w = cairo_image_surface_get_width(surf);
  const int h = cairo_image_surface_get_height(surf);
  const int stride = cairo_image_surface_get_stride(surf);
  const unsigned char *data = cairo_image_surface_get_data(surf);

  // per-channel 256-bin histograms (surface is 32bpp, native BGRA/RGB24)
  guint64 hist[3][256];
  memset(hist, 0, sizeof(hist));
  guint64 total = 0;
  for(int y = 0; y < h; y++)
  {
    const uint32_t *row = (const uint32_t *)(data + (size_t)y * stride);
    for(int x = 0; x < w; x++)
    {
      const uint32_t px = row[x];
      const int r = (px >> 16) & 0xff, g = (px >> 8) & 0xff, b = px & 0xff;
      hist[0][r]++; hist[1][g]++; hist[2][b]++;
      total++;
    }
  }

  static const char *chan[3] = { "r", "g", "b" };
  JsonBuilder *jb = json_builder_new();
  json_builder_begin_object(jb);
  json_builder_set_member_name(jb, "width");
  json_builder_add_int_value(jb, w);
  json_builder_set_member_name(jb, "height");
  json_builder_add_int_value(jb, h);
  json_builder_set_member_name(jb, "channels");
  json_builder_begin_object(jb);
  for(int c = 0; c < 3; c++)
  {
    int minv = 255, maxv = 0;
    guint64 sum = 0;
    int p1 = 0, p50 = 0, p99 = 0;
    const guint64 t1 = total / 100, t50 = total / 2, t99 = (total * 99) / 100;
    guint64 acc = 0;
    gboolean g1 = FALSE, g50 = FALSE, g99 = FALSE;
    for(int v = 0; v < 256; v++)
    {
      const guint64 n = hist[c][v];
      if(n)
      {
        if(v < minv) minv = v;
        if(v > maxv) maxv = v;
        sum += (guint64)v * n;
      }
      acc += n;
      if(!g1 && acc >= t1)   { p1 = v;  g1 = TRUE; }
      if(!g50 && acc >= t50) { p50 = v; g50 = TRUE; }
      if(!g99 && acc >= t99) { p99 = v; g99 = TRUE; }
    }
    json_builder_set_member_name(jb, chan[c]);
    json_builder_begin_object(jb);
    json_builder_set_member_name(jb, "min");
    json_builder_add_int_value(jb, total ? minv : 0);
    json_builder_set_member_name(jb, "max");
    json_builder_add_int_value(jb, maxv);
    json_builder_set_member_name(jb, "mean");
    json_builder_add_double_value(jb, total ? (double)sum / total : 0.0);
    json_builder_set_member_name(jb, "p1");
    json_builder_add_int_value(jb, p1);
    json_builder_set_member_name(jb, "p50");
    json_builder_add_int_value(jb, p50);
    json_builder_set_member_name(jb, "p99");
    json_builder_add_int_value(jb, p99);
    json_builder_set_member_name(jb, "clip_lo");
    json_builder_add_int_value(jb, (gint64)hist[c][0]);
    json_builder_set_member_name(jb, "clip_hi");
    json_builder_add_int_value(jb, (gint64)hist[c][255]);
    json_builder_end_object(jb);
  }
  json_builder_end_object(jb);
  json_builder_end_object(jb);

  char *out = _builder_to_string(jb);
  g_object_unref(jb);
  cairo_surface_destroy(surf);
  if(dt_is_valid_imgid(dup)) dt_image_remove(dup);
  return out;
}

// ---------------------------------------------------------------------------
// library (catalog) tools
// ---------------------------------------------------------------------------

char *dt_bridge_list_images_json(int limit)
{
  sqlite3 *db = dt_database_get(darktable.db);
  JsonBuilder *b = json_builder_new();
  json_builder_begin_array(b);
  sqlite3_stmt *st = NULL;
  const char *q = "SELECT i.id, f.folder, i.filename"
                  " FROM main.images i JOIN main.film_rolls f ON i.film_id = f.id"
                  " ORDER BY i.id";
  if(db && sqlite3_prepare_v2(db, q, -1, &st, NULL) == SQLITE_OK)
  {
    int n = 0;
    while(sqlite3_step(st) == SQLITE_ROW && (limit <= 0 || n < limit))
    {
      const int id = sqlite3_column_int(st, 0);
      const char *folder = (const char *)sqlite3_column_text(st, 1);
      const char *fn = (const char *)sqlite3_column_text(st, 2);
      gchar *path = g_build_filename(folder ? folder : "", fn ? fn : "", NULL);
      json_builder_begin_object(b);
      json_builder_set_member_name(b, "imgid");
      json_builder_add_int_value(b, id);
      json_builder_set_member_name(b, "path");
      json_builder_add_string_value(b, path);
      json_builder_end_object(b);
      g_free(path);
      n++;
    }
    sqlite3_finalize(st);
  }
  json_builder_end_array(b);
  char *out = _builder_to_string(b);
  g_object_unref(b);
  return out;
}

char *dt_bridge_get_history_json(int imgid, char **err)
{
  sqlite3 *db = dt_database_get(darktable.db);
  sqlite3_stmt *st = NULL;
  const char *q = "SELECT num, operation, op_params, module, enabled, multi_priority, multi_name"
                  " FROM main.history WHERE imgid = ?1 ORDER BY num";
  if(!db || sqlite3_prepare_v2(db, q, -1, &st, NULL) != SQLITE_OK)
  {
    _seterr(err, "could not query history");
    return NULL;
  }
  sqlite3_bind_int(st, 1, imgid);

  JsonBuilder *b = json_builder_new();
  json_builder_begin_array(b);
  while(sqlite3_step(st) == SQLITE_ROW)
  {
    const int num = sqlite3_column_int(st, 0);
    const char *op = (const char *)sqlite3_column_text(st, 1);
    const void *blob = sqlite3_column_blob(st, 2);
    const int blen = sqlite3_column_bytes(st, 2);
    const int version = sqlite3_column_int(st, 3);
    const int enabled = sqlite3_column_int(st, 4);
    const int mp = sqlite3_column_int(st, 5);
    const char *mname = (const char *)sqlite3_column_text(st, 6);

    json_builder_begin_object(b);
    json_builder_set_member_name(b, "num");
    json_builder_add_int_value(b, num);
    json_builder_set_member_name(b, "operation");
    json_builder_add_string_value(b, op ? op : "");
    json_builder_set_member_name(b, "version");
    json_builder_add_int_value(b, version);
    json_builder_set_member_name(b, "enabled");
    json_builder_add_boolean_value(b, enabled != 0);
    json_builder_set_member_name(b, "multi_priority");
    json_builder_add_int_value(b, mp);
    if(mname && *mname)
    {
      json_builder_set_member_name(b, "multi_name");
      json_builder_add_string_value(b, mname);
    }

    // decode fields when the stored blob matches the module's current layout
    dt_iop_module_so_t *so = op ? dt_iop_get_module_so(op) : NULL;
    if(so && so->have_introspection && so->get_introspection && so->get_p)
    {
      dt_introspection_t *intro = so->get_introspection();
      if(blob && (size_t)blen == intro->size && version == intro->params_version)
      {
        json_builder_set_member_name(b, "fields");
        _write_fields_object(so, blob, b);
      }
    }
    json_builder_end_object(b);
  }
  sqlite3_finalize(st);
  json_builder_end_array(b);
  char *out = _builder_to_string(b);
  g_object_unref(b);
  return out;
}

char *dt_bridge_list_styles_json(void)
{
  JsonBuilder *b = json_builder_new();
  json_builder_begin_array(b);
  GList *styles = dt_styles_get_list("");
  for(GList *s = styles; s; s = g_list_next(s))
  {
    dt_style_t *st = (dt_style_t *)s->data;
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "name");
    json_builder_add_string_value(b, st->name ? st->name : "");
    json_builder_set_member_name(b, "description");
    json_builder_add_string_value(b, st->description ? st->description : "");
    json_builder_end_object(b);
  }
  g_list_free_full(styles, dt_style_free);
  json_builder_end_array(b);
  char *out = _builder_to_string(b);
  g_object_unref(b);
  return out;
}

gboolean dt_bridge_apply_style(const char *name, int imgid, gboolean overwrite,
                               char **err)
{
  if(!name || !dt_is_valid_imgid((dt_imgid_t)imgid))
  {
    _seterr(err, "apply_style: need 'name' and valid 'imgid'");
    return FALSE;
  }
  dt_styles_apply_to_image(name, FALSE, overwrite, (dt_imgid_t)imgid);
  return TRUE;
}

gboolean dt_bridge_save_style(const char *name, const char *description,
                              int imgid, char **err)
{
  if(!name || !dt_is_valid_imgid((dt_imgid_t)imgid))
  {
    _seterr(err, "save_style: need 'name' and valid 'imgid'");
    return FALSE;
  }
  if(!dt_styles_create_from_image(name, description ? description : "",
                                  (dt_imgid_t)imgid, NULL, TRUE))
  {
    _seterr(err, "could not create style '%s' (already exists?)", name);
    return FALSE;
  }
  return TRUE;
}

gboolean dt_bridge_import_style(const char *path, char **err)
{
  if(!path) { _seterr(err, "import_style: need 'path'"); return FALSE; }
  dt_styles_import_from_file(path);
  return TRUE;
}

gboolean dt_bridge_export_png(const char *in_path, int imgid_in, int width, int height,
                              void *stack_jsonarray, gboolean disable_tone_mappers,
                              int history_end, const char *out_path, char **err)
{
  if(!out_path) { _seterr(err, "export: need 'out_path'"); return FALSE; }
  dt_imgid_t dup = NO_IMGID;
  cairo_surface_t *surf = _render_surface(in_path, imgid_in, width, height,
                                          (JsonArray *)stack_jsonarray,
                                          disable_tone_mappers,
                                          history_end, &dup, err);
  gboolean ok = FALSE;
  if(surf)
  {
    ok = (cairo_surface_write_to_png(surf, out_path) == CAIRO_STATUS_SUCCESS);
    if(!ok) _seterr(err, "could not write PNG to '%s'", out_path);
    cairo_surface_destroy(surf);
  }
  if(dt_is_valid_imgid(dup)) dt_image_remove(dup);
  return ok;
}
