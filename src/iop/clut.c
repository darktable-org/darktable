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
#include "common/colorspaces.h"
#include "common/opencl.h"
#include "develop/develop.h"
#include "control/control.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "dtgtk/paint.h"
#include "gui/presets.h"
#include "bauhaus/bauhaus.h"
#include "develop/imageop.h"

#include <stdlib.h>
#include <math.h>
#include <assert.h>
#include <string.h>

DT_MODULE(1)

// TODO: sometimes the slider doesn't update the dimension, esp. with color picking
// TODO: need to be able to zoom into the thing
// TODO: make transition smoother

#define DT_CLUT_INSET 5
#define DT_CLUT_MAX 40.
#define DT_CLUT_MAX_POINTS 288

typedef struct dt_iop_clut_params_t
{
  // Lab coordinates before and after the mapping:
  uint32_t num;
  float x[DT_CLUT_MAX_POINTS][3]; // LCh
  float r[DT_CLUT_MAX_POINTS][3]; // gauss sigmas for selection
  float y[DT_CLUT_MAX_POINTS][3];
}
dt_iop_clut_params_t;

typedef struct dt_iop_clut_gui_data_t
{
  GtkDrawingArea *area;
  GtkWidget *slider, *combo;
  int selected;    // selected point
  int picking;
  int projection;  // projected axis (L,a,b)
  float cursor[3]; // 3d cursor position
  cmsHPROFILE hDisplay;
  cmsHPROFILE hLab;
  cmsHTRANSFORM xform;
}
dt_iop_clut_gui_data_t;

typedef dt_iop_clut_params_t dt_iop_clut_data_t;

typedef struct dt_iop_clut_global_data_t
{
  int kernel_clut;
}
dt_iop_clut_global_data_t;

const char *name()
{
  return _("color lut");
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING;
}

int
groups ()
{
  return IOP_GROUP_COLOR;
}

void init_presets (dt_iop_module_so_t *self)
{
  // TODO: compile some styles and insert here
}

void init_key_accels(dt_iop_module_so_t *self) { }

void connect_key_accels(dt_iop_module_t *self) { }

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *i, void *o, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_clut_data_t *d = (dt_iop_clut_data_t *)piece->data;
#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(shared)
#endif
  for(int k=0; k<roi_out->width*roi_out->height; k++)
  {
    float *in  = (float *)i + 4*k;
    float *out = (float *)o + 4*k;
    // look up displacement field and update color!
    const float w0 = 1e-1f;
    float sumw = w0;
    const float inL = in[0];
    const float inC = sqrtf(in[1]*in[1] + in[2]*in[2]);
    float inh = atan2f(in[2], in[1]);
    if(inh < 0.0f) inh += 2.0f*M_PI;
    float LCh[3] = {w0*inL, w0*inC, w0*inh};
    for(int i=0;i<d->num;i++)
    {
      // compute hue distance modulo 2 pi
      const float disth = inh - d->x[i][2];
      float disthm = disth;
      if(fabsf(disth + 2.0f*M_PI) < fabsf(disthm))
        disthm = disth + 2.0f*M_PI;
      if(fabsf(disth - 2.0f*M_PI) < fabsf(disthm))
        disthm = disth - 2.0f*M_PI;

      const float dist2 =
       ((inL - d->x[i][0])*(inL - d->x[i][0])/(100*100*d->r[i][0]*d->r[i][0]) +
        (inC - d->x[i][1])*(inC - d->x[i][1])/(128*128*d->r[i][1]*d->r[i][1]) +
        (disthm*disthm)/(4.0*M_PI*M_PI*d->r[i][2]*d->r[i][2]));
      const float w = expf(-dist2);//dt_fast_expf(-dist2);
      sumw += w;
      LCh[0] += (inL + d->y[i][0] - d->x[i][0]) * w;
      LCh[1] += (inC + d->y[i][1] - d->x[i][1]) * w;
      float modh = d->y[i][2] + disthm;
      if(modh > 2.0f*M_PI) modh -= 2.0f*M_PI;
      if(modh < 0.0f) modh += 2.0f*M_PI;
      LCh[2] += modh * w;
    }
    // normalize
    for(int j=0;j<3;j++) LCh[j] /= sumw;
    const float outa = cosf(LCh[2])*LCh[1];
    const float outb = sinf(LCh[2])*LCh[1];
    out[0] = LCh[0];
    out[1] = outa;
    out[2] = outb;
  }
}

