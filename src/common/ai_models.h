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

// Ensure PATH_MAX is defined on all platforms
#ifndef PATH_MAX
#ifdef _WIN32
#define PATH_MAX _MAX_PATH
#else
#define PATH_MAX 4096
#endif
#endif

/**
 * @brief Model download/availability status
 */
typedef enum dt_ai_model_status_t {
  DT_AI_MODEL_NOT_DOWNLOADED = 0,
  DT_AI_MODEL_DOWNLOADING,
  DT_AI_MODEL_DOWNLOADED,
  DT_AI_MODEL_ERROR,
} dt_ai_model_status_t;

/**
 * @brief Information about a single AI model
 */
typedef struct dt_ai_model_t {
  char *id;              // Unique identifier (e.g. "nafnet-sidd-width32")
  char *name;            // Display name
  char *description;     // Short description
  char *task;            // Task type: "denoise", "upscale", etc.
  char *github_asset;    // Asset filename in GitHub release
  char *checksum;        // SHA256 checksum (format: "sha256:...")
  gboolean is_default;   // TRUE if model is a default model for its task
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
typedef void (*dt_ai_progress_callback)(const char *model_id, double progress,
                                        gpointer user_data);

/**
 * @brief AI Models Registry
 * Central registry for managing AI models
 */
typedef struct dt_ai_registry_t {
  GList *models;           // List of dt_ai_model_t*
  char *repository;        // GitHub repository (e.g. "darktable-org/darktable-ai")
  char *models_dir;        // Path to user's models directory
  char *cache_dir;         // Path to download cache directory
  gboolean ai_enabled;     // Global AI enable/disable
  dt_ai_provider_t provider;  // Selected execution provider
  GMutex lock;             // Thread safety for registry access
} dt_ai_registry_t;

// --- Core API ---

/**
 * @brief Initialize the AI models registry
 * @return New registry instance, or NULL on error
 */
dt_ai_registry_t *dt_ai_models_init(void);

/**
 * @brief Load model registry from JSON file
 * @param registry The registry to populate
 * @return TRUE on success
 */
gboolean dt_ai_models_load_registry(dt_ai_registry_t *registry);

/**
 * @brief Scan models directory and update download status
 * @param registry The registry to update
 */
void dt_ai_models_refresh_status(dt_ai_registry_t *registry);

/**
 * @brief Clean up and free the registry
 * @param registry The registry to destroy
 */
void dt_ai_models_cleanup(dt_ai_registry_t *registry);

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
 * @param registry The registry
 * @return Number of models
 */
int dt_ai_models_get_count(dt_ai_registry_t *registry);

/**
 * @brief Get model by index (returns a copy, caller must free with dt_ai_model_free)
 * @param registry The registry
 * @param index Index 0 to count-1
 * @return Model copy (caller owns), or NULL
 */
dt_ai_model_t *dt_ai_models_get_by_index(dt_ai_registry_t *registry, int index);

/**
 * @brief Get model by ID (returns a copy, caller must free with dt_ai_model_free)
 * @param registry The registry
 * @param model_id The unique model ID
 * @return Model copy (caller owns), or NULL
 */
dt_ai_model_t *dt_ai_models_get_by_id(dt_ai_registry_t *registry,
                                       const char *model_id);

// --- Download Operations ---

/**
 * @brief Download a specific model synchronously
 * @param registry The registry
 * @param model_id The model to download
 * @param callback Progress callback (may be NULL)
 * @param user_data Data for callback
 * @param cancel_flag Pointer to boolean checked for cancellation (may be NULL)
 * @return Error message (caller must free) or NULL on success
 */
char *dt_ai_models_download_sync(dt_ai_registry_t *registry, const char *model_id,
                                  dt_ai_progress_callback callback,
                                  gpointer user_data,
                                  const gboolean *cancel_flag);

/**
 * @brief Download a specific model (convenience wrapper)
 * @param registry The registry
 * @param model_id The model to download
 * @param callback Progress callback (may be NULL)
 * @param user_data Data for callback
 * @return TRUE on success
 */
gboolean dt_ai_models_download(dt_ai_registry_t *registry, const char *model_id,
                               dt_ai_progress_callback callback,
                               gpointer user_data);

/**
 * @brief Download all default models (runs in background)
 * @param registry The registry
 * @param callback Progress callback (may be NULL)
 * @param user_data Data for callback
 * @return TRUE if downloads started successfully
 */
gboolean dt_ai_models_download_default(dt_ai_registry_t *registry,
                                       dt_ai_progress_callback callback,
                                       gpointer user_data);

/**
 * @brief Download all models (runs in background)
 * @param registry The registry
 * @param callback Progress callback (may be NULL)
 * @param user_data Data for callback
 * @return TRUE if downloads started successfully
 */
gboolean dt_ai_models_download_all(dt_ai_registry_t *registry,
                                   dt_ai_progress_callback callback,
                                   gpointer user_data);

/**
 * @brief Delete a downloaded model
 * @param registry The registry
 * @param model_id The model to delete
 * @return TRUE on success
 */
gboolean dt_ai_models_delete(dt_ai_registry_t *registry, const char *model_id);

// --- Configuration ---

/**
 * @brief Set model enabled state (persisted to config)
 * @param registry The registry
 * @param model_id The model ID
 * @param enabled Whether the model should be enabled
 */
void dt_ai_models_set_enabled(dt_ai_registry_t *registry, const char *model_id,
                              gboolean enabled);

/**
 * @brief Get the active model ID for a task.
 *
 * Looks up the centralized config key `plugins/ai/models/active/{task}`.
 * If not set, falls back to legacy consumer config keys, then to the
 * default downloaded model for the task.
 *
 * @param task The task type (e.g. "mask", "denoise")
 * @return Newly allocated model ID string (caller must free), or NULL if none active
 */
char *dt_ai_models_get_active_for_task(const char *task);

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
 * @param registry The registry
 * @param model_id The model ID
 * @return Path string (caller must free), or NULL if not downloaded
 */
char *dt_ai_models_get_path(dt_ai_registry_t *registry, const char *model_id);
