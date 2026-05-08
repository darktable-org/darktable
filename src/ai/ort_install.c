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

#include "ai/ort_install.h"

#if !defined(__APPLE__)

#include "ai/backend.h"
#include "common/curl_tools.h"
#include "common/darktable.h"
#include "common/file_location.h"

#include <archive.h>
#include <archive_entry.h>
#include <curl/curl.h>
#include <gio/gio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

// shared permissions for any directory we create (install dir, tmp)
#define DT_ORT_DIR_MODE 0755

// forward declarations
#ifndef _WIN32
static gchar *_get_rocm_version(void);
#endif
static gchar *_get_cuda_version(void);
static gchar *_load_install_docs_url(const char *vendor);

const char *dt_ort_gpu_vendor_label(dt_ort_gpu_vendor_t vendor)
{
  switch(vendor)
  {
    case DT_ORT_GPU_NVIDIA: return "CUDA";
    case DT_ORT_GPU_AMD:    return "MIGraphX";
    case DT_ORT_GPU_INTEL:  return "OpenVINO";
  }
  return "?";
}

// vendor -> lowercase manifest key ("nvidia", "amd", "intel")
static const char *_vendor_manifest_key(dt_ort_gpu_vendor_t vendor)
{
  switch(vendor)
  {
    case DT_ORT_GPU_NVIDIA: return "nvidia";
    case DT_ORT_GPU_AMD:    return "amd";
    case DT_ORT_GPU_INTEL:  return "intel";
  }
  return "";
}

// manifest-driven package metadata (loaded from data/ort_gpu.json)
#include <json-glib/json-glib.h>

typedef struct
{
  char *vendor;        // "nvidia", "amd", "intel"
  char *platform;      // "linux", "windows"
  char *ort_version;
  char *url;
  char *format;        // "tgz", "zip", "whl"
  char *lib_pattern;   // "libonnxruntime" or "onnxruntime"
  gchar **lib_extra_patterns;
  // when TRUE, preserve the archive's relative paths under install_dir.
  // required for manylinux wheels whose providers .so has a hard-coded
  // RPATH like `$ORIGIN/../../<ep>.libs` (e.g. AMD MIGraphX). default
  // FALSE = flat extraction: copy matched files to install_dir/ basename
  gboolean preserve_layout;
  char *install_subdir;
  char *sha256;        // expected SHA-256 hash (optional but recommended)
  char *rocm_min;      // optional, AMD only
  char *rocm_max;      // optional, AMD only
  char *cuda_min;      // optional, NVIDIA only
  char *cuda_max;      // optional, NVIDIA only
  gsize size_mb;
  char *requirements;
  gchar **required_libs;
  // optional: separate runtime download (e.g. OpenVINO runtime wheel)
  char *runtime_url;
  char *runtime_sha256;
  char *runtime_lib_pattern;
  gchar **runtime_extra_patterns;
  gsize runtime_size_mb;
} _ort_package_t;

static void _package_free(_ort_package_t *p)
{
  if(!p) return;
  g_free(p->vendor); g_free(p->platform); g_free(p->ort_version);
  g_free(p->url); g_free(p->format); g_free(p->lib_pattern);
  g_strfreev(p->lib_extra_patterns);
  g_free(p->install_subdir); g_free(p->sha256);
  g_free(p->rocm_min); g_free(p->rocm_max);
  g_free(p->cuda_min); g_free(p->cuda_max);
  g_free(p->requirements);
  g_strfreev(p->required_libs);
  g_free(p->runtime_url); g_free(p->runtime_sha256);
  g_free(p->runtime_lib_pattern);
  g_strfreev(p->runtime_extra_patterns);
  g_free(p);
}

// g_strdup of a JSON string member if present, else NULL
static gchar *_json_opt_string(JsonObject *o, const char *key)
{
  return json_object_has_member(o, key)
         ? g_strdup(json_object_get_string_member(o, key))
         : NULL;
}

// Parse a JSON string array into a NULL-terminated gchar**; returns
// NULL if the key is absent or not an array. caller frees with g_strfreev.
static gchar **_json_opt_string_array(JsonObject *o, const char *key)
{
  if(!json_object_has_member(o, key)) return NULL;
  JsonNode *node = json_object_get_member(o, key);
  if(!node || !JSON_NODE_HOLDS_ARRAY(node)) return NULL;
  JsonArray *arr = json_node_get_array(node);
  const guint n = json_array_get_length(arr);
  gchar **result = g_new0(gchar *, n + 1);
  for(guint i = 0; i < n; i++)
    result[i] = g_strdup(json_array_get_string_element(arr, i));
  return result;
}

static _ort_package_t *_parse_package_from_json(JsonObject *p)
{
  _ort_package_t *pkg = g_new0(_ort_package_t, 1);

  pkg->vendor
    = g_strdup(json_object_get_string_member_with_default(p, "vendor", ""));
  pkg->platform
    = g_strdup(json_object_get_string_member_with_default(p, "platform", ""));
  pkg->ort_version
    = g_strdup(json_object_get_string_member_with_default(p, "ort_version", ""));
  pkg->url
    = g_strdup(json_object_get_string_member_with_default(p, "url", ""));
  pkg->format
    = g_strdup(json_object_get_string_member_with_default(p, "format", "tgz"));
  pkg->lib_pattern
    = g_strdup(json_object_get_string_member_with_default(p,
                                                          "lib_pattern",
                                                          "libonnxruntime"));
  pkg->lib_extra_patterns
    = _json_opt_string_array(p, "lib_extra_patterns");
  pkg->preserve_layout
    = json_object_get_boolean_member_with_default(p, "preserve_layout", FALSE);
  pkg->install_subdir
    = g_strdup(json_object_get_string_member_with_default(p, "install_subdir", ""));
  pkg->requirements
    = g_strdup(json_object_get_string_member_with_default(p, "requirements", ""));
  pkg->size_mb
    = (gsize)json_object_get_int_member_with_default(p, "size_mb", 0);

  pkg->sha256                 = _json_opt_string(p, "sha256");
  pkg->rocm_min               = _json_opt_string(p, "rocm_min");
  pkg->rocm_max               = _json_opt_string(p, "rocm_max");
  pkg->cuda_min               = _json_opt_string(p, "cuda_min");
  pkg->cuda_max               = _json_opt_string(p, "cuda_max");
  pkg->required_libs          = _json_opt_string_array(p, "required_libs");
  pkg->runtime_url            = _json_opt_string(p, "runtime_url");
  pkg->runtime_sha256         = _json_opt_string(p, "runtime_sha256");
  pkg->runtime_lib_pattern    = _json_opt_string(p, "runtime_lib_pattern");
  pkg->runtime_extra_patterns = _json_opt_string_array(p, "runtime_extra_patterns");

  if(json_object_has_member(p, "runtime_size_mb"))
    pkg->runtime_size_mb = (gsize)json_object_get_int_member(p, "runtime_size_mb");

  return pkg;
}

