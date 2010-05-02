/*
    This file is part of darktable,
    copyright (c) 2009--2010 johannes hanika.

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
#include <stdlib.h>
#include <math.h>
#include <assert.h>
#include <string.h>
#include <gtk/gtk.h>
#include <inttypes.h>
#include <gdk/gdkkeysyms.h>
#ifdef HAVE_GEGL
  #include <gegl.h>
#endif
#include "develop/develop.h"
#include "develop/imageop.h"
#include "control/control.h"
#include "dtgtk/slider.h"
#include "dtgtk/togglebutton.h"
#include "gui/gtk.h"
#include "gui/draw.h"

DT_MODULE(2)

/** flip H/V, rotate an image, then clip the buffer. */
typedef enum dt_iop_clipping_flags_t
{
  FLAG_FLIP_HORIZONTAL = 1,
  FLAG_FLIP_VERTICAL = 2
}
dt_iop_clipping_flags_t;

typedef struct dt_iop_clipping_params_t
{
  float angle, cx, cy, cw, ch, aspect;
}
dt_iop_clipping_params_t;

typedef struct dt_iop_clipping_gui_data_t
{
  GtkVBox *vbox1, *vbox2;
  GtkHBox *hbox1, *hbox2;
  GtkLabel *label1, *label2, *label3, *label4, *label5;
  GtkDarktableSlider *scale1, *scale2, *scale3, *scale4, *scale5;
  GtkDarktableToggleButton *hflip,*vflip;
  GtkSpinButton *aspect;
  GtkCheckButton *aspect_on;
  GtkComboBox *aspect_presets;
  GtkLabel *label6;
  GtkComboBox *guide_lines;
  GtkLabel *label7;
  //GtkDarktableToggleButton *flipHorGoldenGuide, *flipVerGoldenGuide;
  GtkCheckButton *flipHorGoldenGuide, *flipVerGoldenGuide;
  GtkCheckButton *goldenSectionBox, *goldenSpiralSectionBox, *goldenSpiralBox, *goldenTriangleBox;

  float button_down_zoom_x, button_down_zoom_y, button_down_angle; // position in image where the button has been pressed.
  float clip_x, clip_y, clip_w, clip_h, handle_x, handle_y;
  int cropping, straightening;
  float aspect_ratios[6];
}
dt_iop_clipping_gui_data_t;

typedef struct dt_iop_clipping_data_t
{
  float angle;              // rotation angle
  float aspect;             // forced aspect ratio
  float m[4];               // rot matrix
  float tx, ty;             // rotation center
  float cx, cy, cw, ch;     // crop window
  float cix, ciy, ciw, cih; // crop window on roi_out 1.0 scale
  uint32_t flags;           // flipping flags
}
dt_iop_clipping_data_t;

void mul_mat_vec_2(const float *m, const float *p, float *o)
{
  o[0] = p[0]*m[0] + p[1]*m[1];
  o[1] = p[0]*m[2] + p[1]*m[3];
}

// helper to count corners in for loops:
void get_corner(const float *aabb, const int i, float *p)
{
  for(int k=0;k<2;k++) p[k] = aabb[2*((i>>k)&1) + k];
}

void adjust_aabb(const float *p, float *aabb)
{
  aabb[0] = fminf(aabb[0], p[0]);
  aabb[1] = fminf(aabb[1], p[1]);
  aabb[2] = fmaxf(aabb[2], p[0]);
  aabb[3] = fmaxf(aabb[3], p[1]);
}

const char *name()
{
  return _("crop and rotate");
}

// 1st pass: how large would the output be, given this input roi?
// this is always called with the full buffer before processing.
void modify_roi_out(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, dt_iop_roi_t *roi_out, const dt_iop_roi_t *roi_in)
{
  *roi_out = *roi_in;
  dt_iop_clipping_data_t *d = (dt_iop_clipping_data_t *)piece->data;

  // use whole-buffer roi information to create matrix and inverse.
  float rt[] = { cosf(d->angle),-sinf(d->angle),
                 sinf(d->angle), cosf(d->angle)};
  if(d->angle == 0.0f) { rt[0] = rt[3] = 1.0; rt[1] = rt[2] = 0.0f; }
  // fwd transform rotated points on corners and scale back inside roi_in bounds.
  float cropscale = 1.0f;
  float p[2], o[2], aabb[4] = {-.5f*roi_in->width, -.5f*roi_in->height, .5f*roi_in->width, .5f*roi_in->height};
  for(int c=0;c<4;c++)
  {
    get_corner(aabb, c, p);
    mul_mat_vec_2(rt, p, o);
    for(int k=0;k<2;k++) if(fabsf(o[k]) > 0.001f) cropscale = fminf(cropscale, aabb[(o[k] > 0 ? 2 : 0) + k]/o[k]);
  }

  // remember rotation center in whole-buffer coordinates:
  d->tx = roi_in->width  * .5f;
  d->ty = roi_in->height * .5f;

  // enforce aspect ratio, only make area smaller
  float ach = d->ch-d->cy, acw = d->cw-d->cx;
  if(d->aspect > 0.0)
  {
    const float ch = acw * roi_in->width / d->aspect  / (roi_in->height);
    const float cw = d->aspect * ach * roi_in->height / (roi_in->width);
    if     (acw >= cw) acw = cw; // width  smaller
    else if(ach >= ch) ach = ch; // height smaller
    else               acw *= ach/ch; // should never happen.
  }

  // rotate and clip to max extent
  roi_out->x      = d->tx - (.5f - d->cx)*cropscale*roi_in->width;
  roi_out->y      = d->ty - (.5f - d->cy)*cropscale*roi_in->height;
  roi_out->width  = acw*cropscale*roi_in->width;
  roi_out->height = ach*cropscale*roi_in->height;
  // sanity check.
  if(roi_out->width  < 1) roi_out->width  = 1;
  if(roi_out->height < 1) roi_out->height = 1;
  // save rotation crop on output buffer in world scale:
  d->cix = roi_out->x;
  d->ciy = roi_out->y;
  d->ciw = roi_out->width;
  d->cih = roi_out->height;

  rt[1] = - rt[1];
  rt[2] = - rt[2];
  for(int k=0;k<4;k++) d->m[k] = rt[k];
  if(d->flags & FLAG_FLIP_HORIZONTAL) { d->m[0] = - rt[0]; d->m[2] = - rt[2]; }
  if(d->flags & FLAG_FLIP_VERTICAL)   { d->m[1] = - rt[1]; d->m[3] = - rt[3]; }
}

// 2nd pass: which roi would this operation need as input to fill the given output region?
void modify_roi_in(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, const dt_iop_roi_t *roi_out, dt_iop_roi_t *roi_in)
{
  dt_iop_clipping_data_t *d = (dt_iop_clipping_data_t *)piece->data;
  *roi_in = *roi_out;
  // modify_roi_out took care of bounds checking for us. we hopefully do not get requests outside the clipping area.
  // transform aabb back to roi_in

  // this aabb is set off by cx/cy
  const float so = roi_out->scale;
  float p[2], o[2], aabb[4] = {roi_out->x+d->cix*so, roi_out->y+d->ciy*so, roi_out->x+d->cix*so+roi_out->width, roi_out->y+d->ciy*so+roi_out->height};
  float aabb_in[4] = {INFINITY, INFINITY, -INFINITY, -INFINITY};
  for(int c=0;c<4;c++)
  {
    // get corner points of roi_out
    get_corner(aabb, c, p);
    // backtransform aabb using m
    p[0] -= d->tx*so; p[1] -= d->ty*so;
    mul_mat_vec_2(d->m, p, o);
    o[0] += d->tx*so; o[1] += d->ty*so;
    // transform to roi_in space, get aabb.
    adjust_aabb(o, aabb_in);
  }
  // adjust roi_in to minimally needed region
  roi_in->x      = aabb_in[0]-2;
  roi_in->y      = aabb_in[1]-2;
  roi_in->width  = aabb_in[2]-aabb_in[0]+4;
  roi_in->height = aabb_in[3]-aabb_in[1]+4;
}

