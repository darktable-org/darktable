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

/*
 * the Wayland presenter backend: the image layer is a subsurface tagged
 * with color-management-v1 as linear BT.2020, carrying the image in half
 * floats with its HDR headroom, and the compositor does the one conversion
 * to the screen. the foreground layer above it is an untagged 8-bit
 * subsurface, which the compositor treats like GTK's own untagged surface.
 * see dev-doc/display_presentation.md.
 */

// for memfd_create()
#define _GNU_SOURCE

#include "gui/presenter_layers.h"
#include "common/darktable.h"
#include "common/math.h"

#include "color-management-v1-client-protocol.h"

#include <gdk/gdkwayland.h>
#include <sys/mman.h>
#include <unistd.h>

// the luminance darktable puts 1.0 at
#define REFERENCE_NITS 203

typedef struct _buffer_t
{
  struct wl_buffer *buffer;
  void *data;
  size_t size;
  int width, height;
  gboolean busy;
} _buffer_t;

typedef struct _layer_t
{
  struct wl_surface *surface;
  struct wl_subsurface *subsurface;
  _buffer_t *buffers[2];
} _layer_t;

typedef struct _backend_t _backend_t;

typedef struct _widget_t
{
  _layer_t image, foreground;
  struct wp_color_management_surface_v1 *color;
} _widget_t;

struct _backend_t
{
  dt_presenter_backend_t backend;
  dt_presenter_window_t *window;
  GtkWidget *toplevel;
  gulong unmap_handler;
  GdkFrameClock *clock;
  gulong paint_handler;
  struct wl_display *display;
  struct wl_compositor *compositor;
  struct wl_subcompositor *subcompositor;
  struct wl_shm *shm;
  struct wp_color_manager_v1 *manager;

  // what the compositor supports
  gboolean done, parametric, luminances, ext_linear, bt2020, relative, half_float;

  // GDK's surface of the toplevel, which the layers are children of
  struct wl_surface *parent;
  struct wp_color_management_surface_feedback_v1 *feedback;

  // the compositor's preferred description and its information, while read
  struct wp_image_description_v1 *preferred;
  struct wp_image_description_info_v1 *info;
  uint32_t reference_lum, max_lum, target_max_lum;
  gboolean preferred_done, preferred_failed;

  // ours: in use, and created, waiting for ready
  struct wp_image_description_v1 *description, *pending;
  uint32_t description_max, pending_max;
  gboolean pending_failed;

  GList *widgets;
};

static uint32_t _intent(const _backend_t *b)
{
  // perceptual is the one every compositor has to support
  return b->relative ? WP_COLOR_MANAGER_V1_RENDER_INTENT_RELATIVE
                     : WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL;
}

static float _headroom(const uint32_t max_lum)
{
  return max_lum ? (float)max_lum / REFERENCE_NITS : 1.0f;
}

// v in [0, 1] as an IEEE half float, rounded to nearest even
static inline uint16_t _half(const float v)
{
  // below the smallest normal half: a multiple of 2^-24
  if(v < 6.103515625e-05f) return (uint16_t)lrintf(v * 16777216.0f);

  union { float f; uint32_t u; } x = { .f = v };
  const uint32_t u = x.u + 0x0fff + ((x.u >> 13) & 1);
  return (uint16_t)((u >> 13) - ((127 - 15) << 10));
}

/* buffers */

static void _buffer_destroy(_buffer_t *buffer)
{
  wl_buffer_destroy(buffer->buffer);
  munmap(buffer->data, buffer->size);
  g_free(buffer);
}

static void _buffer_release(void *data,
                            struct wl_buffer *wl_buffer)
{
  _buffer_t *buffer = data;
  buffer->busy = FALSE;
}

static const struct wl_buffer_listener _buffer_listener = { .release = _buffer_release };

