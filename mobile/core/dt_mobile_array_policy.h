/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DT_MOBILE_ARRAY_POLICY_H
#define DT_MOBILE_ARRAY_POLICY_H

#include "common/introspection.h"
#include "develop/imageop.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

/* Fixed channel vectors and fixed-band desktop curves are not structural
 * counts. Resolve every dimension and storage type before using one; counts,
 * inactive padding coefficients, opaque LUTs and mask-form arrays stay private.
 * Source review: each named src/iop/<operation>.c params/commit_params/process
 * and its channel/band GUI. Band abscissae preserve strict ordering and fixed
 * endpoints, levels preserve black < middle < white, and normalized channel
 * rows may never acquire a zero divisor. No replacement pixel math lives here. */
typedef struct dt_mobile_array_control
{
  const char *operation, *field, *label;
  unsigned rows, columns, dimensions;
  float minimum, maximum;
  int ordered_x;
} dt_mobile_array_control;

static const dt_mobile_array_control dt_mobile_array_controls[] = {
  { "channelmixerrgb", "red", "red output", 1, 3, 1, -2.f, 2.f, 0 },
  { "channelmixerrgb", "green", "green output", 1, 3, 1, -2.f, 2.f, 0 },
  { "channelmixerrgb", "blue", "blue output", 1, 3, 1, -2.f, 2.f, 0 },
  { "channelmixerrgb", "saturation", "saturation", 1, 3, 1, -2.f, 2.f, 0 },
  { "channelmixerrgb", "lightness", "brightness", 1, 3, 1, -2.f, 2.f, 0 },
  { "channelmixerrgb", "grey", "gray output", 1, 3, 1, -2.f, 2.f, 0 },
  { "channelmixer", "red", "red input", 1, 7, 1, -1.f, 1.f, 0 },
  { "channelmixer", "green", "green input", 1, 7, 1, -1.f, 1.f, 0 },
  { "channelmixer", "blue", "blue input", 1, 7, 1, -1.f, 1.f, 0 },
  { "negadoctor", "Dmin", "film substrate", 1, 3, 1, 0.00001f, 1.5f, 0 },
  { "negadoctor", "wb_high", "highlights white balance", 1, 3, 1, 0.25f, 2.f, 0 },
  { "negadoctor", "wb_low", "shadows white balance", 1, 3, 1, 0.25f, 2.f, 0 },
  { "colorbalance", "lift", "lift", 1, 4, 1, 0.001f, 1.999f, 0 },
  { "colorbalance", "gamma", "gamma", 1, 4, 1, 0.001f, 1.999f, 0 },
  { "colorbalance", "gain", "gain", 1, 4, 1, 0.001f, 1.999f, 0 },
  { "colorchecker", "target_L", "target lightness", 1, 49, 1, -100.f, 200.f, 0 },
  { "colorchecker", "target_a", "target Lab a", 1, 49, 1, -128.f, 128.f, 0 },
  { "colorchecker", "target_b", "target Lab b", 1, 49, 1, -128.f, 128.f, 0 },
  { "levels", "levels", "level", 1, 3, 1, 0.f, 1.f, 0 },
  { "rgblevels", "levels", "levels", 3, 3, 2, 0.f, 1.f, 0 },
  { "borders", "color", "border color", 1, 3, 1, 0.f, 1.f, 0 },
  { "borders", "frame_color", "frame color", 1, 3, 1, 0.f, 1.f, 0 },
  { "watermark", "color", "text color", 1, 3, 1, 0.f, 1.f, 0 },
  { "invert", "color", "film material", 1, 3, 1, 0.00001f, 1.f, 0 },
  { "retouch", "fill_color", "fill color", 1, 3, 1, 0.f, 1.f, 0 },
  { "colorharmonizer", "custom_hue", "custom hue", 1, 4, 1, 0.f, 1.f, 0 },
  { "colorharmonizer", "node_saturation", "node saturation", 1, 4, 1, 0.f, 2.f, 0 },
  { "atrous", "x", "frequency", 5, 6, 2, 0.f, 1.f, 1 },
  { "atrous", "y", "band strength", 5, 6, 2, 0.f, 1.f, 0 },
  { "rawdenoise", "x", "frequency", 4, 5, 2, 0.f, 1.f, 1 },
  { "rawdenoise", "y", "band strength", 4, 5, 2, 0.f, 1.f, 0 },
  { "denoiseprofile", "x", "frequency", 6, 7, 2, 0.f, 1.f, 1 },
  { "denoiseprofile", "y", "band strength", 6, 7, 2, 0.f, 1.f, 0 },
  { "equalizer", "equalizer_x", "frequency", 3, 6, 2, 0.f, 1.f, 1 },
  { "equalizer", "equalizer_y", "band strength", 3, 6, 2, 0.f, 1.f, 0 },
  { "lowlight", "transition_x", "transition position", 1, 6, 1, 0.f, 1.f, 1 },
  { "lowlight", "transition_y", "transition strength", 1, 6, 1, 0.f, 1.f, 0 },
};

