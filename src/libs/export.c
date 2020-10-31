/*
    This file is part of darktable,
    Copyright (C) 2009-2020 darktable developers.

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
#include "common/collection.h"
#include "common/colorspaces.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/file_location.h"
#include "common/imageio_module.h"
#include "common/styles.h"
#include "control/conf.h"
#include "control/control.h"
#include "control/jobs.h"
#include "control/signal.h"
#include "dtgtk/button.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "libs/lib.h"
#include "libs/lib_api.h"
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif
#include <gdk/gdkkeysyms.h>
#include <gtk/gtk.h>
#include <stdlib.h>
#include <ctype.h>

#include <glib.h>

DT_MODULE(7)

#define EXPORT_MAX_IMAGE_SIZE UINT16_MAX
#define CONFIG_PREFIX "plugins/lighttable/export/"

typedef struct dt_lib_export_t
{
  GtkWidget *dimensions_type, *print_dpi, *print_height, *print_width;
  GtkBox *print_size;
  GtkWidget *unit_label;
  GtkWidget *width, *height;
  GtkWidget *scale, *size_in_px;
  GtkBox *hbox1, *hbox2, *hbox_scale;
  GtkWidget *storage, *format;
  int format_lut[128];
  uint32_t max_allowed_width , max_allowed_height;
  GtkWidget *upscale, *profile, *intent, *style, *style_mode;
  GtkButton *export_button;
  GtkWidget *storage_extra_container, *format_extra_container;
  GtkWidget *high_quality;
  GtkWidget *export_masks;
  GtkWidget *metadata_button;
  char *metadata_export;
} dt_lib_export_t;


typedef enum dt_dimensions_type_t
{
  DT_DIMENSIONS_PIXELS = 0, // set dimensions exactly in pixels
  DT_DIMENSIONS_CM     = 1, // set dimensions from physical size in centimeters * DPI
  DT_DIMENSIONS_INCH   = 2,  // set dimensions from physical size in inch
  DT_DIMENSIONS_SCALE   = 3  // set dimensions by scale
} dt_dimensions_type_t;

char *dt_lib_export_metadata_configuration_dialog(char *list, const gboolean ondisk);
/** Updates the combo box and shows only the supported formats of current selected storage module */
static void _update_formats_combobox(dt_lib_export_t *d);
/** Sets the max dimensions based upon what storage and format supports */
static void _update_dimensions(dt_lib_export_t *d);
/** get the max output dimension supported by combination of storage and format.. */
static void _get_max_output_dimension(dt_lib_export_t *d, uint32_t *width, uint32_t *height);
static void _resync_print_dimensions(dt_lib_export_t *self);
static void _resync_pixel_dimensions(dt_lib_export_t *self);

#define INCH_TO_CM (2.54f)

static inline float pixels2cm(dt_lib_export_t *self, const uint32_t pix)
{
  const int dpi = atoi(gtk_entry_get_text(GTK_ENTRY(self->print_dpi)));
  return ((float)pix * INCH_TO_CM) / (float)dpi;
}

static inline float pixels2inch(dt_lib_export_t *self, const uint32_t pix)
{
  const int dpi = atoi(gtk_entry_get_text(GTK_ENTRY(self->print_dpi)));
  return (float)pix / (float)dpi;
}

static inline uint32_t cm2pixels(dt_lib_export_t *self, const float cm)
{
  const int dpi = atoi(gtk_entry_get_text(GTK_ENTRY(self->print_dpi)));
  return ceilf((cm * (float)dpi) / INCH_TO_CM);
}

static inline uint32_t inch2pixels(dt_lib_export_t *self, const float inch)
{
  const int dpi = atoi(gtk_entry_get_text(GTK_ENTRY(self->print_dpi)));
  return ceilf(inch * (float)dpi);
}

static inline uint32_t print2pixels(dt_lib_export_t *self, const float value)
{
  const dt_dimensions_type_t d_type = (dt_dimensions_type_t)dt_bauhaus_combobox_get(self->dimensions_type);
  switch(d_type)
  {
    case(DT_DIMENSIONS_PIXELS):
      return ceilf(value);
    case(DT_DIMENSIONS_CM):
      return cm2pixels(self, value);
    case(DT_DIMENSIONS_INCH):
      return inch2pixels(self, value);
    case(DT_DIMENSIONS_SCALE):
      ;
  }

  // should never run this
  return ceilf(value);
}

static inline float pixels2print(dt_lib_export_t *self, const uint32_t pix)
{
  const dt_dimensions_type_t d_type = (dt_dimensions_type_t)dt_bauhaus_combobox_get(self->dimensions_type);
  switch(d_type)
  {
    case(DT_DIMENSIONS_PIXELS):
      return (float)pix;
    case(DT_DIMENSIONS_CM):
      return pixels2cm(self, pix);
    case(DT_DIMENSIONS_INCH):
      return pixels2inch(self, pix);
    case(DT_DIMENSIONS_SCALE):
      ;
  }

  // should never run this
  return (float)pix;
}

const char *name(dt_lib_module_t *self)
{
  return _("export selected");
}

const char **views(dt_lib_module_t *self)
{
  static const char *v[] = {"lighttable", NULL};
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_RIGHT_CENTER;
}

static void _update(dt_lib_module_t *self)
{
  dt_lib_cancel_postponed_update(self);
  dt_lib_export_t *d = (dt_lib_export_t *)self->data;

  const GList *imgs = dt_view_get_images_to_act_on(TRUE, FALSE);
  const gboolean has_act_on = imgs != NULL;

  char *format_name = dt_conf_get_string(CONFIG_PREFIX "format_name");
  char *storage_name = dt_conf_get_string(CONFIG_PREFIX "storage_name");
  const int format_index = dt_imageio_get_index_of_format(dt_imageio_get_format_by_name(format_name));
  const int storage_index = dt_imageio_get_index_of_storage(dt_imageio_get_storage_by_name(storage_name));

  g_free(format_name);
  g_free(storage_name);

  gtk_widget_set_sensitive(GTK_WIDGET(d->export_button), has_act_on && format_index != -1 && storage_index != -1);
}

static void _image_selection_changed_callback(gpointer instance, dt_lib_module_t *self)
{
  _update(self);
}

static void _collection_updated_callback(gpointer instance, dt_collection_change_t query_change, gpointer imgs,
                                        int next, dt_lib_module_t *self)
{
  _update(self);
}

static void _mouse_over_image_callback(gpointer instance, dt_lib_module_t *self)
{
  dt_lib_queue_postponed_update(self, _update);
}

gboolean _is_int(double value)
{
    return (value == (int)value);
}

static void _scale_optim()
{
  double num=1, denum=1;
  dt_imageio_resizing_factor_get_and_parsing(&num, &denum);
  gchar *scale_str = dt_conf_get_string(CONFIG_PREFIX "resizing_factor");
  gchar _str[6] = "";

  gchar *pdiv = strchr(scale_str, '/');

  gchar scale_buf[64] = "";
  if (pdiv == NULL)
  {
    if (_is_int(num) && num > 0.0)
    {
      sprintf(_str, "%d", (int) num);
      g_strlcat(scale_buf, _str, sizeof(scale_buf));
    }
    else
    {
      g_strlcat(scale_buf, scale_str, sizeof(scale_buf));
    }
  }
  else if (pdiv-scale_str == 0)
  {
    if (_is_int(denum) && denum > 0.0)
    {
      sprintf(_str, "%d", (int) denum);
      g_strlcat(scale_buf, _str, sizeof(scale_buf));
    }
    else
    {
      g_strlcat(scale_buf, "1/", sizeof(scale_buf));
      g_strlcat(scale_buf, pdiv+1, sizeof(scale_buf));
    }
  }
  else
  {
    if (_is_int(num) && num > 0.0)
    {
      sprintf(_str, "%d", (int) num);
      g_strlcat(scale_buf, _str, sizeof(scale_buf));
    }
    else
    {
      g_strlcat(scale_buf, scale_str, sizeof(scale_buf));
    }
    g_strlcat(scale_buf, "/", sizeof(scale_buf));
    if (_is_int(denum) && denum > 0.0)
    {
      sprintf(_str, "%d", (int) denum);
      g_strlcat(scale_buf, _str, sizeof(scale_buf));
    }
    else
    {
      g_strlcat(scale_buf, pdiv+1, sizeof(scale_buf));
    }
  }
  dt_conf_set_string(CONFIG_PREFIX "resizing_factor", scale_buf);

  free(scale_str);
}

static void _export_button_clicked(GtkWidget *widget, dt_lib_export_t *d)
{
  char style[128] = { 0 };

  // Let's get the max dimension restriction if any...
  // TODO: pass the relevant values directly, not using the conf ...
  const uint32_t max_width = dt_conf_get_int(CONFIG_PREFIX "width");
  const uint32_t max_height = dt_conf_get_int(CONFIG_PREFIX "height");

  // get the format_name and storage_name settings which are plug-ins name and not necessary what is displayed on the combobox.
  // note that we cannot take directly the combobox entry index as depending on the storage some format are not listed.
  char *format_name = dt_conf_get_string(CONFIG_PREFIX "format_name");
  char *storage_name = dt_conf_get_string(CONFIG_PREFIX "storage_name");
  const int format_index = dt_imageio_get_index_of_format(dt_imageio_get_format_by_name(format_name));
  const int storage_index = dt_imageio_get_index_of_storage(dt_imageio_get_storage_by_name(storage_name));

  g_free(format_name);
  g_free(storage_name);

  if(format_index == -1)
  {
    dt_control_log("invalid format for export selected");
    return;
  }
  if(storage_index == -1)
  {
    dt_control_log("invalid storage for export selected");
    return;
  }

  char *confirm_message = NULL;
  dt_imageio_module_storage_t *mstorage = dt_imageio_get_storage();
  if(mstorage->ask_user_confirmation)
    confirm_message = mstorage->ask_user_confirmation(mstorage);
  if(confirm_message)
  {
    const GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
    GtkWidget *dialog = gtk_message_dialog_new(
        GTK_WINDOW(win), GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO,
        "%s", confirm_message);
#ifdef GDK_WINDOWING_QUARTZ
    dt_osx_disallow_fullscreen(dialog);
#endif

    gtk_window_set_title(GTK_WINDOW(dialog), _("export to disk"));
    const gint res = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    g_free(confirm_message);
    confirm_message = NULL;

    if(res != GTK_RESPONSE_YES)
    {
      return;
    }
  }

  const gboolean upscale = dt_conf_get_bool(CONFIG_PREFIX "upscale");
  const gboolean high_quality = dt_conf_get_bool(CONFIG_PREFIX "high_quality_processing");
  const gboolean export_masks = dt_conf_get_bool(CONFIG_PREFIX "export_masks");
  char *tmp = dt_conf_get_string(CONFIG_PREFIX "style");
  const gboolean style_append = dt_conf_get_bool(CONFIG_PREFIX "style_append");
  if(tmp)
  {
    g_strlcpy(style, tmp, sizeof(style));
    g_free(tmp);
  }
  dt_colorspaces_color_profile_type_t icc_type = dt_conf_get_int(CONFIG_PREFIX "icctype");
  gchar *icc_filename = dt_conf_get_string(CONFIG_PREFIX "iccprofile");
  dt_iop_color_intent_t icc_intent = dt_conf_get_int(CONFIG_PREFIX "iccintent");

  GList *list = g_list_copy((GList *)dt_view_get_images_to_act_on(TRUE, TRUE));
  dt_control_export(list, max_width, max_height, format_index, storage_index, high_quality, upscale, export_masks,
                    style, style_append, icc_type, icc_filename, icc_intent, d->metadata_export);

  g_free(icc_filename);

  _scale_optim();
  gtk_entry_set_text(GTK_ENTRY(d->scale), dt_conf_get_string(CONFIG_PREFIX "resizing_factor"));
}