// 3rd (final) pass: you get this input region (may be different from what was requested above), 
// do your best to fill the ouput region!
void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *i, void *o, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_clipping_data_t *d = (dt_iop_clipping_data_t *)piece->data;
  float *in  = (float *)i;
  float *out = (float *)o;

  float pi[2], p0[2], tmp[2];
  float dx[2], dy[2];
  // get whole-buffer point from i,j
  pi[0] = roi_out->x + roi_out->scale*d->cix;
  pi[1] = roi_out->y + roi_out->scale*d->ciy;
  // transform this point using matrix m
  pi[0] -= d->tx*roi_out->scale; pi[1] -= d->ty*roi_out->scale;
  pi[0] /= roi_out->scale; pi[1] /= roi_out->scale;
  mul_mat_vec_2(d->m, pi, p0);
  p0[0] *= roi_in->scale; p0[1] *= roi_in->scale;
  p0[0] += d->tx*roi_in->scale;  p0[1] += d->ty*roi_in->scale;
  // transform this point to roi_in
  p0[0] -= roi_in->x; p0[1] -= roi_in->y;

  pi[0] = roi_out->x + roi_out->scale*d->cix + 1;
  pi[1] = roi_out->y + roi_out->scale*d->ciy;
  pi[0] -= d->tx*roi_out->scale; pi[1] -= d->ty*roi_out->scale;
  pi[0] /= roi_out->scale; pi[1] /= roi_out->scale;
  mul_mat_vec_2(d->m, pi, tmp);
  tmp[0] *= roi_in->scale; tmp[1] *= roi_in->scale;
  tmp[0] += d->tx*roi_in->scale; tmp[1] += d->ty*roi_in->scale;
  tmp[0] -= roi_in->x; tmp[1] -= roi_in->y;
  dx[0] = tmp[0] - p0[0]; dx[1] = tmp[1] - p0[1];

  pi[0] = roi_out->x + roi_out->scale*d->cix;
  pi[1] = roi_out->y + roi_out->scale*d->ciy + 1;
  pi[0] -= d->tx*roi_out->scale; pi[1] -= d->ty*roi_out->scale;
  pi[0] /= roi_out->scale; pi[1] /= roi_out->scale;
  mul_mat_vec_2(d->m, pi, tmp);
  tmp[0] *= roi_in->scale; tmp[1] *= roi_in->scale;
  tmp[0] += d->tx*roi_in->scale; tmp[1] += d->ty*roi_in->scale;
  tmp[0] -= roi_in->x; tmp[1] -= roi_in->y;
  dy[0] = tmp[0] - p0[0]; dy[1] = tmp[1] - p0[1];

  pi[0] = p0[0]; pi[1] = p0[1];
#ifdef _OPENMP
  #pragma omp parallel for schedule(static) default(none) firstprivate(pi,out) shared(o,p0,dx,dy,in,roi_in,roi_out)
#endif
  for(int j=0;j<roi_out->height;j++)
  {
    out = ((float *)o)+3*roi_out->width*j;
    for(int k=0;k<2;k++) pi[k] = p0[k] + j*dy[k];
    for(int i=0;i<roi_out->width;i++)
    {
      const int ii = (int)pi[0], jj = (int)pi[1];
      if(ii >= 0 && jj >= 0 && ii <= roi_in->width-2 && jj <= roi_in->height-2) 
      {
        const float fi = pi[0] - ii, fj = pi[1] - jj;
        for(int c=0;c<3;c++) out[c] = // in[3*(roi_in->width*jj + ii) + c];
              ((1.0f-fj)*(1.0f-fi)*in[3*(roi_in->width*(jj)   + (ii)  ) + c] +
               (1.0f-fj)*(     fi)*in[3*(roi_in->width*(jj)   + (ii+1)) + c] +
               (     fj)*(     fi)*in[3*(roi_in->width*(jj+1) + (ii+1)) + c] +
               (     fj)*(1.0f-fi)*in[3*(roi_in->width*(jj+1) + (ii)  ) + c]);
      }
      else for(int c=0;c<3;c++) out[c] = 0.0f;
      for(int k=0;k<2;k++) pi[k] += dx[k];
      out += 3;
    }
  }
}

void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_clipping_params_t *p = (dt_iop_clipping_params_t *)p1;
#ifdef HAVE_GEGL
  // pull in new params to gegl
  #error "clipping needs to be ported to GEGL!"
#else
  dt_iop_clipping_data_t *d = (dt_iop_clipping_data_t *)piece->data;
  d->angle = M_PI/180.0 * p->angle;
  d->cx = p->cx;
  d->cy = p->cy;
  d->cw = fabsf(p->cw);
  d->ch = fabsf(p->ch);
  d->aspect = p->aspect;
  d->flags = (p->ch < 0 ? FLAG_FLIP_VERTICAL : 0) | (p->cw < 0 ? FLAG_FLIP_HORIZONTAL : 0);
#endif
}

void init_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
#ifdef HAVE_GEGL
  #error "clipping needs to be ported to GEGL!"
#else
  piece->data = malloc(sizeof(dt_iop_clipping_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
#endif
}

void cleanup_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
#ifdef HAVE_GEGL
  #error "clipping needs to be ported to GEGL!"
#else
  free(piece->data);
#endif
}

static void
apply_box_aspect(dt_iop_module_t *self, int grab)
{
  dt_iop_clipping_gui_data_t *g = (dt_iop_clipping_gui_data_t *)self->gui_data;
  float wd = self->dev->preview_pipe->backbuf_width;
  float ht = self->dev->preview_pipe->backbuf_height;
  // enforce aspect ratio.
  const float aspect = gtk_spin_button_get_value(g->aspect);
  if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g->aspect_on)))
  {
    // TODO: if only one side changed, force aspect by two adjacent in equal parts!
    // 1 2 4 8 : x y w h

    // aspect = wd*w/ht*h
    // if we only modified one dim, respectively, we wanted these values:
    const float target_h = wd*g->clip_w/(ht*aspect);
    const float target_w = ht*g->clip_h*aspect/wd;
    // i.e. target_w/h = w/target_h = aspect


    // corners: move two adjacent 
    if     (grab == 1+2)
    { // move x y
      g->clip_x = fmaxf(0.0f, g->clip_x + g->clip_w - (target_w + g->clip_w)*.5);
      g->clip_y = fmaxf(0.0f, g->clip_y + g->clip_h - (target_h + g->clip_h)*.5);
      g->clip_w = fminf(1.0f, (target_w + g->clip_w)*.5f);
      g->clip_h = fminf(1.0f, (target_h + g->clip_h)*.5f);
    }
    else if(grab == 2+4) // move y w
    {
      g->clip_y = fmaxf(0.0f, g->clip_y + g->clip_h - (target_h + g->clip_h)*.5);
      g->clip_w = fminf(1.0f, (target_w + g->clip_w)*.5);
      g->clip_h = fminf(1.0f, (target_h + g->clip_h)*.5f);
    }
    else if(grab == 4+8) // move w h
    {
      g->clip_w = fminf(1.0f, (target_w + g->clip_w)*.5);
      g->clip_h = fminf(1.0f, (target_h + g->clip_h)*.5);
    }
    else if(grab == 8+1) // move h x
    {
      g->clip_h = fminf(1.0f, (target_h + g->clip_h)*.5);
      g->clip_x = fmaxf(0.0f, g->clip_x + g->clip_w - (target_w + g->clip_w)*.5);
      g->clip_w = fminf(1.0f, (target_w + g->clip_w)*.5);
    }
    else if(grab & 5) // dragged either x or w (1 4)
    { // change h and move y, h equally
      const float off = target_h - g->clip_h;
      g->clip_h = fminf(1.0, g->clip_h + off);
      g->clip_y = fmaxf(0.0, g->clip_y - .5f*off);
    }
    else if(grab & 10) // dragged either y or h (2 8)
    { // channge w and move x, w equally
      const float off = target_w - g->clip_w;
      g->clip_w = fminf(1.0, g->clip_w + off);
      g->clip_x = fmaxf(0.0, g->clip_x - .5f*off);
    }

    if(g->clip_x + g->clip_w > 1.0)
    {
      g->clip_h *= (1.0 - g->clip_x)/g->clip_w;
      g->clip_w  =  1.0 - g->clip_x;
    }
    if(g->clip_y + g->clip_h > 1.0)
    {
      g->clip_w *= (1.0 - g->clip_y)/g->clip_h;
      g->clip_h  =  1.0 - g->clip_y;
    }
  }
}

