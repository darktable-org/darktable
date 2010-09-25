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
 * File name: Clarity.h
 * Author: Cory Quammen <cquammen@cs.unc.edu>
 */


#ifndef __CLARITY_LIB_H_
#define __CLARITY_LIB_H_

/** \mainpage
<p>
Clarity is an open-source C/C++ library implementing many of the common deconvolution algorithms used in fluorescence microscopy. It is designed specifically for processing 3D images generated from optical sectioning.
</p>

<p>
Because deconvolution is a computationally intensive process, Clarity uses multithreaded algorithms to make full use of all the cores on modern multi-core computer systems. For even greater performance, the deconvolution algorithms can optionally run on commodity graphics processing units that feature hundreds of computing cores. Support for acceleration on graphics processing units is currently limited to NVIDIA graphics cards.
</p>

<p>
Please go to the Clarity Deconvolution Library <a href="http://cismm.cs.unc.edu/resources/software-manuals/clarity-deconvolution-library/">web page</a> for additional information and the latest updates.
</p>
*/

#ifdef __cplusplus
# if defined(WIN32) && defined(CLARITY_SHARED_LIB)
#  ifdef Clarity_EXPORTS
#   define C_FUNC_DEF extern "C" __declspec(dllexport)
#  else
#   define C_FUNC_DEF extern "C" __declspec(dllimport)
#  endif
# else
#  define C_FUNC_DEF extern "C"
# endif
#else
# if defined(WIN32) && defined(CLARITY_SHARED_LIB)
#  ifdef Clarity_EXPORTS
#   define C_FUNC_DEF __declspec(dllexport)
#  else
#   define C_FUNC_DEF __declspec(dllimport)
#  endif
# else
#  define C_FUNC_DEF
# endif
#endif

#ifndef NULL
#define NULL 0L
#endif

/** Enumerates the number and type of errors that 
   the Clarity library may produce. */
typedef enum {
   CLARITY_FFT_FAILED,            /** Fast Fourier transform routine 
                                   *  failed to execute. */
   CLARITY_OUT_OF_MEMORY,         /** Host system ran out of memory while 
                                   *  executing the function. */
   CLARITY_DEVICE_OUT_OF_MEMORY,  /** Computational accelerator ran out of
                                   *  memory while executing the function. */
   CLARITY_INVALID_OPERATION,     /** Operation is invalid for the arguments
                                   *  passed to it. */
   CLARITY_INVALID_ARGUMENT,      /** One or more of the arguments was invalid. */
   CLARITY_SUCCESS                /** Function executed successfully. */
} ClarityResult_t;


/*****************************/
/***** TYPES *****************/
/*****************************/

/** Type for specifying image 3D image dimensions. */
typedef struct _Clarity_Dim3_t {
   int x;
   int y;
   int z;
} Clarity_Dim3;


/** Creates a Clarity_Dim from a three-element integer array
 *  representing dimensions in x, y, and z.
 *
 * @param dimArray The three-element array.
 * @return The Clarity_Dim object.
 */
C_FUNC_DEF
Clarity_Dim3
Clarity_Dim3FromArray(int dimArray[3]);


/*****************************/
/***** UTILITY FUNCTIONS *****/
/*****************************/

/**
 * Clients should call this function prior to calling any other Clarity
 * function. Initializes underlying libraries and sets the number of
 * threads to the number of cores on the system.
 */
C_FUNC_DEF
ClarityResult_t
Clarity_Register();


/**
 * Clients should call this function when finished with the Clarity
 * library. It cleans up and releases resources used by the Clarity
 * library.
 */
C_FUNC_DEF
ClarityResult_t
Clarity_UnRegister();


/**
 * Sets the number of threads that should be used by the Clarity library.
 * Usually, you want this to be the same as the number of cores on the
 * CPU. By default, Clarity runs on a number of threads equal to the number
 * of cores on the CPU on which it is running.
 *
 * @param n Number of threads on which to run.
 */
C_FUNC_DEF
ClarityResult_t
Clarity_SetNumberOfThreads(unsigned n);


