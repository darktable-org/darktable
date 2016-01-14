
#ifndef DT_DEVELOP_FOURIER_H
#define DT_DEVELOP_FOURIER_H

#define _FFT_SSE_
#define _FFT_MULTFR_


typedef enum fft_decompose_channels
{
  FFT_DECOMPOSE_CH1 = 0x01,
  FFT_DECOMPOSE_CH2 = 0x02,
  FFT_DECOMPOSE_CH3 = 0x04,
  FFT_DECOMPOSE_CH4 = 0x08,
  FFT_DECOMPOSE_ALL = (FFT_DECOMPOSE_CH1 | FFT_DECOMPOSE_CH2 | FFT_DECOMPOSE_CH3 | FFT_DECOMPOSE_CH4)
} fft_decompose_channels;

typedef enum fft_filter_type
{
  FFT_FILTER_TYPE_HIGHPASS = 1,
  FFT_FILTER_TYPE_LOWPASS = 2,
  FFT_FILTER_TYPE_BANDPASS = 3,
	FFT_FILTER_TYPE_BANDBLOCK = 4,
	FFT_FILTER_TYPE_THRESHOLD = 5,
	FFT_FILTER_TYPE_THRESHOLD_INV = 6,
	FFT_FILTER_TYPE_LOWPASS_BUTTERWORTH = 7,
	FFT_FILTER_TYPE_HIGHPASS_BUTTERWORTH = 8,
	FFT_FILTER_TYPE_LOWPASS_GAUSSIAN = 9,
	FFT_FILTER_TYPE_HIGHPASS_GAUSSIAN = 10
} fft_filter_type;

void fft_FFT2D(float *const  gRe, float *const  gIm, float *const GRe, float *const GIm, const int nWidh, const int mHeight,
                const int inverse, const fft_decompose_channels channels, const dt_iop_colorspace_type_t cst, const int ch);

void fft_apply_filter(float *const InputR, float *const InputI, float *OutputR, float *OutputI,
                        const int nWidh, const int mHeight,
		                    const float range1, const float range2, const int sharpness, const fft_decompose_channels channels,
		                    const fft_filter_type filter_type, const dt_iop_colorspace_type_t cst, const int ch);
void fft_recompose_image(float *GRe, float *GIm, float *gRe, float *gIm, const int nWidh, const int mHeight,
                        const fft_decompose_channels channels, const int ch);

int fft_convert_pow2(int n);
void fft_copy_buffer_to_image(float * image, const float *const buffer, const int w_img, const int h_img,
                                const int w_buff, const int h_buff, const int ch);
void fft_copy_image_to_buffer(const float *const image, float * buffer, const int w_img, const int h_img,
                                const int w_buff, const int h_buff, const int ch);


#endif