/* Parse only the exact names emitted by visit_fields. No pointers, byte
 * offsets, signed indexes, partial names or arbitrary nesting are accepted. */
static inline const dt_mobile_array_control *dt_mobile_array_control_for(
    const char *operation, const char *path, unsigned *row, unsigned *column)
{
  if(!operation || !path) return NULL;
  for(size_t k = 0; k < sizeof(dt_mobile_array_controls) / sizeof(dt_mobile_array_controls[0]); k++)
  {
    const dt_mobile_array_control *control = &dt_mobile_array_controls[k];
    if(strcmp(operation, control->operation)) continue;
    const size_t length = strlen(control->field);
    if(strncmp(path, control->field, length) || path[length] != '[') continue;
    const char *cursor = path + length;
    unsigned indexes[2] = { 0, 0 };
    int valid = 1;
    for(unsigned dim = 0; dim < control->dimensions; dim++)
    {
      if(*cursor++ != '[' || *cursor < '0' || *cursor > '9') { valid = 0; break; }
      if(*cursor == '0' && cursor[1] != ']') { valid = 0; break; }
      while(*cursor >= '0' && *cursor <= '9')
      {
        if(indexes[dim] > 1000) { valid = 0; break; }
        indexes[dim] = indexes[dim] * 10 + (unsigned)(*cursor++ - '0');
      }
      if(!valid || *cursor++ != ']') { valid = 0; break; }
    }
    if(!valid || *cursor) continue;
    *row = control->dimensions == 2 ? indexes[0] : 0;
    *column = indexes[control->dimensions - 1];
    if(*row >= control->rows || *column >= control->columns
       || (control->ordered_x && (*column == 0 || *column == control->columns - 1))) continue;
    return control;
  }
  return NULL;
}

static inline int dt_mobile_array_parameter_allowed(const char *operation, const char *path,
                                                     dt_introspection_type_t type)
{
  unsigned row = 0, column = 0;
  if(operation && path && !strcmp(operation, "rawprepare") && type == DT_INTROSPECTION_TYPE_USHORT)
    return !strcmp(path, "raw_black_level_separate[0]") || !strcmp(path, "raw_black_level_separate[1]")
      || !strcmp(path, "raw_black_level_separate[2]") || !strcmp(path, "raw_black_level_separate[3]");
  return type == DT_INTROSPECTION_TYPE_FLOAT
    && dt_mobile_array_control_for(operation, path, &row, &column) != NULL;
}

static inline void dt_mobile_array_parameter_limits(const char *operation, const char *path,
                                                     long double *minimum, long double *maximum)
{
  unsigned row = 0, column = 0;
  const dt_mobile_array_control *control = dt_mobile_array_control_for(operation, path, &row, &column);
  if(!control) return;
  if(*minimum < control->minimum) *minimum = control->minimum;
  if(*maximum > control->maximum) *maximum = control->maximum;
}

/* Validate a whole fixed float array before looking up a component. The
 * introspection shape, offsets and native span must agree with its dimension. */
static inline const float *dt_mobile_array_values(dt_iop_module_t *module, const char *name,
                                                 unsigned rows, unsigned columns, unsigned dimensions)
{
  if(!module || !module->params || module->params_size <= 0 || !module->get_f) return NULL;
  const dt_introspection_field_t *array = module->get_f(name);
  if(!array || array->header.type != DT_INTROSPECTION_TYPE_ARRAY
     || array->header.offset > (size_t)module->params_size
     || array->header.size > (size_t)module->params_size - array->header.offset) return NULL;
  const dt_introspection_field_t *row = array;
  if(dimensions == 2)
  {
    if(array->Array.count != rows || array->Array.type != DT_INTROSPECTION_TYPE_ARRAY
       || !array->Array.field) return NULL;
    row = array->Array.field;
    if(row->header.type != DT_INTROSPECTION_TYPE_ARRAY || row->header.offset != array->header.offset
       || array->header.size != rows * row->header.size) return NULL;
  }
  /* Some native vectors have a fourth SIMD/alpha slot which is deliberately
   * not an editing target. A one-dimensional array may be wider than the
   * reviewed three color components, but must consist entirely of floats. */
  if(row->Array.count < columns || row->Array.count > (columns < 4 ? 4 : columns)
     || (dimensions == 2 && row->Array.count != columns)
     || row->Array.type != DT_INTROSPECTION_TYPE_FLOAT || !row->Array.field
     || row->Array.field->header.type != DT_INTROSPECTION_TYPE_FLOAT
     || row->Array.field->header.size != sizeof(float)
     || row->Array.field->header.offset != array->header.offset
     || row->header.size != row->Array.count * sizeof(float)) return NULL;
  return (const float *)((const char *)module->params + array->header.offset);
}

