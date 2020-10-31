/*
    This file is part of darktable,
    Copyright (C) 2010-2020 darktable developers.

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

typedef struct dt_gui_presets_edit_dialog_t
{
  GtkTreeView *tree; // CHANGED!
  gint rowid;        // CHANGED!
  GtkLabel *name;
  GtkEntry *description;
  GtkCheckButton *autoapply, *filter;
  GtkWidget *details;
  GtkEntry *model, *maker, *lens;
  GtkSpinButton *iso_min, *iso_max;
  GtkWidget *exposure_min, *exposure_max;
  GtkWidget *aperture_min, *aperture_max;
  GtkSpinButton *focal_length_min, *focal_length_max;
  GtkWidget *format_btn[5];
} dt_gui_presets_edit_dialog_t;

typedef struct dt_gui_accel_search_t
{
  GtkWidget *tree, *search_box;
  gchar *last_search_term;
  int last_found_count, curr_found_count;
} dt_gui_accel_search_t;

typedef struct dt_gui_themetweak_widgets_t
{
  GtkWidget *apply_toggle, *save_button, *css_text_view;
} dt_gui_themetweak_widgets_t;

// FIXME: this is copypasta from gui/presets.c. better put these somewhere so that all places can access the
// same data.
static const int dt_gui_presets_exposure_value_cnt = 24;
static const float dt_gui_presets_exposure_value[]
    = { 0.,       1. / 8000, 1. / 4000, 1. / 2000, 1. / 1000, 1. / 1000, 1. / 500, 1. / 250,
        1. / 125, 1. / 60,   1. / 30,   1. / 15,   1. / 15,   1. / 8,    1. / 4,   1. / 2,
        1,        2,         4,         8,         15,        30,        60,       FLT_MAX };
static const char *dt_gui_presets_exposure_value_str[]
    = { "0",     "1/8000", "1/4000", "1/2000", "1/1000", "1/1000", "1/500", "1/250",
        "1/125", "1/60",   "1/30",   "1/15",   "1/15",   "1/8",    "1/4",   "1/2",
        "1\"",   "2\"",    "4\"",    "8\"",    "15\"",   "30\"",   "60\"",  "+" };
static const int dt_gui_presets_aperture_value_cnt = 19;
static const float dt_gui_presets_aperture_value[]
    = { 0,    0.5,  0.7,  1.0,  1.4,  2.0,  2.8,  4.0,   5.6,    8.0,
        11.0, 16.0, 22.0, 32.0, 45.0, 64.0, 90.0, 128.0, FLT_MAX };
static const char *dt_gui_presets_aperture_value_str[]
    = { "f/0",  "f/0.5", "f/0.7", "f/1.0", "f/1.4", "f/2",  "f/2.8", "f/4",   "f/5.6", "f/8",
        "f/11", "f/16",  "f/22",  "f/32",  "f/45",  "f/64", "f/90",  "f/128", "f/+" };

// format string and corresponding flag stored into the database
static const char *dt_gui_presets_format_value_str[5] = { N_("normal images"),
                                                          N_("raw"),
                                                          N_("HDR"),
                                                          N_("monochrome"),
                                                          N_("color")};
static const int dt_gui_presets_format_flag[5] = { FOR_LDR, FOR_RAW, FOR_HDR, FOR_NOT_MONO, FOR_NOT_COLOR };

// Values for the accelerators/presets treeview

enum
{
  A_ACCEL_COLUMN,
  A_BINDING_COLUMN,
  A_TRANS_COLUMN,
  A_N_COLUMNS
};
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
static void init_tab_accels(GtkWidget *stack, dt_gui_accel_search_t *search_data);
static gboolean accel_search(gpointer widget, gpointer data);
static void tree_insert_accel(gpointer accel_struct, gpointer model_link);
static void tree_insert_rec(GtkTreeStore *model, GtkTreeIter *parent, const gchar *accel_path,
                            const gchar *translated_path, guint accel_key, GdkModifierType accel_mods);
static void path_to_accel(GtkTreeModel *model, GtkTreePath *path, gchar *str, size_t str_len);
static void update_accels_model(gpointer widget, gpointer data);
static void update_accels_model_rec(GtkTreeModel *model, GtkTreeIter *parent, gchar *path, size_t path_len);
static void delete_matching_accels(gpointer path, gpointer key_event);
static void import_export(GtkButton *button, gpointer data);
static void restore_defaults(GtkButton *button, gpointer data);
static gint compare_rows_accels(GtkTreeModel *model, GtkTreeIter *a, GtkTreeIter *b, gpointer data);
static gint compare_rows_presets(GtkTreeModel *model, GtkTreeIter *a, GtkTreeIter *b, gpointer data);
static void import_preset(GtkButton *button, gpointer data);

// Signal handlers
static void tree_row_activated_accels(GtkTreeView *tree, GtkTreePath *path, GtkTreeViewColumn *column,
                                      gpointer data);
static void tree_row_activated_presets(GtkTreeView *tree, GtkTreePath *path, GtkTreeViewColumn *column,
                                       gpointer data);
static void tree_selection_changed(GtkTreeSelection *selection, gpointer data);
static gboolean tree_key_press(GtkWidget *widget, GdkEventKey *event, gpointer data);
static gboolean tree_key_press_presets(GtkWidget *widget, GdkEventKey *event, gpointer data);

static void edit_preset(GtkTreeView *tree, const gint rowid, const gchar *name, const gchar *module);
static void edit_preset_response(GtkDialog *dialog, gint response_id, dt_gui_presets_edit_dialog_t *g);

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
  dt_gui_load_theme(dt_conf_get_string("ui_last/theme"));
  dt_bauhaus_load_theme();
}

static void font_size_changed_callback(GtkWidget *widget, gpointer user_data)
{
  dt_conf_set_float("font_size", gtk_spin_button_get_value(GTK_SPIN_BUTTON(widget)));
  dt_gui_load_theme(dt_conf_get_string("ui_last/theme"));
  dt_bauhaus_load_theme();
}

static void use_performance_callback(GtkWidget *widget, gpointer user_data)
{
  dt_conf_set_bool("ui/performance", gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget)));
  dt_configure_ppd_dpi(darktable.gui);
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

  dt_gui_load_theme(dt_conf_get_string("ui_last/theme"));
  dt_bauhaus_load_theme();
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
  const gchar *usercsscontent = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);

  //write to file
  GError *error = NULL;
  if(!g_file_set_contents(usercsspath, usercsscontent, -1, &error))
  {
    fprintf(stderr, "%s: error saving css to %s: %s\n", G_STRFUNC, usercsspath, error->message);
    g_clear_error(&error);
  }

}

static void save_usercss_callback(GtkWidget *widget, gpointer user_data)
{
  dt_gui_themetweak_widgets_t *tw = (dt_gui_themetweak_widgets_t *)user_data;
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tw->css_text_view));

  save_usercss(buffer);

  if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(tw->apply_toggle)))
  {
    //reload the theme
    dt_gui_load_theme(dt_conf_get_string("ui_last/theme"));
    dt_bauhaus_load_theme();
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
  dt_l10n_language_t *language = (dt_l10n_language_t *)g_list_nth(darktable.l10n->languages, selected)->data;
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
  gtk_widget_set_tooltip_text(labelev,  _("double click to reset to the system language"));
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

  GtkWidget *useperfmode = gtk_check_button_new();
  label = gtk_label_new(_("prefer performance over quality"));
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  labelev = gtk_event_box_new();
  gtk_widget_add_events(labelev, GDK_BUTTON_PRESS_MASK);
  gtk_container_add(GTK_CONTAINER(labelev), label);
  gtk_grid_attach(GTK_GRID(grid), labelev, 0, line++, 1, 1);
  gtk_grid_attach_next_to(GTK_GRID(grid), useperfmode, labelev, GTK_POS_RIGHT, 1, 1);
  gtk_widget_set_tooltip_text(useperfmode,
                              _("if switched on, thumbnails and previews are rendered at lower quality but 4 times faster"));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(useperfmode), dt_conf_get_bool("ui/performance"));
  g_signal_connect(G_OBJECT(useperfmode), "toggled", G_CALLBACK(use_performance_callback), 0);

  //Font size check and spin buttons
  GtkWidget *usesysfont = gtk_check_button_new();
  GtkWidget *fontsize = gtk_spin_button_new_with_range(5.0f, 30.0f, 0.2f);

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
  gtk_grid_attach(GTK_GRID(grid), labelev, 0, line++, 1, 1);
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
  gtk_grid_attach(GTK_GRID(grid), labelev, 0, line++, 1, 1);
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
  gtk_grid_attach(GTK_GRID(grid), labelev, 0, line++, 1, 1);
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
  gtk_widget_set_name(usercssbox, "usercss_box");

  GtkTextBuffer *buffer = gtk_text_buffer_new(NULL);
  tw->css_text_view= gtk_text_view_new_with_buffer(buffer);
  gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(tw->css_text_view), GTK_WRAP_WORD);
  gtk_widget_set_hexpand(tw->css_text_view, TRUE);
  gtk_widget_set_halign(tw->css_text_view, GTK_ALIGN_FILL);

  GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_container_add(GTK_CONTAINER(scroll), tw->css_text_view);
  gtk_box_pack_start(GTK_BOX(usercssbox), scroll, TRUE, TRUE, 0);

  tw->save_button = gtk_button_new_with_label(C_("usercss", "save and apply"));
  g_signal_connect(G_OBJECT(tw->save_button), "clicked", G_CALLBACK(save_usercss_callback), tw);
  g_signal_connect(G_OBJECT(dialog), "response", G_CALLBACK(usercss_dialog_callback), tw);
  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_end(GTK_BOX(hbox), tw->save_button, FALSE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(usercssbox), hbox, FALSE, FALSE, 0);

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
      gtk_text_buffer_set_text(buffer, _("/* ERROR Loading user.css */"), -1);
    }
    g_free(usercsscontent);
  }
  else
  {
    //load default text
    gtk_text_buffer_set_text(buffer, _("/* Enter CSS theme tweaks here */\n\n"), -1);
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

  gtk_window_set_default_size(GTK_WINDOW(_preferences_dialog), DT_PIXEL_APPLY_DPI(1100), DT_PIXEL_APPLY_DPI(700));
#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(_preferences_dialog);
#endif
  gtk_window_set_position(GTK_WINDOW(_preferences_dialog), GTK_WIN_POS_CENTER_ON_PARENT);
  gtk_widget_set_name(_preferences_dialog, "preferences_notebook");

  //grab the content area of the dialog
  GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(_preferences_dialog));
  gtk_widget_set_name(content, "preferences_content");
  gtk_container_set_border_width(GTK_CONTAINER(content), 0);

  //place a box in the content area
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name(box, "preferences_box");
  gtk_container_set_border_width(GTK_CONTAINER(box), 0);
  gtk_box_pack_start(GTK_BOX(content), box, TRUE, TRUE, 0);

  //create stack and sidebar and pack into the box
  GtkWidget *stack = gtk_stack_new();
  GtkWidget *stacksidebar = gtk_stack_sidebar_new();
  gtk_stack_sidebar_set_stack(GTK_STACK_SIDEBAR(stacksidebar), GTK_STACK(stack));
  gtk_widget_set_size_request(stack, DT_PIXEL_APPLY_DPI(900), DT_PIXEL_APPLY_DPI(700));
  gtk_box_pack_start(GTK_BOX(box), stacksidebar, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(box), stack, TRUE, TRUE, 0);

  // Make sure remap mode is off initially
  darktable.control->accel_remap_str = NULL;
  darktable.control->accel_remap_path = NULL;

  dt_gui_accel_search_t *search_data = (dt_gui_accel_search_t *)malloc(sizeof(dt_gui_accel_search_t));
  dt_gui_themetweak_widgets_t *tweak_widgets = (dt_gui_themetweak_widgets_t *)malloc(sizeof(dt_gui_themetweak_widgets_t));

  restart_required = FALSE;

  //setup tabs
  init_tab_general(_preferences_dialog, stack, tweak_widgets);
  init_tab_import(_preferences_dialog, stack);
  init_tab_lighttable(_preferences_dialog, stack);
  init_tab_darkroom(_preferences_dialog, stack);
  init_tab_other_views(_preferences_dialog, stack);
  init_tab_processing(_preferences_dialog, stack);
  init_tab_security(_preferences_dialog, stack);
  init_tab_cpugpu(_preferences_dialog, stack);
  init_tab_storage(_preferences_dialog, stack);
  init_tab_misc(_preferences_dialog, stack);
  init_tab_accels(stack, search_data);
  init_tab_presets(stack);

  //open in the appropriate tab if currently in darkroom or lighttable view
  const gchar *current_view = darktable.view_manager->current_view->name(darktable.view_manager->current_view);
  if(strcmp(current_view, "darkroom") == 0 || strcmp(current_view, "lighttable") == 0)
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

  g_free(search_data->last_search_term);
  free(search_data);
  free(tweak_widgets);
  gtk_widget_destroy(_preferences_dialog);

  if(restart_required)
    dt_control_log(_("darktable needs to be restarted for settings to take effect"));

  // Cleaning up any memory still allocated for remapping
  if(darktable.control->accel_remap_path)
  {
    gtk_tree_path_free(darktable.control->accel_remap_path);
    darktable.control->accel_remap_path = NULL;
  }

  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_PREFERENCES_CHANGE);
}

