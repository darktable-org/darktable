/*
    This file is part of darktable,
    copyright (c) 2018 edgardo hoszowski.

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
   based on G'MIC filter "Freaky B&W"
   http://gmic.eu/

   it also have a dst version in addition to the fft one

#@gui Freaky B&amp;W : fx_freaky_bw, fx_freaky_bw_preview
#@gui : Strength (%) = float(90,0,100)
#@gui : Oddness (%) = float(20,0,100)
#@gui : Brightness (%) = float(0,-100,100)
#@gui : Contrast (%) = float(0,-100,100)
#@gui : Gamma (%) = float(0,-100,100)
#@gui : sep = separator(), Preview type = choice("Full","Forward horizontal","Forward vertical","Backward
horizontal","Backward vertical","Duplicate top","Duplicate left","Duplicate bottom","Duplicate right")
#@gui : sep = separator(), note = note("<small>Author: <i>David Tschumperl&#233;</i>.      Latest update:
<i>09/30/2015</i>.</small>")
fx_freaky_bw :
  -repeat $! -l[$>] -split_opacity -l[0]
    -to_rgb

    # Estimate gradient field of B&W result.
    --expand_xy 1,0 -channels. 0,4
    -f. ">if (c!=4,i,
           Rx = i(x+1,y,0,0) - i(x,y,0,0);
           Ry = i(x,y+1,0,0) - i(x,y,0,0);
           Rn = Rx^2 + Ry^2;
           Gx = i(x+1,y,0,1) - i(x,y,0,1);
           Gy = i(x,y+1,0,1) - i(x,y,0,1);
           Gn = Gx^2 + Gy^2;
           Bx = i(x+1,y,0,2) - i(x,y,0,2);
           By = i(x,y+1,0,2) - i(x,y,0,2);
           Bn = Bx^2 + By^2;
           n = 1e-5 + max(Rn,Gn,Bn)^"{$2%}";
           val = 0;
          if (Rn>=Gn && Rn>=Bn,
            i(x,y,0,3) = Rx/n; val=Ry/n,
          if (Gn>=Rn && Gn>=Bn,
            i(x,y,0,3) = Gx/n; val=Gy/n,
            i(x,y,0,3) = Bx/n; val=By/n));
          val
         )"
    -channels. 3,4
    -luminance[0] ia={0,ia}

    # Estimate laplacian of final image.
    -s. c
    -f.. "i - i(x-1,y,0,0)"
    -f. "i - i(x,y-1,0,0)"
    -+[-2,-1]

    # Reconstruct image from laplacian.
    -fft.
    100%,100%,1,1,'cos(2*x*pi/w)+cos(2*y*pi/h)' -*. 2 --. 4
    -=. 1 -/[-3,-2] . -rm.
    -=.. 0 -=. 0
    -ifft[-2,-1] -rm.
    -shrink_xy. 1 -+. $ia -n. 0,255

    # Merge result with original color image.
    -j[0] [1],0,0,0,0,{$1%} -rm.
    -adjust_colors ${3-5}
  -endl -a c -endl -done

fx_freaky_bw_preview :
  -gui_split_preview "-fx_freaky_bw $*",$-1

*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "bauhaus/bauhaus.h"
#include "common/colorspaces_inline_conversions.h"
#include "develop/imageop_math.h"

#ifdef HAVE_FFTW3
#include <fftw3.h>
#endif

DT_MODULE_INTROSPECTION(1, dt_iop_fbw_params_t)

typedef struct
{
  float *in_src;
  fftwf_complex *out_src;

  fftwf_plan plan_src;
  fftwf_plan plan_inv;

  int width_src;
  int height_src;

  int width_dest;
  int height_dest;

  int width_fft;
  int height_fft;
  int width_fft_complex;
  int height_fft_complex;

  dt_pthread_mutex_t *fftw3_lock;
} fbw_fft_t;

typedef enum dt_iop_fbw_bw_methods_t {
  dt_iop_fbw_bw_mix_dst = 0,
  dt_iop_fbw_bw_max_dst = 1,
  dt_iop_fbw_bw_mix_fft = 2,
  dt_iop_fbw_bw_max_fft = 3,
  dt_iop_fbw_bw_mix_dst_2 = 4,
  dt_iop_fbw_bw_max_dst_2 = 5
} dt_iop_fbw_bw_methods_t;

typedef struct dt_iop_fbw_params_t
{
  int bw_method;
  float oddness;
  float red;
  float green;
  float blue;
} dt_iop_fbw_params_t;

typedef struct dt_iop_fbw_gui_data_t
{
  uint64_t hash;
  dt_pthread_mutex_t lock;
  float img_min_in;
  float img_max_in;
  float img_min_out;
  float img_max_out;

  GtkWidget *cmb_bw_method;
  GtkWidget *sl_oddness;
  GtkWidget *sl_red;
  GtkWidget *sl_green;
  GtkWidget *sl_blue;
  GtkWidget *vbox_rgb;
} dt_iop_fbw_gui_data_t;

typedef struct dt_iop_fbw_params_t dt_iop_fbw_data_t;

const char *name()
{
  return _("freaky bw");
}

int groups()
{
  return IOP_GROUP_COLOR;
}

int flags()
{
  return IOP_FLAGS_SUPPORTS_BLENDING;
}

static void show_hide_controls(dt_iop_module_t *self, dt_iop_fbw_gui_data_t *d, dt_iop_fbw_params_t *p)
{
  switch(p->bw_method)
  {
    case dt_iop_fbw_bw_mix_fft:
    case dt_iop_fbw_bw_mix_dst:
    case dt_iop_fbw_bw_mix_dst_2:
      gtk_widget_show(GTK_WIDGET(d->vbox_rgb));
      break;
    case dt_iop_fbw_bw_max_fft:
    case dt_iop_fbw_bw_max_dst:
    case dt_iop_fbw_bw_max_dst_2:
    default:
      gtk_widget_hide(GTK_WIDGET(d->vbox_rgb));
      break;
  }
}

static void bw_method_callback(GtkComboBox *combo, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_fbw_params_t *p = (dt_iop_fbw_params_t *)self->params;
  dt_iop_fbw_gui_data_t *g = (dt_iop_fbw_gui_data_t *)self->gui_data;

  const int reset = darktable.gui->reset;
  darktable.gui->reset = 1;

  p->bw_method = dt_bauhaus_combobox_get((GtkWidget *)combo);

  show_hide_controls(self, g, p);

  darktable.gui->reset = reset;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void oddness_callback(GtkWidget *slider, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_fbw_params_t *p = (dt_iop_fbw_params_t *)self->params;

  p->oddness = dt_bauhaus_slider_get(slider);

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void red_callback(GtkWidget *slider, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_fbw_params_t *p = (dt_iop_fbw_params_t *)self->params;

  p->red = dt_bauhaus_slider_get(slider);

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void green_callback(GtkWidget *slider, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_fbw_params_t *p = (dt_iop_fbw_params_t *)self->params;

  p->green = dt_bauhaus_slider_get(slider);

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void blue_callback(GtkWidget *slider, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_fbw_params_t *p = (dt_iop_fbw_params_t *)self->params;

  p->blue = dt_bauhaus_slider_get(slider);

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *params, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  memcpy(piece->data, params, sizeof(dt_iop_fbw_params_t));
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_fbw_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_fbw_gui_data_t *g = (dt_iop_fbw_gui_data_t *)self->gui_data;
  dt_iop_fbw_params_t *p = (dt_iop_fbw_params_t *)self->params;

  dt_bauhaus_combobox_set(g->cmb_bw_method, p->bw_method);
  dt_bauhaus_slider_set(g->sl_oddness, p->oddness);
  dt_bauhaus_slider_set(g->sl_red, p->red);
  dt_bauhaus_slider_set(g->sl_green, p->green);
  dt_bauhaus_slider_set(g->sl_blue, p->blue);

  show_hide_controls(self, g, p);

  dt_pthread_mutex_lock(&g->lock);
  g->img_min_in = NAN;
  g->img_max_in = NAN;
  g->img_min_out = NAN;
  g->img_max_out = NAN;
  g->hash = 0;
  dt_pthread_mutex_unlock(&g->lock);
}

void init(dt_iop_module_t *module)
{
  module->data = NULL;
  module->params = calloc(1, sizeof(dt_iop_fbw_params_t));
  module->default_params = calloc(1, sizeof(dt_iop_fbw_params_t));
  module->default_enabled = 0;
  module->priority = 160; // module order created by iop_dependencies.py, do not edit! // right before exposure
  module->params_size = sizeof(dt_iop_fbw_params_t);
  module->gui_data = NULL;

  dt_iop_fbw_params_t tmp = { 0 };

  tmp.bw_method = dt_iop_fbw_bw_mix_dst;
  tmp.oddness = 15.f;

  tmp.red = 0.22248840f;
  tmp.green = 0.71690369f;
  tmp.blue = 0.06060791f;

  memcpy(module->params, &tmp, sizeof(dt_iop_fbw_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_fbw_params_t));
}

void cleanup(dt_iop_module_t *module)
{
  free(module->params);
  module->params = NULL;
}

void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_fbw_gui_data_t));
  dt_iop_fbw_gui_data_t *g = (dt_iop_fbw_gui_data_t *)self->gui_data;
  dt_iop_fbw_params_t *p = (dt_iop_fbw_params_t *)self->params;

  dt_pthread_mutex_init(&g->lock, NULL);
  g->hash = 0;
  g->img_min_in = NAN;
  g->img_max_in = NAN;
  g->img_min_out = NAN;
  g->img_max_out = NAN;

  self->widget = GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE));

  g->cmb_bw_method = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->cmb_bw_method, NULL, _("b&w conversion method"));
  dt_bauhaus_combobox_add(g->cmb_bw_method, _("rgb mix dst"));
  dt_bauhaus_combobox_add(g->cmb_bw_method, _("rgb max dst"));
  dt_bauhaus_combobox_add(g->cmb_bw_method, _("rgb mix fft"));
  dt_bauhaus_combobox_add(g->cmb_bw_method, _("rgb max fft"));
  dt_bauhaus_combobox_add(g->cmb_bw_method, _("rgb mix dst 2"));
  dt_bauhaus_combobox_add(g->cmb_bw_method, _("rgb max dst 2"));
  g_object_set(g->cmb_bw_method, "tooltip-text", _("b&w conversion method"), (char *)NULL);
  g_signal_connect(G_OBJECT(g->cmb_bw_method), "value-changed", G_CALLBACK(bw_method_callback), self);

  gtk_box_pack_start(GTK_BOX(self->widget), g->cmb_bw_method, TRUE, TRUE, 0);

  g->sl_oddness = dt_bauhaus_slider_new_with_range(self, 0.0, 100.0, 1.0, p->oddness, 2);
  dt_bauhaus_widget_set_label(g->sl_oddness, _("oddness"), _("oddness"));
  g_object_set(g->sl_oddness, "tooltip-text", _("oddness"), (char *)NULL);
  g_signal_connect(G_OBJECT(g->sl_oddness), "value-changed", G_CALLBACK(oddness_callback), self);

  gtk_box_pack_start(GTK_BOX(self->widget), g->sl_oddness, TRUE, TRUE, 0);

  g->vbox_rgb = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);

  g->sl_red = dt_bauhaus_slider_new_with_range(self, -2.0, 2.0, 0.005, p->red, 3);
  dt_bauhaus_widget_set_label(g->sl_red, _("red"), _("red"));
  g_object_set(g->sl_red, "tooltip-text", _("red"), (char *)NULL);
  g_signal_connect(G_OBJECT(g->sl_red), "value-changed", G_CALLBACK(red_callback), self);

  gtk_box_pack_start(GTK_BOX(g->vbox_rgb), g->sl_red, TRUE, TRUE, 0);

  g->sl_green = dt_bauhaus_slider_new_with_range(self, -2.0, 2.0, 0.005, p->green, 3);
  dt_bauhaus_widget_set_label(g->sl_green, _("green"), _("green"));
  g_object_set(g->sl_green, "tooltip-text", _("green"), (char *)NULL);
  g_signal_connect(G_OBJECT(g->sl_green), "value-changed", G_CALLBACK(green_callback), self);

  gtk_box_pack_start(GTK_BOX(g->vbox_rgb), g->sl_green, TRUE, TRUE, 0);

  g->sl_blue = dt_bauhaus_slider_new_with_range(self, -2.0, 2.0, 0.005, p->blue, 3);
  dt_bauhaus_widget_set_label(g->sl_blue, _("blue"), _("blue"));
  g_object_set(g->sl_blue, "tooltip-text", _("blue"), (char *)NULL);
  g_signal_connect(G_OBJECT(g->sl_blue), "value-changed", G_CALLBACK(blue_callback), self);

  gtk_box_pack_start(GTK_BOX(g->vbox_rgb), g->sl_blue, TRUE, TRUE, 0);

  gtk_box_pack_start(GTK_BOX(self->widget), g->vbox_rgb, TRUE, TRUE, 0);

  gtk_widget_show_all(g->vbox_rgb);
  gtk_widget_set_no_show_all(g->vbox_rgb, TRUE);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  dt_iop_fbw_gui_data_t *g = (dt_iop_fbw_gui_data_t *)self->gui_data;

  dt_pthread_mutex_destroy(&g->lock);

  free(self->gui_data);
  self->gui_data = NULL;
}

//-----------------------------------------------------------------------

static void fft(fbw_fft_t *fft_fbw, float *image_src, const int width, const int height,
                dt_pthread_mutex_t *fftw3_lock)
{
  fft_fbw->fftw3_lock = fftw3_lock;

  fft_fbw->width_src = width;
  fft_fbw->height_src = height;

  fft_fbw->width_dest = width;
  fft_fbw->height_dest = height;

  fft_fbw->width_fft = width;
  fft_fbw->height_fft = height;

  fft_fbw->width_fft_complex = fft_fbw->width_fft / 2 + 1;
  fft_fbw->height_fft_complex = fft_fbw->height_fft;

  dt_pthread_mutex_lock(fft_fbw->fftw3_lock);

  fft_fbw->in_src = (float *)fftwf_malloc(sizeof(float) * fft_fbw->width_fft * fft_fbw->height_fft);
  fft_fbw->out_src = (fftwf_complex *)fftwf_malloc(sizeof(fftwf_complex) * fft_fbw->width_fft_complex
                                                   * fft_fbw->height_fft_complex);

  fft_fbw->plan_src = fftwf_plan_dft_r2c_2d(fft_fbw->height_fft, fft_fbw->width_fft, fft_fbw->in_src,
                                            fft_fbw->out_src, FFTW_ESTIMATE);
  fft_fbw->plan_inv = fftwf_plan_dft_c2r_2d(fft_fbw->height_fft, fft_fbw->width_fft, fft_fbw->out_src,
                                            fft_fbw->in_src, FFTW_ESTIMATE);

  dt_pthread_mutex_unlock(fft_fbw->fftw3_lock);

  memset(fft_fbw->in_src, 0, sizeof(float) * fft_fbw->width_fft * fft_fbw->height_fft);
  memset(fft_fbw->out_src, 0, sizeof(fftwf_complex) * fft_fbw->width_fft_complex * fft_fbw->height_fft_complex);

  const int w = fft_fbw->width_src;
  const int h = fft_fbw->height_src;
  const int wf = fft_fbw->width_fft;
  float *fft_in_src = fft_fbw->in_src;

#ifdef _OPENMP
#pragma omp parallel for default(none) shared(fft_in_src, image_src) schedule(static)
#endif
  for(int y = 0; y < h; y++)
  {
    float *in_src = fft_in_src + y * wf;
    const float *src = image_src + y * w;

    for(int x = 0; x < w; x++)
    {
      in_src[x] = src[x];
    }
  }

  fftwf_execute(fft_fbw->plan_src);
}

static void ifft(fbw_fft_t *fft_fbw, float *image_dest)
{
  const float scale = 1.0 / (fft_fbw->width_fft * fft_fbw->height_fft);

  memset(fft_fbw->in_src, 0, sizeof(float) * fft_fbw->width_fft * fft_fbw->height_fft);

  fftwf_execute(fft_fbw->plan_inv);

  const int w = fft_fbw->width_dest;
  const int h = fft_fbw->height_dest;
  const int wf = fft_fbw->width_fft;
  float *in_src = fft_fbw->in_src;

#ifdef _OPENMP
#pragma omp parallel for default(none) shared(in_src, image_dest) schedule(static)
#endif
  for(int y = 0; y < h; y++)
  {
    float *dest = image_dest + y * w;
    float *out_inv = in_src + y * wf;

    for(int x = 0; x < w; x++)
    {
      dest[x] = out_inv[x] * scale;
    }
  }
}

static void fft_free(fbw_fft_t *fft_fbw)
{
  dt_pthread_mutex_lock(fft_fbw->fftw3_lock);

  if(fft_fbw->plan_src) fftwf_destroy_plan(fft_fbw->plan_src);
  if(fft_fbw->plan_inv) fftwf_destroy_plan(fft_fbw->plan_inv);

  if(fft_fbw->in_src) fftwf_free(fft_fbw->in_src);
  if(fft_fbw->out_src) fftwf_free(fft_fbw->out_src);

  dt_pthread_mutex_unlock(fft_fbw->fftw3_lock);

  fft_fbw->plan_src = NULL;
  fft_fbw->plan_inv = NULL;

  fft_fbw->in_src = NULL;
  fft_fbw->out_src = NULL;
}

static inline float sRGBtoRGB(float sval)
{
  return (sval <= 0.04045f ? sval / (12.92f) : powf((sval + 0.055f) / (1.055f), 2.4f));
}

static inline float RGBtosRGB(float val)
{
  return (val <= 0.0031308f ? val * 12.92f : 1.055f * powf(val, 0.416667f) - 0.055f);
}

static inline float rgb_luminance(const float r, const float g, const float b, const float rgb[3])
{
  return (r * rgb[0] + g * rgb[1] + b * rgb[2]);
}

static void image_to_output(float *img_src, const int width, const int height, const int ch, float *img_dest)
{
  const int stride = width * height;

#ifdef _OPENMP
#pragma omp parallel for default(none) shared(img_src, img_dest) schedule(static)
#endif
  for(int i = 0; i < stride; i++)
  {
    img_dest[i * ch] = img_dest[i * ch + 1] = img_dest[i * ch + 2] = sRGBtoRGB(img_src[i]);
  }
}

static void pad_image_mix(const float *const img_src, const int width, const int height, const int ch,
                          float *img_dest, const int pad_w, const int pad_h, const float rgb[3])
{
  const int iwidth = width + pad_w * 2;
  const int iheight = height + pad_h * 2;

  memset(img_dest, 0, iwidth * iheight * sizeof(float));

#ifdef _OPENMP
#pragma omp parallel for default(none) shared(img_dest, rgb) schedule(static)
#endif
  for(int y = 0; y < height; y++)
  {
    const float *const s = img_src + y * width * ch;
    float *d = img_dest + (y + pad_h) * iwidth + pad_w;

    for(int x = 0; x < width; x++)
    {
      d[x] = (rgb_luminance(RGBtosRGB(s[x * ch + 0]), RGBtosRGB(s[x * ch + 1]), RGBtosRGB(s[x * ch + 2]), rgb));
    }
  }
}

static void pad_image_max(const float *const img_src, const int width, const int height, const int ch,
                          float *img_dest, const int pad_w, const int pad_h)
{
  const int iwidth = width + pad_w * 2;
  const int iheight = height + pad_h * 2;

  memset(img_dest, 0, iwidth * iheight * ch * sizeof(float));

#ifdef _OPENMP
#pragma omp parallel for default(none) shared(img_dest) schedule(static)
#endif
  for(int y = 0; y < height; y++)
  {
    const float *const s = img_src + y * width * ch;
    float *d = img_dest + (y + pad_h) * iwidth * ch + pad_w * ch;

    for(int x = 0; x < width; x++)
    {
      for(int c = 0; c < ch; c++)
      {
        d[x * ch + c] = RGBtosRGB(s[x * ch + c]);
      }
    }
  }
}

static void unpad_image(float *img_src, const int width, const int height, float *img_dest, const int pad_w,
                        const int pad_h)
{
  const int iwidth = width + pad_w * 2;

  memset(img_dest, 0, width * height * sizeof(float));

#ifdef _OPENMP
#pragma omp parallel for default(none) shared(img_src, img_dest) schedule(static)
#endif
  for(int y = 0; y < height; y++)
  {
    float *s = img_src + (y + pad_h) * iwidth + pad_w;
    float *d = img_dest + y * width;

    memcpy(d, s, width * sizeof(float));
  }
}

static void normalize(float *img_src, const int width, const int height, float *img_min, float *img_max,
                      const float L, const float H)
{
  float min = INFINITY;
  float max = -INFINITY;
  const int stride = width * height;

  if(isnan(*img_min) || isnan(*img_max))
  {
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(img_src) schedule(static) reduction(min : min) reduction(max : max)
#endif
    for(int i = 0; i < stride; i++)
    {
      min = MIN(min, img_src[i]);
      max = MAX(max, img_src[i]);
    }

    *img_min = min;
    *img_max = max;
  }
  else
  {
    min = *img_min;
    max = *img_max;
  }

  if(min == max) return;

  const float mult = (H - L) / (max - min);

#ifdef _OPENMP
#pragma omp parallel for default(none) shared(img_src, min) schedule(static)
#endif
  for(int i = 0; i < stride; i++)
  {
    img_src[i] = ((img_src[i] - min) * mult) + L;
  }
}

static void gradient_rgb_mix(float *img_src, float *img_grx, float *img_gry, const int width, const int height,
                             const int pad_w, const int pad_h, const float _oddness, const float image_scale)
{
  const float oddness = _oddness * sqrtf(image_scale);

  memset(img_grx, 0, width * height * sizeof(float));
  memset(img_gry, 0, width * height * sizeof(float));

#ifdef _OPENMP
#pragma omp parallel for default(none) shared(img_src, img_grx, img_gry) schedule(static)
#endif
  for(int y = 0; y < height - 1; y++)
  {
    float *s0 = img_src + y * width;
    float *s1 = img_src + (y + 1) * width;
    float *dx = img_grx + y * width;
    float *dy = img_gry + y * width;

    for(int x = 0; x < width - 1; x++)
    {
      float grx = fabsf(s0[x + 1]) - fabsf(s0[x]);
      float gry = fabsf(s1[x]) - fabsf(s0[x]);
      float grn = ((grx * grx + gry * gry));

      if(grn > 0.f)
      {
        float n = powf(grn, oddness);

        dx[x] = grx / n;
        dy[x] = gry / n;
      }
    }
  }
}

static void gradient_rgb_max(float *img_src, float *img_grx, float *img_gry, const int width, const int height,
                             const int pad_w, const int pad_h, const float _oddness, const float image_scale)
{
  const int ch = 4;
  const float oddness = _oddness * sqrtf(image_scale);

  memset(img_grx, 0, width * height * sizeof(float));
  memset(img_gry, 0, width * height * sizeof(float));

#ifdef _OPENMP
#pragma omp parallel for default(none) shared(img_src, img_grx, img_gry) schedule(static)
#endif
  for(int y = 0; y < height - 1; y++)
  {
    float *s0 = img_src + y * width * ch;
    float *s1 = img_src + (y + 1) * width * ch;
    float *dx = img_grx + y * width;
    float *dy = img_gry + y * width;

    for(int x = 0; x < width - 1; x++)
    {
      float RGBx[3] = { 0 };
      float RGBy[3] = { 0 };
      float RGBn[3] = { 0 };

      for(int c = 0; c < 3; c++)
      {
        RGBx[c] = fabsf(s0[(x + 1) * ch + c]) - fabsf(s0[x * ch + c]);
        RGBy[c] = fabsf(s1[x * ch + c]) - fabsf(s0[x * ch + c]);

        RGBn[c] = RGBx[c] * RGBx[c] + RGBy[c] * RGBy[c];
      }

      int max_bn = 0;
      if(RGBn[0] > RGBn[1])
        max_bn = (RGBn[0] > RGBn[2]) ? 0 : 2;
      else
        max_bn = (RGBn[1] > RGBn[2]) ? 1 : 2;

      float n = 1e-5 + powf(RGBn[max_bn], oddness);

      dx[x] = RGBx[max_bn] / n;
      dy[x] = RGBy[max_bn] / n;
    }
  }
}

static void estimate_laplacian_fft(float *img_grx, float *img_gry, float *img_dest, const int width,
                                   const int height, const int pad_w, const int pad_h)
{
  const int stride = width * height;

  memset(img_dest, 0, stride * sizeof(float));

#ifdef _OPENMP
#pragma omp parallel for default(none) shared(img_grx) schedule(static)
#endif
  for(int y = 0; y < height; y++)
  {
    float *s0 = img_grx + y * width;

    for(int x = width - 1; x > 0; x--)
    {
      s0[x] -= s0[x - 1];
    }
  }

  for(int y = height - 1; y > 0; y--)
  {
    float *s0 = img_gry + y * width;
    float *s1 = img_gry + (y - 1) * width;

    for(int x = 0; x < width; x++)
    {
      s0[x] -= s1[x];
    }
  }

#ifdef _OPENMP
#pragma omp parallel for default(none) shared(img_grx, img_gry, img_dest) schedule(static)
#endif
  for(int i = 0; i < stride; i++)
  {
    img_dest[i] = (img_grx[i] + img_gry[i]);
  }
}

static void estimate_laplacian_dst(float *img_grx, float *img_gry, float *img_dest, const int width,
                                   const int height)
{
  const int stride = width * height;

  float *img_gxx = NULL;
  float *img_gyy = NULL;

  img_gxx = dt_alloc_align(64, width * height * sizeof(float));
  if(img_gxx == NULL) goto cleanup;

  img_gyy = dt_alloc_align(64, width * height * sizeof(float));
  if(img_gyy == NULL) goto cleanup;

  memset(img_dest, 0, stride * sizeof(float));
  memset(img_gxx, 0, stride * sizeof(float));
  memset(img_gyy, 0, stride * sizeof(float));

#ifdef _OPENMP
#pragma omp parallel for default(none) shared(img_grx, img_gry, img_gyy, img_gxx) schedule(static)
#endif
  for(int y = 0; y < height - 1; y++)
  {
    const int width_y = y * width;
    const int width_y1 = (y + 1) * width;

    float *gyy = img_gyy + width_y1;
    float *gxx = img_gxx + width_y;
    float *gry1 = img_gry + width_y1;
    float *gry = img_gry + width_y;
    float *grx = img_grx + width_y;

    for(int x = 0; x < width - 1; x++)
    {
      gyy[x] = gry1[x] - gry[x];
      gxx[(x + 1)] = grx[(x + 1)] - grx[x];
    }
  }

#ifdef _OPENMP
#pragma omp parallel for default(none) shared(img_dest, img_gyy, img_gxx) schedule(static)
#endif
  for(int i = 0; i < stride; i++)
  {
    img_dest[i] = (img_gxx[i] + img_gyy[i]);
  }

cleanup:
  if(img_gxx) dt_free_align(img_gxx);
  if(img_gyy) dt_free_align(img_gyy);
}

static void recontruct_laplacian_fft(float *img_src, float *img_dest, const int width, const int height,
                                     dt_pthread_mutex_t *fftw3_lock)
{
  fbw_fft_t fft_fbw = { 0 };

  fft(&fft_fbw, img_src, width, height, fftw3_lock);

  const int width_fft = fft_fbw.width_fft;
  const int height_fft = fft_fbw.height_fft;

  const float piw = (2.f * (float)M_PI / (float)width_fft);
  const float pih = (2.f * (float)M_PI / (float)height_fft);

  const int w = fft_fbw.width_fft_complex;
  const int h = fft_fbw.height_fft_complex;
  fftwf_complex *out_src = fft_fbw.out_src;

#ifdef _OPENMP
#pragma omp parallel for default(none) shared(out_src) schedule(static)
#endif
  for(int y = 0; y < h; y++)
  {
    fftwf_complex *d = out_src + y * w;

    const float cos_y = cos(pih * y);

    for(int x = 0; x < w; x++)
    {
      const float cos_x = cos(piw * x);

      if(x > 0 || y > 0)
      {
        float cos_xy = (cos_x + cos_y - 2.f) * 2.f;

        d[x][0] /= cos_xy;
        d[x][1] /= cos_xy;
      }
    }
  }

  fft_fbw.out_src[0][0] = 0.f;
  fft_fbw.out_src[0][1] = 0.f;

  ifft(&fft_fbw, img_dest);

  fft_free(&fft_fbw);
}

static void dst(float *img_src, float *img_dest, const int width, const int height, dt_pthread_mutex_t *fftw3_lock)
{
  float *fft_in = NULL;
  float *fft_out = NULL;
  fftwf_plan fft_dst_plan = NULL;

  dt_pthread_mutex_lock(fftw3_lock);
  fft_in = (float *)fftwf_malloc(width * height * sizeof(float));
  fft_out = (float *)fftwf_malloc(width * height * sizeof(float));

  fft_dst_plan = fftwf_plan_r2r_2d(height, width, fft_in, fft_out, FFTW_RODFT10, FFTW_RODFT10, FFTW_ESTIMATE);
  dt_pthread_mutex_unlock(fftw3_lock);

  if(fft_in == NULL || fft_out == NULL || fft_dst_plan == NULL) goto cleanup;

  memset(fft_in, 0, width * height * sizeof(float));
  memset(fft_out, 0, width * height * sizeof(float));

#ifdef _OPENMP
#pragma omp parallel for default(none) shared(fft_in, img_src) schedule(static)
#endif
  for(int y = 0; y < height; y++)
  {
    float *d = fft_in + y * width;
    const float *s = img_src + y * width;

    for(int x = 0; x < width; x++)
    {
      d[x] = s[x];
    }
  }

  fftwf_execute_r2r(fft_dst_plan, fft_in, fft_out);

#ifdef _OPENMP
#pragma omp parallel for default(none) shared(fft_out, img_dest) schedule(static)
#endif
  for(int y = 0; y < height; y++)
  {
    float *s = fft_out + y * width;
    float *d = img_dest + y * width;

    for(int x = 0; x < width; x++)
    {
      d[x] = s[x];
    }
  }

cleanup:
  dt_pthread_mutex_lock(fftw3_lock);
  fftwf_destroy_plan(fft_dst_plan);

  fftwf_free(fft_in);
  fftwf_free(fft_out);
  dt_pthread_mutex_unlock(fftw3_lock);
}

static void idst(float *img_src, float *img_dest, const int width, const int height, dt_pthread_mutex_t *fftw3_lock)
{
  float *fft_in = NULL;
  float *fft_out = NULL;
  fftwf_plan fft_dst_plan = NULL;

  const float scale = 1.0f / (float)(4 * width * height);

  dt_pthread_mutex_lock(fftw3_lock);
  fft_in = (float *)fftwf_malloc(width * height * sizeof(float));
  fft_out = (float *)fftwf_malloc(width * height * sizeof(float));

  fft_dst_plan = fftwf_plan_r2r_2d(height, width, fft_in, fft_out, FFTW_RODFT01, FFTW_RODFT01, FFTW_ESTIMATE);
  dt_pthread_mutex_unlock(fftw3_lock);

  if(fft_in == NULL || fft_out == NULL || fft_dst_plan == NULL) goto cleanup;

  memset(fft_in, 0, width * height * sizeof(float));
  memset(fft_out, 0, width * height * sizeof(float));

#ifdef _OPENMP
#pragma omp parallel for default(none) shared(fft_in, img_src) schedule(static)
#endif
  for(int y = 0; y < height; y++)
  {
    float *d = fft_in + y * width;
    const float *s = img_src + y * width;

    for(int x = 0; x < width; x++)
    {
      d[x] = s[x];
    }
  }

  fftwf_execute_r2r(fft_dst_plan, fft_in, fft_out);

#ifdef _OPENMP
#pragma omp parallel for default(none) shared(fft_out, img_dest) schedule(static)
#endif
  for(int y = 0; y < height; y++)
  {
    float *s = fft_out + y * width;
    float *d = img_dest + y * width;

    for(int x = 0; x < width; x++)
    {
      d[x] = s[x] * scale;
    }
  }

cleanup:
  dt_pthread_mutex_lock(fftw3_lock);
  fftwf_destroy_plan(fft_dst_plan);

  fftwf_free(fft_in);
  fftwf_free(fft_out);
  dt_pthread_mutex_unlock(fftw3_lock);
}

static void recontruct_laplacian_dst(float *img_src, float *img_dest, const int width, const int height,
                                     dt_pthread_mutex_t *fftw3_lock)
{
  float *img_dst = NULL;

  const float piw = (M_PI / (float)(width - 1));
  const float pih = (M_PI / (float)(height - 1));

  img_dst = dt_alloc_align(64, width * height * sizeof(float));
  if(img_dst == NULL) goto cleanup;

  dst(img_src, img_dst, width, height, fftw3_lock);

#ifdef _OPENMP
#pragma omp parallel for default(none) shared(img_dst) schedule(static)
#endif
  for(int y = 0; y < height; y++)
  {
    float *d = img_dst + y * width;

    const float cos_y = cosf(pih * (float)(y + 1));

    for(int x = 0; x < width; x++)
    {
      const float cos_x = cosf(piw * (float)(x + 1));
      const float cos_xy = 2.f * (cos_y + cos_x) - 4.f;
      if(cos_xy != 0.f) d[x] /= cos_xy;
    }
  }

  idst(img_dst, img_dest, width, height, fftw3_lock);

cleanup:
  if(img_dst) dt_free_align(img_dst);
}

static void get_stats(const float *const img_src, const int width, const int height, const int ch, float *_min,
                      float *_max)
{
  float min = INFINITY;
  float max = -INFINITY;

  const int stride = width * height * ch;
  const int ch1 = (ch == 4) ? 3 : ch;

#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static) reduction(min : min) reduction(max : max)
#endif
  for(int i = 0; i < stride; i += ch)
  {
    for(int c = 0; c < ch1; c++)
    {
      min = MIN(min, img_src[i + c]);
      max = MAX(max, img_src[i + c]);
    }
  }

  *_min = min;
  *_max = max;
}

static void fbw_process(const float *const img_src, float *const img_dest, const int width, const int height,
                        const int ch, const int bw_method, const float oddness, const float red, const float green,
                        const float blue, float *img_min_in, float *img_max_in, float *img_min_out,
                        float *img_max_out, const float image_scale, dt_pthread_mutex_t *fftw3_lock)
{
  float *img_padded = NULL;
  float *img_grx = NULL;
  float *img_gry = NULL;

  const int pad_w = 1;
  const int pad_h = pad_w;

  const int iwidth = width + pad_w * 2;
  const int iheight = height + pad_h * 2;

  const float rgb[3] = { red, green, blue };

  if(isnan(*img_min_in) || isnan(*img_max_in)) get_stats(img_src, width, height, ch, img_min_in, img_max_in);

  img_grx = dt_alloc_align(64, iwidth * iheight * sizeof(float));
  if(img_grx == NULL) goto cleanup;

  img_gry = dt_alloc_align(64, iwidth * iheight * sizeof(float));
  if(img_gry == NULL) goto cleanup;

  img_padded = dt_alloc_align(
      64, iwidth * iheight * ((bw_method == dt_iop_fbw_bw_mix_fft || bw_method == dt_iop_fbw_bw_mix_dst) ? 1 : ch)
              * sizeof(float));
  if(img_padded == NULL) goto cleanup;

  if(bw_method == dt_iop_fbw_bw_mix_fft || bw_method == dt_iop_fbw_bw_mix_dst)
    pad_image_mix(img_src, width, height, ch, img_padded, pad_w, pad_h, rgb);
  else
    pad_image_max(img_src, width, height, ch, img_padded, pad_w, pad_h);

  if(bw_method == dt_iop_fbw_bw_mix_fft || bw_method == dt_iop_fbw_bw_mix_dst)
    gradient_rgb_mix(img_padded, img_grx, img_gry, iwidth, iheight, pad_w, pad_h, oddness, image_scale);
  else
    gradient_rgb_max(img_padded, img_grx, img_gry, iwidth, iheight, pad_w, pad_h, oddness, image_scale);

  if(bw_method == dt_iop_fbw_bw_mix_fft || bw_method == dt_iop_fbw_bw_max_fft)
    estimate_laplacian_fft(img_grx, img_gry, img_padded, iwidth, iheight, pad_w, pad_h);
  else
    estimate_laplacian_dst(img_grx, img_gry, img_padded, iwidth, iheight);

  if(bw_method == dt_iop_fbw_bw_mix_fft || bw_method == dt_iop_fbw_bw_max_fft)
    recontruct_laplacian_fft(img_padded, img_gry, iwidth, iheight, fftw3_lock);
  else
    recontruct_laplacian_dst(img_padded, img_gry, iwidth, iheight, fftw3_lock);

  unpad_image(img_gry, width, height, img_grx, pad_w, pad_h);

  normalize(img_grx, width, height, img_min_out, img_max_out, *img_min_in, *img_max_in);

  image_to_output(img_grx, width, height, ch, img_dest);

cleanup:
  if(img_grx) dt_free_align(img_grx);
  if(img_gry) dt_free_align(img_gry);
  if(img_padded) dt_free_align(img_padded);
}

static void fbw_process_2(const float *const img_src, float *const img_dest, const int width, const int height,
                          const int ch, const int bw_method, const float oddness, const float red,
                          const float green, const float blue, float *img_min_in, float *img_max_in,
                          float *img_min_out, float *img_max_out, const float image_scale,
                          dt_pthread_mutex_t *fftw3_lock)
{
  float *img_padded = NULL;
  float *img_grx = NULL;
  float *img_gry = NULL;

  const int pad_w = 1;
  const int pad_h = pad_w;

  const int iwidth = width + pad_w * 2;
  const int iheight = height + pad_h * 2;

  const float rgb[3] = { red, green, blue };

  if(isnan(*img_min_in) || isnan(*img_max_in)) get_stats(img_src, width, height, ch, img_min_in, img_max_in);

  img_grx = dt_alloc_align(64, iwidth * iheight * sizeof(float));
  if(img_grx == NULL) goto cleanup;

  img_gry = dt_alloc_align(64, iwidth * iheight * sizeof(float));
  if(img_gry == NULL) goto cleanup;

  img_padded
      = dt_alloc_align(64, iwidth * iheight * ((bw_method == dt_iop_fbw_bw_mix_dst_2) ? 1 : ch) * sizeof(float));
  if(img_padded == NULL) goto cleanup;

  if(bw_method == dt_iop_fbw_bw_mix_dst_2)
    pad_image_mix(img_src, width, height, ch, img_padded, pad_w, pad_h, rgb);
  else
    pad_image_max(img_src, width, height, ch, img_padded, pad_w, pad_h);

  if(bw_method == dt_iop_fbw_bw_mix_dst_2)
    gradient_rgb_mix(img_padded, img_grx, img_gry, iwidth, iheight, pad_w, pad_h, oddness, image_scale);
  else
    gradient_rgb_max(img_padded, img_grx, img_gry, iwidth, iheight, pad_w, pad_h, oddness, image_scale);

  estimate_laplacian_dst(img_grx, img_gry, img_padded, iwidth, iheight);

  unpad_image(img_padded, width, height, img_grx, pad_w, pad_h);

  recontruct_laplacian_dst(img_grx, img_gry, width, height, fftw3_lock);

  normalize(img_gry, width, height, img_min_out, img_max_out, *img_min_in, *img_max_in);

  image_to_output(img_gry, width, height, ch, img_dest);

cleanup:
  if(img_grx) dt_free_align(img_grx);
  if(img_gry) dt_free_align(img_gry);
  if(img_padded) dt_free_align(img_padded);
}

void process_internal(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
                      void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_fbw_data_t *const p = (const dt_iop_fbw_data_t *const)piece->data;
  dt_iop_fbw_gui_data_t *g = (dt_iop_fbw_gui_data_t *)self->gui_data;
  const float image_scale = roi_in->scale / piece->iscale;

  float img_min_in = NAN;
  float img_max_in = NAN;
  float img_min_out = NAN;
  float img_max_out = NAN;

  // get image range from full pipe
  if(self->dev->gui_attached && g && piece->pipe->type == DT_DEV_PIXELPIPE_FULL)
  {
    dt_pthread_mutex_lock(&g->lock);
    const uint64_t hash = g->hash;
    dt_pthread_mutex_unlock(&g->lock);

    if(hash != 0 && !dt_dev_sync_pixelpipe_hash(self->dev, piece->pipe, 0, self->priority, &g->lock, &g->hash))
      dt_control_log(_("[freaky bw] inconsistent output"));

    dt_pthread_mutex_lock(&g->lock);
    img_min_in = g->img_min_in;
    img_max_in = g->img_max_in;
    img_min_out = g->img_min_out;
    img_max_out = g->img_max_out;
    dt_pthread_mutex_unlock(&g->lock);
  }

  if(p->bw_method == dt_iop_fbw_bw_mix_fft || p->bw_method == dt_iop_fbw_bw_max_fft
     || p->bw_method == dt_iop_fbw_bw_mix_dst || p->bw_method == dt_iop_fbw_bw_max_dst)
    fbw_process((float *)ivoid, (float *)ovoid, roi_in->width, roi_in->height, piece->colors, p->bw_method,
                p->oddness / 100.f, p->red, p->green, p->blue, &img_min_in, &img_max_in, &img_min_out,
                &img_max_out, image_scale, &darktable.fftw3_threadsafe);
  else
    fbw_process_2((float *)ivoid, (float *)ovoid, roi_in->width, roi_in->height, piece->colors, p->bw_method,
                  p->oddness / 100.f, p->red, p->green, p->blue, &img_min_in, &img_max_in, &img_min_out,
                  &img_max_out, image_scale, &darktable.fftw3_threadsafe);

  // if preview pipe, store the image range
  if(self->dev->gui_attached && g && piece->pipe->type == DT_DEV_PIXELPIPE_PREVIEW)
  {
    uint64_t hash = dt_dev_hash_plus(self->dev, piece->pipe, 0, self->priority);
    dt_pthread_mutex_lock(&g->lock);
    g->img_min_in = img_min_in;
    g->img_max_in = img_max_in;
    g->img_min_out = img_min_out;
    g->img_max_out = img_max_out;
    g->hash = hash;
    dt_pthread_mutex_unlock(&g->lock);
  }

  if(piece->pipe->mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK)
    dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);
}

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  process_internal(self, piece, ivoid, ovoid, roi_in, roi_out);
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
