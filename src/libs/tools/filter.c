/*
    This file is part of darktable,
    copyright (c) 2011 Henrik Andersson.

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
#include "common/collection.h"
#include "control/control.h"
#include "control/conf.h"
#include "develop/develop.h"
#include "libs/lib.h"
#include "gui/gtk.h"

DT_MODULE(1)

typedef struct dt_lib_tool_filter_t
{
  GtkWidget *filter;
  GtkWidget *comparator;
  GtkWidget *sort;
  GtkWidget *reverse;
} dt_lib_tool_filter_t;

static const char *comparators[] = {
  "<", // DT_COLLECTION_RATING_COMP_LT = 0,
  "≤", // DT_COLLECTION_RATING_COMP_LEQ,
  "=", // DT_COLLECTION_RATING_COMP_EQ,
  "≥", // DT_COLLECTION_RATING_COMP_GEQ,
  ">", // DT_COLLECTION_RATING_COMP_GT,
  "≠", // DT_COLLECTION_RATING_COMP_NE,
};

/* proxy function to intelligently reset filter */
static void _lib_filter_reset(dt_lib_module_t *self, gboolean smart_filter);

/* callback for filter combobox change */
static void _lib_filter_combobox_changed(GtkComboBox *widget, gpointer user_data);
/* callback for sort combobox change */
static void _lib_filter_sort_combobox_changed(GtkComboBox *widget, gpointer user_data);
/* callback for reverse sort check button change */
static void _lib_filter_reverse_button_changed(GtkDarktableToggleButton *widget, gpointer user_data);
/* callback for rating comparator check button change */
static void _lib_filter_compare_button_changed(GtkDarktableToggleButton *widget, gpointer user_data);
/* updates the query and redraws the view */
static void _lib_filter_update_query(dt_lib_module_t *self);
/* make sure that the comparator button matches what is shown in the filter dropdown */
static gboolean _lib_filter_sync_combobox_and_comparator(dt_lib_module_t *self);


const char *name()
{
  return _("filter");
}

uint32_t views()
{
  /* for now, show in all view due this affects filmroll too

     TODO: Consider to add flag for all views, which prevents
           unloading/loading a module while switching views.

   */
  return DT_VIEW_ALL;
}

uint32_t container()
{
  return DT_UI_CONTAINER_PANEL_CENTER_TOP_CENTER;
}

int expandable()
{
  return 0;
}

int position()
{
  return 1001;
}

void gui_init(dt_lib_module_t *self)
{
  /* initialize ui widgets */
  dt_lib_tool_filter_t *d = (dt_lib_tool_filter_t *)g_malloc0(sizeof(dt_lib_tool_filter_t));
  self->data = (void *)d;

  self->widget = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);

  /**/
  GtkWidget *widget;

  /* list label */
  widget = gtk_label_new(_("view"));
  gtk_box_pack_start(GTK_BOX(self->widget), widget, FALSE, FALSE, 4);

  d->comparator = widget = gtk_toggle_button_new_with_label(comparators[dt_collection_get_rating_comparator(darktable.collection)]);
  gtk_box_pack_start(GTK_BOX(self->widget), widget, FALSE, FALSE, 4);
  g_signal_connect(G_OBJECT(widget), "toggled", G_CALLBACK(_lib_filter_compare_button_changed),
                   (gpointer)self);

  /* create the filter combobox */
  d->filter = widget = gtk_combo_box_text_new();
  gtk_box_pack_start(GTK_BOX(self->widget), widget, FALSE, FALSE, 0);
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(widget), _("all"));
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(widget), _("unstarred only"));
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(widget), "★");
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(widget), "★ ★");
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(widget), "★ ★ ★");
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(widget), "★ ★ ★ ★");
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(widget), "★ ★ ★ ★ ★");
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(widget), _("rejected only"));
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(widget), _("all except rejected"));

  /* select the last selected value */
  gtk_combo_box_set_active(GTK_COMBO_BOX(widget), dt_collection_get_rating(darktable.collection));

  g_signal_connect(G_OBJECT(widget), "changed", G_CALLBACK(_lib_filter_combobox_changed), (gpointer)self);

  /* sort by label */
  widget = gtk_label_new(_("sort by"));
  gtk_box_pack_start(GTK_BOX(self->widget), widget, FALSE, FALSE, 4);

  /* sort combobox */
  d->sort = widget = gtk_combo_box_text_new();
  gtk_box_pack_start(GTK_BOX(self->widget), widget, FALSE, FALSE, 0);
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(widget), _("filename"));
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(widget), _("time"));
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(widget), _("rating"));
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(widget), _("id"));
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(widget), _("color label"));

  /* select the last selected value */
  gtk_combo_box_set_active(GTK_COMBO_BOX(widget), dt_collection_get_sort_field(darktable.collection));

  g_signal_connect(G_OBJECT(widget), "changed", G_CALLBACK(_lib_filter_sort_combobox_changed), (gpointer)self);

  /* reverse order checkbutton */
  d->reverse = widget
      = dtgtk_togglebutton_new(dtgtk_cairo_paint_solid_arrow, CPF_DO_NOT_USE_BORDER | CPF_STYLE_BOX | CPF_DIRECTION_UP);
  if(darktable.collection->params.descending)
    dtgtk_togglebutton_set_paint(DTGTK_TOGGLEBUTTON(widget), dtgtk_cairo_paint_solid_arrow,
                                 CPF_DO_NOT_USE_BORDER | CPF_STYLE_BOX | CPF_DIRECTION_DOWN);

  gtk_box_pack_start(GTK_BOX(self->widget), widget, FALSE, FALSE, 0);

  /* select the last value and connect callback */
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widget),
                               dt_collection_get_sort_descending(darktable.collection));

  g_signal_connect(G_OBJECT(widget), "toggled", G_CALLBACK(_lib_filter_reverse_button_changed),
                   (gpointer)self);

  /* initialize proxy */
  darktable.view_manager->proxy.filter.module = self;
  darktable.view_manager->proxy.filter.reset_filter = _lib_filter_reset;

  g_signal_connect_swapped(G_OBJECT(d->comparator), "map",
                           G_CALLBACK(_lib_filter_sync_combobox_and_comparator), self);
}

