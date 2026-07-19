/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

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
// our includes go first:
#include "bauhaus/bauhaus.h"
#include "common/chromatic_adaptation.h"
#include "common/color_picker.h"
#include "common/darktable_ucs_22_helpers.h"
#include "common/dtpthread.h"
#include "common/gamut_mapping.h"
#include "common/imagebuf.h"
#include "common/math.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "develop/openmp_maths.h"
#include "dtgtk/drawingarea.h"
#include "gui/color_picker_proxy.h"
#include "gui/draw.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "iop/iop_api.h"

#define DT_IOP_SATCURVE_MAXNODES 20
#define DT_IOP_SATCURVE_RES 256
#define DT_IOP_SATCURVE_HIST_RES 256
#define DT_IOP_SATCURVE_INSET DT_PIXEL_APPLY_DPI(5)
// thickness of, and gap to, the decorative perceptual gradient bar
// drawn below the curve graph
#define DT_IOP_SATCURVE_GRADIENT_SIZE DT_PIXEL_APPLY_DPI(8)
#define DT_IOP_SATCURVE_GRADIENT_GAP DT_PIXEL_APPLY_DPI(2)
#define DT_IOP_SATCURVE_MIN_X_DISTANCE 0.0025f
#define DT_IOP_SATCURVE_GAMUT_STEPS 92

// version of the module's serialized parameters
DT_MODULE_INTROSPECTION(1, dt_iop_satcurve_params_t)

// saturation definition used to compute S_in_norm and drive the curve
typedef enum dt_iop_satcurve_formula_t
{
  DT_IOP_SATCURVE_JZAZBZ = 0,
  DT_IOP_SATCURVE_DTUCS = 1
} dt_iop_satcurve_formula_t;

typedef struct dt_iop_satcurve_node_t
{
  float x, y;
} dt_iop_satcurve_node_t;

// module parameters, serialized to the database
typedef struct dt_iop_satcurve_params_t
{
  dt_iop_satcurve_node_t curve[DT_IOP_SATCURVE_MAXNODES];
  int curve_num_nodes;
  int curve_type;
  dt_iop_satcurve_formula_t formula; // $DEFAULT: 1 $DESCRIPTION: "saturation formula"
} dt_iop_satcurve_params_t;

// per-pixelpipe runtime data, derived from params_t in commit_params()
typedef struct dt_iop_satcurve_data_t
{
  dt_draw_curve_t *curve;
  int curve_num_nodes;
  int curve_type;
  dt_iop_satcurve_formula_t formula;
  float *lut;
  float *gamut_lut;
  gboolean lut_inited;
  const struct dt_iop_order_iccprofile_info_t *work_profile;
} dt_iop_satcurve_data_t;

// GUI state and widget handles
typedef struct dt_iop_satcurve_gui_data_t
{
  GtkDrawingArea *area;
  GtkWidget *colorpicker;
  GtkWidget *formula;
  dt_draw_curve_t *curve;
  int curve_num_nodes;
  int curve_type;
  float draw_ys[DT_IOP_SATCURVE_RES];
  int selected;
  gboolean dragging;
  float mouse_x, mouse_y;

  // input saturation histogram (S_in_norm)
  float histogram[DT_IOP_SATCURVE_HIST_RES];
  float histogram_max;
  dt_pthread_mutex_t histogram_lock;

  // picked region: S_in_norm of mean/min/max
  gboolean picker_valid;
  float picked_s;
  float picked_s_min;
  float picked_s_max;
} dt_iop_satcurve_gui_data_t;

typedef struct dt_iop_satcurve_global_data_t
{
  int kernel_satcurvergb;
} dt_iop_satcurve_global_data_t;

const char *name()
{
  return _("saturation curve");
}

const char *aliases()
{
  return _("sat vs sat|saturation versus saturation|satcurve");
}

const char **description(dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("remap saturation with a curve"),
                                 _("corrective or creative"),
                                 _("linear, RGB, scene-referred"),
                                 _("linear, RGB, scene-referred"),
                                 _("linear, RGB, scene-referred"));
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING;
}

int default_group()
{
  return IOP_GROUP_COLOR | IOP_GROUP_GRADING;
}

dt_iop_colorspace_type_t default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe,
                                             dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

// intended to run before filmic rgb; pipeline order entry is added
// separately in iop_order.c

static inline void reset_curve(dt_iop_satcurve_params_t *p)
{
  p->curve_num_nodes = 2;
  p->curve_type = MONOTONE_HERMITE;
  p->formula = DT_IOP_SATCURVE_DTUCS;
  p->curve[0] = (dt_iop_satcurve_node_t){0.f, .5f};
  p->curve[1] = (dt_iop_satcurve_node_t){1.f, .5f};
}

static inline float lookup_lut(const float *lut, const float x)
{
  const float position = CLAMP(x, 0.f, 1.f) * (DT_IOP_SATCURVE_RES - 1);
  const int i = MIN((int)position, DT_IOP_SATCURVE_RES - 2);
  return lut[i] + (position - i) * (lut[i + 1] - lut[i]);
}

// periodic lookup in the hue-indexed gamut LUT; h = atan2(b, a), in [-pi, pi]
static inline float satcurve_lookup_gamut(const float *const gamut_lut, const float h)
{
  const float position = (h + M_PI_F) * (float)LUT_ELEM / DT_2PI_F;
  const int bin0 = ((int)floorf(position)) % LUT_ELEM;
  const int bin1 = (bin0 + 1) % LUT_ELEM;
  const float f = position - floorf(position);

  return gamut_lut[bin0] * (1.f - f) + gamut_lut[bin1] * f;
}

// smoothly map x from knee onwards towards maximum (reached asymptotically);
// x <= knee is left unchanged
static inline float satcurve_soft_clip(const float x, const float knee, const float maximum)
{
  if(x <= knee)
    return x;

  const float range = maximum - knee;
  if(range <= 0.f)
    return maximum;

  return knee + range * (1.f - expf(-(x - knee) / range));
}

