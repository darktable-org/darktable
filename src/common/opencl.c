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

#ifdef HAVE_OPENCL

#include <strings.h>
#include <stdio.h>

#include "common/darktable.h"
#include "common/opencl.h"
#include "common/dlopencl.h"

void dt_opencl_init(dt_opencl_t *cl, const int argc, char *argv[])
{
  dt_pthread_mutex_init(&cl->lock, NULL);
  cl->inited = 0;
  int use_opencl = 0;
  for(int k=0; k<argc; k++) if(!strcmp(argv[k], "--enable-opencl"))
    {
      dt_print(DT_DEBUG_OPENCL, "[opencl_init] trying to find an opencl runtime on your system\n");
      use_opencl = 1;
    }

  if(!use_opencl) return;

  if(!dt_dlopencl_init(NULL, &cl->dlocl))
    {
      dt_print(DT_DEBUG_OPENCL, "[opencl_init] opencl library not found. Continue with opencl disabled\n");
      return;
    }
    else
    {
      dt_print(DT_DEBUG_OPENCL, "[opencl_init] opencl library %s found on your system and loaded\n", cl->dlocl->library);
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
  err = (cl->dlocl->symbols->dt_clGetDeviceIDs)(platform, CL_DEVICE_TYPE_GPU, 0, NULL, &num_devices);
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
    cl_device_id devid = cl->dev[dev].devid = devices[k];

    char infostr[1024];
    size_t infoint;
    size_t infointtab[1024];
    cl_bool image_support = 0;
    cl_ulong max_global_mem = 0;
    //size_t image_width = 0, image_height = 0;

    // test 1GB mem and image support:
    (cl->dlocl->symbols->dt_clGetDeviceInfo)(devid, CL_DEVICE_NAME, sizeof(infostr), &infostr, NULL);
    (cl->dlocl->symbols->dt_clGetDeviceInfo)(devid, CL_DEVICE_IMAGE_SUPPORT, sizeof(cl_bool), &image_support, NULL);
    (cl->dlocl->symbols->dt_clGetDeviceInfo)(devid, CL_DEVICE_IMAGE2D_MAX_HEIGHT, sizeof(size_t), &(cl->dev[dev].max_image_height), NULL);
    (cl->dlocl->symbols->dt_clGetDeviceInfo)(devid, CL_DEVICE_IMAGE2D_MAX_WIDTH,  sizeof(size_t), &(cl->dev[dev].max_image_width),  NULL);
    if(!image_support)
    {
      dt_print(DT_DEBUG_OPENCL, "[opencl_init] discarding device %d `%s' due to missing image support.\n", k, infostr);
      continue;
    }
    //if(image_height < 8192 || image_width < 8192)
    //{
    //  fprintf(stderr, "[opencl_init] WARNING: your card only supports image sizes of %zd x %zd\n", image_width, image_height);
    //  fprintf(stderr, "[opencl_init] WARNING: expect random crashes, especially with images larger than that.\n");
    //}

    (cl->dlocl->symbols->dt_clGetDeviceInfo)(devid, CL_DEVICE_GLOBAL_MEM_SIZE, sizeof(cl_ulong), &max_global_mem, NULL);
    if(max_global_mem < 1000000000ul)
    {
      dt_print(DT_DEBUG_OPENCL, "[opencl_init] discarding device %d `%s' due to insufficient global memory (%luMB).\n", k, infostr, max_global_mem/1024/1024);
      continue;
    }
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
    cl->dev[dev].cmd_queue = (cl->dlocl->symbols->dt_clCreateCommandQueue)(cl->dev[dev].context, devid, 0, &err);
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
        const int prog = dt_opencl_load_program(cl, k, filename);
        if(dt_opencl_build_program(cl, k, prog))
          dt_print(DT_DEBUG_OPENCL, "[opencl_init] failed to compile program `%s'!\n", programname);
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
    cl->num_devs = dev;
    cl->inited = 1;
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
    }
  dt_pthread_mutex_destroy(&cl->lock);
}

void dt_opencl_finish(cl_command_queue q)
{
  if(!darktable.opencl->inited) return;
  (darktable.opencl->dlocl->symbols->dt_clFinish)(q);
}

int dt_opencl_lock_device(dt_opencl_t *cl, const int _dev)
{
  if(!cl->inited) return -1;
  int dev = _dev;
  if(dev < 0 || dev >= cl->num_devs) dev = 0;
  for(int i=0; i<cl->num_devs; i++)
  {
    // start at argument and get first currently unused processor
    const int try_dev = (dev + i) % cl->num_devs;
    if(!dt_pthread_mutex_trylock(&cl->dev[try_dev].lock)) return try_dev;
  }
  // no free GPU :( lock the requested one, after blocking.
  dt_pthread_mutex_lock(&cl->dev[dev].lock);
  return dev;
}

void dt_opencl_unlock_device(dt_opencl_t *cl, const int dev)
{
  if(!cl->inited) return;
  if(dev < 0 || dev >= cl->num_devs) return;
  dt_pthread_mutex_unlock(&cl->dev[dev].lock);
}

