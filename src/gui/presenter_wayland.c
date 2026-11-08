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
 * the Wayland platform of the layered presenter backend. what watches the
 * compositor lives while the window may be native, whatever its backend
 * (dt_presenter_platform_t in presenter_backend.h): the globals, what the
 * color manager supports and the preferred description of GDK's surface,
 * which give the headroom reports and whether the compositor
 * color-manages the window's tagged content now. the transport
 * exists while the window is native: the image layer is a subsurface
 * tagged with color-management-v1 as linear BT.2020, carrying the image in
 * half floats with its HDR headroom, and the compositor does the one
 * conversion to the screen. the foreground layer above it is an untagged
 * 8-bit subsurface, which the compositor treats like GTK's own untagged
 * surface. see dev-doc/display_presentation.md.
 */

// for memfd_create()
#define _GNU_SOURCE

#include "gui/presenter_layers.h"
#include "common/darktable.h"

#include "color-management-v1-client-protocol.h"

#include <errno.h>
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

typedef struct _widget_t
{
  _layer_t image, foreground;
  struct wp_color_management_surface_v1 *color;
  // the last reservation
  dt_presenter_layers_t layers;
  _buffer_t *reserved_image, *reserved_foreground;
} _widget_t;

typedef struct _platform_t
{
  dt_presenter_platform_t platform;
  dt_presenter_window_t *window;
  GtkWidget *toplevel;
  gulong unmap_handler;
  struct wl_display *display;
  struct wl_compositor *compositor;
  struct wl_subcompositor *subcompositor;
  struct wl_shm *shm;
  struct wp_color_manager_v1 *manager;

  // what the compositor supports
  gboolean done, parametric, luminances, ext_linear, bt2020, relative, half_float;
  // announces GTK's private protocol, as mutter does, and the session is
  // GNOME's
  gboolean gtk_shell, gnome;

  // GDK's surface of the toplevel, which the layers are children of
  struct wl_surface *parent;
  struct wp_color_management_surface_feedback_v1 *feedback;

  // the compositor's preferred description and its information, while read
  struct wp_image_description_v1 *preferred;
  struct wp_image_description_info_v1 *info;
  int32_t primaries[8];
  uint32_t tf, reference_lum, max_lum, target_max_lum;
  gboolean icc, preferred_done, preferred_failed;
  // while the platform sets up, and whether the preference changed then
  gboolean setup, changed_in_setup;
  // what the reports' generation counts: preferred descriptions that
  // differ in more than their luminances, another screen or mode
  int32_t reported_primaries[8];
  uint32_t reported_tf;
  gboolean reported_icc;
  uint32_t generation;

  // the compositor color-manages the window's tagged content, as the last
  // preferred description read says
  gboolean native;
} _platform_t;

typedef struct _transport_t
{
  dt_presenter_transport_t transport;
  _platform_t *p;
  // what makes the descriptions: the platform's color manager, or while
  // the transport sets up, a wrapper of it on the setup's queue
  struct wp_color_manager_v1 *manager;
  GdkFrameClock *clock;
  gulong paint_handler;

  // ours: in use, and created, waiting for ready or failed
  struct wp_image_description_v1 *description, *pending;
  uint32_t description_max, pending_max;
  // while the transport sets up, and why its first description failed
  gboolean setup;
  gchar *failure;

  GList *widgets;
} _transport_t;

static uint32_t _intent(const _platform_t *p)
{
  // perceptual is the one every compositor has to support
  return p->relative ? WP_COLOR_MANAGER_V1_RENDER_INTENT_RELATIVE
                     : WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL;
}

