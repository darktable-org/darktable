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
#include <memory.h>
#include <stdlib.h>

// we assume people have -msee support.
#include <xmmintrin.h>

DT_MODULE(1)

typedef struct dt_iop_demosaic_params_t
{
  // TODO: hot pixels removal/denoise/green eq/whatever
  int32_t flags;
}
dt_iop_demosaic_params_t;

typedef struct dt_iop_demosaic_gui_data_t
{
}
dt_iop_demosaic_gui_data_t;

typedef struct dt_iop_demosaic_global_data_t
{
  // demosaic pattern
  int kernel_ppg_green;
  int kernel_ppg_redblue;
  int kernel_zoom_half_size;
  int kernel_downsample;
}
dt_iop_demosaic_global_data_t;

typedef struct dt_iop_demosaic_data_t
{
  // demosaic pattern
  uint32_t filters;
}
dt_iop_demosaic_data_t;

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

static int
FC(const int row, const int col, const unsigned int filters)
{
  return filters >> ((((row) << 1 & 14) + ((col) & 1)) << 1) & 3;
}

/** 1:1 demosaic from in to out, in is full buf, out is translated/cropped (scale == 1.0!) */
static void
demosaic_ppg(float *out, const uint16_t *in, dt_iop_roi_t *roi_out, const dt_iop_roi_t *roi_in, const int filters)
{
  // snap to start of mosaic block:
  roi_out->x = MAX(0, roi_out->x & ~1);
  roi_out->y = MAX(0, roi_out->y & ~1);
  // offsets only where the buffer ends:
  const int offx = MAX(0, 3 - roi_out->x);
  const int offy = MAX(0, 3 - roi_out->y);
  const int offX = MAX(0, 3 - (roi_in->width  - (roi_out->x + roi_out->width)));
  const int offY = MAX(0, 3 - (roi_in->height - (roi_out->y + roi_out->height)));
  const float i2f = 1.0f/((float)0xffff);
  // for all pixels: interpolate green into float array, or copy color.
#ifdef _OPENMP
  #pragma omp parallel for default(none) shared(roi_in, roi_out, out, in) schedule(static)
#endif
  for (int j=offy; j < roi_out->height-offY; j++)
  {
    float *buf = out + 4*roi_out->width*j;
    const uint16_t *buf_in = in + roi_in->width*(j + roi_out->y) + offx + roi_out->x;
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
      __m128 col;// = _mm_set1_ps(100.0f);//_mm_setzero_ps();
      float *color = (float*)&col;
      const float pc = buf_in[0];
      // if(__builtin_expect(c == 0 || c == 2, 1))
      if(c == 0 || c == 2)
      {
        color[c] = i2f*pc; 
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
          color[1] = i2f*fmaxf(fminf(guessy*.25f, M), m);
        }
        else
        {
          const float m = fminf(pxm, pxM);
          const float M = fmaxf(pxm, pxM);
          color[1] = i2f*fmaxf(fminf(guessx*.25f, M), m);
        }
      }
      else color[1] = i2f*pc; 

      // write using MOVNTPS (write combine omitting caches)
      // _mm_stream_ps(buf, col);
      memcpy(buf, color, 4*sizeof(float));
      buf += 4;
      buf_in ++;
    }
  }
  // SFENCE (make sure stuff is stored now)
  // _mm_sfence();
  // return;

#if 0
  // get offsets in aligned block
  const int g1x = FC(0, 0, filters) & 1 ? 0 : 1;
  const int g2x = FC(0, 0, filters) & 1 ? 1 : 0;
  const int ry = (FC(0, 0, filters) == 1 || FC(0, 0, filters) == 1) ? 0 : 1;
  const int rx = FC(ry, 0, filters) == 1 ? 0 : 1;
  const int gy = 1-ry;
  const int gx = 1-rx;
#endif

  // for all pixels: interpolate colors into float array
