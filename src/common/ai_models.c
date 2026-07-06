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

#include "common/ai_models.h"
#include "ai/backend.h"
#include "common/darktable.h"
#include "common/curl_tools.h"
#include "common/file_location.h"
#include "control/control.h"
#include "control/jobs.h"

#include <archive.h>
#include <archive_entry.h>
#include <curl/curl.h>
#include <json-glib/json-glib.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
// windows doesn't have realpath, use _fullpath instead
static inline char *realpath(const char *path, char *resolved_path)
{
  (void)resolved_path; // ignored, always allocate
  return _fullpath(NULL, path, _MAX_PATH);
}
#endif

// internal layout of the registry singleton (darktable.ai_registry);
// external callers go through the accessors in ai_models.h — the lock
// and the model list are intentionally hidden so nobody can
// race-iterate or deadlock on them
typedef struct dt_ai_registry_t dt_ai_registry_t;
struct dt_ai_registry_t
{
  GList *models;              // list of dt_ai_model_t*
  char *repository;           // github repository (e.g. "darktable-org/darktable-ai")
  char *models_dir;           // path to user's models directory
  char *cache_dir;            // path to download cache directory
  gboolean ai_enabled;        // global AI enable/disable
  dt_ai_provider_t provider;  // selected execution provider
  gboolean updates_checked;   // TRUE after first check_updates call
  struct dt_ai_environment_t *env;  // lazily created backend environment
  GMutex lock;                // thread safety for registry access
};

// config keys
#define CONF_AI_ENABLED "plugins/ai/enabled"
#define CONF_AI_REPOSITORY "plugins/ai/repository"
#define CONF_MODEL_ENABLED_PREFIX "plugins/ai/models/"
#define CONF_ACTIVE_MODEL_PREFIX "plugins/ai/models/active/"

// compare version strings "X.Y", returns -1 if a<b, 0 if a==b, 1 if a>b
static int _version_compare(const char *a, const char *b)
{
  int ax = 0, ay = 0, bx = 0, by = 0;
  if(a) sscanf(a, "%d.%d", &ax, &ay);
  if(b) sscanf(b, "%d.%d", &bx, &by);
  if(ax != bx) return ax < bx ? -1 : 1;
  if(ay != by) return ay < by ? -1 : 1;
  return 0;
}

static void _model_free(dt_ai_model_t *model)
{
  if(!model)
    return;
  g_free(model->id);
  g_free(model->name);
  g_free(model->description);
  g_free(model->task);
  g_free(model->github_asset);
  g_free(model->checksum);
  g_free(model->version);
  g_free(model->min_version);
  g_free(model->spatial_dim_h);
  g_free(model->spatial_dim_w);
  g_free(model);
}

static dt_ai_model_t *_model_copy(const dt_ai_model_t *src)
{
  if(!src)
    return NULL;
  dt_ai_model_t *copy = g_new0(dt_ai_model_t, 1);
  copy->id = g_strdup(src->id);
  copy->name = g_strdup(src->name);
  copy->description = g_strdup(src->description);
  copy->task = g_strdup(src->task);
  copy->github_asset = g_strdup(src->github_asset);
  copy->checksum = g_strdup(src->checksum);
  copy->version = g_strdup(src->version);
  copy->min_version = g_strdup(src->min_version);
  copy->is_default = src->is_default;
  copy->enabled = src->enabled;
  copy->status = src->status;
  copy->download_progress = src->download_progress;
  return copy;
}

void dt_ai_model_free(dt_ai_model_t *model) { _model_free(model); }

static dt_ai_model_t *_model_new(void)
{
  dt_ai_model_t *model = g_new0(dt_ai_model_t, 1);
  model->enabled = TRUE;
  model->status = DT_AI_MODEL_NOT_DOWNLOADED;
  return model;
}

static gboolean _ensure_directory(const char *path)
{
  if(g_file_test(path, G_FILE_TEST_IS_DIR))
    return TRUE;
  return g_mkdir_with_parents(path, 0755) == 0;
}

#ifdef HAVE_AI_DOWNLOAD
// curl write callback that appends to a GString (capped at 1 MB)
static size_t _curl_write_string(void *ptr, size_t size, size_t nmemb, void *userdata)
{
  GString *buf = (GString *)userdata;
  const size_t bytes = size * nmemb;
  if(buf->len + bytes > 1024 * 1024) return 0;  // abort transfer
  g_string_append_len(buf, (const char *)ptr, bytes);
  return bytes;
}

// Extract the leading "X.Y.Z" from darktable_package_version (which
// can look like "5.5.0+156~gabcdef-dirty" or just "5.4.0").
static char *_get_darktable_version_prefix(void)
{
  int major = 0, minor = 0, patch = 0;
  if(sscanf(darktable_package_version, "%d.%d.%d", &major, &minor, &patch) == 3)
    return g_strdup_printf("%d.%d.%d", major, minor, patch);
  return NULL;
}

// Resolve the model-repo release tag for the current darktable version
// via releases-index.json on raw.githubusercontent.com (CDN, no
// api.github.com rate limit). On failure sets *error_msg to a translated
// string the caller owns.
static char *_find_latest_compatible_release(const char *repository, char **error_msg)
{
  if(error_msg) *error_msg = NULL;

  char *dt_version = _get_darktable_version_prefix();
  if(!dt_version)
    return NULL;

  // `HEAD` follows the default branch — survives a master/main rename
  char *url = g_strdup_printf(
    "https://raw.githubusercontent.com/%s/HEAD/releases-index.json",
    repository);

  CURL *curl = curl_easy_init();
  if(!curl)
  {
    g_free(url);
    g_free(dt_version);
    return NULL;
  }
  dt_curl_init(curl, FALSE);

  GString *response = g_string_new(NULL);
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, _curl_write_string);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);

  CURLcode res = curl_easy_perform(curl);
  long http_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
  curl_easy_cleanup(curl);
  g_free(url);

  if(res != CURLE_OK || http_code != 200)
  {
    dt_print(DT_DEBUG_AI,
             "[ai_models] failed to fetch releases-index.json: curl=%d, http=%ld",
             res, http_code);
    if(error_msg)
    {
      if(res != CURLE_OK)
        *error_msg = g_strdup_printf(_("network error: %s"), curl_easy_strerror(res));
      else if(http_code == 404)
        *error_msg = g_strdup_printf(
          _("model repository \"%s\" missing releases-index.json"),
          repository);
      else
        *error_msg = g_strdup_printf(
          _("could not fetch releases-index.json (HTTP %ld)"), http_code);
    }
    g_string_free(response, TRUE);
    g_free(dt_version);
    return NULL;
  }

  JsonParser *parser = json_parser_new();
  if(!json_parser_load_from_data(parser, response->str, response->len, NULL))
  {
    g_object_unref(parser);
    g_string_free(response, TRUE);
    g_free(dt_version);
    return NULL;
  }
  g_string_free(response, TRUE);

  JsonNode *root = json_parser_get_root(parser);
  if(!root || !JSON_NODE_HOLDS_OBJECT(root))
  {
    g_object_unref(parser);
    g_free(dt_version);
    return NULL;
  }

  char *tag = NULL;
  JsonObject *root_obj = json_node_get_object(root);

  // v1 is the frozen contract; future schemas live in parallel keys
  if(json_object_has_member(root_obj, "schema"))
  {
    const int schema = (int)json_object_get_int_member(root_obj, "schema");
    if(schema != 1)
      dt_print(DT_DEBUG_AI,
               "[ai_models] releases-index.json top-level schema %d not "
               "supported by this darktable; expected 1", schema);
  }

  if(json_object_has_member(root_obj, "compatible_releases"))
  {
    JsonObject *map
      = json_object_get_object_member(root_obj, "compatible_releases");
    if(map && json_object_has_member(map, dt_version))
    {
      const char *t = json_object_get_string_member(map, dt_version);
      if(t) tag = g_strdup(t);
    }
  }
  else
  {
    dt_print(DT_DEBUG_AI,
             "[ai_models] releases-index.json missing 'compatible_releases'");
  }

  g_object_unref(parser);

  if(tag)
    dt_print(DT_DEBUG_AI,
             "[ai_models] found compatible release: %s (darktable %s)",
             tag, dt_version);
  else
    dt_print(DT_DEBUG_AI,
             "[ai_models] no compatible release in releases-index.json for darktable %s",
             dt_version);

  g_free(dt_version);
  return tag;
}

