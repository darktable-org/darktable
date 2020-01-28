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

typedef enum dt_thumbnail_border_t
{
  DT_THUMBNAIL_BORDER_NONE = 0,
  DT_THUMBNAIL_BORDER_LEFT = 1 << 0,
  DT_THUMBNAIL_BORDER_TOP = 1 << 1,
  DT_THUMBNAIL_BORDER_RIGHT = 1 << 2,
  DT_THUMBNAIL_BORDER_BOTTOM = 1 << 3,
} dt_thumbnail_border_t;

typedef struct
{
  int imgid, rowid;
  int width, height;         // current thumb size (with the background and the border)
  int x, y;                  // current position at screen
  int img_width, img_height; // current image only size

  gboolean mouse_over;
  gboolean selected;
  gboolean active; // used for filmstrip to mark images worked on

  int rating;
  int colorlabels;
  gchar *filename;
  gchar info_line[50];
  gboolean is_altered;
  gboolean has_audio;
  gboolean is_grouped;
  gboolean has_localcopy;
  int groupid;

  GtkWidget *w_main;
  GtkWidget *w_back;
  GtkWidget *w_ext;

  GtkWidget *w_image;
  cairo_surface_t *img_surf;

  GtkWidget *w_bottom_eb;
  GtkWidget *w_bottom;
  GtkWidget *w_reject;
  GtkWidget *w_stars[5];
  GtkWidget *w_color;

  GtkWidget *w_local_copy;
  GtkWidget *w_altered;
  GtkWidget *w_group;
  GtkWidget *w_audio;

  gboolean moved; // indicate if the thumb is currently moved (zoomable thumbtable case)

  dt_thumbnail_border_t group_borders;
} dt_thumbnail_t;

dt_thumbnail_t *dt_thumbnail_new(int width, int height, int imgid, int rowid);
void dt_thumbnail_destroy(dt_thumbnail_t *thumb);
GtkWidget *dt_thumbnail_create_widget(dt_thumbnail_t *thumb);
void dt_thumbnail_resize(dt_thumbnail_t *thumb, int width, int height);
void dt_thumbnail_set_group_border(dt_thumbnail_t *thumb, dt_thumbnail_border_t border);
void dt_thumbnail_set_mouseover(dt_thumbnail_t *thumb, gboolean over);

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;