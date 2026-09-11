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
#include "gui/gtk.h"
#include "gui/styles.h"
#include <glib-2.0/gio/gmenu.h>
#include <glib-2.0/gio/gmenumodel.h>
#include <glib-2.0/glib-object.h>
#include <glib-2.0/glib.h>

static GList *_menu_data_list = NULL;

// static gboolean _styles_tooltip_callback(GtkWidget* self,
//                                          const gint x,
//                                          const gint y,
//                                          const gboolean keyboard_mode,
//                                          GtkTooltip* tooltip,
//                                          gpointer user_data)
// {
//   gchar *name = (char *)user_data;
//   dt_develop_t *dev = darktable.develop;
//   // get the center-view image in darkroom view, or the active act-on image otherwise
//   const dt_imgid_t imgid = (dev && dt_is_valid_imgid(dev->image_storage.id))
//     ? dev->image_storage.id : dt_act_on_get_main_image();
//
//   if(!dt_is_valid_imgid(imgid))
//     return FALSE;
//
//   // write history to ensure the preview will be done with latest
//   // development history.
//   if(dev)
//     dt_dev_write_history(dev);
//
//   GtkWidget *ht = dt_gui_style_content_dialog(name, imgid);
//
//   return dt_shortcut_tooltip_callback(self, x, y, keyboard_mode, tooltip, ht);
// }


static void _free_menu_data(void *data)
{
  dt_stylemenu_data_t *menu_data = data;
  g_free(menu_data->name);
  free(menu_data);
}

void dtgtk_stylemenu_free_menu_data()
{
  g_list_free_full(_menu_data_list, _free_menu_data);
  _menu_data_list = NULL;
}

static void _build_style_submenus(GMenu *menu,
                                  const gchar *style_name,
                                  gchar **splits,
                                  const int index,
                                  gpointer user_data)
{
  // localize the name of the current level in the hierarchy
  const char *split0 = dt_util_localize_string(splits[index]);

  // check if we already have an item or sub-menu with this name
  GMenu *sm = NULL;
  for(gint i = 0; i < g_menu_model_get_n_items(G_MENU_MODEL(menu)); i++)
  {
    gchar *label = NULL;
    g_menu_model_get_item_attribute(G_MENU_MODEL(menu), i,
                                    G_MENU_ATTRIBUTE_LABEL, "s", &label);

    if(g_strcmp0(split0, label) == 0)
    {
      sm = G_MENU(g_menu_model_get_item_link(G_MENU_MODEL(menu), i,
                                             G_MENU_LINK_SUBMENU));
      g_free(label);
      break;
    }
    g_free(label);
  }

  if(splits[index+1])
  {
    // an intermediate level of the hierarchy: reuse the sub-menu built for an
    // earlier style in the same group, or create the item that opens it
    if(!sm)
    {
      GMenuItem *node = g_menu_item_new(split0, NULL);
      sm = g_menu_new();
      g_menu_item_set_submenu(node, G_MENU_MODEL(sm));
      g_menu_append_item(menu, node);
      g_object_unref(node);
    }
    _build_style_submenus(sm, style_name, splits, index+1, user_data);
    // both paths above leave us owning one reference: g_menu_model_get_item_link()
    // returns a new one, and g_menu_item_set_submenu() takes its own. the parent
    // menu keeps the submenu alive from here on
    g_object_unref(sm);
    return;
  }

  // we've reached the bottom level, so build a final menu item
  // a style can share its name with an existing group, so sm may be set here too
  if(sm)
    g_object_unref(sm);

  dt_stylemenu_data_t *menu_data = malloc(sizeof(dt_stylemenu_data_t));
  if(menu_data)
  {
    menu_data->name = g_strdup(style_name);
    menu_data->user_data = user_data;

    // store all the menu_data allocs to free them on menu close
    _menu_data_list = g_list_prepend(_menu_data_list, menu_data);

    GMenuItem *mi = g_menu_item_new(split0[0] ? split0 : _("none"), NULL);
    g_menu_item_set_action_and_target_value(mi,
                                            "styles.activate",
                                            g_variant_new("t", (guintptr)menu_data));
    g_menu_append_item(menu, mi);
    g_object_unref(mi);
  }

  // we've reached the bottom level, so build a final menu item with preview popup
  // need a tooltip for the signal below to be raised
  // GtkMenuItem *mi = GTK_MENU_ITEM(gtk_menu_item_new_with_label(split0[0] ? split0 : _("none")));
  // gtk_menu_shell_append(menu, GTK_WIDGET(mi));
  // if(style_name && style_name[0]) // don't add tooltip for "none" style
  // {
  //   gtk_widget_set_has_tooltip(GTK_WIDGET(mi), TRUE);
  //   g_signal_connect_data(mi, "query-tooltip",
  //                         G_CALLBACK(_styles_tooltip_callback),
  //                         g_strdup(style_name), (GClosureNotify)g_free, 0);
  //   dt_action_define(&darktable.control->actions_global, "styles", style_name, GTK_WIDGET(mi), NULL);
  // }
  // else
  //   gtk_widget_set_has_tooltip(GTK_WIDGET(mi), FALSE);
}


GMenu *dtgtk_build_style_menu_hierarchy(gboolean allow_none,
                                        gpointer user_data)
{
  GMenu *menu = NULL;

  GList *styles = dt_styles_get_list("");
  if(styles || allow_none)
  {
    menu = g_menu_new();
    if(allow_none)
    {
      const char *none = "";
      gchar *split[] = { (gchar*)none, 0 };
      _build_style_submenus(menu, none, split, 0, user_data);
    }
    for(const GList *st_iter = styles; st_iter; st_iter = g_list_next(st_iter))
    {
      dt_style_t *style = (dt_style_t *)st_iter->data;

      gchar **split = g_strsplit(style->name, "|", 0);
      _build_style_submenus(menu, style->name, split, 0, user_data);
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