// Fallback SHA lookup for downloads when check_updates hasn't run yet.
// Fetches versions.json from the release (CDN, not api.github.com).
static char *_fetch_asset_digest(
  const char *repository,
  const char *release_tag,
  const char *asset_name)
{
  char *model_id = g_strdup(asset_name);
  char *ext = strrchr(model_id, '.');
  if(ext && g_strcmp0(ext, ".dtmodel") == 0) *ext = '\0';

  char *url = g_strdup_printf(
    "https://github.com/%s/releases/download/%s/versions.json",
    repository, release_tag);

  CURL *curl = curl_easy_init();
  if(!curl)
  {
    g_free(url);
    g_free(model_id);
    return NULL;
  }
  dt_curl_init(curl, FALSE);

  GString *response = g_string_new(NULL);
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, _curl_write_string);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);

  CURLcode res = curl_easy_perform(curl);
  long http_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
  curl_easy_cleanup(curl);
  g_free(url);

  if(res != CURLE_OK || http_code != 200)
  {
    dt_print(DT_DEBUG_AI,
             "[ai_models] failed to fetch versions.json: curl=%d, http=%ld",
             res, http_code);
    g_string_free(response, TRUE);
    g_free(model_id);
    return NULL;
  }

  JsonParser *parser = json_parser_new();
  if(!json_parser_load_from_data(parser, response->str, response->len, NULL))
  {
    g_object_unref(parser);
    g_string_free(response, TRUE);
    g_free(model_id);
    return NULL;
  }
  g_string_free(response, TRUE);

  JsonNode *root = json_parser_get_root(parser);
  char *digest = NULL;
  if(root && JSON_NODE_HOLDS_OBJECT(root))
  {
    JsonObject *root_obj = json_node_get_object(root);
    if(json_object_has_member(root_obj, "models"))
    {
      JsonObject *models_obj = json_object_get_object_member(root_obj, "models");
      if(json_object_has_member(models_obj, model_id))
      {
        JsonNode *m_node = json_object_get_member(models_obj, model_id);
        if(JSON_NODE_HOLDS_OBJECT(m_node))
        {
          JsonObject *m_obj = json_node_get_object(m_node);
          if(json_object_has_member(m_obj, "sha256"))
          {
            const char *s = json_object_get_string_member(m_obj, "sha256");
            if(s && g_str_has_prefix(s, "sha256:"))
            {
              digest = g_strdup(s);
              dt_print(DT_DEBUG_AI,
                       "[ai_models] asset %s digest from versions.json: %s",
                       asset_name, digest);
            }
          }
        }
      }
    }
  }

  g_object_unref(parser);

  if(!digest)
    dt_print(DT_DEBUG_AI,
             "[ai_models] no sha256 for %s in versions.json (release %s)",
             model_id, release_tag);

  g_free(model_id);
  return digest;
}
#endif // HAVE_AI_DOWNLOAD

// core API

// set up directories and provider config; returns FALSE if a required
// directory could not be created — the model/cache dirs are unusable
// and downloads/scans will fail; no-op returning TRUE if already
// initialized (models_dir != NULL)
static gboolean _setup_registry(dt_ai_registry_t *registry)
{
  if(registry->models_dir) return TRUE;

  char cachedir[PATH_MAX] = {0};
  dt_loc_get_user_cache_dir(cachedir, sizeof(cachedir));

  // models_dir is the unlocked "already-initialised" sentinel in
  // init_lazy, so it must be the LAST field published
  char *models = dt_ai_resolve_models_path_override();
  if(!models)
    models = g_build_filename(g_get_user_data_dir(),
                              "darktable", "models", NULL);
  char *cache = g_build_filename(cachedir, "ai_downloads", NULL);

  // attempt both before bailing so the log reports every missing dir
  gboolean ok = TRUE;
  if(!_ensure_directory(models))
  {
    dt_print(DT_DEBUG_ALWAYS, "[ai_models] cannot create models dir: %s", models);
    ok = FALSE;
  }
  if(!_ensure_directory(cache))
  {
    dt_print(DT_DEBUG_ALWAYS, "[ai_models] cannot create cache dir: %s", cache);
    ok = FALSE;
  }

  char *prov_str = dt_conf_get_string(DT_AI_CONF_PROVIDER);
  registry->provider = dt_ai_provider_from_string(prov_str);
  g_free(prov_str);

  registry->cache_dir = cache;
  registry->models_dir = models;  // publish last

  dt_print(DT_DEBUG_AI,
           "[ai_models] initialized: models_dir=%s", registry->models_dir);
  dt_print(DT_DEBUG_AI,
           "[ai_models] initialized: cache_dir=%s", registry->cache_dir);
  return ok;
}

// --- Registry state accessors ---

gboolean dt_ai_registry_is_enabled(void)
{
  dt_ai_registry_t *registry = darktable.ai_registry;
  if(!registry) return FALSE;
  g_mutex_lock(&registry->lock);
  const gboolean v = registry->ai_enabled;
  g_mutex_unlock(&registry->lock);
  return v;
}

void dt_ai_registry_set_enabled(const gboolean enabled)
{
  dt_ai_registry_t *registry = darktable.ai_registry;
  if(!registry) return;
  g_mutex_lock(&registry->lock);
  registry->ai_enabled = enabled;
  g_mutex_unlock(&registry->lock);
}

void dt_ai_registry_set_provider(const dt_ai_provider_t provider)
{
  dt_ai_registry_t *registry = darktable.ai_registry;
  if(!registry) return;
  g_mutex_lock(&registry->lock);
  registry->provider = provider;
  g_mutex_unlock(&registry->lock);
}

gboolean dt_ai_models_init(void)
{
  dt_ai_registry_t *registry = g_new0(dt_ai_registry_t, 1);
  g_mutex_init(&registry->lock);

  registry->ai_enabled = dt_conf_get_bool(CONF_AI_ENABLED);

  // when AI starts disabled, directory setup is deferred to init_lazy;
  // nothing can fail here, so report success
  gboolean ok = TRUE;
  if(registry->ai_enabled)
    ok = _setup_registry(registry);

  // capture conf snapshot so *_changed_since_load() helpers reference
  // the startup state, not a value modified by user before first ORT use
  dt_ai_snapshot_conf_state();

  darktable.ai_registry = registry;
  return ok;
}

void dt_ai_models_init_lazy(void)
{
  dt_ai_registry_t *registry = darktable.ai_registry;
  if(!registry || registry->models_dir)
  {
    // catch broken publication order in _setup_registry: any field
    // added must be initialised BEFORE models_dir, the sentinel
    if(registry) g_assert(registry->cache_dir);
    return;
  }

  g_mutex_lock(&registry->lock);
  _setup_registry(registry);
  g_mutex_unlock(&registry->lock);

  dt_ai_models_load_registry();
}

static dt_ai_model_t *_parse_model_json(JsonObject *obj)
{
  if(!json_object_has_member(obj, "id") || !json_object_has_member(obj, "name"))
    return NULL;

  dt_ai_model_t *model = _model_new();
  model->id = g_strdup(json_object_get_string_member(obj, "id"));
  model->name = g_strdup(json_object_get_string_member(obj, "name"));
  model->github_asset = g_strdup_printf("%s.dtmodel", model->id);

  if(json_object_has_member(obj, "task"))
    model->task = g_strdup(json_object_get_string_member(obj, "task"));
  if(json_object_has_member(obj, "min_version"))
    model->min_version = g_strdup(json_object_get_string_member(obj, "min_version"));
  if(json_object_has_member(obj, "default"))
    model->is_default = json_object_get_boolean_member(obj, "default");

  return model;
}

