/*
    This file is part of darktable,
    Copyright (C) 2010-2023 darktable developers.

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
#include "common/file_location.h"
#include "common/image.h"
#include "common/image_cache.h"
#include "control/conf.h"
#include "control/control.h"
#include "dtgtk/button.h"
#include "dtgtk/paint.h"
#include "gui/gtk.h"
#include "imageio/imageio_common.h"
#include "imageio/imageio_module.h"
#include "imageio/storage/imageio_storage_api.h"
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#include <mapi.h>
#include <stringapiset.h>  // for MultiByteToWideChar
#endif

DT_MODULE(2)

typedef struct _email_attachment_t
{
  dt_imgid_t imgid; // The image id of exported image
  gchar *file;    // Full filename of exported image
} _email_attachment_t;

// saved params
typedef struct dt_imageio_email_t
{
  char filename[DT_MAX_PATH_FOR_PARAMS];
  GList *images;
} dt_imageio_email_t;


const char *name(const struct dt_imageio_module_storage_t *self)
{
  return _("send as email");
}

void *legacy_params(dt_imageio_module_storage_t *self,
                    const void *const old_params,
                    const size_t old_params_size,
                    const int old_version,
                    int *new_version,
                    size_t *new_size)
{
  typedef struct dt_imageio_email_v2_t
  {
    char filename[DT_MAX_PATH_FOR_PARAMS];
    GList *images;
  } dt_imageio_email_v2_t;

  if(old_version == 1)
  {
    typedef struct dt_imageio_email_v1_t
    {
      char filename[1024];
      GList *images;
    } dt_imageio_email_v1_t;

    const dt_imageio_email_v1_t *o = (dt_imageio_email_v1_t *)old_params;
    dt_imageio_email_v2_t *n = malloc(sizeof(dt_imageio_email_v2_t));

    g_strlcpy(n->filename, o->filename, sizeof(n->filename));

    *new_version = 2;
    *new_size = sizeof(dt_imageio_email_v2_t) - sizeof(GList *);
    return n;
  }

  // incremental update supported:
  /*
  typedef struct dt_imageio_email_v3_t
  {
    ...
  } dt_imageio_email_v3_t;

  if(old_version == 2)
  {
    // let's update from 2 to 3

    ...
    *new_size = sizeof(dt_imageio_email_v3_t) - sizeof(GList *);
    *new_version = 3;
    return n;
  }
  */
  return NULL;
}

int recommended_dimension(struct dt_imageio_module_storage_t *self,
                          dt_imageio_module_data_t *data,
                          uint32_t *width,
                          uint32_t *height)
{
  *width = 1536;
  *height = 1536;
  return 1;
}


void gui_init(dt_imageio_module_storage_t *self)
{
}

void gui_cleanup(dt_imageio_module_storage_t *self)
{
  free(self->gui_data);
}

void gui_reset(dt_imageio_module_storage_t *self)
{
}

int store(dt_imageio_module_storage_t *self,
          dt_imageio_module_data_t *sdata,
          const dt_imgid_t imgid,
          dt_imageio_module_format_t *format,
          dt_imageio_module_data_t *fdata,
          const int num,
          const int total,
          const gboolean high_quality,
          const gboolean upscale,
          const gboolean export_masks,
          dt_colorspaces_color_profile_type_t icc_type,
          const gchar *icc_filename,
          dt_iop_color_intent_t icc_intent,
          dt_export_metadata_t *metadata)
{
  dt_imageio_email_t *d = (dt_imageio_email_t *)sdata;

  _email_attachment_t *attachment =
    (_email_attachment_t *)g_malloc(sizeof(_email_attachment_t));
  attachment->imgid = imgid;

  /* construct a temporary file name */
  char tmpdir[PATH_MAX] = { 0 };
  dt_loc_get_tmp_dir(tmpdir, sizeof(tmpdir));

  char dirname[PATH_MAX] = { 0 };
  dt_image_full_path(imgid, dirname, sizeof(dirname), NULL);
  gchar *filename = g_path_get_basename(dirname);

  g_strlcpy(dirname, filename, sizeof(dirname));

  dt_image_path_append_version(imgid, dirname, sizeof(dirname));

  gchar *end = g_strrstr(dirname, ".") + 1;

  if(end) *end = '\0';

  g_strlcat(dirname, format->extension(fdata), sizeof(dirname));

  // set exported filename

  attachment->file = g_build_filename(tmpdir, dirname, (char *)NULL);

  if(dt_imageio_export(imgid, attachment->file, format, fdata, high_quality,
                       upscale, TRUE, export_masks, icc_type,
                       icc_filename, icc_intent, self, sdata, num, total, metadata) != 0)
  {
    dt_print(DT_DEBUG_ALWAYS,
             "[imageio_storage_email] could not export to file: `%s'!",
             attachment->file);
    dt_control_log(_("could not export to file `%s'!"), attachment->file);
    g_free(attachment->file);
    g_free(attachment);
    g_free(filename);
    return 1;
  }

  dt_control_log(ngettext("%d/%d exported to `%s'", "%d/%d exported to `%s'", num),
                 num, total, attachment->file);

  DT_OMP_PRAGMA(critical) // store can be called in parallel, so synch access to shared memory
  d->images = g_list_append(d->images, attachment);

  g_free(filename);

  return 0;
}

