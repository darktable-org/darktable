/*
    This file is part of darktable,
    Copyright (C) 2010-2023 darktable developers.

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

#include <gdk/gdkkeysyms.h>
#include <strings.h>

#include "bauhaus/bauhaus.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/file_location.h"
#include "common/l10n.h"
#include "common/presets.h"
#include "control/control.h"
#include "develop/imageop.h"
#include "gui/accelerators.h"
#include "gui/draw.h"
#include "gui/gtk.h"
#include "gui/preferences.h"
#include "gui/presets.h"
#include "libs/lib.h"
#include "preferences_gen.h"
#ifdef USE_LUA
#include "lua/preferences.h"
#endif
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif
#define ICON_SIZE 13

typedef struct dt_gui_themetweak_widgets_t
{
  GtkWidget *apply_toggle, *save_button, *css_text_view;
} dt_gui_themetweak_widgets_t;

// link to values in gui/presets.c
// move to presets.h if needed elsewhere
extern const int dt_gui_presets_exposure_value_cnt;
extern const float dt_gui_presets_exposure_value[];
extern const char *dt_gui_presets_exposure_value_str[];
extern const int dt_gui_presets_aperture_value_cnt;
extern const float dt_gui_presets_aperture_value[];
extern const char *dt_gui_presets_aperture_value_str[];

// Values for the accelerators/presets treeview

enum
{
  P_ROWID_COLUMN,
  P_OPERATION_COLUMN,
  P_MODULE_COLUMN,
  P_EDITABLE_COLUMN,
  P_NAME_COLUMN,
  P_MODEL_COLUMN,
  P_MAKER_COLUMN,
  P_LENS_COLUMN,
  P_ISO_COLUMN,
  P_EXPOSURE_COLUMN,
  P_APERTURE_COLUMN,
  P_FOCAL_LENGTH_COLUMN,
  P_AUTOAPPLY_COLUMN,
  P_N_COLUMNS
};

static void init_tab_presets(GtkWidget *stack);
static void init_tab_accels(GtkWidget *stack);
static gint compare_rows_presets(GtkTreeModel *model, GtkTreeIter *a, GtkTreeIter *b, gpointer data);
static void import_preset(GtkButton *button, gpointer data);
static void export_preset(GtkButton *button, gpointer data);

// Signal handlers
static void tree_row_activated_presets(GtkTreeView *tree, GtkTreePath *path, GtkTreeViewColumn *column,
                                       gpointer data);
static void tree_selection_changed(GtkTreeSelection *selection, gpointer data);
static gboolean tree_key_press(GtkWidget *widget, GdkEventKey *event, gpointer data);
static gboolean tree_key_press_presets(GtkWidget *widget, GdkEventKey *event, gpointer data);

static GtkWidget *_preferences_dialog;

///////////// gui theme selection

static void load_themes_dir(const char *basedir)
{
  char *themes_dir = g_build_filename(basedir, "themes", NULL);
  GDir *dir = g_dir_open(themes_dir, 0, NULL);
  if(dir)
  {
    dt_print(DT_DEBUG_DEV, "adding themes directory: %s\n", themes_dir);

    const gchar *d_name;
    while((d_name = g_dir_read_name(dir)))
      darktable.themes = g_list_append(darktable.themes, g_strdup(d_name));
    g_dir_close(dir);
  }
  g_free(themes_dir);
}

static void load_themes(void)
{
  // Clear theme list...
  g_list_free_full(darktable.themes, g_free);
  darktable.themes = NULL;

  // check themes dirs
  gchar configdir[PATH_MAX] = { 0 };
  gchar datadir[PATH_MAX] = { 0 };
  dt_loc_get_datadir(datadir, sizeof(datadir));
  dt_loc_get_user_config_dir(configdir, sizeof(configdir));

  load_themes_dir(datadir);
  load_themes_dir(configdir);
}

static void reload_ui_last_theme(void)
{
  const char *theme = dt_conf_get_string_const("ui_last/theme");
  dt_gui_load_theme(theme);
  dt_bauhaus_load_theme();
}

static void theme_callback(GtkWidget *widget, gpointer user_data)
{
  const int selected = gtk_combo_box_get_active(GTK_COMBO_BOX(widget));
  gchar *theme = g_list_nth(darktable.themes, selected)->data;
  gchar *i = g_strrstr(theme, ".");
  if(i) *i = '\0';
  dt_gui_load_theme(theme);
  dt_bauhaus_load_theme();
}

static void usercss_callback(GtkWidget *widget, gpointer user_data)
{
  dt_conf_set_bool("themes/usercss", gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget)));
  reload_ui_last_theme();
}

static void font_size_changed_callback(GtkWidget *widget, gpointer user_data)
{
  dt_conf_set_float("font_size", gtk_spin_button_get_value(GTK_SPIN_BUTTON(widget)));
  reload_ui_last_theme();
}

static void dpi_scaling_changed_callback(GtkWidget *widget, gpointer user_data)
{
  float dpi = gtk_spin_button_get_value(GTK_SPIN_BUTTON(widget));
  if(dpi > 0.0) dpi = fmax(64, dpi); // else <= 0 -> use system default
  dt_conf_set_float("screen_dpi_overwrite", dpi);
  restart_required = TRUE;
  dt_configure_ppd_dpi(darktable.gui);
  dt_bauhaus_load_theme();
}

static void use_sys_font_callback(GtkWidget *widget, gpointer user_data)
{
  dt_conf_set_bool("use_system_font", gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget)));
  if(dt_conf_get_bool("use_system_font"))
    gtk_widget_set_state_flags(GTK_WIDGET(user_data), GTK_STATE_FLAG_INSENSITIVE, TRUE);
  else
    gtk_widget_set_state_flags(GTK_WIDGET(user_data), GTK_STATE_FLAG_NORMAL, TRUE);

  reload_ui_last_theme();
}

static void save_usercss(GtkTextBuffer *buffer)
{
  //get file locations
  char usercsspath[PATH_MAX] = { 0 }, configdir[PATH_MAX] = { 0 };
  dt_loc_get_user_config_dir(configdir, sizeof(configdir));
  g_snprintf(usercsspath, sizeof(usercsspath), "%s/user.css", configdir);

  //get the text
  GtkTextIter start, end;
  gtk_text_buffer_get_start_iter(buffer, &start);
  gtk_text_buffer_get_end_iter(buffer, &end);
  gchar *usercsscontent = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);

  //write to file
  GError *error = NULL;
  if(!g_file_set_contents(usercsspath, usercsscontent, -1, &error))
  {
    dt_print(DT_DEBUG_ALWAYS, "%s: error saving css to %s: %s\n",
             G_STRFUNC, usercsspath, error->message);
    g_clear_error(&error);
  }
  g_free(usercsscontent);
}

static void save_usercss_callback(GtkWidget *widget, gpointer user_data)
{
  dt_gui_themetweak_widgets_t *tw = (dt_gui_themetweak_widgets_t *)user_data;
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tw->css_text_view));

  save_usercss(buffer);

  if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(tw->apply_toggle)))
  {
    //reload the theme
    reload_ui_last_theme();
  }
  else
  {
    //toggle the apply button, which will also reload the theme
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(tw->apply_toggle), TRUE);
  }
}

static void usercss_dialog_callback(GtkDialog *dialog, gint response_id, gpointer user_data)
{
  //just save the latest css but don't reload the theme
  dt_gui_themetweak_widgets_t *tw = (dt_gui_themetweak_widgets_t *)user_data;
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tw->css_text_view));
  save_usercss(buffer);
}

///////////// gui language and theme selection

static void language_callback(GtkWidget *widget, gpointer user_data)
{
  int selected = gtk_combo_box_get_active(GTK_COMBO_BOX(widget));
  dt_l10n_language_t *language = (dt_l10n_language_t *)g_list_nth_data(darktable.l10n->languages, selected);
  if(darktable.l10n->sys_default == selected)
  {
    dt_conf_set_string("ui_last/gui_language", "");
    darktable.l10n->selected = darktable.l10n->sys_default;
  }
  else
  {
    dt_conf_set_string("ui_last/gui_language", language->code);
    darktable.l10n->selected = selected;
  }
  restart_required = TRUE;
}

static gboolean reset_language_widget(GtkWidget *label, GdkEventButton *event, GtkWidget *widget)
{
  if(event->type == GDK_2BUTTON_PRESS)
  {
    gtk_combo_box_set_active(GTK_COMBO_BOX(widget), darktable.l10n->sys_default);
    return TRUE;
  }
  return FALSE;
}

static void init_tab_general(GtkWidget *dialog, GtkWidget *stack, dt_gui_themetweak_widgets_t *tw)
{

  GtkWidget *container = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *grid = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(grid), DT_PIXEL_APPLY_DPI(3));
  gtk_grid_set_column_spacing(GTK_GRID(grid), DT_PIXEL_APPLY_DPI(5));
  gtk_widget_set_valign(grid, GTK_ALIGN_START);
  int line = 0;

  gtk_box_pack_start(GTK_BOX(container), grid, FALSE, FALSE, 0);

  gtk_stack_add_titled(GTK_STACK(stack), container, _("general"), _("general"));

  // language

  GtkWidget *label = gtk_label_new(_("interface language"));
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  GtkWidget *labelev = gtk_event_box_new();
  gtk_widget_add_events(labelev, GDK_BUTTON_PRESS_MASK);
  gtk_container_add(GTK_CONTAINER(labelev), label);
  GtkWidget *widget = gtk_combo_box_text_new();

  for(GList *iter = darktable.l10n->languages; iter; iter = g_list_next(iter))
  {
    const char *name = dt_l10n_get_name(iter->data);
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(widget), name);
  }

  gtk_combo_box_set_active(GTK_COMBO_BOX(widget), darktable.l10n->selected);
  g_signal_connect(G_OBJECT(widget), "changed", G_CALLBACK(language_callback), 0);
  gtk_widget_set_tooltip_text(labelev,  _("double-click to reset to the system language"));
  gtk_event_box_set_visible_window(GTK_EVENT_BOX(labelev), FALSE);
  gtk_widget_set_tooltip_text(widget, _("set the language of the user interface. the system default is marked with an * (needs a restart)"));
  gtk_grid_attach(GTK_GRID(grid), labelev, 0, line++, 1, 1);
  gtk_grid_attach_next_to(GTK_GRID(grid), widget, labelev, GTK_POS_RIGHT, 1, 1);
  g_signal_connect(G_OBJECT(labelev), "button-press-event", G_CALLBACK(reset_language_widget), (gpointer)widget);

  // theme

  load_themes();

  label = gtk_label_new(_("theme"));
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  widget = gtk_combo_box_text_new();
  labelev = gtk_event_box_new();
  gtk_widget_add_events(labelev, GDK_BUTTON_PRESS_MASK);
  gtk_container_add(GTK_CONTAINER(labelev), label);
  gtk_grid_attach(GTK_GRID(grid), labelev, 0, line++, 1, 1);
  gtk_grid_attach_next_to(GTK_GRID(grid), widget, labelev, GTK_POS_RIGHT, 1, 1);

  // read all themes
  char *theme_name = dt_conf_get_string("ui_last/theme");
  int selected = 0;
  int k = 0;
  for(GList *iter = darktable.themes; iter; iter = g_list_next(iter))
  {
    gchar *name = g_strdup((gchar*)(iter->data));
    // remove extension
    gchar *i = g_strrstr(name, ".");
    if(i) *i = '\0';
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(widget), name);
    if(!g_strcmp0(name, theme_name)) selected = k;
    k++;
  }
  g_free(theme_name);

  gtk_combo_box_set_active(GTK_COMBO_BOX(widget), selected);

  g_signal_connect(G_OBJECT(widget), "changed", G_CALLBACK(theme_callback), 0);
  gtk_widget_set_tooltip_text(widget, _("set the theme for the user interface"));

  //Font size check and spin buttons
  GtkWidget *usesysfont = gtk_check_button_new();
  GtkWidget *fontsize = gtk_spin_button_new_with_range(5.0f, 30.0f, 0.2f);
  int i = dt_conf_get_bool("font_prefs_align_right") ? gtk_widget_set_hexpand(fontsize, TRUE), 2 : 0;

  //checkbox to use system font size
  if(dt_conf_get_bool("use_system_font"))
    gtk_widget_set_state_flags(fontsize, GTK_STATE_FLAG_INSENSITIVE, TRUE);
  else
    gtk_widget_set_state_flags(fontsize, GTK_STATE_FLAG_NORMAL, TRUE);

  label = gtk_label_new(_("use system font size"));
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  labelev = gtk_event_box_new();
  gtk_widget_add_events(labelev, GDK_BUTTON_PRESS_MASK);
  gtk_container_add(GTK_CONTAINER(labelev), label);
  gtk_grid_attach(GTK_GRID(grid), labelev, i, i?2:line++, 1, 1);
  gtk_grid_attach_next_to(GTK_GRID(grid), usesysfont, labelev, GTK_POS_RIGHT, 1, 1);
  gtk_widget_set_tooltip_text(usesysfont, _("use system font size"));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(usesysfont), dt_conf_get_bool("use_system_font"));
  g_signal_connect(G_OBJECT(usesysfont), "toggled", G_CALLBACK(use_sys_font_callback), (gpointer)fontsize);


  //font size selector
  if(dt_conf_get_float("font_size") < 5.0f || dt_conf_get_float("font_size") > 20.0f)
    dt_conf_set_float("font_size", 12.0f);

  label = gtk_label_new(_("font size in points"));
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  labelev = gtk_event_box_new();
  gtk_widget_add_events(labelev, GDK_BUTTON_PRESS_MASK);
  gtk_container_add(GTK_CONTAINER(labelev), label);
  gtk_grid_attach(GTK_GRID(grid), labelev, i, i?0:line++, 1, 1);
  gtk_grid_attach_next_to(GTK_GRID(grid), fontsize, labelev, GTK_POS_RIGHT, 1, 1);
  gtk_widget_set_tooltip_text(fontsize, _("font size in points"));
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(fontsize), dt_conf_get_float("font_size"));
  g_signal_connect(G_OBJECT(fontsize), "value_changed", G_CALLBACK(font_size_changed_callback), 0);

  GtkWidget *screen_dpi_overwrite = gtk_spin_button_new_with_range(-1.0f, 360, 1.f);
  label = gtk_label_new(_("GUI controls and text DPI"));
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  labelev = gtk_event_box_new();
  gtk_widget_add_events(labelev, GDK_BUTTON_PRESS_MASK);
  gtk_container_add(GTK_CONTAINER(labelev), label);
  gtk_grid_attach(GTK_GRID(grid), labelev, i, i?1:line++, 1, 1);
  gtk_grid_attach_next_to(GTK_GRID(grid), screen_dpi_overwrite, labelev, GTK_POS_RIGHT, 1, 1);
  gtk_widget_set_tooltip_text(screen_dpi_overwrite, _("adjust the global GUI resolution to rescale controls, buttons, labels, etc.\n"
                                                      "increase for a magnified GUI, decrease to fit more content in window.\n"
                                                      "set to -1 to use the system-defined global resolution.\n"
                                                      "default is 96 DPI on most systems.\n"
                                                      "(needs a restart)."));
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(screen_dpi_overwrite), dt_conf_get_float("screen_dpi_overwrite"));
  g_signal_connect(G_OBJECT(screen_dpi_overwrite), "value_changed", G_CALLBACK(dpi_scaling_changed_callback), 0);

  //checkbox to allow user to modify theme with user.css
  label = gtk_label_new(_("modify selected theme with CSS tweaks below"));
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  tw->apply_toggle = gtk_check_button_new();
  labelev = gtk_event_box_new();
  gtk_widget_add_events(labelev, GDK_BUTTON_PRESS_MASK);
  gtk_container_add(GTK_CONTAINER(labelev), label);
  gtk_grid_attach(GTK_GRID(grid), labelev, 0, line++, 1, 1);
  gtk_grid_attach_next_to(GTK_GRID(grid), tw->apply_toggle, labelev, GTK_POS_RIGHT, 1, 1);
  gtk_widget_set_tooltip_text(tw->apply_toggle, _("modify theme with CSS keyed below (saved to user.css)"));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(tw->apply_toggle), dt_conf_get_bool("themes/usercss"));
  g_signal_connect(G_OBJECT(tw->apply_toggle), "toggled", G_CALLBACK(usercss_callback), 0);

  //scrollable textarea with save button to allow user to directly modify user.css file
  GtkWidget *usercssbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_pack_start(GTK_BOX(container), usercssbox, TRUE, TRUE, 0);
  gtk_widget_set_name(usercssbox, "usercss-box");

  GtkTextBuffer *buffer = gtk_text_buffer_new(NULL);
  tw->css_text_view= gtk_text_view_new_with_buffer(buffer);
  gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(tw->css_text_view), GTK_WRAP_WORD);
  dt_gui_add_class(tw->css_text_view, "dt_monospace");
  gtk_widget_set_hexpand(tw->css_text_view, TRUE);
  gtk_widget_set_halign(tw->css_text_view, GTK_ALIGN_FILL);

  GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_container_add(GTK_CONTAINER(scroll), tw->css_text_view);
  gtk_box_pack_start(GTK_BOX(usercssbox), scroll, TRUE, TRUE, 0);

  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *button = gtk_button_new_with_label(_("help"));
  gtk_widget_set_tooltip_text(button, _("open help page for CSS tweaks"));
  dt_gui_add_help_link(button, "css_tweaks");
  g_signal_connect(button, "clicked", G_CALLBACK(dt_gui_show_help), NULL);
  gtk_box_pack_end(GTK_BOX(hbox), button, FALSE, FALSE, 0);
  tw->save_button = gtk_button_new_with_label(C_("usercss", "save CSS and apply"));
  g_signal_connect(G_OBJECT(tw->save_button), "clicked", G_CALLBACK(save_usercss_callback), tw);
  g_signal_connect(G_OBJECT(dialog), "response", G_CALLBACK(usercss_dialog_callback), tw);
  gtk_box_pack_end(GTK_BOX(hbox), tw->save_button, FALSE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(usercssbox), hbox, FALSE, FALSE, 0);
  gtk_widget_set_tooltip_text(tw->save_button, _("click to save and apply the CSS tweaks entered in this editor"));

  //set textarea text from file or default
  char usercsspath[PATH_MAX] = { 0 }, configdir[PATH_MAX] = { 0 };
  dt_loc_get_user_config_dir(configdir, sizeof(configdir));
  g_snprintf(usercsspath, sizeof(usercsspath), "%s/user.css", configdir);

  if(g_file_test(usercsspath, G_FILE_TEST_EXISTS))
  {
    gchar *usercsscontent = NULL;
    //load file into buffer
    if(g_file_get_contents(usercsspath, &usercsscontent, NULL, NULL))
    {
      gtk_text_buffer_set_text(buffer, usercsscontent, -1);
    }
    else
    {
      //load default text with some pointers
      gchar *errtext = g_strconcat("/* ", _("ERROR Loading user.css"), " */", NULL);
      gtk_text_buffer_set_text(buffer, errtext, -1);
      g_free(errtext);
    }
    g_free(usercsscontent);
  }
  else
  {
    //load default text
    gchar *deftext = g_strconcat("/* ", _("Enter CSS theme tweaks here"), " */\n\n", NULL);
    gtk_text_buffer_set_text(buffer, deftext, -1);
    g_free(deftext);
  }

}

