/*
    This file is part of darktable,
    Copyright (C) 2021 darktable developers.

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

/* Internal segmentation algorithms
   - morphological closing operation supporting radius up to 10, tuned for performance
   Hanno Schwalm 2021/12
*/

typedef struct dt_pos_t
{
  int xpos;
  int ypos;
} dt_pos_t;

typedef struct dt_iop_segmentation_t
{
  int *data;      // holding segment id's for every location
  int *size;      // size of segment      
  int *xmin;      // bounding rectangle for each segment
  int *xmax;
  int *ymin;
  int *ymax;
  size_t *ref;    // possibly a reference point for location
  float *val1;
  float *val2;  
  int nr;         // number of found segments
} dt_iop_segmentation_t;

typedef struct dt_ff_stack_t
{
  int pos;
  dt_pos_t *el;
} dt_ff_stack_t;

static inline void push_stack(int xpos, int ypos, dt_ff_stack_t *stack)
{
  const int i = stack->pos;
  stack->el[i].xpos = xpos;
  stack->el[i].ypos = ypos;
  stack->pos++;
}

static inline dt_pos_t * pop_stack(dt_ff_stack_t *stack)
{
  if(stack->pos > 0) stack->pos--;  
  return &stack->el[stack->pos];
}

void dt_segmentation_init_struct(dt_iop_segmentation_t *seg, const int width, const int height, const int segments)
{
  seg->nr = 0;
  seg->data =   dt_alloc_align(64, width * height * sizeof(int));
  seg->size =   dt_alloc_align(64, segments * sizeof(int));
  seg->xmin =   dt_alloc_align(64, segments * sizeof(int));
  seg->xmax =   dt_alloc_align(64, segments * sizeof(int));
  seg->ymin =   dt_alloc_align(64, segments * sizeof(int));
  seg->ymax =   dt_alloc_align(64, segments * sizeof(int));
  seg->ref =    dt_alloc_align(64, segments * sizeof(size_t));
  seg->val1 =   dt_alloc_align_float(segments);
  seg->val2 =   dt_alloc_align_float(segments);
}

void dt_segmentation_free_struct(dt_iop_segmentation_t *seg)
{
  dt_free_align(seg->data);
  dt_free_align(seg->size);
  dt_free_align(seg->xmin);
  dt_free_align(seg->ymin);
  dt_free_align(seg->xmax);
  dt_free_align(seg->ymax);
  dt_free_align(seg->ref);
  dt_free_align(seg->val1);
  dt_free_align(seg->val2);
}

static inline void _dilate_1(const int *img, int *o, const int w1, const int height, const int border)
{
#ifdef _OPENMP
  #pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(img, o) \
  dt_omp_sharedconst(height, w1, border) \
  schedule(simd:static) aligned(o, img : 64)
#endif
  for(int row = border; row < height - border; row++)
  {
    for(int col = border, i = row*w1 + col; col < w1 - border; col++, i++)
      o[i] =   img[i-w1-1] | img[i-w1] | img[i-w1+1] |
               img[i-1]    | img[i]    | img[i+1] |
               img[i+w1-1] | img[i+w1] | img[i+w1+1];
  }
}

static inline void _erode_1(const int *img, int *o, const int w1, const int height, const int border)
{
#ifdef _OPENMP
  #pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(img, o) \
  dt_omp_sharedconst(height, w1, border) \
  schedule(simd:static) aligned(o, img : 64)
#endif
  for(int row = border; row < height - border; row++)
  {
    for(int col = border, i = row*w1 + col; col < w1 - border; col++, i++)
      o[i] =   img[i-w1-1] & img[i-w1] & img[i-w1+1] &
               img[i-1]    & img[i]    & img[i+1] &
               img[i+w1-1] & img[i+w1] & img[i+w1+1];

  }
}

