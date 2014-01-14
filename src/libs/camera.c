/*
    This file is part of darktable,
    copyright (c) 2010 - 2014 henrik andersson.

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
#include "common/darktable.h"
#include "common/camera_control.h"
#include "control/jobs.h"
#include "control/control.h"
#include "control/conf.h"
#include "libs/lib.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "dtgtk/label.h"
#include <gdk/gdkkeysyms.h>
#include "dtgtk/button.h"

DT_MODULE(1)

typedef struct dt_lib_camera_property_t
{
  /** label of property */
  GtkLabel *label;
  /** the visual property name */
  const gchar *name;
  /** the property name */
  const gchar *property_name;
  /**Combobox of values available for the property*/
  GtkComboBox *values;
  /** Show property OSD */
  GtkDarktableToggleButton *osd;
}
dt_lib_camera_property_t;

typedef struct dt_lib_camera_t
{
  /** Gui part of the module */
  struct
  {
    GtkWidget *label1,*label2,*label3,*label4,*label5;               // Capture modes, delay, sequenced, brackets, steps
    GtkDarktableToggleButton *tb1,*tb2,*tb3;        // Delayed capture, Sequenced capture, brackets
    GtkWidget *sb1,*sb2,*sb3,*sb4;                         // delay, sequence, brackets, steps
    GtkWidget *button1;

    GtkBox *pvbox1,*pvbox2;                  // propertylabel,widget
    GtkWidget *plabel,*pname;                  // propertylabel,widget
    GList *properties;                            // a list of dt_lib_camera_property_t

    GtkMenu *properties_menu;                     // available properties

  } gui;

  /** Data part of the module */
  struct
  {
    const gchar *camera_model;
    dt_camctl_listener_t *listener;
  } data;
}
dt_lib_camera_t;



const char*
name ()
{
  return _("camera settings");
}

uint32_t views()
{
  return DT_VIEW_TETHERING;
}

uint32_t container()
{
  return DT_UI_CONTAINER_PANEL_RIGHT_CENTER;
}


void
gui_reset (dt_lib_module_t *self)
{
}

int
position ()
{
  return 997;
}

void init_key_accels(dt_lib_module_t *self)
{
  dt_accel_register_lib(self, NC_("accel", "capture image(s)"), 0, 0);
}

void connect_key_accels(dt_lib_module_t *self)
{
  dt_lib_camera_t *lib = (dt_lib_camera_t*)self->data;

  dt_accel_connect_button_lib(self, "capture image(s)",
                              GTK_WIDGET(lib->gui.button1));
}

/** Property changed*/
void property_changed_callback(GtkComboBox *cb,gpointer data)
{
  dt_lib_camera_property_t *prop=(dt_lib_camera_property_t *)data;
  dt_camctl_camera_set_property_string(darktable.camctl,NULL,prop->property_name,gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(prop->values)));
}

/** Add  a new property of camera to the gui */
dt_lib_camera_property_t *_lib_property_add_new(dt_lib_camera_t * lib, const gchar *label,const gchar *propertyname)
{
  if( dt_camctl_camera_property_exists(darktable.camctl,NULL,propertyname) )
  {
    const char *value;
    if( (value=dt_camctl_camera_property_get_first_choice(darktable.camctl,NULL,propertyname)) != NULL )
    {
      // We got a value for property lets construct the gui for the property and add values
      int i=0;
      const char *current_value = dt_camctl_camera_get_property(darktable.camctl, NULL, propertyname);
      dt_lib_camera_property_t *prop = malloc(sizeof(dt_lib_camera_property_t));
      memset(prop,0,sizeof(dt_lib_camera_property_t));
      prop->name=label;
      prop->property_name=propertyname;
      prop->label = GTK_LABEL(gtk_label_new(label));
      gtk_misc_set_alignment(GTK_MISC(prop->label ), 0.0, 0.5);
      prop->values=GTK_COMBO_BOX(gtk_combo_box_text_new());

      prop->osd=DTGTK_TOGGLEBUTTON(dtgtk_togglebutton_new(dtgtk_cairo_paint_eye,0));
      g_object_set(G_OBJECT(prop->osd), "tooltip-text", _("toggle view property in center view"), (char *)NULL);
      do
      {
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(prop->values), g_dgettext("libgphoto2-2", value));
        if(!strcmp(current_value, g_dgettext("libgphoto2-2", value)))
          gtk_combo_box_set_active(prop->values, i);
        i++;
      }
      while( (value=dt_camctl_camera_property_get_next_choice(darktable.camctl,NULL,propertyname)) != NULL );
      lib->gui.properties=g_list_append(lib->gui.properties,prop);
      // Does dead lock!!!
      g_signal_connect(G_OBJECT(prop->values), "changed", G_CALLBACK(property_changed_callback), (gpointer)prop);
      return prop;
    }
  }
  return NULL;
}