gboolean dt_ai_models_load_registry(void)
{
  dt_ai_registry_t *registry = darktable.ai_registry;
  if(!registry || !registry->ai_enabled)
    return FALSE;

  // find the registry json file in the data directory
  char datadir[PATH_MAX] = {0};
  dt_loc_get_datadir(datadir, sizeof(datadir));
  char *registry_path = g_build_filename(datadir, "ai_models.json", NULL);

  if(!g_file_test(registry_path, G_FILE_TEST_EXISTS))
  {
    dt_print(DT_DEBUG_AI, "[ai_models] registry file not found: %s", registry_path);
    g_free(registry_path);
    return FALSE;
  }

  GError *error = NULL;
  JsonParser *parser = json_parser_new();

  if(!json_parser_load_from_file(parser, registry_path, &error))
  {
    dt_print(DT_DEBUG_AI,
      "[ai_models] failed to parse registry: %s",
      error ? error->message : "unknown error");
    if(error)
      g_error_free(error);
    g_object_unref(parser);
    g_free(registry_path);
    return FALSE;
  }

  JsonNode *root = json_parser_get_root(parser);
  if(!JSON_NODE_HOLDS_OBJECT(root))
  {
    dt_print(DT_DEBUG_AI, "[ai_models] registry root is not an object");
    g_object_unref(parser);
    g_free(registry_path);
    return FALSE;
  }

  JsonObject *root_obj = json_node_get_object(root);

  g_mutex_lock(&registry->lock);

  // clear existing models
  g_list_free_full(registry->models, (GDestroyNotify)_model_free);
  registry->models = NULL;

  // parse repository - config overrides json default
  g_free(registry->repository);
  registry->repository = NULL;

  if(dt_conf_key_exists(CONF_AI_REPOSITORY))
    registry->repository = dt_conf_get_string(CONF_AI_REPOSITORY);

  dt_print(DT_DEBUG_AI,
           "[ai_models] using repository: %s",
           registry->repository ? registry->repository : "(none)");

  // parse models array
  if(json_object_has_member(root_obj, "models"))
  {
    JsonArray *models_arr = json_object_get_array_member(root_obj, "models");
    guint len = json_array_get_length(models_arr);

    for(guint i = 0; i < len; i++)
    {
      JsonNode *node = json_array_get_element(models_arr, i);
      if(!JSON_NODE_HOLDS_OBJECT(node))
        continue;

      dt_ai_model_t *model = _parse_model_json(json_node_get_object(node));
      if(model)
      {
        // load enabled state from user config
        char *conf_key
          = g_strdup_printf("%s%s/enabled", CONF_MODEL_ENABLED_PREFIX, model->id);
        if(dt_conf_key_exists(conf_key))
          model->enabled = dt_conf_get_bool(conf_key);
        g_free(conf_key);

        registry->models = g_list_prepend(registry->models, model);
        dt_print(DT_DEBUG_AI,
                 "[ai_models] registered model: %s (%s)",
                 model->name, model->id);
      }
    }
  }

  // reverse to restore original json order (we used prepend for O(1) insertion)
  registry->models = g_list_reverse(registry->models);

  const int model_count = g_list_length(registry->models);

  g_mutex_unlock(&registry->lock);

  dt_print(DT_DEBUG_AI,
           "[ai_models] registry loaded: %d models from %s",
           model_count, registry_path);

  g_object_unref(parser);
  g_free(registry_path);

  // check which models are actually downloaded
  dt_ai_models_refresh_status();

  return TRUE;
}

// validate that a model_id is a plain directory name with no path separators
// or ".." components that could escape the models directory
static gboolean _valid_model_id(const char *model_id);
static dt_ai_model_t *_find_model_unlocked(dt_ai_registry_t *registry,
                                            const char *model_id);

// parse a local model's config.json into a dt_ai_model_t
// uses directory name as fallback for id/name. no github_asset or checksum
static dt_ai_model_t *_parse_local_model_config(const char *config_path,
                                                 const char *dir_name)
{
  JsonParser *parser = json_parser_new();
  GError *error = NULL;

  if(!json_parser_load_from_file(parser, config_path, &error))
  {
    dt_print(DT_DEBUG_AI, "[ai_models] failed to parse %s: %s",
             config_path, error ? error->message : "unknown");
    if(error) g_error_free(error);
    g_object_unref(parser);
    return NULL;
  }

  JsonNode *root = json_parser_get_root(parser);
  if(!JSON_NODE_HOLDS_OBJECT(root))
  {
    g_object_unref(parser);
    return NULL;
  }

  JsonObject *obj = json_node_get_object(root);

  const char *id = json_object_has_member(obj, "id")
    ? json_object_get_string_member(obj, "id") : dir_name;
  const char *name = json_object_has_member(obj, "name")
    ? json_object_get_string_member(obj, "name") : dir_name;

  if(!id || !id[0])
  {
    g_object_unref(parser);
    return NULL;
  }

  dt_ai_model_t *model = _model_new();
  model->id = g_strdup(id);
  model->name = g_strdup(name);

  if(json_object_has_member(obj, "description"))
    model->description = g_strdup(json_object_get_string_member(obj, "description"));
  if(json_object_has_member(obj, "task"))
    model->task = g_strdup(json_object_get_string_member(obj, "task"));
  if(json_object_has_member(obj, "version"))
    model->version = g_strdup(json_object_get_string_member(obj, "version"));
  if(json_object_has_member(obj, "spatial_dims"))
  {
    JsonArray *arr = json_object_get_array_member(obj, "spatial_dims");
    if(arr && json_array_get_length(arr) >= 2)
    {
      const char *h = json_array_get_string_element(arr, 0);
      const char *w = json_array_get_string_element(arr, 1);
      if(h && h[0]) model->spatial_dim_h = g_strdup(h);
      if(w && w[0]) model->spatial_dim_w = g_strdup(w);
    }
  }

  // no github_asset, no checksum — local-only model
  model->enabled = TRUE;

  g_object_unref(parser);
  return model;
}

static gboolean _valid_model_id(const char *model_id)
{
  if(!model_id || !model_id[0])
    return FALSE;
  if(strchr(model_id, '/') || strchr(model_id, '\\'))
    return FALSE;
  if(strcmp(model_id, "..") == 0 || strcmp(model_id, ".") == 0)
    return FALSE;
  return TRUE;
}

void dt_ai_models_refresh_status(void)
{
  dt_ai_registry_t *registry = darktable.ai_registry;
  if(!registry)
    return;

  g_mutex_lock(&registry->lock);

  // remove previously-discovered local models (no github_asset)
  // these will be re-discovered from disk below if still present
  GList *l = registry->models;
  while(l)
  {
    GList *next = g_list_next(l);
    dt_ai_model_t *model = (dt_ai_model_t *)l->data;
    if(!model->github_asset)
    {
      _model_free(model);
      registry->models = g_list_delete_link(registry->models, l);
    }
    l = next;
  }

  // pass 1: update status for registry models
  for(GList *l2 = registry->models; l2; l2 = g_list_next(l2))
  {
    dt_ai_model_t *model = (dt_ai_model_t *)l2->data;

    // skip models with invalid ids (path traversal protection)
    if(!_valid_model_id(model->id))
      continue;

    // check if model directory exists and contains required files
    char *model_dir = g_build_filename(registry->models_dir, model->id, NULL);
    char *config_path = g_build_filename(model_dir, "config.json", NULL);

    if(g_file_test(model_dir, G_FILE_TEST_IS_DIR)
       && g_file_test(config_path, G_FILE_TEST_EXISTS))
    {
      model->status = DT_AI_MODEL_DOWNLOADED;
      // read version from the model's own config.json
      dt_ai_model_t *local = _parse_local_model_config(config_path, model->id);
      if(local)
      {
        if(local->version && local->version[0])
        {
          g_free(model->version);
          model->version = g_strdup(local->version);
        }
        g_free(model->spatial_dim_h);
        g_free(model->spatial_dim_w);
        model->spatial_dim_h = g_strdup(local->spatial_dim_h);
        model->spatial_dim_w = g_strdup(local->spatial_dim_w);
        _model_free(local);

        // check if installed version meets minimum requirement
        if(model->min_version
           && _version_compare(model->version, model->min_version) < 0)
        {
          model->status = DT_AI_MODEL_UPDATE_REQUIRED;
          dt_print(DT_DEBUG_AI,
                   "[ai_models] model %s v%s is older than "
                   "required v%s - please update",
                   model->id,
                   model->version ? model->version : "0.0",
                   model->min_version);
        }
      }
    }
    else
    {
      model->status = DT_AI_MODEL_NOT_DOWNLOADED;
    }

    g_free(config_path);
    g_free(model_dir);
  }

  // pass 2: discover locally-installed models not in registry
  if(registry->models_dir)
  {
    GDir *dir = g_dir_open(registry->models_dir, 0, NULL);
    if(dir)
    {
      const char *entry_name;
      while((entry_name = g_dir_read_name(dir)))
      {
        if(!_valid_model_id(entry_name))
          continue;

        // skip if already in registry (e.g. downloaded via ai_models.json)
        if(_find_model_unlocked(registry, entry_name))
          continue;

        char *model_dir = g_build_filename(registry->models_dir, entry_name, NULL);
        char *config_path = g_build_filename(model_dir, "config.json", NULL);

        if(g_file_test(model_dir, G_FILE_TEST_IS_DIR)
           && g_file_test(config_path, G_FILE_TEST_EXISTS))
        {
          dt_ai_model_t *model = _parse_local_model_config(config_path, entry_name);
          if(model)
          {
            model->status = DT_AI_MODEL_DOWNLOADED;
            registry->models = g_list_append(registry->models, model);
            dt_print(DT_DEBUG_AI,
                     "[ai_models] discovered local model: %s (%s)",
                     model->name, model->id);
          }
        }

        g_free(config_path);
        g_free(model_dir);
      }
      g_dir_close(dir);
    }
  }

  g_mutex_unlock(&registry->lock);
}

