/*
    This file is part of darktable,
    Copyright (C) 2011-2026 darktable developers.

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
#include "common/gdk_event_utils.h"

#include "common/darktable.h"
#include "bauhaus/bauhaus.h"
#include "common/debug.h"
#include "common/file_location.h"
#include "common/history_snapshot.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/frame.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/draw.h"
#include "libs/lib.h"
#include "libs/lib_api.h"

DT_MODULE(1)

#define HANDLE_SIZE 0.02
#define MAX_SNAPSHOT 10

// the snapshot offset in the memory table to use an area not used by the
// undo/redo support.
#define SNAPSHOT_ID_OFFSET 0xFFFFFF00

/* a snapshot */
typedef struct dt_lib_snapshot_t
{
  GtkWidget *button;
  GtkWidget *num;
  GtkWidget *status;
  GtkWidget *name;
  GtkWidget *entry;
  GtkWidget *restore_button;
  GtkWidget *bbox;
  char *module;
  char *label;
  dt_view_context_t ctx;
  dt_imgid_t imgid;
  uint32_t history_end;
  uint32_t id;
  dt_frame_t *frame;
} dt_lib_snapshot_t;

typedef struct dt_lib_snapshots_t
{
  GtkWidget *snapshots_box;

  int selected;
  gboolean snap_requested;
  guint expose_again_timeout_id;

  /* current active snapshots */
  uint32_t num_snapshots;

  /* snapshots */
  dt_lib_snapshot_t snapshot[MAX_SNAPSHOT];

  /* change snapshot overlay controls */
  gboolean dragging, vertical, inverted, panning, sidebyside;
  double vp_width, vp_height, vp_xpointer, vp_ypointer, vp_xrotate, vp_yrotate;
  gboolean on_going;
  gboolean rotsym_lightup;

  GtkWidget *take_button, *sidebyside_button;

  // id of the snapshot made by a click on the compare icon of a history entry,
  // 0 if none. It is replaced by the next click, so comparing entry after
  // entry takes one slot
  uint32_t compare_id;
  // a press on the split line of a compared step that may still turn into a
  // drag: where it was and where the line was
  gboolean pending_click;
  // the overlay took the press of the button now down, so it takes the release
  gboolean owns_press;
  double press_xp, press_yp, line_xp, line_yp;
  // a press on the boundary of the side-by-side halves of a compared step
  gboolean separator_pressed, separator_dragged;
  // the resize cursor shown over what a click swaps the sides of, NULL if not
  const char *cursor;
  double separator_x, separator_y;
} dt_lib_snapshots_t;

// history_end of the snapshot being compared with the live edit on behalf of
// a history entry, -1 if there is none
static int _compare_history_end(const dt_lib_snapshots_t *d)
{
  if(!d->compare_id || d->selected < 0) return -1;
  const dt_lib_snapshot_t *s = &d->snapshot[d->selected];
  return s->id == d->compare_id && s->imgid == darktable.develop->image_storage.id
    ? (int)s->history_end : -1;
}

static void _compare_changed(void)
{
  if(darktable.lib->proxy.snapshots.compare_changed)
    darktable.lib->proxy.snapshots.compare_changed
      (darktable.lib->proxy.snapshots.compare_user_data);
}

/* callback for take snapshot */
static void _lib_snapshots_add_button_clicked_callback(GtkWidget *widget,
                                                       dt_lib_module_t *self);

static void _lib_snapshots_toggled_callback(GtkToggleButton *widget,
                                            dt_lib_module_t *self);

static void _compare_history(dt_lib_module_t *self,
                             const int history_end);

static int _compare_get(dt_lib_module_t *self);

static void _compare_set_sidebyside(dt_lib_module_t *self,
                                    const gboolean sidebyside);

static void _compare_rotate(dt_lib_module_t *self);

static gboolean _compare_get_sidebyside(dt_lib_module_t *self);

static void _lib_snapshots_restore_callback(GtkButton *widget,
                                            dt_lib_module_t *self);

const char *name(dt_lib_module_t *self)
{
  return _("snapshots");
}

const char *description(dt_lib_module_t *self)
{
  return _("remember a specific edit state and\n"
           "allow comparing it against another\n"
           "or returning to that version");
}

dt_view_type_flags_t views(dt_lib_module_t *self)
{
  return DT_VIEW_DARKROOM;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_LEFT_CENTER;
}

int position(const dt_lib_module_t *self)
{
  return 1000;
}

enum _lib_snapshot_button_items
  {
    _SNAPSHOT_BUTTON_NUM,
    _SNAPSHOT_BUTTON_STATUS,
    _SNAPSHOT_BUTTON_NAME,
    _SNAPSHOT_BUTTON_ENTRY,
  } _lib_snapshot_button_items;

static GtkWidget *_lib_snapshot_button_get_item(GtkWidget *button,
                                                const int num)
{
  GtkWidget *cont = gtk_bin_get_child(GTK_BIN(button));
  GList *items = gtk_container_get_children(GTK_CONTAINER(cont));
  return (GtkWidget *)g_list_nth_data(items, num);
}

// draw snapshot sign
static void _draw_sym(cairo_t *cr,
                      const float x,
                      const float y,
                      const gboolean vertical,
                      const gboolean inverted)
{
  const double inv = inverted ? -0.1 : 1.0;

  PangoRectangle ink;
  PangoFontDescription *desc = dt_gui_get_font();
  pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
  pango_font_description_set_absolute_size(desc, DT_PIXEL_APPLY_DPI(12) * PANGO_SCALE);
  PangoLayout *layout = pango_cairo_create_layout(cr);
  pango_layout_set_font_description(layout, desc);
  pango_layout_set_text(layout, C_("snapshot sign", "S"), -1);
  pango_layout_get_pixel_extents(layout, &ink, NULL);

  if(vertical)
    cairo_move_to(cr, x - (inv * ink.width * 1.2f),
                  y - (ink.height / 2.0f) - DT_PIXEL_APPLY_DPI(3));
  else
    cairo_move_to(cr, x - (ink.width / 2.0),
                  y + (-inv * (ink.height * 1.2f) - DT_PIXEL_APPLY_DPI(2)));

  dt_draw_set_color_overlay(cr, FALSE, 0.9);
  pango_cairo_show_layout(cr, layout);
  pango_font_description_free(desc);
  g_object_unref(layout);
}

static gboolean _snap_expose_again(gpointer user_data)
{
  dt_lib_snapshots_t *d = (dt_lib_snapshots_t *)user_data;

  d->expose_again_timeout_id = 0;
  d->snap_requested = TRUE;
  dt_control_queue_redraw_center();
  return FALSE;
}

/* check if (x,y) closer to rotation sym than area_size. Set the size of area s
   and the center of the sym (rx, ry). Return TRUE if (x,y) in sym area. */
static inline gboolean _get_rotation_area(dt_lib_module_t *self,
                                          const int32_t x,
                                          const int32_t y,
                                          double *s,
                                          gint *rx,
                                          gint *ry)
{
  dt_lib_snapshots_t *d = self->data;

  const double _s = fmin(24, d->vp_width * HANDLE_SIZE);
  const gint _rx = (d->vertical
                    ? d->vp_width * d->vp_xpointer
                    : d->vp_width * 0.5) - (_s * 0.5);
  const gint _ry = (d->vertical
                    ? d->vp_height * 0.5
                    : d->vp_height * d->vp_ypointer) - (_s * 0.5);

  if(s)  *s = _s;
  if(rx) *rx = _rx;
  if(ry) *ry = _ry;

  const int area_size = 40;

  // rotation symbol is light-up or light-off when moving close.
  return (abs(x - _rx) < area_size) && (abs(y - _ry) < area_size);
}