#ifdef _OPENMP
  #pragma omp parallel for default(none) shared(roi_in, roi_out, out, in) schedule(static)
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
      // __m128 col = _mm_loadr_ps(buf);
      // __m128 col = _mm_set_ps(buf[0], buf[1], buf[2], buf[3]);
      float *color = (float *)&col;
      // fill all four pixels with correctly interpolated stuff: r/b for green1/2
      // b for r and r for b
      if(__builtin_expect(c & 1, 1)) // c == 1 || c == 3)
      { // calculate red and blue for green pixels:
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
        { // blue nb
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
        { // red pixel, fill blue:
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
}


void
process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *i, void *o, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_roi_t roi, roo;
  roi.scale = 1.0f;
  roi.x = roi.y = 0;
  roi.width  = self->dev->image->width;
  roi.height = self->dev->image->height;
  roo = *roi_out;
  // global scale:
  const float global_scale = roi_out->scale / piece->iscale;
  roo.scale = roi_out->scale/piece->iscale;

  dt_iop_demosaic_data_t *data = (dt_iop_demosaic_data_t *)piece->data;

  // data->filters = 0x61616161;//0x49494949;//0x94949494;
  data->filters = 0x94949494;
  // data->filters = 0x49494949;
  dt_image_t *img = self->dev->image;
  dt_image_buffer_t full = dt_image_get(img, DT_IMAGE_FULL, 'r');
  if(full != DT_IMAGE_FULL) return;
  if(!self->dev->image->filters)
  {
    // no bayer pattern, directly clip and zoom image
    dt_iop_clip_and_zoom((float *)o, (float *)i, roi_out, &roi);
  }
  else if(global_scale > .999f)
  {
    // output 1:1
    demosaic_ppg((float *)o, (const uint16_t *)self->dev->image->pixels, &roo, &roi, data->filters);
  }
  else if(global_scale > .5f)
  {
    // demosaic and then clip and zoom
    roo.x = roi_out->x / global_scale;
    roo.y = roi_out->y / global_scale;
    roo.width  = roi_out->width / global_scale;
    roo.height = roi_out->height / global_scale;
    roo.scale = 1.0f;
     
    float *tmp = (float *)malloc(roo.width*roo.height*4*sizeof(float));
    demosaic_ppg(tmp, (const uint16_t *)self->dev->image->pixels, &roo, &roi, data->filters);
    roi = *roi_out;
    roi.x = roi.y = 0;
    roi.scale = global_scale;
    dt_iop_clip_and_zoom((float *)o, tmp, &roi, &roo);
    free(tmp);
  }
  else
  {
    // sample half-size raw
    dt_iop_clip_and_zoom_demosaic_half_size((float *)o, (const uint16_t *)self->dev->image->pixels, &roo, &roi, data->filters);
  }
  dt_image_release(img, DT_IMAGE_FULL, 'r');
}

#ifdef HAVE_OPENCL
void
process_cl (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *i, void *o, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_demosaic_data_t *data = (dt_iop_demosaic_data_t *)piece->data;
  dt_iop_demosaic_global_data_t *gd = (dt_iop_demosaic_global_data_t *)self->data;
  // const int ch = piece->colors;
  // assert(ch == 4);
  // global scale is roi scale and pipe input prescale
  const float global_scale = roi_in->scale / piece->iscale;
  // const uint16_t *in = (const uint16_t *)i;
  float *out = (float *)o;

  const int devid = piece->pipe->devid;
  size_t sizes[2] = {roi_out->width, roi_out->height};
  size_t origin[] = {0, 0, 0};
  // TODO: input image region!
  size_t region[] = {self->dev->image->width, self->dev->image->height, 1};
  size_t region_out[] = {roi_out->width, roi_out->height, 1};
  cl_int err;
  cl_mem dev_in, dev_out, dev_tmp = NULL;
  // as images (texture memory)
  cl_image_format fmt1 = {CL_LUMINANCE, CL_UNSIGNED_INT16};
  cl_image_format fmt4 = {CL_RGBA, CL_FLOAT};
  dev_out = clCreateImage2D (darktable.opencl->dev[devid].context,
      CL_MEM_READ_WRITE,
      &fmt4,
      sizes[0], sizes[1], 0,
      NULL, &err);
  if(err != CL_SUCCESS) fprintf(stderr, "could not alloc/copy out buffer on device: %d\n", err);
  
  printf("using filters %X\n", data->filters);

  data->filters = 0x94949494;
  if(0)//!data->filters)
  {
    // TODO:
#if 0
    // actually no demosaic is needed at all
    dt_iop_roi_t roi;
    roi.x = ((int)(roi_in->x/global_scale)) & ~0x1; roi.y = ((int)(roi_in->y/global_scale)) & ~0x1;
    roi.width = roi_in->width/global_scale; roi.height = roi_in->height/global_scale;
    dev_in = clCreateImage2D (darktable.opencl->dev[devid].context,
        CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR,
        &fmt4,
        roi.width, roi.height, sizeof(uint16_t)*region[0],
        ((uint16_t *)self->dev->image->pixels) + roi.y*region[0] + roi.x, &err);
    // scale temp buffer to output buffer
    int zero = 0;
    sizes[0] = roi_out->width; sizes[1] = roi_out->height;
    err = dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_downsample, 0, sizeof(cl_mem), &dev_in);
    if(err != CL_SUCCESS) fprintf(stderr, "param 1 setting failed: %d\n", err);
    err = dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_downsample, 1, sizeof(cl_mem), &dev_out);
    if(err != CL_SUCCESS) fprintf(stderr, "param 2 setting failed: %d\n", err);
    err = dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_downsample, 2, sizeof(int), (void*)&zero);
    if(err != CL_SUCCESS) fprintf(stderr, "param 3 setting failed: %d\n", err);
    err = dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_downsample, 3, sizeof(int), (void*)&zero);
    if(err != CL_SUCCESS) fprintf(stderr, "param 4 setting failed: %d\n", err);
    err = dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_downsample, 4, sizeof(int), (void*)&roi_out->width);
    if(err != CL_SUCCESS) fprintf(stderr, "param 5 setting failed: %d\n", err);
    err = dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_downsample, 5, sizeof(int), (void*)&roi_out->height);
    if(err != CL_SUCCESS) fprintf(stderr, "param 6 setting failed: %d\n", err);
    err = dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_downsample, 6, sizeof(float), (void*)&global_scale);
    if(err != CL_SUCCESS) fprintf(stderr, "param 7 setting failed: %d\n", err);
    err = dt_opencl_enqueue_kernel_2d(darktable.opencl, devid, gd->kernel_downsample, sizes);