size_t params_size(dt_imageio_module_storage_t *self)
{
  return sizeof(dt_imageio_email_t) - sizeof(GList *);
}

void init(dt_imageio_module_storage_t *self)
{
}

void *get_params(dt_imageio_module_storage_t *self)
{
  dt_imageio_email_t *d = (dt_imageio_email_t *)g_malloc0(sizeof(dt_imageio_email_t));
  return d;
}

int set_params(dt_imageio_module_storage_t *self,
               const void *params,
               const int size)
{
  if(size != self->params_size(self)) return 1;
  return 0;
}

void free_params(dt_imageio_module_storage_t *self,
                 dt_imageio_module_data_t *params)
{
  if(!params) return;
  g_free(params);
}

#ifdef _WIN32
static LPWSTR _convert_to_widechar(UINT codepage, LPSTR str)
{
  if (!str) return NULL;

  DWORD length = MultiByteToWideChar(codepage, 0, str, -1, NULL, 0);
  LPWSTR wc_str = g_malloc(length * sizeof(WCHAR));
  MultiByteToWideChar(codepage, 0, str, -1, wc_str, length);

  return wc_str;
}

void finalize_store(dt_imageio_module_storage_t *self,
                    dt_imageio_module_data_t *params)
{
  dt_imageio_email_t *d = (dt_imageio_email_t *)params;

  const gchar *imageBodyFormat = " - %s (%s)\n";      // filename, exif one-liner
  const gint num_images = g_list_length(d->images);
  gchar *body = NULL;

  // We need an array of MapiFileDesc structures to attach images to an email
  lpMapiFileDesc m_fd = g_malloc0(sizeof(MapiFileDesc) * num_images);

  HINSTANCE hInst = LoadLibraryA("mapi32.dll");
  if(!hInst)
  {
    // Unlikely in normal Windows installations, but let's notify the user if it happens
    dt_control_log(_("could not open mapi32.dll"));
    dt_print(DT_DEBUG_ALWAYS, "[imageio_storage_email] could not open mapi32.dll");
    return;
  }

  LPMAPISENDMAIL SendMail;
  SendMail = (LPMAPISENDMAIL)GetProcAddress(hInst, "MAPISendMailW");
  if(!SendMail)
  {
    // Even more unlikely
    dt_control_log(_("could not get SendMail function"));
    dt_print(DT_DEBUG_ALWAYS, "[imageio_storage_email] could not get SendMail function");
    return;
  }

  // Iterate through the list of exported images and create the body of the email
  // from the basic information for those images
  int index = 0;  // index in the array of attached file
  for(GList *iter = d->images; iter; iter = g_list_next(iter))
  {
    gchar exif[256] = { 0 };
    _email_attachment_t *attachment = (_email_attachment_t *)iter->data;
    gchar *filename = g_path_get_basename(attachment->file);

    m_fd[index].lpszPathName = (LPSTR) _convert_to_widechar(CP_ACP, attachment->file);
    m_fd[index].lpszFileName = NULL;  // use the same name as in lpszPathName
    m_fd[index].lpFileType = NULL;
    m_fd[index].nPosition = (ULONG) -1;

    const dt_image_t *img =
      dt_image_cache_get(darktable.image_cache, attachment->imgid, 'r');
    dt_image_print_exif(img, exif, sizeof(exif));
    dt_image_cache_read_release(darktable.image_cache, img);

    gchar *imgbody = g_strdup_printf(imageBodyFormat, filename, exif);
    if(body != NULL)
    {
      gchar *body_bak = body;
      body = g_strconcat(body_bak, imgbody, NULL);
      g_free(body_bak);
    }
    else
    {
      body = g_strdup(imgbody);
    }

    g_free(imgbody);
    g_free(filename);
    index++;
  }

  MapiMessage m_msg;
  ZeroMemory(&m_msg, sizeof (m_msg));
  m_msg.lpszSubject = (LPSTR) _convert_to_widechar(CP_UTF8, 
                                                   _("images exported from darktable"));
  m_msg.lpszNoteText = (LPSTR) _convert_to_widechar(CP_ACP, body);

  m_msg.nFileCount = num_images;

  // Pointer to an array of MapiFileDesc structures.
  // Each structure contains information about one file attachment.
  m_msg.lpFiles = m_fd;

  SendMail(0L,      // use implicit session
           0L,      // ulUIParam; 0 is always valid
           &m_msg,  // the message being sent
           MAPI_LOGON_UI|MAPI_DIALOG,
           0);      // reserved, must be 0

  g_free(m_msg.lpszSubject);
  g_free(m_msg.lpszNoteText);
  for(index = 0; index < num_images; index++)
    g_free(m_fd[index].lpszPathName);
  g_free(m_fd);
}

