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
#include <unistd.h>


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
      metal->dev[i].active_command_buffer = NULL;
      metal->dev[i].active_encoder = NULL;

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


/* ── Multi-buffer API implementation ─────────────────────────────── */

dt_metal_buffer_t dt_metal_alloc_buffer(dt_metal_t *metal, size_t size)
{
  if(!metal || metal->num_devs == 0) return NULL;

  MTL::Device *device = (MTL::Device *)metal->dev[0].device;
  if(!device) return NULL;

  MTL::Buffer *buf = device->newBuffer(size, MTL::ResourceStorageModeShared);
  if(!buf)
  {
    dt_print(DT_DEBUG_METAL,
            "[dt_metal_alloc_buffer] Could not allocate %zu bytes", size);
    return NULL;
  }

  return (dt_metal_buffer_t)buf;
}


void dt_metal_free_buffer(dt_metal_buffer_t buf)
{
  if(!buf) return;
  ((MTL::Buffer *)buf)->release();
}


void *dt_metal_buffer_get_contents(dt_metal_buffer_t buf)
{
  if(!buf) return NULL;
  return ((MTL::Buffer *)buf)->contents();
}


void dt_metal_copy_to_buffer(dt_metal_buffer_t buf, const void *src, size_t size)
{
  if(!buf || !src) return;
  memcpy(((MTL::Buffer *)buf)->contents(), src, size);
}


void dt_metal_copy_from_buffer(dt_metal_buffer_t buf, void *dst, size_t size)
{
  if(!buf || !dst) return;
  memcpy(dst, ((MTL::Buffer *)buf)->contents(), size);
}


int dt_metal_enqueue_kernel_2d_flex(dt_metal_t *metal,
                                    int kernel_id,
                                    int width, int height,
                                    int num_args,
                                    const dt_metal_arg_t *args)
{
  return dt_metal_enqueue_kernel_2d_flex_with_tgs(metal, kernel_id, width, height,
                                                   num_args, args, 0, 0);
}


int dt_metal_enqueue_kernel_2d_flex_with_tgs(dt_metal_t *metal,
                                              int kernel_id,
                                              int width, int height,
                                              int num_args,
                                              const dt_metal_arg_t *args,
                                              int threadW_hint, int threadH_hint)
{
  if(!metal || kernel_id < 0 || kernel_id >= metal->num_kernels)
    return DT_METAL_DEFAULT_ERROR;

  MTL::ComputePipelineState *pipeline = (MTL::ComputePipelineState *)metal->kernels[kernel_id];
  if(!pipeline) return DT_METAL_DEFAULT_ERROR;

  dt_metal_device_t *dev = &metal->dev[0];
  if(!dev->device || !dev->command_queue) return DT_METAL_DEFAULT_ERROR;

  // Check if we're inside a batch
  const gboolean batched = (dev->active_encoder != NULL);
  MTL::ComputeCommandEncoder *encoder = NULL;
  MTL::CommandBuffer *commandBuffer = NULL;

  if(batched)
  {
    encoder = (MTL::ComputeCommandEncoder *)dev->active_encoder;

    // Memory barrier to ensure previous kernel writes are visible
    // Include both buffer and texture scopes for mixed workloads
    encoder->memoryBarrier(MTL::BarrierScope(MTL::BarrierScopeBuffers | MTL::BarrierScopeTextures));
  }
  else
  {
    // Non-batched: create a fresh command buffer + encoder
    MTL::CommandQueue *commandQueue = (MTL::CommandQueue *)dev->command_queue;
    commandBuffer = commandQueue->commandBuffer();
    if(!commandBuffer)
    {
      dt_print(DT_DEBUG_METAL,
              "[dt_metal_enqueue_kernel_2d_flex] Could not create command buffer");
      return DT_METAL_DEFAULT_ERROR;
    }

    encoder = commandBuffer->computeCommandEncoder();
    if(!encoder)
    {
      dt_print(DT_DEBUG_METAL,
              "[dt_metal_enqueue_kernel_2d_flex] Could not create compute encoder");
      return DT_METAL_DEFAULT_ERROR;
    }
  }

  encoder->setComputePipelineState(pipeline);

  // bind arguments: buffers/bytes use buffer index space,
  // textures use a separate texture index space
  int buf_idx = 0;
  int tex_idx = 0;
  for(int i = 0; i < num_args; i++)
  {
    if(args[i].type == DT_METAL_ARG_BUFFER)
    {
      MTL::Buffer *buf = (MTL::Buffer *)args[i].data;
      encoder->setBuffer(buf, 0, buf_idx++);
    }
    else if(args[i].type == DT_METAL_ARG_TEXTURE)
    {
      MTL::Texture *tex = (MTL::Texture *)args[i].data;
      encoder->setTexture(tex, tex_idx++);
    }
    else // DT_METAL_ARG_BYTES
    {
      encoder->setBytes(args[i].data, args[i].size, buf_idx++);
    }
  }

  // calculate thread group sizes
  const NS::UInteger maxThreads = pipeline->maxTotalThreadsPerThreadgroup();
  NS::UInteger threadW = (threadW_hint > 0) ? (NS::UInteger)threadW_hint : 16;
  NS::UInteger threadH = (threadH_hint > 0) ? (NS::UInteger)threadH_hint : 16;
  // clamp to pipeline limits
  while(threadW * threadH > maxThreads)
  {
    if(threadH > 1) threadH /= 2;
    else threadW /= 2;
  }

  const MTL::Size gridSize = MTL::Size::Make(width, height, 1);
  const MTL::Size threadGroupSize = MTL::Size::Make(threadW, threadH, 1);

  encoder->dispatchThreads(gridSize, threadGroupSize);

  if(!batched)
  {
    // Non-batched: finalize immediately
    encoder->endEncoding();
    commandBuffer->commit();
    commandBuffer->waitUntilCompleted();

    if(commandBuffer->status() == MTL::CommandBufferStatusError)
    {
      NS::Error *cbError = commandBuffer->error();
      dt_print(DT_DEBUG_METAL,
              "[dt_metal_enqueue_kernel_2d_flex] Command buffer error: %s",
              cbError ? cbError->localizedDescription()->utf8String() : "unknown");
      return DT_METAL_DEFAULT_ERROR;
    }
  }

  return 0;
}


