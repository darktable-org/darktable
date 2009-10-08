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

/** clip and rotate an image. */

typedef struct dt_iop_clipping_params_t
{
  float angle, cx, cy, cw, ch;
}
dt_iop_clipping_params_t;

typedef struct dt_iop_clipping_gui_data_t
{
  GtkVBox *vbox1, *vbox2;
  GtkLabel *label1, *label2, *label3, *label4;
  GtkHScale *scale1, *scale2, *scale3, *scale4;
}
dt_iop_clipping_gui_data_t;

typedef struct dt_iop_clipping_data_t
{
  float m[4];          // rot matrix
  int cx, cy, cw, ch;  // crop window
}
dt_iop_clipping_data_t;

void mul_mat_vec2(const float *m, const float *p, float *o)
{
  o[0] = p[0]*m[0] + p[1]*m[1];
  o[1] = p[0]*m[2] + p[1]*m[3];
}

void modify_roi_out(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, dt_iop_roi_t *roi_out, const dt_iop_roi_t *roi_in)
{
  *roi_out = *roi_in;
  dt_iop_clipping_data_t *d = (dt_iop_clipping_data_t *)piece->data;
  float s = roi_out->scale;
  int clipx = d->cx * s, clipy = d->cy * s, clipw = d->cw * s, cliph = d->ch * s;
  if(roi_out->x > clipx) roi_out->x = clipx;
  if(roi_out->x + roi_out->width > clipx + clipw) roi_out->width = clipw - roi_out->x;
  if(roi_out->y > clipy) roi_out->y = clipy;
  if(roi_out->y + roi_out->height > clipy + cliph) roi_out->height = cliph - roi_out->y;
}

void modify_roi_in(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, const dt_iop_roi_t *roi_out, dt_iop_roi_t *roi_in)
{
  dt_iop_clipping_data_t *d = (dt_iop_clipping_data_t *)piece->data;
  *roi_in = *roi_out;
  const float s = roi_in->scale/roi_out->scale;
  roi_in->width  = roi_out->width*s;
  roi_in->height = roi_out->height*s;
  roi_in->x = roi_out->x*s + d->cx*roi_in->scale;
  roi_in->y = roi_out->y*s + d->cy*roi_in->scale;
#if 0
  // TODO: assert requested region is not clipped in roi_in
  int clipx = d->cx * roi_in.scale, clipy = d->cy * roi_in.scale, clipw = d->cw * roi_in.scale, cliph = d->ch * roi_in.scale;
  if(roi_out->x > clipx) roi_out->x = clipx;
  if(roi_out->x + roi_out->width > clipx + clipw) roi_out->width = clipw - roi_out->x;
  if(roi_out->y > clipy) roi_out->y = clipy;
  if(roi_out->y + roi_out->height > clipy + cliph) roi_out->height = cliph - roi_out->y;
#endif
#if 0
  if(piece->iscale != 1.0)
  { // preview pipeline
    *roi_in = *roi_out;
  }
  else
  { // inverse transform roi_out, use aabb of corner points
    // TODO: adjust scale of input (samplerate of one rotated line) max mip width!
    // roi_out is already adjusted to cropped area, we assume (gui will only want to view this)
    *roi_in = *roi_out;
    dt_iop_clipping_data_t *d = (dt_iop_clipping_data_t *)piece->data;
    float aabb[2] = {INFINITY, INFINITY, -INFINITY, -INFINITY};
    float p[2], o[2], *m = d->m;
    p[0] = roi_out->x; p[1] = roi_out->y;
    mul_mat_vec(m, p, o);
    aabb[0] = fminf(aabb[0], o[0]);
    aabb[1] = fminf(aabb[1], o[1]);
    aabb[2] = fmaxf(aabb[2], o[0]);
    aabb[3] = fmaxf(aabb[3], o[1]);
    p[0] = roi_out->x+roi_out->width; p[1] = roi_out->y;
    mul_mat_vec(m, p, o);
    aabb[0] = fminf(aabb[0], o[0]);
    aabb[1] = fminf(aabb[1], o[1]);
    aabb[2] = fmaxf(aabb[2], o[0]);
    aabb[3] = fmaxf(aabb[3], o[1]);
    p[0] = roi_out->x+roi_out->width; p[1] = roi_out->y+roi_out->height;
    mul_mat_vec(m, p, o);
    aabb[0] = fminf(aabb[0], o[0]);
    aabb[1] = fminf(aabb[1], o[1]);
    aabb[2] = fmaxf(aabb[2], o[0]);
    aabb[3] = fmaxf(aabb[3], o[1]);
    p[0] = roi_out->x; p[1] = roi_out->y+roi_out->height;
    mul_mat_vec(m, p, o);
    aabb[0] = fminf(aabb[0], o[0]);
    aabb[1] = fminf(aabb[1], o[1]);
    aabb[2] = fmaxf(aabb[2], o[0]);
    aabb[3] = fmaxf(aabb[3], o[1]);
    roi_in->x = aabb[0]; roi_in->y = aabb[1];
    roi_in->width = aabb[2] - aabb[0]; roi_in->height = aabb[3]-aabb[1];
    // TODO: if in->width > DT_IMAGE_WINDOW_SIZE adjust scale to fit inside.
  }
#endif
}

