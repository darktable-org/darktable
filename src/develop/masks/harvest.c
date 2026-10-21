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

#include "develop/masks/harvest.h"

#include "common/darktable.h"
#include "common/exif.h"
#include "develop/blend.h"
#include "develop/masks.h"

#include <gio/gio.h>
#include <inttypes.h>
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

#define HARVEST_FORMAT_VERSION 1

// An XMP sidecar records no image dimensions. Mask geometry is normalized, so
// what a replay needs from them is an aspect ratio; 3:2 is the commonest
// photographic one, and whichever is chosen applies equally to the classic and
// migrated renders a verification compares.
#define HARVEST_XMP_NOMINAL_WIDTH 6000
#define HARVEST_XMP_NOMINAL_HEIGHT 4000

// ---------------------------------------------------------------------------
// JSON emission
//
// written by hand rather than with a library: every value in the output must
// be one someone chose to include, so a field appears only if its name is
// typed here. A serializer reflecting over the structs would emit the group
// name this format leaves out
// ---------------------------------------------------------------------------

typedef struct
{
  FILE *f;
  int indent;
  gboolean need_comma;
} json_t;

static void _j_indent(json_t *j)
{
  if(j->need_comma) fputs(",", j->f);
  fputs("\n", j->f);
  for(int i = 0; i < j->indent; i++) fputs("  ", j->f);
  j->need_comma = TRUE;
}

static void _j_open(json_t *j, const char *key, const char brace)
{
  _j_indent(j);
  if(key) fprintf(j->f, "\"%s\": %c", key, brace);
  else fprintf(j->f, "%c", brace);
  j->indent++;
  j->need_comma = FALSE;
}

static void _j_close(json_t *j, const char brace)
{
  j->indent--;
  fputs("\n", j->f);
  for(int i = 0; i < j->indent; i++) fputs("  ", j->f);
  fprintf(j->f, "%c", brace);
  j->need_comma = TRUE;
}

static void _j_int(json_t *j, const char *key, const int64_t v)
{
  _j_indent(j);
  fprintf(j->f, "\"%s\": %" PRId64, key, v);
}

/** %.9g round-trips a float exactly, so the verifier replays the same value
    the user's edit held rather than a rounded one. */
static void _j_float(json_t *j, const char *key, const float v)
{
  _j_indent(j);
  if(isfinite(v)) fprintf(j->f, "\"%s\": %.9g", key, (double)v);
  else fprintf(j->f, "\"%s\": null", key); // JSON has no NaN/Inf
}

/** Emit a string, escaped. Only ever used for values this file chose: module
    operation names, type names, and fixed format strings. Never for anything
    read out of a user-editable column. */
static void _j_str(json_t *j, const char *key, const char *v)
{
  _j_indent(j);
  fprintf(j->f, "\"%s\": \"", key);
  for(const char *p = v; p && *p; p++)
  {
    if(*p == '"' || *p == '\\') fprintf(j->f, "\\%c", *p);
    else if((unsigned char)*p < 0x20) fprintf(j->f, "\\u%04x", *p);
    else fputc(*p, j->f);
  }
  fputs("\"", j->f);
}

static void _j_float_array(json_t *j, const char *key, const float *v, const int n)
{
  _j_indent(j);
  fprintf(j->f, "\"%s\": [", key);
  for(int i = 0; i < n; i++)
    fprintf(j->f, "%s%.9g", i ? ", " : "", isfinite(v[i]) ? (double)v[i] : 0.0);
  fputs("]", j->f);
}

// ---------------------------------------------------------------------------
// decoding helpers
// ---------------------------------------------------------------------------

/** Human-readable list of the mask-mode bits, so a reader does not have to
    look up what 6 means. */
static void _emit_mask_mode(json_t *j, const uint32_t mode)
{
  _j_int(j, "mask_mode", mode);
  _j_indent(j);
  fprintf(j->f, "\"mask_mode_names\": [");
  const struct { uint32_t bit; const char *name; } bits[] = {
    { DEVELOP_MASK_ENABLED,     "enabled" },
    { DEVELOP_MASK_MASK,        "drawn" },
    { DEVELOP_MASK_CONDITIONAL, "parametric" },
    { DEVELOP_MASK_RASTER,      "raster" },
    { DEVELOP_MASK_FLEXI,       "flexi" },
  };
  gboolean first = TRUE;
  for(size_t i = 0; i < sizeof(bits) / sizeof(bits[0]); i++)
    if(mode & bits[i].bit)
    {
      fprintf(j->f, "%s\"%s\"", first ? "" : ", ", bits[i].name);
      first = FALSE;
    }
  fputs("]", j->f);
}

static const char *_cst_name(const int32_t cst)
{
  switch(cst)
  {
    case DEVELOP_BLEND_CS_NONE:        return "none";
    case DEVELOP_BLEND_CS_RAW:         return "raw";
    case DEVELOP_BLEND_CS_LAB:         return "Lab";
    case DEVELOP_BLEND_CS_RGB_DISPLAY: return "rgb_display";
    case DEVELOP_BLEND_CS_RGB_SCENE:   return "rgb_scene";
    default:                           return "unknown";
  }
}

static const char *_form_type_name(const int type)
{
  // the primitive bit, ignoring the clone/non-clone qualifiers
  if(type & DT_MASKS_CIRCLE)   return "circle";
  if(type & DT_MASKS_PATH)     return "path";
  if(type & DT_MASKS_GROUP)    return "group";
  if(type & DT_MASKS_GRADIENT) return "gradient";
  if(type & DT_MASKS_ELLIPSE)  return "ellipse";
  if(type & DT_MASKS_BRUSH)    return "brush";
  return "unknown";
}

static size_t _point_stride(const int type, const int version)
{
  // the reader's own stride, so the two cannot drift apart
  if(type & DT_MASKS_GROUP)
    return dt_masks_point_stride(type, version, sizeof(dt_masks_point_group_t));
  if(type & DT_MASKS_CIRCLE)   return sizeof(dt_masks_point_circle_t);
  if(type & DT_MASKS_ELLIPSE)  return sizeof(dt_masks_point_ellipse_t);
  if(type & DT_MASKS_PATH)     return sizeof(dt_masks_point_path_t);
  if(type & DT_MASKS_BRUSH)    return sizeof(dt_masks_point_brush_t);
  if(type & DT_MASKS_GRADIENT) return sizeof(dt_masks_point_gradient_t);
  return 0;
}

// ---------------------------------------------------------------------------
// per-form emission
// ---------------------------------------------------------------------------

