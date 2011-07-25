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
#include "develop/imageop.h"
#include "common/opencl.h"
#include "dtgtk/slider.h"
#include "dtgtk/resetlabel.h"
#include "gui/gtk.h"
#include "common/darktable.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include <memory.h>
#include <stdlib.h>
#include <string.h>

// we assume people have -msee support.
#include <xmmintrin.h>

DT_MODULE(3)

typedef struct dt_iop_demosaic_params_t
{
  uint32_t green_eq;
  float median_thrs;
  uint32_t color_smoothing;
  uint32_t demosaicing_method;
  uint32_t yet_unused_data_specific_to_demosaicing_method;
}
dt_iop_demosaic_params_t;

typedef struct dt_iop_demosaic_gui_data_t
{
  GtkDarktableSlider *scale1;
  GtkToggleButton *greeneq;
  GtkWidget *color_smoothing;
  GtkComboBox *demosaic_method;
}
dt_iop_demosaic_gui_data_t;

typedef struct dt_iop_demosaic_global_data_t
{
  // demosaic pattern
  int kernel_green_eq;
  int kernel_pre_median;
  int kernel_ppg_green;
  int kernel_ppg_green_median;
  int kernel_ppg_redblue;
  int kernel_zoom_half_size;
  int kernel_downsample;
  int kernel_border_interpolate;
}
dt_iop_demosaic_global_data_t;

typedef struct dt_iop_demosaic_data_t
{
  // demosaic pattern
  uint32_t filters;
  uint32_t green_eq;
  uint32_t color_smoothing;
  uint32_t demosaicing_method;
  uint32_t yet_unused_data_specific_to_demosaicing_method;
  float median_thrs;
}
dt_iop_demosaic_data_t;

typedef enum dt_iop_demosaic_method_t
{
  DT_IOP_DEMOSAIC_PPG = 0,
  DT_IOP_DEMOSAIC_AMAZE = 1
}
dt_iop_demosaic_method_t;

static void
amaze_demosaic_RT(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const float *const in, float *out, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out, const int filters);

const char *
name()
{
  return _("demosaic");
}

int
groups ()
{
  return IOP_GROUP_BASIC;
}
void init_key_accels()
{
  dtgtk_slider_init_accel(darktable.control->accels_darkroom,"<Darktable>/darkroom/plugins/demosaic/edge threshold");
}

int
legacy_params (dt_iop_module_t *self, const void *const old_params, const int old_version, void *new_params, const int new_version)
{
  if(old_version == 2 && new_version == 3)
  {
    dt_iop_demosaic_params_t *o = (dt_iop_demosaic_params_t *)old_params;
    dt_iop_demosaic_params_t *n = (dt_iop_demosaic_params_t *)new_params;
    n->green_eq = o->green_eq;
    n->median_thrs = o->median_thrs;
    n->color_smoothing = 0;
    n->demosaicing_method = 0;
    n->yet_unused_data_specific_to_demosaicing_method = 0;
    return 0;
  }
  return 1;
}

static int
FC(const int row, const int col, const unsigned int filters)
{
  return filters >> (((row << 1 & 14) + (col & 1)) << 1) & 3;
}

#define SWAP(a, b) {const float tmp = (b); (b) = (a); (a) = tmp;}

static void
pre_median_b(float *out, const float *const in, const dt_iop_roi_t *const roi, const int filters, const int num_passes, const float threshold)
{
#if 1
  memcpy(out, in, roi->width*roi->height*sizeof(float));
#else
  // colors:
  const float thrsc = 2*threshold;
  for (int pass=0; pass < num_passes; pass++)
  {
    for (int c=0; c < 3; c+=2)
    {
      int rows = 3;
      if(FC(rows,3,filters) != c && FC(rows,4,filters) != c) rows++;
#ifdef _OPENMP
      #pragma omp parallel for default(none) shared(rows,c,out) schedule(static)
#endif
      for (int row=rows; row<roi->height-3; row+=2)
      {
        float med[9];
        int col = 3;
        if(FC(row,col,filters) != c) col++;
        float *pixo = out + roi->width * row + col;
        const float *pixi = in + roi->width * row + col;
        for(; col<roi->width-3; col+=2)
        {
          int cnt = 0;
          for (int k=0, i = -2*roi->width; i <= 2*roi->width; i += 2*roi->width)
          {
            for (int j = i-2; j <= i+2; j+=2)
            {
              if(fabsf(pixi[j] - pixi[0]) < thrsc)
              {
                med[k++] = pixi[j];
                cnt ++;
              }
              else med[k++] = 64.0f + pixi[j];
            }
          }
          for (int i=0; i<8; i++) for(int ii=i+1; ii<9; ii++) if(med[i] > med[ii]) SWAP(med[i], med[ii]);
#if 0
          // cnt == 1 and no small edge in greens.
          if(fabsf(pixi[-roi->width] - pixi[+roi->width]) + fabsf(pixi[-1] - pixi[+1])
              + fabsf(pixi[-roi->width] - pixi[+1]) + fabsf(pixi[-1] - pixi[+roi->width])
              + fabsf(pixi[+roi->width] - pixi[+1]) + fabsf(pixi[-1] - pixi[-roi->width])
              > 0.06)
            pixo[0] = med[(cnt-1)/2];
          else
#endif
            pixo[0] = (cnt == 1 ? med[4] - 64.0f : med[(cnt-1)/2]);
          pixo += 2;
          pixi += 2;
        }
      }
    }
  }
#endif

  // now green:
  const int lim[5] = {0, 1, 2, 1, 0};
  for (int pass=0; pass < num_passes; pass++)
  {
#ifdef _OPENMP
    #pragma omp parallel for default(none) shared(out) schedule(static)
#endif
    for (int row=3; row<roi->height-3; row++)
    {
      float med[9];
      int col = 3;
      if(FC(row,col,filters) != 1 && FC(row,col,filters) != 3) col++;
      float *pixo = out + roi->width * row + col;
      const float *pixi = in + roi->width * row + col;
      for(; col<roi->width-3; col+=2)
      {
        int cnt = 0;
        for (int k=0, i = 0; i < 5; i ++)
        {
          for (int j = -lim[i]; j <= lim[i]; j+=2)
          {
            if(fabsf(pixi[roi->width*(i-2) + j] - pixi[0]) < threshold)
            {
              med[k++] = pixi[roi->width*(i-2) + j];
              cnt++;
            }
            else med[k++] = 64.0f+pixi[roi->width*(i-2)+j];
          }
        }
        for (int i=0; i<8; i++) for(int ii=i+1; ii<9; ii++) if(med[i] > med[ii]) SWAP(med[i], med[ii]);
        pixo[0] = (cnt == 1 ? med[4] - 64.0f : med[(cnt-1)/2]);
        // pixo[0] = med[(cnt-1)/2];
        pixo += 2;
        pixi += 2;
      }
    }
  }
}

