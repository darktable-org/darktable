/*
    This file is part of darktable,
    copyright (c) 2009--2011 johannes hanika.

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
#include "iop/densitycurve.h"
#include "gui/histogram.h"
#include "gui/presets.h"
#include "develop/develop.h"
#include "control/control.h"
#include "gui/gtk.h"
#include "common/opencl.h"
#include <gdk/gdkkeysyms.h>

#define DT_GUI_CURVE_EDITOR_INSET 5
#define DT_GUI_CURVE_INFL .3f

DT_MODULE(1)

const char *name()
{
  return _("density curve");
}


int
groups ()
{
  return IOP_GROUP_CORRECT;
}

int
flags ()
{
  return IOP_FLAGS_SUPPORTS_BLENDING;
}

// /********************************************************
// curveeditor_point_exists:
// *********************************************************/
static gboolean point_exists (Gcurve *curve, int selectedPoint, float x)
{
  int i;
  for(i = 0; i < *curve->n_points; i++)
  {
    if ( i!=selectedPoint &&
         fabs(x-curve->points[i].x)< 1.0/256.0)
    {
      return TRUE;
    }
  }
  return FALSE;
}

/********************************************************
    comparator helper functio for qsort function call.
*********************************************************/
static int points_qsort_compare( const void *point1, const void *point2 )
{
  /* Compare points. Must define all three return values! */
  if (((Point*)point1)->x > ((Point*)point2)->x)
  {
    return 1;
  }
  else if (((Point*)point1)->x < ((Point*)point2)->x)
  {
    return -1;
  }
  //this should not happen. x-coords must be different for spline to work.
  return 0;
}

// ev_from gray =LOG(M45/GRAY18)/LOG(2)
// gray_from_lab_l =IF( ((D9+16)/116) > (6/29), ((D9+16)/116)^3, (3*(6/29)^2)* (((D9+16)/116) - 4/29) )
float ev_from_lab_l(float L)
{
  float Ev=0;
  float tmp;

  tmp=(L+16.0)/116.0;
  if(tmp > (6.0/29.0) )
  {
    tmp = tmp * tmp * tmp;
  }
  else
  {
    tmp = 3.0*powf(6.0/29.0,2) * ( tmp - 4.0/29.0);
  }

  Ev = log(tmp/GRAY18)/log(2);

  return Ev;
}

// =116*IF(C45>((6/29)^3),C45^(1/3), (1/3)* (29/6)^2 * C45 + (4/29))-16
// c45 - gray%
float lab_l_from_ev(float Ev)
{
  float L=0.0;
  float gray;

  gray = GRAY18 * powf(2, Ev);
  if(gray > powf((6.0/29.0), 3) )
  {
    //L = C45^(1/3);
    L = cbrtf(gray);
  }
  else
  {
    L = (1.0/3.0)* powf((29.0/6.0), 2) * gray + (4.0/29.0);
  }

  return 116.0 * L -16.0;
}

void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *i, void *o, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  const int ch = piece->colors;
  dt_iop_densitycurve_data_t *d = (dt_iop_densitycurve_data_t *)(piece->data);
#if 0
#ifdef _OPENMP
  // TODO uncomment
  #pragma omp parallel for default(none) shared(roi_out, i, o, d) schedule(static)
  //#pragma omp parallel for default(none) shared(roi_out, i, o) schedule(static)
#endif
#endif
#if 1
  // UPLAB to presserve hue with saturation changes
  // see http://www.brucelindbloom.com/UPLab.html
  // TODO or better CIECAM02 http://www.mail-archive.com/lcms-user@lists.sourceforge.net/msg01263.html
  int rowsize=roi_out->width*3;
  float rgb[rowsize];
  float Lab[rowsize];
  float *in = ((float *)i);
  float *out = ((float *)o);
  const float Evs = lab_l_from_ev( ev_from_lab_l(LabMIN) );

  for(int k=0; k<roi_out->height; k++)
  {
    const int m=(k*(roi_out->width*ch));
    for (int l=0; l<roi_out->width; l++)
    {
      int li=3*l,ii=ch*l;
      Lab[li+0] = in[m+ii+0];
      Lab[li+1] = in[m+ii+1];
      Lab[li+2] = in[m+ii+2];
    }
    // lcms is not thread safe, so use local copy
    // UPLab and curve
    cmsDoTransform (d->xformi[dt_get_thread_num()], Lab, rgb, roi_out->width);

    if(d->lut_type == LUT_COEFFS)
    {
      for (int l=0; l<roi_out->width; l++)
      {
        int li=3*l;
        const int t = CLAMP((int)(rgb[li+0]/100.0*0xfffful), 0, 0xffff);
        float Labl = rgb[li+0];
        rgb[li+0] = Evs + d->table[t]*rgb[li+0];
        // in Lab: correct compressed Luminance for saturation:
        //if(d->scale_saturation!=0 && Labl > 0.01f)
        //if(d->scale_saturation!=0) {
        if(d->scale_saturation!=0)
        {
          //if(Labl > 1.0f && rgb[li+0] > 1.0f ) {
          if(Labl > 0.01f && rgb[li+0] > 0.01f )
          {
            rgb[li+1] = rgb[li+1] * rgb[li+0] / Labl;
            rgb[li+2] = rgb[li+2] * rgb[li+0] / Labl;
          }
          else
          {
            //rgb[li+1] = rgb[li+1] * ((rgb[li+0] / Labl) * 0.01f);
            //rgb[li+2] = rgb[li+2] * ((rgb[li+0] / Labl) * 0.01f);
            rgb[li+1] = 0.0f;
            rgb[li+2] = 0.0f;
          }
        }
        //}
      }
    }
    else
    {
      for (int l=0; l<roi_out->width; l++)
      {
        int li=3*l;
        const int t = CLAMP((int)(rgb[li+0]/100.0*0xfffful), 0, 0xffff);
        float Labl = rgb[li+0];
        rgb[li+0] = d->table[t];
        // in Lab: correct compressed Luminance for saturation:
        //if(d->scale_saturation!=0 && Lab[li+0] > 0.01f)
        if(d->scale_saturation!=0)
        {
          if( Labl > 0.01f )
          {
            rgb[li+1] = rgb[li+1] * rgb[li+0] / Labl;
            rgb[li+2] = rgb[li+2] * rgb[li+0] / Labl;
          }
          else
          {
            rgb[li+1] = 0.0f;
            rgb[li+2] = 0.0f;
          }
        }
      }
    }

    // Back to Lab
    cmsDoTransform (d->xformo[dt_get_thread_num()], rgb, Lab, roi_out->width);
    for (int l=0; l<roi_out->width; l++)
    {
      int oi=ch*l, ri=3*l;
      out[m+oi+0] = Lab[ri+0];
      out[m+oi+1] = Lab[ri+1];
      out[m+oi+2] = Lab[ri+2];
    }

  }
