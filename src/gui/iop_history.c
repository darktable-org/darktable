/*
    This file is part of darktable,
    copyright (c) 2009--2010 Henrik Andersson.

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
#include "gui/styles.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/styles.h"
#include "develop/develop.h"
#include "control/control.h"
#include "gui/iop_history.h"
#include "dtgtk/button.h"


static void
history_compress_clicked (GtkWidget *widget, gpointer user_data)
{
  const int imgid = darktable.develop->image ? darktable.develop->image->id : 0;
  if(!imgid) return;
  // make sure the right history is in there:
  dt_dev_write_history(darktable.develop);
  sqlite3_stmt *stmt;

  DT_DEBUG_SQLITE3_EXEC(darktable.db, "begin", NULL, NULL, NULL);
  DT_DEBUG_SQLITE3_PREPARE_V2(darktable.db, "create temporary table temp_history as select * from history as a where imgid = ?1 and num in (select MAX(num) from history as b where imgid = ?1 and a.operation = b.operation) order by num", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  // dreggn, need this for some reason with newer sqlite3:
  DT_DEBUG_SQLITE3_EXEC(darktable.db, "delete from temp_history", NULL, NULL, NULL);
  DT_DEBUG_SQLITE3_PREPARE_V2(darktable.db, "insert into temp_history select * from history as a where imgid = ?1 and num in (select MAX(num) from history as b where imgid = ?1 and a.operation = b.operation) order by num", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  DT_DEBUG_SQLITE3_PREPARE_V2(darktable.db, "delete from history where imgid = ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  DT_DEBUG_SQLITE3_EXEC(darktable.db, "insert into history select imgid,rowid-1,module,operation,op_params,enabled,blendop_params from temp_history", NULL, NULL, NULL);
  DT_DEBUG_SQLITE3_EXEC(darktable.db, "delete from temp_history", NULL, NULL, NULL);
  // fails anyways:
  // DT_DEBUG_SQLITE3_EXEC(darktable.db, "drop table temp_history", NULL, NULL, NULL);
  DT_DEBUG_SQLITE3_EXEC(darktable.db, "end", NULL, NULL, NULL);
  dt_dev_reload_history_items(darktable.develop);
}

static void
history_button_clicked (GtkWidget *widget, gpointer user_data)
{
  static int reset = 0;
  if(reset) return;
  if(!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget))) return;

  GtkWidget *hbody = darktable.gui->widgets.history_expander_body;
  GtkWidget *hbox = g_list_nth_data (gtk_container_get_children (GTK_CONTAINER (hbody)), 0);
  reset = 1;

  /* inactivate all toggle buttons */
  GList *children = gtk_container_get_children (GTK_CONTAINER (hbox));
  for(int i=0; i<g_list_length (children); i++)
  {
    GtkToggleButton *b = GTK_TOGGLE_BUTTON( g_list_nth_data (children,i));
    if(b != GTK_TOGGLE_BUTTON(widget))
      g_object_set(G_OBJECT(b), "active", FALSE, (char *)NULL);
  }

  reset = 0;
  if(darktable.gui->reset) return;

  /* revert to given history item. */
  long int num = (long int)user_data;
  dt_dev_pop_history_items (darktable.develop, num);
}

static void
create_style_button_clicked (GtkWidget *widget, gpointer user_data)
{
  if(darktable.develop->image)
  {
    dt_dev_write_history(darktable.develop);
    dt_gui_styles_dialog_new (darktable.develop->image->id);
  }
}

static void apply_style_activate (gchar *name)
{
  dt_control_log(_("applied style `%s' on current image"),name);
  dt_styles_apply_to_image (name, FALSE, darktable.develop->image->id);
  dt_dev_raw_reload(darktable.develop);
}