static void
aspect_presets_changed (GtkComboBox *combo, dt_iop_module_t *self)
{
  dt_iop_clipping_gui_data_t *g = (dt_iop_clipping_gui_data_t *)self->gui_data;
  int which = gtk_combo_box_get_active(combo);
  if (which >= 0 && which < 6)
  {
    gtk_spin_button_set_value(g->aspect, g->aspect_ratios[which]);
    dt_control_queue_draw_all();
    self->dev->gui_module = self;
  }
}

static void
cx_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_clipping_params_t *p = (dt_iop_clipping_params_t *)self->params;
  p->cx = dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self);
}

static void
cy_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_clipping_params_t *p = (dt_iop_clipping_params_t *)self->params;
  p->cy = dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self);
}

static void
cw_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_clipping_params_t *p = (dt_iop_clipping_params_t *)self->params;
  p->cw = copysignf( dtgtk_slider_get_value(slider), p->cw);
  dt_dev_add_history_item(darktable.develop, self);
}

static void
ch_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_clipping_params_t *p = (dt_iop_clipping_params_t *)self->params;
  p->ch = copysignf( dtgtk_slider_get_value(slider), p->ch);
  dt_dev_add_history_item(darktable.develop, self);
}

static void
angle_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_clipping_params_t *p = (dt_iop_clipping_params_t *)self->params;
  p->angle = dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self);
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_clipping_gui_data_t *g = (dt_iop_clipping_gui_data_t *)self->gui_data;
  dt_iop_clipping_params_t *p = (dt_iop_clipping_params_t *)self->params;
  dtgtk_slider_set_value(g->scale1, p->cx);
  dtgtk_slider_set_value(g->scale2, p->cy);
  dtgtk_slider_set_value(g->scale3, fabsf(p->cw));
  dtgtk_slider_set_value(g->scale4, fabsf(p->ch));
  dtgtk_slider_set_value(g->scale5, p->angle);
  gtk_spin_button_set_value(g->aspect, fabsf(p->aspect));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->hflip), p->cw < 0);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->vflip), p->ch < 0);
  if(p->aspect > 0)
  {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->aspect_on), TRUE);
    gtk_widget_set_sensitive(GTK_WIDGET(g->aspect), TRUE);
  }
  else
  {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->aspect_on), FALSE);
    gtk_widget_set_sensitive(GTK_WIDGET(g->aspect), FALSE);
  }
}

void init(dt_iop_module_t *module)
{
  // module->data = malloc(sizeof(dt_iop_clipping_data_t));
  module->params = malloc(sizeof(dt_iop_clipping_params_t));
  module->default_params = malloc(sizeof(dt_iop_clipping_params_t));
  module->default_enabled = 0;
  module->params_size = sizeof(dt_iop_clipping_params_t);
  module->gui_data = NULL;
  module->priority = 950;
  dt_iop_clipping_params_t tmp = (dt_iop_clipping_params_t){0.0, 0.0, 0.0, 1.0, 1.0, -1.0};
  tmp.aspect = -module->dev->image->width/(float)module->dev->image->height;
  memcpy(module->params, &tmp, sizeof(dt_iop_clipping_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_clipping_params_t));
}

void cleanup(dt_iop_module_t *module)
{
  free(module->gui_data);
  module->gui_data = NULL;
  free(module->params);
  module->params = NULL;
}

static void
aspect_callback(GtkSpinButton *widget, dt_iop_module_t *self)
{
  apply_box_aspect(self, 5);
  if(darktable.gui->reset) return;
  dt_iop_clipping_params_t *p = (dt_iop_clipping_params_t *)self->params;
  dt_iop_clipping_gui_data_t *g = (dt_iop_clipping_gui_data_t *)self->gui_data;
  gboolean active = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g->aspect_on));
  if(active) p->aspect =   gtk_spin_button_get_value(widget);
  else       p->aspect = - gtk_spin_button_get_value(widget);
  self->dev->gui_module = self;
  dt_control_queue_draw_all();
}

static void
aspect_on_callback(GtkCheckButton *widget, dt_iop_module_t *self)
{
  apply_box_aspect(self, 5);
  if(darktable.gui->reset) return;
  dt_iop_clipping_gui_data_t *g = (dt_iop_clipping_gui_data_t *)self->gui_data;
  gboolean active = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));
  gtk_widget_set_sensitive(GTK_WIDGET(g->aspect), active);
  self->dev->gui_module = self;
  dt_control_queue_draw_all();
}

static void
toggled_callback(GtkDarktableToggleButton *widget, dt_iop_module_t *self)
{
  if(self->dt->gui->reset) return;
  dt_iop_clipping_gui_data_t *g = (dt_iop_clipping_gui_data_t *)self->gui_data;
  dt_iop_clipping_params_t *p = (dt_iop_clipping_params_t *)self->params;
  if(widget==g->hflip)
  {
    if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget))) p->cw = copysignf(p->cw, -1.0);
    else                                     p->cw = copysignf(p->cw,  1.0);
  }
  else if(widget==g->vflip)
  {
    if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget))) p->ch = copysignf(p->ch, -1.0);
    else                                     p->ch = copysignf(p->ch,  1.0);
  }
  if(self->off) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->off), 1);
  dt_dev_add_history_item(darktable.develop, self);
}

static void
key_accel_callback(void *d)
{
  dt_iop_clipping_gui_data_t *g = (dt_iop_clipping_gui_data_t *)d;
  gtk_spin_button_set_value(g->aspect, 1.0/gtk_spin_button_get_value(g->aspect));
}

// Golden number (1+sqrt(5))/2
#define PHI      1.61803398874989479F
// 1/PHI
#define INVPHI   0.61803398874989479F

#define GUIDE_NONE 0
#define GUIDE_GRID 1
#define GUIDE_THIRD 2
#define GUIDE_DIAGONAL 3
#define GUIDE_TRIANGL 4
#define GUIDE_GOLDEN 5

