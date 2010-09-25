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
 * File name: Convolve.cxx
 * Author: Cory Quammen <cquammen@cs.unc.edu>
 */


#include <cstdlib>

#include "Clarity.h"

#include "Complex.h"
#include "Convolve.h"
#include "FFT.h"
#include "Memory.h"

extern bool g_CUDACapable;

#ifdef TIME
#include <iostream>
#include "Stopwatch.h"

static Stopwatch totalTimer("Convolve (total time)");
static Stopwatch transferTimer("Convolve filter (transfer time)");
#endif

ClarityResult_t
Clarity_Convolve( float* inImage, Clarity_Dim3 imageDim,
		  float* kernel, Clarity_Dim3 kernelDim,
		  float* outImage) {

  // Compute working dimensions. The working dimensions are the sum of the
  // image and kernel dimensions. This handles the cyclic nature of convolution
  // using multiplication in the Fourier domain.
  Clarity_Dim3 workDim;
  workDim.x = imageDim.x + kernelDim.x;
  workDim.y = imageDim.y + kernelDim.y;
  workDim.z = imageDim.z + kernelDim.z;
  int workVoxels = workDim.x*workDim.y*workDim.z;

  // Pad the input image to the working dimensions
  float *inImagePad = (float *) malloc(sizeof(float)*workVoxels);
  int zeroShift[] = {0, 0, 0};
  float fillValue = 0.0f;
  Clarity_ImagePadSpatialShift(inImagePad, workDim, inImage, imageDim,
			       zeroShift, fillValue);

  // Pad the kernel to the working dimensions and shift it so that the
  // center of the kernel is at the origin.
  float *kernelPad = (float *) malloc(sizeof(float)*workVoxels);
  int kernelShift[] = {-kernelDim.x/2, -kernelDim.y/2, -kernelDim.z/2};
  Clarity_ImagePadSpatialShift(kernelPad, workDim, kernel, kernelDim,
			       kernelShift, fillValue);

  // Allocate output array
  float *outImagePad = (float *) malloc(sizeof(float)*workVoxels);

#ifdef TIME
   totalTimer.Start();
#endif
   ClarityResult_t result = CLARITY_SUCCESS;

   // Copy over the input image and PSF.
#ifdef BUILD_WITH_CUDA
   if (g_CUDACapable) {
      float* in;
      float* psf;
      
      result = Clarity_Real_MallocCopy((void**) &in, sizeof(float), 
         workDim.x, workDim.y, workDim.z, inImagePad);
      if (result != CLARITY_SUCCESS) {
         return result;
      }
      result = Clarity_Real_MallocCopy((void **) &psf, 
         sizeof(float), workDim.x, workDim.y, workDim.z, kernelPad);
      if (result != CLARITY_SUCCESS) {
         Clarity_Free(in);
         return result;
      }
      Clarity_ConvolveInternal(workDim.x, workDim.y, workDim.z, in, psf, in);
      result = Clarity_CopyFromDevice(workDim.x, workDim.y, workDim.z, 
				      sizeof(float), outImagePad, in);
      Clarity_Free(in); Clarity_Free(psf);

   } else 
#endif // BUILD_WITH_CUDA
   {

      Clarity_ConvolveInternal(workDim.x, workDim.y, workDim.z, inImagePad, 
			       kernelPad, outImagePad);
   }


   // Clip the image to the original dimensions.
   Clarity_ImageClip(outImage, imageDim, outImagePad, workDim);

   // Free up memory
   free(inImagePad);
   free(kernelPad);
   free(outImagePad);

#ifdef TIME
   totalTimer.Stop();
   std::cout << totalTimer << std::endl;
   std::cout << transferTimer << std::endl;
   totalTimer.Reset();
   transferTimer.Reset();
#endif

   return result;
}


ClarityResult_t
Clarity_Convolve_OTF(
   int nx, int ny, int nz, float* in, float* otf, float* out) {

   ClarityResult_t result = CLARITY_SUCCESS;

   float* inFT = NULL;
   result = Clarity_Complex_Malloc((void**) &inFT, sizeof(float), 
      nx, ny, nz);
   if (result == CLARITY_OUT_OF_MEMORY) {
      return result;
   }

   result = Clarity_FFT_R2C_float(nx, ny, nz, in, inFT);
   if (result != CLARITY_SUCCESS) {
      Clarity_Free(inFT);
      return result;
   }

   Clarity_Modulate(nx, ny, nz, inFT, otf, inFT);

   result = Clarity_FFT_C2R_float(nx, ny, nz, inFT, out);
   Clarity_Free(inFT);

   return result;
}


ClarityResult_t
Clarity_ConvolveInternal(
   int nx, int ny, int nz, float* in, float* psf, float* out) {

   ClarityResult_t result = CLARITY_SUCCESS;
   float* inFT = NULL;
   result = Clarity_Complex_Malloc((void**) &inFT, sizeof(float), 
      nx, ny, nz);
   if (result != CLARITY_SUCCESS) { 
      return result;
   }
   result = Clarity_FFT_R2C_float(nx, ny, nz, in, inFT);
   if (result != CLARITY_SUCCESS) {
      Clarity_Free(inFT);
      return result;
   }

   float* psfFT = NULL;
   result = Clarity_Complex_Malloc((void**) &psfFT, sizeof(float),
      nx, ny, nz);
   if (result != CLARITY_SUCCESS) {
      Clarity_Free(inFT);
      return result;
   }
   result = Clarity_FFT_R2C_float(nx, ny, nz, psf, psfFT);
   if (result != CLARITY_SUCCESS) {
      Clarity_Free(inFT); Clarity_Free(psfFT);
      return result;
   }

   // Modulate the two transforms
   Clarity_Modulate(nx, ny, nz, inFT, psfFT, inFT);
   Clarity_Free(psfFT);

   result = Clarity_FFT_C2R_float(nx, ny, nz, inFT, out);
   Clarity_Free(inFT);

   return result;
}


#ifdef BUILD_WITH_CUDA
extern "C"
void
Clarity_Modulate_KernelGPU(
   int nx, int ny, int nz, float* inFT, float* otf, float* outFT);
#endif


void
Clarity_Modulate_KernelCPU(
   int nx, int ny, int nz, float* inFT, float* otf, float* outFT) {
   int numVoxels = nz*ny*(nx/2 + 1);
   float scale = 1.0f / ((float) nz*ny*nx);
#pragma omp parallel for
   for (int i = 0; i < numVoxels; i++) {
      ComplexMultiplyAndScale(inFT + (2*i), otf + (2*i), scale, outFT + (2*i));
   }
}


void
Clarity_Modulate(
   int nx, int ny, int nz, float* in, float* otf, float* out) {

#ifdef BUILD_WITH_CUDA
   if (g_CUDACapable) {
      Clarity_Modulate_KernelGPU(nx, ny, nz, in, otf, out);
   } else
#endif // BUILD_WITH_CUDA
   {
      Clarity_Modulate_KernelCPU(nx, ny, nz, in, otf, out);
   }
}
