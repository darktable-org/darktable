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
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <stdlib.h>
#include <math.h>
#include <assert.h>
#include <string.h>
#ifdef HAVE_GEGL
#include <gegl.h>
#endif
#include "develop/develop.h"
#include "develop/imageop.h"
#include "control/control.h"
#include "gui/gtk.h"
#include "dtgtk/slider.h"
#include "dtgtk/resetlabel.h"
#include "common/opencl.h"
#include <gtk/gtk.h>
#include <inttypes.h>

DT_MODULE(1)

typedef enum dt_iop_highlights_mode_t
{
  DT_IOP_HIGHLIGHTS_CLIP = 0,
  DT_IOP_HIGHLIGHTS_LCH = 1
}
dt_iop_highlights_mode_t;

typedef struct dt_iop_highlights_params_t
{
  dt_iop_highlights_mode_t mode;
  float blendL, blendC, blendh;
}
dt_iop_highlights_params_t;

typedef struct dt_iop_highlights_gui_data_t
{
  GtkDarktableSlider *blendL;
  GtkDarktableSlider *blendC;
  GtkDarktableSlider *blendh;
  GtkComboBox        *mode;
  GtkBox             *slider_box;
}
dt_iop_highlights_gui_data_t;

typedef struct dt_iop_highlights_data_t
{
  dt_iop_highlights_mode_t mode;
  float blendL, blendC, blendh;
}
dt_iop_highlights_data_t;

typedef struct dt_iop_highlights_global_data_t
{
  int kernel_highlights;
}
dt_iop_highlights_global_data_t;

const char *name()
{
  return _("highlight reconstruction");
}

int
groups ()
{
  return IOP_GROUP_BASIC;
}


void init_key_accels()
{
  dtgtk_slider_init_accel(darktable.control->accels_darkroom,"<Darktable>/darkroom/plugins/highlights/blend L");
  dtgtk_slider_init_accel(darktable.control->accels_darkroom,"<Darktable>/darkroom/plugins/highlights/blend C");
  dtgtk_slider_init_accel(darktable.control->accels_darkroom,"<Darktable>/darkroom/plugins/highlights/blend h");
}

static const float xyz_rgb[3][3] =    /* XYZ from RGB */
{
  { 0.412453, 0.357580, 0.180423 },
  { 0.212671, 0.715160, 0.072169 },
  { 0.019334, 0.119193, 0.950227 }
};
static const float rgb_xyz[3][3] =    /* RGB from XYZ */
{
  { 3.24048, -1.53715, -0.498536 },
  { -0.969255, 1.87599, 0.0415559 },
  { 0.0556466, -0.204041, 1.05731 }
};

// convert linear RGB to CIE-LCh
static void
rgb_to_lch(float rgb[3], float lch[3])
{
  float xyz[3], lab[3];
  xyz[0] = xyz[1] = xyz[2] = 0.0;
  for (int c=0; c<3; c++)
    for (int cc=0; cc<3; cc++)
      xyz[cc] += xyz_rgb[cc][c] * rgb[c];
  for (int c=0; c<3; c++)
    xyz[c] = xyz[c] > 0.008856 ? powf(xyz[c], 1/3.0) : 7.787*xyz[c] + 16/116.0;
  lab[0] = 116 * xyz[1] - 16;
  lab[1] = 500 * (xyz[0] - xyz[1]);
  lab[2] = 200 * (xyz[1] - xyz[2]);

  lch[0] = lab[0];
  lch[1] = sqrtf(lab[1]*lab[1]+lab[2]*lab[2]);
  lch[2] = atan2f(lab[2], lab[1]);
}

// convert CIE-LCh to linear RGB
static void
lch_to_rgb(float lch[3], float rgb[3])
{
  float xyz[3], fx, fy, fz, kappa, epsilon, tmpf, lab[3];
  epsilon = 0.008856;
  kappa = 903.3;
  lab[0] = lch[0];
  lab[1] = lch[1] * cosf(lch[2]);
  lab[2] = lch[1] * sinf(lch[2]);
  xyz[1] = (lab[0]<=kappa*epsilon) ?
           (lab[0]/kappa) : (powf((lab[0]+16.0)/116.0, 3.0));
  fy = (xyz[1]<=epsilon) ? ((kappa*xyz[1]+16.0)/116.0) : ((lab[0]+16.0)/116.0);
  fz = fy - lab[2]/200.0;
  fx = lab[1]/500.0 + fy;
  xyz[2] = (powf(fz, 3.0)<=epsilon) ? ((116.0*fz-16.0)/kappa) : (powf(fz, 3.0));
  xyz[0] = (powf(fx, 3.0)<=epsilon) ? ((116.0*fx-16.0)/kappa) : (powf(fx, 3.0));

  for (int c=0; c<3; c++)
  {
    tmpf = 0;
    for (int cc=0; cc<3; cc++)
      tmpf += rgb_xyz[c][cc] * xyz[cc];
    rgb[c] = MAX(tmpf, 0);
  }
}