static void
guides_presets_changed (GtkComboBox *combo, dt_iop_module_t *self)
{
  dt_iop_clipping_gui_data_t *g = (dt_iop_clipping_gui_data_t *)self->gui_data;
  int which = gtk_combo_box_get_active(combo);
  if ( which == GUIDE_TRIANGL || which == GUIDE_GOLDEN ) {
    gtk_widget_set_visible(GTK_WIDGET(g->label7), TRUE);
    gtk_widget_set_visible(GTK_WIDGET(g->flipHorGoldenGuide), TRUE);
    gtk_widget_set_visible(GTK_WIDGET(g->flipVerGoldenGuide), TRUE);
  } else {
    gtk_widget_set_visible(GTK_WIDGET(g->label7), FALSE);
    gtk_widget_set_visible(GTK_WIDGET(g->flipHorGoldenGuide), FALSE);
    gtk_widget_set_visible(GTK_WIDGET(g->flipVerGoldenGuide), FALSE);
  }

  if ( which == GUIDE_GOLDEN){
    gtk_widget_set_visible(GTK_WIDGET(g->goldenSectionBox), TRUE);
    gtk_widget_set_visible(GTK_WIDGET(g->goldenSpiralSectionBox), TRUE);
    gtk_widget_set_visible(GTK_WIDGET(g->goldenSpiralBox), TRUE);
    gtk_widget_set_visible(GTK_WIDGET(g->goldenTriangleBox), TRUE);
  } else {
    gtk_widget_set_visible(GTK_WIDGET(g->goldenSectionBox), FALSE);
    gtk_widget_set_visible(GTK_WIDGET(g->goldenSpiralSectionBox), FALSE);
    gtk_widget_set_visible(GTK_WIDGET(g->goldenSpiralBox), FALSE);
    gtk_widget_set_visible(GTK_WIDGET(g->goldenTriangleBox), FALSE);
  }


  dt_control_queue_draw_all();
}

static void
guides_button_changed (GtkComboBox *combo, dt_iop_module_t *self)
{
  // Redraw guides
  dt_control_queue_draw_all();
}

void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_clipping_gui_data_t));
  dt_iop_clipping_gui_data_t *g = (dt_iop_clipping_gui_data_t *)self->gui_data;
  dt_iop_clipping_params_t *p = (dt_iop_clipping_params_t *)self->params;

  g->clip_x = g->clip_y = g->handle_x = g->handle_y = 0.0;
  g->clip_w = g->clip_h = 1.0;
  g->cropping = 0;
  g->straightening = 0;

  self->widget = GTK_WIDGET(gtk_vbox_new(FALSE, 0));
  g->vbox1 = GTK_VBOX(gtk_vbox_new(TRUE, 0));
  g->vbox2 = GTK_VBOX(gtk_vbox_new(TRUE, 0));
  g->hbox1 = GTK_HBOX(gtk_hbox_new(TRUE, 0));
  g->hbox2 = GTK_HBOX(gtk_hbox_new(FALSE, 0));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->hbox2), FALSE, FALSE, 5);
  gtk_box_pack_start(GTK_BOX(g->hbox2), GTK_WIDGET(g->vbox1), FALSE, FALSE, 5);
  gtk_box_pack_start(GTK_BOX(g->hbox2), GTK_WIDGET(g->vbox2), TRUE, TRUE, 5);
  g->hflip = DTGTK_TOGGLEBUTTON(dtgtk_togglebutton_new(dtgtk_cairo_paint_flip,CPF_DIRECTION_UP));
  g->vflip = DTGTK_TOGGLEBUTTON(dtgtk_togglebutton_new(dtgtk_cairo_paint_flip,0));
  GtkWidget *label = gtk_label_new(_("flip"));
  gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
  gtk_box_pack_start(GTK_BOX(g->vbox1), label, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->hbox1), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(g->hbox1), GTK_WIDGET(g->hflip), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->hbox1), GTK_WIDGET(g->vflip), TRUE, TRUE, 0);
  g->label1 = GTK_LABEL(gtk_label_new(_("crop x")));
  g->label2 = GTK_LABEL(gtk_label_new(_("crop y")));
  g->label3 = GTK_LABEL(gtk_label_new(_("crop w")));
  g->label4 = GTK_LABEL(gtk_label_new(_("crop h")));
  g->label5 = GTK_LABEL(gtk_label_new(_("angle")));
  gtk_misc_set_alignment(GTK_MISC(g->label1), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(g->label2), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(g->label3), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(g->label4), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(g->label5), 0.0, 0.5);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label1), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label2), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label3), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label4), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label5), FALSE, FALSE, 0);
  g->scale1 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 1.0, 0.01,p->cx,2));
  g->scale2 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 1.0, 0.01,p->cy,2));
  g->scale3 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 1.0, 0.01,p->cw,2));
  g->scale4 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 1.0, 0.01,p->ch,2));
  g->scale5 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_VALUE,-180.0, 180.0, 0.5,p->angle,2));
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale1), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale2), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale3), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale4), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale5), TRUE, TRUE, 0);

  g_signal_connect (G_OBJECT (g->hflip), "toggled", G_CALLBACK(toggled_callback), self);
  g_signal_connect (G_OBJECT (g->vflip), "toggled", G_CALLBACK(toggled_callback), self);

  gtk_object_set (GTK_OBJECT(g->hflip), "tooltip-text", _("vertical flip"), NULL);
  gtk_object_set (GTK_OBJECT(g->vflip), "tooltip-text", _("horizontal flip"), NULL);

  GtkWidget *hbox = gtk_hbox_new(FALSE, 5);
  g->aspect_on = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("aspect")));
  // gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->aspect_on), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(g->aspect_on), TRUE, TRUE, 0);
  gtk_object_set (GTK_OBJECT(g->aspect_on), "tooltip-text", _("fixed aspect ratio"), NULL);
  g_signal_connect (G_OBJECT (g->aspect_on), "toggled",
                    G_CALLBACK (aspect_on_callback), self);

  g->aspect = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(0.1, 10.0, 0.01));
  gtk_spin_button_set_increments(g->aspect, 0.01, 0.2);
  gtk_spin_button_set_digits(g->aspect, 3);
  gtk_widget_set_sensitive(GTK_WIDGET(g->aspect), FALSE);
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(g->aspect), TRUE, TRUE, 0);
  g_signal_connect (G_OBJECT (g->aspect), "value-changed",
                    G_CALLBACK (aspect_callback), self);
  gtk_object_set(GTK_OBJECT(g->aspect), "tooltip-text", _("set the aspect ratio (w/h)\npress ctrl-x to swap sides"), NULL);
  g->aspect_presets = GTK_COMBO_BOX(gtk_combo_box_new_text());
  gtk_combo_box_append_text(g->aspect_presets, _("image"));
  gtk_combo_box_append_text(g->aspect_presets, _("golden cut"));
  gtk_combo_box_append_text(g->aspect_presets, _("3:2"));
  gtk_combo_box_append_text(g->aspect_presets, _("4:3"));
  gtk_combo_box_append_text(g->aspect_presets, _("square"));
  gtk_combo_box_append_text(g->aspect_presets, _("din"));
  dt_gui_key_accel_register(GDK_CONTROL_MASK, GDK_x, key_accel_callback, (void *)g);
  g_signal_connect (G_OBJECT (g->aspect_presets), "changed",
                    G_CALLBACK (aspect_presets_changed), self);
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(g->aspect_presets), TRUE, TRUE, 0);
  // gtk_box_pack_start(GTK_BOX(g->vbox2), hbox, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), hbox, FALSE, FALSE, 0);

/*-------------------------------------------*/
  hbox = gtk_hbox_new(TRUE, 5);
  g->label6 = GTK_LABEL(gtk_label_new(_("guides")));
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(g->label6), TRUE, TRUE, 0);
  g->guide_lines = GTK_COMBO_BOX(gtk_combo_box_new_text());
  gtk_combo_box_append_text(g->guide_lines, _("none"));
  gtk_combo_box_append_text(g->guide_lines, _("grid"));
  gtk_combo_box_append_text(g->guide_lines, _("rules of thirds"));
  gtk_combo_box_append_text(g->guide_lines, _("diagonal method"));
  gtk_combo_box_append_text(g->guide_lines, _("harmonious triangles"));
  gtk_combo_box_append_text(g->guide_lines, _("golden mean"));
  gtk_combo_box_set_active(g->guide_lines, GUIDE_NONE);
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(g->guide_lines), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), hbox, FALSE, FALSE, 0);
  gtk_object_set (GTK_OBJECT(g->guide_lines), "tooltip-text", _("with this option, you can display guide lines "
	    "to help compose your photograph."), NULL);
  g_signal_connect (G_OBJECT (g->guide_lines), "changed",
                    G_CALLBACK (guides_presets_changed), self);

