/*
    This file is part of darktable,
    copyright (c) 2011 johannes hanika.

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
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "develop/imageop.h"
#include "control/control.h"
#include "gui/gtk.h"
#include <gtk/gtk.h>
#include <stdlib.h>

#define EPSILON -0.00001

// this is the version of the modules parameters,
// and includes version information about compile-time dt
DT_MODULE(1)

typedef struct spot_t
{
  // position of the spot
  float x, y;
  // position to clone from
  float xc, yc;
  float radius;
}
spot_t;

typedef struct dt_iop_spots_params_t
{
  int num_spots;
  spot_t spot[32];
}
dt_iop_spots_params_t;

typedef struct dt_iop_spots_gui_data_t
{
  GtkLabel *label;
  int dragging;
  int selected;
  gboolean hoover_c; // is the pointer over the "clone from" end?
}
dt_iop_spots_gui_data_t;

typedef struct dt_iop_spots_params_t dt_iop_spots_data_t;

// this returns a translatable name
const char *name()
{
  return _("spot removal");
}

int
groups ()
{
  return IOP_GROUP_CORRECT;
}

int
operation_tags_filter ()
{
  return IOP_TAG_DISTORT;
}

// FIXME: doesn't work if source is outside of ROI
void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *i, void *o, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_spots_params_t *d = (dt_iop_spots_params_t *)piece->data;
  // const float scale = piece->iscale/roi_in->scale;
  const float scale = 1.0f/roi_in->scale;
  const int ch = piece->colors;
  // we don't modify most of the image:
  memcpy(o, i, sizeof(float)*roi_in->width*roi_in->height*ch);

  const float *in = (float *)i;
  float *out = (float *)o;
  // .. just a few spots:
  for(int i=0; i<d->num_spots; i++)
  {
    // convert from world space:
    const int x  = (d->spot[i].x *piece->buf_in.width)/scale - roi_in->x, y  = (d->spot[i].y *piece->buf_in.height)/scale - roi_in->y;
    const int xc = (d->spot[i].xc*piece->buf_in.width)/scale - roi_in->x, yc = (d->spot[i].yc*piece->buf_in.height)/scale - roi_in->y;
    const int rad = d->spot[i].radius * MIN(piece->buf_in.width, piece->buf_in.height)/scale;
    const int um = MIN(rad, MIN(x, xc)), uM = MIN(rad, MIN(roi_in->width -1-xc, roi_in->width -1-x));
    const int vm = MIN(rad, MIN(y, yc)), vM = MIN(rad, MIN(roi_in->height-1-yc, roi_in->height-1-y));
    float filter[2*rad + 1];
    // for(int k=-rad; k<=rad; k++) filter[rad + k] = expf(-k*k*2.f/(rad*rad));
    for(int k=-rad; k<=rad; k++)
    {
      const float kk = 1.0f - fabsf(k/(float)rad);
      filter[rad + k] = kk*kk*(3.0f - 2.0f*kk);
    }
    for(int u=-um; u<=uM; u++) for(int v=-vm; v<=vM; v++)
      {
        const float f = filter[rad+u]*filter[rad+v];
        for(int c=0; c<ch; c++)
          out[4*(roi_out->width*(y+v) + x+u) + c] =
            out[4*(roi_out->width*(y+v) + x+u) + c] * (1.0f-f) +
            in[4*(roi_in->width*(yc+v) + xc+u) + c] * f;
      }
  }
}

/** init, cleanup, commit to pipeline */
void init(dt_iop_module_t *module)
{
  // we don't need global data:
  module->data = NULL; //malloc(sizeof(dt_iop_spots_global_data_t));
  module->params = malloc(sizeof(dt_iop_spots_params_t));
  module->default_params = malloc(sizeof(dt_iop_spots_params_t));
  // our module is disabled by default
  // by default:
  module->default_enabled = 0;
  module->priority = 196; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_spots_params_t);
  module->gui_data = NULL;
  // init defaults:
  dt_iop_spots_params_t tmp = (dt_iop_spots_params_t)
  {
    0
  };

  memcpy(module->params, &tmp, sizeof(dt_iop_spots_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_spots_params_t));
}

void cleanup(dt_iop_module_t *module)
{
  free(module->gui_data);
  module->gui_data = NULL; // just to be sure
  free(module->params);
  module->params = NULL;
  free(module->data); // just to be sure
  module->data = NULL;
}

/** commit is the synch point between core and gui, so it copies params to pipe data. */
void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *params, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  memcpy(piece->data, params, sizeof(dt_iop_spots_params_t));
}

