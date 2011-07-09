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
  GtkWidget *history_label;
  GtkWidget *history_button_hovered;
  GtkWidget *samples_container;
  GtkWidget *samples_mode_selector;
  GtkWidget *samples_statistic_selector;
  GtkWidget *add_sample_button;

  float history_rgb[5][3];
  float history_lab[5][3];

} dt_lib_colorpicker_t;

typedef struct dt_live_sample_t
{
  GtkWidget *output_button;
  GtkWidget *output_label[3];
} dt_live_sample_t;

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
  dt_lib_colorpicker_t *data = ((dt_lib_module_t*)p)->data;
  gtk_widget_set_sensitive(GTK_WIDGET(data->add_sample_button),
                           gtk_toggle_button_get_active(button));
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
  dt_lib_colorpicker_t *data = ((dt_lib_module_t*)p)->data;

  dt_conf_set_int("ui_last/colorpicker_size",
                  gtk_combo_box_get_active(widget));
  gtk_widget_set_sensitive(data->statistic_selector,
                           dt_conf_get_int("ui_last/colorpicker_size"));
  dt_dev_invalidate_from_gui(darktable.develop);
  _update_picker_output(p);
}

static gboolean _history_button_enter(GtkWidget *widget, GdkEvent *event,
                                      gpointer self)
{
  unsigned int n;
  char text[512];
  dt_lib_colorpicker_t *data = ((dt_lib_module_t*)self)->data;

  // First figuring out which button we're dealing with
  for(n = 0; data->history_button[n] != widget && n < 4; n++);

  data->history_button_hovered = data->history_button[n];

  if(dt_conf_get_int("ui_last/colorpicker_model") == 0)
  {
    // Then set RGB
    snprintf(text, 512, "(%d, %d, %d)",
             (int)(255 * data->history_rgb[n][0]),
             (int)(255 * data->history_rgb[n][1]),
             (int)(255 * data->history_rgb[n][2]));
  }
  else
  {
    // ...or Lab
    snprintf(text, 512, "(%.03f, %.03f, %.03f)",
             data->history_lab[n][0],
             data->history_lab[n][1],
             data->history_lab[n][2]);

  }

  gtk_label_set_text(GTK_LABEL(data->history_label), text);

  return FALSE;
}

static gboolean _history_button_leave(GtkWidget *widget, GdkEvent *event,
                                      gpointer self)
{
  dt_lib_colorpicker_t *data = ((dt_lib_module_t*)self)->data;

  // Clearing the history display labels
  gtk_label_set_text(GTK_LABEL(data->history_label), "");

  data->history_button_hovered = NULL;

  return FALSE;
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

  if(data->history_button_hovered)
    _history_button_enter(data->history_button_hovered, NULL, self);
}

static void _update_samples_output(dt_lib_module_t *self)
{
  float fallback[] = {0., 0., 0.};
  float *rgb = fallback;
  float *lab = fallback;
  char text[1024];
  GSList *samples = darktable.gui->color_picker_samples;
  dt_colorpicker_sample_t *sample = NULL;
  GdkColor c;

  int model = dt_conf_get_int("ui_last/colorsamples_model");
  int statistic = dt_conf_get_int("ui_last/colorsamples_mode");

  while(samples)
  {
    sample = samples->data;

    switch(statistic)
    {
    case 0:
      rgb = sample->picked_color_rgb_mean;
      lab = sample->picked_color_lab_mean;
      break;

    case 1:
      rgb = sample->picked_color_rgb_min;
      lab = sample->picked_color_lab_min;
      break;

    case 2:
      rgb = sample->picked_color_rgb_max;
      lab = sample->picked_color_lab_max;
      break;
    }

    // Setting the output button
    c.red = rgb[0] * 65535;
    c.green = rgb[1] * 65535;
    c.blue = rgb[2] * 65535;
    gtk_widget_modify_bg(sample->output_button, GTK_STATE_INSENSITIVE, &c);

    // Setting the output label
    switch(model)
    {
    case 0:
      // RGB
      snprintf(text, 1024, "(%d, %d, %d)",
               (int)(rgb[0] * 255),
               (int)(rgb[1] * 255),
               (int)(rgb[2] * 255));
      break;

    case 1:
      // Lab
      snprintf(text, 1024, "(%.03f, %.03f, %.03f)",
               lab[0], lab[1], lab[2]);
      break;
    }
    gtk_label_set_text(GTK_LABEL(sample->output_label), text);

    samples = g_slist_next(samples);
  }
}

static void _samples_statistic_changed (GtkComboBox *widget, gpointer p)
{
  dt_conf_set_int("ui_last/colorsamples_mode",
                  gtk_combo_box_get_active(widget));
  _update_samples_output((dt_lib_module_t*)p);
}