/*-------------------------------------------*/
  hbox = gtk_hbox_new(TRUE, 5);
  g->label7 = GTK_LABEL(gtk_label_new(_("flip guides")));
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(g->label7), TRUE, TRUE, 0);
  //g->flipHorGoldenGuide = DTGTK_TOGGLEBUTTON(dtgtk_togglebutton_new(dtgtk_cairo_paint_flip,CPF_DIRECTION_UP));
  g->flipHorGoldenGuide = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("horizontally")));
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(g->flipHorGoldenGuide), TRUE, TRUE, 0);
  //g->flipVerGoldenGuide = DTGTK_TOGGLEBUTTON(dtgtk_togglebutton_new(dtgtk_cairo_paint_flip,0));
  g->flipVerGoldenGuide = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("vertically")));
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(g->flipVerGoldenGuide), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), hbox, FALSE, FALSE, 0);
  gtk_object_set (GTK_OBJECT(g->flipHorGoldenGuide), "tooltip-text", _("flip guides horizontally"), NULL);
  gtk_object_set (GTK_OBJECT(g->flipVerGoldenGuide), "tooltip-text", _("flip guides vertically"), NULL);
/*-------------------------------------------*/
  hbox = gtk_hbox_new(TRUE, 5);
  g->goldenSectionBox = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("golden sections")));
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(g->goldenSectionBox), TRUE, TRUE, 0);
  gtk_object_set (GTK_OBJECT(g->goldenSectionBox), "tooltip-text", _("enable this option to show golden sections."), NULL);

  g->goldenSpiralSectionBox = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("golden spiral sections")));
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(g->goldenSpiralSectionBox), TRUE, TRUE, 0);
  gtk_object_set (GTK_OBJECT(g->goldenSpiralSectionBox), "tooltip-text", _("enable this option to show golden spiral sections."), NULL);
  gtk_box_pack_start(GTK_BOX(self->widget), hbox, FALSE, FALSE, 0);

  hbox = gtk_hbox_new(TRUE, 5);
  g->goldenSpiralBox = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("golden spiral")));
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(g->goldenSpiralBox), TRUE, TRUE, 0);
  gtk_object_set (GTK_OBJECT(g->goldenSpiralBox), "tooltip-text", _("enable this option to show a golden spiral guide."), NULL);
  //gtk_object_set (GTK_OBJECT(g->goldenSpiralBox), "tooltip-text", _("it is not implemented yet."), NULL);

  g->goldenTriangleBox = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("golden triangles")));
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(g->goldenTriangleBox), TRUE, TRUE, 0);
  gtk_object_set (GTK_OBJECT(g->goldenTriangleBox), "tooltip-text", _("enable this option to show golden triangles."), NULL);
  gtk_box_pack_start(GTK_BOX(self->widget), hbox, FALSE, FALSE, 0);

  g_signal_connect (G_OBJECT (g->flipHorGoldenGuide), "toggled", G_CALLBACK (guides_button_changed), self);
  g_signal_connect (G_OBJECT (g->flipVerGoldenGuide), "toggled", G_CALLBACK (guides_button_changed), self);
  g_signal_connect (G_OBJECT (g->goldenSectionBox), "toggled", G_CALLBACK (guides_button_changed), self);
  g_signal_connect (G_OBJECT (g->goldenSpiralSectionBox), "toggled", G_CALLBACK (guides_button_changed), self);
  g_signal_connect (G_OBJECT (g->goldenSpiralBox), "toggled", G_CALLBACK (guides_button_changed), self);
  g_signal_connect (G_OBJECT (g->goldenTriangleBox), "toggled", G_CALLBACK (guides_button_changed), self);

  gtk_widget_set_visible(GTK_WIDGET(g->label7), FALSE);
  gtk_widget_set_visible(GTK_WIDGET(g->flipHorGoldenGuide), FALSE);
  gtk_widget_set_visible(GTK_WIDGET(g->flipVerGoldenGuide), FALSE);
  gtk_widget_set_visible(GTK_WIDGET(g->goldenSectionBox), FALSE);
  gtk_widget_set_visible(GTK_WIDGET(g->goldenSpiralSectionBox), FALSE);
  gtk_widget_set_visible(GTK_WIDGET(g->goldenSpiralBox), FALSE);
  gtk_widget_set_visible(GTK_WIDGET(g->goldenTriangleBox), FALSE);

/*-------------------------------------------*/

  g_signal_connect (G_OBJECT (g->scale1), "value-changed",
                    G_CALLBACK (cx_callback), self);
  g_signal_connect (G_OBJECT (g->scale2), "value-changed",
                    G_CALLBACK (cy_callback), self);
  g_signal_connect (G_OBJECT (g->scale3), "value-changed",
                    G_CALLBACK (cw_callback), self);
  g_signal_connect (G_OBJECT (g->scale4), "value-changed",
                    G_CALLBACK (ch_callback), self);
  g_signal_connect (G_OBJECT (g->scale5), "value-changed",
                    G_CALLBACK (angle_callback), self);
  g->aspect_ratios[0] = self->dev->image->width/(float)self->dev->image->height;
  g->aspect_ratios[1] = 1.6280;
  g->aspect_ratios[2] = 3.0/2.0;
  g->aspect_ratios[3] = 4.0/3.0;
  g->aspect_ratios[4] = 1.0;
  g->aspect_ratios[5] = sqrtf(2.0);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  dt_gui_key_accel_unregister(key_accel_callback);
  free(self->gui_data);
  self->gui_data = NULL;
}

static int
get_grab (float pzx, float pzy, dt_iop_clipping_gui_data_t *g, const float border, const float wd, const float ht)
{
  int grab = 0;
  if(pzx >= g->clip_x && pzx*wd < g->clip_x*wd + border) grab |= 1;
  if(pzy >= g->clip_y && pzy*ht < g->clip_y*ht + border) grab |= 2;
  if(pzx <= g->clip_x+g->clip_w && pzx*wd > (g->clip_w+g->clip_x)*wd - border) grab |= 4;
  if(pzy <= g->clip_y+g->clip_h && pzy*ht > (g->clip_h+g->clip_y)*ht - border) grab |= 8;
  return grab;
}

void drawLine(cairo_t *cr, double left, double top, double right, double bottom)
{
  cairo_move_to(cr, left, top); cairo_line_to(cr, right,  bottom);
}

typedef struct QRect
{
  double left, top, right, bottom, width, height;
}
QRect;

void qRect(QRect *R1, double left, double top, double width, double height)
{
  R1->left=left;
  R1->top=top;
  R1->right=left+width;
  R1->bottom=top+height;
  R1->width=width;
  R1->height=height;
}

void printRect(char *name, QRect *R1)
{
  printf("%s sizes left=%e top=%e right=%e bottom=%e width=%e height=%e\n", name, R1->left, R1->top, R1->right, R1->bottom, R1->width, R1->height);
}

void pr_create (char *name, int left, int top, int width, int height)
{
  //printf("CREATE %s sizes left=%e top=%e width=%e height=%e\n", name, left, top, right, width, height);
  printf("CREATE %s sizes left=%d top=%d width=%d height=%d\n", name, left, top, width, height);
}

