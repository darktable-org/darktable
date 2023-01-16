/*
    This file is part of darktable,
    Copyright (C) 2009-2022 darktable developers.

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
  GtkWidget *unit_label;
  GtkWidget *width, *height;
  GtkWidget *px_size, *print_size, *scale, *size_in_px;
  GtkWidget *storage, *format;
  int format_lut[128];
  uint32_t max_allowed_width , max_allowed_height;
  GtkWidget *upscale, *profile, *intent, *style, *style_mode;
  GtkButton *export_button;
  GtkWidget *storage_extra_container, *format_extra_container;
  GtkWidget *high_quality;
  GtkWidget *export_masks;
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
  return _("export");
}

const char **views(dt_lib_module_t *self)
{
  static const char *v1[] = {"lighttable", "darkroom", NULL};
  static const char *v2[] = {"lighttable", NULL};

  if(dt_conf_get_bool("plugins/darkroom/export/visible"))
    return v1;
  else
    return v2;
}

uint32_t container(dt_lib_module_t *self)
{
  const dt_view_t *cv = dt_view_manager_get_current_view(darktable.view_manager);
  if(cv->view((dt_view_t *)cv) == DT_VIEW_DARKROOM)
    return DT_UI_CONTAINER_PANEL_LEFT_CENTER;
  else
    return DT_UI_CONTAINER_PANEL_RIGHT_CENTER;
}

static void _update(dt_lib_module_t *self)
{
  dt_lib_cancel_postponed_update(self);
  const dt_lib_export_t *d = (dt_lib_export_t *)self->data;

  const gboolean has_act_on = (dt_act_on_get_images_nb(TRUE, FALSE) > 0);

  const char *format_name = dt_conf_get_string_const(CONFIG_PREFIX "format_name");
  const char *storage_name = dt_conf_get_string_const(CONFIG_PREFIX "storage_name");
  const int format_index = dt_imageio_get_index_of_format(dt_imageio_get_format_by_name(format_name));
  const int storage_index = dt_imageio_get_index_of_storage(dt_imageio_get_storage_by_name(storage_name));

  gtk_widget_set_sensitive(GTK_WIDGET(d->export_button), has_act_on && format_index != -1 && storage_index != -1);
}

static void _image_selection_changed_callback(gpointer instance, dt_lib_module_t *self)
{
  _update(self);
}

static void _collection_updated_callback(gpointer instance, dt_collection_change_t query_change,
                                         dt_collection_properties_t changed_property, gpointer imgs, int next,
                                         dt_lib_module_t *self)
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
  double num = 1.0, denum = 1.0;
  dt_imageio_resizing_factor_get_and_parsing(&num, &denum);
  gchar *scale_str = dt_conf_get_string(CONFIG_PREFIX "resizing_factor");
  gchar _str[6] = "";

  gchar *pdiv = strchr(scale_str, '/');

  gchar scale_buf[64] = "";
  if(pdiv == NULL)
  {
    if(_is_int(num) && num > 0.0)
    {
      sprintf(_str, "%d", (int) num);
      g_strlcat(scale_buf, _str, sizeof(scale_buf));
    }
    else
    {
      g_strlcat(scale_buf, scale_str, sizeof(scale_buf));
    }
  }
  else if(pdiv-scale_str == 0)
  {
    if(_is_int(denum) && denum > 0.0)
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
    if(_is_int(num) && num > 0.0)
    {
      sprintf(_str, "%d", (int) num);
      g_strlcat(scale_buf, _str, sizeof(scale_buf));
    }
    else
    {
      g_strlcat(scale_buf, scale_str, sizeof(scale_buf));
    }
    g_strlcat(scale_buf, "/", sizeof(scale_buf));
    if(_is_int(denum) && denum > 0.0)
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
  /* write current history changes so nothing gets lost,
     do that only in the darkroom as there is nothing to be saved
     when in the lighttable (and it would write over current history stack) */
  const dt_view_t *cv = dt_view_manager_get_current_view(darktable.view_manager);
  if(cv->view(cv) == DT_VIEW_DARKROOM) dt_dev_write_history(darktable.develop);

  char style[128] = { 0 };

  // get the format_name and storage_name settings which are plug-ins name and not necessary what is displayed on the combobox.
  // note that we cannot take directly the combobox entry index as depending on the storage some format are not listed.
  const char *format_name = dt_conf_get_string_const(CONFIG_PREFIX "format_name");
  const char *storage_name = dt_conf_get_string_const(CONFIG_PREFIX "storage_name");
  const int format_index = dt_imageio_get_index_of_format(dt_imageio_get_format_by_name(format_name));
  const int storage_index = dt_imageio_get_index_of_storage(dt_imageio_get_storage_by_name(storage_name));

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

    gboolean res = dt_gui_show_yes_no_dialog(_("export to disk"),  "%s", confirm_message);
    g_free(confirm_message);
    confirm_message = NULL;

    if(!res)
    {
      return;
    }
  }

  // Let's get the max dimension restriction if any...
  uint32_t max_width = dt_conf_get_int(CONFIG_PREFIX "width");
  uint32_t max_height = dt_conf_get_int(CONFIG_PREFIX "height");

  const gboolean upscale = dt_conf_get_bool(CONFIG_PREFIX "upscale");
  const gboolean scaledimension = dt_conf_get_int(CONFIG_PREFIX "dimensions_type") == DT_DIMENSIONS_SCALE;
  const gboolean high_quality = dt_conf_get_bool(CONFIG_PREFIX "high_quality_processing");
  const gboolean export_masks = dt_conf_get_bool(CONFIG_PREFIX "export_masks");
  const gboolean style_append = dt_conf_get_bool(CONFIG_PREFIX "style_append");
  const char *tmp = dt_conf_get_string_const(CONFIG_PREFIX "style");
  if(tmp)
  {
    g_strlcpy(style, tmp, sizeof(style));
  }

  // if upscale is activated and only one dimension is 0 we adjust it to ensure
  // that the up-scaling will happen. The null dimension is set to MAX_ASPECT_RATIO
  // time the other dimension, allowing for a ratio of max 1:100 exported images.

  if(upscale)
  {
    const uint32_t MAX_ASPECT_RATIO = 100;

    if(max_width == 0 && max_height != 0)
    {
      max_width = max_height * MAX_ASPECT_RATIO;
    }
    else if(max_height == 0 && max_width != 0)
    {
      max_height = max_width * MAX_ASPECT_RATIO;
    }
  }

  const dt_colorspaces_color_profile_type_t icc_type = dt_conf_get_int(CONFIG_PREFIX "icctype");
  gchar *icc_filename = dt_conf_get_string(CONFIG_PREFIX "iccprofile");
  const dt_iop_color_intent_t icc_intent = dt_conf_get_int(CONFIG_PREFIX "iccintent");

  GList *list = dt_act_on_get_images(TRUE, TRUE, TRUE);
  dt_control_export(list, max_width, max_height, format_index, storage_index, high_quality, upscale, scaledimension, export_masks,
                    style, style_append, icc_type, icc_filename, icc_intent, d->metadata_export);

  g_free(icc_filename);

  _scale_optim();
  gtk_entry_set_text(GTK_ENTRY(d->scale), dt_conf_get_string_const(CONFIG_PREFIX "resizing_factor"));
}

