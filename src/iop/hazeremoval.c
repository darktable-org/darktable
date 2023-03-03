/*
    This file is part of darktable,
    Copyright (C) 2017-2023 darktable developers.

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
    This module implements automatic single-image haze removal as
    described by K. He et al. in

    * Kaiming He, Jian Sun, and Xiaoou Tang, "Single Image Haze
      Removal Using Dark Channel Prior," IEEE Transactions on Pattern
      Analysis and Machine Intelligence, vol. 33, no. 12,
      pp. 2341-2353, Dec. 2011. DOI: 10.1109/TPAMI.2010.168

    * K. He, J. Sun, and X. Tang, "Guided Image Filtering," Lecture
      Notes in Computer Science, pp. 1-14, 2010. DOI:
      10.1007/978-3-642-15549-9_1
*/


#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "bauhaus/bauhaus.h"
#include "common/box_filters.h"
#include "common/darktable.h"
#include "common/guided_filter.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "develop/imageop_gui.h"
#include "gui/gtk.h"
#include "gui/accelerators.h"
#include "develop/tiling.h"
#include "iop/iop_api.h"

#ifdef HAVE_OPENCL
#include "common/opencl.h"
#endif

#include <float.h>
#include <gtk/gtk.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

//----------------------------------------------------------------------
// implement the module api
//----------------------------------------------------------------------

DT_MODULE_INTROSPECTION(1, dt_iop_hazeremoval_params_t)

typedef dt_aligned_pixel_t rgb_pixel;

typedef struct dt_iop_hazeremoval_params_t
{
  float strength; // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.2
  float distance; // $MIN:  0.0 $MAX: 1.0 $DEFAULT: 0.2
} dt_iop_hazeremoval_params_t;

// types  dt_iop_hazeremoval_params_t and dt_iop_hazeremoval_data_t are
// equal, thus no commit_params function needs to be implemented
typedef dt_iop_hazeremoval_params_t dt_iop_hazeremoval_data_t;

typedef struct dt_iop_hazeremoval_gui_data_t
{
  GtkWidget *strength;
  GtkWidget *distance;
  rgb_pixel A0;
  float distance_max;
  uint64_t hash;
} dt_iop_hazeremoval_gui_data_t;

typedef struct dt_iop_hazeremoval_global_data_t
{
  int kernel_hazeremoval_transision_map;
  int kernel_hazeremoval_box_min_x;
  int kernel_hazeremoval_box_min_y;
  int kernel_hazeremoval_box_max_x;
  int kernel_hazeremoval_box_max_y;
  int kernel_hazeremoval_dehaze;
} dt_iop_hazeremoval_global_data_t;


const char *name()
{
  return _("haze removal");
}


const char *aliases()
{
  return _("dehaze|defog|smoke|smog");
}

const char **description(struct dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("remove fog and atmospheric hazing from pictures"),
                                      _("corrective"),
                                      _("linear, RGB, scene-referred"),
                                      _("frequential, RGB"),
                                      _("linear, RGB, scene-referred"));
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING;
}


int default_group()
{
  return IOP_GROUP_CORRECT | IOP_GROUP_TECHNICAL;
}


int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}


void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1, sizeof(dt_iop_hazeremoval_data_t));
}


void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}


void init_global(dt_iop_module_so_t *self)
{
  dt_iop_hazeremoval_global_data_t *gd = malloc(sizeof(*gd));
  const int program = 27; // hazeremoval.cl, from programs.conf
  gd->kernel_hazeremoval_transision_map = dt_opencl_create_kernel(program, "hazeremoval_transision_map");
  gd->kernel_hazeremoval_box_min_x = dt_opencl_create_kernel(program, "hazeremoval_box_min_x");
  gd->kernel_hazeremoval_box_min_y = dt_opencl_create_kernel(program, "hazeremoval_box_min_y");
  gd->kernel_hazeremoval_box_max_x = dt_opencl_create_kernel(program, "hazeremoval_box_max_x");
  gd->kernel_hazeremoval_box_max_y = dt_opencl_create_kernel(program, "hazeremoval_box_max_y");
  gd->kernel_hazeremoval_dehaze = dt_opencl_create_kernel(program, "hazeremoval_dehaze");
  self->data = gd;
}


