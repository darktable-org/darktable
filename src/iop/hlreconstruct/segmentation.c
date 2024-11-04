/*
    This file is part of darktable,
    Copyright (C) 2022-2023 darktable developers.

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
   All segmentation stuff works on int32 arrays, to allow performant operations we use an additional border.

   Morphological closing operation supporting radius up to 8, tuned for performance

   The segmentation algorithm uses a modified floodfill, while floddfilling it
   - also takes keeps track of the surrounding rectangle of every segment and
   - marks the segment border locations.

   Hanno Schwalm 2022/05
*/

#define DT_SEG_ID_MASK 0x40000

typedef struct dt_pos_t
{
  int xpos;
  int ypos;
} dt_pos_t;

typedef struct dt_iop_segmentation_t
{
  uint32_t *data; // holding segment id's for every location
  uint32_t *tmp;  // pointer to temporary buffer used for morphological operations
  int *size;      // size of each segment
  int *xmin;      // bounding rectangle for each segment
  int *xmax;
  int *ymin;
  int *ymax;
  float *val1;    // val1 and val2 are free to be used by the segmentation user
  float *val2;
  int nr;         // next index for found segments, starting with 2
  int border;     // while segmentizing we have a border region not used by the algo
  int slots;      // available segment id's
  int width;
  int height;
} dt_iop_segmentation_t;

typedef struct dt_ff_stack_t
{
  int pos;
  int size;
  dt_pos_t *el;
} dt_ff_stack_t;

static inline void _push_stack(int xpos, int ypos, dt_ff_stack_t *stack)
{
  const int i = stack->pos;
  if(i >= stack->size - 1)
  {
    dt_print(DT_DEBUG_ALWAYS, "[segmentation stack overflow] %i", stack->size);
    return;
  }
  stack->el[i].xpos = xpos;
  stack->el[i].ypos = ypos;
  stack->pos++;
}

static inline void _clear_segment_slot(dt_iop_segmentation_t *seg, uint32_t id)
{
  if(id > seg->slots-1)
    return;

  seg->size[id] = seg->xmin[id] = seg->xmax[id] = seg->ymin[id] = seg->ymax[id] = 0;
  seg->val1[id] = seg->val2[id] = 0.0f;
}

static inline dt_pos_t * _pop_stack(dt_ff_stack_t *stack)
{
  if(stack->pos > 0)
    stack->pos--;
  else
    dt_print(DT_DEBUG_ALWAYS, "[segmentation stack underflow]");
  return &stack->el[stack->pos];
}

static inline uint32_t _get_segment_id(dt_iop_segmentation_t *seg, const size_t loc)
{
  if(loc >= (size_t)(seg->width * (seg->height-seg->border)))
    return 0;

  const uint32_t id = seg->data[loc] & (DT_SEG_ID_MASK-1);
  return ((id < seg->nr) && (id > 1)) ? id : 0;
}

