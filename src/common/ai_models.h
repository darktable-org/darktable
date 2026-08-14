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

#pragma once

#include <glib.h>
#include "ai/backend.h"

/**
 * @brief Model download/availability status
 */
typedef enum dt_ai_model_status_t
{
  DT_AI_MODEL_NOT_DOWNLOADED = 0,
  DT_AI_MODEL_DOWNLOADING,
  DT_AI_MODEL_DOWNLOADED,
  DT_AI_MODEL_UPDATE_AVAILABLE,
  DT_AI_MODEL_UPDATE_REQUIRED,
  DT_AI_MODEL_ERROR,
} dt_ai_model_status_t;

/**
 * @brief Information about a single AI model
 */
typedef struct dt_ai_model_t
{
  char *id;              // Unique identifier (e.g. "mask-object-segnext-b2hq")
  char *name;            // Display name
  char *description;     // Short description
  char *task;            // Task type: "denoise", "upscale", etc.
  char *github_asset;    // Asset filename in GitHub release
  char *repository;      // "owner/repo" this model comes from; NULL for
                         // the repository configured globally
  char *checksum;        // checksum string, format "<algo>:<hex>"
                         // (today only "sha256:..." is produced by the
                         // GitHub asset digest API; parser tolerates
                         // other algos but only sha256 is verified)
  char *version;         // actual version from model's config.json
  char *min_version;     // minimum required version from registry
  char *spatial_dim_h;   // symbolic name of height dimension (default "height")
  char *spatial_dim_w;   // symbolic name of width dimension (default "width")
  gboolean is_default;   // TRUE if model is a default model for its task
  gboolean from_catalog;  // TRUE when parsed from the bundled
                            // ai_models.json; FALSE when found on disk or
                            // registered from a repository listing
  gboolean from_file;    // TRUE when installed from a local .dtmodel; there
                         // is no repository to go back to for updates
  gboolean enabled;      // User preference (stored in darktablerc)
  dt_ai_model_status_t status;
  double download_progress;  // 0.0 to 1.0 during download
} dt_ai_model_t;

/**
 * @brief Progress callback for download operations
 * @param model_id The model being downloaded
 * @param progress Download progress 0.0 to 1.0
 * @param user_data User-provided data
 */
typedef void (*dt_ai_progress_callback)(const char *model_id,
                                        const double progress,
                                        gpointer user_data);

// The AI models registry is a per-session singleton owned by
// darktable.ai_registry. It is created by dt_ai_models_init() and
// destroyed by dt_ai_models_cleanup(); the struct itself (private
// mutex + in-memory model list) is defined only in ai_models.c so
// callers can't deadlock on the lock or race-iterate the list. Every
// function below operates on that singleton implicitly — there is no
// registry handle to pass around. All are safe to call before init or
// on a non-AI build, where they no-op (returning FALSE/0/NULL).

// --- Registry state accessors ---
// thin, locked accessors for the few fields external callers need to
// touch. all other registry state is accessed via the model APIs below

/**
 * @brief TRUE if AI is currently enabled (global toggle in prefs).
 *        Safe to call from any thread.
 */
gboolean dt_ai_registry_is_enabled(void);

/**
 * @brief Set the global AI-enabled state in the registry.
 *        Does NOT write to darktablerc — caller must persist the
 *        preference separately. Safe to call from any thread.
 */
void dt_ai_registry_set_enabled(const gboolean enabled);

/**
 * @brief Set the execution provider in the registry.
 *        Does NOT write to darktablerc — caller must persist the
 *        preference separately. Safe to call from any thread.
 */
void dt_ai_registry_set_provider(const dt_ai_provider_t provider);

// --- Core API ---

/**
 * @brief Initialize the AI models registry singleton.
 *        On return darktable.ai_registry points at the new registry
 *        (always created). When AI is enabled at startup this also sets
 *        up the model/cache directories.
 * @return TRUE if ready; FALSE if directory setup failed (the registry
 *         still exists but downloads/scans will not work)
 */
gboolean dt_ai_models_init(void);

/**
 * @brief Load model registry from `ai_models.json` bundled with the
 *        darktable installation (looked up under DATADIR). There is
 *        no caller-supplied path — the registry source is fixed by
 *        the install layout.
 * @return TRUE on success
 */
gboolean dt_ai_models_load_registry(void);