void init_pipe     (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_spots_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe  (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
}

/** gui callbacks, these are needed. */
void gui_update    (dt_iop_module_t *self)
{
  dt_iop_spots_params_t *p = (dt_iop_spots_params_t *)self->params;
  dt_iop_spots_gui_data_t *g = (dt_iop_spots_gui_data_t *)self->gui_data;
  char str[3];
  snprintf(str,3,"%d",p->num_spots);
  gtk_label_set_text(g->label, str);
}

void gui_init     (dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_spots_gui_data_t));
  dt_iop_spots_gui_data_t *g = (dt_iop_spots_gui_data_t *)self->gui_data;
  g->dragging = -1;
  g->selected = -1;
  self->widget = gtk_vbox_new(FALSE, 5);
  GtkWidget *label = gtk_label_new(_("click on a spot and drag on canvas to heal.\nuse the mouse wheel to adjust size.\nright click to remove a stroke."));
  gtk_misc_set_alignment(GTK_MISC(label), 0.0f, 0.5f);
  gtk_box_pack_start(GTK_BOX(self->widget), label, FALSE, TRUE, 0);
  GtkWidget * hbox = gtk_hbox_new(FALSE, 5);
  label = gtk_label_new(_("number of strokes:"));
  gtk_box_pack_start(GTK_BOX(hbox), label, FALSE, TRUE, 0);
  g->label = GTK_LABEL(gtk_label_new("-1"));
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(g->label), FALSE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), hbox, TRUE, TRUE, 0);
}

void gui_cleanup  (dt_iop_module_t *self)
{
  // nothing else necessary, gtk will clean up the labels
  free(self->gui_data);
  self->gui_data = NULL;
}

static void draw_overlay(cairo_t *cr, float rad, float x, float y, float xc, float yc, float xr, float yr)
{
  cairo_arc (cr, x, y, rad, 0, 2.0*M_PI);
  cairo_stroke (cr);
  cairo_arc (cr, xc, yc, rad, 0, 2.0*M_PI);
  cairo_stroke (cr);
  cairo_move_to (cr, xr, yr);
  cairo_line_to (cr, xc, yc);
  cairo_stroke (cr);
}

void gui_post_expose(dt_iop_module_t *self, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx, int32_t pointery)
{
  dt_develop_t *dev = self->dev;
  dt_iop_spots_params_t   *p = (dt_iop_spots_params_t   *)self->params;
  dt_iop_spots_gui_data_t *g = (dt_iop_spots_gui_data_t *)self->gui_data;
  float wd = dev->preview_pipe->backbuf_width;
  float ht = dev->preview_pipe->backbuf_height;
  float pzx, pzy;
  dt_dev_get_pointer_zoom_pos(dev, pointerx, pointery, &pzx, &pzy);
  pzx += 0.5f;
  pzy += 0.5f;
  float zoom_x, zoom_y;
  int32_t zoom, closeup;
  DT_CTL_GET_GLOBAL(zoom_y, dev_zoom_y);
  DT_CTL_GET_GLOBAL(zoom_x, dev_zoom_x);
  DT_CTL_GET_GLOBAL(zoom, dev_zoom);
  DT_CTL_GET_GLOBAL(closeup, dev_closeup);
  float zoom_scale = dt_dev_get_zoom_scale(dev, zoom, closeup ? 2 : 1, 1);

  cairo_set_source_rgb(cr, .3, .3, .3);

  cairo_translate(cr, width/2.0, height/2.0f);
  cairo_scale(cr, zoom_scale, zoom_scale);
  cairo_translate(cr, -.5f*wd-zoom_x*wd, -.5f*ht-zoom_y*ht);

  for(int i=0; i<p->num_spots; i++)
  {
    const float rad = MIN(wd, ht)*p->spot[i].radius;
    const float dx = p->spot[i].xc - p->spot[i].x;
    float dy = p->spot[i].yc - p->spot[i].y;
    if(dx == 0.0 && dy == 0.0) dy = EPSILON; // otherwise we'll have ol = 1.0/0.0 ==> xr = yr = -nan
    const float ol = 1.0f/sqrtf(dx*dx*wd*wd + dy*dy*ht*ht);
    const float d  = rad * ol;

    const float x = p->spot[i].x*wd, y = p->spot[i].y*ht;
    const float xc = p->spot[i].xc*wd, yc = p->spot[i].yc*ht;
    const float xr = (p->spot[i].x + d*dx)*wd, yr = (p->spot[i].y + d*dy)*ht;

    cairo_set_line_cap(cr,CAIRO_LINE_CAP_ROUND);
    if(i == g->selected || i == g->dragging) cairo_set_line_width(cr, 5.0/zoom_scale);
    else                                     cairo_set_line_width(cr, 3.0/zoom_scale);
    cairo_set_source_rgba(cr, .3, .3, .3, .8);
    draw_overlay(cr, rad, x, y, xc, yc, xr, yr);

    if(i == g->selected || i == g->dragging) cairo_set_line_width(cr, 2.0/zoom_scale);
    else                                     cairo_set_line_width(cr, 1.0/zoom_scale);
    cairo_set_source_rgba(cr, .8, .8, .8, .8);
    draw_overlay(cr, rad, x, y, xc, yc, xr, yr);

  }
}