// takes ownership of `parser`
static gboolean _apply_updates_idle(gpointer user_data)
{
  JsonParser *parser = (JsonParser *)user_data;
  dt_ai_registry_t *registry = darktable.ai_registry;
  if(!registry)
  {
    g_object_unref(parser);
    return G_SOURCE_REMOVE;
  }

  JsonNode *root = json_parser_get_root(parser);
  JsonObject *root_obj = json_node_get_object(root);
  JsonObject *models_obj
    = json_object_has_member(root_obj, "models")
      ? json_object_get_object_member(root_obj, "models")
      : NULL;

  if(!models_obj)
  {
    g_object_unref(parser);
    dt_print(DT_DEBUG_AI,
             "[ai_models] check_updates: no 'models' object in "
             "versions.json");
    return G_SOURCE_REMOVE;
  }

  // populate model->checksum from sha256 so downloads skip re-fetching
  g_mutex_lock(&registry->lock);
  for(GList *l = registry->models; l; l = g_list_next(l))
  {
    dt_ai_model_t *model = (dt_ai_model_t *)l->data;
    if(!json_object_has_member(models_obj, model->id)) continue;

    JsonNode *m_node = json_object_get_member(models_obj, model->id);
    if(!m_node || !JSON_NODE_HOLDS_OBJECT(m_node)) continue;
    JsonObject *m_obj = json_node_get_object(m_node);

    const char *remote_version = json_object_has_member(m_obj, "version")
      ? json_object_get_string_member(m_obj, "version") : NULL;
    const char *remote_sha256 = json_object_has_member(m_obj, "sha256")
      ? json_object_get_string_member(m_obj, "sha256") : NULL;

    if(remote_sha256 && g_str_has_prefix(remote_sha256, "sha256:"))
    {
      g_free(model->checksum);
      model->checksum = g_strdup(remote_sha256);
    }

    if(remote_version
       && model->status == DT_AI_MODEL_DOWNLOADED
       && _version_compare(model->version, remote_version) < 0)
    {
      model->status = DT_AI_MODEL_UPDATE_AVAILABLE;
      dt_print(DT_DEBUG_AI,
               "[ai_models] update available for %s: v%s -> v%s",
               model->id,
               model->version ? model->version : "?",
               remote_version);
    }
  }
  g_mutex_unlock(&registry->lock);
  g_object_unref(parser);

  DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_AI_MODELS_CHANGED);
  return G_SOURCE_REMOVE;
}

// takes ownership of `repository`; no registry access
static gpointer _check_updates_worker(gpointer data)
{
  char *repository = (char *)data;

  // find the latest compatible release tag
  char *error_msg = NULL;
  char *release_tag
    = _find_latest_compatible_release(repository, &error_msg);
  if(!release_tag)
  {
    dt_print(DT_DEBUG_AI,
             "[ai_models] check_updates: no compatible release found%s%s",
             error_msg ? ": " : "", error_msg ? error_msg : "");
    g_free(error_msg);
    g_free(repository);
    return NULL;
  }
  g_free(error_msg);

  // fetch versions.json from the release
  char *url = g_strdup_printf(
    "https://github.com/%s/releases/download/%s/versions.json",
    repository, release_tag);

  CURL *curl = curl_easy_init();
  if(!curl)
  {
    g_free(url);
    g_free(release_tag);
    g_free(repository);
    return NULL;
  }
  dt_curl_init(curl, FALSE);

  GString *response = g_string_new(NULL);
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, _curl_write_string);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

  CURLcode res = curl_easy_perform(curl);
  long http_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
  curl_easy_cleanup(curl);
  g_free(url);
  g_free(release_tag);
  g_free(repository);

  if(res != CURLE_OK || http_code != 200)
  {
    dt_print(DT_DEBUG_AI,
             "[ai_models] check_updates: failed to fetch versions.json"
             " (curl=%d, http=%ld)",
             res, http_code);
    g_string_free(response, TRUE);
    return NULL;
  }

  // parse versions.json
  JsonParser *parser = json_parser_new();
  if(!json_parser_load_from_data(parser, response->str,
                                 response->len, NULL))
  {
    g_object_unref(parser);
    g_string_free(response, TRUE);
    dt_print(DT_DEBUG_AI,
             "[ai_models] check_updates: failed to parse versions.json");
    return NULL;
  }
  g_string_free(response, TRUE);

  g_idle_add(_apply_updates_idle, parser);
  return NULL;
}

void dt_ai_models_check_updates(void)
{
  dt_ai_registry_t *registry = darktable.ai_registry;
  if(!registry) return;

  // only check once per session
  g_mutex_lock(&registry->lock);
  if(registry->updates_checked)
  {
    g_mutex_unlock(&registry->lock);
    return;
  }
  registry->updates_checked = TRUE;
  char *repository = g_strdup(registry->repository);
  g_mutex_unlock(&registry->lock);

  if(!repository || !repository[0])
  {
    g_free(repository);
    return;
  }

  // detached: worker owns `repository`, off the GTK main thread
  GThread *t = g_thread_new("ai-model-updates",
                            _check_updates_worker, repository);
  g_thread_unref(t);
}

void dt_ai_models_cleanup(void)
{
  dt_ai_registry_t *registry = darktable.ai_registry;
  if(!registry)
    return;

  // unpublish before teardown so nothing can pick up a half-freed registry
  darktable.ai_registry = NULL;

  g_mutex_lock(&registry->lock);
  g_list_free_full(registry->models, (GDestroyNotify)_model_free);
  registry->models = NULL;
  g_mutex_unlock(&registry->lock);

  g_mutex_clear(&registry->lock);

  if(registry->env)
    dt_ai_env_destroy(registry->env);
  g_free(registry->repository);
  g_free(registry->models_dir);
  g_free(registry->cache_dir);
  g_free(registry);
}

// internal: find model by id without locking. caller must hold registry->lock
// returns direct pointer to registry-owned model (not a copy)
static dt_ai_model_t *
_find_model_unlocked(dt_ai_registry_t *registry, const char *model_id)
{
  for(GList *l = registry->models; l; l = g_list_next(l))
  {
    dt_ai_model_t *model = (dt_ai_model_t *)l->data;
    if(g_strcmp0(model->id, model_id) == 0)
      return model;
  }
  return NULL;
}

int dt_ai_models_get_count(void)
{
  dt_ai_registry_t *registry = darktable.ai_registry;
  if(!registry)
    return 0;
  g_mutex_lock(&registry->lock);
  int count = g_list_length(registry->models);
  g_mutex_unlock(&registry->lock);
  return count;
}

dt_ai_model_t *dt_ai_models_get_by_index(const int index)
{
  dt_ai_registry_t *registry = darktable.ai_registry;
  if(!registry || index < 0)
    return NULL;
  g_mutex_lock(&registry->lock);
  dt_ai_model_t *model = g_list_nth_data(registry->models, index);
  dt_ai_model_t *copy = _model_copy(model);
  g_mutex_unlock(&registry->lock);
  return copy;
}

dt_ai_model_t *dt_ai_models_get_by_id(const char *model_id)
{
  dt_ai_registry_t *registry = darktable.ai_registry;
  if(!registry || !model_id)
    return NULL;
  g_mutex_lock(&registry->lock);
  dt_ai_model_t *model = _find_model_unlocked(registry, model_id);
  dt_ai_model_t *copy = _model_copy(model);
  g_mutex_unlock(&registry->lock);
  return copy;
}

#ifdef HAVE_AI_DOWNLOAD

typedef struct dt_ai_download_data_t
{
  dt_ai_registry_t *registry;
  char *model_id; // owned copy of model id (safe to use without lock)
  dt_ai_progress_callback callback;
  gpointer user_data;
  FILE *file;
  const gboolean *cancel_flag; // optional: set to non-NULL to enable cancellation
} dt_ai_download_data_t;

static size_t _curl_write_callback(void *ptr,
                                   size_t size,
                                   size_t nmemb,
                                   void *data)
{
  dt_ai_download_data_t *dl = (dt_ai_download_data_t *)data;
  return fwrite(ptr, size, nmemb, dl->file);
}