/**
 * @brief Lazy-initialize AI registry at runtime
 *
 * When AI was disabled at startup, the registry is created
 * without directories or models. This function performs
 * the deferred initialization: creates directories, reads
 * provider config, loads the model registry and scans for
 * locally installed models.
 *
 * Safe to call multiple times — no-ops if already initialized.
 */
void dt_ai_models_init_lazy(void);

/**
 * @brief Scan models directory and update download status
 */
void dt_ai_models_refresh_status(void);

/**
 * @brief Check for model updates by fetching versions.json from the
 *        remote repository, and update each model's status:
 *
 *        - installed >= remote → DT_AI_MODEL_DOWNLOADED (no change)
 *        - installed <  remote AND installed >= min_version
 *          → DT_AI_MODEL_UPDATE_AVAILABLE
 *        - installed <  min_version → DT_AI_MODEL_UPDATE_REQUIRED
 *
 *        UPDATE_REQUIRED means darktable cannot use the installed
 *        model with the current code; UPDATE_AVAILABLE is a soft
 *        nudge — the installed model still works.
 */
void dt_ai_models_check_updates(void);

/**
 * @brief Clean up and free the registry singleton (sets it to NULL).
 */
void dt_ai_models_cleanup(void);

// --- Model Access ---
// All get functions return a COPY of the model. Caller must free with dt_ai_model_free().
// This ensures thread safety: no lock is exposed to callers, preventing deadlocks.

/**
 * @brief Free a model copy returned by get_by_index/get_by_id
 * @param model The model to free (may be NULL)
 */
void dt_ai_model_free(dt_ai_model_t *model);

/**
 * @brief Get number of models in registry
 * @return Number of models
 */
int dt_ai_models_get_count(void);

/**
 * @brief Get model by index (returns a copy, caller must free with dt_ai_model_free)
 * @param index Index 0 to count-1
 * @return Model copy (caller owns), or NULL
 */
dt_ai_model_t *dt_ai_models_get_by_index(const int index);

/**
 * @brief Get model by ID (returns a copy, caller must free with dt_ai_model_free)
 * @param model_id The unique model ID
 * @return Model copy (caller owns), or NULL
 */
dt_ai_model_t *dt_ai_models_get_by_id(const char *model_id);

// --- Install Operations ---

/**
 * @brief Install a model from a local .dtmodel file
 * @param filepath Path to the .dtmodel file (zip archive)
 * @return Error message (caller must free) or NULL on success
 */
char *dt_ai_models_install_local(const char *filepath);

// --- Provenance ---
// a model keeps its recorded publisher whether or not this build can
// download, so these stay outside HAVE_AI_DOWNLOAD

/**
 * @brief The official repository, for display
 *
 * plugins/ai/repository, which darktableconfig.xml defaults to
 * darktable-org/darktable-ai. An installation pointed at a mirror is
 * still being served official models, so that mirror is what "official"
 * means for it.
 *
 * @return Borrowed "owner/repo", valid for the registry's lifetime, or
 *         NULL before the registry exists
 */
const char *dt_ai_models_official_repository(void);

/**
 * @brief TRUE if `repository` is this installation's official repository
 */
gboolean dt_ai_models_is_official_repository(const char *repository);

#ifdef HAVE_AI_DOWNLOAD
// --- Download Operations ---

/**
 * @brief Download a specific model synchronously
 * @param model_id The model to download
 * @param callback Progress callback (may be NULL)
 * @param user_data Data for callback
 * @param cancel_flag Pointer to boolean checked for cancellation (may be NULL)
 * @return Error message (caller must free) or NULL on success
 */
char *dt_ai_models_download_sync(const char *model_id,
                                 dt_ai_progress_callback callback,
                                 gpointer user_data,
                                 const gboolean *cancel_flag);

/**
 * @brief Download a specific model (convenience wrapper)
 * @param model_id The model to download
 * @param callback Progress callback (may be NULL)
 * @param user_data Data for callback
 * @return TRUE on success
 */
gboolean dt_ai_models_download(const char *model_id,
                               dt_ai_progress_callback callback,
                               gpointer user_data);

/**
 * @brief Download all default models (runs in background)
 * @param callback Progress callback (may be NULL)
 * @param user_data Data for callback
 * @return TRUE if downloads started successfully
 */
gboolean dt_ai_models_download_default(dt_ai_progress_callback callback,
                                       gpointer user_data);