static inline void _dilate_2(const int *img, int *o, const int w1, const int height, const int border)
{
  const int w2 = 2*w1;
#ifdef _OPENMP
  #pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(img, o) \
  dt_omp_sharedconst(height, w1, w2, border) \
  schedule(simd:static) aligned(o, img : 64)
#endif
  for(int row = border; row < height - border; row++)
  {
    for(int col = border, i = row*w1 + col; col < w1 - border; col++, i++)
      o[i] =         img[i-w2-1] | img[i-w2] | img[i-w2+1] |
       img[i-w1-2] | img[i-w1-1] | img[i-w1] | img[i-w1+1] | img[i-w1+2] | 
       img[i-2] |    img[i-1]    |    img[i] | img[i+1]    | img[i+2] |
       img[i+w1-2] | img[i+w1-1] | img[i+w1] | img[i+w1+1] | img[i+w1+2] |
                     img[i+w2-1] | img[i+w2] | img[i+w2+1];
  }
}

static inline void _erode_2(const int *img, int *o, const int w1, const int height, const int border)
{
  const int w2 = 2*w1;
#ifdef _OPENMP
  #pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(img, o) \
  dt_omp_sharedconst(height, w1, w2, border) \
  schedule(simd:static) aligned(o, img : 64)
#endif
  for(int row = border; row < height - border; row++)
  {
    for(int col = border, i = row*w1 + col; col < w1 - border; col++, i++)
      o[i] =         img[i-w2-1] & img[i-w2] & img[i-w2+1] &
       img[i-w1-2] & img[i-w1-1] & img[i-w1] & img[i-w1+1] & img[i-w1+2] & 
       img[i-2] &    img[i-1]    &    img[i] & img[i+1]    & img[i+2] &
       img[i+w1-2] & img[i+w1-1] & img[i+w1] & img[i+w1+1] & img[i+w1+2] &
                     img[i+w2-1] & img[i+w2] & img[i+w2+1];
  }
}

static inline void _dilate_3(const int *img, int *o, const int w1, const int height, const int border)
{
  const int w2 = 2*w1;
  const int w3 = 3*w1;
#ifdef _OPENMP
  #pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(img, o) \
  dt_omp_sharedconst(height, w1, w2, w3, border) \
  schedule(simd:static) aligned(o, img : 64)
#endif
  for(int row = border; row < height - border; row++)
  {
    for(int col = border, i = row*w1 + col; col < w1 - border; col++, i++)
    {
        o[i] =                               img[i-w3-1] | img[i-w3] | img[i-w3+1] |
                               img[i-w2-2] | img[i-w2-1] | img[i-w2] | img[i-w2+1] | img[i-w2+2] | 
                 img[i-w1-3] | img[i-w1-2] | img[i-w1-1] | img[i-w1] | img[i-w1+1] | img[i-w1+2] | img[i-w1+3] | 
                 img[i-3]    | img[i-2]    | img[i-1]    | img[i]    | img[i+1]    | img[i+2]    | img[i+3]    | 
                 img[i+w1-3] | img[i+w1-2] | img[i+w1-1] | img[i+w1] | img[i+w1+1] | img[i+w1+2] | img[i+w1+3] | 
                               img[i+w2-2] | img[i+w2-1] | img[i+w2] | img[i+w2+1] | img[i+w2+2] |
                                             img[i+w3-1] | img[i+w3] | img[i+w3+1]; 
    }
  }
}

static inline void _erode_3(const int *img, int *o, const int w1, const int height, const int border)
{
  const int w2 = 2*w1;
  const int w3 = 3*w1;
#ifdef _OPENMP
  #pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(img, o) \
  dt_omp_sharedconst(height, w1, w2, w3, border) \
  schedule(simd:static) aligned(o, img : 64)
#endif
  for(int row = border; row < height - border; row++)
  {
    for(int col = border, i = row*w1 + col; col < w1 - border; col++, i++)
    {
        o[i] =                               img[i-w3-1] & img[i-w3] & img[i-w3+1] &
                               img[i-w2-2] & img[i-w2-1] & img[i-w2] & img[i-w2+1] & img[i-w2+2] & 
                 img[i-w1-3] & img[i-w1-2] & img[i-w1-1] & img[i-w1] & img[i-w1+1] & img[i-w1+2] & img[i-w1+3] & 
                 img[i-3]    & img[i-2]    & img[i-1]    & img[i]    & img[i+1]    & img[i+2]    & img[i+3]    & 
                 img[i+w1-3] & img[i+w1-2] & img[i+w1-1] & img[i+w1] & img[i+w1+1] & img[i+w1+2] & img[i+w1+3] & 
                               img[i+w2-2] & img[i+w2-1] & img[i+w2] & img[i+w2+1] & img[i+w2+2] &
                                             img[i+w3-1] & img[i+w3] & img[i+w3+1]; 
    }
  }
}

