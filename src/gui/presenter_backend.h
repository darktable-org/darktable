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

#pragma once

/*
 * internal to the presenter: what presenter.c asks of a window's backend
 * and of its platform. the backend is chosen at the window's first
 * presenting draw, and again whenever the choice changes: layered
 * (presenter_layers.c) while the platform color-manages the window's tagged
 * content and has a transport for it, otherwise, or with
 * ui_last/display_force_cairo set, cairo (presenter_cairo.c). see
 * dev-doc/display_presentation.md.
 */

#include "common/colorspaces.h"
#include "develop/frame.h"
#include "gui/presenter_target.h"

#include <gtk/gtk.h>

typedef struct dt_presenter_image_t
{
  // the presenter holds a reference while the image is shown or kept
  dt_frame_t *frame;
  // the frame in the window's output as float RGBA, for a backend that
  // composes it; NULL on cairo, which paints the frame when it is placed
  const float *pixels;
  // frame pixels to the user space the widget's draw started in
  cairo_matrix_t matrix;
  cairo_filter_t filter;
  // the backend's record of the image, or NULL; the backend keeps it for
  // as long as it shows or keeps the image
  void *data;
} dt_presenter_image_t;

// what the end of a draw does to the widget's kept presentation
typedef enum dt_presenter_kept_t
{
  // the draw was restricted to a damaged area or showed the kept one
  DT_PRESENTER_KEPT_UNCHANGED,
  // the draw retained itself: it is the kept presentation from now on
  DT_PRESENTER_KEPT_THIS,
  // a full draw that neither retained itself nor showed the kept one
  DT_PRESENTER_KEPT_NONE,
} dt_presenter_kept_t;

typedef struct dt_presenter_backend_t dt_presenter_backend_t;
typedef struct dt_presenter_platform_t dt_presenter_platform_t;
typedef struct dt_presenter_transport_t dt_presenter_transport_t;

/*
 * every call takes the widget's data, which begin() creates; base is the
 * user space the widget's draw started in, the one image matrices are in
 */
struct dt_presenter_backend_t
{
  // start a draw of widget, which GTK draws on widget_cr: return the
  // context the widget draws on. *data is the backend's, NULL at first
  cairo_t *(*begin)(dt_presenter_backend_t *self,
                    GtkWidget *widget,
                    void **data,
                    cairo_t *widget_cr);
  // show image where cr's current transformation puts (x, y), in the
  // window's image encoding, or return FALSE if the frame has no rendition
  // in it. a frame made for another output is converted rather than
  // refused, as GTK4's renderers and Chromium convert content to the
  // target at draw; it stays until its producer renders it again for the
  // new serial. the headroom is no conversion: a frame keeps the tone
  // mapping it was rendered with
  gboolean (*image)(dt_presenter_backend_t *self,
                    void *data,
                    cairo_t *cr,
                    const double x,
                    const double y,
                    dt_presenter_image_t *image);
  // paint the kept presentation on cr and append the images it shows to
  // images; FALSE if there is none
  gboolean (*keep)(dt_presenter_backend_t *self,
                   void *data,
                   cairo_t *cr,
                   const cairo_matrix_t *base,
                   GArray *images);
  // finish a draw of images, which stay valid until the next one
  void (*end)(dt_presenter_backend_t *self,
              void *data,
              cairo_t *widget_cr,
              const cairo_matrix_t *base,
              const GArray *images,
              const dt_presenter_kept_t kept);
  // stop showing the widget's images and drop its kept presentation
  void (*clear)(dt_presenter_backend_t *self, void *data);
  // the widget is gone: free *data and set it to NULL
  void (*release)(dt_presenter_backend_t *self, void **data);
  // the window is gone, after every widget was released
  void (*free)(dt_presenter_backend_t *self);
  // the window's headroom target changed: prepare showing frames rendered
  // for it, and publish the output with the headroom that can then be shown
  // (dt_presenter_window_set_output()). NULL for a backend that owns no
  // output
  void (*target)(dt_presenter_backend_t *self, const float headroom);
};

/*
 * a native platform's side of a window, for as long as the window exists,
 * whatever its backend: whether the platform color-manages the window's
 * tagged content now, which a move to another monitor or a new color mode
 * can change, and the transport of the layered backend
 * (presenter_layers.h), which exists only while the window is native
 */
struct dt_presenter_platform_t
{
  // TRUE while the platform color-manages the window's tagged content. the
  // platform calls dt_presenter_window_choose() when that changes
  gboolean (*native)(dt_presenter_platform_t *self);
  // the transport of the window's layers, or NULL if it cannot set up
  dt_presenter_transport_t *(*transport)(dt_presenter_platform_t *self);
  // the window is gone, after its backend
  void (*free)(dt_presenter_platform_t *self);
};

typedef struct dt_presenter_window_t
{
  GtkWidget *toplevel;
  // DT_COLORSPACE_DISPLAY for the main window, DISPLAY2 for the second
  dt_colorspaces_color_profile_type_t kind;
  // NULL where the platform has no transport. the backend is layered if
  // native is TRUE, and NULL while it is chosen
  dt_presenter_platform_t *platform;
  dt_presenter_backend_t *backend;
  gboolean native;
  // presenter.c's records of the window's presenting widgets
  GList *widgets;
  // the headroom target the core settled from the platform's reports, with
  // the timer that settles it and the deadline the timer is set for
  dt_presenter_target_t target;
  guint settle;
  int64_t settle_deadline;
} dt_presenter_window_t;

/** the window's output as it is now */
void dt_presenter_window_output(const dt_presenter_window_t *window,
                                dt_display_output_t *output);
/** for backends, at setup or later: the platform owns the window's output
 * until the window is no longer native. its image headroom is the one the
 * transport encodes for, which follows the core's target once the
 * transport can encode that. the window redraws if its output changed
 * (dt_colorspaces_set_window_output()) */
void dt_presenter_window_set_output(dt_presenter_window_t *window,
                                    const dt_display_output_t *output);
/** for platforms, whenever they say something about the screen's
 * headroom: the core settles its target from the reports and gives it to
 * the backend's target() */
void dt_presenter_window_report_headroom(dt_presenter_window_t *window,
                                         const dt_presenter_headroom_t *report);
/** the window's headroom target now */
float dt_presenter_window_target(const dt_presenter_window_t *window);
/** for backends: draw the window's presenting widgets again */
void dt_presenter_window_redraw(dt_presenter_window_t *window);
/** for platforms: what decides the window's backend may have changed.
 * choose it again, and switch to it if it is another one */
void dt_presenter_window_choose(dt_presenter_window_t *window);

/** the native platform of window, or NULL where the platform has no
 * transport */
dt_presenter_platform_t *dt_presenter_platform_new(dt_presenter_window_t *window);
/** the layered backend of window on its platform's transport, or NULL if
 * the transport cannot set up */
dt_presenter_backend_t *dt_presenter_layered_new(dt_presenter_window_t *window);
/** the cairo backend of window */
dt_presenter_backend_t *dt_presenter_cairo_new(dt_presenter_window_t *window);

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