/**
 * @brief Download all models (runs in background)
 * @param callback Progress callback (may be NULL)
 * @param user_data Data for callback
 * @return TRUE if downloads started successfully
 */
gboolean dt_ai_models_download_all(dt_ai_progress_callback callback,
                                   gpointer user_data);

/**
 * @brief One model offered by the repository's compatible release
 *
 * versions.json lists everything in a release, a superset of the bundled
 * catalog: a model released after this build appears there but not in
 * ai_models.json.
 */
typedef struct dt_ai_repo_model_t
{
  char *id;               // model id, also the .dtmodel asset stem
  char *repository;       // "owner/repo" that published it
  char *version;          // version published in the release
  char *checksum;         // "sha256:...", never NULL; entries without one
                          // are dropped as unverifiable
  char *name;             // display name, may be NULL
  char *description;      // one-line summary, may be NULL
  char *task;             // task the model serves, may be NULL
  char *license;          // license of the weights, may be NULL
  gint64 size;            // download size in bytes, 0 when unpublished
  gboolean in_catalog;  // listed in darktable's bundled ai_models.json
  dt_ai_model_status_t status;  // UPDATE_AVAILABLE when the release is
                                // newer than what is on disk
} dt_ai_repo_model_t;

/**
 * @brief List the models the repository offers beyond the bundled
 *        catalog, which the main model list already manages
 *
 * Blocking network I/O — call from a worker thread.
 *
 * @param repository "owner/repo" to query; NULL uses the configured one
 * @param error_msg Set to an error message on failure (caller must free);
 *                  may be NULL
 * @return List of dt_ai_repo_model_t*, sorted by id, or NULL on failure or
 *         when nothing is offered. Free with dt_ai_repo_model_list_free().
 */
GList *dt_ai_models_fetch_repository_list(const char *repository,
                                          char **error_msg);

/**
 * @brief The repositories models may be installed from
 *
 * plugins/ai/repository first, then any listed in
 * plugins/ai/third_party_repositories. Malformed entries are dropped.
 *
 * @return List of newly allocated "owner/repo" strings; free with
 *         g_list_free_full(list, g_free)
 */
GList *dt_ai_models_get_repositories(void);

/**
 * @brief Just the additional repositories, without the official one
 * @return List of newly allocated "owner/repo" strings; free with
 *         g_list_free_full(list, g_free)
 */
GList *dt_ai_models_get_third_party_repositories(void);

/**
 * @brief Replace the list of additional repositories
 *
 * Writes plugins/ai/third_party_repositories. Entries are not validated here —
 * dt_ai_models_get_repositories() drops malformed ones when reading.
 *
 * @param repositories List of "owner/repo" strings (may be NULL to clear)
 */
void dt_ai_models_set_third_party_repositories(GList *repositories);

/**
 * @brief Free a list returned by dt_ai_models_fetch_repository_list()
 */
void dt_ai_repo_model_list_free(GList *list);

/**
 * @brief Make a repository-listed model downloadable
 *
 * The download path only operates on registry members, so this adds a
 * transient entry carrying the release's digest. It lasts for the session;
 * an installed model is rediscovered from its config.json on the next start.
 *
 * @param entry Entry from dt_ai_models_fetch_repository_list()
 * @return TRUE if the model is now known to the registry
 */
gboolean dt_ai_models_register_repository_model(const dt_ai_repo_model_t *entry);

/**
 * @brief Record where an install actually came from
 *
 * Call only once the download has landed. dt_ai_models_refresh_status()
 * reads this back to restore a model's publisher across sessions.
 *
 * @param model_id The model that was installed
 * @param repository "owner/repo" it came from, or NULL to clear
 */
void dt_ai_models_record_origin(const char *model_id, const char *repository);

/**
 * @brief Undo a registration when the download did not happen
 *
 * Removes an entry the registration created, and restores the publisher
 * and checksum of one it merely redirected, so it is safe to call without
 * tracking whether the model was known beforehand.
 *
 * @param model_id The model to unregister
 */
void dt_ai_models_unregister_repository_model(const char *model_id);

/**
 * @brief Register and download a model listed by the repository
 *
 * Wrapper over dt_ai_models_register_repository_model() and
 * dt_ai_models_download_sync() for callers without their own progress
 * dialog. Blocking; unregisters again if the download fails.
 *
 * @param entry Entry from dt_ai_models_fetch_repository_list()
 * @param callback Progress callback (may be NULL)
 * @param user_data Data for callback
 * @param cancel_flag Pointer to boolean checked for cancellation (may be NULL)
 * @return Error message (caller must free) or NULL on success
 */
