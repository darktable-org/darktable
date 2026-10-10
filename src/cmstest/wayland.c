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

#include "wayland.h"
#include "color-management-v1-client-protocol.h"

#include <glib.h>
#include <glib/gi18n.h>
#include <errno.h>
#include <lcms2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct dt_cmstest_output_t
{
  uint32_t id;
  struct wl_output *output;
  struct wp_color_management_output_v1 *color;
  struct wp_image_description_v1 *description;
  struct wp_image_description_info_v1 *info;
  char *name, *model;
  GString *details;
  gboolean removed, complete, failed, has_icc;
} dt_cmstest_output_t;

typedef struct dt_cmstest_wayland_t
{
  struct wp_color_manager_v1 *manager;
  GList *outputs;
  gboolean compositor, subcompositor, shm, done;
  gboolean parametric, icc, bt709, bt2020, gamma22, relative;
} dt_cmstest_wayland_t;

static void _geometry(void *data, struct wl_output *output, int32_t x, int32_t y,
                      int32_t width, int32_t height, int32_t subpixel,
                      const char *make, const char *model, int32_t transform)
{
  dt_cmstest_output_t *o = data;
  g_free(o->model);
  o->model = g_strdup_printf("%s %s", make, model);
}

static void _mode(void *data, struct wl_output *output, uint32_t flags,
                  int32_t width, int32_t height, int32_t refresh)
{
}

static void _output_done(void *data, struct wl_output *output)
{
}

static void _scale(void *data, struct wl_output *output, int32_t factor)
{
}

#ifdef WL_OUTPUT_NAME_SINCE_VERSION
static void _name(void *data, struct wl_output *output, const char *name)
{
  dt_cmstest_output_t *o = data;
  g_free(o->name);
  o->name = g_strdup(name);
}

static void _output_description(void *data, struct wl_output *output, const char *description)
{
  dt_cmstest_output_t *o = data;
  g_free(o->model);
  o->model = g_strdup(description);
}
#endif

static const struct wl_output_listener _output_listener = {
  .geometry = _geometry, .mode = _mode, .done = _output_done, .scale = _scale,
#ifdef WL_OUTPUT_NAME_SINCE_VERSION
  .name = _name, .description = _output_description
#endif
};

static void _intent(void *data, struct wp_color_manager_v1 *manager, uint32_t value)
{
  dt_cmstest_wayland_t *d = data;
  if(value == WP_COLOR_MANAGER_V1_RENDER_INTENT_RELATIVE) d->relative = TRUE;
}

static void _feature(void *data, struct wp_color_manager_v1 *manager, uint32_t value)
{
  dt_cmstest_wayland_t *d = data;
  if(value == WP_COLOR_MANAGER_V1_FEATURE_PARAMETRIC) d->parametric = TRUE;
  if(value == WP_COLOR_MANAGER_V1_FEATURE_ICC_V2_V4) d->icc = TRUE;
}

static void _transfer(void *data, struct wp_color_manager_v1 *manager, uint32_t value)
{
  dt_cmstest_wayland_t *d = data;
  if(value == WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22) d->gamma22 = TRUE;
}

static void _primaries(void *data, struct wp_color_manager_v1 *manager, uint32_t value)
{
  dt_cmstest_wayland_t *d = data;
  if(value == WP_COLOR_MANAGER_V1_PRIMARIES_SRGB) d->bt709 = TRUE;
  if(value == WP_COLOR_MANAGER_V1_PRIMARIES_BT2020) d->bt2020 = TRUE;
}

static void _manager_done(void *data, struct wp_color_manager_v1 *manager)
{
  dt_cmstest_wayland_t *d = data;
  d->done = TRUE;
}

static const struct wp_color_manager_v1_listener _manager_listener = {
  .supported_intent = _intent, .supported_feature = _feature,
  .supported_tf_named = _transfer, .supported_primaries_named = _primaries,
  .done = _manager_done
};

static void _global(void *data, struct wl_registry *registry, uint32_t name,
                    const char *interface, uint32_t version)
{
  dt_cmstest_wayland_t *d = data;
  if(!strcmp(interface, wp_color_manager_v1_interface.name))
  {
    d->manager = wl_registry_bind(registry, name, &wp_color_manager_v1_interface, 1);
    wp_color_manager_v1_add_listener(d->manager, &_manager_listener, d);
  }
  else if(!strcmp(interface, wl_output_interface.name))
  {
    dt_cmstest_output_t *o = g_malloc0(sizeof(*o));
    o->id = name;
    o->details = g_string_new(NULL);
    o->output = wl_registry_bind(registry, name, &wl_output_interface,
                                MIN(version, (uint32_t)wl_output_interface.version));
    wl_output_add_listener(o->output, &_output_listener, o);
    d->outputs = g_list_append(d->outputs, o);
  }
  else if(!strcmp(interface, wl_compositor_interface.name)) d->compositor = version >= 3;
  else if(!strcmp(interface, wl_subcompositor_interface.name)) d->subcompositor = TRUE;
  else if(!strcmp(interface, wl_shm_interface.name)) d->shm = TRUE;
}