///////////// end of gui and theme language selection

#if 0
// FIXME! this makes some systems hang forever. I don't reproduce.
gboolean preferences_window_deleted(GtkWidget *widget, GdkEvent *event, gpointer data)
{
  // redraw the whole UI in case sizes have changed
  gtk_widget_queue_resize(dt_ui_center(darktable.gui->ui));
  gtk_widget_queue_resize(dt_ui_main_window(darktable.gui->ui));

  gtk_widget_queue_draw(dt_ui_main_window(darktable.gui->ui));
  gtk_widget_queue_draw(dt_ui_center(darktable.gui->ui));

  gtk_widget_hide(widget);
  return TRUE;
}
#endif

static void _resize_dialog(GtkWidget *widget)
{
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  dt_conf_set_int("ui_last/preferences_dialog_width", allocation.width);
  dt_conf_set_int("ui_last/preferences_dialog_height", allocation.height);
}

void dt_gui_preferences_show()
{
  GtkWindow *win = GTK_WINDOW(dt_ui_main_window(darktable.gui->ui));
  _preferences_dialog = gtk_dialog_new_with_buttons(_("darktable preferences"), win,
                                                    GTK_DIALOG_DESTROY_WITH_PARENT | GTK_DIALOG_MODAL,
                                                    NULL, NULL);
#if 0
  // FIXME! this makes some systems hang forever. I don't reproduce.
  g_signal_connect(G_OBJECT(_preferences_dialog), "delete-event", G_CALLBACK(preferences_window_deleted), NULL);
#endif

  gtk_window_set_default_size(GTK_WINDOW(_preferences_dialog),
                              dt_conf_get_int("ui_last/preferences_dialog_width"),
                              dt_conf_get_int("ui_last/preferences_dialog_height"));
  g_signal_connect(G_OBJECT(_preferences_dialog), "check-resize", G_CALLBACK(_resize_dialog), NULL);
#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(_preferences_dialog);
#endif
  gtk_window_set_position(GTK_WINDOW(_preferences_dialog), GTK_WIN_POS_CENTER_ON_PARENT);
  gtk_widget_set_name(_preferences_dialog, "preferences-notebook");

  //grab the content area of the dialog
  GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(_preferences_dialog));
  gtk_widget_set_name(content, "preferences-content");
  gtk_container_set_border_width(GTK_CONTAINER(content), 0);

  //place a box in the content area
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name(box, "preferences-box");
  gtk_container_set_border_width(GTK_CONTAINER(box), 0);
  gtk_box_pack_start(GTK_BOX(content), box, TRUE, TRUE, 0);

  //create stack and sidebar and pack into the box
  GtkWidget *stack = gtk_stack_new();
  GtkWidget *stacksidebar = gtk_stack_sidebar_new();
  gtk_stack_sidebar_set_stack(GTK_STACK_SIDEBAR(stacksidebar), GTK_STACK(stack));
  gtk_box_pack_start(GTK_BOX(box), stacksidebar, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(box), stack, TRUE, TRUE, 0);

  dt_gui_themetweak_widgets_t *tweak_widgets = (dt_gui_themetweak_widgets_t *)malloc(sizeof(dt_gui_themetweak_widgets_t));

  restart_required = FALSE;

  //setup tabs
  init_tab_general(_preferences_dialog, stack, tweak_widgets);
  init_tab_import(_preferences_dialog, stack);
  init_tab_lighttable(_preferences_dialog, stack);
  init_tab_darkroom(_preferences_dialog, stack);
  init_tab_processing(_preferences_dialog, stack);
  init_tab_security(_preferences_dialog, stack);
  init_tab_storage(_preferences_dialog, stack);
  init_tab_misc(_preferences_dialog, stack);
  init_tab_accels(stack);
  init_tab_presets(stack);

  //open in the appropriate tab if currently in darkroom or lighttable view
  const gchar *current_view = darktable.view_manager->current_view->name(darktable.view_manager->current_view);
  if(strcmp(current_view, _("darkroom")) == 0 || strcmp(current_view, _("lighttable")) == 0)
  {
    gtk_stack_set_visible_child(GTK_STACK(stack), gtk_stack_get_child_by_name(GTK_STACK(stack), current_view));
  }

