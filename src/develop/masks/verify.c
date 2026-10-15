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

#include "develop/masks/verify.h"

#include "common/darktable.h"
#include "develop/blend.h"
#include "develop/imageop.h"
#include "develop/masks.h"
#include "develop/masks/harvest_read.h"
#include "develop/masks/verify_internal.h"
#include "develop/masks/probe_image.h"
#include "common/iop_profile.h"
#include "develop/pixelpipe.h"

#ifdef _OPENMP
#include <omp.h>
#endif

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <math.h>
#include <stdio.h>

// a migrated mask is meant to be the same computation, but some paths
// reassociate float arithmetic (an opacity applied per element rather than
// once), so an exact comparison would report noise. The answers are
// identical, equivalent (within the 8-bit step a mask is shown at, invisible)
// and different. As in the integration suite's deltae test, a result fails
// when the worst pixel exceeds the tolerance or the mean over the frame
// exceeds a third of it: the worst pixel alone cannot tell one boundary pixel
// from half the frame
#define VERIFY_EPS_IDENTICAL 1e-6f
#define VERIFY_EPS_EQUIVALENT (1.0f / 255.0f)
#define VERIFY_EPS_EQUIVALENT_MEAN (VERIFY_EPS_EQUIVALENT / 3.0f)

typedef enum
{
  VERIFY_IDENTICAL = 0,
  VERIFY_EQUIVALENT,
  VERIFY_DIFFERENT,
  VERIFY_SKIPPED,
  VERIFY_ERROR,
} verify_result_t;

typedef struct
{
  int total;
  int identical, equivalent, different, skipped, error;
  int inert_before;      // classic mask uniform: comparison proves nothing
  int live;              // classic mask genuinely varies: a real test
  int live_identical, live_equivalent, live_different;
  double worst_max_diff;
  int worst_index;
  // the same edit's mean and differing-pixel count, so the headline number can
  // be read as a magnitude and not just as "something differs somewhere"
  double worst_mean_diff;
  int worst_differing_pixels;

  int gpu_compared;              // edits where both GPU renders succeeded
  double worst_gpu_diff;         // GPU: worst classic-vs-migrated
  int worst_gpu_index;
  double worst_gpu_mean_diff;
  int worst_gpu_differing_pixels;

  // what those mask differences did to the rendered image
  int image_compared;
  double worst_image_diff, worst_image_mean_diff;
  int worst_image_differing_pixels, worst_image_index;
  int gpu_image_compared;
  double worst_gpu_image_diff, worst_gpu_image_mean_diff;
  int worst_gpu_image_differing_pixels, worst_gpu_image_index;
  double worst_dev_before;       // worst CPU/GPU gap on classic edits
  double worst_dev_after;        // ... and on migrated ones
  int dev_gap_widened;           // migrated gap worse than classic by >1/255
  // ... and how many of those survive with the mask post-processing switched
  // off, i.e. are migration's to answer for rather than a downstream stage's
  // (see dev_diff_after_nopost)
  int dev_gap_widened_own;
} verify_stats_t;

// ---------------------------------------------------------------------------
// JSON helpers
// ---------------------------------------------------------------------------

// a member that is absent, null or not a plain value reads as `dflt`
static JsonNode *_obj_value(JsonObject *o, const char *k)
{
  if(!o || !json_object_has_member(o, k)) return NULL;
  JsonNode *n = json_object_get_member(o, k);
  return (n && json_node_get_node_type(n) == JSON_NODE_VALUE) ? n : NULL;
}

gint64 dt_masks_harvest_obj_int(JsonObject *o, const char *k, const gint64 dflt)
{
  JsonNode *n = _obj_value(o, k);
  return n ? json_node_get_int(n) : dflt;
}

float dt_masks_harvest_obj_float(JsonObject *o, const char *k, const float dflt)
{
  JsonNode *n = _obj_value(o, k);
  return n ? (float)json_node_get_double(n) : dflt;
}

const char *dt_masks_harvest_obj_str(JsonObject *o, const char *k, const char *dflt)
{
  JsonNode *n = _obj_value(o, k);
  const char *v = n ? json_node_get_string(n) : NULL;
  return v ? v : dflt;
}

/** read a float array member into `out`, up to `n` entries */
static void _obj_float_array(JsonObject *o, const char *k, float *out, const int n)
{
  if(!json_object_has_member(o, k)) return;
  JsonArray *a = json_object_get_array_member(o, k);
  if(!a) return;
  const int len = MIN(n, (int)json_array_get_length(a));
  for(int i = 0; i < len; i++) out[i] = (float)json_array_get_double_element(a, i);
}

// ---------------------------------------------------------------------------
// reconstruction
// ---------------------------------------------------------------------------

/** rebuild one point of a form from its JSON, field for field as
    _emit_point() in harvest.c writes it. The group name the harvest leaves out
    stays zero, as the loader leaves it for a group without one */
static void _read_point(JsonObject *p, const int type, void *out)
{
  if(type & DT_MASKS_CIRCLE)
  {
    dt_masks_point_circle_t *c = out;
    _obj_float_array(p, "center", c->center, 2);
    c->radius = dt_masks_harvest_obj_float(p, "radius", 0.0f);
    c->border = dt_masks_harvest_obj_float(p, "border", 0.0f);
  }
  else if(type & DT_MASKS_ELLIPSE)
  {
    dt_masks_point_ellipse_t *e = out;
    _obj_float_array(p, "center", e->center, 2);
    _obj_float_array(p, "radius", e->radius, 2);
    e->rotation = dt_masks_harvest_obj_float(p, "rotation", 0.0f);
    e->border = dt_masks_harvest_obj_float(p, "border", 0.0f);
    e->flags = (int)dt_masks_harvest_obj_int(p, "flags", 0);
  }
  else if(type & DT_MASKS_PATH)
  {
    dt_masks_point_path_t *q = out;
    _obj_float_array(p, "corner", q->corner, 2);
    _obj_float_array(p, "ctrl1", q->ctrl1, 2);
    _obj_float_array(p, "ctrl2", q->ctrl2, 2);
    _obj_float_array(p, "border", q->border, 2);
    q->state = (int)dt_masks_harvest_obj_int(p, "state", 0);
  }
  else if(type & DT_MASKS_BRUSH)
  {
    dt_masks_point_brush_t *b = out;
    _obj_float_array(p, "corner", b->corner, 2);
    _obj_float_array(p, "ctrl1", b->ctrl1, 2);
    _obj_float_array(p, "ctrl2", b->ctrl2, 2);
    _obj_float_array(p, "border", b->border, 2);
    b->density = dt_masks_harvest_obj_float(p, "density", 1.0f);
    b->hardness = dt_masks_harvest_obj_float(p, "hardness", 1.0f);
    b->state = (int)dt_masks_harvest_obj_int(p, "state", 0);
  }
  else if(type & DT_MASKS_GRADIENT)
  {
    dt_masks_point_gradient_t *g = out;
    _obj_float_array(p, "anchor", g->anchor, 2);
    g->rotation = dt_masks_harvest_obj_float(p, "rotation", 0.0f);
    g->compression = dt_masks_harvest_obj_float(p, "compression", 0.0f);
    g->steepness = dt_masks_harvest_obj_float(p, "steepness", 0.0f);
    g->curvature = dt_masks_harvest_obj_float(p, "curvature", 0.0f);
    g->state = (int)dt_masks_harvest_obj_int(p, "state", 0);
  }
  else if(type & DT_MASKS_GROUP)
  {
    dt_masks_point_group_t *g = out;
    g->formid = (dt_mask_id_t)dt_masks_harvest_obj_int(p, "formid", INVALID_MASKID);
    g->parentid = (dt_mask_id_t)dt_masks_harvest_obj_int(p, "parentid", INVALID_MASKID);
    g->state = (int)dt_masks_harvest_obj_int(p, "state", 0);
    g->opacity = dt_masks_harvest_obj_float(p, "opacity", 1.0f);
    g->group_opacity = dt_masks_harvest_obj_float(p, "group_opacity", 1.0f);

    if(json_object_has_member(p, "refinement"))
    {
      JsonObject *r = json_object_get_object_member(p, "refinement");
      if(r)
      {
        g->refinement.enabled = (int)dt_masks_harvest_obj_int(r, "enabled", 0);
        g->refinement.feathering_radius = dt_masks_harvest_obj_float(r, "feathering_radius", 0.0f);
        g->refinement.feathering_guide = (int)dt_masks_harvest_obj_int(r, "feathering_guide", 0);
        g->refinement.blur_radius = dt_masks_harvest_obj_float(r, "blur_radius", 0.0f);
        g->refinement.contrast = dt_masks_harvest_obj_float(r, "contrast", 0.0f);
        g->refinement.brightness = dt_masks_harvest_obj_float(r, "brightness", 0.0f);
        g->refinement.details = dt_masks_harvest_obj_float(r, "details", 0.0f);
      }
    }
  }
}