/** Emit one decoded point. Every branch here lists the fields explicitly; in
    particular the group branch emits everything in dt_masks_point_group_t
    *except* `name`, which is user-typed free text. */
static void _emit_point(json_t *j,
                        const int type,
                        const int version,
                        const void *p)
{
  _j_open(j, NULL, '{');

  if(type & DT_MASKS_CIRCLE)
  {
    const dt_masks_point_circle_t *c = p;
    _j_float_array(j, "center", c->center, 2);
    _j_float(j, "radius", c->radius);
    _j_float(j, "border", c->border);
  }
  else if(type & DT_MASKS_ELLIPSE)
  {
    const dt_masks_point_ellipse_t *e = p;
    _j_float_array(j, "center", e->center, 2);
    _j_float_array(j, "radius", e->radius, 2);
    _j_float(j, "rotation", e->rotation);
    _j_float(j, "border", e->border);
    _j_int(j, "flags", e->flags);
  }
  else if(type & DT_MASKS_PATH)
  {
    const dt_masks_point_path_t *q = p;
    _j_float_array(j, "corner", q->corner, 2);
    _j_float_array(j, "ctrl1", q->ctrl1, 2);
    _j_float_array(j, "ctrl2", q->ctrl2, 2);
    _j_float_array(j, "border", q->border, 2);
    _j_int(j, "state", q->state);
  }
  else if(type & DT_MASKS_BRUSH)
  {
    const dt_masks_point_brush_t *b = p;
    _j_float_array(j, "corner", b->corner, 2);
    _j_float_array(j, "ctrl1", b->ctrl1, 2);
    _j_float_array(j, "ctrl2", b->ctrl2, 2);
    _j_float_array(j, "border", b->border, 2);
    _j_float(j, "density", b->density);
    _j_float(j, "hardness", b->hardness);
    _j_int(j, "state", b->state);
  }
  else if(type & DT_MASKS_GRADIENT)
  {
    const dt_masks_point_gradient_t *g = p;
    _j_float_array(j, "anchor", g->anchor, 2);
    _j_float(j, "rotation", g->rotation);
    _j_float(j, "compression", g->compression);
    _j_float(j, "steepness", g->steepness);
    _j_float(j, "curvature", g->curvature);
    _j_int(j, "state", g->state);
  }
  else if(type & DT_MASKS_GROUP)
  {
    const dt_masks_point_group_t *g = p;
    _j_int(j, "formid", g->formid);
    _j_int(j, "parentid", g->parentid);
    _j_int(j, "state", g->state);
    _j_float(j, "opacity", g->opacity);

    // `name` is deliberately absent: it is user-typed free text living inside
    // the points blob. See harvest.h.

    if(version >= 7)
    {
      _j_open(j, "refinement", '{');
      _j_int(j, "enabled", g->refinement.enabled);
      _j_float(j, "feathering_radius", g->refinement.feathering_radius);
      _j_int(j, "feathering_guide", g->refinement.feathering_guide);
      _j_float(j, "blur_radius", g->refinement.blur_radius);
      _j_float(j, "contrast", g->refinement.contrast);
      _j_float(j, "brightness", g->refinement.brightness);
      _j_float(j, "details", g->refinement.details);
      _j_close(j, '}');
      _j_float(j, "group_opacity", g->group_opacity);
    }
  }

  _j_close(j, '}');
}

// ---------------------------------------------------------------------------
// coverage tally
//
// the rare configurations a file holds, which synthetic fixtures otherwise
// stand in for: on stdout, so that a contributor learns the file is unusual,
// and in the JSON, so that a file can be sorted by them without parsing it
// all. Above all the inverted and inclusive combine modes, which take the
// migration's XOR and constant branches and which almost nobody sets
// ---------------------------------------------------------------------------

#define HARVEST_MAX_VERSION 24

typedef struct
{
  int combine_inv, combine_incl, combine_masks_pos;
  int case_drawn_only, case_parametric_only, case_drawn_and_parametric;
  int case_raster, case_already_flexi;
  int uses_feathering, uses_details, uses_blur, uses_contrast_or_brightness;
  int per_shape_refinement, custom_group_opacity;
  int blendop_version[HARVEST_MAX_VERSION];
  int masks_version[HARVEST_MAX_VERSION];
  int form_type_circle, form_type_ellipse, form_type_path;
  int form_type_brush, form_type_gradient, form_type_group;
} harvest_stats_t;

static void _tally_version(int *bucket, const int v)
{
  if(v >= 0 && v < HARVEST_MAX_VERSION) bucket[v]++;
}

// tally the rare configurations of one edit's blend params (see
// harvest_stats_t)
static void _tally_blend(harvest_stats_t *st,
                         const dt_develop_blend_params_t *blend,
                         const int blendop_version)
{
  if(blend->mask_combine & DEVELOP_COMBINE_INV) st->combine_inv++;
  if(blend->mask_combine & DEVELOP_COMBINE_INCL) st->combine_incl++;
  if(blend->mask_combine & DEVELOP_COMBINE_MASKS_POS) st->combine_masks_pos++;

  const gboolean has_drawn = (blend->mask_mode & DEVELOP_MASK_MASK) != 0;
  const gboolean has_param = (blend->mask_mode & DEVELOP_MASK_CONDITIONAL) != 0;
  if(blend->mask_mode & DEVELOP_MASK_FLEXI) st->case_already_flexi++;
  else if(blend->mask_mode & DEVELOP_MASK_RASTER) st->case_raster++;
  else if(has_drawn && has_param) st->case_drawn_and_parametric++;
  else if(has_drawn) st->case_drawn_only++;
  else if(has_param) st->case_parametric_only++;

  if(blend->feathering_radius != 0.0f) st->uses_feathering++;
  if(blend->details != 0.0f) st->uses_details++;
  if(blend->blur_radius != 0.0f) st->uses_blur++;
  if(blend->contrast != 0.0f || blend->brightness != 0.0f)
    st->uses_contrast_or_brightness++;

  _tally_version(st->blendop_version, blendop_version);
}

/** Emit a version histogram as an object keyed by version number, listing only
    the versions actually present. */
static void _emit_version_histogram(json_t *j, const char *key, const int *bucket)
{
  _j_open(j, key, '{');
  for(int v = 0; v < HARVEST_MAX_VERSION; v++)
    if(bucket[v])
    {
      char name[16];
      snprintf(name, sizeof(name), "%d", v);
      _j_int(j, name, bucket[v]);
    }
  _j_close(j, '}');
}

