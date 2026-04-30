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
#include <onnxruntime_c_api.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <dxgi1_6.h>
#include <initguid.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

struct dt_ai_context_t
{
  // ONNX runtime C objects
  OrtSession *session;
  OrtMemoryInfo *memory_info;

  // IO names
  OrtAllocator *allocator;
  char **input_names;
  char **output_names;
  size_t input_count;
  dt_ai_dtype_t *input_types;
  size_t output_count;
  dt_ai_dtype_t *output_types;

  // TRUE when any output has symbolic/dynamic shape dims.
  // in that case dt_ai_run() lets ORT allocate outputs and copies back
  gboolean dynamic_outputs;
};

// minimum ORT API we'll fall back to when the runtime library is older
// than what we were compiled against. v14 = ORT 1.14, the first release
// with ONNX opset 18 support — older ORT can't run our models. bump
// this in lockstep with any model that requires a newer opset
#define DT_ORT_MIN_API_VERSION 14

// global singletons (initialized exactly once via g_once)
// ORT requires at most one OrtEnv per process.
static const OrtApi *g_ort = NULL;
static GOnce g_ort_once = G_ONCE_INIT;
static OrtEnv *g_env = NULL;
static GOnce g_env_once = G_ONCE_INIT;
static GModule *g_ort_module = NULL;  // custom ORT library loaded via g_module_open
// snapshot of plugins/ai/ort_library_path at ORT load — lets the prefs
// page detect a path change mid-session (in-process ORT stale, restart
// needed before GPU EP probes reflect the new library)
static gchar *g_ort_conf_path_at_load = NULL;
// snapshot of per-EP device_id at ORT load. -1 = "no value seen yet"
// (e.g. provider not configured at startup); the change check then
// compares against the current conf value
static int g_loaded_cuda_device_id     = -1;
static int g_loaded_migraphx_device_id = -1;
static int g_loaded_dml_device_id      = -1;

// snapshot of the provider config string at ORT load. NULL until captured
static gchar *g_loaded_provider = NULL;

static int _device_id_from_conf(const char *conf_key, const char *env_var);

#if defined(__linux__)
// check that the CUDA driver supports the installed CUDA runtime version;
// a mismatch causes ORT's CUDA EP to abort() during first inference;
// result is cached — the check runs only once per process
static gboolean _check_cuda_driver_compat(void)
{
  static int cached = -1;  // -1 = unchecked, 0 = incompatible, 1 = ok
  if(cached >= 0) return cached == 1;

  cached = 1;  // assume ok until proven otherwise

  // try unversioned first (available when dev package is installed),
  // then probe versioned names from high to low
  GModule *mod = g_module_open("libcudart.so", G_MODULE_BIND_LAZY | G_MODULE_BIND_LOCAL);
  for(int v = 20; !mod && v >= 11; v--)
  {
    char name[32];
    snprintf(name, sizeof(name), "libcudart.so.%d", v);
    mod = g_module_open(name, G_MODULE_BIND_LAZY | G_MODULE_BIND_LOCAL);
  }
  if(!mod) return TRUE;  // can't check — assume compatible

  typedef int (*cuda_ver_fn)(int *);
  cuda_ver_fn drv_fn = NULL, rt_fn = NULL;
  g_module_symbol(mod, "cudaDriverGetVersion",  (gpointer *)&drv_fn);
  g_module_symbol(mod, "cudaRuntimeGetVersion", (gpointer *)&rt_fn);

  if(drv_fn && rt_fn)
  {
    int drv = 0, rt = 0;
    drv_fn(&drv);
    rt_fn(&rt);
    dt_print(DT_DEBUG_AI,
             "[darktable_ai] CUDA driver %d.%d, runtime %d.%d",
             drv / 1000, (drv % 1000) / 10,
             rt / 1000, (rt % 1000) / 10);
    if(drv < rt)
    {
      dt_print(DT_DEBUG_AI,
               "[darktable_ai] CUDA driver %d.%d is too old for runtime %d.%d — "
               "disabling CUDA to prevent crash. Update your NVIDIA driver.",
               drv / 1000, (drv % 1000) / 10,
               rt / 1000, (rt % 1000) / 10);
      cached = 0;
    }
  }
  g_module_close(mod);
  return cached == 1;
}
#endif  // __linux__

#ifdef ORT_LAZY_LOAD
// redirect fd 2 to /dev/null.  returns the saved fd on success, -1 on failure.
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
// restore fd 2 from the saved fd returned by _stderr_suppress_begin.
static void _stderr_suppress_end(int saved)
{
  if(saved != -1) { dup2(saved, STDERR_FILENO); close(saved); }
}
#endif

// Probe a shared library for OrtGetApiBase to verify it's a valid ORT build.
// Returns the version string (caller must g_free) or NULL on failure.
// Uses BIND_LOCAL to avoid polluting the global symbol namespace.
char *dt_ai_ort_probe_library(const char *path)
{
  if(!path || !path[0]) return NULL;

  GModule *mod = g_module_open(path, G_MODULE_BIND_LAZY | G_MODULE_BIND_LOCAL);
  if(!mod)
  {
    dt_print(DT_DEBUG_AI, "[darktable_ai] probe: failed to open '%s': %s", path, g_module_error());
    return NULL;
  }

  typedef const OrtApiBase *(*OrtGetApiBaseFn)(void);
  OrtGetApiBaseFn get_base = NULL;
  if(!g_module_symbol(mod, "OrtGetApiBase", (gpointer *)&get_base) || !get_base)
  {
    dt_print(DT_DEBUG_AI, "[darktable_ai] probe: OrtGetApiBase not found in '%s'", path);
    g_module_close(mod);
    return NULL;
  }

  const OrtApiBase *base = get_base();
  gchar *version = g_strdup(base->GetVersionString());
  dt_print(DT_DEBUG_AI, "[darktable_ai] probe: ORT %s in '%s'", version, path);
  g_module_close(mod);
  return version;
}

// Probe a library and return version + comma-separated list of supported EPs.
// Both out params are caller-owned (g_free). Returns FALSE if not a valid ORT.
// FALSE before ORT is loaded (no comparison reference yet)
// snapshot the conf values that determine which library / EP / device
// the in-process ORT will be bound to. called eagerly at darktable
// startup so the *_changed_since_load() helpers have a stable reference
// independent of when ORT is lazily initialized
void dt_ai_snapshot_conf_state(void)
{
  gchar *ort_conf = dt_conf_get_string("plugins/ai/ort_library_path");
  g_free(g_ort_conf_path_at_load);
  g_ort_conf_path_at_load = g_strdup(ort_conf ? ort_conf : "");
  g_free(ort_conf);

  g_free(g_loaded_provider);
  g_loaded_provider = dt_conf_get_string(DT_AI_CONF_PROVIDER);
  if(!g_loaded_provider) g_loaded_provider = g_strdup("");

  g_loaded_cuda_device_id     = _device_id_from_conf("plugins/ai/cuda_device_id",
                                                     "DT_CUDA_DEVICE_ID");
  g_loaded_migraphx_device_id = _device_id_from_conf("plugins/ai/migraphx_device_id",
                                                     "DT_MIGRAPHX_DEVICE_ID");
  g_loaded_dml_device_id      = _device_id_from_conf("plugins/ai/dml_device_id",
                                                     "DT_DML_DEVICE_ID");
}

gboolean dt_ai_ort_path_changed_since_load(void)
{
  if(!g_ort_conf_path_at_load) return FALSE;
  gchar *cur = dt_conf_get_string("plugins/ai/ort_library_path");
  const gboolean changed
    = g_strcmp0(cur ? cur : "", g_ort_conf_path_at_load) != 0;
  g_free(cur);
  return changed;
}

gboolean dt_ai_provider_changed_since_load(void)
{
  if(!g_loaded_provider) return FALSE;
  gchar *cur = dt_conf_get_string(DT_AI_CONF_PROVIDER);
  const gboolean changed
    = g_strcmp0(cur ? cur : "", g_loaded_provider) != 0;
  g_free(cur);
  return changed;
}

void dt_ai_device_free(gpointer device)
{
  if(!device) return;
  dt_ai_device_t *d = (dt_ai_device_t *)device;
  g_free(d->name);
  g_free(d);
}