void cleanup_global(dt_iop_module_so_t *self)
{
  dt_iop_hazeremoval_global_data_t *gd = self->data;
  dt_opencl_free_kernel(gd->kernel_hazeremoval_transision_map);
  dt_opencl_free_kernel(gd->kernel_hazeremoval_box_min_x);
  dt_opencl_free_kernel(gd->kernel_hazeremoval_box_min_y);
  dt_opencl_free_kernel(gd->kernel_hazeremoval_box_max_x);
  dt_opencl_free_kernel(gd->kernel_hazeremoval_box_max_y);
  dt_opencl_free_kernel(gd->kernel_hazeremoval_dehaze);
  free(self->data);
  self->data = NULL;
}


void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_hazeremoval_gui_data_t *g = (dt_iop_hazeremoval_gui_data_t *)self->gui_data;

  dt_iop_gui_enter_critical_section(self);
  g->distance_max = NAN;
  g->A0[0] = NAN;
  g->A0[1] = NAN;
  g->A0[2] = NAN;
  g->hash = 0;
  dt_iop_gui_leave_critical_section(self);
}


void gui_init(dt_iop_module_t *self)
{
  dt_iop_hazeremoval_gui_data_t *g = IOP_GUI_ALLOC(hazeremoval);

  g->distance_max = NAN;
  g->A0[0] = NAN;
  g->A0[1] = NAN;
  g->A0[2] = NAN;
  g->hash = 0;

  g->strength = dt_bauhaus_slider_from_params(self, N_("strength"));
  gtk_widget_set_tooltip_text(g->strength, _("amount of haze reduction"));

  g->distance = dt_bauhaus_slider_from_params(self, N_("distance"));
  dt_bauhaus_slider_set_digits(g->distance, 3);
  gtk_widget_set_tooltip_text(g->distance, _("limit haze removal up to a specific spatial depth"));
}


void gui_cleanup(dt_iop_module_t *self)
{
  IOP_GUI_FREE;
}

//----------------------------------------------------------------------
// module local functions and structures required by process function
//----------------------------------------------------------------------

typedef struct tile
{
  int left, right, lower, upper;
} tile;


typedef struct rgb_image
{
  float *data;
  int width, height, stride;
} rgb_image;


typedef struct const_rgb_image
{
  const float *data;
  int width, height, stride;
} const_rgb_image;




// swap the two floats that the pointers point to
static inline void pointer_swap_f(float *a, float *b)
{
  float t = *a;
  *a = *b;
  *b = t;
}


// calculate the dark channel (minimal color component over a box of size (2*w+1) x (2*w+1) )
static void dark_channel(const const_rgb_image img1, const gray_image img2, const int w)
{
  const size_t size = (size_t)img1.height * img1.width;
  const float *const restrict in_data = img1.data;
  float *const restrict out_data = img2.data;
#ifdef _OPENMP
#pragma omp parallel for simd aligned(in_data, out_data: 64) default(none) \
  dt_omp_firstprivate(in_data, out_data, size) \
  schedule(simd:static)
#endif
  for(size_t i = 0; i < size; i++)
  {
    const float *pixel = in_data + 4*i;
    float m = MIN(pixel[0], pixel[1]);
    m = MIN(pixel[2], m);
    out_data[i] = m;
  }
  dt_box_min(img2.data, img2.height, img2.width, 1, w);
}


// calculate the transition map
static void transition_map(const const_rgb_image img1, const gray_image img2, const int w, const float *const A0,
                           const float strength)
{
  const size_t size = (size_t)img1.height * img1.width;
  const float *const restrict in_data = img1.data;
  float *const restrict out_data = img2.data;
  const dt_aligned_pixel_t A0_inv = { 1.0f / A0[0], 1.0f / A0[1], 1.0f / A0[2], 1.0f };
#ifdef _OPENMP
#pragma omp parallel for simd aligned(in_data, out_data: 64) default(none) \
  dt_omp_firstprivate(A0_inv, in_data, out_data, size, strength) \
  schedule(simd:static)
#endif
  for(size_t i = 0; i < size; i++)
  {
    const float *pixel = in_data + 4*i;
    float m = MIN(pixel[0] * A0_inv[0], pixel[1] * A0_inv[1]);
    m = MIN(pixel[2] * A0_inv[2], m);
    out_data[i] = 1.f - m * strength;
  }
  dt_box_max(img2.data, img2.height, img2.width, 1, w);
}