static void cairo_destroy_from_pixbuf(guchar *pixels, gpointer data)
{
  cairo_destroy((cairo_t *)data);
}

static void tree_insert_presets(GtkTreeStore *tree_model)
{
  GtkTreeIter iter, parent;
  sqlite3_stmt *stmt;
  gchar *last_module = NULL;

  // Create a GdkPixbuf with a cairo drawing.
  // lock
  cairo_surface_t *lock_cst = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, DT_PIXEL_APPLY_DPI(ICON_SIZE),
                                                         DT_PIXEL_APPLY_DPI(ICON_SIZE));
  cairo_t *lock_cr = cairo_create(lock_cst);
  cairo_set_source_rgb(lock_cr, 0.7, 0.7, 0.7);
  dtgtk_cairo_paint_lock(lock_cr, 0, 0, DT_PIXEL_APPLY_DPI(ICON_SIZE), DT_PIXEL_APPLY_DPI(ICON_SIZE), 0, NULL);
  cairo_surface_flush(lock_cst);
  guchar *data = cairo_image_surface_get_data(lock_cst);
  dt_draw_cairo_to_gdk_pixbuf(data, DT_PIXEL_APPLY_DPI(ICON_SIZE), DT_PIXEL_APPLY_DPI(ICON_SIZE));
  GdkPixbuf *lock_pixbuf = gdk_pixbuf_new_from_data(data, GDK_COLORSPACE_RGB, TRUE, 8,
                                                    DT_PIXEL_APPLY_DPI(ICON_SIZE), DT_PIXEL_APPLY_DPI(ICON_SIZE),
                                                    cairo_image_surface_get_stride(lock_cst),
                                                    cairo_destroy_from_pixbuf, lock_cr);

  // check mark
  cairo_surface_t *check_cst = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, DT_PIXEL_APPLY_DPI(ICON_SIZE),
                                                          DT_PIXEL_APPLY_DPI(ICON_SIZE));
  cairo_t *check_cr = cairo_create(check_cst);
  cairo_set_source_rgb(check_cr, 0.7, 0.7, 0.7);
  dtgtk_cairo_paint_check_mark(check_cr, 0, 0, DT_PIXEL_APPLY_DPI(ICON_SIZE), DT_PIXEL_APPLY_DPI(ICON_SIZE), 0, NULL);
  cairo_surface_flush(check_cst);
  data = cairo_image_surface_get_data(check_cst);
  dt_draw_cairo_to_gdk_pixbuf(data, DT_PIXEL_APPLY_DPI(ICON_SIZE), DT_PIXEL_APPLY_DPI(ICON_SIZE));
  GdkPixbuf *check_pixbuf = gdk_pixbuf_new_from_data(data, GDK_COLORSPACE_RGB, TRUE, 8,
                                                     DT_PIXEL_APPLY_DPI(ICON_SIZE), DT_PIXEL_APPLY_DPI(ICON_SIZE),
                                                     cairo_image_surface_get_stride(check_cst),
                                                     cairo_destroy_from_pixbuf, check_cr);

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT rowid, name, operation, autoapply, model, maker, lens, iso_min, "
                              "iso_max, exposure_min, exposure_max, aperture_min, aperture_max, "
                              "focal_length_min, focal_length_max, writeprotect FROM data.presets ORDER BY "
                              "operation, name",
                              -1, &stmt, NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW)
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

    gchar *iso = NULL, *exposure = NULL, *aperture = NULL, *focal_length = NULL;
    int min, max;

    gchar *module = g_strdup(dt_iop_get_localized_name(operation));
    if(module == NULL) module = g_strdup(dt_lib_get_localized_name(operation));
    if(module == NULL) module = g_strdup(operation);

    if(iso_min == 0.0 && iso_max == FLT_MAX)
      iso = g_strdup("%");
    else
      iso = g_strdup_printf("%zu – %zu", (size_t)iso_min, (size_t)iso_max);

    min = 0, max = 0;
    for(; min < dt_gui_presets_exposure_value_cnt && exposure_min > dt_gui_presets_exposure_value[min]; min++)
      ;
    for(; max < dt_gui_presets_exposure_value_cnt && exposure_max > dt_gui_presets_exposure_value[max]; max++)
      ;
    if(min == 0 && max == dt_gui_presets_exposure_value_cnt - 1)
      exposure = g_strdup("%");
    else
      exposure = g_strdup_printf("%s – %s", dt_gui_presets_exposure_value_str[min],
                                 dt_gui_presets_exposure_value_str[max]);

    min = 0, max = 0;
    for(; min < dt_gui_presets_aperture_value_cnt && aperture_min > dt_gui_presets_aperture_value[min]; min++)
      ;
    for(; max < dt_gui_presets_aperture_value_cnt && aperture_max > dt_gui_presets_aperture_value[max]; max++)
      ;
    if(min == 0 && max == dt_gui_presets_aperture_value_cnt - 1)
      aperture = g_strdup("%");
    else
      aperture = g_strdup_printf("%s – %s", dt_gui_presets_aperture_value_str[min],
                                 dt_gui_presets_aperture_value_str[max]);

    if(focal_length_min == 0.0 && focal_length_max == 1000.0)
      focal_length = g_strdup("%");
    else
      focal_length = g_strdup_printf("%d – %d", focal_length_min, focal_length_max);

    if(g_strcmp0(last_module, operation) != 0)
    {
      gtk_tree_store_insert_with_values(tree_model, &iter, NULL, -1,
                         P_ROWID_COLUMN, 0, P_OPERATION_COLUMN, "", P_MODULE_COLUMN,
                         _(module), P_EDITABLE_COLUMN, NULL, P_NAME_COLUMN, "", P_MODEL_COLUMN, "",
                         P_MAKER_COLUMN, "", P_LENS_COLUMN, "", P_ISO_COLUMN, "", P_EXPOSURE_COLUMN, "",
                         P_APERTURE_COLUMN, "", P_FOCAL_LENGTH_COLUMN, "", P_AUTOAPPLY_COLUMN, NULL, -1);
      g_free(last_module);
      last_module = g_strdup(operation);
      parent = iter;
    }

    gtk_tree_store_insert_with_values(tree_model, &iter, &parent, -1,
                       P_ROWID_COLUMN, rowid, P_OPERATION_COLUMN, operation,
                       P_MODULE_COLUMN, "", P_EDITABLE_COLUMN, writeprotect ? lock_pixbuf : NULL,
                       P_NAME_COLUMN, name, P_MODEL_COLUMN, model, P_MAKER_COLUMN, maker, P_LENS_COLUMN, lens,
                       P_ISO_COLUMN, iso, P_EXPOSURE_COLUMN, exposure, P_APERTURE_COLUMN, aperture,
                       P_FOCAL_LENGTH_COLUMN, focal_length, P_AUTOAPPLY_COLUMN,
                       autoapply ? check_pixbuf : NULL, -1);

    g_free(focal_length);
    g_free(aperture);
    g_free(exposure);
    g_free(iso);
    g_free(module);
  }
  g_free(last_module);
  sqlite3_finalize(stmt);

  g_object_unref(lock_pixbuf);
  cairo_surface_destroy(lock_cst);
  g_object_unref(check_pixbuf);
  cairo_surface_destroy(check_cst);
}