static int _curl_progress_callback(void *clientp,
                                   curl_off_t dltotal,
                                   curl_off_t dlnow,
                                   curl_off_t ultotal,
                                   curl_off_t ulnow)
{
  dt_ai_download_data_t *dl = (dt_ai_download_data_t *)clientp;

  // check for cancellation
  if(dl->cancel_flag && g_atomic_int_get(dl->cancel_flag))
    return 1; // non-zero aborts the transfer

  if(dltotal > 0)
  {
    double progress = (double)dlnow / (double)dltotal;

    // update model progress under lock
    g_mutex_lock(&dl->registry->lock);
    dt_ai_model_t *m = _find_model_unlocked(dl->registry, dl->model_id);
    if(m)
      m->download_progress = progress;
    g_mutex_unlock(&dl->registry->lock);

    if(dl->callback)
      dl->callback(dl->model_id, progress, dl->user_data);
  }
  return 0;
}

static gboolean _verify_checksum(const char *filepath,
                                 const char *expected)
{
  if(!expected || !g_str_has_prefix(expected, "sha256:"))
  {
    dt_print(DT_DEBUG_AI,
             "[ai_models] no valid checksum provided - rejecting download");
    return FALSE; // reject files without a valid checksum
  }

  const char *expected_hash = expected + 7; // skip "sha256:"

  GChecksum *checksum = g_checksum_new(G_CHECKSUM_SHA256);
  if(!checksum)
    return FALSE;

  // stream file in chunks to avoid loading entire file into memory
  FILE *f = g_fopen(filepath, "rb");
  if(!f)
  {
    dt_print(DT_DEBUG_AI, "[ai_models] failed to open file for checksum: %s", filepath);
    g_checksum_free(checksum);
    return FALSE;
  }

  guchar buf[65536];
  size_t n;
  while((n = fread(buf, 1, sizeof(buf), f)) > 0)
    g_checksum_update(checksum, buf, n);
  fclose(f);

  const gchar *computed = g_checksum_get_string(checksum);
  gboolean match = g_ascii_strcasecmp(computed, expected_hash) == 0;

  if(!match)
  {
    dt_print(DT_DEBUG_AI,
             "[ai_models] checksum mismatch: expected %s, got %s",
             expected_hash, computed);
  }

  g_checksum_free(checksum);
  return match;
}
#endif //HAVE_AI_DOWNLOAD

static gboolean _rmdir_recursive(const char *path);

static gboolean _extract_zip(const char *zippath,
                             const char *destdir)
{
  struct archive *a = archive_read_new();
  struct archive *ext = archive_write_disk_new();
  struct archive_entry *entry;
  int r;
  gboolean success = TRUE;

  archive_read_support_format_zip(a);
  archive_write_disk_set_options(
    ext,
    ARCHIVE_EXTRACT_TIME
      | ARCHIVE_EXTRACT_PERM
      | ARCHIVE_EXTRACT_SECURE_SYMLINKS
      | ARCHIVE_EXTRACT_SECURE_NODOTDOT);

  if((r = archive_read_open_filename(a, zippath, 10240)) != ARCHIVE_OK)
  {
    dt_print(DT_DEBUG_AI,
             "[ai_models] failed to open archive: %s",
             archive_error_string(a));
    archive_read_free(a);
    archive_write_free(ext);
    return FALSE;
  }

  _ensure_directory(destdir);

  // resolve destdir to a canonical path for path traversal validation
  char *real_destdir = realpath(destdir, NULL);
  if(!real_destdir)
  {
    dt_print(DT_DEBUG_AI, "[ai_models] failed to resolve destdir: %s", destdir);
    archive_read_close(a);
    archive_read_free(a);
    archive_write_free(ext);
    return FALSE;
  }

  const size_t destdir_len = strlen(real_destdir);

  while(archive_read_next_header(a, &entry) == ARCHIVE_OK)
  {
    const char *entry_name = archive_entry_pathname(entry);

    // reject entries with path traversal components
    if(g_strstr_len(entry_name, -1, "..") != NULL)
    {
      dt_print(DT_DEBUG_AI,
               "[ai_models] skipping suspicious archive entry: %s",
               entry_name);
      continue;
    }

    // build full path in destination
    char *full_path = g_build_filename(real_destdir, entry_name, NULL);

    // verify the resolved path is within destdir
    char *real_full_path = realpath(full_path, NULL);
    // for new files, realpath returns NULL; check the parent directory instead
    if(!real_full_path)
    {
      char *parent = g_path_get_dirname(full_path);
      _ensure_directory(parent);
      char *real_parent = realpath(parent, NULL);
      g_free(parent);
      if(
        !real_parent || strncmp(real_parent, real_destdir, destdir_len) != 0
        || (real_parent[destdir_len] != '/' && real_parent[destdir_len] != '\\'
            && real_parent[destdir_len] != '\0'))
      {
        dt_print(DT_DEBUG_AI, "[ai_models] path traversal blocked: %s", entry_name);
        free(real_parent);
        g_free(full_path);
        continue;
      }
      free(real_parent);
    }
    else
    {
      if(
        strncmp(real_full_path, real_destdir, destdir_len) != 0
        || (real_full_path[destdir_len] != '/' && real_full_path[destdir_len] != '\\'
            && real_full_path[destdir_len] != '\0'))
      {
        dt_print(DT_DEBUG_AI, "[ai_models] path traversal blocked: %s", entry_name);
        free(real_full_path);
        g_free(full_path);
        continue;
      }
      free(real_full_path);
    }

    archive_entry_set_pathname(entry, full_path);

    r = archive_write_header(ext, entry);
    if(r == ARCHIVE_OK)
    {
      const void *buff;
      size_t size;
      la_int64_t offset;

      while(archive_read_data_block(a, &buff, &size, &offset) == ARCHIVE_OK)
      {
        if(archive_write_data_block(ext, buff, size, offset) != ARCHIVE_OK)
        {
          dt_print(DT_DEBUG_AI, "[ai_models] write error: %s", archive_error_string(ext));
          success = FALSE;
          break;
        }
      }
      if(archive_write_finish_entry(ext) != ARCHIVE_OK)
        success = FALSE;
    }
    else
    {
      dt_print(DT_DEBUG_AI,
               "[ai_models] write header error: %s",
               archive_error_string(ext));
      success = FALSE;
    }

    g_free(full_path);

    if(!success)
      break;
  }

  free(real_destdir);
  archive_read_close(a);
  archive_read_free(a);
  archive_write_close(ext);
  archive_write_free(ext);

  return success;
}

// atomically install a .dtmodel into dest_root/<model_id>/.
// extract into a staging directory inside dest_root, then rename
// the model subdir into its final place only on full success.
// on failure, staging is cleaned up and the existing
// dest_root/<model_id>/ (if any) is left untouched.
static gboolean _extract_zip_atomic(const char *zippath,
                                    const char *dest_root,
                                    const char *model_id)
{
  if(!zippath || !dest_root || !model_id || !model_id[0])
    return FALSE;

  _ensure_directory(dest_root);

  // staging dir as a sibling of the final dir, so the rename is
  // intra-filesystem and atomic. g_mkdtemp fills XXXXXX with a
  // unique suffix and creates the directory
  gchar *staging
    = g_strdup_printf("%s%s.staging.XXXXXX", dest_root, G_DIR_SEPARATOR_S);
  if(!g_mkdtemp(staging))
  {
    dt_print(DT_DEBUG_AI,
             "[ai_models] failed to create staging dir under %s", dest_root);
    g_free(staging);
    return FALSE;
  }

  if(!_extract_zip(zippath, staging))
  {
    _rmdir_recursive(staging);
    g_free(staging);
    return FALSE;
  }

  // the zip's top-level dir is the model id; the extracted tree is
  // staging/<model_id>/. move it to dest_root/<model_id>/.
  gchar *staged_model = g_build_filename(staging, model_id, NULL);
  gchar *final_path = g_build_filename(dest_root, model_id, NULL);

  if(!g_file_test(staged_model, G_FILE_TEST_IS_DIR))
  {
    dt_print(DT_DEBUG_AI,
             "[ai_models] archive top-level does not match model id '%s'",
             model_id);
    _rmdir_recursive(staging);
    g_free(staging);
    g_free(staged_model);
    g_free(final_path);
    return FALSE;
  }

  // replace any existing install; we just verified a complete copy in staging
  if(g_file_test(final_path, G_FILE_TEST_EXISTS))
    _rmdir_recursive(final_path);

  const gboolean ok = g_rename(staged_model, final_path) == 0;
  if(!ok)
    dt_print(DT_DEBUG_AI,
             "[ai_models] failed to move staged model into place: %s -> %s",
             staged_model, final_path);

  g_free(staged_model);
  g_free(final_path);
  _rmdir_recursive(staging);  // remove the now-empty staging parent
  g_free(staging);
  return ok;
}

