/*
    This file is part of darktable,
    Copyright (C) 2024 darktable developers.

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

#include "control/control.h"
#include "common/act_on.h"
#include "common/styles.h"
#include "common/utility.h"
#include "dtgtk/stylemenu.h"
#include "gui/accelerators.h"
#include "gui/styles.h"

static gboolean _styles_tooltip_callback(GtkWidget* self,
                                         const gint x,
                                         const gint y,
                                         const gboolean keyboard_mode,
                                         GtkTooltip* tooltip,
                                         gpointer user_data)
{
  gchar *name = (char *)user_data;
  dt_develop_t *dev = darktable.develop;
  // get the center-view image in darkroom view, or the active act-on image otherwise
  const dt_imgid_t imgid = (dev && dt_is_valid_imgid(dev->image_storage.id))
    ? dev->image_storage.id : dt_act_on_get_main_image();

  if(!dt_is_valid_imgid(imgid))
    return FALSE;

  // write history to ensure the preview will be done with latest
  // development history.
  if(dev)
    dt_dev_write_history(dev);

  GtkWidget *ht = dt_gui_style_content_dialog(name, imgid);

  return dt_shortcut_tooltip_callback(self, x, y, keyboard_mode, tooltip, ht);
}

static void _free_menu_data(dt_stylemenu_data_t *data)
{
  g_free(data->name);
  free(data);
}

static void _build_style_submenus(GtkMenuShell *menu,
                                  const gchar *style_name,
                                  gchar **splits,
                                  const int index,
                                  dtgtk_menuitem_activate_callback_fn *activate_callback,
                                  dtgtk_menuitem_button_callback_fn *button_callback,
                                  gpointer user_data)
{
  // localize the name of the current level in the hierarchy
  const char *split0 = dt_util_localize_string(splits[index]);
  GtkMenuItem *mi = GTK_MENU_ITEM(gtk_menu_item_new_with_label(split0[0] ? split0 : _("none")));

  // check if we already have an item or sub-menu with this name
  GtkMenu *sm = NULL;
  GList *children = gtk_container_get_children(GTK_CONTAINER(menu));
  for(const GList *child = children; child; child = g_list_next(child))
  {
    GtkMenuItem *smi = (GtkMenuItem *)child->data;
    if(g_strcmp0(split0,gtk_menu_item_get_label(smi)) == 0)
    {
      sm = (GtkMenu *)gtk_menu_item_get_submenu(smi);
      break;
    }
  }
  g_list_free(children);

  if(!splits[index+1])
  {
    // we've reached the bottom level, so build a final menu item with preview popup
    // need a tooltip for the signal below to be raised
    gtk_menu_shell_append(menu, GTK_WIDGET(mi));
    if(style_name && style_name[0]) // don't add tooltip for "none" style
    {
      gtk_widget_set_has_tooltip(GTK_WIDGET(mi), TRUE);
      g_signal_connect_data(mi, "query-tooltip",
                            G_CALLBACK(_styles_tooltip_callback),
                            g_strdup(style_name), (GClosureNotify)g_free, 0);
      dt_action_define(&darktable.control->actions_global, "styles", style_name, GTK_WIDGET(mi), NULL);
    }
    else
      gtk_widget_set_has_tooltip(GTK_WIDGET(mi), FALSE);
  }
  else
  {
    if (!sm)
    {
      // we need a sub-menu, but it doesn't exist yet
      sm = (GtkMenu*)gtk_menu_new();
      gtk_menu_item_set_submenu(mi, GTK_WIDGET(sm));
      gtk_menu_shell_append(menu, GTK_WIDGET(mi));
    }
    _build_style_submenus(GTK_MENU_SHELL(sm), style_name, splits, index+1,
                          activate_callback, button_callback, user_data);
  }

  if(activate_callback)
  {
    dt_stylemenu_data_t *menu_data = malloc(sizeof(dt_stylemenu_data_t));
    if(menu_data)
    {
      menu_data->name = g_strdup(style_name);
      menu_data->user_data = user_data;
      g_signal_connect_data(G_OBJECT(mi), "activate",
                            G_CALLBACK(activate_callback),
                            menu_data, (GClosureNotify)_free_menu_data, 0);
    }
  }
  if(button_callback)
  {
    dt_stylemenu_data_t *menu_data = malloc(sizeof(dt_stylemenu_data_t));
    if(menu_data)
    {
      menu_data->name = g_strdup(style_name);
      menu_data->user_data = user_data;
      g_signal_connect_data(G_OBJECT(mi), "button-press-event",
                            G_CALLBACK(button_callback),
                            menu_data, (GClosureNotify)_free_menu_data, 0);
    }
  }

  gtk_widget_show(GTK_WIDGET(mi));
}


GtkMenuShell *dtgtk_build_style_menu_hierarchy(gboolean allow_none,
                                               dtgtk_menuitem_activate_callback_fn *activate_callback,
                                               dtgtk_menuitem_button_callback_fn *button_callback,
                                               gpointer user_data)
{
  GtkMenuShell *menu = NULL;

  GList *styles = dt_styles_get_list("");
  if(styles || allow_none)
  {
    menu = GTK_MENU_SHELL(gtk_menu_new());
    if(allow_none)
    {
      const char *none = "";
      gchar *split[] = { (gchar*)none, 0 };
      _build_style_submenus(menu, none, split, 0, activate_callback, button_callback, user_data);
    }
    for(const GList *st_iter = styles; st_iter; st_iter = g_list_next(st_iter))
    {
      dt_style_t *style = (dt_style_t *)st_iter->data;

      gchar **split = g_strsplit(style->name, "|", 0);
      _build_style_submenus(menu, style->name, split, 0, activate_callback, button_callback, user_data);
      g_strfreev(split);
    }
    g_list_free_full(styles, dt_style_free);
  }
  return menu;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
