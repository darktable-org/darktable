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

#include "control/control.h"
#include "develop/imageop.h"
#include "dwt.h"
#if defined(__SSE__)
#include <xmmintrin.h>
#endif

/* Based on the original source code of GIMP's Wavelet Decompose plugin, by Marco Rossini
 *
 * http://registry.gimp.org/node/11742
 *
*/

dwt_params_t *dt_dwt_init(float *image, const int width, const int height, const int ch, const int scales,
                          const int return_layer, const int merge_from_scale, void *user_data,
                          const float preview_scale, const int use_sse)
{
  dwt_params_t *p = (dwt_params_t *)malloc(sizeof(dwt_params_t));
  if(!p) return NULL;

  p->image = image;
  p->ch = ch;
  p->width = width;
  p->height = height;
  p->scales = scales;
  p->return_layer = return_layer;
  p->merge_from_scale = merge_from_scale;
  p->user_data = user_data;
  p->preview_scale = preview_scale;
  p->use_sse = use_sse;

  return p;
}

void dt_dwt_free(dwt_params_t *p)
{
  if(!p) return;

  free(p);
}

static int _get_max_scale(const int width, const int height, const float preview_scale)
{
  int maxscale = 0;

  // smallest edge must be higher than or equal to 2^scales
  unsigned int size = MIN(width, height);
  float size_tmp = ((size >>= 1) * preview_scale);
  while(size_tmp > 0.f)
  {
    size_tmp = ((size >>= 1) * preview_scale);
    maxscale++;
  }

  // avoid rounding issues...
  size = MIN(width, height);
  while((maxscale > 0) && ((1 << maxscale) * preview_scale >= size)) maxscale--;

  return maxscale;
}

int dwt_get_max_scale(dwt_params_t *p)
{
  return _get_max_scale(p->width / p->preview_scale, p->height / p->preview_scale, p->preview_scale);
}

int _first_scale_visible(const int num_scales, const float preview_scale)
{
  int first_scale = 0;

  for(unsigned int lev = 0; lev < num_scales; lev++)
  {
    int sc = 1 << lev;
    sc *= preview_scale;
    if(sc > 0)
    {
      first_scale = lev + 1;
      break;
    }
  }

  return first_scale;
}

int dt_dwt_first_scale_visible(dwt_params_t *p)
{
  return _first_scale_visible(p->scales, p->preview_scale);
}

#define INDEX_WT_IMAGE(index, num_channels, channel) (((index) * (num_channels)) + (channel))
#define INDEX_WT_IMAGE_SSE(index, num_channels) ((index) * (num_channels))

/* code copied from UFRaw (which originates from dcraw) */
#if defined(__SSE__)
static void dwt_hat_transform_sse(float *temp, const float *const base, const int st, const int size, int sc,
                                  const dwt_params_t *const p)
{
  int i;
  const __m128 hat_mult = _mm_set1_ps(2.f);
  __m128 valb_1, valb_2, valb_3;
  sc = (int)(sc * p->preview_scale);
  if(sc > size) sc = size;

  for(i = 0; i < sc; i++, temp += 4)
  {
    valb_1 = _mm_load_ps(&base[INDEX_WT_IMAGE_SSE(st * i, p->ch)]);
    valb_2 = _mm_load_ps(&base[INDEX_WT_IMAGE_SSE(st * (sc - i), p->ch)]);
    valb_3 = _mm_load_ps(&base[INDEX_WT_IMAGE_SSE(st * (i + sc), p->ch)]);

    _mm_store_ps(temp, _mm_add_ps(_mm_add_ps(_mm_mul_ps(hat_mult, valb_1), valb_2), valb_3));
  }
  for(; i + sc < size; i++, temp += 4)
  {
    valb_1 = _mm_load_ps(&base[INDEX_WT_IMAGE_SSE(st * i, p->ch)]);
    valb_2 = _mm_load_ps(&base[INDEX_WT_IMAGE_SSE(st * (i - sc), p->ch)]);
    valb_3 = _mm_load_ps(&base[INDEX_WT_IMAGE_SSE(st * (i + sc), p->ch)]);

    _mm_store_ps(temp, _mm_add_ps(_mm_add_ps(_mm_mul_ps(hat_mult, valb_1), valb_2), valb_3));
  }
  for(; i < size; i++, temp += 4)
  {
    valb_1 = _mm_load_ps(&base[INDEX_WT_IMAGE_SSE(st * i, p->ch)]);
    valb_2 = _mm_load_ps(&base[INDEX_WT_IMAGE_SSE(st * (i - sc), p->ch)]);
    valb_3 = _mm_load_ps(&base[INDEX_WT_IMAGE_SSE(st * (2 * size - 2 - (i + sc)), p->ch)]);

    _mm_store_ps(temp, _mm_add_ps(_mm_add_ps(_mm_mul_ps(hat_mult, valb_1), valb_2), valb_3));
  }
}
#endif

