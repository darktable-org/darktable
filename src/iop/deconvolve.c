/*
    This file is part of darktable,
    copyright (c) 2009--2010 johannes hanika.

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
#include <stdlib.h>
#include <math.h>
#include <assert.h>
#include <string.h>
#ifdef HAVE_GEGL
  #include <gegl.h>
#endif
#include "develop/develop.h"
#include "develop/imageop.h"
#include "control/control.h"
#include "dtgtk/slider.h"
#include "gui/gtk.h"
#include <gtk/gtk.h>
#include <inttypes.h>
#include "Clarity-1.0/include/Clarity.h"

extern pthread_mutex_t clarity_mutex; 

DT_MODULE(1)

#define MAXR 8

typedef struct dt_iop_deconvolve_params_t
{
  float radius, amount, threshold, deconvdamping;
  unsigned method : 4, iterations:12, L:1, A:1, B:1;
  float  snr;
}
dt_iop_deconvolve_params_t;

typedef struct dt_iop_deconvolve_gui_data_t
{
  //GtkVBox   *vbox1,  *vbox2;
  GtkLabel  *label1, *label2, *label3;
  GtkDarktableSlider *scale1, *scale2, *scale3;
  GtkComboBox *method;
  GtkDarktableSlider *snr, *num_iter;
  GtkLabel  *label4, *label5, *label6, *label7;
  GtkCheckButton *chan_L, *chan_A, *chan_B;
}
dt_iop_deconvolve_gui_data_t;

typedef struct dt_iop_deconvolve_data_t
{
  //float radius, amount, threshold;
  float radius, amount, threshold, deconvdamping;
  unsigned method : 4, iterations:12, L:1, A:1, B:1;
  float  snr;
}
dt_iop_deconvolve_data_t;

const char *name()
{
  return _("deconvolve_sharpen");
}


int 
groups () 
{
	return IOP_GROUP_CORRECT;
}

/**
 * Generates image representing a Gaussian convolution kernel.
 *
 * @param dim   Parameter for the dimensions of the kernel.
 * @param sigma Standard deviation of blurring kernel.
 * @return Kernel image stored in memory allocated by malloc. The caller is
 * responsible for free'ing this memory.
 */
float * Tst_GenerateGaussianKernel(Clarity_Dim3 *dim, float sigma) {
  int PSFX, PSFY, PSFZ;
  PSFX = dim->x;  PSFY = dim->y;  PSFZ = dim->z;
  float fz, fy, fx, value;

  printf("Generate kernel x=%d y=%d z=%d, sigma=%f\n", dim->x, dim->y, dim->z, sigma);

  float *kernel = (float *) malloc(sizeof(float)*dim->x*dim->y*dim->z);

  float sum = 0.0f;
  float sigma2 = sigma*sigma;
  for (int iz = 0; iz < PSFZ; iz++) {
    //float fz = (float)(iz-(PSFZ/2));
    fz = (float)(iz-(PSFZ/2));
    for (int iy = 0; iy < PSFY; iy++) {
//      float fy = static_cast<float>(iy-(PSFY/2));
      fy = (float)(iy-(PSFY/2));
      for (int ix = 0; ix < PSFX; ix++) {
//        float fx = static_cast<float>(ix-(PSFX/2));
        fx = (float)(ix-(PSFX/2));
	//float value =
	value = (1.0f / pow(2.0*M_PI*sigma2, 1.5)) *
          exp(-((fx*fx + fy*fy + fz*fz)/(2*sigma2)));
        kernel[(iz*PSFX*PSFY) + (iy*PSFX) + ix] = value;
	sum += value;
      }
    }
  }

  // Normalize the kernel
  float div = 1.0f / sum;
  for (int i = 0; i < PSFX*PSFY*PSFZ; i++) {
    kernel[i] *= div;
  }

  return kernel;

}