// mirrors the JzAzBz chroma clip in color balance rgb (test conversion to
// L'M'S'): keeps the curve from producing colors that go negative on the
// JzAzBz -> XYZ back-transform
static inline float clip_jz_chroma(const float Jz, const float Cz, const float ch, const float sh)
{
  const float d0 = 1.6295499532821566e-11f;
  const float dd = -0.56f;
  float Iz = (Jz + d0) / (1.f + dd - dd * (Jz + d0));
  Iz = MAX(Iz, 0.f);
  static const dt_colormatrix_t AI_trans = {{1.f, 1.f, 1.f, 0.f},
                                             {.1386050432715393f, -.1386050432715393f, -.0960192420263190f, 0.f},
                                             {.0580473161561189f, -.0580473161561189f, -.8118918960560390f, 0.f}};
  dt_aligned_pixel_t izab = {Iz, Cz * ch, Cz * sh, 0.f}, lms;
  dt_apply_transposed_color_matrix(izab, AI_trans, lms);
  float max_c = Cz;
  if(lms[0] < 0.f)
    max_c = MIN(max_c, -Iz / (AI_trans[1][0] * ch + AI_trans[2][0] * sh));
  if(lms[1] < 0.f)
    max_c = MIN(max_c, -Iz / (AI_trans[1][1] * ch + AI_trans[2][1] * sh));
  if(lms[2] < 0.f)
    max_c = MIN(max_c, -Iz / (AI_trans[1][2] * ch + AI_trans[2][2] * sh));
  return MAX(max_c, 0.f);
}

// shared S_in_norm computation, used by process(), the histogram, and the
// picker, so all three use the exact same scene-referred math
static inline float pixel_s_in_norm_jzazbz(const float *const restrict rgb_in,
                                     const dt_colormatrix_t inputmatrix_trans,
                                     const float *const restrict gamut_lut,
                                     float *const restrict h_out)
{
  dt_aligned_pixel_t rgb, xyz, jab;
  copy_pixel(rgb, rgb_in);
  dt_vector_clipneg(rgb);
  dt_apply_transposed_color_matrix(rgb, inputmatrix_trans, xyz);
  dt_XYZ_2_JzAzBz(xyz, jab);

  const float Jz = MAX(jab[0], 0.f);
  const float Cz = dt_fast_hypotf(jab[1], jab[2]);
  const float h = atan2f(jab[2], jab[1]);
  const float gamut = MAX(satcurve_lookup_gamut(gamut_lut, h), FLT_MIN);
  const float s_in = Jz > 0.f ? Cz / Jz : 0.f;

  if(h_out) *h_out = h;
  return s_in / gamut;
}

static inline float pixel_s_in_norm_ucs(
    const float *const restrict rgb_in,
    const dt_colormatrix_t inputmatrix_trans,
    const float *const restrict gamut_lut,
    const float L_white,
    float *const restrict h_out)
{
  dt_aligned_pixel_t rgb, xyz, xyY, JCH, HCB;
  dt_aligned_pixel_t JCH_gamut_boundary, HSB_gamut_boundary;

  copy_pixel(rgb, rgb_in);
  dt_vector_clipneg(rgb);
  dt_apply_transposed_color_matrix(rgb, inputmatrix_trans, xyz);

  dt_D65_XYZ_to_xyY(xyz, xyY);
  xyY_to_dt_UCS_JCH(xyY, L_white, JCH);
  dt_UCS_JCH_to_HCB(JCH, HCB);

  const float max_colorfulness
      = MAX(satcurve_lookup_gamut(gamut_lut, JCH[2]), FLT_MIN);

  const float max_chroma
      = 15.932993652962535f
        * powf(JCH[0] / L_white, 0.6523997524738018f)
        * powf(max_colorfulness, 0.6007557017508491f)
        / L_white;

  JCH_gamut_boundary[0] = JCH[0];
  JCH_gamut_boundary[1] = max_chroma;
  JCH_gamut_boundary[2] = JCH[2];
  JCH_gamut_boundary[3] = 0.f;

  dt_UCS_JCH_to_HSB(JCH_gamut_boundary, HSB_gamut_boundary);

  if(h_out) *h_out = JCH[2];

  const float saturation = HCB[2] > 0.f ? HCB[1] / HCB[2] : 0.f;
  return saturation / MAX(HSB_gamut_boundary[1], FLT_MIN);
}

static inline float pixel_s_in_norm(
    const dt_iop_satcurve_data_t *const d,
    const float *const restrict rgb_in,
    const dt_colormatrix_t inputmatrix_trans,
    const float *const restrict gamut_lut,
    const float L_white,
    float *const restrict h_out)
{
  if(d->formula == DT_IOP_SATCURVE_DTUCS)
    return pixel_s_in_norm_ucs(rgb_in, inputmatrix_trans, gamut_lut, L_white, h_out);

  return pixel_s_in_norm_jzazbz(rgb_in, inputmatrix_trans, gamut_lut, h_out);
}

// updates the input-referred saturation histogram; GUI full-preview only,
// mirrors the display_mask gating used in colorzones.c
static void _update_sat_histogram(dt_iop_module_t *self,
                                   const dt_iop_satcurve_data_t *d,
                                   const dt_colormatrix_t inputmatrix_trans,
                                   const float *const restrict in,
                                   const size_t npixels)
{
  dt_iop_satcurve_gui_data_t *g = self->gui_data;
  if(!g) return;

  float local_hist[DT_IOP_SATCURVE_HIST_RES] = { 0.f };
  const float L_white = Y_to_dt_UCS_L_star(1.f); // constant; compute once, not per pixel

  DT_OMP_FOR(reduction(+ : local_hist[:DT_IOP_SATCURVE_HIST_RES]))
  for(size_t k = 0; k < npixels; k++)
  {
    const float s_in_norm = pixel_s_in_norm(d, in + 4 * k, inputmatrix_trans, d->gamut_lut, L_white, NULL);
    const int bin = CLAMP((int)(s_in_norm * (DT_IOP_SATCURVE_HIST_RES - 1)),
                           0, DT_IOP_SATCURVE_HIST_RES - 1);
    local_hist[bin] += 1.f;
  }

  // log1p scaling: S_in_norm is typically skewed heavily toward low values;
  // the x-axis (S_in_norm) itself stays linear
  float max_val = 0.f;
  for(int i = 0; i < DT_IOP_SATCURVE_HIST_RES; i++)
  {
    local_hist[i] = log1pf(local_hist[i]);
    max_val = MAX(max_val, local_hist[i]);
  }

  dt_pthread_mutex_lock(&g->histogram_lock);
  memcpy(g->histogram, local_hist, sizeof(local_hist));
  g->histogram_max = MAX(max_val, 1e-6f);
  dt_pthread_mutex_unlock(&g->histogram_lock);

  dt_control_queue_redraw_widget(GTK_WIDGET(g->area));
}

