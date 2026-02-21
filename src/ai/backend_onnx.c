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
#include <glib.h>
#include <onnxruntime_c_api.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

// --- Internal Structures ---

struct dt_ai_context_t
{
  // ONNX Runtime C Objects
  OrtSession *session;
  OrtMemoryInfo *memory_info;

  // IO Names
  OrtAllocator *allocator;
  char **input_names;
  char **output_names;
  size_t input_count;
  dt_ai_dtype_t *input_types;
  size_t output_count;
  dt_ai_dtype_t *output_types;

  // TRUE when any output has symbolic/dynamic shape dims.
  // In that case dt_ai_run() lets ORT allocate outputs and copies back.
  gboolean dynamic_outputs;
};

// Global singletons (initialized exactly once via g_once)
// ORT requires at most one OrtEnv per process.
static const OrtApi *g_ort = NULL;
static GOnce g_ort_once = G_ONCE_INIT;
static OrtEnv *g_env = NULL;
static GOnce g_env_once = G_ONCE_INIT;

#ifdef ORT_LAZY_LOAD
// Redirect fd 2 to /dev/null.  Returns the saved fd on success, -1 on failure.
static int _stderr_suppress_begin(void)
{
  int saved = dup(STDERR_FILENO);
  if(saved == -1) return -1;
  int devnull = open("/dev/null", O_WRONLY);
  if(devnull == -1) { close(saved); return -1; }
  dup2(devnull, STDERR_FILENO);
  close(devnull);
  return saved;
}
// Restore fd 2 from the saved fd returned by _stderr_suppress_begin.
static void _stderr_suppress_end(int saved)
{
  if(saved != -1) { dup2(saved, STDERR_FILENO); close(saved); }
}
#endif

static gpointer _init_ort_api(gpointer data)
{
  (void)data;
  const OrtApi *api = NULL;

#ifdef ORT_LAZY_LOAD
  // Ubuntu/Debian's system ORT links against libonnx, causing harmless but noisy
  // "already registered" ONNX schema warnings when the library is first loaded.
  // Suppress them by loading ORT explicitly, with stderr temporarily redirected.
  // G_MODULE_BIND_LAZY = RTLD_LAZY; default (no BIND_LOCAL) = RTLD_GLOBAL so
  // provider symbols remain visible to the rest of the process via dlsym(NULL).
  int saved = _stderr_suppress_begin();
  // The handle is intentionally not stored: ORT must stay loaded for the process
  // lifetime and g_module_close is never called, so the library stays resident.
  GModule *ort_mod = g_module_open(ORT_LIBRARY_PATH, G_MODULE_BIND_LAZY);
  _stderr_suppress_end(saved);

  if(!ort_mod)
  {
    dt_print(DT_DEBUG_AI, "[darktable_ai] Failed to load ORT library '%s': %s",
             ORT_LIBRARY_PATH, g_module_error());
    return NULL;
  }
  typedef const OrtApiBase *(*OrtGetApiBaseFn)(void);
  OrtGetApiBaseFn get_api_base = NULL;
  if(!g_module_symbol(ort_mod, "OrtGetApiBase", (gpointer *)&get_api_base) || !get_api_base)
  {
    dt_print(DT_DEBUG_AI, "[darktable_ai] OrtGetApiBase symbol not found");
    return NULL;
  }
  api = get_api_base()->GetApi(ORT_API_VERSION);
#else
  api = OrtGetApiBase()->GetApi(ORT_API_VERSION);
#endif

  if(!api)
    dt_print(DT_DEBUG_AI, "[darktable_ai] Failed to init ONNX Runtime API");
  return (gpointer)api;
}

static gpointer _init_ort_env(gpointer data)
{
  (void)data;
  OrtEnv *env = NULL;
#ifdef ORT_LAZY_LOAD
  // ORT may emit additional schema-registration noise during env creation.
  int saved = _stderr_suppress_begin();
#endif
  OrtStatus *status = g_ort->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "DarktableAI", &env);
#ifdef ORT_LAZY_LOAD
  _stderr_suppress_end(saved);
#endif
  if(status)
  {
    dt_print(
      DT_DEBUG_AI,
      "[darktable_ai] Failed to create ORT environment: %s",
      g_ort->GetErrorMessage(status));
    g_ort->ReleaseStatus(status);
    return NULL;
  }
  return (gpointer)env;
}

// --- Helper Functions ---

// Map ONNX tensor element type to our dt_ai_dtype_t.
// Returns TRUE on success, FALSE if the type is unsupported.
static gboolean _map_onnx_type(ONNXTensorElementDataType onnx_type, dt_ai_dtype_t *out)
{
  switch(onnx_type)
  {
  case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT:
    *out = DT_AI_FLOAT;
    return TRUE;
  case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16:
    *out = DT_AI_FLOAT16;
    return TRUE;
  case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8:
    *out = DT_AI_UINT8;
    return TRUE;
  case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT8:
    *out = DT_AI_INT8;
    return TRUE;
  case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32:
    *out = DT_AI_INT32;
    return TRUE;
  case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64:
    *out = DT_AI_INT64;
    return TRUE;
  default:
    return FALSE;
  }
}

// Compute total element count from shape dimensions with overflow checking.
// Returns the product of all shape dimensions, or -1 if any dimension is
// non-positive or the multiplication would overflow int64_t.
static int64_t _safe_element_count(const int64_t *shape, int ndim)
{
  int64_t count = 1;
  for(int i = 0; i < ndim; i++)
  {
    if(shape[i] <= 0)
      return -1;
    if(count > INT64_MAX / shape[i])
      return -1;
    count *= shape[i];
  }
  return count;
}

