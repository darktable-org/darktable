
#include "bauhaus/bauhaus.h"
#include "common/opencl.h"
#include "common/dtpthread.h"
#include "common/debug.h"
#include "control/control.h"
#include "fft.h"
#include <memory.h>
#include <stdlib.h>
#include <xmmintrin.h>

#define _FFT_SSE_
#define _FFT_MULTFR_

#ifdef _FFT_SSE_
void fft_filter_fft(float *const InputR, float *const InputI, float *const OutputR, float *const OutputI,
                    const int nWidh, const int mHeight,
                    const float range1, const float range2, const int sharpness, const fft_decompose_channels channels,
                    const fft_filter_type filter_type, const dt_iop_colorspace_type_t cst, const int ch)
{
  const int nWidh1 = nWidh*ch;
  float dist = 0;

  memset(OutputR, 0, nWidh*mHeight*ch*sizeof(float));
  memset(OutputI, 0, nWidh*mHeight*ch*sizeof(float));


#ifdef _FFT_MULTFR_x
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static)
#endif
#endif
  for (int y=0; y < mHeight; y++)
  {
    const int ii = nWidh1*y;

    float *inR = &InputR[ii];
    float *inI = &InputI[ii];
    float *outR = &OutputR[ii];
    float *outI = &OutputI[ii];

    for (int x=0, col = 0; x < nWidh1; x+=ch, col++)
    {
      float val = 0;

       // Bandpass Ideal
       if (filter_type == FFT_FILTER_TYPE_BANDPASS_IDEAL)
       {
         dist = (((nWidh/2.f-col)*(nWidh/2.f-col))+((mHeight/2.f-y)*(mHeight/2.f-y)));

         if (range1 <= dist && dist <= range2)
         {
           val = 1.f;
         }
         else
         {
           val = 0;
         }
       }

       // Lowpass Ideal
       if (filter_type == FFT_FILTER_TYPE_LOWPASS_IDEAL)
       {
         dist = (((nWidh/2.f-col)*(nWidh/2.f-col))+((mHeight/2.f-y)*(mHeight/2.f-y)));

         if (range1 <= dist)
         {
           val = 1.f;
         }
         else
         {
           val = 0;
         }
       }

       // Highpass Ideal
       if (filter_type == FFT_FILTER_TYPE_HIGHPASS_IDEAL)
       {
         dist = (((nWidh/2.f-col)*(nWidh/2.f-col))+((mHeight/2.f-y)*(mHeight/2.f-y)));

         if (range1 >= dist)
         {
           val = 1.f;
         }
         else
         {
           val = 0;
         }
       }

       // Butterworth Highpass
       if (filter_type == FFT_FILTER_TYPE_HIGHPASS_BUTTERWORTH)
       {
         dist = (((nWidh/2.f-col)*(nWidh/2.f-col))+((mHeight/2.f-y)*(mHeight/2.f-y)));

         if (range1 == 0)
         {
           val = 1.f;
         }
         else
         {
           val = 1.f / (1.f + powf(dist / (range1*range1), sharpness));
         }
       }

       // Butterworth Lowpass
       else if (filter_type == FFT_FILTER_TYPE_LOWPASS_BUTTERWORTH)
       {
         dist = (((nWidh/2.f-col)*(nWidh/2.f-col))+((mHeight/2.f-y)*(mHeight/2.f-y)));

         if (dist == 0)
         {
           val = 1.f;
         }
         else
         {
           val = 1.f / (1.f + powf((range1*range1) / dist, sharpness));
         }
       }

       // Butterworth Bandpass
       if (filter_type == FFT_FILTER_TYPE_BANDPASS_BUTTERWORTH)
       {
         dist = (((nWidh/2.f-col)*(nWidh/2.f-col))+((mHeight/2.f-y)*(mHeight/2.f-y)));
         if (range1 == 0 && range2 == 0)
         {
           val = 1.f;
         }
         else
         {
           val = (1.f / (1.f + powf(dist / (range2*range2), sharpness))) * (1.f / (1.f + powf((range1*range1) / dist, sharpness)));
         }
       }

       // Gaussian Lowpass
       else if (filter_type == FFT_FILTER_TYPE_LOWPASS_GAUSSIAN)
       {
         int dv = (y < mHeight / 2) ? y : y - mHeight;
         int du = (col < nWidh / 2) ? col : col - nWidh;
         dist = (float)(dv * dv + du * du);

         if (range1 == 0)
         {
           val = 1.f;
         }
         else
         {
           val = expf(dist / (-2.f*range1*range1));
         }
       }

       // Gaussian Highpass
       else if (filter_type == FFT_FILTER_TYPE_HIGHPASS_GAUSSIAN)
       {
         int dv = (y < mHeight / 2) ? y : y - mHeight;
         int du = (col < nWidh / 2) ? col : col - nWidh;
         dist = (float)(dv * dv + du * du);

         if (range1 == 0)
         {
           val = 1.f;
         }
         else
         {
           val = 1.f - expf(dist / (-2.f*range1*range1));
         }
       }

       // Gaussian Bandpass
       else if (filter_type == FFT_FILTER_TYPE_BANDPASS_GAUSSIAN)
       {
         int dv = (y < mHeight / 2) ? y : y - mHeight;
         int du = (col < nWidh / 2) ? col : col - nWidh;
         dist = (float)(dv * dv + du * du);

         if (range1 == 0 || range2 == 0)
         {
           val = 1.f;
         }
         else
         {
           val = ( expf(dist / (-2.f*range1*range1)) ) * ( 1 - expf(dist / (-2.f*range2*range2)) );
         }
       }

       else if (filter_type == FFT_FILTER_TYPE_LOWPASS_SMOOTH)
       {
         /*               1      f < cutoff - w
             f H(x) =     0      f > cutoff + w
                          else   1/2*(1-sin(pi*(f-cutoff)/2*w))
          */

         const float cutoff = range1;
         const float w = range2;
         const float x0 = nWidh/2.f;
         const float y0 = mHeight/2.f;
         const float xa = col;
         const float ya = y;
         const float f = sqrtf(((x0-xa)*(x0-xa))+((y0-ya)*(y0-ya)));
           if (f < cutoff - w)
             val = 1.f;
           else if (f > cutoff + w)
             val = 0.f;
           else if (w == 0)
             val = 0.f;
           else
           {
             val = 0.5f*(1.f-sinf(M_PI*(f-cutoff)/(2.f*w)));
           }
       }


       // BARTLETT
       else if (filter_type == FFT_FILTER_TYPE_BARTLETT)
       {
         int dv = (y < mHeight / 2) ? y : y - mHeight;
         int du = (col < nWidh / 2) ? col : col - nWidh;
         dist = (float)(dv * dv + du * du);
          if (range1 == 0)
            val = 1.f;
          else
          {
             float tmp = dist - (range1 / 2.f);
             val = 1.0f - (2.0f * fabs(tmp)) / range1;
          }
       }

        const float val4[4] = { (channels & FFT_DECOMPOSE_CH1) ? val : 0.f,
                                (channels & FFT_DECOMPOSE_CH2) ? val : 0.f,
                                (channels & FFT_DECOMPOSE_CH3) ? val : 0.f,
                                (channels & FFT_DECOMPOSE_CH4) ? val : 0.f };
        const __m128 val4a = _mm_load_ps(val4);

        const float tR[4] = { inR[x], inR[x+1], inR[x+2], inR[x+3] };
        const float tI[4] = { inI[x], inI[x+1], inI[x+2], inI[x+3] };

        _mm_store_ps(&(inR[x]), _mm_mul_ps(_mm_load_ps(&(inR[x])), val4a));
        _mm_store_ps(&(inI[x]), _mm_mul_ps(_mm_load_ps(&(inI[x])), val4a));

        _mm_store_ps(&(outR[x]), _mm_sub_ps(_mm_load_ps(tR), _mm_load_ps(&(inR[x]))));
        _mm_store_ps(&(outI[x]), _mm_sub_ps(_mm_load_ps(tI), _mm_load_ps(&(inI[x]))));
    }
  }

}
#else
void fft_filter_fft(float *const InputR, float *const InputI, float *const OutputR, float *const OutputI,
                    const int nWidh, const int mHeight,
                    const float range1, const float range2, const int sharpness, const fft_decompose_channels channels, const fft_filter_type filter_type,
                    const dt_iop_colorspace_type_t cst, const int ch)
{
  const int nWidh1 = nWidh*ch;

  memset(OutputR, 0, nWidh*mHeight*ch*sizeof(float));
  memset(OutputI, 0, nWidh*mHeight*ch*sizeof(float));

#ifdef _FFT_MULTFR_
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static)
#endif
#endif
  for (int y=0; y < mHeight; y++)
  {
    const int ii = nWidh1*y;

    float *inR = &InputR[ii];
    float *inI = &InputI[ii];
    float *outR = &OutputR[ii];
    float *outI = &OutputI[ii];

    int col = 0;
    for (int x=0; x < nWidh1; x+=ch)
    {
      float val = 0;

       int dv = (y < mHeight / 2) ? y : y - mHeight;
       int du = (col < nWidh / 2) ? col : col - nWidh;
       float dist = (float)(dv * dv + du * du);

       // Butterworth Lowpass
       if (filter_type == FFT_FILTER_TYPE_LOWPASS_BUTTERWORTH)
         val = 1 / (1 + powf(dist / (range1*range1), sharpness));

       // Butterworth Highpass
       else if (filter_type == FFT_FILTER_TYPE_HIGHPASS_BUTTERWORTH)
         val = 1 / (1 + powf((range1*range1) / dist, sharpness));


       // Gaussian Lowpass
       else if (filter_type == FFT_FILTER_TYPE_LOWPASS_GAUSSIAN)
         val = expf(dist / (-2.f*range1*range1));

       // Gaussian Highpass
       else if (filter_type == FFT_FILTER_TYPE_HIGHPASS_GAUSSIAN)
         val = 1 - expf(dist / (-2.f*range1*range1));

       else if (filter_type == FFT_FILTER_TYPE_LOWPASS_SMOOTH)
       {
         /*               1      f < cutoff - w
             f H(x) =     0      f > cutoff + w
                          else   1/2*(1-sin(pi*(f-cutoff)/2*w))
          */

         const float cutoff = range1;
         const float w = range2;
         const float x0 = nWidh/2.f;
         const float y0 = mHeight/2.f;
         const float xa = col;
         const float ya = y;
         const float f = sqrtf(((x0-xa)*(x0-xa))+((y0-ya)*(y0-ya)));

         if (filter_type == FFT_FILTER_TYPE_LOWPASS_SMOOTH)
         {
           if (f < cutoff - w)
             val = 1;
           else if (f > cutoff + w)
             val = 0;
           else
           {
             val = 0.5f*(1-sin(M_PI*(f-cutoff)/(2*w)));
           }
         }
       }


      for (int i=0; i<ch; i++)
      {
        if ( ((channels & FFT_DECOMPOSE_CH1) && i == 0) ||
            ((channels & FFT_DECOMPOSE_CH2) && i == 1) ||
            ((channels & FFT_DECOMPOSE_CH3) && i == 2) ||
            ((channels & FFT_DECOMPOSE_CH4) && i == 3) )
        {
          const float tR = inR[x+i];
          const float tI = inI[x+i];

          inR[x+i] *= val;
          inI[x+i] *= val;

          outR[x+i] = tR - inR[x+i];
          outI[x+i] = tI - inI[x+i];
        }
        else
        {
          outR[x+i] = inR[x+i];
          outI[x+i] = inI[x+i];
          inR[x+i]=0; inI[x+i]=0;
        }
      }

      col++;
    }
  }

}
#endif