gint _compare_property_by_name(gconstpointer a,gconstpointer b)
{
  dt_lib_camera_property_t *ca=(dt_lib_camera_property_t *)a;
  return strcmp(ca->property_name,(char *)b);
}

/** Invoked when a value of a property is changed. */
static void _camera_property_value_changed(const dt_camera_t *camera,const char *name,const char *value,void *data)
{
  dt_lib_camera_t *lib=(dt_lib_camera_t *)data;
  // Find the property in lib->data.properties, update value
  GList *citem;
  if( (citem=g_list_find_custom(lib->gui.properties,name,_compare_property_by_name)) != NULL )
  {
    dt_lib_camera_property_t *prop=(dt_lib_camera_property_t*)citem->data;
    GtkTreeModel *model=gtk_combo_box_get_model(prop->values);
    GtkTreeIter iter;
    uint32_t index=0;
    if( gtk_tree_model_get_iter_first(model,&iter)==TRUE )
      do
      {
        gchar *str;
        gtk_tree_model_get(model,&iter,0,&str,-1);
        if(strcmp(str,value)==0)
        {
          gtk_combo_box_set_active(prop->values,index);

          break;
        }
        index++;
      }
      while( gtk_tree_model_iter_next(model,&iter) == TRUE);
  }
}

/** Invoked when accesibility of a property is changed. */
static void _camera_property_accessibility_changed(const dt_camera_t *camera,const char *name,gboolean read_only,void *data)
{
}

gboolean _bailout_of_tethering(gpointer user_data)
{
  /* consider all error types as failure and bailout of tethering mode */
  dt_lib_camera_t *lib= (dt_lib_camera_t *)user_data;
  dt_camctl_tether_mode (darktable.camctl,NULL,FALSE);
  dt_camctl_unregister_listener (darktable.camctl,lib->data.listener);

  /* switch back to library mode */
  dt_ctl_switch_mode_to (DT_LIBRARY);

  return FALSE;
}

/** Invoked when camera error appear */
static void _camera_error_callback(const dt_camera_t *camera,dt_camera_error_t error,void *user_data)
{
  dt_control_log(_("connection with camera lost, exiting tethering mode"));
  g_idle_add(_bailout_of_tethering,user_data);
}

static void
_capture_button_clicked(GtkWidget *widget, gpointer user_data)
{
  const char *jobcode = NULL;
  dt_lib_camera_t *lib=(dt_lib_camera_t *)user_data;
  uint32_t delay = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(lib->gui.tb1))==TRUE?(uint32_t)gtk_spin_button_get_value(GTK_SPIN_BUTTON(lib->gui.sb1)):0;
  uint32_t count = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(lib->gui.tb2))==TRUE?(uint32_t)gtk_spin_button_get_value(GTK_SPIN_BUTTON(lib->gui.sb2)):1;
  uint32_t brackets = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(lib->gui.tb3))==TRUE?(uint32_t)gtk_spin_button_get_value(GTK_SPIN_BUTTON(lib->gui.sb3)):0;
  uint32_t steps = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(lib->gui.tb3))==TRUE?(uint32_t)gtk_spin_button_get_value(GTK_SPIN_BUTTON(lib->gui.sb4)):0;
  dt_job_t j;

  /* create a capture background job */
  jobcode = dt_view_tethering_get_job_code(darktable.view_manager);
  dt_camera_capture_job_init(&j, jobcode, delay, count, brackets, steps);
  dt_control_add_job(darktable.control, &j);
}


static void _osd_button_clicked(GtkWidget *widget, gpointer user_data)
{
  dt_control_queue_redraw_center();
}

static void _property_choice_callback(GtkMenuItem *item, gpointer user_data)
{
  dt_lib_camera_t *lib=(dt_lib_camera_t *)user_data;
  gtk_entry_set_text(GTK_ENTRY (lib->gui.pname), gtk_menu_item_get_label(item));
}


static void
_show_property_popupmenu_clicked(GtkWidget *widget, gpointer user_data)
{
  dt_lib_camera_t *lib=(dt_lib_camera_t *)user_data;
  gtk_menu_popup (lib->gui.properties_menu, NULL, NULL, NULL, NULL, 1,  gtk_get_current_event_time());
}

