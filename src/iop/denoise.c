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
#include "iop/denoise.h"
#include "develop/develop.h"
#include "control/control.h"
#include "gui/gtk.h"

#define POW2(a) ((a)*(a))

/* this piece of 1337-code is from dcraw. most enjoyable parts removed, as we don't need them here. */
void hat_transform (float *temp, float *base, int st, int size, int sc)
{
  int i;
  for (i=0; i < sc; i++)
    temp[i] = 2*base[st*i] + base[st*(sc-i)] + base[st*(i+sc)];
  for (; i+sc < size; i++)
    temp[i] = 2*base[st*i] + base[st*(i-sc)] + base[st*(i+sc)];
  for (; i < size; i++)
    temp[i] = 2*base[st*i] + base[st*(i-sc)] + base[st*(2*size-2-(i+sc))];
}

void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *ivoid, void *ovoid, int x, int y, float scale, int iwidth, int iheight)
{
  dt_iop_denoise_data_t *d = (dt_iop_denoise_data_t *)piece->data;
  float *in  = (float *)ivoid;
  float *out = (float *)ovoid;
  // FIXME: proper scale handling!
  if((d->luma == 0.0 && d->chroma == 0.0) || scale < 1.0)
  {
    memcpy(out, in, 3*iwidth*iheight*sizeof(float));
  }
  else
  {
    float threshold[3] = {d->luma*0.03, d->chroma*0.03, d->chroma*0.03};

    float *fimg=0, *temp, thold;
    int size, lev, hpass, lpass, row, col;
    static const float noise[] =
    { 0.8002,0.2735,0.1202,0.0585,0.0291,0.0152,0.0080,0.0044 };

    if ((size = iheight*iwidth) < 0x15550000)
      fimg = (float *) malloc ((size*3 + iheight + iwidth) * sizeof *fimg);
    if(!fimg)
    {
      fprintf(stderr, "[denoise] could not alloc temp memory!\n");
      return;
    }
    temp = fimg + size*3;
    // denoise each channel
    for(int c=0;c<3;c++)
    {
      for (int i=0; i < size; i++)
        // fimg[i] = 256 * sqrt(image[3*i+c]);
        // fimg[i] = sqrtf(in[3*i+c]);
        fimg[i] = in[3*i+c];
      for (hpass=lev=0; lev < 5; lev++) {
        lpass = size*((lev & 1)+1);
        for (row=0; row < iheight; row++) {
          hat_transform (temp, fimg+hpass+row*iwidth, 1, iwidth, 1 << lev);
          for (col=0; col < iwidth; col++)
            fimg[lpass + row*iwidth + col] = temp[col] * 0.25;
        }
        for (col=0; col < iwidth; col++) {
          hat_transform (temp, fimg+lpass+col, iwidth, iheight, 1 << lev);
          for (row=0; row < iheight; row++)
            fimg[lpass + row*iwidth + col] = temp[row] * 0.25;
        }
        thold = threshold[c] * noise[lev];
        for (int i=0; i < size; i++) {
          fimg[hpass+i] -= fimg[lpass+i];
          if	(fimg[hpass+i] < -thold) fimg[hpass+i] += thold;
          else if (fimg[hpass+i] >  thold) fimg[hpass+i] -= thold;
          else	 fimg[hpass+i] = 0;
          if (hpass) fimg[i] += fimg[hpass+i];
        }
        hpass = lpass;
      }
      for (int i=0; i < size; i++)
      {
        // image[i][c] = CLIP(SQR(fimg[i]+fimg[lpass+i])/0x10000);
        // out[3*i+c] = POW2(fimg[i]+fimg[lpass+i]);
        out[3*i+c] = fimg[i]+fimg[lpass+i];
        // printf("%f ", out[3*i+c]);
      }
      fflush(stdout);
    }
    free (fimg);
  }
}

#if 0
static void
hat_transform (float *temp, float *base, int st, int size, int sc)
{
  int i;
  for (i = 0; i < sc; i++)
    temp[i] = 2 * base[st * i] + base[st * (sc - i)] + base[st * (i + sc)];
  for (; i + sc < size; i++)
    temp[i] = 2 * base[st * i] + base[st * (i - sc)] + base[st * (i + sc)];
  for (; i < size; i++)
    temp[i] = 2 * base[st * i] + base[st * (i - sc)]
      + base[st * (2 * size - 2 - (i + sc))];
}