static _buffer_t *_buffer_new(_backend_t *b,
                              const int width,
                              const int height,
                              const int stride,
                              const uint32_t format)
{
  const size_t size = (size_t)stride * height;
  const int fd = memfd_create("darktable-presenter", MFD_CLOEXEC);
  if(fd < 0) return NULL;
  if(ftruncate(fd, size) < 0)
  {
    close(fd);
    return NULL;
  }
  void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if(data == MAP_FAILED)
  {
    close(fd);
    return NULL;
  }

  struct wl_shm_pool *pool = wl_shm_create_pool(b->shm, fd, size);
  _buffer_t *buffer = g_malloc0(sizeof(_buffer_t));
  buffer->buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride, format);
  buffer->data = data;
  buffer->size = size;
  buffer->width = width;
  buffer->height = height;
  wl_buffer_add_listener(buffer->buffer, &_buffer_listener, buffer);
  wl_shm_pool_destroy(pool);
  close(fd);
  return buffer;
}

// a buffer the compositor does not hold, or NULL if it holds both
static _buffer_t *_buffer_get(_backend_t *b,
                              _layer_t *layer,
                              const int width,
                              const int height,
                              const int stride,
                              const uint32_t format)
{
  for(int k = 0; k < 2; k++)
  {
    _buffer_t *buffer = layer->buffers[k];
    // even while the compositor holds it: a wl_buffer may go before its
    // release as long as its storage is not written again, and each one
    // has a memfd of its own
    if(buffer && (buffer->width != width || buffer->height != height))
    {
      _buffer_destroy(buffer);
      layer->buffers[k] = NULL;
    }
  }
  for(int k = 0; k < 2; k++)
    if(layer->buffers[k] && !layer->buffers[k]->busy) return layer->buffers[k];
  for(int k = 0; k < 2; k++)
    if(!layer->buffers[k])
      return layer->buffers[k] = _buffer_new(b, width, height, stride, format);
  return NULL;
}

/* layers */

static void _layer_init(_backend_t *b,
                        _layer_t *layer)
{
  layer->surface = wl_compositor_create_surface(b->compositor);
  layer->subsurface = wl_subcompositor_get_subsurface(b->subcompositor,
                                                      layer->surface, b->parent);
  // the layers only show; GTK's surface below keeps getting the input
  struct wl_region *region = wl_compositor_create_region(b->compositor);
  wl_surface_set_input_region(layer->surface, region);
  wl_region_destroy(region);
  // no opaque region on either layer: the compositor must never consider
  // GTK's surface below occluded, or it may stop the frame callbacks that
  // drive GTK's frame clock
}

static void _layer_free(_layer_t *layer)
{
  if(layer->subsurface) wl_subsurface_destroy(layer->subsurface);
  if(layer->surface) wl_surface_destroy(layer->surface);
  for(int k = 0; k < 2; k++)
    if(layer->buffers[k]) _buffer_destroy(layer->buffers[k]);
  memset(layer, 0, sizeof(_layer_t));
}

static void _layer_commit(_layer_t *layer,
                          _buffer_t *buffer,
                          const int scale)
{
  wl_surface_attach(layer->surface, buffer->buffer, 0, 0);
  wl_surface_set_buffer_scale(layer->surface, scale);
  wl_surface_damage_buffer(layer->surface, 0, 0, buffer->width, buffer->height);
  // the layers are synchronized subsurfaces: their state applies with the
  // commit GDK makes of its own surface after the frame's paint phase,
  // which is what keeps them in step with what GTK draws
  wl_surface_commit(layer->surface);
  buffer->busy = TRUE;
}

static void _widget_layers_init(_backend_t *b,
                                _widget_t *w)
{
  _layer_init(b, &w->image);
  _layer_init(b, &w->foreground);
  // a new subsurface goes on top of its siblings, which would cover GTK's
  // popovers, subsurfaces of GDK's surface too: keep the layers right above
  // GDK's surface. applied with GDK's next commit, like the position
  wl_subsurface_place_above(w->image.subsurface, b->parent);
  wl_subsurface_place_above(w->foreground.subsurface, w->image.surface);
  w->color = wp_color_manager_v1_get_surface(b->manager, w->image.surface);
  if(b->description)
    wp_color_management_surface_v1_set_image_description(w->color, b->description,
                                                         _intent(b));
}

static void _widget_layers_free(_widget_t *w)
{
  if(w->color) wp_color_management_surface_v1_destroy(w->color);
  w->color = NULL;
  _layer_free(&w->foreground);
  _layer_free(&w->image);
}

/* image descriptions */