static GList *_load_manifest(void)
{
  char datadir[PATH_MAX] = { 0 };
  dt_loc_get_datadir(datadir, sizeof(datadir));
  gchar *path = g_build_filename(datadir, "ort_gpu.json", NULL);

  JsonParser *parser = json_parser_new();
  GError *err = NULL;
  if(!json_parser_load_from_file(parser, path, &err))
  {
    dt_print(DT_DEBUG_AI, "[ort_install] failed to load manifest '%s': %s",
             path, err ? err->message : "unknown");
    g_clear_error(&err);
    g_object_unref(parser);
    g_free(path);
    return NULL;
  }
  g_free(path);

  JsonNode *root = json_parser_get_root(parser);
  JsonObject *obj = json_node_get_object(root);
  JsonArray *packages = json_object_get_array_member(obj, "packages");
  if(!packages)
  {
    g_object_unref(parser);
    return NULL;
  }

  GList *result = NULL;
  const guint n = json_array_get_length(packages);
  for(guint i = 0; i < n; i++)
    result = g_list_append(result,
                           _parse_package_from_json(json_array_get_object_element(packages, i)));

  g_object_unref(parser);
  return result;
}

// extract major.minor from a version string like "7.2.1-120"
static gchar *_version_major_minor(const char *version)
{
  if(!version) return NULL;
  gchar **parts = g_strsplit(version, ".", 3);
  if(!parts[0] || !parts[1]) { g_strfreev(parts); return NULL; }
  gchar *mm = g_strdup_printf("%s.%s", parts[0], parts[1]);
  g_strfreev(parts);
  return mm;
}

// numeric version compare: returns -1, 0, or 1
static int _version_cmp(const char *a, const char *b)
{
  int ax = 0, ay = 0, bx = 0, by = 0;
  if(a) sscanf(a, "%d.%d", &ax, &ay);
  if(b) sscanf(b, "%d.%d", &bx, &by);
  if(ax != bx) return ax < bx ? -1 : 1;
  if(ay != by) return ay < by ? -1 : 1;
  return 0;
}

// Match vendor + platform + ROCm/CUDA. returns first hit in list order,
// so the manifest must be newest-first (refresh-ort-gpu.py keeps that).
static _ort_package_t *_find_package(GList *packages, dt_ort_gpu_vendor_t vendor)
{
  const char *vendor_str = _vendor_manifest_key(vendor);
#ifdef _WIN32
  const char *platform = "windows";
#else
  const char *platform = "linux";
#endif

  gchar *rocm_mm = NULL;
#ifndef _WIN32
  if(vendor == DT_ORT_GPU_AMD)
  {
    gchar *rocm_ver = _get_rocm_version();
    rocm_mm = _version_major_minor(rocm_ver);
    g_free(rocm_ver);
  }
#endif

  // detect CUDA toolkit version (nvcc on Linux, CUDA_PATH on Windows)
  gchar *cuda_mm = (vendor == DT_ORT_GPU_NVIDIA) ? _get_cuda_version() : NULL;

  _ort_package_t *best = NULL;
  for(GList *l = packages; l; l = g_list_next(l))
  {
    _ort_package_t *pkg = l->data;
    if(g_strcmp0(pkg->vendor, vendor_str) != 0) continue;
    if(g_strcmp0(pkg->platform, platform) != 0) continue;

    // AMD: match ROCm version range
    if(rocm_mm && pkg->rocm_min)
    {
      if(_version_cmp(rocm_mm, pkg->rocm_min) < 0) continue;
      if(pkg->rocm_max && _version_cmp(rocm_mm, pkg->rocm_max) > 0) continue;
    }

    // NVIDIA: match CUDA version range
    if(cuda_mm && pkg->cuda_min)
    {
      if(_version_cmp(cuda_mm, pkg->cuda_min) < 0) continue;
      if(pkg->cuda_max && _version_cmp(cuda_mm, pkg->cuda_max) > 0) continue;
    }

    best = pkg;
    break;
  }
  g_free(rocm_mm);
  g_free(cuda_mm);
  return best;
}

// Helpers

#ifndef _WIN32
// Run a command via sh -c and capture stdout; use for commands that
// need shell features like pipes or redirections.
static gboolean _run_cmd(const char *cmd, char **out)
{
  gchar *stdout_buf = NULL;
  gint exit_status = 0;
  const gchar *argv[] = { "/bin/sh", "-c", cmd, NULL };
  gboolean ok = g_spawn_sync(NULL, (gchar **)argv, NULL,
                             G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL,
                             NULL, NULL, &stdout_buf, NULL, &exit_status, NULL);
  if(ok && exit_status == 0 && stdout_buf && stdout_buf[0])
  {
    g_strstrip(stdout_buf);
    if(out) *out = stdout_buf;
    else g_free(stdout_buf);
    return TRUE;
  }
  g_free(stdout_buf);
  if(out) *out = NULL;
  return FALSE;
}
#endif

#ifndef _WIN32
// Run a command directly (no shell) and capture stdout; use this for
// commands that don't need shell features.
static gboolean _run_argv(const gchar * const *argv, char **out)
{
  gchar *stdout_buf = NULL;
  gint exit_status = 0;
  gboolean ok = g_spawn_sync(NULL, (gchar **)argv, NULL,
                             G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL,
                             NULL, NULL, &stdout_buf, NULL, &exit_status, NULL);
  if(ok && exit_status == 0 && stdout_buf && stdout_buf[0])
  {
    g_strstrip(stdout_buf);
    if(out) *out = stdout_buf;
    else g_free(stdout_buf);
    return TRUE;
  }
  g_free(stdout_buf);
  if(out) *out = NULL;
  return FALSE;
}
#endif

#ifdef _WIN32
// Enumerate display adapters via Win32 API (no subprocess overhead)
// and return the first whose DeviceString contains `vendor_substr`;
// returned label is newly allocated (caller frees), NULL if no match.
static gchar *_win32_find_display_adapter(const char *vendor_substr)
{
  DISPLAY_DEVICEA dev = { .cb = sizeof(dev) };
  for(DWORD i = 0; EnumDisplayDevicesA(NULL, i, &dev, 0); i++)
    if(strstr(dev.DeviceString, vendor_substr))
      return g_strdup(dev.DeviceString);
  return NULL;
}

// Recursively search a directory for a file whose name starts with prefix;
// returns TRUE as soon as a match is found.
static gboolean _find_file_recursive(const char *dir, const char *prefix)
{
  GDir *d = g_dir_open(dir, 0, NULL);
  if(!d) return FALSE;

  const gchar *name;
  gboolean found = FALSE;
  while(!found && (name = g_dir_read_name(d)))
  {
    gchar *path = g_build_filename(dir, name, NULL);
    if(g_file_test(path, G_FILE_TEST_IS_DIR))
      found = _find_file_recursive(path, prefix);
    else if(g_str_has_prefix(name, prefix))
      found = TRUE;
    g_free(path);
  }
  g_dir_close(d);
  return found;
}

