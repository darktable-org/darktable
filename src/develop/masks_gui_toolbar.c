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

// the flexi masks panel's toolbar: the add buttons and a presets button, on
// one line when the panel is wide enough, and on two or three rows otherwise.
//
//   one line:   [ group | shapes | channels | import ] gap [presets]
//   two rows:   [ group | shapes ] gap [presets]
//               [ channels | import ]
//   three rows: [ shapes ] gap [presets]
//               [ channels ]
//               [ group | import ]
//
// Each row is centered on the full width, the first pushed left only as far
// as it must be to clear the presets button, which keeps to the right edge.
// The third arrangement is what lets the panel fit the narrowest side panel:
// the shape and channel runs are the widest things in it, so the two buttons
// that would otherwise lengthen them go to a row of their own.
//
// a height-for-width container, not a box rearranged from "size-allocate":
// the row count is decided in GTK's measure and allocate passes, so nothing
// is reparented, shown, hidden or resized during layout, which would race it

#include "develop/blend_gui_internal.h"

typedef enum
{
  _TB_GROUP = 0,
  _TB_SHAPES,
  _TB_CHANNELS,
  _TB_IMPORT,
  _TB_PRESETS,
  _TB_GAP, // never drawn, only measured: its CSS width is the spacing
  _TB_N
} _tb_slot_t;

typedef struct
{
  GtkContainer parent;
  GtkWidget *child[_TB_N];
} DtMasksToolbar;

typedef struct
{
  GtkContainerClass parent_class;
} DtMasksToolbarClass;

G_DEFINE_TYPE(DtMasksToolbar, _masks_toolbar, GTK_TYPE_CONTAINER)

#define _TB(w) ((DtMasksToolbar *)(w))

// the arrangements, widest first. Each row lists its slots left to right, -1
// terminated; the presets button closes the first row
#define _TB_ROWS 3
#define _TB_LAYOUTS 3
static const int _tb_layouts[_TB_LAYOUTS][_TB_ROWS][_TB_N] =
{
  { { _TB_GROUP, _TB_SHAPES, _TB_CHANNELS, _TB_IMPORT, -1 }, { -1 }, { -1 } },
  { { _TB_GROUP, _TB_SHAPES, -1 }, { _TB_CHANNELS, _TB_IMPORT, -1 }, { -1 } },
  { { _TB_SHAPES, -1 }, { _TB_CHANNELS, -1 }, { _TB_GROUP, _TB_IMPORT, -1 } },
};

// natural size of a slot, zero when it is empty or hidden
typedef struct
{
  int w[_TB_N], h[_TB_N];
} _tb_sizes_t;

static void _tb_measure(DtMasksToolbar *tb, _tb_sizes_t *s)
{
  for(int i = 0; i < _TB_N; i++)
  {
    GtkWidget *c = tb->child[i];
    s->w[i] = s->h[i] = 0;
    if(!c || !gtk_widget_get_visible(c)) continue;
    gtk_widget_get_preferred_width(c, NULL, &s->w[i]);
    gtk_widget_get_preferred_height(c, NULL, &s->h[i]);
  }
}

// the width of a row's run of slots, with a gap between each two shown ones
static int _tb_run_width(const _tb_sizes_t *s, const int *row)
{
  int w = 0;
  for(int k = 0; row[k] >= 0; k++)
    if(s->w[row[k]]) w += (w ? s->w[_TB_GAP] : 0) + s->w[row[k]];
  return w;
}

// the width a row needs, the presets button included on the first
static int _tb_row_width(const _tb_sizes_t *s, const int layout, const int r)
{
  const int run = _tb_run_width(s, _tb_layouts[layout][r]);
  if(r) return run;
  return run + (run && s->w[_TB_PRESETS] ? s->w[_TB_GAP] : 0) + s->w[_TB_PRESETS];
}

static int _tb_row_height(const _tb_sizes_t *s, const int layout, const int r)
{
  const int *row = _tb_layouts[layout][r];
  int h = r ? 0 : s->h[_TB_PRESETS];
  for(int k = 0; row[k] >= 0; k++) h = MAX(h, s->h[row[k]]);
  return h;
}

static int _tb_layout_width(const _tb_sizes_t *s, const int layout)
{
  int w = 0;
  for(int r = 0; r < _TB_ROWS; r++) w = MAX(w, _tb_row_width(s, layout, r));
  return w;
}

static int _tb_layout_height(const _tb_sizes_t *s, const int layout)
{
  int h = 0;
  for(int r = 0; r < _TB_ROWS; r++) h += _tb_row_height(s, layout, r);
  return h;
}

// the widest arrangement that fits, or the narrowest when none does
static int _tb_pick_layout(const _tb_sizes_t *s, const int width)
{
  for(int l = 0; l < _TB_LAYOUTS - 1; l++)
    if(width >= _tb_layout_width(s, l)) return l;
  return _TB_LAYOUTS - 1;
}

static GtkSizeRequestMode _tb_get_request_mode(GtkWidget *widget)
{
  return GTK_SIZE_REQUEST_HEIGHT_FOR_WIDTH;
}

static void _tb_get_preferred_width(GtkWidget *widget, int *minimum, int *natural)
{
  _tb_sizes_t s;
  _tb_measure(_TB(widget), &s);
  *minimum = _tb_layout_width(&s, _TB_LAYOUTS - 1);
  *natural = MAX(*minimum, _tb_layout_width(&s, 0));
}

static void _tb_get_preferred_height_for_width(GtkWidget *widget,
                                               const int width,
                                               int *minimum,
                                               int *natural)
{
  _tb_sizes_t s;
  _tb_measure(_TB(widget), &s);
  *minimum = *natural = _tb_layout_height(&s, _tb_pick_layout(&s, width));
}