/** Emit one form. Shared by both harvest drivers -- the library one reads its
    columns from masks_history, the XMP one from a parsed sidecar, and the JSON
    they produce has to be identical field for field or a corpus's provenance
    would change what it means. */
static void _emit_one_form(json_t *j,
                           const int formid,
                           const int type,
                           const int version,
                           const void *pts,
                           const int pts_bytes,
                           const int pts_count,
                           const void *src,
                           const int src_bytes,
                           harvest_stats_t *st)
{
    _tally_version(st->masks_version, version);
    if(type & DT_MASKS_CIRCLE)   st->form_type_circle++;
    if(type & DT_MASKS_ELLIPSE)  st->form_type_ellipse++;
    if(type & DT_MASKS_PATH)     st->form_type_path++;
    if(type & DT_MASKS_BRUSH)    st->form_type_brush++;
    if(type & DT_MASKS_GRADIENT) st->form_type_gradient++;
    if(type & DT_MASKS_GROUP)    st->form_type_group++;

    _j_open(j, NULL, '{');
    _j_int(j, "formid", formid);
    _j_int(j, "type", type);
    _j_str(j, "type_name", _form_type_name(type));
    _j_int(j, "version", version);

    // `masks_history.name` is deliberately not selected: user-renameable.

    // source point, for clone/heal forms. Historic rows stored 2, 3 or 4
    // floats (the 4-float form carried an unused scale field); normalize to
    // the three the current code keeps, as dt_masks_read_masks_history does.
    float source[3] = { 0.0f, 0.0f, 0.0f };
    if(src && src_bytes >= (int)(sizeof(float) * 2))
      memcpy(source, src, MIN((size_t)src_bytes, sizeof(float) * 3));
    _j_float_array(j, "source", source, 3);

    const size_t stride = _point_stride(type, version);
    _j_int(j, "points_count", pts_count);

    if(version > dt_masks_version())
    {
      // a newer darktable's layout, which this build cannot know
      _j_int(j, "points_blob_bytes", pts_bytes);
      _j_str(j, "points_error", "masks version newer than this build");
    }
    else if(pts_count == 0 && pts_bytes == 0)
    {
      // a group whose members were all removed: empty, not undecodable
      _j_open(j, "points", '[');
      _j_close(j, ']');
    }
    else if(stride > 0 && pts && pts_count > 0
            && (size_t)pts_bytes >= stride * (size_t)pts_count)
    {
      _j_open(j, "points", '[');
      for(int i = 0; i < pts_count; i++)
      {
        // Zero-fill the tail exactly as the loader does, so a point from an
        // older masks version is reported with the same neutral defaults the
        // reader would give it rather than with uninitialized memory.
        char point[sizeof(dt_masks_point_group_t)];
        memset(point, 0, sizeof(point));
        memcpy(point, (const char *)pts + stride * (size_t)i, stride);
        _emit_point(j, type, version, point);

        if(type & DT_MASKS_GROUP)
        {
          const dt_masks_point_group_t *g = (const dt_masks_point_group_t *)point;
          if(version >= 7 && g->refinement.enabled) st->per_shape_refinement++;
          if(version >= 7 && g->group_opacity != 1.0f) st->custom_group_opacity++;
        }
      }
      _j_close(j, ']');
    }
    else
    {
      // A blob that does not match its declared stride and count is not
      // something to guess at: say so in the output rather than emitting
      // fabricated geometry.
      _j_int(j, "points_blob_bytes", pts_bytes);
      _j_str(j, "points_error", "blob size does not match stride * count");
    }

    _j_close(j, '}');
}

/** Read the forms attached to one history entry and emit them. */
static int _emit_forms(json_t *j,
                       sqlite3 *db,
                       const int imgid,
                       const int num,
                       harvest_stats_t *st)
{
  sqlite3_stmt *stmt;
  // every form visible at this history position, not only those written at
  // it: a form has a row where it was created or changed, and a later entry
  // that only refers to a mask writes none. As dt_masks_read_masks_history()
  // does, read every row up to `num` and let the latest per form win. With
  // MAX(num) selected, SQLite takes the other columns from the row holding
  // the maximum
  const char *q = "SELECT formid, form, version, points, points_count, source,"
                  "       MAX(num)"
                  " FROM masks_history WHERE imgid = ?1 AND num <= ?2"
                  " GROUP BY formid"
                  " ORDER BY formid";
  if(sqlite3_prepare_v2(db, q, -1, &stmt, NULL) != SQLITE_OK) return 0;
  sqlite3_bind_int(stmt, 1, imgid);
  sqlite3_bind_int(stmt, 2, num);

  int n = 0;
  _j_open(j, "forms", '[');
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const int formid = sqlite3_column_int(stmt, 0);
    const int type = sqlite3_column_int(stmt, 1);
    const int version = sqlite3_column_int(stmt, 2);
    const void *pts = sqlite3_column_blob(stmt, 3);
    const int pts_bytes = sqlite3_column_bytes(stmt, 3);
    const int pts_count = sqlite3_column_int(stmt, 4);
    const void *src = sqlite3_column_blob(stmt, 5);
    const int src_bytes = sqlite3_column_bytes(stmt, 5);
    _emit_one_form(j, formid, type, version, pts, pts_bytes, pts_count,
                   src, src_bytes, st);
    n++;
  }
  _j_close(j, ']');
  sqlite3_finalize(stmt);
  return n;
}

// ---------------------------------------------------------------------------
// blend params
// ---------------------------------------------------------------------------

static void _emit_blend_params(json_t *j, const dt_develop_blend_params_t *b)
{
  _j_open(j, "blend", '{');
  _emit_mask_mode(j, b->mask_mode);
  _j_int(j, "blend_cst", b->blend_cst);
  _j_str(j, "blend_cst_name", _cst_name(b->blend_cst));
  _j_int(j, "blend_mode", b->blend_mode);
  _j_float(j, "blend_parameter", b->blend_parameter);
  _j_float(j, "opacity", b->opacity);
  _j_int(j, "mask_combine", b->mask_combine);
  _j_int(j, "mask_id", b->mask_id);
  _j_int(j, "blendif", b->blendif);
  _j_float(j, "feathering_radius", b->feathering_radius);
  _j_int(j, "feathering_guide", b->feathering_guide);
  _j_float(j, "blur_radius", b->blur_radius);
  _j_float(j, "contrast", b->contrast);
  _j_float(j, "brightness", b->brightness);
  _j_float(j, "details", b->details);
  _j_int(j, "feather_version", b->feather_version);
  _j_float_array(j, "blendif_parameters", b->blendif_parameters,
                 4 * DEVELOP_BLENDIF_SIZE);
  _j_float_array(j, "blendif_boost_factors", b->blendif_boost_factors,
                 DEVELOP_BLENDIF_SIZE);
  // raster_mask_source is a module operation name (e.g. "retouch"), not user
  // text -- the set of possible values is fixed by the build.
  _j_str(j, "raster_mask_source", b->raster_mask_source);
  _j_int(j, "raster_mask_instance", b->raster_mask_instance);
  _j_int(j, "raster_mask_id", b->raster_mask_id);
  _j_int(j, "raster_mask_invert", b->raster_mask_invert ? 1 : 0);
  _j_close(j, '}');
}

