/*
    This file is part of darktable,
    Copyright (C) 2010-2021 darktable developers.

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
#include "common/camera_control.h"
#include "common/darktable.h"
#include "control/conf.h"
#include "control/control.h"
#include "control/jobs.h"
#include "dtgtk/button.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include "libs/lib_api.h"
#include <gdk/gdkkeysyms.h>

DT_MODULE(1)

typedef struct dt_lib_camera_property_t
{
  /** the visual property name */
  gchar *name;
  /** the property name */
  gchar *property_name;
  /**Combobox of values available for the property*/
  GtkWidget *values;
  /** Show property OSD */
  GtkDarktableToggleButton *osd;
} dt_lib_camera_property_t;

typedef struct dt_lib_camera_t
{
  /** Gui part of the module */
  struct
  {
    GtkGrid *main_grid;
    GtkDarktableToggleButton *toggle_timer, *toggle_sequence, *toggle_bracket;
    GtkWidget *timer, *count, *brackets, *steps;
    GtkWidget *button1;

    int rows; // the number of row in the grid
    int prop_start; // the row of the grid above the first property
    int prop_end; // the row of the grid where to insert new properties

    GtkWidget *plabel, *pname; // propertylabel,widget
    GList *properties;         // a list of dt_lib_camera_property_t

    GtkMenu *properties_menu; // available properties

  } gui;

  /** Data part of the module */
  struct
  {
    const gchar *camera_model;
    dt_camctl_listener_t *listener;
  } data;
} dt_lib_camera_t;



const char *name(dt_lib_module_t *self)
{
  return _("Camera settings");
}

const char **views(dt_lib_module_t *self)
{
  static const char *v[] = {"tethering", NULL};
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_RIGHT_CENTER;
}


void gui_reset(dt_lib_module_t *self)
{
}

int position(const dt_lib_module_t *self)
{
  return 997;
}

/** Property changed*/
static void property_changed_callback(GtkComboBox *cb, gpointer data)
{
  dt_lib_camera_property_t *prop = (dt_lib_camera_property_t *)data;
  dt_camctl_camera_set_property_string(darktable.camctl, NULL, prop->property_name,
                                       dt_bauhaus_combobox_get_text(prop->values));
}

/** Add  a new property of camera to the gui */
static dt_lib_camera_property_t *_lib_property_add_new(dt_lib_camera_t *lib, const gchar *label,
                                                       const gchar *propertyname)
{
  if(dt_camctl_camera_property_exists(darktable.camctl, NULL, propertyname))
  {
    const char *value;
    if((value = dt_camctl_camera_property_get_first_choice(darktable.camctl, NULL, propertyname)) != NULL)
    {
      // We got a value for property lets construct the gui for the property and add values
      int i = 0;
      const char *current_value = dt_camctl_camera_get_property(darktable.camctl, NULL, propertyname);
      dt_lib_camera_property_t *prop = calloc(1, sizeof(dt_lib_camera_property_t));
      prop->name = strdup(label);
      prop->property_name = strdup(propertyname);
      prop->values = dt_bauhaus_combobox_new(NULL);
      dt_bauhaus_widget_set_label(prop->values, NULL, label);
      g_object_ref_sink(prop->values);

      prop->osd = DTGTK_TOGGLEBUTTON(dtgtk_togglebutton_new(dtgtk_cairo_paint_eye, 0, NULL));
      dt_gui_add_class(GTK_WIDGET(prop->osd), "dt_transparent_background");
      g_object_ref_sink(prop->osd);
      gtk_widget_set_tooltip_text(GTK_WIDGET(prop->osd), _("Toggle view property in center view"));
      do
      {
        dt_bauhaus_combobox_add(prop->values, g_dgettext("libgphoto2-6", value));
        if(!strcmp(current_value, g_dgettext("libgphoto2-6", value)))
          dt_bauhaus_combobox_set(prop->values, i);
        i++;
      } while((value = dt_camctl_camera_property_get_next_choice(darktable.camctl, NULL, propertyname))
              != NULL);
      lib->gui.properties = g_list_append(lib->gui.properties, prop);
      // Does dead lock!!!
      g_signal_connect(G_OBJECT(prop->values), "value-changed", G_CALLBACK(property_changed_callback),
                       (gpointer)prop);
      return prop;
    }
  }
  return NULL;
}