// Map dt_ai_dtype_t to ONNX type and element size.
// Returns TRUE on success, FALSE if the type is unsupported.
static gboolean
_dtype_to_onnx(dt_ai_dtype_t dtype, ONNXTensorElementDataType *out_type, size_t *out_size)
{
  switch(dtype)
  {
  case DT_AI_FLOAT:
    *out_type = ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT;
    *out_size = sizeof(float);
    return TRUE;
  case DT_AI_FLOAT16:
    *out_type = ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16;
    *out_size = sizeof(uint16_t);
    return TRUE;
  case DT_AI_UINT8:
    *out_type = ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8;
    *out_size = sizeof(uint8_t);
    return TRUE;
  case DT_AI_INT8:
    *out_type = ONNX_TENSOR_ELEMENT_DATA_TYPE_INT8;
    *out_size = sizeof(int8_t);
    return TRUE;
  case DT_AI_INT32:
    *out_type = ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32;
    *out_size = sizeof(int32_t);
    return TRUE;
  case DT_AI_INT64:
    *out_type = ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64;
    *out_size = sizeof(int64_t);
    return TRUE;
  default:
    return FALSE;
  }
}

// Float16 Conversion Utilities
// Based on: https://gist.github.com/rygorous/2156668
// Handles Zero, Denormals, and Infinity correctly.
static uint16_t _float_to_half(float f)
{
  uint32_t x;
  memcpy(&x, &f, sizeof(x));
  uint32_t sign = (x >> 31) & 1;
  uint32_t exp = (x >> 23) & 0xFF;
  uint32_t mant = x & 0x7FFFFF;

  // Handle Zero and float32 denormals (too small for float16)
  if(exp == 0)
    return (uint16_t)(sign << 15);

  // Handle Infinity / NaN
  if(exp == 255)
    return (uint16_t)((sign << 15) | 0x7C00 | (mant ? 1 : 0));

  // Re-bias exponent from float32 (bias 127) to float16 (bias 15)
  int new_exp = (int)exp - 127 + 15;

  if(new_exp <= 0)
  {
    // Encode as float16 denormal: shift mantissa with implicit leading 1
    // The implicit 1 bit plus 10 mantissa bits, shifted right by (1 - new_exp)
    int shift = 1 - new_exp;
    if(shift > 24)
      return (uint16_t)(sign << 15);       // too small even for denormal
    uint32_t full_mant = (1 << 23) | mant; // restore implicit leading 1
    uint16_t half_mant = (uint16_t)(full_mant >> (13 + shift));
    return (uint16_t)((sign << 15) | half_mant);
  }
  else if(new_exp >= 31)
  {
    // Overflow to Infinity
    return (uint16_t)((sign << 15) | 0x7C00);
  }

  return (uint16_t)((sign << 15) | (new_exp << 10) | (mant >> 13));
}

static float _half_to_float(uint16_t h)
{
  uint32_t sign = (h >> 15) & 1;
  uint32_t exp = (h >> 10) & 0x1F;
  uint32_t mant = h & 0x3FF;

  if(exp == 0)
  {
    if(mant == 0)
    {
      // Zero
      uint32_t result = (sign << 31);
      float f;
      memcpy(&f, &result, 4);
      return f;
    }
    // Denormal: value = (-1)^sign * 2^(-14) * (mant / 1024)
    // Convert to float32 by normalizing: find leading 1 and shift
    uint32_t m = mant;
    int e = -1;
    while(!(m & 0x400))
    { // shift until leading 1 reaches bit 10
      m <<= 1;
      e--;
    }
    m &= 0x3FF; // remove the leading 1
    uint32_t new_exp = (uint32_t)(e + 127 - 14 + 1);
    uint32_t result = (sign << 31) | (new_exp << 23) | (m << 13);
    float f;
    memcpy(&f, &result, 4);
    return f;
  }
  else if(exp == 31)
  {
    // Inf / NaN
    uint32_t result = (sign << 31) | 0x7F800000 | (mant << 13);
    float f;
    memcpy(&f, &result, 4);
    return f;
  }

  // Normalized
  uint32_t new_exp = exp + 127 - 15;
  uint32_t result = (sign << 31) | (new_exp << 23) | (mant << 13);
  float f;
  memcpy(&f, &result, sizeof(f));
  return f;
}

// --- Optimization Helpers ---

// Try to find and call an ORT execution provider function at runtime via
// dynamic symbol lookup (GModule/dlsym).  Returns TRUE if the provider was
// enabled successfully, FALSE otherwise.
static gboolean _try_provider(
  OrtSessionOptions *session_opts,
  const char *symbol_name,
  const char *provider_name)
{
  OrtStatus *status = NULL;
  gboolean ok = FALSE;

  dt_print(DT_DEBUG_AI, "[darktable_ai] Attempting to enable %s...", provider_name);

#ifdef _WIN32
  // On Windows, we need to get the handle to onnxruntime.dll, not the main executable
  HMODULE h = GetModuleHandleA("onnxruntime.dll");
  if(!h)
  {
    // If not already loaded, try to load it
    h = LoadLibraryA("onnxruntime.dll");
  }
  void *func_ptr = NULL;
  if(h)
  {
    func_ptr = (void *)GetProcAddress(h, symbol_name);
    // Don't call FreeLibrary - we want to keep onnxruntime.dll loaded
  }
#else
  GModule *mod = g_module_open(NULL, 0);
  void *func_ptr = NULL;
  if(mod)
    g_module_symbol(mod, symbol_name, &func_ptr);
#endif

  if(func_ptr)
  {
    // All provider append functions take (OrtSessionOptions*, uint32_t/int)
    typedef OrtStatus *(*ProviderAppender)(OrtSessionOptions *, uint32_t);
    ProviderAppender appender = (ProviderAppender)func_ptr;
    status = appender(session_opts, 0);
    if(!status)
    {
      dt_print(DT_DEBUG_AI, "[darktable_ai] %s enabled successfully.", provider_name);
      ok = TRUE;
    }
    else
    {
      dt_print(
        DT_DEBUG_AI,
        "[darktable_ai] %s enable failed: %s",
        provider_name,
        g_ort->GetErrorMessage(status));
      g_ort->ReleaseStatus(status);
    }
  }
  else
  {
    dt_print(DT_DEBUG_AI, "[darktable_ai] %s provider not found.", provider_name);
  }

#ifndef _WIN32
  if(mod)
    g_module_close(mod);
#endif

  return ok;
}