#ifdef USE_LUA
  GtkGrid* lua_grid = init_tab_lua(_preferences_dialog, stack);
#endif
  gtk_widget_show_all(_preferences_dialog);
  (void)gtk_dialog_run(GTK_DIALOG(_preferences_dialog));

#ifdef USE_LUA
  destroy_tab_lua(lua_grid);
#endif

  free(tweak_widgets);
  gtk_widget_destroy(_preferences_dialog);

  if(restart_required)
    dt_control_log(_("darktable needs to be restarted for settings to take effect"));

  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_PREFERENCES_CHANGE);
}

static void cairo_destroy_from_pixbuf(guchar *pixels, gpointer data)
{
  cairo_destroy((cairo_t *)data);
}

static void _create_lock_check_pixbuf(GdkPixbuf **lock_pixbuf, GdkPixbuf **check_pixbuf)
{
  // Create GdkPixbufs with cairo drawings.

  // lock
  cairo_surface_t *lock_cst = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, DT_PIXEL_APPLY_DPI(ICON_SIZE),
                                                         DT_PIXEL_APPLY_DPI(ICON_SIZE));
  cairo_t *lock_cr = cairo_create(lock_cst);
  cairo_set_source_rgb(lock_cr, 0.7, 0.7, 0.7);
  dtgtk_cairo_paint_lock(lock_cr, 0, 0, DT_PIXEL_APPLY_DPI(ICON_SIZE), DT_PIXEL_APPLY_DPI(ICON_SIZE), 0, NULL);
  cairo_surface_flush(lock_cst);
  guchar *data = cairo_image_surface_get_data(lock_cst);
  dt_draw_cairo_to_gdk_pixbuf(data, DT_PIXEL_APPLY_DPI(ICON_SIZE), DT_PIXEL_APPLY_DPI(ICON_SIZE));
  *lock_pixbuf = gdk_pixbuf_new_from_data(data, GDK_COLORSPACE_RGB, TRUE, 8,
                                          DT_PIXEL_APPLY_DPI(ICON_SIZE), DT_PIXEL_APPLY_DPI(ICON_SIZE),
                                          cairo_image_surface_get_stride(lock_cst),
                                          cairo_destroy_from_pixbuf, lock_cr);
  cairo_surface_destroy(lock_cst);

  // check mark
  cairo_surface_t *check_cst = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, DT_PIXEL_APPLY_DPI(ICON_SIZE),
                                                          DT_PIXEL_APPLY_DPI(ICON_SIZE));
  cairo_t *check_cr = cairo_create(check_cst);
  cairo_set_source_rgb(check_cr, 0.7, 0.7, 0.7);
  dtgtk_cairo_paint_check_mark(check_cr, 0, 0, DT_PIXEL_APPLY_DPI(ICON_SIZE), DT_PIXEL_APPLY_DPI(ICON_SIZE), 0, NULL);
  cairo_surface_flush(check_cst);
  data = cairo_image_surface_get_data(check_cst);
  dt_draw_cairo_to_gdk_pixbuf(data, DT_PIXEL_APPLY_DPI(ICON_SIZE), DT_PIXEL_APPLY_DPI(ICON_SIZE));
  *check_pixbuf = gdk_pixbuf_new_from_data(data, GDK_COLORSPACE_RGB, TRUE, 8,
                                           DT_PIXEL_APPLY_DPI(ICON_SIZE), DT_PIXEL_APPLY_DPI(ICON_SIZE),
                                           cairo_image_surface_get_stride(check_cst),
                                           cairo_destroy_from_pixbuf, check_cr);
  cairo_surface_destroy(check_cst);
}