static void _lib_property_free(gpointer data)
{
  dt_lib_camera_property_t * prop = (dt_lib_camera_property_t *)data;
  g_object_unref(prop->osd);
  g_object_unref(prop->values);
  free(prop->name);
  free(prop->property_name);
}

static gint _compare_property_by_name(gconstpointer a, gconstpointer b)
{
  dt_lib_camera_property_t *ca = (dt_lib_camera_property_t *)a;
  return strcmp(ca->property_name, (char *)b);
}

/** Invoked when a value of a property is changed. */
static void _camera_property_value_changed(const dt_camera_t *camera, const char *name, const char *value,
                                           void *data)
{
  dt_lib_camera_t *lib = (dt_lib_camera_t *)data;
  // Find the property in lib->data.properties, update value
  GList *citem;
  if((citem = g_list_find_custom(lib->gui.properties, name, _compare_property_by_name)) != NULL)
  {
    dt_lib_camera_property_t *prop = (dt_lib_camera_property_t *)citem->data;
    dt_bauhaus_combobox_set_from_text(prop->values, value);
  }
}

/** Invoked when accessibility of a property is changed. */
static void _camera_property_accessibility_changed(const dt_camera_t *camera, const char *name,
                                                   gboolean read_only, void *data)
{
}

static gboolean _bailout_of_tethering(gpointer user_data)
{
  /* consider all error types as failure and bailout of tethering mode */
  dt_lib_camera_t *lib = (dt_lib_camera_t *)user_data;
  dt_camctl_tether_mode(darktable.camctl, NULL, FALSE);
  dt_camctl_unregister_listener(darktable.camctl, lib->data.listener);

  /* switch back to library mode */
  dt_ctl_switch_mode_to("lighttable");

  return FALSE;
}

/** Invoked when camera error appear */
static void _camera_error_callback(const dt_camera_t *camera, dt_camera_error_t error, void *user_data)
{
  dt_control_log(_("Connection with camera lost, exiting tethering mode"));
  g_idle_add(_bailout_of_tethering, user_data);
}

static void _capture_button_clicked(GtkWidget *widget, gpointer user_data)
{
  const char *jobcode = NULL;
  const dt_lib_camera_t *lib = (dt_lib_camera_t *)user_data;
  const uint32_t delay = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(lib->gui.toggle_timer)) == TRUE
                             ? (uint32_t)gtk_spin_button_get_value(GTK_SPIN_BUTTON(lib->gui.timer))
                             : 0;
  const uint32_t count = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(lib->gui.toggle_sequence)) == TRUE
                             ? (uint32_t)gtk_spin_button_get_value(GTK_SPIN_BUTTON(lib->gui.count))
                             : 1;
  const uint32_t brackets = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(lib->gui.toggle_bracket)) == TRUE
                                ? (uint32_t)gtk_spin_button_get_value(GTK_SPIN_BUTTON(lib->gui.brackets))
                                : 0;
  const uint32_t steps = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(lib->gui.toggle_bracket)) == TRUE
                             ? (uint32_t)gtk_spin_button_get_value(GTK_SPIN_BUTTON(lib->gui.steps))
                             : 0;

  /* create a capture background job */
  jobcode = dt_view_tethering_get_job_code(darktable.view_manager);
  dt_control_add_job(darktable.control, DT_JOB_QUEUE_USER_FG,
                     dt_camera_capture_job_create(jobcode, delay, count, brackets, steps));
}


static void _osd_button_clicked(GtkWidget *widget, gpointer user_data)
{
  dt_control_queue_redraw_center();
}

static void _property_choice_callback(GtkMenuItem *item, gpointer user_data)
{
  dt_lib_camera_t *lib = (dt_lib_camera_t *)user_data;
  gtk_entry_set_text(GTK_ENTRY(lib->gui.pname), gtk_menu_item_get_label(item));
}


static void _show_property_popupmenu_clicked(GtkWidget *widget, gpointer user_data)
{
  dt_lib_camera_t *lib = (dt_lib_camera_t *)user_data;

  dt_gui_menu_popup(lib->gui.properties_menu, widget, GDK_GRAVITY_SOUTH_EAST, GDK_GRAVITY_NORTH_EAST);
}

