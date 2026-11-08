/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DT_MOBILE_EDITOR_ACTIONS_H
#define DT_MOBILE_EDITOR_ACTIONS_H

/* Original plugins own desktop control coupling. These hooks only describe
 * controls and apply one complete parameter batch; the engine saves history. */
typedef int (*mobile_editor_control_function)(dt_iop_module_t *, const char *, double *, double *, double *, double *, int *);
typedef int (*mobile_editor_number_function)(dt_iop_module_t *, const char *, double);

/* Desktop hides the manual control while its automatic driver supplies the
 * effective value. Read the original boolean through plugin introspection. */
static gboolean mobile_editor_parameter_visible(dt_iop_module_t *module, const char *name)
{
  if(!module || !name) return TRUE;
  const char *automatic = !strcmp(module->op, "agx") && !strcmp(name, "curve_gamma") ? "auto_gamma"
    : !strcmp(module->op, "filmicrgb") && !strcmp(name, "output_power") ? "auto_hardness" : NULL;
  if(!automatic || !module->params || !module->get_f || !module->get_p) return TRUE;
  const dt_introspection_field_t *field = module->get_f(automatic);
  if(!field || field->header.type != DT_INTROSPECTION_TYPE_BOOL) return TRUE;
  const gboolean *enabled = module->get_p(module->params, automatic);
  return !enabled || !*enabled;
}

static void append_mobile_editor_actions(dt_iop_module_t *module, parameter_json *description)
{
  if(!module || !module->module || !description
     || (strcmp(module->op, "agx") && strcmp(module->op, "filmicrgb"))) return;
  mobile_editor_control_function get = NULL;
  g_module_symbol(module->module, "dt_iop_mobile_editor_get_control", (gpointer *)&get);
  if(!get) return;
  static const struct { const char *operation, *name, *label; } controls[] = {
    { "agx", "dynamic_range_scaling", "dynamic range scaling" },
    { "agx", "agx.range_from_exposure", "read range from exposure" },
    { "agx", "agx.primaries_blender", "Blender-like primaries" },
    { "agx", "agx.primaries_smooth", "smooth primaries" },
    { "agx", "agx.primaries_unmodified", "unmodified primaries" },
    { "agx", "agx.copy_pre_to_post", "reverse pre-mapping primaries" },
    { "filmicrgb", "security_factor", "dynamic range scaling" },
    { "filmicrgb", "auto_hardness", "auto adjust hardness" },
  };
  for(size_t index = 0; index < sizeof(controls) / sizeof(controls[0]); index++)
  {
    if(strcmp(module->op, controls[index].operation)) continue;
    /* Keep this safe if a reviewed control later joins scalar introspection. */
    char *key = g_strdup_printf("{\"name\":\"%s\",", controls[index].name);
    const char *parameters = g_strrstr(description->json->str, ",\"parameters\":[");
    const gboolean duplicate = strstr(parameters ? parameters : description->json->str, key) != NULL;
    g_free(key);
    if(duplicate) continue;
    double value, minimum, maximum, original;
    int kind;
    if(get(module, controls[index].name, &value, &minimum, &maximum, &original, &kind) != 1
       || (kind != 0 && kind != 2 && kind != 3)) continue;
    append_mobile_blend_number(description, controls[index].name, controls[index].label,
      kind == 2 ? "bool" : kind == 3 ? "action" : "float", value, minimum, maximum, original);
  }
}

static int update_mobile_editor_parameter(dt_iop_module_t *module, const char *name, double value)
{
  if(!module || !name || (strcmp(module->op, "agx") && strcmp(module->op, "filmicrgb"))) return 0;
  mobile_editor_number_function update = NULL;
  if(module->module)
    g_module_symbol(module->module, "dt_iop_mobile_editor_set_number", (gpointer *)&update);
  return update ? update(module, name, value) : -1;
}

#endif
