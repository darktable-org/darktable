/*
    This file is part of darktable,
    Copyright (C) 2011-2021 darktable developers.

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

#include "common/darktable.h"
#include "bauhaus/bauhaus.h"
#include "common/debug.h"
#include "common/file_location.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/draw.h"
#include "libs/lib.h"
#include "libs/lib_api.h"

DT_MODULE(1)

#define DT_LIB_SNAPSHOTS_COUNT 4

#define HANDLE_SIZE 0.02

/* a snapshot */
typedef struct dt_lib_snapshot_t
{
  GtkWidget *button;
  float zoom_scale;
  uint32_t imgid;
  uint32_t history_end;
  /* snapshot cairo surface */
  cairo_surface_t *surface;
  uint32_t width, height;
} dt_lib_snapshot_t;

typedef struct dt_lib_snapshot_params_t
{
  dt_imageio_module_data_t head;
  int bpp;
  uint8_t *buf;
  uint32_t width, height;
} dt_lib_snapshot_params_t;

typedef struct dt_lib_snapshots_t
{
  GtkWidget *snapshots_box;

  int selected;
  dt_lib_snapshot_params_t params;
  gboolean snap_requested;
  int expose_again_timeout_id;

  /* current active snapshots */
  uint32_t num_snapshots;

  /* size of snapshots */
  uint32_t size;

  /* snapshots */
  dt_lib_snapshot_t *snapshot;

  /* change snapshot overlay controls */
  gboolean dragging, vertical, inverted;
  double vp_width, vp_height, vp_xpointer, vp_ypointer, vp_xrotate, vp_yrotate;
  gboolean on_going;

  GtkWidget *take_button;
} dt_lib_snapshots_t;

/* callback for take snapshot */
static void _lib_snapshots_add_button_clicked_callback(GtkWidget *widget, gpointer user_data);
static void _lib_snapshots_toggled_callback(GtkToggleButton *widget, gpointer user_data);

const char *name(dt_lib_module_t *self)
{
  return _("snapshots");
}

const char **views(dt_lib_module_t *self)
{
  static const char *v[] = {"darkroom", NULL};
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_LEFT_CENTER;
}

int position(const dt_lib_module_t *self)
{
  return 1000;
}

// draw snapshot sign
static void _draw_sym(cairo_t *cr, float x, float y, gboolean vertical, gboolean inverted)
{
  const double inv = inverted ? -0.1 : 1.0;

  PangoRectangle ink;
  PangoFontDescription *desc = pango_font_description_copy_static(darktable.bauhaus->pango_font_desc);
  pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
  pango_font_description_set_absolute_size(desc, DT_PIXEL_APPLY_DPI(12) * PANGO_SCALE);
  PangoLayout *layout = pango_cairo_create_layout(cr);
  pango_layout_set_font_description(layout, desc);
  pango_layout_set_text(layout, C_("snapshot sign", "S"), -1);
  pango_layout_get_pixel_extents(layout, &ink, NULL);

  if(vertical)
    cairo_move_to(cr, x - (inv * ink.width * 1.2f), y - (ink.height / 2.0f) - DT_PIXEL_APPLY_DPI(3));
  else
    cairo_move_to(cr, x - (ink.width / 2.0), y + (-inv * (ink.height * 1.2f) - DT_PIXEL_APPLY_DPI(2)));

  dt_draw_set_color_overlay(cr, FALSE, 0.9);
  pango_cairo_show_layout(cr, layout);
  pango_font_description_free(desc);
  g_object_unref(layout);
}
static int write_image(dt_imageio_module_data_t *data, const char *filename, const void *in,
                       dt_colorspaces_color_profile_type_t over_type, const char *over_filename,
                       void *exif, int exif_len, int imgid, int num, int total, dt_dev_pixelpipe_t *pipe,
                       const gboolean export_masks)
{
  dt_lib_snapshot_params_t *d = (dt_lib_snapshot_params_t *)data;

  memcpy(d->buf, in, sizeof(uint32_t) * data->width * data->height);
  d->width = data->width;
  d->height = data->height;

  return 0;
}