// performs a bit reversal for all the rows and columns
static inline void fft_bit_reversal(float *GRe, float *GIm, const int nWidh, const int mHeight, const int ch)
{
  const int nWidh1 = nWidh*ch;

  //Bit reversal of each row, this is always done for all the channels
#ifdef _FFT_MULTFR_
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(GRe, GIm) schedule(static)
#endif
#endif
  for (int y = 0; y < mHeight; y++) // for each row
  {
    unsigned int target = 0; // swap position

    float *re = &GRe[y*nWidh1];
    float *im = &GIm[y*nWidh1];

    // process all positions of input signal
    for (unsigned int position = 0; position < nWidh; position++)
    {
      if (target > position) // only for not yet swapped entries
      {
          for (int i = 0; i < ch; i++)
          {
            // swap entries
            const int index_t = target*ch+i;
            const int index_p = position*ch+i;

            const float tempR = re[index_t];
            const float tempI = im[index_t];

            re[index_t] = re[index_p];
            im[index_t] = im[index_p];
            re[index_p] = tempR;
            im[index_p] = tempI;
          }

       }

      unsigned int mask = nWidh; // bit mask

      // while bit is set
      while (target & (mask >>= 1)) target &= ~mask; // drop bit

      target |= mask; // the current bit is 0 - set it
    }
  }

  //Bit reversal of each column, this is always done for all the channels
#ifdef _FFT_MULTFR_
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(GRe, GIm) schedule(static)
#endif
#endif
  for(int x = 0; x < nWidh; x++) //for each column
  {
    unsigned int target = 0; // swap position
    const int x1 = x * ch;

    // process all positions of input signal
    for (unsigned int position = 0; position < mHeight; ++position)
    {
      if (target > position) //only for not yet swapped entries
      {
        for (int i = 0; i < ch; i++)
        {
          // swap entries
          const int source = (nWidh1 * position) + x1 + i;
          const int dest = (nWidh1 * target) + x1 + i;

          const float tempR = GRe[dest];
          const float tempI = GIm[dest];
          GRe[dest] = GRe[source];
          GIm[dest] = GIm[source];
          GRe[source] = tempR;
          GIm[source] = tempI;
        }
      }

      unsigned int mask = mHeight; //bit mask

      // while bit is set
      while (target & (mask >>= 1)) target &= ~mask; //drop bit

      target |= mask; // the current bit is 0 - set it
    }
  }
}

