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
#include <strings.h>
#include <stdio.h>

#include "common/darktable.h"
#include "common/opencl.h"

void dt_opencl_init(dt_opencl_t *cl)
{
  bzero(cl->program_used, sizeof(int)*DT_OPENCL_MAX_PROGRAMS);
  bzero(cl->kernel_used,  sizeof(int)*DT_OPENCL_MAX_KERNELS);
  cl->inited = 0;
  cl_int err;
  cl_platform_id all_platforms[5];
  cl_platform_id platform = NULL;
  cl_uint num_platforms = 5;
  err = clGetPlatformIDs (5, all_platforms, &num_platforms);
  if(err != CL_SUCCESS)
  {
    dt_print(DT_DEBUG_OPENCL, "[opencl_init] could not get platforms: %d\n", err);
    return;
  }
  platform = all_platforms[0];

  // get the number of GPU devices available to the platform
  // the other common option is CL_DEVICE_TYPE_CPU (but doesn't work with the nvidia drivers)
  cl_uint num_devices = 0;
  err = clGetDeviceIDs(platform, CL_DEVICE_TYPE_GPU, 0, NULL, &num_devices);
  // err = clGetDeviceIDs(platform, CL_DEVICE_TYPE_ALL, 0, NULL, &num_devices);
  if(err != CL_SUCCESS)
  {
    dt_print(DT_DEBUG_OPENCL, "[opencl_init] could not get device id size: %d\n", err);
    return;
  }

  // create the device list
  cl_device_id *devices = (cl_device_id *)malloc(sizeof(cl_device_id)*num_devices);
  err = clGetDeviceIDs(platform, CL_DEVICE_TYPE_ALL, num_devices, devices, NULL);
  if(err != CL_SUCCESS)
  {
    dt_print(DT_DEBUG_OPENCL, "[opencl_init] could not get devices list: %d\n", err);
    return;
  }
  dt_print(DT_DEBUG_OPENCL, "[opencl_init] found %d devices\n", num_devices);

  cl_int device_used = 0;
  cl->devid = devices[device_used];
  cl->context = clCreateContext(0, 1, &cl->devid, NULL, NULL, &err);
  if(err != CL_SUCCESS)
  {
    dt_print(DT_DEBUG_OPENCL, "[opencl_init] could not create context: %d\n", err);
    return;
  }

  // create a command queue for first device the context reported
  cl->cmd_queue = clCreateCommandQueue(cl->context, cl->devid, 0, &err);
  if(err != CL_SUCCESS)
  {
    dt_print(DT_DEBUG_OPENCL, "[opencl_init] could not create command queue: %d\n", err);
    return;
  }


  char dtpath[1024], filename[1024], programname[1024];
  dt_get_datadir(dtpath, 1024);
  snprintf(filename, 1024, "%s/programs.conf", dtpath);
  // now load all darktable cl kernels.
  // TODO: compile as a job?
  FILE *f = fopen(filename, "rb");
  if(f)
  {
    while(!feof(f))
    {
      int rd = fscanf(f, "%[^#]%*[^\n]\n", programname);
      if(rd != 1) continue;
      if(programname[0] == '#') continue;
      snprintf(filename, 1024, "%s/%s", dtpath, programname);
      const int prog = dt_opencl_load_program(cl, filename);
      if(dt_opencl_build_program(cl, prog))
        dt_print(DT_DEBUG_OPENCL, "[opencl_init] failed to compile program `%s'!\n", programname);
    }
    fclose(f);
  }
  else
  {
    dt_print(DT_DEBUG_OPENCL, "[opencl_init] could not open `%s'!\n", filename);
    return;
  }

  dt_print(DT_DEBUG_OPENCL, "[opencl_init] successfully initialized.\n");
  cl->inited = 1;
  return;
}

void dt_opencl_cleanup(dt_opencl_t *cl)
{
  for(int k=0;k<DT_OPENCL_MAX_KERNELS; k++) if(cl->kernel_used [k]) clReleaseKernel (cl->kernel [k]);
  for(int k=0;k<DT_OPENCL_MAX_PROGRAMS;k++) if(cl->program_used[k]) clReleaseProgram(cl->program[k]);
  clReleaseCommandQueue(cl->cmd_queue);
  clReleaseContext(cl->context);
}