void drawDiagonalMethod(cairo_t *cr, const int x, const int y, const int w, const int h)
{
  //p.setRenderHint(QPainter::Antialiasing);

  if (w > h)
  {
    drawLine(cr, x, y, x+h, y+h);
    drawLine(cr, x, y+h, x+h, y);
    drawLine(cr, x+w-h, y, x+w, y+h);
    drawLine(cr, x+w-h, y+h, x+w, y);
  }
  else
  {
    drawLine(cr, x, y, x+w, y+w);
    drawLine(cr, x, y+w, x+w, y);
    drawLine(cr, x, y+h-w, x+w, y+h);
    drawLine(cr, x, y+h, x+w, y+h-w);
  }
}

void drawRulesOfThirds(cairo_t *cr, const int left, const int top,  const int right, const int bottom, const int xThird, const int yThird)
{

  //p.setRenderHint(QPainter::Antialiasing);
  drawLine(cr, left + xThird, top, left + xThird, bottom);
  drawLine(cr, left + 2*xThird, top, left + 2*xThird, bottom);

  drawLine(cr, left, top + yThird, right, top + yThird);
  drawLine(cr, left, top + 2*yThird, right, top + 2*yThird);

}

void drawHarmoniousTriangles(cairo_t *cr, const int left, const int top,  const int right, const int bottom, const int dst)
{
  int width, height;
  width = right - left;
  height = bottom - top;
  //p.setRenderHint(QPainter::Antialiasing);

  drawLine(cr, -width/2, -height/2, width/2,  height/2);
  drawLine(cr, -width/2+dst, -height/2, -width/2,  height/2);
  drawLine(cr, width/2, -height/2, width/2-dst,  height/2);

}

#define RADIANS(degrees) ((degrees) * (M_PI / 180.))
void drawGoldenMean(struct dt_iop_module_t *self, cairo_t *cr, QRect* R1, QRect* R2, QRect* R3, QRect* R4, QRect* R5, QRect* R6, QRect* R7)
{
    dt_iop_clipping_gui_data_t *g = (dt_iop_clipping_gui_data_t *)self->gui_data;
    //p.setRenderHint(QPainter::Antialiasing);

    // Drawing Golden sections.
    if ( gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g->goldenSectionBox)))
    {
       // horizontal lines:
       drawLine(cr, R1->left, R2->top, R2->right, R2->top);
       drawLine(cr, R1->left, R1->top + R2->height, R2->right, R1->top + R2->height);

       // vertical lines:
       drawLine(cr, R1->right, R1->top, R1->right, R1->bottom);
       drawLine(cr, R1->left+R2->width, R1->top, R1->left+R2->width, R1->bottom);
    }

    // Drawing Golden triangle guides.
    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g->goldenTriangleBox)))
    {
       drawLine(cr, R1->left, R1->bottom, R2->right, R1->top);
       drawLine(cr, R1->left, R1->top, R2->right-R1->width, R1->bottom);
       drawLine(cr, R1->left + R1->width, R1->top, R2->right, R1->bottom);
    }

    // Drawing Golden spiral sections.
    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g->goldenSpiralSectionBox)))
    {
      drawLine(cr, R1->right, R1->top,    R1->right, R1->bottom);
      drawLine(cr, R2->left,  R2->top,    R2->right, R2->top);
      drawLine(cr, R3->left,  R3->top,    R3->left, R3->bottom);
      drawLine(cr, R4->left,  R4->bottom, R4->right, R4->bottom);
      drawLine(cr, R5->right, R5->top,    R5->right, R5->bottom);
      drawLine(cr, R6->left,  R6->top,    R6->right, R6->top);
      drawLine(cr, R7->left,  R7->top,    R7->left, R7->bottom);
    }

    // Drawing Golden Spiral.
    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g->goldenSpiralBox)))
    {
      cairo_save(cr);
      cairo_new_sub_path(cr);
      cairo_scale(cr, R1->width/R1->height, 1);
      cairo_arc ( cr, R1->right/R1->width*R1->height, R1->top, R1->height, RADIANS(90), RADIANS(180) );
      cairo_restore(cr);

      cairo_save(cr);
      cairo_new_sub_path(cr);
      cairo_scale(cr, R2->width/R2->height, 1);
      cairo_arc ( cr, R2->left/R2->width*R2->height, R2->top, R2->height, RADIANS(0), RADIANS(90));
      cairo_restore(cr);
 
      cairo_save(cr);
      cairo_new_sub_path(cr);
      cairo_scale(cr, R3->width/R3->height, 1);
      cairo_arc ( cr, R3->left/R3->width*R3->height, R3->bottom, R3->height, RADIANS(270), RADIANS(360));
      cairo_restore(cr);

      cairo_save(cr);
      cairo_new_sub_path(cr);
      cairo_scale(cr, 1, R4->height/R4->width);
      cairo_arc ( cr, R4->right, R4->bottom/R4->height*R4->width, R4->width, RADIANS(180), RADIANS(270));
      cairo_restore(cr);
 
      cairo_save(cr);
      cairo_new_sub_path(cr);
      cairo_scale(cr, 1, R5->height/R5->width);
      cairo_arc ( cr, R5->right, R5->top/R5->height*R5->width, R5->width, RADIANS(90), RADIANS(180));
      cairo_restore(cr);
  
      cairo_save(cr);
      cairo_new_sub_path(cr);
      cairo_scale(cr, 1, R6->height/R6->width);
      cairo_arc ( cr, R6->left, R6->top/R6->height*R6->width, R6->width, RADIANS(0), RADIANS(90));
      cairo_restore(cr);
  
      cairo_save(cr);
      cairo_new_sub_path(cr);
      cairo_scale(cr, R7->width/R7->height, 1);
      cairo_arc ( cr, R7->left/R7->width*R7->height, R7->bottom, R7->height, RADIANS(270), RADIANS(360));
      cairo_restore(cr);

      cairo_save(cr);
      cairo_new_sub_path(cr);
      cairo_scale(cr, (R6->width-R7->width)/R7->height, 1);
      cairo_arc ( cr, R7->left/(R6->width-R7->width)*R7->height, R7->bottom, R7->height, RADIANS(210), RADIANS(270));
      cairo_restore(cr);
    }
}

void draw_simple_grid(cairo_t *cr, float wd, float ht, float zoom_scale)
{
  // cairo_set_operator(cr, CAIRO_OPERATOR_XOR);
  cairo_set_line_width(cr, 1.0/zoom_scale);
  cairo_set_source_rgb(cr, .2, .2, .2);
  dt_draw_grid(cr, 3, wd, ht);
  cairo_translate(cr, 1.0/zoom_scale, 1.0/zoom_scale);
  cairo_set_source_rgb(cr, .8, .8, .8);
  dt_draw_grid(cr, 3, wd, ht);
  cairo_set_source_rgba(cr, .8, .8, .8, 0.5);
  double dashes = 5.0/zoom_scale;
  cairo_set_dash(cr, &dashes, 1, 0);
  dt_draw_grid(cr, 9, wd, ht);
}