static int bpp(dt_imageio_module_data_t *data)
{
  return 8;
}

static int levels(dt_imageio_module_data_t *data)
{
  return IMAGEIO_RGB | IMAGEIO_INT8;
}

static const char *mime(dt_imageio_module_data_t *data)
{
  return "memory";
}

// export image for the snapshot d->snapshot[d->selected]
static int _export_image(dt_lib_module_t *self, size_t width, size_t height)
{
  dt_develop_t *dev = darktable.develop;

  dt_imageio_module_format_t buf;
  buf.mime = mime;
  buf.levels = levels;
  buf.bpp = bpp;
  buf.write_image = write_image;

  const dt_dev_zoom_t zoom = dt_control_get_dev_zoom();
  const int closeup = dt_control_get_dev_closeup();
  const float zoom_scale = dt_dev_get_zoom_scale(dev, zoom, 1<<closeup, 1);
  const float iwidth = dev->preview_pipe->backbuf_width * zoom_scale;
  const float iheight = dev->preview_pipe->backbuf_height * zoom_scale;

  // output with exact size as the current image in darkroom
  // if DT_ZOOM_FIT we use the current view port width/height
  // as-is for better overlay alignment.
  const size_t w =  zoom == DT_ZOOM_FIT ? width  : roundf(iwidth);
  const size_t h = (zoom == DT_ZOOM_FIT ? height : roundf(iheight)) + 2;

  dt_lib_snapshot_params_t dat;
  dat.head.max_width = w;
  dat.head.max_height = h;
  dat.head.width = w;
  dat.head.height = h;
  dat.head.style[0] = '\0';
  dat.head.style_append = FALSE;
  dat.bpp = 8;
  dat.buf = (uint8_t *)dt_alloc_align(64, sizeof(uint32_t) * w * h);

  const gboolean high_quality = FALSE;
  const gboolean upscale = TRUE;
  const gboolean export_masks = FALSE;
  const gboolean is_scaling = FALSE;

  dt_lib_snapshots_t *d = (dt_lib_snapshots_t *)self->data;
  dt_lib_snapshot_t *snap = &d->snapshot[d->selected];

  dt_imageio_export_with_flags
    (snap->imgid, "snapshot", &buf, (dt_imageio_module_data_t *)&dat, TRUE, TRUE,
     high_quality, upscale, is_scaling, FALSE, NULL, FALSE, export_masks,
     darktable.color_profiles->display_type, darktable.color_profiles->display_filename,
     DT_INTENT_LAST, NULL, NULL, 1, 1, NULL,
     snap->history_end);

  d->params.buf = dat.buf;
  d->params.width = dat.head.width;
  d->params.height = dat.head.height;
  dat.buf = NULL;

  return 0;
}

static gboolean _snap_expose_again(gpointer user_data)
{
  dt_control_queue_redraw_center();
  return FALSE;
}

