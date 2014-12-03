/*
    This file is part of darktable,
    copyright (c) 2009--2010 henrik andersson.

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
#ifndef DT_GUI_HIST_DIALOG
#define DT_GUI_HIST_DIALOG

typedef struct dt_gui_hist_dialog_t
{
  GList *selops;
  GtkTreeView *items;
  int copied_imageid;
} dt_gui_hist_dialog_t;

/** shows a dialog for creating a new style, w if not null is a widget to
    change the sensitive state depending on the dialog response.  */
int dt_gui_hist_dialog_new(dt_gui_hist_dialog_t *d, int imgid, gboolean iscopy);

/** must be called to initialize the structure. */
void dt_gui_hist_dialog_init(dt_gui_hist_dialog_t *d);

#endif
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