// name a half of the side-by-side view of a history entry, so that it is clear
// which one is the live edit and which the frozen snapshot
static void _draw_caption(cairo_t *cr,
                          const char *text,
                          const double x,
                          const double y)
{
  PangoFontDescription *desc = dt_gui_get_font();
  pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
  pango_font_description_set_absolute_size(desc, DT_PIXEL_APPLY_DPI(12) * PANGO_SCALE);
  PangoLayout *layout = pango_cairo_create_layout(cr);
  pango_layout_set_font_description(layout, desc);
  pango_layout_set_text(layout, text, -1);

  PangoRectangle ink;
  pango_layout_get_pixel_extents(layout, &ink, NULL);

  const double pad = DT_PIXEL_APPLY_DPI(4);

  // a box under the text, so that it reads over any image
  cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.8);
  cairo_rectangle(cr, x - pad, y - pad, ink.width + 2 * pad, ink.height + 2 * pad);
  cairo_fill(cr);

  cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.95);
  cairo_move_to(cr, x, y);
  pango_cairo_show_layout(cr, layout);

  pango_font_description_free(desc);
  g_object_unref(layout);
}

/* expose snapshot over center viewport */
void gui_post_expose(dt_lib_module_t *self,
                     cairo_t *cri,
                     const int32_t width,
                     const int32_t height,
                     const int32_t pointerx,
                     const int32_t pointery)
{
  dt_lib_snapshots_t *d = self->data;
  dt_develop_t *dev = darktable.develop;

  if(d->sidebyside && (!darktable.gui->drawing_snapshot ^ !d->inverted))
  {
    // this half shows the live edit
    if(_compare_history_end(d) >= 0) _draw_caption(cri, _("current edit"), DT_PIXEL_APPLY_DPI(8), DT_PIXEL_APPLY_DPI(8));
    return;
  }

  if(d->selected >= 0)
  {
    dt_lib_snapshot_t *snap = &d->snapshot[d->selected];

    const dt_view_context_t ctx = dt_view_get_context_hash();

    // if a new snapshot is needed, do this now
    if(d->snap_requested && snap->ctx == ctx)
    {
      dt_frame_unref(snap->frame);

      // export image with proper size
      dt_display_output_t output;
      dt_colorspaces_get_display_output(DT_COLORSPACE_DISPLAY, &output);
      snap->frame = dt_dev_image(snap->imgid,
                                 width,
                                 height,
                                 FALSE,
                                 snap->history_end,
                                 snap->id,
                                 NULL,
                                 DT_DEVICE_NONE,
                                 FALSE,
                                 &output);
      d->snap_requested = FALSE;
      d->expose_again_timeout_id = 0;
    }

    // if ctx has changed, get a new snapshot at the right zoom
    // level. this is using a time out to ensure we don't try to
    // create many snapshot while zooming (this is slow), so we wait
    // to the zoom level to be stabilized to create the new snapshot.
    if(snap->ctx != ctx
       || !snap->frame)
    {
      // request a new snapshot in the following conditions:
      //    1. we are not panning
      //    2. the mouse is not over the center area, probably panning
      //    with the navigation module

      snap->ctx = ctx;
      if(!d->panning && dev->darkroom_mouse_in_center_area)
        d->snap_requested = TRUE;
      if(d->expose_again_timeout_id != 0)
        g_source_remove(d->expose_again_timeout_id);

      d->expose_again_timeout_id = g_timeout_add(150, _snap_expose_again, d);
    }

    float pzx, pzy, zoom_scale;
    dt_dev_get_pointer_zoom_pos(&dev->full, 0, 0, &pzx, &pzy, &zoom_scale);

    pzx = fmin(pzx + 0.5f, 0.0f);
    pzy = fmin(pzy + 0.5f, 0.0f);

    d->vp_width = width;
    d->vp_height = height;

    const double lx = d->sidebyside ? d->inverted ? 0 : width : width * d->vp_xpointer;
    const double ly = d->sidebyside ? d->inverted ? 0 : height : height * d->vp_ypointer;

    const double size = DT_PIXEL_APPLY_DPI(d->inverted ? -15 : 15);

    // clear background
    dt_gui_gtk_set_source_rgb(cri, DT_GUI_COLOR_DARKROOM_BG);
    if(d->vertical)
    {
      if(d->inverted)
        cairo_rectangle(cri, lx, 0, width - lx, height);
      else
        cairo_rectangle(cri, 0, 0, lx, height);
    }
    else
    {
      if(d->inverted)
        cairo_rectangle(cri, 0, ly, width, height - ly);
      else
        cairo_rectangle(cri, 0, 0, width, ly);
    }
    cairo_save(cri);
    cairo_clip(cri);
    cairo_fill(cri);

    if(snap->frame)
      dt_view_paint_surface(cri, width, height, &dev->full, DT_WINDOW_MAIN, snap->frame);

    cairo_restore(cri);

    if(_compare_history_end(d) >= 0)
    {
      // name what each part shows, in the corner of its part. In the overlay
      // the snapshot is clipped to one side of the split line, see above
      const double margin = DT_PIXEL_APPLY_DPI(8);
      double snap_x = margin, snap_y = margin;
      if(!d->sidebyside)
      {
        double live_x = margin, live_y = margin;
        if(d->vertical)
        {
          if(d->inverted) snap_x += lx; else live_x += lx;
        }
        else
        {
          if(d->inverted) snap_y += ly; else live_y += ly;
        }
        _draw_caption(cri, _("current edit"), live_x, live_y);
      }
      char *caption = g_strdup_printf(_("history: %s (#%u)"),
                                      snap->label ? snap->label : snap->module ? snap->module : "",
                                      snap->history_end);
      _draw_caption(cri, caption, snap_x, snap_y);
      g_free(caption);
    }

    // draw the split line using the selected overlay color
    dt_draw_set_color_overlay(cri, TRUE, 0.7);

    cairo_set_line_width(cri, 1.);

    if(d->vertical)
    {
      const float iheight = dev->preview_pipe->backbuf_height * zoom_scale;
      const double offset = (double)(iheight * (-pzy));
      const double center = (fabs(size) * 2.0) + offset;

      // line
      cairo_move_to(cri, lx, 0.0f);
      cairo_line_to(cri, lx, height);
      cairo_stroke(cri);

      if(!d->dragging)
      {
        // triangle
        cairo_move_to(cri, lx, center - size);
        cairo_line_to(cri, lx - (size * 1.2), center);
        cairo_line_to(cri, lx, center + size);
        cairo_close_path(cri);
        cairo_fill(cri);

        // symbol
        _draw_sym(cri, lx, center, TRUE, d->inverted);
      }
    }
    else
    {
      const float iwidth = dev->preview_pipe->backbuf_width * zoom_scale;
      const double offset = (double)(iwidth * (-pzx));
      const double center = (fabs(size) * 2.0) + offset;

      // line
      cairo_move_to(cri, 0.0f, ly);
      cairo_line_to(cri, width, ly);
      cairo_stroke(cri);

      if(!d->dragging)
      {
        // triangle
        cairo_move_to(cri, center - size, ly);
        cairo_line_to(cri, center, ly - (size * 1.2));
        cairo_line_to(cri, center + size, ly);
        cairo_close_path(cri);
        cairo_fill(cri);

        // symbol
        _draw_sym(cri, center, ly, FALSE, d->inverted);
      }
    }

    /* if mouse over control lets draw center rotate control, hide if split is dragged */
    if(!d->dragging && !d->sidebyside)
    {
      double s = 0.0;
      gint rx = 0;
      gint ry = 0;

      d->rotsym_lightup = _get_rotation_area(self, pointerx, pointery, &s, &rx, &ry);

      dt_draw_set_color_overlay(cri, TRUE, d->rotsym_lightup ? 1.0 : 0.3);

      cairo_set_line_width(cri, 0.5);
      dtgtk_cairo_paint_refresh(cri, rx, ry, s, s, 0, NULL);
    }

    d->on_going = FALSE;
  }
}