// draw 3x3 grid over the image
void gui_post_expose(struct dt_iop_module_t *self, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx, int32_t pointery)
{
  dt_develop_t *dev = self->dev;
  dt_iop_clipping_gui_data_t *g = (dt_iop_clipping_gui_data_t *)self->gui_data;
  int32_t zoom, closeup;
  float zoom_x, zoom_y;
  float wd = dev->preview_pipe->backbuf_width;
  float ht = dev->preview_pipe->backbuf_height;
  DT_CTL_GET_GLOBAL(zoom_y, dev_zoom_y);
  DT_CTL_GET_GLOBAL(zoom_x, dev_zoom_x);
  DT_CTL_GET_GLOBAL(zoom, dev_zoom);
  DT_CTL_GET_GLOBAL(closeup, dev_closeup);
  float zoom_scale = dt_dev_get_zoom_scale(dev, zoom, closeup ? 2 : 1, 1);

  cairo_translate(cr, width/2.0, height/2.0f);
  cairo_scale(cr, zoom_scale, zoom_scale);
  cairo_translate(cr, -.5f*wd-zoom_x*wd, -.5f*ht-zoom_y*ht);

//   // cairo_set_operator(cr, CAIRO_OPERATOR_XOR);
//   cairo_set_line_width(cr, 1.0/zoom_scale);
//   cairo_set_source_rgb(cr, .2, .2, .2);
//   dt_draw_grid(cr, 3, wd, ht);
//   cairo_translate(cr, 1.0/zoom_scale, 1.0/zoom_scale);
//   cairo_set_source_rgb(cr, .8, .8, .8);
//   dt_draw_grid(cr, 3, wd, ht);
//   cairo_set_source_rgba(cr, .8, .8, .8, 0.5);
  int which = gtk_combo_box_get_active(g->guide_lines);
  if (GUIDE_GRID == which) {
    draw_simple_grid(cr, wd, ht, zoom_scale);
  }
  double dashes = 5.0/zoom_scale;
//   cairo_set_dash(cr, &dashes, 1, 0);
//   dt_draw_grid(cr, 9, wd, ht);

  // draw cropping window handles:
  float pzx, pzy;
  dt_dev_get_pointer_zoom_pos(dev, pointerx, pointery, &pzx, &pzy);
  pzx += 0.5f; pzy += 0.5f;
  cairo_set_dash (cr, &dashes, 0, 0);
  cairo_set_source_rgba(cr, .3, .3, .3, .8);
  cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
  cairo_rectangle (cr, 0, 0, wd, ht);
  cairo_rectangle (cr, g->clip_x*wd, g->clip_y*ht, g->clip_w*wd, g->clip_h*ht);
  cairo_fill (cr);

  cairo_stroke (cr);


  // draw crop area guides 
  int left, top, right, bottom, xThird, yThird;
  left = g->clip_x*wd;
  top = g->clip_y*ht;
  right = g->clip_x*wd + g->clip_w*wd;
  bottom = g->clip_y*ht + g->clip_h*ht;
  int cwidth = g->clip_w*wd;
  int cheight = g->clip_h*ht;
  xThird = cwidth  / 3;
  yThird = cheight / 3;

  // save context and draw guides
  cairo_save(cr);
  cairo_rectangle (cr, left, top, cwidth, cheight);
  cairo_clip(cr);
  //p.setPen(QPen(Qt::white, d->guideSize, Qt::SolidLine));
  cairo_set_line_width(cr, 1.0/zoom_scale);
  cairo_set_source_rgb(cr, .8, .8, .8);
  cairo_set_dash(cr, &dashes, 1, 0);
  if (which == GUIDE_DIAGONAL) {
    drawDiagonalMethod(cr, left, top, cwidth, cheight);
    cairo_stroke (cr);
    cairo_set_dash (cr, &dashes, 0, 0);
    cairo_set_source_rgba(cr, .3, .3, .3, .8);
    //p.setPen(QPen(d->guideColor, d->guideSize, Qt::DotLine));
    drawDiagonalMethod(cr, left, top, cwidth, cheight);
    cairo_stroke (cr);
  } else if (which == GUIDE_THIRD) {
    drawRulesOfThirds(cr, left, top,  right, bottom, xThird, yThird);
    cairo_stroke (cr);
    //p.setPen(QPen(d->guideColor, d->guideSize, Qt::DotLine));
    cairo_set_dash (cr, &dashes, 0, 0);
    cairo_set_source_rgba(cr, .3, .3, .3, .8);
    drawRulesOfThirds(cr, left, top,  right, bottom, xThird, yThird);
    cairo_stroke (cr);
  } else if (which == GUIDE_TRIANGL) {
    int dst = (int)((cheight*cos(atan(cwidth/cheight)) / (cos(atan(cheight/cwidth)))));
    // Move coordinates to local center selection.
    cairo_translate(cr, ((right - left)/2+left), ((bottom - top)/2+top));

    // Flip horizontal.
    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g->flipHorGoldenGuide)))
      cairo_scale(cr, -1, 1);
    // Flip vertical.
    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g->flipVerGoldenGuide)))
      cairo_scale(cr, 1, -1);

    drawHarmoniousTriangles(cr, left, top,  right, bottom, dst);
    cairo_stroke (cr);
    //p.setPen(QPen(d->guideColor, d->guideSize, Qt::DotLine));
    cairo_set_dash (cr, &dashes, 0, 0);
    cairo_set_source_rgba(cr, .3, .3, .3, .8);
    drawHarmoniousTriangles(cr, left, top,  right, bottom, dst);
    cairo_stroke (cr);

  } else if (which == GUIDE_GOLDEN) {
    // Move coordinates to local center selection.
    cairo_translate(cr, ((right - left)/2+left), ((bottom - top)/2+top));

    // Flip horizontal.
    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g->flipHorGoldenGuide)))
      cairo_scale(cr, -1, 1);
    // Flip vertical.
    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g->flipVerGoldenGuide)))
      cairo_scale(cr, 1, -1);

    int w = cwidth;
    int h = cheight;

    // lengths for the golden mean and half the sizes of the region:
    int w_g = (int)(w*INVPHI);
    int h_g = (int)(h*INVPHI);
    int w_2 = w/2;
    int h_2 = h/2;

    QRect R1, R2, R3, R4, R5, R6, R7;
    qRect (&R1, -w_2, -h_2, w_g, h);

    // w - 2*w_2 corrects for one-pixel difference
    // so that R2.right() is really at the right end of the region
    qRect (&R2, w_g-w_2, h_2-h_g, w-w_g+1-(w - 2*w_2), h_g);

    qRect (&R3, (int)(w_2 - R2.width*INVPHI), -h_2,
	      (int)(R2.width*INVPHI), h - R2.height);

    qRect (&R4, R2.left, R1.top, R3.left - R2.left,
	      (int)(R3.height*INVPHI));

    qRect (&R5, R4.left, R4.bottom, (int)(R4.width*INVPHI),
	      R3.height - R4.height);

    qRect (&R6, R5.left + R5.width, R5.bottom - (int)(R5.height*INVPHI),
	      R3.left - R5.right, (int)(R5.height*INVPHI));

    qRect (&R7, R6.right - (int)(R6.width*INVPHI), R4.bottom,
	      (int)(R6.width*INVPHI), R5.height - R6.height);

    //p.setPen(QPen(Qt::white, d->guideSize, Qt::SolidLine));
    drawGoldenMean(self, cr, &R1, &R2, &R3, &R4, &R5, &R6, &R7);
    cairo_stroke (cr);

    //p.setPen(QPen(d->guideColor, d->guideSize, Qt::DotLine));
    cairo_set_dash (cr, &dashes, 0, 0);
    cairo_set_source_rgba(cr, .3, .3, .3, .8);
    drawGoldenMean(self, cr, &R1, &R2, &R3, &R4, &R5, &R6, &R7);
    cairo_stroke (cr);

  }
  cairo_restore(cr);

  cairo_set_line_width(cr, 2.0/zoom_scale);
  cairo_set_source_rgb(cr, .3, .3, .3);
  const int border = 30.0/zoom_scale;
  if(g->straightening)
  {
    float bzx = g->button_down_zoom_x + .5f, bzy = g->button_down_zoom_y + .5f;
    cairo_arc (cr, bzx*wd, bzy*ht, 3, 0, 2.0*M_PI);
    cairo_stroke (cr);
    cairo_arc (cr, pzx*wd, pzy*ht, 3, 0, 2.0*M_PI);
    cairo_stroke (cr);
    cairo_move_to (cr, bzx*wd, bzy*ht);
    cairo_line_to (cr, pzx*wd, pzy*ht);
    cairo_stroke (cr);
  }
  else
  {
    int grab = g->cropping ? g->cropping : get_grab (pzx, pzy, g, border, wd, ht);
    if(grab == 1)  cairo_rectangle (cr, g->clip_x*wd, g->clip_y*ht, border, g->clip_h*ht);
    if(grab == 2)  cairo_rectangle (cr, g->clip_x*wd, g->clip_y*ht, g->clip_w*wd, border);
    if(grab == 3)  cairo_rectangle (cr, g->clip_x*wd, g->clip_y*ht, border, border);
    if(grab == 4)  cairo_rectangle (cr, (g->clip_x+g->clip_w)*wd-border, g->clip_y*ht, border, g->clip_h*ht);
    if(grab == 8)  cairo_rectangle (cr, g->clip_x*wd, (g->clip_y+g->clip_h)*ht-border, g->clip_w*wd, border);
    if(grab == 12) cairo_rectangle (cr, (g->clip_x+g->clip_w)*wd-border, (g->clip_y+g->clip_h)*ht-border, border, border);
    if(grab == 6)  cairo_rectangle (cr, (g->clip_x+g->clip_w)*wd-border, g->clip_y*ht, border, border);
    if(grab == 9)  cairo_rectangle (cr, g->clip_x*wd, (g->clip_y+g->clip_h)*ht-border, border, border);
    cairo_stroke (cr);
  }
}

