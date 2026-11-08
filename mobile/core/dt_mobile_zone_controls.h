/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DT_MOBILE_ZONE_CONTROLS_H
#define DT_MOBILE_ZONE_CONTROLS_H

#include "common/zonemap.h"

enum { MOBILE_ZONE_MAX_SIZE = 24, MOBILE_ZONE_STORAGE = 25 };
typedef struct mobile_zone_layout
{
  size_t size_offset, zone_offset;
  int count;
  float values[MOBILE_ZONE_STORAGE], map[MOBILE_ZONE_MAX_SIZE];
} mobile_zone_layout;

/* Zone count and -1 automatic sentinels are a coupled native structure. The
 * ordinary scalar/array bridge cannot safely expose either independently. */
static gboolean mobile_zone_layout_read(dt_iop_module_t *module, mobile_zone_layout *layout)
{
  if(!module || strcmp(module->op, "zonesystem") || !module->have_introspection
     || !module->get_f || !module->params || module->params_size <= 0) return FALSE;
  const dt_introspection_field_t *size = module->get_f("size"), *zone = module->get_f("zone");
  if(!size || size->header.type != DT_INTROSPECTION_TYPE_INT || size->header.size != sizeof(int)
     || size->header.offset > (size_t)module->params_size
     || size->header.size > (size_t)module->params_size - size->header.offset
     || !zone || zone->header.type != DT_INTROSPECTION_TYPE_ARRAY
     || zone->Array.count != MOBILE_ZONE_STORAGE || zone->Array.type != DT_INTROSPECTION_TYPE_FLOAT
     || zone->header.size != MOBILE_ZONE_STORAGE * sizeof(float) || !zone->Array.field
     || zone->Array.field->header.offset != zone->header.offset
     || zone->Array.field->header.type != DT_INTROSPECTION_TYPE_FLOAT
     || zone->Array.field->header.size != sizeof(float)
     || zone->header.offset > (size_t)module->params_size
     || zone->header.size > (size_t)module->params_size - zone->header.offset) return FALSE;
  layout->size_offset = size->header.offset;
  layout->zone_offset = zone->header.offset;
  memcpy(&layout->count, (const char *)module->params + layout->size_offset, sizeof(layout->count));
  if(layout->count < 4 || layout->count > MOBILE_ZONE_MAX_SIZE) return FALSE;
  memcpy(layout->values, (const char *)module->params + layout->zone_offset, sizeof(layout->values));
  for(int index = 1; index < layout->count - 1; index++)
    if(!isfinite(layout->values[index]) || (layout->values[index] != -1.f
       && (layout->values[index] <= 0.f || layout->values[index] >= 1.f))) return FALSE;
  dt_zonesystem_calculate_zonemap(layout->values, layout->count, layout->map);
  for(int index = 0; index < layout->count; index++)
    if(!isfinite(layout->map[index]) || (index && layout->map[index] - layout->map[index - 1] < 1e-6f)) return FALSE;
  return TRUE;
}

static void append_mobile_zone_parameters(dt_iop_module_t *module, parameter_json *description)
{
  mobile_zone_layout zones;
  if(!mobile_zone_layout_read(module, &zones)) return;
  double default_count = NAN;
  if(module->default_params)
  {
    int original;
    memcpy(&original, (const char *)module->default_params + zones.size_offset, sizeof(original));
    if(original >= 4 && original <= MOBILE_ZONE_MAX_SIZE) default_count = original;
  }
  append_mobile_blend_number(description, "zones.count", "zone divisions (resets boundary positions)",
    "int", zones.count, 4, MOBILE_ZONE_MAX_SIZE, default_count);
  for(int index = 1; index < zones.count - 1; index++)
  {
    char name[64], label[96];
    snprintf(name, sizeof(name), "zones.position[%d]", index);
    snprintf(label, sizeof(label), "zone %d output boundary", index);
    const float minimum = zones.map[index - 1] + 2e-6f;
    const float maximum = zones.map[index + 1] - 2e-6f;
    append_mobile_blend_number(description, name, label, "float", zones.map[index], minimum, maximum, NAN);
    snprintf(name, sizeof(name), "zones.automatic[%d]", index);
    snprintf(label, sizeof(label), "zone %d automatic position", index);
    append_mobile_blend_number(description, name, label, "bool", zones.values[index] == -1.f, 0, 1, 1);
  }
}

static int update_mobile_zone_parameter(dt_iop_module_t *module, const char *parameter, double value)
{
  if(strncmp(parameter, "zones.", 6)) return 0;
  mobile_zone_layout zones;
  if(!isfinite(value) || !mobile_zone_layout_read(module, &zones)) return -1;
  if(!strcmp(parameter, "zones.count"))
  {
    if(value < 4 || value > MOBILE_ZONE_MAX_SIZE || trunc(value) != value) return -1;
    const int count = (int)value;
    /* Changing the partition count is one explicit batch: reset all positions
     * to the upstream automatic sentinel, so retained pins cannot cross new
     * neighbors or leave an out-of-range structural state. */
    for(int index = 0; index < MOBILE_ZONE_STORAGE; index++) zones.values[index] = -1.f;
    memcpy((char *)module->params + zones.zone_offset, zones.values, sizeof(zones.values));
    memcpy((char *)module->params + zones.size_offset, &count, sizeof(count));
    return 1;
  }
  for(int index = 1; index < zones.count - 1; index++)
  {
    char name[64];
    snprintf(name, sizeof(name), "zones.position[%d]", index);
    if(!strcmp(parameter, name))
    {
      const float position = (float)value;
      if(!isfinite(position) || position < zones.map[index - 1] + 2e-6f
         || position > zones.map[index + 1] - 2e-6f) return -1;
      memcpy((char *)module->params + zones.zone_offset + (size_t)index * sizeof(float), &position, sizeof(position));
      return 1;
    }
    snprintf(name, sizeof(name), "zones.automatic[%d]", index);
    if(!strcmp(parameter, name))
    {
      if(value != 0.0 && value != 1.0) return -1;
      const float position = value ? -1.f : zones.map[index];
      memcpy((char *)module->params + zones.zone_offset + (size_t)index * sizeof(float), &position, sizeof(position));
      return 1;
    }
  }
  return -1;
}
#endif
