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

#include "dt_metal.h"
#include "common/file_location.h"

#define NS_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#include "Metal.hpp"

#include <string.h>


/**
 * Create and store a Metal library for a device.
 * Also creates a command queue for later kernel dispatch.
 * Returns TRUE on success.
 */
static gboolean _dt_metal_create_library(NS::URL *url, dt_metal_device_t *dev, MTL::Device *pDevice)
{
  dt_print(DT_DEBUG_METAL,
          "[dt_metal_create_library] Create Metal library for device: %s",
          pDevice->name()->utf8String());

  NS::Error *libraryError = NULL;
  MTL::Library *library = pDevice->newLibrary(url, &libraryError);
  if(!library)
  {
    dt_print(DT_DEBUG_METAL,
            "[dt_metal_create_library] Could not create library: %s",
            libraryError ? libraryError->localizedDescription()->utf8String()
                         : "unknown error");
    return FALSE;
  }

  dt_print(DT_DEBUG_METAL, "[dt_metal_create_library] Library created");

  // list all kernel functions found in the library
  NS::Array *funcNames = library->functionNames();
  if(funcNames)
  {
    for(unsigned long fi = 0; fi < funcNames->count(); fi++)
    {
      const char *functionName = ((NS::String *)funcNames->object(fi))->utf8String();
      dt_print(DT_DEBUG_METAL,
              "[dt_metal_create_library] Function: %s",
              functionName);
    }
  }

  // create a command queue for this device
  MTL::CommandQueue *commandQueue = pDevice->newCommandQueue();
  if(!commandQueue)
  {
    dt_print(DT_DEBUG_METAL,
            "[dt_metal_create_library] Could not create command queue");
    library->release();
    return FALSE;
  }

  dev->library = (void *)library;
  dev->command_queue = (void *)commandQueue;

  return TRUE;
}


void dt_metal_init(dt_metal_t *metal)
{
  dt_print(DT_DEBUG_METAL,
           "[dt_metal_init] Initializing Metal devices");

  memset(metal->kernels, 0, sizeof(metal->kernels));
  metal->num_kernels = 0;

  // get the path to the metal library
  char metallibpath[PATH_MAX] = { 0 };
  dt_loc_get_sharedir(metallibpath, sizeof(metallibpath));
  g_strlcat(metallibpath, "/darktable/metal/darktable.metallib", sizeof(metallibpath));

  dt_print(DT_DEBUG_METAL,
           "[dt_metal_init] metallib path: %s",
           metallibpath);

  NS::URL *url = NS::URL::fileURLWithPath(NS::String::string(metallibpath,
                                          NS::StringEncoding::UTF8StringEncoding));

  // get all Metal devices
  NS::Array *devices = MTL::CopyAllDevices();
  if(!devices)
  {
    dt_print(DT_DEBUG_METAL,
             "[dt_metal_init] MTL::CopyAllDevices() returned NULL");
    metal->num_devs = 0;
    metal->dev = NULL;
    return;
  }

  const int num_devices = devices->count();

  if(num_devices)
  {
    metal->num_devs = num_devices;
    metal->dev = (dt_metal_device_t *)calloc(num_devices, sizeof(dt_metal_device_t));

    for(int i = 0; i < num_devices; i++)
    {
      MTL::Device *pDevice = (MTL::Device *)devices->object(i);
      const char *deviceName = pDevice->name()->utf8String();

      dt_print(DT_DEBUG_METAL,
              "[dt_metal_init] Device: %s",
              deviceName);

      dt_pthread_mutex_init(&metal->dev[i].lock, NULL);

      if(_dt_metal_create_library(url, &metal->dev[i], pDevice))
      {
        metal->dev[i].devid = pDevice->registryID();
        metal->dev[i].device = (void *)pDevice;
      }
    }
  }
  else
  {
    metal->num_devs = 0;
    metal->dev = NULL;
  }

  devices->release();
}


