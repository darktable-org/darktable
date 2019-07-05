/*
    This file is part of darktable,
    copyright (c) 2019 Jacques Le Clerc

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


#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "bauhaus/bauhaus.h"
#include "develop/imageop.h"
#include "dtgtk/button.h"
#include "dtgtk/resetlabel.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"

/* We use G'MIC library in this module */
#include "gmic_libc.h"

#include <gtk/gtk.h>
#include <stdlib.h>

#define min(a, b) ((a) < (b)) ? (a) : (b)
#define max(a, b) ((a) > (b)) ? (a) : (b)

// this is the version of the modules parameters,
// and includes version information about compile-time dt
DT_MODULE_INTROSPECTION(1, dt_iop_inpaint_params_t)


typedef enum dt_iop_inpaint_algo_t
{
  DT_IOP_INPAINT_GMIC_HOLES = 0,
  DT_IOP_INPAINT_GMIC_MORPHOLOGICAL = 1,
  DT_IOP_INPAINT_GMIC_MULTI_SCALE = 2,
  DT_IOP_INPAINT_GMIC_PATCH_BASED = 3,
  DT_IOP_INPAINT_GMIC_TRANSPORT_DIFFUSION = 4,
  DT_IOP_INPAINT_GMIC_DISPLAY = 5,
  DT_IOP_INPAINT_BCT = 6,
} dt_iop_inpaint_algo_t;

typedef enum dt_iop_mask_t
{
  DT_IOP_MASK_RED = 0,
  DT_IOP_MASK_GREEN = 1,
  DT_IOP_MASK_BLUE = 2,
  DT_IOP_MASK_BLACK = 3,
  DT_IOP_MASK_WHITE = 4
} dt_iop_mask_t;

typedef enum GMIC_CONNECTIVITY_t
{
  GMIC_CONNECTIVITY_LOW = 0,
  GMIC_CONNECTIVITY_HIGH = 1
} GMIC_CONNECTIVITY_t;

typedef enum GMIC_PROCESS_BLOCK_SIZE_t
{
  GMIC_PROCESS_BLOCK_SIZE_100 = 0,
  GMIC_PROCESS_BLOCK_SIZE_75 = 1,
  GMIC_PROCESS_BLOCK_SIZE_50 = 2,
  GMIC_PROCESS_BLOCK_SIZE_25 = 3,
  GMIC_PROCESS_BLOCK_SIZE_10 = 4,
  GMIC_PROCESS_BLOCK_SIZE_5 = 5,
  GMIC_PROCESS_BLOCK_SIZE_2 = 6,
  GMIC_PROCESS_BLOCK_SIZE_1 = 7
} GMIC_PROCESS_BLOCK_SIZE_t;

typedef enum GMIC_REGUL_t
{
  GMIC_REGUL_ISOTROPIC = 0,
  GMIC_REGUL_DELAUNAY_ORIENTED = 1,
  GMIC_REGUL_EDGE_ORIENTED = 2
} GMIC_REGUL_t;


#define DEBUG

// G'MIC verbose level: "v - " = quiet, "" = verbose
#define GMIC_VERBOSE  ""

/* G'MIC command timeout (seconds) */
#define GMIC_TIMEOUT  "30"

/* G'MIC mask selection tolerance */
#define GMIC_SELECTION_TOLERANCE  "25"

#define GMIC_MASK_DEFAULT DT_IOP_MASK_RED

#define GMIC_MASK_DILATION_MIN 0
#define GMIC_MASK_DILATION_MAX 32
#define GMIC_MASK_DILATION_STEP 1
#define GMIC_MASK_DILATION_DEFAULT 0

#define GMIC_MAX_AREA_MIN 1.0
#define GMIC_MAX_AREA_MAX 512.0
#define GMIC_MAX_AREA_STEP 1.0
#define GMIC_MAX_AREA_DEFAULT 4.0

#define GMIC_TOLERANCE_MIN 0.0
#define GMIC_TOLERANCE_MAX 255.0
#define GMIC_TOLERANCE_STEP 1.0
#define GMIC_TOLERANCE_DEFAULT 20.0

#define GMIC_CONNECTIVITY_DEFAULT GMIC_CONNECTIVITY_HIGH

#define GMIC_NB_SCALES_MIN 0
#define GMIC_NB_SCALES_MAX 16
#define GMIC_NB_SCALES_STEP 1
#define GMIC_NB_SCALES_DEFAULT 0

#define GMIC_PATCH_SIZE_MIN 1
#define GMIC_PATCH_SIZE_MAX 64
#define GMIC_PATCH_SIZE_STEP 1
#define GMIC_PATCH_SIZE_DEFAULT 9

#define GMIC_LOOKUP_SIZE_MIN 1.0
#define GMIC_LOOKUP_SIZE_MAX 32.0
#define GMIC_LOOKUP_SIZE_STEP 0.1
#define GMIC_LOOKUP_SIZE_DEFAULT 16.0

#define GMIC_LOOKUP_FACTOR_MIN 0.0
#define GMIC_LOOKUP_FACTOR_MAX 1.0
#define GMIC_LOOKUP_FACTOR_STEP 0.1
#define GMIC_LOOKUP_FACTOR_DEFAULT 0.1

#define GMIC_ITER_PER_SCALE_MIN 1
#define GMIC_ITER_PER_SCALE_MAX 100
#define GMIC_ITER_PER_SCALE_STEP 1
#define GMIC_ITER_PER_SCALE_DEFAULT 10

#define GMIC_BLEND_SIZEI_MIN 0
#define GMIC_BLEND_SIZEI_MAX 32
#define GMIC_BLEND_SIZEI_STEP 1
#define GMIC_BLEND_SIZEI_DEFAULT 5

#define GMIC_BLEND_SIZEF_MIN 0.0
#define GMIC_BLEND_SIZEF_MAX 5.0
#define GMIC_BLEND_SIZEF_STEP 0.1
#define GMIC_BLEND_SIZEF_DEFAULT 1.2

#define GMIC_BLEND_THRESHOLD_MIN 0.0
#define GMIC_BLEND_THRESHOLD_MAX 1.0
#define GMIC_BLEND_THRESHOLD_STEP 0.01
#define GMIC_BLEND_THRESHOLD_DEFAULT 0.0

#define GMIC_BLEND_DECAY_MIN 0.0
#define GMIC_BLEND_DECAY_MAX 0.5
#define GMIC_BLEND_DECAY_STEP 0.01
#define GMIC_BLEND_DECAY_DEFAULT 0.05