static void
pre_median(float *out, const float *const in, const dt_iop_roi_t *const roi, const int filters, const int num_passes, const float threshold)
{
  pre_median_b(out, in, roi, filters, num_passes, threshold);
}

#define SWAPmed(I,J) if (med[I] > med[J]) SWAP(med[I], med[J])

static void
color_smoothing(float *out, const dt_iop_roi_t *const roi_out, const int num_passes)
{
  const int width4 = 4 * roi_out->width;

  for (int pass=0; pass < num_passes; pass++)
  {
    for (int c=0; c < 3; c+=2)
    {
      float *outp = out;
      for (int j=0; j<roi_out->height; j++) for(int i=0; i<roi_out->width; i++,outp+=4)
          outp[3] = outp[c];
#ifdef _OPENMP
      #pragma omp parallel for schedule(static) default(none) shared(out,c)
#endif
      for (int j=1; j<roi_out->height-1; j++)
      {
        float *outp = out + 4*j*roi_out->width + 4;
        for(int i=1; i<roi_out->width-1; i++,outp+=4)
        {
          float med[9] =
          {
            outp[-width4-4+3] - outp[-width4-4+1],
            outp[-width4+0+3] - outp[-width4+0+1],
            outp[-width4+4+3] - outp[-width4+4+1],
            outp[-4+3] - outp[-4+1],
            outp[+0+3] - outp[+0+1],
            outp[+4+3] - outp[+4+1],
            outp[+width4-4+3] - outp[+width4-4+1],
            outp[+width4+0+3] - outp[+width4+0+1],
            outp[+width4+4+3] - outp[+width4+4+1],
          };
          /* optimal 9-element median search */
          SWAPmed(1,2);
          SWAPmed(4,5);
          SWAPmed(7,8);
          SWAPmed(0,1);
          SWAPmed(3,4);
          SWAPmed(6,7);
          SWAPmed(1,2);
          SWAPmed(4,5);
          SWAPmed(7,8);
          SWAPmed(0,3);
          SWAPmed(5,8);
          SWAPmed(4,7);
          SWAPmed(3,6);
          SWAPmed(1,4);
          SWAPmed(2,5);
          SWAPmed(4,7);
          SWAPmed(4,2);
          SWAPmed(6,4);
          SWAPmed(4,2);
          outp[c] = CLAMPS(med[4] + outp[1], 0.0f, 1.0f);
        }
      }
    }
  }
}
#undef SWAP

static void
green_equilibration(float *out, const float *const in, const int width, const int height, const uint32_t filters)
{
  int oj = 2, oi = 2;
  const float thr = 0.01f;
  const float maximum = 1.0f;
  if(FC(oj, oi, filters) != 1) oj++;
  if(FC(oj, oi, filters) != 1) oi++;
  if(FC(oj, oi, filters) != 1) oj--;

  memcpy(out,in,height*width*sizeof(float));

#ifdef _OPENMP
  #pragma omp parallel for schedule(static) default(none) shared(out,oi,oj)
#endif
  for(int j=oj; j<height-2; j+=2)
  {
    for(int i=oi; i<width-2; i+=2)
    {
      const float o1_1 = in[(j-1)*width+i-1];
      const float o1_2 = in[(j-1)*width+i+1];
      const float o1_3 = in[(j+1)*width+i-1];
      const float o1_4 = in[(j+1)*width+i+1];
      const float o2_1 = in[(j-2)*width+i];
      const float o2_2 = in[(j+2)*width+i];
      const float o2_3 = in[j*width+i-2];
      const float o2_4 = in[j*width+i+2];

      const float m1 = (o1_1+o1_2+o1_3+o1_4)/4.0f;
      const float m2 = (o2_1+o2_2+o2_3+o2_4)/4.0f;
      if (m2 > 0.0f)
      {
        const float c1 = (fabsf(o1_1-o1_2)+fabsf(o1_1-o1_3)+fabsf(o1_1-o1_4)+fabsf(o1_2-o1_3)+fabsf(o1_3-o1_4)+fabsf(o1_2-o1_4))/6.0f;
        const float c2 = (fabsf(o2_1-o2_2)+fabsf(o2_1-o2_3)+fabsf(o2_1-o2_4)+fabsf(o2_2-o2_3)+fabsf(o2_3-o2_4)+fabsf(o2_2-o2_4))/6.0f;
        if((in[j*width+i]<maximum*0.95f)&&(c1<maximum*thr)&&(c2<maximum*thr))
        {
          out[j*width+i] = in[j*width+i]*m1/m2;
        }
      }
    }
  }
}