// Check whether any directory in `roots` contains (recursively) a
// file whose name starts with `prefix`. the array stops at the first
// NULL entry; use empty-string or a non-existent path to mean "skip".
static gboolean _find_file_in_any(const char *prefix, const char *const *roots)
{
  for(int i = 0; roots[i]; i++)
  {
    if(roots[i][0]
       && g_file_test(roots[i], G_FILE_TEST_IS_DIR)
       && _find_file_recursive(roots[i], prefix))
      return TRUE;
  }
  return FALSE;
}
#endif

#ifndef _WIN32
// Check if a library is in /etc/ld.so.cache — fast but only works
// when the distro package registered a SONAME (Debian's libcudnn9
// package does not, so this misses cuDNN there).
static gboolean _ldconfig_has(const char *lib_name)
{
  gchar *cmd = g_strdup_printf("ldconfig -p 2>/dev/null | grep -q '%s'", lib_name);
  gboolean found = _run_cmd(cmd, NULL);
  g_free(cmd);
  return found;
}

// Check if a library is loadable via the dynamic linker; walks
// LD_LIBRARY_PATH, /etc/ld.so.cache and DT_RUNPATH the same way ORT
// does at session init, so a TRUE result guarantees ORT can load it.
static gboolean _dlopen_has(const char *soname)
{
  GModule *m = g_module_open(soname, G_MODULE_BIND_LAZY | G_MODULE_BIND_LOCAL);
  if(!m) return FALSE;
  g_module_close(m);
  return TRUE;
}

// Check whether `soname` is loadable by the dynamic linker; prefers
// the fast ldconfig cache, falls back to dlopen (catches distro
// packages that don't register a SONAME, e.g. Debian's libcudnn9-cuda-12).
static gboolean _lib_available(const char *soname)
{
  return _ldconfig_has(soname) || _dlopen_has(soname);
}

// detect installed ROCm version via a distro-agnostic cascade. mirrors
// the same probe order as install-ort-gpu.sh. returns NULL if no
// source identified a version (caller can still proceed if rocminfo is
// available — version is informational, not gating)
static gchar *_get_rocm_version(void)
{
  // try sources in order; first non-empty wins
  static const char *const cmds[] = {
    // AMD canonical install (radeon.com repo)
    "cat /opt/rocm/.info/version 2>/dev/null",
    // /opt/rocm symlink target (e.g. /opt/rocm -> rocm-7.1.0)
    "readlink -f /opt/rocm 2>/dev/null"
      " | grep -oE 'rocm-[0-9]+\\.[0-9]+(\\.[0-9]+)?'"
      " | sed 's/rocm-//'",
    // side-by-side /opt/rocm-X.Y[.Z] directory
    "ls -d /opt/rocm-[0-9]*.[0-9]* 2>/dev/null"
      " | sed 's|.*/rocm-||' | sort -V | tail -1",
    // pkg-config (works for any distro that ships rocm-core.pc)
    "pkg-config --modversion rocm-core 2>/dev/null",
    // Fedora/RHEL: rocm-core, rocm-runtime, or rocm-hip (distro-packaged)
    "rpm -q --qf '%{VERSION}' rocm-core 2>/dev/null    | grep -v 'not installed'",
    "rpm -q --qf '%{VERSION}' rocm-runtime 2>/dev/null | grep -v 'not installed'",
    "rpm -q --qf '%{VERSION}' rocm-hip 2>/dev/null     | grep -v 'not installed'",
    // Debian / Ubuntu
    "dpkg-query -W -f='${Version}' rocm-core 2>/dev/null",
    "dpkg-query -W -f='${Version}' rocm-runtime 2>/dev/null",
    // Arch
    "pacman -Q rocm-core 2>/dev/null | awk '{print $2}'",
    NULL,
  };
  for(int i = 0; cmds[i]; i++)
  {
    gchar *out = NULL;
    if(_run_cmd(cmds[i], &out) && out)
    {
      g_strstrip(out);
      if(out[0]) return out;
      g_free(out);
    }
  }
  return NULL;
}
#endif // !_WIN32

// vendor's install-docs URL from the manifest. caller frees
static gchar *_load_install_docs_url(const char *vendor)
{
  char datadir[PATH_MAX] = { 0 };
  dt_loc_get_datadir(datadir, sizeof(datadir));
  gchar *path = g_build_filename(datadir, "ort_gpu.json", NULL);

  JsonParser *parser = json_parser_new();
  gchar *result = NULL;
  if(json_parser_load_from_file(parser, path, NULL))
  {
    JsonNode *root = json_parser_get_root(parser);
    if(root && JSON_NODE_HOLDS_OBJECT(root))
    {
      JsonObject *obj = json_node_get_object(root);
      JsonObject *docs = json_object_has_member(obj, "install_docs")
                         ? json_object_get_object_member(obj, "install_docs")
                         : NULL;
      if(docs && json_object_has_member(docs, vendor))
        result = g_strdup(json_object_get_string_member(docs, vendor));
    }
  }
  g_object_unref(parser);
  g_free(path);
  return result;
}

// Join non-empty trimmed lines with ", ". for multi-GPU labels from
// nvidia-smi / lspci output. caller frees with g_free.
static gchar *_join_lines_csv(const gchar *s)
{
  if(!s) return g_strdup("");
  gchar **lines = g_strsplit(s, "\n", -1);
  GPtrArray *kept = g_ptr_array_new();
  for(gchar **p = lines; *p; p++)
  {
    gchar *t = g_strstrip(*p);
    if(*t) g_ptr_array_add(kept, t);
  }
  g_ptr_array_add(kept, NULL);
  gchar *joined = g_strjoinv(", ", (gchar **)kept->pdata);
  g_ptr_array_free(kept, TRUE);
  g_strfreev(lines);
  return joined;
}

// CUDA toolkit version "major.minor" or NULL. Win: CUDA_PATH; Linux: nvcc
static gchar *_get_cuda_version(void)
{
#ifdef _WIN32
  const char *cuda_path = g_getenv("CUDA_PATH");
  if(!cuda_path) return NULL;
  const char *sep = strrchr(cuda_path, '\\');
  if(!sep) sep = strrchr(cuda_path, '/');
  const char *ver = sep ? sep + 1 : cuda_path;
  if(ver[0] == 'v' || ver[0] == 'V') ver++;
  return _version_major_minor(ver);
#else
  gchar *nvcc_out = NULL;
  if(!g_spawn_command_line_sync("nvcc --version", &nvcc_out, NULL, NULL, NULL)
     || !nvcc_out)
    return NULL;
  gchar *v = g_strstr_len(nvcc_out, -1, ", V");
  gchar *result = NULL;
  if(v)
    result = _version_major_minor(v + 3);  // skip ", V"
  g_free(nvcc_out);
  return result;
#endif
}