static void _scale_changed(GtkEntry *spin, dt_lib_export_t *d)
{
  const char *validSign = ",.0123456789";
  const gchar *value = gtk_entry_get_text(spin);

  int len = sizeof(value);
  int i, j = 0, idec = 0, idiv = 0, pdiv = 0;
  char new_value[30] = "";

  for (i = 0; i < len; i++)
  {
    char *val = strchr(validSign, value[i]);
    if (val == NULL)
    {
      if (idiv==0)
      {
        if (i == 0)
        {
          new_value[j++] = '1';
        }
        else
        {
          if (atof(value) == 0.0)
          {
            new_value[0] = '1';
          }
          idec = 0;
          idiv = 1;
          new_value[j++] = '/';
          pdiv = j;
        }
      }
    }
    else if ((val[0] == '.') || (val[0] == ','))
    {
      if (idec == 0)
      {
        if ((i == 0) || (i == pdiv))
        {
          new_value[j++] = '0';
        }
        else
        {
          idec = 1;
          new_value[j++] = value[i];
        }
      }
    }
    else if (value[i] == '\0')
    {
      break;
    }
    else
    {
      new_value[j++] = value[i];
    }
  }
  dt_conf_set_string(CONFIG_PREFIX "resizing_factor", new_value);
  gtk_entry_set_text(spin, new_value);
}

static void _width_changed(GtkEditable *entry, gpointer user_data);
static void _height_changed(GtkEditable *entry, gpointer user_data);

static void _scale_mdlclick(GtkEntry *spin, GdkEventButton *event, dt_lib_export_t *d)
{
  if (event->button == 2)
  {
    dt_conf_set_string(CONFIG_PREFIX "resizing_factor", "1");
    g_signal_handlers_block_by_func(spin, _scale_changed, d);
    gtk_entry_set_text(GTK_ENTRY(spin), "1");
    g_signal_handlers_unblock_by_func(spin, _scale_changed, d);
  }
  else
{
    _scale_changed(spin, d);
  }
}

static void _widht_mdlclick(GtkEntry *spin, GdkEventButton *event, gpointer user_data)
{
  if (event->button == 2)
{
    dt_conf_set_int(CONFIG_PREFIX "width", 0);
    g_signal_handlers_block_by_func(spin, _width_changed, user_data);
    gtk_entry_set_text(GTK_ENTRY(spin), "0");
    g_signal_handlers_unblock_by_func(spin, _width_changed, user_data);
  }
  else
  {
    _width_changed(GTK_EDITABLE(spin), user_data);
  }
}

static void _height_mdlclick(GtkEntry *spin, GdkEventButton *event, gpointer user_data)
{
  if (event->button == 2)
{
    dt_conf_set_int(CONFIG_PREFIX "height", 0);
    g_signal_handlers_block_by_func(spin, _height_changed, user_data);
    gtk_entry_set_text(GTK_ENTRY(spin), "0");
    g_signal_handlers_unblock_by_func(spin, _height_changed, user_data);
  }
  else
  {
    _height_changed(GTK_EDITABLE(spin), user_data);
  }
}

static void _size_in_px_update(dt_lib_export_t *d)
{
  const dt_dimensions_type_t d_type = (dt_dimensions_type_t)dt_bauhaus_combobox_get(d->dimensions_type);

  if ((d_type == DT_DIMENSIONS_SCALE) || (d_type == DT_DIMENSIONS_PIXELS))
  {
    gtk_label_set_text(GTK_LABEL(d->size_in_px), "");
  }
  else
  {
    gchar size_in_px_txt[25];
    sprintf(size_in_px_txt, "that is equal %s x %s px", gtk_entry_get_text(GTK_ENTRY(d->width)), gtk_entry_get_text(GTK_ENTRY(d->height)));
    gtk_label_set_text(GTK_LABEL(d->size_in_px), size_in_px_txt);
  }
}

void _set_dimensions(dt_lib_export_t *d, uint32_t max_width, uint32_t max_height)
{
  gchar *max_width_char = g_strdup_printf("%d", max_width);
  gchar *max_height_char = g_strdup_printf("%d", max_height);

  ++darktable.gui->reset;
  gtk_entry_set_text(GTK_ENTRY(d->width), max_width_char);
  gtk_entry_set_text(GTK_ENTRY(d->height), max_height_char);
  _size_in_px_update(d);
  --darktable.gui->reset;

  dt_conf_set_int(CONFIG_PREFIX "width", max_width);
  dt_conf_set_int(CONFIG_PREFIX "height", max_height);

  g_free(max_width_char);
  g_free(max_height_char);
  _resync_print_dimensions(d);
}


void _print_size_update_display(dt_lib_export_t *self)
{
  const dt_dimensions_type_t d_type = (dt_dimensions_type_t)dt_bauhaus_combobox_get(self->dimensions_type);

  if(d_type == DT_DIMENSIONS_PIXELS)
  {
    gtk_widget_set_visible(GTK_WIDGET(self->print_size), FALSE);
    gtk_widget_set_sensitive(GTK_WIDGET(self->width), TRUE);
    gtk_widget_set_sensitive(GTK_WIDGET(self->height), TRUE);
  }
  else
  {
    if (strcmp(dt_conf_get_string(CONFIG_PREFIX "resizing"), "scaling") != 0)
    {
      // max size
      gtk_widget_set_visible(GTK_WIDGET(self->print_size), TRUE);
    }
    gtk_widget_set_sensitive(GTK_WIDGET(self->width), FALSE);
    gtk_widget_set_sensitive(GTK_WIDGET(self->height), FALSE);

    char str[20];
    if(d_type == DT_DIMENSIONS_CM)
      g_strlcpy(str, _("cm"), sizeof(str));
    else // DT_DIMENSIONS_INCH
      g_strlcpy(str, C_("unit", "in"), sizeof(str));

    g_strlcat(str, " @", sizeof(str));
    gtk_label_set_text(GTK_LABEL(self->unit_label), str);
  }
}

void gui_reset(dt_lib_module_t *self)
{
  // make sure we don't do anything useless:
  if(!dt_control_running()) return;
  dt_lib_export_t *d = (dt_lib_export_t *)self->data;
  dt_bauhaus_combobox_set(d->dimensions_type, dt_conf_get_int(CONFIG_PREFIX "dimensions_type"));
  _print_size_update_display(d);

  // Set storage
  gchar *storage_name = dt_conf_get_string(CONFIG_PREFIX "storage_name");
  const int storage_index = dt_imageio_get_index_of_storage(dt_imageio_get_storage_by_name(storage_name));
  g_free(storage_name);
  dt_bauhaus_combobox_set(d->storage, storage_index);

  dt_bauhaus_combobox_set(d->upscale, dt_conf_get_bool(CONFIG_PREFIX "upscale") ? 1 : 0);
  dt_bauhaus_combobox_set(d->high_quality, dt_conf_get_bool(CONFIG_PREFIX "high_quality_processing") ? 1 : 0);
  dt_bauhaus_combobox_set(d->export_masks, dt_conf_get_bool(CONFIG_PREFIX "export_masks") ? 1 : 0);

  dt_bauhaus_combobox_set(d->intent, dt_conf_get_int(CONFIG_PREFIX "iccintent") + 1);

  // iccprofile
  int icctype = dt_conf_get_int(CONFIG_PREFIX "icctype");
  gchar *iccfilename = dt_conf_get_string(CONFIG_PREFIX "iccprofile");
  dt_bauhaus_combobox_set(d->profile, 0);
  if(icctype != DT_COLORSPACE_NONE)
  {
    for(GList *profiles = darktable.color_profiles->profiles; profiles; profiles = g_list_next(profiles))
    {
      dt_colorspaces_color_profile_t *pp = (dt_colorspaces_color_profile_t *)profiles->data;
      if(pp->out_pos > -1 &&
         icctype == pp->type && (icctype != DT_COLORSPACE_FILE || !strcmp(iccfilename, pp->filename)))
      {
        dt_bauhaus_combobox_set(d->profile, pp->out_pos + 1);
        break;
      }
    }
  }

  g_free(iccfilename);

  // style
  // set it to none if the var is not set or the style doesn't exist anymore
  gboolean rc = FALSE;
  gchar *style = dt_conf_get_string(CONFIG_PREFIX "style");
  if(style != NULL)
  {
    rc = dt_bauhaus_combobox_set_from_text(d->style, style);
    if(rc == FALSE) dt_bauhaus_combobox_set(d->style, 0);
    g_free(style);
  }
  else
    dt_bauhaus_combobox_set(d->style, 0);

  // style mode to overwrite as it was the initial behavior
  dt_bauhaus_combobox_set(d->style_mode, dt_conf_get_bool(CONFIG_PREFIX "style_append"));

  gtk_widget_set_sensitive(GTK_WIDGET(d->style_mode), dt_bauhaus_combobox_get(d->style)==0?FALSE:TRUE);

  // export metadata presets
  g_free(d->metadata_export);
  d->metadata_export = dt_lib_export_metadata_get_conf();

  dt_imageio_module_format_t *mformat = dt_imageio_get_format();
  if(mformat) mformat->gui_reset(mformat);
  dt_imageio_module_storage_t *mstorage = dt_imageio_get_storage();
  if(mstorage) mstorage->gui_reset(mstorage);

  _update(self);
}