static void init_tab_presets(GtkWidget *stack)
{
  GtkWidget *container = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
  GtkWidget *tree = gtk_tree_view_new();
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
  gtk_tree_view_append_column(GTK_TREE_VIEW(tree), column);

  renderer = gtk_cell_renderer_pixbuf_new();
  column = gtk_tree_view_column_new_with_attributes("", renderer, "pixbuf", P_EDITABLE_COLUMN, NULL);
  gtk_tree_view_append_column(GTK_TREE_VIEW(tree), column);

  renderer = gtk_cell_renderer_text_new();
  column = gtk_tree_view_column_new_with_attributes(_("name"), renderer, "text", P_NAME_COLUMN, NULL);
  gtk_tree_view_append_column(GTK_TREE_VIEW(tree), column);

  renderer = gtk_cell_renderer_text_new();
  column = gtk_tree_view_column_new_with_attributes(_("model"), renderer, "text", P_MODEL_COLUMN, NULL);
  gtk_tree_view_append_column(GTK_TREE_VIEW(tree), column);

  renderer = gtk_cell_renderer_text_new();
  column = gtk_tree_view_column_new_with_attributes(_("maker"), renderer, "text", P_MAKER_COLUMN, NULL);
  gtk_tree_view_append_column(GTK_TREE_VIEW(tree), column);

  renderer = gtk_cell_renderer_text_new();
  column = gtk_tree_view_column_new_with_attributes(_("lens"), renderer, "text", P_LENS_COLUMN, NULL);
  gtk_tree_view_append_column(GTK_TREE_VIEW(tree), column);

  renderer = gtk_cell_renderer_text_new();
  column = gtk_tree_view_column_new_with_attributes(_("ISO"), renderer, "text", P_ISO_COLUMN, NULL);
  gtk_tree_view_append_column(GTK_TREE_VIEW(tree), column);

  renderer = gtk_cell_renderer_text_new();
  column = gtk_tree_view_column_new_with_attributes(_("exposure"), renderer, "text", P_EXPOSURE_COLUMN, NULL);
  gtk_tree_view_append_column(GTK_TREE_VIEW(tree), column);

  renderer = gtk_cell_renderer_text_new();
  column = gtk_tree_view_column_new_with_attributes(_("aperture"), renderer, "text", P_APERTURE_COLUMN, NULL);
  gtk_tree_view_append_column(GTK_TREE_VIEW(tree), column);

  renderer = gtk_cell_renderer_text_new();
  column = gtk_tree_view_column_new_with_attributes(_("focal length"), renderer, "text",
                                                    P_FOCAL_LENGTH_COLUMN, NULL);
  gtk_tree_view_append_column(GTK_TREE_VIEW(tree), column);

  renderer = gtk_cell_renderer_pixbuf_new();
  column = gtk_tree_view_column_new_with_attributes(_("auto"), renderer, "pixbuf", P_AUTOAPPLY_COLUMN, NULL);
  gtk_tree_view_append_column(GTK_TREE_VIEW(tree), column);

  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_box_pack_start(GTK_BOX(container), scroll, TRUE, TRUE, 0);

  // Adding the import/export buttons
  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

  GtkWidget *button = gtk_button_new_with_label(C_("preferences", "import..."));
  gtk_widget_set_name(hbox, "preset_controls");
  gtk_box_pack_start(GTK_BOX(hbox), button, FALSE, TRUE, 0);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(import_preset), (gpointer)model);

  gtk_box_pack_start(GTK_BOX(container), hbox, FALSE, FALSE, 0);

  // Attaching treeview signals

  // row-activated either expands/collapses a row or activates editing
  g_signal_connect(G_OBJECT(tree), "row-activated", G_CALLBACK(tree_row_activated_presets), NULL);

  // A keypress may delete preset
  g_signal_connect(G_OBJECT(tree), "key-press-event", G_CALLBACK(tree_key_press_presets), (gpointer)model);

  // Setting up the search functionality
  gtk_tree_view_set_search_column(GTK_TREE_VIEW(tree), P_NAME_COLUMN);
  gtk_tree_view_set_enable_search(GTK_TREE_VIEW(tree), TRUE);

  // Attaching the model to the treeview
  gtk_tree_view_set_model(GTK_TREE_VIEW(tree), GTK_TREE_MODEL(model));

  // Adding the treeview to its containers
  gtk_container_add(GTK_CONTAINER(scroll), tree);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);

  g_object_unref(G_OBJECT(model));
}

static void init_tab_accels(GtkWidget *stack, dt_gui_accel_search_t *search_data)
{
  GtkWidget *container = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
  GtkWidget *tree = gtk_tree_view_new();
  GtkWidget *button, *searchentry;
  GtkWidget *hbox;
  GtkTreeStore *model = gtk_tree_store_new(A_N_COLUMNS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
  GtkCellRenderer *renderer;
  GtkTreeViewColumn *column;

  // Adding the outer container
  gtk_stack_add_titled(GTK_STACK(stack), container, _("shortcuts"), _("shortcuts"));

  // Building the accelerator tree
  g_slist_foreach(darktable.control->accelerator_list, tree_insert_accel, (gpointer)model);

  // Setting a custom sort functions so expandable groups rise to the top
  gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(model), A_TRANS_COLUMN, GTK_SORT_ASCENDING);
  gtk_tree_sortable_set_sort_func(GTK_TREE_SORTABLE(model), A_TRANS_COLUMN, compare_rows_accels, NULL, NULL);

  // Setting up the cell renderers
  renderer = gtk_cell_renderer_text_new();
  column = gtk_tree_view_column_new_with_attributes(_("shortcut"), renderer, "text", A_TRANS_COLUMN, NULL);
  gtk_tree_view_append_column(GTK_TREE_VIEW(tree), column);

  renderer = gtk_cell_renderer_text_new();
  column = gtk_tree_view_column_new_with_attributes(_("binding"), renderer, "text", A_BINDING_COLUMN, NULL);
  gtk_tree_view_append_column(GTK_TREE_VIEW(tree), column);

  // Attaching treeview signals

  // row-activated either expands/collapses a row or activates remapping
  g_signal_connect(G_OBJECT(tree), "row-activated", G_CALLBACK(tree_row_activated_accels), NULL);

  // A selection change will cancel a currently active remapping
  g_signal_connect(G_OBJECT(gtk_tree_view_get_selection(GTK_TREE_VIEW(tree))), "changed",
                   G_CALLBACK(tree_selection_changed), NULL);

  // A keypress may remap an accel or delete one
  g_signal_connect(G_OBJECT(tree), "key-press-event", G_CALLBACK(tree_key_press), (gpointer)model);

  // Attaching the model to the treeview
  gtk_tree_view_set_model(GTK_TREE_VIEW(tree), GTK_TREE_MODEL(model));

  // Adding the treeview to its containers
  gtk_container_add(GTK_CONTAINER(scroll), tree);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_box_pack_start(GTK_BOX(container), scroll, TRUE, TRUE, 0);

  // Adding toolbar at bottom of treeview
  hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name(hbox, "shortcut_controls");

  // Adding search box
  searchentry = gtk_entry_new();
  g_signal_connect(G_OBJECT(searchentry), "activate", G_CALLBACK(accel_search), (gpointer)search_data);

  gtk_box_pack_start(GTK_BOX(hbox), searchentry, FALSE, TRUE, 10);

  // Adding the search button
  button = gtk_button_new_with_label(C_("preferences", "search"));
  gtk_widget_set_tooltip_text(GTK_WIDGET(button), _("click or press enter to search\nclick or press enter again to cycle through results"));
  gtk_box_pack_start(GTK_BOX(hbox), button, FALSE, TRUE, 0);
  search_data->tree = tree;
  search_data->search_box = searchentry;
  search_data->last_search_term = NULL;
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(accel_search), (gpointer)search_data);

  // Adding the restore defaults button
  button = gtk_button_new_with_label(C_("preferences", "default"));
  gtk_box_pack_end(GTK_BOX(hbox), button, FALSE, TRUE, 0);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(restore_defaults), NULL);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(update_accels_model), (gpointer)model);

  // Adding the import/export buttons

  button = gtk_button_new_with_label(C_("preferences", "import..."));
  gtk_box_pack_end(GTK_BOX(hbox), button, FALSE, TRUE, 0);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(import_export), (gpointer)0);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(update_accels_model), (gpointer)model);

  button = gtk_button_new_with_label(_("export..."));
  gtk_box_pack_end(GTK_BOX(hbox), button, FALSE, TRUE, 0);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(import_export), (gpointer)1);

  gtk_box_pack_start(GTK_BOX(container), hbox, FALSE, FALSE, 0);

  g_object_unref(G_OBJECT(model));
}