// partition the array [first, last) using the pivot value val, i.e.,
// reorder the elements in the range [first, last) in such a way that
// all elements that are less than the pivot precede the elements
// which are larger or equal the pivot
static float *partition(float *first, float *last, float val)
{
  for(; first != last; ++first)
  {
    if(!((*first) < val)) break;
  }
  if(first == last) return first;
  for(float *i = first + 1; i != last; ++i)
  {
    if((*i) < val)
    {
      pointer_swap_f(i, first);
      ++first;
    }
  }
  return first;
}


// quick select algorithm, arranges the range [first, last) such that
// the element pointed to by nth is the same as the element that would
// be in that position if the entire range [first, last) had been
// sorted, additionally, none of the elements in the range [nth, last)
// is less than any of the elements in the range [first, nth)
void quick_select(float *first, float *nth, float *last)
{
  if(first == last) return;
  for(;;)
  {
    // select pivot by median of three heuristic for better performance
    float *p1 = first;
    float *pivot = first + (last - first) / 2;
    float *p3 = last - 1;
    if(!(*p1 < *pivot)) pointer_swap_f(p1, pivot);
    if(!(*p1 < *p3)) pointer_swap_f(p1, p3);
    if(!(*pivot < *p3)) pointer_swap_f(pivot, p3);
    pointer_swap_f(pivot, last - 1); // move pivot to end
    partition(first, last - 1, *(last - 1));
    pointer_swap_f(last - 1, pivot); // move pivot to its final place
    if(nth == pivot)
      break;
    else if(nth < pivot)
      last = pivot;
    else
      first = pivot + 1;
  }
}


// calculate diffusive ambient light and the maximal depth in the image
// depth is estimated by the local amount of haze and given in units of the
// characteristic haze depth, i.e., the distance over which object light is
// reduced by the factor exp(-1)
static float ambient_light(const const_rgb_image img, int w1, rgb_pixel *pA0)
{
  const float dark_channel_quantil = 0.95f; // quantil for determining the most hazy pixels
  const float bright_quantil = 0.95f; // quantil for determining the brightest pixels among the most hazy pixels
  const int width = img.width;
  const int height = img.height;
  const size_t size = (size_t)width * height;
  // calculate dark channel, which is an estimate for local amount of haze
  gray_image dark_ch = new_gray_image(width, height);
  dark_channel(img, dark_ch, w1);
  // determine the brightest pixels among the most hazy pixels
  gray_image bright_hazy = new_gray_image(width, height);
  // first determine the most hazy pixels
  copy_gray_image(dark_ch, bright_hazy);
  size_t p = (size_t)(size * dark_channel_quantil);
  quick_select(bright_hazy.data, bright_hazy.data + p, bright_hazy.data + size);
  const float crit_haze_level = bright_hazy.data[p];
  size_t N_most_hazy = 0;
  for(size_t i = 0; i < size; i++)
    if(dark_ch.data[i] >= crit_haze_level)
    {
      const float *pixel_in = img.data + i * img.stride;
      // next line prevents parallelization via OpenMP
      bright_hazy.data[N_most_hazy] = pixel_in[0] + pixel_in[1] + pixel_in[2];
      N_most_hazy++;
    }
  p = (size_t)(N_most_hazy * bright_quantil);
  quick_select(bright_hazy.data, bright_hazy.data + p, bright_hazy.data + N_most_hazy);
  const float crit_brightness = bright_hazy.data[p];
  free_gray_image(&bright_hazy);
  // average over the brightest pixels among the most hazy pixels to
  // estimate the diffusive ambient light
  float A0_r = 0, A0_g = 0, A0_b = 0;
  size_t N_bright_hazy = 0;
  const float *const restrict data = dark_ch.data;
  const float *const restrict in_data = img.data;
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(crit_brightness, crit_haze_level, data, in_data, size) \
  schedule(static) \
  reduction(+ : N_bright_hazy, A0_r, A0_g, A0_b)
#endif
  for(size_t i = 0; i < size; i++)
  {
    const float *pixel_in = in_data + 4*i;
    if((data[i] >= crit_haze_level) && (pixel_in[0] + pixel_in[1] + pixel_in[2] >= crit_brightness))
    {
      A0_r += pixel_in[0];
      A0_g += pixel_in[1];
      A0_b += pixel_in[2];
      N_bright_hazy++;
    }
  }
  if(N_bright_hazy > 0)
  {
    A0_r /= N_bright_hazy;
    A0_g /= N_bright_hazy;
    A0_b /= N_bright_hazy;
  }
  (*pA0)[0] = A0_r;
  (*pA0)[1] = A0_g;
  (*pA0)[2] = A0_b;
  free_gray_image(&dark_ch);
  // for almost haze free images it may happen that crit_haze_level=0, this means
  // there is a very large image depth, in this case a large number is returned, that
  // is small enough to avoid overflow in later processing
  // the critical haze level is at dark_channel_quantil (not 100%) to be insensitive
  // to extreme outliners, compensate for that by some factor slightly larger than
  // unity when calculating the maximal image depth
  return crit_haze_level > 0 ? -1.125f * logf(crit_haze_level) : logf(FLT_MAX) / 2; // return the maximal depth
}


