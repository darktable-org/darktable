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
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <limits.h>
#include <string.h>

// provider table

// clang-format off
const dt_ai_provider_desc_t dt_ai_providers[DT_AI_PROVIDER_COUNT] = {
  { DT_AI_PROVIDER_AUTO,     "auto",     "auto",
    1 },
  { DT_AI_PROVIDER_CPU,      "CPU",      "CPU",
    1 },
  { DT_AI_PROVIDER_COREML,   "CoreML",   "Apple CoreML",
#if defined(__APPLE__)
    1
#else
    0
#endif
  },
  { DT_AI_PROVIDER_CUDA,     "CUDA",     "NVIDIA CUDA",
#if defined(__linux__) || defined(_WIN32)
    1
#else
    0
#endif
  },
  { DT_AI_PROVIDER_MIGRAPHX, "MIGraphX", "AMD MIGraphX",
#if defined(__linux__)
    1
#else
    0
#endif
  },
  { DT_AI_PROVIDER_OPENVINO, "OpenVINO", "Intel OpenVINO",
#if defined(__linux__) || defined(_WIN32) || (defined(__APPLE__) && defined(__x86_64__))
    1
#else
    0
#endif
  },
  { DT_AI_PROVIDER_DIRECTML, "DirectML", "Windows DirectML",
#if defined(_WIN32)
    1
#else
    0
#endif
  },
};
// clang-format on

struct dt_ai_environment_t
{
  GList *models;           // list of dt_ai_model_info_t*
  GHashTable *model_paths; // id -> path (string)

  // to keep pointers in dt_ai_model_info_t valid
  GList *string_storage; // list of char*

  // remembered for refresh
  char *search_paths;

  // default execution provider (read from config at init, override with dt_ai_env_set_provider)
  dt_ai_provider_t provider; // DT_AI_PROVIDER_AUTO = platform auto-detect