static dt_ort_gpu_info_t *_detect_nvidia(GList *packages)
{
  gchar *gpu_label = NULL;

#ifdef _WIN32
  gpu_label = _win32_find_display_adapter("NVIDIA");
  if(!gpu_label) return NULL;
#else
  // Linux: use nvidia-smi to get GPU name(s). Multi-GPU systems return
  // one row per GPU; comma-join into a single label.
  const gchar *argv[] = { "nvidia-smi", "--query-gpu=name",
                          "--format=csv,noheader", NULL };
  gchar *raw = NULL;
  if(!_run_argv(argv, &raw) || !raw || !raw[0])
  {
    g_free(raw);
    return NULL;
  }
  gpu_label = _join_lines_csv(raw);
  g_free(raw);
  if(!gpu_label[0])
  {
    g_free(gpu_label);
    return NULL;
  }
#endif

  dt_ort_gpu_info_t *info = g_new0(dt_ort_gpu_info_t, 1);
  info->vendor = DT_ORT_GPU_NVIDIA;
  info->label = gpu_label;

  // Pull the download size from the matched manifest entry so it
  // stays in sync with what actually gets downloaded. fall back to a
  // ballpark value when no package matches (no install path anyway).
  const _ort_package_t *pkg = _find_package(packages, DT_ORT_GPU_NVIDIA);
  info->download_size_mb = pkg ? pkg->size_mb + pkg->runtime_size_mb : 200;

  gchar *cuda_ver = _get_cuda_version();
  info->runtime_version = cuda_ver ? g_strdup_printf("CUDA %s", cuda_ver) : NULL;
  g_free(cuda_ver);

  // check cuDNN
  info->deps_met = TRUE;
  GString *missing = g_string_new(NULL);

#ifdef _WIN32
  // ORT-CUDA needs cuDNN 9.x. layout varies, so search both standard
  // install roots for any cudnn64_* file.
  const char *cuda_path = g_getenv("CUDA_PATH");
  gchar *standalone = g_build_filename(g_getenv("ProgramFiles"), "NVIDIA", "CUDNN", NULL);
  const char *const cudnn_roots[] = { cuda_path ? cuda_path : "", standalone, NULL };
  if(!_find_file_in_any("cudnn64_", cudnn_roots))
  {
    info->deps_met = FALSE;
    g_string_append(missing, "cuDNN 9.x");
    info->deps_hint = _load_install_docs_url("nvidia");
  }
  g_free(standalone);
#else
  // ORT-CUDA needs cuDNN 9.x
  if(!_lib_available("libcudnn.so.9"))
  {
    info->deps_met = FALSE;
    g_string_append(missing, "cuDNN 9.x");
    info->deps_hint = _load_install_docs_url("nvidia");
  }
#endif

  info->deps_missing = g_string_free(missing, FALSE);
  if(!info->deps_missing[0]) { g_free(info->deps_missing); info->deps_missing = NULL; }

  return info;
}

static dt_ort_gpu_info_t *_detect_amd(GList *packages)
{
#ifdef _WIN32
  (void)packages;
  return NULL;  // ROCm is Linux-only
#else
  // gate on rocminfo availability — works for both AMD-repo
  // (/opt/rocm) and distro-packaged installs. version is informational
  gchar *rocminfo_path = g_find_program_in_path("rocminfo");
  if(!rocminfo_path) return NULL;
  g_free(rocminfo_path);

  gchar *rocm_ver = _get_rocm_version();  // may be NULL if no recognised package

  // Rocminfo lists CPU/GPU/NPU agents — filter by Device Type=GPU
  // and emit the Marketing Name that preceded it within the block.
  gchar *gpu_name = NULL;
  gchar *rocm_output = NULL;
  if(_run_cmd("rocminfo 2>/dev/null | awk '"
              "/Marketing Name:/ { sub(/.*Marketing Name:[[:space:]]*/, \"\"); sub(/[[:space:]]+$/, \"\"); mn=$0 } "
              "/Device Type:.*GPU/{ if(out) out = out \", \" mn; else out = mn } "
              "END                { print out }"
              "'", &rocm_output))
  {
    gchar *stripped = g_strstrip(rocm_output);
    if(stripped && stripped[0]) gpu_name = g_strdup(stripped);
    g_free(rocm_output);
  }

  dt_ort_gpu_info_t *info = g_new0(dt_ort_gpu_info_t, 1);
  info->vendor = DT_ORT_GPU_AMD;
  info->label = gpu_name ? gpu_name : g_strdup("AMD GPU");
  info->runtime_version = rocm_ver
                          ? g_strdup_printf("ROCm %s", rocm_ver)
                          : g_strdup("ROCm");

  // Pull the download size from the matched manifest entry so the UI
  // reflects the real wheel size (varies across ROCm 6.x / 7.x).
  const _ort_package_t *pkg = _find_package(packages, DT_ORT_GPU_AMD);
  info->download_size_mb = pkg ? pkg->size_mb + pkg->runtime_size_mb : 300;

  // check SONAMEs from manifest's "required_libs". manylinux-repaired
  // wheels bundle their deps and skip the check; plain linux_x86_64
  // wheels rely on it to catch SOVER skew vs distro-rebuilt ROCm
  info->deps_met = TRUE;
  GString *missing = g_string_new(NULL);
  if(pkg && pkg->required_libs)
  {
    for(int i = 0; pkg->required_libs[i]; i++)
    {
      const char *soname = pkg->required_libs[i];
      gboolean have = _lib_available(soname);

      if(!have)
      {
        // last-ditch: scan /opt/rocm*/lib{,64}/ for any file whose
        // basename starts with the SONAME's base (e.g. "libmigraphx_c.so").
        // AMD's RHEL packages don't create /opt/rocm or register ldconfig,
        // so files may be on disk but invisible to dlopen
        const char *so = strstr(soname, ".so");
        gchar *base = so ? g_strndup(soname, so - soname) : g_strdup(soname);
        gchar *prefix = g_strdup_printf("%s.so", base);
        GDir *top = g_dir_open("/opt", 0, NULL);
        if(top)
        {
          const gchar *entry;
          while(!have && (entry = g_dir_read_name(top)))
          {
            if(!g_str_has_prefix(entry, "rocm")) continue;
            static const char *const subdirs[] = { "lib", "lib64", NULL };
            for(int s = 0; !have && subdirs[s]; s++)
            {
              gchar *dir = g_build_filename("/opt", entry, subdirs[s], NULL);
              GDir *d = g_dir_open(dir, 0, NULL);
              if(d)
              {
                const gchar *fn;
                while((fn = g_dir_read_name(d)))
                  if(g_str_has_prefix(fn, prefix)) { have = TRUE; break; }
                g_dir_close(d);
              }
              g_free(dir);
            }
          }
          g_dir_close(top);
        }
        g_free(prefix);
        g_free(base);
      }

      if(!have)
      {
        if(missing->len) g_string_append(missing, ", ");
        g_string_append(missing, soname);
      }
    }
  }

  if(missing->len)
  {
    info->deps_met = FALSE;
    info->deps_missing = g_string_free(missing, FALSE);
    info->deps_hint = _load_install_docs_url("amd");
  }
  else
  {
    g_string_free(missing, TRUE);
  }

  g_free(rocm_ver);
  return info;
#endif
}