static void set_format_by_name(dt_lib_export_t *d, const char *name)
{
  // Find the selected format plugin among all existing plugins
  dt_imageio_module_format_t *module = NULL;
  for(GList *it = darktable.imageio->plugins_format; it; it = g_list_next(it))
  {
    if(g_strcmp0(((dt_imageio_module_format_t *)it->data)->name(), name) == 0
        || g_strcmp0(((dt_imageio_module_format_t *)it->data)->plugin_name, name) == 0)
    {
      module = (dt_imageio_module_format_t *)it->data;
      break;
    }
  }

  if(!module)
  {
    gtk_widget_hide(d->format_extra_container);
    return;
  }
  else if(module->widget)
  {
    gtk_widget_show_all(d->format_extra_container);
    gtk_stack_set_visible_child(GTK_STACK(d->format_extra_container), module->widget);
  }
  else
  {
    gtk_widget_hide(d->format_extra_container);
  }

  // Store the new format
  dt_conf_set_string(CONFIG_PREFIX "format_name", module->plugin_name);

  if(dt_bauhaus_combobox_set_from_text(d->format, module->name()) == FALSE)
    dt_bauhaus_combobox_set(d->format, 0);

  // Let's also update combination of storage/format dimension restrictions
  _update_dimensions(d);

  // only some modules support export of masks
  // set it to 0 when insensitive and restore when making it sensitive again. this doesn't survive dt restarts.
  const gboolean support_layers = (module->flags(NULL) & FORMAT_FLAGS_SUPPORT_LAYERS) == FORMAT_FLAGS_SUPPORT_LAYERS;
  const gboolean is_enabled = gtk_widget_get_sensitive(d->export_masks);
  if(support_layers && !is_enabled)
  {
    // combobox was disabled and shall be enabled. we want to restore the old setting.
    const gboolean export_masks = dt_conf_get_bool(CONFIG_PREFIX "export_masks");
    gtk_widget_set_sensitive(d->export_masks, TRUE);
    dt_bauhaus_combobox_set(d->export_masks, export_masks ? 1 : 0);
  }
  else if(!support_layers && is_enabled)
  {
    // combobox was enabled but shall be disabled. we want to save the current setting.
    const int export_masks = dt_bauhaus_combobox_get(d->export_masks);
    dt_bauhaus_combobox_set(d->export_masks, 0);
    dt_conf_set_bool(CONFIG_PREFIX "export_masks", export_masks == 1);
    gtk_widget_set_sensitive(d->export_masks, FALSE);
  }
}

static void _format_changed(GtkWidget *widget, dt_lib_export_t *d)
{
  const gchar *name = dt_bauhaus_combobox_get_text(d->format);
  g_signal_handlers_block_by_func(widget, _format_changed, d);
  set_format_by_name(d, name);
  g_signal_handlers_unblock_by_func(widget, _format_changed, d);
}

static void _get_max_output_dimension(dt_lib_export_t *d, uint32_t *width, uint32_t *height)
{
  gchar *storage_name = dt_conf_get_string(CONFIG_PREFIX "storage_name");
  dt_imageio_module_storage_t *storage = dt_imageio_get_storage_by_name(storage_name);
  g_free(storage_name);
  char *format_name = dt_conf_get_string(CONFIG_PREFIX "format_name");
  dt_imageio_module_format_t *format = dt_imageio_get_format_by_name(format_name);
  g_free(format_name);
  if(storage && format)
  {
    uint32_t fw, fh, sw, sh;
    fw = fh = sw = sh = 0; // We are all equals!!!
    storage->dimension(storage, NULL, &sw, &sh);
    format->dimension(format, NULL, &fw, &fh);

    if(sw == 0 || fw == 0)
      *width = sw > fw ? sw : fw;
    else
      *width = sw < fw ? sw : fw;

    if(sh == 0 || fh == 0)
      *height = sh > fh ? sh : fh;
    else
      *height = sh < fh ? sh : fh;
  }
}

static void _validate_dimensions(dt_lib_export_t *d)
{
  //reset dimensions to previously stored value if they exceed the maximum
  uint32_t width = atoi(gtk_entry_get_text(GTK_ENTRY(d->width)));
  uint32_t height = atoi(gtk_entry_get_text(GTK_ENTRY(d->height)));
  if(width > d->max_allowed_width || height > d->max_allowed_height)
  {
    width = width > d->max_allowed_width ? dt_conf_get_int(CONFIG_PREFIX "width") : width;
    height = height > d->max_allowed_height ? dt_conf_get_int(CONFIG_PREFIX "height") : height;
    _set_dimensions(d, width, height);
  }
}

static void _update_dimensions(dt_lib_export_t *d)
{
  uint32_t max_w = 0, max_h = 0;
  _get_max_output_dimension(d, &max_w, &max_h);
  d->max_allowed_width = max_w > 0 ? max_w : EXPORT_MAX_IMAGE_SIZE;
  d->max_allowed_height = max_h > 0 ? max_h : EXPORT_MAX_IMAGE_SIZE;
  _validate_dimensions(d);
}

static void set_storage_by_name(dt_lib_export_t *d, const char *name)
{
  int k = -1;
  GList *it = g_list_first(darktable.imageio->plugins_storage);
  dt_imageio_module_storage_t *module = NULL;

  if(it != NULL)
    do
    {
      k++;
      if(strcmp(((dt_imageio_module_storage_t *)it->data)->name(((dt_imageio_module_storage_t *)it->data)),
                name) == 0 || strcmp(((dt_imageio_module_storage_t *)it->data)->plugin_name, name) == 0)
      {
        module = (dt_imageio_module_storage_t *)it->data;
        break;
      }
    } while((it = g_list_next(it)));

  if(!module)
  {
    gtk_widget_hide(d->storage_extra_container);
    return;
  }
  else if(module->widget)
  {
    gtk_widget_show_all(d->storage_extra_container);
    gtk_stack_set_visible_child(GTK_STACK(d->storage_extra_container),module->widget);
  }
  else
  {
    gtk_widget_hide(d->storage_extra_container);
  }
  dt_bauhaus_combobox_set(d->storage, k);
  dt_conf_set_string(CONFIG_PREFIX "storage_name", module->plugin_name);


  // Check if plugin recommends a max dimension and set
  // if not implemented the stored conf values are used..
  uint32_t w = 0, h = 0;
  module->recommended_dimension(module, NULL, &w, &h);

  const uint32_t cw = dt_conf_get_int(CONFIG_PREFIX "width");
  const uint32_t ch = dt_conf_get_int(CONFIG_PREFIX "height");

  // If user's selected value is below the max, select it
  if(w > cw || w == 0) w = cw;
  if(h > ch || h == 0) h = ch;

  // Set the recommended dimension
  _set_dimensions(d, w, h);

  // Let's update formats combobox with supported formats of selected storage module...
  _update_formats_combobox(d);

  // Lets try to set selected format if fail select first in list..
  gchar *format_name = dt_conf_get_string(CONFIG_PREFIX "format_name");
  dt_imageio_module_format_t *format = dt_imageio_get_format_by_name(format_name);
  g_free(format_name);
  if(format == NULL || dt_bauhaus_combobox_set_from_text(d->format, format->name()) == FALSE)
    dt_bauhaus_combobox_set(d->format, 0);
}

static void _storage_changed(GtkWidget *widget, dt_lib_export_t *d)
{
  const gchar *name = dt_bauhaus_combobox_get_text(d->storage);
  g_signal_handlers_block_by_func(widget, _storage_changed, d);
  if(name) set_storage_by_name(d, name);
  g_signal_handlers_unblock_by_func(widget, _storage_changed, d);
}

static void _profile_changed(GtkWidget *widget, dt_lib_export_t *d)
{
  int pos = dt_bauhaus_combobox_get(widget);
  if(pos > 0)
  {
    pos--;
    for(GList *profiles = darktable.color_profiles->profiles; profiles; profiles = g_list_next(profiles))
    {
      dt_colorspaces_color_profile_t *pp = (dt_colorspaces_color_profile_t *)profiles->data;
      if(pp->out_pos == pos)
      {
        dt_conf_set_int(CONFIG_PREFIX "icctype", pp->type);
        if(pp->type == DT_COLORSPACE_FILE)
          dt_conf_set_string(CONFIG_PREFIX "iccprofile", pp->filename);
        else
          dt_conf_set_string(CONFIG_PREFIX "iccprofile", "");
        return;
      }
    }
  }
  dt_conf_set_int(CONFIG_PREFIX "icctype", DT_COLORSPACE_NONE);
  dt_conf_set_string(CONFIG_PREFIX "iccprofile", "");
}

static void _dimensions_type_changed(GtkWidget *widget, dt_lib_export_t *d)
{
  if(darktable.gui->reset) return;

  const dt_dimensions_type_t d_type = (dt_dimensions_type_t)dt_bauhaus_combobox_get(widget);

  dt_conf_set_int(CONFIG_PREFIX "dimensions_type", d_type);
  if(d_type != DT_DIMENSIONS_SCALE)
  {
    if(d_type != DT_DIMENSIONS_PIXELS)
    {
      gtk_widget_hide(GTK_WIDGET(d->hbox1));
      gtk_widget_show(GTK_WIDGET(d->hbox2));
      gtk_widget_hide(GTK_WIDGET(d->scale));
      _resync_print_dimensions(d);
    }
    else
    {
      gtk_widget_show(GTK_WIDGET(d->hbox1));
      gtk_widget_hide(GTK_WIDGET(d->hbox2));
      gtk_widget_hide(GTK_WIDGET(d->scale));
    }
    dt_conf_set_string(CONFIG_PREFIX "resizing", "max_size");
    _print_size_update_display(d);
  }
  else
  {
    gtk_widget_show(GTK_WIDGET(d->scale));
    gtk_widget_hide(GTK_WIDGET(d->hbox1));
    gtk_widget_hide(GTK_WIDGET(d->hbox2));
    dt_conf_set_string(CONFIG_PREFIX "resizing", "scaling");
  }
  _size_in_px_update(d);
}

static void _resync_print_dimensions(dt_lib_export_t *self)
{
  if(darktable.gui->reset) return;

  const uint32_t width = dt_conf_get_int(CONFIG_PREFIX "width");
  const uint32_t height = dt_conf_get_int(CONFIG_PREFIX "height");
  const int dpi = atoi(gtk_entry_get_text(GTK_ENTRY(self->print_dpi)));

  const float p_width = pixels2print(self, width);
  const float p_height = pixels2print(self, height);

  ++darktable.gui->reset;
  gchar *pwidth = g_strdup_printf("%.2f", p_width);
  gchar *pheight = g_strdup_printf("%.2f", p_height);
  gchar *pdpi = g_strdup_printf("%d", dpi);
  gtk_entry_set_text(GTK_ENTRY(self->print_width), pwidth);
  gtk_entry_set_text(GTK_ENTRY(self->print_height), pheight);
  gtk_entry_set_text(GTK_ENTRY(self->print_dpi), pdpi);
  g_free(pwidth);
  g_free(pheight);
  g_free(pdpi);
  --darktable.gui->reset;
}