/*extract from an RGB image the R, G of B channel*/
float *get_channel (int w, int h, float * im, int chan, float *out)
{
  int i, j;
  int index, index1;
  int cmin, cmax;
  float min, max;
  min=99.0;
  max=0.0;
  cmin=0; cmax=0;
  //float *out = (float *) malloc (sizeof (float) * w * h);
  printf("Get channel w=%d h=%d chan=%d\n", w, h, chan);

  for (i = 0; i < h; i++) {
      index = i * w;
      for (j = 0; j < w; j++) {
	  index1 = (index + j) * 3;
	  //out[index+j] = im->rgb_data[index1 + chan];
	  if(im[index1 + chan] < min ) {min=im[index1 + chan]; cmin++;}
	  if(im[index1 + chan] > max ) {max=im[index1 + chan]; cmax++;}
	  out[index+j] = im[index1 + chan];
      }
  }
  printf("Input MIN=%f:%d MAX=%f:%d\n", min, cmin, max, cmax);
  return out;
}

/*extract from an RGB image the R, G of B channel*/
float *put_channel (int w, int h, float * im, float *in, int chan, float maxValue, float minValue)
{
  int i, j;
  int index, index1, cmin, cmax;
  float min, max;
  min=99.0;
  max=0.0;
  cmin=0; cmax=0;

  for (i = 0; i < h; i++) {
      index = i * w;
      for (j = 0; j < w; j++) {
	  index1 = (index + j) * 3;
	  if(in[index+j] < min ) {min=in[index+j]; cmin++;}
	  if(in[index+j] > max ) {max=in[index+j]; cmax++;}
	  if(in[index+j] > maxValue ) in[index+j] = maxValue;
	  if(in[index+j] < minValue ) in[index+j] = minValue;
	  im[index1 + chan] = in[index+j];
      }
  }
  printf("Converted MIN=%f:%d MAX=%f:%d\n", min, cmin, max, cmax);
  return in;
}

/*normalise PSF to 1*/
float *normpsf (float *psfch, int w, int h)
{
  int i, j;
  double sum;
  printf (" \tnormpsf psf dimensions %dx%d\n", w, h);
  sum = 0.0;
  for (j = 0; j < h; j++) {
    for (i = 0; i < w; i++) {
      sum += fabs(psfch[i + w * j]);
    }
  }
  printf (" \tnormpsf psf sum ready\n");
  for (j = 0; j < h; j++) {
    for (i = 0; i < w; i++) {
      psfch[i + w * j] = psfch[i + w * j] / sum;
    }
  }
  printf (" \tnormpsf psf norm ready\n");
  sum = 0.0;
  for (j = 0; j < h; j++) {
    for (i = 0; i < w; i++) {
      sum += psfch[i + w * j];
    }
  }
  printf (" \tnormpsf psf ready\n");
  return psfch;
}

#define MAXIMUMLIKELIHOOD 3
#define JANSENVANCITTERT 2
#define WIENER 1
#define JOHANNES 0

