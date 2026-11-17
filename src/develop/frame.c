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

#include "develop/frame.h"
#include "common/atomic.h"
#include "common/darktable.h"
#include "common/iop_profile.h"

typedef struct _rendition_t
{
  dt_frame_format_t format;
  dt_color_encoding_t encoding;
  float *pixels;
  cairo_surface_t *surface;
} _rendition_t;

struct dt_frame_t
{
  dt_atomic_int refs;
  uint32_t id;
  uint32_t made_for;
  dt_frame_geometry_t geometry;
  dt_frame_format_t format;
  dt_color_encoding_t encoding;
  // the frame's own pixels: float RGBA, or the surface's for RGB24
  float *pixels;
  cairo_surface_t *surface;
  // renditions are made on demand, by any thread
  dt_pthread_mutex_t lock;
  GList *renditions;
};

static const cairo_user_data_key_t _data_key;

static void _free_data(void *data)
{
  dt_free_align(data);
}

// an RGB24 surface on aligned memory it owns: focus peaking reads its data
// with aligned loads
static cairo_surface_t *_surface_new(const int width,
                                     const int height)
{
  const int stride = cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, width);
  uint8_t *data = dt_alloc_aligned((size_t)stride * height);
  if(!data) return NULL;
  cairo_surface_t *surface =
    cairo_image_surface_create_for_data(data, CAIRO_FORMAT_RGB24, width, height, stride);
  if(cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS
     || cairo_surface_set_user_data(surface, &_data_key, data, _free_data)
        != CAIRO_STATUS_SUCCESS)
  {
    cairo_surface_destroy(surface);
    dt_free_align(data);
    return NULL;
  }
  return surface;
}

static cairo_surface_t *_pack(const float *const pixels,
                              const int width,
                              const int height)
{
  cairo_surface_t *surface = _surface_new(width, height);
  if(!surface) return NULL;
  uint8_t *const data = cairo_image_surface_get_data(surface);
  const size_t stride = cairo_image_surface_get_stride(surface);
  DT_OMP_FOR()
  for(int y = 0; y < height; y++)
    dt_display_to_8bit_pixels(pixels + (size_t)4 * width * y, data + stride * y, width, TRUE);
  cairo_surface_mark_dirty(surface);
  return surface;
}

dt_frame_t *dt_frame_new(const float *pixels,
                         const dt_frame_geometry_t *geometry,
                         const dt_frame_format_t format,
                         const dt_color_encoding_t *encoding,
                         const uint32_t made_for)
{
  const int width = geometry->width, height = geometry->height;
  if(!pixels || width <= 0 || height <= 0) return NULL;

  dt_frame_t *f = g_malloc0(sizeof(dt_frame_t));
  if(format == DT_FRAME_RGB24)
    f->surface = _pack(pixels, width, height);
  else if((f->pixels = dt_alloc_align_float((size_t)4 * width * height)))
    memcpy(f->pixels, pixels, sizeof(float) * 4 * width * height);
  if(!f->surface && !f->pixels)
  {
    g_free(f);
    return NULL;
  }

  static dt_atomic_int id;
  f->id = (uint32_t)dt_atomic_add_int(&id, 1) + 1;
  dt_atomic_set_int(&f->refs, 1);
  f->made_for = made_for;
  f->geometry = *geometry;
  f->format = format;
  f->encoding = *encoding;
  dt_pthread_mutex_init(&f->lock);
  return f;
}

dt_frame_t *dt_frame_ref(dt_frame_t *frame)
{
  if(frame) dt_atomic_add_int(&frame->refs, 1);
  return frame;
}

void dt_frame_unref(dt_frame_t *frame)
{
  if(!frame || dt_atomic_sub_int(&frame->refs, 1) != 1) return;

  for(GList *l = frame->renditions; l; l = g_list_next(l))
  {
    _rendition_t *r = l->data;
    if(r->surface) cairo_surface_destroy(r->surface);
    dt_free_align(r->pixels);
    g_free(r);
  }
  g_list_free(frame->renditions);
  if(frame->surface) cairo_surface_destroy(frame->surface);
  dt_free_align(frame->pixels);
  dt_pthread_mutex_destroy(&frame->lock);
  g_free(frame);
}

uint32_t dt_frame_id(const dt_frame_t *frame)
{
  return frame->id;
}

uint32_t dt_frame_made_for(const dt_frame_t *frame)
{
  return frame->made_for;
}

const dt_frame_geometry_t *dt_frame_geometry(const dt_frame_t *frame)
{
  return &frame->geometry;
}

dt_frame_format_t dt_frame_format(const dt_frame_t *frame)
{
  return frame->format;
}

const dt_color_encoding_t *dt_frame_encoding(const dt_frame_t *frame)
{
  return &frame->encoding;
}

const void *dt_frame_pixels(const dt_frame_t *frame)
{
  return frame->surface ? (const void *)cairo_image_surface_get_data(frame->surface)
                        : (const void *)frame->pixels;
}

