/*
    This file is part of darktable,
    copyright (c) 2009--2010 johannes hanika.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

const sampler_t sampleri =  CLK_NORMALIZED_COORDS_FALSE | CLK_ADDRESS_CLAMP_TO_EDGE | CLK_FILTER_NEAREST;
const sampler_t samplerf =  CLK_NORMALIZED_COORDS_FALSE | CLK_ADDRESS_CLAMP_TO_EDGE | CLK_FILTER_LINEAR;


int
FC(const int row, const int col, const unsigned int filters)
{
  return filters >> ((((row) << 1 & 14) + ((col) & 1)) << 1) & 3;
}

float4
ui4_to_float4(uint4 in)
{
  return (float4)(in.x/65535.0f, in.y/65535.0f, in.z/65535.0f, in.w/65535.0f);
}

int2
backtransformi (int2 p, const int r_x, const int r_y, const int r_wd, const int r_ht, const float r_scale)
{
  return (int2)((p.x + r_x)/r_scale, (p.y + r_y)/r_scale);
}

float2
backtransformf (float2 p, const int r_x, const int r_y, const int r_wd, const int r_ht, const float r_scale)
{
  return (float2)((p.x + r_x)/r_scale, (p.y + r_y)/r_scale);
}


#if 0
/**
 * convert gpu float4 format to cpu float*3 representation
 */
__kernel void
convert_float4_to_float3(__read_only image2d_t in, __global float *out, const int width, const int height)
{
  // TODO: so we really need 3 texture accesses per pixel??
  // TODO: read into shared mem and coalesce out?
  // TODO: fuck it, fast enough?
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  const int z = get_global_id(2);
  if(y < width && z < height)
  {
    float4 color = read_imagef(in, sampleri, (int2)(y, z));
    out[3*(width*z + y) + x] = color.x
  }
}
#endif

/**
 * downscale and clip a buffer (in) to the given roi (r_*) and write it to out.
 * output will be linear in memory.
 * operates on float4 -> float4 textures.
 */
__kernel void
clip_and_zoom(__read_only image2d_t in, __write_only image2d_t out,
              const int r_x, const int r_y, const int r_wd, const int r_ht, const float r_scale)
{
  // global id is pixel in output image (float4)
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  float4 color = (float4)(1.0f, 0.0f, 0.0f, 0.0f);

  const float px_footprint = .5f/r_scale;
  const int samples = ((int)px_footprint);
  float2 p = backtransformf((float2)(x, y), r_x, r_y, r_wd, r_ht, r_scale);
  for(int j=-samples;j<=samples;j++) for(int i=-samples;i<=samples;i++)
  {
    float4 px = read_imagef(in, samplerf, (float2)(p.x+i, p.y+j));
    color += px;
  }
  color /= (2*samples+1)*(2*samples+1);
  write_imagef (out, (int2)(x, y), color);
}

/**
 * downscales and clips a mosaiced buffer (in) to the given region of interest (r_*)
 * and writes it to out in float4 format.
 * filters is the dcraw supplied int encoding the bayer pattern.
 * resamping is done via rank-1 lattices and demosaicing using half-size interpolation.
 */
