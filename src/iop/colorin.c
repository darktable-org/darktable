/*
    This file is part of darktable,
    copyright (c) 2009--2011 johannes hanika.
    copyright (c) 2011 henrik andersson

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
#include "iop/colorin.h"
#include "develop/develop.h"
#include "control/control.h"
#include "gui/gtk.h"
#include "common/colorspaces.h"
#include "common/colormatrices.c"
#include "common/opencl.h"
#include "dtgtk/resetlabel.h"
#include "external/adobe_coeff.c"

DT_MODULE(1)

typedef struct dt_iop_colorin_global_data_t
{
  int kernel_colorin;
}
dt_iop_colorin_global_data_t;


const char *
name()
{
  return _("input color profile");
}

int
groups ()
{
  return IOP_GROUP_COLOR;
}

void
init_global(dt_iop_module_so_t *module)
{
  const int program = 2; // basic.cl, from programs.conf
  dt_iop_colorin_global_data_t *gd = (dt_iop_colorin_global_data_t *)malloc(sizeof(dt_iop_colorin_global_data_t));
  module->data = gd;
  gd->kernel_colorin = dt_opencl_create_kernel(program, "colorin");
}

void
cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_colorin_global_data_t *gd = (dt_iop_colorin_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_colorin);
  free(module->data);
  module->data = NULL;
}

#if 0
static void intent_changed (GtkComboBox *widget, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_colorin_params_t *p = (dt_iop_colorin_params_t *)self->params;
  p->intent = (dt_iop_color_intent_t)gtk_combo_box_get_active(widget);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}
#endif

static void
profile_changed (GtkComboBox *widget, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  self->dev->gui_module = self;
  dt_iop_colorin_params_t *p = (dt_iop_colorin_params_t *)self->params;
  dt_iop_colorin_gui_data_t *g = (dt_iop_colorin_gui_data_t *)self->gui_data;
  int pos = gtk_combo_box_get_active(widget);
  GList *prof = g->profiles;
  while(prof)
  {
    // could use g_list_nth. this seems safer?
    dt_iop_color_profile_t *pp = (dt_iop_color_profile_t *)prof->data;
    if(pp->pos == pos)
    {
      g_strlcpy(p->iccprofile, pp->filename, sizeof(p->iccprofile));
      dt_dev_add_history_item(darktable.develop, self, TRUE);
      return;
    }
    prof = g_list_next(prof);
  }
  // should really never happen.
  fprintf(stderr, "[colorin] color profile %s seems to have disappeared!\n", p->iccprofile);
}

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

#ifdef HAVE_OPENCL
int
process_cl (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_colorin_data_t *d = (dt_iop_colorin_data_t *)piece->data;
  dt_iop_colorin_global_data_t *gd = (dt_iop_colorin_global_data_t *)self->data;
  cl_mem dev_m = NULL, dev_r = NULL, dev_g = NULL, dev_b = NULL, dev_coeffs = NULL;

  cl_int err = -999;
  const int map_blues = self->dev->image->flags & DT_IMAGE_RAW;
  const int devid = piece->pipe->devid;
  size_t sizes[] = {roi_in->width, roi_in->height, 1};
  dev_m = dt_opencl_copy_host_to_device_constant(devid, sizeof(float)*9, d->cmatrix);
  if (dev_m == NULL) goto error;
  dev_r = dt_opencl_copy_host_to_device(devid, d->lut[0], 256, 256, sizeof(float));
  if (dev_r == NULL) goto error;
  dev_g = dt_opencl_copy_host_to_device(devid, d->lut[1], 256, 256, sizeof(float));
  if (dev_g == NULL) goto error;
  dev_b = dt_opencl_copy_host_to_device(devid, d->lut[2], 256, 256, sizeof(float));
  if (dev_b == NULL) goto error;
  dev_coeffs = dt_opencl_copy_host_to_device_constant(devid, sizeof(float)*2*3, (float *)d->unbounded_coeffs);
  if (dev_coeffs == NULL) goto error;
  dt_opencl_set_kernel_arg(devid, gd->kernel_colorin, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_colorin, 1, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(devid, gd->kernel_colorin, 2, sizeof(cl_mem), (void *)&dev_m);
  dt_opencl_set_kernel_arg(devid, gd->kernel_colorin, 3, sizeof(cl_mem), (void *)&dev_r);
  dt_opencl_set_kernel_arg(devid, gd->kernel_colorin, 4, sizeof(cl_mem), (void *)&dev_g);
  dt_opencl_set_kernel_arg(devid, gd->kernel_colorin, 5, sizeof(cl_mem), (void *)&dev_b);
  dt_opencl_set_kernel_arg(devid, gd->kernel_colorin, 6, sizeof(cl_int), (void *)&map_blues);
  dt_opencl_set_kernel_arg(devid, gd->kernel_colorin, 7, sizeof(cl_mem), (void *)&dev_coeffs);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_colorin, sizes);
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
  dt_print(DT_DEBUG_OPENCL, "[opencl_colorin] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif


void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *i, void *o, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  const dt_iop_colorin_data_t *const d = (dt_iop_colorin_data_t *)piece->data;
  const float *const mat = d->cmatrix;
  float *in  = (float *)i;
  float *out = (float *)o;
  const int ch = piece->colors;
  const int map_blues = self->dev->image->flags & DT_IMAGE_RAW;

  if(mat[0] != -666.0f)
  {
    // only color matrix. use our optimized fast path!
#ifdef _OPENMP
    #pragma omp parallel for default(none) shared(roi_out, out, in) schedule(static)
#endif
    for(int k=0; k<roi_out->width*roi_out->height; k++)
    {
      const float *const buf_in  = in  + ch*k;
      float *const buf_out = out + ch*k;
      float cam[3], XYZ[3], Lab[3];
      // memcpy(cam, buf_in, sizeof(float)*3);
      // avoid calling this for linear profiles (marked with negative entries), assures unbounded
      // color management without extrapolation.
      for(int i=0; i<3; i++) cam[i] = (d->lut[i][0] >= 0.0f) ?
        ((buf_in[i] < 1.0f) ? lerp_lut(d->lut[i], buf_in[i])
        : dt_iop_eval_exp(d->unbounded_coeffs[i], buf_in[i]))
        : buf_in[i];

      if(map_blues)
      {
        // manual gamut mapping. these values cause trouble when converting back from Lab to sRGB.
        // deeply saturated blues turn into purple fringes, so dampen them before conversion.
        // this is off for non-raw images, which don't seem to have this problem.
        // might be caused by too loose clipping bounds during highlight clipping?
        const float YY = cam[0]+cam[1]+cam[2];
        const float zz = cam[2]/YY;
        // lower amount and higher bound_z make the effect smaller.
        // the effect is weakened the darker input values are, saturating at bound_Y
        const float bound_z = 0.5f, bound_Y = 0.8f;
        const float amount = 0.11f;
        if (zz > bound_z)
        {
          const float t = (zz - bound_z)/(1.0f-bound_z) * fminf(1.0, YY/bound_Y);
          cam[1] += t*amount;
          cam[2] -= t*amount;
        }
      }
      // now convert camera to XYZ using the color matrix
      for(int j=0; j<3; j++)
      {
        XYZ[j] = 0.0f;
        for(int i=0; i<3; i++) XYZ[j] += mat[3*j+i] * cam[i];
      }
      dt_XYZ_to_Lab(XYZ, Lab);
      memcpy(buf_out, Lab, sizeof(float)*3);
    }
  }
  else
  {
    // use general lcms2 fallback
    int rowsize=roi_out->width*3;
    float cam[rowsize];
    float Lab[rowsize];

    // FIXME: for some unapparent reason even this breaks lcms2 :(
#if 0//def _OPENMP
    #pragma omp parallel for default(none) shared(roi_out, out, in, d, cam, Lab, rowsize) schedule(static)
#endif
    for(int k=0; k<roi_out->height; k++)
    {
      const int m=(k*(roi_out->width*ch));

      for (int l=0; l<roi_out->width; l++)
      {
        int ci=3*l, ii=ch*l;

        cam[ci+0] = in[m+ii+0];
        cam[ci+1] = in[m+ii+1];
        cam[ci+2] = in[m+ii+2];

        const float YY = cam[ci+0]+cam[ci+1]+cam[ci+2];
        const float zz = cam[ci+2]/YY;
        const float bound_z = 0.5f, bound_Y = 0.5f;
        const float amount = 0.11f;
        if (zz > bound_z)
        {
          const float t = (zz - bound_z)/(1.0f-bound_z) * fminf(1.0, YY/bound_Y);
          cam[ci+1] += t*amount;
          cam[ci+2] -= t*amount;
        }
      }
      // convert to (L,a/L,b/L) to be able to change L without changing saturation.
      // lcms is not thread safe, so work on one copy for each thread :(
      cmsDoTransform (d->xform[dt_get_thread_num()], cam, Lab, roi_out->width);

      for (int l=0; l<roi_out->width; l++)
      {
        int li=3*l, oi=ch*l;
        out[m+oi+0] = Lab[li+0];
        out[m+oi+1] = Lab[li+1];
        out[m+oi+2] = Lab[li+2];
      }
    }
  }
}

void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_colorin_params_t *p = (dt_iop_colorin_params_t *)p1;
  dt_iop_colorin_data_t *d = (dt_iop_colorin_data_t *)piece->data;
  if(d->input) cmsCloseProfile(d->input);
  const int num_threads = dt_get_num_threads();
  d->input = NULL;
  for(int t=0; t<num_threads; t++) if(d->xform[t])
    {
      cmsDeleteTransform(d->xform[t]);
      d->xform[t] = NULL;
    }
  d->cmatrix[0] = -666.0f;
  d->lut[0][0] = -1.0f;
  d->lut[1][0] = -1.0f;
  d->lut[2][0] = -1.0f;
  piece->process_cl_ready = 1;
  char datadir[1024];
  char filename[1024];
  dt_get_datadir(datadir, 1024);
  if(!strcmp(p->iccprofile, "darktable"))
  {
    char makermodel[1024];
    dt_colorspaces_get_makermodel(makermodel, 1024, self->dev->image->exif_maker, self->dev->image->exif_model);
    d->input = dt_colorspaces_create_darktable_profile(makermodel);
    if(!d->input) sprintf(p->iccprofile, "cmatrix");
  }
  if(!strcmp(p->iccprofile, "cmatrix"))
  {
    // color matrix
    dt_image_full_path(self->dev->image->id, filename, 1024);
    char makermodel[1024];
    dt_colorspaces_get_makermodel(makermodel, 1024, self->dev->image->exif_maker, self->dev->image->exif_model);
    float cam_xyz[12];
    dt_dcraw_adobe_coeff(makermodel, "", (float (*)[12])cam_xyz);
    d->input = dt_colorspaces_create_xyzimatrix_profile((float (*)[3])cam_xyz);
  }
  else if(!strcmp(p->iccprofile, "sRGB"))
  {
    d->input = dt_colorspaces_create_srgb_profile();
  }
  else if(!strcmp(p->iccprofile, "infrared"))
  {
    d->input = dt_colorspaces_create_linear_infrared_profile();
  }
  else if(!strcmp(p->iccprofile, "XYZ"))
  {
    d->input = dt_colorspaces_create_xyz_profile();
  }
  else if(!strcmp(p->iccprofile, "adobergb"))
  {
    d->input = dt_colorspaces_create_adobergb_profile();
  }
  else if(!strcmp(p->iccprofile, "linear_rgb"))
  {
    d->input = dt_colorspaces_create_linear_rgb_profile();
  }
  else if(!d->input)
  {
    dt_colorspaces_find_profile(filename, 1024, p->iccprofile, "in");
    d->input = cmsOpenProfileFromFile(filename, "r");
  }
  if(d->input)
  {
    if(dt_colorspaces_get_matrix_from_input_profile (d->input, d->cmatrix, d->lut[0], d->lut[1], d->lut[2], LUT_SAMPLES))
    {
      piece->process_cl_ready = 0;
      d->cmatrix[0] = -666.0f;
      for(int t=0; t<num_threads; t++) d->xform[t] = cmsCreateTransform(d->input, TYPE_RGB_FLT, d->Lab, TYPE_Lab_FLT, p->intent, 0);
    }
  }
  else
  {
    if(strcmp(p->iccprofile, "sRGB"))
    {
      // use linear_rgb as fallback for missing profiles:
      d->input = dt_colorspaces_create_linear_rgb_profile();
    }
    if(!d->input) d->input = dt_colorspaces_create_srgb_profile();
    if(dt_colorspaces_get_matrix_from_input_profile (d->input, d->cmatrix, d->lut[0], d->lut[1], d->lut[2], LUT_SAMPLES))
    {
      piece->process_cl_ready = 0;
      d->cmatrix[0] = -666.0f;
      for(int t=0; t<num_threads; t++) d->xform[t] = cmsCreateTransform(d->input, TYPE_RGB_FLT, d->Lab, TYPE_Lab_FLT, p->intent, 0);
    }
  }
  // user selected a non-supported output profile, check that:
  if(!d->xform[0] && d->cmatrix[0] == -666.0f)
  {
    dt_control_log(_("unsupported input profile has been replaced by linear rgb!"));
    if(d->input) dt_colorspaces_cleanup_profile(d->input);
    d->input = dt_colorspaces_create_linear_rgb_profile();
    if(dt_colorspaces_get_matrix_from_input_profile (d->input, d->cmatrix, d->lut[0], d->lut[1], d->lut[2], LUT_SAMPLES))
    {
      piece->process_cl_ready = 0;
      d->cmatrix[0] = -666.0f;
      for(int t=0; t<num_threads; t++) d->xform[t] = cmsCreateTransform(d->Lab, TYPE_RGB_FLT, d->input, TYPE_Lab_FLT, p->intent, 0);
    }
  }
  
  // now try to initialize unbounded mode:
  // we do a extrapolation for input values above 1.0f.
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
}

void init_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_colorin_data_t));
  dt_iop_colorin_data_t *d = (dt_iop_colorin_data_t *)piece->data;
  d->input = NULL;
  d->xform = (cmsHTRANSFORM *)malloc(sizeof(cmsHTRANSFORM)*dt_get_num_threads());
  for(int t=0; t<dt_get_num_threads(); t++) d->xform[t] = NULL;
  d->Lab = dt_colorspaces_create_lab_profile();
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
#ifdef HAVE_GEGL
  // clean up everything again.
  // (void)gegl_node_remove_child(pipe->gegl, piece->input);
#else
  // pthread_mutex_lock(&darktable.plugin_threadsafe);
  dt_iop_colorin_data_t *d = (dt_iop_colorin_data_t *)piece->data;
  if(d->input) dt_colorspaces_cleanup_profile(d->input);
  dt_colorspaces_cleanup_profile(d->Lab);
  for(int t=0; t<dt_get_num_threads(); t++) if(d->xform[t]) cmsDeleteTransform(d->xform[t]);
  free(d->xform);
  free(piece->data);
  // pthread_mutex_unlock(&darktable.plugin_threadsafe);
#endif
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_module_t *module = (dt_iop_module_t *)self;
  dt_iop_colorin_gui_data_t *g = (dt_iop_colorin_gui_data_t *)self->gui_data;
  dt_iop_colorin_params_t *p = (dt_iop_colorin_params_t *)module->params;
  // gtk_combo_box_set_active(g->cbox1, (int)p->intent);
  GList *prof = g->profiles;
  while(prof)
  {
    dt_iop_color_profile_t *pp = (dt_iop_color_profile_t *)prof->data;
    if(!strcmp(pp->filename, p->iccprofile))
    {
      gtk_combo_box_set_active(g->cbox2, pp->pos);
      return;
    }
    prof = g_list_next(prof);
  }
  gtk_combo_box_set_active(g->cbox2, 0);
  if(strcmp(p->iccprofile, "darktable")) fprintf(stderr, "[colorin] could not find requested profile `%s'!\n", p->iccprofile);
}

void reload_defaults(dt_iop_module_t *module)
{
  dt_iop_colorin_params_t tmp = (dt_iop_colorin_params_t)
  {
    "darktable", DT_INTENT_PERCEPTUAL
  };
  if(dt_image_is_ldr(module->dev->image)) g_strlcpy(tmp.iccprofile, "sRGB", sizeof(tmp.iccprofile));
  memcpy(module->params, &tmp, sizeof(dt_iop_colorin_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_colorin_params_t));
}

void init(dt_iop_module_t *module)
{
  // module->data = malloc(sizeof(dt_iop_colorin_data_t));
  module->params = malloc(sizeof(dt_iop_colorin_params_t));
  module->default_params = malloc(sizeof(dt_iop_colorin_params_t));
  module->params_size = sizeof(dt_iop_colorin_params_t);
  module->gui_data = NULL;
  module->priority = 333; // module order created by iop_dependencies.py, do not edit!
  module->hide_enable_button = 1;
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
  // pthread_mutex_lock(&darktable.plugin_threadsafe);
  self->gui_data = malloc(sizeof(dt_iop_colorin_gui_data_t));
  dt_iop_colorin_gui_data_t *g = (dt_iop_colorin_gui_data_t *)self->gui_data;
  dt_iop_colorin_params_t *p   = (dt_iop_colorin_params_t *)self->params;

  g->profiles = NULL;

  // get color matrix from raw image:
  dt_iop_color_profile_t *prof = (dt_iop_color_profile_t *)g_malloc0(sizeof(dt_iop_color_profile_t));
  g_strlcpy(prof->filename, "cmatrix", sizeof(prof->filename));
  g_strlcpy(prof->name, "cmatrix", sizeof(prof->name));
  g->profiles = g_list_append(g->profiles, prof);
  int pos = prof->pos = 0;

  char makermodel[1024];
  dt_colorspaces_get_makermodel(makermodel, 1024, self->dev->image->exif_maker, self->dev->image->exif_model);
  // darktable built-in, if applicable
  for(int k=0; k<dt_profiled_colormatrix_cnt; k++)
  {
    if(!strcmp(makermodel, dt_profiled_colormatrices[k].makermodel))
    {
      prof = (dt_iop_color_profile_t *)malloc(sizeof(dt_iop_color_profile_t));
      g_strlcpy(prof->filename, "darktable", sizeof(prof->filename));
      g_strlcpy(prof->name, "darktable", sizeof(prof->name));
      g->profiles = g_list_append(g->profiles, prof);
      prof->pos = ++pos;
      break;
    }
  }

  // sRGB for ldr image input
  prof = (dt_iop_color_profile_t *)g_malloc0(sizeof(dt_iop_color_profile_t));
  g_strlcpy(prof->filename, "sRGB", sizeof(prof->filename));
  g_strlcpy(prof->name, "sRGB", sizeof(prof->name));
  g->profiles = g_list_append(g->profiles, prof);
  prof->pos = ++pos;

  // adobe rgb built-in
  prof = (dt_iop_color_profile_t *)g_malloc0(sizeof(dt_iop_color_profile_t));
  g_strlcpy(prof->filename, "adobergb", sizeof(prof->filename));
  g_strlcpy(prof->name, "adobergb", sizeof(prof->name));
  g->profiles = g_list_append(g->profiles, prof);
  prof->pos = ++pos;

  // add std RGB profile:
  prof = (dt_iop_color_profile_t *)g_malloc0(sizeof(dt_iop_color_profile_t));
  g_strlcpy(prof->filename, "linear_rgb", sizeof(prof->filename));
  g_strlcpy(prof->name, "linear_rgb", sizeof(prof->name));
  g->profiles = g_list_append(g->profiles, prof);
  prof->pos = ++pos;

  // XYZ built-in
  prof = (dt_iop_color_profile_t *)g_malloc0(sizeof(dt_iop_color_profile_t));
  g_strlcpy(prof->filename, "XYZ", sizeof(prof->filename));
  g_strlcpy(prof->name, "XYZ", sizeof(prof->name));
  g->profiles = g_list_append(g->profiles, prof);
  prof->pos = ++pos;

  // infrared built-in
  prof = (dt_iop_color_profile_t *)g_malloc0(sizeof(dt_iop_color_profile_t));
  g_strlcpy(prof->filename, "infrared", sizeof(prof->filename));
  g_strlcpy(prof->name, "infrared", sizeof(prof->name));
  g->profiles = g_list_append(g->profiles, prof);
  prof->pos = ++pos;

  // read {userconfig,datadir}/color/in/*.icc, in this order.
  char datadir[1024], confdir[1024], dirname[1024], filename[1024];
  dt_get_user_config_dir(confdir, 1024);
  dt_get_datadir(datadir, 1024);
  snprintf(dirname, 1024, "%s/color/in", confdir);
  if(!g_file_test(dirname, G_FILE_TEST_IS_DIR))
    snprintf(dirname, 1024, "%s/color/in", datadir);
  cmsHPROFILE tmpprof;
  const gchar *d_name;
  GDir *dir = g_dir_open(dirname, 0, NULL);
  if(dir)
  {
    while((d_name = g_dir_read_name(dir)))
    {
      if(!strcmp(d_name, "linear_rgb")) continue;
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
        cmsCloseProfile(tmpprof);
        g->profiles = g_list_append(g->profiles, prof);
        prof->pos = ++pos;
      }
    }
    g_dir_close(dir);
  }

  self->widget = GTK_WIDGET(gtk_hbox_new(FALSE, 0));
  g->vbox1 = GTK_VBOX(gtk_vbox_new(TRUE, DT_GUI_IOP_MODULE_CONTROL_SPACING));
  g->vbox2 = GTK_VBOX(gtk_vbox_new(TRUE, DT_GUI_IOP_MODULE_CONTROL_SPACING));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->vbox1), FALSE, FALSE, 5);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->vbox2), TRUE, TRUE, 5);
  GtkWidget *label = dtgtk_reset_label_new(_("profile"), self, &p->iccprofile, sizeof(char)*DT_IOP_COLOR_ICC_LEN);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(label), TRUE, TRUE, 0);
  g->cbox2 = GTK_COMBO_BOX(gtk_combo_box_new_text());
  GList *l = g->profiles;
  while(l)
  {
    dt_iop_color_profile_t *prof = (dt_iop_color_profile_t *)l->data;
    if(!strcmp(prof->name, "cmatrix"))
      gtk_combo_box_append_text(g->cbox2, _("standard color matrix"));
    else if(!strcmp(prof->name, "darktable"))
      gtk_combo_box_append_text(g->cbox2, _("enhanced color matrix"));
    else if(!strcmp(prof->name, "sRGB"))
      gtk_combo_box_append_text(g->cbox2, _("sRGB (e.g. jpg)"));
    else if(!strcmp(prof->name, "adobergb"))
      gtk_combo_box_append_text(g->cbox2, _("Adobe RGB"));
    else if(!strcmp(prof->name, "linear_rgb"))
      gtk_combo_box_append_text(g->cbox2, _("linear RGB"));
    else if(!strcmp(prof->name, "infrared"))
      gtk_combo_box_append_text(g->cbox2, _("linear infrared BGR"));
    else if(!strcmp(prof->name, "XYZ"))
      gtk_combo_box_append_text(g->cbox2, _("linear XYZ"));
    else
      gtk_combo_box_append_text(g->cbox2, prof->name);
    l = g_list_next(l);
  }
  gtk_combo_box_set_active(g->cbox2, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->cbox2), TRUE, TRUE, 0);

  char tooltip[1024];
  snprintf(tooltip, 1024, _("icc profiles in %s/color/in or %s/color/in"), confdir, datadir);
  g_object_set(G_OBJECT(g->cbox2), "tooltip-text", tooltip, (char *)NULL);

  g_signal_connect (G_OBJECT (g->cbox2), "changed",
                    G_CALLBACK (profile_changed),
                    (gpointer)self);
  // pthread_mutex_unlock(&darktable.plugin_threadsafe);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  dt_iop_colorin_gui_data_t *g = (dt_iop_colorin_gui_data_t *)self->gui_data;
  while(g->profiles)
  {
    g_free(g->profiles->data);
    g->profiles = g_list_delete_link(g->profiles, g->profiles);
  }
  free(self->gui_data);
  self->gui_data = NULL;
}

// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
