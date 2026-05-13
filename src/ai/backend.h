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

#include <stdint.h>
#include <glib.h>

/**
 * @brief AI Execution Provider
 */
typedef enum {
  DT_AI_PROVIDER_AUTO = 0,
  DT_AI_PROVIDER_CPU,
  DT_AI_PROVIDER_COREML,
  DT_AI_PROVIDER_CUDA,
  DT_AI_PROVIDER_MIGRAPHX,
  DT_AI_PROVIDER_OPENVINO,
  DT_AI_PROVIDER_DIRECTML,
  DT_AI_PROVIDER_COUNT  // must be last
} dt_ai_provider_t;

/** Sentinel for dt_ai_load_model / dt_ai_load_model_ext: read the
 *  user's configured provider from darktablerc instead of forcing
 *  a specific one. Not a real provider — never store in config or
 *  the provider table. */
#define DT_AI_PROVIDER_CONFIGURED (-1)

/**
 * @brief Provider descriptor: maps enum to config/display strings.
 *
 * config_string: persisted to darktablerc, matches ONNX Runtime provider names
 * display_name:  shown in UI combo boxes and log messages
 * available:     compile-time platform guard (FALSE = hidden from UI)
 */
typedef struct dt_ai_provider_desc_t {
  dt_ai_provider_t value;
  const char *config_string;
  const char *display_name;
  int available;
} dt_ai_provider_desc_t;

extern const dt_ai_provider_desc_t dt_ai_providers[DT_AI_PROVIDER_COUNT];

/** Config key for the AI execution provider preference */
#define DT_AI_CONF_PROVIDER "plugins/ai/provider"

/** Get display name for a provider enum value */
const char *dt_ai_provider_to_string(dt_ai_provider_t provider);

/** Parse provider from config string (with legacy alias support) */
dt_ai_provider_t dt_ai_provider_from_string(const char *str);

/** Parse the comma-separated EP list produced by
 *  dt_ai_ort_probe_library_full() into a bitmask of (1u << dt_ai_provider_t)
 *  values. AUTO and CPU are always set. Unknown tokens are ignored. */
guint dt_ai_providers_from_eps(const char *eps);

/** Bitmask of providers that ship with darktable's bundled ORT on the
 *  current platform. Includes AUTO and CPU always, plus CoreML on macOS
 *  and DirectML on Windows. Use as a safe default when the loaded ORT
 *  cannot be probed. */
guint dt_ai_providers_bundled(void);

/** Snapshot the conf state (ORT path, provider, device ids) for the
 *  *_changed_since_load() helpers.  Call once at darktable startup so
 *  the snapshot is independent of when ORT is lazily initialized. */
void dt_ai_snapshot_conf_state(void);

/** TRUE if plugins/ai/ort_library_path differs from the value seen
 *  when ORT was loaded — the in-process ORT is stale, restart needed. */
gboolean dt_ai_ort_path_changed_since_load(void);

/** TRUE if the configured provider differs from the value seen when
 *  ORT was loaded — long-lived sessions are stale, restart needed. */
gboolean dt_ai_provider_changed_since_load(void);

/** A selectable GPU device for a multi-GPU-capable execution provider. */
typedef struct dt_ai_device_t
{
  int    id;     /**< device_id passed to the EP (e.g. CUDA ordinal, DXGI adapter index) */
  gchar *name;   /**< human-readable label, e.g. "NVIDIA GeForce RTX 5060" */
} dt_ai_device_t;

/** Enumerate selectable GPU devices for a provider. Returns NULL or an
 *  empty list for AUTO/CPU/OpenVINO/CoreML (no per-device choice).
 *  Caller frees with `g_list_free_full(list, dt_ai_device_free)`. */
GList *dt_ai_enum_devices_for_provider(const dt_ai_provider_t provider);

/** Free a single dt_ai_device_t (the struct + its name). */
void dt_ai_device_free(gpointer device);

/** Return the conf key (`"plugins/ai/<ep>_device_id"`) for a provider,
 *  or NULL for providers that don't support device selection
 *  (AUTO/CPU/OpenVINO/CoreML). The returned string is statically
 *  allocated and must not be freed. */
const char *dt_ai_device_conf_key_for_provider(const dt_ai_provider_t provider);