__kernel void
clip_and_zoom_demosaic_half_size(__read_only image2d_t in, __write_only image2d_t out,
    const int r_x, const int r_y, const int r_wd, const int r_ht, const float r_scale, const unsigned int filters)
{
  // global id is pixel in output image (float4)
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  float4 color = (float4)(0.0f, 0.0f, 0.0f, 0.0f);
  // adjust to pixel region and don't sample more than scale/2 nbs!
  // pixel footprint on input buffer, radius:
  const float px_footprint = .5f/r_scale;
  // how many 2x2 blocks can be sampled inside that area
  const int samples = ((int)px_footprint)/2;

  // init gauss with sigma = samples (half footprint)
  // float filter[2*samples + 1];
  // float sum = 0.0f;
  // for(int i=-samples;i<=samples;i++) sum += (filter[i+samples] = expf(-i*i/(samples*samples)));
  // for(int k=0;k<2*samples+1;k++) filter[k] /= sum;

  // upper left corner:
  const int2 p = backtransformi((int2)(x, y), r_x, r_y, r_wd, r_ht, r_scale);

  // round down to next even number:
  p.x &= ~0x1; p.y &= ~0x1;

  // now move p to point to an rggb block:
  if(FC(p.y, p.x+1, filters) != 1) p.x ++;
  if(FC(p.y, p.x,   filters) != 0) p.x ++;

  for(int j=-samples;j<=samples;j++) for(int i=-samples;i<=samples;i++)
  {
    // get four mosaic pattern uint16:
    uint4 p1 = read_imageui(in, sampleri, (int2)(p.x+2*i,   p.y+2*j  ));
    uint4 p2 = read_imageui(in, sampleri, (int2)(p.x+2*i+1, p.y+2*j  ));
    uint4 p3 = read_imageui(in, sampleri, (int2)(p.x+2*i,   p.y+2*j+1));
    uint4 p4 = read_imageui(in, sampleri, (int2)(p.x+2*i+1, p.y+2*j+1));
    // color += filter[j+samples]*filter[i+samples]*(float4)(p1.x/65535.0f, (p2.x+p3.x)/(2.0f*65535.0f), p4.x/65535.0f, 0.0f);
    color += (float4)(p1.x/65535.0f, (p2.x+p3.x)/(2.0f*65535.0f), p4.x/65535.0f, 0.0f);
  }
  color /= (2*samples+1)*(2*samples+1);
  write_imagef (out, (int2)(x, y), color);
}


/**
 * fill greens pass of pattern pixel grouping.
 * in (uint16_t) -> out (float4)
 */
__kernel void
ppg_demosaic_green (__read_only image2d_t in, __write_only image2d_t out, const unsigned int filters)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  // process all non-green pixels
  const int row = y;
  const int col = x;
  const int c = FC(row, col, filters);
  float4 color = (float4)(100.0f, 100.0f, 100.0f, 10000.0f); // output color

  const float4 pc   = ui4_to_float4(read_imageui(in, sampleri, (int2)(col, row)));

  if     (c == 0) color.x = pc.x; // red
  else if(c == 1) color.y = pc.x; // green1
  else if(c == 2) color.z = pc.x; // blue
  else            color.y = pc.x; // green2

  // fill green layer for red and blue pixels:
  if(c == 0 || c == 2)
  {
    // look up horizontal and vertical neighbours, sharpened weight:
    const float4 pym  = ui4_to_float4(read_imageui(in, sampleri, (int2)(col, row-1)));
    const float4 pym2 = ui4_to_float4(read_imageui(in, sampleri, (int2)(col, row-2)));
    const float4 pym3 = ui4_to_float4(read_imageui(in, sampleri, (int2)(col, row-3)));
    const float4 pyM  = ui4_to_float4(read_imageui(in, sampleri, (int2)(col, row+1)));
    const float4 pyM2 = ui4_to_float4(read_imageui(in, sampleri, (int2)(col, row+2)));
    const float4 pyM3 = ui4_to_float4(read_imageui(in, sampleri, (int2)(col, row+3)));
    const float4 pxm  = ui4_to_float4(read_imageui(in, sampleri, (int2)(col-1, row)));
    const float4 pxm2 = ui4_to_float4(read_imageui(in, sampleri, (int2)(col-2, row)));
    const float4 pxm3 = ui4_to_float4(read_imageui(in, sampleri, (int2)(col-3, row)));
    const float4 pxM  = ui4_to_float4(read_imageui(in, sampleri, (int2)(col+1, row)));
    const float4 pxM2 = ui4_to_float4(read_imageui(in, sampleri, (int2)(col+2, row)));
    const float4 pxM3 = ui4_to_float4(read_imageui(in, sampleri, (int2)(col+3, row)));
    const float guessx = (pxm.x + pc.x + pxM.x) * 2.0f - pxM2.x - pxm2.x;
    const float diffx  = (fabs(pxm2.x - pc.x) +
                          fabs(pxM2.x - pc.x) + 
                          fabs(pxm.x  - pxM.x)) * 3.0f +
                         (fabs(pxM3.x - pxM.x) + fabs(pxm3.x - pxm.x)) * 2.0f;
    const float guessy = (pym.x + pc.x + pyM.x) * 2.0f - pyM2.x - pym2.x;
    const float diffy  = (fabs(pym2.x - pc.x) +
                          fabs(pyM2.x - pc.x) + 
                          fabs(pym.x  - pyM.x)) * 3.0f +
                         (fabs(pyM3.x - pyM.x) + fabs(pym3.x - pym.x)) * 2.0f;
    if(diffx > diffy)
    {
      // use guessy
      const float m = fmin(pym.x, pyM.x);
      const float M = fmax(pym.x, pyM.x);
      color.y = fmax(fmin(guessy*.25f, M), m);
    }
    else
    {
      const float m = fmin(pxm.x, pxM.x);
      const float M = fmax(pxm.x, pxM.x);
      color.y = fmax(fmin(guessx*.25f, M), m);
    }
  }
  write_imagef (out, (int2)(x, y), color);
}

