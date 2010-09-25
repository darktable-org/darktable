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
 * File name: WienerDeconvolveGPU.h
 * Author: Cory Quammen <cquammen@cs.unc.edu>
 */


#ifndef __WIENER_DECONVOLVE_GPU_H_
#define __WIENER_DECONVOLVE_GPU_H_

#include "ComplexCUDA.h"

/**
 * Configures and launches the device function for Wiener
 * filter deconvolution.
 *
 * @param nx      Size in x-dimension of inFT, psfFT, and outFT.
 * @param ny      Size in y-dimension of inFT, psfFT, and outFT.
 * @param nz      Size in z-dimension of inFT, psfFT, and outFT.
 * @param inFT    Fourier transform of the input image.
 * @param psfFT   Fourier transform of the PSF.
 * @param outFT   Fourier transform of the result of the Wiener
 *                filter.
 * @param epsilon Smoothing factor.
 */
extern "C"
void
WienerDeconvolveKernelGPU(
   int nx, int ny, int nz, float* inFT, float* psfFT, 
   float* outFT, float epsilon);


#endif // __WIENER_DECONVOLVE_GPU_H_
