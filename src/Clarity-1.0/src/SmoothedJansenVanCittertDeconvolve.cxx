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
 * File name: SmoothedJansenVanCittertDeconvolve.cxx
 * Author: Cory Quammen <cquammen@cs.unc.edu>
 */


#include "Clarity.h"

ClarityResult_t
Clarity_SmoothedJansenVanCittertDeconvolve(
   float* outImage, float* inImage, float* psfImage,
   Clarity_Dim3 dim, unsigned iterations, 
   unsigned smoothInterval, float smoothSigma[3]) {

   // Temporary code to produce something.
   int size = dim.x * dim.y * dim.z;
   for (int i = 0; i < size; i++) {
      outImage[i] = 0.0f;
   }

   return CLARITY_SUCCESS;
}