// ---------------------------------------------------------------------------
// the harvest
// ---------------------------------------------------------------------------

/** write a gzip copy of `src_path` at `dst_path`. Both stay: the person who
    ran this is asked to read the JSON and to send the copy, which this data
    compresses to about a tenth, small enough to attach to a forum post.
    Streamed, as these files run to hundreds of MB. Failing is not fatal: the
    JSON is complete, and the caller reports it */
static gboolean _gzip_file(const char *src_path,
                           const char *dst_path,
                           GError **err)
{
  GFile *src = g_file_new_for_path(src_path);
  GFile *dst = g_file_new_for_path(dst_path);
  gboolean ok = FALSE;

  GFileInputStream *in = g_file_read(src, NULL, err);
  if(in)
  {
    GFileOutputStream *out =
      g_file_replace(dst, NULL, FALSE, G_FILE_CREATE_NONE, NULL, err);
    if(out)
    {
      GZlibCompressor *comp =
        g_zlib_compressor_new(G_ZLIB_COMPRESSOR_FORMAT_GZIP, -1);
      GOutputStream *zout =
        g_converter_output_stream_new(G_OUTPUT_STREAM(out), G_CONVERTER(comp));

      ok = g_output_stream_splice(zout, G_INPUT_STREAM(in),
                                  G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE
                                  | G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET,
                                  NULL, err) >= 0;

      g_object_unref(zout);
      g_object_unref(comp);
      g_object_unref(out);
    }
    g_object_unref(in);
  }

  g_object_unref(src);
  g_object_unref(dst);
  return ok;
}

/** Size of a file in bytes, or -1. */
static goffset _file_size(const char *path)
{
  GFile *f = g_file_new_for_path(path);
  GFileInfo *info = g_file_query_info(f, G_FILE_ATTRIBUTE_STANDARD_SIZE,
                                      G_FILE_QUERY_INFO_NONE, NULL, NULL);
  const goffset size = info ? g_file_info_get_size(info) : -1;
  if(info) g_object_unref(info);
  g_object_unref(f);
  return size;
}

/** the "coverage" section (see harvest_stats_t), the same for both harvests */
static void _emit_coverage(json_t *j, const harvest_stats_t *st)
{
  _j_open(j, "coverage", '{');
  _j_open(j, "mask_combine", '{');
  _j_int(j, "inverted", st->combine_inv);
  _j_int(j, "inclusive", st->combine_incl);
  _j_int(j, "drawn_mask_polarity", st->combine_masks_pos);
  _j_close(j, '}');
  _j_open(j, "cases", '{');
  _j_int(j, "drawn_only", st->case_drawn_only);
  _j_int(j, "parametric_only", st->case_parametric_only);
  _j_int(j, "drawn_and_parametric", st->case_drawn_and_parametric);
  _j_int(j, "raster", st->case_raster);
  _j_int(j, "already_flexi", st->case_already_flexi);
  _j_close(j, '}');
  _j_open(j, "refinements", '{');
  _j_int(j, "feathering", st->uses_feathering);
  _j_int(j, "details", st->uses_details);
  _j_int(j, "blur", st->uses_blur);
  _j_int(j, "contrast_or_brightness", st->uses_contrast_or_brightness);
  _j_int(j, "per_shape_refinement", st->per_shape_refinement);
  _j_int(j, "custom_group_opacity", st->custom_group_opacity);
  _j_close(j, '}');
  _j_open(j, "form_types", '{');
  _j_int(j, "circle", st->form_type_circle);
  _j_int(j, "ellipse", st->form_type_ellipse);
  _j_int(j, "path", st->form_type_path);
  _j_int(j, "brush", st->form_type_brush);
  _j_int(j, "gradient", st->form_type_gradient);
  _j_int(j, "group", st->form_type_group);
  _j_close(j, '}');
  _emit_version_histogram(j, "blendop_versions", st->blendop_version);
  _emit_version_histogram(j, "masks_versions", st->masks_version);
  _j_close(j, '}');

}

/** write a gzip copy beside the harvest and say so, so that the person sending
    it needs no other tool to compress it, notably on Windows */
static void _report_and_gzip(const char *output_path)
{
  gchar *gz_path = g_strconcat(output_path, ".gz", NULL);
  GError *gz_err = NULL;
  const gboolean gz_ok = _gzip_file(output_path, gz_path, &gz_err);
  if(gz_ok)
  {
    const goffset raw = _file_size(output_path);
    const goffset gz = _file_size(gz_path);
    if(raw > 0 && gz > 0)
    {
      gchar *raw_s = g_format_size(raw);
      gchar *gz_s = g_format_size(gz);
      printf("[harvest] wrote %s  (%s, from %s)\n", gz_path, gz_s, raw_s);
      g_free(raw_s);
      g_free(gz_s);
    }
    else
      printf("[harvest] wrote %s\n", gz_path);
  }
  else
  {
    printf("[harvest] could not write %s: %s\n"
           "[harvest] (not a problem -- the JSON above is complete;"
           " compress it yourself if you like)\n",
           gz_path, gz_err ? gz_err->message : "unknown error");
  }
  if(gz_ok)
    printf("[harvest] Read %s, then send %s -- they hold the same thing.\n",
           output_path, gz_path);
  g_free(gz_path);
  g_clear_error(&gz_err);
}

// ---------------------------------------------------------------------------
// harvesting from XMP sidecars
// ---------------------------------------------------------------------------