static void _lib_property_add_to_gui(dt_lib_camera_property_t *prop, dt_lib_camera_t *lib)
{
  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_PIXEL_APPLY_DPI(5));
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(prop->values), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(prop->osd), FALSE, FALSE, 0);
  gtk_grid_insert_row(lib->gui.main_grid, lib->gui.prop_end); // make space for the new row
  gtk_grid_attach(lib->gui.main_grid, GTK_WIDGET(hbox), 0, lib->gui.prop_end, 2, 1);
  g_signal_connect(G_OBJECT(prop->osd), "clicked", G_CALLBACK(_osd_button_clicked), prop);
  gtk_widget_show_all(GTK_WIDGET(hbox));
  lib->gui.rows++;
  lib->gui.prop_end++;
}

static void _add_property_button_clicked(GtkWidget *widget, gpointer user_data)
{
  dt_lib_camera_t *lib = (dt_lib_camera_t *)user_data;
  const gchar *label = gtk_entry_get_text(GTK_ENTRY(lib->gui.plabel));
  const gchar *property = gtk_entry_get_text(GTK_ENTRY(lib->gui.pname));

  /* let's try to add property */
  if(label && property)
  {
    dt_lib_camera_property_t *prop = NULL;

    if((prop = _lib_property_add_new(lib, label, property)) != NULL)
    {
      _lib_property_add_to_gui(prop, lib);

      gchar key[256] = { "plugins/capture/tethering/properties/" };
      g_strlcat(key, label, sizeof(key));
      gchar *p = key;
      const char *end = key + strlen(key);
      while(p++ < end)
        if(*p == ' ') *p = '_';
      dt_conf_set_string(key, property);

      /* clean entries */
      gtk_entry_set_text(GTK_ENTRY(lib->gui.plabel), "");
      gtk_entry_set_text(GTK_ENTRY(lib->gui.pname), "");
    }
  }
}


static void _toggle_capture_mode_clicked(GtkWidget *widget, gpointer user_data)
{
  dt_lib_camera_t *lib = (dt_lib_camera_t *)user_data;
  GtkWidget *w = NULL;
  if(widget == GTK_WIDGET(lib->gui.toggle_timer))
    w = lib->gui.timer;
  else if(widget == GTK_WIDGET(lib->gui.toggle_sequence))
    w = lib->gui.count;
  else if(widget == GTK_WIDGET(lib->gui.toggle_bracket))
  {
    gtk_widget_set_sensitive(lib->gui.brackets, gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget)));
    gtk_widget_set_sensitive(lib->gui.steps, gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget)));
  }

  if(w) gtk_widget_set_sensitive(w, gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget)));
}


