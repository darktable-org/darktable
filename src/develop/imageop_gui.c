/*
    This file is part of darktable,
    Copyright (C) 2009-2026 darktable developers.

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
#include "develop/develop.h"
#include "bauhaus/bauhaus.h"
#include "dtgtk/button.h"
#include "gui/accelerators.h"
#include "common/curve_tools.h"

#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif

#include <gmodule.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

static gchar *_iop_section_for_params(dt_iop_module_t *self)
{
  if(!self->widget) self->widget = dt_gui_vbox();
  return self->actions == DT_ACTION_TYPE_IOP_SECTION && self->data ? self->data : NULL;
}

#define DT_IOP_PAGE_BINDING_KEY "dt-iop-page-binding"

typedef struct _param_range_t
{
  size_t offset;
  size_t size;
} _param_range_t;

typedef struct _page_binding_t
{
  dt_iop_module_t *module;
  GArray *ranges;     // _param_range_t
  GSList *containers; // GtkWidget
} _page_binding_t;

static void _page_binding_free(gpointer data)
{
  _page_binding_t *binding = data;
  g_array_free(binding->ranges, TRUE);
  g_slist_free(binding->containers);
  g_free(binding);
}

static _page_binding_t *_page_binding(GtkWidget *page)
{
  _page_binding_t *binding = g_object_get_data(G_OBJECT(page), DT_IOP_PAGE_BINDING_KEY);
  if(!binding)
  {
    binding = g_new0(_page_binding_t, 1);
    binding->ranges = g_array_new(FALSE, FALSE, sizeof(_param_range_t));
    g_object_set_data_full(G_OBJECT(page), DT_IOP_PAGE_BINDING_KEY,
                           binding, _page_binding_free);
  }
  return binding;
}

void dt_iop_page_bind_field(GtkWidget *page,
                            dt_iop_module_t *self,
                            const char *field,
                            const int row)
{
  const dt_introspection_field_t *f = self->get_f(field);
  if(!f)
  {
    dt_print(DT_DEBUG_ALWAYS, "[dt_iop_page_bind_field] %s has no field '%s'",
             self->op, field);
    return;
  }

  _param_range_t range = { f->header.offset, f->header.size };
  if(row >= 0)
  {
    if(f->header.type != DT_INTROSPECTION_TYPE_ARRAY || (size_t)row >= f->Array.count)
    {
      dt_print(DT_DEBUG_ALWAYS, "[dt_iop_page_bind_field] %s has no row %d in '%s'",
               self->op, row, field);
      return;
    }
    range.size = f->Array.field->header.size;
    range.offset += row * range.size;
  }

  _page_binding_t *binding = _page_binding(page);
  binding->module = self;
  g_array_append_val(binding->ranges, range);
}

void dt_iop_page_bind_widgets(GtkWidget *page,
                              GtkWidget *container)
{
  _page_binding_t *binding = _page_binding(page);
  binding->containers = g_slist_append(binding->containers, container);
}

/* Collect the parts of a module's params a notebook page governs: the fields
   of the parameter widgets on it, wherever they are nested, and what it was
   bound to. Sections and collapsible sections keep folded content in the
   widget tree, so it is included. With visible_only, widgets a module has
   hidden are left out; bound fields always count. */
static void _page_ranges(GtkWidget *w,
                         const gboolean visible_only,
                         dt_iop_module_t **module,
                         GArray *ranges)
{
  const _page_binding_t *binding = g_object_get_data(G_OBJECT(w), DT_IOP_PAGE_BINDING_KEY);
  if(binding)
  {
    if(binding->module)
    {
      *module = binding->module;
      g_array_append_vals(ranges, binding->ranges->data, binding->ranges->len);
    }
    for(const GSList *c = binding->containers; c; c = g_slist_next(c))
      _page_ranges(c->data, visible_only, module, ranges);
  }

  if(visible_only && !gtk_widget_get_visible(w)) return;

  if(DT_IS_BAUHAUS_WIDGET(w))
  {
    const size_t size = dt_bauhaus_widget_get_field_size(w);
    dt_iop_module_t *m = dt_bauhaus_widget_get_module(w);
    if(!size || !m || !m->params) return;

    const ptrdiff_t offset = (uint8_t *)dt_bauhaus_widget_get_field(w)
                             - (uint8_t *)m->params;
    if(offset < 0 || (size_t)offset + size > (size_t)MAX(m->params_size, 0)) return;

    *module = m;
    g_array_append_val(ranges, ((_param_range_t){ offset, size }));
  }
  else if(GTK_IS_CONTAINER(w))
  {
    GList *children = gtk_container_get_children(GTK_CONTAINER(w));
    for(GList *c = children; c; c = g_list_next(c))
      _page_ranges(c->data, visible_only, module, ranges);
    g_list_free(children);
  }
}

