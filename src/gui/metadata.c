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

#include "gui/gtk.h"
#include "gui/metadata.h"
#include "develop/develop.h"
#include "control/control.h"
#include "common/image_cache.h"
#include "common/metadata.h"

void dt_gui_metadata_update()
{
  GtkWidget *widget;
  int32_t mouse_over_id = -1;
  DT_CTL_GET_GLOBAL(mouse_over_id, lib_image_mouse_over_id);

  if(mouse_over_id >= 0)
  {
    const int ll = 512;
    char lbl[ll];
    dt_image_t *img = dt_image_cache_get(mouse_over_id, 'r');
    if(!img || img->film_id == -1)
    {
      dt_image_cache_release(img, 'r');
      goto fill_minuses;
    }
    widget = darktable.gui->widgets.metadata_label_filmroll;
    dt_image_film_roll(img, lbl, ll);
    gtk_label_set_text(GTK_LABEL(widget), lbl);
    gtk_label_set_ellipsize(GTK_LABEL(widget), PANGO_ELLIPSIZE_MIDDLE);
    g_object_set(G_OBJECT(widget), "tooltip-text", lbl, (char *)NULL);
    widget = darktable.gui->widgets.metadata_label_filename;
    snprintf(lbl, ll, "%s", img->filename);
    gtk_label_set_text(GTK_LABEL(widget), lbl);
    gtk_label_set_ellipsize(GTK_LABEL(widget), PANGO_ELLIPSIZE_MIDDLE);
    g_object_set(G_OBJECT(widget), "tooltip-text", img->filename, (char *)NULL);
    widget = darktable.gui->widgets.metadata_label_model;
    snprintf(lbl, ll, "%s", img->exif_model);
    gtk_label_set_text(GTK_LABEL(widget), lbl);
    gtk_label_set_ellipsize(GTK_LABEL(widget), PANGO_ELLIPSIZE_MIDDLE);
    g_object_set(G_OBJECT(widget), "tooltip-text", img->exif_model, (char *)NULL);
    widget = darktable.gui->widgets.metadata_label_lens;
    snprintf(lbl, ll, "%s", img->exif_lens);
    gtk_label_set_text(GTK_LABEL(widget), lbl);
    gtk_label_set_ellipsize(GTK_LABEL(widget), PANGO_ELLIPSIZE_END);
    g_object_set(G_OBJECT(widget), "tooltip-text", img->exif_lens, (char *)NULL);
    widget = darktable.gui->widgets.metadata_label_maker;
    snprintf(lbl, ll, "%s", img->exif_maker);
    gtk_label_set_text(GTK_LABEL(widget), lbl);
    gtk_label_set_ellipsize(GTK_LABEL(widget), PANGO_ELLIPSIZE_MIDDLE);
    g_object_set(G_OBJECT(widget), "tooltip-text", img->exif_maker, (char *)NULL);
    widget = darktable.gui->widgets.metadata_label_aperture;
    snprintf(lbl, ll, "F/%.1f", img->exif_aperture);
    gtk_label_set_text(GTK_LABEL(widget), lbl);
    gtk_label_set_ellipsize(GTK_LABEL(widget), PANGO_ELLIPSIZE_MIDDLE);
    widget = darktable.gui->widgets.metadata_label_exposure;
    if(img->exif_exposure <= 0.5) snprintf(lbl, ll, "1/%.0f", 1.0/img->exif_exposure);
    else                          snprintf(lbl, ll, "%.1f''", img->exif_exposure);
    gtk_label_set_text(GTK_LABEL(widget), lbl);
    gtk_label_set_ellipsize(GTK_LABEL(widget), PANGO_ELLIPSIZE_MIDDLE);
    widget = darktable.gui->widgets.metadata_label_focal_length;
    snprintf(lbl, ll, "%.0f", img->exif_focal_length);
    gtk_label_set_text(GTK_LABEL(widget), lbl);
    gtk_label_set_ellipsize(GTK_LABEL(widget), PANGO_ELLIPSIZE_MIDDLE);
    widget = darktable.gui->widgets.metadata_label_focus_distance;
    snprintf(lbl, ll, "%.0f", img->exif_focus_distance);
    gtk_label_set_text(GTK_LABEL(widget), lbl);
    gtk_label_set_ellipsize(GTK_LABEL(widget), PANGO_ELLIPSIZE_MIDDLE);
    widget = darktable.gui->widgets.metadata_label_iso;
    snprintf(lbl, ll, "%.0f", img->exif_iso);
    gtk_label_set_text(GTK_LABEL(widget), lbl);
    gtk_label_set_ellipsize(GTK_LABEL(widget), PANGO_ELLIPSIZE_MIDDLE);
    widget = darktable.gui->widgets.metadata_label_datetime;
    snprintf(lbl, ll, "%s", img->exif_datetime_taken);
    gtk_label_set_text(GTK_LABEL(widget), lbl);
    gtk_label_set_ellipsize(GTK_LABEL(widget), PANGO_ELLIPSIZE_MIDDLE);
    g_object_set(G_OBJECT(widget), "tooltip-text", img->exif_datetime_taken, (char *)NULL);
    widget = darktable.gui->widgets.metadata_label_width;
    snprintf(lbl, ll, "%d", img->width);
    gtk_label_set_text(GTK_LABEL(widget), lbl);
    gtk_label_set_ellipsize(GTK_LABEL(widget), PANGO_ELLIPSIZE_MIDDLE);
    widget = darktable.gui->widgets.metadata_label_height;
    snprintf(lbl, ll, "%d", img->height);
    gtk_label_set_text(GTK_LABEL(widget), lbl);
    gtk_label_set_ellipsize(GTK_LABEL(widget), PANGO_ELLIPSIZE_MIDDLE);

    widget = darktable.gui->widgets.metadata_label_title;
    GList *res = dt_metadata_get(img->id, "Xmp.dc.title", NULL);
    if(res != NULL)
    {
      snprintf(lbl, ll, "%s", (char*)res->data);
      gtk_label_set_text(GTK_LABEL(widget), lbl);
      gtk_label_set_ellipsize(GTK_LABEL(widget), PANGO_ELLIPSIZE_MIDDLE);
      g_free(res->data);
      g_list_free(res);
    }
    else
      gtk_label_set_text(GTK_LABEL(widget), "-");
    widget = darktable.gui->widgets.metadata_label_creator;
    res = dt_metadata_get(img->id, "Xmp.dc.creator", NULL);
    if(res != NULL)
    {
      snprintf(lbl, ll, "%s", (char*)res->data);
      gtk_label_set_text(GTK_LABEL(widget), lbl);
      gtk_label_set_ellipsize(GTK_LABEL(widget), PANGO_ELLIPSIZE_MIDDLE);
      g_free(res->data);
      g_list_free(res);
    }
    else
      gtk_label_set_text(GTK_LABEL(widget), "-");
    widget = darktable.gui->widgets.metadata_label_rights;
    res = dt_metadata_get(img->id, "Xmp.dc.rights", NULL);
    if(res != NULL)
    {
      snprintf(lbl, ll, "%s", (char*)res->data);
      gtk_label_set_text(GTK_LABEL(widget), lbl);
      gtk_label_set_ellipsize(GTK_LABEL(widget), PANGO_ELLIPSIZE_MIDDLE);
      g_free(res->data);
      g_list_free(res);
    }
    else
      gtk_label_set_text(GTK_LABEL(widget), "-");

    dt_image_cache_release(img, 'r');
  }
  return;
fill_minuses:
  widget = darktable.gui->widgets.metadata_label_filename;
  gtk_label_set_text(GTK_LABEL(widget), "-");
  widget = darktable.gui->widgets.metadata_label_model;
  gtk_label_set_text(GTK_LABEL(widget), "-");
  widget = darktable.gui->widgets.metadata_label_maker;
  gtk_label_set_text(GTK_LABEL(widget), "-");
  widget = darktable.gui->widgets.metadata_label_aperture;
  gtk_label_set_text(GTK_LABEL(widget), "-");
  widget = darktable.gui->widgets.metadata_label_exposure;
  gtk_label_set_text(GTK_LABEL(widget), "-");
  widget = darktable.gui->widgets.metadata_label_focal_length;
  gtk_label_set_text(GTK_LABEL(widget), "-");
  widget = darktable.gui->widgets.metadata_label_iso;
  gtk_label_set_text(GTK_LABEL(widget), "-");
  widget = darktable.gui->widgets.metadata_label_datetime;
  gtk_label_set_text(GTK_LABEL(widget), "-");
  widget = darktable.gui->widgets.metadata_label_lens;
  gtk_label_set_text(GTK_LABEL(widget), "-");
  widget = darktable.gui->widgets.metadata_label_width;
  gtk_label_set_text(GTK_LABEL(widget), "-");
  widget = darktable.gui->widgets.metadata_label_height;
  gtk_label_set_text(GTK_LABEL(widget), "-");
  return;
}