#ifdef HAVE_OPENCL
int
process_cl (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_highlights_data_t *d = (dt_iop_highlights_data_t *)piece->data;
  dt_iop_highlights_global_data_t *gd = (dt_iop_highlights_global_data_t *)self->data;

  cl_int err = -999;
  const int devid = piece->pipe->devid;
  size_t sizes[] = {roi_in->width, roi_in->height, 1};
  const float clip = fminf(piece->pipe->processed_maximum[0], fminf(piece->pipe->processed_maximum[1], piece->pipe->processed_maximum[2]));
  dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_highlights, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_highlights, 1, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_highlights, 2, sizeof(int), (void *)&d->mode);
  dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_highlights, 3, sizeof(float), (void *)&clip);
  dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_highlights, 4, sizeof(float), (void *)&d->blendL);
  dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_highlights, 5, sizeof(float), (void *)&d->blendC);
  dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_highlights, 6, sizeof(float), (void *)&d->blendh);
  err = dt_opencl_enqueue_kernel_2d(darktable.opencl, devid, gd->kernel_highlights, sizes);
  if(err != CL_SUCCESS) goto error;
  return TRUE;

error:
  dt_print(DT_DEBUG_OPENCL, "[opencl_highlights] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif

void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *ivoid, void *ovoid, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_highlights_data_t *data = (dt_iop_highlights_data_t *)piece->data;
  float *in;
  float *out;
  const int ch = piece->colors;

  const float clip = fminf(piece->pipe->processed_maximum[0], fminf(piece->pipe->processed_maximum[1], piece->pipe->processed_maximum[2]));
  float inc[3], lch[3], lchc[3], lchi[3];

  switch(data->mode)
  {
    case DT_IOP_HIGHLIGHTS_LCH:
#ifdef _OPENMP
      #pragma omp parallel for schedule(dynamic) default(none) shared(ovoid, ivoid, roi_out, data, piece) private(in, out, inc, lch, lchc, lchi)
#endif
      for(int j=0; j<roi_out->height; j++)
      {
        out = (float *)ovoid + ch*roi_out->width*j;
        in  = (float *)ivoid + ch*roi_out->width*j;
        for(int i=0; i<roi_out->width; i++)
        {
          if(in[0] <= clip && in[1] <= clip && in[2] <= clip)
          {
            // fast path for well-exposed pixels.
            for(int c=0; c<3; c++) out[c] = in[c];
          }
          else
          {
            for(int c=0; c<3; c++) inc[c] = fminf(clip, in[c]);
            rgb_to_lch(in, lchi);
            rgb_to_lch(inc, lchc);
            lch[0] = lchc[0] + data->blendL * (lchi[0] - lchc[0]);
            lch[1] = lchc[1] + data->blendC * (lchi[1] - lchc[1]);
            lch[2] = lchc[2] + data->blendh * (lchi[2] - lchc[2]);
            lch_to_rgb(lch, out);
          }
          out += ch;
          in += ch;
        }
      }
      break;
    default:
    case DT_IOP_HIGHLIGHTS_CLIP:
#ifdef _OPENMP
      #pragma omp parallel for schedule(dynamic) default(none) shared(ovoid, ivoid, roi_out) private(in, out, inc, lch, lchc, lchi)
#endif
      for(int j=0; j<roi_out->height; j++)
      {
        out = (float *)ovoid + ch*roi_out->width*j;
        in  = (float *)ivoid + ch*roi_out->width*j;
        for(int i=0; i<roi_out->width; i++)
        {
          for(int c=0; c<3; c++) out[c] = fminf(clip, in[c]);
          out += ch;
          in += ch;
        }
      }
      break;
  }
}

