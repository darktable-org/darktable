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
#include "common/debug.h"
#include "control/control.h"
#include "control/conf.h"
#include "common/image_cache.h"
#include "develop/develop.h"
#include "libs/lib.h"
#include "gui/gtk.h"
#include "dtgtk/button.h"

DT_MODULE(1)

typedef struct dt_lib_viewswitcher_t
{

}
dt_lib_viewswitcher_t;

/* callback when a view label is pressed */
static gboolean _lib_viewswitcher_button_press_callback(GtkWidget *w,GdkEventButton *ev,gpointer user_data);
/* helper function to create a label */
static GtkWidget* _lib_viewswitcher_create_label(dt_view_t *view);
/* callback when view changed signal happens */
static void _lib_viewswitcher_view_changed_callback(gpointer instance, gpointer user_data);

const char* name()
{
  return _("viewswitcher");
}

uint32_t views()
{
  return DT_VIEW_ALL;
}

uint32_t container()
{
  return DT_UI_CONTAINER_PANEL_TOP_RIGHT;
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
  dt_lib_viewswitcher_t *d = (dt_lib_viewswitcher_t *)g_malloc(sizeof(dt_lib_viewswitcher_t));
  self->data = (void *)d;
  memset(d,0,sizeof(dt_lib_viewswitcher_t));

  self->widget = gtk_hbox_new(FALSE,5);

  for (int k=0; k<darktable.view_manager->num_views; k++)
  {
    if (darktable.view_manager->view[k].module)
    {
      /* create view label */
      dt_view_t *v = &darktable.view_manager->view[k];
      GtkWidget *w = _lib_viewswitcher_create_label(v);
      gtk_box_pack_start(GTK_BOX(self->widget),w,FALSE,FALSE,0);

      /* create space if more views */
      if (k < darktable.view_manager->num_views-1)
      {
        GtkWidget *w = gtk_label_new("<span color=\"#7f7f7f\"><big><big><b>|</b></big></big></span>");
        gtk_misc_set_alignment(GTK_MISC(w), 0, 0.5);
        gtk_label_set_use_markup(GTK_LABEL(w), TRUE);
        gtk_widget_set_name(w,"view_label");
        gtk_box_pack_start(GTK_BOX(self->widget),w,FALSE,FALSE,5);
      }

    }
  }

  /* connect callback to view change signal */
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_VIEWMANAGER_VIEW_CHANGED,
                            G_CALLBACK(_lib_viewswitcher_view_changed_callback), self);
}

void gui_cleanup(dt_lib_module_t *self)
{
  g_free(self->data);
  self->data = NULL;
}

#define LABEL_HIGHLIGHTED  "<span color=\"#a0a0a0\"><big><big><b>%s</b></big></big></span>"
#define LABEL_SELECTED     "<span color=\"#afafaf\"><big><big><b>%s</b></big></big></span>"
#define LABEL_DEFAULT      "<span color=\"#7f7f7f\"><big><big><b>%s</b></big></big></span>"


static void _lib_viewswitcher_enter_notify_callback(GtkWidget *w, GdkEventCrossing *e, gpointer user_data)
{
  char label[512]= {0};
  GtkLabel *l = (GtkLabel *)user_data;

  /* if not active view lets highlight */
  if (strcmp(g_object_get_data(G_OBJECT(w),"view-label"),
             dt_view_manager_name(darktable.view_manager)))
  {
    g_snprintf(label,sizeof(label),LABEL_HIGHLIGHTED,
               (gchar *)g_object_get_data(G_OBJECT(w),"view-label"));
    gtk_label_set_markup(l,label);
  }
}

static void _lib_viewswitcher_leave_notify_callback(GtkWidget *w, GdkEventCrossing *e, gpointer user_data)
{
  char label[512]= {0};
  GtkLabel *l = (GtkLabel *)user_data;

  /* if not active view lets set default */
  if (strcmp(g_object_get_data(G_OBJECT(w),"view-label"),
             dt_view_manager_name(darktable.view_manager)))
  {
    g_snprintf(label,sizeof(label),LABEL_DEFAULT,
               (gchar *)g_object_get_data(G_OBJECT(w),"view-label"));
    gtk_label_set_markup(l,label);
  }
}

