/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DT_MOBILE_LENS_AUTO_CHECKS_H
#define DT_MOBILE_LENS_AUTO_CHECKS_H

#include "develop/develop.h"

/* A parameter-state fixture using the installed original lens plugin. It does
 * not establish HERO12 RAW decoding, calibration, or corrected image pixels. */
static int mobile_lens_auto_checks(void)
{
  typedef char *(*metadata_function)(dt_iop_module_t *);
  typedef int (*prepare_function)(dt_iop_module_t *);
  typedef int (*number_function)(dt_iop_module_t *, const char *, double);
  typedef int (*text_function)(dt_iop_module_t *, const char *, const char *);
  metadata_function metadata = NULL;
  prepare_function prepare = NULL;
  number_function set_number = NULL;
  text_function set_text = NULL;
  dt_iop_module_t *module = NULL;
  dt_develop_t *develop = NULL;
  void *params_before = NULL, *defaults_before = NULL;
  char *json = NULL;
  int success = 0;
#define LENS_AUTO_CHECK(condition) do { if(!(condition)) { \
  fprintf(stderr, "lens automatic settings:%d: check failed: %s\n", __LINE__, #condition); \
  goto finished; } } while(0)

  dt_iop_module_so_t *plugin = dt_iop_get_module_so("lens");
  LENS_AUTO_CHECK(plugin && plugin->module && plugin->data && plugin->get_introspection && plugin->get_p && plugin->get_f);
  const dt_introspection_t *schema = plugin->get_introspection();
  LENS_AUTO_CHECK(schema && schema->size > 0);
  const dt_introspection_field_t *camera_field = plugin->get_f("camera");
  const dt_introspection_field_t *lens_field = plugin->get_f("lens");
  LENS_AUTO_CHECK(camera_field && camera_field->header.type == DT_INTROSPECTION_TYPE_ARRAY
    && camera_field->Array.type == DT_INTROSPECTION_TYPE_CHAR && camera_field->Array.count > 12);
  LENS_AUTO_CHECK(lens_field && lens_field->header.type == DT_INTROSPECTION_TYPE_ARRAY
    && lens_field->Array.type == DT_INTROSPECTION_TYPE_CHAR && lens_field->Array.count > 5);
  LENS_AUTO_CHECK(g_module_symbol(plugin->module, "dt_iop_mobile_lens_metadata", (gpointer *)&metadata));
  LENS_AUTO_CHECK(g_module_symbol(plugin->module, "dt_iop_mobile_lens_prepare", (gpointer *)&prepare));
  LENS_AUTO_CHECK(g_module_symbol(plugin->module, "dt_iop_mobile_lens_set_number", (gpointer *)&set_number));
  LENS_AUTO_CHECK(g_module_symbol(plugin->module, "dt_iop_mobile_lens_set_text", (gpointer *)&set_text));

  module = g_new0(dt_iop_module_t, 1);
  develop = g_new0(dt_develop_t, 1);
  module->module = plugin->module;
  module->so = plugin;
  module->global_data = plugin->data; /* Shared immutable database owned by the runtime. */
  module->dev = develop;
  module->enabled = TRUE;
  module->params_size = schema->size;
  module->params = g_malloc0(schema->size);
  module->default_params = g_malloc0(schema->size);
  g_strlcpy(module->op, "lens", sizeof(module->op));
  develop->image_storage.width = develop->image_storage.height = 256;
  g_strlcpy(develop->image_storage.exif_model, "HERO12 Black", sizeof(develop->image_storage.exif_model));
  g_strlcpy(develop->image_storage.exif_lens, "HERO4", sizeof(develop->image_storage.exif_lens));

#define LENS_AUTO_SET(field_name, field_type, field_value) do { \
  field_type *field = plugin->get_p(module->default_params, #field_name); \
  LENS_AUTO_CHECK(field); *field = field_value; } while(0)
  LENS_AUTO_SET(method, int, 1);
  LENS_AUTO_SET(modify_flags, int, 7);
  LENS_AUTO_SET(target_geom, int, 1);
  LENS_AUTO_SET(crop, float, 5.57f);
  LENS_AUTO_SET(focal, float, 2.66f);
  LENS_AUTO_SET(aperture, float, 2.8f);
  LENS_AUTO_SET(distance, float, 1000.0f);
  LENS_AUTO_SET(scale, float, 1.0f);
  LENS_AUTO_SET(v_radius, float, .5f);
  LENS_AUTO_SET(v_steepness, float, .5f);
  LENS_AUTO_SET(has_been_set, gboolean, FALSE);
