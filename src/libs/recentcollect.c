/*
    This file is part of darktable,
    copyright (c) 2009--2011 johannes hanika.

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
#include "control/conf.h"
#include "control/signal.h"
#include "gui/gtk.h"
#include "dtgtk/button.h"
#include "libs/lib.h"
#include "libs/collect.h"

DT_MODULE(1)

/** this module stores recently used image collection queries and displays
  * them as one-click buttons to the user. */

#define NUM_LINES 10

typedef struct dt_lib_recentcollect_item_t
{
  GtkWidget *button;
}
dt_lib_recentcollect_item_t;

typedef struct dt_lib_recentcollect_t
{
  // 1st is always most recently used entry (buttons stay fixed)
  dt_lib_recentcollect_item_t item[NUM_LINES];
}
dt_lib_recentcollect_t;

const char*
name ()
{
  return _("recently used collections");
}

uint32_t
views()
{
  return DT_VIEW_LIGHTTABLE;
}

uint32_t container()
{
  return DT_UI_CONTAINER_PANEL_LEFT_CENTER;
}

int
position ()
{
  return 300;
}

static int
serialize(char *buf, int bufsize)
{
  char confname[200];
  int c;
  const int num_rules = dt_conf_get_int("plugins/lighttable/collect/num_rules");
  c = snprintf(buf, bufsize, "%d:", num_rules);
  buf += c;
  bufsize -= c;
  for(int k=0; k<num_rules; k++)
  {
    snprintf(confname, 200, "plugins/lighttable/collect/mode%1d", k);
    const int mode = dt_conf_get_int(confname);
    c = snprintf(buf, bufsize, "%d:", mode);
    buf += c;
    bufsize -= c;
    snprintf(confname, 200, "plugins/lighttable/collect/item%1d", k);
    const int item = dt_conf_get_int(confname);
    c = snprintf(buf, bufsize, "%d:", item);
    buf += c;
    bufsize -= c;
    snprintf(confname, 200, "plugins/lighttable/collect/string%1d", k);
    gchar *str = dt_conf_get_string(confname);
    if(str && (str[0] != '\0'))
      c = snprintf(buf, bufsize, "%s$", str);
    else
      c = snprintf(buf, bufsize, "%%$");
    buf += c;
    bufsize -= c;
    g_free(str);
  }
  return 0;
}

static void
deserialize(char *buf)
{
  int num_rules = 0;
  char str[400], confname[200];
  sprintf(str, "%%");
  int mode = 0, item = 0;
  sscanf(buf, "%d", &num_rules);
  if(num_rules == 0) num_rules = 1;
  dt_conf_set_int("plugins/lighttable/collect/num_rules", num_rules);
  while(buf[0] != ':') buf++;
  buf++;
  for(int k=0; k<num_rules; k++)
  {
    sscanf(buf, "%d:%d:%[^$]", &mode, &item, str);
    snprintf(confname, 200, "plugins/lighttable/collect/mode%1d", k);
    dt_conf_set_int(confname, mode);
    snprintf(confname, 200, "plugins/lighttable/collect/item%1d", k);
    dt_conf_set_int(confname, item);
    snprintf(confname, 200, "plugins/lighttable/collect/string%1d", k);
    dt_conf_set_string(confname, str);
    while(buf[0] != '$' && buf[0] != '\0') buf++;
    buf++;
  }
  dt_collection_update_query(darktable.collection);
}

static void
pretty_print(char *buf, char *out)
{
  if(!buf || buf[0] == '\0') return;
  int num_rules = 0;
  char str[400] = {0};
  int mode, item;
  sscanf(buf, "%d", &num_rules);
  while(buf[0] != ':') buf++;
  buf++;
  for(int k=0; k<num_rules; k++)
  {
    sscanf(buf, "%d:%d:%[^$]", &mode, &item, str);
    str[399] = '$';

    if(k > 0) switch(mode)
      {
        case DT_LIB_COLLECT_MODE_AND:
          out += sprintf(out, _(" and "));
          break;
        case DT_LIB_COLLECT_MODE_OR:
          out += sprintf(out, _(" or "));
          break;
        default: //case DT_LIB_COLLECT_MODE_AND_NOT:
          out += sprintf(out, _(" but not "));
          break;
      }
    int i = 0;
    while(str[i] != '$') i++;
    str[i] = '\0';

    out += sprintf(out, "%s %s", _(dt_lib_collect_string[item]), item == 0 ? dt_image_film_roll_name(str) : str);
    while(buf[0] != '$' && buf[0] != '\0') buf++;
    buf++;
  }
}

static void
button_pressed (GtkButton *button, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_recentcollect_t *d = (dt_lib_recentcollect_t *) self->data;

  // deserialize this button's preset
  int n = -1;
  for(int k=0; k<NUM_LINES; k++)
  {
    if(button == GTK_BUTTON(d->item[k].button))
    {
      n = k;
      break;
    }
  }
  if(n < 0) return;
  char confname[200];
  snprintf(confname, 200, "plugins/lighttable/recentcollect/line%1d", n);
  gchar *line = dt_conf_get_string(confname);
  if(line)
  {
    deserialize(line);
    g_free(line);
  }
}

