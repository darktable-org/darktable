/*
 *  This file is part of darktable,
 *  copyright (c) 2009--2013 johannes hanika.
 *  copyright (c) 2014 Ulrich Pegelow.
 *  copyright (c) 2014 LebedevRI.
 *
 *  darktable is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  darktable is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with darktable.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "common.h"

/* kernel for the interpolation resample helper */
kernel void
interpolation_resample (read_only image2d_t in, write_only image2d_t out, const int width, const int height,
                        const global int *hmeta, const global int *vmeta,
                        const global int *hlength, const global int *vlength,
                        const global int *hindex, const global int *vindex,
                        const global float *hkernel, const global float *vkernel,
                        const int htaps, const int vtaps,
                        local float *lkernel, local int *lindex,
                        local float4 *buffer)
{
  const int x = get_global_id(0);
  const int yi = get_global_id(1);
  const int ylsz = get_local_size(1);
  const int xlid = get_local_id(0);
  const int ylid = get_local_id(1);
  const int y = yi / vtaps;
  const int iy = yi % vtaps;

  // Initialize resampling indices
  const int xm = min(x, width - 1);
  const int ym = min(y, height - 1);
  const int hlidx = hmeta[xm*3];   // H(orizontal) L(ength) I(n)d(e)x
  const int hkidx = hmeta[xm*3+1]; // H(orizontal) K(ernel) I(n)d(e)x
  const int hiidx = hmeta[xm*3+2]; // H(orizontal) I(ndex) I(n)d(e)x
  const int vlidx = vmeta[ym*3];   // V(ertical) L(ength) I(n)d(e)x
  const int vkidx = vmeta[ym*3+1]; // V(ertical) K(ernel) I(n)d(e)x
  const int viidx = vmeta[ym*3+2]; // V(ertical) I(ndex) I(n)d(e)x

  const int hl = hlength[hlidx];   // H(orizontal) L(ength)
  const int vl = vlength[vlidx];   // V(ertical) L(ength)

  // generate local copy of horizontal index field and kernel
  for(int n = 0; n <= htaps/ylsz; n++)
  {
    int k = mad24(n, ylsz, ylid);
    if(k >= hl) continue;
    lindex[k] = hindex[hiidx+k];
    lkernel[k] = hkernel[hkidx+k];
  }

  barrier(CLK_LOCAL_MEM_FENCE);

  // horizontal convolution kernel; store intermediate result in local buffer
  if(x < width && y < height)
  {
    const int yvalid = iy < vl;

    const int yy = yvalid ? vindex[viidx+iy] : -1;

    float4 vpixel = (float4)0.0f;

    for (int ix = 0; ix < hl && yvalid; ix++)
    {
      const int xx = lindex[ix];
      float4 hpixel = read_imagef(in, sampleri,(int2)(xx, yy));
      vpixel += hpixel * lkernel[ix];
    }

    buffer[ylid] = yvalid ? vpixel * vkernel[vkidx+iy] : (float4)0.0f;
  }
  else
    buffer[ylid] = (float4)0.0f;

  barrier(CLK_LOCAL_MEM_FENCE);

  // recursively reduce local buffer (vertical convolution kernel)
  for(int offset = vtaps / 2; offset > 0; offset >>= 1)
  {
    if (iy < offset)
    {
      buffer[ylid] += buffer[ylid + offset];
    }
    barrier(CLK_LOCAL_MEM_FENCE);
  }

  // store final result
  if (iy == 0 && x < width && y < height)
  {
    write_imagef (out, (int2)(x, y), buffer[ylid]);
  }
}
