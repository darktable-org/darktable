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
 * File name: FFT.h
 * Author: Cory Quammen <cquammen@cs.unc.edu>
 */


#ifndef __FFT_H_
#define __FFT_H_

#include "Clarity.h"


/** 
 * 3D forward FFT function. 
 * 
 * @param nx  X-dimension of in and out.
 * @param ny  Y-dimension of in and out.
 * @param nz  Z-dimension of in and out.
 * @param in  Input real image.
 * @param out Output complex image without redundant coefficients.
 */
C_FUNC_DEF
ClarityResult_t
Clarity_FFT_R2C_float(
   int nx, int ny, int nz, float* in, float* out);


/** 
 * 3D inverse FFT function.
 * 
 * @param nx  X-dimension of in and out.
 * @param ny  Y-dimension of in and out.
 * @param nz  Z-dimension of in and out.
 * @param in  Input complex image without redundant coefficients.
 * @param out Output real image.
 */
C_FUNC_DEF
ClarityResult_t
Clarity_FFT_C2R_float(
   int nx, int ny, int nz, float* in, float* out);


#endif // __FFT_H_