// TODO: process_cl

void init_global(dt_iop_module_so_t *module)
{
  dt_iop_clut_global_data_t *gd = (dt_iop_clut_global_data_t *)malloc(sizeof(dt_iop_clut_global_data_t));
  module->data = gd;
#if 0
  const int program = 2; // basic.cl from programs.conf
  gd->kernel_clut = dt_opencl_create_kernel(program, "clut");
#endif
}


void cleanup_global(dt_iop_module_so_t *module)
{
  // dt_iop_clut_global_data_t *gd = (dt_iop_clut_global_data_t *)module->data;
  // dt_opencl_free_kernel(gd->kernel_clut);
  free(module->data);
  module->data = NULL;
}


void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_clut_params_t *p = (dt_iop_clut_params_t *)p1;
  dt_iop_clut_data_t *d = (dt_iop_clut_data_t *)piece->data;
  memcpy(d, p, sizeof(dt_iop_clut_params_t));
}

void init_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_clut_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
}

void gui_update(struct dt_iop_module_t *self)
{
  // dt_iop_module_t *module = (dt_iop_module_t *)self;
  dt_iop_clut_gui_data_t *g = (dt_iop_clut_gui_data_t *)self->gui_data;
  // dt_iop_clut_params_t *p = (dt_iop_clut_params_t *)module->params;
  // dt_bauhaus_slider_set(g->slider, p->saturation);
  dt_bauhaus_combobox_set(g->combo, g->projection);
  gtk_widget_queue_draw(self->widget);
}

void init(dt_iop_module_t *module)
{
  module->params = malloc(sizeof(dt_iop_clut_params_t));
  module->default_params = malloc(sizeof(dt_iop_clut_params_t));
  module->default_enabled = 0;
  module->priority = 340; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_clut_params_t);
  module->gui_data = NULL;
  dt_iop_clut_params_t tmp;
  memset(&tmp, 0, sizeof(dt_iop_clut_params_t));
  memcpy(module->params, &tmp, sizeof(dt_iop_clut_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_clut_params_t));
}

void cleanup(dt_iop_module_t *module)
{
  free(module->gui_data);
  module->gui_data = NULL;
  free(module->params);
  module->params = NULL;
}

static void slider_callback (GtkWidget *slider, gpointer user_data);
static void pick_toggled (GtkWidget *w, dt_iop_module_t *self);
static void combo_callback (GtkWidget *w, gpointer user_data);
static gboolean dt_iop_clut_expose(GtkWidget *widget, GdkEventExpose *event, gpointer user_data);
static gboolean dt_iop_clut_motion_notify(GtkWidget *widget, GdkEventMotion *event, gpointer user_data);
static gboolean dt_iop_clut_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data);
static gboolean dt_iop_clut_leave_notify(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data);
static gboolean dt_iop_clut_scrolled(GtkWidget *widget, GdkEventScroll *event, gpointer user_data);