/* detected by content rather than by extension, so that a renamed file
   still loads */
JsonParser *dt_masks_harvest_load(const char *path, GError **error)
{
  JsonParser *parser = json_parser_new();

  guchar magic[2] = { 0, 0 };
  FILE *probe = g_fopen(path, "rb");
  const gboolean gzipped =
    probe && fread(magic, 1, 2, probe) == 2 && magic[0] == 0x1f && magic[1] == 0x8b;
  if(probe) fclose(probe);

  if(!gzipped)
  {
    if(json_parser_load_from_file(parser, path, error)) return parser;
    g_object_unref(parser);
    return NULL;
  }

  GFile *file = g_file_new_for_path(path);
  GFileInputStream *raw = g_file_read(file, NULL, error);
  gboolean ok = FALSE;
  if(raw)
  {
    GZlibDecompressor *decomp = g_zlib_decompressor_new(G_ZLIB_COMPRESSOR_FORMAT_GZIP);
    GInputStream *plain =
      g_converter_input_stream_new(G_INPUT_STREAM(raw), G_CONVERTER(decomp));
    ok = json_parser_load_from_stream(parser, plain, NULL, error);
    g_object_unref(plain);
    g_object_unref(decomp);
    g_object_unref(raw);
  }
  g_object_unref(file);

  if(ok) return parser;
  g_object_unref(parser);
  return NULL;
}

JsonArray *dt_masks_harvest_open_edits(const char *json_path,
                                       const char *tag,
                                       JsonParser **parser)
{
  GError *err = NULL;
  // accepts the .gz the contributor actually sent, as well as a plain file
  *parser = dt_masks_harvest_load(json_path, &err);
  if(!*parser)
  {
    fprintf(stderr, "[%s] cannot read %s: %s\n",
            tag, json_path, err ? err->message : "unknown error");
    g_clear_error(&err);
    return NULL;
  }

  JsonNode *root = json_parser_get_root(*parser);
  JsonObject *ro = root ? json_node_get_object(root) : NULL;
  JsonArray *edits = ro && json_object_has_member(ro, "edits")
    ? json_object_get_array_member(ro, "edits") : NULL;
  if(!edits)
  {
    fprintf(stderr, "[%s] %s has no \"edits\" array\n", tag, json_path);
    g_clear_object(parser);
  }
  return edits;
}

gboolean dt_masks_harvest_report(const char *json_path,
                                 const char *report_path,
                                 const char *tag,
                                 gboolean (*section)(const char *json_path, FILE *rf))
{
  FILE *rf = report_path ? g_fopen(report_path, "wb") : NULL;
  if(rf) fputs("{", rf);
  const gboolean ok = section(json_path, rf);
  if(rf)
  {
    fputs("\n}\n", rf);
    fclose(rf);
    printf("[%s] per-edit report written to %s\n", tag, report_path);
  }
  return ok;
}

void dt_masks_harvest_remember(GHashTable *seen,
                               gchar *key,
                               const void *rep,
                               const size_t size)
{
  if(!key) return;
  void *store = calloc(1, size);
  if(store)
  {
    memcpy(store, rep, size);
    g_hash_table_insert(seen, key, store);
  }
  else
    g_free(key);
}

gchar *dt_masks_harvest_edit_key(JsonObject *edit)
{
  // "index" and "image_index" are deliberately absent: they say where the edit
  // sat in the harvest, which changes nothing about what it renders.
  static const char *const members[] =
    { "operation", "blendop_version", "multi_priority", "enabled",
      "image", "blend", "forms", NULL };

  GString *acc = g_string_new(NULL);
  JsonGenerator *gen = json_generator_new();
  for(int i = 0; members[i]; i++)
  {
    JsonNode *n = json_object_get_member(edit, members[i]);
    if(n)
    {
      json_generator_set_root(gen, n);
      gsize len = 0;
      gchar *txt = json_generator_to_data(gen, &len);
      if(txt) g_string_append_len(acc, txt, (gssize)len);
      g_free(txt);
    }
    // a separator so absent and empty members cannot alias into each other
    g_string_append_c(acc, 0x1f);
  }
  g_object_unref(gen);

  gchar *key = g_compute_checksum_for_string(G_CHECKSUM_SHA256, acc->str, acc->len);
  g_string_free(acc, TRUE);
  return key;
}

GList *dt_masks_harvest_read_forms(JsonArray *forms_arr)
{
  GList *forms = NULL;

  for(guint i = 0; i < json_array_get_length(forms_arr); i++)
  {
    JsonObject *fo = json_array_get_object_element(forms_arr, i);
    if(!fo) goto fail;

    const int type = (int)dt_masks_harvest_obj_int(fo, "type", 0);
    dt_masks_form_t *form = dt_masks_create(type);
    if(!form) goto fail;

    form->formid = (dt_mask_id_t)dt_masks_harvest_obj_int(fo, "formid", INVALID_MASKID);
    form->version = (int)dt_masks_harvest_obj_int(fo, "version", dt_masks_version());
    snprintf(form->name, sizeof(form->name), "form %d", form->formid);
    _obj_float_array(fo, "source", form->source, 3);

    if(json_object_has_member(fo, "points_error"))
    {
      dt_masks_free_form(form);
      goto fail;
    }

    if(json_object_has_member(fo, "points") && form->functions)
    {
      JsonArray *pts = json_object_get_array_member(fo, "points");
      const size_t psize = form->functions->point_struct_size;
      for(guint k = 0; pts && k < json_array_get_length(pts); k++)
      {
        JsonObject *po = json_array_get_object_element(pts, k);
        if(!po) continue;
        void *point = calloc(1, psize);
        if(!point) continue;
        _read_point(po, type, point);
        form->points = g_list_append(form->points, point);
      }
    }

    // replayed at the current masks version: the harvest decoded each blob
    // with its version's stride and the loader's zero fill, so this is the
    // state after loading, not the stored one
    form->version = dt_masks_version();

    forms = g_list_append(forms, form);
  }
  return forms;

fail:
  g_list_free_full(forms, (GDestroyNotify)dt_masks_free_form);
  return NULL;
}

void dt_masks_harvest_read_blend_params(JsonObject *b, dt_develop_blend_params_t *p)
{
  memset(p, 0, sizeof(*p));
  p->mask_mode = (uint32_t)dt_masks_harvest_obj_int(b, "mask_mode", 0);
  p->blend_cst = (int32_t)dt_masks_harvest_obj_int(b, "blend_cst", 0);
  p->blend_mode = (uint32_t)dt_masks_harvest_obj_int(b, "blend_mode", 0);
  p->blend_parameter = dt_masks_harvest_obj_float(b, "blend_parameter", 0.0f);
  p->opacity = dt_masks_harvest_obj_float(b, "opacity", 100.0f);
  p->mask_combine = (uint32_t)dt_masks_harvest_obj_int(b, "mask_combine", 0);
  p->mask_id = (dt_mask_id_t)dt_masks_harvest_obj_int(b, "mask_id", INVALID_MASKID);
  p->blendif = (uint32_t)dt_masks_harvest_obj_int(b, "blendif", 0);
  p->feathering_radius = dt_masks_harvest_obj_float(b, "feathering_radius", 0.0f);
  p->feathering_guide = (uint32_t)dt_masks_harvest_obj_int(b, "feathering_guide", 0);
  p->blur_radius = dt_masks_harvest_obj_float(b, "blur_radius", 0.0f);
  p->contrast = dt_masks_harvest_obj_float(b, "contrast", 0.0f);
  p->brightness = dt_masks_harvest_obj_float(b, "brightness", 0.0f);
  p->details = dt_masks_harvest_obj_float(b, "details", 0.0f);
  p->feather_version = (uint32_t)dt_masks_harvest_obj_int(b, "feather_version", 0);
  _obj_float_array(b, "blendif_parameters", p->blendif_parameters,
                   4 * DEVELOP_BLENDIF_SIZE);
  _obj_float_array(b, "blendif_boost_factors", p->blendif_boost_factors,
                   DEVELOP_BLENDIF_SIZE);
  const char *src = dt_masks_harvest_obj_str(b, "raster_mask_source", "");
  dt_strlcpy_to_fixed(p->raster_mask_source, src ? src : "", sizeof(p->raster_mask_source));
  p->raster_mask_instance = (int)dt_masks_harvest_obj_int(b, "raster_mask_instance", 0);
  p->raster_mask_id = (dt_mask_id_t)dt_masks_harvest_obj_int(b, "raster_mask_id", INVALID_MASKID);
  p->raster_mask_invert = dt_masks_harvest_obj_int(b, "raster_mask_invert", 0) ? TRUE : FALSE;
}

