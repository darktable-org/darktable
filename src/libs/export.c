#include "common/darktable.h"
#include "control/control.h"
#include "control/jobs.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include <stdlib.h>
#include <gtk/gtk.h>
#include <glade/glade.h>

typedef struct dt_lib_export_t
{
  GtkComboBox *export_format;
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
  // read "export_format" to global settings
  GtkWidget *wid = glade_xml_get_widget (darktable.gui->main_window, "export_format");
  int i = gtk_combo_box_get_active(GTK_COMBO_BOX(wid));
  if     (i == 0)  gconf_client_set_int  (darktable.control->gconf, DT_GCONF_DIR"/plugins_lighttable/export/format",   DT_DEV_EXPORT_JPG, NULL);
  else if(i == 1)  gconf_client_set_int  (darktable.control->gconf, DT_GCONF_DIR"/plugins_lighttable/export/format",   DT_DEV_EXPORT_PNG, NULL);
  else if(i == 2)  gconf_client_set_int  (darktable.control->gconf, DT_GCONF_DIR"/plugins_lighttable/export/format",   DT_DEV_EXPORT_PPM16, NULL);
  else if(i == 3)  gconf_client_set_int  (darktable.control->gconf, DT_GCONF_DIR"/plugins_lighttable/export/format",   DT_DEV_EXPORT_PFM, NULL);
  pthread_mutex_lock(&(darktable.film->images_mutex));
  darktable.film->last_exported = 0;
  pthread_mutex_unlock(&(darktable.film->images_mutex));
  dt_control_export();
}

static void
export_quality_changed (GtkRange *range, gpointer user_data)
{
  GtkWidget *widget;
  int quality = (int)gtk_range_get_value(range);
  gconf_client_set_int  (darktable.control->gconf, DT_GCONF_DIR"/plugins_lighttable/export/quality", quality, NULL);
  widget = glade_xml_get_widget (darktable.gui->main_window, "center");
  gtk_widget_queue_draw(widget);
}

void
gui_reset (dt_lib_module_t *self)
{
  // TODO: slider 97, jpeg
  // TODO: also set gconf variables.
  // TODO: move gconf to ui_last/plugins_lighttable/* ?
}

void
gui_init (dt_lib_module_t *self)
{
  dt_lib_export_t *d = (dt_lib_export_t *)malloc(sizeof(dt_lib_export_t));
  self->data = (void *)d;
  self->widget = gtk_vbox_new(FALSE, 0);
  GtkBox *hbox = GTK_BOX(gtk_hbox_new(FALSE, 5));
  gtk_box_pack_start(hbox, gtk_label_new(_("quality")), FALSE, FALSE, 5);
  GtkVScale *scale = GTK_VSCALE(gtk_vscale_new_with_range(0, 100, 1));
  gtk_box_pack_start(hbox, GTK_WIDGET(scale), FALSE, FALSE, 5);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hbox), FALSE, FALSE, 0);
  hbox = GTK_BOX(gtk_hbox_new(FALSE, 5));
  d->export_format = GTK_COMBO_BOX(gtk_combo_box_new_text());
  gtk_combo_box_append_text(d->export_format, _("8-bit jpg"));
  gtk_combo_box_append_text(d->export_format, _("8-bit png"));
  gtk_combo_box_append_text(d->export_format, _("16-bit ppm"));
  gtk_combo_box_append_text(d->export_format, _("float pfm"));
  GtkButton *button = GTK_BUTTON(gtk_button_new_with_label(_("export")));
  gtk_box_pack_start(hbox, GTK_WIDGET(d->export_format), FALSE, FALSE, 5);
  gtk_box_pack_start(hbox, GTK_WIDGET(button), FALSE, FALSE, 5);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hbox), FALSE, FALSE, 0);

  g_signal_connect (G_OBJECT (button), "clicked",
                    G_CALLBACK (export_button_clicked),
                    (gpointer)0);
  g_signal_connect (G_OBJECT (scale), "value-changed",
                    G_CALLBACK (export_quality_changed),
                    (gpointer)0);
}

void
gui_cleanup (dt_lib_module_t *self)
{
  free(self->data);
}


