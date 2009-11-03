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
#include "iop/lensfun.h"

static Image *ApplyModifier (int modflags, bool reverse, Image *img,
                             const lfModifier *mod)
{
    // Create a new image where we will copy the modified image
    Image *newimg = new Image ();
    // Output image always equals input image size, although
    // this is not a requirement of the library, it's just a
    // limitation of the testbed.
    newimg->Resize (img->width, img->height);

    int lwidth = img->width * 2 * 3;
    float *pos = new float [lwidth];

    int step_start = reverse ? 2 : 0;
    int step_delta = reverse ? -1 : +1;
    int step_finish = reverse ? -1 : 3;

    for (int step = step_start; step != step_finish; step += step_delta)
    {
        RGBpixel *dst = newimg->image;
        char *imgdata = (char *)img->image;
        bool ok = true;

        img->InitInterpolation (opts.Interpolation);

        for (unsigned y = 0; ok && y < img->height; y++)
            switch (step)
            {
                case 0:
                    ok = false;
                    break;

                case 2:
                    /* TCA and geometry correction */
                    ok = mod->ApplySubpixelGeometryDistortion (0.0, y, img->width, 1, pos);
                    if (ok)
                    {
                        float *src = pos;
                        for (unsigned x = 0; x < img->width; x++)
                        {
                            dst->red   = img->GetR (src [0], src [1]);
                            dst->green = img->GetG (src [2], src [3]);
                            dst->blue  = img->GetB (src [4], src [5]);
                            src += 2 * 3;
                            dst++;
                        }
                    }
                    break;

                case 1:
                    /* Colour correction: vignetting and CCI */
                    ok = mod->ApplyColorModification (imgdata, 0.0, y, img->width, 1,
                        LF_CR_4 (RED, GREEN, BLUE, UNKNOWN), 0);
                    imgdata += img->width * 4;
                    break;

            }
        // After TCA and distortion steps switch img and newimg.
        // This is crucial since newimg is now the input image
        // to the next stage.
        if (ok && (step == 0 || step == 2))
        {
            Image *tmp = newimg;
            newimg = img;
            img = tmp;
        }
    }

    delete [] pos;
    delete newimg;
    return img;
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

void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *i, void *o, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_lensfun_data_t *d = (dt_iop_lensfun_data_t *)piece->data;
  float *in  = (float *)i;
  float *out = (float *)o;
  
  for(int j=0;j<roi_out->height;j++)
  {
    for(int i=0;i<roi_out->width;i++)
    {
    }
  }
}

void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_lensfun_params_t *p = (dt_iop_lensfun_params_t *)p1;
#ifdef HAVE_GEGL
  // pull in new params to gegl
  #error "lensfun needs to be ported to GEGL!"
#else
  dt_iop_lensfun_data_t *d = (dt_iop_lensfun_data_t *)piece->data;
  // TODO:
  // d->* = 
#endif
}

void init_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
#ifdef HAVE_GEGL
  #error "lensfun needs to be ported to GEGL!"
#else
  piece->data = malloc(sizeof(dt_iop_lensfun_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
          g_print ("\b\b\b[%ux%u], processing", img->width, img->height);
        lfModifier *mod = lfModifier::Create (lens, opts.Crop, img->width, img->height);
        int modflags = mod->Initialize (
            lens, LF_PF_F32, opts.Focal,
            opts.Aperture, opts.Distance, opts.Scale, opts.TargetGeom,
            opts.ModifyFlags, opts.Inverse);

        if (!mod)
        {
            g_print ("\rWarning: failed to create modifier\n");
            delete img;
            continue;
        }

        if (modflags & LF_MODIFY_TCA)
            g_print ("[tca]");
        if (modflags & LF_MODIFY_VIGNETTING)
            g_print ("[vign]");
        if (modflags & LF_MODIFY_CCI)
            g_print ("[cci]");
        if (modflags & LF_MODIFY_DISTORTION)
            g_print ("[dist]");
        if (modflags & LF_MODIFY_GEOMETRY)
            g_print ("[geom]");
        if (opts.Scale != 1.0)
            g_print ("[scale]");
        g_print (" ...");
#endif
}

void cleanup_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
#ifdef HAVE_GEGL
  #error "lensfun needs to be ported to GEGL!"
#else
  
        mod->Destroy ();
  free(piece->data);
#endif
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)self->gui_data;
  dt_iop_lensfun_params_t *p = (dt_iop_lensfun_params_t *)self->params;
  // gtk_range_set_value(GTK_RANGE(g->scale1), p->cx);
}

void init(dt_iop_module_t *module)
{
  // module->data = malloc(sizeof(dt_iop_lensfun_data_t));
  module->params = malloc(sizeof(dt_iop_lensfun_params_t));
  module->default_params = malloc(sizeof(dt_iop_lensfun_params_t));
  module->default_enabled = 0;
  module->params_size = sizeof(dt_iop_lensfun_params_t);
  module->gui_data = NULL;
  module->priority = 95;
  dt_iop_lensfun_params_t tmp = (dt_iop_lensfun_params_t){0.0, 0.0, 0.0, 1.0, 1.0};
  memcpy(module->params, &tmp, sizeof(dt_iop_lensfun_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_lensfun_params_t));

  
    lfDatabase *ldb = lf_db_new ();
    ldb->Load ();

    const lfLens **lenses = ldb->FindLenses (NULL, NULL, opts.Lens);
    if (!lenses)
    {
        g_print ("Cannot find the lens `%s' in database\n", opts.Lens);
        return -1;
    }

    const lfLens *lens = lenses [0];
    lf_free (lenses);

}

void cleanup(dt_iop_module_t *module)
{
  
    ldb->Destroy ();

  free(module->gui_data);
  module->gui_data = NULL;
  free(module->params);
  module->params = NULL;
}

void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_lensfun_gui_data_t));
  dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)self->gui_data;
  dt_iop_lensfun_params_t *p = (dt_iop_lensfun_params_t *)self->params;

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
    dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)self->gui_data;
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
    dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)self->gui_data;
    dt_iop_lensfun_params_t   *p = (dt_iop_lensfun_params_t   *)self->params;
    dt_dev_get_pointer_zoom_pos(self->dev, x, y, &g->button_down_zoom_x, &g->button_down_zoom_y);
    g->button_down_angle = p->angle;
    return 1;
  }
  else return 0;
}