const char *dt_masks_harvest_read_classic_edit(JsonObject *edit,
                                               dt_masks_harvest_edit_t *e)
{
  memset(e, 0, sizeof(*e));

  JsonObject *bo = json_object_get_object_member(edit, "blend");
  if(!bo) return "no blend object";
  dt_masks_harvest_read_blend_params(bo, &e->bp);
  if(e->bp.mask_mode & DEVELOP_MASK_FLEXI) return "already flexi";

  JsonObject *img = json_object_get_object_member(edit, "image");
  e->width = (int)dt_masks_harvest_obj_int(img, "width", 0);
  e->height = (int)dt_masks_harvest_obj_int(img, "height", 0);
  if(e->width <= 0 || e->height <= 0) return "no image dimensions";

  JsonArray *fa = json_object_has_member(edit, "forms")
    ? json_object_get_array_member(edit, "forms") : NULL;
  e->forms = fa ? dt_masks_harvest_read_forms(fa) : NULL;
  if(fa && json_array_get_length(fa) > 0 && !e->forms)
    return "forms could not be reconstructed";

  e->operation = dt_masks_harvest_obj_str(edit, "operation", NULL);
  e->multi_priority = (int)dt_masks_harvest_obj_int(edit, "multi_priority", 0);
  e->blendop_version = (int)dt_masks_harvest_obj_int(edit, "blendop_version", 14);
  return NULL;
}

// ---------------------------------------------------------------------------
// the replay harness
// ---------------------------------------------------------------------------


/** the OpenCL device the GPU replays run on, locked once for the run: per
    edit, locking would dominate the runtime. -1 without one, and the run then
    says it reports CPU results only */
static int _verify_devid = -1;

/** the output of the module the mask belongs to: the probe at +1 EV. The
    blend computes `out = in * (1 - mask) + module_out * mask`, so with the
    input as output every mask would render the same image; and blendif's
    output channels read this output, and would only repeat the input
    channels. In the linear probe +1 EV is `in * 2`, so the effect
    `|module_out - in|` is the image value itself, nonzero but for black and at
    most 1: the mask difference bounds the image difference, which is why the
    verdict is on the mask. Not clipped: scene-linear values above 1 are
    ordinary, and clipping would flatten the effect over the probe's upper
    half */
static float *_make_module_output(const float *const probe, const size_t npix)
{
  float *m = dt_alloc_align_float(npix * 4);
  if(!m) return NULL;
  for(size_t i = 0; i < npix * 4; i++) m[i] = probe[i] * 2.0f;
  return m;
}

/** the mask dt_develop_blend_process() published for the last render */
static const float *_published_mask(replay_t *r)
{
  return g_hash_table_lookup(r->piece.raster_masks,
                             GINT_TO_POINTER(BLEND_RASTER_ID));
}

float *dt_masks_verify_render_mask(replay_t *r, float **image)
{
  const size_t npix = (size_t)r->roi.width * r->roi.height;
  if(image) *image = NULL;

  // the blend writes into `out`, mixing it with the input by the mask -- so it
  // starts as what the module produced, not as a copy of the input (see
  // _make_module_output)
  memcpy(r->out, r->modout, sizeof(float) * npix * 4);

  // the group lookup walks pipe->forms, and migration may have added forms to
  // dev->forms since the last render
  r->pipe.forms = r->dev.forms;

  // wired as in a live pipe: the classic raster branch reads
  // module->raster_mask.sink.source, which only dt_iop_commit_blend_params()
  // sets, and the same call registers the sources of the flexi raster
  // elements (_reconcile_raster_form_users). Done for every edit, raster or
  // not, so that there is one path
  dt_develop_blend_params_t committed = *r->module.blend_params;
  dt_iop_commit_blend_params(&r->module, &committed, &r->pipe);

  dt_develop_blend_process(&r->module, &r->piece, r->probe, r->out,
                           &r->roi, &r->roi);

  const float *m = _published_mask(r);
  if(!m) return NULL;

  float *copy = dt_alloc_align_float(npix);
  if(!copy) return NULL;
  memcpy(copy, m, sizeof(float) * npix);

  // the blended image, to compare as well
  if(image)
  {
    *image = dt_alloc_align_float(npix * 4);
    if(*image) memcpy(*image, r->out, sizeof(float) * npix * 4);
  }
  return copy;
}

/** the same render through dt_develop_blend_process_cl(), a separate
    implementation kept in step by hand, and the one most users run. It
    publishes the finished mask through dt_iop_piece_set_raster(), as the CPU
    one does */
static float *_render_mask_cl(replay_t *r, float **image)
{
  if(image) *image = NULL;
#ifdef HAVE_OPENCL
  if(r->devid < 0) return NULL;

  const size_t npix = (size_t)r->roi.width * r->roi.height;
  const int w = r->roi.width, h = r->roi.height;

  float *copy = NULL;
  cl_mem dev_in = dt_opencl_alloc_device(r->devid, w, h, sizeof(float) * 4);
  cl_mem dev_out = dt_opencl_alloc_device(r->devid, w, h, sizeof(float) * 4);
  if(!dev_in || !dev_out) goto done;

  // the same starting state as the CPU render: the output starts as the
  // module's output (see _make_module_output)
  if(dt_opencl_write_host_to_image(r->devid, r->probe, dev_in, w, h, sizeof(float) * 4)
     != CL_SUCCESS) goto done;
  if(dt_opencl_write_host_to_image(r->devid, r->modout, dev_out, w, h, sizeof(float) * 4)
     != CL_SUCCESS) goto done;

  r->pipe.forms = r->dev.forms;
  r->pipe.devid = r->devid;

  dt_develop_blend_params_t committed = *r->module.blend_params;
  dt_iop_commit_blend_params(&r->module, &committed, &r->pipe);

  if(!dt_develop_blend_process_cl(&r->module, &r->piece, dev_in, dev_out,
                                  &r->roi, &r->roi))
    goto done;

  const float *m = _published_mask(r);
  if(!m) goto done;

  copy = dt_alloc_align_float(npix);
  if(copy) memcpy(copy, m, sizeof(float) * npix);

  if(image)
  {
    float *img = dt_alloc_align_float(npix * 4);
    // a failed readback leaves *image NULL, which the caller treats as
    // "no image comparison here" rather than comparing against garbage
    if(img
       && dt_opencl_copy_image_to_host(r->devid, img, dev_out, w, h,
                                       sizeof(float) * 4) == CL_SUCCESS)
      *image = img;
    else
      dt_free_align(img);
  }

done:
  dt_opencl_release_mem_object(dev_in);
  dt_opencl_release_mem_object(dev_out);
  r->pipe.devid = DT_DEVICE_CPU;
  return copy;
#else
  return NULL;
#endif
}

/** how two masks differ: the worst deviation, the mean over every pixel, and
    how many pixels differ. The worst deviation alone cannot tell one stray
    pixel from an inverted mask */
typedef struct _diff_stats_t
{
  double max;
  double mean;
  int differing;
} _diff_stats_t;

static _diff_stats_t _diff_stats(const float *a, const float *b, const size_t n)
{
  _diff_stats_t st = { 0.0, 0.0, 0 };
  double sum = 0.0;
  for(size_t i = 0; i < n; i++)
  {
    const double d = fabs((double)a[i] - (double)b[i]);
    if(d > st.max) st.max = d;
    sum += d;
    if(d > VERIFY_EPS_IDENTICAL) st.differing++;
  }
  st.mean = n ? sum / (double)n : 0.0;
  return st;
}

/** the same over a rendered image, RGB only: the fourth float is not image
    content. A mask difference shows wherever the mask moved; an image
    difference is what a user could see, the mask difference scaled by the
    module's effect, and hides a mask error where that effect is small. Either
    alone misleads, so both are reported */
