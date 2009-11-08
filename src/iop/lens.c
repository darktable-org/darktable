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
#include "iop/lens.h"

static lfDatabase *dt_iop_lensfun_db = NULL;

void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *i, void *o, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_lensfun_data_t *d = (dt_iop_lensfun_data_t *)piece->data;
  float *in  = (float *)i;
  float *out = (float *)o;

  lfModifier *modifier = lf_modifier_new( d->lens, d->crop, piece->iwidth, piece->iheight);

  // TODO: build these in gui somewhere:
  // float real_scale = powf(2.0, uf->conf->lens_scale);

  int modflags = lf_modifier_initialize(
      modifier, d->lens, LF_PF_F32,
      d->focal, d->aperture,
      d->distance, d->scale,
      d->target_geom, d->modify_flags, d->inverse);

  if(d->inverse)
  {
    // TODO: reverse direction (useful for renderings)
  }
  else
  {
    // acquire temp memory for image buffer
    const size_t req = roi_in->width*roi_in->height*3*sizeof(float);
    if(req > 0 && d->tmpbuf_len < req)
    {
      d->tmpbuf_len = req;
      d->tmpbuf = (float *)realloc(d->tmpbuf, req);
    }
    memcpy(d->tmpbuf, in, req);
    if (modflags & LF_MODIFY_VIGNETTING)
    {
      // TODO: openmp this? is lf thread-safe?
      for (int y = 0; y < roi_in->height; y++)
      {
        /* Colour correction: vignetting and CCI */
        // TODO: actually this way row stride does not matter. but give a buffer pointer
        //       offset by -roi_in.x * sizeof pixel :)
        if(lf_modifier_apply_color_modification (modifier,
              d->tmpbuf + 3*roi_in->width*y, 0.0, y,
              roi_in->width, 1, LF_CR_3 (RED, GREEN, BLUE), 0)) break;
      }
    }

    const size_t req2 = roi_in->width*2*3*sizeof(float);
    if (modflags & (LF_MODIFY_TCA | LF_MODIFY_DISTORTION |
          LF_MODIFY_GEOMETRY | LF_MODIFY_SCALE))
    {
      // acquire temp memory for distorted pixel coords
      if(req2 > 0 && d->tmpbuf2_len < req2)
      {
        d->tmpbuf2_len = req2;
        d->tmpbuf2 = (float *)realloc(d->tmpbuf2, req2);
      }
      // TODO: openmp this?
      for (int y = 0; y < roi_out->height; y++)
      {
        if (!lf_modifier_apply_subpixel_geometry_distortion (
              modifier, 0, y, roi_out->width, 1, d->tmpbuf2)) break;
        // TODO: reverse transform the global coords from lf to our buffer!
        const float *pi = d->tmpbuf2;
        for (int x = 0; x < roi_out->width; x++)
        {
          for(int c=0;c<3;c++) 
          {
            const int ii = (int)pi[0], jj = (int)pi[1];
            if(ii >= 0 || jj >= 0 || ii <= roi_in->width-2 || jj <= roi_in->height-2) 
            {
              const float fi = pi[0] - ii, fj = pi[1] - jj;
              out[c] = // in[3*(roi_in->width*jj + ii) + c];
              ((1.0f-fj)*(1.0f-fi)*in[3*(roi_in->width*(jj)   + (ii)  ) + c] +
               (1.0f-fj)*(     fi)*in[3*(roi_in->width*(jj)   + (ii+1)) + c] +
               (     fj)*(     fi)*in[3*(roi_in->width*(jj+1) + (ii+1)) + c] +
               (     fj)*(1.0f-fi)*in[3*(roi_in->width*(jj+1) + (ii)  ) + c]);
            }
            else for(int c=0;c<3;c++) out[c] = 0.0f;
            pi++;
          }
          out += 3;
        }
      }
    }
  }
  lf_modifier_destroy(modifier);
}

const char *name()
{
  return _("lens distortions");
}

void modify_roi_out(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, dt_iop_roi_t *roi_out, const dt_iop_roi_t *roi_in)
{
  *roi_out = *roi_in;
  // TODO: fwd transform with given params
}

void modify_roi_in(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, const dt_iop_roi_t *roi_out, dt_iop_roi_t *roi_in)
{
  *roi_in = *roi_out;
  // TODO: inverse transform with given params
}

void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_lensfun_params_t *p = (dt_iop_lensfun_params_t *)p1;
#ifdef HAVE_GEGL
  // pull in new params to gegl
#error "lensfun needs to be ported to GEGL!"
#else
  dt_iop_lensfun_data_t *d = (dt_iop_lensfun_data_t *)piece->data;

  const lfCamera *camera = NULL;
  const lfCamera **cam = NULL;
  dt_image_t *img = self->dev->image;
  if(img->exif_maker[0] || img->exif_model[0])
  {
    cam = lf_db_find_cameras_ext(dt_iop_lensfun_db,
        img->exif_maker, img->exif_model, 0);
    if(cam) camera = cam[0];
  }
  if(p->lens[0])
  {
    const lfLens **lens = lf_db_find_lenses_hd(dt_iop_lensfun_db, camera, NULL,
        p->lens, 0);
    if(lens)
    {
      printf("lensfun found lens %s\n", lens[0]->Model);
      lf_lens_copy(d->lens, lens[0]);
      lf_free (lens);
    }
  }
  lf_free(cam);
  d->modify_flags = p->modify_flags;
  d->inverse      = p->inverse;
  d->scale        = p->scale;
  d->crop         = p->crop;
  d->focal        = p->focal;
  d->aperture     = p->aperture;
  d->distance     = p->distance;
  d->target_geom  = p->target_geom;
