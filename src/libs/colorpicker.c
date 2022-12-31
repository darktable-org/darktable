/*
    This file is part of darktable,
    Copyright (C) 2011-2021 darktable developers.

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
#include "libs/colorpicker.h"
#include "common/darktable.h"
#include "common/color_vocabulary.h"
#include "common/iop_profile.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "dtgtk/button.h"
#include "dtgtk/togglebutton.h"
#include "gui/accelerators.h"
#include "gui/color_picker_proxy.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include "libs/lib_api.h"
#include "common/colorspaces_inline_conversions.h"

DT_MODULE(1);

typedef enum dt_lib_colorpicker_model_t
{
  DT_LIB_COLORPICKER_MODEL_RGB = 0,
  DT_LIB_COLORPICKER_MODEL_LAB,
  DT_LIB_COLORPICKER_MODEL_LCH,
  DT_LIB_COLORPICKER_MODEL_HSL,
  DT_LIB_COLORPICKER_MODEL_HSV,
  DT_LIB_COLORPICKER_MODEL_HEX,
  DT_LIB_COLORPICKER_MODEL_NONE,
} dt_lib_colorpicker_model_t;

const gchar *dt_lib_colorpicker_model_names[]
  = { N_("RGB"), N_("Lab"), N_("LCh"), N_("HSL"), N_("HSV"), N_("Hex"), N_("none"), NULL };
const gchar *dt_lib_colorpicker_statistic_names[]
  = { N_("mean"), N_("min"), N_("max"), NULL };

typedef struct dt_lib_colorpicker_t
{
  dt_lib_colorpicker_model_t model;
  dt_lib_colorpicker_statistic_t statistic;
  GtkWidget *large_color_patch;
  GtkWidget *color_mode_selector;
  GtkWidget *statistic_selector;
  GtkWidget *picker_button;
  GtkWidget *samples_container;
  GtkWidget *add_sample_button;
  GtkWidget *display_samples_check_box;
  dt_colorpicker_sample_t primary_sample;
} dt_lib_colorpicker_t;

const char *name(dt_lib_module_t *self)
{
  return _("color picker");
}

const char **views(dt_lib_module_t *self)
{
  static const char *v[] = {"darkroom", NULL};
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_LEFT_CENTER;
}

int expandable(dt_lib_module_t *self)
{
  return 1;
}

int position(const dt_lib_module_t *self)
{
  return 800;
}

// GUI callbacks

static gboolean _sample_draw_callback(GtkWidget *widget, cairo_t *cr, dt_colorpicker_sample_t *sample)
{
  const guint width = gtk_widget_get_allocated_width(widget);
  const guint height = gtk_widget_get_allocated_height(widget);

  set_color(cr, sample->swatch);
  cairo_rectangle(cr, 0, 0, width, height);
  cairo_fill (cr);

  // if the sample is locked we want to add a lock
  if(sample->locked)
  {
    const int border = DT_PIXEL_APPLY_DPI(2);
    const int icon_width = width - 2 * border;
    const int icon_height = height - 2 * border;
    if(icon_width > 0 && icon_height > 0)
    {
      GdkRGBA fg_color;
      gtk_style_context_get_color(gtk_widget_get_style_context(widget), gtk_widget_get_state_flags(widget), &fg_color);

      gdk_cairo_set_source_rgba(cr, &fg_color);
      dtgtk_cairo_paint_lock(cr, border, border, icon_width, icon_height, 0, NULL);
    }
  }

  return FALSE;
}

static void _update_sample_label(dt_lib_module_t *self, dt_colorpicker_sample_t *sample)
{
  dt_lib_colorpicker_t *data = self->data;
  const dt_lib_colorpicker_statistic_t statistic = data->statistic;

  sample->swatch.red   = sample->display[statistic][0];
  sample->swatch.green = sample->display[statistic][1];
  sample->swatch.blue  = sample->display[statistic][2];
  for_each_channel(ch)
  {
    sample->label_rgb[ch]  = (int)roundf(sample->scope[statistic][ch] * 255.f);
  }

  // Setting the output label
  char text[128] = { 0 };
  dt_aligned_pixel_t alt = { 0 };

  switch(data->model)
  {
    case DT_LIB_COLORPICKER_MODEL_RGB:
      snprintf(text, sizeof(text), "%6d %6d %6d", sample->label_rgb[0], sample->label_rgb[1], sample->label_rgb[2]);
      break;

    case DT_LIB_COLORPICKER_MODEL_LAB:
      snprintf(text, sizeof(text), "%6.02f %6.02f %6.02f",
               CLAMP(sample->lab[statistic][0], .0f, 100.0f), sample->lab[statistic][1], sample->lab[statistic][2]);
      break;

    case DT_LIB_COLORPICKER_MODEL_LCH:
      dt_Lab_2_LCH(sample->lab[statistic], alt);
      snprintf(text, sizeof(text), "%6.02f %6.02f %6.02f", CLAMP(alt[0], .0f, 100.0f), alt[1], alt[2] * 360.f);
      break;

    case DT_LIB_COLORPICKER_MODEL_HSL:
      dt_RGB_2_HSL(sample->scope[statistic], alt);
      snprintf(text, sizeof(text), "%6.02f %6.02f %6.02f", alt[0] * 360.f, alt[1] * 100.f, alt[2] * 100.f);
      break;

    case DT_LIB_COLORPICKER_MODEL_HSV:
      dt_RGB_2_HSV(sample->scope[statistic], alt);
      snprintf(text, sizeof(text), "%6.02f %6.02f %6.02f", alt[0] * 360.f, alt[1] * 100.f, alt[2] * 100.f);
      break;

    case DT_LIB_COLORPICKER_MODEL_HEX:
      // clamping to allow for 2-digit hex display
      snprintf(text, sizeof(text), "0x%02X%02X%02X",
               CLAMP(sample->label_rgb[0], 0, 255), CLAMP(sample->label_rgb[1], 0, 255), CLAMP(sample->label_rgb[2], 0, 255));
      break;

    default:
    case DT_LIB_COLORPICKER_MODEL_NONE:
      snprintf(text, sizeof(text), "◎");
      break;
  }

  if(g_strcmp0(gtk_label_get_text(GTK_LABEL(sample->output_label)), text))
    gtk_label_set_text(GTK_LABEL(sample->output_label), text);
  gtk_widget_queue_draw(sample->color_patch);
}

static void _update_picker_output(dt_lib_module_t *self)
{
  dt_lib_colorpicker_t *data = self->data;
  _update_sample_label(self, &data->primary_sample);
  gtk_widget_queue_draw(data->large_color_patch);

  // allow live sample button to work for iop samples
  gtk_widget_set_sensitive(GTK_WIDGET(data->add_sample_button),
                           darktable.lib->proxy.colorpicker.picker_proxy != NULL);
}

static gboolean _large_patch_toggle(GtkWidget *widget, GdkEvent *event, dt_lib_colorpicker_t *data)
{
  const gboolean show_large_patch = !dt_conf_get_bool("ui_last/colorpicker_large");
  dt_conf_set_bool("ui_last/colorpicker_large", show_large_patch);

  gtk_widget_set_visible(gtk_widget_get_parent(data->large_color_patch), show_large_patch);

  return FALSE;
}

static void _picker_button_toggled(GtkToggleButton *button, dt_lib_colorpicker_t *data)
{
  gtk_widget_set_sensitive(GTK_WIDGET(data->add_sample_button), gtk_toggle_button_get_active(button));
}

static void _update_size(dt_lib_module_t *self, dt_lib_colorpicker_size_t size)
{
  dt_lib_colorpicker_t *data = self->data;
  data->primary_sample.size = size;

  _update_picker_output(self);
}

static void _update_samples_output(dt_lib_module_t *self)
{
  for(GSList *samples = darktable.lib->proxy.colorpicker.live_samples;
      samples;
      samples = g_slist_next(samples))
  {
    _update_sample_label(self, samples->data);
  }
}

/* set sample area proxy impl */