void process(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece,
             const void *const ivoid, void *const ovoid,
             const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_satcurve_data_t *d = piece->data;
  const gboolean neutral_curve = d->curve_num_nodes == 2
                              && fabsf(d->lut[0] - .5f) < 1e-6f
                              && fabsf(d->lut[DT_IOP_SATCURVE_RES - 1] - .5f) < 1e-6f;
  if(neutral_curve)
  {
    dt_iop_image_copy_by_size(ovoid, ivoid, roi_out->width, roi_out->height, piece->colors);
    return;
  }

  const dt_iop_order_iccprofile_info_t *work_profile = dt_ioppr_get_pipe_current_profile_info(self, piece->pipe);
  if(!work_profile || piece->colors != 4)
  {
    dt_iop_image_copy_by_size(ovoid, ivoid, roi_out->width, roi_out->height, piece->colors);
    return;
  }

  dt_colormatrix_t inputmatrix = {{ 0.0f }};
  dt_colormatrix_t outputmatrix = {{ 0.0f }};
  dt_colormatrix_t inputmatrix_trans;
  dt_colormatrix_t outputmatrix_trans;
  dt_colormatrix_mul(inputmatrix, XYZ_D50_to_D65_CAT16, work_profile->matrix_in);
  dt_colormatrix_transpose(inputmatrix_trans, inputmatrix);
  dt_colormatrix_mul(outputmatrix, work_profile->matrix_out, XYZ_D65_to_D50_CAT16);
  dt_colormatrix_transpose(outputmatrix_trans, outputmatrix);

  const float *const restrict in = DT_IS_ALIGNED((const float *)ivoid);
  float *const restrict out = DT_IS_ALIGNED((float *)ovoid);
  const size_t npixels = (size_t)roi_out->width * roi_out->height;

  if(self->dev->gui_attached && dt_pipe_is_full(piece->pipe) && dt_iop_has_focus(self)
     && piece->pipe == self->dev->full.pipe)
    _update_sat_histogram(self, d, inputmatrix_trans, in, npixels);

  // constant regardless of pixel data; hoisted out of the per-pixel loop
  const float L_white = Y_to_dt_UCS_L_star(1.f);

  DT_OMP_FOR()
  for(size_t k = 0; k < npixels; k++)
  {
    dt_aligned_pixel_t rgb, xyz, pixout;
    copy_pixel(rgb, in + 4 * k);
    dt_vector_clipneg(rgb);
    dt_apply_transposed_color_matrix(rgb, inputmatrix_trans, xyz);

    if(d->formula == DT_IOP_SATCURVE_DTUCS)
    {
      dt_aligned_pixel_t xyY, JCH, HCB, HSB, JCH_gamut_boundary, HSB_gamut_boundary;
      dt_D65_XYZ_to_xyY(xyz, xyY);
      xyY_to_dt_UCS_JCH(xyY, L_white, JCH);
      dt_UCS_JCH_to_HCB(JCH, HCB);

      const float max_colorfulness = MAX(satcurve_lookup_gamut(d->gamut_lut, JCH[2]), FLT_MIN);
      const float max_chroma = 15.932993652962535f
                       * powf(JCH[0] / L_white, 0.6523997524738018f)
                       * powf(max_colorfulness, 0.6007557017508491f)
                       / L_white;
      JCH_gamut_boundary[0] = JCH[0];
      JCH_gamut_boundary[1] = max_chroma;
      JCH_gamut_boundary[2] = JCH[2];
      JCH_gamut_boundary[3] = 0.f;
      dt_UCS_JCH_to_HSB(JCH_gamut_boundary, HSB_gamut_boundary);

      HSB[0] = HCB[0];
      HSB[1] = HCB[2] > 0.f ? HCB[1] / HCB[2] : 0.f;
      HSB[2] = HCB[2];
      HSB[3] = 0.f;

      const float gamut_s = MAX(HSB_gamut_boundary[1], FLT_MIN);
      const float s_in_norm = HSB[1] / gamut_s;
      const float c = CLAMP(lookup_lut(d->lut, s_in_norm), 0.f, 1.f);
      HSB[1] = MAX(HSB[1] * (2.f * c), 0.f);
      HSB[1] = satcurve_soft_clip(HSB[1], .8f * gamut_s, gamut_s);

      dt_UCS_HSB_to_JCH(HSB, JCH);
      dt_UCS_JCH_to_xyY(JCH, L_white, xyY);
      dt_xyY_to_XYZ(xyY, xyz);
    }
    else
    {
      dt_aligned_pixel_t jab;
      dt_XYZ_2_JzAzBz(xyz, jab);
      const float Jz = MAX(jab[0], 0.f);
      const float Cz = dt_fast_hypotf(jab[1], jab[2]);
      const float h = atan2f(jab[2], jab[1]);
      const float ch = cosf(h), sh = sinf(h);
      const float gamut = MAX(satcurve_lookup_gamut(d->gamut_lut, h), FLT_MIN);
      const float s_in_norm = (Jz > 0.f ? Cz / Jz : 0.f) / gamut;
      const float c = CLAMP(lookup_lut(d->lut, s_in_norm), 0.f, 1.f);
      const float s_out = satcurve_soft_clip(MAX(s_in_norm * (2.f * c), 0.f), .8f, 1.f) * gamut;
      const float r = dt_fast_hypotf(Jz, Cz);
      // T = atanf(s_out); cosf(T) = 1/sqrt(1+s_out^2), sinf(T) = s_out/sqrt(1+s_out^2)
      // avoids one atanf + one cosf + one sinf per pixel
      const float inv_norm = 1.f / sqrtf(1.f + s_out * s_out);
      const float Jz_out = r * inv_norm;
      const float Cz_out = clip_jz_chroma(Jz_out, r * s_out * inv_norm, ch, sh);
      jab[0] = Jz_out;
      jab[1] = Cz_out * ch;
      jab[2] = Cz_out * sh;
      dt_JzAzBz_2_XYZ(jab, xyz);
    }

    dt_apply_transposed_color_matrix(xyz, outputmatrix_trans, pixout);
    dt_vector_clipneg(pixout);
    copy_pixel_nontemporal(out + 4 * k, pixout);
  }
  dt_omploop_sfence();
}

