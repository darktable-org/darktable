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
 * File name: Clarity.cxx
 * Author: Cory Quammen <cquammen@cs.unc.edu>
 */


#include "Clarity.h"

#include <fftw3.h>
#include <iostream>
#include <omp.h>

#ifdef BUILD_WITH_CUDA
#include <cuda.h>
#include <cuda_runtime_api.h>
#endif

/** How many clients are registered. */
static unsigned g_RegisteredClients = 0;

/** Indicates that a CUDA-capable device is available. */
bool g_CUDACapable = false;

ClarityResult_t
Clarity_Register() {
   if (g_RegisteredClients <= 0) {
      fftwf_init_threads();
      int np = omp_get_num_procs();
      Clarity_SetNumberOfThreads(np);

#ifdef BUILD_WITH_CUDA
      int deviceCount = 0;
      cudaGetDeviceCount(&deviceCount);
      if (deviceCount >= 1) {
         cudaDeviceProp deviceProp;
         cudaGetDeviceProperties(&deviceProp, 0);
         std::cout << "CUDA device found: '" << deviceProp.name << "'" << std::endl;
         g_CUDACapable = true;
      }
#endif
   }
   g_RegisteredClients++;

   return CLARITY_SUCCESS;
}


ClarityResult_t
Clarity_UnRegister() {
   g_RegisteredClients--;
   if (g_RegisteredClients <= 0) {
      fftwf_cleanup_threads();
      g_CUDACapable = false;
   }

   return CLARITY_SUCCESS;
}


ClarityResult_t
Clarity_SetNumberOfThreads(unsigned n) {
   omp_set_num_threads(n);
   int np = omp_get_num_procs();
   fftwf_plan_with_nthreads(np);

   return CLARITY_SUCCESS;
}


Clarity_Dim3
Clarity_Dim3FromArray(int dimArray[3]) {
   Clarity_Dim3 dim;
   dim.x = dimArray[0];
   dim.y = dimArray[1];
   dim.z = dimArray[2];
   return dim;
}
