
#include "bauhaus/bauhaus.h"
#include "common/opencl.h"
#include "common/dtpthread.h"
#include "common/debug.h"
#include "control/control.h"
#include "fft.h"
#include <memory.h>
#include <stdlib.h>
#include <xmmintrin.h>


void fft_Threshold(float *const InputR, float *const InputI, float *const OutputR, float *const OutputI,
                    const int nWidh, const int mHeight,
                    const float range1, const float range2, const fft_decompose_channels channels, const fft_filter_type filter_type,
                    const dt_iop_colorspace_type_t cst, const int ch)
{
  const float Th1 = range1/100.f; // cutoff frequency
  const float Th2 = range2/100.f; // cutoff frequency
    
  float maxA = 0; // max(A(u, v))
  const int nWidh1 = nWidh*ch;
    
  memset(OutputR, 0, nWidh*mHeight*ch*sizeof(float));
  memset(OutputI, 0, nWidh*mHeight*ch*sizeof(float));

    // calculate max(A(u, v))
#ifdef _FFT_MULTFR_
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(maxA) schedule(static)
#endif
#endif
	for (int y=0; y < mHeight; y++)
	{
	  float *inI = &InputI[nWidh1*y];
		
		for (int x=0; x < nWidh1; x+=ch)
		{
			if (channels & FFT_DECOMPOSE_CH1) {
        if (fabs(inI[x]) > maxA) maxA = fabs(inI[x]);}
			if (channels & FFT_DECOMPOSE_CH2) {
        if (fabs(inI[x+1]) > maxA) maxA = fabs(inI[x+1]);}
			if (channels & FFT_DECOMPOSE_CH3) {
        if (fabs(inI[x+2]) > maxA) maxA = fabs(inI[x+2]);}
			if (channels & FFT_DECOMPOSE_CH4) {
        if (fabs(inI[x+3]) > maxA) maxA = fabs(inI[x+3]);}
		}
	}

#ifdef _FFT_MULTFR_
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(maxA) schedule(static)
#endif
#endif
	for (int y=0; y < mHeight; y++)
	{
		const int i = nWidh1*y;
		
    float *inR = &InputR[i];
    float *inI = &InputI[i];
    float *outR = &OutputR[i];
    float *outI = &OutputI[i];
		
		for (int x=0; x < nWidh1; x+=ch)
		{ 
			float abs_filter = 0;
			int count = 0;
			 
			if (channels & FFT_DECOMPOSE_CH1) {
				if (abs(inI[x]) > abs_filter) abs_filter += abs(inI[x]); count++;}
			if (channels & FFT_DECOMPOSE_CH2) {
				if (abs(inI[x+1]) > abs_filter) abs_filter += abs(inI[x+1]); count++;}
			if (channels & FFT_DECOMPOSE_CH3) {
				if (abs(inI[x+2]) > abs_filter) abs_filter += abs(inI[x+2]); count++;}
			if (channels & FFT_DECOMPOSE_CH4) {
				if (abs(inI[x+3]) > abs_filter) abs_filter += abs(inI[x+3]); count++;}

			abs_filter /= count;
			 
			int pass = (((filter_type == FFT_FILTER_TYPE_THRESHOLD) && (Th1*maxA <= abs_filter && abs_filter <= Th2*maxA)) ||
						((filter_type == FFT_FILTER_TYPE_THRESHOLD_INV) && (Th1*maxA >= abs_filter || abs_filter >= Th2*maxA)));
			
      if (!pass)
      {
        for (int i=0;i<ch;i++)
        {
          outR[x+i] = inR[x+i];
          outI[x+i] = inI[x+i];
          inR[x+i] = inI[x+i] = 0;
        }
      }
      else
      {
        if (!(channels & FFT_DECOMPOSE_CH1))
        {
          outR[x] = inR[x];
          outI[x] = inI[x];
          inR[x]=0; inI[x]=0;
        }

        if (!(channels & FFT_DECOMPOSE_CH2))
        {
          outR[x+1] = inR[x+1];
          outI[x+1] = inI[x+1];
          inR[x+1]=0; inI[x+1]=0;
        }

        if (!(channels & FFT_DECOMPOSE_CH3))
        {
          outR[x+2] = inR[x+2];
          outI[x+2] = inI[x+2];
          inR[x+2]=0; inI[x+2]=0;
        }

        if (!(channels & FFT_DECOMPOSE_CH4))
        {
          outR[x+3] = inR[x+3];
          outI[x+3] = inI[x+3];
          inR[x+3]=0; inI[x+3]=0;
        }
      }
    }
  }
}