void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *ivoid, void *ovoid, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
	dt_iop_deconvolve_data_t *data = (dt_iop_deconvolve_data_t *)piece->data;
  const int rad = MIN(MAXR, ceilf(data->radius * roi_in->scale / piece->iscale));
 	if(rad == 0)
  {
    memcpy(ovoid, ivoid, sizeof(float)*3*roi_out->width*roi_out->height);
    return;
  }

  float *in  = (float *)ivoid;
  float *out = (float *)ovoid;

  float mat[2*(MAXR+1)*2*(MAXR+1)];
  const int wd = 2*rad+1;
  float *m = mat + rad*wd + rad;
  const float sigma2 = (2.5*2.5)*(data->radius*roi_in->scale/piece->iscale)*(data->radius*roi_in->scale/piece->iscale);
  //////////////////////////////////////////////////////////
  int channels[3];
  channels[0] = (data->L > 0) ? 1:0;
  channels[1] = (data->A > 0) ? 1:0;
  channels[2] = (data->B > 0) ? 1:0;

  int method = 0;
  method = data->method;
  if (method > 4) {
    method = JANSENVANCITTERT;
  }
  int iterations;
  iterations = data->iterations;
  if (iterations<=0 || iterations >=4095) {
    iterations = 1;
  }
  printf("METHOD %d ITERATIONS %d\n", method, iterations);
  printf("SIZES IN %dx%d OUT %dx%d SIGMA %f radius %f effective radius %f\n\n", roi_in->width, roi_in->height, roi_out->width, roi_out->height, 
	 sqrtf(sigma2/(2.5*2.5)), data->radius, data->radius*roi_in->scale/piece->iscale);
  //iterations = 2;
  if (method > 0) {
    pthread_mutex_lock(&clarity_mutex);

    Clarity_Dim3 imageDims; // Image dimensions
    Clarity_Dim3 kernelDims; // Kernel dimensions
    float *kernelImage;

    in  = (float *)ivoid;
    out = (float *)ovoid;
    memcpy(ovoid, ivoid, sizeof(float)*3*roi_out->width*roi_out->height);
    imageDims.x = roi_in->width;
    imageDims.y = roi_in->height;
    imageDims.z = 1;
    kernelDims.x =32;  kernelDims.y =32;  kernelDims.z=1;
    float *deconvolvedImage = (float *) malloc(sizeof(float)*imageDims.x*imageDims.y*imageDims.z);
    float *inputImage = (float *) malloc(sizeof(float)*imageDims.x*imageDims.y*imageDims.z);

    printf("Generate kernel\n");
    //kernelImage = Tst_GenerateGaussianKernel(&kernelDims, sqrtf(sigma2/(2.5*2.5)));
    // Create gaussian kernel. But can be external image, for example.
    // TODO - deconvolve in tiles. In theory can give better results.
    kernelImage = Tst_GenerateGaussianKernel(&kernelDims, data->radius*roi_in->scale/piece->iscale);
    Clarity_Register();
    for(int chan =0; chan<3; chan++) {
      if (channels[chan]) {
	printf("Get channel %d\n", chan);
	get_channel (imageDims.x, imageDims.y, in, chan, inputImage);
	// Now we are ready to apply a deconvolution algorithm.
	printf("Deconvolve\n");
	if (MAXIMUMLIKELIHOOD == method) {
	  printf("MaximumLikelihood sizes x=%d y=%d z=%d\n", imageDims.x, imageDims.y, imageDims.z);
	  Clarity_MaximumLikelihoodDeconvolve(inputImage, imageDims,
						kernelImage, kernelDims,
						deconvolvedImage, iterations);
	} else if (JANSENVANCITTERT == method) {
	  printf("JansenVanCittert sizes x=%d y=%d z=%d\n", imageDims.x, imageDims.y, imageDims.z);
	  Clarity_JansenVanCittertDeconvolve(inputImage, imageDims,
					    kernelImage, kernelDims,
					    deconvolvedImage, iterations);
	} else if (WIENER == method) {
	  printf("Wiener sizes x=%d y=%d z=%d K=%f\n", imageDims.x, imageDims.y, imageDims.z, data->snr);
	  Clarity_WienerDeconvolve(inputImage, imageDims, kernelImage, kernelDims,
				  deconvolvedImage, data->snr);
				  //deconvolvedImage, data->threshold);
	}
	printf("Save deconvolved channel\n");
	if (chan == 0) put_channel (imageDims.x, imageDims.y, out, deconvolvedImage, chan, 100.0f, 0.0f);
	else put_channel (imageDims.x, imageDims.y, out, deconvolvedImage, chan, 127.0f, -128.0f);
      }
    }
    Clarity_UnRegister();
    free(kernelImage);  
    free(inputImage);
    free(deconvolvedImage);
    pthread_mutex_unlock(&clarity_mutex);
    return;
  }
  //////////////////////////////////////////////////////////
  float weight = 0.0f;
  // init gaussian kernel
  for(int l=-rad;l<=rad;l++) for(int k=-rad;k<=rad;k++)
    weight += m[l*wd + k] = expf(- (l*l + k*k)/(2.f*sigma2));
  for(int l=-rad;l<=rad;l++) for(int k=-rad;k<=rad;k++)
    m[l*wd + k] /= weight;

  // gauss blur the image
#ifdef _OPENMP
  #pragma omp parallel for default(none) private(in, out) shared(m, ivoid, ovoid, roi_out, roi_in) schedule(static)