static void _description_failed(void *data,
                                struct wp_image_description_v1 *description,
                                uint32_t cause,
                                const char *message)
{
  _backend_t *b = data;
  dt_print(DT_DEBUG_ALWAYS, "[presenter wayland] image description failed: %s", message);
  if(description == b->preferred)
    b->preferred_failed = TRUE;
  else if(description == b->pending)
    b->pending_failed = TRUE;
}

static void _info_done(void *data,
                       struct wp_image_description_info_v1 *info);

static void _info_icc_file(void *data,
                           struct wp_image_description_info_v1 *info,
                           int32_t icc,
                           uint32_t icc_size)
{
  close(icc);
}

static void _info_primaries(void *data,
                            struct wp_image_description_info_v1 *info,
                            int32_t r_x,
                            int32_t r_y,
                            int32_t g_x,
                            int32_t g_y,
                            int32_t b_x,
                            int32_t b_y,
                            int32_t w_x,
                            int32_t w_y)
{
}

static void _info_primaries_named(void *data,
                                  struct wp_image_description_info_v1 *info,
                                  uint32_t primaries)
{
}

static void _info_tf_power(void *data,
                           struct wp_image_description_info_v1 *info,
                           uint32_t eexp)
{
}

static void _info_tf_named(void *data,
                           struct wp_image_description_info_v1 *info,
                           uint32_t tf)
{
}

static void _info_luminances(void *data,
                             struct wp_image_description_info_v1 *info,
                             uint32_t min_lum,
                             uint32_t max_lum,
                             uint32_t reference_lum)
{
  _backend_t *b = data;
  b->max_lum = max_lum;
  b->reference_lum = reference_lum;
}

static void _info_target_luminance(void *data,
                                   struct wp_image_description_info_v1 *info,
                                   uint32_t min_lum,
                                   uint32_t max_lum)
{
  _backend_t *b = data;
  b->target_max_lum = max_lum;
}

static void _info_target_max_cll(void *data,
                                 struct wp_image_description_info_v1 *info,
                                 uint32_t max_cll)
{
}

static void _info_target_max_fall(void *data,
                                  struct wp_image_description_info_v1 *info,
                                  uint32_t max_fall)
{
}

static const struct wp_image_description_info_v1_listener _info_listener = {
  .done = _info_done,
  .icc_file = _info_icc_file,
  .primaries = _info_primaries,
  .primaries_named = _info_primaries_named,
  .tf_power = _info_tf_power,
  .tf_named = _info_tf_named,
  .luminances = _info_luminances,
  .target_primaries = _info_primaries,
  .target_luminance = _info_target_luminance,
  .target_max_cll = _info_target_max_cll,
  .target_max_fall = _info_target_max_fall,
};

static void _description_ready(_backend_t *b,
                               struct wp_image_description_v1 *description);

static void _ready(void *data,
                   struct wp_image_description_v1 *description,
                   uint32_t identity)
{
  _description_ready(data, description);
}

static const struct wp_image_description_v1_listener _description_listener = {
  .failed = _description_failed,
  .ready = _ready,
};

static void _create_description(_backend_t *b,
                                const uint32_t max_lum)
{
  if(b->pending) wp_image_description_v1_destroy(b->pending);

  struct wp_image_description_creator_params_v1 *creator =
    wp_color_manager_v1_create_parametric_creator(b->manager);
  wp_image_description_creator_params_v1_set_tf_named
    (creator, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_EXT_LINEAR);
  wp_image_description_creator_params_v1_set_primaries_named
    (creator, WP_COLOR_MANAGER_V1_PRIMARIES_BT2020);
  // with min 0, a signal E in [0, 1] means E * max nits: 1.0, reference
  // white, is E = REFERENCE_NITS / max
  if(b->luminances)
    wp_image_description_creator_params_v1_set_luminances(creator, 0, max_lum,
                                                          REFERENCE_NITS);
  b->pending = wp_image_description_creator_params_v1_create(creator);
  b->pending_max = b->luminances ? max_lum : 0;
  b->pending_failed = FALSE;
  wp_image_description_v1_add_listener(b->pending, &_description_listener, b);
}

