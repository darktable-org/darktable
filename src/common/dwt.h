/*
    This file is part of darktable,
    copyright (c) 2017 edgardo hoszowski.

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

#ifndef DT_DEVELOP_DWT_H
#define DT_DEVELOP_DWT_H

/* structure returned by dt_dwt_init() to be used when calling dwt_decompose() */
typedef struct dwt_params_t
{
  float *image;
  int ch;
  int width;
  int height;
  int scales;
  int return_layer;
  int merge_from_scale;
  void *user_data;
  float preview_scale;
  int use_sse;
} dwt_params_t;

/* function prototype for the layer_func on dwt_decompose() call */
typedef void(_dwt_layer_func)(float *layer, dwt_params_t *const p, const int scale);

/* returns a structure used when calling dwt_decompose(), free it with dt_dwt_free()
 * image: image to be decomposed and output image
 * width, height, ch: dimensions of the image
 * scales: number of scales to decompose, if > dwt_get_max_scale() the this last will be used
 * return_layer: 0 returns the recomposed image, 1..scales returns a detail scale, scales+1 returns the residual
 * image
 * merge_from_scale: detail scales will be merged together before calling layer_func
 * user_data: user-supplied data to be passed to layer_func on each call
 * preview_scale: image scale (zoom factor)
 * use_sse: use SSE instructions
 */
dwt_params_t *dt_dwt_init(float *image, const int width, const int height, const int ch, const int scales,
                          const int return_layer, const int merge_from_scale, void *user_data,
                          const float preview_scale, const int use_sse);

/* free resources used by dwt_decompose() */
void dt_dwt_free(dwt_params_t *p);

/* returns the maximum number of scales that dwt_decompose() will accept for the current image size */
int dwt_get_max_scale(dwt_params_t *p);

/* returns the first visible detail scale at the current zoom level */
int dt_dwt_first_scale_visible(dwt_params_t *p);

/* decomposes an image into several wavelet scales
 * p: returned by dt_dwt_init()
 * layer_func: this function is called for the original image and then once for each scale, including the residual
 * image
 */
void dwt_decompose(dwt_params_t *p, _dwt_layer_func layer_func);

#ifdef HAVE_OPENCL
typedef struct dt_dwt_cl_global_t
{
  int kernel_dwt_add_img_to_layer;
  int kernel_dwt_subtract_layer;
  int kernel_dwt_hat_transform_col;
  int kernel_dwt_hat_transform_row;
  int kernel_dwt_init_buffer;
} dt_dwt_cl_global_t;

typedef struct dwt_params_cl_t
{
  dt_dwt_cl_global_t *global;
  int devid;
  cl_mem image;
  int width;
  int height;
  int ch;
  int scales;
  int return_layer;
  int merge_from_scale;
  void *user_data;
  float preview_scale;
} dwt_params_cl_t;

typedef cl_int(_dwt_layer_func_cl)(cl_mem layer, dwt_params_cl_t *const p, const int scale);

dt_dwt_cl_global_t *dt_dwt_init_cl_global(void);
void dt_dwt_free_cl_global(dt_dwt_cl_global_t *g);

dwt_params_cl_t *dt_dwt_init_cl(const int devid, cl_mem image, const int width, const int height, const int scales,
                                const int return_layer, const int merge_from_scale, void *user_data,
                                const float preview_scale);
void dt_dwt_free_cl(dwt_params_cl_t *p);

int dwt_get_max_scale_cl(dwt_params_cl_t *p);

int dt_dwt_first_scale_visible_cl(dwt_params_cl_t *p);

cl_int dwt_decompose_cl(dwt_params_cl_t *p, _dwt_layer_func_cl layer_func);

#endif

#endif
