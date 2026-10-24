/*
    This file is part of darktable,
    Copyright (C) 2009-2026 darktable developers.

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
#pragma once

#include <gio/gio.h>

// keep codec writes and metadata rewrites local before a sequential transfer
static inline gboolean _imageio_publish_staged_file(const char *staged_path,
                                         const char *filename,
                                         const gboolean overwrite,
                                         gboolean (*is_canceled)(gpointer),
                                         gpointer cancel_data,
                                         GError **error)
{
  gboolean success = FALSE;
  GFile *source = g_file_new_for_path(staged_path);
  GFile *destination = g_file_new_for_path(filename);
  char *directory = g_path_get_dirname(filename);
  char *uuid = g_uuid_string_random();
  char *temporary_name = g_strdup_printf(".darktable-export-%s.tmp", uuid);
  char *temporary_path = g_build_filename(directory, temporary_name, NULL);
  GFile *temporary = g_file_new_for_path(temporary_path);
  GFileInputStream *input = NULL;
  GFileOutputStream *output = NULL;
  const gsize buffer_size = 1024 * 1024;
  void *buffer = g_try_malloc(buffer_size);

  if(!buffer)
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "cannot allocate output copy buffer");
  else if((input = g_file_read(source, NULL, error)))
    output = g_file_create(temporary, G_FILE_CREATE_NONE, NULL, error);

  if(output)
  {
    while(TRUE)
    {
      if(is_canceled && is_canceled(cancel_data))
      {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                            "output copy canceled");
        break;
      }
      const gssize bytes = g_input_stream_read(G_INPUT_STREAM(input), buffer,
                                               buffer_size, NULL, error);
      if(bytes < 0) break;
      if(bytes == 0)
      {
        success = TRUE;
        break;
      }
      if(!g_output_stream_write_all(G_OUTPUT_STREAM(output), buffer, bytes,
                                    NULL, NULL, error))
        break;
    }
    if(!g_output_stream_close(G_OUTPUT_STREAM(output), NULL,
                              success ? error : NULL))
      success = FALSE;
  }
  if(input && !g_input_stream_close(G_INPUT_STREAM(input), NULL,
                                    success ? error : NULL))
    success = FALSE;
  if(success && is_canceled && is_canceled(cancel_data))
  {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                        "output copy canceled");
    success = FALSE;
  }
  // publish only after close; never fall back to a second remote copy
  if(success)
    success = g_file_move(temporary, destination,
                          G_FILE_COPY_NO_FALLBACK_FOR_MOVE
                            | (overwrite ? G_FILE_COPY_OVERWRITE : G_FILE_COPY_NONE),
                          NULL, NULL, NULL, error);
  if(output && !success) g_file_delete(temporary, NULL, NULL);

  g_clear_object(&output);
  g_clear_object(&input);
  g_object_unref(temporary);
  g_object_unref(destination);
  g_object_unref(source);
  g_free(buffer);
  g_free(temporary_path);
  g_free(temporary_name);
  g_free(uuid);
  g_free(directory);
  return success;
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