static void _update_preset_line(GtkTreeStore *tree_store, GtkTreeIter *iter, sqlite3_stmt *stmt,
                                GdkPixbuf *lock_pixbuf, GdkPixbuf *check_pixbuf)
{
  const gint rowid = sqlite3_column_int(stmt, 0);
  const gchar *name = (gchar *)sqlite3_column_text(stmt, 1);
  const gchar *operation = (gchar *)sqlite3_column_text(stmt, 2);
  const gboolean autoapply = (sqlite3_column_int(stmt, 3) == 0 ? FALSE : TRUE);
  const gchar *model = (gchar *)sqlite3_column_text(stmt, 4);
  const gchar *maker = (gchar *)sqlite3_column_text(stmt, 5);
  const gchar *lens = (gchar *)sqlite3_column_text(stmt, 6);
  const float iso_min = sqlite3_column_double(stmt, 7);
  const float iso_max = sqlite3_column_double(stmt, 8);
  const float exposure_min = sqlite3_column_double(stmt, 9);
  const float exposure_max = sqlite3_column_double(stmt, 10);
  const float aperture_min = sqlite3_column_double(stmt, 11);
  const float aperture_max = sqlite3_column_double(stmt, 12);
  const int focal_length_min = sqlite3_column_double(stmt, 13);
  const int focal_length_max = sqlite3_column_double(stmt, 14);
  const gboolean writeprotect = (sqlite3_column_int(stmt, 15) == 0 ? FALSE : TRUE);

  gchar *iso = NULL, *exposure = NULL, *aperture = NULL, *focal_length = NULL, *smaker = NULL, *smodel = NULL, *slens = NULL;
  int min, max;

  gchar *module = g_strdup(dt_iop_get_localized_name(operation));
  if(module == NULL) module = g_strdup(dt_lib_get_localized_name(operation));
  if(module == NULL) module = g_strdup(operation);

  if(!dt_presets_module_can_autoapply(operation))
  {
    iso = g_strdup("");
    exposure = g_strdup("");
    aperture = g_strdup("");
    focal_length = g_strdup("");
    smaker = g_strdup("");
    smodel = g_strdup("");
    slens = g_strdup("");
  }
  else
  {
    smaker = g_strdup(maker);
    smodel = g_strdup(model);
    slens = g_strdup(lens);

    if(iso_min == 0.0 && iso_max == FLT_MAX)
      iso = g_strdup("%");
    else
      iso = g_strdup_printf("%zu – %zu", (size_t)iso_min, (size_t)iso_max);

    for(min = 0; min < dt_gui_presets_exposure_value_cnt && exposure_min > dt_gui_presets_exposure_value[min]; min++)
      ;
    for(max = 0; max < dt_gui_presets_exposure_value_cnt && exposure_max > dt_gui_presets_exposure_value[max]; max++)
      ;
    if(min == 0 && max == dt_gui_presets_exposure_value_cnt - 1)
      exposure = g_strdup("%");
    else
      exposure = g_strdup_printf("%s – %s", dt_gui_presets_exposure_value_str[min],
                                 dt_gui_presets_exposure_value_str[max]);

    for(min = 0; min < dt_gui_presets_aperture_value_cnt && aperture_min > dt_gui_presets_aperture_value[min]; min++)
      ;
    for(max = 0; max < dt_gui_presets_aperture_value_cnt && aperture_max > dt_gui_presets_aperture_value[max]; max++)
      ;
    if(min == 0 && max == dt_gui_presets_aperture_value_cnt - 1)
      aperture = g_strdup("%");
    else
      aperture = g_strdup_printf("%s – %s", dt_gui_presets_aperture_value_str[min],
                                 dt_gui_presets_aperture_value_str[max]);

    if(focal_length_min == 0.0f && focal_length_max == 1000.0f)
      focal_length = g_strdup("%");
    else
      focal_length = g_strdup_printf("%d – %d", focal_length_min, focal_length_max);
  }

  gtk_tree_store_set(GTK_TREE_STORE(tree_store), iter,
                      P_ROWID_COLUMN, rowid, P_OPERATION_COLUMN, operation,
                      P_MODULE_COLUMN, "", P_EDITABLE_COLUMN, writeprotect ? lock_pixbuf : NULL,
                      P_NAME_COLUMN, name, P_MODEL_COLUMN, smodel, P_MAKER_COLUMN, smaker, P_LENS_COLUMN, slens,
                      P_ISO_COLUMN, iso, P_EXPOSURE_COLUMN, exposure, P_APERTURE_COLUMN, aperture,
                      P_FOCAL_LENGTH_COLUMN, focal_length, P_AUTOAPPLY_COLUMN,
                      autoapply ? check_pixbuf : NULL, -1);

  g_free(focal_length);
  g_free(aperture);
  g_free(exposure);
  g_free(iso);
  g_free(module);
  g_free(smaker);
  g_free(smodel);
  g_free(slens);
}