static inline void fft_bit_reversal_R_Forward(float *GRe, const int nWidh, const int mHeight, const int ch)
{
  const int nWidh1 = nWidh*ch;

  //Bit reversal of each row, this is always done for all the channels
#ifdef _FFT_MULTFR_
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(GRe) schedule(static)
#endif
#endif
  for (int y = 0; y < mHeight; y++) // for each row
  {
    unsigned int target = 0; // swap position

    float *re = &GRe[y*nWidh1];

    // process all positions of input signal
    for (unsigned int position = 0; position < nWidh; position++)
    {
      if (target > position) // only for not yet swapped entries
      {
          for (int i = 0; i < ch; i++)
          {
            // swap entries
            const int index_t = target*ch+i;
            const int index_p = position*ch+i;
            const float tempR = re[index_t];

            re[index_t] = re[index_p];
            re[index_p] = tempR;
          }

       }

      unsigned int mask = nWidh; // bit mask

      // while bit is set
      while (target & (mask >>= 1)) target &= ~mask; // drop bit

      target |= mask; // the current bit is 0 - set it
    }
  }

  //Bit reversal of each column, this is always done for all the channels
#ifdef _FFT_MULTFR_
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(GRe) schedule(static)
#endif
#endif
  for(int x = 0; x < nWidh; x++) //for each column
  {
    unsigned int target = 0; // swap position
    const int x1 = x * ch;

    // process all positions of input signal
    for (unsigned int position = 0; position < mHeight; ++position)
    {
      if (target > position) //only for not yet swapped entries
      {
        for (int i = 0; i < ch; i++)
        {
          // swap entries
          const int source = (nWidh1 * position) + x1 + i;
          const int dest = (nWidh1 * target) + x1 + i;

          const float tempR = GRe[dest];
          GRe[dest] = GRe[source];
          GRe[source] = tempR;
        }
      }

      unsigned int mask = mHeight; //bit mask

      // while bit is set
      while (target & (mask >>= 1)) target &= ~mask; //drop bit

      target |= mask; // the current bit is 0 - set it
    }
  }
}

