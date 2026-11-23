/*
    This file is part of PhotographyLAB, a darktable-based photo editor.
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

// the one translation unit of PhotographyLAB that calls into libdarktable.
// the history and headless-render handling follows src/mcp/dt_bridge.c

#include "plab/plab_engine.h"

#include "common/darktable.h"
#include "common/film.h"
#include "common/history.h"
#include "common/illuminants.h"
#include "common/image.h"
#include "common/image_cache.h"
#include "common/introspection.h"
#include "common/iop_order.h"
#include "common/styles.h"
#include "control/conf.h"
#include "develop/blend.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "imageio/imageio_common.h"
#include "imageio/imageio_module.h"

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <stdarg.h>
#include <string.h>

static void _seterr(char **err, const char *fmt, ...)
{
  if(!err) return;
  va_list ap;
  va_start(ap, fmt);
  g_free(*err);
  *err = g_strdup_vprintf(fmt, ap);
  va_end(ap);
}

int plab_init(int argc, char **argv)
{
  // dt_init() keeps pointers into argv for the whole session
  char **args = g_new0(char *, argc + 2);
  args[0] = g_strdup("photographylab");
  for(int i = 0; i < argc; i++) args[i + 1] = g_strdup(argv[i]);
  return dt_init(argc + 1, args, FALSE, TRUE, NULL);
}

void plab_shutdown(void)
{
  dt_cleanup();
}

void plab_free(void *p)
{
  g_free(p);
}

int plab_import(const char *path, char **err)
{
  // the library keys images by absolute path
  GFile *gf = g_file_new_for_path(path);
  gchar *canon = g_file_get_path(gf);
  g_object_unref(gf);
  if(!g_file_test(canon, G_FILE_TEST_IS_REGULAR))
  {
    _seterr(err, "'%s' is not a readable file", path);
    g_free(canon);
    return -1;
  }
  dt_imgid_t id = dt_image_get_id_full_path(canon);
  if(!dt_is_valid_imgid(id))
  {
    gchar *dir = g_path_get_dirname(canon);
    dt_film_t film;
    dt_film_init(&film); // holds a mutex that dt_film_new() does not set up
    const dt_filmid_t filmid = dt_film_new(&film, dir);
    dt_film_cleanup(&film);
    g_free(dir);
    if(dt_is_valid_filmid(filmid)) id = dt_image_import(filmid, canon, TRUE, FALSE);
  }
  if(!dt_is_valid_imgid(id)) _seterr(err, "could not import '%s'", path);
  g_free(canon);
  return dt_is_valid_imgid(id) ? id : -1;
}

// ---------------------------------------------------------------------------
// rendering into memory, through an export "format" that keeps the pixels
// (dt_imageio_preview() needs the GUI, see src/mcp/dt_bridge.c)
// ---------------------------------------------------------------------------

typedef struct
{
  dt_imageio_module_data_t head;
  uint8_t *buf;
  uint32_t width, height;
} _mem_fmt_t;

static int _mem_write(dt_imageio_module_data_t *data, const char *filename,
                      const void *in,
                      const dt_colorspaces_color_profile_type_t over_type,
                      const char *over_filename, void *exif, const int exif_len,
                      const dt_imgid_t imgid, const int num, const int total,
                      dt_dev_pixelpipe_t *pipe, const gboolean export_masks)
{
  _mem_fmt_t *d = (_mem_fmt_t *)data;
  if(!in) return 1;
  const size_t bytes = sizeof(uint32_t) * (size_t)data->width * data->height;
  g_free(d->buf);
  d->buf = g_try_malloc(bytes);
  if(!d->buf) return 1;
  memcpy(d->buf, in, bytes);
  d->width = data->width;
  d->height = data->height;
  return 0;
}

static int _mem_bpp(dt_imageio_module_data_t *d) { return 8; }
static int _mem_levels(dt_imageio_module_data_t *d) { return IMAGEIO_RGB | IMAGEIO_INT8; }
static const char *_mem_mime(dt_imageio_module_data_t *d) { return "memory"; }

uint8_t *plab_render(int imgid, int max_w, int max_h, int history_end,
                     int *out_w, int *out_h, char **err)
{
  dt_imageio_module_format_t fmt;
  memset(&fmt, 0, sizeof(fmt));
  fmt.mime = _mem_mime;
  fmt.levels = _mem_levels;
  fmt.bpp = _mem_bpp;
  fmt.write_image = _mem_write;

  _mem_fmt_t dat;
  memset(&dat, 0, sizeof(dat));
  dat.head.max_width = MAX(max_w, 0);
  dat.head.max_height = MAX(max_h, 0);
  dat.head.width = dat.head.max_width;
  dat.head.height = dat.head.max_height;
  dat.head.style_append = TRUE;

  // returns FALSE on success
  const gboolean failed = dt_imageio_export_with_flags(
      imgid, "memory", &fmt, (dt_imageio_module_data_t *)&dat,
      TRUE, TRUE, FALSE /*hq*/, FALSE /*upscale*/, FALSE, 1.0,
      FALSE, NULL, FALSE, FALSE,
      DT_COLORSPACE_SRGB, NULL, DT_INTENT_PERCEPTUAL, NULL, NULL,
      1, 1, NULL, history_end);

  if(failed || !dat.buf)
  {
    g_free(dat.buf);
    _seterr(err, "render failed");
    return NULL;
  }
  *out_w = dat.width;
  *out_h = dat.height;
  return dat.buf;
}