#if HAVE_OPENCL
int process_cl(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece,
               cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_satcurve_data_t *d = piece->data;
  const dt_iop_satcurve_global_data_t *gd = self->global_data;

  cl_int err = DT_OPENCL_DEFAULT_ERROR;
  if(piece->colors != 4) return err;

  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;

  const gboolean neutral_curve = d->curve_num_nodes == 2
      && fabsf(d->lut[0] - .5f) < 1e-6f
      && fabsf(d->lut[DT_IOP_SATCURVE_RES - 1] - .5f) < 1e-6f;
  if(neutral_curve)
    return dt_opencl_enqueue_copy_image(devid, dev_in, dev_out, (size_t[]){0,0,0},
                                         (size_t[]){0,0,0}, (size_t[]){width, height, 1});

  const dt_iop_order_iccprofile_info_t *const work_profile
      = dt_ioppr_get_pipe_current_profile_info(self, piece->pipe);
  if(work_profile == NULL) return err;

   // Histogram needs a host-side copy of the input buffer; only do this
  // when the GUI actually needs it, to avoid a needless GPU->CPU sync
  // on every full-pipe run.
  if(self->dev->gui_attached && dt_pipe_is_full(piece->pipe) && dt_iop_has_focus(self)
     && piece->pipe == self->dev->full.pipe)
  {
    dt_colormatrix_t hist_input_matrix;
    dt_colormatrix_t hist_input_matrix_trans;
    dt_colormatrix_mul(hist_input_matrix, XYZ_D50_to_D65_CAT16, work_profile->matrix_in);
    dt_colormatrix_transpose(hist_input_matrix_trans, hist_input_matrix);

    float *host_in = dt_alloc_align_float((size_t)4 * width * height);
    if(host_in)
    {
      cl_int hist_err = dt_opencl_copy_image_to_host(devid, host_in, dev_in, width, height, sizeof(float) * 4);
      if(hist_err == CL_SUCCESS)
        _update_sat_histogram(self, d, hist_input_matrix_trans, host_in, (size_t)width * height);
      dt_free_align(host_in);
    }
  }
  
  cl_mem lut_cl = NULL;
  cl_mem gamut_lut_cl = NULL;
  cl_mem input_matrix_cl = NULL;
  cl_mem output_matrix_cl = NULL;

  dt_colormatrix_t input_matrix;
  dt_colormatrix_t output_matrix;
  dt_colormatrix_mul(input_matrix, XYZ_D50_to_D65_CAT16, work_profile->matrix_in);
  dt_colormatrix_mul(output_matrix, work_profile->matrix_out, XYZ_D65_to_D50_CAT16);

  input_matrix_cl  = dt_opencl_copy_host_to_device_constant(devid, 12 * sizeof(float), input_matrix);
  output_matrix_cl = dt_opencl_copy_host_to_device_constant(devid, 12 * sizeof(float), output_matrix);
  lut_cl       = dt_opencl_copy_host_to_device_constant(devid, DT_IOP_SATCURVE_RES * sizeof(float), d->lut);
  gamut_lut_cl = dt_opencl_copy_host_to_device_constant(devid, LUT_ELEM * sizeof(float), d->gamut_lut);

  if(input_matrix_cl == NULL || output_matrix_cl == NULL || lut_cl == NULL || gamut_lut_cl == NULL)
  {
    err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
    goto error;
  }

  // constant regardless of pixel data; compute once on the host instead of
  // once per GPU thread inside the kernel
  const float L_white = Y_to_dt_UCS_L_star(1.f);

  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_satcurvergb, width, height,
      CLARG(dev_in), CLARG(dev_out), CLARG(width), CLARG(height),
      CLARG(input_matrix_cl), CLARG(output_matrix_cl),
      CLARG(lut_cl), CLARG(gamut_lut_cl), CLARG(d->formula), CLARG(L_white));

error:
  dt_opencl_release_mem_object(input_matrix_cl);
  dt_opencl_release_mem_object(output_matrix_cl);
  dt_opencl_release_mem_object(lut_cl);
  dt_opencl_release_mem_object(gamut_lut_cl);
  return err;
}

void init_global(dt_iop_module_so_t *self)
{
  const int program = 42; // satcurve.cl in programs.conf
  dt_iop_satcurve_global_data_t *gd = malloc(sizeof(dt_iop_satcurve_global_data_t));
  self->data = gd;
  gd->kernel_satcurvergb = dt_opencl_create_kernel(program, "satcurvergb");
}

void cleanup_global(dt_iop_module_so_t *self)
{
  const dt_iop_satcurve_global_data_t *gd = self->data;
  dt_opencl_free_kernel(gd->kernel_satcurvergb);
  free(self->data);
  self->data = NULL;
}
#endif


void init_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_satcurve_data_t *d = dt_calloc_align_type(dt_iop_satcurve_data_t, 1);
  d->lut = dt_alloc_align_float(DT_IOP_SATCURVE_RES);
  d->gamut_lut = dt_alloc_align_float(LUT_ELEM);
  d->curve = dt_draw_curve_new(0.f, 1.f, MONOTONE_HERMITE);
  d->lut_inited = FALSE;
  piece->data = d;
}

void cleanup_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_satcurve_data_t *d = piece->data;
  dt_draw_curve_destroy(d->curve);
  dt_free_align(d->lut);
  dt_free_align(d->gamut_lut);
  dt_free_align(d);
  piece->data = NULL;
}