static inline int _test_dilate(const uint32_t *img, const size_t i, const size_t w1, const int radius)
{
  int retval = 0;
  retval = img[i-w1-1] | img[i-w1] | img[i-w1+1] |
           img[i-1]    | img[i]    | img[i+1] |
           img[i+w1-1] | img[i+w1] | img[i+w1+1];
  if(retval || (radius < 2)) return retval;

  const size_t w2 = 2*w1;
  retval = img[i-w2-1] | img[i-w2]   | img[i-w2+1] |
           img[i-w1-2] | img[i-w1+2] |
           img[i-2]    | img[i+2] |
           img[i+w1-2] | img[i+w1+2] |
           img[i+w2-1] | img[i+w2]   | img[i+w2+1];
  if(retval || (radius < 3)) return retval;

  const size_t w3 = 3*w1;
  retval = img[i-w3-2] | img[i-w3-1] | img[i-w3] | img[i-w3+1] | img[i-w3+2] |
           img[i-w2-3] | img[i-w2-2] | img[i-w2+2] | img[i-w2+3] |
           img[i-w1-3] | img[i-w1+3] |
           img[i-3]    | img[i+3]    |
           img[i+w1-3] | img[i+w1+3] |
           img[i+w2-3] | img[i+w2-2] | img[i+w2+2] | img[i+w2+3] |
           img[i+w3-2] | img[i+w3-1] | img[i+w3] | img[i+w3+1] | img[i+w3+2];
  if(retval || (radius < 4)) return retval;

  const size_t w4 = 4*w1;
  retval = img[i-w4-2] | img[i-w4-1] | img[i-w4] | img[i-w4+1] | img[i-w4+2] |
           img[i-w3-3] | img[i-w3+3] |
           img[i-w2-4] | img[i-w2+4] |
           img[i-w1-4] | img[i-w1+4] |
           img[i-4]    | img[i+4] |
           img[i+w1-4] | img[i+w1+4] |
           img[i+w2-4] | img[i+w2+4] |
           img[i+w3-3] | img[i+w3+3] |
           img[i+w4-2] | img[i+w4-1] | img[i+w4] | img[i+w4+1] | img[i+w4+2];
  if(retval || (radius < 5)) return retval;

  const size_t w5 = 5*w1;
  retval = img[i-w5-2] | img[i-w5-1] | img[i-w5] | img[i-w5+1] | img[i-w5+2] |
           img[i-w4-4] | img[i-w4-3] | img[i-w4+3] | img[i-w4+4] |
           img[i-w3-4] | img[i-w3+4] |
           img[i-w2-5] | img[i-w2+5] |
           img[i-w1-5] | img[i-w1+5] |
           img[i-5]    | img[i+5] |
           img[i+w1-5] | img[i+w1+5] |
           img[i+w2-5] | img[i+w2+5] |
           img[i+w3-4] | img[i+w3+4] |
           img[i+w4-4] | img[i+w4-3] | img[i+w4+3] | img[i+w4+4] |
           img[i+w5-2] | img[i+w5-1] | img[i+w5] | img[i+w5+1] | img[i+w5+2];
  if(retval || (radius < 6)) return retval;

  const size_t w6 = 6*w1;
  retval = img[i-w6-2] | img[i-w6-1] | img[i-w6] | img[i-w6+1] | img[i-w6+2] |
           img[i-w5-4] | img[i-w5-3] | img[i-w5+3] | img[i-w5+4] |
           img[i-w4-5] | img[i-w4+5] |
           img[i-w3-5] | img[i-w3+5] |
           img[i-w2-6] | img[i-w2+6] |
           img[i-w1-6] | img[i-w1+6] |
           img[i-6]    | img[i+6] |
           img[i+w1-6] | img[i+w1+6] |
           img[i+w2-6] | img[i+w2+6] |
           img[i+w3-5] | img[i+w3+5] |
           img[i+w4-5] | img[i+w4+5] |
           img[i+w5-4] | img[i+w5-3] | img[i+w5+3] | img[i+w5+4] |
           img[i+w6-2] | img[i+w6-1] | img[i+w6] | img[i+w6+1] | img[i+w6+2] ;
  if(retval || (radius < 7)) return retval;

  const size_t w7 = 7*w1;
  retval = img[i-w7-3] | img[i-w7-2] | img[i-w7-1] | img[i-w7] | img[i-w7+1] | img[i-w7+2] | img[i-w7+3] |
           img[i-w6-4] | img[i-w6-3] | img[i-w6+3] | img[i-w6+4] |
           img[i-w5-6] | img[i-w5-5] | img[i-w5+5] | img[i-w5+6] |
           img[i-w4-6] | img[i-w4+6] |
           img[i-w3-7] | img[i-w3-6] | img[i-w3+6] | img[i-w3+7] |
           img[i-w2-7] | img[i-w2+7] |
           img[i-w1-7] | img[i-w1+7] |
           img[i-7]    | img[i+7] |
           img[i+w1-7] | img[i+w1+7] |
           img[i+w2-7] | img[i+w2+7] |
           img[i+w3-7] | img[i+w3-6] | img[i+w3+6] | img[i+w3+7] |
           img[i+w4-6] | img[i+w4+6] |
           img[i+w5-6] | img[i+w5-5] | img[i+w5+5] | img[i+w5+6] |
           img[i+w6-4] | img[i+w6-3] | img[i+w6+3] | img[i+w6+4] |
           img[i+w7-3] | img[i+w7-2] | img[i+w7-1] | img[i+w7] | img[i+w7+1] | img[i+w7+2] | img[i+w7+3];
  if(retval || (radius < 8)) return retval;

  const size_t w8 = 8*w1;
  retval = img[i-w8-4] | img[i-w8-3] | img[i-w8-2] | img[i-w8-1] | img[i-w8] | img[i-w8+1] | img[i-w8+2] | img[i-w8+3] | img[i-w8+4] |
           img[i-w7-6] | img[i-w7-5] | img[i-w7-4] | img[i-w7+4] | img[i-w7+5] | img[i-w7+6] |
           img[i-w6-6] | img[i-w6-5] | img[i-w6+5] | img[i-w6+6] |
           img[i-w5-7] | img[i-w5+6] |
           img[i-w4-8] | img[i-w4-7] | img[i-w4+7] | img[i-w4+8] |
           img[i-w3-8] | img[i-w3-7] | img[i-w3+7] | img[i-w3+8] |
           img[i-w2-8] | img[i-w2+8] |
           img[i-w1-8] | img[i-w1+8] |
           img[i-8]    | img[i+8] |
           img[i+w1-8] | img[i+w1+8] |
           img[i+w2-8] | img[i+w2+8] |
           img[i+w3-8] | img[i+w3-7] | img[i+w3+7] | img[i+w3+8] |
           img[i+w4-8] | img[i+w4-7] | img[i+w4+7] | img[i+w4+8] |
           img[i+w5-7] | img[i+w5+7] |
           img[i+w6-6] | img[i+w6-5] | img[i+w6+5] | img[i+w6+6] |
           img[i+w7-6] | img[i+w7-5] | img[i+w7-4] | img[i+w7+4] | img[i+w7+5] | img[i-w7+6] |
           img[i+w8-4] | img[i+w8-3] | img[i+w8-2] | img[i+w8-1] | img[i+w8] | img[i+w8+1] | img[i+w8+2] | img[i+w8+3] | img[i+w8+4];
  return retval;
}