static void
_enable_acceleration(OrtSessionOptions *session_opts, dt_ai_provider_t provider)
{
  switch(provider)
  {
  case DT_AI_PROVIDER_CPU:
    // CPU only - don't enable any accelerator
    dt_print(DT_DEBUG_AI, "[darktable_ai] Using CPU only (no hardware acceleration)");
    break;

  case DT_AI_PROVIDER_COREML:
#if defined(__APPLE__)
    _try_provider(
      session_opts,
      "OrtSessionOptionsAppendExecutionProvider_CoreML",
      "Apple CoreML");
#else
    dt_print(DT_DEBUG_AI, "[darktable_ai] Apple CoreML not available on this platform");
#endif
    break;

  case DT_AI_PROVIDER_CUDA:
    _try_provider(session_opts, "OrtSessionOptionsAppendExecutionProvider_CUDA", "NVIDIA CUDA");
    break;

  case DT_AI_PROVIDER_MIGRAPHX:
    // Try MIGraphX first; fall back to ROCm for older ORT builds
    if(!_try_provider(session_opts, "OrtSessionOptionsAppendExecutionProvider_MIGraphX", "AMD MIGraphX"))
      _try_provider(session_opts, "OrtSessionOptionsAppendExecutionProvider_ROCM", "AMD ROCm (legacy)");
    break;

  case DT_AI_PROVIDER_OPENVINO:
    _try_provider(session_opts, "OrtSessionOptionsAppendExecutionProvider_OpenVINO", "Intel OpenVINO");
    break;

  case DT_AI_PROVIDER_DIRECTML:
#if defined(_WIN32)
    _try_provider(
      session_opts,
      "OrtSessionOptionsAppendExecutionProvider_DML",
      "Windows DirectML");
#else
    dt_print(DT_DEBUG_AI, "[darktable_ai] Windows DirectML not available on this platform");
#endif
    break;

  case DT_AI_PROVIDER_AUTO:
  default:
    // Auto-detect best provider based on platform
#if defined(__APPLE__)
    _try_provider(
      session_opts,
      "OrtSessionOptionsAppendExecutionProvider_CoreML",
      "Apple CoreML");
#elif defined(_WIN32)
    _try_provider(
      session_opts,
      "OrtSessionOptionsAppendExecutionProvider_DML",
      "Windows DirectML");
#elif defined(__linux__)
    // Try CUDA first, then MIGraphX
    if(!_try_provider(
         session_opts,
         "OrtSessionOptionsAppendExecutionProvider_CUDA",
         "NVIDIA CUDA"))
    {
      if(!_try_provider(
           session_opts,
           "OrtSessionOptionsAppendExecutionProvider_MIGraphX",
           "AMD MIGraphX"))
        _try_provider(
          session_opts,
          "OrtSessionOptionsAppendExecutionProvider_ROCM",
          "AMD ROCm (legacy)");
    }
#endif
    break;
  }
}

// --- Provider Probe ---

int dt_ai_probe_provider(dt_ai_provider_t provider)
{
  // AUTO and CPU are always available
  if(provider == DT_AI_PROVIDER_AUTO || provider == DT_AI_PROVIDER_CPU)
    return 1;

  // Ensure ORT API is initialized
  g_once(&g_ort_once, _init_ort_api, NULL);
  g_ort = (const OrtApi *)g_ort_once.retval;
  if(!g_ort) return 0;

  g_once(&g_env_once, _init_ort_env, NULL);
  g_env = (OrtEnv *)g_env_once.retval;
  if(!g_env) return 0;

  // Create temporary session options for the probe
  OrtSessionOptions *opts = NULL;
  OrtStatus *status = g_ort->CreateSessionOptions(&opts);
  if(status)
  {
    g_ort->ReleaseStatus(status);
    return 0;
  }

  gboolean ok = FALSE;

  switch(provider)
  {
  case DT_AI_PROVIDER_COREML:
    ok = _try_provider(opts, "OrtSessionOptionsAppendExecutionProvider_CoreML", "Apple CoreML");
    break;
  case DT_AI_PROVIDER_CUDA:
    ok = _try_provider(opts, "OrtSessionOptionsAppendExecutionProvider_CUDA", "NVIDIA CUDA");
    break;
  case DT_AI_PROVIDER_MIGRAPHX:
    ok = _try_provider(opts, "OrtSessionOptionsAppendExecutionProvider_MIGraphX", "AMD MIGraphX")
      || _try_provider(opts, "OrtSessionOptionsAppendExecutionProvider_ROCM", "AMD ROCm (legacy)");
    break;
  case DT_AI_PROVIDER_OPENVINO:
    ok = _try_provider(opts, "OrtSessionOptionsAppendExecutionProvider_OpenVINO", "Intel OpenVINO");
    break;
  case DT_AI_PROVIDER_DIRECTML:
    ok = _try_provider(opts, "OrtSessionOptionsAppendExecutionProvider_DML", "Windows DirectML");
    break;
  default:
    break;
  }

  g_ort->ReleaseSessionOptions(opts);
  return ok ? 1 : 0;
}