static void
blend_callback (GtkDarktableSlider *slider, dt_iop_module_t *self)
{
  if(self->dt->gui->reset) return;
  dt_iop_highlights_params_t *p = (dt_iop_highlights_params_t *)self->params;
  dt_iop_highlights_gui_data_t *g = (dt_iop_highlights_gui_data_t *)self->gui_data;
  if      (slider == g->blendL) p->blendL = dtgtk_slider_get_value(slider);
  else if (slider == g->blendC) p->blendC = dtgtk_slider_get_value(slider);
  else if (slider == g->blendh) p->blendh = dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void
mode_changed (GtkComboBox *combo, dt_iop_module_t *self)
{
  dt_iop_highlights_params_t *p = (dt_iop_highlights_params_t *)self->params;
  dt_iop_highlights_gui_data_t *g = (dt_iop_highlights_gui_data_t *)self->gui_data;
  int active = gtk_combo_box_get_active(combo);

  switch(active)
  {
    case DT_IOP_HIGHLIGHTS_CLIP:
      p->mode = DT_IOP_HIGHLIGHTS_CLIP;
      gtk_widget_set_visible(GTK_WIDGET(g->slider_box), FALSE);
      break;
    default:
    case DT_IOP_HIGHLIGHTS_LCH:
      p->mode = DT_IOP_HIGHLIGHTS_LCH;
      gtk_widget_set_visible(GTK_WIDGET(g->slider_box), TRUE);
      gtk_widget_set_no_show_all(GTK_WIDGET(g->slider_box), FALSE);
      gtk_widget_show_all(GTK_WIDGET(g->slider_box));
      gtk_widget_set_no_show_all(GTK_WIDGET(g->slider_box), TRUE);
      break;
  }
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_highlights_params_t *p = (dt_iop_highlights_params_t *)p1;
  dt_iop_highlights_data_t *d = (dt_iop_highlights_data_t *)piece->data;
  d->blendL = p->blendL;
  d->blendC = p->blendC;
  d->blendh = p->blendh;
  d->mode   = p->mode;
}

void init_global(dt_iop_module_so_t *module)
{
  const int program = 2; // basic.cl, from programs.conf
  dt_iop_highlights_global_data_t *gd = (dt_iop_highlights_global_data_t *)malloc(sizeof(dt_iop_highlights_global_data_t));
  module->data = gd;
  gd->kernel_highlights = dt_opencl_create_kernel(darktable.opencl, program, "highlights");
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_highlights_global_data_t *gd = (dt_iop_highlights_global_data_t *)module->data;
  dt_opencl_free_kernel(darktable.opencl, gd->kernel_highlights);
  free(module->data);
  module->data = NULL;
}

void init_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_highlights_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_module_t *module = (dt_iop_module_t *)self;
  dt_iop_highlights_gui_data_t *g = (dt_iop_highlights_gui_data_t *)self->gui_data;
  dt_iop_highlights_params_t *p = (dt_iop_highlights_params_t *)module->params;
  dtgtk_slider_set_value(g->blendL, p->blendL);
  dtgtk_slider_set_value(g->blendC, p->blendC);
  dtgtk_slider_set_value(g->blendh, p->blendh);
  if(p->mode == DT_IOP_HIGHLIGHTS_CLIP)
  {
    gtk_widget_set_visible(GTK_WIDGET(g->slider_box), FALSE);
  }
  else
  {
    gtk_widget_set_visible(GTK_WIDGET(g->slider_box), TRUE);
    gtk_widget_set_no_show_all(GTK_WIDGET(g->slider_box), FALSE);
    gtk_widget_show_all(GTK_WIDGET(g->slider_box));
    gtk_widget_set_no_show_all(GTK_WIDGET(g->slider_box), TRUE);
  }
  gtk_combo_box_set_active(g->mode, p->mode);
}

void reload_defaults(dt_iop_module_t *module)
{
  // only on for raw images:
  if(module->dev->image->flags & DT_IMAGE_RAW)
    module->default_enabled = 1;
  else
    module->default_enabled = 0;

  dt_iop_highlights_params_t tmp = (dt_iop_highlights_params_t)
  {
    0, 1.0, 0.0, 0.0
  };
  memcpy(module->params, &tmp, sizeof(dt_iop_highlights_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_highlights_params_t));
}

