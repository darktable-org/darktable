/*
    This file is part of darktable,
    copyright (c) 2011 Henrik Andersson.

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

#include "common/collection.h"
#include "common/darktable.h"
#include "common/l10n.h"
#include "control/conf.h"
#include "control/control.h"
#include "dtgtk/button.h"
#include "dtgtk/togglebutton.h"
#include "gui/accelerators.h"
#include "gui/preferences.h"
#include "libs/lib.h"
#include "libs/lib_api.h"
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif

DT_MODULE(1)

typedef struct dt_lib_tool_preferences_t
{
  GtkWidget *preferences_button, *grouping_button, *overlays_button, *help_button;
} dt_lib_tool_preferences_t;

/* callback for grouping button */
static void _lib_filter_grouping_button_clicked(GtkWidget *widget, gpointer user_data);
/* callback for preference button */
static void _lib_preferences_button_clicked(GtkWidget *widget, gpointer user_data);
/* callback for overlays button */
static void _lib_overlays_button_clicked(GtkWidget *widget, gpointer user_data);
/* callback for help button */
static void _lib_help_button_clicked(GtkWidget *widget, gpointer user_data);
static void _paint_help(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);

const char *name(dt_lib_module_t *self)
{
  return _("preferences");
}

const char **views(dt_lib_module_t *self)
{
  static const char *v[] = {"*", NULL};
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_CENTER_TOP_RIGHT;
}

int expandable(dt_lib_module_t *self)
{
  return 0;
}

int position()
{
  return 1001;
}

void gui_init(dt_lib_module_t *self)
{
  /* initialize ui widgets */
  dt_lib_tool_preferences_t *d = (dt_lib_tool_preferences_t *)g_malloc0(sizeof(dt_lib_tool_preferences_t));
  self->data = (void *)d;

  self->widget = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);

  /* create the grouping button */
  d->grouping_button = dtgtk_togglebutton_new(dtgtk_cairo_paint_grouping, CPF_STYLE_FLAT, NULL);
  gtk_widget_set_size_request(d->grouping_button, DT_PIXEL_APPLY_DPI(18), DT_PIXEL_APPLY_DPI(18));
  gtk_box_pack_start(GTK_BOX(self->widget), d->grouping_button, FALSE, FALSE, 2);
  if(darktable.gui->grouping)
    gtk_widget_set_tooltip_text(d->grouping_button, _("expand grouped images"));
  else
    gtk_widget_set_tooltip_text(d->grouping_button, _("collapse grouped images"));
  g_signal_connect(G_OBJECT(d->grouping_button), "clicked", G_CALLBACK(_lib_filter_grouping_button_clicked),
                   NULL);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->grouping_button), darktable.gui->grouping);

  /* create the "show/hide overlays" button */
  d->overlays_button = dtgtk_togglebutton_new(dtgtk_cairo_paint_overlays, CPF_STYLE_FLAT, NULL);
  gtk_widget_set_size_request(d->overlays_button, DT_PIXEL_APPLY_DPI(18), DT_PIXEL_APPLY_DPI(18));
  gtk_box_pack_start(GTK_BOX(self->widget), d->overlays_button, FALSE, FALSE, 2);
  if(darktable.gui->show_overlays)
    gtk_widget_set_tooltip_text(d->overlays_button, _("hide image overlays"));
  else
    gtk_widget_set_tooltip_text(d->overlays_button, _("show image overlays"));
  g_signal_connect(G_OBJECT(d->overlays_button), "clicked", G_CALLBACK(_lib_overlays_button_clicked), NULL);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->overlays_button), darktable.gui->show_overlays);

  /* create the widget help button */
  d->help_button = dtgtk_togglebutton_new(_paint_help, CPF_STYLE_FLAT, NULL);
  gtk_widget_set_size_request(d->help_button, DT_PIXEL_APPLY_DPI(18), DT_PIXEL_APPLY_DPI(18));
  gtk_box_pack_start(GTK_BOX(self->widget), d->help_button, FALSE, FALSE, 2);
  gtk_widget_set_tooltip_text(d->help_button, _("enable this, then click on a control element to see its online help"));
  g_signal_connect(G_OBJECT(d->help_button), "clicked", G_CALLBACK(_lib_help_button_clicked), d);
  dt_gui_add_help_link(d->help_button, dt_get_help_url("global_toolbox_help"));

  // the rest of these is added in reverse order as they are always put at the end of the container.
  // that's done so that buttons added via Lua will come first.

  /* create the preference button */
  d->preferences_button = dtgtk_button_new(dtgtk_cairo_paint_preferences, CPF_STYLE_FLAT, NULL);
  gtk_widget_set_size_request(d->preferences_button, DT_PIXEL_APPLY_DPI(18), DT_PIXEL_APPLY_DPI(18));
  gtk_box_pack_end(GTK_BOX(self->widget), d->preferences_button, FALSE, FALSE, 2);
  gtk_widget_set_tooltip_text(d->preferences_button, _("show global preferences"));
  g_signal_connect(G_OBJECT(d->preferences_button), "clicked", G_CALLBACK(_lib_preferences_button_clicked),
                   NULL);
  dt_gui_add_help_link(d->preferences_button, dt_get_help_url("global_toolbox_preferences"));
}