#define BAR_HEIGHT DT_PIXEL_APPLY_DPI(18) /* also change in views/tethering.c */
static void _expose_info_bar(dt_lib_module_t *self, cairo_t *cr, int32_t width, int32_t height,
                             int32_t pointerx, int32_t pointery)
{
  dt_lib_camera_t *lib = (dt_lib_camera_t *)self->data;

  // Draw infobar background at top
  cairo_set_source_rgb(cr, .0, .0, .0);
  cairo_rectangle(cr, 0, 0, width, BAR_HEIGHT);
  cairo_fill(cr);

  cairo_set_source_rgb(cr, .8, .8, .8);

  // Draw left aligned value camera model value
  PangoLayout *layout;
  PangoRectangle ink;
  PangoFontDescription *desc = pango_font_description_copy_static(darktable.bauhaus->pango_font_desc);
  pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
  layout = pango_cairo_create_layout(cr);
  const int fontsize = DT_PIXEL_APPLY_DPI(11.5);
  pango_font_description_set_absolute_size(desc, fontsize * PANGO_SCALE);
  pango_layout_set_font_description(layout, desc);
  char model[4096] = { 0 };
  g_strlcpy(model, lib->data.camera_model, strlen(model));
  pango_layout_set_text(layout, model, -1);
  pango_layout_get_pixel_extents(layout, &ink, NULL);
  cairo_move_to(cr, DT_PIXEL_APPLY_DPI(5), DT_PIXEL_APPLY_DPI(1) + BAR_HEIGHT - ink.height / 2 - fontsize);
  pango_cairo_show_layout(cr, layout);

  // Draw right aligned battery value
  const char *battery_value = dt_camctl_camera_get_property(darktable.camctl, NULL, "batterylevel");
  char battery[4096] = { 0 };
  snprintf(battery, sizeof(battery), "%s: %s", _("Battery"), battery_value ? battery_value : _("N/a"));
  pango_layout_set_text(layout, battery, -1);
  pango_layout_get_pixel_extents(layout, &ink, NULL);
  cairo_move_to(cr, width - ink.width - DT_PIXEL_APPLY_DPI(5), DT_PIXEL_APPLY_DPI(1) + BAR_HEIGHT - ink.height / 2 - fontsize);
  pango_cairo_show_layout(cr, layout);

  // Let's cook up the middle part of infobar
  gchar center[1024] = { 0 };
  for(GList *l = lib->gui.properties; l; l = g_list_next(l))
  {
    dt_lib_camera_property_t *prop = (dt_lib_camera_property_t *)l->data;
    if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(prop->osd)) == TRUE)
    {
      g_strlcat(center, "      ", sizeof(center));
      g_strlcat(center, prop->name, sizeof(center));
      g_strlcat(center, ": ", sizeof(center));
      g_strlcat(center, dt_bauhaus_combobox_get_text(prop->values), sizeof(center));
    }
  }
  g_strlcat(center, "      ", sizeof(center));

  // Now lets put it in center view...
  pango_layout_set_text(layout, center, -1);
  pango_layout_get_pixel_extents(layout, &ink, NULL);
  cairo_move_to(cr, (width / 2) - (ink.width / 2), DT_PIXEL_APPLY_DPI(1) + BAR_HEIGHT - ink.height / 2 - fontsize);
  pango_cairo_show_layout(cr, layout);
  pango_font_description_free(desc);
  g_object_unref(layout);
}

static void _expose_settings_bar(dt_lib_module_t *self, cairo_t *cr, int32_t width, int32_t height,
                                 int32_t pointerx, int32_t pointery)
{
  /*// Draw control bar at bottom
  cairo_set_source_rgb (cr, .0,.0,.0);
  cairo_rectangle(cr, 0, height-BAR_HEIGHT, width, BAR_HEIGHT);
  cairo_fill (cr);*/
}

void gui_post_expose(dt_lib_module_t *self, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx,
                     int32_t pointery)
{
  // Setup cairo font..
  cairo_set_font_size(cr, 11.5);
  //cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);

  _expose_info_bar(self, cr, width, height, pointerx, pointery);
  _expose_settings_bar(self, cr, width, height, pointerx, pointery);
}

