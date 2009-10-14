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
}
dt_iop_clipping_gui_data_t;

typedef struct dt_iop_clipping_data_t
{
  float angle;              // rotation angle
  float m[9], invm[9];      // rot matrix
  float tx, ty;             // rotation center
  float cx, cy, cw, ch;     // crop window
  float cix, ciy, ciw, cih; // crop window on roi_out 1.0 scale
}
dt_iop_clipping_data_t;

void mul_mat_vec_2(const float *m, const float *p, float *o)
{
  o[0] = p[0]*m[0] + p[1]*m[1] + m[2];
  o[1] = p[0]*m[3] + p[1]*m[4] + m[5];
}

void mul_mat_mat_2(const float *m1, const float *m2, float *res)
{
  for(int j=0;j<3;j++) for(int i=0;i<3;i++)
  {
    res[3*j+i] = 0.0f;
    for(int k=0;k<3;k++)
      res[3*j+i] += m1[3*j+k]*m2[3*k+i];
  }
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

void modify_roi_out(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, dt_iop_roi_t *roi_out, const dt_iop_roi_t *roi_in)
{
  *roi_out = *roi_in;
  dt_iop_clipping_data_t *d = (dt_iop_clipping_data_t *)piece->data;
  float sw = roi_in->width, sh = roi_in->height, s = roi_out->scale/roi_in->scale;
  // modify cx/cy/.. to be on scale of roi out: (0..1) of roi in size
  d->cix = d->cx * sw*s; d->ciy = d->cy * sh*s;
  d->ciw = d->cw * sw*s; d->cih = d->ch * sh*s;
  // printf("set clip: %f %f %f %f scale: %f -> %f\n", d->cx, d->cy, d->cw, d->ch, roi_out->scale, roi_in->scale);
  // int clipx = d->cix, clipy = d->ciy, clipw = d->ciw, cliph = d->cih;
  // if(roi_out->x < clipx) roi_out->x = clipx;
  // if(roi_out->width > clipw) roi_out->width = clipw;
  // if(roi_out->y < clipy) roi_out->y = clipy;
  // if(roi_out->height > cliph) roi_out->height = cliph;
  // if(roi_out->x + roi_out->width > clipx + clipw) roi_out->width = clipw - roi_out->x;
  // if(roi_out->y + roi_out->height > clipy + cliph) roi_out->height = cliph - roi_out->y;
  // printf("clipping: in (%d %d %d %d) -> (%d %d %d %d)\n", roi_in->x, roi_in->y, roi_in->width, roi_in->height, roi_out->x, roi_out->y, roi_out->width, roi_out->height);

  // use whole-buffer roi information to create matrix and inverse.
  float rt[] = { cosf(d->angle),-sinf(d->angle), 0.0f,
                 sinf(d->angle), cosf(d->angle), 0.0f,
                           0.0f,           0.0f, 1.0f};
#if 1
  // fwd transform rotated points on corners and scale back inside roi_in bounds.
  float cropscale = 1.0f;
#if 1
  float p[2], o[2], aabb[4] = {-.5f*roi_in->width, -.5f*roi_in->height, .5f*roi_in->width, .5f*roi_in->height};
  // for(int k=0;k<4;k++) aabb[k] *= 1.0f/roi_in->scale;
  for(int c=0;c<4;c++)
  {
    get_corner(aabb, c, p);
    mul_mat_vec_2(rt, p, o);
    for(int k=0;k<2;k++) if(fabsf(o[k]) > 0.001f) cropscale = fminf(cropscale, aabb[(o[k] > 0 ? 2 : 0) + k]/o[k]);
  }
#endif

  // modify roi_out by scaling inside rotated buffer.
  d->cix += (1.0 - cropscale)*.5f*d->ciw;
  d->ciy += (1.0 - cropscale)*.5f*d->cih;
  d->ciw *= cropscale;
  d->cih *= cropscale;
  // roi_out->x += (1.0 - cropscale)*.5f*roi_out->width;
  // roi_out->y += (1.0 - cropscale)*.5f*roi_out->height;
  // roi_out->width  *= cropscale;
  // roi_out->height *= cropscale;
  int clipx = d->cix, clipy = d->ciy, clipw = d->ciw, cliph = d->cih;
  if(roi_out->x < clipx) roi_out->x = clipx;
  if(roi_out->width > clipw) roi_out->width = clipw;
  if(roi_out->y < clipy) roi_out->y = clipy;
  if(roi_out->height > cliph) roi_out->height = cliph;

  float t1[] = {1.0f, 0.0f, 0.0f,//-roi_in->width *0.5f,
                0.0f, 1.0f, 0.0f,//-roi_in->height*0.5f,
                0.0f, 0.0f, 1.0f};
  float t2[] = {1.0f, 0.0f, 0.0f,//roi_in->width *0.5f,
                0.0f, 1.0f, 0.0f,//roi_in->height*0.5f,
                0.0f, 0.0f, 1.0f};
  float tmp[9];
  mul_mat_mat_2(t2, rt, tmp);
  mul_mat_mat_2(tmp, t1, d->invm); // in => out
  rt[1] = - rt[1];
  rt[3] = - rt[3];
  // t1[2] = - t1[2];
  // t1[5] = - t1[5];
  // t2[2] = - t2[2];
  // t2[5] = - t2[5];
  mul_mat_mat_2(t2, rt, tmp);
  mul_mat_mat_2(tmp, t1, d->m);    // out => in
  d->tx = roi_in->width  * .5f;
  d->ty = roi_in->height * .5f;

#if 0
  printf("matrix m = \n");
  printf(" %.2f %.2f %.2f\n", d->m[0], d->m[1], d->m[2]);
  printf(" %.2f %.2f %.2f\n", d->m[3], d->m[4], d->m[5]);
  printf(" %.2f %.2f %.2f\n", d->m[6], d->m[7], d->m[8]);

  mul_mat_mat_2(d->m, d->invm, tmp);
  printf("matrix tmp = \n");
  printf(" %.2f %.2f %.2f\n", tmp[0], tmp[1], tmp[2]);
  printf(" %.2f %.2f %.2f\n", tmp[3], tmp[4], tmp[5]);
  printf(" %.2f %.2f %.2f\n", tmp[6], tmp[7], tmp[8]);
#endif


  // printf("cropscale: %f\n", cropscale);
  // printf("clipping: in (%d %d %d %d) -> (%d %d %d %d)\n", roi_in->x, roi_in->y, roi_in->width, roi_in->height, roi_out->x, roi_out->y, roi_out->width, roi_out->height);
#endif
}

void modify_roi_in(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, const dt_iop_roi_t *roi_out, dt_iop_roi_t *roi_in)
{
  dt_iop_clipping_data_t *d = (dt_iop_clipping_data_t *)piece->data;
  *roi_in = *roi_out;
  // modify_roi_out took care of bounds checking for us. we hopefully do not get requests outside the clipping area.
  // transform (unclipped) aabb back to roi_in
  // const float s = roi_in->scale/roi_out->scale;
  // roi_in->width  = roi_out->width*s;
  // roi_in->height = roi_out->height*s;
  // roi_in->x = roi_out->x*s + d->cix*s*roi_out->scale;
  // roi_in->y = roi_out->y*s + d->ciy*s*roi_out->scale;

  // printf("clip: %f %f %f %f scale: %f -> %f\n", d->cx, d->cy, d->cw, d->ch, roi_out->scale, roi_in->scale);
  // printf("clipping: out (%d %d %d %d) -> (%d %d %d %d)\n", roi_out->x, roi_out->y, roi_out->width, roi_out->height, roi_in->x, roi_in->y, roi_in->width, roi_in->height);
  // return;

  // this aabb is set off by cx/cy
  const float so = roi_out->scale;
  float p[2], o[2], aabb[4] = {roi_out->x+d->cix*so, roi_out->y+d->ciy*so, roi_out->x+d->cix*so+roi_out->width, roi_out->y+d->ciy*so+roi_out->height};
  // float p[2], o[2], aabb[4] = {roi_out->x, roi_out->y, roi_out->x+roi_out->width, roi_out->y+roi_out->height};
  float aabb_in[4] = {INFINITY, INFINITY, -INFINITY, -INFINITY};
  // for(int k=0;k<4;k++) aabb[k] *= 1.0f/roi_out->scale;
  for(int c=0;c<4;c++)
  {
    // get corner points of roi_out
    get_corner(aabb, c, p);
    // backtransform aabb using m
    p[0] -= d->tx*so; p[1] -= d->ty*so;
    mul_mat_vec_2(d->m, p, o);
    o[0] += d->tx*so; o[1] += d->ty*so;
    // transform to roi_in space, get aabb.
    // o[0] *= roi_in->scale; o[1] *= roi_in->scale;
    adjust_aabb(o, aabb_in);
  }
  // adjust roi_in to maximally needed region
  roi_in->x      = aabb_in[0];
  roi_in->y      = aabb_in[1];
  roi_in->width  = aabb_in[2]-aabb_in[0];
  roi_in->height = aabb_in[3]-aabb_in[1];
  // roi_in->x = fminf(roi_in->x, aabb_in[0]);
  // roi_in->y = fminf(roi_in->y, aabb_in[1]);
  // roi_in->width  = fmaxf(roi_in->width,  aabb_in[2]-roi_in->x);
  // roi_in->height = fmaxf(roi_in->height, aabb_in[3]-roi_in->y);
  // printf("clipping: out (%d %d %d %d) -> (%d %d %d %d)\n", roi_out->x, roi_out->y, roi_out->width, roi_out->height, roi_in->x, roi_in->y, roi_in->width, roi_in->height);
}

void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *i, void *o, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_clipping_data_t *d = (dt_iop_clipping_data_t *)piece->data;
  float *in  = (float *)i;
  float *out = (float *)o;
  float p1[2], p2[2];
  for(int j=0;j<roi_out->height;j++) for(int i=0;i<roi_out->width;i++)
  {
    // get whole-buffer point from i,j
    p1[0] = (roi_out->x + roi_out->scale*d->cix + i);// /roi_out->scale;
    p1[1] = (roi_out->y + roi_out->scale*d->ciy + j);// /roi_out->scale;
    // transform this point from using matrix m
    p1[0] -= d->tx*roi_out->scale; p1[1] -= d->ty*roi_out->scale;
    mul_mat_vec_2(d->m, p1, p2);
    p2[0] += d->tx*roi_in->scale;  p2[1] += d->ty*roi_in->scale;
    // transform this point to roi_in
    // p2[0] *= roi_in->scale; p2[1] *= roi_in->scale;
    // if(p2[0] < roi_in->x || p2[0] > roi_in->x + roi_in->width)  printf("point outside x: %f  | %d %d \n", p2[0], roi_in->x, roi_in->x+roi_in->width);
    // if(p2[1] < roi_in->y || p2[1] > roi_in->y + roi_in->height) printf("point outside y: %f  | %d %d \n", p2[1], roi_in->y, roi_in->y+roi_in->height);
    p2[0] -= roi_in->x; p2[1] -= roi_in->y;
    // set color
    // for(int c=0;c<3;c++) out[c] = in[3*roi_in->width*j + 3*i + c];
    // for(int c=0;c<3;c++) out[c] = ((roi_out->x+i)/10+(roi_out->y+j)/10) & 1 ? .7 : 0.0f;
    const int ii = (int)p2[0], jj = (int)p2[1];
    const float fi = p2[0] - ii, fj = p2[1] - jj;
    for(int c=0;c<3;c++) out[c] = // in[3*roi_in->width*(int)p2[1] + 3*(int)p2[0] + c];
          ((1.0f-fj)*(1.0f-fi)*in[3*(roi_in->width*(jj)   + (ii)  ) + c] +
           (1.0f-fj)*(     fi)*in[3*(roi_in->width*(jj)   + (ii+1)) + c] +
           (     fj)*(     fi)*in[3*(roi_in->width*(jj+1) + (ii+1)) + c] +
           (     fj)*(1.0f-fi)*in[3*(roi_in->width*(jj+1) + (ii)  ) + c]);
    out += 3;
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
  d->cw = (p->cw-p->cx);
  d->ch = (p->ch-p->cy);
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
  dt_iop_module_t *module = (dt_iop_module_t *)self;
  dt_iop_clipping_gui_data_t *g = (dt_iop_clipping_gui_data_t *)self->gui_data;
  dt_iop_clipping_params_t *p = (dt_iop_clipping_params_t *)module->params;
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
  module->priority = 95;
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
  g->label1 = GTK_LABEL(gtk_label_new("crop x"));
  g->label2 = GTK_LABEL(gtk_label_new("crop y"));
  g->label3 = GTK_LABEL(gtk_label_new("crop w"));
  g->label4 = GTK_LABEL(gtk_label_new("crop h"));
  g->label5 = GTK_LABEL(gtk_label_new("angle"));
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
  g->scale5 = GTK_HSCALE(gtk_hscale_new_with_range(0.0, 360, 0.5));
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