// RGB/D50 -> XYZ/D65, the input expected by dt_XYZ_2_JzAzBz()
static void build_jzazbz_gamut_lut(dt_iop_satcurve_data_t *d, const dt_iop_order_iccprofile_info_t *work_profile)
{
  dt_colormatrix_t inputmatrix = { {0.0f} };

  dt_colormatrix_mul(inputmatrix, XYZ_D50_to_D65_CAT16, work_profile->matrix_in);
  dt_colormatrix_t inputmatrix_trans;
  dt_colormatrix_transpose(inputmatrix_trans, inputmatrix);
  float *sampler = dt_calloc_align_float(LUT_ELEM);

  DT_OMP_FOR(reduction(max : sampler[:LUT_ELEM]) collapse(3))
  for(int r = 0; r < DT_IOP_SATCURVE_GAMUT_STEPS; r++)
    for(int g = 0; g < DT_IOP_SATCURVE_GAMUT_STEPS; g++)
      for(int b = 0; b < DT_IOP_SATCURVE_GAMUT_STEPS; b++)
      {
        const dt_aligned_pixel_t rgb = {r / (float)(DT_IOP_SATCURVE_GAMUT_STEPS - 1),
                                         g / (float)(DT_IOP_SATCURVE_GAMUT_STEPS - 1),
                                         b / (float)(DT_IOP_SATCURVE_GAMUT_STEPS - 1), 0.f};
        dt_aligned_pixel_t xyz, jab;
        dt_apply_transposed_color_matrix(rgb, inputmatrix_trans, xyz);
        dt_XYZ_2_JzAzBz(xyz, jab);
        const float saturation = jab[0] > 0.f ? dt_fast_hypotf(jab[1], jab[2]) / jab[0] : 0.f;
        int index = roundf((LUT_ELEM - 1) * (atan2f(jab[2], jab[1]) + M_PI_F) / DT_2PI_F);
        index = index < 0 ? LUT_ELEM - 1 : (index >= LUT_ELEM ? 0 : index);
        sampler[index] = MAX(sampler[index], saturation);
      }

  for(size_t i = 2; i < LUT_ELEM - 2; i++)
    d->gamut_lut[i] = (sampler[i - 2] + sampler[i - 1] + sampler[i] + sampler[i + 1] + sampler[i + 2]) / 5.f;
  d->gamut_lut[0] = (sampler[LUT_ELEM - 2] + sampler[LUT_ELEM - 1] + sampler[0] + sampler[1] + sampler[2]) / 5.f;
  d->gamut_lut[1] = (sampler[LUT_ELEM - 1] + sampler[0] + sampler[1] + sampler[2] + sampler[3]) / 5.f;
  d->gamut_lut[LUT_ELEM - 2] = (sampler[LUT_ELEM - 4] + sampler[LUT_ELEM - 3] + sampler[LUT_ELEM - 2] + sampler[LUT_ELEM - 1] + sampler[0]) / 5.f;
  d->gamut_lut[LUT_ELEM - 1] = (sampler[LUT_ELEM - 3] + sampler[LUT_ELEM - 2] + sampler[LUT_ELEM - 1] + sampler[0] + sampler[1]) / 5.f;
  dt_free_align(sampler);
}

static void build_gamut_lut(dt_iop_satcurve_data_t *d,
                            const dt_iop_order_iccprofile_info_t *work_profile)
{
  if(d->formula == DT_IOP_SATCURVE_DTUCS)
  {
    dt_colormatrix_t inputmatrix = {{ 0.0f }};
    dt_colormatrix_mul(inputmatrix, XYZ_D50_to_D65_CAT16, work_profile->matrix_in);
    dt_UCS_22_build_gamut_LUT(inputmatrix, d->gamut_lut);
  }
  else
    build_jzazbz_gamut_lut(d, work_profile);
}

void commit_params(dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                    dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_satcurve_data_t *d = piece->data;
  const dt_iop_satcurve_params_t *p = (const dt_iop_satcurve_params_t *)p1;
  if(d->formula != p->formula)
  {
    d->formula = p->formula;
    d->lut_inited = FALSE;
  }
  if(d->curve_type != p->curve_type || d->curve_num_nodes != p->curve_num_nodes)
  {
    dt_draw_curve_destroy(d->curve);
    d->curve = dt_draw_curve_new(0.f, 1.f, p->curve_type);
    d->curve_type = p->curve_type;
    d->curve_num_nodes = p->curve_num_nodes;
    for(int i = 0; i < p->curve_num_nodes; i++)
      dt_draw_curve_add_point(d->curve, p->curve[i].x, p->curve[i].y);
  }
  else
    for(int i = 0; i < p->curve_num_nodes; i++)
      dt_draw_curve_set_point(d->curve, i, p->curve[i].x, p->curve[i].y);
  dt_draw_curve_calc_values(d->curve, 0.f, 1.f, DT_IOP_SATCURVE_RES, NULL, d->lut);

  const dt_iop_order_iccprofile_info_t *work_profile = dt_ioppr_get_pipe_current_profile_info(self, piece->pipe);
  if(work_profile && (!d->lut_inited || work_profile != d->work_profile))
  {
    build_gamut_lut(d, work_profile);
    d->work_profile = work_profile;
    d->lut_inited = TRUE;
  }
}

void init_presets(dt_iop_module_so_t *self)
{
  dt_iop_satcurve_params_t p = {0};
  reset_curve(&p);
  dt_gui_presets_add_generic(_("neutral"), self->op, self->version(), &p, sizeof(p), TRUE, DEVELOP_BLEND_CS_RGB_SCENE);

  p.curve[0].y = .60f;
  p.curve[1].y = .55f;
  dt_gui_presets_add_generic(_("gentle saturation"), self->op, self->version(), &p, sizeof(p), TRUE, DEVELOP_BLEND_CS_RGB_SCENE);

  reset_curve(&p);
  p.curve_num_nodes = 3;
  p.curve[1] = (dt_iop_satcurve_node_t){.45f, .62f};
  p.curve[2] = (dt_iop_satcurve_node_t){1.f, .42f};
  dt_gui_presets_add_generic(_("protect saturated colors"), self->op, self->version(), &p, sizeof(p), TRUE, DEVELOP_BLEND_CS_RGB_SCENE);

  reset_curve(&p);
  p.curve_num_nodes = 3;
  p.curve[0].y = .38f;
  p.curve[1] = (dt_iop_satcurve_node_t){.35f, .48f};
  p.curve[2] = (dt_iop_satcurve_node_t){1.f, .58f};
  dt_gui_presets_add_generic(_("boost saturated colors"), self->op, self->version(), &p, sizeof(p), TRUE, DEVELOP_BLEND_CS_RGB_SCENE);
}

static inline int add_node(dt_iop_satcurve_params_t *p, const float x, const float y)
{
  int at = 0;
  while(at < p->curve_num_nodes && p->curve[at].x < x)
    at++;
  if((at && x - p->curve[at - 1].x < DT_IOP_SATCURVE_MIN_X_DISTANCE) ||
     (at < p->curve_num_nodes && p->curve[at].x - x < DT_IOP_SATCURVE_MIN_X_DISTANCE))
    return -1;
  for(int i = p->curve_num_nodes; i > at; i--)
    p->curve[i] = p->curve[i - 1];
  p->curve[at] = (dt_iop_satcurve_node_t){x, y};
  p->curve_num_nodes++;
  return at;
}

