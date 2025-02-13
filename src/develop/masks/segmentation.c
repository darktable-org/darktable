/*
    This file is part of darktable,
    Copyright (C) 2025 darktable developers.

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

/*
  Initial docs
  For each pipe we have the dt_dev_segmentation_t struct holding all required
  data to be used later by other modules requesting a segment mask.

  As we might want to ask for a combined mask of several segments we always
  pass a list of segments amd the number of segments it holds.

  For every location in the segments map we have segments uint8_t data, we might
  reconcider to use just bits for this but using uint8_t would allow some border attenuation.

  If any module wants a segmentation mask it
  a) must request to do so via dt_dev_pixelpipe_segmentation()
  b) gets the distorted mask for a list of segments via dt_dev_distort_segmentation_mask()

  Currently the segmentation module is default enabled and visible, processing each piece
  in the pipe will be disabled until dt_dev_pixelpipe_segmentation() has been called
  As we haven't decided yet about the model we allow several models for testing right now
  and pass a single parameter. This might all be changed/fixed after evaluation.

  Note: As we don't have a UI mask getter yet this is switched on via the details mask requested
  to allow preliminary testing.

  Some runtime logs are provided via the -d pipe switch.
*/


// generate a combined float mask with original dimensions
// *list holds the segments to be tested
float *dt_masks_get_ai_segments(dt_dev_pixelpipe_iop_t *piece,
                                const int tested,
                                const int *list)
{
  dt_dev_pixelpipe_t *pipe = piece->pipe;
  dt_dev_segmentation_t *seg = &pipe->segmentation;

  if(tested < 1 || list == NULL)
  {
    dt_print(DT_DEBUG_ALWAYS, "[dt_masks_get_ai_segments] no valid data provided");
    return NULL;
  }
  for(int i = 0; i < tested; i++)
  {
    if(list[i] < 1 || list[i] >= seg->segments)
    {
      dt_print(DT_DEBUG_ALWAYS, "[dt_masks_get_ai_segments] invalid segment %d (%d)", list[i], seg->segments);
      return NULL;
    }
  }

  const size_t owidth = seg->iwidth;
  const size_t oheight = seg->iheight;

  float *tmp = dt_calloc_align_float(owidth * oheight);
  float *mask = dt_alloc_align_float(owidth * oheight);
  if(!tmp || !mask)
  {
    dt_free_align(tmp);
    dt_free_align(mask);
    dt_print(DT_DEBUG_ALWAYS, "[dt_masks_get_ai_segments] could not allocate mask memory");
    return NULL;
  }

  const size_t iwidth = seg->swidth;
  const size_t iheight = seg->sheight;
  const float height_ratio = (float)iheight / (float)oheight;
  const float width_ratio = (float)iwidth / (float)owidth;

  DT_OMP_FOR()
  for(size_t row = 0; row < oheight; row++)
  {
    for(size_t col = 0; col < owidth; col++)
    {
      const size_t in_row = (size_t)((float)row * height_ratio);
      const size_t in_col = (size_t)((float)col * width_ratio);
      const size_t start = (size_t)seg->segments * (in_row*iwidth + in_col);
      float val = 0.0f;
      for(int i = 0; i < tested; i++)
        val = MAX(val, (float)seg->map[start + list[i]] / 255.0f);
      tmp[row*owidth + col] = val;
    }
  }

  dt_gaussian_fast_blur(tmp, mask, owidth, oheight, 1.0f, 0.0f, 1.0f, 1);
  dt_free_align(tmp);
  return mask;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