static void tree_insert_presets(GtkTreeStore *tree_store)
{
  GtkTreeIter iter, parent;
  sqlite3_stmt *stmt;
  gchar *last_module = NULL;

  GdkPixbuf *lock_pixbuf, *check_pixbuf;
  _create_lock_check_pixbuf(&lock_pixbuf, &check_pixbuf);

  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT rowid, name, operation, autoapply, model, maker, lens, iso_min, "
                              "iso_max, exposure_min, exposure_max, aperture_min, aperture_max, "
                              "focal_length_min, focal_length_max, writeprotect "
                              "FROM data.presets "
                              "ORDER BY operation, name",
                              -1, &stmt, NULL);
  // clang-format on
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const gchar *operation = (gchar *)sqlite3_column_text(stmt, 2);
    if(g_strcmp0(operation, last_module))
    {
      gchar *module = g_strdup(dt_iop_get_localized_name(operation));
      if(module == NULL) module = g_strdup(dt_lib_get_localized_name(operation));
      if(module == NULL) module = g_strdup(operation);

      gtk_tree_store_insert_with_values(tree_store, &parent, NULL, -1,
                                        P_MODULE_COLUMN, module, -1);

      g_free(module);
      g_free(last_module);
      last_module = g_strdup(operation);
    }

    gtk_tree_store_insert(tree_store, &iter, &parent, -1);

    _update_preset_line(tree_store, &iter, stmt, lock_pixbuf, check_pixbuf);
  }
  g_free(last_module);
  sqlite3_finalize(stmt);

  g_object_unref(lock_pixbuf);
  g_object_unref(check_pixbuf);
}

static gboolean _search_func(GtkTreeModel *model, gint column, const gchar *key, GtkTreeIter *iter, gpointer search_data)
{
  gchar *key_case = g_utf8_casefold(key, -1), *label = NULL;

  gtk_tree_model_get(model, iter, P_NAME_COLUMN, &label, -1);
  gchar *name_case = g_utf8_casefold(label, -1);
  g_free(label);
  gtk_tree_model_get(model, iter, P_MODULE_COLUMN, &label, -1);
  gchar *module_case = g_utf8_casefold(label, -1);
  g_free(label);

  const gboolean different = !((name_case && strstr(name_case, key_case))
                               || (module_case && strstr(module_case, key_case)));

  g_free(name_case);
  g_free(module_case);
  g_free(key_case);

  if(!different)
  {
    GtkTreePath *path = gtk_tree_model_get_path(model, iter);
    gtk_tree_view_expand_to_path(GTK_TREE_VIEW(search_data), path);
    gtk_tree_path_free(path);

    return FALSE;
  }

  GtkTreeIter child;
  if(gtk_tree_model_iter_children(model, &child, iter))
  {
    do
    {
      _search_func(model, column, key, &child, search_data);
    }
    while(gtk_tree_model_iter_next(model, &child));
  }

  return TRUE;
}

static void init_tab_presets(GtkWidget *stack)
{
  GtkWidget *container = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
  GtkTreeView *tree = GTK_TREE_VIEW(gtk_tree_view_new());
  GtkTreeStore *model = gtk_tree_store_new(
      P_N_COLUMNS, G_TYPE_INT /*rowid*/, G_TYPE_STRING /*operation*/, G_TYPE_STRING /*module*/,
      GDK_TYPE_PIXBUF /*editable*/, G_TYPE_STRING /*name*/, G_TYPE_STRING /*model*/, G_TYPE_STRING /*maker*/,
      G_TYPE_STRING /*lens*/, G_TYPE_STRING /*iso*/, G_TYPE_STRING /*exposure*/, G_TYPE_STRING /*aperture*/,
      G_TYPE_STRING /*focal length*/, GDK_TYPE_PIXBUF /*auto*/);
  GtkCellRenderer *renderer;
  GtkTreeViewColumn *column;

  // Adding the outer container
  gtk_stack_add_titled(GTK_STACK(stack), container, _("presets"), _("presets"));

  tree_insert_presets(model);

  // Setting a custom sort functions so expandable groups rise to the top
  gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(model), P_MODULE_COLUMN, GTK_SORT_ASCENDING);
  gtk_tree_sortable_set_sort_func(GTK_TREE_SORTABLE(model), P_MODULE_COLUMN, compare_rows_presets, NULL, NULL);

  // Setting up the cell renderers
  renderer = gtk_cell_renderer_text_new();
  column = gtk_tree_view_column_new_with_attributes(_("module"), renderer, "text", P_MODULE_COLUMN, NULL);
  gtk_tree_view_append_column(tree, column);

  renderer = gtk_cell_renderer_pixbuf_new();
  column = gtk_tree_view_column_new_with_attributes("", renderer, "pixbuf", P_EDITABLE_COLUMN, NULL);
  gtk_tree_view_append_column(tree, column);

  renderer = gtk_cell_renderer_text_new();
  column = gtk_tree_view_column_new_with_attributes(_("name"), renderer, "text", P_NAME_COLUMN, NULL);
  gtk_tree_view_append_column(tree, column);

  renderer = gtk_cell_renderer_text_new();
  column = gtk_tree_view_column_new_with_attributes(_("model"), renderer, "text", P_MODEL_COLUMN, NULL);
  gtk_tree_view_append_column(tree, column);

  renderer = gtk_cell_renderer_text_new();
  column = gtk_tree_view_column_new_with_attributes(_("maker"), renderer, "text", P_MAKER_COLUMN, NULL);
  gtk_tree_view_append_column(tree, column);

  renderer = gtk_cell_renderer_text_new();
  column = gtk_tree_view_column_new_with_attributes(_("lens"), renderer, "text", P_LENS_COLUMN, NULL);
  gtk_tree_view_append_column(tree, column);

  renderer = gtk_cell_renderer_text_new();
  column = gtk_tree_view_column_new_with_attributes(_("ISO"), renderer, "text", P_ISO_COLUMN, NULL);
  gtk_tree_view_append_column(tree, column);

  renderer = gtk_cell_renderer_text_new();
  column = gtk_tree_view_column_new_with_attributes(_("exposure"), renderer, "text", P_EXPOSURE_COLUMN, NULL);
  gtk_tree_view_append_column(tree, column);

  renderer = gtk_cell_renderer_text_new();
  column = gtk_tree_view_column_new_with_attributes(_("aperture"), renderer, "text", P_APERTURE_COLUMN, NULL);
  gtk_tree_view_append_column(tree, column);

  renderer = gtk_cell_renderer_text_new();
  column = gtk_tree_view_column_new_with_attributes(_("focal length"), renderer, "text",
                                                    P_FOCAL_LENGTH_COLUMN, NULL);
  gtk_tree_view_append_column(tree, column);

  renderer = gtk_cell_renderer_pixbuf_new();
  column = gtk_tree_view_column_new_with_attributes(_("auto"), renderer, "pixbuf", P_AUTOAPPLY_COLUMN, NULL);
  gtk_tree_view_append_column(tree, column);

  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_box_pack_start(GTK_BOX(container), scroll, TRUE, TRUE, 0);

  // Adding the import/export buttons
  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name(hbox, "preset-controls");

  GtkWidget *search_presets = gtk_search_entry_new();
  gtk_box_pack_start(GTK_BOX(hbox), search_presets, FALSE, TRUE, 0);
  gtk_entry_set_placeholder_text(GTK_ENTRY(search_presets), _("search presets list"));
  gtk_widget_set_tooltip_text(GTK_WIDGET(search_presets), _("incrementally search the list of presets\npress up or down keys to cycle through matches"));
  g_signal_connect(G_OBJECT(search_presets), "activate", G_CALLBACK(dt_gui_search_stop), tree);
  g_signal_connect(G_OBJECT(search_presets), "stop-search", G_CALLBACK(dt_gui_search_stop), tree);
  g_signal_connect(G_OBJECT(tree), "key-press-event", G_CALLBACK(dt_gui_search_start), search_presets);
  gtk_tree_view_set_search_entry(tree, GTK_ENTRY(search_presets));

  GtkWidget *button = gtk_button_new_with_label(_("help"));
  dt_gui_add_help_link(button, "presets");
  g_signal_connect(button, "clicked", G_CALLBACK(dt_gui_show_help), NULL);
  gtk_box_pack_end(GTK_BOX(hbox), button, FALSE, FALSE, 0);

  button = gtk_button_new_with_label(C_("preferences", "import..."));
  gtk_box_pack_end(GTK_BOX(hbox), button, FALSE, TRUE, 0);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(import_preset), (gpointer)model);

  button = gtk_button_new_with_label(C_("preferences", "export..."));
  gtk_box_pack_end(GTK_BOX(hbox), button, FALSE, TRUE, 0);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(export_preset), (gpointer)model);

  gtk_box_pack_start(GTK_BOX(container), hbox, FALSE, FALSE, 0);

  // Attaching treeview signals

  // row-activated either expands/collapses a row or activates editing
  g_signal_connect(G_OBJECT(tree), "row-activated", G_CALLBACK(tree_row_activated_presets), NULL);

  // A keypress may delete preset
  g_signal_connect(G_OBJECT(tree), "key-press-event", G_CALLBACK(tree_key_press_presets), (gpointer)model);

  // Setting up the search functionality
  gtk_tree_view_set_search_equal_func(tree, _search_func, tree, NULL);

  // Attaching the model to the treeview
  gtk_tree_view_set_model(tree, GTK_TREE_MODEL(model));

  // Adding the treeview to its containers
  gtk_container_add(GTK_CONTAINER(scroll), GTK_WIDGET(tree));
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);

  g_object_unref(G_OBJECT(model));
}

