
#include "bauhaus/bauhaus.h"
#include "common/opencl.h"
#include "common/dtpthread.h"
#include "common/debug.h"
#include "control/control.h"
#include "develop/imageop.h"
#include "develop/tiling.h"
#include "develop/masks.h"
#include "common/gaussian.h"
#include "common/colorspaces.h"
#include "blend.h"
#include "freqsep.h"
#include "fft.h"

#define _FFT_MULTFR_
//#define _TIME_FFT_

fft_decompose_channels fs_get_channels_from_colorspace(dt_develop_blend_params_t *d, const dt_iop_colorspace_type_t cst)
{
  fft_decompose_channels resp = 0;
  if (d->fs_show_luma_chroma)
  {
    resp =  FFT_DECOMPOSE_CH1 | FFT_DECOMPOSE_CH2 | FFT_DECOMPOSE_CH3;
  }
  else if (d->fs_show_luma)
  {
    if (cst == iop_cs_Lab)
    {
      resp =  FFT_DECOMPOSE_CH1;
    }
    else
    {
      resp =  FFT_DECOMPOSE_CH1 | FFT_DECOMPOSE_CH2 | FFT_DECOMPOSE_CH3;
    }
  }
  else if (d->fs_show_chroma)
  {
    if (cst == iop_cs_Lab)
    {
      resp =  FFT_DECOMPOSE_CH2 | FFT_DECOMPOSE_CH3;
    }
    else
    {
      resp =  FFT_DECOMPOSE_CH1 | FFT_DECOMPOSE_CH2 | FFT_DECOMPOSE_CH3;
    }
  }
  else
  {
    if (cst == iop_cs_rgb)
    {
      if (d->fs_show_channel_1) resp |=  FFT_DECOMPOSE_CH1;
      if (d->fs_show_channel_2) resp |=  FFT_DECOMPOSE_CH2;
      if (d->fs_show_channel_3) resp |=  FFT_DECOMPOSE_CH3;
    }
    else
    {
      resp =  FFT_DECOMPOSE_CH1 | FFT_DECOMPOSE_CH2 | FFT_DECOMPOSE_CH3;
    }
  }

  return resp;
}

void fs_copy_in_to_ft(float *const in, const struct dt_iop_roi_t *const roi_in, float *const ft, const struct dt_iop_roi_t *const roi_ft, const int ch)
{
  const int rowsize = roi_in->width*ch*sizeof(float);
  const int w_ft = roi_ft->width*ch;
  const int w_in = roi_in->width*ch;

#ifdef _FFT_MULTFR_
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static)
#endif
#endif
  for (int y=0; y < roi_in->height; y++) // copy each row
  {
    memcpy(&ft[y*w_ft], &in[y*w_in], rowsize);
  }
}

void fs_copy_ft_to_in(const float *const ft, const struct dt_iop_roi_t *const roi_ft, float *const in, const struct dt_iop_roi_t *const roi_in, const int ch)
{
  const int rowsize = roi_in->width*ch*sizeof(float);
  const int w_ft = roi_ft->width*ch;
  const int w_in = roi_in->width*ch;

#ifdef _FFT_MULTFR_
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static)
#endif
#endif
  for (int y=0; y < roi_in->height; y++) // copy each row
  {
    memcpy(&in[y*w_in], &ft[y*w_ft], rowsize);
  }
}

void fs_copy_ft_to_out(float *const ft, const struct dt_iop_roi_t *const roi_ft, float *const out, const struct dt_iop_roi_t *const roi_out, const int ch)
{
  const int rowsize = roi_out->width*ch*sizeof(float);
  const int xoffs = roi_out->x - roi_ft->x;
  const int yoffs = roi_out->y - roi_ft->y;
  const int iwidth = roi_ft->width;

#ifdef _FFT_MULTFR_
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static)
#endif
#endif
  for (int y=0; y < roi_out->height; y++) // copy each row
  {
    size_t iindex = ((size_t)(y + yoffs) * iwidth + xoffs) * ch;
    size_t oindex = (size_t)y * roi_out->width * ch;
    float *in = (float *)ft + iindex;
    float *out1 = (float *)out + oindex;

    memcpy(out1, in, rowsize);
  }
}

void fs_copy_out_to_ft(float *const out, const struct dt_iop_roi_t *const roi_out, float *const ft, const struct dt_iop_roi_t *const roi_ft, const int ch)
{
  const int rowsize = roi_out->width*ch*sizeof(float);
  const int xoffs = roi_out->x - roi_ft->x;
  const int yoffs = roi_out->y - roi_ft->y;
  const int iwidth = roi_ft->width;

#ifdef _FFT_MULTFR_
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static)
#endif
#endif
  for (int y=0; y < roi_out->height; y++) // copy each row
  {
    size_t iindex = ((size_t)(y + yoffs) * iwidth + xoffs) * ch;
    size_t oindex = (size_t)y * roi_out->width * ch;
    float *in = (float *)ft + iindex;
    float *out1 = (float *)out + oindex;

    memcpy(in, out1, rowsize);
  }
}