static void _set_output(_backend_t *b)
{
  dt_display_output_t output = { .type = DT_COLORSPACE_LIN_REC2020,
                                 .intent = b->relative ? DT_INTENT_RELATIVE_COLORIMETRIC
                                                       : DT_INTENT_PERCEPTUAL,
                                 .headroom = _headroom(b->description_max),
                                 .managed = TRUE };
  // GTK3 cannot tag its surface, and an untagged surface is sRGB to the
  // compositor
  dt_display_output_t gtk = { .type = DT_COLORSPACE_SRGB,
                              .intent = output.intent,
                              .headroom = 1.0f };
  dt_presenter_window_set_output(b->window, &output, &gtk);
}

static void _info_done(void *data,
                       struct wp_image_description_info_v1 *info)
{
  _backend_t *b = data;
  wp_image_description_info_v1_destroy(info);
  b->info = NULL;
  b->preferred_done = TRUE;

  // a hint of the compositor, not a measurement of the screen
  const uint32_t peak = b->target_max_lum ? b->target_max_lum : b->max_lum;
  const float headroom = b->reference_lum ? fmaxf(1.0f, (float)peak / b->reference_lum)
                                          : 1.0f;
  const uint32_t max_lum = lroundf(headroom * REFERENCE_NITS);

  if(!b->luminances) return;
  if(max_lum == b->description_max)
  {
    // back to the one in use: one still on its way is obsolete
    if(b->pending) wp_image_description_v1_destroy(b->pending);
    b->pending = NULL;
  }
  else if(!b->pending || max_lum != b->pending_max)
    _create_description(b, max_lum);
}

static void _description_ready(_backend_t *b,
                               struct wp_image_description_v1 *description)
{
  if(description == b->preferred)
  {
    b->reference_lum = b->max_lum = b->target_max_lum = 0;
    b->info = wp_image_description_v1_get_information(description);
    wp_image_description_info_v1_add_listener(b->info, &_info_listener, b);
    wp_image_description_v1_destroy(description);
    b->preferred = NULL;
    return;
  }
  if(description != b->pending) return;

  if(b->description) wp_image_description_v1_destroy(b->description);
  b->description = b->pending;
  b->description_max = b->pending_max;
  b->pending = NULL;
  for(GList *l = b->widgets; l; l = g_list_next(l))
  {
    _widget_t *w = l->data;
    if(w->color)
      wp_color_management_surface_v1_set_image_description(w->color, b->description,
                                                           _intent(b));
  }
  // during setup, dt_presenter_wayland_new() declares the output
  if(b->backend.present) _set_output(b);
}

static void _request_preferred(_backend_t *b)
{
  if(b->preferred) wp_image_description_v1_destroy(b->preferred);
  if(b->info) wp_image_description_info_v1_destroy(b->info);
  b->info = NULL;
  b->preferred_done = b->preferred_failed = FALSE;
  b->preferred =
    wp_color_management_surface_feedback_v1_get_preferred_parametric(b->feedback);
  wp_image_description_v1_add_listener(b->preferred, &_description_listener, b);
}

static void _preferred_changed(void *data,
                               struct wp_color_management_surface_feedback_v1 *feedback,
                               uint32_t identity)
{
  _request_preferred(data);
}

static const struct wp_color_management_surface_feedback_v1_listener
  _feedback_listener = {
  .preferred_changed = _preferred_changed,
};

static void _feedback_init(_backend_t *b)
{
  b->feedback = wp_color_manager_v1_get_surface_feedback(b->manager, b->parent);
  wp_color_management_surface_feedback_v1_add_listener(b->feedback,
                                                       &_feedback_listener, b);
  _request_preferred(b);
}

// GtkWindow's unmap, which runs before this handler, makes GDK destroy its
// surface, and the feedback on it becomes inert: asking it for the
// preferred description after a preferred_changed is a protocol error.
// drop what belongs to that surface before events are dispatched again.
// darktable never shows such a toplevel again (the main window is hidden
// at quit, the second window is destroyed after hiding), so nothing is
// presented from here on
static void _toplevel_unmap(GtkWidget *toplevel,
                            _backend_t *b)
{
  if(b->info) wp_image_description_info_v1_destroy(b->info);
  if(b->preferred) wp_image_description_v1_destroy(b->preferred);
  if(b->feedback) wp_color_management_surface_feedback_v1_destroy(b->feedback);
  b->info = NULL;
  b->preferred = NULL;
  b->feedback = NULL;
  b->parent = NULL;
}

