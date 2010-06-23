/*
    This file is part of darktable,
    copyright (c) 2010 henrik andersson.

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
#include "gui/gtk.h"
#include "dtgtk/label.h"
#include <gdk/gdkkeysyms.h>

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
  struct {
      GtkWidget *label1,*label2,*label3;               // Capture modes, delay, sequenced
      GtkDarktableToggleButton *tb1,*tb2;        // Delayed capture, Sequenced capture
      GtkWidget *sb1,*sb2;                         // delay, sequence
      GtkWidget *button1;
      GList *properties;    // a list of dt_lib_camera_property_t
  } gui;
  
  /** Data part of the module */
  struct  {
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
  return DT_CAPTURE_VIEW;
}


void
gui_reset (dt_lib_module_t *self)
{
}

int
position ()
{
  return 998;
}

/** Property changed*/
void property_changed_callback(GtkComboBox *cb,gpointer data) {
  dt_lib_camera_property_t *prop=(dt_lib_camera_property_t *)data;
  dt_camctl_camera_set_property(darktable.camctl,NULL,prop->property_name,gtk_combo_box_get_active_text(prop->values));
}

/** Add  a new property of camera to the gui */
dt_lib_camera_property_t *_lib_property_add_new(dt_lib_camera_t * lib, const gchar *label,const gchar *propertyname)
{
  if( dt_camctl_camera_property_exists(darktable.camctl,NULL,propertyname) )
  {
    const char *value;
    if( (value=dt_camctl_camera_property_get_first_choice(darktable.camctl,NULL,propertyname)) != NULL )
    { // We got a value for property lets construct the gui for the property and add values
      dt_lib_camera_property_t *prop = malloc(sizeof(dt_lib_camera_property_t));
      memset(prop,0,sizeof(dt_lib_camera_property_t));
      prop->name=label;
      prop->property_name=propertyname;
      prop->label = GTK_LABEL(gtk_label_new(label));
      gtk_misc_set_alignment(GTK_MISC(prop->label ), 0.0, 0.5);
      prop->values=GTK_COMBO_BOX(gtk_combo_box_new_text());
      
      prop->osd=DTGTK_TOGGLEBUTTON(dtgtk_togglebutton_new(dtgtk_cairo_paint_eye,0));
      gtk_object_set (GTK_OBJECT(prop->osd), "tooltip-text", _("toggle view property in center view"), NULL);
      do
      {    
        gtk_combo_box_append_text(prop->values, value);
      } while( (value=dt_camctl_camera_property_get_next_choice(darktable.camctl,NULL,propertyname)) != NULL );
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
      } while( gtk_tree_model_iter_next(model,&iter) == TRUE);
  }
  dt_control_gui_queue_draw();
}

/** Invoked when accesibility of a property is changed. */
static void _camera_property_accessibility_changed(const dt_camera_t *camera,const char *name,gboolean read_only,void *data)
{  
}

/** Listener callback from camera control when image are downloaded from camera. */
static void _camera_tethered_downloaded_callback(const dt_camera_t *camera,const char *filename,void *data)
{
  dt_job_t j;
  dt_captured_image_import_job_init(&j,filename);
  dt_control_add_job(darktable.control, &j);
}


/// TODO: Handle brackets into capture job....
static void
_capture_button_clicked(GtkWidget *widget, gpointer user_data)
{
  dt_lib_camera_t *lib=(dt_lib_camera_t *)user_data;
  uint32_t delay= gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(lib->gui.tb1))==TRUE?(uint32_t)gtk_spin_button_get_value(GTK_SPIN_BUTTON(lib->gui.sb1)):0;
  uint32_t count= gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(lib->gui.tb2))==TRUE?(uint32_t)gtk_spin_button_get_value(GTK_SPIN_BUTTON(lib->gui.sb2)):1;
  uint32_t brackets=0;
  dt_job_t j;
  dt_camera_capture_job_init(&j,delay,count,brackets);
  dt_control_add_job(darktable.control, &j);
}