#else
  const float Evs = lab_l_from_ev( ev_from_lab_l(LabMIN) );
#ifdef _OPENMP
  // TODO uncomment
  #pragma omp parallel for default(none) shared(roi_out, i, o, d) schedule(static)
  //#pragma omp parallel for default(none) shared(roi_out, i, o) schedule(static)
#endif
  for(int k=0; k<roi_out->height; k++)
  {
    float *in = ((float *)i) + k*ch*roi_out->width;
    float *out = ((float *)o) + k*ch*roi_out->width;
    if(d->lut_type == LUT_COEFFS)
    {
      for (int j=0; j<roi_out->width; j++,in+=ch,out+=ch)
      {
        const int t = CLAMP((int)(in[0]/100.0*0xfffful), 0, 0xffff);
        out[0] = Evs + d->table[t]*in[0];
        //out[0] = d->table[t];
        if(d->scale_saturation!=0 && in[0] > 0.01f)
        {
          out[1] = in[1] * out[0]/in[0];
          out[2] = in[2] * out[0]/in[0];
        }
        else
        {
          out[1] = in[1];
          out[2] = in[2];
        }
      }
    }
    else
    {
      for (int j=0; j<roi_out->width; j++,in+=ch,out+=ch)
      {
        const int t = CLAMP((int)(in[0]/100.0*0xfffful), 0, 0xffff);
        out[0] = d->table[t];
        if(d->scale_saturation!=0 && in[0] > 0.01f)
        {
          out[1] = in[1] * out[0]/in[0];
          out[2] = in[2] * out[0]/in[0];
        }
        else
        {
          out[1] = in[1];
          out[2] = in[2];
        }
      }
    }
  }
#endif
}

//=0-LOG(IF(D12> 7.9996248, ((D12+16)/116)^3,(D12*27)/24389),10)
float density_from_lab_L(float L)
{
  float D=0;

  if (L > 7.9996248)
  {
    D = powf( (L+16.0)/116.0, 3);
  }
  else
  {
    D = (L*27.0)/24389.0;
  }
  D=-log10f(D);

  return D;
}

//=IF( (10^(0-D)) > (216/24389), 116*((10^(0-D))^(1/3)) - 16, (24389/27)*(10^(0-D)) )
float lab_L_from_density(float D)
{
  float L;
  float tmp1;

  tmp1 = powf(10, 0.0 - D);
  if (tmp1 > 216.0/24389.0 )
  {
    L = 116.0 * cbrtf(tmp1) - 16.0;
  }
  else
  {
    L = (24389.0/27.0) * tmp1;
  }

  return L;
}

// density_from_ev ??????

void init_presets (dt_iop_module_so_t *self)
{
  dt_iop_densitycurve_params_t p;
  float Evs=-9.0, Eve=2.0, Ds=3.95583022953788, De=0.0;
  float step_x, step_y;

  p.densitycurve_preset = 0;
  p.size=MAX_DENSITY_SYSTEM_SIZE+2;

  Evs=ev_from_lab_l(LabMIN);
  Eve=ev_from_lab_l(100.0);
  step_x =(Eve-Evs)/(p.size-1);
  float ev_scale = 1.0/(Eve-Evs);
  float ev_off = (0.0 - Evs)*ev_scale;

  for(int k=0; k<p.size; k++)
  {
    p.points[k].x = Evs + step_x*k;
    p.points[k].y = density_from_lab_L(lab_l_from_ev(p.points[k].x))/DsMAX;
    p.points[k].x = ev_off + (Evs + step_x*k) * ev_scale;
  }
  p.points[p.size-1].y = 0.0;
  dt_gui_presets_add_generic(_("linear Lab gray"), self->op, &p, sizeof(p), 1);

  // linear density preset calculate border values for x and y
  p.size=6;
  Evs=ev_from_lab_l(LabMIN);
  Eve=ev_from_lab_l(100.0);
  Ds=density_from_lab_L(LabMIN);
  De=density_from_lab_L(100.0);

  step_y =(De-Ds)/(p.size-1);
  ev_scale = 1.0/(Eve-Evs);
  ev_off = (0.0 - Evs)*ev_scale;
  step_x =(Eve-Evs)/(p.size-1);

  for(int k=0; k<p.size; k++)
  {
    p.points[k].y =( Ds + step_y*k)/DsMAX;
    p.points[k].x = ev_off + (Evs + step_x*k) * ev_scale;
  }
  p.points[p.size-1].y = 0.0;

  //p.densitycurve_preset = 1;
  dt_gui_presets_add_generic(_("linear dencity"), self->op, &p, sizeof(p), 1);
}

void init(dt_iop_module_t *module)
{
  module->params = malloc(sizeof(dt_iop_densitycurve_params_t));
  module->default_params = malloc(sizeof(dt_iop_densitycurve_params_t));
  module->default_enabled = 0;
  module->request_histogram = 1;
  module->priority = 699;
  module->params_size = sizeof(dt_iop_densitycurve_params_t);
  module->gui_data = NULL;
  dt_iop_densitycurve_params_t tmp;

  float Evs=-9.0, Eve=2.0; // Startpoint and Endpoint not min max
  float step_x;

  tmp.densitycurve_preset = 0;
  tmp.size=2;

  tmp.spline_type = 0;
  tmp.lut_type =0;
  tmp.scale_saturation =1;

  tmp.points[tmp.size-1].x = ev_from_lab_l(100.0);
  tmp.points[tmp.size-1].y = density_from_lab_L(100.0)/DsMAX;
  tmp.points[tmp.size-1].y = 0.0;
  tmp.points[0].x = ev_from_lab_l(LabMIN);
  tmp.points[0].y = density_from_lab_L(LabMIN);

  Evs=ev_from_lab_l(LabMIN);
  Eve=ev_from_lab_l(100.0);
  step_x =(Eve-Evs)/(tmp.size-1);
  float ev_scale = 1.0/(Eve-Evs);
  float ev_off = (0.0 - Evs)*ev_scale;

  for(int k=0; k<tmp.size; k++)
  {
    tmp.points[k].x = ev_off +(Evs + step_x*k) * ev_scale;
    tmp.points[k].y = density_from_lab_L(lab_l_from_ev(Evs + step_x*k))/DsMAX;
  }
  tmp.points[tmp.size-1].y = 0.0;

  memcpy(module->params, &tmp, sizeof(dt_iop_densitycurve_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_densitycurve_params_t));
}

void init_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  // create part of the gegl pipeline
  dt_iop_densitycurve_data_t *d = (dt_iop_densitycurve_data_t *)malloc(sizeof(dt_iop_densitycurve_data_t));
  dt_iop_densitycurve_params_t *default_params = (dt_iop_densitycurve_params_t *)self->default_params;
  piece->data = (void *)d;

  d->curve = dt_draw_curve_new(0.0, 1.0, CUBIC_SPLINE/*MONOTONE_HERMITE*//*HERMITE_SPLINE*/ /*CATMULL_ROM*/);

  for(int k=0; k< default_params->size; k++)
  {
    (void)dt_draw_curve_add_point(d->curve, default_params->points[k].x, default_params->points[k].y);
  }

