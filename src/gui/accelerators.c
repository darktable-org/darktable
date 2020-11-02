/*
    This file is part of darktable,
    Copyright (C) 2011-2020 darktable developers.

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

#include "gui/accelerators.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/utility.h"
#include "control/control.h"
#include "develop/blend.h"

#include "bauhaus/bauhaus.h"

#include <assert.h>
#include <gtk/gtk.h>

typedef struct _accel_iop_t
{
  dt_accel_t *accel;
  GClosure *closure;
} _accel_iop_t;

void dt_accel_path_global(char *s, size_t n, const char *path)
{
  snprintf(s, n, "<Darktable>/%s/%s", "global", path);
}

void dt_accel_path_view(char *s, size_t n, char *module, const char *path)
{
  snprintf(s, n, "<Darktable>/%s/%s/%s", "views", module, path);
}

void dt_accel_path_iop(char *s, size_t n, char *module, const char *path)
{
  if(path)
  {

    gchar **split_paths = g_strsplit(path, "`", 4);
    // transitionally keep "preset" translated in keyboardrc to avoid breakage for now
    // this also needs to be amended in preferences
    if(!strcmp(split_paths[0], "preset"))
    {
      g_free(split_paths[0]);
      split_paths[0] = g_strdup(_("preset"));
    }
    for(gchar **cur_path = split_paths; *cur_path; cur_path++)
    {
      gchar *after_context = strchr(*cur_path,'|');
      if(after_context) memmove(*cur_path, after_context + 1, strlen(after_context));
    }
    gchar *joined_paths = g_strjoinv("/", split_paths);
    snprintf(s, n, "<Darktable>/%s/%s/%s", "image operations", module, joined_paths);
    g_free(joined_paths);
    g_strfreev(split_paths);
  }
  else
    snprintf(s, n, "<Darktable>/%s/%s", "image operations", module);
}

void dt_accel_path_lib(char *s, size_t n, char *module, const char *path)
{
  snprintf(s, n, "<Darktable>/%s/%s/%s", "modules", module, path);
}

void dt_accel_path_lua(char *s, size_t n, const char *path)
{
  snprintf(s, n, "<Darktable>/%s/%s", "lua", path);
}

void dt_accel_path_manual(char *s, size_t n, const char *full_path)
{
  snprintf(s, n, "<Darktable>/%s", full_path);
}

static void dt_accel_path_global_translated(char *s, size_t n, const char *path)
{
  snprintf(s, n, "<Darktable>/%s/%s", C_("accel", "global"), g_dpgettext2(NULL, "accel", path));
}

static void dt_accel_path_view_translated(char *s, size_t n, dt_view_t *module, const char *path)
{
  snprintf(s, n, "<Darktable>/%s/%s/%s", C_("accel", "views"), module->name(module),
           g_dpgettext2(NULL, "accel", path));
}

static void dt_accel_path_iop_translated(char *s, size_t n, dt_iop_module_so_t *module, const char *path)
{
  gchar *module_clean = g_strdelimit(g_strdup(module->name()), "/", '-');

  if(path)
  {
    gchar **split_paths = g_strsplit(path, "`", 4);
    for(gchar **cur_path = split_paths; *cur_path; cur_path++)
    {
      gchar *saved_path = *cur_path;
      *cur_path = g_strdelimit(g_strconcat(Q_(*cur_path), (strcmp(*cur_path, "preset") ? NULL : " "), NULL), "/", '`');
      g_free(saved_path);
    }
    gchar *joined_paths = g_strjoinv("/", split_paths);
    snprintf(s, n, "<Darktable>/%s/%s/%s", C_("accel", "processing modules"), module_clean, joined_paths);
    g_free(joined_paths);
    g_strfreev(split_paths);
  }
  else
    snprintf(s, n, "<Darktable>/%s/%s", C_("accel", "processing modules"), module_clean);

  g_free(module_clean);
}

static void dt_accel_path_lib_translated(char *s, size_t n, dt_lib_module_t *module, const char *path)
{
  snprintf(s, n, "<Darktable>/%s/%s/%s", C_("accel", "utility modules"), module->name(module),
           g_dpgettext2(NULL, "accel", path));
}

static void dt_accel_path_lua_translated(char *s, size_t n, const char *path)
{
  snprintf(s, n, "<Darktable>/%s/%s", C_("accel", "lua"), g_dpgettext2(NULL, "accel", path));
}

static void dt_accel_path_manual_translated(char *s, size_t n, const char *full_path)
{
  snprintf(s, n, "<Darktable>/%s", g_dpgettext2(NULL, "accel", full_path));
}

void dt_accel_register_global(const gchar *path, guint accel_key, GdkModifierType mods)
{
  gchar accel_path[256];
  dt_accel_t *accel = (dt_accel_t *)g_malloc0(sizeof(dt_accel_t));

  dt_accel_path_global(accel_path, sizeof(accel_path), path);
  gtk_accel_map_add_entry(accel_path, accel_key, mods);

  g_strlcpy(accel->path, accel_path, sizeof(accel->path));
  dt_accel_path_global_translated(accel_path, sizeof(accel_path), path);
  g_strlcpy(accel->translated_path, accel_path, sizeof(accel->translated_path));

  *(accel->module) = '\0';
  accel->local = FALSE;
  accel->views = DT_VIEW_DARKROOM | DT_VIEW_LIGHTTABLE | DT_VIEW_TETHERING | DT_VIEW_MAP | DT_VIEW_PRINT | DT_VIEW_SLIDESHOW;
  darktable.control->accelerator_list = g_slist_prepend(darktable.control->accelerator_list, accel);
}

void dt_accel_register_view(dt_view_t *self, const gchar *path, guint accel_key, GdkModifierType mods)
{
  gchar accel_path[256];
  dt_accel_t *accel = (dt_accel_t *)g_malloc0(sizeof(dt_accel_t));

  dt_accel_path_view(accel_path, sizeof(accel_path), self->module_name, path);
  gtk_accel_map_add_entry(accel_path, accel_key, mods);

  g_strlcpy(accel->path, accel_path, sizeof(accel->path));
  dt_accel_path_view_translated(accel_path, sizeof(accel_path), self, path);
  g_strlcpy(accel->translated_path, accel_path, sizeof(accel->translated_path));

  g_strlcpy(accel->module, self->module_name, sizeof(accel->module));
  accel->local = FALSE;
  accel->views = self->view(self);
  darktable.control->accelerator_list = g_slist_prepend(darktable.control->accelerator_list, accel);
}

void dt_accel_register_iop(dt_iop_module_so_t *so, gboolean local, const gchar *path, guint accel_key,
                           GdkModifierType mods)
{
  dt_accel_t *accel = (dt_accel_t *)g_malloc0(sizeof(dt_accel_t));

  dt_accel_path_iop(accel->path, sizeof(accel->path), so->op, path);
  gtk_accel_map_add_entry(accel->path, accel_key, mods);
  dt_accel_path_iop_translated(accel->translated_path, sizeof(accel->translated_path), so, path);

  g_strlcpy(accel->module, so->op, sizeof(accel->module));
  accel->local = local;
  accel->views = DT_VIEW_DARKROOM;
  darktable.control->accelerator_list = g_slist_prepend(darktable.control->accelerator_list, accel);
}

void dt_accel_register_lib_as_view(gchar *view_name, const gchar *path, guint accel_key, GdkModifierType mods)
{
  //register a lib shortcut but place it in the path of a view
  gchar accel_path[256];
  dt_accel_path_view(accel_path, sizeof(accel_path), view_name, path);
  if (dt_accel_find_by_path(accel_path)) return; // return if nothing to add, to avoid multiple entries

  dt_accel_t *accel = (dt_accel_t *)g_malloc0(sizeof(dt_accel_t));
  gtk_accel_map_add_entry(accel_path, accel_key, mods);
  g_strlcpy(accel->path, accel_path, sizeof(accel->path));

  snprintf(accel_path, sizeof(accel_path), "<Darktable>/%s/%s/%s", C_("accel", "views"),
           g_dgettext(NULL, view_name),
           g_dpgettext2(NULL, "accel", path));

  g_strlcpy(accel->translated_path, accel_path, sizeof(accel->translated_path));

  g_strlcpy(accel->module, view_name, sizeof(accel->module));
  accel->local = FALSE;

  if(strcmp(view_name, "lighttable") == 0)
    accel->views = DT_VIEW_LIGHTTABLE;
  else if(strcmp(view_name, "darkroom") == 0)
    accel->views = DT_VIEW_DARKROOM;
  else if(strcmp(view_name, "print") == 0)
    accel->views = DT_VIEW_PRINT;
  else if(strcmp(view_name, "slideshow") == 0)
    accel->views = DT_VIEW_SLIDESHOW;
  else if(strcmp(view_name, "map") == 0)
    accel->views = DT_VIEW_MAP;
  else if(strcmp(view_name, "tethering") == 0)
    accel->views = DT_VIEW_TETHERING;

  darktable.control->accelerator_list = g_slist_prepend(darktable.control->accelerator_list, accel);
}

void dt_accel_register_lib_for_views(dt_lib_module_t *self, dt_view_type_flags_t views, const gchar *path,
                                     guint accel_key, GdkModifierType mods)
{
  gchar accel_path[256];
  dt_accel_path_lib(accel_path, sizeof(accel_path), self->plugin_name, path);
  if (dt_accel_find_by_path(accel_path)) return; // return if nothing to add, to avoid multiple entries

  dt_accel_t *accel = (dt_accel_t *)g_malloc0(sizeof(dt_accel_t));

  gtk_accel_map_add_entry(accel_path, accel_key, mods);
  g_strlcpy(accel->path, accel_path, sizeof(accel->path));
  dt_accel_path_lib_translated(accel_path, sizeof(accel_path), self, path);
  g_strlcpy(accel->translated_path, accel_path, sizeof(accel->translated_path));

  g_strlcpy(accel->module, self->plugin_name, sizeof(accel->module));
  accel->local = FALSE;
  // we get the views in which the lib will be displayed
  accel->views = views;
  darktable.control->accelerator_list = g_slist_prepend(darktable.control->accelerator_list, accel);
}

void dt_accel_register_lib(dt_lib_module_t *self, const gchar *path, guint accel_key, GdkModifierType mods)
{
  dt_view_type_flags_t v = 0;
  int i=0;
  const gchar **views = self->views(self);
  while (views[i])
  {
    if(strcmp(views[i], "lighttable") == 0)
      v |= DT_VIEW_LIGHTTABLE;
    else if(strcmp(views[i], "darkroom") == 0)
      v |= DT_VIEW_DARKROOM;
    else if(strcmp(views[i], "print") == 0)
      v |= DT_VIEW_PRINT;
    else if(strcmp(views[i], "slideshow") == 0)
      v |= DT_VIEW_SLIDESHOW;
    else if(strcmp(views[i], "map") == 0)
      v |= DT_VIEW_MAP;
    else if(strcmp(views[i], "tethering") == 0)
      v |= DT_VIEW_TETHERING;
    else if(strcmp(views[i], "*") == 0)
      v |= DT_VIEW_DARKROOM | DT_VIEW_LIGHTTABLE | DT_VIEW_TETHERING | DT_VIEW_MAP | DT_VIEW_PRINT
           | DT_VIEW_SLIDESHOW;
    i++;
  }
  dt_accel_register_lib_for_views(self, v, path, accel_key, mods);
}

const gchar *_common_actions[]
  = { NC_("accel", "show module"),
      NC_("accel", "enable module"),
      NC_("accel", "focus module"),
      NC_("accel", "reset module parameters"),
      NC_("accel", "show preset menu"),
      NULL };

const gchar *_slider_actions[]
  = { NC_("accel", "increase"),
      NC_("accel", "decrease"),
      NC_("accel", "reset"),
      NC_("accel", "edit"),
      NC_("accel", "dynamic"),
      NULL };

const gchar *_combobox_actions[]
  = { NC_("accel", "next"),
      NC_("accel", "previous"),
      NC_("accel", "dynamic"),
      NULL };

void _accel_register_actions_iop(dt_iop_module_so_t *so, gboolean local, const gchar *path, const char **actions)
{
  gchar accel_path[256];
  gchar accel_path_trans[256];
  dt_accel_path_iop(accel_path, sizeof(accel_path), so->op, path);
  dt_accel_path_iop_translated(accel_path_trans, sizeof(accel_path_trans), so, path);

  for(const char **action = actions; *action; action++)
  {
    dt_accel_t *accel = (dt_accel_t *)g_malloc0(sizeof(dt_accel_t));
    snprintf(accel->path, sizeof(accel->path), "%s/%s", accel_path, *action);
    gtk_accel_map_add_entry(accel->path, 0, 0);
    snprintf(accel->translated_path, sizeof(accel->translated_path), "%s/%s ", accel_path_trans,
             g_dpgettext2(NULL, "accel", *action));
    g_strlcpy(accel->module, so->op, sizeof(accel->module));
    accel->local = local;
    accel->views = DT_VIEW_DARKROOM;

    darktable.control->accelerator_list = g_slist_prepend(darktable.control->accelerator_list, accel);
  }
}

void dt_accel_register_common_iop(dt_iop_module_so_t *so)
{
  _accel_register_actions_iop(so, FALSE, NULL, _common_actions);
}

void dt_accel_register_combobox_iop(dt_iop_module_so_t *so, gboolean local, const gchar *path)
{
  _accel_register_actions_iop(so, local, path, _combobox_actions);
}

void dt_accel_register_slider_iop(dt_iop_module_so_t *so, gboolean local, const gchar *path)
{
  _accel_register_actions_iop(so, local, path, _slider_actions);
}

void dt_accel_register_lua(const gchar *path, guint accel_key, GdkModifierType mods)
{
  gchar accel_path[256];
  dt_accel_t *accel = (dt_accel_t *)g_malloc0(sizeof(dt_accel_t));

  dt_accel_path_lua(accel_path, sizeof(accel_path), path);
  gtk_accel_map_add_entry(accel_path, accel_key, mods);

  g_strlcpy(accel->path, accel_path, sizeof(accel->path));
  dt_accel_path_lua_translated(accel_path, sizeof(accel_path), path);
  g_strlcpy(accel->translated_path, accel_path, sizeof(accel->translated_path));

  *(accel->module) = '\0';
  accel->local = FALSE;
  accel->views = DT_VIEW_DARKROOM | DT_VIEW_LIGHTTABLE | DT_VIEW_TETHERING | DT_VIEW_MAP | DT_VIEW_PRINT | DT_VIEW_SLIDESHOW;
  darktable.control->accelerator_list = g_slist_prepend(darktable.control->accelerator_list, accel);
}

void dt_accel_register_manual(const gchar *full_path, dt_view_type_flags_t views, guint accel_key,
                              GdkModifierType mods)
{
  gchar accel_path[256];
  dt_accel_t *accel = (dt_accel_t *)g_malloc0(sizeof(dt_accel_t));

  dt_accel_path_manual(accel_path, sizeof(accel_path), full_path);
  gtk_accel_map_add_entry(accel_path, accel_key, mods);

  g_strlcpy(accel->path, accel_path, sizeof(accel->path));
  dt_accel_path_manual_translated(accel_path, sizeof(accel_path), full_path);
  g_strlcpy(accel->translated_path, accel_path, sizeof(accel->translated_path));

  *(accel->module) = '\0';
  accel->local = FALSE;
  accel->views = views;
  darktable.control->accelerator_list = g_slist_prepend(darktable.control->accelerator_list, accel);
}

static dt_accel_t *_lookup_accel(const gchar *path)
{
  GSList *l = darktable.control->accelerator_list;
  while(l)
  {
    dt_accel_t *accel = (dt_accel_t *)l->data;
    if(accel && !strcmp(accel->path, path)) return accel;
    l = g_slist_next(l);
  }
  return NULL;
}

void dt_accel_connect_global(const gchar *path, GClosure *closure)
{
  gchar accel_path[256];
  dt_accel_path_global(accel_path, sizeof(accel_path), path);
  dt_accel_t *laccel = _lookup_accel(accel_path);
  laccel->closure = closure;
  gtk_accel_group_connect_by_path(darktable.control->accelerators, accel_path, closure);
}

void dt_accel_connect_view(dt_view_t *self, const gchar *path, GClosure *closure)
{
  gchar accel_path[256];
  dt_accel_path_view(accel_path, sizeof(accel_path), self->module_name, path);
  gtk_accel_group_connect_by_path(darktable.control->accelerators, accel_path, closure);
  dt_accel_t *laccel = _lookup_accel(accel_path);
  laccel->closure = closure;

  self->accel_closures = g_slist_prepend(self->accel_closures, laccel);
}

dt_accel_t *dt_accel_connect_lib_as_view(dt_lib_module_t *module, gchar *view_name, const gchar *path, GClosure *closure)
{
  gchar accel_path[256];
  dt_accel_path_view(accel_path, sizeof(accel_path), view_name, path);
  gtk_accel_group_connect_by_path(darktable.control->accelerators, accel_path, closure);

  dt_accel_t *accel = _lookup_accel(accel_path);
  if(!accel) return NULL; // this happens when the path doesn't match any accel (typos, ...)

  accel->closure = closure;

  module->accel_closures = g_slist_prepend(module->accel_closures, accel);
  return accel;
}

dt_accel_t *dt_accel_connect_lib_as_global(dt_lib_module_t *module, const gchar *path, GClosure *closure)
{
  gchar accel_path[256];
  dt_accel_path_global(accel_path, sizeof(accel_path), path);

  dt_accel_t *accel = _lookup_accel(accel_path);
  if(!accel) return NULL; // this happens when the path doesn't match any accel (typos, ...)

  gtk_accel_group_connect_by_path(darktable.control->accelerators, accel_path, closure);

  accel->closure = closure;

  module->accel_closures = g_slist_prepend(module->accel_closures, accel);
  return accel;
}

static dt_accel_t *_store_iop_accel_closure(dt_iop_module_t *module, gchar *accel_path, GClosure *closure)
{
  // Looking up the entry in the global accelerators list
  dt_accel_t *accel = _lookup_accel(accel_path);
  if(!accel) return NULL; // this happens when the path doesn't match any accel (typos, ...)

  GSList **save_list = accel->local ? &module->accel_closures_local : &module->accel_closures;

  _accel_iop_t *stored_accel = g_malloc(sizeof(_accel_iop_t));
  stored_accel->accel = accel;
  stored_accel->closure = closure;

  g_closure_ref(closure);
  g_closure_sink(closure);
  *save_list = g_slist_prepend(*save_list, stored_accel);

  return accel;
}

dt_accel_t *dt_accel_connect_iop(dt_iop_module_t *module, const gchar *path, GClosure *closure)
{
  gchar accel_path[256];
  dt_accel_path_iop(accel_path, sizeof(accel_path), module->op, path);

  return _store_iop_accel_closure(module, accel_path, closure);
}

dt_accel_t *dt_accel_connect_lib(dt_lib_module_t *module, const gchar *path, GClosure *closure)
{
  gchar accel_path[256];
  dt_accel_path_lib(accel_path, sizeof(accel_path), module->plugin_name, path);
  gtk_accel_group_connect_by_path(darktable.control->accelerators, accel_path, closure);

  dt_accel_t *accel = _lookup_accel(accel_path);
  if(!accel) return NULL; // this happens when the path doesn't match any accel (typos, ...)

  accel->closure = closure;

  module->accel_closures = g_slist_prepend(module->accel_closures, accel);
  return accel;
}

void dt_accel_connect_lua(const gchar *path, GClosure *closure)
{
  gchar accel_path[256];
  dt_accel_path_lua(accel_path, sizeof(accel_path), path);
  dt_accel_t *laccel = _lookup_accel(accel_path);
  laccel->closure = closure;
  gtk_accel_group_connect_by_path(darktable.control->accelerators, accel_path, closure);
}

void dt_accel_connect_manual(GSList **list_ptr, const gchar *full_path, GClosure *closure)
{
  gchar accel_path[256];
  dt_accel_path_manual(accel_path, sizeof(accel_path), full_path);
  dt_accel_t *accel = _lookup_accel(accel_path);
  accel->closure = closure;
  gtk_accel_group_connect_by_path(darktable.control->accelerators, accel_path, closure);
  *list_ptr = g_slist_prepend(*list_ptr, accel);
}

static gboolean _press_button_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                       GdkModifierType modifier, gpointer data)
{
  if(!(GTK_IS_BUTTON(data))) return FALSE;

  gtk_button_clicked(GTK_BUTTON(data));
  return TRUE;
}

static gboolean _tooltip_callback(GtkWidget *widget, gint x, gint y, gboolean keyboard_mode,
                                  GtkTooltip *tooltip, gpointer user_data)
{
  char *text = gtk_widget_get_tooltip_text(widget);

  GtkAccelKey key;
  dt_accel_t *accel = g_object_get_data(G_OBJECT(widget), "dt-accel");
  if(accel && gtk_accel_map_lookup_entry(accel->path, &key))
  {
    gchar *key_name = gtk_accelerator_get_label(key.accel_key, key.accel_mods);
    if(key_name && *key_name)
    {
      char *tmp = g_strdup_printf("%s (%s)", text, key_name);
      g_free(text);
      text = tmp;
    }
    g_free(key_name);
  }

  gtk_tooltip_set_text(tooltip, text);
  g_free(text);
  return TRUE;
}

void dt_accel_connect_button_iop(dt_iop_module_t *module, const gchar *path, GtkWidget *button)
{
  GClosure *closure = g_cclosure_new(G_CALLBACK(_press_button_callback), button, NULL);
  dt_accel_t *accel = dt_accel_connect_iop(module, path, closure);
  g_object_set_data(G_OBJECT(button), "dt-accel", accel);

  if(gtk_widget_get_has_tooltip(button))
    g_signal_connect(G_OBJECT(button), "query-tooltip", G_CALLBACK(_tooltip_callback), NULL);
}

void dt_accel_connect_button_lib(dt_lib_module_t *module, const gchar *path, GtkWidget *button)
{
  GClosure *closure = g_cclosure_new(G_CALLBACK(_press_button_callback), button, NULL);
  dt_accel_t *accel = dt_accel_connect_lib(module, path, closure);
  g_object_set_data(G_OBJECT(button), "dt-accel", accel);

  if(gtk_widget_get_has_tooltip(button))
    g_signal_connect(G_OBJECT(button), "query-tooltip", G_CALLBACK(_tooltip_callback), NULL);
}

void dt_accel_connect_button_lib_as_global(dt_lib_module_t *module, const gchar *path, GtkWidget *button)
{
  GClosure *closure = g_cclosure_new(G_CALLBACK(_press_button_callback), button, NULL);
  dt_accel_t *accel = dt_accel_connect_lib_as_global(module, path, closure);
  g_object_set_data(G_OBJECT(button), "dt-accel", accel);

  if(gtk_widget_get_has_tooltip(button))
    g_signal_connect(G_OBJECT(button), "query-tooltip", G_CALLBACK(_tooltip_callback), NULL);
}

static gboolean bauhaus_slider_edit_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                             GdkModifierType modifier, gpointer data)
{
  GtkWidget *slider = GTK_WIDGET(data);

  // FIXME: crashes when slider hidden
  dt_bauhaus_show_popup(DT_BAUHAUS_WIDGET(slider));

  return TRUE;
}

void dt_accel_widget_toast(GtkWidget *widget)
{
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)DT_BAUHAUS_WIDGET(widget);

  if(!darktable.gui->reset)
  {
    char *text = NULL;

    switch(w->type){
      case DT_BAUHAUS_SLIDER:
      {
        text = dt_bauhaus_slider_get_text(widget);
        break;
      }
      case DT_BAUHAUS_COMBOBOX:
        text = g_strdup(dt_bauhaus_combobox_get_text(widget));
        break;
      default: //literally impossible but hey
        return;
        break;
    }

    if(w->label[0] != '\0')
    { // label is not empty
      if(w->module && w->module->multi_name[0] != '\0')
        dt_toast_log(_("%s %s / %s: %s"), w->module->name(), w->module->multi_name, w->label, text);
      else if(w->module && !strstr(w->module->name(), w->label))
        dt_toast_log(_("%s / %s: %s"), w->module->name(), w->label, text);
      else
        dt_toast_log(_("%s: %s"), w->label, text);
    }
    else
    { //label is empty
      if(w->module && w->module->multi_name[0] != '\0')
        dt_toast_log(_("%s %s / %s"), w->module->name(), w->module->multi_name, text);
      else if(w->module)
        dt_toast_log(_("%s / %s"), w->module->name(), text);
      else
        dt_toast_log("%s", text);
    }

    g_free(text);
  }

}

float dt_accel_get_slider_scale_multiplier()
{
  const int slider_precision = dt_conf_get_int("accel/slider_precision");

  if(slider_precision == DT_IOP_PRECISION_COARSE)
  {
    return dt_conf_get_float("darkroom/ui/scale_rough_step_multiplier");
  }
  else if(slider_precision == DT_IOP_PRECISION_FINE)
  {
    return dt_conf_get_float("darkroom/ui/scale_precise_step_multiplier");
  }

  return dt_conf_get_float("darkroom/ui/scale_step_multiplier");
}

static gboolean bauhaus_slider_increase_callback(GtkAccelGroup *accel_group, GObject *acceleratable,
                                                 guint keyval, GdkModifierType modifier, gpointer data)
{
  GtkWidget *slider = GTK_WIDGET(data);

  float value = dt_bauhaus_slider_get(slider);
  float step = dt_bauhaus_slider_get_step(slider);
  float multiplier = dt_accel_get_slider_scale_multiplier();

  const float min_visible = powf(10.0f, -dt_bauhaus_slider_get_digits(slider));
  if(fabsf(step*multiplier) < min_visible)
    multiplier = min_visible / fabsf(step);

  dt_bauhaus_slider_set(slider, value + step * multiplier);

  g_signal_emit_by_name(G_OBJECT(slider), "value-changed");

  dt_accel_widget_toast(slider);
  return TRUE;
}

static gboolean bauhaus_slider_decrease_callback(GtkAccelGroup *accel_group, GObject *acceleratable,
                                                 guint keyval, GdkModifierType modifier, gpointer data)
{
  GtkWidget *slider = GTK_WIDGET(data);

  float value = dt_bauhaus_slider_get(slider);
  float step = dt_bauhaus_slider_get_step(slider);
  float multiplier = dt_accel_get_slider_scale_multiplier();

  const float min_visible = powf(10.0f, -dt_bauhaus_slider_get_digits(slider));
  if(fabsf(step*multiplier) < min_visible)
    multiplier = min_visible / fabsf(step);

  dt_bauhaus_slider_set(slider, value - step * multiplier);

  g_signal_emit_by_name(G_OBJECT(slider), "value-changed");

  dt_accel_widget_toast(slider);
  return TRUE;
}

static gboolean bauhaus_slider_reset_callback(GtkAccelGroup *accel_group, GObject *acceleratable,
                                              guint keyval, GdkModifierType modifier, gpointer data)
{
  GtkWidget *slider = GTK_WIDGET(data);

  dt_bauhaus_slider_reset(slider);

  g_signal_emit_by_name(G_OBJECT(slider), "value-changed");

  dt_accel_widget_toast(slider);
  return TRUE;
}

static gboolean bauhaus_dynamic_callback(GtkAccelGroup *accel_group, GObject *acceleratable,
                                         guint keyval, GdkModifierType modifier, gpointer data)
{
  if(DT_IS_BAUHAUS_WIDGET(data))
  {
    dt_bauhaus_widget_t *widget = DT_BAUHAUS_WIDGET(data);

    darktable.view_manager->current_view->dynamic_accel_current = GTK_WIDGET(widget);

    gchar *txt = g_strdup_printf (_("scroll to change <b>%s</b> of module %s %s"),
                                  dt_bauhaus_widget_get_label(GTK_WIDGET(widget)),
                                  widget->module->name(), widget->module->multi_name);
    dt_control_hinter_message(darktable.control, txt);
    g_free(txt);
  }
  else
    dt_control_hinter_message(darktable.control, "");

  return TRUE;
}

static gboolean bauhaus_combobox_next_callback(GtkAccelGroup *accel_group, GObject *acceleratable,
                                                 guint keyval, GdkModifierType modifier, gpointer data)
{
  GtkWidget *combobox = GTK_WIDGET(data);

  const int currentval = dt_bauhaus_combobox_get(combobox);
  const int nextval = currentval + 1 >= dt_bauhaus_combobox_length(combobox) ? 0 : currentval + 1;
  dt_bauhaus_combobox_set(combobox, nextval);

  dt_accel_widget_toast(combobox);

  return TRUE;
}

static gboolean bauhaus_combobox_prev_callback(GtkAccelGroup *accel_group, GObject *acceleratable,
                                                 guint keyval, GdkModifierType modifier, gpointer data)
{
  GtkWidget *combobox = GTK_WIDGET(data);

  const int currentval = dt_bauhaus_combobox_get(combobox);
  const int prevval = currentval - 1 < 0 ? dt_bauhaus_combobox_length(combobox) : currentval - 1;
  dt_bauhaus_combobox_set(combobox, prevval);

  dt_accel_widget_toast(combobox);

  return TRUE;
}

void _accel_connect_actions_iop(dt_iop_module_t *module, const gchar *path,
                               GtkWidget *w, const gchar *actions[], void *callbacks[])
{
  gchar accel_path[256];
  dt_accel_path_iop(accel_path, sizeof(accel_path) - 1, module->op, path);
  size_t path_len = strlen(accel_path);
  accel_path[path_len++] = '/';

  for(const char **action = actions; *action; action++, callbacks++)
  {
    strncpy(accel_path + path_len, *action, sizeof(accel_path) - path_len);

    GClosure *closure = g_cclosure_new(G_CALLBACK(*callbacks), (gpointer)w, NULL);

    _store_iop_accel_closure(module, accel_path, closure);
  }
}

void dt_accel_connect_combobox_iop(dt_iop_module_t *module, const gchar *path, GtkWidget *combobox)
{
  assert(DT_IS_BAUHAUS_WIDGET(combobox));

  void *combobox_callbacks[]
    = { bauhaus_combobox_next_callback,
        bauhaus_combobox_prev_callback,
        bauhaus_dynamic_callback };

  _accel_connect_actions_iop(module, path, combobox, _combobox_actions, combobox_callbacks);
}

void dt_accel_connect_slider_iop(dt_iop_module_t *module, const gchar *path, GtkWidget *slider)
{
  assert(DT_IS_BAUHAUS_WIDGET(slider));

  void *slider_callbacks[]
    = { bauhaus_slider_increase_callback,
        bauhaus_slider_decrease_callback,
        bauhaus_slider_reset_callback,
        bauhaus_slider_edit_callback,
        bauhaus_dynamic_callback };

  _accel_connect_actions_iop(module, path, slider, _slider_actions, slider_callbacks);
}

void dt_accel_connect_instance_iop(dt_iop_module_t *module)
{
  for(GSList *l = module->accel_closures; l; l = g_slist_next(l))
  {
    _accel_iop_t *stored_accel = (_accel_iop_t *)l->data;
    if(stored_accel && stored_accel->accel && stored_accel->closure)
    {

      if(stored_accel->accel->closure)
        gtk_accel_group_disconnect(darktable.control->accelerators, stored_accel->accel->closure);

      stored_accel->accel->closure = stored_accel->closure;

      gtk_accel_group_connect_by_path(darktable.control->accelerators,
                                      stored_accel->accel->path, stored_accel->closure);
    }
  }
}

void dt_accel_connect_locals_iop(dt_iop_module_t *module)
{
  GSList *l = module->accel_closures_local;
  while(l)
  {
    _accel_iop_t *accel = (_accel_iop_t *)l->data;
    if(accel)
    {
      gtk_accel_group_connect_by_path(darktable.control->accelerators, accel->accel->path, accel->closure);
    }
    l = g_slist_next(l);
  }

  module->local_closures_connected = TRUE;
}

void dt_accel_disconnect_list(GSList **list_ptr)
{
  GSList *list = *list_ptr;
  while(list)
  {
    dt_accel_t *accel = (dt_accel_t *)list->data;
    if(accel) gtk_accel_group_disconnect(darktable.control->accelerators, accel->closure);
    list = g_slist_delete_link(list, list);
  }
  *list_ptr = NULL;
}

void dt_accel_disconnect_locals_iop(dt_iop_module_t *module)
{
  if(!module->local_closures_connected) return;

  GSList *l = module->accel_closures_local;
  while(l)
  {
    _accel_iop_t *accel = (_accel_iop_t *)l->data;
    if(accel)
    {
      gtk_accel_group_disconnect(darktable.control->accelerators, accel->closure);
    }
    l = g_slist_next(l);
  }

  module->local_closures_connected = FALSE;
}

void _free_iop_accel(gpointer data)
{
  _accel_iop_t *accel = (_accel_iop_t *) data;

  if(accel->accel->closure == accel->closure)
  {
    gtk_accel_group_disconnect(darktable.control->accelerators, accel->closure);
    accel->accel->closure = NULL;
  }

  if(accel->closure->ref_count != 1)
    fprintf(stderr, "iop accel refcount %d %s\n", accel->closure->ref_count, accel->accel->path);

  g_closure_unref(accel->closure);

  g_free(accel);
}

void dt_accel_cleanup_closures_iop(dt_iop_module_t *module)
{
  dt_accel_disconnect_locals_iop(module);

  g_slist_free_full(module->accel_closures, _free_iop_accel);
  g_slist_free_full(module->accel_closures_local, _free_iop_accel);
  module->accel_closures = NULL;
  module->accel_closures_local = NULL;
}

typedef struct
{
  dt_iop_module_t *module;
  char *name;
} preset_iop_module_callback_description;

static void preset_iop_module_callback_destroyer(gpointer data, GClosure *closure)
{
  preset_iop_module_callback_description *callback_description
      = (preset_iop_module_callback_description *)data;
  g_free(callback_description->name);
  g_free(data);
}

static gboolean preset_iop_module_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                           GdkModifierType modifier, gpointer data)
{
  preset_iop_module_callback_description *callback_description
      = (preset_iop_module_callback_description *)data;
  dt_iop_module_t *module = callback_description->module;
  const char *name = callback_description->name;

  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT op_params, enabled, blendop_params, "
                                                             "blendop_version FROM data.presets "
                                                             "WHERE operation = ?1 AND name = ?2",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, module->op, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, name, -1, SQLITE_TRANSIENT);

  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const void *op_params = sqlite3_column_blob(stmt, 0);
    int op_length = sqlite3_column_bytes(stmt, 0);
    int enabled = sqlite3_column_int(stmt, 1);
    const void *blendop_params = sqlite3_column_blob(stmt, 2);
    int bl_length = sqlite3_column_bytes(stmt, 2);
    int blendop_version = sqlite3_column_int(stmt, 3);
    if(op_params && (op_length == module->params_size))
    {
      memcpy(module->params, op_params, op_length);
      module->enabled = enabled;
    }
    if(blendop_params && (blendop_version == dt_develop_blend_version())
       && (bl_length == sizeof(dt_develop_blend_params_t)))
    {
      memcpy(module->blend_params, blendop_params, sizeof(dt_develop_blend_params_t));
    }
    else if(blendop_params
            && dt_develop_blend_legacy_params(module, blendop_params, blendop_version, module->blend_params,
                                              dt_develop_blend_version(), bl_length) == 0)
    {
      // do nothing
    }
    else
    {
      memcpy(module->blend_params, module->default_blendop_params, sizeof(dt_develop_blend_params_t));
    }
  }
  sqlite3_finalize(stmt);
  dt_iop_gui_update(module);
  dt_dev_add_history_item(darktable.develop, module, FALSE);
  gtk_widget_queue_draw(module->widget);
  return TRUE;
}

void dt_accel_connect_preset_iop(dt_iop_module_t *module, const gchar *path)
{
  char build_path[1024];
  gchar *name = g_strdup(path);
  snprintf(build_path, sizeof(build_path), "%s`%s", N_("preset"), name);
  preset_iop_module_callback_description *callback_description
      = g_malloc(sizeof(preset_iop_module_callback_description));
  callback_description->module = module;
  callback_description->name = name;

  GClosure *closure = g_cclosure_new(G_CALLBACK(preset_iop_module_callback), callback_description,
                                     preset_iop_module_callback_destroyer);
  dt_accel_connect_iop(module, build_path, closure);
}



typedef struct
{
  dt_lib_module_t *module;
  char *name;
} preset_lib_module_callback_description;

static void preset_lib_module_callback_destroyer(gpointer data, GClosure *closure)
{
  preset_lib_module_callback_description *callback_description
      = (preset_lib_module_callback_description *)data;
  g_free(callback_description->name);
  g_free(data);
}
static gboolean preset_lib_module_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                           GdkModifierType modifier, gpointer data)

{
  preset_lib_module_callback_description *callback_description
      = (preset_lib_module_callback_description *)data;
  dt_lib_module_t *module = callback_description->module;
  const char *pn = callback_description->name;

  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get(darktable.db),
      "SELECT op_params FROM data.presets WHERE operation = ?1 AND op_version = ?2 AND name = ?3", -1, &stmt,
      NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, module->plugin_name, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, module->version());
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, pn, -1, SQLITE_TRANSIENT);

  int res = 0;
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const void *blob = sqlite3_column_blob(stmt, 0);
    int length = sqlite3_column_bytes(stmt, 0);
    if(blob)
    {
      GList *it = darktable.lib->plugins;
      while(it)
      {
        dt_lib_module_t *search_module = (dt_lib_module_t *)it->data;
        if(!strncmp(search_module->plugin_name, module->plugin_name, 128))
        {
          res = module->set_params(module, blob, length);
          break;
        }
        it = g_list_next(it);
      }
    }
  }
  sqlite3_finalize(stmt);
  if(res)
  {
    dt_control_log(_("deleting preset for obsolete module"));
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "DELETE FROM data.presets WHERE operation = ?1 AND op_version = ?2 AND name = ?3",
                                -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, module->plugin_name, -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, module->version());
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, pn, -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }
  return TRUE;
}

void dt_accel_connect_preset_lib(dt_lib_module_t *module, const gchar *path)
{
  char build_path[1024];
  gchar *name = g_strdup(path);
  snprintf(build_path, sizeof(build_path), "%s/%s", _("preset"), name);
  preset_lib_module_callback_description *callback_description
      = g_malloc(sizeof(preset_lib_module_callback_description));
  callback_description->module = module;
  callback_description->name = name;

  GClosure *closure = g_cclosure_new(G_CALLBACK(preset_lib_module_callback), callback_description,
                                     preset_lib_module_callback_destroyer);
  dt_accel_connect_lib(module, build_path, closure);
}

void dt_accel_deregister_iop(dt_iop_module_t *module, const gchar *path)
{
  char build_path[1024];
  dt_accel_path_iop(build_path, sizeof(build_path), module->op, path);

  dt_accel_t *accel = NULL;

  GList *modules = g_list_first(darktable.develop->iop);
  while(modules)
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)modules->data;

    if(mod->so == module->so)
    {
      GSList **current_list = &mod->accel_closures;
      GSList *l = *current_list;
      while(l)
      {
        _accel_iop_t *iop_accel = (_accel_iop_t *)l->data;

        if(iop_accel && iop_accel->accel && !strncmp(iop_accel->accel->path, build_path, 1024))
        {
          accel = iop_accel->accel;

          if(iop_accel->closure == accel->closure || (accel->local && module->local_closures_connected))
            gtk_accel_group_disconnect(darktable.control->accelerators, iop_accel->closure);

          *current_list = g_slist_delete_link(*current_list, l);

          g_closure_unref(iop_accel->closure);

          g_free(iop_accel);

          break;
        }

        l = g_slist_next(l);
        if(!l && current_list == &mod->accel_closures) l = *(current_list = &module->accel_closures_local);
      }
    }

    modules = g_list_next(modules);
  }

  if(accel)
  {
      darktable.control->accelerator_list = g_slist_remove(darktable.control->accelerator_list, accel);

      g_free(accel);
  }
}

void dt_accel_deregister_lib(dt_lib_module_t *module, const gchar *path)
{
  char build_path[1024];
  dt_accel_path_lib(build_path, sizeof(build_path), module->plugin_name, path);
  GSList *l = module->accel_closures;
  while(l)
  {
    dt_accel_t *accel = (dt_accel_t *)l->data;
    if(accel && !strncmp(accel->path, build_path, 1024))
    {
      module->accel_closures = g_slist_delete_link(module->accel_closures, l);
      gtk_accel_group_disconnect(darktable.control->accelerators, accel->closure);
      l = NULL;
    }
    else
    {
      l = g_slist_next(l);
    }
  }
  l = darktable.control->accelerator_list;
  while(l)
  {
    dt_accel_t *accel = (dt_accel_t *)l->data;
    if(accel && !strncmp(accel->path, build_path, 1024))
    {
      darktable.control->accelerator_list = g_slist_delete_link(darktable.control->accelerator_list, l);
      l = NULL;
      g_free(accel);
    }
    else
    {
      l = g_slist_next(l);
    }
  }
}

void dt_accel_deregister_global(const gchar *path)
{
  char build_path[1024];
  dt_accel_path_global(build_path, sizeof(build_path), path);
  GSList *l = darktable.control->accelerator_list;
  while(l)
  {
    dt_accel_t *accel = (dt_accel_t *)l->data;
    if(accel && !strncmp(accel->path, build_path, 1024))
    {
      darktable.control->accelerator_list = g_slist_delete_link(darktable.control->accelerator_list, l);
      gtk_accel_group_disconnect(darktable.control->accelerators, accel->closure);
      l = NULL;
      g_free(accel);
    }
    else
    {
      l = g_slist_next(l);
    }
  }
}

void dt_accel_deregister_lua(const gchar *path)
{
  char build_path[1024];
  dt_accel_path_lua(build_path, sizeof(build_path), path);
  GSList *l = darktable.control->accelerator_list;
  while(l)
  {
    dt_accel_t *accel = (dt_accel_t *)l->data;
    if(accel && !strncmp(accel->path, build_path, 1024))
    {
      darktable.control->accelerator_list = g_slist_delete_link(darktable.control->accelerator_list, l);
      gtk_accel_group_disconnect(darktable.control->accelerators, accel->closure);
      l = NULL;
      g_free(accel);
    }
    else
    {
      l = g_slist_next(l);
    }
  }
}

void dt_accel_deregister_manual(GSList *list, const gchar *full_path)
{
  GSList *l;
  char build_path[1024];
  dt_accel_path_manual(build_path, sizeof(build_path), full_path);
  l = list;
  while(l)
  {
    dt_accel_t *accel = (dt_accel_t *)l->data;
    if(accel && !strncmp(accel->path, build_path, 1024))
    {
      list = g_slist_delete_link(list, l);
      gtk_accel_group_disconnect(darktable.control->accelerators, accel->closure);
      l = NULL;
    }
    else
    {
      l = g_slist_next(l);
    }
  }
  l = darktable.control->accelerator_list;
  while(l)
  {
    dt_accel_t *accel = (dt_accel_t *)l->data;
    if(accel && !strncmp(accel->path, build_path, 1024))
    {
      darktable.control->accelerator_list = g_slist_delete_link(darktable.control->accelerator_list, l);
      l = NULL;
      g_free(accel);
    }
    else
    {
      l = g_slist_next(l);
    }
  }
}

gboolean find_accel_internal(GtkAccelKey *key, GClosure *closure, gpointer data)
{
  return (closure == data);
}

void dt_accel_rename_preset_iop(dt_iop_module_t *module, const gchar *path, const gchar *new_path)
{
  char *path_preset = g_strdup_printf("%s`%s", N_("preset"), path);

  char build_path[1024];
  dt_accel_path_iop(build_path, sizeof(build_path), module->op, path_preset);

  GSList *l = module->accel_closures;
  while(l)
  {
    _accel_iop_t *iop_accel = (_accel_iop_t *)l->data;
    if(iop_accel && iop_accel->accel && !strncmp(iop_accel->accel->path, build_path, 1024))
    {
      GtkAccelKey tmp_key
          = *(gtk_accel_group_find(darktable.control->accelerators, find_accel_internal, iop_accel->closure));
      gboolean local = iop_accel->accel->local;

      dt_accel_deregister_iop(module, path_preset);

      snprintf(build_path, sizeof(build_path), "%s`%s", N_("preset"), new_path);
      dt_accel_register_iop(module->so, local, build_path, tmp_key.accel_key, tmp_key.accel_mods);

      GList *modules = g_list_first(darktable.develop->iop);
      while(modules)
      {
        dt_iop_module_t *mod = (dt_iop_module_t *)modules->data;

        if(mod->so == module->so) dt_accel_connect_preset_iop(mod, new_path);

        modules = g_list_next(modules);
      }

      break;
    }

    l = g_slist_next(l);
  }

  g_free(path_preset);

  dt_accel_connect_instance_iop(module);
}

void dt_accel_rename_preset_lib(dt_lib_module_t *module, const gchar *path, const gchar *new_path)
{
  char build_path[1024];
  dt_accel_path_lib(build_path, sizeof(build_path), module->plugin_name, path);
  GSList *l = module->accel_closures;
  while(l)
  {
    dt_accel_t *accel = (dt_accel_t *)l->data;
    if(accel && !strncmp(accel->path, build_path, 1024))
    {
      GtkAccelKey tmp_key
          = *(gtk_accel_group_find(darktable.control->accelerators, find_accel_internal, accel->closure));
      dt_accel_deregister_lib(module, path);
      snprintf(build_path, sizeof(build_path), "%s/%s", _("preset"), new_path);
      dt_accel_register_lib(module, build_path, tmp_key.accel_key, tmp_key.accel_mods);
      dt_accel_connect_preset_lib(module, new_path);
      l = NULL;
    }
    else
    {
      l = g_slist_next(l);
    }
  }
}

void dt_accel_rename_global(const gchar *path, const gchar *new_path)
{
  char build_path[1024];
  dt_accel_path_global(build_path, sizeof(build_path), path);
  GSList *l = darktable.control->accelerator_list;
  while(l)
  {
    dt_accel_t *accel = (dt_accel_t *)l->data;
    if(accel && !strncmp(accel->path, build_path, 1024))
    {
      GtkAccelKey tmp_key
          = *(gtk_accel_group_find(darktable.control->accelerators, find_accel_internal, accel->closure));
      dt_accel_deregister_global(path);
      g_closure_ref(accel->closure);
      dt_accel_register_global(new_path, tmp_key.accel_key, tmp_key.accel_mods);
      dt_accel_connect_global(new_path, accel->closure);
      g_closure_unref(accel->closure);
      l = NULL;
    }
    else
    {
      l = g_slist_next(l);
    }
  }
}

void dt_accel_rename_lua(const gchar *path, const gchar *new_path)
{
  char build_path[1024];
  dt_accel_path_lua(build_path, sizeof(build_path), path);
  GSList *l = darktable.control->accelerator_list;
  while(l)
  {
    dt_accel_t *accel = (dt_accel_t *)l->data;
    if(accel && !strncmp(accel->path, build_path, 1024))
    {
      GtkAccelKey tmp_key
          = *(gtk_accel_group_find(darktable.control->accelerators, find_accel_internal, accel->closure));
      dt_accel_deregister_lua(path);
      g_closure_ref(accel->closure);
      dt_accel_register_lua(new_path, tmp_key.accel_key, tmp_key.accel_mods);
      dt_accel_connect_lua(new_path, accel->closure);
      g_closure_unref(accel->closure);
      l = NULL;
    }
    else
    {
      l = g_slist_next(l);
    }
  }
}

dt_accel_t *dt_accel_find_by_path(const gchar *path)
{
  return _lookup_accel(path);
}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
