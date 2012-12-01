/*
    This file is part of darktable,
    copyright (c) 2012 johannes hanika.

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
// our includes go first:
#include "develop/imageop.h"
#include "bauhaus/bauhaus.h"
#include "common/interpolation.h"
#include "common/denoise.h"
#include "gui/gtk.h"
#include "gui/simple_gui.h"

#include <gtk/gtk.h>
#include <stdlib.h>

DT_MODULE(1)

typedef struct dt_iop_useless_global_data_t
{
}
dt_iop_useless_global_data_t;

// this returns a translatable name
const char *name()
{
  return _("super resolution");
}

// some additional flags (self explanatory i think):
int
flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES;
}

// where does it appear in the gui?
int
groups()
{
  return IOP_GROUP_CORRECT;
}

// modify regions of interest, we scale the image:
void modify_roi_out(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, dt_iop_roi_t *roi_out, const dt_iop_roi_t *roi_in)
{
  float *d = (float *)piece->data;
  const float scale = d[2];
  *roi_out = *roi_in;
  if(piece->pipe->type == DT_DEV_PIXELPIPE_PREVIEW ||
     piece->pipe->type == DT_DEV_PIXELPIPE_THUMBNAIL) return;
  // don't set scale (or else segfault because the input buffer will be ridiculously oversized)
  roi_out->width  = scale * roi_in->width;
  roi_out->height = scale * roi_in->height;
}

void modify_roi_in(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, const dt_iop_roi_t *roi_out, dt_iop_roi_t *roi_in)
{
  float *d = (float *)piece->data;
  float scale = d[2];
  *roi_in = *roi_out;
  if(piece->pipe->type == DT_DEV_PIXELPIPE_PREVIEW ||
     piece->pipe->type == DT_DEV_PIXELPIPE_THUMBNAIL) return;

  // if the requested scale is < 1, we don't actually want to request downsized
  // and upsize it again, but just use it as good as it gets first:
  if(roi_in->scale < 1.0f)
  {
    // let's take away scale from roi_in->scale, but only until we reach 1:1
    const float iscale = MIN(1.0f, roi_in->scale * scale);
    // given this new input scale, what will be left to scale?
    scale = (roi_in->scale * scale) / iscale;
    roi_in->scale = iscale;
  }

  // don't set scale (or else segfault because the input buffer will be ridiculously oversized)
  roi_in->x = roi_out->x/scale;
  roi_in->y = roi_out->y/scale;
  roi_in->width  = roi_out->width /scale;
  roi_in->height = roi_out->height/scale;
}

/** process, all real work is done here. */
void process(
    struct dt_iop_module_t *self,
    dt_dev_pixelpipe_iop_t *piece,
    void *ivoid,
    void *ovoid,
    const dt_iop_roi_t *roi_in,
    const dt_iop_roi_t *roi_out)
{
  if(piece->pipe->type == DT_DEV_PIXELPIPE_PREVIEW ||
     piece->pipe->type == DT_DEV_PIXELPIPE_THUMBNAIL)
  {
    // nothing to do from this distance:
    memcpy (ovoid, ivoid, sizeof(float)*4*roi_in->width*roi_in->height);
    return;
  }

  /*
  fprintf(stderr, "process with roi in %d %d %d %d (%f)-- out %d %d %d %d (%f)\n",
      roi_in->x, roi_in->y, roi_in->width, roi_in->height, roi_in->scale,
      roi_out->x, roi_out->y, roi_out->width, roi_out->height, roi_out->scale);
      */

  float *d = (float *)piece->data; // the default param format is an array of int or float, depending on the type of widget
  const float radius   = d[0];
  const float strength = d[1];
  const float scale    = d[2];
  const float luma     = d[3]/10.0f;
  const float chroma   = d[4]/10.0f;

  // adjust to zoom size:
  const int P = ceilf(radius * roi_in->scale / piece->iscale); // pixel filter size
  const int K = ceilf(7 * roi_in->scale / piece->iscale); // nbhood
  // const float sharpness = 0.0f;//1000000.0f/(1.0f+strength);
  const float sharpness = 1.0f/(100.0f+strength);
#if 0
  if(P < 1)
  {
    // nothing to do from this distance:
    // TODO: this is wrong and needs repair:
    memcpy (ovoid, ivoid, sizeof(float)*4*roi_in->width*roi_in->height);
    return;
  }
#endif

  // adjust to Lab, make L more important
  // TODO: put that where?
  // float max_L = 120.0f, max_C = 512.0f;
  // float nL = 1.0f/max_L, nC = 1.0f/max_C;
  // const float norm2[4] = { nL*nL, nC*nC, nC*nC, 1.0f };

  float *tmp = (float *)dt_alloc_align(64, sizeof(float)*roi_out->width*dt_get_num_threads());

  // prior_feature = blur(input) = U(D(I))
  float *prior_feature = (float *)dt_alloc_align(64, 4*sizeof(float)*roi_in->width*roi_in->height);
  // prior_payload = input - prior_feature
  float *prior_payload = (float *)dt_alloc_align(64, 4*sizeof(float)*roi_in->width*roi_in->height);
  const float *input = (float *)ivoid;

#if 0
  const float filter[5] = {1.0f/16.0f, 4.0f/16.0f, 6.0f/16.0f, 4.0f/16.0f, 1.0f/16.0f};
  // blur lines in parallel
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static) shared(input,prior_payload,roi_in)
#endif
  for(int j=0;j<roi_in->height;j++)
  {
    for(int i=2;i<roi_in->width-3;i++)
    {
      for(int c=0;c<3;c++)
      {
        float sum = 0.0f;
        for(int k=-2;k<=2;k++)
          sum += filter[2+k] * input[4*(j*roi_in->width + i + k) + c];
        prior_payload[4*(j*roi_in->width + i) + c] = sum; // abused as temp buffer
      }
    }
  }
  // blur columns in parallel
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static) shared(prior_payload,prior_feature,roi_in)
#endif
  for(int i=0;i<roi_in->width;i++)
  {
    for(int j=2;j<roi_in->height-2;j++)
    {
      for(int c=0;c<3;c++)
      {
        float sum = 0.0f;
        for(int k=-2;k<=2;k++)
          sum += filter[2+k] * prior_payload[4*((j+k)*roi_in->width + i) + c];
        prior_feature[4*(j*roi_in->width + i) + c] = sum;
      }
    }
  }