// the pointer is on the split line or its rotation handle, the only places of
// the overlay of a history entry that take a click
static gboolean _near_split_line(const dt_lib_snapshots_t *d,
                                 const double x,
                                 const double y)
{
  const double tol = DT_PIXEL_APPLY_DPI(8);
  const double line = d->vertical ? d->vp_xpointer * d->vp_width
                                  : d->vp_ypointer * d->vp_height;
  return fabs((d->vertical ? x : y) - line) < tol;
}

static gboolean _on_split_control(dt_lib_module_t *self,
                                  const double x,
                                  const double y)
{
  const dt_lib_snapshots_t *d = self->data;
  if(_near_split_line(d, x, y)) return TRUE;
  return _get_rotation_area(self, x, y, NULL, NULL, NULL);
}

// side-by-side: the pointer is on the edge the two halves share. The first
// widget is the left or top half, the snapshot widget the other one
static gboolean _on_sidebyside_separator(const dt_lib_snapshots_t *d,
                                         const double x,
                                         const double y,
                                         const gboolean second)
{
  const double tol = DT_PIXEL_APPLY_DPI(8);
  const double pos = d->vertical ? x : y;
  const double size = d->vertical ? d->vp_width : d->vp_height;
  return second ? pos < tol : pos > size - tol;
}

// a click of the darkroom, not of the split, while comparing a history entry
static gboolean _compare_passes(dt_lib_module_t *self,
                                const double x,
                                const double y)
{
  const dt_lib_snapshots_t *d = self->data;
  if(_compare_history_end(d) < 0) return FALSE;
  return d->sidebyside || !_on_split_control(self, x, y);
}

int mouse_leave(struct dt_lib_module_t *self)
{
  dt_lib_snapshots_t *d = self->data;
  if(d->cursor)
  {
    d->cursor = NULL;
    dt_control_change_cursor("default");
  }
  return 0;
}

int button_released(struct dt_lib_module_t *self,
                    const double x,
                    const double y,
                    const int which,
                    const uint32_t state)
{
  dt_lib_snapshots_t *d = self->data;

  if(d->panning)
  {
    d->panning = FALSE;
    d->owns_press = FALSE;
    return 0;
  }

  if(d->separator_pressed)
  {
    d->separator_pressed = FALSE;
    // only a click swaps the halves, not an attempt to drag the boundary
    if(!d->separator_dragged)
    {
      d->inverted = !d->inverted;
      d->snap_requested = TRUE;
      dt_control_queue_redraw_center();
    }
    return 1;
  }

  // the darkroom needs the release of a click it got the press of, see
  // button_pressed(). Redraws clear on_going and dragging, so the press is
  // tracked on its own
  const gboolean owned = d->owns_press;
  d->owns_press = FALSE;
  if(_compare_history_end(d) >= 0 && !owned)
    return 0;

  if(d->selected >= 0)
  {
    // a click on the split line, not a drag, swaps the sides
    if(d->pending_click)
    {
      d->pending_click = FALSE;
      d->vp_xpointer = d->line_xp;
      d->vp_ypointer = d->line_yp;
      d->inverted = !d->inverted;
      d->snap_requested = TRUE;
      dt_control_queue_redraw_center();
    }
    d->dragging = FALSE;
    return 1;
  }
  return 0;
}

static int _lib_snapshot_rotation_cnt = 0;

int button_pressed(struct dt_lib_module_t *self,
                   const double x,
                   const double y,
                   const double pressure,
                   const int which,
                   const int type,
                   const uint32_t state)
{
  dt_lib_snapshots_t *d = self->data;

  if(darktable.develop->darkroom_skip_mouse_events)
  {
    d->panning = TRUE;
    return 0;
  }

  // comparing a history entry side-by-side: the image stays editable, so the
  // click goes on to the darkroom instead of turning the split
  // a click on the boundary between the halves swaps them
  if(d->sidebyside && _compare_history_end(d) >= 0 && _on_sidebyside_separator(d, x, y, darktable.gui->press_in_snapshot))
  {
    d->separator_pressed = TRUE;
    d->separator_dragged = FALSE;
    d->separator_x = x;
    d->separator_y = y;
    return 1;
  }

  if(_compare_passes(self, x, y)) return 0;

  if(d->selected >= 0 && which != GDK_BUTTON_MIDDLE)
  {
    d->owns_press = TRUE;
    if(d->on_going) return 1;

    const double xp = x / d->vp_width;
    const double yp = y / d->vp_height;

    /* do the split rotating */
    const double hhs = HANDLE_SIZE * 0.5;
    if(((d->vertical
         && xp > d->vp_xpointer - hhs
         && xp < d->vp_xpointer + hhs)
        && yp > 0.5 - hhs && yp < 0.5 + hhs)
        || ((!d->vertical && yp > d->vp_ypointer - hhs
             && yp < d->vp_ypointer + hhs)
            && xp > 0.5 - hhs && xp < 0.5 + hhs)
        || d->sidebyside
        || (d->vp_xrotate > xp - hhs
            && d->vp_xrotate <= xp + hhs
            && d->vp_yrotate > yp - hhs
            && d->vp_yrotate <= yp + hhs))
    {
      /* let's rotate */
      _lib_snapshot_rotation_cnt++;

      d->vertical = !d->vertical;
      gtk_orientable_set_orientation(GTK_ORIENTABLE(gtk_widget_get_parent(dt_ui_snapshot(darktable.gui->ui))),
                                     d->vertical ?  GTK_ORIENTATION_HORIZONTAL : GTK_ORIENTATION_VERTICAL);
      if(_lib_snapshot_rotation_cnt % 2) d->inverted = !d->inverted;
      if(d->sidebyside)
        d->snap_requested = TRUE;

      d->vp_xpointer = xp;
      d->vp_ypointer = yp;
      d->vp_xrotate = xp;
      d->vp_yrotate = yp;
      d->on_going = TRUE;
      dt_control_queue_redraw_center();
    }
    /* do the dragging !? */
    else
    {
      d->pending_click = _compare_history_end(d) >= 0;
      d->press_xp = xp;
      d->press_yp = yp;
      d->line_xp = d->vp_xpointer;
      d->line_yp = d->vp_ypointer;
      d->dragging = TRUE;
      d->vp_ypointer = yp;
      d->vp_xpointer = xp;
      d->vp_xrotate = 0.0;
      d->vp_yrotate = 0.0;
      dt_control_queue_redraw_center();
    }
    return 1;
  }
  return 0;
}

