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

int2
backtransformi (float2 p, const int r_x, const int r_y, const int r_wd, const int r_ht, const float r_scale)
{
  return (int2)((p.x + r_x)/r_scale, (p.y + r_y)/r_scale);
}

float2
backtransformf (float2 p, const int r_x, const int r_y, const int r_wd, const int r_ht, const float r_scale)
{
  return (float2)((p.x + r_x)/r_scale, (p.y + r_y)/r_scale);
}

__kernel void
green_equilibration(__read_only image2d_t in, __write_only image2d_t out, const unsigned int filters)
{
  const float thr = 0.01f;
  const float maximum = 1.0f;
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  const int c = FC(y, x, filters);

  const float o = read_imagef(in, sampleri, (int2)(x, y)).x;
  if(c == 1 && (y & 1))
  {
    const float o1_1 = read_imagef(in, sampleri, (int2)(x-1, y-1)).x;
    const float o1_2 = read_imagef(in, sampleri, (int2)(x+1, y-1)).x;
    const float o1_3 = read_imagef(in, sampleri, (int2)(x-1, y+1)).x;
    const float o1_4 = read_imagef(in, sampleri, (int2)(x+1, y+1)).x;
    const float o2_1 = read_imagef(in, sampleri, (int2)(x, y-2)).x;
    const float o2_2 = read_imagef(in, sampleri, (int2)(x, y+2)).x;
    const float o2_3 = read_imagef(in, sampleri, (int2)(x-2, y)).x;
    const float o2_4 = read_imagef(in, sampleri, (int2)(x+2, y)).x;

    const float m1 = (o1_1+o1_2+o1_3+o1_4)/4.0f;
    const float m2 = (o2_1+o2_2+o2_3+o2_4)/4.0f;
    if (m2 > 0.0f)
    {
      const float c1 = (fabs(o1_1-o1_2)+fabs(o1_1-o1_3)+fabs(o1_1-o1_4)+fabs(o1_2-o1_3)+fabs(o1_3-o1_4)+fabs(o1_2-o1_4))/6.0f;
      const float c2 = (fabs(o2_1-o2_2)+fabs(o2_1-o2_3)+fabs(o2_1-o2_4)+fabs(o2_2-o2_3)+fabs(o2_3-o2_4)+fabs(o2_2-o2_4))/6.0f;
      if((o<maximum*0.95f)&&(c1<maximum*thr)&&(c2<maximum*thr))
      {
        write_imagef (out, (int2)(x, y), o*m1/m2);
      }
      else write_imagef (out, (int2)(x, y), o);
    }
    else write_imagef (out, (int2)(x, y), o);
  }
  else write_imagef (out, (int2)(x, y), o);
}

constant int goffx[18] = { -2,  0,  2, -2,  0,  2, -2,  0,  2,   // r, b
                            0, -1,  1, -2,  0,  2, -1,  1,  0};  // green
constant int goffy[18] = { -2, -2, -2,  0,  0,  0,  2,  2,  2,   // r, b
                           -2, -1, -1,  0,  0,  0,  1,  1,  2};  // green

