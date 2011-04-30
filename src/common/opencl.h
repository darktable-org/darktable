/*
    This file is part of darktable,
    copyright (c) 2009--2010 johannes hanika.

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
#ifndef DT_OPENCL_H
#define DT_OPENCL_H

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#define DT_OPENCL_MAX_PROGRAMS 256
#define DT_OPENCL_MAX_KERNELS 512

#ifdef HAVE_OPENCL

// #pragma GCC diagnostic push
// #pragma GCC diagnostic ignored "-Wcomment"
#include <CL/cl.h>
// #pragma GCC diagnostic
#include "common/dtpthread.h"
#include "common/dlopencl.h"
#include "control/conf.h"

/**
 * to support multi-gpu and mixed systems with cpu support,
 * we encapsulate devices and use separate command queues.
 */
typedef struct dt_opencl_device_t
{
  dt_pthread_mutex_t lock;
  cl_device_id devid;
  cl_context context;
  cl_command_queue cmd_queue;
  size_t max_image_width;
  size_t max_image_height;
  cl_program program[DT_OPENCL_MAX_PROGRAMS];
  cl_kernel  kernel [DT_OPENCL_MAX_KERNELS];
  int program_used[DT_OPENCL_MAX_PROGRAMS];
  int kernel_used [DT_OPENCL_MAX_KERNELS];
}
dt_opencl_device_t;

/**
 * main struct, stored in darktable.opencl.
 * holds pointers to all
 */
typedef struct dt_opencl_t
{
  dt_pthread_mutex_t lock;
  int inited;
  int enabled;
  int num_devs;
  dt_opencl_device_t *dev;
  dt_dlopencl_t *dlocl;
}
dt_opencl_t;

/** inits the opencl subsystem. */
void dt_opencl_init(dt_opencl_t *cl, const int argc, char *argv[]);

/** cleans up the opencl subsystem. */
void dt_opencl_cleanup(dt_opencl_t *cl);

/** cleans up command queue. */
void dt_opencl_finish(cl_command_queue q);

/** locks a device for your thread's exclusive use. blocks if it's busy. pass -1 to let dt chose a device.
 *  always use the devid returned, in case you didn't get your request! */
int dt_opencl_lock_device(dt_opencl_t *cl, const int dev);

/** done with your command queue. */
void dt_opencl_unlock_device(dt_opencl_t *cl, const int dev);

/** loads the given .cl file and returns a reference to an internal program. */
int dt_opencl_load_program(dt_opencl_t *cl, const int dev, const char *filename);

/** builds the given program. */
int dt_opencl_build_program(dt_opencl_t *cl, const int dev, const int program);

/** inits a kernel. returns the index or -1 if fail. */
int dt_opencl_create_kernel(dt_opencl_t *cl, const int program, const char *name);

/** releases kernel resources again. */
void dt_opencl_free_kernel(dt_opencl_t *cl, const int kernel);

/** return max size in sizes[3]. */
int dt_opencl_get_max_work_item_sizes(dt_opencl_t *cl, const int dev, size_t *sizes);

/** attach arg. */
int dt_opencl_set_kernel_arg(dt_opencl_t *cl, const int dev, const int kernel, const int num, const size_t size, const void *arg);

/** launch kernel! */
int dt_opencl_enqueue_kernel_2d(dt_opencl_t *cl, const int dev, const int kernel, const size_t *sizes);

/** check if opencl is inited */
int dt_opencl_is_inited(void);

/** check if opencl is enabled */
int dt_opencl_is_enabled(void);

/** update enabled flag with value from preferences */
void dt_opencl_update_enabled(void);

/** HAVE_OPENCL mode only: copy and alloc buffers. */
void dt_opencl_copy_device_to_host(void *host, void *device, const int width, const int height, const int devid, const int bpp);

void* dt_opencl_copy_host_to_device(void *host, const int width, const int height, const int devid, const int bpp);

void* dt_opencl_copy_host_to_device_constant(const int size, const int devid, void *host);

int dt_opencl_enqueue_copy_image(cl_command_queue q, cl_mem src, cl_mem dst, size_t *orig_src, size_t *orig_dst, size_t *region, int events, cl_event *wait, cl_event *event);

void* dt_opencl_alloc_device(const int width, const int height, const int devid, const int bpp);

void dt_opencl_release_mem_object(void *mem);

/** check if image size fit into limits given by OpenCL runtime */
int dt_opencl_image_fits_device(const int devid, const size_t width, const size_t height);

#else
#include <stdlib.h>
#include "control/conf.h"
typedef struct dt_opencl_t
{
  int inited;
  int enabled;
} dt_opencl_t;
static inline void dt_opencl_init(dt_opencl_t *cl, const int argc, char *argv[])
{
  cl->inited = 0;
  cl->enabled = 0;
  dt_conf_set_bool("opencl", FALSE);
}
static inline void dt_opencl_cleanup(dt_opencl_t *cl) {}
static inline int dt_opencl_lock_device(dt_opencl_t *cl, const int dev)
{
  return -1;
}
static inline void dt_opencl_unlock_device(dt_opencl_t *cl, const int dev) {}
static inline int dt_opencl_load_program(dt_opencl_t *cl, const int dev, const char *filename)
{
  return -1;
}
static inline int dt_opencl_build_program(dt_opencl_t *cl, const int dev, const int program)
{
  return -1;
}
static inline int dt_opencl_create_kernel(dt_opencl_t *cl, const int program, const char *name)
{
  return -1;
}
static inline void dt_opencl_free_kernel(dt_opencl_t *cl, const int kernel) {}
static inline int  dt_opencl_get_max_work_item_sizes(dt_opencl_t *cl, const int dev, size_t *sizes)
{
  return 0;
}
static inline int dt_opencl_set_kernel_arg(dt_opencl_t *cl, const int dev, const int kernel, const size_t size, const void *arg)
{
  return -1;
}
static inline int dt_opencl_enqueue_kernel_2d(dt_opencl_t *cl, const int dev, const int kernel, const size_t *sizes)
{
  return -1;
}
static inline int dt_opencl_is_inited(void)
{
  return 0;
}
static inline int dt_opencl_is_enabled(void)
{
  return 0;
}
static inline void dt_opencl_update_enabled(void) {}

#endif


#endif
