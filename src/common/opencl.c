/*
    This file is part of darktable,
    copyright (c) 2009--2011 johannes hanika.

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

#ifdef HAVE_OPENCL

#include <string.h>
#include <stdio.h>
#include <assert.h>

#include "common/darktable.h"
#include "common/opencl.h"
#include "common/dlopencl.h"
#include "control/conf.h"

void dt_opencl_init(dt_opencl_t *cl, const int argc, char *argv[])
{
  dt_pthread_mutex_init(&cl->lock, NULL);
  cl->inited = 0;
  cl->enabled = 0;
  cl->dlocl = NULL;
  int exclude_opencl = 0;

  // preliminary disable opencl in prefs. We will re-set it to previous state later if possible
  // Will remain disabled if initialization fails.
  const int prefs = dt_conf_get_bool("opencl");
  dt_conf_set_bool("opencl", FALSE);

  for(int k=0; k<argc; k++) if(!strcmp(argv[k], "--disable-opencl"))
    {
      dt_print(DT_DEBUG_OPENCL, "[opencl_init] do not try to find and use an opencl runtime library due to explicit user request\n");
      exclude_opencl = 1;
    }

  if(exclude_opencl) return;


  // look for explicit definition of opencl_runtime library in preferences
  const char *library = dt_conf_get_string("opencl_runtime");
  dt_print(DT_DEBUG_OPENCL, "[opencl_init] trying to load opencl library: '%s'\n", library && strlen(library) != 0 ? library : "<system default>");

  // dynamically load opencl runtime
  if(!dt_dlopencl_init(library, &cl->dlocl))
    {
      dt_print(DT_DEBUG_OPENCL, "[opencl_init] no working opencl library found. Continue with opencl disabled\n");
      return;
    }
    else
    {
      dt_print(DT_DEBUG_OPENCL, "[opencl_init] opencl library '%s' found on your system and loaded\n", cl->dlocl->library);
    }

  cl_int err;
  cl_platform_id all_platforms[5];
  cl_platform_id platform = NULL;
  cl_uint num_platforms = 5;
  err = (cl->dlocl->symbols->dt_clGetPlatformIDs) (5, all_platforms, &num_platforms);
  if(err != CL_SUCCESS)
  {
    dt_print(DT_DEBUG_OPENCL, "[opencl_init] could not get platforms: %d\n", err);
    return;
  }
  platform = all_platforms[0];

  // get the number of GPU devices available to the platform
  // the other common option is CL_DEVICE_TYPE_GPU/CPU (but the latter doesn't work with the nvidia drivers)
  cl_uint num_devices = 0;
  err = (cl->dlocl->symbols->dt_clGetDeviceIDs)(platform, CL_DEVICE_TYPE_ALL, 0, NULL, &num_devices);
  if(err != CL_SUCCESS)
  {
    dt_print(DT_DEBUG_OPENCL, "[opencl_init] could not get device id size: %d\n", err);
    return;
  }

  // create the device list
  cl->dev = (dt_opencl_device_t *)malloc(sizeof(dt_opencl_device_t)*num_devices);
  cl_device_id *devices = (cl_device_id *)malloc(sizeof(cl_device_id)*num_devices);
  err = (cl->dlocl->symbols->dt_clGetDeviceIDs)(platform, CL_DEVICE_TYPE_ALL, num_devices, devices, NULL);
  if(err != CL_SUCCESS)
  {
    dt_print(DT_DEBUG_OPENCL, "[opencl_init] could not get devices list: %d\n", err);
    return;
  }
  dt_print(DT_DEBUG_OPENCL, "[opencl_init] found %d devices\n", num_devices);
  int dev = 0;
  for(int k=0; k<num_devices; k++)
  {
    memset(cl->dev[dev].program_used, 0x0, sizeof(int)*DT_OPENCL_MAX_PROGRAMS);
    memset(cl->dev[dev].kernel_used,  0x0, sizeof(int)*DT_OPENCL_MAX_KERNELS);
    cl->dev[dev].eventlist = NULL;
    cl->dev[dev].eventtags = NULL;
    cl->dev[dev].numevents = 0;
    cl->dev[dev].eventsconsolidated = 0;
    cl->dev[dev].maxevents = 0;
    cl->dev[dev].summary=CL_COMPLETE;
    cl->dev[dev].used_global_mem = 0;
    cl_device_id devid = cl->dev[dev].devid = devices[k];

    char infostr[1024];
    size_t infoint;
    size_t infointtab[1024];
    cl_bool image_support = 0;

    // test 1GB mem and image support:
    (cl->dlocl->symbols->dt_clGetDeviceInfo)(devid, CL_DEVICE_NAME, sizeof(infostr), &infostr, NULL);
    (cl->dlocl->symbols->dt_clGetDeviceInfo)(devid, CL_DEVICE_IMAGE_SUPPORT, sizeof(cl_bool), &image_support, NULL);
    (cl->dlocl->symbols->dt_clGetDeviceInfo)(devid, CL_DEVICE_IMAGE2D_MAX_HEIGHT, sizeof(size_t), &(cl->dev[dev].max_image_height), NULL);
    (cl->dlocl->symbols->dt_clGetDeviceInfo)(devid, CL_DEVICE_IMAGE2D_MAX_WIDTH,  sizeof(size_t), &(cl->dev[dev].max_image_width),  NULL);
    (cl->dlocl->symbols->dt_clGetDeviceInfo)(devid, CL_DEVICE_MAX_MEM_ALLOC_SIZE,  sizeof(cl_ulong), &(cl->dev[dev].max_mem_alloc),  NULL);
    if(!image_support)
    {
      dt_print(DT_DEBUG_OPENCL, "[opencl_init] discarding device %d `%s' due to missing image support.\n", k, infostr);
      continue;
    }

    (cl->dlocl->symbols->dt_clGetDeviceInfo)(devid, CL_DEVICE_GLOBAL_MEM_SIZE, sizeof(cl_ulong), &(cl->dev[dev].max_global_mem), NULL);
    if(cl->dev[dev].max_global_mem < 256*1024*1024)
    {
      dt_print(DT_DEBUG_OPENCL, "[opencl_init] discarding device %d `%s' due to insufficient global memory (%luMB).\n", k, infostr, cl->dev[dev].max_global_mem/1024/1024);
      continue;
    }

    dt_print(DT_DEBUG_OPENCL, "[opencl_init] device %d `%s' supports image sizes of %zd x %zd\n", k, infostr, cl->dev[dev].max_image_width, cl->dev[dev].max_image_height);
    dt_print(DT_DEBUG_OPENCL, "[opencl_init] device %d `%s' allows GPU memory allocations of up to %luMB\n", k, infostr, cl->dev[dev].max_mem_alloc/1024/1024);

    if(darktable.unmuted & DT_DEBUG_OPENCL)
    {
      printf("[opencl_init] device %d: %s \n", k, infostr);
      (cl->dlocl->symbols->dt_clGetDeviceInfo)(devid, CL_DEVICE_MAX_WORK_GROUP_SIZE, sizeof(infoint), &infoint, NULL);
      printf("     MAX_WORK_GROUP_SIZE:      %zd\n", infoint);
      (cl->dlocl->symbols->dt_clGetDeviceInfo)(devid, CL_DEVICE_MAX_WORK_ITEM_DIMENSIONS, sizeof(infoint), &infoint, NULL);
      printf("     MAX_WORK_ITEM_DIMENSIONS: %zd\n", infoint);
      printf("     MAX_WORK_ITEM_SIZES:      [ ");
      (cl->dlocl->symbols->dt_clGetDeviceInfo)(devid, CL_DEVICE_MAX_WORK_ITEM_SIZES, sizeof(infointtab), infointtab, NULL);
      for (int i=0; i<infoint; i++) printf("%zd ", infointtab[i]);
      printf("]\n");
    }
    dt_pthread_mutex_init(&cl->dev[dev].lock, NULL);

    cl->dev[dev].context = (cl->dlocl->symbols->dt_clCreateContext)(0, 1, &devid, NULL, NULL, &err);
    if(err != CL_SUCCESS)
    {
      dt_print(DT_DEBUG_OPENCL, "[opencl_init] could not create context for device %d: %d\n", k, err);
      return;
    }
    // create a command queue for first device the context reported
    cl->dev[dev].cmd_queue = (cl->dlocl->symbols->dt_clCreateCommandQueue)(cl->dev[dev].context, devid, (darktable.unmuted & DT_DEBUG_PERF) ? CL_QUEUE_PROFILING_ENABLE : 0, &err);
    if(err != CL_SUCCESS)
    {
      dt_print(DT_DEBUG_OPENCL, "[opencl_init] could not create command queue for device %d: %d\n", k, err);
      return;
    }
    char dtpath[1024], filename[1024], programname[1024];
    dt_get_datadir(dtpath, 1024);
    snprintf(filename, 1024, "%s/kernels/programs.conf", dtpath);
    // now load all darktable cl kernels.
    // TODO: compile as a job?
    FILE *f = fopen(filename, "rb");
    if(f)
    {
      while(!feof(f))
      {
        int rd = fscanf(f, "%[^\n]\n", programname);
        if(rd != 1) continue;
        // remove comments:
        for(int k=0; k<strlen(programname); k++) if(programname[k] == '#')
          {
            programname[k] = '\0';
            while(programname[--k] == ' ') programname[k] = '\0';
            break;
          }
        if(programname[0] == '\0') continue;
        snprintf(filename, 1024, "%s/kernels/%s", dtpath, programname);
        dt_print(DT_DEBUG_OPENCL, "[opencl_init] compiling program `%s' ..\n", programname);
        const int prog = dt_opencl_load_program(k, filename);
        if(dt_opencl_build_program(k, prog))
        {
          dt_print(DT_DEBUG_OPENCL, "[opencl_init] failed to compile program `%s'!\n", programname);
          return;
        }
      }
      fclose(f);
    }
    else
    {
      dt_print(DT_DEBUG_OPENCL, "[opencl_init] could not open `%s'!\n", filename);
      return;
    }
    ++dev;
  }
  free(devices);
  if(dev > 0)
  {
    dt_print(DT_DEBUG_OPENCL, "[opencl_init] successfully initialized.\n");
    dt_print(DT_DEBUG_OPENCL, "[opencl_init] initial status of opencl enabled flag is %s.\n", prefs ? "ON" : "OFF");
    cl->num_devs = dev;
    cl->inited = 1;
    cl->enabled = prefs;
    // set preferences to saved state
    dt_conf_set_bool("opencl", prefs);
  }
  else
  {
    dt_print(DT_DEBUG_OPENCL, "[opencl_init] no suitable devices found.\n");
  }
  return;
}

void dt_opencl_cleanup(dt_opencl_t *cl)
{
  if(cl->inited) for(int i=0; i<cl->num_devs; i++)
    {
      dt_pthread_mutex_destroy(&cl->dev[i].lock);
      for(int k=0; k<DT_OPENCL_MAX_KERNELS; k++) if(cl->dev[i].kernel_used [k]) (cl->dlocl->symbols->dt_clReleaseKernel) (cl->dev[i].kernel [k]);
      for(int k=0; k<DT_OPENCL_MAX_PROGRAMS; k++) if(cl->dev[i].program_used[k]) (cl->dlocl->symbols->dt_clReleaseProgram)(cl->dev[i].program[k]);
      (cl->dlocl->symbols->dt_clReleaseCommandQueue)(cl->dev[i].cmd_queue);
      (cl->dlocl->symbols->dt_clReleaseContext)(cl->dev[i].context);
      dt_opencl_events_reset(i);
      if(cl->dev[i].eventlist) free(cl->dev[i].eventlist);
      if(cl->dev[i].eventtags) free(cl->dev[i].eventtags);
    }

  if(cl->dlocl) {
    free(cl->dlocl->symbols);
    free(cl->dlocl);
  }

  dt_pthread_mutex_destroy(&cl->lock);
}

int dt_opencl_finish(const int devid)
{
  dt_opencl_t *cl = darktable.opencl;
  if(!cl->inited || devid < 0) return -1;

  cl_int err = (cl->dlocl->symbols->dt_clFinish)(cl->dev[devid].cmd_queue);

  // take the opportunity to release some event handles, but without
  // sumary statistics
  dt_opencl_events_flush(devid, 0);

  return err;
}

int dt_opencl_enqueue_barrier(const int devid)
{
  dt_opencl_t *cl = darktable.opencl;
  if(!cl->inited || devid < 0) return -1;
  return (cl->dlocl->symbols->dt_clEnqueueBarrier)(cl->dev[devid].cmd_queue);
}

int dt_opencl_lock_device(const int _dev)
{
  dt_opencl_t *cl = darktable.opencl;
  if(!cl->inited) return -1;
  int dev = _dev;
  if(dev < 0 || dev >= cl->num_devs) dev = 0;
  for(int i=0; i<cl->num_devs; i++)
  {
    // start at argument and get first currently unused processor
    const int try_dev = (dev + i) % cl->num_devs;
    if(!dt_pthread_mutex_trylock(&cl->dev[try_dev].lock)) return try_dev;
  }
  // no free GPU :(
  // use CPU processing, if no free device:
  if(!dt_pthread_mutex_trylock(&cl->dev[dev].lock)) return dev;
  return -1;
}

void dt_opencl_unlock_device(const int dev)
{
  dt_opencl_t *cl = darktable.opencl;
  if(!cl->inited) return;
  if(dev < 0 || dev >= cl->num_devs) return;
  dt_pthread_mutex_unlock(&cl->dev[dev].lock);
}

int dt_opencl_load_program(const int dev, const char *filename)
{
  cl_int err;
  dt_opencl_t *cl = darktable.opencl;
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
  for(int k=0; k<filesize; k++) if(file[k] == '\n') lines++;
  const char *sptr[lines+1];
  size_t lengths[lines];
  int curr = 0;
  sptr[curr++] = file;
  for(int k=0; k<filesize; k++)
    if(file[k] == '\n')
    {
      sptr[curr] = file + k + 1;
      lengths[curr-1] = sptr[curr] - sptr[curr-1];
      curr++;
    }
  lengths[lines-1] = file + filesize - sptr[lines-1];
  sptr[lines] = NULL;
  int k = 0;
  for(; k<DT_OPENCL_MAX_PROGRAMS; k++) if(!cl->dev[dev].program_used[k])
    {
      cl->dev[dev].program_used[k] = 1;
      cl->dev[dev].program[k] = (cl->dlocl->symbols->dt_clCreateProgramWithSource)(cl->dev[dev].context, lines, sptr, lengths, &err);
      if(err != CL_SUCCESS)
      {
        dt_print(DT_DEBUG_OPENCL, "[opencl_load_program] could not create program from file `%s'! (%d)\n", filename, err);
        cl->dev[dev].program_used[k] = 0;
        return -1;
      }
      else break;
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

int dt_opencl_build_program(const int dev, const int prog)
{
  if(prog < 0 || prog >= DT_OPENCL_MAX_PROGRAMS) return -1;
  dt_opencl_t *cl = darktable.opencl;
  cl_program program = cl->dev[dev].program[prog];
  cl_int err;
  err = (cl->dlocl->symbols->dt_clBuildProgram)(program, 1, &cl->dev[dev].devid, "-cl-fast-relaxed-math -cl-strict-aliasing", 0, 0);
  if(err != CL_SUCCESS)
  {
    dt_print(DT_DEBUG_OPENCL, "[opencl_build_program] could not build program: %d\n", err);
    cl_build_status build_status;
    (cl->dlocl->symbols->dt_clGetProgramBuildInfo)(program, cl->dev[dev].devid, CL_PROGRAM_BUILD_STATUS, sizeof(cl_build_status), &build_status, NULL);
    if (build_status != CL_SUCCESS)
    {
      char *build_log;
      size_t ret_val_size;
      (cl->dlocl->symbols->dt_clGetProgramBuildInfo)(program, cl->dev[dev].devid, CL_PROGRAM_BUILD_LOG, 0, NULL, &ret_val_size);
      build_log = (char *)malloc(sizeof(char)*(ret_val_size+1));
      (cl->dlocl->symbols->dt_clGetProgramBuildInfo)(program, cl->dev[dev].devid, CL_PROGRAM_BUILD_LOG, ret_val_size, build_log, NULL);

      build_log[ret_val_size] = '\0';

      dt_print(DT_DEBUG_OPENCL, "BUILD LOG:\n");
      dt_print(DT_DEBUG_OPENCL, "%s\n", build_log);

      free(build_log);
    }
  }
  else
  {
    dt_print(DT_DEBUG_OPENCL, "[opencl_build_program] successfully built program\n");
  }
  return err;
}

int dt_opencl_create_kernel(const int prog, const char *name)
{
  dt_opencl_t *cl = darktable.opencl;
  if(!cl->inited) return -1;
  if(prog < 0 || prog >= DT_OPENCL_MAX_PROGRAMS) return -1;
  dt_pthread_mutex_lock(&cl->lock);
  int k = 0;
  for(int dev=0; dev<cl->num_devs; dev++)
  {
    cl_int err;
    for(; k<DT_OPENCL_MAX_KERNELS; k++) if(!cl->dev[dev].kernel_used[k])
      {
        cl->dev[dev].kernel_used[k] = 1;
        cl->dev[dev].kernel[k] = (cl->dlocl->symbols->dt_clCreateKernel)(cl->dev[dev].program[prog], name, &err);
        if(err != CL_SUCCESS)
        {
          dt_print(DT_DEBUG_OPENCL, "[opencl_create_kernel] could not create kernel `%s'! (%d)\n", name, err);
          cl->dev[dev].kernel_used[k] = 0;
          goto error;
        }
        else break;
      }
    if(k < DT_OPENCL_MAX_KERNELS)
    {
      dt_print(DT_DEBUG_OPENCL, "[opencl_create_kernel] successfully loaded kernel `%s' (%d) for device %d\n", name, k, dev);
    }
    else
    {
      dt_print(DT_DEBUG_OPENCL, "[opencl_create_kernel] too many kernels! can't create kernel `%s'\n", name);
      goto error;
    }
  }
  dt_pthread_mutex_unlock(&cl->lock);
  return k;
error:
  dt_pthread_mutex_unlock(&cl->lock);
  return -1;
}

void dt_opencl_free_kernel(const int kernel)
{
  dt_opencl_t *cl = darktable.opencl;
  if(!cl->inited) return;
  if(kernel < 0 || kernel >= DT_OPENCL_MAX_KERNELS) return;
  dt_pthread_mutex_lock(&cl->lock);
  for(int dev=0; dev<cl->num_devs; dev++)
  {
    cl->dev[dev].kernel_used [kernel] = 0;
    (cl->dlocl->symbols->dt_clReleaseKernel) (cl->dev[dev].kernel [kernel]);
  }
  dt_pthread_mutex_unlock(&cl->lock);
}

int dt_opencl_get_max_work_item_sizes(const int dev, size_t *sizes)
{
  dt_opencl_t *cl = darktable.opencl;
  if(!cl->inited || dev < 0) return -1;
  return (cl->dlocl->symbols->dt_clGetDeviceInfo)(cl->dev[dev].devid, CL_DEVICE_MAX_WORK_ITEM_SIZES, sizeof(size_t)*3, sizes, NULL);
}

int dt_opencl_set_kernel_arg(const int dev, const int kernel, const int num, const size_t size, const void *arg)
{
  dt_opencl_t *cl = darktable.opencl;
  if(!cl->inited || dev < 0) return -1;
  if(kernel < 0 || kernel >= DT_OPENCL_MAX_KERNELS) return -1;
  return (cl->dlocl->symbols->dt_clSetKernelArg)(cl->dev[dev].kernel[kernel], num, size, arg);
}

int dt_opencl_enqueue_kernel_2d(const int dev, const int kernel, const size_t *sizes)
{
  dt_opencl_t *cl = darktable.opencl;
  if(!cl->inited || dev < 0) return -1;
  if(kernel < 0 || kernel >= DT_OPENCL_MAX_KERNELS) return -1;
  int err;
  char buf[256];
  buf[0]='\0';
  (cl->dlocl->symbols->dt_clGetKernelInfo)(cl->dev[dev].kernel[kernel], CL_KERNEL_FUNCTION_NAME, 256, buf, NULL);
  cl_event *eventp = dt_opencl_events_get_slot(dev, buf);
  // const size_t local[2] = {16, 16};
  // let the driver choose:
  const size_t *local = NULL;
  err = (cl->dlocl->symbols->dt_clEnqueueNDRangeKernel)(cl->dev[dev].cmd_queue, cl->dev[dev].kernel[kernel], 2, NULL, sizes, local, 0, NULL, eventp);
  // if (err == CL_SUCCESS) err = dt_opencl_finish(dev);
  return err;
}


int dt_opencl_copy_device_to_host(const int devid, void *host, void *device, const int width, const int height, const int bpp)
{
  return dt_opencl_read_host_from_device(devid, host, device, width, height, bpp);
}

int dt_opencl_read_host_from_device(const int devid, void *host, void *device, const int width, const int height, const int bpp)
{
  return dt_opencl_read_host_from_device_rowpitch(devid, host, device, width, height, bpp*width);
}

int dt_opencl_read_host_from_device_rowpitch(const int devid, void *host, void *device, const int width, const int height, const int rowpitch)
{
  if(!darktable.opencl->inited || devid < 0) return -1;
  const size_t origin[] = {0, 0, 0};
  const size_t region[] = {width, height, 1};
  // blocking.
  return dt_opencl_read_host_from_device_raw(devid, host, device, origin, region, rowpitch, CL_TRUE);
}

int dt_opencl_read_host_from_device_raw(const int devid, void *host, void *device, const size_t *origin, const size_t *region, const int rowpitch, const int blocking)
{
  if(!darktable.opencl->inited) return -1;

  cl_event *eventp = dt_opencl_events_get_slot(devid, "[Read Image (from device to host)]");

  return (darktable.opencl->dlocl->symbols->dt_clEnqueueReadImage)(darktable.opencl->dev[devid].cmd_queue, device, blocking, origin, region, rowpitch, 0, host, 0, NULL, eventp);
}

int dt_opencl_write_host_to_device(const int devid, void *host, void *device, const int width, const int height, const int bpp)
{
  return dt_opencl_write_host_to_device_rowpitch(devid, host, device, width, height, width*bpp);
}

int dt_opencl_write_host_to_device_rowpitch(const int devid, void *host, void *device, const int width, const int height, const int rowpitch)
{
  if(!darktable.opencl->inited || devid < 0) return -1;
  const size_t origin[] = {0, 0, 0};
  const size_t region[] = {width, height, 1};
  // blocking.
  return dt_opencl_write_host_to_device_raw(devid, host, device, origin, region, rowpitch, CL_TRUE);
}

int dt_opencl_write_host_to_device_raw(const int devid, void *host, void *device, const size_t *origin, const size_t *region, const int rowpitch, const int blocking)
{
  if(!darktable.opencl->inited) return -1;

  cl_event *eventp = dt_opencl_events_get_slot(devid, "[Write Image (from host to device)]");

  return (darktable.opencl->dlocl->symbols->dt_clEnqueueWriteImage)(darktable.opencl->dev[devid].cmd_queue, device, blocking, origin, region, rowpitch, 0, host, 0, NULL, eventp);
}

int dt_opencl_enqueue_copy_image(const int devid, cl_mem src, cl_mem dst, size_t *orig_src, size_t *orig_dst, size_t *region)
{
  if(!darktable.opencl->inited || devid < 0) return -1;
  cl_int err;
  cl_event *eventp = dt_opencl_events_get_slot(devid, "[Copy Image (on device)]");
  err = (darktable.opencl->dlocl->symbols->dt_clEnqueueCopyImage)(darktable.opencl->dev[devid].cmd_queue, src, dst, orig_src, orig_dst, region, 0, NULL, eventp);
  if(err != CL_SUCCESS) dt_print(DT_DEBUG_OPENCL, "[opencl copy_image] could not copy image: %d\n", err);
  return err;
}

int dt_opencl_enqueue_copy_image_to_buffer(const int devid, cl_mem src_image, cl_mem dst_buffer, size_t *origin, size_t *region, size_t offset)
{
  if(!darktable.opencl->inited) return -1;
  cl_int err;
  cl_event *eventp = dt_opencl_events_get_slot(devid, "[Copy Image (on device)]");
  err = (darktable.opencl->dlocl->symbols->dt_clEnqueueCopyImageToBuffer)(darktable.opencl->dev[devid].cmd_queue, src_image, dst_buffer, origin, region, offset, 0, NULL, eventp);
  if(err != CL_SUCCESS) dt_print(DT_DEBUG_OPENCL, "[opencl copy_image_to_buffer] could not copy image: %d\n", err);
  return err;
}

int dt_opencl_enqueue_copy_buffer_to_image(const int devid, cl_mem src_buffer, cl_mem dst_image, size_t offset, size_t *origin, size_t *region)
{
  if(!darktable.opencl->inited) return -1;
  cl_int err;
  cl_event *eventp = dt_opencl_events_get_slot(devid, "[Copy Image (on device)]");
  err = (darktable.opencl->dlocl->symbols->dt_clEnqueueCopyBufferToImage)(darktable.opencl->dev[devid].cmd_queue, src_buffer, dst_image, offset, origin, region, 0, NULL, eventp);
  if(err != CL_SUCCESS) dt_print(DT_DEBUG_OPENCL, "[opencl copy_buffer_to_image] could not copy buffer: %d\n", err);
  return err;
}

void* dt_opencl_copy_host_to_device_constant(const int devid, const int size, void *host)
{
  if(!darktable.opencl->inited || devid < 0) return NULL;
  cl_int err;
  cl_mem dev = (darktable.opencl->dlocl->symbols->dt_clCreateBuffer) (darktable.opencl->dev[devid].context,
                               CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR,
                               size,
                               host, &err);
  if(err != CL_SUCCESS) dt_print(DT_DEBUG_OPENCL, "[opencl copy_host_to_device_constant] could not alloc buffer on device %d: %d\n", devid, err);
  return dev;
}

void* dt_opencl_copy_host_to_device(const int devid, void *host, const int width, const int height, const int bpp)
{
  return dt_opencl_copy_host_to_device_rowpitch(devid, host, width, height, bpp, 0);
}

void* dt_opencl_copy_host_to_device_rowpitch(const int devid, void *host, const int width, const int height, const int bpp, const int rowpitch)
{
  if(!darktable.opencl->inited || devid < 0) return NULL;
  cl_int err;
  cl_image_format fmt;
  // guess pixel format from bytes per pixel
  if(bpp == 4*sizeof(float))
    fmt = (cl_image_format)
  {
    CL_RGBA, CL_FLOAT
  };
  else if(bpp == sizeof(float))
    fmt = (cl_image_format)
  {
    CL_R, CL_FLOAT
  };
  else if(bpp == sizeof(uint16_t))
    fmt = (cl_image_format)
  {
    CL_R, CL_UNSIGNED_INT16
  };
  else return NULL;

  // TODO: if fmt = uint16_t, blow up to 4xuint16_t and copy manually!
  cl_mem dev = (darktable.opencl->dlocl->symbols->dt_clCreateImage2D) (darktable.opencl->dev[devid].context,
                                CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR,
                                &fmt,
                                width, height, rowpitch,
                                host, &err);
  if(err != CL_SUCCESS) dt_print(DT_DEBUG_OPENCL, "[opencl copy_host_to_device] could not alloc/copy img buffer on device %d: %d\n", devid, err);
  return dev;
}


void dt_opencl_release_mem_object(void *mem)
{
  if (!darktable.opencl->inited) return;
  (darktable.opencl->dlocl->symbols->dt_clReleaseMemObject)(mem);
}


void* dt_opencl_alloc_device(const int devid, const int width, const int height, const int bpp)
{
  if(!darktable.opencl->inited || devid < 0) return NULL;
  cl_int err;
  cl_image_format fmt;
  // guess pixel format from bytes per pixel
  if(bpp == 4*sizeof(float))
    fmt = (cl_image_format)
  {
    CL_RGBA, CL_FLOAT
  };
  else if(bpp == sizeof(float))
    fmt = (cl_image_format)
  {
    CL_R, CL_FLOAT
  };
  else if(bpp == sizeof(uint16_t))
    fmt = (cl_image_format)
  {
    CL_R, CL_UNSIGNED_INT16
  };
  else return NULL;

  cl_mem dev = (darktable.opencl->dlocl->symbols->dt_clCreateImage2D) (darktable.opencl->dev[devid].context,
                                CL_MEM_READ_WRITE,
                                &fmt,
                                width, height, 0,
                                NULL, &err);
  if(err != CL_SUCCESS) dt_print(DT_DEBUG_OPENCL, "[opencl alloc_device] could not alloc img buffer on device %d: %d\n", devid, err);
  return dev;
}


void* dt_opencl_alloc_device_use_host_pointer(const int devid, const int width, const int height, const int bpp, const int rowpitch, void *host)
{
  if(!darktable.opencl->inited || devid < 0) return NULL;
  cl_int err;
  cl_image_format fmt;
  // guess pixel format from bytes per pixel
  if(bpp == 4*sizeof(float))
    fmt = (cl_image_format)
  {
    CL_RGBA, CL_FLOAT
  };
  else if(bpp == sizeof(float))
    fmt = (cl_image_format)
  {
    CL_R, CL_FLOAT
  };
  else if(bpp == sizeof(uint16_t))
    fmt = (cl_image_format)
  {
    CL_R, CL_UNSIGNED_INT16
  };
  else return NULL;

  cl_mem dev = (darktable.opencl->dlocl->symbols->dt_clCreateImage2D) (darktable.opencl->dev[devid].context,
                                CL_MEM_READ_WRITE | ((host == NULL) ? CL_MEM_ALLOC_HOST_PTR : CL_MEM_USE_HOST_PTR),
                                &fmt,
                                width, height, rowpitch,
                                host, &err);
  if(err != CL_SUCCESS) dt_print(DT_DEBUG_OPENCL, "[opencl alloc_device_use_host_pointer] could not alloc img buffer on device %d: %d\n", devid, err);
  return dev;
}


void* dt_opencl_alloc_device_buffer(const int devid, const int size)
{
  if(!darktable.opencl->inited) return NULL;
  cl_int err;

  cl_mem buf = (darktable.opencl->dlocl->symbols->dt_clCreateBuffer) (darktable.opencl->dev[devid].context,
                               CL_MEM_READ_WRITE,
                               size,
                               NULL, &err);
  if(err != CL_SUCCESS) dt_print(DT_DEBUG_OPENCL, "[opencl alloc_device_buffer] could not alloc buffer on device %d: %d\n", devid, err);
  return buf;
}


/** check if image size fit into limits given by OpenCL runtime */
int dt_opencl_image_fits_device(const int devid, const size_t width, const size_t height, const size_t bytes)
{
  if(!darktable.opencl->inited || devid < 0) return FALSE;

  if(darktable.opencl->dev[devid].max_image_width < width || darktable.opencl->dev[devid].max_image_height < height) return FALSE;

  return darktable.opencl->dev[devid].max_global_mem >= bytes + DT_OPENCL_MEMORY_HEADROOM;
}