int mouse_moved(dt_lib_module_t *self,
                const double x,
                const double y,
                const double pressure,
                const int which)
{
  dt_lib_snapshots_t *d = self->data;

  // if panning, do not handle here, let darkroom do the job
  if(d->panning) return 0;

  // an arrow over the split line or the boundary of the halves of a compared step
  if(!d->separator_pressed && !d->dragging)
  {
    const gboolean over = _compare_history_end(d) >= 0
      && (d->sidebyside
          ? _on_sidebyside_separator(d, x, y, darktable.gui->pointer_in_snapshot)
          : _near_split_line(d, x, y));
    // an arrow across the split: left/right for a vertical one
    const char *cursor = over ? d->vertical ? "ew-resize" : "ns-resize" : NULL;
    if(cursor != d->cursor)
    {
      d->cursor = cursor;
      dt_control_change_cursor(cursor ? cursor : "default");
    }
  }

  if(d->separator_pressed)
  {
    if(fabs(x - d->separator_x) + fabs(y - d->separator_y) > DT_PIXEL_APPLY_DPI(4))
      d->separator_dragged = TRUE;
    return 1;
  }

  // nothing to move unless the split is being dragged
  if(_compare_history_end(d) >= 0 && (d->sidebyside || !d->dragging))
  {
    if(!d->sidebyside
       && _get_rotation_area(self, x, y, NULL, NULL, NULL) != d->rotsym_lightup)
      dt_control_queue_redraw_center();
    return 0;
  }

  if(d->selected >= 0)
  {
    const double xp = x / d->vp_width;
    const double yp = y / d->vp_height;

    if(d->pending_click
       && fabs(xp - d->press_xp) * d->vp_width + fabs(yp - d->press_yp) * d->vp_height
          > DT_PIXEL_APPLY_DPI(4))
      d->pending_click = FALSE;

    /* update x pointer */
    if(d->dragging)
    {
      d->vp_xpointer = xp;
      d->vp_ypointer = yp;
    }

    // Here to ensure the rotation symbol is light-up or light-off
    // when moving close.
    const gboolean display_rotation = _get_rotation_area(self, x, y, NULL, NULL, NULL);

    if(d->dragging || display_rotation != d->rotsym_lightup)
      dt_control_queue_redraw_center();

    return 1;
  }

  return 0;
}

static void _lib_snapshots_toggle_last(dt_action_t *action)
{
  dt_lib_snapshots_t *d = dt_action_lib(action)->data;

  const int32_t index = d->num_snapshots - 1;

  if(d->num_snapshots)
    gtk_toggle_button_set_active
      (GTK_TOGGLE_BUTTON(d->snapshot[index].button),
       !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->snapshot[index].button)));
}

static int _look_for_widget(dt_lib_module_t *self, GtkWidget *widget, gboolean entry)
{
  dt_lib_snapshots_t *d = self->data;

  for(int k=0; k<MAX_SNAPSHOT; k++)
  {
    if((entry ? d->snapshot[k].entry : d->snapshot[k].button) == widget)
      return k;
  }

  return 0;
}

static void _entry_activated_callback(GtkEntry *entry, dt_lib_module_t *self)
{
  dt_lib_snapshots_t *d = self->data;

  const int index = _look_for_widget(self, (GtkWidget *)entry, TRUE);

  const char *txt = gtk_entry_get_text(GTK_ENTRY(d->snapshot[index].entry));

  char *label = dt_history_get_name_label(d->snapshot[index].module, txt, TRUE, TRUE);
  gtk_label_set_markup(GTK_LABEL(d->snapshot[index].name), label);
  g_free(label);

  gtk_widget_hide(d->snapshot[index].entry);
  gtk_widget_show(d->snapshot[index].name);
  gtk_widget_grab_focus(d->snapshot[index].button);
}

static void _lib_button_button_pressed_callback(GtkGestureSingle *gesture,
                                                gint n_press,
                                                gdouble x,
                                                gdouble y,
                                                dt_lib_module_t *self)
{
  dt_lib_snapshots_t *d = self->data;
  GtkWidget *widget = dt_gui_get_widget(gesture);

  const int index = _look_for_widget(self, widget, FALSE);

  if(dt_key_modifier_state() & GDK_CONTROL_MASK)
  {
    gtk_widget_hide(d->snapshot[index].name);
    gtk_widget_show(d->snapshot[index].entry);
    gtk_widget_grab_focus(d->snapshot[index].entry);
  }

  gtk_widget_set_focus_on_click(widget, FALSE);
}

static void _init_snapshot_entry(dt_lib_module_t *self,
                                 dt_lib_snapshot_t *s)
{
  /* create snapshot button */
  s->button = gtk_toggle_button_new();
  gtk_widget_set_name(s->button, "snapshot-button");
  g_signal_connect(G_OBJECT(s->button), "toggled",
                   G_CALLBACK(_lib_snapshots_toggled_callback), self);
  dt_gui_connect_click(s->button, _lib_button_button_pressed_callback, NULL, self);

  s->num = gtk_label_new("");
  gtk_widget_set_name(s->num, "history-number");
  dt_gui_add_class(s->num, "dt_monospace");

  s->status = gtk_label_new("");
  dt_gui_add_class(s->status, "dt_monospace");

  s->name = gtk_label_new("");
  gtk_label_set_ellipsize(GTK_LABEL(s->name), PANGO_ELLIPSIZE_MIDDLE);
  gtk_widget_set_halign(s->name, GTK_ALIGN_START);

  s->entry = gtk_entry_new();
  gtk_widget_set_halign(s->entry, GTK_ALIGN_FILL);
  g_signal_connect(G_OBJECT(s->entry), "activate",
                   G_CALLBACK(_entry_activated_callback), self);

  s->restore_button = dtgtk_button_new_full(dtgtk_cairo_paint_snapshots_restore, CPF_NONE, NULL,
      &(dtgtk_button_config_t){
        .tooltip = _("restore snapshot into current history"),
        .clicked_cb = G_CALLBACK(_lib_snapshots_restore_callback),
        .clicked_data = self,
      });
  gtk_widget_set_name(s->restore_button, "non-flat");
}

static void _clear_snapshot_entry(dt_lib_snapshot_t *s)
{
  // delete corresponding entry from the database

  dt_history_snapshot_clear(s->imgid, s->id);

  s->ctx = 0;
  s->imgid = NO_IMGID;
  s->history_end = -1;

  if(s->button)
  {
    GtkWidget *lstatus = _lib_snapshot_button_get_item(s->button, _SNAPSHOT_BUTTON_STATUS);
    gtk_widget_set_tooltip_text(s->button, "");
    gtk_widget_set_tooltip_text(lstatus, "");
    gtk_widget_hide(s->button);
    gtk_widget_hide(s->restore_button);
  }

  g_free(s->module);
  g_free(s->label);
  dt_frame_unref(s->frame);
  s->module = NULL;
  s->label = NULL;
  s->frame = NULL;
}

