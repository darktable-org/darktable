/*
    This file is part of darktable,
    copyright (c) 2009--2011 johannes hanika.

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
#include <gtk/gtk.h>
#include <inttypes.h>
#include <ctype.h>
#ifdef HAVE_GEGL
#include <gegl.h>
#endif
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/tiling.h"
#include "common/opencl.h"
#include "common/interpolation.h"
#include "control/control.h"
#include "dtgtk/button.h"
#include "dtgtk/resetlabel.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/draw.h"
#include "iop/lens.h"


DT_MODULE(2)

const char*
name()
{
  return _("lens correction");
}

int
groups ()
{
  return IOP_GROUP_CORRECT;
}

int
operation_tags ()
{
  return IOP_TAG_DISTORT;
}

int flags()
{
  return IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_TILING_FULL_ROI;
}


void init_key_accels(dt_iop_module_so_t *self)
{
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "scale"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "tca R"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "tca B"));

  dt_accel_register_iop(self, FALSE, NC_("accel", "find camera"), 0, 0);
  dt_accel_register_iop(self, FALSE, NC_("accel", "find lens"), 0, 0);
  dt_accel_register_iop(self, FALSE, NC_("accel", "auto scale"), 0, 0);
  dt_accel_register_iop(self, FALSE, NC_("accel", "camera model"), 0, 0);
  dt_accel_register_iop(self, FALSE, NC_("accel", "lens model"), 0, 0);
}

void connect_key_accels(dt_iop_module_t *self)
{
  dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t*)self->gui_data;

  dt_accel_connect_button_iop(self, "find lens",
                              GTK_WIDGET(g->find_lens_button));
  dt_accel_connect_button_iop(self, "lens model",
                              GTK_WIDGET(g->lens_model));
  dt_accel_connect_button_iop(self, "camera model",
                              GTK_WIDGET(g->camera_model));
  dt_accel_connect_button_iop(self, "find camera",
                              GTK_WIDGET(g->find_camera_button));
  dt_accel_connect_button_iop(self, "auto scale",
                              GTK_WIDGET(g->auto_scale_button));

  dt_accel_connect_slider_iop(self, "scale", GTK_WIDGET(g->scale));
  dt_accel_connect_slider_iop(self, "tca R", GTK_WIDGET(g->tca_r));
  dt_accel_connect_slider_iop(self, "tca B", GTK_WIDGET(g->tca_b));
}

void
process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *ivoid, void *ovoid, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_lensfun_data_t *d = (dt_iop_lensfun_data_t *)piece->data;
  float *in  = (float *)ivoid;
  float *out = (float *)ovoid;
  const int ch = piece->colors;
  const int ch_width = ch*roi_in->width;
  const int mask_display = piece->pipe->mask_display;

  const unsigned int pixelformat = ch == 3 ? LF_CR_3 (RED, GREEN, BLUE) : LF_CR_4 (RED, GREEN, BLUE, UNKNOWN);

  if(!d->lens->Maker)
  {
    memcpy(out, in, ch*sizeof(float)*roi_out->width*roi_out->height);
    return;
  }

  const float orig_w = roi_in->scale*piece->iwidth,
              orig_h = roi_in->scale*piece->iheight;
  dt_pthread_mutex_lock(&darktable.plugin_threadsafe);
  lfModifier *modifier = lf_modifier_new(d->lens, d->crop, orig_w, orig_h);

  int modflags = lf_modifier_initialize(
                   modifier, d->lens, LF_PF_F32,
                   d->focal, d->aperture,
                   d->distance, d->scale,
                   d->target_geom, d->modify_flags, d->inverse);
  dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);

  if(d->inverse)
  {
    // reverse direction (useful for renderings)
    if (modflags & (LF_MODIFY_TCA | LF_MODIFY_DISTORTION |
                    LF_MODIFY_GEOMETRY | LF_MODIFY_SCALE))
    {
      // acquire temp memory for distorted pixel coords
      const size_t req2 = roi_out->width*2*3*sizeof(float);
      if(req2 > 0 && d->tmpbuf2_len < req2*dt_get_num_threads())
      {
        d->tmpbuf2_len = req2*dt_get_num_threads();
        free(d->tmpbuf2);
        d->tmpbuf2 = (float *)dt_alloc_align(16, d->tmpbuf2_len);
      }

      const struct  dt_interpolation* interpolation = dt_interpolation_new(DT_INTERPOLATION_USERPREF);

#ifdef _OPENMP
      #pragma omp parallel for default(none) shared(roi_out, roi_in, in, d, ovoid, modifier, interpolation) schedule(static)
#endif
      for (int y = 0; y < roi_out->height; y++)
      {
        float *pi = (float *)(((char *)d->tmpbuf2) + req2*dt_get_thread_num());
        lf_modifier_apply_subpixel_geometry_distortion (
          modifier, roi_out->x, roi_out->y+y, roi_out->width, 1, pi);
        // reverse transform the global coords from lf to our buffer
        float *buf = ((float *)ovoid) + y*roi_out->width*ch;
        for (int x = 0; x < roi_out->width; x++,buf+=ch,pi+=6)
        {
          for(int c=0; c<3; c++)
          {
            const float pi0 = pi[c*2] - roi_in->x;
            const float pi1 = pi[c*2+1] - roi_in->y;
            buf[c] = dt_interpolation_compute_sample(interpolation, in+c, pi0, pi1, roi_in->width, roi_in->height, ch, ch_width);
          }

          if(mask_display)
          {
            // take green channel distortion also for alpha channel
            const float pi0 = pi[2] - roi_in->x;
            const float pi1 = pi[3] - roi_in->y;
            buf[3] = dt_interpolation_compute_sample(interpolation, in+3, pi0, pi1, roi_in->width, roi_in->height, ch, ch_width);
          }
        }
      }
    }
    else
    {
#ifdef _OPENMP
      #pragma omp parallel for default(none) shared(roi_out, out, in) schedule(static)
#endif
      for (int y = 0; y < roi_out->height; y++)
        memcpy(out+ch*y*roi_out->width, in+ch*y*roi_out->width, ch*sizeof(float)*roi_out->width);
    }

    if (modflags & LF_MODIFY_VIGNETTING)
    {
#ifdef _OPENMP
      #pragma omp parallel for default(none) shared(roi_out, out, modifier) schedule(static)
#endif
      for (int y = 0; y < roi_out->height; y++)
      {
        /* Colour correction: vignetting and CCI */
        // actually this way row stride does not matter.
        float *buf = out;
        lf_modifier_apply_color_modification (modifier,
                                              buf + ch*roi_out->width*y, roi_out->x, roi_out->y + y,
                                              roi_out->width, 1, pixelformat, ch*roi_out->width);
      }
    }
  }
  else // correct distortions:
  {
    // acquire temp memory for image buffer
    const size_t req = roi_in->width*roi_in->height*ch*sizeof(float);
    if(req > 0 && d->tmpbuf_len < req)
    {
      d->tmpbuf_len = req;
      free(d->tmpbuf);
      d->tmpbuf = (float *)dt_alloc_align(16, d->tmpbuf_len);
    }
    memcpy(d->tmpbuf, in, req);
    if (modflags & LF_MODIFY_VIGNETTING)
    {
#ifdef _OPENMP
      #pragma omp parallel for default(none) shared(roi_in, out, modifier, d) schedule(static)
#endif
      for (int y = 0; y < roi_in->height; y++)
      {
        /* Colour correction: vignetting and CCI */
        // actually this way row stride does not matter.
        float *buf = d->tmpbuf;
        lf_modifier_apply_color_modification (modifier,
                                              buf + ch*roi_in->width*y, roi_in->x, roi_in->y + y,
                                              roi_in->width, 1, pixelformat, ch*roi_in->width);
      }
    }

    const size_t req2 = roi_out->width*2*3*sizeof(float);
    if (modflags & (LF_MODIFY_TCA | LF_MODIFY_DISTORTION |
                    LF_MODIFY_GEOMETRY | LF_MODIFY_SCALE))
    {
      // acquire temp memory for distorted pixel coords
      if(req2 > 0 && d->tmpbuf2_len < req2*dt_get_num_threads())
      {
        d->tmpbuf2_len = req2*dt_get_num_threads();
        free(d->tmpbuf2);
        d->tmpbuf2 = (float *)dt_alloc_align(16, d->tmpbuf2_len);
      }

      const struct dt_interpolation* interpolation = dt_interpolation_new(DT_INTERPOLATION_USERPREF);

#ifdef _OPENMP
      #pragma omp parallel for default(none) shared(roi_in, roi_out, d, ovoid, modifier, interpolation) schedule(static)
#endif
      for (int y = 0; y < roi_out->height; y++)
      {
        float *pi = (float *)(((char *)d->tmpbuf2) + dt_get_thread_num()*req2);
        lf_modifier_apply_subpixel_geometry_distortion (
          modifier, roi_out->x, roi_out->y+y, roi_out->width, 1, pi);
        // reverse transform the global coords from lf to our buffer
        float *out = ((float *)ovoid) + y*roi_out->width*ch;
        for (int x = 0; x < roi_out->width; x++,pi+=6)
        {
          for(int c=0; c<3; c++)
          {
            const float pi0 = pi[c*2] - roi_in->x;
            const float pi1 = pi[c*2+1] - roi_in->y;
            out[c] = dt_interpolation_compute_sample(interpolation, d->tmpbuf+c, pi0, pi1, roi_in->width, roi_in->height, ch, ch_width);
          }

          if(mask_display)
          {
            // take green channel distortion also for alpha channel
            const float pi0 = pi[2] - roi_in->x;
            const float pi1 = pi[3] - roi_in->y;
            out[3] = dt_interpolation_compute_sample(interpolation, d->tmpbuf+3, pi0, pi1, roi_in->width, roi_in->height, ch, ch_width);
          }
          out += ch;
        }
      }
    }
    else
    {
      const size_t len = sizeof(float)*ch*roi_out->width*roi_out->height;
      const float *const input = (d->tmpbuf_len >= len) ? d->tmpbuf : in;
#ifdef _OPENMP
      #pragma omp parallel for default(none) shared(roi_out, out) schedule(static)
#endif
      for (int y = 0; y < roi_out->height; y++)
        memcpy(out+ch*y*roi_out->width, input+ch*y*roi_out->width, ch*sizeof(float)*roi_out->width);
    }
  }
  lf_modifier_destroy(modifier);
}