/** TRUE if the device_id conf for the given provider differs from the
 *  value used when ORT was loaded — i.e. a restart is needed before
 *  GPU selection takes effect. FALSE for providers without device
 *  selection or before ORT is loaded. */
gboolean dt_ai_device_id_changed_since_load(const dt_ai_provider_t provider);

/** Test if a provider is available at runtime (checks deps, not just compile-time).
 *  @return 1 if available, 0 if not. */
int dt_ai_probe_provider(dt_ai_provider_t provider);

/** Probe a shared library to check if it's a valid ONNX Runtime build.
 *  @return version string (caller must g_free) or NULL on failure. */
char *dt_ai_ort_probe_library(const char *path);

/** Probe a library and return version + supported execution providers.
 *  @param out_version version string (caller must g_free), may be NULL
 *  @param out_eps comma-separated EP names (caller must g_free), may be NULL
 *  @return 1 if valid ORT library, 0 otherwise */
int dt_ai_ort_probe_library_full(const char *path, char **out_version, char **out_eps);

/** Result from dt_ai_ort_find_libraries(). Caller owns all strings. */
typedef struct dt_ai_ort_found_t
{
  char *path;     // full path to the library
  char *version;  // ORT version string
  char *eps;      // comma-separated execution provider names
} dt_ai_ort_found_t;

/** Scan system and user-space paths for valid ORT libraries.
 *  @return GList of dt_ai_ort_found_t (caller must free with dt_ai_ort_found_free) */
GList *dt_ai_ort_find_libraries(void);

/** Free a dt_ai_ort_found_t */
void dt_ai_ort_found_free(dt_ai_ort_found_t *f);

/**
 * @brief Graph Optimization Level
 *
 * Models with fully dynamic output shapes (e.g. SAM2 decoder) can fail
 * under aggressive graph optimization because ONNX Runtime's shape
 * inference mis-computes intermediate tensor sizes.  Use DT_AI_OPT_BASIC
 * for such models to avoid internal shape validation errors.
 */
typedef enum {
  DT_AI_OPT_ALL = 0,      ///< All optimizations (default, fastest)
  DT_AI_OPT_BASIC = 1,    ///< Basic only (constant folding, redundant node elimination)
  DT_AI_OPT_DISABLED = 2, ///< No optimization (reserved for future use)
} dt_ai_opt_level_t;

/**
 * @brief Library Environment Handle
 * Opaque handle representing the initialized AI library environment.
 */
typedef struct dt_ai_environment_t dt_ai_environment_t;

/**
 * @brief Execution Context Handle
 * Opaque handle for a loaded model session.
 */
typedef struct dt_ai_context_t dt_ai_context_t;

/**
 * @brief Model Metadata (ReadOnly)
 */
typedef struct dt_ai_model_info_t {
  const char *id;          ///< Unique ID (e.g. "mask-object-segnext-b2hq")
  const char *name;        ///< Display name
  const char *description; ///< Short description
  const char *task_type;   ///< e.g. "mask", "denoise"
  const char *arch;        ///< e.g. "sam2", "segnext"
  const char *backend;     ///< Backend type (e.g. "onnx")
  int num_inputs;          ///< Number of model inputs (default 1)
  const char *attributes;  ///< Optional attributes
  const char *cpu_only;    ///< Top-level cpu_only block (JSON: array or object).
                           ///< Consumed internally by the load function to
                           ///< override the provider when the model declares
                           ///< the configured EP unsafe.
} dt_ai_model_info_t;

/* --- Model "attributes" lookup ---
 *
 * Models declare optional behavior hints under an "attributes" object
 * in their config.json, e.g.:
 *   "attributes": {
 *     "shadow_boost": true,
 *     "tile_factor": 1.5,
 *     "color_space": "sRGB"
 *   }
 *
 * The accessors parse the stored JSON on demand. A missing key (or
 * one of a different type) returns the supplied default — or FALSE /
 * NULL for the bool / string variants.
 */

gboolean dt_ai_model_attribute_bool(const dt_ai_model_info_t *info,
                                    const char *key);

int dt_ai_model_attribute_int(const dt_ai_model_info_t *info,
                              const char *key,
                              int default_value);

