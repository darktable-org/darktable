/*
    This file is part of darktable,
    Copyright (C) 2024 darktable developers.

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

#define DARKTABLE_COMPILE
#ifdef DARKTABLE_COMPILE
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "bauhaus/bauhaus.h"
#include "common/imagebuf.h"
#include "common/opencl.h"
#include "common/gaussian.h"
#include "common/box_filters.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "develop/imageop_gui.h"
#include "develop/tiling.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "iop/iop_api.h"
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <gtk/gtk.h>
#include <inttypes.h>

DT_MODULE_INTROSPECTION(2, dt_iop_pixeldeblur_params_t)

// note -- the comments after the // are used by the introspection module to define min,max,default
typedef struct dt_iop_pixeldeblur_params_t
{
  float amount;            // $MIN: -5.0  $MAX:  5.0 $DEFAULT: 1.25 $DESCRIPTION: "Strength of deblur"
  float gaussian_strength; // $MIN:  0.0  $MAX:  1.0 $DEFAULT: 0.0 $DESCRIPTION: "Smooth deblur algorithm"
  float halo_control;      // $MIN:  0.0  $MAX:  1.0 $DEFAULT: 0.33 $DESCRIPTION: "Halo control"
  float iterations;        // $MIN:  1.0  $MAX: 10.0 $DEFAULT: 2.0 $DESCRIPTION: "iterations"
  float noise_threshold;   // $MIN:  0.25  $MAX:  4.0 $DEFAULT: 2.5 $DESCRIPTION: "Threshold to correct noise pixels"
  gboolean large_radius;   // $MIN: FALSE $MAX: TRUE $DEFAULT: FALSE $DESCRIPTION" "Large radius for pixel comparisions"
} dt_iop_pixeldeblur_params_t;



// only copy params struct to avoid a commit_params() func in this module
typedef struct dt_iop_pixeldeblur_params_t dt_iop_pixeldeblur_data_t ;

typedef struct dt_iop_pixeldeblur_gui_data_t
{
  GtkWidget *amount ;
  GtkWidget *gaussian_strength ;
  GtkWidget *halo_control ;
  GtkWidget *iterations ;
  GtkWidget *noise_threshold ;
  GtkWidget *large_radius ;
} dt_iop_pixeldeblur_gui_data_t;

typedef struct dt_iop_pixeldeblur_global_data_t
{
/*    int kernel_copy_image_component_from_rgb ;  */
/*    int kernel_guided_filter_generate_result ;  */
/*    int kernel_guided_filter_add_rgb_to_image ;  */
/*    int kernel_guided_filter_split_rgb_image ;  */
/*    int kernel_guided_filter_box_mean_x ;  */
/*    int kernel_guided_filter_box_mean_y ;  */
/*    int kernel_guided_filter_covariances ;  */
/*    int kernel_guided_filter_variances ;  */
/*    int kernel_guided_filter_update_covariance ;  */
/*    int kernel_guided_filter_solve ;  */
} dt_iop_pixeldeblur_global_data_t;


const char *name()
{
  return C_("modulename", "pixel deblurring");
}

int default_group()
{
  return IOP_GROUP_CORRECT | IOP_GROUP_EFFECTS;
}

int flags()
{
  return IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_ALLOW_TILING;
}

dt_iop_colorspace_type_t default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
#define MODULE_IN_LAB_SPACE
/*  #define MODULE_IN_RGB_SPACE  */

#ifdef MODULE_IN_LAB_SPACE
  return IOP_CS_LAB;
#else
  return IOP_CS_RGB;
#endif

}

const char **description(struct dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("deblur (sharpen) the edges in the image using pixel level operations"),
                                      _("corrective"),
                                      _("linear, lab, display or scene-referred"),
                                      _("linear, lab"),
                                      _("linear, lab, display or scene-referred"));
}