static _diff_stats_t _diff_stats_rgb(const float *a, const float *b, const size_t npix)
{
  _diff_stats_t st = { 0.0, 0.0, 0 };
  double sum = 0.0;
  for(size_t i = 0; i < npix; i++)
  {
    double worst_ch = 0.0;
    for(int c = 0; c < 3; c++)
    {
      const double d = fabs((double)a[i * 4 + c] - (double)b[i * 4 + c]);
      if(d > worst_ch) worst_ch = d;
      sum += d;
    }
    if(worst_ch > st.max) st.max = worst_ch;
    // a pixel counts once if any of its channels moved, as in the integration
    // suite's count-diff-pixels
    if(worst_ch > VERIFY_EPS_IDENTICAL) st.differing++;
  }
  st.mean = npix ? sum / (double)(npix * 3) : 0.0;
  return st;
}

/** worst absolute deviation between two masks */
double dt_masks_verify_max_abs_diff(const float *a, const float *b, const size_t n)
{
  return _diff_stats(a, b, n).max;
}

/** is this mask the same value everywhere? Then the comparison proves
    nothing: it matches another uniform mask whatever migration did */
static gboolean _is_uniform(const float *m, const size_t n)
{
  if(n == 0) return TRUE;
  for(size_t i = 1; i < n; i++)
    if(fabsf(m[i] - m[0]) > VERIFY_EPS_IDENTICAL) return FALSE;
  return TRUE;
}

/** Find the module shared-object for an operation name. */
static dt_iop_module_so_t *_find_so(const char *op)
{
  if(!op || !*op) return NULL;
  for(GList *l = darktable.iop; l; l = g_list_next(l))
  {
    dt_iop_module_so_t *so = l->data;
    if(so && !strcmp(so->op, op)) return so;
  }
  return NULL;
}

void dt_masks_verify_replay_cleanup(replay_t *r)
{
  darktable.develop = r->saved_develop;
  if(r->module_loaded) dt_iop_cleanup_module(&r->module);
  if(r->source_loaded) dt_iop_cleanup_module(&r->source_module);
  if(r->dev_mutex_ready) dt_pthread_mutex_destroy(&r->dev.history_mutex);
  if(r->piece.raster_masks) g_hash_table_destroy(r->piece.raster_masks);
  if(r->source_piece.raster_masks) g_hash_table_destroy(r->source_piece.raster_masks);
  g_list_free(r->pipe.nodes);
  g_list_free(r->dev.iop);
  dt_free_align(r->probe);
  dt_free_align(r->modout);
  dt_free_align(r->out);
  g_list_free_full(r->dev.forms, (GDestroyNotify)dt_masks_free_form);
  memset(r, 0, sizeof(*r));
}

/** the mask the stand-in source module has written. Not flat and not taken
    from the probe: it varies enough for an inversion, an opacity, a blur or a
    tone curve to leave a trace, reaches exactly 0 and 1 so that a polarity
    error shows, and its smooth falloff disagrees with the probe's hard edges,
    which guided-filter feathering needs */
static float *_synthetic_raster_mask(const int w, const int h)
{
  float *m = dt_alloc_align_float((size_t)w * h);
  if(!m) return NULL;

  const float cx = 0.5f * (float)(w - 1);
  const float cy = 0.5f * (float)(h - 1);
  const float norm = 1.0f / sqrtf(cx * cx + cy * cy);

  for(int y = 0; y < h; y++)
    for(int x = 0; x < w; x++)
    {
      const float dx = ((float)x - cx) * norm;
      const float dy = ((float)y - cy) * norm;
      // a soft disc, exactly 1 near the center and 0 in the corners; the
      // diagonal term breaks the symmetry, so that a transposition shows
      const float rad = sqrtf(dx * dx + dy * dy) * 1.6f;
      const float diag = 0.15f * (((float)x / (float)MAX(1, w - 1))
                                  - ((float)y / (float)MAX(1, h - 1)));
      const float t = CLAMPF(1.0f - rad + diag, 0.0f, 1.0f);
      // smoothstep, so the interior has real gradient rather than a cone
      m[(size_t)y * w + x] = t * t * (3.0f - 2.0f * t);
    }
  return m;
}

/** give the replay the source piece a raster edit reads from. The classic
    raster branch and the flexi raster element both fetch through
    dt_dev_get_raster_mask(), which walks pipe->nodes for the source piece and
    dev->iop for the source module: without them both sides get no mask and
    match on nothing. The fetch is shared, so the comparison isolates what
    differs: classic's inline opacity and raster_mask_invert against the flexi
    group combining the raster as an element. NULL on success, else a skip
    reason */
static const char *_attach_raster_source(replay_t *r,
                                         const dt_develop_blend_params_t *bp)
{
  dt_iop_module_so_t *src_so = _find_so(bp->raster_mask_source);
  if(!src_so) return "raster source module not in this build";

  if(dt_iop_load_module_by_so(&r->source_module, src_so, &r->dev))
    return "raster source instance could not be loaded";
  r->source_loaded = TRUE;
  r->source_module.dev = &r->dev;
  r->source_module.multi_priority = bp->raster_mask_instance;

  // the source must come earlier in the pipe, or dt_dev_get_raster_mask()
  // refuses the fetch; in the edit it did. The order list cannot place it: a
  // harvested instance often has no entry, and a source of the target's
  // operation differs from it by instance only
  r->source_module.iop_order = r->module.iop_order - 1.0;

  // dt_dev_get_raster_mask() deletes the masks of a source that is disabled
  // or writes none, so the stand-in must look like one that writes masks
  if(r->source_module.blend_params)
    r->source_module.blend_params->mask_mode =
      DEVELOP_MASK_ENABLED | DEVELOP_MASK_MASK;

  float *raster = _synthetic_raster_mask(r->roi.width, r->roi.height);
  if(!raster) return "raster mask allocation failure";

  r->source_piece.pipe = &r->pipe;
  r->source_piece.module = &r->source_module;
  r->source_piece.enabled = TRUE;
  r->source_piece.colors = 4;
  r->source_piece.iscale = 1.0f;
  r->source_piece.processed_roi_in = r->roi;
  r->source_piece.processed_roi_out = r->roi;
  r->source_piece.raster_masks =
    g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, dt_free_align_ptr);
  g_hash_table_insert(r->source_piece.raster_masks,
                      GINT_TO_POINTER(bp->raster_mask_id), raster);

  // source first: the fetch walks nodes forward from the source to the target,
  // and stops as soon as it reaches the target module
  r->pipe.nodes = g_list_append(r->pipe.nodes, &r->source_piece);
  r->pipe.nodes = g_list_append(r->pipe.nodes, &r->piece);
  // _raster_resolve_source() (masks/raster.c) matches op + multi_priority
  // against dev->iop, which is how the flexi side finds the same module the
  // classic side reaches through blend_params
  r->dev.iop = g_list_append(r->dev.iop, &r->source_module);
  r->dev.iop = g_list_append(r->dev.iop, &r->module);

  return NULL;
}

/** the least a blend needs: an instance of the harvested module, a dev
    holding the forms, and a pipe and piece carrying the blend params. A real
    instance, not a filled-in struct: dt_develop_blend_process() calls its
    flags() and blend_colorspace(), and a Lab module and a scene-referred RGB
    one take different paths through the blendif code */
void dt_masks_verify_replay_size(const int full_width,
                                 const int full_height,
                                 int *width,
                                 int *height)
{
  *width = full_width;
  *height = full_height;
  if(full_width > VERIFY_MAX_EDGE || full_height > VERIFY_MAX_EDGE)
  {
    const double s = (double)VERIFY_MAX_EDGE / (double)MAX(full_width, full_height);
    *width = MAX(8, (int)(full_width * s));
    *height = MAX(8, (int)(full_height * s));
  }
}

