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
 * File name: Test_Common.h
 * Author: Cory Quammen <cquammen@cs.unc.edu>
 */


#ifndef __TEST_COMMON_H_
#define __TEST_COMMON_H_

#include <cmath>
#include <cstdlib>
#include <cstdio>

#define IMG_X 128
#define IMG_Y 128
#define IMG_Z 32

#define PSF_X 32
#define PSF_Y 32
#define PSF_Z 32

/**
 * Generates image representing the true signal.
 * 
 * @param dim Return parameter for the dimensions of the true signal.
 * @return Image stored in memory allocated by malloc. The caller is
 * responsible for free'ing this memory.
 */
float *
Test_GenerateTrueImage(Clarity_Dim3 *dim) {
  dim->x = IMG_X;  dim->y = IMG_Y;  dim->z = IMG_Z;

  float *image = (float *) malloc(sizeof(float)*dim->x*dim->y*dim->z);

  // Initialize with zeros.
  for (int i = 0; i < dim->x*dim->y*dim->z; i++) {
    image[i] = 0.0f;
  }

  for (int iz = IMG_Z/4; iz < IMG_Z - (IMG_Z/4); iz++) {
    for (int iy = IMG_Y/4; iy < IMG_Y - IMG_Y/4; iy++) {
      for (int ix = IMG_X/4; ix < IMG_X - IMG_X/4; ix++) {
        image[iz*IMG_X*IMG_Y + iy*IMG_X + ix] = 1.0f;
      }
    }
  }

  return image;
}


/**
 * Generates image representing a Gaussian convolution kernel.
 *
 * @param dim   Return parameter for the dimensions of the kernel.
 * @param sigma Standard deviation of blurring kernel.
 * @return Kernel image stored in memory allocated by malloc. The caller is
 * responsible for free'ing this memory.
 */
float *
Test_GenerateGaussianKernel(Clarity_Dim3 *dim, float sigma) {
  dim->x = PSF_X;  dim->y = PSF_Y;  dim->z = PSF_Z;

  float *kernel = (float *) malloc(sizeof(float)*dim->x*dim->y*dim->z);

  float sum = 0.0f;
  float sigma2 = sigma*sigma;
  for (int iz = 0; iz < PSF_Z; iz++) {
    float fz = static_cast<float>(iz-(PSF_Z/2));
    for (int iy = 0; iy < PSF_Y; iy++) {
      float fy = static_cast<float>(iy-(PSF_Y/2));
      for (int ix = 0; ix < PSF_X; ix++) {
        float fx = static_cast<float>(ix-(PSF_X/2));
	float value =
          (1.0f / pow(2.0*M_PI*sigma2, 1.5)) *
          exp(-((fx*fx + fy*fy + fz*fz)/(2*sigma2)));
        kernel[(iz*PSF_X*PSF_Y) + (iy*PSF_X) + ix] = value;
	sum += value;
      }
    }
  }

  // Normalize the kernel
  float div = 1.0f / sum;
  for (int i = 0; i < PSF_X*PSF_Y*PSF_Z; i++) {
    kernel[i] *= div;
  }

  return kernel;

}


/**
 * Reports match between a known deconvolution solution and the
 * deconvolved image.
 *
 * @param inputImage       Known deconvolution solution.
 * @param deconvolvedImage Result of deconvolution algorithm.
 * @param imageDims        Dimensions of inputImage and deconvolvedImage.
 */
void
Test_ReportMatch(float* inputImage, float* deconvolvedImage,
		 Clarity_Dim3 imageDims) {
  float inputSum = 0.0f;
  float deconvolvedSum = 0.0f;
  float sum2 = 0.0f;
  for (int iz = 0; iz < imageDims.z; iz++) {
    for (int iy = 0; iy < imageDims.y; iy++) {
      for (int ix = 0; ix < imageDims.x; ix++) {
	int index = iz*imageDims.x*imageDims.y + iy*imageDims.x + ix;
	float diff = deconvolvedImage[index] - inputImage[index];
	sum2 += diff*diff;
	inputSum += inputImage[index];
	deconvolvedSum += deconvolvedImage[index];
      }
    }
  }

  printf("RMS is: %f\n", 
	 sqrt(sum2/static_cast<float>(imageDims.x*imageDims.y*imageDims.z)));
  printf("Difference in total intensity between images: %f\n",
	 inputSum - deconvolvedSum);

}


#endif // __TEST_COMMON_H_
