/*
    This file is part of darktable,
    Copyright (C) 2020-2024 darktable developers.

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

#include "bauhaus/bauhaus.h"
#include "common/darktable.h"
#include "common/opencl.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "develop/imageop_gui.h"
#include "develop/openmp_maths.h"
#include "dtgtk/button.h"
#include "dtgtk/resetlabel.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "gui/color_picker_proxy.h"
#include "iop/iop_api.h"

#include <glib.h>
#include <math.h>
#include <stdlib.h>

/** DOCUMENTATION
 *
 * This module inverts scanned film negatives and produces linear, scene-referred
 * RGB output using the Kodak Cineon densitometry algorithm.
 *
 * Pipeline:
 *   1. Convert scanner transmission values to density using Dmin (film base) as fulcrum
 *   2. Correct density in log space using per-channel white balance and offset
 *   3. Convert corrected density back to linear light via 10^x
 *   4. Apply a contrast power curve on the linear result
 *   5. Scale by an output exposure factor
 *
 * The output is linear scene-referred, suitable for further processing by
 * tone-mapping modules such as filmic, sigmoid, or agx.
 *
 * References:
 *  - https://www.kodak.com/uploadedfiles/motion/US_plugins_acrobat_en_motion_education_sensitometry_workbook.pdf
 *  - http://www.digital-intermediate.co.uk/film/pdf/Cineon.pdf
 *  - https://lists.gnu.org/archive/html/openexr-devel/2005-03/msg00009.html
 **/

 #define THRESHOLD 2.3283064365386963e-10f // -32 EV
 #define LOG2_to_LOG10 0.3010299956f           // log10(2)


DT_MODULE_INTROSPECTION(2, dt_iop_filminversion_params_t)


typedef enum dt_iop_filminversion_filmstock_t
{
  // What kind of emulsion are we working on ?
  DT_FILMSTOCK_NB = 0,   // $DESCRIPTION: "black and white film"
  DT_FILMSTOCK_COLOR = 1 // $DESCRIPTION: "color film"
} dt_iop_filminversion_filmstock_t;


typedef struct dt_iop_filminversion_params_t
{
  dt_iop_filminversion_filmstock_t film_stock; /* $DEFAULT: DT_FILMSTOCK_COLOR $DESCRIPTION: "film stock" */
  float Dmin[4];                            /* color of film substrate
                                               $MIN: 0.00001 $MAX: 1.5 $DEFAULT: 1.0 */
  float wb_high[4];                         /* white balance RGB coeffs (illuminant)
                                               $MIN: 0.25 $MAX: 2 $DEFAULT: 1.0 */
  float wb_low[4];                          /* white balance RGB offsets (base light)
                                               $MIN: 0.25 $MAX: 2 $DEFAULT: 1.0 */
  float D_max;                              /* max density of film
                                               $MIN: 0.1 $MAX: 6 $DEFAULT: 2.046 */
  float offset;                             /* inversion offset
                                               $MIN: -1.0 $MAX: 1.0 $DEFAULT: -0.05 $DESCRIPTION: "scan exposure bias" */
  float contrast;                           /* contrast power curve applied on linear result
                                               $MIN: 1.0 $MAX: 5.0 $DEFAULT: 3.0 $DESCRIPTION: "contrast" */
  float exposure;                           /* output exposure scaling
                                               $MIN: 0.5 $MAX: 8.0 $DEFAULT: 1.0 $DESCRIPTION: "output exposure" */
} dt_iop_filminversion_params_t;


typedef struct dt_iop_filminversion_data_t
{
  dt_aligned_pixel_t Dmin;                // color of film substrate
  dt_aligned_pixel_t wb_high;             // white balance RGB coeffs / Dmax
  dt_aligned_pixel_t offset;              // inversion offset
  float contrast;                         // contrast power on corrected density
  float exposure;                         // output exposure scaling
} dt_iop_filminversion_data_t;


typedef struct dt_iop_filminversion_gui_data_t
{
  GtkNotebook *notebook;
  GtkWidget *film_stock;
  GtkWidget *Dmin_R, *Dmin_G, *Dmin_B;
  GtkWidget *wb_high_R, *wb_high_G, *wb_high_B;
  GtkWidget *wb_low_R, *wb_low_G, *wb_low_B;
  GtkWidget *D_max;
  GtkWidget *offset;
  GtkWidget *contrast;
  GtkWidget *exposure;
  GtkWidget *Dmin_picker, *Dmin_sampler;
  GtkWidget *WB_high_picker, *WB_high_sampler;
  GtkWidget *WB_low_picker, *WB_low_sampler;
} dt_iop_filminversion_gui_data_t;


typedef struct dt_iop_filminversion_global_data_t
{
  int kernel_filminversion;
} dt_iop_filminversion_global_data_t;


const char *name()
{
  return _("film inversion");
}

const char *aliases()
{
  return _("film|invert|negative|scan|scene-referred");
}

const char **description(dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("invert film negative scans to linear scene-referred RGB"),
                                      _("corrective"),
                                      _("linear, RGB, scene-referred"),
                                      _("linear, RGB"),
                                      _("linear, RGB, scene-referred"));
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_ONE_INSTANCE;
}


int default_group()
{
  return IOP_GROUP_BASIC | IOP_GROUP_TECHNICAL;
}