#ifdef HAVE_OPENCL
int
process_cl (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_lensfun_data_t *d = (dt_iop_lensfun_data_t *)piece->data;
  dt_iop_lensfun_global_data_t *gd = (dt_iop_lensfun_global_data_t *)self->data;
  cl_mem dev_tmpbuf = NULL;
  cl_mem dev_tmp = NULL;
  cl_int err = -999;

  float *tmpbuf = NULL;
  lfModifier *modifier = NULL;

  const int devid = piece->pipe->devid;
  const int iwidth = roi_in->width;
  const int iheight = roi_in->height;
  const int owidth = roi_out->width;
  const int oheight = roi_out->height;
  const int roi_in_x = roi_in->x;
  const int roi_in_y = roi_in->y;
  const int width = MAX(iwidth, owidth);
  const int height = MAX(iheight, oheight);
  const int ch = piece->colors;
  const int tmpbufwidth = owidth*2*3;
  const int tmpbuflen = d->inverse ? oheight*owidth*2*3*sizeof(float) : MAX(oheight*owidth*2*3, iheight*iwidth*ch)*sizeof(float);
  const unsigned int pixelformat = ch == 3 ? LF_CR_3 (RED, GREEN, BLUE) : LF_CR_4 (RED, GREEN, BLUE, UNKNOWN);

  const float orig_w = roi_in->scale*piece->iwidth,
              orig_h = roi_in->scale*piece->iheight;

  size_t origin[] = { 0, 0, 0};
  size_t iregion[] = { iwidth, iheight, 1};
  size_t oregion[] = { owidth, oheight, 1};
  size_t isizes[] = { ROUNDUPWD(iwidth), ROUNDUPHT(iheight), 1};
  size_t osizes[] = { ROUNDUPWD(owidth), ROUNDUPHT(oheight), 1};

  if(!d->lens->Maker)
  {
    err = dt_opencl_enqueue_copy_image(devid, dev_in, dev_out, origin, origin, oregion);
    if(err != CL_SUCCESS) goto error;
    return TRUE;
  }

  const struct dt_interpolation* interpolation = dt_interpolation_new(DT_INTERPOLATION_USERPREF);

  int ldkernel = -1;

  switch(interpolation->id)
  {
    case DT_INTERPOLATION_BILINEAR:
      ldkernel = gd->kernel_lens_distort_bilinear;
      break;
    case DT_INTERPOLATION_BICUBIC:
      ldkernel = gd->kernel_lens_distort_bicubic;
      break;
    case DT_INTERPOLATION_LANCZOS2:
      ldkernel = gd->kernel_lens_distort_lanczos2;
      break;
    case DT_INTERPOLATION_LANCZOS3:
      ldkernel = gd->kernel_lens_distort_lanczos3;
      break;
    default:
      return FALSE;
  }


  tmpbuf = (float *)dt_alloc_align(16, tmpbuflen);
  if(tmpbuf == NULL) goto error;

  dev_tmp = dt_opencl_alloc_device(devid, width, height, 4*sizeof(float));
  if(dev_tmp == NULL) goto error;

  dev_tmpbuf = dt_opencl_alloc_device_buffer(devid, tmpbuflen);
  if(dev_tmpbuf == NULL) goto error;


  dt_pthread_mutex_lock(&darktable.plugin_threadsafe);
  modifier = lf_modifier_new(d->lens, d->crop, orig_w, orig_h);

  int modflags = lf_modifier_initialize(
                   modifier, d->lens, LF_PF_F32,
                   d->focal, d->aperture,
                   d->distance, d->scale,
                   d->target_geom, d->modify_flags, d->inverse);
  dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);

  if(d->inverse)
  {
    // reverse direction (useful for renderings)
    if(modflags & (LF_MODIFY_TCA | LF_MODIFY_DISTORTION |
                    LF_MODIFY_GEOMETRY | LF_MODIFY_SCALE))
    {
#ifdef _OPENMP
      #pragma omp parallel for default(none) shared(roi_out, roi_in, tmpbuf, d, modifier) schedule(static)
#endif
      for (int y = 0; y < roi_out->height; y++)
      {
        float *pi = tmpbuf + y * tmpbufwidth;
        lf_modifier_apply_subpixel_geometry_distortion (
          modifier, roi_out->x, roi_out->y+y, roi_out->width, 1, pi);
      }

      /* non-blocking memory transfer: host tmpbuf buffer -> opencl dev_tmpbuf */
      err = dt_opencl_write_buffer_to_device(devid, tmpbuf, dev_tmpbuf, 0, owidth*oheight*2*3*sizeof(float), CL_FALSE);
      if(err != CL_SUCCESS) goto error;

      dt_opencl_set_kernel_arg(devid, ldkernel, 0, sizeof(cl_mem), (void *)&dev_in);
      dt_opencl_set_kernel_arg(devid, ldkernel, 1, sizeof(cl_mem), (void *)&dev_tmp);
      dt_opencl_set_kernel_arg(devid, ldkernel, 2, sizeof(int), (void *)&owidth);
      dt_opencl_set_kernel_arg(devid, ldkernel, 3, sizeof(int), (void *)&oheight);
      dt_opencl_set_kernel_arg(devid, ldkernel, 4, sizeof(int), (void *)&iwidth);
      dt_opencl_set_kernel_arg(devid, ldkernel, 5, sizeof(int), (void *)&iheight);
      dt_opencl_set_kernel_arg(devid, ldkernel, 6, sizeof(int), (void *)&roi_in_x);
      dt_opencl_set_kernel_arg(devid, ldkernel, 7, sizeof(int), (void *)&roi_in_y);
      dt_opencl_set_kernel_arg(devid, ldkernel, 8, sizeof(cl_mem), (void *)&dev_tmpbuf);
      err = dt_opencl_enqueue_kernel_2d(devid, ldkernel, osizes);
      if(err != CL_SUCCESS) goto error;

    }
    else
    {
      err = dt_opencl_enqueue_copy_image(devid, dev_in, dev_tmp, origin, origin, oregion);
      if(err != CL_SUCCESS) goto error;
    }

    if(modflags & LF_MODIFY_VIGNETTING)
    {
#ifdef _OPENMP
      #pragma omp parallel for default(none) shared(roi_out, roi_in, tmpbuf, modifier, d) schedule(static)
#endif
      for (int y = 0; y < roi_out->height; y++)
      {
        /* Colour correction: vignetting and CCI */
        // actually this way row stride does not matter.
        float *buf = tmpbuf + y * ch*roi_out->width;
        for (int k=0; k < ch*roi_out->width; k++) buf[k] = 0.5f;
        lf_modifier_apply_color_modification (modifier, buf, roi_out->x, roi_out->y + y,
                                              roi_out->width, 1, pixelformat, ch*roi_out->width);
      }

      /* non-blocking memory transfer: host tmpbuf buffer -> opencl dev_tmpbuf */
      err = dt_opencl_write_buffer_to_device(devid, tmpbuf, dev_tmpbuf, 0, ch*roi_out->width*roi_out->height*sizeof(float), CL_FALSE);
      if(err != CL_SUCCESS) goto error;

      dt_opencl_set_kernel_arg(devid, gd->kernel_lens_vignette, 0, sizeof(cl_mem), (void *)&dev_tmp);
      dt_opencl_set_kernel_arg(devid, gd->kernel_lens_vignette, 1, sizeof(cl_mem), (void *)&dev_out);
      dt_opencl_set_kernel_arg(devid, gd->kernel_lens_vignette, 2, sizeof(int), (void *)&owidth);
      dt_opencl_set_kernel_arg(devid, gd->kernel_lens_vignette, 3, sizeof(int), (void *)&oheight);
      dt_opencl_set_kernel_arg(devid, gd->kernel_lens_vignette, 4, sizeof(cl_mem), (void *)&dev_tmpbuf); 
      err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_lens_vignette, osizes);
      if(err != CL_SUCCESS) goto error;

    }
    else
    {
      err = dt_opencl_enqueue_copy_image(devid, dev_tmp, dev_out, origin, origin, oregion);
      if(err != CL_SUCCESS) goto error;
    }

  }

  else // correct distortions:
  {

    if(modflags & LF_MODIFY_VIGNETTING)
    {
#ifdef _OPENMP
      #pragma omp parallel for default(none) shared(roi_out, roi_in, tmpbuf, modifier, d) schedule(static)
#endif
      for (int y = 0; y < roi_in->height; y++)
      {
        /* Colour correction: vignetting and CCI */
        // actually this way row stride does not matter.
        float *buf = tmpbuf + y * ch*roi_in->width;
        for (int k=0; k < ch*roi_in->width; k++) buf[k] = 0.5f;
        lf_modifier_apply_color_modification (modifier, buf, roi_in->x, roi_in->y + y,
                                              roi_in->width, 1, pixelformat, ch*roi_in->width);
      }

      /* non-blocking memory transfer: host tmpbuf buffer -> opencl dev_tmpbuf */
      err = dt_opencl_write_buffer_to_device(devid, tmpbuf, dev_tmpbuf, 0, ch*roi_in->width*roi_in->height*sizeof(float), CL_FALSE);
      if(err != CL_SUCCESS) goto error;

      dt_opencl_set_kernel_arg(devid, gd->kernel_lens_vignette, 0, sizeof(cl_mem), (void *)&dev_in);
      dt_opencl_set_kernel_arg(devid, gd->kernel_lens_vignette, 1, sizeof(cl_mem), (void *)&dev_tmp);
      dt_opencl_set_kernel_arg(devid, gd->kernel_lens_vignette, 2, sizeof(int), (void *)&iwidth);
      dt_opencl_set_kernel_arg(devid, gd->kernel_lens_vignette, 3, sizeof(int), (void *)&iheight);
      dt_opencl_set_kernel_arg(devid, gd->kernel_lens_vignette, 4, sizeof(cl_mem), (void *)&dev_tmpbuf); 
      err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_lens_vignette, isizes);
      if(err != CL_SUCCESS) goto error;
    }
    else
    {
      err = dt_opencl_enqueue_copy_image(devid, dev_in, dev_tmp, origin, origin, iregion);
      if(err != CL_SUCCESS) goto error;
    }


    if(modflags & (LF_MODIFY_TCA | LF_MODIFY_DISTORTION |
                    LF_MODIFY_GEOMETRY | LF_MODIFY_SCALE))
    {
#ifdef _OPENMP
      #pragma omp parallel for default(none) shared(roi_out, roi_in, tmpbuf, d, modifier) schedule(static)
#endif
      for (int y = 0; y < roi_out->height; y++)
      {
        float *pi = tmpbuf + y * tmpbufwidth;
        lf_modifier_apply_subpixel_geometry_distortion (
          modifier, roi_out->x, roi_out->y+y, roi_out->width, 1, pi);
      }

      /* non-blocking memory transfer: host tmpbuf buffer -> opencl dev_tmpbuf */
      err = dt_opencl_write_buffer_to_device(devid, tmpbuf, dev_tmpbuf, 0, owidth*oheight*2*3*sizeof(float), CL_FALSE);
      if(err != CL_SUCCESS) goto error;

      dt_opencl_set_kernel_arg(devid, ldkernel, 0, sizeof(cl_mem), (void *)&dev_tmp);
      dt_opencl_set_kernel_arg(devid, ldkernel, 1, sizeof(cl_mem), (void *)&dev_out);
      dt_opencl_set_kernel_arg(devid, ldkernel, 2, sizeof(int), (void *)&owidth);
      dt_opencl_set_kernel_arg(devid, ldkernel, 3, sizeof(int), (void *)&oheight);
      dt_opencl_set_kernel_arg(devid, ldkernel, 4, sizeof(int), (void *)&iwidth);
      dt_opencl_set_kernel_arg(devid, ldkernel, 5, sizeof(int), (void *)&iheight);
      dt_opencl_set_kernel_arg(devid, ldkernel, 6, sizeof(int), (void *)&roi_in_x);
      dt_opencl_set_kernel_arg(devid, ldkernel, 7, sizeof(int), (void *)&roi_in_y);
      dt_opencl_set_kernel_arg(devid, ldkernel, 8, sizeof(cl_mem), (void *)&dev_tmpbuf);
      err = dt_opencl_enqueue_kernel_2d(devid, ldkernel, osizes);
      if(err != CL_SUCCESS) goto error;
    }
    else
    {
      err = dt_opencl_enqueue_copy_image(devid, dev_tmp, dev_out, origin, origin, oregion);
      if(err != CL_SUCCESS) goto error;
    }

  }

  dt_opencl_release_mem_object(dev_tmpbuf);
  dt_opencl_release_mem_object(dev_tmp);
  if (tmpbuf != NULL) free(tmpbuf);
  if (modifier != NULL) lf_modifier_destroy(modifier);
  return TRUE;

