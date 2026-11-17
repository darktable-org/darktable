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

#include "common/colorspaces.h"

#include <cairo.h>

G_BEGIN_DECLS

/*
 * a frame is a rendered image as a screen pipe publishes it: its pixels in
 * the format its sink asked for, what they mean, and the output it was made
 * for. see dev-doc/display_presentation.md
 *
 * a frame never changes once it is made, and is shared by reference: a
 * consumer holds one for as long as it may draw the frame. any other
 * format or encoding of it is a rendition, made from the frame's own
 * pixels when it is first asked for and kept with the frame
 */

typedef struct dt_frame_t dt_frame_t;

typedef struct dt_frame_geometry_t
{
  int width, height;
  // where the frame lies in the image: its scale, and its center and two
  // points beside it taken back to the input image, which carry rotation
  float scale;
  dt_dev_zoom_pos_t zoom_pos;
} dt_frame_geometry_t;

/** a frame of float RGBA pixels in encoding, stored in format, made for
 * the output with serial made_for. the pixels are copied (an RGB24 frame
 * packs them with dt_display_to_8bit()). NULL if out of memory */
dt_frame_t *dt_frame_new(const float *pixels,
                         const dt_frame_geometry_t *geometry,
                         const dt_frame_format_t format,
                         const dt_color_encoding_t *encoding,
                         const uint32_t made_for);

/** take a reference; NULL is passed through */
dt_frame_t *dt_frame_ref(dt_frame_t *frame);
/** drop a reference; NULL does nothing */
void dt_frame_unref(dt_frame_t *frame);

/** a 32-bit count of the frames made: two frames with the same id are the
 * same frame, for as long as a session runs in practice */
uint32_t dt_frame_id(const dt_frame_t *frame);
/** the serial of the output the frame was made for */
uint32_t dt_frame_made_for(const dt_frame_t *frame);
const dt_frame_geometry_t *dt_frame_geometry(const dt_frame_t *frame);
dt_frame_format_t dt_frame_format(const dt_frame_t *frame);
/** what the pixels are: what the pipe produced, not what it was asked for */
const dt_color_encoding_t *dt_frame_encoding(const dt_frame_t *frame);
/** the pixels in the frame's format, 4 * width values or bytes per row */
const void *dt_frame_pixels(const dt_frame_t *frame);

/** the frame as an RGB24 surface in encoding, the frame's own if NULL.
 * valid while the frame is: take a reference on it to keep it longer.
 * NULL if it cannot be made. any thread */
cairo_surface_t *dt_frame_surface(dt_frame_t *frame,
                                  const dt_color_encoding_t *encoding);
/** the frame as float RGBA in encoding, the frame's own if NULL. valid
 * while the frame is; NULL if it cannot be made. any thread */
const float *dt_frame_float(dt_frame_t *frame,
                            const dt_color_encoding_t *encoding);
/** the frame as GTK shows it in window (DT_COLORSPACE_DISPLAY or
 * DISPLAY2): dt_frame_surface() in the window's device encoding */
cairo_surface_t *dt_frame_device_surface(dt_frame_t *frame,
                                         const dt_colorspaces_color_profile_type_t window);

/** release what renditions keep for the session, before
 * dt_colorspaces_cleanup() */
void dt_frame_cleanup(void);

G_END_DECLS

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