#ifdef HAVE_GEGL
  piece->input = piece->output = gegl_node_new_child(pipe->gegl, "operation", "gegl:dt-contrast-curve", "sampling-points", 65535, "curve", d->curve, NULL);
  //piece->data = NULL;
#else
  dt_draw_curve_calc_values(d->curve, 0.0f, 100.0f, 0x10000, NULL, d->table);
#endif

  d->input = NULL;
  d->xformi = (cmsHTRANSFORM *)malloc(sizeof(cmsHTRANSFORM)*dt_get_num_threads());
  d->xformo = (cmsHTRANSFORM *)malloc(sizeof(cmsHTRANSFORM)*dt_get_num_threads());
  for(int t=0; t<dt_get_num_threads(); t++)
  {
    d->xformi[t] = NULL;
    d->xformo[t] = NULL;
  }
  d->Lab = dt_colorspaces_create_lab_profile();
}

void cleanup_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  // clean up everything again.
#ifdef HAVE_GEGL
  // dt_iop_densitycurve_data_t *d = (dt_iop_densitycurve_data_t *)(piece->data);
  (void)gegl_node_remove_child(pipe->gegl, piece->input);
  // (void)gegl_node_remove_child(pipe->gegl, d->node);
  // (void)gegl_node_remove_child(pipe->gegl, piece->output);
#endif
  dt_iop_densitycurve_data_t *d = (dt_iop_densitycurve_data_t *)(piece->data);
  dt_draw_curve_destroy(d->curve);

  if(d->input) dt_colorspaces_cleanup_profile(d->input);
  dt_colorspaces_cleanup_profile(d->Lab);
  for(int t=0; t<dt_get_num_threads(); t++)
  {
    if(d->xformi[t]) cmsDeleteTransform(d->xformi[t]);
    if(d->xformo[t]) cmsDeleteTransform(d->xformo[t]);
  }
  free(d->xformi);
  free(d->xformo);

  free(piece->data);
}

void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_densitycurve_data_t *d = (dt_iop_densitycurve_data_t *)(piece->data);
  dt_iop_densitycurve_params_t *p = (dt_iop_densitycurve_params_t *)p1;
  float draw_xs[DT_IOP_DENSITYCURVE_RES], draw_ys[DT_IOP_DENSITYCURVE_RES];
  //const char *filename = "/opt/darktable/share/darktable/color/out/CIELab_to_UPLab2.icc";
  char filename[DT_MAX_PATH_LEN];


  //int sampler[20]={0,2,4,6,8,10,12,14,20,27,34,41,46,50,53,55,57,59,61,63};
  //int sampler[20]={0,4,7,10,13,16,19,14,23,27,34,41,46,50,53,55,57,59,61,63};
  //int sampler[12]={0,4,9,20,27,34,46,53,57,59,61,63};
  int sampler[20]= {0,4,9,13,17,20,23,25,27,30,34,41,46,50,53,55,57,59,61,63};
  int np=20;
  dt_draw_curve_t *tmp_curve;
  tmp_curve = dt_draw_curve_new(0.0, 1.0, p->spline_type/*0*/);

#ifdef HAVE_GEGL
  // pull in new params to gegl
  for(int k=0; k<p->size; k++) dt_draw_curve_set_point(d->curve, k, p->points[k].x, p->points[k].y);
  gegl_node_set(piece->input, "curve", d->curve, NULL);
#else
  // calculate users curve
  dt_draw_curve_destroy(d->curve);
  d->curve = dt_draw_curve_new(0.0, 1.0, p->spline_type);
  for(int k=0; k<p->size; k++)
  {
    (void)dt_draw_curve_add_point(d->curve, p->points[k].x, p->points[k].y);
  }
  dt_draw_curve_calc_values(d->curve, 0.0, density_from_lab_L(LabMIN)/DsMAX, DT_IOP_DENSITYCURVE_RES, draw_xs, draw_ys);

  float Evs=ev_from_lab_l(LabMIN);
  float Eve=ev_from_lab_l(100.0);
  float step_x =(Eve-Evs)/(DT_IOP_DENSITYCURVE_RES-1);

  // calculate Lab *L curve
  for(int k = 0; k < np; k++)
  {
    (void)dt_draw_curve_add_point(tmp_curve, lab_l_from_ev( Evs + step_x*sampler[k] )/100.0, lab_L_from_density(draw_ys[sampler[k]] * DsMAX)/100.0);
  }
  dt_draw_curve_calc_values(tmp_curve, 0.0f, 100.0f, 0x10000, NULL, d->table);

  d->lut_type = p->lut_type;
  d->scale_saturation = p->scale_saturation;
  if(d->lut_type == LUT_COEFFS)
  {
    for(int k=0; k<  (int) (lab_l_from_ev( Evs )*0xfffful/100.0); k++)
    {
      d->table[k] = 0.0f;
    }
    //const int t = CLAMP((int)(rgb[li+0]/100.0*0xfffful), 0, 0xffff);
    for(int k=(int) (lab_l_from_ev( Evs )/100.0*0xfffful); k<0x10000; k++)
    {
      d->table[k] = (d->table[k]- LabMIN)/(100.0 * k/0xfffful) ;
    }
  }

#endif
  if(d->input == NULL )
  {

    if ( ! dt_colorspaces_find_profile(filename, DT_MAX_PATH_LEN, "CIELab_to_UPLab2.icc", "out") )
    {
      d->input = cmsOpenProfileFromFile(filename, "r");
    }
    if(!d->input)
    {
      dt_control_log(_("not found UPLab profile fallback to Lab!"));
      d->input = dt_colorspaces_create_lab_profile();
    }
    const int num_threads = dt_get_num_threads();
    for(int t=0; t<num_threads; t++)
    {
      if(d->xformi[t])
      {
        cmsDeleteTransform(d->xformi[t]);
      }
      if(d->xformo[t])
      {
        cmsDeleteTransform(d->xformo[t]);
      }
      d->xformi[t] = cmsCreateTransform(d->Lab, TYPE_Lab_FLT, d->input, TYPE_Lab_FLT, INTENT_ABSOLUTE_COLORIMETRIC, 0);
      if(!d->xformi[t])
      {
        dt_control_log(_("Error create transform in!"));
      }
      d->xformo[t] = cmsCreateTransform(d->input, TYPE_Lab_FLT, d->Lab, TYPE_Lab_FLT, INTENT_ABSOLUTE_COLORIMETRIC, 0);
      if(!d->xformo[t])
      {
        dt_control_log(_("Error create transform out!"));
      }
    }
  }

  dt_draw_curve_destroy(tmp_curve);
}