void fft_PassFilter(float *const InputR, float *const InputI, float *const OutputR, float *const OutputI,
                    const int nWidh, const int mHeight,
                    const float range1, const float range2, const fft_decompose_channels channels, const fft_filter_type filter_type,
                    const dt_iop_colorspace_type_t cst, const int ch)
{
  const float max_freq = 0.7071067811865475;
  const float min_rng = 0;
  const float max_rng = 100;
  const float Fc1 = fmin(fmax(range1*(max_freq/max_rng),min_rng),max_rng); // cutoff frequency
  const float Fc2 = fmin(fmax(range2*(max_freq/max_rng),min_rng),max_rng); // cutoff frequency
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
    const int i = nWidh1*y;

    float *inR = &InputR[i];
    float *inI = &InputI[i];
    float *outR = &OutputR[i];
    float *outI = &OutputI[i];

    for (int col=0, x=0; x < nWidh1; col++, x+=ch)
    {
      const float m = (y-(float)mHeight/2)/(float)mHeight; // (u-M/2)/M   -0.5<m<0.5
      const float n = (col-(float)nWidh/2)/(float)nWidh; // (v-N/2)/N   -0.5<n<0.5
      const float r = sqrtf(powf(m,2) + powf(n,2)); // sqrt(m^2 + n^2)      [0, 0.7071067811865475]

      int pass = ( ((filter_type == FFT_FILTER_TYPE_BANDPASS) && (Fc1 <= r && r <= Fc2)) ||
          ((filter_type == FFT_FILTER_TYPE_BANDBLOCK) && (Fc1 >= r || r >= Fc2)) ||
          ((filter_type == FFT_FILTER_TYPE_LOWPASS) && (r >= Fc2)) ||
          ((filter_type == FFT_FILTER_TYPE_HIGHPASS) && (r <= Fc1)) );

      if (!pass)
      {
        for (int i=0;i<ch;i++)
        {
          outR[x+i] = inR[x+i];
          outI[x+i] = inI[x+i];
          inR[x+i] = inI[x+i] = 0;
        }
      }
      else
      {
        if (!(channels & FFT_DECOMPOSE_CH1))
        {
          outR[x] = inR[x];
          outI[x] = inI[x];
          inR[x]=0; inI[x]=0;
        }

        if (!(channels & FFT_DECOMPOSE_CH2))
        {
          outR[x+1] = inR[x+1];
          outI[x+1] = inI[x+1];
          inR[x+1]=0; inI[x+1]=0;
        }

        if (!(channels & FFT_DECOMPOSE_CH3))
        {
          outR[x+2] = inR[x+2];
          outI[x+2] = inI[x+2];
          inR[x+2]=0; inI[x+2]=0;
        }

        if (!(channels & FFT_DECOMPOSE_CH4))
        {
          outR[x+3] = inR[x+3];
          outI[x+3] = inI[x+3];
          inR[x+3]=0; inI[x+3]=0;
        }
      }
    }
  }
}

#define m_(col)  ( ((float)(col) - ((float)nWidh/2.f  )) / (float)nWidh) /* [-0.5, 0.5] */
#define n_(y)    ( ((float)(y)   - ((float)mHeight/2.f)) / (float)mHeight) /* [-0.5, 0.5] */