/* expose snapshot over center viewport */
void gui_post_expose(dt_lib_module_t *self, cairo_t *cri, int32_t width, int32_t height, int32_t pointerx,
                     int32_t pointery)
{
  dt_lib_snapshots_t *d = (dt_lib_snapshots_t *)self->data;
  dt_develop_t *dev = darktable.develop;

  if(d->selected >= 0)
  {
    dt_lib_snapshot_t *snap = &d->snapshot[d->selected];

    const dt_dev_zoom_t zoom = dt_control_get_dev_zoom();
    const int closeup = dt_control_get_dev_closeup();
    const float zoom_scale = dt_dev_get_zoom_scale(dev, zoom, 1<<closeup, 1);

    // if a new snapshot is needed, do this now
    if(d->snap_requested && snap->zoom_scale == zoom_scale)
    {
      // export image with proper size, remove the darkroom borders
      _export_image(self, width - dev->border_size * 2, height- dev->border_size * 2);
      const int32_t stride = cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, d->params.width);
      if(snap->surface) cairo_surface_destroy(snap->surface);
      snap->surface = dt_cairo_image_surface_create_for_data
        (d->params.buf, CAIRO_FORMAT_RGB24, d->params.width, d->params.height, stride);
      snap->zoom_scale = zoom_scale;
      snap->width  = d->params.width;
      snap->height = d->params.height;
      d->snap_requested = FALSE;
      d->expose_again_timeout_id = -1;
    }

    // if zoom_scale changed, get a new snapshot at the right zoom level. this is using
    // a time out to ensure we don't try to create many snapshot while zooming (this is
    // slow), so we wait to the zoom level to be stabilized to create the new snapshot.
    if(snap->zoom_scale != zoom_scale
       || !snap->surface)
    {
      d->snap_requested = TRUE;
      snap->zoom_scale = zoom_scale;
      if(d->expose_again_timeout_id != -1) g_source_remove(d->expose_again_timeout_id);
      d->expose_again_timeout_id = g_timeout_add(150, _snap_expose_again, d);
    }

    float pzx, pzy;
    dt_dev_get_pointer_zoom_pos(dev, 0, 0, &pzx, &pzy);
    pzx = fmin(pzx + 0.5f, 0.0f);
    pzy = fmin(pzy + 0.5f, 0.0f);

    d->vp_width = width;
    d->vp_height = height;

    /* set x,y,w,h of surface depending on split align and invert */
    const double x = d->vertical
      ? (d->inverted ? width * d->vp_xpointer : 0)
      : 0;
    const double y = d->vertical
      ? 0
      : (d->inverted ? height * d->vp_ypointer : 0);
    const double w = d->vertical
      ? (d->inverted ? (width * (1.0 - d->vp_xpointer)) : width * d->vp_xpointer)
      : width;
    const double h = d->vertical
      ? height
      : (d->inverted ? (height * (1.0 - d->vp_ypointer)) : height * d->vp_ypointer);

    const double size = DT_PIXEL_APPLY_DPI(d->inverted ? -15 : 15);

    const double lx = width * d->vp_xpointer;
    const double ly = height * d->vp_ypointer;

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
    cairo_fill(cri);

    // preserve the darkroom border if defined. this is needed as the zoomed
    // snapshot will be bigger than the area. we also expect that when zooming
    // the whole area is taken
    if(dev->border_size > 0)
    {
      const int bs = dev->border_size;
      const int bs2 = dev->border_size * 2;
      cairo_rectangle(cri, bs, bs, width - bs2, height - bs2);
      cairo_clip(cri);
    }

    if(!d->snap_requested)
    {
      // display snapshot image surface
      cairo_save(cri);

      cairo_rectangle(cri, x, y, w, h);

      // center of viewport
      const float zoom_y = dt_control_get_dev_zoom_y();
      const float zoom_x = dt_control_get_dev_zoom_x();
      const float sw = (float)snap->width;
      const float sh = (float)snap->height;
      // offset are: center_area - zoom_offset
      const float offset_x = (.5f * (float)width) - (sw * (zoom_x + .5f));
      const float offset_y = (.5f * (float)height) - (sh * (zoom_y +  .5f));

      cairo_translate(cri, offset_x, offset_y);
      cairo_set_source_surface (cri, snap->surface, 0, 0);
      cairo_pattern_set_filter
        (cairo_get_source(cri),
         zoom_scale >= 0.9999f ? CAIRO_FILTER_FAST : darktable.gui->dr_filter_image);
      cairo_fill(cri);
      cairo_restore(cri);
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
    if(!d->dragging)
    {
      const double s = fmin(24, width * HANDLE_SIZE);
      const gint rx = (d->vertical ? width * d->vp_xpointer : width * 0.5) - (s * 0.5);
      const gint ry = (d->vertical ? height * 0.5 : height * d->vp_ypointer) - (s * 0.5);

      const gboolean display_rotation = (abs(pointerx - rx) < 40) && (abs(pointery - ry) < 40);
      dt_draw_set_color_overlay(cri, TRUE, display_rotation ? 1.0 : 0.3);

      cairo_set_line_width(cri, 0.5);
      dtgtk_cairo_paint_refresh(cri, rx, ry, s, s, 0, NULL);
    }

    d->on_going = FALSE;
  }
}