/**
 * Utility function to create a new image of a desired size containing the shifted
 * contents of the input image. Useful for padding and shifting convolution
 * kernels. 
 * 
 * @param dst       Destination buffer for shifted result. Buffer must be allocated
 *                  by the caller.
 * @param dstDim    Dimensions of the destination buffer.
 * @param src       Source buffer for image data to be shiftd.
 * @param srcDim    Dimensions of the source buffer.
 * @param shift     Three-element array corresponding the spatial shift 
                    in x, y, and z. Shifting operates cyclically across image 
                    boundaries.
 * @param fillValue Value to which pixels in parts of the new image not
 *                  corresponding to a shifted pixel get set.
 */
C_FUNC_DEF
ClarityResult_t
Clarity_ImagePadSpatialShift(float *dst, Clarity_Dim3 dstDim, 
                             float *src, Clarity_Dim3 srcDim,
                             int shift[3], float fillValue);

/**
 * Utility function to clip out a portion of an image. Useful for truncating the 
 * result of a convolution of a padded image.
 *
 * @param dst    Destination buffer for clipped result. Buffer must be allocated
 *               by the caller.
 * @param dstDim Dimensions of the destination buffer. Implicitly represents a 
                 coordinate in the source image. The clipped image corresponds to 
                 cropping the source image from the origin to coordinate (x, y, z).
 * @param src    Source buffer image to clip. Assumed to be larger or equal in size
                 in all three dimensions to the clipped image.
 * @param srcDim Dimensions of the source buffer.
 */
C_FUNC_DEF
ClarityResult_t
Clarity_ImageClip(float *dst, Clarity_Dim3 dstDim, float *src, Clarity_Dim3 srcDim);


/***********************************/
/***** DECONVOLUTION FUNCTIONS *****/
/***********************************/

/**
 * Applies a Wiener filter for deconvolution.
 *
 * @param inImage     Image to be deconvolved. Dimensions of this buffer are 
 *                    given by imageDim.
 * @param imageDim    Dimensions of inImage.
 * @param kernelImage Image of the blurring kernel of the system that produced
 *                    the image in inImage.
 * @param kernelDim   Dimensions of kernelImage.
 * @param outImage    Caller-allocated buffer holding result of Wiener filter.
 *                    Dimensions of this buffer are given by imageDim.
 * @param epsilon     Constant standing in place of the ratio between power 
 *                    spectra of noise and the power spectra of the underlying
 *                    image, which are unknown parameters. In practice, acts 
 *                    as a smoothing factor. Typically set in the range 
 *                    0.001 to 0.1.
 */
C_FUNC_DEF
ClarityResult_t 
Clarity_WienerDeconvolve(float* inImage, Clarity_Dim3 imageDim,
			 float* kernelImage, Clarity_Dim3 kernelDim,
			 float* outImage, float epsilon);


/**
 * Classic Jansen-van Cittert formulation for constrained iterative 
 * deconvolution.
 * 
 * @param inImage     Image to be deconvolved. Dimensions of this buffer are 
 *                    given by imageDim.
 * @param imageDim    Dimensions of inImage.
 * @param kernelImage Image of the blurring kernel of the system that produced
 *                    the image in inImage.
 * @param kernelDim   Dimensions of kernelImage.
 * @param outImage    Caller-allocated buffer holding result of Wiener filter.
 *                    Dimensions of this buffer are given by imageDim.
 * @param iterations  Number of algorithm iterations to run.
 */
C_FUNC_DEF
ClarityResult_t 
Clarity_JansenVanCittertDeconvolve(float* inImage, Clarity_Dim3 imageDim,
				   float* kernelImage, Clarity_Dim3 kernelDim,
				   float* outImage, int iterations);

/**
 * WARNING: This function's implementation is incomplete and produces undefined
 *  results.
 *
 * Implementation of the Jansen-van Cittert formulation for constrained
 * iterative deconvolution that applies a smoothing step every few iterations 
 * to reduce noise amplification.
 * 
 * @warning              Implementation incomplete.
 * @param inImage        Image to be deconvolved. Dimensions of this buffer 
 *                       are given by imageDim.
 * @param imageDim       Dimensions of inImage.
 * @param kernelImage    Image of the blurring kernel of the system that
 *                       produced the image in inImage.
 * @param kernelDim      Dimensions of kernelImage.
 * @param outImage       Caller-allocated buffer holding result of Wiener
 *                       filter. Dimensions of this buffer are given by
 *                       imageDim.
 * @param iterations     Number of algorithm iterations to run.
 * @param smoothInterval Iteration interval between applications of smoothing.
 * @param smoothSigma    Blurring Gaussian kernel parameters.
 */
