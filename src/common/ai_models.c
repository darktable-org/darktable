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
// Windows doesn't have realpath, use _fullpath instead
static inline char *realpath(const char *path, char *resolved_path)
{
  (void)resolved_path; // ignored, always allocate
  return _fullpath(NULL, path, _MAX_PATH);
}
#endif

// Config keys
#define CONF_AI_ENABLED "plugins/ai/enabled"
#define CONF_AI_REPOSITORY "plugins/ai/repository"
#define CONF_MODEL_ENABLED_PREFIX "plugins/ai/models/"
#define CONF_ACTIVE_MODEL_PREFIX "plugins/ai/models/active/"

// --- Internal Helpers ---

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

// --- Version Helpers ---

#ifdef HAVE_AI_DOWNLOAD
// Curl write callback that appends to a GString
static size_t _curl_write_string(void *ptr, size_t size, size_t nmemb, void *userdata)
{
  GString *buf = (GString *)userdata;
  g_string_append_len(buf, (const char *)ptr, size * nmemb);
  return size * nmemb;
}

/**
 * @brief Extract "major.minor.patch" from darktable_package_version.
 *
 * darktable_package_version looks like "5.5.0+156~gabcdef-dirty" or "5.4.0".
 * We extract the leading "X.Y.Z" portion.
 *
 * @return Newly allocated string "X.Y.Z", or NULL on parse failure.
 */
static char *_get_darktable_version_prefix(void)
{
  int major = 0, minor = 0, patch = 0;
  if(sscanf(darktable_package_version, "%d.%d.%d", &major, &minor, &patch) == 3)
    return g_strdup_printf("%d.%d.%d", major, minor, patch);
  return NULL;
}

/**
 * @brief Query the GitHub API to find the latest model release compatible
 *        with the current darktable version.
 *
 * Looks for releases tagged "vX.Y.Z" or "vX.Y.Z.N" where X.Y.Z matches
 * the darktable version. Returns the tag with the highest revision number.
 *
 * @param repository  GitHub "owner/repo" string
 * @return Newly allocated tag string (e.g. "v5.5.0.2"), or NULL if none found.
 */
static char *_find_latest_compatible_release(const char *repository, char **error_msg)
{
  if(error_msg) *error_msg = NULL;

  char *dt_version = _get_darktable_version_prefix();
  if(!dt_version)
    return NULL;

  char *api_url = g_strdup_printf(
    "https://api.github.com/repos/%s/releases?per_page=100",
    repository);

  CURL *curl = curl_easy_init();
  if(!curl)
  {
    g_free(api_url);
    g_free(dt_version);
    return NULL;
  }
  dt_curl_init(curl, FALSE);

  GString *response = g_string_new(NULL);
  curl_easy_setopt(curl, CURLOPT_URL, api_url);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, _curl_write_string);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

  struct curl_slist *headers = NULL;
  headers = curl_slist_append(headers, "Accept: application/vnd.github+json");
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

  CURLcode res = curl_easy_perform(curl);
  long http_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  g_free(api_url);

  if(res != CURLE_OK || http_code != 200)
  {
    dt_print(
      DT_DEBUG_AI,
      "[ai_models] GitHub API request failed: curl=%d, http=%ld",
      res,
      http_code);
    if(error_msg)
    {
      if(res != CURLE_OK)
        *error_msg = g_strdup_printf(_("network error: %s"), curl_easy_strerror(res));
      else if(http_code == 404)
        *error_msg = g_strdup_printf(_("model repository \"%s\" not found"), repository);
      else if(http_code == 403)
        *error_msg = g_strdup(_("GitHub API rate limit exceeded, try again later"));
      else
        *error_msg = g_strdup_printf(_("GitHub API error (HTTP %ld)"), http_code);
    }
    g_string_free(response, TRUE);
    g_free(dt_version);
    return NULL;
  }

  // Parse JSON array of releases
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
  if(!root || !JSON_NODE_HOLDS_ARRAY(root))
  {
    g_object_unref(parser);
    g_free(dt_version);
    return NULL;
  }

  // Build prefix to match: accept both "vX.Y.Z" and "X.Y.Z" tag formats
  size_t ver_len = strlen(dt_version);

  char *best_tag = NULL;
  int best_revision = -1; // -1 means no revision suffix (e.g., "5.5.0" itself)

  JsonArray *releases = json_node_get_array(root);
  guint len = json_array_get_length(releases);
  for(guint i = 0; i < len; i++)
  {
    JsonNode *node = json_array_get_element(releases, i);
    if(!JSON_NODE_HOLDS_OBJECT(node))
      continue;
    JsonObject *rel = json_node_get_object(node);

    if(!json_object_has_member(rel, "tag_name"))
      continue;
    const char *tag = json_object_get_string_member(rel, "tag_name");
    if(!tag)
      continue;

    // Skip any non-digit prefix (e.g. "v", "release-") to extract X.Y.Z.W
    const char *ver_part = tag;
    while(*ver_part && !g_ascii_isdigit(*ver_part))
      ver_part++;
    if(!*ver_part)
      continue;

    if(strncmp(ver_part, dt_version, ver_len) != 0)
      continue;

    // Tag matches version prefix. Check what follows:
    // "X.Y.Z" (exact) -> revision = 0
    // "X.Y.Z.N"       -> revision = N
    const char *suffix = ver_part + ver_len;
    int revision = 0;
    if(suffix[0] == '\0')
    {
      revision = 0;
    }
    else if(suffix[0] == '.' && suffix[1] >= '0' && suffix[1] <= '9')
    {
      revision = atoi(suffix + 1);
    }
    else
    {
      continue; // doesn't match pattern
    }

    if(revision > best_revision)
    {
      best_revision = revision;
      g_free(best_tag);
      best_tag = g_strdup(tag);
    }
  }

  g_free(dt_version);
  g_object_unref(parser);

  if(best_tag)
    dt_print(DT_DEBUG_AI, "[ai_models] Found compatible release: %s", best_tag);
  else
    dt_print(
      DT_DEBUG_AI,
      "[ai_models] No compatible release found for darktable %s",
      darktable_package_version);

  return best_tag;
}