static void
spline_type_callback (GtkComboBox *combo, dt_iop_module_t *self)
{
  dt_iop_densitycurve_gui_data_t *c = (dt_iop_densitycurve_gui_data_t *)self->gui_data;
  dt_iop_densitycurve_params_t *p = (dt_iop_densitycurve_params_t *)self->params;
  int active = gtk_combo_box_get_active(combo);

  switch(active)
  {
    case 1:
      p->spline_type = CATMULL_ROM;
      break;
    default:
    case 0:
      p->spline_type = CUBIC_SPLINE;
      break;
  }

  dt_draw_curve_destroy(c->minmax_curve);
  c->minmax_curve = dt_draw_curve_new(0.0, 1.0, p->spline_type);

  for(int k = 0; k < *c->Curve.n_points; k++)
  {
    (void)dt_draw_curve_add_point(c->minmax_curve, c->Curve.points[k].x, c->Curve.points[k].y);
  }

  dt_dev_add_history_item(darktable.develop, self, TRUE);

  gtk_widget_queue_draw(self->widget);
}

static void
lut_type_callback (GtkComboBox *combo, dt_iop_module_t *self)
{
//  dt_iop_densitycurve_gui_data_t *c = (dt_iop_densitycurve_gui_data_t *)self->gui_data;
  dt_iop_densitycurve_params_t *p = (dt_iop_densitycurve_params_t *)self->params;
  int active = gtk_combo_box_get_active(combo);

  switch(active)
  {
    case 1:
      p->lut_type = 1;
      break;
    default:
    case 0:
      p->lut_type = 0;
      break;
  }

  dt_dev_add_history_item(darktable.develop, self, TRUE);

  gtk_widget_queue_draw(self->widget);
}


void gui_update(struct dt_iop_module_t *self)
{

  dt_iop_densitycurve_gui_data_t *g = (dt_iop_densitycurve_gui_data_t *)self->gui_data;
  dt_iop_densitycurve_params_t *p = (dt_iop_densitycurve_params_t *)self->params;

  gtk_combo_box_set_active(g->spline_type, p->spline_type);
  g->numpoints = p->size;
  gtk_combo_box_set_active(g->calc_type, p->lut_type);

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->scale_sat), p->scale_saturation);

  dt_draw_curve_destroy(g->minmax_curve);
  g->minmax_curve = dt_draw_curve_new(0.0, 1.0, p->spline_type);

  for(int k=0; k < p->size; k++)
  {
    (void)dt_draw_curve_add_point(g->minmax_curve, p->points[k].x, p->points[k].y);
  }

  // nothing to do, gui curve is read directly from params during expose event.
  gtk_widget_queue_draw(self->widget);
}

void cleanup(dt_iop_module_t *module)
{
  free(module->gui_data);
  module->gui_data = NULL;
  free(module->params);
  module->params = NULL;
}

static void
scale_sat_changed (GtkToggleButton *button, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_densitycurve_params_t *p = (dt_iop_densitycurve_params_t *)self->params;

  p->scale_saturation = gtk_toggle_button_get_active(button);

  dt_dev_add_history_item(darktable.develop, self, TRUE);
  //dt_control_queue_draw_all();
  gtk_widget_queue_draw(self->widget);
}

void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_densitycurve_gui_data_t));
  dt_iop_densitycurve_gui_data_t *c = (dt_iop_densitycurve_gui_data_t *)self->gui_data;
  dt_iop_densitycurve_params_t *p = (dt_iop_densitycurve_params_t *)self->params;

  c->minmax_curve = dt_draw_curve_new(0.0, 1.0, CUBIC_SPLINE);
  //c->minmax_curve = dt_draw_curve_new(0.0, 1.0, /* HERMITE_SPLINE */ CATMULL_ROM);

  c->minmax_curve->c.m_max_y = density_from_lab_L(LabMIN)/DsMAX;
  c->minmax_curve->c.m_min_y = 0.0; // density_from_lab_L(100.0);

  for(int k=0; k < p->size; k++)
  {
    (void)dt_draw_curve_add_point(c->minmax_curve, p->points[k].x, p->points[k].y);
  }

  c->mouse_x = c->mouse_y = -1.0;
  c->selected = -1;
  c->selected_offset = c->selected_y = c->selected_min = c->selected_max = 0.0;
  c->dragging = 0;
  c->x_move = -1;
  c->numpoints = p->size;

  c->Curve.n_points=&c->numpoints;
  //c->Curve.n_points=&p->size;
  c->Curve.minmax_curve = c->minmax_curve;
  c->Curve.points = p->points;

  c->zonesystem_params = malloc(sizeof(dt_iop_zonesystem_params_t));
  // number of zones according to Evs
  dt_iop_zonesystem_params_t tmp = (dt_iop_zonesystem_params_t)
  {
    //11, {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1}
    (int) ceilf(ev_from_lab_l(100.0) - ev_from_lab_l(LabMIN)), {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1}
  };
  memcpy(c->zonesystem_params, &tmp, sizeof(dt_iop_zonesystem_params_t));


  self->widget = GTK_WIDGET(gtk_vbox_new(FALSE, 5));
  c->area = GTK_DRAWING_AREA(gtk_drawing_area_new());
  GtkWidget *asp = gtk_aspect_frame_new(NULL, 0.5, 0.5, 1.0, TRUE);
  gtk_box_pack_start(GTK_BOX(self->widget), asp, TRUE, TRUE, 0);
  gtk_container_add(GTK_CONTAINER(asp), GTK_WIDGET(c->area));
  gtk_drawing_area_size(c->area, 258, 258);

  c->zones = gtk_drawing_area_new();
  gtk_widget_set_size_request(c->zones, -1, 25);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(c->zones), TRUE, TRUE, 0);

  GtkTable *table = GTK_TABLE(gtk_table_new(2, 3, FALSE));
  int row=0;

  GtkWidget *label1 = gtk_label_new(_("spline type"));
  c->spline_type = GTK_COMBO_BOX(gtk_combo_box_new_text());
  gtk_combo_box_append_text(c->spline_type, _("cubic spline"));
  gtk_combo_box_append_text(c->spline_type, _("catmull rom"));

  gtk_misc_set_alignment(GTK_MISC(label1), 0.0, 0.5);
  gtk_table_attach(table, label1, 0, 1, row, row+1, GTK_EXPAND|GTK_FILL, 0, 0, 0);
  gtk_table_attach(table, GTK_WIDGET(c->spline_type), 1, 2, row, row+1, GTK_EXPAND|GTK_FILL, 0, 0, 0);
  row++;


  label1 = gtk_label_new(_("lut"));
  c->calc_type = GTK_COMBO_BOX(gtk_combo_box_new_text());
  gtk_combo_box_append_text(c->calc_type, _("values"));
  gtk_combo_box_append_text(c->calc_type, _("coefficients"));

  gtk_misc_set_alignment(GTK_MISC(label1), 0.0, 0.5);
  gtk_table_attach(table, label1, 0, 1, row, row+1, GTK_EXPAND|GTK_FILL, 0, 0, 0);
  gtk_table_attach(table, GTK_WIDGET(c->calc_type), 1, 2, row, row+1, GTK_EXPAND|GTK_FILL, 0, 0, 0);
  row++;

  c->scale_sat = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("scale saturation")));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(c->scale_sat), TRUE);
  g_object_set(G_OBJECT(c->scale_sat), "tooltip-text", _("change saturation when changing luminance."), (char *)NULL);
  gtk_table_attach(table, GTK_WIDGET(c->scale_sat), 0, 2, row, row+1, GTK_EXPAND|GTK_FILL, 0, 0, 0);
  row++;

  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(table), TRUE, TRUE, 0);

  gtk_widget_add_events(GTK_WIDGET(c->area), GDK_POINTER_MOTION_MASK | GDK_POINTER_MOTION_HINT_MASK
                        | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_LEAVE_NOTIFY_MASK | GTK_CAN_FOCUS);
  g_signal_connect (G_OBJECT (c->area), "expose-event",
                    G_CALLBACK (dt_iop_densitycurve_expose), self);
  g_signal_connect (G_OBJECT (c->area), "button-press-event",
                    G_CALLBACK (dt_iop_densitycurve_button_press), self);
  g_signal_connect (G_OBJECT (c->area), "button-release-event",
                    G_CALLBACK (dt_iop_densitycurve_button_release), self);
  g_signal_connect (G_OBJECT (c->area), "motion-notify-event",
                    G_CALLBACK (dt_iop_densitycurve_motion_notify), self);
  g_signal_connect (G_OBJECT (c->area), "leave-notify-event",
                    G_CALLBACK (dt_iop_densitycurve_leave_notify), self);
  g_signal_connect (G_OBJECT (c->area), "key-press-event",
                    G_CALLBACK(dt_iop_densitycurve_keypress_notify), self);
  g_signal_connect (G_OBJECT (c->area), "focus-in-event",
                    G_CALLBACK(dt_iop_densitycurve_on_focus_event), self);
  g_signal_connect (G_OBJECT (c->area), "focus-out-event",
                    G_CALLBACK(dt_iop_densitycurve_on_focus_event), self);
  g_signal_connect (G_OBJECT (c->spline_type), "changed",
                    G_CALLBACK (spline_type_callback), self);
  g_signal_connect (G_OBJECT (c->calc_type), "changed",
                    G_CALLBACK (lut_type_callback), self);
  g_signal_connect (G_OBJECT (c->scale_sat), "toggled",
                    G_CALLBACK (scale_sat_changed), self);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  dt_iop_densitycurve_gui_data_t *c = (dt_iop_densitycurve_gui_data_t *)self->gui_data;
  dt_draw_curve_destroy(c->minmax_curve);
  free(c->zonesystem_params);
  free(self->gui_data);
  self->gui_data = NULL;
}