// scale the result
#ifdef _FFT_SSE_
void fft_scale(float *const GRe, float *const GIm, const int nWidh, const int mHeight, const fft_decompose_channels channels, const int ch)
{
    const float factor = 1.f / (float)(nWidh*mHeight);
    const int nWidh1 = nWidh*ch;
    const __m128 fRI = _mm_set1_ps(factor);

#ifdef _FFT_MULTFR_
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static)
#endif
#endif
    for (int y = 0; y < mHeight; y++)
    {
      float *re = &GRe[y*nWidh1];
      float *im = &GIm[y*nWidh1];

      for (int x = 0; x < nWidh1; x+=ch)
      {
        _mm_store_ps(&(re[x]), _mm_mul_ps(_mm_load_ps(&(re[x])), fRI));
        _mm_store_ps(&(im[x]), _mm_mul_ps(_mm_load_ps(&(im[x])), fRI));
      }
    }
}

void inline fft_scale_R(float *const GRe, const int nWidh, const int mHeight, const fft_decompose_channels channels, const int ch)
{
    const float factor = 1.f / (float)(nWidh*mHeight);
    const int nWidh1 = nWidh*ch;
    const __m128 fRI = _mm_set1_ps(factor);

#ifdef _FFT_MULTFR_
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static)
#endif
#endif
    for (int y = 0; y < mHeight; y++)
    {
      float *re = &GRe[y*nWidh1];

      for (int x = 0; x < nWidh1; x+=ch)
      {
        _mm_store_ps(&(re[x]), _mm_mul_ps(_mm_load_ps(&(re[x])), fRI));
      }
    }
}
#else
void fft_scale(float * GRe, float * GIm, const int nWidh, const int mHeight, const fft_decompose_channels channels, const int ch)
{
    const float factor = 1.f / (float)(nWidh*mHeight);
    const int nWidh1 = nWidh*ch;

    for (int y = 0; y < mHeight; y++)
    {
      float *re = &GRe[y*nWidh1];
      float *im = &GIm[y*nWidh1];

      for (int x = 0; x < nWidh1; x+=ch)
      {
        if (channels & FFT_DECOMPOSE_CH1) {
          re[x] *= factor;
          im[x] *= factor;
         }
        if (channels & FFT_DECOMPOSE_CH2) {
          re[x+1] *= factor;
          im[x+1] *= factor;
         }
        if (channels & FFT_DECOMPOSE_CH3) {
          re[x+2] *= factor;
          im[x+2] *= factor;
         }
        if (channels & FFT_DECOMPOSE_CH4) {
          re[x+3] *= factor;
          im[x+3] *= factor;
         }
      }
    }
}
#endif