static void tree_insert_accel(gpointer accel_struct, gpointer model_link)
{
  GtkTreeStore *model = (GtkTreeStore *)model_link;
  dt_accel_t *accel = (dt_accel_t *)accel_struct;
  GtkAccelKey key;

  // Getting the first significant parts of the paths
  const char *accel_path = accel->path;
  const char *translated_path = accel->translated_path;

  /* if prefixed lets forward pointer */
  if(!strncmp(accel_path, "<Darktable>", strlen("<Darktable>")))
  {
    accel_path += strlen("<Darktable>") + 1;
    translated_path += strlen("<Darktable>") + 1;
  }

  // Getting the accelerator keys
  gtk_accel_map_lookup_entry(accel->path, &key);

  /* lets recurse path */
  tree_insert_rec(model, NULL, accel_path, translated_path, key.accel_key, key.accel_mods);
}

static void tree_insert_rec(GtkTreeStore *model, GtkTreeIter *parent, const gchar *accel_path,
                            const gchar *translated_path, guint accel_key, GdkModifierType accel_mods)
{
  gboolean found = FALSE;
  gchar *val_str;
  GtkTreeIter iter;

  /* if we are on end of path lets bail out of recursive insert */
  if(*accel_path == 0) return;

  /* check if we are on a leaf or a branch  */
  const gchar *end = strchr(accel_path, '/');
  const gchar *trans_end = strchr(translated_path, '/');
  if(!end || !trans_end)
  {
    gchar *translated_path_slashed = g_strdelimit(g_strdup(translated_path), "`", '/');

    /* we are on a leaf lets add */
    gchar *name = gtk_accelerator_get_label(accel_key, accel_mods);
    gtk_tree_store_insert_with_values(model, &iter, parent, -1,
                                      A_ACCEL_COLUMN, accel_path,
                                      A_BINDING_COLUMN, g_dpgettext2("gtk30", "keyboard label", name),
                                      A_TRANS_COLUMN, translated_path_slashed, -1);
    g_free(name);
    g_free(translated_path_slashed);
  }
  else
  {
    gchar *trans_node = g_strndup(translated_path, trans_end - translated_path);
    gchar *trans_scan = trans_node;
    while((trans_scan = strchr(trans_scan, '`')))
    {
      *(trans_scan) = '/';
      if(end) end = strchr(++end, '/');
    }

    // safeguard against broken translations
    if(!end)
    {
      fprintf(stderr, "error: translation mismatch: `%s' vs. `%s'\n", accel_path, trans_node);
      g_free(trans_node);
      return;
    }


    gchar *node = g_strndup(accel_path, end - accel_path);

    /* search the tree if we already have a sibling with node name */
    int siblings = gtk_tree_model_iter_n_children(GTK_TREE_MODEL(model), parent);
    for(int i = 0; i < siblings; i++)
    {
      gtk_tree_model_iter_nth_child(GTK_TREE_MODEL(model), &iter, parent, i);
      gtk_tree_model_get(GTK_TREE_MODEL(model), &iter, A_ACCEL_COLUMN, &val_str, -1);

      /* do we match current sibling */
      if(!strcmp(val_str, node)) found = TRUE;

      g_free(val_str);

      /* if we found a matching node let's break out */
      if(found) break;
    }

    /* if not found let's add a branch */
    if(!found)
    {
      gtk_tree_store_insert_with_values(model, &iter, parent, -1,
                                        A_ACCEL_COLUMN, node,
                                        A_BINDING_COLUMN, "",
                                        A_TRANS_COLUMN, trans_node, -1);
    }

    /* recurse further down the path */
    tree_insert_rec(model, &iter, accel_path + strlen(node) + 1, translated_path + strlen(trans_node) + 1,
                    accel_key, accel_mods);

    /* free up data */
    g_free(node);
    g_free(trans_node);
  }
}

static void path_to_accel(GtkTreeModel *model, GtkTreePath *path, gchar *str, size_t str_len)
{
  gint depth;
  gint *indices;
  GtkTreeIter parent;
  GtkTreeIter child;
  gint i;
  gchar *data_str;

  // Start out with the base <Darktable>
  g_strlcpy(str, "<Darktable>", str_len);

  // For each index in the path, append a '/' and that section of the path
  depth = gtk_tree_path_get_depth(path);
  indices = gtk_tree_path_get_indices(path);
  for(i = 0; i < depth; i++)
  {
    g_strlcat(str, "/", str_len);
    gtk_tree_model_iter_nth_child(model, &child, i == 0 ? NULL : &parent, indices[i]);
    gtk_tree_model_get(model, &child, A_ACCEL_COLUMN, &data_str, -1);
    g_strlcat(str, data_str, str_len);
    g_free(data_str);
    parent = child;
  }
}

static void update_accels_model(gpointer widget, gpointer data)
{
  GtkTreeModel *model = (GtkTreeModel *)data;
  GtkTreeIter iter;
  gchar path[256];
  gchar *end;
  gint i;

  g_strlcpy(path, "<Darktable>", sizeof(path));
  end = path + strlen(path);

  for(i = 0; i < gtk_tree_model_iter_n_children(model, NULL); i++)
  {
    gtk_tree_model_iter_nth_child(model, &iter, NULL, i);
    update_accels_model_rec(model, &iter, path, sizeof(path));
    *end = '\0'; // Trimming the string back to the base for the next iteration
  }
}

gboolean accel_search_children(dt_gui_accel_search_t *search_data, GtkTreeIter *parent)
{
  GtkTreeView *tv = GTK_TREE_VIEW(search_data->tree);
  GtkTreeModel *tvmodel = gtk_tree_view_get_model(tv);
  const gchar *search_term = gtk_entry_get_text(GTK_ENTRY(search_data->search_box));

  gchar *row_data;
  GtkTreeIter iter;

  //check the current item for a match
  gtk_tree_model_get(tvmodel, parent, A_TRANS_COLUMN, &row_data, -1);

  GtkTreePath *childpath = gtk_tree_model_get_path(tvmodel, parent);

  if(strstr(row_data, search_term))
  {
    search_data->curr_found_count++;
    if(search_data->curr_found_count > search_data->last_found_count)
    {
      gtk_tree_view_expand_to_path(tv, childpath);
      gtk_tree_view_set_cursor(tv, childpath, gtk_tree_view_get_column(tv, A_TRANS_COLUMN), FALSE);
      search_data->last_found_count++;
      return TRUE;
    }
  }

  if(gtk_tree_model_iter_has_child(tvmodel, parent))
  {
    //match not found then call again for each child, each time exiting if matched
    const int siblings = gtk_tree_model_iter_n_children(GTK_TREE_MODEL(tvmodel), parent);
    for(int i = 0; i < siblings; i++)
    {
      gtk_tree_model_iter_nth_child(GTK_TREE_MODEL(tvmodel), &iter, parent, i);
      if(accel_search_children(search_data, &iter))
        return TRUE;
    }
  }

  return FALSE;
}

static gboolean accel_search(gpointer widget, gpointer data)
{
  dt_gui_accel_search_t *search_data = (dt_gui_accel_search_t *)data;
  GtkTreeView *tv = GTK_TREE_VIEW(search_data->tree);
  GtkTreeModel *tvmodel = gtk_tree_view_get_model(tv);
  const gchar *search_term = gtk_entry_get_text(GTK_ENTRY(search_data->search_box));
  if(!search_data->last_search_term || strcmp(search_data->last_search_term, search_term) != 0)
  {
    g_free(search_data->last_search_term);
    search_data->last_search_term = g_strdup(search_term);
    search_data->last_found_count = 0;
  }
  search_data->curr_found_count = 0;
  GtkTreeIter childiter;

  gtk_tree_view_collapse_all(GTK_TREE_VIEW(tv));

  const int siblings = gtk_tree_model_iter_n_children(GTK_TREE_MODEL(tvmodel), NULL);
  for(int i = 0; i < siblings; i++)
  {
    gtk_tree_model_iter_nth_child(GTK_TREE_MODEL(tvmodel), &childiter, NULL, i);
    if(accel_search_children(search_data, &childiter))
      return TRUE;
  }
  search_data->last_found_count = 0;
  return FALSE;
}