int button_released(struct dt_lib_module_t *self, double x, double y, int which, uint32_t state)
{
  dt_lib_snapshots_t *d = (dt_lib_snapshots_t *)self->data;
  if(d->selected >= 0)
  {
    d->dragging = FALSE;
    return 1;
  }
  return 0;
}

static int _lib_snapshot_rotation_cnt = 0;

int button_pressed(struct dt_lib_module_t *self, double x, double y, double pressure, int which, int type,
                   uint32_t state)
{
  dt_lib_snapshots_t *d = (dt_lib_snapshots_t *)self->data;

  if(d->selected >= 0)
  {
    if(d->on_going) return 1;

    const double xp = x / d->vp_width;
    const double yp = y / d->vp_height;

    /* do the split rotating */
    const double hhs = HANDLE_SIZE * 0.5;
    if(which == 1
       && (((d->vertical && xp > d->vp_xpointer - hhs && xp < d->vp_xpointer + hhs)
            && yp > 0.5 - hhs && yp < 0.5 + hhs)
           || ((!d->vertical && yp > d->vp_ypointer - hhs && yp < d->vp_ypointer + hhs)
               && xp > 0.5 - hhs && xp < 0.5 + hhs)
           || (d->vp_xrotate > xp - hhs && d->vp_xrotate <= xp + hhs && d->vp_yrotate > yp - hhs
               && d->vp_yrotate <= yp + hhs )))
    {
      /* let's rotate */
      _lib_snapshot_rotation_cnt++;

      d->vertical = !d->vertical;
      if(_lib_snapshot_rotation_cnt % 2) d->inverted = !d->inverted;

      d->vp_xpointer = xp;
      d->vp_ypointer = yp;
      d->vp_xrotate = xp;
      d->vp_yrotate = yp;
      d->on_going = TRUE;
      dt_control_queue_redraw_center();
    }
    /* do the dragging !? */
    else if(which == 1)
    {
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

int mouse_moved(dt_lib_module_t *self, double x, double y, double pressure, int which)
{
  dt_lib_snapshots_t *d = (dt_lib_snapshots_t *)self->data;

  if(d->selected >= 0)
  {
    const double xp = x / d->vp_width;
    const double yp = y / d->vp_height;

    /* update x pointer */
    if(d->dragging)
    {
      d->vp_xpointer = xp;
      d->vp_ypointer = yp;
    }
    dt_control_queue_redraw_center();
    return 1;
  }

  return 0;
}

static void _lib_snapshots_toggle_last(dt_action_t *action)
{
  dt_lib_snapshots_t *d = dt_action_lib(action)->data;

  if(d->num_snapshots)
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->snapshot[0].button), !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->snapshot[0].button)));
}

void gui_reset(dt_lib_module_t *self)
{
  dt_lib_snapshots_t *d = (dt_lib_snapshots_t *)self->data;
  d->num_snapshots = 0;
  d->selected = -1;
  d->snap_requested = FALSE;

  for(uint32_t k = 0; k < d->size; k++)
  {
    dt_lib_snapshot_t *s = &d->snapshot[k];

    if(s->surface) cairo_surface_destroy(s->surface);
    s->surface = NULL;
    s->zoom_scale = 0.f;
    gtk_widget_hide(s->button);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(s->button), FALSE);
  }

  dt_control_queue_redraw_center();
}

static void _signal_profile_changed(gpointer instance, uint8_t profile_type, gpointer user_data)
{
  // when the display profile is changed, make sure we recreate the snapshot
  if(profile_type == DT_COLORSPACES_PROFILE_TYPE_DISPLAY)
  {
    dt_lib_module_t *self = (dt_lib_module_t *)user_data;
    dt_lib_snapshots_t *d = (dt_lib_snapshots_t *)self->data;

    if(d->selected >= 0)
      d->snap_requested = TRUE;

    dt_control_queue_redraw_center();
  }
}