static void _samples_mode_changed(GtkComboBox *widget, gpointer p)
{
  dt_conf_set_int("ui_last/colorsamples_model",
                  gtk_combo_box_get_active(widget));
  _update_samples_output((dt_lib_module_t*)p);
}

static void _remove_sample(GtkButton *widget, gpointer data)
{
  dt_colorpicker_sample_t *sample = (dt_colorpicker_sample_t*)data;
  gtk_widget_hide_all(sample->container);
  gtk_widget_destroy(sample->output_button);
  gtk_widget_destroy(sample->output_label);
  gtk_widget_destroy(sample->delete_button);
  gtk_widget_destroy(sample->container);
  darktable.gui->color_picker_samples =
      g_slist_remove(darktable.gui->color_picker_samples, data);
  free(sample);
}

static void _add_sample(GtkButton *widget, gpointer self)
{
  dt_lib_colorpicker_t *data = ((dt_lib_module_t*)self)->data;
  dt_colorpicker_sample_t *sample =
      (dt_colorpicker_sample_t*)malloc(sizeof(dt_colorpicker_sample_t));
  darktable.gui->color_picker_samples =
      g_slist_append(darktable.gui->color_picker_samples, sample);
  dt_iop_module_t *module = get_colorout_module();
  int i;

  // Initializing the UI
  sample->container = gtk_hbox_new(FALSE, 10);
  gtk_box_pack_start(GTK_BOX(data->samples_container), sample->container,
                     TRUE, TRUE, 0);

  sample->output_button = gtk_button_new();
  gtk_widget_set_size_request(sample->output_button, 40, -1);
  gtk_widget_set_sensitive(sample->output_button, FALSE);
  gtk_box_pack_start(GTK_BOX(sample->container), sample->output_button,
                     FALSE, FALSE, 0);

  sample->output_label = gtk_label_new("");
  gtk_box_pack_start(GTK_BOX(sample->container), sample->output_label,
                     TRUE, TRUE, 0);

  sample->delete_button = gtk_button_new_from_stock(GTK_STOCK_REMOVE);
  gtk_box_pack_start(GTK_BOX(sample->container), sample->delete_button,
                     FALSE, FALSE, 0);

  g_signal_connect(G_OBJECT(sample->delete_button), "clicked",
                   G_CALLBACK(_remove_sample), sample);

  gtk_widget_show_all(data->samples_container);

  // Setting the actual data
  if(dt_conf_get_int("ui_last/colorpicker_size"))
  {
    sample->size = DT_COLORPICKER_SIZE_BOX;
    for(i = 0; i < 4; i++)
      sample->box[i] = module->color_picker_box[i];
  }
  else
  {
    sample->size = DT_COLORPICKER_SIZE_POINT;
    for(i = 0; i < 2; i++)
      sample->point[i] = module->color_picker_point[i];
  }

  for(i = 0; i < 3; i++)
    sample->picked_color_lab_max[i] = module->picked_color_max[i];
  for(i = 0; i < 3; i++)
    sample->picked_color_lab_mean[i] = module->picked_color[i];
  for(i = 0; i < 3; i++)
    sample->picked_color_lab_min[i] = module->picked_color_min[i];
  for(i = 0; i < 3; i++)
    sample->picked_color_rgb_max[i] =
        darktable.gui->picked_color_output_cs_max[i];
  for(i = 0; i < 3; i++)
    sample->picked_color_rgb_mean[i] =
        darktable.gui->picked_color_output_cs[i];
  for(i = 0; i < 3; i++)
    sample->picked_color_rgb_min[i] =
        darktable.gui->picked_color_output_cs_min[i];

  // Updating the display
  _update_samples_output((dt_lib_module_t*)self);
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
  GtkWidget *history_label = dtgtk_label_new(_("static history"),
                                             DARKTABLE_LABEL_TAB
                                             | DARKTABLE_LABEL_ALIGN_RIGHT);
  GtkWidget *history_buttons_row = gtk_hbox_new(FALSE, 10);
  GtkWidget *samples_label = dtgtk_label_new(_("live samples"),
                                             DARKTABLE_LABEL_TAB
                                             | DARKTABLE_LABEL_ALIGN_RIGHT);
  GtkWidget *samples_options_row = gtk_hbox_new(FALSE, 10);

  // Initializing self data structure
  dt_lib_colorpicker_t *data =
      (dt_lib_colorpicker_t*)malloc(sizeof(dt_lib_colorpicker_t));
  self->data = (void*)data;
  memset(data, 0, sizeof(dt_lib_colorpicker_t));

  // Initializing proxy functions
  darktable.lib->proxy.colorpicker.module = self;
  darktable.lib->proxy.colorpicker.update_panel =  _update_picker_output;
  darktable.lib->proxy.colorpicker.update_samples = _update_samples_output;

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
                   G_CALLBACK(_size_changed), (gpointer)self);

  data->picker_button = dtgtk_togglebutton_new(dtgtk_cairo_paint_colorpicker,
                                               CPF_STYLE_BOX);
  gtk_widget_set_size_request(data->picker_button, 10, -1);
  gtk_box_pack_start(GTK_BOX(picker_subrow), data->picker_button,
                     TRUE, TRUE, 0);

  g_signal_connect(G_OBJECT(data->picker_button), "toggled",
                   G_CALLBACK(_picker_button_toggled), self);

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
  gtk_widget_set_sensitive(data->statistic_selector,
                           dt_conf_get_int("ui_last/colorpicker_size"));
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

    gtk_widget_set_events(data->history_button[i],
                          GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK);
    g_signal_connect(G_OBJECT(data->history_button[i]), "enter-notify-event",
                     G_CALLBACK(_history_button_enter), (gpointer)self);
    g_signal_connect(G_OBJECT(data->history_button[i]), "leave-notify-event",
                     G_CALLBACK(_history_button_leave), (gpointer)self);
    g_signal_connect(G_OBJECT(data->history_button[i]), "clicked",
                     G_CALLBACK(_history_button_clicked), (gpointer)self);
  }
  gtk_box_pack_start(GTK_BOX(container), history_buttons_row,
                     TRUE, TRUE, 0);

  data->history_label = gtk_label_new("");
  gtk_box_pack_start(GTK_BOX(container), data->history_label, TRUE, TRUE, 0);

  // Adding the live samples section
  gtk_box_pack_start(GTK_BOX(container), samples_label, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(container), samples_options_row, TRUE, TRUE, 0);

  data->samples_statistic_selector = gtk_combo_box_new_text();
  gtk_combo_box_append_text(GTK_COMBO_BOX(data->samples_statistic_selector),
                            _("mean"));
  gtk_combo_box_append_text(GTK_COMBO_BOX(data->samples_statistic_selector),
                            _("min"));
  gtk_combo_box_append_text(GTK_COMBO_BOX(data->samples_statistic_selector),
                            _("max"));
  gtk_combo_box_set_active(GTK_COMBO_BOX(data->samples_statistic_selector),
                           dt_conf_get_int("ui_last/colorsamples_mode"));
  gtk_box_pack_start(GTK_BOX(samples_options_row),
                     data->samples_statistic_selector,
                     TRUE, TRUE, 0);

  g_signal_connect(G_OBJECT(data->samples_statistic_selector), "changed",
                   G_CALLBACK(_samples_statistic_changed), self);

  data->samples_mode_selector = gtk_combo_box_new_text();
  gtk_combo_box_append_text(GTK_COMBO_BOX(data->samples_mode_selector),
                            _("rgb"));
  gtk_combo_box_append_text(GTK_COMBO_BOX(data->samples_mode_selector),
                            _("Lab"));
  gtk_combo_box_set_active(GTK_COMBO_BOX(data->samples_mode_selector),
                           dt_conf_get_int("ui_last/colorsamples_model"));
  gtk_box_pack_start(GTK_BOX(samples_options_row), data->samples_mode_selector,
                     TRUE, TRUE, 0);

  g_signal_connect(G_OBJECT(data->samples_mode_selector), "changed",
                   G_CALLBACK(_samples_mode_changed), self);

  data->add_sample_button = gtk_button_new_from_stock(GTK_STOCK_ADD);
  gtk_widget_set_sensitive(data->add_sample_button, FALSE);
  gtk_box_pack_start(GTK_BOX(samples_options_row), data->add_sample_button,
                     FALSE, FALSE, 0);

  g_signal_connect(G_OBJECT(data->add_sample_button), "clicked",
                   G_CALLBACK(_add_sample), self);

  data->samples_container = gtk_vbox_new(FALSE, 10);
  gtk_box_pack_start(GTK_BOX(container), data->samples_container,
                     TRUE, TRUE, 0);
}

void gui_cleanup(dt_lib_module_t *self)
{
  // Clearing proxy functions
  darktable.lib->proxy.colorpicker.module = NULL;
  darktable.lib->proxy.colorpicker.update_panel = NULL;
  darktable.lib->proxy.colorpicker.update_samples = NULL;

  while(darktable.gui->color_picker_samples)
    _remove_sample(NULL, darktable.gui->color_picker_samples->data);

  free(self->data);
  self->data = NULL;
}