static void _lib_viewswitcher_view_changed_callback(gpointer instance, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t*)user_data;

  /* set default color for all labels */
  int x=0;
  GList *childs = gtk_container_get_children(GTK_CONTAINER(self->widget));
  while(childs)
  {
    x++;

    /* check if even number then continue to skip separator widgets */
    if(!(x%2))
    {
      childs = g_list_next(childs);
      continue;
    }

    GtkLabel *w = GTK_LABEL(gtk_bin_get_child(GTK_BIN(childs->data)));
    char label[512]= {0};
    /* check if current is the same as the one we iterate, then hilite */
    if(!strcmp(g_object_get_data(G_OBJECT(w),"view-label"),dt_view_manager_name(darktable.view_manager)))
      g_snprintf(label,sizeof(label),LABEL_SELECTED,
                 (gchar *)g_object_get_data(G_OBJECT(w),"view-label"));
    else
      g_snprintf(label,sizeof(label),LABEL_DEFAULT,
                 (gchar *)g_object_get_data(G_OBJECT(w),"view-label"));

    /* set label */
    gtk_label_set_markup(w,label);

    /* get next */
    childs = g_list_next(childs);
  }
}

static GtkWidget* _lib_viewswitcher_create_label(dt_view_t *v)
{
  GtkWidget *eb = gtk_event_box_new();
  char label[512]= {0};
  g_snprintf(label,sizeof(label),LABEL_DEFAULT,v->name(v));
  GtkWidget *b = gtk_label_new(label);
  gtk_container_add(GTK_CONTAINER(eb),b);
  /*setup label*/
  gtk_misc_set_alignment(GTK_MISC(b), 0, 0.5);
  g_object_set_data(G_OBJECT(b),"view-label",(gchar *)v->name(v));
  g_object_set_data(G_OBJECT(eb),"view-label",(gchar *)v->name(v));
  gtk_label_set_use_markup(GTK_LABEL(b), TRUE);
  gtk_widget_set_name(b,"view_label");

  /* connect button press handler */
  g_signal_connect(G_OBJECT(eb),"button-press-event", G_CALLBACK(_lib_viewswitcher_button_press_callback), GINT_TO_POINTER(v->view(v)));

  /* set enter/leave notify events and connect signals */
  gtk_widget_add_events(GTK_WIDGET(eb),
                        GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK);

  g_signal_connect(G_OBJECT(eb),
                   "enter-notify-event",
                   G_CALLBACK(_lib_viewswitcher_enter_notify_callback), b);
  g_signal_connect(G_OBJECT(eb),
                   "leave-notify-event",
                   G_CALLBACK(_lib_viewswitcher_leave_notify_callback), b);



  return eb;
}

static gboolean _lib_viewswitcher_button_press_callback(GtkWidget *w,GdkEventButton *ev,gpointer user_data)
{
  if(ev->button == 1)
  {
    int which = GPOINTER_TO_INT(user_data);
    /* FIXME: get rid of these mappings and old DT_xxx */
    if (which == DT_VIEW_LIGHTTABLE)
      dt_ctl_switch_mode_to(DT_LIBRARY);
    else if (which == DT_VIEW_DARKROOM)
      dt_ctl_switch_mode_to(DT_DEVELOP);
#ifdef HAVE_GPHOTO2
    else if (which == DT_VIEW_TETHERING)
      dt_ctl_switch_mode_to(DT_CAPTURE);
#endif
#ifdef HAVE_MAP
    else if (which == DT_VIEW_MAP)
      dt_ctl_switch_mode_to(DT_MAP);
#endif
    else if (which == DT_VIEW_SLIDESHOW)
      dt_ctl_switch_mode_to(DT_SLIDESHOW);

    return TRUE;
  }
  return FALSE;
}


// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