static inline void _dilating(const uint32_t *img,
                             uint32_t *o,
                             const int w1,
                             const int height,
                             const int border,
                             const int radius)
{
  DT_OMP_FOR(collapse(2))
  for(int row = border; row < height - border; row++)
  {
    for(int col = border; col < w1 - border; col++)
    {
      const size_t i = (size_t)row*w1 + col;
      o[i] = _test_dilate(img, i, w1, radius) ? 1 : 0;
    }
  }
}

static inline int _test_erode(const uint32_t *img, const size_t i, const size_t w1, const int radius)
{
  int retval = 1;
  retval =     img[i-w1-1] & img[i-w1] & img[i-w1+1] &
               img[i-1]    & img[i]    & img[i+1] &
               img[i+w1-1] & img[i+w1] & img[i+w1+1];
  if((retval == 0) || (radius < 2)) return retval;

  const size_t w2 = 2*w1;
  retval = img[i-w2-1] & img[i-w2]   & img[i-w2+1] &
           img[i-w1-2] & img[i-w1+2] &
           img[i-2]    & img[i+2] &
           img[i+w1-2] & img[i+w1+2] &
           img[i+w2-1] & img[i+w2]   & img[i+w2+1];
  if((retval == 0) || (radius < 3)) return retval;

  const size_t w3 = 3*w1;
  retval = img[i-w3-2] & img[i-w3-1] & img[i-w3] & img[i-w3+1] & img[i-w3+2] &
           img[i-w2-3] & img[i-w2-2] & img[i-w2+2] & img[i-w2+3] &
           img[i-w1-3] & img[i-w1+3] &
           img[i-3]    & img[i+3]    &
           img[i+w1-3] & img[i+w1+3] &
           img[i+w2-3] & img[i+w2-2] & img[i+w2+2] & img[i+w2+3] &
           img[i+w3-2] & img[i+w3-1] & img[i+w3] & img[i+w3+1] & img[i+w3+2];
  if((retval == 0) || (radius < 4)) return retval;

  const size_t w4 = 4*w1;
  retval = img[i-w4-2] & img[i-w4-1] & img[i-w4] & img[i-w4+1] & img[i-w4+2] &
           img[i-w3-3] & img[i-w3+3] &
           img[i-w2-4] & img[i-w2+4] &
           img[i-w1-4] & img[i-w1+4] &
           img[i-4]    & img[i+4] &
           img[i+w1-4] & img[i+w1+4] &
           img[i+w2-4] & img[i+w2+4] &
           img[i+w3-3] & img[i+w3+3] &
           img[i+w4-2] & img[i+w4-1] & img[i+w4] & img[i+w4+1] & img[i+w4+2];
  if((retval == 0) || (radius < 5)) return retval;

  const size_t w5 = 5*w1;
  retval = img[i-w5-2] & img[i-w5-1] & img[i-w5] & img[i-w5+1] & img[i-w5+2] &
           img[i-w4-4] & img[i-w4-3] & img[i-w4+3] & img[i-w4+4] &
           img[i-w3-4] & img[i-w3+4] &
           img[i-w2-5] & img[i-w2+5] &
           img[i-w1-5] & img[i-w1+5] &
           img[i-5]    & img[i+5] &
           img[i+w1-5] & img[i+w1+5] &
           img[i+w2-5] & img[i+w2+5] &
           img[i+w3-4] & img[i+w3+4] &
           img[i+w4-4] & img[i+w4-3] & img[i+w4+3] & img[i+w4+4] &
           img[i+w5-2] & img[i+w5-1] & img[i+w5] & img[i+w5+1] & img[i+w5+2];
  return retval;
}

