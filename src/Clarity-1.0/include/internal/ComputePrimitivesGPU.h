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
 * File name: ComputePrimitivesGPU.h
 * Author: Cory Quammen <cquammen@cs.unc.edu>
 */


#ifndef __COMPUTE_PRIMITIVES_GPU_H_
#define __COMPUTE_PRIMITIVES_GPU_H_


/**
 * Sums the elements of an array on the GPU.
 *
 * @param result Single-element return parameter containing 
  *              result of reduction.
 * @param buffer Array of values to sum.
 * @param n      Number of float values to sum.
 */
extern "C"
void
Clarity_ReduceSumGPU(float* result, float* buffer, int n);

/**
 * Multiplies two arrays together component-wise.
 *
 * @param result The multiplied array.
 * @param a      First input array.
 * @param b      Second input array.
 * @param n      Length of arrays.
 */
extern "C"
void
Clarity_MultiplyArraysComponentWiseGPU(
   float* result, float* a, float* b, int n);


/**
 * Divides two arrays together component-wise.
 *
 * @param result The multiplied array.
 * @param a      First input array.
 * @param b      Second input array.
 * @param value  Value for result if denominator is zero.
 * @param n      Length of arrays.
 */
extern "C"
void
Clarity_DivideArraysComponentWiseGPU(
   float* result, float* a, float* b, float value, int n);


/**
 * Scales an array of real values by a constant.
 *
 * @param result The scaled array.
 * @param a      Array to scale.
 * @param n      Length of array.
 * @param scale  Scale factor.
 */
extern "C"
void
Clarity_ScaleArrayGPU(
   float* result, float* a, int n, float scale);


#endif // __COMPUTE_PRIMITIVES_GPU_H_
