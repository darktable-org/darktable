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

#include "backend.h"
#include "common/darktable.h"
#include "common/file_location.h"
#include "control/conf.h"
#include <glib.h>
#include <json-glib/json-glib.h>
#include <string.h>

// --- Internal Structures ---

struct dt_ai_environment_t
{
  GList *models;           // List of dt_ai_model_info_t*
  GHashTable *model_paths; // ID -> Path (string)

  // To keep pointers in dt_ai_model_info_t valid
  GList *string_storage; // List of char*

  // Remembered for refresh
  char *search_paths;

  // Default execution provider (read from config at init, override with dt_ai_env_set_provider)
  dt_ai_provider_t provider; // DT_AI_PROVIDER_AUTO = platform auto-detect

  GMutex lock; // Thread safety for model list access
};

// --- Helper Functions ---

static void _store_string(dt_ai_environment_t *env, const char *str, const char **out_ptr)
{
  char *copy = g_strdup(str);
  env->string_storage = g_list_prepend(env->string_storage, copy);
  *out_ptr = copy;
}

static void _scan_directory(dt_ai_environment_t *env, const char *root_path)
{
  GDir *dir = g_dir_open(root_path, 0, NULL);
  if(!dir)
    return;

  const char *entry_name;
  while((entry_name = g_dir_read_name(dir)))
  {
    char *full_path = g_build_filename(root_path, entry_name, NULL);
    if(g_file_test(full_path, G_FILE_TEST_IS_DIR))
    {
      char *config_path = g_build_filename(full_path, "config.json", NULL);

      if(g_file_test(config_path, G_FILE_TEST_EXISTS))
      {
        JsonParser *parser = json_parser_new();
        GError *error = NULL;

        if(json_parser_load_from_file(parser, config_path, &error))
        {
          JsonNode *root = json_parser_get_root(parser);
          JsonObject *obj = json_node_get_object(root);

          const char *id = json_object_get_string_member(obj, "id");
          const char *name = json_object_get_string_member(obj, "name");
          const char *desc = json_object_has_member(obj, "description")
            ? json_object_get_string_member(obj, "description")
            : "";
          const char *task = json_object_has_member(obj, "task")
            ? json_object_get_string_member(obj, "task")
            : "general";
          const char *backend = json_object_has_member(obj, "backend")
            ? json_object_get_string_member(obj, "backend")
            : "onnx";

          if(id && name)
          {
            // Skip duplicate model IDs (first discovered wins)
            if(g_hash_table_contains(env->model_paths, id))
            {
              dt_print(DT_DEBUG_AI, "[darktable_ai] Skipping duplicate model ID: %s", id);
            }
            else
            {
              dt_ai_model_info_t *info = g_new0(dt_ai_model_info_t, 1);
              _store_string(env, id, &info->id);
              _store_string(env, name, &info->name);
              _store_string(env, desc, &info->description);
              _store_string(env, task, &info->task_type);
              _store_string(env, backend, &info->backend);
              info->num_inputs = json_object_has_member(obj, "num_inputs")
                ? (int)json_object_get_int_member(obj, "num_inputs")
                : 1;

              env->models = g_list_prepend(env->models, info);
              g_hash_table_insert(
                env->model_paths,
                g_strdup(info->id),
                g_strdup(full_path));

              dt_print(
                DT_DEBUG_AI,
                "[darktable_ai] Discovered: %s (%s, backend=%s)",
                name,
                id,
                backend);
            }
          }
        }
        else
        {
          dt_print(DT_DEBUG_AI, "[darktable_ai] Parse error: %s", error->message);
          g_error_free(error);
        }
        g_object_unref(parser);
      }
      g_free(config_path);
    }
    g_free(full_path);
  }
  g_dir_close(dir);
}