static inline void _dilate_4(const int *img, int *o, const int w1, const int height, const int border)
{
  const int w2 = 2*w1;
  const int w3 = 3*w1;
  const int w4 = 4*w1;
#ifdef _OPENMP
  #pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(img, o) \
  dt_omp_sharedconst(height, w1, w2, w3, w4, border) \
  schedule(simd:static) aligned(o, img : 64)
#endif
  for(int row = border; row < height - border; row++)
  {
    for(int col = border, i = row*w1 + col; col < w1 - border; col++, i++)
    {
        o[i] =                     img[i-w4-2] | img[i-w4-1] | img[i-w4] | img[i-w4+1] | img[i-w4+2] |
                     img[i-w3-3] | img[i-w3-2] | img[i-w3-1] | img[i-w3] | img[i-w3+1] | img[i-w3+2] | img[i-w3+3] |
       img[i-w2-4] | img[i-w2-3] | img[i-w2-2] | img[i-w2-1] | img[i-w2] | img[i-w2+1] | img[i-w2+2] | img[i-w2+3] | img[i-w2+4] | 
       img[i-w1-4] | img[i-w1-3] | img[i-w1-2] | img[i-w1-1] | img[i-w1] | img[i-w1+1] | img[i-w1+2] | img[i-w1+3] | img[i-w1+4] | 
       img[i-4]    | img[i-3]    | img[i-2]    | img[i-1]    | img[i]    | img[i+1]    | img[i+2]    | img[i+3]    | img[i+4] | 
       img[i+w1-4] | img[i+w1-3] | img[i+w1-2] | img[i+w1-1] | img[i+w1] | img[i+w1+1] | img[i+w1+2] | img[i+w1+3] | img[i+w1+4] | 
       img[i+w2-4] | img[i+w2-3] | img[i+w2-2] | img[i+w2-1] | img[i+w2] | img[i+w2+1] | img[i+w2+2] | img[i+w2+3] | img[i+w2+4] | 
                     img[i+w3-3] | img[i+w3-2] | img[i+w3-1] | img[i+w3] | img[i+w3+1] | img[i+w3+2] | img[i+w3+3] |
                                   img[i+w4-2] | img[i+w4-1] | img[i+w4] | img[i+w4+1] | img[i+w4+2]; 
    }
  }
}

static inline void _erode_4(const int *img, int *o, const int w1, const int height, const int border)
{
  const int w2 = 2*w1;
  const int w3 = 3*w1;
  const int w4 = 4*w1;
#ifdef _OPENMP
  #pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(img, o) \
  dt_omp_sharedconst(height, w1, w2, w3, w4, border) \
  schedule(simd:static) aligned(o, img : 64)
#endif
  for(int row = border; row < height - border; row++)
  {
    for(int col = border, i = row*w1 + col; col < w1 - border; col++, i++)
    {
        o[i] =                     img[i-w4-2] & img[i-w4-1] & img[i-w4] & img[i-w4+1] & img[i-w4+2] &
                     img[i-w3-3] & img[i-w3-2] & img[i-w3-1] & img[i-w3] & img[i-w3+1] & img[i-w3+2] & img[i-w3+3] &
       img[i-w2-4] & img[i-w2-3] & img[i-w2-2] & img[i-w2-1] & img[i-w2] & img[i-w2+1] & img[i-w2+2] & img[i-w2+3] & img[i-w2+4] & 
       img[i-w1-4] & img[i-w1-3] & img[i-w1-2] & img[i-w1-1] & img[i-w1] & img[i-w1+1] & img[i-w1+2] & img[i-w1+3] & img[i-w1+4] & 
       img[i-4]    & img[i-3]    & img[i-2]    & img[i-1]    & img[i]    & img[i+1]    & img[i+2]    & img[i+3]    & img[i+4] & 
       img[i+w1-4] & img[i+w1-3] & img[i+w1-2] & img[i+w1-1] & img[i+w1] & img[i+w1+1] & img[i+w1+2] & img[i+w1+3] & img[i+w1+4] & 
       img[i+w2-4] & img[i+w2-3] & img[i+w2-2] & img[i+w2-1] & img[i+w2] & img[i+w2+1] & img[i+w2+2] & img[i+w2+3] & img[i+w2+4] & 
                     img[i+w3-3] & img[i+w3-2] & img[i+w3-1] & img[i+w3] & img[i+w3+1] & img[i+w3+2] & img[i+w3+3] &
                                   img[i+w4-2] & img[i+w4-1] & img[i+w4] & img[i+w4+1] & img[i+w4+2]; 

    }
  }
}