/**
 * @brief Fetch the SHA256 digest for a release asset from the GitHub API.
 *
 * Queries /repos/{repo}/releases/tags/{tag}, iterates the assets array,
 * and returns the "digest" field for the asset whose "name" matches.
 *
 * @param repository  GitHub "owner/repo" string
 * @param release_tag Release tag (e.g. "5.5.0.1")
 * @param asset_name  Asset filename (e.g. "denoise-nafnet.zip")
 * @return Newly allocated string "sha256:...", or NULL if not found.
 */
static char *_fetch_asset_digest(
  const char *repository,
  const char *release_tag,
  const char *asset_name)
{
  char *api_url = g_strdup_printf(
    "https://api.github.com/repos/%s/releases/tags/%s",
    repository,
    release_tag);

  CURL *curl = curl_easy_init();
  if(!curl)
  {
    g_free(api_url);
    return NULL;
  }
  dt_curl_init(curl, FALSE);

  GString *response = g_string_new(NULL);
  curl_easy_setopt(curl, CURLOPT_URL, api_url);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, _curl_write_string);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

  struct curl_slist *headers = NULL;
  headers = curl_slist_append(headers, "Accept: application/vnd.github+json");
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

  CURLcode res = curl_easy_perform(curl);
  long http_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  g_free(api_url);

  if(res != CURLE_OK || http_code != 200)
  {
    dt_print(
      DT_DEBUG_AI,
      "[ai_models] Failed to fetch release metadata: curl=%d, http=%ld",
      res,
      http_code);
    g_string_free(response, TRUE);
    return NULL;
  }

  // Parse the release JSON object
  JsonParser *parser = json_parser_new();
  if(!json_parser_load_from_data(parser, response->str, response->len, NULL))
  {
    g_object_unref(parser);
    g_string_free(response, TRUE);
    return NULL;
  }
  g_string_free(response, TRUE);

  JsonNode *root = json_parser_get_root(parser);
  if(!root || !JSON_NODE_HOLDS_OBJECT(root))
  {
    g_object_unref(parser);
    return NULL;
  }

  JsonObject *release = json_node_get_object(root);
  if(!json_object_has_member(release, "assets"))
  {
    g_object_unref(parser);
    return NULL;
  }

  char *digest = NULL;
  JsonArray *assets = json_object_get_array_member(release, "assets");
  guint len = json_array_get_length(assets);
  for(guint i = 0; i < len; i++)
  {
    JsonNode *node = json_array_get_element(assets, i);
    if(!JSON_NODE_HOLDS_OBJECT(node))
      continue;
    JsonObject *asset_obj = json_node_get_object(node);

    if(!json_object_has_member(asset_obj, "name"))
      continue;
    const char *name = json_object_get_string_member(asset_obj, "name");
    if(g_strcmp0(name, asset_name) != 0)
      continue;

    if(json_object_has_member(asset_obj, "digest"))
    {
      const char *d = json_object_get_string_member(asset_obj, "digest");
      if(d && g_str_has_prefix(d, "sha256:"))
      {
        digest = g_strdup(d);
        dt_print(DT_DEBUG_AI, "[ai_models] Asset %s digest: %s", asset_name, digest);
      }
    }
    break;
  }

  g_object_unref(parser);

  if(!digest)
    dt_print(
      DT_DEBUG_AI,
      "[ai_models] No digest found for asset %s in release %s",
      asset_name,
      release_tag);

  return digest;
}
#endif /* HAVE_AI_DOWNLOAD */