/** check if opencl is inited */
int dt_opencl_is_inited(void)
{
  return darktable.opencl->inited;
}


/** check if opencl is enabled */
int dt_opencl_is_enabled(void)
{
  if(!darktable.opencl->inited) return FALSE;
  return darktable.opencl->enabled;
}


/** disable opencl */
void dt_opencl_disable(void)
{
  if(!darktable.opencl->inited) return;
  darktable.opencl->enabled = FALSE;
  dt_conf_set_bool("opencl", FALSE);
}


/** update enabled flag with value from preferences */
int dt_opencl_update_enabled(void)
{
  if(!darktable.opencl->inited) return FALSE;
  const int prefs = dt_conf_get_bool("opencl");

  //printf("[opencl_update_enabled] preferences is set to %d\n", prefs);

  if (darktable.opencl->enabled != prefs)
  {
    darktable.opencl->enabled = prefs;
    dt_print(DT_DEBUG_OPENCL, "[opencl_update_enabled] enabled flag set to %s\n", prefs ? "ON" : "OFF");    
  }
  return darktable.opencl->enabled;
}

/** get global memory of device */
cl_ulong dt_opencl_get_max_global_mem(const int devid)
{
  if(!darktable.opencl->inited || devid < 0) return 0;
  return darktable.opencl->dev[devid].max_global_mem;
}