void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  if(!dt_iop_have_required_input_format(4 /*we need full-color pixels*/, self, piece->colors,
                                         ivoid, ovoid, roi_in, roi_out))
    return;
  dt_iop_hazeremoval_gui_data_t *const g = (dt_iop_hazeremoval_gui_data_t*)self->gui_data;
  dt_iop_hazeremoval_params_t *d = piece->data;

  const int width = roi_in->width;
  const int height = roi_in->height;
  const size_t size = (size_t)width * height;
  const int w1 = 6; // window size (positive integer) for determining the dark channel and the transition map
  const int w2 = 9; // window size (positive integer) for the guided filter

  // module parameters
  const float strength = d->strength; // strength of haze removal
  const float distance = d->distance; // maximal distance from camera to remove haze
  const float eps = sqrtf(0.025f);    // regularization parameter for guided filter

  const float *const restrict in = (float*)ivoid;
  float *const restrict out = (float*)ovoid;
  const const_rgb_image img_in = (const_rgb_image){ in, width, height, 4 };

  // estimate diffusive ambient light and image depth
  rgb_pixel A0 = { NAN, NAN, NAN, 0.0f };
  float distance_max = NAN;

  // hazeremoval module needs the color and the haziness (which yields
  // distance_max) of the most hazy region of the image.  In pixelpipe
  // FULL we can not reliably get this value as the pixelpipe might
  // only see part of the image (region of interest).  Therefore, we
  // try to get A0 and distance_max from the PREVIEW pixelpipe which
  // luckily stores it for us.
  if(self->dev->gui_attached && g && (piece->pipe->type & DT_DEV_PIXELPIPE_FULL))
  {
    dt_iop_gui_enter_critical_section(self);
    const uint64_t hash = g->hash;
    dt_iop_gui_leave_critical_section(self);
    // Note that the case 'hash == 0' on first invocation in a session
    // implies that g->distance_max is NAN, which initiates special
    // handling below to avoid inconsistent results.  In all other
    // cases we make sure that the preview pipe has left us with
    // proper readings for distance_max and A0.  If data are not yet
    // there we need to wait (with timeout).
    if(hash != 0
       && !dt_dev_sync_pixelpipe_hash(self->dev, piece->pipe, self->iop_order, DT_DEV_TRANSFORM_DIR_BACK_INCL,
                                      &self->gui_lock, &g->hash))
      dt_control_log(_("inconsistent output"));
    dt_iop_gui_enter_critical_section(self);
    A0[0] = g->A0[0];
    A0[1] = g->A0[1];
    A0[2] = g->A0[2];
    distance_max = g->distance_max;
    dt_iop_gui_leave_critical_section(self);
  }
  // In all other cases we calculate distance_max and A0 here.
  if(isnan(distance_max)) distance_max = ambient_light(img_in, w1, &A0);
  // PREVIEW pixelpipe stores values.
  if(self->dev->gui_attached && g && (piece->pipe->type & DT_DEV_PIXELPIPE_PREVIEW))
  {
    uint64_t hash = dt_dev_hash_plus(self->dev, piece->pipe, self->iop_order, DT_DEV_TRANSFORM_DIR_BACK_INCL);
    dt_iop_gui_enter_critical_section(self);
    g->A0[0] = A0[0];
    g->A0[1] = A0[1];
    g->A0[2] = A0[2];
    g->distance_max = distance_max;
    g->hash = hash;
    dt_iop_gui_leave_critical_section(self);
  }

  // calculate the transition map
  gray_image trans_map = new_gray_image(width, height);
  transition_map(img_in, trans_map, w1, A0, strength);

  // refine the transition map
  dt_box_min(trans_map.data, trans_map.height, trans_map.width, 1, w1);
  gray_image trans_map_filtered = new_gray_image(width, height);
  // apply guided filter with no clipping
  guided_filter(img_in.data, trans_map.data, trans_map_filtered.data, width, height, 4, w2, eps, 1.f, -FLT_MAX,
                FLT_MAX);

  // finally, calculate the haze-free image
  const float t_min
      = fminf(fmaxf(expf(-distance * distance_max), 1.f / 1024), 1.f); // minimum allowed value for transition map
  const dt_aligned_pixel_t c_A0 = { A0[0], A0[1], A0[2], A0[3] };
  const gray_image c_trans_map_filtered = trans_map_filtered;
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(c_A0, c_trans_map_filtered, in, out, size, t_min) \
  schedule(static)