#define GMIC_BLEND_SCALES_MIN 1
#define GMIC_BLEND_SCALES_MAX 20
#define GMIC_BLEND_SCALES_STEP 1
#define GMIC_BLEND_SCALES_DEFAULT 10

#define GMIC_ALLOW_OUTER_BLENDING_DEFAULT 1

#define GMIC_BLOCK_SIZE_MIN 0
#define GMIC_BLOCK_SIZE_MAX 255
#define GMIC_BLOCK_SIZE_STEP 1
#define GMIC_BLOCK_SIZE_DEFAULT 20

#define GMIC_PROCESS_BLOCK_SIZE_DEFAULT GMIC_PROCESS_BLOCK_SIZE_100

#define GMIC_SMOOTHNESS_MIN 0.0
#define GMIC_SMOOTHNESS_MAX 100.0
#define GMIC_SMOOTHNESS_STEP 1.0
#define GMIC_SMOOTHNESS_DEFAULT 75.0

#define GMIC_REGUL_DEFAULT GMIC_REGUL_DELAUNAY_ORIENTED

#define GMIC_REGUL_ITER_MIN 0
#define GMIC_REGUL_ITER_MAX 100
#define GMIC_REGUL_ITER_STEP 1
#define GMIC_REGUL_ITER_DEFAULT 20



typedef struct dt_iop_inpaint_params_t
{
  // these are stored in db.
  dt_iop_inpaint_algo_t algo;
  dt_iop_mask_t mask;
  uint32_t mask_dilation;
  float max_area;
  float tolerance;
  GMIC_CONNECTIVITY_t connectivity;
  uint32_t nb_scales;
  uint32_t patch_size;
  float lookup_size;
  float lookup_factor;
  uint32_t iter_per_scale;
  uint32_t blend_sizei;
  float blend_sizef;
  float blend_threshold;
  float blend_decay;
  uint32_t blend_scales;
  int allow_outer_blending;
  GMIC_PROCESS_BLOCK_SIZE_t process_bloc_size;
  float smoothness;
  GMIC_REGUL_t regul;
  int regul_iter;
} dt_iop_inpaint_params_t;


typedef struct dt_iop_inpaint_gui_data_t
{
  GtkWidget *algo;
  GtkWidget *mask_area;
  GtkWidget *mask;
  GtkWidget *mask_color;
  GtkWidget *mask_dilation;
  GtkWidget *max_area;
  GtkWidget *tolerance;
  GtkWidget *connectivity;
  GtkWidget *nb_scales;
  GtkWidget *patch_size;
  GtkWidget *lookup_size;
  GtkWidget *lookup_factor;
  GtkWidget *iter_per_scale;
  GtkWidget *blend_sizei;
  GtkWidget *blend_sizef;
  GtkWidget *blend_threshold;
  GtkWidget *blend_decay;
  GtkWidget *blend_scales;
  GtkWidget *allow_outer_blending;
  GtkWidget *process_bloc_size;
  GtkWidget *smoothness;
  GtkWidget *regul;
  GtkWidget *regul_iter;
  GSList *gw_list;
} dt_iop_inpaint_gui_data_t;

typedef struct dt_iop_inpaint_params_t dt_iop_inpaint_data_t;


// this returns a translatable name
const char *name()
{
  return _("inpaint");
}

int default_group()
{
  return IOP_GROUP_CORRECT;
}

int flags()
{
  return IOP_FLAGS_SUPPORTS_BLENDING;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return iop_cs_rgb;
}


static void dt_to_gmic(const float *const in, gmic_interface_image *img, const float scale)
{
    float *ptr = img->data;
    const int ch = 4;

    for(unsigned int y = 0; y < img->height; ++y)
    {
      const float *g_in = in + (y * img->width * ch);

      for(unsigned int x = 0; x < img->width; ++x)
        for(unsigned int c = 0; c < img->spectrum; ++c)
          ptr[(c * img->width * img->height) + (y * img->width) + x] = fmin(g_in[(x * ch) + c] * scale, scale);
    }
}


static void gmic_to_dt(gmic_interface_image *img, float *out, const float scale)
{
    float *ptr = img->data;
    const int ch = 4;

    for(int y = 0; y < img->height; ++y)
    {
      float *g_out = out + (y * img->width * ch);

      for(int x = 0; x < img->width; ++x)
        for(int c = 0; c < img->spectrum; ++c)
          g_out[(x * ch) + c] = ptr[(c * img->width * img->height) + (y * img->width) + x] * scale;
    }
}


static void gmic_process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ibuf, void *const obuf,
             const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out,
             char *gmic_cmd_line, int image_nb)
{
  const int width = roi_in->width;
  const int height = roi_in->height;
  const int ch = piece->colors;

  const dt_iop_order_iccprofile_info_t *const srgb_profile = dt_ioppr_add_profile_info_to_list(self->dev, DT_COLORSPACE_SRGB, "", INTENT_PERCEPTUAL);
  const dt_iop_order_iccprofile_info_t *const work_profile = dt_ioppr_get_pipe_work_profile_info(piece->pipe);

  gmic_interface_image images[2];
  unsigned int nofImages = image_nb;
  bool abort = false;
  float progress;

  memset(&images, 0, sizeof(gmic_interface_image) * 2);

  // Set the names of the images (optional)
  g_stpcpy(images[0].name, "Input Image");
  g_stpcpy(images[1].name, "Mask Image");

  // Set the dimensions of the input image [0]
  images[0].width = width;
  images[0].height = height;
  images[0].spectrum = 3;
  images[0].depth = 1;
  images[0].is_interleaved = false;
  images[0].format = E_FORMAT_FLOAT;

  // Set the dimensions of the input image [1]
  images[1].width = width;
  images[1].height = height;
  images[1].spectrum = 3;
  images[1].depth = 1;
  images[1].is_interleaved = false;
  images[1].format = E_FORMAT_FLOAT;

  // Set pointer to iop input in the images structure.
  images[0].data = malloc(width * height * sizeof(float) * 3);
  images[1].data = malloc(width * height * sizeof(float) * 3);

  // Create options structure and initialize it.
  gmic_interface_options options;
  memset(&options, 0, sizeof(gmic_interface_options));
  options.ignore_stdlib = false;
  options.p_is_abort = &abort;
  options.p_progress = &progress;
  options.interleave_output = false;
  options.no_inplace_processing = true;
  options.output_format = E_FORMAT_FLOAT;

  // Work profile to SRGB profile
  if (work_profile && srgb_profile)
    dt_ioppr_transform_image_colorspace_rgb(ibuf, obuf, width, height, work_profile, srgb_profile, "GMIC process");
  else
    memcpy(obuf, ibuf, width * height * ch * sizeof(float));

  dt_to_gmic(obuf, &images[0], 255.0);
  dt_pthread_mutex_lock(&darktable.plugin_threadsafe);

#ifdef DEBUG
  g_printf(">>>>> %s\n", gmic_cmd_line);
#endif

  // call to the G'MIC library
  gmic_call(gmic_cmd_line, &nofImages, &images[0], &options);

  dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);
  gmic_to_dt(&images[0], obuf, 1.0/255.0);

  // SRGB profile to work profile
  if (work_profile && srgb_profile)
    dt_ioppr_transform_image_colorspace_rgb(obuf, obuf, width, height, srgb_profile, work_profile, "GMIC process");

  free(images[0].data);
  free(images[1].data);

  // We have to dispose output images we got back from the gmic_call that were
  // not created by this thread.
  // Therefore, for any image data we did not allocate ourselves, we have to call the
  // external delete function.
  //for (int i = 0; i<nofImages; ++i) {
  //  if (images[i].data!=obuf) {
  //    gmic_delete_external((float*)images[i].data);
  //  }
  //}

}

