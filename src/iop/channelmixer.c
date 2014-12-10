/*
  This file is part of darktable,
  copyright (c) 2010-2012 Henrik Andersson.

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
#include "bauhaus/bauhaus.h"
#include "common/colorspaces.h"
#include "common/opencl.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "control/control.h"
#include "common/debug.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/presets.h"

#include <gtk/gtk.h>
#include <inttypes.h>
#include <stdlib.h>
#include <math.h>
#include <assert.h>
#include <string.h>

/** Crazy presets b&w ...
  Film Type			R		G		B						R	G	B
  AGFA 200X		18		41		41		Ilford Pan F		33	36	31
  Agfapan 25		25		39		36		Ilford SFX		36	31	33
  Agfapan 100		21		40		39		Ilford XP2 Super	21	42	37
  Agfapan 400		20		41		39		Kodak T-Max 100	24	37	39
  Ilford Delta 100	21		42		37		Kodak T-Max 400	27	36	37
  Ilford Delta 400	22		42		36		Kodak Tri-X 400	25	35	40
  Ilford Delta 3200	31		36		33		Normal Contrast	43	33	30
  Ilford FP4		28		41		31		High Contrast		40	34	60
  Ilford HP5		23		37		40		Generic B/W		24	68	8
*/

#define CLIP(x) ((x < 0) ? 0.0 : (x > 1.0) ? 1.0 : x)

DT_MODULE_INTROSPECTION(1, dt_iop_channelmixer_params_t)

typedef enum _channelmixer_output_t
{
  /** mixes into hue channel */
  CHANNEL_HUE = 0,
  /** mixes into lightness channel */
  CHANNEL_SATURATION,
  /** mixes into lightness channel */
  CHANNEL_LIGHTNESS,
  /** mixes into red channel of image */
  CHANNEL_RED,
  /** mixes into green channel of image */
  CHANNEL_GREEN,
  /** mixes into blue channel of image */
  CHANNEL_BLUE,
  /** mixes into gray channel of image = monochrome*/
  CHANNEL_GRAY,

  CHANNEL_SIZE
} _channelmixer_output_t;

typedef struct dt_iop_channelmixer_params_t
{
  //_channelmixer_output_t output_channel;
  /** amount of red to mix value -1.0 - 1.0 */
  float red[CHANNEL_SIZE];
  /** amount of green to mix value -1.0 - 1.0 */
  float green[CHANNEL_SIZE];
  /** amount of blue to mix value -1.0 - 1.0 */
  float blue[CHANNEL_SIZE];
} dt_iop_channelmixer_params_t;

typedef struct dt_iop_channelmixer_gui_data_t
{
  GtkBox *vbox;
  GtkWidget *combo1;                      // Output channel
  GtkLabel *dtlabel1, *dtlabel2;          // output channel, source channels
  GtkLabel *label1, *label2, *label3;     // red, green, blue
  GtkWidget *scale1, *scale2, *scale3;    // red, green, blue
} dt_iop_channelmixer_gui_data_t;

typedef struct dt_iop_channelmixer_data_t
{
  //_channelmixer_output_t output_channel;
  float red[CHANNEL_SIZE];
  float green[CHANNEL_SIZE];
  float blue[CHANNEL_SIZE];
} dt_iop_channelmixer_data_t;

typedef struct dt_iop_channelmixer_global_data_t
{
  int kernel_channelmixer;
} dt_iop_channelmixer_global_data_t;


const char *name()
{
  return _("channel mixer");
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING;
}

int groups()
{
  return IOP_GROUP_COLOR;
}

#if 0 // BAUHAUS doesn't support keyaccels yet...
void init_key_accels(dt_iop_module_so_t *self)
{
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "red"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "green"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "blue"));
}

void connect_key_accels(dt_iop_module_t *self)
{
  dt_iop_channelmixer_gui_data_t *g =
    (dt_iop_channelmixer_gui_data_t*)self->gui_data;

  dt_accel_connect_slider_iop(self, "red", GTK_WIDGET(g->scale1));
  dt_accel_connect_slider_iop(self, "green", GTK_WIDGET(g->scale2));
  dt_accel_connect_slider_iop(self, "blue", GTK_WIDGET(g->scale3));
}
#endif

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *ivoid, void *ovoid,
             const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  const dt_iop_channelmixer_data_t *data = (dt_iop_channelmixer_data_t *)piece->data;
  const gboolean gray_mix_mode = (data->red[CHANNEL_GRAY] != 0.0 || data->green[CHANNEL_GRAY] != 0.0
                                  || data->blue[CHANNEL_GRAY] != 0.0)
                                     ? TRUE
                                     : FALSE;
  const int ch = piece->colors;