#endif
  for(int j=rad;j<roi_out->height-rad;j++)
  {
    in  = ((float *)ivoid) + 3*(j*roi_in->width  + rad);
    out = ((float *)ovoid) + 3*(j*roi_out->width + rad);
    for(int i=rad;i<roi_out->width-rad;i++)
    {
      for(int c=0;c<3;c++) out[c] = 0.0f;
      for(int l=-rad;l<=rad;l++) for(int k=-rad;k<=rad;k++)
        for(int c=0;c<3;c++) out[c] += m[l*wd+k]*in[3*(l*roi_in->width+k)+c];
      out += 3; in += 3;
    }
  }
  in  = (float *)ivoid;
  out = (float *)ovoid;

  // fill unsharpened border
  for(int j=0;j<rad;j++)
    memcpy(((float*)ovoid) + 3*j*roi_out->width, ((float*)ivoid) + 3*j*roi_in->width, 3*sizeof(float)*roi_out->width);
  for(int j=roi_out->height-rad;j<roi_out->height;j++)
    memcpy(((float*)ovoid) + 3*j*roi_out->width, ((float*)ivoid) + 3*j*roi_in->width, 3*sizeof(float)*roi_out->width);
#ifdef _OPENMP
  #pragma omp parallel for default(none) shared(in, out, roi_out, roi_in) schedule(static)
#endif
  for(int j=rad;j<roi_out->height-rad;j++)
  {
    for(int i=0;i<rad;i++)
      for(int c=0;c<3;c++) out[3*(roi_out->width*j + i) + c] = in[3*(roi_in->width*j + i) + c];
    for(int i=roi_out->width-rad;i<roi_out->width;i++)
      for(int c=0;c<3;c++) out[3*(roi_out->width*j + i) + c] = in[3*(roi_in->width*j + i) + c];
  }
#ifdef _OPENMP
  #pragma omp parallel for default(none) private(in, out) shared(data, ivoid, ovoid, roi_out, roi_in, channels) schedule(static)
#endif
  // subtract blurred image, if diff > thrs, add *amount to orginal image
  for(int j=0;j<roi_out->height;j++)
  { 
    in  = (float *)ivoid + j*3*roi_out->width;
    out = (float *)ovoid + j*3*roi_out->width;
	  
	  for(int i=0;i<roi_out->width;i++)
    {
      for(int c=0;c<3;c++)
      {
        const float diff = (in[c] - out[c]) * channels[c];
        if(fabsf(diff) > data->threshold)
        {
          const float detail = copysignf(fmaxf(fabsf(diff) - data->threshold, 0.0), diff);
          out[c] = fmaxf(0.0, in[c] + detail*data->amount);
        }
        else out[c] = in[c];
      }
      out += 3; in += 3;
    }
	}
}

static void
radius_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_deconvolve_params_t *p = (dt_iop_deconvolve_params_t *)self->params;
  p->radius = dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self);
}

static void
amount_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_deconvolve_params_t *p = (dt_iop_deconvolve_params_t *)self->params;
  p->amount = dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self);
}

static void
threshold_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_deconvolve_params_t *p = (dt_iop_deconvolve_params_t *)self->params;
  p->threshold = dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self);
}

static void
iterations_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_deconvolve_params_t *p = (dt_iop_deconvolve_params_t *)self->params;
  p->iterations = dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self);
}

static void
noise_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_deconvolve_params_t *p = (dt_iop_deconvolve_params_t *)self->params;
  p->snr = dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self);
}