/* ── Batch dispatch API ──────────────────────────────────────────── */

int dt_metal_begin_batch(dt_metal_t *metal)
{
  if(!metal || metal->num_devs == 0) return DT_METAL_DEFAULT_ERROR;

  dt_metal_device_t *dev = &metal->dev[0];
  if(!dev->device || !dev->command_queue) return DT_METAL_DEFAULT_ERROR;

  // Don't nest batches
  if(dev->active_encoder)
  {
    dt_print(DT_DEBUG_METAL,
            "[dt_metal_begin_batch] Batch already active, ignoring");
    return 0;
  }

  MTL::CommandQueue *commandQueue = (MTL::CommandQueue *)dev->command_queue;
  MTL::CommandBuffer *commandBuffer = commandQueue->commandBuffer();
  if(!commandBuffer)
  {
    dt_print(DT_DEBUG_METAL,
            "[dt_metal_begin_batch] Could not create command buffer");
    return DT_METAL_DEFAULT_ERROR;
  }

  MTL::ComputeCommandEncoder *encoder = commandBuffer->computeCommandEncoder();
  if(!encoder)
  {
    dt_print(DT_DEBUG_METAL,
            "[dt_metal_begin_batch] Could not create compute encoder");
    return DT_METAL_DEFAULT_ERROR;
  }

  dev->active_command_buffer = (void *)commandBuffer;
  dev->active_encoder = (void *)encoder;

  return 0;
}


int dt_metal_end_batch(dt_metal_t *metal)
{
  if(!metal || metal->num_devs == 0) return DT_METAL_DEFAULT_ERROR;

  dt_metal_device_t *dev = &metal->dev[0];

  MTL::ComputeCommandEncoder *encoder = (MTL::ComputeCommandEncoder *)dev->active_encoder;
  MTL::CommandBuffer *commandBuffer = (MTL::CommandBuffer *)dev->active_command_buffer;

  if(!encoder || !commandBuffer)
  {
    dt_print(DT_DEBUG_METAL,
            "[dt_metal_end_batch] No active batch");
    return DT_METAL_DEFAULT_ERROR;
  }

  // Clear batch state before waiting (so error paths don't try to double-end)
  dev->active_encoder = NULL;
  dev->active_command_buffer = NULL;

  encoder->endEncoding();
  commandBuffer->commit();
  commandBuffer->waitUntilCompleted();

  if(commandBuffer->status() == MTL::CommandBufferStatusError)
  {
    NS::Error *cbError = commandBuffer->error();
    dt_print(DT_DEBUG_METAL,
            "[dt_metal_end_batch] Command buffer error: %s",
            cbError ? cbError->localizedDescription()->utf8String() : "unknown");
    return DT_METAL_DEFAULT_ERROR;
  }

  return 0;
}