#ifdef _OPENMP
#pragma omp parallel for default(none) shared(ivoid, ovoid, roi_in, roi_out, data) schedule(static)
#endif
  for(int j = 0; j < roi_out->height; j++)
  {
    const float *in = ((float *)ivoid) + (size_t)ch * j * roi_out->width;
    float *out = ((float *)ovoid) + (size_t)ch * j * roi_out->width;
    for(int i = 0; i < roi_out->width; i++)
    {
      float h, s, l, hmix, smix, lmix, rmix, gmix, bmix, graymix;
      // Calculate the HSL mix
      hmix = CLIP(in[0] * data->red[CHANNEL_HUE]) + (in[1] * data->green[CHANNEL_HUE])
             + (in[2] * data->blue[CHANNEL_HUE]);
      smix = CLIP(in[0] * data->red[CHANNEL_SATURATION]) + (in[1] * data->green[CHANNEL_SATURATION])
             + (in[2] * data->blue[CHANNEL_SATURATION]);
      lmix = CLIP(in[0] * data->red[CHANNEL_LIGHTNESS]) + (in[1] * data->green[CHANNEL_LIGHTNESS])
             + (in[2] * data->blue[CHANNEL_LIGHTNESS]);

      // If HSL mix is used apply to out[]
      if(hmix != 0.0 || smix != 0.0 || lmix != 0.0)
      {
        // mix into HSL output channels
        rgb2hsl(in, &h, &s, &l);
        h = (hmix != 0.0) ? hmix : h;
        s = (smix != 0.0) ? smix : s;
        l = (lmix != 0.0) ? lmix : l;
        hsl2rgb(out, h, s, l);
      }
      else // no HSL copt in[] to out[]
        for(int i = 0; i < 3; i++) out[i] = in[i];

      // Calculate graymix and RGB mix
      graymix = CLIP((out[0] * data->red[CHANNEL_GRAY]) + (out[1] * data->green[CHANNEL_GRAY])
                     + (out[2] * data->blue[CHANNEL_GRAY]));

      rmix = CLIP((out[0] * data->red[CHANNEL_RED]) + (out[1] * data->green[CHANNEL_RED])
                  + (out[2] * data->blue[CHANNEL_RED]));
      gmix = CLIP((out[0] * data->red[CHANNEL_GREEN]) + (out[1] * data->green[CHANNEL_GREEN])
                  + (out[2] * data->blue[CHANNEL_GREEN]));
      bmix = CLIP((out[0] * data->red[CHANNEL_BLUE]) + (out[1] * data->green[CHANNEL_BLUE])
                  + (out[2] * data->blue[CHANNEL_BLUE]));


      if(gray_mix_mode) // Graymix is used...
        out[0] = out[1] = out[2] = graymix;
      else // RGB mix is used...
      {
        out[0] = rmix;
        out[1] = gmix;
        out[2] = bmix;
      }

      /*mix = CLIP( in[0] * data->red)+( in[1] * data->green)+( in[2] * data->blue );

      if( data->output_channel <= CHANNEL_LIGHTNESS ) {
        // mix into HSL output channels
        rgb2hsl(in,&h,&s,&l);
        h = ( data->output_channel == CHANNEL_HUE )              ? mix : h;
        s = ( data->output_channel == CHANNEL_SATURATION )   ? mix : s;
        l = ( data->output_channel == CHANNEL_LIGHTNESS )     ?  mix : l;
        hsl2rgb(out,h,s,l);
      } else  if( data->output_channel > CHANNEL_LIGHTNESS && data->output_channel  < CHANNEL_GRAY) {
        // mix into rgb output channels
        out[0] = ( data->output_channel == CHANNEL_RED )      ? mix : in[0];
        out[1] = ( data->output_channel == CHANNEL_GREEN )  ? mix : in[1];
        out[2] = ( data->output_channel == CHANNEL_BLUE )     ? mix : in[2];
      } else   if( data->output_channel <= CHANNEL_GRAY ) {
        out[0]=out[1]=out[2] = mix;
      }
      */
      out += ch;
      in += ch;
    }
  }

  if(piece->pipe->mask_display) dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);
}


