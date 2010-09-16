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
#include "gui/gtk.h"
#include "iop/Permutohedral.h"
#include <gtk/gtk.h>
#include <inttypes.h>

/**
 * implementation of the 5d-color bilateral filter using andrew adams et al.'s
 * permutohedral lattice, which they kindly provided online as c++ code, under new bsd license.
 */

DT_MODULE(1)

typedef struct dt_iop_bilateral_params_t
{
  // standard deviations of the gauss to use for blurring in the dimensions x,y,r,g,b (or L*,a*,b*)
  float sigma[5];
}
dt_iop_bilateral_params_t;

typedef struct dt_iop_bilateral_gui_data_t
{
  GtkVBox   *vbox1,  *vbox2;
  GtkLabel  *label1, *label2, *label3, *label4, *label5;
  GtkDarktableSlider *scale1, *scale2, *scale3, *scale4, *scale5;
}
dt_iop_bilateral_gui_data_t;

typedef struct dt_iop_bilateral_data_t
{
  float sigma[5];
}
dt_iop_bilateral_data_t;

const char *name()
{
  return _("denoise (slow)");
}

int
groups ()
{
  return IOP_GROUP_CORRECT;
}

void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *ivoid, void *ovoid, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_bilateral_data_t *data = (dt_iop_bilateral_data_t *)piece->data;
  float *in  = (float *)ivoid;
  float *out = (float *)ovoid;

  float sigma[5];
  sigma[0] = data->sigma[0] * roi_in->scale / piece->iscale;
  sigma[1] = data->sigma[1] * roi_in->scale / piece->iscale;
  sigma[2] = data->sigma[2];
  sigma[3] = data->sigma[3];
  sigma[4] = data->sigma[4];
  if(fmaxf(sigma[0], sigma[1]) < .1)
  {
    memcpy(out, in, sizeof(float)*3*roi_out->width*roi_out->height);
    return;
  }

  // if rad <= 6 use naive version!
  const int rad = (int)(3.0*fmaxf(sigma[0],sigma[1])+1.0);
  if(rad <= 6 && self->dev->image->flags & DT_IMAGE_THUMBNAIL)
  { // no use denoising the thumbnail. takes ages without permutohedral
    memcpy(out, in, sizeof(float)*3*roi_out->width*roi_out->height);
  }
  else if(rad <= 6)
  {
    float mat[2*(6+1)*2*(6+1)];
    const int wd = 2*rad+1;
    float *m = mat + rad*wd + rad;
    float weight = 0.0f;
    const float isig2col[3] = {1.0/(2.0f*sigma[2]*sigma[2]), 1.0/(2.0f*sigma[3]*sigma[3]), 1.0/(2.0f*sigma[4]*sigma[4])};
    // init gaussian kernel
    for(int l=-rad;l<=rad;l++) for(int k=-rad;k<=rad;k++)
      weight += m[l*wd + k] = expf(- (l*l + k*k)/(2.f*sigma[0]*sigma[0]));
    for(int l=-rad;l<=rad;l++) for(int k=-rad;k<=rad;k++)
      m[l*wd + k] /= weight;
#ifdef _OPENMP
  #pragma omp parallel for default(none) schedule(static) shared(ivoid,ovoid,roi_in,roi_out,m,mat,isig2col) private(in,out)
#endif
    for(int j=rad;j<roi_out->height-rad;j++)
    {
      in  = ((float *)ivoid) + 3*(j*roi_in->width  + rad);
      out = ((float *)ovoid) + 3*(j*roi_out->width + rad);
      float weights[2*(6+1)*2*(6+1)];
      float *w = weights + rad*wd + rad;
      float sumw;
      for(int i=rad;i<roi_out->width-rad;i++)
      {
        sumw = 0.0f;
        for(int l=-rad;l<=rad;l++) for(int k=-rad;k<=rad;k++)
          sumw += w[l*wd+k] = m[l*wd+k]*expf(-
              ((in[0]-in[3*(l*roi_in->width+k)+0])*(in[0]-in[3*(l*roi_in->width+k)+0])*isig2col[0] +
               (in[1]-in[3*(l*roi_in->width+k)+1])*(in[1]-in[3*(l*roi_in->width+k)+1])*isig2col[1] +
               (in[2]-in[3*(l*roi_in->width+k)+2])*(in[2]-in[3*(l*roi_in->width+k)+2])*isig2col[2]));
        for(int l=-rad;l<=rad;l++) for(int k=-rad;k<=rad;k++) w[l*wd+k] /= sumw;
        for(int c=0;c<3;c++) out[c] = 0.0f;
        for(int l=-rad;l<=rad;l++) for(int k=-rad;k<=rad;k++)
          for(int c=0;c<3;c++) out[c] += in[3*(l*roi_in->width+k)+c]*w[l*wd+k];
        out += 3; in += 3;
      }
    }
    // fill unprocessed border
    in  = (float *)ivoid;
    out = (float *)ovoid;
    for(int j=0;j<rad;j++)
      memcpy(((float*)ovoid) + 3*j*roi_out->width, ((float*)ivoid) + 3*j*roi_in->width, 3*sizeof(float)*roi_out->width);
    for(int j=roi_out->height-rad;j<roi_out->height;j++)
      memcpy(((float*)ovoid) + 3*j*roi_out->width, ((float*)ivoid) + 3*j*roi_in->width, 3*sizeof(float)*roi_out->width);
    for(int j=rad;j<roi_out->height-rad;j++)
    {
      for(int i=0;i<rad;i++)
        for(int c=0;c<3;c++) out[3*(roi_out->width*j + i) + c] = in[3*(roi_in->width*j + i) + c];
      for(int i=roi_out->width-rad;i<roi_out->width;i++)
        for(int c=0;c<3;c++) out[3*(roi_out->width*j + i) + c] = in[3*(roi_in->width*j + i) + c];
    }
  }
  else
  {
    for(int k=0;k<5;k++) sigma[k] = 1.0f/sigma[k];
    PermutohedralLattice lattice(5, 4, roi_in->width*roi_in->height);
    
    // splat into the lattice
    for(int j=0;j<roi_in->height;j++) for(int i=0;i<roi_in->width;i++)
    {
      float pos[5] = {i*sigma[0], j*sigma[1], in[0]*sigma[2], in[1]*sigma[3], in[2]*sigma[4]};
      float val[4] = {in[0], in[1], in[2], 1.0};
      lattice.splat(pos, val);
      in += 3;
    }
    
    // blur the lattice
    lattice.blur();
    
    // slice from the lattice
    lattice.beginSlice();
    for(int j=0;j<roi_in->height;j++) for(int i=0;i<roi_in->width;i++)
    {
      float val[4];
      lattice.slice(val);
      for(int k=0;k<3;k++) out[k] = val[k]/val[3];
      out += 3;
    }
  }
}