void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_clut_gui_data_t));
  dt_iop_clut_gui_data_t *g = (dt_iop_clut_gui_data_t *)self->gui_data;

  g->selected = -1;
  g->picking = 0;
  g->projection = 0;
  g->cursor[0] = 50.0f;
  g->cursor[1] = 0.0f;
  g->cursor[2] = 0.0f;

  self->widget = gtk_vbox_new(FALSE, DT_BAUHAUS_SPACE);
  g->area = GTK_DRAWING_AREA(gtk_drawing_area_new());
  GtkWidget *asp = gtk_aspect_frame_new(NULL, 0.5, 0.5, 1.0, TRUE);
  gtk_box_pack_start(GTK_BOX(self->widget), asp, TRUE, TRUE, 0);
  gtk_container_add(GTK_CONTAINER(asp), GTK_WIDGET(g->area));
  gtk_widget_set_size_request(GTK_WIDGET(g->area), 258, 258);
  g_object_set (GTK_OBJECT(g->area), "tooltip-text", _("click to add new source/destination control point pair, drag to change mapping, (ctrl-)mouse wheel to change radii of influence, right click to remove a pair."), (char *)NULL);

  gtk_widget_add_events(GTK_WIDGET(g->area), GDK_POINTER_MOTION_MASK | GDK_POINTER_MOTION_HINT_MASK | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_LEAVE_NOTIFY_MASK);
  g_signal_connect (G_OBJECT (g->area), "expose-event",
                    G_CALLBACK (dt_iop_clut_expose), self);
  g_signal_connect (G_OBJECT (g->area), "button-press-event",
                    G_CALLBACK (dt_iop_clut_button_press), self);
  g_signal_connect (G_OBJECT (g->area), "motion-notify-event",
                    G_CALLBACK (dt_iop_clut_motion_notify), self);
  g_signal_connect (G_OBJECT (g->area), "leave-notify-event",
                    G_CALLBACK (dt_iop_clut_leave_notify), self);
  g_signal_connect (G_OBJECT (g->area), "scroll-event",
                    G_CALLBACK (dt_iop_clut_scrolled), self);

  pthread_rwlock_rdlock(&darktable.control->xprofile_lock);
  g->hDisplay = 0;
  g->xform = 0;
  if(darktable.control->xprofile_data)
    g->hDisplay = cmsOpenProfileFromMem(darktable.control->xprofile_data, darktable.control->xprofile_size);
  pthread_rwlock_unlock(&darktable.control->xprofile_lock);
  g->hLab  = dt_colorspaces_create_lab_profile();
  if(g->hDisplay)
    g->xform = cmsCreateTransform(g->hLab, TYPE_Lab_DBL, g->hDisplay, TYPE_RGB_DBL,
                                INTENT_PERCEPTUAL, 0);//cmsFLAGS_NOTPRECALC);
  if(!g->xform)
  { // fall back to srgb
    g->hDisplay = dt_colorspaces_create_srgb_profile();
    g->xform = cmsCreateTransform(g->hLab, TYPE_Lab_DBL, g->hDisplay, TYPE_RGB_DBL,
        INTENT_PERCEPTUAL, 0);//cmsFLAGS_NOTPRECALC);
  }

  // TODO: use xform to get display profile values of those colors:
  g->slider = dt_bauhaus_slider_new_with_range_and_feedback(self, 0.0f, 1.0f, 0.01f, 0.0f, 2, 0);
  dt_bauhaus_widget_set_quad_paint(g->slider, dtgtk_cairo_paint_colorpicker, 0);
  dt_bauhaus_widget_set_label(g->slider, NULL, _("L"));
  dt_bauhaus_slider_set_stop(g->slider, 0., 0.0f, 0.0f, 0.0f);
  dt_bauhaus_slider_set_stop(g->slider, 1., 1.0f, 1.0f, 0.0f);
  g_object_set(G_OBJECT(g->slider), "tooltip-text", _("select 3rd coordinate"), (char *)NULL);
  g_signal_connect (G_OBJECT (g->slider), "value-changed",
                    G_CALLBACK (slider_callback), self);
  g_signal_connect (G_OBJECT (g->slider), "quad-pressed", G_CALLBACK (pick_toggled), self);
  gtk_box_pack_start(GTK_BOX(self->widget), g->slider, TRUE, TRUE, 0);

  g->combo = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->combo, NULL, _("projection"));
  dt_bauhaus_combobox_add(g->combo, _("saturation vs. hue"));
  dt_bauhaus_combobox_add(g->combo, _("L vs. hue"));
  dt_bauhaus_combobox_add(g->combo, _("L vs. saturation"));
  dt_bauhaus_combobox_set(g->combo, 0);
  g_object_set(G_OBJECT(g->combo), "tooltip-text", _("select projection of LCh cube"), (char *)NULL);
  g_signal_connect (G_OBJECT (g->combo), "value-changed",
                    G_CALLBACK (combo_callback), self);
  gtk_box_pack_start(GTK_BOX(self->widget), g->combo, TRUE, TRUE, 0);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  dt_iop_clut_gui_data_t *g = (dt_iop_clut_gui_data_t *)self->gui_data;
  // don't clean up display profile
  // dt_colorspaces_cleanup_profile(g->hDisplay);
  dt_colorspaces_cleanup_profile(g->hLab);
  cmsDeleteTransform(g->xform);
  free(self->gui_data);
  self->gui_data = NULL;
}