void gui_cleanup(dt_lib_module_t *self)
{
  g_free(self->data);
  self->data = NULL;
}

void _lib_preferences_button_clicked(GtkWidget *widget, gpointer user_data)
{
  dt_gui_preferences_show();
}

static void _lib_filter_grouping_button_clicked(GtkWidget *widget, gpointer user_data)
{

  darktable.gui->grouping = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));
  if(darktable.gui->grouping)
    gtk_widget_set_tooltip_text(widget, _("expand grouped images"));
  else
    gtk_widget_set_tooltip_text(widget, _("collapse grouped images"));
  dt_conf_set_bool("ui_last/grouping", darktable.gui->grouping);
  darktable.gui->expanded_group_id = -1;
  dt_collection_update_query(darktable.collection);

#ifdef USE_LUA
  dt_lua_async_call_alien(dt_lua_event_trigger_wrapper,
      0,NULL,NULL,
      LUA_ASYNC_TYPENAME,"const char*","global_toolbox-grouping_toggle",
      LUA_ASYNC_TYPENAME,"bool",darktable.gui->grouping,
      LUA_ASYNC_DONE);
#endif // USE_LUA
}

static void _lib_overlays_button_clicked(GtkWidget *widget, gpointer user_data)
{
  darktable.gui->show_overlays = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));
  if(darktable.gui->show_overlays)
    gtk_widget_set_tooltip_text(widget, _("hide image overlays"));
  else
    gtk_widget_set_tooltip_text(widget, _("show image overlays"));
  dt_conf_set_bool("lighttable/ui/expose_statuses", darktable.gui->show_overlays);
  dt_control_signal_raise(darktable.signals, DT_SIGNAL_COLLECTION_CHANGED);

#ifdef USE_LUA
  dt_lua_async_call_alien(dt_lua_event_trigger_wrapper,
      0,NULL,NULL,
      LUA_ASYNC_TYPENAME,"const char*","global_toolbox-overlay_toggle",
      LUA_ASYNC_TYPENAME,"bool",darktable.gui->show_overlays,
      LUA_ASYNC_DONE);
#endif // USE_LUA
}

// TODO: this doesn't work for all widgets. the reason being that the GtkEventBox we put libs/iops into catches events.
static char *get_help_url(GtkWidget *widget)
{
  while(widget)
  {
    // if the widget doesn't have a help url set go up the widget hierarchy to find a parent that has an url
    gchar *help_url = g_object_get_data(G_OBJECT(widget), "dt-help-url");

    if(help_url)
      return help_url;

    // TODO: shall we cross from libs/iops to the core gui? if not, here is the place to break out of the loop

    widget = gtk_widget_get_parent(widget);
  }

  return NULL;
}