static void
sigma_callback (GtkDarktableSlider *slider, dt_iop_module_t *self)
{
  if(self->dt->gui->reset) return;
  dt_iop_bilateral_params_t   *p = (dt_iop_bilateral_params_t *)self->params;
  dt_iop_bilateral_gui_data_t *g = (dt_iop_bilateral_gui_data_t *)self->gui_data;
  int i = 0;
  if     (slider == g->scale1) i = 0;
  else if(slider == g->scale2) i = 1;
  else if(slider == g->scale3) i = 2;
  else if(slider == g->scale4) i = 3;
  else if(slider == g->scale5) i = 4;
  p->sigma[i] = dtgtk_slider_get_value(slider);
  if(i == 0) p->sigma[1] = p->sigma[0];
  dt_dev_add_history_item(darktable.develop, self);
}

void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_bilateral_params_t *p = (dt_iop_bilateral_params_t *)p1;
#ifdef HAVE_GEGL
  fprintf(stderr, "[bilateral] TODO: implement gegl version!\n");
  // pull in new params to gegl
#else
  dt_iop_bilateral_data_t *d = (dt_iop_bilateral_data_t *)piece->data;
  for(int k=0;k<5;k++) d->sigma[k] = p->sigma[k];
#endif
}

void init_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
#ifdef HAVE_GEGL
  // create part of the gegl pipeline
  piece->data = NULL;
#else
  piece->data = malloc(sizeof(dt_iop_bilateral_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
#endif
}

void cleanup_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
#ifdef HAVE_GEGL
  // clean up everything again.
  (void)gegl_node_remove_child(pipe->gegl, piece->input);
  // no free necessary, no data is alloc'ed
#else
  free(piece->data);
#endif
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_module_t *module = (dt_iop_module_t *)self;
  dt_iop_bilateral_gui_data_t *g = (dt_iop_bilateral_gui_data_t *)self->gui_data;
  dt_iop_bilateral_params_t *p = (dt_iop_bilateral_params_t *)module->params;
  dtgtk_slider_set_value(g->scale1, p->sigma[0]);
  // dtgtk_slider_set_value(g->scale2, p->sigma[1]);
  dtgtk_slider_set_value(g->scale3, p->sigma[2]);
  dtgtk_slider_set_value(g->scale4, p->sigma[3]);
  dtgtk_slider_set_value(g->scale5, p->sigma[4]);
}

