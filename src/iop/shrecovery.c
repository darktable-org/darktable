/*
  This file is part of darktable,
  copyright (c) 2011 Denis Cheremisov.

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
#ifdef HAVE_GEGL
#include <gegl.h>
#endif
#include "common/colorspaces.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "control/control.h"
#include "dtgtk/slider.h"
#include "dtgtk/resetlabel.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include <gtk/gtk.h>
#include <inttypes.h>

#define MAX_RADIUS  32

#define CLIP(x) ((x<0)?0.0:(x>1.0)?1.0:x)
#define LCLIP(x) ((x<0)?0.0:(x>100.0)?100.0:x)
#define min(x,y) ((x<y)?x:y)
#define max(x,y) ((x<y)?y:x)
#define exposure2white(x)       exp2f(-(x))

DT_MODULE(3)


inline float sqr(float x) { return x*x; }


typedef struct dt_iop_enfuse_params_t
{
    // Exposure scale compared to currently (taken with black == 0 in mind)
    float strength;

    // Weights
    float contrast;
    float saturation;
    float exposedness;

    // Gauss and Laplace pyramide depth
} dt_iop_enfuse_params_t;

typedef struct dt_iop_enfuse_data_t
{
    float strength, contrast, saturation, exposedness;

} dt_iop_enfuse_data_t;


typedef struct dt_iop_enfuse_gui_data_t
{
    GtkVBox   *vbox1,  *vbox2;
    GtkDarktableSlider *scale1,*scale2,*scale3,*scale4;       // strength, contrast, saturation, exposedness
}
    dt_iop_enfuse_gui_data_t;

const char *name()
{
    return _("shadow recovery");
}

int flags()
{
    return IOP_FLAGS_INCLUDE_IN_STYLES;
}

int
groups ()
{
    return IOP_GROUP_EFFECT;
}

void init_key_accels(dt_iop_module_so_t *self)
{
    dt_accel_register_slider_iop(self, FALSE, NC_("accel", "strength"));
    dt_accel_register_slider_iop(self, FALSE, NC_("accel", "contrast"));
    dt_accel_register_slider_iop(self, FALSE, NC_("accel", "saturation"));
    dt_accel_register_slider_iop(self, FALSE, NC_("accel", "exposedness"));
}

void connect_key_accels(dt_iop_module_t *self)
{
    dt_iop_enfuse_gui_data_t *g = (dt_iop_enfuse_gui_data_t*)self->gui_data;

    dt_accel_connect_slider_iop(self, "strength", GTK_WIDGET(g->scale1));
    dt_accel_connect_slider_iop(self, "contrast", GTK_WIDGET(g->scale2));
    dt_accel_connect_slider_iop(self, "saturation", GTK_WIDGET(g->scale3));
    dt_accel_connect_slider_iop(self, "exposedness", GTK_WIDGET(g->scale4));
}

const float _w_[5] = {0.05, 0.25, 0.4, 0.25, 0.05};
const int size_limit = 2;

void create_image_weight(int height, int width, int ch, float *in, float *W, float scale, float wc, float ws, float we) {
    float c1, s1, e1, c2, s2, e2, r, g, b, m, w1, w2;
    float *l[3];
    int s[3];
    for(int i = 0; i < height; i++) {
        l[0] = in + max(i-1,0)*width*ch;
        l[1] = in + i*width*ch;
        l[2] = in + min(i,height-1)*width*ch;
        for(int j = 0; j < width; j++) {
            s[0] = max(j-1,0)*ch;
            s[1] = j*ch;
            s[2] = min(j,width-1)*ch;
#define A(i,j,S) l[i][s[j]+S]
            c1 = 0.3*(A(0,0,0) + A(0,1,0) + A(0,2,0) + A(1,0,0) + A(1,2,0) + A(2,0,0) + A(2,1,0) + A(2,2,0) - 8*A(1,1,0));
            c1 += 0.59*(A(0,0,1) + A(0,1,1) + A(0,2,1) + A(1,0,1) + A(1,2,1) + A(2,0,1) + A(2,1,1) + A(2,2,1) - 8*A(1,1,1));
            c1 += 0.11*(A(0,0,2) + A(0,1,2) + A(0,2,2) + A(1,0,2) + A(1,2,2) + A(2,0,2) + A(2,1,2) + A(2,2,2) - 8*A(1,1,2));
            c1 = powf(fabs(c1),wc);
#undef A
#define B(i,j,S) min(l[i][s[j]+S]*scale,1.0)
            c2 = 0.3*(B(0,0,0) + B(0,1,0) + B(0,2,0) + B(1,0,0) + B(1,2,0) + B(2,0,0) + B(2,1,0) + B(2,2,0) - 8*B(1,1,0));
            c2 += 0.59*(B(0,0,1) + B(0,1,1) + B(0,2,1) + B(1,0,1) + B(1,2,1) + B(2,0,1) + B(2,1,1) + B(2,2,1) - 8*B(1,1,1));
            c2 += 0.11*(B(0,0,2) + B(0,1,2) + B(0,2,2) + B(1,0,2) + B(1,2,2) + B(2,0,2) + B(2,1,2) + B(2,2,2) - 8*B(1,1,2));
            c2 = powf(fabs(c2),wc);
#undef B
            r = l[1][s[1]];
            g = l[1][s[1]+1];
            b = l[1][s[1]+2];
            m = (r + g + b)/3.0;
            s1 = powf(sqrt((sqr(r-m) + sqr(g-m) + sqr(b-m))/3.0),ws);
            e1 = exp(-(sqr(r-0.5) + sqr(g-0.5) + sqr(b-0.5))/0.08*we);
            r = min(l[1][s[1]]*scale,1.0);
            g = min(l[1][s[1]+1]*scale,1.0);
            b = min(l[1][s[1]+2]*scale,1.0);
            m = (r + g + b)/3.0;
            s2 = powf(sqrt((sqr(r-m) + sqr(g-m) + sqr(b-m))/3.0),ws);
            e2 = exp(-(sqr(r-0.5) + sqr(g-0.5) + sqr(b-0.5))/0.08*we);
            w1 = c1*s1*e1;
            w2 = c2*s2*e2;
            if(fabs(w1 + w2) < 1e-6) {
                w1 = 1.;
                w2 = 0.;
            }
            W[i*width+j] = w1/(w1+w2);
        }
    }
}

void gauss_image_weight(int width, int height, float *W) {
    float *wn = W + width*height;
    int w = width/2, h = height/2;
    float *l1, *l2, *l3, *l4, *l5;
    int st1, st2, st3, st4, st5;
    for(int i = 0; i < h; i++) {
        l1 = W + max(2*i-2,0)*width;
        l2 = W + max(2*i-1,0)*width;
        l3 = W + 2*i*width;
        l4 = W + min(2*i+1,height-1)*width;
        l5 = W + min(2*i+2,height-1)*width;
        for(int j = 0; j < w; j++) {
            st1 = max(2*j-2,0);
            st2 = max(2*j-1,0);
            st3 = 2*j;
            st4 = min(2*j+1,width-1);
            st5 = min(2*j+2,width-1);
            wn[i*w+j] = 0.0625*(l2[st2]+l2[st4]+l4[st2]+l4[st4])+
                0.0125*(l1[st2]+l1[st4]+l2[st1]+l2[st5]+l4[st1]+l4[st5]+l5[st2]+l5[st4])+
                0.1*(l2[st3]+l3[st2]+l3[st4]+l4[st3])+
                0.16*(l3[st3])+
                0.0025*(l1[st1]+l1[st5]+l5[st1]+l5[st5])+
                0.02*(l1[st3]+l3[st1]+l3[st5]+l5[st3]);
        }
    }
    if((w/2>0) && (h/2>0))
        gauss_image_weight(w,h,wn);
}

void laplace_image(int width, int height, float *im) {
    float *imn = im + 3*width*height;
    int w = width/2, h = height/2, i1, j1;
    float *l1, *l2, *l3, *l4, *l5;
    int st1, st2, st3, st4, st5;
    for(int i = 0; i < h; i++) {
        l1 = im + max(2*i-2,0)*width*3;
        l2 = im + max(2*i-1,0)*width*3;
        l3 = im + 2*i*width*3;
        l4 = im + min(2*i+1,height-1)*width*3;
        l5 = im + min(2*i+2,height-1)*width*3;
        for(int j = 0; j < w; j++) {
            st1 = max(2*j-2,0)*3;
            st2 = max(2*j-1,0)*3;
            st3 = 6*j;
            st4 = min(2*j+1,width-1)*3;
            st5 = min(2*j+2,width-1)*3;
            imn[3*(i*w+j)] = 0.0625*(l2[st2]+l2[st4]+l4[st2]+l4[st4])+
                0.0125*(l1[st2]+l1[st4]+l2[st1]+l2[st5]+l4[st1]+l4[st5]+l5[st2]+l5[st4])+
                0.1*(l2[st3]+l3[st2]+l3[st4]+l4[st3])+
                0.16*(l3[st3])+
                0.0025*(l1[st1]+l1[st5]+l5[st1]+l5[st5])+
                0.02*(l1[st3]+l3[st1]+l3[st5]+l5[st3]);
            imn[3*(i*w+j)+1] = 0.0625*(l2[st2+1]+l2[st4+1]+l4[st2+1]+l4[st4+1])+
                0.0125*(l1[st2+1]+l1[st4+1]+l2[st1+1]+l2[st5+1]+l4[st1+1]+l4[st5+1]+l5[st2+1]+l5[st4+1])+
                0.1*(l2[st3+1]+l3[st2+1]+l3[st4+1]+l4[st3+1])+
                0.16*(l3[st3+1])+
                0.0025*(l1[st1+1]+l1[st5+1]+l5[st1+1]+l5[st5+1])+
                0.02*(l1[st3+1]+l3[st1+1]+l3[st5+1]+l5[st3+1]);
            imn[3*(i*w+j)+2] = 0.0625*(l2[st2+2]+l2[st4+2]+l4[st2+2]+l4[st4+2])+
                0.0125*(l1[st2+2]+l1[st4+2]+l2[st1+2]+l2[st5+2]+l4[st1+2]+l4[st5+2]+l5[st2+2]+l5[st4+2])+
                0.1*(l2[st3+2]+l3[st2+2]+l3[st4+2]+l4[st3+2])+
                0.16*(l3[st3+2])+
                0.0025*(l1[st1+2]+l1[st5+2]+l5[st1+2]+l5[st5+2])+
                0.02*(l1[st3+2]+l3[st1+2]+l3[st5+2]+l5[st3+2]);
        }
    }
    for(int i = 0; i < height; i++) {
        for(int j = 0; j < width; j++) {
            for(int m = -2; m < 3; m++) {
                for(int n = -2; n < 3; n++) {
                    if((i-m)%2)
                        continue;
                    if((j-n)%2)
                        continue;
                    i1 = min(max((i-m)/2,0),h-1);
                    j1 = min(max((j-n)/2,0),w-1);
                    im[3*(i*width+j)]   -= 4*_w_[n+2]*_w_[m+2]*imn[3*(i1*w+j1)];
                    im[3*(i*width+j)+1] -= 4*_w_[n+2]*_w_[m+2]*imn[3*(i1*w+j1)+1];
                    im[3*(i*width+j)+2] -= 4*_w_[n+2]*_w_[m+2]*imn[3*(i1*w+j1)+2];
                }
            }
        }
    }
    if((w/2>size_limit)&&(h/2>size_limit)) {
        laplace_image(w,h,imn);
    }
}

void weighted_image(int width, int height, float *im1, float *im2, float *W) {
    int i1, j1, w = width/2, h = height/2;
    float *imn1 = im1 + width*height*3, *imn2 = im2 + width*height*3, *wn = W + width*height;

    for(int i = width*height; i--; ) {
        im1[3*i]   = im1[3*i]*W[i] + im2[3*i]*(1.0-W[i]);
        im1[3*i+1] = im1[3*i+1]*W[i] + im2[3*i+1]*(1.0-W[i]);
        im1[3*i+2] = im1[3*i+2]*W[i] + im2[3*i+2]*(1.0-W[i]);
    }
    if((w <= size_limit) || (h <= size_limit))
        return;
    weighted_image(w, h, imn1, imn2, wn);
    for(int i = 0; i < height; i++) {
        for(int j = 0; j < width; j++) {
            for(int m = -2; m < 3; m++) {
                for(int n = -2; n < 3; n++) {
                    if((i-m)%2)
                        continue;
                    if((j-n)%2)
                        continue;
                    i1 = min(max((i-m)/2,0),h-1);
                    j1 = min(max((j-n)/2,0),w-1);
                    im1[3*(i*width+j)]   += 4*_w_[n+2]*_w_[m+2]*imn1[3*(i1*w+j1)];
                    im1[3*(i*width+j)+1] += 4*_w_[n+2]*_w_[m+2]*imn1[3*(i1*w+j1)+1];
                    im1[3*(i*width+j)+2] += 4*_w_[n+2]*_w_[m+2]*imn1[3*(i1*w+j1)+2];

                }
            }
        }
    }
}


void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *i, void *o, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
    dt_iop_enfuse_data_t *d = (dt_iop_enfuse_data_t *)piece->data;
    const float scale = 1.0 + d->strength;
    const int ch = piece->colors;
    /*
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(roi_out,i,o) schedule(static)
#endif
    */
    float *in = (float*)i, *out = (float*)o;
    float *W = (float*)malloc(2*sizeof(float)*roi_out->height*roi_out->width);
    create_image_weight(roi_out->width, roi_out->height, ch, in, W, scale, d->contrast, d->saturation, d->exposedness);
    gauss_image_weight(roi_out->width, roi_out->height, W);

    float *im1 = (float*)malloc(6*sizeof(float)*roi_out->height*roi_out->width);
    float *im2 = (float*)malloc(6*sizeof(float)*roi_out->height*roi_out->width);
    for(int i = roi_out->height*roi_out->width; i--;) {
        im1[i*3] = in[i*ch];
        im1[i*3+1] = in[i*ch+1];
        im1[i*3+2] = in[i*ch+2];
        im2[i*3] = min(in[i*ch]*scale,1.0);
        im2[i*3+1] = min(in[i*ch+1]*scale,1.0);
        im2[i*3+2] = min(in[i*ch+2]*scale,1.0);
    }
    laplace_image(roi_out->width, roi_out->height, im1);
    laplace_image(roi_out->width, roi_out->height, im2);
    weighted_image(roi_out->width, roi_out->height, im1, im2, W);

    for(int i = roi_out->width*roi_out->height; i--;) {
        out[i*ch] = im1[3*i];
        out[i*ch + 1] = im1[3*i+1];
        out[i*ch + 2] = im1[3*i+2];
    }

    free(W);
    free(im1);
    free(im2);
}