/* originates from dcraw */
void
wavelet_denoise (float *fimg[3], unsigned int width,
		 unsigned int height, float threshold, double low)
{
  float *temp, thold;
  unsigned int i, lev, lpass, hpass, size, col, row;
  double stdev[5];
  unsigned int samples[5];

  size = width * height;

  /* FIXME: replace by GIMP functions */
  temp = (float *) malloc (MAX2 (width, height) * sizeof (float));

  hpass = 0;
  for (lev = 0; lev < 5; lev++)
  {
    if (b != 0) gimp_progress_update (a + b * lev / 5.0);

    lpass = ((lev & 1) + 1);
    for (row = 0; row < height; row++)
    {
      hat_transform (temp, fimg[hpass] + row * width, 1, width, 1 << lev);
      for (col = 0; col < width; col++)
      {
        fimg[lpass][row * width + col] = temp[col] * 0.25;
      }
    }

    for (col = 0; col < width; col++)
    {
      hat_transform (temp, fimg[lpass] + col, width, height, 1 << lev);
      for (row = 0; row < height; row++)
      {
        fimg[lpass][row * width + col] = temp[row] * 0.25;
      }
    }

    thold = 5.0 / (1 << 6) * exp (-2.6 * sqrt (lev + 1)) * 0.8002 / exp (-2.6);

    /* initialize stdev values for all intensities */
    stdev[0] = stdev[1] = stdev[2] = stdev[3] = stdev[4] = 0.0;
    samples[0] = samples[1] = samples[2] = samples[3] = samples[4] = 0;

    /* calculate stdevs for all intensities */
    for (i = 0; i < size; i++)
    {
      fimg[hpass][i] -= fimg[lpass][i];
      if (fimg[hpass][i] < thold && fimg[hpass][i] > -thold)
      {
        if (fimg[lpass][i] > 0.8) {
          stdev[4] += fimg[hpass][i] * fimg[hpass][i];
          samples[4]++;
        } else if (fimg[lpass][i] > 0.6) {
          stdev[3] += fimg[hpass][i] * fimg[hpass][i];
          samples[3]++;
        }	else if (fimg[lpass][i] > 0.4) {
          stdev[2] += fimg[hpass][i] * fimg[hpass][i];
          samples[2]++;
        }	else if (fimg[lpass][i] > 0.2) {
          stdev[1] += fimg[hpass][i] * fimg[hpass][i];
          samples[1]++;
        } else {
          stdev[0] += fimg[hpass][i] * fimg[hpass][i];
          samples[0]++;
        }
      }
    }
    stdev[0] = sqrt (stdev[0] / (samples[0] + 1));
    stdev[1] = sqrt (stdev[1] / (samples[1] + 1));
    stdev[2] = sqrt (stdev[2] / (samples[2] + 1));
    stdev[3] = sqrt (stdev[3] / (samples[3] + 1));
    stdev[4] = sqrt (stdev[4] / (samples[4] + 1));

    /* do thresholding */
    for (i = 0; i < size; i++)
    {
      if (fimg[lpass][i] > 0.8) {
        thold = threshold * stdev[4];
      } else if (fimg[lpass][i] > 0.6) {
        thold = threshold * stdev[3];
      } else if (fimg[lpass][i] > 0.4) {
        thold = threshold * stdev[2];
      } else if (fimg[lpass][i] > 0.2) {
        thold = threshold * stdev[1];
      } else {
        thold = threshold * stdev[0];
      }

      if (fimg[hpass][i] < -thold)
        fimg[hpass][i] += thold - thold * low;
      else if (fimg[hpass][i] > thold)
        fimg[hpass][i] -= thold - thold * low;
      else
        fimg[hpass][i] *= low;

      if (hpass)
        fimg[0][i] += fimg[hpass][i];
    }
    hpass = lpass;
  }

  for (i = 0; i < size; i++)
    fimg[0][i] = fimg[0][i] + fimg[lpass][i];

  /* FIXME: replace by GIMP functions */
  free (temp);
}
#endif