int mouse_moved(struct dt_iop_module_t *self, double x, double y, int which)
{
  dt_iop_clipping_gui_data_t *g = (dt_iop_clipping_gui_data_t *)self->gui_data;
  int32_t zoom, closeup;
  float wd = self->dev->preview_pipe->backbuf_width;
  float ht = self->dev->preview_pipe->backbuf_height;
  DT_CTL_GET_GLOBAL(zoom, dev_zoom);
  DT_CTL_GET_GLOBAL(closeup, dev_closeup);
  float zoom_scale = dt_dev_get_zoom_scale(self->dev, zoom, closeup ? 2 : 1, 1);
  float pzx, pzy;
  dt_dev_get_pointer_zoom_pos(self->dev, x, y, &pzx, &pzy);
  pzx += 0.5f; pzy += 0.5f;
  int grab = get_grab (pzx, pzy, g, 30.0/zoom_scale, wd, ht);

  if(darktable.control->button_down && darktable.control->button_down_which == 1)
  {
    float bzx = g->button_down_zoom_x + .5f, bzy = g->button_down_zoom_y + .5f;
    if(!g->cropping && !g->straightening)
    {
      g->cropping = grab;
      if(grab & 1) g->handle_x = bzx-g->clip_x;
      if(grab & 2) g->handle_y = bzy-g->clip_y;
      if(grab & 4) g->handle_x = bzx-(g->clip_w + g->clip_x);
      if(grab & 8) g->handle_y = bzy-(g->clip_h + g->clip_y);
      if(!grab) g->straightening = 1;
    }
    if(!g->straightening)
    {
      grab = g->cropping;

      if(grab & 1)
      {
        const float old_clip_x = g->clip_x;
        g->clip_x = fmaxf(0.0, pzx - g->handle_x);
        g->clip_w = fmaxf(0.1, old_clip_x + g->clip_w - g->clip_x);
      }
      if(grab & 2)
      {
        const float old_clip_y = g->clip_y;
        g->clip_y = fmaxf(0.0, pzy - g->handle_y);
        g->clip_h = fmaxf(0.1, old_clip_y + g->clip_h - g->clip_y);
      }
      if(grab & 4) g->clip_w = fmaxf(0.1, fminf(1.0, pzx - g->clip_x - g->handle_x));
      if(grab & 8) g->clip_h = fmaxf(0.1, fminf(1.0, pzy - g->clip_y - g->handle_y));

      if(g->clip_x + g->clip_w > 1.0) g->clip_w = 1.0 - g->clip_x;
      if(g->clip_y + g->clip_h > 1.0) g->clip_h = 1.0 - g->clip_y;
      apply_box_aspect(self, grab);
    }
    dt_control_gui_queue_draw();
    return 1;
  }
  else if (grab)
  { // hover over active borders
    dt_control_gui_queue_draw();
  }
  else
  { // somewhere besides borders. maybe rotate?
    g->straightening = g->cropping = 0;
    dt_control_gui_queue_draw();
  }
  return 0;
}

static void
commit_box (dt_iop_module_t *self, dt_iop_clipping_gui_data_t *g, dt_iop_clipping_params_t *p)
{
  g->cropping = 0;
  p->aspect = -gtk_spin_button_get_value(g->aspect);
  const float cx = p->cx, cy = p->cy;
  const float cw = fabsf(p->cw), ch = fabsf(p->ch);
  p->cx += g->clip_x*(cw-cx);
  p->cy += g->clip_y*(ch-cy);
  p->cw = copysignf(p->cx + (cw - cx)*g->clip_w, p->cw);
  p->ch = copysignf(p->cy + (ch - cy)*g->clip_h, p->ch);
  g->clip_x = g->clip_y = 0.0f;
  g->clip_w = g->clip_h = 1.0;
  darktable.gui->reset = 1;
  self->gui_update(self);
  if(self->off) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->off), 1);
  darktable.gui->reset = 0;
  dt_dev_add_history_item(darktable.develop, self);
}

int button_released(struct dt_iop_module_t *self, double x, double y, int which, uint32_t state)
{
  dt_iop_clipping_gui_data_t *g = (dt_iop_clipping_gui_data_t *)self->gui_data;
  if(g->straightening)
  {
    float dx = x - darktable.control->button_x, dy = y - darktable.control->button_y;
    if(dx < 0) { dx = -dx; dy = - dy; }
    const float angle = atan2f(dy, dx);
    assert(angle >= - M_PI/2.0 && angle <= M_PI/2.0);
    float close = angle;
    if     (close >  M_PI/4.0) close =  M_PI/2.0 - close;
    else if(close < -M_PI/4.0) close = -M_PI/2.0 - close;
    else close = - close;
    float a = 180.0/M_PI*close + g->button_down_angle;
    if(a < -180.0) a += 360.0;
    if(a >  180.0) a -= 360.0;
    if(self->off) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->off), 1);
    dtgtk_slider_set_value(g->scale5, a);
  }
  g->straightening = g->cropping = 0;
  return 1;
}

int button_pressed(struct dt_iop_module_t *self, double x, double y, int which, int type, uint32_t state)
{
  dt_iop_clipping_gui_data_t *g = (dt_iop_clipping_gui_data_t *)self->gui_data;
  dt_iop_clipping_params_t   *p = (dt_iop_clipping_params_t   *)self->params;
  if(which == 1 && darktable.control->button_type == GDK_2BUTTON_PRESS)
  {
    commit_box(self, g, p);
    return 1;
  }
  else if(which == 1)
  {
    dt_dev_get_pointer_zoom_pos(self->dev, x, y, &g->button_down_zoom_x, &g->button_down_zoom_y);
    g->button_down_angle = p->angle;
    return 1;
  }
  else return 0;
}

int key_pressed (struct dt_iop_module_t *self, uint16_t which)
{
  dt_iop_clipping_gui_data_t *g = (dt_iop_clipping_gui_data_t *)self->gui_data;
  dt_iop_clipping_params_t   *p = (dt_iop_clipping_params_t   *)self->params;
  switch (which)
  {
    case KEYCODE_Return:
      commit_box(self, g, p);
      return TRUE;
    default:
      break;
  }
  return FALSE;
}