static void _resync_pixel_dimensions(dt_lib_export_t *self)
{
  if(darktable.gui->reset) return;

  const float p_width = atof(gtk_entry_get_text(GTK_ENTRY(self->print_width)));
  const float p_height = atof(gtk_entry_get_text(GTK_ENTRY(self->print_height)));

  const uint32_t width = print2pixels(self, p_width);
  const uint32_t height = print2pixels(self, p_height);

  dt_conf_set_int(CONFIG_PREFIX "width", width);
  dt_conf_set_int(CONFIG_PREFIX "height", height);

  ++darktable.gui->reset;
  gchar *pwidth = g_strdup_printf("%d", width);
  gchar *pheight = g_strdup_printf("%d", height);
  gtk_entry_set_text(GTK_ENTRY(self->width), pwidth);
  gtk_entry_set_text(GTK_ENTRY(self->height), pheight);
  g_free(pwidth);
  g_free(pheight);
  --darktable.gui->reset;
}

static void _width_changed(GtkEditable *entry, gpointer user_data)
{
  if(darktable.gui->reset) return;

  dt_lib_export_t *d = (dt_lib_export_t *)user_data;
  const uint32_t width = atoi(gtk_entry_get_text(GTK_ENTRY(d->width)));
  dt_conf_set_int(CONFIG_PREFIX "width", width);
}

static void _print_width_changed(GtkEditable *entry, gpointer user_data)
{
  if(darktable.gui->reset) return;

  dt_lib_export_t *d = (dt_lib_export_t *)user_data;

  const float p_width = atof(gtk_entry_get_text(GTK_ENTRY(d->print_width)));
  const uint32_t width = print2pixels(d, p_width);
  dt_conf_set_int(CONFIG_PREFIX "width", width);

  ++darktable.gui->reset;
  gchar *pwidth = g_strdup_printf("%d", width);
  gtk_entry_set_text(GTK_ENTRY(d->width), pwidth);
  g_free(pwidth);
  _size_in_px_update(d);
  --darktable.gui->reset;
}

static void _height_changed(GtkEditable *entry, gpointer user_data)
{
  if(darktable.gui->reset) return;

  dt_lib_export_t *d = (dt_lib_export_t *)user_data;
  const uint32_t height = atoi(gtk_entry_get_text(GTK_ENTRY(d->height)));
  dt_conf_set_int(CONFIG_PREFIX "height", height);
}

static void _print_height_changed(GtkEditable *entry, gpointer user_data)
{
  if(darktable.gui->reset) return;

  dt_lib_export_t *d = (dt_lib_export_t *)user_data;

  const float p_height = atof(gtk_entry_get_text(GTK_ENTRY(d->print_height)));
  const uint32_t height = print2pixels(d, p_height);
  dt_conf_set_int(CONFIG_PREFIX "height", height);

  ++darktable.gui->reset;
  gchar *pheight = g_strdup_printf("%d", height);
  gtk_entry_set_text(GTK_ENTRY(d->height), pheight);
  g_free(pheight);
  _size_in_px_update(d);
  --darktable.gui->reset;
}

static void _print_dpi_changed(GtkWidget *widget, gpointer user_data)
{
  if(darktable.gui->reset) return;

  dt_lib_export_t *d = (dt_lib_export_t *)user_data;
  const int dpi = atoi(gtk_entry_get_text(GTK_ENTRY(d->print_dpi)));

  dt_conf_set_int(CONFIG_PREFIX "print_dpi", dpi);

  _resync_pixel_dimensions(d);
  _size_in_px_update(d);
}

static void _callback_bool(GtkWidget *widget, gpointer user_data)
{
  const char *key = (const char *)user_data;
  dt_conf_set_bool(key, dt_bauhaus_combobox_get(widget) == 1);
}

static void _intent_changed(GtkWidget *widget, dt_lib_export_t *d)
{
  const int pos = dt_bauhaus_combobox_get(widget);
  dt_conf_set_int(CONFIG_PREFIX "iccintent", pos - 1);
}

static void _style_changed(GtkWidget *widget, dt_lib_export_t *d)
{
  if(dt_bauhaus_combobox_get(d->style) == 0)
  {
    dt_conf_set_string(CONFIG_PREFIX "style", "");
    gtk_widget_set_sensitive(GTK_WIDGET(d->style_mode), FALSE);
  }
  else
  {
    const gchar *style = dt_bauhaus_combobox_get_text(d->style);
    dt_conf_set_string(CONFIG_PREFIX "style", style);
    gtk_widget_set_sensitive(GTK_WIDGET(d->style_mode), TRUE);
  }
}

int position()
{
  return 0;
}

static void _update_formats_combobox(dt_lib_export_t *d)
{
  // Clear format combo box
  dt_bauhaus_combobox_clear(d->format);

  // Get current selected storage
  gchar *storage_name = dt_conf_get_string(CONFIG_PREFIX "storage_name");
  dt_imageio_module_storage_t *storage = dt_imageio_get_storage_by_name(storage_name);
  g_free(storage_name);

  // Add supported formats to combobox
  GList *it = darktable.imageio->plugins_format;
  gboolean empty = TRUE;
  while(it)
  {
    dt_imageio_module_format_t *format = (dt_imageio_module_format_t *)it->data;
    if(storage->supported(storage, format))
    {
      dt_bauhaus_combobox_add(d->format, format->name());
      empty = FALSE;
    }

    it = g_list_next(it);
  }

  gtk_widget_set_sensitive(d->format, !empty);
}

static void _on_storage_list_changed(gpointer instance, dt_lib_module_t *self)
{
  dt_lib_export_t *d = self->data;
  dt_imageio_module_storage_t *storage = dt_imageio_get_storage();
  dt_bauhaus_combobox_clear(d->storage);

  GList *children, *iter;

  children = gtk_container_get_children(GTK_CONTAINER(d->storage_extra_container));
  for(iter = children; iter != NULL; iter = g_list_next(iter))
    gtk_container_remove(GTK_CONTAINER(d->storage_extra_container),GTK_WIDGET(iter->data));
  g_list_free(children);


  GList *it = darktable.imageio->plugins_storage;
  if(it != NULL) do
  {
    dt_imageio_module_storage_t *module = (dt_imageio_module_storage_t *)it->data;
    dt_bauhaus_combobox_add(d->storage, module->name(module));
    if(module->widget)
    {
      gtk_container_add(GTK_CONTAINER(d->storage_extra_container), module->widget);
    }
  } while((it = g_list_next(it)));
  dt_bauhaus_combobox_set(d->storage, dt_imageio_get_index_of_storage(storage));
}

static void _lib_export_styles_changed_callback(gpointer instance, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_export_t *d = self->data;

  dt_bauhaus_combobox_clear(d->style);
  dt_bauhaus_combobox_add(d->style, _("none"));

  GList *styles = dt_styles_get_list("");
  while(styles)
  {
    const dt_style_t *style = (dt_style_t *)styles->data;
    dt_bauhaus_combobox_add(d->style, style->name);
    styles = g_list_next(styles);
  }
  dt_bauhaus_combobox_set(d->style, 0);

  g_list_free_full(styles, dt_style_free);
}

static void _metadata_export_clicked(GtkComboBox *widget, dt_lib_export_t *d)
{
  const gchar *name = dt_bauhaus_combobox_get_text(d->storage);
  const gboolean ondisk = name && !g_strcmp0(name, _("file on disk")); // FIXME: NO!!!!!one!
  d->metadata_export = dt_lib_export_metadata_configuration_dialog(d->metadata_export, ondisk);
}