static void update_accels_model_rec(GtkTreeModel *model, GtkTreeIter *parent, gchar *path, size_t path_len)
{
  GtkAccelKey key;
  GtkTreeIter iter;
  gchar *str_data;

  // First concatenating this part of the key
  g_strlcat(path, "/", path_len);
  gtk_tree_model_get(model, parent, A_ACCEL_COLUMN, &str_data, -1);
  g_strlcat(path, str_data, path_len);
  g_free(str_data);

  if(gtk_tree_model_iter_has_child(model, parent))
  {
    // Branch node, carry on with recursion
    gchar *end = path + strlen(path);

    for(gint i = 0; i < gtk_tree_model_iter_n_children(model, parent); i++)
    {
      gtk_tree_model_iter_nth_child(model, &iter, parent, i);
      update_accels_model_rec(model, &iter, path, path_len);
      *end = '\0';
    }
  }
  else
  {
    // Leaf node, update the text

    gtk_accel_map_lookup_entry(path, &key);
    gchar *name = gtk_accelerator_get_label(key.accel_key, key.accel_mods);
    gtk_tree_store_set(GTK_TREE_STORE(model), parent, A_BINDING_COLUMN, name, -1);
    g_free(name);
  }
}

static void delete_matching_accels(gpointer current, gpointer mapped)
{
  const dt_accel_t *current_accel = (dt_accel_t *)current;
  const dt_accel_t *mapped_accel = (dt_accel_t *)mapped;
  GtkAccelKey current_key;
  GtkAccelKey mapped_key;

  // Make sure we're not deleting the key we just remapped
  if(!strcmp(current_accel->path, mapped_accel->path)) return;

  // Finding the relevant keyboard shortcuts
  gtk_accel_map_lookup_entry(current_accel->path, &current_key);
  gtk_accel_map_lookup_entry(mapped_accel->path, &mapped_key);

  if(current_key.accel_key == mapped_key.accel_key                 // Key code matches
     && current_key.accel_mods == mapped_key.accel_mods            // Key state matches
     && !(current_accel->local && mapped_accel->local              // Not both local to
          && strcmp(current_accel->module, mapped_accel->module))
     && (current_accel->views & mapped_accel->views) != 0) // diff mods
    gtk_accel_map_change_entry(current_accel->path, 0, 0, TRUE);
}

static gint _accelcmp(gconstpointer a, gconstpointer b)
{
  return (gint)(strcmp(((dt_accel_t *)a)->path, ((dt_accel_t *)b)->path));
}

// TODO: remember which sections were collapsed/expanded and where the view was scrolled to and restore that
// after editing is done
//      Alternative: change edit_preset_response to not clear+refill the tree, but to update the single row
//      which changed.
static void tree_row_activated_presets(GtkTreeView *tree, GtkTreePath *path, GtkTreeViewColumn *column,
                                       gpointer data)
{
  GtkTreeIter iter;
  GtkTreeModel *model = gtk_tree_view_get_model(tree);

  gtk_tree_model_get_iter(model, &iter, path);

  if(gtk_tree_model_iter_has_child(model, &iter))
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
    gtk_tree_model_get(model, &iter, P_ROWID_COLUMN, &rowid, P_NAME_COLUMN, &name, P_OPERATION_COLUMN,
                       &operation, P_EDITABLE_COLUMN, &editable, -1);
    if(editable == NULL)
      edit_preset(tree, rowid, name, operation);
    else
      g_object_unref(editable);
    g_free(name);
    g_free(operation);
  }
}

static void tree_row_activated_accels(GtkTreeView *tree, GtkTreePath *path, GtkTreeViewColumn *column,
                                      gpointer data)
{
  GtkTreeIter iter;
  GtkTreeModel *model = gtk_tree_view_get_model(tree);

  static gchar accel_path[256];

  gtk_tree_model_get_iter(model, &iter, path);

  if(gtk_tree_model_iter_has_child(model, &iter))
  {
    // For branch nodes, toggle expansion on activation
    if(gtk_tree_view_row_expanded(tree, path))
      gtk_tree_view_collapse_row(tree, path);
    else
      gtk_tree_view_expand_row(tree, path, FALSE);
  }
  else
  {
    // For leaf nodes, enter remapping mode

    // Assembling the full accelerator path
    path_to_accel(model, path, accel_path, sizeof(accel_path));

    // Setting the notification text
    gtk_tree_store_set(GTK_TREE_STORE(model), &iter, A_BINDING_COLUMN, _("press key combination to remap..."),
                       -1);

    // Activating remapping
    darktable.control->accel_remap_str = accel_path;
    darktable.control->accel_remap_path = gtk_tree_path_copy(path);
  }
}

static void tree_selection_changed(GtkTreeSelection *selection, gpointer data)
{
  GtkTreeModel *model;
  GtkTreeIter iter;

  GtkAccelKey key;

  // If remapping is currently activated, it needs to be deactivated
  if(!darktable.control->accel_remap_str) return;

  model = gtk_tree_view_get_model(gtk_tree_selection_get_tree_view(selection));
  gtk_tree_model_get_iter(model, &iter, darktable.control->accel_remap_path);

  // Restoring the A_BINDING_COLUMN text
  gtk_accel_map_lookup_entry(darktable.control->accel_remap_str, &key);
  gchar *name = gtk_accelerator_get_label(key.accel_key, key.accel_mods);
  gtk_tree_store_set(GTK_TREE_STORE(model), &iter, A_BINDING_COLUMN, name, -1);
  g_free(name);

  // Cleaning up the darktable.gui info
  darktable.control->accel_remap_str = NULL;
  gtk_tree_path_free(darktable.control->accel_remap_path);
  darktable.control->accel_remap_path = NULL;
}

static gboolean tree_key_press(GtkWidget *widget, GdkEventKey *event, gpointer data)
{
  GtkTreeModel *model = (GtkTreeModel *)data;
  GtkTreeIter iter;
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(widget));
  GtkTreePath *path;
  dt_accel_t query;

  gchar accel[256];
  gchar datadir[PATH_MAX] = { 0 };
  gchar accelpath[PATH_MAX] = { 0 };

  // We can just ignore mod key presses outright
  if(event->is_modifier) return FALSE;

  dt_loc_get_user_config_dir(datadir, sizeof(datadir));
  snprintf(accelpath, sizeof(accelpath), "%s/keyboardrc", datadir);

  // Otherwise, determine whether we're in remap mode or not
  if(darktable.control->accel_remap_str)
  {
    const guint event_mods = dt_gui_translated_key_state(event);

    // First locate the accel list entry
    g_strlcpy(query.path, darktable.control->accel_remap_str, sizeof(query.path));
    GSList *remapped = g_slist_find_custom(darktable.control->accelerator_list, (gpointer)&query, _accelcmp);
    const dt_accel_t *accel_current = (dt_accel_t *)remapped->data;

    // let's search for conflicts
    dt_accel_t *accel_conflict = NULL;
    GSList *l = darktable.control->accelerator_list;
    while (l)
    {
      dt_accel_t *a = (dt_accel_t *)l->data;
      GtkAccelKey key;
      if (a != accel_current && gtk_accel_map_lookup_entry(a->path, &key))
      {
        if (key.accel_key == gdk_keyval_to_lower(event->keyval) &&
            key.accel_mods == event_mods &&
            !(a->local && accel_current->local && strcmp(a->module, accel_current->module)) &&
            (a->views & accel_current->views) != 0)
        {
          accel_conflict = a;
          break;
        }
      }
      l = g_slist_next(l);
    }

    if(!accel_conflict)
    {
      // no conflict
      gtk_accel_map_change_entry(darktable.control->accel_remap_str, gdk_keyval_to_lower(event->keyval),
                                 event_mods, TRUE);
    }
    else
    {
      // we ask for confirmation
      gchar *accel_txt
          = gtk_accelerator_get_label(gdk_keyval_to_lower(event->keyval), event_mods);
      gchar txt[512] = { 0 };
      if(g_str_has_prefix(accel_conflict->translated_path, "<Darktable>/"))
        g_strlcpy(txt, accel_conflict->translated_path + 12, sizeof(txt));
      else
        g_strlcpy(txt, accel_conflict->translated_path, sizeof(txt));
      GtkWidget *dialog = gtk_message_dialog_new(
        GTK_WINDOW(_preferences_dialog), GTK_DIALOG_DESTROY_WITH_PARENT | GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO,
          _("%s accel is already mapped to\n%s.\ndo you want to replace it ?"), accel_txt, txt);
      g_free(accel_txt);
#ifdef GDK_WINDOWING_QUARTZ
      dt_osx_disallow_fullscreen(dialog);
#endif

      gtk_window_set_title(GTK_WINDOW(dialog), _("accel conflict"));
      gint res = gtk_dialog_run(GTK_DIALOG(dialog));
      gtk_widget_destroy(dialog);
      if(res == GTK_RESPONSE_YES)
      {
        // Change the accel map entry
        if(gtk_accel_map_change_entry(darktable.control->accel_remap_str, gdk_keyval_to_lower(event->keyval),
                                      event_mods, TRUE))
        {
          // Then remove conflicts
          g_slist_foreach(darktable.control->accelerator_list, delete_matching_accels, (gpointer)(accel_current));
        }
      }
    }

    // Then update the text in the A_BINDING_COLUMN of each row
    update_accels_model(NULL, model);

    // Finally clear the remap state
    darktable.control->accel_remap_str = NULL;
    gtk_tree_path_free(darktable.control->accel_remap_path);
    darktable.control->accel_remap_path = NULL;

    // Save the changed keybindings
    gtk_accel_map_save(accelpath);

    return TRUE;
  }
  else if(event->keyval == GDK_KEY_BackSpace)
  {
    // If a leaf node is selected, clear that accelerator

    // If nothing is selected, or branch node selected, just return
    if(!gtk_tree_selection_get_selected(selection, &model, &iter)
       || gtk_tree_model_iter_has_child(model, &iter))
      return FALSE;

    // Otherwise, construct the proper accelerator path and delete its entry
    g_strlcpy(accel, "<Darktable>", sizeof(accel));
    path = gtk_tree_model_get_path(model, &iter);
    path_to_accel(model, path, accel, sizeof(accel));
    gtk_tree_path_free(path);

    gtk_accel_map_change_entry(accel, 0, 0, TRUE);
    update_accels_model(NULL, model);

    // Saving the changed bindings
    gtk_accel_map_save(accelpath);

    return TRUE;
  }
  else
  {
    return FALSE;
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
    gchar *name;
    GdkPixbuf *editable;
    gtk_tree_model_get(model, &iter, P_ROWID_COLUMN, &rowid, P_NAME_COLUMN, &name,
                       P_EDITABLE_COLUMN, &editable, -1);
    if(editable == NULL)
    {
      sqlite3_stmt *stmt;
      gchar* operation = NULL;

      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT name, operation FROM data.presets WHERE rowid = ?1",
                              -1, &stmt, NULL);
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, rowid);
      if(sqlite3_step(stmt) == SQLITE_ROW)
      {
        operation = g_strdup( (const char*)sqlite3_column_text(stmt,1));
      }
      sqlite3_finalize(stmt);

      GtkWidget *dialog = gtk_message_dialog_new
        (GTK_WINDOW(_preferences_dialog), GTK_DIALOG_DESTROY_WITH_PARENT | GTK_DIALOG_MODAL,
         GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO,
         _("do you really want to delete the preset `%s'?"), name);
