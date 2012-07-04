/*
    This file is part of darktable,
    copyright (c) 2009--2012 johannes hanika.
    copyright (c) 2011 Henrik Andersson.
    copyright (c) 2012 Ulrich Pegelow

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
#include "bauhaus/bauhaus.h"
#include "common/opencl.h"
#include "common/dtpthread.h"
#include "common/debug.h"
#include "control/control.h"
#include "develop/imageop.h"
#include "develop/develop.h"
#include "develop/blend.h"
#include "develop/tiling.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "dtgtk/button.h"
#include "dtgtk/icon.h"
#include "dtgtk/tristatebutton.h"
#include "dtgtk/slider.h"
#include "dtgtk/tristatebutton.h"
#include "dtgtk/gradientslider.h"
#include "dtgtk/label.h"

#include <strings.h>
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <gmodule.h>
#include <xmmintrin.h>


#define CLAMP_RANGE(x,y,z)      (CLAMP(x,y,z))

typedef enum _iop_gui_blendif_channel_t
{
  ch_L     = 0,
  ch_a     = 1,
  ch_b     = 2,
  ch_gray  = 0,
  ch_red   = 1,
  ch_green = 2,
  ch_blue  = 3,
  ch_max   = 4
}
_iop_gui_blendif_channel_t;


typedef struct _iop_gui_blendif_colorstop_t
{
  float stoppoint;
  GdkColor color;
}
_iop_gui_blendif_colorstop_t;


static void
_blendif_scale(dt_iop_colorspace_type_t cst, const float *in, float *out)
{
  switch(cst)
  {
    case iop_cs_Lab:
      out[0] = CLAMP_RANGE(in[0] / 100.0f, 0.0f, 1.0f);
      out[1] = CLAMP_RANGE((in[1] + 128.0f)/256.0f, 0.0f, 1.0f);
      out[2] = CLAMP_RANGE((in[2] + 128.0f)/256.0f, 0.0f, 1.0f);
      out[3] = -1.0f;
    break;
    case iop_cs_rgb:
      out[0] = CLAMP_RANGE(0.3f*in[0] + 0.59f*in[1] + 0.11f*in[2], 0.0f, 1.0f);
      out[1] = CLAMP_RANGE(in[0], 0.0f, 1.0f);
      out[2] = CLAMP_RANGE(in[1], 0.0f, 1.0f);
      out[3] = CLAMP_RANGE(in[2], 0.0f, 1.0f);
    break;
    default:
      out[0] = out[1] = out[2] = out[3] = -1.0f;
  }
}

static void
_blendif_cook(dt_iop_colorspace_type_t cst, const float *in, float *out)
{
  switch(cst)
  {
    case iop_cs_Lab:
      out[0] = in[0];
      out[1] = in[1];
      out[2] = in[2];
      out[3] = -1.0f;
    break;
    case iop_cs_rgb:
      out[0] = (0.3f*in[0] + 0.59f*in[1] + 0.11f*in[2])*255.0f;
      out[1] = in[0]*255.0f;
      out[2] = in[1]*255.0f;
      out[3] = in[2]*255.0f;
    break;
    default:
      out[0] = out[1] = out[2] = out[3] = -1.0f;
  }
}



static void
_blendif_scale_print_L(float value, char *string, int n)
{
  snprintf(string, n, "%-4.0f", value*100.0f);
}

static void
_blendif_scale_print_ab(float value, char *string, int n)
{
  snprintf(string, n, "%-4.0f", value*256.0f-128.0f);
}

static void
_blendif_scale_print_rgb(float value, char *string, int n)
{
  snprintf(string, n, "%-4.0f", value*255.0f);
}


static void
_blendop_mode_callback (GtkWidget *combo, dt_iop_gui_blend_data_t *data)
{
  data->module->blend_params->mode = data->modes[dt_bauhaus_combobox_get(data->blend_modes_combo)].mode;
  if(data->module->blend_params->mode != DEVELOP_BLEND_DISABLED)
  {
    gtk_widget_show(data->opacity_slider);
    if(data->blendif_support)
    {
      gtk_widget_show(data->blendif_enable);
      if(dt_bauhaus_combobox_get(data->blendif_enable))
        gtk_widget_show(GTK_WIDGET(data->blendif_box));
    }
  }
  else
  {
    gtk_widget_hide(data->opacity_slider);
    if(data->blendif_support)
    {
      gtk_widget_hide(GTK_WIDGET(data->blendif_enable));
      gtk_widget_hide(GTK_WIDGET(data->blendif_box));
    }
  }
  dt_dev_add_history_item(darktable.develop, data->module, TRUE);
}

static void
_blendop_opacity_callback (GtkWidget *slider, dt_iop_gui_blend_data_t *data)
{
  data->module->blend_params->opacity = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, data->module, TRUE);
}

static void
_blendop_blendif_callback(GtkWidget *b, dt_iop_gui_blend_data_t *data)
{
  if(dt_bauhaus_combobox_get(b))
  {
    data->module->blend_params->blendif |= (1<<DEVELOP_BLENDIF_active);
    gtk_widget_show(GTK_WIDGET(data->blendif_box));
  }
  else
  {
    gtk_widget_hide(GTK_WIDGET(data->blendif_box));
    data->module->blend_params->blendif &= ~(1<<DEVELOP_BLENDIF_active);
  }
  dt_dev_add_history_item(darktable.develop, data->module, TRUE);
}


static void
_blendop_blendif_upper_callback (GtkDarktableGradientSlider *slider, dt_iop_gui_blend_data_t *data)
{
  if(darktable.gui->reset) return;
  dt_develop_blend_params_t *bp = data->module->blend_params;

  int tab = data->tab;
  int ch = data->channels[tab][1];

  float *parameters = &(bp->blendif_parameters[4*ch]);

  for(int k=0; k < 4; k++)
    parameters[k] = dtgtk_gradient_slider_multivalue_get_value(slider, k);

  for(int k=0; k < 4 ; k++)
  {
    char text[256];
    (data->scale_print[tab])(parameters[k], text, 256);
    gtk_label_set_text(data->upper_label[k], text);
  }

  /** de-activate processing of this channel if maximum span is selected */
  if(parameters[1] == 0.0f && parameters[2] == 1.0f)
    bp->blendif &= ~(1<<ch);
  else
    bp->blendif |= (1<<ch);

  dt_dev_add_history_item(darktable.develop, data->module, TRUE);
}