static void _global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
  dt_cmstest_wayland_t *d = data;
  for(GList *l = d->outputs; l; l = g_list_next(l))
  {
    dt_cmstest_output_t *o = l->data;
    if(o->id == name) o->removed = TRUE;
  }
}

static const struct wl_registry_listener _registry_listener = {
  .global = _global, .global_remove = _global_remove
};

static void _info_done(void *data, struct wp_image_description_info_v1 *info)
{
  dt_cmstest_output_t *o = data;
  o->complete = TRUE;
  // done destroys the server object; the client proxy still needs releasing
  wp_image_description_info_v1_destroy(info);
  o->info = NULL;
}

static void _icc_file(void *data, struct wp_image_description_info_v1 *info,
                      int32_t fd, uint32_t size)
{
  dt_cmstest_output_t *o = data;
  o->has_icc = TRUE;
  g_string_append_printf(o->details, _("  ICC file: %u bytes\n"), size);
  struct stat st;
  // validate before mapping: a short file would fault inside the ICC parser
  if(size > 0 && size <= 64u * 1024u * 1024u
     && fstat(fd, &st) == 0 && st.st_size >= size)
  {
    void *mapping = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if(mapping != MAP_FAILED)
    {
      cmsHPROFILE profile = cmsOpenProfileFromMem(mapping, size);
      if(profile)
      {
        char name[512] = { 0 };
        cmsGetProfileInfoASCII(profile, cmsInfoDescription, "en", "US", name, sizeof(name));
        g_string_append_printf(o->details, _("  profile: %s\n"), name);
        cmsCloseProfile(profile);
      }
      else
        g_string_append(o->details, _("  ICC description could not be parsed\n"));
      munmap(mapping, size);
    }
    else
      g_string_append(o->details, _("  ICC description could not be mapped\n"));
  }
  else
    g_string_append(o->details, _("  ICC description is unreadable or exceeds the 64 MiB inspection limit\n"));
  close(fd);
}

static void _coordinates(GString *text, const char *label,
                         int32_t rx, int32_t ry, int32_t gx, int32_t gy,
                         int32_t bx, int32_t by, int32_t wx, int32_t wy)
{
  g_string_append_printf(text,
    "  %s: R(%.6f, %.6f) G(%.6f, %.6f) B(%.6f, %.6f) W(%.6f, %.6f)\n",
    label, rx / 1e6, ry / 1e6, gx / 1e6, gy / 1e6, bx / 1e6, by / 1e6, wx / 1e6, wy / 1e6);
}

static void _info_primaries(void *data, struct wp_image_description_info_v1 *info,
                            int32_t rx, int32_t ry, int32_t gx, int32_t gy,
                            int32_t bx, int32_t by, int32_t wx, int32_t wy)
{
  dt_cmstest_output_t *o = data;
  _coordinates(o->details, _("encoding primaries (CIE xy)"), rx, ry, gx, gy, bx, by, wx, wy);
}

static void _target_primaries(void *data, struct wp_image_description_info_v1 *info,
                              int32_t rx, int32_t ry, int32_t gx, int32_t gy,
                              int32_t bx, int32_t by, int32_t wx, int32_t wy)
{
  dt_cmstest_output_t *o = data;
  _coordinates(o->details, _("target primaries (CIE xy)"), rx, ry, gx, gy, bx, by, wx, wy);
}

static void _named_primaries(void *data, struct wp_image_description_info_v1 *info,
                             uint32_t value)
{
  dt_cmstest_output_t *o = data;
  const char *name = _("other");
  switch(value)
  {
    case WP_COLOR_MANAGER_V1_PRIMARIES_SRGB: name = "sRGB / BT.709"; break;
    case WP_COLOR_MANAGER_V1_PRIMARIES_BT2020: name = "BT.2020"; break;
    case WP_COLOR_MANAGER_V1_PRIMARIES_DISPLAY_P3: name = "Display P3"; break;
    case WP_COLOR_MANAGER_V1_PRIMARIES_ADOBE_RGB: name = "Adobe RGB"; break;
  }
  g_string_append_printf(o->details, _("  named primaries: %s (protocol value %u)\n"), name, value);
}