/* Only the original active color-checker patches are editing targets. The
 * source patch matrix and num_patches remain private, preventing edits from
 * changing interpolation topology or exposing the unused array padding. */
static inline int dt_mobile_array_parameter_visible(dt_iop_module_t *module, const char *path)
{
  if(strcmp(module->op, "colorchecker")) return 1;
  unsigned row = 0, column = 0;
  const dt_mobile_array_control *control = dt_mobile_array_control_for(module->op, path, &row, &column);
  if(!control) return 1;
  const dt_introspection_field_t *count_field = module->get_f ? module->get_f("num_patches") : NULL;
  if(!count_field || count_field->header.type != DT_INTROSPECTION_TYPE_INT
     || count_field->header.size != sizeof(int)
     || count_field->header.offset > (size_t)module->params_size
     || count_field->header.size > (size_t)module->params_size - count_field->header.offset) return 0;
  int count;
  memcpy(&count, (const char *)module->params + count_field->header.offset, sizeof(count));
  return count >= 1 && count <= 49 && column < (unsigned)count;
}

static inline int dt_mobile_array_parameter_value_valid(dt_iop_module_t *module,
                                                        const char *path, double value)
{
  if(!module || !module->get_f || !module->params || !path || !isfinite(value)) return 0;
  unsigned row = 0, column = 0;
  const dt_mobile_array_control *control = dt_mobile_array_control_for(module->op, path, &row, &column);
  if(!dt_mobile_array_parameter_visible(module, path)) return 0;
  if(control)
  {
    const float *values = dt_mobile_array_values(module, control->field,
                                                control->rows, control->columns, control->dimensions);
    if(!values) return 0;
    if(control->ordered_x)
    {
      const float *points = values + row * control->columns;
      float previous = 0.f;
      for(unsigned index = 0; index < control->columns; index++)
      {
        const float current = index == column ? (float)value : points[index];
        if(!isfinite(current) || current < 0.f || current > 1.f
           || (index && (double)current - previous < (double)1e-6f)
           || (index == 0 && current != 0.f)
           || (index == control->columns - 1 && current != 1.f)) return 0;
        previous = current;
      }
    }
  }
  if(control && (!strcmp(control->field, "y") || !strcmp(control->field, "equalizer_y")
                 || !strcmp(control->field, "transition_y")))
  {
    const char *x_name = !strcmp(control->field, "equalizer_y") ? "equalizer_x"
      : !strcmp(control->field, "transition_y") ? "transition_x" : "x";
    const float *x = dt_mobile_array_values(module, x_name, control->rows, control->columns, control->dimensions);
    if(!x) return 0;
    for(unsigned channel = 0; channel < control->rows; channel++)
      for(unsigned index = 0; index < control->columns; index++)
      {
        const float current = x[channel * control->columns + index];
        if(!isfinite(current) || current < 0.f || current > 1.f
           || (index && (double)current - x[channel * control->columns + index - 1] < (double)1e-6f)
           || (index == 0 && current != 0.f) || (index == control->columns - 1 && current != 1.f)) return 0;
      }
  }
  if(!strcmp(module->op, "rawprepare"))
  {
    const dt_introspection_field_t *black = module->get_f("raw_black_level_separate");
    const dt_introspection_field_t *white = module->get_f("raw_white_point");
    if(!black || black->header.type != DT_INTROSPECTION_TYPE_ARRAY || black->Array.count != 4
       || black->Array.type != DT_INTROSPECTION_TYPE_USHORT || black->header.size != 4 * sizeof(unsigned short)
       || !black->Array.field || black->Array.field->header.type != DT_INTROSPECTION_TYPE_USHORT
       || black->Array.field->header.size != sizeof(unsigned short)
       || black->Array.field->header.offset != black->header.offset
       || !white || white->header.type != DT_INTROSPECTION_TYPE_USHORT || white->header.size != sizeof(unsigned short)
       || black->header.offset > (size_t)module->params_size
       || black->header.size > (size_t)module->params_size - black->header.offset
       || white->header.offset > (size_t)module->params_size
       || white->header.size > (size_t)module->params_size - white->header.offset) return 0;
    const unsigned short *blacks = (const unsigned short *)((const char *)module->params + black->header.offset);
    const double white_value = !strcmp(path, "raw_white_point") ? value
      : *(const unsigned short *)((const char *)module->params + white->header.offset);
    for(unsigned index = 0; index < 4; index++)
    {
      char name[64];
      snprintf(name, sizeof(name), "raw_black_level_separate[%u]", index);
      const double black_value = !strcmp(path, name) ? value : blacks[index];
      if(black_value >= white_value) return 0;
    }
  }
  if(!strcmp(module->op, "levels") || !strcmp(module->op, "rgblevels"))
  {
    const int rgb = !strcmp(module->op, "rgblevels");
    const float *points = dt_mobile_array_values(module, "levels", rgb ? 3 : 1, 3, rgb ? 2 : 1);
    if(!points) return 0;
    for(unsigned channel = 0; channel < (rgb ? 3u : 1u); channel++)
    {
      float candidate[3];
      for(unsigned index = 0; index < 3; index++) candidate[index] = control && row == channel && column == index
        ? (float)value : points[channel * 3 + index];
      if(!isfinite(candidate[0]) || !isfinite(candidate[1]) || !isfinite(candidate[2])
         || candidate[0] < 0.f || candidate[2] > 1.f
         || candidate[1] - candidate[0] < 1e-6f || candidate[2] - candidate[1] < 1e-6f) return 0;
    }
    if(!rgb)
    {
      const char *names[] = { "black", "gray", "white" };
      float percentiles[3];
      for(unsigned index = 0; index < 3; index++)
      {
        const dt_introspection_field_t *field = module->get_f(names[index]);
        if(!field || field->header.type != DT_INTROSPECTION_TYPE_FLOAT || field->header.size != sizeof(float)
           || field->header.offset > (size_t)module->params_size
           || sizeof(float) > (size_t)module->params_size - field->header.offset) return 0;
        percentiles[index] = !strcmp(path, names[index]) ? (float)value
          : *(const float *)((const char *)module->params + field->header.offset);
        if(!isfinite(percentiles[index])) return 0;
      }
      if(percentiles[0] < 0.f || percentiles[2] > 100.f
         || percentiles[0] > percentiles[1] || percentiles[1] > percentiles[2]
         || percentiles[0] == percentiles[2]) return 0;
    }
  }
  if(!strcmp(module->op, "channelmixerrgb"))
  {
    const char *rows[] = { "red", "green", "blue" };
    const char *normalizes[] = { "normalize_R", "normalize_G", "normalize_B" };
    for(unsigned channel = 0; channel < 3; channel++)
    {
      const dt_introspection_field_t *flag = module->get_f(normalizes[channel]);
      const float *coefficients = dt_mobile_array_values(module, rows[channel], 1, 3, 1);
      if(!flag || flag->header.type != DT_INTROSPECTION_TYPE_BOOL || flag->header.size != sizeof(gboolean)
         || flag->header.offset > (size_t)module->params_size
         || sizeof(gboolean) > (size_t)module->params_size - flag->header.offset || !coefficients) return 0;
      const int normalize = !strcmp(path, normalizes[channel]) ? value != 0.
        : *(const gboolean *)((const char *)module->params + flag->header.offset);
      float sum = 0.f;
      for(unsigned index = 0; index < 3; index++) sum += control && !strcmp(control->field, rows[channel]) && column == index
        ? (float)value : coefficients[index];
      if(normalize && (!isfinite(sum) || fabsf(sum) < 1e-6f)) return 0;
    }
  }
  return 1;
}

