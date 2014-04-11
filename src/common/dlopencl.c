/*
    This file is part of darktable,
    copyright (c) 2011 Ulrich Pegelow

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

#include "common/darktable.h"
#include "common/dynload.h"
#include "common/dlopencl.h"

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>


/* only for debugging: default noop function for all unassigned function pointers */
void dt_dlopencl_noop(void)
{
  /* we should normally never get here */
  fprintf(stderr, "dt_dlopencl error: unsupported function call\n");
  assert(FALSE);
}


/* dynamically load OpenCL library and bind needed symbols */
int dt_dlopencl_init(const char *name, dt_dlopencl_t **ocl)
{
  dt_gmodule_t *module = NULL;
  dt_dlopencl_t *d;
  const char *library;
  int success;
  int k;


  /* check if our platform supports gmodules */
  success = dt_gmodule_supported();
  if (!success)
  {
    *ocl = NULL;
    return FALSE;
  }

  /* try to load library */
  library = (name == NULL || name[0] == '\0') ? DT_OPENCL_LIBRARY : name;
  module = dt_gmodule_open(library);

  if (module == NULL)
  {
    dt_print(DT_DEBUG_OPENCL, "[opencl_init] could not find opencl runtime library '%s'\n", library);
    *ocl = NULL;
    return FALSE;
  }
  else
  {
    /* now bind symbols */
    success = TRUE;
    d = (dt_dlopencl_t *)malloc(sizeof(dt_dlopencl_t));

    if (d == NULL)
    {
      free(module);
      *ocl = NULL;
      return FALSE;
    }

    d->symbols = (dt_dlopencl_symbols_t *)malloc(sizeof(dt_dlopencl_symbols_t));

    if (d->symbols == NULL)
    {
      free(d);
      free(module);
      *ocl = NULL;
      return FALSE;
    }

    memset(d->symbols, 0, sizeof(dt_dlopencl_symbols_t));
    d->library = module->library;

    /* assign noop function as a default to each function pointer */
    void (** slist)(void) = (void (**)(void))d->symbols;
    /* sanity check against padding */
    if (sizeof(dt_dlopencl_symbols_t) % sizeof(void (*)(void)) == 0)
      for (k=0; k < sizeof(dt_dlopencl_symbols_t)/sizeof(void (*)(void)); k++) slist[k] = dt_dlopencl_noop;

    /* only bind needed symbols */
    success = success && dt_gmodule_symbol(module, "clGetPlatformIDs", (void (**)(void))&d->symbols->dt_clGetPlatformIDs);
    success = success && dt_gmodule_symbol(module, "clGetPlatformInfo", (void (**)(void))&d->symbols->dt_clGetPlatformInfo);
    success = success && dt_gmodule_symbol(module, "clGetDeviceIDs", (void (**)(void))&d->symbols->dt_clGetDeviceIDs);
    success = success && dt_gmodule_symbol(module, "clGetDeviceInfo", (void (**)(void))&d->symbols->dt_clGetDeviceInfo);
    success = success && dt_gmodule_symbol(module, "clCreateContext", (void (**)(void))&d->symbols->dt_clCreateContext);
    success = success && dt_gmodule_symbol(module, "clCreateCommandQueue", (void (**)(void))&d->symbols->dt_clCreateCommandQueue);
    success = success && dt_gmodule_symbol(module, "clCreateProgramWithSource", (void (**)(void))&d->symbols->dt_clCreateProgramWithSource);
    success = success && dt_gmodule_symbol(module, "clBuildProgram", (void (**)(void))&d->symbols->dt_clBuildProgram);
    success = success && dt_gmodule_symbol(module, "clGetProgramBuildInfo", (void (**)(void))&d->symbols->dt_clGetProgramBuildInfo);
    success = success && dt_gmodule_symbol(module, "clCreateKernel", (void (**)(void))&d->symbols->dt_clCreateKernel);
    success = success && dt_gmodule_symbol(module, "clCreateBuffer", (void (**)(void))&d->symbols->dt_clCreateBuffer);
    success = success && dt_gmodule_symbol(module, "clCreateImage2D", (void (**)(void))&d->symbols->dt_clCreateImage2D);
    success = success && dt_gmodule_symbol(module, "clEnqueueWriteBuffer", (void (**)(void))&d->symbols->dt_clEnqueueWriteBuffer);
    success = success && dt_gmodule_symbol(module, "clSetKernelArg", (void (**)(void))&d->symbols->dt_clSetKernelArg);
    success = success && dt_gmodule_symbol(module, "clGetKernelWorkGroupInfo", (void (**)(void))&d->symbols->dt_clGetKernelWorkGroupInfo);
    success = success && dt_gmodule_symbol(module, "clEnqueueNDRangeKernel", (void (**)(void))&d->symbols->dt_clEnqueueNDRangeKernel);
    success = success && dt_gmodule_symbol(module, "clEnqueueReadImage", (void (**)(void))&d->symbols->dt_clEnqueueReadImage);
    success = success && dt_gmodule_symbol(module, "clEnqueueWriteImage", (void (**)(void))&d->symbols->dt_clEnqueueWriteImage);
    success = success && dt_gmodule_symbol(module, "clEnqueueCopyImage", (void (**)(void))&d->symbols->dt_clEnqueueCopyImage);
    success = success && dt_gmodule_symbol(module, "clEnqueueCopyImageToBuffer", (void (**)(void))&d->symbols->dt_clEnqueueCopyImageToBuffer);
    success = success && dt_gmodule_symbol(module, "clEnqueueCopyBufferToImage", (void (**)(void))&d->symbols->dt_clEnqueueCopyBufferToImage);
    success = success && dt_gmodule_symbol(module, "clFinish", (void (**)(void))&d->symbols->dt_clFinish);
    success = success && dt_gmodule_symbol(module, "clEnqueueReadBuffer", (void (**)(void))&d->symbols->dt_clEnqueueReadBuffer);
    success = success && dt_gmodule_symbol(module, "clReleaseMemObject", (void (**)(void))&d->symbols->dt_clReleaseMemObject);
    success = success && dt_gmodule_symbol(module, "clReleaseProgram", (void (**)(void))&d->symbols->dt_clReleaseProgram);
    success = success && dt_gmodule_symbol(module, "clReleaseKernel", (void (**)(void))&d->symbols->dt_clReleaseKernel);
    success = success && dt_gmodule_symbol(module, "clReleaseCommandQueue", (void (**)(void))&d->symbols->dt_clReleaseCommandQueue);
    success = success && dt_gmodule_symbol(module, "clReleaseContext", (void (**)(void))&d->symbols->dt_clReleaseContext);
    success = success && dt_gmodule_symbol(module, "clReleaseEvent", (void (**)(void))&d->symbols->dt_clReleaseEvent);
    success = success && dt_gmodule_symbol(module, "clWaitForEvents", (void (**)(void))&d->symbols->dt_clWaitForEvents);
    success = success && dt_gmodule_symbol(module, "clGetEventInfo", (void (**)(void))&d->symbols->dt_clGetEventInfo);
    success = success && dt_gmodule_symbol(module, "clGetEventProfilingInfo", (void (**)(void))&d->symbols->dt_clGetEventProfilingInfo);
    success = success && dt_gmodule_symbol(module, "clGetKernelInfo", (void (**)(void))&d->symbols->dt_clGetKernelInfo);
    success = success && dt_gmodule_symbol(module, "clEnqueueBarrier", (void (**)(void))&d->symbols->dt_clEnqueueBarrier);
    success = success && dt_gmodule_symbol(module, "clGetKernelWorkGroupInfo", (void (**)(void))&d->symbols->dt_clGetKernelWorkGroupInfo);
    success = success && dt_gmodule_symbol(module, "clEnqueueReadBuffer", (void (**)(void))&d->symbols->dt_clEnqueueReadBuffer);
    success = success && dt_gmodule_symbol(module, "clEnqueueWriteBuffer", (void (**)(void))&d->symbols->dt_clEnqueueWriteBuffer);
    success = success && dt_gmodule_symbol(module, "clGetProgramInfo", (void (**)(void))&d->symbols->dt_clGetProgramInfo);
    success = success && dt_gmodule_symbol(module, "clCreateProgramWithBinary", (void (**)(void))&d->symbols->dt_clCreateProgramWithBinary);
    success = success && dt_gmodule_symbol(module, "clEnqueueCopyBuffer", (void (**)(void))&d->symbols->dt_clEnqueueCopyBuffer);
    success = success && dt_gmodule_symbol(module, "clEnqueueMapBuffer", (void (**)(void))&d->symbols->dt_clEnqueueMapBuffer);
    success = success && dt_gmodule_symbol(module, "clEnqueueUnmapMemObject", (void (**)(void))&d->symbols->dt_clEnqueueUnmapMemObject);

    if (!success) dt_print(DT_DEBUG_OPENCL, "[opencl_init] could not load all required symbols from library\n");
    d->have_opencl = success;
    *ocl = success ? d : NULL;
  }

  free(module);

  if (success == FALSE)
  {
    free(d->symbols);
    free(d);
  }

  return success;
}

#endif

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