void init_presets(dt_iop_module_so_t *self)
{
  // deblurring presets

  dt_gui_presets_add_generic(_("no halo control, mild"), self->op, self->version(),
                             &(dt_iop_pixeldeblur_params_t)
                               {
                                 .amount = 1.,
                                 .gaussian_strength=0.0,
                                 .halo_control=0.0,
                                 .iterations=2,
                                 .noise_threshold=3.0,
                                 .large_radius=FALSE,
                                },
                             sizeof(dt_iop_pixeldeblur_params_t), 1, DEVELOP_BLEND_CS_RGB_SCENE);

  dt_gui_presets_add_generic(_("no halo control, medium"), self->op, self->version(),
                             &(dt_iop_pixeldeblur_params_t)
                               {
                                 .amount = 1.5,
                                 .gaussian_strength=0.0,
                                 .halo_control=0.0,
                                 .iterations=2,
                                 .noise_threshold=3.0,
                                 .large_radius=FALSE,
                                },
                             sizeof(dt_iop_pixeldeblur_params_t), 1, DEVELOP_BLEND_CS_RGB_SCENE);

  dt_gui_presets_add_generic(_("no halo control, strong"), self->op, self->version(),
                             &(dt_iop_pixeldeblur_params_t)
                               {
                                 .amount = 3.0,
                                 .gaussian_strength=0.0,
                                 .halo_control=0.0,
                                 .iterations=3,
                                 .noise_threshold=3.0,
                                 .large_radius=FALSE,
                                },
                             sizeof(dt_iop_pixeldeblur_params_t), 1, DEVELOP_BLEND_CS_RGB_SCENE);

  dt_gui_presets_add_generic(_("average halo control, mild"), self->op, self->version(),
                             &(dt_iop_pixeldeblur_params_t)
                               {
                                 .amount = 1.,
                                 .gaussian_strength=0.0,
                                 .halo_control=0.3,
                                 .iterations=2,
                                 .noise_threshold=3.0,
                                 .large_radius=FALSE,
                                },
                             sizeof(dt_iop_pixeldeblur_params_t), 1, DEVELOP_BLEND_CS_RGB_SCENE);

  dt_gui_presets_add_generic(_("average halo control, medium"), self->op, self->version(),
                             &(dt_iop_pixeldeblur_params_t)
                               {
                                 .amount = 1.5,
                                 .gaussian_strength=0.0,
                                 .halo_control=0.3,
                                 .iterations=2,
                                 .noise_threshold=3.0,
                                 .large_radius=FALSE,
                                },
                             sizeof(dt_iop_pixeldeblur_params_t), 1, DEVELOP_BLEND_CS_RGB_SCENE);

  dt_gui_presets_add_generic(_("average halo control, strong"), self->op, self->version(),
                             &(dt_iop_pixeldeblur_params_t)
                               {
                                 .amount = 3.0,
                                 .gaussian_strength=0.0,
                                 .halo_control=0.3,
                                 .iterations=3,
                                 .noise_threshold=3.0,
                                 .large_radius=FALSE,
                                },
                             sizeof(dt_iop_pixeldeblur_params_t), 1, DEVELOP_BLEND_CS_RGB_SCENE);

  dt_gui_presets_add_generic(_("full halo control, mild"), self->op, self->version(),
                             &(dt_iop_pixeldeblur_params_t)
                               {
                                 .amount = 1.,
                                 .gaussian_strength=0.0,
                                 .halo_control=1.0,
                                 .iterations=4,
                                 .noise_threshold=3.0,
                                 .large_radius=FALSE,
                                },
                             sizeof(dt_iop_pixeldeblur_params_t), 1, DEVELOP_BLEND_CS_RGB_SCENE);

  dt_gui_presets_add_generic(_("full halo control, medium"), self->op, self->version(),
                             &(dt_iop_pixeldeblur_params_t)
                               {
                                 .amount = 1.5,
                                 .gaussian_strength=0.0,
                                 .halo_control=1.0,
                                 .iterations=2,
                                 .noise_threshold=3.0,
                                 .large_radius=FALSE,
                                },
                             sizeof(dt_iop_pixeldeblur_params_t), 1, DEVELOP_BLEND_CS_RGB_SCENE);

  dt_gui_presets_add_generic(_("full halo control, strong"), self->op, self->version(),
                             &(dt_iop_pixeldeblur_params_t)
                               {
                                 .amount = 3.0,
                                 .gaussian_strength=0.0,
                                 .halo_control=1.0,
                                 .iterations=3,
                                 .noise_threshold=3.0,
                                 .large_radius=FALSE,
                                },
                             sizeof(dt_iop_pixeldeblur_params_t), 1, DEVELOP_BLEND_CS_RGB_SCENE);


}



void gui_init(struct dt_iop_module_t *self)
{
  dt_iop_pixeldeblur_gui_data_t *g = IOP_GUI_ALLOC(pixeldeblur);

  g->amount = dt_bauhaus_slider_from_params(self, N_("amount"));
  dt_bauhaus_slider_set_digits(g->amount, 3);
  gtk_widget_set_tooltip_text(g->amount, _("strength of the deblurring"));

  g->halo_control = dt_bauhaus_slider_from_params(self, N_("halo_control"));
  dt_bauhaus_slider_set_digits(g->halo_control, 3);
  gtk_widget_set_tooltip_text(g->halo_control, _("0: allow halos\n1: no halos\n  with a large number of iterations can make this smaller"));

  g->iterations = dt_bauhaus_slider_from_params(self, N_("iterations"));
  dt_bauhaus_slider_set_digits(g->iterations, 0);
  gtk_widget_set_tooltip_text(g->iterations, _("increase for better halo control, especially on noisy pixels.\n     usually 3 is enough"));

  g->noise_threshold = dt_bauhaus_slider_from_params(self, N_("noise_threshold"));
  dt_bauhaus_slider_set_digits(g->noise_threshold, 2);
  gtk_widget_set_tooltip_text(g->noise_threshold, _("standard deviations from local mean to be considered a noise pixel\n- very small values will blur the image"));
  gtk_box_pack_start(GTK_BOX(self->widget), dt_ui_section_label_new(C_("section", "advanced parameters")), TRUE, TRUE, 0);

  g->gaussian_strength = dt_bauhaus_slider_from_params(self, N_("gaussian_strength"));
  dt_bauhaus_slider_set_digits(g->gaussian_strength, 3);
  gtk_widget_set_tooltip_text(g->gaussian_strength, _("higher strength blur window will `soften' results"));

  g->large_radius = dt_bauhaus_toggle_from_params(self, "large_radius");
  gtk_widget_set_tooltip_text(g->large_radius, _("expands pixel comparison radius to 2.\n"
                                                    "can get slight improvement when\n"
                                                    "inpainting is occuring.\n"
                                                    "switching to diffuse-sharpen module\n"
                                                    "is another alternative.\n"));

}



void tiling_callback(struct dt_iop_module_t *self,
                     struct dt_dev_pixelpipe_iop_t *piece,
                     const dt_iop_roi_t *roi_in,
                     const dt_iop_roi_t *roi_out,
                     struct dt_develop_tiling_t *tiling)
{
/*    dt_iop_pixeldeblur_data_t *d = (dt_iop_pixeldeblur_data_t *)piece->data;  */

  tiling->factor = 3.75f; // in + out + 7 single channel temp buffers
  tiling->factor_cl = 6.0f; // in + out + tmp
  tiling->maxbuf = 1.0f;
  tiling->maxbuf_cl = 1.0f;
  tiling->overhead = 0;
  tiling->overlap = 1; // need this for halo control search
  tiling->xalign = 1;
  tiling->yalign = 1;
  return;
}