__kernel void
pre_median(__read_only image2d_t in, __write_only image2d_t out, const unsigned int filters, const float thrs, const int f4)
{
  constant int (*offx)[9] = (constant int (*)[9])goffx;
  constant int (*offy)[9] = (constant int (*)[9])goffy;
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  const int c = FC(y, x, filters);
  const int c1 = c & 1;
  const float pix = read_imagef(in, sampleri, (int2)(x, y)).x;
  const float n1 = read_imagef(in, sampleri, (int2)(x-1, y)).x;
  const float n2 = read_imagef(in, sampleri, (int2)(x+1, y)).x;
  const float n3 = read_imagef(in, sampleri, (int2)(x, y+1)).x;
  const float n4 = read_imagef(in, sampleri, (int2)(x, y-1)).x;
  const float variation = fabs(n1 - n2) + fabs(n4 - n3)
                        + fabs(n1 - n3) + fabs(n2 - n4)
                        + fabs(n1 - n4) + fabs(n2 - n3);
  const float thrs2 = c1 ? thrs : 2*thrs;
  float med[9];

  // avoid branch divergence, use constant memory to bake mem accesses, use data-based fetches:
  int cnt = 9;
  for(int k=0;k<9;k++) med[k] = read_imagef(in, sampleri, (int2)(x+offx[c1][k], y+offy[c1][k])).x;
  for(int k=0;k<9;k++) if(fabs(med[k] - pix) > thrs)
  {
    med[k] += 64.0f;
    cnt --;
  }

  // sort:
  for (int i=0;i<8;i++) for(int ii=i+1;ii<9;ii++) if(med[i] > med[ii])
  {
    const float tmp = med[i];
    med[i] = med[ii];
    med[ii] = tmp;
  }
  float4 color = (float4)(0.0f);
  // const float cc = (cnt > 1 || variation > 0.06) ? med[(cnt-1)/2]) : med[4] - 64.0f;
  const float cc = (c1 || cnt > 1 || variation > 0.06) ? med[(cnt-1)/2] : med[4] - 64.0f;
  if(f4) ((float *)&color)[c] = cc;
  else   color.x              = cc;
  write_imagef (out, (int2)(x, y), color);
}

#if 0
__kernel void
color_smoothing(__read_only image2d_t in, __write_only image2d_t out)
{
  // TODO: load image block into shared memory
  // TODO: median filter this - 1 px border
  // TODO: output whole block..?


  float med[9];
  // TODO: put to constant memory:
  const uint8_t opt[] = /* Optimal 9-element median search */
  { 1,2, 4,5, 7,8, 0,1, 3,4, 6,7, 1,2, 4,5, 7,8,
    0,3, 5,8, 4,7, 3,6, 1,4, 2,5, 4,7, 4,2, 6,4, 4,2 };

  // TODO: get 9 nb pixels and store c-g
  // TODO: sort
  // TODO: synchthreads (block boundaries: bad luck)
  // TODO: push median
      for (int j=0;j<roi_out->height;j++) for(int i=0;i<roi_out->width;i++)
        out[4*(j*roi_out->width + i) + 3] = out[4*(j*roi_out->width + i) + c];
      for (int j=1;j<roi_out->height-1;j++) for(int i=1;i<roi_out->width-1;i++)
      {
        int k = 0;
        for (int jj=-1;jj<=1;jj++) for(int ii=-1;ii<=1;ii++) med[k++] = out[4*((j+jj)*roi_out->width + i + ii) + 3] - out[4*((j+jj)*roi_out->width + i + ii) + 1];
        for (int ii=0; ii < sizeof opt; ii+=2)
          if     (med[opt[ii]] > med[opt[ii+1]])
            SWAP (med[opt[ii]] , med[opt[ii+1]]);
        out[4*(j*roi_out->width + i) + c] = CLAMPS(med[4] + out[4*(j*roi_out->width + i) + 1], 0.0f, 1.0f);
      }
    }
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
  float4 color = (float4)(0.0f, 0.0f, 0.0f, 0.0f);

  const float px_footprint = .5f/r_scale;
  const int samples = ((int)px_footprint);
  float2 p = backtransformf((float2)(x+.5f, y+.5f), r_x, r_y, r_wd, r_ht, r_scale);
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
  const int2 p = backtransformi((float2)(x+.5f, y+.5f), r_x, r_y, r_wd, r_ht, r_scale);

  // round down to next even number:
  p.x &= ~0x1; p.y &= ~0x1;

  // now move p to point to an rggb block:
  if(FC(p.y, p.x+1, filters) != 1) p.x ++;
  if(FC(p.y, p.x,   filters) != 0) { p.x ++; p.y ++; }

  for(int j=-samples;j<=samples;j++) for(int i=-samples;i<=samples;i++)
  {
    // get four mosaic pattern uint16:
    float4 p1 = read_imagef(in, sampleri, (int2)(p.x+2*i,   p.y+2*j  ));
    float4 p2 = read_imagef(in, sampleri, (int2)(p.x+2*i+1, p.y+2*j  ));
    float4 p3 = read_imagef(in, sampleri, (int2)(p.x+2*i,   p.y+2*j+1));
    float4 p4 = read_imagef(in, sampleri, (int2)(p.x+2*i+1, p.y+2*j+1));
    // color += filter[j+samples]*filter[i+samples]*(float4)(p1.x, (p2.x+p3.x)*.5f, p4.x, 0.0f);
    color += (float4)(p1.x, (p2.x+p3.x)*0.5f, p4.x, 0.0f);
  }
  color /= (2*samples+1)*(2*samples+1);
  write_imagef (out, (int2)(x, y), color);
}