void gui_init(dt_lib_module_t *self)
{
  /* initialize ui widgets */
  dt_lib_snapshots_t *d = (dt_lib_snapshots_t *)g_malloc0(sizeof(dt_lib_snapshots_t));
  self->data = (void *)d;

  /* initialize snapshot storages */
  d->size = 4;
  d->snapshot = (dt_lib_snapshot_t *)g_malloc0_n(d->size, sizeof(dt_lib_snapshot_t));
  d->vp_xpointer = 0.5;
  d->vp_ypointer = 0.5;
  d->vp_xrotate = 0.0;
  d->vp_yrotate = 0.0;
  d->vertical = TRUE;
  d->on_going = FALSE;
  d->selected = -1;
  d->snap_requested = FALSE;
  d->expose_again_timeout_id = -1;

  /* initialize ui containers */
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  d->snapshots_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  /* create take snapshot button */
  d->take_button = dt_action_button_new(self, N_("take snapshot"), _lib_snapshots_add_button_clicked_callback, self,
                                        _("take snapshot to compare with another image "
                                          "or the same image at another stage of development"), 0, 0);

  /*
   * initialize snapshots
   */
  char wdname[32] = { 0 };
  char localtmpdir[PATH_MAX] = { 0 };
  dt_loc_get_tmp_dir(localtmpdir, sizeof(localtmpdir));

  for(int k = 0; k < d->size; k++)
  {
    /* create snapshot button */
    d->snapshot[k].button = gtk_toggle_button_new_with_label(wdname);
    GtkWidget *label = gtk_bin_get_child(GTK_BIN(d->snapshot[k].button));
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_MIDDLE);

    g_signal_connect(G_OBJECT(d->snapshot[k].button), "clicked",
                     G_CALLBACK(_lib_snapshots_toggled_callback), self);

    /* assign snapshot number to widget */
    g_object_set_data(G_OBJECT(d->snapshot[k].button), "snapshot", GINT_TO_POINTER(k + 1));

    /* add button to snapshot box */
    gtk_box_pack_start(GTK_BOX(d->snapshots_box), d->snapshot[k].button, FALSE, FALSE, 0);

    /* prevent widget to show on external show all */
    gtk_widget_set_no_show_all(d->snapshot[k].button, TRUE);
  }

  /* add snapshot box and take snapshot button to widget ui*/
  gtk_box_pack_start(GTK_BOX(self->widget),
                     dt_ui_scroll_wrap(d->snapshots_box, 1, "plugins/darkroom/snapshots/windowheight"), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), d->take_button, TRUE, TRUE, 0);

  dt_action_register(DT_ACTION(self), N_("toggle last snapshot"), _lib_snapshots_toggle_last, 0, 0);

  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_CONTROL_PROFILE_USER_CHANGED,
                                  G_CALLBACK(_signal_profile_changed), self);
}

void gui_cleanup(dt_lib_module_t *self)
{
  dt_lib_snapshots_t *d = (dt_lib_snapshots_t *)self->data;

  g_free(d->snapshot);

  g_free(self->data);
  self->data = NULL;
}

static void _lib_snapshots_add_button_clicked_callback(GtkWidget *widget, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_snapshots_t *d = (dt_lib_snapshots_t *)self->data;

  // first make sure the current history is properly written
  dt_dev_write_history(darktable.develop);

  /* backup last snapshot slot */
  dt_lib_snapshot_t last = d->snapshot[d->size - 1];

  /* rotate slots down to make room for new one on top */
  for(int k = d->size - 1; k > 0; k--)
  {
    GtkWidget *b = d->snapshot[k].button;
    GtkWidget *bp = d->snapshot[k - 1].button;
    d->snapshot[k] = d->snapshot[k - 1];
    d->snapshot[k].button = b;

    gtk_label_set_text(GTK_LABEL(gtk_bin_get_child(GTK_BIN(b))),
      gtk_label_get_text(GTK_LABEL(gtk_bin_get_child(GTK_BIN(bp)))));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(b),
                                 gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(bp)));
    gtk_widget_set_tooltip_text(b, g_strdup(gtk_widget_get_tooltip_text(bp)));
  }

  /* update top slot with new snapshot */
  char label[64];
  GtkWidget *b = d->snapshot[0].button;
  d->snapshot[0] = last;
  d->snapshot[0].button = b;
  const gchar *name = _("original");
  if(darktable.develop->history_end > 0)
  {
    dt_dev_history_item_t *history_item = g_list_nth_data(darktable.develop->history,
                                                          darktable.develop->history_end - 1);
    if(history_item && history_item->module)
      name = history_item->module->name();
    else
      name = _("unknown");
  }

  dt_lib_snapshot_t *s = &d->snapshot[0];
  s->zoom_scale = .0f;
  s->history_end = darktable.develop->history_end;
  s->imgid = darktable.develop->image_storage.id;
  s->surface = NULL;

  g_snprintf(label, sizeof(label), "%s (%d)", name, s->history_end);
  gtk_label_set_text(GTK_LABEL(gtk_bin_get_child(GTK_BIN(d->snapshot[0].button))), label);

  gtk_widget_set_tooltip_text(d->snapshot[0].button,
                              g_strdup(darktable.develop->image_storage.filename));

  /* update slots used */
  if(d->num_snapshots != d->size) d->num_snapshots++;

  /* show active snapshot slots */
  for(uint32_t k = 0; k < d->num_snapshots; k++) gtk_widget_show(d->snapshot[k].button);
}