#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_channelmixer_data_t *data = (dt_iop_channelmixer_data_t *)piece->data;
  dt_iop_channelmixer_global_data_t *gd = (dt_iop_channelmixer_global_data_t *)self->data;

  cl_mem dev_red = NULL;
  cl_mem dev_green = NULL;
  cl_mem dev_blue = NULL;

  cl_int err = -999;

  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;

  const int gray_mix_mode = (data->red[CHANNEL_GRAY] != 0.0f || data->green[CHANNEL_GRAY] != 0.0f
                             || data->blue[CHANNEL_GRAY] != 0.0f)
                                ? TRUE
                                : FALSE;

  size_t sizes[] = { ROUNDUPWD(width), ROUNDUPHT(height), 1 };

  dev_red = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * CHANNEL_SIZE, data->red);
  if(dev_red == NULL) goto error;
  dev_green = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * CHANNEL_SIZE, data->green);
  if(dev_green == NULL) goto error;
  dev_blue = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * CHANNEL_SIZE, data->blue);
  if(dev_blue == NULL) goto error;

  dt_opencl_set_kernel_arg(devid, gd->kernel_channelmixer, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_channelmixer, 1, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(devid, gd->kernel_channelmixer, 2, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_channelmixer, 3, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_channelmixer, 4, sizeof(int), (void *)&gray_mix_mode);
  dt_opencl_set_kernel_arg(devid, gd->kernel_channelmixer, 5, sizeof(cl_mem), (void *)&dev_red);
  dt_opencl_set_kernel_arg(devid, gd->kernel_channelmixer, 6, sizeof(cl_mem), (void *)&dev_green);
  dt_opencl_set_kernel_arg(devid, gd->kernel_channelmixer, 7, sizeof(cl_mem), (void *)&dev_blue);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_channelmixer, sizes);
  if(err != CL_SUCCESS) goto error;

  dt_opencl_release_mem_object(dev_red);
  dt_opencl_release_mem_object(dev_green);
  dt_opencl_release_mem_object(dev_blue);

  return TRUE;

error:
  if(dev_red != NULL) dt_opencl_release_mem_object(dev_red);
  if(dev_green != NULL) dt_opencl_release_mem_object(dev_green);
  if(dev_blue != NULL) dt_opencl_release_mem_object(dev_blue);
  dt_print(DT_DEBUG_OPENCL, "[opencl_channelmixer] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif

void init_global(dt_iop_module_so_t *module)
{
  const int program = 8; // extended.cl, from programs.conf
  dt_iop_channelmixer_global_data_t *gd
      = (dt_iop_channelmixer_global_data_t *)malloc(sizeof(dt_iop_channelmixer_global_data_t));
  module->data = gd;
  gd->kernel_channelmixer = dt_opencl_create_kernel(program, "channelmixer");
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_channelmixer_global_data_t *gd = (dt_iop_channelmixer_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_channelmixer);
  free(module->data);
  module->data = NULL;
}

static void red_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_channelmixer_params_t *p = (dt_iop_channelmixer_params_t *)self->params;
  dt_iop_channelmixer_gui_data_t *g = (dt_iop_channelmixer_gui_data_t *)self->gui_data;
  int combo1_index = dt_bauhaus_combobox_get(g->combo1);
  if(combo1_index >= 0)
  {
    p->red[combo1_index] = dt_bauhaus_slider_get(slider);
  }
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void green_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_channelmixer_params_t *p = (dt_iop_channelmixer_params_t *)self->params;
  dt_iop_channelmixer_gui_data_t *g = (dt_iop_channelmixer_gui_data_t *)self->gui_data;
  int combo1_index = dt_bauhaus_combobox_get(g->combo1);
  if(combo1_index >= 0)
  {
    p->green[combo1_index] = dt_bauhaus_slider_get(slider);
  }
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}


