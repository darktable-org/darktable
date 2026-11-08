/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DT_MOBILE_LIQUIFY_CONTROLS_H
#define DT_MOBILE_LIQUIFY_CONTROLS_H

typedef int (*mobile_liquify_get_function)(dt_iop_module_t *, int, const char *, double *);
typedef int (*mobile_liquify_set_function)(dt_iop_module_t *, int, const char *, double);

static gboolean mobile_liquify_functions(dt_iop_module_t *module,
    mobile_liquify_get_function *get, mobile_liquify_set_function *set)
{
  *get = NULL; *set = NULL;
  if(!module || strcmp(module->op, "liquify") || !module->module) return FALSE;
  return g_module_symbol(module->module, "dt_iop_mobile_liquify_get", (gpointer *)get)
    && g_module_symbol(module->module, "dt_iop_mobile_liquify_set", (gpointer *)set);
}

static void append_mobile_liquify_parameters(dt_iop_module_t *module, parameter_json *description)
{
  mobile_liquify_get_function get;
  mobile_liquify_set_function set;
  if(!mobile_liquify_functions(module, &get, &set)) return;
  double count, minimum_radius;
  if(get(module, -1, "count", &count) != 1 || get(module, -1, "min_radius", &minimum_radius) != 1) return;
  if(count < 100)
    append_mobile_blend_number(description, "liquify.add", "add point warp", "action", 1, 1, 1, NAN);
  static const struct { const char *name, *label; double minimum, maximum; } fields[] = {
    { "x", "input center x", 0, 1 }, { "y", "input center y", 0, 1 },
    { "radius", "radius (fraction of shorter side)", 0, .5 },
    { "strength_x", "horizontal strength (fraction of shorter side)", -.5, .5 },
    { "strength_y", "vertical strength (fraction of shorter side)", -.5, .5 },
    { "hardness1", "hardness inner control", 0, 1 },
    { "hardness2", "hardness outer control", 0, 1 },
  };
  for(int index = 0; index < (int)count; index++)
  {
    double editable;
    if(get(module, index, "editable", &editable) != 1 || !editable) continue;
    for(size_t field = 0; field < sizeof(fields) / sizeof(fields[0]); field++)
    {
      double value;
      if(get(module, index, fields[field].name, &value) != 1) continue;
      char name[96], label[192];
      snprintf(name, sizeof(name), "liquify.%d.%s", index, fields[field].name);
      snprintf(label, sizeof(label), "warp %d · %s", index + 1, fields[field].label);
      double minimum = fields[field].minimum, maximum = fields[field].maximum;
      if(!strcmp(fields[field].name, "radius")) minimum = minimum_radius;
      else if(!strcmp(fields[field].name, "hardness1")) get(module, index, "hardness2", &maximum);
      else if(!strcmp(fields[field].name, "hardness2")) get(module, index, "hardness1", &minimum);
      append_mobile_blend_number(description, name, label, "float", value, minimum, maximum, NAN);
    }
    double type;
    if(get(module, index, "type", &type) == 1)
    {
      char name[96], label[160];
      snprintf(name, sizeof(name), "liquify.%d.type", index);
      snprintf(label, sizeof(label), "warp %d · type", index + 1);
      if(!description->first) g_string_append_c(description->json, ',');
      description->first = FALSE;
      GString *json = description->json;
      g_string_append(json, "{\"name\":"); json_string(json, name);
      g_string_append(json, ",\"label\":"); json_string(json, label);
      g_string_append(json, ",\"type\":\"enum\",\"value\":"); json_number(json, type);
      g_string_append(json, ",\"choices\":[{\"value\":0,\"label\":\"linear\"},"
        "{\"value\":1,\"label\":\"radial grow\"},{\"value\":2,\"label\":\"radial shrink\"}]}");
    }
    char name[96], label[160];
    snprintf(name, sizeof(name), "liquify.%d.delete", index);
    snprintf(label, sizeof(label), "delete warp %d", index + 1);
    append_mobile_blend_number(description, name, label, "action", 1, 1, 1, NAN);
  }
}

static int update_mobile_liquify_parameter(dt_iop_module_t *module, const char *name, double value)
{
  if(strncmp(name, "liquify.", 8)) return 0;
  mobile_liquify_get_function get;
  mobile_liquify_set_function set;
  if(!mobile_liquify_functions(module, &get, &set)) return -1;
  if(!strcmp(name, "liquify.add")) return set(module, -1, "add", value);
  char *end;
  const long index = strtol(name + 8, &end, 10);
  if(end == name + 8 || *end != '.' || index < 0 || index >= 100) return -1;
  char canonical[48];
  snprintf(canonical, sizeof(canonical), "liquify.%ld.", index);
  if(strncmp(name, canonical, strlen(canonical))) return -1;
  return set(module, (int)index, end + 1, value);
}
#endif