const char *dt_ai_device_conf_key_for_provider(dt_ai_provider_t provider)
{
  switch(provider)
  {
    case DT_AI_PROVIDER_CUDA:     return "plugins/ai/cuda_device_id";
    case DT_AI_PROVIDER_MIGRAPHX: return "plugins/ai/migraphx_device_id";
    case DT_AI_PROVIDER_DIRECTML: return "plugins/ai/dml_device_id";
    default:                      return NULL;
  }
}

gboolean dt_ai_device_id_changed_since_load(dt_ai_provider_t provider)
{
  const char *key = dt_ai_device_conf_key_for_provider(provider);
  if(!key) return FALSE;
  int loaded;
  switch(provider)
  {
    case DT_AI_PROVIDER_CUDA:     loaded = g_loaded_cuda_device_id;     break;
    case DT_AI_PROVIDER_MIGRAPHX: loaded = g_loaded_migraphx_device_id; break;
    case DT_AI_PROVIDER_DIRECTML: loaded = g_loaded_dml_device_id;      break;
    default:                      return FALSE;
  }
  if(loaded < 0) return FALSE;  // ORT not yet loaded
  const int cur = dt_conf_key_exists(key) ? dt_conf_get_int(key) : 0;
  return cur != loaded;
}

// shell out to a command and return its stdout. caller frees. NULL on failure
static gchar *_run_capture(const char *cmd)
{
  gchar *out = NULL;
  GError *err = NULL;
  gint exit_status = 0;
  if(!g_spawn_command_line_sync(cmd, &out, NULL, &exit_status, &err))
  {
    if(err) g_clear_error(&err);
    g_free(out);
    return NULL;
  }
  if(exit_status != 0)
  {
    g_free(out);
    return NULL;
  }
  return out;
}

// CUDA device enumeration via nvidia-smi. respects CUDA_VISIBLE_DEVICES
// since nvidia-smi inherits the parent process env. one row per GPU,
// ordinal in column 1, name in column 2 (CSV)
static GList *_enum_cuda_devices(void)
{
  gchar *out = _run_capture("nvidia-smi --query-gpu=index,name "
                            "--format=csv,noheader,nounits");
  if(!out) return NULL;

  GList *result = NULL;
  gchar **lines = g_strsplit(out, "\n", -1);
  for(gchar **lp = lines; *lp; lp++)
  {
    gchar *line = g_strstrip(*lp);
    if(!line[0]) continue;
    gchar **fields = g_strsplit(line, ",", 2);
    if(fields[0] && fields[1])
    {
      dt_ai_device_t *d = g_new0(dt_ai_device_t, 1);
      d->id = atoi(g_strstrip(fields[0]));
      d->name = g_strdup(g_strstrip(fields[1]));
      result = g_list_append(result, d);
    }
    g_strfreev(fields);
  }
  g_strfreev(lines);
  g_free(out);
  return result;
}

// MIGraphX device enumeration via rocminfo. filter by Device Type=GPU
// to skip the CPU and Ryzen-AI NPU agents. Marketing Name appears
// before Device Type within each agent block, so we hold the most
// recent name and emit it when we hit a GPU agent. id is the ordinal
// index of GPU agents (0, 1, ...) — matches MIGraphX's device_id
static GList *_enum_migraphx_devices(void)
{
#ifndef __linux__
  return NULL;  // ROCm is Linux-only
#else
  gchar *out = _run_capture("rocminfo");
  if(!out) return NULL;

  GList *result = NULL;
  gchar **lines = g_strsplit(out, "\n", -1);
  gchar *pending_name = NULL;
  int gpu_index = 0;
  for(gchar **lp = lines; *lp; lp++)
  {
    gchar *line = *lp;
    const char *mn = strstr(line, "Marketing Name:");
    if(mn)
    {
      const char *colon = strchr(mn, ':');
      if(colon)
      {
        g_free(pending_name);
        pending_name = g_strdup(g_strstrip((gchar *)colon + 1));
      }
      continue;
    }
    if(strstr(line, "Device Type:") && strstr(line, "GPU") && pending_name)
    {
      dt_ai_device_t *d = g_new0(dt_ai_device_t, 1);
      d->id = gpu_index++;
      d->name = pending_name;
      pending_name = NULL;
      result = g_list_append(result, d);
    }
  }
  g_free(pending_name);
  g_strfreev(lines);
  g_free(out);
  return result;
#endif
}