// --- Core API ---

dt_ai_registry_t *dt_ai_models_init(void)
{
  dt_ai_registry_t *registry = g_new0(dt_ai_registry_t, 1);
  g_mutex_init(&registry->lock);

  // Set up directories
  char configdir[PATH_MAX] = {0};
  char cachedir[PATH_MAX] = {0};
  dt_loc_get_user_config_dir(configdir, sizeof(configdir));
  dt_loc_get_user_cache_dir(cachedir, sizeof(cachedir));

  // Models go alongside darktable config (respects --configdir).
  // On Linux: ~/.config/darktable/models
  // On Windows: %APPDATA%\darktable\models
  registry->models_dir = g_build_filename(configdir, "models", NULL);
  registry->cache_dir = g_build_filename(cachedir, "ai_downloads", NULL);

  // Ensure directories exist
  _ensure_directory(registry->models_dir);
  _ensure_directory(registry->cache_dir);

  // Load settings from config
  registry->ai_enabled = dt_conf_get_bool(CONF_AI_ENABLED);

  char *provider_str = dt_conf_get_string(DT_AI_CONF_PROVIDER);
  registry->provider = dt_ai_provider_from_string(provider_str);
  g_free(provider_str);

  dt_print(
    DT_DEBUG_AI,
    "[ai_models] Initialized: models_dir=%s, cache_dir=%s",
    registry->models_dir,
    registry->cache_dir);

  return registry;
}

static dt_ai_model_t *_parse_model_json(JsonObject *obj)
{
  if(!json_object_has_member(obj, "id") || !json_object_has_member(obj, "name"))
    return NULL;

  dt_ai_model_t *model = _model_new();
  model->id = g_strdup(json_object_get_string_member(obj, "id"));
  model->name = g_strdup(json_object_get_string_member(obj, "name"));

  if(json_object_has_member(obj, "description"))
    model->description = g_strdup(json_object_get_string_member(obj, "description"));
  if(json_object_has_member(obj, "task"))
    model->task = g_strdup(json_object_get_string_member(obj, "task"));
  if(json_object_has_member(obj, "github_asset"))
    model->github_asset = g_strdup(json_object_get_string_member(obj, "github_asset"));
  if(json_object_has_member(obj, "checksum"))
    model->checksum = g_strdup(json_object_get_string_member(obj, "checksum"));
  if(json_object_has_member(obj, "default"))
    model->is_default = json_object_get_boolean_member(obj, "default");

  return model;
}

