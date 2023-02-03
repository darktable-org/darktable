/*
    This file is part of darktable,
    Copyright (C) 2010-2021 darktable developers.

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

#include <cairo.h>
#include <gtk/gtk.h>

#define CPF_USER_DATA 0x1000

typedef enum dtgtk_cairo_paint_flags_t
{
  CPF_NONE = 0,
  CPF_DIRECTION_UP = 1 << 0,
  CPF_DIRECTION_DOWN = 1 << 1,
  CPF_DIRECTION_LEFT = 1 << 2,
  CPF_DIRECTION_RIGHT = 1 << 3,
  CPF_ACTIVE = 1 << 4,
  CPF_PRELIGHT = 1 << 5,
  CPF_FOCUS = 1 << 13,
  CPF_SPECIAL_FLAG = 1 << 14, // this needs to be the last one. also update shift in dtgtk_cairo_paint_alignment

  // colorlabels
  CPF_LABEL_RED = CPF_DIRECTION_UP,
  CPF_LABEL_YELLOW = CPF_DIRECTION_DOWN,
  CPF_LABEL_GREEN = CPF_DIRECTION_LEFT,
  CPF_LABEL_BLUE = CPF_DIRECTION_RIGHT,
  CPF_LABEL_PURPLE = 1 << 7
} dtgtk_cairo_paint_flags_t;


typedef void (*DTGTKCairoPaintIconFunc)(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** Paint nothing */
void dtgtk_cairo_paint_empty(cairo_t *cr, gint x, int y, gint w, gint h, gint flags, void *data);
/** Paint a triangle left/right/up/down */
void dtgtk_cairo_paint_triangle(cairo_t *cr, gint x, int y, gint w, gint h, gint flags, void *data);
/** Paint a solid triangle left/right/up/down */
void dtgtk_cairo_paint_solid_triangle(cairo_t *cr, gint x, int y, gint w, gint h, gint flags, void *data);
/** Paint an arrow left or right */
void dtgtk_cairo_paint_arrow(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** Paint a solid arrow left/right/up/down */
void dtgtk_cairo_paint_solid_arrow(cairo_t *cr, gint x, int y, gint w, gint h, gint flags, void *data);
/** Paint an arrow with a line */
void dtgtk_cairo_paint_line_arrow(cairo_t *cr, gint x, int y, gint w, gint h, gint flags, void *data);
/** Paint a sort by icon */
void dtgtk_cairo_paint_sortby(cairo_t *cr, gint x, int y, gint w, gint h, gint flags, void *data);
/** Paint a store icon */
void dtgtk_cairo_paint_store(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** Paint a reset icon */
void dtgtk_cairo_paint_reset(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** Paint a preset icon - similar to hamburger menu */
void dtgtk_cairo_paint_presets(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** Paint a flip icon */
void dtgtk_cairo_paint_flip(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** Paint a switch icon */
void dtgtk_cairo_paint_switch(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** Paint an in-active switch icon */
void dtgtk_cairo_paint_switch_inactive(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** Paint an always-on switch icon */
void dtgtk_cairo_paint_switch_on(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** Paint an always-off switch icon */
void dtgtk_cairo_paint_switch_off(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** Paint a deprecated switch icon */
void dtgtk_cairo_paint_switch_deprecated(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** Paint a plusminus icon */
void dtgtk_cairo_paint_plusminus(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** Same as dtgtk_cairo_paint_plusminus, but render as a plus even when CFP_ACTIVE is disabled. */
void dtgtk_cairo_paint_plus(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** Paint a square plus icon */
void dtgtk_cairo_paint_square_plus(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** Paint a double arrow up/down */
void dtgtk_cairo_paint_sorting(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** Paint a simple plus icon */
void dtgtk_cairo_paint_plus_simple(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** Paint a simple minus icon */
void dtgtk_cairo_paint_minus_simple(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** Paint a simple multiply icon */
void dtgtk_cairo_paint_multiply_small(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** Paint a treelist icon */
void dtgtk_cairo_paint_treelist(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** Paint an invert icon */
void dtgtk_cairo_paint_invert(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** Paint a color rect icon */
void dtgtk_cairo_paint_color(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** Paint an eye icon */
void dtgtk_cairo_paint_eye(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** Paint an eye icon which is crossed out if toggled */
void dtgtk_cairo_paint_eye_toggle(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** Paint a timer icon */
void dtgtk_cairo_paint_timer(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** Paint a filmstrip icon */
void dtgtk_cairo_paint_filmstrip(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** Paint a directory icon */
void dtgtk_cairo_paint_directory(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** Paint a refresh/reload icon */
void dtgtk_cairo_paint_refresh(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** Paint a perspective correction icon */
void dtgtk_cairo_paint_perspective(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** Paint a get structure icon */
void dtgtk_cairo_paint_structure(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** Paint a drawn structure icon */
void dtgtk_cairo_paint_draw_structure(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** Paint a cancel X icon */
void dtgtk_cairo_paint_cancel(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** paint two boxes indicating portrait/landscape flip */
void dtgtk_cairo_paint_aspectflip(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** Paint a color label icon */
void dtgtk_cairo_paint_label(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** Paint a color label icon for selection */
void dtgtk_cairo_paint_label_sel(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** Paint the local copy symbol */
void dtgtk_cairo_paint_local_copy(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** Paint the reject icon on thumbs */
void dtgtk_cairo_paint_reject(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** Paint the remove icon */
void dtgtk_cairo_paint_remove(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** Paint a rating icon on thumbs */
void dtgtk_cairo_paint_star(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** Paint an icon for unrating star on thumbs */
void dtgtk_cairo_paint_unratestar(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** Paint an altered icon on thumbs */
void dtgtk_cairo_paint_altered(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** Paint an audio icon on thumbs */
void dtgtk_cairo_paint_audio(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** Paint a colorlabel "flower" icon on thumbs */
void dtgtk_cairo_paint_label_flower(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** paint a color picker icon - a pipette for bigger buttons */
void dtgtk_cairo_paint_colorpicker(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** paint a color picker icon with a plus sign */
void dtgtk_cairo_paint_colorpicker_set_values(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** Paint a display mask icon */
void dtgtk_cairo_paint_showmask(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** paint an alignment icon */
void dtgtk_cairo_paint_alignment(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** paint a text label icon */
void dtgtk_cairo_paint_text_label(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** paint a styles icon */
void dtgtk_cairo_paint_styles(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** paint the ? help label */
void dtgtk_cairo_paint_help(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** paint the grouping icon. */
void dtgtk_cairo_paint_grouping(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** paint the preferences wheel. */
void dtgtk_cairo_paint_preferences(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** paint the "show ovelays" icon. */
void dtgtk_cairo_paint_overlays(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** paint and */
void dtgtk_cairo_paint_and(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** paint or */
void dtgtk_cairo_paint_or(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** paint and not */
void dtgtk_cairo_paint_andnot(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** paint dropdown arrow */
void dtgtk_cairo_paint_dropdown(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** paint bracket capture */
void dtgtk_cairo_paint_bracket(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** paint lock icon */
void dtgtk_cairo_paint_lock(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** paint check mark icon */
void dtgtk_cairo_paint_check_mark(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** paint an over/under exposure icon */
void dtgtk_cairo_paint_overexposed(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** paint a raw over exposure icon */
void dtgtk_cairo_paint_rawoverexposed(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** paint a light bulb */
void dtgtk_cairo_paint_bulb(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** paint a gamut check icon */
void dtgtk_cairo_paint_gamut_check(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** paint a soft proofing icon */
void dtgtk_cairo_paint_softproof(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** paint a display icon */
void dtgtk_cairo_paint_display(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** paint a display2 icon */
void dtgtk_cairo_paint_display2(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** paint a landscape rectangle */
void dtgtk_cairo_paint_rect_landscape(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** paint a portrait rectangle */
void dtgtk_cairo_paint_rect_portrait(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** paint a polygon */
void dtgtk_cairo_paint_polygon(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** paint a zoom icon */
void dtgtk_cairo_paint_zoom(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** paint a duplicate/multi instance indicator */
void dtgtk_cairo_paint_multiinstance(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** paint a simple grid icon */
void dtgtk_cairo_paint_grid(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** paint focus peaking icon */
void dtgtk_cairo_paint_focus_peaking(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** paint camera icon */
void dtgtk_cairo_paint_camera(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** paint histogram scope icon */
void dtgtk_cairo_paint_histogram_scope(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** paint waveform scope icon */
void dtgtk_cairo_paint_waveform_scope(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** paint vectorscope icon */
void dtgtk_cairo_paint_vectorscope(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** paint linear scale icon */
void dtgtk_cairo_paint_linear_scale(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** paint logarithmic scale icon */
void dtgtk_cairo_paint_logarithmic_scale(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** paint waveform overlaid icon */
void dtgtk_cairo_paint_waveform_overlaid(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** paint RGB parade icon */
void dtgtk_cairo_paint_rgb_parade(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** paint Luv icon */
void dtgtk_cairo_paint_luv(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** paint JzAzBz icon */
void dtgtk_cairo_paint_jzazbz(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** paint RYB icon */
void dtgtk_cairo_paint_ryb(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** paint color harmony icon */
void dtgtk_cairo_paint_color_harmony(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);

/** paint active modulegroup icon */
void dtgtk_cairo_paint_modulegroup_active(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** paint favorites modulegroup icon */
void dtgtk_cairo_paint_modulegroup_favorites(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** paint basics widgets modulegroup icon */
void dtgtk_cairo_paint_modulegroup_basics(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** paint basic modulegroup icon */
void dtgtk_cairo_paint_modulegroup_basic(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** paint tone modulegroup icon */
void dtgtk_cairo_paint_modulegroup_tone(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** paint color modulegroup icon */
void dtgtk_cairo_paint_modulegroup_color(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** paint correct modulegroup icon */
void dtgtk_cairo_paint_modulegroup_correct(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** paint effect modulegroup icon */
void dtgtk_cairo_paint_modulegroup_effect(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** paint grading modulegroup icon */
void dtgtk_cairo_paint_modulegroup_grading(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** paint technical modulegroup icon */
void dtgtk_cairo_paint_modulegroup_technical(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);

/** paint the pin for map thumbnails */
void dtgtk_cairo_paint_map_pin(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);

/** Paint an eye icon for masks */
void dtgtk_cairo_paint_masks_eye(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** Paint a circle icon for masks */
void dtgtk_cairo_paint_masks_circle(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** Paint an ellipse icon for masks */
void dtgtk_cairo_paint_masks_ellipse(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** Paint a gradient icon for masks */
void dtgtk_cairo_paint_masks_gradient(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** Paint a path icon for masks */
void dtgtk_cairo_paint_masks_path(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);

// FLO

/** paint an uniform mask icon */
void dtgtk_cairo_paint_masks_uniform(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** paint a drawn mask icon */
void dtgtk_cairo_paint_masks_drawn(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** paint a parametric mask icon */
void dtgtk_cairo_paint_masks_parametric(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** paint a combined drawn&parametric mask icon */
void dtgtk_cairo_paint_masks_drawn_and_parametric(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** paint a raster mask icon */
void dtgtk_cairo_paint_masks_raster(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** paint a mask brush icon */
void dtgtk_cairo_paint_masks_brush(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** Paint a vertical gradient icon for masks selection */
void dtgtk_cairo_paint_masks_vertgradient(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** Paint a brush + inverse icon for masks selection */
void dtgtk_cairo_paint_masks_brush_and_inverse(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** Paint a multi-path icon for masks */
void dtgtk_cairo_paint_masks_multi(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** Paint an inverse icon for masks */
void dtgtk_cairo_paint_masks_inverse(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** Paint an op union icon for masks */
void dtgtk_cairo_paint_masks_union(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** Paint an op intersection icon for masks */
void dtgtk_cairo_paint_masks_intersection(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** Paint an op difference icon for masks */
void dtgtk_cairo_paint_masks_difference(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** Paint an op exclusion icon for masks */
void dtgtk_cairo_paint_masks_exclusion(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** Paint a used icon for masks */
void dtgtk_cairo_paint_masks_used(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** Paint a clone tool for retouch */
void dtgtk_cairo_paint_tool_clone(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** Paint a heal tool for retouch */
void dtgtk_cairo_paint_tool_heal(cairo_t *cr, gint x, gint y, gint w, gint h,  gint flags, void *data);
/** Paint a fill tool for retouch */
void dtgtk_cairo_paint_tool_fill(cairo_t *cr, gint x, gint y, gint w, gint h,  gint flags, void *data);
/** Paint a blur tool for retouch */
void dtgtk_cairo_paint_tool_blur(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** Paint a past icon for retouch */
void dtgtk_cairo_paint_paste_forms(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** Paint a cut icon for retouch */
void dtgtk_cairo_paint_cut_forms(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** Paint a display scale for retouch */
void dtgtk_cairo_paint_display_wavelet_scale(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** Paint a auto level icon for retouch */
void dtgtk_cairo_paint_auto_levels(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** Paint a generic four-pointed compass star */
void dtgtk_cairo_paint_compass_star(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** Paint a magic wand for tone equalizer */
void dtgtk_cairo_paint_wand(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);

// Lighttable modes

/** Lighttable: Grid mode */
void dtgtk_cairo_paint_lt_mode_grid(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** Lighttable: Zoomable */
void dtgtk_cairo_paint_lt_mode_zoom(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** Lighttable: Culling fixed */
void dtgtk_cairo_paint_lt_mode_culling_fixed(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** Lighttable: Culling dynamic */
void dtgtk_cairo_paint_lt_mode_culling_dynamic(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** Lighttable: Full Preview */
void dtgtk_cairo_paint_lt_mode_fullpreview(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);

/** Paint a link icon for basic adjustments */
void dtgtk_cairo_paint_link(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);


/** Paint a shortcut icon for input ng */
void dtgtk_cairo_paint_shortcut(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);

/** Paint a pined icon for filtering */
void dtgtk_cairo_paint_pin(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);
/** Paint a menu icon for filtering in topbar */
void dtgtk_cairo_paint_filtering_menu(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