// --- ONNX Model Loading ---

// Load ONNX model from model_dir/model_file with dimension overrides.
// If model_file is NULL, defaults to "model.onnx".
dt_ai_context_t *
dt_ai_onnx_load_ext(const char *model_dir, const char *model_file,
                    dt_ai_provider_t provider, dt_ai_opt_level_t opt_level,
                    const dt_ai_dim_override_t *dim_overrides, int n_overrides)
{
  if(!model_dir)
    return NULL;

  char *onnx_path
    = g_build_filename(model_dir, model_file ? model_file : "model.onnx", NULL);
  if(!g_file_test(onnx_path, G_FILE_TEST_EXISTS))
  {
    dt_print(DT_DEBUG_AI, "[darktable_ai] Model file missing: %s", onnx_path);
    g_free(onnx_path);
    return NULL;
  }

  // Lazy init ORT API and shared environment on first load
  g_once(&g_ort_once, _init_ort_api, NULL);
  g_ort = (const OrtApi *)g_ort_once.retval;
  if(!g_ort)
  {
    g_free(onnx_path);
    return NULL;
  }

  g_once(&g_env_once, _init_ort_env, NULL);
  g_env = (OrtEnv *)g_env_once.retval;
  if(!g_env)
  {
    g_free(onnx_path);
    return NULL;
  }

  dt_print(DT_DEBUG_AI, "[darktable_ai] Loading: %s", onnx_path);

  dt_ai_context_t *ctx = g_new0(dt_ai_context_t, 1);

  OrtStatus *status;
  OrtSessionOptions *session_opts;
  status = g_ort->CreateSessionOptions(&session_opts);
  if(status)
  {
    g_ort->ReleaseStatus(status);
    g_free(onnx_path);
    dt_ai_unload_model(ctx);
    return NULL;
  }

  // Optimize: Use all available cores (intra-op parallelism)
#ifdef _WIN32
  SYSTEM_INFO sysinfo;
  GetSystemInfo(&sysinfo);
  long num_cores = sysinfo.dwNumberOfProcessors;
#else
  long num_cores = sysconf(_SC_NPROCESSORS_ONLN);
#endif
  if(num_cores < 1)
    num_cores = 1;

  status = g_ort->SetIntraOpNumThreads(session_opts, (int)num_cores);
  if(status)
  {
    g_ort->ReleaseStatus(status);
    g_ort->ReleaseSessionOptions(session_opts);
    g_free(onnx_path);
    dt_ai_unload_model(ctx);
    return NULL;
  }

  const GraphOptimizationLevel ort_opt
    = (opt_level == DT_AI_OPT_DISABLED) ? ORT_DISABLE_ALL
    : (opt_level == DT_AI_OPT_BASIC)    ? ORT_ENABLE_BASIC
                                         : ORT_ENABLE_ALL;
  status = g_ort->SetSessionGraphOptimizationLevel(session_opts, ort_opt);
  if(status)
  {
    g_ort->ReleaseStatus(status);
    g_ort->ReleaseSessionOptions(session_opts);
    g_free(onnx_path);
    dt_ai_unload_model(ctx);
    return NULL;
  }

  // Override symbolic dimensions (fixes shape inference for dynamic-shape models)
  for(int i = 0; i < n_overrides; i++)
  {
    if(!dim_overrides[i].name) continue;
    status = g_ort->AddFreeDimensionOverrideByName(
      session_opts, dim_overrides[i].name, dim_overrides[i].value);
    if(status)
    {
      dt_print(DT_DEBUG_AI, "[darktable_ai] Dim override '%s' failed: %s",
               dim_overrides[i].name, g_ort->GetErrorMessage(status));
      g_ort->ReleaseStatus(status);
    }
  }

  // Optimize: Enable Hardware Acceleration
  _enable_acceleration(session_opts, provider);

#ifdef _WIN32
  // On Windows, CreateSession expects a wide character string
  wchar_t *onnx_path_wide = (wchar_t *)g_utf8_to_utf16(onnx_path, -1, NULL, NULL, NULL);
  status = g_ort->CreateSession(g_env, onnx_path_wide, session_opts, &ctx->session);
#else
  status = g_ort->CreateSession(g_env, onnx_path, session_opts, &ctx->session);
#endif

  // If accelerated provider failed, fall back to CPU-only
  if(status && provider != DT_AI_PROVIDER_CPU)
  {
    dt_print(DT_DEBUG_AI,
             "[darktable_ai] Accelerated session failed: %s — falling back to CPU",
             g_ort->GetErrorMessage(status));
    g_ort->ReleaseStatus(status);
    g_ort->ReleaseSessionOptions(session_opts);

    status = g_ort->CreateSessionOptions(&session_opts);
    if(status)
    {
      g_ort->ReleaseStatus(status);
#ifdef _WIN32
      g_free(onnx_path_wide);
#endif
      g_free(onnx_path);
      dt_ai_unload_model(ctx);
      return NULL;
    }
    status = g_ort->SetIntraOpNumThreads(session_opts, (int)num_cores);
    if(status) g_ort->ReleaseStatus(status);
    status = g_ort->SetSessionGraphOptimizationLevel(session_opts, ort_opt);
    if(status) g_ort->ReleaseStatus(status);
    for(int i = 0; i < n_overrides; i++)
    {
      if(!dim_overrides[i].name) continue;
      status = g_ort->AddFreeDimensionOverrideByName(
        session_opts, dim_overrides[i].name, dim_overrides[i].value);
      if(status) g_ort->ReleaseStatus(status);
    }
    // CPU-only: no _enable_acceleration call
#ifdef _WIN32
    status = g_ort->CreateSession(g_env, onnx_path_wide, session_opts, &ctx->session);
#else
    status = g_ort->CreateSession(g_env, onnx_path, session_opts, &ctx->session);
#endif
  }

#ifdef _WIN32
  g_free(onnx_path_wide);
#endif
  g_ort->ReleaseSessionOptions(session_opts);
  g_free(onnx_path);

  if(status)
  {
    dt_print(
      DT_DEBUG_AI,
      "[darktable_ai] Failed to create session: %s",
      g_ort->GetErrorMessage(status));
    g_ort->ReleaseStatus(status);
    dt_ai_unload_model(ctx);
    return NULL;
  }

  status
    = g_ort->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &ctx->memory_info);
  if(status)
  {
    g_ort->ReleaseStatus(status);
    dt_ai_unload_model(ctx);
    return NULL;
  }

  // Resolve IO Names
  status = g_ort->GetAllocatorWithDefaultOptions(&ctx->allocator);
  if(status)
  {
    g_ort->ReleaseStatus(status);
    dt_ai_unload_model(ctx);
    return NULL;
  }

  status = g_ort->SessionGetInputCount(ctx->session, &ctx->input_count);
  if(status)
  {
    g_ort->ReleaseStatus(status);
    dt_ai_unload_model(ctx);
    return NULL;
  }

  status = g_ort->SessionGetOutputCount(ctx->session, &ctx->output_count);
  if(status)
  {
    g_ort->ReleaseStatus(status);
    dt_ai_unload_model(ctx);
    return NULL;
  }

  ctx->input_names = g_new0(char *, ctx->input_count);
  ctx->input_types = g_new0(dt_ai_dtype_t, ctx->input_count);
  for(size_t i = 0; i < ctx->input_count; i++)
  {
    status
      = g_ort->SessionGetInputName(ctx->session, i, ctx->allocator, &ctx->input_names[i]);
    if(status)
    {
      g_ort->ReleaseStatus(status);
      dt_ai_unload_model(ctx);
      return NULL;
    }

    // Get Input Type
    OrtTypeInfo *typeinfo = NULL;
    status = g_ort->SessionGetInputTypeInfo(ctx->session, i, &typeinfo);
    if(status)
    {
      g_ort->ReleaseStatus(status);
      dt_ai_unload_model(ctx);
      return NULL;
    }
    const OrtTensorTypeAndShapeInfo *tensor_info = NULL;
    status = g_ort->CastTypeInfoToTensorInfo(typeinfo, &tensor_info);
    if(status)
    {
      g_ort->ReleaseStatus(status);
      g_ort->ReleaseTypeInfo(typeinfo);
      dt_ai_unload_model(ctx);
      return NULL;
    }
    ONNXTensorElementDataType type;
    status = g_ort->GetTensorElementType(tensor_info, &type);
    if(status)
    {
      g_ort->ReleaseStatus(status);
      g_ort->ReleaseTypeInfo(typeinfo);
      dt_ai_unload_model(ctx);
      return NULL;
    }

    if(!_map_onnx_type(type, &ctx->input_types[i]))
    {
      dt_print(
        DT_DEBUG_AI,
        "[darktable_ai] Unsupported ONNX input type %d for input %zu",
        type,
        i);
      g_ort->ReleaseTypeInfo(typeinfo);
      dt_ai_unload_model(ctx);
      return NULL;
    }

    g_ort->ReleaseTypeInfo(typeinfo);
  }

  ctx->output_names = g_new0(char *, ctx->output_count);
  ctx->output_types = g_new0(dt_ai_dtype_t, ctx->output_count);
  for(size_t i = 0; i < ctx->output_count; i++)
  {
    status = g_ort->SessionGetOutputName(
      ctx->session,
      i,
      ctx->allocator,
      &ctx->output_names[i]);
    if(status)
    {
      g_ort->ReleaseStatus(status);
      dt_ai_unload_model(ctx);
      return NULL;
    }

    // Get Output Type
    OrtTypeInfo *typeinfo = NULL;
    status = g_ort->SessionGetOutputTypeInfo(ctx->session, i, &typeinfo);
    if(status)
    {
      g_ort->ReleaseStatus(status);
      dt_ai_unload_model(ctx);
      return NULL;
    }
    const OrtTensorTypeAndShapeInfo *tensor_info = NULL;
    status = g_ort->CastTypeInfoToTensorInfo(typeinfo, &tensor_info);
    if(status)
    {
      g_ort->ReleaseStatus(status);
      g_ort->ReleaseTypeInfo(typeinfo);
      dt_ai_unload_model(ctx);
      return NULL;
    }
    ONNXTensorElementDataType type;
    status = g_ort->GetTensorElementType(tensor_info, &type);
    if(status)
    {
      g_ort->ReleaseStatus(status);
      g_ort->ReleaseTypeInfo(typeinfo);
      dt_ai_unload_model(ctx);
      return NULL;
    }

    if(!_map_onnx_type(type, &ctx->output_types[i]))
    {
      dt_print(
        DT_DEBUG_AI,
        "[darktable_ai] Unsupported ONNX output type %d for output %zu",
        type,
        i);
      g_ort->ReleaseTypeInfo(typeinfo);
      dt_ai_unload_model(ctx);
      return NULL;
    }

    g_ort->ReleaseTypeInfo(typeinfo);
  }

  // Detect dynamic output shapes (any dim <= 0 means symbolic/unknown).
  // When detected, dt_ai_run() will let ORT allocate outputs during
  // execution and copy the results back to the caller's buffer.
  ctx->dynamic_outputs = FALSE;
  for(size_t i = 0; i < ctx->output_count; i++)
  {
    int64_t shape[16];
    int ndim = dt_ai_get_output_shape(ctx, (int)i, shape, 16);
    if(ndim > 0)
    {
      for(int d = 0; d < ndim; d++)
      {
        if(shape[d] <= 0)
        {
          ctx->dynamic_outputs = TRUE;
          dt_print(DT_DEBUG_AI,
                   "[darktable_ai] Output[%zu] has dynamic dims — using ORT-allocated outputs",
                   i);
          break;
        }
      }
    }
    if(ctx->dynamic_outputs) break;
  }

  return ctx;
}