gboolean dt_ai_models_load_registry(dt_ai_registry_t *registry)
{
  if(!registry)
    return FALSE;

  // Find the registry JSON file in the data directory
  char datadir[PATH_MAX] = {0};
  dt_loc_get_datadir(datadir, sizeof(datadir));
  char *registry_path = g_build_filename(datadir, "ai_models.json", NULL);

  if(!g_file_test(registry_path, G_FILE_TEST_EXISTS))
  {
    dt_print(DT_DEBUG_AI, "[ai_models] Registry file not found: %s", registry_path);
    g_free(registry_path);
    return FALSE;
  }

  GError *error = NULL;
  JsonParser *parser = json_parser_new();

  if(!json_parser_load_from_file(parser, registry_path, &error))
  {
    dt_print(
      DT_DEBUG_AI,
      "[ai_models] Failed to parse registry: %s",
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
    dt_print(DT_DEBUG_AI, "[ai_models] Registry root is not an object");
    g_object_unref(parser);
    g_free(registry_path);
    return FALSE;
  }

  JsonObject *root_obj = json_node_get_object(root);

  g_mutex_lock(&registry->lock);

  // Clear existing models
  g_list_free_full(registry->models, (GDestroyNotify)_model_free);
  registry->models = NULL;

  // Parse repository - config overrides JSON default
  g_free(registry->repository);
  registry->repository = NULL;

  if(dt_conf_key_exists(CONF_AI_REPOSITORY))
    registry->repository = dt_conf_get_string(CONF_AI_REPOSITORY);

  dt_print(
    DT_DEBUG_AI,
    "[ai_models] Using repository: %s",
    registry->repository ? registry->repository : "(none)");

  // Parse models array
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
        // Load enabled state from user config
        char *conf_key
          = g_strdup_printf("%s%s/enabled", CONF_MODEL_ENABLED_PREFIX, model->id);
        if(dt_conf_key_exists(conf_key))
          model->enabled = dt_conf_get_bool(conf_key);
        g_free(conf_key);

        registry->models = g_list_prepend(registry->models, model);
        dt_print(
          DT_DEBUG_AI,
          "[ai_models] Loaded model: %s (%s)",
          model->name,
          model->id);
      }
    }
  }

  // Reverse to restore original JSON order (we used prepend for O(1) insertion)
  registry->models = g_list_reverse(registry->models);

  const int model_count = g_list_length(registry->models);

  g_mutex_unlock(&registry->lock);

  dt_print(
    DT_DEBUG_AI,
    "[ai_models] Registry loaded: %d models from %s",
    model_count,
    registry_path);

  g_object_unref(parser);
  g_free(registry_path);

  // Check which models are actually downloaded
  dt_ai_models_refresh_status(registry);

  return TRUE;
}

// Validate that a model_id is a plain directory name with no path separators
// or ".." components that could escape the models directory.
static gboolean _valid_model_id(const char *model_id);
static dt_ai_model_t *_find_model_unlocked(dt_ai_registry_t *registry,
                                            const char *model_id);

