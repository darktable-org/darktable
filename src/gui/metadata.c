
#include "gui/gtk.h"
#include "gui/metadata.h"
#include "library/library.h"
#include "develop/develop.h"
#include "control/control.h"
#include "common/image_cache.h"

void dt_gui_metadata_update()
{
  GtkWidget *widget;
  int32_t mouse_over_id = -1;
  DT_CTL_GET_GLOBAL(mouse_over_id, lib_image_mouse_over_id);
  
  if(mouse_over_id >= 0)
  {
    char lbl[30];
    dt_image_t *img = dt_image_cache_use(mouse_over_id, 'r');
    if(!img || img->film_id == -1)
    {
      dt_image_cache_release(img, 'r');
      goto fill_minuses;
    }
    widget = glade_xml_get_widget (darktable.gui->main_window, "metadata_label_filename");
    gtk_label_set_text(GTK_LABEL(widget), img->filename);
    widget = glade_xml_get_widget (darktable.gui->main_window, "metadata_label_model");
    gtk_label_set_text(GTK_LABEL(widget), img->exif_model);
    widget = glade_xml_get_widget (darktable.gui->main_window, "metadata_label_maker");
    gtk_label_set_text(GTK_LABEL(widget), img->exif_maker);
    widget = glade_xml_get_widget (darktable.gui->main_window, "metadata_label_aperture");
    snprintf(lbl, 30, "F/%.1f", img->exif_aperture);
    gtk_label_set_text(GTK_LABEL(widget), lbl);
    widget = glade_xml_get_widget (darktable.gui->main_window, "metadata_label_exposure");
    if(img->exif_exposure <= 0.5) snprintf(lbl, 30, "1/%.0f", 1.0/img->exif_exposure);
    else                          snprintf(lbl, 30, "%.1f''", img->exif_exposure);
    gtk_label_set_text(GTK_LABEL(widget), lbl);
    widget = glade_xml_get_widget (darktable.gui->main_window, "metadata_label_focal_length");
    snprintf(lbl, 30, "%.0f", img->exif_focal_length);
    gtk_label_set_text(GTK_LABEL(widget), lbl);
    widget = glade_xml_get_widget (darktable.gui->main_window, "metadata_label_iso");
    snprintf(lbl, 30, "%.0f", img->exif_iso);
    gtk_label_set_text(GTK_LABEL(widget), lbl);
    widget = glade_xml_get_widget (darktable.gui->main_window, "metadata_label_datetime");
    gtk_label_set_text(GTK_LABEL(widget), img->exif_datetime_taken);
    dt_image_cache_release(img, 'r');
  }
  return;
fill_minuses:
  widget = glade_xml_get_widget (darktable.gui->main_window, "metadata_label_filename");
  gtk_label_set_text(GTK_LABEL(widget), "-");
  widget = glade_xml_get_widget (darktable.gui->main_window, "metadata_label_model");
  gtk_label_set_text(GTK_LABEL(widget), "-");
  widget = glade_xml_get_widget (darktable.gui->main_window, "metadata_label_maker");
  gtk_label_set_text(GTK_LABEL(widget), "-");
  widget = glade_xml_get_widget (darktable.gui->main_window, "metadata_label_aperture");
  gtk_label_set_text(GTK_LABEL(widget), "-");
  widget = glade_xml_get_widget (darktable.gui->main_window, "metadata_label_exposure");
  gtk_label_set_text(GTK_LABEL(widget), "-");
  widget = glade_xml_get_widget (darktable.gui->main_window, "metadata_label_focal_length");
  gtk_label_set_text(GTK_LABEL(widget), "-");
  widget = glade_xml_get_widget (darktable.gui->main_window, "metadata_label_iso");
  gtk_label_set_text(GTK_LABEL(widget), "-");
  widget = glade_xml_get_widget (darktable.gui->main_window, "metadata_label_datetime");
  gtk_label_set_text(GTK_LABEL(widget), "-");
  return;
}