#undef LENS_AUTO_SET
  char *default_camera = plugin->get_p(module->default_params, "camera");
  char *default_lens = plugin->get_p(module->default_params, "lens");
  char *current_lens = plugin->get_p(module->params, "lens");
  gboolean *modified = plugin->get_p(module->params, "has_been_set");
  LENS_AUTO_CHECK(default_camera && default_lens && current_lens && modified);
  g_strlcpy(default_camera, "HERO12 Black", camera_field->Array.count);
  g_strlcpy(default_lens, "HERO4", lens_field->Array.count);
  memcpy(module->params, module->default_params, schema->size);
  params_before = g_memdup2(module->params, schema->size);
  defaults_before = g_memdup2(module->default_params, schema->size);

  fprintf(stderr, "lens automatic settings: unmatched EXIF is not a database selection\n");
  json = metadata(module);
  LENS_AUTO_CHECK(json && strstr(json, "\"status\":\"missing_camera\"")
    && strstr(json, "\"cameraMatched\":false") && strstr(json, "\"lensMatched\":false")
    && strstr(json, "\"camera\":\"\"") && strstr(json, "\"lens\":\"\"")
    && strstr(json, "\"lenses\":[]"));
  LENS_AUTO_CHECK(strstr(json, "\"imageCamera\":\"HERO12 Black\"")
    && strstr(json, "\"imageLens\":\"HERO4\""));
  LENS_AUTO_CHECK(!memcmp(module->params, params_before, schema->size)
    && !memcmp(module->default_params, defaults_before, schema->size));
  g_clear_pointer(&json, g_free);

  LENS_AUTO_CHECK(prepare(module) == 1);
  LENS_AUTO_CHECK(*modified && !current_lens[0]);
  LENS_AUTO_CHECK(!strcmp(default_lens, "HERO4")
    && !strcmp(develop->image_storage.exif_model, "HERO12 Black")
    && !strcmp(develop->image_storage.exif_lens, "HERO4"));
  LENS_AUTO_CHECK(!memcmp(module->default_params, defaults_before, schema->size));

  /* Existing explicit manual parameters are retained, but must be labelled
   * unverified and cannot open the normal compatible-profile selection route. */
  g_strlcpy(current_lens, "HERO4", lens_field->Array.count);
  *modified = TRUE;
  memcpy(params_before, module->params, schema->size);
  LENS_AUTO_CHECK(prepare(module) == 0);
  LENS_AUTO_CHECK(!memcmp(module->params, params_before, schema->size));
  json = metadata(module);
  LENS_AUTO_CHECK(json && strstr(json, "\"status\":\"unverified_camera\"")
    && strstr(json, "\"cameraMatched\":false") && strstr(json, "\"lensMatched\":true")
    && strstr(json, "\"lenses\":[]"));
  g_clear_pointer(&json, g_free);
  LENS_AUTO_CHECK(set_text(module, "lens.profile", "HERO4") == -1);
  LENS_AUTO_CHECK(!memcmp(module->params, params_before, schema->size));

  /* Re-entering automatic database correction also verifies the body instead
   * of reviving the unrelated lens carried by another correction method. */
  LENS_AUTO_CHECK(set_number(module, "method", 2) == 1);
  LENS_AUTO_CHECK(!strcmp(current_lens, "HERO4"));
  LENS_AUTO_CHECK(set_number(module, "method", 1) == 1);
  LENS_AUTO_CHECK(*modified && !current_lens[0]);

  /* Resetting to image defaults must not reintroduce an unverified EXIF lens. */
  g_strlcpy(current_lens, "HERO4", lens_field->Array.count);
  LENS_AUTO_CHECK(set_number(module, "lens.autodetect", 1) == 1);
  LENS_AUTO_CHECK(*modified && !current_lens[0]);
  LENS_AUTO_CHECK(!memcmp(module->default_params, defaults_before, schema->size));
  success = 1;
finished:
  g_free(json);
  g_free(params_before);
  g_free(defaults_before);
  if(module)
  {
    g_free(module->params);
    g_free(module->default_params);
    g_free(module);
  }
  g_free(develop);
#undef LENS_AUTO_CHECK
  return success;
}

#endif