static inline void _dilate_5(const int *img, int *o, const int w1, const int height, const int border)
{
  const int w2 = 2*w1;
  const int w3 = 3*w1;
  const int w4 = 4*w1;
  const int w5 = 5*w1;
#ifdef _OPENMP
  #pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(img, o) \
  dt_omp_sharedconst(height, w1, w2, w3, w4, w5, border) \
  schedule(simd:static) aligned(o, img : 64)
#endif
  for(int row = border; row < height - border; row++)
  {
    for(int col = border, i = row*w1 + col; col < w1 - border; col++, i++)
    {
      o[i] =                                     img[i-w5-2] | img[i-w5-1] | img[i-w5] | img[i-w5+1] | img[i-w5+2] |
                                   img[i-w4-3] | img[i-w4-2] | img[i-w4-1] | img[i-w4] | img[i-w4+1] | img[i-w4+2] | img[i-w4+3] |
                     img[i-w3-4] | img[i-w3-3] | img[i-w3-2] | img[i-w3-1] | img[i-w3] | img[i-w3+1] | img[i-w3+2] | img[i-w3+3] | img[i-w3+4] |
       img[i-w2-5] | img[i-w2-4] | img[i-w2-3] | img[i-w2-2] | img[i-w2-1] | img[i-w2] | img[i-w2+1] | img[i-w2+2] | img[i-w2+3] | img[i-w2+4] | img[i-w2+5] | 
       img[i-w1-5] | img[i-w1-4] | img[i-w1-3] | img[i-w1-2] | img[i-w1-1] | img[i-w1] | img[i-w1+1] | img[i-w1+2] | img[i-w1+3] | img[i-w1+4] | img[i-w1+5] | 
       img[i-5]    | img[i-4]    | img[i-3]    | img[i-2]    | img[i-1]    | img[i]    | img[i+1]    | img[i+2]    | img[i+3]    | img[i+4]    | img[i+5] | 
       img[i+w1-5] | img[i+w1-4] | img[i+w1-3] | img[i+w1-2] | img[i+w1-1] | img[i+w1] | img[i+w1+1] | img[i+w1+2] | img[i+w1+3] | img[i+w1+4] | img[i+w1+5] | 
       img[i+w2-5] | img[i+w2-4] | img[i+w2-3] | img[i+w2-2] | img[i+w2-1] | img[i+w2] | img[i+w2+1] | img[i+w2+2] | img[i+w2+3] | img[i+w2+4] | img[i+w2+5] |  
                     img[i+w3-4] | img[i+w3-3] | img[i+w3-2] | img[i+w3-1] | img[i+w3] | img[i+w3+1] | img[i+w3+2] | img[i+w3+3] | img[i+w3+4] | 
                                   img[i+w4-3] | img[i+w4-2] | img[i+w4-1] | img[i+w4] | img[i+w4+1] | img[i+w4+2] | img[i+w4+3] |
                                                 img[i+w5-2] | img[i+w5-1] | img[i+w5] | img[i+w5+1] | img[i+w5+2]; 
    }
  }
}

