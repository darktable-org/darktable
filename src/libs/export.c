/*
    This file is part of darktable,
    copyright (c) 2009--2010 johannes hanika.

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
#include "common/imageio_module.h"
#include "control/control.h"
#include "control/jobs.h"
#include "control/conf.h"
#include "gui/gtk.h"
#include "dtgtk/slider.h"
#include "libs/lib.h"
#include <stdlib.h>
#include <gtk/gtk.h>
#include <glade/glade.h>
#include <lcms.h>

DT_MODULE(1)

typedef struct dt_lib_export_t
{
  GtkSpinButton *width, *height;
  GtkComboBox *storage, *format;
  GtkContainer *storage_box, *format_box;
  GtkComboBox *profile, *intent;
  GList *profiles;
}
dt_lib_export_t;

typedef struct dt_lib_export_profile_t
{
  char filename[512]; // icc file name
  char name[512];     // product name
  int  pos;           // position in combo box    
}
dt_lib_export_profile_t;

const char*
name ()
{
  return _("export selected");
}

uint32_t views() 
{
  return DT_LIGHTTABLE_VIEW;
}

static void
export_button_clicked (GtkWidget *widget, gpointer user_data)
{
#if 0
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_export_t *d = (dt_lib_export_t *)self->data;
  // read "format" to global settings
  int i = gtk_combo_box_get_active(d->format);
  if     (i == 0)  dt_conf_set_int ("plugins/lighttable/export/format", DT_DEV_EXPORT_JPG);
  else if(i == 1)  dt_conf_set_int ("plugins/lighttable/export/format", DT_DEV_EXPORT_PNG);
  else if(i == 2)  dt_conf_set_int ("plugins/lighttable/export/format", DT_DEV_EXPORT_TIFF8);
  else if(i == 3)  dt_conf_set_int ("plugins/lighttable/export/format", DT_DEV_EXPORT_PPM16);
  else if(i == 4)  dt_conf_set_int ("plugins/lighttable/export/format", DT_DEV_EXPORT_TIFF16);
  else if(i == 5)  dt_conf_set_int ("plugins/lighttable/export/format", DT_DEV_EXPORT_PFM);
  else if(i == 6)  dt_conf_set_int ("plugins/lighttable/export/format", DT_DEV_EXPORT_EXR);
#endif
  dt_control_export();
}

#if 0
static void
export_quality_changed (GtkDarktableSlider *slider, gpointer user_data)
{
  GtkWidget *widget;
  int quality = (int)dtgtk_slider_get_value(slider);
  dt_conf_set_int ("plugins/lighttable/export/quality", quality);
  widget = glade_xml_get_widget (darktable.gui->main_window, "center");
  gtk_widget_queue_draw(widget);
}
#endif

static void
width_changed (GtkSpinButton *spin, gpointer user_data)
{
  int value = gtk_spin_button_get_value(spin);
  dt_conf_set_int ("plugins/lighttable/export/width", value);
}

static void
height_changed (GtkSpinButton *spin, gpointer user_data)
{
  int value = gtk_spin_button_get_value(spin);
  dt_conf_set_int ("plugins/lighttable/export/height", value);
}

void
gui_reset (dt_lib_module_t *self)
{
  // make sure we don't do anything useless:
  if(!darktable.control->running) return;
  dt_lib_export_t *d = (dt_lib_export_t *)self->data;
  // int quality = MIN(100, MAX(1, dt_conf_get_int ("plugins/lighttable/export/quality")));
  // dtgtk_slider_set_value(d->quality, quality);
  gtk_spin_button_set_value(d->width,  dt_conf_get_int("plugins/lighttable/export/width"));
  gtk_spin_button_set_value(d->height, dt_conf_get_int("plugins/lighttable/export/height"));
  int k = dt_conf_get_int ("plugins/lighttable/export/format");
  gtk_combo_box_set_active(d->format, k);
  GList *it = g_list_nth(darktable.imageio->plugins_format, k);
  if(it)
  {
    dt_imageio_module_format_t *module = (dt_imageio_module_format_t *)it->data;
    GtkWidget *old = gtk_bin_get_child(GTK_BIN(d->format_box));
    // printf("export: gui_reset replacing %lu with %lu\n", (long int)old, (long int)module->widget);
    if(old != module->widget)
    {
      if(old) gtk_container_remove(d->format_box, old);
      if(module->widget) gtk_container_add(d->format_box, module->widget);
    }
    gtk_widget_show_all(GTK_WIDGET(d->format_box));
  }
  gtk_combo_box_set_active(d->intent, (int)dt_conf_get_int("plugins/lighttable/export/iccintent") + 1);
  int iccfound = 0;
  gchar *iccprofile = dt_conf_get_string("plugins/lighttable/export/iccprofile");
  if(iccprofile)
  {
    GList *prof = d->profiles;
    while(prof)
    {
      dt_lib_export_profile_t *pp = (dt_lib_export_profile_t *)prof->data;
      if(!strcmp(pp->filename, iccprofile))
      {
        gtk_combo_box_set_active(d->profile, pp->pos);
        iccfound = 1;
      }
      if(iccfound) break;
      prof = g_list_next(prof);
    }
    g_free(iccprofile);
  }
  if(!iccfound) gtk_combo_box_set_active(d->profile, 0);
  dt_imageio_module_format_t *mformat = dt_imageio_get_format();
  if(mformat) mformat->gui_reset(mformat);
  dt_imageio_module_storage_t *mstorage = dt_imageio_get_storage();
  if(mstorage) mstorage->gui_reset(mstorage);
}

static void
format_changed (GtkComboBox *widget, dt_lib_export_t *d)
{
  int k = gtk_combo_box_get_active(d->format);
  dt_conf_set_int ("plugins/lighttable/export/format", k);
  GList *it = g_list_nth(darktable.imageio->plugins_format, k);
  if(it)
  {
    dt_imageio_module_format_t *module = (dt_imageio_module_format_t *)it->data;
    GtkWidget *old = gtk_bin_get_child(GTK_BIN(d->format_box));
    // printf("export: combo replacing %lu with %lu\n", (long int)old, (long int)module->widget);
    if(old != module->widget)
    {
      if(old) gtk_container_remove(d->format_box, old);
      if(module->widget) gtk_container_add(d->format_box, module->widget);
    }
    gtk_widget_show_all(GTK_WIDGET(d->format_box));
  }
}

static void
profile_changed (GtkComboBox *widget, dt_lib_export_t *d)
{
  int pos = gtk_combo_box_get_active(widget);
  GList *prof = d->profiles;
  while(prof)
  { // could use g_list_nth. this seems safer?
    dt_lib_export_profile_t *pp = (dt_lib_export_profile_t *)prof->data;
    if(pp->pos == pos)
    {
      dt_conf_set_string("plugins/lighttable/export/iccprofile", pp->filename);
      return;
    }
    prof = g_list_next(prof);
  }
  dt_conf_set_string("plugins/lighttable/export/iccprofile", "image");
}

static void
intent_changed (GtkComboBox *widget, dt_lib_export_t *d)
{
  int pos = gtk_combo_box_get_active(widget);
  dt_conf_set_int("plugins/lighttable/export/iccintent", pos-1);
}

int
position ()
{
  return 0;
}

void
gui_init (dt_lib_module_t *self)
{
  dt_lib_export_t *d = (dt_lib_export_t *)malloc(sizeof(dt_lib_export_t));
  self->data = (void *)d;
  self->widget = gtk_table_new(8, 2, FALSE);
  gtk_table_set_row_spacings(GTK_TABLE(self->widget), 5);
  
  GtkWidget *label;

  label = gtk_label_new(_("target storage"));
  gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
  gtk_table_attach(GTK_TABLE(self->widget), label, 0, 1, 0, 1, GTK_FILL|GTK_EXPAND, 0, 0, 0);
  d->storage = GTK_COMBO_BOX(gtk_combo_box_new_text());
  // TODO: load modules!
#if 0
  GList *it = darktable.imageio->plugins_format;
  while(it)
  {
    dt_imageio_module_storage_t *module = (dt_imageio_module_storage_t *)it->data;
    gtk_combo_box_append_text(d->format, module->name());
    it = g_list_next(it);
  }
#endif
  gtk_combo_box_append_text(d->storage, _("file on harddrive"));
  gtk_combo_box_set_active(d->storage, 0);
  gtk_table_attach(GTK_TABLE(self->widget), GTK_WIDGET(d->storage), 1, 2, 0, 1, GTK_EXPAND|GTK_FILL, 0, 0, 0);

  d->storage_box = GTK_CONTAINER(gtk_alignment_new(1.0, 1.0, 1.0, 1.0));
  gtk_alignment_set_padding(GTK_ALIGNMENT(d->storage_box), 0, 0, 0, 0);
  gtk_table_attach(GTK_TABLE(self->widget), GTK_WIDGET(d->storage_box), 0, 2, 1, 2, GTK_EXPAND|GTK_FILL, 0, 0, 0);

  label = gtk_label_new(_("file format"));
  gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
  gtk_table_attach(GTK_TABLE(self->widget), label, 0, 1, 2, 3, GTK_EXPAND|GTK_FILL, 0, 0, 0);
  d->format = GTK_COMBO_BOX(gtk_combo_box_new_text());
  GList *it = darktable.imageio->plugins_format;
  while(it)
  {
    dt_imageio_module_format_t *module = (dt_imageio_module_format_t *)it->data;
    gtk_combo_box_append_text(d->format, module->name());
    it = g_list_next(it);
  }
  gtk_table_attach(GTK_TABLE(self->widget), GTK_WIDGET(d->format), 1, 2, 2, 3, GTK_EXPAND|GTK_FILL, 0, 0, 0);
  g_signal_connect (G_OBJECT (d->format), "changed",
                    G_CALLBACK (format_changed),
                    (gpointer)d);

  d->format_box = GTK_CONTAINER(gtk_alignment_new(1.0, 1.0, 1.0, 1.0));
  gtk_alignment_set_padding(GTK_ALIGNMENT(d->format_box), 0, 0, 0, 0);
  gtk_table_attach(GTK_TABLE(self->widget), GTK_WIDGET(d->format_box), 0, 2, 3, 4, GTK_EXPAND|GTK_FILL, 0, 0, 0);

  d->width  = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(0, 10000, 1));
  gtk_object_set(GTK_OBJECT(d->width), "tooltip-text", _("maximum output width\nset to 0 for no scaling"), NULL);
  d->height = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(0, 10000, 1));
  gtk_object_set(GTK_OBJECT(d->height), "tooltip-text", _("maximum output height\nset to 0 for no scaling"), NULL);
  label = gtk_label_new(_("maximum size"));
  gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
  gtk_table_attach(GTK_TABLE(self->widget), label, 0, 1, 4, 5, GTK_EXPAND|GTK_FILL, 0, 0, 0);
  GtkBox *hbox = GTK_BOX(gtk_hbox_new(FALSE, 5));
  gtk_box_pack_start(hbox, GTK_WIDGET(d->width), TRUE, TRUE, 0);
  gtk_box_pack_start(hbox, gtk_label_new(_("x")), FALSE, FALSE, 0);
  gtk_box_pack_start(hbox, GTK_WIDGET(d->height), TRUE, TRUE, 0);
  gtk_table_attach(GTK_TABLE(self->widget), GTK_WIDGET(hbox), 1, 2, 4, 5, GTK_EXPAND|GTK_FILL, 0, 0, 0);

  label = gtk_label_new(_("rendering intent"));
  gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
  gtk_table_attach(GTK_TABLE(self->widget), label, 0, 1, 5, 6, GTK_EXPAND|GTK_FILL, 0, 0, 0);
  d->intent = GTK_COMBO_BOX(gtk_combo_box_new_text());
  gtk_combo_box_append_text(d->intent, _("image settings"));
  gtk_combo_box_append_text(d->intent, _("perceptual"));
  gtk_combo_box_append_text(d->intent, _("relative colorimetric"));
  gtk_combo_box_append_text(d->intent, C_("rendering intent", "saturation"));
  gtk_combo_box_append_text(d->intent, _("absolute colorimetric"));
  gtk_table_attach(GTK_TABLE(self->widget), GTK_WIDGET(d->intent), 1, 2, 5, 6, GTK_EXPAND|GTK_FILL, 0, 0, 0);

  d->profiles = NULL;

  dt_lib_export_profile_t *prof = (dt_lib_export_profile_t *)malloc(sizeof(dt_lib_export_profile_t));
  strcpy(prof->filename, "sRGB");
  strcpy(prof->name, _("srgb (web-safe)"));
  int pos;
  prof->pos = 1;
  d->profiles = g_list_append(d->profiles, prof);

  prof = (dt_lib_export_profile_t *)malloc(sizeof(dt_lib_export_profile_t));
  strcpy(prof->filename, "adobergb");
  strcpy(prof->name, _("adobe rgb"));
  prof->pos = 2;
  d->profiles = g_list_append(d->profiles, prof);

  prof = (dt_lib_export_profile_t *)malloc(sizeof(dt_lib_export_profile_t));
  strcpy(prof->filename, "X profile");
  strcpy(prof->name, "X profile");
  pos = prof->pos = 3;
  d->profiles = g_list_append(d->profiles, prof);

  prof = (dt_lib_export_profile_t *)malloc(sizeof(dt_lib_export_profile_t));
  strcpy(prof->filename, "linear_rgb");
  strcpy(prof->name, _("linear rgb"));
  pos = prof->pos = 3;
  d->profiles = g_list_append(d->profiles, prof);

  // read datadir/color/out/*.icc
  char datadir[1024], dirname[1024], filename[1024];
  dt_get_datadir(datadir, 1024);
  snprintf(dirname, 1024, "%s/color/out", datadir);
  cmsHPROFILE tmpprof;
  const gchar *d_name;
  GDir *dir = g_dir_open(dirname, 0, NULL);
  (void)cmsErrorAction(LCMS_ERROR_IGNORE);
  if(dir)
  {
    while((d_name = g_dir_read_name(dir)))
    {
      snprintf(filename, 1024, "%s/%s", dirname, d_name);
      tmpprof = cmsOpenProfileFromFile(filename, "r");
      if(tmpprof)
      {
        dt_lib_export_profile_t *prof = (dt_lib_export_profile_t *)malloc(sizeof(dt_lib_export_profile_t));
        strcpy(prof->name, cmsTakeProductDesc(tmpprof));
        strcpy(prof->filename, d_name);
        prof->pos = ++pos;
        cmsCloseProfile(tmpprof);
        d->profiles = g_list_append(d->profiles, prof);
      }
    }
    g_dir_close(dir);
  }
  GList *l = d->profiles;
  label = gtk_label_new(_("output profile"));
  gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
  gtk_table_attach(GTK_TABLE(self->widget), label, 0, 1, 6, 7, GTK_EXPAND|GTK_FILL, 0, 0, 0);
  d->profile = GTK_COMBO_BOX(gtk_combo_box_new_text());
  gtk_table_attach(GTK_TABLE(self->widget), GTK_WIDGET(d->profile), 1, 2, 6, 7, GTK_EXPAND|GTK_FILL, 0, 0, 0);
  gtk_combo_box_append_text(d->profile, _("image settings"));
  while(l)
  {
    dt_lib_export_profile_t *prof = (dt_lib_export_profile_t *)l->data;
    if(!strcmp(prof->name, "X profile"))
      gtk_combo_box_append_text(d->profile, _("system display profile"));
    else
      gtk_combo_box_append_text(d->profile, prof->name);
    l = g_list_next(l);
  }
  gtk_combo_box_set_active(d->profile, 0);
  char tooltip[1024];
  snprintf(tooltip, 1024, _("icc profiles in %s/color/out"), datadir);
  gtk_object_set(GTK_OBJECT(d->profile), "tooltip-text", tooltip, NULL);
  g_signal_connect (G_OBJECT (d->intent), "changed",
                    G_CALLBACK (intent_changed),
                    (gpointer)d);
  g_signal_connect (G_OBJECT (d->profile), "changed",
                    G_CALLBACK (profile_changed),
                    (gpointer)d);

  GtkButton *button = GTK_BUTTON(gtk_button_new_with_label(_("export")));
  gtk_table_attach(GTK_TABLE(self->widget), GTK_WIDGET(button), 1, 2, 7, 8, GTK_EXPAND|GTK_FILL, 0, 0, 0);

  g_signal_connect (G_OBJECT (button), "clicked",
                    G_CALLBACK (export_button_clicked),
                    (gpointer)self);
  g_signal_connect (G_OBJECT (d->width), "value-changed",
                    G_CALLBACK (width_changed),
                    (gpointer)0);
  g_signal_connect (G_OBJECT (d->height), "value-changed",
                    G_CALLBACK (height_changed),
                    (gpointer)0);

  self->gui_reset(self);
}

void
gui_cleanup (dt_lib_module_t *self)
{
  dt_lib_export_t *d = (dt_lib_export_t *)self->data;
  GtkWidget *old = gtk_bin_get_child(GTK_BIN(d->format_box));
  if(old) gtk_container_remove(d->format_box, old);
  free(self->data);
  self->data = NULL;
}