#endif
  for(size_t i = 0; i < size; i++)
  {
    float t = MAX(c_trans_map_filtered.data[i], t_min);
    dt_aligned_pixel_t res;
    for_each_channel(c, aligned(in))
      res[c] =  (in[4*i + c] - c_A0[c]) / t + c_A0[c];
    copy_pixel_nontemporal(out + 4*i, res);
  }
  dt_omploop_sfence();

  free_gray_image(&trans_map);
  free_gray_image(&trans_map_filtered);
}

#ifdef HAVE_OPENCL

// calculate diffusive ambient light and the maximal depth in the image
// depth is estimated by the local amount of haze and given in units of the
// characteristic haze depth, i.e., the distance over which object light is
// reduced by the factor exp(-1)
// some parts of the calculation are not suitable for a parallel implementation,
// thus we copy data to host memory fall back to a cpu routine
static float ambient_light_cl(struct dt_iop_module_t *self, int devid, cl_mem img, int w1, rgb_pixel *pA0)
{
  const int width = dt_opencl_get_image_width(img);
  const int height = dt_opencl_get_image_height(img);
  const int element_size = dt_opencl_get_image_element_size(img);
  float *in = dt_alloc_align(64, (size_t)width * height * element_size);
  int err = dt_opencl_read_host_from_device(devid, in, img, width, height, element_size);
  if(err != CL_SUCCESS) goto error;
  const const_rgb_image img_in = (const_rgb_image){ in, width, height, element_size / sizeof(float) };
  const float max_depth = ambient_light(img_in, w1, pA0);
  dt_free_align(in);
  return max_depth;
error:
  dt_print(DT_DEBUG_OPENCL, "[hazeremoval, ambient_light_cl] unknown error: %d\n", err);
  dt_free_align(in);
  return 0.f;
}


static int box_min_cl(struct dt_iop_module_t *self, int devid, cl_mem in, cl_mem out, const int w)
{
  dt_iop_hazeremoval_global_data_t *gd = self->global_data;
  const int width = dt_opencl_get_image_width(in);
  const int height = dt_opencl_get_image_height(in);
  void *temp = dt_opencl_alloc_device(devid, width, height, (int)sizeof(float));

  const int kernel_x = gd->kernel_hazeremoval_box_min_x;
  dt_opencl_set_kernel_args(devid, kernel_x, 0, CLARG(width), CLARG(height), CLARG(in), CLARG(temp),
    CLARG(w));
  const size_t sizes_x[] = { 1, ROUNDUPDHT(height, devid) };
  int err = dt_opencl_enqueue_kernel_2d(devid, kernel_x, sizes_x);
  if(err != CL_SUCCESS) goto error;

  const int kernel_y = gd->kernel_hazeremoval_box_min_y;
  dt_opencl_set_kernel_args(devid, kernel_y, 0, CLARG(width), CLARG(height), CLARG(temp), CLARG(out),
    CLARG(w));
  const size_t sizes_y[] = { ROUNDUPDWD(width, devid), 1 };
  err = dt_opencl_enqueue_kernel_2d(devid, kernel_y, sizes_y);

error:
  if(err != CL_SUCCESS) dt_print(DT_DEBUG_OPENCL, "[hazeremoval, box_min_cl] unknown error: %d\n", err);
  dt_opencl_release_mem_object(temp);
  return err;
}