static void
method_callback (GtkComboBox *box, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_deconvolve_gui_data_t *g = (dt_iop_deconvolve_gui_data_t *)self->gui_data;

  if(self->dt->gui->reset) return;
  dt_iop_deconvolve_params_t *p = (dt_iop_deconvolve_params_t *)self->params;
  unsigned int active = gtk_combo_box_get_active(box);
  //unsigned int method = gtk_combo_box_get_active(g->method);
  
  p->method = active & 0xf;
  printf("method_callback METHOD %d, %d\n", p->method, active);

//   GtkLabel  *label1, *label2, *label3;
//   GtkDarktableSlider *scale1, *scale2, *scale3;
//   GtkComboBox *method;
//   GtkDarktableSlider *snr, *num_iter;
//   GtkLabel  *label4, *label5, *label6, *label7;
//   GtkCheckButton *chan_L, *chan_A, *chan_B;

  gtk_widget_set_visible(GTK_WIDGET(g->label2), FALSE);
  gtk_widget_set_visible(GTK_WIDGET(g->scale2), FALSE);

  gtk_widget_set_visible(GTK_WIDGET(g->label3), FALSE);
  gtk_widget_set_visible(GTK_WIDGET(g->scale3), FALSE);

  gtk_widget_set_visible(GTK_WIDGET(g->label5), FALSE);
  gtk_widget_set_visible(GTK_WIDGET(g->snr), FALSE);

  gtk_widget_set_visible(GTK_WIDGET(g->label6), FALSE);
  gtk_widget_set_visible(GTK_WIDGET(g->num_iter), FALSE);

  gtk_widget_set_visible(GTK_WIDGET(g->label7), FALSE);
  gtk_widget_set_visible(GTK_WIDGET(g->chan_L), FALSE);
  gtk_widget_set_visible(GTK_WIDGET(g->chan_A), FALSE);
  gtk_widget_set_visible(GTK_WIDGET(g->chan_B), FALSE);
//////////////////////
  gtk_widget_set_no_show_all(GTK_WIDGET(g->label2), TRUE);
  gtk_widget_set_no_show_all(GTK_WIDGET(g->scale2), TRUE);

  gtk_widget_set_no_show_all(GTK_WIDGET(g->label3), TRUE);
  gtk_widget_set_no_show_all(GTK_WIDGET(g->scale3), TRUE);

  gtk_widget_set_no_show_all(GTK_WIDGET(g->label5), TRUE);
  gtk_widget_set_no_show_all(GTK_WIDGET(g->snr),    TRUE);

  gtk_widget_set_no_show_all(GTK_WIDGET(g->label6), TRUE);
  gtk_widget_set_no_show_all(GTK_WIDGET(g->num_iter), TRUE);

  gtk_widget_set_no_show_all(GTK_WIDGET(g->label7), TRUE);
  gtk_widget_set_no_show_all(GTK_WIDGET(g->chan_L), TRUE);
  gtk_widget_set_no_show_all(GTK_WIDGET(g->chan_A), TRUE);
  gtk_widget_set_no_show_all(GTK_WIDGET(g->chan_B), TRUE);

  if ( active == JOHANNES )
  {
    gtk_widget_set_no_show_all(GTK_WIDGET(g->label2), FALSE);
    gtk_widget_set_no_show_all(GTK_WIDGET(g->scale2), FALSE);

    gtk_widget_set_no_show_all(GTK_WIDGET(g->label3), FALSE);
    gtk_widget_set_no_show_all(GTK_WIDGET(g->scale3), FALSE);

    gtk_widget_show_all(GTK_WIDGET(g->label2));
    gtk_widget_show_all(GTK_WIDGET(g->scale2));

    gtk_widget_show_all(GTK_WIDGET(g->label3));
    gtk_widget_show_all(GTK_WIDGET(g->scale3));
  }
  else if ( active == WIENER )
  {
    gtk_widget_set_no_show_all(GTK_WIDGET(g->label5), FALSE);
    gtk_widget_set_no_show_all(GTK_WIDGET(g->snr), FALSE);
    gtk_widget_show_all(GTK_WIDGET(g->label5));
    gtk_widget_show_all(GTK_WIDGET(g->snr));
  }
  else if ( active == JANSENVANCITTERT )
  {
    gtk_widget_set_no_show_all(GTK_WIDGET(g->label6), FALSE);
    gtk_widget_set_no_show_all(GTK_WIDGET(g->num_iter), FALSE);
    gtk_widget_show_all(GTK_WIDGET(g->label6));
    gtk_widget_show_all(GTK_WIDGET(g->num_iter));
  }
  else if ( active == MAXIMUMLIKELIHOOD )
  {
    gtk_widget_set_no_show_all(GTK_WIDGET(g->label6), FALSE);
    gtk_widget_set_no_show_all(GTK_WIDGET(g->num_iter), FALSE);
    gtk_widget_show_all(GTK_WIDGET(g->label6));
    gtk_widget_show_all(GTK_WIDGET(g->num_iter));
  }
  dt_dev_add_history_item(darktable.develop, self);
}
#undef MAXIMUMLIKELIHOOD
#undef JANSENVANCITTERT
#undef WIENER
#undef JOHANNES