static void _scale_changed(GtkEntry *spin, dt_lib_export_t *d)
{
  const char *validSign = ",.0123456789";
  const gchar *value = gtk_entry_get_text(spin);

  const int len = sizeof(value);
  int i, j = 0, idec = 0, idiv = 0, pdiv = 0;
  char new_value[30] = "";

  for(i = 0; i < len; i++)
  {
    char *val = strchr(validSign, value[i]);
    if(val == NULL)
    {
      if(idiv==0)
      {
        if(i == 0)
        {
          new_value[j++] = '1';
        }
        else
        {
          if(atof(value) == 0.0)
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
    else if((val[0] == '.') || (val[0] == ','))
    {
      if(idec == 0)
      {
        if((i == 0) || (i == pdiv))
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
    else if(value[i] == '\0')
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

static gboolean _scale_mdlclick(GtkEntry *spin, GdkEventButton *event, dt_lib_export_t *d)
{
  if(event->button == 2)
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
  return FALSE;
}

static void _widht_mdlclick(GtkEntry *spin, GdkEventButton *event, gpointer user_data)
{
  if(event->button == 2)
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
  if(event->button == 2)
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

  if((d_type == DT_DIMENSIONS_SCALE) || (d_type == DT_DIMENSIONS_PIXELS))
  {
    gtk_widget_hide(d->size_in_px);
  }
  else
  {
    gtk_widget_show(d->size_in_px);
    gchar size_in_px_txt[120];
    snprintf(size_in_px_txt, sizeof(size_in_px_txt) / sizeof(size_in_px_txt[0]), _("which is equal to %s × %s px"),
             gtk_entry_get_text(GTK_ENTRY(d->width)), gtk_entry_get_text(GTK_ENTRY(d->height)));
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


void _size_update_display(dt_lib_export_t *self)
{
  const dt_dimensions_type_t d_type = (dt_dimensions_type_t)dt_bauhaus_combobox_get(self->dimensions_type);

  gtk_widget_set_visible(self->px_size, d_type == DT_DIMENSIONS_PIXELS);
  gtk_widget_set_visible(self->print_size, d_type == DT_DIMENSIONS_CM || d_type == DT_DIMENSIONS_INCH);
  gtk_widget_set_visible(self->scale, d_type == DT_DIMENSIONS_SCALE);

  gtk_label_set_text(GTK_LABEL(self->unit_label),
                     d_type == DT_DIMENSIONS_CM ? _("cm") : C_("unit", "in"));
  _size_in_px_update(self);
}

void gui_reset(dt_lib_module_t *self)
{
  // make sure we don't do anything useless:
  if(!dt_control_running()) return;
  dt_lib_export_t *d = (dt_lib_export_t *)self->data;
  gtk_entry_set_text(GTK_ENTRY(d->width), dt_confgen_get(CONFIG_PREFIX "width", DT_DEFAULT));
  gtk_entry_set_text(GTK_ENTRY(d->height), dt_confgen_get(CONFIG_PREFIX "height", DT_DEFAULT));
  dt_bauhaus_combobox_set(d->dimensions_type, dt_confgen_get_int(CONFIG_PREFIX "dimensions_type", DT_DEFAULT));
  _size_update_display(d);

  // Set storage
  const int storage_index = dt_imageio_get_index_of_storage(dt_imageio_get_storage_by_name(dt_confgen_get(CONFIG_PREFIX "storage_name", DT_DEFAULT)));
  dt_bauhaus_combobox_set(d->storage, storage_index);

  dt_bauhaus_combobox_set(d->upscale, dt_confgen_get_bool(CONFIG_PREFIX "upscale", DT_DEFAULT) ? 1 : 0);
  dt_bauhaus_combobox_set(d->high_quality, dt_confgen_get_bool(CONFIG_PREFIX "high_quality_processing", DT_DEFAULT) ? 1 : 0);
  dt_bauhaus_combobox_set(d->export_masks, dt_confgen_get_bool(CONFIG_PREFIX "export_masks", DT_DEFAULT) ? 1 : 0);

  dt_bauhaus_combobox_set(d->intent, dt_confgen_get_int(CONFIG_PREFIX "iccintent", DT_DEFAULT) + 1);

  // iccprofile
  const int icctype = dt_confgen_get_int(CONFIG_PREFIX "icctype", DT_DEFAULT);
  gchar *iccfilename = dt_conf_get_string(CONFIG_PREFIX "iccprofile");
  dt_bauhaus_combobox_set(d->profile, 0);
  if(icctype != DT_COLORSPACE_NONE)
  {
    for(GList *profiles = darktable.color_profiles->profiles; profiles; profiles = g_list_next(profiles))
    {
      const dt_colorspaces_color_profile_t *pp = (dt_colorspaces_color_profile_t *)profiles->data;
      if(pp->out_pos > -1
         && icctype == pp->type
         && (icctype != DT_COLORSPACE_FILE || !strcmp(iccfilename, pp->filename)))
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
  const char *style = dt_confgen_get(CONFIG_PREFIX "style", DT_DEFAULT);
  if(style != NULL && strlen(style) > 0)
  {
    rc = dt_bauhaus_combobox_set_from_text(d->style, style);
    if(rc == FALSE) dt_bauhaus_combobox_set(d->style, 0);
  }
  else
    dt_bauhaus_combobox_set(d->style, 0);

  // style mode to overwrite as it was the initial behavior
  dt_bauhaus_combobox_set(d->style_mode, dt_confgen_get_bool(CONFIG_PREFIX "style_append", DT_DEFAULT));

  gtk_widget_set_visible(GTK_WIDGET(d->style_mode), dt_bauhaus_combobox_get(d->style)==0?FALSE:TRUE);

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
  const char *storage_name = dt_conf_get_string_const(CONFIG_PREFIX "storage_name");
  dt_imageio_module_storage_t *storage = dt_imageio_get_storage_by_name(storage_name);

  const char *format_name = dt_conf_get_string_const(CONFIG_PREFIX "format_name");
  dt_imageio_module_format_t *format = dt_imageio_get_format_by_name(format_name);

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
    width  = width  > d->max_allowed_width  ? dt_conf_get_int(CONFIG_PREFIX "width")  : width;
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
  dt_imageio_module_storage_t *module = NULL;

  for(const GList *it = darktable.imageio->plugins_storage; it; it = g_list_next(it))
  {
    dt_imageio_module_storage_t *storage = (dt_imageio_module_storage_t *)it->data;
    k++;
    if(strcmp(storage->name(storage), name) == 0 || strcmp(storage->plugin_name, name) == 0)
    {
      module = storage;
      break;
    }
  }

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
  const char *format_name = dt_conf_get_string_const(CONFIG_PREFIX "format_name");
  const dt_imageio_module_format_t *format = dt_imageio_get_format_by_name(format_name);

  if(format == NULL
     || dt_bauhaus_combobox_set_from_text(d->format, format->name()) == FALSE)
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
      const dt_colorspaces_color_profile_t *pp = (dt_colorspaces_color_profile_t *)profiles->data;
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
  dt_conf_set_string(CONFIG_PREFIX "resizing",
                     d_type == DT_DIMENSIONS_SCALE ? "scaling" : "max_size");
  if(d_type == DT_DIMENSIONS_CM || d_type == DT_DIMENSIONS_INCH)
  {
    // set dpi to user-set dpi
    dt_conf_set_int("metadata/resolution", dt_conf_get_int(CONFIG_PREFIX "print_dpi"));
    _resync_print_dimensions(d);
  }
  else
  {
    // reset export dpi to default value for scale/pixel specific export
    dt_conf_set_int("metadata/resolution", dt_confgen_get_int("metadata/resolution", DT_DEFAULT));
  }
  _size_update_display(d);
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

  const dt_lib_export_t *d = (dt_lib_export_t *)user_data;
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

  const dt_lib_export_t *d = (dt_lib_export_t *)user_data;
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
  dt_conf_set_int("metadata/resolution", dpi);

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
    gtk_widget_set_visible(GTK_WIDGET(d->style_mode), FALSE);
  }
  else
  {
    const gchar *style = dt_bauhaus_combobox_get_text(d->style);
    dt_conf_set_string(CONFIG_PREFIX "style", style);
    gtk_widget_set_visible(GTK_WIDGET(d->style_mode), TRUE);
  }
}

int position(const dt_lib_module_t *self)
{
  return 0;
}

static void _update_formats_combobox(dt_lib_export_t *d)
{
  // Clear format combo box
  dt_bauhaus_combobox_clear(d->format);

  // Get current selected storage
  const char *storage_name = dt_conf_get_string_const(CONFIG_PREFIX "storage_name");
  dt_imageio_module_storage_t *storage = dt_imageio_get_storage_by_name(storage_name);

  // Add supported formats to combobox
  gboolean empty = TRUE;
  for(const GList *it = darktable.imageio->plugins_format; it; it = g_list_next(it))
  {
    dt_imageio_module_format_t *format = (dt_imageio_module_format_t *)it->data;
    if(storage->supported(storage, format))
    {
      dt_bauhaus_combobox_add(d->format, format->name());
      empty = FALSE;
    }
  }

  gtk_widget_set_sensitive(d->format, !empty);
}

static void _on_storage_list_changed(gpointer instance, dt_lib_module_t *self)
{
  dt_lib_export_t *d = self->data;
  dt_imageio_module_storage_t *storage = dt_imageio_get_storage();
  dt_bauhaus_combobox_clear(d->storage);

  dt_gui_container_remove_children(GTK_CONTAINER(d->storage_extra_container));

  for(const GList *it = darktable.imageio->plugins_storage; it; it = g_list_next(it))
  {
    const dt_imageio_module_storage_t *module = (dt_imageio_module_storage_t *)it->data;
    dt_bauhaus_combobox_add(d->storage, module->name(module));
    if(module->widget)
    {
      gtk_container_add(GTK_CONTAINER(d->storage_extra_container), module->widget);
    }
  }
  dt_bauhaus_combobox_set(d->storage, dt_imageio_get_index_of_storage(storage));
}

static void _lib_export_styles_changed_callback(gpointer instance, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_export_t *d = self->data;

  dt_bauhaus_combobox_clear(d->style);
  dt_bauhaus_combobox_add(d->style, _("none"));

  GList *styles = dt_styles_get_list("");
  for(const GList *st_iter = styles; st_iter; st_iter = g_list_next(st_iter))
  {
    const dt_style_t *style = (dt_style_t *)st_iter->data;
    dt_bauhaus_combobox_add(d->style, style->name);
  }
  dt_bauhaus_combobox_set(d->style, 0);

  g_list_free_full(styles, dt_style_free);
}

void _menuitem_preferences(GtkMenuItem *menuitem, dt_lib_module_t *self)
{
  dt_lib_export_t *d = (dt_lib_export_t *)self->data;
  const gchar *name = dt_bauhaus_combobox_get_text(d->storage);
  const gboolean ondisk = name && !g_strcmp0(name, _("file on disk")); // FIXME: NO!!!!!one!
  d->metadata_export = dt_lib_export_metadata_configuration_dialog(d->metadata_export, ondisk);
}

void set_preferences(void *menu, dt_lib_module_t *self)
{
  GtkWidget *mi = gtk_menu_item_new_with_label(_("preferences..."));
  g_signal_connect(G_OBJECT(mi), "activate", G_CALLBACK(_menuitem_preferences), self);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
}

void gui_init(dt_lib_module_t *self)
{
  dt_lib_export_t *d = (dt_lib_export_t *)malloc(sizeof(dt_lib_export_t));
  self->timeout_handle = 0;
  self->data = (void *)d;
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  dt_action_insert_sorted(DT_ACTION(self), &darktable.control->actions_format);
  dt_action_insert_sorted(DT_ACTION(self), &darktable.control->actions_storage);

  GtkWidget *label = dt_ui_section_label_new(_("storage options"));
  gtk_box_pack_start(GTK_BOX(self->widget), label, FALSE, TRUE, 0);

  d->storage = dt_bauhaus_combobox_new_action(DT_ACTION(self));
  dt_bauhaus_widget_set_label(d->storage, NULL, N_("target storage"));
  gtk_box_pack_start(GTK_BOX(self->widget), d->storage, FALSE, TRUE, 0);

  // add all storage widgets to the stack widget
  d->storage_extra_container = gtk_stack_new();
  gtk_stack_set_homogeneous(GTK_STACK(d->storage_extra_container),FALSE);
  gtk_box_pack_start(GTK_BOX(self->widget), d->storage_extra_container, FALSE, TRUE, 0);
  for(const GList *it = darktable.imageio->plugins_storage; it; it = g_list_next(it))
  {
    const dt_imageio_module_storage_t *module = (dt_imageio_module_storage_t *)it->data;
    dt_bauhaus_combobox_add(d->storage, module->name(module));
    if(module->widget)
    {
      gtk_container_add(GTK_CONTAINER(d->storage_extra_container), module->widget);
    }
  }

  // postponed so we can do the two steps in one loop
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_IMAGEIO_STORAGE_CHANGE,
                            G_CALLBACK(_on_storage_list_changed), self);
  g_signal_connect(G_OBJECT(d->storage), "value-changed", G_CALLBACK(_storage_changed), (gpointer)d);

  label = dt_ui_section_label_new(_("format options"));
  gtk_box_pack_start(GTK_BOX(self->widget), label, FALSE, TRUE, 0);

  d->format = dt_bauhaus_combobox_new_action(DT_ACTION(self));
  dt_bauhaus_widget_set_label(d->format, NULL, N_("file format"));
  gtk_box_pack_start(GTK_BOX(self->widget), d->format, FALSE, TRUE, 0);
  g_signal_connect(G_OBJECT(d->format), "value-changed", G_CALLBACK(_format_changed), (gpointer)d);

  // add all format widgets to the stack widget
  d->format_extra_container = gtk_stack_new();
  gtk_stack_set_homogeneous(GTK_STACK(d->format_extra_container),FALSE);
  gtk_box_pack_start(GTK_BOX(self->widget), d->format_extra_container, FALSE, TRUE, 0);
  for(const GList *it = darktable.imageio->plugins_format; it; it = g_list_next(it))
  {
    const dt_imageio_module_format_t *module = (dt_imageio_module_format_t *)it->data;
    if(module->widget)
    {
      gtk_container_add(GTK_CONTAINER(d->format_extra_container), module->widget);
    }
  }

  label = dt_ui_section_label_new(_("global options"));
  gtk_box_pack_start(GTK_BOX(self->widget), label, FALSE, TRUE, 0);

  DT_BAUHAUS_COMBOBOX_NEW_FULL(d->dimensions_type, self, NULL, N_("set size"),
                               _("choose a method for setting the output size"),
                               dt_conf_get_int(CONFIG_PREFIX "dimensions_type"),
                               _dimensions_type_changed, d,
                               N_("in pixels (for file)"),
                               N_("in cm (for print)"),
                               N_("in inch (for print)"),
                               N_("by scale (for file)"));

  d->print_width = dt_action_entry_new(DT_ACTION(self), N_("print width"), G_CALLBACK(_print_width_changed), d,
                                       _("maximum output width limit.\n"
                                         "click middle mouse button to reset to 0."), NULL);

  d->print_height = dt_action_entry_new(DT_ACTION(self), N_("print height"), G_CALLBACK(_print_height_changed), d,
                                       _("maximum output height limit.\n"
                                         "click middle mouse button to reset to 0."), NULL);

  d->print_dpi = dt_action_entry_new(DT_ACTION(self), N_("dpi"), G_CALLBACK(_print_dpi_changed), d,
                                     _("resolution in dot per inch"),
                                     dt_conf_get_string_const(CONFIG_PREFIX "print_dpi"));

  d->width = dt_action_entry_new(DT_ACTION(self), N_("width"), G_CALLBACK(_width_changed), d,
                                 _("maximum output width limit.\n"
                                   "click middle mouse button to reset to 0."), NULL);

  d->height = dt_action_entry_new(DT_ACTION(self), N_("height"), G_CALLBACK(_height_changed), d,
                                  _("maximum output height limit.\n"
                                    "click middle mouse button to reset to 0."), NULL);

  d->print_size = gtk_flow_box_new();
  gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(d->print_size), 5);
  gtk_flow_box_set_column_spacing (GTK_FLOW_BOX(d->print_size), 3);
  gtk_container_add(GTK_CONTAINER(d->print_size), d->print_width);
  gtk_container_add(GTK_CONTAINER(d->print_size), gtk_label_new(_("x")));
  gtk_container_add(GTK_CONTAINER(d->print_size), d->print_height);
  d->unit_label = gtk_label_new(_("cm"));
  gtk_container_add(GTK_CONTAINER(d->print_size), d->unit_label);
  GtkBox *dpi_box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 3));
  gtk_box_pack_start(dpi_box, gtk_label_new(_("@")), FALSE, FALSE, 0);
  gtk_box_pack_start(dpi_box, d->print_dpi, TRUE, TRUE, 0);
  gtk_box_pack_start(dpi_box, gtk_label_new(_("dpi")), FALSE, FALSE, 0);
  gtk_container_add(GTK_CONTAINER(d->print_size), GTK_WIDGET(dpi_box));
  gtk_container_foreach(GTK_CONTAINER(d->print_size), (GtkCallback)gtk_widget_set_can_focus, GINT_TO_POINTER(FALSE));

  d->px_size = gtk_flow_box_new();
  gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(d->px_size), 3);
  gtk_flow_box_set_column_spacing (GTK_FLOW_BOX(d->px_size), 3);
  gtk_container_add(GTK_CONTAINER(d->px_size), d->width);
  gtk_container_add(GTK_CONTAINER(d->px_size), gtk_label_new(_("x")));
  GtkBox *px_box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 3));
  gtk_box_pack_start(px_box, d->height, TRUE, TRUE, 0);
  gtk_box_pack_start(px_box, gtk_label_new(_("px")), FALSE, FALSE, 0);
  gtk_container_add(GTK_CONTAINER(d->px_size), GTK_WIDGET(px_box));
  gtk_container_foreach(GTK_CONTAINER(d->px_size), (GtkCallback)gtk_widget_set_can_focus, GINT_TO_POINTER(FALSE));

  d->scale = dt_action_entry_new(DT_ACTION(self), N_("scale"), G_CALLBACK(_scale_changed), d,
                                 _("it can be an integer, decimal number or simple fraction.\n"
                                   "zero or empty values are equal to 1.\n"
                                   "click middle mouse button to reset to 1."),
                                 dt_conf_get_string_const(CONFIG_PREFIX "resizing_factor"));
  gtk_widget_set_halign(GTK_WIDGET(d->scale), GTK_ALIGN_END);

  d->size_in_px = gtk_label_new("");
  gtk_label_set_ellipsize(GTK_LABEL(d->size_in_px), PANGO_ELLIPSIZE_START);
  gtk_widget_set_sensitive(GTK_WIDGET(d->size_in_px), FALSE);

  gtk_widget_set_halign(GTK_WIDGET(d->scale), GTK_ALIGN_FILL);
  gtk_widget_set_halign(GTK_WIDGET(d->size_in_px), GTK_ALIGN_END);

  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(d->dimensions_type), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(d->px_size), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(d->print_size), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(d->scale), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(d->size_in_px), FALSE, FALSE, 0);

  DT_BAUHAUS_COMBOBOX_NEW_FULL(d->upscale, self, NULL, N_("allow upscaling"), NULL,
                               dt_conf_get_bool(CONFIG_PREFIX "upscale") ? 1 : 0, _callback_bool,
                               (gpointer)CONFIG_PREFIX "upscale",
                               N_("no"), N_("yes"));
  gtk_box_pack_start(GTK_BOX(self->widget), d->upscale, FALSE, TRUE, 0);

  DT_BAUHAUS_COMBOBOX_NEW_FULL(d->high_quality, self, NULL, N_("high quality resampling"),
                               _("do high quality resampling during export"),
                               dt_conf_get_bool(CONFIG_PREFIX "high_quality_processing") ? 1 : 0, _callback_bool,
                               (gpointer)CONFIG_PREFIX "high_quality_processing",
                               N_("no"), N_("yes"));
  gtk_box_pack_start(GTK_BOX(self->widget), d->high_quality, FALSE, TRUE, 0);

  DT_BAUHAUS_COMBOBOX_NEW_FULL(d->export_masks, self, NULL, N_("store masks"),
                               _("store masks as layers in exported images. only works for some formats."),
                               dt_conf_get_bool(CONFIG_PREFIX "export_masks") ? 1 : 0, _callback_bool,
                               (gpointer)CONFIG_PREFIX "export_masks",
                               N_("no"), N_("yes"));
  gtk_box_pack_start(GTK_BOX(self->widget), d->export_masks, FALSE, TRUE, 0);

  //  Add profile combo

  char datadir[PATH_MAX] = { 0 };
  char confdir[PATH_MAX] = { 0 };
  dt_loc_get_user_config_dir(confdir, sizeof(confdir));
  dt_loc_get_datadir(datadir, sizeof(datadir));

  d->profile = dt_bauhaus_combobox_new_action(DT_ACTION(self));
  dt_bauhaus_widget_set_label(d->profile, NULL, N_("profile"));
  gtk_box_pack_start(GTK_BOX(self->widget), d->profile, FALSE, TRUE, 0);
  dt_bauhaus_combobox_add(d->profile, _("image settings"));
  for(GList *l = darktable.color_profiles->profiles; l; l = g_list_next(l))
  {
    const dt_colorspaces_color_profile_t *prof = (dt_colorspaces_color_profile_t *)l->data;
    if(prof->out_pos > -1)
      dt_bauhaus_combobox_add(d->profile, prof->name);
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

  DT_BAUHAUS_COMBOBOX_NEW_FULL(d->intent, self, NULL, N_("intent"),
                               _("• perceptual: "
                              "smoothly moves out-of-gamut colors into gamut,"
                              " preserving gradations, but distorts in-gamut colors in the process."
                              " note that perceptual is often a proprietary LUT that depends"
                              " on the destination space."
                              "\n\n"

                              "• relative colorimetric: "
                              "keeps luminance while reducing as little as possible saturation"
                              " until colors fit in gamut."
                              "\n\n"

                              "• saturation: "
                              "designed to present eye-catching business graphics"
                              " by preserving the saturation. (not suited for photography)."
                              "\n\n"

                              "• absolute colorimetric: "
                              "adapt white point of the image to the white point of the"
                              " destination medium and do nothing else. mainly used when"
                              " proofing colors. (not suited for photography)."),
                               0, _intent_changed, self,
                               N_("image settings"),
                               N_("perceptual"),
                               N_("relative colorimetric"),
                               NC_("rendering intent", "saturation"),
                               N_("absolute colorimetric"));
  gtk_box_pack_start(GTK_BOX(self->widget), d->intent, FALSE, TRUE, 0);

  //  Add style combo

  d->style = dt_bauhaus_combobox_new_action(DT_ACTION(self));
  dt_bauhaus_widget_set_label(d->style, NULL, N_("style"));
  _lib_export_styles_changed_callback(NULL, self);
  gtk_box_pack_start(GTK_BOX(self->widget), d->style, FALSE, TRUE, 0);
  gtk_widget_set_tooltip_text(d->style, _("temporary style to use while exporting"));

  //  Add check to control whether the style is to replace or append the current module

  DT_BAUHAUS_COMBOBOX_NEW_FULL(d->style_mode, self, NULL, N_("mode"),
                               _("whether the style items are appended to the history or replacing the history"),
                               dt_conf_get_bool(CONFIG_PREFIX "style_append") ? 1 : 0, _callback_bool,
                               (gpointer)CONFIG_PREFIX "style_append",
                               N_("replace history"), N_("append history"));
  gtk_box_pack_start(GTK_BOX(self->widget), d->style_mode, FALSE, TRUE, 0);

  //  Set callback signals

  g_signal_connect(G_OBJECT(d->profile), "value-changed", G_CALLBACK(_profile_changed), (gpointer)d);
  g_signal_connect(G_OBJECT(d->style), "value-changed", G_CALLBACK(_style_changed), (gpointer)d);

  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_STYLE_CHANGED,
                            G_CALLBACK(_lib_export_styles_changed_callback), self);

  GtkBox *hbox = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hbox), FALSE, TRUE, 0);

  // Export button
  d->export_button = GTK_BUTTON(dt_action_button_new(self, N_("export"), _export_button_clicked, d,
                                                     _("export with current settings"), GDK_KEY_e, GDK_CONTROL_MASK));
  gtk_box_pack_start(hbox, GTK_WIDGET(d->export_button), TRUE, TRUE, 0);

  gtk_widget_add_events(d->width, GDK_BUTTON_PRESS_MASK);
  gtk_widget_add_events(d->height, GDK_BUTTON_PRESS_MASK);
  gtk_widget_add_events(d->print_width, GDK_BUTTON_PRESS_MASK);
  gtk_widget_add_events(d->print_height, GDK_BUTTON_PRESS_MASK);
  gtk_widget_add_events(d->scale, GDK_BUTTON_PRESS_MASK);

  g_signal_connect(G_OBJECT(d->width), "button-press-event", G_CALLBACK(_widht_mdlclick), (gpointer)d);
  g_signal_connect(G_OBJECT(d->height), "button-press-event", G_CALLBACK(_height_mdlclick), (gpointer)d);
  g_signal_connect(G_OBJECT(d->print_width), "button-press-event", G_CALLBACK(_widht_mdlclick), (gpointer)d);
  g_signal_connect(G_OBJECT(d->print_height), "button-press-event", G_CALLBACK(_height_mdlclick), (gpointer)d);
  g_signal_connect(G_OBJECT(d->scale), "button-press-event", G_CALLBACK(_scale_mdlclick), (gpointer)d);

  // this takes care of keeping hidden widgets hidden
  gtk_widget_show_all(self->widget);
  gtk_widget_set_no_show_all(self->widget, TRUE);

  const char* setting = dt_conf_get_string_const(CONFIG_PREFIX "width");
  gtk_entry_set_text(GTK_ENTRY(d->width), setting);
  setting = dt_conf_get_string_const(CONFIG_PREFIX "height");
  gtk_entry_set_text(GTK_ENTRY(d->height), setting);

  _size_update_display(d);

  // Set storage
  setting = dt_conf_get_string_const(CONFIG_PREFIX "storage_name");
  const int storage_index = dt_imageio_get_index_of_storage(dt_imageio_get_storage_by_name(setting));
  dt_bauhaus_combobox_set(d->storage, storage_index);

  dt_bauhaus_combobox_set(d->intent, dt_conf_get_int(CONFIG_PREFIX "iccintent") + 1);

  // iccprofile
  const int icctype = dt_conf_get_int(CONFIG_PREFIX "icctype");
  gchar *iccfilename = dt_conf_get_string(CONFIG_PREFIX "iccprofile");
  dt_bauhaus_combobox_set(d->profile, 0);
  if(icctype != DT_COLORSPACE_NONE)
  {
    for(GList *profiles = darktable.color_profiles->profiles; profiles; profiles = g_list_next(profiles))
    {
      const dt_colorspaces_color_profile_t *pp = (dt_colorspaces_color_profile_t *)profiles->data;
      if(pp->out_pos > -1
         && icctype == pp->type
         && (icctype != DT_COLORSPACE_FILE || !strcmp(iccfilename, pp->filename)))
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
  setting = dt_conf_get_string_const(CONFIG_PREFIX "style");
  if(setting != NULL && strlen(setting) > 0)
  {
    rc = dt_bauhaus_combobox_set_from_text(d->style, setting);
    if(rc == FALSE)
      dt_bauhaus_combobox_set(d->style, 0);
  }
  else
    dt_bauhaus_combobox_set(d->style, 0);

  // style mode to overwrite as it was the initial behavior
  gtk_widget_set_no_show_all(d->style_mode, TRUE);
  gtk_widget_set_visible(d->style_mode, dt_bauhaus_combobox_get(d->style)==0?FALSE:TRUE);

  // export metadata presets
  d->metadata_export = dt_lib_export_metadata_get_conf();

  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_SELECTION_CHANGED,
                            G_CALLBACK(_image_selection_changed_callback), self);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_MOUSE_OVER_IMAGE_CHANGE,
                            G_CALLBACK(_mouse_over_image_callback), self);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_COLLECTION_CHANGED,
                            G_CALLBACK(_collection_updated_callback), self);
}