static void
_blendop_blendif_lower_callback (GtkDarktableGradientSlider *slider, dt_iop_gui_blend_data_t *data)
{
  if(darktable.gui->reset) return;
  dt_develop_blend_params_t *bp = data->module->blend_params;

  int tab = data->tab;
  int ch = data->channels[tab][0];

  float *parameters = &(bp->blendif_parameters[4*ch]);
  
  for(int k=0; k < 4; k++)
    parameters[k] = dtgtk_gradient_slider_multivalue_get_value(slider, k);

  for(int k=0; k < 4 ; k++)
  {
    char text[256];
    (data->scale_print[tab])(parameters[k], text, 256);
    gtk_label_set_text(data->lower_label[k], text);
  }

  /** de-activate processing of this channel if maximum span is selected */
  if(parameters[1] == 0.0f && parameters[2] == 1.0f)
    bp->blendif &= ~(1<<ch);
  else
    bp->blendif |= (1<<ch);

  dt_dev_add_history_item(darktable.develop, data->module, TRUE);
}



static void
_blendop_blendif_tab_switch(GtkNotebook *notebook, GtkNotebookPage *notebook_page, guint page_num,dt_iop_gui_blend_data_t *data)
{
  data->tab = page_num;
  dt_iop_gui_update_blendif(data->module);
}


static void
_blendop_blendif_pick_toggled(GtkToggleButton *togglebutton, dt_iop_module_t *module)
{
  module->request_color_pick = gtk_toggle_button_get_active(togglebutton);
  if(darktable.gui->reset) return;

  
  /* set the area sample size*/
  if (module->request_color_pick)
    dt_lib_colorpicker_set_point(darktable.lib, 0.5, 0.5);
  
  if(module->off) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(module->off), 1);
  dt_iop_request_focus(module);
}