static inline void _eroding(const uint32_t *img,
                            uint32_t *o,
                            const int w1,
                            const int height,
                            const int border,
                            const int radius)
{
  DT_OMP_FOR(collapse(2))
  for(int row = border; row < height - border; row++)
  {
    for(int col = border; col < w1 - border; col++)
    {
      const size_t i = (size_t)row*w1 + col;
      o[i] = _test_erode(img, i, w1, radius) ? 1 : 0;
    }
  }
}

static inline void _intimage_borderfill(uint32_t *d,
                                        const int width,
                                        const int height,
                                        const int val,
                                        const int border)
{
  const size_t di = (height - border - 1) * width;
  for(size_t i = 0; i < border * width; i++)
    d[i] = d[i + di] = val;

  for(size_t row = border; row < height - border; row++)
  {
    const size_t j = row * width;
    const size_t dj = width - border;
    for(size_t i = 0; i < border; i++)
      d[j+i] = d[j+i+dj] = val;
  }
}

static gboolean _floodfill_segmentize(int yin,
                                      int xin,
                                      dt_iop_segmentation_t *seg,
                                      const int w,
                                      const int h,
                                      const int id,
                                      dt_ff_stack_t *stack)
{
  if(id >= seg->slots - 2) return FALSE;

  const int border = seg->border;
  uint32_t *d = seg->data;
  int xp = 0;
  int yp = 0;
  size_t rp = 0;
  int min_x = xin;
  int max_x = xin;
  int min_y = yin;
  int max_y = yin;
  int cnt = 0;
  stack->pos = 0;
  _clear_segment_slot(seg, id);

  _push_stack(xin, yin, stack);
  while(stack->pos)
  {
    dt_pos_t *coord = _pop_stack(stack);
    const int x = coord->xpos;
    const int y = coord->ypos;
    if(d[y*w+x] == 1)
    {
      int yUp = y - 1, yDown = y + 1;
      gboolean lastXUp = FALSE, lastXDown = FALSE, firstXUp = FALSE, firstXDown = FALSE;
      d[y*w+x] = id;
      cnt++;
      if(yUp >= border && d[yUp*w+x] == 1)
      {
        _push_stack(x, yUp, stack); firstXUp = lastXUp = TRUE;
      }
      else
      {
        xp = x;
        yp = yUp;
        rp = yp*w + xp;
        if(xp > border+1 && d[rp] == 0)
        {
          min_x = MIN(min_x, xp);
          max_x = MAX(max_x, xp);
          min_y = MIN(min_y, yp);
          max_y = MAX(max_y, yp);
          d[rp] = DT_SEG_ID_MASK | id;
        }
      }

      if(yDown < h-border && d[yDown*w+x] == 1)
      {
        _push_stack(x, yDown, stack); firstXDown = lastXDown = TRUE;
      }
      else
      {
        xp = x;
        yp = yDown;
        rp = yp*w + xp;
        if(yp < h-border-2 && d[rp] == 0)
        {
          min_x = MIN(min_x, xp);
          max_x = MAX(max_x, xp);
          min_y = MIN(min_y, yp);
          max_y = MAX(max_y, yp);
          d[rp] = DT_SEG_ID_MASK | id;
        }
      }

      int xr = x + 1;
      while(xr < w-border && d[y*w+xr] == 1)
      {
        d[y*w+xr] = id;
        cnt++;
        if(yUp >= border && d[yUp*w + xr] == 1)
        {
          if(!lastXUp) { _push_stack(xr, yUp, stack); lastXUp = TRUE; }
        }
        else
        {
          xp = xr;
          yp = yUp;
          rp = yp*w + xp;
          if(yp > border+1 && d[rp] == 0)
          {
            min_x = MIN(min_x, xp);
            max_x = MAX(max_x, xp);
            min_y = MIN(min_y, yp);
            max_y = MAX(max_y, yp);
            d[rp] = DT_SEG_ID_MASK | id;
          }
          lastXUp = FALSE;
        }

        if(yDown < h-border && d[yDown*w+xr] == 1)
        {
          if(!lastXDown) { _push_stack(xr, yDown, stack); lastXDown = TRUE; }
        }
        else
        {
          xp = xr;
          yp = yDown;
          rp = yp*w + xp;
          if(yp < h-border-2 && d[rp] == 0)
          {
            min_x = MIN(min_x, xp);
            max_x = MAX(max_x, xp);
            min_y = MIN(min_y, yp);
            max_y = MAX(max_y, yp);
            d[rp] = DT_SEG_ID_MASK | id;
          }
          lastXDown = FALSE;
        }
        xr++;
      }

      xp = xr;
      yp = y;
      rp = yp*w + xp;
      if(xp < w-border-2 && d[rp] == 0)
      {
        min_x = MIN(min_x, xp);
        max_x = MAX(max_x, xp);
        min_y = MIN(min_y, yp);
        max_y = MAX(max_y, yp);
        d[rp] = DT_SEG_ID_MASK | id;
      }

      int xl = x - 1;
      lastXUp = firstXUp;
      lastXDown = firstXDown;
      while(xl >= border && d[y*w+xl] == 1)
      {
        d[y*w+xl] = id;
        cnt++;
        if(yUp >= border && d[yUp*w+xl] == 1)
        {
          if(!lastXUp) { _push_stack(xl, yUp, stack); lastXUp = TRUE; }
        }
        else
        {
          xp = xl;
          yp = yUp;
          rp = yp*w + xp;
          if(yp > border+1 && d[rp] == 0)
          {
            min_x = MIN(min_x, xp);
            max_x = MAX(max_x, xp);
            min_y = MIN(min_y, yp);
            max_y = MAX(max_y, yp);
            d[rp] = DT_SEG_ID_MASK | id;
          }
          lastXUp = FALSE;
        }

        if(yDown < h-border && d[yDown*w+xl] == 1)
        {
          if(!lastXDown) { _push_stack(xl, yDown, stack); lastXDown = TRUE; }
        }
        else
        {
          xp = xl;
          yp = yDown;
          rp = yp*w + xp;
          if(yp < h-border-2 && d[rp] == 0)
          {
            min_x = MIN(min_x, xp);
            max_x = MAX(max_x, xp);
            min_y = MIN(min_y, yp);
            max_y = MAX(max_y, yp);
            d[rp] = DT_SEG_ID_MASK | id;
          }
          lastXDown = FALSE;
        }
        xl--;
      }

      d[y*w+x] = id;

      xp = xl;
      yp = y;
      rp = yp*w + xp;
      if(xp > border+1 && d[rp] == 0)
      {
        min_x = MIN(min_x, xp);
        max_x = MAX(max_x, xp);
        min_y = MIN(min_y, yp);
        max_y = MAX(max_y, yp);
        d[rp] = DT_SEG_ID_MASK | id;
      }
    }
  }

  const gboolean success = cnt > 3;
  if(!success) // To avoid oversegmentizing we only use segments with a minimum size of 4
  {
    // data in too In any case we want to revert border markings too
    for(int row = min_y; row <= max_y; row++)
    {
      for(int col = min_x; col <= max_x; col++)
      {
        size_t loc = (size_t)w*row + col;
        if(d[loc] == id)
          d[loc] = 1;
        else if(d[loc] == (id | DT_SEG_ID_MASK))
          d[loc] = 0;
      }
    }
  }
  else
  {
    seg->size[id] = cnt;
    seg->xmin[id] = min_x;
    seg->xmax[id] = max_x;
    seg->ymin[id] = min_y;
    seg->ymax[id] = max_y;

    seg->nr += 1;
    _clear_segment_slot(seg, id+1);
  }

  return success;
}