static inline void _erode_5(const int *img, int *o, const int w1, const int height, const int border)
{
  const int w2 = 2*w1;
  const int w3 = 3*w1;
  const int w4 = 4*w1;
  const int w5 = 5*w1;
#ifdef _OPENMP
  #pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(img, o) \
  dt_omp_sharedconst(height, w1, w2, w3, w4, w5, border) \
  schedule(simd:static) aligned(o, img : 64)
#endif
  for(int row = border; row < height - border; row++)
  {
    for(int col = border, i = row*w1 + col; col < w1 - border; col++, i++)
    {
      o[i] =                                     img[i-w5-2] & img[i-w5-1] & img[i-w5] & img[i-w5+1] & img[i-w5+2] &
                                   img[i-w4-3] & img[i-w4-2] & img[i-w4-1] & img[i-w4] & img[i-w4+1] & img[i-w4+2] & img[i-w4+3] &
                     img[i-w3-4] & img[i-w3-3] & img[i-w3-2] & img[i-w3-1] & img[i-w3] & img[i-w3+1] & img[i-w3+2] & img[i-w3+3] & img[i-w3+4] &
       img[i-w2-5] & img[i-w2-4] & img[i-w2-3] & img[i-w2-2] & img[i-w2-1] & img[i-w2] & img[i-w2+1] & img[i-w2+2] & img[i-w2+3] & img[i-w2+4] & img[i-w2+5] & 
       img[i-w1-5] & img[i-w1-4] & img[i-w1-3] & img[i-w1-2] & img[i-w1-1] & img[i-w1] & img[i-w1+1] & img[i-w1+2] & img[i-w1+3] & img[i-w1+4] & img[i-w1+5] & 
       img[i-5]    & img[i-4]    & img[i-3]    & img[i-2]    & img[i-1]    & img[i]    & img[i+1]    & img[i+2]    & img[i+3]    & img[i+4]    & img[i+5] & 
       img[i+w1-5] & img[i+w1-4] & img[i+w1-3] & img[i+w1-2] & img[i+w1-1] & img[i+w1] & img[i+w1+1] & img[i+w1+2] & img[i+w1+3] & img[i+w1+4] & img[i+w1+5] & 
       img[i+w2-5] & img[i+w2-4] & img[i+w2-3] & img[i+w2-2] & img[i+w2-1] & img[i+w2] & img[i+w2+1] & img[i+w2+2] & img[i+w2+3] & img[i+w2+4] & img[i+w2+5] &  
                     img[i+w3-4] & img[i+w3-3] & img[i+w3-2] & img[i+w3-1] & img[i+w3] & img[i+w3+1] & img[i+w3+2] & img[i+w3+3] & img[i+w3+4] & 
                                   img[i+w4-3] & img[i+w4-2] & img[i+w4-1] & img[i+w4] & img[i+w4+1] & img[i+w4+2] & img[i+w4+3] &
                                                 img[i+w5-2] & img[i+w5-1] & img[i+w5] & img[i+w5+1] & img[i+w5+2]; 
    }
  }
}

static inline void _intimage_borderfill(int *d, const int width, const int height, const int val, const int border)
{
  for(int i = 0; i < border * width; i++)                            
    d[i] = val;
  for(int i = (height - border - 1) * width; i < width*height; i++)
    d[i] = val;
  for(int row = border; row < height - border; row++)
  {
    int *p1 = d + row*width;
    int *p2 = d + (row+1)*width - border;
    for(int i = 0; i < border; i++)
      p1[i] = p2[i] = val;
  }
}

static void _intimage_dilate(int *src, int *out, const int width, const int height, const int rad, const int border)
{
  _intimage_borderfill(src, width, height, 0, border);
  if(rad == 1)       _dilate_1(src, out, width, height, border);
  else if(rad == 2)  _dilate_2(src, out, width, height, border);
  else if(rad == 3)  _dilate_3(src, out, width, height, border);
  else if(rad == 4)  _dilate_4(src, out, width, height, border);
  else               _dilate_5(src, out, width, height, border);
}

static void _intimage_erode(int *src, int *out, const int width, const int height, const int rad, const int border)
{
  _intimage_borderfill(src, width, height, 1, border);
  if(rad == 1)       _erode_1(src, out, width, height, border);
  else if(rad == 2)  _erode_2(src, out, width, height, border);
  else if(rad == 3)  _erode_3(src, out, width, height, border);
  else if(rad == 4)  _erode_4(src, out, width, height, border);
  else               _erode_5(src, out, width, height, border);
}


