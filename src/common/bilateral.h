/*
    This file is part of darktable,
    copyright (c) 2012 johannes hanika.

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

typedef struct dt_bilateral_t
{
  int size_x, size_y, size_z;
  int width, height;
  float sigma_s, sigma_r;
  float *buf, *scratch;
}
dt_bilateral_t;

static void
image_to_grid(
    const dt_bilateral_t *const b,
    const int i,
    const int j,
    const float L,
    float *x,
    float *y,
    float *z)
{
  *x = CLAMPS(i/b->sigma_s, 0, b->size_x-1);
  *y = CLAMPS(j/b->sigma_s, 0, b->size_y-1);
  *z = CLAMPS(L/b->sigma_r, 0, b->size_z-1);
}

static void
blur_line(
    float    *buf,
    float    *scratch,
    const int offset1,
    const int offset2,
    const int offset3,
    const int size1,
    const int size2,
    const int size3,
    const float wm2,
    const float wm1,
    const float w0,
    const float w1,
    const float w2);

dt_bilateral_t *
dt_bilateral_init(
    const int width,       // width of input image
    const int height,      // height of input image
    const float sigma_s,   // spatial sigma (blur pixel coords)
    const float sigma_r)   // range sigma (blur luma values)
{
  dt_bilateral_t *b = (dt_bilateral_t *)malloc(sizeof(dt_bilateral_t));
  b->size_x = CLAMPS((int)roundf(width/sigma_s), 4, 1000);
  b->size_y = CLAMPS((int)roundf(height/sigma_s), 4, 1000);
  b->size_z = CLAMPS((int)roundf(120.0f/sigma_r), 4, 1000);
  b->width = width;
  b->height = height;
  b->sigma_s = sigma_s;
  b->sigma_r = sigma_r;
  b->buf = dt_alloc_align(16, b->size_x*b->size_y*b->size_z*sizeof(float));
  b->scratch = dt_alloc_align(16, MAX(MAX(b->size_x, b->size_y), b->size_z)*dt_get_num_threads()*sizeof(float));

  fprintf(stderr, "[bilateral] created grid [%d %d %d] with sigma %f %f\n", b->size_x, b->size_y, b->size_z, b->sigma_s, b->sigma_r);
#if 0
  blur_line(b->buf, b->scratch, 5*5, 5, 1,
      5, 5, 5, 0, 0, 0, 0, 0);
  blur_line(b->buf, b->scratch, 1, 5*5, 4,
      5, 5, 5, 0, 0, 0, 0, 0);
  blur_line(b->buf, b->scratch, 1, 5, 5*5,
      5, 5, 5, 0, 0, 0, 0, 0);
  exit(0);
#endif
  return b;
}

void
dt_bilateral_splat(
    dt_bilateral_t *b,
    const float    *const in)
{
  // splat into downsampled grid
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(b)
#endif
  for(int j=0;j<b->height;j++)
  {
    int index = 4*j*b->width;
    for(int i=0;i<b->width;i++)
    {
      float x, y, z;
      const float L = in[index];
      image_to_grid(b, i, j, L, &x, &y, &z);
      // nearest neighbour splatting:
      const int grid_index = (int)x + b->size_x*((int)y + b->size_y*(int)z);
      // sum up payload here, doesn't have to be same as edge stopping data
      // for cross bilateral applications.
      // also note that this is not clipped (as L->z is), so potentially hdr/out of gamut
      // should not cause clipping here.
#ifdef _OPENMP
#pragma omp atomic
#endif
      // TODO: this causes slight grid aliasing. maybe splat bilinear, too?
      b->buf[grid_index] += 120.0f/(b->sigma_s*b->sigma_s);
      index += 4;
    }
  }
}

static void
blur_line(
    float    *buf,
    float    *scratch,
    const int offset1,
    const int offset2,
    const int offset3,
    const int size1,
    const int size2,
    const int size3,
    const float wm2,
    const float wm1,
    const float w0,
    const float w1,
    const float w2)
{
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(buf, scratch)
#endif
  for(int k=0;k<size1;k++)
  {
    int index = k*offset1;
    for(int j=0;j<size2;j++)
    {
      if(index != j*offset2 + k*offset1)
      {
        printf("index %d [%d %d %d]\n", index, k, j, 0);
        printf("** wrong index!\n");
      }
      // need to cache our neighbours because we want to do
      // the convolution in place:
      float *cache = scratch + dt_get_thread_num() * size3;
      for(int i=0;i<size3;i++)
      {
        cache[i] = buf[index];
        index += offset3;
      }
      index -= offset3*size3;
      buf[index]  = w0 * cache[0];
      buf[index] += w1 * cache[1];
      buf[index] += w2 * cache[2];
      index += offset3;
      buf[index]  = wm1* cache[0];
      buf[index] += w0 * cache[1];
      buf[index] += w1 * cache[2];
      buf[index] += w2 * cache[3];
      index += offset3;
      for(int i=2;i<size3-2;i++)
      {
        float sum;
        // TODO: check if indices make any sense
        sum  = wm2* cache[i-2];
        sum += wm1* cache[i-1];
        sum += w0 * cache[i];
        sum += w1 * cache[i+1];
        sum += w2 * cache[i+2];
        buf[index] = sum;
        index += offset3;
      }
      buf[index]  = wm2* cache[size3-4];
      buf[index] += wm1* cache[size3-3];
      buf[index] += w0 * cache[size3-2];
      buf[index] += w1 * cache[size3-1];
      index += offset3;
      buf[index]  = wm2* cache[size3-3];
      buf[index] += wm1* cache[size3-2];
      buf[index] += w0 * cache[size3-1];
      if(index > size1*size2*size3)
      {
        printf("[bilateral] index out of bounds!!\n");
        break;
      }
      index += offset3;
      index += offset2 - offset3*size3;
    }
  }
}


void
dt_bilateral_blur(
    dt_bilateral_t *b)
{
  // gaussian up to 3 sigma
  blur_line(b->buf, b->scratch, b->size_x*b->size_y, b->size_x, 1,
      b->size_z, b->size_y, b->size_x,
      // 1.0f/(b->sigma_s*16.0f), 4.0f/(b->sigma_s*16.0f), 6.0f/(b->sigma_s*16.0f), 4.0f/(b->sigma_s*16.0f), 1.0f/(b->sigma_s*16.0f));
      // 1.0f*b->sigma_s/16.0f, 4.0f*b->sigma_s/16.0f, 6.0f*b->sigma_s/16.0f, 4.0f*b->sigma_s/16.0f, 1.0f*b->sigma_s/16.0f);
      1.0f/16.0f, 4.0f/16.0f, 6.0f/16.0f, 4.0f/16.0f, 1.0f/16.0f);
  // gaussian up to 3 sigma
  blur_line(b->buf, b->scratch, b->size_x*b->size_y, 1, b->size_x,
      b->size_z, b->size_x, b->size_y,
      // 1.0f/(b->sigma_s*16.0f), 4.0f/(b->sigma_s*16.0f), 6.0f/(b->sigma_s*16.0f), 4.0f/(b->sigma_s*16.0f), 1.0f/(b->sigma_s*16.0f));
      // 1.0f*b->sigma_s/16.0f, 4.0f*b->sigma_s/16.0f, 6.0f*b->sigma_s/16.0f, 4.0f*b->sigma_s/16.0f, 1.0f*b->sigma_s/16.0f);
      1.0f/16.0f, 4.0f/16.0f, 6.0f/16.0f, 4.0f/16.0f, 1.0f/16.0f);
  // -2 derivative of the gaussian up to 3 sigma: x*exp(-x*x)
  blur_line(b->buf, b->scratch, 1, b->size_x, b->size_x*b->size_y,
      b->size_x, b->size_y, b->size_z,
      // 1.0f/16.0f, 4.0f/16.0f, 6.0f/16.0f, 4.0f/16.0f, 1.0f/16.0f);
      -2.0f*b->sigma_r*1.0f/16.0f, -1.0f*b->sigma_r*4.0f/16.0f, 0.0f, 1.0f*b->sigma_r*4.0f/16.0f, 2.0f*b->sigma_r*1.0f/16.0f);
      // -0.03663127777746836f, -0.36787944117144233f, 0.0f, 0.36787944117144233f, 0.03663127777746836f);
}


void
dt_bilateral_slice(
    const dt_bilateral_t *const b,
    const float          *const in,
    float                *out)
{
  // FIXME: this is wrong:
  const float norm = 1.0f/(b->sigma_r);//*b->sigma_s);//1.0f;
#if 1
  const int ox = 1;
  const int oy = b->size_x;
  const int oz = b->size_y*b->size_x;
#endif
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(out)
#endif
  for(int j=0;j<b->height;j++)
  {
    int index = 4*j*b->width;
    for(int i=0;i<b->width;i++)
    {
      float x, y, z, xf, yf, zf;
      const float L = in[index];
      image_to_grid(b, i, j, L, &x, &y, &z);
      // trilinear lookup:
      const int xi = MIN((int)x, b->size_x-2);
      const int yi = MIN((int)y, b->size_y-2);
      const int zi = MIN((int)z, b->size_z-2);
#if 1
      xf = x - xi;
      yf = y - yi;
      zf = z - zi;
#endif
      const int gi = xi + b->size_x*(yi + b->size_y*zi);
      const float Lout = L + norm * (//*b->buf[gi];
#if 1
        b->buf[gi]          * (1.0f - xf) * (1.0f - yf) * (1.0f - zf) +
        b->buf[gi+ox]       * (       xf) * (1.0f - yf) * (1.0f - zf) +
        b->buf[gi+oy]       * (1.0f - xf) * (       yf) * (1.0f - zf) +
        b->buf[gi+ox+oy]    * (       xf) * (       yf) * (1.0f - zf) +
        b->buf[gi+oz]       * (1.0f - xf) * (1.0f - yf) * (       zf) +
        b->buf[gi+ox+oz]    * (       xf) * (1.0f - yf) * (       zf) +
        b->buf[gi+oy+oz]    * (1.0f - xf) * (       yf) * (       zf) +
        b->buf[gi+ox+oy+oz] * (       xf) * (       yf) * (       zf));
#endif
      out[index] = MAX(0.0f, Lout);
      // and copy color and mask
      out[index+1] = 0.0f;//in[index+1];
      out[index+2] = 0.0f;//in[index+2];
      out[index+3] = in[index+3];
      index += 4;
    }
  }
}

void
dt_bilateral_free(
    dt_bilateral_t *b)
{
  free(b->buf);
  free(b->scratch);
  free(b);
}