static void combo_callback (GtkWidget *w, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_clut_gui_data_t *g = (dt_iop_clut_gui_data_t *)self->gui_data;
  int val = dt_bauhaus_combobox_get(w);
  g->projection = val;

  if(val == 0)
  {
    dt_bauhaus_slider_set_stop(g->slider, 0., 0.0f, 0.0f, 0.0f);
    dt_bauhaus_slider_set_stop(g->slider, 1., 1.0f, 1.0f, 0.0f);
    dt_bauhaus_widget_set_label(g->slider, 0, _("brightness"));
  }
  else if(val == 1)
  {
    // TODO: make these stops look reasonable!
    dt_bauhaus_slider_set_stop(g->slider, 0., 0.0f, 1.0f, 0.0f);
    dt_bauhaus_slider_set_stop(g->slider, 1., 1.0f, 0.0f, 1.0f);
    dt_bauhaus_widget_set_label(g->slider, 0, _("saturation"));
  }
  else
  {
    dt_bauhaus_slider_set_stop(g->slider, 0., 0.0f, 0.0f, 1.0f);
    dt_bauhaus_slider_set_stop(g->slider, 1., 1.0f, 1.0f, 0.0f);
    dt_bauhaus_widget_set_label(g->slider, 0, _("hue"));
  }
  dt_bauhaus_slider_set(g->slider, g->cursor[g->projection]);

  gtk_widget_queue_draw(self->widget);
}

static void pick_toggled (GtkWidget *w, dt_iop_module_t *self)
{
  dt_iop_clut_gui_data_t *g = (dt_iop_clut_gui_data_t *)self->gui_data;

  g->picking = 1-g->picking;

  self->request_color_pick = g->picking;
  if(self->request_color_pick)
  {
    dt_lib_colorpicker_set_point(darktable.lib, 0.5, 0.5);
    dt_dev_reprocess_all(self->dev);
  }
  else
    dt_control_queue_redraw();

  if(self->off) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->off), 1);
  dt_iop_request_focus(self);
}

static void slider_callback (GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_clut_gui_data_t *g = (dt_iop_clut_gui_data_t *)self->gui_data;
  float val = dt_bauhaus_slider_get(slider);
  if(g->projection == 0) val *= 100.0f;
  else if(g->projection == 1) val *= 128.0f;
  else if(g->projection == 2) val *= 2.0f*M_PI;
  g->cursor[g->projection] = val;
  gtk_widget_queue_draw(self->widget);
}