void fft_OtherFilter(float *const InputR, float *const InputI, float *const OutputR, float *const OutputI,
                    const int nWidh, const int mHeight,
                    const float range1, const float range2, const int sharpness, const fft_decompose_channels channels, const fft_filter_type filter_type,
                    const dt_iop_colorspace_type_t cst, const int ch)
{
  float R = fmax( fmax( fmax( -m_(0), m_(nWidh-1)), -n_(0)), n_(mHeight-1));

  const float max_freq = R/2.f;
  const float min_rng = 0;
  const float max_rng = 100;
  float maxvar = 0, minvar = 1;
/*  const float Fc1 = fmin(fmax(range1*(max_freq/max_rng),min_rng),max_rng); // cutoff frequency (0, 0.5]
  const float Fc2 = fmin(fmax(range2*(max_freq/max_rng),min_rng),max_rng); // cutoff frequency (0, 0.5]*/
  const float Fc2 = fmin(fmax(range2*(max_freq/max_rng),min_rng),max_rng); // cutoff frequency (0, 0.5]
  const int nWidh1 = nWidh*ch;

  fprintf(stderr, "R=%f, Fc2=%f, sharpness=%i\n", R, Fc2, sharpness);

  memset(OutputR, 0, nWidh*mHeight*ch*sizeof(float));
  memset(OutputI, 0, nWidh*mHeight*ch*sizeof(float));
/*
#ifdef _FFT_MULTFR_
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static)
#endif
#endif
*/  for (int y=0; y < mHeight; y++)
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

//--------------------------------------------------------------------
      {
        /*               1      f < catoff - w
            f H(x) =     0      f > catoff + w
                         else   1/2*(1-sin(pi*(f-cutoff)/2*w))
         */
      float f = sqrt((float)(y-mHeight/2)*(y-mHeight/2)+(col-nWidh/2)*(col-nWidh/2));
      if (f < range1 - range2)
        val = 1;
      else if (f > range1 + range2)
        val = 0;
      else
      {
        val = 0.5f*(1-sin(M_PI*(f-range1)/(2*range2)));
      }
      }

/*



//--------------------------------------------------------------------
      {
        float dist = sqrt((float)(y-mHeight/2)*(y-mHeight/2)+(col-nWidh/2)*(col-nWidh/2));
      // Butterworth Highpass
      if (filter_type == FFT_FILTER_TYPE_HIGHPASS_BUTTERWORTH)
        val = 1 / (1 + pow(range1/dist, (double)2*sharpness));

      // butterworth lowpass
      if (filter_type == FFT_FILTER_TYPE_LOWPASS_BUTTERWORTH)
        val = 1 / (1 + pow(dist/range1, (double)2*sharpness));

      // Gaussian Highpass
      if (filter_type == FFT_FILTER_TYPE_HIGHPASS_GAUSSIAN)
        val = 1 - exp(-dist*dist/(2*range1*range1));

      // Gaussian Lowpass
      if (filter_type == FFT_FILTER_TYPE_LOWPASS_GAUSSIAN)
        val = exp(-dist*dist/(2*range1*range1));
      }

//--------------------------------------------------------------------
       {
        int dv = (y < mHeight / 2) ? y : y - mHeight;
       int du = (col < nWidh / 2) ? col : col - nWidh;
       float dist = (float)(dv * dv + du * du);

       // Butterworth Highpass
       if (filter_type == FFT_FILTER_TYPE_HIGHPASS_BUTTERWORTH)
         val = 1 / (1 + pow((range1*range1) / dist, sharpness));

       // Butterworth Lowpass
       if (filter_type == FFT_FILTER_TYPE_LOWPASS_BUTTERWORTH)
         val = 1 / (1 + pow(dist / (range1*range1), sharpness));

       // Gaussian Highpass
       if (filter_type == FFT_FILTER_TYPE_HIGHPASS_GAUSSIAN)
         val = 1 - exp(dist / (-2*range1*range1));

       // Gaussian Lowpass
       if (filter_type == FFT_FILTER_TYPE_LOWPASS_GAUSSIAN)
         val = exp(dist / (-2*range1*range1));
       }
//------------------------------------------------------------------------

*/



      if (val < minvar) minvar = val;
      if (val > maxvar) maxvar = val;






      for (int i=0; i<ch; i++)
      {
  /*      if ( ((channels & FFT_DECOMPOSE_CH1) && i == 0) ||
            ((channels & FFT_DECOMPOSE_CH2) && i == 1) ||
            ((channels & FFT_DECOMPOSE_CH3) && i == 2) ||
            ((channels & FFT_DECOMPOSE_CH4) && i == 3) )
        {*/
          const float tR = inR[x+i];
          const float tI = inI[x+i];

          inR[x+i] *= val;
          inI[x+i] *= val;

          outR[x+i] = tR - inR[x+i];
          outI[x+i] = tI - inI[x+i];
  /*      }
        else
        {
          outR[x+i] = inR[x+i];
          outI[x+i] = inI[x+i];
          inR[x+i]=0; inI[x+i]=0;
        }*/
      }

      col++;
    }
  }
