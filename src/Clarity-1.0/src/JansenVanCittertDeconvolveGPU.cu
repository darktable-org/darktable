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
 * File name: JansenVanCittertDeconvolveGPU.cu
 * Author: Cory Quammen <cquammen@cs.unc.edu>
 */


#include <stdio.h>

#define BLOCKS 16
#define THREADS_PER_BLOCK 128

#include "JansenVanCittertDeconvolveGPU.h"


__global__
void
JansenVanCittertCUDAKernel(
   int n, float* in, float inMax, float invMaxSq, float* i_k,
   float* o_k, float* i_kNext) {

   const int tid     = __mul24(blockIdx.x, blockDim.x) + threadIdx.x;
   const int threadN = __mul24(blockDim.x, gridDim.x);

   for (int j = tid; j < n; j += threadN) {
      float diff = o_k[j] - inMax;
      float gamma = 1.0f - ((diff * diff) * invMaxSq);
      float val = i_k[j] + (gamma * (in[j] - o_k[j]));
      i_kNext[j] = max(val, 0.0f);
   }
}


extern "C"
void
JansenVanCittertDeconvolveKernelGPU(
   int nx, int ny, int nz, float* in, float inMax, float invMaxSq,
   float* i_k, float* o_k, float* i_kNext) {

   int n = nz*ny*nx;
   dim3 grid(BLOCKS);
   dim3 block(THREADS_PER_BLOCK);

   JansenVanCittertCUDAKernel<<<grid, block>>>(n, in, inMax, invMaxSq, 
      i_k, o_k, i_kNext);

}