static void _set_sample_box_area(dt_lib_module_t *self, const dt_boundingbox_t box)
{
  dt_lib_colorpicker_t *data = self->data;

  // primary sample always follows/represents current picker
  for(int k = 0; k < 4; k++)
    data->primary_sample.box[k] = box[k];

  _update_size(self, DT_LIB_COLORPICKER_SIZE_BOX);
}

static void _set_sample_point(dt_lib_module_t *self, const float pos[2])
{
  dt_lib_colorpicker_t *data = self->data;

  // primary sample always follows/represents current picker
  data->primary_sample.point[0] = pos[0];
  data->primary_sample.point[1] = pos[1];

  _update_size(self, DT_LIB_COLORPICKER_SIZE_POINT);
}

static gboolean _sample_tooltip_callback(GtkWidget *widget, gint x, gint y, gboolean keyboard_mode,
                                         GtkTooltip *tooltip, const dt_colorpicker_sample_t *sample)
{
  gchar **sample_parts = g_malloc0_n(14, sizeof(char*));

  sample_parts[3] = g_strdup_printf("%22s(0x%02X%02X%02X)\n<big><b>%14s</b></big>", " ",
                                    CLAMP(sample->label_rgb[0], 0, 255), CLAMP(sample->label_rgb[1], 0, 255),
                                    CLAMP(sample->label_rgb[2], 0, 255), _("RGB"));
  sample_parts[7] = g_strdup_printf("\n<big><b>%14s</b></big>", _("Lab"));

  for(int i = 0; i < DT_PICK_N; i++)
  {
    sample_parts[i] = g_strdup_printf("<span background='#%02X%02X%02X'>%32s</span>",
                                      (int)roundf(CLAMP(sample->display[i][0], 0.f, 1.f) * 255.f),
                                      (int)roundf(CLAMP(sample->display[i][1], 0.f, 1.f) * 255.f),
                                      (int)roundf(CLAMP(sample->display[i][2], 0.f, 1.f) * 255.f), " ");

    sample_parts[i + 4] = g_strdup_printf("<span foreground='#FF7F7F'>%6d</span>  "
                                          "<span foreground='#7FFF7F'>%6d</span>  "
                                          "<span foreground='#7F7FFF'>%6d</span>  %s",
                                          (int)roundf(sample->scope[i][0] * 255.f),
                                          (int)roundf(sample->scope[i][1] * 255.f),
                                          (int)roundf(sample->scope[i][2] * 255.f),
                                          _(dt_lib_colorpicker_statistic_names[i]));

    sample_parts[i + 8] = g_strdup_printf("%6.02f  %6.02f  %6.02f  %s",
                                          sample->lab[i][0], sample->lab[i][1], sample->lab[i][2],
                                          _(dt_lib_colorpicker_statistic_names[i]));
  }

  dt_aligned_pixel_t color;
  dt_Lab_2_LCH(sample->lab[DT_PICK_MEAN], color);
  sample_parts[11] = g_strdup_printf("\n<big><b>%14s</b></big>", _("color"));
  sample_parts[12] = g_strdup_printf("%6s", Lch_to_color_name(color));

  gchar *tooltip_text = g_strjoinv("\n", sample_parts);
  g_strfreev(sample_parts);

  static GtkWidget *view = NULL;
  if(!view)
  {
    view = gtk_text_view_new();
    dt_gui_add_class(view, "dt_transparent_background");
    dt_gui_add_class(view, "dt_monospace");
    g_signal_connect(G_OBJECT(view), "destroy", G_CALLBACK(gtk_widget_destroyed), &view);
  }

  GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
  gtk_text_buffer_set_text(buffer, "", -1);
  GtkTextIter iter;
  gtk_text_buffer_get_start_iter(buffer, &iter);
  gtk_text_buffer_insert_markup(buffer, &iter, tooltip_text, -1);
  gtk_tooltip_set_custom(tooltip, view);
  gtk_widget_map(view); // FIXME: workaround added in order to fix #9908, probably a Gtk issue, remove when fixed upstream

  g_free(tooltip_text);

  return TRUE;
}