double dt_ai_model_attribute_double(const dt_ai_model_info_t *info,
                                    const char *key,
                                    double default_value);

/** Returned string is newly allocated and must be freed with g_free().
 *  Returns NULL if the key is absent or not a string. */
char *dt_ai_model_attribute_string(const dt_ai_model_info_t *info,
                                   const char *key);

/** Return a newly-allocated int array from the JSON-array attribute
 *  named `key`. *out_count is set to the array length; NULL is returned
 *  (and *out_count = 0) when the key is absent or not a JSON array.
 *  Caller frees the returned array with g_free(). */
int *dt_ai_model_attribute_int_array(const dt_ai_model_info_t *info,
                                     const char *key,
                                     int *out_count);

/* --- Discovery --- */

/**
 * @brief Initialize the library environment and scan for models.
 * @param search_paths Semicolon-separated list of paths to scan.
 * @return dt_ai_environment_t* Handle, or NULL on error.
 */
dt_ai_environment_t *dt_ai_env_init(const char *search_paths);

/**
 * @brief Get the number of discovered models.
 */
int dt_ai_get_model_count(dt_ai_environment_t *env);

/**
 * @brief Get model details by index.
 * @param env The environment handle.
 * @param index Index 0 to count-1.
 * @return const dt_ai_model_info_t* Pointer to info struct.
 */
const dt_ai_model_info_t *
dt_ai_get_model_info_by_index(dt_ai_environment_t *env, int index);

/**
 * @brief Get model details by unique ID.
 * @param env The environment handle.
 * @param id The unique ID of the model.
 * @return const dt_ai_model_info_t* Pointer to info struct.
 */
const dt_ai_model_info_t *
dt_ai_get_model_info_by_id(dt_ai_environment_t *env, const char *id);

/**
 * @brief Refresh the environment by rescanning model directories.
 * @param env The environment handle to refresh.
 * @note Call this after downloading new models.
 */
void dt_ai_env_refresh(dt_ai_environment_t *env);

/**
 * @brief Cleanup the library environment.
 * @param env The environment handle to destroy.
 */
void dt_ai_env_destroy(dt_ai_environment_t *env);

/**
 * @brief Set the default execution provider for this environment.
 * @param env The environment handle.
 * @param provider The provider to use (DT_AI_PROVIDER_AUTO = probe all EPs).
 */
void dt_ai_env_set_provider(dt_ai_environment_t *env, dt_ai_provider_t provider);

/**
 * @brief Get the default execution provider for this environment.
 * @param env The environment handle.
 * @return The currently set provider.
 */
dt_ai_provider_t dt_ai_env_get_provider(dt_ai_environment_t *env);

/* --- Execution --- */

/**
 * @brief Load a model for execution from the registry.
 * @param env Library environment.
 * @param model_id ID of the model to load.
 * @param model_file Filename within the model directory (NULL = "model.onnx").
 * @param provider Execution provider (DT_AI_PROVIDER_CONFIGURED = use user config).
 * @return dt_ai_context_t* Context ready for inference, or NULL.
 */
dt_ai_context_t *dt_ai_load_model(dt_ai_environment_t *env,
                                  const char *model_id,
                                  const char *model_file,
                                  dt_ai_provider_t provider);

/**
 * @brief Symbolic dimension override for models with dynamic shapes.
 */
typedef struct {
  const char *name;  ///< Symbolic dimension name (e.g. "num_labels")
  int64_t value;     ///< Concrete value to use
} dt_ai_dim_override_t;

/**
 * @brief Load a model with optimization options and symbolic dimension overrides.
 *        Dimension overrides fix shape inference for models with symbolic dims
 *        that prevent ONNX Runtime from resolving intermediate tensor shapes.
 * @param env Library environment.
 * @param model_id ID of the model to load.
 * @param model_file Filename within the model directory (NULL = "model.onnx").
 *                   When the model package declares per-file cpu_only entries,
 *                   matching is done against this filename's stem (the
 *                   basename minus the ".onnx" extension).
 * @param provider Execution provider (DT_AI_PROVIDER_CONFIGURED = use user config).
 *                 The load function may override this to CPU when the model
 *                 declares the configured GPU EP unsafe (see cpu_only attribute).
 * @param opt_level Graph optimization level.
 * @param dim_overrides Array of symbolic dimension overrides (NULL = none).
 * @param n_overrides Number of overrides.
 * @param ep_flags EP-specific flag bitmask. Callers should pass 0; the
 *                 load function adds bits internally based on the model's
 *                 cpu_only declaration (e.g. COREML_FLAG_USE_CPU_ONLY).
 * @return dt_ai_context_t* Context ready for inference, or NULL.
 */
