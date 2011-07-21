/*
		This file is part of darktable,
		copyright (c) 2009--2011 johannes hanika.
		copyright (c) 2011 henrik andersson.

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
#include <gdk/gdkkeysyms.h>
#include "iop/colorout.h"
#include "develop/develop.h"
#include "control/control.h"
#include "control/conf.h"
#include "gui/gtk.h"
#include "common/colorspaces.h"
#include "common/opencl.h"
#include "dtgtk/resetlabel.h"

DT_MODULE(2)

static gchar *_get_profile_from_pos(GList *profiles, int pos);

const char
*name()
{
  return _("output color profile");
}

int
groups ()
{
  return IOP_GROUP_COLOR;
}

static void key_softproof_callback(GtkAccelGroup *accel_group,
                                   GObject *acceleratable,
                                   guint keyval, GdkModifierType modifier,
                                   gpointer data)
{
  dt_iop_module_t* self = (dt_iop_module_t*)data;
  dt_iop_colorout_gui_data_t *g = (dt_iop_colorout_gui_data_t *)self->gui_data;
  dt_iop_colorout_params_t *p = (dt_iop_colorout_params_t *)self->params;

  /* toggle softproofing on/off */
  g->softproofing = !g->softproofing;
  if(g->softproofing)
  {
    int pos = gtk_combo_box_get_active(g->cbox5);
    gchar *filename = _get_profile_from_pos(g->profiles, pos);
    if (filename)
    {
      if (g->softproofprofile)
        g_free(g->softproofprofile);
      g->softproofprofile = g_strdup(filename);
    }
  }


  /// FIXME: this is certanly the wrong way to do this...
  p->seq++;
  dt_dev_add_history_item(darktable.develop, self, TRUE);
  dt_control_queue_draw_all();
}

int
legacy_params (dt_iop_module_t *self, const void *const old_params, const int old_version, void *new_params, const int new_version)
{
  if(old_version == 1 && new_version == 2)
  {
    dt_iop_colorout_params_t *o = (dt_iop_colorout_params_t *)old_params;
    dt_iop_colorout_params_t *n = (dt_iop_colorout_params_t *)new_params;
    memcpy(n,o,sizeof(dt_iop_colorout_params_t));
    n->seq = 0;
    return 0;
  }
  return 1;
}

void
init_global(dt_iop_module_so_t *module)
{
  const int program = 2; // basic.cl, from programs.conf
  dt_iop_colorout_global_data_t *gd = (dt_iop_colorout_global_data_t *)malloc(sizeof(dt_iop_colorout_global_data_t));
  module->data = gd;
  gd->kernel_colorout = dt_opencl_create_kernel(program, "colorout");
}

void
cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_colorout_global_data_t *gd = (dt_iop_colorout_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_colorout);
  free(module->data);
  module->data = NULL;
}