void dt_image_transform_dilate(int *img, const int width, const int height, const int radius, const int border)
{
  if(radius < 1) return;
  int *tmp = dt_alloc_align(64, width * height * sizeof(int));
  if(!tmp) return;

  const int rad = MIN(radius, 10);
  _intimage_dilate(img, tmp, width, height, MIN(5, rad), border);

  if(rad < 6)
  {
    memcpy(img, tmp, width*height * sizeof(int));
    dt_free_align(tmp);
    return;
  }
  _intimage_dilate(tmp, img, width, height, MIN(5, rad - 5), border);
}
  
void dt_image_transform_erode(int *img, const int width, const int height, const int radius, const int border)
{
  if(radius < 1) return;
  int *tmp = dt_alloc_align(64, width * height * sizeof(int));
  if(!tmp) return;

  const int rad = MIN(radius, 10);
  _intimage_erode(img, tmp, width, height, MIN(5, rad), border);

  if(rad < 6)
  {
    memcpy(img, tmp, width*height * sizeof(int));
    dt_free_align(tmp);
    return;
  }
  _intimage_erode(tmp, img, width, height, MIN(5, rad - 5), border);
}
  
void dt_image_transform_closing(int *img, const int width, const int height, const int radius, const int border)
{
  if(radius < 1) return;
  dt_image_transform_dilate(img, width, height, radius, border);
  dt_image_transform_erode(img, width, height, radius, border);
}

