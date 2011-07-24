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
#define DT_OPENCL_EVENTLISTSIZE 256
#define DT_OPENCL_EVENTNAMELENGTH 64
#define DT_OPENCL_MAX_EVENTS 256

#define DT_OPENCL_MEMORY_HEADROOM (256*1024*1024)

#ifdef HAVE_OPENCL

// #pragma GCC diagnostic push
// #pragma GCC diagnostic ignored "-Wcomment"
#include <CL/cl.h>
// #pragma GCC diagnostic
#include "common/dtpthread.h"
#include "common/dlopencl.h"
#include "control/conf.h"


/**
 * Accounting information used for OpenCL events.
 */
typedef struct dt_opencl_eventtag_t
{
  cl_int retval;
  cl_ulong timelapsed;
  char tag[DT_OPENCL_EVENTNAMELENGTH];
}
dt_opencl_eventtag_t;


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
  cl_ulong max_mem_alloc;
  cl_ulong max_global_mem;
  cl_ulong used_global_mem;
  cl_program program[DT_OPENCL_MAX_PROGRAMS];
  cl_kernel  kernel [DT_OPENCL_MAX_KERNELS];
  int program_used[DT_OPENCL_MAX_PROGRAMS];
  int kernel_used [DT_OPENCL_MAX_KERNELS];
  cl_event *eventlist;
  dt_opencl_eventtag_t *eventtags;
  int numevents;
  int eventsconsolidated;
  int maxevents;
  cl_int summary;
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
int dt_opencl_finish(const int devid);

/** enqueues a synchronization point. */
int dt_opencl_enqueue_barrier(const int devid);

/** locks a device for your thread's exclusive use. blocks if it's busy. pass -1 to let dt chose a device.
 *  always use the devid returned, in case you didn't get your request! */
int dt_opencl_lock_device(const int dev);

/** done with your command queue. */
void dt_opencl_unlock_device(const int dev);

/** loads the given .cl file and returns a reference to an internal program. */
int dt_opencl_load_program(const int dev, const char *filename);

/** builds the given program. */
int dt_opencl_build_program(const int dev, const int program);

/** inits a kernel. returns the index or -1 if fail. */
int dt_opencl_create_kernel(const int program, const char *name);

/** releases kernel resources again. */
void dt_opencl_free_kernel(const int kernel);

/** return max size in sizes[3]. */
int dt_opencl_get_max_work_item_sizes(const int dev, size_t *sizes);

/** attach arg. */
int dt_opencl_set_kernel_arg(const int dev, const int kernel, const int num, const size_t size, const void *arg);

/** launch kernel! */
int dt_opencl_enqueue_kernel_2d(const int dev, const int kernel, const size_t *sizes);

/** check if opencl is inited */
int dt_opencl_is_inited(void);

/** check if opencl is enabled */
int dt_opencl_is_enabled(void);

/** disable opencl */
void dt_opencl_disable(void);

/** update enabled flag with value from preferences */
int dt_opencl_update_enabled(void);

/** HAVE_OPENCL mode only: copy and alloc buffers. */
int dt_opencl_copy_device_to_host(const int devid, void *host, void *device, const int width, const int height, const int bpp);

int dt_opencl_read_host_from_device(const int devid, void *host, void *device, const int width, const int height, const int bpp);

int dt_opencl_read_host_from_device_rowpitch(const int devid, void *host, void *device, const int width, const int height, const int rowpitch);

int dt_opencl_read_host_from_device_raw(const int devid, void *host, void *device, const size_t *origin, const size_t *region, const int rowpitch, const int blocking);

int dt_opencl_write_host_to_device(const int devid, void *host, void *device, const int width, const int height, const int bpp);

int dt_opencl_write_host_to_device_rowpitch(const int devid, void *host, void *device, const int width, const int height, const int rowpitch);

int dt_opencl_write_host_to_device_raw(const int devid, void *host, void *device, const size_t *origin, const size_t *region, const int rowpitch, const int blocking);

