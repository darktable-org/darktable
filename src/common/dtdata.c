/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

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

#include "common/dtdata.h"
#include "common/darktable.h"
#include "common/database.h"
#include "common/debug.h"
#include "common/image.h"
#include "common/image_cache.h"
#include "common/math.h"

#include <archive.h>
#include <archive_entry.h>
#include <errno.h>
#include <glib/gstdio.h>
#include <png.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

// the format version written into every new file
#define DTDATA_VERSION "1\n"
#define DTDATA_VERSION_ENTRY "version"

// larger than any mask we would store; a bigger entry is damage, not data
#define DTDATA_MAX_ENTRY_SIZE ((int64_t)256 << 20)

// writers rename over the zip, which Windows refuses while a reader has it
// open. not recursive: nothing under the write lock may call _zip_read/_zip_list
static GRWLock _lock;

typedef struct _scanner_t
{
  char op[32];
  uint32_t kinds;
  dt_dtdata_scan_fn fn;
} _scanner_t;

// filled once at module load, read-only afterwards
static GList *_scanners = NULL;

static const char *_kind_prefix(const dt_dtdata_kind_t kind)
{
  switch(kind)
  {
    case DT_DTDATA_KIND_MASK:  return "mask";
    case DT_DTDATA_KIND_DEPTH: return "depth";
    case DT_DTDATA_KIND_BRUSH: return "brush";
    case DT_DTDATA_KIND_PATCH: return "patch";
  }
  return "data";
}

int dt_dtdata_entry_kind(const char *entry)
{
  if(!entry) return -1;
  for(int k = DT_DTDATA_KIND_MASK; k <= DT_DTDATA_KIND_PATCH; k++)
  {
    const char *prefix = _kind_prefix(k);
    const size_t n = strlen(prefix);
    if(!strncmp(entry, prefix, n) && entry[n] == '-') return k;
  }
  return -1;
}

gboolean dt_dtdata_enabled(void)
{
  return dt_image_get_xmp_mode() != DT_WRITE_XMP_NEVER;
}

void dt_dtdata_path_for_image(const char *versioned_image_path,
                              char *path,
                              const size_t len)
{
  snprintf(path, len, "%s%s", versioned_image_path, DT_DTDATA_EXT);
}

// empty when the image is unknown, so no caller ends up on a bare ".dtdata"
void dt_dtdata_path(const dt_imgid_t imgid, char *path, const size_t len)
{
  path[0] = 0;
  char image[PATH_MAX] = { 0 };
  gboolean from_cache = FALSE;
  dt_image_full_path(imgid, image, sizeof(image), &from_cache);
  if(!image[0]) return;
  // same rule as dt_image_write_sidecar_file(): beside the original when
  // it is reachable, else beside the local copy
  if(!g_file_test(image, G_FILE_TEST_EXISTS))
  {
    from_cache = TRUE;
    dt_image_full_path(imgid, image, sizeof(image), &from_cache);
  }
  dt_image_path_append_version(imgid, image, sizeof(image));
  dt_dtdata_path_for_image(image, path, len);
}

// --- PNG in memory ---

static void _png_write_cb(png_structp png, png_bytep data, png_size_t len)
{
  g_byte_array_append((GByteArray *)png_get_io_ptr(png), data, len);
}

static void _png_flush_cb(png_structp png)
{
}