static dt_ort_gpu_info_t *_detect_intel(GList *packages)
{
#ifdef _WIN32
  gchar *gpu_name = _win32_find_display_adapter("Intel");
  if(!gpu_name) return NULL;

  dt_ort_gpu_info_t *info = g_new0(dt_ort_gpu_info_t, 1);
  info->vendor = DT_ORT_GPU_INTEL;
  info->label = gpu_name;
  info->runtime_version = NULL;  // OpenVINO runtime bundled with the ORT package
  info->deps_met = TRUE;
  // ORT package + separate OpenVINO runtime on Windows
  const _ort_package_t *pkg = _find_package(packages, DT_ORT_GPU_INTEL);
  info->download_size_mb = pkg ? pkg->size_mb + pkg->runtime_size_mb : 100;
  return info;
#elif defined(__APPLE__)
  (void)packages;
  return NULL;
#else
  // Check for Intel GPU via lspci (render nodes exist for all GPUs, not Intel-specific).
  // multi-GPU systems (iGPU + dGPU) produce one row per device; comma-join.
  gchar *gpu_name = NULL;
  gchar *raw = NULL;
  if(_run_cmd("lspci 2>/dev/null | grep -i 'VGA.*Intel\\|Display.*Intel' | sed 's/.*: //'", &raw))
  {
    gpu_name = _join_lines_csv(raw);
    g_free(raw);
    if(!gpu_name[0]) { g_free(gpu_name); gpu_name = NULL; }
  }
  if(!gpu_name)
  {
    // also check for Level Zero (Intel GPU compute runtime)
    if(!_lib_available("libze_loader"))
      return NULL;
  }

  dt_ort_gpu_info_t *info = g_new0(dt_ort_gpu_info_t, 1);
  info->vendor = DT_ORT_GPU_INTEL;
  info->label = gpu_name ? gpu_name : g_strdup("Intel GPU");
  info->runtime_version = NULL;
  info->deps_met = TRUE;  // OpenVINO wheel bundles everything
  // Linux: size comes from the manifest; wheel bundles everything
  const _ort_package_t *pkg = _find_package(packages, DT_ORT_GPU_INTEL);
  info->download_size_mb = pkg ? pkg->size_mb + pkg->runtime_size_mb : 150;

  return info;
#endif
}

GList *dt_ort_detect_gpus(void)
{
  // shared manifest gives detectors per-package sizes; NULL = ballpark fallback
  GList *packages = _load_manifest();

  GList *result = NULL;

  dt_ort_gpu_info_t *nvidia = _detect_nvidia(packages);
  if(nvidia) result = g_list_append(result, nvidia);

  dt_ort_gpu_info_t *amd = _detect_amd(packages);
  if(amd) result = g_list_append(result, amd);

  dt_ort_gpu_info_t *intel = _detect_intel(packages);
  if(intel) result = g_list_append(result, intel);

  g_list_free_full(packages, (GDestroyNotify)_package_free);
  return result;
}

void dt_ort_gpu_info_free(dt_ort_gpu_info_t *info)
{
  if(!info) return;
  g_free(info->label);
  g_free(info->runtime_version);
  g_free(info->deps_missing);
  g_free(info->deps_hint);
  g_free(info);
}

// download helpers

#ifdef HAVE_AI_DOWNLOAD

// Install phases — the table below assigns each phase a progress
// range and a user-visible message, so we only have to tune one place
// if the weighting changes.
typedef enum
{
  DT_ORT_PHASE_DOWNLOAD = 0,
  DT_ORT_PHASE_VERIFY,
  DT_ORT_PHASE_EXTRACT,
  DT_ORT_PHASE_RUNTIME,
  DT_ORT_PHASE_BUNDLE_DEPS,
  DT_ORT_PHASE_VALIDATE,
  DT_ORT_PHASE_DONE,
  DT_ORT_PHASE_COUNT
} _install_phase_t;

static const struct
{
  double start;     // progress value at phase start
  double end;       // progress value when phase finishes (== start for instant phases)
  const char *msg;  // untranslated message (wrap in _() at report time)
} _phase_table[DT_ORT_PHASE_COUNT] = {
  [DT_ORT_PHASE_DOWNLOAD]    = { 0.00, 0.80, N_("downloading...") },
  [DT_ORT_PHASE_VERIFY]      = { 0.82, 0.82, N_("checking download...") },
  [DT_ORT_PHASE_EXTRACT]     = { 0.85, 0.85, N_("extracting...") },
  [DT_ORT_PHASE_RUNTIME]     = { 0.87, 0.92, N_("downloading runtime...") },
  [DT_ORT_PHASE_BUNDLE_DEPS] = { 0.93, 0.93, N_("preparing CUDA support files...") },
  [DT_ORT_PHASE_VALIDATE]    = { 0.95, 0.95, N_("finishing up...") },
  [DT_ORT_PHASE_DONE]        = { 1.00, 1.00, N_("done") },
};

typedef struct
{
  dt_ort_install_progress_cb cb;
  gpointer user_data;
  const gboolean *cancel;
  _install_phase_t phase;  // set by _install_report_phase for curl callback lookup
} _install_progress_t;

static void _install_report_phase(_install_progress_t *p, _install_phase_t phase)
{
  if(!p) return;
  p->phase = phase;
  if(p->cb) p->cb(_phase_table[phase].start, _(_phase_table[phase].msg), p->user_data);
}

static size_t _curl_write_file(void *ptr, size_t size, size_t nmemb, void *stream)
{
  return fwrite(ptr, size, nmemb, (FILE *)stream);
}

// map curl byte progress onto the current phase's [start, end] range
static int _curl_progress(void *clientp, curl_off_t dltotal, curl_off_t dlnow,
                           curl_off_t ultotal, curl_off_t ulnow)
{
  (void)ultotal; (void)ulnow;
  _install_progress_t *p = clientp;
  if(p->cancel && *p->cancel) return 1;  // abort
  if(p->cb && dltotal > 0)
  {
    const double start = _phase_table[p->phase].start;
    const double span = _phase_table[p->phase].end - start;
    const double frac = (double)dlnow / (double)dltotal;
    p->cb(start + frac * span, _(_phase_table[p->phase].msg), p->user_data);
  }
  return 0;
}

// Download a URL to a local file. Returns error string or NULL on success.
static char *_download_file(const char *url, const char *dest,
                            _install_progress_t *progress)
{
  FILE *f = g_fopen(dest, "wb");
  if(!f) return g_strdup_printf(_("could not write to %s"), dest);

  CURL *curl = curl_easy_init();
  if(!curl) { fclose(f); return g_strdup(_("could not start the download")); }  // fclose is OK after g_fopen
  dt_curl_init(curl, FALSE);

  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, _curl_write_file);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, f);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  if(progress)
  {
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, _curl_progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, progress);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
  }

  CURLcode res = curl_easy_perform(curl);
  fclose(f);

  char *error = NULL;
  if(res != CURLE_OK)
  {
    error = (res == CURLE_ABORTED_BY_CALLBACK)
      ? g_strdup(_("download cancelled"))
      : g_strdup_printf(_("download failed: %s"), curl_easy_strerror(res));
    g_unlink(dest);
  }
  else
  {
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    if(http_code != 200)
    {
      error = g_strdup_printf(_("download failed (server returned %ld)"), http_code);
      g_unlink(dest);
    }
  }

  curl_easy_cleanup(curl);
  return error;
}