#else
#if 1
  dt_iop_roi_t roii = *roi_in;
  dt_iop_roi_t roio = *roi_in;
  roio.x = roii.x = 0;
  roio.y = roii.y = 0;
  roio.width  = roii.width  / scale;
  roio.height = roii.height / scale;
  roii.scale = 1.0f;
  roio.scale = 1.0f/scale;
  // need this to be smooth and without overshoots/ringing:
  const struct dt_interpolation* itor = dt_interpolation_new(DT_INTERPOLATION_BILINEAR);//CUBIC);
  dt_interpolation_resample(itor, prior_payload, &roio, roio.width*4*sizeof(float),
                                  input,         &roii, roii.width*4*sizeof(float));
  roio.scale = 1.0f;
  roii.scale = scale;
  dt_interpolation_resample(itor, prior_feature, &roii, roii.width*4*sizeof(float),
                                  prior_payload, &roio, roio.width*4*sizeof(float));
#endif
  // now prior_feature = U(D(I))
#endif

#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static) shared(prior_payload,prior_feature,input,roi_in)
#endif
  for(int i=0;i<roi_in->width*roi_in->height;i++)
  {
    // if that doesn't long for sse.
    prior_payload[4*i]   = input[4*i]   - prior_feature[4*i];
    prior_payload[4*i+1] = input[4*i+1] - prior_feature[4*i+1];
    prior_payload[4*i+2] = input[4*i+2] - prior_feature[4*i+2];
  }


  // output payload will be nlm filtered result of the above three buffers.
  float *output_payload = (float *)dt_alloc_align(64, 4*sizeof(float)*roi_out->width*roi_out->height);
  // we want to sum up weights in col[3], so need to init to 0:
  memset(output_payload, 0x0, sizeof(float)*roi_out->width*roi_out->height*4);

#if 1
  // input_feature = upsample(input) (stored in out buffer)
  roii = *roi_in;
  roio = *roi_out;
  roii.scale = 1.0f;
  roio.x = roii.x = 0;
  roio.y = roii.y = 0;
  roio.scale = roi_out->width / (float)roi_in->width;
  // need this to be smooth:
  // const struct dt_interpolation* itor = dt_interpolation_new(DT_INTERPOLATION_BICUBIC);
  dt_interpolation_resample(itor, (float *)ovoid, &roio, roi_out->width*4*sizeof(float),
                                  (float *)ivoid, &roii, roi_in->width *4*sizeof(float));
#endif

#if 1
  // output_payload = nlm(of the above)
  dt_nlm_accum_scaled((float *)ovoid, prior_payload, prior_feature, output_payload,
      roi_out->width, roi_out->height, roi_in->width, roi_in->height, P, K, sharpness, tmp);
  // TODO: multiple scales for denoising?
  dt_nlm_normalize_add((float *)ovoid, output_payload, roi_out->width, roi_out->height, luma, chroma);

