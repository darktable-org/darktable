/*
    This file is part of darktable,
    copyright (c) 2011 Henrik Andersson.
    copyright (c) 2012 Tobias Ellinghaus.

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
#include "common/debug.h"
#include "control/control.h"
#include "control/conf.h"
#include "common/image_cache.h"
#include "develop/develop.h"
#include "libs/lib.h"
#include "gui/gtk.h"
#include "gui/draw.h"

DT_MODULE(1)

#define DT_MODULE_LIST_SPACING DT_PIXEL_APPLY_DPI(2)

#define ICON_SIZE DT_PIXEL_APPLY_DPI(20)
typedef struct dt_lib_modulelist_t
{
  GtkTreeView *tree;
  GdkPixbuf *fav_pixbuf;
} dt_lib_modulelist_t;

/* handle iop module click */
static void _lib_modulelist_row_changed_callback(GtkTreeView *tree_view, gpointer user_data);
/* callback for iop modules loaded signal */
static void _lib_modulelist_populate_callback(gpointer instance, gpointer user_data);
/* callback that makes sure that the tree is repopulated when the style changes */
static void _lib_modulelist_style_set(GtkWidget *widget, GtkStyle *previous_style, gpointer user_data);
/* force refresh of tree */
static void _lib_modulelist_gui_update(struct dt_lib_module_t *);
/* helper for sorting */
static gint _lib_modulelist_gui_sort(GtkTreeModel *model, GtkTreeIter *a, GtkTreeIter *b, gpointer userdata);

const char *name()
{
  return _("more modules");
}

uint32_t views()
{
  return DT_VIEW_DARKROOM;
}

uint32_t container()
{
  return DT_UI_CONTAINER_PANEL_RIGHT_BOTTOM;
}

int position()
{
  return 1;
}

void gui_init(dt_lib_module_t *self)
{
  /* initialize ui widgets */
  dt_lib_modulelist_t *d = (dt_lib_modulelist_t *)g_malloc0(sizeof(dt_lib_modulelist_t));
  self->data = (void *)d;
  self->widget = gtk_scrolled_window_new(
      NULL, NULL); // GTK_ADJUSTMENT(gtk_adjustment_new(200, 100, 200, 10, 100, 100))
  gtk_widget_set_size_request(self->widget, -1, DT_PIXEL_APPLY_DPI(208));
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(self->widget), GTK_POLICY_AUTOMATIC, GTK_POLICY_ALWAYS);
  d->tree = GTK_TREE_VIEW(gtk_tree_view_new());
  gtk_widget_set_size_request(GTK_WIDGET(d->tree), DT_PIXEL_APPLY_DPI(50), -1);
  gtk_container_add(GTK_CONTAINER(self->widget), GTK_WIDGET(d->tree));
  gtk_widget_set_name(GTK_WIDGET(self->widget), "lib-modulelist");

  /* connect to signal for darktable.develop initialization */
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_DEVELOP_INITIALIZE,
                            G_CALLBACK(_lib_modulelist_populate_callback), self);
  g_signal_connect(GTK_WIDGET(d->tree), "style-set", G_CALLBACK(_lib_modulelist_style_set), self);
  g_signal_connect(GTK_WIDGET(d->tree), "cursor-changed", G_CALLBACK(_lib_modulelist_row_changed_callback),
                   NULL);

  darktable.view_manager->proxy.more_module.module = self;
  darktable.view_manager->proxy.more_module.update = _lib_modulelist_gui_update;
}

void gui_cleanup(dt_lib_module_t *self)
{
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(_lib_modulelist_populate_callback), self);
  g_free(self->data);
  self->data = NULL;
}