static void
_add_property_button_clicked (GtkWidget *widget, gpointer user_data)
{
  dt_lib_camera_t *lib=(dt_lib_camera_t *)user_data;
  const gchar *label = gtk_entry_get_text (GTK_ENTRY (lib->gui.plabel));
  const gchar *property = gtk_entry_get_text (GTK_ENTRY (lib->gui.pname));

  /* let's try to add property */
  if ( label && property )
  {
    dt_lib_camera_property_t *prop = NULL;
    if ((prop=_lib_property_add_new (lib, label,property)) != NULL)
    {
      GtkBox *hbox = GTK_BOX(gtk_hbox_new(FALSE, 0));
      gtk_box_pack_start (GTK_BOX (hbox), GTK_WIDGET (prop->values), TRUE, TRUE, 0);
      gtk_box_pack_start (GTK_BOX (hbox), GTK_WIDGET (prop->osd), FALSE, FALSE, 0);
      gtk_box_pack_start (lib->gui.pvbox1, GTK_WIDGET (prop->label), TRUE, TRUE, 0);
      gtk_box_pack_start (lib->gui.pvbox2, GTK_WIDGET (hbox), TRUE, TRUE, 0);
      g_signal_connect (G_OBJECT (prop->osd), "clicked", G_CALLBACK (_osd_button_clicked), prop);

      gchar key[256]= {"plugins/capture/tethering/properties/"};
      g_strlcat(key, label, 256);
      gchar *p = key;
      while( p++<key+strlen(key) ) if(*p==' ') *p='_';
      dt_conf_set_string(key,property);

      /* clean entries */
      gtk_entry_set_text (GTK_ENTRY (lib->gui.plabel),"");
      gtk_entry_set_text (GTK_ENTRY (lib->gui.pname),"");

      gtk_widget_show_all (GTK_WIDGET (prop->label));
      gtk_widget_show_all (GTK_WIDGET (hbox));
    }

  }
}


static void _toggle_capture_mode_clicked(GtkWidget *widget, gpointer user_data)
{
  dt_lib_camera_t *lib=(dt_lib_camera_t *)user_data;
  GtkWidget *w=NULL;
  if( widget == GTK_WIDGET(lib->gui.tb1) )
    w = lib->gui.sb1;
  else if( widget == GTK_WIDGET(lib->gui.tb2) )
    w = lib->gui.sb2;
  else if( widget == GTK_WIDGET(lib->gui.tb3) )
  {
    gtk_widget_set_sensitive( lib->gui.sb3, gtk_toggle_button_get_active( GTK_TOGGLE_BUTTON(widget) ) );
    gtk_widget_set_sensitive( lib->gui.sb4, gtk_toggle_button_get_active( GTK_TOGGLE_BUTTON(widget) ) );
  }

  if(w)
    gtk_widget_set_sensitive( w, gtk_toggle_button_get_active( GTK_TOGGLE_BUTTON(widget) ) );

}


#define BAR_HEIGHT 18 /* also change in views/capture.c */
static void _expose_info_bar(dt_lib_module_t *self, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx, int32_t pointery)
{
  dt_lib_camera_t *lib=(dt_lib_camera_t *)self->data;

  // Draw infobar background at top
  cairo_set_source_rgb (cr, .0,.0,.0);
  cairo_rectangle(cr, 0, 0, width, BAR_HEIGHT);
  cairo_fill (cr);

  cairo_set_source_rgb (cr,.8,.8,.8);

  // Draw left aligned value camera model value
  cairo_text_extents_t te;
  char model[4096]= {0};
  sprintf(model+strlen(model),"%s", lib->data.camera_model );
  cairo_text_extents (cr, model, &te);
  cairo_move_to (cr,5, 1+BAR_HEIGHT - te.height / 2 );
  cairo_show_text(cr, model);

  // Draw right aligned battery value
  const char *battery_value=dt_camctl_camera_get_property(darktable.camctl,NULL,"batterylevel");
  char battery[4096]= {0};
  sprintf(battery,"%s: %s", _("battery"), battery_value?battery_value:_("n/a"));
  cairo_text_extents (cr, battery, &te);
  cairo_move_to(cr,width-te.width-5, 1+BAR_HEIGHT - te.height / 2 );
  cairo_show_text(cr, battery);

  // Let's cook up the middle part of infobar
  gchar center[1024]= {0};
  for(guint i=0; i<g_list_length(lib->gui.properties); i++)
  {
    dt_lib_camera_property_t *prop=(dt_lib_camera_property_t *)g_list_nth_data(lib->gui.properties,i);
    if( gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(prop->osd)) == TRUE )
    {
      g_strlcat(center,"      ",1024);
      g_strlcat(center,prop->name,1024);
      g_strlcat(center,": ",1024);
      g_strlcat(center,gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(prop->values)),1024);
    }
  }
  g_strlcat(center,"      ",1024);

  // Now lets put it in center view...
  cairo_text_extents (cr, center, &te);
  cairo_move_to(cr,(width/2)-(te.width/2), 1+BAR_HEIGHT - te.height / 2 );
  cairo_show_text(cr, center);


}