static void _statistic_changed(GtkWidget *widget, dt_lib_module_t *self)
{
  dt_lib_colorpicker_t *data = self->data;
  data->statistic = dt_bauhaus_combobox_get(widget);
  darktable.lib->proxy.colorpicker.statistic = (int)data->statistic;
  dt_conf_set_string("ui_last/colorpicker_mode", dt_lib_colorpicker_statistic_names[data->statistic]);

  _update_picker_output(self);
  _update_samples_output(self);
  if(darktable.lib->proxy.colorpicker.display_samples)
      dt_dev_invalidate_from_gui(darktable.develop);
}

static void _color_mode_changed(GtkWidget *widget, dt_lib_module_t *self)
{
  dt_lib_colorpicker_t *data = self->data;
  data->model = dt_bauhaus_combobox_get(widget);
  dt_conf_set_string("ui_last/colorpicker_model", dt_lib_colorpicker_model_names[data->model]);

  _update_picker_output(self);
  _update_samples_output(self);
}

static void _label_size_allocate_callback(GtkWidget *widget, GdkRectangle *allocation, gpointer user_data)
{
  gint label_width;
  gtk_label_set_attributes(GTK_LABEL(widget), NULL);

  PangoStretch stretch = PANGO_STRETCH_NORMAL;

  while(gtk_widget_get_preferred_width(widget, NULL, &label_width),
        label_width > allocation->width && stretch != PANGO_STRETCH_ULTRA_CONDENSED)
  {
    stretch--;

    PangoAttrList *attrlist = pango_attr_list_new();
    PangoAttribute *attr = pango_attr_stretch_new(stretch);
    pango_attr_list_insert(attrlist, attr);
    gtk_label_set_attributes(GTK_LABEL(widget), attrlist);
    pango_attr_list_unref(attrlist);
  }
}