// buffer to store single-channel image along with its dimensions
typedef struct gray_image
{
  float *data;
  int width, height, stride; // allows for common interface with lab_image
} gray_image;

#else

// buffer to store single-channel image along with its dimensions
typedef struct gray_image
{
  float *data;
  int width, height, stride; // allows for common interface with lab_image
} gray_image;

// compiling outside of darktable need some support funcs
#include "dt_support_funcs.c"

#endif


// allocate space for 1-component image of size width x height
static inline gray_image _new_gray_image(const int width,
                                        const int height)
{
  gray_image gi ;
  gi.data = dt_alloc_aligned(sizeof(float) * width * height) ;
  gi.width = width ;
  gi.height = height ;
  gi.stride = 1 ; // 1 is the stride
  return gi ;
}


// free space for 1-component image
static inline void _free_gray_image(gray_image *img_p)
{
  dt_free_align(img_p->data);
  img_p->data = NULL;
}


// copy 1-component image img1 to img2
static inline void _copy_gray_image(const gray_image img1,
                                    gray_image img2)
{
  memcpy(img2.data, img1.data, sizeof(float) * img1.width * img1.height);
}


typedef struct lab_image
{
  float *data;
  int width, height, stride;
} lab_image;


typedef struct const_lab_image
{
  const float *data;
  int width, height, stride;
} const_lab_image;


// allocate space for n-component image of size width x height
static inline lab_image _new_lab_image(const int width, const int height, const int ch)
{
  lab_image li ;
  li.data = dt_alloc_align_float(sizeof(float) * width * height * ch) ;
  li.width = width ;
  li.height = height ;
  li.stride = ch ; // 1 is the stride
  return li ;
/*    return (lab_image){ dt_alloc_align_float((size_t)width * height * ch), width, height, ch };  */
}

// free space for n-component image
static inline void _free_lab_image(lab_image *img_p)
{
  dt_free_align(img_p->data);
  img_p->data = NULL;
}

#ifndef DARKTABLE_COMPILE
#include "pixeldeblur.h"
#endif


// scale the amount of sharpening based on the zoom scale
float compute_scaled_amount(const float view_scale,
                            const float unscaled_amount)
{
  if(view_scale < 1.f)
    return unscaled_amount*view_scale ; // have views < 100% reduce with the square of the scale
  else
    return unscaled_amount ;  // in the case where preview zoom is > 100%

}


#define notPREDAMP_localminmax

// this almost certainly is going to not be necessary
void convolve3x3_separable(gray_image *pInput, float kernel[3], gray_image *pOutput, gray_image *pTmp)
{
  int width = pInput->width ;
  int height =  pInput->height ;

  float k0=kernel[0] ;
  float k1=kernel[1] ;
  // float k2=kernel[2] ; // "k2" would be same as "k0" 

  // make sure 1d is normalized
  float sum=2.*k0+k1 ;
  k0 /= sum ;
  k1 /= sum ;

  // first the rows
  {

  const float *const restrict in = (float *)pInput->data ;
  float *const restrict out = (float *)pTmp->data ;

  for(int y = 0 ; y < height ; y++) {
    int base_i = y*width ;
    // x==0
    out[base_i] = in[base_i]*k1 + 2.f*in[base_i+1]*k0 ;

#ifdef _OPENMPnot
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(base_i,in,out,width,height,k0,k1) \
  schedule(static)
#endif
    for(int x = 1 ; x < width-1 ; x++) {
      base_i++ ;
      out[base_i] = in[base_i]*k1 + (in[base_i+1] + in[base_i-1])*k0 ;
    }

    // x==width-1
    base_i = (width-1)+ y*width ;
    out[base_i] = in[base_i]*k1 + 2.f*in[base_i-1]*k0 ;
  }
  }

  // now the columns
  {

    const float *const restrict in = (float *)pTmp->data ;
    float *const restrict out = (float *)pOutput->data ;

  for(int x = 0 ; x < width ; x++) {
    int base_i = x ;
    // y==0
    out[base_i] = in[base_i]*k1 + 2.f*in[base_i+width]*k0 ;

#ifdef _OPENMPnot
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(base_i,in,out,width,height,k0,k1) \
  schedule(static)
#endif

    for(int y = 1 ; y < height-1 ; y++) {
      base_i += width ;
      out[base_i] = in[base_i]*k1 + (in[base_i+width] + in[base_i-width])*k0 ;
    }

    // y==height-1
    base_i  = x+(height-1)*width ;
    out[base_i] = in[base_i]*k1 + 2.f*in[base_i-width]*k0 ;
  }
  }

}



void clean_noisy_pixels(gray_image *pImg_input,
                        gray_image *pImg_tmp,
                        gray_image *pImg_cpe,
                        gray_image *pImg_damping,
                        float noise_threshold,
                        float maxval,
                        float halo_control)