int plab_base_history(int imgid)
{
  const dt_image_t *img = dt_image_cache_get(imgid, 'r');
  const gboolean developed = img && (img->flags & DT_IMAGE_AUTO_PRESETS_APPLIED);
  dt_image_cache_read_release(img);

  // the first pipeline run writes out the modules darktable auto-applies
  if(!developed)
  {
    int w = 0, h = 0;
    g_free(plab_render(imgid, 64, 64, -1, &w, &h, NULL));
  }
  int end = 0;
  dt_image_get_history_end(imgid, &end);
  return end;
}

// ---------------------------------------------------------------------------
// white balance
// ---------------------------------------------------------------------------

static gboolean _as_shot_xy(int imgid, float *x, float *y)
{
  dt_develop_t dev;
  dt_dev_init(&dev, FALSE);
  dt_dev_load_image(&dev, imgid);
  gboolean ok = FALSE;
  // color calibration's defaults hold the as-shot illuminant
  // (channelmixerrgb.c reload_defaults)
  dt_iop_module_t *m = dt_iop_get_module_by_op_priority(dev.iop, "channelmixerrgb", 0);
  if(m && m->so->get_p)
  {
    const float *px = m->so->get_p(m->default_params, "x");
    const float *py = m->so->get_p(m->default_params, "y");
    if(px && py && *px > 0.f && *py > 0.f)
    {
      *x = *px;
      *y = *py;
      ok = TRUE;
    }
  }
  dt_dev_cleanup(&dev);
  return ok;
}

static void _locus(const float t, float *x, float *y)
{
  CCT_to_xy_blackbody(CLAMP(t, 1667.f, 25000.f), x, y);
}

double plab_as_shot_temperature(int imgid)
{
  float x = 0.f, y = 0.f;
  if(!_as_shot_xy(imgid, &x, &y)) return 0.0;
  return xy_to_CCT(x, y);
}

void plab_white_balance_xy(int imgid, double kelvin, double tint,
                           double *x, double *y)
{
  float x0 = 0.3457f, y0 = 0.3585f; // D50 when the image gives nothing
  _as_shot_xy(imgid, &x0, &y0);
  float lx0, ly0, lx, ly;
  _locus(xy_to_CCT(x0, y0), &lx0, &ly0);
  _locus(kelvin, &lx, &ly);
  // a greener illuminant is compensated toward magenta, hence +y for +tint
  *x = lx + (x0 - lx0);
  *y = ly + (y0 - ly0) + 0.0002 * tint;
}

// ---------------------------------------------------------------------------
// edits
// ---------------------------------------------------------------------------

static gboolean _module_in_history(dt_develop_t *dev, dt_iop_module_t *m)
{
  int i = 0;
  for(GList *h = dev->history; h && i < dev->history_end; h = g_list_next(h), i++)
    if(((dt_dev_history_item_t *)h->data)->module == m) return TRUE;
  return FALSE;
}

// the n-th (from 1) instance of op that the base history does not use,
// created when there are fewer
static dt_iop_module_t *_fresh_instance(dt_develop_t *dev, const char *op, int n)
{
  dt_iop_module_t *base = NULL;
  int found = 0;
  for(GList *l = dev->iop; l; l = g_list_next(l))
  {
    dt_iop_module_t *m = l->data;
    if(strcmp(m->op, op)) continue;
    if(!base || m->multi_priority < base->multi_priority) base = m;
  }
  if(!base) return NULL;

  // in multi_priority order, so a layer keeps its instance across renders
  for(int prio = 0; prio < 1000; prio++)
  {
    dt_iop_module_t *m = dt_iop_get_module_by_op_priority(dev->iop, op, prio);
    if(!m || _module_in_history(dev, m)) continue;
    if(++found == n) return m;
  }
  dt_iop_module_t *m = NULL;
  while(found < n)
  {
    m = dt_dev_module_duplicate_ext(dev, base, TRUE);
    if(!m) return NULL;
    found++;
  }
  return m;
}