static int floodfill_segmentize(int yin, int xin, dt_iop_segmentation_t *seg, const int w, const int h, const int id, dt_ff_stack_t *stack)
{
  if((id < 2) || (id >= HLMAXSEGMENTS - 1)) return 0;

  int *d = seg->data;

  int xp = 0;
  int yp = 0;
  size_t rp = 0;
  int min_x = xin;
  int max_x = xin;
  int min_y = yin;
  int max_y = yin;

  int cnt = 0;
  stack->pos = 0;

  seg->size[id] = 0;
  seg->ref[id] = 0;
  seg->val1[id] = 0.0f;
  seg->val2[id] = 0.0f;
  seg->xmin[id] = min_x;
  seg->xmax[id] = max_x;
  seg->ymin[id] = min_y;
  seg->ymax[id] = max_y;

  push_stack(xin, yin, stack);
  while(stack->pos)
  {
    dt_pos_t *coord = pop_stack(stack);
    const int x = coord->xpos;
    const int y = coord->ypos;
    if(d[y*w+x] == 1)
    {
      int yUp = y - 1, yDown = y + 1;
      gboolean lastXUp = FALSE, lastXDown = FALSE, firstXUp = FALSE, firstXDown = FALSE;
      d[y*w+x] = id;
      cnt++;
      if(yUp >= HLBORDER && d[yUp*w+x] == 1)
      {
        push_stack(x, yUp, stack); firstXUp = lastXUp = TRUE;
      }
      else
      {
        xp = x;
        yp = yUp;
        rp = yp*w + xp;
        if(xp > HLBORDER+2 && d[rp] == 0)
        {
          min_x = MIN(min_x, xp);
          max_x = MAX(max_x, xp);
          min_y = MIN(min_y, yp);
          max_y = MAX(max_y, yp);
          d[rp] = HLMAXSEGMENTS+id;
        }
      }
      
      if(yDown < h-HLBORDER && d[yDown*w+x] == 1)
      {
        push_stack(x, yDown, stack); firstXDown = lastXDown = TRUE;
      }
      else
      {
        xp = x;
        yp = yDown;
        rp = yp*w + xp;
        if(yp < h-HLBORDER-3 && d[rp] == 0)
        {
          min_x = MIN(min_x, xp);
          max_x = MAX(max_x, xp);
          min_y = MIN(min_y, yp);
          max_y = MAX(max_y, yp);
          d[rp] = HLMAXSEGMENTS+id;
        }
      }
      
      int xr = x + 1;
      while(xr < w-HLBORDER && d[y*w+xr] == 1)
      {
        d[y*w+xr] = id;
        cnt++;
        if(yUp >= HLBORDER && d[yUp*w + xr] == 1)
        {
          if(!lastXUp) { push_stack(xr, yUp, stack); lastXUp = TRUE; }
        }
        else
        {
          xp = xr;
          yp = yUp;
          rp = yp*w + xp;
          if(yp > HLBORDER+2 && d[rp] == 0)
          {
            min_x = MIN(min_x, xp);
            max_x = MAX(max_x, xp);
            min_y = MIN(min_y, yp);
            max_y = MAX(max_y, yp);
            d[rp] = HLMAXSEGMENTS+id;
          }
          lastXUp = FALSE;
        }

        if(yDown < h-HLBORDER && d[yDown*w+xr] == 1)
        {
          if(!lastXDown) { push_stack(xr, yDown, stack); lastXDown = TRUE; }
        }
        else
        {
          xp = xr;
          yp = yDown;
          rp = yp*w + xp;
          if(yp < h-HLBORDER-3 && d[rp] == 0)
          {
            min_x = MIN(min_x, xp);
            max_x = MAX(max_x, xp);
            min_y = MIN(min_y, yp);
            max_y = MAX(max_y, yp);
            d[rp] = HLMAXSEGMENTS+id;
          }
          lastXDown = FALSE;
        }
        xr++;
      }

      xp = xr;
      yp = y;
      rp = yp*w + xp;
      if(xp < w-HLBORDER-3 && d[rp] == 0) 
      {
        min_x = MIN(min_x, xp);
        max_x = MAX(max_x, xp);
        min_y = MIN(min_y, yp);
        max_y = MAX(max_y, yp);
        d[rp] = HLMAXSEGMENTS+id;
      }

      int xl = x - 1;
      lastXUp = firstXUp;
      lastXDown = firstXDown;
      while(xl >= HLBORDER && d[y*w+xl] == 1)
      {
        d[y*w+xl] = id;
        cnt++;
        if(yUp >= HLBORDER && d[yUp*w+xl] == 1)
        {
          if(!lastXUp) { push_stack(xl, yUp, stack); lastXUp = TRUE; }
        }
        else
        {
          xp = xl;
          yp = yUp;
          rp = yp*w + xp;
          if(yp > HLBORDER+2 && d[rp] == 0)
          {
            min_x = MIN(min_x, xp);
            max_x = MAX(max_x, xp);
            min_y = MIN(min_y, yp);
            max_y = MAX(max_y, yp);
            d[rp] = HLMAXSEGMENTS+id;
          }
          lastXUp = FALSE;
        }

        if(yDown < h-HLBORDER && d[yDown*w+xl] == 1)
        {
          if(!lastXDown) { push_stack(xl, yDown, stack); lastXDown = TRUE; }
        }
        else
        {
          xp = xl;
          yp = yDown;
          rp = yp*w + xp;
          if(yp < h-HLBORDER-3 && d[rp] == 0)
          {
            min_x = MIN(min_x, xp);
            max_x = MAX(max_x, xp);
            min_y = MIN(min_y, yp);
            max_y = MAX(max_y, yp);
            d[rp] = HLMAXSEGMENTS+id;
          }
          lastXDown = FALSE;
        }
        xl--;
      }

      d[y*w+x] = id;

      xp = xl;
      yp = y;
      rp = yp*w + xp;
      if(xp > HLBORDER+2 && d[rp] == 0)
      {
        min_x = MIN(min_x, xp);
        max_x = MAX(max_x, xp);
        min_y = MIN(min_y, yp);
        max_y = MAX(max_y, yp);
        d[rp] = HLMAXSEGMENTS+id;
      }
      cnt++;
    }
  }

  seg->size[id] = cnt;
  seg->xmin[id] = min_x;
  seg->xmax[id] = max_x;
  seg->ymin[id] = min_y;
  seg->ymax[id] = max_y;
  if(cnt) seg->nr += 1;
  return cnt;
}

static void segmentize_plane(dt_iop_segmentation_t *seg, const int width, const int height)
{
  dt_ff_stack_t stack;  
  stack.el = dt_alloc_align(16, width * height * sizeof(int));
  if(!stack.el) return;
 
  int id = 2;
  for(int row = HLBORDER; row < height - HLBORDER; row++)
  {
    for(int col = HLBORDER; col < width - HLBORDER; col++)
    {
      if(id >= HLMAXSEGMENTS-1) goto finish;
      if(seg->data[width * row + col] == 1)
      {
        if(floodfill_segmentize(row, col, seg, width, height, id, &stack) > 0) id++;
      }
    }
  }

  finish:
  dt_free_align(stack.el);
}