int dt_ai_run(
  dt_ai_context_t *ctx,
  dt_ai_tensor_t *inputs,
  int num_inputs,
  dt_ai_tensor_t *outputs,
  int num_outputs)
{
  if(!ctx || !ctx->session)
    return -1;
  if(num_inputs != ctx->input_count || num_outputs != ctx->output_count)
  {
    dt_print(
      DT_DEBUG_AI,
      "[darktable_ai] IO count mismatch. Expected %zu/%zu, got %d/%d",
      ctx->input_count,
      ctx->output_count,
      num_inputs,
      num_outputs);
    return -2;
  }

  // Run
  OrtStatus *status = NULL;
  int ret = 0;

  // Track temporary buffers to free later
  void **temp_input_buffers = g_new0(void *, num_inputs);

  // Create Input Tensors
  OrtValue **input_tensors = g_new0(OrtValue *, num_inputs);
  OrtValue **output_tensors = g_new0(OrtValue *, num_outputs);
  const char **input_names = (const char **)ctx->input_names; // Cast for Run()

  for(int i = 0; i < num_inputs; i++)
  {
    const int64_t element_count = _safe_element_count(inputs[i].shape, inputs[i].ndim);
    if(element_count < 0)
    {
      dt_print(
        DT_DEBUG_AI,
        "[darktable_ai] Invalid or overflowing shape for Input[%d]",
        i);
      ret = -4;
      goto cleanup;
    }

    ONNXTensorElementDataType onnx_type = ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT;
    size_t type_size = sizeof(float);
    void *data_ptr = inputs[i].data;

    // Check for Type Mismatch (Float -> Half)
    if(inputs[i].type == DT_AI_FLOAT && ctx->input_types[i] == DT_AI_FLOAT16)
    {
      dt_print(
        DT_DEBUG_AI,
        "[darktable_ai] Auto-converting Input[%d] Float32 -> Float16",
        i);
      // Auto-convert Float32 -> Float16
      onnx_type = ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16;
      type_size = sizeof(uint16_t); // Half is 2 bytes

      if((size_t)element_count > SIZE_MAX / type_size)
      {
        dt_print(DT_DEBUG_AI, "[darktable_ai] Tensor size overflow for Input[%d]", i);
        ret = -4;
        goto cleanup;
      }
      uint16_t *half_data = g_malloc(element_count * type_size);
      const float *src = (const float *)inputs[i].data;
      for(int64_t k = 0; k < element_count; k++)
      {
        half_data[k] = _float_to_half(src[k]);
      }

      data_ptr = half_data;
      temp_input_buffers[i] = half_data;
    }
    else
    {
      if(!_dtype_to_onnx(inputs[i].type, &onnx_type, &type_size))
      {
        dt_print(
          DT_DEBUG_AI,
          "[darktable_ai] Unsupported input type %d for Input[%d]",
          inputs[i].type,
          i);
        ret = -4;
        goto cleanup;
      }
    }

    if((size_t)element_count > SIZE_MAX / type_size)
    {
      dt_print(DT_DEBUG_AI, "[darktable_ai] Tensor size overflow for Input[%d]", i);
      ret = -4;
      goto cleanup;
    }
    status = g_ort->CreateTensorWithDataAsOrtValue(
      ctx->memory_info,
      data_ptr,
      element_count * type_size,
      inputs[i].shape,
      inputs[i].ndim,
      onnx_type,
      &input_tensors[i]);

    if(status)
    {
      dt_print(
        DT_DEBUG_AI,
        "[darktable_ai] CreateTensor Input[%d] fail: %s",
        i,
        g_ort->GetErrorMessage(status));
      g_ort->ReleaseStatus(status);
      ret = -4;
      goto cleanup;
    }
  }

  // Create Output Tensors
  const char **output_names = (const char **)ctx->output_names;

  for(int i = 0; i < num_outputs; i++)
  {
    // Dynamic outputs or Float16 mismatch: let ORT allocate during Run()
    if(ctx->dynamic_outputs
       || (outputs[i].type == DT_AI_FLOAT && ctx->output_types[i] == DT_AI_FLOAT16))
    {
      output_tensors[i] = NULL;
      continue;
    }

    const int64_t element_count = _safe_element_count(outputs[i].shape, outputs[i].ndim);
    if(element_count < 0)
    {
      dt_print(
        DT_DEBUG_AI,
        "[darktable_ai] Invalid or overflowing shape for Output[%d]",
        i);
      ret = -4;
      goto cleanup;
    }

    ONNXTensorElementDataType onnx_type;
    size_t type_size;

    if(!_dtype_to_onnx(outputs[i].type, &onnx_type, &type_size))
    {
      dt_print(
        DT_DEBUG_AI,
        "[darktable_ai] Unsupported output type %d for Output[%d]",
        outputs[i].type,
        i);
      ret = -4;
      goto cleanup;
    }

    if((size_t)element_count > SIZE_MAX / type_size)
    {
      dt_print(DT_DEBUG_AI, "[darktable_ai] Tensor size overflow for Output[%d]", i);
      ret = -4;
      goto cleanup;
    }
    status = g_ort->CreateTensorWithDataAsOrtValue(
      ctx->memory_info,
      outputs[i].data,
      element_count * type_size,
      outputs[i].shape,
      outputs[i].ndim,
      onnx_type,
      &output_tensors[i]);

    if(status)
    {
      dt_print(
        DT_DEBUG_AI,
        "[darktable_ai] CreateTensor Output[%d] fail: %s",
        i,
        g_ort->GetErrorMessage(status));
      g_ort->ReleaseStatus(status);
      ret = -4;
      goto cleanup;
    }
  }

  // RUN
  status = g_ort->Run(
    ctx->session,
    NULL,
    input_names,
    (const OrtValue *const *)input_tensors,
    num_inputs,
    output_names,
    num_outputs,
    output_tensors);

  if(status)
  {
    dt_print(DT_DEBUG_AI, "[darktable_ai] Run error: %s", g_ort->GetErrorMessage(status));
    g_ort->ReleaseStatus(status);
    ret = -3;
  }
  else
  {
    // Post-Run: Copy data from ORT-allocated outputs to caller's buffers.
    // This handles both dynamic-shape models (where we can't pre-allocate
    // because ORT's shape inference disagrees with the actual output shape)
    // and Float16→Float auto-conversion.
    for(int i = 0; i < num_outputs; i++)
    {
      const gboolean ort_allocated = ctx->dynamic_outputs
        || (outputs[i].type == DT_AI_FLOAT && ctx->output_types[i] == DT_AI_FLOAT16);
      if(!ort_allocated || !output_tensors[i]) continue;

      void *raw_data = NULL;
      status = g_ort->GetTensorMutableData(output_tensors[i], &raw_data);
      if(status)
      {
        dt_print(
          DT_DEBUG_AI,
          "[darktable_ai] GetTensorMutableData Output[%d] failed: %s",
          i,
          g_ort->GetErrorMessage(status));
        g_ort->ReleaseStatus(status);
        continue;
      }

      // Query ORT's actual tensor size to avoid reading past its allocation.
      // The caller's expected shape may differ from what ORT produced
      // (e.g., dynamic-shape models).
      OrtTensorTypeAndShapeInfo *tensor_info = NULL;
      status = g_ort->GetTensorTypeAndShape(output_tensors[i], &tensor_info);
      if(status)
      {
        dt_print(DT_DEBUG_AI, "[darktable_ai] GetTensorTypeAndShape Output[%d] failed: %s",
                 i, g_ort->GetErrorMessage(status));
        g_ort->ReleaseStatus(status);
        continue;
      }
      // Update caller's shape array with actual ORT output dimensions.
      // This is essential for dynamic-shape models where the caller's
      // pre-assumed shape may differ from what ORT actually produced.
      size_t actual_ndim = 0;
      OrtStatus *dim_st = g_ort->GetDimensionsCount(tensor_info, &actual_ndim);
      if(!dim_st && actual_ndim > 0 && (int)actual_ndim <= outputs[i].ndim)
      {
        OrtStatus *get_st = g_ort->GetDimensions(tensor_info, outputs[i].shape, actual_ndim);
        if(!get_st)
          outputs[i].ndim = (int)actual_ndim;
        else
          g_ort->ReleaseStatus(get_st);
      }
      if(dim_st) g_ort->ReleaseStatus(dim_st);

      size_t ort_element_count = 0;
      status = g_ort->GetTensorShapeElementCount(tensor_info, &ort_element_count);
      g_ort->ReleaseTensorTypeAndShapeInfo(tensor_info);
      if(status)
      {
        dt_print(DT_DEBUG_AI, "[darktable_ai] GetTensorShapeElementCount Output[%d] failed: %s",
                 i, g_ort->GetErrorMessage(status));
        g_ort->ReleaseStatus(status);
        continue;
      }

      const int64_t caller_count
        = _safe_element_count(outputs[i].shape, outputs[i].ndim);
      if(caller_count < 0)
      {
        dt_print(
          DT_DEBUG_AI,
          "[darktable_ai] Invalid shape for Output[%d] post-copy",
          i);
        continue;
      }

      // Use the smaller of ORT's actual size and caller's expected size
      const int64_t element_count = ((int64_t)ort_element_count < caller_count)
        ? (int64_t)ort_element_count
        : caller_count;

      if(element_count != caller_count)
      {
        dt_print(DT_DEBUG_AI,
                 "[darktable_ai] Output[%d] shape mismatch: ORT has %zu elements, "
                 "caller expects %" PRId64,
                 i, ort_element_count, caller_count);
      }

      if(ctx->output_types[i] == DT_AI_FLOAT16 && outputs[i].type == DT_AI_FLOAT)
      {
        // Float16 → Float conversion
        uint16_t *half_data = (uint16_t *)raw_data;
        float *dst = (float *)outputs[i].data;
        for(int64_t k = 0; k < element_count; k++)
          dst[k] = _half_to_float(half_data[k]);
      }
      else
      {
        // Same-type copy from ORT allocation to caller's buffer
        ONNXTensorElementDataType onnx_type;
        size_t type_size;
        if(!_dtype_to_onnx(outputs[i].type, &onnx_type, &type_size))
        {
          dt_print(
            DT_DEBUG_AI,
            "[darktable_ai] Unknown dtype %d for Output[%d] post-copy",
            outputs[i].type,
            i);
          continue;
        }
        memcpy(outputs[i].data, raw_data, element_count * type_size);
      }
    }
  }

cleanup:
  // Cleanup OrtValues (Wrappers only, data is owned by caller)
  for(int i = 0; i < num_inputs; i++)
    if(input_tensors[i])
      g_ort->ReleaseValue(input_tensors[i]);
  for(int i = 0; i < num_outputs; i++)
    if(output_tensors[i])
      g_ort->ReleaseValue(output_tensors[i]);

  // Free temp input buffers
  for(int i = 0; i < num_inputs; i++)
  {
    if(temp_input_buffers[i])
      g_free(temp_input_buffers[i]);
  }
  g_free(temp_input_buffers);

  g_free(input_tensors);
  g_free(output_tensors);

  return ret;
}