/* The sidecar is parsed here rather than by dt_exif_xmp_read(), which writes
   into darktable's database and so needs the whole startup the harvest avoids
   (see harvest.h). A sidecar is XML with darktable's hex or gz blobs in it,
   readable with no database, no exiv2 and no lock: GMarkupParser for the XML,
   dt_exif_xmp_decode() for the blobs.

   Only the attributes named below are read. A sidecar also holds the file
   name, GPS coordinates, timestamps and ratings: none is read, and the output
   has the fields the library harvest writes. */

static gint _int_cmp(gconstpointer a, gconstpointer b)
{
  return GPOINTER_TO_INT(a) - GPOINTER_TO_INT(b);
}

typedef struct _xmp_form_t
{
  int num, formid, type, version, points_count;
  unsigned char *points; int points_bytes;
  unsigned char *source; int source_bytes;
} _xmp_form_t;

typedef struct _xmp_hist_t
{
  int num, multi_priority, enabled, blendop_version;
  gchar *operation;
  unsigned char *blendop; int blendop_bytes;
} _xmp_hist_t;

typedef struct _xmp_ctx_t
{
  GList *hist;   // _xmp_hist_t*, in file order
  GList *forms;  // _xmp_form_t*, in file order
  gboolean in_history, in_masks;
} _xmp_ctx_t;

static const char *_attr(const gchar **names, const gchar **values, const char *want)
{
  for(int i = 0; names[i]; i++)
    if(!strcmp(names[i], want)) return values[i];
  return NULL;
}

static int _attr_int(const gchar **names, const gchar **values,
                     const char *want, const int dflt)
{
  const char *v = _attr(names, values, want);
  return v ? atoi(v) : dflt;
}

static unsigned char *_attr_blob(const gchar **names, const gchar **values,
                                 const char *want, int *out_len)
{
  *out_len = 0;
  const char *v = _attr(names, values, want);
  if(!v) return NULL;
  return dt_exif_xmp_decode(v, (int)strlen(v), out_len);
}

static void _xmp_start(GMarkupParseContext *ctx,
                       const gchar *element,
                       const gchar **names,
                       const gchar **values,
                       gpointer user,
                       GError **error)
{
  (void)ctx; (void)error;
  _xmp_ctx_t *x = user;

  if(!strcmp(element, "darktable:history")) { x->in_history = TRUE; return; }
  if(!strcmp(element, "darktable:masks_history")) { x->in_masks = TRUE; return; }
  if(strcmp(element, "rdf:li")) return;

  if(x->in_masks)
  {
    _xmp_form_t *f = calloc(1, sizeof(_xmp_form_t));
    if(!f) return;
    f->num = _attr_int(names, values, "darktable:mask_num", 0);
    f->formid = _attr_int(names, values, "darktable:mask_id", 0);
    f->type = _attr_int(names, values, "darktable:mask_type", 0);
    f->version = _attr_int(names, values, "darktable:mask_version", 0);
    f->points_count = _attr_int(names, values, "darktable:mask_nb", 0);
    // `darktable:mask_name` is deliberately not read: user-typed free text.
    f->points = _attr_blob(names, values, "darktable:mask_points", &f->points_bytes);
    f->source = _attr_blob(names, values, "darktable:mask_src", &f->source_bytes);
    x->forms = g_list_prepend(x->forms, f);
  }
  else if(x->in_history)
  {
    _xmp_hist_t *h = calloc(1, sizeof(_xmp_hist_t));
    if(!h) return;
    h->num = _attr_int(names, values, "darktable:num", 0);
    h->multi_priority = _attr_int(names, values, "darktable:multi_priority", 0);
    h->enabled = _attr_int(names, values, "darktable:enabled", 1);
    h->blendop_version = _attr_int(names, values, "darktable:blendop_version", 14);
    const char *op = _attr(names, values, "darktable:operation");
    h->operation = g_strdup(op ? op : "");
    // `darktable:params` (the module's own settings) and `darktable:multi_name`
    // are not read: the first is not needed to replay a mask, the second is
    // user-typed.
    h->blendop = _attr_blob(names, values, "darktable:blendop_params",
                            &h->blendop_bytes);
    x->hist = g_list_prepend(x->hist, h);
  }
}

static void _xmp_end(GMarkupParseContext *ctx,
                     const gchar *element,
                     gpointer user,
                     GError **error)
{
  (void)ctx; (void)error;
  _xmp_ctx_t *x = user;
  if(!strcmp(element, "darktable:history")) x->in_history = FALSE;
  else if(!strcmp(element, "darktable:masks_history")) x->in_masks = FALSE;
}

static void _xmp_free(_xmp_ctx_t *x)
{
  for(GList *l = x->hist; l; l = g_list_next(l))
  {
    _xmp_hist_t *h = l->data;
    g_free(h->operation);
    g_free(h->blendop);
    free(h);
  }
  for(GList *l = x->forms; l; l = g_list_next(l))
  {
    _xmp_form_t *f = l->data;
    g_free(f->points);
    g_free(f->source);
    free(f);
  }
  g_list_free(x->hist);
  g_list_free(x->forms);
}

/** Every form visible at history position `num`, emitted in formid order.

    The same rule the library harvest applies, for the same reason: a sidecar
    records a form under the history entry that changed it, so an entry that
    merely references an existing mask lists none of its own, and the latest
    row at or below `num` is the one that is live. */
static int _emit_xmp_forms(json_t *j,
                           GList *forms,
                           const int num,
                           harvest_stats_t *st)
{
  // gather the winning row per formid: highest mask_num <= num
  GHashTable *live = g_hash_table_new(g_direct_hash, g_direct_equal);
  for(GList *l = forms; l; l = g_list_next(l))
  {
    _xmp_form_t *f = l->data;
    if(f->num > num) continue;
    _xmp_form_t *cur = g_hash_table_lookup(live, GINT_TO_POINTER(f->formid));
    if(!cur || f->num >= cur->num)
      g_hash_table_insert(live, GINT_TO_POINTER(f->formid), f);
  }

  GList *ids = g_hash_table_get_keys(live);
  ids = g_list_sort(ids, (GCompareFunc)_int_cmp);

  int n = 0;
  _j_open(j, "forms", '[');
  for(GList *l = ids; l; l = g_list_next(l))
  {
    const _xmp_form_t *f = g_hash_table_lookup(live, l->data);
    _emit_one_form(j, f->formid, f->type, f->version,
                   f->points, f->points_bytes, f->points_count,
                   f->source, f->source_bytes, st);
    n++;
  }
  _j_close(j, ']');

  g_list_free(ids);
  g_hash_table_destroy(live);
  return n;
}