static void _toggle_capture_mode_clicked(GtkWidget *widget, gpointer user_data) {
  dt_lib_camera_t *lib=(dt_lib_camera_t *)user_data;
  GtkWidget *w=NULL;
  if( widget == GTK_WIDGET(lib->gui.tb1) ) w = lib->gui.sb1;
  else if( widget == GTK_WIDGET(lib->gui.tb2) ) w = lib->gui.sb2;
  
  if(w)
    gtk_widget_set_sensitive( w, gtk_toggle_button_get_active( GTK_TOGGLE_BUTTON(widget) ) );
  
}

static void _osd_button_clicked(GtkWidget *widget, gpointer user_data) {
  dt_control_gui_queue_draw();
}

#define BAR_HEIGHT 18
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
  char model[4096]={0};
  sprintf(model+strlen(model),"%s", lib->data.camera_model );
  cairo_text_extents (cr, model, &te);
  cairo_move_to (cr,5, 1+BAR_HEIGHT - te.height / 2 );
  cairo_show_text(cr, model);
  
  // Draw right aligned battary value
  const char *battery_value=dt_camctl_camera_get_property(darktable.camctl,NULL,"batterylevel");
  char battery[4096]={0};
  sprintf(battery,"%s: %s", _("battery"), battery_value?battery_value:_("n/a"));
  cairo_text_extents (cr, battery, &te);
  cairo_move_to(cr,width-te.width-5, 1+BAR_HEIGHT - te.height / 2 );
  cairo_show_text(cr, battery);
  
  // Let's cook up the middle part of infobar
  gchar center[1024]={0};
  for(int i=0;i<g_list_length(lib->gui.properties);i++) {
    dt_lib_camera_property_t *prop=(dt_lib_camera_property_t *)g_list_nth_data(lib->gui.properties,i);
    if( gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(prop->osd)) == TRUE ) {
      g_strlcat(center,"      ",1024);
      g_strlcat(center,prop->name,1024);
      g_strlcat(center,": ",1024);
      g_strlcat(center,gtk_combo_box_get_active_text(prop->values),1024);
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
  lib->data.listener->image_downloaded=_camera_tethered_downloaded_callback;
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
  gtk_misc_set_alignment(GTK_MISC(lib->gui.label1 ), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(lib->gui.label2 ), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(lib->gui.label3 ), 0.0, 0.5);
  gtk_box_pack_start(vbox1, GTK_WIDGET(lib->gui.label1), FALSE, FALSE, 0);
  gtk_box_pack_start(vbox1, GTK_WIDGET(lib->gui.label2), FALSE, FALSE, 0);
  gtk_box_pack_start(vbox1, GTK_WIDGET(lib->gui.label3), FALSE, FALSE, 0);
  
  // capture modes buttons
  lib->gui.tb1=DTGTK_TOGGLEBUTTON(dtgtk_togglebutton_new(dtgtk_cairo_paint_timer,0));
  lib->gui.tb2=DTGTK_TOGGLEBUTTON(dtgtk_togglebutton_new(dtgtk_cairo_paint_filmstrip,0));
  
  hbox = GTK_BOX(gtk_hbox_new(TRUE, 5));
  gtk_box_pack_start(hbox, GTK_WIDGET(lib->gui.tb1), TRUE, TRUE, 0);
  gtk_box_pack_start(hbox, GTK_WIDGET(lib->gui.tb2), TRUE, TRUE, 0);
  gtk_box_pack_start(vbox2, GTK_WIDGET(hbox),FALSE, FALSE, 0);
  
  lib->gui.sb1=gtk_spin_button_new_with_range(1,60,1);
  lib->gui.sb2=gtk_spin_button_new_with_range(1,500,1);
  gtk_box_pack_start(vbox2, GTK_WIDGET(lib->gui.sb1), TRUE, TRUE, 0);
  gtk_box_pack_start(vbox2, GTK_WIDGET(lib->gui.sb2), TRUE, TRUE, 0);
  

  hbox = GTK_BOX(gtk_hbox_new(FALSE, 0));
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(vbox1), FALSE, FALSE, 5);
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(vbox2), TRUE, TRUE, 5);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hbox), FALSE, FALSE, 5);
  lib->gui.button1=gtk_button_new_with_label(_("capture image(s)"));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(lib->gui.button1), FALSE, FALSE, 5);

  gtk_object_set (GTK_OBJECT(lib->gui.tb1), "tooltip-text", _("toggle delayed capture mode"), NULL);
  gtk_object_set (GTK_OBJECT( lib->gui.tb2), "tooltip-text", _("toggle sequenced capture mode"), NULL);
  gtk_object_set (GTK_OBJECT( lib->gui.sb1), "tooltip-text", _("the count of seconds before actually doing a capture"), NULL);
  gtk_object_set (GTK_OBJECT( lib->gui.sb2), "tooltip-text", _("the amount of images to capture in a sequence,\nyou can use this in conjuction with delayed mode to create stop-motion sequences."), NULL);

  g_signal_connect(G_OBJECT(lib->gui.tb1), "clicked", G_CALLBACK(_toggle_capture_mode_clicked), lib);
  g_signal_connect(G_OBJECT(lib->gui.tb2), "clicked", G_CALLBACK(_toggle_capture_mode_clicked), lib);
  g_signal_connect(G_OBJECT(lib->gui.button1), "clicked", G_CALLBACK(_capture_button_clicked), lib);

  gtk_widget_set_sensitive( GTK_WIDGET(lib->gui.sb1),FALSE);
  gtk_widget_set_sensitive( GTK_WIDGET(lib->gui.sb2),FALSE);
  
  // Camera settings
  dt_lib_camera_property_t *prop;
  gtk_box_pack_start(GTK_BOX(self->widget), dtgtk_label_new(_("properties"),DARKTABLE_LABEL_TAB|DARKTABLE_LABEL_ALIGN_RIGHT), TRUE, TRUE, 0);
  vbox1 = GTK_BOX(gtk_vbox_new(TRUE, 0));
  vbox2 = GTK_BOX(gtk_vbox_new(TRUE, 0));
  
  if( (prop=_lib_property_add_new(lib, _("program"),"expprogram"))!=NULL )
  {
    hbox = GTK_BOX(gtk_hbox_new(FALSE, 0));
    gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(prop->values), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(prop->osd), FALSE, FALSE, 0);
    gtk_box_pack_start(vbox1, GTK_WIDGET(prop->label), TRUE, TRUE, 0);
    gtk_box_pack_start(vbox2, GTK_WIDGET(hbox), TRUE, TRUE, 0);
    g_signal_connect(G_OBJECT(prop->osd), "clicked", G_CALLBACK(_osd_button_clicked), prop);
  }
  
   if( (prop=_lib_property_add_new(lib, _("focus mode"),"focusmode"))!=NULL )
  {
    hbox = GTK_BOX(gtk_hbox_new(FALSE, 0));
    gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(prop->values), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(prop->osd), FALSE, FALSE, 0);
    gtk_box_pack_start(vbox1, GTK_WIDGET(prop->label), TRUE, TRUE, 0);
    gtk_box_pack_start(vbox2, GTK_WIDGET(hbox), TRUE, TRUE, 0);
    g_signal_connect(G_OBJECT(prop->osd), "clicked", G_CALLBACK(_osd_button_clicked), prop);
  }
 
  if( (prop=_lib_property_add_new(lib, _("aperture"),"f-number"))!=NULL )
  {
    hbox = GTK_BOX(gtk_hbox_new(FALSE, 0));
    gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(prop->values), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(prop->osd), FALSE, FALSE, 0);
    gtk_box_pack_start(vbox1, GTK_WIDGET(prop->label), TRUE, TRUE, 0);
    gtk_box_pack_start(vbox2, GTK_WIDGET(hbox), TRUE, TRUE, 0);
    g_signal_connect(G_OBJECT(prop->osd), "clicked", G_CALLBACK(_osd_button_clicked), prop);
  }
  
  if( (prop=_lib_property_add_new(lib, _("focal length"),"focallength"))!=NULL )
  {
    hbox = GTK_BOX(gtk_hbox_new(FALSE, 0));
    gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(prop->values), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(prop->osd), FALSE, FALSE, 0);
    gtk_box_pack_start(vbox1, GTK_WIDGET(prop->label), TRUE, TRUE, 0);
    gtk_box_pack_start(vbox2, GTK_WIDGET(hbox), TRUE, TRUE, 0);
    g_signal_connect(G_OBJECT(prop->osd), "clicked", G_CALLBACK(_osd_button_clicked), prop);
  }
 
  if( (prop=_lib_property_add_new(lib, _("shutterspeed2"),"shutterspeed2"))!=NULL )
  {
    hbox = GTK_BOX(gtk_hbox_new(FALSE, 0));
    gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(prop->values), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(prop->osd), FALSE, FALSE, 0);
    gtk_box_pack_start(vbox1, GTK_WIDGET(prop->label), TRUE, TRUE, 0);
    gtk_box_pack_start(vbox2, GTK_WIDGET(hbox), TRUE, TRUE, 0);
    g_signal_connect(G_OBJECT(prop->osd), "clicked", G_CALLBACK(_osd_button_clicked), prop);
  }
 
  if( (prop=_lib_property_add_new(lib, _("iso"),"iso"))!=NULL)
  {
    hbox = GTK_BOX(gtk_hbox_new(FALSE, 0));
    gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(prop->values), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(prop->osd), FALSE, FALSE, 0);
    gtk_box_pack_start(vbox1, GTK_WIDGET(prop->label), TRUE, TRUE, 0);
    gtk_box_pack_start(vbox2, GTK_WIDGET(hbox), TRUE, TRUE, 0);
    g_signal_connect(G_OBJECT(prop->osd), "clicked", G_CALLBACK(_osd_button_clicked), prop);
  }
  
  if( (prop=_lib_property_add_new(lib, _("wb"),"whitebalance"))!=NULL)
  {
    hbox = GTK_BOX(gtk_hbox_new(FALSE, 0));
    gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(prop->values), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(prop->osd), FALSE, FALSE, 0);
    gtk_box_pack_start(vbox1, GTK_WIDGET(prop->label), TRUE, TRUE, 0);
    gtk_box_pack_start(vbox2, GTK_WIDGET(hbox), TRUE, TRUE, 0);
    g_signal_connect(G_OBJECT(prop->osd), "clicked", G_CALLBACK(_osd_button_clicked), prop);
  }
  
  if( (prop=_lib_property_add_new(lib, _("quality"),"imagequality"))!=NULL)
  {
    hbox = GTK_BOX(gtk_hbox_new(FALSE, 0));
    gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(prop->values), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(prop->osd), FALSE, FALSE, 0);
    gtk_box_pack_start(vbox1, GTK_WIDGET(prop->label), TRUE, TRUE, 0);
    gtk_box_pack_start(vbox2, GTK_WIDGET(hbox), TRUE, TRUE, 0);
    g_signal_connect(G_OBJECT(prop->osd), "clicked", G_CALLBACK(_osd_button_clicked), prop);
  }
  
  if( (prop=_lib_property_add_new(lib, _("size"),"imagesize"))!=NULL)
  {
    hbox = GTK_BOX(gtk_hbox_new(FALSE, 0));
    gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(prop->values), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(prop->osd), FALSE, FALSE, 0);
    gtk_box_pack_start(vbox1, GTK_WIDGET(prop->label), TRUE, TRUE, 0);
    gtk_box_pack_start(vbox2, GTK_WIDGET(hbox), TRUE, TRUE, 0);
    g_signal_connect(G_OBJECT(prop->osd), "clicked", G_CALLBACK(_osd_button_clicked), prop);
  }
  
  
  hbox = GTK_BOX(gtk_hbox_new(FALSE, 0));
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(vbox1), FALSE, FALSE, 5);
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(vbox2), TRUE, TRUE, 5);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hbox), FALSE, FALSE, 5);

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
  // remove listener from camera control..
  dt_camctl_tether_mode(darktable.camctl,NULL,FALSE);
  dt_camctl_unregister_listener(darktable.camctl,lib->data.listener);
  
}