// peek at the first archive entry to recover the model_id (the top
// level directory name in the zip layout)
static char *_zip_top_dir(const char *zippath)
{
  struct archive *a = archive_read_new();
  archive_read_support_format_zip(a);
  if(archive_read_open_filename(a, zippath, 10240) != ARCHIVE_OK)
  {
    archive_read_free(a);
    return NULL;
  }
  struct archive_entry *entry;
  char *result = NULL;
  if(archive_read_next_header(a, &entry) == ARCHIVE_OK)
  {
    const char *path = archive_entry_pathname(entry);
    const char *slash = path ? strchr(path, '/') : NULL;
    if(slash)      result = g_strndup(path, slash - path);
    else if(path)  result = g_strdup(path);
  }
  archive_read_close(a);
  archive_read_free(a);
  return result;
}

// activate `model_id` only when nothing is active for its task
static void _activate_if_unset(dt_ai_registry_t *registry,
                               const char *model_id)
{
  if(!registry || !model_id) return;
  gchar *task = NULL;
  g_mutex_lock(&registry->lock);
  const dt_ai_model_t *m = _find_model_unlocked(registry, model_id);
  if(m && m->task) task = g_strdup(m->task);
  g_mutex_unlock(&registry->lock);
  if(!task) return;
  char *current = dt_ai_models_get_active_for_task(task);
  if(!current || !current[0])
    dt_ai_models_set_active_for_task(task, model_id);
  g_free(current);
  g_free(task);
}

// install a local .dtmodel file (zip archive) into the models directory.
// returns error message (caller must free) or NULL on success.
char *dt_ai_models_install_local(const char *filepath)
{
  dt_ai_registry_t *registry = darktable.ai_registry;
  if(!registry || !filepath)
    return g_strdup(_("invalid parameters"));

  if(!g_file_test(filepath, G_FILE_TEST_IS_REGULAR))
    return g_strdup_printf(_("file not found: %s"), filepath);

  char *installed_id = _zip_top_dir(filepath);
  if(!_valid_model_id(installed_id))
  {
    // zip's top-level dir becomes the model id and a conf key — reject
    // empty / "." / ".." / path-separator content before it gets there
    char *err = g_strdup_printf(
      _("archive top-level directory is not a valid model id: \"%s\""),
      installed_id ? installed_id : "");
    g_free(installed_id);
    return err;
  }

  if(!_extract_zip_atomic(filepath, registry->models_dir, installed_id))
  {
    g_free(installed_id);
    return g_strdup(_("failed to extract model archive"));
  }

  // rescan models directory to pick up newly installed model
  dt_ai_models_refresh_status();

  _activate_if_unset(registry, installed_id);

  dt_print(DT_DEBUG_AI, "[ai_models] model installed from: %s", filepath);

  g_free(installed_id);
  return NULL; // success
}

// best installed model for `task`: default preferred, else first found.
// caller must hold registry->lock
static const char *_pick_fallback_active_unlocked(dt_ai_registry_t *registry,
                                                  const char *task)
{
  if(!registry || !task) return NULL;
  const char *first_installed = NULL;
  for(GList *l = registry->models; l; l = g_list_next(l))
  {
    const dt_ai_model_t *m = (const dt_ai_model_t *)l->data;
    if(!m->task || strcmp(m->task, task) != 0) continue;
    if(m->status != DT_AI_MODEL_DOWNLOADED) continue;
    if(m->is_default) return m->id;
    if(!first_installed) first_installed = m->id;
  }
  return first_installed;
}

#ifdef HAVE_AI_DOWNLOAD
// synchronous download - returns error message or NULL on success
char *dt_ai_models_download_sync(const char *model_id,
                                 dt_ai_progress_callback callback,
                                 gpointer user_data,
                                 const gboolean *cancel_flag)
{
  dt_ai_registry_t *registry = darktable.ai_registry;
  dt_print(DT_DEBUG_AI,
           "[ai_models] download requested for: %s",
           model_id ? model_id : "(null)");

  if(!registry || !model_id)
    return g_strdup(_("invalid parameters"));

  // lock once to validate, copy immutable fields, and set status
  g_mutex_lock(&registry->lock);
  dt_ai_model_t *model = _find_model_unlocked(registry, model_id);
  if(!model)
  {
    g_mutex_unlock(&registry->lock);
    return g_strdup(_("model not found in registry"));
  }

  if(!model->github_asset)
  {
    g_mutex_unlock(&registry->lock);
    return g_strdup(_("model has no download asset defined"));
  }

  // validate asset filename: reject path separators and query strings
  if(strchr(model->github_asset, '/')
    || strchr(model->github_asset, '\\')
    || strchr(model->github_asset, '?')
    || strchr(model->github_asset, '#')
    || strstr(model->github_asset, ".."))
  {
    g_mutex_unlock(&registry->lock);
    return g_strdup(_("invalid asset filename"));
  }

  if(model->status == DT_AI_MODEL_DOWNLOADING)
  {
    g_mutex_unlock(&registry->lock);
    return g_strdup(_("model is already downloading"));
  }
  model->status = DT_AI_MODEL_DOWNLOADING;
  model->download_progress = 0.0;

  // copy fields we need outside the lock (repository can be replaced by reload)
  char *asset = g_strdup(model->github_asset);
  char *checksum_copy = g_strdup(model->checksum);
  char *repository = g_strdup(registry->repository);
  g_mutex_unlock(&registry->lock);

// helper macro: set model status under lock and return error
// uses _find_model_unlocked to avoid keeping a stale pointer
#define SET_STATUS_AND_RETURN(status_val, err_expr)                                      \
  do                                                                                     \
  {                                                                                      \
    g_mutex_lock(&registry->lock);                                                       \
    dt_ai_model_t *_m = _find_model_unlocked(registry, model_id);                        \
    if(_m)                                                                               \
      _m->status = (status_val);                                                         \
    g_mutex_unlock(&registry->lock);                                                     \
    g_free(asset);                                                                       \
    g_free(checksum_copy);                                                               \
    g_free(repository);                                                                  \
    return (err_expr);                                                                   \
  } while(0)

  // validate repository format (must be "owner/repo" with safe characters)
  if(!repository
    || !g_regex_match_simple("^[a-zA-Z0-9._-]+/[a-zA-Z0-9._-]+$", repository, 0, 0))
  {
    SET_STATUS_AND_RETURN(DT_AI_MODEL_ERROR, g_strdup(_("invalid repository format")));
  }

  {
    char *ver = _get_darktable_version_prefix();
    dt_print(DT_DEBUG_AI, "[ai_models] repository: %s", repository);
    dt_print(DT_DEBUG_AI,
             "[ai_models] darktable version: %s (full: %s)",
             ver ? ver : "unknown", darktable_package_version);
    g_free(ver);
  }

  // find the latest compatible release for this darktable version
  char *release_error = NULL;
  char *release_tag = _find_latest_compatible_release(repository, &release_error);
  if(!release_tag)
  {
    if(release_error)
    {
      SET_STATUS_AND_RETURN(DT_AI_MODEL_ERROR, release_error);
    }
    else
    {
      char *dt_ver = _get_darktable_version_prefix();
      char *err = g_strdup_printf(
        _("no compatible ai model release found for darktable %s"),
        dt_ver ? dt_ver : darktable_package_version);
      g_free(dt_ver);
      SET_STATUS_AND_RETURN(DT_AI_MODEL_ERROR, err);
    }
  }

  // fetch sha256 digest from github releases api if not already known
  if(!checksum_copy || !g_str_has_prefix(checksum_copy, "sha256:"))
  {
    g_free(checksum_copy);
    checksum_copy = _fetch_asset_digest(repository, release_tag, asset);
    if(!checksum_copy)
    {
      g_free(release_tag);

      char *msg = g_strdup_printf(_("could not obtain checksum for %s — "
                                    "refusing to download without integrity verification"),
                                  asset);

      SET_STATUS_AND_RETURN(DT_AI_MODEL_ERROR, msg);
    }
  }

  // build github download url using local copies (not model pointer)
  char *url = g_strdup_printf(
    "https://github.com/%s/releases/download/%s/%s",
    repository,
    release_tag,
    asset);
  g_free(release_tag);

  if(!url)
  {
    SET_STATUS_AND_RETURN(DT_AI_MODEL_ERROR, g_strdup(_("failed to build download url")));
  }

  dt_print(DT_DEBUG_AI, "[ai_models] downloading: %s", url);

  // prepare download path using local copy
  char *download_path = g_build_filename(registry->cache_dir, asset, NULL);

  FILE *file = g_fopen(download_path, "wb");
  if(!file)
  {
    char *err = g_strdup_printf(_("failed to create file: %s"), download_path);
    g_free(download_path);
    g_free(url);
    SET_STATUS_AND_RETURN(DT_AI_MODEL_ERROR, err);
  }

  // set up download data (uses model_id copy, not model pointer)
  dt_ai_download_data_t dl = {
    .registry = registry,
    .model_id = (char *)model_id,
    .callback = callback,
    .user_data = user_data,
    .file = file,
    .cancel_flag = cancel_flag};

  // initialize curl
  CURL *curl = curl_easy_init();
  if(!curl)
  {
    fclose(file);
    g_free(download_path);
    g_free(url);
    SET_STATUS_AND_RETURN(
      DT_AI_MODEL_ERROR,
      g_strdup(_("failed to initialize download")));
  }
  dt_curl_init(curl, FALSE);

  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, _curl_write_callback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &dl);
  curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, _curl_progress_callback);
  curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &dl);
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);

  CURLcode res = curl_easy_perform(curl);

  fclose(file);

  char *error = NULL;

  if(res != CURLE_OK)
  {
    if(res == CURLE_ABORTED_BY_CALLBACK)
      error = g_strdup(_("download cancelled"));
    else
      error = g_strdup_printf(_("download failed: %s"), curl_easy_strerror(res));
    g_unlink(download_path);
  }
  else
  {
    // check http response code
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    if(http_code != 200)
    {
      error = g_strdup_printf(_("http error: %ld"), http_code);
      g_unlink(download_path);
    }
  }

  curl_easy_cleanup(curl);
  g_free(url);

  if(error)
  {
    g_free(download_path);
    SET_STATUS_AND_RETURN(DT_AI_MODEL_ERROR, error);
  }

  // verify checksum if available (fetched from github api or stored in registry)
  if(checksum_copy && g_str_has_prefix(checksum_copy, "sha256:"))
  {
    if(!_verify_checksum(download_path, checksum_copy))
    {
      g_unlink(download_path);
      g_free(download_path);
      SET_STATUS_AND_RETURN(
        DT_AI_MODEL_ERROR,
        g_strdup(_("checksum verification failed")));
    }
  }
  else
  {
    // should not reach here — checksum is now required before download
    g_unlink(download_path);
    g_free(download_path);
    SET_STATUS_AND_RETURN(
      DT_AI_MODEL_ERROR,
      g_strdup_printf(_("no checksum available for %s — "
                        "refusing to install without integrity verification"),
                      asset));
  }

  // extract to models directory (zip already contains model_id folder).
  // atomic: staging dir + rename, so a partial extract never leaves a
  // half-written model_id directory that refresh_status would treat as
  // DOWNLOADED.
  if(!_extract_zip_atomic(download_path, registry->models_dir, model_id))
  {
    g_unlink(download_path);
    g_free(download_path);
    SET_STATUS_AND_RETURN(DT_AI_MODEL_ERROR, g_strdup(_("failed to extract archive")));
  }

  // clean up downloaded zip
  g_unlink(download_path);
  g_free(download_path);

  // invalidate before flipping status so the next session sees a fresh
  // compile, not a stale artifact from the previous model file
  dt_ai_backend_cache_invalidate(model_id);

  // mark success
  g_mutex_lock(&registry->lock);
  dt_ai_model_t *m = _find_model_unlocked(registry, model_id);
  if(m)
  {
    m->status = DT_AI_MODEL_DOWNLOADED;
    m->download_progress = 1.0;
  }
  g_mutex_unlock(&registry->lock);

  _activate_if_unset(registry, model_id);

  dt_print(DT_DEBUG_AI, "[ai_models] download complete: %s", model_id);

  // final callback
  if(callback)
    callback(model_id, 1.0, user_data);

  g_free(asset);
  g_free(checksum_copy);
  g_free(repository);

