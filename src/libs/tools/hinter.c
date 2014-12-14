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

DT_MODULE(1)


typedef struct dt_lib_hinter_t
{
  GtkWidget *label;
} dt_lib_hinter_t;


static void _lib_hinter_set_message(dt_lib_module_t *self, const char *message);

const char *name()
{
  return _("hinter");
}

uint32_t views()
{
  return DT_VIEW_LIGHTTABLE | DT_VIEW_DARKROOM | DT_VIEW_TETHERING;
}

uint32_t container()
{
  return DT_UI_CONTAINER_PANEL_TOP_CENTER;
}

int expandable()
{
  return 0;
}

int position()
{
  return 1;
}


void gui_init(dt_lib_module_t *self)
{
  /* initialize ui widgets */
  dt_lib_hinter_t *d = (dt_lib_hinter_t *)g_malloc0(sizeof(dt_lib_hinter_t));
  self->data = (void *)d;

  self->widget = gtk_event_box_new();
  d->label = gtk_label_new("");
  gtk_container_add(GTK_CONTAINER(self->widget), d->label);

  darktable.control->proxy.hinter.module = self;
  darktable.control->proxy.hinter.set_message = _lib_hinter_set_message;
}

void gui_cleanup(dt_lib_module_t *self)
{
  //  dt_lib_hinter_t *d = (dt_lib_hinter_t *)self->data;
  darktable.control->proxy.hinter.module = NULL;
  g_free(self->data);
  self->data = NULL;
}


void _lib_hinter_set_message(dt_lib_module_t *self, const char *message)
{
  dt_lib_hinter_t *d = (dt_lib_hinter_t *)self->data;
  gtk_label_set_markup(GTK_LABEL(d->label), message);
#if 0

  int c = 0;
  char *str = g_strdup(message);
  /* FIXME: If this code is re-enabled, strtok() should be changed
   * for g_strsplit() for thread-safeness */
  char *s = strtok(str," ");
  gchar *markup=NULL;

  if (!s)
  {
    g_free(str);
    return;
  }


  markup = dt_util_dstrcat(markup, "<span size=\"smaller\">");
  while (s)
  {
    if ((++c)%8 == 0)
      markup = dt_util_dstrcat(markup, "\n   ");

    markup = dt_util_dstrcat(markup,"%s ", s);
    s = strtok(NULL," ");
  }

  markup = dt_util_dstrcat(markup, "</span>");

  gtk_label_set_markup(GTK_LABEL(d->label), markup);

  g_free(markup);
  g_free(str);
#endif
}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
