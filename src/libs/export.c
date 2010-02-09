#include "common/darktable.h"
#include "control/control.h"
#include "control/jobs.h"
#include "control/conf.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include <stdlib.h>
#include <gtk/gtk.h>
#include <glade/glade.h>

DT_MODULE

typedef struct dt_lib_export_t
{
  GtkSpinButton *width, *height;
  GtkComboBox *format;
  GtkScale *quality;
}
dt_lib_export_t;


const char*
name ()
{
  return _("export selected");
}

static void
export_button_clicked (GtkWidget *widget, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_export_t *d = (dt_lib_export_t *)self->data;
  // read "format" to global settings
  int i = gtk_combo_box_get_active(d->format);
  if     (i == 0)  dt_conf_set_int ("plugins/lighttable/export/format", DT_DEV_EXPORT_JPG);
  else if(i == 1)  dt_conf_set_int ("plugins/lighttable/export/format", DT_DEV_EXPORT_PNG);
  else if(i == 2)  dt_conf_set_int ("plugins/lighttable/export/format", DT_DEV_EXPORT_PPM16);
  else if(i == 3)  dt_conf_set_int ("plugins/lighttable/export/format", DT_DEV_EXPORT_PFM);
  dt_control_export();
}

static void
export_quality_changed (GtkRange *range, gpointer user_data)
{
  GtkWidget *widget;
  int quality = (int)gtk_range_get_value(range);
  dt_conf_set_int ("plugins/lighttable/export/quality", quality);
  widget = glade_xml_get_widget (darktable.gui->main_window, "center");
  gtk_widget_queue_draw(widget);
}

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
  dt_lib_export_t *d = (dt_lib_export_t *)self->data;
  int quality = MIN(100, MAX(1, dt_conf_get_int ("plugins/lighttable/export/quality")));
  gtk_range_set_value(GTK_RANGE(d->quality), quality);
  int k = dt_conf_get_int ("plugins/lighttable/export/format");
  int i = 0;
  if     (k == DT_DEV_EXPORT_JPG)   i = 0;
  else if(k == DT_DEV_EXPORT_PNG)   i = 1;
  else if(k == DT_DEV_EXPORT_PPM16) i = 2;
  else if(k == DT_DEV_EXPORT_PFM)   i = 3;
  gtk_combo_box_set_active(d->format, i);
}

void
gui_init (dt_lib_module_t *self)
{
  dt_lib_export_t *d = (dt_lib_export_t *)malloc(sizeof(dt_lib_export_t));
  self->data = (void *)d;
  self->widget = gtk_vbox_new(TRUE, 5);
  
  GtkBox *hbox;
  GtkWidget *label;
  hbox = GTK_BOX(gtk_hbox_new(FALSE, 0));
  d->width  = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(0, 10000, 1));
  gtk_object_set(GTK_OBJECT(d->width), "tooltip-text", _("maximum output width\nset to 0 for no scaling"), NULL);
  d->height = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(0, 10000, 1));
  gtk_object_set(GTK_OBJECT(d->height), "tooltip-text", _("maximum output height\nset to 0 for no scaling"), NULL);
  label = gtk_label_new(_("maximum size"));
  gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
  gtk_box_pack_start(hbox, label, TRUE, TRUE, 5);
  gtk_box_pack_start(hbox, GTK_WIDGET(d->width), FALSE, FALSE, 5);
  gtk_box_pack_start(hbox, gtk_label_new(_("x")), FALSE, FALSE, 5);
  gtk_box_pack_start(hbox, GTK_WIDGET(d->height), FALSE, FALSE, 5);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hbox), TRUE, TRUE, 0);

  hbox = GTK_BOX(gtk_hbox_new(FALSE, 0));
  d->quality = GTK_SCALE(gtk_hscale_new_with_range(0, 100, 1));
  gtk_scale_set_value_pos(d->quality, GTK_POS_LEFT);
  gtk_range_set_value(GTK_RANGE(d->quality), 97);
  gtk_scale_set_digits(d->quality, 0);
  gtk_box_pack_start(hbox, gtk_label_new(_("quality")), FALSE, FALSE, 5);
  gtk_box_pack_start(hbox, GTK_WIDGET(d->quality), TRUE, TRUE, 5);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hbox), TRUE, TRUE, 0);