// Float to String with 1 decimal digit and . separator (not ,)
static void f_to_s1(char *s, float f)
{
  sprintf(s, "%.1f", f);
  for (char * p = s; (p = strchr(p, ',')); ++p)
    *p = '.';
}

// Float to String with 2 decimal digit and . separator (not ,)
static void f_to_s2(char *s, float f)
{
  sprintf(s, "%.2f", f);
  for (char * p = s; (p = strchr(p, ',')); ++p)
    *p = '.';
}

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ibuf, void *const obuf,
             const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const int width = roi_in->width;
  const int height = roi_in->height;
  const int ch = piece->colors;
  char *mask_color;
  int block_size;

  char gmic_cmd_line[1000];
  char s_max_area[10];
  char s_tolerance[10];
  char s_lookup_size[10];
  char s_lookup_factor[10];
  char s_blend_size[10];
  char s_blend_threshold[10];
  char s_blend_decay[10];
  char s_smoothness[10];

  dt_iop_inpaint_params_t *p = (dt_iop_inpaint_params_t *)piece->data;

  switch(p->mask)
  {
   default:
   case DT_IOP_MASK_RED: mask_color = "255,0,0"; break;
   case DT_IOP_MASK_GREEN: mask_color = "0,255,0"; break;
   case DT_IOP_MASK_BLUE: mask_color = "0,0,255"; break;
   case DT_IOP_MASK_BLACK: mask_color = "0,0,0"; break;
   case DT_IOP_MASK_WHITE: mask_color = "255,255,255"; break;
  }

  switch(p->algo)
  {
    case DT_IOP_INPAINT_GMIC_HOLES:
      f_to_s1(s_max_area, p->max_area);
      f_to_s1(s_tolerance, p->tolerance);
      sprintf(gmic_cmd_line, GMIC_VERBOSE"apply_timeout \"inpaint_holes {%s^1.5},%s,%i\","GMIC_TIMEOUT, s_max_area, s_tolerance, p->connectivity);
      gmic_process(self, piece, ibuf, obuf, roi_in, roi_out, gmic_cmd_line, 1);
      break;

    case DT_IOP_INPAINT_GMIC_MORPHOLOGICAL:
      if (p->mask_dilation > 0)
        sprintf(gmic_cmd_line, GMIC_VERBOSE"+round select_color. "GMIC_SELECTION_TOLERANCE",{round([%s])} dilate. {1+2*%i} apply_timeout \"inpaint_morpho.. [1]\","GMIC_TIMEOUT" rm.",
          mask_color, p->mask_dilation);
      else
        sprintf(gmic_cmd_line, GMIC_VERBOSE"+round select_color. "GMIC_SELECTION_TOLERANCE",{round([%s])} apply_timeout \"inpaint_morpho.. [1]\","GMIC_TIMEOUT" rm.",
          mask_color);
      gmic_process(self, piece, ibuf, obuf, roi_in, roi_out, gmic_cmd_line, 1);
      break;

    case DT_IOP_INPAINT_GMIC_MULTI_SCALE:
      if (p->mask_dilation > 0)
        sprintf(gmic_cmd_line, GMIC_VERBOSE"+round select_color. "GMIC_SELECTION_TOLERANCE",{round([%s])} dilate. {1+2*%i} srand 0 apply_timeout \"inpaint_matchpatch.. [1],%i,%i,%i,%i,%i\","GMIC_TIMEOUT" rm.",
          mask_color, p->mask_dilation, p->nb_scales, p->patch_size, p->iter_per_scale, p->blend_sizei, p->allow_outer_blending);
      else
        sprintf(gmic_cmd_line, GMIC_VERBOSE"+round select_color. "GMIC_SELECTION_TOLERANCE",{round([%s])} srand 0 apply_timeout \"inpaint_matchpatch.. [1],%i,%i,%i,%i,%i\","GMIC_TIMEOUT" rm.",
          mask_color, p->nb_scales, p->patch_size, p->iter_per_scale, p->blend_sizei, p->allow_outer_blending);
      gmic_process(self, piece, ibuf, obuf, roi_in, roi_out, gmic_cmd_line, 1);
      break;

    case DT_IOP_INPAINT_GMIC_PATCH_BASED:
      switch (p->process_bloc_size)
      {
        default:
        case GMIC_PROCESS_BLOCK_SIZE_100: block_size = 100; break;
        case GMIC_PROCESS_BLOCK_SIZE_75: block_size = 75; break;
        case GMIC_PROCESS_BLOCK_SIZE_50: block_size = 50; break;
        case GMIC_PROCESS_BLOCK_SIZE_25: block_size = 25; break;
        case GMIC_PROCESS_BLOCK_SIZE_10: block_size = 10; break;
        case GMIC_PROCESS_BLOCK_SIZE_5: block_size = 5; break;
        case GMIC_PROCESS_BLOCK_SIZE_2: block_size = 2; break;
        case GMIC_PROCESS_BLOCK_SIZE_1: block_size = 1; break;
      }
      block_size = max(16, min(width, height) * block_size / 100);
      f_to_s1(s_lookup_size, p->patch_size * p->lookup_size);
      f_to_s1(s_lookup_factor, p->lookup_factor);
      f_to_s1(s_blend_size, p->patch_size * p->blend_sizef);
      f_to_s2(s_blend_threshold, p->blend_threshold);
      f_to_s2(s_blend_decay, p->blend_decay);

      if (p->mask_dilation > 0)
        sprintf(gmic_cmd_line, GMIC_VERBOSE"at \"+round select_color. "GMIC_SELECTION_TOLERANCE",{round([%s])} dilate. {1+2*%i} inpaint.. [1],%i,%s,%s,1,%s,%s,%s,%i,%i rm.\",%i,%i,1,25%%,25%%,0,2",
          mask_color, p->mask_dilation,
          p->patch_size, s_lookup_size, s_lookup_factor,
          s_blend_size, s_blend_threshold, s_blend_decay, p->blend_scales, p->allow_outer_blending,
          block_size, block_size);
      else
        sprintf(gmic_cmd_line, GMIC_VERBOSE"at \"+round select_color. "GMIC_SELECTION_TOLERANCE",{round([%s])} inpaint.. [1],%i,%s,%s,1,%s,%s,%s,%i,%i rm.\",%i,%i,1,25%%,25%%,0,2",
          mask_color, 
          p->patch_size, s_lookup_size, s_lookup_factor,
          s_blend_size, s_blend_threshold, s_blend_decay, p->blend_scales, p->allow_outer_blending,
          block_size, block_size);
      gmic_process(self, piece, ibuf, obuf, roi_in, roi_out, gmic_cmd_line, 1);
      break;

    case DT_IOP_INPAINT_GMIC_TRANSPORT_DIFFUSION:
      f_to_s1(s_smoothness, p->smoothness);
      if (p->mask_dilation > 0)
        sprintf(gmic_cmd_line, GMIC_VERBOSE"+select_color "GMIC_SELECTION_TOLERANCE",%s dilate. {1+2*%i} apply_timeout \"inpaint_pde.. [1],%s,%i,%i\","GMIC_TIMEOUT" rm. cut 0,255",
          mask_color, p->mask_dilation, s_smoothness, p->regul, p->regul_iter);
      else
        sprintf(gmic_cmd_line, GMIC_VERBOSE"+select_color "GMIC_SELECTION_TOLERANCE",%s apply_timeout \"inpaint_pde.. [1],%s,%i,%i\","GMIC_TIMEOUT" rm. cut 0,255",
          mask_color, s_smoothness, p->regul, p->regul_iter);
      gmic_process(self, piece, ibuf, obuf, roi_in, roi_out, gmic_cmd_line, 1);
      break;

    case DT_IOP_INPAINT_GMIC_DISPLAY:
      sprintf(gmic_cmd_line, GMIC_VERBOSE"d0");
      gmic_process(self, piece, ibuf, obuf, roi_in, roi_out, gmic_cmd_line, 1);
      break;

    case DT_IOP_INPAINT_BCT:

    default:
      memcpy(obuf, ibuf, width * height * ch * sizeof(float));
      break;
  }
}


