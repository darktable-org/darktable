/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DT_MOBILE_FILE_CONTROLS_H
#define DT_MOBILE_FILE_CONTROLS_H

#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include "common/film.h"
#include "common/image.h"
#include "common/overlay.h"

/* A document import updates the original coupled filename fields. It is not
 * permission to write arbitrary string arrays or opaque compressed LUT bytes.
 * The Android owner retains immutable cache files for the image session so an
 * undo can still resolve the file selected by its saved parameter snapshot. */
typedef struct mobile_file_layout
{
  const dt_introspection_field_t *path, *file, *keypoints, *compressed, *lutname, *image_id;
  int raster, overlay;
  size_t maximum_path_bytes;
} mobile_file_layout;

static const dt_introspection_field_t *mobile_file_char_field(dt_iop_module_t *module,
                                                              const char *name, size_t count)
{
  const dt_introspection_field_t *field = module->get_f(name);
  if(!field || field->header.type != DT_INTROSPECTION_TYPE_ARRAY
     || field->Array.type != DT_INTROSPECTION_TYPE_CHAR || !field->Array.field
     || field->Array.field->header.type != DT_INTROSPECTION_TYPE_CHAR
     || field->Array.field->header.size != sizeof(char)
     || field->Array.field->header.offset != field->header.offset
     || field->Array.count != count || field->header.size != count
     || field->header.offset > (size_t)module->params_size
     || field->header.size > (size_t)module->params_size - field->header.offset) return NULL;
  return field;
}

static int mobile_file_import_layout(dt_iop_module_t *module, mobile_file_layout *layout)
{
  memset(layout, 0, sizeof(*layout));
  if(!module || !module->params || module->params_size <= 0 || !module->get_f
     || !module->have_introspection) return 0;
  if(!strcmp(module->op, "rasterfile"))
  {
    layout->raster = 1;
    layout->path = mobile_file_char_field(module, "path", 2048);
    layout->file = mobile_file_char_field(module, "file", 2048);
    layout->maximum_path_bytes = MIN((size_t)PATH_MAX - 1, (size_t)4095);
    return layout->path && layout->file;
  }
  if(!strcmp(module->op, "overlay"))
  {
    layout->overlay = 1;
    layout->path = mobile_file_char_field(module, "filename", 1024);
    layout->image_id = module->get_f("imgid");
    const dt_introspection_field_t *id = layout->image_id;
    if(!id || id->header.type != DT_INTROSPECTION_TYPE_INT || id->header.size != sizeof(dt_imgid_t)
       || id->header.offset > (size_t)module->params_size
       || id->header.size > (size_t)module->params_size - id->header.offset) return 0;
    layout->maximum_path_bytes = 1023;
    return layout->path && module->dev;
  }
  if(strcmp(module->op, "lut3d")) return 0;
  layout->path = mobile_file_char_field(module, "filepath", 512);
  layout->compressed = mobile_file_char_field(module, "c_clut", 2048 * 2 * 3);
  layout->lutname = mobile_file_char_field(module, "lutname", 128);
  layout->keypoints = module->get_f("nb_keypoints");
  const dt_introspection_field_t *keypoints = layout->keypoints;
  if(!keypoints || keypoints->header.type != DT_INTROSPECTION_TYPE_INT
     || keypoints->header.size != sizeof(int)
     || keypoints->header.offset > (size_t)module->params_size
     || sizeof(int) > (size_t)module->params_size - keypoints->header.offset) return 0;
  layout->maximum_path_bytes = 511;
  return layout->path && layout->compressed && layout->lutname;
}

static const char *mobile_file_stored_string(dt_iop_module_t *module,
                                            const dt_introspection_field_t *field)
{
  const char *value = (const char *)module->params + field->header.offset;
  const size_t length = strnlen(value, field->header.size);
  return length < field->header.size && g_utf8_validate(value, length, NULL) ? value : "";
}

static void append_mobile_file_parameter(dt_iop_module_t *module, GString *json, gboolean *first)
{
  mobile_file_layout layout;
  if(!mobile_file_import_layout(module, &layout)) return;
  const char *path = mobile_file_stored_string(module, layout.path);
  gchar *current = NULL;
  if(layout.raster)
  {
    const char *file = mobile_file_stored_string(module, layout.file);
    current = path[0] && file[0] ? g_build_filename(path, file, NULL) : g_strdup("");
  }
  else current = g_strdup(path);
  if(!*first) g_string_append_c(json, ',');
  *first = FALSE;
  g_string_append(json, "{\"name\":\"file.import\",\"label\":");
  json_string(json, layout.raster ? "raster mask file" : layout.overlay ? "overlay image" : "3D LUT file");
  g_string_append(json, ",\"value\":"); json_string(json, current);
  g_string_append_printf(json, ",\"maxBytes\":%zu,\"file\":true,\"extensions\":", layout.maximum_path_bytes);
  g_string_append(json, layout.raster ? "[\"png\",\"pfm\"]}" : layout.overlay
    ? "[\"png\",\"jpg\",\"jpeg\",\"tif\",\"tiff\"]}" : "[\"cube\",\"3dl\",\"png\"]}");
  g_free(current);
}

