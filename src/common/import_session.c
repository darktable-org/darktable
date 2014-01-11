/*
    This file is part of darktable,
    copyright (c) 2014 Henrik Andersson.

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

#include <stdio.h>

#include "control/conf.h"
#include "control/control.h"
#include "common/film.h"
#include "common/import_session.h"
#include "common/variables.h"

/* TODO: Investigate if we can make one import session instance thread safe
         eg. having several background jobs working with same instance.
*/

typedef struct dt_import_session_t
{
  uint32_t ref;

  dt_film_t *film;
  dt_variables_params_t *vp;

  gchar *current_path;
  gchar *current_filename;

}
dt_import_session_t;


static void
_import_session_cleanup_filmroll(dt_import_session_t *self)
{
  /* if current filmroll for session is empty, remove it */
  /* TODO: check if dt_film_remove actual removes directories */
  if (dt_film_is_empty(self->film->id))
    dt_film_remove(self->film->id);
  else
    dt_film_cleanup(self->film);

  g_free(self->film);
  self->film = NULL;
}


static int
_import_session_initialize_filmroll(dt_import_session_t *self, const char *path)
{
  int32_t film_id;

  /* cleanup of previously used filmroll */
  _import_session_cleanup_filmroll(self);

  /* recursively create directories, abort if failed */
  if (g_mkdir_with_parents(path, 0755) == -1)
  {
    dt_control_log(_("failed to create session path %s."), path);
    _import_session_cleanup_filmroll(self);
    return 1;
  }

  /* open one or initialize a filmroll for the session */
  self->film = (dt_film_t *)g_malloc(sizeof(dt_film_t));
  dt_film_init(self->film);
  film_id = dt_film_new(self->film, path);
  if (film_id == 0)
  {
    dt_control_log(_("failed to open film roll for session."));
    _import_session_cleanup_filmroll(self);
    return 1;
  }

  return 0;
}


static void
_import_session_migrate_old_config()
{
  /* TODO: check if old config exists, migrate to new and remove old */
}


static char *
_import_session_path_pattern()
{
  char *res;
  char *base;
  char *sub;

  res = NULL;
  base = dt_conf_get_string("session/base_directory_pattern");
  sub = dt_conf_get_string("session/sub_path_pattern");

  if (!sub || !base)
  {
    fprintf(stderr, "[import_session] No base or subpath configured...\n");
    goto bail_out;
  }

  res = g_build_path(G_DIR_SEPARATOR_S, base, sub, (char *)NULL);

 bail_out:
  g_free(base);
  g_free(sub);
  return res;
}


static char *
_import_session_filename_pattern()
{
  char *name;

  name = dt_conf_get_string("session/filename_pattern");
  if (!name)
  {
    fprintf(stderr, "[import_session] No name configured...\n");
    return NULL;
  }

  return name;
}


struct dt_import_session_t *
dt_import_session_new()
{
  dt_import_session_t *is;

  is = (dt_import_session_t *)g_malloc(sizeof(dt_import_session_t));
  memset(is, 0, sizeof(dt_import_session_t));

  dt_variables_params_init(&is->vp);

  /* migrate old configuration */
  _import_session_migrate_old_config();
  return is;
}


void
dt_import_session_destroy(struct dt_import_session_t *self)
{
  if(--self->ref != 0)
    return;

  /* cleanup of session import film roll */
  _import_session_cleanup_filmroll(self);

  dt_variables_params_destroy(self->vp);

  g_free(self);
}

void
dt_import_session_ref(struct dt_import_session_t *self)
{
  self->ref++;
}

void
dt_import_session_unref(struct dt_import_session_t *self)
{
  self->ref--;
}

void
dt_import_session_import(struct dt_import_session_t *self)
{
  int id = dt_image_import(self->film->id, self->current_filename, TRUE);
  if(id)
  {
    dt_view_filmstrip_set_active_image(darktable.view_manager,id);
    dt_control_queue_redraw();
  }
}


void
dt_import_session_set_name(struct dt_import_session_t *self, const char *name)
{
  /* free previous jobcode name */
  if (self->vp->jobcode)
    g_free((void *)self->vp->jobcode);

  self->vp->jobcode = g_strdup(name);

  /* setup new filmroll if path has changed */
  dt_import_session_path(self, FALSE);
}


const char *
dt_import_session_name(struct dt_import_session_t *self)
{
  return self->vp->jobcode;
}


const char *
dt_import_session_filename(struct dt_import_session_t *self, gboolean current)
{
  char *pattern;

  if (current)
    return self->current_filename;

  /* expand next filename */
  g_free(self->current_filename);

  pattern = _import_session_filename_pattern();

  dt_variables_expand(self->vp, pattern, TRUE);
  self->current_filename = g_strdup(dt_variables_get_result(self->vp));

  g_free(pattern);
  return self->current_filename;
}


const char *
dt_import_session_path(struct dt_import_session_t *self, gboolean current)
{
  char *pattern;
  char *new_path;

  if (current)
    return self->current_path;

  /* check if expanded path differs from current */
  pattern = _import_session_path_pattern();
  dt_variables_expand(self->vp, pattern, TRUE);
  new_path = g_strdup(dt_variables_get_result(self->vp));
  g_free(pattern);

  if (strcmp(self->current_path, new_path) == 0)
    return self->current_path;

  /* we need to initialize a new filmroll for the new path */
  _import_session_initialize_filmroll(self, new_path);

  return self->current_path;
}