// User interface
void dt_segmentize_plane(dt_iop_segmentation_t *seg)
{
  dt_ff_stack_t stack;
  const int width = seg->width;
  const int height = seg->height;
  stack.size = (size_t)width * height / 32;
  stack.el = dt_alloc_align_type(dt_pos_t, stack.size);
  if(!stack.el)
  {
    dt_print(DT_DEBUG_ALWAYS, "[segmentize_plane] can't allocate segmentation stack");
    return;
  }
  const int border = seg->border;
  int id = 2;
  for(int row = border; row < height - border; row++)
  {
    for(int col = border; col < width - border; col++)
    {
      if(id >= (seg->slots - 2))
        goto finish;
      if(seg->data[(size_t)width * row + col] == 1)
      {
        if(_floodfill_segmentize(row, col, seg, width, height, id, &stack))
          id++;
      }
    }
  }

  finish:

  if(id >= (seg->slots - 2))
    dt_print(DT_DEBUG_ALWAYS, "[segmentize_plane] %ix%i number of segments exceeds maximum=%i",
             (int)width, (int)height, seg->slots);

  dt_free_align(stack.el);
}

void dt_segments_combine(dt_iop_segmentation_t *seg, const int radius)
{
  uint32_t *img = seg->data;
  const int width = seg->width;
  const int height = seg->height;
  const int border = seg->border;
  _intimage_borderfill(img, width, height, 0, border);

  _dilating(img, seg->tmp, width, height, border, radius);
  if(radius > 3)
  {
    _intimage_borderfill(seg->tmp, width, height, 1, border);
    _eroding(seg->tmp, img, width, height, border, radius-3);
  }
  else
    memcpy(img, seg->tmp, (size_t) width * height * sizeof(uint32_t));

  _intimage_borderfill(img, width, height, 0, border);
}