/** init, cleanup, commit to pipeline */
void init(dt_iop_module_t *module)
{
  // we don't need global data:
  module->global_data = NULL; // malloc(sizeof(dt_iop_inpaint_global_data_t));
  module->params = calloc(1, sizeof(dt_iop_inpaint_params_t));
  module->default_params = calloc(1, sizeof(dt_iop_inpaint_params_t));
  // our module is disabled by default
  module->default_enabled = 0;
  module->params_size = sizeof(dt_iop_inpaint_params_t);
  module->gui_data = NULL;

  // init defaults:
  dt_iop_inpaint_params_t tmp = {
    .algo = DT_IOP_INPAINT_GMIC_HOLES,
    .mask = GMIC_MASK_DEFAULT,
    .mask_dilation = GMIC_MASK_DILATION_DEFAULT,
    .max_area = GMIC_MAX_AREA_DEFAULT,
    .tolerance = GMIC_TOLERANCE_DEFAULT,
    .connectivity = GMIC_CONNECTIVITY_DEFAULT,
    .nb_scales = GMIC_NB_SCALES_DEFAULT,
    .patch_size = GMIC_PATCH_SIZE_DEFAULT,
    .lookup_size = GMIC_LOOKUP_SIZE_DEFAULT,
    .lookup_factor = GMIC_LOOKUP_FACTOR_DEFAULT,
    .iter_per_scale = GMIC_ITER_PER_SCALE_DEFAULT,
    .blend_sizei = GMIC_BLEND_SIZEI_DEFAULT,
    .blend_sizef = GMIC_BLEND_SIZEF_DEFAULT,
    .blend_threshold = GMIC_BLEND_THRESHOLD_DEFAULT,
    .blend_decay = GMIC_BLEND_DECAY_DEFAULT,
    .blend_scales = GMIC_BLEND_SCALES_DEFAULT,
    .allow_outer_blending = GMIC_ALLOW_OUTER_BLENDING_DEFAULT,
    .process_bloc_size = GMIC_PROCESS_BLOCK_SIZE_DEFAULT,
    .smoothness = GMIC_SMOOTHNESS_DEFAULT,
    .regul = GMIC_REGUL_DEFAULT,
    .regul_iter = GMIC_REGUL_ITER_DEFAULT
  };

  memcpy(module->params, &tmp, sizeof(dt_iop_inpaint_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_inpaint_params_t));
}

void cleanup(dt_iop_module_t *module)
{
  free(module->params);
  module->params = NULL;
}



/** commit is the synch point between core and gui, so it copies params to pipe data. */
void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *params, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  memcpy(piece->data, params, sizeof(dt_iop_inpaint_params_t));
}