static void blue_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_channelmixer_params_t *p = (dt_iop_channelmixer_params_t *)self->params;
  dt_iop_channelmixer_gui_data_t *g = (dt_iop_channelmixer_gui_data_t *)self->gui_data;
  int combo1_index = dt_bauhaus_combobox_get(g->combo1);
  if(combo1_index >= 0)
  {
    p->blue[combo1_index] = dt_bauhaus_slider_get(slider);
  }
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void output_callback(GtkComboBox *combo, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_channelmixer_params_t *p = (dt_iop_channelmixer_params_t *)self->params;
  dt_iop_channelmixer_gui_data_t *g = (dt_iop_channelmixer_gui_data_t *)self->gui_data;

  // p->output_channel= gtk_combo_box_get_active(combo);
  int combo1_index = dt_bauhaus_combobox_get(g->combo1);
  if(combo1_index >= 0)
  {
    dt_bauhaus_slider_set(g->scale1, p->red[combo1_index]);
    dt_bauhaus_slider_set(g->scale2, p->green[combo1_index]);
    dt_bauhaus_slider_set(g->scale3, p->blue[combo1_index]);
  }
  // dt_dev_add_history_item(darktable.develop, self, TRUE);
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_channelmixer_params_t *p = (dt_iop_channelmixer_params_t *)p1;
#ifdef HAVE_GEGL
  fprintf(stderr, "[channel mixer] TODO: implement gegl version!\n");
// pull in new params to gegl
#else
  dt_iop_channelmixer_data_t *d = (dt_iop_channelmixer_data_t *)piece->data;
  // d->output_channel= p->output_channel;
  for(int i = 0; i < CHANNEL_SIZE; i++)
  {
    d->red[i] = p->red[i];
    d->blue[i] = p->blue[i];
    d->green[i] = p->green[i];
  }
#endif
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
#ifdef HAVE_GEGL
  // create part of the gegl pipeline
  piece->data = NULL;
#else
  piece->data = calloc(1, sizeof(dt_iop_channelmixer_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
#endif
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
#ifdef HAVE_GEGL
  // clean up everything again.
  (void)gegl_node_remove_child(pipe->gegl, piece->input);
// no free necessary, no data is alloc'ed
#else
  free(piece->data);
  piece->data = NULL;
#endif
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_module_t *module = (dt_iop_module_t *)self;
  dt_iop_channelmixer_gui_data_t *g = (dt_iop_channelmixer_gui_data_t *)self->gui_data;
  dt_iop_channelmixer_params_t *p = (dt_iop_channelmixer_params_t *)module->params;
  // gtk_combo_box_set_active(g->combo1, p->output_channel);

  int combo1_index = dt_bauhaus_combobox_get(g->combo1);
  if(combo1_index >= 0)
  {
    dt_bauhaus_slider_set(g->scale1, p->red[combo1_index]);
    dt_bauhaus_slider_set(g->scale2, p->green[combo1_index]);
    dt_bauhaus_slider_set(g->scale3, p->blue[combo1_index]);
  }
}

void init(dt_iop_module_t *module)
{
  module->params = malloc(sizeof(dt_iop_channelmixer_params_t));
  module->default_params = malloc(sizeof(dt_iop_channelmixer_params_t));
  module->default_enabled = 0;
  module->priority = 833; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_channelmixer_params_t);
  module->gui_data = NULL;
  dt_iop_channelmixer_params_t tmp = (dt_iop_channelmixer_params_t){ { 0, 0, 0, 1, 0, 0, 0 },
                                                                     { 0, 0, 0, 0, 1, 0, 0 },
                                                                     { 0, 0, 0, 0, 0, 1, 0 } };
  memcpy(module->params, &tmp, sizeof(dt_iop_channelmixer_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_channelmixer_params_t));
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
  self->gui_data = malloc(sizeof(dt_iop_channelmixer_gui_data_t));
  dt_iop_channelmixer_gui_data_t *g = (dt_iop_channelmixer_gui_data_t *)self->gui_data;
  dt_iop_channelmixer_params_t *p = (dt_iop_channelmixer_params_t *)self->params;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  /* output */
  g->combo1 = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->combo1, NULL, _("destination"));
  dt_bauhaus_combobox_add(g->combo1, _("hue"));
  dt_bauhaus_combobox_add(g->combo1, _("saturation"));
  dt_bauhaus_combobox_add(g->combo1, _("lightness"));
  dt_bauhaus_combobox_add(g->combo1, _("red"));
  dt_bauhaus_combobox_add(g->combo1, _("green"));
  dt_bauhaus_combobox_add(g->combo1, _("blue"));
  dt_bauhaus_combobox_add(g->combo1, C_("channelmixer", "gray"));
  dt_bauhaus_combobox_set(g->combo1, CHANNEL_RED);

  g_signal_connect(G_OBJECT(g->combo1), "value-changed", G_CALLBACK(output_callback), self);

  /* red */
  g->scale1 = dt_bauhaus_slider_new_with_range(self, -2.0, 2.0, 0.005, p->red[CHANNEL_RED], 3);
  g_object_set(G_OBJECT(g->scale1), "tooltip-text", _("amount of red channel in the output channel"),
               (char *)NULL);
  dt_bauhaus_widget_set_label(g->scale1, NULL, _("red"));
  g_signal_connect(G_OBJECT(g->scale1), "value-changed", G_CALLBACK(red_callback), self);

  /* green */
  g->scale2 = dt_bauhaus_slider_new_with_range(self, -2.0, 2.0, 0.005, p->green[CHANNEL_RED], 3);
  g_object_set(G_OBJECT(g->scale2), "tooltip-text", _("amount of green channel in the output channel"),
               (char *)NULL);
  dt_bauhaus_widget_set_label(g->scale2, NULL, _("green"));
  g_signal_connect(G_OBJECT(g->scale2), "value-changed", G_CALLBACK(green_callback), self);

  /* blue */
  g->scale3 = dt_bauhaus_slider_new_with_range(self, -2.0, 2.0, 0.005, p->blue[CHANNEL_RED], 3);
  g_object_set(g->scale3, "tooltip-text", _("amount of blue channel in the output channel"), (char *)NULL);
  dt_bauhaus_widget_set_label(g->scale3, NULL, _("blue"));
  g_signal_connect(G_OBJECT(g->scale3), "value-changed", G_CALLBACK(blue_callback), self);


  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->combo1), TRUE, TRUE, 0);

  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->scale1), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->scale2), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->scale3), TRUE, TRUE, 0);
}