// GTK3 runs all draw handlers of a frame in the frame clock's paint phase
// and GDK commits its own surface in after-paint
// (gdkwindow-wayland.c, on_frame_clock_after_paint()), so the end of paint
// is the one point where every layer of the frame is drawn and nothing is
// shown yet
static void _paint(GdkFrameClock *clock,
                   _backend_t *b)
{
  dt_presenter_window_present(b->window);
}

/* setup */

static void _supported_intent(void *data,
                              struct wp_color_manager_v1 *manager,
                              uint32_t intent)
{
  _backend_t *b = data;
  if(intent == WP_COLOR_MANAGER_V1_RENDER_INTENT_RELATIVE) b->relative = TRUE;
}

static void _supported_feature(void *data,
                               struct wp_color_manager_v1 *manager,
                               uint32_t feature)
{
  _backend_t *b = data;
  if(feature == WP_COLOR_MANAGER_V1_FEATURE_PARAMETRIC) b->parametric = TRUE;
  if(feature == WP_COLOR_MANAGER_V1_FEATURE_SET_LUMINANCES) b->luminances = TRUE;
}

static void _supported_tf_named(void *data,
                                struct wp_color_manager_v1 *manager,
                                uint32_t tf)
{
  _backend_t *b = data;
  if(tf == WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_EXT_LINEAR) b->ext_linear = TRUE;
}

static void _supported_primaries_named(void *data,
                                       struct wp_color_manager_v1 *manager,
                                       uint32_t primaries)
{
  _backend_t *b = data;
  if(primaries == WP_COLOR_MANAGER_V1_PRIMARIES_BT2020) b->bt2020 = TRUE;
}

static void _manager_done(void *data,
                          struct wp_color_manager_v1 *manager)
{
  _backend_t *b = data;
  b->done = TRUE;
}

static const struct wp_color_manager_v1_listener _manager_listener = {
  .supported_intent = _supported_intent,
  .supported_feature = _supported_feature,
  .supported_tf_named = _supported_tf_named,
  .supported_primaries_named = _supported_primaries_named,
  .done = _manager_done,
};

static void _shm_format(void *data,
                        struct wl_shm *shm,
                        uint32_t format)
{
  _backend_t *b = data;
  if(format == WL_SHM_FORMAT_ABGR16161616F) b->half_float = TRUE;
}

static const struct wl_shm_listener _shm_listener = { .format = _shm_format };

static void _global(void *data,
                    struct wl_registry *registry,
                    uint32_t name,
                    const char *interface,
                    uint32_t version)
{
  _backend_t *b = data;
  // 4 for wl_surface.damage_buffer
  if(!strcmp(interface, wl_compositor_interface.name) && !b->compositor && version >= 4)
    b->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
  else if(!strcmp(interface, wl_subcompositor_interface.name) && !b->subcompositor)
    b->subcompositor = wl_registry_bind(registry, name, &wl_subcompositor_interface, 1);
  else if(!strcmp(interface, wl_shm_interface.name) && !b->shm)
  {
    b->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    wl_shm_add_listener(b->shm, &_shm_listener, b);
  }
  else if(!strcmp(interface, wp_color_manager_v1_interface.name) && !b->manager)
  {
    b->manager = wl_registry_bind(registry, name, &wp_color_manager_v1_interface, 1);
    wp_color_manager_v1_add_listener(b->manager, &_manager_listener, b);
  }
}

static void _global_remove(void *data,
                           struct wl_registry *registry,
                           uint32_t name)
{
}

static const struct wl_registry_listener _registry_listener = {
  .global = _global,
  .global_remove = _global_remove,
};

/* backend interface */