void gui_init(dt_lib_module_t *self)
{
  dt_lib_export_t *d = (dt_lib_export_t *)malloc(sizeof(dt_lib_export_t));
  self->timeout_handle = 0;
  self->data = (void *)d;
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  dt_gui_add_help_link(self->widget, dt_get_help_url(self->plugin_name));

  GtkWidget *label = dt_ui_section_label_new(_("storage options"));
  GtkStyleContext *context = gtk_widget_get_style_context(GTK_WIDGET(label));
  gtk_style_context_add_class(context, "section_label_top");
  gtk_box_pack_start(GTK_BOX(self->widget), label, FALSE, TRUE, 0);
  dt_gui_add_help_link(self->widget, "export_selected.html#export_selected_usage");

  d->storage = dt_bauhaus_combobox_new(NULL);
  dt_bauhaus_widget_set_label(d->storage, NULL, N_("target storage"));
  gtk_box_pack_start(GTK_BOX(self->widget), d->storage, FALSE, TRUE, 0);

  // add all storage widgets to the stack widget
  d->storage_extra_container = gtk_stack_new();
  gtk_stack_set_homogeneous(GTK_STACK(d->storage_extra_container),FALSE);
  gtk_box_pack_start(GTK_BOX(self->widget), d->storage_extra_container, FALSE, TRUE, 0);
  GList *it = g_list_first(darktable.imageio->plugins_storage);
  if(it != NULL) do
  {
    const dt_imageio_module_storage_t *module = (dt_imageio_module_storage_t *)it->data;
    dt_bauhaus_combobox_add(d->storage, module->name(module));
    if(module->widget)
    {
      gtk_container_add(GTK_CONTAINER(d->storage_extra_container), module->widget);
    }
  } while((it = g_list_next(it)));

  // postponed so we can do the two steps in one loop
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_IMAGEIO_STORAGE_CHANGE,
                            G_CALLBACK(_on_storage_list_changed), self);
  g_signal_connect(G_OBJECT(d->storage), "value-changed", G_CALLBACK(_storage_changed), (gpointer)d);

  label = dt_ui_section_label_new(_("format options"));
  gtk_box_pack_start(GTK_BOX(self->widget), label, FALSE, TRUE, 0);
  dt_gui_add_help_link(self->widget, "export_selected.html#export_selected_usage");

  d->format = dt_bauhaus_combobox_new(NULL);
  dt_bauhaus_widget_set_label(d->format, NULL, N_("file format"));
  gtk_box_pack_start(GTK_BOX(self->widget), d->format, FALSE, TRUE, 0);
  g_signal_connect(G_OBJECT(d->format), "value-changed", G_CALLBACK(_format_changed), (gpointer)d);

  // add all format widgets to the stack widget
  d->format_extra_container = gtk_stack_new();
  gtk_stack_set_homogeneous(GTK_STACK(d->format_extra_container),FALSE);
  gtk_box_pack_start(GTK_BOX(self->widget), d->format_extra_container, FALSE, TRUE, 0);
  it = g_list_first(darktable.imageio->plugins_format);
  if(it != NULL) do
  {
    dt_imageio_module_format_t *module = (dt_imageio_module_format_t *)it->data;
    if(module->widget)
    {
      gtk_container_add(GTK_CONTAINER(d->format_extra_container), module->widget);
    }
  } while((it = g_list_next(it)));

  label = dt_ui_section_label_new(_("global options"));
  gtk_box_pack_start(GTK_BOX(self->widget), label, FALSE, TRUE, 0);
  dt_gui_add_help_link(self->widget, "export_selected.html#export_selected_usage");

  d->dimensions_type = dt_bauhaus_combobox_new(NULL);
  dt_bauhaus_widget_set_label(d->dimensions_type, NULL, N_("set size"));
  gtk_widget_set_tooltip_text(d->dimensions_type, _("choose a method for setting the output size"));
  dt_bauhaus_combobox_add(d->dimensions_type, _("in pixels (for file)"));
  dt_bauhaus_combobox_add(d->dimensions_type, _("in cm (for print)"));
  dt_bauhaus_combobox_add(d->dimensions_type, _("in inch (for print)"));
  dt_bauhaus_combobox_add(d->dimensions_type, _("by scale (for file)"));
  dt_bauhaus_combobox_set(d->dimensions_type, dt_conf_get_int(CONFIG_PREFIX "dimensions_type"));

  d->print_width = gtk_entry_new();
  gtk_widget_set_tooltip_text(d->print_width, _("maximum output width limit.\n"
                                                "click middle mouse button to reset to 0."));
  gtk_entry_set_width_chars(GTK_ENTRY(d->print_width), 5);
  d->print_height = gtk_entry_new();
  gtk_widget_set_tooltip_text(d->print_height, _("maximum output height limit.\n"
                                                 "click middle mouse button to reset to 0."));
  gtk_entry_set_width_chars(GTK_ENTRY(d->print_height), 5);
  d->print_dpi = gtk_entry_new();
  gtk_widget_set_tooltip_text(d->print_dpi, _("resolution in dot per inch"));
  gtk_entry_set_width_chars(GTK_ENTRY(d->print_dpi), 4);
  char *dpi = dt_conf_get_string(CONFIG_PREFIX "print_dpi");
  gtk_entry_set_text(GTK_ENTRY(d->print_dpi), dpi);
  g_free(dpi);

  dt_gui_key_accel_block_on_focus_connect(d->print_width);
  dt_gui_key_accel_block_on_focus_connect(d->print_height);
  dt_gui_key_accel_block_on_focus_connect(d->print_dpi);

  d->width = gtk_entry_new();
  gtk_widget_set_tooltip_text(d->width, _("maximum output width limit.\n"
                                          "click middle mouse button to reset to 0."));
  gtk_entry_set_width_chars(GTK_ENTRY(d->width), 5);
  d->height = gtk_entry_new();
  gtk_widget_set_tooltip_text(d->height, _("maximum output height limit.\n"
                                           "click middle mouse button to reset to 0."));
  gtk_entry_set_width_chars(GTK_ENTRY(d->height), 5);

  gtk_widget_add_events(d->width, GDK_BUTTON_PRESS_MASK);
  gtk_widget_add_events(d->height, GDK_BUTTON_PRESS_MASK);

  dt_gui_key_accel_block_on_focus_connect(d->width);
  dt_gui_key_accel_block_on_focus_connect(d->height);

  d->hbox2 = d->print_size = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 3));
  gtk_box_pack_start(d->hbox2, d->print_width, TRUE, TRUE, 0);
  gtk_box_pack_start(d->hbox2, gtk_label_new(_("x")), FALSE, FALSE, 0);
  gtk_box_pack_start(d->hbox2, d->print_height, TRUE, TRUE, 0);
  d->unit_label = gtk_label_new(_("cm"));
  gtk_box_pack_start(d->hbox2, d->unit_label, FALSE, FALSE, 0);
  gtk_box_pack_start(d->hbox2, d->print_dpi, TRUE, TRUE, 0);
  gtk_box_pack_start(d->hbox2, gtk_label_new(_("dpi")), FALSE, FALSE, 0);

  d->hbox1 = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 3));
  gtk_box_pack_start(d->hbox1, d->width, TRUE, TRUE, 0);
  gtk_box_pack_start(d->hbox1, gtk_label_new(_("x")), FALSE, FALSE, 0);
  gtk_box_pack_start(d->hbox1, d->height, TRUE, TRUE, 0);
  gtk_box_pack_start(d->hbox1, gtk_label_new(_("px")), FALSE, FALSE, 0);

  d->scale = gtk_entry_new();
  gtk_entry_set_text (GTK_ENTRY(d->scale), dt_conf_get_string(CONFIG_PREFIX "resizing_factor"));
  gtk_widget_set_tooltip_text(d->scale, _("it can be an integer, decimal number or simple fraction.\n"
                                          "zero or empty values are equal to 1.\n"
                                          "click middle mouse button to reset to 1."));
  dt_gui_key_accel_block_on_focus_connect(d->scale);
  gtk_widget_set_halign(GTK_WIDGET(d->scale), GTK_ALIGN_END);
  gtk_widget_add_events(d->scale, GDK_BUTTON_PRESS_MASK);

  d->size_in_px = gtk_label_new("");
  gtk_widget_set_sensitive(GTK_WIDGET(d->size_in_px), FALSE);

  GtkGrid *grid_outer = GTK_GRID(gtk_grid_new());
  gtk_grid_set_row_homogeneous(grid_outer, TRUE);

  GtkFrame *bgr2 = GTK_FRAME(gtk_frame_new(""));
  GtkOverlay *bottom_ovl = GTK_OVERLAY(gtk_overlay_new());

  gtk_widget_set_valign(GTK_WIDGET(d->dimensions_type), GTK_ALIGN_END);

  gtk_widget_set_halign(GTK_WIDGET(d->scale), GTK_ALIGN_FILL);
  gtk_widget_set_halign(GTK_WIDGET(d->size_in_px), GTK_ALIGN_END);

  gtk_container_add(GTK_CONTAINER(bottom_ovl), GTK_WIDGET(bgr2));
  gtk_overlay_add_overlay(bottom_ovl, GTK_WIDGET(d->hbox1));
  gtk_overlay_add_overlay(bottom_ovl, GTK_WIDGET(d->hbox2));
  gtk_overlay_add_overlay(bottom_ovl, GTK_WIDGET(d->scale));

  gtk_grid_attach(grid_outer, GTK_WIDGET(d->dimensions_type), 0, 0, 1, 1);
  gtk_grid_attach(grid_outer, GTK_WIDGET(bottom_ovl), 0, 1, 1, 1);
  gtk_grid_attach(grid_outer, GTK_WIDGET(d->size_in_px), 0, 2, 1, 1);
  gtk_widget_set_hexpand(GTK_WIDGET(bottom_ovl), TRUE);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(grid_outer), TRUE, TRUE, 0);

  d->upscale = dt_bauhaus_combobox_new(NULL);
  dt_bauhaus_widget_set_label(d->upscale, NULL, N_("allow upscaling"));
  dt_bauhaus_combobox_add(d->upscale, _("no"));
  dt_bauhaus_combobox_add(d->upscale, _("yes"));
  gtk_box_pack_start(GTK_BOX(self->widget), d->upscale, FALSE, TRUE, 0);

  d->high_quality = dt_bauhaus_combobox_new(NULL);
  dt_bauhaus_widget_set_label(d->high_quality, NULL, N_("high quality resampling"));
  dt_bauhaus_combobox_add(d->high_quality, _("no"));
  dt_bauhaus_combobox_add(d->high_quality, _("yes"));
  gtk_widget_set_tooltip_text(d->high_quality, _("do high quality resampling during export"));
  gtk_box_pack_start(GTK_BOX(self->widget), d->high_quality, FALSE, TRUE, 0);

  d->export_masks = dt_bauhaus_combobox_new(NULL);
  dt_bauhaus_widget_set_label(d->export_masks, NULL, N_("store masks"));
  dt_bauhaus_combobox_add(d->export_masks, _("no"));
  dt_bauhaus_combobox_add(d->export_masks, _("yes"));
  gtk_widget_set_tooltip_text(d->export_masks, _("store masks as layers in exported images. only works for some formats."));
  gtk_box_pack_start(GTK_BOX(self->widget), d->export_masks, FALSE, TRUE, 0);

  //  Add profile combo

  char datadir[PATH_MAX] = { 0 };
  char confdir[PATH_MAX] = { 0 };
  dt_loc_get_user_config_dir(confdir, sizeof(confdir));
  dt_loc_get_datadir(datadir, sizeof(datadir));

  d->profile = dt_bauhaus_combobox_new(NULL);
  dt_bauhaus_widget_set_label(d->profile, NULL, N_("profile"));
  gtk_box_pack_start(GTK_BOX(self->widget), d->profile, FALSE, TRUE, 0);
  dt_bauhaus_combobox_add(d->profile, _("image settings"));
  for(GList *l = darktable.color_profiles->profiles; l; l = g_list_next(l))
  {
    dt_colorspaces_color_profile_t *prof = (dt_colorspaces_color_profile_t *)l->data;
    if(prof->out_pos > -1) dt_bauhaus_combobox_add(d->profile, prof->name);
  }

  dt_bauhaus_combobox_set(d->profile, 0);

  char *system_profile_dir = g_build_filename(datadir, "color", "out", NULL);
  char *user_profile_dir = g_build_filename(confdir, "color", "out", NULL);
  char *tooltip = g_strdup_printf(_("output ICC profiles in %s or %s"), user_profile_dir, system_profile_dir);
  gtk_widget_set_tooltip_text(d->profile, tooltip);
  g_free(system_profile_dir);
  g_free(user_profile_dir);
  g_free(tooltip);

  //  Add intent combo

  d->intent = dt_bauhaus_combobox_new(NULL);
  dt_bauhaus_widget_set_label(d->intent, NULL, N_("intent"));
  dt_bauhaus_combobox_add(d->intent, _("image settings"));
  dt_bauhaus_combobox_add(d->intent, _("perceptual"));
  dt_bauhaus_combobox_add(d->intent, _("relative colorimetric"));
  dt_bauhaus_combobox_add(d->intent, C_("rendering intent", "saturation"));
  dt_bauhaus_combobox_add(d->intent, _("absolute colorimetric"));
  gtk_box_pack_start(GTK_BOX(self->widget), d->intent, FALSE, TRUE, 0);

  //  Add style combo

  d->style = dt_bauhaus_combobox_new(NULL);
  dt_bauhaus_widget_set_label(d->style, NULL, N_("style"));
  _lib_export_styles_changed_callback(NULL, self);
  gtk_box_pack_start(GTK_BOX(self->widget), d->style, FALSE, TRUE, 0);
  gtk_widget_set_tooltip_text(d->style, _("temporary style to use while exporting"));

  //  Add check to control whether the style is to replace or append the current module

  d->style_mode = dt_bauhaus_combobox_new(NULL);
  dt_bauhaus_widget_set_label(d->style_mode, NULL, N_("mode"));

  gtk_box_pack_start(GTK_BOX(self->widget), d->style_mode, FALSE, TRUE, 0);

  dt_bauhaus_combobox_add(d->style_mode, _("replace history"));
  dt_bauhaus_combobox_add(d->style_mode, _("append history"));

  dt_bauhaus_combobox_set(d->style_mode, 0);

  gtk_widget_set_tooltip_text(d->style_mode,
                              _("whether the style items are appended to the history or replacing the history"));

  //  Set callback signals

  g_signal_connect(G_OBJECT(d->upscale), "value-changed", G_CALLBACK(_callback_bool),
                   (gpointer)CONFIG_PREFIX "upscale");
  g_signal_connect(G_OBJECT(d->high_quality), "value-changed", G_CALLBACK(_callback_bool),
                   (gpointer)CONFIG_PREFIX "high_quality_processing");
  g_signal_connect(G_OBJECT(d->export_masks), "value-changed", G_CALLBACK(_callback_bool),
                   (gpointer)CONFIG_PREFIX "export_masks");
  g_signal_connect(G_OBJECT(d->intent), "value-changed", G_CALLBACK(_intent_changed), (gpointer)d);
  g_signal_connect(G_OBJECT(d->profile), "value-changed", G_CALLBACK(_profile_changed), (gpointer)d);
  g_signal_connect(G_OBJECT(d->style), "value-changed", G_CALLBACK(_style_changed), (gpointer)d);
  g_signal_connect(G_OBJECT(d->style_mode), "value-changed", G_CALLBACK(_callback_bool),
                   (gpointer)CONFIG_PREFIX "style_append");

  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_STYLE_CHANGED,
                            G_CALLBACK(_lib_export_styles_changed_callback), self);

  GtkBox *hbox = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hbox), FALSE, TRUE, 0);

  // Export button
  d->export_button = GTK_BUTTON(dt_ui_button_new(_("export"), _("export with current settings"), NULL));
  gtk_box_pack_start(hbox, GTK_WIDGET(d->export_button), TRUE, TRUE, 0);

  //  Add metadata exportation control
  d->metadata_button = dtgtk_button_new(dtgtk_cairo_paint_preferences, CPF_STYLE_BOX, NULL);
  gtk_widget_set_name(d->metadata_button, "non-flat");
  gtk_widget_set_tooltip_text(d->metadata_button, _("edit metadata exportation details"));
  gtk_box_pack_end(hbox, d->metadata_button, FALSE, TRUE, 0);

  g_signal_connect(G_OBJECT(d->dimensions_type), "value_changed", G_CALLBACK(_dimensions_type_changed), (gpointer)d);
  g_signal_connect(G_OBJECT(d->export_button), "clicked", G_CALLBACK(_export_button_clicked), (gpointer)d);
  g_signal_connect(G_OBJECT(d->width), "changed", G_CALLBACK(_width_changed), (gpointer)d);
  g_signal_connect(G_OBJECT(d->height), "changed", G_CALLBACK(_height_changed), (gpointer)d);
  g_signal_connect(G_OBJECT(d->print_width), "changed", G_CALLBACK(_print_width_changed), (gpointer)d);
  g_signal_connect(G_OBJECT(d->print_height), "changed", G_CALLBACK(_print_height_changed), (gpointer)d);
  g_signal_connect(G_OBJECT(d->print_dpi), "changed", G_CALLBACK(_print_dpi_changed), (gpointer)d);

  g_signal_connect(G_OBJECT(d->metadata_button), "clicked", G_CALLBACK(_metadata_export_clicked), (gpointer)d);
  g_signal_connect(G_OBJECT(d->width), "changed", G_CALLBACK(_width_changed), (gpointer)d);
  g_signal_connect(G_OBJECT(d->height), "changed", G_CALLBACK(_height_changed), (gpointer)d);

  g_signal_connect(G_OBJECT(d->width), "button-press-event", G_CALLBACK(_widht_mdlclick), (gpointer)d);
  g_signal_connect(G_OBJECT(d->height), "button-press-event", G_CALLBACK(_height_mdlclick), (gpointer)d);
  g_signal_connect(G_OBJECT(d->print_width), "button-press-event", G_CALLBACK(_widht_mdlclick), (gpointer)d);
  g_signal_connect(G_OBJECT(d->print_height), "button-press-event", G_CALLBACK(_height_mdlclick), (gpointer)d);

  g_signal_connect(G_OBJECT(d->scale), "button-press-event", G_CALLBACK(_scale_mdlclick), (gpointer)d);
  g_signal_connect(G_OBJECT(d->scale), "changed", G_CALLBACK(_scale_changed), (gpointer)d);

  // this takes care of keeping hidden widgets hidden
  gtk_widget_show_all(self->widget);
  gtk_widget_set_no_show_all(self->widget, TRUE);
  _print_size_update_display(d);

  if (strcmp(dt_conf_get_string(CONFIG_PREFIX "resizing"), "scaling") == 0)
  {
    // scaling
    gtk_widget_show(GTK_WIDGET(d->scale));
    gtk_widget_hide(GTK_WIDGET(d->hbox1));
    gtk_widget_hide(GTK_WIDGET(d->hbox2));
  }
  else
  {
    // max size
    gtk_widget_hide(GTK_WIDGET(d->scale));
    gtk_widget_show(GTK_WIDGET(d->hbox1));
    gtk_widget_show(GTK_WIDGET(d->hbox2));
  }

  d->metadata_export = NULL;

  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_SELECTION_CHANGED,
                            G_CALLBACK(_image_selection_changed_callback), self);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_MOUSE_OVER_IMAGE_CHANGE,
                            G_CALLBACK(_mouse_over_image_callback), self);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_COLLECTION_CHANGED,
                            G_CALLBACK(_collection_updated_callback), self);

  self->gui_reset(self);
}