static gboolean _sample_enter_callback(GtkWidget *widget, GdkEvent *event, dt_colorpicker_sample_t *sample)
{
  if(darktable.lib->proxy.colorpicker.picker_proxy)
  {
    darktable.lib->proxy.colorpicker.selected_sample = sample;
    if(darktable.lib->proxy.colorpicker.display_samples)
      dt_dev_invalidate_from_gui(darktable.develop);
   	else
   	  dt_control_queue_redraw_center();
  }

  return FALSE;
}

static gboolean _sample_leave_callback(GtkWidget *widget, GdkEvent *event, gpointer data)
{
  if(event->crossing.detail == GDK_NOTIFY_INFERIOR) return FALSE;

  if(darktable.lib->proxy.colorpicker.selected_sample)
  {
    darktable.lib->proxy.colorpicker.selected_sample = NULL;
    if(darktable.lib->proxy.colorpicker.display_samples)
      dt_dev_invalidate_from_gui(darktable.develop);
   	else
   	  dt_control_queue_redraw_center();
  }

  return FALSE;
}

static void _remove_sample(dt_colorpicker_sample_t *sample)
{
  gtk_widget_destroy(sample->container);
  darktable.lib->proxy.colorpicker.live_samples
    = g_slist_remove(darktable.lib->proxy.colorpicker.live_samples, (gpointer)sample);
  free(sample);
}

static void _remove_sample_cb(GtkButton *widget, dt_colorpicker_sample_t *sample)
{
  _remove_sample(sample);
  dt_dev_invalidate_from_gui(darktable.develop);
}

static gboolean _live_sample_button(GtkWidget *widget, GdkEventButton *event, dt_colorpicker_sample_t *sample)
{
  if(event->button == 1)
  {
    sample->locked = !sample->locked;
    gtk_widget_queue_draw(widget);
  }
  else if(event->button == 3)
  {
    // copy to active picker
    dt_lib_module_t *self = darktable.lib->proxy.colorpicker.module;
    dt_iop_color_picker_t *picker = darktable.lib->proxy.colorpicker.picker_proxy;

    // no active picker, too much iffy GTK work to activate a default
    if(!picker) return FALSE;

    if(sample->size == DT_LIB_COLORPICKER_SIZE_POINT)
      _set_sample_point(self, sample->point);
    else if(sample->size == DT_LIB_COLORPICKER_SIZE_BOX)
      _set_sample_box_area(self, sample->box);
    else
      return FALSE;

    if(picker->module)
    {
      picker->module->dev->preview_status = DT_DEV_PIXELPIPE_DIRTY;
      dt_control_queue_redraw_center();
    }
    else
    {
      dt_dev_invalidate_from_gui(darktable.develop);
    }
  }
  return FALSE;
}