error:
  if (dev_tmp != NULL) dt_opencl_release_mem_object(dev_tmp);
  if (dev_tmpbuf != NULL) dt_opencl_release_mem_object(dev_tmpbuf);
  if (tmpbuf != NULL) free(tmpbuf);
  if (modifier != NULL) lf_modifier_destroy(modifier);
  dt_print(DT_DEBUG_OPENCL, "[opencl_lens] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif

void tiling_callback (struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out, struct dt_develop_tiling_t *tiling)
{
  tiling->factor = 4.5f;  // in + out + tmp + tmpbuf
  tiling->maxbuf = 1.5f;
  tiling->overhead = 0;
  tiling->overlap = 4;
  tiling->xalign = 1;
  tiling->yalign = 1;
  return;
}


void modify_roi_out(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, dt_iop_roi_t *roi_out, const dt_iop_roi_t *roi_in)
{
  *roi_out = *roi_in;
}

void modify_roi_in(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, const dt_iop_roi_t *roi_out, dt_iop_roi_t *roi_in)
{
  dt_iop_lensfun_data_t *d = (dt_iop_lensfun_data_t *)piece->data;
  *roi_in = *roi_out;
  // inverse transform with given params

  if(!d->lens->Maker) return;

  const float orig_w = roi_in->scale*piece->iwidth,
              orig_h = roi_in->scale*piece->iheight;
  lfModifier *modifier = lf_modifier_new(d->lens, d->crop, orig_w, orig_h);

  float xm = INFINITY, xM = - INFINITY, ym = INFINITY, yM = - INFINITY;

  int modflags = lf_modifier_initialize(
                   modifier, d->lens, LF_PF_F32,
                   d->focal, d->aperture,
                   d->distance, d->scale,
                   d->target_geom, d->modify_flags, d->inverse);

  if (modflags & (LF_MODIFY_TCA | LF_MODIFY_DISTORTION |
                  LF_MODIFY_GEOMETRY | LF_MODIFY_SCALE))
  {
    // acquire temp memory for distorted pixel coords
    const size_t req2 = roi_in->width*2*3*sizeof(float);
    if(req2 > 0 && d->tmpbuf2_len < req2)
    {
      d->tmpbuf2_len = req2;
      free(d->tmpbuf2);
      d->tmpbuf2 = (float *)dt_alloc_align(16, d->tmpbuf2_len);
    }
    for (int y = 0; y < roi_out->height; y++)
    {
      lf_modifier_apply_subpixel_geometry_distortion (
        modifier, roi_out->x, roi_out->y+y, roi_out->width, 1, d->tmpbuf2);
      const float *pi = d->tmpbuf2;
      // reverse transform the global coords from lf to our buffer
      for (int x = 0; x < roi_out->width; x++)
      {
        for(int c=0; c<3; c++)
        {
          xm = fminf(xm, pi[0]);
          xM = fmaxf(xM, pi[0]);
          ym = fminf(ym, pi[1]);
          yM = fmaxf(yM, pi[1]);
          pi+=2;
        }
      }
    }

    const struct dt_interpolation* interpolation = dt_interpolation_new(DT_INTERPOLATION_USERPREF);
    roi_in->x = fmaxf(0.0f, xm-interpolation->width);
    roi_in->y = fmaxf(0.0f, ym-interpolation->width);
    roi_in->width = fminf(orig_w-roi_in->x, xM - roi_in->x + interpolation->width);
    roi_in->height = fminf(orig_h-roi_in->y, yM - roi_in->y + interpolation->width);
  }
  lf_modifier_destroy(modifier);
}

void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_lensfun_params_t *p = (dt_iop_lensfun_params_t *)p1;
#ifdef HAVE_GEGL
  // pull in new params to gegl
#error "lensfun needs to be ported to GEGL!"
#else
  dt_iop_lensfun_data_t *d = (dt_iop_lensfun_data_t *)piece->data;

  dt_iop_lensfun_global_data_t *gd = (dt_iop_lensfun_global_data_t *)self->data;
  lfDatabase *dt_iop_lensfun_db = (lfDatabase *)gd->db;
  const lfCamera *camera = NULL;
  const lfCamera **cam = NULL;
  if(p->camera[0])
  {
    dt_pthread_mutex_lock(&darktable.plugin_threadsafe);
    cam = lf_db_find_cameras_ext(dt_iop_lensfun_db,
                                 NULL, p->camera, 0);
    if(cam)
    {
        camera = cam[0];
        p->crop = cam[0]->CropFactor;
    }
    dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);
  }
  if(p->lens[0])
  {
    dt_pthread_mutex_lock(&darktable.plugin_threadsafe);
    const lfLens **lens = lf_db_find_lenses_hd(dt_iop_lensfun_db, camera, NULL,
                          p->lens, 0);
    dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);
    if(lens)
    {
      lf_lens_copy(d->lens, lens[0]);
      if(p->tca_override)
      {
        // add manual d->lens stuff:
        lfLensCalibTCA tca;
        memset(&tca, 0, sizeof(lfLensCalibTCA));
        tca.Focal = 0;
        tca.Model = LF_TCA_MODEL_LINEAR;
        tca.Terms[0] = p->tca_b;
        tca.Terms[1] = p->tca_r;
        if(d->lens->CalibTCA) 
          while (d->lens->CalibTCA[0])
            lf_lens_remove_calib_tca (d->lens, 0);
        lf_lens_add_calib_tca (d->lens, &tca);
      }
      lf_free (lens);
    }
  }
  lf_free(cam);
  d->modify_flags = p->modify_flags;
  d->inverse      = p->inverse;
  d->scale        = p->scale;
  d->crop         = p->crop;
  d->focal        = p->focal;
  d->aperture     = p->aperture;
  d->distance     = p->distance;
  d->target_geom  = p->target_geom;