enum
{
  COL_IMAGE = 0,
  COL_MODULE,
  NUM_COLS
};
static void image_renderer_function(GtkTreeViewColumn *col, GtkCellRenderer *renderer, GtkTreeModel *model,
                                    GtkTreeIter *iter, gpointer user_data)
{
  // FIXME: is that correct?
  GtkImage *pixbuf;
  dt_iop_module_t *module;
  gtk_tree_model_get(model, iter, COL_IMAGE, &pixbuf, -1);
  gtk_tree_model_get(model, iter, COL_MODULE, &module, -1);
  g_object_set(renderer, "pixbuf", pixbuf, NULL);
  g_object_set(renderer, "cell-background-set", module->state != dt_iop_state_HIDDEN, NULL);
  g_object_unref(pixbuf);
}
static void favorite_renderer_function(GtkTreeViewColumn *col, GtkCellRenderer *renderer, GtkTreeModel *model,
                                       GtkTreeIter *iter, gpointer user_data)
{
  dt_iop_module_t *module;
  gtk_tree_model_get(model, iter, COL_MODULE, &module, -1);
  g_object_set(renderer, "cell-background-set", module->state != dt_iop_state_HIDDEN, NULL);
  GdkPixbuf *fav_pixbuf
      = ((dt_lib_modulelist_t *)darktable.view_manager->proxy.more_module.module->data)->fav_pixbuf;
  g_object_set(renderer, "pixbuf", module->state == dt_iop_state_FAVORITE ? fav_pixbuf : NULL, NULL);
}
static void text_renderer_function(GtkTreeViewColumn *col, GtkCellRenderer *renderer, GtkTreeModel *model,
                                   GtkTreeIter *iter, gpointer user_data)
{
  dt_iop_module_t *module;
  gtk_tree_model_get(model, iter, COL_MODULE, &module, -1);
  g_object_set(renderer, "text", module->name(), NULL);
  g_object_set(renderer, "cell-background-set", module->state != dt_iop_state_HIDDEN, NULL);
}

static GdkPixbuf *load_image(const char *filename)
{
  GError *error = NULL;
  if(!g_file_test(filename, G_FILE_TEST_IS_REGULAR)) return NULL;

  GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file_at_size(filename, ICON_SIZE, ICON_SIZE, &error);
  if(!pixbuf)
  {
    fprintf(stderr, "error loading file `%s': %s\n", filename, error->message);
    g_error_free(error);
  }
  return pixbuf;
}

static const uint8_t fallback_pixel[4] = { 0, 0, 0, 0 };

