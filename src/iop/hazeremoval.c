/*
    This file is part of darktable,
    Copyright (C) 2017-2025 darktable developers.

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

DT_MODULE_INTROSPECTION(2, dt_iop_hazeremoval_params_t)

typedef dt_aligned_pixel_t rgb_pixel;

typedef struct dt_iop_hazeremoval_params_t
{
  float strength; // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.2
  float distance; // $MIN:  0.0 $MAX: 1.0 $DEFAULT: 0.2
  gboolean compatibility_mode; // $DEFAULT: FALSE
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
  dt_hash_t hash;
  gboolean redo;
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

const char **description(dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("remove fog and atmospheric hazing from images"),
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


dt_iop_colorspace_type_t default_colorspace(dt_iop_module_t *self,
                                            dt_dev_pixelpipe_t *pipe,
                                            dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}


void init_pipe(dt_iop_module_t *self,
               dt_dev_pixelpipe_t *pipe,
               dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1, sizeof(dt_iop_hazeremoval_data_t));
}


void cleanup_pipe(dt_iop_module_t *self,
                  dt_dev_pixelpipe_t *pipe,
                  dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

int legacy_params(dt_iop_module_t *self,
                  const void *const old_params,
                  const int old_version,
                  void **new_params,
                  int32_t *new_params_size,
                  int *new_version)
{
  if(old_version == 1)
  {
    typedef struct dt_iop_hazeremoval_params_v1_t
    {
      float strength;
      float distance;
    } dt_iop_hazeremoval_params_v1_t;
    const dt_iop_hazeremoval_params_v1_t *o = old_params;

    dt_iop_hazeremoval_params_t *n = malloc(sizeof(dt_iop_hazeremoval_params_t));
    memcpy(n, o, sizeof(dt_iop_hazeremoval_params_v1_t));

    n->compatibility_mode = TRUE;
    *new_params = n;
    *new_params_size = sizeof(dt_iop_hazeremoval_params_t);
    *new_version = 2;
    return 0;
  }

  return 1;
}

void init_global(dt_iop_module_so_t *self)
{
  dt_iop_hazeremoval_global_data_t *gd = malloc(sizeof(*gd));
  const int program = 27; // hazeremoval.cl, from programs.conf
  gd->kernel_hazeremoval_transision_map =
    dt_opencl_create_kernel(program, "hazeremoval_transision_map");
  gd->kernel_hazeremoval_box_min_x =
    dt_opencl_create_kernel(program, "hazeremoval_box_min_x");
  gd->kernel_hazeremoval_box_min_y =
    dt_opencl_create_kernel(program, "hazeremoval_box_min_y");
  gd->kernel_hazeremoval_box_max_x =
    dt_opencl_create_kernel(program, "hazeremoval_box_max_x");
  gd->kernel_hazeremoval_box_max_y =
    dt_opencl_create_kernel(program, "hazeremoval_box_max_y");
  gd->kernel_hazeremoval_dehaze =
    dt_opencl_create_kernel(program, "hazeremoval_dehaze");
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

void gui_changed(dt_iop_module_t *self,
                 GtkWidget *w,
                 void *previous)
{
  dt_iop_hazeremoval_params_t *p = self->params;
  if(w)
    p->compatibility_mode = FALSE;
}

void gui_init(dt_iop_module_t *self)
{
  dt_iop_hazeremoval_gui_data_t *g = IOP_GUI_ALLOC(hazeremoval);

  g->hash = DT_INVALID_CACHEHASH;
  g->redo = FALSE;
  g->strength = dt_bauhaus_slider_from_params(self, N_("strength"));
  gtk_widget_set_tooltip_text(g->strength, _("amount of haze reduction"));

  g->distance = dt_bauhaus_slider_from_params(self, N_("distance"));
  dt_bauhaus_slider_set_digits(g->distance, 3);
  gtk_widget_set_tooltip_text(g->distance,
                              _("limit haze removal up to a specific spatial depth"));
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
static inline void _pointer_swap_f(float *a, float *b)
{
  float t = *a;
  *a = *b;
  *b = t;
}

// calculate the dark channel (minimal color component over a box of
// size (2*w+1) x (2*w+1) )
static void _dark_channel(const const_rgb_image img1,
                          const gray_image img2,
                          const int w)
{
  const size_t size = (size_t)img1.height * img1.width;
  const float *const restrict in_data = img1.data;
  float *const restrict out_data = img2.data;
  DT_OMP_FOR_SIMD(aligned(in_data, out_data: 64))
  for(size_t i = 0; i < size; i++)
  {
    const float *pixel = in_data + 4*i;
    float m = MIN(MIN(pixel[0], pixel[1]),pixel[2]);
    out_data[i] = m;
  }
  dt_box_min(img2.data, img2.height, img2.width, 1, w);
}


// calculate the transition map
static void _transition_map(const const_rgb_image img1,
                            const gray_image img2,
                            const int w,
                            const float *const A0,
                            const float strength)
{
  const size_t size = (size_t)img1.height * img1.width;
  const float *const restrict in_data = img1.data;
  float *const restrict out_data = img2.data;
  const dt_aligned_pixel_t A0_inv = { 1.0f / A0[0], 1.0f / A0[1], 1.0f / A0[2], 1.0f };
  DT_OMP_FOR_SIMD(aligned(in_data, out_data: 64))

  for(size_t i = 0; i < size; i++)
  {
    const float *pixel = in_data + 4*i;
    const float m = MIN(MIN(pixel[0] * A0_inv[0], pixel[1] * A0_inv[1]),
                        pixel[2] * A0_inv[2]);
    out_data[i] = 1.f - m * strength;
  }
  dt_box_max(img2.data, img2.height, img2.width, 1, w);
}


// partition the array [first, last) using the pivot value val, i.e.,
// reorder the elements in the range [first, last) in such a way that
// all elements that are less than the pivot precede the elements
// which are larger or equal the pivot
static float *_partition(float *first,
                         float *last,
                         const float val,
                         const gboolean compatibility_mode)
{
  if(compatibility_mode)
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
        _pointer_swap_f(i, first);
        ++first;
      }
    }
    return first;
  }
  last++;
  while(TRUE)
  {
    // scan from start until we find an element which needs to be swapped to the second half
    do
    {
      first++;
    } while(first < last && *first < val);
    // scan from end until we find an element which needs to be swapped to the first half
    do
    {
      last--;
    } while(first < last && *last > val);
    // if we didn't find anything to swap, return that position as the partitioning location
    if(first >= last)
      return first;
    // swap the out-of-place elements
    _pointer_swap_f(first, last);
  }
}

// quick select algorithm, arranges the range [first, last) such that
// the element pointed to by nth is the same as the element that would
// be in that position if the entire range [first, last) had been
// sorted, additionally, none of the elements in the range [nth, last)
// is less than any of the elements in the range [first, nth)
void _quick_select(float *first,
                   float *nth,
                   float *last,
                   const gboolean compatibility_mode)
{
  if(first == last) return;
  for(;last > first+1;)
  {
    // select pivot by median of three heuristic for better performance
    float *p1 = first;
    float *p3 = compatibility_mode ? first + (last - first) / 2 : nth;
    float *pivot = last - 1; // put median in last to avoid additional swap

    if(!(*p1 < *pivot))
      _pointer_swap_f(p1, pivot);
    if(!(*p1 < *p3))
      _pointer_swap_f(p1, p3);
    if(!(*pivot < *p3))
      _pointer_swap_f(pivot, p3);

    float *new_pivot = _partition(first, last - 1, last[-1], compatibility_mode);
    if(compatibility_mode)
      pivot = p3; // old code simply assumed pivot would end up in middle
    else
      pivot = new_pivot;

    _pointer_swap_f(last - 1, pivot); // move pivot to its final place
    if(nth == pivot)
      break;
    else if(nth < pivot)
      last = pivot;
    else
      first = pivot + 1;
  }
}

// calculate diffusive ambient light and the maximal depth in the image.
// depth is estimated by the local amount of haze and given in units of the
// characteristic haze depth, i.e., the distance over which object light is
// reduced by the factor exp(-1)
static float _ambient_light(const const_rgb_image img,
                            const int w1,
                            rgb_pixel *pA0,
                            const gboolean compatibility_mode)
{
  const float dark_channel_quantil = 0.95f; // quantil for determining the most hazy pixels
  const float bright_quantil = 0.95f; // quantil for determining the
                                      // brightest pixels among the
                                      // most hazy pixels
  const int width = img.width;
  const int height = img.height;
  const size_t size = (size_t)width * height;
  // calculate dark channel, which is an estimate for local amount of haze
  gray_image dark_ch = new_gray_image(width, height);
  _dark_channel(img, dark_ch, w1);
  // determine the brightest pixels among the most hazy pixels
  gray_image bright_hazy = new_gray_image(width, height);
  // first determine the most hazy pixels
  copy_gray_image(dark_ch, bright_hazy);
  float *const restrict hazy_data = bright_hazy.data;
  size_t p = (size_t)(size * dark_channel_quantil);
  _quick_select(hazy_data, hazy_data + p, hazy_data + size, compatibility_mode);
  const float crit_haze_level = hazy_data[p];
  const float *const restrict img_data = img.data;
  const float *const restrict dark_data = dark_ch.data;
  size_t N_most_hazy_start = size/2;
  size_t N_most_hazy_end = size/2;
  DT_OMP_PRAGMA(parallel num_threads(2) default(firstprivate) \
                shared(N_most_hazy_start, N_most_hazy_end))
  DT_OMP_PRAGMA(sections)
  {
  for(size_t i = 0; i < size/2; i++)
    if(dark_data[i] >= crit_haze_level)
    {
      const float *pixel_in = img_data + 4*i;
      // The next line prevents full parallelization via OpenMP.  But we can use
      // two threads by growing outward from the center
      hazy_data[--N_most_hazy_start] = pixel_in[0] + pixel_in[1] + pixel_in[2];
    }
  DT_OMP_PRAGMA(section)
  for(size_t i = size/2; i < size; i++)
    if(dark_data[i] >= crit_haze_level)
    {
      const float *pixel_in = img_data + 4*i;
      // next line prevents full parallelization via OpenMP
      hazy_data[N_most_hazy_end++] = pixel_in[0] + pixel_in[1] + pixel_in[2];
    }
  }
  if(compatibility_mode)
  {
    // for backwards compatibility with the original broken
    // quick_select, we need to put all of the items in hazy_data in
    // the order in which they appear in the original image.  Our
    // first loop above put them in reverse order, so un-reverse.
    const size_t start = N_most_hazy_start;
    const size_t end = size/2;
    const size_t midpoint = start + (end-start)/2;
    for(size_t i = start; i < midpoint; i++)
    {
      const float tmp = hazy_data[i];
      hazy_data[i] = hazy_data[(end-1) - (i-start)];
      hazy_data[(end-1) - (i-start)] = tmp;
    }
  }
  size_t N_most_hazy = N_most_hazy_end - N_most_hazy_start;
  p = (size_t)(N_most_hazy * bright_quantil) + N_most_hazy_start;
  _quick_select(hazy_data + N_most_hazy_start,
                hazy_data + p,
                hazy_data + N_most_hazy_end,
                compatibility_mode);
  const float crit_brightness = hazy_data[p];
  free_gray_image(&bright_hazy);
  // average over the brightest pixels among the most hazy pixels to
  // estimate the diffusive ambient light
  dt_aligned_pixel_t A0 = { 0.0f, 0.0f, 0.0f, 0.0f };
  size_t N_bright_hazy = 0;
  const float *const restrict data = dark_ch.data;
  const float *const restrict in_data = img.data;
  DT_OMP_FOR(reduction(+ : A0[0:4]) reduction(+ : N_bright_hazy))
  for(size_t i = 0; i < size; i++)
  {
    const float *pixel_in = in_data + 4*i;
    if((data[i] >= crit_haze_level)
       && (pixel_in[0] + pixel_in[1] + pixel_in[2] >= crit_brightness))
    {
      for_each_channel(c,aligned(pixel_in))
        A0[c] += pixel_in[c];
      N_bright_hazy++;
    }
  }
  if(N_bright_hazy > 0)
  {
    for_each_channel(c)
      A0[c] /= N_bright_hazy;
  }
  (*pA0)[0] = A0[0];
  (*pA0)[1] = A0[1];
  (*pA0)[2] = A0[2];
  free_gray_image(&dark_ch);
  // for almost haze free images it may happen that crit_haze_level=0, this means
  // there is a very large image depth, in this case a large number is returned, that
  // is small enough to avoid overflow in later processing
  // the critical haze level is at dark_channel_quantil (not 100%) to be insensitive
  // to extreme outliners, compensate for that by some factor slightly larger than
  // unity when calculating the maximal image depth
  return crit_haze_level > 0
    ? -1.125f * logf(crit_haze_level)
    : logf(FLT_MAX) / 2; // return the maximal depth
}

static inline void _restart_pipe(dt_dev_pixelpipe_t *pipe, dt_iop_module_t *self)
{
  dt_atomic_set_int(&pipe->shutdown, TRUE);
  pipe->changed |= DT_DEV_PIPE_SYNCH;
  // for now we flush the whole pipe cache
  dt_dev_pixelpipe_cache_flush(pipe);
}

void process(dt_iop_module_t *self,
             dt_dev_pixelpipe_iop_t *piece,
             const void *const ivoid,
             void *const ovoid,
             const dt_iop_roi_t *const roi_in,
             const dt_iop_roi_t *const roi_out)
{
  if(!dt_iop_have_required_input_format(4 /*we need full-color pixels*/,
                                        self, piece->colors,
                                         ivoid, ovoid, roi_in, roi_out))
    return;
  dt_iop_hazeremoval_gui_data_t *const g = self->gui_data;
  dt_iop_hazeremoval_params_t *d = piece->data;
  dt_dev_pixelpipe_t *pipe = piece->pipe;

  const int width = roi_in->width;
  const int height = roi_in->height;
  const size_t size = (size_t)width * height;
  const int w1 = 6; // window size (positive integer) for determining
                    // the dark channel and the transition map
  const int w2 = 9; // window size (positive integer) for the guided filter

  // module parameters
  const float strength = d->strength; // strength of haze removal
  const float distance = d->distance; // maximal distance from camera to remove haze
  const float eps = sqrtf(0.025f);    // regularization parameter for guided filter
  const gboolean compatibility_mode = d->compatibility_mode;

  const float *const restrict in = (float*)ivoid;
  float *const restrict out = (float*)ovoid;
  const const_rgb_image img_in = (const_rgb_image){ in, width, height, 4 };

  const dt_hash_t phash = dt_dev_pixelpipe_piece_hash(piece, NULL, TRUE);
  const gboolean fullpipe = pipe->type == DT_DEV_PIXELPIPE_FULL;
  const gboolean gui = self->dev->gui_attached && g;

  /* hazeremoval needs the color and the haziness (which yields distance_max)
      of the most hazy region of the image.
      In pixelpipe DT_DEV_PIXELPIPE_FULL we can not reliably get this value as
      the pixelpipe sees only part of the image (region of interest).

      To get consistent output in darkroom canvas and the exported image we don't
      use A0 and distance_max (A&D) calculated from the preview pipe but want data
      from a HQ pixelpipe.

      So we
      a) ensure correct data by a hash calculated from all upstream modules.
      b) If we run a full pipe without validated A&D we enforce an immediate
         HQ pipe re-run, calculate A&D and again do an immediate re-run of the pipe.
      c) If we have valid A&D we always use them, other do the fallback via
         _ambient_light().
  */
  if(gui && fullpipe && phash != g->hash)
  {
    if(!darktable.develop->late_scaling.enabled)
    {
      dt_print_pipe(DT_DEBUG_PIPE | DT_DEBUG_VERBOSE, "HQ request", pipe, piece->module, pipe->devid, NULL, NULL);
      darktable.develop->late_scaling.enabled = TRUE;
      g->redo = TRUE;
      _restart_pipe(pipe, self);
      return;
    }
    else
    {
      g->distance_max = _ambient_light(img_in, w1, &g->A0, compatibility_mode);
      g->hash = phash;
      if(g->redo)
      {
        dt_print_pipe(DT_DEBUG_PIPE | DT_DEBUG_VERBOSE, "HQ done", pipe, piece->module, pipe->devid, NULL, NULL);
        darktable.develop->late_scaling.enabled = FALSE;
        g->redo = FALSE;
        _restart_pipe(pipe, self);
        return;
      }
    }
  }

  // estimated diffusive ambient light and image depth
  rgb_pixel A0;
  float distance_max;

  const gboolean hashed = gui && phash == g->hash;
  if(hashed)
  {
    dt_print_pipe(DT_DEBUG_PIPE, "haze from HQ", pipe, piece->module, pipe->devid, NULL, NULL);
    for(int i = 0; i < 3; i++) A0[i] = g->A0[i];
    distance_max = g->distance_max;
  }
  else // In all other cases we calculate distance_max and A0 here.
    distance_max = _ambient_light(img_in, w1, &A0, compatibility_mode);

  // calculate the transition map
  gray_image trans_map = new_gray_image(width, height);
  _transition_map(img_in, trans_map, w1, A0, strength);

  // refine the transition map
  dt_box_min(trans_map.data, trans_map.height, trans_map.width, 1, w1);
  gray_image trans_map_filtered = new_gray_image(width, height);
  // apply guided filter with no clipping
  guided_filter(img_in.data, trans_map.data, trans_map_filtered.data,
                width, height, 4, w2, eps, 1.f, -FLT_MAX, FLT_MAX);

  // finally, calculate the haze-free image
  const float t_min = CLAMP(expf(-distance * distance_max), 1.0f / 1024.0f, 1.0f);

  const dt_aligned_pixel_t c_A0 = { A0[0], A0[1], A0[2], A0[3] };
  const gray_image c_trans_map_filtered = trans_map_filtered;
  DT_OMP_FOR()
  for(size_t i = 0; i < size; i++)
  {
    const float t = MAX(c_trans_map_filtered.data[i], t_min);
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
static float _ambient_light_cl(dt_iop_module_t *self,
                               const int devid,
                               cl_mem img,
                               const int w1,
                               rgb_pixel *pA0,
                               const gboolean compatibility_mode)
{
  const int width = dt_opencl_get_image_width(img);
  const int height = dt_opencl_get_image_height(img);
  const int element_size = dt_opencl_get_image_element_size(img);

  cl_int err = DT_OPENCL_SYSMEM_ALLOCATION;
  float max_depth = 0.0f;

  float *in = dt_alloc_aligned((size_t)width * height * element_size);
  if(in == NULL) goto error;

  err = dt_opencl_read_host_from_device(devid, in, img, width, height, element_size);
  if(err != CL_SUCCESS) goto error;

  const const_rgb_image img_in = (const_rgb_image)
    { in, width, height, element_size / sizeof(float) };

  max_depth = _ambient_light(img_in, w1, pA0, compatibility_mode);

error:
  dt_free_align(in);
  return max_depth;
}

static int _box_min_cl(dt_iop_module_t *self,
                       int devid,
                       cl_mem in,
                       cl_mem out,
                       const int w)
{
  dt_iop_hazeremoval_global_data_t *gd = self->global_data;
  const int width = dt_opencl_get_image_width(in);
  const int height = dt_opencl_get_image_height(in);
  cl_int err = CL_MEM_OBJECT_ALLOCATION_FAILURE;

  cl_mem temp = dt_opencl_alloc_device(devid, width, height, sizeof(float));
  if(temp == NULL) goto error;

  err = dt_opencl_enqueue_kernel_1d_args(devid, gd->kernel_hazeremoval_box_min_x, height,
              CLARG(width), CLARG(height), CLARG(in), CLARG(temp), CLARG(w));
  if(err != CL_SUCCESS) goto error;

  err = dt_opencl_enqueue_kernel_1d_args(devid, gd->kernel_hazeremoval_box_min_y, width,
              CLARG(width), CLARG(height), CLARG(temp), CLARG(out), CLARG(w));

error:
  dt_opencl_release_mem_object(temp);
  return err;
}

static int _box_max_cl(dt_iop_module_t *self,
                       int devid,
                       cl_mem in,
                       cl_mem out,
                       const int w)
{
  dt_iop_hazeremoval_global_data_t *gd = self->global_data;
  const int width = dt_opencl_get_image_width(in);
  const int height = dt_opencl_get_image_height(in);
  cl_int err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
  cl_mem temp = dt_opencl_alloc_device(devid, width, height, (int)sizeof(float));
  if(temp == NULL) goto error;

  err = dt_opencl_enqueue_kernel_1d_args(devid, gd->kernel_hazeremoval_box_max_x, height,
            CLARG(width), CLARG(height), CLARG(in), CLARG(temp), CLARG(w));
  if(err != CL_SUCCESS) goto error;

  err = dt_opencl_enqueue_kernel_1d_args(devid, gd->kernel_hazeremoval_box_max_y, width,
            CLARG(width), CLARG(height), CLARG(temp), CLARG(out), CLARG(w));
error:
  dt_opencl_release_mem_object(temp);
  return err;
}


static int _transition_map_cl(dt_iop_module_t *self,
                              int devid,
                              cl_mem img1,
                              cl_mem img2,
                              const int w1,
                              const float strength,
                              const float *const A0)
{
  dt_iop_hazeremoval_global_data_t *gd = self->global_data;
  const int width = dt_opencl_get_image_width(img1);
  const int height = dt_opencl_get_image_height(img1);

  cl_int err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_hazeremoval_transision_map, width, height,
                                                CLARG(width), CLARG(height),
                                                CLARG(img1), CLARG(img2), CLARG(strength),
                                                CLARG(A0[0]), CLARG(A0[1]), CLARG(A0[2]));
  if(err != CL_SUCCESS)
    return err;

  return _box_max_cl(self, devid, img2, img2, w1);
}