// archive extraction

// Extract shared libraries from an archive (tgz, zip, or whl) whose
// basename starts with lib_pattern or any of the extra_patterns
// (NULL-terminated array, may be NULL). When preserve_layout is TRUE
// the archive's relative paths are kept under destdir; otherwise files
// are flattened to destdir/<basename>
static gboolean _extract_archive(const char *archive_path, const char *destdir,
                                  const char *lib_pattern,
                                  const char * const *extra_patterns,
                                  const gboolean preserve_layout)
{
  struct archive *a = archive_read_new();
  struct archive_entry *entry;

  archive_read_support_format_tar(a);
  archive_read_support_format_zip(a);
  archive_read_support_filter_gzip(a);
  archive_read_support_filter_none(a);

  if(archive_read_open_filename(a, archive_path, 10240) != ARCHIVE_OK)
  {
    dt_print(DT_DEBUG_AI, "[ort_install] failed to open archive: %s", archive_error_string(a));
    archive_read_free(a);
    return FALSE;
  }

  g_mkdir_with_parents(destdir, DT_ORT_DIR_MODE);
  int extracted = 0;

  while(archive_read_next_header(a, &entry) == ARCHIVE_OK)
  {
    const char *name = archive_entry_pathname(entry);
    if(!name) continue;

    // reject path traversal
    if(strstr(name, "..")) continue;

    // only extract shared libraries matching the filter
    const char *basename = strrchr(name, '/');
    basename = basename ? basename + 1 : name;

    // check primary pattern
    gboolean match = g_str_has_prefix(basename, lib_pattern);
    // check extra patterns
    if(!match && extra_patterns)
    {
      for(int i = 0; extra_patterns[i]; i++)
      {
        if(g_str_has_prefix(basename, extra_patterns[i]))
        {
          match = TRUE;
          break;
        }
      }
    }
    if(!match)
    {
      archive_read_data_skip(a);
      continue;
    }

    // skip directories
    if(archive_entry_filetype(entry) == AE_IFDIR)
    {
      archive_read_data_skip(a);
      continue;
    }

    // preserve_layout: keep the archive's relative path. manylinux
    // wheels with auditwheel-bundled deps need it (providers .so RPATH
    // is `$ORIGIN/../../<ep>.libs`). flat: copy to destdir/<basename>
    gchar *out_path;
    if(preserve_layout)
    {
      out_path = g_build_filename(destdir, name, NULL);
      gchar *out_dir = g_path_get_dirname(out_path);
      g_mkdir_with_parents(out_dir, DT_ORT_DIR_MODE);
      g_free(out_dir);
    }
    else
    {
      out_path = g_build_filename(destdir, basename, NULL);
    }
    // archive_entry_set_pathname invalidates the earlier name/basename
    // pointers (libarchive owns those). snapshot before mutating
    gchar *log_name = g_strdup(preserve_layout ? name : basename);
    archive_entry_set_pathname(entry, out_path);

    struct archive *ext = archive_write_disk_new();
    archive_write_disk_set_options(ext, ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM);

    if(archive_write_header(ext, entry) == ARCHIVE_OK)
    {
      const void *buf;
      size_t size;
      la_int64_t offset;
      while(archive_read_data_block(a, &buf, &size, &offset) == ARCHIVE_OK)
        archive_write_data_block(ext, buf, size, offset);
      archive_write_finish_entry(ext);
      extracted++;
      dt_print(DT_DEBUG_AI, "[ort_install] extracted: %s", log_name);
    }
    g_free(log_name);

    archive_write_free(ext);
    g_free(out_path);
  }

  archive_read_close(a);
  archive_read_free(a);

  dt_print(DT_DEBUG_AI, "[ort_install] extracted %d files to %s", extracted, destdir);
  return extracted > 0;
}

// helpers for the main install function

static gchar *_compute_install_dir(const char *subdir)
{
#ifdef _WIN32
  const char *root = g_getenv("LOCALAPPDATA");
  if(!root) root = g_get_home_dir();
  return g_build_filename(root, subdir, NULL);
#else
  return g_build_filename(g_get_home_dir(), ".local/lib", subdir, NULL);
#endif
}

// Returns TRUE if the file's SHA-256 matches expected_hex (case-insensitive),
// or if expected_hex is NULL/empty (caller opted out of verification).
static gboolean _verify_sha256(const char *path, const char *expected_hex)
{
  if(!expected_hex || !expected_hex[0]) return TRUE;

  gchar *contents = NULL;
  gsize length = 0;
  if(!g_file_get_contents(path, &contents, &length, NULL))
    return FALSE;

  GChecksum *cs = g_checksum_new(G_CHECKSUM_SHA256);
  g_checksum_update(cs, (guchar *)contents, length);
  const gchar *computed = g_checksum_get_string(cs);
  const gboolean match = g_ascii_strcasecmp(computed, expected_hex) == 0;
  if(!match)
    dt_print(DT_DEBUG_AI, "[ort_install] checksum mismatch: expected %s, got %s",
             expected_hex, computed);
  else
    dt_print(DT_DEBUG_AI, "[ort_install] checksum verified: %s", computed);

  g_checksum_free(cs);
  g_free(contents);
  return match;
}

// Find the validated onnxruntime library under install_dir. recurse=TRUE
// for layout-preserving installs (lib lives in a subdir like
// onnxruntime/capi/); FALSE for flat installs (entry-point sits directly
// in install_dir). returned path is newly allocated (caller frees).
static gchar *_find_valid_library(const char *install_dir,
                                  const char *lib_pattern,
                                  const gboolean recurse)
{
  GDir *dir = g_dir_open(install_dir, 0, NULL);
  if(!dir) return NULL;

  gchar *result = NULL;
  const gchar *name;
  while((name = g_dir_read_name(dir)) && !result)
  {
    gchar *path = g_build_filename(install_dir, name, NULL);
    if(g_file_test(path, G_FILE_TEST_IS_DIR))
    {
      // skip the auditwheel-bundled lib dir; the real entry-point
      // lives in onnxruntime/capi/, *.libs/ holds peer deps
      if(recurse && !g_str_has_suffix(name, ".libs"))
        result = _find_valid_library(path, lib_pattern, recurse);
    }
    else if(g_str_has_prefix(name, lib_pattern)
            && (g_str_has_suffix(name, ".so") || strstr(name, ".so.")
                || g_str_has_suffix(name, ".dll")))
    {
      char *version = dt_ai_ort_probe_library(path);
      if(version)
      {
        g_free(version);
        result = g_strdup(path);
      }
    }
    g_free(path);
  }
  g_dir_close(dir);
  return result;
}

#ifdef _WIN32
// Extract CUDA major version from CUDA_PATH (e.g. ".../CUDA/v13.2" -> "13");
// returned string is newly allocated (caller frees) or NULL if not found.
static gchar *_get_cuda_major(void)
{
  // reuse the version parser, then truncate "13.2" -> "13"
  gchar *mm = _get_cuda_version();
  if(!mm) return NULL;
  const char *dot = strchr(mm, '.');
  if(!dot) return mm;
  gchar *major = g_strndup(mm, dot - mm);
  g_free(mm);
  return major;
}

