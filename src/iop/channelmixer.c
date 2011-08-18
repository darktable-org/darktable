/*
  This file is part of darktable,
  copyright (c) 2010 Henrik Andersson.

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
#include "common/colorspaces.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "control/control.h"
#include "common/debug.h"
#include "dtgtk/slider.h"
#include "dtgtk/label.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include <gtk/gtk.h>
#include <inttypes.h>

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

#define CLIP(x) ((x<0)?0.0:(x>1.0)?1.0:x)
DT_MODULE(1)

typedef  enum _channelmixer_output_t
{
  /** mixes into hue channel */
  CHANNEL_HUE=0,
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
}
dt_iop_channelmixer_params_t;

typedef struct dt_iop_channelmixer_gui_data_t
{
  GtkVBox   *vbox;
  GtkComboBox *combo1;                                           // Output channel
  GtkDarktableLabel *dtlabel1,*dtlabel2;                      // output channel, source channels
  GtkLabel  *label1,*label2,*label3;	 	                          // red, green, blue
  GtkDarktableSlider *scale1,*scale2,*scale3;      	      // red, green, blue
}
dt_iop_channelmixer_gui_data_t;

typedef struct dt_iop_channelmixer_data_t
{
  //_channelmixer_output_t output_channel;
  float red[CHANNEL_SIZE];
  float green[CHANNEL_SIZE];
  float blue[CHANNEL_SIZE];
}
dt_iop_channelmixer_data_t;


const char *name()
{
  return _("channel mixer");
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING;
}

int
groups ()
{
  return IOP_GROUP_COLOR;
}
void init_key_accels(dt_iop_module_so_t *self)
{
//  dtgtk_slider_init_accel(darktable.control->accels_darkroom,"<Darktable>/darkroom/plugins/channelmixer/red");
//  dtgtk_slider_init_accel(darktable.control->accels_darkroom,"<Darktable>/darkroom/plugins/channelmixer/green");
//  dtgtk_slider_init_accel(darktable.control->accels_darkroom,"<Darktable>/darkroom/plugins/channelmixer/blue");
}

void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *ivoid, void *ovoid, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  const dt_iop_channelmixer_data_t *data = (dt_iop_channelmixer_data_t *)piece->data;
  const gboolean gray_mix_mode = ( data->red[CHANNEL_GRAY] !=0.0 &&  data->green[CHANNEL_GRAY] !=0.0 &&  data->blue[CHANNEL_GRAY] !=0.0)?TRUE:FALSE;
  const int ch = piece->colors;

#ifdef _OPENMP
  #pragma omp parallel for default(none) shared(ivoid, ovoid, roi_in, roi_out, data) schedule(static)