// Scan custom search_paths + default config/data directories
static void _scan_all_paths(dt_ai_environment_t *env)
{
  if(env->search_paths)
  {
    char **tokens = g_strsplit(env->search_paths, ";", -1);
    for(int i = 0; tokens[i] != NULL; i++)
    {
      _scan_directory(env, tokens[i]);
    }
    g_strfreev(tokens);
  }

  // Scan darktable's own config dir (respects --configdir).
  // On Linux: ~/.config/darktable/models
  // On Windows: %APPDATA%\darktable\models
  char configdir[PATH_MAX] = {0};
  dt_loc_get_user_config_dir(configdir, sizeof(configdir));
  if(configdir[0])
  {
    char *p = g_build_filename(configdir, "models", NULL);
    _scan_directory(env, p);
    g_free(p);
  }
}

// --- API Implementation ---

dt_ai_environment_t *dt_ai_env_init(const char *search_paths)
{
  dt_print(DT_DEBUG_AI, "[darktable_ai] dt_ai_env_init start.");

  dt_ai_environment_t *env = g_new0(dt_ai_environment_t, 1);
  g_mutex_init(&env->lock);
  env->model_paths = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  env->search_paths = g_strdup(search_paths);

  // Read user's preferred execution provider from config
  char *prov_str = dt_conf_get_string(DT_AI_CONF_PROVIDER);
  env->provider = dt_ai_provider_from_string(prov_str);
  g_free(prov_str);

  _scan_all_paths(env);
  env->models = g_list_reverse(env->models);

  return env;
}

int dt_ai_get_model_count(dt_ai_environment_t *env)
{
  if(!env)
    return 0;
  return g_list_length(env->models);
}

const dt_ai_model_info_t *
dt_ai_get_model_info_by_index(dt_ai_environment_t *env, int index)
{
  if(!env)
    return NULL;
  GList *item = g_list_nth(env->models, index);
  if(!item)
    return NULL;
  return (const dt_ai_model_info_t *)item->data;
}

const dt_ai_model_info_t *
dt_ai_get_model_info_by_id(dt_ai_environment_t *env, const char *id)
{
  if(!env || !id)
    return NULL;
  for(GList *l = env->models; l != NULL; l = l->next)
  {
    dt_ai_model_info_t *info = (dt_ai_model_info_t *)l->data;
    if(strcmp(info->id, id) == 0)
      return info;
  }
  return NULL;
}

static void _free_model_info(gpointer data) { g_free(data); }

void dt_ai_env_refresh(dt_ai_environment_t *env)
{
  if(!env)
    return;

  g_mutex_lock(&env->lock);

  dt_print(DT_DEBUG_AI, "[darktable_ai] Refreshing model list");

  // Clear existing data
  g_list_free_full(env->models, _free_model_info);
  env->models = NULL;

  g_list_free_full(env->string_storage, g_free);
  env->string_storage = NULL;

  g_hash_table_remove_all(env->model_paths);

  _scan_all_paths(env);

  dt_print(
    DT_DEBUG_AI,
    "[darktable_ai] Refresh complete, found %d models",
    g_list_length(env->models));

  g_mutex_unlock(&env->lock);
}

void dt_ai_env_destroy(dt_ai_environment_t *env)
{
  if(!env)
    return;

  g_list_free_full(env->models, _free_model_info);
  g_list_free_full(env->string_storage, g_free);
  g_hash_table_destroy(env->model_paths);
  g_free(env->search_paths);
  g_mutex_clear(&env->lock);

  g_free(env);
}

void dt_ai_env_set_provider(dt_ai_environment_t *env, dt_ai_provider_t provider)
{
  if(!env)
    return;
  g_mutex_lock(&env->lock);
  env->provider = provider;
  g_mutex_unlock(&env->lock);
}

dt_ai_provider_t dt_ai_env_get_provider(dt_ai_environment_t *env)
{
  if(!env)
    return DT_AI_PROVIDER_AUTO;
  g_mutex_lock(&env->lock);
  const dt_ai_provider_t p = env->provider;
  g_mutex_unlock(&env->lock);
  return p;
}