// this function adjusts noisy pixels back to reasonable values, based on the results fitting a local central pixel area
// for all pixels,then comparing the central pixel error in a 3x3 window to the mean and stddev of the 8 neighbors
{

  if(noise_threshold > 3.9)
    return ;

  size_t n_cleaned = 0 ;
  size_t size = pImg_input->width * pImg_input->height ;

  _copy_gray_image(*pImg_input, *pImg_tmp) ;

  dt_box_mean(pImg_tmp->data,pImg_tmp->height,pImg_tmp->width, 1, 1, 1) ;

  // pImg_tmp holds the box mean value of the 3x3 window, 
  // the desired calculation is the central pixel value minus the mean of the neighbor
  // values, so the mean must have the central pixel value (cp_v) removed
  // but then the denominator for the mean must be changed from 9 to 8
  // the algebra looks like:
/*    cp_v - (mean-cp_v/9)*9/8 ;  */
/*    cp_v - mean*9/8 + cp_v/9*9/8 ;  */
/*    cp_v - mean*9/8 + cp_v/8 ;  */
/*    cp_v+cp_v/8 - mean*9/8 ;  */
/*    cp_v*9/8 - mean*9/8  */
/*    (cp_v - mean)*9/8 ;  */

#ifdef _OPENMP
#pragma omp simd
#endif
  for(int i=0 ; i < size ; i++)
    pImg_tmp->data[i] = (pImg_input->data[i] - pImg_tmp->data[i])*1.125 ;

#ifdef _OPENMP
  // everything read from tmp written to  input and cpe, no possibility of race condition for writing individual element
#pragma omp parallel for default(none) \
        dt_omp_firstprivate(noise_threshold,size,pImg_tmp,pImg_input,pImg_cpe,maxval) \
        schedule(static) \
        reduction(+:n_cleaned)
#endif

  // compute mean, stddev of pixel errors in local window
  for(int y=1 ; y < pImg_input->height-1 ; y++)
  {
    for(int x=1 ; x < pImg_input->width-1 ; x++)
    {
      int i_c = x+y*pImg_input->width ;

      float sum=0. ;
      float sum2=0. ;

      for(int iy = y-1 ; iy <= y+1 ; iy++)
      {
        int base_i = iy*pImg_input->width ;
#ifdef _OPENMP
#pragma omp simd reduction(+:sum, sum2)
#endif
        for(int ix = x-1 ; ix <= x+1 ; ix++)
        {
          float v=pImg_tmp->data[base_i+ix] ;
          sum += v ;
          sum2 += v*v ;
        }
      }

      float mean=sum/9. ;
      float var = (sum2 - sum*sum/9.)/8. ;
      float r=0. ;
      float cpe= pImg_tmp->data[i_c]-mean ; // central pixel error

      if(var > 0.) {
        r = cpe/sqrtf(var) ;
      }

      // noise pixel correction 
      float abs_cpe = fabsf(cpe) ;
      float abs_r = fabsf(r) ;

      // using maxval here to normalize the absolute value error
      // if in Lab space maxval will be 100., if in RGB space maxval will be 1.
#ifdef MODULE_IN_LAB_SPACE
      if(abs_cpe > .001f*maxval && abs_r > noise_threshold)
#else
      if(abs_cpe > .001f && abs_r > noise_threshold)
#endif
      {
        float ratio = noise_threshold/abs_r ;
        float delta=ratio*cpe-cpe ;
        delta=-cpe ;

        pImg_input->data[i_c] += delta ;
        pImg_cpe->data[i_c] = 0. ;

        n_cleaned++ ;
      } else {
        pImg_cpe->data[i_c] = cpe ;
      }

#ifdef PREDAMP_localminmax
      // now check for individual pixels that are local mins or maxs and damp them down
      float v[9] ;
      int n_in_window = window2vector(pImg_input, x, y, 1, v) ;

      int n_greater=0 ; // number of neighbors whose value is greater than central pixel
      float min=200.f ;
      float max=-200.f ;

      for(int i=0 ; i < 4 ; i++)
      {
        if(v[4] < v[i]) n_greater++ ;
        if(min > v[i]) min = v[i] ;
        if(max < v[i]) max = v[i] ;
      }

      for(int i=5 ; i < 9 ; i++)
      {
        if(v[4] < v[i]) n_greater++ ;
        if(min > v[i]) min = v[i] ;
        if(max < v[i]) max = v[i] ;
      }

      if(n_greater < 1)
      {
        // central pixel looks like a local min
        // restrain it to be equal to the neighbor max
        pImg_damping->data[i_c] = .01f - .01f*halo_control ;
      }

      if(n_greater > 7)
      {
        // central pixel looks like a local max
        // restrain it to be equal to the neighbor min
        pImg_damping->data[i_c] = .01f - .01f*halo_control ;
      }
#endif


    }
  }

  printf("Adjusted %d noisy pixels (%5.2f%% of all pixels)\n", (int)n_cleaned, (float)n_cleaned/(float)size*100.f) ;

}


