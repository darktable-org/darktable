
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

// filters a FD image (InputR, InputI)
// excluded data is left on (OutputR, OutputI)
// nWidh, mHeight: image dimentions
// range1: high range
// range2: low range
// channels: channels to filter
// filter_type: Lowpass, Highpass, Bandpass, Bandblock
void fs_apply_filter(dt_develop_blend_params_t *d, float *const InputR, float *const InputI, float *OutputR, float *OutputI,
                      const int nWidh, const int mHeight,
                      /*const float range1, const float range2, const int sharpness,*/ const fft_decompose_channels channels, const fft_filter_type filter_type,
                      const dt_iop_colorspace_type_t cst, const int ch)
{
    float rng1 = 0;
    float rng2 = 0;

    if (filter_type == FFT_FILTER_TYPE_LOWPASS_BUTTERWORTH)
    {
      const float max_rng = fmin(nWidh, mHeight)/2.f;
      rng1 = (((100.f-d->fs_frequency_low)*max_rng)/(100.f));
    }
    else if (filter_type == FFT_FILTER_TYPE_HIGHPASS_BUTTERWORTH)
    {
      const float max_rng = fmax(nWidh, mHeight);
      rng1 = ((d->fs_frequency_high*max_rng)/(100.f));
    }
    else if (filter_type == FFT_FILTER_TYPE_LOWPASS_GAUSSIAN)
    {
      const float max_rng = fmin(nWidh, mHeight)/2.f;
      rng1 = (((100.f-d->fs_frequency_low)*max_rng)/(100.f));
    }
    else if (filter_type == FFT_FILTER_TYPE_HIGHPASS_GAUSSIAN)
    {
      const float max_rng = fmax(nWidh, mHeight);
      rng1 = ((d->fs_frequency_high*max_rng)/(100.f));
    }
    else if (filter_type == FFT_FILTER_TYPE_HIGHPASS_SMOOTH)
    {
      const float max_rng = fmax(nWidh, mHeight);
      rng1 = (d->fs_frequency*max_rng)/(100.f);
      rng2 = (d->fs_frequency_range*max_rng)/(100.f);
    }

    fft_filter_fft(InputR, InputI, OutputR, OutputI, nWidh, mHeight, rng1, rng2, d->fs_sharpness, channels, filter_type, cst, ch);
}

fft_decompose_channels fs_get_channels_from_colorspace(const fs_filter_by filter_by, const dt_iop_colorspace_type_t cst)
{
  fft_decompose_channels resp = FFT_DECOMPOSE_ALL;

  if (filter_by == FS_FILTER_BY_ALL)
  {
    resp = FFT_DECOMPOSE_CH1|FFT_DECOMPOSE_CH2|FFT_DECOMPOSE_CH3;
  }
  else if (cst == iop_cs_Lab)
  {
    if (filter_by == FS_FILTER_BY_l)
      resp = FFT_DECOMPOSE_CH1;
    else if (filter_by == FS_FILTER_BY_a)
      resp = FFT_DECOMPOSE_CH2;
    else if (filter_by == FS_FILTER_BY_b)
      resp = FFT_DECOMPOSE_CH3;
    else if (filter_by == FS_FILTER_BY_ab)
      resp = FFT_DECOMPOSE_CH2|FFT_DECOMPOSE_CH3;
  }
  else if (cst == iop_cs_rgb)
  {
    if (filter_by == FS_FILTER_BY_R)
      resp = FFT_DECOMPOSE_CH1;
    else if (filter_by == FS_FILTER_BY_G)
      resp = FFT_DECOMPOSE_CH2;
    else if (filter_by == FS_FILTER_BY_B)
      resp = FFT_DECOMPOSE_CH3;
    else if (filter_by == FS_FILTER_BY_H)
        resp = FFT_DECOMPOSE_CH1|FFT_DECOMPOSE_CH2|FFT_DECOMPOSE_CH3;
    else if (filter_by == FS_FILTER_BY_S)
      resp = FFT_DECOMPOSE_CH1|FFT_DECOMPOSE_CH2|FFT_DECOMPOSE_CH3;
    else if (filter_by == FS_FILTER_BY_L)
      resp = FFT_DECOMPOSE_CH1|FFT_DECOMPOSE_CH2|FFT_DECOMPOSE_CH3;
    else if (filter_by == FS_FILTER_BY_HS)
      resp = FFT_DECOMPOSE_CH1|FFT_DECOMPOSE_CH2|FFT_DECOMPOSE_CH3;
  }

  return resp;
}