static void _add_sample(GtkButton *widget, dt_lib_module_t *self)
{
  dt_lib_colorpicker_t *data = self->data;
  dt_colorpicker_sample_t *sample = (dt_colorpicker_sample_t *)malloc(sizeof(dt_colorpicker_sample_t));

  if(!darktable.lib->proxy.colorpicker.picker_proxy)
    return;

  memcpy(sample, &data->primary_sample, sizeof(dt_colorpicker_sample_t));
  sample->locked = FALSE;

  sample->container = gtk_event_box_new();
  gtk_widget_add_events(sample->container, GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK);
  g_signal_connect(G_OBJECT(sample->container), "enter-notify-event", G_CALLBACK(_sample_enter_callback), sample);
  g_signal_connect(G_OBJECT(sample->container), "leave-notify-event", G_CALLBACK(_sample_leave_callback), sample);

  GtkWidget *container = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_container_add(GTK_CONTAINER(sample->container), container);

  sample->color_patch = gtk_drawing_area_new();
  gtk_widget_add_events(sample->color_patch, GDK_BUTTON_PRESS_MASK);
  gtk_widget_set_tooltip_text(sample->color_patch, _("hover to highlight sample on canvas,\n"
                                                     "click to lock sample,\n"
                                                     "right-click to load sample area into active color picker"));
  g_signal_connect(G_OBJECT(sample->color_patch), "button-press-event", G_CALLBACK(_live_sample_button), sample);
  g_signal_connect(G_OBJECT(sample->color_patch), "draw", G_CALLBACK(_sample_draw_callback), sample);

  GtkWidget *color_patch_wrapper = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name(color_patch_wrapper, "live-sample");
  gtk_box_pack_start(GTK_BOX(color_patch_wrapper), sample->color_patch, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(container), color_patch_wrapper, TRUE, TRUE, 0);

  sample->output_label = gtk_label_new("");
  dt_gui_add_class(sample->output_label, "dt_monospace");
  gtk_label_set_ellipsize(GTK_LABEL(sample->output_label), PANGO_ELLIPSIZE_START);
  gtk_label_set_selectable(GTK_LABEL(sample->output_label), TRUE);
  gtk_widget_set_has_tooltip(sample->output_label, TRUE);
  g_signal_connect(G_OBJECT(sample->output_label), "query-tooltip", G_CALLBACK(_sample_tooltip_callback), sample);
  g_signal_connect(G_OBJECT(sample->output_label), "size-allocate", G_CALLBACK(_label_size_allocate_callback), sample);
  gtk_box_pack_start(GTK_BOX(container), sample->output_label, TRUE, TRUE, 0);

  GtkWidget *delete_button = dtgtk_togglebutton_new(dtgtk_cairo_paint_remove, 0, NULL);
  g_signal_connect(G_OBJECT(delete_button), "clicked", G_CALLBACK(_remove_sample_cb), sample);
  gtk_box_pack_start(GTK_BOX(container), delete_button, FALSE, FALSE, 0);

  gtk_box_pack_start(GTK_BOX(data->samples_container), sample->container, FALSE, FALSE, 0);
  gtk_widget_show_all(sample->container);

  darktable.lib->proxy.colorpicker.live_samples
      = g_slist_append(darktable.lib->proxy.colorpicker.live_samples, sample);

  // remove emphasis on primary sample from mouseover on this button
  darktable.lib->proxy.colorpicker.selected_sample = NULL;

  // Updating the display
  _update_samples_output(self);
  if(darktable.lib->proxy.colorpicker.display_samples)
      dt_dev_invalidate_from_gui(darktable.develop);
   	else
   	  dt_control_queue_redraw_center();
}

static void _display_samples_changed(GtkToggleButton *button, gpointer data)
{
  dt_conf_set_bool("ui_last/colorpicker_display_samples", gtk_toggle_button_get_active(button));
  darktable.lib->proxy.colorpicker.display_samples = gtk_toggle_button_get_active(button);
  dt_dev_invalidate_from_gui(darktable.develop);
}

static void _restrict_histogram_changed(GtkToggleButton *button, gpointer data)
{
  dt_conf_set_bool("ui_last/colorpicker_restrict_histogram", gtk_toggle_button_get_active(button));
  darktable.lib->proxy.colorpicker.restrict_histogram = gtk_toggle_button_get_active(button);
  dt_dev_invalidate_from_gui(darktable.develop);
}