static float _headroom(const uint32_t max_lum)
{
  return max_lum ? (float)max_lum / REFERENCE_NITS : 1.0f;
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

static _buffer_t *_buffer_new(_transport_t *t,
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

  struct wl_shm_pool *pool = wl_shm_create_pool(t->p->shm, fd, size);
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
static _buffer_t *_buffer_get(_transport_t *t,
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
      return layer->buffers[k] = _buffer_new(t, width, height, stride, format);
  return NULL;
}

/* layers */

static void _layer_init(_platform_t *p,
                        _layer_t *layer)
{
  layer->surface = wl_compositor_create_surface(p->compositor);
  layer->subsurface = wl_subcompositor_get_subsurface(p->subcompositor,
                                                      layer->surface, p->parent);
  // the layers only show; GTK's surface below keeps getting the input
  struct wl_region *region = wl_compositor_create_region(p->compositor);
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

static void _widget_layers_init(_transport_t *t,
                                _widget_t *w)
{
  _platform_t *p = t->p;
  _layer_init(p, &w->image);
  _layer_init(p, &w->foreground);
  // a new subsurface goes on top of its siblings, which would cover GTK's
  // popovers, subsurfaces of GDK's surface too: keep the layers right above
  // GDK's surface. applied with GDK's next commit, like the position
  wl_subsurface_place_above(w->image.subsurface, p->parent);
  wl_subsurface_place_above(w->foreground.subsurface, w->image.surface);
  w->color = wp_color_manager_v1_get_surface(p->manager, w->image.surface);
  if(t->description)
    wp_color_management_surface_v1_set_image_description(w->color, t->description,
                                                         _intent(p));
}

static void _widget_layers_free(_widget_t *w)
{
  if(w->color) wp_color_management_surface_v1_destroy(w->color);
  w->color = NULL;
  _layer_free(&w->foreground);
  _layer_free(&w->image);
}

/* image descriptions */

static void _preferred_failed(void *data,
                              struct wp_image_description_v1 *description,
                              uint32_t cause,
                              const char *message)
{
  _platform_t *p = data;
  dt_print(DT_DEBUG_ALWAYS, "[presenter wayland] %s window: preferred image description"
           " failed (%u): %s", p->window->kind == DT_COLORSPACE_DISPLAY ? "main" : "second",
           cause, message);
  p->preferred_failed = TRUE;
}

// the one in use stays, and so does the output published for it. a
// failed description never becomes ready: dropped, so that the next target
// is described again
static void _description_failed(void *data,
                                struct wp_image_description_v1 *description,
                                uint32_t cause,
                                const char *message)
{
  _transport_t *t = data;
  if(description != t->pending) return;
  // the core logs a failed setup with its reason
  if(t->setup)
  {
    g_free(t->failure);
    t->failure = g_strdup_printf("image description for headroom %.4f failed (%u): %s",
                                 _headroom(t->pending_max), cause, message);
  }
  else
    dt_print(DT_DEBUG_ALWAYS, "[presenter wayland] %s window: image description for"
             " headroom %.4f failed (%u): %s",
             t->p->window->kind == DT_COLORSPACE_DISPLAY ? "main" : "second",
             _headroom(t->pending_max), cause, message);
  wp_image_description_v1_destroy(t->pending);
  t->pending = NULL;
}

static void _info_done(void *data,
                       struct wp_image_description_info_v1 *info);

static void _info_icc_file(void *data,
                           struct wp_image_description_info_v1 *info,
                           int32_t icc,
                           uint32_t icc_size)
{
  _platform_t *p = data;
  p->icc = TRUE;
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
  _platform_t *p = data;
  const int32_t xy[8] = { r_x, r_y, g_x, g_y, b_x, b_y, w_x, w_y };
  memcpy(p->primaries, xy, sizeof(xy));
}

static void _info_target_primaries(void *data,
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
  _platform_t *p = data;
  p->tf = tf;
}

static void _info_luminances(void *data,
                             struct wp_image_description_info_v1 *info,
                             uint32_t min_lum,
                             uint32_t max_lum,
                             uint32_t reference_lum)
{
  _platform_t *p = data;
  p->max_lum = max_lum;
  p->reference_lum = reference_lum;
}

static void _info_target_luminance(void *data,
                                   struct wp_image_description_info_v1 *info,
                                   uint32_t min_lum,
                                   uint32_t max_lum)
{
  _platform_t *p = data;
  p->target_max_lum = max_lum;
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
  .target_primaries = _info_target_primaries,
  .target_luminance = _info_target_luminance,
  .target_max_cll = _info_target_max_cll,
  .target_max_fall = _info_target_max_fall,
};

static void _preferred_ready(void *data,
                             struct wp_image_description_v1 *description,
                             uint32_t identity);

static const struct wp_image_description_v1_listener _preferred_listener = {
  .failed = _preferred_failed,
  .ready = _preferred_ready,
};

static void _description_ready(void *data,
                               struct wp_image_description_v1 *description,
                               uint32_t identity);

static const struct wp_image_description_v1_listener _description_listener = {
  .failed = _description_failed,
  .ready = _description_ready,
};

static void _create_description(_transport_t *t,
                                const uint32_t max_lum)
{
  if(t->pending) wp_image_description_v1_destroy(t->pending);

  struct wp_image_description_creator_params_v1 *creator =
    wp_color_manager_v1_create_parametric_creator(t->manager);
  wp_image_description_creator_params_v1_set_tf_named
    (creator, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_EXT_LINEAR);
  wp_image_description_creator_params_v1_set_primaries_named
    (creator, WP_COLOR_MANAGER_V1_PRIMARIES_BT2020);
  // with min 0, a signal E in [0, 1] means E * max nits: 1.0, reference
  // white, is E = REFERENCE_NITS / max
  if(t->p->luminances)
    wp_image_description_creator_params_v1_set_luminances(creator, 0, max_lum,
                                                          REFERENCE_NITS);
  t->pending = wp_image_description_creator_params_v1_create(creator);
  t->pending_max = t->p->luminances ? max_lum : 0;
  wp_image_description_v1_add_listener(t->pending, &_description_listener, t);
  dt_print(DT_DEBUG_CONTROL | DT_DEBUG_VERBOSE,
           "[presenter wayland] %s window: image description requested, headroom %.4f,"
           " %s luminances", t->p->window->kind == DT_COLORSPACE_DISPLAY ? "main" : "second",
           _headroom(t->pending_max), t->p->luminances ? "explicit" : "protocol-default");
}

static void _set_output(_transport_t *t)
{
  const dt_iop_color_intent_t intent = t->p->relative ? DT_INTENT_RELATIVE_COLORIMETRIC
                                                      : DT_INTENT_PERCEPTUAL;
  // GTK3 cannot tag its surface, and an untagged surface is sRGB to the
  // compositor. the image headroom is the range of the description in use,
  // which _reserve() clips to, not the target it was made for: that also
  // holds for a description made for no target (no set_luminances)
  const dt_display_output_t output =
    { .image = { .type = DT_COLORSPACE_LIN_REC2020, .intent = intent,
                 .headroom = _headroom(t->description_max) },
      .device = { .type = DT_COLORSPACE_SRGB, .intent = intent, .headroom = 1.0f },
      .format = DT_FRAME_FLOAT };
  dt_presenter_window_set_output(t->p->window, &output);
}

// describe the image for the core's headroom target: the reservation's
// encoding follows the description in use, so the transport range follows
// the description made for the target, never the compositor's raw
// preference
static void _describe(_transport_t *t,
                      const float headroom)
{
  if(!t->p->luminances) return;
  // rounded up, so that the range the transport clips to, [0, max /
  // REFERENCE_NITS] (_reserve()), holds the target
  const uint32_t max_lum = (uint32_t)ceilf(headroom * REFERENCE_NITS);
  if(max_lum == t->description_max)
  {
    // back to the one in use: one still on its way is obsolete
    if(t->pending) wp_image_description_v1_destroy(t->pending);
    t->pending = NULL;
  }
  else if(!t->pending || max_lum != t->pending_max)
    _create_description(t, max_lum);
}

static gboolean _mutter_default_color_mode(const _platform_t *p);

static void _info_done(void *data,
                       struct wp_image_description_info_v1 *info)
{
  _platform_t *p = data;
  wp_image_description_info_v1_destroy(info);
  p->info = NULL;
  p->preferred_done = TRUE;

  if(memcmp(p->primaries, p->reported_primaries, sizeof(p->primaries))
     || p->tf != p->reported_tf || p->icc != p->reported_icc)
  {
    memcpy(p->reported_primaries, p->primaries, sizeof(p->primaries));
    p->reported_tf = p->tf;
    p->reported_icc = p->icc;
    p->generation++;
  }

  // a hint of the compositor, not a measurement of the screen: KWin moves
  // the reference luminance with the HDR brightness, one description per
  // frame of its animation. without set_luminances the image is SDR
  const uint32_t peak = p->target_max_lum ? p->target_max_lum : p->max_lum;
  const dt_presenter_headroom_t report =
    { .value = p->reference_lum ? (float)peak / p->reference_lum : NAN,
      .ceiling = p->luminances ? 0.0f : 1.0f,
      .generation = p->generation };
  dt_presenter_window_report_headroom(p->window, &report);

  // after the report: a native backend chosen now describes its image for
  // the target the report settled
  const gboolean native = !_mutter_default_color_mode(p);
  if(native == p->native) return;
  p->native = native;
  dt_print(DT_DEBUG_CONTROL, "[presenter wayland] %s window: %s",
           p->window->kind == DT_COLORSPACE_DISPLAY ? "main" : "second",
           native ? "the compositor color-manages it"
                  : "mutter's default color mode requires application-side display conversion"
                    " (mutter#4597)");
  dt_presenter_window_choose(p->window);
}

// the mutter workaround: mutter's default color mode describes plain SDR sRGB
// and passes untagged pixels through, ignoring the profile assigned to the
// screen (mutter#4597), so only darktable can apply it, on cairo. mutter is
// known by GTK's private gtk_shell1 in a GNOME session: Wayfire's gtk-shell
// plugin announces gtk_shell1 too. stops matching once mutter describes its
// real output (!5177)
static gboolean _mutter_default_color_mode(const _platform_t *p)
{
  if(!p->gtk_shell || !p->gnome) return FALSE;

  // sRGB primaries and D65 white, in the protocol's units of 1e-6
  static const int32_t srgb[8] = { 640000, 330000, 300000, 600000,
                                   150000, 60000, 312700, 329000 };
  for(int k = 0; k < 8; k++)
    if(ABS(p->primaries[k] - srgb[k]) > 1000) return FALSE;

  const gboolean sdr_tf = p->tf == WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_SRGB
                          || p->tf == WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22
                          || p->tf == WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_COMPOUND_POWER_2_4;
  const uint32_t peak = p->target_max_lum ? p->target_max_lum : p->max_lum;
  return sdr_tf && !p->icc && (!p->reference_lum || peak <= p->reference_lum);
}

static void _preferred_ready(void *data,
                             struct wp_image_description_v1 *description,
                             uint32_t identity)
{
  _platform_t *p = data;
  memset(p->primaries, 0, sizeof(p->primaries));
  p->tf = p->reference_lum = p->max_lum = p->target_max_lum = 0;
  p->icc = FALSE;
  p->info = wp_image_description_v1_get_information(description);
  wp_image_description_info_v1_add_listener(p->info, &_info_listener, p);
  wp_image_description_v1_destroy(description);
  p->preferred = NULL;
}

static void _description_ready(void *data,
                               struct wp_image_description_v1 *description,
                               uint32_t identity)
{
  _transport_t *t = data;
  if(description != t->pending) return;

  // the image shown was composed for the description replaced: compose it
  // again for this one
  if(t->description)
  {
    wp_image_description_v1_destroy(t->description);
    dt_presenter_window_redraw(t->p->window);
  }
  t->description = t->pending;
  t->description_max = t->pending_max;
  t->pending = NULL;
  dt_print(DT_DEBUG_CONTROL, "[presenter wayland] %s window: linear BT.2020, FP16 shm,"
           " %s intent, headroom %.4f, %s luminances",
           t->p->window->kind == DT_COLORSPACE_DISPLAY ? "main" : "second",
           t->p->relative ? "relative" : "perceptual", _headroom(t->description_max),
           t->p->luminances ? "explicit" : "protocol-default");
  // set now, and still shown with the image encoded for the old one: an
  // image description is double-buffered state, applied by the image
  // surface's next commit (color-management-v1, set_image_description).
  // that surface is committed only with a buffer composed for the encoding
  // a reservation gives from now on, and a new encoding composes a new buffer
  // (presenter_layers.c, _image_changed()), so the description and the
  // first buffer encoded for it apply together
  for(GList *l = t->widgets; l; l = g_list_next(l))
  {
    _widget_t *w = l->data;
    if(w->color)
      wp_color_management_surface_v1_set_image_description(w->color, t->description,
                                                           _intent(t->p));
  }
  _set_output(t);
}

static void _request_preferred(_platform_t *p)
{
  if(p->preferred) wp_image_description_v1_destroy(p->preferred);
  if(p->info) wp_image_description_info_v1_destroy(p->info);
  p->info = NULL;
  p->preferred_done = p->preferred_failed = FALSE;
  p->preferred =
    wp_color_management_surface_feedback_v1_get_preferred_parametric(p->feedback);
  wp_image_description_v1_add_listener(p->preferred, &_preferred_listener, p);
}

static void _preferred_changed(void *data,
                               struct wp_color_management_surface_feedback_v1 *feedback,
                               uint32_t identity)
{
  _platform_t *p = data;
  // setup waits for the description it asked for: asking again would drop
  // it, and KWin changes the preference with every frame of a brightness
  // animation. the latest one is asked for once setup is over
  if(p->setup)
    p->changed_in_setup = TRUE;
  else
    _request_preferred(p);
}

static const struct wp_color_management_surface_feedback_v1_listener
  _feedback_listener = {
  .preferred_changed = _preferred_changed,
};

static void _feedback_init(_platform_t *p)
{
  p->feedback = wp_color_manager_v1_get_surface_feedback(p->manager, p->parent);
  wp_color_management_surface_feedback_v1_add_listener(p->feedback,
                                                       &_feedback_listener, p);
  _request_preferred(p);
}

// GtkWindow's unmap, which runs before this handler, makes GDK destroy its
// surface, and the feedback on it becomes inert: asking it for the
// preferred description after a preferred_changed is a protocol error.
// drop what belongs to that surface before events are dispatched again.
// darktable never shows such a toplevel again (the main window is hidden
// at quit, the second window is destroyed after hiding), so nothing is
// presented from here on, and the window keeps its backend
static void _toplevel_unmap(GtkWidget *toplevel,
                            _platform_t *p)
{
  if(p->info) wp_image_description_info_v1_destroy(p->info);
  if(p->preferred) wp_image_description_v1_destroy(p->preferred);
  if(p->feedback) wp_color_management_surface_feedback_v1_destroy(p->feedback);
  p->info = NULL;
  p->preferred = NULL;
  p->feedback = NULL;
  p->parent = NULL;
}

// GTK3 runs all draw handlers of a frame in the frame clock's paint phase
// and GDK commits its own surface in after-paint
// (gdkwindow-wayland.c, on_frame_clock_after_paint()), so the end of paint
// is the one point where every layer of the frame is drawn and nothing is
// shown yet
static void _paint(GdkFrameClock *clock,
                   _transport_t *t)
{
  dt_presenter_window_present(t->p->window);
}

/* setup */

static void _supported_intent(void *data,
                              struct wp_color_manager_v1 *manager,
                              uint32_t intent)
{
  _platform_t *p = data;
  if(intent == WP_COLOR_MANAGER_V1_RENDER_INTENT_RELATIVE) p->relative = TRUE;
}

static void _supported_feature(void *data,
                               struct wp_color_manager_v1 *manager,
                               uint32_t feature)
{
  _platform_t *p = data;
  if(feature == WP_COLOR_MANAGER_V1_FEATURE_PARAMETRIC) p->parametric = TRUE;
  if(feature == WP_COLOR_MANAGER_V1_FEATURE_SET_LUMINANCES) p->luminances = TRUE;
}

static void _supported_tf_named(void *data,
                                struct wp_color_manager_v1 *manager,
                                uint32_t tf)
{
  _platform_t *p = data;
  if(tf == WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_EXT_LINEAR) p->ext_linear = TRUE;
}

static void _supported_primaries_named(void *data,
                                       struct wp_color_manager_v1 *manager,
                                       uint32_t primaries)
{
  _platform_t *p = data;
  if(primaries == WP_COLOR_MANAGER_V1_PRIMARIES_BT2020) p->bt2020 = TRUE;
}

static void _manager_done(void *data,
                          struct wp_color_manager_v1 *manager)
{
  _platform_t *p = data;
  p->done = TRUE;
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
  _platform_t *p = data;
  if(format == WL_SHM_FORMAT_ABGR16161616F) p->half_float = TRUE;
}

static const struct wl_shm_listener _shm_listener = { .format = _shm_format };

static void _global(void *data,
                    struct wl_registry *registry,
                    uint32_t name,
                    const char *interface,
                    uint32_t version)
{
  _platform_t *p = data;
  // 4 for wl_surface.damage_buffer
  if(!strcmp(interface, wl_compositor_interface.name) && !p->compositor && version >= 4)
    p->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
  else if(!strcmp(interface, wl_subcompositor_interface.name) && !p->subcompositor)
    p->subcompositor = wl_registry_bind(registry, name, &wl_subcompositor_interface, 1);
  else if(!strcmp(interface, wl_shm_interface.name) && !p->shm)
  {
    p->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    wl_shm_add_listener(p->shm, &_shm_listener, p);
  }
  else if(!strcmp(interface, wp_color_manager_v1_interface.name) && !p->manager)
  {
    p->manager = wl_registry_bind(registry, name, &wp_color_manager_v1_interface, 1);
    wp_color_manager_v1_add_listener(p->manager, &_manager_listener, p);
  }
  else if(!strcmp(interface, "gtk_shell1"))
    p->gtk_shell = TRUE;
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

/* transport interface */

static void _add_shown_popover(GtkWidget *widget,
                               gpointer data)
{
  if(GTK_IS_POPOVER(widget) && gtk_widget_get_mapped(widget))
    *(GList **)data = g_list_append(*(GList **)data, widget);
}

// with DT_PRESENTER_REPLAY_POPOVERS set, the layers go above the popovers
// shown, so that they cover them as on Quartz and Win32, and the layered
// backend's replay of popovers can be tested. a popover's subsurface is
// made anew whenever it is shown, on top of its siblings, so this is done
// on every present. the toplevel lists its popovers bottom to top
static void _cover_popovers(_transport_t *t,
                            _widget_t *w)
{
  GList *popovers = NULL;
  gtk_container_forall(GTK_CONTAINER(t->p->toplevel), _add_shown_popover, &popovers);
  for(GList *p = popovers; p; p = g_list_next(p))
  {
    GdkWindow *window = gtk_widget_get_parent_window(p->data);
    struct wl_surface *surface =
      window && GDK_IS_WAYLAND_WINDOW(window) ? gdk_wayland_window_get_wl_surface(window) : NULL;
    if(surface) wl_subsurface_place_above(w->image.subsurface, surface);
  }
  if(popovers) wl_subsurface_place_above(w->foreground.subsurface, w->image.surface);
  g_list_free(popovers);
}

static gboolean _reserve(dt_presenter_transport_t *self,
                         GtkWidget *widget,
                         void **widget_data,
                         const dt_presenter_layers_t *layers,
                         dt_presenter_reservation_t *reservation)
{
  _transport_t *t = (_transport_t *)self;

  // unmapped, see _toplevel_unmap()
  if(!t->p->parent) return FALSE;

  _widget_t *w = *widget_data;
  if(!w)
  {
    w = g_malloc0(sizeof(_widget_t));
    _widget_layers_init(t, w);
    t->widgets = g_list_prepend(t->widgets, w);
    *widget_data = w;
  }

  // the foreground goes with every present. the compositor still holding
  // both of its buffers means trying again next frame
  w->reserved_foreground = _buffer_get(t, &w->foreground, layers->width, layers->height,
                                       4 * layers->width, WL_SHM_FORMAT_ARGB8888);
  if(!w->reserved_foreground) return FALSE;
  w->reserved_image = _buffer_get(t, &w->image, layers->width, layers->height,
                                  8 * layers->width, WL_SHM_FORMAT_ABGR16161616F);
  w->layers = *layers;

  // the description is ext_linear with luminances (0, max, reference): a
  // value E shows at E x max nits, so reference white is E = 1 / headroom,
  // and outside [0, 1] colorimetry is undefined
  const float headroom = _headroom(t->description_max);
  const float white = 1.0f / headroom;
  *reservation = (dt_presenter_reservation_t){
    .foreground = w->reserved_foreground->data,
    .foreground_stride = (size_t)4 * layers->width,
    .image = w->reserved_image ? w->reserved_image->data : NULL,
    .image_stride = (size_t)8 * layers->width,
    .encoding = { .clip = { 0.0f, headroom },
                  .matrix = { { white, 0.0f, 0.0f },
                              { 0.0f, white, 0.0f },
                              { 0.0f, 0.0f, white } } } };
  return TRUE;
}

static void _commit(dt_presenter_transport_t *self,
                    void **widget_data,
                    const gboolean image)
{
  _transport_t *t = (_transport_t *)self;
  _widget_t *w = *widget_data;

  // applied with GDK's next commit, like the buffers
  wl_subsurface_set_position(w->image.subsurface, w->layers.x, w->layers.y);
  wl_subsurface_set_position(w->foreground.subsurface, w->layers.x, w->layers.y);
  if(!t->transport.popovers_above) _cover_popovers(t, w);

  if(image) _layer_commit(&w->image, w->reserved_image, w->layers.scale);
  _layer_commit(&w->foreground, w->reserved_foreground, w->layers.scale);
  wl_display_flush(t->p->display);
}

static void _clear(dt_presenter_transport_t *self,
                   void **widget_data)
{
  _transport_t *t = (_transport_t *)self;
  _widget_t *w = *widget_data;
  if(!w) return;

  // unmapped with GDK's next commit, like any other change of the layers
  wl_surface_attach(w->image.surface, NULL, 0, 0);
  wl_surface_commit(w->image.surface);
  wl_surface_attach(w->foreground.surface, NULL, 0, 0);
  wl_surface_commit(w->foreground.surface);
  wl_display_flush(t->p->display);
}

static void _release(dt_presenter_transport_t *self,
                     void **widget_data)
{
  _transport_t *t = (_transport_t *)self;
  _widget_t *w = *widget_data;
  if(!w) return;
  _widget_layers_free(w);
  t->widgets = g_list_remove(t->widgets, w);
  g_free(w);
  *widget_data = NULL;
}

static void _free(dt_presenter_transport_t *self)
{
  _transport_t *t = (_transport_t *)self;
  if(t->paint_handler) g_signal_handler_disconnect(t->clock, t->paint_handler);
  for(GList *l = t->widgets; l; l = g_list_next(l))
  {
    _widget_layers_free(l->data);
    g_free(l->data);
  }
  g_list_free(t->widgets);
  if(t->pending) wp_image_description_v1_destroy(t->pending);
  if(t->description) wp_image_description_v1_destroy(t->description);
  wl_display_flush(t->p->display);
  g_free(t->failure);
  g_free(t);
}

static void _target(dt_presenter_transport_t *self,
                    const float headroom)
{
  _describe((_transport_t *)self, headroom);
}

// what broke the connection, GDK's too: a protocol error is fatal to it
// (wayland.xml, wl_display.error)
static gchar *_display_error(struct wl_display *display)
{
  const int error = wl_display_get_error(display);
  // a dispatch can fail without an error on the display, as when no file
  // descriptor is left for the events
  if(!error) return g_strdup("wayland event dispatch failed");
  if(error != EPROTO) return g_strdup(g_strerror(error));
  const struct wl_interface *interface;
  uint32_t id;
  const uint32_t code = wl_display_get_protocol_error(display, &interface, &id);
  return g_strdup_printf("protocol error %u on %s@%u", code,
                         interface ? interface->name : "unknown", id);
}

static dt_presenter_transport_t *_transport_new(dt_presenter_platform_t *self,
                                                gchar **reason)
{
  _platform_t *p = (_platform_t *)self;
  *reason = NULL;
  // unmapped, see _toplevel_unmap()
  if(!p->parent)
  {
    *reason = g_strdup("the window is not mapped");
    return NULL;
  }

  _transport_t *t = g_malloc0(sizeof(_transport_t));
  t->p = p;
  t->setup = TRUE;

  // wait for the first description on a queue of our own, so that waiting
  // dispatches nothing of GDK's or the platform's: the core may choose a
  // native backend from one of their handlers. a created description gets
  // ready or failed, however long the compositor takes (color-management-v1,
  // wp_image_description_v1)
  struct wl_event_queue *queue = wl_display_create_queue(p->display);
  t->manager = wl_proxy_create_wrapper(p->manager);
  wl_proxy_set_queue((struct wl_proxy *)t->manager, queue);
  _describe(t, dt_presenter_window_target(p->window));
  if(!t->pending) _create_description(t, REFERENCE_NITS);
  gboolean ok = TRUE;
  while(!t->description && t->pending && ok)
    ok = wl_display_dispatch_queue(p->display, queue) >= 0;
  wl_proxy_wrapper_destroy(t->manager);
  t->manager = p->manager;
  if(t->pending) wl_proxy_set_queue((struct wl_proxy *)t->pending, NULL);
  if(t->description) wl_proxy_set_queue((struct wl_proxy *)t->description, NULL);
  wl_event_queue_destroy(queue);
  t->setup = FALSE;

  if(!t->description)
  {
    // the description failed, or else waiting for it did
    *reason = t->failure ? g_steal_pointer(&t->failure) : _display_error(p->display);
    _free(&t->transport);
    return NULL;
  }
  if(!p->luminances)
    dt_print(DT_DEBUG_CONTROL, "[presenter wayland] %s window: linear output limited to SDR,"
             " set_luminances not advertised",
             p->window->kind == DT_COLORSPACE_DISPLAY ? "main" : "second");

  t->transport.reserve = _reserve;
  t->transport.commit = _commit;
  t->transport.clear = _clear;
  t->transport.release = _release;
  t->transport.free = _free;
  t->transport.target = _target;
  // GTK3 popovers are subsurfaces of GDK's surface too, above the layers,
  // which are placed right above GDK's surface (_widget_layers_init())
  t->transport.popovers_above = !g_getenv("DT_PRESENTER_REPLAY_POPOVERS");
  t->clock = gdk_window_get_frame_clock(gtk_widget_get_window(p->toplevel));
  t->paint_handler = g_signal_connect_after(t->clock, "paint", G_CALLBACK(_paint), t);
  return &t->transport;
}

static gboolean _native(dt_presenter_platform_t *self)
{
  return ((_platform_t *)self)->native;
}

static void _platform_free(dt_presenter_platform_t *self)
{
  _platform_t *p = (_platform_t *)self;
  if(p->unmap_handler) g_signal_handler_disconnect(p->toplevel, p->unmap_handler);
  if(p->info) wp_image_description_info_v1_destroy(p->info);
  if(p->preferred) wp_image_description_v1_destroy(p->preferred);
  if(p->feedback) wp_color_management_surface_feedback_v1_destroy(p->feedback);
  if(p->manager) wp_color_manager_v1_destroy(p->manager);
  if(p->shm) wl_shm_destroy(p->shm);
  if(p->subcompositor) wl_subcompositor_destroy(p->subcompositor);
  if(p->compositor) wl_compositor_destroy(p->compositor);
  if(p->display) wl_display_flush(p->display);
  g_free(p);
}

static void _move_to_default_queue(_platform_t *p)
{
  void *proxies[] = { p->compositor, p->subcompositor, p->shm, p->manager, p->feedback,
                      p->preferred, p->info };
  for(int k = 0; k < G_N_ELEMENTS(proxies); k++)
    if(proxies[k]) wl_proxy_set_queue(proxies[k], NULL);
}

dt_presenter_platform_t *dt_presenter_wayland_new(GtkWidget *toplevel,
                                                  dt_presenter_window_t *window,
                                                  gchar **reason)
{
  *reason = NULL;
  GdkDisplay *gdk_display = gtk_widget_get_display(toplevel);
  GdkWindow *gdk_window = gtk_widget_get_window(toplevel);
  // wl_shm's formats are little endian, cairo's are native
  if(G_BYTE_ORDER != G_LITTLE_ENDIAN
     || !GDK_IS_WAYLAND_DISPLAY(gdk_display)
     || !GDK_IS_WAYLAND_WINDOW(gdk_window))
    return NULL;

  _platform_t *p = g_malloc0(sizeof(_platform_t));
  p->window = window;
  p->toplevel = toplevel;
  p->display = gdk_wayland_display_get_wl_display(gdk_display);
  p->parent = gdk_wayland_window_get_wl_surface(gdk_window);
  // until a preferred description says otherwise: one without parametric
  // information, as Weston's for an ICC output, is not plain sRGB
  p->native = TRUE;
  // a colon-separated list, such as ubuntu:GNOME
  const gchar *desktop = g_getenv("XDG_CURRENT_DESKTOP");
  gchar **desktops = g_strsplit(desktop ? desktop : "", ":", -1);
  p->gnome = g_strv_contains((const gchar *const *)desktops, "GNOME");
  g_strfreev(desktops);

  // set up on a queue of our own, so that waiting for the compositor
  // dispatches nothing of GDK's
  struct wl_event_queue *queue = wl_display_create_queue(p->display);
  struct wl_display *wrapper = wl_proxy_create_wrapper(p->display);
  wl_proxy_set_queue((struct wl_proxy *)wrapper, queue);
  struct wl_registry *registry = wl_display_get_registry(wrapper);
  wl_proxy_wrapper_destroy(wrapper);
  wl_registry_add_listener(registry, &_registry_listener, p);

  gboolean ok = p->parent
    && wl_display_roundtrip_queue(p->display, queue) >= 0
    && p->compositor && p->subcompositor && p->shm && p->manager
    && wl_display_roundtrip_queue(p->display, queue) >= 0
    && p->done && p->parametric && p->ext_linear && p->bt2020 && p->half_float;

  // the first verdict, which the core reads once the platform is made:
  // the preferred description gets ready or failed, and its information
  // ends with done
  if(ok)
  {
    p->setup = TRUE;
    _feedback_init(p);
    while(!p->preferred_done && !p->preferred_failed && ok)
      ok = wl_display_dispatch_queue(p->display, queue) >= 0;
  }

  wl_registry_destroy(registry);
  _move_to_default_queue(p);
  wl_event_queue_destroy(queue);
  p->setup = FALSE;
  // on the default queue, answered while GDK dispatches
  if(ok && p->changed_in_setup) _request_preferred(p);

  if(p->done)
    dt_print(DT_DEBUG_CONTROL | DT_DEBUG_VERBOSE,
             "[presenter wayland] %s window: parametric %d, ext_linear %d, bt2020 %d,"
             " half float shm %d, luminances %d, gtk_shell1 %d, GNOME %d",
             window->kind == DT_COLORSPACE_DISPLAY ? "main" : "second",
             p->parametric, p->ext_linear, p->bt2020, p->half_float, p->luminances, p->gtk_shell,
             p->gnome);
  if(!ok)
  {
    // the core logs it
    *reason = wl_display_get_error(p->display) ? _display_error(p->display)
      : g_strdup(!p->parent ? "no parent wl_surface"
                 : !p->compositor ? "wl_compositor version 4 not advertised"
                 : !p->subcompositor ? "wl_subcompositor not advertised"
                 : !p->shm ? "wl_shm not advertised"
                 : !p->manager ? "wp_color_manager_v1 not advertised"
                 : !p->done ? "color manager capabilities incomplete"
                 : !p->parametric ? "parametric image descriptions not advertised"
                 : !p->ext_linear ? "ext_linear transfer function not advertised"
                 : !p->bt2020 ? "BT.2020 primaries not advertised"
                 : !p->half_float ? "wl_shm format ABGR16161616F not advertised"
                 : "wayland event dispatch failed");
    _platform_free(&p->platform);
    return NULL;
  }

  p->platform.native = _native;
  p->platform.transport = _transport_new;
  p->platform.free = _platform_free;
  p->unmap_handler = g_signal_connect(toplevel, "unmap", G_CALLBACK(_toplevel_unmap), p);
  return &p->platform;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