#ifdef GDK_WINDOWING_QUARTZ
      dt_osx_disallow_fullscreen(dialog);
#endif
      gtk_window_set_title(GTK_WINDOW(dialog), _("delete preset?"));

      if(gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_YES)
      {
        //deregistering accel...
        if(operation)
        {
          gchar accel[256];
          gchar datadir[PATH_MAX] = { 0 };
          gchar accelpath[PATH_MAX] = { 0 };

          dt_loc_get_user_config_dir(datadir, sizeof(datadir));
          snprintf(accelpath, sizeof(accelpath), "%s/keyboardrc", datadir);

          gchar *preset_name = g_strdup_printf("%s`%s", N_("preset"), name);
          dt_accel_path_iop(accel, sizeof(accel), operation, preset_name);
          g_free(preset_name);

          gtk_accel_map_change_entry(accel, 0, 0, TRUE);

          // Saving the changed bindings
          gtk_accel_map_save(accelpath);
        }

        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                    "DELETE FROM data.presets WHERE rowid=?1 AND writeprotect=0", -1, &stmt, NULL);
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, rowid);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        GtkTreeStore *tree_store = GTK_TREE_STORE(model);
        gtk_tree_store_clear(tree_store);
        tree_insert_presets(tree_store);
      }
      gtk_widget_destroy(dialog);
      if(operation)
        g_free(operation);
    }
    else
      g_object_unref(editable);
    g_free(name);

    return TRUE;
  }
  else
  {
    return FALSE;
  }
}

static void import_export(GtkButton *button, gpointer data)
{
  GtkWidget *chooser;
  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  gchar confdir[PATH_MAX] = { 0 };
  gchar accelpath[PATH_MAX] = { 0 };

  if(data)
  {
    // Non-zero value indicates export
    chooser = gtk_file_chooser_dialog_new(_("select file to export"), GTK_WINDOW(win), GTK_FILE_CHOOSER_ACTION_SAVE,
                                          _("_cancel"), GTK_RESPONSE_CANCEL, _("_save"), GTK_RESPONSE_ACCEPT,
                                          NULL);
#ifdef GDK_WINDOWING_QUARTZ
    dt_osx_disallow_fullscreen(chooser);
#endif
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(chooser), TRUE);
    gchar *exported_path = dt_conf_get_string("ui_last/exported_path");
    if(exported_path != NULL)
    {
      gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(chooser), exported_path);
      g_free(exported_path);
    }
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(chooser), "keyboardrc");
    if(gtk_dialog_run(GTK_DIALOG(chooser)) == GTK_RESPONSE_ACCEPT)
    {
      gtk_accel_map_save(gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser)));
      gchar *folder = gtk_file_chooser_get_current_folder(GTK_FILE_CHOOSER(chooser));
      dt_conf_set_string("ui_last/export_path", folder);
      g_free(folder);
    }
    gtk_widget_destroy(chooser);
  }
  else
  {
    // Zero value indicates import
    chooser = gtk_file_chooser_dialog_new(_("select file to import"), GTK_WINDOW(win), GTK_FILE_CHOOSER_ACTION_OPEN,
                                          _("_cancel"), GTK_RESPONSE_CANCEL, _("_open"), GTK_RESPONSE_ACCEPT,
                                          NULL);
#ifdef GDK_WINDOWING_QUARTZ
    dt_osx_disallow_fullscreen(chooser);
#endif

    gchar *import_path = dt_conf_get_string("ui_last/import_path");
    if(import_path != NULL)
    {
      gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(chooser), import_path);
      g_free(import_path);
    }
    if(gtk_dialog_run(GTK_DIALOG(chooser)) == GTK_RESPONSE_ACCEPT)
    {
      if(g_file_test(gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser)), G_FILE_TEST_EXISTS))
      {
        // Loading the file
        gtk_accel_map_load(gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser)));

        // Saving to the permanent keyboardrc
        dt_loc_get_user_config_dir(confdir, sizeof(confdir));
        snprintf(accelpath, sizeof(accelpath), "%s/keyboardrc", confdir);
        gtk_accel_map_save(accelpath);

        gchar *folder = gtk_file_chooser_get_current_folder(GTK_FILE_CHOOSER(chooser));
        dt_conf_set_string("ui_last/import_path", folder);
        g_free(folder);
      }
    }
    gtk_widget_destroy(chooser);
  }
}

static void restore_defaults(GtkButton *button, gpointer data)
{
  gchar accelpath[256];
  gchar dir[PATH_MAX] = { 0 };
  gchar path[PATH_MAX] = { 0 };

  GtkWidget *message
      = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING, GTK_BUTTONS_OK_CANCEL,
                               _("are you sure you want to restore the default keybindings?  this will "
                                 "erase any modifications you have made."));
#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(message);
#endif
  if(gtk_dialog_run(GTK_DIALOG(message)) == GTK_RESPONSE_OK)
  {
    // First load the default keybindings for immediate effect
    dt_loc_get_user_config_dir(dir, sizeof(dir));
    snprintf(path, sizeof(path), "%s/keyboardrc_default", dir);
    gtk_accel_map_load(path);

    // Now deleting any iop show shortcuts
    GList *ops = darktable.iop;
    while(ops)
    {
      dt_iop_module_so_t *op = (dt_iop_module_so_t *)ops->data;
      snprintf(accelpath, sizeof(accelpath), "<Darktable>/darkroom/modules/%s/show", op->op);
      gtk_accel_map_change_entry(accelpath, 0, 0, TRUE);
      ops = g_list_next(ops);
    }

    // Then delete any changes to the user's keyboardrc so it gets reset
    // on next startup
    dt_loc_get_user_config_dir(dir, sizeof(dir));
    snprintf(path, sizeof(path), "%s/keyboardrc", dir);

    GFile *gpath = g_file_new_for_path(path);
    g_file_delete(gpath, NULL, NULL);
    g_object_unref(gpath);
  }
  gtk_widget_destroy(message);
}

static void import_preset(GtkButton *button, gpointer data)
{
  GtkTreeModel *model = (GtkTreeModel *)data;
  GtkWidget *chooser;
  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);

  // Zero value indicates import
  chooser = gtk_file_chooser_dialog_new(_("select preset to import"), GTK_WINDOW(win), GTK_FILE_CHOOSER_ACTION_OPEN,
                                        _("_cancel"), GTK_RESPONSE_CANCEL, _("_open"), GTK_RESPONSE_ACCEPT,
                                        NULL);
#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(chooser);
#endif

  gchar *import_path = dt_conf_get_string("ui_last/import_path");
  if(import_path != NULL)
  {
    gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(chooser), import_path);
    g_free(import_path);
  }
  if(gtk_dialog_run(GTK_DIALOG(chooser)) == GTK_RESPONSE_ACCEPT)
  {
    if(g_file_test(gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser)), G_FILE_TEST_EXISTS))
    {
      if(dt_presets_import_from_file(gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser))))
      {
        dt_control_log(_("failed to import preset"));
      }
      else
      {
        GtkTreeStore *tree_store = GTK_TREE_STORE(model);
        gtk_tree_store_clear(tree_store);
        tree_insert_presets(tree_store);
      }

      gchar *folder = gtk_file_chooser_get_current_folder(GTK_FILE_CHOOSER(chooser));
      dt_conf_set_string("ui_last/import_path", folder);
      g_free(folder);
    }
  }
  gtk_widget_destroy(chooser);
}