static void _tf_power(void *data, struct wp_image_description_info_v1 *info, uint32_t value)
{
  dt_cmstest_output_t *o = data;
  g_string_append_printf(o->details, _("  transfer function: power %.4f\n"), value / 10000.0);
}

static void _tf_named(void *data, struct wp_image_description_info_v1 *info, uint32_t value)
{
  dt_cmstest_output_t *o = data;
  const char *name = _("other");
  switch(value)
  {
    case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_BT1886: name = "BT.1886"; break;
    case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22: name = "gamma 2.2"; break;
    case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA28: name = "gamma 2.8"; break;
    case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_SRGB: name = _("sRGB piecewise"); break;
    case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_EXT_LINEAR: name = _("extended linear"); break;
    case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ: name = "ST 2084 PQ"; break;
    case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_HLG: name = "HLG"; break;
  }
  g_string_append_printf(o->details, _("  transfer function: %s (protocol value %u)\n"), name, value);
}

static void _luminances(void *data, struct wp_image_description_info_v1 *info,
                        uint32_t minimum, uint32_t maximum, uint32_t reference)
{
  dt_cmstest_output_t *o = data;
  g_string_append_printf(o->details,
    _("  encoding luminance: min %.4f, max %u, reference white %u cd/m^2\n"),
    minimum / 10000.0, maximum, reference);
}

static void _target_luminance(void *data, struct wp_image_description_info_v1 *info,
                              uint32_t minimum, uint32_t maximum)
{
  dt_cmstest_output_t *o = data;
  g_string_append_printf(o->details, _("  target luminance: min %.4f, max %u cd/m^2\n"),
                         minimum / 10000.0, maximum);
}

static void _max_cll(void *data, struct wp_image_description_info_v1 *info, uint32_t value)
{
  dt_cmstest_output_t *o = data;
  g_string_append_printf(o->details, _("  max CLL: %u cd/m^2\n"), value);
}

static void _max_fall(void *data, struct wp_image_description_info_v1 *info, uint32_t value)
{
  dt_cmstest_output_t *o = data;
  g_string_append_printf(o->details, _("  max FALL: %u cd/m^2\n"), value);
}

static const struct wp_image_description_info_v1_listener _info_listener = {
  .done = _info_done, .icc_file = _icc_file, .primaries = _info_primaries,
  .primaries_named = _named_primaries, .tf_power = _tf_power, .tf_named = _tf_named,
  .luminances = _luminances, .target_primaries = _target_primaries,
  .target_luminance = _target_luminance, .target_max_cll = _max_cll, .target_max_fall = _max_fall
};

static void _ready(void *data, struct wp_image_description_v1 *description, uint32_t identity)
{
  dt_cmstest_output_t *o = data;
  o->info = wp_image_description_v1_get_information(description);
  wp_image_description_info_v1_add_listener(o->info, &_info_listener, o);
}

static void _failed(void *data, struct wp_image_description_v1 *description,
                    uint32_t cause, const char *message)
{
  dt_cmstest_output_t *o = data;
  o->complete = TRUE;
  o->failed = TRUE;
  g_string_append_printf(o->details, _("  color description unavailable: %s (reason %u)\n"), message, cause);
}

static const struct wp_image_description_v1_listener _description_listener = {
  .ready = _ready, .failed = _failed
};

static void _changed(void *data, struct wp_color_management_output_v1 *output)
{
  // report the description requested at startup
}

static const struct wp_color_management_output_v1_listener _color_listener = {
  .image_description_changed = _changed
};

static void _capability(const char *name, gboolean supported)
{
  printf("  %s: %s\n", name, supported ? _("yes") : _("no"));
}