/**
 * fills the reds and blues in the gaps (done after ppg_demosaic_green).
 * in (float4) -> out (float4)
 */
__kernel void
ppg_demosaic_redblue (__read_only image2d_t in, __write_only image2d_t out, const unsigned int filters)
{
  // image in contains full green and sparse r b
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  const int row = y;
  const int col = x;
  const int c = FC(row, col, filters);
  float4 color = read_imagef(in, sampleri, (int2)(col, row));

  if(c == 1 || c == 3)
  { // calculate red and blue for green pixels:
    // need 4-nbhood:
    float4 nt = read_imagef(in, sampleri, (int2)(col, row-1));
    float4 nb = read_imagef(in, sampleri, (int2)(col, row+1));
    float4 nl = read_imagef(in, sampleri, (int2)(col-1, row));
    float4 nr = read_imagef(in, sampleri, (int2)(col+1, row));
    if(FC(row, col+1, filters) == 0) // red nb in same row
    {
      color.z = (nt.z + nb.z + 2.0f*color.y - nt.y - nb.y)*.5f;
      color.x = (nl.x + nr.x + 2.0f*color.y - nl.y - nr.y)*.5f;
    }
    else
    { // blue nb
      color.x = (nt.x + nb.x + 2.0f*color.y - nt.y - nb.y)*.5f;
      color.z = (nl.z + nr.z + 2.0f*color.y - nl.y - nr.y)*.5f;
    }
  }
  else
  {
    // get 4-star-nbhood:
    float4 ntl = read_imagef(in, sampleri, (int2)(col-1, row-1));
    float4 ntr = read_imagef(in, sampleri, (int2)(col+1, row-1));
    float4 nbl = read_imagef(in, sampleri, (int2)(col-1, row+1));
    float4 nbr = read_imagef(in, sampleri, (int2)(col+1, row+1));

    if(c == 0)
    { // red pixel, fill blue:
      const float diff1  = fabs(ntl.z - nbr.z) + fabs(ntl.y - color.y) + fabs(nbr.y - color.y);
      const float guess1 = ntl.z + nbr.z + 2.0f*color.y - ntl.y - nbr.y;
      const float diff2  = fabs(ntr.z - nbl.z) + fabs(ntr.y - color.y) + fabs(nbl.y - color.y);
      const float guess2 = ntr.z + nbl.z + 2.0f*color.y - ntr.y - nbl.y;
      if     (diff1 > diff2) color.z = guess2 * .5f;
      else if(diff1 < diff2) color.z = guess1 * .5f;
      else color.z = (guess1 + guess2)*.25f;
    }
    else // c == 2, blue pixel, fill red:
    {
      const float diff1  = fabs(ntl.x - nbr.x) + fabs(ntl.y - color.y) + fabs(nbr.y - color.y);
      const float guess1 = ntl.x + nbr.x + 2.0f*color.y - ntl.y - nbr.y;
      const float diff2  = fabs(ntr.x - nbl.x) + fabs(ntr.y - color.y) + fabs(nbl.y - color.y);
      const float guess2 = ntr.x + nbl.x + 2.0f*color.y - ntr.y - nbl.y;
      if     (diff1 > diff2) color.x = guess2 * .5f;
      else if(diff1 < diff2) color.x = guess1 * .5f;
      else color.x = (guess1 + guess2)*.25f;
    }
  }
  write_imagef (out, (int2)(x, y), color);
}