static gboolean _present(dt_presenter_backend_t *self,
                         GtkWidget *widget,
                         void **widget_data,
                         const dt_presenter_layers_t *layers)
{
  _backend_t *b = (_backend_t *)self;

  // unmapped, see _toplevel_unmap()
  if(!b->parent) return FALSE;

  _widget_t *w = *widget_data;
  if(!w)
  {
    w = g_malloc0(sizeof(_widget_t));
    _widget_layers_init(b, w);
    b->widgets = g_list_prepend(b->widgets, w);
    *widget_data = w;
  }

  // both layers or neither: the compositor still holding the buffers of
  // one means trying again next frame
  _buffer_t *image = layers->image
    ? _buffer_get(b, &w->image, layers->width, layers->height, 8 * layers->width,
                  WL_SHM_FORMAT_ABGR16161616F)
    : NULL;
  _buffer_t *foreground = _buffer_get(b, &w->foreground, layers->width, layers->height,
                                      4 * layers->width, WL_SHM_FORMAT_ARGB8888);
  if(!foreground || (layers->image && !image))
  {
    dt_presenter_window_redraw(b->window);
    return FALSE;
  }

  // applied with GDK's next commit, like the buffers
  wl_subsurface_set_position(w->image.subsurface, layers->x, layers->y);
  wl_subsurface_set_position(w->foreground.subsurface, layers->x, layers->y);

  if(image)
  {
    // outside the volume the description declares, colorimetry is
    // undefined: clip to it. alpha is 1 on the images, 0 off them
    const float headroom = _headroom(b->description_max);
    const float scale = 1.0f / headroom;
    const size_t npixels = (size_t)layers->width * layers->height;
    uint16_t *const out = image->data;
    const float *const in = layers->image;
    DT_OMP_FOR()
    for(size_t k = 0; k < npixels; k++)
    {
      if(in[4 * k + 3] == 0.0f)
      {
        memset(out + 4 * k, 0, 4 * sizeof(uint16_t));
        continue;
      }
      for(int c = 0; c < 3; c++)
      {
        const float v = in[4 * k + c];
        out[4 * k + c] = _half(dt_isnan(v) ? 0.0f : CLAMPF(v, 0.0f, headroom) * scale);
      }
      out[4 * k + 3] = _half(1.0f);
    }
    _layer_commit(&w->image, image, layers->scale);
  }

  memcpy(foreground->data, layers->foreground, foreground->size);
  _layer_commit(&w->foreground, foreground, layers->scale);
  wl_display_flush(b->display);
  return TRUE;
}

static void _clear(dt_presenter_backend_t *self,
                   void **widget_data)
{
  _backend_t *b = (_backend_t *)self;
  _widget_t *w = *widget_data;
  if(!w) return;

  // unmapped with GDK's next commit, like any other change of the layers
  wl_surface_attach(w->image.surface, NULL, 0, 0);
  wl_surface_commit(w->image.surface);
  wl_surface_attach(w->foreground.surface, NULL, 0, 0);
  wl_surface_commit(w->foreground.surface);
  wl_display_flush(b->display);
}

static void _release(dt_presenter_backend_t *self,
                     void **widget_data)
{
  _backend_t *b = (_backend_t *)self;
  _widget_t *w = *widget_data;
  if(!w) return;
  _widget_layers_free(w);
  b->widgets = g_list_remove(b->widgets, w);
  g_free(w);
  *widget_data = NULL;
}

static void _free(dt_presenter_backend_t *self)
{
  _backend_t *b = (_backend_t *)self;
  if(b->unmap_handler) g_signal_handler_disconnect(b->toplevel, b->unmap_handler);
  if(b->paint_handler) g_signal_handler_disconnect(b->clock, b->paint_handler);
  for(GList *l = b->widgets; l; l = g_list_next(l))
  {
    _widget_layers_free(l->data);
    g_free(l->data);
  }
  g_list_free(b->widgets);
  if(b->info) wp_image_description_info_v1_destroy(b->info);
  if(b->preferred) wp_image_description_v1_destroy(b->preferred);
  if(b->pending) wp_image_description_v1_destroy(b->pending);
  if(b->description) wp_image_description_v1_destroy(b->description);
  if(b->feedback) wp_color_management_surface_feedback_v1_destroy(b->feedback);
  if(b->manager) wp_color_manager_v1_destroy(b->manager);
  if(b->shm) wl_shm_destroy(b->shm);
  if(b->subcompositor) wl_subcompositor_destroy(b->subcompositor);
  if(b->compositor) wl_compositor_destroy(b->compositor);
  if(b->display) wl_display_flush(b->display);
  g_free(b);
}