void gui_init(dt_lib_module_t *self)
{
  // Initializing self data structure
  dt_lib_colorpicker_t *data = (dt_lib_colorpicker_t *)calloc(1, sizeof(dt_lib_colorpicker_t));

  self->data = (void *)data;

  // _update_samples_output() will update the RGB values
  data->primary_sample.swatch.alpha = 1.0;

  // Initializing proxy functions and data
  darktable.lib->proxy.colorpicker.module = self;
  darktable.lib->proxy.colorpicker.display_samples = dt_conf_get_bool("ui_last/colorpicker_display_samples");
  darktable.lib->proxy.colorpicker.primary_sample = &data->primary_sample;
  darktable.lib->proxy.colorpicker.picker_proxy = NULL;
  darktable.lib->proxy.colorpicker.live_samples = NULL;
  darktable.lib->proxy.colorpicker.update_panel = _update_picker_output;
  darktable.lib->proxy.colorpicker.update_samples = _update_samples_output;
  darktable.lib->proxy.colorpicker.set_sample_box_area = _set_sample_box_area;
  darktable.lib->proxy.colorpicker.set_sample_point = _set_sample_point;

  const char *str = dt_conf_get_string_const("ui_last/colorpicker_model"), **names;
  names = dt_lib_colorpicker_model_names;
  for(dt_lib_colorpicker_model_t i=0; *names; names++, i++)
    if(g_strcmp0(str, *names) == 0)
      data->model = i;

  str = dt_conf_get_string_const("ui_last/colorpicker_mode");
  names = dt_lib_colorpicker_statistic_names;
  for(dt_lib_colorpicker_statistic_t i=0; *names; names++, i++)
    if(g_strcmp0(str, *names) == 0)
      data->statistic = i;

  // Setting up the GUI
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  dt_gui_add_class(self->widget, "picker-module");

  // The color patch
  GtkWidget *color_patch_wrapper = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name(GTK_WIDGET(color_patch_wrapper), "color-picker-area");
  GtkWidget *color_patch = gtk_drawing_area_new();
  data->large_color_patch = color_patch;
  gtk_widget_set_tooltip_text(color_patch, _("click to (un)hide large color patch"));
  gtk_widget_set_events(color_patch, GDK_BUTTON_PRESS_MASK | GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK);
  g_signal_connect(G_OBJECT(color_patch), "draw", G_CALLBACK(_sample_draw_callback), &data->primary_sample);
  g_signal_connect(G_OBJECT(color_patch), "button-press-event", G_CALLBACK(_large_patch_toggle), data);
  g_signal_connect(G_OBJECT(color_patch), "enter-notify-event", G_CALLBACK(_sample_enter_callback), &data->primary_sample);
  g_signal_connect(G_OBJECT(color_patch), "leave-notify-event", G_CALLBACK(_sample_leave_callback), &data->primary_sample);
  gtk_box_pack_start(GTK_BOX(color_patch_wrapper), color_patch, TRUE, TRUE, 0);
  gtk_widget_show(color_patch);
  gtk_widget_set_no_show_all(color_patch_wrapper, dt_conf_get_bool("ui_last/colorpicker_large") == FALSE);
  gtk_box_pack_start(GTK_BOX(self->widget), color_patch_wrapper, FALSE, FALSE, 0);

  // The picker button, mode and statistic combo boxes
  GtkWidget *picker_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

  data->statistic_selector = dt_bauhaus_combobox_new_full(DT_ACTION(self), NULL, N_("statistic"),
                                                          _("select which statistic to show"),
                                                          data->statistic, (GtkCallback)_statistic_changed,
                                                          self, dt_lib_colorpicker_statistic_names);
  dt_bauhaus_combobox_set_entries_ellipsis(data->statistic_selector, PANGO_ELLIPSIZE_NONE);
  dt_bauhaus_widget_set_label(data->statistic_selector, NULL, NULL);
  gtk_widget_set_valign(data->statistic_selector, GTK_ALIGN_CENTER);
  gtk_box_pack_start(GTK_BOX(picker_row), data->statistic_selector, TRUE, TRUE, 0);

  data->color_mode_selector = dt_bauhaus_combobox_new_full(DT_ACTION(self), NULL, N_("color mode"),
                                                           _("select which color mode to use"),
                                                           data->model, (GtkCallback)_color_mode_changed, self,
                                                           dt_lib_colorpicker_model_names);
  dt_bauhaus_combobox_set_entries_ellipsis(data->color_mode_selector, PANGO_ELLIPSIZE_NONE);
  dt_bauhaus_widget_set_label(data->color_mode_selector, NULL, NULL);
  gtk_widget_set_valign(data->color_mode_selector, GTK_ALIGN_CENTER);
  gtk_box_pack_start(GTK_BOX(picker_row), data->color_mode_selector, TRUE, TRUE, 0);

  data->picker_button = dt_color_picker_new(NULL, DT_COLOR_PICKER_POINT_AREA, picker_row);
  gtk_widget_set_tooltip_text(data->picker_button, _("turn on color picker\nctrl+click or right-click to select an area"));
  gtk_widget_set_name(GTK_WIDGET(data->picker_button), "color-picker-button");
  g_signal_connect(G_OBJECT(data->picker_button), "toggled", G_CALLBACK(_picker_button_toggled), data);
  dt_action_define(DT_ACTION(self), NULL, N_("pick color"), data->picker_button, &dt_action_def_toggle);

  gtk_box_pack_start(GTK_BOX(self->widget), picker_row, TRUE, TRUE, 0);

  // The small sample, label and add button
  GtkWidget *sample_row_events = gtk_event_box_new();
  gtk_widget_add_events(sample_row_events, GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK);
  g_signal_connect(G_OBJECT(sample_row_events), "enter-notify-event", G_CALLBACK(_sample_enter_callback), &data->primary_sample);
  g_signal_connect(G_OBJECT(sample_row_events), "leave-notify-event", G_CALLBACK(_sample_leave_callback), &data->primary_sample);
  gtk_box_pack_start(GTK_BOX(self->widget), sample_row_events, TRUE, TRUE, 0);

  GtkWidget *sample_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_container_add(GTK_CONTAINER(sample_row_events), sample_row);

  data->primary_sample.color_patch = color_patch = gtk_drawing_area_new();
  gtk_widget_set_tooltip_text(color_patch, _("click to (un)hide large color patch"));
  gtk_widget_set_events(color_patch, GDK_BUTTON_PRESS_MASK);
  g_signal_connect(G_OBJECT(color_patch), "button-press-event", G_CALLBACK(_large_patch_toggle), data);
  g_signal_connect(G_OBJECT(color_patch), "draw", G_CALLBACK(_sample_draw_callback), &data->primary_sample);

  color_patch_wrapper = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name(color_patch_wrapper, "live-sample");
  gtk_box_pack_start(GTK_BOX(color_patch_wrapper), color_patch, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(sample_row), color_patch_wrapper, TRUE, TRUE, 0);

  GtkWidget *label = data->primary_sample.output_label = gtk_label_new("");
  gtk_label_set_justify(GTK_LABEL(label), GTK_JUSTIFY_CENTER);
  gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_START);
  gtk_label_set_selectable(GTK_LABEL(label), TRUE);
  dt_gui_add_class(label, "dt_monospace");
  gtk_widget_set_has_tooltip(label, TRUE);
  g_signal_connect(G_OBJECT(label), "query-tooltip", G_CALLBACK(_sample_tooltip_callback), &data->primary_sample);
  g_signal_connect(G_OBJECT(label), "size-allocate", G_CALLBACK(_label_size_allocate_callback), &data->primary_sample);
  gtk_box_pack_start(GTK_BOX(sample_row), label, TRUE, TRUE, 0);

  data->add_sample_button = dtgtk_button_new(dtgtk_cairo_paint_square_plus, 0, NULL);
  ;
  gtk_widget_set_sensitive(data->add_sample_button, FALSE);
  g_signal_connect(G_OBJECT(data->add_sample_button), "clicked", G_CALLBACK(_add_sample), self);
  dt_action_define(DT_ACTION(self), NULL, N_("add sample"), data->add_sample_button, &dt_action_def_button);
  gtk_box_pack_end(GTK_BOX(sample_row), data->add_sample_button, FALSE, FALSE, 0);

  // Adding the live samples section
  label = dt_ui_section_label_new(_("live samples"));
  gtk_box_pack_start(GTK_BOX(self->widget), label, TRUE, TRUE, 0);


  data->samples_container = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_pack_start(GTK_BOX(self->widget),
                     dt_ui_scroll_wrap(data->samples_container, 1, "plugins/darkroom/colorpicker/windowheight"), TRUE, TRUE, 0);

  data->display_samples_check_box = gtk_check_button_new_with_label(_("display samples on image/vectorscope"));
  gtk_label_set_ellipsize(GTK_LABEL(gtk_bin_get_child(GTK_BIN(data->display_samples_check_box))),
                          PANGO_ELLIPSIZE_MIDDLE);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(data->display_samples_check_box),
                               dt_conf_get_bool("ui_last/colorpicker_display_samples"));
  g_signal_connect(G_OBJECT(data->display_samples_check_box), "toggled",
                   G_CALLBACK(_display_samples_changed), NULL);
  gtk_box_pack_start(GTK_BOX(self->widget), data->display_samples_check_box, TRUE, TRUE, 0);

  GtkWidget *restrict_button = gtk_check_button_new_with_label(_("restrict scope to selection"));
  gtk_label_set_ellipsize(GTK_LABEL(gtk_bin_get_child(GTK_BIN(restrict_button))), PANGO_ELLIPSIZE_MIDDLE);
  gboolean restrict_histogram = dt_conf_get_bool("ui_last/colorpicker_restrict_histogram");
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(restrict_button), restrict_histogram);
  darktable.lib->proxy.colorpicker.restrict_histogram = restrict_histogram;
  g_signal_connect(G_OBJECT(restrict_button), "toggled", G_CALLBACK(_restrict_histogram_changed), NULL);
  gtk_box_pack_start(GTK_BOX(self->widget), restrict_button, TRUE, TRUE, 0);
}