// profile infos for conversions, built from the profile an encoding names.
// registry profiles live for the session, so the infos do too. a develop's
// list would not do: temporary develops free theirs with them
typedef struct _info_t
{
  dt_colorspaces_color_profile_type_t type;
  char filename[DT_IOP_COLOR_ICC_LEN];
  cmsHPROFILE profile;
  dt_iop_color_intent_t intent;
  dt_iop_order_iccprofile_info_t *info;
} _info_t;

static GMutex _infos_lock;
static GList *_infos;

static const dt_iop_order_iccprofile_info_t *_info(const dt_color_encoding_t *encoding,
                                                    const dt_iop_color_intent_t intent)
{
  if(encoding->type == DT_COLORSPACE_NONE || !encoding->profile) return NULL;

  g_mutex_lock(&_infos_lock);
  const _info_t *found = NULL;
  for(GList *l = _infos; l && !found; l = g_list_next(l))
  {
    const _info_t *i = l->data;
    if(i->type == encoding->type && i->profile == encoding->profile && i->intent == intent
       && !strcmp(i->filename, encoding->filename))
      found = i;
  }
  const dt_iop_order_iccprofile_info_t *info = found ? found->info : NULL;
  if(!found)
  {
    _info_t *i = g_malloc0(sizeof(_info_t));
    i->type = encoding->type;
    g_strlcpy(i->filename, encoding->filename, sizeof(i->filename));
    i->profile = encoding->profile;
    i->intent = intent;
    // a failure is kept too, so that it is not tried again
    i->info = dt_ioppr_new_profile_info(encoding->type, encoding->filename,
                                        encoding->profile, intent);
    _infos = g_list_prepend(_infos, i);
    info = i->info;
  }
  g_mutex_unlock(&_infos_lock);
  return info;
}

void dt_frame_cleanup(void)
{
  g_mutex_lock(&_infos_lock);
  for(GList *l = _infos; l; l = g_list_next(l))
  {
    _info_t *i = l->data;
    dt_ioppr_free_profile_info(i->info);
    g_free(i);
  }
  g_list_free(_infos);
  _infos = NULL;
  g_mutex_unlock(&_infos_lock);
}

// the frame's pixels as float RGBA: its own, or unpacked into *unpacked,
// which the caller frees
static const float *_float_pixels(const dt_frame_t *f,
                                  float **unpacked)
{
  *unpacked = NULL;
  if(f->pixels) return f->pixels;

  const int width = f->geometry.width, height = f->geometry.height;
  float *out = dt_alloc_align_float((size_t)4 * width * height);
  if(!out) return NULL;
  const uint8_t *const data = cairo_image_surface_get_data(f->surface);
  const size_t stride = cairo_image_surface_get_stride(f->surface);
  DT_OMP_FOR()
  for(int y = 0; y < height; y++)
  {
    const uint8_t *in = data + stride * y;
    float *px = out + (size_t)4 * width * y;
    for(int x = 0; x < width; x++, in += 4, px += 4)
    {
      // cairo's RGB24 is BGRX in memory
      px[0] = in[2] / 255.0f;
      px[1] = in[1] / 255.0f;
      px[2] = in[0] / 255.0f;
      px[3] = 1.0f;
    }
  }
  *unpacked = out;
  return out;
}

// cairo's RGB24 is BGRX in memory
static inline void _store(uint8_t *const out,
                          const float *const rgb)
{
  out[0] = dt_display_to_8bit(rgb[2]);
  out[1] = dt_display_to_8bit(rgb[1]);
  out[2] = dt_display_to_8bit(rgb[0]);
  out[3] = 0xff;
}

// decoded, converted on unclipped values and encoded, into out or data. an
// 8-bit target takes no negative values into its transfer function, and
// stores the encoded values clipped to [0, 1]: above reference white, PQ and
// HLG encode within 8 bits
static void _convert_matrix(const float *const in,
                            const int width,
                            const int height,
                            const dt_iop_order_iccprofile_info_t *const from,
                            const dt_iop_order_iccprofile_info_t *const to,
                            float *const out,
                            uint8_t *const data,
                            const size_t stride)
{
  dt_colormatrix_t product, m;
  dt_colormatrix_mul(product, to->matrix_out, from->matrix_in);
  transpose_3xSSE(product, m);
  DT_OMP_FOR(shared(m))
  for(int y = 0; y < height; y++)
  {
    const float *px = in + (size_t)4 * width * y;
    for(int x = 0; x < width; x++, px += 4)
    {
      dt_aligned_pixel_t linear, rgb, encoded;
      dt_ioppr_decode_rgb(px, linear, from);
      dt_apply_transposed_color_matrix(linear, m, rgb);
      if(data) for_three_channels(c) rgb[c] = rgb[c] >= 0.0f ? rgb[c] : 0.0f;
      dt_ioppr_encode_rgb(rgb, encoded, to);
      if(data)
        _store(data + stride * y + 4 * x, encoded);
      else
      {
        encoded[3] = px[3];
        copy_pixel(out + (size_t)4 * (width * y + x), encoded);
      }
    }
  }
}