fprintf(stderr, "max=%f, min=%f\n", maxvar, minvar);
}

// filters a FD image (InputR, InputI)
// excluded data is left on (OutputR, OutputI)
// nWidh, mHeight: image dimentions
// range1: high range
// range2: low range
// channels: channels to filter
// filter_type: Lowpass, Highpass, Bandpass, Bandblock
void fft_apply_filter(float *const InputR, float *const InputI, float *OutputR, float *OutputI,
                      const int nWidh, const int mHeight,
                      const float range1, const float range2, const int sharpness, const fft_decompose_channels channels, const fft_filter_type filter_type,
                      const dt_iop_colorspace_type_t cst, const int ch)
{
  if (filter_type == FFT_FILTER_TYPE_THRESHOLD || filter_type == FFT_FILTER_TYPE_THRESHOLD_INV)
  {
    fft_Threshold(InputR, InputI, OutputR, OutputI, nWidh, mHeight, range1, range2, channels, filter_type, cst, ch);
  }
  else if (filter_type == FFT_FILTER_TYPE_HIGHPASS || filter_type == FFT_FILTER_TYPE_LOWPASS ||
            filter_type == FFT_FILTER_TYPE_BANDPASS || filter_type == FFT_FILTER_TYPE_BANDBLOCK)
  {
    fft_PassFilter(InputR, InputI, OutputR, OutputI, nWidh, mHeight, range1, range2, channels, filter_type, cst, ch);
  }
  else //if (filter_type == FFT_FILTER_TYPE_LOWPASS_BUTTERWORTH)
  {
    fft_OtherFilter(InputR, InputI, OutputR, OutputI, nWidh, mHeight, range1*10, range2*10, sharpness, channels, filter_type, cst, ch);
  }
}

// performs a bit reversal for all the rows and columns
static void fft_bit_reversal(float *GRe, float *GIm, const int nWidh, const int mHeight, const int ch)
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