dt_ai_context_t *dt_ai_load_model_ext(dt_ai_environment_t *env,
                                       const char *model_id,
                                       const char *model_file,
                                       dt_ai_provider_t provider,
                                       dt_ai_opt_level_t opt_level,
                                       const dt_ai_dim_override_t *dim_overrides,
                                       int n_overrides,
                                       uint32_t ep_flags);

/**
 * @brief Tensor Data Types
 */
typedef enum {
  DT_AI_FLOAT = 1,
  DT_AI_UINT8 = 2,
  DT_AI_INT8 = 3,
  DT_AI_INT32 = 4,
  DT_AI_INT64 = 5,
  DT_AI_FLOAT16 = 10
} dt_ai_dtype_t;

/**
 * @brief Tensor descriptor for I/O
 */
typedef struct dt_ai_tensor_t {
  void *data;         ///< Pointer to raw data buffer
  dt_ai_dtype_t type; ///< Data type of elements
  int64_t *shape;     ///< Array of dimensions
  int ndim;           ///< Number of dimensions
} dt_ai_tensor_t;

/**
 * @brief Run inference through the loaded model.
 * @param ctx The AI context.
 * @param inputs Array of input tensors.
 * @param num_inputs Number of input tensors.
 * @param outputs Array of output tensors.
 * @param num_outputs Number of output tensors.
 * @return int 0 on success, <0 on error.
 */
int dt_ai_run(dt_ai_context_t *ctx, dt_ai_tensor_t *inputs,
                           int num_inputs, dt_ai_tensor_t *outputs,
                           int num_outputs);

/**
 * @brief Get the number of model inputs.
 * @param ctx The AI context.
 * @return Number of inputs, or 0 if ctx is NULL.
 */
int dt_ai_get_input_count(dt_ai_context_t *ctx);

/**
 * @brief Get the number of model outputs.
 * @param ctx The AI context.
 * @return Number of outputs, or 0 if ctx is NULL.
 */
int dt_ai_get_output_count(dt_ai_context_t *ctx);

/**
 * @brief Get the name of a model input by index.
 * @param ctx The AI context.
 * @param index Input index (0-based).
 * @return Input name string (owned by ctx, do not free), or NULL.
 */
const char *dt_ai_get_input_name(dt_ai_context_t *ctx, int index);

/**
 * @brief Get the data type of a model input by index.
 * @param ctx The AI context.
 * @param index Input index (0-based).
 * @return Data type, or DT_AI_FLOAT as fallback.
 */
dt_ai_dtype_t dt_ai_get_input_type(dt_ai_context_t *ctx,
                                                 int index);

/**
 * @brief Get the name of a model output by index.
 * @param ctx The AI context.
 * @param index Output index (0-based).
 * @return Output name string (owned by ctx, do not free), or NULL.
 */
const char *dt_ai_get_output_name(dt_ai_context_t *ctx,
                                                int index);

/**
 * @brief Get the data type of a model output by index.
 * @param ctx The AI context.
 * @param index Output index (0-based).
 * @return Data type, or DT_AI_FLOAT as fallback.
 */
dt_ai_dtype_t dt_ai_get_output_type(dt_ai_context_t *ctx,
                                                  int index);

/**
 * @brief Get the shape of a model output by index.
 * @param ctx The AI context.
 * @param index Output index (0-based).
 * @param shape Output array to fill with dimension sizes.
 * @param max_dims Maximum number of dimensions to write.
 * @return Number of dimensions, or -1 on error.
 */
int dt_ai_get_output_shape(dt_ai_context_t *ctx, int index,
                           int64_t *shape, int max_dims);

/**
 * @brief Unload a model and free execution context.
 * @param ctx The AI context to unload.
 */
void dt_ai_unload_model(dt_ai_context_t *ctx);
