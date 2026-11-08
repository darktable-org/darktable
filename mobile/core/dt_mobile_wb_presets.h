/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DT_MOBILE_WB_PRESETS_H
#define DT_MOBILE_WB_PRESETS_H

/* The original temperature plugin owns catalog grouping, interpolation and
 * parameter layout. The engine owns the one history entry and chroma update. */
static void append_mobile_wb_presets(dt_iop_module_t *module, GString *json)
{
  if(!module || strcmp(module->op, "temperature") || !module->module) return;
  typedef char *(*metadata_function)(dt_iop_module_t *);
  metadata_function metadata = NULL;
  g_module_symbol(module->module, "dt_iop_mobile_temperature_presets", (gpointer *)&metadata);
  char *result = metadata ? metadata(module) : NULL;
  if(result)
  {
    g_string_append(json, ",\"cameraPresets\":");
    g_string_append(json, result);
    g_free(result);
  }
}

static int set_mobile_whitebalance_preset(dt_iop_module_t *module, int id, int tuning)
{
  if(!module || strcmp(module->op, "temperature") || !module->module) return -1;
  typedef int (*setter_function)(dt_iop_module_t *, int, int);
  setter_function setter = NULL;
  g_module_symbol(module->module, "dt_iop_mobile_temperature_set_preset", (gpointer *)&setter);
  return setter ? setter(module, id, tuning) : -1;
}
#endif