void perform_heat_transfer(gray_image *pImg_input,
                            gray_image *pDeltas,
                            float scaled_amount,
                            gray_image *pDamping_factor,
                            float maxval,
                            gboolean large_radius)
{
  // note -- using Img_blurred over and over will dampen the speed of
  // changes as more of a gradient appears in the pixel to pixel values in
  // the intermediate result, because the gradient in Img_blurred will stay constant
  // this ultimately will spread the deblurring to more distant pixels, which is inconsistent
  // with the physical properties if the original blurring that happened through the camera lens
  // to the camera sensor.   Only at very high F-Stops will the actual blur amounts hitting
  // the camera sensor be significant
  // beyond a few pixels away due to diffraction blurring (with an Airy blur)

  // This algorithm does all possible pixel comparisons in a 5x5 window, comparing the central pixel
  // to all its neighbors.  It only "looks" at pixels East and South, to prevent duplicate comparisons
  // when neighbor pixels become the central pixel at further iteration

  gray_image Img_for_diff = *pImg_input ;

  // for the immediately adjacent pixels to the central pixel
  int nb_deltas_inner[][3] = {
    // top row
/*        {-1, -1, 2},  */
/*        {-1,  0, 1},  */
/*        {-1,  1, 2},  */

    // West
/*        { 0, -1, 1},  */

    // East
    { 0,  1, 1},

    // bottom row
    { 1, -1, 2},
    { 1,  0, 1},
    { 1,  1, 2}
  } ;

  // for the pixels two pixels away from the central pixel
  int nb_deltas_outer[][3] = {
    // top row
/*        {-2, -2, 8},  */
/*        {-2, -1, 5},  */
/*        {-2,  0, 4},  */
/*        {-2,  1, 5},  */
/*        {-2,  2, 8},  */

    // West,East
/*        {-1,  -2, 5},  */
/*        {-1,   2, 5},  */
/*        { 0,  -2, 4},  */

    { 0,   2, 4},
    { 1,  -2, 5},
    { 1,   2, 5},

    // bottom row
    { 2, -2, 8},
    { 2, -1, 5},
    { 2,  0, 4},
    { 2,  1, 5},
    { 2,  2, 8},

  } ;


  int width=Img_for_diff.width ;
  int height=Img_for_diff.height ;

  int MAX_NB_DIST = (large_radius == TRUE ? 2 : 1) ;

  // do inner, then outer neighbors
  for(int nb_dist = 1 ; nb_dist <= MAX_NB_DIST ; nb_dist++)
  {
    int n_nb = nb_dist == 1 ? 4 : 8 ;

    float iteration_amount = scaled_amount/(float)(n_nb)/(float)(MAX_NB_DIST) ;

    printf("PIXELDEBLUR: width:%d, amount:%f MAX_NB_DIST:%d\n", width, iteration_amount,MAX_NB_DIST) ;

    int (*nb_deltas)[3] ;

    if(nb_dist == 1)
    {
      nb_deltas = nb_deltas_inner ;
    } else {
      nb_deltas = nb_deltas_outer ;
    }

    float neighbor_wgt[8] ;
#ifdef _OPENMP
#pragma omp simd
#endif
    for(int i=0 ; i < n_nb ; i++)
    {
      float dist_sq = (float)nb_deltas[i][2] ;

      // this should be replaced by the appropriate amount
      // determined from a blurring kernel
      // looks like 1/dist is definitely better than 1./dist**2
      neighbor_wgt[i] = 1./sqrtf(dist_sq) ;
/*            neighbor_wgt[i] = 1./dist_sq ;  */
    }


#ifdef _OPENMP
// could end up with race condition with pDeltas
#pragma omp parallel for default(none) \
        dt_omp_firstprivate(iteration_amount,neighbor_wgt,n_nb, nb_dist,width,height,nb_deltas,pDamping_factor,Img_for_diff) \
        shared(pDeltas) \
        schedule(static)
#endif
    for(int y=0 ; y < height-nb_dist ; y++)
    {
      // precompute linear index offset from x,y (starting with x=0)
      int indexNoffset[8] ;

      for(int i=0 ; i < n_nb ; i++)
      {
        int dy = nb_deltas[i][0] ;
        int dx = nb_deltas[i][1] ;
        int x=0 ;

        indexNoffset[i] = (y+dy) * width + (x+dx) ;
      }

      int index0 = y * width ;

      for(int x=0 ; x < width-nb_dist ; x++, index0++)
      {
        float Img_0 = Img_for_diff.data[index0] ;
#if 1
        float f_0 = pDamping_factor->data[index0] ;
#endif
        float *pixel_out0 = &pDeltas->data[index0] ;

        for(int i=0 ; i < n_nb ; i++)
        {

          int indexN = indexNoffset[i]+x ;

#if 0
          // double check value of indexN
          int dy = nb_deltas[i][0] ;
          int dx = nb_deltas[i][1] ;
          int check = (dy+y) * width + (dx+x) ;

          if(check != indexN)
          {
            printf("%d != %d, y=%d, x=%d\n", check, indexN, y, x) ;
          }
#endif

          float Img_N = Img_for_diff.data[indexN] ;

          float Img_diffN = (Img_N-Img_0) ;

#if 1
          float f_N = pDamping_factor->data[indexN] ;
          float damping=f_0*f_N ; // damping to prevent halos for pixels just at edge of gradients ;
          float final_delta = damping*Img_diffN*iteration_amount*neighbor_wgt[i] ;
#else
          float final_delta = Img_diffN*iteration_amount*neighbor_wgt[i] ;
#endif

          float *pixel_outN = &pDeltas->data[indexN] ;

          // accumulate the changes
          pixel_out0[0] -=         final_delta ;
          pixel_outN[0] +=         final_delta ;

        }

      } // for y
    } // for x
  } // for nb_dist

}