int dt_ai_get_input_count(dt_ai_context_t *ctx)
{
  return ctx ? (int)ctx->input_count : 0;
}

int dt_ai_get_output_count(dt_ai_context_t *ctx)
{
  return ctx ? (int)ctx->output_count : 0;
}

const char *dt_ai_get_input_name(dt_ai_context_t *ctx, int index)
{
  if(!ctx || index < 0 || (size_t)index >= ctx->input_count)
    return NULL;
  return ctx->input_names[index];
}

dt_ai_dtype_t dt_ai_get_input_type(dt_ai_context_t *ctx, int index)
{
  if(!ctx || index < 0 || (size_t)index >= ctx->input_count)
    return DT_AI_FLOAT;
  return ctx->input_types[index];
}

const char *dt_ai_get_output_name(dt_ai_context_t *ctx, int index)
{
  if(!ctx || index < 0 || (size_t)index >= ctx->output_count)
    return NULL;
  return ctx->output_names[index];
}

dt_ai_dtype_t dt_ai_get_output_type(dt_ai_context_t *ctx, int index)
{
  if(!ctx || index < 0 || (size_t)index >= ctx->output_count)
    return DT_AI_FLOAT;
  return ctx->output_types[index];
}

int dt_ai_get_output_shape(dt_ai_context_t *ctx, int index,
                           int64_t *shape, int max_dims)
{
  if(!ctx || !ctx->session || index < 0 || (size_t)index >= ctx->output_count
     || !shape || max_dims <= 0)
    return -1;

  OrtTypeInfo *typeinfo = NULL;
  OrtStatus *status = g_ort->SessionGetOutputTypeInfo(ctx->session, index, &typeinfo);
  if(status)
  {
    g_ort->ReleaseStatus(status);
    return -1;
  }

  const OrtTensorTypeAndShapeInfo *tensor_info = NULL;
  status = g_ort->CastTypeInfoToTensorInfo(typeinfo, &tensor_info);
  if(status)
  {
    g_ort->ReleaseStatus(status);
    g_ort->ReleaseTypeInfo(typeinfo);
    return -1;
  }

  size_t ndim = 0;
  status = g_ort->GetDimensionsCount(tensor_info, &ndim);
  if(status)
  {
    g_ort->ReleaseStatus(status);
    g_ort->ReleaseTypeInfo(typeinfo);
    return -1;
  }

  const int dims = (int)ndim < max_dims ? (int)ndim : max_dims;
  int64_t full_shape[16];
  if(ndim > 16)
  {
    g_ort->ReleaseTypeInfo(typeinfo);
    return -1;
  }

  status = g_ort->GetDimensions(tensor_info, full_shape, ndim);
  g_ort->ReleaseTypeInfo(typeinfo);
  if(status)
  {
    g_ort->ReleaseStatus(status);
    return -1;
  }

  memcpy(shape, full_shape, dims * sizeof(int64_t));
  return (int)ndim;
}

void dt_ai_unload_model(dt_ai_context_t *ctx)
{
  if(ctx)
  {
    if(ctx->session)
      g_ort->ReleaseSession(ctx->session);
    // Note: OrtEnv is a shared singleton (g_env), not per-context
    if(ctx->memory_info)
      g_ort->ReleaseMemoryInfo(ctx->memory_info);

    // Release IO names using the allocator that created them
    if(ctx->allocator)
    {
      for(size_t i = 0; i < ctx->input_count; i++)
      {
        if(ctx->input_names[i])
          ctx->allocator->Free(ctx->allocator, ctx->input_names[i]);
      }
      for(size_t i = 0; i < ctx->output_count; i++)
      {
        if(ctx->output_names[i])
          ctx->allocator->Free(ctx->allocator, ctx->output_names[i]);
      }
    }

    g_free(ctx->input_names);
    g_free(ctx->output_names);
    g_free(ctx->input_types);
    g_free(ctx->output_types);
    g_free(ctx);
  }
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