static void init_tab_accels(GtkWidget *stack)
{
  gtk_stack_add_titled(GTK_STACK(stack), dt_shortcuts_prefs(NULL), _("shortcuts"), _("shortcuts"));
}

static void _delete_line_and_empty_parent(GtkTreeModel *model, GtkTreeIter *iter)
{
  GtkTreeIter parent;
  {
    gtk_tree_model_iter_parent(model, &parent, iter);
    gtk_tree_store_remove(GTK_TREE_STORE(model), iter);
    if(!gtk_tree_model_iter_has_child(model, &parent))
      gtk_tree_store_remove(GTK_TREE_STORE(model), &parent);
  }
}

static GtkTreeIter edited_iter;

static void edit_preset_response(dt_gui_presets_edit_dialog_t *g)
{
  if(!g->old_id)
    _delete_line_and_empty_parent(g->data, &edited_iter);
  else
  {
    GdkPixbuf *lock_pixbuf, *check_pixbuf;
    _create_lock_check_pixbuf(&lock_pixbuf, &check_pixbuf);

    sqlite3_stmt *stmt;

    // clang-format off
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "SELECT rowid, name, operation, autoapply, model, maker, lens, iso_min, "
                                "iso_max, exposure_min, exposure_max, aperture_min, aperture_max, "
                                "focal_length_min, focal_length_max, writeprotect "
                                "FROM data.presets "
                                "WHERE rowid = ?1",
                                -1, &stmt, NULL);
    // clang-format on
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, g->old_id);

    if(sqlite3_step(stmt) == SQLITE_ROW)
      _update_preset_line(g->data, &edited_iter, stmt, lock_pixbuf, check_pixbuf);

    sqlite3_finalize(stmt);
  }
}

static void tree_row_activated_presets(GtkTreeView *tree, GtkTreePath *path, GtkTreeViewColumn *column,
                                       gpointer data)
{
  GtkTreeModel *model = gtk_tree_view_get_model(tree);

  gtk_tree_model_get_iter(model, &edited_iter, path);

  if(gtk_tree_model_iter_has_child(model, &edited_iter))
  {
    // For branch nodes, toggle expansion on activation
    if(gtk_tree_view_row_expanded(tree, path))
      gtk_tree_view_collapse_row(tree, path);
    else
      gtk_tree_view_expand_row(tree, path, FALSE);
  }
  else
  {
    // For leaf nodes, open editing window if the preset is not writeprotected
    gint rowid;
    gchar *name, *operation;
    GdkPixbuf *editable;
    gtk_tree_model_get(model, &edited_iter, P_ROWID_COLUMN, &rowid, P_NAME_COLUMN, &name, P_OPERATION_COLUMN,
                       &operation, P_EDITABLE_COLUMN, &editable, -1);
    if(editable == NULL)
      dt_gui_presets_show_edit_dialog(name, operation, rowid, G_CALLBACK(edit_preset_response), model, TRUE, TRUE, TRUE,
                                      GTK_WINDOW(_preferences_dialog));
    else
      g_object_unref(editable);
    g_free(name);
    g_free(operation);
  }
}

static gboolean tree_key_press_presets(GtkWidget *widget, GdkEventKey *event, gpointer data)
{
  GtkTreeModel *model = (GtkTreeModel *)data;
  GtkTreeIter iter;
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(widget));

  // We can just ignore mod key presses outright
  if(event->is_modifier) return FALSE;

  if(event->keyval == GDK_KEY_Delete || event->keyval == GDK_KEY_BackSpace)
  {
    // If a leaf node is selected, delete that preset

    // If nothing is selected, or branch node selected, just return
    if(!gtk_tree_selection_get_selected(selection, &model, &iter)
       || gtk_tree_model_iter_has_child(model, &iter))
      return FALSE;

    // For leaf nodes, open delete confirmation window if the preset is not writeprotected
    gint rowid;
    gchar *name, *operation;
    GdkPixbuf *editable;
    gtk_tree_model_get(model, &iter, P_ROWID_COLUMN, &rowid, P_NAME_COLUMN, &name,
                       P_MODULE_COLUMN, &operation, P_EDITABLE_COLUMN, &editable, -1);
    if(editable == NULL)
    {
      if(dt_gui_presets_confirm_and_delete(name, operation, rowid))
        _delete_line_and_empty_parent(model, &iter);
    }
    else
      g_object_unref(editable);

    g_free(name);
    g_free(operation);

    return TRUE;
  }
  else
  {
    return FALSE;
  }
}

static void _import_preset_from_file(const gchar* filename)
{
  if(!dt_presets_import_from_file(filename))
  {
    dt_control_log(_("failed to import preset %s"), filename);
  }
}