/** The image a sidecar belongs to, or NULL if it is not beside it.

    darktable's own convention is `<image file>.xmp` -- `IMG_1234.CR3.xmp` next
    to `IMG_1234.CR3` -- so stripping the suffix is the answer nearly always.
    The "short" variant some setups produce (`IMG_1234.xmp`) names no
    extension, so fall back to looking for a sibling with the same stem that is
    not itself a sidecar. */
static gchar *_image_for_sidecar(const char *xmp_path)
{
  const size_t len = strlen(xmp_path);
  if(len < 5) return NULL;

  gchar *stripped = g_strndup(xmp_path, len - 4);   // drop ".xmp"
  if(g_file_test(stripped, G_FILE_TEST_IS_REGULAR)) return stripped;

  /* a duplicate's sidecar has darktable's index before the extension:
     `TLK_0591_04.CR3.xmp` is a duplicate of `TLK_0591.CR3`, and belongs to
     that image. Without this its masks would fall back to a nominal canvas */
  gchar *dot = strrchr(stripped, '.');
  if(dot && dot != stripped)
  {
    const gchar *underscore = NULL;
    for(const gchar *c = dot - 1; c > stripped; c--)
    {
      if(g_ascii_isdigit(*c)) continue;
      if(*c == '_' && c != dot - 1) underscore = c;
      break;
    }
    if(underscore)
    {
      gchar *base = g_strndup(stripped, (gsize)(underscore - stripped));
      gchar *cand = g_strconcat(base, dot, NULL);
      g_free(base);
      if(g_file_test(cand, G_FILE_TEST_IS_REGULAR))
      {
        g_free(stripped);
        return cand;
      }
      g_free(cand);
    }
  }

  gchar *dir = g_path_get_dirname(stripped);
  gchar *stem = g_path_get_basename(stripped);
  g_free(stripped);

  // the short sidecar form (`IMG_1234_01.xmp`) names no extension, so the
  // duplicate index has to come off the stem before the sibling scan too
  gchar *us = strrchr(stem, '_');
  if(us && us != stem)
  {
    gboolean all_digits = us[1] != '\0';
    for(const gchar *c = us + 1; *c && all_digits; c++)
      if(!g_ascii_isdigit(*c)) all_digits = FALSE;
    if(all_digits) *us = '\0';
  }

  gchar *found = NULL;
  GDir *d = g_dir_open(dir, 0, NULL);
  if(d)
  {
    gchar *prefix = g_strconcat(stem, ".", NULL);
    const gchar *name;
    while(!found && (name = g_dir_read_name(d)))
    {
      if(!g_str_has_prefix(name, prefix)) continue;
      if(g_str_has_suffix(name, ".xmp") || g_str_has_suffix(name, ".XMP")) continue;
      gchar *cand = g_build_filename(dir, name, NULL);
      if(g_file_test(cand, G_FILE_TEST_IS_REGULAR)) found = cand;
      else g_free(cand);
    }
    g_free(prefix);
    g_dir_close(d);
  }
  g_free(dir);
  g_free(stem);
  return found;
}

/** Collect every .xmp under `dir`, recursively, in a stable order. */
static void _collect_xmps(const char *dir, GList **out, int depth)
{
  // sidecars sit beside the images; a deep tree is normal, a cyclic one is not
  if(depth > 32) return;
  GDir *d = g_dir_open(dir, 0, NULL);
  if(!d) return;

  GList *entries = NULL;
  const gchar *name;
  while((name = g_dir_read_name(d))) entries = g_list_prepend(entries, g_strdup(name));
  g_dir_close(d);
  entries = g_list_sort(entries, (GCompareFunc)g_strcmp0);

  for(GList *e = entries; e; e = g_list_next(e))
  {
    gchar *path = g_build_filename(dir, (const char *)e->data, NULL);
    if(g_file_test(path, G_FILE_TEST_IS_DIR))
    {
      _collect_xmps(path, out, depth + 1);
      g_free(path);
    }
    else if(g_str_has_suffix((const char *)e->data, ".xmp")
            || g_str_has_suffix((const char *)e->data, ".XMP"))
      *out = g_list_prepend(*out, path);
    else
      g_free(path);
  }
  g_list_free_full(entries, g_free);
}

