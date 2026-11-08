/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DT_MOBILE_BLENDING_CONTROLS_H
#define DT_MOBILE_BLENDING_CONTROLS_H

#include "develop/blend.h"

/* Basic blending uses the original module parameters and commit path, retaining
 * drawn forms and parametric thresholds. Raster selection uses the original
 * source registration; separate helpers edit drawn geometry.
 * Choice membership follows src/develop/blend_gui.c, including colorspace
 * restrictions. Labels come from its original introspection table. */
static dt_develop_blend_colorspace_t mobile_blending_colorspace(dt_iop_module_t *module)
{
  const dt_develop_blend_colorspace_t original = dt_develop_blend_default_module_blend_colorspace(module);
  if(original == DEVELOP_BLEND_CS_RAW || original == DEVELOP_BLEND_CS_NONE) return original;
  switch(module->blend_params->blend_cst)
  {
    case DEVELOP_BLEND_CS_LAB:
    case DEVELOP_BLEND_CS_RGB_DISPLAY:
    case DEVELOP_BLEND_CS_RGB_SCENE: return module->blend_params->blend_cst;
    default: return original;
  }
}

static gboolean mobile_blending_available(dt_iop_module_t *module)
{
  return module && module->blend_params && module->default_blendop_params
    && (module->flags() & IOP_FLAGS_SUPPORTS_BLENDING)
    && mobile_blending_colorspace(module) != DEVELOP_BLEND_CS_NONE;
}

static gboolean mobile_blending_mode_allowed(dt_develop_blend_colorspace_t colorspace, uint32_t mode)
{
  switch(mode)
  {
    case DEVELOP_BLEND_NORMAL2:
    case DEVELOP_BLEND_AVERAGE:
    case DEVELOP_BLEND_DIFFERENCE2:
    case DEVELOP_BLEND_MULTIPLY:
    case DEVELOP_BLEND_ADD:
    case DEVELOP_BLEND_SUBTRACT: return TRUE;
    case DEVELOP_BLEND_DIVIDE:
    case DEVELOP_BLEND_GEOMETRIC_MEAN:
    case DEVELOP_BLEND_HARMONIC_MEAN: return colorspace == DEVELOP_BLEND_CS_RGB_SCENE;
    case DEVELOP_BLEND_RGB_R:
    case DEVELOP_BLEND_RGB_G:
    case DEVELOP_BLEND_RGB_B:
      return colorspace == DEVELOP_BLEND_CS_RGB_SCENE || colorspace == DEVELOP_BLEND_CS_RGB_DISPLAY;
    case DEVELOP_BLEND_LIGHTNESS:
    case DEVELOP_BLEND_CHROMATICITY: return colorspace != DEVELOP_BLEND_CS_RAW;
    case DEVELOP_BLEND_LAB_LIGHTNESS:
    case DEVELOP_BLEND_LAB_A:
    case DEVELOP_BLEND_LAB_B:
    case DEVELOP_BLEND_LAB_COLOR: return colorspace == DEVELOP_BLEND_CS_LAB;
    case DEVELOP_BLEND_HSV_VALUE:
    case DEVELOP_BLEND_HSV_COLOR: return colorspace == DEVELOP_BLEND_CS_RGB_DISPLAY;
    case DEVELOP_BLEND_HUE:
    case DEVELOP_BLEND_COLOR:
    case DEVELOP_BLEND_COLORADJUST:
      return colorspace == DEVELOP_BLEND_CS_LAB || colorspace == DEVELOP_BLEND_CS_RGB_DISPLAY;
    case DEVELOP_BLEND_BOUNDED:
    case DEVELOP_BLEND_LIGHTEN:
    case DEVELOP_BLEND_DARKEN:
    case DEVELOP_BLEND_SCREEN:
    case DEVELOP_BLEND_OVERLAY:
    case DEVELOP_BLEND_SOFTLIGHT:
    case DEVELOP_BLEND_HARDLIGHT:
    case DEVELOP_BLEND_VIVIDLIGHT:
    case DEVELOP_BLEND_LINEARLIGHT:
    case DEVELOP_BLEND_PINLIGHT: return colorspace != DEVELOP_BLEND_CS_RGB_SCENE;
    default: return FALSE;
  }
}