void init(dt_iop_module_t *module)
{
  // module->data = malloc(sizeof(dt_iop_bilateral_data_t));
  module->params = (dt_iop_params_t *)malloc(sizeof(dt_iop_bilateral_params_t));
  module->default_params = (dt_iop_params_t *)malloc(sizeof(dt_iop_bilateral_params_t));
  module->default_enabled = 0;
  module->priority = 150;
  module->params_size = sizeof(dt_iop_bilateral_params_t);
  module->gui_data = NULL;
  dt_iop_bilateral_params_t tmp = (dt_iop_bilateral_params_t){{15.0, 15.0, 0.005, 0.005, 0.005}};
  memcpy(module->params, &tmp, sizeof(dt_iop_bilateral_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_bilateral_params_t));
}

void cleanup(dt_iop_module_t *module)
{
  free(module->gui_data);
  module->gui_data = NULL;
  free(module->params);
  module->params = NULL;
}

void gui_init(dt_iop_module_t *self)
{
  self->gui_data = (dt_iop_gui_data_t *)malloc(sizeof(dt_iop_bilateral_gui_data_t));
  dt_iop_bilateral_gui_data_t *g = (dt_iop_bilateral_gui_data_t *)self->gui_data;
  dt_iop_bilateral_params_t *p = (dt_iop_bilateral_params_t *)self->params;

  self->widget = GTK_WIDGET(gtk_hbox_new(FALSE, 0));
  g->vbox1 = GTK_VBOX(gtk_vbox_new(FALSE, 0));
  g->vbox2 = GTK_VBOX(gtk_vbox_new(FALSE, 0));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->vbox1), FALSE, FALSE, 5);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->vbox2), TRUE, TRUE, 5);
  g->label1 = GTK_LABEL(gtk_label_new(_("radius")));
  // g->label2 = GTK_LABEL(gtk_label_new(_("height")));
  g->label3 = GTK_LABEL(gtk_label_new(_("red")));
  g->label4 = GTK_LABEL(gtk_label_new(_("green")));
  g->label5 = GTK_LABEL(gtk_label_new(_("blue")));
  gtk_misc_set_alignment(GTK_MISC(g->label1), 0.0, 0.5);
  // gtk_misc_set_alignment(GTK_MISC(g->label2), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(g->label3), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(g->label4), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(g->label5), 0.0, 0.5);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label1), TRUE, TRUE, 0);
  // gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label2), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label3), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label4), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label5), TRUE, TRUE, 0);
  g->scale1 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR, 1.0, 30.0, 1.0, p->sigma[0], 1));
  // g->scale2 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR, 1.0, 30.0, 1.0, p->sigma[1], 0));
  g->scale3 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR, 0.0001, .1, 0.001, p->sigma[2], 4));
  g->scale4 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR, 0.0001, .1, 0.001, p->sigma[3], 4));
  g->scale5 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR, 0.0001, .1, 0.001, p->sigma[4], 4));
  gtk_object_set(GTK_OBJECT(g->scale1), "tooltip-text", _("spatial extent of the gaussian"), NULL);
  // gtk_object_set(GTK_OBJECT(g->scale2), "tooltip-text", _(""), NULL);
  gtk_object_set(GTK_OBJECT(g->scale3), "tooltip-text", _("how much to blur red"), NULL);
  gtk_object_set(GTK_OBJECT(g->scale4), "tooltip-text", _("how much to blur green"), NULL);
  gtk_object_set(GTK_OBJECT(g->scale5), "tooltip-text", _("how much to blur blue"), NULL);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale1), TRUE, TRUE, 0);
  // gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale2), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale3), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale4), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale5), TRUE, TRUE, 0);

  g_signal_connect (G_OBJECT (g->scale1), "value-changed",
                    G_CALLBACK (sigma_callback), self);
  // g_signal_connect (G_OBJECT (g->scale2), "value-changed",
                    // G_CALLBACK (sigma_callback), self);
  g_signal_connect (G_OBJECT (g->scale3), "value-changed",
                    G_CALLBACK (sigma_callback), self);
  g_signal_connect (G_OBJECT (g->scale4), "value-changed",
                    G_CALLBACK (sigma_callback), self);
  g_signal_connect (G_OBJECT (g->scale5), "value-changed",
                    G_CALLBACK (sigma_callback), self);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

}