static gboolean dt_iop_densitycurve_on_focus_event(GtkWidget *widget, GdkEventFocus *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_densitycurve_gui_data_t *c = (dt_iop_densitycurve_gui_data_t *)self->gui_data;

  if (event->in)
  {
    //c->selected = 0;
  }
  else
  {
    c->dragging = 0;
  }

  gtk_widget_queue_draw(widget);

  return FALSE;
}

static gboolean dt_iop_densitycurve_button_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  if(event->button == 1)
  {
    dt_iop_module_t *self = (dt_iop_module_t *)user_data;
    dt_iop_densitycurve_gui_data_t *c = (dt_iop_densitycurve_gui_data_t *)self->gui_data;
    c->dragging = 0;
    //c->selected = -1;
    c->x_move = -1;
    return TRUE;
  }
  return FALSE;
}

static gboolean dt_iop_densitycurve_leave_notify(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_densitycurve_gui_data_t *c = (dt_iop_densitycurve_gui_data_t *)self->gui_data;
  c->mouse_x = c->mouse_y = -1.0;
  //c->selected = -1;
  c->dragging = 0;
  gtk_grab_remove(widget);
  gtk_widget_queue_draw(widget);
  return TRUE;
}

static void dt_iop_densitycurve_sort(gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_densitycurve_gui_data_t *c = (dt_iop_densitycurve_gui_data_t *)self->gui_data;
  dt_iop_densitycurve_params_t *p = (dt_iop_densitycurve_params_t *)self->params;
  float selectedPoint_x=-1.0;

  //remember current selected point x value
  if (c->selected>=0)
    selectedPoint_x = c->Curve.points[c->selected].x;

  //make sure the points are in sorted order by x coord
  qsort(c->Curve.points,*c->Curve.n_points,sizeof(Point),
        points_qsort_compare);

  //update the selection to match the sorted list
  if (c->selected>=0)
  {
    for(int i = 0; i < *c->Curve.n_points; i++)
    {
      if (c->Curve.points[i].x == selectedPoint_x)
      {
        c->selected = i;
        break;
      }
    }
  }
  // update interpolation points
  for(int k=0; k < p->size; k++)
  {
    (void)dt_draw_curve_set_point(c->minmax_curve, k, c->Curve.points[k].x, c->Curve.points[k].y);
  }
}

static gboolean dt_iop_densitycurve_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  gtk_grab_add(widget);
  gtk_widget_grab_focus(widget);
  // set active point
  if(event->button == 1)
  {
    dt_iop_module_t *self = (dt_iop_module_t *)user_data;
    dt_iop_densitycurve_gui_data_t *c = (dt_iop_densitycurve_gui_data_t *)self->gui_data;
    dt_iop_densitycurve_params_t *p = (dt_iop_densitycurve_params_t *)self->params;

    const int inset = DT_GUI_CURVE_EDITOR_INSET;
    int width = widget->allocation.width, height = widget->allocation.height;
    width -= 2*inset;
    height -= 2*inset;
    const float mx = CLAMP(event->x - inset, 0, width)/(float)width;
    const float my = 1.0 - CLAMP(event->y - inset, 0, height)/(float)height;
    //int x = CLAMP(event->x - inset, 0, width);
    //int y = CLAMP(event->y - inset, 0, height);
    int closest_point;

    closest_point = curve_get_closest_point(&c->Curve, mx);
    c->selected = -1;
    c->mouse_x=event->x;
    c->mouse_y=event->y;

    if(event->y > height)
    {
      // move only x points. Dont touch y
      c->x_move = closest_point; //1;
    }
    else
    {
      // add new point or move existing
      if ( fabs(mx - c->Curve.points[closest_point].x)*width < 7 &&
           fabsf(my - c->Curve.points[closest_point].y )*height < 7 )
      {
        c->selected = closest_point;
      }
      if (*c->Curve.n_points < MAX_DENSITY_SYSTEM_SIZE+2)
      {
        // Add point only if no other point exists with the same x coordinate
        // and the added point is between the first and the last points.
        if (c->selected == -1 && !point_exists(&c->Curve, -1, mx) &&
            mx > c->Curve.points[0].x && mx < c->Curve.points[(*c->Curve.n_points)-1].x)
        {
          //add point
          int num = *c->Curve.n_points;

          //add it to the end
          c->Curve.points[num].x = mx;
          c->Curve.points[num].y = my;
          c->selected = num;

          *c->Curve.n_points = num + 1;
          p->size = *c->Curve.n_points;
          (void)dt_draw_curve_add_point(c->minmax_curve, c->Curve.points[num].x, c->Curve.points[num].y);
          dt_iop_densitycurve_sort(user_data);
          dt_dev_add_history_item(darktable.develop, self, TRUE);
        }
      }
      c->dragging = 1;
    }
    gtk_widget_queue_draw(widget);
    return TRUE;
  }
  return FALSE;
}