static int box_max_cl(struct dt_iop_module_t *self, int devid, cl_mem in, cl_mem out, const int w)
{
  dt_iop_hazeremoval_global_data_t *gd = self->global_data;
  const int width = dt_opencl_get_image_width(in);
  const int height = dt_opencl_get_image_height(in);
  void *temp = dt_opencl_alloc_device(devid, width, height, (int)sizeof(float));

  const int kernel_x = gd->kernel_hazeremoval_box_max_x;
  dt_opencl_set_kernel_args(devid, kernel_x, 0, CLARG(width), CLARG(height), CLARG(in), CLARG(temp),
    CLARG(w));
  const size_t sizes_x[] = { 1, ROUNDUPDHT(height, devid) };
  int err = dt_opencl_enqueue_kernel_2d(devid, kernel_x, sizes_x);
  if(err != CL_SUCCESS) goto error;

  const int kernel_y = gd->kernel_hazeremoval_box_max_y;
  dt_opencl_set_kernel_args(devid, kernel_y, 0, CLARG(width), CLARG(height), CLARG(temp), CLARG(out),
    CLARG(w));
  const size_t sizes_y[] = { ROUNDUPDWD(width, devid), 1 };
  err = dt_opencl_enqueue_kernel_2d(devid, kernel_y, sizes_y);

error:
  if(err != CL_SUCCESS) dt_print(DT_DEBUG_OPENCL, "[hazeremoval, box_max_cl] unknown error: %d\n", err);
  dt_opencl_release_mem_object(temp);
  return err;
}


static int transition_map_cl(struct dt_iop_module_t *self, int devid, cl_mem img1, cl_mem img2, const int w1,
                             const float strength, const float *const A0)
{
  dt_iop_hazeremoval_global_data_t *gd = self->global_data;
  const int width = dt_opencl_get_image_width(img1);
  const int height = dt_opencl_get_image_height(img1);

  const int kernel = gd->kernel_hazeremoval_transision_map;
  int err = dt_opencl_enqueue_kernel_2d_args(devid, kernel, width, height,
    CLARG(width), CLARG(height), CLARG(img1), CLARG(img2), CLARG(strength), CLARG(A0[0]), CLARG(A0[1]),
    CLARG(A0[2]));
  if(err != CL_SUCCESS)
  {
    dt_print(DT_DEBUG_OPENCL, "[hazeremoval, transition_map_cl] unknown error: %d\n", err);
    return err;
  }
  err = box_max_cl(self, devid, img2, img2, w1);

  return err;
}


static int dehaze_cl(struct dt_iop_module_t *self, int devid, cl_mem img_in, cl_mem trans_map, cl_mem img_out,
                     const float t_min, const float *const A0)
{
  dt_iop_hazeremoval_global_data_t *gd = self->global_data;
  const int width = dt_opencl_get_image_width(img_in);
  const int height = dt_opencl_get_image_height(img_in);

  const int kernel = gd->kernel_hazeremoval_dehaze;
  int err = dt_opencl_enqueue_kernel_2d_args(devid, kernel, width, height,
    CLARG(width), CLARG(height), CLARG(img_in), CLARG(trans_map), CLARG(img_out), CLARG(t_min), CLARG(A0[0]),
    CLARG(A0[1]), CLARG(A0[2]));
  if(err != CL_SUCCESS) dt_print(DT_DEBUG_OPENCL, "[hazeremoval, dehaze_cl] unknown error: %d\n", err);
  return err;
}

void tiling_callback(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                     const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out,
                     struct dt_develop_tiling_t *tiling)
{
  tiling->factor = 2.5f;  // in + out + two single-channel temp buffers
  tiling->factor_cl = 5.0f;
  tiling->maxbuf = 1.0f;
  tiling->maxbuf_cl = 1.0f;
  tiling->overhead = 0;
  tiling->overlap = 0;
  tiling->xalign = 1;
  tiling->yalign = 1;
}

