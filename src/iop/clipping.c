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
#include "gui/gtk.h"
#include "gui/draw.h"

DT_MODULE(1)

/** rotate an image, then clip the buffer. */

typedef struct dt_iop_clipping_params_t
{
  float angle, cx, cy, cw, ch;
}
dt_iop_clipping_params_t;

typedef struct dt_iop_clipping_gui_data_t
{
  GtkVBox *vbox1, *vbox2;
  GtkLabel *label1, *label2, *label3, *label4, *label5;
  GtkHScale *scale1, *scale2, *scale3, *scale4, *scale5;
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

  // rotate and clip to max extent
  roi_out->x      = d->tx - (.5f - d->cx)*cropscale*roi_in->width;
  roi_out->y      = d->ty - (.5f - d->cy)*cropscale*roi_in->height;
  roi_out->width  = (d->cw-d->cx)*cropscale*roi_in->width;
  roi_out->height = (d->ch-d->cy)*cropscale*roi_in->height;
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
  for(int j=0;j<roi_out->height;j++)
  {
    const float tmppi[2] = {pi[0], pi[1]};
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
    for(int k=0;k<2;k++) pi[k] = tmppi[k];
    for(int k=0;k<2;k++) pi[k] += dy[k];
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
  d->cw = p->cw;
  d->ch = p->ch;
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

void cx_callback (GtkRange *range, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_clipping_params_t *p = (dt_iop_clipping_params_t *)self->params;
  p->cx = gtk_range_get_value(range);
  dt_dev_add_history_item(darktable.develop, self);
}

void cy_callback (GtkRange *range, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_clipping_params_t *p = (dt_iop_clipping_params_t *)self->params;
  p->cy = gtk_range_get_value(range);
  dt_dev_add_history_item(darktable.develop, self);
}

void cw_callback (GtkRange *range, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_clipping_params_t *p = (dt_iop_clipping_params_t *)self->params;
  p->cw = gtk_range_get_value(range);
  dt_dev_add_history_item(darktable.develop, self);
}

void ch_callback (GtkRange *range, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_clipping_params_t *p = (dt_iop_clipping_params_t *)self->params;
  p->ch = gtk_range_get_value(range);
  dt_dev_add_history_item(darktable.develop, self);
}

void angle_callback (GtkRange *range, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_clipping_params_t *p = (dt_iop_clipping_params_t *)self->params;
  p->angle = gtk_range_get_value(range);
  dt_dev_add_history_item(darktable.develop, self);
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_clipping_gui_data_t *g = (dt_iop_clipping_gui_data_t *)self->gui_data;
  dt_iop_clipping_params_t *p = (dt_iop_clipping_params_t *)self->params;
  gtk_range_set_value(GTK_RANGE(g->scale1), p->cx);
  gtk_range_set_value(GTK_RANGE(g->scale2), p->cy);
  gtk_range_set_value(GTK_RANGE(g->scale3), p->cw);
  gtk_range_set_value(GTK_RANGE(g->scale4), p->ch);
  gtk_range_set_value(GTK_RANGE(g->scale5), p->angle);
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
  dt_iop_clipping_params_t tmp = (dt_iop_clipping_params_t){0.0, 0.0, 0.0, 1.0, 1.0}; 
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

static gchar *fv_callback(GtkScale *scale, gdouble value)
{
  int digits = gtk_scale_get_digits(scale);
  return g_strdup_printf("%# *.*f", 4+1+digits, digits, value);
}

void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_clipping_gui_data_t));
  dt_iop_clipping_gui_data_t *g = (dt_iop_clipping_gui_data_t *)self->gui_data;
  dt_iop_clipping_params_t *p = (dt_iop_clipping_params_t *)self->params;

  self->widget = GTK_WIDGET(gtk_hbox_new(FALSE, 0));
  g->vbox1 = GTK_VBOX(gtk_vbox_new(FALSE, 0));
  g->vbox2 = GTK_VBOX(gtk_vbox_new(FALSE, 0));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->vbox1), FALSE, FALSE, 5);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->vbox2), TRUE, TRUE, 5);
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
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label1), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label2), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label3), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label4), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label5), TRUE, TRUE, 0);
  g->scale1 = GTK_HSCALE(gtk_hscale_new_with_range(0.0, 1.0, 0.01));
  g->scale2 = GTK_HSCALE(gtk_hscale_new_with_range(0.0, 1.0, 0.01));
  g->scale3 = GTK_HSCALE(gtk_hscale_new_with_range(0.0, 1.0, 0.01));
  g->scale4 = GTK_HSCALE(gtk_hscale_new_with_range(0.0, 1.0, 0.01));
  g->scale5 = GTK_HSCALE(gtk_hscale_new_with_range(-180.0, 180.0, 0.5));
  gtk_scale_set_digits(GTK_SCALE(g->scale1), 2);
  gtk_scale_set_digits(GTK_SCALE(g->scale2), 2);
  gtk_scale_set_digits(GTK_SCALE(g->scale3), 2);
  gtk_scale_set_digits(GTK_SCALE(g->scale4), 2);
  gtk_scale_set_digits(GTK_SCALE(g->scale5), 2);
  gtk_scale_set_value_pos(GTK_SCALE(g->scale1), GTK_POS_LEFT);
  gtk_scale_set_value_pos(GTK_SCALE(g->scale2), GTK_POS_LEFT);
  gtk_scale_set_value_pos(GTK_SCALE(g->scale3), GTK_POS_LEFT);
  gtk_scale_set_value_pos(GTK_SCALE(g->scale4), GTK_POS_LEFT);
  gtk_scale_set_value_pos(GTK_SCALE(g->scale5), GTK_POS_LEFT);
  gtk_range_set_value(GTK_RANGE(g->scale1), p->cx);
  gtk_range_set_value(GTK_RANGE(g->scale2), p->cy);
  gtk_range_set_value(GTK_RANGE(g->scale3), p->cw);
  gtk_range_set_value(GTK_RANGE(g->scale4), p->ch);
  gtk_range_set_value(GTK_RANGE(g->scale5), p->angle);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale1), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale2), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale3), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale4), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale5), TRUE, TRUE, 0);

  g_signal_connect (G_OBJECT (g->scale1), "format-value",
                    G_CALLBACK (fv_callback), self);
  g_signal_connect (G_OBJECT (g->scale2), "format-value",
                    G_CALLBACK (fv_callback), self);
  g_signal_connect (G_OBJECT (g->scale3), "format-value",
                    G_CALLBACK (fv_callback), self);
  g_signal_connect (G_OBJECT (g->scale4), "format-value",
                    G_CALLBACK (fv_callback), self);
  g_signal_connect (G_OBJECT (g->scale5), "format-value",
                    G_CALLBACK (fv_callback), self);
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
    gtk_range_set_value(GTK_RANGE(g->scale5), angle);
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