void gui_cleanup(dt_lib_module_t *self)
{
  dt_lib_cancel_postponed_update(self);
  dt_lib_export_t *d = (dt_lib_export_t *)self->data;

  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_on_storage_list_changed), self);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_lib_export_styles_changed_callback), self);

  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_image_selection_changed_callback), self);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_mouse_over_image_callback), self);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_collection_updated_callback), self);

  for(const GList *it = darktable.imageio->plugins_storage; it; it = g_list_next(it))
  {
    dt_imageio_module_storage_t *module = (dt_imageio_module_storage_t *)it->data;
    if(module->widget) gtk_container_remove(GTK_CONTAINER(d->storage_extra_container), module->widget);
  }

  for(const GList *it = darktable.imageio->plugins_format; it; it = g_list_next(it))
  {
    dt_imageio_module_format_t *module = (dt_imageio_module_format_t *)it->data;
    if(module->widget) gtk_container_remove(GTK_CONTAINER(d->format_extra_container), module->widget);
  }

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
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                  "DELETE FROM data.presets WHERE rowid=?1", -1,
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
      const int32_t new_fversion = fmod->version(), new_sversion = smod->version();

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
        const size_t new_params_size = op_params_size - (fsize + ssize) + (new_fsize + new_ssize);
        void *new_params = malloc(new_params_size);
        memcpy(new_params, op_params, copy_over_part);
        // next we have fversion, sversion, fsize, ssize, fdata, sdata which is the stuff that might change
        size_t pos = copy_over_part;
        memcpy((uint8_t *)new_params + pos, &new_fversion, sizeof(int32_t));
        pos += sizeof(int32_t);
        memcpy((uint8_t *)new_params + pos, &new_sversion, sizeof(int32_t));
        pos += sizeof(int32_t);
        memcpy((uint8_t *)new_params + pos, &new_fsize, sizeof(int32_t));
        pos += sizeof(int32_t);
        memcpy((uint8_t *)new_params + pos, &new_ssize, sizeof(int32_t));
        pos += sizeof(int32_t);
        if(new_fdata)
          memcpy((uint8_t *)new_params + pos, new_fdata, new_fsize);
        else
          memcpy((uint8_t *)new_params + pos, fdata, fsize);
        pos += new_fsize;
        if(new_sdata)
          memcpy((uint8_t *)new_params + pos, new_sdata, new_ssize);
        else
          memcpy((uint8_t *)new_params + pos, sdata, ssize);

        // write the updated preset back to db
        fprintf(stderr,
                "[export_init_presets] updating export preset '%s' from versions %d/%d to versions %d/%d\n",
                name, fversion, sversion, new_fversion, new_sversion);
        sqlite3_stmt *innerstmt;
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                    "UPDATE data.presets SET op_params=?1 WHERE rowid=?2",
                                    -1, &innerstmt, NULL);
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
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                  "DELETE FROM data.presets WHERE rowid=?1", -1,
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
    const size_t new_params_size = old_params_size + 2 * sizeof(int32_t);
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
    memcpy((uint8_t *)new_params + first_half, &fversion, sizeof(int32_t));
    memcpy((uint8_t *)new_params + first_half + sizeof(int32_t), &sversion, sizeof(int32_t));
    // copy the rest of the old params over
    memcpy((uint8_t *)new_params + first_half + sizeof(int32_t) * 2, buf, old_params_size - first_half);

    *new_size = new_params_size;
    *new_version = 2;
    return new_params;
  }
  else if(old_version == 2)
  {
    // add upscale to params
    const size_t new_params_size = old_params_size + sizeof(int32_t);
    void *new_params = calloc(1, new_params_size);

    memcpy(new_params, old_params, sizeof(int32_t) * 2);
    memcpy((uint8_t *)new_params + sizeof(int32_t) * 3, (uint8_t *)old_params + sizeof(int32_t) * 2, old_params_size - sizeof(int32_t) * 2);

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
    memcpy(new_params, old_params, sizeof(int32_t) * 4);
    pos += 4 * sizeof(int32_t);
    memcpy((uint8_t *)new_params + pos, &icctype, sizeof(int32_t));
    pos += sizeof(int32_t);
    memcpy((uint8_t *)new_params + pos, iccfilename, strlen(iccfilename) + 1);
    pos += strlen(iccfilename) + 1;
    size_t old_pos = 4 * sizeof(int32_t) + strlen(iccprofile) + 1;
    memcpy((uint8_t *)new_params + pos, (uint8_t *)old_params + old_pos, old_params_size - old_pos);

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

    const size_t new_params_size = old_params_size + sizeof(int32_t);
    void *new_params = calloc(1, new_params_size);

    size_t pos = 0;
    memcpy(new_params, old_params, sizeof(int32_t) * 3);
    pos += 4 * sizeof(int32_t);
    memcpy((uint8_t *)new_params + pos, (uint8_t *)old_params + pos - sizeof(int32_t), old_params_size - sizeof(int32_t) * 3);

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
    gchar *flags = g_strdup_printf("%x", dt_lib_export_metadata_default_flags() | (omit ? DT_META_OMIT_HIERARCHY : 0));
    const int flags_size = strlen(flags) + 1;
    const size_t new_params_size = old_params_size + flags_size;
    void *new_params = calloc(1, new_params_size);
    size_t pos = 0;
    memcpy(new_params, old_params, sizeof(int32_t) * 6);
    pos += 6 * sizeof(int32_t);
    memcpy((uint8_t *)new_params + pos, flags, flags_size);
    pos += flags_size;
    memcpy((uint8_t *)new_params + pos, (uint8_t *)old_params + pos - flags_size, old_params_size - sizeof(int32_t) * 6);

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
    memcpy(new_params, old_params, sizeof(int32_t) * 4);
    pos += 5 * sizeof(int32_t);
    memcpy((uint8_t *)new_params + pos, (uint8_t *)old_params + pos - sizeof(int32_t), old_params_size - sizeof(int32_t) * 4);

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
  const int32_t fversion = mformat->version();
  const int32_t sversion = mstorage->version();
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

  const char *fname = mformat->plugin_name;
  const char *sname = mstorage->plugin_name;
  const int32_t fname_len = strlen(fname);
  const int32_t sname_len = strlen(sname);

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
      const dt_colorspaces_color_profile_t *pp = (dt_colorspaces_color_profile_t *)iter->data;
      if(pp->out_pos > -1
         && icctype == pp->type
         && (icctype != DT_COLORSPACE_FILE || !strcmp(iccfilename, pp->filename)))
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

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