// a sync reply does not guarantee that asynchronous descriptions have arrived
static int _wait_for_descriptions(struct wl_display *display,
                                  dt_cmstest_wayland_t *d,
                                  const gint64 deadline)
{
  while(TRUE)
  {
    if(wl_display_dispatch_pending(display) < 0) return -1;
    gboolean pending = FALSE;
    for(GList *l = d->outputs; l; l = g_list_next(l))
    {
      const dt_cmstest_output_t *o = l->data;
      if(o->description && !o->removed && !o->complete) pending = TRUE;
    }
    if(!pending) return 1;
    const gint64 remaining = deadline - g_get_monotonic_time();
    if(remaining <= 0) return 0;
    if(wl_display_prepare_read(display) < 0) continue;

    GPollFD fd = { .fd = wl_display_get_fd(display), .events = G_IO_IN };
    if(wl_display_flush(display) < 0)
    {
      if(errno != EAGAIN)
      {
        wl_display_cancel_read(display);
        return -1;
      }
      fd.events |= G_IO_OUT;
    }
    const int status = g_poll(&fd, 1, (remaining + 999) / 1000);
    if(status > 0 && (fd.revents & G_IO_IN))
    {
      if(wl_display_read_events(display) < 0) return -1;
    }
    else
    {
      wl_display_cancel_read(display);
      if(status == 0) return 0;
      if((status < 0 && errno != EINTR)
         || (fd.revents & (G_IO_ERR | G_IO_HUP | G_IO_NVAL))) return -1;
    }
  }
}

int dt_cmstest_wayland(void)
{
  struct wl_display *display = wl_display_connect(NULL);
  if(!display)
  {
    fprintf(stderr, _("cannot connect to the Wayland display\n"));
    return DT_CMSTEST_WAYLAND_UNAVAILABLE;
  }
  printf(_("backend: Wayland\n"));
  dt_cmstest_wayland_t d = { 0 };
  struct wl_registry *registry = wl_display_get_registry(display);
  wl_registry_add_listener(registry, &_registry_listener, &d);
  int result = EXIT_FAILURE;
  gboolean timed_out = FALSE;
  if(wl_display_roundtrip(display) < 0 || wl_display_roundtrip(display) < 0) goto cleanup;

  if(d.manager && d.done)
  {
    printf(_("color-management-v1: available\n"));
    _capability(_("parametric input"), d.parametric);
    _capability(_("ICC input"), d.icc);
    _capability(_("BT.709 primaries"), d.bt709);
    _capability(_("BT.2020 primaries"), d.bt2020);
    _capability(_("gamma 2.2 transfer function"), d.gamma22);
    _capability(_("relative colorimetric intent"), d.relative);
    for(GList *l = d.outputs; l; l = g_list_next(l))
    {
      dt_cmstest_output_t *o = l->data;
      if(o->removed) continue;
      o->color = wp_color_manager_v1_get_output(d.manager, o->output);
      wp_color_management_output_v1_add_listener(o->color, &_color_listener, o);
      o->description = wp_color_management_output_v1_get_image_description(o->color);
      wp_image_description_v1_add_listener(o->description, &_description_listener, o);
    }
    const int status = _wait_for_descriptions
      (display, &d, g_get_monotonic_time() + 2 * G_TIME_SPAN_SECOND);
    if(status < 0) goto cleanup;
    timed_out = status == 0;
  }
  else
    printf(_("color-management-v1: unavailable\n"));

  for(GList *l = d.outputs; l; l = g_list_next(l))
  {
    dt_cmstest_output_t *o = l->data;
    if(o->removed) continue;
    printf("\n%s (%s)\n", o->name ? o->name : _("unnamed output"),
           o->model ? o->model : _("unknown model"));
    printf("%s", o->details->str);
    if(o->description && !o->complete)
      printf(_("  color description: timed out\n"));
    if(!o->has_icc && (!o->description || (o->complete && !o->failed)))
      printf(_("  ICC file: not provided\n"));
  }
  if(!d.outputs) printf(_("no Wayland outputs were advertised\n"));
  const gboolean supported = d.done && d.compositor && d.subcompositor && d.shm
    && d.parametric && d.bt709 && d.bt2020 && d.gamma22 && d.relative;
  printf("\n%s\n", supported ? _("darktable Wayland requirements: met")
                                : _("darktable Wayland requirements: not met"));
  result = timed_out ? EXIT_FAILURE : EXIT_SUCCESS;

cleanup:
  if(result != EXIT_SUCCESS) fprintf(stderr, _("failed to read Wayland color-management information\n"));
  for(GList *l = d.outputs; l; l = g_list_next(l))
  {
    dt_cmstest_output_t *o = l->data;
    if(o->info) wp_image_description_info_v1_destroy(o->info);
    if(o->description) wp_image_description_v1_destroy(o->description);
    if(o->color) wp_color_management_output_v1_destroy(o->color);
    wl_output_destroy(o->output);
    g_string_free(o->details, TRUE);
    g_free(o->name);
    g_free(o->model);
  }
  g_list_free_full(d.outputs, g_free);
  if(d.manager) wp_color_manager_v1_destroy(d.manager);
  wl_registry_destroy(registry);
  wl_display_disconnect(display);
  return result;
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