static void
toggle_L_callback (GtkToggleButton *toggle, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_deconvolve_params_t *p = (dt_iop_deconvolve_params_t *)self->params;
  int active = gtk_toggle_button_get_active(toggle);
  if (active) p->L = 1;
  else        p->L = 0;
  dt_dev_add_history_item(darktable.develop, self);
}

static void
toggle_A_callback (GtkToggleButton *toggle, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_deconvolve_params_t *p = (dt_iop_deconvolve_params_t *)self->params;
  int active = gtk_toggle_button_get_active(toggle);
  if (active) p->A = 1;
  else        p->A = 0;
  dt_dev_add_history_item(darktable.develop, self);
}

static void
toggle_B_callback (GtkToggleButton *toggle, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_deconvolve_params_t *p = (dt_iop_deconvolve_params_t *)self->params;
  int active = gtk_toggle_button_get_active(toggle);
  if (active) p->B = 1;
  else        p->B = 0;
  dt_dev_add_history_item(darktable.develop, self);
}

void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_deconvolve_params_t *p = (dt_iop_deconvolve_params_t *)p1;
#ifdef HAVE_GEGL
  fprintf(stderr, "[deconvolve] TODO: implement gegl version!\n");
  // pull in new params to gegl
#else
  dt_iop_deconvolve_data_t *d = (dt_iop_deconvolve_data_t *)piece->data;
  d->radius = p->radius;
  d->amount = p->amount;
  d->threshold = p->threshold;
  d->deconvdamping = p->deconvdamping;
  d->method        = p->method;
  d->iterations    = p->iterations;
  d->L             = p->L;
  d->A             = p->A;
  d->B             = p->B;
  d->snr           = p->snr;
#endif
}

void init_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
#ifdef HAVE_GEGL
  // create part of the gegl pipeline
  piece->data = NULL;
#else
  piece->data = malloc(sizeof(dt_iop_deconvolve_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
#endif
}

void cleanup_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
#ifdef HAVE_GEGL
  // clean up everything again.
  (void)gegl_node_remove_child(pipe->gegl, piece->input);
  // no free necessary, no data is alloc'ed
#else
  free(piece->data);
#endif
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_module_t *module = (dt_iop_module_t *)self;
  dt_iop_deconvolve_gui_data_t *g = (dt_iop_deconvolve_gui_data_t *)self->gui_data;
  dt_iop_deconvolve_params_t *p = (dt_iop_deconvolve_params_t *)module->params;
  dtgtk_slider_set_value(g->scale1, p->radius);
  dtgtk_slider_set_value(g->scale2, p->amount);
  dtgtk_slider_set_value(g->scale3, p->threshold);
  dtgtk_slider_set_value(g->snr, p->snr);
  dtgtk_slider_set_value(g->num_iter, p->iterations);
  gtk_combo_box_set_active(g->method, p->method);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->chan_L), p->L);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->chan_A), p->A);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->chan_B), p->B);
}
/*
typedef struct dt_iop_deconvolve_gui_data_t
{
  GtkDarktableSlider *scale1, *scale2, *scale3;
  GtkComboBox *method;
  GtkDarktableSlider *snr, *num_iter;
  GtkCheckButton *chan_L, *chan_A, *chan_B;
}
dt_iop_deconvolve_gui_data_t;

typedef struct dt_iop_deconvolve_data_t
{
  float radius, amount, threshold, deconvdamping;
  unsigned method : 4, iterations:12, L:1, A:1, B:1;
  float  snr;
}
dt_iop_deconvolve_data_t;
*/