void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *i, void *o, int x, int y, float scale, int width, int height)
{
  // dt_iop_clipping_data_t *d = (dt_iop_clipping_data_t *)piece->data;
  float *in  = (float *)i;
  float *out = (float *)o;
  for(int j=0;j<height;j++) for(int i=0;i<width;i++)
  {
    for(int c=0;c<3;c++) out[c] = in[c];
    out += 3; in += 3;
#if 0
    p[0] = i; p[1] = j;
    mul_mat_vec2(d->m, p, o);
    const int   ii = (int)o[0], jj = (int)o[1];
    const float fi = o[0] - ii; fj = o[1] - jj;
    // TODO: pump through matrix: get inverse sample, interpolate
    for(int c=0;c<3;c++) out[c] = .25*(in[c];
    out += 3; in += 3;
#endif
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
#if 0
  // set up rotation matrix
  // TODO: rotate around center of the image!
  // TODO: adjust scale of input image
  const float a = M_PI/180.0 * p->angle;
  d->m[0] = cosf(a); d->m[1] = -sinf(a);
  d->m[2] = sinf(a); d->m[3] =  cosf(a); 
  // set crop in dev->image
  p[0] = roi_out->width; p[1] = 0.0;
  mul_mat_vec(d->m, p, o);
  d->scale = fabsf(o[0]);
  // TODO: inverse transform uncropped image corners and use inbox as max crop area!
#endif
  d->cx = p->cx*pipe->iwidth;
  d->cy = p->cy*pipe->iheight;
  d->cw = p->cw*pipe->iwidth;
  d->ch = p->ch*pipe->iheight;
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

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_module_t *module = (dt_iop_module_t *)self;
  dt_iop_clipping_gui_data_t *g = (dt_iop_clipping_gui_data_t *)self->gui_data;
  dt_iop_clipping_params_t *p = (dt_iop_clipping_params_t *)module->params;
  gtk_range_set_value(GTK_RANGE(g->scale1), p->cx);
  gtk_range_set_value(GTK_RANGE(g->scale2), p->cy);
  gtk_range_set_value(GTK_RANGE(g->scale3), p->cw);
  gtk_range_set_value(GTK_RANGE(g->scale4), p->ch);
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
  gtk_misc_set_alignment(GTK_MISC(g->label1), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(g->label2), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(g->label3), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(g->label4), 0.0, 0.5);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label1), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label2), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label3), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label4), TRUE, TRUE, 0);
  g->scale1 = GTK_HSCALE(gtk_hscale_new_with_range(0.0, 1.0, 0.01));
  g->scale2 = GTK_HSCALE(gtk_hscale_new_with_range(0.0, 1.0, 0.01));
  g->scale3 = GTK_HSCALE(gtk_hscale_new_with_range(0.0, 1.0, 0.01));
  g->scale4 = GTK_HSCALE(gtk_hscale_new_with_range(0.0, 1.0, 0.01));
  gtk_scale_set_digits(GTK_SCALE(g->scale1), 2);
  gtk_scale_set_digits(GTK_SCALE(g->scale2), 2);
  gtk_scale_set_digits(GTK_SCALE(g->scale3), 2);
  gtk_scale_set_digits(GTK_SCALE(g->scale4), 2);
  gtk_scale_set_value_pos(GTK_SCALE(g->scale1), GTK_POS_LEFT);
  gtk_scale_set_value_pos(GTK_SCALE(g->scale2), GTK_POS_LEFT);
  gtk_scale_set_value_pos(GTK_SCALE(g->scale3), GTK_POS_LEFT);
  gtk_scale_set_value_pos(GTK_SCALE(g->scale4), GTK_POS_LEFT);
  gtk_range_set_value(GTK_RANGE(g->scale1), p->cx);
  gtk_range_set_value(GTK_RANGE(g->scale2), p->cy);
  gtk_range_set_value(GTK_RANGE(g->scale3), p->cw);
  gtk_range_set_value(GTK_RANGE(g->scale4), p->ch);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale1), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale2), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale3), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale4), TRUE, TRUE, 0);

  g_signal_connect (G_OBJECT (g->scale1), "value-changed",
                    G_CALLBACK (cx_callback), self);
  g_signal_connect (G_OBJECT (g->scale2), "value-changed",
                    G_CALLBACK (cy_callback), self);
  g_signal_connect (G_OBJECT (g->scale3), "value-changed",
                    G_CALLBACK (cw_callback), self);
  g_signal_connect (G_OBJECT (g->scale4), "value-changed",
                    G_CALLBACK (ch_callback), self);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