#endif
}

void init_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
#ifdef HAVE_GEGL
#error "lensfun needs to be ported to GEGL!"
#else
  piece->data = malloc(sizeof(dt_iop_lensfun_data_t));
  dt_iop_lensfun_data_t *d = (dt_iop_lensfun_data_t *)piece->data;

  d->tmpbuf2_len = 0;
  d->tmpbuf2 = NULL;
  d->tmpbuf_len = 0;
  d->tmpbuf = NULL;
  d->lens = lf_lens_new();
  self->commit_params(self, self->default_params, pipe, piece);
#endif
}

void cleanup_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
#ifdef HAVE_GEGL
#error "lensfun needs to be ported to GEGL!"
#else
  dt_iop_lensfun_data_t *d = (dt_iop_lensfun_data_t *)piece->data;
  lf_lens_destroy(d->lens);
  free(d->tmpbuf);
  free(d->tmpbuf2);
  free(piece->data);
#endif
}

void init_global(dt_iop_module_so_t *module)
{
  const int program = 2; // basic.cl, from programs.conf
  dt_iop_lensfun_global_data_t *gd = (dt_iop_lensfun_global_data_t *)malloc(sizeof(dt_iop_lensfun_global_data_t));
  module->data = gd;
  gd->kernel_lens_distort_bilinear = dt_opencl_create_kernel(program, "lens_distort_bilinear");
  gd->kernel_lens_distort_bicubic = dt_opencl_create_kernel(program, "lens_distort_bicubic");
  gd->kernel_lens_distort_lanczos2 = dt_opencl_create_kernel(program, "lens_distort_lanczos2");
  gd->kernel_lens_distort_lanczos3 = dt_opencl_create_kernel(program, "lens_distort_lanczos3");
  gd->kernel_lens_vignette = dt_opencl_create_kernel(program, "lens_vignette");

  lfDatabase *dt_iop_lensfun_db = lf_db_new();
  gd->db = (void *)dt_iop_lensfun_db;
#if defined(__MACH__) || defined(__APPLE__)
#else
  if(lf_db_load(dt_iop_lensfun_db) != LF_NO_ERROR)
#endif
  {
    char path[1024];
    dt_loc_get_datadir(path, 1024);
    char *c = path + strlen(path);
    for(; c>path && *c != '/'; c--);
    sprintf(c, "/lensfun");
    dt_iop_lensfun_db->HomeDataDir = g_strdup(path);
    if(lf_db_load(dt_iop_lensfun_db) != LF_NO_ERROR)
      fprintf(stderr, "[iop_lens]: could not load lensfun database in `%s'!\n", path);
  }
}


void reload_defaults(dt_iop_module_t *module)
{
  dt_iop_lensfun_global_data_t *gd = (dt_iop_lensfun_global_data_t *)module->data;
  lfDatabase *dt_iop_lensfun_db = (lfDatabase *)gd->db;
  const dt_image_t *img = &module->dev->image_storage;
  // reload image specific stuff
  // get all we can from exif:
  dt_iop_lensfun_params_t tmp;
  g_strlcpy(tmp.lens, img->exif_lens, 52);
  g_strlcpy(tmp.camera, img->exif_model, 52);
  tmp.crop     = img->exif_crop;
  tmp.aperture = img->exif_aperture;
  tmp.focal    = img->exif_focal_length;
  tmp.scale    = 1.0;
  tmp.inverse  = 0;
  tmp.modify_flags = LF_MODIFY_TCA | LF_MODIFY_VIGNETTING |
                     LF_MODIFY_DISTORTION | LF_MODIFY_GEOMETRY | LF_MODIFY_SCALE;
  tmp.distance = img->exif_focus_distance;
  tmp.target_geom = LF_RECTILINEAR;
  tmp.tca_override = 0;
  tmp.tca_r = 1.0;
  tmp.tca_b = 1.0;

  // init crop from db:
  char model[100];  // truncate often complex descriptions.
  g_strlcpy(model, img->exif_model, 100);
  for(char cnt = 0, *c = model; c < model+100 && *c != '\0'; c++) if(*c == ' ') if(++cnt == 2) *c = '\0';
  if(img->exif_maker[0] || model[0])
  {
    dt_pthread_mutex_lock(&darktable.plugin_threadsafe);
    const lfCamera **cam = lf_db_find_cameras_ext(dt_iop_lensfun_db,
                           img->exif_maker, img->exif_model, 0);
    dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);
    if(cam)
    {
      tmp.crop = cam[0]->CropFactor;
      lf_free(cam);
    }
  }

  memcpy(module->params, &tmp, sizeof(dt_iop_lensfun_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_lensfun_params_t));
  module->default_enabled = 0;
}

void init(dt_iop_module_t *module)
{
  module->params = malloc(sizeof(dt_iop_lensfun_params_t));
  module->default_params = malloc(sizeof(dt_iop_lensfun_params_t));
  module->default_enabled = 0;
  module->params_size = sizeof(dt_iop_lensfun_params_t);
  module->gui_data = NULL;
  module->priority = 215; // module order created by iop_dependencies.py, do not edit!
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
  dt_iop_lensfun_global_data_t *gd = (dt_iop_lensfun_global_data_t *)module->data;
  lfDatabase *dt_iop_lensfun_db = (lfDatabase *)gd->db;
  lf_db_destroy(dt_iop_lensfun_db);

  dt_opencl_free_kernel(gd->kernel_lens_distort_bilinear);
  dt_opencl_free_kernel(gd->kernel_lens_distort_bicubic);
  dt_opencl_free_kernel(gd->kernel_lens_distort_lanczos2);
  dt_opencl_free_kernel(gd->kernel_lens_distort_lanczos3);
  dt_opencl_free_kernel(gd->kernel_lens_vignette);
  free(module->data);
  module->data = NULL;
}


/// ############################################################
/// gui stuff: inspired by ufraws lensfun tab:


static GtkComboBoxEntry *combo_entry_text (
  GtkWidget *container, guint x, guint y, gchar *lbl, gchar *tip)
{
  GtkWidget *label, *combo;

  (void)label;

  combo = gtk_combo_box_entry_new_text ();
  if (GTK_IS_TABLE (container))
    gtk_table_attach (GTK_TABLE (container), combo, x+1, x+2, y, y+1, 0, 0, 2, 0);
  else if (GTK_IS_BOX (container))
    gtk_box_pack_start (GTK_BOX (container), combo, TRUE, TRUE, 2);
  g_object_set(G_OBJECT(combo), "tooltip-text", tip, (char *)NULL);

  return GTK_COMBO_BOX_ENTRY (combo);
}

/* simple function to compute the floating-point precision
   which is enough for "normal use". The criteria is to have
   about 3 leading digits after the initial zeros.  */
static int precision (double x, double adj)
{
  x *= adj;

  if (x == 0) return 1;
  if (x < 1.0)
    if (x < 0.1)
      if (x < 0.01)
        return 5;
      else
        return 4;
    else
      return 3;
  else if (x < 100.0)
    if (x < 10.0)
      return 2;
    else
      return 1;
  else
    return 0;
}

static GtkComboBoxEntry *combo_entry_numeric (
  GtkWidget *container, guint x, guint y, gchar *lbl, gchar *tip,
  gdouble val, gdouble precadj, gdouble *values, int nvalues)
{
  int i;
  char txt [30];
  GtkEntry *entry;
  GtkComboBoxEntry *combo;

  combo = combo_entry_text (container, x, y, lbl, tip);
  entry = GTK_ENTRY (GTK_BIN (combo)->child);

  gtk_entry_set_width_chars (entry, 4);

  snprintf (txt, sizeof (txt), "%.*f", precision (val, precadj), val);
  gtk_entry_set_text (entry, txt);

  for (i = 0; i < nvalues; i++)
  {
    gdouble v = values [i];
    snprintf (txt, sizeof (txt), "%.*f", precision (v, precadj), v);
    gtk_combo_box_append_text (GTK_COMBO_BOX (combo), txt);
  }

  return combo;
}

