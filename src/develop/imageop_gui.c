/*
    This file is part of darktable,
    Copyright (C) 2009-2021 darktable developers.

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

#include "develop/imageop_gui.h"
#include "develop/imageop.h"
#include "bauhaus/bauhaus.h"
#include "dtgtk/button.h"
#include "gui/accelerators.h"

#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif

#include <assert.h>
#include <gmodule.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#if defined(__SSE__)
#include <xmmintrin.h>
#endif
#include <time.h>

typedef struct dt_module_param_t
{
  dt_iop_module_t *module;
  void            *param;
} dt_module_param_t;

static void _iop_toggle_callback(GtkWidget *togglebutton, dt_module_param_t *data)
{
  if(darktable.gui->reset) return;

  dt_iop_module_t *self = data->module;
  gboolean *field = (gboolean*)(data->param);

  gboolean previous = *field;
  *field = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(togglebutton));

  if(*field != previous)
  {
    dt_iop_gui_changed(DT_ACTION(self), togglebutton, &previous);
  }
}

static gchar *_section_from_package(dt_iop_module_t **self)
{
  if((*self)->actions != DT_ACTION_TYPE_IOP_SECTION) return NULL;

  dt_iop_module_section_t *package = (dt_iop_module_section_t *)*self;
  *self = package->self;
  return package->section;
}

static void _store_intro_section(const dt_introspection_field_t *f, gchar *section)
{
  if(section)
  {
    GHashTable **sections = &f->header.so->get_introspection()->sections;
    if(!*sections) *sections = g_hash_table_new(NULL, NULL);
    g_hash_table_insert(*sections, GINT_TO_POINTER(f->header.offset), section);
  }
}

GtkWidget *dt_bauhaus_slider_from_params(dt_iop_module_t *self, const char *param)
{
  gchar *section = _section_from_package(&self);

  dt_iop_params_t *p = (dt_iop_params_t *)self->params;
  dt_iop_params_t *d = (dt_iop_params_t *)self->default_params;

  size_t param_index = 0;
  gboolean skip_label = FALSE;

  const size_t param_length = strlen(param) + 1;
  char *param_name = g_malloc(param_length);
  char *base_name = g_malloc(param_length);
  if(sscanf(param, "%[^[][%zu]", base_name, &param_index) == 2)
  {
    sprintf(param_name, "%s[0]", base_name);
    skip_label = !section;
  }
  else
  {
    memcpy(param_name, param, param_length);
  }
  g_free(base_name);

  const dt_introspection_field_t *f = self->so->get_f(param_name);

  GtkWidget *slider = NULL;
  size_t offset = 0;

  if(f)
  {
    if(f->header.type == DT_INTROSPECTION_TYPE_FLOAT)
    {
      const float min = f->Float.Min;
      const float max = f->Float.Max;
      offset = f->header.offset + param_index * sizeof(float);
      const float defval = *(float*)((uint8_t *)d + offset);

      const float top = fminf(max-min, fmaxf(fabsf(min), fabsf(max)));
      const int digits = MAX(2, -floorf(log10f(top/100)+.1));

      slider = dt_bauhaus_slider_new_with_range_and_feedback(self, min, max, 0, defval, digits, 1);
    }
    else if(f->header.type == DT_INTROSPECTION_TYPE_INT)
    {
      const int min = f->Int.Min;
      const int max = f->Int.Max;
      offset = f->header.offset + param_index * sizeof(int);
      const int defval = *(int*)((uint8_t *)d + offset);

      slider = dt_bauhaus_slider_new_with_range_and_feedback(self, min, max, 1, defval, 0, 1);
    }
    else if(f->header.type == DT_INTROSPECTION_TYPE_USHORT)
    {
      const unsigned short min = f->UShort.Min;
      const unsigned short max = f->UShort.Max;
      offset = f->header.offset + param_index * sizeof(unsigned short);
      const unsigned short defval = *(unsigned short*)((uint8_t *)d + offset);

      slider = dt_bauhaus_slider_new_with_range_and_feedback(self, min, max, 1, defval, 0, 1);
    }
    else f = NULL;
  }

  if(f)
  {
    dt_bauhaus_widget_set_field(slider, (uint8_t *)p + offset, f->header.type);
    _store_intro_section(f, section);

    if(!skip_label)
    {
      if(*f->header.description)
      {
        // we do not want to support a context as it break all translations see #5498
        // dt_bauhaus_widget_set_label(slider, NULL, g_dpgettext2(NULL, "introspection description", f->header.description));
        dt_bauhaus_widget_set_label(slider, section, f->header.description);
      }
      else
      {
        gchar *str = dt_util_str_replace(param, "_", " ");

        dt_bauhaus_widget_set_label(slider,  section, str);

        g_free(str);
      }
    }
  }
  else
  {
    gchar *str = g_strdup_printf("'%s' is not a float/int/unsigned short/slider parameter", param_name);

    slider = dt_bauhaus_slider_new(self);
    dt_bauhaus_widget_set_label(slider, NULL, str);

    g_free(str);
  }

  if(!self->widget) self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
  gtk_box_pack_start(GTK_BOX(self->widget), slider, FALSE, FALSE, 0);

  g_free(param_name);

  return slider;
}

GtkWidget *dt_bauhaus_combobox_from_params(dt_iop_module_t *self, const char *param)
{
  gchar *section = _section_from_package(&self);

  dt_iop_params_t *p = (dt_iop_params_t *)self->params;
  dt_iop_params_t *d = (dt_iop_params_t *)self->default_params;
  dt_introspection_field_t *f = self->so->get_f(param);

  GtkWidget *combobox = dt_bauhaus_combobox_new(self);
  gchar *str = NULL;

  if(f && (f->header.type == DT_INTROSPECTION_TYPE_ENUM ||
            f->header.type == DT_INTROSPECTION_TYPE_INT  ||
            f->header.type == DT_INTROSPECTION_TYPE_UINT ||
            f->header.type == DT_INTROSPECTION_TYPE_BOOL ))
  {
    dt_bauhaus_widget_set_field(combobox, (uint8_t *)p + f->header.offset, f->header.type);
    _store_intro_section(f, section);

    str = *f->header.description ? g_strdup(f->header.description)
                                 : dt_util_str_replace(param, "_", " ");

    dt_action_t *action = dt_bauhaus_widget_set_label(combobox, section, str);

    if(f->header.type == DT_INTROSPECTION_TYPE_BOOL)
    {
      dt_bauhaus_combobox_add(combobox, _("no"));
      dt_bauhaus_combobox_add(combobox, _("yes"));
      dt_bauhaus_combobox_set_default(combobox, *(gboolean*)((uint8_t *)d + f->header.offset));
    }
    else if(f->header.type == DT_INTROSPECTION_TYPE_ENUM)
    {
      for(dt_introspection_type_enum_tuple_t *iter = f->Enum.values; iter && iter->name; iter++)
      {
        // we do not want to support a context as it break all translations see #5498
        // dt_bauhaus_combobox_add_full(combobox, g_dpgettext2(NULL, "introspection description", iter->description), DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT, GINT_TO_POINTER(iter->value), NULL, TRUE);
        if(*iter->description)
          dt_bauhaus_combobox_add_full(combobox, gettext(iter->description), DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT, GINT_TO_POINTER(iter->value), NULL, TRUE);
      }
      dt_bauhaus_combobox_set_default(combobox, *(int*)((uint8_t *)d + f->header.offset));

      if(action && f->Enum.values)
        g_hash_table_insert(darktable.control->combo_introspection, action, f->Enum.values);
    }
  }
  else
  {
    str = g_strdup_printf("'%s' is not an enum/int/bool/combobox parameter", param);

    dt_bauhaus_widget_set_label(combobox, section, str);
  }

  g_free(str);

  if(!self->widget) self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
  gtk_box_pack_start(GTK_BOX(self->widget), combobox, FALSE, FALSE, 0);

  return combobox;
}

GtkWidget *dt_bauhaus_toggle_from_params(dt_iop_module_t *self, const char *param)
{
  gchar *section = _section_from_package(&self);

  dt_iop_params_t *p = (dt_iop_params_t *)self->params;
  dt_introspection_field_t *f = self->so->get_f(param);

  GtkWidget *button = NULL;
  gchar *str = NULL;

  if(f && f->header.type == DT_INTROSPECTION_TYPE_BOOL)
  {
    // we do not want to support a context as it break all translations see #5498
    // button = gtk_check_button_new_with_label(g_dpgettext2(NULL, "introspection description", f->header.description));
    str = *f->header.description
        ? g_strdup(f->header.description)
        : dt_util_str_replace(param, "_", " ");

    GtkWidget *label = gtk_label_new(_(str));
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    button = gtk_check_button_new();
    gtk_container_add(GTK_CONTAINER(button), label);
    dt_module_param_t *module_param = (dt_module_param_t *)g_malloc(sizeof(dt_module_param_t));
    module_param->module = self;
    module_param->param = (uint8_t *)p + f->header.offset;
    g_signal_connect_data(G_OBJECT(button), "toggled", G_CALLBACK(_iop_toggle_callback), module_param, (GClosureNotify)g_free, 0);

    _store_intro_section(f, section);
    dt_action_define_iop(self, section, str, button, &dt_action_def_toggle);
  }
  else
  {
    str = g_strdup_printf("'%s' is not a bool/togglebutton parameter", param);

    button = gtk_check_button_new_with_label(str);
  }

  g_free(str);
  if(!self->widget) self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
  gtk_box_pack_start(GTK_BOX(self->widget), button, FALSE, FALSE, 0);

  return button;
}

GtkWidget *dt_iop_togglebutton_new(dt_iop_module_t *self, const char *section, const gchar *label, const gchar *ctrl_label,
                                   GCallback callback, gboolean local, guint accel_key, GdkModifierType mods,
                                   DTGTKCairoPaintIconFunc paint, GtkWidget *box)
{
  GtkWidget *w = dtgtk_togglebutton_new(paint, 0, NULL);
  g_signal_connect(G_OBJECT(w), "button-press-event", callback, self);

  if(!ctrl_label)
    gtk_widget_set_tooltip_text(w, _(label));
  else
  {
    gchar *tooltip = g_strdup_printf(_("%s\nctrl+click to %s"), _(label), _(ctrl_label));
    gtk_widget_set_tooltip_text(w, tooltip);
    g_free(tooltip);
  }

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w), FALSE);
  if(GTK_IS_BOX(box)) gtk_box_pack_end(GTK_BOX(box), w, FALSE, FALSE, 0);

  dt_action_define_iop(self, section, label, w, &dt_action_def_toggle);

  return w;
}

GtkWidget *dt_iop_button_new(dt_iop_module_t *self, const gchar *label,
                             GCallback callback, gboolean local, guint accel_key, GdkModifierType mods,
                             DTGTKCairoPaintIconFunc paint, gint paintflags, GtkWidget *box)
{
  GtkWidget *button = NULL;

  if(paint)
  {
    button = dtgtk_button_new(paint, paintflags, NULL);
    gtk_widget_set_tooltip_text(button, Q_(label));
  }
  else
  {
    button = gtk_button_new_with_label(Q_(label));
    gtk_label_set_ellipsize(GTK_LABEL(gtk_bin_get_child(GTK_BIN(button))), PANGO_ELLIPSIZE_END);
  }

  g_signal_connect(G_OBJECT(button), "clicked", callback, (gpointer)self);

  dt_action_t *ac = dt_action_define_iop(self, NULL, label, button, &dt_action_def_button);
  if(darktable.control->accel_initialising)
    dt_shortcut_register(ac, 0, 0, accel_key, mods);

  if(GTK_IS_BOX(box)) gtk_box_pack_start(GTK_BOX(box), button, TRUE, TRUE, 0);

  return button;
}

gboolean dt_mask_scroll_increases(int up)
{
  const gboolean mask_down = dt_conf_get_bool("masks_scroll_down_increases");
  return up ? !mask_down : mask_down;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