static void
_blendop_blendif_showmask_toggled(GtkToggleButton *togglebutton, dt_iop_module_t *module)
{
  dt_develop_blend_params_t *bp = module->blend_params;

  module->request_mask_display = gtk_toggle_button_get_active(togglebutton);
  if(darktable.gui->reset) return;

  if(module->off) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(module->off), 1);
  dt_iop_request_focus(module);

  // hack! in order to force instant redraw we toggle an unused bit in blendif params
  // TODO: need to find a better way to trigger redraw
  bp->blendif ^= (1<<DEVELOP_BLENDIF_unused);
  dt_dev_add_history_item(darktable.develop, module, TRUE);
}


static gboolean
_blendop_blendif_expose(GtkWidget *widget, GdkEventExpose *event, dt_iop_module_t *module)
{
  if(darktable.gui->reset) return FALSE;

  dt_iop_gui_blend_data_t *data = module->blend_data;

  float picker_mean[4], picker_min[4], picker_max[4];
  float cooked[4];
  float *raw_mean, *raw_min, *raw_max;
  char text[256];
  GtkLabel *label;

  if(widget == GTK_WIDGET(data->lower_slider))
  {
    raw_mean = module->picked_color;
    raw_min = module->picked_color_min;
    raw_max = module->picked_color_max;
    label = data->lower_picker_label;
  }
  else
  {
    raw_mean = module->picked_output_color;
    raw_min = module->picked_output_color_min;
    raw_max = module->picked_output_color_max;
    label = data->upper_picker_label;
  }

  darktable.gui->reset = 1;
  if(module->request_color_pick)
  {
    _blendif_scale(data->csp, raw_mean, picker_mean);
    _blendif_scale(data->csp, raw_min, picker_min);
    _blendif_scale(data->csp, raw_max, picker_max);
    _blendif_cook(data->csp, raw_mean, cooked);

    snprintf(text, 256, "(%.1f)", cooked[data->tab]);

    dtgtk_gradient_slider_multivalue_set_picker_meanminmax(DTGTK_GRADIENT_SLIDER(widget), picker_mean[data->tab], picker_min[data->tab], picker_max[data->tab]);
    gtk_label_set_text(label, text);
  }
  else
  {
    dtgtk_gradient_slider_multivalue_set_picker(DTGTK_GRADIENT_SLIDER(widget), -1.0f);
    gtk_label_set_text(label, "");
  }
  darktable.gui->reset = 0;

  return FALSE;
}


void
dt_iop_gui_update_blendif(dt_iop_module_t *module)
{
  dt_iop_gui_blend_data_t *data = module->blend_data;
  dt_develop_blend_params_t *bp = module->blend_params;
  dt_develop_blend_params_t *dp = module->default_blendop_params;

  if (!data->blendif_support) return;

  int tab = data->tab;
  int in_ch = data->channels[tab][0];
  int out_ch = data->channels[tab][1];

  float *iparameters = &(bp->blendif_parameters[4*in_ch]);
  float *oparameters = &(bp->blendif_parameters[4*out_ch]);
  float *idefaults = &(dp->blendif_parameters[4*in_ch]);
  float *odefaults = &(dp->blendif_parameters[4*out_ch]);
  char text[256];

  int reset = darktable.gui->reset;
  darktable.gui->reset = 1;

  for(int k=0; k < 4; k++)
  {
    dtgtk_gradient_slider_multivalue_set_value(data->lower_slider, iparameters[k], k);
    dtgtk_gradient_slider_multivalue_set_value(data->upper_slider, oparameters[k], k);
    dtgtk_gradient_slider_multivalue_set_resetvalue(data->lower_slider, idefaults[k], k);
    dtgtk_gradient_slider_multivalue_set_resetvalue(data->upper_slider, odefaults[k], k);
  }

  for(int k=0; k < 4 ; k++)
  {
    (data->scale_print[tab])(iparameters[k], text, 256);
    gtk_label_set_text(data->lower_label[k], text);
    (data->scale_print[tab])(oparameters[k], text, 256);
    gtk_label_set_text(data->upper_label[k], text);
  }

  dtgtk_gradient_slider_multivalue_set_stop(data->lower_slider, 0.0f, data->colors[tab][0]);
  dtgtk_gradient_slider_multivalue_set_stop(data->lower_slider, 0.5f, data->colors[tab][1]);
  dtgtk_gradient_slider_multivalue_set_stop(data->lower_slider, 1.0f, data->colors[tab][2]);

  dtgtk_gradient_slider_multivalue_set_stop(data->upper_slider, 0.0f, data->colors[tab][0]);
  dtgtk_gradient_slider_multivalue_set_stop(data->upper_slider, 0.5f, data->colors[tab][1]);
  dtgtk_gradient_slider_multivalue_set_stop(data->upper_slider, 1.0f, data->colors[tab][2]);

  dtgtk_gradient_slider_multivalue_set_increment(data->lower_slider, data->increments[tab]);
  dtgtk_gradient_slider_multivalue_set_increment(data->upper_slider, data->increments[tab]);

  darktable.gui->reset = reset;
}