gboolean dt_masks_harvest_xmp_dir(const char *dir, const char *output_path)
{
  if(!g_file_test(dir, G_FILE_TEST_IS_DIR))
  {
    fprintf(stderr, "[harvest] not a directory: %s\n", dir);
    return FALSE;
  }

  /* without darktable's log handler, exiv2 prints harmless complaints such as
     "Error: Directory Canon ... considered invalid" to stderr, which read like
     a failure. dt_exif_init() only registers namespaces and the handler, and
     touches nothing on disk */
  dt_exif_init();

  GList *files = NULL;
  _collect_xmps(dir, &files, 0);
  files = g_list_reverse(files);
  const guint nfiles = g_list_length(files);
  printf("[harvest] %u XMP sidecars under %s\n", nfiles, dir);
  if(nfiles == 0)
  {
    g_list_free_full(files, g_free);
    fprintf(stderr, "[harvest] nothing to harvest.\n");
    return FALSE;
  }

  FILE *f = g_fopen(output_path, "wb");
  if(!f)
  {
    fprintf(stderr, "[harvest] cannot write output file: %s\n", output_path);
    g_list_free_full(files, g_free);
    return FALSE;
  }

  json_t j = { .f = f, .indent = 0, .need_comma = FALSE };
  fputs("{", f);
  j.indent = 1;
  j.need_comma = FALSE;

  _j_str(&j, "format", "darktable-mask-harvest");
  _j_int(&j, "format_version", HARVEST_FORMAT_VERSION);
  _j_str(&j, "source_kind", "xmp");
  _j_str(&j, "darktable_version", darktable_package_version);
  _j_int(&j, "current_blend_version", DEVELOP_BLEND_VERSION);
  _j_int(&j, "current_masks_version", dt_masks_version());
  _j_str(&j, "contents",
         "Mask configurations only, read from XMP sidecars. No file names, no "
         "folder names, no shape or group names, no module instance names, no "
         "image content, no thumbnails, no timestamps, no EXIF, no GPS. Image "
         "identity is reduced to a sequential index.");
  _j_str(&j, "image_dimensions",
         "read from each sidecar's own image file, metadata only, nothing else "
         "about it opened. Where that file is missing a nominal 3:2 canvas is "
         "used instead and the edit is marked dimensions_known: 0; mask geometry "
         "is stored normalized, so that only sets the aspect a replay draws on, "
         "identically for the classic and migrated renders it compares.");

  _j_open(&j, "edits", '[');

  harvest_stats_t st;
  memset(&st, 0, sizeof(st));

  int harvested = 0, scanned = 0, skipped_no_mask = 0, skipped_size = 0;
  int total_forms = 0, image_index = -1, images_with_masks = 0, files_read = 0;
  int dims_from_image = 0, dims_nominal = 0;

  for(GList *fl = files; fl; fl = g_list_next(fl))
  {
    const char *path = fl->data;
    gchar *contents = NULL;
    gsize len = 0;
    if(!g_file_get_contents(path, &contents, &len, NULL)) continue;

    _xmp_ctx_t x;
    memset(&x, 0, sizeof(x));
    static const GMarkupParser parser =
      { _xmp_start, _xmp_end, NULL, NULL, NULL };
    GMarkupParseContext *mctx =
      g_markup_parse_context_new(&parser, 0, &x, NULL);
    const gboolean parsed =
      g_markup_parse_context_parse(mctx, contents, (gssize)len, NULL)
      && g_markup_parse_context_end_parse(mctx, NULL);
    g_markup_parse_context_free(mctx);
    g_free(contents);

    if(!parsed) { _xmp_free(&x); continue; }
    files_read++;

    /* The sidecar records no dimensions, but it sits beside the image that
       does. Only the pixel size is read from it (see dt_exif_get_dimensions);
       nothing else about the file is opened, and nothing but width and height
       reaches the output. */
    int img_w = HARVEST_XMP_NOMINAL_WIDTH, img_h = HARVEST_XMP_NOMINAL_HEIGHT;
    gboolean dims_known = FALSE;
    gchar *image_path = _image_for_sidecar(path);
    if(image_path)
    {
      dims_known = dt_exif_get_dimensions(image_path, &img_w, &img_h);
      g_free(image_path);
    }
    if(dims_known) dims_from_image++;
    else dims_nominal++;

    // both lists were built by prepending, so put them back in file order
    x.hist = g_list_reverse(x.hist);
    x.forms = g_list_reverse(x.forms);

    image_index++;
    gboolean counted_this_image = FALSE;

    for(GList *hl = x.hist; hl; hl = g_list_next(hl))
    {
      const _xmp_hist_t *h = hl->data;
      scanned++;

      // same rule as the library harvest: only blobs this build's struct can
      // decode field by field, counted rather than silently dropped
      if(!h->blendop || h->blendop_bytes != (int)sizeof(dt_develop_blend_params_t))
      {
        if(h->blendop) skipped_size++;
        continue;
      }

      dt_develop_blend_params_t blend;
      memcpy(&blend, h->blendop, sizeof(blend));

      const uint32_t interesting = DEVELOP_MASK_MASK | DEVELOP_MASK_CONDITIONAL
                                 | DEVELOP_MASK_RASTER | DEVELOP_MASK_FLEXI;
      if(!(blend.mask_mode & interesting)) { skipped_no_mask++; continue; }

      if(!counted_this_image) { images_with_masks++; counted_this_image = TRUE; }

      _j_open(&j, NULL, '{');
      _j_int(&j, "index", harvested);
      _j_int(&j, "image_index", image_index);
      _j_open(&j, "image", '{');
      // See "image_dimensions" above: a sidecar does not carry them.
      _j_int(&j, "width", img_w);
      _j_int(&j, "height", img_h);
      _j_int(&j, "dimensions_known", dims_known ? 1 : 0);
      _j_close(&j, '}');

      _j_str(&j, "operation", h->operation);
      _j_int(&j, "multi_priority", h->multi_priority);
      _j_int(&j, "enabled", h->enabled);
      _j_int(&j, "blendop_version", h->blendop_version);

      _tally_blend(&st, &blend, h->blendop_version);

      _emit_blend_params(&j, &blend);
      total_forms += _emit_xmp_forms(&j, x.forms, h->num, &st);

      _j_close(&j, '}');
      harvested++;
    }

    _xmp_free(&x);

    if(files_read % 250 == 0)
      printf("[harvest]   %d/%u sidecars ...\n", files_read, nfiles);
  }
  g_list_free_full(files, g_free);

  _j_close(&j, ']');

  _j_open(&j, "summary", '{');
  _j_int(&j, "sidecars_found", nfiles);
  _j_int(&j, "sidecars_parsed", files_read);
  _j_int(&j, "history_entries_scanned", scanned);
  _j_int(&j, "edits_harvested", harvested);
  _j_int(&j, "images_with_masks", images_with_masks);
  _j_int(&j, "forms_harvested", total_forms);
  _j_int(&j, "skipped_no_mask", skipped_no_mask);
  _j_int(&j, "skipped_unsupported_blendop_size", skipped_size);
  _j_int(&j, "dimensions_from_image", dims_from_image);
  _j_int(&j, "dimensions_nominal", dims_nominal);
  _j_close(&j, '}');

  _emit_coverage(&j, &st);

  fputs("\n}\n", f);
  fclose(f);

  dt_exif_cleanup();

  printf("[harvest] %d edits from %d sidecars (%d with masks), %d forms\n",
         harvested, files_read, images_with_masks, total_forms);
  printf("[harvest]   image size read from the file : %d\n", dims_from_image);
  if(dims_nominal)
    printf("[harvest]   image missing, nominal 3:2 used: %d\n", dims_nominal);
  printf("[harvest] written to %s\n", output_path);
  _report_and_gzip(output_path);
  return harvested > 0;
}