// copy a single file with overwrite
static void _copy_dll(const char *from_dir, const char *name, const char *to_dir)
{
  gchar *from = g_build_filename(from_dir, name, NULL);
  gchar *to   = g_build_filename(to_dir,   name, NULL);
  GFile *gf_from = g_file_new_for_path(from);
  GFile *gf_to   = g_file_new_for_path(to);
  dt_print(DT_DEBUG_AI, "[ort_install] bundling %s", name);
  g_file_copy(gf_from, gf_to, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, NULL);
  g_object_unref(gf_from);
  g_object_unref(gf_to);
  g_free(from);
  g_free(to);
}

// Copy every cudnn*.dll from src_dir into install_dir;
// returns TRUE if any file was copied.
static gboolean _bundle_cudnn_from_dir(const char *src_dir, const char *install_dir)
{
  GDir *sd = g_dir_open(src_dir, 0, NULL);
  if(!sd) return FALSE;

  gboolean any = FALSE;
  const gchar *fn;
  while((fn = g_dir_read_name(sd)))
  {
    if(g_str_has_prefix(fn, "cudnn") && g_str_has_suffix(fn, ".dll"))
    {
      _copy_dll(src_dir, fn, install_dir);
      any = TRUE;
    }
  }
  g_dir_close(sd);
  return any;
}

// Walk %ProgramFiles%/NVIDIA/CUDNN/<ver>/bin/<cuda_major>*[/x64]/ and copy
// all cudnn*.dll files into install_dir for the first matching location.
static void _bundle_cudnn(const char *install_dir, const char *cuda_major)
{
  const char *pf = g_getenv("ProgramFiles");
  if(!pf || !cuda_major) return;

  gchar *cudnn_root = g_build_filename(pf, "NVIDIA", "CUDNN", NULL);
  if(g_file_test(cudnn_root, G_FILE_TEST_IS_DIR))
  {
    GDir *vd = g_dir_open(cudnn_root, 0, NULL);
    if(vd)
    {
      const gchar *vname;
      gboolean done = FALSE;
      while(!done && (vname = g_dir_read_name(vd)))
      {
        gchar *bin_dir = g_build_filename(cudnn_root, vname, "bin", NULL);
        if(g_file_test(bin_dir, G_FILE_TEST_IS_DIR))
        {
          GDir *bd = g_dir_open(bin_dir, 0, NULL);
          if(bd)
          {
            const gchar *cname;
            while(!done && (cname = g_dir_read_name(bd)))
            {
              if(!g_str_has_prefix(cname, cuda_major)) continue;

              gchar *src = g_build_filename(bin_dir, cname, "x64", NULL);
              if(!g_file_test(src, G_FILE_TEST_IS_DIR))
              {
                g_free(src);
                src = g_build_filename(bin_dir, cname, NULL);
              }
              done = _bundle_cudnn_from_dir(src, install_dir);
              g_free(src);
            }
            g_dir_close(bd);
          }
        }
        g_free(bin_dir);
      }
      g_dir_close(vd);
    }
  }
  g_free(cudnn_root);
}

// Copy the CUDA toolkit DLLs we redistribute (cublas, cudart, cufft, curand);
// searches CUDA_PATH/bin/x64 then CUDA_PATH/bin.
static void _bundle_cuda_toolkit(const char *install_dir)
{
  const char *cuda_path = g_getenv("CUDA_PATH");
  if(!cuda_path) return;

  const char *subdirs[] = { "bin" G_DIR_SEPARATOR_S "x64", "bin", NULL };
  const char *prefixes[] = { "cublas", "cublasLt", "cudart", "cufft", "curand", NULL };

  for(int s = 0; subdirs[s]; s++)
  {
    gchar *dir = g_build_filename(cuda_path, subdirs[s], NULL);
    if(g_file_test(dir, G_FILE_TEST_IS_DIR))
    {
      GDir *d = g_dir_open(dir, 0, NULL);
      if(d)
      {
        const gchar *fn;
        while((fn = g_dir_read_name(d)))
        {
          if(!g_str_has_suffix(fn, ".dll")) continue;
          for(int p = 0; prefixes[p]; p++)
          {
            if(!g_str_has_prefix(fn, prefixes[p])) continue;
            gchar *dest = g_build_filename(install_dir, fn, NULL);
            if(!g_file_test(dest, G_FILE_TEST_EXISTS))
              _copy_dll(dir, fn, install_dir);
            g_free(dest);
            break;
          }
        }
        g_dir_close(d);
      }
    }
    g_free(dir);
  }
}

// Download + extract a separate runtime package (currently OpenVINO)
// when the manifest specifies one; errors here are non-fatal — the
// main install has already succeeded.
static void _bundle_runtime_if_needed(const _ort_package_t *pkg,
                                      const char *install_dir,
                                      _install_progress_t *progress)
{
  if(!pkg->runtime_url || !pkg->runtime_url[0]) return;

  _install_report_phase(progress, DT_ORT_PHASE_RUNTIME);
  dt_print(DT_DEBUG_AI, "[ort_install] downloading runtime: %s", pkg->runtime_url);

  gchar *tmpdir = g_dir_make_tmp("dt-ort-rt-XXXXXX", NULL);
  if(!tmpdir) return;

  gchar *archive = g_build_filename(tmpdir, "runtime-package", NULL);
  char *err = _download_file(pkg->runtime_url, archive, progress);
  if(!err)
  {
    if(_verify_sha256(archive, pkg->runtime_sha256))
    {
      const char *rt_pattern = pkg->runtime_lib_pattern
                               ? pkg->runtime_lib_pattern : "openvino";
      // extra prefixes (e.g. "tbb" for OpenVINO) come from the manifest
      _extract_archive(archive, install_dir, rt_pattern,
                       (const char * const *)pkg->runtime_extra_patterns,
                       FALSE /* runtime addons stay flat */);
    }
  }
  else
  {
    dt_print(DT_DEBUG_AI, "[ort_install] runtime download failed: %s", err);
    g_free(err);
  }

  g_unlink(archive);
  g_rmdir(tmpdir);
  g_free(archive);
  g_free(tmpdir);
}

// Copy cuDNN + CUDA toolkit DLLs next to the installed onnxruntime
// DLL; only applies to NVIDIA packages.
static void _bundle_windows_cuda_deps(const _ort_package_t *pkg,
                                      const char *install_dir,
                                      _install_progress_t *progress)
{
  if(g_strcmp0(pkg->vendor, "nvidia") != 0) return;

  _install_report_phase(progress, DT_ORT_PHASE_BUNDLE_DEPS);

  gchar *cuda_major = _get_cuda_major();
  _bundle_cudnn(install_dir, cuda_major);
  _bundle_cuda_toolkit(install_dir);
  g_free(cuda_major);
}
#endif // _WIN32