static GtkComboBoxEntry *combo_entry_numeric_log (
  GtkWidget *container, guint x, guint y, gchar *lbl, gchar *tip,
  gdouble val, gdouble min, gdouble max, gdouble step, gdouble precadj)
{
  int phase, nvalues;
  gdouble *values = NULL;
  for (phase = 0; phase < 2; phase++)
  {
    nvalues = 0;
    gboolean done = FALSE;
    gdouble v = min;
    while (!done)
    {
      if (v > max)
      {
        v = max;
        done = TRUE;
      }

      if (values)
        values [nvalues++] = v;
      else
        nvalues++;

      v *= step;
    }
    if (!values)
      values = g_new (gdouble, nvalues);
  }

  GtkComboBoxEntry *cbe = combo_entry_numeric (
                            container, x, y, lbl, tip, val, precadj, values, nvalues);
  g_free (values);
  return cbe;
}

/* -- ufraw ptr array functions -- */

static int ptr_array_insert_sorted (
  GPtrArray *array, const void *item, GCompareFunc compare)
{
  int length = array->len;
  g_ptr_array_set_size (array, length + 1);
  const void **root = (const void **)array->pdata;

  int m = 0, l = 0, r = length - 1;

  // Skip trailing NULL, if any
  if (l <= r && !root [r])
    r--;

  while (l <= r)
  {
    m = (l + r) / 2;
    int cmp = compare (root [m], item);

    if (cmp == 0)
    {
      ++m;
      goto done;
    }
    else if (cmp < 0)
      l = m + 1;
    else
      r = m - 1;
  }
  if (r == m)
    m++;

done:
  memmove (root + m + 1, root + m, (length - m) * sizeof (void *));
  root [m] = item;
  return m;
}

static int ptr_array_find_sorted (
  const GPtrArray *array, const void *item, GCompareFunc compare)
{
  int length = array->len;
  void **root = array->pdata;

  int l = 0, r = length - 1;
  int m = 0, cmp = 0;

  if (!length)
    return -1;

  // Skip trailing NULL, if any
  if (!root [r])
    r--;

  while (l <= r)
  {
    m = (l + r) / 2;
    cmp = compare (root [m], item);

    if (cmp == 0)
      return m;
    else if (cmp < 0)
      l = m + 1;
    else
      r = m - 1;
  }

  return -1;
}

static void ptr_array_insert_index (
  GPtrArray *array, const void *item, int index)
{
  const void **root;
  int length = array->len;
  g_ptr_array_set_size (array, length + 1);
  root = (const void **)array->pdata;
  memmove (root + index + 1, root + index, (length - index) * sizeof (void *));
  root [index] = item;
}

/* -- end ufraw ptr array functions -- */

/* -- camera -- */

static void camera_set (dt_iop_module_t *self, const lfCamera *cam)
{
  dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)self->gui_data;
  dt_iop_lensfun_params_t *p = (dt_iop_lensfun_params_t *)self->params;
  gchar *fm;
  const char *maker, *model, *variant;
  char _variant [100];

  if (!cam)
  {
    gtk_button_set_label(GTK_BUTTON(g->camera_model), "");
    gtk_label_set_ellipsize(GTK_LABEL(gtk_bin_get_child(GTK_BIN(g->camera_model))), PANGO_ELLIPSIZE_END);
    g_object_set(G_OBJECT(g->camera_model), "tooltip-text", "", (char *)NULL);
    return;
  }

  g_strlcpy(p->camera, cam->Model, 52);
  p->crop = cam->CropFactor;
  g->camera = cam;

  maker = lf_mlstr_get (cam->Maker);
  model = lf_mlstr_get (cam->Model);
  variant = lf_mlstr_get (cam->Variant);

  if (model)
  {
    if (maker)
      fm = g_strdup_printf ("%s, %s", maker, model);
    else
      fm = g_strdup_printf ("%s", model);
    gtk_button_set_label (GTK_BUTTON (g->camera_model), fm);
    gtk_label_set_ellipsize(GTK_LABEL(gtk_bin_get_child(GTK_BIN(g->camera_model))), PANGO_ELLIPSIZE_END);
    g_free (fm);
  }

  if (variant)
    snprintf (_variant, sizeof (_variant), " (%s)", variant);
  else
    _variant [0] = 0;

  fm = g_strdup_printf (_("maker:\t\t%s\n"
                          "model:\t\t%s%s\n"
                          "mount:\t\t%s\n"
                          "crop factor:\t%.1f"),
                        maker, model, _variant,
                        cam->Mount, cam->CropFactor);
  g_object_set(G_OBJECT(g->camera_model), "tooltip-text", fm, (char *)NULL);
  g_free (fm);
}

static void camera_menu_select (
  GtkMenuItem *menuitem, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  camera_set (self, (lfCamera *)g_object_get_data(G_OBJECT(menuitem), "lfCamera"));
  if(!darktable.gui->reset) dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void camera_menu_fill (dt_iop_module_t *self, const lfCamera *const *camlist)
{
  dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)self->gui_data;
  unsigned i;
  GPtrArray *makers, *submenus;

  if (g->camera_menu)
  {
    gtk_widget_destroy (GTK_WIDGET(g->camera_menu));
    g->camera_menu = NULL;
  }

  /* Count all existing camera makers and create a sorted list */
  makers = g_ptr_array_new ();
  submenus = g_ptr_array_new ();
  for (i = 0; camlist [i]; i++)
  {
    GtkWidget *submenu, *item;
    const char *m = lf_mlstr_get (camlist [i]->Maker);
    int idx = ptr_array_find_sorted (makers, m, (GCompareFunc)g_utf8_collate);
    if (idx < 0)
    {
      /* No such maker yet, insert it into the array */
      idx = ptr_array_insert_sorted (makers, m, (GCompareFunc)g_utf8_collate);
      /* Create a submenu for cameras by this maker */
      submenu = gtk_menu_new ();
      ptr_array_insert_index (submenus, submenu, idx);
    }

    submenu = g_ptr_array_index (submenus, idx);
    /* Append current camera name to the submenu */
    m = lf_mlstr_get (camlist [i]->Model);
    if (!camlist [i]->Variant)
      item = gtk_menu_item_new_with_label (m);
    else
    {
      gchar *fm = g_strdup_printf ("%s (%s)", m, camlist [i]->Variant);
      item = gtk_menu_item_new_with_label (fm);
      g_free (fm);
    }
    gtk_widget_show (item);
    g_object_set_data(G_OBJECT(item), "lfCamera", (void *)camlist [i]);
    g_signal_connect(G_OBJECT(item), "activate",
                     G_CALLBACK(camera_menu_select), self);
    gtk_menu_shell_append (GTK_MENU_SHELL (submenu), item);
  }

  g->camera_menu = GTK_MENU(gtk_menu_new ());
  for (i = 0; i < makers->len; i++)
  {
    GtkWidget *item = gtk_menu_item_new_with_label (g_ptr_array_index (makers, i));
    gtk_widget_show (item);
    gtk_menu_shell_append (GTK_MENU_SHELL (g->camera_menu), item);
    gtk_menu_item_set_submenu (
      GTK_MENU_ITEM (item), (GtkWidget *)g_ptr_array_index (submenus, i));
  }

  g_ptr_array_free (submenus, TRUE);
  g_ptr_array_free (makers, TRUE);
}

static void parse_maker_model (
  const char *txt, char *make, size_t sz_make, char *model, size_t sz_model)
{
  const gchar *sep;

  while (txt [0] && isspace (txt [0]))
    txt++;
  sep = strchr (txt, ',');
  if (sep)
  {
    size_t len = sep - txt;
    if (len > sz_make - 1)
      len = sz_make - 1;
    memcpy (make, txt, len);
    make [len] = 0;

    while (*++sep && isspace (sep [0]))
      ;
    len = strlen (sep);
    if (len > sz_model - 1)
      len = sz_model - 1;
    memcpy (model, sep, len);
    model [len] = 0;
  }
  else
  {
    size_t len = strlen (txt);
    if (len > sz_model - 1)
      len = sz_model - 1;
    memcpy (model, txt, len);
    model [len] = 0;
    make [0] = 0;
  }
}

static void camera_menusearch_clicked(
  GtkWidget *button, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_lensfun_global_data_t *gd = (dt_iop_lensfun_global_data_t *)self->data;
  lfDatabase *dt_iop_lensfun_db = (lfDatabase *)gd->db;
  dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)self->gui_data;

  (void)button;

  const lfCamera *const *camlist;
  dt_pthread_mutex_lock(&darktable.plugin_threadsafe);
  camlist = lf_db_get_cameras (dt_iop_lensfun_db);
  dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);
  if (!camlist) return;
  camera_menu_fill (self, camlist);
  gtk_menu_popup (GTK_MENU (g->camera_menu), NULL, NULL, NULL, NULL,
                  0, gtk_get_current_event_time ());
}
static void camera_autosearch_clicked(
  GtkWidget *button, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_lensfun_global_data_t *gd = (dt_iop_lensfun_global_data_t *)self->data;
  lfDatabase *dt_iop_lensfun_db = (lfDatabase *)gd->db;
  dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)self->gui_data;
  char make [200], model [200];
  const gchar *txt = ((dt_iop_lensfun_params_t*)self->default_params)->camera;

  (void)button;

  if(txt[0] == '\0')
  {
    const lfCamera *const *camlist;
    dt_pthread_mutex_lock(&darktable.plugin_threadsafe);
    camlist = lf_db_get_cameras (dt_iop_lensfun_db);
    dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);
    if (!camlist) return;
    camera_menu_fill (self, camlist);
  }
  else
  {
    parse_maker_model (txt, make, sizeof (make), model, sizeof (model));
    dt_pthread_mutex_lock(&darktable.plugin_threadsafe);
    const lfCamera **camlist = lf_db_find_cameras_ext (dt_iop_lensfun_db, make, model, 0);
    dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);
    if (!camlist) return;
    camera_menu_fill (self, camlist);
    lf_free (camlist);
  }

  gtk_menu_popup (GTK_MENU (g->camera_menu), NULL, NULL, NULL, NULL,
                  0, gtk_get_current_event_time ());
}