// performs the math for a single row/column
#ifdef _FFT_SSE_
static inline void fft_single_fft2d(float *GRe, float *GIm, const int original, const int match, const float fR, const float fI,
                              const fft_decompose_channels channels, const dt_iop_colorspace_type_t cst, const int ch)
{
  //(tR, tI) = (fR, fI)*(GRe, GIm)[match]
  const __m128 tRa = _mm_sub_ps(_mm_mul_ps( _mm_set1_ps(fR), _mm_load_ps( &(GRe[match]))),
                                _mm_mul_ps( _mm_set1_ps(fI), _mm_load_ps( &(GIm[match]))));
  const __m128 tIa = _mm_add_ps(_mm_mul_ps( _mm_set1_ps(fR), _mm_load_ps( &(GIm[match]))),
                                _mm_mul_ps( _mm_set1_ps(fI), _mm_load_ps( &(GRe[match]))));

  // (GRe, GIm)[match] = (GRe, GIm)[original] - (tR, tI)
  _mm_store_ps( &(GRe[match]), _mm_sub_ps( _mm_load_ps( &(GRe[original])), tRa ) );
  _mm_store_ps( &(GIm[match]), _mm_sub_ps( _mm_load_ps( &(GIm[original])), tIa ) );

  // (GRe, GIm)[original] = (GRe, GIm)[original] + (tR, tI)
  _mm_store_ps( &(GRe[original]), _mm_add_ps( _mm_load_ps( &(GRe[original])), tRa ) );
  _mm_store_ps( &(GIm[original]), _mm_add_ps( _mm_load_ps( &(GIm[original])), tIa ) );

}
#else
static void fft_single_fft2d(float *GRe, float *GIm, const int original, const int match, const float fR, const float fI,
                              const fft_decompose_channels channels, const dt_iop_colorspace_type_t cst, const int ch)
{
  float tR[ch];
  float tI[ch];

  //(tR, tI) = (fR, fI)*(GRe, GIm)
  if (channels & FFT_DECOMPOSE_CH1) {
    tR[0] = fR * GRe[match] - fI * GIm[match];
    tI[0] = fR * GIm[match] + fI * GRe[match];}
  if (channels & FFT_DECOMPOSE_CH2) {
    tR[1] = fR * GRe[match+1] - fI * GIm[match+1];
    tI[1] = fR * GIm[match+1] + fI * GRe[match+1];}
  if (channels & FFT_DECOMPOSE_CH3) {
    tR[2] = fR * GRe[match+2] - fI * GIm[match+2];
    tI[2] = fR * GIm[match+2] + fI * GRe[match+2];}
  if (channels & FFT_DECOMPOSE_CH4) {
    tR[3] = fR * GRe[match+3] - fI * GIm[match+3];
    tI[3] = fR * GIm[match+3] + fI * GRe[match+3];}

  // (GRe, GIm) = (GRe, GIm) - (tR, tI)
  if (channels & FFT_DECOMPOSE_CH1) {
    GRe[match] = GRe[original] - tR[0];
    GIm[match] = GIm[original] - tI[0];}
  if (channels & FFT_DECOMPOSE_CH2) {
    GRe[match+1] = GRe[original+1] - tR[1];
    GIm[match+1] = GIm[original+1] - tI[1];}
  if (channels & FFT_DECOMPOSE_CH3) {
    GRe[match+2] = GRe[original+2] - tR[2];
    GIm[match+2] = GIm[original+2] - tI[2];}
  if (channels & FFT_DECOMPOSE_CH4) {
    GRe[match+3] = GRe[original+3] - tR[3];
    GIm[match+3] = GIm[original+3] - tI[3];}

  // (GRe, GIm) = (GRe, GIm) + (tR, tI)
  if (channels & FFT_DECOMPOSE_CH1) {
    GRe[original] += tR[0];
    GIm[original] += tI[0];}
  if (channels & FFT_DECOMPOSE_CH2) {
    GRe[original+1] += tR[1];
    GIm[original+1] += tI[1];}
  if (channels & FFT_DECOMPOSE_CH3) {
    GRe[original+2] += tR[2];
    GIm[original+2] += tI[2];}
  if (channels & FFT_DECOMPOSE_CH4) {
    GRe[original+3] += tR[3];
    GIm[original+3] += tI[3];}

}
#endif