static void dwt_hat_transform(float *temp, const float *const base, const int st, const int size, int sc,
                              dwt_params_t *const p)
{
#if defined(__SSE__)
  if(p->ch == 4 && p->use_sse)
  {
    dwt_hat_transform_sse(temp, base, st, size, sc, p);
    return;
  }
#endif

  int i, c;
  const float hat_mult = 2.f;
  sc = (int)(sc * p->preview_scale);
  if(sc > size) sc = size;

  for(i = 0; i < sc; i++)
  {
    for(c = 0; c < p->ch; c++, temp++)
    {
      *temp = hat_mult * base[INDEX_WT_IMAGE(st * i, p->ch, c)] + base[INDEX_WT_IMAGE(st * (sc - i), p->ch, c)]
              + base[INDEX_WT_IMAGE(st * (i + sc), p->ch, c)];
    }
  }
  for(; i + sc < size; i++)
  {
    for(c = 0; c < p->ch; c++, temp++)
    {
      *temp = hat_mult * base[INDEX_WT_IMAGE(st * i, p->ch, c)] + base[INDEX_WT_IMAGE(st * (i - sc), p->ch, c)]
              + base[INDEX_WT_IMAGE(st * (i + sc), p->ch, c)];
    }
  }
  for(; i < size; i++)
  {
    for(c = 0; c < p->ch; c++, temp++)
    {
      *temp = hat_mult * base[INDEX_WT_IMAGE(st * i, p->ch, c)] + base[INDEX_WT_IMAGE(st * (i - sc), p->ch, c)]
              + base[INDEX_WT_IMAGE(st * (2 * size - 2 - (i + sc)), p->ch, c)];
    }
  }
}

#if defined(__SSE__)
static void dwt_add_layer_sse(float *const img, float *layers, dwt_params_t *const p, const int n_scale)
{
  const int i_size = p->width * p->height * 4;

#ifdef _OPENMP
#pragma omp parallel for default(none) shared(layers) schedule(static)
#endif
  for(int i = 0; i < i_size; i += 4)
  {
    _mm_store_ps(&(layers[i]), _mm_add_ps(_mm_load_ps(&(layers[i])), _mm_load_ps(&(img[i]))));
  }
}
#endif

static void dwt_add_layer(float *const img, float *layers, dwt_params_t *const p, const int n_scale)
{
#if defined(__SSE__)
  if(p->ch == 4 && p->use_sse)
  {
    dwt_add_layer_sse(img, layers, p, n_scale);
    return;
  }
#endif

  const int i_size = p->width * p->height * p->ch;

#ifdef _OPENMP
#pragma omp parallel for default(none) shared(layers) schedule(static)
#endif
  for(int i = 0; i < i_size; i++) layers[i] += img[i];
}

static void dwt_get_image_layer(float *const layer, dwt_params_t *const p)
{
  if(p->image != layer) memcpy(p->image, layer, p->width * p->height * p->ch * sizeof(float));
}

#if defined(__SSE__)
static void dwt_subtract_layer_sse(float *bl, float *bh, dwt_params_t *const p)
{
  const __m128 v4_lpass_mult = _mm_set1_ps((1.f / 16.f));
  const int size = p->width * p->height * 4;

#ifdef _OPENMP
#pragma omp parallel for default(none) shared(bl, bh) schedule(static)
#endif
  for(int i = 0; i < size; i += 4)
  {
    // rounding errors introduced here (division by 16)
    _mm_store_ps(&(bl[i]), _mm_mul_ps(_mm_load_ps(&(bl[i])), v4_lpass_mult));
    _mm_store_ps(&(bh[i]), _mm_sub_ps(_mm_load_ps(&(bh[i])), _mm_load_ps(&(bl[i]))));
  }
}
#endif