/**
 * fill greens pass of pattern pixel grouping.
 * in (float) -> out (float4)
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
  float4 color;// = (float4)(100.0f, 100.0f, 100.0f, 10000.0f); // output color

  const float4 pc   = read_imagef(in, sampleri, (int2)(col, row));

  if     (c == 0) color.x = pc.x; // red
  else if(c == 1) color.y = pc.x; // green1
  else if(c == 2) color.z = pc.x; // blue
  else            color.y = pc.x; // green2

  // fill green layer for red and blue pixels:
  if(c == 0 || c == 2)
  {
    // look up horizontal and vertical neighbours, sharpened weight:
    const float4 pym  = read_imagef(in, sampleri, (int2)(col, row-1));
    const float4 pym2 = read_imagef(in, sampleri, (int2)(col, row-2));
    const float4 pym3 = read_imagef(in, sampleri, (int2)(col, row-3));
    const float4 pyM  = read_imagef(in, sampleri, (int2)(col, row+1));
    const float4 pyM2 = read_imagef(in, sampleri, (int2)(col, row+2));
    const float4 pyM3 = read_imagef(in, sampleri, (int2)(col, row+3));
    const float4 pxm  = read_imagef(in, sampleri, (int2)(col-1, row));
    const float4 pxm2 = read_imagef(in, sampleri, (int2)(col-2, row));
    const float4 pxm3 = read_imagef(in, sampleri, (int2)(col-3, row));
    const float4 pxM  = read_imagef(in, sampleri, (int2)(col+1, row));
    const float4 pxM2 = read_imagef(in, sampleri, (int2)(col+2, row));
    const float4 pxM3 = read_imagef(in, sampleri, (int2)(col+3, row));
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

__kernel void
ppg_demosaic_green_median (__read_only image2d_t in, __write_only image2d_t out, const unsigned int filters)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  // process all non-green pixels
  const int row = y;
  const int col = x;
  const int c = FC(row, col, filters);

  const float4 pc = read_imagef(in, sampleri, (int2)(col, row));

  // fill green layer for red and blue pixels:
  if(c == 0 || c == 2)
  {
    // look up horizontal and vertical neighbours, sharpened weight:
    const float4 pym  = read_imagef(in, sampleri, (int2)(col, row-1));  // g
    const float4 pym2 = read_imagef(in, sampleri, (int2)(col, row-2));
    const float4 pym3 = read_imagef(in, sampleri, (int2)(col, row-3));  // g
    const float4 pyM  = read_imagef(in, sampleri, (int2)(col, row+1));  // g
    const float4 pyM2 = read_imagef(in, sampleri, (int2)(col, row+2));
    const float4 pyM3 = read_imagef(in, sampleri, (int2)(col, row+3));  // g
    const float4 pxm  = read_imagef(in, sampleri, (int2)(col-1, row));  // g
    const float4 pxm2 = read_imagef(in, sampleri, (int2)(col-2, row));
    const float4 pxm3 = read_imagef(in, sampleri, (int2)(col-3, row));  // g
    const float4 pxM  = read_imagef(in, sampleri, (int2)(col+1, row));  // g
    const float4 pxM2 = read_imagef(in, sampleri, (int2)(col+2, row));
    const float4 pxM3 = read_imagef(in, sampleri, (int2)(col+3, row));  // g
    // FIXME: now we need the xyz mess again!
    const float guessx = (pxm.y + ((float *)&pc)[c] + pxM.y) * 2.0f - ((float *)&pxM2)[c] - ((float *)&pxm2)[c];
    const float diffx  = (fabs(((float *)&pxm2)[c] - ((float *)&pc)[c]) +
                          fabs(((float *)&pxM2)[c] - ((float *)&pc)[c]) + 
                          fabs(pxm.y  - pxM.y)) * 3.0f +
                         (fabs(pxM3.y - pxM.y) + fabs(pxm3.y - pxm.y)) * 2.0f;
    const float guessy = (pym.y + ((float *)&pc)[c] + pyM.y) * 2.0f - ((float *)&pyM2)[c] - ((float *)&pym2)[c];
    const float diffy  = (fabs(((float *)&pym2)[c] - ((float *)&pc)[c]) +
                          fabs(((float *)&pyM2)[c] - ((float *)&pc)[c]) + 
                          fabs(pym.y  - pyM.y)) * 3.0f +
                         (fabs(pyM3.y - pyM.y) + fabs(pym3.y - pym.y)) * 2.0f;
    if(diffx > diffy)
    {
      // use guessy
      const float m = fmin(pym.y, pyM.y);
      const float M = fmax(pym.y, pyM.y);
      pc.y = fmax(fmin(guessy*.25f, M), m);
    }
    else
    {
      const float m = fmin(pxm.y, pxM.y);
      const float M = fmax(pxm.y, pxM.y);
      pc.y = fmax(fmin(guessx*.25f, M), m);
    }
  }
  write_imagef (out, (int2)(x, y), pc);
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

/**
 * FCN() is a functional equivalent to FC() avoiding right-shift operations. FC() would trigger
 * a compiler error in kernel border_interpolate() for NVIDIA openCL drivers (as of version 270.41).
 * TODO: revise if still needed for newer versions of NVIDIA's driver.
 */