int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem img_in, cl_mem img_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_hazeremoval_gui_data_t *const g = (dt_iop_hazeremoval_gui_data_t*)self->gui_data;
  dt_iop_hazeremoval_params_t *d = piece->data;

  const int ch = piece->colors;
  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;
  const int w1 = 6; // window size (positive integer) for determining the dark channel and the transition map
  const int w2 = 9; // window size (positive integer) for the guided filter

  // module parameters
  const float strength = d->strength; // strength of haze removal
  const float distance = d->distance; // maximal distance from camera to remove haze
  const float eps = sqrtf(0.025f);    // regularization parameter for guided filter

  // estimate diffusive ambient light and image depth
  rgb_pixel A0 = { NAN, NAN, NAN, 0.0f };
  float distance_max = NAN;

  // hazeremoval module needs the color and the haziness (which yields
  // distance_max) of the most hazy region of the image.  In pixelpipe
  // FULL we can not reliably get this value as the pixelpipe might
  // only see part of the image (region of interest).  Therefore, we
  // try to get A0 and distance_max from the PREVIEW pixelpipe which
  // luckily stores it for us.
  if(self->dev->gui_attached && g && (piece->pipe->type & DT_DEV_PIXELPIPE_FULL))
  {
    dt_iop_gui_enter_critical_section(self);
    const uint64_t hash = g->hash;
    dt_iop_gui_leave_critical_section(self);
    // Note that the case 'hash == 0' on first invocation in a session
    // implies that g->distance_max is NAN, which initiates special
    // handling below to avoid inconsistent results.  In all other
    // cases we make sure that the preview pipe has left us with
    // proper readings for distance_max and A0.  If data are not yet
    // there we need to wait (with timeout).
    if(hash != 0
       && !dt_dev_sync_pixelpipe_hash(self->dev, piece->pipe, self->iop_order, DT_DEV_TRANSFORM_DIR_BACK_INCL,
                                      &self->gui_lock, &g->hash))
      dt_control_log(_("inconsistent output"));
    dt_iop_gui_enter_critical_section(self);
    A0[0] = g->A0[0];
    A0[1] = g->A0[1];
    A0[2] = g->A0[2];
    distance_max = g->distance_max;
    dt_iop_gui_leave_critical_section(self);
  }
  // In all other cases we calculate distance_max and A0 here.
  if(isnan(distance_max)) distance_max = ambient_light_cl(self, devid, img_in, w1, &A0);
  // PREVIEW pixelpipe stores values.
  if(self->dev->gui_attached && g && (piece->pipe->type & DT_DEV_PIXELPIPE_PREVIEW))
  {
    uint64_t hash = dt_dev_hash_plus(self->dev, piece->pipe, self->iop_order, DT_DEV_TRANSFORM_DIR_BACK_INCL);
    dt_iop_gui_enter_critical_section(self);
    g->A0[0] = A0[0];
    g->A0[1] = A0[1];
    g->A0[2] = A0[2];
    g->distance_max = distance_max;
    g->hash = hash;
    dt_iop_gui_leave_critical_section(self);
  }

  // calculate the transition map
  void *trans_map = dt_opencl_alloc_device(devid, width, height, (int)sizeof(float));
  transition_map_cl(self, devid, img_in, trans_map, w1, strength, A0);
  // refine the transition map
  box_min_cl(self, devid, trans_map, trans_map, w1);
  void *trans_map_filtered = dt_opencl_alloc_device(devid, width, height, (int)sizeof(float));
  // apply guided filter with no clipping
  guided_filter_cl(devid, img_in, trans_map, trans_map_filtered, width, height, ch, w2, eps, 1.f, -CL_FLT_MAX,
                   CL_FLT_MAX);

  // finally, calculate the haze-free image
  const float t_min
      = fminf(fmaxf(expf(-distance * distance_max), 1.f / 1024), 1.f); // minimum allowed value for transition map
  dehaze_cl(self, devid, img_in, trans_map_filtered, img_out, t_min, A0);

  dt_opencl_release_mem_object(trans_map);
  dt_opencl_release_mem_object(trans_map_filtered);

  return TRUE;
}
#endif

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

