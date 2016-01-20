
#ifndef DT_DEVELOP_FOURIER_H
#define DT_DEVELOP_FOURIER_H

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
	FFT_FILTER_TYPE_LOWPASS_BUTTERWORTH = 1,
	FFT_FILTER_TYPE_HIGHPASS_BUTTERWORTH = 2,
	FFT_FILTER_TYPE_LOWPASS_GAUSSIAN = 3,
  FFT_FILTER_TYPE_HIGHPASS_GAUSSIAN = 4,
  FFT_FILTER_TYPE_HIGHPASS_SMOOTH = 5
} fft_filter_type;

void fft_filter_fft(float *const InputR, float *const InputI, float *const OutputR, float *const OutputI,
                    const int nWidh, const int mHeight,
                    const float range1, const float range2, const int sharpness, const fft_decompose_channels channels, const fft_filter_type filter_type,
                    const dt_iop_colorspace_type_t cst, const int ch);

void fft_FFT2D(float *const  gRe, float *const  gIm, float *const GRe, float *const GIm, const int nWidh, const int mHeight,
                const int inverse, const fft_decompose_channels channels, const dt_iop_colorspace_type_t cst, const int ch);
void fft_FFT2D_R_Forward(float *const GRe, float *const GIm, const int nWidh, const int mHeight,
                const fft_decompose_channels channels, const dt_iop_colorspace_type_t cst, const int ch);
void fft_FFT2D_R_Inverse(float *const GRe, float *const GIm, const int nWidh, const int mHeight,
                const fft_decompose_channels channels, const dt_iop_colorspace_type_t cst, const int ch);

void fft_recompose_image(float *GRe, float *GIm, float *gRe, float *gIm, const int nWidh, const int mHeight, const int ch);

int fft_convert_pow2(int n);


#endif

