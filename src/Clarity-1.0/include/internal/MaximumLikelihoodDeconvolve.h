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
 * File name: MaximumLikelihoodDeconvolve.h
 * Author: Cory Quammen <cquammen@cs.unc.edu>
 */


#ifndef __MAXIMUM_LIKELIHOOD_DECONVOLVE_H_
#define __MAXIMUM_LIKELIHOOD_DECONVOLVE_H_

#include "Clarity.h"


/**
 * Update step of the maximum likelihood algorithm.
 * 
 * @param nx           X-dimension of in, currentGuess, otf, newGuess.
 * @param ny           Y-dimension of in, currentGuess, otf, newGuess.
 * @param nz           Z-dimension of in, currentGuess, otf, newGuess.
 * @param in           Original real-valued image.
 * @param energy       Original energy of the image 'in'.
 * @param currentGuess Current guess of the uncorrupted image.
 * @param otf          Fourier transform of the convolution kernel.
 * @param s1           Temporary storage buffer big enough to store
 *                     real-valued image of dimensions nx*ny*nz.
 * @param s2           Temporary storage buffer the size of s1.
 * @param newGuess     Real-valued result of the function corresponding to
 *                     the next best guess of the uncorrupted image.
 */
ClarityResult_t
Clarity_MaximumLikelihoodUpdate(
   int nx, int ny, int nz, float* in, float energy,
   float* currentGuess, float* otf, float* s1, float* s2,
   float* newGuess);


#endif // __MAXIMUM_LIKELIHOOD_DECONVOLVE_H_