#elif defined __APPLE__
void finalize_store(dt_imageio_module_storage_t *self,
                    dt_imageio_module_data_t *params)
{
  dt_imageio_email_t *d = (dt_imageio_email_t *)params;

  const gint nb_images = g_list_length(d->images);
  const gint argc = 3 + nb_images;

  char **argv = g_malloc0(sizeof(char *) * (argc + 1));

  argv[0] = "open";
  argv[1] = "-a";
  argv[2] = "Mail";
  int n = 3;

  for(GList *iter = d->images; iter; iter = g_list_next(iter))
  {
    _email_attachment_t *attachment = (_email_attachment_t *)iter->data;

    // use attachment->file directly as we need to free it, and this way it will be
    // freed as part of the argument release after the spawn below.
    argv[n] = attachment->file;
    n += 1;
  }
  g_list_free_full(d->images, g_free);
  d->images = NULL;

  argv[argc] = NULL;

  gchar *cmdline = g_strjoinv(" ", argv);
  dt_print(DT_DEBUG_IMAGEIO, "[email] launching '%s'", cmdline);
  g_free(cmdline);

  gint exit_status = 0;

  g_spawn_sync
    (NULL, argv, NULL,
     G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
     NULL, NULL, NULL, NULL, &exit_status, NULL);

  for(int k=3; k<argc; k++)
    g_free(argv[k]);
  g_free(argv);

  if(exit_status)
  {
    dt_control_log(_("could not launch email client!"));
  }
}


#else // this is for !_WIN32 and !__APPLE__
void finalize_store(dt_imageio_module_storage_t *self,
                    dt_imageio_module_data_t *params)
{
  dt_imageio_email_t *d = (dt_imageio_email_t *)params;

  const gchar *imageBodyFormat = " - %s (%s)\\n";      // filename, exif oneliner
  const gint nb_images = g_list_length(d->images);
  const gint argc = 5 + (2 * nb_images);

  char **argv = g_malloc0(sizeof(char *) * (argc + 1));

  gchar *body = NULL;

  argv[0] = "xdg-email";
  argv[1] = "--subject";
  argv[2] = _("images exported from darktable");
  argv[3] = "--body";
  int n = 5;

  for(GList *iter = d->images; iter; iter = g_list_next(iter))
  {
    gchar exif[256] = { 0 };
    _email_attachment_t *attachment = (_email_attachment_t *)iter->data;
    gchar *filename = g_path_get_basename(attachment->file);
    const dt_image_t *img =
      dt_image_cache_get(darktable.image_cache, attachment->imgid, 'r');
    dt_image_print_exif(img, exif, sizeof(exif));
    dt_image_cache_read_release(darktable.image_cache, img);

    gchar *imgbody = g_strdup_printf(imageBodyFormat, filename, exif);
    if(body != NULL)
    {
      gchar *body_bak = body;
      body = g_strconcat(body_bak, imgbody, NULL);
      g_free(body_bak);
    }
    else
    {
      body = g_strdup(imgbody);
    }
    g_free(imgbody);
    g_free(filename);

    argv[n]   = g_strdup("--attach");
    // use attachment->file directly as we need to freed it, and this way it will be
    // freed as part of the argument release after the spawn below.
    argv[n+1] = attachment->file;
    n += 2;
  }
  g_list_free_full(d->images, g_free);
  d->images = NULL;

  argv[4] = body;

  argv[argc] = NULL;

  gchar *cmdline = g_strjoinv(" ", argv);
  dt_print(DT_DEBUG_IMAGEIO, "[email] launching '%s'", cmdline);
  g_free(cmdline);

  gint exit_status = 0;

  g_spawn_sync
    (NULL, argv, NULL,
     G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
     NULL, NULL, NULL, NULL, &exit_status, NULL);

  for(int k=4; k<argc; k++)
    g_free(argv[k]);
  g_free(argv);

  if(exit_status)
  {
    dt_control_log(_("could not launch email client!"));
  }
}
#endif // !_WIN32 and !__APPLE__

gboolean supported(struct dt_imageio_module_storage_t *storage,
                   struct dt_imageio_module_format_t *format)
{
  const char *mime = format->mime(NULL);
  if(mime[0] == '\0') // this seems to be the copy format
    return FALSE;

  return TRUE;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
