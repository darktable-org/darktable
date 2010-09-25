/* 
 * Clarity is Copyright 2008 Center for Integrated Systems for Microscopy, 
 * Copyright 2008 University of North Carolina at Chapel Hill.
 *
 * Clarity is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Public License as published by the Free Software 
 * Foundation; either version 2 of the License, or (at your option) any 
 * later version.
 *
 * This program is distributed in the hope that it will be useful, but 
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License 
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along 
 * with this program; if not, write to the Free Software Foundation, Inc., 
 * 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA. You can also find 
 * the GPL on the GNU web site (http://www.gnu.org/copyleft/gpl.html).
 *
 * File name: Memory.cxx
 * Author: Cory Quammen <cquammen@cs.unc.edu>
 */


#include "Clarity.h"
#include "Memory.h"

#include <fftw3.h>

#ifdef BUILD_WITH_CUDA
#include <cuda.h>
#include <cuda_runtime_api.h>
#include <cufft.h>
#endif

extern bool g_CUDACapable;

ClarityResult_t
Clarity_Complex_Malloc(
   void** buffer, size_t size, int nx, int ny, int nz) {

   ClarityResult_t result = CLARITY_SUCCESS;

   size_t totalSize = size*2*nz*ny*(nx/2 + 1);
#ifdef BUILD_WITH_CUDA
   if (g_CUDACapable) {
      cudaError_t cudaResult = cudaMalloc(buffer, totalSize);
      if (cudaResult != cudaSuccess) {
         return CLARITY_DEVICE_OUT_OF_MEMORY;
      }
   } else 
#endif // BUILD_WITH_CUDA
   {
      *buffer = fftwf_malloc(totalSize);
      if (*buffer == NULL) {
         result = CLARITY_OUT_OF_MEMORY;
      }
   }

   return result;
}


ClarityResult_t
Clarity_Real_Malloc(
   void** buffer, size_t size, int nx, int ny, int nz) {

   ClarityResult_t result = CLARITY_SUCCESS;

   size_t totalSize = size*nz*ny*nx;
#ifdef BUILD_WITH_CUDA
   if (g_CUDACapable) {
      cudaError_t cudaResult = cudaMalloc(buffer, totalSize);
      if (cudaResult != cudaSuccess) {
         return CLARITY_DEVICE_OUT_OF_MEMORY;
      }
   } else
#endif // BUILD_WITH_CUDA
   {
      *buffer = fftwf_malloc(sizeof(size)*nz*ny*nx);
      if (*buffer == NULL) {
         result = CLARITY_OUT_OF_MEMORY;
      }
   }

   return result;
}

#ifdef BUILD_WITH_CUDA


ClarityResult_t
Clarity_CopyToDevice(
   int nx, int ny, int nz, size_t size, void* dst, void* src) {

   if (!g_CUDACapable)
      return CLARITY_INVALID_OPERATION;

#ifdef BUILD_WITH_CUDA
   size_t totalSize = size*nx*ny*nz;
   cudaError_t cudaResult = cudaMemcpy(dst, src, totalSize,
      cudaMemcpyHostToDevice);
   if (cudaResult != cudaSuccess) {
      return CLARITY_INVALID_OPERATION;
   }
#endif

   return CLARITY_SUCCESS;
}


ClarityResult_t
Clarity_CopyFromDevice(
   int nx, int ny, int nz, size_t size, void* dst, void* src) {

   if (!g_CUDACapable)
      return CLARITY_INVALID_OPERATION;

#ifdef BUILD_WITH_CUDA
   size_t totalSize = size*nx*ny*nz;
   cudaError_t cudaResult = cudaMemcpy(dst, src, totalSize,
      cudaMemcpyDeviceToHost);
   if (cudaResult != cudaSuccess) {
      return CLARITY_INVALID_OPERATION;
   }
#endif

   return CLARITY_SUCCESS;
}


ClarityResult_t
Clarity_Real_MallocCopy(void** buffer, size_t size, 
                        int nx, int ny, int nz, void* src) {
   if (!g_CUDACapable)
      return CLARITY_INVALID_OPERATION;

   ClarityResult_t result = CLARITY_SUCCESS;
   result = Clarity_Real_Malloc(buffer, size, nx, ny, nz);
   if (result != CLARITY_SUCCESS) {
      return result;
   }

   result = Clarity_CopyToDevice(nx, ny, nz, size, *buffer, src);
   if (result != CLARITY_SUCCESS) {
      Clarity_Free(*buffer);
      return result;
   }

   return result;
}

#endif // BUILD_WITH_CUDA


void
Clarity_Free(void* buffer) {
   if (buffer == NULL)
      return;
#ifdef BUILD_WITH_CUDA
   if (g_CUDACapable) {
      cudaFree(buffer);
   } else
#endif // BUILD_WITH_CUDA
   {
      fftwf_free(buffer);
   }
}