void dt_metal_cleanup(dt_metal_t *metal)
{
  if(!metal) return;

  // free all registered pipelines
  for(int k = 0; k < metal->num_kernels; k++)
  {
    if(metal->kernels[k])
    {
      ((MTL::ComputePipelineState *)metal->kernels[k])->release();
      metal->kernels[k] = NULL;
    }
  }
  metal->num_kernels = 0;

  // free per-device resources
  for(int i = 0; i < metal->num_devs; i++)
  {
    dt_metal_device_t *dev = &metal->dev[i];

    if(dev->command_queue)
    {
      ((MTL::CommandQueue *)dev->command_queue)->release();
      dev->command_queue = NULL;
    }
    if(dev->library)
    {
      ((MTL::Library *)dev->library)->release();
      dev->library = NULL;
    }
    // Note: we don't release dev->device because MTL::CopyAllDevices()
    // returns autoreleased objects managed by the Metal runtime.

    dt_pthread_mutex_destroy(&dev->lock);
  }

  free(metal->dev);
  metal->dev = NULL;
  metal->num_devs = 0;
}


void dt_metal_list_devices(dt_metal_t *metal)
{
  for(int i = 0; i < metal->num_devs; i++)
  {
    MTL::Device *pDevice = (MTL::Device *)metal->dev[i].device;
    if(!pDevice) continue;
    const char *deviceName = pDevice->name()->utf8String();

    dt_print(DT_DEBUG_METAL,
            "[dt_metal_list_devices] Device: %s",
            deviceName);
  }
}


gboolean dt_metal_is_available(dt_metal_t *metal)
{
  if(!metal || metal->num_devs == 0) return FALSE;

  // check that at least one device has a valid library and command queue
  for(int i = 0; i < metal->num_devs; i++)
  {
    if(metal->dev[i].device && metal->dev[i].library && metal->dev[i].command_queue)
      return TRUE;
  }
  return FALSE;
}


int dt_metal_create_kernel(dt_metal_t *metal, const char *kernel_name)
{
  if(!metal || metal->num_devs == 0) return -1;

  // use device 0 (primary GPU) for now
  dt_metal_device_t *dev = &metal->dev[0];
  if(!dev->device || !dev->library) return -1;

  MTL::Library *library = (MTL::Library *)dev->library;
  MTL::Device *device = (MTL::Device *)dev->device;

  // look up the function by name
  NS::String *funcName = NS::String::string(kernel_name, NS::StringEncoding::UTF8StringEncoding);
  MTL::Function *function = library->newFunction(funcName);
  if(!function)
  {
    dt_print(DT_DEBUG_METAL,
            "[dt_metal_create_kernel] Function '%s' not found in metallib",
            kernel_name);
    return -1;
  }

  // create compute pipeline state
  NS::Error *error = NULL;
  MTL::ComputePipelineState *pipeline = device->newComputePipelineState(function, &error);
  function->release();

  if(!pipeline)
  {
    dt_print(DT_DEBUG_METAL,
            "[dt_metal_create_kernel] Could not create pipeline for '%s': %s",
            kernel_name,
            error ? error->localizedDescription()->utf8String() : "unknown error");
    return -1;
  }

  // store the pipeline in the kernels array
  if(metal->num_kernels >= DT_METAL_MAX_KERNELS)
  {
    dt_print(DT_DEBUG_METAL,
            "[dt_metal_create_kernel] Maximum number of Metal kernels reached");
    pipeline->release();
    return -1;
  }

  const int kernel_id = metal->num_kernels;
  metal->kernels[kernel_id] = (void *)pipeline;
  metal->num_kernels++;

  dt_print(DT_DEBUG_METAL,
          "[dt_metal_create_kernel] Created kernel '%s' with id=%d",
          kernel_name, kernel_id);

  return kernel_id;
}


void dt_metal_free_kernel(dt_metal_t *metal, int kernel_id)
{
  if(!metal || kernel_id < 0 || kernel_id >= metal->num_kernels) return;
  if(!metal->kernels[kernel_id]) return;

  MTL::ComputePipelineState *pipeline = (MTL::ComputePipelineState *)metal->kernels[kernel_id];
  pipeline->release();
  metal->kernels[kernel_id] = NULL;
}