void gui_init(dt_lib_module_t *self)
{
  self->data = calloc(1, sizeof(dt_lib_camera_t));

  // Setup lib data
  dt_lib_camera_t *lib = self->data;
  lib->data.listener = calloc(1, sizeof(dt_camctl_listener_t));
  lib->data.listener->data = lib;
  lib->data.listener->camera_error = _camera_error_callback;
  lib->data.listener->camera_property_value_changed = _camera_property_value_changed;
  lib->data.listener->camera_property_accessibility_changed = _camera_property_accessibility_changed;

  // Setup gui
  lib->gui.rows = 0;
  lib->gui.prop_end = 0;
  self->widget = gtk_grid_new();
  gtk_grid_set_column_spacing(GTK_GRID(self->widget), DT_PIXEL_APPLY_DPI(5));
  lib->gui.main_grid = GTK_GRID(self->widget);
  gtk_grid_set_row_spacing(GTK_GRID(self->widget), DT_PIXEL_APPLY_DPI(5));

  gtk_grid_set_column_homogeneous(GTK_GRID(self->widget), FALSE);

  GtkBox *hbox;

  // Camera control
  GtkWidget *label = dt_ui_section_label_new(_("Camera control"));
  gtk_widget_set_hexpand(label, TRUE);
  gtk_grid_attach(GTK_GRID(self->widget), label, lib->gui.rows++, 0, 2, 1);

  GtkWidget *modes_label = gtk_label_new(_("Modes"));
  GtkWidget *timer_label = gtk_label_new(_("Timer (s)"));
  GtkWidget *count_label = gtk_label_new(_("Count"));
  GtkWidget *brackets_label = gtk_label_new(_("Brackets"));
  GtkWidget *steps_label = gtk_label_new(_("Bkt. Steps"));
  gtk_widget_set_halign(GTK_WIDGET(modes_label), GTK_ALIGN_START);
  gtk_widget_set_halign(GTK_WIDGET(timer_label), GTK_ALIGN_START);
  gtk_widget_set_halign(GTK_WIDGET(count_label), GTK_ALIGN_START);
  gtk_widget_set_halign(GTK_WIDGET(brackets_label), GTK_ALIGN_START);
  gtk_widget_set_halign(GTK_WIDGET(steps_label), GTK_ALIGN_START);

  gtk_grid_attach(GTK_GRID(self->widget), GTK_WIDGET(modes_label), 0, lib->gui.rows++, 1, 1);
  gtk_grid_attach(GTK_GRID(self->widget), GTK_WIDGET(timer_label), 0, lib->gui.rows++, 1, 1);
  gtk_grid_attach(GTK_GRID(self->widget), GTK_WIDGET(count_label), 0, lib->gui.rows++, 1, 1);
  gtk_grid_attach(GTK_GRID(self->widget), GTK_WIDGET(brackets_label), 0, lib->gui.rows++, 1, 1);
  gtk_grid_attach(GTK_GRID(self->widget), GTK_WIDGET(steps_label), 0, lib->gui.rows++, 1, 1);

  // capture modes buttons
  lib->gui.toggle_timer = DTGTK_TOGGLEBUTTON(dtgtk_togglebutton_new(dtgtk_cairo_paint_timer, 0, NULL));
  lib->gui.toggle_sequence = DTGTK_TOGGLEBUTTON(dtgtk_togglebutton_new(dtgtk_cairo_paint_filmstrip, 0, NULL));
  lib->gui.toggle_bracket = DTGTK_TOGGLEBUTTON(dtgtk_togglebutton_new(dtgtk_cairo_paint_bracket, 0, NULL));

  hbox = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0));
  gtk_box_pack_start(hbox, GTK_WIDGET(lib->gui.toggle_timer), TRUE, TRUE, 0);
  gtk_box_pack_start(hbox, GTK_WIDGET(lib->gui.toggle_sequence), TRUE, TRUE, 0);
  gtk_box_pack_start(hbox, GTK_WIDGET(lib->gui.toggle_bracket), TRUE, TRUE, 0);
  gtk_grid_attach_next_to(GTK_GRID(self->widget), GTK_WIDGET(hbox), GTK_WIDGET(modes_label), GTK_POS_RIGHT, 1, 1);

  lib->gui.timer = gtk_spin_button_new_with_range(1, 60, 1);
  lib->gui.count = gtk_spin_button_new_with_range(1, 9999, 1);
  lib->gui.brackets = gtk_spin_button_new_with_range(1, 5, 1);
  lib->gui.steps = gtk_spin_button_new_with_range(1, 9, 1);
  gtk_grid_attach_next_to(GTK_GRID(self->widget), GTK_WIDGET(lib->gui.timer), GTK_WIDGET(timer_label), GTK_POS_RIGHT, 1, 1);
  gtk_grid_attach_next_to(GTK_GRID(self->widget), GTK_WIDGET(lib->gui.count), GTK_WIDGET(count_label), GTK_POS_RIGHT, 1, 1);
  gtk_grid_attach_next_to(GTK_GRID(self->widget), GTK_WIDGET(lib->gui.brackets), GTK_WIDGET(brackets_label), GTK_POS_RIGHT, 1, 1);
  gtk_grid_attach_next_to(GTK_GRID(self->widget), GTK_WIDGET(lib->gui.steps), GTK_WIDGET(steps_label), GTK_POS_RIGHT, 1, 1);

  lib->gui.button1 = dt_action_button_new(self, N_("Capture image(s)"), _capture_button_clicked, lib, NULL, 0, 0);
  gtk_grid_attach(GTK_GRID(self->widget), GTK_WIDGET(lib->gui.button1), 0, lib->gui.rows++, 2, 1);

  gtk_widget_set_tooltip_text(GTK_WIDGET(lib->gui.toggle_timer), _("Toggle delayed capture mode"));
  gtk_widget_set_tooltip_text(GTK_WIDGET(lib->gui.toggle_sequence), _("Toggle sequenced capture mode"));
  gtk_widget_set_tooltip_text(GTK_WIDGET(lib->gui.toggle_bracket), _("Toggle bracketed capture mode"));
  gtk_widget_set_tooltip_text(GTK_WIDGET(lib->gui.timer), _("The count of seconds before actually doing a capture"));
  gtk_widget_set_tooltip_text(GTK_WIDGET(lib->gui.count),
               _("The amount of images to capture in a sequence,\nyou can use this in conjunction with "
                 "delayed mode to create stop-motion sequences"));
  gtk_widget_set_tooltip_text(GTK_WIDGET(lib->gui.brackets),
               _("The amount of brackets on each side of centered shoot, amount of images = (brackets*2) + 1"));
  gtk_widget_set_tooltip_text(GTK_WIDGET(lib->gui.steps),
               _("The amount of steps per bracket, steps is camera configurable and usually 3 steps per "
                 "stop\nwith other words, 3 steps is 1EV exposure step between brackets"));

  g_signal_connect(G_OBJECT(lib->gui.toggle_timer), "clicked", G_CALLBACK(_toggle_capture_mode_clicked), lib);
  g_signal_connect(G_OBJECT(lib->gui.toggle_sequence), "clicked", G_CALLBACK(_toggle_capture_mode_clicked), lib);
  g_signal_connect(G_OBJECT(lib->gui.toggle_bracket), "clicked", G_CALLBACK(_toggle_capture_mode_clicked), lib);

  gtk_widget_set_sensitive(GTK_WIDGET(lib->gui.timer), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(lib->gui.count), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(lib->gui.brackets), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(lib->gui.steps), FALSE);



  // Camera settings
  label = dt_ui_section_label_new(_("Properties"));
  gtk_grid_attach(GTK_GRID(self->widget), GTK_WIDGET(label), 0, lib->gui.rows++, 2, 1);

  lib->gui.prop_start = lib->gui.rows -1;
  lib->gui.prop_end = lib->gui.rows;


  // user specified properties
  label = dt_ui_section_label_new(_("Additional properties"));
  gtk_grid_attach(GTK_GRID(self->widget), GTK_WIDGET(label), 0, lib->gui.rows++, 2, 1);

  label = gtk_label_new(_("Label"));
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  lib->gui.plabel = gtk_entry_new();
  gtk_entry_set_width_chars(GTK_ENTRY(lib->gui.plabel), 0);
  gtk_grid_attach(GTK_GRID(self->widget), GTK_WIDGET(label), 0, lib->gui.rows++, 1, 1);
  gtk_grid_attach_next_to(GTK_GRID(self->widget), GTK_WIDGET(lib->gui.plabel), GTK_WIDGET(label), GTK_POS_RIGHT, 1, 1);

  hbox = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0));
  label = gtk_label_new(_("Property"));
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  GtkWidget *widget = gtk_button_new_with_label("O");
  g_signal_connect(G_OBJECT(widget), "clicked", G_CALLBACK(_show_property_popupmenu_clicked), lib);
  lib->gui.pname = gtk_entry_new();
  gtk_entry_set_width_chars(GTK_ENTRY(lib->gui.pname), 0);
  gtk_box_pack_start(hbox, GTK_WIDGET(lib->gui.pname), TRUE, TRUE, 0);
  gtk_box_pack_start(hbox, GTK_WIDGET(widget), FALSE, FALSE, 0);
  gtk_grid_attach(GTK_GRID(self->widget), GTK_WIDGET(label), 0, lib->gui.rows++, 1, 1);
  gtk_grid_attach_next_to(GTK_GRID(self->widget), GTK_WIDGET(hbox), GTK_WIDGET(label), GTK_POS_RIGHT, 1, 1);


  widget = gtk_button_new_with_label(_("Add user property"));
  g_signal_connect(G_OBJECT(widget), "clicked", G_CALLBACK(_add_property_button_clicked), lib);
  gtk_widget_show(widget);
  gtk_grid_attach(GTK_GRID(self->widget), GTK_WIDGET(widget), 0, lib->gui.rows++, 2, 1);
}