// Custom sort function for TreeModel entries for accels list
static gint compare_rows_accels(GtkTreeModel *model, GtkTreeIter *a, GtkTreeIter *b, gpointer data)
{
  int res = 0;

  gchar *a_text;
  gchar *b_text;

  // First prioritize branch nodes over leaves
  if(gtk_tree_model_iter_has_child(model, a)) res -= 2;
  if(gtk_tree_model_iter_has_child(model, b)) res += 2;

  // Otherwise just return alphabetical order
  gtk_tree_model_get(model, a, A_TRANS_COLUMN, &a_text, -1);
  gtk_tree_model_get(model, b, A_TRANS_COLUMN, &b_text, -1);

  // but put default actions (marked with space at end) first
  if(a_text[strlen(a_text)-1] == ' ') res = -4; // ignore children
  if(b_text[strlen(b_text)-1] == ' ') res += 4;

  res += strcoll(a_text, b_text) < 0 ? -1 : 1;

  g_free(a_text);
  g_free(b_text);

  return res;
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

// FIXME: Mostly c&p from gui/presets.c
static void check_buttons_activated(GtkCheckButton *button, dt_gui_presets_edit_dialog_t *g)
{
  if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g->autoapply))
     || gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g->filter)))
  {
    gtk_widget_set_visible(GTK_WIDGET(g->details), TRUE);
    gtk_widget_set_no_show_all(GTK_WIDGET(g->details), FALSE);
    gtk_widget_show_all(GTK_WIDGET(g->details));
    gtk_widget_set_no_show_all(GTK_WIDGET(g->details), TRUE);
  }
  else
    gtk_widget_set_visible(GTK_WIDGET(g->details), FALSE);
}

static void edit_preset(GtkTreeView *tree, const gint rowid, const gchar *name, const gchar *module)
{
  GtkWidget *dialog;
  /* Create the widgets */
  char title[1024];
  snprintf(title, sizeof(title), _("edit `%s' for module `%s'"), name, module);
  dialog = gtk_dialog_new_with_buttons(title, GTK_WINDOW(_preferences_dialog),
                                       GTK_DIALOG_DESTROY_WITH_PARENT | GTK_DIALOG_MODAL,
                                       _("_cancel"), GTK_RESPONSE_CANCEL,
                                       _("_save"), GTK_RESPONSE_YES,
                                       _("delete"), GTK_RESPONSE_REJECT,
                                       _("_ok"), GTK_RESPONSE_OK, NULL);
#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(dialog);
#endif
  GtkContainer *content_area = GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dialog)));
  GtkBox *box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
  gtk_container_add(content_area, GTK_WIDGET(box));
  GtkWidget *label;

  dt_gui_presets_edit_dialog_t *g
      = (dt_gui_presets_edit_dialog_t *)malloc(sizeof(dt_gui_presets_edit_dialog_t));
  g->rowid = rowid;
  g->tree = tree;
  g->name = GTK_LABEL(gtk_label_new(name));
  gtk_box_pack_start(box, GTK_WIDGET(g->name), FALSE, FALSE, 0);

  g->description = GTK_ENTRY(gtk_entry_new());
  gtk_box_pack_start(box, GTK_WIDGET(g->description), FALSE, FALSE, 0);
  gtk_widget_set_tooltip_text(GTK_WIDGET(g->description), _("description or further information"));

  g->autoapply
      = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("auto apply this preset to matching images")));
  gtk_box_pack_start(box, GTK_WIDGET(g->autoapply), FALSE, FALSE, 0);
  g->filter
      = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("only show this preset for matching images")));
  gtk_widget_set_tooltip_text(GTK_WIDGET(g->filter),
                              _("be very careful with this option. this might be the last time you see your preset."));
  gtk_box_pack_start(box, GTK_WIDGET(g->filter), FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(g->autoapply), "toggled", G_CALLBACK(check_buttons_activated), g);
  g_signal_connect(G_OBJECT(g->filter), "toggled", G_CALLBACK(check_buttons_activated), g);

  int line = 0;
  g->details = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(g->details), DT_PIXEL_APPLY_DPI(3));
  gtk_grid_set_column_spacing(GTK_GRID(g->details), DT_PIXEL_APPLY_DPI(10));
  gtk_box_pack_start(box, GTK_WIDGET(g->details), FALSE, FALSE, 0);

  // model, maker, lens
  g->model = GTK_ENTRY(gtk_entry_new());
  /* xgettext:no-c-format */
  gtk_widget_set_tooltip_text(GTK_WIDGET(g->model), _("string to match model (use % as wildcard)"));
  label = gtk_label_new(_("model"));
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(g->details), label, 0, line++, 1, 1);
  gtk_grid_attach_next_to(GTK_GRID(g->details), GTK_WIDGET(g->model), label, GTK_POS_RIGHT, 2, 1);

  g->maker = GTK_ENTRY(gtk_entry_new());
  /* xgettext:no-c-format */
  gtk_widget_set_tooltip_text(GTK_WIDGET(g->maker), _("string to match maker (use % as wildcard)"));
  label = gtk_label_new(_("maker"));
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(g->details), label, 0, line++, 1, 1);
  gtk_grid_attach_next_to(GTK_GRID(g->details), GTK_WIDGET(g->maker), label, GTK_POS_RIGHT, 2, 1);

  g->lens = GTK_ENTRY(gtk_entry_new());
  /* xgettext:no-c-format */
  gtk_widget_set_tooltip_text(GTK_WIDGET(g->lens), _("string to match lens (use % as wildcard)"));
  label = gtk_label_new(_("lens"));
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(g->details), label, 0, line++, 1, 1);
  gtk_grid_attach_next_to(GTK_GRID(g->details), GTK_WIDGET(g->lens), label, GTK_POS_RIGHT, 2, 1);

  // iso
  label = gtk_label_new(_("ISO"));
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  g->iso_min = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(0, FLT_MAX, 100));
  gtk_widget_set_tooltip_text(GTK_WIDGET(g->iso_min), _("minimum ISO value"));
  gtk_spin_button_set_digits(g->iso_min, 0);
  g->iso_max = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(0, FLT_MAX, 100));
  gtk_widget_set_tooltip_text(GTK_WIDGET(g->iso_max), _("maximum ISO value"));
  gtk_spin_button_set_digits(g->iso_max, 0);
  gtk_grid_attach(GTK_GRID(g->details), label, 0, line++, 1, 1);
  gtk_grid_attach_next_to(GTK_GRID(g->details), GTK_WIDGET(g->iso_min), label, GTK_POS_RIGHT, 1, 1);
  gtk_grid_attach_next_to(GTK_GRID(g->details), GTK_WIDGET(g->iso_max), GTK_WIDGET(g->iso_min), GTK_POS_RIGHT, 1, 1);

  // exposure
  label = gtk_label_new(_("exposure"));
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  g->exposure_min = dt_bauhaus_combobox_new(NULL);
  g->exposure_max = dt_bauhaus_combobox_new(NULL);
  gtk_widget_set_tooltip_text(g->exposure_min, _("minimum exposure time"));
  gtk_widget_set_tooltip_text(g->exposure_max, _("maximum exposure time"));
  for(int k = 0; k < dt_gui_presets_exposure_value_cnt; k++)
    dt_bauhaus_combobox_add(g->exposure_min, dt_gui_presets_exposure_value_str[k]);
  for(int k = 0; k < dt_gui_presets_exposure_value_cnt; k++)
    dt_bauhaus_combobox_add(g->exposure_max, dt_gui_presets_exposure_value_str[k]);
  gtk_grid_attach(GTK_GRID(g->details), label, 0, line++, 1, 1);
  gtk_grid_attach_next_to(GTK_GRID(g->details), g->exposure_min, label, GTK_POS_RIGHT, 1, 1);
  gtk_grid_attach_next_to(GTK_GRID(g->details), g->exposure_max, g->exposure_min, GTK_POS_RIGHT, 1, 1);

  // aperture
  label = gtk_label_new(_("aperture"));
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  g->aperture_min = dt_bauhaus_combobox_new(NULL);
  g->aperture_max = dt_bauhaus_combobox_new(NULL);
  gtk_widget_set_tooltip_text(g->aperture_min, _("minimum aperture value"));
  gtk_widget_set_tooltip_text(g->aperture_max, _("maximum aperture value"));
  for(int k = 0; k < dt_gui_presets_aperture_value_cnt; k++)
    dt_bauhaus_combobox_add(g->aperture_min, dt_gui_presets_aperture_value_str[k]);
  for(int k = 0; k < dt_gui_presets_aperture_value_cnt; k++)
    dt_bauhaus_combobox_add(g->aperture_max, dt_gui_presets_aperture_value_str[k]);
  gtk_grid_attach(GTK_GRID(g->details), label, 0, line++, 1, 1);
  gtk_grid_attach_next_to(GTK_GRID(g->details), g->aperture_min, label, GTK_POS_RIGHT, 1, 1);
  gtk_grid_attach_next_to(GTK_GRID(g->details), g->aperture_max, g->aperture_min, GTK_POS_RIGHT, 1, 1);

  // focal length
  label = gtk_label_new(_("focal length"));
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  g->focal_length_min = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(0, 1000, 10));
  gtk_spin_button_set_digits(g->focal_length_min, 0);
  g->focal_length_max = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(0, 1000, 10));
  gtk_widget_set_tooltip_text(GTK_WIDGET(g->focal_length_min), _("minimum focal length"));
  gtk_widget_set_tooltip_text(GTK_WIDGET(g->focal_length_max), _("maximum focal length"));
  gtk_spin_button_set_digits(g->focal_length_max, 0);
  gtk_grid_attach(GTK_GRID(g->details), label, 0, line++, 1, 1);
  gtk_grid_attach_next_to(GTK_GRID(g->details), GTK_WIDGET(g->focal_length_min), label, GTK_POS_RIGHT, 1, 1);
  gtk_grid_attach_next_to(GTK_GRID(g->details), GTK_WIDGET(g->focal_length_max), GTK_WIDGET(g->focal_length_min), GTK_POS_RIGHT, 1, 1);
  gtk_widget_set_hexpand(GTK_WIDGET(g->focal_length_min), TRUE);
  gtk_widget_set_hexpand(GTK_WIDGET(g->focal_length_max), TRUE);

  // raw/hdr/ldr/monochrome/color
  label = gtk_label_new(_("format"));
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(g->details), label, 0, line, 1, 1);
  gtk_widget_set_tooltip_text(label, _("select image types you want this preset to be available for"));

  for(int i = 0; i < 5; i++)
  {
    g->format_btn[i] = gtk_check_button_new_with_label(_(dt_gui_presets_format_value_str[i]));
    gtk_grid_attach(GTK_GRID(g->details), g->format_btn[i], 1, line + i, 2, 1);
  }

  gtk_widget_set_no_show_all(GTK_WIDGET(g->details), TRUE);

  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT description, model, maker, lens, iso_min, iso_max, exposure_min, "
                              "exposure_max, aperture_min, aperture_max, focal_length_min, focal_length_max, "
                              "autoapply, filter, format FROM data.presets WHERE rowid = ?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, rowid);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    gtk_entry_set_text(g->description, (const char *)sqlite3_column_text(stmt, 0));
    gtk_entry_set_text(g->model, (const char *)sqlite3_column_text(stmt, 1));
    gtk_entry_set_text(g->maker, (const char *)sqlite3_column_text(stmt, 2));
    gtk_entry_set_text(g->lens, (const char *)sqlite3_column_text(stmt, 3));
    gtk_spin_button_set_value(g->iso_min, sqlite3_column_double(stmt, 4));
    gtk_spin_button_set_value(g->iso_max, sqlite3_column_double(stmt, 5));

    float val = sqlite3_column_double(stmt, 6);
    int k = 0;
    for(; k < dt_gui_presets_exposure_value_cnt && val > dt_gui_presets_exposure_value[k]; k++)
      ;
    dt_bauhaus_combobox_set(g->exposure_min, k);
    val = sqlite3_column_double(stmt, 7);
    for(k = 0; k < dt_gui_presets_exposure_value_cnt && val > dt_gui_presets_exposure_value[k]; k++)
      ;
    dt_bauhaus_combobox_set(g->exposure_max, k);
    val = sqlite3_column_double(stmt, 8);
    for(k = 0; k < dt_gui_presets_aperture_value_cnt && val > dt_gui_presets_aperture_value[k]; k++)
      ;
    dt_bauhaus_combobox_set(g->aperture_min, k);
    val = sqlite3_column_double(stmt, 9);
    for(k = 0; k < dt_gui_presets_aperture_value_cnt && val > dt_gui_presets_aperture_value[k]; k++)
      ;
    dt_bauhaus_combobox_set(g->aperture_max, k);
    gtk_spin_button_set_value(g->focal_length_min, sqlite3_column_double(stmt, 10));
    gtk_spin_button_set_value(g->focal_length_max, sqlite3_column_double(stmt, 11));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->autoapply), sqlite3_column_int(stmt, 12));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->filter), sqlite3_column_int(stmt, 13));
    const int format = (sqlite3_column_int(stmt, 14)) ^ DT_PRESETS_FOR_NOT;
    for(k = 0; k < 5; k++)
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->format_btn[k]), format & (dt_gui_presets_format_flag[k]));
  }
  sqlite3_finalize(stmt);

  g_signal_connect(dialog, "response", G_CALLBACK(edit_preset_response), g);
  gtk_widget_show_all(dialog);
}

