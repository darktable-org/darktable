/*
    This file is part of darktable,
    copyright (c)2010 johannes hanika.

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

#include "dtgtk/resetlabel.h"

static void _reset_label_class_init (GtkDarktableResetLabelClass *klass);
static void _reset_label_init (GtkDarktableResetLabel *label);


static void
_reset_label_class_init (GtkDarktableResetLabelClass *klass)
{
}

static void
_reset_label_init(GtkDarktableResetLabel *label)
{
}

static gboolean
_reset_label_callback(GtkDarktableResetLabel *label, GdkEventButton *event, gpointer user_data)
{
  if(event->type == GDK_2BUTTON_PRESS)
  {
    memcpy(((char *)label->module->params) + label->offset, ((char *)label->module->default_params) + label->offset, label->size);
    label->module->gui_update(label->module);
    dt_dev_add_history_item(darktable.develop, label->module, FALSE);
    return TRUE;
  }
  return FALSE;
}

// public functions
GtkWidget*
dtgtk_reset_label_new (const gchar *text, dt_iop_module_t *module, void *param, int param_size)
{
  GtkDarktableResetLabel *label;
  label = gtk_type_new (dtgtk_reset_label_get_type());
  label->module = module;
  label->offset = param - (void *)module->params;
  label->size = param_size;

  label->lb = GTK_LABEL(gtk_label_new(text));
  gtk_misc_set_alignment(GTK_MISC(label->lb), 0.0, 0.5);
  gtk_event_box_set_visible_window(GTK_EVENT_BOX(label), FALSE);
  g_object_set(G_OBJECT(label), "tooltip-text", _("double-click to reset"), (char *)NULL);
  gtk_container_add(GTK_CONTAINER(label), GTK_WIDGET(label->lb));
  gtk_widget_add_events(GTK_WIDGET(label), GDK_BUTTON_PRESS_MASK);
  g_signal_connect(G_OBJECT(label), "button-press-event", G_CALLBACK(_reset_label_callback), (gpointer)NULL);

  return (GtkWidget *)label;
}

GtkType dtgtk_reset_label_get_type()
{
  static GtkType dtgtk_reset_label_type = 0;
  if (!dtgtk_reset_label_type)
  {
    static const GtkTypeInfo dtgtk_reset_label_info =
    {
      "GtkDarktableResetLabel",
      sizeof(GtkDarktableResetLabel),
      sizeof(GtkDarktableResetLabelClass),
      (GtkClassInitFunc) _reset_label_class_init,
      (GtkObjectInitFunc) _reset_label_init,
      NULL,
      NULL,
      (GtkClassInitFunc) NULL
    };
    dtgtk_reset_label_type = gtk_type_unique (GTK_TYPE_EVENT_BOX, &dtgtk_reset_label_info);
  }
  return dtgtk_reset_label_type;
}

void
dtgtk_reset_label_set_text(GtkDarktableResetLabel *label, const gchar *str)
{
  gtk_label_set_text(label->lb, str);
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
