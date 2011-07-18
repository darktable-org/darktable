/*
    This file is part of darktable,
    copyright (c) 2009--2010 Thierry Leconte.

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

//
// A tonemapping module using Durand's process :
// <http://graphics.lcs.mit.edu/~fredo/PUBLI/Siggraph2002/>
//
// Use andrew adams et al.'s permutohedral lattice, for fast bilateral filtering
// See Permutohedral.h
//

extern "C"
{

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
#include "develop/develop.h"
#include "develop/imageop.h"
#include "control/control.h"
#include "dtgtk/slider.h"
#include "dtgtk/resetlabel.h"
#include "gui/gtk.h"
#include <gtk/gtk.h>
#include <inttypes.h>
}

#include "iop/Permutohedral.h"

extern "C"
{
  DT_MODULE(1)

  typedef struct dt_iop_tonemapping_params_t
  {
    float contrast,Fsize;
  }
  dt_iop_tonemapping_params_t;

  typedef struct dt_iop_tonemapping_gui_data_t
  {
    GtkDarktableSlider *contrast, *Fsize;
  }
  dt_iop_tonemapping_gui_data_t;

  typedef struct dt_iop_tonemapping_data_t
  {
    float contrast,Fsize;
  }
  dt_iop_tonemapping_data_t;

  const char *name()
  {
    return _("tone mapping");
  }


  int
  groups ()
  {
    return IOP_GROUP_BASIC;
  }
 
  int
  flags ()
  {
    return IOP_FLAGS_SUPPORTS_BLENDING;
  }

  void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *ivoid, void *ovoid, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
  {
    dt_iop_tonemapping_data_t *data = (dt_iop_tonemapping_data_t *)piece->data;
    const int ch = piece->colors;

    int width,height,size;
    float sigma_s;
    const float sigma_r=0.4;

    width=roi_in->width;
    height=roi_in->height;
    size=width*height;
    const float iw=piece->buf_in.width*roi_out->scale;
    const float ih=piece->buf_in.height*roi_out->scale;

    sigma_s=(data->Fsize/100.0)*fminf(iw,ih);
    if(sigma_s<3.0) sigma_s=3.0;

    PermutohedralLattice<3,2> lattice(size, omp_get_max_threads());

    // Build I=log(L)
    // and splat into the lattice
#ifdef _OPENMP
#pragma omp parallel for shared(lattice)
#endif
    for(int j=0; j<height; j++)
    {
      int index = j*width;
      const int thread = omp_get_thread_num();
      const float *in = (const float*)ivoid + j*width*ch;
      for(int i=0; i<width; i++, index++, in+=ch)
      {
        float L = 0.2126*in[0]+ 0.7152*in[1] + 0.0722*in[2];
        if(L<=0.0) L=1e-6;
        L = logf(L);
        float pos[3] = {i/sigma_s, j/sigma_s, L/sigma_r};
        float val[2] = {L,  1.0};
        lattice.splat(pos, val, index, thread);
      }
    }

    lattice.merge_splat_threads();

    // blur the lattice
    lattice.blur();

    //
    // Durand process :
    // r=R/(input intensity), g=G/input intensity, B=B/input intensity
    // log(base)=Bilateral(log(input intensity))
    // log(detail)=log(input intensity)-log(base)
    // log (output intensity)=log(base)*compressionfactor+log(detail)
    // R output = r*exp(log(output intensity)), etc.
    //
    // Simplyfing :
    // R output = R/(input intensity)*exp(log(output intensity))
    //          = R*exp(log(output intensity)-log(input intensity))
    //          = R*exp(log(base)*compressionfactor+log(input intensity)-log(base)-log(input intensity))
    //          = R*exp(log(base)*(compressionfactor-1))
    //
    // Plus :
    //  Before compressing the base intensity , we remove average base intensity in order to not have
    //  variable average intensity when varying compression factor.
    //  after compression we substract 2.0 to have an average intensiy at middle tone.
    //

    const float contr = 1./data->contrast;
#ifdef _OPENMP
#pragma omp parallel for
#endif
    for(int j=0; j<height; j++)
    {
      int index = j*width;
      const float *in = (const float*)ivoid + j*width*ch;
      float *out = (float*)ovoid + j*width*ch;
      for(int i=0; i<width; i++, index++, in+=ch, out+=ch)
      {
        float val[2];
        lattice.slice(val, index);
        float L = 0.2126*in[0]+ 0.7152*in[1] + 0.0722*in[2];
        if(L<=0.0) L=1e-6;
        L = logf(L);
        const float B = val[0]/val[1];
        const float detail = L - B;
        const float Ln = expf(B*(contr - 1.0f) + detail - 1.0f);

        out[0]=in[0]*Ln;
        out[1]=in[1]*Ln;
        out[2]=in[2]*Ln;
      }
    }
    // also process the clipping point, as good as we can without knowing
    // the local environment (i.e. assuming detail == 0)
    float *pmax = piece->pipe->processed_maximum;
    float L = 0.2126*pmax[0]+ 0.7152*pmax[1] + 0.0722*pmax[2];
    if(L<=0.0) L=1e-6;
    L = logf(L);
    const float Ln = expf(L*(contr - 1.0f) - 1.0f);
    for(int k=0; k<3; k++) pmax[k] *= Ln;
  }


// GUI
//
  static void
  contrast_callback (GtkDarktableSlider *slider, gpointer user_data)
  {
    dt_iop_module_t *self = (dt_iop_module_t *)user_data;
    if(self->dt->gui->reset) return;
    dt_iop_tonemapping_params_t *p = (dt_iop_tonemapping_params_t *)self->params;
    p->contrast = dtgtk_slider_get_value(slider);
    dt_dev_add_history_item(darktable.develop, self, TRUE);
  }

  static void
  Fsize_callback (GtkDarktableSlider *slider, gpointer user_data)
  {
    dt_iop_module_t *self = (dt_iop_module_t *)user_data;
    if(self->dt->gui->reset) return;
    dt_iop_tonemapping_params_t *p = (dt_iop_tonemapping_params_t *)self->params;
    p->Fsize = dtgtk_slider_get_value(slider);
    dt_dev_add_history_item(darktable.develop, self, TRUE);
  }

  void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
  {
    dt_iop_tonemapping_params_t *p = (dt_iop_tonemapping_params_t *)p1;
    dt_iop_tonemapping_data_t *d = (dt_iop_tonemapping_data_t *)piece->data;
    d->contrast = p->contrast;
    d->Fsize = p->Fsize/100.0;
  }

  void init_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
  {
    piece->data = malloc(sizeof(dt_iop_tonemapping_data_t));
    self->commit_params(self, self->default_params, pipe, piece);
  }

  void cleanup_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
  {
    free(piece->data);
  }

  void gui_update(struct dt_iop_module_t *self)
  {
    dt_iop_module_t *module = (dt_iop_module_t *)self;
    dt_iop_tonemapping_gui_data_t *g = (dt_iop_tonemapping_gui_data_t *)self->gui_data;
    dt_iop_tonemapping_params_t *p = (dt_iop_tonemapping_params_t *)module->params;
    dtgtk_slider_set_value(g->contrast, p->contrast);
    dtgtk_slider_set_value(g->Fsize, p->Fsize*100.0);
  }

  void reload_defaults(dt_iop_module_t *module)
  {
    if(module->dev->image->flags & DT_IMAGE_HDR)
      module->default_enabled = 1;
    else
      module->default_enabled = 0;

    dt_iop_tonemapping_params_t tmp = (dt_iop_tonemapping_params_t)
    {
      2.5,0.1
    };
    memcpy(module->params, &tmp, sizeof(dt_iop_tonemapping_params_t));
    memcpy(module->default_params, &tmp, sizeof(dt_iop_tonemapping_params_t));
  }

  void init(dt_iop_module_t *module)
  {
    // module->data = malloc(sizeof(dt_iop_tonemapping_data_t));
    module->params = (dt_iop_params_t*)malloc(sizeof(dt_iop_tonemapping_params_t));
    module->default_params = (dt_iop_params_t*)malloc(sizeof(dt_iop_tonemapping_params_t));
    module->default_enabled = 1;
  module->priority = 177; // module order created by iop_dependencies.py, do not edit!
    module->params_size = sizeof(dt_iop_tonemapping_params_t);
    module->gui_data = NULL;
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
    self->gui_data = malloc(sizeof(dt_iop_tonemapping_gui_data_t));
    dt_iop_tonemapping_gui_data_t *g = (dt_iop_tonemapping_gui_data_t *)self->gui_data;
    dt_iop_tonemapping_params_t *p = (dt_iop_tonemapping_params_t *)self->params;

    self->widget = gtk_hbox_new(FALSE, 0);
    GtkWidget *vbox = gtk_vbox_new(FALSE, DT_GUI_IOP_MODULE_CONTROL_SPACING);
    gtk_box_pack_start(GTK_BOX(self->widget), vbox, TRUE, TRUE, 5);

    g->contrast = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,1.0, 5.0000, 0.1, p->contrast, 3));
    gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(g->contrast), TRUE, TRUE, 0);
    dtgtk_slider_set_label(g->contrast,_("contrast compression"));
    g_signal_connect (G_OBJECT (g->contrast), "value-changed",G_CALLBACK (contrast_callback), self);

    g->Fsize = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0,100.0, 1.0, p->Fsize*100.0, 1));
    dtgtk_slider_set_format_type(g->Fsize, DARKTABLE_SLIDER_FORMAT_PERCENT);
    dtgtk_slider_set_label(g->Fsize,_("spatial extent"));
    dtgtk_slider_set_unit(g->Fsize,(gchar *)"%");
    gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(g->Fsize), TRUE, TRUE, 0);
    g_signal_connect (G_OBJECT (g->Fsize), "value-changed",G_CALLBACK (Fsize_callback), self);
  }

  void gui_cleanup(struct dt_iop_module_t *self)
  {
    free(self->gui_data);
    self->gui_data = NULL;
  }

}

// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