void init(dt_iop_module_t *module)
{
  // module->data = malloc(sizeof(dt_iop_highlights_data_t));
  module->params = malloc(sizeof(dt_iop_highlights_params_t));
  module->default_params = malloc(sizeof(dt_iop_highlights_params_t));
  module->priority = 155; // module order created by iop_dependencies.py, do not edit!
  module->default_enabled = 1;
  module->params_size = sizeof(dt_iop_highlights_params_t);
  module->gui_data = NULL;
}

void cleanup(dt_iop_module_t *module)
{
  free(module->gui_data);
  module->gui_data = NULL;
  free(module->params);
  module->params = NULL;
}

void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_highlights_gui_data_t));
  dt_iop_highlights_gui_data_t *g = (dt_iop_highlights_gui_data_t *)self->gui_data;
  dt_iop_highlights_params_t *p = (dt_iop_highlights_params_t *)self->params;

  self->widget = GTK_WIDGET(gtk_vbox_new(FALSE, 5));

  GtkBox *hbox  = GTK_BOX(gtk_hbox_new(FALSE, 5));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hbox), FALSE, FALSE, 0);
  GtkWidget *label = dtgtk_reset_label_new(_("method"), self, &p->mode, sizeof(float));
  gtk_box_pack_start(hbox, label, FALSE, FALSE, 0);
  g->mode = GTK_COMBO_BOX(gtk_combo_box_new_text());
  gtk_combo_box_append_text(g->mode, _("clip highlights"));
  gtk_combo_box_append_text(g->mode, _("reconstruct in LCh"));
  g_object_set(G_OBJECT(g->mode), "tooltip-text", _("highlight reconstruction method"), (char *)NULL);
  gtk_box_pack_start(hbox, GTK_WIDGET(g->mode), TRUE, TRUE, 0);

  g->slider_box = GTK_BOX(gtk_vbox_new(FALSE, 5));
  gtk_widget_set_no_show_all(GTK_WIDGET(g->slider_box), TRUE);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->slider_box), FALSE, FALSE, 0);

  g->blendL = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 1.0, 0.01, p->blendL, 3));
  g->blendC = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 1.0, 0.01, p->blendC, 3));
  g->blendh = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 1.0, 0.01, p->blendh, 3));
  g_object_set(G_OBJECT(g->blendL), "tooltip-text", _("blend lightness (0 is same as clipping)"), (char *)NULL);
  g_object_set(G_OBJECT(g->blendC), "tooltip-text", _("blend colorness (0 is same as clipping)"), (char *)NULL);
  g_object_set(G_OBJECT(g->blendh), "tooltip-text", _("blend hue (0 is same as clipping)"), (char *)NULL);
  dtgtk_slider_set_label(g->blendL,_("blend L"));
  dtgtk_slider_set_label(g->blendC,_("blend C"));
  dtgtk_slider_set_label(g->blendh,_("blend h"));
  dtgtk_slider_set_accel(g->blendL,darktable.control->accels_darkroom,"<Darktable>/darkroom/plugins/highlights/blend L");
  dtgtk_slider_set_accel(g->blendC,darktable.control->accels_darkroom,"<Darktable>/darkroom/plugins/highlights/blend C");
  dtgtk_slider_set_accel(g->blendh,darktable.control->accels_darkroom,"<Darktable>/darkroom/plugins/highlights/blend h");
  dtgtk_slider_set_default_value(g->blendL, p->blendL);
  dtgtk_slider_set_default_value(g->blendC, p->blendC);
  dtgtk_slider_set_default_value(g->blendh, p->blendh);
  gtk_box_pack_start(g->slider_box, GTK_WIDGET(g->blendL), TRUE, TRUE, 0);
  gtk_box_pack_start(g->slider_box, GTK_WIDGET(g->blendC), TRUE, TRUE, 0);
  gtk_box_pack_start(g->slider_box, GTK_WIDGET(g->blendh), TRUE, TRUE, 0);

  g_signal_connect (G_OBJECT (g->blendL), "value-changed",
                    G_CALLBACK (blend_callback), self);
  g_signal_connect (G_OBJECT (g->blendC), "value-changed",
                    G_CALLBACK (blend_callback), self);
  g_signal_connect (G_OBJECT (g->blendh), "value-changed",
                    G_CALLBACK (blend_callback), self);
  g_signal_connect (G_OBJECT (g->mode), "changed",
                    G_CALLBACK (mode_changed), self);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