int pixel_component_sharpen(const_lab_image *img_in,
                   lab_image *img_out,
                   const int width,
                   const int height,
                   const int ch,
                   const int debug_flag,
                   const dt_iop_pixeldeblur_data_t *d, // module parameters
                   const float scaled_amount,
                   const float view_scale,
                   const float minval,
                   const float maxval,
                   const float normalize_factor,
                   int component_to_sharpen
                   )
{
  int radius = 1 ;

  printf("PIXELDEBLUR: amount:%f, radius,%d, gaussstrength:%f halo_control:%f iterations:%d,noise_threshold:%f\n",
      scaled_amount,radius,d->gaussian_strength,d->halo_control,(int)(d->iterations+0.5),d->noise_threshold
      ) ;

  float noise_threshold = d->noise_threshold ; // pixels with (prediction error)/RMSE  greater than this will be adjusted back to the threshold value

  const size_t size = (size_t)(width * height) ;
  printf("PIXELDEBLUR: width:%d, height,%d, size:%ld in_stride:%d out_stride:%d\n", width, height, size, img_in->stride, img_out->stride) ;

  float halo_control = d->halo_control ;

  // if looking at a small scale preview, halo control will just blur the image
/*    float halo_control = d->halo_control*(view_scale-0.5f)/0.5 ;  */
/*    if(view_scale < 0.) halo_control=0. ;  */
/*    if(view_scale > 1.) halo_control=1. ;  */

/*********************************************
  // iteration strategy
  perform heat transfer
  For each pixel:
    . examine gradients from this iteration to neighbor pixels, compare to gradients from input image
    . if there has been a gradient reversal, reduce the delta for the offending pixel and also
    . set the damping factor to < 1. for the offending pixel
*********************************************************/


  gray_image img_input = _new_gray_image(width, height) ;
  gray_image img_input0 = _new_gray_image(width, height) ;
  gray_image img_blurred = _new_gray_image(width, height) ;

  gray_image img_tmp = _new_gray_image(width, height) ;
  gray_image img_damping = _new_gray_image(width, height) ;
  gray_image img_cpe = _new_gray_image(width, height) ;

  gray_image deltas = _new_gray_image(width, height) ;

  // not checking them all, if first or last alloc's failed, must be out of memory
  if( img_input.data == NULL || deltas.data == NULL)
  {
      _free_gray_image(&img_input) ;
      _free_gray_image(&img_input0) ;
      _free_gray_image(&img_blurred) ;
      _free_gray_image(&img_tmp) ;
      _free_gray_image(&img_damping) ;
      _free_gray_image(&img_cpe) ;
      _free_gray_image(&deltas) ;
    return 1;
  }

  // retrieve the component from the input image
  // need to have this in a re-writeable array, but not alter the orginal
  // copy the channel to the buffer
#ifdef _OPENMP
#pragma omp parallel for default(none) \
    dt_omp_firstprivate(size,img_in,component_to_sharpen,img_damping,img_input0) \
    schedule(static)
#endif

  for(size_t i = 0; i < size; i++)
  {
    const float *pixel_in = img_in->data + i * img_in->stride;
    img_input0.data[i] = pixel_in[component_to_sharpen] ;
    img_damping.data[i] = 1. ;
  }

  memset(deltas.data,0,sizeof(float)*size) ; // set to 0. -- can get away with memset for floats=0.0 because 0.0 is all '0' bytes

  clean_noisy_pixels(&img_input0, &img_tmp, &img_cpe, &img_damping, noise_threshold, maxval, halo_control) ;

  if(0 && scaled_amount > 1.e-12f && d->gaussian_strength > .0001f)
  {
    float desired_cw = 1. - .66*d->gaussian_strength ; // center weight desired for a kernel length 3
    float ln_x = logf(desired_cw) ;
    float ln_sigma = 1.4780914f*ln_x*ln_x-0.9754651f*ln_x-1.38629436 ;

    float sigma = expf(ln_sigma) ;

    float s2 = 2.*sigma*sigma ;

    float k0 = 1./(sqrtf(2.*M_PI)*sigma) * expf(- (1.f/s2)) ;
    float k1 = 1./(sqrtf(2.*M_PI)*sigma) * expf(- (0.f/s2)) ;
    float sum = 2.f*k0 + k1 ;
    k0 /= sum ;
    k1 /= sum ;
    float kernel[3] = {k0,k1,k0} ;

    convolve3x3_separable(&img_input0, kernel, &img_blurred, &img_tmp) ;

/*      dt_gaussian_t *g = dt_gaussian_init(width, height, 1, &maxval, &minval, sigma, 0) ;  */
/*      dt_gaussian_blur(g,img_input0.data,img_blurred.data) ;  */
/*      dt_gaussian_free(g) ;  */

  } else {
    _copy_gray_image(img_input0,img_blurred) ;
  }

  _copy_gray_image(img_input0, img_input) ;

/*    _copy_gray_image(img_blurred, img_input) ;  */


  int n_iterations = (int)(d->iterations+0.5f) ;

  for(int iteration=0 ; iteration < n_iterations ; iteration++)
  {

    perform_heat_transfer(&img_input, &deltas, scaled_amount/(float)n_iterations, &img_damping, maxval, d->large_radius) ;

    printf("Iteration %d of %d\n", iteration+1, n_iterations) ;

    if(fabsf(scaled_amount) > 1.e-12f && (halo_control > 1.e-12f))
    {
      // constrain_backward_diffusion

      // delta has the sum of changes to apply to img_input

      /*        size_t n_halo_pixels=0 ;  */

      int nb_deltas[][2] = {
        {1,0},
        {0,1},

        {-1,0},
        {0,-1}
      };


      // starting in the NW corner, and only comparing
      // to E and S

      for(int y=0 ; y < height-1 ; y++)
      {
        int base_i=y*width ;
        int i_c=base_i ; // central pixel index, this will be incremented at the end of the for(int x=...) loop immediately following

        float *in = img_input.data ; 
        float *delta = deltas.data ;
        float *damping = img_damping.data ;
        float *blurred = img_blurred.data ;

        //#pragma omp parallel for default(none) dt_omp_firstprivate(y,halo_control,deltas,base_i,nb_deltas, height, width, img_blurred, img_input, img_damping) reduction(+:n_halo_pixels)

#ifdef _OPENMP
#pragma omp parallel for default(none) dt_omp_firstprivate(y,halo_control,base_i,nb_deltas, height, width, in, delta, damping, blurred, i_c) \
        linear(i_c : 1)
#endif

        for(int x=0 ; x < width-1 ; x++)
        {


#ifdef _OPENMP
#pragma omp simd aligned(in,blurred,delta,damping : 32)
#endif
          // compare S,E
          for(int nb=0 ; nb < 2 ; nb++)
          {
            int nb_y=nb_deltas[nb][0] + y ;
            int nb_x=nb_deltas[nb][1] + x ;
            int nb_i=nb_x + nb_y*width ;

            float gradient_0 = blurred[i_c] - blurred[nb_i] ;
            float v_new = in[i_c] + delta[i_c] ;
            float vnb_new = in[nb_i] + delta[nb_i] ;
            float gradient__new = v_new - vnb_new ;

            // FIXME ? Is there a more efficient way to make this test for gradient reversal?

            if( (gradient_0 < 0. && gradient__new > 0.) || (gradient_0 > 0. && gradient__new < 0.) )
            {
              /*                n_halo_pixels++ ;  */

              if(fabs(delta[i_c]) > fabs(delta[nb_i]))
              {
                // pixel at i changed the most, make its new value equal to
                // the previous value of the neighbor
                float new_delta = in[nb_i] - in[i_c] ;

                // may want to try sqrt(halo_control) here
                delta[i_c] = (1.-halo_control)*delta[i_c]+halo_control*new_delta ;

                // just halo_control here
                damping[i_c] *= (1.-halo_control) ;
              } else {
                // vice-versa
                float new_delta = in[i_c] - in[nb_i] ;

                // may want to try sqrt(halo_control) here
                delta[nb_i] = (1.-halo_control)*delta[nb_i]+halo_control*new_delta ;

                // just halo_control here
                damping[nb_i] *= (1.-halo_control) ;
              }

            }

          } // for nb comarisons

          i_c++ ;
        } // x
      } // y

      // unfortunately this must be done at every iteration
      float *in = img_input.data ; 
      float *delta = deltas.data ;
      float *damping = img_damping.data ;
      for(int y=1 ; y < height-2 ; y++)
      {
        int base_i=y*width ;
        int i_c=base_i+1 ; // central pixel index, this will be incremented at the end of the for(int x=...) loop immediately following

#ifdef _OPENMP
#pragma omp parallel for default(none) dt_omp_firstprivate(y,halo_control,base_i,height, width,in,delta,damping) \
        linear(i_c : 1)
#endif

        for(int x=1 ; x < width-2 ; x++)
        {
          int n_greater=0 ;
          float min=200.f ;
          float max=-200.f ;
          float v_ic = in[i_c] + delta[i_c] ;

#ifdef _OPENMP
#pragma omp simd reduction(+:n_greater) reduction(min:min) reduction(max:max) aligned(in,delta,damping : 32)
#endif
          // first & last row (note the ib += width*2 in the loop)
          for(int ib=base_i-width ; ib <= base_i+width ; ib += width*2)
          {
            for(int ix=x-1 ; ix <= x+1 ; ix++)
            {
              int i=ib+ix ;
              float v = in[i] + delta[i] ;

              if(v_ic < v) n_greater++ ;
              if(min > v) min = v ;
              if(max < v) max = v ;
            }
          }

          // west
          int i=base_i-1 ;
          float v = in[i] + delta[i] ;

          if(v_ic < v) n_greater++ ;
          if(min > v) min = v ;
          if(max < v) max = v ;

          // east
          i += 2 ;
          v = in[i] + delta[i] ;

          if(v_ic < v) n_greater++ ;
          if(min > v) min = v ;
          if(max < v) max = v ;

          if(n_greater < 1)
          {
            // central pixel looks like a local min
            // damp its change

            damping[i_c] = (1.f-halo_control) ;
          }

          if(n_greater > 7)
          {
            // central pixel looks like a local max
            // damp its change

            damping[i_c] = (1.f-halo_control) ;
          }


          i_c++ ;
        } // x
      } // y

      /*        printf("Detected %d pixels with gradient reversals\n", (int)n_halo_pixels) ;  */
    }


    // now add the changes to img_input with applied halo constraints
    float *in = img_input.data ;
    float *plus = deltas.data ;
#ifdef _OPENMP
#pragma omp simd aligned(in,plus : 32)
#endif
    for(size_t i = 0; i < size; i++)
    {
      in[i] += plus[i] ;
    }


    if(iteration < n_iterations-1)
    {

      memset(deltas.data,0,sizeof(float)*size) ; // reset to 0. for next iteration -- can get away with memset because 0.0 is all '0' bytes

    } else {
      clean_noisy_pixels(&img_input, &img_tmp, &img_cpe, &img_damping, noise_threshold, maxval, halo_control) ;

      // last iteration, store result in element 0 of img_out pixels
#ifdef _OPENMP
#pragma omp parallel for default(none) dt_omp_firstprivate(size,img_out,height,width,img_input,img_input0,img_blurred,component_to_sharpen)
#endif
      for(size_t i = 0; i < size; i++)
      {

        float *pixel_out = img_out->data + i * img_out->stride;

        int y = (int)i/width ;
        int x = (int)i - y*width ;

        if(x > 0 && x < width-1 && y > 0 && y < height-1)
        {
          pixel_out[component_to_sharpen] = img_input.data[i] ;

        } else {

          // don't change border pixels ?
          pixel_out[component_to_sharpen] = img_input0.data[i] ;

        }

      }

    }

  }

  printf("PIXELDEBLUR: free()\n") ;
  _free_gray_image(&img_input) ;
  _free_gray_image(&img_input0) ;
  _free_gray_image(&img_blurred) ;
  _free_gray_image(&img_tmp) ;
  _free_gray_image(&img_damping) ;
  _free_gray_image(&img_cpe) ;
  _free_gray_image(&deltas) ;

  return 0 ;
}