static void _main_do_event(GdkEvent *event, gpointer data)
{
  dt_lib_tool_preferences_t *d = (dt_lib_tool_preferences_t *)data;

  gboolean handled = FALSE;

  switch(event->type)
  {
    case GDK_BUTTON_PRESS:
    {
      // reset GTK to normal behaviour
      dt_control_allow_change_cursor();
      dt_control_change_cursor(GDK_LEFT_PTR);
      gdk_event_handler_set((GdkEventFunc)gtk_main_do_event, NULL, NULL);
      g_signal_handlers_block_by_func(d->help_button, _lib_help_button_clicked, d);
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->help_button), FALSE);
      g_signal_handlers_unblock_by_func(d->help_button, _lib_help_button_clicked, d);

      GtkWidget *event_widget = gtk_get_event_widget(event);
      if(event_widget)
      {
        // TODO: When the widget doesn't have a help url set we should probably look at the parent(s)
        gchar *help_url = get_help_url(event_widget);
        if(help_url && *help_url)
        {
          GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
          dt_print(DT_DEBUG_CONTROL, "[context help] opening `%s'\n", help_url);
          char *base_url = dt_conf_get_string("context_help/url");
          if(!base_url || !*base_url)
          {
            g_free(base_url);
            base_url = NULL;

            // ask the user if darktable.org may be accessed
            GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(win), GTK_DIALOG_DESTROY_WITH_PARENT,
                                                       GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO,
                                                       _("do you want to access https://www.darktable.org/?"));
#ifdef GDK_WINDOWING_QUARTZ
            dt_osx_disallow_fullscreen(dialog);
#endif

            gtk_window_set_title(GTK_WINDOW(dialog), _("access the online usermanual?"));
            gint res = gtk_dialog_run(GTK_DIALOG(dialog));
            gtk_widget_destroy(dialog);
            if(res == GTK_RESPONSE_YES)
            {
              base_url = g_strdup("https://www.darktable.org/usermanual/");
              dt_conf_set_string("context_help/url", base_url);
            }
          }
          if(base_url)
          {
            gboolean is_language_supported = FALSE;
            char *lang = "en";
            if(darktable.l10n!=NULL)
            {
              dt_l10n_language_t *language = NULL;
              if(darktable.l10n->selected!=-1)
                  language = (dt_l10n_language_t *)g_list_nth(darktable.l10n->languages, darktable.l10n->selected)->data;
              if (language != NULL)
                lang = language->code;
              // array of languages the usermanual supports.
              // NULL MUST remain the last element of the array
              const char *supported_languages[] = { "en", "fr", "it", "es", NULL };
              int i = 0;
              while(supported_languages[i])
              {
                if(!strcmp(lang, supported_languages[i]))
                {
                  is_language_supported = TRUE;
                  break;
                }
                i++;
              }
            }
            if(!is_language_supported) lang = "en";
            char *url = g_build_path("/", base_url, lang, help_url, NULL);
            // TODO: call the web browser directly so that file:// style base for local installs works
#if GTK_CHECK_VERSION(3, 22, 0)
            gtk_show_uri_on_window(GTK_WINDOW(win), url, gtk_get_current_event_time(), NULL);
#else
            gtk_show_uri(gdk_screen_get_default(), url, gtk_get_current_event_time(), NULL);
#endif
            g_free(base_url);
            g_free(url);
            dt_control_log(_("help url opened in web brower"));
          }
        }
        else
        {
          dt_control_log(_("there is no help available for this element"));
        }
      }
      handled = TRUE;
      break;
    }
    case GDK_ENTER_NOTIFY:
    case GDK_LEAVE_NOTIFY:
    {
      GtkWidget *event_widget = gtk_get_event_widget(event);
      if(event_widget)
      {
        gchar *help_url = get_help_url(event_widget);
        if(help_url)
        {
          // TODO: find a better way to tell the user that the hovered widget has a help link
          dt_cursor_t cursor = event->type == GDK_ENTER_NOTIFY ? GDK_QUESTION_ARROW : GDK_X_CURSOR;
          dt_control_allow_change_cursor();
          dt_control_change_cursor(cursor);
          dt_control_forbid_change_cursor();
        }
      }
      break;
    }
    default:
      break;
  }

  if(!handled) gtk_main_do_event(event);
}