/** 1:1 demosaic from in to out, in is full buf, out is translated/cropped (scale == 1.0!) */
static void
demosaic_ppg(float *out, const float *in, dt_iop_roi_t *roi_out, const dt_iop_roi_t *roi_in, const int filters, const float thrs)
{
  // snap to start of mosaic block:
  roi_out->x = 0;//MAX(0, roi_out->x & ~1);
  roi_out->y = 0;//MAX(0, roi_out->y & ~1);
  // offsets only where the buffer ends:
  const int offx = 3; //MAX(0, 3 - roi_out->x);
  const int offy = 3; //MAX(0, 3 - roi_out->y);
  const int offX = 3; //MAX(0, 3 - (roi_in->width  - (roi_out->x + roi_out->width)));
  const int offY = 3; //MAX(0, 3 - (roi_in->height - (roi_out->y + roi_out->height)));

  // border interpolate
  float sum[8];
  for (int j=0; j < roi_out->height; j++) for (int i=0; i < roi_out->width; i++)
    {
      if (i == offx && j >= offy && j < roi_out->height-offY)
        i = roi_out->width-offX;
      if(i == roi_out->width) break;
      memset (sum, 0, sizeof(float)*8);
      for (int y=j-1; y != j+2; y++) for (int x=i-1; x != i+2; x++)
        {
          const int yy = y + roi_out->y, xx = x + roi_out->x;
          if (yy >= 0 && xx >= 0 && yy < roi_in->height && xx < roi_in->width)
          {
            int f = FC(y,x,filters);
            sum[f] += in[yy*roi_in->width+xx];
            sum[f+4]++;
          }
        }
      int f = FC(j,i,filters);
      for(int c=0; c<3; c++)
      {
        if (c != f && sum[c+4] > 0.0f)
          out[4*(j*roi_out->width+i)+c] = sum[c] / sum[c+4];
        else
          out[4*(j*roi_out->width+i)+c] = in[(j+roi_out->y)*roi_in->width+i+roi_out->x];
      }
    }
  const int median = thrs > 0.0f;
  // if(median) fbdd_green(out, in, roi_out, roi_in, filters);
  if(median)
  {
    float *med_in = (float *)dt_alloc_align(16, roi_in->height*roi_in->width*sizeof(float));
    pre_median(med_in, in, roi_in, filters, 1, thrs);
    in = med_in;
  }
  // for all pixels: interpolate green into float array, or copy color.
#ifdef _OPENMP
  #pragma omp parallel for default(none) shared(roi_in, roi_out, in, out) schedule(static)
#endif
  for (int j=offy; j < roi_out->height-offY; j++)
  {
    float *buf = out + 4*roi_out->width*j + 4*offx;
    const float *buf_in = in + roi_in->width*(j + roi_out->y) + offx + roi_out->x;
    for (int i=offx; i < roi_out->width-offX; i++)
    {
      const int c = FC(j,i,filters);
      // prefetch what we need soon (load to cpu caches)
      _mm_prefetch((char *)buf_in + 256, _MM_HINT_NTA); // TODO: try HINT_T0-3
      _mm_prefetch((char *)buf_in +   roi_in->width + 256, _MM_HINT_NTA);
      _mm_prefetch((char *)buf_in + 2*roi_in->width + 256, _MM_HINT_NTA);
      _mm_prefetch((char *)buf_in + 3*roi_in->width + 256, _MM_HINT_NTA);
      _mm_prefetch((char *)buf_in -   roi_in->width + 256, _MM_HINT_NTA);
      _mm_prefetch((char *)buf_in - 2*roi_in->width + 256, _MM_HINT_NTA);
      _mm_prefetch((char *)buf_in - 3*roi_in->width + 256, _MM_HINT_NTA);
      __m128 col = _mm_load_ps(buf);
      float *color = (float*)&col;
      const float pc = buf_in[0];
      // if(__builtin_expect(c == 0 || c == 2, 1))
      if(c == 0 || c == 2)
      {
        color[c] = pc;
        // get stuff (hopefully from cache)
        const float pym  = buf_in[ - roi_in->width*1];
        const float pym2 = buf_in[ - roi_in->width*2];
        const float pym3 = buf_in[ - roi_in->width*3];
        const float pyM  = buf_in[ + roi_in->width*1];
        const float pyM2 = buf_in[ + roi_in->width*2];
        const float pyM3 = buf_in[ + roi_in->width*3];
        const float pxm  = buf_in[ - 1];
        const float pxm2 = buf_in[ - 2];
        const float pxm3 = buf_in[ - 3];
        const float pxM  = buf_in[ + 1];
        const float pxM2 = buf_in[ + 2];
        const float pxM3 = buf_in[ + 3];

        const float guessx = (pxm + pc + pxM) * 2.0f - pxM2 - pxm2;
        const float diffx  = (fabsf(pxm2 - pc) +
                              fabsf(pxM2 - pc) +
                              fabsf(pxm  - pxM)) * 3.0f +
                             (fabsf(pxM3 - pxM) + fabsf(pxm3 - pxm)) * 2.0f;
        const float guessy = (pym + pc + pyM) * 2.0f - pyM2 - pym2;
        const float diffy  = (fabsf(pym2 - pc) +
                              fabsf(pyM2 - pc) +
                              fabsf(pym  - pyM)) * 3.0f +
                             (fabsf(pyM3 - pyM) + fabsf(pym3 - pym)) * 2.0f;
        if(diffx > diffy)
        {
          // use guessy
          const float m = fminf(pym, pyM);
          const float M = fmaxf(pym, pyM);
          color[1] = fmaxf(fminf(guessy*.25f, M), m);
        }
        else
        {
          const float m = fminf(pxm, pxM);
          const float M = fmaxf(pxm, pxM);
          color[1] = fmaxf(fminf(guessx*.25f, M), m);
        }
      }
      else color[1] = pc;

      // write using MOVNTPS (write combine omitting caches)
      // _mm_stream_ps(buf, col);
      memcpy(buf, color, 4*sizeof(float));
      buf += 4;
      buf_in ++;
    }
  }
  // SFENCE (make sure stuff is stored now)
  // _mm_sfence();

  // for all pixels: interpolate colors into float array
#ifdef _OPENMP
  #pragma omp parallel for default(none) shared(roi_in, roi_out, out) schedule(static)
#endif
  for (int j=1; j < roi_out->height-1; j++)
  {
    float *buf = out + 4*roi_out->width*j + 4;
    for (int i=1; i < roi_out->width-1; i++)
    {
      // also prefetch direct nbs top/bottom
      _mm_prefetch((char *)buf + 256, _MM_HINT_NTA);
      _mm_prefetch((char *)buf - roi_out->width*4*sizeof(float) + 256, _MM_HINT_NTA);
      _mm_prefetch((char *)buf + roi_out->width*4*sizeof(float) + 256, _MM_HINT_NTA);

      const int c = FC(j, i, filters);
      __m128 col = _mm_load_ps(buf);
      float *color = (float *)&col;
      // fill all four pixels with correctly interpolated stuff: r/b for green1/2
      // b for r and r for b
      if(__builtin_expect(c & 1, 1)) // c == 1 || c == 3)
      {
        // calculate red and blue for green pixels:
        // need 4-nbhood:
        const float* nt = buf - 4*roi_out->width;
        const float* nb = buf + 4*roi_out->width;
        const float* nl = buf - 4;
        const float* nr = buf + 4;
        if(FC(j, i+1, filters) == 0) // red nb in same row
        {
          color[2] = (nt[2] + nb[2] + 2.0f*color[1] - nt[1] - nb[1])*.5f;
          color[0] = (nl[0] + nr[0] + 2.0f*color[1] - nl[1] - nr[1])*.5f;
        }
        else
        {
          // blue nb
          color[0] = (nt[0] + nb[0] + 2.0f*color[1] - nt[1] - nb[1])*.5f;
          color[2] = (nl[2] + nr[2] + 2.0f*color[1] - nl[1] - nr[1])*.5f;
        }
      }
      else
      {
        // get 4-star-nbhood:
        const float* ntl = buf - 4 - 4*roi_out->width;
        const float* ntr = buf + 4 - 4*roi_out->width;
        const float* nbl = buf - 4 + 4*roi_out->width;
        const float* nbr = buf + 4 + 4*roi_out->width;

        if(c == 0)
        {
          // red pixel, fill blue:
          const float diff1  = fabsf(ntl[2] - nbr[2]) + fabsf(ntl[1] - color[1]) + fabsf(nbr[1] - color[1]);
          const float guess1 = ntl[2] + nbr[2] + 2.0f*color[1] - ntl[1] - nbr[1];
          const float diff2  = fabsf(ntr[2] - nbl[2]) + fabsf(ntr[1] - color[1]) + fabsf(nbl[1] - color[1]);
          const float guess2 = ntr[2] + nbl[2] + 2.0f*color[1] - ntr[1] - nbl[1];
          if     (diff1 > diff2) color[2] = guess2 * .5f;
          else if(diff1 < diff2) color[2] = guess1 * .5f;
          else color[2] = (guess1 + guess2)*.25f;
        }
        else // c == 2, blue pixel, fill red:
        {
          const float diff1  = fabsf(ntl[0] - nbr[0]) + fabsf(ntl[1] - color[1]) + fabsf(nbr[1] - color[1]);
          const float guess1 = ntl[0] + nbr[0] + 2.0f*color[1] - ntl[1] - nbr[1];
          const float diff2  = fabsf(ntr[0] - nbl[0]) + fabsf(ntr[1] - color[1]) + fabsf(nbl[1] - color[1]);
          const float guess2 = ntr[0] + nbl[0] + 2.0f*color[1] - ntr[1] - nbl[1];
          if     (diff1 > diff2) color[0] = guess2 * .5f;
          else if(diff1 < diff2) color[0] = guess1 * .5f;
          else color[0] = (guess1 + guess2)*.25f;
        }
      }
      // _mm_stream_ps(buf, col);
      memcpy(buf, color, 4*sizeof(float));
      buf += 4;
    }
  }
  // _mm_sfence();
  if (median)
    free((float*)in);
}


