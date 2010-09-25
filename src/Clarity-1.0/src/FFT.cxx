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
 * File name: FFT.cxx
 * Author: Cory Quammen <cquammen@cs.unc.edu>
 */


#include "Clarity.h"

#include <fftw3.h>

#include "FFT.h"
#include "Memory.h"

#ifdef BUILD_WITH_CUDA
#include <cuda.h>
#include <cuda_runtime_api.h>
#include <cufft.h>
#endif

extern bool g_CUDACapable;

#ifdef TIME
#include <iostream>
#include "Stopwatch.h"
#endif


ClarityResult_t
Clarity_FFT_R2C_float(
   int nx, int ny, int nz, float* in, float* out) {

#ifdef BUILD_WITH_CUDA
   if (g_CUDACapable) {
      cufftHandle plan;
      cufftResult cufftResult = cufftPlan3d(&plan, nz, ny, nx, 
         CUFFT_R2C);
      if (cufftResult != CUFFT_SUCCESS) {
         return CLARITY_FFT_FAILED;
      }
#ifdef TIME
      Stopwatch timer;
      timer.Start();
#endif
      cufftResult = cufftExecR2C(plan, (cufftReal*)in, 
         (cufftComplex*)out);
#ifdef TIME
      timer.Stop();
      std::cout << "R2C: " << timer << std::endl;
#endif

      cufftDestroy(plan);
      if (cufftResult != CUFFT_SUCCESS) {
         return CLARITY_FFT_FAILED;
      }
   } else 
#endif // BUILD_WITH_CUDA
    {
      fftwf_complex* outComplex = (fftwf_complex*) out;

      // Holy smokes, I wasted a lot of time just to find that 
      // FFTW expects data arranged not in the normal way in 
      // medical/biological imaging. Dimension sizes need to be 
      // reversed.
      fftwf_plan plan = fftwf_plan_dft_r2c_3d(nz, ny, nx, in, 
         outComplex, FFTW_ESTIMATE);
      if (plan == NULL) {
         return CLARITY_FFT_FAILED;
      }
#ifdef TIME
      Stopwatch timer;
      timer.Start();
#endif
      fftwf_execute(plan);
#ifdef TIME
      timer.Stop();
      std::cout << "R2C: " << timer << std::endl;
#endif

      fftwf_destroy_plan(plan);
   }

   return CLARITY_SUCCESS;
}


ClarityResult_t
Clarity_FFT_C2R_float(
   int nx, int ny, int nz, float* in, float* out) {

   int numVoxels = nx*ny*nz;

#ifdef BUILD_WITH_CUDA
   if (g_CUDACapable) {
      cufftHandle plan;
      cufftResult cufftResult = cufftPlan3d(&plan, nz, ny, nx, 
         CUFFT_C2R);
      if (cufftResult != CUFFT_SUCCESS) {
         return CLARITY_FFT_FAILED;
      }
      cufftResult = cufftExecC2R(plan, (cufftComplex*)in, 
         (cufftReal*)out);
      cufftDestroy(plan);
      if (cufftResult != CUFFT_SUCCESS) {
         return CLARITY_FFT_FAILED;
      }
   } else
#endif
   {
      fftwf_complex* inComplex = (fftwf_complex*) in;

      // Holy smokes, I wasted a lot of time just to find that 
      // FFTW expects data arranged not in the normal way in 
      // medical/biological imaging. Dimension sizes need to be 
      // reversed.
      fftwf_plan plan = fftwf_plan_dft_c2r_3d(nz, ny, nx, 
         inComplex, out, FFTW_ESTIMATE);
      if (plan == NULL) {
         return CLARITY_FFT_FAILED;
      }
      fftwf_execute(plan);
      fftwf_destroy_plan(plan);
   }

   return CLARITY_SUCCESS;
}
