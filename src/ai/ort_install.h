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

/** GPU vendor types for ORT GPU install */
typedef enum
{
  DT_ORT_GPU_NVIDIA = 0,  // CUDA EP
  DT_ORT_GPU_AMD,         // MIGraphX EP (ROCm)
  DT_ORT_GPU_INTEL,       // OpenVINO EP
} dt_ort_gpu_vendor_t;

/** Display label for a vendor's EP (e.g. "CUDA", "MIGraphX", "OpenVINO").
 *  Returned string is a literal, do not free. */
const char *dt_ort_gpu_vendor_label(dt_ort_gpu_vendor_t vendor);

/** Detection result for a GPU vendor.
 *  Ownership: all string fields are either NULL (absent) or point to a
 *  freshly-allocated non-empty string. Callers can test presence with
 *  a plain NULL check; no need to also test for empty strings. */
typedef struct
{
  dt_ort_gpu_vendor_t vendor;
  char *label;            // e.g. "NVIDIA GeForce RTX 4090"
  char *runtime_version;  // e.g. "CUDA 13.2"; NULL if unknown
  gboolean deps_met;      // all runtime prerequisites satisfied
  char *deps_missing;     // what's missing; NULL if deps_met
  char *deps_hint;        // distro-specific install command; may be NULL
  gsize download_size_mb; // approximate download size
} dt_ort_gpu_info_t;

/** Progress callback for ORT install.
 *  @param progress 0.0 to 1.0
 *  @param status human-readable phase (e.g. "downloading...", "extracting...")
 *  @param user_data opaque pointer */
typedef void (*dt_ort_install_progress_cb)(double progress,
                                           const char *status,
                                           gpointer user_data);

/** Detect available GPU vendors on this system.
 *  @return GList of dt_ort_gpu_info_t* (caller owns, free with dt_ort_gpu_info_free) */
GList *dt_ort_detect_gpus(void);

/** Free a dt_ort_gpu_info_t */
void dt_ort_gpu_info_free(dt_ort_gpu_info_t *info);

/** Download and install ORT for the given GPU vendor. Synchronous -- call from worker thread.
 *  @param vendor which GPU vendor's package to install
 *  @param cb progress callback (may be NULL)
 *  @param user_data data for callback
 *  @param cancel checked periodically; set to TRUE to abort
 *  @param out_lib_path on success, set to the installed libonnxruntime path (caller frees)
 *  @return error message (caller must g_free) or NULL on success */
char *dt_ort_install_gpu(dt_ort_gpu_vendor_t vendor,
                         dt_ort_install_progress_cb cb,
                         gpointer user_data,
                         const gboolean *cancel,
                         char **out_lib_path);