/** the following eventlist functions assume that affected structures are locked upstream */

/** get next free slot in eventlist (and manage size of eventlist) */
cl_event *dt_opencl_events_get_slot(const int devid, const char *tag)
{
  dt_opencl_t *cl = darktable.opencl;
  if(!cl->inited || devid < 0) return NULL;

  static const cl_event zeroevent[1];   // implicitly initialized to zero
  cl_event **eventlist = &(cl->dev[devid].eventlist);
  dt_opencl_eventtag_t **eventtags = &(cl->dev[devid].eventtags);
  int *numevents = &(cl->dev[devid].numevents);
  int *maxevents = &(cl->dev[devid].maxevents);

  // if first time called: allocate initial buffers
  if (*eventlist == NULL)
  {
    int newevents = DT_OPENCL_EVENTLISTSIZE;
    *eventlist = malloc(newevents*sizeof(cl_event));
    *eventtags = malloc(newevents*sizeof(dt_opencl_eventtag_t));
    if (!*eventlist || !*eventtags)
    {
      free(*eventlist);
      free(*eventtags);
      *eventlist=NULL;
      *eventtags=NULL;
      return NULL;
    }
    memset(*eventtags, 0, newevents*sizeof(dt_opencl_eventtag_t));
    *maxevents = newevents;
  }

  // check if currently highest event slot was actually consumed. If not use it again
  if (*numevents > 0 && !memcmp((*eventlist)+*numevents-1, zeroevent, sizeof(cl_event)))
  {
    if (tag != NULL)
    {
      strncpy((*eventtags)[*numevents-1].tag, tag, DT_OPENCL_EVENTNAMELENGTH);
    }
    else
    {
      (*eventtags)[*numevents-1].tag[0]='\0';
    }
    return (*eventlist)+*numevents-1;
  }

  // if no more space left in eventlist: grow buffer		
  if (*numevents == *maxevents)
  {
    int newevents = *maxevents + DT_OPENCL_EVENTLISTSIZE;
    cl_event *neweventlist = malloc(newevents*sizeof(cl_event));
    dt_opencl_eventtag_t *neweventtags = malloc(newevents*sizeof(dt_opencl_eventtag_t));
    if (!neweventlist || !neweventtags)
    {
      free(neweventlist);
      free(neweventtags);
      return NULL;
    }
    memset(neweventtags, 0, newevents*sizeof(dt_opencl_eventtag_t));
    memcpy(neweventlist, *eventlist, *maxevents*sizeof(cl_event));
    memcpy(neweventtags, *eventtags, *maxevents*sizeof(dt_opencl_eventtag_t));	
    free(*eventlist);
    free(*eventtags);
    *eventlist = neweventlist;
    *eventtags = neweventtags;
    *maxevents = newevents;
  }

  // init next event slot and return it
  (*numevents)++;
  memcpy((*eventlist)+*numevents-1, zeroevent, sizeof(cl_event));
  if (tag != NULL)
  {
    strncpy((*eventtags)[*numevents-1].tag, tag, DT_OPENCL_EVENTNAMELENGTH);
  }
  else
  {
    (*eventtags)[*numevents-1].tag[0]='\0';
  }
  return (*eventlist)+*numevents-1;
}