gboolean dt_masks_harvest_library(const char *library_path,
                                  const char *output_path)
{
  sqlite3 *db = NULL;

  // read-only twice over: the URI says mode=ro and the flags
  // SQLITE_OPEN_READONLY, so that changing one alone cannot drop the promise
  gchar *uri = g_strdup_printf("file:%s?mode=ro", library_path);
  const int rc = sqlite3_open_v2(uri, &db,
                                 SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, NULL);
  g_free(uri);

  if(rc != SQLITE_OK)
  {
    fprintf(stderr, "[harvest] cannot open library read-only: %s\n  %s\n",
            library_path, db ? sqlite3_errmsg(db) : "(no handle)");
    if(db) sqlite3_close(db);
    return FALSE;
  }

  FILE *f = g_fopen(output_path, "wb");
  if(!f)
  {
    fprintf(stderr, "[harvest] cannot write output file: %s\n", output_path);
    sqlite3_close(db);
    return FALSE;
  }

  json_t j = { .f = f, .indent = 0, .need_comma = FALSE };

  fputs("{", f);
  j.indent = 1;
  j.need_comma = FALSE;

  _j_str(&j, "format", "darktable-mask-harvest");
  _j_int(&j, "format_version", HARVEST_FORMAT_VERSION);
  _j_str(&j, "darktable_version", darktable_package_version);
  _j_int(&j, "current_blend_version", DEVELOP_BLEND_VERSION);
  _j_int(&j, "current_masks_version", dt_masks_version());
  _j_str(&j, "contents",
         "Mask configurations only. No file names, no folder or film-roll "
         "names, no shape or group names, no module instance names, no image "
         "content, no thumbnails, no timestamps, no EXIF. Image identity is "
         "reduced to pixel dimensions and a sequential index.");

  _j_open(&j, "edits", '[');

  sqlite3_stmt *stmt;
  // Only what is needed, and nothing free-text: no filename, no multi_name.
  const char *query =
    "SELECT h.imgid, h.num, h.operation, h.blendop_params, h.blendop_version,"
    "       h.multi_priority, h.enabled, i.width, i.height"
    " FROM history h JOIN images i ON i.id = h.imgid"
    " WHERE h.blendop_params IS NOT NULL"
    " ORDER BY h.imgid, h.num";

  if(sqlite3_prepare_v2(db, query, -1, &stmt, NULL) != SQLITE_OK)
  {
    fprintf(stderr, "[harvest] query failed: %s\n", sqlite3_errmsg(db));
    fclose(f);
    sqlite3_close(db);
    return FALSE;
  }

  harvest_stats_t st;
  memset(&st, 0, sizeof(st));

  int scanned = 0, harvested = 0, skipped_no_mask = 0, skipped_size = 0;
  int total_forms = 0, last_imgid = -1, image_index = -1, images_with_masks = 0;
  gboolean counted_this_image = FALSE;

  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    scanned++;
    const int imgid = sqlite3_column_int(stmt, 0);
    const int num = sqlite3_column_int(stmt, 1);
    const char *operation = (const char *)sqlite3_column_text(stmt, 2);
    const void *bp = sqlite3_column_blob(stmt, 3);
    const int bp_bytes = sqlite3_column_bytes(stmt, 3);
    const int bp_version = sqlite3_column_int(stmt, 4);
    const int multi_priority = sqlite3_column_int(stmt, 5);
    const int enabled = sqlite3_column_int(stmt, 6);
    const int width = sqlite3_column_int(stmt, 7);
    const int height = sqlite3_column_int(stmt, 8);

    if(imgid != last_imgid)
    {
      last_imgid = imgid;
      image_index++;
      counted_this_image = FALSE;
    }

    // only a blob of this build's size can be decoded field by field: an older
    // one needs dt_develop_blend_legacy_params(), which needs a module. They
    // are counted and reported, so that a library of them does not look like
    // one without masks
    if(!bp || bp_bytes != (int)sizeof(dt_develop_blend_params_t))
    {
      skipped_size++;
      continue;
    }

    dt_develop_blend_params_t blend;
    memcpy(&blend, bp, sizeof(blend));

    // without a mask there is nothing to migrate or verify
    const uint32_t interesting = DEVELOP_MASK_MASK | DEVELOP_MASK_CONDITIONAL
                               | DEVELOP_MASK_RASTER | DEVELOP_MASK_FLEXI;
    if(!(blend.mask_mode & interesting))
    {
      skipped_no_mask++;
      continue;
    }

    if(!counted_this_image)
    {
      images_with_masks++;
      counted_this_image = TRUE;
    }

    _j_open(&j, NULL, '{');
    _j_int(&j, "index", harvested);
    // A sequential index, not the database id: nothing here can be correlated
    // with anything outside this file.
    _j_int(&j, "image_index", image_index);
    _j_open(&j, "image", '{');
    _j_int(&j, "width", width);
    _j_int(&j, "height", height);
    _j_close(&j, '}');

    _j_str(&j, "operation", operation ? operation : "");
    _j_int(&j, "multi_priority", multi_priority);
    _j_int(&j, "enabled", enabled);
    _j_int(&j, "blendop_version", bp_version);

    _tally_blend(&st, &blend, bp_version);

    _emit_blend_params(&j, &blend);
    total_forms += _emit_forms(&j, db, imgid, num, &st);

    _j_close(&j, '}');
    harvested++;
  }
  sqlite3_finalize(stmt);

  _j_close(&j, ']');

  _j_open(&j, "summary", '{');
  _j_int(&j, "history_entries_scanned", scanned);
  _j_int(&j, "edits_harvested", harvested);
  _j_int(&j, "images_with_masks", images_with_masks);
  _j_int(&j, "forms_harvested", total_forms);
  _j_int(&j, "skipped_no_mask", skipped_no_mask);
  _j_int(&j, "skipped_unsupported_blendop_size", skipped_size);
  _j_close(&j, '}');

  _emit_coverage(&j, &st);

  j.indent = 0;
  fputs("\n}\n", f);
  fclose(f);
  sqlite3_close(db);

  printf("[harvest] read-only scan of %s\n", library_path);
  printf("[harvest]   history entries scanned : %d\n", scanned);
  printf("[harvest]   edits with masks        : %d\n", harvested);
  printf("[harvest]   images involved         : %d\n", images_with_masks);
  printf("[harvest]   mask forms              : %d\n", total_forms);
  if(skipped_size)
    printf("[harvest]   skipped (old blendop)   : %d\n", skipped_size);
  printf("[harvest] wrote %s\n", output_path);

  _report_and_gzip(output_path);

  // say when a library holds configurations real edits rarely have, so that
  // the person who ran this knows the file is worth sending
  if(st.combine_inv || st.combine_incl)
  {
    printf("[harvest]\n[harvest] This library contains rarely-used mask combine modes:\n");
    if(st.combine_inv)
      printf("[harvest]   inverted mask combine : %d edit(s)\n", st.combine_inv);
    if(st.combine_incl)
      printf("[harvest]   inclusive mask combine: %d edit(s)\n", st.combine_incl);
    printf("[harvest] These exercise migration paths that almost no real edit "
           "reaches,\n[harvest] so this file is especially useful to us.\n");
  }
  printf("[harvest]\n");
  printf("[harvest] The output is plain JSON containing only numbers and "
         "module names.\n"
         "[harvest] It has no file names, folder names, shape or group names, "
         "or image content.\n"
         "[harvest] Please open it and check before sharing it.\n");

  return TRUE;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