/* Keep touch ranges inside current linked bounds as well as hard domains.
 * A two-micro-unit gap survives float rounding at normalized endpoints. */
static inline void dt_mobile_array_parameter_dynamic_limits(dt_iop_module_t *module,
    const char *path, long double *minimum, long double *maximum)
{
  if(!strcmp(module->op, "rawprepare") && module->get_f && module->params)
  {
    const dt_introspection_field_t *black = module->get_f("raw_black_level_separate");
    const dt_introspection_field_t *white = module->get_f("raw_white_point");
    if(black && black->header.type == DT_INTROSPECTION_TYPE_ARRAY && black->Array.count == 4
       && black->Array.type == DT_INTROSPECTION_TYPE_USHORT && black->header.size == 4 * sizeof(unsigned short)
       && white && white->header.type == DT_INTROSPECTION_TYPE_USHORT && white->header.size == sizeof(unsigned short)
       && black->header.offset <= (size_t)module->params_size
       && black->header.size <= (size_t)module->params_size - black->header.offset
       && white->header.offset <= (size_t)module->params_size
       && white->header.size <= (size_t)module->params_size - white->header.offset)
    {
      if(!strcmp(path, "raw_white_point"))
      {
        const unsigned short *blacks = (const unsigned short *)((const char *)module->params + black->header.offset);
        for(unsigned index = 0; index < 4; index++) if(*minimum <= blacks[index]) *minimum = blacks[index] + 1;
      }
      else if(!strncmp(path, "raw_black_level_separate[", 25))
      {
        const unsigned short current = *(const unsigned short *)((const char *)module->params + white->header.offset);
        if(*maximum >= current) *maximum = (int)current - 1;
      }
    }
  }
  if(!strcmp(module->op, "retouch") && module->get_f && module->params)
  {
    const char *other_name = !strcmp(path, "num_scales") ? "merge_from_scale"
      : !strcmp(path, "merge_from_scale") ? "num_scales" : NULL;
    const dt_introspection_field_t *field = other_name ? module->get_f(other_name) : NULL;
    if(field && field->header.type == DT_INTROSPECTION_TYPE_INT && field->header.size == sizeof(int)
       && field->header.offset <= (size_t)module->params_size
       && field->header.size <= (size_t)module->params_size - field->header.offset)
    {
      int current;
      memcpy(&current, (const char *)module->params + field->header.offset, sizeof(current));
      if(!strcmp(path, "num_scales") && *minimum < current) *minimum = current;
      if(!strcmp(path, "merge_from_scale") && *maximum > current) *maximum = current;
    }
  }
  unsigned row = 0, column = 0;
  const dt_mobile_array_control *control = dt_mobile_array_control_for(module->op, path, &row, &column);
  if(control && (control->ordered_x || !strcmp(module->op, "levels") || !strcmp(module->op, "rgblevels")))
  {
    const float *values = dt_mobile_array_values(module, control->field,
      control->rows, control->columns, control->dimensions);
    if(!values) return;
    const float *current = values + row * control->columns;
    if(column && isfinite(current[column - 1]))
    {
      const float lower = current[column - 1] + 2e-6f;
      if(*minimum < lower) *minimum = lower;
    }
    if(column + 1 < control->columns && isfinite(current[column + 1]))
    {
      const float upper = current[column + 1] - 2e-6f;
      if(*maximum > upper) *maximum = upper;
    }
  }
}

