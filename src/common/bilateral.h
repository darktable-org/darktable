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
  float sigma_s, sigma_r;
  float *buf;
}
dt_bilateral_t;

static void
image_to_grid(
    const dt_bilateral_t *const b,
    const int i,
    const int j,
    const float L,
    float &x,
    float &y,
    float &z)
{
  *x = CLAMPS(i/b->sigma_s, 0, b->size_x-1);
  *y = CLAMPS(i/b->sigma_s, 0, b->size_y-1);
  *z = CLAMPS(i/b->sigma_r, 0, b->size_z-1);
}

dt_bilateral_t *
dt_bilateral_init(
    const int width,       // width of input image
    const int height,      // height of input image
    const float sigma_s,   // spatial sigma (blur pixel coords)
    const float sigma_r)   // range sigma (blur luma values)
{
  dt_bilateral_t *b = (dt_bilateral_t *)malloc(sizeof(dt_bilateral_t));
  b->size_x = (int)roundf(width/sigma_s);
  b->size_y = (int)roundf(height/sigma_s);
  b->size_z = (int)roundf(1.0f/sigma_r);
  b->buf = dt_alloc_align(b->size_x*b->size_y*b->size_z*sizeof(float));
  b->scratch = dt_alloc_align(MAX(MAX(b->size_x, b->size_y), b->size_z)*dt_get_num_threads()*sizeof(float));
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
      const int grid_index = (int)x + d->size_y*((int)y + d->size_z*(int)z);
      // sum up payload here, doesn't have to be same as edge stopping data
      // for cross bilateral applications.
      // also note that this is not clipped (as L->z is), so potentially hdr/out of gamut
      // should not cause clipping here.
#ifdef _OPENMP
#pragma omp atomic
#endif
      b->buf[grid_index] += 1.0f;
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
      // need to cache our neighbours because we want to do
      // the convolution in place:
      float *cache = scratch + dt_get_thread_num() * size3;
      float sum = 0.0f;
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
      index += offset3;
      index += offset2 - offset3;
    }
  }
}


void
dt_bilateral_blur(
    dt_bilateral_t *b)
{
  blur_line(b->buf, scratch, b->size_x*b->size_y, b->size_y, 1,
      b->size_z, b->size_y, b->size_x,
      0.01f, 0.5f, 1.0f, 0.5f, 0.01f); // FIXME: gaussian kernel
  blur_line(b->buf, scratch, b->size_x*b->size_y, 1, b->size_y,
      b->size_z, b->size_x, b->size_y,
      0.01f, 0.5f, 1.0f, 0.5f, 0.01f); // FIXME: gaussian
  blur_line(b->buf, scratch, 1, b->size_y, b->size_x*b->size_y,
      b->size_x, b->size_y, b->size_z,
      -0.01f, -0.5f, 0.0f, 0.5f, 0.01f); // FIXME: derivative of gaussian x*exp(-x^2)
}


void
dt_bilateral_slice(
    const dt_bilateral_t *const b,
    const float          *out)
{
  const int ox = 1;
  const int oy = b->size_y;
  const int oz = b->size_y*b->size_x;
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(out)
#endif
  for(int j=0;j<b->height;j++)
  {
    int index = 4*j*b->width;
    for(int i=0;i<b->width;i++)
    {
      float x, y, z, xf, yf, zf;
      const float L = out[index];
      image_to_grid(b, i, j, L, &x, &y, &z);
      // trilinear lookup:
      xf = x - (int)x;
      x  = (int)x;
      yf = y - (int)y;
      y  = (int)y;
      zf = z - (int)z;
      z  = (int)z;
      const int gi = (int)x + d->size_y*((int)y + d->size_z*(int)z);
      const float Lout = L +
        b->buf[gi]          * (1.0f - xf) * (1.0f - yf) * (1.0f - zf) +
        b->buf[gi+ox]       * (       xf) * (1.0f - yf) * (1.0f - zf) +
        b->buf[gi+oy]       * (1.0f - xf) * (       yf) * (1.0f - zf) +
        b->buf[gi+ox+oy]    * (       xf) * (       yf) * (1.0f - zf) +
        b->buf[gi+oz]       * (1.0f - xf) * (1.0f - yf) * (       zf) +
        b->buf[gi+ox+oz]    * (       xf) * (1.0f - yf) * (       zf) +
        b->buf[gi+oy+oz]    * (1.0f - xf) * (       yf) * (       zf) +
        b->buf[gi+ox+oy+oz] * (       xf) * (       yf) * (       zf);
      out[index] = Lout;
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