void fs_filter_HLS_from_RGB(float *const o, dt_iop_roi_t *const roi_out, float *const filtered_ch, const fs_filter_by filter_by,
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

int fs_convert_from_to_colorspace(float * b, dt_iop_roi_t * roi_out, float * filtered_ch,
                                    const fs_filter_by filter_by, const dt_iop_colorspace_type_t cst, const int ch, const int forward)
{
  int converted = 0;
  fft_decompose_channels channels = 0;
  int cst_dest = 1;

  if (cst == iop_cs_rgb)
  {
    if (filter_by == FS_FILTER_BY_H) {
      channels = FFT_DECOMPOSE_CH1|FFT_DECOMPOSE_CH3; cst_dest = 1; /* HSL */}
    else if (filter_by == FS_FILTER_BY_S) {
      channels = FFT_DECOMPOSE_CH2|FFT_DECOMPOSE_CH3; cst_dest = 1; /* HSL */}
    else if (filter_by == FS_FILTER_BY_L) {
      channels = FFT_DECOMPOSE_CH3; cst_dest = 1; /* HSL */}
    else if (filter_by == FS_FILTER_BY_HS) {
      channels = FFT_DECOMPOSE_CH1|FFT_DECOMPOSE_CH2; cst_dest = 1; /* HSL */}
  }

  if (channels != 0)
  {
    converted = 1;
    if (forward)
      memset(filtered_ch, 0, roi_out->height*roi_out->width*ch*sizeof(float));

    if (cst_dest == 1 /* HSL */)
    {
      fs_filter_HLS_from_RGB(b, roi_out, filtered_ch, filter_by, cst, ch, channels, forward);
    }
  }

  return converted;
}

// adjust the exposure of the frequency layer, for display purpuses only
// the image will be recomposed without the exposure change
void dt_fs_freqlayer_exposure(void *ivoid, const int width, const int height, const float white, const int ch)
{
#ifdef _FFT_MULTFR_
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(ivoid) schedule(static)
#endif
#endif
  for(int k = 0; k < height; k++)
  {
    const __m128 whitev = _mm_set1_ps(white+1);
    float *out = ((float *)ivoid) + (size_t)ch * k * width;
    for(int j = 0; j < width; j++, out += ch)
      _mm_store_ps(out, _mm_mul_ps(_mm_load_ps(out), whitev));
  }
}

void dt_fs_freqlayer_lighten(void *ivoid, const int width, const int height, const float white, const int ch)
{
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
}

/*
void fs_copy_in_to_out(float *const out, dt_iop_roi_t *const roi_out, float *const in, dt_iop_roi_t *const roi_in, const int ch)
{
#ifdef _FFT_MULTFR_
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static)
#endif
#endif
  for (int y = 0; y < roi_out->height; y++)
  {
    float *dest = &out[y * roi_out->width * ch];
    float *source = &in[ch * roi_in->width * (y + roi_out->y - roi_in->y) + ch * (roi_out->x - roi_in->x)];

    for (int x = 0; x < roi_out->width*ch; x++)
    {
        dest[x] = source[x];
    }
  }
}
*/

void dt_develop_freqsep_preprocess(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, void *ivoid,
                              void *ovoid, const struct dt_iop_roi_t *roi_in, const struct dt_iop_roi_t *roi_out)
{
  dt_develop_blend_params_t *d = (dt_develop_blend_params_t *)piece->blendop_data;

  if(!d) return;
  if(!(d->fs_filter_type > 0)) return;

  float *in = (float *)ivoid;
  float *out = (float *)ovoid;
  const int ch = piece->colors;
  const dt_iop_colorspace_type_t cst = dt_iop_module_colorspace(self);

  // save image width and height as the nearest >= pow2()
  d->fs_roi_tF1 = *roi_in;
  d->fs_roi_tF1.width = fft_convert_pow2(roi_out->width);
  d->fs_roi_tF1.height = fft_convert_pow2(roi_out->height);

  const int buffersize = d->fs_roi_tF1.width * d->fs_roi_tF1.height * ch * sizeof(float);

  // allocate space for decomposed image
  d->tF1 = dt_alloc_align(64, buffersize);
  d->tF2 = dt_alloc_align(64, buffersize);
  d->tF3 = dt_alloc_align(64, buffersize);
  float *tF4 = dt_alloc_align(64, buffersize);
  float *tF5 = dt_alloc_align(64, buffersize);
  float *tF6 = dt_alloc_align(64, buffersize);

  // allocate space for backup original image
  d->fs_ivoid = dt_alloc_align(64, (size_t)roi_in->width * roi_in->height * ch * sizeof(float));

  if (d->tF1 == NULL || d->tF2 == NULL || d->fs_ivoid == NULL || d->tF3 == NULL || tF4 == NULL || tF5 == NULL || tF6 == NULL)
  {
    fprintf(stderr, "NULL buffer for FFT!!!\n");

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
  memcpy(d->fs_ivoid, in, (size_t)roi_in->width * roi_in->height * 4 * sizeof(float));
  d->fs_roi_ivoid = *roi_in;

  // copy the input image (ivoid) as the FFT real part in tF1
  fft_copy_image_to_buffer(in, d->tF1, roi_in->width, roi_in->height, d->fs_roi_tF1.width, d->fs_roi_tF1.height, ch);

  // decompose the image into (tF3, tF4)
  fft_FFT2D(d->tF1, d->tF2, d->tF3, tF4, d->fs_roi_tF1.width, d->fs_roi_tF1.height, 0,
            fs_get_channels_from_colorspace(d->fs_filter_by, cst), cst, ch);

  // apply filter to (tF3, tF4)
  // (tF1, tF2) will store the complementary data (excluded by the filter)
  fs_apply_filter(d, d->tF3, tF4, d->tF1, d->tF2, d->fs_roi_tF1.width, d->fs_roi_tF1.height,
                /*d->fs_frequency, d->fs_frequency_range, 0,*/
                fs_get_channels_from_colorspace(d->fs_filter_by, cst), d->fs_filter_type, cst, ch);

  if (d->fs_invert_freq_layer)
  {
    float *tmp = d->tF3;
    d->tF3 = d->tF1;
    d->tF1 = tmp;

    tmp = tF4;
    tF4 = d->tF2;
    d->tF2 = tmp;
  }

  // recompose the filtered image (tF3, tF4) into (tF5, tF6), so it can be edited/displayed
  fft_FFT2D(d->tF3, tF4, tF5, tF6, d->fs_roi_tF1.width, d->fs_roi_tF1.height, 1,
            fs_get_channels_from_colorspace(d->fs_filter_by, cst), cst, ch);

  fs_convert_from_to_colorspace(tF5, &d->fs_roi_tF1, d->tF3, d->fs_filter_by, cst, ch, 1);

  // copy tF5 to input for the parent module
  fft_copy_buffer_to_image(in, tF5, d->fs_roi_ivoid.width, d->fs_roi_ivoid.height, d->fs_roi_tF1.width, d->fs_roi_tF1.height, ch);
  fft_copy_buffer_to_image(out, tF5, d->fs_roi_ivoid.width, d->fs_roi_ivoid.height, d->fs_roi_tF1.width, d->fs_roi_tF1.height, ch);

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
  if (tF6)
  {
    dt_free_align(tF6);
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
    fprintf(stderr, "NULL buffer for FFT!!!\n");
    return;
  }

  float *in = (float *)ivoid;
  float *out = (float *)ovoid;
  const int ch = piece->colors;
  const dt_iop_colorspace_type_t cst = dt_iop_module_colorspace(self);

  const int buffersize = d->fs_roi_tF1.width * d->fs_roi_tF1.height * ch * sizeof(float);

  float *tF4 = dt_alloc_align(64, buffersize);
  float *tF5 = dt_alloc_align(64, buffersize);
  float *tF6 = dt_alloc_align(64, buffersize);

  // if the user wants to see just the frequency layer, just copy ivoid to ovoid
  if (d->fs_preview == DEVELOP_FS_PREVIEW_FREQLAY)
  {
    memcpy(out, in, (size_t)roi_in->width * roi_in->height * ch * sizeof(float));
    if (d->fs_lighten_freq_layer)
      dt_fs_freqlayer_lighten(out, roi_out->width, roi_out->height, .018, ch);
    dt_fs_freqlayer_exposure(out, roi_out->width, roi_out->height, d->fs_freqlay_exposure, ch);
  }
  else if (d->fs_preview == DEVELOP_FS_PREVIEW_FINAL_IMAGE)
  {
    // copy the parent module output image (ovoid) as the FFT real part in tF5
    fft_copy_image_to_buffer(out, tF5, roi_out->width, roi_out->height, d->fs_roi_tF1.width, d->fs_roi_tF1.height, ch);

    fs_convert_from_to_colorspace(tF5, &d->fs_roi_tF1, d->tF3, d->fs_filter_by, cst, ch, 0);

    // decompose (again) the image (tF5, tF6) into (tF3, tF4)
    fft_FFT2D(tF5, tF6, d->tF3, tF4, d->fs_roi_tF1.width, d->fs_roi_tF1.height, 0,
              fs_get_channels_from_colorspace(d->fs_filter_by, cst), cst, ch);

    // apply filter to (tF3, tF4) to merge it with (tF1, tF2)
    fft_recompose_image(d->tF3, tF4, d->tF1, d->tF2, d->fs_roi_tF1.width, d->fs_roi_tF1.height,
                        /*fs_get_channels_from_colorspace(d->fs_filter_by, cst),*/ ch);

    // recompose back (tF3, tF4) into (tF5, tF6), and get the final image
    fft_FFT2D(d->tF3, tF4, tF5, tF6, d->fs_roi_tF1.width, d->fs_roi_tF1.height, 1,
              fs_get_channels_from_colorspace(d->fs_filter_by, cst), cst, ch);

    // copy tF5 to output
    //fs_copy_in_to_out(out, roi_out, tF5, roi_in, ch);
    fft_copy_buffer_to_image(out, tF5, roi_out->width, roi_out->height, d->fs_roi_tF1.width, d->fs_roi_tF1.height, ch);
  }
  else if (d->fs_preview == DEVELOP_FS_PREVIEW_FREQLAY_CHNG)
  {
    dt_fs_freqlayer_exposure(out, roi_out->width, roi_out->height, d->fs_freqlay_exposure, ch);
  }

  // restore the original image into ivoid, we have abuse it long enough
  memcpy(in, d->fs_ivoid, (size_t)roi_in->width * roi_in->height * ch * sizeof(float));

  // at this point we can free the buffers
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
  if (tF4)
  {
    dt_free_align(tF4);
  }
  if (tF5)
  {
    dt_free_align(tF5);
  }
  if (tF6)
  {
    dt_free_align(tF6);
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