static void _lib_modulelist_populate_callback(gpointer instance, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  if(!self || !(self->data)) return;

  GtkListStore *store;
  GtkTreeIter iter;
  GtkWidget *view = GTK_WIDGET(((dt_lib_modulelist_t *)self->data)->tree);
  GtkCellRenderer *pix_renderer, *fav_renderer, *text_renderer;
  GdkRGBA color;
  GtkStyleContext *context = gtk_widget_get_style_context(view);
  gboolean color_found = gtk_style_context_lookup_color (context, "selected_bg_color", &color);
  if(!color_found)
  {
    color.red = 1.0;
    color.green = 0.0;
    color.blue = 0.0;
    color.alpha = 1.0;
  }

  store = gtk_list_store_new(NUM_COLS, GDK_TYPE_PIXBUF, G_TYPE_POINTER);
  gtk_tree_view_set_model(GTK_TREE_VIEW(view), GTK_TREE_MODEL(store));
  g_object_unref(store);

  gtk_tree_sortable_set_sort_func(GTK_TREE_SORTABLE(store), COL_MODULE, _lib_modulelist_gui_sort, NULL, NULL);
  gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(store), COL_MODULE, GTK_SORT_ASCENDING);

  pix_renderer = gtk_cell_renderer_pixbuf_new();
  g_object_set(pix_renderer, "cell-background-rgba", &color, NULL);

  fav_renderer = gtk_cell_renderer_pixbuf_new();
  cairo_surface_t *fav_cst = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, ICON_SIZE, ICON_SIZE);
  cairo_t *fav_cr = cairo_create(fav_cst);
  cairo_set_source_rgb(fav_cr, 0.7, 0.7, 0.7);
  dtgtk_cairo_paint_modulegroup_favorites(fav_cr, 0, 0, ICON_SIZE, ICON_SIZE, 0);
  guchar *data = cairo_image_surface_get_data(fav_cst);
  dt_draw_cairo_to_gdk_pixbuf(data, ICON_SIZE, ICON_SIZE);
  ((dt_lib_modulelist_t *)self->data)->fav_pixbuf
      = gdk_pixbuf_new_from_data(data, GDK_COLORSPACE_RGB, TRUE, 8, ICON_SIZE, ICON_SIZE,
                                 cairo_image_surface_get_stride(fav_cst), NULL, NULL);
  g_object_set(fav_renderer, "cell-background-rgba", &color, NULL);
  g_object_set(fav_renderer, "width", gdk_pixbuf_get_width(((dt_lib_modulelist_t *)self->data)->fav_pixbuf),
               NULL);

  text_renderer = gtk_cell_renderer_text_new();
  g_object_set(text_renderer, "cell-background-rgba", &color, NULL);

  gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(view), FALSE);
  gtk_tree_view_set_headers_clickable(GTK_TREE_VIEW(view), FALSE);
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(view));
  gtk_tree_selection_set_mode(selection, GTK_SELECTION_NONE);

  GtkTreeViewColumn *col;
  col = gtk_tree_view_get_column(GTK_TREE_VIEW(view), 0);
  if(col) gtk_tree_view_remove_column(GTK_TREE_VIEW(view), col);
  gtk_tree_view_insert_column_with_data_func(GTK_TREE_VIEW(view), 0, "favorite", fav_renderer,
                                             favorite_renderer_function, NULL, NULL);
  col = gtk_tree_view_get_column(GTK_TREE_VIEW(view), 1);
  if(col) gtk_tree_view_remove_column(GTK_TREE_VIEW(view), col);
  gtk_tree_view_insert_column_with_data_func(GTK_TREE_VIEW(view), 1, "image", pix_renderer,
                                             image_renderer_function, NULL, NULL);
  col = gtk_tree_view_get_column(GTK_TREE_VIEW(view), 2);
  if(col) gtk_tree_view_remove_column(GTK_TREE_VIEW(view), col);
  gtk_tree_view_insert_column_with_data_func(GTK_TREE_VIEW(view), 2, "name", text_renderer,
                                             text_renderer_function, NULL, NULL);

  /* go thru list of iop modules and add them to the list */
  GList *modules = g_list_last(darktable.develop->iop);

  char datadir[PATH_MAX] = { 0 };
  dt_loc_get_datadir(datadir, sizeof(datadir));

  while(modules)
  {
    dt_iop_module_t *module = (dt_iop_module_t *)(modules->data);
    if(!dt_iop_is_hidden(module) && !(module->flags() & IOP_FLAGS_DEPRECATED) && module->multi_priority == 0)
    {
      GdkPixbuf *pixbuf;
      char filename[PATH_MAX] = { 0 };

      snprintf(filename, sizeof(filename), "%s/pixmaps/plugins/darkroom/%s.svg", datadir, module->op);
      pixbuf = load_image(filename);
      if(pixbuf) goto end;

      snprintf(filename, sizeof(filename), "%s/pixmaps/plugins/darkroom/%s.png", datadir, module->op);
      pixbuf = load_image(filename);
      if(pixbuf) goto end;

      snprintf(filename, sizeof(filename), "%s/pixmaps/plugins/darkroom/template.svg", datadir);
      pixbuf = load_image(filename);
      if(pixbuf) goto end;

      snprintf(filename, sizeof(filename), "%s/pixmaps/plugins/darkroom/template.png", datadir);
      pixbuf = load_image(filename);
      if(pixbuf) goto end;

      // wow, we could neither load the SVG nor the PNG files. something is fucked up.
      pixbuf = gdk_pixbuf_new_from_data(fallback_pixel, GDK_COLORSPACE_RGB, TRUE, 8, 1, 1, 4, NULL, NULL);

    end:
      gtk_list_store_append(store, &iter);
      gtk_list_store_set(store, &iter, COL_IMAGE, pixbuf, COL_MODULE, module, -1);
      g_object_unref(pixbuf);
    }

    modules = g_list_previous(modules);
  }
}

static void _lib_modulelist_style_set(GtkWidget *widget, GtkStyle *previous_style, gpointer user_data)
{
  _lib_modulelist_populate_callback(NULL, user_data);
}

static void _lib_modulelist_row_changed_callback(GtkTreeView *treeview, gpointer user_data)
{
  dt_iop_module_t *module;
  GtkTreeIter iter;
  GtkTreeModel *model;
  GtkTreePath *path;

  gtk_tree_view_get_cursor(treeview, &path, NULL);

  if(path != NULL)
  {
    model = gtk_tree_view_get_model(treeview);
    gtk_tree_model_get_iter(model, &iter, path);
    gtk_tree_path_free(path);
    gtk_tree_model_get(model, &iter, COL_MODULE, &module, -1);

    dt_iop_gui_set_state(module, (module->state + 1) % dt_iop_state_LAST);
  }
}

static void _lib_modulelist_gui_update(struct dt_lib_module_t *module)
{
  gtk_widget_queue_draw(GTK_WIDGET(((dt_lib_modulelist_t *)module->data)->tree));
}

static gint _lib_modulelist_gui_sort(GtkTreeModel *model, GtkTreeIter *a, GtkTreeIter *b, gpointer userdata)
{
  dt_iop_module_t *modulea, *moduleb;
  gtk_tree_model_get(model, a, COL_MODULE, &modulea, -1);
  gtk_tree_model_get(model, b, COL_MODULE, &moduleb, -1);
  return g_utf8_collate(modulea->name(), moduleb->name());
}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
