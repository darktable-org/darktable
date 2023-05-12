/*
    This file is part of darktable,
    Copyright (C) 2011-2023 darktable developers.

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

#include "common/dynload.h"
#include "common/dlopencl.h"
#include "common/darktable.h"

#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(WIN32)
static const char *ocllib[] = { "OpenCL.dll", NULL };
#elif defined(__APPLE__)
static const char *ocllib[] = { "/System/Library/Frameworks/OpenCL.framework/Versions/Current/OpenCL", NULL };
#else
static const char *ocllib[] = { "libOpenCL", "libOpenCL.so", "libOpenCL.so.1", NULL };
#endif


/* only for debugging: default noop function for all unassigned function pointers */
void dt_dlopencl_noop(void)
{
  /* we should normally never get here */
  dt_print(DT_DEBUG_ALWAYS, "dt_dlopencl internal error: unsupported function call\n");
  raise(SIGABRT);
}


/* dynamically load OpenCL library and bind needed symbols */
dt_dlopencl_t *dt_dlopencl_init(const char *name)
{
  dt_gmodule_t *module = NULL;
  dt_dlopencl_t *ocl = NULL;
  const char *library = NULL;

  /* check if our platform supports gmodules */
  gboolean success = dt_gmodule_supported();
  if(!success) return NULL;

  /* try to load library. if a name is given check only that library - else iterate over default names. */
  if(name != NULL && name[0] != '\0')
  {
    library = name;
    module = dt_gmodule_open(library);
    if(module == NULL)
      dt_print(DT_DEBUG_OPENCL, "[dt_dlopencl_init] could not find specified opencl runtime library '%s'\n", library);
    else
      dt_print(DT_DEBUG_OPENCL | DT_DEBUG_VERBOSE, "[dt_dlopencl_init] found specified opencl runtime library '%s'\n", library);
  }
  else
  {
    const char **iter = ocllib;
    while(*iter && (module == NULL))
    {
      library = *iter;
      module = dt_gmodule_open(library);
      if(module == NULL)
        dt_print(DT_DEBUG_OPENCL, "[dt_dlopencl_init] could not find default opencl runtime library '%s'\n", library);
      else
        dt_print(DT_DEBUG_OPENCL | DT_DEBUG_VERBOSE, "[dt_dlopencl_init] found default opencl runtime library '%s'\n", library);
      iter++;
    }
  }

  if(module == NULL)
    return NULL;

  /* now bind symbols */
  ocl = (dt_dlopencl_t *)malloc(sizeof(dt_dlopencl_t));

  if(ocl == NULL)
  {
    free(module);
    return NULL;
  }

  ocl->symbols = (dt_dlopencl_symbols_t *)calloc(1, sizeof(dt_dlopencl_symbols_t));

  if(ocl->symbols == NULL)
  {
    free(ocl);
    free(module);
    return NULL;
  }

  ocl->library = module->library;

  /* assign noop function as a default to each function pointer */
  void (**slist)(void) = (void (**)(void))ocl->symbols;

  success = FALSE;

  /* sanity check against padding */
  if(sizeof(dt_dlopencl_symbols_t) % sizeof(void (*)(void)) == 0)
  {
    for(int k = 0; k < sizeof(dt_dlopencl_symbols_t) / sizeof(void (*)(void)); k++)
      slist[k] = dt_dlopencl_noop;

    success = TRUE;

    /* only bind needed symbols */
    success = success && dt_gmodule_symbol(module, "clGetPlatformIDs",
                                           (void (**)(void)) & ocl->symbols->dt_clGetPlatformIDs);
    success = success && dt_gmodule_symbol(module, "clGetPlatformInfo",
                                           (void (**)(void)) & ocl->symbols->dt_clGetPlatformInfo);
    success = success && dt_gmodule_symbol(module, "clGetDeviceIDs",
                                           (void (**)(void)) & ocl->symbols->dt_clGetDeviceIDs);
    success = success && dt_gmodule_symbol(module, "clGetDeviceInfo",
                                           (void (**)(void)) & ocl->symbols->dt_clGetDeviceInfo);
    success = success && dt_gmodule_symbol(module, "clCreateContext",
                                           (void (**)(void)) & ocl->symbols->dt_clCreateContext);
    success = success && dt_gmodule_symbol(module, "clCreateCommandQueue",
                                           (void (**)(void)) & ocl->symbols->dt_clCreateCommandQueue);
    success = success && dt_gmodule_symbol(module, "clCreateProgramWithSource",
                                           (void (**)(void)) & ocl->symbols->dt_clCreateProgramWithSource);
    success = success && dt_gmodule_symbol(module, "clBuildProgram",
                                           (void (**)(void)) & ocl->symbols->dt_clBuildProgram);
    success = success && dt_gmodule_symbol(module, "clGetProgramBuildInfo",
                                           (void (**)(void)) & ocl->symbols->dt_clGetProgramBuildInfo);
    success = success && dt_gmodule_symbol(module, "clCreateKernel",
                                           (void (**)(void)) & ocl->symbols->dt_clCreateKernel);
    success = success && dt_gmodule_symbol(module, "clCreateBuffer",
                                           (void (**)(void)) & ocl->symbols->dt_clCreateBuffer);
    success = success && dt_gmodule_symbol(module, "clCreateImage2D",
                                           (void (**)(void)) & ocl->symbols->dt_clCreateImage2D);
    success = success && dt_gmodule_symbol(module, "clEnqueueWriteBuffer",
                                           (void (**)(void)) & ocl->symbols->dt_clEnqueueWriteBuffer);
    success = success && dt_gmodule_symbol(module, "clSetKernelArg",
                                           (void (**)(void)) & ocl->symbols->dt_clSetKernelArg);
    success = success && dt_gmodule_symbol(module, "clGetKernelWorkGroupInfo",
                                           (void (**)(void)) & ocl->symbols->dt_clGetKernelWorkGroupInfo);
    success = success && dt_gmodule_symbol(module, "clEnqueueNDRangeKernel",
                                           (void (**)(void)) & ocl->symbols->dt_clEnqueueNDRangeKernel);
    success = success && dt_gmodule_symbol(module, "clEnqueueReadImage",
                                           (void (**)(void)) & ocl->symbols->dt_clEnqueueReadImage);
    success = success && dt_gmodule_symbol(module, "clEnqueueWriteImage",
                                           (void (**)(void)) & ocl->symbols->dt_clEnqueueWriteImage);
    success = success && dt_gmodule_symbol(module, "clEnqueueCopyImage",
                                           (void (**)(void)) & ocl->symbols->dt_clEnqueueCopyImage);
    success = success && dt_gmodule_symbol(module, "clEnqueueCopyImageToBuffer",
                                           (void (**)(void)) & ocl->symbols->dt_clEnqueueCopyImageToBuffer);
    success = success && dt_gmodule_symbol(module, "clEnqueueCopyBufferToImage",
                                           (void (**)(void)) & ocl->symbols->dt_clEnqueueCopyBufferToImage);
    success = success && dt_gmodule_symbol(module, "clFinish", (void (**)(void)) & ocl->symbols->dt_clFinish);
    success = success && dt_gmodule_symbol(module, "clEnqueueReadBuffer",
                                           (void (**)(void)) & ocl->symbols->dt_clEnqueueReadBuffer);
    success = success && dt_gmodule_symbol(module, "clReleaseMemObject",
                                           (void (**)(void)) & ocl->symbols->dt_clReleaseMemObject);
    success = success && dt_gmodule_symbol(module, "clReleaseProgram",
                                           (void (**)(void)) & ocl->symbols->dt_clReleaseProgram);
    success = success && dt_gmodule_symbol(module, "clReleaseKernel",
                                           (void (**)(void)) & ocl->symbols->dt_clReleaseKernel);
    success = success && dt_gmodule_symbol(module, "clReleaseCommandQueue",
                                           (void (**)(void)) & ocl->symbols->dt_clReleaseCommandQueue);
    success = success && dt_gmodule_symbol(module, "clReleaseContext",
                                           (void (**)(void)) & ocl->symbols->dt_clReleaseContext);
    success = success && dt_gmodule_symbol(module, "clReleaseEvent",
                                           (void (**)(void)) & ocl->symbols->dt_clReleaseEvent);
    success = success && dt_gmodule_symbol(module, "clWaitForEvents",
                                           (void (**)(void)) & ocl->symbols->dt_clWaitForEvents);
    success = success && dt_gmodule_symbol(module, "clGetEventInfo",
                                           (void (**)(void)) & ocl->symbols->dt_clGetEventInfo);
    success = success && dt_gmodule_symbol(module, "clGetEventProfilingInfo",
                                           (void (**)(void)) & ocl->symbols->dt_clGetEventProfilingInfo);
    success = success && dt_gmodule_symbol(module, "clGetKernelInfo",
                                           (void (**)(void)) & ocl->symbols->dt_clGetKernelInfo);
    success = success && dt_gmodule_symbol(module, "clEnqueueBarrier",
                                           (void (**)(void)) & ocl->symbols->dt_clEnqueueBarrier);
    success = success && dt_gmodule_symbol(module, "clGetKernelWorkGroupInfo",
                                           (void (**)(void)) & ocl->symbols->dt_clGetKernelWorkGroupInfo);
    success = success && dt_gmodule_symbol(module, "clEnqueueReadBuffer",
                                           (void (**)(void)) & ocl->symbols->dt_clEnqueueReadBuffer);
    success = success && dt_gmodule_symbol(module, "clEnqueueWriteBuffer",
                                           (void (**)(void)) & ocl->symbols->dt_clEnqueueWriteBuffer);
    success = success && dt_gmodule_symbol(module, "clGetProgramInfo",
                                           (void (**)(void)) & ocl->symbols->dt_clGetProgramInfo);
    success = success && dt_gmodule_symbol(module, "clCreateProgramWithBinary",
                                           (void (**)(void)) & ocl->symbols->dt_clCreateProgramWithBinary);
    success = success && dt_gmodule_symbol(module, "clEnqueueCopyBuffer",
                                           (void (**)(void)) & ocl->symbols->dt_clEnqueueCopyBuffer);
    success = success && dt_gmodule_symbol(module, "clEnqueueMapBuffer",
                                           (void (**)(void)) & ocl->symbols->dt_clEnqueueMapBuffer);
    success = success && dt_gmodule_symbol(module, "clEnqueueUnmapMemObject",
                                           (void (**)(void)) & ocl->symbols->dt_clEnqueueUnmapMemObject);
    success = success && dt_gmodule_symbol(module, "clGetMemObjectInfo",
                                           (void (**)(void)) & ocl->symbols->dt_clGetMemObjectInfo);
    success = success && dt_gmodule_symbol(module, "clGetImageInfo",
                                           ((void (**)(void)) & ocl->symbols->dt_clGetImageInfo));
  }

  ocl->have_opencl = success;

  if(!success)
    dt_print(DT_DEBUG_OPENCL, "[opencl_init] could not load all required symbols from library\n");

  free(module);

  if(!success)
  {
    free(ocl->symbols);
    free(ocl);
    return NULL;
  }

  return ocl;
}

#endif

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