  GMutex lock; // thread safety for model list access
};

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
            // skip duplicate model IDs (first discovered wins)
            if(g_hash_table_contains(env->model_paths, id))
            {
              dt_print(DT_DEBUG_AI, "[darktable_ai] skipping duplicate model ID: %s", id);
            }
            else
            {
              dt_ai_model_info_t *info = g_new0(dt_ai_model_info_t, 1);
              _store_string(env, id, &info->id);
              _store_string(env, name, &info->name);
              _store_string(env, desc, &info->description);
              _store_string(env, task, &info->task_type);

              const char *arch = json_object_has_member(obj, "arch")
                ? json_object_get_string_member(obj, "arch")
                : "";
              _store_string(env, arch, &info->arch);
              _store_string(env, backend, &info->backend);
              info->num_inputs = json_object_has_member(obj, "num_inputs")
                ? (int)json_object_get_int_member(obj, "num_inputs")
                : 1;

              // capture optional "attributes" object as a JSON string;
              // accessors (e.g. dt_ai_model_attribute_bool) parse on demand
              info->attributes = NULL;
              if(json_object_has_member(obj, "attributes"))
              {
                JsonNode *attr_node = json_object_get_member(obj, "attributes");
                if(attr_node && JSON_NODE_HOLDS_OBJECT(attr_node))
                {
                  JsonGenerator *gen = json_generator_new();
                  json_generator_set_root(gen, attr_node);
                  gchar *s = json_generator_to_data(gen, NULL);
                  if(s)
                  {
                    _store_string(env, s, &info->attributes);
                    g_free(s);
                  }
                  g_object_unref(gen);
                }
              }

              // capture top-level "cpu_only" (sibling to "attributes")
              // as a JSON string; can be either an array (applies to all
              // models in the package) or an object (keyed by onnx filename)
              info->cpu_only = NULL;
              if(json_object_has_member(obj, "cpu_only"))
              {
                JsonNode *co_node = json_object_get_member(obj, "cpu_only");
                if(co_node
                   && (JSON_NODE_HOLDS_ARRAY(co_node)
                       || JSON_NODE_HOLDS_OBJECT(co_node)))
                {
                  JsonGenerator *gen = json_generator_new();
                  json_generator_set_root(gen, co_node);
                  gchar *s = json_generator_to_data(gen, NULL);
                  if(s)
                  {
                    _store_string(env, s, &info->cpu_only);
                    g_free(s);
                  }
                  g_object_unref(gen);
                }
              }

              // capture top-level "coreml_format" (string or
              // stem-keyed object) as a JSON string
              info->coreml_format = NULL;
              if(json_object_has_member(obj, "coreml_format"))
              {
                JsonNode *cf_node
                  = json_object_get_member(obj, "coreml_format");
                if(cf_node
                   && (JSON_NODE_HOLDS_VALUE(cf_node)
                       || JSON_NODE_HOLDS_OBJECT(cf_node)))
                {
                  JsonGenerator *gen = json_generator_new();
                  json_generator_set_root(gen, cf_node);
                  gchar *s = json_generator_to_data(gen, NULL);
                  if(s)
                  {
                    _store_string(env, s, &info->coreml_format);
                    g_free(s);
                  }
                  g_object_unref(gen);
                }
              }

              env->models = g_list_prepend(env->models, info);
              g_hash_table_insert(
                env->model_paths,
                g_strdup(info->id),
                g_strdup(full_path));

              dt_print(DT_DEBUG_AI,
                       "[darktable_ai] discovered: %s (%s, backend=%s)",
                       name, id, backend);
            }
          }
        }
        else
        {
          dt_print(DT_DEBUG_AI, "[darktable_ai] parse error: %s", error->message);
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

// scan search_paths + XDG data dir fallback. duplicate IDs are
// rejected by _scan_directory (first-seen wins)
static void _scan_all_paths(dt_ai_environment_t *env)
{
  if(env->search_paths && env->search_paths[0])
  {
    char **tokens = g_strsplit(env->search_paths, ";", -1);
    for(int i = 0; tokens[i] != NULL; i++)
      _scan_directory(env, tokens[i]);
    g_strfreev(tokens);
  }

  char *datadir = g_build_filename(g_get_user_data_dir(), "darktable", "models", NULL);
  _scan_directory(env, datadir);
  g_free(datadir);
}

// API implementation

gchar *dt_ai_resolve_models_path_override(void)
{
  char *override = dt_conf_get_string("plugins/ai/models_path");
  if(!override || !override[0])
  {
    g_free(override);
    return NULL;
  }
  gchar *resolved;
  if(override[0] == '~' && (override[1] == '/' || override[1] == '\0'))
    resolved = g_build_filename(g_get_home_dir(), override + 2, NULL);
  else
    resolved = g_strdup(override);
  g_free(override);
  return resolved;
}

dt_ai_environment_t *dt_ai_env_init(const char *search_paths)
{
  if(!dt_conf_get_bool("plugins/ai/enabled"))
  {
    dt_print(DT_DEBUG_AI,
             "[darktable_ai] AI subsystem is disabled");
    return NULL;
  }

  dt_print(DT_DEBUG_AI, "[darktable_ai] dt_ai_env_init start.");

  // honor plugins/ai/models_path when no explicit paths are given
  gchar *resolved_paths = NULL;
  if(!search_paths)
  {
    resolved_paths = dt_ai_resolve_models_path_override();
    if(resolved_paths) search_paths = resolved_paths;
  }

  dt_ai_environment_t *env = g_new0(dt_ai_environment_t, 1);
  g_mutex_init(&env->lock);
  env->model_paths = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  env->search_paths = g_strdup(search_paths);
  g_free(resolved_paths);

  // read user's preferred execution provider from config
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
  g_mutex_lock(&env->lock);
  int count = g_list_length(env->models);
  g_mutex_unlock(&env->lock);
  return count;
}

const dt_ai_model_info_t *
dt_ai_get_model_info_by_index(dt_ai_environment_t *env, int index)
{
  if(!env)
    return NULL;
  g_mutex_lock(&env->lock);
  GList *item = g_list_nth(env->models, index);
  const dt_ai_model_info_t *info = item ? (const dt_ai_model_info_t *)item->data : NULL;
  g_mutex_unlock(&env->lock);
  return info;
}

const dt_ai_model_info_t *
dt_ai_get_model_info_by_id(dt_ai_environment_t *env, const char *id)
{
  if(!env || !id)
    return NULL;
  g_mutex_lock(&env->lock);
  const dt_ai_model_info_t *result = NULL;
  for(GList *l = env->models; l != NULL; l = l->next)
  {
    dt_ai_model_info_t *info = (dt_ai_model_info_t *)l->data;
    if(strcmp(info->id, id) == 0)
    {
      result = info;
      break;
    }
  }
  g_mutex_unlock(&env->lock);
  return result;
}

static void _free_model_info(gpointer data) { g_free(data); }

void dt_ai_env_refresh(dt_ai_environment_t *env)
{
  if(!env)
    return;

  g_mutex_lock(&env->lock);

  dt_print(DT_DEBUG_AI, "[darktable_ai] refreshing model list");

  // clear existing data
  g_list_free_full(env->models, _free_model_info);
  env->models = NULL;

  g_list_free_full(env->string_storage, g_free);
  env->string_storage = NULL;

  g_hash_table_remove_all(env->model_paths);

  _scan_all_paths(env);

  dt_print(DT_DEBUG_AI,
           "[darktable_ai] refresh complete, found %d models",
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

// =backend-specific load (defined in backend_onnx.c)

extern dt_ai_context_t *
dt_ai_onnx_load_ext(const char *model_dir, const char *model_file,
                    dt_ai_provider_t provider, dt_ai_opt_level_t opt_level,
                    const dt_ai_dim_override_t *dim_overrides, int n_overrides,
                    uint32_t ep_flags);

static gboolean _provider_cpu_only(const dt_ai_model_info_t *info,
                                   const char *model_file,
                                   dt_ai_provider_t configured);

static dt_ai_provider_t _resolve_provider(dt_ai_provider_t configured,
                                          const char *model_id,
                                          const dt_ai_model_info_t *info,
                                          const char *model_file,
                                          uint32_t *inout_ep_flags);

// model loading with backend dispatch

dt_ai_context_t *dt_ai_load_model(dt_ai_environment_t *env,
                                  const char *model_id,
                                  const char *model_file,
                                  dt_ai_provider_t provider)
{
  return dt_ai_load_model_ext(env, model_id, model_file, provider,
                              DT_AI_OPT_ALL, NULL, 0);
}

dt_ai_context_t *dt_ai_load_model_ext(dt_ai_environment_t *env,
                                      const char *model_id,
                                      const char *model_file,
                                      dt_ai_provider_t provider,
                                      dt_ai_opt_level_t opt_level,
                                      const dt_ai_dim_override_t *dim_overrides,
                                      int n_overrides)
{
  if(!env || !model_id)
    return NULL;

  if(!dt_conf_get_bool("plugins/ai/enabled"))
  {
    dt_print(DT_DEBUG_AI,
             "[darktable_ai] AI subsystem is disabled");
    return NULL;
  }

  // resolve CONFIGURED/AUTO to a concrete EP and apply the model's
  // cpu_only safety constraint in one place. ep_flags is owned by the
  // load function (e.g. COREML_FLAG_USE_CPU_ONLY) so the EP can stay
  // enabled while honoring the constraint
  uint32_t ep_flags = 0;
  const dt_ai_model_info_t *model_info
    = dt_ai_get_model_info_by_id(env, model_id);
  provider = _resolve_provider(provider, model_id, model_info,
                               model_file, &ep_flags);

  g_mutex_lock(&env->lock);
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
                               dim_overrides, n_overrides, ep_flags);
  }
  else
  {
    dt_print(DT_DEBUG_AI,
             "[darktable_ai] unknown backend '%s' for model '%s'",
             backend_copy, model_id);
  }

  g_free(model_dir);
  g_free(backend_copy);
  return ctx;
}

// model attribute lookup — parses the JSON-encoded attributes string
// on demand; callers pass info from dt_ai_get_model_info_by_id()
//
// _attribute_node returns the parsed JsonParser plus a borrowed JsonNode*
// for the named key; caller must g_object_unref the returned parser;
// returns NULL parser if the attribute set is absent or the key is missing
//
// the key accepts a dotted path ("variants.bayer.onnx"): each segment
// except the last must resolve to a JSON object; the final segment is
// the leaf lookup and may hold any JSON value type
static JsonParser *_attribute_node(const dt_ai_model_info_t *info,
                                   const char *key,
                                   JsonNode **out_node)
{
  *out_node = NULL;
  if(!info || !info->attributes || !key) return NULL;
  JsonParser *parser = json_parser_new();
  if(!json_parser_load_from_data(parser, info->attributes, -1, NULL))
  {
    g_object_unref(parser);
    return NULL;
  }
  JsonNode *root = json_parser_get_root(parser);
  if(!root || !JSON_NODE_HOLDS_OBJECT(root))
  {
    g_object_unref(parser);
    return NULL;
  }
  JsonObject *obj = json_node_get_object(root);
  gchar **segments = g_strsplit(key, ".", -1);
  const int n = g_strv_length(segments);
  JsonNode *node = NULL;
  for(int i = 0; i < n; i++)
  {
    if(!json_object_has_member(obj, segments[i])) goto out;
    node = json_object_get_member(obj, segments[i]);
    if(i == n - 1) break;
    // intermediate segments must be objects to descend further
    if(!node || !JSON_NODE_HOLDS_OBJECT(node)) { node = NULL; goto out; }
    obj = json_node_get_object(node);
  }
out:
  g_strfreev(segments);
  if(!node)
  {
    g_object_unref(parser);
    return NULL;
  }
  *out_node = node;
  return parser;
}

gboolean dt_ai_model_attribute_bool(const dt_ai_model_info_t *info,
                                    const char *key)
{
  JsonNode *v = NULL;
  JsonParser *p = _attribute_node(info, key, &v);
  gboolean result = FALSE;
  if(v && JSON_NODE_HOLDS_VALUE(v))
    result = json_node_get_boolean(v);
  if(p) g_object_unref(p);
  return result;
}

int dt_ai_model_attribute_int(const dt_ai_model_info_t *info,
                              const char *key,
                              int default_value)
{
  JsonNode *v = NULL;
  JsonParser *p = _attribute_node(info, key, &v);
  int result = default_value;
  if(v && JSON_NODE_HOLDS_VALUE(v))
    result = (int)json_node_get_int(v);
  if(p) g_object_unref(p);
  return result;
}

double dt_ai_model_attribute_double(const dt_ai_model_info_t *info,
                                    const char *key,
                                    double default_value)
{
  JsonNode *v = NULL;
  JsonParser *p = _attribute_node(info, key, &v);
  double result = default_value;
  if(v && JSON_NODE_HOLDS_VALUE(v))
    result = json_node_get_double(v);
  if(p) g_object_unref(p);
  return result;
}

char *dt_ai_model_attribute_string(const dt_ai_model_info_t *info,
                                   const char *key)
{
  JsonNode *v = NULL;
  JsonParser *p = _attribute_node(info, key, &v);
  char *result = NULL;
  if(v && JSON_NODE_HOLDS_VALUE(v))
  {
    const char *s = json_node_get_string(v);
    if(s) result = g_strdup(s);
  }
  if(p) g_object_unref(p);
  return result;
}

int *dt_ai_model_attribute_int_array(const dt_ai_model_info_t *info,
                                     const char *key,
                                     int *out_count)
{
  if(out_count) *out_count = 0;
  JsonNode *v = NULL;
  JsonParser *p = _attribute_node(info, key, &v);
  int *result = NULL;
  if(v && JSON_NODE_HOLDS_ARRAY(v))
  {
    JsonArray *arr = json_node_get_array(v);
    const guint n = json_array_get_length(arr);
    if(n > 0)
    {
      result = g_new(int, n);
      for(guint i = 0; i < n; i++)
        result[i] = (int)json_array_get_int_element(arr, i);
      if(out_count) *out_count = (int)n;
    }
  }
  if(p) g_object_unref(p);
  return result;
}

// resolve the model's "cpu_only" block against a concrete EP. the
// block can take two forms:
//
//   cpu_only: [coreml, directml]        (flat list — applies to all)
//   cpu_only:                           (keyed by onnx filename stem)
//     model_bayer: [coreml, directml]
//     model_linear: []
//
// returns TRUE when `concrete` appears (case-insensitively) in the
// relevant list. callers must pass a concrete provider — AUTO and
// CONFIGURED are resolved upstream by _resolve_provider and never
// reach here. CPU short-circuits to FALSE (it is never restricted)
static gboolean _provider_cpu_only(const dt_ai_model_info_t *info,
                                   const char *model_file,
                                   dt_ai_provider_t concrete)
{
  if(!info || !info->cpu_only || concrete == DT_AI_PROVIDER_CPU)
    return FALSE;

  // map provider enum to its config_string (e.g. "CoreML", "DirectML")
  const char *prov_name = NULL;
  for(int i = 0; i < DT_AI_PROVIDER_COUNT; i++)
    if(dt_ai_providers[i].value == concrete)
    {
      prov_name = dt_ai_providers[i].config_string;
      break;
    }
  if(!prov_name) return FALSE;

  JsonParser *parser = json_parser_new();
  gboolean result = FALSE;
  if(json_parser_load_from_data(parser, info->cpu_only, -1, NULL))
  {
    JsonNode *root = json_parser_get_root(parser);

    // "cpu_only" can be either a flat array (applies to every model in
    // the package) or an object keyed by onnx filename stem — the
    // basename without the ".onnx" extension. dropping the extension
    // keeps the YAML readable (no quoting needed for keys with dots)
    JsonArray *arr = NULL;
    gchar *stem = NULL;
    if(root && JSON_NODE_HOLDS_ARRAY(root))
    {
      arr = json_node_get_array(root);
    }
    else if(root && JSON_NODE_HOLDS_OBJECT(root) && model_file)
    {
      const char *dot = strrchr(model_file, '.');
      stem = dot ? g_strndup(model_file, dot - model_file)
                 : g_strdup(model_file);
      JsonObject *obj = json_node_get_object(root);
      if(json_object_has_member(obj, stem))
      {
        JsonNode *vn = json_object_get_member(obj, stem);
        if(vn && JSON_NODE_HOLDS_ARRAY(vn))
          arr = json_node_get_array(vn);
      }
    }

    if(arr)
    {
      const guint n = json_array_get_length(arr);
      for(guint i = 0; i < n; i++)
      {
        const gchar *s = json_array_get_string_element(arr, i);
        if(s && !g_ascii_strcasecmp(s, prov_name))
        {
          result = TRUE;
          break;
        }
      }
    }
    g_free(stem);
  }
  g_object_unref(parser);
  return result;
}

// TRUE if the manifest opts in to MLProgram via "mlprogram" — flat
// string or stem-keyed object, same shape as cpu_only. unknown values
// and missing blocks default to NeuralNetwork (Apple's historical
// CoreML default, lowest-surprise for untested models)
static gboolean _coreml_use_mlprogram(const dt_ai_model_info_t *info,
                                      const char *model_file)
{
  if(!info || !info->coreml_format) return FALSE;

  JsonParser *parser = json_parser_new();
  gboolean use_mlprogram = FALSE;
  if(json_parser_load_from_data(parser, info->coreml_format, -1, NULL))
  {
    JsonNode *root = json_parser_get_root(parser);
    const char *value = NULL;
    gchar *stem = NULL;

    if(root && JSON_NODE_HOLDS_VALUE(root))
    {
      value = json_node_get_string(root);
    }
    else if(root && JSON_NODE_HOLDS_OBJECT(root) && model_file)
    {
      const char *dot = strrchr(model_file, '.');
      stem = dot ? g_strndup(model_file, dot - model_file)
                 : g_strdup(model_file);
      JsonObject *obj = json_node_get_object(root);
      if(json_object_has_member(obj, stem))
      {
        JsonNode *vn = json_object_get_member(obj, stem);
        if(vn && JSON_NODE_HOLDS_VALUE(vn))
          value = json_node_get_string(vn);
      }
    }

    if(value && !g_ascii_strcasecmp(value, "mlprogram"))
      use_mlprogram = TRUE;
    g_free(stem);
  }
  g_object_unref(parser);
  return use_mlprogram;
}

// platform-specific candidate order for AUTO. each candidate is tried
// via dt_ai_probe_provider() and the model's cpu_only list — the first
// available, non-banned EP wins. CoreML's cpu_only entry does not skip
// the candidate but pins it to CPU compute units via USE_CPU_ONLY. on
// linux multiple GPU EPs may coexist, so a model that bans one still
// gets GPU acceleration via the next. falls through to CPU when every
// candidate is unavailable or banned
static const dt_ai_provider_t *_auto_candidates(int *n_out)
{
#if defined(__APPLE__)
  static const dt_ai_provider_t list[] = { DT_AI_PROVIDER_COREML };
#elif defined(_WIN32)
  static const dt_ai_provider_t list[] = { DT_AI_PROVIDER_DIRECTML };
#elif defined(__linux__)
  static const dt_ai_provider_t list[] = { DT_AI_PROVIDER_CUDA,
                                           DT_AI_PROVIDER_MIGRAPHX,
                                           DT_AI_PROVIDER_OPENVINO };
#else
  static const dt_ai_provider_t list[1] = { DT_AI_PROVIDER_CPU };
  *n_out = 0;
  return list;
#endif
  *n_out = (int)(sizeof(list) / sizeof(list[0]));
  return list;
}

static const char *_provider_config_name(dt_ai_provider_t p)
{
  for(int i = 0; i < DT_AI_PROVIDER_COUNT; i++)
    if(dt_ai_providers[i].value == p) return dt_ai_providers[i].config_string;
  return "?";
}

// resolve a configured provider value to the concrete EP that will be
// loaded. handles three transformations in one place:
//
//   1. CONFIGURED -> reads plugins/ai/provider, then recurses.
//   2. AUTO       -> probes the platform candidate list; for each
//                    candidate, applies cpu_only. first candidate that
//                    is available and either not banned, or (CoreML
//                    only) tolerable via USE_CPU_ONLY, wins. otherwise
//                    falls through to CPU.
//   3. concrete   -> applies cpu_only. CoreML stays with USE_CPU_ONLY,
//                    any other banned EP demotes to CPU.
//
// updates *inout_ep_flags (sets COREML_FLAG_USE_CPU_ONLY where the
// constraint dictates). guaranteed to return a concrete provider —
// never AUTO or CONFIGURED — so _enable_acceleration in the backend
// has no AUTO case to handle
static dt_ai_provider_t _resolve_provider(dt_ai_provider_t configured,
                                          const char *model_id,
                                          const dt_ai_model_info_t *info,
                                          const char *model_file,
                                          uint32_t *inout_ep_flags)
{
  if(configured == DT_AI_PROVIDER_CONFIGURED)
  {
    char *prov_str = dt_conf_get_string(DT_AI_CONF_PROVIDER);
    configured = dt_ai_provider_from_string(prov_str);
    g_free(prov_str);
  }

  if(configured == DT_AI_PROVIDER_CPU) return DT_AI_PROVIDER_CPU;

  const char *id = model_id ? model_id : "?";
  dt_ai_provider_t result = DT_AI_PROVIDER_CPU;

  if(configured == DT_AI_PROVIDER_AUTO)
  {
    int n = 0;
    const dt_ai_provider_t *cands = _auto_candidates(&n);
    // probe every candidate — even single-candidate platforms (macOS,
    // Windows). assuming the bundled EP is present breaks when a user
    // points plugins/ai/ort_library_path at a non-bundled or CPU-only
    // ORT. dt_ai_probe_provider memoizes its result, so the cost is
    // a single _try_provider call per EP per process
    gboolean resolved = FALSE;
    for(int i = 0; i < n && !resolved; i++)
    {
      const dt_ai_provider_t c = cands[i];
      if(!dt_ai_probe_provider(c)) continue;
      const gboolean banned = _provider_cpu_only(info, model_file, c);
      if(!banned)
      {
        dt_print(DT_DEBUG_AI,
                 "[darktable_ai] provider auto -> %s for %s",
                 _provider_config_name(c), id);
        result = c;
        resolved = TRUE;
      }
      else if(c == DT_AI_PROVIDER_COREML)
      {
        *inout_ep_flags |= 1;  // COREML_FLAG_USE_CPU_ONLY
        dt_print(DT_DEBUG_AI,
                 "[darktable_ai] provider auto -> %s (cpu compute units) "
                 "for %s — model prefers CPU on %s",
                 _provider_config_name(c), id, _provider_config_name(c));
        result = c;
        resolved = TRUE;
      }
      else
      {
        dt_print(DT_DEBUG_AI,
                 "[darktable_ai] auto: skipping %s for %s — model prefers CPU",
                 _provider_config_name(c), id);
      }
    }
    if(!resolved)
      dt_print(DT_DEBUG_AI, "[darktable_ai] provider auto -> CPU for %s", id);
  }
  else if(_provider_cpu_only(info, model_file, configured))
  {
    // concrete EP banned by cpu_only
    if(configured == DT_AI_PROVIDER_COREML)
    {
      *inout_ep_flags |= 1;  // COREML_FLAG_USE_CPU_ONLY
      dt_print(DT_DEBUG_AI,
               "[darktable_ai] %s prefers CPU on %s — using CoreML CPU "
               "compute units",
               id, _provider_config_name(configured));
      result = configured;
    }
    else
    {
      dt_print(DT_DEBUG_AI,
               "[darktable_ai] %s prefers CPU on %s — switching to CPU provider",
               id, _provider_config_name(configured));
    }
  }
  else
  {
    result = configured;
  }

  if(result == DT_AI_PROVIDER_COREML
     && _coreml_use_mlprogram(info, model_file))
    *inout_ep_flags |= 16;  // COREML_FLAG_CREATE_MLPROGRAM

  return result;
}

// provider string conversion

const char *dt_ai_provider_to_string(dt_ai_provider_t provider)
{
  for(int i = 0; i < DT_AI_PROVIDER_COUNT; i++)
  {
    if(dt_ai_providers[i].value == provider)
      return dt_ai_providers[i].display_name;
  }
  return dt_ai_providers[0].display_name;  // fallback to "auto"
}

size_t dt_ai_tensor_element_count(const int64_t *shape, int ndim)
{
  if(!shape || ndim <= 0) return 0;
  size_t n = 1;
  for(int i = 0; i < ndim; i++)
  {
    if(shape[i] <= 0) return 0;
    n *= (size_t)shape[i];
  }
  return n;
}

dt_ai_provider_t dt_ai_provider_from_string(const char *str)
{
  if(!str)
    return DT_AI_PROVIDER_AUTO;

  // match against config_string (primary) and display_name
  for(int i = 0; i < DT_AI_PROVIDER_COUNT; i++)
  {
    if(g_ascii_strcasecmp(str, dt_ai_providers[i].config_string) == 0)
      return dt_ai_providers[i].value;
    if(g_ascii_strcasecmp(str, dt_ai_providers[i].display_name) == 0)
      return dt_ai_providers[i].value;
  }

  // legacy alias: ROCm was renamed to MIGraphX
  if(g_ascii_strcasecmp(str, "ROCm") == 0)
    return DT_AI_PROVIDER_MIGRAPHX;

  return DT_AI_PROVIDER_AUTO;
}

// parse the comma-separated EP list from dt_ai_ort_probe_library_full().
// AUTO and CPU are always set — they work regardless of what the library
// advertises; unknown tokens (e.g. "ROCm", "TensorRT") are ignored
guint dt_ai_providers_from_eps(const char *eps)
{
  guint mask = (1u << DT_AI_PROVIDER_AUTO) | (1u << DT_AI_PROVIDER_CPU);
  if(!eps) return mask;

  gchar **tokens = g_strsplit(eps, ",", -1);
  for(int i = 0; tokens[i]; i++)
  {
    const dt_ai_provider_t p = dt_ai_provider_from_string(g_strstrip(tokens[i]));
    if(p != DT_AI_PROVIDER_AUTO)  // from_string returns AUTO on unknown
      mask |= 1u << p;
  }
  g_strfreev(tokens);
  return mask;
}

guint dt_ai_providers_bundled(void)
{
  guint mask = (1u << DT_AI_PROVIDER_AUTO) | (1u << DT_AI_PROVIDER_CPU);
#if defined(__APPLE__)
  mask |= 1u << DT_AI_PROVIDER_COREML;
#elif defined(_WIN32)
  mask |= 1u << DT_AI_PROVIDER_DIRECTML;
#endif
  return mask;
}

// compile-cache helpers

static const char *_ep_name(dt_ai_provider_t provider)
{
  switch(provider)
  {
    case DT_AI_PROVIDER_COREML:    return "coreml";
    case DT_AI_PROVIDER_CUDA:      return "cuda";
    case DT_AI_PROVIDER_MIGRAPHX:  return "migraphx";
    case DT_AI_PROVIDER_OPENVINO:  return "openvino";
    case DT_AI_PROVIDER_DIRECTML:  return "directml";
    default:                       return NULL;
  }
}

static void _cache_rmdir_recursive(const char *path)
{
  GDir *dir = g_dir_open(path, 0, NULL);
  if(!dir) return;

  const gchar *name;
  while((name = g_dir_read_name(dir)))
  {
    gchar *child = g_build_filename(path, name, NULL);
    if(g_file_test(child, G_FILE_TEST_IS_DIR))
      _cache_rmdir_recursive(child);
    else
      g_unlink(child);
    g_free(child);
  }
  g_dir_close(dir);
  g_rmdir(path);
}

gboolean dt_ai_backend_cache_dir(dt_ai_provider_t provider,
                                 const char *fingerprint,
                                 const char *model_id,
                                 char *out, size_t size)
{
  if(!out || size == 0) return FALSE;
  const char *ep = _ep_name(provider);
  if(!ep || !fingerprint || !model_id || !model_id[0]) return FALSE;

  char cachedir[PATH_MAX] = { 0 };
  dt_loc_get_user_cache_dir(cachedir, sizeof(cachedir));

  gchar *subdir = g_strdup_printf("ai_v%d_%s_%s",
                                  DT_AI_CACHE_SCHEMA, ep, fingerprint);
  gchar *full = g_build_filename(cachedir, subdir, model_id, NULL);
  g_free(subdir);

  const size_t len = strlen(full);
  if(len + 1 > size)
  {
    g_free(full);
    return FALSE;
  }
  if(g_mkdir_with_parents(full, 0700) == -1)
  {
    dt_print(DT_DEBUG_AI,
             "[ai_cache] failed to create cache directory: %s", full);
    g_free(full);
    return FALSE;
  }
  memcpy(out, full, len + 1);
  g_free(full);
  return TRUE;
}

void dt_ai_backend_cache_invalidate(const char *model_id)
{
  if(!model_id || !model_id[0]) return;

  char cachedir[PATH_MAX] = { 0 };
  dt_loc_get_user_cache_dir(cachedir, sizeof(cachedir));

  GDir *dir = g_dir_open(cachedir, 0, NULL);
  if(!dir) return;

  // match every "ai_v<N>_<ep>_<fingerprint>" sibling and walk into
  // <model_id>, catching old schemas/EPs too
  const gchar *name;
  while((name = g_dir_read_name(dir)))
  {
    if(!g_str_has_prefix(name, "ai_v")) continue;
    gchar *model_subdir = g_build_filename(cachedir, name, model_id, NULL);
    if(g_file_test(model_subdir, G_FILE_TEST_IS_DIR))
    {
      dt_print(DT_DEBUG_AI, "[ai_cache] invalidating %s", model_subdir);
      _cache_rmdir_recursive(model_subdir);
    }
    g_free(model_subdir);
  }
  g_dir_close(dir);
}

void dt_ai_backend_cache_invalidate_all(void)
{
  char cachedir[PATH_MAX] = { 0 };
  dt_loc_get_user_cache_dir(cachedir, sizeof(cachedir));

  GDir *dir = g_dir_open(cachedir, 0, NULL);
  if(!dir) return;

  const gchar *name;
  while((name = g_dir_read_name(dir)))
  {
    if(!g_str_has_prefix(name, "ai_v")) continue;
    gchar *full = g_build_filename(cachedir, name, NULL);
    if(g_file_test(full, G_FILE_TEST_IS_DIR))
    {
      dt_print(DT_DEBUG_AI, "[ai_cache] invalidating %s", full);
      _cache_rmdir_recursive(full);
    }
    g_free(full);
  }
  g_dir_close(dir);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