// without a width, the height at the minimum width, as GTK expects of a
// height-for-width widget
static void _tb_get_preferred_height(GtkWidget *widget, int *minimum, int *natural)
{
  _tb_sizes_t s;
  _tb_measure(_TB(widget), &s);
  *minimum = *natural = _tb_layout_height(&s, _TB_LAYOUTS - 1);
}

static void _tb_get_preferred_width_for_height(GtkWidget *widget,
                                               const int height,
                                               int *minimum,
                                               int *natural)
{
  _tb_get_preferred_width(widget, minimum, natural);
}

static void _tb_place(GtkWidget *c,
                      const GtkAllocation *a,
                      const gboolean rtl,
                      const int x,
                      const int y,
                      const int w,
                      const int h)
{
  if(!c || !gtk_widget_get_visible(c)) return;
  GtkAllocation ca = { .x = a->x + (rtl ? a->width - x - w : x), .y = a->y + y,
                       .width = w, .height = h };
  gtk_widget_size_allocate(c, &ca);
}

// a run of width w, centered on the full width but kept clear of `limit`
static int _tb_center(const int width, const int w, const int limit)
{
  return MAX(0, MIN((width - w) / 2, limit - w));
}

static void _tb_size_allocate(GtkWidget *widget, GtkAllocation *a)
{
  DtMasksToolbar *tb = _TB(widget);
  gtk_widget_set_allocation(widget, a);

  _tb_sizes_t s;
  _tb_measure(tb, &s);
  const gboolean rtl = gtk_widget_get_direction(widget) == GTK_TEXT_DIR_RTL;
  const int W = a->width;
  const int gap = s.w[_TB_GAP];
  const int wp = s.w[_TB_PRESETS];
  const int layout = _tb_pick_layout(&s, W);

  int y = 0, gap_x = 0;
  for(int r = 0; r < _TB_ROWS; r++)
  {
    const int *row = _tb_layouts[layout][r];
    const int h = _tb_row_height(&s, layout, r);
    const int run = _tb_run_width(&s, row);
    const int limit = !r && wp ? W - wp - gap : W;
    int x = _tb_center(W, run, limit);
    for(int k = 0; row[k] >= 0; k++)
    {
      if(!s.w[row[k]]) continue;
      _tb_place(tb->child[row[k]], a, rtl, x, y, s.w[row[k]], h);
      x += s.w[row[k]] + gap;
      if(!r) gap_x = x - gap;
    }
    if(!r) _tb_place(tb->child[_TB_PRESETS], a, rtl, W - wp, y, wp, h);
    y += h;
  }
  // every visible child gets an allocation; the gap's is just empty space
  _tb_place(tb->child[_TB_GAP], a, rtl, MIN(gap_x, MAX(0, W - gap)), 0, gap,
            _tb_row_height(&s, layout, 0));
}

static void _tb_forall(GtkContainer *container,
                       const gboolean include_internals,
                       GtkCallback callback,
                       gpointer data)
{
  DtMasksToolbar *tb = _TB(container);
  // the callback may remove the child (destroy does), so read each slot afresh
  for(int i = 0; i < _TB_N; i++)
    if(tb->child[i]) callback(tb->child[i], data);
}

static void _tb_remove(GtkContainer *container, GtkWidget *child)
{
  DtMasksToolbar *tb = _TB(container);
  for(int i = 0; i < _TB_N; i++)
  {
    if(tb->child[i] != child) continue;
    const gboolean was_visible = gtk_widget_get_visible(child);
    gtk_widget_unparent(child);
    tb->child[i] = NULL;
    if(was_visible) gtk_widget_queue_resize(GTK_WIDGET(container));
    return;
  }
}

static GType _tb_child_type(GtkContainer *container)
{
  return GTK_TYPE_WIDGET;
}

static void _masks_toolbar_class_init(DtMasksToolbarClass *klass)
{
  GtkWidgetClass *wclass = GTK_WIDGET_CLASS(klass);
  GtkContainerClass *cclass = GTK_CONTAINER_CLASS(klass);

  wclass->get_request_mode = _tb_get_request_mode;
  wclass->get_preferred_width = _tb_get_preferred_width;
  wclass->get_preferred_height = _tb_get_preferred_height;
  wclass->get_preferred_height_for_width = _tb_get_preferred_height_for_width;
  wclass->get_preferred_width_for_height = _tb_get_preferred_width_for_height;
  wclass->size_allocate = _tb_size_allocate;

  cclass->forall = _tb_forall;
  cclass->remove = _tb_remove;
  cclass->child_type = _tb_child_type;

  // styled as a box
  gtk_widget_class_set_css_name(wclass, "box");
}

static void _masks_toolbar_init(DtMasksToolbar *tb)
{
  gtk_widget_set_has_window(GTK_WIDGET(tb), FALSE);
}

GtkWidget *dt_masks_gui_toolbar_new(GtkWidget *group,
                                    GtkWidget *shapes,
                                    GtkWidget *channels,
                                    GtkWidget *import,
                                    GtkWidget *presets,
                                    GtkWidget *gap)
{
  DtMasksToolbar *tb = g_object_new(_masks_toolbar_get_type(), NULL);
  GtkWidget *children[_TB_N] = { group, shapes, channels, import, presets, gap };
  for(int i = 0; i < _TB_N; i++)
  {
    tb->child[i] = children[i];
    gtk_widget_set_parent(children[i], GTK_WIDGET(tb));
  }
  return GTK_WIDGET(tb);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