static void _clear_snapshots(dt_lib_module_t *self)
{
  dt_lib_snapshots_t *d = self->data;
  d->selected = -1;
  darktable.lib->proxy.snapshots.enabled = FALSE;
  d->snap_requested = FALSE;

  for(uint32_t k = 0; k < d->num_snapshots; k++)
  {
    dt_lib_snapshot_t *s = &d->snapshot[k];
    _clear_snapshot_entry(s);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(s->button), FALSE);
  }

  d->num_snapshots = 0;
  d->compare_id = 0;
  gtk_widget_set_sensitive(d->take_button, TRUE);

  dt_control_queue_redraw_center();
  _compare_changed();
}

void gui_reset(dt_lib_module_t *self)
{
  _clear_snapshots(self);
}

static void _remove_snapshot_entry(dt_lib_module_t *self, const uint32_t index)
{
  dt_lib_snapshots_t *d = self->data;

  // the entries after index move down one slot: keep the selection on the
  // same snapshot, or end the comparison if it is the one removed. A stale
  // index points at the next snapshot or, past the last one, at an empty
  // slot, which gui_post_expose() then renders as image NO_IMGID
  if(d->selected == (int)index)
  {
    d->selected = -1;
    darktable.lib->proxy.snapshots.enabled = FALSE;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->snapshot[index].button), FALSE);
  }
  else if(d->selected > (int)index)
    d->selected--;

  if(d->snapshot[index].id == d->compare_id) d->compare_id = 0;

  //  First clean the entry
  _clear_snapshot_entry(&d->snapshot[index]);

  // move the later entries down, and reuse the cleared entry, widgets
  // included, as the free last slot. Clearing the old last slot after the
  // move instead would free the data, and delete the stored history, of
  // the snapshot copied down from it
  const dt_lib_snapshot_t removed = d->snapshot[index];
  memmove(&d->snapshot[index], &d->snapshot[index + 1],
          (MAX_SNAPSHOT - 1 - index) * sizeof(dt_lib_snapshot_t));
  d->snapshot[MAX_SNAPSHOT - 1] = removed;

  // the reused row is still shown where the removed snapshot was. Rows are
  // packed from the end, so the last child is at the top, where the newest
  // snapshot belongs
  gtk_box_reorder_child(GTK_BOX(d->snapshots_box), removed.bbox, -1);

  //  We have one less snapshot
  d->num_snapshots--;

  // taking the tenth snapshot disabled the button, and a slot is free again
  gtk_widget_set_sensitive(d->take_button, TRUE);

  _compare_changed();
}

static void _signal_image_removed(gpointer instance,
                                  const dt_imgid_t imgid,
                                  dt_lib_module_t *self)
{
  dt_lib_snapshots_t *d = self->data;

  uint32_t k = 0;

  while(k < MAX_SNAPSHOT)
  {
    dt_lib_snapshot_t *s = &d->snapshot[k];

    if(s->imgid == imgid)
    {
      _remove_snapshot_entry(self, k);
      dt_control_log(_("snapshots for removed image have been deleted"));
    }
    else
      k++;
  }
}

static void _signal_image_changed(gpointer instance,
                                  dt_lib_module_t *self)
{
  dt_lib_snapshots_t *d = self->data;

  const dt_imgid_t imgid = darktable.develop->image_storage.id;

  for(uint32_t k = 0; k < MAX_SNAPSHOT; k++)
  {
    dt_lib_snapshot_t *s = &d->snapshot[k];

    if(!dt_is_valid_imgid(s->imgid))
      continue;

    GtkWidget *b = d->snapshot[k].button;
    GtkWidget *st = _lib_snapshot_button_get_item(b, _SNAPSHOT_BUTTON_STATUS);

    char stat[8] = { 0 };

    if(s->imgid == imgid)
    {
      g_strlcpy(stat, " ", sizeof(stat));

      gtk_widget_set_tooltip_text(b, "");
      gtk_widget_set_tooltip_text(st, "");
    }
    else
    {
      g_strlcpy(stat, "↗", sizeof(stat));

      char tooltip[128] = { 0 };
      // tooltip
      char *name = dt_image_get_filename(s->imgid);
      snprintf(tooltip, sizeof(tooltip),
               _("↗ %s '%s'"), _("this snapshot was taken from"), name);
      g_free(name);
      gtk_widget_set_tooltip_text(b, tooltip);
      gtk_widget_set_tooltip_text(st, tooltip);
    }

    gtk_label_set_text(GTK_LABEL(st), stat);
  }

  dt_control_queue_redraw_center();
}

static void _sidebyside_button_clicked(GtkWidget *widget, dt_lib_module_t *self)
{
  dt_lib_snapshots_t *d = self->data;

  d->sidebyside = !d->sidebyside;
  d->snap_requested = TRUE;
  gtk_widget_set_visible(dt_ui_snapshot(darktable.gui->ui),
                         d->sidebyside && d->selected >= 0);
}

void gui_init(dt_lib_module_t *self)
{
  /* initialize ui widgets */
  dt_lib_snapshots_t *d = g_malloc0(sizeof(dt_lib_snapshots_t));
  self->data = (void *)d;

  /* initialize snapshot storages */
  d->vp_xpointer = 0.5;
  d->vp_ypointer = 0.5;
  d->vp_xrotate = 0.0;
  d->vp_yrotate = 0.0;
  d->vertical = TRUE;
  d->on_going = FALSE;
  d->rotsym_lightup = FALSE;
  d->panning = FALSE;
  d->selected = -1;
  d->snap_requested = FALSE;
  d->expose_again_timeout_id = 0;
  d->num_snapshots = 0;
  darktable.lib->proxy.snapshots.enabled = FALSE;
  darktable.lib->proxy.snapshots.module = self;
  darktable.lib->proxy.snapshots.compare_history = _compare_history;
  darktable.lib->proxy.snapshots.compared_history_end = _compare_get;
  darktable.lib->proxy.snapshots.set_sidebyside = _compare_set_sidebyside;
  darktable.lib->proxy.snapshots.get_sidebyside = _compare_get_sidebyside;
  darktable.lib->proxy.snapshots.rotate_layout = _compare_rotate;

  /* initialize ui containers */
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  d->snapshots_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  /* create take snapshot button */
  d->take_button = dt_action_button_new
    (self,
     N_("take snapshot"),
     _lib_snapshots_add_button_clicked_callback, self,
     _("take snapshot to compare with another image "
       "or the same image at another stage of development"), 0, 0);

  /*
   * initialize snapshots
   */
  char localtmpdir[PATH_MAX] = { 0 };
  dt_loc_get_tmp_dir(localtmpdir, sizeof(localtmpdir));

  for(int k = 0; k < MAX_SNAPSHOT; k++)
  {
    dt_lib_snapshot_t *s = &d->snapshot[k];
    // each entry keeps this id for the whole session, as it moves between
    // slots in _remove_snapshot_entry(). The stored history of a snapshot is
    // read by id alone, so two entries must never share one
    s->id = SNAPSHOT_ID_OFFSET | k;

    _clear_snapshot_entry(s);
    _init_snapshot_entry(self, s);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

    // 4 items inside box, num, status, name, label

    gtk_box_pack_start(GTK_BOX(box), s->num, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), s->status, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), s->name, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), s->entry, TRUE, TRUE, 0);

    gtk_widget_show_all(box);

    // hide entry, will be used only when editing
    gtk_widget_hide(s->entry);

    gtk_container_add(GTK_CONTAINER(s->button), box);

    // add snap button and restore button
    s->bbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(s->bbox), s->button, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(s->bbox), s->restore_button, FALSE, FALSE, 0);

    /* add button to snapshot box */
    gtk_box_pack_end(GTK_BOX(d->snapshots_box), s->bbox, FALSE, FALSE, 0);

    /* prevent widget to show on external show all */
    gtk_widget_set_no_show_all(s->button, TRUE);
    gtk_widget_set_no_show_all(s->restore_button, TRUE);
  }

  /* add snapshot box and take snapshot button to widget ui*/
  gtk_box_pack_start(GTK_BOX(self->widget),
                     dt_ui_resize_wrap(d->snapshots_box, 1,
                                       "plugins/darkroom/snapshots/windowheight"),
                     TRUE, TRUE, 0);

  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_start(GTK_BOX(hbox), d->take_button, TRUE, TRUE, 0);
  d->sidebyside_button = dtgtk_togglebutton_new_full(dtgtk_cairo_paint_lt_mode_culling_dynamic, 0, NULL,
      &(dtgtk_button_config_t){
        .tooltip = _("place the snapshot side-by-side / above-below the current image instead of overlaying"),
        .action = DT_ACTION(self),
        .action_label = N_("side-by-side"),
        .action_def = &dt_action_def_toggle,
        .clicked_cb = G_CALLBACK(_sidebyside_button_clicked),
        .clicked_data = self,
      });
  gtk_box_pack_start(GTK_BOX(hbox), d->sidebyside_button, FALSE, TRUE, 0);

  gtk_box_pack_start(GTK_BOX(self->widget), hbox, TRUE, TRUE, 0);

  dt_action_register(DT_ACTION(self), N_("toggle last snapshot"),
                     _lib_snapshots_toggle_last, 0, 0);

  DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_DEVELOP_IMAGE_CHANGED, _signal_image_changed);
  DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_IMAGE_REMOVED, _signal_image_removed);
}