void gui_cleanup(dt_lib_module_t *self)
{
  g_free(self->data);
  self->data = NULL;
}

// TODO: decide what to do with the button when not in explicit star mode.
//       - insensitive makes a bright button and is ugly
//       - changing the string makes the width change slightly
//       - leaving it clickable is probably confusing as it doesn't do anything then
static gboolean _lib_filter_sync_combobox_and_comparator(dt_lib_module_t *self)
{
  dt_lib_tool_filter_t *d = (dt_lib_tool_filter_t *)self->data;
  int filter = gtk_combo_box_get_active(GTK_COMBO_BOX(d->filter));
  //   dt_collection_rating_comperator_t comparator =
  //   dt_collection_get_rating_comparator(darktable.collection);

  // 0 all
  // 1 unstarred only
  // 2 ★
  // 3 ★ ★
  // 4 ★ ★ ★
  // 5 ★ ★ ★ ★
  // 6 ★ ★ ★ ★ ★
  // 7 rejected only
  // 8 all except rejected

  g_signal_handlers_block_by_func(d->comparator, _lib_filter_compare_button_changed, self);

  if(filter > 1 && filter < 7)
  {
    // stars -> enable d->comparator and set its text to comparators[comparator]
    //     gtk_widget_set_sensitive(d->comparator, TRUE);
    //     gtk_button_set_label(GTK_BUTTON(d->comparator), comparators[comparator]);
    gtk_widget_show(d->comparator);
  }
  else
  {
    // disable d->comparator and set its text to something funny or stupid or just different
    //     gtk_widget_set_sensitive(d->comparator, FALSE);
    //     gtk_button_set_label(GTK_BUTTON(d->comparator), " ");
    gtk_widget_hide(d->comparator);
  }

  g_signal_handlers_unblock_by_func(d->comparator, _lib_filter_compare_button_changed, self);

  return FALSE;
}

static void _lib_filter_combobox_changed(GtkComboBox *widget, gpointer user_data)
{
  /* update last settings */
  int i = gtk_combo_box_get_active(widget);

  /* update collection star filter flags */
  if(i == 0) // all
    dt_collection_set_filter_flags(darktable.collection,
                                   dt_collection_get_filter_flags(darktable.collection)
                                   & ~(COLLECTION_FILTER_ATLEAST_RATING | COLLECTION_FILTER_EQUAL_RATING
                                       | COLLECTION_FILTER_CUSTOM_COMPARE));
  else if(i == 1 || i == 7) // unstarred only || rejected only
    dt_collection_set_filter_flags(
        darktable.collection,
        (dt_collection_get_filter_flags(darktable.collection) | COLLECTION_FILTER_EQUAL_RATING)
        & ~(COLLECTION_FILTER_ATLEAST_RATING | COLLECTION_FILTER_CUSTOM_COMPARE));
  else if(i == 8) // all except rejected
    dt_collection_set_filter_flags(darktable.collection,
                                   (dt_collection_get_filter_flags(darktable.collection)
                                    | COLLECTION_FILTER_ATLEAST_RATING) & ~COLLECTION_FILTER_CUSTOM_COMPARE);
  else // explicit stars
    dt_collection_set_filter_flags(darktable.collection, dt_collection_get_filter_flags(darktable.collection)
                                                         | COLLECTION_FILTER_CUSTOM_COMPARE);

  /* set the star filter in collection */
  dt_collection_set_rating(darktable.collection, i);

  /* update the gui accordingly */
  _lib_filter_sync_combobox_and_comparator(user_data);

  /* update the query and view */
  _lib_filter_update_query(user_data);
}