static void _lib_snapshots_toggled_callback(GtkToggleButton *widget, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_snapshots_t *d = (dt_lib_snapshots_t *)self->data;
  /* get current snapshot index */
  const int which = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "snapshot"));

  d->selected = -1;

  /* check if snapshot is activated */
  if(gtk_toggle_button_get_active(widget))
  {
    /* lets deactivate all togglebuttons except for self */
    for(uint32_t k = 0; k < d->size; k++)
      if(GTK_WIDGET(widget) != d->snapshot[k].button)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->snapshot[k].button), FALSE);

    /* setup snapshot */
    d->selected = which - 1;
    dt_lib_snapshot_t *s = &d->snapshot[d->selected];
    s->zoom_scale = 0.0f;

    dt_dev_invalidate(darktable.develop);
  }

  /* redraw center view */
  dt_control_queue_redraw_center();
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
  dt_lib_snapshots_t *d = (dt_lib_snapshots_t *)self->data;
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
    return 0;
  }
}

static int ratio_member(lua_State *L)
{
  dt_lib_module_t *self = *(dt_lib_module_t **)lua_touserdata(L, 1);
  dt_lib_snapshots_t *d = (dt_lib_snapshots_t *)self->data;
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
    return 0;
  }
}

static int max_snapshot_member(lua_State *L)
{
  dt_lib_module_t *self = *(dt_lib_module_t **)lua_touserdata(L, 1);
  dt_lib_snapshots_t *d = (dt_lib_snapshots_t *)self->data;
  lua_pushinteger(L, d->size);
  return 1;
}

static int lua_take_snapshot(lua_State *L)
{
  dt_lib_module_t *self = lua_touserdata(L, lua_upvalueindex(1));
  dt_lib_snapshots_t *d = (dt_lib_snapshots_t *)self->data;
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
  dt_lib_snapshots_t *d = (dt_lib_snapshots_t *)self->data;
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
  dt_lib_snapshots_t *d = (dt_lib_snapshots_t *)self->data;
  lua_pushinteger(L, d->num_snapshots);
  return 1;
}

static int number_member(lua_State *L)
{
  dt_lib_module_t *self = *(dt_lib_module_t **)lua_touserdata(L, 1);
  dt_lib_snapshots_t *d = (dt_lib_snapshots_t *)self->data;
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
  dt_lib_snapshots_t *d = (dt_lib_snapshots_t *)module->data;
  if(index >= d->num_snapshots || index < 0)
  {
    return luaL_error(L, "Accessing a non-existent snapshot");
  }
  lua_pushstring(L, gtk_button_get_label(GTK_BUTTON(d->snapshot[index].button)));
  return 1;
}

static int lua_select(lua_State *L)
{
  dt_lua_snapshot_t index;
  luaA_to(L, dt_lua_snapshot_t, &index, 1);
  dt_lib_module_t *module = lua_touserdata(L, lua_upvalueindex(1));
  dt_lib_snapshots_t *d = (dt_lib_snapshots_t *)module->data;
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