static gboolean mobile_blending_fulcrum_for_mode(dt_develop_blend_colorspace_t colorspace, uint32_t mode)
{
  if(colorspace != DEVELOP_BLEND_CS_RGB_SCENE) return FALSE;
  switch(mode & DEVELOP_BLEND_MODE_MASK)
  {
    case DEVELOP_BLEND_ADD:
    case DEVELOP_BLEND_MULTIPLY:
    case DEVELOP_BLEND_SUBTRACT:
    case DEVELOP_BLEND_SUBTRACT_INVERSE:
    case DEVELOP_BLEND_DIVIDE:
    case DEVELOP_BLEND_DIVIDE_INVERSE:
    case DEVELOP_BLEND_RGB_R:
    case DEVELOP_BLEND_RGB_G:
    case DEVELOP_BLEND_RGB_B: return TRUE;
    default: return FALSE;
  }
}

static gboolean mobile_blending_fulcrum_available(dt_iop_module_t *module)
{
  return mobile_blending_fulcrum_for_mode(mobile_blending_colorspace(module), module->blend_params->blend_mode);
}

static void append_mobile_blend_number(parameter_json *description, const char *name, const char *label,
                                       const char *type, double value, double minimum, double maximum,
                                       double original)
{
  if(!isfinite(value)) return;
  if(!description->first) g_string_append_c(description->json, ',');
  description->first = FALSE;
  GString *json = description->json;
  g_string_append(json, "{\"name\":"); json_string(json, name);
  g_string_append(json, ",\"label\":"); json_string(json, label);
  g_string_append(json, ",\"type\":"); json_string(json, type);
  g_string_append(json, ",\"value\":"); json_number(json, value);
  g_string_append(json, ",\"min\":"); json_number(json, minimum);
  g_string_append(json, ",\"max\":"); json_number(json, maximum);
  if(isfinite(original) && original >= minimum && original <= maximum)
  {
    g_string_append(json, ",\"default\":"); json_number(json, original);
  }
  g_string_append_c(json, '}');
}

#include "dt_mobile_parametric_controls.h"

static gint mobile_raster_id_compare(gconstpointer a, gconstpointer b)
{
  const int left = GPOINTER_TO_INT(a), right = GPOINTER_TO_INT(b);
  return (left > right) - (left < right);
}

/* Raster sources must precede their consumer in the native pixelpipe. Stable
 * operation/instance/mask identities avoid retargeting a selection as sources
 * are enabled or disabled. The original advertised mask table supplies labels. */
static void append_mobile_raster_parameters(dt_iop_module_t *module, parameter_json *description)
{
  if(!module->dev || (module->flags() & IOP_FLAGS_NO_MASKS)) return;
  const dt_develop_blend_params_t *p = module->blend_params;
  for(GList *item = module->dev->iop; item; item = item->next)
  {
    dt_iop_module_t *source = item->data;
    if(source == module) break;
    if(!source->enabled || !source->raster_mask.source.masks) continue;
    GList *keys = g_list_sort(g_hash_table_get_keys(source->raster_mask.source.masks), mobile_raster_id_compare);
    for(GList *mask = keys; mask; mask = mask->next)
    {
      const int id = GPOINTER_TO_INT(mask->data);
      if(id < 0) continue;
      char identifier[128], name[192], label[256];
      module_identifier(source, identifier, sizeof(identifier));
      snprintf(name, sizeof(name), "blend.raster_source.%s.%d", identifier, id);
      const char *native_label = g_hash_table_lookup(source->raster_mask.source.masks, mask->data);
      snprintf(label, sizeof(label), "use raster mask · %s", native_label ? native_label : identifier);
      const gboolean selected = (p->mask_mode & DEVELOP_MASK_RASTER)
        && !strcmp(p->raster_mask_source, source->op) && p->raster_mask_instance == source->multi_priority
        && p->raster_mask_id == id;
      append_mobile_blend_number(description, name, label, "bool", selected, 0, 1, 0);
    }
    g_list_free(keys);
  }
  if(p->mask_mode & DEVELOP_MASK_RASTER)
    append_mobile_blend_number(description, "blend.raster_invert", "invert raster mask", "bool",
      p->raster_mask_invert != 0, 0, 1, 0);
}