static void dwt_subtract_layer(float *bl, float *bh, dwt_params_t *const p)
{
#if defined(__SSE__)
  if(p->ch == 4 && p->use_sse)
  {
    dwt_subtract_layer_sse(bl, bh, p);
    return;
  }
#endif

  const float lpass_mult = (1.f / 16.f);
  const int size = p->width * p->height * p->ch;

#ifdef _OPENMP
#pragma omp parallel for default(none) shared(bl, bh) schedule(static)
#endif
  for(int i = 0; i < size; i++)
  {
    // rounding errors introduced here (division by 16)
    bl[i] = bl[i] * lpass_mult;
    bh[i] -= bl[i];
  }
}

/* actual decomposing algorithm */
static void dwt_wavelet_decompose(float *img, dwt_params_t *const p, _dwt_layer_func layer_func)
{
  float *temp = NULL;
  float *layers = NULL;
  float *merged_layers = NULL;
  unsigned int lpass, hpass;
  float *buffer[2] = { 0, 0 };
  int bcontinue = 1;
  const int size = p->width * p->height * p->ch;

  if(layer_func) layer_func(img, p, 0);

  if(p->scales <= 0) goto cleanup;

  /* image buffers */
  buffer[0] = img;
  /* temporary storage */
  buffer[1] = dt_alloc_align(64, size * sizeof(float));
  if(buffer[1] == NULL)
  {
    printf("not enough memory for wavelet decomposition");
    goto cleanup;
  }
  memset(buffer[1], 0, size * sizeof(float));

  // setup a temp buffer
  temp = dt_alloc_align(64, MAX(p->width, p->height) * p->ch * sizeof(float));
  if(temp == NULL)
  {
    printf("not enough memory for wavelet decomposition");
    goto cleanup;
  }
  memset(temp, 0, MAX(p->width, p->height) * p->ch * sizeof(float));

  // buffer to reconstruct the image
  layers = dt_alloc_align(64, p->width * p->height * p->ch * sizeof(float));
  if(layers == NULL)
  {
    printf("not enough memory for wavelet decomposition");
    goto cleanup;
  }
  memset(layers, 0, p->width * p->height * p->ch * sizeof(float));

  if(p->merge_from_scale > 0)
  {
    merged_layers = dt_alloc_align(64, p->width * p->height * p->ch * sizeof(float));
    if(merged_layers == NULL)
    {
      printf("not enough memory for wavelet decomposition");
      goto cleanup;
    }
    memset(merged_layers, 0, p->width * p->height * p->ch * sizeof(float));
  }

  // iterate over wavelet scales
  lpass = 1;
  hpass = 0;
  for(unsigned int lev = 0; lev < p->scales && bcontinue; lev++)
  {
    lpass = (1 - (lev & 1));

    for(int row = 0; row < p->height; row++)
    {
      dwt_hat_transform(temp, buffer[hpass] + (row * p->width * p->ch), 1, p->width, 1 << lev, p);
      memcpy(&(buffer[lpass][row * p->width * p->ch]), temp, p->width * p->ch * sizeof(float));
    }

    for(int col = 0; col < p->width; col++)
    {
      dwt_hat_transform(temp, buffer[lpass] + col * p->ch, p->width, p->height, 1 << lev, p);
      for(int row = 0; row < p->height; row++)
      {
        for(int c = 0; c < p->ch; c++)
          buffer[lpass][INDEX_WT_IMAGE(row * p->width + col, p->ch, c)] = temp[INDEX_WT_IMAGE(row, p->ch, c)];
      }
    }

    dwt_subtract_layer(buffer[lpass], buffer[hpass], p);

    // no merge scales or we didn't reach the merge scale from yet
    if(p->merge_from_scale == 0 || p->merge_from_scale > lev + 1)
    {
      // allow to process this detail scale
      if(layer_func) layer_func(buffer[hpass], p, lev + 1);

      // user wants to preview this detail scale
      if(p->return_layer == lev + 1)
      {
        // return this detail scale
        dwt_get_image_layer(buffer[hpass], p);

        bcontinue = 0;
      }
      // user wants the entire reconstructed image
      else if(p->return_layer == 0)
      {
        // add this detail scale to the final image
        dwt_add_layer(buffer[hpass], layers, p, lev + 1);
      }
    }
    // we are on the merge scales range
    else
    {
      // add this detail scale to the merged ones
      dwt_add_layer(buffer[hpass], merged_layers, p, lev + 1);

      // allow to process this merged scale
      if(layer_func) layer_func(merged_layers, p, lev + 1);

      // user wants to preview this merged scale
      if(p->return_layer == lev + 1)
      {
        // return this merged scale
        dwt_get_image_layer(merged_layers, p);

        bcontinue = 0;
      }
    }

    hpass = lpass;
  }

  // all scales have been processed
  if(bcontinue)
  {
    // allow to process residual image
    if(layer_func) layer_func(buffer[hpass], p, p->scales + 1);

    // user wants to preview residual image
    if(p->return_layer == p->scales + 1)
    {
      // return residual image
      dwt_get_image_layer(buffer[hpass], p);
    }
    // return reconstructed image
    else if(p->return_layer == 0)
    {
      // some of the detail scales are on the merged layers
      if(p->merge_from_scale > 0)
      {
        // add merged layers to final image
        dwt_add_layer(merged_layers, layers, p, p->scales + 1);
      }

      // add residual image to final image
      dwt_add_layer(buffer[hpass], layers, p, p->scales + 1);

      // allow to process reconstructed image
      if(layer_func) layer_func(layers, p, p->scales + 2);

      // return reconstructed image
      dwt_get_image_layer(layers, p);
    }
  }

cleanup:
  if(layers) dt_free_align(layers);
  if(merged_layers) dt_free_align(merged_layers);
  if(temp) dt_free_align(temp);
  if(buffer[1]) dt_free_align(buffer[1]);
}