static void import_preset(GtkButton *button, gpointer data)
{
  GtkTreeModel *model = (GtkTreeModel *)data;
  GtkWindow *win = GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(button)));

  // Zero value indicates import
  GtkFileChooserNative *chooser = gtk_file_chooser_native_new(
        _("select preset(s) to import"), GTK_WINDOW(win), GTK_FILE_CHOOSER_ACTION_OPEN,
        _("_open"), _("_cancel"));

  dt_conf_get_folder_to_file_chooser("ui_last/import_path", GTK_FILE_CHOOSER(chooser));
  gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(chooser), TRUE);

  GtkFileFilter *filter;
  filter = GTK_FILE_FILTER(gtk_file_filter_new());
  gtk_file_filter_add_pattern(filter, "*.dtpreset");
  gtk_file_filter_add_pattern(filter, "*.DTPRESET");
  gtk_file_filter_set_name(filter, _("darktable preset files"));
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(chooser), filter);

  filter = GTK_FILE_FILTER(gtk_file_filter_new());
  gtk_file_filter_add_pattern(filter, "*");
  gtk_file_filter_set_name(filter, _("all files"));

  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(chooser), filter);

  if(gtk_native_dialog_run(GTK_NATIVE_DIALOG(chooser)) == GTK_RESPONSE_ACCEPT)
  {
    GSList *filenames = gtk_file_chooser_get_filenames(GTK_FILE_CHOOSER(chooser));
    g_slist_foreach(filenames, (GFunc)_import_preset_from_file, NULL);
    g_slist_free_full(filenames, g_free);

    GtkTreeStore *tree_store = GTK_TREE_STORE(model);
    gtk_tree_store_clear(tree_store);
    tree_insert_presets(tree_store);

    dt_conf_set_folder_from_file_chooser("ui_last/import_path", GTK_FILE_CHOOSER(chooser));
  }
  g_object_unref(chooser);
}

static void export_preset(GtkButton *button, gpointer data)
{
  GtkWindow *win = GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(button)));

  GtkFileChooserNative *filechooser = gtk_file_chooser_native_new(
        _("select directory"), GTK_WINDOW(win), GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
        _("_save"), _("_cancel"));

  dt_conf_get_folder_to_file_chooser("ui_last/export_path", GTK_FILE_CHOOSER(filechooser));

  if(gtk_native_dialog_run(GTK_NATIVE_DIALOG(filechooser)) == GTK_RESPONSE_ACCEPT)
  {
    gchar *filedir = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(filechooser));
    sqlite3_stmt *stmt;

    // we have n+1 selects for saving presets, using single transaction for whole process saves us microlocks
    dt_database_start_transaction(darktable.db);

    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "SELECT rowid, name, operation FROM data.presets WHERE writeprotect = 0",
                                -1, &stmt, NULL);

    while(sqlite3_step(stmt) == SQLITE_ROW)
    {
      const gint rowid = sqlite3_column_int(stmt, 0);
      const gchar *name = (gchar *)sqlite3_column_text(stmt, 1);
      const gchar *operation = (gchar *)sqlite3_column_text(stmt, 2);
      gchar* preset_name = g_strdup_printf("%s_%s", operation, name);

      dt_presets_save_to_file(rowid, preset_name, filedir);

      g_free(preset_name);
    }

    sqlite3_finalize(stmt);

    dt_database_release_transaction(darktable.db);

    dt_conf_set_folder_from_file_chooser("ui_last/export_path", GTK_FILE_CHOOSER(filechooser));

    g_free(filedir);
  }
  g_object_unref(filechooser);
}

// Custom sort function for TreeModel entries for presets list
static gint compare_rows_presets(GtkTreeModel *model, GtkTreeIter *a, GtkTreeIter *b, gpointer data)
{
  gchar *a_text;
  gchar *b_text;

  gtk_tree_model_get(model, a, P_MODULE_COLUMN, &a_text, -1);
  gtk_tree_model_get(model, b, P_MODULE_COLUMN, &b_text, -1);
  if(*a_text == '\0' && *b_text == '\0')
  {
    g_free(a_text);
    g_free(b_text);

    gtk_tree_model_get(model, a, P_NAME_COLUMN, &a_text, -1);
    gtk_tree_model_get(model, b, P_NAME_COLUMN, &b_text, -1);
  }

  const int res = strcoll(a_text, b_text);

  g_free(a_text);
  g_free(b_text);

  return res;
}

static void
_gui_preferences_bool_callback(GtkWidget *widget, gpointer data)
{
  dt_conf_set_bool((char *)data, gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget)));
}

void dt_gui_preferences_bool_reset(GtkWidget *widget)
{
  const char *key = gtk_widget_get_name(widget);
  const gboolean def = dt_confgen_get_bool(key, DT_DEFAULT);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widget), def);
}

static gboolean
_gui_preferences_bool_reset(GtkWidget *label, GdkEventButton *event, GtkWidget *widget)
{
  if(event->type == GDK_2BUTTON_PRESS)
  {
    dt_gui_preferences_bool_reset(widget);
    return TRUE;
  }
  return FALSE;
}

void dt_gui_preferences_bool_update(GtkWidget *widget)
{
  const char *key = gtk_widget_get_name(widget);
  const gboolean val = dt_conf_get_bool(key);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widget), val);
}

GtkWidget *dt_gui_preferences_bool(GtkGrid *grid, const char *key, const guint col,
                                   const guint line, const gboolean swap)
{
  GtkWidget *w_label = dt_ui_label_new(_(dt_confgen_get_label(key)));
  gtk_widget_set_tooltip_text(w_label, _(dt_confgen_get_tooltip(key)));
  GtkWidget *labelev = gtk_event_box_new();
  gtk_widget_add_events(labelev, GDK_BUTTON_PRESS_MASK);
  gtk_container_add(GTK_CONTAINER(labelev), w_label);
  GtkWidget *w = gtk_check_button_new();
  gtk_widget_set_name(w, key);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w), dt_conf_get_bool(key));
  gtk_grid_attach(GTK_GRID(grid), labelev, swap ? (col + 1) : col, line, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), w, swap ? col : (col + 1), line, 1, 1);
  g_signal_connect(G_OBJECT(w), "toggled", G_CALLBACK(_gui_preferences_bool_callback), (gpointer)key);
  g_signal_connect(G_OBJECT(labelev), "button-press-event", G_CALLBACK(_gui_preferences_bool_reset), (gpointer)w);
  return w;
}

static void
_gui_preferences_int_callback(GtkWidget *widget, gpointer data)
{
  dt_conf_set_int((char *)data, gtk_spin_button_get_value(GTK_SPIN_BUTTON(widget)));
}

void dt_gui_preferences_int_reset(GtkWidget *widget)
{
  const char *key = gtk_widget_get_name(widget);
  const int def = dt_confgen_get_int(key, DT_DEFAULT);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(widget), def);
}

static gboolean
_gui_preferences_int_reset(GtkWidget *label, GdkEventButton *event, GtkWidget *widget)
{
  if(event->type == GDK_2BUTTON_PRESS)
  {
    dt_gui_preferences_int_reset(widget);
    return TRUE;
  }
  return FALSE;
}

void dt_gui_preferences_int_update(GtkWidget *widget)
{
  const char *key = gtk_widget_get_name(widget);
  const int val = dt_conf_get_int(key);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(widget), val);
}