static void _lib_help_button_clicked(GtkWidget *widget, gpointer user_data)
{
  dt_control_change_cursor(GDK_X_CURSOR);
  dt_control_forbid_change_cursor();
  gdk_event_handler_set(_main_do_event, user_data, NULL);
}

static void _paint_help(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PangoLayout *layout;
  PangoRectangle ink;
  // grow is needed because ink.* are int and everything gets rounded to 1 or so otherwise,
  // leading to imprecise positioning
  static const float grow = 10.0;
  PangoFontDescription *desc = pango_font_description_from_string("sans-serif bold");
  pango_font_description_set_absolute_size(desc, 2.7 * grow * PANGO_SCALE);
  layout = pango_cairo_create_layout(cr);
  pango_layout_set_font_description(layout, desc);
  gint s = (w < h ? w : h);
  cairo_translate(cr, x + (w / 2.0), y + (h / 2.0));
  cairo_scale(cr, s / grow, s / grow);

  pango_layout_set_text(layout, "?", -1);
  pango_layout_get_pixel_extents(layout, &ink, NULL);
  cairo_move_to(cr, 0 - ink.x - ink.width / 2.0, 0 - ink.y - ink.height / 2.0);
  pango_cairo_show_layout(cr, layout);
  pango_font_description_free(desc);
  g_object_unref(layout);
}

void init_key_accels(dt_lib_module_t *self)
{
  dt_accel_register_lib(self, NC_("accel", "grouping"), 0, 0);
  dt_accel_register_lib(self, NC_("accel", "preferences"), 0, 0);
  dt_accel_register_lib(self, NC_("accel", "show overlays"), 0, 0);
}

void connect_key_accels(dt_lib_module_t *self)
{
  dt_lib_tool_preferences_t *d = (dt_lib_tool_preferences_t *)self->data;

  dt_accel_connect_button_lib(self, "grouping", d->grouping_button);
  dt_accel_connect_button_lib(self, "preferences", d->preferences_button);
  dt_accel_connect_button_lib(self, "show overlays", d->overlays_button);
}

#ifdef USE_LUA

static int grouping_member(lua_State *L)
{
  dt_lib_module_t *self = *(dt_lib_module_t **)lua_touserdata(L, 1);
  dt_lib_tool_preferences_t *d = (dt_lib_tool_preferences_t *)self->data;
  if(lua_gettop(L) != 3)
  {
    lua_pushboolean(L, darktable.gui->grouping);
    return 1;
  }
  else
  {
    gboolean value = lua_toboolean(L, 3);
    if(darktable.gui->grouping != value)
    {
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->grouping_button), value);
    }
  }
  return 0;
}

static int show_overlays_member(lua_State *L)
{
  dt_lib_module_t *self = *(dt_lib_module_t **)lua_touserdata(L, 1);
  dt_lib_tool_preferences_t *d = (dt_lib_tool_preferences_t *)self->data;
  if(lua_gettop(L) != 3)
  {
    lua_pushboolean(L, darktable.gui->show_overlays);
    return 1;
  }
  else
  {
    gboolean value = lua_toboolean(L, 3);
    if(darktable.gui->show_overlays != value)
    {
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->overlays_button), value);
    }
  }
  return 0;
}

void init(struct dt_lib_module_t *self)
{
  lua_State *L = darktable.lua_state.state;
  int my_type = dt_lua_module_entry_get_type(L, "lib", self->plugin_name);

  lua_pushcfunction(L, grouping_member);
  dt_lua_gtk_wrap(L);
  dt_lua_type_register_type(L, my_type, "grouping");
  lua_pushcfunction(L, show_overlays_member);
  dt_lua_gtk_wrap(L);
  dt_lua_type_register_type(L, my_type, "show_overlays");

  lua_pushcfunction(L, dt_lua_event_multiinstance_register);
  lua_pushcfunction(L, dt_lua_event_multiinstance_trigger);
  dt_lua_event_add(L, "global_toolbox-grouping_toggle");

  lua_pushcfunction(L, dt_lua_event_multiinstance_register);
  lua_pushcfunction(L, dt_lua_event_multiinstance_trigger);
  dt_lua_event_add(L, "global_toolbox-overlay_toggle");
}

#endif // USE_LUA

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
