/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DT_MOBILE_TEXT_CONTROLS_H
#define DT_MOBILE_TEXT_CONTROLS_H

#include "dt_mobile_file_controls.h"

/* Only original user-facing fixed strings are writable. Profile filenames,
 * unused UI labels, LUTs and opaque arrays are not generic text parameters. */
static const char *mobile_text_label(const char *operation, const char *parameter)
{
  if(!strcmp(operation, "watermark"))
  {
    if(!strcmp(parameter, "text")) return "watermark text";
    if(!strcmp(parameter, "font")) return "text font";
    if(!strcmp(parameter, "filename")) return "watermark template filename";
  }
  if(!strcmp(operation, "lens"))
  {
    if(!strcmp(parameter, "camera")) return "camera model";
    if(!strcmp(parameter, "lens")) return "lens model";
  }
  return NULL;
}

static const dt_introspection_field_t *mobile_text_field(dt_iop_module_t *module, const char *parameter)
{
  if(!module || !module->params || module->params_size <= 0 || !module->get_f
     || !module->have_introspection || !mobile_text_label(module->op, parameter)) return NULL;
  const dt_introspection_field_t *f = module->get_f(parameter);
  if(!f || f->header.type != DT_INTROSPECTION_TYPE_ARRAY
     || f->Array.type != DT_INTROSPECTION_TYPE_CHAR || !f->Array.field
     || f->Array.field->header.type != DT_INTROSPECTION_TYPE_CHAR
     || f->Array.field->header.size != sizeof(char)
     || f->Array.count < 2 || f->Array.count > 4096 || f->header.size != f->Array.count
     || f->header.offset > (size_t)module->params_size
     || f->header.size > (size_t)module->params_size - f->header.offset) return NULL;
  return f;
}

static void append_mobile_text_parameters(dt_iop_module_t *module, GString *json)
{
  static const char *const names[] = { "text", "font", "filename", "camera", "lens" };
  g_string_append(json, ",\"textParameters\":[");
  gboolean first = TRUE;
  for(size_t k = 0; k < sizeof(names) / sizeof(names[0]); k++)
  {
    const dt_introspection_field_t *f = mobile_text_field(module, names[k]);
    if(!f) continue;
    const char *value = (const char *)module->params + f->header.offset;
    const size_t length = strnlen(value, f->header.size);
    if(length == f->header.size || !g_utf8_validate(value, length, NULL)) continue;
    if(!first) g_string_append_c(json, ',');
    first = FALSE;
    g_string_append(json, "{\"name\":"); json_string(json, names[k]);
    g_string_append(json, ",\"label\":"); json_string(json, mobile_text_label(module->op, names[k]));
    g_string_append(json, ",\"value\":"); json_string(json, value);
    g_string_append_printf(json, ",\"maxBytes\":%zu", f->header.size - 1);
    if(module->default_params)
    {
      const char *original = (const char *)module->default_params + f->header.offset;
      const size_t original_length = strnlen(original, f->header.size);
      if(original_length < f->header.size && g_utf8_validate(original, original_length, NULL))
      {
        g_string_append(json, ",\"default\":"); json_string(json, original);
      }
    }
    g_string_append_c(json, '}');
  }
  append_mobile_file_parameter(module, json, &first);
  g_string_append_c(json, ']');
}

/* Returns 0 for unsupported fields, -1 for invalid input, 1 after changing the
 * original parameter struct. The owning engine records the one history step. */
static int update_mobile_text_parameter(dt_iop_module_t *module, const char *parameter, const char *value)
{
  if(!strcmp(parameter, "file.import")) return update_mobile_file_parameter(module, value);
  const dt_introspection_field_t *f = mobile_text_field(module, parameter);
  if(!f) return 0;
  const size_t length = strnlen(value, f->header.size);
  if(length >= f->header.size || !g_utf8_validate(value, length, NULL)) return -1;
  /* The desktop template picker selects a basename in its watermark resource
   * directories; it does not treat this string as a document import path. */
  if(!strcmp(module->op, "watermark") && !strcmp(parameter, "filename")
     && (!length || strchr(value, '/') || strchr(value, '\\') || !strcmp(value, ".") || !strcmp(value, "..")))
    return -1;
  char *destination = (char *)module->params + f->header.offset;
  memset(destination, 0, f->header.size);
  memcpy(destination, value, length);
  return 1;
}

#endif