static gboolean _place_after(dt_develop_t *dev, dt_iop_module_t *mod, const char *ops)
{
  gchar **names = g_strsplit(ops, ",", -1);
  dt_iop_module_t *target = NULL;
  for(int i = 0; names[i] && !target; i++)
  {
    dt_iop_module_t *t = dt_iop_get_module_by_op_priority(dev->iop, names[i], -1);
    if(t && t->enabled) target = t;
  }
  g_strfreev(names);
  if(!target || target == mod) return TRUE;
  if(mod->iop_order > target->iop_order) return TRUE;
  if(!dt_ioppr_check_can_move_after_iop(dev->iop, mod, target)) return FALSE;
  return dt_ioppr_move_iop_after(dev, mod, target);
}

static gboolean _set_field(dt_iop_module_t *mod, const plab_field_t *f, char **err)
{
  dt_introspection_field_t *fi = mod->so->get_f ? mod->so->get_f(f->name) : NULL;
  void *p = fi ? mod->so->get_p(mod->params, f->name) : NULL;
  if(!p)
  {
    _seterr(err, "module '%s' has no field '%s'", mod->op, f->name);
    return FALSE;
  }
  switch(fi->header.type)
  {
    case DT_INTROSPECTION_TYPE_FLOAT:
    {
      float *v = p;
      const float val = f->relative ? *v + f->value : f->value;
      *v = CLAMP(val, fi->Float.Min, fi->Float.Max);
      return TRUE;
    }
    case DT_INTROSPECTION_TYPE_INT:
      *(int *)p = f->relative ? *(int *)p + (int)f->value : (int)f->value;
      return TRUE;
    case DT_INTROSPECTION_TYPE_BOOL:
      *(gboolean *)p = f->value != 0.0;
      return TRUE;
    case DT_INTROSPECTION_TYPE_ENUM:
      if(f->str)
      {
        for(dt_introspection_type_enum_tuple_t *e = fi->Enum.values; e && e->name; e++)
          if(!strcmp(e->name, f->str))
          {
            *(int *)p = e->value;
            return TRUE;
          }
        _seterr(err, "'%s' is not a value of %s.%s", f->str, mod->op, f->name);
        return FALSE;
      }
      *(int *)p = (int)f->value;
      return TRUE;
    case DT_INTROSPECTION_TYPE_ARRAY:
      if(f->str && fi->Array.type == DT_INTROSPECTION_TYPE_CHAR)
      {
        g_strlcpy(p, f->str, fi->header.size);
        return TRUE;
      }
      // fall through
    default:
      _seterr(err, "field '%s' of '%s' cannot be set", f->name, mod->op);
      return FALSE;
  }
}

static gboolean _apply_layer(dt_develop_t *dev, const plab_layer_t *l, char **err)
{
  dt_iop_module_t *mod = l->fresh > 0
    ? _fresh_instance(dev, l->op, l->fresh)
    : dt_iop_get_module_by_op_priority(dev->iop, l->op, 0);
  if(!mod)
  {
    _seterr(err, "module '%s' is not available", l->op);
    return FALSE;
  }

  // a layer nobody switched on, on a module the image does not use, is no edit
  if(!l->enabled && !mod->enabled) return TRUE;

  if(l->after && !_place_after(dev, mod, l->after))
  {
    _seterr(err, "cannot place '%s' after '%s'", l->op, l->after);
    return FALSE;
  }

  for(int i = 0; i < l->n_fields; i++)
    if(!_set_field(mod, &l->fields[i], err)) return FALSE;

  if(l->opacity >= 0.f && mod->blend_params)
  {
    mod->blend_params->mask_mode = l->opacity < 100.f ? DEVELOP_MASK_ENABLED
                                                      : DEVELOP_MASK_DISABLED;
    mod->blend_params->opacity = CLAMP(l->opacity, 0.f, 100.f);
  }
  if(l->label && l->label[0])
  {
    g_strlcpy(mod->multi_name, l->label, sizeof(mod->multi_name));
    mod->multi_name_hand_edited = TRUE;
  }
  mod->enabled = l->enabled;
  dt_dev_add_history_item_ext(dev, mod, l->enabled, TRUE);
  return TRUE;
}

static void _load_truncated(dt_develop_t *dev, int imgid, int base_end)
{
  dt_dev_init(dev, FALSE);
  dt_dev_load_image(dev, imgid);
  // load reads the history but does not replay it into the module params
  const int n = (int)g_list_length(dev->history);
  dt_dev_pop_history_items_ext(dev, CLAMP(base_end, 0, n));
}

