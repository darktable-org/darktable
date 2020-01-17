/*
    This file is part of darktable,
    copyright (c) 2019--2020 Aldric Renaudin.

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
/** this is the thumbnail class for the lighttable module.  */
#include <glib.h>
#include <gtk/gtk.h>

typedef struct
{
  int imgid;
  int width, height;         // current thumb size (with the background and the border)
  int x, y;                  // current position at screen
  int img_width, img_height; // current image only size

  gboolean mouse_over;
  gboolean selected;

  gboolean thumb_border; // TODO : replace with the corresponding state of the image (on-screen ?)
  gboolean image_border; // TODO : replace with the corresponding state of the image (on-screen ?)

  GtkWidget *w_main;
  GtkWidget *w_back;
  cairo_surface_t *img_surf;
  GtkWidget *w_info_back_eb;
  GtkWidget *w_info_back;
  GtkWidget *w_btn_audio;
  GtkWidget *w_btn_group;
  GtkWidget *w_btn_reject;
  GtkWidget *w_stars[4];
  GtkWidget *w_stars_box;

} dt_thumbnail_t;

dt_thumbnail_t *dt_thumbnail_new(int width, int height, int imgid);
void dt_thumbnail_destroy(dt_thumbnail_t *thumb);
GtkWidget *dt_thumbnail_create_widget(dt_thumbnail_t *thumb);


// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;