void init(dt_iop_module_t *module)
{
  // module->data = malloc(sizeof(dt_iop_deconvolve_data_t));
  module->params = malloc(sizeof(dt_iop_deconvolve_params_t));
  module->default_params = malloc(sizeof(dt_iop_deconvolve_params_t));
  module->default_enabled = 0;
  module->priority = 549;
  module->params_size = sizeof(dt_iop_deconvolve_params_t);
  module->gui_data = NULL;
  dt_iop_deconvolve_params_t tmp = (dt_iop_deconvolve_params_t){0.5, 0.5, 0.004, 0.0, 0, 10, 1, 0, 0, 0.0001};
  memcpy(module->params, &tmp, sizeof(dt_iop_deconvolve_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_deconvolve_params_t));
}
/*
typedef struct dt_iop_deconvolve_params_t
{
  float radius, amount, threshold, deconvdamping;
  unsigned method : 4, iterations:12, L:1, A:1, B:1;
  float snr;
}
*/
void cleanup(dt_iop_module_t *module)
{
  free(module->gui_data);
  module->gui_data = NULL;
  free(module->params);
  module->params = NULL;
}

void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_deconvolve_gui_data_t));
  dt_iop_deconvolve_gui_data_t *g = (dt_iop_deconvolve_gui_data_t *)self->gui_data;
  dt_iop_deconvolve_params_t *p = (dt_iop_deconvolve_params_t *)self->params;

  self->widget = gtk_table_new(10, 6, FALSE);
  g->label1 = GTK_LABEL(gtk_label_new(_("radius")));
  g->label2 = GTK_LABEL(gtk_label_new(_("amount")));
  g->label3 = GTK_LABEL(gtk_label_new(_("threshold")));
  gtk_misc_set_alignment(GTK_MISC(g->label1), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(g->label2), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(g->label3), 0.0, 0.5);

  g->scale1 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 8.0000, 0.100, p->radius, 3));
  g->scale2 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 2.0000, 0.010, p->amount, 3));
  g->scale3 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 1.0000, 0.001, p->threshold, 3));


  g->label4 = GTK_LABEL(gtk_label_new(_("method")));
  gtk_misc_set_alignment(GTK_MISC(g->label4), 0.0, 0.5);

  g->method = GTK_COMBO_BOX(gtk_combo_box_new_text());
  gtk_combo_box_append_text(g->method, C_("method", "traditional"));
  gtk_combo_box_append_text(g->method, "wiener");
  gtk_combo_box_append_text(g->method, "jansenvancittert");
  gtk_combo_box_append_text(g->method, "maximumlikelihood");
  gtk_combo_box_set_active(g->method, p->method);

  gtk_table_attach(GTK_TABLE(self->widget), GTK_WIDGET(g->label4), 0, 2, 0, 1, GTK_EXPAND|GTK_FILL, 0, 0, 0);
  gtk_table_attach(GTK_TABLE(self->widget), GTK_WIDGET(g->method), 2, 6, 0, 1, GTK_EXPAND|GTK_FILL, 0, 0, 0);

  gtk_table_attach(GTK_TABLE(self->widget), GTK_WIDGET(g->label1), 0, 2, 1, 2, GTK_EXPAND|GTK_FILL, 0, 0, 0);
  gtk_table_attach(GTK_TABLE(self->widget), GTK_WIDGET(g->scale1), 2, 6, 1, 2, GTK_EXPAND|GTK_FILL, 0, 0, 0);

  gtk_table_attach(GTK_TABLE(self->widget), GTK_WIDGET(g->label2), 0, 2, 2, 3, GTK_EXPAND|GTK_FILL, 0, 0, 0);
  gtk_table_attach(GTK_TABLE(self->widget), GTK_WIDGET(g->scale2), 2, 6, 2, 3, GTK_EXPAND|GTK_FILL, 0, 0, 0);

  gtk_table_attach(GTK_TABLE(self->widget), GTK_WIDGET(g->label3), 0, 2, 3, 4, GTK_EXPAND|GTK_FILL, 0, 0, 0);
  gtk_table_attach(GTK_TABLE(self->widget), GTK_WIDGET(g->scale3), 2, 6, 3, 4, GTK_EXPAND|GTK_FILL, 0, 0, 0);

  g->label5 = GTK_LABEL(gtk_label_new(_("snr")));
  gtk_misc_set_alignment(GTK_MISC(g->label5), 0.0, 0.5);
  g->snr = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 1.0000, 0.0001, p->snr, 5));
  gtk_object_set(GTK_OBJECT(g->snr), "tooltip-text", _("Noise level."), (char *)NULL);
  gtk_table_attach(GTK_TABLE(self->widget), GTK_WIDGET(g->label5), 0, 2, 4, 5, GTK_EXPAND|GTK_FILL, 0, 0, 0);
  gtk_table_attach(GTK_TABLE(self->widget), GTK_WIDGET(g->snr   ), 2, 6, 4, 5, GTK_EXPAND|GTK_FILL, 0, 0, 0);

  g->label6 = GTK_LABEL(gtk_label_new(_("iteartions")));
  gtk_misc_set_alignment(GTK_MISC(g->label6), 0.0, 0.5);
  g->num_iter = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 4094, 1.0, p->iterations, 0));
  gtk_table_attach(GTK_TABLE(self->widget), GTK_WIDGET(g->label6), 0, 2, 5, 6, GTK_EXPAND|GTK_FILL, 0, 0, 0);
  gtk_table_attach(GTK_TABLE(self->widget), GTK_WIDGET(g->num_iter), 2, 6, 5, 6, GTK_EXPAND|GTK_FILL, 0, 0, 0);

  g->label7 = GTK_LABEL(gtk_label_new(_("Lab channel")));
  gtk_misc_set_alignment(GTK_MISC(g->label7), 0.0, 0.5);
  gtk_table_attach(GTK_TABLE(self->widget), GTK_WIDGET(g->label7), 0, 2, 6, 7, GTK_EXPAND|GTK_FILL, 0, 0, 0);
  g->chan_L = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("L")));
  gtk_object_set(GTK_OBJECT(g->chan_L), "tooltip-text", _("Perform action on L channel of Lab color space."), (char *)NULL);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->chan_L), (p->L));
  gtk_table_attach(GTK_TABLE(self->widget), GTK_WIDGET(g->chan_L), 2, 3, 6, 7, GTK_EXPAND|GTK_FILL, 0, 0, 0);

  g->chan_A = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("a")));
  gtk_object_set(GTK_OBJECT(g->chan_A), "tooltip-text", _("Perform action on a channel of Lab color space."), (char *)NULL);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->chan_A), (p->A));
  gtk_table_attach(GTK_TABLE(self->widget), GTK_WIDGET(g->chan_A), 3, 4, 6, 7, GTK_EXPAND|GTK_FILL, 0, 0, 0);

  g->chan_B = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("b")));
  gtk_object_set(GTK_OBJECT(g->chan_B), "tooltip-text", _("Perform action on b channel of Lab color space."), (char *)NULL);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->chan_B), (p->B));
  gtk_table_attach(GTK_TABLE(self->widget), GTK_WIDGET(g->chan_B), 4, 5, 6, 7, GTK_EXPAND|GTK_FILL, 0, 0, 0);


  g_signal_connect (G_OBJECT (g->scale1), "value-changed",
                    G_CALLBACK (radius_callback), self);
  g_signal_connect (G_OBJECT (g->scale2), "value-changed",
                    G_CALLBACK (amount_callback), self);
  g_signal_connect (G_OBJECT (g->scale3), "value-changed",
                    G_CALLBACK (threshold_callback), self);
  g_signal_connect (G_OBJECT (g->num_iter), "value-changed",
                    G_CALLBACK (iterations_callback), self);
  g_signal_connect (G_OBJECT (g->method), "changed",
		    G_CALLBACK (method_callback), self);
  g_signal_connect (G_OBJECT (g->chan_L), "toggled", 
		    G_CALLBACK (toggle_L_callback), self);
  g_signal_connect (G_OBJECT (g->chan_A), "toggled", 
		    G_CALLBACK (toggle_A_callback), self);
  g_signal_connect (G_OBJECT (g->chan_B), "toggled", 
		    G_CALLBACK (toggle_B_callback), self);
  g_signal_connect (G_OBJECT (g->snr), "value-changed",
                    G_CALLBACK (noise_callback), self);

  gtk_combo_box_set_active(g->method, p->method);
  self->gui_update(self);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

//#define MAXIMUMLIKELIHOOD 4
//#define JANSENVANCITTERT 3
//#define WIENER 2
#undef MAXR