static void edit_preset_response(GtkDialog *dialog, gint response_id, dt_gui_presets_edit_dialog_t *g)
{
  // commit all the user input fields
  if(response_id == GTK_RESPONSE_OK)
  {
    sqlite3_stmt *stmt;
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "UPDATE data.presets SET description = ?1, model = ?2, maker = ?3, lens = ?4, "
                                "iso_min = ?5, iso_max = ?6, exposure_min = ?7, exposure_max = ?8, "
                                "aperture_min = ?9, aperture_max = ?10, focal_length_min = ?11, "
                                "focal_length_max = ?12, autoapply = ?13, filter = ?14, def = 0, format = ?15 "
                                "WHERE rowid = ?16",
                                -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, gtk_entry_get_text(g->description), -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, gtk_entry_get_text(g->model), -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, gtk_entry_get_text(g->maker), -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 4, gtk_entry_get_text(g->lens), -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 5, gtk_spin_button_get_value(g->iso_min));
    DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 6, gtk_spin_button_get_value(g->iso_max));
    DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 7,
                                 dt_gui_presets_exposure_value[dt_bauhaus_combobox_get(g->exposure_min)]);
    DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 8,
                                 dt_gui_presets_exposure_value[dt_bauhaus_combobox_get(g->exposure_max)]);
    DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 9,
                                 dt_gui_presets_aperture_value[dt_bauhaus_combobox_get(g->aperture_min)]);
    DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 10,
                                 dt_gui_presets_aperture_value[dt_bauhaus_combobox_get(g->aperture_max)]);
    DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 11, gtk_spin_button_get_value(g->focal_length_min));
    DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 12, gtk_spin_button_get_value(g->focal_length_max));
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 13, gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g->autoapply)));
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 14, gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g->filter)));
    int format = 0;
    for(int k = 0; k < 5; k++)
      format += gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g->format_btn[k])) * dt_gui_presets_format_flag[k];
    format ^= DT_PRESETS_FOR_NOT;
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 15, format);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 16, g->rowid);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }
  else if(response_id == GTK_RESPONSE_YES)
  {
    const gchar *name = gtk_label_get_text(g->name);

    // ask for destination directory

    GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
    GtkWidget *filechooser = gtk_file_chooser_dialog_new(
      _("select directory"), GTK_WINDOW(win), GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER, _("_cancel"),
      GTK_RESPONSE_CANCEL, _("_select as output destination"), GTK_RESPONSE_ACCEPT, (char *)NULL);
#ifdef GDK_WINDOWING_QUARTZ
    dt_osx_disallow_fullscreen(filechooser);
#endif

    // save if accepted

    if(gtk_dialog_run(GTK_DIALOG(filechooser)) == GTK_RESPONSE_ACCEPT)
    {
      char *filedir = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(filechooser));
      dt_presets_save_to_file(g->rowid, name, filedir);
      dt_control_log(_("preset %s was successfully saved"), name);
      g_free(filedir);
    }

    gtk_widget_destroy(GTK_WIDGET(filechooser));
  }
  else if(response_id == GTK_RESPONSE_REJECT)
  {
    const gchar *name = gtk_label_get_text(g->name);

    sqlite3_stmt *stmt;
    gchar* operation = NULL;

      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT name, operation FROM data.presets WHERE rowid = ?1",
                              -1, &stmt, NULL);
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, g->rowid);
      if(sqlite3_step(stmt) == SQLITE_ROW)
      {
        operation = g_strdup( (const char*)sqlite3_column_text(stmt,1));
      }
      sqlite3_finalize(stmt);

      GtkWidget *win = gtk_message_dialog_new
        (GTK_WINDOW(_preferences_dialog), GTK_DIALOG_DESTROY_WITH_PARENT | GTK_DIALOG_MODAL,
         GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO,
         _("do you really want to delete the preset `%s'?"), name);
#ifdef GDK_WINDOWING_QUARTZ
      dt_osx_disallow_fullscreen(win);
#endif
      gtk_window_set_title(GTK_WINDOW(win), _("delete preset?"));
      if(gtk_dialog_run(GTK_DIALOG(win)) == GTK_RESPONSE_YES)
      {
        //deregistering accel...
        if(operation)
        {
          gchar accel[256];
          gchar datadir[PATH_MAX] = { 0 };
          gchar accelpath[PATH_MAX] = { 0 };

          dt_loc_get_user_config_dir(datadir, sizeof(datadir));
          snprintf(accelpath, sizeof(accelpath), "%s/keyboardrc", datadir);

          gchar *preset_name = g_strdup_printf("%s`%s", N_("preset"), name);
          dt_accel_path_iop(accel, sizeof(accel), operation, preset_name);
          g_free(preset_name);

          gtk_accel_map_change_entry(accel, 0, 0, TRUE);

          // Saving the changed bindings
          gtk_accel_map_save(accelpath);
        }

        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                    "DELETE FROM data.presets WHERE rowid=?1 AND writeprotect=0", -1, &stmt, NULL);
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, g->rowid);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
      }
      gtk_widget_destroy(win);
      if(operation)
        g_free(operation);
  }

  GtkTreeStore *tree_store = GTK_TREE_STORE(gtk_tree_view_get_model(g->tree));
  gtk_tree_store_clear(tree_store);
  tree_insert_presets(tree_store);

  gtk_widget_destroy(GTK_WIDGET(dialog));
  free(g);
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
