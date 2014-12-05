/*
    This file is part of darktable,
    copyright (c) 2011 tobias ellinghaus.

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
#include "libs/lib.h"
#include "gui/gtk.h"
#include <vte/vte.h>
#include <signal.h>

DT_MODULE(1)

typedef struct dt_lib_file_manager_t
{
  pid_t pid;
  VteTerminal *terminal;
} dt_lib_file_manager_t;

const char *name()
{
  // FIXME: Hack to get a translated name without the need to touch the .po files
  //   char* n = _("zoomable light table\nfile manager");
  //   while(*n != '\n') n++; n++;
  //   return n;
  return _("file manager");
}

uint32_t views()
{
  return DT_VIEW_LIGHTTABLE | DT_VIEW_TETHERING;
}

uint32_t container()
{
  return DT_UI_CONTAINER_PANEL_LEFT_CENTER;
}

void gui_reset(dt_lib_module_t *self)
{
  dt_lib_file_manager_t *d = (dt_lib_file_manager_t *)self->data;
  kill(d->pid, SIGHUP);
#ifdef VTE_DEPRECATED
  d->pid = vte_terminal_fork_command(d->terminal, NULL, NULL, NULL, NULL, FALSE, FALSE, FALSE);
#else
  char *argv[2] = { g_strdup(g_getenv("SHELL")), NULL };
  vte_terminal_fork_command_full(d->terminal, VTE_PTY_DEFAULT, NULL, argv, NULL, 0, NULL, NULL, &d->pid, NULL);
  g_free(argv[0]);
#endif
  vte_terminal_reset(d->terminal, TRUE, TRUE);
}

int position()
{
  return 510;
}

void gui_init(dt_lib_module_t *self)
{
  dt_lib_file_manager_t *d = (dt_lib_file_manager_t *)g_malloc0(sizeof(dt_lib_file_manager_t));
  self->data = (void *)d;

  d->terminal = VTE_TERMINAL(vte_terminal_new());
  self->widget = GTK_WIDGET(d->terminal);
  dt_gui_key_accel_block_on_focus_connect(GTK_WIDGET(d->terminal));
#if defined(__MACH__) || defined(__APPLE__)
  vte_terminal_set_font_from_string(d->terminal, "Monospace 11");
#else
  vte_terminal_set_font_from_string(d->terminal, "Monospace 8");
#endif
#ifdef VTE_DEPRECATED
  d->pid = vte_terminal_fork_command(d->terminal, NULL, NULL, NULL, NULL, FALSE, FALSE, FALSE);
#else
  char *argv[2] = { g_strdup(g_getenv("SHELL")), NULL };
  vte_terminal_fork_command_full(d->terminal, VTE_PTY_DEFAULT, NULL, argv, NULL, 0, NULL, NULL, &d->pid, NULL);
  g_free(argv[0]);
#endif
  g_object_set(G_OBJECT(d->terminal), "tooltip-text", _("\
ls\t\t\t\t\tlist content of directory\n\
cd <dir>\t\t\tchange directory\n\
mkdir <dir>\t\t\tcreate directory\n\
mv <src> <dst>\tmove <src> to <dst>\n\
cp <src> <dst>\t\tcopy <src> to <dst>\n\
rm <file>\t\t\tdelete <file>\n\
rmdir <dir>\t\t\tdelete empty directory"),
               (char *)NULL);
}

void gui_cleanup(dt_lib_module_t *self)
{
  dt_lib_file_manager_t *d = (dt_lib_file_manager_t *)self->data;
  dt_gui_key_accel_block_on_focus_disconnect(GTK_WIDGET(d->terminal));
  kill(d->pid, SIGKILL);
  g_free(self->data);
  self->data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