// filters a FD image (InputR, InputI)
// excluded data is left on (OutputR, OutputI)
// nWidh, mHeight: image dimentions
// range1: high range
// range2: low range
// channels: channels to filter
// filter_type: Lowpass, Highpass, Bandpass, Bandblock
void fs_apply_filter(dt_develop_blend_params_t *d, float *const InputR, float *const InputI, float *OutputR, float *OutputI,
                      const int nWidh, const int mHeight,
                      const fft_decompose_channels channels, const fft_filter_type filter_type,
                      const dt_iop_colorspace_type_t cst, const int ch)
{
    float rng1 = 0;
    float rng2 = 0;

    if (filter_type == FFT_FILTER_TYPE_HIGHPASS_IDEAL)
    {
      const float max_rng1 = ((nWidh/2)*(nWidh/2) + (mHeight/2)*(mHeight/2));
      rng1 = (((d->fs_frequency_high)*max_rng1)/(100.f));
    }
    else if (filter_type == FFT_FILTER_TYPE_LOWPASS_IDEAL)
    {
      const float max_rng1 = ((nWidh/2)*(nWidh/2) + (mHeight/2)*(mHeight/2));
      rng1 = (((d->fs_frequency_low)*max_rng1)/(100.f));
    }
    else if (filter_type == FFT_FILTER_TYPE_BANDPASS_IDEAL)
    {
      const float max_rng1 = ((nWidh/2)*(nWidh/2) + (mHeight/2)*(mHeight/2));
      rng1 = (((d->fs_frequency_low)*max_rng1)/(100.f));
      rng2 = (((d->fs_frequency_high)*max_rng1)/(100.f));
    }
    else if (filter_type == FFT_FILTER_TYPE_LOWPASS_BUTTERWORTH)
    {
      const float max_rng1 = fmax(nWidh, mHeight)/*fmin(nWidh, mHeight)/2.f*/;
      rng1 = (((d->fs_frequency_low)*max_rng1)/(100.f));
    }
    else if (filter_type == FFT_FILTER_TYPE_HIGHPASS_BUTTERWORTH)
    {
      const float max_rng1 = fmax(nWidh, mHeight);
      rng1 = ((d->fs_frequency_high*max_rng1)/(100.f));
    }
    else if (filter_type == FFT_FILTER_TYPE_BANDPASS_BUTTERWORTH)
    {
      const float max_rng1 = fmax(nWidh, mHeight);
      rng1 = (((d->fs_frequency_low)*max_rng1)/(100.f));
      rng2 = (((d->fs_frequency_high)*max_rng1)/(100.f));
    }
    else if (filter_type == FFT_FILTER_TYPE_LOWPASS_GAUSSIAN)
    {
      const float max_rng1 = fmin(nWidh, mHeight)/2.f;
      rng1 = (((d->fs_frequency_low)*max_rng1)/(100.f));
    }
    else if (filter_type == FFT_FILTER_TYPE_HIGHPASS_GAUSSIAN)
    {
      const float max_rng1 = fmax(nWidh, mHeight);
      rng1 = ((d->fs_frequency_high*max_rng1)/(100.f));
    }
    else if (filter_type == FFT_FILTER_TYPE_BANDPASS_GAUSSIAN)
    {
      const float max_rng1 = fmin(nWidh, mHeight)/2.f;
      const float max_rng2 = fmax(nWidh, mHeight);
      rng1 = (((d->fs_frequency_low)*max_rng1)/(100.f));
      rng2 = (((d->fs_frequency_high)*max_rng2)/(100.f));
    }
    else if (filter_type == FFT_FILTER_TYPE_LOWPASS_SMOOTH)
    {
      const float max_rng1 = fmin(nWidh, mHeight);///10.f;

      rng1 = (d->fs_frequency*max_rng1)/(100.f);
      rng2 = (d->fs_frequency_range*max_rng1)/(100.f);
    }
    else if (filter_type == FFT_FILTER_TYPE_BARTLETT)
    {
      const float max_rng1 = fmax(nWidh, mHeight)*2;
      rng1 = (((d->fs_frequency_low)*max_rng1)/(100.f));
    }

    fft_filter_fft(InputR, InputI, OutputR, OutputI, nWidh, mHeight, rng1, rng2, d->fs_sharpness, channels, filter_type, cst, ch);
}

/*
void fs_filter_HLS_from_RGB(float *const o, dt_iop_roi_t *const roi_out, float *const filtered_ch,
                              const dt_iop_colorspace_type_t cst, const int ch, const fft_decompose_channels channels, const int forward)
{
#ifdef _FFT_MULTFR_
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static)
#endif
#endif
  for(size_t y = 0; y < roi_out->height; y++)
  {
    size_t oindex = (size_t)y * roi_out->width * ch;
    float *b = (float *)o + oindex;
    float *f = (float *)filtered_ch + oindex;


    for(size_t j = 0; j < roi_out->width * ch; j += ch)
    {
      float h, s, l;

      rgb2hsl(&b[j], &h, &s, &l);

      if (forward)
      {
        if (!(channels & FFT_DECOMPOSE_CH1))
        {
          f[j] = h;
          h = 0;
        }

        if (!(channels & FFT_DECOMPOSE_CH2))
        {
          f[j+1] = s;
          s = 0;
        }

        if (!(channels & FFT_DECOMPOSE_CH3))
        {
          f[j+2] = l;
          l = 0;
        }
      }
      else
      {
        if (!(channels & FFT_DECOMPOSE_CH1)) h = f[j];
        if (!(channels & FFT_DECOMPOSE_CH2)) s = f[j+1];
        if (!(channels & FFT_DECOMPOSE_CH3)) l = f[j+2];
      }

      hsl2rgb(&b[j], h, s, l);
    }
  }
}
*/