#endif
  for(int j=0; j<roi_out->height; j++)
  {
    const float *in = ((float *)ivoid) + ch*j*roi_out->width;
    float *out = ((float *)ovoid) + ch*j*roi_out->width;
    for(int i=0; i<roi_out->width; i++)
    {
      float h,s,l, hmix,smix,lmix,rmix,gmix,bmix,graymix;
      // Calculate the HSL mix
      hmix = CLIP( in[0] * data->red[CHANNEL_HUE] )+( in[1] * data->green[CHANNEL_HUE])+( in[2] * data->blue[CHANNEL_HUE] );
      smix = CLIP( in[0] * data->red[CHANNEL_SATURATION] )+( in[1] * data->green[CHANNEL_SATURATION])+( in[2] * data->blue[CHANNEL_SATURATION] );
      lmix = CLIP( in[0] * data->red[CHANNEL_LIGHTNESS] )+( in[1] * data->green[CHANNEL_LIGHTNESS])+( in[2] * data->blue[CHANNEL_LIGHTNESS] );

      // If HSL mix is used apply to out[]
      if( hmix != 0.0 || smix != 0.0 || lmix != 0.0 )
      {
        // mix into HSL output channels
        rgb2hsl(in,&h,&s,&l);
        h = (hmix != 0.0 )  ? hmix : h;
        s = (smix != 0.0 )  ? smix : s;
        l = (lmix != 0.0 )  ? lmix : l;
        hsl2rgb(out,h,s,l);
      }
      else   // no HSL copt in[] to out[]
        for(int i=0; i<3; i++) out[i]=in[i];

      // Calculate graymix and RGB mix
      graymix = CLIP(( out[0] * data->red[CHANNEL_GRAY] )+( out[1] * data->green[CHANNEL_GRAY])+( out[2] * data->blue[CHANNEL_GRAY] ));

      rmix = CLIP(( out[0] * data->red[CHANNEL_RED] )+( out[1] * data->green[CHANNEL_RED])+( out[2] * data->blue[CHANNEL_RED] ));
      gmix = CLIP(( out[0] * data->red[CHANNEL_GREEN] )+( out[1] * data->green[CHANNEL_GREEN])+( out[2] * data->blue[CHANNEL_GREEN] ));
      bmix = CLIP(( out[0] * data->red[CHANNEL_BLUE] )+( out[1] * data->green[CHANNEL_BLUE])+( out[2] * data->blue[CHANNEL_BLUE] ));


      if (gray_mix_mode)  // Graymix is used...
        out[0] = out[1] = out[2] = graymix;
      else   // RGB mix is used...
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
}

static void
red_callback(GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_channelmixer_params_t *p = (dt_iop_channelmixer_params_t *)self->params;
  dt_iop_channelmixer_gui_data_t *g = (dt_iop_channelmixer_gui_data_t *)self->gui_data;
  p->red[ gtk_combo_box_get_active( g->combo1 ) ]= dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void
green_callback(GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_channelmixer_params_t *p = (dt_iop_channelmixer_params_t *)self->params;
  dt_iop_channelmixer_gui_data_t *g = (dt_iop_channelmixer_gui_data_t *)self->gui_data;
  p->green[ gtk_combo_box_get_active( g->combo1 ) ]= dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}


static void
blue_callback(GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_channelmixer_params_t *p = (dt_iop_channelmixer_params_t *)self->params;
  dt_iop_channelmixer_gui_data_t *g = (dt_iop_channelmixer_gui_data_t *)self->gui_data;
  p->blue[ gtk_combo_box_get_active( g->combo1 ) ]= dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void
output_callback(GtkComboBox *combo, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_channelmixer_params_t *p = (dt_iop_channelmixer_params_t *)self->params;
  dt_iop_channelmixer_gui_data_t *g = (dt_iop_channelmixer_gui_data_t *)self->gui_data;

  // p->output_channel= gtk_combo_box_get_active(combo);
  dtgtk_slider_set_value( g->scale1, p->red[ gtk_combo_box_get_active( g->combo1 ) ] );
  dtgtk_slider_set_value( g->scale2, p->green[ gtk_combo_box_get_active( g->combo1 ) ] );
  dtgtk_slider_set_value( g->scale3, p->blue[ gtk_combo_box_get_active( g->combo1 ) ] );
  //dt_dev_add_history_item(darktable.develop, self, TRUE);
}

void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_channelmixer_params_t *p = (dt_iop_channelmixer_params_t *)p1;
#ifdef HAVE_GEGL
  fprintf(stderr, "[channel mixer] TODO: implement gegl version!\n");
  // pull in new params to gegl
#else
  dt_iop_channelmixer_data_t *d = (dt_iop_channelmixer_data_t *)piece->data;
// d->output_channel= p->output_channel;
  for( int i=0; i<CHANNEL_SIZE; i++)
  {
    d->red[i]= p->red[i];
    d->blue[i]= p->blue[i];
    d->green[i]= p->green[i];
  }
#endif
}

void init_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
#ifdef HAVE_GEGL
  // create part of the gegl pipeline
  piece->data = NULL;
#else
  piece->data = malloc(sizeof(dt_iop_channelmixer_data_t));
  memset(piece->data,0,sizeof(dt_iop_channelmixer_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
#endif
}

void cleanup_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
#ifdef HAVE_GEGL
  // clean up everything again.
  (void)gegl_node_remove_child(pipe->gegl, piece->input);
  // no free necessary, no data is alloc'ed
#else
  free(piece->data);
#endif
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_module_t *module = (dt_iop_module_t *)self;
  dt_iop_channelmixer_gui_data_t *g = (dt_iop_channelmixer_gui_data_t *)self->gui_data;
  dt_iop_channelmixer_params_t *p = (dt_iop_channelmixer_params_t *)module->params;
// gtk_combo_box_set_active(g->combo1, p->output_channel);
  dtgtk_slider_set_value(g->scale1, p->red[ gtk_combo_box_get_active( g->combo1 ) ] );
  dtgtk_slider_set_value(g->scale2, p->green[ gtk_combo_box_get_active( g->combo1 ) ] );
  dtgtk_slider_set_value(g->scale3, p->blue[ gtk_combo_box_get_active( g->combo1 ) ] );
}

void init(dt_iop_module_t *module)
{
  module->params = malloc(sizeof(dt_iop_channelmixer_params_t));
  module->default_params = malloc(sizeof(dt_iop_channelmixer_params_t));
  module->default_enabled = 0;
  module->priority = 804; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_channelmixer_params_t);
  module->gui_data = NULL;
  dt_iop_channelmixer_params_t tmp = (dt_iop_channelmixer_params_t)
  {
    {
      0,0,0,1,0,0,0
    }, {0,0,0,0,1,0,0}, {0,0,0,0,0,1,0}
  };
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

  self->widget = GTK_WIDGET(gtk_vbox_new(FALSE, DT_GUI_IOP_MODULE_CONTROL_SPACING));

  g->dtlabel1 = DTGTK_LABEL( dtgtk_label_new(_("output channel"), DARKTABLE_LABEL_TAB | DARKTABLE_LABEL_ALIGN_RIGHT) );
  g->combo1 = GTK_COMBO_BOX( gtk_combo_box_new_text() );
  gtk_combo_box_append_text(g->combo1,_("hue"));
  gtk_combo_box_append_text(g->combo1,_("saturation"));
  gtk_combo_box_append_text(g->combo1,_("lightness"));
  gtk_combo_box_append_text(g->combo1,_("red"));
  gtk_combo_box_append_text(g->combo1,_("green"));
  gtk_combo_box_append_text(g->combo1,_("blue"));
  gtk_combo_box_append_text(g->combo1,_("gray"));
  gtk_combo_box_set_active(g->combo1, CHANNEL_RED );
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->dtlabel1), TRUE, TRUE, 5);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->combo1), TRUE, TRUE, 5);

  g->dtlabel2 = DTGTK_LABEL( dtgtk_label_new(_("source channels"), DARKTABLE_LABEL_TAB | DARKTABLE_LABEL_ALIGN_RIGHT) );
  GtkBox *hbox = GTK_BOX( gtk_hbox_new(FALSE, 0));
  g->vbox = GTK_VBOX(gtk_vbox_new(FALSE, DT_GUI_IOP_MODULE_CONTROL_SPACING));
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(g->vbox), TRUE, TRUE, 5);

  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->dtlabel2), TRUE, TRUE, 5);

  g->scale1 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_VALUE,-2.0, 2.0, 0.005, p->red[CHANNEL_RED] , 3));
  g_object_set (GTK_OBJECT(g->scale1), "tooltip-text", _("amount of red channel in the output channel"), (char *)NULL);
  dtgtk_slider_set_label(g->scale1,_("red"));