int
FCN(const int row, const int col, const unsigned int filters)
{
  return (filters >> (2*((2*row & 14) + (col & 1)) )) & 3;
}

/**
 * Demosaic image border
 */
__kernel void
border_interpolate(__read_only image2d_t in, __write_only image2d_t out, unsigned int width, unsigned int height, const unsigned int filters)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  int border = 3;
  int avgwindow = 1;

  if(x>=border && x<width-border && y>=border && y<height-border) return;

  float4 o;
  float sum[4] = { 0.0f };
  int count[4] = { 0 };

  for (int j=y-avgwindow; j<=y+avgwindow; j++) for (int i=x-avgwindow; i<=x+avgwindow; i++)
  {
    if (j>=0 && i>=0 && j<height && i<width)
    {
      int f = FCN(j,i,filters);
      sum[f] += read_imagef(in, sampleri, (int2)(i, j)).x;
      count[f]++;
    }
  }

  float i = read_imagef(in, sampleri, (int2)(x, y)).x;
  o.x = count[0] > 0 ? sum[0]/count[0] : i;
  o.y = count[1]+count[3] > 0 ? (sum[1]+sum[3])/(count[1]+count[3]) : i;
  o.z = count[2] > 0 ? sum[2]/count[2] : i;

  int f = FCN(y,x,filters);

  if     (f == 0) o.x = i;
  else if(f == 1) o.y = i;
  else if(f == 2) o.z = i;
  else            o.y = i;

  write_imagef (out, (int2)(x, y), o);
}