/* ── No-copy buffer API ──────────────────────────────────────────── */

dt_metal_buffer_t dt_metal_alloc_buffer_nocopy(dt_metal_t *metal, void *ptr, size_t size)
{
  if(!metal || metal->num_devs == 0 || !ptr) return NULL;

  MTL::Device *device = (MTL::Device *)metal->dev[0].device;
  if(!device) return NULL;

  // Check page alignment (required for no-copy buffers)
  const size_t page_size = sysconf(_SC_PAGESIZE);
  if(((uintptr_t)ptr & (page_size - 1)) != 0)
  {
    // Not page-aligned: fall back to alloc + copy
    dt_print(DT_DEBUG_METAL,
            "[dt_metal_alloc_buffer_nocopy] Pointer not page-aligned, falling back to copy");
    dt_metal_buffer_t buf = dt_metal_alloc_buffer(metal, size);
    if(buf) dt_metal_copy_to_buffer(buf, ptr, size);
    return buf;
  }

  // Create no-copy buffer. Pass NULL deallocator — the caller owns the memory.
  MTL::Buffer *buf = device->newBuffer(ptr, size,
                                        MTL::ResourceStorageModeShared | MTL::ResourceCPUCacheModeDefaultCache,
                                        nullptr);
  if(!buf)
  {
    dt_print(DT_DEBUG_METAL,
             "[dt_metal_alloc_buffer_nocopy] Could not create no-copy buffer (%zu bytes)", size);
    return NULL;
  }

  return (dt_metal_buffer_t)buf;
}


/* ── Texture API implementation ──────────────────────────────────── */

static MTL::Texture *_dt_metal_alloc_texture(dt_metal_t *metal,
                                              int width, int height,
                                              MTL::PixelFormat pixelFormat)
{
  if(!metal || metal->num_devs == 0) return NULL;

  MTL::Device *device = (MTL::Device *)metal->dev[0].device;
  if(!device) return NULL;

  MTL::TextureDescriptor *desc = MTL::TextureDescriptor::texture2DDescriptor(
      pixelFormat, (NS::UInteger)width, (NS::UInteger)height, false);
  if(!desc) return NULL;

  desc->setStorageMode(MTL::StorageModeShared);
  desc->setUsage(MTL::TextureUsageShaderRead | MTL::TextureUsageShaderWrite);

  MTL::Texture *tex = device->newTexture(desc);

  if(!tex)
  {
    dt_print(DT_DEBUG_METAL,
            "[dt_metal_alloc_texture] Could not allocate %dx%d texture", width, height);
  }

  return tex;
}


dt_metal_texture_t dt_metal_alloc_texture_rgba_f32(dt_metal_t *metal, int width, int height)
{
  return (dt_metal_texture_t)_dt_metal_alloc_texture(metal, width, height,
                                                      MTL::PixelFormatRGBA32Float);
}


dt_metal_texture_t dt_metal_alloc_texture_r8(dt_metal_t *metal, int width, int height)
{
  return (dt_metal_texture_t)_dt_metal_alloc_texture(metal, width, height,
                                                      MTL::PixelFormatR8Uint);
}


void dt_metal_free_texture(dt_metal_texture_t tex)
{
  if(!tex) return;
  ((MTL::Texture *)tex)->release();
}


void dt_metal_copy_to_texture(dt_metal_texture_t tex, const void *src, size_t bytes_per_row)
{
  if(!tex || !src) return;
  MTL::Texture *t = (MTL::Texture *)tex;
  MTL::Region region = MTL::Region::Make2D(0, 0, t->width(), t->height());
  t->replaceRegion(region, 0, src, bytes_per_row);
}


void dt_metal_copy_from_texture(dt_metal_texture_t tex, void *dst, size_t bytes_per_row)
{
  if(!tex || !dst) return;
  MTL::Texture *t = (MTL::Texture *)tex;
  MTL::Region region = MTL::Region::Make2D(0, 0, t->width(), t->height());
  t->getBytes(dst, bytes_per_row, region, 0);
}