#if 0 // bilateral filter is prohibitively slow.
void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *i, void *o, int x, int y, float scale, int width, int height)
{
  dt_iop_denoise_data_t *d = (dt_iop_denoise_data_t *)piece->data;
  float *in  = (float *)i;
  float *out = (float *)o;
  if(d->luma == 0.0 && d->chroma == 0.0)
  {
    memcpy(out, in, 3*width*height*sizeof(float));
  }
  else
  {
    // init scale-dependent blur mask:
    float luma = scale * d->luma+0.01, chroma = scale * d->chroma + 0.01;
    int iradius = MIN(DT_IOP_DENOISE_MAX_RAD, MAX(luma, chroma));
    int wd = 2*iradius + 1;
    float *gauss_luma   = (float *)malloc(sizeof(float)*wd*wd);
    float *gauss_chroma = (float *)malloc(sizeof(float)*wd*wd);
    for (int y=-iradius;y<=iradius;y++) for (int x=-iradius;x<=iradius;x++)
    {
      gauss_luma  [x+iradius + (y+iradius)*wd] = expf(- 0.5f*(POW2(x)+POW2(y))/luma  );
      gauss_chroma[x+iradius + (y+iradius)*wd] = expf(- 0.5f*(POW2(x)+POW2(y))/chroma);
    }
    // process pixels:
    for(int x=iradius+1;x<width-iradius-1;x++) for(int y=iradius+1;y<height-iradius-1;y++)
    {
      float *center = in + (x+iradius + (y+iradius)*width*3);
      float accum[3] = {0, 0, 0};
      float count_luma = 0.0, count_chroma = 0.0;
      for(int v=-iradius;v<=iradius;v++) for(int u=-iradius;u<=iradius;u++)
      {
        int i = x + iradius + u, j = y + iradius + v;
        float *src = in + (i + j*width)*3;
        float diff_luma   = expf(- POW2(center[0] - src[0]) * d->edges);
        float diff_chroma = expf(- (POW2(center[1] - src[1])+POW2(center[2] - src[2])) * d->edges);
        float w_luma   = diff_luma   * gauss_luma  [u + iradius + (v + iradius)*wd];
        float w_chroma = diff_chroma * gauss_chroma[u + iradius + (v + iradius)*wd];
        count_luma += w_luma;
        count_chroma += w_chroma;
        accum[0] += w_luma   * src[0];
        accum[1] += w_chroma * src[1];
        accum[2] += w_chroma * src[2];
      }
      out[3*(x + width*y)]   = accum[0] / count_luma;
      out[3*(x + width*y)+1] = accum[1] / count_chroma;
      out[3*(x + width*y)+2] = accum[2] / count_chroma;
    }
    free(gauss_luma);
    free(gauss_chroma);
  }
}
#endif

void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_denoise_params_t *p = (dt_iop_denoise_params_t *)p1;
#ifdef HAVE_GEGL
  fprintf(stderr, "[denoise] TODO: implement gegl version!\n");
  // pull in new params to gegl
  // gegl_node_set(piece->input, "linear_value", p->linear, "gamma_value", p->gamma, NULL);
#else
  dt_iop_denoise_data_t *d = (dt_iop_denoise_data_t *)piece->data;
  d->luma   = p->luma;
  d->chroma = p->chroma;
  // d->edges  = p->edges;
#endif
}

void init_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
#ifdef HAVE_GEGL
  // create part of the gegl pipeline
  piece->data = NULL;
  dt_iop_denoise_params_t *default_params = (dt_iop_denoise_params_t *)self->default_params;
  // piece->input = piece->output = gegl_node_new_child(pipe->gegl, "operation", "gegl:dt-gamma", "linear_value", default_params->linear, "gamma_value", default_params->gamma, NULL);
#else
  piece->data = malloc(sizeof(dt_iop_denoise_data_t));
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
  dt_iop_denoise_gui_data_t *g = (dt_iop_denoise_gui_data_t *)self->gui_data;
  dt_iop_denoise_params_t *p = (dt_iop_denoise_params_t *)module->params;
  gtk_range_set_value(GTK_RANGE(g->scale1), p->luma);
  gtk_range_set_value(GTK_RANGE(g->scale2), p->chroma);
  // gtk_range_set_value(GTK_RANGE(g->scale3), p->edges);
}