void gui_cleanup(dt_lib_module_t *self)
{
  _clear_snapshots(self);
  darktable.lib->proxy.snapshots.module = NULL;
  darktable.lib->proxy.snapshots.compare_history = NULL;
  darktable.lib->proxy.snapshots.compared_history_end = NULL;
  darktable.lib->proxy.snapshots.set_sidebyside = NULL;
  darktable.lib->proxy.snapshots.get_sidebyside = NULL;
  darktable.lib->proxy.snapshots.rotate_layout = NULL;

  g_free(self->data);
  self->data = NULL;
}

// snapshot the edit as it was after history_end items
static void _take_snapshot(dt_lib_module_t *self,
                           const int history_end)
{
  dt_lib_snapshots_t *d = self->data;

  // once every slot is used the take button is made insensitive (see the end
  // of this function), but lua_take_snapshot() calls this directly, where
  // sensitivity means nothing: the snapshot below would be written one slot
  // past the end of d->snapshot, and the garbage found there dereferenced as
  // a widget
  if(d->num_snapshots >= MAX_SNAPSHOT)
  {
    dt_control_log(_("all %d snapshot slots are in use"), MAX_SNAPSHOT);
    return;
  }

  // first make sure the current history is properly written
  dt_dev_write_history(darktable.develop);

  dt_lib_snapshot_t *s = &d->snapshot[d->num_snapshots];

  _clear_snapshot_entry(s);

  if(history_end > 0)
  {
    dt_dev_history_item_t *history_item =
      g_list_nth_data(darktable.develop->history, history_end - 1);
    if(history_item && history_item->module)
    {
      s->module = g_strdup(history_item->module->name());

      if(strlen(history_item->multi_name) > 0
         && history_item->multi_name[0] != ' ')
      {
        s->label = history_item->multi_name_hand_edited
          ? g_strdup(history_item->multi_name)
          : dt_util_localize_segmented_name(history_item->multi_name, TRUE);
      }
    }
    else
      s->module = g_strdup(_("unknown"));
  }
  else
    s->module = g_strdup(_("original"));

  s->history_end = history_end;
  s->imgid = darktable.develop->image_storage.id;

  dt_history_snapshot_create(s->imgid, s->id, s->history_end);

  GtkLabel *lnum =
    (GtkLabel *)_lib_snapshot_button_get_item(s->button, _SNAPSHOT_BUTTON_NUM);
  GtkLabel *lstatus =
    (GtkLabel *)_lib_snapshot_button_get_item(s->button, _SNAPSHOT_BUTTON_STATUS);
  GtkLabel *lname =
    (GtkLabel *)_lib_snapshot_button_get_item(s->button, _SNAPSHOT_BUTTON_NAME);
  GtkEntry *lentry =
    (GtkEntry *)_lib_snapshot_button_get_item(s->button, _SNAPSHOT_BUTTON_ENTRY);

  char num[8];
  g_snprintf(num, sizeof(num), "%2u", s->history_end);

  gtk_label_set_text(lnum, num);
  gtk_label_set_text(lstatus, " ");

  char *txt = dt_history_get_name_label(s->module, s->label, TRUE, TRUE);
  gtk_label_set_markup(lname, txt);

  gtk_entry_set_text(lentry, s->label ? s->label : "");

  g_free(txt);

  /* update slots used */
  d->num_snapshots++;

  /* show active snapshot slots */
  for(uint32_t k = 0; k < d->num_snapshots; k++)
  {
    gtk_widget_show(d->snapshot[k].button);
    gtk_widget_show(d->snapshot[k].restore_button);
  }

  // a snapshot taken while the module is collapsed (from a shortcut or a Lua
  // script) must not move the keyboard focus into it: the window would keep a
  // focus widget that is not realized, and every key press would be swallowed
  // by it instead of reaching the shortcuts
  if(gtk_widget_get_mapped(s->button))
    gtk_widget_grab_focus(s->button);

  if(d->num_snapshots == MAX_SNAPSHOT)
    gtk_widget_set_sensitive(d->take_button, FALSE);
}

static void _lib_snapshots_add_button_clicked_callback(GtkWidget *widget,
                                                       dt_lib_module_t *self)
{
  _take_snapshot(self, darktable.develop->history_end);
}

// compare the edit as it was after history_end items with the live edit, which
// stays editable; the same entry again ends the comparison
static void _compare_history(dt_lib_module_t *self,
                             const int history_end)
{
  dt_lib_snapshots_t *d = self->data;

  const int shown = _compare_history_end(d);

  if(d->compare_id)
  {
    for(uint32_t k = 0; k < d->num_snapshots; k++)
      if(d->snapshot[k].id == d->compare_id)
      {
        _remove_snapshot_entry(self, k);
        break;
      }
    d->compare_id = 0;
  }

  if(shown == history_end)
  {
    dt_control_queue_redraw_center();
    return;
  }

  if(d->num_snapshots >= MAX_SNAPSHOT)
  {
    dt_control_log(_("all %d snapshot slots are in use"), MAX_SNAPSHOT);
    return;
  }

  _take_snapshot(self, history_end);
  d->compare_id = d->snapshot[d->num_snapshots - 1].id;

  // side-by-side is the way to start, but whatever the user picks after that
  // stays. The toggled callback below reads d->sidebyside, so set it first
  if(shown < 0) _compare_set_sidebyside(self, TRUE);

  gtk_toggle_button_set_active
    (GTK_TOGGLE_BUTTON(d->snapshot[d->num_snapshots - 1].button), TRUE);
}