static void display_algo_param_widget(dt_iop_inpaint_gui_data_t *g, dt_iop_inpaint_algo_t algo)
{
  for (GSList *gw = g->gw_list; gw ;gw = gw->next)
    switch(algo)
    {
      case DT_IOP_INPAINT_GMIC_HOLES:
        if (gw->data == g->max_area
         || gw->data == g->tolerance
         || gw->data == g->connectivity)
          gtk_widget_show(gw->data);
        else
          gtk_widget_hide(gw->data);
        break;

      case DT_IOP_INPAINT_GMIC_MORPHOLOGICAL:
        if (gw->data == g->mask_area
         || gw->data == g->mask_dilation)
          gtk_widget_show(gw->data);
        else
          gtk_widget_hide(gw->data);
        break;

      case DT_IOP_INPAINT_GMIC_MULTI_SCALE:
        if (gw->data == g->mask_area
         || gw->data == g->mask_dilation
         || gw->data == g->nb_scales
         || gw->data == g->patch_size
         || gw->data == g->iter_per_scale
         || gw->data == g->blend_sizei
         || gw->data == g->allow_outer_blending)
          gtk_widget_show(gw->data);
        else
          gtk_widget_hide(gw->data);
        break;

      case DT_IOP_INPAINT_GMIC_PATCH_BASED:
        if (gw->data == g->mask_area
         || gw->data == g->mask_dilation
         || gw->data == g->patch_size
         || gw->data == g->lookup_size
         || gw->data == g->lookup_factor
         || gw->data == g->blend_sizef
         || gw->data == g->blend_threshold
         || gw->data == g->blend_decay
         || gw->data == g->blend_scales
         || gw->data == g->allow_outer_blending
         || gw->data == g->process_bloc_size)
          gtk_widget_show(gw->data);
        else
          gtk_widget_hide(gw->data);
        break;

      case DT_IOP_INPAINT_GMIC_TRANSPORT_DIFFUSION:
        if (gw->data == g->mask_area
         || gw->data == g->mask_dilation
         || gw->data == g->smoothness
         || gw->data == g->regul
         || gw->data == g->regul_iter)
          gtk_widget_show(gw->data);
        else
          gtk_widget_hide(gw->data);
        break;

      default:
        gtk_widget_hide(gw->data);
        break;
    }
}