void init(dt_iop_module_t *module)
{
  // module->data = malloc(sizeof(dt_iop_denoise_data_t));
  module->params = malloc(sizeof(dt_iop_denoise_params_t));
  module->default_params = malloc(sizeof(dt_iop_denoise_params_t));
  module->params_size = sizeof(dt_iop_denoise_params_t);
  module->gui_data = NULL;
  dt_iop_denoise_params_t tmp = (dt_iop_denoise_params_t){0.0, 0.0};//, 0.0};
  memcpy(module->params, &tmp, sizeof(dt_iop_denoise_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_denoise_params_t));
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
  self->gui_data = malloc(sizeof(dt_iop_denoise_gui_data_t));
  dt_iop_denoise_gui_data_t *g = (dt_iop_denoise_gui_data_t *)self->gui_data;
  dt_iop_denoise_params_t *p = (dt_iop_denoise_params_t *)self->params;

  self->widget = GTK_WIDGET(gtk_hbox_new(FALSE, 0));
  g->vbox1 = GTK_VBOX(gtk_vbox_new(FALSE, 0));
  g->vbox2 = GTK_VBOX(gtk_vbox_new(FALSE, 0));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->vbox1), FALSE, FALSE, 5);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->vbox2), TRUE, TRUE, 5);
  g->label1 = GTK_LABEL(gtk_label_new("luma"));
  g->label2 = GTK_LABEL(gtk_label_new("chroma"));
  // g->label3 = GTK_LABEL(gtk_label_new("edges"));
  gtk_misc_set_alignment(GTK_MISC(g->label1), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(g->label2), 0.0, 0.5);
  // gtk_misc_set_alignment(GTK_MISC(g->label3), 0.0, 0.5);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label1), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label2), TRUE, TRUE, 0);
  // gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label3), TRUE, TRUE, 0);
  g->scale1 = GTK_HSCALE(gtk_hscale_new_with_range(0.0, 1.0000, 0.01));
  g->scale2 = GTK_HSCALE(gtk_hscale_new_with_range(0.0, 1.0000, 0.01));
  // g->scale3 = GTK_HSCALE(gtk_hscale_new_with_range(0.0, 1.0000, 0.01));
  gtk_scale_set_digits(GTK_SCALE(g->scale1), 2);
  gtk_scale_set_digits(GTK_SCALE(g->scale2), 2);
  // gtk_scale_set_digits(GTK_SCALE(g->scale3), 2);
  gtk_scale_set_value_pos(GTK_SCALE(g->scale1), GTK_POS_LEFT);
  gtk_scale_set_value_pos(GTK_SCALE(g->scale2), GTK_POS_LEFT);
  // gtk_scale_set_value_pos(GTK_SCALE(g->scale3), GTK_POS_LEFT);
  gtk_range_set_value(GTK_RANGE(g->scale1), p->luma);
  gtk_range_set_value(GTK_RANGE(g->scale2), p->chroma);
  // gtk_range_set_value(GTK_RANGE(g->scale3), p->edges);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale1), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale2), TRUE, TRUE, 0);
  // gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale3), TRUE, TRUE, 0);

  g_signal_connect (G_OBJECT (g->scale1), "value-changed",
                    G_CALLBACK (luma_callback), self);
  g_signal_connect (G_OBJECT (g->scale2), "value-changed",
                    G_CALLBACK (chroma_callback), self);
  // g_signal_connect (G_OBJECT (g->scale3), "value-changed",
                    // G_CALLBACK (edges_callback), self);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

void luma_callback (GtkRange *range, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_denoise_params_t *p = (dt_iop_denoise_params_t *)self->params;
  p->luma = gtk_range_get_value(range);
  dt_dev_add_history_item(darktable.develop, self);
}

void chroma_callback (GtkRange *range, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_denoise_params_t *p = (dt_iop_denoise_params_t *)self->params;
  p->chroma = gtk_range_get_value(range);
  dt_dev_add_history_item(darktable.develop, self);
}

/*void edges_callback (GtkRange *range, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_denoise_params_t *p = (dt_iop_denoise_params_t *)self->params;
  p->edges = gtk_range_get_value(range);
  dt_dev_add_history_item(darktable.develop, self);
}*/