const char *dt_masks_verify_replay_init(replay_t *r,
                             const char *operation,
                             const dt_develop_blend_params_t *bp,
                             GList *forms,
                             const int full_width,
                             const int full_height,
                             const int width,
                             const int height)
{
  memset(r, 0, sizeof(*r));

  r->devid = _verify_devid;

  dt_iop_module_so_t *so = _find_so(operation);
  if(!so) return "module not in this build";

  r->dev.forms = forms;

  /* darktable.develop must be this dev while the replay renders:
     dt_masks_group_hash() resolves members through it, and hashes a member's
     state, opacity and refinement only when it finds the member's form.
     Headless it is NULL, so the hash would not follow the mask, blend.c's
     drawn-mask cache would serve the first render's mask to every later one,
     and a check comparing two renders would pass on nothing. Restored on
     cleanup */
  r->saved_develop = darktable.develop;
  darktable.develop = &r->dev;

  /* and a form_gui: a shape's modify_property(), behind every geometry
     slider, reads the canvas editing state, and brush.c does so without a
     NULL check. Zeroed with nothing selected, it is the panel's state while a
     slider is dragged on a selected shape: the sliders edit its points, not
     the defaults of the next shape, and act on the whole shape */
  memset(&r->form_gui, 0, sizeof(r->form_gui));
  r->form_gui.point_selected = -1;
  r->form_gui.point_edited = -1;
  r->form_gui.feather_selected = -1;
  r->form_gui.seg_selected = -1;
  r->form_gui.group_selected = -1;
  r->dev.form_gui = &r->form_gui;

  // masks.c takes dev->history_mutex to change dev->forms, and migration goes
  // through it. It must be recursive, as dt_dev_init() makes it: these paths
  // re-enter, and a plain mutex would deadlock
  dt_pthread_recursive_mutex_init(&r->dev.history_mutex);
  r->dev_mutex_ready = TRUE;

  // note the sense: this returns TRUE on *failure* (see its callers in
  // imageop.c and blend.c, which all read it that way)
  if(dt_iop_load_module_by_so(&r->module, so, &r->dev))
    return "module instance could not be loaded";
  r->module_loaded = TRUE;
  r->module.dev = &r->dev;

  if(!r->module.blend_params)
  {
    dt_iop_cleanup_module(&r->module);
    darktable.develop = r->saved_develop;
    memset(r, 0, sizeof(*r));
    return "module has no blend_params";
  }
  // copy into the module's own allocation rather than repointing it: the
  // module owns that buffer and frees it on cleanup
  memcpy(r->module.blend_params, bp, sizeof(dt_develop_blend_params_t));

  r->pipe.forms = forms;
  r->pipe.type = DT_DEV_PIXELPIPE_EXPORT; // never the focused GUI pipe
  // the full image size: masks are normalized against it, even though the
  // replay renders smaller
  r->pipe.iwidth = full_width;
  r->pipe.iheight = full_height;
  // makes dt_develop_blend_process() publish its finished mask, which is how
  // the mask is read without touching the blend code
  r->pipe.store_all_raster_masks = TRUE;

  r->piece.pipe = &r->pipe;
  r->piece.module = &r->module;
  r->piece.blendop_data = r->module.blend_params;
  r->piece.colors = 4;
  r->piece.enabled = TRUE;
  // not 0: radii are converted to pixels as `roi_out->scale / piece->iscale`
  // (blend.c), and a zero iscale asks the guided filter for an infinite window,
  // which runs forever
  r->piece.iscale = 1.0f;
  r->piece.raster_masks =
    g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, dt_free_align_ptr);

  // color management: make_mask() leaves the mask untouched when it gets no
  // profile, so parametric masks would not be evaluated, and the two sides
  // would differ, classic through its CONDITIONAL branch, migrated without it.
  // The profile lookup needs an iop order list, to place the module against
  // colorin and colorout, and the pipe needs profiles: linear Rec2020, the
  // default working space
  r->dev.iop_order_list = darktable.iop_order_list;
  r->module.iop_order =
    dt_ioppr_get_iop_order(r->dev.iop_order_list, r->module.op, r->module.multi_priority);

  dt_ioppr_set_pipe_work_profile_info(&r->dev, &r->pipe,
                                      DT_COLORSPACE_LIN_REC2020, "", DT_INTENT_PERCEPTUAL);
  dt_ioppr_set_pipe_output_profile_info(&r->dev, &r->pipe,
                                        DT_COLORSPACE_LIN_REC2020, "", DT_INTENT_PERCEPTUAL);

  // the input profile too: on this replay's order list colorin and colorout
  // have no order ("cannot get iop-order for colorin instance 0"), so every
  // module counts as before colorin and the input profile is the one read.
  // Without it the OpenCL kernel writes no mask at all, and a GPU comparison
  // of two empty masks passes on nothing. Set directly, not through
  // dt_ioppr_set_pipe_input_profile_info(), which reads the image cache and
  // crashes with no image behind the replay
  r->pipe.input_profile_info =
    dt_ioppr_add_profile_info_to_list(&r->dev, DT_COLORSPACE_LIN_REC2020, "",
                                      DT_INTENT_PERCEPTUAL);

  r->roi.x = 0;
  r->roi.y = 0;
  r->roi.width = width;
  r->roi.height = height;
  // the downscale actually applied, so radii shrink with the raster
  r->roi.scale = full_width > 0 ? (float)width / (float)full_width : 1.0f;

  // the raster fetch reads these off the target piece, to decide whether a
  // module in between distorts the mask (equal rois: none does) and to check
  // the size of the mask it returns
  r->piece.processed_roi_in = r->roi;
  r->piece.processed_roi_out = r->roi;

  if(bp->raster_mask_source[0])
  {
    const char *raster_err = _attach_raster_source(r, bp);
    if(raster_err)
    {
      dt_masks_verify_replay_cleanup(r);
      return raster_err;
    }
  }

  r->probe = dt_masks_probe_new(width, height);
  r->modout = _make_module_output(r->probe, (size_t)width * height);
  r->out = dt_alloc_align_float((size_t)width * height * 4);
  if(!r->probe || !r->out)
  {
    dt_masks_verify_replay_cleanup(r);
    return "buffer allocation failure";
  }
  return NULL;
}

// ---------------------------------------------------------------------------
// one edit
// ---------------------------------------------------------------------------

// how many group levels sit below `grp`: 0 for a list of shapes alone
static int _nesting(GList *forms, const dt_masks_form_t *grp, const int depth)
{
  if(!grp || depth > DT_MASKS_NESTING_MAX) return 0;
  int deepest = 0;
  for(const GList *l = grp->points; l; l = g_list_next(l))
  {
    const dt_masks_point_group_t *pt = l->data;
    if(dt_masks_point_is_marker(pt)) continue;
    const dt_masks_form_t *f = dt_masks_get_from_id_ext(forms, pt->formid);
    if(f && f != grp && (f->type & DT_MASKS_GROUP))
      deepest = MAX(deepest, 1 + _nesting(forms, f, depth + 1));
  }
  return deepest;
}

typedef struct
{
  verify_result_t result;
  const char *skip_reason;
  gboolean inert;
  // group levels below the module's mask after migration
  int nesting;
  // this edit is byte-identical to an earlier one and reused its verdict
  // rather than being rendered again (see dt_masks_harvest_edit_key)
  gboolean repeat;
  double max_diff;          // CPU: classic vs migrated -- the original verdict
  double mean_diff;
  int differing_pixels;

  // GPU replay. Present only when an OpenCL device was available; `gpu_ran`
  // says whether these numbers mean anything.
  gboolean gpu_ran;
  double gpu_max_diff;      // GPU: classic vs migrated
  double gpu_mean_diff;     // ... and how large that difference actually is
  int gpu_differing_pixels;

  // the same comparisons on the rendered image, i.e. what the mask difference
  // actually did to pixels (see _diff_stats_rgb)
  gboolean image_compared, gpu_image_compared;
  double image_max_diff, image_mean_diff;
  int image_differing_pixels;
  double gpu_image_max_diff, gpu_image_mean_diff;
  int gpu_image_differing_pixels;
  // the CPU and GPU disagreement on both sides of the migration. The two
  // blend implementations differ slightly on any edit, so only migration
  // widening the gap is a defect, and the classic gap is the baseline
  double dev_diff_before;
  double dev_diff_after;

  /* the migrated gap again with the mask refinement off, measured only where
     the gap widened past the threshold; `nopost_ran` says whether it was. A
     wider gap comes from migration, or from a stage after the mask, the same
     on both sides, that differs between CPU and OpenCL and got a slightly
     different input: above all feathering, a guided filter that amplifies.
     If the gap survives without it, migration owns it */
  gboolean nopost_ran;
  double dev_diff_after_nopost;

} edit_report_t;