// DirectML device enumeration via DXGI. uses EnumAdapterByGpuPreference
// (DXGI 1.6, Windows 10 1803+) with HIGH_PERFORMANCE so dGPUs come
// before iGPUs. dedupes by AdapterLuid and skips software adapters.
// id is the index in the returned list, which is what DirectML's EP
// takes as device_id
static GList *_enum_directml_devices(void)
{
#ifndef _WIN32
  return NULL;
#else
  IDXGIFactory6 *factory = NULL;
  HRESULT hr = CreateDXGIFactory2(0, &IID_IDXGIFactory6, (void **)&factory);
  if(FAILED(hr) || !factory) return NULL;

  GList *result = NULL;
  // track LUIDs we've already added (dedup against the multi-adapter
  // double-listing quirk on some Windows versions)
  GArray *seen_luids = g_array_new(FALSE, FALSE, sizeof(LUID));
  int next_id = 0;
  for(UINT i = 0; ; i++)
  {
    IDXGIAdapter1 *adapter = NULL;
    if(FAILED(factory->lpVtbl->EnumAdapterByGpuPreference(
         factory, i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
         &IID_IDXGIAdapter1, (void **)&adapter)))
      break;

    DXGI_ADAPTER_DESC1 desc;
    if(SUCCEEDED(adapter->lpVtbl->GetDesc1(adapter, &desc))
       && (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0)
    {
      gboolean dup = FALSE;
      for(guint k = 0; k < seen_luids->len; k++)
      {
        LUID *l = &g_array_index(seen_luids, LUID, k);
        if(l->LowPart == desc.AdapterLuid.LowPart
           && l->HighPart == desc.AdapterLuid.HighPart)
        {
          dup = TRUE;
          break;
        }
      }
      if(!dup)
      {
        g_array_append_val(seen_luids, desc.AdapterLuid);
        gchar *name = g_utf16_to_utf8((const gunichar2 *)desc.Description,
                                      -1, NULL, NULL, NULL);
        dt_ai_device_t *d = g_new0(dt_ai_device_t, 1);
        d->id = next_id++;
        d->name = name ? name : g_strdup("DirectML adapter");
        result = g_list_append(result, d);
      }
    }
    adapter->lpVtbl->Release(adapter);
  }
  g_array_free(seen_luids, TRUE);
  factory->lpVtbl->Release(factory);
  return result;
#endif
}

GList *dt_ai_enum_devices_for_provider(dt_ai_provider_t provider)
{
  switch(provider)
  {
    case DT_AI_PROVIDER_CUDA:     return _enum_cuda_devices();
    case DT_AI_PROVIDER_MIGRAPHX: return _enum_migraphx_devices();
    case DT_AI_PROVIDER_DIRECTML: return _enum_directml_devices();
    // OpenVINO and CoreML self-manage their devices; AUTO/CPU don't apply
    case DT_AI_PROVIDER_AUTO:
    case DT_AI_PROVIDER_CPU:
    case DT_AI_PROVIDER_COREML:
    case DT_AI_PROVIDER_OPENVINO:
    default:
      return NULL;
  }
}

int dt_ai_ort_probe_library_full(const char *path, char **out_version, char **out_eps)
{
  if(!path || !path[0]) return FALSE;
  if(out_version) *out_version = NULL;
  if(out_eps) *out_eps = NULL;

  GModule *mod = g_module_open(path, G_MODULE_BIND_LAZY | G_MODULE_BIND_LOCAL);
  if(!mod) return FALSE;

  typedef const OrtApiBase *(*OrtGetApiBaseFn)(void);
  OrtGetApiBaseFn get_base = NULL;
  if(!g_module_symbol(mod, "OrtGetApiBase", (gpointer *)&get_base) || !get_base)
  {
    g_module_close(mod);
    return FALSE;
  }

  const OrtApiBase *base = get_base();
  if(out_version)
    *out_version = g_strdup(base->GetVersionString());

  if(out_eps)
  {
    GString *eps = g_string_new(NULL);

    // preferred path: ask the OrtApi for the list of providers compiled into
    // the library; this is the only reliable method for ROCm/MIGraphX, since
    // AMD's official builds do not export the legacy C shim symbols
    // (OrtSessionOptionsAppendExecutionProvider_ROCM, _MIGraphX) — ROCm is
    // only reachable through the OrtApi struct
    const OrtApi *probe_api = base->GetApi(ORT_API_VERSION);
    if(!probe_api)
    {
      for(int v = ORT_API_VERSION - 1;
          v >= DT_ORT_MIN_API_VERSION && !probe_api; v--)
        probe_api = base->GetApi(v);
    }
    if(probe_api && probe_api->GetAvailableProviders)
    {
      char **providers = NULL;
      int n_providers = 0;
      if(probe_api->GetAvailableProviders(&providers, &n_providers) == NULL && providers)
      {
        // map ORT provider names to short labels
        static const struct { const char *ort_name; const char *label; } map[] = {
          { "CUDAExecutionProvider",     "CUDA" },
          { "MIGraphXExecutionProvider", "MIGraphX" },
          { "ROCMExecutionProvider",     "ROCm" },
          { "OpenVINOExecutionProvider", "OpenVINO" },
          { "DmlExecutionProvider",      "DirectML" },
          { "CoreMLExecutionProvider",   "CoreML" },
          { "TensorrtExecutionProvider", "TensorRT" },
          { NULL, NULL }
        };
        for(int i = 0; i < n_providers; i++)
        {
          const char *label = NULL;
          for(int j = 0; map[j].ort_name; j++)
          {
            if(g_strcmp0(providers[i], map[j].ort_name) == 0)
            {
              label = map[j].label;
              break;
            }
          }
          // skip CPU here; added below if nothing else matched
          if(!label || g_strcmp0(providers[i], "CPUExecutionProvider") == 0) continue;
          if(eps->len > 0) g_string_append(eps, ", ");
          g_string_append(eps, label);
        }
        if(probe_api->ReleaseAvailableProviders)
          probe_api->ReleaseAvailableProviders(providers, n_providers);
      }
    }

    // fallback: legacy C-shim symbol probing for very old ORT builds where
    // GetAvailableProviders is unavailable
    if(eps->len == 0)
    {
      static const struct { const char *symbol; const char *name; } ep_table[] = {
        { "OrtSessionOptionsAppendExecutionProvider_CUDA",     "CUDA" },
        { "OrtSessionOptionsAppendExecutionProvider_MIGraphX", "MIGraphX" },
        { "OrtSessionOptionsAppendExecutionProvider_ROCM",     "ROCm" },
        { "OrtSessionOptionsAppendExecutionProvider_OpenVINO", "OpenVINO" },
        { "OrtSessionOptionsAppendExecutionProvider_Dml",      "DirectML" },
        { "OrtSessionOptionsAppendExecutionProvider_CoreML",   "CoreML" },
        { NULL, NULL }
      };
      gpointer sym;
      for(int i = 0; ep_table[i].symbol; i++)
      {
        if(g_module_symbol(mod, ep_table[i].symbol, &sym) && sym)
        {
          if(eps->len > 0) g_string_append(eps, ", ");
          g_string_append(eps, ep_table[i].name);
        }
      }
    }

    if(eps->len == 0) g_string_append(eps, "CPU");
    *out_eps = g_string_free(eps, FALSE);
  }

  g_module_close(mod);
  return TRUE;
}

// Scan system and user-space paths for valid ORT libraries.
// Returns a GList of dt_ai_ort_found_t (caller owns list and contents).
GList *dt_ai_ort_find_libraries(void)
{
#ifdef _WIN32
  static const char *system_paths[] = { NULL };
#else
  // system library paths (distro packages)
  static const char *system_paths[] = {
    "/usr/lib/libonnxruntime.so",
    "/usr/lib64/libonnxruntime.so",
    "/usr/lib/x86_64-linux-gnu/libonnxruntime.so",
    "/usr/lib/aarch64-linux-gnu/libonnxruntime.so",
    "/usr/local/lib/libonnxruntime.so",
    "/usr/local/lib64/libonnxruntime.so",
    NULL
  };
#endif

  // user-space paths (install scripts) — scan for ORT libraries
  static const char *subdirs[] = { "onnxruntime-cuda", "onnxruntime-migraphx", "onnxruntime-openvino" };
  gchar *user_paths[3] = { NULL };

#ifdef _WIN32
  // on Windows the install script puts libraries under %LOCALAPPDATA%
  const char *local_app = g_getenv("LOCALAPPDATA");
  const char *base_dir = local_app ? local_app : g_get_home_dir();
#else
  const char *base_dir = g_get_home_dir();
#endif

  for(int i = 0; i < 3; i++)
  {
#ifdef _WIN32
    gchar *dir = g_build_filename(base_dir, subdirs[i], NULL);
#else
    gchar *dir = g_build_filename(base_dir, ".local/lib", subdirs[i], NULL);
#endif
    if(g_file_test(dir, G_FILE_TEST_IS_DIR))
    {
#ifdef _WIN32
      // look for onnxruntime.dll
      gchar *exact = g_build_filename(dir, "onnxruntime.dll", NULL);
      if(g_file_test(exact, G_FILE_TEST_EXISTS))
      {
        user_paths[i] = exact;
      }
      else
      {
        g_free(exact);
        GDir *d = g_dir_open(dir, 0, NULL);
        if(d)
        {
          const gchar *name;
          while((name = g_dir_read_name(d)))
          {
            if(g_str_has_prefix(name, "onnxruntime") &&
               g_str_has_suffix(name, ".dll"))
            {
              user_paths[i] = g_build_filename(dir, name, NULL);
              break;
            }
          }
          g_dir_close(d);
        }
      }
#else
      gchar *exact = g_build_filename(dir, "libonnxruntime.so", NULL);
      if(g_file_test(exact, G_FILE_TEST_EXISTS))
      {
        user_paths[i] = exact;
      }
      else
      {
        g_free(exact);
        GDir *d = g_dir_open(dir, 0, NULL);
        if(d)
        {
          const gchar *name;
          while((name = g_dir_read_name(d)))
          {
            if(g_str_has_prefix(name, "libonnxruntime.so."))
            {
              user_paths[i] = g_build_filename(dir, name, NULL);
              break;
            }
          }
          g_dir_close(d);
        }
      }
#endif
    }
    g_free(dir);
  }

  GList *results = NULL;

  // probe system paths
  for(int i = 0; system_paths[i]; i++)
  {
    if(!g_file_test(system_paths[i], G_FILE_TEST_EXISTS)) continue;
    char *version = NULL, *ep = NULL;
    if(dt_ai_ort_probe_library_full(system_paths[i], &version, &ep))
    {
      dt_ai_ort_found_t *f = g_new0(dt_ai_ort_found_t, 1);
      f->path = g_strdup(system_paths[i]);
      f->version = version;
      f->eps = ep;
      results = g_list_append(results, f);
    }
    else
    {
      g_free(version);
      g_free(ep);
    }
  }

  // probe user-space paths
  for(int i = 0; i < 3; i++)
  {
    if(!user_paths[i]) continue;
    char *version = NULL, *ep = NULL;
    if(dt_ai_ort_probe_library_full(user_paths[i], &version, &ep))
    {
      dt_ai_ort_found_t *f = g_new0(dt_ai_ort_found_t, 1);
      f->path = user_paths[i];
      f->version = version;
      f->eps = ep;
      user_paths[i] = NULL; // ownership transferred
      results = g_list_append(results, f);
    }
    else
    {
      g_free(version);
      g_free(ep);
    }
  }

  for(int i = 0; i < 3; i++) g_free(user_paths[i]);
  return results;
}

void dt_ai_ort_found_free(dt_ai_ort_found_t *f)
{
  if(!f) return;
  g_free(f->path);
  g_free(f->version);
  g_free(f->eps);
  g_free(f);
}

// Load ORT API from a dynamically loaded module. Returns NULL on failure.
static const OrtApi *_ort_api_from_module(GModule *mod, const char *label)
{
  typedef const OrtApiBase *(*OrtGetApiBaseFn)(void);
  OrtGetApiBaseFn get_api_base = NULL;
  if(!g_module_symbol(mod, "OrtGetApiBase", (gpointer *)&get_api_base) || !get_api_base)
  {
    dt_print(DT_DEBUG_AI, "[darktable_ai] OrtGetApiBase symbol not found in '%s'", label);
    return NULL;
  }
  const OrtApiBase *base = get_api_base();
  const char *lib_version = base->GetVersionString();
  dt_print(DT_DEBUG_AI, "[darktable_ai] loaded ORT %s from '%s'", lib_version, label);

  // try the compiled API version first, then fall back to lower versions
  // so that older ORT libraries (e.g. MIGraphX on ROCm 6.x) still work
  const OrtApi *api = base->GetApi(ORT_API_VERSION);
  if(!api)
  {
    // minimum API version 14 = ORT 1.14, required for ONNX opset 18
    for(int v = ORT_API_VERSION - 1; v >= DT_ORT_MIN_API_VERSION; v--)
    {
      api = base->GetApi(v);
      if(api)
      {
        dt_print(DT_DEBUG_AI,
                 "[darktable_ai] ORT %s: using API version %d (compiled for %d)",
                 lib_version, v, ORT_API_VERSION);
        break;
      }
    }
    if(!api)
      dt_print(DT_DEBUG_AI,
               "[darktable_ai] ORT %s does not support any compatible API version",
               lib_version);
  }
  return api;
}

static gpointer _init_ort_api(gpointer data)
{
  (void)data;
  const OrtApi *api = NULL;

  // Custom ORT library: check darktablerc preference first, then env var.
  // This allows users to point to a GPU-enabled ORT build (e.g. CUDA or
  // ROCm) without rebuilding darktable. On Linux this overrides the
  // compile-time default; on Windows/macOS it dynamically loads a
  // user-supplied library instead of the bundled DirectML/CoreML one.
  gchar *ort_conf = dt_conf_get_string("plugins/ai/ort_library_path");
  // snapshots are now taken eagerly at startup via dt_ai_snapshot_conf_state()
  const char *ort_env = g_getenv("DT_ORT_LIBRARY");
  const char *ort_override = (ort_conf && ort_conf[0]) ? ort_conf
                           : (ort_env && ort_env[0]) ? ort_env
                           : NULL;

  if(ort_override)
  {
#ifdef _WIN32
    // set the DLL search directory to the custom ORT location so that
    // provider DLLs (onnxruntime_providers_cuda.dll) and their bundled
    // dependencies (cuDNN, cublas) are found by LoadLibrary;
    // the install script copies all required DLLs into this directory
    gchar *ort_dir = g_path_get_dirname(ort_override);
    if(ort_dir)
    {
      wchar_t *wdir = g_utf8_to_utf16(ort_dir, -1, NULL, NULL, NULL);
      if(wdir)
      {
        SetDllDirectoryW(wdir);
        dt_print(DT_DEBUG_AI, "[darktable_ai] set DLL directory: %s", ort_dir);
        g_free(wdir);
      }
      g_free(ort_dir);
    }
#endif
    GModule *ort_mod = g_module_open(ort_override, G_MODULE_BIND_LAZY);
    if(!ort_mod)
    {
      dt_print(DT_DEBUG_AI,
               "[darktable_ai] failed to load ORT library '%s': %s",
               ort_override, g_module_error());
      goto done;
    }
    g_ort_module = ort_mod;  // keep handle for _try_provider EP lookups
    api = _ort_api_from_module(ort_mod, ort_override);
  }
#ifdef ORT_LAZY_LOAD
  else
  {
    // Linux default: lazy-load the bundled or system ORT library.
    // Suppress stderr during load - Ubuntu/Debian's system ORT links against
    // libonnx, causing harmless "already registered" ONNX schema warnings.
    const int saved = _stderr_suppress_begin();
    GModule *ort_mod = g_module_open(ORT_LIBRARY_PATH, G_MODULE_BIND_LAZY);
    _stderr_suppress_end(saved);

    // If the linker can't find it by name alone (e.g. custom install prefix
    // like /opt/darktable), try the darktable plugin directory where cmake
    // installs the bundled ORT alongside other libraries.
    if(!ort_mod && darktable.plugindir)
    {
      gchar *bundled = g_build_filename(darktable.plugindir, ORT_LIBRARY_PATH, NULL);
      const int saved2 = _stderr_suppress_begin();
      ort_mod = g_module_open(bundled, G_MODULE_BIND_LAZY);
      _stderr_suppress_end(saved2);
      if(ort_mod)
        dt_print(DT_DEBUG_AI, "[darktable_ai] loaded ORT from plugindir: %s", bundled);
      g_free(bundled);
    }

    if(!ort_mod)
    {
      dt_print(DT_DEBUG_AI,
               "[darktable_ai] failed to load ORT library '%s': %s",
               ORT_LIBRARY_PATH, g_module_error());
      goto done;
    }
    api = _ort_api_from_module(ort_mod, ORT_LIBRARY_PATH);
  }
#else
  else
  {
    // Windows/macOS: use the directly linked ORT library (DirectML/CoreML).
    const OrtApiBase *base = OrtGetApiBase();
    dt_print(DT_DEBUG_AI, "[darktable_ai] loaded ORT %s (bundled)",
             base->GetVersionString());
    api = base->GetApi(ORT_API_VERSION);
  }
#endif

done:
  g_free(ort_conf);
  if(!api)
  {
    dt_print(DT_DEBUG_AI, "[darktable_ai] failed to init ONNX runtime API");
  }
  else
  {
    g_ort = api;
    gchar *prov_str = dt_conf_get_string(DT_AI_CONF_PROVIDER);
    dt_print(DT_DEBUG_AI, "[darktable_ai] execution provider: %s",
             prov_str && prov_str[0] ? prov_str : "auto");
    g_free(prov_str);
  }
  return (gpointer)api;
}

#if defined(__linux__)
// configure AMD GPU caches (MIOpen kernel db + MIGraphX compiled-program
// cache) via environment variables. these MUST be set before any ORT
// internals query them — in practice that means before the MIGraphX
// provider .so is loaded, which happens during the first model load.
// setting them at OrtEnv init guarantees correct ordering
//
// MIOpen: without MIOPEN_USER_DB_PATH, the kernel find-db lives in a
// temp location and may not survive between runs on consumer/iGPU AMD
// where no prebuilt kdb ships — every launch recompiles conv shapes
// from source via comgr/clang (10+ minutes on a Radeon iGPU)
// MIGraphX: without ORT_MIGRAPHX_MODEL_CACHE_PATH, recompiles the whole
// graph (compile_program: Begin/Complete) on every launch
//
// only sets vars the user hasn't already overridden so power users
// keep control
static void _setup_amd_caches(void)
{
  if(!g_file_test("/opt/rocm", G_FILE_TEST_IS_DIR))
    return;

  char cachedir[PATH_MAX] = { 0 };
  dt_loc_get_user_cache_dir(cachedir, sizeof(cachedir));

  gchar *miopen_dir = g_build_filename(cachedir, "ai", "amd", "miopen", NULL);
  g_mkdir_with_parents(miopen_dir, 0700);
  g_setenv("MIOPEN_USER_DB_PATH", miopen_dir, FALSE);
  g_setenv("MIOPEN_CUSTOM_CACHE_DIR", miopen_dir, FALSE);
  dt_print(DT_DEBUG_AI, "[darktable_ai] MIOpen cache: %s", miopen_dir);
  g_free(miopen_dir);

  gchar *migraphx_dir = g_build_filename(cachedir, "ai", "amd", "migraphx", NULL);
  g_mkdir_with_parents(migraphx_dir, 0700);
  g_setenv("ORT_MIGRAPHX_CACHE_PATH", migraphx_dir, FALSE);
  g_setenv("ORT_MIGRAPHX_MODEL_CACHE_PATH", migraphx_dir, FALSE);
  dt_print(DT_DEBUG_AI, "[darktable_ai] MIGraphX cache: %s", migraphx_dir);
  g_free(migraphx_dir);
}
#endif

// try to enable OpenVINO with disk cache via the dedicated V2 API;
// SessionOptionsAppendExecutionProvider_OpenVINO_V2 (OrtApi, since 1.17)
// takes string key/value pairs directly — version-stable, no struct ABI
// to mismatch, and documented as the recommended path. OpenVINO compiles
// ONNX → its internal IR on first inference (5-30s for a UNet), then
// writes .blob files to cache_dir for instant reload on subsequent runs
static gboolean _try_openvino_with_cache(OrtSessionOptions *session_opts)
{
  if(!g_ort || !g_ort->SessionOptionsAppendExecutionProvider_OpenVINO_V2)
    return FALSE;

  char cachedir[PATH_MAX] = { 0 };
  dt_loc_get_user_cache_dir(cachedir, sizeof(cachedir));
  gchar *ov_dir = g_build_filename(cachedir, "ai", "intel", "openvino", NULL);
  g_mkdir_with_parents(ov_dir, 0700);

  dt_print(DT_DEBUG_AI,
           "[darktable_ai] attempting Intel OpenVINO (cache: %s)...", ov_dir);

  const char *keys[] = { "device_type", "cache_dir" };
  const char *values[] = { "AUTO", ov_dir };
  OrtStatus *status = g_ort->SessionOptionsAppendExecutionProvider_OpenVINO_V2(
    session_opts, keys, values, 2);

  if(status)
  {
    dt_print(DT_DEBUG_AI,
             "[darktable_ai] OpenVINO (with cache) failed: %s",
             g_ort->GetErrorMessage(status));
    g_ort->ReleaseStatus(status);
    g_free(ov_dir);
    return FALSE;
  }
  dt_print(DT_DEBUG_AI, "[darktable_ai] Intel OpenVINO enabled with disk cache.");
  g_free(ov_dir);
  return TRUE;
}

static gpointer _init_ort_env(gpointer data)
{
  (void)data;
#if defined(__linux__)
  _setup_amd_caches();
#endif
  OrtEnv *env = NULL;
#ifdef ORT_LAZY_LOAD
  // ORT may emit additional schema-registration noise during env creation.
  const int saved = _stderr_suppress_begin();
#endif
  OrtStatus *status = g_ort->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "DarktableAI", &env);
#ifdef ORT_LAZY_LOAD
  _stderr_suppress_end(saved);
#endif
  if(status)
  {
    dt_print(DT_DEBUG_AI,
             "[darktable_ai] failed to create ORT environment: %s",
             g_ort->GetErrorMessage(status));
    g_ort->ReleaseStatus(status);
    return NULL;
  }
  g_env = env;
  return (gpointer)env;
}