// paints the input-referred saturation histogram as a filled area behind
// the curve; similar to the histogram overlay in colorzones.c, but backed
// by our own float accumulator instead of the Lab-based histogram
static void _draw_sat_histogram(cairo_t *cr, dt_iop_satcurve_gui_data_t *g, const int w, const int h)
{
  dt_pthread_mutex_lock(&g->histogram_lock);
  const float hist_max = g->histogram_max;
  cairo_save(cr);
  cairo_set_source_rgba(cr, .8, .8, .8, .35);
  cairo_move_to(cr, 0, h);
  for(int i = 0; i < DT_IOP_SATCURVE_HIST_RES; i++)
  {
    const float x = w * i / (float)(DT_IOP_SATCURVE_HIST_RES - 1);
    const float y = h * (1.f - g->histogram[i] / hist_max);
    cairo_line_to(cr, x, y);
  }
  cairo_line_to(cr, w, h);
  cairo_close_path(cr);
  cairo_fill(cr);
  cairo_restore(cr);
  dt_pthread_mutex_unlock(&g->histogram_lock);
}

// paints the picker overlay (mean/min/max S_in_norm of the picked region),
// analogous to colorequal.c / colorzones.c but using our own S_in_norm
// metric instead of Lab/LCh or dt UCS HSB
static void _draw_sat_picker(cairo_t *cr, dt_iop_module_t *self, const int w, const int h)
{
  dt_iop_satcurve_gui_data_t *g = self->gui_data;
  if(!g->picker_valid) return;
  if(self->request_color_pick != DT_REQUEST_COLORPICK_MODULE) return;

  const float x_mean = CLAMP(g->picked_s, 0.f, 1.f) * w;
  const float x_min = CLAMP(g->picked_s_min, 0.f, 1.f) * w;
  const float x_max = CLAMP(g->picked_s_max, 0.f, 1.f) * w;

  cairo_save(cr);
  cairo_set_source_rgba(cr, 0.5, 0.7, 0.5, 0.25);
  cairo_rectangle(cr, x_min, 0, fmaxf(x_max - x_min, 0.f), h);
  cairo_fill(cr);

  cairo_set_source_rgba(cr, 0.5, 0.9, 0.5, 0.9);
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(2.));
  cairo_move_to(cr, x_mean, 0);
  cairo_line_to(cr, x_mean, h);
  cairo_stroke(cr);
  cairo_restore(cr);
}

// shared geometry for the curve graph itself: origin is offset from the
// widget's top-left by the inset; only the bottom reserves extra space
// for the decorative gradient bar, so area_draw and the pointer
// hit-testing functions always agree on where (0,0)-(1,1) map to
static inline void _get_graph_geometry(const GtkAllocation *a, float *x0, float *y0, float *w, float *h)
{
  const float inset = DT_IOP_SATCURVE_INSET;
  const float reserved = DT_IOP_SATCURVE_GRADIENT_SIZE + DT_IOP_SATCURVE_GRADIENT_GAP;
  *x0 = inset;
  *y0 = inset;
  *w = a->width - 2.f * inset;
  *h = a->height - 2.f * inset - reserved;
}

static gboolean area_draw(GtkWidget *widget, cairo_t *cr, dt_iop_module_t *self)
{
  dt_iop_satcurve_gui_data_t *g = self->gui_data;
  dt_iop_satcurve_params_t *p = self->params;
  GtkAllocation a;
  gtk_widget_get_allocation(widget, &a);
  float gx0, gy0, gw, gh;
  _get_graph_geometry(&a, &gx0, &gy0, &gw, &gh);
  const int w = (int)gw, h = (int)gh;
  if(g->curve_type != p->curve_type || g->curve_num_nodes != p->curve_num_nodes)
  {
    dt_draw_curve_destroy(g->curve);
    g->curve = dt_draw_curve_new(0.f, 1.f, p->curve_type);
    g->curve_type = p->curve_type;
    g->curve_num_nodes = p->curve_num_nodes;
    for(int i = 0; i < p->curve_num_nodes; i++)
      dt_draw_curve_add_point(g->curve, p->curve[i].x, p->curve[i].y);
  }
  else
    for(int i = 0; i < p->curve_num_nodes; i++)
      dt_draw_curve_set_point(g->curve, i, p->curve[i].x, p->curve[i].y);
  dt_draw_curve_calc_values(g->curve, 0.f, 1.f, DT_IOP_SATCURVE_RES, NULL, g->draw_ys);

  // background first: cairo_paint() fills the whole widget regardless of
  // any later cairo_translate(), so it must happen before the gradient
  // bars are drawn, or it would overpaint them
  cairo_set_source_rgb(cr, .18, .18, .18);
  cairo_paint(cr);

  // purely decorative perceptual gradient bar, below the graph; same
  // cairo_pattern_create_linear + dt_cairo_perceptual_gradient combo as
  // used in toneequal.c, just without any axis legend
  cairo_pattern_t *grad;

  // horizontal bar, below the graph
  grad = cairo_pattern_create_linear(gx0, 0.0, gx0 + gw, 0.0);
  dt_cairo_perceptual_gradient(grad, 1.0);
  cairo_set_line_width(cr, 0.0);
  cairo_rectangle(cr, gx0, gy0 + gh + DT_IOP_SATCURVE_GRADIENT_GAP,
                  gw, DT_IOP_SATCURVE_GRADIENT_SIZE);
  cairo_set_source(cr, grad);
  cairo_fill(cr);
  cairo_pattern_destroy(grad);

  cairo_translate(cr, gx0, gy0);
  cairo_set_source_rgba(cr, .4, .4, .4, .5);
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1));
  for(int i = 1; i < 4; i++)
  {
    cairo_move_to(cr, w * i / 4.f, 0);
    cairo_line_to(cr, w * i / 4.f, h);
    cairo_move_to(cr, 0, h * i / 4.f);
    cairo_line_to(cr, w, h * i / 4.f);
  }
  cairo_stroke(cr);

  // input histogram first, so it stays in the background
  _draw_sat_histogram(cr, g, w, h);
  _draw_sat_picker(cr, self, w, h);

  cairo_set_source_rgb(cr, .85, .85, .85);
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(2));
  cairo_move_to(cr, 0, h * (1.f - g->draw_ys[0]));
  for(int i = 1; i < DT_IOP_SATCURVE_RES; i++)
    cairo_line_to(cr, w * i / (float)(DT_IOP_SATCURVE_RES - 1), h * (1.f - g->draw_ys[i]));
  cairo_stroke(cr);
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1));
  for(int i = 0; i < p->curve_num_nodes; i++)
  {
    cairo_arc(cr, w * p->curve[i].x, h * (1.f - p->curve[i].y), DT_PIXEL_APPLY_DPI(i == g->selected ? 5 : 3), 0, DT_2PI_F);
    cairo_stroke(cr);
  }
  return FALSE;
}