#endif
  }
  else if(global_scale > .999f)
  {
    // 1:1 demosaic
    size_t origin_in[] = {((size_t)roi_out->x)&~0x1, ((size_t)roi_out->y)&~0x1, 0};
    dev_in = clCreateImage2D (darktable.opencl->dev[devid].context,
        CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR,
        &fmt1,
        region_out[0], region_out[1], sizeof(uint16_t)*region[0],
        ((uint16_t *)self->dev->image->pixels) + origin_in[1]*region[0] + origin_in[0], &err);
    if(err != CL_SUCCESS) fprintf(stderr, "could not alloc/copy img buffer on device: %d\n", err);
    // clEnqueueWriteImage(darktable.opencl->cmd_queue, dev_in, CL_FALSE, origin_in, region_out, region[0]*sizeof(uint16_t), 0, ((uint16_t *)self->dev->image->pixels) + origin_in[1]*region[0] + origin_in[0], 0, NULL, NULL);
    // demosaic!
    err = dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_ppg_green, 0, sizeof(cl_mem), &dev_in);
    if(err != CL_SUCCESS) fprintf(stderr, "param 1 setting failed: %d\n", err);
    err = dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_ppg_green, 1, sizeof(cl_mem), &dev_out);
    if(err != CL_SUCCESS) fprintf(stderr, "param 2 setting failed: %d\n", err);
    err = dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_ppg_green, 2, sizeof(uint32_t), (void*)&data->filters);
    if(err != CL_SUCCESS) fprintf(stderr, "param 3 setting failed: %d\n", err);
    err = dt_opencl_enqueue_kernel_2d(darktable.opencl, devid, gd->kernel_ppg_green, sizes);

    err = dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_ppg_redblue, 0, sizeof(cl_mem), &dev_out);
    if(err != CL_SUCCESS) fprintf(stderr, "param 1 setting failed: %d\n", err);
    err = dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_ppg_redblue, 1, sizeof(cl_mem), &dev_out);
    if(err != CL_SUCCESS) fprintf(stderr, "param 2 setting failed: %d\n", err);
    err = dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_ppg_redblue, 2, sizeof(uint32_t), (void*)&data->filters);
    if(err != CL_SUCCESS) fprintf(stderr, "param 3 setting failed: %d\n", err);
    err = dt_opencl_enqueue_kernel_2d(darktable.opencl, devid, gd->kernel_ppg_redblue, sizes);
  }
  else if(global_scale > .5f)
  {
    // need to scale to right res
    dt_iop_roi_t roi;
    roi.x = ((int)(roi_in->x/global_scale)) & ~0x1; roi.y = ((int)(roi_in->y/global_scale)) & ~0x1;
    roi.width = roi_in->width/global_scale; roi.height = roi_in->height/global_scale;
    dev_in = clCreateImage2D (darktable.opencl->dev[devid].context,
        CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR,
        &fmt1,
        roi.width, roi.height, sizeof(uint16_t)*region[0],
        ((uint16_t *)self->dev->image->pixels) + roi.y*region[0] + roi.x, &err);
    if(err != CL_SUCCESS) fprintf(stderr, "could not alloc/copy img buffer on device: %d\n", err);
    dev_tmp = clCreateImage2D (darktable.opencl->dev[devid].context,
        CL_MEM_READ_WRITE,
        &fmt4,
        roi.width, roi.height, 0,
        NULL, &err);
    if(err != CL_SUCCESS) fprintf(stderr, "could not alloc tmp buffer on device: %d\n", err);

    sizes[0] = roi.width; sizes[1] = roi.height;
    err = dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_ppg_green, 0, sizeof(cl_mem), &dev_in);
    if(err != CL_SUCCESS) fprintf(stderr, "param 1 setting failed: %d\n", err);
    err = dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_ppg_green, 1, sizeof(cl_mem), &dev_tmp);
    if(err != CL_SUCCESS) fprintf(stderr, "param 2 setting failed: %d\n", err);
    err = dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_ppg_green, 2, sizeof(uint32_t), (void*)&data->filters);
    if(err != CL_SUCCESS) fprintf(stderr, "param 3 setting failed: %d\n", err);
    err = dt_opencl_enqueue_kernel_2d(darktable.opencl, devid, gd->kernel_ppg_green, sizes);

    err = dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_ppg_redblue, 0, sizeof(cl_mem), &dev_tmp);
    if(err != CL_SUCCESS) fprintf(stderr, "param 1 setting failed: %d\n", err);
    err = dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_ppg_redblue, 1, sizeof(cl_mem), &dev_tmp);
    if(err != CL_SUCCESS) fprintf(stderr, "param 2 setting failed: %d\n", err);
    err = dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_ppg_redblue, 2, sizeof(uint32_t), (void*)&data->filters);
    if(err != CL_SUCCESS) fprintf(stderr, "param 3 setting failed: %d\n", err);
    err = dt_opencl_enqueue_kernel_2d(darktable.opencl, devid, gd->kernel_ppg_redblue, sizes);

    // scale temp buffer to output buffer
    int zero = 0;
    sizes[0] = roi_out->width; sizes[1] = roi_out->height;
    err = dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_downsample, 0, sizeof(cl_mem), &dev_tmp);
    if(err != CL_SUCCESS) fprintf(stderr, "param 1 setting failed: %d\n", err);
    err = dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_downsample, 1, sizeof(cl_mem), &dev_out);
    if(err != CL_SUCCESS) fprintf(stderr, "param 2 setting failed: %d\n", err);
    err = dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_downsample, 2, sizeof(int), (void*)&zero);
    if(err != CL_SUCCESS) fprintf(stderr, "param 3 setting failed: %d\n", err);
    err = dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_downsample, 3, sizeof(int), (void*)&zero);
    if(err != CL_SUCCESS) fprintf(stderr, "param 4 setting failed: %d\n", err);
    err = dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_downsample, 4, sizeof(int), (void*)&roi_out->width);
    if(err != CL_SUCCESS) fprintf(stderr, "param 5 setting failed: %d\n", err);
    err = dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_downsample, 5, sizeof(int), (void*)&roi_out->height);
    if(err != CL_SUCCESS) fprintf(stderr, "param 6 setting failed: %d\n", err);
    err = dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_downsample, 6, sizeof(float), (void*)&global_scale);
    if(err != CL_SUCCESS) fprintf(stderr, "param 7 setting failed: %d\n", err);
    err = dt_opencl_enqueue_kernel_2d(darktable.opencl, devid, gd->kernel_downsample, sizes);
  }
  else
  {
    // sample half-size image:
    dev_in = clCreateImage2D (darktable.opencl->dev[devid].context,
        CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR,
        &fmt1,
        region[0], region[1], 0,
        self->dev->image->pixels, &err);
    if(err != CL_SUCCESS) fprintf(stderr, "could not alloc/copy img buffer on device: %d\n", err);
    // clEnqueueWriteImage(darktable.opencl->cmd_queue, dev_in, CL_FALSE, origin, region, region[0]*sizeof(uint16_t), 0, (uint16_t *)self->dev->image->pixels, 0, NULL, NULL);
    err = dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_zoom_half_size, 0, sizeof(cl_mem), &dev_in);
    if(err != CL_SUCCESS) fprintf(stderr, "param 1 setting failed: %d\n", err);
    err = dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_zoom_half_size, 1, sizeof(cl_mem), &dev_out);
    if(err != CL_SUCCESS) fprintf(stderr, "param 2 setting failed: %d\n", err);
    err = dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_zoom_half_size, 2, sizeof(int), (void*)&roi_out->x);
    if(err != CL_SUCCESS) fprintf(stderr, "param 3 setting failed: %d\n", err);
    err = dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_zoom_half_size, 3, sizeof(int), (void*)&roi_out->y);
    if(err != CL_SUCCESS) fprintf(stderr, "param 4 setting failed: %d\n", err);
    err = dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_zoom_half_size, 4, sizeof(int), (void*)&roi_out->width);
    if(err != CL_SUCCESS) fprintf(stderr, "param 5 setting failed: %d\n", err);
    err = dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_zoom_half_size, 5, sizeof(int), (void*)&roi_out->height);
    if(err != CL_SUCCESS) fprintf(stderr, "param 6 setting failed: %d\n", err);
    err = dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_zoom_half_size, 6, sizeof(float), (void*)&global_scale);
    if(err != CL_SUCCESS) fprintf(stderr, "param 7 setting failed: %d\n", err);
    err = dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_zoom_half_size, 7, sizeof(uint32_t), (void*)&data->filters);
    if(err != CL_SUCCESS) fprintf(stderr, "param 8 setting failed: %d\n", err);

    err = dt_opencl_enqueue_kernel_2d(darktable.opencl, devid, gd->kernel_zoom_half_size, sizes);
  }

  // double start = dt_get_wtime();
  // clEnqueueReadImage(cmd_queue, dev_out, CL_FALSE, orig0, region, 4*width*sizeof(float), 0, out + 4*(width*origin[1] + origin[0]), 0, NULL, NULL);
  // blocking:
  clEnqueueReadImage(darktable.opencl->dev[devid].cmd_queue, dev_out, CL_TRUE, origin, region_out, 4*region_out[0]*sizeof(float), 0, out, 0, NULL, NULL);
  // double end = dt_get_wtime();
  // dt_print(DT_DEBUG_PERF, "[demosaic] took %.3f secs\n", end - start);

  // for(int k=0;k<roi_out->width*roi_out->height;k++) for(int c=0;c<3;c++) out[3*k+c] = out[4*k+c];

  clReleaseMemObject(dev_in);
  clReleaseMemObject(dev_out);
  if(dev_tmp) clReleaseMemObject(dev_tmp);
}
#endif