static int _compare_get(dt_lib_module_t *self)
{
  return _compare_history_end(self->data);
}

static void _compare_set_sidebyside(dt_lib_module_t *self,
                                    const gboolean sidebyside)
{
  dt_lib_snapshots_t *d = self->data;
  if(d->sidebyside == sidebyside) return;

  // the toggle follows without its click handler, which flips the mode and
  // is called here once instead
  g_signal_handlers_block_matched(d->sidebyside_button, G_SIGNAL_MATCH_FUNC | G_SIGNAL_MATCH_DATA,
                                  0, 0, NULL, _sidebyside_button_clicked, self);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->sidebyside_button), sidebyside);
  g_signal_handlers_unblock_matched(d->sidebyside_button, G_SIGNAL_MATCH_FUNC | G_SIGNAL_MATCH_DATA,
                                    0, 0, NULL, _sidebyside_button_clicked, self);
  _sidebyside_button_clicked(NULL, self);
  dt_control_queue_redraw_center();
  _compare_changed();
}

// turn a left/right split into a top/bottom one and back
static void _compare_rotate(dt_lib_module_t *self)
{
  dt_lib_snapshots_t *d = self->data;

  d->vertical = !d->vertical;
  gtk_orientable_set_orientation(GTK_ORIENTABLE(gtk_widget_get_parent(dt_ui_snapshot(darktable.gui->ui))),
                                 d->vertical ? GTK_ORIENTATION_HORIZONTAL : GTK_ORIENTATION_VERTICAL);
  d->snap_requested = TRUE;
  dt_control_queue_redraw_center();
  _compare_changed();
}

static gboolean _compare_get_sidebyside(dt_lib_module_t *self)
{
  const dt_lib_snapshots_t *d = self->data;
  return d->sidebyside;
}

static int _lib_snapshots_get_activated(dt_lib_module_t *self, GtkWidget *widget)
{
  dt_lib_snapshots_t *d = self->data;

  for(uint32_t k = 0; k < d->num_snapshots; k++)
    if(widget == d->snapshot[k].button
       || widget == d->snapshot[k].restore_button)
      return k;

  return -1;
}

static void _lib_snapshots_toggled_callback(GtkToggleButton *widget,
                                            dt_lib_module_t *self)
{
  dt_lib_snapshots_t *d = self->data;

  DT_TRY_GUI_UPDATE();

  d->selected = -1;

  /* check if snapshot is activated */
  if(gtk_toggle_button_get_active(widget))
  {
    d->selected = _lib_snapshots_get_activated(self, GTK_WIDGET(widget));

    /* lets deactivate all togglebuttons except for self */
    for(uint32_t k = 0; k < d->num_snapshots; k++)
      if(d->selected != k)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->snapshot[k].button), FALSE);
  }
  // a compared history step leaves the live edit editable, handles included
  darktable.lib->proxy.snapshots.enabled = d->selected >= 0 && _compare_history_end(d) < 0;
  gtk_widget_set_visible(dt_ui_snapshot(darktable.gui->ui), d->sidebyside && d->selected >= 0);

  DT_LEAVE_GUI_UPDATE();

  /* redraw center view */
  dt_control_queue_redraw_center();
  _compare_changed();
}

static void _lib_snapshots_restore_callback(GtkButton *widget,
                                            dt_lib_module_t *self)
{
  dt_lib_snapshots_t *d = self->data;

  const int restore_idx = _lib_snapshots_get_activated(self, GTK_WIDGET(widget));

  dt_lib_snapshot_t *s = &d->snapshot[restore_idx];

  const dt_imgid_t imgid = darktable.develop->image_storage.id;

  dt_history_snapshot_restore(s->imgid, imgid, s->id, s->history_end);

  dt_dev_undo_start_record(darktable.develop);

  // reload history and set back snapshot history end
  dt_dev_reload_history_items(darktable.develop);

  dt_dev_pixelpipe_rebuild(darktable.develop);
  darktable.develop->history_end = s->history_end;
  dt_dev_pop_history_items(darktable.develop, darktable.develop->history_end);
  dt_ioppr_resync_modules_order(darktable.develop);
  dt_dev_modulegroups_set(darktable.develop,
                          dt_dev_modulegroups_get(darktable.develop));
  dt_image_update_final_size(imgid);
  dt_dev_write_history(darktable.develop);

  /* signal history changed */
  dt_dev_undo_end_record(darktable.develop);
}

#ifdef USE_LUA
typedef enum
{
  SNS_LEFT,
  SNS_RIGHT,
  SNS_TOP,
  SNS_BOTTOM,
} snapshot_direction_t;

static int direction_member(lua_State *L)
{
  dt_lib_module_t *self = *(dt_lib_module_t **)lua_touserdata(L, 1);
  dt_lib_snapshots_t *d = self->data;
  if(lua_gettop(L) != 3)
  {
    snapshot_direction_t result;
    if(!d->vertical && !d->inverted)
    {
      result = SNS_TOP;
    }
    else if(!d->vertical && d->inverted)
    {
      result = SNS_BOTTOM;
    }
    else if(d->vertical && !d->inverted)
    {
      result = SNS_LEFT;
    }
    else
    {
      result = SNS_RIGHT;
    }
    luaA_push(L, snapshot_direction_t, &result);
    return 1;
  }
  else
  {
    snapshot_direction_t direction;
    luaA_to(L, snapshot_direction_t, &direction, 3);
    if(direction == SNS_TOP)
    {
      d->vertical = FALSE;
      d->inverted = FALSE;
    }
    else if(direction == SNS_BOTTOM)
    {
      d->vertical = FALSE;
      d->inverted = TRUE;
    }
    else if(direction == SNS_LEFT)
    {
      d->vertical = TRUE;
      d->inverted = FALSE;
    }
    else
    {
      d->vertical = TRUE;
      d->inverted = TRUE;
    }
    dt_control_queue_redraw_center();
    return 0;
  }
}

static int ratio_member(lua_State *L)
{
  dt_lib_module_t *self = *(dt_lib_module_t **)lua_touserdata(L, 1);
  dt_lib_snapshots_t *d = self->data;
  if(lua_gettop(L) != 3)
  {
    if(!d->vertical && !d->inverted)
    {
      lua_pushnumber(L, d->vp_ypointer);
    }
    else if(!d->vertical && d->inverted)
    {
      lua_pushnumber(L, 1 - d->vp_ypointer);
    }
    else if(d->vertical && !d->inverted)
    {
      lua_pushnumber(L, d->vp_xpointer);
    }
    else
    {
      lua_pushnumber(L, 1 - d->vp_xpointer);
    }
    return 1;
  }
  else
  {
    double ratio;
    luaA_to(L, double, &ratio, 3);
    if(ratio < 0.0) ratio = 0.0;
    if(ratio > 1.0) ratio = 1.0;
    if(!d->vertical && !d->inverted)
    {
      d->vp_ypointer = ratio;
    }
    else if(!d->vertical && d->inverted)
    {
      d->vp_ypointer = 1.0 - ratio;
    }
    else if(d->vertical && !d->inverted)
    {
      d->vp_xpointer = ratio;
    }
    else
    {
      d->vp_xpointer = 1.0 - ratio;
    }
    dt_control_queue_redraw_center();
    return 0;
  }
}