static float cbrt_5f(float f)
{
  uint32_t *p = (uint32_t *)&f;
  *p = *p / 3 + 709921077;
  return f;
}

static float cbrta_halleyf(const float a, const float R)
{
  const float a3 = a * a * a;
  const float b = a * (a3 + R + R) / (a3 + a3 + R);
  return b;
}

static float lab_f(const float x)
{
  const float epsilon = 216.0f / 24389.0f;
  const float kappa = 24389.0f / 27.0f;
  if(x > epsilon)
  {
    // approximate cbrtf(x):
    const float a = cbrt_5f(x);
    return cbrta_halleyf(a, x);
  }
  else
    return (kappa * x + 16.0f) / 116.0f;
}

static float lab_f_inv(const float x)
{
  const float epsilon = 0.20689655172413796; // cbrtf(216.0f/24389.0f);
  const float kappa = 24389.0f / 27.0f;
  if(x > epsilon)
    return x * x * x;
  else
    return (116.0f * x - 16.0f) / kappa;
}

void fs_XYZ2RGB(float *XYZ, float *RGB)
{
  /*  float xyz[9] = { 1.9624274, -0.6105343, -0.3413404,
                    -0.9787684,  1.9161415,  0.0334540,
                     0.0286869, -0.1406752,  1.3487655 };*/
/*  float xyz[9] = { 3.2404542, -1.5371385, -0.4985314,
                  -0.9692660,  1.8760108,  0.0415560,
                   0.0556434, -0.2040259,  1.0572252 };*/

  float xyz[9] = { 3.1338561, -1.6168667, -0.4906146,
                  -0.9787684,  1.9161415,  0.0334540,
                   0.0719453, -0.2289914,  1.4052427 };

  // xyz * XYZ
  RGB[0] = xyz[0] * XYZ[0] + xyz[1] * XYZ[1] + xyz[2] * XYZ[2];
  RGB[1] = xyz[3] * XYZ[0] + xyz[4] * XYZ[1] + xyz[5] * XYZ[2];
  RGB[2] = xyz[6] * XYZ[0] + xyz[7] * XYZ[1] + xyz[8] * XYZ[2];

}

void fs_RGB2XYZ(float *RGB, float *XYZ)
{
/*  float xyz[9] = {0.6097559,  0.2052401,  0.1492240,
                  0.3111242,  0.6256560 , 0.0632197,
                  0.0194811,  0.0608902,  0.7448387};*/
/*  const float xyz[9] = { 0.4124564, 0.3575761, 0.1804375,
                         0.2126729, 0.7151522, 0.0721750,
                         0.0193339, 0.1191920, 0.9503041 };*/

  const float xyz[9] = { 0.4360747,  0.3850649,  0.1430804,
                          0.2225045,  0.7168786,  0.0606169,
                          0.0139322,  0.0971045,  0.7141733 };


  // xyz * XYZ
  XYZ[0] = xyz[0] * RGB[0] + xyz[1] * RGB[1] + xyz[2] * RGB[2];
  XYZ[1] = xyz[3] * RGB[0] + xyz[4] * RGB[1] + xyz[5] * RGB[2];
  XYZ[2] = xyz[6] * RGB[0] + xyz[7] * RGB[1] + xyz[8] * RGB[2];

}

void fs_XYZ2Lab(const float *XYZ, float *Lab) // dt_XYZ_to_Lab
{
  const float d50[3] = { 0.9642, 1.0, 0.8249 };
  const float f[3] = { lab_f(XYZ[0] / d50[0]), lab_f(XYZ[1] / d50[1]), lab_f(XYZ[2] / d50[2]) };
  Lab[0] = 116.0f * f[1] - 16.0f;
  Lab[1] = 500.0f * (f[0] - f[1]);
  Lab[2] = 200.0f * (f[1] - f[2]);
}

void fs_Lab2XYZ(const float *Lab, float *XYZ) // dt_Lab_to_XYZ
{
  const float d50[3] = { 0.9642, 1.0, 0.8249 };
  const float fy = (Lab[0] + 16.0f) / 116.0f;
  const float fx = Lab[1] / 500.0f + fy;
  const float fz = fy - Lab[2] / 200.0f;
  XYZ[0] = d50[0] * lab_f_inv(fx);
  XYZ[1] = d50[1] * lab_f_inv(fy);
  XYZ[2] = d50[2] * lab_f_inv(fz);
}

void fs_RGB2Lab(float *RGB, float *Lab)
{
  float XYZ[3] = {0};

  fs_RGB2XYZ(RGB, XYZ);

  fs_XYZ2Lab(XYZ, Lab);
}