// --- Backend-specific load (defined in backend_onnx.c) ---

extern dt_ai_context_t *
dt_ai_onnx_load_ext(const char *model_dir, const char *model_file,
                    dt_ai_provider_t provider, dt_ai_opt_level_t opt_level,
                    const dt_ai_dim_override_t *dim_overrides, int n_overrides);

// --- Model Loading with Backend Dispatch ---

dt_ai_context_t *dt_ai_load_model(
  dt_ai_environment_t *env,
  const char *model_id,
  const char *model_file,
  dt_ai_provider_t provider)
{
  return dt_ai_load_model_ext(env, model_id, model_file, provider,
                               DT_AI_OPT_ALL, NULL, 0);
}

dt_ai_context_t *dt_ai_load_model_ext(
  dt_ai_environment_t *env,
  const char *model_id,
  const char *model_file,
  dt_ai_provider_t provider,
  dt_ai_opt_level_t opt_level,
  const dt_ai_dim_override_t *dim_overrides,
  int n_overrides)
{
  if(!env || !model_id)
    return NULL;

  g_mutex_lock(&env->lock);

  // Resolve AUTO to environment-level provider preference (under lock)
  if(provider == DT_AI_PROVIDER_AUTO)
    provider = env->provider;
  const char *model_dir_orig
    = (const char *)g_hash_table_lookup(env->model_paths, model_id);
  char *model_dir = model_dir_orig ? g_strdup(model_dir_orig) : NULL;

  const char *backend = "onnx";
  for(GList *l = env->models; l != NULL; l = l->next)
  {
    dt_ai_model_info_t *info = (dt_ai_model_info_t *)l->data;
    if(strcmp(info->id, model_id) == 0)
    {
      backend = info->backend;
      break;
    }
  }
  char *backend_copy = g_strdup(backend);
  g_mutex_unlock(&env->lock);

  if(!model_dir)
  {
    dt_print(DT_DEBUG_AI, "[darktable_ai] ID not found: %s", model_id);
    g_free(backend_copy);
    return NULL;
  }

  dt_ai_context_t *ctx = NULL;

  if(strcmp(backend_copy, "onnx") == 0)
  {
    ctx = dt_ai_onnx_load_ext(model_dir, model_file, provider, opt_level,
                               dim_overrides, n_overrides);
  }
  else
  {
    dt_print(
      DT_DEBUG_AI,
      "[darktable_ai] Unknown backend '%s' for model '%s'",
      backend_copy,
      model_id);
  }

  g_free(model_dir);
  g_free(backend_copy);
  return ctx;
}

// --- Provider String Conversion ---

const char *dt_ai_provider_to_string(dt_ai_provider_t provider)
{
  for(int i = 0; i < DT_AI_PROVIDER_COUNT; i++)
  {
    if(dt_ai_providers[i].value == provider)
      return dt_ai_providers[i].display_name;
  }
  return dt_ai_providers[0].display_name;  // fallback to "auto"
}

dt_ai_provider_t dt_ai_provider_from_string(const char *str)
{
  if(!str)
    return DT_AI_PROVIDER_AUTO;

  // Match against config_string (primary) and display_name
  for(int i = 0; i < DT_AI_PROVIDER_COUNT; i++)
  {
    if(g_ascii_strcasecmp(str, dt_ai_providers[i].config_string) == 0)
      return dt_ai_providers[i].value;
    if(g_ascii_strcasecmp(str, dt_ai_providers[i].display_name) == 0)
      return dt_ai_providers[i].value;
  }

  // Legacy alias: ROCm was renamed to MIGraphX
  if(g_ascii_strcasecmp(str, "ROCm") == 0)
    return DT_AI_PROVIDER_MIGRAPHX;

  return DT_AI_PROVIDER_AUTO;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