void gui_cleanup(dt_lib_module_t *self)
{
  dt_iop_color_picker_reset(NULL, FALSE);

  // Clearing proxy functions
  darktable.lib->proxy.colorpicker.module = NULL;
  darktable.lib->proxy.colorpicker.update_panel = NULL;
  darktable.lib->proxy.colorpicker.update_samples = NULL;
  darktable.lib->proxy.colorpicker.set_sample_box_area = NULL;
  darktable.lib->proxy.colorpicker.set_sample_point = NULL;

  darktable.lib->proxy.colorpicker.primary_sample = NULL;
  while(darktable.lib->proxy.colorpicker.live_samples)
    _remove_sample(darktable.lib->proxy.colorpicker.live_samples->data);

  free(self->data);
  self->data = NULL;
}

void gui_reset(dt_lib_module_t *self)
{
  dt_lib_colorpicker_t *data = self->data;

  // First turn off any active picking, reprocessing if necessary
  if(darktable.lib->proxy.colorpicker.restrict_histogram
     && darktable.lib->proxy.colorpicker.picker_proxy)
  {
    dt_dev_invalidate_from_gui(darktable.develop);
  }
  dt_iop_color_picker_reset(NULL, FALSE);

  // Resetting the picked colors
  for(int i = 0; i < 3; i++)
  {
    for(int s = 0; s < DT_PICK_N; s++)
    {
      data->primary_sample.display[s][i] = 0.f;
      data->primary_sample.scope[s][i] = 0.f;
      data->primary_sample.lab[s][i] = 0.f;
    }
    data->primary_sample.label_rgb[i] = 0;
  }
  data->primary_sample.swatch.red = data->primary_sample.swatch.green
    = data->primary_sample.swatch.blue = 0.0;

  _update_picker_output(self);

  // Removing any live samples
  while(darktable.lib->proxy.colorpicker.live_samples)
    _remove_sample(darktable.lib->proxy.colorpicker.live_samples->data);

  // Resetting GUI elements
  dt_bauhaus_combobox_set(data->statistic_selector, 0);
  dt_bauhaus_combobox_set(data->color_mode_selector, 0);
  if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(data->display_samples_check_box)))
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(data->display_samples_check_box), FALSE);
  else
    dt_dev_invalidate_from_gui(darktable.develop);

  // redraw without a picker
  dt_control_queue_redraw_center();
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
