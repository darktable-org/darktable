/*
    This file is part of darktable,
    copyright (c) 2014 LebedevRI.

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
#include "develop/imageop.h"
#include "common/interpolation.h"
#include "develop/tiling.h"
#include "bauhaus/bauhaus.h"
#include "gui/gtk.h"
#include "gui/accelerators.h"

#include <gtk/gtk.h>
#include <stdlib.h>

DT_MODULE_INTROSPECTION(1, dt_iop_scalepixels_params_t)

typedef struct dt_iop_scalepixels_params_t
{
  // Aspect ratio of the pixels, usually 1 but some cameras need scaling
  // <1 means the image needs to be stretched vertically, (0.5 means 2x)
  // >1 means the image needs to be stretched horizontally (2 mean 2x)
  float pixel_aspect_ratio;
}
dt_iop_scalepixels_params_t;

typedef struct dt_iop_scalepixels_gui_data_t
{
  GtkWidget *pixel_aspect_ratio;
}
dt_iop_scalepixels_gui_data_t;

typedef struct dt_iop_scalepixels_params_t dt_iop_scalepixels_data_t;

const char *
name()
{
  return C_("modulename", "scale pixels");
}

int
flags()
{
  return IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_TILING_FULL_ROI |
         IOP_FLAGS_ONE_INSTANCE;
}

int
groups()
{
  return IOP_GROUP_CORRECT;
}

int
operation_tags()
{
  return IOP_TAG_DISTORT;
}

void
init_key_accels(
  dt_iop_module_so_t *self)
{
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "pixel aspect ratio"));
}

void
connect_key_accels(
  dt_iop_module_t *self)
{
  dt_iop_scalepixels_gui_data_t* g = self->gui_data;

  dt_accel_connect_slider_iop(self, "pixel aspect ratio", GTK_WIDGET(g->pixel_aspect_ratio));
}

int
output_bpp(
  dt_iop_module_t *self,
  dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return 4*sizeof(float);
}

static void
transform(
  const dt_dev_pixelpipe_iop_t *const piece, float *p)
{
  dt_iop_scalepixels_data_t *d = piece->data;

  if(d->pixel_aspect_ratio < 1.0f)
  {
    p[1] /= d->pixel_aspect_ratio;
  }
  else
  {
    p[0] *= d->pixel_aspect_ratio;
  }
}

static void
backtransform(
  const dt_dev_pixelpipe_iop_t *const piece, float *p)
{
  dt_iop_scalepixels_data_t *d = piece->data;

  if(d->pixel_aspect_ratio < 1.0f)
  {
    p[1] *= d->pixel_aspect_ratio;
  }
  else
  {
    p[0] /= d->pixel_aspect_ratio;
  }
}

int
distort_transform(
  dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece,
  float *points, size_t points_count)
{
  for (size_t i = 0; i < points_count * 2; i += 2)
  {
    transform(piece, &(points[i]));
  }

  return 1;
}

int
distort_backtransform(
  dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece,
  float *points, size_t points_count)
{
  for (size_t i = 0; i < points_count * 2; i += 2)
  {
    backtransform(piece, &(points[i]));
  }

  return 1;
}

void
modify_roi_out(
  dt_iop_module_t *self,
  dt_dev_pixelpipe_iop_t *piece,
  dt_iop_roi_t *roi_out,
  const dt_iop_roi_t * const roi_in)
{
  *roi_out = *roi_in;

  float xy[2] = {roi_out->x,     roi_out->y};
  float wh[2] = {roi_out->width, roi_out->height};

  transform(piece, xy);
  transform(piece, wh);

  roi_out->x      = (int)floorf(xy[0]);
  roi_out->y      = (int)floorf(xy[1]);
  roi_out->width  = (int)ceilf (wh[0]);
  roi_out->height = (int)ceilf (wh[1]);

  // sanity check.
  if(roi_out->x < 0)      roi_out->x = 0;
  if(roi_out->y < 0)      roi_out->y = 0;
  if(roi_out->width  < 1) roi_out->width  = 1;
  if(roi_out->height < 1) roi_out->height = 1;
}

void
modify_roi_in(
  dt_iop_module_t *self,
  dt_dev_pixelpipe_iop_t *piece,
  const dt_iop_roi_t * const roi_out,
  dt_iop_roi_t *roi_in)
{
  *roi_in = *roi_out;

  float xy[2] = {roi_in->x,     roi_in->y};
  float wh[2] = {roi_in->width, roi_in->height};

  backtransform(piece, xy);
  backtransform(piece, wh);

  roi_in->x      = (int)floorf(xy[0]);
  roi_in->y      = (int)floorf(xy[1]);
  roi_in->width  = (int)ceilf (wh[0]);
  roi_in->height = (int)ceilf (wh[1]);
}

void
process(
  dt_iop_module_t *self,
  const dt_dev_pixelpipe_iop_t *const piece,
  const void *const ivoid,
  void *ovoid,
  const dt_iop_roi_t *const roi_in,
  const dt_iop_roi_t *const roi_out)
{
  const int ch = piece->colors;
  const int ch_width = ch*roi_in->width;

  const struct dt_interpolation* interpolation = dt_interpolation_new(DT_INTERPOLATION_USERPREF);

#ifdef _OPENMP
  #pragma omp parallel for schedule(static) default(none) shared(ovoid,interpolation)
#endif
  // (slow) point-by-point transformation.
  // TODO: optimize with scanlines and linear steps between?
  for(int j=0; j<roi_out->height; j++)
  {
    float *out = ((float *)ovoid)+(size_t)4*j*roi_out->width;
    for(int i=0; i<roi_out->width; i++,out+=4)
    {
      float po[2] = {i, j};

      backtransform(piece, po);

      dt_interpolation_compute_pixel4c(
        interpolation,
        (float *)ivoid, out,
        po[0], po[1],
        roi_in->width, roi_in->height,
        ch_width);
    }
  }

}

void
commit_params(
  dt_iop_module_t *self,
  const dt_iop_params_t * const params,
  dt_dev_pixelpipe_t *pipe,
  dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_scalepixels_params_t *p = self->params;

  memcpy(piece->data, params, sizeof(dt_iop_scalepixels_data_t));

  if(isnan(p->pixel_aspect_ratio) || p->pixel_aspect_ratio <= 0.0f ||
      p->pixel_aspect_ratio == 1.0f)
    piece->enabled = 0;
}

void
tiling_callback(
  dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece,
  const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out,
  dt_develop_tiling_t *tiling)
{
  float ioratio = ((float)roi_out->width * roi_out->height) /
                  ((float)roi_in->width * roi_in->height);

  tiling->factor = 1.0f + ioratio; // in + out, no temp
  tiling->maxbuf = 1.0f;
  tiling->overhead = 0;
  tiling->overlap = 4;
  tiling->xalign = 1;
  tiling->yalign = 1;
  return;
}

void
init_pipe(
  dt_iop_module_t *self,
  dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1, sizeof(dt_iop_scalepixels_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
}

void
cleanup_pipe(
  dt_iop_module_t *self,
  dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void
reload_defaults(
  dt_iop_module_t *self)
{
  dt_iop_scalepixels_params_t tmp = (dt_iop_scalepixels_params_t)
  {
    .pixel_aspect_ratio = NAN
  };

  // we might be called from presets update infrastructure => there is no image
  if(!self || !self->dev) goto end;

  const dt_image_t * const image = &(self->dev->image_storage);

  tmp.pixel_aspect_ratio = image->pixel_aspect_ratio;

  self->default_enabled = (!isnan(tmp.pixel_aspect_ratio) &&
                           tmp.pixel_aspect_ratio  > 0.0f &&
                           tmp.pixel_aspect_ratio != 1.0f);

end:
  memcpy(self->params, &tmp, sizeof(dt_iop_scalepixels_params_t));
  memcpy(self->default_params, &tmp, sizeof(dt_iop_scalepixels_params_t));
}

void
init(
  dt_iop_module_t *self)
{
  const dt_image_t * const image = &(self->dev->image_storage);

  self->params = calloc(1, sizeof(dt_iop_scalepixels_params_t));
  self->default_params = calloc(1, sizeof(dt_iop_scalepixels_params_t));
  self->default_enabled = (!isnan(image->pixel_aspect_ratio) &&
                           image->pixel_aspect_ratio  > 0.0f &&
                           image->pixel_aspect_ratio != 1.0f);
  self->priority = 219; // module order created by iop_dependencies.py, do not edit!
  self->params_size = sizeof(dt_iop_scalepixels_params_t);
  self->gui_data = NULL;
}

void
cleanup(
  dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
  free(self->params);
  self->params = NULL;
}

void
gui_update(
  dt_iop_module_t *self)
{
  dt_iop_scalepixels_gui_data_t *g = self->gui_data;
  dt_iop_scalepixels_params_t *p = self->params;

  dt_bauhaus_slider_set(g->pixel_aspect_ratio, p->pixel_aspect_ratio);
}

static void
callback(
  GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = user_data;
  if(self->dt->gui->reset) return;

  dt_iop_scalepixels_params_t *p = self->params;

  p->pixel_aspect_ratio = dt_bauhaus_slider_get(slider);

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

void
gui_init(
  dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_scalepixels_gui_data_t));
  dt_iop_scalepixels_gui_data_t *g = self->gui_data;
  dt_iop_scalepixels_params_t *p = self->params;

  self->widget = gtk_vbox_new(TRUE, DT_BAUHAUS_SPACE);

  g->pixel_aspect_ratio = dt_bauhaus_slider_new_with_range(self, 0.0, 2.0, .1, p->pixel_aspect_ratio, 2);
  g_object_set(G_OBJECT(g->pixel_aspect_ratio), "tooltip-text", _("<1 means the image needs to be stretched vertically, (0.5 means 2x)\n>1 means the image needs to be stretched horizontally (2 mean 2x)"), (char *)NULL);
  dt_bauhaus_widget_set_label(g->pixel_aspect_ratio, NULL, _("pixel aspect ratio"));
  dt_bauhaus_slider_enable_soft_boundaries(g->pixel_aspect_ratio, 0.0, 10.0);
  g_signal_connect(G_OBJECT(g->pixel_aspect_ratio), "value-changed", G_CALLBACK(callback), self);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->pixel_aspect_ratio), TRUE, TRUE, 0);
}

void
gui_cleanup(
  dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