// increments the transform fator at the end of earch loop for both row and cols processing
static inline void fft_increment_transform_factor(float *fR, float *fI, const float ca, const float sa)
{
  // (fR, fI) = (ca, sa)*(fR, fI)+(fR, fI)
  const float z =  (*fR * ca - *fI * sa)+*fR;
  *fI = (*fR * sa + *fI * ca)+*fI;
  *fR = z;
}

static inline void load_lookup_ca_sa(float *lookup_ca, float *lookup_sa, const float pi, const int buffsize)
{

  for (int l=1; l < buffsize; l<<= 1)
  {
    const double delta = pi / (double)l; /* angle increment */
    const double Sine = sinf(delta * .5); /* auxiliary sin(delta / 2) */
    const float ca = (-2. * Sine * Sine); /* multiplier for trigonometric recurrence */
    const float sa = (sinf(delta));

    lookup_ca[l] = ca;
    lookup_sa[l] = sa;
  }
}

// initialize the transform fator for both row and cols processing
#ifndef _FFT_SSE_
#define INIT_TRANSFORM_FACTOR()                                                                        \
    l2 = l*2;                                                                                          \
    float fR = 1.0; /* start value for transform factor */                                             \
    float fI = 0.0;                                                                                    \
    const double delta = pi / (double)l; /* angle increment */                                         \
    const double Sine = sinf(delta * .5); /* auxiliary sin(delta / 2) */                               \
    const float ca = (-2. * Sine * Sine); /* multiplier for trigonometric recurrence */                \
    const float sa = (sinf(delta));
#else
#define INIT_TRANSFORM_FACTOR()                                                                        \
    l2 = l*2;                                                                                          \
    float fR = 1.0; /* start value for transform factor */                                             \
    float fI = 0.0;                                                                                    \
    const float ca = lookup_ca[l]; /* multiplier for trigonometric recurrence */                       \
    const float sa = lookup_sa[l];
#endif

// performs a FFT (gRe, gIm) --> (GRe, GIm)
// nWidh, mHeight of both images
// inverse = 1, performs the inverse transformation
// channels: channels to decompose
void fft_FFT2D(float *const  gRe, float *const  gIm, float *const GRe, float *const GIm, const int nWidh, const int mHeight,
                const int inverse, const fft_decompose_channels channels, const dt_iop_colorspace_type_t cst, const int ch)
{
  const double pi = inverse ? M_PI : -M_PI;
  const int nWidh1 = nWidh * ch;

  // copy channels to the output buffers
  memcpy(GRe, gRe, nWidh1*mHeight*sizeof(float));
  memcpy(GIm, gIm, nWidh1*mHeight*sizeof(float));
  
  const int buffsize = fmax(nWidh, mHeight);
  float *lookup_ca = dt_alloc_align(64, buffsize*sizeof(float));
  float *lookup_sa = dt_alloc_align(64, buffsize*sizeof(float));

  load_lookup_ca_sa(lookup_ca, lookup_sa, pi, buffsize);

  //Bit reversal
  fft_bit_reversal(GRe, GIm, nWidh, mHeight, ch);
  
  //Calculate the FFT of the columns
#ifdef _FFT_MULTFR_
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(lookup_ca, lookup_sa) schedule(static)
#endif
#endif
  for (int x = 0; x < nWidh; x++) //for each column
  {
    // this is the 1D FFT:
    const int x1 = x * ch;

    int l2 = 1;
    for (int l=1; l < mHeight; l<<= 1)
    {
      INIT_TRANSFORM_FACTOR();

      for (int j = 0; j < l; j++)
      {
        for (int y = j; y < mHeight; y += l2)
        {
          const int i1 = y + l;
          const int original = nWidh1 * y + x1;
          const int match = nWidh1 * i1 + x1;

          fft_single_fft2d(GRe, GIm, original, match, fR, fI, channels, cst, ch);
        }

        fft_increment_transform_factor(&fR, &fI, ca, sa);
      }
    }
  }

// calculate the FFT of the rows
#ifdef _FFT_MULTFR_
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(lookup_ca, lookup_sa) schedule(static)
#endif
#endif
  for (int y = 0; y < mHeight; y++) // for each row
  {
    const int y1 = y*nWidh1;
    float *re = GRe+y1;
    float *im = GIm+y1;

    //This is the 1D FFT

    int l2 = 1;
    for(int l = 1; l < nWidh; l<<= 1)
    {
      INIT_TRANSFORM_FACTOR();

      for(int j = 0; j < l; j++)
      {
        for(int x = j; x < nWidh; x += l2)
        {
          int i1 = x + l;
          const int original = x*ch;
          const int match = i1*ch;

          fft_single_fft2d(re, im, original, match, fR, fI, channels, cst, ch);
        }

        fft_increment_transform_factor(&fR, &fI, ca, sa);
      }
    }
  }

  // scale the result if inverse is called
  if(inverse)
  {
    fft_scale(GRe, GIm, nWidh, mHeight, channels, ch);
  }

  dt_free_align(lookup_ca);
  dt_free_align(lookup_sa);
}