/** get the sequence number (in combo box) of a blend mode */
int dt_iop_gui_blending_mode_seq(dt_iop_gui_blend_data_t *bd, int mode)
{
  int result = 0;
  for (int k=0; k < bd->number_modes; k++)
    if (bd->modes[k].mode == mode)
    {
      result = k;
      break;
    }

  return result;
}


void dt_iop_gui_init_blendif(GtkVBox *blendw, dt_iop_module_t *module)
{
  dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t*)module->blend_data;

  /* create and add blendif support if module supports it */
  if (bd->blendif_support)
  {
    int lightness=32768;
    char *Lab_labels[3] = { "  L  ", "  a  ", "  b  " };
    char *Lab_tooltips[3] = { _("sliders for L channel"), _("sliders for a channel"), _("sliders for b channel") };
    char *rgb_labels[4] = { _(" gray "), _(" red "), _(" green "), _(" blue ") };
    char *rgb_tooltips[4] = { _("sliders for gray value"), _("sliders for red channel"), _("sliders for green channel"), _("sliders for blue channel") };

    GdkColor Lab_colors[4][3] = { { /* L channel */
                                   (GdkColor){ 0,0,0,0 }, (GdkColor){ 0,lightness/2,lightness/2,lightness/2 }, (GdkColor){ 0,lightness,lightness,lightness }
                                  },
                                  { /* a channel */
                                   (GdkColor){ 0,0,0.34*lightness*2,0.27*lightness*2 }, (GdkColor){ 0,lightness,lightness,lightness }, (GdkColor){ 0,0.53*lightness*2,0.08*lightness*2,0.28*lightness*2 }
                                  },
                                  { /* b channel */
                                   (GdkColor){ 0,0,0.27*lightness*2,0.58*lightness*2 }, (GdkColor){ 0,lightness,lightness,lightness }, (GdkColor){ 0,0.81*lightness*2,0.66*lightness*2,0 }
                                  },
                                  { /* not used */
                                   (GdkColor){ 0,0,0,0 }, (GdkColor){ 0,0,0,0 }, (GdkColor){ 0,0,0,0 }
                                  } };                     

    GdkColor rgb_colors[4][3] = { { /* gray channel */
                                   (GdkColor){ 0,0,0,0 }, (GdkColor){ 0,lightness/2,lightness/2,lightness/2 }, (GdkColor){ 0,lightness,lightness,lightness }
                                  },
                                  { /* red channel */
                                   (GdkColor){ 0,0,0,0 }, (GdkColor){ 0,lightness/2,0,0 }, (GdkColor){ 0,lightness,0,0 }
                                  },
                                  { /* green channel */
                                   (GdkColor){ 0,0,0,0 }, (GdkColor){ 0,0,lightness/2,0 }, (GdkColor){ 0,0,lightness,0 }
                                  },
                                  { /* blue channel */
                                   (GdkColor){ 0,0,0,0 }, (GdkColor){ 0,0,0,lightness/2 }, (GdkColor){ 0,0,0,lightness }
                                  } };

    char *ttinput = _("adjustment based on input received by this module:\n* range between inner markers (upper filled triangles): blend fully\n* range outside of outer markers (lower open triangles): do not blend at all\n* range between adjacent inner/outer markers: blend gradually");

    char *ttoutput = _("adjustment based on unblended output of this module:\n* range between inner markers (upper filled triangles): blend fully\n* range outside of outer markers (lower open triangles): do not blend at all\n* range between adjacent inner/outer markers: blend gradually");

    bd->tab = 0;

    int maxchannels = 0;
    char **labels = NULL;
    char **tooltips = NULL;

    switch(bd->csp)
    {
      case iop_cs_Lab:
        maxchannels = 3;
        labels = Lab_labels;
        tooltips = Lab_tooltips;
        memcpy(bd->colors, Lab_colors, sizeof(rgb_colors));
        bd->scale_print[0] = _blendif_scale_print_L;
        bd->scale_print[1] = _blendif_scale_print_ab;
        bd->scale_print[2] = _blendif_scale_print_ab;
        bd->scale_print[3] = NULL;
        bd->increments[0] = 1.0f/100.0f;
        bd->increments[1] = 1.0f/256.0f;
        bd->increments[2] = 1.0f/256.0f;
        bd->increments[3] = 1.0f;
        bd->channels[0][0] = DEVELOP_BLENDIF_L_in;
        bd->channels[0][1] = DEVELOP_BLENDIF_L_out;
        bd->channels[1][0] = DEVELOP_BLENDIF_A_in;
        bd->channels[1][1] = DEVELOP_BLENDIF_A_out;
        bd->channels[2][0] = DEVELOP_BLENDIF_B_in;
        bd->channels[2][1] = DEVELOP_BLENDIF_B_out;
        break;
      case iop_cs_rgb:
        maxchannels = 4;
        labels = rgb_labels;
        tooltips = rgb_tooltips;
        memcpy(bd->colors, rgb_colors, sizeof(rgb_colors));
        bd->scale_print[0] = _blendif_scale_print_rgb;
        bd->scale_print[1] = _blendif_scale_print_rgb;
        bd->scale_print[2] = _blendif_scale_print_rgb;
        bd->scale_print[3] = _blendif_scale_print_rgb;
        bd->increments[0] = 1.0f/255.0f;
        bd->increments[1] = 1.0f/255.0f;
        bd->increments[2] = 1.0f/255.0f;
        bd->increments[3] = 1.0f/255.0f;
        bd->channels[0][0] = DEVELOP_BLENDIF_GRAY_in;
        bd->channels[0][1] = DEVELOP_BLENDIF_GRAY_out;
        bd->channels[1][0] = DEVELOP_BLENDIF_RED_in;
        bd->channels[1][1] = DEVELOP_BLENDIF_RED_out;
        bd->channels[2][0] = DEVELOP_BLENDIF_GREEN_in;
        bd->channels[2][1] = DEVELOP_BLENDIF_GREEN_out;
        bd->channels[3][0] = DEVELOP_BLENDIF_BLUE_in;
        bd->channels[3][1] = DEVELOP_BLENDIF_BLUE_out;
        break;
      default:
        assert(FALSE);		// blendif not supported for RAW, which is already catched upstream; we should not get here
    }


    bd->blendif_box = GTK_VBOX(gtk_vbox_new(FALSE,DT_GUI_IOP_MODULE_CONTROL_SPACING));
    GtkWidget *uplabel = gtk_hbox_new(FALSE,0);
    GtkWidget *lowlabel = gtk_hbox_new(FALSE,0);
    GtkWidget *notebook = gtk_hbox_new(FALSE,0);

    bd->blendif_enable = dt_bauhaus_combobox_new(module);
    dt_bauhaus_widget_set_label(bd->blendif_enable, _("blend"));
    dt_bauhaus_combobox_add(bd->blendif_enable, _("uniformly"));
    dt_bauhaus_combobox_add(bd->blendif_enable, _("only, if.."));

    bd->channel_tabs = GTK_NOTEBOOK(gtk_notebook_new());

    for(int ch=0; ch<maxchannels; ch++)
    {
      gtk_notebook_append_page(GTK_NOTEBOOK(bd->channel_tabs), GTK_WIDGET(gtk_hbox_new(FALSE,0)), gtk_label_new(labels[ch]));
      g_object_set(G_OBJECT(gtk_notebook_get_tab_label(bd->channel_tabs, gtk_notebook_get_nth_page(bd->channel_tabs, -1))), "tooltip-text", tooltips[ch], NULL);
    }

    gtk_widget_show_all(GTK_WIDGET(gtk_notebook_get_nth_page(bd->channel_tabs, bd->tab)));
    gtk_notebook_set_current_page(GTK_NOTEBOOK(bd->channel_tabs), bd->tab);
    g_object_set(G_OBJECT(bd->channel_tabs), "homogeneous", TRUE, (char *)NULL);

    GtkWidget *tb = dtgtk_togglebutton_new(dtgtk_cairo_paint_colorpicker, CPF_STYLE_FLAT);
    g_object_set(G_OBJECT(tb), "tooltip-text", _("pick gui color from image"), (char *)NULL);

    GtkWidget *sm = dtgtk_togglebutton_new(dtgtk_cairo_paint_showmask, CPF_STYLE_FLAT);
    g_object_set(G_OBJECT(sm), "tooltip-text", _("display mask"), (char *)NULL);

    gtk_box_pack_start(GTK_BOX(notebook), GTK_WIDGET(bd->channel_tabs), FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(notebook), GTK_WIDGET(tb), FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(notebook), GTK_WIDGET(sm), FALSE, FALSE, 0);

    bd->lower_slider = DTGTK_GRADIENT_SLIDER_MULTIVALUE(dtgtk_gradient_slider_multivalue_new(4));
    bd->upper_slider = DTGTK_GRADIENT_SLIDER_MULTIVALUE(dtgtk_gradient_slider_multivalue_new(4));

    dtgtk_gradient_slider_multivalue_set_marker(bd->lower_slider, GRADIENT_SLIDER_MARKER_LOWER_OPEN_BIG, 0);
    dtgtk_gradient_slider_multivalue_set_marker(bd->lower_slider, GRADIENT_SLIDER_MARKER_UPPER_FILLED_BIG, 1);
    dtgtk_gradient_slider_multivalue_set_marker(bd->lower_slider, GRADIENT_SLIDER_MARKER_UPPER_FILLED_BIG, 2);
    dtgtk_gradient_slider_multivalue_set_marker(bd->lower_slider, GRADIENT_SLIDER_MARKER_LOWER_OPEN_BIG, 3);

    dtgtk_gradient_slider_multivalue_set_marker(bd->upper_slider, GRADIENT_SLIDER_MARKER_LOWER_OPEN_BIG, 0);
    dtgtk_gradient_slider_multivalue_set_marker(bd->upper_slider, GRADIENT_SLIDER_MARKER_UPPER_FILLED_BIG, 1);
    dtgtk_gradient_slider_multivalue_set_marker(bd->upper_slider, GRADIENT_SLIDER_MARKER_UPPER_FILLED_BIG, 2);
    dtgtk_gradient_slider_multivalue_set_marker(bd->upper_slider, GRADIENT_SLIDER_MARKER_LOWER_OPEN_BIG, 3);

    dtgtk_gradient_slider_multivalue_set_stop(bd->lower_slider, 0.0f, bd->colors[bd->tab][0]);
    dtgtk_gradient_slider_multivalue_set_stop(bd->lower_slider, 0.5f, bd->colors[bd->tab][1]);
    dtgtk_gradient_slider_multivalue_set_stop(bd->lower_slider, 1.0f, bd->colors[bd->tab][2]);

    dtgtk_gradient_slider_multivalue_set_stop(bd->upper_slider, 0.0f, bd->colors[bd->tab][0]);
    dtgtk_gradient_slider_multivalue_set_stop(bd->upper_slider, 0.5f, bd->colors[bd->tab][1]);
    dtgtk_gradient_slider_multivalue_set_stop(bd->upper_slider, 1.0f, bd->colors[bd->tab][2]);

    GtkWidget *output = gtk_label_new(_("output"));
    bd->upper_picker_label = GTK_LABEL(gtk_label_new(""));
    gtk_box_pack_start(GTK_BOX(uplabel), GTK_WIDGET(output), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(uplabel), GTK_WIDGET(bd->upper_picker_label), TRUE, TRUE, 0);
    for(int k=0; k < 4 ; k++)
    {
      bd->upper_label[k] = GTK_LABEL(gtk_label_new(NULL));
      gtk_label_set_width_chars(bd->upper_label[k], 6);
      gtk_box_pack_start(GTK_BOX(uplabel), GTK_WIDGET(bd->upper_label[k]), FALSE, FALSE, 0);      
    }

    GtkWidget *input = gtk_label_new(_("input"));
    bd->lower_picker_label = GTK_LABEL(gtk_label_new(""));
    gtk_box_pack_start(GTK_BOX(lowlabel), GTK_WIDGET(input), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(lowlabel), GTK_WIDGET(bd->lower_picker_label), TRUE, TRUE, 0);
    for(int k=0; k < 4 ; k++)
    {
      bd->lower_label[k] = GTK_LABEL(gtk_label_new(NULL));
      gtk_label_set_width_chars(bd->lower_label[k], 6);
      gtk_box_pack_start(GTK_BOX(lowlabel), GTK_WIDGET(bd->lower_label[k]), FALSE, FALSE, 0);      
    }

    gtk_object_set(GTK_OBJECT(bd->blendif_enable), "tooltip-text", _("enable conditional blending"), (char *)NULL);
    gtk_object_set(GTK_OBJECT(bd->lower_slider), "tooltip-text", _("double click to reset"), (char *)NULL);
    gtk_object_set(GTK_OBJECT(bd->upper_slider), "tooltip-text", _("double click to reset"), (char *)NULL);
    gtk_object_set(GTK_OBJECT(output), "tooltip-text", ttoutput, (char *)NULL);
    gtk_object_set(GTK_OBJECT(input), "tooltip-text", ttinput, (char *)NULL);


    g_signal_connect (G_OBJECT (bd->lower_slider), "expose-event",
                      G_CALLBACK (_blendop_blendif_expose), module);

    g_signal_connect (G_OBJECT (bd->upper_slider), "expose-event",
                      G_CALLBACK (_blendop_blendif_expose), module);

    g_signal_connect (G_OBJECT (bd->blendif_enable), "value-changed",
                      G_CALLBACK (_blendop_blendif_callback), bd);

    g_signal_connect(G_OBJECT (bd->channel_tabs), "switch_page",
                     G_CALLBACK (_blendop_blendif_tab_switch), bd);

    g_signal_connect (G_OBJECT (bd->upper_slider), "value-changed",
                      G_CALLBACK (_blendop_blendif_upper_callback), bd);

    g_signal_connect (G_OBJECT (bd->lower_slider), "value-changed",
                      G_CALLBACK (_blendop_blendif_lower_callback), bd);

    g_signal_connect (G_OBJECT(tb), "toggled", 
                      G_CALLBACK (_blendop_blendif_pick_toggled), module);

    g_signal_connect (G_OBJECT(sm), "toggled", 
                      G_CALLBACK (_blendop_blendif_showmask_toggled), module);


    gtk_box_pack_start(GTK_BOX(bd->blendif_box), GTK_WIDGET(notebook), TRUE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bd->blendif_box), GTK_WIDGET(uplabel), TRUE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bd->blendif_box), GTK_WIDGET(bd->upper_slider), TRUE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bd->blendif_box), GTK_WIDGET(lowlabel), TRUE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bd->blendif_box), GTK_WIDGET(bd->lower_slider), TRUE, FALSE, 0);

    gtk_box_pack_end(GTK_BOX(blendw), GTK_WIDGET(bd->blendif_box),TRUE,TRUE,0);
    gtk_box_pack_end(GTK_BOX(blendw), GTK_WIDGET(bd->blendif_enable),TRUE,TRUE,0);
  }
}