//  dtgtk_slider_set_accel(g->scale1,darktable.control->accels_darkroom,"<Darktable>/darkroom/plugins/channelmixer/red");
  g->scale2 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_VALUE,-2.0, 2.0, 0.005, p->green[CHANNEL_RED] , 3));
  g_object_set (GTK_OBJECT(g->scale2), "tooltip-text", _("amount of green channel in the output channel"), (char *)NULL);
  dtgtk_slider_set_label(g->scale2,_("green"));
//  dtgtk_slider_set_accel(g->scale2,darktable.control->accels_darkroom,"<Darktable>/darkroom/plugins/channelmixer/green");
  g->scale3 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_VALUE,-2.0, 2.0, 0.005, p->blue[CHANNEL_RED] , 3));
  g_object_set (GTK_OBJECT(g->scale3), "tooltip-text", _("amount of blue channel in the output channel"), (char *)NULL);
  dtgtk_slider_set_label(g->scale3,_("blue"));
//  dtgtk_slider_set_accel(g->scale3,darktable.control->accels_darkroom,"<Darktable>/darkroom/plugins/channelmixer/blue");

  gtk_box_pack_start(GTK_BOX(g->vbox), GTK_WIDGET(g->scale1), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox), GTK_WIDGET(g->scale2), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox), GTK_WIDGET(g->scale3), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hbox), TRUE, TRUE, 5);

  g_signal_connect (G_OBJECT (g->scale1), "value-changed",
                    G_CALLBACK (red_callback), self);
  g_signal_connect (G_OBJECT (g->scale2), "value-changed",
                    G_CALLBACK (green_callback), self);
  g_signal_connect (G_OBJECT (g->scale3), "value-changed",
                    G_CALLBACK (blue_callback), self);
  g_signal_connect (G_OBJECT (g->combo1), "changed",
                    G_CALLBACK (output_callback), self);
}

