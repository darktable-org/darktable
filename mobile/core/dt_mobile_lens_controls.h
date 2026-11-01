/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DT_MOBILE_LENS_CONTROLS_H
#define DT_MOBILE_LENS_CONTROLS_H

/* The original plugin resolves image defaults, database matches and mode-specific
 * consumption. Never write its has_been_set flag through the generic setter. */
typedef int (*mobile_lens_control_function)(dt_iop_module_t *, const char *, double *, double *, double *, double *, int *);
typedef int (*mobile_lens_number_function)(dt_iop_module_t *, const char *, double);
typedef int (*mobile_lens_text_function)(dt_iop_module_t *, const char *, const char *);
typedef char *(*mobile_lens_metadata_function)(dt_iop_module_t *);
typedef int (*mobile_lens_choice_function)(dt_iop_module_t *, const char *, int);

static void append_mobile_lens_parameters(dt_iop_module_t *module, parameter_json *description)
{
  if(!module || strcmp(module->op, "lens") || !module->module) return;
  mobile_lens_control_function get = NULL;
  mobile_lens_choice_function allowed = NULL;
  g_module_symbol(module->module, "dt_iop_mobile_lens_get_control", (gpointer *)&get);
  g_module_symbol(module->module, "dt_iop_mobile_lens_choice_allowed", (gpointer *)&allowed);
  if(!get) return;
  static const struct { const char *name, *label; } controls[] = {
    { "method", "correction method" }, { "modify_flags", "corrections" }, { "inverse", "mode" },
    { "target_geom", "target geometry" }, { "scale", "image scale" }, { "crop", "sensor crop factor" },
    { "focal", "focal length (mm)" }, { "aperture", "aperture (f/)" }, { "distance", "focus distance (m)" },
    { "tca_override", "manual chromatic aberration" }, { "tca_r", "TCA red" }, { "tca_b", "TCA blue" },
    { "cor_dist_ft", "embedded warp strength" }, { "cor_vig_ft", "embedded vignetting strength" },
    { "cor_ca_r_ft", "embedded TCA red" }, { "cor_ca_b_ft", "embedded TCA blue" },
    { "scale_md", "embedded image scale" }, { "v_strength", "manual vignette strength" },
    { "v_radius", "manual vignette radius" }, { "v_steepness", "manual vignette steepness" },
    { "lens.autodetect", "use image lens defaults" }, { "lens.autoscale", "automatic lens scale" },
  };
  for(size_t index = 0; index < sizeof(controls) / sizeof(controls[0]); index++)
  {
    double value, minimum, maximum, original;
    int kind;
    if(get(module, controls[index].name, &value, &minimum, &maximum, &original, &kind) != 1) continue;
    if(kind != 1)
    {
      append_mobile_blend_number(description, controls[index].name, controls[index].label,
        kind == 2 ? "bool" : kind == 3 ? "action" : "float", value, minimum, maximum, original);
      continue;
    }
    const dt_introspection_field_t *field = module->get_f ? module->get_f(controls[index].name) : NULL;
    if(!field || field->header.type != DT_INTROSPECTION_TYPE_ENUM) continue;
    if(!description->first) g_string_append_c(description->json, ',');
    description->first = FALSE;
    GString *json = description->json;
    g_string_append(json, "{\"name\":"); json_string(json, controls[index].name);
    g_string_append(json, ",\"label\":"); json_string(json, controls[index].label);
    g_string_append(json, ",\"type\":\"enum\",\"value\":"); json_number(json, value);
    g_string_append(json, ",\"min\":"); json_number(json, minimum);
    g_string_append(json, ",\"max\":"); json_number(json, maximum);
    if(original >= minimum && original <= maximum)
    { g_string_append(json, ",\"default\":"); json_number(json, original); }
    g_string_append(json, ",\"choices\":[");
    gboolean first = TRUE;
    for(size_t choice = 0; choice < enum_entry_count(field); choice++)
    {
      const dt_introspection_type_enum_tuple_t *entry = field->Enum.values + choice;
      if(entry->value < minimum || entry->value > maximum) continue;
      if(allowed && !allowed(module, controls[index].name, entry->value)) continue;
      if(!first) g_string_append_c(json, ',');
      first = FALSE;
      g_string_append_printf(json, "{\"value\":%d,\"label\":", entry->value);
      json_string(json, entry->description ? entry->description : entry->name);
      g_string_append_c(json, '}');
    }
    g_string_append(json, "]}");
  }
}

static void append_mobile_lens_correction(dt_iop_module_t *module, GString *json)
{
  if(!module || strcmp(module->op, "lens") || !module->module) return;
  mobile_lens_metadata_function metadata = NULL;
  g_module_symbol(module->module, "dt_iop_mobile_lens_metadata", (gpointer *)&metadata);
  char *result = metadata ? metadata(module) : NULL;
  if(result)
  { g_string_append(json, ",\"lensCorrection\":"); g_string_append(json, result); g_free(result); }
}

static int update_mobile_lens_parameter(dt_iop_module_t *module, const char *name, double value)
{
  if(!module || strcmp(module->op, "lens")) return 0;
  mobile_lens_number_function update = NULL;
  if(module->module) g_module_symbol(module->module, "dt_iop_mobile_lens_set_number", (gpointer *)&update);
  return update ? update(module, name, value) : -1;
}

static int update_mobile_lens_text_parameter(dt_iop_module_t *module, const char *name, const char *value)
{
  if(!module || strcmp(module->op, "lens")) return 0;
  mobile_lens_text_function update = NULL;
  if(module->module) g_module_symbol(module->module, "dt_iop_mobile_lens_set_text", (gpointer *)&update);
  return update ? update(module, name, value) : -1;
}
#endif