int dt_metal_enqueue_kernel_2d(dt_metal_t *metal,
                               int kernel_id,
                               int width, int height,
                               const float *input,
                               float *output,
                               int num_extra_args,
                               const void **extra_args,
                               const size_t *extra_arg_sizes)
{
  if(!metal || kernel_id < 0 || kernel_id >= metal->num_kernels)
    return DT_METAL_DEFAULT_ERROR;

  MTL::ComputePipelineState *pipeline = (MTL::ComputePipelineState *)metal->kernels[kernel_id];
  if(!pipeline) return DT_METAL_DEFAULT_ERROR;

  // use device 0 for now
  dt_metal_device_t *dev = &metal->dev[0];
  if(!dev->device || !dev->command_queue) return DT_METAL_DEFAULT_ERROR;

  MTL::Device *device = (MTL::Device *)dev->device;
  MTL::CommandQueue *commandQueue = (MTL::CommandQueue *)dev->command_queue;

  const size_t num_pixels = (size_t)width * height;
  const size_t buf_size = num_pixels * 4 * sizeof(float);  // float4 per pixel

  // create Metal buffers for input and output
  // Use shared storage mode (CPU and GPU share memory on Apple Silicon)
  MTL::Buffer *inputBuffer = device->newBuffer(buf_size, MTL::ResourceStorageModeShared);
  MTL::Buffer *outputBuffer = device->newBuffer(buf_size, MTL::ResourceStorageModeShared);

  if(!inputBuffer || !outputBuffer)
  {
    dt_print(DT_DEBUG_METAL,
            "[dt_metal_enqueue_kernel_2d] Could not allocate Metal buffers "
            "(%zu bytes each)\n", buf_size);
    if(inputBuffer) inputBuffer->release();
    if(outputBuffer) outputBuffer->release();
    return DT_METAL_DEFAULT_ERROR;
  }

  // copy input data to Metal buffer
  memcpy(inputBuffer->contents(), input, buf_size);

  // create command buffer and compute command encoder
  MTL::CommandBuffer *commandBuffer = commandQueue->commandBuffer();
  if(!commandBuffer)
  {
    dt_print(DT_DEBUG_METAL,
            "[dt_metal_enqueue_kernel_2d] Could not create command buffer");
    inputBuffer->release();
    outputBuffer->release();
    return DT_METAL_DEFAULT_ERROR;
  }

  MTL::ComputeCommandEncoder *encoder = commandBuffer->computeCommandEncoder();
  if(!encoder)
  {
    dt_print(DT_DEBUG_METAL,
            "[dt_metal_enqueue_kernel_2d] Could not create compute encoder");
    inputBuffer->release();
    outputBuffer->release();
    return DT_METAL_DEFAULT_ERROR;
  }

  // set the compute pipeline
  encoder->setComputePipelineState(pipeline);

  // set buffer arguments:
  //   buffer(0) = input pixel data
  //   buffer(1) = output pixel data
  //   buffer(2) = width (int)
  //   buffer(3) = height (int)
  //   buffer(4..) = extra arguments
  encoder->setBuffer(inputBuffer, 0, 0);
  encoder->setBuffer(outputBuffer, 0, 1);
  encoder->setBytes(&width, sizeof(int), 2);
  encoder->setBytes(&height, sizeof(int), 3);

  for(int i = 0; i < num_extra_args; i++)
  {
    encoder->setBytes(extra_args[i], extra_arg_sizes[i], 4 + i);
  }

  // calculate thread group sizes
  // use the pipeline's max threads per threadgroup, capped to reasonable 2D sizes
  const NS::UInteger maxThreads = pipeline->maxTotalThreadsPerThreadgroup();
  NS::UInteger threadW = 16;
  NS::UInteger threadH = 16;
  // adjust if maxThreads is smaller
  while(threadW * threadH > maxThreads)
  {
    if(threadH > 1) threadH /= 2;
    else threadW /= 2;
  }

  const MTL::Size gridSize = MTL::Size::Make(width, height, 1);
  const MTL::Size threadGroupSize = MTL::Size::Make(threadW, threadH, 1);

  encoder->dispatchThreads(gridSize, threadGroupSize);
  encoder->endEncoding();

  // submit and wait for completion
  commandBuffer->commit();
  commandBuffer->waitUntilCompleted();

  // check for errors
  if(commandBuffer->status() == MTL::CommandBufferStatusError)
  {
    NS::Error *cbError = commandBuffer->error();
    dt_print(DT_DEBUG_METAL,
            "[dt_metal_enqueue_kernel_2d] Command buffer error: %s",
            cbError ? cbError->localizedDescription()->utf8String() : "unknown");
    inputBuffer->release();
    outputBuffer->release();
    return DT_METAL_DEFAULT_ERROR;
  }

  // copy results back to the output buffer
  memcpy(output, outputBuffer->contents(), buf_size);

  // clean up
  inputBuffer->release();
  outputBuffer->release();

  return 0;
}
