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
 * File name: JansenVanCittertDeconvolveGPU.h
 * Author: Cory Quammen <cquammen@cs.unc.edu>
 */


#ifndef __JANSEN_VAN_CITTERT_DECONVOLVE_H_
#define __JANSEN_VAN_CITTERT_DECONVOLVE_H_

/**
 * Function to invoke computation kernel on the GPU.
 * 
 * @param nx       X-dimension of in, i_k, o_k, and i_kNext.
 * @param ny       Y-dimension of in, i_k, o_k, and i_kNext.
 * @param nz       Z-dimension of in, i_k, o_k, and i_kNext.
 * @param in       Real input image.
 * @param inMax    Half the maximum value in the input image.
 * @param invMaxSq Inverse of half the maximum squared value in the input
 *                 image.
 * @param i_k      Current guess of the uncorrupted image.
 * @param o_k      Storage pointer.
 * @param i_kNext  Next guess of the corrupted image.
 */
extern "C"
void
JansenVanCittertDeconvolveKernelGPU(
   int nx, int ny, int nz, float* in, float inMax, float invMaxSq,
   float* i_k, float* o_k, float* i_kNext);


#endif // __JANSEN_VAN_CITTERT_DECONVOLVE_H_