dt_iop_colorspace_type_t default_colorspace(dt_iop_module_t *self,
                                            dt_dev_pixelpipe_t *pipe,
                                            dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

int legacy_params(dt_iop_module_t *self,
                  const void *const old_params,
                  const int old_version,
                  void **new_params,
                  int32_t *new_params_size,
                  int *new_version)
{
  typedef struct dt_iop_filminversion_params_v2_t
  {
    dt_iop_filminversion_filmstock_t film_stock;
    float Dmin[4];
    float wb_high[4];
    float wb_low[4];
    float D_max;
    float offset;
    float contrast;
    float exposure;
  } dt_iop_filminversion_params_v2_t;

  if(old_version == 1)
  {
    typedef struct dt_iop_filminversion_params_v1_t
    {
      dt_iop_filminversion_filmstock_t film_stock;
      float Dmin[4];
      float wb_high[4];
      float wb_low[4];
      float D_max;
      float offset;
      float exposure;
    } dt_iop_filminversion_params_v1_t;

    const dt_iop_filminversion_params_v1_t *o = (dt_iop_filminversion_params_v1_t *)old_params;
    dt_iop_filminversion_params_v2_t *n = malloc(sizeof(dt_iop_filminversion_params_v2_t));

    n->film_stock = o->film_stock;
    n->Dmin[0] = o->Dmin[0];
    n->Dmin[1] = o->Dmin[1];
    n->Dmin[2] = o->Dmin[2];
    n->Dmin[3] = o->Dmin[3];
    n->wb_high[0] = o->wb_high[0];
    n->wb_high[1] = o->wb_high[1];
    n->wb_high[2] = o->wb_high[2];
    n->wb_high[3] = o->wb_high[3];
    n->wb_low[0] = o->wb_low[0];
    n->wb_low[1] = o->wb_low[1];
    n->wb_low[2] = o->wb_low[2];
    n->wb_low[3] = o->wb_low[3];
    n->D_max = o->D_max;
    n->offset = o->offset;
    n->contrast = 1.0f; // v1 had no contrast, equivalent to 1.0
    n->exposure = o->exposure;

    *new_params = n;
    *new_params_size = sizeof(dt_iop_filminversion_params_v2_t);
    *new_version = 2;
    return 0;
  }
  return 1;
}

void commit_params(dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  const dt_iop_filminversion_params_t *const p = (dt_iop_filminversion_params_t *)p1;
  dt_iop_filminversion_data_t *const d = piece->data;

  // keep WB_high even in B&W mode to apply sepia or warm tone look
  // but premultiply it ahead with Dmax to spare one div per pixel
  for(size_t c = 0; c < 4; c++)
    d->wb_high[c] = p->wb_high[c] / p->D_max;

  for(size_t c = 0; c < 4; c++)
    d->offset[c] = p->wb_high[c] * p->offset * p->wb_low[c];

  // ensure we use a monochrome Dmin for B&W film
  if(p->film_stock == DT_FILMSTOCK_COLOR)
    for(size_t c = 0; c < 4; c++) d->Dmin[c] = p->Dmin[c];
  else if(p->film_stock == DT_FILMSTOCK_NB)
    for(size_t c = 0; c < 4; c++) d->Dmin[c] = p->Dmin[0];

  d->contrast = p->contrast;
  d->exposure = p->exposure;
}

static inline void _process_pixel(const dt_aligned_pixel_t pix_in,
                                  dt_aligned_pixel_t pix_out,
                                  const dt_aligned_pixel_t Dmin,
                                  const dt_aligned_pixel_t wb_high,
                                  const dt_aligned_pixel_t offset,
                                  const float contrast,
                                  const float exposure)
{
    // Step 1: clamp input and compute density ratio Dmin / pixel
    dt_aligned_pixel_t density;
    for_each_channel(c)
      density[c] = Dmin[c] / fmaxf(pix_in[c], THRESHOLD);

    // Step 2: log2(density) then convert to -log10 via LOG2_to_LOG10
    dt_aligned_pixel_t log_density;
    dt_vector_log2(density, log_density);
    for_each_channel(c)
      log_density[c] *= -LOG2_to_LOG10;
    // now log_density = -log10( Dmin / max(pix_in, THRESHOLD) )

    // Step 3: corrected density in log space
    dt_aligned_pixel_t corrected_de;
    for_each_channel(c)
      corrected_de[c] = wb_high[c] * log_density[c] + offset[c];

    // Step 4: convert corrected density to linear light: 1 - 10^corrected_de
    dt_aligned_pixel_t ten_to_x;
    dt_vector_exp10(corrected_de, ten_to_x);
    dt_aligned_pixel_t print_linear;
    for_each_channel(c)
      print_linear[c] = fmaxf(1.0f - ten_to_x[c], 0.0f);

    // Step 5: apply contrast as power curve on linear result
    // pow(linear, contrast) with contrast > 1 deepens shadows and adds punch
    dt_aligned_pixel_t print_contrast;
    dt_vector_pow1(print_linear, contrast, print_contrast);

    // Step 6: scale by exposure and preserve alpha
    for_each_channel(c)
      pix_out[c] = exposure * print_contrast[c];
    pix_out[3] = pix_in[3];
}

void process(dt_iop_module_t *const self, dt_dev_pixelpipe_iop_t *const piece,
             const void *const restrict ivoid, void *const restrict ovoid,
             const dt_iop_roi_t *const restrict roi_in, const dt_iop_roi_t *const restrict roi_out)
{
  const dt_iop_filminversion_data_t *const d = piece->data;
  assert(piece->colors == 4);

  const float *const restrict in = (float *)ivoid;
  float *const restrict out = (float *)ovoid;

  const float exposure = d->exposure;
  const float contrast = d->contrast;

  // Unpack per-channel vectors with alignment hints for auto-vectorization
  const float *const restrict Dmin = DT_IS_ALIGNED_PIXEL(d->Dmin);
  const float *const restrict wb_high = DT_IS_ALIGNED_PIXEL(d->wb_high);
  const float *const restrict offset = DT_IS_ALIGNED_PIXEL(d->offset);

  DT_OMP_FOR()
  for(size_t k = 0; k < (size_t)roi_out->height * roi_out->width * 4; k += 4)
  {
    const float *const restrict pix_in = in + k;
    float *const restrict pix_out = out + k;
    _process_pixel(pix_in, pix_out, Dmin, wb_high, offset, contrast, exposure);
  }
}


#ifdef HAVE_OPENCL
int process_cl(dt_iop_module_t *const self, dt_dev_pixelpipe_iop_t *const piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const restrict roi_in, const dt_iop_roi_t *const restrict roi_out)
{
  const dt_iop_filminversion_data_t *const d = piece->data;
  const dt_iop_filminversion_global_data_t *const gd = self->global_data;

  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;

  return dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_filminversion, width, height,
    CLARG(dev_in), CLARG(dev_out), CLARG(width), CLARG(height), CLARG(d->Dmin), CLARG(d->wb_high),
    CLARG(d->offset), CLARG(d->contrast), CLARG(d->exposure));
}
#endif