static gboolean dt_iop_densitycurve_expose(GtkWidget *widget, GdkEventExpose *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_densitycurve_gui_data_t *c = (dt_iop_densitycurve_gui_data_t *)self->gui_data;


  dt_iop_densitycurve_sort(user_data);

  const int inset = DT_GUI_CURVE_EDITOR_INSET;
  int width = widget->allocation.width, height = widget->allocation.height;

  cairo_surface_t *cst = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);

  // clear bg
  cairo_set_source_rgb (cr, .2, .2, .2);
  cairo_paint(cr);

  cairo_translate(cr, inset, inset);
  width -= 2*inset;
  height -= 2*inset;

//   const float mx = CLAMP(c->mouse_x - inset, 0, width)/(float)width;
//   const float my = CLAMP(c->mouse_y - inset, 0, height)/(float)height;

  // Draw graph area
  cairo_set_line_width(cr, 1.0);
  cairo_set_source_rgb (cr, .1, .1, .1);
  cairo_rectangle(cr, 0, 0, width, height);
  cairo_stroke(cr);

  cairo_set_source_rgb (cr, .3, .3, .3);
  cairo_rectangle(cr, 0, 0, width, height);
  cairo_fill(cr);

  // interpolate missing values
  dt_draw_curve_calc_values(c->minmax_curve, 0.0, density_from_lab_L(LabMIN)/DsMAX, DT_IOP_DENSITYCURVE_RES, c->draw_xs, c->draw_ys);


  // draw grid
  cairo_set_line_width(cr, .4);
  cairo_set_source_rgb (cr, .1, .1, .1);
  dt_draw_grid(cr, 4, 0, 0, width, height);

  // draw x positions
  cairo_set_line_width(cr, 1.);
  cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
  const float arrw = 7.0f;
  for(int k=1; k< (*c->Curve.n_points)-1; k++)
  {
    cairo_move_to(cr, width * c->Curve.points[k].x, height+inset-1);
    cairo_rel_line_to(cr, -arrw*.5f, 0);
    cairo_rel_line_to(cr, arrw*.5f, -arrw);
    cairo_rel_line_to(cr, arrw*.5f, arrw);
    cairo_close_path(cr);
    // selected pos of x only moved point
    if(c->x_move == k) cairo_fill(cr);
    else               cairo_stroke(cr);
  }

  // draw selected cursor
  cairo_set_line_width(cr, 1.);
  cairo_translate(cr, 0, height);

  // draw lum h istogram in background
  float *hist, hist_max;
  hist = self->histogram;
  hist_max = self->histogram_max[0];
  if(hist && hist_max > 0)
  {
    cairo_save(cr);
    cairo_scale(cr, width/63.0, -(height-5)/(float)hist_max);
    cairo_set_source_rgba(cr, .2, .2, .2, 0.5);
    dt_gui_histogram_draw_8(cr, hist, 0);
    cairo_restore(cr);
  }

  // draw mouse focus circle
  cairo_set_source_rgb(cr, .9, .9, .9);
  //const float pos = MAX(0, (DT_IOP_DENSITYCURVE_RES-1) * c->mouse_x/(float)width - 1);
  const float pos = MAX(0, (DT_IOP_DENSITYCURVE_RES-1) * c->mouse_x/(float)width - 1);
  int k = (int)pos;
  const float f = k - pos;
  if(k >= DT_IOP_DENSITYCURVE_RES-1) k = DT_IOP_DENSITYCURVE_RES - 2;
  float ht = -height*(f*c->draw_ys[k] + (1-f)*c->draw_ys[k+1]);
  cairo_arc(cr, c->mouse_x, ht, 4, 0, 2.*M_PI);
  cairo_stroke(cr);

  // draw curve
  cairo_set_line_width(cr, 2.);
  cairo_set_source_rgb(cr, .9, .9, .9);
  // cairo_set_line_cap  (cr, CAIRO_LINE_CAP_SQUARE);
  cairo_move_to(cr, 0, -height*c->draw_ys[0]);
  for(int k=1; k<DT_IOP_DENSITYCURVE_RES; k++) cairo_line_to(cr, k*width/(DT_IOP_DENSITYCURVE_RES-1.0), - height*c->draw_ys[k]);
  cairo_stroke(cr);
  //cairo_close_path(cr);

  // draw points
  for(int k=0; k< *c->Curve.n_points; k++)
  {
    cairo_new_sub_path(cr);
    cairo_arc(cr, width * c->Curve.points[k].x, -height * c->Curve.points[k].y, 3, 0, 2.*M_PI);
  }
  cairo_stroke(cr);

  //highlight selected point
  if (c->selected >= 0)
  {
    cairo_new_sub_path(cr);
    cairo_arc(cr, width * c->Curve.points[c->selected].x, -height * c->Curve.points[c->selected].y, 4, 0, 2.*M_PI);
    cairo_fill(cr);
  }

  cairo_destroy(cr);
  cairo_t *cr_pixmap = gdk_cairo_create(gtk_widget_get_window(widget));
  cairo_set_source_surface (cr_pixmap, cst, 0, 0);
  cairo_paint(cr_pixmap);
  cairo_destroy(cr_pixmap);
  cairo_surface_destroy(cst);

  //zonesystem_params
  dt_iop_zonesystem_bar_expose(c->zones, c->zonesystem_params);
  return TRUE;
}