int mouse_moved(dt_iop_module_t *self, double x, double y, int which)
{
  dt_iop_spots_params_t   *p = (dt_iop_spots_params_t   *)self->params;
  dt_iop_spots_gui_data_t *g = (dt_iop_spots_gui_data_t *)self->gui_data;
  // draw line (call post expose)
  // highlight selected point, if any
  float pzx, pzy;
  dt_dev_get_pointer_zoom_pos(self->dev, x, y, &pzx, &pzy);
  pzx += 0.5f;
  pzy += 0.5f;
  float mind = FLT_MAX;
  int selected = -1;
  const int old_sel = g->selected;
  gboolean hoover_c = g->hoover_c;
  g->selected = -1;
  if(g->dragging < 0) for(int i=0; i<p->num_spots; i++)
    {
      float dist = (pzx - p->spot[i].x)*(pzx - p->spot[i].x) + (pzy - p->spot[i].y)*(pzy - p->spot[i].y);
      if(dist < mind)
      {
        mind = dist;
        selected = i;
        hoover_c = FALSE;
      }
      dist = (pzx - p->spot[i].xc)*(pzx - p->spot[i].xc) + (pzy - p->spot[i].yc)*(pzy - p->spot[i].yc);
      if(dist < mind)
      {
        mind = dist;
        selected = i;
        hoover_c = TRUE;
      }
    }
  else
  {
    if(g->hoover_c)
    {
      p->spot[g->dragging].xc = pzx;
      p->spot[g->dragging].yc = pzy;
    }
    else
    {
      p->spot[g->dragging].x = pzx;
      p->spot[g->dragging].y = pzy;
    }
  }
  if(selected >= 0 && mind < p->spot[selected].radius * p->spot[selected].radius)
  {
    g->selected = selected;
    g->hoover_c = hoover_c;
  }
  if(g->dragging >= 0 || g->selected != old_sel) dt_control_queue_redraw_center();
  return 1;
}

int scrolled(dt_iop_module_t *self, double x, double y, int up, uint32_t state)
{
  dt_iop_spots_params_t   *p = (dt_iop_spots_params_t   *)self->params;
  dt_iop_spots_gui_data_t *g = (dt_iop_spots_gui_data_t *)self->gui_data;
  if(g->selected >= 0)
  {
    if(up && p->spot[g->selected].radius > 0.005f) p->spot[g->selected].radius *= 0.9f;
    else  if(p->spot[g->selected].radius < 0.1f  ) p->spot[g->selected].radius *= 1.0f/0.9f;
    dt_dev_add_history_item(darktable.develop, self, TRUE);
    return 1;
  }
  return 0;
}

int button_pressed(dt_iop_module_t *self, double x, double y, int which, int type, uint32_t state)
{
  // set new point, select it, start drag
  if(self->off) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->off), 1);
  dt_iop_spots_params_t   *p = (dt_iop_spots_params_t   *)self->params;
  dt_iop_spots_gui_data_t *g = (dt_iop_spots_gui_data_t *)self->gui_data;
  if(which == 1)
  {
    if(g->selected < 0)
    {
      if(p->num_spots == 32)
      {
        dt_control_log(_("spot removal only supports up to 32 spots"));
        return 1;
      }
      float pzx, pzy;
      dt_dev_get_pointer_zoom_pos(self->dev, x, y, &pzx, &pzy);
      pzx += 0.5f;
      pzy += 0.5f;
      const int i = p->num_spots++;
      g->dragging = i;
      // on *wd|*ht scale, radius on *min(wd, ht).
      p->spot[i].x = p->spot[i].xc = pzx;
      p->spot[i].y = p->spot[i].yc = pzy;
      p->spot[i].radius = 0.01f;
      g->selected = i;
      g->hoover_c = TRUE;
    }
    else
    {
      g->dragging = g->selected;
    }
    return 1;
  }
  return 0;
}

int button_released(struct dt_iop_module_t *self, double x, double y, int which, uint32_t state)
{
  // end point, stop drag
  dt_iop_spots_params_t   *p = (dt_iop_spots_params_t   *)self->params;
  dt_iop_spots_gui_data_t *g = (dt_iop_spots_gui_data_t *)self->gui_data;
  if(which == 1 && g->dragging >= 0)
  {
    float pzx, pzy;
    dt_dev_get_pointer_zoom_pos(self->dev, x, y, &pzx, &pzy);
    pzx += 0.5f;
    pzy += 0.5f;
    const int i = g->dragging;
    if(g->hoover_c)
    {
      p->spot[i].xc = pzx;
      p->spot[i].yc = pzy;
    }
    else
    {
      p->spot[i].x = pzx;
      p->spot[i].y = pzy;
    }
    g->selected = -1;
    dt_dev_add_history_item(darktable.develop, self, TRUE);
    g->dragging = -1;
    char str[3];
    snprintf(str,3,"%d",p->num_spots);
    gtk_label_set_text(g->label, str);

    return 1;
  }
  else if(which == 3 && g->selected >= 0)
  {
    // remove brush stroke
    const int i = --(p->num_spots);
    if(i > 0) memcpy(p->spot + g->selected, p->spot + i, sizeof(spot_t));
    dt_dev_add_history_item(darktable.develop, self, TRUE);
    g->selected = -1;
    char str[3];
    snprintf(str,3,"%d",p->num_spots);
    gtk_label_set_text(g->label, str);
  }
  return 0;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