#if 0
  g->vbox1 = GTK_VBOX(gtk_vbox_new(FALSE, 0));
  g->vbox2 = GTK_VBOX(gtk_vbox_new(FALSE, 0));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->vbox1), FALSE, FALSE, 5);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->vbox2), TRUE, TRUE, 5);
  g->label1 = GTK_LABEL(gtk_label_new(_("rendering intent")));
  gtk_misc_set_alignment(GTK_MISC(g->label1), 0.0, 0.5);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label1), TRUE, TRUE, 0);
  g->cbox1 = GTK_COMBO_BOX(gtk_combo_box_new_text());
  gtk_combo_box_append_text(g->cbox1, _("perceptual"));
  gtk_combo_box_append_text(g->cbox1, _("relative colorimetric"));
  gtk_combo_box_append_text(g->cbox1, _("saturation"));
  gtk_combo_box_append_text(g->cbox1, _("absolute colorimetric"));
  GList *l = g->profiles;
  while(l)
  {
    dt_iop_color_profile_t *prof = (dt_iop_color_profile_t *)l->data;
    if(!strcmp(prof->name, "X profile"))
    {
      gtk_combo_box_append_text(g->cbox2, _("system display profile"));
      gtk_combo_box_append_text(g->cbox3, _("system display profile"));
    }
    else
    {
      gtk_combo_box_append_text(g->cbox2, prof->name);
      gtk_combo_box_append_text(g->cbox3, prof->name);
    }
    l = g_list_next(l);
  }
  gtk_combo_box_set_active(g->cbox1, 0);
  char tooltip[1024];
  snprintf(tooltip, 1024, _("icc profiles in %s/color/out"), datadir);
  gtk_object_set(GTK_OBJECT(g->cbox2), "tooltip-text", tooltip, NULL);
  g_signal_connect (G_OBJECT (g->cbox4), "changed",
                    G_CALLBACK (display_intent_changed),
                    (gpointer)self);
  g_signal_connect (G_OBJECT (g->cbox2), "changed",
                    G_CALLBACK (profile_changed),
                    (gpointer)self);
#endif
  

  hbox = GTK_BOX(gtk_hbox_new(FALSE, 0));
  d->format = GTK_COMBO_BOX(gtk_combo_box_new_text());
  gtk_combo_box_append_text(d->format, _("8-bit jpg"));
  gtk_combo_box_append_text(d->format, _("8-bit png"));
  gtk_combo_box_append_text(d->format, _("16-bit ppm"));
  gtk_combo_box_append_text(d->format, _("float pfm"));
  GtkButton *button = GTK_BUTTON(gtk_button_new_with_label(_("export")));
  gtk_box_pack_start(hbox, GTK_WIDGET(d->format), TRUE, TRUE, 0);
  gtk_box_pack_start(hbox, GTK_WIDGET(button), FALSE, FALSE, 5);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hbox), TRUE, TRUE, 0);

  g_signal_connect (G_OBJECT (button), "clicked",
                    G_CALLBACK (export_button_clicked),
                    (gpointer)self);
  g_signal_connect (G_OBJECT (d->quality), "value-changed",
                    G_CALLBACK (export_quality_changed),
                    (gpointer)0);
  g_signal_connect (G_OBJECT (d->width), "value-changed",
                    G_CALLBACK (width_changed),
                    (gpointer)0);
  g_signal_connect (G_OBJECT (d->height), "value-changed",
                    G_CALLBACK (height_changed),
                    (gpointer)0);
}

void
gui_cleanup (dt_lib_module_t *self)
{
  free(self->data);
  self->data = NULL;
}