// map ONNX tensor element type to our dt_ai_dtype_t.
// returns TRUE on success, FALSE if the type is unsupported
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

// compute total element count from shape dimensions with overflow checking.
// returns the product of all shape dimensions, or -1 if any dimension is
// non-positive or the multiplication would overflow int64_t
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

// map dt_ai_dtype_t to ONNX type and element size.
// returns TRUE on success, FALSE if the type is unsupported
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

// float16 conversion utilities
// based on: https://gist.github.com/rygorous/2156668
// handles zero, denormals, and infinity correctly
static uint16_t _float_to_half(float f)
{
  uint32_t x;
  memcpy(&x, &f, sizeof(x));
  uint32_t sign = (x >> 31) & 1;
  uint32_t exp = (x >> 23) & 0xFF;
  uint32_t mant = x & 0x7FFFFF;

  // handle zero and float32 denormals (too small for float16)
  if(exp == 0)
    return (uint16_t)(sign << 15);

  // handle infinity / NaN
  if(exp == 255)
    return (uint16_t)((sign << 15) | 0x7C00 | (mant ? 1 : 0));

  // re-bias exponent from float32 (bias 127) to float16 (bias 15)
  const int new_exp = (int)exp - 127 + 15;

  if(new_exp <= 0)
  {
    // encode as float16 denormal: shift mantissa with implicit leading 1
    // the implicit 1 bit plus 10 mantissa bits, shifted right by (1 - new_exp)
    const int shift = 1 - new_exp;
    if(shift > 24)
      return (uint16_t)(sign << 15);       // too small even for denormal
    const uint64_t full_mant = (1 << 23) | mant; // restore implicit leading 1
    const uint16_t half_mant = (uint16_t)(full_mant >> (13 + shift));
    return (uint16_t)((sign << 15) | half_mant);
  }
  else if(new_exp >= 31)
  {
    // overflow to infinity
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
      // zero
      uint32_t result = (sign << 31);
      float f;
      memcpy(&f, &result, 4);
      return f;
    }
    // denormal: value = (-1)^sign * 2^(-14) * (mant / 1024)
    // convert to float32 by normalizing: find leading 1 and shift
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
    // inf / NaN
    uint32_t result = (sign << 31) | 0x7F800000 | (mant << 13);
    float f;
    memcpy(&f, &result, 4);
    return f;
  }

  // normalized
  const uint32_t new_exp = exp + 127 - 15;
  const uint32_t result = (sign << 31) | (new_exp << 23) | (mant << 13);
  float f;
  memcpy(&f, &result, sizeof(f));
  return f;
}