#endif
}

void init_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
#ifdef HAVE_GEGL
#error "lensfun needs to be ported to GEGL!"
#else
  piece->data = malloc(sizeof(dt_iop_lensfun_data_t));
  dt_iop_lensfun_data_t *d = (dt_iop_lensfun_data_t *)piece->data;

  d->tmpbuf2_len = 0;
  d->tmpbuf2 = NULL;
  d->tmpbuf_len = 0;
  d->tmpbuf = NULL;
  d->lens = lf_lens_new();
  self->commit_params(self, self->default_params, pipe, piece);
#endif
}

void cleanup_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
#ifdef HAVE_GEGL
#error "lensfun needs to be ported to GEGL!"
#else
  dt_iop_lensfun_data_t *d = (dt_iop_lensfun_data_t *)piece->data;
  lf_lens_destroy(d->lens);
  free(d->tmpbuf);
  free(d->tmpbuf2);
  free(piece->data);
#endif
}

void gui_update(struct dt_iop_module_t *self)
{
  // dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)self->gui_data;
  // dt_iop_lensfun_params_t *p = (dt_iop_lensfun_params_t *)self->params;
  // gtk_range_set_value(GTK_RANGE(g->scale1), p->cx);
}

void init(dt_iop_module_t *module)
{
  dt_iop_lensfun_db = lf_db_new ();
  if(lf_db_load(dt_iop_lensfun_db) != LF_NO_ERROR)
  {
    fprintf(stderr, "[iop_lens]: could not load lensfun database!\n");
  }
  // module->data = malloc(sizeof(dt_iop_lensfun_data_t));
  module->params = malloc(sizeof(dt_iop_lensfun_params_t));
  module->default_params = malloc(sizeof(dt_iop_lensfun_params_t));
  module->default_enabled = 0;
  module->params_size = sizeof(dt_iop_lensfun_params_t);
  module->gui_data = NULL;
  module->priority = 940;
  // get all we can from exif:
  dt_iop_lensfun_params_t tmp;
  strncpy(tmp.lens, module->dev->image->exif_lens, 52);
  tmp.crop     = module->dev->image->exif_crop;
  tmp.aperture = module->dev->image->exif_aperture;
  tmp.focal    = module->dev->image->exif_focal_length;
  tmp.scale    = 1.0;
  tmp.inverse  = 0;
  tmp.modify_flags = LF_MODIFY_TCA | LF_MODIFY_VIGNETTING |
    LF_MODIFY_DISTORTION | LF_MODIFY_GEOMETRY | LF_MODIFY_SCALE;
  tmp.distance = 5.0;
  tmp.target_geom = LF_RECTILINEAR;
  printf("exif returns crop %f\n", tmp.crop);

  // init crop from db:
  dt_image_t *img = module->dev->image;
  char model[100];  // truncate often complex descriptions.
  strncpy(model, img->exif_model, 100);
  for(char cnt = 0, *c = model; c < model+100 && *c != '\0'; c++) if(*c == ' ') if(++cnt == 2) *c = '\0';
  printf("lensfun searching for %s - %s\n", img->exif_maker, model);
  if(img->exif_maker[0] || model[0])
  {
    printf("...\n");
    // const lfCamera **cam = lf_db_find_cameras_ext(dt_iop_lensfun_db,
        // img->exif_maker, model, LF_SEARCH_LOOSE);
    const lfCamera **cam = lf_db_find_cameras_ext(dt_iop_lensfun_db,
        img->exif_maker, img->exif_model, 0);
    if(cam)
    {
      printf("lensfun db found %s - %s\n", cam[0]->Maker, cam[0]->Model);
      img->exif_crop = tmp.crop = cam[0]->CropFactor;
      lf_free(cam);
    }
  }

  printf("lensfun inited with crop %f\n", tmp.crop);

  memcpy(module->params, &tmp, sizeof(dt_iop_lensfun_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_lensfun_params_t));
}

void cleanup(dt_iop_module_t *module)
{
  lf_db_destroy(dt_iop_lensfun_db);
  free(module->gui_data);
  module->gui_data = NULL;
  free(module->params);
  module->params = NULL;
}

void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_lensfun_gui_data_t));
  dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)self->gui_data;
  // dt_iop_lensfun_params_t *p = (dt_iop_lensfun_params_t *)self->params;

  self->widget = GTK_WIDGET(gtk_hbox_new(FALSE, 0));
  g->vbox1 = GTK_VBOX(gtk_vbox_new(FALSE, 0));
  g->vbox2 = GTK_VBOX(gtk_vbox_new(FALSE, 0));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->vbox1), FALSE, FALSE, 5);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->vbox2), TRUE, TRUE, 5);
  g->label1 = GTK_LABEL(gtk_label_new("crop x"));
  g->label2 = GTK_LABEL(gtk_label_new("crop y"));
#if 0
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
#endif
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