void fft_FFT2D_R_Forward(float *const GRe, float *const GIm, const int nWidh, const int mHeight,
                const fft_decompose_channels channels, const dt_iop_colorspace_type_t cst, const int ch)
{
  const double pi = -M_PI;
  const int nWidh1 = nWidh * ch;

  // copy channels to the output buffers
  memset(GIm, 0, nWidh1*mHeight*sizeof(float));

  const int buffsize = fmax(nWidh, mHeight);
  float *lookup_ca = dt_alloc_align(64, buffsize*sizeof(float));
  float *lookup_sa = dt_alloc_align(64, buffsize*sizeof(float));

  load_lookup_ca_sa(lookup_ca, lookup_sa, pi, buffsize);

  //Bit reversal
  fft_bit_reversal_R_Forward(GRe, nWidh, mHeight, ch);

  //Calculate the FFT of the columns
#ifdef _FFT_MULTFR_
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(lookup_ca, lookup_sa) schedule(static)
#endif
#endif
  for (int x = 0; x < nWidh; x++) //for each column
  {
    // this is the 1D FFT:
    const int x1 = x * ch;

    int l2 = 1;
    for (int l=1; l < mHeight; l<<= 1)
    {
      INIT_TRANSFORM_FACTOR();

      for (int j = 0; j < l; j++)
      {
        for (int y = j; y < mHeight; y += l2)
        {
          const int i1 = y + l;
          const int original = nWidh1 * y + x1;
          const int match = nWidh1 * i1 + x1;

          fft_single_fft2d(GRe, GIm, original, match, fR, fI, channels, cst, ch);
        }

        fft_increment_transform_factor(&fR, &fI, ca, sa);
      }
    }
  }

// calculate the FFT of the rows
#ifdef _FFT_MULTFR_
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(lookup_ca, lookup_sa) schedule(static)
#endif
#endif
  for (int y = 0; y < mHeight; y++) // for each row
  {
    const int y1 = y*nWidh1;
    float *re = GRe+y1;
    float *im = GIm+y1;

    //This is the 1D FFT

    int l2 = 1;
    for(int l = 1; l < nWidh; l<<= 1)
    {
      INIT_TRANSFORM_FACTOR();

      for(int j = 0; j < l; j++)
      {
        for(int x = j; x < nWidh; x += l2)
        {
          int i1 = x + l;
          const int original = x*ch;
          const int match = i1*ch;

          fft_single_fft2d(re, im, original, match, fR, fI, channels, cst, ch);
        }

        fft_increment_transform_factor(&fR, &fI, ca, sa);
      }
    }
  }

  dt_free_align(lookup_ca);
  dt_free_align(lookup_sa);
}