/** reset eventlist to empty state */
void dt_opencl_events_reset(const int devid)
{
  dt_opencl_t *cl = darktable.opencl;
  if(!cl->inited || devid < 0) return;

  cl_event **eventlist = &(cl->dev[devid].eventlist);
  dt_opencl_eventtag_t **eventtags = &(cl->dev[devid].eventtags);
  int *numevents = &(cl->dev[devid].numevents);
  int *maxevents = &(cl->dev[devid].maxevents);
  int *eventsconsolidated = &(cl->dev[devid].eventsconsolidated);
  cl_int *summary = &(cl->dev[devid].summary);

  if (*eventlist == NULL || *numevents == 0) return; // nothing to do

  // release all remaining events in eventlist, not to waste resources
  for (int k = *eventsconsolidated; k < *numevents; k++)
  {
    (cl->dlocl->symbols->dt_clReleaseEvent)((*eventlist)[k]);
  }

  memset(*eventtags, 0, *maxevents*sizeof(dt_opencl_eventtag_t));
  *numevents=0;
  *eventsconsolidated=0;
  *summary=CL_COMPLETE;
  return;
}


/** Wait for events in eventlist to terminate -> this is a blocking synchronization point!
    Does not flush eventlist. Side effect: might adjust numevents. */
void dt_opencl_events_wait_for(const int devid)
{
  dt_opencl_t *cl = darktable.opencl;
  if(!cl->inited || devid < 0) return;

  static const cl_event zeroevent[1];   // implicitly initialized to zero
  cl_event **eventlist = &(cl->dev[devid].eventlist);
  int *numevents = &(cl->dev[devid].numevents);
  int *eventsconsolidated = &(cl->dev[devid].eventsconsolidated);

  if (*eventlist==NULL || *numevents==0) return; // nothing to do

  // check if last event slot was acutally used and correct numevents if needed	
  if (!memcmp((*eventlist)+*numevents-1, zeroevent, sizeof(cl_event))) (*numevents)--;

  if (*numevents == *eventsconsolidated) return; // nothing to do

  assert(*numevents > *eventsconsolidated);

  // now wait for all remaining events to terminate
  // Risk: might never return in case of OpenCL blocks or endless loops
  // TODO: run clWaitForEvents in separate thread and implement watchdog timer
  (cl->dlocl->symbols->dt_clWaitForEvents)(*numevents - *eventsconsolidated, (*eventlist)+*eventsconsolidated);

  return;
}