void gui_cleanup(dt_lib_module_t *self)
{
  dt_lib_cancel_postponed_update(self);
  dt_lib_export_t *d = (dt_lib_export_t *)self->data;
  dt_gui_key_accel_block_on_focus_disconnect(GTK_WIDGET(d->width));
  dt_gui_key_accel_block_on_focus_disconnect(GTK_WIDGET(d->height));

  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_on_storage_list_changed), self);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_lib_export_styles_changed_callback), self);

  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_image_selection_changed_callback), self);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_mouse_over_image_callback), self);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_collection_updated_callback), self);

  GList *it = g_list_first(darktable.imageio->plugins_storage);
  if(it != NULL) do
  {
    dt_imageio_module_storage_t *module = (dt_imageio_module_storage_t *)it->data;
    if(module->widget) gtk_container_remove(GTK_CONTAINER(d->storage_extra_container), module->widget);
  } while((it = g_list_next(it)));

  it = g_list_first(darktable.imageio->plugins_format);
  if(it != NULL) do
  {
    dt_imageio_module_format_t *module = (dt_imageio_module_format_t *)it->data;
    if(module->widget) gtk_container_remove(GTK_CONTAINER(d->format_extra_container), module->widget);
  } while((it = g_list_next(it)));

  g_free(d->metadata_export);

  free(self->data);
  self->data = NULL;
}