GtkWidget *dt_gui_preferences_int(GtkGrid *grid, const char *key, const guint col,
                                  const guint line)
{
  GtkWidget *w_label = dt_ui_label_new(_(dt_confgen_get_label(key)));
  gtk_widget_set_tooltip_text(w_label, _(dt_confgen_get_tooltip(key)));
  GtkWidget *labelev = gtk_event_box_new();
  gtk_widget_add_events(labelev, GDK_BUTTON_PRESS_MASK);
  gtk_container_add(GTK_CONTAINER(labelev), w_label);
  gint min = MAX(G_MININT, dt_confgen_get_int(key, DT_MIN));
  gint max = MIN(G_MAXINT, dt_confgen_get_int(key, DT_MAX));
  GtkWidget *w = gtk_spin_button_new_with_range(min, max, 1.0);
  gtk_widget_set_name(w, key);
  gtk_widget_set_hexpand(w, FALSE);
  gtk_spin_button_set_digits(GTK_SPIN_BUTTON(w), 0);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(w), dt_conf_get_int(key));
  gtk_grid_attach(GTK_GRID(grid), labelev, col, line, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), w, col + 1, line, 1, 1);
  g_signal_connect(G_OBJECT(w), "value-changed", G_CALLBACK(_gui_preferences_int_callback), (gpointer)key);
  g_signal_connect(G_OBJECT(labelev), "button-press-event", G_CALLBACK(_gui_preferences_int_reset), (gpointer)w);
  return w;
}

static void
_gui_preferences_enum_callback(GtkWidget *widget, gpointer data)
{
  GtkTreeIter iter;
  if(gtk_combo_box_get_active_iter(GTK_COMBO_BOX(widget), &iter))
  {
    gchar *s = NULL;
    gtk_tree_model_get(gtk_combo_box_get_model(GTK_COMBO_BOX(widget)), &iter, 0, &s, -1);
    dt_conf_set_string((char *)data, s);
    g_free(s);
  }
}

void _gui_preferences_enum_set(GtkWidget *widget, const char *str)
{
  GtkTreeModel *model = gtk_combo_box_get_model(GTK_COMBO_BOX(widget));
  GtkTreeIter iter;
  gboolean valid = gtk_tree_model_get_iter_first(model, &iter);
  gint i = 0;
  gboolean found = FALSE;
  while(valid)
  {
    char *value;
    gtk_tree_model_get(model, &iter, 0, &value, -1);
    if(!g_strcmp0(value, str))
    {
      g_free(value);
      found = TRUE;
      break;
    }
    i++;
    g_free(value);
    valid = gtk_tree_model_iter_next(model, &iter);
  }
  if(found)
    gtk_combo_box_set_active(GTK_COMBO_BOX(widget), i);
}

void dt_gui_preferences_enum_reset(GtkWidget *widget)
{
  const char *key = gtk_widget_get_name(widget);
  const char *str = dt_confgen_get(key, DT_DEFAULT);
  _gui_preferences_enum_set(widget, str);
}

static gboolean
_gui_preferences_enum_reset(GtkWidget *label, GdkEventButton *event, GtkWidget *widget)
{
  if(event->type == GDK_2BUTTON_PRESS)
  {
    dt_gui_preferences_enum_reset(widget);
    return TRUE;
  }
  return FALSE;
}

void dt_gui_preferences_enum_update(GtkWidget *widget)
{
  const char *key = gtk_widget_get_name(widget);
  char *str = dt_conf_get_string(key);
  _gui_preferences_enum_set(widget, str);
  g_free(str);
}

GtkWidget *dt_gui_preferences_enum(GtkGrid *grid, const char *key, const guint col,
                                   const guint line)
{
  GtkWidget *w_label = dt_ui_label_new(_(dt_confgen_get_label(key)));
  gtk_widget_set_tooltip_text(w_label, _(dt_confgen_get_tooltip(key)));
  GtkWidget *labelev = gtk_event_box_new();
  gtk_widget_add_events(labelev, GDK_BUTTON_PRESS_MASK);
  gtk_container_add(GTK_CONTAINER(labelev), w_label);

  GtkTreeIter iter;
  GtkListStore *store = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_STRING);
  gchar *str = dt_conf_get_string(key);
  const char *values = dt_confgen_get(key, DT_VALUES);
  gint i = 0;
  gint pos = -1;
  GList *vals = dt_util_str_to_glist("][", values);
  for(GList *val = vals; val; val = g_list_next(val))
  {
    char *item = (char *)val->data;
    // remove remaining [ or ]
    if(item[0] == '[') item++;
    else if(item[strlen(item) - 1] == ']') item[strlen(item) - 1] = '\0';
    gtk_list_store_append(store, &iter);
    gtk_list_store_set(store, &iter, 0, item, 1, g_dpgettext2(NULL, "preferences", item), -1);
    if(pos == -1 && !g_strcmp0(str, item))
    {
      pos = i;
    }
    i++;
  }
  g_list_free_full(vals, g_free);
  g_free(str);

  GtkWidget *w = gtk_combo_box_new_with_model(GTK_TREE_MODEL(store));
  gtk_widget_set_name(w, key);
  gtk_widget_set_hexpand(w, FALSE);
  g_object_unref(store);
  GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
  gtk_cell_renderer_set_padding(renderer, 0, 0);
  gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(w), renderer, TRUE);
  gtk_cell_layout_set_attributes(GTK_CELL_LAYOUT(w), renderer, "text", 1, NULL);
  gtk_combo_box_set_active(GTK_COMBO_BOX(w), pos);

  gtk_grid_attach(GTK_GRID(grid), labelev, col, line, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), w, col + 1, line, 1, 1);
  g_signal_connect(G_OBJECT(w), "changed", G_CALLBACK(_gui_preferences_enum_callback), (gpointer)key);
  g_signal_connect(G_OBJECT(labelev), "button-press-event", G_CALLBACK(_gui_preferences_enum_reset), (gpointer)w);
  return w;
}

static void
_gui_preferences_string_callback(GtkWidget *widget, gpointer data)
{
  const char *str = gtk_entry_get_text(GTK_ENTRY(widget));
  dt_conf_set_string((char *)data, str);
}

void dt_gui_preferences_string_reset(GtkWidget *widget)
{
  const char *key = gtk_widget_get_name(widget);
  const char *str = dt_confgen_get(key, DT_DEFAULT);
  gtk_entry_set_text(GTK_ENTRY(widget), str);
}

static gboolean
_gui_preferences_string_reset(GtkWidget *label, GdkEventButton *event, GtkWidget *widget)
{
  if(event->type == GDK_2BUTTON_PRESS)
  {
    dt_gui_preferences_string_reset(widget);
    return TRUE;
  }
  return FALSE;
}

void dt_gui_preferences_string_update(GtkWidget *widget)
{
  const char *key = gtk_widget_get_name(widget);
  const char *str = dt_conf_get_string_const(key);
  gtk_entry_set_text(GTK_ENTRY(widget), str);
}

GtkWidget *dt_gui_preferences_string(GtkGrid *grid, const char *key, const guint col,
                                     const guint line)
{
  GtkWidget *w_label = dt_ui_label_new(_(dt_confgen_get_label(key)));
  gtk_widget_set_tooltip_text(w_label, _(dt_confgen_get_tooltip(key)));
  GtkWidget *labelev = gtk_event_box_new();
  gtk_widget_add_events(labelev, GDK_BUTTON_PRESS_MASK);
  gtk_container_add(GTK_CONTAINER(labelev), w_label);

  GtkWidget *w = gtk_entry_new();
  const char *str = dt_conf_get_string_const(key);
  gtk_entry_set_text(GTK_ENTRY(w), str);
  gtk_widget_set_hexpand(w, TRUE);
  gtk_widget_set_name(w, key);

  gtk_grid_attach(GTK_GRID(grid), labelev, col, line, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), w, col + 1, line, 1, 1);
  g_signal_connect(G_OBJECT(w), "changed", G_CALLBACK(_gui_preferences_string_callback), (gpointer)key);
  g_signal_connect(G_OBJECT(labelev), "button-press-event", G_CALLBACK(_gui_preferences_string_reset), (gpointer)w);
  return w;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