// Look up the device name for `device_id` from the provider's enumeration.
// Returns a newly-allocated string (caller frees) or NULL if no match.
static gchar *_lookup_device_name(dt_ai_provider_t provider, int device_id)
{
  GList *devices = dt_ai_enum_devices_for_provider(provider);
  gchar *name = NULL;
  for(GList *l = devices; l; l = g_list_next(l))
  {
    dt_ai_device_t *d = l->data;
    if(d->id == device_id) { name = g_strdup(d->name); break; }
  }
  g_list_free_full(devices, dt_ai_device_free);
  return name;
}

// try to find and call an ORT execution provider function at runtime via
// dynamic symbol lookup (GModule/dlsym).  returns TRUE if the provider was
// enabled successfully, FALSE otherwise.
// most providers take (OrtSessionOptions*, uint32_t device_id), but OpenVINO
// takes (OrtSessionOptions*, const char* device_type).  pass device_type for
// string-argument providers, NULL for integer-argument ones
static gboolean _try_provider(OrtSessionOptions *session_opts,
                              const char *symbol_name,
                              const char *provider_name,
                              const char *device_type,
                              uint32_t flags,
                              dt_ai_provider_t provider)
{
  OrtStatus *status = NULL;
  gboolean ok = FALSE;

  dt_print(DT_DEBUG_AI, "[darktable_ai] attempting to enable %s...", provider_name);

#ifdef _WIN32
  void *func_ptr = NULL;
  // if a custom ORT library was loaded (e.g. CUDA build), look up the
  // EP symbol there — the bundled DirectML onnxruntime.dll won't have it
  if(g_ort_module)
  {
    g_module_symbol(g_ort_module, symbol_name, &func_ptr);
  }
  if(!func_ptr)
  {
    HMODULE h = GetModuleHandleA("onnxruntime.dll");
    if(!h)
      h = LoadLibraryA("onnxruntime.dll");
    if(h)
      func_ptr = (void *)GetProcAddress(h, symbol_name);
  }
#else
#if defined(__linux__)
  // before enabling CUDA EP, verify the driver supports the installed runtime;
  // a driver/runtime version mismatch causes ORT to abort() during inference
  if(strstr(symbol_name, "CUDA") && !_check_cuda_driver_compat())
    return FALSE;
#endif
  GModule *mod = g_module_open(NULL, 0);
  void *func_ptr = NULL;
  if(mod)
    g_module_symbol(mod, symbol_name, &func_ptr);
#endif

  if(func_ptr)
  {
    if(device_type)
    {
      // string-argument providers (e.g. OpenVINO)
      typedef OrtStatus *(*ProviderAppenderStr)(OrtSessionOptions *, const char *);
      ProviderAppenderStr appender = (ProviderAppenderStr)func_ptr;
      status = appender(session_opts, device_type);
    }
    else
    {
      // integer-argument providers (CUDA, CoreML, DML, MIGraphX, ROCm)
      typedef OrtStatus *(*ProviderAppenderInt)(OrtSessionOptions *, uint32_t);
      ProviderAppenderInt appender = (ProviderAppenderInt)func_ptr;
      status = appender(session_opts, flags);
    }
    if(!status)
    {
      // for integer-argument (device-id) providers, also log which GPU was selected
      gchar *dev_name = device_type ? NULL : _lookup_device_name(provider, (int)flags);
      if(dev_name)
        dt_print(DT_DEBUG_AI, "[darktable_ai] %s enabled successfully on device %d: %s",
                 provider_name, (int)flags, dev_name);
      else
        dt_print(DT_DEBUG_AI, "[darktable_ai] %s enabled successfully.", provider_name);
      g_free(dev_name);
      ok = TRUE;
    }
    else
    {
      dt_print(DT_DEBUG_AI,
               "[darktable_ai] %s enable failed: %s",
               provider_name, g_ort->GetErrorMessage(status));
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

// pick a GPU device_id: env var wins, then conf, fallback to 0.
// each multi-GPU-capable EP has its own conf key + env var so users
// switching between providers don't carry stale indices across vendors
static int _device_id_from_conf(const char *conf_key, const char *env_var)
{
  const char *env_val = g_getenv(env_var);
  if(env_val && env_val[0])
  {
    const int v = atoi(env_val);
    return v >= 0 ? v : 0;
  }
  if(dt_conf_key_exists(conf_key))
  {
    const int v = dt_conf_get_int(conf_key);
    return v >= 0 ? v : 0;
  }
  return 0;
}

static void
_enable_acceleration(OrtSessionOptions *session_opts,
                     dt_ai_provider_t provider,
                     uint32_t coreml_flags)
{
  switch(provider)
  {
  case DT_AI_PROVIDER_CPU:
    // CPU only - don't enable any accelerator
    dt_print(DT_DEBUG_AI, "[darktable_ai] using CPU only (no hardware acceleration)");
    break;

  case DT_AI_PROVIDER_COREML:
#if defined(__APPLE__)
    _try_provider(
      session_opts,
      "OrtSessionOptionsAppendExecutionProvider_CoreML",
      "Apple CoreML", NULL, coreml_flags, DT_AI_PROVIDER_COREML);
#else
    dt_print(DT_DEBUG_AI, "[darktable_ai] apple CoreML not available on this platform");
#endif
    break;

  case DT_AI_PROVIDER_CUDA:
  {
    const int dev = _device_id_from_conf("plugins/ai/cuda_device_id",
                                         "DT_CUDA_DEVICE_ID");
    _try_provider(session_opts, "OrtSessionOptionsAppendExecutionProvider_CUDA",
                  "NVIDIA CUDA", NULL, (uint32_t)dev, DT_AI_PROVIDER_CUDA);
    break;
  }

  case DT_AI_PROVIDER_MIGRAPHX:
  {
    // MIGraphX reads its cache env vars once at provider library
    // load time, so they must be set before CreateEnv() — see
    // _setup_amd_caches() above. OpenVINO (below) takes options
    // per-session, so its cache path is passed inline here
    const int dev = _device_id_from_conf("plugins/ai/migraphx_device_id",
                                         "DT_MIGRAPHX_DEVICE_ID");
    if(!_try_provider(session_opts, "OrtSessionOptionsAppendExecutionProvider_MIGraphX",
                      "AMD MIGraphX", NULL, (uint32_t)dev, DT_AI_PROVIDER_MIGRAPHX))
      _try_provider(session_opts, "OrtSessionOptionsAppendExecutionProvider_ROCM",
                    "AMD ROCm (legacy)", NULL, (uint32_t)dev, DT_AI_PROVIDER_MIGRAPHX);
    break;
  }

  case DT_AI_PROVIDER_OPENVINO:
    if(!_try_openvino_with_cache(session_opts))
      _try_provider(session_opts, "OrtSessionOptionsAppendExecutionProvider_OpenVINO",
                    "Intel OpenVINO", "AUTO", 0, DT_AI_PROVIDER_OPENVINO);
    break;

  case DT_AI_PROVIDER_DIRECTML:
#if defined(_WIN32)
  {
    const int dev = _device_id_from_conf("plugins/ai/dml_device_id",
                                         "DT_DML_DEVICE_ID");
    _try_provider(session_opts,
                  "OrtSessionOptionsAppendExecutionProvider_DML",
                  "Windows DirectML", NULL, (uint32_t)dev, DT_AI_PROVIDER_DIRECTML);
  }
#else
    dt_print(DT_DEBUG_AI, "[darktable_ai] windows DirectML not available on this platform");
#endif
    break;

  case DT_AI_PROVIDER_AUTO:
  default:
    // auto-detect best provider based on platform
#if defined(__APPLE__)
    _try_provider(
      session_opts,
      "OrtSessionOptionsAppendExecutionProvider_CoreML",
      "Apple CoreML", NULL, coreml_flags, DT_AI_PROVIDER_COREML);
#elif defined(_WIN32)
    {
      const int dev = _device_id_from_conf("plugins/ai/dml_device_id",
                                           "DT_DML_DEVICE_ID");
      _try_provider(session_opts,
                    "OrtSessionOptionsAppendExecutionProvider_DML",
                    "Windows DirectML", NULL, (uint32_t)dev, DT_AI_PROVIDER_DIRECTML);
    }
#elif defined(__linux__)
    // try CUDA first, then MIGraphX (cache configured at env init)
    {
      const int cuda_dev = _device_id_from_conf("plugins/ai/cuda_device_id",
                                                "DT_CUDA_DEVICE_ID");
      const int amd_dev  = _device_id_from_conf("plugins/ai/migraphx_device_id",
                                                "DT_MIGRAPHX_DEVICE_ID");
      if(!_try_provider(
           session_opts,
           "OrtSessionOptionsAppendExecutionProvider_CUDA",
           "NVIDIA CUDA", NULL, (uint32_t)cuda_dev, DT_AI_PROVIDER_CUDA))
      {
        if(!_try_provider(
             session_opts,
             "OrtSessionOptionsAppendExecutionProvider_MIGraphX",
             "AMD MIGraphX", NULL, (uint32_t)amd_dev, DT_AI_PROVIDER_MIGRAPHX))
          _try_provider(
            session_opts,
            "OrtSessionOptionsAppendExecutionProvider_ROCM",
            "AMD ROCm (legacy)", NULL, (uint32_t)amd_dev, DT_AI_PROVIDER_MIGRAPHX);
      }
    }
#endif
    break;
  }
}

// provider probe

int dt_ai_probe_provider(dt_ai_provider_t provider)
{
  // AUTO and CPU are always available
  if(provider == DT_AI_PROVIDER_AUTO || provider == DT_AI_PROVIDER_CPU)
    return 1;

  // refuse to probe when AI is disabled
  if(!dt_conf_get_bool("plugins/ai/enabled"))
    return 0;

  // ensure ORT API is initialized
  g_once(&g_ort_once, _init_ort_api, NULL);
  if(!g_ort) return 0;

  g_once(&g_env_once, _init_ort_env, NULL);
  if(!g_env) return 0;

  // create temporary session options for the probe
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
    ok = _try_provider(opts, "OrtSessionOptionsAppendExecutionProvider_CoreML",
                       "Apple CoreML", NULL, 0, DT_AI_PROVIDER_COREML);
    break;
  case DT_AI_PROVIDER_CUDA:
  {
    const int dev = _device_id_from_conf("plugins/ai/cuda_device_id",
                                         "DT_CUDA_DEVICE_ID");
    ok = _try_provider(opts, "OrtSessionOptionsAppendExecutionProvider_CUDA",
                       "NVIDIA CUDA", NULL, (uint32_t)dev, DT_AI_PROVIDER_CUDA);
    break;
  }
  case DT_AI_PROVIDER_MIGRAPHX:
  {
    const int dev = _device_id_from_conf("plugins/ai/migraphx_device_id",
                                         "DT_MIGRAPHX_DEVICE_ID");
    ok = _try_provider(opts, "OrtSessionOptionsAppendExecutionProvider_MIGraphX",
                       "AMD MIGraphX", NULL, (uint32_t)dev, DT_AI_PROVIDER_MIGRAPHX)
      || _try_provider(opts, "OrtSessionOptionsAppendExecutionProvider_ROCM",
                       "AMD ROCm (legacy)", NULL, (uint32_t)dev, DT_AI_PROVIDER_MIGRAPHX);
    break;
  }
  case DT_AI_PROVIDER_OPENVINO:
    ok = _try_provider(opts, "OrtSessionOptionsAppendExecutionProvider_OpenVINO",
                       "Intel OpenVINO", "AUTO", 0, DT_AI_PROVIDER_OPENVINO);
    break;
  case DT_AI_PROVIDER_DIRECTML:
  {
    const int dev = _device_id_from_conf("plugins/ai/dml_device_id",
                                         "DT_DML_DEVICE_ID");
    ok = _try_provider(opts, "OrtSessionOptionsAppendExecutionProvider_DML",
                       "Windows DirectML", NULL, (uint32_t)dev, DT_AI_PROVIDER_DIRECTML);
    break;
  }
  default:
    break;
  }

  g_ort->ReleaseSessionOptions(opts);
  return ok ? 1 : 0;
}

// ONNX Model Loading

// load ONNX model from model_dir/model_file with dimension overrides.
// if model_file is NULL, defaults to "model.onnx".
dt_ai_context_t *
dt_ai_onnx_load_ext(const char *model_dir, const char *model_file,
                    dt_ai_provider_t provider, dt_ai_opt_level_t opt_level,
                    const dt_ai_dim_override_t *dim_overrides, int n_overrides,
                    uint32_t ep_flags)
{
  if(!model_dir)
    return NULL;

  char *onnx_path
    = g_build_filename(model_dir, model_file ? model_file : "model.onnx", NULL);
  if(!g_file_test(onnx_path, G_FILE_TEST_EXISTS))
  {
    dt_print(DT_DEBUG_AI, "[darktable_ai] model file missing: %s", onnx_path);
    g_free(onnx_path);
    return NULL;
  }

  // lazy init ORT API and shared environment on first load
  g_once(&g_ort_once, _init_ort_api, NULL);
  if(!g_ort)
  {
    g_free(onnx_path);
    return NULL;
  }

  g_once(&g_env_once, _init_ort_env, NULL);
  if(!g_env)
  {
    g_free(onnx_path);
    return NULL;
  }

  dt_print(DT_DEBUG_AI, "[darktable_ai] loading: %s", onnx_path);

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

  // let ORT auto-select thread count (pass 0)
  status = g_ort->SetIntraOpNumThreads(session_opts, 0);
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

  // override symbolic dimensions (fixes shape inference for dynamic-shape models)
  for(int i = 0; i < n_overrides; i++)
  {
    if(!dim_overrides[i].name) continue;
    status = g_ort->AddFreeDimensionOverrideByName(session_opts,
                                                   dim_overrides[i].name,
                                                   dim_overrides[i].value);
    if(status)
    {
      dt_print(DT_DEBUG_AI, "[darktable_ai] dim override '%s' failed: %s",
               dim_overrides[i].name, g_ort->GetErrorMessage(status));
      g_ort->ReleaseStatus(status);
    }
  }

  // optimize: enable hardware acceleration (AMD caches set at env init)
  _enable_acceleration(session_opts, provider, ep_flags);

#ifdef _WIN32
  // on windows, CreateSession expects a wide character string
  wchar_t *onnx_path_wide = (wchar_t *)g_utf8_to_utf16(onnx_path, -1, NULL, NULL, NULL);
  status = g_ort->CreateSession(g_env, onnx_path_wide, session_opts, &ctx->session);
#else
  status = g_ort->CreateSession(g_env, onnx_path, session_opts, &ctx->session);
#endif

  // smart fallback: try progressively simpler configurations
  // 1. provider + BASIC optimization
  // 2. CPU + full optimization
  // 3. CPU + BASIC optimization
  // 4. CPU + disabled optimization (last resort)
  // skip provider + DISABLE_ALL -- always slower than CPU
  if(status)
  {
    typedef struct
    {
      dt_ai_provider_t prov;
      GraphOptimizationLevel opt;
      const char *desc;
    } _fallback_t;

    const _fallback_t fallbacks[] = {
      { provider,             ORT_ENABLE_BASIC, "provider + basic opt" },
      { DT_AI_PROVIDER_CPU,   ort_opt,          "CPU + full opt" },
      { DT_AI_PROVIDER_CPU,   ORT_ENABLE_BASIC, "CPU + basic opt" },
      { DT_AI_PROVIDER_CPU,   ORT_DISABLE_ALL,  "CPU + no opt" },
    };
    const int n_fallbacks = sizeof(fallbacks) / sizeof(fallbacks[0]);

    for(int fb = 0; fb < n_fallbacks && status; fb++)
    {
      // skip redundant attempts
      if(fallbacks[fb].opt >= ort_opt
         && fallbacks[fb].prov == provider)
        continue;
      if(fallbacks[fb].prov == provider
         && provider == DT_AI_PROVIDER_CPU
         && fallbacks[fb].opt >= ort_opt)
        continue;

      dt_print(DT_DEBUG_AI,
               "[darktable_ai] session failed: %s - retrying with %s",
               g_ort->GetErrorMessage(status), fallbacks[fb].desc);
      g_ort->ReleaseStatus(status);
      g_ort->ReleaseSessionOptions(session_opts);

      status = g_ort->CreateSessionOptions(&session_opts);
      if(status) break;
      OrtStatus *s = g_ort->SetIntraOpNumThreads(session_opts, 0);
      if(s) g_ort->ReleaseStatus(s);
      s = g_ort->SetSessionGraphOptimizationLevel(session_opts, fallbacks[fb].opt);
      if(s) g_ort->ReleaseStatus(s);
      for(int i = 0; i < n_overrides; i++)
      {
        if(!dim_overrides[i].name) continue;
        s = g_ort->AddFreeDimensionOverrideByName(session_opts,
                                                  dim_overrides[i].name,
                                                  dim_overrides[i].value);
        if(s) g_ort->ReleaseStatus(s);
      }
      if(fallbacks[fb].prov != DT_AI_PROVIDER_CPU)
        _enable_acceleration(session_opts, fallbacks[fb].prov, ep_flags);
#ifdef _WIN32
      status = g_ort->CreateSession(g_env, onnx_path_wide, session_opts, &ctx->session);
#else
      status = g_ort->CreateSession(g_env, onnx_path, session_opts, &ctx->session);
#endif
    }
  }

#ifdef _WIN32
  g_free(onnx_path_wide);
#endif
  g_ort->ReleaseSessionOptions(session_opts);
  g_free(onnx_path);

  if(status)
  {
    dt_print(DT_DEBUG_AI,
             "[darktable_ai] failed to create session: %s",
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

  // resolve IO names
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

    // get input type
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
      dt_print(DT_DEBUG_AI,
               "[darktable_ai] unsupported ONNX input type %d for input %zu",
               type, i);
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

    // get output type
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
      dt_print(DT_DEBUG_AI,
               "[darktable_ai] unsupported ONNX output type %d for output %zu",
               type, i);
      g_ort->ReleaseTypeInfo(typeinfo);
      dt_ai_unload_model(ctx);
      return NULL;
    }

    g_ort->ReleaseTypeInfo(typeinfo);
  }

  // detect dynamic output shapes (any dim <= 0 means symbolic/unknown).
  // when detected, dt_ai_run() will let ORT allocate outputs during
  // execution and copy the results back to the caller's buffer
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
                   "[darktable_ai] output[%zu] has dynamic dims — using ORT-allocated outputs",
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
    dt_print(DT_DEBUG_AI,
             "[darktable_ai] IO count mismatch: expected %zu/%zu, got %d/%d",
             ctx->input_count, ctx->output_count, num_inputs, num_outputs);
    return -2;
  }

  // run
  OrtStatus *status = NULL;
  int ret = 0;

  // track temporary buffers to free later
  void **temp_input_buffers = g_new0(void *, num_inputs);

  // create input tensors
  OrtValue **input_tensors = g_new0(OrtValue *, num_inputs);
  OrtValue **output_tensors = g_new0(OrtValue *, num_outputs);
  const char **input_names = (const char **)ctx->input_names; // cast for Run()

  for(int i = 0; i < num_inputs; i++)
  {
    const int64_t element_count = _safe_element_count(inputs[i].shape, inputs[i].ndim);
    if(element_count < 0)
    {
      dt_print(DT_DEBUG_AI,
               "[darktable_ai] invalid or overflowing shape for input[%d]", i);
      ret = -4;
      goto cleanup;
    }

    ONNXTensorElementDataType onnx_type = ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT;
    size_t type_size = sizeof(float);
    void *data_ptr = inputs[i].data;

    // check for type mismatch (float -> half)
    if(inputs[i].type == DT_AI_FLOAT && ctx->input_types[i] == DT_AI_FLOAT16)
    {
      dt_print(DT_DEBUG_AI,
              "[darktable_ai] auto-converting input[%d] float32 -> float16", i);
      // auto-convert float32 -> float16
      onnx_type = ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16;
      type_size = sizeof(uint16_t); // half is 2 bytes

      if((size_t)element_count > SIZE_MAX / type_size)
      {
        dt_print(DT_DEBUG_AI, "[darktable_ai] tensor size overflow for input[%d]", i);
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
        dt_print(DT_DEBUG_AI,
                 "[darktable_ai] unsupported input type %d for input[%d]",
                 inputs[i].type, i);
        ret = -4;
        goto cleanup;
      }
    }

    if((size_t)element_count > SIZE_MAX / type_size)
    {
      dt_print(DT_DEBUG_AI, "[darktable_ai] tensor size overflow for input[%d]", i);
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
      dt_print(DT_DEBUG_AI,
               "[darktable_ai] CreateTensor input[%d] fail: %s", 
               i, g_ort->GetErrorMessage(status));
      g_ort->ReleaseStatus(status);
      ret = -4;
      goto cleanup;
    }
  }

  // create output tensors
  const char **output_names = (const char **)ctx->output_names;

  for(int i = 0; i < num_outputs; i++)
  {
    // dynamic outputs or float16 mismatch: let ORT allocate during Run()
    if(ctx->dynamic_outputs
       || (outputs[i].type == DT_AI_FLOAT && ctx->output_types[i] == DT_AI_FLOAT16))
    {
      output_tensors[i] = NULL;
      continue;
    }

    const int64_t element_count = _safe_element_count(outputs[i].shape, outputs[i].ndim);
    if(element_count < 0)
    {
      dt_print(DT_DEBUG_AI,
               "[darktable_ai] invalid or overflowing shape for output[%d]", i);
      ret = -4;
      goto cleanup;
    }

    ONNXTensorElementDataType onnx_type;
    size_t type_size;

    if(!_dtype_to_onnx(outputs[i].type, &onnx_type, &type_size))
    {
      dt_print(DT_DEBUG_AI,
               "[darktable_ai] unsupported output type %d for output[%d]",
               outputs[i].type, i);
      ret = -4;
      goto cleanup;
    }

    if((size_t)element_count > SIZE_MAX / type_size)
    {
      dt_print(DT_DEBUG_AI, "[darktable_ai] tensor size overflow for output[%d]", i);
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
      dt_print(DT_DEBUG_AI,
               "[darktable_ai] CreateTensor output[%d] fail: %s",
               i, g_ort->GetErrorMessage(status));
      g_ort->ReleaseStatus(status);
      ret = -4;
      goto cleanup;
    }
  }

  // run
  status = g_ort->Run(ctx->session,
                      NULL,
                      input_names,
                      (const OrtValue *const *)input_tensors,
                      num_inputs,
                      output_names,
                      num_outputs,
                      output_tensors);

  if(status)
  {
    dt_print(DT_DEBUG_AI, "[darktable_ai] run error: %s", g_ort->GetErrorMessage(status));
    g_ort->ReleaseStatus(status);
    ret = -3;
  }
  else
  {
    // post-run: copy data from ORT-allocated outputs to caller's buffers.
    // this handles both dynamic-shape models (where we can't pre-allocate
    // because ORT's shape inference disagrees with the actual output shape)
    // and Float16→Float auto-conversion
    for(int i = 0; i < num_outputs; i++)
    {
      const gboolean ort_allocated = ctx->dynamic_outputs
        || (outputs[i].type == DT_AI_FLOAT && ctx->output_types[i] == DT_AI_FLOAT16);
      if(!ort_allocated || !output_tensors[i]) continue;

      void *raw_data = NULL;
      status = g_ort->GetTensorMutableData(output_tensors[i], &raw_data);
      if(status)
      {
        dt_print(DT_DEBUG_AI,
                 "[darktable_ai] GetTensorMutableData output[%d] failed: %s",
                 i, g_ort->GetErrorMessage(status));
        g_ort->ReleaseStatus(status);
        continue;
      }

      // query ORT's actual tensor size to avoid reading past its allocation.
      // the caller's expected shape may differ from what ORT produced
      // (e.g., dynamic-shape models)
      OrtTensorTypeAndShapeInfo *tensor_info = NULL;
      status = g_ort->GetTensorTypeAndShape(output_tensors[i], &tensor_info);
      if(status)
      {
        dt_print(DT_DEBUG_AI, "[darktable_ai] GetTensorTypeAndShape output[%d] failed: %s",
                 i, g_ort->GetErrorMessage(status));
        g_ort->ReleaseStatus(status);
        continue;
      }
      // update caller's shape array with actual ORT output dimensions.
      // this is essential for dynamic-shape models where the caller's
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
        dt_print(DT_DEBUG_AI, "[darktable_ai] GetTensorShapeElementCount output[%d] failed: %s",
                 i, g_ort->GetErrorMessage(status));
        g_ort->ReleaseStatus(status);
        continue;
      }

      const int64_t caller_count
        = _safe_element_count(outputs[i].shape, outputs[i].ndim);
      if(caller_count < 0)
      {
        dt_print(DT_DEBUG_AI,
                 "[darktable_ai] invalid shape for output[%d] post-copy", i);
        continue;
      }

      // use the smaller of ORT's actual size and caller's expected size
      const int64_t element_count = ((int64_t)ort_element_count < caller_count)
        ? (int64_t)ort_element_count
        : caller_count;

      if(element_count != caller_count)
      {
        dt_print(DT_DEBUG_AI,
                 "[darktable_ai] output[%d] shape mismatch: ORT has %zu elements, "
                 "caller expects %" PRId64,
                 i, ort_element_count, caller_count);
      }

      // allocate caller buffer if NULL (dynamic output, caller
      // couldn't pre-allocate because shapes were unknown)
      if(!outputs[i].data)
      {
        ONNXTensorElementDataType onnx_type;
        size_t type_size;
        if(!_dtype_to_onnx(outputs[i].type, &onnx_type, &type_size))
        {
          dt_print(DT_DEBUG_AI,
                   "[darktable_ai] unknown dtype %d for output[%d]",
                   outputs[i].type, i);
          continue;
        }
        outputs[i].data = g_try_malloc(ort_element_count * type_size);
        if(!outputs[i].data)
        {
          dt_print(DT_DEBUG_AI,
                   "[darktable_ai] failed to allocate output[%d] (%zu elements)",
                   i, ort_element_count);
          continue;
        }
      }

      if(ctx->output_types[i] == DT_AI_FLOAT16 && outputs[i].type == DT_AI_FLOAT)
      {
        // float16 → float conversion
        uint16_t *half_data = (uint16_t *)raw_data;
        float *dst = (float *)outputs[i].data;
        for(int64_t k = 0; k < element_count; k++)
          dst[k] = _half_to_float(half_data[k]);
      }
      else
      {
        // same-type copy from ORT allocation to caller's buffer
        ONNXTensorElementDataType onnx_type;
        size_t type_size;
        if(!_dtype_to_onnx(outputs[i].type, &onnx_type, &type_size))
        {
          dt_print(DT_DEBUG_AI,
                   "[darktable_ai] unknown dtype %d for output[%d] post-copy",
                   outputs[i].type, i);
          continue;
        }
        memcpy(outputs[i].data, raw_data, element_count * type_size);
      }
    }
  }

cleanup:
  // cleanup OrtValues (wrappers only, data is owned by caller)
  for(int i = 0; i < num_inputs; i++)
    if(input_tensors[i])
      g_ort->ReleaseValue(input_tensors[i]);
  for(int i = 0; i < num_outputs; i++)
    if(output_tensors[i])
      g_ort->ReleaseValue(output_tensors[i]);

  // free temp input buffers
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
    // note: OrtEnv is a shared singleton (g_env), not per-context
    if(ctx->memory_info)
      g_ort->ReleaseMemoryInfo(ctx->memory_info);

    // release IO names using the allocator that created them
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