#else
  // DEBUG: XXX
  for(int j=0;j<roi_in->height;j++)
  {
    memcpy(ovoid + 4*sizeof(float)*roi_out->width*j, prior_feature + 4*roi_in->width*j, 4*sizeof(float)*roi_in->width);
    memcpy(ovoid + 4*sizeof(float)*(roi_out->width*j + roi_in->width), prior_payload + 4*roi_in->width*j, 4*sizeof(float)*roi_in->width);
  }

#endif

  free(prior_feature);
  free(prior_payload);
  free(output_payload);
  free(tmp);

  // TODO: upsample that? or if output is the upsampled input just keep it?
  // if(piece->pipe->mask_display)
    // dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);
}


/** init, cleanup, commit to pipeline. when using the simple api you don't need to care about params, ... */
void init(dt_iop_module_t *module)
{
  // order has to be changed by editing the dependencies in tools/iop_dependencies.py
  module->priority = 471; // module order created by iop_dependencies.py, do not edit!
}

/** gui callbacks, these are needed. */
dt_gui_simple_t* gui_init_simple(dt_iop_module_so_t *self) // sorry, only dt_iop_module_so_t* in here :(
{
  static dt_gui_simple_t gui =
  {
    0, // not used currently
    {
      /* patch size */
      {
        .slider = {
          DT_SIMPLE_GUI_SLIDER,
          "patch size",                                     // always make sure to add an id
          N_("patch size"),                                 // just mark the strings for translation using N_()
          N_("radius of the patches to search for"),        // same here
          NULL,                                             // the rest are specific settings for sliders
          1.0f, 4.0f, 1.0f, 2.0f,
          0,
          NULL,                                        // when no callback is specified a default one is used
          NULL                                         // no parameter means self. keep that in mind when you want to pass the number 0!
        }
      },

      /* strength */
      {
        .slider = {
          DT_SIMPLE_GUI_SLIDER,
          "strength",                                     // always make sure to add an id
          N_("strength"),                                 // just mark the strings for translation using N_()
          N_("strength of the effect"),                   // same here
          NULL,                                           // the rest are specific settings for sliders
          0.0f, 100.0f, 1.0f, 50.0f,
          0,
          NULL,                                           // when no callback is specified a default one is used
          NULL                                            // no parameter means self. keep that in mind when you want to pass the number 0!
        }
      },

      /* scale */
      {
        .slider = {
          DT_SIMPLE_GUI_SLIDER,
          "scale",                                     // always make sure to add an id
          N_("scale"),                                 // just mark the strings for translation using N_()
          N_("how much to scale up"),                  // same here
          NULL,                                        // the rest are specific settings for sliders
          1.0f, 3.0f, 0.1f, 2.0f,
          0,
          NULL,                                        // when no callback is specified a default one is used
          NULL                                         // no parameter means self. keep that in mind when you want to pass the number 0!
        }
      },

      /* luma */
      {
        .slider = {
          DT_SIMPLE_GUI_SLIDER,
          "luma",                                     // always make sure to add an id
          N_("luma"),                                 // just mark the strings for translation using N_()
          N_("how much to affect brightness"),        // same here
          NULL,                                       // the rest are specific settings for sliders
          0.0f, 100.0f, 1.0f, 50.0f,
          0,
          NULL,                                       // when no callback is specified a default one is used
          NULL                                        // no parameter means self. keep that in mind when you want to pass the number 0!
        }
      },

      /* chroma */
      {
        .slider = {
          DT_SIMPLE_GUI_SLIDER,
          "chroma",                                     // always make sure to add an id
          N_("chroma"),                                 // just mark the strings for translation using N_()
          N_("how much to affect colors"),              // same here
          NULL,                                         // the rest are specific settings for sliders
          0.0f, 100.0f, 1.0f, 100.0f,
          0,
          NULL,                                         // when no callback is specified a default one is used
          NULL                                          // no parameter means self. keep that in mind when you want to pass the number 0!
        }
      },

      /** the last element has to be of type DT_SIMPLE_GUI_NONE */
      {.common = {DT_SIMPLE_GUI_NONE, NULL, NULL, NULL}}
    }
  };

  return &gui;
}

/** not needed when using the simple gui api. */
// void gui_init(dt_iop_module_t* self);
// void gui_cleanup(dt_iop_module_t *self);
// void gui_update(dt_iop_module_t *self);

/** additional, optional callbacks to capture darkroom center events. */
// void gui_post_expose(dt_iop_module_t *self, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx, int32_t pointery);
// int mouse_moved(dt_iop_module_t *self, double x, double y, int which);
// int button_pressed(dt_iop_module_t *self, double x, double y, int which, int type, uint32_t state);
// int button_released(struct dt_iop_module_t *self, double x, double y, int which, uint32_t state);
// int scrolled(dt_iop_module_t *self, double x, double y, int up, uint32_t state);

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
