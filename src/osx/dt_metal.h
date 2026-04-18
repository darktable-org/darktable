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

#include "common/darktable.h"
#include <sys/types.h>

/** Maximum number of Metal compute pipelines (kernel functions) that can be registered */
#define DT_METAL_MAX_KERNELS 512

/** Error code returned by Metal kernel dispatch on failure */
#define DT_METAL_DEFAULT_ERROR -1

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dt_metal_device_t
{
  dt_pthread_mutex_t lock;
  u_int64_t devid;
  void *device;         // stores MTL::Device* as void*
  void *library;        // stores MTL::Library* as void*
  void *command_queue;  // stores MTL::CommandQueue* as void*

  /** Active batch state (non-NULL when inside begin_batch/end_batch) */
  void *active_command_buffer;  // MTL::CommandBuffer* during batch
  void *active_encoder;         // MTL::ComputeCommandEncoder* during batch
} dt_metal_device_t;


/**
 * main struct, stored in darktable.metal.
 * holds pointers to Metal devices, the compiled metallib library,
 * and registered compute pipelines.
 */
typedef struct dt_metal_t
{
  int num_devs;
  dt_metal_device_t *dev;

  /** registered compute pipeline states (indexed by kernel_id) */
  int num_kernels;
  void *kernels[DT_METAL_MAX_KERNELS];  // stores MTL::ComputePipelineState* as void*
} dt_metal_t;


/** Initialize Metal devices and load the compiled metallib */
void dt_metal_init(dt_metal_t *metal);

/** Clean up all Metal resources (pipelines, queues, libraries) */
void dt_metal_cleanup(dt_metal_t *metal);

/** Print all discovered Metal device names */
void dt_metal_list_devices(dt_metal_t *metal);

/** Check if Metal compute is available (at least one device with a valid library) */
gboolean dt_metal_is_available(dt_metal_t *metal);

/**
 * Create a compute pipeline for a named kernel function in the metallib.
 * Returns a kernel_id >= 0 on success, or -1 on error.
 * The kernel_id is used with dt_metal_enqueue_kernel_2d().
 */
int dt_metal_create_kernel(dt_metal_t *metal, const char *kernel_name);

/**
 * Free a previously created kernel pipeline.
 */
void dt_metal_free_kernel(dt_metal_t *metal, int kernel_id);

/**
 * Dispatch a 2D compute kernel over a width x height grid.
 *
 * The kernel function in the metallib must follow this argument convention:
 *   buffer(0): input  pixel data (device const float4*)
 *   buffer(1): output pixel data (device float4*)
 *   buffer(2): constant int &width
 *   buffer(3): constant int &height
 *   buffer(4..): additional scalar arguments, set via extra_args/extra_arg_sizes
 *
 * @param metal       The global Metal context
 * @param kernel_id   Pipeline ID returned by dt_metal_create_kernel()
 * @param width       Image width in pixels
 * @param height      Image height in pixels
 * @param input       Input pixel data (float4 per pixel, width*height*4 floats)
 * @param output      Output pixel data (same layout)
 * @param num_extra_args  Number of additional scalar arguments (bound to buffer(4), buffer(5), ...)
 * @param extra_args      Array of pointers to the extra argument data
 * @param extra_arg_sizes Array of sizes (in bytes) for each extra argument
 * @return 0 on success, DT_METAL_DEFAULT_ERROR on failure
 */
int dt_metal_enqueue_kernel_2d(dt_metal_t *metal,
                               int kernel_id,
                               int width, int height,
                               const float *input,
                               float *output,
                               int num_extra_args,
                               const void **extra_args,
                               const size_t *extra_arg_sizes);

/* ── Multi-buffer API ──────────────────────────────────────────────
 * For modules that need multiple GPU buffers (e.g. diffuse/sharpen).
 * Buffers are allocated on the Metal device with shared storage and
 * persist across kernel dispatches until explicitly freed.
 */

/** Opaque handle for a Metal buffer */
typedef void *dt_metal_buffer_t;

/**
 * Allocate a Metal buffer on the device.
 * @param metal  The global Metal context
 * @param size   Size in bytes
 * @return Handle to Metal buffer, or NULL on failure
 */
dt_metal_buffer_t dt_metal_alloc_buffer(dt_metal_t *metal, size_t size);

/**
 * Free a Metal buffer previously allocated with dt_metal_alloc_buffer().
 */
void dt_metal_free_buffer(dt_metal_buffer_t buf);