void init_presets(dt_iop_module_so_t *self)
{
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "begin", NULL, NULL, NULL);

  dt_gui_presets_add_generic(_("swap R and B"), self->op, self->version(),
                             &(dt_iop_channelmixer_params_t){ { 0, 0, 0, 0, 0, 1, 0 },
                                                              { 0, 0, 0, 0, 1, 0, 0 },
                                                              { 0, 0, 0, 1, 0, 0, 0 } },
                             sizeof(dt_iop_channelmixer_params_t), 1);
  dt_gui_presets_add_generic(_("swap G and B"), self->op, self->version(),
                             &(dt_iop_channelmixer_params_t){ { 0, 0, 0, 1, 0, 0, 0 },
                                                              { 0, 0, 0, 0, 0, 1, 0 },
                                                              { 0, 0, 0, 0, 1, 0, 0 } },
                             sizeof(dt_iop_channelmixer_params_t), 1);
  dt_gui_presets_add_generic(_("color contrast boost"), self->op, self->version(),
                             &(dt_iop_channelmixer_params_t){ { 0, 0, 0.8, 1, 0, 0, 0 },
                                                              { 0, 0, 0.1, 0, 1, 0, 0 },
                                                              { 0, 0, 0.1, 0, 0, 1, 0 } },
                             sizeof(dt_iop_channelmixer_params_t), 1);
  dt_gui_presets_add_generic(_("color details boost"), self->op, self->version(),
                             &(dt_iop_channelmixer_params_t){ { 0, 0, 0.1, 1, 0, 0, 0 },
                                                              { 0, 0, 0.8, 0, 1, 0, 0 },
                                                              { 0, 0, 0.1, 0, 0, 1, 0 } },
                             sizeof(dt_iop_channelmixer_params_t), 1);
  dt_gui_presets_add_generic(_("color artifacts boost"), self->op, self->version(),
                             &(dt_iop_channelmixer_params_t){ { 0, 0, 0.1, 1, 0, 0, 0 },
                                                              { 0, 0, 0.1, 0, 1, 0, 0 },
                                                              { 0, 0, 0.800, 0, 0, 1, 0 } },
                             sizeof(dt_iop_channelmixer_params_t), 1);
  dt_gui_presets_add_generic(_("B/W"), self->op, self->version(),
                             &(dt_iop_channelmixer_params_t){ { 0, 0, 0, 1, 0, 0, 0.21 },
                                                              { 0, 0, 0, 0, 1, 0, 0.72 },
                                                              { 0, 0, 0, 0, 0, 1, 0.07 } },
                             sizeof(dt_iop_channelmixer_params_t), 1);
  dt_gui_presets_add_generic(_("B/W artifacts boost"), self->op, self->version(),
                             &(dt_iop_channelmixer_params_t){ { 0, 0, 0, 1, 0, 0, -0.275 },
                                                              { 0, 0, 0, 0, 1, 0, -0.275 },
                                                              { 0, 0, 0, 0, 0, 1, 1.275 } },
                             sizeof(dt_iop_channelmixer_params_t), 1);
  dt_gui_presets_add_generic(_("B/W smooth skin"), self->op, self->version(),
                             &(dt_iop_channelmixer_params_t){ { 0, 0, 0, 1, 0, 0, 1.0 },
                                                              { 0, 0, 0, 0, 0, 1, 0.325 },
                                                              { 0, 0, 0, 0, 0, 0, -0.4 } },
                             sizeof(dt_iop_channelmixer_params_t), 1);
  dt_gui_presets_add_generic(_("B/W blue artifacts reduce"), self->op, self->version(),
                             &(dt_iop_channelmixer_params_t){ { 0, 0, 0, 0, 0, 0, 0.4 },
                                                              { 0, 0, 0, 0, 0, 0, 0.750 },
                                                              { 0, 0, 0, 0, 0, 0, -0.15 } },
                             sizeof(dt_iop_channelmixer_params_t), 1);
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "commit", NULL, NULL, NULL);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