static GBytes *_encode_gray_png(const float *mask,
                                const int width,
                                const int height,
                                const int bpc)
{
  png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
  if(!png) return NULL;
  png_infop info = png_create_info_struct(png);
  if(!info)
  {
    png_destroy_write_struct(&png, NULL);
    return NULL;
  }

  GByteArray *out = g_byte_array_new();
  // 16 bit needs two bytes per pixel; sized for the larger case
  uint8_t *row = g_malloc((size_t)width * 2);

  if(setjmp(png_jmpbuf(png)))
  {
    png_destroy_write_struct(&png, &info);
    g_byte_array_unref(out);
    g_free(row);
    return NULL;
  }

  png_set_write_fn(png, out, _png_write_cb, _png_flush_cb);
  png_set_IHDR(png, info, width, height, bpc, PNG_COLOR_TYPE_GRAY,
               PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
               PNG_FILTER_TYPE_DEFAULT);
  png_write_info(png, info);
  // rows are handed over in host byte order
  if(bpc == 16) png_set_swap(png);

  for(int y = 0; y < height; y++)
  {
    const float *in = mask + (size_t)y * width;
    if(bpc == 16)
    {
      uint16_t *r = (uint16_t *)row;
      for(int x = 0; x < width; x++) r[x] = (uint16_t)(CLIP(in[x]) * 65535.0f + 0.5f);
    }
    else
    {
      for(int x = 0; x < width; x++) row[x] = (uint8_t)(CLIP(in[x]) * 255.0f + 0.5f);
    }
    png_write_row(png, row);
  }
  png_write_end(png, NULL);
  png_destroy_write_struct(&png, &info);
  g_free(row);
  return g_byte_array_free_to_bytes(out);
}

typedef struct _png_mem_t
{
  const uint8_t *data;
  size_t len;
  size_t pos;
} _png_mem_t;

static void _png_read_cb(png_structp png, png_bytep out, png_size_t len)
{
  _png_mem_t *m = png_get_io_ptr(png);
  if(m->pos + len > m->len) png_error(png, "truncated png");
  memcpy(out, m->data + m->pos, len);
  m->pos += len;
}