static void _verify_edit(JsonObject *edit, edit_report_t *rep)
{
  memset(rep, 0, sizeof(*rep));
  rep->result = VERIFY_SKIPPED;

  // among others, an edit already flexi is skipped: migration leaves it
  // alone, so it has nothing to prove
  dt_masks_harvest_edit_t e;
  rep->skip_reason = dt_masks_harvest_read_classic_edit(edit, &e);
  if(rep->skip_reason) return;

  int w, h;
  dt_masks_verify_replay_size(e.width, e.height, &w, &h);

  replay_t r;
  const char *init_err =
    dt_masks_verify_replay_init(&r, e.operation, &e.bp, e.forms, e.width, e.height, w, h);
  if(init_err)
  {
    rep->result = VERIFY_ERROR;
    rep->skip_reason = init_err;
    return;
  }

  const size_t npix = (size_t)w * h;

  // --- before migration -------------------------------------------------
  float *before_img = NULL, *after_img = NULL;
  float *before_cl_img = NULL, *after_cl_img = NULL;
  float *before = dt_masks_verify_render_mask(&r, &before_img);
  if(!before)
  {
    rep->result = VERIFY_ERROR;
    rep->skip_reason = "classic render produced no mask";
    dt_masks_verify_replay_cleanup(&r);
    return;
  }

  rep->inert = _is_uniform(before, npix);

  // the classic edit on the GPU: the baseline of the CPU and GPU gap
  float *before_cl = _render_mask_cl(&r, &before_cl_img);

  // --- migrate ----------------------------------------------------------
  dt_masks_migrate_classic_to_flexi(&r.module, r.module.blend_params, -1);
  rep->nesting =
    _nesting(r.dev.forms,
             dt_masks_get_from_id_ext(r.dev.forms, r.module.blend_params->mask_id), 0);

  // --- after migration --------------------------------------------------
  float *after = dt_masks_verify_render_mask(&r, &after_img);
  if(!after)
  {
    rep->result = VERIFY_ERROR;
    rep->skip_reason = "flexi render produced no mask";
    dt_free_align(before);
    dt_free_align(before_cl);
    dt_free_align(before_img);
    dt_free_align(before_cl_img);
    dt_masks_verify_replay_cleanup(&r);
    return;
  }

  float *after_cl = _render_mask_cl(&r, &after_cl_img);

  // the GPU comparison needs both renders; one without the other is counted
  // as an error below
  if(before_cl && after_cl && (darktable.unmuted & DT_DEBUG_MASKS))
  {
    // with -d masks, the range of all four masks
    float mn[4], mx[4]; double sm[4];
    const float *bufs[4] = { before, after, before_cl, after_cl };
    const char *nm[4] = { "cpu_classic", "cpu_flexi ", "gpu_classic", "gpu_flexi " };
    for(int k = 0; k < 4; k++)
    {
      mn[k] = mx[k] = bufs[k][0]; sm[k] = 0.0;
      for(size_t i = 0; i < npix; i++)
      { mn[k] = fminf(mn[k], bufs[k][i]); mx[k] = fmaxf(mx[k], bufs[k][i]); sm[k] += bufs[k][i]; }
      printf("[verify]      %s min=%.4f max=%.4f mean=%.4f\n", nm[k], mn[k], mx[k], sm[k]/npix);
    }
    printf("[verify]      mask_mode %u -> %u, mask_id %d -> %d, combine %u -> %u\n",
           e.bp.mask_mode, r.module.blend_params->mask_mode,
           e.bp.mask_id, r.module.blend_params->mask_id,
           e.bp.mask_combine, r.module.blend_params->mask_combine);
  }

  if(before_cl && after_cl)
  {
    rep->gpu_ran = TRUE;
    const _diff_stats_t g = _diff_stats(before_cl, after_cl, npix);
    rep->gpu_max_diff = g.max;
    rep->gpu_mean_diff = g.mean;
    rep->gpu_differing_pixels = g.differing;

    if(before_cl_img && after_cl_img)
    {
      const _diff_stats_t gi = _diff_stats_rgb(before_cl_img, after_cl_img, npix);
      rep->gpu_image_compared = TRUE;
      rep->gpu_image_max_diff = gi.max;
      rep->gpu_image_mean_diff = gi.mean;
      rep->gpu_image_differing_pixels = gi.differing;
    }
    rep->dev_diff_before = dt_masks_verify_max_abs_diff(before, before_cl, npix);
    rep->dev_diff_after = dt_masks_verify_max_abs_diff(after, after_cl, npix);

    // the gap widened: render the migrated pair again with the refinement
    // off (see dev_diff_after_nopost). Migration leaves these fields as they
    // are, so zeroing them turns off the same stages on both sides, and every
    // render commits the params again
    if(rep->dev_diff_after - rep->dev_diff_before > VERIFY_EPS_EQUIVALENT)
    {
      dt_develop_blend_params_t *const p = r.module.blend_params;
      const float keep_feather = p->feathering_radius;
      const float keep_blur = p->blur_radius;
      const float keep_contrast = p->contrast;
      const float keep_brightness = p->brightness;
      const float keep_details = p->details;

      p->feathering_radius = 0.0f;
      p->blur_radius = 0.0f;
      p->contrast = 0.0f;
      p->brightness = 0.0f;
      p->details = 0.0f;

      float *np = dt_masks_verify_render_mask(&r, NULL);
      float *np_cl = _render_mask_cl(&r, NULL);
      if(np && np_cl)
      {
        rep->nopost_ran = TRUE;
        rep->dev_diff_after_nopost = dt_masks_verify_max_abs_diff(np, np_cl, npix);
      }
      dt_free_align(np);
      dt_free_align(np_cl);

      p->feathering_radius = keep_feather;
      p->blur_radius = keep_blur;
      p->contrast = keep_contrast;
      p->brightness = keep_brightness;
      p->details = keep_details;
    }
  }

  // --- compare ----------------------------------------------------------
  const _diff_stats_t c = _diff_stats(before, after, npix);
  const double max_d = c.max;

  if(before_img && after_img)
  {
    const _diff_stats_t ci = _diff_stats_rgb(before_img, after_img, npix);
    rep->image_compared = TRUE;
    rep->image_max_diff = ci.max;
    rep->image_mean_diff = ci.mean;
    rep->image_differing_pixels = ci.differing;
  }

  rep->max_diff = c.max;
  rep->mean_diff = c.mean;
  rep->differing_pixels = c.differing;

  if((darktable.unmuted & DT_DEBUG_MASKS) && max_d > VERIFY_EPS_EQUIVALENT)
  {
    float bmin = before[0], bmax = before[0], amin = after[0], amax = after[0];
    double bsum = 0.0, asum = 0.0;
    for(size_t i = 0; i < npix; i++)
    {
      bmin = fminf(bmin, before[i]); bmax = fmaxf(bmax, before[i]); bsum += before[i];
      amin = fminf(amin, after[i]);  amax = fmaxf(amax, after[i]);  asum += after[i];
    }
    printf("[verify]   DIFF before[min=%.4f max=%.4f mean=%.4f] "
           "after[min=%.4f max=%.4f mean=%.4f]\n",
           bmin, bmax, bsum / npix, amin, amax, asum / npix);
    printf("[verify]        mask_mode %u -> %u, blend_cst %d -> %d, "
           "opacity %.1f -> %.1f, mask_id %d -> %d, forms %d -> %d\n",
           e.bp.mask_mode, r.module.blend_params->mask_mode,
           e.bp.blend_cst, r.module.blend_params->blend_cst,
           e.bp.opacity, r.module.blend_params->opacity,
           e.bp.mask_id, r.module.blend_params->mask_id,
           g_list_length(e.forms), g_list_length(r.dev.forms));
  }

  // the verdict needs both: migration kept the mask on the CPU (max_d), and
  // did not widen the CPU and GPU gap past this edit's classic gap by more
  // than one 8-bit step, which absorbs kernel noise. Classic against migrated
  // on the GPU (gpu_max_diff) is only reported: classic's OpenCL blend can
  // differ from its CPU one by up to 0.97, and a migrated mask agreeing with
  // the CPU then differs from it
  double verdict_d = max_d;
  const double verdict_mean = c.mean;
  if(rep->gpu_ran)
  {
    // the widening compares two worst-pixel gaps and has no mean: it only
    // raises the max side of the test
    const double widened = rep->dev_diff_after - rep->dev_diff_before;
    if(widened > VERIFY_EPS_EQUIVALENT) verdict_d = MAX(verdict_d, widened);
  }

  // `deltae`'s rule: over tolerance on the worst pixel, or over a third of it
  // on average, is a real difference; well under it everywhere is identical.
  if(verdict_d <= VERIFY_EPS_IDENTICAL) rep->result = VERIFY_IDENTICAL;
  else if(verdict_d <= VERIFY_EPS_EQUIVALENT
          && verdict_mean <= VERIFY_EPS_EQUIVALENT_MEAN) rep->result = VERIFY_EQUIVALENT;
  else rep->result = VERIFY_DIFFERENT;

  // one GPU render without the other is an asymmetry: it must not pass
  if(_verify_devid >= 0 && !rep->gpu_ran && (before_cl || after_cl))
  {
    rep->result = VERIFY_ERROR;
    rep->skip_reason = before_cl ? "GPU rendered classic but not migrated"
                                 : "GPU rendered migrated but not classic";
  }

  dt_free_align(before);
  dt_free_align(after);
  dt_free_align(before_cl);
  dt_free_align(after_cl);
  dt_free_align(before_img);
  dt_free_align(after_img);
  dt_free_align(before_cl_img);
  dt_free_align(after_cl_img);
  dt_masks_verify_replay_cleanup(&r);
}