void* dt_opencl_copy_host_to_device(const int devid, void *host, const int width, const int height, const int bpp);

void* dt_opencl_copy_host_to_device_rowpitch(const int devid, void *host, const int width, const int height, const int bpp, const int rowpitch);

void* dt_opencl_copy_host_to_device_constant(const int devid, const int size, void *host);

int dt_opencl_enqueue_copy_image(const int devid, cl_mem src, cl_mem dst, size_t *orig_src, size_t *orig_dst, size_t *region);

void* dt_opencl_alloc_device(const int devid, const int width, const int height, const int bpp);

void* dt_opencl_alloc_device_use_host_pointer(const int devid, const int width, const int height, const int bpp, const int rowpitch, void *host);

int dt_opencl_enqueue_copy_image_to_buffer(const int devid, cl_mem src_image, cl_mem dst_buffer, size_t *origin, size_t *region, size_t offset);

int dt_opencl_enqueue_copy_buffer_to_image(const int devid, cl_mem src_buffer, cl_mem dst_image, size_t offset, size_t *origin, size_t *region);

void* dt_opencl_alloc_device_buffer(const int devid, const int size);

void dt_opencl_release_mem_object(void *mem);

/** check if image size fit into limits given by OpenCL runtime */
int dt_opencl_image_fits_device(const int devid, const size_t width, const size_t height, const size_t bytes);

/** get global memory of device */
cl_ulong dt_opencl_get_max_global_mem(const int devid);

/** get next free slot in eventlist and manage size of eventlist */
cl_event *dt_opencl_events_get_slot(const int devid, const char *tag);

/** reset eventlist to empty state */
void dt_opencl_events_reset(const int devid);

/** Wait for events in eventlist to terminate -> this is a blocking synchronization point
    Does not flush eventlist */
void dt_opencl_events_wait_for(const int devid);

/** Wait for events in eventlist to terminate, check for return status of events and
    report summary success info (CL_COMPLETE or last error code) */
cl_int dt_opencl_events_flush(const int devid, const int reset);

/** display OpenCL profiling information. If summary is not 0, try to generate summarized info for kernels */
void dt_opencl_events_profiling(const int devid, const int aggregated);

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
static inline int dt_opencl_finish(const int devid)
{
  return -1;
}
static inline int dt_opencl_enqueue_barrier(const int devid)
{
  return -1;
}
static inline int dt_opencl_lock_device(const int dev)
{
  return -1;
}
static inline void dt_opencl_unlock_device(const int dev) {}
static inline int dt_opencl_load_program(const int dev, const char *filename)
{
  return -1;
}
static inline int dt_opencl_build_program(const int dev, const int program)
{
  return -1;
}
static inline int dt_opencl_create_kernel(const int program, const char *name)
{
  return -1;
}
static inline void dt_opencl_free_kernel(const int kernel) {}
static inline int  dt_opencl_get_max_work_item_sizes(const int dev, size_t *sizes)
{
  return 0;
}
static inline int dt_opencl_set_kernel_arg(const int dev, const int kernel, const size_t size, const void *arg)
{
  return -1;
}
static inline int dt_opencl_enqueue_kernel_2d(const int dev, const int kernel, const size_t *sizes)
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
static inline void dt_opencl_disable(void) {}
static inline int dt_opencl_update_enabled(void)
{
  return 0;
}
static int dt_opencl_image_fits_device(const int devid, const size_t width, const size_t height, const size_t bytes)
{
  return 0;
}
static inline int dt_opencl_get_max_global_mem(const int devid)
{
  return 0;
}
static inline void dt_opencl_release_mem_object(void *mem) {}
static inline void *dt_opencl_events_get_slot(const int devid, const char *tag)
{
  return NULL;
}
static inline void dt_opencl_events_reset(const int devid) {}
static void dt_opencl_events_wait_for(const int devid) {}
static inline int dt_opencl_events_flush(const int devid, const int reset)
{
  return -1;
}
void dt_opencl_events_profiling(const int devid, const int aggregated) {}
#endif


#endif