void gui_cleanup(dt_lib_module_t *self)
{
  dt_lib_camera_t *lib = self->data;
  free(lib->data.listener);
  lib->data.listener = NULL;
  free(self->data);
  self->data = NULL;
}

void view_enter(struct dt_lib_module_t *self,struct dt_view_t *old_view,struct dt_view_t *new_view)
{
  dt_lib_camera_t *lib = self->data;
  /* add all camera properties to the widget */
  dt_lib_camera_property_t *prop;
  if((prop = _lib_property_add_new(lib, _("Program"), "expprogram")) != NULL)
    _lib_property_add_to_gui(prop, lib);

  if((prop = _lib_property_add_new(lib, _("Focus mode"), "focusmode")) != NULL)
    _lib_property_add_to_gui(prop, lib);
  else if((prop = _lib_property_add_new(lib, _("Focus mode"), "drivemode")) != NULL)
    _lib_property_add_to_gui(prop, lib);

  if((prop = _lib_property_add_new(lib, _("Aperture"), "f-number")) != NULL)
    _lib_property_add_to_gui(prop, lib);
  else if((prop = _lib_property_add_new(lib, _("Aperture"), "aperture")) != NULL) // for Canon cameras
    _lib_property_add_to_gui(prop, lib);

  if((prop = _lib_property_add_new(lib, _("Focal length"), "focallength")) != NULL)
    _lib_property_add_to_gui(prop, lib);

  if((prop = _lib_property_add_new(lib, _("Shutterspeed2"), "shutterspeed2")) != NULL)
    _lib_property_add_to_gui(prop, lib);
  else if((prop = _lib_property_add_new(lib, _("Shutterspeed"), "shutterspeed")) != NULL) // Canon, again
    _lib_property_add_to_gui(prop, lib);

  if((prop = _lib_property_add_new(lib, _("ISO"), "iso")) != NULL)
    _lib_property_add_to_gui(prop, lib);

  if((prop = _lib_property_add_new(lib, _("WB"), "whitebalance")) != NULL)
    _lib_property_add_to_gui(prop, lib);

  if((prop = _lib_property_add_new(lib, _("Quality"), "imagequality")) != NULL)
    _lib_property_add_to_gui(prop, lib);

  if((prop = _lib_property_add_new(lib, _("Size"), "imagesize")) != NULL)
    _lib_property_add_to_gui(prop, lib);

  /* add user widgets */
  GSList *options = dt_conf_all_string_entries("plugins/capture/tethering/properties");
  if(options)
  {
    for(GSList *item = options; item; item = g_slist_next(item))
    {
      dt_conf_string_entry_t *entry = (dt_conf_string_entry_t *)item->data;

      /* get the label from key */
      char *p = entry->key;
      const char *end = entry->key + strlen(entry->key);
      while(p++ < end)
        if(*p == '_') *p = ' ';

      if((prop = _lib_property_add_new(lib, entry->key, entry->value)) != NULL)
        _lib_property_add_to_gui(prop, lib);
    }
    g_slist_free_full(options, dt_conf_string_entry_free);
  }
  /* build the propertymenu  we do it now because it needs an actual camera */
  dt_camctl_camera_build_property_menu(darktable.camctl, NULL, &lib->gui.properties_menu,
                                       G_CALLBACK(_property_choice_callback), lib);

  // Register listener
  dt_camctl_register_listener(darktable.camctl, lib->data.listener);
  dt_camctl_tether_mode(darktable.camctl, NULL, TRUE);
  // Get camera model name
  lib->data.camera_model = dt_camctl_camera_get_model(darktable.camctl, NULL);
}
void view_leave(struct dt_lib_module_t *self,struct dt_view_t *old_view,struct dt_view_t *new_view)
{
  dt_lib_camera_t *lib = self->data;
  // remove listener from camera control..
  dt_camctl_tether_mode(darktable.camctl, NULL, FALSE);
  dt_camctl_unregister_listener(darktable.camctl, lib->data.listener);
  gtk_widget_destroy(GTK_WIDGET(lib->gui.properties_menu));
  lib->gui.properties_menu = NULL;
  // remove all properties
  while(lib->gui.prop_end > lib->gui.prop_start +1) {
    gtk_grid_remove_row(lib->gui.main_grid,lib->gui.prop_start +1);
    lib->gui.rows--;
    lib->gui.prop_end--;
  }
  // no need to free widgets, they are freed when the line of the grid is destroyed
  g_list_free_full(lib->gui.properties,_lib_property_free);
  lib->gui.properties = NULL;
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