void fs_Lab2RGB(float *Lab, float *RGB)
{
  float XYZ[3] = {0};

  fs_Lab2XYZ(Lab, XYZ);

  fs_XYZ2RGB(XYZ, RGB);
}

void fs_filter_Lab_from_RGB(float *const o, dt_iop_roi_t *const roi_out, float *const filtered_ch,
                              const dt_iop_colorspace_type_t cst, const int ch, const fft_decompose_channels channels, const int forward)
{
#ifdef _FFT_MULTFR_
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static)
#endif
#endif
  for(size_t y = 0; y < roi_out->height; y++)
  {
    size_t oindex = (size_t)y * roi_out->width * ch;
    float *b = (float *)o + oindex;
    float *f = (float *)filtered_ch + oindex;


    for(size_t j = 0; j < roi_out->width * ch; j += ch)
    {
      float Lab[3] = {0};

      fs_RGB2Lab(&b[j], Lab);

      if (forward)
      {
        if (!(channels & FFT_DECOMPOSE_CH1))
        {
          f[j] = Lab[0];
          Lab[0] = 0;
        }

        if (!(channels & FFT_DECOMPOSE_CH2))
        {
          f[j+1] = Lab[1];
          Lab[1] = 0;
        }

        if (!(channels & FFT_DECOMPOSE_CH3))
        {
          f[j+2] = Lab[2];
          Lab[2] = 0;
        }
      }
      else
      {
        if (!(channels & FFT_DECOMPOSE_CH1)) Lab[0] = f[j];
        if (!(channels & FFT_DECOMPOSE_CH2)) Lab[1] = f[j+1];
        if (!(channels & FFT_DECOMPOSE_CH3)) Lab[2] = f[j+2];
      }

      fs_Lab2RGB(Lab, &b[j]);
    }
  }
}

void fs_filter_RGB_from_Lab(float *const o, dt_iop_roi_t *const roi_out, float *const filtered_ch,
                              const dt_iop_colorspace_type_t cst, const int ch, const fft_decompose_channels channels, const int forward)
{
#ifdef _FFT_MULTFR_
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static)
#endif
#endif
  for(size_t y = 0; y < roi_out->height; y++)
  {
    size_t oindex = (size_t)y * roi_out->width * ch;
    float *b = (float *)o + oindex;
    float *f = (float *)filtered_ch + oindex;


    for(size_t j = 0; j < roi_out->width * ch; j += ch)
    {
      float Lab[3] = {0};

      fs_Lab2RGB(&b[j], Lab);

      if (forward)
      {
        if (!(channels & FFT_DECOMPOSE_CH1))
        {
          f[j] = Lab[0];
          Lab[0] = 0;
        }

        if (!(channels & FFT_DECOMPOSE_CH2))
        {
          f[j+1] = Lab[1];
          Lab[1] = 0;
        }

        if (!(channels & FFT_DECOMPOSE_CH3))
        {
          f[j+2] = Lab[2];
          Lab[2] = 0;
        }
      }
      else
      {
        if (!(channels & FFT_DECOMPOSE_CH1)) Lab[0] = f[j];
        if (!(channels & FFT_DECOMPOSE_CH2)) Lab[1] = f[j+1];
        if (!(channels & FFT_DECOMPOSE_CH3)) Lab[2] = f[j+2];
      }

      fs_RGB2Lab(Lab, &b[j]);
    }
  }
}

int fs_convert_from_to_colorspace(dt_develop_blend_params_t *d, float * b, dt_iop_roi_t * roi_out, float * filtered_ch,
                                    const dt_iop_colorspace_type_t cst, const int ch, const int forward)
{
  int converted = 0;

  if (cst == iop_cs_Lab && (d->fs_show_channel_1 || d->fs_show_channel_2 || d->fs_show_channel_3))
  {
    int channels = 0;
    if (d->fs_show_channel_1) channels |=  FFT_DECOMPOSE_CH1;
    if (d->fs_show_channel_2) channels |=  FFT_DECOMPOSE_CH2;
    if (d->fs_show_channel_3) channels |=  FFT_DECOMPOSE_CH3;

    if (forward)
      memset(filtered_ch, 0, roi_out->height*roi_out->width*ch*sizeof(float));

    fs_filter_RGB_from_Lab(b, roi_out, filtered_ch, /*filter_by,*/ cst, ch, channels, forward);

    converted = 1;
  }

  if (cst == iop_cs_rgb && (d->fs_show_luma || d->fs_show_chroma))
  {
    int channels = 0;
    if (d->fs_show_luma) channels =  FFT_DECOMPOSE_CH1;
    if (d->fs_show_chroma) channels =  FFT_DECOMPOSE_CH2 | FFT_DECOMPOSE_CH3;
//    if (d->fs_show_luma) channels =  FFT_DECOMPOSE_CH3;
//    if (d->fs_show_chroma) channels =  FFT_DECOMPOSE_CH1 | FFT_DECOMPOSE_CH2;

    if (forward)
      memset(filtered_ch, 0, roi_out->height*roi_out->width*ch*sizeof(float));

    fs_filter_Lab_from_RGB(b, roi_out, filtered_ch, cst, ch, channels, forward);
//    fs_filter_HLS_from_RGB(b, roi_out, filtered_ch, cst, ch, channels, forward);

    converted = 1;
  }

  return converted;
}