#undef INDEX_WT_IMAGE
#undef INDEX_WT_IMAGE_SSE

/* this function prepares for decomposing, which is done in the function dwt_wavelet_decompose() */
void dwt_decompose(dwt_params_t *p, _dwt_layer_func layer_func)
{
  // this is a zoom scale, not a wavelet scale
  if(p->preview_scale <= 0.f) p->preview_scale = 1.f;

  // if a single scale is requested it cannot be grather than the residual
  if(p->return_layer > p->scales + 1)
  {
    p->return_layer = p->scales + 1;
  }

  const int max_scale = dwt_get_max_scale(p);

  // if requested scales is grather than max scales adjust it
  if(p->scales > max_scale)
  {
    // residual shoud be returned
    if(p->return_layer > p->scales) p->return_layer = max_scale + 1;
    // a scale should be returned, it cannot be grather than max scales
    else if(p->return_layer > max_scale)
      p->return_layer = max_scale;

    p->scales = max_scale;
  }

  // call the actual decompose
  dwt_wavelet_decompose(p->image, p, layer_func);
}

#ifdef HAVE_OPENCL
dt_dwt_cl_global_t *dt_dwt_init_cl_global()
{
  dt_dwt_cl_global_t *g = (dt_dwt_cl_global_t *)malloc(sizeof(dt_dwt_cl_global_t));

  const int program = 20; // dwt.cl, from programs.conf
  g->kernel_dwt_add_img_to_layer = dt_opencl_create_kernel(program, "dwt_add_img_to_layer");
  g->kernel_dwt_subtract_layer = dt_opencl_create_kernel(program, "dwt_subtract_layer");
  g->kernel_dwt_hat_transform_col = dt_opencl_create_kernel(program, "dwt_hat_transform_col");
  g->kernel_dwt_hat_transform_row = dt_opencl_create_kernel(program, "dwt_hat_transform_row");
  g->kernel_dwt_init_buffer = dt_opencl_create_kernel(program, "dwt_init_buffer");
  return g;
}

void dt_dwt_free_cl_global(dt_dwt_cl_global_t *g)
{
  if(!g) return;

  // destroy kernels
  dt_opencl_free_kernel(g->kernel_dwt_add_img_to_layer);
  dt_opencl_free_kernel(g->kernel_dwt_subtract_layer);
  dt_opencl_free_kernel(g->kernel_dwt_hat_transform_col);
  dt_opencl_free_kernel(g->kernel_dwt_hat_transform_row);
  dt_opencl_free_kernel(g->kernel_dwt_init_buffer);

  free(g);
}