// scale the result
#ifdef _FFT_SSE_
void fft_scale(float *const GRe, float *const GIm, const int nWidh, const int mHeight, const fft_decompose_channels channels, const int ch)
{
    const float factor = 1.f / (float)(nWidh*mHeight);
    const int nWidh1 = nWidh*ch;
    const float fRI[4] = { (channels & FFT_DECOMPOSE_CH1) ? factor : 1.f,
                            (channels & FFT_DECOMPOSE_CH2) ? factor : 1.f,
                            (channels & FFT_DECOMPOSE_CH3) ? factor : 1.f,
                            (channels & FFT_DECOMPOSE_CH4) ? factor : 1.f };

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
        const __m128 factor1 = _mm_load_ps(fRI);
        _mm_store_ps(&(re[x]), _mm_mul_ps(_mm_load_ps(&(re[x])), factor1));
        _mm_store_ps(&(im[x]), _mm_mul_ps(_mm_load_ps(&(im[x])), factor1));
      }
    }
}
#else
void fft_scale(float */*const*/ GRe, float */*const*/ GIm, const int nWidh, const int mHeight, const fft_decompose_channels channels, const int ch)
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
static void fft_single_fft2d(float *GRe, float *GIm, const int original, const int match, const float fR, const float fI,
                              const fft_decompose_channels channels, const dt_iop_colorspace_type_t cst, const int ch)
{
  float tR[4];
  float tI[4];

  //(tR, tI) = (fR, fI)*(GRe, GIm)[match]
  const __m128 tRa = _mm_sub_ps(_mm_mul_ps( _mm_set1_ps(fR), _mm_load_ps( &(GRe[match]))),
                                _mm_mul_ps( _mm_set1_ps(fI), _mm_load_ps( &(GIm[match]))));
  const __m128 tIa = _mm_add_ps(_mm_mul_ps( _mm_set1_ps(fR), _mm_load_ps( &(GIm[match]))),
                                _mm_mul_ps( _mm_set1_ps(fI), _mm_load_ps( &(GRe[match]))));
  _mm_store_ps(tR, tRa);
  _mm_store_ps(tI, tIa);

  // (GRe, GIm)[match] = (GRe, GIm)[original] - (tR, tI)
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

  // (GRe, GIm)[original] = (GRe, GIm)[original] + (tR, tI)
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
static void fft_increment_transform_factor(float *fR, float *fI, const float ca, const float sa)
{
  // (fR, fI) = (ca, sa)*(fR, fI)+(fR, fI)
  const float z =  (*fR * ca - *fI * sa)+*fR;
  *fI = (*fR * sa + *fI * ca)+*fI;
  *fR = z;
}

// initialize the transform fator for both row and cols processing
#define INIT_TRANSFORM_FACTOR()                                                                        \
    l2 = l*2;                                                                                          \
    float fR = 1.0; /* start value for transform factor */                                             \
    float fI = 0.0;                                                                                    \
    const double delta = pi / (double)l; /* angle increment */                                         \
    const double Sine = sin(delta * .5); /* auxiliary sin(delta / 2) */                                \
    const float ca = (-2. * Sine * Sine); /* multiplier for trigonometric recurrence */                \
    const float sa = (sin(delta));

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
  
  //Bit reversal
  fft_bit_reversal(GRe, GIm, nWidh, mHeight, ch);
  
  //Calculate the FFT of the columns
#ifdef _FFT_MULTFR_
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static)
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
#pragma omp parallel for default(none) schedule(static)
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
}

// (FRe, FIm) = FFT + filter applied
// (FRe2, FIm2) = output of the filter
// image recomposed --> (FRe, FIm)
void fft_recompose_image(float *GRe, float *GIm, float *gRe, float *gIm, const int nWidh, const int mHeight,
                          const fft_decompose_channels channels, const int ch)
{
    const int nWidh1 = nWidh*ch;

#ifdef _FFT_MULTFR_
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(GRe, GIm, gRe, gIm) schedule(static)
#endif
#endif
    for (int y = 0; y < mHeight; y++)
    {
      float *reO = &GRe[y*nWidh1];
      float *imO = &GIm[y*nWidh1];
      float *reI = &gRe[y*nWidh1];
      float *imI = &gIm[y*nWidh1];

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

// return x, when x is a power of 2 and x >= n
int fft_convert_pow2(int n)
{
	int result = log2(n);
	result = pow(2, result);
	while (result < n) result *= 2;
	
	return result;
}

// copy image to a buffer, assume buffer size >= image size
void fft_copy_image_to_buffer(const float *const image, float * buffer, const int w_img, const int h_img,
                              const int w_buff, const int h_buff, const int ch)
{
	memset(buffer, 0, w_buff*h_buff*ch*sizeof(float));
  const int rowsize = w_img*ch*sizeof(float);
  const int w_buff1 = w_buff*ch;
  const int w_img1 = w_img*ch;

#ifdef _FFT_MULTFR_
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(buffer) schedule(static)
#endif
#endif
  for (int y=0; y < h_img; y++) // copy each row
	{
		memcpy(&buffer[y*w_buff1], &image[y*w_img1], rowsize);
	}
}

// copy the buffer to image, assume buffer size >= image size
void fft_copy_buffer_to_image(float * image, const float *const buffer, const int w_img, const int h_img,
                                const int w_buff, const int h_buff, const int ch)
{
	const int rowsize = w_img*ch*sizeof(float);
  const int w_buff1 = w_buff*ch;
  const int w_img1 = w_img*ch;

#ifdef _FFT_MULTFR_
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(image) schedule(static)
#endif
#endif
  for (int y=0; y < h_img; y++) // copy each row
	{
		memcpy(&image[y*w_img1], &buffer[y*w_buff1], rowsize);
	}
}