// adjust the exposure of the frequency layer, for display purpuses only
// the image will be recomposed without the exposure change
void dt_fs_freqlayer_exposure(void *ivoid, const int width, const int height, const float exposure, const dt_iop_colorspace_type_t cst, const int ch)
{
  if (exposure == 0) return;

    const float scale = (exposure >= 0.f) ? (1.0f + exposure): (1.0f/(1.0f + fabs(exposure)));


  if (cst == iop_cs_rgb)
  {
#ifdef _FFT_MULTFR_
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(ivoid) schedule(static)
#endif
#endif
    for(int k = 0; k < height; k++)
    {
      const __m128 s = _mm_set1_ps(scale);
      float *out = ((float *)ivoid) + (size_t)ch * k * width;
      for(int j = 0; j < width; j++, out += ch)
        _mm_store_ps(out, _mm_mul_ps(_mm_load_ps(out), s));
    }
  }
  else
  {
    const int width1 = width*ch;

    for (int y=0; y < height; y++)
    {
      float *out = ((float *)ivoid) + (y*width1);

      for (int x=0; x < width; x ++, out += ch)
      {
        if (out[0] != 0)
        {
               const float L = out[0] * scale;
                const float L_ab = fabs(L / out[0]);

                out[0] = L;
                out[1] *= L_ab;
                out[2] *= L_ab;
        }
        else
        {
          out[0] += exposure*10;
          out[1] *= exposure*10;
          out[2] *= exposure*10;
        }
      }
    }
  }
}

void dt_fs_freqlayer_lighten(void *ivoid, const int width, const int height, const float exposure, const float clip_percent,
                              const dt_iop_colorspace_type_t cst, const int ch)
{
  /*
#ifdef _FFT_MULTFR_
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(ivoid) schedule(static)
#endif
#endif
  for(int k = 0; k < height; k++)
  {
    const float xx[4] = {white, white, white, 0};
    const __m128 whitev = _mm_load_ps(xx);
    float *out = ((float *)ivoid) + (size_t)ch * k * width;
    for(int j = 0; j < width; j++, out += ch)
      _mm_store_ps(out, _mm_add_ps(_mm_load_ps(out), whitev));
  }
*/

  const int width1 = width*ch;
  float min_rng = 0;
  float max_rng = 0;
  if (cst == iop_cs_rgb)
  {
    min_rng = .001f;
    max_rng = .9f;
  }
  else
  {
    min_rng = .01f;
    max_rng = 100.f;
  }

  float min1 = INFINITY;
  float max1 = -INFINITY;
  float min2 = INFINITY;
  float max2 = -INFINITY;
  float min3 = INFINITY;
  float max3 = -INFINITY;
/*  int nblack1 = 0;
  int nblack2 = 0;
  int nblack3 = 0;
  int nwhite1 = 0;
  int nwhite2 = 0;
  int nwhite3 = 0;*/

  // get range values
  for (int y=0; y < height; y++)
  {
    float *out = ((float *)ivoid) + (y*width1);

    for (int x=0; x < width; x ++, out += ch)
    {
      min1 = fmin(min1, out[0]);
      max1 = fmax (max1, out[0]);

      min2 = fmin(min2, out[1]);
      max2 = fmax (max2, out[1]);

      min3 = fmin(min3, out[2]);
      max3 = fmax (max3, out[2]);

/*      if (out[0] < 0) nblack1++;
      if (out[1] < 0) nblack2++;
      if (out[2] < 1) nblack3++;
      if (out[0] > 0) nwhite1++;
      if (out[1] > 0) nwhite2++;
      if (out[2] > 0) nwhite3++;*/
    }
  }

/*  fprintf(stderr, "[fftw3_dt_FFT_R_Forward] min1=%f, max1=%f\n", min1, max1);
  fprintf(stderr, "[fftw3_dt_FFT_R_Forward] min2=%f, max2=%f\n", min2, max2);
  fprintf(stderr, "[fftw3_dt_FFT_R_Forward] min3=%f, max3=%f\n", min3, max3);
  fprintf(stderr, "[fftw3_dt_FFT_R_Forward] nblack1=%i, nblack2=%i, nblack3=%i\n", nblack1, nblack2, nblack3);
  fprintf(stderr, "[fftw3_dt_FFT_R_Forward] nwhite1=%i, nwhite2=%i, nwhite3=%i\n", nwhite1, nwhite2, nwhite3);
*/
  const float percent = clip_percent/100.f;
  const float black1 = fmin(min1 + (max1-min1)*percent, 0.f);
  const float black2 = fmin(min2 + (max2-min2)*percent, 0.f);
  const float black3 = fmin(min3 + (max3-min3)*percent, 0.f);

  float maxa = 0;
  float mina = 0;
  float a = 0;
  float b = 0;
  if (cst == iop_cs_rgb)
  {
    maxa = fmax(max1, fmax(max2, max3));
    mina = fmin(fmin(black1, fmin(black2, black3)), 0.f);
    a = (max_rng-min_rng)/(maxa-mina);
    b = max_rng - (a * maxa);

//  fprintf(stderr, "[fftw3_dt_FFT_R_Forward] black1=%f, black2=%f, black3=%f\n", black1, black2, black3);
//  fprintf(stderr, "[fftw3_dt_FFT_R_Forward] maxa-mina=%f---------------\n", maxa-mina);

  // normalize
    for (int y=0; y < height; y++)
    {
      float *out = ((float *)ivoid) + (y*width1);

      for (int x=0; x < width; x ++, out += ch)
      {
        if (out[0] < black1) out[0] = min_rng;
        else out[0] = (a * out[0] + b);
        if (out[1] < black2) out[1] = min_rng;
        else out[1] = (a * out[1] + b);
        if (out[2] < black3) out[2] = min_rng;
        else out[2] = (a * out[2] + b);



      }
    }
  }
  else
  {
//    maxa = fmax(max1, fmax(max2, max3));
//    mina = fmin(fmin(black1, fmin(black2, black3)), 0.f);
    maxa = max1;
    mina = fmin(black1, 0.f);
    a = (max_rng-min_rng)/(maxa-mina);
    b = max_rng - (a * maxa);

/*    fprintf(stderr, "[fftw3_dt_FFT_R_Forward] black1=%f, black2=%f, black3=%f\n", black1, black2, black3);
    fprintf(stderr, "[fftw3_dt_FFT_R_Forward] maxa-mina=%f---------------\n", maxa-mina);
    fprintf(stderr, "[fftw3_dt_FFT_R_Forward] a=%f, b=%f\n", a, b);
*/
    if (max1 != 0)
    {
      for (int y=0; y < height; y++)
      {
        float *out = ((float *)ivoid) + (y*width1);

        for (int x=0; x < width; x ++, out += ch)
        {
          if (out[0] < black1) out[0] = min_rng;
          else out[0] = (a * out[0] + b);
        }
      }
    }
    else
    {
      for (int y=0; y < height; y++)
      {
        float *out = ((float *)ivoid) + (y*width1);

        for (int x=0; x < width; x ++, out += ch)
        {
          out[0] = ((max_rng - min_rng)/2.f);
        }
      }
    }
  }

}