static gboolean
dt_iop_clut_expose(GtkWidget *widget, GdkEventExpose *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_clut_gui_data_t *g = (dt_iop_clut_gui_data_t *)self->gui_data;
  dt_iop_clut_params_t *p  = (dt_iop_clut_params_t *)self->params;

  int ci, cj;
  const float scale[3] = {100.0f, 128.0f, 2.0f*M_PI};
  if(g->projection == 0)
  { // C/h
    ci = 2; cj = 1;
  }
  else if(g->projection == 1)
  { // L/h
    ci = 2; cj = 0;
  }
  else
  { // L/C
    ci = 1; cj = 0;
  }

  int pick = 0;
  if(!(self->picked_color_max[0] < 0.0f || self->picked_color[0] == 0.0f))
  {
    pick = 1;
    const float *c = self->picked_color;
    g->cursor[0] = c[0];
    g->cursor[1] = sqrtf(c[1]*c[1] + c[2]*c[2]);
    g->cursor[2] = atan2f(c[2], c[1]);
    if(g->cursor[2] < 0.0f) g->cursor[2] += 2.0f * M_PI;
    // dt_bauhaus_slider_set(g->slider, (g->cursor[g->projection]+offset[g->projection])/scale[g->projection]);
  }

  const int inset = DT_CLUT_INSET;
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  int width = allocation.width, height = allocation.height;
  cairo_surface_t *cst = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);
  // clear bg
  cairo_set_source_rgb (cr, .2, .2, .2);
  cairo_paint(cr);

  cairo_translate(cr, inset, inset);
  width -= 2*inset;
  height -= 2*inset;
  // flip y:
  cairo_translate(cr, 0, height);
  cairo_scale(cr, 1., -1.);
  const int cells = 8;
  double rgb[3] = {0.5, 0.5, 0.5};

  for(int j=0; j<cells; j++) for(int i=0; i<cells; i++)
    {
      double LCh[3] = {0.0f};
      LCh[g->projection] = g->cursor[g->projection];
      LCh[ci] = (i+.5f)/cells * scale[ci];
      LCh[cj] = (j+.5f)/cells * scale[cj];
      cmsCIELab lab;
      lab.L = LCh[0]; lab.a = LCh[1]*cosf(LCh[2]); lab.b = LCh[1]*sinf(LCh[2]);
      cmsDoTransform(g->xform, &lab, rgb, 1);
      cairo_set_source_rgb (cr, rgb[0], rgb[1], rgb[2]);
      cairo_rectangle(cr, width*i/(float)cells, height*j/(float)cells, width/(float)cells-1, height/(float)cells-1);
      cairo_fill(cr);
    }

  for(int kk=0;kk<=p->num;kk++)
  {
    int k = kk;
    if(g->selected >= 0)
    {
      // selected last:
      if(kk == g->selected/2) continue;
      if(kk == p->num) k = g->selected/2;
    }
    else if(kk == p->num) break;

    float loa, hia, lob, hib;
    loa = width *p->x[k][ci]/scale[ci];
    lob = height*p->x[k][cj]/scale[cj];
    hia = width *p->y[k][ci]/scale[ci];
    hib = height*p->y[k][cj]/scale[cj];

    cairo_set_line_width(cr, 2.);
    cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
    // const float len = sqrtf((hia-loa)*(hia-loa)+(hib-lob)*(hib-lob));
    // if(len > 0.0f)
      // cairo_move_to(cr, loa + radius/len*(hia-loa), lob + radius/len*(hib-lob));
    // else
      cairo_move_to(cr, loa, lob);
    cairo_line_to(cr, hia, hib);
    cairo_stroke(cr);

    cmsCIELab lab;
    lab.L = p->x[k][0];
    lab.a = p->x[k][1] * cosf(p->x[k][2]);
    lab.b = p->x[k][1] * sinf(p->x[k][2]);
    cmsDoTransform(g->xform, &lab, rgb, 1);
    if(g->selected == 2*k)
      cairo_set_source_rgb(cr, rgb[0], rgb[1], rgb[2]);
    else
      cairo_set_source_rgba(cr, rgb[0], rgb[1], rgb[2], fminf(0.7f, 1.0-fabsf(p->x[k][g->projection]-g->cursor[g->projection])/scale[g->projection]));

    cairo_save(cr);
    cairo_translate(cr, loa, lob);
    cairo_arc(cr, 0.0f, 0.0f, 8.0f, 0, 2.*M_PI);
    cairo_fill(cr);
    if(g->selected >= 0 && g->selected/2 == k)
    { // only draw large thing in case we're selected
      cairo_scale(cr, fmaxf(3.0f, width*p->r[k][ci]), fmaxf(3.0f, height*p->r[k][cj]));
      cairo_arc(cr, 0.0f, 0.0f, 1.0f, 0, 2.*M_PI);
    }
    else cairo_arc(cr, 0.0f, 0.0f, 6.0f, 0, 2.*M_PI);
    cairo_restore(cr);
    if(g->selected >= 0 && g->selected == 2*k)
      cairo_set_source_rgb(cr, 0.3, 0.3, 0.3);
    else
      cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
    cairo_stroke(cr);

    if(g->selected == 2*k+1)
      cairo_set_source_rgb(cr, 0.3, 0.3, 0.3);
    else
      cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
    cairo_arc(cr, hia, hib, 3, 0, 2.*M_PI);
    cairo_stroke(cr);
  }

  // draw color picker spot
  if(pick)
  {
    const float radius = 5;
    const float a = width *g->cursor[ci]/scale[ci];
    const float b = height*g->cursor[cj]/scale[cj];
    cairo_arc(cr, a, b, radius, 0, 2.*M_PI);
    cairo_set_source_rgb(cr, 0.1, 0.1, 0.1);
    cairo_stroke(cr);
  }

  cairo_destroy(cr);
  cairo_t *cr_pixmap = gdk_cairo_create(gtk_widget_get_window(widget));
  cairo_set_source_surface (cr_pixmap, cst, 0, 0);
  cairo_paint(cr_pixmap);
  cairo_destroy(cr_pixmap);
  cairo_surface_destroy(cst);
  return TRUE;
}