// which roi input is needed to process to this output?
// roi_out is unchanged, full buffer in is full buffer out.
void
modify_roi_in (struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, const dt_iop_roi_t *roi_out, dt_iop_roi_t *roi_in)
{
  // this op is disabled for preview pipe/filters == 0

  *roi_in = *roi_out;
  // need 1:1, demosaic and then sub-sample. or directly sample half-size
  roi_in->x /= roi_out->scale;
  roi_in->y /= roi_out->scale;
  roi_in->width /= roi_out->scale;
  roi_in->height /= roi_out->scale;
  roi_in->scale = 1.0f;
  // clamp to even x/y, to make demosaic pattern still hold..
  roi_in->x = MAX(0, roi_in->x & ~1);
  roi_in->y = MAX(0, roi_in->y & ~1);

  // clamp numeric inaccuracies to full buffer, to avoid scaling/copying in pixelpipe:
  if(self->dev->image->width - roi_in->width < 10 && self->dev->image->height - roi_in->height < 10)
  {
    roi_in->width  = self->dev->image->width;
    roi_in->height = self->dev->image->height;
  }
}

void
process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *i, void *o, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_roi_t roi, roo;
  roi = *roi_in;
  roo = *roi_out;
  roo.x = roo.y = 0;
  // roi_out->scale = global scale: (iscale == 1.0, always when demosaic is on)

  dt_iop_demosaic_data_t *data = (dt_iop_demosaic_data_t *)piece->data;

  const float *const pixels = (float *)i;
  if(roi_out->scale > .999f)
  {
    // output 1:1
    // green eq:
    if(data->green_eq)
    {
      float *in = (float *)dt_alloc_align(16, roi_in->height*roi_in->width*sizeof(float));
      green_equilibration(in, pixels, roi_in->width, roi_in->height, data->filters);
      if (data->demosaicing_method != DT_IOP_DEMOSAIC_AMAZE)
        demosaic_ppg((float *)o, in, &roo, &roi, data->filters, data->median_thrs);
      else
        amaze_demosaic_RT(self, piece, in, (float *)o, &roi, &roo, data->filters);
      free(in);
    }
    else
    {
      if (data->demosaicing_method != DT_IOP_DEMOSAIC_AMAZE)
        demosaic_ppg((float *)o, pixels, &roo, &roi, data->filters, data->median_thrs);
      else
        amaze_demosaic_RT(self, piece, pixels, (float *)o, &roi, &roo, data->filters);
    }
  }
  else if(roi_out->scale > .5f)
  {
    // demosaic and then clip and zoom
    // roo.x = roi_out->x / global_scale;
    // roo.y = roi_out->y / global_scale;
    roo.width  = roi_out->width / roi_out->scale;
    roo.height = roi_out->height / roi_out->scale;
    roo.scale = 1.0f;

    float *tmp = (float *)dt_alloc_align(16, roo.width*roo.height*4*sizeof(float));
    if(data->green_eq)
    {
      float *in = (float *)dt_alloc_align(16, roi_in->height*roi_in->width*sizeof(float));
      green_equilibration(in, pixels, roi_in->width, roi_in->height, data->filters);
      if (data->demosaicing_method != DT_IOP_DEMOSAIC_AMAZE)
        demosaic_ppg(tmp, in, &roo, &roi, data->filters, data->median_thrs);
      else
        amaze_demosaic_RT(self, piece, in, tmp, &roi, &roo, data->filters);
      free(in);
    }
    else
    {
      if (data->demosaicing_method != DT_IOP_DEMOSAIC_AMAZE)
        demosaic_ppg(tmp, pixels, &roo, &roi, data->filters, data->median_thrs);
      else
        amaze_demosaic_RT(self, piece, pixels, tmp, &roi, &roo, data->filters);
    }
    roi = *roi_out;
    roi.x = roi.y = 0;
    roi.scale = roi_out->scale;
    dt_iop_clip_and_zoom((float *)o, tmp, &roi, &roo, roi.width, roo.width);
    free(tmp);
  }
  else
  {
    // sample half-size raw
    if(piece->pipe->type == DT_DEV_PIXELPIPE_EXPORT && data->median_thrs > 0.0f)
    {
      float *tmp = (float *)dt_alloc_align(16, sizeof(float)*roi_in->width*roi_in->height);
      pre_median_b(tmp, pixels, roi_in, data->filters, 1, data->median_thrs);
      dt_iop_clip_and_zoom_demosaic_half_size_f((float *)o, tmp, &roo, &roi, roo.width, roi.width, data->filters);
      free(tmp);
    }
    else
      dt_iop_clip_and_zoom_demosaic_half_size_f((float *)o, pixels, &roo, &roi, roo.width, roi.width, data->filters);
  }
  if(data->color_smoothing) color_smoothing(o, roi_out, data->color_smoothing);
}

