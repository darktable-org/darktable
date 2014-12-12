/*
    This file is part of darktable,
    copyright (c) 2012 tobias ellinghaus.

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
#ifndef __DT_GUI_GUIDES_H__
#define __DT_GUI_GUIDES_H__

#include "draw.h"

typedef struct dt_QRect_t
{
  float left, top, right, bottom, width, height;
} dt_QRect_t;

void dt_guides_q_rect(dt_QRect_t *R1, float left, float top, float width, float height);

void dt_guides_draw_simple_grid(cairo_t *cr, const float left, const float top, const float right,
                                const float bottom, float zoom_scale);

void dt_guides_draw_diagonal_method(cairo_t *cr, const float x, const float y, const float w, const float h);

void dt_guides_draw_rules_of_thirds(cairo_t *cr, const float left, const float top, const float right,
                                    const float bottom, const float xThird, const float yThird);

void dt_guides_draw_perspective(cairo_t *cr, const float x, const float y, const float w, const float h);

void dt_guides_draw_metering(cairo_t *cr, const float x, const float y, const float w, const float h);

void dt_guides_draw_harmonious_triangles(cairo_t *cr, const float left, const float top, const float right,
                                         const float bottom, const float dst);

void dt_guides_draw_golden_mean(cairo_t *cr, dt_QRect_t *R1, dt_QRect_t *R2, dt_QRect_t *R3, dt_QRect_t *R4,
                                dt_QRect_t *R5, dt_QRect_t *R6, dt_QRect_t *R7, gboolean goldenSection,
                                gboolean goldenTriangle, gboolean goldenSpiralSection, gboolean goldenSpiral);

#endif
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