static void
intent_changed (GtkComboBox *widget, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_colorout_params_t *p = (dt_iop_colorout_params_t *)self->params;
  p->intent = (dt_iop_color_intent_t)gtk_combo_box_get_active(widget);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void
display_intent_changed (GtkComboBox *widget, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_colorout_params_t *p = (dt_iop_colorout_params_t *)self->params;
  p->displayintent = (dt_iop_color_intent_t)gtk_combo_box_get_active(widget);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static gchar *_get_profile_from_pos(GList *profiles, int pos)
{
  while(profiles)
  {
    dt_iop_color_profile_t *pp = (dt_iop_color_profile_t *)profiles->data;
    if(pp->pos == pos)
      return pp->filename;
    profiles = g_list_next(profiles);
  }
  return NULL;
}

static void
profile_changed (GtkComboBox *widget, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_colorout_params_t *p = (dt_iop_colorout_params_t *)self->params;
  dt_iop_colorout_gui_data_t *g = (dt_iop_colorout_gui_data_t *)self->gui_data;
  int pos = gtk_combo_box_get_active(widget);

  gchar *filename = _get_profile_from_pos(g->profiles, pos);
  if (filename)
  {
    g_strlcpy(p->iccprofile, filename, sizeof(p->iccprofile));
    dt_dev_add_history_item(darktable.develop, self, TRUE);
    return;
  }

  fprintf(stderr, "[colorout] color profile %s seems to have disappeared!\n", p->iccprofile);
}

static void
softproof_profile_changed (GtkComboBox *widget, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_colorout_params_t *p = (dt_iop_colorout_params_t *)self->params;
  dt_iop_colorout_gui_data_t *g = (dt_iop_colorout_gui_data_t *)self->gui_data;
  int pos = gtk_combo_box_get_active(widget);

  gchar *filename = _get_profile_from_pos(g->profiles, pos);
  if (filename)
  {
    if (g->softproofprofile)
      g_free(g->softproofprofile);
    g->softproofprofile = g_strdup(filename);
    if(g->softproofing)
    {
      p->seq++;
      dt_dev_add_history_item(darktable.develop, self, TRUE);
    }
    return;
  }
}

static void
display_profile_changed (GtkComboBox *widget, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_colorout_params_t *p = (dt_iop_colorout_params_t *)self->params;
  dt_iop_colorout_gui_data_t *g = (dt_iop_colorout_gui_data_t *)self->gui_data;
  int pos = gtk_combo_box_get_active(widget);
  gchar *filename = _get_profile_from_pos(g->profiles, pos);
  if (filename)
  {
    g_strlcpy(p->displayprofile, filename, sizeof(p->displayprofile));
    dt_dev_add_history_item(darktable.develop, self, TRUE);
    return;
  }

  // should really never happen.
  fprintf(stderr, "[colorout] display color profile %s seems to have disappeared!\n", p->displayprofile);
}

#if 1
static float
lerp_lut(const float *const lut, const float v)
{
  // TODO: check if optimization is worthwhile!
  const float ft = CLAMPS(v*(LUT_SAMPLES-1), 0, LUT_SAMPLES-1);
  const int t = ft < LUT_SAMPLES-2 ? ft : LUT_SAMPLES-2;
  const float f = ft - t;
  const float l1 = lut[t];
  const float l2 = lut[t+1];
  return l1*(1.0f-f) + l2*f;
}
#endif

#ifdef HAVE_OPENCL
int
process_cl (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_colorout_data_t *d = (dt_iop_colorout_data_t *)piece->data;
  dt_iop_colorout_global_data_t *gd = (dt_iop_colorout_global_data_t *)self->data;
  cl_mem dev_m = NULL, dev_r = NULL, dev_g = NULL, dev_b = NULL, dev_coeffs = NULL;

  cl_int err = -999;
  const int devid = piece->pipe->devid;
  size_t sizes[] = {roi_in->width, roi_in->height, 1};

  dev_m = dt_opencl_copy_host_to_device_constant(devid, sizeof(float)*9, d->cmatrix);
  if (dev_m == NULL) goto error;
  dev_r = dt_opencl_copy_host_to_device(devid, d->lut[0], 256, 256, sizeof(float));
  if (dev_r == NULL) goto error;
  dev_g = dt_opencl_copy_host_to_device(devid, d->lut[1], 256, 256, sizeof(float));
  if (dev_g == NULL) goto error;
  dev_b = dt_opencl_copy_host_to_device(devid, d->lut[2], 256, 256, sizeof(float));
  if (dev_g == NULL) goto error;
  dev_coeffs = dt_opencl_copy_host_to_device_constant(devid, sizeof(float)*2*3, (float *)d->unbounded_coeffs);
  if (dev_coeffs == NULL) goto error;
  dt_opencl_set_kernel_arg(devid, gd->kernel_colorout, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_colorout, 1, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(devid, gd->kernel_colorout, 2, sizeof(cl_mem), (void *)&dev_m);
  dt_opencl_set_kernel_arg(devid, gd->kernel_colorout, 3, sizeof(cl_mem), (void *)&dev_r);
  dt_opencl_set_kernel_arg(devid, gd->kernel_colorout, 4, sizeof(cl_mem), (void *)&dev_g);
  dt_opencl_set_kernel_arg(devid, gd->kernel_colorout, 5, sizeof(cl_mem), (void *)&dev_b);
  dt_opencl_set_kernel_arg(devid, gd->kernel_colorout, 6, sizeof(cl_mem), (void *)&dev_coeffs);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_colorout, sizes);
  if(err != CL_SUCCESS) goto error;
  dt_opencl_release_mem_object(dev_m);
  dt_opencl_release_mem_object(dev_r);
  dt_opencl_release_mem_object(dev_g);
  dt_opencl_release_mem_object(dev_b);
  dt_opencl_release_mem_object(dev_coeffs);
  return TRUE;

error:
  if (dev_m != NULL) dt_opencl_release_mem_object(dev_m);
  if (dev_r != NULL) dt_opencl_release_mem_object(dev_r);
  if (dev_g != NULL) dt_opencl_release_mem_object(dev_g);
  if (dev_b != NULL) dt_opencl_release_mem_object(dev_b);
  if (dev_coeffs != NULL) dt_opencl_release_mem_object(dev_coeffs);
  dt_print(DT_DEBUG_OPENCL, "[opencl_colorout] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif

void
process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *i, void *o, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  const dt_iop_colorout_data_t *const d = (dt_iop_colorout_data_t *)piece->data;
  float *in  = (float *)i;
  float *out = (float *)o;
  const int ch = piece->colors;

  if(d->cmatrix[0] != -0.666f)
  {
    //fprintf(stderr,"Using cmatrix codepath\n");
#ifdef _OPENMP
    #pragma omp parallel for schedule(static) default(none) shared(out, roi_out, in, i, o)
#endif
    for (int k=0; k<roi_out->width*roi_out->height; k++)
    {
      const float *const in = ((float *)i) + ch*k;
      float *const out = ((float *)o) + ch*k;
      float Lab[3], XYZ[3], rgb[3];
      Lab[0] = in[0];
      Lab[1] = in[1];
      Lab[2] = in[2];
      dt_Lab_to_XYZ(Lab, XYZ);
      for(int i=0; i<3; i++)
      {
        rgb[i] = 0.0f;
        for(int j=0; j<3; j++) rgb[i] += d->cmatrix[3*i+j]*XYZ[j];
      }
      for(int i=0; i<3; i++) out[i] = (d->lut[i][0] >= 0.0f) ?
        ((rgb[i] < 1.0f) ? lerp_lut(d->lut[i], rgb[i])
        : dt_iop_eval_exp(d->unbounded_coeffs[i], rgb[i]))
        : rgb[i];
    }
  }
  else
  {
    //fprintf(stderr,"Using xform codepath\n");

    // lcms2 fallback, slow:
    int rowsize=roi_out->width*3;

    // FIXME: breaks :(
#if 0//def _OPENMP
    #pragma omp parallel for schedule(static) default(none) shared(out, roi_out, in, d, rowsize)
#endif
    for (int k=0; k<roi_out->height; k++)
    {
      float Lab[rowsize];
      float rgb[rowsize];

      const int m=(k*(roi_out->width*ch));
      for (int l=0; l<roi_out->width; l++)
      {
        int li=3*l,ii=ch*l;
        Lab[li+0] = in[m+ii+0];
        Lab[li+1] = in[m+ii+1];
        Lab[li+2] = in[m+ii+2];
      }

      // lcms is not thread safe, so use local copy
      cmsDoTransform (d->xform[dt_get_thread_num()], Lab, rgb, roi_out->width);

      for (int l=0; l<roi_out->width; l++)
      {
        int oi=ch*l, ri=3*l;
        out[m+oi+0] = rgb[ri+0];
        out[m+oi+1] = rgb[ri+1];
        out[m+oi+2] = rgb[ri+2];
      }
    }
  }
}

static cmsHPROFILE _create_profile(gchar *iccprofile)
{
  cmsHPROFILE profile = NULL;
  if(!strcmp(iccprofile, "sRGB"))
  {
    // default: sRGB
    profile = dt_colorspaces_create_srgb_profile();
  }
  else if(!strcmp(iccprofile, "linear_rgb"))
  {
    profile = dt_colorspaces_create_linear_rgb_profile();
  }
  else if(!strcmp(iccprofile, "adobergb"))
  {
    profile = dt_colorspaces_create_adobergb_profile();
  }
  else if(!strcmp(iccprofile, "X profile"))
  {
    // x default
    if(darktable.control->xprofile_data)
      profile = cmsOpenProfileFromMem(darktable.control->xprofile_data, darktable.control->xprofile_size);
    else
      profile = NULL;
  }
  else
  {
    // else: load file name
    char filename[1024];
    dt_colorspaces_find_profile(filename, 1024, iccprofile, "out");
    profile = cmsOpenProfileFromFile(filename, "r");
  }

  /* if no match lets fallback to srgb profile */
  if (!profile)
    profile = dt_colorspaces_create_srgb_profile();

  return profile;
}

void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_colorout_params_t *p = (dt_iop_colorout_params_t *)p1;
  dt_iop_colorout_data_t *d = (dt_iop_colorout_data_t *)piece->data;
  gchar *overprofile = dt_conf_get_string("plugins/lighttable/export/iccprofile");
  const int overintent = dt_conf_get_int("plugins/lighttable/export/iccintent");
  const int high_quality_processing = dt_conf_get_bool("plugins/lighttable/export/force_lcms2");
  gchar *outprofile=NULL;
  int outintent = 0;
  dt_iop_colorout_gui_data_t *g=NULL;

  /* check if we should enable softproofing */
  if ((g=(dt_iop_colorout_gui_data_t *)self->gui_data) && g->softproofing && g->softproofprofile )
    d->softproofing = TRUE;
  else
    d->softproofing = FALSE;

  /* cleanup profiles */
  if (d->output)
    dt_colorspaces_cleanup_profile(d->output);
  d->output = NULL;

  if (d->softproof)
    dt_colorspaces_cleanup_profile(d->softproof);
  d->softproof = NULL;

  const int num_threads = dt_get_num_threads();
  for (int t=0; t<num_threads; t++)
    if (d->xform[t])
    {
      cmsDeleteTransform(d->xform[t]);
      d->xform[t] = NULL;
    }
  d->cmatrix[0] = -0.666f;
  d->lut[0][0] = -1.0f;
  d->lut[1][0] = -1.0f;
  d->lut[2][0] = -1.0f;
  piece->process_cl_ready = 1;

  /* if we are exporting then check and set usage of override profile */
  if (pipe->type == DT_DEV_PIXELPIPE_EXPORT)
  {
    if (overprofile && strcmp(overprofile, "image"))
      snprintf(p->iccprofile, DT_IOP_COLOR_ICC_LEN, "%s", overprofile);
    if (overintent >= 0)
      p->intent = overintent;

    outprofile = p->iccprofile;
    outintent = p->intent;
  }
  else
  {
    /* we are not exporting, using display profile as output */
    outprofile = p->displayprofile;
    outintent = p->displayintent;
  }

  /* creating output profile */
  d->output = _create_profile(outprofile);

  /* creating softproof profile if softproof is enabled */
  if (d->softproofing)
    d->softproof =  _create_profile(g->softproofprofile);

  /*
   * Setup transform flags
   */
  uint32_t transformFlags = 0;

  /* TODO: the use of bpc should be userconfigurable either from module or preference pane */
  /* softproof flag and black point compensation */
  transformFlags |= (d->softproofing ? cmsFLAGS_SOFTPROOFING|cmsFLAGS_BLACKPOINTCOMPENSATION : 0);
      


  /* get matrix from profile, if softproofing or high quality exporting always go xform codepath */
  if (d->softproofing || (pipe->type == DT_DEV_PIXELPIPE_EXPORT && high_quality_processing) || 
          dt_colorspaces_get_matrix_from_output_profile (d->output, d->cmatrix, d->lut[0], d->lut[1], d->lut[2], LUT_SAMPLES))
  {
    d->cmatrix[0] = -0.666f;
    piece->process_cl_ready = 0;
    for(int t=0; t<num_threads; t++)
      d->xform[t] = cmsCreateProofingTransform(
                      d->Lab,TYPE_Lab_FLT, d->output, TYPE_RGB_FLT,
                      d->softproof, outintent,
                      INTENT_RELATIVE_COLORIMETRIC,
                      transformFlags);
  }

  // user selected a non-supported output profile, check that:
  if (!d->xform[0] && d->cmatrix[0] == -0.666f)
  {
    dt_control_log(_("unsupported output profile has been replaced by sRGB!"));
    if (d->output)
      dt_colorspaces_cleanup_profile(d->output);
    d->output = dt_colorspaces_create_srgb_profile();
    if (d->softproofing || dt_colorspaces_get_matrix_from_output_profile (d->output, d->cmatrix, d->lut[0], d->lut[1], d->lut[2], LUT_SAMPLES))
    {
      d->cmatrix[0] = -0.666f;
      piece->process_cl_ready = 0;
      for (int t=0; t<num_threads; t++)
	
        d->xform[t] = cmsCreateProofingTransform(
                        d->Lab,TYPE_Lab_FLT, d->output, TYPE_RGB_FLT,
                        d->softproof, outintent,
                        INTENT_RELATIVE_COLORIMETRIC,
			transformFlags);
    }
  }

  // now try to initialize unbounded mode:
  // we do extrapolation for input values above 1.0f.
  // unfortunately we can only do this if we got the computation
  // in our hands, i.e. for the fast builtin-dt-matrix-profile path.
  for(int k=0;k<3;k++)
  {
    // omit luts marked as linear (negative as marker)
    if(d->lut[k][0] >= 0.0f)
    {
      const float x[4] = {0.7f, 0.8f, 0.9f, 1.0f};
      const float y[4] = {lerp_lut(d->lut[k], x[0]),
                          lerp_lut(d->lut[k], x[1]),
                          lerp_lut(d->lut[k], x[2]),
                          lerp_lut(d->lut[k], x[3])};
      dt_iop_estimate_exp(x, y, 4, d->unbounded_coeffs[k]);
    }
    else d->unbounded_coeffs[k][0] = -1.0f;
  }

  //fprintf(stderr, " Output profile %s, softproof %s%s%s\n", outprofile, d->softproofing?"enabled ":"disabled",d->softproofing?"using profile ":"",d->softproofing?g->softproofprofile:"");

  g_free(overprofile);
}

void init_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
#ifdef HAVE_GEGL
  // create part of the gegl pipeline
#else
  piece->data = malloc(sizeof(dt_iop_colorout_data_t));
  dt_iop_colorout_data_t *d = (dt_iop_colorout_data_t *)piece->data;
  d->softproof = d->output = NULL;
  d->xform = (cmsHTRANSFORM *)malloc(sizeof(cmsHTRANSFORM)*dt_get_num_threads());
  for(int t=0; t<dt_get_num_threads(); t++)
    d->xform[t] = NULL;
  d->Lab = dt_colorspaces_create_lab_profile();
  self->commit_params(self, self->default_params, pipe, piece);
#endif
}

void cleanup_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
#ifdef HAVE_GEGL
  // clean up everything again.
  // (void)gegl_node_remove_child(pipe->gegl, piece->input);
#else
  dt_iop_colorout_data_t *d = (dt_iop_colorout_data_t *)piece->data;
  if(d->output) dt_colorspaces_cleanup_profile(d->output);
  dt_colorspaces_cleanup_profile(d->Lab);
  for(int t=0; t<dt_get_num_threads(); t++)
    if(d->xform[t])
      cmsDeleteTransform(d->xform[t]);

  free(d->xform);
  free(piece->data);
#endif
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_module_t *module = (dt_iop_module_t *)self;
  dt_iop_colorout_gui_data_t *g = (dt_iop_colorout_gui_data_t *)self->gui_data;
  dt_iop_colorout_params_t *p = (dt_iop_colorout_params_t *)module->params;
  gtk_combo_box_set_active(g->cbox1, (int)p->intent);
  gtk_combo_box_set_active(g->cbox4, (int)p->displayintent);
  gtk_combo_box_set_active(g->cbox5, 0);
  int iccfound = 0, displayfound = 0;
  GList *prof = g->profiles;
  while(prof)
  {
    dt_iop_color_profile_t *pp = (dt_iop_color_profile_t *)prof->data;
    if(!strcmp(pp->filename, p->iccprofile))
    {
      gtk_combo_box_set_active(g->cbox2, pp->pos);
      iccfound = 1;
    }
    if(!strcmp(pp->filename, p->displayprofile))
    {
      gtk_combo_box_set_active(g->cbox3, pp->pos);
      displayfound = 1;
    }
    if(iccfound && displayfound) break;
    prof = g_list_next(prof);
  }
  if(!iccfound)     gtk_combo_box_set_active(g->cbox2, 0);
  if(!displayfound) gtk_combo_box_set_active(g->cbox3, 0);
  if(!iccfound)     fprintf(stderr, "[colorout] could not find requested profile `%s'!\n", p->iccprofile);
  if(!displayfound) fprintf(stderr, "[colorout] could not find requested display profile `%s'!\n", p->displayprofile);
}

