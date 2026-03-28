/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

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

#include "gui/duplicate_review_dialog.h"
#include "common/act_on.h"
#include "common/collection.h"
#include "common/darktable.h"
#include "common/duplicate_review.h"
#include "common/ratings.h"
#include "common/selection.h"
#include "control/jobs/control_jobs.h"
#include "gui/gtk.h"
#include "views/view.h"
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif

#include <gtk/gtk.h>

typedef struct dt_dup_review_dialog_t
{
  GtkWidget *dialog;
  GtkWidget *dup_count_label;
  GtkWidget *burst_count_label;
  GList *duplicate_ids;
  GList *burst_ids;
} dt_dup_review_dialog_t;

static void _free_lists(dt_dup_review_dialog_t *d)
{
  dt_duplicate_review_free_id_list(d->duplicate_ids);
  dt_duplicate_review_free_id_list(d->burst_ids);
  d->duplicate_ids = NULL;
  d->burst_ids = NULL;
}

static void _rescan(dt_dup_review_dialog_t *d)
{
  _free_lists(d);
  dt_collection_memory_update();
  d->duplicate_ids = dt_duplicate_review_get_duplicate_ids();
  d->burst_ids = dt_duplicate_review_get_burst_candidate_ids();

  const guint n_dup = g_list_length(d->duplicate_ids);
  const guint n_burst = g_list_length(d->burst_ids);

  gchar *dup_txt = g_strdup_printf(_("library duplicates (non-original versions): %u"), n_dup);
  gchar *burst_txt = g_strdup_printf(_("burst / similar candidates (heuristic): %u"), n_burst);
  gtk_label_set_text(GTK_LABEL(d->dup_count_label), dup_txt);
  gtk_label_set_text(GTK_LABEL(d->burst_count_label), burst_txt);
  g_free(dup_txt);
  g_free(burst_txt);
}

static void _rescan_clicked(GtkWidget *widget, gpointer user_data)
{
  _rescan((dt_dup_review_dialog_t *)user_data);
}

static void _on_destroy(GtkWidget *widget, gpointer user_data)
{
  dt_dup_review_dialog_t *d = (dt_dup_review_dialog_t *)user_data;
  _free_lists(d);
  g_free(d);
}

static void _select_duplicates(GtkWidget *widget, gpointer user_data)
{
  dt_dup_review_dialog_t *d = (dt_dup_review_dialog_t *)user_data;
  if(!d->duplicate_ids) return;
  GList *copy = g_list_copy(d->duplicate_ids);
  dt_selection_clear(darktable.selection);
  dt_selection_select_list(darktable.selection, copy);
  g_list_free(copy);
}

static void _select_burst(GtkWidget *widget, gpointer user_data)
{
  dt_dup_review_dialog_t *d = (dt_dup_review_dialog_t *)user_data;
  if(!d->burst_ids) return;
  GList *copy = g_list_copy(d->burst_ids);
  dt_selection_clear(darktable.selection);
  dt_selection_select_list(darktable.selection, copy);
  g_list_free(copy);
}

static void _reject_selected(GtkWidget *widget, gpointer user_data)
{
  GList *imgs = dt_act_on_get_images(TRUE, TRUE, FALSE);
  if(!imgs) return;
  dt_ratings_apply_on_list(imgs, DT_VIEW_REJECT, TRUE);
  g_list_free(imgs);
}

static void _delete_selected(GtkWidget *widget, gpointer user_data)
{
  dt_control_delete_images();
}

void dt_gui_duplicate_review_dialog_show(void)
{
  dt_dup_review_dialog_t *d = g_malloc0(sizeof(dt_dup_review_dialog_t));

  GtkWindow *parent = GTK_WINDOW(dt_ui_main_window(darktable.gui->ui));
  GtkWidget *dlg = gtk_dialog_new_with_buttons(
      _("duplicate review"),
      parent,
      GTK_DIALOG_DESTROY_WITH_PARENT | GTK_DIALOG_MODAL,
      _("_close"),
      GTK_RESPONSE_CLOSE,
      NULL);

  d->dialog = dlg;
  gtk_window_set_default_size(GTK_WINDOW(dlg), 480, 420);

#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(dlg);
#endif

  GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
  gtk_widget_set_margin_top(content, 10);
  gtk_widget_set_margin_bottom(content, 10);
  gtk_widget_set_margin_start(content, 10);
  gtk_widget_set_margin_end(content, 10);

  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_container_add(GTK_CONTAINER(content), vbox);

  GtkWidget *explain = gtk_label_new(
      _("this tool finds duplicate library entries (same file, different edit versions) and "
        "suggests burst shots that may be redundant.\n"
        "similar-image detection is heuristic only; review before deleting."));
  gtk_label_set_line_wrap(GTK_LABEL(explain), TRUE);
  gtk_label_set_xalign(GTK_LABEL(explain), 0.0);
  gtk_box_pack_start(GTK_BOX(vbox), explain, FALSE, FALSE, 0);

  d->dup_count_label = gtk_label_new("");
  d->burst_count_label = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(d->dup_count_label), 0.0);
  gtk_label_set_xalign(GTK_LABEL(d->burst_count_label), 0.0);
  gtk_box_pack_start(GTK_BOX(vbox), d->dup_count_label, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(vbox), d->burst_count_label, FALSE, FALSE, 0);

  GtkWidget *btn_grid = gtk_grid_new();
  gtk_grid_set_column_spacing(GTK_GRID(btn_grid), 8);
  gtk_grid_set_row_spacing(GTK_GRID(btn_grid), 8);
  gtk_box_pack_start(GTK_BOX(vbox), btn_grid, FALSE, FALSE, 0);

  GtkWidget *b1 = gtk_button_new_with_label(_("select duplicates (keep original)"));
  gtk_widget_set_tooltip_text(b1,
                              _("select non-original duplicate versions in the current collection; "
                                "the lowest version per filename is kept unselected"));
  g_signal_connect(b1, "clicked", G_CALLBACK(_select_duplicates), d);
  gtk_grid_attach(GTK_GRID(btn_grid), b1, 0, 0, 1, 1);

  GtkWidget *b2 = gtk_button_new_with_label(_("select burst candidates"));
  gtk_widget_set_tooltip_text(
      b2, _("select images suggested as redundant burst shots (same roll, close time, unstarred)"));
  g_signal_connect(b2, "clicked", G_CALLBACK(_select_burst), d);
  gtk_grid_attach(GTK_GRID(btn_grid), b2, 1, 0, 1, 1);

  GtkWidget *b3 = gtk_button_new_with_label(_("rescan"));
  g_signal_connect(b3, "clicked", G_CALLBACK(_rescan_clicked), d);
  gtk_grid_attach(GTK_GRID(btn_grid), b3, 0, 1, 1, 1);

  GtkWidget *b4 = gtk_button_new_with_label(_("reject selected"));
  g_signal_connect(b4, "clicked", G_CALLBACK(_reject_selected), d);
  gtk_grid_attach(GTK_GRID(btn_grid), b4, 1, 1, 1, 1);

  GtkWidget *b5 = gtk_button_new_with_label(_("delete selected"));
  g_signal_connect(b5, "clicked", G_CALLBACK(_delete_selected), d);
  gtk_grid_attach(GTK_GRID(btn_grid), b5, 0, 2, 2, 1);

  g_signal_connect(dlg, "destroy", G_CALLBACK(_on_destroy), d);

  _rescan(d);

  gtk_widget_show_all(dlg);
  gtk_dialog_run(GTK_DIALOG(dlg));
  gtk_widget_destroy(dlg);
}