void dt_iop_gui_init_blending(GtkWidget *iopw, dt_iop_module_t *module)
{
  /* create and add blend mode if module supports it */
  if (module->flags()&IOP_FLAGS_SUPPORTS_BLENDING)
  {
    module->blend_data = g_malloc(sizeof(dt_iop_gui_blend_data_t));
    dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t*)module->blend_data;

    dt_iop_gui_blendop_modes_t modes[21]; /* number must fit exactly!!! */
    modes[0].mode  = DEVELOP_BLEND_DISABLED;          modes[0].name  = _("off");
    modes[1].mode  = DEVELOP_BLEND_NORMAL;            modes[1].name  = _("normal");
    modes[2].mode  = DEVELOP_BLEND_INVERSE;           modes[2].name  = _("inverse");
    modes[3].mode  = DEVELOP_BLEND_LIGHTEN;           modes[3].name  = _("lighten");
    modes[4].mode  = DEVELOP_BLEND_DARKEN;            modes[4].name  = _("darken");
    modes[5].mode  = DEVELOP_BLEND_MULTIPLY;          modes[5].name  = _("multiply");
    modes[6].mode  = DEVELOP_BLEND_AVERAGE;           modes[6].name  = _("average");
    modes[7].mode  = DEVELOP_BLEND_ADD;               modes[7].name  = _("addition");
    modes[8].mode  = DEVELOP_BLEND_SUBSTRACT;         modes[8].name  = _("subtract");
    modes[9].mode  = DEVELOP_BLEND_DIFFERENCE;        modes[9].name  = _("difference");
    modes[10].mode = DEVELOP_BLEND_SCREEN;            modes[10].name = _("screen");
    modes[11].mode = DEVELOP_BLEND_OVERLAY;           modes[11].name = _("overlay");
    modes[12].mode = DEVELOP_BLEND_SOFTLIGHT;         modes[12].name = _("softlight");
    modes[13].mode = DEVELOP_BLEND_HARDLIGHT;         modes[13].name = _("hardlight");
    modes[14].mode = DEVELOP_BLEND_VIVIDLIGHT;        modes[14].name = _("vividlight");
    modes[15].mode = DEVELOP_BLEND_LINEARLIGHT;       modes[15].name = _("linearlight");
    modes[16].mode = DEVELOP_BLEND_PINLIGHT;          modes[16].name = _("pinlight");
    modes[17].mode = DEVELOP_BLEND_LIGHTNESS;         modes[17].name = _("lightness");
    modes[18].mode = DEVELOP_BLEND_CHROMA;            modes[18].name = _("chroma");
    modes[19].mode = DEVELOP_BLEND_HUE;               modes[19].name = _("hue");
    modes[20].mode = DEVELOP_BLEND_COLOR;             modes[20].name = _("color");


    bd->number_modes = sizeof(modes) / sizeof(dt_iop_gui_blendop_modes_t);
    memcpy(bd->modes, modes, bd->number_modes * sizeof(dt_iop_gui_blendop_modes_t));
    bd->iopw = iopw;
    bd->module = module;
    bd->csp = dt_iop_module_colorspace(module);
    bd->blendif_support = (bd->csp == iop_cs_Lab || bd->csp == iop_cs_rgb);
    bd->blendif_box = NULL;

    bd->blend_modes_combo = dt_bauhaus_combobox_new(module);
    dt_bauhaus_widget_set_label(bd->blend_modes_combo, _("blend mode"));
    bd->opacity_slider = dt_bauhaus_slider_new_with_range(module, 0.0, 100.0, 1, 100.0, 0);
    dt_bauhaus_widget_set_label(bd->opacity_slider, _("opacity"));
    dt_bauhaus_slider_set_format(bd->opacity_slider, "%.0f%%");
    module->fusion_slider = bd->opacity_slider;


    for(int k = 0; k < bd->number_modes; k++)
      dt_bauhaus_combobox_add(bd->blend_modes_combo, bd->modes[k].name);
    
    dt_bauhaus_combobox_set(bd->blend_modes_combo, 0);

    gtk_object_set(GTK_OBJECT(bd->opacity_slider), "tooltip-text", _("set the opacity of the blending"), (char *)NULL);
    gtk_object_set(GTK_OBJECT(bd->blend_modes_combo), "tooltip-text", _("choose blending mode"), (char *)NULL);

    g_signal_connect (G_OBJECT (bd->opacity_slider), "value-changed",
                      G_CALLBACK (_blendop_opacity_callback), bd);
    g_signal_connect (G_OBJECT (bd->blend_modes_combo), "value-changed",
                      G_CALLBACK (_blendop_mode_callback), bd);

    gtk_box_pack_start(GTK_BOX(iopw), bd->blend_modes_combo, TRUE, TRUE,0);
    gtk_box_pack_start(GTK_BOX(iopw), bd->opacity_slider, TRUE, TRUE,0);

    if(bd->blendif_support)
    {
      dt_iop_gui_init_blendif(GTK_VBOX(iopw), module);
    }
    gtk_widget_queue_draw(GTK_WIDGET(iopw));
  }
}


// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
