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
 * File name: ComputePrimitivesGPU.cu
 * Author: Cory Quammen <cquammen@cs.unc.edu>
 */


#include <cuda.h>
#include <cuda_runtime_api.h>
#include "ComputePrimitivesGPU.h"


#define BLOCKS 16
#define THREADS_PER_BLOCK 128


__global__
void
ReduceSumKernelGPU(
   float* blockResults, float* data, int n, int padN) {
   
   extern __shared__ float sdata[];
   int tid  = blockDim.x*blockIdx.x + threadIdx.x;
   int incr = gridDim.x*blockDim.x;
   float sum = 0.0f;

   for (int i = tid; i < padN; i += incr) {
      // Load data into shared memory. All reads 
      // should be coalesced by reading them this way.
      sdata[threadIdx.x] = 0.0f;
      if (i < n)
         sdata[threadIdx.x] = data[i];

      // Reduce the values in shared memory.
      for (int d = blockDim.x >> 1; d > 0; d >>= 1) {
         __syncthreads(); // Make sure all data is read before
                          // proceeding.

         // No bank conflicts in shared memory here.
         if (threadIdx.x < d)
            sdata[threadIdx.x] += sdata[threadIdx.x+d];
      }
      __syncthreads();

      // The reduction results end up in element 0 of shared memory.
      sum += sdata[0];
   }

   // Only thread 0 writes the sum to memory.
   if (threadIdx.x == 0)
      blockResults[blockIdx.x] = sum;
}


extern "C"
void
Clarity_ReduceSumGPU(
   float* result, float* buffer, int n) {
   
   // Set up device call configuration.
   dim3 blockSize(THREADS_PER_BLOCK);
   dim3 gridSize(BLOCKS);
   size_t sharedSize = sizeof(float)*blockSize.x;
   int numThreads = blockSize.x * gridSize.x;
   int paddedArraySize = n;
   int remainder = paddedArraySize % numThreads;
   if (remainder)
      paddedArraySize = ((n / numThreads) + 1) * numThreads;

   // Allocate memory on the device for block-wise partial 
   // reductions computed by the kernel.
   float *blockResultsDev = NULL;
   cudaMalloc((void**)&blockResultsDev, sizeof(float)*gridSize.x);

   ReduceSumKernelGPU<<<gridSize, blockSize, sharedSize>>>(
      blockResultsDev, buffer, n, paddedArraySize);

   // Read the partial sums from the blocks back to the host.
   float* blockResultsHost = (float*) malloc(sizeof(float)*gridSize.x);
   cudaMemcpy(blockResultsHost, blockResultsDev, 
      sizeof(float)*gridSize.x, cudaMemcpyDeviceToHost);

   // Add up the results
   *result = 0.0f;
   for (int i = 0; i < gridSize.x; i++) {
      *result += blockResultsHost[i];
   }

   free(blockResultsHost);
   cudaFree(blockResultsDev);
}


__global__
void
MultiplyArraysComponentWiseKernelGPU(
   float* result, float* a, float* b, int n) {

   int tid  = blockDim.x*blockIdx.x + threadIdx.x;
   int incr = gridDim.x*blockDim.x;
   
   for (int i = tid; i < n; i += incr) {
      result[i] = a[i] * b[i];
   }
}


void
Clarity_MultiplyArraysComponentWiseGPU(
   float* result, float* a, float* b, int n) {

   // Set up device call configuration.
   dim3 blockSize(THREADS_PER_BLOCK);
   dim3 gridSize(BLOCKS);

   MultiplyArraysComponentWiseKernelGPU<<<gridSize, blockSize>>>(
      result, a, b, n);
}


__global__
void
DivideArraysComponentWiseKernelGPU(
   float* result, float* a, float* b, float value, int n) {

   int tid  = blockDim.x*blockIdx.x + threadIdx.x;
   int incr = gridDim.x*blockDim.x;
   
   for (int i = tid; i < n; i += incr) {
      if (fabs(b[i]) < 1e-5) {
         result[i] = value;
      } else {
         result[i] = a[i] / b[i];
      }
   }
}


void
Clarity_DivideArraysComponentWiseGPU(
   float* result, float* a, float* b, float value, int n) {

   // Set up device call configuration.
   dim3 blockSize(THREADS_PER_BLOCK);
   dim3 gridSize(BLOCKS);

   DivideArraysComponentWiseKernelGPU<<<gridSize, blockSize>>>(
      result, a, b, value, n);
}


__global__
void
ScaleArrayKernelGPU(
   float* result, float* a, int n, float scale) {

   int tid  = blockDim.x*blockIdx.x + threadIdx.x;
   int incr = gridDim.x*blockDim.x;
   
   for (int i = tid; i < n; i += incr) {
      result[i] = a[i] * scale;
   }
}


extern "C"
void
Clarity_ScaleArrayGPU(
   float* result, float* a, int n, float scale) {

   // Set up device call configuration.
   dim3 blockSize(THREADS_PER_BLOCK);
   dim3 gridSize(BLOCKS);

   ScaleArrayKernelGPU<<<gridSize, blockSize>>>(
      result, a, n, scale);
}