#ifdef HAVE_OPENCL
int
process_cl (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
            const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_demosaic_data_t *data = (dt_iop_demosaic_data_t *)piece->data;
  dt_iop_demosaic_global_data_t *gd = (dt_iop_demosaic_global_data_t *)self->data;

  const int devid = piece->pipe->devid;
  size_t sizes[2] = {roi_out->width, roi_out->height};
  cl_mem dev_tmp = NULL;
  cl_mem dev_green_eq = NULL;
  cl_int err = -999;

  if(roi_out->scale > .99999f)
  {
    // 1:1 demosaic
    dev_green_eq = NULL;
    if(data->green_eq)
    {
      // green equilibration
      dev_green_eq = dt_opencl_alloc_device(devid, roi_in->width, roi_in->height, sizeof(float));
      if (dev_green_eq == NULL) goto error;      
      dt_opencl_set_kernel_arg(devid, gd->kernel_green_eq, 0, sizeof(cl_mem), &dev_in);
      dt_opencl_set_kernel_arg(devid, gd->kernel_green_eq, 1, sizeof(cl_mem), &dev_green_eq);
      dt_opencl_set_kernel_arg(devid, gd->kernel_green_eq, 2, sizeof(uint32_t), (void*)&data->filters);
      err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_green_eq, sizes);
      if(err != CL_SUCCESS) goto error;
      dev_in = dev_green_eq;
    }

    if(data->median_thrs > 0.0f)
    {
      const int one = 1;
      dt_opencl_set_kernel_arg(devid, gd->kernel_pre_median, 0, sizeof(cl_mem), &dev_in);
      dt_opencl_set_kernel_arg(devid, gd->kernel_pre_median, 1, sizeof(cl_mem), &dev_out);
      dt_opencl_set_kernel_arg(devid, gd->kernel_pre_median, 2, sizeof(uint32_t), (void*)&data->filters);
      dt_opencl_set_kernel_arg(devid, gd->kernel_pre_median, 3, sizeof(float), (void*)&data->median_thrs);
      dt_opencl_set_kernel_arg(devid, gd->kernel_pre_median, 4, sizeof(uint32_t), (void*)&one);
      err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_pre_median, sizes);
      if(err != CL_SUCCESS) goto error;

      dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_green_median, 0, sizeof(cl_mem), &dev_out);
      dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_green_median, 1, sizeof(cl_mem), &dev_out);
      dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_green_median, 2, sizeof(uint32_t), (void*)&data->filters);
      err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_ppg_green_median, sizes);
      if(err != CL_SUCCESS) goto error;
    }
    else
    {
      dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_green, 0, sizeof(cl_mem), &dev_in);
      dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_green, 1, sizeof(cl_mem), &dev_out);
      dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_green, 2, sizeof(uint32_t), (void*)&data->filters);
      err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_ppg_green, sizes);
      if(err != CL_SUCCESS) goto error;
    }

    dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_redblue, 0, sizeof(cl_mem), &dev_out);
    dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_redblue, 1, sizeof(cl_mem), &dev_out);
    dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_redblue, 2, sizeof(uint32_t), (void*)&data->filters);
    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_ppg_redblue, sizes);
    if(err != CL_SUCCESS) goto error;

    // manage borders
    dt_opencl_set_kernel_arg(devid, gd->kernel_border_interpolate, 0, sizeof(cl_mem), &dev_in);
    dt_opencl_set_kernel_arg(devid, gd->kernel_border_interpolate, 1, sizeof(cl_mem), &dev_out);
    dt_opencl_set_kernel_arg(devid, gd->kernel_border_interpolate, 2, sizeof(uint32_t), (void*)&roi_out->width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_border_interpolate, 3, sizeof(uint32_t), (void*)&roi_out->height);
    dt_opencl_set_kernel_arg(devid, gd->kernel_border_interpolate, 4, sizeof(uint32_t), (void*)&data->filters);
    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_border_interpolate, sizes);
    if(err != CL_SUCCESS) goto error;

  }
  else if(roi_out->scale > .5f)
  {
    // need to scale to right res
    dev_tmp = dt_opencl_alloc_device(devid, roi_in->width, roi_in->height, 4*sizeof(float));
    if (dev_tmp == NULL) goto error;
    sizes[0] = roi_in->width;
    sizes[1] = roi_in->height;
    if(data->green_eq)
    {
      // green equilibration
      dev_green_eq = dt_opencl_alloc_device(devid, roi_in->width, roi_in->height, sizeof(float));
      if (dev_green_eq == NULL) goto error;
      dt_opencl_set_kernel_arg(devid, gd->kernel_green_eq, 0, sizeof(cl_mem), &dev_in);
      dt_opencl_set_kernel_arg(devid, gd->kernel_green_eq, 1, sizeof(cl_mem), &dev_green_eq);
      dt_opencl_set_kernel_arg(devid, gd->kernel_green_eq, 2, sizeof(uint32_t), (void*)&data->filters);
      err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_green_eq, sizes);
      if(err != CL_SUCCESS) goto error;
      dev_in = dev_green_eq;
    }

    if(data->median_thrs > 0.0f)
    {
      dt_opencl_set_kernel_arg(devid, gd->kernel_pre_median, 0, sizeof(cl_mem), &dev_in);
      dt_opencl_set_kernel_arg(devid, gd->kernel_pre_median, 1, sizeof(cl_mem), &dev_tmp);
      dt_opencl_set_kernel_arg(devid, gd->kernel_pre_median, 2, sizeof(uint32_t), (void*)&data->filters);
      dt_opencl_set_kernel_arg(devid, gd->kernel_pre_median, 3, sizeof(float), (void*)&data->median_thrs);
      err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_pre_median, sizes);
      if(err != CL_SUCCESS) goto error;

      dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_green_median, 0, sizeof(cl_mem), &dev_tmp);
      dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_green_median, 1, sizeof(cl_mem), &dev_tmp);
      dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_green_median, 2, sizeof(uint32_t), (void*)&data->filters);
      err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_ppg_green_median, sizes);
      if(err != CL_SUCCESS) goto error;
    }
    else
    {
      dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_green, 0, sizeof(cl_mem), &dev_in);
      dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_green, 1, sizeof(cl_mem), &dev_tmp);
      dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_green, 2, sizeof(uint32_t), (void*)&data->filters);
      err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_ppg_green, sizes);
      if(err != CL_SUCCESS) goto error;
    }

    dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_redblue, 0, sizeof(cl_mem), &dev_tmp);
    dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_redblue, 1, sizeof(cl_mem), &dev_tmp);
    dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_redblue, 2, sizeof(uint32_t), (void*)&data->filters);
    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_ppg_redblue, sizes);
    if(err != CL_SUCCESS) goto error;

    // manage borders
    dt_opencl_set_kernel_arg(devid, gd->kernel_border_interpolate, 0, sizeof(cl_mem), &dev_in);
    dt_opencl_set_kernel_arg(devid, gd->kernel_border_interpolate, 1, sizeof(cl_mem), &dev_tmp);
    dt_opencl_set_kernel_arg(devid, gd->kernel_border_interpolate, 2, sizeof(uint32_t), (void*)&roi_in->width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_border_interpolate, 3, sizeof(uint32_t), (void*)&roi_in->height);
    dt_opencl_set_kernel_arg(devid, gd->kernel_border_interpolate, 4, sizeof(uint32_t), (void*)&data->filters);
    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_border_interpolate, sizes);
    if(err != CL_SUCCESS) goto error;

    // scale temp buffer to output buffer
    int zero = 0;
    sizes[0] = roi_out->width;
    sizes[1] = roi_out->height;
    dt_opencl_set_kernel_arg(devid, gd->kernel_downsample, 0, sizeof(cl_mem), &dev_tmp);
    dt_opencl_set_kernel_arg(devid, gd->kernel_downsample, 1, sizeof(cl_mem), &dev_out);
    dt_opencl_set_kernel_arg(devid, gd->kernel_downsample, 2, sizeof(int), (void*)&zero);
    dt_opencl_set_kernel_arg(devid, gd->kernel_downsample, 3, sizeof(int), (void*)&zero);
    dt_opencl_set_kernel_arg(devid, gd->kernel_downsample, 4, sizeof(int), (void*)&roi_out->width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_downsample, 5, sizeof(int), (void*)&roi_out->height);
    dt_opencl_set_kernel_arg(devid, gd->kernel_downsample, 6, sizeof(float), (void*)&roi_out->scale);
    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_downsample, sizes);
    if(err != CL_SUCCESS) goto error;
  }
  else
  {
    // sample half-size image:
    int zero = 0;
    cl_mem dev_pix = dev_in;
    if(piece->pipe->type == DT_DEV_PIXELPIPE_EXPORT && data->median_thrs > 0.0f)
    {
      sizes[0] = roi_in->width;
      sizes[1] = roi_in->height;
      dev_tmp = dt_opencl_alloc_device(devid, roi_in->width, roi_in->height, sizeof(float));
      if (dev_tmp == NULL) goto error;
      dt_opencl_set_kernel_arg(devid, gd->kernel_pre_median, 0, sizeof(cl_mem), &dev_in);
      dt_opencl_set_kernel_arg(devid, gd->kernel_pre_median, 1, sizeof(cl_mem), &dev_tmp);
      dt_opencl_set_kernel_arg(devid, gd->kernel_pre_median, 2, sizeof(uint32_t), (void*)&data->filters);
      dt_opencl_set_kernel_arg(devid, gd->kernel_pre_median, 3, sizeof(float), (void*)&data->median_thrs);
      dt_opencl_set_kernel_arg(devid, gd->kernel_pre_median, 4, sizeof(uint32_t), (void*)&zero);
      err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_pre_median, sizes);
      if(err != CL_SUCCESS) goto error;
      dev_pix = dev_tmp;
      sizes[0] = roi_out->width;
      sizes[1] = roi_out->height;
    }

    dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_half_size, 0, sizeof(cl_mem), &dev_pix);
    dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_half_size, 1, sizeof(cl_mem), &dev_out);
    dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_half_size, 2, sizeof(int), (void*)&zero);
    dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_half_size, 3, sizeof(int), (void*)&zero);
    dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_half_size, 4, sizeof(int), (void*)&roi_out->width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_half_size, 5, sizeof(int), (void*)&roi_out->height);
    dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_half_size, 6, sizeof(float), (void*)&roi_out->scale);
    dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_half_size, 7, sizeof(uint32_t), (void*)&data->filters);
    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_zoom_half_size, sizes);
    if(err != CL_SUCCESS) goto error;
  }

  if(dev_tmp != NULL) dt_opencl_release_mem_object(dev_tmp);
  if(dev_green_eq != NULL) dt_opencl_release_mem_object(dev_green_eq);
  return TRUE;