/** Wait for events in eventlist to terminate, check for return status and profiling
info of events.
If "reset" is TRUE report summary info (would be CL_COMPLETE or last error code) and
print profiling info if needed.
If "reset" is FALSE just store info (success value, profiling) from terminated events
and release events for re-use by OpenCL driver. */
cl_int dt_opencl_events_flush(const int devid, const int reset)
{
  dt_opencl_t *cl = darktable.opencl;
  if(!cl->inited || devid < 0) return FALSE;

  cl_event **eventlist = &(cl->dev[devid].eventlist);
  dt_opencl_eventtag_t **eventtags = &(cl->dev[devid].eventtags);
  int *numevents = &(cl->dev[devid].numevents);
  int *eventsconsolidated = &(cl->dev[devid].eventsconsolidated);
  cl_int *summary = &(cl->dev[devid].summary);

  if (*eventlist==NULL || *numevents==0) return CL_COMPLETE; // nothing to do, no news is good news

  // Wait for command queue to terminate (side effect: might adjust *numevents)
  dt_opencl_events_wait_for(devid);

  // now check return status and profiling data of all newly terminated events	
  for (int k = *eventsconsolidated; k < *numevents; k++)
  {
    cl_int err;
    char *tag = (*eventtags)[k].tag;
    cl_int *retval = &((*eventtags)[k].retval);

    // get return value of event
    err = (cl->dlocl->symbols->dt_clGetEventInfo)((*eventlist)[k], CL_EVENT_COMMAND_EXECUTION_STATUS, sizeof(cl_int), retval, NULL);
    if (err != CL_SUCCESS)
    {
      dt_print(DT_DEBUG_OPENCL, "[opencl_events_flush] could not get event info for '%s': %d\n", tag[0] == '\0' ? "<?>" : tag, err);
    }
    else if (*retval != CL_COMPLETE)
    {
      dt_print(DT_DEBUG_OPENCL, "[opencl_events_flush] execution of '%s' %s: %d\n", tag[0] == '\0' ? "<?>" : tag, *retval == CL_COMPLETE ? "was successful" : "failed", *retval);
      *summary=*retval;
    }

    // get profiling info of event
    cl_ulong start;
    cl_ulong end;
    cl_int errs = (cl->dlocl->symbols->dt_clGetEventProfilingInfo)((*eventlist)[k], CL_PROFILING_COMMAND_START, sizeof(cl_ulong), &start, NULL);
    cl_int erre = (cl->dlocl->symbols->dt_clGetEventProfilingInfo)((*eventlist)[k], CL_PROFILING_COMMAND_END, sizeof(cl_ulong), &end, NULL);
    if (errs == CL_SUCCESS && erre == CL_SUCCESS) (*eventtags)[k].timelapsed = end - start;

    // finally release event to be re-used by driver
    (cl->dlocl->symbols->dt_clReleaseEvent)((*eventlist)[k]);
    (*eventsconsolidated)++;
  }

  cl_int result = *summary;

  // do we want to get rid of all stored info?
  if(reset)
  {
    // output profiling info if wanted
    if (darktable.unmuted & DT_DEBUG_PERF)
        dt_opencl_events_profiling(devid, 1);

    // reset eventlist structures to empty state
    dt_opencl_events_reset(devid);
  }

  return result == CL_COMPLETE ? 0 : result;
}