static int update_mobile_raster_parameter(dt_iop_module_t *module, const char *parameter, double value)
{
  if(strncmp(parameter, "blend.raster_", 13)) return 0;
  if(!module->dev || (module->flags() & IOP_FLAGS_NO_MASKS) || (value != 0 && value != 1)) return -1;
  dt_develop_blend_params_t p = *module->blend_params;
  if(!strcmp(parameter, "blend.raster_invert"))
  {
    if(!(p.mask_mode & DEVELOP_MASK_RASTER)) return -1;
    p.raster_mask_invert = value != 0;
    dt_iop_commit_blend_params(module, &p, NULL);
    return 1;
  }
  if(strncmp(parameter, "blend.raster_source.", 20)) return -1;
  for(GList *item = module->dev->iop; item; item = item->next)
  {
    dt_iop_module_t *source = item->data;
    if(source == module) break;
    if(!source->enabled || !source->raster_mask.source.masks) continue;
    GHashTableIter masks;
    gpointer key, label;
    g_hash_table_iter_init(&masks, source->raster_mask.source.masks);
    while(g_hash_table_iter_next(&masks, &key, &label))
    {
      const int id = GPOINTER_TO_INT(key);
      if(id < 0) continue;
      char identifier[128], name[192];
      module_identifier(source, identifier, sizeof(identifier));
      snprintf(name, sizeof(name), "blend.raster_source.%s.%d", identifier, id);
      if(strcmp(name, parameter)) continue;
      const gboolean selected = (p.mask_mode & DEVELOP_MASK_RASTER) && !strcmp(p.raster_mask_source, source->op)
        && p.raster_mask_instance == source->multi_priority && p.raster_mask_id == id;
      if(value)
      {
        g_strlcpy(p.raster_mask_source, source->op, sizeof(p.raster_mask_source));
        p.raster_mask_instance = source->multi_priority;
        p.raster_mask_id = id;
        p.mask_mode = DEVELOP_MASK_ENABLED | DEVELOP_MASK_RASTER;
      }
      else if(selected)
      {
        memset(p.raster_mask_source, 0, sizeof(p.raster_mask_source));
        p.raster_mask_instance = 0;
        p.raster_mask_id = INVALID_MASKID;
        p.mask_mode = DEVELOP_MASK_ENABLED;
      }
      else return 1; /* An unselected source's off state does not clear another mask. */
      if(module->raster_mask.sink.source && module->raster_mask.sink.source != (value ? source : NULL))
        g_hash_table_remove(module->raster_mask.sink.source->raster_mask.source.users, module);
      dt_iop_commit_blend_params(module, &p, NULL);
      return 1;
    }
  }
  return -1;
}