C_FUNC_DEF
ClarityResult_t 
Clarity_SmoothedJansenVanCittertDeconvolve(
  float* inImage, Clarity_Dim3 imageDim, 
  float* kernelImage, Clarity_Dim3 kernelDim,
  float* outImage, unsigned iterations, unsigned smoothInterval,
  float* smoothSigma[3]);


/**
 * WARNING: This function's implementation is incomplete and produces 
 * undefined results.
 *
 * Unimplemented, but promising, deconvolution method based on the paper:
 * J. Markham and J.A. Conchello, Fast maximum-likelihood image-restoration
 * algorithms for three-dimensional fluorescence microscopy, J. Opt. Soc. Am. 
 * A, Vol. 18, No. 5, May 2001.
 *
 * @warning           Implementation incomplete.
 * @param inImage     Image to be deconvolved. Dimensions of this buffer 
 *                    are given by imageDim.
 * @param imageDim    Dimensions of inImage.
 * @param kernelImage Image of the blurring kernel of the system that
 *                    produced the image in inImage.
 * @param kernelDim   Dimensions of kernelImage.
 * @param outImage    Caller-allocated buffer holding result of Wiener
 *                    filter. Dimensions of this buffer are given by
 *                    imageDim.
 */
C_FUNC_DEF
ClarityResult_t
Clarity_IDivergenceDeconvolve(
  float* inImage, Clarity_Dim3 imageDim,
  float* kernelImage, Clarity_Dim3 kernelDim,
  float* outImage);


/**
 * Maximum-likelihood deconvolution method cited in the paper:
 * J.B. Sibarita, Deconvolution microscopy, Adv. Biochem. Engin./Biotechnology (2005) 95: 201-243.
 *
 * @param inImage     Image to be deconvolved. Dimensions of this image are
 *                    given by imageDim.
 * @param imageDim    Dimensions of the input image.
 * @param kernelImage Image of the point-spread function of the system that 
 *                    produced. Dimensions of this image are given by
 *                    psfDim.
 * @param kernelDim   Dimensions of the PSF image.
 * @param outImage    Caller-allocated buffer holding result of filter.
 *                    Dimensions of this buffer are given by imageDim.
 * @param iterations  Number of algorithm iterations to run.
 */
C_FUNC_DEF
ClarityResult_t
Clarity_MaximumLikelihoodDeconvolve(float* inImage, Clarity_Dim3 imageDim,
				    float* kernelImage, Clarity_Dim3 kernelDim,
				    float* outImage, int iterations);

/**
 * Blind maximum-likelihood deconvolution method cited in the paper:
 * J.B. Sibarita, Deconvolution microscopy, Adv. Biochem. Engin./Biotechnology (2005) 95: 201-243.
 *
 * @param outImage   Caller-allocated buffer holding result of Wiener filter.
 *                   Dimensions of this buffer are given by dim.
 * @param inImage    Image to be deconvolved. Dimensions of this buffer are
 *                   given by dim.
 * @param psfImage   Image of the point-spread function of the system that produced
 *                   the image in the outImage parameter.
 * @param dim        Dimension of outImage, inImage, and psfImage.
 * @param iterations Number of algorithm iterations to run.
 */
C_FUNC_DEF
ClarityResult_t
Clarity_BlindMaximumLikelihoodDeconvolve(float* outImage, float* inImage, float* psfImage,
                                         Clarity_Dim3 dim, unsigned iterations);


/*************************/
/* CONVOLUTION FUNCTIONS */
/*************************/

/**
 * Convolves two images of equal dimensions.
 * 
 * @param inImage   Real image to convolve.
 * @param imageDim  Dimensions of image to convolve.
 * @param kernel    Convolution kernel.
 * @param kernelDim Dimensions of convolution kernel.
 * @param outImage  Resulting real image of convolution of inImage and kernel.
 */
C_FUNC_DEF
ClarityResult_t
Clarity_Convolve(float* inImage, Clarity_Dim3 imageDim,
		 float* kernel, Clarity_Dim3 kernelDim,
		 float* outImage);


#endif // __CLARITY_LIB_H_