static inline const char *dt_mobile_array_parameter_label(const char *operation, const char *path,
                                                          const char *fallback)
{
  unsigned row = 0, column = 0;
  const dt_mobile_array_control *control = dt_mobile_array_control_for(operation, path, &row, &column);
  static _Thread_local char label[192];
  if(!control)
  {
    if(!strcmp(operation, "rawprepare") && !strncmp(path, "raw_black_level_separate[", 25))
    {
      snprintf(label, sizeof(label), "sensor black level · channel %c", path[25]);
      return label;
    }
    return fallback;
  }
  const char *rgb[] = { "red", "green", "blue" };
  const char *levels[] = { "black", "middle gray", "white" };
  const char *mixer[] = { "hue", "saturation", "lightness", "red", "green", "blue", "gray" };
  if(!strcmp(operation, "colorchecker")) snprintf(label, sizeof(label), "patch %u · %s", column + 1, control->label);
  else if(!strcmp(operation, "levels")) snprintf(label, sizeof(label), "%s", levels[column]);
  else if(!strcmp(operation, "rgblevels")) snprintf(label, sizeof(label), "%s · %s", rgb[row], levels[column]);
  else if(!strcmp(operation, "channelmixer")) snprintf(label, sizeof(label), "%s · %s output", control->label, mixer[column]);
  else if(!strcmp(operation, "colorbalance")) snprintf(label, sizeof(label), "%s · %s", control->label,
      column == 0 ? "factor" : rgb[column - 1]);
  else if(control->dimensions == 2) snprintf(label, sizeof(label), "%s · channel %u · band %u", control->label, row + 1, column + 1);
  else if(control->columns == 3) snprintf(label, sizeof(label), "%s · %s", control->label, rgb[column]);
  else snprintf(label, sizeof(label), "%s · %u", control->label, column + 1);
  return label;
}
#endif