void fft_FFT2D_R_Inverse(float *const GRe, float *const GIm, const int nWidh, const int mHeight,
                        const fft_decompose_channels channels, const dt_iop_colorspace_type_t cst, const int ch)
{
  const double pi = M_PI;
  const int nWidh1 = nWidh * ch;

  const int buffsize = fmax(nWidh, mHeight);
  float *lookup_ca = dt_alloc_align(64, buffsize*sizeof(float));
  float *lookup_sa = dt_alloc_align(64, buffsize*sizeof(float));

  load_lookup_ca_sa(lookup_ca, lookup_sa, pi, buffsize);

  //Bit reversal
  fft_bit_reversal(GRe, GIm, nWidh, mHeight, ch);

  //Calculate the FFT of the columns
#ifdef _FFT_MULTFR_
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(lookup_ca, lookup_sa) schedule(static)
#endif
#endif
  for (int x = 0; x < nWidh; x++) //for each column
  {
    // this is the 1D FFT:
    const int x1 = x * ch;

    int l2 = 1;
    for (int l=1; l < mHeight; l<<= 1)
    {
      INIT_TRANSFORM_FACTOR();

      for (int j = 0; j < l; j++)
      {
        for (int y = j; y < mHeight; y += l2)
        {
          const int i1 = y + l;
          const int original = nWidh1 * y + x1;
          const int match = nWidh1 * i1 + x1;

          fft_single_fft2d(GRe, GIm, original, match, fR, fI, channels, cst, ch);
        }

        fft_increment_transform_factor(&fR, &fI, ca, sa);
      }
    }
  }

// calculate the FFT of the rows
#ifdef _FFT_MULTFR_
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(lookup_ca, lookup_sa) schedule(static)
#endif
#endif
  for (int y = 0; y < mHeight; y++) // for each row
  {
    const int y1 = y*nWidh1;
    float *re = GRe+y1;
    float *im = GIm+y1;

    //This is the 1D FFT

    int l2 = 1;
    for(int l = 1; l < nWidh; l<<= 1)
    {
      INIT_TRANSFORM_FACTOR();

      for(int j = 0; j < l; j++)
      {
        for(int x = j; x < nWidh; x += l2)
        {
          int i1 = x + l;
          const int original = x*ch;
          const int match = i1*ch;

          fft_single_fft2d(re, im, original, match, fR, fI, channels, cst, ch);
        }

        fft_increment_transform_factor(&fR, &fI, ca, sa);
      }
    }
  }

  // scale the result if inverse is called
  fft_scale_R(GRe, /*GIm,*/ nWidh, mHeight, channels, ch);

  dt_free_align(lookup_ca);
  dt_free_align(lookup_sa);
}

// (FRe, FIm) = FFT + filter applied
// (FRe2, FIm2) = output of the filter
// image recomposed --> (FRe, FIm)
#ifdef _FFT_SSE_
void fft_recompose_image(float *GRe, float *GIm, float *gRe, float *gIm, const int nWidh, const int mHeight, const int ch)
{
    const int nWidh1 = nWidh*ch;

#ifdef _FFT_MULTFR_
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(GRe, GIm, gRe, gIm) schedule(static)
#endif
#endif
    for (int y = 0; y < mHeight; y++)
    {
      const int y1 = y*nWidh1;

      float *reO = &GRe[y1];
      float *imO = &GIm[y1];
      float *reI = &gRe[y1];
      float *imI = &gIm[y1];

      for (int x = 0; x < nWidh1; x+=ch)
      {
        _mm_store_ps( &(reO[x]), _mm_add_ps( _mm_load_ps( &(reO[x])), _mm_load_ps( &(reI[x])) ) );
        _mm_store_ps( &(imO[x]), _mm_add_ps( _mm_load_ps( &(imO[x])), _mm_load_ps( &(imI[x])) ) );
      }
    }
}
#else
void fft_recompose_image(float *GRe, float *GIm, float *gRe, float *gIm, const int nWidh, const int mHeight, const int ch)
{
    const int nWidh1 = nWidh*ch;

#ifdef _FFT_MULTFR_
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(GRe, GIm, gRe, gIm) schedule(static)
#endif
#endif
    for (int y = 0; y < mHeight; y++)
    {
      const int y1 = y*nWidh1;

      float *reO = &GRe[y1];
      float *imO = &GIm[y1];
      float *reI = &gRe[y1];
      float *imI = &gIm[y1];

      for (int x = 0; x < nWidh1; x+=ch)
      {
        if ( (reI[x] != 0 || imI[x] != 0)) {
          reO[x] += reI[x];
          imO[x] += imI[x];}
        if ( (reI[x+1] != 0 || imI[x+1] != 0)) {
          reO[x+1] += reI[x+1];
          imO[x+1] += imI[x+1];}
        if ( (reI[x+2] != 0 || imI[x+2] != 0)) {
          reO[x+2] += reI[x+2];
          imO[x+2] += imI[x+2];}
        if ( (reI[x+3] != 0 || imI[x+3] != 0)) {
          reO[x+3] += reI[x+3];
          imO[x+3] += imI[x+3];}
      }
    }
}
#endif

// return x, when x is a power of 2 and x >= n
int fft_convert_pow2(int n)
{
	int result = log2(n);
	result = pow(2, result);
	while (result < n) result *= 2;
	
	return result;
}