static void algo_callback(GtkWidget *w, dt_iop_module_t *self)
{
  if (darktable.gui->reset) return;
  dt_iop_inpaint_gui_data_t *g = (dt_iop_inpaint_gui_data_t *)self->gui_data;
  dt_iop_inpaint_params_t *p = (dt_iop_inpaint_params_t *)self->params;
  p->algo = dt_bauhaus_combobox_get(w);
  display_algo_param_widget(g, p->algo);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void mask_callback(GtkWidget *w, dt_iop_module_t *self)
{
  if (darktable.gui->reset) return;
  dt_iop_inpaint_params_t *p = (dt_iop_inpaint_params_t *)self->params;
  p->mask = dt_bauhaus_combobox_get(w);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void mask_dilation_callback(GtkWidget *w, dt_iop_module_t *self)
{
  if (darktable.gui->reset) return;
  dt_iop_inpaint_params_t *p = (dt_iop_inpaint_params_t *)self->params;
  p->mask_dilation = dt_bauhaus_slider_get(w);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void max_area_callback(GtkWidget *w, dt_iop_module_t *self)
{
  if (darktable.gui->reset) return;
  dt_iop_inpaint_params_t *p = (dt_iop_inpaint_params_t *)self->params;
  p->max_area = dt_bauhaus_slider_get(w);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void tolerance_callback(GtkWidget *w, dt_iop_module_t *self)
{
  if (darktable.gui->reset) return;
  dt_iop_inpaint_params_t *p = (dt_iop_inpaint_params_t *)self->params;
  p->tolerance = dt_bauhaus_slider_get(w);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void connectivity_callback(GtkWidget *w, dt_iop_module_t *self)
{
  if (darktable.gui->reset) return;
  dt_iop_inpaint_params_t *p = (dt_iop_inpaint_params_t *)self->params;
  p->connectivity = dt_bauhaus_combobox_get(w);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void nb_scales_callback(GtkWidget *w, dt_iop_module_t *self)
{
  if (darktable.gui->reset) return;
  dt_iop_inpaint_params_t *p = (dt_iop_inpaint_params_t *)self->params;
  p->nb_scales = dt_bauhaus_slider_get(w);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void patch_size_callback(GtkWidget *w, dt_iop_module_t *self)
{
  if (darktable.gui->reset) return;
  dt_iop_inpaint_params_t *p = (dt_iop_inpaint_params_t *)self->params;
  p->patch_size = dt_bauhaus_slider_get(w);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void lookup_size_callback(GtkWidget *w, dt_iop_module_t *self)
{
  if (darktable.gui->reset) return;
  dt_iop_inpaint_params_t *p = (dt_iop_inpaint_params_t *)self->params;
  p->lookup_size = dt_bauhaus_slider_get(w);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void lookup_factor_callback(GtkWidget *w, dt_iop_module_t *self)
{
  if (darktable.gui->reset) return;
  dt_iop_inpaint_params_t *p = (dt_iop_inpaint_params_t *)self->params;
  p->lookup_factor = dt_bauhaus_slider_get(w);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void iter_per_scale_callback(GtkWidget *w, dt_iop_module_t *self)
{
  if (darktable.gui->reset) return;
  dt_iop_inpaint_params_t *p = (dt_iop_inpaint_params_t *)self->params;
  p->iter_per_scale = dt_bauhaus_slider_get(w);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void blend_sizei_callback(GtkWidget *w, dt_iop_module_t *self)
{
  if (darktable.gui->reset) return;
  dt_iop_inpaint_params_t *p = (dt_iop_inpaint_params_t *)self->params;
  p->blend_sizei = dt_bauhaus_slider_get(w);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void blend_sizef_callback(GtkWidget *w, dt_iop_module_t *self)
{
  if (darktable.gui->reset) return;
  dt_iop_inpaint_params_t *p = (dt_iop_inpaint_params_t *)self->params;
  p->blend_sizef = dt_bauhaus_slider_get(w);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void blend_threshold_callback(GtkWidget *w, dt_iop_module_t *self)
{
  if (darktable.gui->reset) return;
  dt_iop_inpaint_params_t *p = (dt_iop_inpaint_params_t *)self->params;
  p->blend_threshold = dt_bauhaus_slider_get(w);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void blend_decay_callback(GtkWidget *w, dt_iop_module_t *self)
{
  if (darktable.gui->reset) return;
  dt_iop_inpaint_params_t *p = (dt_iop_inpaint_params_t *)self->params;
  p->blend_decay = dt_bauhaus_slider_get(w);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void blend_scales_callback(GtkWidget *w, dt_iop_module_t *self)
{
  if (darktable.gui->reset) return;
  dt_iop_inpaint_params_t *p = (dt_iop_inpaint_params_t *)self->params;
  p->blend_scales = dt_bauhaus_slider_get(w);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void allow_outer_blending_callback(GtkWidget *w, dt_iop_module_t *self)
{
  if (darktable.gui->reset) return;
  dt_iop_inpaint_params_t *p = (dt_iop_inpaint_params_t *)self->params;
  p->allow_outer_blending = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w));
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void process_bloc_size_callback(GtkWidget *w, dt_iop_module_t *self)
{
  if (darktable.gui->reset) return;
  dt_iop_inpaint_params_t *p = (dt_iop_inpaint_params_t *)self->params;
  p->process_bloc_size = dt_bauhaus_combobox_get(w);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void smoothness_callback(GtkWidget *w, dt_iop_module_t *self)
{
  if (darktable.gui->reset) return;
  dt_iop_inpaint_params_t *p = (dt_iop_inpaint_params_t *)self->params;
  p->smoothness = dt_bauhaus_combobox_get(w);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void regul_callback(GtkWidget *w, dt_iop_module_t *self)
{
  if (darktable.gui->reset) return;
  dt_iop_inpaint_params_t *p = (dt_iop_inpaint_params_t *)self->params;
  p->regul = dt_bauhaus_combobox_get(w);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void regul_iter_callback(GtkWidget *w, dt_iop_module_t *self)
{
  if (darktable.gui->reset) return;
  dt_iop_inpaint_params_t *p = (dt_iop_inpaint_params_t *)self->params;
  p->regul_iter = dt_bauhaus_combobox_get(w);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}


/** gui callbacks, these are needed. */
void gui_update(dt_iop_module_t *self)
{
  // let gui match current parameters:
  dt_iop_inpaint_gui_data_t *g = (dt_iop_inpaint_gui_data_t *)self->gui_data;
  dt_iop_inpaint_params_t *p = (dt_iop_inpaint_params_t *)self->params;

  dt_bauhaus_combobox_set(g->algo, p->algo);
  dt_bauhaus_combobox_set(g->mask, p->mask);
  dt_bauhaus_slider_set(g->mask_dilation, p->mask_dilation);
  dt_bauhaus_slider_set(g->max_area, p->max_area);
  dt_bauhaus_slider_set(g->tolerance, p->tolerance);
  dt_bauhaus_combobox_set(g->connectivity, p->connectivity);
  dt_bauhaus_slider_set(g->nb_scales, p->nb_scales);
  dt_bauhaus_slider_set(g->patch_size, p->patch_size);
  dt_bauhaus_slider_set(g->lookup_size, p->lookup_size);
  dt_bauhaus_slider_set(g->lookup_factor, p->lookup_factor);
  dt_bauhaus_slider_set(g->iter_per_scale, p->iter_per_scale);
  dt_bauhaus_slider_set(g->blend_sizei, p->blend_sizei);
  dt_bauhaus_slider_set(g->blend_sizef, p->blend_sizef);
  dt_bauhaus_slider_set(g->blend_threshold, p->blend_threshold);
  dt_bauhaus_slider_set(g->blend_decay, p->blend_decay);
  dt_bauhaus_slider_set(g->blend_scales, p->blend_scales);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->allow_outer_blending), p->allow_outer_blending);
  dt_bauhaus_combobox_set(g->process_bloc_size, p->process_bloc_size);
  dt_bauhaus_slider_set(g->smoothness, p->smoothness);
  dt_bauhaus_combobox_set(g->regul, p->regul);
  dt_bauhaus_slider_set(g->regul_iter, p->regul_iter);

  display_algo_param_widget(g, p->algo);
}

void gui_init(dt_iop_module_t *self)
{
  // init the slider 
  self->gui_data = malloc(sizeof(dt_iop_inpaint_gui_data_t));
  dt_iop_inpaint_gui_data_t *g = (dt_iop_inpaint_gui_data_t *)self->gui_data;
  g->gw_list = NULL;

  dt_iop_inpaint_params_t *p = (dt_iop_inpaint_params_t *)self->params;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
  dt_gui_add_help_link(self->widget, dt_get_help_url(self->op));

  // Algorithm combobox
  g->algo = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->algo, NULL, _("algorithm"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->algo, TRUE, TRUE, 0);
  dt_bauhaus_combobox_add(g->algo, _("G'MIC Holes"));
  dt_bauhaus_combobox_add(g->algo, _("G'MIC Morphological"));
  dt_bauhaus_combobox_add(g->algo, _("G'MIC Multi Scales"));
  dt_bauhaus_combobox_add(g->algo, _("G'MIC Patch Based"));
  dt_bauhaus_combobox_add(g->algo, _("G'MIC Transport Diffusion"));
//dt_bauhaus_combobox_add(g->algo, _("G'MIC Display (debug)"));
//dt_bauhaus_combobox_add(g->algo, _("Fast Inpaint BCT"));
//dt_bauhaus_combobox_add(g->algo, _("Patch Match"));
  gtk_widget_set_tooltip_text(g->algo, _("in-paint algorithm"));
  g_signal_connect(G_OBJECT(g->algo), "value-changed", G_CALLBACK(algo_callback), self);

  g->mask_area = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_BAUHAUS_SPACE);
  g->gw_list = g_slist_append(g->gw_list, g->mask_area);
  gtk_box_pack_start(GTK_BOX(self->widget), g->mask_area, TRUE, TRUE, 0);

  // Mask combobox
  g->mask = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->mask, NULL, _("mask"));
  gtk_box_pack_start(GTK_BOX(g->mask_area), g->mask, TRUE, TRUE, 0);
  dt_bauhaus_combobox_add(g->mask, _("Red"));
  dt_bauhaus_combobox_add(g->mask, _("Green"));
  dt_bauhaus_combobox_add(g->mask, _("Blue"));
  dt_bauhaus_combobox_add(g->mask, _("Black"));
  dt_bauhaus_combobox_add(g->mask, _("White"));
  gtk_widget_set_tooltip_text(g->mask, _("Mask"));
  g_signal_connect(G_OBJECT(g->mask), "value-changed", G_CALLBACK(mask_callback), self);

  g->mask_color = dtgtk_togglebutton_new(dtgtk_cairo_paint_color, CPF_STYLE_BOX, NULL);
  gtk_box_pack_start(GTK_BOX(g->mask_area), g->mask_color, FALSE, FALSE, 0);

   // Mask Dilation slider
  g->mask_dilation = dt_bauhaus_slider_new_with_range(self,
    GMIC_MASK_DILATION_MIN, GMIC_MASK_DILATION_MAX, GMIC_MASK_DILATION_STEP, p->mask_dilation, 0);
  g->gw_list = g_slist_append(g->gw_list, g->mask_dilation);
  gtk_widget_set_tooltip_text(g->mask_dilation, _("Mask Dilation"));
  dt_bauhaus_widget_set_label(g->mask_dilation, NULL, _("mask dilation"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->mask_dilation, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(g->mask_dilation), "value-changed", G_CALLBACK(mask_dilation_callback), self);

  // Max Area slider
  g->max_area = dt_bauhaus_slider_new_with_range(self,
    GMIC_MAX_AREA_MIN, GMIC_MAX_AREA_MAX, GMIC_MAX_AREA_STEP, p->max_area, 0);
  g->gw_list = g_slist_append(g->gw_list, g->max_area);
  gtk_widget_set_tooltip_text(g->max_area, _("Maximum area"));
  dt_bauhaus_widget_set_label(g->max_area, NULL, _("max area"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->max_area, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(g->max_area), "value-changed", G_CALLBACK(max_area_callback), self);

  // Tolerance slider
  g->tolerance = dt_bauhaus_slider_new_with_range(self,
    GMIC_TOLERANCE_MIN, GMIC_TOLERANCE_MAX, GMIC_TOLERANCE_STEP, p->tolerance, 0);
  g->gw_list = g_slist_append(g->gw_list, g->tolerance);
  gtk_widget_set_tooltip_text(g->tolerance, _("Tolerance"));
  dt_bauhaus_widget_set_label(g->tolerance, NULL, _("tolerance"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->tolerance, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(g->tolerance), "value-changed", G_CALLBACK(tolerance_callback), self);

  // Connectivity combobox
  g->connectivity = dt_bauhaus_combobox_new(self);
  g->gw_list = g_slist_append(g->gw_list, g->connectivity);
  dt_bauhaus_widget_set_label(g->connectivity, NULL, _("connectivity"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->connectivity, TRUE, TRUE, 0);
  dt_bauhaus_combobox_add(g->connectivity, _("Low"));
  dt_bauhaus_combobox_add(g->connectivity, _("High"));
  gtk_widget_set_tooltip_text(g->connectivity, _("Connectivity"));
  g_signal_connect(G_OBJECT(g->connectivity), "value-changed", G_CALLBACK(connectivity_callback), self);

  // Number of Scales slider
  g->nb_scales = dt_bauhaus_slider_new_with_range(self,
    GMIC_NB_SCALES_MIN, GMIC_NB_SCALES_MAX, GMIC_NB_SCALES_STEP, p->nb_scales, 0);
  g->gw_list = g_slist_append(g->gw_list, g->nb_scales);
  gtk_widget_set_tooltip_text(g->nb_scales, _("Number of scales. Set to 0 for automatic scale detection"));
  dt_bauhaus_widget_set_label(g->nb_scales, NULL, _("number of scales"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->nb_scales, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(g->nb_scales), "value-changed", G_CALLBACK(nb_scales_callback), self);

  // Patch Size slider
  g->patch_size = dt_bauhaus_slider_new_with_range(self,
    GMIC_PATCH_SIZE_MIN, GMIC_PATCH_SIZE_MAX, GMIC_PATCH_SIZE_STEP, p->patch_size, 0);
  g->gw_list = g_slist_append(g->gw_list, g->patch_size);
  gtk_widget_set_tooltip_text(g->patch_size, _("Patch Size"));
  dt_bauhaus_widget_set_label(g->patch_size, NULL, _("patch size"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->patch_size, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(g->patch_size), "value-changed", G_CALLBACK(patch_size_callback), self);

  // Lookup Size slider
  g->lookup_size = dt_bauhaus_slider_new_with_range(self,
    GMIC_LOOKUP_SIZE_MIN, GMIC_LOOKUP_SIZE_MAX, GMIC_LOOKUP_SIZE_STEP, p->lookup_size, 1);
  g->gw_list = g_slist_append(g->gw_list, g->lookup_size);
  gtk_widget_set_tooltip_text(g->lookup_size, _("Lookup Size"));
  dt_bauhaus_widget_set_label(g->lookup_size, NULL, _("lookup size"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->lookup_size, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(g->lookup_size), "value-changed", G_CALLBACK(lookup_size_callback), self);

  // Lookup Factor slider
  g->lookup_factor = dt_bauhaus_slider_new_with_range(self,
    GMIC_LOOKUP_FACTOR_MIN, GMIC_LOOKUP_FACTOR_MAX, GMIC_LOOKUP_FACTOR_STEP, p->lookup_factor, 1);
  g->gw_list = g_slist_append(g->gw_list, g->lookup_factor);
  gtk_widget_set_tooltip_text(g->lookup_factor, _("Lookup Factor"));
  dt_bauhaus_widget_set_label(g->lookup_factor, NULL, _("lookup factor"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->lookup_factor, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(g->lookup_factor), "value-changed", G_CALLBACK(lookup_factor_callback), self);

  // Number of Iterations per Scale slider
  g->iter_per_scale = dt_bauhaus_slider_new_with_range(self,
    GMIC_ITER_PER_SCALE_MIN, GMIC_ITER_PER_SCALE_MAX, GMIC_ITER_PER_SCALE_STEP, p->iter_per_scale, 0);
  g->gw_list = g_slist_append(g->gw_list, g->iter_per_scale);
  gtk_widget_set_tooltip_text(g->iter_per_scale, _("Number of Iterations per Scale"));
  dt_bauhaus_widget_set_label(g->iter_per_scale, NULL, _("iterations per scale"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->iter_per_scale, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(g->iter_per_scale), "value-changed", G_CALLBACK(iter_per_scale_callback), self);

  // Blend Size slider (int)
  g->blend_sizei = dt_bauhaus_slider_new_with_range(self,
    GMIC_BLEND_SIZEI_MIN, GMIC_BLEND_SIZEI_MAX, GMIC_BLEND_SIZEI_STEP, p->blend_sizei, 0);
  g->gw_list = g_slist_append(g->gw_list, g->blend_sizei);
  gtk_widget_set_tooltip_text(g->blend_sizei, _("Blend Size"));
  dt_bauhaus_widget_set_label(g->blend_sizei, NULL, _("blend size"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->blend_sizei, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(g->blend_sizei), "value-changed", G_CALLBACK(blend_sizei_callback), self);

  // Blend Size slider (float)
  g->blend_sizef = dt_bauhaus_slider_new_with_range(self,
    GMIC_BLEND_SIZEF_MIN, GMIC_BLEND_SIZEF_MAX, GMIC_BLEND_SIZEF_STEP, p->blend_sizef, 1);
  g->gw_list = g_slist_append(g->gw_list, g->blend_sizef);
  gtk_widget_set_tooltip_text(g->blend_sizef, _("Blend Size"));
  dt_bauhaus_widget_set_label(g->blend_sizef, NULL, _("blend size"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->blend_sizef, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(g->blend_sizef), "value-changed", G_CALLBACK(blend_sizef_callback), self);

  // Blend Threshold slider
  g->blend_threshold = dt_bauhaus_slider_new_with_range(self,
    GMIC_BLEND_THRESHOLD_MIN, GMIC_BLEND_THRESHOLD_MAX, GMIC_BLEND_THRESHOLD_STEP, p->blend_threshold, 2);
  g->gw_list = g_slist_append(g->gw_list, g->blend_threshold);
  gtk_widget_set_tooltip_text(g->blend_threshold, _("Blend Threshold"));
  dt_bauhaus_widget_set_label(g->blend_threshold, NULL, _("blend thresold"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->blend_threshold, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(g->blend_threshold), "value-changed", G_CALLBACK(blend_threshold_callback), self);

 // Blend Decay slider
  g->blend_decay = dt_bauhaus_slider_new_with_range(self,
    GMIC_BLEND_DECAY_MIN, GMIC_BLEND_DECAY_MAX, GMIC_BLEND_DECAY_STEP, p->blend_decay, 2);
  g->gw_list = g_slist_append(g->gw_list, g->blend_decay);
  gtk_widget_set_tooltip_text(g->blend_decay, _("Blend Decay"));
  dt_bauhaus_widget_set_label(g->blend_decay, NULL, _("blend decay"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->blend_decay, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(g->blend_decay), "value-changed", G_CALLBACK(blend_decay_callback), self);

 // Blend Scales slider
  g->blend_scales = dt_bauhaus_slider_new_with_range(self,
    GMIC_BLEND_SCALES_MIN, GMIC_BLEND_SCALES_MAX, GMIC_BLEND_SCALES_STEP, p->blend_scales, 0);
  g->gw_list = g_slist_append(g->gw_list, g->blend_scales);
  gtk_widget_set_tooltip_text(g->blend_scales, _("Blend Scales"));
  dt_bauhaus_widget_set_label(g->blend_scales, NULL, _("blend scales"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->blend_scales, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(g->blend_scales), "value-changed", G_CALLBACK(blend_scales_callback), self);

  // Allow Outer Blending check button
  g->allow_outer_blending = gtk_check_button_new_with_label(_("allow outer blending"));
  g->gw_list = g_slist_append(g->gw_list, g->allow_outer_blending);
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON(g->allow_outer_blending), p->allow_outer_blending);
  gtk_widget_set_tooltip_text(g->allow_outer_blending, _("Allow Outer Blending"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->allow_outer_blending , TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(g->allow_outer_blending), "toggled", G_CALLBACK(allow_outer_blending_callback), self);

  // Process by Blocs of Size combobox
  g->process_bloc_size = dt_bauhaus_combobox_new(self);
  g->gw_list = g_slist_append(g->gw_list, g->process_bloc_size);
  dt_bauhaus_widget_set_label(g->process_bloc_size, NULL, _("process by blocs of size"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->process_bloc_size, TRUE, TRUE, 0);
  dt_bauhaus_combobox_add(g->process_bloc_size, _("100%"));
  dt_bauhaus_combobox_add(g->process_bloc_size, _("75%"));
  dt_bauhaus_combobox_add(g->process_bloc_size, _("50%"));
  dt_bauhaus_combobox_add(g->process_bloc_size, _("25%"));
  dt_bauhaus_combobox_add(g->process_bloc_size, _("10%"));
  dt_bauhaus_combobox_add(g->process_bloc_size, _("5%"));
  dt_bauhaus_combobox_add(g->process_bloc_size, _("2%"));
  dt_bauhaus_combobox_add(g->process_bloc_size, _("1%"));
  gtk_widget_set_tooltip_text(g->process_bloc_size, _("Process by Blocs of Size"));
  g_signal_connect(G_OBJECT(g->process_bloc_size), "value-changed", G_CALLBACK(process_bloc_size_callback), self);

 // Smoothness slider
  g->smoothness = dt_bauhaus_slider_new_with_range(self,
    GMIC_SMOOTHNESS_MIN, GMIC_SMOOTHNESS_MAX, GMIC_SMOOTHNESS_STEP, p->smoothness, 0);
  g->gw_list = g_slist_append(g->gw_list, g->smoothness);
  gtk_widget_set_tooltip_text(g->smoothness, _("Smoothness (%)"));
  dt_bauhaus_widget_set_label(g->smoothness, NULL, _("smoothness (%)"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->smoothness, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(g->smoothness), "value-changed", G_CALLBACK(smoothness_callback), self);

 // Regularization combobox
  g->regul = dt_bauhaus_combobox_new(self);
  g->gw_list = g_slist_append(g->gw_list, g->regul);
  dt_bauhaus_widget_set_label(g->regul, NULL, _("regularization"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->regul, TRUE, TRUE, 0);
  dt_bauhaus_combobox_add(g->regul, _("Isotropic"));
  dt_bauhaus_combobox_add(g->regul, _("Delaunay-Oriented"));
  dt_bauhaus_combobox_add(g->regul, _("Edge-Oriented"));
  gtk_widget_set_tooltip_text(g->regul, _("Regularization"));
  g_signal_connect(G_OBJECT(g->regul), "value-changed", G_CALLBACK(regul_callback), self);

 // Regularization iterations slider
  g->regul_iter = dt_bauhaus_slider_new_with_range(self,
    GMIC_REGUL_ITER_MIN, GMIC_REGUL_ITER_MAX, GMIC_REGUL_ITER_STEP, p->regul_iter, 0);
  g->gw_list = g_slist_append(g->gw_list, g->regul_iter);
  gtk_widget_set_tooltip_text(g->regul_iter, _("regularization iterations"));
  dt_bauhaus_widget_set_label(g->regul_iter, NULL, _("Regularization Iterations"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->regul_iter, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(g->regul_iter), "value-changed", G_CALLBACK(regul_iter_callback), self);

}

void gui_cleanup(dt_iop_module_t *self)
{
  dt_iop_inpaint_gui_data_t *g = (dt_iop_inpaint_gui_data_t *)self->gui_data;
  g_slist_free(g->gw_list);

  // nothing else necessary, gtk will clean up the labels
  free(self->gui_data);
  self->gui_data = NULL;
}


// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