int dt_opencl_load_program(dt_opencl_t *cl, const char *filename)
{
  cl_int err;
  FILE *f = fopen(filename, "rb");
  if(!f)
  {
    dt_print(DT_DEBUG_OPENCL, "[opencl_load_program] could not open file `%s'!\n", filename);
    return -1;
  }
  fseek(f, 0, SEEK_END);
  size_t filesize = ftell(f);
  fseek(f, 0, SEEK_SET);
  char file[filesize+1];
  int rd = fread(file, sizeof(char), filesize, f);
  fclose(f);
  if(rd != filesize)
  {
    dt_print(DT_DEBUG_OPENCL, "[opencl_load_program] could not read all of file `%s'!\n", filename);
    return -1;
  }
  if(file[filesize-1] != '\n')
  {
    dt_print(DT_DEBUG_OPENCL, "[opencl_load_program] no newline at end of file `%s'!\n", filename);
    file[filesize] = '\n';
  }
  int lines = 0;
  for(int k=0;k<filesize;k++) if(file[k] == '\n') lines++;
  const char *sptr[lines+1];
  size_t lengths[lines];
  int curr = 0;
  sptr[curr++] = file;
  for(int k=0;k<filesize;k++)
    if(file[k] == '\n')
    {
      sptr[curr] = file + k + 1;
      lengths[curr-1] = sptr[curr] - sptr[curr-1];
      curr++;
    }
  lengths[lines-1] = file + filesize - sptr[lines-1];
  sptr[lines] = NULL;
  int k = 0;
  for(;k<DT_OPENCL_MAX_PROGRAMS;k++) if(!cl->program_used[k])
  {
    cl->program_used[k] = 1;
    cl->program[k] = clCreateProgramWithSource(cl->context, lines, sptr, lengths, &err);
    if(err != CL_SUCCESS)
    {
      dt_print(DT_DEBUG_OPENCL, "[opencl_load_program] could not create program from file `%s'! (%d)\n", filename, err);
      cl->program_used[k] = 0;
      return -1;
    }
  }
  if(k < DT_OPENCL_MAX_PROGRAMS)
  {
    dt_print(DT_DEBUG_OPENCL, "[opencl_load_program] successfully loaded program from `%s'\n", filename);
    return k;
  }
  else
  {
    dt_print(DT_DEBUG_OPENCL, "[opencl_load_program] too many programs! can't load `%s'\n", filename);
    return -1;
  }
}

int dt_opencl_build_program(dt_opencl_t *cl, const int prog)
{
  if(prog < 0 || prog >= DT_OPENCL_MAX_PROGRAMS) return -1;
  cl_program program = cl->program[prog];
  cl_int err;
  // TODO: how to pass sm_23 for fermi?
  err = clBuildProgram(program, 1, &cl->devid, "-cl-fast-relaxed-math -cl-strict-aliasing", 0, 0);
  if(err != CL_SUCCESS)
  {
    dt_print(DT_DEBUG_OPENCL, "[opencl_build_program] could not build program: %d\n", err);
    cl_build_status build_status;
    clGetProgramBuildInfo(program, cl->devid, CL_PROGRAM_BUILD_STATUS, sizeof(cl_build_status), &build_status, NULL);
    if (build_status != CL_SUCCESS)
    {
      char *build_log;
      size_t ret_val_size;
      clGetProgramBuildInfo(program, cl->devid, CL_PROGRAM_BUILD_LOG, 0, NULL, &ret_val_size);
      build_log = (char *)malloc(sizeof(char)*(ret_val_size+1));
      clGetProgramBuildInfo(program, cl->devid, CL_PROGRAM_BUILD_LOG, ret_val_size, build_log, NULL);

      build_log[ret_val_size] = '\0';

      dt_print(DT_DEBUG_OPENCL, "BUILD LOG:\n");
      dt_print(DT_DEBUG_OPENCL, "%s\n", build_log);

      free(build_log);
    }
  }
  return err;
}

int dt_opencl_create_kernel(dt_opencl_t *cl, const int prog, const char *name)
{
  if(prog < 0 || prog >= DT_OPENCL_MAX_PROGRAMS) return -1;
  cl_int err;
  int k = 0;
  for(;k<DT_OPENCL_MAX_KERNELS;k++) if(!cl->kernel_used[k])
  {
    cl->kernel_used[k] = 1;
    cl->kernel[k] = clCreateKernel(cl->program[prog], name, &err);
    if(err != CL_SUCCESS)
    {
      dt_print(DT_DEBUG_OPENCL, "[opencl_create_kernel] could not create kernel `%s'! (%d)\n", name, err);
      cl->kernel_used[k] = 0;
      return -1;
    }
  }
  if(k < DT_OPENCL_MAX_KERNELS)
  {
    dt_print(DT_DEBUG_OPENCL, "[opencl_create_kernel] successfully loaded kernel `%s'\n", name);
    return k;
  }
  else
  {
    dt_print(DT_DEBUG_OPENCL, "[opencl_create_kernel] too many kernels! can't create kernel `%s'\n", name);
    return -1;
  }
}

void dt_opencl_get_max_work_item_sizes(dt_opencl_t *cl, size_t *sizes)
{
  cl_int err = clGetDeviceInfo(cl->devid, CL_DEVICE_MAX_WORK_ITEM_SIZES, sizeof(size_t)*3, sizes, NULL);
  if(err != CL_SUCCESS) dt_print(DT_DEBUG_OPENCL, "[opencl_get_max_work_item_sizes] could not get size! %d\n", err);
}

int dt_opencl_set_kernel_arg(dt_opencl_t *cl, const int kernel, const int num, const size_t size, const void *arg)
{
  if(kernel < 0 || kernel >= DT_OPENCL_MAX_KERNELS) return -1;
  return clSetKernelArg(cl->kernel[kernel], num, size, arg);
}

int dt_opencl_enqueue_kernel_2d(dt_opencl_t *cl, const int kernel, const size_t *sizes)
{
  if(kernel < 0 || kernel >= DT_OPENCL_MAX_KERNELS) return -1;
  const size_t local[2] = {16, 16};
  return clEnqueueNDRangeKernel(cl->cmd_queue, cl->kernel[kernel], 2, NULL, sizes, local, 0, NULL, NULL);
}