// Parse a local model's config.json into a dt_ai_model_t.
// Uses directory name as fallback for id/name. No github_asset or checksum.
static dt_ai_model_t *_parse_local_model_config(const char *config_path,
                                                 const char *dir_name)
{
  JsonParser *parser = json_parser_new();
  GError *error = NULL;

  if(!json_parser_load_from_file(parser, config_path, &error))
  {
    dt_print(DT_DEBUG_AI, "[ai_models] Failed to parse %s: %s",
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

  // No github_asset, no checksum — local-only model
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

void dt_ai_models_refresh_status(dt_ai_registry_t *registry)
{
  if(!registry)
    return;

  g_mutex_lock(&registry->lock);

  // --- Remove previously-discovered local models (no github_asset) ---
  // These will be re-discovered from disk below if still present.
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

  // --- Pass 1: Update status for registry models ---
  for(GList *l2 = registry->models; l2; l2 = g_list_next(l2))
  {
    dt_ai_model_t *model = (dt_ai_model_t *)l2->data;

    // Skip models with invalid IDs (path traversal protection)
    if(!_valid_model_id(model->id))
      continue;

    // Check if model directory exists and contains required files
    char *model_dir = g_build_filename(registry->models_dir, model->id, NULL);
    char *config_path = g_build_filename(model_dir, "config.json", NULL);

    if(g_file_test(model_dir, G_FILE_TEST_IS_DIR)
       && g_file_test(config_path, G_FILE_TEST_EXISTS))
    {
      model->status = DT_AI_MODEL_DOWNLOADED;
    }
    else
    {
      model->status = DT_AI_MODEL_NOT_DOWNLOADED;
    }

    g_free(config_path);
    g_free(model_dir);
  }

  // --- Pass 2: Discover locally-installed models not in registry ---
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

        // Skip if already in registry (e.g. downloaded via ai_models.json)
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
                     "[ai_models] Discovered local model: %s (%s)",
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

void dt_ai_models_cleanup(dt_ai_registry_t *registry)
{
  if(!registry)
    return;

  g_mutex_lock(&registry->lock);
  g_list_free_full(registry->models, (GDestroyNotify)_model_free);
  registry->models = NULL;
  g_mutex_unlock(&registry->lock);

  g_mutex_clear(&registry->lock);

  g_free(registry->repository);
  g_free(registry->models_dir);
  g_free(registry->cache_dir);
  g_free(registry);
}

// --- Model Access ---

// Internal: find model by ID without locking. Caller must hold registry->lock.
// Returns direct pointer to registry-owned model (not a copy).
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

int dt_ai_models_get_count(dt_ai_registry_t *registry)
{
  if(!registry)
    return 0;
  g_mutex_lock(&registry->lock);
  int count = g_list_length(registry->models);
  g_mutex_unlock(&registry->lock);
  return count;
}

dt_ai_model_t *dt_ai_models_get_by_index(dt_ai_registry_t *registry, int index)
{
  if(!registry || index < 0)
    return NULL;
  g_mutex_lock(&registry->lock);
  dt_ai_model_t *model = g_list_nth_data(registry->models, index);
  dt_ai_model_t *copy = _model_copy(model);
  g_mutex_unlock(&registry->lock);
  return copy;
}

dt_ai_model_t *dt_ai_models_get_by_id(dt_ai_registry_t *registry, const char *model_id)
{
  if(!registry || !model_id)
    return NULL;
  g_mutex_lock(&registry->lock);
  dt_ai_model_t *model = _find_model_unlocked(registry, model_id);
  dt_ai_model_t *copy = _model_copy(model);
  g_mutex_unlock(&registry->lock);
  return copy;
}

#ifdef HAVE_AI_DOWNLOAD
// --- Download Implementation ---

typedef struct dt_ai_download_data_t
{
  dt_ai_registry_t *registry;
  char *model_id; // Owned copy of model ID (safe to use without lock)
  dt_ai_progress_callback callback;
  gpointer user_data;
  FILE *file;
  const gboolean *cancel_flag; // Optional: set to non-NULL to enable cancellation
} dt_ai_download_data_t;

static size_t _curl_write_callback(void *ptr, size_t size, size_t nmemb, void *data)
{
  dt_ai_download_data_t *dl = (dt_ai_download_data_t *)data;
  return fwrite(ptr, size, nmemb, dl->file);
}

static int _curl_progress_callback(
  void *clientp,
  curl_off_t dltotal,
  curl_off_t dlnow,
  curl_off_t ultotal,
  curl_off_t ulnow)
{
  dt_ai_download_data_t *dl = (dt_ai_download_data_t *)clientp;

  // Check for cancellation
  if(dl->cancel_flag && g_atomic_int_get(dl->cancel_flag))
    return 1; // Non-zero aborts the transfer

  if(dltotal > 0)
  {
    double progress = (double)dlnow / (double)dltotal;

    // Update model progress under lock
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

static gboolean _verify_checksum(const char *filepath, const char *expected)
{
  if(!expected || !g_str_has_prefix(expected, "sha256:"))
  {
    dt_print(DT_DEBUG_AI, "[ai_models] No valid checksum provided - rejecting download");
    return FALSE; // Reject files without a valid checksum
  }

  const char *expected_hash = expected + 7; // Skip "sha256:"

  GChecksum *checksum = g_checksum_new(G_CHECKSUM_SHA256);
  if(!checksum)
    return FALSE;

  // Stream file in chunks to avoid loading entire file into memory
  FILE *f = g_fopen(filepath, "rb");
  if(!f)
  {
    dt_print(DT_DEBUG_AI, "[ai_models] Failed to open file for checksum: %s", filepath);
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
    dt_print(
      DT_DEBUG_AI,
      "[ai_models] Checksum mismatch: expected %s, got %s",
      expected_hash,
      computed);
  }

  g_checksum_free(checksum);
  return match;
}
#endif /* HAVE_AI_DOWNLOAD */

static gboolean _extract_zip(const char *zippath, const char *destdir)
{
  struct archive *a = archive_read_new();
  struct archive *ext = archive_write_disk_new();
  struct archive_entry *entry;
  int r;
  gboolean success = TRUE;

  archive_read_support_format_zip(a);
  archive_write_disk_set_options(
    ext,
    ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM | ARCHIVE_EXTRACT_SECURE_SYMLINKS
      | ARCHIVE_EXTRACT_SECURE_NODOTDOT);

  if((r = archive_read_open_filename(a, zippath, 10240)) != ARCHIVE_OK)
  {
    dt_print(
      DT_DEBUG_AI,
      "[ai_models] Failed to open archive: %s",
      archive_error_string(a));
    archive_read_free(a);
    archive_write_free(ext);
    return FALSE;
  }

  _ensure_directory(destdir);

  // Resolve destdir to a canonical path for path traversal validation
  char *real_destdir = realpath(destdir, NULL);
  if(!real_destdir)
  {
    dt_print(DT_DEBUG_AI, "[ai_models] Failed to resolve destdir: %s", destdir);
    archive_read_close(a);
    archive_read_free(a);
    archive_write_free(ext);
    return FALSE;
  }

  const size_t destdir_len = strlen(real_destdir);

  while(archive_read_next_header(a, &entry) == ARCHIVE_OK)
  {
    const char *entry_name = archive_entry_pathname(entry);

    // Reject entries with path traversal components
    if(g_strstr_len(entry_name, -1, "..") != NULL)
    {
      dt_print(
        DT_DEBUG_AI,
        "[ai_models] Skipping suspicious archive entry: %s",
        entry_name);
      continue;
    }

    // Build full path in destination
    char *full_path = g_build_filename(real_destdir, entry_name, NULL);

    // Verify the resolved path is within destdir
    char *real_full_path = realpath(full_path, NULL);
    // For new files, realpath returns NULL; check the parent directory instead
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
        dt_print(DT_DEBUG_AI, "[ai_models] Path traversal blocked: %s", entry_name);
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
        dt_print(DT_DEBUG_AI, "[ai_models] Path traversal blocked: %s", entry_name);
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
          dt_print(DT_DEBUG_AI, "[ai_models] Write error: %s", archive_error_string(ext));
          success = FALSE;
          break;
        }
      }
      if(archive_write_finish_entry(ext) != ARCHIVE_OK)
        success = FALSE;
    }
    else
    {
      dt_print(
        DT_DEBUG_AI,
        "[ai_models] Write header error: %s",
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

// Install a local .dtmodel file (zip archive) into the models directory.
// Returns error message (caller must free) or NULL on success.
char *dt_ai_models_install_local(dt_ai_registry_t *registry, const char *filepath)
{
  if(!registry || !filepath)
    return g_strdup(_("invalid parameters"));

  if(!g_file_test(filepath, G_FILE_TEST_IS_REGULAR))
    return g_strdup_printf(_("file not found: %s"), filepath);

  if(!_extract_zip(filepath, registry->models_dir))
    return g_strdup(_("failed to extract model archive"));

  // Rescan models directory to pick up newly installed model
  dt_ai_models_refresh_status(registry);

  dt_print(DT_DEBUG_AI, "[ai_models] Model installed from: %s", filepath);

  return NULL; // Success
}

#ifdef HAVE_AI_DOWNLOAD
// Synchronous download - returns error message or NULL on success
char *dt_ai_models_download_sync(
  dt_ai_registry_t *registry,
  const char *model_id,
  dt_ai_progress_callback callback,
  gpointer user_data,
  const gboolean *cancel_flag)
{
  dt_print(
    DT_DEBUG_AI,
    "[ai_models] Download requested for: %s",
    model_id ? model_id : "(null)");

  if(!registry || !model_id)
    return g_strdup(_("invalid parameters"));

  // Lock once to validate, copy immutable fields, and set status
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

  // Validate asset filename: reject path separators and query strings
  if(
    strchr(model->github_asset, '/') || strchr(model->github_asset, '\\')
    || strchr(model->github_asset, '?') || strchr(model->github_asset, '#')
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

  // Copy fields we need outside the lock (repository can be replaced by reload)
  char *asset = g_strdup(model->github_asset);
  char *checksum_copy = g_strdup(model->checksum);
  char *repository = g_strdup(registry->repository);
  g_mutex_unlock(&registry->lock);

// Helper macro: set model status under lock and return error
// Uses _find_model_unlocked to avoid keeping a stale pointer
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

  // Validate repository format (must be "owner/repo" with safe characters)
  if(
    !repository
    || !g_regex_match_simple("^[a-zA-Z0-9._-]+/[a-zA-Z0-9._-]+$", repository, 0, 0))
  {
    SET_STATUS_AND_RETURN(DT_AI_MODEL_ERROR, g_strdup(_("invalid repository format")));
  }

  {
    char *ver = _get_darktable_version_prefix();
    dt_print(DT_DEBUG_AI, "[ai_models] Repository: %s", repository);
    dt_print(
      DT_DEBUG_AI,
      "[ai_models] darktable version: %s (full: %s)",
      ver ? ver : "unknown",
      darktable_package_version);
    g_free(ver);
  }

  // Find the latest compatible release for this darktable version
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
        _("no compatible AI model release found for darktable %s"),
        dt_ver ? dt_ver : darktable_package_version);
      g_free(dt_ver);
      SET_STATUS_AND_RETURN(DT_AI_MODEL_ERROR, err);
    }
  }

  // Fetch SHA256 digest from GitHub Releases API if not already known
  if(!checksum_copy || !g_str_has_prefix(checksum_copy, "sha256:"))
  {
    g_free(checksum_copy);
    checksum_copy = _fetch_asset_digest(repository, release_tag, asset);
    if(!checksum_copy)
      dt_print(
        DT_DEBUG_AI,
        "[ai_models] WARNING: could not obtain checksum for %s — "
        "download will proceed without integrity verification",
        asset);
  }

  // Build GitHub download URL using local copies (not model pointer)
  char *url = g_strdup_printf(
    "https://github.com/%s/releases/download/%s/%s",
    repository,
    release_tag,
    asset);
  g_free(release_tag);

  if(!url)
  {
    SET_STATUS_AND_RETURN(DT_AI_MODEL_ERROR, g_strdup(_("failed to build download URL")));
  }

  dt_print(DT_DEBUG_AI, "[ai_models] Downloading: %s", url);

  // Prepare download path using local copy
  char *download_path = g_build_filename(registry->cache_dir, asset, NULL);

  FILE *file = g_fopen(download_path, "wb");
  if(!file)
  {
    char *err = g_strdup_printf(_("failed to create file: %s"), download_path);
    g_free(download_path);
    g_free(url);
    SET_STATUS_AND_RETURN(DT_AI_MODEL_ERROR, err);
  }

  // Set up download data (uses model_id copy, not model pointer)
  dt_ai_download_data_t dl = {
    .registry = registry,
    .model_id = (char *)model_id,
    .callback = callback,
    .user_data = user_data,
    .file = file,
    .cancel_flag = cancel_flag};

  // Initialize curl
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
    // Check HTTP response code
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    if(http_code != 200)
    {
      error = g_strdup_printf(_("HTTP error: %ld"), http_code);
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

  // Verify checksum if available (fetched from GitHub API or stored in registry)
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
    dt_print(
      DT_DEBUG_AI,
      "[ai_models] WARNING: no checksum available for %s — skipping verification",
      asset);
  }

  // Extract to models directory (ZIP already contains model_id folder)
  if(!_extract_zip(download_path, registry->models_dir))
  {
    g_unlink(download_path);
    g_free(download_path);
    SET_STATUS_AND_RETURN(DT_AI_MODEL_ERROR, g_strdup(_("failed to extract archive")));
  }

  // Clean up downloaded zip
  g_unlink(download_path);
  g_free(download_path);

  // Mark success
  g_mutex_lock(&registry->lock);
  dt_ai_model_t *m = _find_model_unlocked(registry, model_id);
  if(m)
  {
    m->status = DT_AI_MODEL_DOWNLOADED;
    m->download_progress = 1.0;
  }
  g_mutex_unlock(&registry->lock);

  dt_print(DT_DEBUG_AI, "[ai_models] Download complete: %s", model_id);

  // Final callback
  if(callback)
    callback(model_id, 1.0, user_data);

  g_free(asset);
  g_free(checksum_copy);
  g_free(repository);

#undef SET_STATUS_AND_RETURN

  return NULL; // Success
}

// Wrapper that returns boolean for compatibility
gboolean dt_ai_models_download(
  dt_ai_registry_t *registry,
  const char *model_id,
  dt_ai_progress_callback callback,
  gpointer user_data)
{
  char *error = dt_ai_models_download_sync(registry, model_id, callback, user_data, NULL);
  if(error)
  {
    dt_print(DT_DEBUG_AI, "[ai_models] Download error: %s", error);
    g_free(error);
    return FALSE;
  }
  return TRUE;
}

gboolean dt_ai_models_download_default(
  dt_ai_registry_t *registry,
  dt_ai_progress_callback callback,
  gpointer user_data)
{
  if(!registry)
    return FALSE;

  // Collect IDs while holding lock, then download without lock
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
    if(dt_ai_models_download(registry, (const char *)l->data, callback, user_data))
      any_started = TRUE;
  }
  g_list_free_full(ids, g_free);
  return any_started;
}

gboolean dt_ai_models_download_all(
  dt_ai_registry_t *registry,
  dt_ai_progress_callback callback,
  gpointer user_data)
{
  if(!registry)
    return FALSE;

  // Collect IDs while holding lock, then download without lock
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
    if(dt_ai_models_download(registry, (const char *)l->data, callback, user_data))
      any_started = TRUE;
  }
  g_list_free_full(ids, g_free);
  return any_started;
}
#endif /* HAVE_AI_DOWNLOAD */

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
      g_unlink(child); // Remove the symlink itself, never follow
    else if(g_file_test(child, G_FILE_TEST_IS_DIR))
      _rmdir_recursive(child);
    else
      g_unlink(child);
    g_free(child);
  }
  g_dir_close(dir);
  return g_rmdir(path) == 0;
}

gboolean dt_ai_models_delete(dt_ai_registry_t *registry, const char *model_id)
{
  if(!registry || !_valid_model_id(model_id))
    return FALSE;

  // Check model exists
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

  // Clear active status if this was the active model for its task
  if(task_copy)
  {
    char *active = dt_ai_models_get_active_for_task(task_copy);
    if(active && strcmp(active, model_id) == 0)
      dt_ai_models_set_active_for_task(task_copy, NULL);
    g_free(active);
    g_free(task_copy);
  }

  return TRUE;
}

// --- Configuration ---

void dt_ai_models_set_enabled(
  dt_ai_registry_t *registry,
  const char *model_id,
  gboolean enabled)
{
  if(!registry || !model_id)
    return;

  g_mutex_lock(&registry->lock);
  dt_ai_model_t *model = _find_model_unlocked(registry, model_id);
  if(model)
    model->enabled = enabled;
  g_mutex_unlock(&registry->lock);

  if(!model)
    return;

  // Persist to config
  char *conf_key = g_strdup_printf("%s%s/enabled", CONF_MODEL_ENABLED_PREFIX, model_id);
  dt_conf_set_bool(conf_key, enabled);
  g_free(conf_key);
}

// Legacy consumer config keys, used for first-run migration only
static const char *_legacy_task_keys[][2] = {
  {"mask", "plugins/darkroom/masks/object/model"},
  {"denoise", "plugins/lighttable/denoise_ai/model"},
  {NULL, NULL}
};

char *dt_ai_models_get_active_for_task(const char *task)
{
  if(!task || !task[0])
    return NULL;

  // 1. Check central config key
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

  // 2. Fall back to legacy consumer config key (first-run migration)
  for(int i = 0; _legacy_task_keys[i][0]; i++)
  {
    if(strcmp(_legacy_task_keys[i][0], task) == 0)
    {
      const char *legacy_key = _legacy_task_keys[i][1];
      if(dt_conf_key_exists(legacy_key))
      {
        char *model_id = dt_conf_get_string(legacy_key);
        if(model_id && model_id[0])
        {
          // Migrate: persist to central key so we don't check legacy again
          dt_ai_models_set_active_for_task(task, model_id);
          return model_id;
        }
        g_free(model_id);
      }
      break;
    }
  }

  // 3. Fall back to the default downloaded model for this task
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

void dt_ai_models_set_active_for_task(const char *task, const char *model_id)
{
  if(!task || !task[0])
    return;

  char *conf_key = g_strdup_printf("%s%s", CONF_ACTIVE_MODEL_PREFIX, task);
  dt_conf_set_string(conf_key, model_id ? model_id : "");
  g_free(conf_key);
}

char *dt_ai_models_get_path(dt_ai_registry_t *registry, const char *model_id)
{
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

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
