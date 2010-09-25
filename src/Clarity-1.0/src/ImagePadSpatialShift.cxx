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
 * File name: ImagePadSpatialShift.cxx
 * Author: Cory Quammen <cquammen@cs.unc.edu>
 */


#include "Clarity.h"

#include <malloc.h>

// WARNING! Only the CPU side is provided here because padding and
// shifting is a low-frequency operation.
// Assumes adequate CPU side memory has been allocated in dst.
ClarityResult_t
Clarity_ImagePadSpatialShift(
   float *dst, Clarity_Dim3 dstDim, 
   float *src, Clarity_Dim3 srcDim,
   int shift[3], float fillValue) {

   if (dst == NULL || src == NULL) {
      return CLARITY_INVALID_ARGUMENT;
   }

   for (int dk = 0; dk < dstDim.z; dk++) {
      int sk = dk - shift[2];
      if (sk < 0) sk += dstDim.z;
      sk = sk % dstDim.z;
      bool withinK = sk >= 0 && sk < srcDim.z;
      for (int dj = 0; dj < dstDim.y; dj++) {
         int sj = dj - shift[1];
         if (sj < 0) sj += dstDim.y;
         sj = sj % dstDim.y;
         bool withinJ = sj >= 0 && sj < srcDim.y;
         for (int di = 0; di < dstDim.x; di++) {
            int si = di - shift[0];
            if (si < 0) si += dstDim.x;
            si = si % dstDim.x;
            bool withinI = si >= 0 && si < srcDim.x;
            int dIndex = (dk*dstDim.y*dstDim.x) + (dj*dstDim.x) + di;
            if (withinI && withinJ && withinK) {
               int sIndex = (sk*srcDim.y*srcDim.x) + (sj*srcDim.x) + si;
               dst[dIndex] = src[sIndex];
            } else {
               dst[dIndex] = fillValue;
            }
         }
      }
   }

   return CLARITY_SUCCESS;
}