static gboolean area_button(GtkWidget *widget, GdkEventButton *event, dt_iop_module_t *self)
{
  dt_iop_satcurve_gui_data_t *g = self->gui_data;
  dt_iop_satcurve_params_t *p = self->params;
  GtkAllocation a;
  gtk_widget_get_allocation(widget, &a);
  float gx0, gy0, w, h;
  _get_graph_geometry(&a, &gx0, &gy0, &w, &h);
  const float x = CLAMP((event->x - gx0) / w, 0.f, 1.f), y = CLAMP(1.f - (event->y - gy0) / h, 0.f, 1.f);
  int hit = -1;
  for(int i = 0; i < p->curve_num_nodes; i++)
    if(sqf(x - p->curve[i].x) + sqf(y - p->curve[i].y) < .0025f)
    {
      hit = i;
      break;
    }

  if(event->type == GDK_2BUTTON_PRESS && event->button == GDK_BUTTON_PRIMARY)
  {
    reset_curve(p);
    g->selected = -1;
    dt_dev_add_history_item(darktable.develop, self, TRUE);
    gtk_widget_queue_draw(widget);
    return TRUE;
  }
  if(event->button == GDK_BUTTON_SECONDARY && hit >= 0)
  {
    if((event->state & GDK_CONTROL_MASK) || p->curve_num_nodes <= 2)
      p->curve[hit].y = .5f;
    else
    {
      for(int i = hit; i < p->curve_num_nodes - 1; i++)
        p->curve[i] = p->curve[i + 1];
      p->curve_num_nodes--;
    }
    g->selected = -1;
    dt_dev_add_history_item(darktable.develop, self, TRUE);
    gtk_widget_queue_draw(widget);
    return TRUE;
  }
  if(event->button == GDK_BUTTON_PRIMARY)
  {
    // clicking/dragging outside an existing node inserts a new one on the curve
    if(hit < 0 && p->curve_num_nodes < DT_IOP_SATCURVE_MAXNODES)
      hit = add_node(p, x, CLAMP(dt_draw_curve_calc_value(g->curve, x), 0.f, 1.f));
    g->selected = hit;
    g->dragging = hit >= 0;
    return TRUE;
  }
  return FALSE;
}

static gboolean area_motion(GtkWidget *widget, GdkEventMotion *event, dt_iop_module_t *self)
{
  dt_iop_satcurve_gui_data_t *g = self->gui_data;
  dt_iop_satcurve_params_t *p = self->params;
  if(!g->dragging || g->selected < 0)
    return FALSE;
  GtkAllocation a;
  gtk_widget_get_allocation(widget, &a);
  float gx0, gy0, w, h;
  _get_graph_geometry(&a, &gx0, &gy0, &w, &h);
  const float x = CLAMP((event->x - gx0) / w, 0.f, 1.f);
  const int n = g->selected;
  if(n == 0)
    p->curve[n].x = 0.f;
  else if(n == p->curve_num_nodes - 1)
    p->curve[n].x = 1.f;
  else
    p->curve[n].x = CLAMP(x, p->curve[n - 1].x + DT_IOP_SATCURVE_MIN_X_DISTANCE, p->curve[n + 1].x - DT_IOP_SATCURVE_MIN_X_DISTANCE);
  p->curve[n].y = CLAMP(1.f - (event->y - gy0) / h, 0.f, 1.f);
  gtk_widget_queue_draw(widget);
  return TRUE;
}

static gboolean area_release(GtkWidget *widget, GdkEventButton *event, dt_iop_module_t *self)
{
  dt_iop_satcurve_gui_data_t *g = self->gui_data;
  if(g->dragging)
  {
    g->dragging = FALSE;
    dt_dev_add_history_item(darktable.develop, self, TRUE);
  }
  return TRUE;
}

// returns the module's committed gamut_lut if already available (module
// processed at least once); otherwise falls back to a neutral unity LUT
// (gamut = 1 for all hues) so the picker cannot crash right after loading,
// though it then shows unnormalized S values
static const float *_get_gamut_lut_for_picker(dt_iop_module_t *self)
{
  static float unity_gamut_lut[LUT_ELEM];
  static gboolean unity_inited = FALSE;
  if(!unity_inited)
  {
    for(int i = 0; i < LUT_ELEM; i++) unity_gamut_lut[i] = 1.f;
    unity_inited = TRUE;
  }

  for(GList *nodes = self->dev->full.pipe ? self->dev->full.pipe->nodes : NULL; nodes; nodes = g_list_next(nodes))
  {
    dt_dev_pixelpipe_iop_t *piece = nodes->data;
    if(piece->module == self && piece->data)
    {
      const dt_iop_satcurve_data_t *d = piece->data;
      if(d->lut_inited && d->gamut_lut) return d->gamut_lut;
    }
  }
  return unity_gamut_lut;
}

// called automatically after a color pick while g->colorpicker is active.
// self->picked_color/-min/-max are already in pipeline RGB (default_colorspace
// is IOP_CS_RGB), so they go straight through pixel_s_in_norm() using the
// same metric as the curve and histogram
void color_picker_apply(dt_iop_module_t *self, GtkWidget *widget, dt_dev_pixelpipe_t *pipe)
{
  dt_iop_satcurve_gui_data_t *g = self->gui_data;
  if(!g) return;

  const dt_iop_order_iccprofile_info_t *work_profile =
      dt_ioppr_get_iop_work_profile_info(self, self->dev->iop);
  if(!work_profile) return;

  dt_colormatrix_t inputmatrix = {{ 0.0f }};
  dt_colormatrix_t inputmatrix_trans;
  dt_colormatrix_mul(inputmatrix, XYZ_D50_to_D65_CAT16, work_profile->matrix_in);
  dt_colormatrix_transpose(inputmatrix_trans, inputmatrix);

  const float *const gamut_lut = _get_gamut_lut_for_picker(self);
  const float L_white = Y_to_dt_UCS_L_star(1.f);

  const dt_aligned_pixel_t mean_rgb = { self->picked_color[0], self->picked_color[1], self->picked_color[2], 0.f };
  const dt_aligned_pixel_t min_rgb  = { self->picked_color_min[0], self->picked_color_min[1], self->picked_color_min[2], 0.f };
  const dt_aligned_pixel_t max_rgb  = { self->picked_color_max[0], self->picked_color_max[1], self->picked_color_max[2], 0.f };

  dt_iop_satcurve_data_t picker_data = { 0 };
  picker_data.formula = ((const dt_iop_satcurve_params_t *)self->params)->formula;
  g->picked_s     = pixel_s_in_norm(&picker_data, mean_rgb, inputmatrix_trans, gamut_lut, L_white, NULL);
  g->picked_s_min = pixel_s_in_norm(&picker_data, min_rgb, inputmatrix_trans, gamut_lut, L_white, NULL);
  g->picked_s_max = pixel_s_in_norm(&picker_data, max_rgb, inputmatrix_trans, gamut_lut, L_white, NULL);
  g->picker_valid = TRUE;

  gtk_widget_queue_draw(GTK_WIDGET(g->area));
}