#undef SET_STATUS_AND_RETURN

  return NULL; // success
}

// wrapper that returns boolean for compatibility
gboolean dt_ai_models_download(const char *model_id,
                               dt_ai_progress_callback callback,
                               gpointer user_data)
{
  char *error = dt_ai_models_download_sync(model_id, callback, user_data, NULL);
  if(error)
  {
    dt_print(DT_DEBUG_AI, "[ai_models] download error: %s", error);
    g_free(error);
    return FALSE;
  }
  return TRUE;
}

gboolean dt_ai_models_download_default(dt_ai_progress_callback callback,
                                       gpointer user_data)
{
  dt_ai_registry_t *registry = darktable.ai_registry;
  if(!registry)
    return FALSE;

  // collect ids while holding lock, then download without lock
  GList *ids = NULL;
  g_mutex_lock(&registry->lock);
  for(GList *l = registry->models; l; l = g_list_next(l))
  {
    dt_ai_model_t *model = (dt_ai_model_t *)l->data;
    if(model->is_default && model->status == DT_AI_MODEL_NOT_DOWNLOADED)
      ids = g_list_prepend(ids, g_strdup(model->id));
  }
  g_mutex_unlock(&registry->lock);

  gboolean any_started = FALSE;
  for(GList *l = ids; l; l = g_list_next(l))
  {
    if(dt_ai_models_download((const char *)l->data, callback, user_data))
      any_started = TRUE;
  }
  g_list_free_full(ids, g_free);
  return any_started;
}

gboolean dt_ai_models_download_all(dt_ai_progress_callback callback,
                                   gpointer user_data)
{
  dt_ai_registry_t *registry = darktable.ai_registry;
  if(!registry)
    return FALSE;

  // collect ids while holding lock, then download without lock
  GList *ids = NULL;
  g_mutex_lock(&registry->lock);
  for(GList *l = registry->models; l; l = g_list_next(l))
  {
    dt_ai_model_t *model = (dt_ai_model_t *)l->data;
    if(model->status == DT_AI_MODEL_NOT_DOWNLOADED)
      ids = g_list_prepend(ids, g_strdup(model->id));
  }
  g_mutex_unlock(&registry->lock);

  gboolean any_started = FALSE;
  for(GList *l = ids; l; l = g_list_next(l))
  {
    if(dt_ai_models_download((const char *)l->data, callback, user_data))
      any_started = TRUE;
  }
  g_list_free_full(ids, g_free);
  return any_started;
}
#endif // HAVE_AI_DOWNLOAD

static gboolean _rmdir_recursive(const char *path)
{
  if(!g_file_test(path, G_FILE_TEST_IS_DIR))
  {
    g_unlink(path);
    return TRUE;
  }

  GDir *dir = g_dir_open(path, 0, NULL);
  if(!dir)
    return FALSE;

  const gchar *name;
  while((name = g_dir_read_name(dir)))
  {
    char *child = g_build_filename(path, name, NULL);
    if(g_file_test(child, G_FILE_TEST_IS_SYMLINK))
      g_unlink(child); // remove the symlink itself, never follow
    else if(g_file_test(child, G_FILE_TEST_IS_DIR))
      _rmdir_recursive(child);
    else
      g_unlink(child);
    g_free(child);
  }
  g_dir_close(dir);
  return g_rmdir(path) == 0;
}

gboolean dt_ai_models_delete(const char *model_id)
{
  dt_ai_registry_t *registry = darktable.ai_registry;
  if(!registry || !_valid_model_id(model_id))
    return FALSE;

  // check model exists
  g_mutex_lock(&registry->lock);
  dt_ai_model_t *model = _find_model_unlocked(registry, model_id);
  if(!model)
  {
    g_mutex_unlock(&registry->lock);
    return FALSE;
  }
  g_mutex_unlock(&registry->lock);

  char *model_dir = g_build_filename(registry->models_dir, model_id, NULL);
  _rmdir_recursive(model_dir);
  g_free(model_dir);

  dt_ai_backend_cache_invalidate(model_id);

  char *task_copy = NULL;
  g_mutex_lock(&registry->lock);
  model = _find_model_unlocked(registry, model_id);
  if(model)
  {
    model->status = DT_AI_MODEL_NOT_DOWNLOADED;
    model->download_progress = 0.0;
    if(model->task)
      task_copy = g_strdup(model->task);
  }
  g_mutex_unlock(&registry->lock);

  // if deleted model was active, pick a fallback (default preferred)
  if(task_copy)
  {
    char *active = dt_ai_models_get_active_for_task(task_copy);
    if(active && strcmp(active, model_id) == 0)
    {
      g_mutex_lock(&registry->lock);
      const char *fallback = _pick_fallback_active_unlocked(registry, task_copy);
      char *fallback_copy = fallback ? g_strdup(fallback) : NULL;
      g_mutex_unlock(&registry->lock);
      dt_ai_models_set_active_for_task(task_copy, fallback_copy);
      g_free(fallback_copy);
    }
    g_free(active);
    g_free(task_copy);
  }

  return TRUE;
}