#if defined(__linux__)
// clear PT_GNU_STACK PF_X on a .so so glibc >= 2.41 will dlopen it.
// ELF64 little-endian only; other architectures silently no-op
static gboolean _clear_execstack_on_file(const char *path)
{
  FILE *f = g_fopen(path, "r+b");
  if(!f) return FALSE;

  uint8_t ehdr[64];
  if(fread(ehdr, 1, sizeof(ehdr), f) != sizeof(ehdr)) { fclose(f); return FALSE; }
  // ELF magic + ELF64 + little-endian only
  if(ehdr[0] != 0x7f || ehdr[1] != 'E' || ehdr[2] != 'L' || ehdr[3] != 'F'
     || ehdr[4] != 2 || ehdr[5] != 1) { fclose(f); return FALSE; }

  uint64_t e_phoff   = *(uint64_t *)(ehdr + 32);
  uint16_t e_phents  = *(uint16_t *)(ehdr + 54);
  uint16_t e_phnum   = *(uint16_t *)(ehdr + 56);
  if(e_phents < 8 || e_phnum == 0) { fclose(f); return FALSE; }

  gboolean changed = FALSE;
  for(uint16_t i = 0; i < e_phnum; i++)
  {
    const long off = (long)(e_phoff + (uint64_t)i * e_phents);
    if(fseek(f, off, SEEK_SET) != 0) break;
    uint32_t hdr[2];
    if(fread(hdr, sizeof(hdr[0]), 2, f) != 2) break;
    // PT_GNU_STACK = 0x6474e551, PF_X = 0x1
    if(hdr[0] == 0x6474e551u && (hdr[1] & 0x1u))
    {
      hdr[1] &= ~0x1u;
      if(fseek(f, off + 4, SEEK_SET) != 0) break;
      if(fwrite(&hdr[1], sizeof(uint32_t), 1, f) != 1) break;
      changed = TRUE;
      break;
    }
  }
  fclose(f);
  return changed;
}

static void _clear_execstack_on_tree(const char *dir)
{
  GDir *d = g_dir_open(dir, 0, NULL);
  if(!d) return;
  const gchar *name;
  while((name = g_dir_read_name(d)))
  {
    gchar *path = g_build_filename(dir, name, NULL);
    if(g_file_test(path, G_FILE_TEST_IS_DIR))
    {
      _clear_execstack_on_tree(path);
    }
    else if(g_strstr_len(name, -1, ".so"))
    {
      if(_clear_execstack_on_file(path))
        dt_print(DT_DEBUG_AI,
                 "[ort_install] cleared executable-stack flag on %s", name);
    }
    g_free(path);
  }
  g_dir_close(d);
}
#endif // __linux__

// main install function — manifest-driven

char *dt_ort_install_gpu(dt_ort_gpu_vendor_t vendor,
                         dt_ort_install_progress_cb cb,
                         gpointer user_data,
                         const gboolean *cancel,
                         char **out_lib_path)
{
  if(out_lib_path) *out_lib_path = NULL;

  GList *packages = NULL;
  gchar *install_dir = NULL;
  gchar *tmpdir = NULL;
  gchar *archive_path = NULL;
  gchar *lib_path = NULL;
  char *error = NULL;

  packages = _load_manifest();
  if(!packages)
  {
    error = g_strdup(_("GPU package list is missing"));
    goto cleanup;
  }

  _ort_package_t *pkg = _find_package(packages, vendor);
  if(!pkg)
  {
    error = g_strdup(_("no ONNX Runtime package available for your GPU"));
    goto cleanup;
  }

  dt_print(DT_DEBUG_AI, "[ort_install] package: ORT %s for %s/%s",
           pkg->ort_version, pkg->vendor, pkg->platform);
  dt_print(DT_DEBUG_AI, "[ort_install] downloading %s", pkg->url);

  install_dir = _compute_install_dir(pkg->install_subdir);

  tmpdir = g_dir_make_tmp("dt-ort-XXXXXX", NULL);
  if(!tmpdir)
  {
    error = g_strdup(_("could not prepare a temporary folder for the download"));
    goto cleanup;
  }
  archive_path = g_build_filename(tmpdir, "ort-package", NULL);

  _install_progress_t progress = { .cb = cb, .user_data = user_data, .cancel = cancel,
                                   .phase = DT_ORT_PHASE_DOWNLOAD };
  _install_report_phase(&progress, DT_ORT_PHASE_DOWNLOAD);
  error = _download_file(pkg->url, archive_path, &progress);
  if(error) goto cleanup;

  if(pkg->sha256 && pkg->sha256[0])
  {
    _install_report_phase(&progress, DT_ORT_PHASE_VERIFY);
    if(!_verify_sha256(archive_path, pkg->sha256))
    {
      error = g_strdup(_("downloaded file is corrupted, please try again"));
      goto cleanup;
    }
  }

  _install_report_phase(&progress, DT_ORT_PHASE_EXTRACT);
  if(!_extract_archive(archive_path, install_dir, pkg->lib_pattern,
                       (const char *const *)pkg->lib_extra_patterns,
                       pkg->preserve_layout))
  {
    error = g_strdup(_("failed to extract ONNX Runtime package"));
    goto cleanup;
  }

  // primary archive done; drop it before optional bundling work
  g_unlink(archive_path);
  g_rmdir(tmpdir);
  g_free(archive_path); archive_path = NULL;
  g_free(tmpdir);       tmpdir = NULL;

#ifdef _WIN32
  _bundle_runtime_if_needed(pkg, install_dir, &progress);
  _bundle_windows_cuda_deps(pkg, install_dir, &progress);
#endif

#if defined(__linux__)
  // glibc >= 2.41 won't dlopen libraries with PT_GNU_STACK PF_X set
  // (AMD/ROCm wheels are the known offenders)
  _clear_execstack_on_tree(install_dir);
#endif

  _install_report_phase(&progress, DT_ORT_PHASE_VALIDATE);
  lib_path = _find_valid_library(install_dir, pkg->lib_pattern,
                                 pkg->preserve_layout);
  if(!lib_path)
  {
    error = g_strdup(_("installed library could not be loaded"));
    goto cleanup;
  }

  _install_report_phase(&progress, DT_ORT_PHASE_DONE);
  dt_print(DT_DEBUG_AI, "[ort_install] installed: %s", lib_path);
  if(out_lib_path) { *out_lib_path = lib_path; lib_path = NULL; }

cleanup:
  if(archive_path) g_unlink(archive_path);
  if(tmpdir) g_rmdir(tmpdir);
  g_free(archive_path);
  g_free(tmpdir);
  g_free(install_dir);
  g_free(lib_path);
  g_list_free_full(packages, (GDestroyNotify)_package_free);
  return error;
}

#else // !HAVE_AI_DOWNLOAD

char *dt_ort_install_gpu(dt_ort_gpu_vendor_t vendor,
                         dt_ort_install_progress_cb cb,
                         gpointer user_data,
                         const gboolean *cancel,
                         char **out_lib_path)
{
  (void)vendor; (void)cb; (void)user_data; (void)cancel;
  if(out_lib_path) *out_lib_path = NULL;
  return g_strdup(_("download is not available in this build of darktable"));
}

#endif // HAVE_AI_DOWNLOAD

#endif // !__APPLE__