void dt_develop_freqsep_preprocess(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, void *ivoid,
                              void *ovoid, const struct dt_iop_roi_t *roi_in, const struct dt_iop_roi_t *roi_out)
{
  dt_develop_blend_params_t *d = (dt_develop_blend_params_t *)piece->blendop_data;

  if(!d) return;
  if(!(d->fs_filter_type > 0)) return;

  const int xoffs = roi_out->x - roi_in->x;
  const int yoffs = roi_out->y - roi_in->y;

  /* we can only handle frequency separation if roi_out and roi_in have the same scale and
     if roi_out fits into the area given by roi_in */
  if(roi_out->scale != roi_in->scale || xoffs < 0 || yoffs < 0
     || ((xoffs > 0 || yoffs > 0)
         && (roi_out->width + xoffs > roi_in->width || roi_out->height + yoffs > roi_in->height)))
  {
    dt_control_log(_("skipped frequency separation in module '%s': roi's do not match"), self->op);
    return;
  }

  float *in = (float *)ivoid;
  float *out = (float *)ovoid;
  const int ch = piece->colors;
  const dt_iop_colorspace_type_t cst = dt_iop_module_colorspace(self);
  const fft_decompose_channels channels = fs_get_channels_from_colorspace(d, cst);


  d->fs_roi_tF1 = *roi_in;
  // save image width and height as the nearest >= pow2()
  d->fs_roi_tF1.width = fft_convert_pow2(roi_in->width);
  d->fs_roi_tF1.height = fft_convert_pow2(roi_in->height);

  const int buffersize = d->fs_roi_tF1.width * d->fs_roi_tF1.height * ch * sizeof(float);

#ifdef _TIME_FFT_
  struct timeval tm1,tm2;
  gettimeofday(&tm1,NULL);
#endif

  // allocate space for decomposed image
  d->tF1 = dt_alloc_align(64, buffersize);
  d->tF2 = dt_alloc_align(64, buffersize);
  d->tF3 = dt_alloc_align(64, buffersize);
  float *tF4 = dt_alloc_align(64, buffersize);
  float *tF5 = dt_alloc_align(64, buffersize);

  // allocate space for backup original image
  d->fs_ivoid = dt_alloc_align(64, (size_t)roi_in->width * roi_in->height * ch * sizeof(float));

  if (d->tF1 == NULL || d->tF2 == NULL || d->fs_ivoid == NULL || d->tF3 == NULL || tF4 == NULL || tF5 == NULL)
  {
    fprintf(stderr, "[dt_develop_freqsep_preprocess] NULL buffer for FFT!!!\n");

    if (d->tF1)
    {
      dt_free_align(d->tF1);
      d->tF1 = NULL;
    }
    if (d->tF2)
    {
      dt_free_align(d->tF2);
      d->tF2 = NULL;
    }
    if (d->tF3)
    {
      dt_free_align(d->tF3);
      d->tF3 = NULL;
    }
    if (d->fs_ivoid)
    {
      dt_free_align(d->fs_ivoid);
      d->fs_ivoid = NULL;
    }
    goto cleanup;
  }

  // backup original image
  memcpy(d->fs_ivoid, in, (size_t)roi_in->width * roi_in->height * ch * sizeof(float));
  d->fs_roi_ivoid = *roi_in;

  // copy the input image (ivoid) as the FFT real part in tF4
  fs_copy_in_to_ft(in, roi_in, tF4, &d->fs_roi_tF1, ch);

  fs_convert_from_to_colorspace(d, tF4, &d->fs_roi_tF1, d->tF3, cst, ch, 1);

  // decompose the image into (tF4, tF5)
  fft_FFT2D_R_Forward(tF4, tF5, d->fs_roi_tF1.width, d->fs_roi_tF1.height,
      channels, cst, ch);

  // apply filter to (tF4, tF5)
  // (tF1, tF2) will store the complementary data (excluded by the filter)
  fs_apply_filter(d, tF4, tF5, d->tF1, d->tF2, d->fs_roi_tF1.width, d->fs_roi_tF1.height,
      channels, d->fs_filter_type, cst, ch);

  if (d->fs_invert_freq_layer)
  {
    float *tmp = tF5;
    tF5 = d->tF2;
    d->tF2 = tmp;

    tmp = tF4;
    tF4 = d->tF1;
    d->tF1 = tmp;
  }

  // recompose the filtered image (tF4, tF5), so it can be edited/displayed
  fft_FFT2D_R_Inverse(tF4, tF5, d->fs_roi_tF1.width, d->fs_roi_tF1.height,
      channels, cst, ch);

//  fs_convert_from_to_colorspace(d, tF4, &d->fs_roi_tF1, d->tF3, cst, ch, 1);

  // copy tF4 to input for the parent module
  fs_copy_ft_to_in(tF4, &d->fs_roi_tF1, in, roi_in, ch);
  fs_copy_ft_to_out(tF4, &d->fs_roi_tF1, out, roi_out, ch);

#ifdef _TIME_FFT_
  gettimeofday(&tm2,NULL);
  float perf = (tm2.tv_sec-tm1.tv_sec)*1000.0f + (tm2.tv_usec-tm1.tv_usec)/1000.0f;
  printf("time spent dt_develop_freqsep_preprocess: %.4f\n",perf/100.0f);
#endif

cleanup:
  // release buffers
  if (tF4)
  {
    dt_free_align(tF4);
  }
  if (tF5)
  {
    dt_free_align(tF5);
  }

  return;
}