void init(dt_iop_module_t *self)
{
  dt_iop_default_init(self);

  dt_iop_filminversion_params_t *d = self->default_params;

  d->Dmin[0] = 1.00f;
  d->Dmin[1] = 0.45f;
  d->Dmin[2] = 0.25f;
  d->Dmin[3] = 1.00f;
}

void init_presets(dt_iop_module_so_t *self)
{
  dt_iop_filminversion_params_t tmp = (dt_iop_filminversion_params_t){ .film_stock = DT_FILMSTOCK_COLOR,
                                                                 .Dmin = { 1.13f, 0.49f, 0.27f, 0.0f},
                                                                 .wb_high = { 1.0f, 1.0f, 1.0f, 0.0f },
                                                                 .wb_low = { 1.0f, 1.0f, 1.0f, 0.0f },
                                                                 .D_max = 1.6f,
                                                                 .offset = -0.05f,
                                                                 .contrast = 3.0f,
                                                                 .exposure = 1.0f };


  dt_gui_presets_add_generic(_("color film"), self->op,
                             self->version(), &tmp, sizeof(tmp), TRUE, DEVELOP_BLEND_CS_RGB_SCENE);

  dt_iop_filminversion_params_t tmq = (dt_iop_filminversion_params_t){ .film_stock = DT_FILMSTOCK_NB,
                                                                 .Dmin = { 1.0f, 1.0f, 1.0f, 0.0f},
                                                                 .wb_high = { 1.0f, 1.0f, 1.0f, 0.0f },
                                                                 .wb_low = { 1.0f, 1.0f, 1.0f, 0.0f },
                                                                 .D_max = 2.2f,
                                                                 .offset = -0.05f,
                                                                 .contrast = 3.5f,
                                                                 .exposure = 1.0f };


  dt_gui_presets_add_generic(_("black and white film"), self->op,
                             self->version(), &tmq, sizeof(tmq), TRUE, DEVELOP_BLEND_CS_RGB_SCENE);
}

void init_global(dt_iop_module_so_t *self)
{
  dt_iop_filminversion_global_data_t *gd = malloc(sizeof(dt_iop_filminversion_global_data_t));

  self->data = gd;
  const int program = 41; // filminversion.cl, from programs.conf
  gd->kernel_filminversion = dt_opencl_create_kernel(program, "filminversion");
}

void cleanup_global(dt_iop_module_so_t *self)
{
  dt_iop_filminversion_global_data_t *gd = self->data;
  dt_opencl_free_kernel(gd->kernel_filminversion);
  free(self->data);
  self->data = NULL;
}

void init_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = g_malloc0(sizeof(dt_iop_filminversion_data_t));
}

void cleanup_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  g_free(piece->data);
  piece->data = NULL;
}


/* Global GUI stuff */

static void setup_color_variables(dt_iop_filminversion_gui_data_t *const g, const gint state)
{
  gtk_widget_set_visible(g->Dmin_G, state);
  gtk_widget_set_visible(g->Dmin_B, state);
}


static void toggle_stock_controls(dt_iop_module_t *const self)
{
  dt_iop_filminversion_gui_data_t *const g = self->gui_data;
  const dt_iop_filminversion_params_t *const p = self->params;

  if(p->film_stock == DT_FILMSTOCK_NB)
  {
    // Hide color controls
    setup_color_variables(g, FALSE);
    dt_bauhaus_widget_set_label(g->Dmin_R, NULL, N_("D min"));
  }
  else if(p->film_stock == DT_FILMSTOCK_COLOR)
  {
    // Show color controls
    setup_color_variables(g, TRUE);
    dt_bauhaus_widget_set_label(g->Dmin_R, NULL, N_("D min red component"));
  }
  else
  {
    // We shouldn't be there
    dt_print(DT_DEBUG_ALWAYS, "filminversion film stock: undefined behavior");
  }
}