static void _reset_widgets(GtkWidget *w,
                           gpointer user_data)
{
  if(DT_IS_BAUHAUS_WIDGET(w))
    dt_bauhaus_widget_reset(w);
  else if(GTK_IS_CONTAINER(w))
    gtk_container_foreach(GTK_CONTAINER(w), _reset_widgets, NULL);
}

void dt_iop_page_reset(GtkWidget *page)
{
  dt_iop_module_t *module = NULL;
  GArray *ranges = g_array_new(FALSE, FALSE, sizeof(_param_range_t));
  _page_ranges(page, FALSE, &module, ranges);

  if(module && module->default_params)
  {
    for(guint r = 0; r < ranges->len; r++)
    {
      const _param_range_t *range = &g_array_index(ranges, _param_range_t, r);
      memcpy((uint8_t *)module->params + range->offset,
             (uint8_t *)module->default_params + range->offset,
             range->size);
    }

    dt_iop_gui_update(module);
    dt_dev_add_history_item(darktable.develop, module, TRUE);
  }
  else
  {
    // a page that is not part of a processing module resets its widgets
    _reset_widgets(page, NULL);
  }

  g_array_free(ranges, TRUE);
}

static void _highlight_changed_tab(GtkWidget *page,
                                   gpointer notebook)
{
  dt_iop_module_t *module = NULL;
  GArray *ranges = g_array_new(FALSE, FALSE, sizeof(_param_range_t));
  _page_ranges(page, TRUE, &module, ranges);

  gboolean is_changed = FALSE;
  for(guint r = 0; module && module->default_params && r < ranges->len && !is_changed; r++)
  {
    const _param_range_t *range = &g_array_index(ranges, _param_range_t, r);
    is_changed = memcmp((uint8_t *)module->params + range->offset,
                        (uint8_t *)module->default_params + range->offset,
                        range->size) != 0;
  }
  g_array_free(ranges, TRUE);

  GtkWidget *label = gtk_notebook_get_tab_label(GTK_NOTEBOOK(notebook), page);
  if(is_changed)
    dt_gui_add_class(label, "changed");
  else
    dt_gui_remove_class(label, "changed");
}

static void _refresh_notebooks(GtkWidget *w,
                               gpointer user_data)
{
  if(GTK_IS_NOTEBOOK(w))
    gtk_container_foreach(GTK_CONTAINER(w), _highlight_changed_tab, w);

  if(GTK_IS_CONTAINER(w))
    gtk_container_foreach(GTK_CONTAINER(w), _refresh_notebooks, NULL);
}

void dt_iop_gui_refresh_tabs(dt_iop_module_t *module)
{
  if(module && module->widget) _refresh_notebooks(module->widget, NULL);
}

GtkWidget *dt_bauhaus_slider_from_params(dt_iop_module_t *self, const char *param)
{
  gchar *section = _iop_section_for_params(self);

  dt_iop_params_t *p = self->params;
  dt_iop_params_t *d = self->default_params;

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

  const dt_introspection_field_t *f = self->get_f(param_name);

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

  dt_gui_box_add(self->widget, slider);

  g_free(param_name);

  return slider;
}

GtkWidget *dt_bauhaus_combobox_from_params(dt_iop_module_t *self, const char *param)
{
  gchar *section = _iop_section_for_params(self);

  dt_iop_params_t *p = self->params;
  dt_iop_params_t *d = self->default_params;
  dt_introspection_field_t *f = self->get_f(param);

  GtkWidget *combobox = dt_bauhaus_combobox_new(self);
  gchar *str = NULL;

  if(f && (f->header.type == DT_INTROSPECTION_TYPE_ENUM ||
            f->header.type == DT_INTROSPECTION_TYPE_INT  ||
            f->header.type == DT_INTROSPECTION_TYPE_UINT ||
            f->header.type == DT_INTROSPECTION_TYPE_BOOL ))
  {
    dt_bauhaus_widget_set_field(combobox, (uint8_t *)p + f->header.offset, f->header.type);

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
      dt_bauhaus_combobox_add_introspection(combobox,
                                            action,
                                            f->Enum.values,
                                            f->Enum.values[0].value,
                                            f->Enum.values[f->Enum.entries - 1].value);
      dt_bauhaus_combobox_set_default(combobox, *(int*)((uint8_t *)d + f->header.offset));
    }
  }
  else
  {
    str = g_strdup_printf("'%s' is not an enum/int/bool/combobox parameter", param);

    dt_bauhaus_widget_set_label(combobox, section, str);
  }

  g_free(str);

  dt_gui_box_add(self->widget, combobox);

  return combobox;
}