#ifdef DARKTABLE_COMPILE
void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  if (!dt_iop_have_required_input_format(4 /*we need full-color pixels*/, self, piece->colors,
                                         ivoid, ovoid, roi_in, roi_out))
    return;

  int ch=piece->colors ;

  const int width = roi_in->width;
  const int height = roi_in->height;

  const dt_iop_pixeldeblur_data_t *const d = (dt_iop_pixeldeblur_data_t *)piece->data;

  const int rad = 1;
  const int w = rad*2+1 ;

  const float view_scale = (float)roi_in->scale / (float)piece->iscale ;
  const float scaled_amount = compute_scaled_amount(view_scale, d->amount) ;

  printf("PIXELDEBLUR: WxH= %d x %d, window=%d, scl_amount=%f view_scale=%f roiscale:%f piecescale:%f\n",
      width, height, w, scaled_amount, view_scale, (float)roi_in->scale , (float)piece->iscale) ;

  // Special case handling: very small image with one or two dimensions below 2*rad+1 treat as no processing and just
  // pass through.  also, if piece->iscale > 1.2, no point trying to improve an image that is already scaled down by 1./1.2
  if((float)piece->iscale > 1.2f || rad == 0 || (roi_out->width < 2 * rad + 1 || roi_out->height < 2 * rad + 1))
  {
    printf("PIXELDEBLUR: COPY WxH= %d x %d\n", width, height) ;
    dt_iop_image_copy_by_size(ovoid, ivoid, roi_out->width, roi_out->height, 4);
    return;
  }

  const_lab_image img_in = (const_lab_image){ ivoid, width, height, ch };
  lab_image img_out = (lab_image){ ovoid, width, height, ch };
  dt_iop_copy_image_roi(ovoid, ivoid, 4, roi_in, roi_out);

  int debug_flag=0 ;
  int failed=0 ;