/* -- end camera -- */

static void lens_comboentry_focal_update (GtkComboBox *widget, dt_iop_module_t *self)
{
  dt_iop_lensfun_params_t *p = (dt_iop_lensfun_params_t *)self->params;
  (void)sscanf (gtk_combo_box_get_active_text (widget), "%f", &p->focal);
  if(!darktable.gui->reset) dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void lens_focal_reset(
  GtkWidget *button, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)self->gui_data;
  char txt [30];
  const float val = ((dt_iop_lensfun_params_t*)self->default_params)->focal;
  snprintf (txt, sizeof (txt), "%.*f", precision (val, 10.0), val);
  GtkEntry *entry;
  entry = GTK_ENTRY (GTK_BIN (g->cbe[0])->child);
  gtk_entry_set_text (entry, txt);
}

static void lens_comboentry_aperture_update (GtkComboBox *widget, dt_iop_module_t *self)
{
  dt_iop_lensfun_params_t *p = (dt_iop_lensfun_params_t *)self->params;
  (void)sscanf (gtk_combo_box_get_active_text (widget), "%f", &p->aperture);
  if(!darktable.gui->reset) dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void lens_aperture_reset(
  GtkWidget *button, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)self->gui_data;
  char txt [30];
  float val = ((dt_iop_lensfun_params_t*)self->default_params)->aperture;
  snprintf (txt, sizeof (txt), "%.*f", precision (val, 10.0), val);
  GtkEntry *entry;
  entry = GTK_ENTRY (GTK_BIN (g->cbe[1])->child);
  gtk_entry_set_text (entry, txt);
}
static void lens_distance_reset(GtkWidget *button, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)self->gui_data;
  char txt[30];
  float val = ((dt_iop_lensfun_params_t *)self->default_params)->distance;
  snprintf(txt, sizeof(txt), "%.*f", precision (val, 10.0), val);
  GtkEntry *entry;
  entry = GTK_ENTRY(GTK_BIN(g->cbe[2])->child);
  gtk_entry_set_text(entry, txt);
}

static void lens_comboentry_distance_update (GtkComboBox *widget, dt_iop_module_t *self)
{
  dt_iop_lensfun_params_t *p = (dt_iop_lensfun_params_t *)self->params;
  (void)sscanf (gtk_combo_box_get_active_text (widget), "%f", &p->distance);
  if(!darktable.gui->reset) dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void delete_children (GtkWidget *widget, gpointer data)
{
  (void)data;
  gtk_widget_destroy (widget);
}

static void lens_set (dt_iop_module_t *self, const lfLens *lens)
{
  dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)self->gui_data;
  dt_iop_lensfun_params_t *p = (dt_iop_lensfun_params_t *)self->params;
  gchar *fm;
  const char *maker, *model;
  unsigned i;
  static gdouble focal_values [] =
  {
    4.5, 8, 10, 12, 14, 15, 16, 17, 18, 20, 24, 28, 30, 31, 35, 38, 40, 43,
    45, 50, 55, 60, 70, 75, 77, 80, 85, 90, 100, 105, 110, 120, 135,
    150, 200, 210, 240, 250, 300, 400, 500, 600, 800, 1000
  };
  static gdouble aperture_values [] =
  {
    1, 1.2, 1.4, 1.7, 2, 2.4, 2.8, 3.4, 4, 4.8, 5.6, 6.7,
    8, 9.5, 11, 13, 16, 19, 22, 27, 32, 38
  };

  if (!lens)
  {
    gtk_button_set_label(GTK_BUTTON(g->lens_model), "");
    gtk_label_set_ellipsize(GTK_LABEL(gtk_bin_get_child(GTK_BIN(g->lens_model))), PANGO_ELLIPSIZE_END);
    g_object_set(G_OBJECT(g->lens_model), "tooltip-text", "", (char *)NULL);
    return;
  }

  maker = lf_mlstr_get (lens->Maker);
  model = lf_mlstr_get (lens->Model);

  g_strlcpy(p->lens, model, 52);

  if (model)
  {
    if (maker)
      fm = g_strdup_printf ("%s, %s", maker, model);
    else
      fm = g_strdup_printf ("%s", model);
    gtk_button_set_label (GTK_BUTTON (g->lens_model), fm);
    gtk_label_set_ellipsize(GTK_LABEL(gtk_bin_get_child(GTK_BIN(g->lens_model))), PANGO_ELLIPSIZE_END);
    g_free (fm);
  }

  char focal [100], aperture [100], mounts [200];

  if (lens->MinFocal < lens->MaxFocal)
    snprintf (focal, sizeof (focal), "%g-%gmm", lens->MinFocal, lens->MaxFocal);
  else
    snprintf (focal, sizeof (focal), "%gmm", lens->MinFocal);
  if (lens->MinAperture < lens->MaxAperture)
    snprintf (aperture, sizeof (aperture), "%g-%g", lens->MinAperture, lens->MaxAperture);
  else
    snprintf (aperture, sizeof (aperture), "%g", lens->MinAperture);

  mounts [0] = 0;
  if (lens->Mounts)
    for (i = 0; lens->Mounts [i]; i++)
    {
      if (i > 0)
        g_strlcat (mounts, ", ", sizeof (mounts));
      g_strlcat (mounts, lens->Mounts [i], sizeof (mounts));
    }

  fm = g_strdup_printf (_("maker:\t\t%s\n"
                          "model:\t\t%s\n"
                          "focal range:\t%s\n"
                          "aperture:\t\t%s\n"
                          "crop factor:\t%.1f\n"
                          "type:\t\t%s\n"
                          "mounts:\t\t%s"),
                        maker ? maker : "?", model ? model : "?",
                        focal, aperture, lens->CropFactor,
                        lf_get_lens_type_desc (lens->Type, NULL), mounts);
  g_object_set(G_OBJECT(g->lens_model), "tooltip-text", fm, (char *)NULL);
  g_free (fm);

  /* Create the focal/aperture/distance combo boxes */
  gtk_container_foreach (
    GTK_CONTAINER (g->lens_param_box), delete_children, NULL);

  int ffi = 0, fli = -1;
  for (i = 0; i < sizeof (focal_values) / sizeof (gdouble); i++)
  {
    if (focal_values [i] < lens->MinFocal)
      ffi = i + 1;
    if (focal_values [i] > lens->MaxFocal && fli == -1)
      fli = i;
  }
  if (lens->MaxFocal == 0 || fli < 0)
    fli = sizeof (focal_values) / sizeof (gdouble);
  if (fli < ffi)
    fli = ffi + 1;
  g->cbe[0] = combo_entry_numeric (
                g->lens_param_box, 0, 0, _("mm"), _("focal length (mm)"),
                p->focal, 10.0, focal_values + ffi, fli - ffi);
  g_signal_connect (G_OBJECT(g->cbe[0]), "changed",
                    G_CALLBACK(lens_comboentry_focal_update), self);
  GtkWidget* button = dtgtk_button_new(dtgtk_cairo_paint_refresh, CPF_STYLE_FLAT);
  gtk_box_pack_start(GTK_BOX(g->lens_param_box), button, FALSE, FALSE, 0);
  g_object_set(G_OBJECT(button), "tooltip-text", _("reset from exif data"), (char *)NULL);
  g_signal_connect (G_OBJECT (button), "clicked",
                    G_CALLBACK (lens_focal_reset), self);

  ffi = 0;
  for (i = 0; i < sizeof (aperture_values) / sizeof (gdouble); i++)
    if (aperture_values [i] < lens->MinAperture)
      ffi = i + 1;
  g->cbe[1] = combo_entry_numeric (
                g->lens_param_box, 0, 0, _("f/"), _("f-number (aperture)"),
                p->aperture, 10.0, aperture_values + ffi, sizeof (aperture_values) / sizeof (gdouble) - ffi);
  g_signal_connect (G_OBJECT(g->cbe[1]), "changed",
                    G_CALLBACK(lens_comboentry_aperture_update), self);
  button = dtgtk_button_new(dtgtk_cairo_paint_refresh, CPF_STYLE_FLAT);
  gtk_box_pack_start(GTK_BOX(g->lens_param_box), button, FALSE, FALSE, 0);
  g_object_set(G_OBJECT(button), "tooltip-text", _("reset from exif data"), (char *)NULL);
  g_signal_connect (G_OBJECT (button), "clicked",
                    G_CALLBACK (lens_aperture_reset), self);

  g->cbe[2] = combo_entry_numeric_log (
                g->lens_param_box, 0, 0, _("d"), _("distance to subject"),
                p->distance, 0.25, 1000, 1.41421356237309504880, 10.0);
  g_signal_connect (G_OBJECT(g->cbe[2]), "changed",
                    G_CALLBACK(lens_comboentry_distance_update), self);
  button = dtgtk_button_new(dtgtk_cairo_paint_refresh, CPF_STYLE_FLAT);
  gtk_box_pack_start(GTK_BOX(g->lens_param_box), button, FALSE, FALSE, 0);
  g_object_set(G_OBJECT(button), "tooltip-text", _("reset from exif data"), (char *)NULL);
  g_signal_connect (G_OBJECT (button), "clicked",
                    G_CALLBACK (lens_distance_reset), self);

  gtk_widget_show_all (g->lens_param_box);
}