static void _move_to_default_queue(_backend_t *b)
{
  void *proxies[] = { b->compositor, b->subcompositor, b->shm, b->manager, b->feedback,
                      b->preferred, b->info, b->pending, b->description };
  for(int k = 0; k < G_N_ELEMENTS(proxies); k++)
    if(proxies[k]) wl_proxy_set_queue(proxies[k], NULL);
}

dt_presenter_backend_t *dt_presenter_wayland_new(GtkWidget *toplevel,
                                                 dt_presenter_window_t *window)
{
  GdkDisplay *gdk_display = gtk_widget_get_display(toplevel);
  GdkWindow *gdk_window = gtk_widget_get_window(toplevel);
  // wl_shm's formats are little endian, cairo's are native
  if(G_BYTE_ORDER != G_LITTLE_ENDIAN
     || !GDK_IS_WAYLAND_DISPLAY(gdk_display)
     || !GDK_IS_WAYLAND_WINDOW(gdk_window))
    return NULL;

  _backend_t *b = g_malloc0(sizeof(_backend_t));
  b->window = window;
  b->toplevel = toplevel;
  b->display = gdk_wayland_display_get_wl_display(gdk_display);
  b->parent = gdk_wayland_window_get_wl_surface(gdk_window);

  // set up on a queue of our own, so that waiting for the compositor
  // dispatches nothing of GDK's
  struct wl_event_queue *queue = wl_display_create_queue(b->display);
  struct wl_display *wrapper = wl_proxy_create_wrapper(b->display);
  wl_proxy_set_queue((struct wl_proxy *)wrapper, queue);
  struct wl_registry *registry = wl_display_get_registry(wrapper);
  wl_proxy_wrapper_destroy(wrapper);
  wl_registry_add_listener(registry, &_registry_listener, b);

  gboolean ok = b->parent
    && wl_display_roundtrip_queue(b->display, queue) >= 0
    && b->compositor && b->subcompositor && b->shm && b->manager
    && wl_display_roundtrip_queue(b->display, queue) >= 0
    && b->done && b->parametric && b->ext_linear && b->bt2020 && b->half_float;

  if(ok)
  {
    _feedback_init(b);
    for(int k = 0; k < 4 && !b->preferred_done && !b->preferred_failed && ok; k++)
      ok = wl_display_roundtrip_queue(b->display, queue) >= 0;
    if(!b->pending) _create_description(b, REFERENCE_NITS);
    for(int k = 0; k < 4 && !b->description && !b->pending_failed && ok; k++)
      ok = wl_display_roundtrip_queue(b->display, queue) >= 0;
    ok = ok && b->description;
  }

  wl_registry_destroy(registry);
  _move_to_default_queue(b);
  wl_event_queue_destroy(queue);

  const gboolean managed = b->manager != NULL;
  dt_print
    (DT_DEBUG_CONTROL,
     "[presenter wayland] %s: parametric %d, ext_linear %d, bt2020 %d, half float shm %d,"
     " luminances %d, headroom %.2f",
     ok ? "color managed" : managed ? "not available, using cairo in sRGB"
                                    : "not available, using cairo",
     b->parametric, b->ext_linear, b->bt2020, b->half_float, b->luminances,
     _headroom(b->description_max));
  if(!ok)
  {
    _free(&b->backend);
    // a compositor that manages color takes GTK's untagged surface as sRGB
    // and converts it to the screen itself: rendering for the display
    // profile would convert twice
    if(managed)
    {
      const dt_display_output_t srgb = { .type = DT_COLORSPACE_SRGB,
                                         .intent = DT_INTENT_PERCEPTUAL,
                                         .headroom = 1.0f };
      dt_presenter_window_set_output(window, &srgb, &srgb);
    }
    return NULL;
  }

  b->backend.present = _present;
  b->backend.clear = _clear;
  b->backend.release = _release;
  b->backend.free = _free;
  b->unmap_handler = g_signal_connect(toplevel, "unmap", G_CALLBACK(_toplevel_unmap), b);
  b->clock = gdk_window_get_frame_clock(gdk_window);
  b->paint_handler = g_signal_connect_after(b->clock, "paint", G_CALLBACK(_paint), b);
  _set_output(b);
  return &b->backend;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