#ifdef MODULE_IN_LAB_SPACE
  for(int component=0 ; failed==0 && component<3 ; component++) {
    float normalize_factor=.01 ;
    float minval[3] = {0., -128., -128.} ;
    float maxval[3] = {150., 128., 128.} ;
#else
  for(int component=0 ; failed==0 && component<3 ; component++) {
    float minval[3] = {0., 0., 0.} ;
    float maxval[3] = {1., 1., 1.} ;
    float normalize_factor=1. ;
#endif


    failed = pixel_component_sharpen(&img_in, &img_out, width, height, ch, debug_flag, d, scaled_amount,view_scale,
                                     minval[component],maxval[component],normalize_factor,component) ;
  }

  if(failed) {
    dt_print(DT_DEBUG_ALWAYS,"[pixeldebug] out of memory\n");
    dt_iop_copy_image_roi(ovoid, ivoid, 4, roi_in, roi_out);
  } else if(piece->pipe->mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK) {
    dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);
  }

  printf("PIXELDEBLUR: Finished\n") ;
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_pixeldeblur_data_t));
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}


int create_and_check_kernel(const int program, char *kernel_name)
{
    int kernel_id = dt_opencl_create_kernel(program, kernel_name) ;

    if(kernel_id < 0) fprintf(stderr, "FAILED to create kernel %s\n", kernel_name) ;

    return kernel_id ;
}


void init_global(dt_iop_module_so_t *module)
{
  dt_iop_pixeldeblur_global_data_t *gd
      = (dt_iop_pixeldeblur_global_data_t *)malloc(sizeof(dt_iop_pixeldeblur_global_data_t));
  module->data = gd;

/*    const int program = 26; // guided_filter.cl, from data/kernels/programs.conf  */

/*    gd->kernel_copy_image_component_from_rgb  */
/*      = create_and_check_kernel(program, "copy_image_component_from_rgb") ;  */
/*    */
/*    gd->kernel_guided_filter_generate_result  */
/*      = create_and_check_kernel(program, "guided_filter_generate_result") ;  */
/*    */
/*    gd->kernel_guided_filter_add_rgb_to_image  */
/*      = create_and_check_kernel(program, "guided_filter_add_rgb_to_image") ;  */
/*    */
/*    gd->kernel_guided_filter_split_rgb_image  */
/*      = create_and_check_kernel(program, "guided_filter_split_rgb_image") ;  */
/*    */
/*    gd->kernel_guided_filter_box_mean_x  */
/*      = create_and_check_kernel(program, "guided_filter_box_mean_x") ;  */
/*    */
/*    gd->kernel_guided_filter_box_mean_y  */
/*      = create_and_check_kernel(program, "guided_filter_box_mean_y") ;  */
/*    */
/*    gd->kernel_guided_filter_covariances  */
/*      = create_and_check_kernel(program, "guided_filter_covariances") ;  */
/*    */
/*    gd->kernel_guided_filter_variances  */
/*      = create_and_check_kernel(program, "guided_filter_variances") ;  */
/*    */
/*    gd->kernel_guided_filter_update_covariance  */
/*      = create_and_check_kernel(program, "guided_filter_update_covariance") ;  */
/*    */
/*    gd->kernel_guided_filter_solve  */
/*      = create_and_check_kernel(program, "guided_filter_solve") ;  */

}

void cleanup_global(dt_iop_module_so_t *module)
{
/*    dt_iop_pixeldeblur_global_data_t *gd = (dt_iop_pixeldeblur_global_data_t *)module->data;  */

/*    dt_opencl_free_kernel( gd->kernel_copy_image_component_from_rgb ) ;  */
/*    dt_opencl_free_kernel( gd->kernel_guided_filter_generate_result ) ;  */
/*    dt_opencl_free_kernel( gd->kernel_guided_filter_add_rgb_to_image ) ;  */
/*    dt_opencl_free_kernel( gd->kernel_guided_filter_split_rgb_image ) ;  */
/*    dt_opencl_free_kernel( gd->kernel_guided_filter_box_mean_x ) ;  */
/*    dt_opencl_free_kernel( gd->kernel_guided_filter_box_mean_y ) ;  */
/*    dt_opencl_free_kernel( gd->kernel_guided_filter_covariances ) ;  */
/*    dt_opencl_free_kernel( gd->kernel_guided_filter_variances ) ;  */
/*    dt_opencl_free_kernel( gd->kernel_guided_filter_update_covariance ) ;  */
/*    dt_opencl_free_kernel( gd->kernel_guided_filter_solve ) ;  */

  free(module->data);
  module->data = NULL;
}

#endif

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