/**
 * Copy host data into a Metal buffer.
 * @param buf   Metal buffer handle
 * @param src   Source host pointer
 * @param size  Number of bytes to copy
 */
void dt_metal_copy_to_buffer(dt_metal_buffer_t buf, const void *src, size_t size);

/**
 * Copy data from a Metal buffer to host memory.
 * @param buf   Metal buffer handle
 * @param dst   Destination host pointer
 * @param size  Number of bytes to copy
 */
void dt_metal_copy_from_buffer(dt_metal_buffer_t buf, void *dst, size_t size);

/** Argument type for dt_metal_enqueue_kernel_2d_flex() */
typedef enum dt_metal_arg_type_t
{
  DT_METAL_ARG_BUFFER,   /**< a dt_metal_buffer_t handle */
  DT_METAL_ARG_BYTES     /**< inline scalar data (like setBytes) */
} dt_metal_arg_type_t;

typedef struct dt_metal_arg_t
{
  dt_metal_arg_type_t type;
  const void *data;   /**< for BUFFER: pointer to dt_metal_buffer_t; for BYTES: pointer to scalar data */
  size_t size;         /**< for BYTES: size of scalar data; for BUFFER: unused */
} dt_metal_arg_t;

/**
 * Flexible 2D kernel dispatch with arbitrary buffer and scalar arguments.
 * Arguments are bound to buffer(0), buffer(1), ..., buffer(N-1) in order.
 *
 * @param metal       The global Metal context
 * @param kernel_id   Pipeline ID returned by dt_metal_create_kernel()
 * @param width       Grid width
 * @param height      Grid height
 * @param num_args    Number of arguments
 * @param args        Array of argument descriptors
 * @return 0 on success, DT_METAL_DEFAULT_ERROR on failure
 */
int dt_metal_enqueue_kernel_2d_flex(dt_metal_t *metal,
                                    int kernel_id,
                                    int width, int height,
                                    int num_args,
                                    const dt_metal_arg_t *args);

/* ── Batch dispatch API ────────────────────────────────────────────
 * Batch multiple kernel dispatches into a single Metal command buffer
 * to eliminate per-kernel GPU round-trip overhead.  Kernels within a
 * batch execute sequentially with automatic memory barriers between them.
 *
 * Usage:
 *   dt_metal_begin_batch(metal);
 *   dt_metal_enqueue_kernel_2d_flex(metal, ...);  // encoded, not submitted
 *   dt_metal_enqueue_kernel_2d_flex(metal, ...);  // encoded, not submitted
 *   dt_metal_end_batch(metal);                    // submit + wait
 */

/**
 * Begin a batch of kernel dispatches.
 * Creates a command buffer and compute encoder that will be reused
 * by subsequent dt_metal_enqueue_kernel_2d_flex() calls.
 * @return 0 on success, DT_METAL_DEFAULT_ERROR on failure
 */
int dt_metal_begin_batch(dt_metal_t *metal);

/**
 * End a batch: finalize encoding, submit to GPU, and wait for completion.
 * @return 0 on success, DT_METAL_DEFAULT_ERROR on failure
 */
int dt_metal_end_batch(dt_metal_t *metal);

/* ── No-copy buffer API ────────────────────────────────────────────
 * On Apple Silicon (unified memory), create a Metal buffer that wraps
 * an existing host pointer without copying.  The host pointer must be
 * page-aligned and the caller must keep it alive while the buffer exists.
 */

/**
 * Create a Metal buffer wrapping an existing host pointer (no copy).
 * Falls back to alloc+copy if the pointer is not page-aligned.
 * @param metal  The global Metal context
 * @param ptr    Host pointer (ideally page-aligned)
 * @param size   Size in bytes
 * @return Handle to Metal buffer, or NULL on failure
 */
dt_metal_buffer_t dt_metal_alloc_buffer_nocopy(dt_metal_t *metal, void *ptr, size_t size);

/**
 * Flexible 2D kernel dispatch with custom threadgroup size.
 * Same as dt_metal_enqueue_kernel_2d_flex but allows specifying
 * the threadgroup dimensions for optimal memory access patterns.
 * Pass threadW=0, threadH=0 to use the default 16x16.
 */
int dt_metal_enqueue_kernel_2d_flex_with_tgs(dt_metal_t *metal,
                                              int kernel_id,
                                              int width, int height,
                                              int num_args,
                                              const dt_metal_arg_t *args,
                                              int threadW, int threadH);


#ifdef __cplusplus
}
#endif

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

