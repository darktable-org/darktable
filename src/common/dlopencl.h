/*
    This file is part of darktable,
    Copyright (C) 2011-2020 darktable developers.

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

#ifdef HAVE_OPENCL

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <CL/cl.h>

typedef cl_int (*dt_clGetPlatformIDs_t)(cl_uint, cl_platform_id *, cl_uint *);
typedef cl_int (*dt_clGetPlatformInfo_t)(cl_platform_id, cl_platform_info, size_t, void *, size_t *);
typedef cl_int (*dt_clGetDeviceIDs_t)(cl_platform_id, cl_device_type, cl_uint, cl_device_id *, cl_uint *);
typedef cl_int (*dt_clGetDeviceInfo_t)(cl_device_id, cl_device_info, size_t, void *, size_t *);
typedef cl_context (*dt_clCreateContext_t)(const cl_context_properties *, cl_uint, const cl_device_id *,
                                           void (*)(const char *, const void *, size_t, void *), void *,
                                           cl_int *);
typedef cl_context (*dt_clCreateContextFromType_t)(const cl_context_properties *, cl_device_type,
                                                   void (*)(const char *, const void *, size_t, void *),
                                                   void *, cl_int *);
typedef cl_int (*dt_clRetainContext_t)(cl_context);
typedef cl_int (*dt_clReleaseContext_t)(cl_context);
typedef cl_int (*dt_clGetContextInfo_t)(cl_context, cl_context_info, size_t, void *, size_t *);
typedef cl_command_queue (*dt_clCreateCommandQueue_t)(cl_context, cl_device_id, cl_command_queue_properties,
                                                      cl_int *);
typedef cl_int (*dt_clRetainCommandQueue_t)(cl_command_queue);
typedef cl_int (*dt_clReleaseCommandQueue_t)(cl_command_queue);
typedef cl_int (*dt_clGetCommandQueueInfo_t)(cl_command_queue, cl_command_queue_info, size_t, void *,
                                             size_t *);
typedef cl_int (*dt_clSetCommandQueueProperty_t)(cl_command_queue, cl_command_queue_properties, cl_bool,
                                                 cl_command_queue_properties *);
typedef cl_mem (*dt_clCreateBuffer_t)(cl_context, cl_mem_flags, size_t, void *, cl_int *);
typedef cl_mem (*dt_clCreateSubBuffer_t)(cl_mem, cl_mem_flags, cl_buffer_create_type, const void *, cl_int *);
typedef cl_mem (*dt_clCreateImage2D_t)(cl_context, cl_mem_flags, const cl_image_format *, size_t, size_t,
                                       size_t, void *, cl_int *);
typedef cl_mem (*dt_clCreateImage3D_t)(cl_context, cl_mem_flags, const cl_image_format *, size_t, size_t,
                                       size_t, size_t, size_t, void *, cl_int *);
typedef cl_int (*dt_clRetainMemObject_t)(cl_mem);
typedef cl_int (*dt_clReleaseMemObject_t)(cl_mem);
typedef cl_int (*dt_clGetSupportedImageFormats_t)(cl_context, cl_mem_flags, cl_mem_object_type, cl_uint,
                                                  cl_image_format *, cl_uint *);
typedef cl_int (*dt_clGetMemObjectInfo_t)(cl_mem, cl_mem_info, size_t, void *, size_t *);
typedef cl_int (*dt_clGetImageInfo_t)(cl_mem, cl_image_info, size_t, void *, size_t *);
typedef cl_int (*dt_clSetMemObjectDestructorCallback_t)(cl_mem, void(*), void *);
typedef cl_sampler (*dt_clCreateSampler_t)(cl_context, cl_bool, cl_addressing_mode, cl_filter_mode, cl_int *);
typedef cl_int (*dt_clRetainSampler_t)(cl_sampler);
typedef cl_int (*dt_clReleaseSampler_t)(cl_sampler);
typedef cl_int (*dt_clGetSamplerInfo_t)(cl_sampler, cl_sampler_info, size_t, void *, size_t *);
typedef cl_program (*dt_clCreateProgramWithSource_t)(cl_context, cl_uint, const char **, const size_t *,
                                                     cl_int *);
typedef cl_program (*dt_clCreateProgramWithBinary_t)(cl_context, cl_uint, const cl_device_id *,
                                                     const size_t *, const unsigned char **, cl_int *,
                                                     cl_int *);
typedef cl_int (*dt_clRetainProgram_t)(cl_program);
typedef cl_int (*dt_clReleaseProgram_t)(cl_program);
typedef cl_int (*dt_clBuildProgram_t)(cl_program, cl_uint, const cl_device_id *, const char *, void(*),
                                      void *);
typedef cl_int (*dt_clUnloadCompiler_t)(void);
typedef cl_int (*dt_clGetProgramInfo_t)(cl_program, cl_program_info, size_t, void *, size_t *);
typedef cl_int (*dt_clGetProgramBuildInfo_t)(cl_program, cl_device_id, cl_program_build_info, size_t, void *,
                                             size_t *);
typedef cl_kernel (*dt_clCreateKernel_t)(cl_program, const char *, cl_int *);
typedef cl_int (*dt_clCreateKernelsInProgram_t)(cl_program, cl_uint, cl_kernel *, cl_uint *);
typedef cl_int (*dt_clRetainKernel_t)(cl_kernel);
typedef cl_int (*dt_clReleaseKernel_t)(cl_kernel);
typedef cl_int (*dt_clSetKernelArg_t)(cl_kernel, cl_uint, size_t, const void *);
typedef cl_int (*dt_clGetKernelInfo_t)(cl_kernel, cl_kernel_info, size_t, void *, size_t *);
typedef cl_int (*dt_clGetKernelWorkGroupInfo_t)(cl_kernel, cl_device_id, cl_kernel_work_group_info, size_t,
                                                void *, size_t *);
typedef cl_int (*dt_clWaitForEvents_t)(cl_uint, const cl_event *);
typedef cl_int (*dt_clGetEventInfo_t)(cl_event, cl_event_info, size_t, void *, size_t *);
typedef cl_event (*dt_clCreateUserEvent_t)(cl_context, cl_int *);
typedef cl_int (*dt_clRetainEvent_t)(cl_event);
typedef cl_int (*dt_clReleaseEvent_t)(cl_event);
typedef cl_int (*dt_clSetUserEventStatus_t)(cl_event, cl_int);
typedef cl_int (*dt_clSetEventCallback_t)(cl_event, cl_int, void (*)(cl_event, cl_int, void *), void *);
typedef cl_int (*dt_clGetEventProfilingInfo_t)(cl_event, cl_profiling_info, size_t, void *, size_t *);
typedef cl_int (*dt_clFlush_t)(cl_command_queue);
typedef cl_int (*dt_clFinish_t)(cl_command_queue);
typedef cl_int (*dt_clEnqueueReadBuffer_t)(cl_command_queue, cl_mem, cl_bool, size_t, size_t, void *, cl_uint,
                                           const cl_event *, cl_event *);
typedef cl_int (*dt_clEnqueueReadBufferRect_t)(cl_command_queue, cl_mem, cl_bool, const size_t *,
                                               const size_t *, const size_t *, size_t, size_t, size_t, size_t,
                                               void *, cl_uint, const cl_event *, cl_event *);
typedef cl_int (*dt_clEnqueueWriteBuffer_t)(cl_command_queue, cl_mem, cl_bool, size_t, size_t, const void *,
                                            cl_uint, const cl_event *, cl_event *);
typedef cl_int (*dt_clEnqueueWriteBufferRect_t)(cl_command_queue, cl_mem, cl_bool, const size_t *,
                                                const size_t *, const size_t *, size_t, size_t, size_t,
                                                size_t, const void *, cl_uint, const cl_event *, cl_event *);
typedef cl_int (*dt_clEnqueueCopyBuffer_t)(cl_command_queue, cl_mem, cl_mem, size_t, size_t, size_t, cl_uint,
                                           const cl_event *, cl_event *);
typedef cl_int (*dt_clEnqueueCopyBufferRect_t)(cl_command_queue, cl_mem, cl_mem, const size_t *,
                                               const size_t *, const size_t *, size_t, size_t, size_t, size_t,
                                               cl_uint, const cl_event *, cl_event *);
typedef cl_int (*dt_clEnqueueReadImage_t)(cl_command_queue, cl_mem, cl_bool, const size_t *, const size_t *,
                                          size_t, size_t, void *, cl_uint, const cl_event *, cl_event *);
typedef cl_int (*dt_clEnqueueWriteImage_t)(cl_command_queue, cl_mem, cl_bool, const size_t *, const size_t *,
                                           size_t, size_t, const void *, cl_uint, const cl_event *,
                                           cl_event *);
typedef cl_int (*dt_clEnqueueCopyImage_t)(cl_command_queue, cl_mem, cl_mem, const size_t *, const size_t *,
                                          const size_t *, cl_uint, const cl_event *, cl_event *);
typedef cl_int (*dt_clEnqueueCopyImageToBuffer_t)(cl_command_queue, cl_mem, cl_mem, const size_t *,
                                                  const size_t *, size_t, cl_uint, const cl_event *,
                                                  cl_event *);
typedef cl_int (*dt_clEnqueueCopyBufferToImage_t)(cl_command_queue, cl_mem, cl_mem, size_t, const size_t *,
                                                  const size_t *, cl_uint, const cl_event *, cl_event *);
typedef void *(*dt_clEnqueueMapBuffer_t)(cl_command_queue, cl_mem, cl_bool, cl_map_flags, size_t, size_t,
                                         cl_uint, const cl_event *, cl_event *, cl_int *);
typedef void *(*dt_clEnqueueMapImage_t)(cl_command_queue, cl_mem, cl_bool, cl_map_flags, const size_t *,
                                        const size_t *, size_t *, size_t *, cl_uint, const cl_event *,
                                        cl_event *, cl_int *);
typedef cl_int (*dt_clEnqueueUnmapMemObject_t)(cl_command_queue, cl_mem, void *, cl_uint, const cl_event *,
                                               cl_event *);
typedef cl_int (*dt_clEnqueueNDRangeKernel_t)(cl_command_queue, cl_kernel, cl_uint, const size_t *,
                                              const size_t *, const size_t *, cl_uint, const cl_event *,
                                              cl_event *);
typedef cl_int (*dt_clEnqueueTask_t)(cl_command_queue, cl_kernel, cl_uint, const cl_event *, cl_event *);
typedef cl_int (*dt_clEnqueueNativeKernel_t)(cl_command_queue, void (*user_func)(void *), void *, size_t,
                                             cl_uint, const cl_mem *, const void **, cl_uint,
                                             const cl_event *, cl_event *);
typedef cl_int (*dt_clEnqueueMarker_t)(cl_command_queue, cl_event *);
typedef cl_int (*dt_clEnqueueWaitForEvents_t)(cl_command_queue, cl_uint, const cl_event *);
typedef cl_int (*dt_clEnqueueBarrier_t)(cl_command_queue);

typedef struct dt_dlopencl_symbols_t
{
  dt_clGetPlatformIDs_t dt_clGetPlatformIDs;
  dt_clGetPlatformInfo_t dt_clGetPlatformInfo;
  dt_clGetDeviceIDs_t dt_clGetDeviceIDs;
  dt_clGetDeviceInfo_t dt_clGetDeviceInfo;
  dt_clCreateContext_t dt_clCreateContext;
  dt_clCreateContextFromType_t dt_clCreateContextFromType;
  dt_clRetainContext_t dt_clRetainContext;
  dt_clReleaseContext_t dt_clReleaseContext;
  dt_clGetContextInfo_t dt_clGetContextInfo;
  dt_clCreateCommandQueue_t dt_clCreateCommandQueue;
  dt_clRetainCommandQueue_t dt_clRetainCommandQueue;
  dt_clReleaseCommandQueue_t dt_clReleaseCommandQueue;
  dt_clGetCommandQueueInfo_t dt_clGetCommandQueueInfo;
  dt_clSetCommandQueueProperty_t dt_clSetCommandQueueProperty;
  dt_clCreateBuffer_t dt_clCreateBuffer;
  dt_clCreateSubBuffer_t dt_clCreateSubBuffer;
  dt_clCreateImage2D_t dt_clCreateImage2D;
  dt_clCreateImage3D_t dt_clCreateImage3D;
  dt_clRetainMemObject_t dt_clRetainMemObject;
  dt_clReleaseMemObject_t dt_clReleaseMemObject;
  dt_clGetSupportedImageFormats_t dt_clGetSupportedImageFormats;
  dt_clGetMemObjectInfo_t dt_clGetMemObjectInfo;
  dt_clGetImageInfo_t dt_clGetImageInfo;
  dt_clSetMemObjectDestructorCallback_t dt_clSetMemObjectDestructorCallback;
  dt_clCreateSampler_t dt_clCreateSampler;
  dt_clRetainSampler_t dt_clRetainSampler;
  dt_clReleaseSampler_t dt_clReleaseSampler;
  dt_clGetSamplerInfo_t dt_clGetSamplerInfo;
  dt_clCreateProgramWithSource_t dt_clCreateProgramWithSource;
  dt_clCreateProgramWithBinary_t dt_clCreateProgramWithBinary;
  dt_clRetainProgram_t dt_clRetainProgram;
  dt_clReleaseProgram_t dt_clReleaseProgram;
  dt_clBuildProgram_t dt_clBuildProgram;
  dt_clUnloadCompiler_t dt_clUnloadCompiler;
  dt_clGetProgramInfo_t dt_clGetProgramInfo;
  dt_clGetProgramBuildInfo_t dt_clGetProgramBuildInfo;
  dt_clCreateKernel_t dt_clCreateKernel;
  dt_clCreateKernelsInProgram_t dt_clCreateKernelsInProgram;
  dt_clRetainKernel_t dt_clRetainKernel;
  dt_clReleaseKernel_t dt_clReleaseKernel;
  dt_clSetKernelArg_t dt_clSetKernelArg;
  dt_clGetKernelInfo_t dt_clGetKernelInfo;
  dt_clGetKernelWorkGroupInfo_t dt_clGetKernelWorkGroupInfo;
  dt_clWaitForEvents_t dt_clWaitForEvents;
  dt_clGetEventInfo_t dt_clGetEventInfo;
  dt_clCreateUserEvent_t dt_clCreateUserEvent;
  dt_clRetainEvent_t dt_clRetainEvent;
  dt_clReleaseEvent_t dt_clReleaseEvent;
  dt_clSetUserEventStatus_t dt_clSetUserEventStatus;
  dt_clSetEventCallback_t dt_clSetEventCallback;
  dt_clGetEventProfilingInfo_t dt_clGetEventProfilingInfo;
  dt_clFlush_t dt_clFlush;
  dt_clFinish_t dt_clFinish;
  dt_clEnqueueReadBuffer_t dt_clEnqueueReadBuffer;
  dt_clEnqueueReadBufferRect_t dt_clEnqueueReadBufferRect;
  dt_clEnqueueWriteBuffer_t dt_clEnqueueWriteBuffer;
  dt_clEnqueueWriteBufferRect_t dt_clEnqueueWriteBufferRect;
  dt_clEnqueueCopyBuffer_t dt_clEnqueueCopyBuffer;
  dt_clEnqueueCopyBufferRect_t dt_clEnqueueCopyBufferRect;
  dt_clEnqueueReadImage_t dt_clEnqueueReadImage;
  dt_clEnqueueWriteImage_t dt_clEnqueueWriteImage;
  dt_clEnqueueCopyImage_t dt_clEnqueueCopyImage;
  dt_clEnqueueCopyImageToBuffer_t dt_clEnqueueCopyImageToBuffer;
  dt_clEnqueueCopyBufferToImage_t dt_clEnqueueCopyBufferToImage;
  dt_clEnqueueMapBuffer_t dt_clEnqueueMapBuffer;
  dt_clEnqueueMapImage_t dt_clEnqueueMapImage;
  dt_clEnqueueUnmapMemObject_t dt_clEnqueueUnmapMemObject;
  dt_clEnqueueNDRangeKernel_t dt_clEnqueueNDRangeKernel;
  dt_clEnqueueTask_t dt_clEnqueueTask;
  dt_clEnqueueNativeKernel_t dt_clEnqueueNativeKernel;
  dt_clEnqueueMarker_t dt_clEnqueueMarker;
  dt_clEnqueueWaitForEvents_t dt_clEnqueueWaitForEvents;
  dt_clEnqueueBarrier_t dt_clEnqueueBarrier;
} dt_dlopencl_symbols_t;



typedef struct dt_dlopencl_t
{
  int have_opencl;
  dt_dlopencl_symbols_t *symbols;
  char *library;
} dt_dlopencl_t;

/* default noop function for all unassigned function pointers */
void dt_dlopencl_noop(void);

/* dynamically load OpenCL library and bind needed functions */
dt_dlopencl_t *dt_dlopencl_init(const char *);

#endif // HAVE_OPENCL

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