static gboolean dt_iop_densitycurve_motion_notify(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_densitycurve_gui_data_t *c = (dt_iop_densitycurve_gui_data_t *)self->gui_data;
//  dt_iop_densitycurve_params_t *p = (dt_iop_densitycurve_params_t *)self->params;
  const int inset = DT_GUI_CURVE_EDITOR_INSET;
  int height = widget->allocation.height - 2*inset, width = widget->allocation.width - 2*inset;
  if(!c->dragging) c->mouse_x = CLAMP(event->x - inset, 0, width); // variate only y coordinate
  c->mouse_x = CLAMP(event->x - inset, 0, width);
  c->mouse_y = CLAMP(event->y - inset, 0, height);
  const float mx = CLAMP(event->x - inset, 0, width)/(float)width;
  const float my = 1.0 - CLAMP(event->y - inset, 0, height)/(float)height;

//   if ((event->state&GDK_BUTTON1_MASK)==0) return TRUE;

  if (c->selected >=0 && c->dragging )
  {
    // Don't allow a draging point to exceed or preceed neighbors or
    // else the spline algorithm will explode.
    // Also prevent moving central points beyond the two end points.
    if ( !point_exists(&c->Curve, c->selected, mx) &&
         ( c->selected==0 || mx > c->Curve.points[0].x ) &&
         ( c->selected==((*c->Curve.n_points) - 1) ||
           mx < c->Curve.points[(*c->Curve.n_points) - 1].x ) )
    {
      dt_draw_curve_set_point(c->minmax_curve, c->selected, mx, my);
      c->Curve.points[c->selected].x = mx;
      c->Curve.points[c->selected].y = my;
    }
    dt_dev_add_history_item(darktable.develop, self, TRUE);
  }

  gtk_widget_queue_draw(widget);

  gint x, y;
  gdk_window_get_pointer(event->window, &x, &y, NULL);
  return TRUE;
}

int curve_get_closest_point (Gcurve *curve, float x)
{
  int    closest_point = 0;
  float distance      = 999999.99;
  int    i;

  for (i = 0; i < *curve->n_points; i++)
  {
    if (curve->points[i].x >= 0.0 &&
        fabs (x - curve->points[i].x) < distance)
    {
      distance = fabs (x - curve->points[i].x);
      closest_point = i;
    }
  }

  if (distance > (1.0 / ((*curve->n_points) * 2.0)))
    closest_point = roundf(x * (float) ((*curve->n_points) - 1));

  return closest_point;
}

static gboolean dt_iop_densitycurve_keypress_notify(GtkWidget *widget, GdkEventKey *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_densitycurve_gui_data_t *c = (dt_iop_densitycurve_gui_data_t *)self->gui_data;
  dt_iop_densitycurve_params_t *p = (dt_iop_densitycurve_params_t *)self->params;
  const int inset = DT_GUI_CURVE_EDITOR_INSET;
  int width = widget->allocation.width, height = widget->allocation.height;
  width -= 2*inset;
  height -= 2*inset;

  //There must be a point selected
  //if (c->selected<0) return FALSE;

  //insert adds a point between the current one an the next one
  if(event->keyval == GDK_Insert)
  {
    if(c->selected >= *c->Curve.n_points - 1) return TRUE;
    if(*c->Curve.n_points >= MAX_DENSITY_SYSTEM_SIZE+2) return TRUE;
    if( (c->Curve.points[c->selected+1].x -
         c->Curve.points[c->selected].x ) < 2.0/(MAX_DENSITY_SYSTEM_SIZE+2-1) )
      return TRUE;

    dt_draw_curve_calc_values(c->minmax_curve, 0.0, density_from_lab_L(LabMIN)/DsMAX, DT_IOP_DENSITYCURVE_RES, c->draw_xs, c->draw_ys);

    //Add the point at the end - it will be sorted later anyway
    c->Curve.points[*c->Curve.n_points].x =
      (c->Curve.points[c->selected].x + c->Curve.points[c->selected+1].x )/2;


    c->Curve.points[*c->Curve.n_points].y = c->draw_ys[
        (int)( c->Curve.points[*c->Curve.n_points].x*(DT_IOP_DENSITYCURVE_RES) )];

    (void)dt_draw_curve_add_point(c->minmax_curve, c->Curve.points[*c->Curve.n_points].x, c->Curve.points[*c->Curve.n_points].y);

    c->selected = (*c->Curve.n_points);
    (*c->Curve.n_points)++;
    p->size = *c->Curve.n_points;
    dt_iop_densitycurve_sort(user_data);

    //redraw graph
    dt_dev_add_history_item(darktable.develop, self, TRUE);
    gtk_widget_queue_draw(widget);
    return TRUE;
  }

  //delete removes points
  if (event->keyval == GDK_Delete)
  {
    //a minimum of two points must be available at all times!
    if (*c->Curve.n_points == 2) return TRUE;
    if(c->selected >= *c->Curve.n_points - 1 || c->selected <= 0) return TRUE;

    for (int i=c->selected; i < *c->Curve.n_points - 1; i++)
    {
      c->Curve.points[i].x = c->Curve.points[i + 1].x;
      c->Curve.points[i].y = c->Curve.points[i + 1].y;
    }
    (*c->Curve.n_points)--;

    //set selected point to next point, but dont move to last point
    if (c->selected >= *c->Curve.n_points - 1) c->selected--;
    p->size = *c->Curve.n_points;

    dt_draw_curve_destroy(c->minmax_curve);
    c->minmax_curve = dt_draw_curve_new(0.0, 1.0, p->spline_type);
    for(int k=0; k < p->size; k++)
    {
      (void)dt_draw_curve_add_point(c->minmax_curve, p->points[k].x, p->points[k].y);
    }

    dt_dev_add_history_item(darktable.develop, self, TRUE);
    //redraw graph
    gtk_widget_queue_draw(widget);

    return TRUE;
  }

  //Home jumps to first point
  if (event->keyval == GDK_Home)
  {
    c->selected = 0;
    gtk_widget_queue_draw(widget);

    return TRUE;
  }

  //End jumps to last point
  if (event->keyval == GDK_End)
  {
    c->selected = *c->Curve.n_points - 1;
    gtk_widget_queue_draw(widget);

    return TRUE;
  }

  //Page Up jumps to previous point
  if (event->keyval == GDK_Page_Up)
  {
    c->selected--;
    if (c->selected < 0)
      c->selected = 0;
    gtk_widget_queue_draw(widget);

    return TRUE;
  }

  //Page Down jumps to next point
  if (event->keyval == GDK_Page_Down)
  {
    c->selected++;
    if (c->selected >= *c->Curve.n_points)
      c->selected = *c->Curve.n_points - 1;
    gtk_widget_queue_draw(widget);

    return TRUE;
  }

  //Up/Down/Left/Right moves the point around
  if (event->keyval == GDK_Up || event->keyval == GDK_Down ||
      event->keyval == GDK_Left || event->keyval == GDK_Right)
  {
    if (event->keyval==GDK_Up)
    {
      c->Curve.points[c->selected].y += 1.0/(height-1);
      if (c->Curve.points[c->selected].y > 1.0)
        c->Curve.points[c->selected].y = 1.0;
    }
    if (event->keyval == GDK_Down)
    {
      c->Curve.points[c->selected].y -= 1.0/(height-1);
      if (c->Curve.points[c->selected].y < 0.0)
        c->Curve.points[c->selected].y = 0.0;
    }
    if (event->keyval==GDK_Right)
    {
      float x = c->Curve.points[c->selected].x + 1.0/(width-1);
      //float y = c->Curve.points[c->selected].y;
      if (x > 1.0) x = 1.0;
      // Update point only if it does not override the next one
      if (c->selected == *c->Curve.n_points -1 ||
          x < c->Curve.points[c->selected +1].x-0.5/(width-1))
      {
        c->Curve.points[c->selected].x = x;
      }
    }
    if (event->keyval==GDK_Left)
    {
      float x = c->Curve.points[c->selected].x - 1.0/(width-1);
      //double y = curve->m_anchors[data->selectedPoint].y;
      if (x<0.0) x = 0.0;
      // Update point only if it does not override the previous one
      if (c->selected == 0 ||
          x > c->Curve.points[c->selected-1].x + 0.5/(width-1))
      {
        c->Curve.points[c->selected].x = x;
      }
    }
    dt_dev_add_history_item(darktable.develop, self, TRUE);
    gtk_widget_queue_draw(widget);

    return TRUE;
  }
  return FALSE;
}