static int _dehaze_cl(dt_iop_module_t *self,
                      int devid,
                      cl_mem img_in,
                      cl_mem trans_map,
                      cl_mem img_out,
                      const float t_min,
                      const float *const A0)
{
  dt_iop_hazeremoval_global_data_t *gd = self->global_data;
  const int width = dt_opencl_get_image_width(img_in);
  const int height = dt_opencl_get_image_height(img_in);

  return dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_hazeremoval_dehaze, width, height,
                                          CLARG(width), CLARG(height),
                                          CLARG(img_in), CLARG(trans_map),
                                          CLARG(img_out), CLARG(t_min),
                                          CLARG(A0[0]), CLARG(A0[1]), CLARG(A0[2]));
}

void tiling_callback(dt_iop_module_t *self,
                     dt_dev_pixelpipe_iop_t *piece,
                     const dt_iop_roi_t *roi_in,
                     const dt_iop_roi_t *roi_out,
                     dt_develop_tiling_t *tiling)
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

int process_cl(dt_iop_module_t *self,
               dt_dev_pixelpipe_iop_t *piece,
               cl_mem img_in,
               cl_mem img_out,
               const dt_iop_roi_t *const roi_in,
               const dt_iop_roi_t *const roi_out)
{
  dt_iop_hazeremoval_gui_data_t *const g = (dt_iop_hazeremoval_gui_data_t*)self->gui_data;
  dt_iop_hazeremoval_params_t *d = piece->data;
  dt_dev_pixelpipe_t *pipe = piece->pipe;

  const int ch = piece->colors;
  const int devid = pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;
  const int w1 = 6; // window size (positive integer) for determining
                    // the dark channel and the transition map
  const int w2 = 9; // window size (positive integer) for the guided filter

  // module parameters
  const float strength = d->strength; // strength of haze removal
  const float distance = d->distance; // maximal distance from camera to remove haze
  const float eps = sqrtf(0.025f);    // regularization parameter for guided filter
  const gboolean compatibility_mode = d->compatibility_mode;

  const dt_hash_t phash = dt_dev_pixelpipe_piece_hash(piece, NULL, TRUE);
  const gboolean fullpipe = pipe->type == DT_DEV_PIXELPIPE_FULL;
  const gboolean gui = self->dev->gui_attached && g;

  // For "how this works" see cpu code
  if(gui && fullpipe && phash != g->hash)
  {
    if(!darktable.develop->late_scaling.enabled)
    {
      dt_print_pipe(DT_DEBUG_PIPE | DT_DEBUG_VERBOSE, "HQ request", pipe, piece->module, devid, NULL, NULL);
      darktable.develop->late_scaling.enabled = TRUE;
      g->redo = TRUE;
      _restart_pipe(pipe, self);
      return CL_SUCCESS;
    }
    else
    {
      g->distance_max = _ambient_light_cl(self, devid, img_in, w1, &g->A0, compatibility_mode);
      g->hash = phash;
      if(g->redo)
      {
        dt_print_pipe(DT_DEBUG_PIPE | DT_DEBUG_VERBOSE, "HQ done", pipe, piece->module, devid, NULL, NULL);
        darktable.develop->late_scaling.enabled = FALSE;
        g->redo = FALSE;
        _restart_pipe(pipe, self);
        return CL_SUCCESS;
      }
    }
  }

  // estimated diffusive ambient light and image depth
  rgb_pixel A0;
  float distance_max;

  const gboolean hashed = gui && phash == g->hash;
  if(hashed)
  {
    dt_print_pipe(DT_DEBUG_PIPE, "haze from HQ", pipe, piece->module, pipe->devid, NULL, NULL);
    for(int i = 0; i < 3; i++) A0[i] = g->A0[i];
    distance_max = g->distance_max;
  }
  else
    distance_max = _ambient_light_cl(self, devid, img_in, w1, &A0, compatibility_mode);

  cl_mem trans_map = NULL;
  cl_mem trans_map_filtered = NULL;
  cl_int err = CL_MEM_OBJECT_ALLOCATION_FAILURE;

  // calculate the transition map
  trans_map = dt_opencl_alloc_device(devid, width, height, (int)sizeof(float));
  if(trans_map == NULL) goto error;

  err = _transition_map_cl(self, devid, img_in, trans_map, w1, strength, A0);
  if(err != CL_SUCCESS) goto error;

  // refine the transition map
  err = _box_min_cl(self, devid, trans_map, trans_map, w1);
  if(err != CL_SUCCESS) goto error;

  trans_map_filtered = dt_opencl_alloc_device(devid, width, height, (int)sizeof(float));
  if(trans_map_filtered == NULL)
  {
    err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
    goto error;
  }

  // apply guided filter with no clipping
  err = guided_filter_cl(devid, img_in, trans_map, trans_map_filtered,
                         width, height, ch, w2, eps, 1.f, -CL_FLT_MAX, CL_FLT_MAX);
  if(err != CL_SUCCESS) goto error;

  // finally, calculate the haze-free image
  const float t_min = CLAMP(expf(-distance * distance_max), 1.0f / 1024.0f, 1.0f);
  err = _dehaze_cl(self, devid, img_in, trans_map_filtered, img_out, t_min, A0);

error:
  dt_opencl_release_mem_object(trans_map);
  dt_opencl_release_mem_object(trans_map_filtered);
  return err;
}
#endif

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
