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
 * File name: WienerDeconvolve.cxx
 * Author: Cory Quammen <cquammen@cs.unc.edu>
 */


#include "Clarity.h"

#include <iostream>
#include <omp.h>
#include <fftw3.h>
#include <math.h>

#include "Complex.h"
#include "FFT.h"
#include "Memory.h"

#ifdef TIME
#include <iostream>
#include "Stopwatch.h"

static Stopwatch totalTimer("Wiener filter (total time)");
static Stopwatch transferTimer("Wiener filter (transfer time)");
#endif

extern bool g_CUDACapable;


void
WienerDeconvolveKernelCPU(
   int nx, int ny, int nz, float* inFT, float* psfFT, 
   float* result, float epsilon) {

   int numVoxels = nz*ny*(nx/2 + 1);
   float scale = 1.0f / ((float) nz*ny*nx);

   // From Sibarita, "Deconvolution Microscopy"
#pragma omp parallel for
   for (int i = 0; i < numVoxels; i++) {
      float H[2] = {psfFT[2*i + 0], psfFT[2*i + 1]};
      float HConj[2];
      ComplexConjugate(H, HConj);
      float HMagSquared = ComplexMagnitudeSquared(H);
      ComplexMultiply(HConj, 1.0f / (HMagSquared + epsilon), HConj);
      ComplexMultiplyAndScale(HConj, inFT + (2*i), scale, result + (2*i));
   }
}


ClarityResult_t
Clarity_WienerDeconvolveCPU(
   float* outImage, float* inImage, float* psfImage,
   int nx, int ny, int nz, float epsilon) {

   ClarityResult_t result = CLARITY_SUCCESS;

   // Forward Fourier transform of input image.
   float* inFT = NULL;
   result = Clarity_Complex_Malloc((void**) &inFT, sizeof(float), 
      nx, ny, nz);
   if (result != CLARITY_SUCCESS) {
      return result;
   }
   result = Clarity_FFT_R2C_float(nx, ny, nz, inImage, inFT);
   if (result != CLARITY_SUCCESS) {
      Clarity_Free(inFT);
      return result;
   }
   
   // Fourier transform of PSF.
   float* psfFT = NULL;
   result = Clarity_Complex_Malloc((void**) &psfFT, sizeof(float), 
      nx, ny, nz);
   if (result != CLARITY_SUCCESS) {
      Clarity_Free(inFT);
      return CLARITY_OUT_OF_MEMORY;
   }
   result = Clarity_FFT_R2C_float(nx, ny, nz, psfImage, psfFT);
   if (result != CLARITY_SUCCESS) {
      Clarity_Free(inFT); Clarity_Free(psfFT);
      return result;
   }

   WienerDeconvolveKernelCPU(nx, ny, nz, inFT, psfFT, inFT, epsilon);

   result = Clarity_FFT_C2R_float(nx, ny, nz, inFT, outImage);

   Clarity_Free(inFT);
   Clarity_Free(psfFT);

   return result;
}


#ifdef BUILD_WITH_CUDA

#include "WienerDeconvolveGPU.h"


ClarityResult_t
Clarity_WienerDeconvolveGPU(
   float* outImage, float* inImage, float* psfImage,
   int nx, int ny, int nz, float epsilon) {

   ClarityResult_t result = CLARITY_SUCCESS;

   // Send PSF image.
   float* psf = NULL;
   result = Clarity_Real_MallocCopy((void**) &psf, sizeof(float),
      nx, ny, nz, psfImage);
   if (result != CLARITY_SUCCESS) {
      return result;
   }

   // Fourier transform of PSF.
   float* psfFT = NULL;
   result = Clarity_Complex_Malloc((void**) &psfFT, sizeof(float), 
      nx, ny, nz);
   if (result != CLARITY_SUCCESS) {
      Clarity_Free(psf);
      return result;
   }
   result = Clarity_FFT_R2C_float(nx, ny, nz, psf, psfFT);
   if (result != CLARITY_SUCCESS) {
      Clarity_Free(psf); Clarity_Free(psfFT);
      return result;
   }
   Clarity_Free(psf); // Don't need this anymore.

   // Send input image.
   float* in = NULL;
   result = Clarity_Real_MallocCopy((void**) &in, sizeof(float), 
      nx, ny, nz, inImage);
   if (result != CLARITY_SUCCESS) {
      Clarity_Free(psfFT);
      return result;
   }

   // Forward Fourier transform of input image.
   float* inFT = NULL;
   result = Clarity_Complex_Malloc((void**) &inFT, sizeof(float), 
      nx, ny, nz);
   if (result != CLARITY_SUCCESS) {
      Clarity_Free(psfFT); Clarity_Free(in);
      return result;
   }
   result = Clarity_FFT_R2C_float(nx, ny, nz, in, inFT);
   if (result != CLARITY_SUCCESS) {
      Clarity_Free(psfFT); Clarity_Free(in); Clarity_Free(inFT);
      return result;
   }

   // Apply Wiener filter
   WienerDeconvolveKernelGPU(nx, ny, nz, inFT, psfFT, inFT, 
      epsilon);

   result = Clarity_FFT_C2R_float(nx, ny, nz, inFT, in);
   
   // Read back
   result = Clarity_CopyFromDevice(nx, ny, nz, sizeof(float), 
      outImage, in);

   return result;
}

#endif // BUILD_WITH_CUDA


ClarityResult_t 
Clarity_WienerDeconvolve(float* inImage, Clarity_Dim3 imageDim,
			 float* kernelImage, Clarity_Dim3 kernelDim,
			 float* outImage, float epsilon) {

  ClarityResult_t result = CLARITY_SUCCESS;

#ifdef TIME
  totalTimer.Start();
#endif

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
  float *kernelImagePad = (float *) malloc(sizeof(float)*workVoxels);
  int kernelShift[] = {-kernelDim.x/2, -kernelDim.y/2, -kernelDim.z/2};
  Clarity_ImagePadSpatialShift(kernelImagePad, workDim, kernelImage, kernelDim,
			       kernelShift, fillValue);

  // Allocate output array
  float *outImagePad = (float *) malloc(sizeof(float)*workVoxels);

#ifdef BUILD_WITH_CUDA
  if (g_CUDACapable) {
    result = Clarity_WienerDeconvolveGPU(outImagePad, inImagePad,
					 kernelImagePad,
					 workDim.x, workDim.y, workDim.z, 
					 epsilon);
  } else
#endif // BUILD_WITH_CUDA
  {
    result = Clarity_WienerDeconvolveCPU(outImagePad, inImagePad,
					 kernelImagePad,
					 workDim.x, workDim.y, workDim.z, 
					 epsilon);
   }

   // Clip the image to the original dimensions.
   Clarity_ImageClip(outImage, imageDim, outImagePad, workDim);

  // Free up memory.
  free(inImagePad);
  free(kernelImagePad);
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