GtkWidget *dt_bauhaus_toggle_from_params(dt_iop_module_t *self, const char *param)
{
  gchar *section = _iop_section_for_params(self);

  dt_iop_params_t *p = self->params;
  dt_iop_params_t *d = self->default_params;
  dt_introspection_field_t *f = self->get_f(param);

  GtkWidget *toggle = dt_bauhaus_toggle_new(self);
  gchar *str = NULL;

  if(f && f->header.type == DT_INTROSPECTION_TYPE_BOOL)
  {
    // the field has to be attached before the label is set: setting the
    // label is what registers the widget as an action, and only widgets
    // that already carry a field are collected for the automatic
    // params-to-gui updates
    dt_bauhaus_widget_set_field(toggle, (uint8_t *)p + f->header.offset,
                                DT_INTROSPECTION_TYPE_BOOL);

    // we do not want to support a context as it break all translations see #5498
    str = *f->header.description
        ? g_strdup(f->header.description)
        : dt_util_str_replace(param, "_", " ");

    dt_bauhaus_widget_set_label(toggle, section, str);

    dt_bauhaus_toggle_set_default(toggle,
                                  *(gboolean *)((uint8_t *)d + f->header.offset));
  }
  else
  {
    str = g_strdup_printf("'%s' is not a bool/togglebutton parameter", param);

    dt_bauhaus_widget_set_label(toggle, section, str);
  }

  g_free(str);

  dt_gui_box_add(self->widget, toggle);

  return toggle;
}

/* Claim the event sequence in CAPTURE phase so the togglebutton's
 * internal GtkGestureMultiPress (GTK_PHASE_BUBBLE) does NOT process
 * the event.  This prevents GtkButton from emitting "clicked" and
 * toggling the button state behind our callback, which would conflict
 * with callbacks that implement radio-button behaviour by explicitly
 * managing all toggle states.
 *
 * GTK4 migration: the pattern is the same — just rename
 * GtkGestureMultiPress to GtkGestureClick. */
static void _gesture_begin_claim(GtkGesture *gesture,
                                  GdkEventSequence *sequence,
                                  gpointer user_data)
{
  gtk_gesture_set_sequence_state(gesture, sequence, GTK_EVENT_SEQUENCE_CLAIMED);
}

GtkWidget *dt_iop_togglebutton_new(dt_iop_module_t *self, const char *section, const gchar *label, const gchar *ctrl_label,
                                   GCallback callback, gboolean local, guint accel_key, GdkModifierType mods,
                                   DTGTKCairoPaintIconFunc paint, GtkWidget *box)
{
  GtkWidget *w = dtgtk_togglebutton_new(paint, 0, NULL);
  {
    GtkGesture *gesture = gtk_gesture_multi_press_new(w);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(gesture),
                                               GTK_PHASE_CAPTURE);
    dt_gui_add_controller(w, gesture);
    g_signal_connect_data(gesture, "pressed", callback, self, NULL, 0);
    g_signal_connect(gesture, "begin", G_CALLBACK(_gesture_begin_claim), NULL);
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(gesture), 0);
    /* shortcut activation routes through this gesture (DT_ACTION_GESTURE_KEY,
     * see _action_process_toggle in accelerators.c) */
    g_object_set_data(G_OBJECT(w), DT_ACTION_GESTURE_KEY, gesture);
  }

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
    button = dtgtk_button_new_full(paint, paintflags, NULL,
      &(dtgtk_button_config_t){
        .tooltip = Q_(label),
      });
  }
  else
  {
    button = gtk_button_new_with_label(Q_(label));
    gtk_label_set_ellipsize(GTK_LABEL(gtk_bin_get_child(GTK_BIN(button))), PANGO_ELLIPSIZE_END);
  }

  g_signal_connect_data(G_OBJECT(button), "clicked", callback, (gpointer)self, NULL, 0);

  dt_action_t *ac = dt_action_define_iop(self, NULL, label, button, &dt_action_def_button);
    dt_shortcut_register(ac, 0, 0, accel_key, mods);

  if(GTK_IS_BOX(box)) gtk_box_pack_start(GTK_BOX(box), button, TRUE, TRUE, 0);

  return button;
}

gboolean dt_mask_scroll_increases(int up)
{
  const gboolean mask_down = dt_conf_get_bool("masks_scroll_down_increases");
  return up ? !mask_down : mask_down;
}

GtkWidget *dt_bauhaus_combobox_new_interpolation(dt_iop_module_t *self)
{
  static const dt_introspection_type_enum_tuple_t interpolation_names[]
    = { { N_("cubic spline"), CUBIC_SPLINE },
        { N_("centripetal spline"), CATMULL_ROM },
        { N_("monotonic spline"), MONOTONE_HERMITE },
        { } };
  GtkWidget *w = dt_bauhaus_combobox_new(self);
  dt_action_t *ac = dt_bauhaus_widget_set_label(w, NULL, N_("interpolation method"));
  dt_bauhaus_combobox_add_introspection(w, ac, interpolation_names, 0, -1);

  return w;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