static void
strength_callback (GtkDarktableSlider *slider, gpointer user_data)
{
    dt_iop_module_t *self = (dt_iop_module_t *)user_data;
    if(self->dt->gui->reset) return;
    dt_iop_enfuse_params_t *p = (dt_iop_enfuse_params_t *)self->params;
    p->strength= dtgtk_slider_get_value(slider);
    dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void
contrast_callback (GtkDarktableSlider *slider, gpointer user_data)
{
    dt_iop_module_t *self = (dt_iop_module_t *)user_data;
    if(self->dt->gui->reset) return;
    dt_iop_enfuse_params_t *p = (dt_iop_enfuse_params_t *)self->params;
    p->contrast = dtgtk_slider_get_value(slider);
    dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void
saturation_callback (GtkDarktableSlider *slider, gpointer user_data)
{
    dt_iop_module_t *self = (dt_iop_module_t *)user_data;
    if(self->dt->gui->reset) return;
    dt_iop_enfuse_params_t *p = (dt_iop_enfuse_params_t *)self->params;
    p->saturation = dtgtk_slider_get_value(slider);
    dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void
exposedness_callback (GtkDarktableSlider *slider, gpointer user_data)
{
    dt_iop_module_t *self = (dt_iop_module_t *)user_data;
    if(self->dt->gui->reset) return;
    dt_iop_enfuse_params_t *p = (dt_iop_enfuse_params_t *)self->params;
    p->exposedness = dtgtk_slider_get_value(slider);
    dt_dev_add_history_item(darktable.develop, self, TRUE);
}


void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
    dt_iop_enfuse_params_t *p = (dt_iop_enfuse_params_t *)p1;
#ifdef HAVE_GEGL
    fprintf(stderr, "[bloom] TODO: implement gegl version!\n");
    // pull in new params to gegl
#else
    dt_iop_enfuse_data_t *d = (dt_iop_enfuse_data_t *)piece->data;
    d->strength= p->strength;
    d->contrast = p->contrast;
    d->saturation = p->saturation;
    d->exposedness = p->exposedness;
#endif
}

void init_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
    piece->data = malloc(sizeof(dt_iop_enfuse_data_t));
    memset(piece->data,0,sizeof(dt_iop_enfuse_data_t));
    self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
    free(piece->data);
}

void gui_update(struct dt_iop_module_t *self)
{
    dt_iop_module_t *module = (dt_iop_module_t *)self;
    dt_iop_enfuse_gui_data_t *g = (dt_iop_enfuse_gui_data_t *)self->gui_data;
    dt_iop_enfuse_params_t *p = (dt_iop_enfuse_params_t *)module->params;
    dtgtk_slider_set_value(g->scale1, p->strength);
    dtgtk_slider_set_value(g->scale2, p->contrast);
    dtgtk_slider_set_value(g->scale3, p->saturation);
    dtgtk_slider_set_value(g->scale4, p->exposedness);
}

void init(dt_iop_module_t *module)
{
    module->params = malloc(sizeof(dt_iop_enfuse_params_t));
    module->default_params = malloc(sizeof(dt_iop_enfuse_params_t));
    module->default_enabled = 0;
    module->priority = 833; // module order created by iop_dependencies.py, do not edit!
    module->params_size = sizeof(dt_iop_enfuse_params_t);
    module->gui_data = NULL;
    dt_iop_enfuse_params_t tmp = (dt_iop_enfuse_params_t)
        {
            2.0, 1.0, 1.0, 1.0
        };
    memcpy(module->params, &tmp, sizeof(dt_iop_enfuse_params_t));
    memcpy(module->default_params, &tmp, sizeof(dt_iop_enfuse_params_t));
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
    self->gui_data = malloc(sizeof(dt_iop_enfuse_gui_data_t));
    dt_iop_enfuse_gui_data_t *g = (dt_iop_enfuse_gui_data_t *)self->gui_data;
    dt_iop_enfuse_params_t *p = (dt_iop_enfuse_params_t *)self->params;

    self->widget = GTK_WIDGET(gtk_hbox_new(FALSE, 0));
    g->vbox1 = GTK_VBOX(gtk_vbox_new(FALSE, DT_GUI_IOP_MODULE_CONTROL_SPACING));
    g->vbox2 = GTK_VBOX(gtk_vbox_new(FALSE, DT_GUI_IOP_MODULE_CONTROL_SPACING));
    gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->vbox2), TRUE, TRUE, 5);

    g->scale1 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 10.0, 0.01, p->strength, 2));
    g->scale2 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 1.0, 0.01, p->contrast, 2));
    g->scale3 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 1.0, 0.01, p->saturation, 2));
    g->scale4 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 1.0, 0.01, p->exposedness, 2));
    dtgtk_slider_set_label(g->scale1,_("strength"));
    dtgtk_slider_set_label(g->scale2,_("contrast"));
    dtgtk_slider_set_label(g->scale3,_("saturation"));
    dtgtk_slider_set_label(g->scale4,_("well exposedness"));
    dtgtk_slider_set_unit(g->scale1,"EV");
    /* dtgtk_slider_set_format_type(g->scale1,DARKTABLE_SLIDER_FORMAT_PERCENT); */
    /* dtgtk_slider_set_format_type(g->scale2,DARKTABLE_SLIDER_FORMAT_PERCENT); */
    /* dtgtk_slider_set_format_type(g->scale4,DARKTABLE_SLIDER_FORMAT_PERCENT); */

    gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale1), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale2), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale3), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale4), TRUE, TRUE, 0);
    g_object_set(G_OBJECT(g->scale1), "tooltip-text", _("the size of blur"), (char *)NULL);
    g_object_set(G_OBJECT(g->scale2), "tooltip-text", _("the saturation of blur"), (char *)NULL);
    g_object_set(G_OBJECT(g->scale3), "tooltip-text", _("the brightness of blur"), (char *)NULL);
    g_object_set(G_OBJECT(g->scale4), "tooltip-text", _("the mix of effect"), (char *)NULL);

    g_signal_connect (G_OBJECT (g->scale1), "value-changed",
                      G_CALLBACK (strength_callback), self);
    g_signal_connect (G_OBJECT (g->scale2), "value-changed",
                      G_CALLBACK (contrast_callback), self);
    g_signal_connect (G_OBJECT (g->scale3), "value-changed",
                      G_CALLBACK (saturation_callback), self);
    g_signal_connect (G_OBJECT (g->scale4), "value-changed",
                      G_CALLBACK (exposedness_callback), self);

}

void gui_cleanup(struct dt_iop_module_t *self)
{
    free(self->gui_data);
    self->gui_data = NULL;
}

// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