static void lens_menu_select (
  GtkMenuItem *menuitem, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  lens_set (self, (lfLens *)g_object_get_data(G_OBJECT(menuitem), "lfLens"));
  if(!darktable.gui->reset) dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void lens_menu_fill (
  dt_iop_module_t *self, const lfLens *const *lenslist)
{
  dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)self->gui_data;
  unsigned i;
  GPtrArray *makers, *submenus;

  if (g->lens_menu)
  {
    gtk_widget_destroy (GTK_WIDGET(g->lens_menu));
    g->lens_menu = NULL;
  }

  /* Count all existing lens makers and create a sorted list */
  makers = g_ptr_array_new ();
  submenus = g_ptr_array_new ();
  for (i = 0; lenslist [i]; i++)
  {
    GtkWidget *submenu, *item;
    const char *m = lf_mlstr_get (lenslist [i]->Maker);
    int idx = ptr_array_find_sorted (makers, m, (GCompareFunc)g_utf8_collate);
    if (idx < 0)
    {
      /* No such maker yet, insert it into the array */
      idx = ptr_array_insert_sorted (makers, m, (GCompareFunc)g_utf8_collate);
      /* Create a submenu for lenses by this maker */
      submenu = gtk_menu_new ();
      ptr_array_insert_index (submenus, submenu, idx);
    }

    submenu = g_ptr_array_index (submenus, idx);
    /* Append current lens name to the submenu */
    item = gtk_menu_item_new_with_label (lf_mlstr_get (lenslist [i]->Model));
    gtk_widget_show (item);
    g_object_set_data(G_OBJECT(item), "lfLens", (void *)lenslist [i]);
    g_signal_connect(G_OBJECT(item), "activate",
                     G_CALLBACK(lens_menu_select), self);
    gtk_menu_shell_append (GTK_MENU_SHELL (submenu), item);
  }

  g->lens_menu = GTK_MENU(gtk_menu_new ());
  for (i = 0; i < makers->len; i++)
  {
    GtkWidget *item = gtk_menu_item_new_with_label (g_ptr_array_index (makers, i));
    gtk_widget_show (item);
    gtk_menu_shell_append (GTK_MENU_SHELL (g->lens_menu), item);
    gtk_menu_item_set_submenu (
      GTK_MENU_ITEM (item), (GtkWidget *)g_ptr_array_index (submenus, i));
  }

  g_ptr_array_free (submenus, TRUE);
  g_ptr_array_free (makers, TRUE);
}


static void lens_menusearch_clicked(
  GtkWidget *button, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_lensfun_global_data_t *gd = (dt_iop_lensfun_global_data_t *)self->data;
  lfDatabase *dt_iop_lensfun_db = (lfDatabase *)gd->db;
  dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)self->gui_data;
  const lfLens **lenslist;

  (void)button;

  dt_pthread_mutex_lock(&darktable.plugin_threadsafe);
  lenslist = lf_db_find_lenses_hd (dt_iop_lensfun_db, g->camera,NULL,NULL, 0);
  dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);
  if (!lenslist) return;
  lens_menu_fill (self, lenslist);
  lf_free (lenslist);

  gtk_menu_popup (GTK_MENU (g->lens_menu), NULL, NULL, NULL, NULL,
                  0, gtk_get_current_event_time ());
}

static void lens_autosearch_clicked(
  GtkWidget *button, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_lensfun_global_data_t *gd = (dt_iop_lensfun_global_data_t *)self->data;
  lfDatabase *dt_iop_lensfun_db = (lfDatabase *)gd->db;
  dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)self->gui_data;
  const lfLens **lenslist;
  char make [200], model [200];
  const gchar *txt = ((dt_iop_lensfun_params_t*)self->default_params)->lens;

  (void)button;

  parse_maker_model (txt, make, sizeof (make), model, sizeof (model));
  dt_pthread_mutex_lock(&darktable.plugin_threadsafe);
  lenslist = lf_db_find_lenses_hd (dt_iop_lensfun_db, g->camera,
                                   make [0] ? make : NULL,
                                   model [0] ? model : NULL, 0);
  dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);
  if (!lenslist) return;
  lens_menu_fill (self, lenslist);
  lf_free (lenslist);

  gtk_menu_popup (GTK_MENU (g->lens_menu), NULL, NULL, NULL, NULL,
                  0, gtk_get_current_event_time ());
}

/* -- end lens -- */

static void target_geometry_changed (GtkComboBox *widget, gpointer user_data)
{
  if(darktable.gui->reset) return;
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_lensfun_params_t *p = (dt_iop_lensfun_params_t *)self->params;

  int pos = gtk_combo_box_get_active(widget);
  p->target_geom = pos + LF_UNKNOWN + 1;
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void reverse_toggled(GtkToggleButton *togglebutton, gpointer user_data)
{
  if(darktable.gui->reset) return;
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_lensfun_params_t *p = (dt_iop_lensfun_params_t *)self->params;
  if(gtk_toggle_button_get_active(togglebutton)) p->inverse = 1;
  else p->inverse = 0;
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void tca_changed(GtkDarktableSlider *slider, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_lensfun_params_t   *p = (dt_iop_lensfun_params_t   *)self->params;
  dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)self->gui_data;
  const float val = dtgtk_slider_get_value(slider);
  if(slider == g->tca_r) p->tca_r = val;
  else                   p->tca_b = val;
  if(p->tca_r != 1.0 || p->tca_b != 1.0) p->tca_override = 1;
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void scale_changed(GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_lensfun_params_t *p = (dt_iop_lensfun_params_t *)self->params;
  if(darktable.gui->reset) return;
  p->scale = dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static float get_autoscale(dt_iop_module_t *self)
{
  dt_iop_lensfun_params_t   *p = (dt_iop_lensfun_params_t   *)self->params;
  dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)self->gui_data;
  dt_iop_lensfun_global_data_t *gd = (dt_iop_lensfun_global_data_t *)self->data;
  lfDatabase *dt_iop_lensfun_db = (lfDatabase *)gd->db;
  float scale = 1.0;
  if(p->lens[0] != '\0')
  {
    char make [200], model [200];
    const gchar *txt = gtk_button_get_label(GTK_BUTTON(g->lens_model));
    parse_maker_model (txt, make, sizeof (make), model, sizeof (model));
    dt_pthread_mutex_lock(&darktable.plugin_threadsafe);
    const lfLens **lenslist = lf_db_find_lenses_hd (dt_iop_lensfun_db, g->camera, NULL, p->lens, 0);
    if(lenslist && !lenslist[1])
    {
      // create dummy modifier
      lfModifier *modifier = lf_modifier_new(lenslist[0], p->crop, self->dev->image_storage.width, self->dev->image_storage.height);
      (void)lf_modifier_initialize(
        modifier, lenslist[0], LF_PF_F32,
        p->focal, p->aperture,
        p->distance, 1.0f,
        p->target_geom, p->modify_flags, p->inverse);
      scale = lf_modifier_get_auto_scale (modifier, p->inverse);
      lf_modifier_destroy(modifier);
    }
    lf_free (lenslist);
    dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);
  }
  return scale;
}

static void autoscale_pressed(GtkWidget *button, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  const float scale = get_autoscale(self);
  dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)self->gui_data;
  dtgtk_slider_set_value(g->scale, scale);
}