static gboolean
dt_iop_clut_motion_notify(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_clut_gui_data_t *g = (dt_iop_clut_gui_data_t *)self->gui_data;
  dt_iop_clut_params_t *p = (dt_iop_clut_params_t *)self->params;
  const int inset = DT_CLUT_INSET;
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  int width = allocation.width - 2*inset, height = allocation.height - 2*inset;
  const float mouse_x = CLAMP(event->x - inset, 0, width);
  const float mouse_y = CLAMP(height - 1 - event->y + inset, 0, height);
  int ci, cj;
  const float scale[3] = {100.0f, 128.0f, 2.0f*M_PI};
  float mi, mj;
  if(g->projection == 0)
  { // C/h
    ci = 2; cj = 1;
    mi = scale[2] * mouse_x/(float)width;
    mj = scale[1] * mouse_y/(float)height;
  }
  else if(g->projection == 2)
  { // C/L
    ci = 1; cj = 0;
    mi = scale[1] * mouse_x/(float)width;
    mj = scale[0] * mouse_y/(float)height;
  }
  else
  { // h/L
    ci = 2; cj = 0;
    mi = scale[2] * mouse_x/(float)width;
    mj = scale[0] * mouse_y/(float)height;
  }
  if(event->state & GDK_BUTTON1_MASK)
  {
    if(g->selected >= 0 && !(g->selected & 1))
    {
      p->x[g->selected/2][ci] = mi;
      p->x[g->selected/2][cj] = mj;
      memcpy(g->cursor, p->x[g->selected/2], sizeof(float)*3);
      dt_bauhaus_slider_set(g->slider, g->cursor[g->projection]/scale[g->projection]);
      dt_dev_add_history_item(darktable.develop, self, TRUE);
    }
    else if(g->selected >= 0)
    {
      p->y[g->selected/2][ci] = mi;
      p->y[g->selected/2][cj] = mj;
      memcpy(g->cursor, p->y[g->selected/2], sizeof(float)*3);
      dt_bauhaus_slider_set(g->slider, g->cursor[g->projection]/scale[g->projection]);
      dt_dev_add_history_item(darktable.develop, self, TRUE);
    }
  }
  else
  {
    g->selected = -1;
    float dist = FLT_MAX;
    for(int k=0;k<p->num;k++)
    {
      const float _thrs = 35.0/width;
      const float thrs = _thrs*_thrs;
      float LCh[3];
      LCh[g->projection] = g->cursor[g->projection];
      LCh[ci] = mi; LCh[cj] = mj;
      const float ri = fmaxf(1.f, p->r[k][ci]);
      const float rj = fmaxf(1.f, p->r[k][cj]);
      const float distx =
        (LCh[ci] - p->x[k][ci])*(LCh[ci] - p->x[k][ci])/(ri*ri*scale[ci]*scale[ci])+
        (LCh[cj] - p->x[k][cj])*(LCh[cj] - p->x[k][cj])/(rj*rj*scale[cj]*scale[cj]);
      const float disty =
        (LCh[ci] - p->y[k][ci])*(LCh[ci] - p->y[k][ci])/(ri*ri*scale[ci]*scale[ci])+
        (LCh[cj] - p->y[k][cj])*(LCh[cj] - p->y[k][cj])/(rj*rj*scale[cj]*scale[cj]);
      if(disty < thrs && disty < dist) { g->selected = 2*k+1; dist = disty;}
      if(distx < thrs && distx < dist) { g->selected = 2*k; dist = distx;}
    }
  }
  gtk_widget_queue_draw(self->widget);
  return TRUE;
}

