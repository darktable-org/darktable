/*
    This file is part of darktable,
    copyright (c) 2011 Robert Bieber, Johannes Hanika.

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

#include "common/darktable.h"
#include "control/control.h"
#include "control/conf.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "dtgtk/label.h"
#include "dtgtk/togglebutton.h"
#include "gui/gtk.h"
#include "libs/lib.h"

DT_MODULE(1);

typedef struct dt_lib_colorpicker_t
{
  GtkWidget *output_button;
  GtkWidget *output_label;
  GtkWidget *color_mode_selector;
  GtkWidget *statistic_selector;
  GtkWidget *size_selector;
  GtkWidget *picker_button;
  GtkWidget *history_button[5];

  float history_rgb[5][3];
  float history_lab[5][3];
} dt_lib_colorpicker_t;

const char *name()
{
  return _("color picker");
}

uint32_t views()
{
  return DT_VIEW_DARKROOM;
}

uint32_t container()
{
  return DT_UI_CONTAINER_PANEL_LEFT_CENTER;
}

int expandable()
{
  return 1;
}

int position()
{
  return 800;
}

// GUI callbacks

static void _update_picker_output(dt_lib_module_t *self)
{
  GdkColor c;
  dt_lib_colorpicker_t *data = self->data;
  dt_iop_module_t *module = get_colorout_module();
  if(module)
  {
    char colstring[512];

    darktable.gui->reset = 1;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(data->picker_button),
                                 module->request_color_pick);
    darktable.gui->reset = 0;

    int input_color = dt_conf_get_int("ui_last/colorpicker_model");

    // always adjust picked color:
    int m = dt_conf_get_int("ui_last/colorpicker_mode");
    float fallback_col[] = {0,0,0};
    float *rgb = fallback_col;
    float *lab = fallback_col;
    switch(m)
    {
      case 0: // mean
        rgb = darktable.gui->picked_color_output_cs;
        lab = module->picked_color;
        break;
      case 1: //min
        rgb = darktable.gui->picked_color_output_cs_min;
        lab = module->picked_color_min;
        break;
      default:
        rgb = darktable.gui->picked_color_output_cs_max;
        lab = module->picked_color_max;
        break;
    }
    switch(input_color)
    {
    case 0: // rgb
      snprintf(colstring, 512, "(%d, %d, %d)", (int)(255 * rgb[0]),
               (int)(255 * rgb[1]), (int)(255 * rgb[2]));
      break;
    case 1: // Lab
      snprintf(colstring, 512, "(%.03f, %.03f, %.03f)", lab[0], lab[1], lab[2]);
      break;
    }
    gtk_label_set_label(GTK_LABEL(data->output_label), colstring);

    // Setting the button color
    c.red = rgb[0] * 65535;
    c.green = rgb[1] * 65535;
    c.blue = rgb[2] * 65535;
    gtk_widget_modify_bg(data->output_button, GTK_STATE_INSENSITIVE, &c);
  }
}

static void _picker_button_toggled (GtkToggleButton *button, gpointer p)
{
  if(darktable.gui->reset) return;
  dt_iop_module_t *module = get_colorout_module();
  if(module)
  {
    dt_iop_request_focus(module);
    module->request_color_pick = gtk_toggle_button_get_active(button);
  }
  else
  {
    dt_iop_request_focus(NULL);
  }
  dt_control_gui_queue_draw();
}

static void _statistic_changed (GtkComboBox *widget, gpointer p)
{
  dt_conf_set_int("ui_last/colorpicker_mode", gtk_combo_box_get_active(widget));
  _update_picker_output((dt_lib_module_t*)p);
}

static void _color_mode_changed(GtkComboBox *widget, gpointer p)
{
  dt_conf_set_int("ui_last/colorpicker_model",
                  gtk_combo_box_get_active(widget));
  _update_picker_output((dt_lib_module_t*)p);
}

static void _size_changed(GtkComboBox *widget, gpointer p)
{
  dt_conf_set_int("ui_last/colorpicker_size",
                  gtk_combo_box_get_active(widget));
}

static void _history_button_clicked(GtkButton *button, gpointer self)
{

  unsigned int n;
  unsigned int i;
  int m;
  GdkColor c;
  dt_iop_module_t *module = get_colorout_module();
  dt_lib_colorpicker_t *data = ((dt_lib_module_t*)self)->data;

  // First figuring out which button we're dealing with
  for(n = 0; data->history_button[n] != GTK_WIDGET(button) && n < 4; n++);

  // Saving the current picker data
  m = dt_conf_get_int("ui_last/colorpicker_mode");
  float fallback_col[] = {0,0,0};
  float *rgb = fallback_col;
  float *lab = fallback_col;
  switch(m)
  {
    case 0: // mean
      rgb = darktable.gui->picked_color_output_cs;
      lab = module->picked_color;
      break;
    case 1: //min
      rgb = darktable.gui->picked_color_output_cs_min;
      lab = module->picked_color_min;
      break;
    default:
      rgb = darktable.gui->picked_color_output_cs_max;
      lab = module->picked_color_max;
      break;
  }

  for(i = 0; i < 3; i++)
    data->history_rgb[n][i] = rgb[i];
  for(i = 0; i < 3; i++)
    data->history_lab[n][i] = lab[i];

  c.red = rgb[0] * 65535;
  c.green = rgb[1] * 65535;
  c.blue = rgb[2] * 65535;
  gtk_widget_modify_bg(GTK_WIDGET(button), GTK_STATE_NORMAL, &c);
  gtk_widget_modify_bg(GTK_WIDGET(button), GTK_STATE_PRELIGHT, &c);
  gtk_widget_modify_bg(GTK_WIDGET(button), GTK_STATE_ACTIVE, &c);
}

void gui_init(dt_lib_module_t *self)
{
  unsigned int i;
  unsigned int j;

  GdkColor c;

  GtkWidget *container = gtk_vbox_new(FALSE, 10);
  GtkWidget *output_row = gtk_hbox_new(FALSE, 10);
  GtkWidget *output_options = gtk_vbox_new(FALSE, 10);
  GtkWidget *picker_subrow = gtk_hbox_new(FALSE, 10);
  GtkWidget *history_label = dtgtk_label_new(_("history"),
                                             DARKTABLE_LABEL_TAB
                                             | DARKTABLE_LABEL_ALIGN_RIGHT);
  GtkWidget *history_buttons_row = gtk_hbox_new(FALSE, 10);

  // Initializing self data structure
  dt_lib_colorpicker_t *data =
      (dt_lib_colorpicker_t*)malloc(sizeof(dt_lib_colorpicker_t));
  self->data = (void*)data;
  memset(data, 0, sizeof(dt_lib_colorpicker_t));

  // Initializing proxy functions
  darktable.lib->proxy.colorpicker.module = self;
  darktable.lib->proxy.colorpicker.update_panel =  _update_picker_output;

  // Setting up the GUI
  self->widget = container;
  gtk_box_pack_start(GTK_BOX(container), output_row, TRUE, TRUE, 0);

  // The output button
  data->output_button = gtk_button_new();
  gtk_widget_set_sensitive(data->output_button, FALSE);
  gtk_box_pack_start(GTK_BOX(output_row), data->output_button, TRUE, TRUE, 0);

  // The picker button, output selectors and label
  gtk_box_pack_start(GTK_BOX(output_row), output_options, TRUE, TRUE, 0);

  data->size_selector = gtk_combo_box_new_text();
  gtk_combo_box_append_text(GTK_COMBO_BOX(data->size_selector),
                            _("point"));
  gtk_combo_box_append_text(GTK_COMBO_BOX(data->size_selector),
                            _("area"));
  gtk_combo_box_set_active(GTK_COMBO_BOX(data->size_selector),
                           dt_conf_get_int("ui_last/colorpicker_size"));
  gtk_widget_set_size_request(data->size_selector, 30, -1);
  gtk_box_pack_start(GTK_BOX(picker_subrow), data->size_selector,
                     TRUE, TRUE, 0);

  g_signal_connect(G_OBJECT(data->size_selector), "changed",
                   G_CALLBACK(_size_changed), NULL);

  data->picker_button = dtgtk_togglebutton_new(dtgtk_cairo_paint_colorpicker,
                                               CPF_STYLE_BOX);
  gtk_widget_set_size_request(data->picker_button, 10, -1);
  gtk_box_pack_start(GTK_BOX(picker_subrow), data->picker_button,
                     TRUE, TRUE, 0);

  g_signal_connect(G_OBJECT(data->picker_button), "toggled",
                   G_CALLBACK(_picker_button_toggled), NULL);

  gtk_box_pack_start(GTK_BOX(output_options), picker_subrow, TRUE, TRUE, 0);

  data->statistic_selector = gtk_combo_box_new_text();
  gtk_combo_box_append_text(GTK_COMBO_BOX(data->statistic_selector),
                            _("mean"));
  gtk_combo_box_append_text(GTK_COMBO_BOX(data->statistic_selector),
                            _("min"));
  gtk_combo_box_append_text(GTK_COMBO_BOX(data->statistic_selector),
                            _("max"));
  gtk_combo_box_set_active(GTK_COMBO_BOX(data->statistic_selector),
                           dt_conf_get_int("ui_last/colorpicker_mode"));
  gtk_box_pack_start(GTK_BOX(output_options), data->statistic_selector,
                     TRUE, TRUE, 0);

  g_signal_connect(G_OBJECT(data->statistic_selector), "changed",
                   G_CALLBACK(_statistic_changed), self);

  data->color_mode_selector = gtk_combo_box_new_text();
  gtk_combo_box_append_text(GTK_COMBO_BOX(data->color_mode_selector),
                            _("rgb"));
  gtk_combo_box_append_text(GTK_COMBO_BOX(data->color_mode_selector),
                            _("Lab"));
  gtk_combo_box_set_active(GTK_COMBO_BOX(data->color_mode_selector),
                           dt_conf_get_int("ui_last/colorpicker_model"));
  gtk_box_pack_start(GTK_BOX(output_options), data->color_mode_selector,
                     TRUE, TRUE, 0);

  g_signal_connect(G_OBJECT(data->color_mode_selector), "changed",
                   G_CALLBACK(_color_mode_changed), self);

  data->output_label = gtk_label_new(_(""));
  gtk_label_set_justify(GTK_LABEL(data->output_label), GTK_JUSTIFY_CENTER);
  gtk_widget_set_size_request(data->output_label, 80, -1);
  gtk_box_pack_start(GTK_BOX(output_options), data->output_label,
                     FALSE, FALSE, 0);

  // Adding the history
  gtk_box_pack_start(GTK_BOX(container), history_label, TRUE, TRUE, 0);

  // First the buttons
  c.red = 0;
  c.green = 0;
  c.blue = 0;

  for(i = 0; i < 5; i++)
  {
    data->history_button[i] = gtk_button_new();
    gtk_widget_set_size_request(data->history_button[i], -1, 40);
    gtk_widget_set_tooltip_text(data->history_button[i],
                                _("click to save a color in this slot"));
    gtk_box_pack_start(GTK_BOX(history_buttons_row), data->history_button[i],
                       TRUE, TRUE, 0);

    // Initializing each slot in the history to black
    for(j = 0; j < 3; j++)
      data->history_rgb[i][j] = 0;
    for(j = 0; j < 3; j++)
      data->history_lab[i][j] = 0;

    gtk_widget_modify_bg(data->history_button[i], GTK_STATE_NORMAL, &c);
    gtk_widget_modify_bg(data->history_button[i], GTK_STATE_PRELIGHT, &c);
    gtk_widget_modify_bg(data->history_button[i], GTK_STATE_ACTIVE, &c);


    g_signal_connect(G_OBJECT(data->history_button[i]), "clicked",
                     G_CALLBACK(_history_button_clicked), (gpointer)self);
  }
  gtk_box_pack_start(GTK_BOX(container), history_buttons_row,
                     TRUE, TRUE, 0);
}

void gui_cleanup(dt_lib_module_t *self)
{
  // Clearing proxy functions
  darktable.lib->proxy.colorpicker.module = NULL;
  darktable.lib->proxy.colorpicker.update_panel = NULL;


  free(self->data);
  self->data = NULL;
}