// any PNG comes back as a single gray float plane in [0,1]
static float *_decode_gray_png(const uint8_t *data,
                               const size_t len,
                               int *width,
                               int *height)
{
  if(len < 8 || png_sig_cmp(data, 0, 8)) return NULL;

  png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
  if(!png) return NULL;
  png_infop info = png_create_info_struct(png);
  if(!info)
  {
    png_destroy_read_struct(&png, NULL, NULL);
    return NULL;
  }

  _png_mem_t mem = { data, len, 0 };
  // libpng reports errors with a longjmp to png_jmpbuf; these are assigned
  // after setjmp and freed in that branch, so volatile keeps their values
  uint8_t *volatile rows = NULL;
  float *volatile mask = NULL;

  if(setjmp(png_jmpbuf(png)))
  {
    png_destroy_read_struct(&png, &info, NULL);
    dt_free_align(rows);
    dt_free_align(mask);
    return NULL;
  }

  png_set_read_fn(png, &mem, _png_read_cb);
  png_set_user_limits(png, 32768, 32768);
  png_read_info(png, info);

  png_uint_32 w = 0, h = 0;
  int bit_depth = 0, color_type = 0;
  png_get_IHDR(png, info, &w, &h, &bit_depth, &color_type, NULL, NULL, NULL);

  if(color_type == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
  if(color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) png_set_expand_gray_1_2_4_to_8(png);
  if(png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
  if(color_type & PNG_COLOR_MASK_COLOR) png_set_rgb_to_gray_fixed(png, 1, -1, -1);
  png_set_strip_alpha(png);
  if(bit_depth == 16) png_set_swap(png);
  png_read_update_info(png, info);

  const size_t rowbytes = png_get_rowbytes(png, info);
  const int out_depth = png_get_bit_depth(png, info);
  rows = dt_alloc_aligned(rowbytes * h);
  mask = dt_alloc_align_float((size_t)w * h);
  if(!rows || !mask) png_error(png, "out of memory");

  for(png_uint_32 y = 0; y < h; y++)
    png_read_row(png, rows + (size_t)y * rowbytes, NULL);
  png_read_end(png, NULL);
  png_destroy_read_struct(&png, &info, NULL);

  if(out_depth == 16)
  {
    const float norm = 1.0f / 65535.0f;
    for(size_t k = 0; k < (size_t)w * h; k++)
      mask[k] = ((const uint16_t *)rows)[k] * norm;
  }
  else
  {
    const float norm = 1.0f / 255.0f;
    for(size_t k = 0; k < (size_t)w * h; k++)
      mask[k] = rows[k] * norm;
  }
  dt_free_align(rows);
  *width = (int)w;
  *height = (int)h;
  return mask;
}

// --- zip ---

static gboolean _write_entry(struct archive *w,
                             const char *name,
                             const void *data,
                             const size_t len)
{
  struct archive_entry *e = archive_entry_new();
  archive_entry_set_pathname(e, name);
  archive_entry_set_size(e, len);
  archive_entry_set_filetype(e, AE_IFREG);
  archive_entry_set_perm(e, 0644);
  gboolean ok = archive_write_header(w, e) == ARCHIVE_OK
                && (len == 0 || archive_write_data(w, data, len) == (ssize_t)len);
  archive_entry_free(e);
  return ok;
}

// libarchive leaves the error string unset on a plain short read
static const char *_archive_err(struct archive *a)
{
  const char *s = archive_error_string(a);
  return s ? s : "unexpected end of file";
}

// libarchive's own open takes the path in the ANSI code page on Windows,
// g_fopen keeps it UTF-8. the FILE outlives the archive: caller closes it
static FILE *_zip_open_read(struct archive *r, const char *path)
{
  FILE *f = g_fopen(path, "rb");
  if(!f)
  {
    dt_print(DT_DEBUG_ALWAYS, "[dtdata] cannot read '%s': %s", path, g_strerror(errno));
    return NULL;
  }
  archive_read_support_format_zip(r);
  if(archive_read_open_FILE(r, f) != ARCHIVE_OK)
  {
    dt_print(DT_DEBUG_ALWAYS, "[dtdata] cannot read '%s': %s", path, _archive_err(r));
    fclose(f);
    return NULL;
  }
  return f;
}

// a header loop keeps going on ARCHIVE_WARN; anything else but EOF at its
// end means the zip could not be read through
static gboolean _next_header(struct archive *r, struct archive_entry **e, int *rc)
{
  *rc = archive_read_next_header(r, e);
  return *rc == ARCHIVE_OK || *rc == ARCHIVE_WARN;
}

// the current entry's bytes into buf. FALSE when the data does not end
// cleanly or exceeds the size limit, so a cut entry is never copied short
static gboolean _read_entry_data(struct archive *r,
                                 struct archive_entry *e,
                                 GByteArray *buf)
{
  const char *n = archive_entry_pathname(e);
  if(archive_entry_size_is_set(e) && archive_entry_size(e) > DTDATA_MAX_ENTRY_SIZE)
  {
    dt_print(DT_DEBUG_ALWAYS, "[dtdata] entry '%s' is larger than 256 MB, refused", n);
    return FALSE;
  }
  const void *blk = NULL;
  size_t sz = 0;
  int64_t off = 0;
  int rc;
  while((rc = archive_read_data_block(r, &blk, &sz, &off)) == ARCHIVE_OK || rc == ARCHIVE_WARN)
  {
    if((int64_t)buf->len + (int64_t)sz > DTDATA_MAX_ENTRY_SIZE)
    {
      dt_print(DT_DEBUG_ALWAYS, "[dtdata] entry '%s' is larger than 256 MB, refused", n);
      return FALSE;
    }
    g_byte_array_append(buf, blk, sz);
  }
  if(rc != ARCHIVE_EOF)
    dt_print(DT_DEBUG_ALWAYS, "[dtdata] cannot read entry '%s': %s", n, _archive_err(r));
  return rc == ARCHIVE_EOF;
}

// copy the zip to a temp file without the entries in drop, adding name if
// it is not there yet, then rename it over. name may be NULL to only drop
static gboolean _zip_rewrite(const char *path,
                             const GList *drop,
                             const char *name,
                             const void *data,
                             const size_t len)
{
  gboolean ok = FALSE;
  gchar *tmp = g_strdup_printf("%s.tmp", path);

  FILE *wf = g_fopen(tmp, "wb");
  if(!wf)
  {
    dt_print(DT_DEBUG_ALWAYS, "[dtdata] cannot write '%s': %s", tmp, g_strerror(errno));
    g_free(tmp);
    return FALSE;
  }
  struct archive *w = archive_write_new();
  archive_write_set_format_zip(w);
  // zip is not a tape format: no zero padding to the 10 KiB block
  archive_write_set_bytes_in_last_block(w, 1);
  if(archive_write_open_FILE(w, wf) != ARCHIVE_OK)
  {
    dt_print(DT_DEBUG_ALWAYS, "[dtdata] cannot write '%s': %s", tmp, _archive_err(w));
    archive_write_free(w);
    fclose(wf);
    g_unlink(tmp);
    g_free(tmp);
    return FALSE;
  }

  gboolean have_version = FALSE, have_entry = FALSE, copy_ok = TRUE;
  if(g_file_test(path, G_FILE_TEST_EXISTS))
  {
    struct archive *r = archive_read_new();
    FILE *rf = _zip_open_read(r, path);
    if(rf)
    {
      struct archive_entry *e = NULL;
      int rc = ARCHIVE_EOF;
      while(copy_ok && _next_header(r, &e, &rc))
      {
        const char *n = archive_entry_pathname(e);
        if(!n) continue;
        if(!strcmp(n, DTDATA_VERSION_ENTRY)) have_version = TRUE;
        if(name && !strcmp(n, name)) have_entry = TRUE;
        if(g_list_find_custom((GList *)drop, n, (GCompareFunc)g_strcmp0))
        {
          archive_read_data_skip(r);
          continue;
        }

        GByteArray *buf = g_byte_array_new();
        copy_ok = _read_entry_data(r, e, buf) && _write_entry(w, n, buf->data, buf->len);
        g_byte_array_unref(buf);
      }
      if(copy_ok && rc != ARCHIVE_EOF)
      {
        dt_print(DT_DEBUG_ALWAYS, "[dtdata] cannot read '%s': %s", path, _archive_err(r));
        copy_ok = FALSE;
      }
      archive_read_close(r);
    }
    else
      copy_ok = FALSE;
    archive_read_free(r);
    if(rf) fclose(rf);
  }

  if(copy_ok && !have_version)
    copy_ok = _write_entry(w, DTDATA_VERSION_ENTRY, DTDATA_VERSION, strlen(DTDATA_VERSION));
  if(copy_ok && name && !have_entry)
    copy_ok = _write_entry(w, name, data, len);

  ok = archive_write_close(w) == ARCHIVE_OK && copy_ok;
  archive_write_free(w);
  // the FILE buffer is flushed here, so it must be closed before the rename
  if(fclose(wf) != 0) ok = FALSE;

  if(ok && g_rename(tmp, path) != 0)
  {
    dt_print(DT_DEBUG_ALWAYS, "[dtdata] cannot replace '%s'", path);
    ok = FALSE;
  }
  if(!ok) g_unlink(tmp);
  g_free(tmp);
  return ok;
}

// the entry's bytes, or NULL if the file or entry is missing or damaged
static GBytes *_zip_read(const char *path, const char *name)
{
  GBytes *result = NULL;
  gboolean found = FALSE;
  g_rw_lock_reader_lock(&_lock);
  struct archive *r = archive_read_new();
  FILE *f = _zip_open_read(r, path);
  if(f)
  {
    struct archive_entry *e = NULL;
    int rc = ARCHIVE_EOF;
    while(!found && _next_header(r, &e, &rc))
    {
      const char *n = archive_entry_pathname(e);
      if(!n || strcmp(n, name)) continue;
      found = TRUE;
      GByteArray *buf = g_byte_array_new();
      if(_read_entry_data(r, e, buf))
        result = g_byte_array_free_to_bytes(buf);
      else
        g_byte_array_unref(buf);
    }
    if(!found && rc != ARCHIVE_EOF)
      dt_print(DT_DEBUG_ALWAYS, "[dtdata] cannot read '%s': %s", path, _archive_err(r));
    archive_read_close(r);
  }
  archive_read_free(r);
  if(f) fclose(f);
  g_rw_lock_reader_unlock(&_lock);
  return result;
}

// entry names without "version", sorted. NULL with *ok FALSE when the zip
// could not be read to its end, so a damaged file does not look merely short
static GList *_zip_list(const char *path, gboolean *ok)
{
  GList *list = NULL;
  *ok = FALSE;
  g_rw_lock_reader_lock(&_lock);
  struct archive *r = archive_read_new();
  FILE *f = _zip_open_read(r, path);
  if(f)
  {
    struct archive_entry *e = NULL;
    int rc = ARCHIVE_EOF;
    while(_next_header(r, &e, &rc))
    {
      const char *n = archive_entry_pathname(e);
      if(n && strcmp(n, DTDATA_VERSION_ENTRY)) list = g_list_prepend(list, g_strdup(n));
    }
    *ok = rc == ARCHIVE_EOF;
    if(!*ok)
      dt_print(DT_DEBUG_ALWAYS, "[dtdata] cannot read '%s': %s", path, _archive_err(r));
    archive_read_close(r);
  }
  archive_read_free(r);
  if(f) fclose(f);
  g_rw_lock_reader_unlock(&_lock);
  if(!*ok)
  {
    g_list_free_full(list, g_free);
    return NULL;
  }
  return g_list_sort(list, (GCompareFunc)g_strcmp0);
}

GList *dt_dtdata_file_list_entries(const char *path)
{
  if(!path || !g_file_test(path, G_FILE_TEST_EXISTS)) return NULL;
  gboolean ok = FALSE;
  return _zip_list(path, &ok);
}

// --- entries ---

// "<kind>-<sha1>.png": the name is the checksum, so a damaged or swapped
// file is caught on read without any manifest
static void _entry_name(const dt_dtdata_kind_t kind,
                        GBytes *png,
                        char *name,
                        const size_t len)
{
  gchar *sha = g_compute_checksum_for_bytes(G_CHECKSUM_SHA1, png);
  snprintf(name, len, "%s-%s.png", _kind_prefix(kind), sha);
  g_free(sha);
}

gboolean dt_dtdata_file_write_gray(const char *path,
                                   const dt_dtdata_kind_t kind,
                                   const dt_dtdata_origin_t origin,
                                   const char *producer,
                                   const float *mask,
                                   const int width,
                                   const int height,
                                   const int bpc,
                                   dt_dtdata_ref_t *ref)
{
  if(!path || !mask || width <= 0 || height <= 0 || (bpc != 8 && bpc != 16)) return FALSE;

  GBytes *png = _encode_gray_png(mask, width, height, bpc);
  if(!png)
  {
    dt_print(DT_DEBUG_ALWAYS, "[dtdata] cannot encode %dx%d mask", width, height);
    return FALSE;
  }

  char name[DT_DTDATA_ENTRY_LEN] = { 0 };
  _entry_name(kind, png, name, sizeof(name));

  gsize len = 0;
  const void *data = g_bytes_get_data(png, &len);
  g_rw_lock_writer_lock(&_lock);
  const gboolean ok = _zip_rewrite(path, NULL, name, data, len);
  g_rw_lock_writer_unlock(&_lock);
  g_bytes_unref(png);
  if(!ok) return FALSE;

  if(ref)
  {
    memset(ref, 0, sizeof(*ref));
    g_strlcpy(ref->entry, name, sizeof(ref->entry));
    ref->kind = kind;
    ref->origin = origin;
    ref->width = width;
    ref->height = height;
    ref->bpc = bpc;
    if(producer) g_strlcpy(ref->producer, producer, sizeof(ref->producer));
  }
  dt_print(DT_DEBUG_MASKS, "[dtdata] wrote %s (%dx%d, %d bit) to '%s'",
           name, width, height, bpc, path);
  return TRUE;
}

float *dt_dtdata_file_read_gray(const char *path,
                                const dt_dtdata_ref_t *ref,
                                int *width,
                                int *height)
{
  *width = *height = 0;
  if(!path || !ref || !ref->entry[0]) return NULL;

  GBytes *png = _zip_read(path, ref->entry);
  if(!png)
  {
    dt_print(DT_DEBUG_ALWAYS, "[dtdata] entry '%s' not found in '%s'", ref->entry, path);
    return NULL;
  }

  // the hash sits between the kind prefix and ".png"
  const char *dash = strchr(ref->entry, '-');
  const char *dot = strrchr(ref->entry, '.');
  gchar *sha = g_compute_checksum_for_bytes(G_CHECKSUM_SHA1, png);
  const gboolean intact = dash && dot && dot > dash + 1
                          && strlen(sha) == (size_t)(dot - dash - 1)
                          && !strncmp(sha, dash + 1, dot - dash - 1);
  g_free(sha);
  if(!intact)
  {
    dt_print(DT_DEBUG_ALWAYS, "[dtdata] entry '%s' in '%s' does not match its checksum",
             ref->entry, path);
    g_bytes_unref(png);
    return NULL;
  }

  gsize len = 0;
  const void *data = g_bytes_get_data(png, &len);
  float *mask = _decode_gray_png(data, len, width, height);
  g_bytes_unref(png);
  if(!mask)
    dt_print(DT_DEBUG_ALWAYS, "[dtdata] entry '%s' in '%s' is not a readable PNG",
             ref->entry, path);
  return mask;
}

gboolean dt_dtdata_file_merge(const char *src_path, const char *dst_path)
{
  if(!g_file_test(src_path, G_FILE_TEST_EXISTS)) return TRUE;
  if(!g_file_test(dst_path, G_FILE_TEST_EXISTS))
  {
    GFile *src = g_file_new_for_path(src_path);
    GFile *dst = g_file_new_for_path(dst_path);
    const gboolean ok = g_file_copy(src, dst, G_FILE_COPY_NONE, NULL, NULL, NULL, NULL);
    g_object_unref(src);
    g_object_unref(dst);
    return ok;
  }

  // both listed and read before the write lock: the lock is not recursive
  gboolean src_ok = FALSE, dst_ok = FALSE;
  GList *entries = _zip_list(src_path, &src_ok);
  GList *have = _zip_list(dst_path, &dst_ok);
  gboolean ok = src_ok && dst_ok;
  for(GList *l = entries; l && ok; l = g_list_next(l))
  {
    const char *name = l->data;
    if(g_list_find_custom(have, name, (GCompareFunc)g_strcmp0)) continue;
    GBytes *data = _zip_read(src_path, name);
    if(!data) continue;
    gsize len = 0;
    const void *bytes = g_bytes_get_data(data, &len);
    g_rw_lock_writer_lock(&_lock);
    ok = _zip_rewrite(dst_path, NULL, name, bytes, len);
    g_rw_lock_writer_unlock(&_lock);
    g_bytes_unref(data);
  }
  g_list_free_full(entries, g_free);
  g_list_free_full(have, g_free);
  return ok;
}

gboolean dt_dtdata_file_sweep(const char *path,
                              const GList *keep,
                              const uint32_t kinds)
{
  if(!path || !g_file_test(path, G_FILE_TEST_EXISTS)) return TRUE;
  // a zip that cannot be read through would look emptier than it is
  gboolean listed = FALSE;
  GList *entries = _zip_list(path, &listed);
  if(!listed) return FALSE;
  if(!entries) return TRUE;

  GList *drop = NULL;
  int remaining = 0;
  for(GList *l = entries; l; l = g_list_next(l))
  {
    const char *name = l->data;
    const int kind = dt_dtdata_entry_kind(name);
    const gboolean claimed = kind >= 0 && (kinds & (1u << kind));
    if(claimed && !g_list_find_custom((GList *)keep, name, (GCompareFunc)g_strcmp0))
      drop = g_list_prepend(drop, g_strdup(name));
    else
      remaining++;
  }
  g_list_free_full(entries, g_free);

  gboolean ok = TRUE;
  if(drop)
  {
    g_rw_lock_writer_lock(&_lock);
    if(remaining == 0)
      ok = g_unlink(path) == 0;
    else
      ok = _zip_rewrite(path, drop, NULL, NULL, 0);
    g_rw_lock_writer_unlock(&_lock);
    dt_print(DT_DEBUG_MASKS, "[dtdata] swept %d unreferenced entries from '%s'%s",
             g_list_length(drop), path, remaining ? "" : ", file removed");
  }
  g_list_free_full(drop, g_free);
  return ok;
}

// --- by image ---

void dt_dtdata_register_scanner(const char *op,
                                const uint32_t kinds,
                                dt_dtdata_scan_fn fn)
{
  if(!op || !fn) return;
  _scanner_t *s = g_new0(_scanner_t, 1);
  g_strlcpy(s->op, op, sizeof(s->op));
  s->kinds = kinds;
  s->fn = fn;
  _scanners = g_list_append(_scanners, s);
}

static const _scanner_t *_scanner_for(const char *op)
{
  for(const GList *l = _scanners; l; l = g_list_next(l))
    if(!strcmp(((_scanner_t *)l->data)->op, op)) return l->data;
  return NULL;
}

void dt_dtdata_sweep(const dt_imgid_t imgid)
{
  if(!dt_is_valid_imgid(imgid) || !_scanners) return;
  char path[PATH_MAX] = { 0 };
  dt_dtdata_path(imgid, path, sizeof(path));
  if(!path[0] || !g_file_test(path, G_FILE_TEST_EXISTS)) return;

  uint32_t kinds = 0;
  for(const GList *l = _scanners; l; l = g_list_next(l))
    kinds |= ((_scanner_t *)l->data)->kinds;

  GList *keep = NULL;
  gboolean readable = TRUE;
  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT operation, op_params, module"
                              " FROM main.history"
                              " WHERE imgid = ?1",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  while(readable && sqlite3_step(stmt) == SQLITE_ROW)
  {
    const char *op = (const char *)sqlite3_column_text(stmt, 0);
    const _scanner_t *s = op ? _scanner_for(op) : NULL;
    if(!s) continue;
    char entry[DT_DTDATA_ENTRY_LEN] = { 0 };
    const int res = s->fn(sqlite3_column_blob(stmt, 1),
                          sqlite3_column_bytes(stmt, 1),
                          sqlite3_column_int(stmt, 2),
                          entry, sizeof(entry));
    if(res > 0)
      keep = g_list_prepend(keep, g_strdup(entry));
    else if(res < 0)
      readable = FALSE;
  }
  sqlite3_finalize(stmt);

  // a params version this build cannot read might reference anything:
  // leave the file to the build that wrote it
  if(readable)
    dt_dtdata_file_sweep(path, keep, kinds);
  else
    dt_print(DT_DEBUG_MASKS, "[dtdata] history of image %d has unreadable params, '%s' not swept",
             imgid, path);
  g_list_free_full(keep, g_free);
}

void dt_dtdata_delete(const dt_imgid_t imgid)
{
  if(!dt_is_valid_imgid(imgid)) return;
  char path[PATH_MAX] = { 0 };
  dt_dtdata_path(imgid, path, sizeof(path));
  if(!path[0]) return;
  g_rw_lock_writer_lock(&_lock);
  if(g_file_test(path, G_FILE_TEST_EXISTS) && g_unlink(path) == 0)
    dt_print(DT_DEBUG_MASKS, "[dtdata] removed '%s'", path);
  g_rw_lock_writer_unlock(&_lock);
}

gboolean dt_dtdata_write_gray(const dt_imgid_t imgid,
                              const dt_dtdata_kind_t kind,
                              const dt_dtdata_origin_t origin,
                              const char *producer,
                              const float *mask,
                              const int width,
                              const int height,
                              const int bpc,
                              dt_dtdata_ref_t *ref)
{
  if(!dt_dtdata_enabled() || !dt_is_valid_imgid(imgid)) return FALSE;
  char path[PATH_MAX] = { 0 };
  dt_dtdata_path(imgid, path, sizeof(path));
  if(!path[0]) return FALSE;
  return dt_dtdata_file_write_gray(path, kind, origin, producer, mask, width, height, bpc, ref);
}

// the sidecar beside the local copy, empty when the image has none. a mask
// imported while the original was offline lives there until
// dt_image_local_copy_reset() merges it beside the original
static void _cache_path(const dt_imgid_t imgid, char *path, const size_t len)
{
  path[0] = 0;
  const dt_image_t *img = dt_image_cache_get(imgid, 'r');
  const gboolean local_copy = img && (img->flags & DT_IMAGE_LOCAL_COPY);
  dt_image_cache_read_release(img);
  if(!local_copy) return;
  char image[PATH_MAX] = { 0 };
  gboolean from_cache = TRUE;
  dt_image_full_path(imgid, image, sizeof(image), &from_cache);
  if(!from_cache || !image[0]) return;
  dt_image_path_append_version(imgid, image, sizeof(image));
  dt_dtdata_path_for_image(image, path, len);
}

float *dt_dtdata_read_gray(const dt_imgid_t imgid,
                           const dt_dtdata_ref_t *ref,
                           int *width,
                           int *height)
{
  *width = *height = 0;
  if(!dt_is_valid_imgid(imgid)) return NULL;
  char path[PATH_MAX] = { 0 };
  dt_dtdata_path(imgid, path, sizeof(path));
  if(!path[0]) return NULL;
  float *mask = dt_dtdata_file_read_gray(path, ref, width, height);
  if(mask) return mask;

  char cache[PATH_MAX] = { 0 };
  _cache_path(imgid, cache, sizeof(cache));
  if(!cache[0] || !strcmp(cache, path) || !g_file_test(cache, G_FILE_TEST_EXISTS))
    return NULL;
  dt_print(DT_DEBUG_MASKS, "[dtdata] entry '%s' not beside the original, trying '%s'",
           ref ? ref->entry : "", cache);
  return dt_dtdata_file_read_gray(cache, ref, width, height);
}

GList *dt_dtdata_list_entries(const dt_imgid_t imgid)
{
  if(!dt_is_valid_imgid(imgid)) return NULL;
  char path[PATH_MAX] = { 0 };
  dt_dtdata_path(imgid, path, sizeof(path));
  if(!path[0]) return NULL;
  return dt_dtdata_file_list_entries(path);
}

gboolean dt_dtdata_merge(const dt_imgid_t src_imgid, const dt_imgid_t dst_imgid)
{
  if(!dt_is_valid_imgid(src_imgid) || !dt_is_valid_imgid(dst_imgid)) return FALSE;
  char src[PATH_MAX] = { 0 }, dst[PATH_MAX] = { 0 };
  dt_dtdata_path(src_imgid, src, sizeof(src));
  dt_dtdata_path(dst_imgid, dst, sizeof(dst));
  if(!src[0] || !dst[0]) return FALSE;
  return dt_dtdata_file_merge(src, dst);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