// configuration

void dt_ai_models_set_enabled(const char *model_id, gboolean enabled)
{
  dt_ai_registry_t *registry = darktable.ai_registry;
  if(!registry || !model_id)
    return;

  g_mutex_lock(&registry->lock);
  dt_ai_model_t *model = _find_model_unlocked(registry, model_id);
  if(model)
    model->enabled = enabled;
  g_mutex_unlock(&registry->lock);

  if(!model)
    return;

  // persist to config
  char *conf_key = g_strdup_printf("%s%s/enabled", CONF_MODEL_ENABLED_PREFIX, model_id);
  dt_conf_set_bool(conf_key, enabled);
  g_free(conf_key);
}

char *dt_ai_models_get_active_for_task(const char *task)
{
  if(!task || !task[0])
    return NULL;

  // 1. check central config key
  char *conf_key = g_strdup_printf("%s%s", CONF_ACTIVE_MODEL_PREFIX, task);
  if(dt_conf_key_exists(conf_key))
  {
    char *model_id = dt_conf_get_string(conf_key);
    g_free(conf_key);
    if(model_id && model_id[0])
      return model_id;
    g_free(model_id);
    return NULL;
  }
  g_free(conf_key);

  // 2. fall back to the default downloaded model for this task
  if(darktable.ai_registry)
  {
    char *result = NULL;
    g_mutex_lock(&darktable.ai_registry->lock);
    for(GList *l = darktable.ai_registry->models; l; l = g_list_next(l))
    {
      dt_ai_model_t *m = (dt_ai_model_t *)l->data;
      if(m->task && strcmp(m->task, task) == 0
         && m->is_default && m->status == DT_AI_MODEL_DOWNLOADED)
      {
        result = g_strdup(m->id);
        break;
      }
    }
    g_mutex_unlock(&darktable.ai_registry->lock);

    if(result)
    {
      dt_ai_models_set_active_for_task(task, result);
      return result;
    }
  }

  return NULL;
}

const char *dt_ai_model_get_version(const char *model_id)
{
  if(!model_id || !darktable.ai_registry)
    return "0.0";

  const char *result = "0.0";
  g_mutex_lock(&darktable.ai_registry->lock);
  for(GList *l = darktable.ai_registry->models; l; l = g_list_next(l))
  {
    const dt_ai_model_t *m = l->data;
    if(m->id && strcmp(m->id, model_id) == 0)
    {
      if(m->version && m->version[0])
        result = m->version;
      break;
    }
  }
  g_mutex_unlock(&darktable.ai_registry->lock);
  return result;
}

const char *dt_ai_model_get_min_version(const char *model_id)
{
  if(!model_id || !darktable.ai_registry)
    return NULL;

  const char *result = NULL;
  g_mutex_lock(&darktable.ai_registry->lock);
  for(GList *l = darktable.ai_registry->models; l; l = g_list_next(l))
  {
    const dt_ai_model_t *m = l->data;
    if(m->id && strcmp(m->id, model_id) == 0)
    {
      result = m->min_version;
      break;
    }
  }
  g_mutex_unlock(&darktable.ai_registry->lock);
  return result;
}

void dt_ai_models_set_active_for_task(const char *task, const char *model_id)
{
  if(!task || !task[0])
    return;

  char *conf_key = g_strdup_printf("%s%s", CONF_ACTIVE_MODEL_PREFIX, task);
  dt_conf_set_string(conf_key, model_id ? model_id : "");
  g_free(conf_key);
}

char *dt_ai_models_get_path(const char *model_id)
{
  dt_ai_registry_t *registry = darktable.ai_registry;
  if(!registry || !_valid_model_id(model_id))
    return NULL;

  g_mutex_lock(&registry->lock);
  dt_ai_model_t *model = _find_model_unlocked(registry, model_id);
  gboolean downloaded = model && model->status == DT_AI_MODEL_DOWNLOADED;
  g_mutex_unlock(&registry->lock);

  if(!downloaded)
    return NULL;

  return g_build_filename(registry->models_dir, model_id, NULL);
}

void dt_ai_models_get_spatial_dims(const char *model_id,
                                   const char **out_h,
                                   const char **out_w)
{
  dt_ai_registry_t *registry = darktable.ai_registry;
  *out_h = "height";
  *out_w = "width";
  if(!registry || !_valid_model_id(model_id)) return;

  g_mutex_lock(&registry->lock);
  dt_ai_model_t *model = _find_model_unlocked(registry, model_id);
  if(model)
  {
    if(model->spatial_dim_h && model->spatial_dim_h[0])
      *out_h = model->spatial_dim_h;
    if(model->spatial_dim_w && model->spatial_dim_w[0])
      *out_w = model->spatial_dim_w;
  }
  g_mutex_unlock(&registry->lock);
}

// read an optional string from a JSON object, returns
// g_strdup'd copy or NULL if missing/empty
static char *_card_str(JsonObject *obj, const char *key)
{
  if(!obj || !json_object_has_member(obj, key))
    return NULL;
  const char *val = json_object_get_string_member(obj, key);
  return (val && val[0]) ? g_strdup(val) : NULL;
}

dt_ai_model_card_t *dt_ai_models_get_card(const char *model_id)
{
  char *model_path = dt_ai_models_get_path(model_id);
  if(!model_path)
  {
    dt_print(DT_DEBUG_AI,
             "[ai_models] model card: %s not on disk", model_id);
    return NULL;
  }

  char *config_path = g_build_filename(model_path, "config.json", NULL);
  g_free(model_path);

  JsonParser *parser = json_parser_new();
  if(!json_parser_load_from_file(parser, config_path, NULL))
  {
    g_free(config_path);
    g_object_unref(parser);
    return NULL;
  }
  g_free(config_path);

  JsonNode *root = json_parser_get_root(parser);
  if(!root || !JSON_NODE_HOLDS_OBJECT(root))
  {
    g_object_unref(parser);
    return NULL;
  }

  JsonObject *config = json_node_get_object(root);
  dt_ai_model_card_t *card = g_new0(dt_ai_model_card_t, 1);

  // read name from top level
  card->name = _card_str(config, "name");

  // card fields live under the "model_card" nested object
  JsonObject *mc = NULL;
  if(json_object_has_member(config, "model_card"))
  {
    JsonNode *cn = json_object_get_member(config, "model_card");
    if(cn && JSON_NODE_HOLDS_OBJECT(cn))
      mc = json_node_get_object(cn);
  }

  card->long_description = _card_str(mc, "long_description");
  card->scope = _card_str(mc, "scope");
  card->author = _card_str(mc, "author");
  card->source = _card_str(mc, "source");
  card->paper = _card_str(mc, "paper");
  card->license = _card_str(mc, "license");
  card->training_data = _card_str(mc, "training_data");
  card->training_data_license = _card_str(mc, "training_data_license");
  card->notes = _card_str(mc, "notes");

  g_object_unref(parser);
  return card;
}

void dt_ai_model_card_free(dt_ai_model_card_t *card)
{
  if(!card) return;
  g_free(card->name);
  g_free(card->long_description);
  g_free(card->scope);
  g_free(card->author);
  g_free(card->source);
  g_free(card->paper);
  g_free(card->license);
  g_free(card->training_data);
  g_free(card->training_data_license);
  g_free(card->notes);
  g_free(card);
}

dt_ai_environment_t *dt_ai_registry_get_env(void)
{
  dt_ai_registry_t *registry = darktable.ai_registry;
  if(!registry || !registry->ai_enabled)
    return NULL;

  g_mutex_lock(&registry->lock);
  if(!registry->env)
    registry->env = dt_ai_env_init(registry->models_dir);
  g_mutex_unlock(&registry->lock);

  return registry->env;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