static void
apply_style_button_press (GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{

  GList *styles = dt_styles_get_list("");
  GtkWidget *menu = NULL;
  if(styles)
  {
    menu= gtk_menu_new();
    do
    {
      dt_style_t *style=(dt_style_t *)styles->data;
      GtkWidget *mi=gtk_menu_item_new_with_label(style->name);
      gtk_menu_append (GTK_MENU (menu), mi);
      gtk_signal_connect_object (GTK_OBJECT (mi), "activate",
                                 GTK_SIGNAL_FUNC (apply_style_activate),
                                 (gpointer) g_strdup (style->name));
      gtk_widget_show (mi);
    }
    while ((styles=g_list_next(styles))!=NULL);
  }

  /* if we got any styles, lets popup menu for selection */
  if (menu)
  {
    gtk_menu_popup (GTK_MENU(menu), NULL, NULL, NULL, NULL,
                    event->button, event->time);
  }
  else dt_control_log(_("no styles have been created yet"));
}

void
dt_gui_iop_history_init ()
{
  GtkWidget *hbody = darktable.gui->widgets.history_expander_body;
  GtkWidget *hhbox = gtk_hbox_new (FALSE,2);
  GtkWidget *hvbox = gtk_vbox_new (FALSE,0);
  GtkWidget *hbutton = gtk_button_new_with_label (_("compress history stack"));
  g_object_set (G_OBJECT (hbutton), "tooltip-text", _("create a minimal history stack which produces the same image"), (char *)NULL);
  gtk_box_pack_start (GTK_BOX (hbody),hvbox,FALSE,FALSE,0);
  g_signal_connect (G_OBJECT (hbutton), "clicked", G_CALLBACK (history_compress_clicked),(gpointer)0);

  /* add toolbar button for creating style */
  GtkWidget *hbutton2 = dtgtk_button_new (dtgtk_cairo_paint_styles,0);
  //gtk_widget_set_size_request (hbutton,24,-1);
  g_signal_connect (G_OBJECT (hbutton2), "clicked", G_CALLBACK (create_style_button_clicked),(gpointer)0);
  g_object_set (G_OBJECT (hbutton2), "tooltip-text", _("create a style from the current history stack"), (char *)NULL);

  /* add toolbar button for applying a style */
  GtkWidget *hbutton3 = dtgtk_button_new (dtgtk_cairo_paint_styles,1);
  //gtk_widget_set_size_request (hbutton,24,-1);
  g_signal_connect (G_OBJECT (hbutton3), "button-press-event", G_CALLBACK (apply_style_button_press),(gpointer)0);
  g_object_set (G_OBJECT (hbutton3), "tooltip-text", _("applies a style selected from popup menu"), (char *)NULL);


  gtk_box_pack_start (GTK_BOX (hhbox),hbutton,TRUE,TRUE,0);
  gtk_box_pack_start (GTK_BOX (hhbox),hbutton2,FALSE,FALSE,0);
  gtk_box_pack_start (GTK_BOX (hhbox),hbutton3,FALSE,FALSE,0);
  gtk_box_pack_start (GTK_BOX (hbody),hhbox,FALSE,FALSE,0);


  gtk_widget_show_all (hbody);
  gtk_widget_show_all (hhbox);

}

void
dt_gui_iop_history_reset ()
{
  GtkWidget *hbody = darktable.gui->widgets.history_expander_body;
  GtkWidget *hvbox =  g_list_nth_data (gtk_container_get_children (GTK_CONTAINER (hbody)), 0);

  /* clear from history items */
  gtk_container_foreach (GTK_CONTAINER (hvbox),(GtkCallback)gtk_widget_destroy,NULL);

  /* add default history entry */
  GtkWidget *b=dt_gui_iop_history_add_item (-1, "orginal");
  gtk_button_set_label (GTK_BUTTON (b), _("0 - original"));
}

GtkWidget *
dt_gui_iop_history_add_item (long int num, const gchar *label)
{
  GtkWidget *hbody = darktable.gui->widgets.history_expander_body;
  GtkWidget *hvbox = g_list_nth_data (gtk_container_get_children (GTK_CONTAINER (hbody)), 0);

  GList *items = gtk_container_get_children (GTK_CONTAINER (hvbox));

  /* let's check if top item has same label as this hist change label */
  if( g_list_nth_data (items,0) && strcmp (gtk_button_get_label ( GTK_BUTTON (g_list_nth_data (items,0))),label) ==0 )
    return GTK_WIDGET(g_list_nth_data (items,0));

  /* add a new */
  num++;

  /* create label */
  GtkWidget *widget = NULL;
  gchar numlabel[256];
  g_snprintf(numlabel, 256, "%ld - %s", num, label);

  /* create toggle button */
  widget =  dtgtk_togglebutton_new_with_label (numlabel,NULL,CPF_STYLE_FLAT);
  g_object_set_data (G_OBJECT (widget),"history_number",(gpointer)num);
  g_object_set_data (G_OBJECT (widget),"label",(gpointer) g_strdup(label));

  g_signal_connect (G_OBJECT (widget), "clicked",
                    G_CALLBACK (history_button_clicked),
                    (gpointer)num);

  gtk_box_pack_start (GTK_BOX (hvbox),widget,FALSE,FALSE,0);
  gtk_box_reorder_child (GTK_BOX (hvbox),widget,0);
  gtk_widget_show(widget);

  /* */
  darktable.gui->reset = 1;
  g_object_set(G_OBJECT(widget), "active", TRUE, (char *)NULL);
  darktable.gui->reset = 0;
  return widget;
}

long int
dt_gui_iop_history_get_top()
{
  GtkWidget *hbody = darktable.gui->widgets.history_expander_body;
  GtkWidget *hvbox = g_list_nth_data (gtk_container_get_children (GTK_CONTAINER (hbody)), 0);

  GtkWidget *th = g_list_nth_data (gtk_container_get_children (GTK_CONTAINER (hvbox)), 0);
  return (long int)g_object_get_data (G_OBJECT (th),"history_number");
}

void
dt_gui_iop_history_pop_top()
{
  GtkWidget *hbody = darktable.gui->widgets.history_expander_body;
  GtkWidget *hvbox = g_list_nth_data (gtk_container_get_children (GTK_CONTAINER (hbody)), 0);

  /* remove top */
  gtk_widget_destroy (GTK_WIDGET (g_list_nth_data (gtk_container_get_children (GTK_CONTAINER (hvbox)), 0)) );

  /* activate new top */
  g_object_set(G_OBJECT (g_list_nth_data (gtk_container_get_children (GTK_CONTAINER (hvbox)), 0)) , "active", TRUE, (char *)NULL);
}

void
dt_gui_iop_history_update_labels ()
{
  GtkWidget *hbody = darktable.gui->widgets.history_expander_body;
  GtkWidget *hvbox = g_list_nth_data (gtk_container_get_children (GTK_CONTAINER (hbody)), 0);
  GList *items = gtk_container_get_children (GTK_CONTAINER (hvbox));

  /* update labels for all hist items excluding oringal */
  int hsize = g_list_length(darktable.develop->history);
  for(int i=0; i<hsize; i++)
  {
    gchar numlabel[256]= {0}, numlabel2[256]= {0};
    dt_dev_history_item_t *hist = (dt_dev_history_item_t *)g_list_nth_data (darktable.develop->history, i);
    if( !hist ) break;

    /* get new label */
    dt_dev_get_history_item_label (hist, numlabel2, 256);
    snprintf(numlabel, 256, "%d - %s", i+1, numlabel2);

    /* update ui hist item label from bottom to top */
    GtkWidget *button=g_list_nth_data (items,(hsize-1)-i);
    if(button) gtk_button_set_label (GTK_BUTTON (button),numlabel);
  }
  // might not yet be inited when popping just before pushing a new history item:
  GtkWidget *button=g_list_nth_data (items, hsize);
  if(button) gtk_button_set_label (GTK_BUTTON (button), _("0 - original"));
}