void dt_segmentation_free_struct(dt_iop_segmentation_t *seg)
{
  dt_free_align(seg->data);
  dt_free_align(seg->tmp);
  dt_free_align(seg->size);
  dt_free_align(seg->xmin);
  dt_free_align(seg->ymin);
  dt_free_align(seg->xmax);
  dt_free_align(seg->ymax);
  dt_free_align(seg->val1);
  dt_free_align(seg->val2);
  memset(seg, 0, sizeof(dt_iop_segmentation_t));
}

// returns TRUE in case of errors
gboolean dt_segmentation_init_struct(dt_iop_segmentation_t *seg,
                                     const int width,
                                     const int height,
                                     const int border,
                                     const int islots)
{
  memset(seg, 0, sizeof(dt_iop_segmentation_t));
  const int slots = MAX(256, MIN(islots, DT_SEG_ID_MASK - 2));
  const size_t bsize = (size_t) width * height * sizeof(uint32_t);

  seg->data =   dt_calloc_aligned(bsize);
  seg->tmp =    dt_alloc_aligned(bsize);
  seg->size =   dt_alloc_align_int(slots);
  seg->xmin =   dt_alloc_align_int(slots);
  seg->xmax =   dt_alloc_align_int(slots);
  seg->ymin =   dt_alloc_align_int(slots);
  seg->ymax =   dt_alloc_align_int(slots);
  seg->val1 =   dt_alloc_align_float(slots);
  seg->val2 =   dt_alloc_align_float(slots);

  if(!seg->data || !seg->size
                || !seg->xmin || !seg->xmax || !seg->ymin || !seg->ymax
                || !seg->val2 || !seg->val2)
  {
    dt_segmentation_free_struct(seg);
    return TRUE;
  }

  // allocation is fine so we now define struct data and initialize first two unused lines for safety
  seg->nr = 2;
  seg->border = border;
  seg->slots = slots;
  seg->width = width;
  seg->height = height;
  _clear_segment_slot(seg, 0);
  _clear_segment_slot(seg, 1);

  return FALSE;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