int dt_opencl_load_program(dt_opencl_t *cl, const int dev, const char *filename)
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

int dt_opencl_build_program(dt_opencl_t *cl, const int dev, const int prog)
{
  if(prog < 0 || prog >= DT_OPENCL_MAX_PROGRAMS) return -1;
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
  dt_print(DT_DEBUG_OPENCL, "[opencl_build_program] successfully built program\n");
  return err;
}

int dt_opencl_create_kernel(dt_opencl_t *cl, const int prog, const char *name)
{
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

void dt_opencl_free_kernel(dt_opencl_t *cl, const int kernel)
{
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

int dt_opencl_get_max_work_item_sizes(dt_opencl_t *cl, const int dev, size_t *sizes)
{
  if(!cl->inited) return -1;
  return (cl->dlocl->symbols->dt_clGetDeviceInfo)(cl->dev[dev].devid, CL_DEVICE_MAX_WORK_ITEM_SIZES, sizeof(size_t)*3, sizes, NULL);
}

int dt_opencl_set_kernel_arg(dt_opencl_t *cl, const int dev, const int kernel, const int num, const size_t size, const void *arg)
{
  if(!cl->inited) return -1;
  if(kernel < 0 || kernel >= DT_OPENCL_MAX_KERNELS) return -1;
  return (cl->dlocl->symbols->dt_clSetKernelArg)(cl->dev[dev].kernel[kernel], num, size, arg);
}

int dt_opencl_enqueue_kernel_2d(dt_opencl_t *cl, const int dev, const int kernel, const size_t *sizes)
{
  if(!cl->inited) return -1;
  if(kernel < 0 || kernel >= DT_OPENCL_MAX_KERNELS) return -1;
  // const size_t local[2] = {16, 16};
  // let the driver choose:
  const size_t *local = NULL;
  return (cl->dlocl->symbols->dt_clEnqueueNDRangeKernel)(cl->dev[dev].cmd_queue, cl->dev[dev].kernel[kernel], 2, NULL, sizes, local, 0, NULL, NULL);
}

void dt_opencl_copy_device_to_host(void *host, void *device, const int width, const int height, const int devid, const int bpp)
{
  if(!darktable.opencl->inited) return;
  size_t origin[] = {0, 0, 0};
  size_t region[] = {width, height, 1};
  // blocking.
  (darktable.opencl->dlocl->symbols->dt_clEnqueueReadImage)(darktable.opencl->dev[devid].cmd_queue, device, CL_TRUE, origin, region, region[0]*bpp, 0, host, 0, NULL, NULL);
}

int dt_opencl_enqueue_copy_image(cl_command_queue q, cl_mem src, cl_mem dst, size_t *orig_src, size_t *orig_dst, size_t *region, int events, cl_event *wait, cl_event *event)
{
  if(!darktable.opencl->inited) return -1;
  cl_int err;
  err = (darktable.opencl->dlocl->symbols->dt_clEnqueueCopyImage)(q, src, dst, orig_src, orig_dst, region, events, wait, event);
  if(err != CL_SUCCESS) fprintf(stderr, "[opencl copy_image] could not copy image: %d\n", err);
  return err;
}

void* dt_opencl_copy_host_to_device_constant(const int size, const int devid, void *host)
{
  if(!darktable.opencl->inited) return NULL;
  cl_int err;
  cl_mem dev = (darktable.opencl->dlocl->symbols->dt_clCreateBuffer) (darktable.opencl->dev[devid].context,
                               CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR,
                               size,
                               host, &err);
  if(err != CL_SUCCESS) fprintf(stderr, "[opencl alloc_device] could not alloc img buffer on device %d: %d\n", devid, err);
  return dev;
}

void* dt_opencl_copy_host_to_device(void *host, const int width, const int height, const int devid, const int bpp)
{
  if(!darktable.opencl->inited) return NULL;
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
                                width, height, 0,
                                host, &err);
  if(err != CL_SUCCESS) fprintf(stderr, "[opencl copy_host_to_device] could not alloc/copy img buffer onto device %d: %d\n", devid, err);
  return dev;
}


void dt_opencl_release_mem_object(void *mem)
{
  if (!darktable.opencl->inited) return;
  (darktable.opencl->dlocl->symbols->dt_clReleaseMemObject)(mem);
}


void* dt_opencl_alloc_device(const int width, const int height, const int devid, const int bpp)
{
  if(!darktable.opencl->inited) return NULL;
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
  if(err != CL_SUCCESS) fprintf(stderr, "[opencl alloc_device] could not alloc img buffer on device %d: %d\n", devid, err);
  return dev;
}


int dt_opencl_image_fits_device(const int devid, const size_t width, const size_t height)
{
  if(!darktable.opencl->inited) return FALSE;

  return (darktable.opencl->dev[devid].max_image_width >= width && darktable.opencl->dev[devid].max_image_height >= height);
}

#endif