/* calculate a zonemap with scale values for each zone based on controlpoints from param */
static inline void
_iop_zonesystem_calculate_zonemap (struct dt_iop_zonesystem_params_t *p, float *zonemap)
{
  int steps=0;
  int pk=0;

  for (int k=0; k<p->size; k++)
  {
    if((k>0 && k<p->size-1) && p->zone[k] == -1)
      steps++;
    else
    {
      /* set 0 and 1.0 for first and last element in zonesystem size, or the actually parameter value */
      zonemap[k] = k==0?0.0:k==(p->size-1)?1.0:p->zone[k];

      /* for each step from pk to k, calculate values
          for now this is linear distributed
      */
      for (int l=1; l<=steps; l++)
        zonemap[pk+l] = zonemap[pk]+(((zonemap[k]-zonemap[pk])/(steps+1))*l);

      /* store k into pk and reset zone steps for next range*/
      pk = k;
      steps = 0;
    }
  }
}

#define DT_ZONESYSTEM_INSET 5
#define DT_ZONESYSTEM_BAR_SPLIT_WIDTH 0.0
#define DT_ZONESYSTEM_REFERENCE_SPLIT 0.30
//dt_iop_zonesystem_bar_expose (GtkWidget *widget, GdkEventExpose *event, dt_iop_module_t *self)
static gboolean
dt_iop_zonesystem_bar_expose (GtkWidget *widget, dt_iop_zonesystem_params_t *p)
{
  //dt_iop_zonesystem_params_t *p = (dt_iop_zonesystem_params_t *)self->params;

  const int inset = DT_ZONESYSTEM_INSET;
  int width = widget->allocation.width, height = widget->allocation.height;
  cairo_surface_t *cst = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);

  /* clear background */
  cairo_set_source_rgb (cr, .15, .15, .15);
  cairo_paint(cr);


  /* translate and scale */
  width-=2*inset;
  height-=2*inset;
  cairo_save(cr);
  cairo_translate(cr, inset, inset);
  cairo_scale(cr,width,height);

  /* render the bars */
  float zonemap[MAX_ZONE_SYSTEM_SIZE]= {0};
  _iop_zonesystem_calculate_zonemap (p, zonemap);
  float s=(1./(p->size-2));
  cairo_set_antialias (cr, CAIRO_ANTIALIAS_NONE);
  for(int i=0; i<p->size-1; i++)
  {
    /* draw the reference zone */
    float z=s*i;
    cairo_rectangle (cr,(1./(p->size-1))*i,0,(1./(p->size-1)),DT_ZONESYSTEM_REFERENCE_SPLIT-DT_ZONESYSTEM_BAR_SPLIT_WIDTH);
    cairo_set_source_rgb (cr, z, z, z);
    cairo_fill (cr);

    /* draw zone mappings */
    cairo_rectangle (cr,
                     zonemap[i],DT_ZONESYSTEM_REFERENCE_SPLIT+DT_ZONESYSTEM_BAR_SPLIT_WIDTH,
                     (zonemap[i+1]-zonemap[i]),1.0-DT_ZONESYSTEM_REFERENCE_SPLIT);
    cairo_set_source_rgb (cr, z, z, z);
    cairo_fill (cr);

  }
  cairo_set_antialias (cr, CAIRO_ANTIALIAS_DEFAULT);
  cairo_restore (cr);

  /* render zonebar control lines */
  cairo_set_antialias (cr, CAIRO_ANTIALIAS_NONE);
  cairo_set_line_width (cr, 1.);
  cairo_rectangle (cr,inset,inset,width,height);
  cairo_set_source_rgb (cr, .1,.1,.1);
  cairo_stroke (cr);
  cairo_set_antialias (cr, CAIRO_ANTIALIAS_DEFAULT);

  /* render control points handles */
//   cairo_set_source_rgb (cr, 0.6, 0.6, 0.6);
//   cairo_set_line_width (cr, 1.);
//   const float arrw = 7.0f;
//   for (int k=1; k<p->size-1; k++)
//   {
//     float nzw=zonemap[k+1]-zonemap[k];
//     float pzw=zonemap[k]-zonemap[k-1];
//     if (
//       ( ((g->mouse_x/width) > (zonemap[k]-(pzw/2.0))) &&
//         ((g->mouse_x/width) < (zonemap[k]+(nzw/2.0))) ) ||
//       p->zone[k] != -1)
//     {
//       gboolean is_under_mouse = ((width*zonemap[k]) - arrw*.5f < g->mouse_x &&  (width*zonemap[k]) + arrw*.5f > g->mouse_x);
//
//       cairo_move_to(cr, inset+(width*zonemap[k]), height+(2*inset)-1);
//       cairo_rel_line_to(cr, -arrw*.5f, 0);
//       cairo_rel_line_to(cr, arrw*.5f, -arrw);
//       cairo_rel_line_to(cr, arrw*.5f, arrw);
//       cairo_close_path(cr);
//
//       if ( is_under_mouse )
//         cairo_fill(cr);
//       else
//         cairo_stroke(cr);
//
//     }
//   }


  /* push mem surface into widget */
  cairo_destroy (cr);
  cairo_t *cr_pixmap = gdk_cairo_create (gtk_widget_get_window (widget));
  cairo_set_source_surface (cr_pixmap, cst, 0, 0);
  cairo_paint (cr_pixmap);
  cairo_destroy (cr_pixmap);
  cairo_surface_destroy (cst);

  return TRUE;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