static void Dmin_picker_update(dt_iop_module_t *self)
{
  dt_iop_filminversion_gui_data_t *const g = self->gui_data;
  const dt_iop_filminversion_params_t *const p = self->params;

  GdkRGBA color;
  color.alpha = 1.0f;

  if(p->film_stock == DT_FILMSTOCK_COLOR)
  {
    color.red = p->Dmin[0];
    color.green = p->Dmin[1];
    color.blue = p->Dmin[2];
  }
  else if(p->film_stock == DT_FILMSTOCK_NB)
  {
    color.red = color.green = color.blue = p->Dmin[0];
  }

  gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(g->Dmin_picker), &color);
}

static void Dmin_picker_callback(GtkColorButton *widget, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_filminversion_gui_data_t *g = self->gui_data;
  dt_iop_filminversion_params_t *p = self->params;

  dt_iop_color_picker_reset(self, TRUE);

  GdkRGBA c;
  gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(widget), &c);
  p->Dmin[0] = c.red;
  p->Dmin[1] = c.green;
  p->Dmin[2] = c.blue;

  ++darktable.gui->reset;
  dt_bauhaus_slider_set(g->Dmin_R, p->Dmin[0]);
  dt_bauhaus_slider_set(g->Dmin_G, p->Dmin[1]);
  dt_bauhaus_slider_set(g->Dmin_B, p->Dmin[2]);
  --darktable.gui->reset;

  Dmin_picker_update(self);
  dt_iop_color_picker_reset(self, TRUE);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void WB_low_picker_update(dt_iop_module_t *self)
{
  dt_iop_filminversion_gui_data_t *const g = self->gui_data;
  const dt_iop_filminversion_params_t *const p = self->params;

  GdkRGBA color;
  color.alpha = 1.0f;

  dt_aligned_pixel_t WB_low_invert;
  for(size_t c = 0; c < 3; ++c) WB_low_invert[c] = 2.0f - p->wb_low[c];
  const float WB_low_max = v_maxf(WB_low_invert);
  for(size_t c = 0; c < 3; ++c) WB_low_invert[c] /= WB_low_max;

  color.red = WB_low_invert[0];
  color.green = WB_low_invert[1];
  color.blue = WB_low_invert[2];

  gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(g->WB_low_picker), &color);
}

static void WB_low_picker_callback(GtkColorButton *widget, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_filminversion_gui_data_t *g = self->gui_data;
  dt_iop_filminversion_params_t *p = self->params;

  dt_iop_color_picker_reset(self, TRUE);

  GdkRGBA c;
  gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(widget), &c);

  dt_aligned_pixel_t RGB = { 2.0f - c.red, 2.0f - c.green, 2.0f - c.blue };

  float RGB_min = v_minf(RGB);
  for(size_t k = 0; k < 3; k++) p->wb_low[k] = RGB[k] / RGB_min;
  p->wb_low[3] = 1.0f;

  ++darktable.gui->reset;
  dt_bauhaus_slider_set(g->wb_low_R, p->wb_low[0]);
  dt_bauhaus_slider_set(g->wb_low_G, p->wb_low[1]);
  dt_bauhaus_slider_set(g->wb_low_B, p->wb_low[2]);
  --darktable.gui->reset;

  WB_low_picker_update(self);
  dt_iop_color_picker_reset(self, TRUE);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}


static void WB_high_picker_update(dt_iop_module_t *self)
{
  dt_iop_filminversion_gui_data_t *const g = self->gui_data;
  const dt_iop_filminversion_params_t *const p = self->params;

  GdkRGBA color;
  color.alpha = 1.0f;

  dt_aligned_pixel_t WB_high_invert;
  for(size_t c = 0; c < 3; ++c) WB_high_invert[c] = 2.0f - p->wb_high[c];
  const float WB_high_max = v_maxf(WB_high_invert);
  for(size_t c = 0; c < 3; ++c) WB_high_invert[c] /= WB_high_max;

  color.red = WB_high_invert[0];
  color.green = WB_high_invert[1];
  color.blue = WB_high_invert[2];

  gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(g->WB_high_picker), &color);
}

static void WB_high_picker_callback(GtkColorButton *widget, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_filminversion_gui_data_t *g = self->gui_data;
  dt_iop_filminversion_params_t *p = self->params;

  dt_iop_color_picker_reset(self, TRUE);

  GdkRGBA c;
  gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(widget), &c);

  dt_aligned_pixel_t RGB = { 2.0f - c.red, 2.0f - c.green, 2.0f - c.blue };
  float RGB_min = v_minf(RGB);
  for(size_t k = 0; k < 3; k++) p->wb_high[k] = RGB[k] / RGB_min;
  p->wb_high[3] = 1.0f;

  ++darktable.gui->reset;
  dt_bauhaus_slider_set(g->wb_high_R, p->wb_high[0]);
  dt_bauhaus_slider_set(g->wb_high_G, p->wb_high[1]);
  dt_bauhaus_slider_set(g->wb_high_B, p->wb_high[2]);
  --darktable.gui->reset;

  WB_high_picker_update(self);
  dt_iop_color_picker_reset(self, TRUE);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}


/* Color pickers auto-tuners */