static void _lib_recentcollection_updated(gpointer instance, gpointer user_data)
{
  dt_lib_module_t *self =(dt_lib_module_t *)user_data;
  dt_lib_recentcollect_t *d = (dt_lib_recentcollect_t *)self->data;
  // serialize, check for recently used
  char confname[200];
  const int bufsize = 4096;
  char buf[bufsize];
  if(serialize(buf, bufsize)) return;

  int n = -1;
  for(int k=0; k<CLAMPS(dt_conf_get_int("plugins/lighttable/recentcollect/num_items"), 0, NUM_LINES); k++)
  {
    // is it already in the current list?
    snprintf(confname, 200, "plugins/lighttable/recentcollect/line%1d", k);
    gchar *line = dt_conf_get_string(confname);
    if(!line) continue;
    if(!strcmp(line, buf))
    {
      n = k;
      break;
    }
    g_free(line);
  }
  if(n < 0)
  {
    const int num_items = CLAMPS(dt_conf_get_int("plugins/lighttable/recentcollect/num_items"), 0, NUM_LINES);
    if(num_items < NUM_LINES)
    {
      // new, unused entry
      n = num_items;
      dt_conf_set_int("plugins/lighttable/recentcollect/num_items", num_items + 1);
    }
    else
    {
      // kill least recently used entry:
      n = num_items - 1;
    }
  }
  if(n >= 0 && n < NUM_LINES)
  {
    // sort n to the top
    for(int k=n; k>0; k--)
    {
      snprintf(confname, 200, "plugins/lighttable/recentcollect/line%1d", k-1);
      gchar *line1 = dt_conf_get_string(confname);
      if(line1 && line1[0] != '\0')
      {
        snprintf(confname, 200, "plugins/lighttable/recentcollect/line%1d", k);
        dt_conf_set_string(confname, line1);
      }
      g_free(line1);
    }
    dt_conf_set_string("plugins/lighttable/recentcollect/line0", buf);
  }
  // update button descriptions:
  for(int k=0; k<NUM_LINES; k++)
  {
    char str[200] = {0};
    char str_cut[200] = {0};
    char str_pretty[200] = {0};

    snprintf(confname, 200, "plugins/lighttable/recentcollect/line%1d", k);
    gchar *buf = dt_conf_get_string(confname);
    if(buf && buf[0] != '\0')
    {
      pretty_print(buf, str);
      g_free(buf);
    }
    g_object_set(G_OBJECT(d->item[k].button), "tooltip-text", str, (char *)NULL);
    const int cut = 45;
    if (g_utf8_strlen(str, -1) > cut)
    {
      g_utf8_strncpy(str_cut, str, cut);
      snprintf(str_pretty, 200, "%s...", str_cut);
      gtk_button_set_label(GTK_BUTTON(d->item[k].button), str_pretty);
    }
    else
    {
      gtk_button_set_label(GTK_BUTTON(d->item[k].button), str);
    }
    gtk_widget_set_no_show_all(d->item[k].button, TRUE);
    gtk_widget_set_visible(d->item[k].button, FALSE);
  }
  for(int k=0; k<CLAMPS(dt_conf_get_int("plugins/lighttable/recentcollect/num_items"), 0, NUM_LINES); k++)
  {
    gtk_widget_set_no_show_all(d->item[k].button, FALSE);
    gtk_widget_set_visible(d->item[k].button, TRUE);
  }
}

void
gui_reset (dt_lib_module_t *self)
{
  dt_conf_set_int("plugins/lighttable/recentcollect/num_items", 0);
  char confname[200];
  for(int k=0; k<NUM_LINES; k++)
  {
    snprintf(confname, 200, "plugins/lighttable/recentcollect/line%1d", k);
    dt_conf_set_string(confname, "");
  }
  _lib_recentcollection_updated(NULL,self);
}

void
gui_init (dt_lib_module_t *self)
{
  dt_lib_recentcollect_t *d = (dt_lib_recentcollect_t *)malloc(sizeof(dt_lib_recentcollect_t));
  memset(d,0,sizeof(dt_lib_recentcollect_t));
  self->data = (void *)d;
  self->widget = gtk_vbox_new(FALSE, 0);

  // add buttons in the list, set them all to invisible
  for(int k=0; k<NUM_LINES; k++)
  {
    d->item[k].button = dtgtk_button_new(NULL, CPF_STYLE_FLAT);
    gtk_box_pack_start(GTK_BOX(self->widget), d->item[k].button, FALSE, TRUE, 0);
    g_signal_connect(G_OBJECT(d->item[k].button), "clicked", G_CALLBACK(button_pressed), (gpointer)self);
    gtk_widget_set_no_show_all(d->item[k].button, TRUE);
    gtk_widget_set_visible(d->item[k].button, FALSE);
  }
  _lib_recentcollection_updated(NULL,self);

  /* connect collection changed signal */
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_COLLECTION_CHANGED,
                            G_CALLBACK(_lib_recentcollection_updated),
                            (gpointer)self);
}

void
gui_cleanup (dt_lib_module_t *self)
{
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(_lib_recentcollection_updated), self);
  free(self->data);
  self->data = NULL;
}

// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