dwt_params_cl_t *dt_dwt_init_cl(const int devid, cl_mem image, const int width, const int height, const int scales,
                                const int return_layer, const int merge_from_scale, void *user_data,
                                const float preview_scale)
{
  dwt_params_cl_t *p = (dwt_params_cl_t *)malloc(sizeof(dwt_params_cl_t));
  if(!p) return NULL;

  p->global = darktable.opencl->dwt;
  p->devid = devid;
  p->image = image;
  p->ch = 4;
  p->width = width;
  p->height = height;
  p->scales = scales;
  p->return_layer = return_layer;
  p->merge_from_scale = merge_from_scale;
  p->user_data = user_data;
  p->preview_scale = preview_scale;

  return p;
}

void dt_dwt_free_cl(dwt_params_cl_t *p)
{
  if(!p) return;

  // be sure we're done with the memory:
  dt_opencl_finish(p->devid);

  free(p);
}

int dwt_get_max_scale_cl(dwt_params_cl_t *p)
{
  return _get_max_scale(p->width / p->preview_scale, p->height / p->preview_scale, p->preview_scale);
}

int dt_dwt_first_scale_visible_cl(dwt_params_cl_t *p)
{
  return _first_scale_visible(p->scales, p->preview_scale);
}

static cl_int dwt_subtract_layer_cl(cl_mem bl, cl_mem bh, dwt_params_cl_t *const p)
{
  cl_int err = CL_MEM_OBJECT_ALLOCATION_FAILURE;

  const int devid = p->devid;
  const int kernel = p->global->kernel_dwt_subtract_layer;

  size_t sizes[] = { ROUNDUPWD(p->width), ROUNDUPHT(p->height), 1 };

  const float lpass_mult = (1.f / 16.f);
  const int width = p->width;
  const int height = p->height;

  dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), (void *)&bl);
  dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), (void *)&bh);
  dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(int), (void *)&(width));
  dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), (void *)&(height));
  dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(float), (void *)&lpass_mult);
  err = dt_opencl_enqueue_kernel_2d(devid, kernel, sizes);

  return err;
}

static cl_int dwt_add_layer_cl(cl_mem img, cl_mem layers, dwt_params_cl_t *const p, const int n_scale)
{
  cl_int err = CL_MEM_OBJECT_ALLOCATION_FAILURE;

  const int devid = p->devid;
  const int kernel = p->global->kernel_dwt_add_img_to_layer;

  size_t sizes[] = { ROUNDUPWD(p->width), ROUNDUPHT(p->height), 1 };

  const int width = p->width;
  const int height = p->height;

  dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), (void *)&img);
  dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), (void *)&layers);
  dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(int), (void *)&(width));
  dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), (void *)&(height));
  err = dt_opencl_enqueue_kernel_2d(devid, kernel, sizes);

  return err;
}

static cl_int dwt_get_image_layer_cl(cl_mem layer, dwt_params_cl_t *const p)
{
  cl_int err = CL_SUCCESS;

  if(p->image != layer)
    err = dt_opencl_enqueue_copy_buffer_to_buffer(p->devid, layer, p->image, 0, 0,
                                                  (size_t)p->width * p->height * p->ch * sizeof(float));

  return err;
}

