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
 * File name: WienerDeconvolveGPU.cu
 * Author: Cory Quammen <cquammen@cs.unc.edu>
 */


#include <cuda.h>
#include <stdio.h>

#include "ComplexCUDA.h"
#include "WienerDeconvolveGPU.h"

#define BLOCKS 16
#define THREADS_PER_BLOCK 128


__global__
void
WienerCUDAKernel(
   int n, float scale, Complex* inFT, Complex* psfFT, 
   Complex* outFT, float epsilon) {

   const int tid     = __mul24(blockIdx.x, blockDim.x) + threadIdx.x;
   const int threadN = __mul24(blockDim.x, gridDim.x);

   for (int voxelID = tid; voxelID < n; voxelID += threadN) {
      Complex H = psfFT[voxelID];
      Complex HConj = ComplexConjugate(H);
      float HMagSquared = ComplexMagnitudeSquared(H);
      HConj = ComplexScale(HConj, 1.0f / (HMagSquared + epsilon));
      outFT[voxelID] = ComplexMultiplyAndScale(
         HConj, inFT[voxelID], scale);
   }
}


extern "C"
void
WienerDeconvolveKernelGPU(
   int nx, int ny, int nz, float* inFT, float* psfFT, 
   float* outFT, float epsilon) {

   int n = nz*ny*(nx/2 + 1);
   dim3 grid(BLOCKS);
   dim3 block(THREADS_PER_BLOCK);
   float scale = 1.0f / ((float) nx*ny*nz);

   WienerCUDAKernel<<<grid, block>>>(n, scale, (Complex*)inFT,
      (Complex*)psfFT, (Complex*)outFT, epsilon);

   cudaError result = cudaThreadSynchronize();
   if (result != cudaSuccess) {
      fprintf(stderr, "CUDA error: %s in file '%s' in line %i : %s.\n",
         "WienerCUDAKernel failed", __FILE__, __LINE__, cudaGetErrorString(result));
   }
}