void init_presets(dt_lib_module_t *self)
{
  // TODO: store presets in db:
  // dt_lib_presets_add(const char *name, const char *plugin_name, const void *params, const int32_t
  // params_size)


  // I know that it is super ugly to have this inside a module, but then is export not your average module
  // since it handles the params blobs of imageio libs.
  // - get all existing presets for export from db,
  // - extract the versions of the embedded format/storage blob
  // - check if it's up to date
  // - if older than the module -> call its legacy_params and update the preset
  // - drop presets that cannot be updated

  const int version = self->version();

  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get(darktable.db),
      "SELECT rowid, op_version, op_params, name FROM data.presets WHERE operation='export'", -1, &stmt, NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const int rowid = sqlite3_column_int(stmt, 0);
    const int op_version = sqlite3_column_int(stmt, 1);
    const void *op_params = (void *)sqlite3_column_blob(stmt, 2);
    const size_t op_params_size = sqlite3_column_bytes(stmt, 2);
    const char *name = (char *)sqlite3_column_text(stmt, 3);

    if(op_version != version)
    {
      // shouldn't happen, we run legacy_params on the lib level before calling this
      fprintf(stderr, "[export_init_presets] found export preset '%s' with version %d, version %d was "
                      "expected. dropping preset.\n",
              name, op_version, version);
      sqlite3_stmt *innerstmt;
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "DELETE FROM data.presets WHERE rowid=?1", -1,
                                  &innerstmt, NULL);
      DT_DEBUG_SQLITE3_BIND_INT(innerstmt, 1, rowid);
      sqlite3_step(innerstmt);
      sqlite3_finalize(innerstmt);
    }
    else
    {
      // extract the interesting parts from the blob
      const char *buf = (const char *)op_params;

      // skip 6*int32_t: max_width, max_height, upscale, high_quality and iccintent, icctype
      buf += 6 * sizeof(int32_t);
      // skip metadata presets string
      buf += strlen(buf) + 1;
      // next skip iccfilename
      buf += strlen(buf) + 1;

      // parse both names to '\0'
      const char *fname = buf;
      buf += strlen(fname) + 1;
      const char *sname = buf;
      buf += strlen(sname) + 1;

      // get module by name and skip if not there.
      dt_imageio_module_format_t *fmod = dt_imageio_get_format_by_name(fname);
      dt_imageio_module_storage_t *smod = dt_imageio_get_storage_by_name(sname);
      if(!fmod || !smod) continue;

      // next we have fversion, sversion, fsize, ssize, fdata, sdata which is the stuff that might change
      size_t copy_over_part = (void *)buf - (void *)op_params;

      const int fversion = *(const int *)buf;
      buf += sizeof(int32_t);
      const int sversion = *(const int *)buf;
      buf += sizeof(int32_t);
      const int fsize = *(const int *)buf;
      buf += sizeof(int32_t);
      const int ssize = *(const int *)buf;
      buf += sizeof(int32_t);

      const void *fdata = buf;
      buf += fsize;
      const void *sdata = buf;

      void *new_fdata = NULL, *new_sdata = NULL;
      size_t new_fsize = fsize, new_ssize = ssize;
      int32_t new_fversion = fmod->version(), new_sversion = smod->version();

      if(fversion < new_fversion)
      {
        if(!(fmod->legacy_params
             && (new_fdata = fmod->legacy_params(fmod, fdata, fsize, fversion, new_fversion, &new_fsize))
                != NULL))
          goto delete_preset;
      }

      if(sversion < new_sversion)
      {
        if(!(smod->legacy_params
             && (new_sdata = smod->legacy_params(smod, sdata, ssize, sversion, new_sversion, &new_ssize))
                != NULL))
          goto delete_preset;
      }

      if(new_fdata || new_sdata)
      {
        // we got an updated blob -> reassemble the parts and update the preset
        size_t new_params_size = op_params_size - (fsize + ssize) + (new_fsize + new_ssize);
        void *new_params = malloc(new_params_size);
        memcpy(new_params, op_params, copy_over_part);
        // next we have fversion, sversion, fsize, ssize, fdata, sdata which is the stuff that might change
        size_t pos = copy_over_part;
        memcpy(new_params + pos, &new_fversion, sizeof(int32_t));
        pos += sizeof(int32_t);
        memcpy(new_params + pos, &new_sversion, sizeof(int32_t));
        pos += sizeof(int32_t);
        memcpy(new_params + pos, &new_fsize, sizeof(int32_t));
        pos += sizeof(int32_t);
        memcpy(new_params + pos, &new_ssize, sizeof(int32_t));
        pos += sizeof(int32_t);
        if(new_fdata)
          memcpy(new_params + pos, new_fdata, new_fsize);
        else
          memcpy(new_params + pos, fdata, fsize);
        pos += new_fsize;
        if(new_sdata)
          memcpy(new_params + pos, new_sdata, new_ssize);
        else
          memcpy(new_params + pos, sdata, ssize);

        // write the updated preset back to db
        fprintf(stderr,
                "[export_init_presets] updating export preset '%s' from versions %d/%d to versions %d/%d\n",
                name, fversion, sversion, new_fversion, new_sversion);
        sqlite3_stmt *innerstmt;
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                    "UPDATE data.presets SET op_params=?1 WHERE rowid=?2", -1, &innerstmt, NULL);
        DT_DEBUG_SQLITE3_BIND_BLOB(innerstmt, 1, new_params, new_params_size, SQLITE_TRANSIENT);
        DT_DEBUG_SQLITE3_BIND_INT(innerstmt, 2, rowid);
        sqlite3_step(innerstmt);
        sqlite3_finalize(innerstmt);

        free(new_fdata);
        free(new_sdata);
        free(new_params);
      }

      continue;

    delete_preset:
      free(new_fdata);
      free(new_sdata);
      fprintf(stderr, "[export_init_presets] export preset '%s' can't be updated from versions %d/%d to "
                      "versions %d/%d. dropping preset\n",
              name, fversion, sversion, new_fversion, new_sversion);
      sqlite3_stmt *innerstmt;
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "DELETE FROM data.presets WHERE rowid=?1", -1,
                                  &innerstmt, NULL);
      DT_DEBUG_SQLITE3_BIND_INT(innerstmt, 1, rowid);
      sqlite3_step(innerstmt);
      sqlite3_finalize(innerstmt);
    }
  }
  sqlite3_finalize(stmt);
}

void *legacy_params(dt_lib_module_t *self, const void *const old_params, const size_t old_params_size,
                    const int old_version, int *new_version, size_t *new_size)
{
  if(old_version == 1)
  {
    // add version of format & storage to params
    size_t new_params_size = old_params_size + 2 * sizeof(int32_t);
    void *new_params = malloc(new_params_size);

    const char *buf = (const char *)old_params;

    // skip 3*int32_t: max_width, max_height and iccintent
    buf += 3 * sizeof(int32_t);
    // next skip iccprofile
    buf += strlen(buf) + 1;

    // parse both names to '\0'
    const char *fname = buf;
    buf += strlen(fname) + 1;
    const char *sname = buf;
    buf += strlen(sname) + 1;

    // get module by name and fail if not there.
    dt_imageio_module_format_t *fmod = dt_imageio_get_format_by_name(fname);
    dt_imageio_module_storage_t *smod = dt_imageio_get_storage_by_name(sname);
    if(!fmod || !smod)
    {
      free(new_params);
      return NULL;
    }

    // now we are just behind the module/storage names and before their param sizes. this is the place where
    // we want their versions
    // copy everything until here to the new params
    size_t first_half = (void *)buf - (void *)old_params;
    memcpy(new_params, old_params, first_half);
    // add the versions. at the time this code was added all modules were at version 1, except of picasa which was at 2.
    // every newer version of the imageio modules should result in a preset that is not going through this code.
    int32_t fversion = 1;
    int32_t sversion = (strcmp(sname, "picasa") == 0 ? 2 : 1);
    memcpy(new_params + first_half, &fversion, sizeof(int32_t));
    memcpy(new_params + first_half + sizeof(int32_t), &sversion, sizeof(int32_t));
    // copy the rest of the old params over
    memcpy(new_params + first_half + 2 * sizeof(int32_t), buf, old_params_size - first_half);

    *new_size = new_params_size;
    *new_version = 2;
    return new_params;
  }
  else if(old_version == 2)
  {
    // add upscale to params
    size_t new_params_size = old_params_size + sizeof(int32_t);
    void *new_params = calloc(1, new_params_size);

    memcpy(new_params, old_params, 2 * sizeof(int32_t));
    memcpy(new_params + 3 * sizeof(int32_t), old_params + 2 * sizeof(int32_t), old_params_size - 2 * sizeof(int32_t));

    *new_size = new_params_size;
    *new_version = 3;
    return new_params;
  }
  else if(old_version == 3)
  {
    // replace iccprofile by type + filename
    // format of v3:
    //  - 4 x int32_t (max_width, max_height, upscale, iccintent)
    //  - char* (iccprofile)
    //  - rest
    // format of v4:
    //  - 5 x int32_t (max_width, max_height, upscale, iccintent, icctype)
    //  - char* (iccfilename)
    //  - old rest

    const char *buf = (const char *)old_params;

    // first get the old iccprofile to find out how big our new blob has to be
    const char *iccprofile = buf + 4 * sizeof(int32_t);

    size_t new_params_size = old_params_size - strlen(iccprofile) + sizeof(int32_t);
    int icctype;
    const char *iccfilename = "";

    if(!strcmp(iccprofile, "image"))
      icctype = DT_COLORSPACE_NONE;
    else if(!strcmp(iccprofile, "sRGB"))
      icctype = DT_COLORSPACE_SRGB;
    else if(!strcmp(iccprofile, "linear_rec709_rgb") || !strcmp(iccprofile, "linear_rgb"))
      icctype = DT_COLORSPACE_LIN_REC709;
    else if(!strcmp(iccprofile, "linear_rec2020_rgb"))
      icctype = DT_COLORSPACE_LIN_REC2020;
    else if(!strcmp(iccprofile, "adobergb"))
      icctype = DT_COLORSPACE_ADOBERGB;
    else
    {
      icctype = DT_COLORSPACE_FILE;
      iccfilename = iccprofile;
      new_params_size += strlen(iccfilename);
    }

    void *new_params = calloc(1, new_params_size);
    size_t pos = 0;
    memcpy(new_params, old_params, 4 * sizeof(int32_t));
    pos += 4 * sizeof(int32_t);
    memcpy(new_params + pos, &icctype, sizeof(int32_t));
    pos += sizeof(int32_t);
    memcpy(new_params + pos, iccfilename, strlen(iccfilename) + 1);
    pos += strlen(iccfilename) + 1;
    size_t old_pos = 4 * sizeof(int32_t) + strlen(iccprofile) + 1;
    memcpy(new_params + pos, old_params + old_pos, old_params_size - old_pos);

    *new_size = new_params_size;
    *new_version = 4;
    return new_params;
  }
  else if(old_version == 4)
  {
    // add high_quality to params

    // format of v4:
    //  - 5 x int32_t (max_width, max_height, upscale, iccintent, icctype)
    //  - char* (iccfilename)
    //  - old rest
    // format of v5:
    //  - 6 x int32_t (max_width, max_height, upscale, high_quality, iccintent, icctype)
    //  - char* (iccfilename)
    //  - old rest

    size_t new_params_size = old_params_size + sizeof(int32_t);
    void *new_params = calloc(1, new_params_size);

    size_t pos = 0;
    memcpy(new_params, old_params, 3 * sizeof(int32_t));
    pos += 4 * sizeof(int32_t);
    memcpy(new_params + pos, old_params + pos - sizeof(int32_t), old_params_size - 3 * sizeof(int32_t));

    *new_size = new_params_size;
    *new_version = 5;
    return new_params;
  }
  else if(old_version == 5)
  {
    // add metadata preset string

    // format of v5:
    //  - 6 x int32_t (max_width, max_height, upscale, high_quality, iccintent, icctype)
    //  - char* (iccfilename)
    //  - old rest
    // format of v6:
    //  - 6 x int32_t (max_width, max_height, upscale, high_quality, iccintent, icctype)
    //  - char* (metadata_export)
    //  - char* (iccfilename)
    //  - old rest

    const gboolean omit = dt_conf_get_bool("omit_tag_hierarchy");
    char *flags = dt_util_dstrcat(NULL, "%x", dt_lib_export_metadata_default_flags() | (omit ? DT_META_OMIT_HIERARCHY : 0));
    const int flags_size = strlen(flags) + 1;
    size_t new_params_size = old_params_size + flags_size;
    void *new_params = calloc(1, new_params_size);
    size_t pos = 0;
    memcpy(new_params, old_params, 6 * sizeof(int32_t));
    pos += 6 * sizeof(int32_t);
    memcpy(new_params + pos, flags, flags_size);
    pos += flags_size;
    memcpy(new_params + pos, old_params + pos - flags_size, old_params_size - 6 * sizeof(int32_t));

    g_free(flags);
    *new_size = new_params_size;
    *new_version = 6;
    return new_params;
  }
  else if(old_version == 6)
  {
    // add export_masks

    // format of v6:
    //  - 6 x int32_t (max_width, max_height, upscale, high_quality, iccintent, icctype)
    //  - old rest
    // format of v7:
    //  - 7 x int32_t (max_width, max_height, upscale, high_quality, export_masks, iccintent, icctype)
    //  - old rest

    const size_t new_params_size = old_params_size + sizeof(int32_t);
    void *new_params = calloc(1, new_params_size);

    size_t pos = 0;
    memcpy(new_params, old_params, 4 * sizeof(int32_t));
    pos += 5 * sizeof(int32_t);
    memcpy(new_params + pos, old_params + pos - sizeof(int32_t), old_params_size - 4 * sizeof(int32_t));

    *new_size = new_params_size;
    *new_version = 7;
    return new_params;
  }

  return NULL;
}