static cl_int dwt_wavelet_decompose_cl(cl_mem img, dwt_params_cl_t *const p, _dwt_layer_func_cl layer_func)
{
  cl_int err = CL_SUCCESS;

  const int devid = p->devid;

  cl_mem temp = NULL;
  cl_mem layers = NULL;
  cl_mem merged_layers = NULL;
  unsigned int lpass, hpass;
  cl_mem buffer[2] = { 0, 0 };
  int bcontinue = 1;

  if(layer_func)
  {
    err = layer_func(img, p, 0);
    if(err != CL_SUCCESS) goto cleanup;
  }

  if(p->scales <= 0) goto cleanup;

  /* image buffers */
  buffer[0] = img;
  /* temporary storage */
  buffer[1] = dt_opencl_alloc_device_buffer(devid, (size_t)p->width * p->height * p->ch * sizeof(float));
  if(buffer[1] == NULL)
  {
    printf("not enough memory for wavelet decomposition");
    err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
    goto cleanup;
  }

  // buffer to reconstruct the image
  layers = dt_opencl_alloc_device_buffer(devid, (size_t)p->width * p->height * p->ch * sizeof(float));
  if(layers == NULL)
  {
    printf("not enough memory for wavelet decomposition");
    err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
    goto cleanup;
  }
  // init layer buffer
  {
    const int kernel = p->global->kernel_dwt_init_buffer;

    size_t sizes[] = { ROUNDUPWD(p->width), ROUNDUPHT(p->height), 1 };
    const int width = p->width;
    const int height = p->height;

    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), (void *)&layers);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(int), (void *)&(width));
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(int), (void *)&(height));
    err = dt_opencl_enqueue_kernel_2d(devid, kernel, sizes);
    if(err != CL_SUCCESS) goto cleanup;
  }

  if(p->merge_from_scale > 0)
  {
    merged_layers = dt_opencl_alloc_device_buffer(devid, (size_t)p->width * p->height * p->ch * sizeof(float));
    if(merged_layers == NULL)
    {
      printf("not enough memory for wavelet decomposition");
      err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
      goto cleanup;
    }
    // init reconstruct buffer
    {
      const int kernel = p->global->kernel_dwt_init_buffer;

      size_t sizes[] = { ROUNDUPWD(p->width), ROUNDUPHT(p->height), 1 };
      const int width = p->width;
      const int height = p->height;

      dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), (void *)&merged_layers);
      dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(int), (void *)&(width));
      dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(int), (void *)&(height));
      err = dt_opencl_enqueue_kernel_2d(devid, kernel, sizes);
      if(err != CL_SUCCESS) goto cleanup;
    }
  }

  // iterate over wavelet scales
  lpass = 1;
  hpass = 0;
  for(unsigned int lev = 0; lev < p->scales && bcontinue; lev++)
  {
    lpass = (1 - (lev & 1));

    // when (*layer_func) uses too much memory I get a -4 error, so alloc and free for each scale
    // setup a temp buffer
    temp = dt_opencl_alloc_device_buffer(devid, (size_t)p->width * p->height * p->ch * sizeof(float));
    if(temp == NULL)
    {
      printf("not enough memory for wavelet decomposition");
      err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
      goto cleanup;
    }

    // hat transform by row
    {
      const int kernel = p->global->kernel_dwt_hat_transform_row;

      int sc = 1 << lev;
      sc = (int)(sc * p->preview_scale);
      if(sc > p->width) sc = p->width;

      size_t sizes[] = { ROUNDUPWD(p->width), ROUNDUPHT(p->height), 1 };

      dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), (void *)&temp);
      dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), (void *)&(buffer[hpass]));
      dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(int), (void *)&(p->width));
      dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), (void *)&(p->height));
      dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), (void *)&sc);
      err = dt_opencl_enqueue_kernel_2d(devid, kernel, sizes);
      if(err != CL_SUCCESS) goto cleanup;
    }

    // hat transform by col
    {
      const int kernel = p->global->kernel_dwt_hat_transform_col;

      int sc = 1 << lev;
      sc = (int)(sc * p->preview_scale);
      if(sc > p->height) sc = p->height;
      const float lpass_mult = (1.f / 16.f);

      size_t sizes[] = { ROUNDUPWD(p->width), ROUNDUPHT(p->height), 1 };

      dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), (void *)&temp);
      dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(int), (void *)&(p->width));
      dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(int), (void *)&(p->height));
      dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), (void *)&sc);
      dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(cl_mem), (void *)&(buffer[lpass]));
      dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(float), (void *)&lpass_mult);
      err = dt_opencl_enqueue_kernel_2d(devid, kernel, sizes);
      if(err != CL_SUCCESS) goto cleanup;
    }

    if(temp)
    {
      dt_opencl_release_mem_object(temp);
      temp = NULL;
    }

    err = dwt_subtract_layer_cl(buffer[lpass], buffer[hpass], p);
    if(err != CL_SUCCESS) goto cleanup;

    // no merge scales or we didn't reach the merge scale from yet
    if(p->merge_from_scale == 0 || p->merge_from_scale > lev + 1)
    {
      // allow to process this detail scale
      if(layer_func)
      {
        err = layer_func(buffer[hpass], p, lev + 1);
        if(err != CL_SUCCESS) goto cleanup;
      }

      // user wants to preview this detail scale
      if(p->return_layer == lev + 1)
      {
        // return this detail scale
        err = dwt_get_image_layer_cl(buffer[hpass], p);
        if(err != CL_SUCCESS) goto cleanup;

        bcontinue = 0;
      }
      // user wants the entire reconstructed image
      else if(p->return_layer == 0)
      {
        // add this detail scale to the final image
        err = dwt_add_layer_cl(buffer[hpass], layers, p, lev + 1);
        if(err != CL_SUCCESS) goto cleanup;
      }
    }
    // we are on the merge scales range
    else
    {
      // add this detail scale to the merged ones
      err = dwt_add_layer_cl(buffer[hpass], merged_layers, p, lev + 1);
      if(err != CL_SUCCESS) goto cleanup;

      // allow to process this merged scale
      if(layer_func)
      {
        err = layer_func(merged_layers, p, lev + 1);
        if(err != CL_SUCCESS) goto cleanup;
      }

      // user wants to preview this merged scale
      if(p->return_layer == lev + 1)
      {
        // return this merged scale
        err = dwt_get_image_layer_cl(merged_layers, p);
        if(err != CL_SUCCESS) goto cleanup;

        bcontinue = 0;
      }
    }

    hpass = lpass;
  }

  // all scales have been processed
  if(bcontinue)
  {
    // allow to process residual image
    if(layer_func)
    {
      err = layer_func(buffer[hpass], p, p->scales + 1);
      if(err != CL_SUCCESS) goto cleanup;
    }

    // user wants to preview residual image
    if(p->return_layer == p->scales + 1)
    {
      // return residual image
      err = dwt_get_image_layer_cl(buffer[hpass], p);
      if(err != CL_SUCCESS) goto cleanup;
    }
    // return reconstructed image
    else if(p->return_layer == 0)
    {
      // some of the detail scales are on the merged layers
      if(p->merge_from_scale > 0)
      {
        // add merged layers to final image
        err = dwt_add_layer_cl(merged_layers, layers, p, p->scales + 1);
        if(err != CL_SUCCESS) goto cleanup;
      }

      // add residual image to final image
      err = dwt_add_layer_cl(buffer[hpass], layers, p, p->scales + 1);
      if(err != CL_SUCCESS) goto cleanup;

      // allow to process reconstructed image
      if(layer_func)
      {
        err = layer_func(layers, p, p->scales + 2);
        if(err != CL_SUCCESS) goto cleanup;
      }

      // return reconstructed image
      err = dwt_get_image_layer_cl(layers, p);
      if(err != CL_SUCCESS) goto cleanup;
    }
  }