void init(dt_iop_module_t *module)
{
  module->params = malloc(sizeof(dt_iop_demosaic_params_t));
  module->default_params = malloc(sizeof(dt_iop_demosaic_params_t));
  module->default_enabled = 1;
  module->priority = 1;
  module->hide_enable_button = 1;
  module->params_size = sizeof(dt_iop_demosaic_params_t);
  module->gui_data = NULL;
  dt_iop_demosaic_params_t tmp = (dt_iop_demosaic_params_t){0};
  memcpy(module->params, &tmp, sizeof(dt_iop_demosaic_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_demosaic_params_t));

  const int program = 0; // from programs.conf
  dt_iop_demosaic_global_data_t *gd = (dt_iop_demosaic_global_data_t *)malloc(sizeof(dt_iop_demosaic_global_data_t));
  module->data = gd;
  gd->kernel_zoom_half_size = dt_opencl_create_kernel(darktable.opencl, program, "clip_and_zoom_demosaic_half_size");
  gd->kernel_ppg_green      = dt_opencl_create_kernel(darktable.opencl, program, "ppg_demosaic_green");
  gd->kernel_ppg_redblue    = dt_opencl_create_kernel(darktable.opencl, program, "ppg_demosaic_redblue");
  gd->kernel_downsample     = dt_opencl_create_kernel(darktable.opencl, program, "clip_and_zoom");
}