static int max_snapshot_member(lua_State *L)
{
  lua_pushinteger(L, MAX_SNAPSHOT);
  return 1;
}

static int lua_take_snapshot(lua_State *L)
{
  dt_lib_module_t *self = lua_touserdata(L, lua_upvalueindex(1));
  dt_lib_snapshots_t *d = self->data;
  _lib_snapshots_add_button_clicked_callback(d->take_button, self);
  return 0;
}

static int lua_clear_snapshots(lua_State *L)
{
  dt_lib_module_t *self = lua_touserdata(L, lua_upvalueindex(1));
  gui_reset(self);
  return 0;
}

typedef int dt_lua_snapshot_t;
static int selected_member(lua_State *L)
{
  dt_lib_module_t *self = *(dt_lib_module_t **)lua_touserdata(L, 1);
  dt_lib_snapshots_t *d = self->data;
  for(int i = 0; i < d->num_snapshots; i++)
  {
    GtkWidget *widget = d->snapshot[i].button;
    if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget)))
    {
      luaA_push(L, dt_lua_snapshot_t, &i);
      return 1;
    }
  }
  lua_pushnil(L);
  return 1;
}

static int snapshots_length(lua_State *L)
{
  dt_lib_module_t *self = *(dt_lib_module_t **)lua_touserdata(L, 1);
  dt_lib_snapshots_t *d = self->data;
  lua_pushinteger(L, d->num_snapshots);
  return 1;
}

static int number_member(lua_State *L)
{
  dt_lib_module_t *self = *(dt_lib_module_t **)lua_touserdata(L, 1);
  dt_lib_snapshots_t *d = self->data;
  int index = luaL_checkinteger(L, 2);
  if( index < 1)
  {
    return luaL_error(L, "Accessing a non-existent snapshot");
  }else if(index > d->num_snapshots ) {
    lua_pushnil(L);
    return 1;
  }
  index = index - 1;
  luaA_push(L, dt_lua_snapshot_t, &index);
  return 1;
}

static int name_member(lua_State *L)
{
  dt_lua_snapshot_t index;
  luaA_to(L, dt_lua_snapshot_t, &index, 1);
  dt_lib_module_t *module = lua_touserdata(L, lua_upvalueindex(1));
  dt_lib_snapshots_t *d = module->data;
  if(index >= d->num_snapshots || index < 0)
  {
    return luaL_error(L, "Accessing a non-existent snapshot");
  }
  GtkLabel *num = (GtkLabel *)_lib_snapshot_button_get_item(d->snapshot[index].button,
                                                            _SNAPSHOT_BUTTON_NUM);
  GtkLabel *name = (GtkLabel *)_lib_snapshot_button_get_item(d->snapshot[index].button,
                                                             _SNAPSHOT_BUTTON_NAME);

  // skip first space if present
  char *n = (char *)gtk_label_get_text(num);
  if(*n == ' ') n++;

  char *value = g_strdup_printf("%s (%s)",
                                gtk_label_get_text(name),
                                n);
  lua_pushstring (L, value);

  g_free(value);
  return 1;
}

static int remove_member(lua_State *L)
{
  dt_lua_snapshot_t index;
  luaA_to(L, dt_lua_snapshot_t, &index, 1);
  dt_lib_module_t *module = lua_touserdata(L, lua_upvalueindex(1));
  dt_lib_snapshots_t *d = module->data;
  if(index >= d->num_snapshots || index < 0)
  {
    return luaL_error(L, "Accessing a non-existent snapshot");
  }
  _remove_snapshot_entry(module, index);
  return 0;
}

static int lua_select(lua_State *L)
{
  dt_lua_snapshot_t index;
  luaA_to(L, dt_lua_snapshot_t, &index, 1);
  dt_lib_module_t *module = lua_touserdata(L, lua_upvalueindex(1));
  dt_lib_snapshots_t *d = module->data;
  if(index >= d->num_snapshots || index < 0)
  {
    return luaL_error(L, "Accessing a non-existent snapshot");
  }
  dt_lib_snapshot_t *self = &d->snapshot[index];
  gtk_button_clicked(GTK_BUTTON(self->button));
  return 0;
}

// selected : boolean r/w

void init(struct dt_lib_module_t *self)
{
  lua_State *L = darktable.lua_state.state;
  int my_type = dt_lua_module_entry_get_type(L, "lib", self->plugin_name);
  lua_pushcfunction(L, direction_member);
  dt_lua_type_register_type(L, my_type, "direction");
  lua_pushcfunction(L, ratio_member);
  dt_lua_type_register_type(L, my_type, "ratio");
  lua_pushcfunction(L, max_snapshot_member);
  dt_lua_type_register_const_type(L, my_type, "max_snapshot");
  lua_pushlightuserdata(L, self);
  lua_pushcclosure(L, lua_take_snapshot, 1);
  dt_lua_gtk_wrap(L);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, my_type, "take_snapshot");
  lua_pushlightuserdata(L, self);
  lua_pushcclosure(L, lua_clear_snapshots, 1);
  dt_lua_gtk_wrap(L);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, my_type, "clear_snapshots");
  lua_pushcfunction(L, snapshots_length);
  lua_pushcfunction(L, number_member);
  dt_lua_type_register_number_const_type(L, my_type);
  lua_pushcfunction(L, selected_member);
  dt_lua_gtk_wrap(L);
  dt_lua_type_register_const_type(L, my_type, "selected");

  dt_lua_init_int_type(L, dt_lua_snapshot_t);
  lua_pushlightuserdata(L, self);
  lua_pushcclosure(L, name_member, 1);
  dt_lua_gtk_wrap(L);
  dt_lua_type_register_const(L, dt_lua_snapshot_t, "name");
  lua_pushlightuserdata(L, self);
  lua_pushcclosure(L, remove_member, 1);
  dt_lua_gtk_wrap(L);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const(L, dt_lua_snapshot_t, "remove");
  lua_pushlightuserdata(L, self);
  lua_pushcclosure(L, lua_select, 1);
  dt_lua_gtk_wrap(L);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const(L, dt_lua_snapshot_t, "select");

  lua_pushlightuserdata(L, self);
  lua_pushcclosure(L, name_member, 1);
  dt_lua_gtk_wrap(L);
  dt_lua_type_setmetafield(L,dt_lua_snapshot_t,"__tostring");

  luaA_enum(L, snapshot_direction_t);
  luaA_enum_value_name(L, snapshot_direction_t, SNS_LEFT, "left");
  luaA_enum_value_name(L, snapshot_direction_t, SNS_RIGHT, "right");
  luaA_enum_value_name(L, snapshot_direction_t, SNS_TOP, "top");
  luaA_enum_value_name(L, snapshot_direction_t, SNS_BOTTOM, "bottom");
}
#endif // USE_LUA

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