static void append_mobile_blending_parameters(dt_iop_module_t *module, parameter_json *description)
{
  if(!mobile_blending_available(module)) return;
  const dt_develop_blend_params_t *p = module->blend_params, *d = module->default_blendop_params;
  const dt_develop_blend_colorspace_t colorspace = mobile_blending_colorspace(module);
  append_mobile_blend_number(description, "blend.enabled", "enable blending", "bool",
                             !!(p->mask_mode & DEVELOP_MASK_ENABLED), 0, 1,
                             !!(d->mask_mode & DEVELOP_MASK_ENABLED));
  append_mobile_blend_number(description, "blend.opacity", "opacity (%)", "float", p->opacity, 0, 100, d->opacity);
  append_mobile_blend_number(description, "blend.reverse", "reverse blend order", "bool",
                             !!(p->blend_mode & DEVELOP_BLEND_REVERSE), 0, 1,
                             !!(d->blend_mode & DEVELOP_BLEND_REVERSE));
  if(mobile_blending_fulcrum_available(module))
    append_mobile_blend_number(description, "blend.fulcrum", "blend fulcrum (EV)", "float",
                               p->blend_parameter, -18, 18, d->blend_parameter);
  GString *json = description->json;
  if(!description->first) g_string_append_c(json, ',');
  description->first = FALSE;
  g_string_append_printf(json, "{\"name\":\"blend.mode\",\"label\":\"blend mode\",\"type\":\"enum\",\"value\":%u,\"min\":0,\"max\":255",
                         p->blend_mode & DEVELOP_BLEND_MODE_MASK);
  const uint32_t original = d->blend_mode & DEVELOP_BLEND_MODE_MASK;
  if(mobile_blending_mode_allowed(colorspace, original))
    g_string_append_printf(json, ",\"default\":%u", original);
  g_string_append(json, ",\"choices\":[");
  gboolean first = TRUE;
  for(size_t k = 0; dt_develop_blend_mode_names[k].name; k++)
  {
    const dt_introspection_type_enum_tuple_t *choice = dt_develop_blend_mode_names + k;
    if(!mobile_blending_mode_allowed(colorspace, choice->value)) continue;
    if(!first) g_string_append_c(json, ',');
    first = FALSE;
    g_string_append_printf(json, "{\"value\":%d,\"label\":", choice->value);
    json_string(json, choice->description && choice->description[0] ? choice->description : choice->name);
    g_string_append_c(json, '}');
  }
  g_string_append(json, "]}");
  append_mobile_raster_parameters(module, description);
  append_mobile_parametric_parameters(module, description);
}

/* 0: unrelated field, -1: invalid/unsupported, 1: committed successfully.
 * Validate the entire request before committing; failures leave masks, params
 * and history unchanged. Non-enable changes activate blending just as choosing
 * uniform blending in the desktop panel does, preserving any mask flags. */
static int update_mobile_blending_parameter(dt_iop_module_t *module, const char *parameter, double value)
{
  if(strncmp(parameter, "blend.", 6)) return 0;
  if(!mobile_blending_available(module) || !isfinite(value)) return -1;
  const int parametric_update = update_mobile_parametric_parameter(module, parameter, value);
  if(parametric_update) return parametric_update;
  const int raster_update = update_mobile_raster_parameter(module, parameter, value);
  if(raster_update) return raster_update;
  dt_develop_blend_params_t p = *module->blend_params;
  if(!strcmp(parameter, "blend.enabled"))
  {
    if(value != 0 && value != 1) return -1;
    if(value) p.mask_mode |= DEVELOP_MASK_ENABLED;
    else p.mask_mode &= ~DEVELOP_MASK_ENABLED;
  }
  else
  {
    if(!strcmp(parameter, "blend.opacity"))
    {
      if(value < 0 || value > 100) return -1;
      p.opacity = (float)value;
    }
    else if(!strcmp(parameter, "blend.reverse"))
    {
      if(value != 0 && value != 1) return -1;
      if(value) p.blend_mode |= DEVELOP_BLEND_REVERSE;
      else p.blend_mode &= ~DEVELOP_BLEND_REVERSE;
    }
    else if(!strcmp(parameter, "blend.mode"))
    {
      if(value < 0 || value > 255 || value != trunc(value)
         || !mobile_blending_mode_allowed(mobile_blending_colorspace(module), (uint32_t)value)) return -1;
      p.blend_mode = (p.blend_mode & ~DEVELOP_BLEND_MODE_MASK) | (uint32_t)value;
      if(!mobile_blending_fulcrum_for_mode(mobile_blending_colorspace(module), p.blend_mode)) p.blend_parameter = 0;
    }
    else if(!strcmp(parameter, "blend.fulcrum"))
    {
      if(!mobile_blending_fulcrum_available(module) || value < -18 || value > 18) return -1;
      p.blend_parameter = (float)value;
    }
    else return -1;
    p.mask_mode |= DEVELOP_MASK_ENABLED;
  }
  dt_iop_commit_blend_params(module, &p, NULL);
  return 1;
}

#endif