void cleanup(dt_iop_module_t *module)
{
  free(module->gui_data);
  module->gui_data = NULL;
  free(module->params);
  module->params = NULL;
  dt_iop_demosaic_global_data_t *gd = (dt_iop_demosaic_global_data_t *)module->data;
  dt_opencl_free_kernel(darktable.opencl, gd->kernel_zoom_half_size);
  dt_opencl_free_kernel(darktable.opencl, gd->kernel_ppg_green);
  dt_opencl_free_kernel(darktable.opencl, gd->kernel_ppg_redblue);
  dt_opencl_free_kernel(darktable.opencl, gd->kernel_downsample);
  free(module->data);
  module->data = NULL;
}

void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *params, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  // dt_iop_demosaic_params_t *p = (dt_iop_demosaic_params_t *)params;
  dt_iop_demosaic_data_t *d = (dt_iop_demosaic_data_t *)piece->data;
  // if(pipe->type == DT_DEV_PIXELPIPE_PREVIEW) piece->enabled = 0;
  d->filters = self->dev->image->filters;
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
  // nothing
}

void gui_init     (struct dt_iop_module_t *self)
{
  self->widget = gtk_label_new(_("this module doesn't have any options"));
}

void gui_cleanup  (struct dt_iop_module_t *self)
{
  // nothing
}