// ---------------------------------------------------------------------------
// driver
// ---------------------------------------------------------------------------

static const char *_result_name(const verify_result_t r)
{
  switch(r)
  {
    case VERIFY_IDENTICAL:  return "identical";
    case VERIFY_EQUIVALENT: return "equivalent";
    case VERIFY_DIFFERENT:  return "DIFFERENT";
    case VERIFY_SKIPPED:    return "skipped";
    default:                return "ERROR";
  }
}

gboolean dt_masks_verify_harvest_section(const char *json_path, FILE *rf)
{
  // line-buffered: a crash mid-replay must not swallow the progress output
  // that says which edit it was on
  setvbuf(stdout, NULL, _IOLBF, 0);

#ifdef _OPENMP
  // one thread, so that results reproduce: the blend reduces over pixels in
  // parallel, so the order of float additions, and with it a mask, varies
  // between runs, on some edits by 0.1, which looks like a migration bug. A
  // verdict that moves between runs cannot be investigated, and without that
  // noise the tolerance can stay tight
  omp_set_num_threads(1);
#endif

#ifdef HAVE_OPENCL
  // one device for the run, picked as for an export, the pipe type the replay
  // declares
  if(darktable.opencl && darktable.opencl->inited)
    _verify_devid = dt_opencl_lock_device(DT_DEV_PIXELPIPE_EXPORT);
  if(_verify_devid >= 0)
    printf("[verify] OpenCL device %d acquired: GPU blend will be replayed too\n",
           _verify_devid);
  else
    printf("[verify] no OpenCL device: CPU blend only\n");
#else
  printf("[verify] built without OpenCL: CPU blend only\n");
#endif

  JsonParser *parser;
  JsonArray *edits = dt_masks_harvest_open_edits(json_path, "verify", &parser);
  if(!edits) return FALSE;

  verify_stats_t st;
  memset(&st, 0, sizeof(st));
  st.worst_index = -1;
  st.worst_gpu_index = -1;
  st.worst_image_index = -1;
  st.worst_gpu_image_index = -1;

  if(rf) fprintf(rf, "\n  \"source\": \"%s\",\n  \"edits\": [", json_path);
  gboolean first_report = TRUE;

  const guint n = json_array_get_length(edits);
  printf("[verify] replaying %u harvested edits from %s\n", n, json_path);

  /* Exact repeats are rendered once (see dt_masks_harvest_edit_key). The
     verdict is stored against the edit's content key and reused, so every
     occurrence is still counted, reported and aggregated exactly as if it had
     been replayed -- only the four renders are skipped. */
  GHashTable *seen =
    g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  int replayed_unique = 0;

  for(guint i = 0; i < n; i++)
  {
    JsonObject *edit = json_array_get_object_element(edits, i);
    if(!edit) continue;

    edit_report_t rep;
    gchar *key = dt_masks_harvest_edit_key(edit);
    const edit_report_t *cached = key ? g_hash_table_lookup(seen, key) : NULL;
    if(cached)
    {
      rep = *cached;
      rep.repeat = TRUE;
      g_free(key);
    }
    else
    {
      // -d masks names each edit before replaying it: when one of these wedges
      // or crashes, the last line printed is the only thing that says which
      // configuration did it.
      if(darktable.unmuted & DT_DEBUG_MASKS)
        printf("[verify] edit %u op=%s mask_mode=%d\n", i,
               dt_masks_harvest_obj_str(edit, "operation", "?"),
               (int)dt_masks_harvest_obj_int(json_object_get_object_member(edit, "blend"),
                             "mask_mode", -1));
      _verify_edit(edit, &rep);
      rep.repeat = FALSE;
      replayed_unique++;
      dt_masks_harvest_remember(seen, key, &rep, sizeof(rep));
    }

    st.total++;
    switch(rep.result)
    {
      case VERIFY_IDENTICAL:  st.identical++; break;
      case VERIFY_EQUIVALENT: st.equivalent++; break;
      case VERIFY_DIFFERENT:  st.different++; break;
      case VERIFY_SKIPPED:    st.skipped++; break;
      default:                st.error++; break;
    }

    if(rep.result == VERIFY_IDENTICAL || rep.result == VERIFY_EQUIVALENT
       || rep.result == VERIFY_DIFFERENT)
    {
      if(rep.inert) st.inert_before++;
      else
      {
        st.live++;
        if(rep.result == VERIFY_IDENTICAL) st.live_identical++;
        else if(rep.result == VERIFY_EQUIVALENT) st.live_equivalent++;
        else st.live_different++;
      }
      if(rep.max_diff > st.worst_max_diff)
      {
        st.worst_max_diff = rep.max_diff;
        st.worst_index = (int)i;
        st.worst_mean_diff = rep.mean_diff;
        st.worst_differing_pixels = rep.differing_pixels;
      }
      if(rep.image_compared)
      {
        st.image_compared++;
        if(rep.image_max_diff > st.worst_image_diff)
        {
          st.worst_image_diff = rep.image_max_diff;
          st.worst_image_mean_diff = rep.image_mean_diff;
          st.worst_image_differing_pixels = rep.image_differing_pixels;
          st.worst_image_index = (int)i;
        }
      }
      if(rep.gpu_image_compared)
      {
        st.gpu_image_compared++;
        if(rep.gpu_image_max_diff > st.worst_gpu_image_diff)
        {
          st.worst_gpu_image_diff = rep.gpu_image_max_diff;
          st.worst_gpu_image_mean_diff = rep.gpu_image_mean_diff;
          st.worst_gpu_image_differing_pixels = rep.gpu_image_differing_pixels;
          st.worst_gpu_image_index = (int)i;
        }
      }
      if(rep.gpu_ran)
      {
        st.gpu_compared++;
        if(rep.gpu_max_diff > st.worst_gpu_diff)
        {
          st.worst_gpu_diff = rep.gpu_max_diff;
          st.worst_gpu_index = (int)i;
          st.worst_gpu_mean_diff = rep.gpu_mean_diff;
          st.worst_gpu_differing_pixels = rep.gpu_differing_pixels;
        }
        st.worst_dev_before = MAX(st.worst_dev_before, rep.dev_diff_before);
        st.worst_dev_after = MAX(st.worst_dev_after, rep.dev_diff_after);
        if(rep.dev_diff_after - rep.dev_diff_before > VERIFY_EPS_EQUIVALENT)
        {
          st.dev_gap_widened++;
          // the migrated pipeline disagreeing with itself once nothing runs
          // after the mask is migration's own inconsistency; a widening that
          // vanishes here was amplification by a stage classic runs too
          if(!rep.nopost_ran || rep.dev_diff_after_nopost > VERIFY_EPS_EQUIVALENT)
            st.dev_gap_widened_own++;
        }
      }
    }

    // skipped edits are written too: the report has to account for every edit
    // in the harvest, or reading it means reconciling it against the terminal
    // output to find out what happened to the missing indices
    if(rf)
    {
      fprintf(rf, "%s\n    {\"index\": %u, \"operation\": \"%s\", \"result\": \"%s\","
                  " \"inert\": %s, \"nesting\": %d, \"max_diff\": %.9g, \"mean_diff\": %.9g,"
                  " \"differing_pixels\": %d, \"gpu_ran\": %s,"
                  " \"gpu_max_diff\": %.9g, \"gpu_mean_diff\": %.9g,"
                  " \"gpu_differing_pixels\": %d,"
                              " \"repeat\": %s, \"image_compared\": %s, \"image_max_diff\": %.9g,"
                  " \"image_mean_diff\": %.9g, \"image_differing_pixels\": %d,"
                  " \"gpu_image_compared\": %s, \"gpu_image_max_diff\": %.9g,"
                  " \"gpu_image_mean_diff\": %.9g,"
                  " \"gpu_image_differing_pixels\": %d,"
                  " \"dev_diff_before\": %.9g,"
                  " \"dev_diff_after\": %.9g,"
                  " \"nopost_ran\": %s, \"dev_diff_after_nopost\": %.9g%s%s%s}",
              first_report ? "" : ",", i,
              dt_masks_harvest_obj_str(edit, "operation", "?"),
              _result_name(rep.result),
              rep.inert ? "true" : "false", rep.nesting,
              rep.max_diff, rep.mean_diff, rep.differing_pixels,
              rep.gpu_ran ? "true" : "false",
              rep.gpu_max_diff, rep.gpu_mean_diff, rep.gpu_differing_pixels,
              rep.repeat ? "true" : "false",
              rep.image_compared ? "true" : "false",
              rep.image_max_diff, rep.image_mean_diff, rep.image_differing_pixels,
              rep.gpu_image_compared ? "true" : "false",
              rep.gpu_image_max_diff, rep.gpu_image_mean_diff,
              rep.gpu_image_differing_pixels,
              rep.dev_diff_before, rep.dev_diff_after,
              rep.nopost_ran ? "true" : "false", rep.dev_diff_after_nopost,
              rep.skip_reason ? ", \"reason\": \"" : "",
              rep.skip_reason ? rep.skip_reason : "",
              rep.skip_reason ? "\"" : "");
      first_report = FALSE;
    }

    if((i + 1) % 250 == 0)
      printf("[verify]   %u/%u ...\n", i + 1, n);
  }

  g_object_unref(parser);

  const gboolean passed = st.different == 0 && st.error == 0;

  // Every number the summary below prints also goes into the report, so the
  // file is self-contained: reading a run must not require having kept the
  // terminal output that went with it.
  if(rf)
  {
    fputs("\n  ],\n  \"summary\": {\n", rf);
    fprintf(rf, "    \"passed\": %s,\n", passed ? "true" : "false");
    fprintf(rf, "    \"harvested\": %u,\n", n);
    fprintf(rf, "    \"replayed\": %d,\n", st.total);
    fprintf(rf, "    \"distinct_edits\": %d,\n", replayed_unique);
    fprintf(rf, "    \"identical\": %d,\n", st.identical);
    fprintf(rf, "    \"equivalent\": %d,\n", st.equivalent);
    fprintf(rf, "    \"different\": %d,\n", st.different);
    fprintf(rf, "    \"skipped\": %d,\n", st.skipped);
    fprintf(rf, "    \"errors\": %d,\n", st.error);
    fprintf(rf, "    \"live\": %d,\n", st.live);
    fprintf(rf, "    \"live_identical\": %d,\n", st.live_identical);
    fprintf(rf, "    \"live_equivalent\": %d,\n", st.live_equivalent);
    fprintf(rf, "    \"live_different\": %d,\n", st.live_different);
    fprintf(rf, "    \"inert\": %d,\n", st.inert_before);
    fprintf(rf, "    \"worst_cpu_diff\": %.9g,\n", st.worst_max_diff);
    fprintf(rf, "    \"worst_cpu_diff_index\": %d,\n", st.worst_index);
    fprintf(rf, "    \"worst_cpu_mean_diff\": %.9g,\n", st.worst_mean_diff);
    fprintf(rf, "    \"worst_cpu_differing_pixels\": %d,\n", st.worst_differing_pixels);
    fprintf(rf, "    \"gpu_compared\": %d,\n", st.gpu_compared);
    fprintf(rf, "    \"worst_gpu_diff\": %.9g,\n", st.worst_gpu_diff);
    fprintf(rf, "    \"worst_gpu_diff_index\": %d,\n", st.worst_gpu_index);
    fprintf(rf, "    \"worst_gpu_mean_diff\": %.9g,\n", st.worst_gpu_mean_diff);
    fprintf(rf, "    \"worst_gpu_differing_pixels\": %d,\n", st.worst_gpu_differing_pixels);
    fprintf(rf, "    \"image_compared\": %d,\n", st.image_compared);
    fprintf(rf, "    \"worst_image_diff\": %.9g,\n", st.worst_image_diff);
    fprintf(rf, "    \"worst_image_diff_index\": %d,\n", st.worst_image_index);
    fprintf(rf, "    \"worst_image_mean_diff\": %.9g,\n", st.worst_image_mean_diff);
    fprintf(rf, "    \"worst_image_differing_pixels\": %d,\n",
            st.worst_image_differing_pixels);
    fprintf(rf, "    \"gpu_image_compared\": %d,\n", st.gpu_image_compared);
    fprintf(rf, "    \"worst_gpu_image_diff\": %.9g,\n", st.worst_gpu_image_diff);
    fprintf(rf, "    \"worst_gpu_image_diff_index\": %d,\n", st.worst_gpu_image_index);
    fprintf(rf, "    \"worst_gpu_image_mean_diff\": %.9g,\n",
            st.worst_gpu_image_mean_diff);
    fprintf(rf, "    \"worst_gpu_image_differing_pixels\": %d,\n",
            st.worst_gpu_image_differing_pixels);
    fprintf(rf, "    \"worst_dev_gap_classic\": %.9g,\n", st.worst_dev_before);
    fprintf(rf, "    \"worst_dev_gap_migrated\": %.9g,\n", st.worst_dev_after);
    fprintf(rf, "    \"dev_gap_widened\": %d,\n", st.dev_gap_widened);
    fprintf(rf, "    \"dev_gap_widened_own\": %d\n", st.dev_gap_widened_own);
    fputs("  }", rf);
  }

  g_hash_table_destroy(seen);

  printf("[verify]\n");
  printf("[verify] replayed          : %d  (%d distinct, %d exact repeats reused)\n",
         st.total, replayed_unique, st.total - replayed_unique);
  printf("[verify]   identical       : %d\n", st.identical);
  printf("[verify]   equivalent      : %d"
         "  (worst pixel below 1/255 and mean below 1/765, invisible)\n",
         st.equivalent);
  printf("[verify]   DIFFERENT       : %d\n", st.different);
  printf("[verify]   skipped         : %d\n", st.skipped);
  printf("[verify]   errors          : %d\n", st.error);
  printf("[verify]\n");
  // The distinction that decides what the run is worth: a comparison between
  // two uniform masks would have passed no matter what migration did.
  printf("[verify] of those actually compared:\n");
  printf("[verify]   live (mask varies)   : %d   -> %d identical, %d equivalent, %d different\n",
         st.live, st.live_identical, st.live_equivalent, st.live_different);
  printf("[verify]   inert (uniform mask) : %d   (proves nothing either way)\n",
         st.inert_before);
  if(st.worst_index >= 0)
    printf("[verify] worst CPU difference: %.9g at edit %d"
           " (mean %.9g over %d differing pixels)\n",
           st.worst_max_diff, st.worst_index,
           st.worst_mean_diff, st.worst_differing_pixels);

  if(st.image_compared && st.worst_image_index >= 0)
    printf("[verify] worst image difference: %.9g at edit %d"
           " (mean %.9g over %d differing pixels)\n",
           st.worst_image_diff, st.worst_image_index,
           st.worst_image_mean_diff, st.worst_image_differing_pixels);


  if(st.gpu_compared)
  {
    printf("[verify]\n");
    printf("[verify] GPU (OpenCL blend), %d edits replayed on both paths:\n",
           st.gpu_compared);
    printf("[verify]   migration on GPU, worst difference : %.9g at edit %d"
           " (mean %.9g over %d differing pixels)\n",
           st.worst_gpu_diff, st.worst_gpu_index,
           st.worst_gpu_mean_diff, st.worst_gpu_differing_pixels);
    // Reported side by side on purpose. The absolute CPU/GPU gap is not a
    // defect -- two separate implementations of the same blend never agree to
    // the last bit -- so the number that matters is whether migration made it
    // worse, not how large it is.
    printf("[verify]   CPU vs GPU gap, classic  : %.9g  (pre-existing baseline)\n",
           st.worst_dev_before);
    printf("[verify]   CPU vs GPU gap, migrated : %.9g\n", st.worst_dev_after);
    printf("[verify]   edits where migration widened that gap by >1/255 : %d\n",
           st.dev_gap_widened);
    if(st.dev_gap_widened)
      printf("[verify]     of those, still widened with mask post-processing off"
             " (migration's own) : %d\n", st.dev_gap_widened_own);
  }
  else
    printf("[verify] GPU: not replayed (no OpenCL device)\n");

#ifdef HAVE_OPENCL
  if(_verify_devid >= 0)
  {
    dt_opencl_unlock_device(_verify_devid);
    _verify_devid = -1;
  }
#endif

  return passed;
}

gboolean dt_masks_verify_harvest(const char *json_path, const char *report_path)
{
  return dt_masks_harvest_report(json_path, report_path, "verify",
                                 dt_masks_verify_harvest_section);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