/* Match the original raster/PFM, Hald-CLUT, Cube and 3DL readers; the readers
 * remain responsible for decoding and validating their original file formats. */
static int mobile_file_extension_valid(const char *path, int raster, int overlay)
{
  const char *extension = strrchr(path, '.');
  if(!extension) return 0;
  if(!g_ascii_strcasecmp(extension, ".png")) return 1;
  if(overlay) return !g_ascii_strcasecmp(extension, ".jpg") || !g_ascii_strcasecmp(extension, ".jpeg")
    || !g_ascii_strcasecmp(extension, ".tif") || !g_ascii_strcasecmp(extension, ".tiff");
  if(raster) return !g_ascii_strcasecmp(extension, ".pfm");
  return !g_ascii_strcasecmp(extension, ".cube") || !g_ascii_strcasecmp(extension, ".3dl");
}

/* Returns 0 for an unsupported module/layout, -1 for invalid input, and 1
 * only after the complete native tuple has been changed. No mutation occurs
 * before layout, UTF-8, length, extension and regular/readable-file checks. */
static int update_mobile_file_parameter(dt_iop_module_t *module, const char *value)
{
  mobile_file_layout layout;
  if(!mobile_file_import_layout(module, &layout)) return 0;
  if(!value || !g_path_is_absolute(value)
     || strnlen(value, layout.maximum_path_bytes + 1) > layout.maximum_path_bytes
     || !g_utf8_validate(value, -1, NULL)) return -1;
  gchar *path = realpath(value, NULL);
  gchar *directory = NULL, *basename = NULL;
  int result = -1;
  if(!path || strlen(path) > layout.maximum_path_bytes || !g_utf8_validate(path, -1, NULL)
     || !mobile_file_extension_valid(path, layout.raster, layout.overlay)) goto finished;
  if(layout.raster)
  {
    directory = g_path_get_dirname(path);
    basename = g_path_get_basename(path);
    if(strlen(directory) >= layout.path->header.size || strlen(basename) >= layout.file->header.size) goto finished;
  }
  /* fstat the opened descriptor rather than trusting an earlier path lookup.
   * Image sessions use owner-managed cache files, retained through undo/redo. */
  const int descriptor = g_open(path, O_RDONLY | O_CLOEXEC, 0);
  if(descriptor < 0) goto finished;
  struct stat info;
  const int valid_file = fstat(descriptor, &info) == 0 && S_ISREG(info.st_mode) && info.st_size > 0;
  close(descriptor);
  if(!valid_file) goto finished;
  dt_imgid_t overlay_id = NO_IMGID;
  if(layout.overlay)
  {
    gchar *folder = g_path_get_dirname(path);
    dt_film_t film;
    const dt_filmid_t film_id = dt_film_new(&film, folder);
    g_free(folder);
    overlay_id = dt_is_valid_filmid(film_id) ? dt_image_import(film_id, path, TRUE, FALSE) : NO_IMGID;
    if(!dt_is_valid_imgid(overlay_id) || overlay_id == module->dev->image_storage.id
       || dt_overlay_used_by(overlay_id, module->dev->image_storage.id)) goto finished;
  }
  char *destination = (char *)module->params + layout.path->header.offset;
  memset(destination, 0, layout.path->header.size);
  memcpy(destination, layout.raster ? directory : path, strlen(layout.raster ? directory : path));
  if(layout.raster)
  {
    destination = (char *)module->params + layout.file->header.offset;
    memset(destination, 0, layout.file->header.size);
    memcpy(destination, basename, strlen(basename));
  }
  else if(layout.overlay)
  {
    /* The original overlay processor resolves its image through darktable's
     * private image database. A filename by itself would be an inert control. */
    memcpy((char *)module->params + layout.image_id->header.offset, &overlay_id, sizeof(overlay_id));
  }
  else
  {
    /* Match the original file picker's uncompressed-file transition. Stored
     * compressed data must never override the document the user just chose. */
    *(int *)((char *)module->params + layout.keypoints->header.offset) = 0;
    memset((char *)module->params + layout.compressed->header.offset, 0, layout.compressed->header.size);
    memset((char *)module->params + layout.lutname->header.offset, 0, layout.lutname->header.size);
  }
  result = 1;
finished:
  g_free(directory); g_free(basename); g_free(path);
  return result;
}

/* Dependency records are derived state, like raster registrations. Rebuild
 * from the current live instances after edit/undo/reset, so an old overlay
 * cannot remain a cycle dependency after its parameters have been restored. */
static void sync_mobile_overlay_dependencies(dt_develop_t *develop)
{
  dt_overlays_remove(develop->image_storage.id);
  for(GList *item = develop->iop; item; item = item->next)
  {
    dt_iop_module_t *module = item->data;
    mobile_file_layout layout;
    if(strcmp(module->op, "overlay") || !mobile_file_import_layout(module, &layout)) continue;
    dt_imgid_t id;
    memcpy(&id, (const char *)module->params + layout.image_id->header.offset, sizeof(id));
    if(dt_is_valid_imgid(id) && id != develop->image_storage.id)
      dt_overlay_record(develop->image_storage.id, id);
  }
}
#endif