void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_lensfun_gui_data_t));
  // dt_iop_lensfun_global_data_t *gd = (dt_iop_lensfun_global_data_t *)self->data;
  // lfDatabase *dt_iop_lensfun_db = (lfDatabase *)gd->db;
  dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)self->gui_data;
  dt_iop_lensfun_params_t *p = (dt_iop_lensfun_params_t *)self->params;
  g->camera = NULL;
  g->camera_menu = NULL;
  g->lens_menu = NULL;

  GtkWidget *button;
  GtkWidget *label;

  self->widget = gtk_table_new(8, 3, FALSE);
  gtk_table_set_col_spacings(GTK_TABLE(self->widget), 5);
  gtk_table_set_row_spacings(GTK_TABLE(self->widget), DT_GUI_IOP_MODULE_CONTROL_SPACING);

  // camera selector
  GtkWidget *hbox = gtk_hbox_new(FALSE, 0);
  g->camera_model = GTK_BUTTON(dtgtk_button_new(NULL, CPF_STYLE_FLAT|CPF_DO_NOT_USE_BORDER));
  dt_gui_key_accel_block_on_focus (GTK_WIDGET (g->camera_model));
  gtk_button_set_label(g->camera_model, self->dev->image_storage.exif_model);
  gtk_label_set_ellipsize(GTK_LABEL(gtk_bin_get_child(GTK_BIN(g->camera_model))), PANGO_ELLIPSIZE_END);
  g_signal_connect (G_OBJECT (g->camera_model), "clicked",
                    G_CALLBACK (camera_menusearch_clicked), self);
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(g->camera_model), TRUE, TRUE, 0);
  button = dtgtk_button_new(dtgtk_cairo_paint_refresh, CPF_STYLE_FLAT);
  g->find_camera_button = button;
  gtk_box_pack_start(GTK_BOX(hbox), button, FALSE, FALSE, 0);
  g_object_set(G_OBJECT(button), "tooltip-text", _("find camera"), (char *)NULL);
  g_signal_connect (G_OBJECT (button), "clicked",
                    G_CALLBACK (camera_autosearch_clicked), self);
  gtk_table_attach(GTK_TABLE(self->widget), hbox, 0, 2, 0, 1, GTK_SHRINK|GTK_FILL, 0, 0, 0);

  // lens selector
  hbox = gtk_hbox_new(FALSE, 0);
  g->lens_model = GTK_BUTTON(dtgtk_button_new(NULL, CPF_STYLE_FLAT|CPF_DO_NOT_USE_BORDER));
  dt_gui_key_accel_block_on_focus (GTK_WIDGET (g->lens_model));
  gtk_button_set_label(g->lens_model, self->dev->image_storage.exif_lens);
  gtk_label_set_ellipsize(GTK_LABEL(gtk_bin_get_child(GTK_BIN(g->lens_model))), PANGO_ELLIPSIZE_END);
  g_signal_connect (G_OBJECT (g->lens_model), "clicked",
                    G_CALLBACK (lens_menusearch_clicked), self);
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(g->lens_model), TRUE, TRUE, 0);
  button = dtgtk_button_new(dtgtk_cairo_paint_refresh, CPF_STYLE_FLAT);
  g->find_lens_button = GTK_WIDGET(button);
  gtk_box_pack_start(GTK_BOX(hbox), button, FALSE, FALSE, 0);
  g_object_set(G_OBJECT(button), "tooltip-text", _("find lens"), (char *)NULL);
  g_signal_connect (G_OBJECT (button), "clicked",
                    G_CALLBACK (lens_autosearch_clicked), self);
  gtk_table_attach(GTK_TABLE(self->widget), hbox, 0, 2, 1, 2, GTK_SHRINK|GTK_FILL, 0, 0, 0);


  // lens properties
  g->lens_param_box = gtk_hbox_new(FALSE, 0);
  gtk_table_attach(GTK_TABLE(self->widget), GTK_WIDGET(g->lens_param_box), 0, 2, 2, 3, GTK_EXPAND|GTK_FILL|GTK_SHRINK, 0, 0, 0);

#if 0
  // if unambigious info is there, use it.
  if(self->dev->image_storage.exif_lens[0] != '\0')
  {
    char make [200], model [200];
    const gchar *txt = gtk_entry_get_text(GTK_ENTRY(g->lens_model));
    parse_maker_model (txt, make, sizeof (make), model, sizeof (model));
    dt_pthread_mutex_lock(&darktable.plugin_threadsafe);
    const lfLens **lenslist = lf_db_find_lenses_hd (dt_iop_lensfun_db, g->camera,
                              make [0] ? make : NULL,
                              model [0] ? model : NULL, 0);
    if(lenslist && !lenslist[1]) lens_set (self, lenslist[0]);
    lf_free (lenslist);
    dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);
  }
#endif

  // target geometry
  label = dtgtk_reset_label_new(_("geometry"), self, &p->target_geom, sizeof(lfLensType));
  gtk_table_attach(GTK_TABLE(self->widget), label, 0, 1, 3, 4, GTK_FILL, 0, 0, 0);

  g->target_geom = GTK_COMBO_BOX(gtk_combo_box_new_text());
  g_object_set(G_OBJECT(g->target_geom), "tooltip-text",
               _("target geometry"), (char *)NULL);
  gtk_combo_box_append_text(g->target_geom, _("rectilinear"));
  gtk_combo_box_append_text(g->target_geom, _("fish-eye"));
  gtk_combo_box_append_text(g->target_geom, _("panoramic"));
  gtk_combo_box_append_text(g->target_geom, _("equirectangular"));
  gtk_combo_box_set_active(g->target_geom, p->target_geom - LF_UNKNOWN - 1);
  g_signal_connect (G_OBJECT (g->target_geom), "changed",
                    G_CALLBACK (target_geometry_changed),
                    (gpointer)self);
  gtk_table_attach(GTK_TABLE(self->widget), GTK_WIDGET(g->target_geom), 1, 2, 3, 4, GTK_EXPAND|GTK_FILL, 0, 0, 0);

  // scale
  g->scale = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.1, 2.0, 0.005, p->scale, 3));
  g_signal_connect (G_OBJECT (g->scale), "value-changed",
                    G_CALLBACK (scale_changed), self);
  dtgtk_slider_set_label(g->scale, _("scale"));
  hbox = gtk_hbox_new(FALSE, 0);
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(g->scale), TRUE, TRUE, 0);

  button = dtgtk_button_new(dtgtk_cairo_paint_refresh, CPF_STYLE_FLAT);
  g->auto_scale_button = GTK_WIDGET(button);
  gtk_box_pack_start(GTK_BOX(hbox), button, FALSE, FALSE, 0);
  g_object_set(G_OBJECT(button), "tooltip-text", _("auto scale"), (char *)NULL);
  g_signal_connect (G_OBJECT (button), "clicked",
                    G_CALLBACK (autoscale_pressed), self);
  gtk_table_attach(GTK_TABLE(self->widget), hbox, 0, 2, 4, 5, GTK_EXPAND|GTK_FILL, 0, 0, 0);


  // reverse direction
  g->reverse = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("reverse")));
  g_object_set(G_OBJECT(g->reverse), "tooltip-text", _("apply distortions instead of correcting them"), (char *)NULL);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->reverse), p->inverse);
  gtk_table_attach(GTK_TABLE(self->widget), GTK_WIDGET(g->reverse), 1, 2, 5, 6, GTK_EXPAND|GTK_FILL, 0, 0, 0);
  g_signal_connect (G_OBJECT (g->reverse), "toggled",
                    G_CALLBACK (reverse_toggled), self);

  // override linear tca (if not 1.0):
  g->tca_r = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR, 0.99, 1.01, 0.0001, p->tca_r, 5));
  g->tca_b = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR, 0.99, 1.01, 0.0001, p->tca_b, 5));
  dtgtk_slider_set_label(g->tca_r, _("tca R"));
  dtgtk_slider_set_label(g->tca_b, _("tca B"));
  gtk_table_attach(GTK_TABLE(self->widget), GTK_WIDGET(g->tca_r), 0, 2, 6, 7, GTK_EXPAND|GTK_FILL, 0, 0, 0);
  gtk_table_attach(GTK_TABLE(self->widget), GTK_WIDGET(g->tca_b), 0, 2, 7, 8, GTK_EXPAND|GTK_FILL, 0, 0, 0);
  g_signal_connect (G_OBJECT (g->tca_r), "value-changed", G_CALLBACK (tca_changed), self);
  g_signal_connect (G_OBJECT (g->tca_b), "value-changed", G_CALLBACK (tca_changed), self);
  g_object_set(G_OBJECT(g->tca_r), "tooltip-text", _("override transversal chromatic aberration correction for red channel\nleave at 1.0 for defaults"), (char *)NULL);
  g_object_set(G_OBJECT(g->tca_b), "tooltip-text", _("override transversal chromatic aberration correction for blue channel\nleave at 1.0 for defaults"), (char *)NULL);
}


void gui_update(struct dt_iop_module_t *self)
{
  // let gui elements reflect params
  dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)self->gui_data;
  dt_iop_lensfun_params_t *p = (dt_iop_lensfun_params_t *)self->params;
  dt_iop_lensfun_global_data_t *gd = (dt_iop_lensfun_global_data_t *)self->data;
  lfDatabase *dt_iop_lensfun_db = (lfDatabase *)gd->db;
  gtk_button_set_label(g->camera_model, p->camera);
  gtk_button_set_label(g->lens_model, p->lens);
  gtk_label_set_ellipsize(GTK_LABEL(gtk_bin_get_child(GTK_BIN(g->camera_model))), PANGO_ELLIPSIZE_END);
  gtk_label_set_ellipsize(GTK_LABEL(gtk_bin_get_child(GTK_BIN(g->lens_model))), PANGO_ELLIPSIZE_END);
  gtk_combo_box_set_active(g->target_geom, p->target_geom - LF_UNKNOWN - 1);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->reverse), p->inverse);
  dtgtk_slider_set_value(g->tca_r, p->tca_r);
  dtgtk_slider_set_value(g->tca_b, p->tca_b);
  dtgtk_slider_set_value(g->scale, p->scale);
  const lfCamera **cam = NULL;
  g->camera = NULL;
  if(p->camera[0])
  {
    dt_pthread_mutex_lock(&darktable.plugin_threadsafe);
    cam = lf_db_find_cameras_ext(dt_iop_lensfun_db,
                                 NULL, p->camera, 0);
    dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);
    if(cam) g->camera = cam[0];
  }
  if(p->lens[0])
  {
    char make [200], model [200];
    const gchar *txt = gtk_button_get_label(GTK_BUTTON(g->lens_model));
    parse_maker_model (txt, make, sizeof (make), model, sizeof (model));
    dt_pthread_mutex_lock(&darktable.plugin_threadsafe);
    const lfLens **lenslist = lf_db_find_lenses_hd (dt_iop_lensfun_db, g->camera,
                              make [0] ? make : NULL,
                              model [0] ? model : NULL, 0);
    if(lenslist && !lenslist[1]) lens_set (self, lenslist[0]);
    lf_free (lenslist);
    dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);
  }
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