char *dt_ai_models_install_from_repository(const dt_ai_repo_model_t *entry,
                                           dt_ai_progress_callback callback,
                                           gpointer user_data,
                                           const gboolean *cancel_flag);
#endif /* HAVE_AI_DOWNLOAD */

/**
 * @brief Delete a downloaded model
 * @param model_id The model to delete
 * @return TRUE on success
 */
gboolean dt_ai_models_delete(const char *model_id);

// --- Configuration ---

/**
 * @brief Set model enabled state (persisted to config)
 * @param model_id The model ID
 * @param enabled Whether the model should be enabled
 */
void dt_ai_models_set_enabled(const char *model_id, gboolean enabled);

/**
 * @brief Get the active model ID for a task.
 *
 * Looks up the centralized config key `plugins/ai/models/active/{task}`.
 * If not set, falls back to legacy consumer config keys, then to the
 * default downloaded model for the task.
 *
 * SIDE EFFECT: when the lookup resolves via the default-downloaded
 * fallback (no conf key present), the resulting id is also written
 * back to `plugins/ai/models/active/{task}`. Subsequent calls see an
 * "explicit" choice that was actually materialized by the first call.
 * Once the conf key holds any value (including the empty string from
 * an explicit clear), the fallback is bypassed and no write happens.
 *
 * @param task The task type (e.g. "mask", "denoise")
 * @return Newly allocated model ID string (caller must free), or NULL if none active
 */
char *dt_ai_models_get_active_for_task(const char *task);

/**
 * @brief Get the version string of a model by ID.
 * @return Version string (e.g. "1.0"), or "0.0" if not set.
 *         Pointer valid until next registry refresh. Do not free.
 */
const char *dt_ai_model_get_version(const char *model_id);

/**
 * @brief Get the minimum required version from the registry.
 * @return Min version string, or NULL if not set.
 *         Pointer valid until next registry refresh. Do not free.
 */
const char *dt_ai_model_get_min_version(const char *model_id);

/**
 * @brief Set the active model for a task (exclusive — clears previous).
 *
 * Persists to `plugins/ai/models/active/{task}` in darktablerc.
 * Pass model_id=NULL to clear (disable the task).
 *
 * @param task The task type (e.g. "mask", "denoise")
 * @param model_id The model ID to activate, or NULL to clear
 */
void dt_ai_models_set_active_for_task(const char *task, const char *model_id);

/**
 * @brief Get the path to a downloaded model's directory
 * @param model_id The model ID
 * @return Path string (caller must free), or NULL if not downloaded
 */
char *dt_ai_models_get_path(const char *model_id);

/**
 * @brief Get the symbolic spatial dimension names for a model
 * @param model_id The model identifier
 * @param out_h Output: height dimension name (never NULL; points to static default if unset)
 * @param out_w Output: width dimension name (never NULL; points to static default if unset)
 */
void dt_ai_models_get_spatial_dims(const char *model_id,
                                   const char **out_h,
                                   const char **out_w);

/**
 * @brief Model card — provenance and transparency info read
 *        from config.json on demand. All fields are optional;
 *        NULL means the field was not present
 */
typedef struct dt_ai_model_card_t
{
  char *name;
  char *long_description;
  char *scope;
  char *author;
  char *source;
  char *paper;
  char *license;
  char *training_data;
  char *training_data_license;
  char *notes;
} dt_ai_model_card_t;

/**
 * @brief Read model card from config.json on disk
 * @param model_id The model identifier
 * @return Card struct (caller must free with dt_ai_model_card_free),
 *         or NULL if model directory not found
 */
dt_ai_model_card_t *dt_ai_models_get_card(const char *model_id);

/**
 * @brief Free a model card returned by dt_ai_models_get_card
 */
void dt_ai_model_card_free(dt_ai_model_card_t *card);

/**
 * @brief Get or lazily create the AI backend environment.
 *        The environment is cached in the registry and destroyed
 *        by dt_ai_models_cleanup().
 * @return Environment handle, or NULL if AI is disabled
 */
dt_ai_environment_t *dt_ai_registry_get_env(void);