int plab_apply(int imgid, int base_end, const plab_layer_t *layers, int n_layers,
               char **err)
{
  dt_develop_t dev;
  _load_truncated(&dev, imgid, base_end);

  gboolean ok = TRUE;
  for(int i = 0; i < n_layers && ok; i++) ok = _apply_layer(&dev, &layers[i], err);

  // drop whatever an earlier apply left above the base, even with no layers
  if(ok)
  {
    GList *tail = g_list_nth(dev.history, dev.history_end);
    if(tail)
    {
      if(tail->prev) tail->prev->next = NULL;
      else dev.history = NULL;
      tail->prev = NULL;
      g_list_free_full(tail, dt_dev_free_history_item);
    }
    dt_dev_write_history_ext(&dev, imgid);
    dt_image_write_sidecar_file(imgid);
  }
  dt_dev_cleanup(&dev);
  return ok ? 0 : -1;
}

int plab_apply_style(int imgid, int base_end, const char *name, char **err)
{
  if(!dt_styles_exists(name))
  {
    _seterr(err, "no style named '%s'", name);
    return base_end;
  }
  if(plab_apply(imgid, base_end, NULL, 0, err)) return base_end;
  dt_styles_apply_to_image(name, FALSE, FALSE, imgid);
  int end = base_end;
  dt_image_get_history_end(imgid, &end);
  return end;
}

int plab_import_styles(const char *path)
{
  int n = 0;
  if(g_file_test(path, G_FILE_TEST_IS_DIR))
  {
    GDir *dir = g_dir_open(path, 0, NULL);
    const gchar *f;
    while(dir && (f = g_dir_read_name(dir)))
    {
      if(!g_str_has_suffix(f, ".dtstyle")) continue;
      gchar *full = g_build_filename(path, f, NULL);
      dt_styles_import_from_file(full);
      g_free(full);
      n++;
    }
    if(dir) g_dir_close(dir);
  }
  else if(g_file_test(path, G_FILE_TEST_IS_REGULAR))
  {
    dt_styles_import_from_file(path);
    n = 1;
  }
  return n;
}

char *plab_list_styles(void)
{
  GString *s = g_string_new(NULL);
  GList *styles = dt_styles_get_list("");
  for(GList *l = styles; l; l = g_list_next(l))
  {
    dt_style_t *st = l->data;
    if(s->len) g_string_append_c(s, '\n');
    g_string_append(s, st->name);
  }
  g_list_free_full(styles, dt_style_free);
  return g_string_free(s, FALSE);
}

int plab_export(int imgid, const char *out_path, int quality, char **err)
{
  const char *dot = strrchr(out_path, '.');
  gchar *ext = g_ascii_strdown(dot ? dot + 1 : "jpg", -1);
  const char *name = !strcmp(ext, "jpg") ? "jpeg" : !strcmp(ext, "tif") ? "tiff" : ext;
  dt_imageio_module_format_t *fmt = dt_imageio_get_format_by_name(name);
  g_free(ext);
  if(!fmt)
  {
    _seterr(err, "no export format for '%s'", out_path);
    return -1;
  }

  gchar *qkey = g_strdup_printf("plugins/imageio/format/%s/quality", fmt->plugin_name);
  if(quality > 0 && dt_conf_key_exists(qkey)) dt_conf_set_int(qkey, quality);
  g_free(qkey);

  dt_imageio_module_data_t *fdata = fmt->get_params(fmt);
  if(!fdata)
  {
    _seterr(err, "export format '%s' failed", fmt->plugin_name);
    return -1;
  }
  fdata->max_width = 0;
  fdata->max_height = 0;
  fdata->style[0] = '\0';
  fdata->style_append = TRUE;

  gchar *parent = g_path_get_dirname(out_path);
  g_mkdir_with_parents(parent, 0755);
  g_free(parent);

  const gboolean failed = dt_imageio_export_with_flags(
      imgid, out_path, fmt, fdata, FALSE, FALSE, TRUE, FALSE, FALSE, 1.0, FALSE, NULL,
      TRUE, FALSE, DT_COLORSPACE_SRGB, NULL, DT_INTENT_PERCEPTUAL, NULL, NULL,
      1, 1, NULL, -1);
  fmt->free_params(fmt, fdata);
  if(failed) _seterr(err, "could not write '%s'", out_path);
  return failed ? -1 : 0;
}

void plab_set_lut_folder(const char *folder)
{
  dt_conf_set_string("plugins/darkroom/lut3d/def_path", folder);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
