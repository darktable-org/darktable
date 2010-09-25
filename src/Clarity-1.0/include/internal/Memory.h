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
 * File name: Memory.h
 * Author: Cory Quammen <cquammen@cs.unc.edu>
 */


#ifndef __MEMORY_H_
#define __MEMORY_H_

#include <cstring>

/**
 * Allocates a 3D complex-valued image of given size.
 * 
 * @param buffer Return parameter for allocated buffer.
 * @param size   Size of the buffer to allocate.
 * @param nx     X-dimension of the image to allocate.
 * @param ny     Y-dimension of the image to allocate.
 * @param nz     Z-dimension of the image to allocate.
 */
ClarityResult_t
Clarity_Complex_Malloc(
   void** buffer, size_t size, int nx, int ny, int nz);


/**
 * Allocates a 3D real-valued image of given size.
 * 
 * @param buffer Return parameter for allocated buffer.
 * @param size   Size of the buffer to allocate.
 * @param nx     X-dimension of the image to allocate.
 * @param ny     Y-dimension of the image to allocate.
 * @param nz     Z-dimension of the image to allocate.
 */
ClarityResult_t
Clarity_Real_Malloc(
   void** buffer, size_t size, int nx, int ny, int nz);


/**
 * Copies an image from the CPU to the GPU device.
 * 
 * @param nx   X-dimension of the image to copy.
 * @param ny   Y-dimension of the image to copy.
 * @param nz   Z-dimension of the image to copy.
 * @param size Size of the data element being copied.
 * @param dst  Destination buffer pre-allocated on the device.
 * @param src  Source buffer on the CPU.
 */
ClarityResult_t
Clarity_CopyToDevice(
   int nx, int ny, int nz, size_t size, void* dst, void* src);


/**
 * Copies an image from the GPU device to the CPU.
 * 
 * @param nx   X-dimension of the image to copy.
 * @param ny   Y-dimension of the image to copy.
 * @param nz   Z-dimension of the image to copy.
 * @param size Size of the data element being copied.
 * @param dst  Destination buffer pre-allocated on the CPU.
 * @param src  Source buffer on the GPU device.
 */
ClarityResult_t
Clarity_CopyFromDevice(
   int nx, int ny, int nz, size_t size, void* dst, void* src);


/**
 * Allocates a memory buffer on the GPU device and then copies data from
 * a real-valued image on the CPU to it.
 * 
 * @param buffer Return parameter pointing to allocated buffer.
 * @param size Size of the data element being allocated and copied.
 * @param nx   X-dimension of the image to copy.
 * @param ny   Y-dimension of the image to copy.
 * @param nz   Z-dimension of the image to copy.
 * @param src  Source buffer on the CPU holding real-valued image of size
 *             nx*ny*nz.
 */
ClarityResult_t
Clarity_Real_MallocCopy(
   void** buffer, size_t size, int nx, int ny, int nz, void* src);


/**
 * Frees memory allocated by Clarity_Complex_Malloc, Clarity_Real_Malloc,
 * and Clarity_Real_MallocCopy.
 * 
 * @param buffer Buffer to free.
 */
void
Clarity_Free(void* buffer);


#endif //__MEMORY_H_
