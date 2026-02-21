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

// clang-format off
static const dt_ai_provider_desc_t dt_ai_providers[] = {
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
#if defined(__linux__)
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
#if defined(__linux__) || (defined(__APPLE__) && defined(__x86_64__))
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

_Static_assert(sizeof(dt_ai_providers) / sizeof(dt_ai_providers[0]) == DT_AI_PROVIDER_COUNT,
               "dt_ai_providers table out of sync with dt_ai_provider_t enum");

/** Config key for the AI execution provider preference */
#define DT_AI_CONF_PROVIDER "plugins/ai/provider"

/** Get display name for a provider enum value */
const char *dt_ai_provider_to_string(dt_ai_provider_t provider);

/** Parse provider from config string (with legacy alias support) */
dt_ai_provider_t dt_ai_provider_from_string(const char *str);

/** Test if a provider is available at runtime (checks deps, not just compile-time).
 *  @return 1 if available, 0 if not. */
int dt_ai_probe_provider(dt_ai_provider_t provider);

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
  const char *id;          ///< Unique ID (e.g. "nafnet-sidd")
  const char *name;        ///< Display name
  const char *description; ///< Short description
  const char *task_type;   ///< e.g. "denoise", "inpainting"
  const char *backend;     ///< Backend type (e.g. "onnx")
  int num_inputs;          ///< Number of model inputs (default 1)
} dt_ai_model_info_t;

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
 *        When dt_ai_load_model / dt_ai_load_model_opts is called with
 *        DT_AI_PROVIDER_AUTO, the environment's provider is used instead.
 * @param env The environment handle.
 * @param provider The provider to use (DT_AI_PROVIDER_AUTO = platform auto-detect).
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
 * @param provider Execution provider to use for hardware acceleration.
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
 * @param provider Execution provider to use for hardware acceleration.
 * @param opt_level Graph optimization level.
 * @param dim_overrides Array of symbolic dimension overrides (NULL = none).
 * @param n_overrides Number of overrides.
 * @return dt_ai_context_t* Context ready for inference, or NULL.
 */
dt_ai_context_t *dt_ai_load_model_ext(dt_ai_environment_t *env,
                                       const char *model_id,
                                       const char *model_file,
                                       dt_ai_provider_t provider,
                                       dt_ai_opt_level_t opt_level,
                                       const dt_ai_dim_override_t *dim_overrides,
                                       int n_overrides);

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