error:
  if (dev_tmp != NULL) dt_opencl_release_mem_object(dev_tmp);
  if (dev_green_eq != NULL) dt_opencl_release_mem_object(dev_green_eq);
  dt_print(DT_DEBUG_OPENCL, "[opencl_demosaic] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif

void init(dt_iop_module_t *module)
{
  module->params = malloc(sizeof(dt_iop_demosaic_params_t));
  module->default_params = malloc(sizeof(dt_iop_demosaic_params_t));
  module->default_enabled = 1;
  module->priority = 133; // module order created by iop_dependencies.py, do not edit!
  module->hide_enable_button = 1;
  module->params_size = sizeof(dt_iop_demosaic_params_t);
  module->gui_data = NULL;
  dt_iop_demosaic_params_t tmp = (dt_iop_demosaic_params_t)
  {
    0, 0.0f,0,0,0
  };
  memcpy(module->params, &tmp, sizeof(dt_iop_demosaic_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_demosaic_params_t));
}

void init_global(dt_iop_module_so_t *module)
{
  const int program = 0; // from programs.conf
  dt_iop_demosaic_global_data_t *gd = (dt_iop_demosaic_global_data_t *)malloc(sizeof(dt_iop_demosaic_global_data_t));
  module->data = gd;
  gd->kernel_zoom_half_size     = dt_opencl_create_kernel(program, "clip_and_zoom_demosaic_half_size");
  gd->kernel_ppg_green          = dt_opencl_create_kernel(program, "ppg_demosaic_green");
  gd->kernel_green_eq           = dt_opencl_create_kernel(program, "green_equilibration");
  gd->kernel_pre_median         = dt_opencl_create_kernel(program, "pre_median");
  gd->kernel_ppg_green_median   = dt_opencl_create_kernel(program, "ppg_demosaic_green_median");
  gd->kernel_ppg_redblue        = dt_opencl_create_kernel(program, "ppg_demosaic_redblue");
  gd->kernel_downsample         = dt_opencl_create_kernel(program, "clip_and_zoom");
  gd->kernel_border_interpolate = dt_opencl_create_kernel(program, "border_interpolate");
}

void cleanup(dt_iop_module_t *module)
{
  free(module->gui_data);
  module->gui_data = NULL;
  free(module->params);
  module->params = NULL;
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_demosaic_global_data_t *gd = (dt_iop_demosaic_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_zoom_half_size);
  dt_opencl_free_kernel(gd->kernel_ppg_green);
  dt_opencl_free_kernel(gd->kernel_pre_median);
  dt_opencl_free_kernel(gd->kernel_green_eq);
  dt_opencl_free_kernel(gd->kernel_ppg_green_median);
  dt_opencl_free_kernel(gd->kernel_ppg_redblue);
  dt_opencl_free_kernel(gd->kernel_downsample);
  dt_opencl_free_kernel(gd->kernel_border_interpolate);
  free(module->data);
  module->data = NULL;
}

void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *params, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_demosaic_params_t *p = (dt_iop_demosaic_params_t *)params;
  dt_iop_demosaic_data_t *d = (dt_iop_demosaic_data_t *)piece->data;
  d->filters = dt_image_flipped_filter(self->dev->image);
  if(!d->filters || pipe->type == DT_DEV_PIXELPIPE_PREVIEW) piece->enabled = 0;
  d->green_eq = p->green_eq;
  d->color_smoothing = p->color_smoothing;
  d->median_thrs = p->median_thrs;
  d->demosaicing_method = p->demosaicing_method;
}

