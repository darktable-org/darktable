#include "gui/filmview.h"
#include "gui/gtk.h"
#include "control/control.h"
#include "control/conf.h"
#include "common/film.h"
#include <glade/glade.h>
#include <sqlite3.h>

typedef enum dt_gui_filmview_columns_t
{
  DT_GUI_FILM_COL_FOLDER=0,
  DT_GUI_FILM_COL_ID,
  DT_GUI_FILM_NUM_COLS
}
dt_gui_filmview_columns_t;

void
dt_gui_filmview_update(const char *filter)
{
  char filterstring[512];
  snprintf(filterstring, 512, "%%%s%%", filter);
  GtkTreeIter iter;
  GtkWidget *view = glade_xml_get_widget (darktable.gui->main_window, "treeview_film");
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(view));

  g_object_ref(model);
  gtk_tree_view_set_model(GTK_TREE_VIEW(view), NULL);

  gtk_list_store_clear(GTK_LIST_STORE(model));

  // single images
  if(g_strrstr(_("single images"), filter))
  {
    gtk_list_store_append(GTK_LIST_STORE(model), &iter);
    gtk_list_store_set (GTK_LIST_STORE(model), &iter,
                        DT_GUI_FILM_COL_FOLDER, _("single images"),
                        DT_GUI_FILM_COL_ID, (guint)1,
                        -1);
  }
  // sql query insert
  sqlite3_stmt *stmt;
  int rc;
  // TODO: datetime_created?
  rc = sqlite3_prepare_v2(darktable.db, "select id, folder from film_rolls where folder like ?1 and id != 1 order by folder", -1, &stmt, NULL);
  rc = sqlite3_bind_text(stmt, 1, filterstring, strlen(filterstring), SQLITE_TRANSIENT);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    gtk_list_store_append(GTK_LIST_STORE(model), &iter);
    gtk_list_store_set (GTK_LIST_STORE(model), &iter,
                        DT_GUI_FILM_COL_FOLDER, sqlite3_column_text(stmt, 1),
                        DT_GUI_FILM_COL_ID, (guint)sqlite3_column_int(stmt, 0),
                        -1);
  }

  gtk_tree_view_set_model(GTK_TREE_VIEW(view), model);
  g_object_unref(model);
}

static void
button_callback(GtkWidget *button, gpointer user_data)
{
  GtkTreeIter iter;
  GtkTreeModel *model = NULL;
  GtkWidget *view = glade_xml_get_widget (darktable.gui->main_window, "treeview_film");
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(view));
  if(!gtk_tree_selection_get_selected(selection, &model, &iter)) return;
  guint id;
  gtk_tree_model_get (model, &iter, 
                      DT_GUI_FILM_COL_ID, &id,
                      -1);
  switch((long int)user_data)
  {
    case 0: // remove film!
      if(id == 1) 
      {
        dt_control_log(_("single images are persistent"));
        return;
      }
      if(dt_conf_get_bool("ask_before_remove"))
      {
        GtkWidget *dialog;
        GtkWidget *win = glade_xml_get_widget (darktable.gui->main_window, "main_window");
        dialog = gtk_message_dialog_new(GTK_WINDOW(win),
            GTK_DIALOG_DESTROY_WITH_PARENT,
            GTK_MESSAGE_QUESTION,
            GTK_BUTTONS_YES_NO,
            _("do you really want to remove this film roll and all its images from the collection?"));
        gtk_window_set_title(GTK_WINDOW(dialog), _("remove film roll?"));
        gint res = gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        if(res != GTK_RESPONSE_YES) return;
      }
      if(darktable.film->id == id)
        dt_ctl_switch_mode_to(DT_MODE_NONE);
      dt_film_remove(id);
      break;
    default: // open this film id.
      (void)dt_film_open(darktable.film, id);
      dt_ctl_switch_mode_to(DT_LIBRARY);
      break;
  }
  GtkEntry *entry = GTK_ENTRY(glade_xml_get_widget (darktable.gui->main_window, "entry_film"));
  dt_gui_filmview_update(gtk_entry_get_text(entry));
}

static gboolean
entry_callback (GtkEntry *entry, GdkEventKey *event, gpointer user_data)
{
  dt_gui_filmview_update(gtk_entry_get_text(entry));
  return FALSE;
}

static void
row_activated_callback (GtkTreeView        *view,
                        GtkTreePath        *path,
                        GtkTreeViewColumn  *col,
                        gpointer            user_data)
{
  GtkTreeModel *model;
  GtkTreeIter   iter;

  model = gtk_tree_view_get_model(view);
  if (!gtk_tree_model_get_iter(model, &iter, path)) return;

  guint id;
  gtk_tree_model_get (model, &iter, 
                      DT_GUI_FILM_COL_ID, &id,
                      -1);
  (void)dt_film_open(darktable.film, id);
  dt_ctl_switch_mode_to(DT_LIBRARY);
}

void
dt_gui_filmview_init()
{
  GtkListStore *liststore = gtk_list_store_new(DT_GUI_FILM_NUM_COLS, G_TYPE_STRING, G_TYPE_UINT);

  GtkWidget *view = glade_xml_get_widget (darktable.gui->main_window, "treeview_film");
  g_signal_connect(view, "row-activated", G_CALLBACK(row_activated_callback), NULL);

  GtkTreeViewColumn *col = gtk_tree_view_column_new();
  gtk_tree_view_append_column(GTK_TREE_VIEW(view), col);
  GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
  gtk_tree_view_column_pack_start(col, renderer, TRUE);
  gtk_tree_view_column_add_attribute(col, renderer, "text", DT_GUI_FILM_COL_FOLDER);

  gtk_tree_selection_set_mode(gtk_tree_view_get_selection(GTK_TREE_VIEW(view)),
      GTK_SELECTION_SINGLE);

  gtk_tree_view_set_model(GTK_TREE_VIEW(view), GTK_TREE_MODEL(liststore));

  g_object_unref(liststore);

  dt_gui_filmview_update("");

  GtkWidget *entry = glade_xml_get_widget (darktable.gui->main_window, "entry_film");
  g_signal_connect(entry, "key-release-event", G_CALLBACK(entry_callback), NULL);

  GtkWidget *button = glade_xml_get_widget (darktable.gui->main_window, "button_film_remove");
  g_signal_connect(button, "clicked", G_CALLBACK(button_callback), (gpointer)0);
  button = glade_xml_get_widget (darktable.gui->main_window, "button_film_open");
  g_signal_connect(button, "clicked", G_CALLBACK(button_callback), (gpointer)1);
}

