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

#include "backend.h"

/**
 * @brief Client side of the remote ONNX inference wire protocol (v1).
 *
 * DT_AI_REMOTE=127.0.0.1:8794 routes every load to the loopback worker —
 * including explicit CPU requests, whose preference is forwarded as
 * cpu:true (the worker then runs the CPU execution provider). One
 * persistent TCP connection per loaded model, serialized by the model
 * mutex; a 300s timeout or a malformed frame kills the connection and
 * the model fails. A configured remote that is unreachable or refuses
 * the load returns NULL — the caller never falls back to local inference.
 *
 * Wire limits shared with the worker (protocol v1).
 */
#define DT_AI_REMOTE_MAX_IO       16
#define DT_AI_REMOTE_MAX_OVERRIDES 16
#define DT_AI_REMOTE_MAX_RANK     8
#define DT_AI_REMOTE_MAX_JSON     (64 * 1024)
#define DT_AI_REMOTE_MAX_BYTES    ((guint64)512 << 20)
#define DT_AI_REMOTE_TIMEOUT_S    300

/** One tensor descriptor as exchanged in load metadata and run frames. */
typedef struct dt_ai_remote_io_t
{
  char *name;      // set from load metadata, NULL in run frames
  dt_ai_dtype_t type;
  int64_t shape[DT_AI_REMOTE_MAX_RANK]; // -1 dims only where allowed
  int ndim;
  int64_t bytes;   // validated payload size, 0 when absent
} dt_ai_remote_io_t;

typedef struct dt_ai_remote_model_t
{
  int fd;          // persistent connection, -1 once dead
  GMutex lock;     // serializes one request/response per connection
  int n_inputs;
  int n_outputs;
  dt_ai_remote_io_t inputs[DT_AI_REMOTE_MAX_IO];
  dt_ai_remote_io_t outputs[DT_AI_REMOTE_MAX_IO];
} dt_ai_remote_model_t;

/** TRUE if DT_AI_REMOTE is set to a non-empty value. The endpoint
 *  itself is parsed and loopback-checked when connecting. */
gboolean dt_ai_remote_enabled(void);

/** Connect to the configured worker and load a model. `cpu` forwards the
 *  caller's CPU preference to the worker (CPU execution provider there).
 *  On any failure (parse, connect, protocol, worker refusal) returns NULL
 *  and closes the socket; no local retry happens anywhere. */
dt_ai_remote_model_t *dt_ai_remote_connect_load(
    const char *model_id, const char *model_file,
    const gboolean cpu,
    const dt_ai_opt_level_t opt_level,
    const dt_ai_dim_override_t *dim_overrides, const int n_overrides);

/** One inference round trip. Same contract as dt_ai_run(): outputs[i].data
 *  == NULL receives a freshly allocated buffer the caller frees with
 *  g_free() once the backend resolved the shape; caller shapes are
 *  updated on success only and never written past. 0 ok, <0 error. */
int dt_ai_remote_run(dt_ai_remote_model_t *model,
                     dt_ai_tensor_t *inputs, const int num_inputs,
                     dt_ai_tensor_t *outputs, const int num_outputs);

/** Close the connection and free the model. */
void dt_ai_remote_free(dt_ai_remote_model_t *model);

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
