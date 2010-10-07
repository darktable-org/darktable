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

#ifdef HAVE_OPENCL

#include <CL/opencl.h>

#define DT_OPENCL_MAX_PROGRAMS 256
#define DT_OPENCL_MAX_KERNELS 512 

typedef struct dt_opencl_t
{
  int inited;
  cl_context *context;
  cl_device_id *devid;
  cl_command_queue *cmd_queue;
  cl_program program[DT_OPENCL_MAX_PROGRAMS];
  cl_kernel  kernel [DT_OPENCL_MAX_KERNELS];
  int program_used[DT_OPENCL_MAX_PROGRAMS];
  int kernel_used [DT_OPENCL_MAX_KERNELS];
}
dt_opencl_t;

/** inits the opencl subsystem. */
void dt_opencl_init(dt_opencl_t *cl);

/** cleans up the opencl subsystem. */
void dt_opencl_cleanup(dt_opencl_t *cl);

/** loads the given .cl file and returns a reference to an internal program. */
int dt_opencl_load_program(dt_opencl_t *cl, const char *filename);

/** builds the given program. */
int dt_opencl_build_program(dt_opencl_t *cl, const int program);

/** inits a kernel. returns the index or -1 if fail. */
int dt_opencl_create_kernel(dt_opencl_t *cl, const int program, const char *name);

/** return max size in sizes[3]. */
void dt_opencl_get_max_work_item_sizes(dt_opencl_t *cl, size_t *sizes);

/** attach arg. */
int dt_opencl_set_kernel_arg(dt_opencl_t *cl, const int kernel, const size_t size, const void *arg);

/** launch kernel! */
int dt_opencl_enqueue_kernel_2d(dt_opencl_t *cl, const int kernel, const size_t *sizes);


#else
typedef struct dt_opencl_t void*;
#endif

#endif