/** display OpenCL profiling information. If "aggregated" is TRUE, try to generate summarized info for each kernel */
void dt_opencl_events_profiling(const int devid, const int aggregated)
{
  dt_opencl_t *cl = darktable.opencl;
  if(!cl->inited || devid < 0) return;

  cl_event **eventlist = &(cl->dev[devid].eventlist);
  dt_opencl_eventtag_t **eventtags = &(cl->dev[devid].eventtags);
  int *numevents = &(cl->dev[devid].numevents);
  int *eventsconsolidated = &(cl->dev[devid].eventsconsolidated);

  if (*eventlist == NULL || *numevents == 0 ||
      *eventtags == NULL || *eventsconsolidated == 0) return; // nothing to do

  char *tags[*eventsconsolidated+1];
  float timings[*eventsconsolidated+1];
  int items = 1;
  tags[0] = "";
  timings[0] = 0.0f;

  // get profiling info and arrange it
  for (int k = 0; k < *eventsconsolidated; k++)
  {
    // if aggregated is TRUE, try to sum up timings for multiple runs of each kernel
    if (aggregated)
    {
      // linear search: this is not efficient at all but acceptable given the limited
      // number of events (ca. 10 - 20)
      int tagfound = -1;
      for (int i=0; i<items; i++)
      {
        if (!strncmp(tags[i], (*eventtags)[k].tag, DT_OPENCL_EVENTNAMELENGTH))
        {
          tagfound=i;
          break;
        }
      }

      if (tagfound >= 0) // tag was already detected before
      {
        // sum up timings
        timings[tagfound] += (*eventtags)[k].timelapsed * 1e-9;
      }
      else // tag is new
      {
        // make new entry
	items++;
        tags[items-1] = (*eventtags)[k].tag;
        timings[items-1] = (*eventtags)[k].timelapsed * 1e-9;
      }
    }

    else // no aggregated info wanted -> arrange event by event
    {
      items++;
      tags[items-1] = (*eventtags)[k].tag;
      timings[items-1] = (*eventtags)[k].timelapsed * 1e-9;
    }
  }

  // now display profiling info
  float total = 0.0f;
  for (int i=1; i<items; i++)
  {
    dt_print(DT_DEBUG_OPENCL, "[opencl_profiling] spent %7.4f seconds in %s\n", (double)timings[i], tags[i][0] == '\0' ? "<?>" : tags[i]);
    total += timings[i];
  }
  // aggregated timing info for items without tag (if any)
  if (timings[0] != 0.0f)
  {		
    dt_print(DT_DEBUG_OPENCL, "[opencl_profiling] spent %7.4f seconds (unallocated)\n", (double)timings[0]);
    total += timings[0];
  }

  if (total != 0.0f);
  {
    dt_print(DT_DEBUG_OPENCL, "[opencl_profiling] spent %7.4f seconds totally in command queue\n", (double)total);
  }
  return;
}

#endif