// reset the picker state when the module loses GUI focus, so no stale
// overlay lingers in another module
void gui_focus(dt_iop_module_t *self, gboolean in)
{
  if(!in)
    dt_iop_color_picker_reset(self, FALSE);
}

// S_in_norm is defined differently per formula (JzAzBz vs darktable UCS), so
// a histogram computed under the previous formula is stale and must not
// linger on screen after switching. The bauhaus binding for the formula
// combobox already updates self->params and adds a history item, but not
// necessarily a *forced* one (see the explicit TRUE argument used for curve
// node edits below) - force it here as well so process() always re-runs
// and _update_sat_histogram() refreshes the histogram in either direction.
void gui_changed(dt_iop_module_t *self, GtkWidget *widget, void *previous)
{
  dt_iop_satcurve_gui_data_t *g = self->gui_data;
  if(!g) return;

  if(widget == g->formula)
    dt_dev_add_history_item(darktable.develop, self, TRUE);
}

void gui_update(dt_iop_module_t *self)
{
  dt_iop_satcurve_gui_data_t *g = self->gui_data;
  const dt_iop_satcurve_params_t *p = self->params;
  if(!g) return;

  if(g->formula) dt_bauhaus_combobox_set(g->formula, p->formula);
  if(g->area) gtk_widget_queue_draw(GTK_WIDGET(g->area));
}

void gui_cleanup(dt_iop_module_t *self)
{
  dt_iop_satcurve_gui_data_t *g = self->gui_data;
  dt_draw_curve_destroy(g->curve);
  dt_pthread_mutex_destroy(&g->histogram_lock);
}

void gui_init(dt_iop_module_t *self)
{
  dt_iop_satcurve_gui_data_t *g = IOP_GUI_ALLOC(satcurve);
  const dt_iop_satcurve_params_t *p = self->default_params;
  g->selected = -1;
  g->curve_type = p->curve_type;
  g->curve_num_nodes = p->curve_num_nodes;
  g->curve = dt_draw_curve_new(0.f, 1.f, p->curve_type);
  for(int i = 0; i < p->curve_num_nodes; i++)
    dt_draw_curve_add_point(g->curve, p->curve[i].x, p->curve[i].y);

  memset(g->histogram, 0, sizeof(g->histogram));
  g->histogram_max = 1e-6f;
  dt_pthread_mutex_init(&g->histogram_lock, NULL);

  g->picker_valid = FALSE;
  g->picked_s = g->picked_s_min = g->picked_s_max = 0.f;

  self->widget = dt_gui_vbox();

  g->area = GTK_DRAWING_AREA(dt_ui_resize_wrap(NULL, 0, "plugins/darkroom/satcurve/graph_height"));
  gtk_widget_set_size_request(GTK_WIDGET(g->area), -1, DT_PIXEL_APPLY_DPI(180));
  gtk_widget_add_events(GTK_WIDGET(g->area), GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK);
  g_signal_connect(G_OBJECT(g->area), "draw", G_CALLBACK(area_draw), self);
  g_signal_connect(G_OBJECT(g->area), "button-press-event", G_CALLBACK(area_button), self);
  g_signal_connect(G_OBJECT(g->area), "button-release-event", G_CALLBACK(area_release), self);
  g_signal_connect(G_OBJECT(g->area), "motion-notify-event", G_CALLBACK(area_motion), self);
  dt_gui_box_add(self->widget, GTK_WIDGET(g->area));

  // Row below the draw area: picker button on the left, formula combobox on
  // the right. dt_bauhaus_combobox_from_params() auto-packs the widget into
  // self->widget, so it is detached here and re-packed into the shared hbox
  // (pack_start/pack_end keep both widgets at their natural width with a
  // flexible gap in between).
  // dt_color_picker_new_with_cst() is used instead of dt_color_picker_new()
  // because the latter attaches the picker icon to an existing bauhaus
  // widget rather than creating a standalone button.
  g->formula = dt_bauhaus_combobox_from_params(self, "formula");
  dt_bauhaus_combobox_add(g->formula, _("JzAzBz"));
  dt_bauhaus_combobox_add(g->formula, _("darktable UCS"));
  gtk_widget_set_tooltip_text(g->formula,
                              _("choose the perceptual saturation definition used by the curve"));

  g_object_ref(g->formula);
  gtk_container_remove(GTK_CONTAINER(self->widget), g->formula);

  GtkWidget *controls_hbox = dt_gui_hbox();
  gtk_box_set_spacing(GTK_BOX(controls_hbox), DT_PIXEL_APPLY_DPI(5));

  g->colorpicker = dt_color_picker_new_with_cst(self, DT_COLOR_PICKER_POINT, NULL, IOP_CS_RGB);
  gtk_widget_set_tooltip_text(g->colorpicker, _("pick saturation from image"));
  gtk_box_pack_start(GTK_BOX(controls_hbox), g->colorpicker, FALSE, FALSE, 0);
  gtk_box_pack_end(GTK_BOX(controls_hbox), g->formula, FALSE, FALSE, 0);
  g_object_unref(g->formula);

  dt_gui_box_add(self->widget, controls_hbox);
}

void init(dt_iop_module_t *self)
{
  self->params = calloc(1, sizeof(dt_iop_satcurve_params_t));
  self->default_params = calloc(1, sizeof(dt_iop_satcurve_params_t));
  self->params_size = sizeof(dt_iop_satcurve_params_t);
  reset_curve(self->params);
  reset_curve(self->default_params);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on