static void _expose_settings_bar(dt_lib_module_t *self, cairo_t *cr,int32_t width, int32_t height, int32_t pointerx, int32_t pointery)
{
  /*// Draw control bar at bottom
  cairo_set_source_rgb (cr, .0,.0,.0);
  cairo_rectangle(cr, 0, height-BAR_HEIGHT, width, BAR_HEIGHT);
  cairo_fill (cr);*/
}

void
gui_post_expose(dt_lib_module_t *self, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx, int32_t pointery)
{
  // Setup cairo font..
  cairo_set_font_size (cr, 11.5);
  cairo_select_font_face (cr, "Sans",CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);

  _expose_info_bar(self,cr,width,height,pointerx,pointery);
  _expose_settings_bar(self,cr,width,height,pointerx,pointery);
}

void
gui_init (dt_lib_module_t *self)
{
  self->widget = gtk_vbox_new(FALSE, 5);
  self->data = malloc(sizeof(dt_lib_camera_t));
  memset(self->data,0,sizeof(dt_lib_camera_t));

  // Setup lib data
  dt_lib_camera_t *lib=self->data;
  lib->data.listener = malloc(sizeof(dt_camctl_listener_t));
  memset(lib->data.listener,0,sizeof(dt_camctl_listener_t));
  lib->data.listener->data=lib;
  lib->data.listener->camera_error=_camera_error_callback;
  lib->data.listener->camera_property_value_changed=_camera_property_value_changed;
  lib->data.listener->camera_property_accessibility_changed=_camera_property_accessibility_changed;

  // Setup gui
  self->widget = gtk_vbox_new(FALSE, 5);
  GtkBox *hbox, *vbox1, *vbox2;

  // Camera control
  gtk_box_pack_start(GTK_BOX(self->widget), dtgtk_label_new(_("camera control"),DARKTABLE_LABEL_TAB|DARKTABLE_LABEL_ALIGN_RIGHT), TRUE, TRUE, 5);
  vbox1 = GTK_BOX(gtk_vbox_new(TRUE, 0));
  vbox2 = GTK_BOX(gtk_vbox_new(TRUE, 0));

  lib->gui.label1=gtk_label_new(_("modes"));
  lib->gui.label2=gtk_label_new(_("timer (s)"));
  lib->gui.label3=gtk_label_new(_("count"));
  lib->gui.label4=gtk_label_new(_("brackets"));
  lib->gui.label5=gtk_label_new(_("bkt. steps"));
  gtk_misc_set_alignment(GTK_MISC(lib->gui.label1 ), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(lib->gui.label2 ), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(lib->gui.label3 ), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(lib->gui.label4 ), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(lib->gui.label5 ), 0.0, 0.5);
  gtk_box_pack_start(vbox1, GTK_WIDGET(lib->gui.label1), FALSE, FALSE, 0);
  gtk_box_pack_start(vbox1, GTK_WIDGET(lib->gui.label2), FALSE, FALSE, 0);
  gtk_box_pack_start(vbox1, GTK_WIDGET(lib->gui.label3), FALSE, FALSE, 0);
  gtk_box_pack_start(vbox1, GTK_WIDGET(lib->gui.label4), FALSE, FALSE, 0);
  gtk_box_pack_start(vbox1, GTK_WIDGET(lib->gui.label5), FALSE, FALSE, 0);

  // capture modes buttons
  lib->gui.tb1=DTGTK_TOGGLEBUTTON(dtgtk_togglebutton_new(dtgtk_cairo_paint_timer,CPF_STYLE_FLAT|CPF_DO_NOT_USE_BORDER));
  lib->gui.tb2=DTGTK_TOGGLEBUTTON(dtgtk_togglebutton_new(dtgtk_cairo_paint_filmstrip,CPF_STYLE_FLAT|CPF_DO_NOT_USE_BORDER));
  lib->gui.tb3=DTGTK_TOGGLEBUTTON(dtgtk_togglebutton_new(dtgtk_cairo_paint_bracket,CPF_STYLE_FLAT|CPF_DO_NOT_USE_BORDER));

  hbox = GTK_BOX(gtk_hbox_new(TRUE, 5));
  gtk_box_pack_start(hbox, GTK_WIDGET(lib->gui.tb1), TRUE, TRUE, 0);
  gtk_box_pack_start(hbox, GTK_WIDGET(lib->gui.tb2), TRUE, TRUE, 0);
  gtk_box_pack_start(hbox, GTK_WIDGET(lib->gui.tb3), TRUE, TRUE, 0);
  gtk_box_pack_start(vbox2, GTK_WIDGET(hbox),FALSE, FALSE, 0);

  lib->gui.sb1=gtk_spin_button_new_with_range(1,60,1);
  lib->gui.sb2=gtk_spin_button_new_with_range(1,500,1);
  lib->gui.sb3=gtk_spin_button_new_with_range(1,5,1);
  lib->gui.sb4=gtk_spin_button_new_with_range(1,9,1);
  gtk_box_pack_start(vbox2, GTK_WIDGET(lib->gui.sb1), TRUE, TRUE, 0);
  gtk_box_pack_start(vbox2, GTK_WIDGET(lib->gui.sb2), TRUE, TRUE, 0);
  gtk_box_pack_start(vbox2, GTK_WIDGET(lib->gui.sb3), TRUE, TRUE, 0);
  gtk_box_pack_start(vbox2, GTK_WIDGET(lib->gui.sb4), TRUE, TRUE, 0);

  hbox = GTK_BOX(gtk_hbox_new(FALSE, 0));
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(vbox1), FALSE, FALSE, 5);
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(vbox2), TRUE, TRUE, 5);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hbox), FALSE, FALSE, 5);
  lib->gui.button1=gtk_button_new_with_label(_("capture image(s)"));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(lib->gui.button1), FALSE, FALSE, 5);

  g_object_set(G_OBJECT(lib->gui.tb1), "tooltip-text", _("toggle delayed capture mode"), (char *)NULL);
  g_object_set(G_OBJECT( lib->gui.tb2), "tooltip-text", _("toggle sequenced capture mode"), (char *)NULL);
  g_object_set(G_OBJECT( lib->gui.tb3), "tooltip-text", _("toggle bracketed capture mode"), (char *)NULL);
  g_object_set(G_OBJECT( lib->gui.sb1), "tooltip-text", _("the count of seconds before actually doing a capture"), (char *)NULL);
  g_object_set(G_OBJECT( lib->gui.sb2), "tooltip-text", _("the amount of images to capture in a sequence,\nyou can use this in conjunction with delayed mode to create stop-motion sequences."), (char *)NULL);
  g_object_set(G_OBJECT( lib->gui.sb3), "tooltip-text", _("the amount of brackets on each side of centered shoot, amount of images = (brackets*2)+1."), (char *)NULL);
  g_object_set(G_OBJECT( lib->gui.sb4), "tooltip-text", _("the amount of steps per bracket, steps is camera configurable and usually 3 steps per stop\nwith other words, 3 steps is 1EV exposure step between brackets."), (char *)NULL);

  g_signal_connect(G_OBJECT(lib->gui.tb1), "clicked", G_CALLBACK(_toggle_capture_mode_clicked), lib);
  g_signal_connect(G_OBJECT(lib->gui.tb2), "clicked", G_CALLBACK(_toggle_capture_mode_clicked), lib);
  g_signal_connect(G_OBJECT(lib->gui.tb3), "clicked", G_CALLBACK(_toggle_capture_mode_clicked), lib);
  g_signal_connect(G_OBJECT(lib->gui.button1), "clicked", G_CALLBACK(_capture_button_clicked), lib);

  gtk_widget_set_sensitive( GTK_WIDGET(lib->gui.sb1),FALSE);
  gtk_widget_set_sensitive( GTK_WIDGET(lib->gui.sb2),FALSE);
  gtk_widget_set_sensitive( GTK_WIDGET(lib->gui.sb3),FALSE);
  gtk_widget_set_sensitive( GTK_WIDGET(lib->gui.sb4),FALSE);

  // Camera settings
  dt_lib_camera_property_t *prop;
  gtk_box_pack_start(GTK_BOX(self->widget), dtgtk_label_new(_("properties"),DARKTABLE_LABEL_TAB|DARKTABLE_LABEL_ALIGN_RIGHT), TRUE, TRUE, 0);
  lib->gui.pvbox1 = GTK_BOX(gtk_vbox_new(TRUE, 0));
  lib->gui.pvbox2 = GTK_BOX(gtk_vbox_new(TRUE, 0));

  if( (prop=_lib_property_add_new(lib, _("program"),"expprogram"))!=NULL )
  {
    hbox = GTK_BOX(gtk_hbox_new(FALSE, 0));
    gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(prop->values), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(prop->osd), FALSE, FALSE, 0);
    gtk_box_pack_start(lib->gui.pvbox1, GTK_WIDGET(prop->label), TRUE, TRUE, 0);
    gtk_box_pack_start(lib->gui.pvbox2, GTK_WIDGET(hbox), TRUE, TRUE, 0);
    g_signal_connect(G_OBJECT(prop->osd), "clicked", G_CALLBACK(_osd_button_clicked), prop);
  }

  if( (prop=_lib_property_add_new(lib, _("focus mode"),"focusmode"))!=NULL )
  {
    hbox = GTK_BOX(gtk_hbox_new(FALSE, 0));
    gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(prop->values), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(prop->osd), FALSE, FALSE, 0);
    gtk_box_pack_start(lib->gui.pvbox1, GTK_WIDGET(prop->label), TRUE, TRUE, 0);
    gtk_box_pack_start(lib->gui.pvbox2, GTK_WIDGET(hbox), TRUE, TRUE, 0);
    g_signal_connect(G_OBJECT(prop->osd), "clicked", G_CALLBACK(_osd_button_clicked), prop);
  }
  else if( (prop=_lib_property_add_new(lib, _("focus mode"),"drivemode"))!=NULL )
  {
    hbox = GTK_BOX(gtk_hbox_new(FALSE, 0));
    gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(prop->values), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(prop->osd), FALSE, FALSE, 0);
    gtk_box_pack_start(lib->gui.pvbox1, GTK_WIDGET(prop->label), TRUE, TRUE, 0);
    gtk_box_pack_start(lib->gui.pvbox2, GTK_WIDGET(hbox), TRUE, TRUE, 0);
    g_signal_connect(G_OBJECT(prop->osd), "clicked", G_CALLBACK(_osd_button_clicked), prop);
  }

  if( (prop=_lib_property_add_new(lib, _("aperture"),"f-number"))!=NULL )
  {
    hbox = GTK_BOX(gtk_hbox_new(FALSE, 0));
    gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(prop->values), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(prop->osd), FALSE, FALSE, 0);
    gtk_box_pack_start(lib->gui.pvbox1, GTK_WIDGET(prop->label), TRUE, TRUE, 0);
    gtk_box_pack_start(lib->gui.pvbox2, GTK_WIDGET(hbox), TRUE, TRUE, 0);
    g_signal_connect(G_OBJECT(prop->osd), "clicked", G_CALLBACK(_osd_button_clicked), prop);
  }
  else if( (prop=_lib_property_add_new(lib, _("aperture"),"aperture"))!=NULL ) // for Canon cameras
  {
    hbox = GTK_BOX(gtk_hbox_new(FALSE, 0));
    gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(prop->values), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(prop->osd), FALSE, FALSE, 0);
    gtk_box_pack_start(lib->gui.pvbox1, GTK_WIDGET(prop->label), TRUE, TRUE, 0);
    gtk_box_pack_start(lib->gui.pvbox2, GTK_WIDGET(hbox), TRUE, TRUE, 0);
    g_signal_connect(G_OBJECT(prop->osd), "clicked", G_CALLBACK(_osd_button_clicked), prop);
  }

  if( (prop=_lib_property_add_new(lib, _("focal length"),"focallength"))!=NULL )
  {
    hbox = GTK_BOX(gtk_hbox_new(FALSE, 0));
    gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(prop->values), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(prop->osd), FALSE, FALSE, 0);
    gtk_box_pack_start(lib->gui.pvbox1, GTK_WIDGET(prop->label), TRUE, TRUE, 0);
    gtk_box_pack_start(lib->gui.pvbox2, GTK_WIDGET(hbox), TRUE, TRUE, 0);
    g_signal_connect(G_OBJECT(prop->osd), "clicked", G_CALLBACK(_osd_button_clicked), prop);
  }

  if( (prop=_lib_property_add_new(lib, _("shutterspeed2"),"shutterspeed2"))!=NULL )
  {
    hbox = GTK_BOX(gtk_hbox_new(FALSE, 0));
    gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(prop->values), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(prop->osd), FALSE, FALSE, 0);
    gtk_box_pack_start(lib->gui.pvbox1, GTK_WIDGET(prop->label), TRUE, TRUE, 0);
    gtk_box_pack_start(lib->gui.pvbox2, GTK_WIDGET(hbox), TRUE, TRUE, 0);
    g_signal_connect(G_OBJECT(prop->osd), "clicked", G_CALLBACK(_osd_button_clicked), prop);
  }
  else if( (prop=_lib_property_add_new(lib, _("shutterspeed"),"shutterspeed"))!=NULL ) // Canon, again
  {
    hbox = GTK_BOX(gtk_hbox_new(FALSE, 0));
    gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(prop->values), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(prop->osd), FALSE, FALSE, 0);
    gtk_box_pack_start(lib->gui.pvbox1, GTK_WIDGET(prop->label), TRUE, TRUE, 0);
    gtk_box_pack_start(lib->gui.pvbox2, GTK_WIDGET(hbox), TRUE, TRUE, 0);
    g_signal_connect(G_OBJECT(prop->osd), "clicked", G_CALLBACK(_osd_button_clicked), prop);
  }

  if( (prop=_lib_property_add_new(lib, _("ISO"),"iso"))!=NULL)
  {
    hbox = GTK_BOX(gtk_hbox_new(FALSE, 0));
    gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(prop->values), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(prop->osd), FALSE, FALSE, 0);
    gtk_box_pack_start(lib->gui.pvbox1, GTK_WIDGET(prop->label), TRUE, TRUE, 0);
    gtk_box_pack_start(lib->gui.pvbox2, GTK_WIDGET(hbox), TRUE, TRUE, 0);
    g_signal_connect(G_OBJECT(prop->osd), "clicked", G_CALLBACK(_osd_button_clicked), prop);
  }

  if( (prop=_lib_property_add_new(lib, _("WB"),"whitebalance"))!=NULL)
  {
    hbox = GTK_BOX(gtk_hbox_new(FALSE, 0));
    gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(prop->values), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(prop->osd), FALSE, FALSE, 0);
    gtk_box_pack_start(lib->gui.pvbox1, GTK_WIDGET(prop->label), TRUE, TRUE, 0);
    gtk_box_pack_start(lib->gui.pvbox2, GTK_WIDGET(hbox), TRUE, TRUE, 0);
    g_signal_connect(G_OBJECT(prop->osd), "clicked", G_CALLBACK(_osd_button_clicked), prop);
  }

  if( (prop=_lib_property_add_new(lib, _("quality"),"imagequality"))!=NULL)
  {
    hbox = GTK_BOX(gtk_hbox_new(FALSE, 0));
    gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(prop->values), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(prop->osd), FALSE, FALSE, 0);
    gtk_box_pack_start(lib->gui.pvbox1, GTK_WIDGET(prop->label), TRUE, TRUE, 0);
    gtk_box_pack_start(lib->gui.pvbox2, GTK_WIDGET(hbox), TRUE, TRUE, 0);
    g_signal_connect(G_OBJECT(prop->osd), "clicked", G_CALLBACK(_osd_button_clicked), prop);
  }

  if( (prop=_lib_property_add_new(lib, _("size"),"imagesize"))!=NULL)
  {
    hbox = GTK_BOX(gtk_hbox_new(FALSE, 0));
    gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(prop->values), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(prop->osd), FALSE, FALSE, 0);
    gtk_box_pack_start(lib->gui.pvbox1, GTK_WIDGET(prop->label), TRUE, TRUE, 0);
    gtk_box_pack_start(lib->gui.pvbox2, GTK_WIDGET(hbox), TRUE, TRUE, 0);
    g_signal_connect(G_OBJECT(prop->osd), "clicked", G_CALLBACK(_osd_button_clicked), prop);
  }

  /* add user widgets */
  GSList *options = dt_conf_all_string_entries ("plugins/capture/tethering/properties");
  if (options)
  {
    GSList *item = options;
    if (item)
    {
      do
      {
        dt_conf_string_entry_t *entry = (dt_conf_string_entry_t *)item->data;

        /* get the label from key */
        char *p=entry->key;
        while (p++<entry->key+strlen(entry->key)) if (*p=='_') *p=' ';

        if ((prop = _lib_property_add_new (lib, entry->key,entry->value )) != NULL)
        {
          hbox = GTK_BOX (gtk_hbox_new(FALSE, 0));
          gtk_box_pack_start (GTK_BOX (hbox), GTK_WIDGET (prop->values), TRUE, TRUE, 0);
          gtk_box_pack_start (GTK_BOX (hbox), GTK_WIDGET (prop->osd), FALSE, FALSE, 0);
          gtk_box_pack_start (lib->gui.pvbox1, GTK_WIDGET (prop->label), TRUE, TRUE, 0);
          gtk_box_pack_start (lib->gui.pvbox2, GTK_WIDGET (hbox), TRUE, TRUE, 0);
          g_signal_connect (G_OBJECT (prop->osd), "clicked", G_CALLBACK (_osd_button_clicked), prop);
        }
      }
      while ( (item = g_slist_next (item)) != NULL );
    }
  }


  hbox = GTK_BOX(gtk_hbox_new(FALSE, 0));
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(lib->gui.pvbox1), FALSE, FALSE, 5);
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(lib->gui.pvbox2), TRUE, TRUE, 5);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hbox), FALSE, FALSE, 5);

  /* build the propertymenu */
  dt_camctl_camera_build_property_menu (darktable.camctl,NULL,&lib->gui.properties_menu,G_CALLBACK(_property_choice_callback),lib);

  // user specified properties
  gtk_box_pack_start (GTK_BOX (self->widget), dtgtk_label_new (_("additional properties"),DARKTABLE_LABEL_TAB|DARKTABLE_LABEL_ALIGN_RIGHT), TRUE, TRUE, 5);
  vbox1 = GTK_BOX (gtk_vbox_new (TRUE, 0));
  vbox2 = GTK_BOX (gtk_vbox_new (TRUE, 0));

  gtk_box_pack_start (vbox1, GTK_WIDGET ( gtk_label_new (_("label"))), TRUE, TRUE, 0);
  lib->gui.plabel = gtk_entry_new ();
  dt_gui_key_accel_block_on_focus_connect (lib->gui.plabel);
  gtk_box_pack_start (vbox2, GTK_WIDGET ( lib->gui.plabel ), TRUE, TRUE, 0);

  hbox = GTK_BOX(gtk_hbox_new(FALSE, 0));
  gtk_box_pack_start (vbox1, GTK_WIDGET ( gtk_label_new (_("property"))), TRUE, TRUE, 0);
  GtkWidget *widget = gtk_button_new_with_label("O");
  g_signal_connect (G_OBJECT (widget), "clicked", G_CALLBACK (_show_property_popupmenu_clicked), lib);
  lib->gui.pname = gtk_entry_new ();
  dt_gui_key_accel_block_on_focus_connect (lib->gui.pname);
  gtk_box_pack_start (hbox, GTK_WIDGET ( lib->gui.pname ), TRUE, TRUE, 0);
  gtk_box_pack_start (hbox, GTK_WIDGET ( widget ), FALSE, FALSE, 0);
  gtk_box_pack_start (vbox2, GTK_WIDGET ( hbox ), TRUE, TRUE, 0);

  hbox = GTK_BOX (gtk_hbox_new(FALSE, 0));
  gtk_box_pack_start (GTK_BOX (hbox), GTK_WIDGET (vbox1), FALSE, FALSE, 5);
  gtk_box_pack_start (GTK_BOX (hbox), GTK_WIDGET (vbox2), TRUE, TRUE, 5);
  widget = gtk_button_new_with_label (_("add user property"));
  g_signal_connect (G_OBJECT (widget), "clicked", G_CALLBACK (_add_property_button_clicked), lib);
  gtk_widget_show(widget);
  gtk_box_pack_start (GTK_BOX (self->widget), GTK_WIDGET(hbox), FALSE, FALSE, 5);
  gtk_box_pack_start (GTK_BOX (self->widget), GTK_WIDGET (widget), TRUE, TRUE, 5);



  // Get camera model name
  lib->data.camera_model=dt_camctl_camera_get_model(darktable.camctl,NULL);

  // Register listener
  dt_camctl_register_listener(darktable.camctl,lib->data.listener);
  dt_camctl_tether_mode(darktable.camctl,NULL,TRUE);

}

void
gui_cleanup (dt_lib_module_t *self)
{
  dt_lib_camera_t *lib = self->data;
  dt_gui_key_accel_block_on_focus_disconnect (lib->gui.plabel);
  dt_gui_key_accel_block_on_focus_disconnect (lib->gui.pname);
  // remove listener from camera control..
  dt_camctl_tether_mode(darktable.camctl,NULL,FALSE);
  dt_camctl_unregister_listener(darktable.camctl,lib->data.listener);

}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