void init_pipe     (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_demosaic_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe  (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
}

void gui_update   (struct dt_iop_module_t *self)
{
  dt_iop_demosaic_gui_data_t *g = (dt_iop_demosaic_gui_data_t *)self->gui_data;
  dt_iop_demosaic_params_t *p = (dt_iop_demosaic_params_t *)self->params;
  dtgtk_slider_set_value(g->scale1, p->median_thrs);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(g->color_smoothing), p->color_smoothing);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->greeneq), p->green_eq);
  gtk_combo_box_set_active(g->demosaic_method, p->demosaicing_method);
}

static void
median_thrs_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(darktable.gui->reset) return;
  dt_iop_demosaic_params_t *p = (dt_iop_demosaic_params_t *)self->params;
  p->median_thrs = dtgtk_slider_get_value(slider);
  if(p->median_thrs < 0.001f) p->median_thrs = 0.0f;
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void
color_smoothing_callback (GtkSpinButton *button, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(darktable.gui->reset) return;
  dt_iop_demosaic_params_t *p = (dt_iop_demosaic_params_t *)self->params;
  p->color_smoothing = gtk_spin_button_get_value(GTK_SPIN_BUTTON(button));
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void
greeneq_callback (GtkToggleButton *button, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(darktable.gui->reset) return;
  dt_iop_demosaic_params_t *p = (dt_iop_demosaic_params_t *)self->params;
  p->green_eq = gtk_toggle_button_get_active(button);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void
demosaic_method_callback (GtkComboBox *combo, dt_iop_module_t *self)
{
  dt_iop_demosaic_params_t *p = (dt_iop_demosaic_params_t *)self->params;
  int active = gtk_combo_box_get_active(combo);

  switch(active)
  {
    case DT_IOP_DEMOSAIC_AMAZE:
      p->demosaicing_method = DT_IOP_DEMOSAIC_AMAZE;
      break;
    default:
    case DT_IOP_DEMOSAIC_PPG:
      p->demosaicing_method = DT_IOP_DEMOSAIC_PPG;
      break;
  }
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

void gui_init     (struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_demosaic_gui_data_t));
  dt_iop_demosaic_gui_data_t *g = (dt_iop_demosaic_gui_data_t *)self->gui_data;
  dt_iop_demosaic_params_t *p = (dt_iop_demosaic_params_t *)self->params;

  self->widget = gtk_table_new(4, 2, FALSE);
  gtk_table_set_row_spacings(GTK_TABLE(self->widget), DT_GUI_IOP_MODULE_CONTROL_SPACING);
  gtk_table_set_col_spacings(GTK_TABLE(self->widget), DT_GUI_IOP_MODULE_CONTROL_SPACING);

  ////////////////////////////
  GtkWidget *label = dtgtk_reset_label_new(_("method"), self, &p->demosaicing_method, sizeof(uint32_t));
  gtk_table_attach(GTK_TABLE(self->widget), label, 0, 1, 0, 1, GTK_FILL, 0, 0, 0);
  g->demosaic_method = GTK_COMBO_BOX(gtk_combo_box_new_text());
  gtk_combo_box_append_text(g->demosaic_method, _("ppg"));
  gtk_combo_box_append_text(g->demosaic_method, _("AMaZE"));
  g_object_set(G_OBJECT(g->demosaic_method), "tooltip-text", _("demosaicing raw data method"), (char *)NULL);
  gtk_table_attach(GTK_TABLE(self->widget), GTK_WIDGET(g->demosaic_method), 1, 2, 0, 1, GTK_FILL|GTK_EXPAND, 0, 0, 0);
  ////////////////////////////

  g->scale1 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR, 0.0, 1.000, 0.001, p->median_thrs, 3));
  g_object_set(G_OBJECT(g->scale1), "tooltip-text", _("threshold for edge-aware median.\nset to 0.0 to switch off.\nset to 1.0 to ignore edges."), (char *)NULL);
  dtgtk_slider_set_label(g->scale1,_("edge threshold"));
  dtgtk_slider_set_accel(g->scale1,darktable.control->accels_darkroom,"<Darktable>/darkroom/plugins/demosaic/edge threshold");
  gtk_table_attach(GTK_TABLE(self->widget), GTK_WIDGET(g->scale1), 0, 2, 1, 2, GTK_FILL|GTK_EXPAND, 0, 0, 0);

  GtkWidget *widget;
  widget = dtgtk_reset_label_new(_("color smoothing"), self, &p->color_smoothing, sizeof(uint32_t));
  gtk_table_attach(GTK_TABLE(self->widget), widget, 0, 1, 2, 3, GTK_FILL, 0, 0, 0);
  g->color_smoothing = gtk_spin_button_new_with_range(0, 5, 1);
  gtk_spin_button_set_digits(GTK_SPIN_BUTTON(g->color_smoothing), 0);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(g->color_smoothing), p->color_smoothing);
  g_object_set(G_OBJECT(g->color_smoothing), "tooltip-text", _("how many color smoothing median steps after demosaicing"), (char *)NULL);
  gtk_table_attach(GTK_TABLE(self->widget), g->color_smoothing, 1, 2, 2, 3, GTK_FILL|GTK_EXPAND, 0, 0, 0);

  widget = dtgtk_reset_label_new(_("match greens"), self, &p->green_eq, sizeof(uint32_t));
  gtk_table_attach(GTK_TABLE(self->widget), widget, 0, 1, 3, 4, GTK_FILL, 0, 0, 0);
  g->greeneq = GTK_TOGGLE_BUTTON(gtk_check_button_new());
  g_object_set(G_OBJECT(g->greeneq), "tooltip-text", _("switch on green equilibration before demosaicing.\nnecessary for some mid-range cameras such as the EOS 400D."), (char *)NULL);
  gtk_table_attach(GTK_TABLE(self->widget), GTK_WIDGET(g->greeneq), 1, 2, 3, 4, GTK_FILL|GTK_EXPAND, 0, 0, 0);

  g_signal_connect (G_OBJECT (g->scale1), "value-changed",
                    G_CALLBACK (median_thrs_callback), self);
  g_signal_connect (G_OBJECT (g->color_smoothing), "value-changed",
                    G_CALLBACK (color_smoothing_callback), self);
  g_signal_connect (G_OBJECT (g->greeneq), "toggled",
                    G_CALLBACK (greeneq_callback), self);
  g_signal_connect (G_OBJECT (g->demosaic_method), "changed",
                    G_CALLBACK (demosaic_method_callback), self);
}

void gui_cleanup  (struct dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

#include "iop/amaze_demosaic_RT.cc"
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