static void _lib_filter_reverse_button_changed(GtkDarktableToggleButton *widget, gpointer user_data)
{
  gboolean reverse = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));

  if(reverse)
    dtgtk_togglebutton_set_paint(widget, dtgtk_cairo_paint_solid_arrow, CPF_DO_NOT_USE_BORDER | CPF_STYLE_BOX | CPF_DIRECTION_DOWN);
  else
    dtgtk_togglebutton_set_paint(widget, dtgtk_cairo_paint_solid_arrow, CPF_DO_NOT_USE_BORDER | CPF_STYLE_BOX | CPF_DIRECTION_UP);
  gtk_widget_queue_draw(GTK_WIDGET(widget));

  /* update last settings */
  dt_collection_set_sort(darktable.collection, -1, reverse);

  /* update query and view */
  _lib_filter_update_query(user_data);
}

static void _lib_filter_compare_button_changed(GtkDarktableToggleButton *widget, gpointer user_data)
{
  g_signal_handlers_block_by_func(widget, _lib_filter_compare_button_changed, user_data);

  dt_collection_rating_comperator_t comparator = dt_collection_get_rating_comparator(darktable.collection);
  comparator = (comparator + 1) % DT_COLLECTION_RATING_N_COMPS;
  dt_collection_set_rating_comparator(darktable.collection, comparator);

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widget), FALSE);
  gtk_button_set_label(GTK_BUTTON(widget), comparators[comparator]);

  g_signal_handlers_unblock_by_func(widget, _lib_filter_compare_button_changed, user_data);

  _lib_filter_update_query(user_data);
}

static void _lib_filter_sort_combobox_changed(GtkComboBox *widget, gpointer user_data)
{
  /* update the ui last settings */
  dt_collection_set_sort(darktable.collection, gtk_combo_box_get_active(widget), -1);

  /* update the query and view */
  _lib_filter_update_query(user_data);
}

static void _lib_filter_update_query(dt_lib_module_t *self)
{
  /* sometimes changes */
  dt_collection_set_query_flags(darktable.collection, COLLECTION_QUERY_FULL);

  /* updates query */
  dt_collection_update_query(darktable.collection);

  /* update film strip, jump to currently opened image, if any: */
  dt_view_filmstrip_scroll_to_image(darktable.view_manager, darktable.develop->image_storage.id, FALSE);
}

static void _lib_filter_reset(dt_lib_module_t *self, gboolean smart_filter)
{
  dt_lib_tool_filter_t *dropdowns = (dt_lib_tool_filter_t *)self->data;

  if(smart_filter == TRUE)
  {
    /* initial import rating setting */
    int initial_rating = dt_conf_get_int("ui_last/import_initial_rating");

    /* current selection in filter dropdown */
    int current_filter = gtk_combo_box_get_active(GTK_COMBO_BOX(dropdowns->filter));

    /* convert filter dropdown to rating: 2-6 is 1-5 stars, for anything else, assume 0 stars */
    int current_filter_rating = (current_filter >= 2 && current_filter <= 6) ? current_filter - 1 : 0;

    /* new filter is the lesser of the initial rating and the current filter rating */
    int new_filter_rating = MIN(initial_rating, current_filter_rating);

    /* convert new filter rating to filter dropdown selector */
    int new_filter = (new_filter_rating >= 1 && new_filter_rating <= 5) ? new_filter_rating + 1
                                                                        : new_filter_rating;

    /* Reset to new filter dropdown item */
    gtk_combo_box_set_active(GTK_COMBO_BOX(dropdowns->filter), new_filter);
  }
  else
  {
    /* Reset to topmost item, 'all' */
    gtk_combo_box_set_active(GTK_COMBO_BOX(dropdowns->filter), 0);
  }
}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
