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
  float button_down_zoom_x, button_down_zoom_y, button_down_angle; // position in image where the button has been pressed.
}
dt_iop_clipping_gui_data_t;

typedef struct dt_iop_clipping_data_t
{
  float angle;              // rotation angle
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
  return _("clipping");
}

// 1st pass: how large would the output be, given this input roi?
// this is always called with the full buffer before processing.
void modify_roi_out(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, dt_iop_roi_t *roi_out, const dt_iop_roi_t *roi_in)
{
  *roi_out = *roi_in;
  dt_iop_clipping_data_t *d = (dt_iop_clipping_data_t *)piece->data;
  dt_iop_clipping_params_t *pm = (dt_iop_clipping_params_t *)self->params;

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
  if(pm->aspect > 0.0)
  {
    const float ch = acw * roi_in->width / pm->aspect  / (roi_in->height);
    const float cw = pm->aspect * ach * roi_in->height / (roi_in->width);
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
  if(self->dt->gui->reset) return;
  dt_iop_clipping_params_t *p = (dt_iop_clipping_params_t *)self->params;
  p->aspect = gtk_spin_button_get_value(widget);
  dt_dev_add_history_item(darktable.develop, self);
}

static void
aspect_on_callback(GtkCheckButton *widget, dt_iop_module_t *self)
{
  if(self->dt->gui->reset) return;
  dt_iop_clipping_gui_data_t *g = (dt_iop_clipping_gui_data_t *)self->gui_data;
  dt_iop_clipping_params_t *p = (dt_iop_clipping_params_t *)self->params;
  gboolean active = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));
  gtk_widget_set_sensitive(GTK_WIDGET(g->aspect), active);
  if(active)
    p->aspect =   gtk_spin_button_get_value(g->aspect);
  else
    p->aspect = - gtk_spin_button_get_value(g->aspect);
  if(self->off) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->off), 1);
  dt_dev_add_history_item(darktable.develop, self);
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

void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_clipping_gui_data_t));
  dt_iop_clipping_gui_data_t *g = (dt_iop_clipping_gui_data_t *)self->gui_data;
  dt_iop_clipping_params_t *p = (dt_iop_clipping_params_t *)self->params;

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

  gtk_object_set (GTK_OBJECT(g->hflip), "tooltip-text", _("horizontal flip"), NULL);
  gtk_object_set (GTK_OBJECT(g->vflip), "tooltip-text", _("vertical flip"), NULL);

  g->aspect_on = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("aspect")));
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->aspect_on), TRUE, TRUE, 0);
  gtk_object_set (GTK_OBJECT(g->aspect_on), "tooltip-text", _("fixed aspect ratio"), NULL);
  g_signal_connect (G_OBJECT (g->aspect_on), "toggled",
                    G_CALLBACK (aspect_on_callback), self);
  g->aspect = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(0.1, 10.0, 0.01));
  gtk_spin_button_set_increments(g->aspect, 0.01, 0.2);
  gtk_spin_button_set_digits(g->aspect, 2);
  gtk_widget_set_sensitive(GTK_WIDGET(g->aspect), FALSE);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->aspect), FALSE, FALSE, 0);
  g_signal_connect (G_OBJECT (g->aspect), "value-changed",
                    G_CALLBACK (aspect_callback), self);

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
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

// draw 3x3 grid over the image
void gui_post_expose(struct dt_iop_module_t *self, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx, int32_t pointery)
{
  dt_develop_t *dev = self->dev;
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

// TODO: if mode crop
int mouse_moved(struct dt_iop_module_t *self, double x, double y, int which)
{
  if(darktable.control->button_down && darktable.control->button_down_which == 1)
  {
    dt_iop_clipping_gui_data_t *g = (dt_iop_clipping_gui_data_t *)self->gui_data;
    float zoom_x, zoom_y;
    dt_dev_get_pointer_zoom_pos(self->dev, x, y, &zoom_x, &zoom_y);
    float old_angle = atan2f(g->button_down_zoom_y, g->button_down_zoom_x);
    float angle     = atan2f(zoom_y, zoom_x);
    angle = fmaxf(-180.0, fminf(180.0, g->button_down_angle + 180.0/M_PI * (angle - old_angle)));
    dtgtk_slider_set_value(g->scale5, angle);
    dt_control_gui_queue_draw();
    return 1;
  }
  else return 0;
}

int button_pressed(struct dt_iop_module_t *self, double x, double y, int which, int type, uint32_t state)
{
  if(which == 1)
  {
    dt_iop_clipping_gui_data_t *g = (dt_iop_clipping_gui_data_t *)self->gui_data;
    dt_iop_clipping_params_t   *p = (dt_iop_clipping_params_t   *)self->params;
    dt_dev_get_pointer_zoom_pos(self->dev, x, y, &g->button_down_zoom_x, &g->button_down_zoom_y);
    g->button_down_angle = p->angle;
    return 1;
  }
  else return 0;
}