// the frame in (format, encoding), from its own pixels. where no profile
// info converts between the two encodings the values stay as they are,
// as a display-encoded buffer was always shown
static _rendition_t *_render(const dt_frame_t *f,
                             const dt_frame_format_t format,
                             const dt_color_encoding_t *encoding)
{
  const int width = f->geometry.width, height = f->geometry.height;
  const size_t npixels = (size_t)width * height;

  float *unpacked;
  const float *in = _float_pixels(f, &unpacked);
  if(!in) return NULL;

  const dt_iop_order_iccprofile_info_t *from = NULL, *to = NULL;
  if(!dt_color_encoding_same_content(&f->encoding, encoding))
  {
    from = _info(&f->encoding, encoding->intent);
    to = _info(encoding, encoding->intent);
  }
  const gboolean convert = from && to;

  _rendition_t *r = g_malloc0(sizeof(_rendition_t));
  r->format = format;
  r->encoding = *encoding;

  if(convert
     && dt_is_valid_colormatrix(from->matrix_in[0][0])
     && dt_is_valid_colormatrix(to->matrix_out[0][0]))
  {
    if(format == DT_FRAME_RGB24 && (r->surface = _surface_new(width, height)))
    {
      _convert_matrix(in, width, height, from, to, NULL,
                      cairo_image_surface_get_data(r->surface),
                      cairo_image_surface_get_stride(r->surface));
      cairo_surface_mark_dirty(r->surface);
    }
    else if(format == DT_FRAME_FLOAT && (r->pixels = dt_alloc_align_float(4 * npixels)))
      _convert_matrix(in, width, height, from, to, r->pixels, NULL, 0);
  }
  else
  {
    // lcms converts a float copy. without a conversion, or with one lcms
    // refuses, the values stay as they are
    float *values = NULL;
    if(!convert && unpacked && format == DT_FRAME_FLOAT)
    {
      values = unpacked;
      unpacked = NULL;
    }
    else if((convert || format == DT_FRAME_FLOAT)
            && (values = dt_alloc_align_float(4 * npixels)))
    {
      memcpy(values, in, sizeof(float) * 4 * npixels);
      if(convert)
        dt_ioppr_transform_image_colorspace_rgb(in, values, width, height, from, to,
                                                "frame rendition");
    }

    if(format == DT_FRAME_FLOAT)
      r->pixels = values;
    else
    {
      if(values || !convert) r->surface = _pack(values ? values : in, width, height);
      dt_free_align(values);
    }
  }

  dt_free_align(unpacked);
  if(!r->surface && !r->pixels)
  {
    g_free(r);
    return NULL;
  }
  return r;
}

// the rendition in (format, encoding), made now if there is none yet
static const _rendition_t *_rendition(dt_frame_t *f,
                                      const dt_frame_format_t format,
                                      const dt_color_encoding_t *encoding)
{
  dt_pthread_mutex_lock(&f->lock);
  const _rendition_t *found = NULL;
  for(GList *l = f->renditions; l && !found; l = g_list_next(l))
  {
    const _rendition_t *r = l->data;
    if(r->format == format && r->encoding.intent == encoding->intent
       && dt_color_encoding_same_content(&r->encoding, encoding))
      found = r;
  }
  if(!found)
  {
    _rendition_t *r = _render(f, format, encoding);
    if(r) f->renditions = g_list_prepend(f->renditions, r);
    found = r;
  }
  dt_pthread_mutex_unlock(&f->lock);
  return found;
}

// a rendition in the frame's own format and profile is its pixels: the
// headroom says what a frame was rendered for, not what its values mean
static gboolean _own(const dt_frame_t *f,
                     const dt_frame_format_t format,
                     const dt_color_encoding_t *encoding)
{
  return format == f->format
    && (!encoding || dt_color_encoding_same_content(encoding, &f->encoding));
}

cairo_surface_t *dt_frame_surface(dt_frame_t *frame,
                                  const dt_color_encoding_t *encoding)
{
  if(!frame) return NULL;
  if(_own(frame, DT_FRAME_RGB24, encoding)) return frame->surface;
  const _rendition_t *r =
    _rendition(frame, DT_FRAME_RGB24, encoding ? encoding : &frame->encoding);
  return r ? r->surface : NULL;
}

const float *dt_frame_float(dt_frame_t *frame,
                            const dt_color_encoding_t *encoding)
{
  if(!frame) return NULL;
  if(_own(frame, DT_FRAME_FLOAT, encoding)) return frame->pixels;
  const _rendition_t *r =
    _rendition(frame, DT_FRAME_FLOAT, encoding ? encoding : &frame->encoding);
  return r ? r->pixels : NULL;
}

cairo_surface_t *dt_frame_device_surface(dt_frame_t *frame,
                                         const dt_colorspaces_color_profile_type_t window)
{
  dt_display_output_t output;
  dt_colorspaces_get_display_output(window, &output);
  return dt_frame_surface(frame, &output.device);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