static gboolean
dt_iop_clut_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_clut_gui_data_t *g = (dt_iop_clut_gui_data_t *)self->gui_data;
  dt_iop_clut_params_t *p = (dt_iop_clut_params_t *)self->params;
  const int inset = DT_CLUT_INSET;
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  int width = allocation.width - 2*inset, height = allocation.height - 2*inset;
  const float mouse_x = CLAMP(event->x - inset, 0, width);
  const float mouse_y = CLAMP(height - 1 - event->y + inset, 0, height);
  int ci, cj;
  float mi, mj;
  const float scale[3] = {100.0f, 128.0f, 2.0f*M_PI};
  if(g->projection == 0)
  { // C/h
    ci = 2; cj = 1;
    mi = scale[ci] * mouse_x/(float)width;
    mj = scale[cj] * mouse_y/(float)height;
  }
  else if(g->projection == 2)
  { // C/L
    ci = 1; cj = 0;
    mi = scale[ci] * mouse_x/(float)width;
    mj = scale[cj] * mouse_y/(float)height;
  }
  else
  { // h/L
    ci = 2; cj = 0;
    mi = scale[ci] * mouse_x/(float)width;
    mj = scale[cj] * mouse_y/(float)height;
  }
  if(event->button == 1 && event->type == GDK_2BUTTON_PRESS)
  {
    // double click resets:
    if(g->selected >= 0)
    {
      // only reset current projection:
      p->y[g->selected/2][ci] = p->x[g->selected/2][ci];
      p->y[g->selected/2][cj] = p->x[g->selected/2][cj];
      p->r[g->selected/2][ci] = .10f;
      p->r[g->selected/2][cj] = .10f;
    }
    else
    { // reset everything
      dt_iop_clut_params_t *d = (dt_iop_clut_params_t *)self->default_params;
      memcpy(p, d, sizeof(*p));
    }
    dt_dev_add_history_item(darktable.develop, self, TRUE);
    g->selected = -1;
    return TRUE;
  }
  else if((g->selected < 0) && (event->button == 1) && (p->num < DT_CLUT_MAX_POINTS))
  { // add new point
    p->x[p->num][g->projection] = g->cursor[g->projection];
    p->x[p->num][ci] = mi;
    p->x[p->num][cj] = mj;
    p->r[p->num][0] = .10f; // sigma Lab
    p->r[p->num][1] = .10f;
    p->r[p->num][2] = .10f;
    p->y[p->num][g->projection] = g->cursor[g->projection];
    p->y[p->num][ci] = mi;
    p->y[p->num][cj] = mj;
    p->num++;
    g->selected = 2*p->num;
    dt_dev_add_history_item(darktable.develop, self, TRUE);
    return TRUE;
  }
  else if(event->button != 1)
  { // delete selected point
    if(p->num > 0 && g->selected >= 0)
    {
      memmove(p->x[g->selected/2], p->x[p->num-1], sizeof(float)*3);
      memmove(p->r[g->selected/2], p->r[p->num-1], sizeof(float)*3);
      memmove(p->y[g->selected/2], p->y[p->num-1], sizeof(float)*3);
      p->num--;
      g->selected = -1;
      dt_dev_add_history_item(darktable.develop, self, TRUE);
      return TRUE;
    }
  }
  return FALSE;
}

static gboolean
dt_iop_clut_leave_notify(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  gtk_widget_queue_draw(self->widget);
  return TRUE;
}

static gboolean
dt_iop_clut_scrolled(GtkWidget *widget, GdkEventScroll *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_clut_gui_data_t *g = (dt_iop_clut_gui_data_t *)self->gui_data;
  dt_iop_clut_params_t *p = (dt_iop_clut_params_t *)self->params;
  guint modifiers = gtk_accelerator_get_default_mod_mask ();

  if(g->selected < 0) return FALSE;
  const float step = 4.0f/5.0f;
  const float scale[3] = {100.0f, 128.0f, 2.0f*M_PI};
  int ci, cj;
  if(g->projection == 0)
  { // C/h
    ci = 2; cj = 1;
  }
  else if(g->projection == 2)
  { // C/L
    ci = 1; cj = 0;
  }
  else
  { // h/L
    ci = 2; cj = 0;
  }
  if((event->state & modifiers) & GDK_SHIFT_MASK)
  {
    int c = ci;
    if((event->state & modifiers) & GDK_CONTROL_MASK) c = cj;
    if(c == 2)
    {
      if(event->direction == GDK_SCROLL_UP  ) p->y[g->selected/2][c] -= scale[c]*0.0005f;
      if(event->direction == GDK_SCROLL_DOWN) p->y[g->selected/2][c] += scale[c]*0.0005f;
      if(p->y[g->selected/2][c] < 0.0f)     p->y[g->selected/2][c] += scale[c];
      if(p->y[g->selected/2][c] > scale[c]) p->y[g->selected/2][c] -= scale[c];
    }
    else
    {
      if(event->direction == GDK_SCROLL_UP   && p->y[g->selected/2][c] > 0.0)      p->y[g->selected/2][c] -= scale[c]*0.0005f;
      if(event->direction == GDK_SCROLL_DOWN && p->y[g->selected/2][c] < scale[c]) p->y[g->selected/2][c] += scale[c]*0.0005f;
    }
  }
  else
  {
    float *r = &p->r[g->selected/2][ci];
    if((event->state & modifiers) & GDK_CONTROL_MASK)
      r = &p->r[g->selected/2][cj];
    if(event->direction == GDK_SCROLL_UP   && *r > 0.001) *r *= step;
    if(event->direction == GDK_SCROLL_DOWN && *r < 10.00) *r /= step;
  }
  dt_dev_add_history_item(darktable.develop, self, TRUE);
  gtk_widget_queue_draw(widget);
  return TRUE;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