cleanup:
  if(layers) dt_opencl_release_mem_object(layers);
  if(merged_layers) dt_opencl_release_mem_object(merged_layers);
  if(temp) dt_opencl_release_mem_object(temp);
  if(buffer[1]) dt_opencl_release_mem_object(buffer[1]);

  return err;
}

cl_int dwt_decompose_cl(dwt_params_cl_t *p, _dwt_layer_func_cl layer_func)
{
  cl_int err = CL_SUCCESS;

  // this is a zoom scale, not a wavelet scale
  if(p->preview_scale <= 0.f) p->preview_scale = 1.f;

  // if a single scale is requested it cannot be grather than the residual
  if(p->return_layer > p->scales + 1)
  {
    p->return_layer = p->scales + 1;
  }

  const int max_scale = dwt_get_max_scale_cl(p);

  // if requested scales is grather than max scales adjust it
  if(p->scales > max_scale)
  {
    // residual shoud be returned
    if(p->return_layer > p->scales) p->return_layer = max_scale + 1;
    // a scale should be returned, it cannot be grather than max scales
    else if(p->return_layer > max_scale)
      p->return_layer = max_scale;

    p->scales = max_scale;
  }

  // call the actual decompose
  err = dwt_wavelet_decompose_cl(p->image, p, layer_func);

  return err;
}

#endif