void *get_params(dt_lib_module_t *self, int *size)
{
  dt_lib_export_t *d = (dt_lib_export_t *)self->data;
  // concat storage and format, size is max + header
  dt_imageio_module_format_t *mformat = dt_imageio_get_format();
  dt_imageio_module_storage_t *mstorage = dt_imageio_get_storage();
  if(!mformat || !mstorage) return NULL;

  // size will be only as large as needed to remove random pointers from params (stored at the end).
  size_t fsize = mformat->params_size(mformat);
  dt_imageio_module_data_t *fdata = mformat->get_params(mformat);
  size_t ssize = mstorage->params_size(mstorage);
  void *sdata = mstorage->get_params(mstorage);
  int32_t fversion = mformat->version();
  int32_t sversion = mstorage->version();
  // we allow null pointers (plugin not ready for export in current state), and just don't copy back the
  // settings later:
  if(!sdata) ssize = 0;
  if(!fdata) fsize = 0;
  if(fdata)
  {
    // clean up format global params (need to set all bytes to reliably detect which preset is active).
    // we happen to want to set it all to 0
    memset(fdata, 0, sizeof(dt_imageio_module_data_t));
  }

  // FIXME: also the web preset has to be applied twice to be known as preset! (other dimension magic going on
  // here?)
  // TODO: get this stuff from gui and not from conf, so it will be sanity-checked (you can never delete an
  // insane preset)?
  // also store icc profile/intent here.
  const int32_t iccintent = dt_conf_get_int(CONFIG_PREFIX "iccintent");
  const int32_t icctype = dt_conf_get_int(CONFIG_PREFIX "icctype");
  const int32_t max_width = dt_conf_get_int(CONFIG_PREFIX "width");
  const int32_t max_height = dt_conf_get_int(CONFIG_PREFIX "height");
  const int32_t upscale = dt_conf_get_bool(CONFIG_PREFIX "upscale") ? 1 : 0;
  const int32_t high_quality = dt_conf_get_bool(CONFIG_PREFIX "high_quality_processing") ? 1 : 0;
  const int32_t export_masks = dt_conf_get_bool(CONFIG_PREFIX "export_masks") ? 1 : 0;
  gchar *iccfilename = dt_conf_get_string(CONFIG_PREFIX "iccprofile");
  gchar *style = dt_conf_get_string(CONFIG_PREFIX "style");
  const gboolean style_append = dt_conf_get_bool(CONFIG_PREFIX "style_append");
  const char *metadata_export = d->metadata_export;

  if(fdata)
  {
    g_strlcpy(fdata->style, style, sizeof(fdata->style));
    fdata->style_append = style_append;
  }

  if(icctype != DT_COLORSPACE_FILE)
  {
    g_free(iccfilename);
    iccfilename = NULL;
  }
  if(!iccfilename) iccfilename = g_strdup("");
  if(!metadata_export) metadata_export = g_strdup("");

  char *fname = mformat->plugin_name, *sname = mstorage->plugin_name;
  int32_t fname_len = strlen(fname), sname_len = strlen(sname);
  *size = fname_len + sname_len + 2 + 4 * sizeof(int32_t) + fsize + ssize + 7 * sizeof(int32_t)
          + strlen(iccfilename) + 1 + strlen(metadata_export) + 1;

  char *params = (char *)calloc(1, *size);
  int pos = 0;
  memcpy(params + pos, &max_width, sizeof(int32_t));
  pos += sizeof(int32_t);
  memcpy(params + pos, &max_height, sizeof(int32_t));
  pos += sizeof(int32_t);
  memcpy(params + pos, &upscale, sizeof(int32_t));
  pos += sizeof(int32_t);
  memcpy(params + pos, &high_quality, sizeof(int32_t));
  pos += sizeof(int32_t);
  memcpy(params + pos, &export_masks, sizeof(int32_t));
  pos += sizeof(int32_t);
  memcpy(params + pos, &iccintent, sizeof(int32_t));
  pos += sizeof(int32_t);
  memcpy(params + pos, &icctype, sizeof(int32_t));
  pos += sizeof(int32_t);
  memcpy(params + pos, metadata_export, strlen(metadata_export) + 1);
  pos += strlen(metadata_export) + 1;
  memcpy(params + pos, iccfilename, strlen(iccfilename) + 1);
  pos += strlen(iccfilename) + 1;
  memcpy(params + pos, fname, fname_len + 1);
  pos += fname_len + 1;
  memcpy(params + pos, sname, sname_len + 1);
  pos += sname_len + 1;
  memcpy(params + pos, &fversion, sizeof(int32_t));
  pos += sizeof(int32_t);
  memcpy(params + pos, &sversion, sizeof(int32_t));
  pos += sizeof(int32_t);
  memcpy(params + pos, &fsize, sizeof(int32_t));
  pos += sizeof(int32_t);
  memcpy(params + pos, &ssize, sizeof(int32_t));
  pos += sizeof(int32_t);
  if(fdata != NULL) // otherwise fsize == 0, but clang doesn't like it ...
  {
    memcpy(params + pos, fdata, fsize);
    pos += fsize;
  }
  if(sdata != NULL) // see above
  {
    memcpy(params + pos, sdata, ssize);
    pos += ssize;
  }
  g_assert(pos == *size);

  g_free(iccfilename);
  g_free(style);

  if(fdata) mformat->free_params(mformat, fdata);
  if(sdata) mstorage->free_params(mstorage, sdata);
  return params;
}

int set_params(dt_lib_module_t *self, const void *params, int size)
{
  dt_lib_export_t *d = (dt_lib_export_t *)self->data;
  // apply these stored presets again (parse blob)
  const char *buf = (const char *)params;

  const int max_width = *(const int *)buf;
  buf += sizeof(int32_t);
  const int max_height = *(const int *)buf;
  buf += sizeof(int32_t);
  const int upscale = *(const int *)buf;
  buf += sizeof(int32_t);
  const int high_quality = *(const int *)buf;
  buf += sizeof(int32_t);
  const int export_masks = *(const int *)buf;
  buf += sizeof(int32_t);
  const int iccintent = *(const int *)buf;
  buf += sizeof(int32_t);
  const int icctype = *(const int *)buf;
  buf += sizeof(int32_t);
  const char *metadata_export = buf;
  buf += strlen(metadata_export) + 1;
  g_free(d->metadata_export);
  d->metadata_export = g_strdup(metadata_export);
  dt_lib_export_metadata_set_conf(d->metadata_export);
  const char *iccfilename = buf;
  buf += strlen(iccfilename) + 1;

  // reverse these by setting the gui, not the conf vars!
  dt_bauhaus_combobox_set(d->intent, iccintent + 1);

  dt_bauhaus_combobox_set(d->profile, 0);
  if(icctype != DT_COLORSPACE_NONE)
  {
    for(GList *iter = darktable.color_profiles->profiles; iter; iter = g_list_next(iter))
    {
      dt_colorspaces_color_profile_t *pp = (dt_colorspaces_color_profile_t *)iter->data;
      if(pp->out_pos > -1 &&
         icctype == pp->type && (icctype != DT_COLORSPACE_FILE || !strcmp(iccfilename, pp->filename)))
      {
        dt_bauhaus_combobox_set(d->profile, pp->out_pos + 1);
        break;
      }
    }
  }

  // parse both names to '\0'
  const char *fname = buf;
  buf += strlen(fname) + 1;
  const char *sname = buf;
  buf += strlen(sname) + 1;

  // get module by name and fail if not there.
  dt_imageio_module_format_t *fmod = dt_imageio_get_format_by_name(fname);
  dt_imageio_module_storage_t *smod = dt_imageio_get_storage_by_name(sname);
  if(!fmod || !smod) return 1;

  const int32_t fversion = *(const int32_t *)buf;
  buf += sizeof(int32_t);
  const int32_t sversion = *(const int32_t *)buf;
  buf += sizeof(int32_t);

  const int fsize = *(const int *)buf;
  buf += sizeof(int32_t);
  const int ssize = *(const int *)buf;
  buf += sizeof(int32_t);

  if(size
     != strlen(fname) + strlen(sname) + 2 + 4 * sizeof(int32_t) + fsize + ssize + 7 * sizeof(int32_t)
        + strlen(iccfilename) + 1 + strlen(metadata_export) + 1)
    return 1;
  if(fversion != fmod->version() || sversion != smod->version()) return 1;

  const dt_imageio_module_data_t *fdata = (const dt_imageio_module_data_t *)buf;
  if(fdata->style[0] == '\0')
    dt_bauhaus_combobox_set(d->style, 0);
  else
    dt_bauhaus_combobox_set_from_text(d->style, fdata->style);

  dt_bauhaus_combobox_set(d->style_mode, fdata->style_append ? 1 : 0);

  buf += fsize;
  const void *sdata = buf;

  // switch modules
  set_storage_by_name(d, sname);
  set_format_by_name(d, fname);

  // set dimensions after switching, to have new range ready.
  _set_dimensions(d, max_width, max_height);
  dt_bauhaus_combobox_set(d->upscale, upscale ? 1 : 0);
  dt_bauhaus_combobox_set(d->high_quality, high_quality ? 1 : 0);
  dt_bauhaus_combobox_set(d->export_masks, export_masks ? 1 : 0);

  // propagate to modules
  int res = 0;
  if(ssize) res += smod->set_params(smod, sdata, ssize);
  if(fsize) res += fmod->set_params(fmod, fdata, fsize);
  return res;
}

void init_key_accels(dt_lib_module_t *self)
{
  dt_accel_register_lib(self, NC_("accel", "export"), GDK_KEY_e, GDK_CONTROL_MASK);
}

void connect_key_accels(dt_lib_module_t *self)
{
  const dt_lib_export_t *d = (dt_lib_export_t *)self->data;

  dt_accel_connect_button_lib(self, "export", GTK_WIDGET(d->export_button));
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