// measure Dmin from the brightest (most transparent) part of the negative scan.
// The film base is the unexposed area, which transmits the most light and thus
// has the highest scanner values.  We use picked_color_max to target those bright
// areas, but reject near-white pixels (>= 0.98) that are likely dust or sensor
// clipping.  When the max is clipped we fall back to a conservative blend of
// max and mean so the estimate still trends toward the bright end.
static void apply_auto_Dmin(dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_filminversion_gui_data_t *g = self->gui_data;
  dt_iop_filminversion_params_t *p = self->params;

  const float dust_threshold = 0.98f; // above this, assume dust/clipping

  for(int k = 0; k < 3; k++)
  {
    const float vmax  = self->picked_color_max[k];
    const float vmean = self->picked_color[k];

    if(vmax < dust_threshold)
      // max is clean — use it directly as the film base estimate
      p->Dmin[k] = vmax;
    else
      // max is likely dust — blend 80% max / 20% mean for a safer estimate
      p->Dmin[k] = fminf(0.8f * vmax + 0.2f * vmean, dust_threshold);
  }
  p->Dmin[3] = 0.0f;

  ++darktable.gui->reset;
  dt_bauhaus_slider_set(g->Dmin_R, p->Dmin[0]);
  dt_bauhaus_slider_set(g->Dmin_G, p->Dmin[1]);
  dt_bauhaus_slider_set(g->Dmin_B, p->Dmin[2]);
  --darktable.gui->reset;

  Dmin_picker_update(self);
  dt_control_queue_redraw_widget(self->widget);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

// from Dmin, find out the range of density values of the film and compute Dmax
static void apply_auto_Dmax(dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_filminversion_gui_data_t *g = self->gui_data;
  dt_iop_filminversion_params_t *p = self->params;

  dt_aligned_pixel_t RGB;
  for(int c = 0; c < 3; c++)
  {
    RGB[c] = log10f(p->Dmin[c] / fmaxf(self->picked_color_min[c], THRESHOLD));
  }

  // Take the max(RGB) for safety. Big values unclip whites
  p->D_max = v_maxf(RGB);

  ++darktable.gui->reset;
  dt_bauhaus_slider_set(g->D_max, p->D_max);
  --darktable.gui->reset;

  dt_control_queue_redraw_widget(self->widget);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

// from Dmax, compute the offset so the range of density is rescaled between [0; 1]
static void apply_auto_offset(dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_filminversion_gui_data_t *g = self->gui_data;
  dt_iop_filminversion_params_t *p = self->params;

  dt_aligned_pixel_t RGB;
  for(int c = 0; c < 3; c++)
    RGB[c] = log10f(p->Dmin[c] / fmaxf(self->picked_color_max[c], THRESHOLD)) / p->D_max;

  // Take the min(RGB) for safety. Negative values unclip blacks
  p->offset = v_minf(RGB);

  ++darktable.gui->reset;
  dt_bauhaus_slider_set(g->offset, p->offset);
  --darktable.gui->reset;

  dt_control_queue_redraw_widget(self->widget);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

// from Dmax and offset, compute the white balance correction as multipliers of the offset
// such that offset × wb[c] make black monochrome
static void apply_auto_WB_low(dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_filminversion_gui_data_t *g = self->gui_data;
  dt_iop_filminversion_params_t *p = self->params;

  dt_aligned_pixel_t RGB_min;
  for(int c = 0; c < 3; c++)
    RGB_min[c] = log10f(p->Dmin[c] / fmaxf(self->picked_color[c], THRESHOLD)) / p->D_max;

  const float RGB_v_min = v_minf(RGB_min); // warning: can be negative
  for(int c = 0; c < 3; c++) p->wb_low[c] =  RGB_v_min / RGB_min[c];
  p->wb_low[3] = 1.0f;

  ++darktable.gui->reset;
  dt_bauhaus_slider_set(g->wb_low_R, p->wb_low[0]);
  dt_bauhaus_slider_set(g->wb_low_G, p->wb_low[1]);
  dt_bauhaus_slider_set(g->wb_low_B, p->wb_low[2]);
  --darktable.gui->reset;

  WB_low_picker_update(self);
  dt_control_queue_redraw_widget(self->widget);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

// from Dmax, offset and white balance multipliers, compute the white balance of the illuminant as multipliers of 1/Dmax
// such that WB[c] / Dmax make white monochrome
// Pixel math: corrected_de[c] = wb_high[c] * (log_density[c]/D_max + offset * wb_low[c])
// For white to be monochrome, corrected_de must be equal across channels.
// So wb_high[c] ∝ 1 / (log_density[c]/D_max + offset * wb_low[c])
static void apply_auto_WB_high(dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_filminversion_gui_data_t *g = self->gui_data;
  dt_iop_filminversion_params_t *p = self->params;

  dt_aligned_pixel_t correction;
  for(int c = 0; c < 3; c++)
  {
    const float log_density = -log10f(p->Dmin[c] / fmaxf(self->picked_color[c], THRESHOLD));
    correction[c] = fabsf(log_density / p->D_max + p->offset * p->wb_low[c]);
  }

  // wb_high[c] should be proportional to 1/correction[c], normalized so min = 1
  const float corr_max = v_maxf(correction); // max correction = min wb_high (which we normalize to 1)
  for(int c = 0; c < 3; c++) p->wb_high[c] = fmaxf(corr_max / fmaxf(correction[c], THRESHOLD), 0.25f);
  p->wb_high[3] = 1.0f;

  ++darktable.gui->reset;
  dt_bauhaus_slider_set(g->wb_high_R, p->wb_high[0]);
  dt_bauhaus_slider_set(g->wb_high_G, p->wb_high[1]);
  dt_bauhaus_slider_set(g->wb_high_B, p->wb_high[2]);
  --darktable.gui->reset;

  WB_high_picker_update(self);
  dt_control_queue_redraw_widget(self->widget);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

// from Dmax, offset, both white balances, and contrast, compute the output exposure
// such that white maps to approximately 0.96 in linear light
// Full pixel math: output = exposure * max(1 - 10^(contrast * corrected_de), 0)
// where corrected_de = wb_high[c]/D_max * log_density + wb_high[c] * offset * wb_low[c]
static void apply_auto_exposure(dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_filminversion_gui_data_t *g = self->gui_data;
  dt_iop_filminversion_params_t *p = self->params;

  dt_aligned_pixel_t RGB;
  for(int c = 0; c < 3; c++)
  {
    const float log_density = -log10f(p->Dmin[c] / fmaxf(self->picked_color_min[c], THRESHOLD));
    const float corrected_de = p->wb_high[c] / p->D_max * log_density + p->wb_high[c] * p->offset * p->wb_low[c];
    // linear_value = max(1 - 10^corrected_de, 0)^contrast
    const float linear = fmaxf(1.0f - fast_exp10f(corrected_de), THRESHOLD);
    const float contrasted = powf(linear, p->contrast);
    RGB[c] = 1.0f / fmaxf(contrasted, THRESHOLD);
  }
  // target: white at ~0.96 in linear
  p->exposure = 0.96f * v_minf(RGB);

  ++darktable.gui->reset;
  dt_bauhaus_slider_set(g->exposure, log2f(p->exposure));
  --darktable.gui->reset;

  dt_control_queue_redraw_widget(self->widget);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}


void color_picker_apply(dt_iop_module_t *self, GtkWidget *picker,
                        dt_dev_pixelpipe_t *pipe)
{
  if(darktable.gui->reset) return;
  dt_iop_filminversion_gui_data_t *g = self->gui_data;

  if     (picker == g->Dmin_sampler)
    apply_auto_Dmin(self);
  else if(picker == g->WB_high_sampler)
    apply_auto_WB_high(self);
  else if(picker == g->offset)
    apply_auto_offset(self);
  else if(picker == g->D_max)
    apply_auto_Dmax(self);
  else if(picker == g->WB_low_sampler)
    apply_auto_WB_low(self);
  else if(picker == g->exposure)
    apply_auto_exposure(self);
  else
    dt_print(DT_DEBUG_ALWAYS, "[filminversion] unknown color picker");
}

void gui_init(dt_iop_module_t *self)
{
  dt_iop_filminversion_gui_data_t *g = IOP_GUI_ALLOC(filminversion);

  static dt_action_def_t notebook_def = { };
  g->notebook = dt_ui_notebook_new(&notebook_def);
  dt_action_define_iop(self, NULL, N_("page"), GTK_WIDGET(g->notebook), &notebook_def);

  // Page FILM PROPERTIES
  GtkWidget *page1 = self->widget = dt_ui_notebook_page(g->notebook, N_("film properties"), NULL);

  // Dmin
  g->Dmin_picker = gtk_color_button_new();
  gtk_color_chooser_set_use_alpha(GTK_COLOR_CHOOSER(g->Dmin_picker), FALSE);
  gtk_color_button_set_title(GTK_COLOR_BUTTON(g->Dmin_picker), _("select color of film material from a swatch"));
  g_signal_connect(G_OBJECT(g->Dmin_picker), "color-set", G_CALLBACK(Dmin_picker_callback), self);

  g->Dmin_sampler = dt_color_picker_new(self, DT_COLOR_PICKER_AREA, NULL);
  gtk_widget_set_tooltip_text(g->Dmin_sampler , _("pick color of film material from image"));
  dt_action_define_iop(self, N_("pickers"), N_("film material"), g->Dmin_sampler, &dt_action_def_toggle);

  dt_gui_box_add(page1, dt_ui_section_label_new(C_("section", "color of the film base")),
                        dt_gui_hbox(dt_gui_expand(g->Dmin_picker), g->Dmin_sampler));

  g->Dmin_R = dt_bauhaus_slider_from_params(self, "Dmin[0]");
  dt_bauhaus_slider_set_digits(g->Dmin_R, 4);
  dt_bauhaus_slider_set_format(g->Dmin_R, "%");
  dt_bauhaus_slider_set_factor(g->Dmin_R, 100);
  dt_bauhaus_widget_set_label(g->Dmin_R, NULL, N_("D min red component"));
  gtk_widget_set_tooltip_text(g->Dmin_R, _("adjust the color and shade of the film transparent base.\n"
                                           "this value depends on the film material, \n"
                                           "the chemical fog produced while developing the film,\n"
                                           "and the scanner white balance."));

  g->Dmin_G = dt_bauhaus_slider_from_params(self, "Dmin[1]");
  dt_bauhaus_slider_set_digits(g->Dmin_G, 4);
  dt_bauhaus_slider_set_format(g->Dmin_G, "%");
  dt_bauhaus_slider_set_factor(g->Dmin_G, 100);
  dt_bauhaus_widget_set_label(g->Dmin_G, NULL, N_("D min green component"));
  gtk_widget_set_tooltip_text(g->Dmin_G, _("adjust the color and shade of the film transparent base.\n"
                                           "this value depends on the film material, \n"
                                           "the chemical fog produced while developing the film,\n"
                                           "and the scanner white balance."));

  g->Dmin_B = dt_bauhaus_slider_from_params(self, "Dmin[2]");
  dt_bauhaus_slider_set_digits(g->Dmin_B, 4);
  dt_bauhaus_slider_set_format(g->Dmin_B, "%");
  dt_bauhaus_slider_set_factor(g->Dmin_B, 100);
  dt_bauhaus_widget_set_label(g->Dmin_B, NULL, N_("D min blue component"));
  gtk_widget_set_tooltip_text(g->Dmin_B, _("adjust the color and shade of the film transparent base.\n"
                                           "this value depends on the film material, \n"
                                           "the chemical fog produced while developing the film,\n"
                                           "and the scanner white balance."));

  // D max and scanner bias
  dt_gui_box_add(page1, dt_ui_section_label_new(C_("section", "dynamic range of the film")));

  g->D_max = dt_color_picker_new(self, DT_COLOR_PICKER_AREA, dt_bauhaus_slider_from_params(self, "D_max"));
  dt_bauhaus_slider_set_format(g->D_max, " dB");
  gtk_widget_set_tooltip_text(g->D_max, _("maximum density of the film, corresponding to white after inversion.\n"
                                          "this value depends on the film specifications, the developing process,\n"
                                          "the dynamic range of the scene and the scanner exposure settings."));

  dt_gui_box_add(page1, dt_ui_section_label_new(C_("section", "scanner exposure settings")));

  g->offset = dt_color_picker_new(self, DT_COLOR_PICKER_AREA, dt_bauhaus_slider_from_params(self, "offset"));
  dt_bauhaus_slider_set_format(g->offset, " dB");
  gtk_widget_set_tooltip_text(g->offset, _("correct the exposure of the scanner, for all RGB channels,\n"
                                           "before the inversion, so blacks are neither clipped or too pale."));

  // Page CORRECTIONS
  GtkWidget *page2 = self->widget = dt_ui_notebook_page(g->notebook, N_("corrections"), NULL);

  // WB shadows
  g->WB_low_picker = gtk_color_button_new();
  gtk_color_chooser_set_use_alpha(GTK_COLOR_CHOOSER(g->WB_low_picker), FALSE);
  gtk_color_button_set_title(GTK_COLOR_BUTTON(g->WB_low_picker), _("select color of shadows from a swatch"));
  g_signal_connect(G_OBJECT(g->WB_low_picker), "color-set", G_CALLBACK(WB_low_picker_callback), self);

  g->WB_low_sampler = dt_color_picker_new(self, DT_COLOR_PICKER_AREA, NULL);
  gtk_widget_set_tooltip_text(g->WB_low_sampler, _("pick shadows color from image"));
  dt_action_define_iop(self, N_("pickers"), N_("shadows"), g->WB_low_sampler, &dt_action_def_toggle);

  dt_gui_box_add(page2, dt_ui_section_label_new(C_("section", "shadows color cast")),
                        dt_gui_hbox(dt_gui_expand(g->WB_low_picker), g->WB_low_sampler));

  g->wb_low_R = dt_bauhaus_slider_from_params(self, "wb_low[0]");
  dt_bauhaus_widget_set_label(g->wb_low_R, NULL, N_("shadows red offset"));
  gtk_widget_set_tooltip_text(g->wb_low_R, _("correct the color cast in shadows so blacks are\n"
                                             "truly achromatic. Setting this value before\n"
                                             "the highlights illuminant white balance will help\n"
                                             "recovering the global white balance in difficult cases."));

  g->wb_low_G = dt_bauhaus_slider_from_params(self, "wb_low[1]");
  dt_bauhaus_widget_set_label(g->wb_low_G, NULL, N_("shadows green offset"));
  gtk_widget_set_tooltip_text(g->wb_low_G, _("correct the color cast in shadows so blacks are\n"
                                             "truly achromatic. Setting this value before\n"
                                             "the highlights illuminant white balance will help\n"
                                             "recovering the global white balance in difficult cases."));

  g->wb_low_B = dt_bauhaus_slider_from_params(self, "wb_low[2]");
  dt_bauhaus_widget_set_label(g->wb_low_B, NULL, N_("shadows blue offset"));
  gtk_widget_set_tooltip_text(g->wb_low_B, _("correct the color cast in shadows so blacks are\n"
                                             "truly achromatic. Setting this value before\n"
                                             "the highlights illuminant white balance will help\n"
                                             "recovering the global white balance in difficult cases."));

  // WB highlights
  g->WB_high_picker = gtk_color_button_new();
  gtk_color_chooser_set_use_alpha(GTK_COLOR_CHOOSER(g->WB_high_picker), FALSE);
  gtk_color_button_set_title(GTK_COLOR_BUTTON(g->WB_high_picker), _("select color of illuminant from a swatch"));
  g_signal_connect(G_OBJECT(g->WB_high_picker), "color-set", G_CALLBACK(WB_high_picker_callback), self);

  g->WB_high_sampler = dt_color_picker_new(self, DT_COLOR_PICKER_AREA, NULL);
  gtk_widget_set_tooltip_text(g->WB_high_sampler , _("pick illuminant color from image"));
  dt_action_define_iop(self, N_("pickers"), N_("illuminant"), g->WB_high_sampler, &dt_action_def_toggle);

  dt_gui_box_add(page2, dt_ui_section_label_new(C_("section", "highlights white balance")),
                        dt_gui_hbox(dt_gui_expand(g->WB_high_picker), g->WB_high_sampler));

  g->wb_high_R = dt_bauhaus_slider_from_params(self, "wb_high[0]");
  dt_bauhaus_widget_set_label(g->wb_high_R, NULL, N_("illuminant red gain"));
  gtk_widget_set_tooltip_text(g->wb_high_R, _("correct the color of the illuminant so whites are\n"
                                              "truly achromatic. Setting this value after\n"
                                              "the shadows color cast will help\n"
                                              "recovering the global white balance in difficult cases."));

  g->wb_high_G = dt_bauhaus_slider_from_params(self, "wb_high[1]");
  dt_bauhaus_widget_set_label(g->wb_high_G, NULL, N_("illuminant green gain"));
  gtk_widget_set_tooltip_text(g->wb_high_G, _("correct the color of the illuminant so whites are\n"
                                              "truly achromatic. Setting this value after\n"
                                              "the shadows color cast will help\n"
                                              "recovering the global white balance in difficult cases."));

  g->wb_high_B = dt_bauhaus_slider_from_params(self, "wb_high[2]");
  dt_bauhaus_widget_set_label(g->wb_high_B, NULL, N_("illuminant blue gain"));
  gtk_widget_set_tooltip_text(g->wb_high_B, _("correct the color of the illuminant so whites are\n"
                                              "truly achromatic. Setting this value after\n"
                                              "the shadows color cast will help\n"
                                              "recovering the global white balance in difficult cases."));

  // Page OUTPUT
  GtkWidget *page3 = self->widget = dt_ui_notebook_page(g->notebook, N_("output"), NULL);

  dt_gui_box_add(page3, dt_ui_section_label_new(C_("section", "output tone control")));

  g->contrast = dt_bauhaus_slider_from_params(self, "contrast");
  dt_bauhaus_slider_set_soft_min(g->contrast, 1.0);
  dt_bauhaus_slider_set_soft_max(g->contrast, 5.0);
  dt_bauhaus_slider_set_digits(g->contrast, 2);
  gtk_widget_set_tooltip_text(g->contrast, _("controls the steepness of the density-to-linear conversion.\n"
                                             "higher values produce more contrast and tonal separation.\n"
                                             "this acts as a slope multiplier on the film characteristic curve\n"
                                             "in log space before linearization."));

  g->exposure = dt_color_picker_new(self, DT_COLOR_PICKER_AREA, dt_bauhaus_slider_from_params(self, "exposure"));
  dt_bauhaus_slider_set_hard_min(g->exposure, -3.0);
  dt_bauhaus_slider_set_soft_min(g->exposure, -1.0);
  dt_bauhaus_slider_set_hard_max(g->exposure, 3.0);
  dt_bauhaus_slider_set_soft_max(g->exposure, 3.0);
  dt_bauhaus_slider_set_format(g->exposure, _(" EV"));
  gtk_widget_set_tooltip_text(g->exposure, _("adjust the output exposure of the inverted image.\n"
                                             "this scales the linear scene-referred output\n"
                                             "to set the overall brightness before tone mapping."));

  // start building top level widget
  self->widget = dt_gui_vbox();

  // Film emulsion
  g->film_stock = dt_bauhaus_combobox_from_params(self, "film_stock");
  gtk_widget_set_tooltip_text(g->film_stock, _("toggle on or off the color controls"));

  dt_gui_box_add(self->widget, g->notebook);
}


void gui_changed(dt_iop_module_t *self, GtkWidget *w, void *previous)
{
  dt_iop_filminversion_params_t *p = self->params;
  dt_iop_filminversion_gui_data_t *g = self->gui_data;
  if(!w || w == g->film_stock)
  {
    toggle_stock_controls(self);
    Dmin_picker_update(self);
  }
  else if(w == g->Dmin_R && p->film_stock == DT_FILMSTOCK_NB)
  {
    dt_bauhaus_slider_set(g->Dmin_G, p->Dmin[0]);
    dt_bauhaus_slider_set(g->Dmin_B, p->Dmin[0]);
  }
  else if(w == g->Dmin_R || w == g->Dmin_G || w == g->Dmin_B)
  {
    Dmin_picker_update(self);
  }
  else if(w == g->exposure)
  {
    p->exposure = powf(2.0f, p->exposure);
  }

  if(!w || w == g->wb_high_R || w == g->wb_high_G || w == g->wb_high_B)
  {
    WB_high_picker_update(self);
  }

  if(!w || w == g->wb_low_R || w == g->wb_low_G || w == g->wb_low_B)
  {
    WB_low_picker_update(self);
  }
}


void gui_update(dt_iop_module_t *const self)
{
  // let gui slider match current parameters:
  dt_iop_filminversion_gui_data_t *const g = self->gui_data;
  const dt_iop_filminversion_params_t *const p = self->params;

  dt_iop_color_picker_reset(self, TRUE);


  dt_bauhaus_slider_set(g->exposure, log2f(p->exposure));     // warning: GUI is in EV
  dt_bauhaus_slider_set_default(g->exposure, log2f(p->exposure)); // otherwise always showes as "changed"

  // Update custom stuff
  gui_changed(self, NULL, NULL);
}

void gui_reset(dt_iop_module_t *self)
{
  dt_iop_color_picker_reset(self, TRUE);
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