void dt_develop_freqsep_postprocess(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, void *ivoid,
                                          void *ovoid, const struct dt_iop_roi_t *roi_in, const struct dt_iop_roi_t *roi_out)
{
  dt_develop_blend_params_t *d = (dt_develop_blend_params_t *)piece->blendop_data;

  if(!d) return;
  if(!(d->fs_filter_type > 0)) return;
  if (d->tF1 == NULL || d->tF2 == NULL || d->fs_ivoid == NULL)
  {
    fprintf(stderr, "[dt_develop_freqsep_postprocess] NULL buffer for FFT!!!\n");
    goto cleanup;
  }

  const int xoffs = roi_out->x - roi_in->x;
  const int yoffs = roi_out->y - roi_in->y;

  /* we can only handle frequency separation if roi_out and roi_in have the same scale and
     if roi_out fits into the area given by roi_in */
  if(roi_out->scale != roi_in->scale || xoffs < 0 || yoffs < 0
     || ((xoffs > 0 || yoffs > 0)
         && (roi_out->width + xoffs > roi_in->width || roi_out->height + yoffs > roi_in->height)))
  {
    dt_control_log(_("skipped frequency separation in module '%s': roi's do not match"), self->op);
    goto cleanup;
  }

  float *in = (float *)ivoid;
  float *out = (float *)ovoid;
  const int ch = piece->colors;
  const dt_iop_colorspace_type_t cst = dt_iop_module_colorspace(self);
  const fft_decompose_channels channels = fs_get_channels_from_colorspace(d, cst);
  const int buffersize = d->fs_roi_tF1.width * d->fs_roi_tF1.height * ch * sizeof(float);

#ifdef _TIME_FFT_
  struct timeval tm1,tm2;
  gettimeofday(&tm1,NULL);
#endif

  float *tF4 = dt_alloc_align(64, buffersize);
  g_assert(tF4 != NULL);
  float *tF5 = dt_alloc_align(64, buffersize);
  g_assert(tF5 != NULL);

  // if the user wants to see just the frequency layer, just copy ivoid to ovoid
  if (d->fs_preview == DEVELOP_FS_PREVIEW_FREQLAY)
  {
    fs_copy_ft_to_out(in, roi_in, out, roi_out, ch);

    if (d->fs_lighten_freq_layer)
    {
      dt_fs_freqlayer_lighten(out, roi_out->width, roi_out->height, d->fs_freqlay_exposure, d->fs_clip_percent, cst, ch);
    }
    dt_fs_freqlayer_exposure(out, roi_out->width, roi_out->height, d->fs_freqlay_exposure, cst, ch);
  }
  else if (d->fs_preview == DEVELOP_FS_PREVIEW_FINAL_IMAGE)
  {
    // copy the parent module output image (ovoid) as the FFT real part in tF4
    fs_copy_out_to_ft(out, roi_out, tF4, &d->fs_roi_tF1, ch);

//    fs_convert_from_to_colorspace(d, tF4, &d->fs_roi_tF1, d->tF3, cst, ch, 0);

    // decompose (again) the image (tF4, tF5)
    fft_FFT2D_R_Forward(tF4, tF5, d->fs_roi_tF1.width, d->fs_roi_tF1.height,
        channels, cst, ch);

    // apply inverse filter to (tF4, tF5) to merge it with (tF1, tF2)
    fft_recompose_image(tF4, tF5, d->tF1, d->tF2, d->fs_roi_tF1.width, d->fs_roi_tF1.height, ch);

    // recompose back (tF4, tF5) and get the final image
    fft_FFT2D_R_Inverse(tF4, tF5, d->fs_roi_tF1.width, d->fs_roi_tF1.height,
        channels, cst, ch);

    fs_convert_from_to_colorspace(d, tF4, &d->fs_roi_tF1, d->tF3, cst, ch, 0);

    // copy tF4 to output
    fs_copy_ft_to_out(tF4, &d->fs_roi_tF1, out, roi_out, ch);
  }
  else if (d->fs_preview == DEVELOP_FS_PREVIEW_FREQLAY_CHNG)
  {
    if (d->fs_lighten_freq_layer)
    {
      dt_fs_freqlayer_lighten(out, roi_out->width, roi_out->height, d->fs_freqlay_exposure, d->fs_clip_percent, cst, ch);
    }
    dt_fs_freqlayer_exposure(out, roi_out->width, roi_out->height, d->fs_freqlay_exposure, cst, ch);
  }

  // restore the original image into ivoid, we have abuse it long enough
  memcpy(in, d->fs_ivoid, (size_t)roi_in->width * roi_in->height * ch * sizeof(float));

  // at this point we can free the buffers
  if (tF4)
  {
    dt_free_align(tF4);
  }
  if (tF5)
  {
    dt_free_align(tF5);
  }

#ifdef _TIME_FFT_
  gettimeofday(&tm2,NULL);
  float perf = (tm2.tv_sec-tm1.tv_sec)*1000.0f + (tm2.tv_usec-tm1.tv_usec)/1000.0f;
  printf("time spent dt_develop_freqsep_postprocess: %.4f\n",perf/100.0f);
#endif

cleanup:
  if (d->tF1)
  {
    dt_free_align(d->tF1);
    d->tF1 = NULL;
  }
  if (d->tF2)
  {
    dt_free_align(d->tF2);
    d->tF2 = NULL;
  }
  if (d->tF3)
  {
    dt_free_align(d->tF3);
    d->tF3 = NULL;
  }
  if (d->fs_ivoid)
  {
    dt_free_align(d->fs_ivoid);
    d->fs_ivoid = NULL;
  }
}

