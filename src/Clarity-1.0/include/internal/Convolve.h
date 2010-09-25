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
 * File name: Convolve.h
 * Author: Cory Quammen <cquammen@cs.unc.edu>
 */


#ifndef __CONVOLVE_H_
#define __CONVOLVE_H_

/** 
 * Convolution function with pre-Fourier-transformed kernel,
 * sometimes called the optical transfer function (OTF). 
 * 
 * @param nx  X-dimension of in, otf, and out.
 * @param ny  Y-dimension of in, otf, and out.
 * @param nz  Z-dimension of in, otf, and out.
 * @param in  Real input image.
 * @param otf Optical transfer function (Fourier transform of
 *            convolution kernel.
 * @param out Resulting real image.
 * 
 */
ClarityResult_t
Clarity_Convolve_OTF(
   int nx, int ny, int nz, float* in, float* otf, float* out);


/** 
 * Internal convolution function.
 * 
 * @param nx     X-dimension of in, kernel, and out.
 * @param ny     Y-dimension of in, kernel, and out.
 * @param nz     Z-dimension of in, kernel, and out.
 * @param in     Real input image.
 * @param kernel Real convolution kernel.
 * @param out    Resulting real image.
 */
ClarityResult_t
Clarity_ConvolveInternal(
   int nx, int ny, int nz, float* in, float* kernel, float* out);


/** Per-pixel modulation of one transformed image with another.
 *
 * @param nx  X-dimension of in, otf, and out.
 * @param ny  Y-dimension of in, otf, and out.
 * @param nz  Z-dimension of in, otf, and out.
 * @param in  Complex input image.
 * @param otf Modulating complex optical transfer function.
 * @param out Output of modulation.
 */
void
Clarity_Modulate(
   int nx, int ny, int nz, float* in, float* otf, float* out);


#endif // __CONVOLVE_H_