void init_presets (dt_iop_module_t *self)
{
  DT_DEBUG_SQLITE3_EXEC(darktable.db, "begin", NULL, NULL, NULL);

  dt_gui_presets_add_generic(_("swap R and B"), self->op, &(dt_iop_channelmixer_params_t)
  {
    {
      0,0,0,0,0,1,0
    }, {0,0,0,0,1,0,0}, {0,0,0,1,0,0,0}
  } , sizeof(dt_iop_channelmixer_params_t), 1);
  dt_gui_presets_add_generic(_("swap G and B"), self->op, &(dt_iop_channelmixer_params_t)
  {
    {
      0,0,0,1,0,0,0
    }, {0,0,0,0,0,1,0}, {0,0,0,0,1,0,0}
  } , sizeof(dt_iop_channelmixer_params_t), 1);
  dt_gui_presets_add_generic(_("color contrast boost"), self->op, &(dt_iop_channelmixer_params_t)
  {
    {
      0,0,0.8,1,0,0,0
    }, {0,0,0.1,0,1,0,0}, {0,0,0.1,0,0,1,0}
  } , sizeof(dt_iop_channelmixer_params_t), 1);
  dt_gui_presets_add_generic(_("color details boost"), self->op, &(dt_iop_channelmixer_params_t)
  {
    {
      0,0,0.1,1,0,0,0
    }, {0,0,0.8,0,1,0,0}, {0,0,0.1,0,0,1,0}
  } , sizeof(dt_iop_channelmixer_params_t), 1);
  dt_gui_presets_add_generic(_("color artifacts boost"), self->op, &(dt_iop_channelmixer_params_t)
  {
    {
      0,0,0.1,1,0,0,0
    }, {0,0,0.1,0,1,0,0}, {0,0,0.800,0,0,1,0}
  } , sizeof(dt_iop_channelmixer_params_t), 1);
  dt_gui_presets_add_generic(_("b/w"), self->op, &(dt_iop_channelmixer_params_t)
  {
    {
      0,0,0,1,0,0,0.21
    }, {0,0,0,0,1,0,0.72}, {0,0,0,0,0,1,0.07}
  } , sizeof(dt_iop_channelmixer_params_t), 1);
  dt_gui_presets_add_generic(_("b/w artifacts boost"), self->op, &(dt_iop_channelmixer_params_t)
  {
    {
      0,0,0,1,0,0,-0.275
    }, {0,0,0,0,1,0,-0.275}, {0,0,0,0,0,1,1.275}
  } , sizeof(dt_iop_channelmixer_params_t), 1);
  dt_gui_presets_add_generic(_("b/w smooth skin"), self->op, &(dt_iop_channelmixer_params_t)
  {
    {
      0,0,0,1,0,0,1.0
    }, {0,0,0,0,0,1,0.325}, {0,0,0,0,0,0,-0.4}
  } , sizeof(dt_iop_channelmixer_params_t), 1);
  dt_gui_presets_add_generic(_("b/w blue artifacts reduce"), self->op, &(dt_iop_channelmixer_params_t)
  {
    {
      0,0,0,0,0,0,0.4
    }, {0,0,0,0,0,0,0.750}, {0,0,0,0,0,0,-0.15}
  } , sizeof(dt_iop_channelmixer_params_t), 1);
  DT_DEBUG_SQLITE3_EXEC(darktable.db, "commit", NULL, NULL, NULL);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