void init(dt_iop_module_t *module)
{
  module->params = malloc(sizeof(dt_iop_colorout_params_t));
  module->default_params = malloc(sizeof(dt_iop_colorout_params_t));
  module->params_size = sizeof(dt_iop_colorout_params_t);
  module->gui_data = NULL;
  module->priority = 777; // module order created by iop_dependencies.py, do not edit!
  module->hide_enable_button = 1;
  dt_iop_colorout_params_t tmp = (dt_iop_colorout_params_t)
  {"sRGB", "X profile", DT_INTENT_PERCEPTUAL
  };
  memcpy(module->params, &tmp, sizeof(dt_iop_colorout_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_colorout_params_t));
}

void cleanup(dt_iop_module_t *module)
{
  free(module->gui_data);
  module->gui_data = NULL;
  free(module->params);
  module->params = NULL;
}

void gui_post_expose (struct dt_iop_module_t *self, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx, int32_t pointery)
{
  dt_iop_colorout_gui_data_t *g = (dt_iop_colorout_gui_data_t *)self->gui_data;
  if(g->softproofing)
  {
    gchar *label=_("SoftProof");
    cairo_set_source_rgba(cr,0.5,0.5,0.5,0.5);
    cairo_text_extents_t te;
    cairo_select_font_face (cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size (cr, 20);
    cairo_text_extents (cr, label, &te);
    cairo_move_to (cr, te.height*2, height-(te.height*2));
    cairo_text_path (cr, _("SoftProof"));
    cairo_set_source_rgb(cr, 0.7, 0.7, 0.7);
    cairo_fill_preserve(cr);
    cairo_set_line_width(cr, 0.7);
    cairo_set_source_rgb(cr, 0.3, 0.3, 0.3);
    cairo_stroke(cr);
  }
}

void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_colorout_gui_data_t));
  memset(self->gui_data,0,sizeof(dt_iop_colorout_gui_data_t));
  dt_iop_colorout_gui_data_t *g = (dt_iop_colorout_gui_data_t *)self->gui_data;
  dt_iop_colorout_params_t *p = (dt_iop_colorout_params_t *)self->params;

  g->profiles = NULL;
  dt_iop_color_profile_t *prof = (dt_iop_color_profile_t *)g_malloc0(sizeof(dt_iop_color_profile_t));
  g_strlcpy(prof->filename, "sRGB", sizeof(prof->filename));
  g_strlcpy(prof->name, "sRGB", sizeof(prof->name));
  int pos;
  prof->pos = 0;
  g->profiles = g_list_append(g->profiles, prof);

  prof = (dt_iop_color_profile_t *)g_malloc0(sizeof(dt_iop_color_profile_t));
  g_strlcpy(prof->filename, "adobergb", sizeof(prof->filename));
  g_strlcpy(prof->name, "adobergb", sizeof(prof->name));
  prof->pos = 1;
  g->profiles = g_list_append(g->profiles, prof);

  prof = (dt_iop_color_profile_t *)g_malloc0(sizeof(dt_iop_color_profile_t));
  g_strlcpy(prof->filename, "X profile", sizeof(prof->filename));
  g_strlcpy(prof->name, "X profile", sizeof(prof->name));
  prof->pos = 2;
  g->profiles = g_list_append(g->profiles, prof);

  prof = (dt_iop_color_profile_t *)g_malloc0(sizeof(dt_iop_color_profile_t));
  g_strlcpy(prof->filename, "linear_rgb", sizeof(prof->filename));
  g_strlcpy(prof->name, "linear_rgb", sizeof(prof->name));
  pos = prof->pos = 3;
  g->profiles = g_list_append(g->profiles, prof);

  // read {conf,data}dir/color/out/*.icc
  char datadir[1024], confdir[1024], dirname[1024], filename[1024];
  dt_get_user_config_dir(confdir, 1024);
  dt_get_datadir(datadir, 1024);
  snprintf(dirname, 1024, "%s/color/out", confdir);
  if(!g_file_test(dirname, G_FILE_TEST_IS_DIR))
    snprintf(dirname, 1024, "%s/color/out", datadir);
  cmsHPROFILE tmpprof;
  const gchar *d_name;
  GDir *dir = g_dir_open(dirname, 0, NULL);
  if(dir)
  {
    while((d_name = g_dir_read_name(dir)))
    {
      snprintf(filename, 1024, "%s/%s", dirname, d_name);
      tmpprof = cmsOpenProfileFromFile(filename, "r");
      if(tmpprof)
      {
	char *lang = getenv("LANG");
	if (!lang) lang = "en_US";

        dt_iop_color_profile_t *prof = (dt_iop_color_profile_t *)g_malloc0(sizeof(dt_iop_color_profile_t));
        char name[1024];
        cmsGetProfileInfoASCII(tmpprof, cmsInfoDescription, lang, lang+3, name, 1024);
        g_strlcpy(prof->name, name, sizeof(prof->name));
        g_strlcpy(prof->filename, d_name, sizeof(prof->filename));
        prof->pos = ++pos;
        cmsCloseProfile(tmpprof);
        g->profiles = g_list_append(g->profiles, prof);
      }
    }
    g_dir_close(dir);
  }

  self->widget = GTK_WIDGET(gtk_hbox_new(FALSE, 0));
  g->vbox1 = GTK_VBOX(gtk_vbox_new(FALSE, DT_GUI_IOP_MODULE_CONTROL_SPACING));
  g->vbox2 = GTK_VBOX(gtk_vbox_new(FALSE, DT_GUI_IOP_MODULE_CONTROL_SPACING));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->vbox1), FALSE, FALSE, 5);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->vbox2), TRUE, TRUE, 5);

  GtkWidget *label1, *label2, *label3, *label4, *label5;
  label1 = dtgtk_reset_label_new(_("output intent"), self, &p->intent, sizeof(dt_iop_color_intent_t));
  label2 = dtgtk_reset_label_new(_("output profile"), self, &p->iccprofile, sizeof(char)*DT_IOP_COLOR_ICC_LEN);
  label5 = gtk_label_new(_("softproof profile"));
  gtk_misc_set_alignment(GTK_MISC(label5), 0.0, 0.5);
  label4 = dtgtk_reset_label_new(_("display intent"), self, &p->displayintent, sizeof(dt_iop_color_intent_t));
  label3 = dtgtk_reset_label_new(_("display profile"), self, &p->displayprofile, sizeof(char)*DT_IOP_COLOR_ICC_LEN);
  gtk_box_pack_start(GTK_BOX(g->vbox1), label1, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox1), label2, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox1), label5, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox1), label4, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox1), label3, TRUE, TRUE, 0);
  g->cbox1 = GTK_COMBO_BOX(gtk_combo_box_new_text());
  gtk_combo_box_append_text(g->cbox1, _("perceptual"));
  gtk_combo_box_append_text(g->cbox1, _("relative colorimetric"));
  gtk_combo_box_append_text(g->cbox1, C_("rendering intent", "saturation"));
  gtk_combo_box_append_text(g->cbox1, _("absolute colorimetric"));
  g->cbox4 = GTK_COMBO_BOX(gtk_combo_box_new_text());
  gtk_combo_box_append_text(g->cbox4, _("perceptual"));
  gtk_combo_box_append_text(g->cbox4, _("relative colorimetric"));
  gtk_combo_box_append_text(g->cbox4, C_("rendering intent", "saturation"));
  gtk_combo_box_append_text(g->cbox4, _("absolute colorimetric"));
  g->cbox2 = GTK_COMBO_BOX(gtk_combo_box_new_text());
  g->cbox3 = GTK_COMBO_BOX(gtk_combo_box_new_text());
  g->cbox5 = GTK_COMBO_BOX(gtk_combo_box_new_text());
  GList *l = g->profiles;
  while(l)
  {
    dt_iop_color_profile_t *prof = (dt_iop_color_profile_t *)l->data;
    if(!strcmp(prof->name, "X profile"))
    {
      gtk_combo_box_append_text(g->cbox2, _("system display profile"));
      gtk_combo_box_append_text(g->cbox3, _("system display profile"));
      gtk_combo_box_append_text(g->cbox5, _("system display profile"));	/// TODO: this is useless, but here for test
    }
    else if(!strcmp(prof->name, "linear_rgb"))
    {
      gtk_combo_box_append_text(g->cbox2, _("linear RGB"));
      gtk_combo_box_append_text(g->cbox3, _("linear RGB"));
      gtk_combo_box_append_text(g->cbox5, _("linear RGB"));
    }
    else if(!strcmp(prof->name, "sRGB"))
    {
      gtk_combo_box_append_text(g->cbox2, _("sRGB (web-safe)"));
      gtk_combo_box_append_text(g->cbox3, _("sRGB (web-safe)"));
      gtk_combo_box_append_text(g->cbox5, _("sRGB (web-safe)"));
    }
    else if(!strcmp(prof->name, "adobergb"))
    {
      gtk_combo_box_append_text(g->cbox2, _("Adobe RGB"));
      gtk_combo_box_append_text(g->cbox3, _("Adobe RGB"));
      gtk_combo_box_append_text(g->cbox5, _("Adobe RGB"));
    }
    else
    {
      gtk_combo_box_append_text(g->cbox2, prof->name);
      gtk_combo_box_append_text(g->cbox3, prof->name);
      gtk_combo_box_append_text(g->cbox5, prof->name);
    }
    l = g_list_next(l);
  }

  gtk_combo_box_set_active(g->cbox1, 0);
  gtk_combo_box_set_active(g->cbox2, 0);
  gtk_combo_box_set_active(g->cbox3, 0);
  gtk_combo_box_set_active(g->cbox4, 0);
  gtk_combo_box_set_active(g->cbox5, 0);	// Defaults softproofing against srgb profile
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->cbox1), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->cbox2), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->cbox5), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->cbox4), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->cbox3), TRUE, TRUE, 0);

  char tooltip[1024];
  g_object_set(G_OBJECT(g->cbox1), "tooltip-text", _("rendering intent"), (char *)NULL);
  snprintf(tooltip, 1024, _("icc profiles in %s/color/out or %s/color/out"), confdir, datadir);
  g_object_set(G_OBJECT(g->cbox2), "tooltip-text", tooltip, (char *)NULL);
  snprintf(tooltip, 1024, _("display icc profiles in %s/color/out or %s/color/out"), confdir, datadir);
  g_object_set(G_OBJECT(g->cbox3), "tooltip-text", tooltip, (char *)NULL);
  snprintf(tooltip, 1024, _("softproof icc profiles in %s/color/out or %s/color/out"), confdir, datadir);
  g_object_set(G_OBJECT(g->cbox5), "tooltip-text", tooltip, (char *)NULL);

  g_signal_connect (G_OBJECT (g->cbox1), "changed",
                    G_CALLBACK (intent_changed),
                    (gpointer)self);
  g_signal_connect (G_OBJECT (g->cbox4), "changed",
                    G_CALLBACK (display_intent_changed),
                    (gpointer)self);
  g_signal_connect (G_OBJECT (g->cbox2), "changed",
                    G_CALLBACK (profile_changed),
                    (gpointer)self);
  g_signal_connect (G_OBJECT (g->cbox3), "changed",
                    G_CALLBACK (display_profile_changed),
                    (gpointer)self);
  g_signal_connect (G_OBJECT (g->cbox5), "changed",
                    G_CALLBACK (softproof_profile_changed),
                    (gpointer)self);


  // Connecting the accelerator
  g->softproof_callback = g_cclosure_new(G_CALLBACK(key_softproof_callback),
                                         (gpointer)self, NULL);
  dt_accel_group_connect_by_path(
      darktable.control->accels_darkroom,
      "<Darktable>/darkroom/plugins/colorout/toggle softproofing",
      g->softproof_callback);

}

void gui_cleanup(struct dt_iop_module_t *self)
{
  dt_iop_colorout_gui_data_t *g = (dt_iop_colorout_gui_data_t *)self->gui_data;
  while(g->profiles)
  {
    g_free(g->profiles->data);
    g->profiles = g_list_delete_link(g->profiles, g->profiles);
  }
  dt_accel_group_disconnect(darktable.control->accels_darkroom,
                             ((dt_iop_colorout_gui_data_t*)(self->gui_data))->
                             softproof_callback);
  free(self->gui_data);
  self->gui_data = NULL;
}

void init_key_accels()
{
  gtk_accel_map_add_entry("<Darktable>/darkroom/plugins/colorout/toggle softproofing",
                          GDK_s, 0);

  dt_accel_group_connect_by_path(
      darktable.control->accels_darkroom,
      "<Darktable>/darkroom/plugins/colorout/toggle softproofing",
      NULL);
}

// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