int dt_develop_freqsep_preprocess_tiling(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, void *ivoid,
                              	  	  	  	  void *ovoid, const struct dt_iop_roi_t *roi_in, const struct dt_iop_roi_t *roi_out)
{
	dt_develop_freqsep_preprocess(self, piece, ivoid, ovoid, roi_in, roi_out);
	
	return TRUE;
}

int dt_develop_freqsep_postprocess_tiling(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, void *ivoid,
                              void *ovoid, const struct dt_iop_roi_t *roi_in, const struct dt_iop_roi_t *roi_out)
{
	dt_develop_freqsep_postprocess(self, piece, ivoid, ovoid, roi_in, roi_out);
	
	return TRUE;
}

#ifdef HAVE_OPENCL
int dt_develop_freqsep_preprocess_tiling_cl(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, void *ivoid,
                              void *ovoid, const struct dt_iop_roi_t *roi_in, const struct dt_iop_roi_t *roi_out)
{
	//fprintf(stderr, "[dt_develop_freqsep_preprocess_tiling_cl]\n");
	return TRUE;
}

int dt_develop_freqsep_postprocess_tiling_cl(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, void *ivoid,
                              void *ovoid, const struct dt_iop_roi_t *roi_in, const struct dt_iop_roi_t *roi_out)
{
	//fprintf(stderr, "[dt_develop_freqsep_postprocess_tiling_cl]\n");
	return TRUE;
}

  
int dt_develop_freqsep_preprocess_cl(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                                cl_mem dev_in, cl_mem dev_out, const struct dt_iop_roi_t *roi_in,
                                const struct dt_iop_roi_t *roi_out)
{
	//fprintf(stderr, "[dt_develop_freqsep_preprocess_cl]\n");
	return TRUE;
}

int dt_develop_freqsep_postprocess_cl(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                                cl_mem dev_in, cl_mem dev_out, const struct dt_iop_roi_t *roi_in,
                                const struct dt_iop_roi_t *roi_out)
{
	//fprintf(stderr, "[dt_develop_freqsep_postprocess_cl]\n");
	return TRUE;
}
#endif

