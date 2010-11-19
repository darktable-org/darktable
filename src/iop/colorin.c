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
#include <stdlib.h>
#include <math.h>
#include <assert.h>
#include <string.h>
// TODO: if using GEGL, this needs to be wrapped in color conversion routines of gegl?
#include "iop/colorin.h"
#include "develop/develop.h"
#include "control/control.h"
#include "gui/gtk.h"
#include "libraw/libraw.h"
#include "common/colorspaces.h"
#include "common/colormatrices.c"

DT_MODULE(1)

const char *name()
{
  return _("input color profile");
}

int 
groups () 
{
	return IOP_GROUP_COLOR;
}

static void intent_changed (GtkComboBox *widget, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_colorin_params_t *p = (dt_iop_colorin_params_t *)self->params;
  p->intent = (dt_iop_color_intent_t)gtk_combo_box_get_active(widget);
  dt_dev_add_history_item(darktable.develop, self);
}

static void profile_changed (GtkComboBox *widget, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_colorin_params_t *p = (dt_iop_colorin_params_t *)self->params;
  dt_iop_colorin_gui_data_t *g = (dt_iop_colorin_gui_data_t *)self->gui_data;
  int pos = gtk_combo_box_get_active(widget);
  GList *prof = g->profiles;
  while(prof)
  { // could use g_list_nth. this seems safer?
    dt_iop_color_profile_t *pp = (dt_iop_color_profile_t *)prof->data;
    if(pp->pos == pos)
    {
      strcpy(p->iccprofile, pp->filename);
      dt_dev_add_history_item(darktable.develop, self);
      return;
    }
    prof = g_list_next(prof);
  }
  // should really never happen.
  fprintf(stderr, "[colorin] color profile %s seems to have disappeared!\n", p->iccprofile);
}

void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *i, void *o, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_colorin_data_t *d = (dt_iop_colorin_data_t *)piece->data;
  float *in  = (float *)i;
  float *out = (float *)o;
  const int ch = piece->colors;

  // with the critical section around lcms, this is slower than monothread, even on dual cores.
#ifdef _OPENMP
  #pragma omp parallel for default(none) shared(roi_out, out, in, d) schedule(static)
#endif
  for(int k=0;k<roi_out->width*roi_out->height;k++)
  {
    float cam[3] = {0., 0., 0.};
    float Lab[3];
    for(int c=0;c<3;c++) cam[c] = in[ch*k + c];
#if 1
    // manual gamut mapping. these values cause trouble when converting back from Lab to sRGB:
    const float YY = cam[0]+cam[1]+cam[2];
    const float zz = cam[2]/YY;
    // lower amount and higher bound_z make the effect smaller.
    // the effect is weakened the darker input values are, saturating at bound_Y
    const float bound_z = 0.5f, bound_Y = 0.5f;
    const float amount = 0.11f;
    // if(YY > bound_Y && zz > bound_z)
    if(zz > bound_z)
    {
      const float t = (zz - bound_z)/(1.0f-bound_z) * fminf(1.0, YY/bound_Y);
      cam[1] += t*amount;
      cam[2] -= t*amount;
    }
#endif
    // convert to (L,a/L,b/L) to be able to change L without changing saturation.
    // lcms is not thread safe, so work on one copy for each thread :(
    cmsDoTransform(d->xform[dt_get_thread_num()], cam, Lab, 1);
    out[ch*k + 0] = Lab[0];
    if(Lab[0] > 0)
    {
      out[ch*k + 1] = 100.0*Lab[1]/Lab[0];
      out[ch*k + 2] = 100.0*Lab[2]/Lab[0];
    }
    else
    {
      out[ch*k + 1] = Lab[1];
      out[ch*k + 2] = Lab[2];
    }
  }
}

void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  // pthread_mutex_lock(&darktable.plugin_threadsafe);
  dt_iop_colorin_params_t *p = (dt_iop_colorin_params_t *)p1;
#ifdef HAVE_GEGL
  // pull in new params to gegl
  #error "gegl version needs some more care!"
#else
  dt_iop_colorin_data_t *d = (dt_iop_colorin_data_t *)piece->data;
  if(d->input) cmsCloseProfile(d->input);
  const int num_threads = dt_get_num_threads();
  d->input = NULL;
  for(int t=0;t<num_threads;t++) if(d->xform[t]) cmsDeleteTransform(d->xform[t]);
  d->cmatrix[0][0] = -666.0;
  char datadir[1024];
  char filename[1024];
  dt_get_datadir(datadir, 1024);
  int preview_thumb = self->dev->image->flags & DT_IMAGE_THUMBNAIL;
  if(!strcmp(p->iccprofile, "darktable") && !preview_thumb)
  {
    char maker[512];
    snprintf(maker, 512, "%s", self->dev->image->exif_maker);
    char makermodel[512];
    char *c = g_strstr_len(maker, 512, "CORPORATION");
    if(c) *(c-1) = '\0';
    if(!strncmp(maker, self->dev->image->exif_model, strlen(maker)))
      snprintf(makermodel, 512, "%s", self->dev->image->exif_model);
    else
      snprintf(makermodel, 512, "%s %s", maker, self->dev->image->exif_model);
    // printf("searching matrix for `%s'\n", makermodel);
    d->input = dt_colorspaces_create_darktable_profile(makermodel);
    // if(!d->input) printf("could not find enhanced color matrix for `%s'!\n", makermodel);
    if(!d->input) sprintf(p->iccprofile, "cmatrix");
  }
  if(!strcmp(p->iccprofile, "cmatrix") && !preview_thumb)
  { // color matrix
    int ret;
    dt_image_full_path(self->dev->image, filename, 1024);
    libraw_data_t *raw = libraw_init(0);
    ret = libraw_open_file(raw, filename);
    if(!ret)
    {
      float cmat[3][4];
      for(int k=0;k<4;k++) for(int i=0;i<3;i++)
      {
        // d->cmatrix[i][k] = raw->color.rgb_cam[i][k];
        cmat[i][k] = raw->color.rgb_cam[i][k];
      }
      d->input = dt_colorspaces_create_cmatrix_profile(cmat);
    }
    libraw_close(raw);
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
  else if(!strcmp(p->iccprofile, "linear_rgb") || preview_thumb)
  {
    d->input = dt_colorspaces_create_linear_rgb_profile();
  }
  else if(!d->input)
  {
    snprintf(filename, 1024, "%s/color/in/%s", datadir, p->iccprofile);
    d->input = cmsOpenProfileFromFile(filename, "r");
  }
  if(d->input)
    for(int t=0;t<num_threads;t++)
    {
      d->xform[t] = cmsCreateTransform(d->input, TYPE_RGB_FLT, d->Lab, TYPE_Lab_FLT, p->intent, 0);
    }
  else
  {
    if(strcmp(p->iccprofile, "sRGB"))
    { // use linear_rgb as fallback for missing profiles:
      d->input = dt_colorspaces_create_linear_rgb_profile();
    }
    if(!d->input) d->input = dt_colorspaces_create_srgb_profile();
    for(int t=0;t<num_threads;t++) d->xform[t] = cmsCreateTransform(d->input, TYPE_RGB_FLT, d->Lab, TYPE_Lab_FLT, p->intent, 0);
  }
  // user selected a non-supported output profile, check that:
  if(!d->xform[0])
  {
    dt_control_log(_("unsupported input profile has been replaced by linear rgb!"));
    if(d->input) dt_colorspaces_cleanup_profile(d->input);
    d->input = dt_colorspaces_create_linear_rgb_profile();
    for(int t=0;t<num_threads;t++) d->xform[t] = cmsCreateTransform(d->Lab, TYPE_RGB_FLT, d->input, TYPE_Lab_FLT, p->intent, 0);
  }
  // pthread_mutex_unlock(&darktable.plugin_threadsafe);
#endif
}

void init_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
#ifdef HAVE_GEGL
  // create part of the gegl pipeline
#else
  piece->data = malloc(sizeof(dt_iop_colorin_data_t));
  dt_iop_colorin_data_t *d = (dt_iop_colorin_data_t *)piece->data;
  d->input = NULL;
  d->xform = (cmsHTRANSFORM *)malloc(sizeof(cmsHTRANSFORM)*dt_get_num_threads());
  for(int t=0;t<dt_get_num_threads();t++) d->xform[t] = NULL;
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
  // pthread_mutex_lock(&darktable.plugin_threadsafe);
  dt_iop_colorin_data_t *d = (dt_iop_colorin_data_t *)piece->data;
  if(d->input) dt_colorspaces_cleanup_profile(d->input);
  dt_colorspaces_cleanup_profile(d->Lab);
  for(int t=0;t<dt_get_num_threads();t++) if(d->xform[t]) cmsDeleteTransform(d->xform[t]);
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
  gtk_combo_box_set_active(g->cbox1, (int)p->intent);
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

void init(dt_iop_module_t *module)
{
  // module->data = malloc(sizeof(dt_iop_colorin_data_t));
  module->params = malloc(sizeof(dt_iop_colorin_params_t));
  module->default_params = malloc(sizeof(dt_iop_colorin_params_t));
  module->params_size = sizeof(dt_iop_colorin_params_t);
  module->gui_data = NULL;
  module->priority = 300;
  module->hide_enable_button = 1;
  dt_iop_colorin_params_t tmp = (dt_iop_colorin_params_t){"darktable", DT_INTENT_PERCEPTUAL};
  if(dt_image_is_ldr(module->dev->image)) strcpy(tmp.iccprofile, "sRGB");
  memcpy(module->params, &tmp, sizeof(dt_iop_colorin_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_colorin_params_t));
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
  // dt_iop_colorin_params_t *p = (dt_iop_colorin_params_t *)self->params;

  g->profiles = NULL;

  // get color matrix from raw image:
  dt_iop_color_profile_t *prof = (dt_iop_color_profile_t *)malloc(sizeof(dt_iop_color_profile_t));
  strcpy(prof->filename, "cmatrix");
  strcpy(prof->name, "cmatrix");
  g->profiles = g_list_append(g->profiles, prof);
  int pos = prof->pos = 0;

  // darktable built-in, if applicable
  char makermodel[512];
  snprintf(makermodel, 512, "%s %s", self->dev->image->exif_maker, self->dev->image->exif_model);
  for(int k=0;k<dt_profiled_colormatrix_cnt;k++)
  {
    if(!strcmp(makermodel, dt_profiled_colormatrices[k].makermodel))
    {
      prof = (dt_iop_color_profile_t *)malloc(sizeof(dt_iop_color_profile_t));
      strcpy(prof->filename, "darktable");
      strcpy(prof->name, "darktable");
      g->profiles = g_list_append(g->profiles, prof);
      prof->pos = ++pos;
      break;
    }
  }

  // sRGB for ldr image input
  prof = (dt_iop_color_profile_t *)malloc(sizeof(dt_iop_color_profile_t));
  strcpy(prof->filename, "sRGB");
  strcpy(prof->name, "sRGB");
  g->profiles = g_list_append(g->profiles, prof);
  prof->pos = ++pos;

  // adobe rgb built-in
  prof = (dt_iop_color_profile_t *)malloc(sizeof(dt_iop_color_profile_t));
  strcpy(prof->filename, "adobergb");
  strcpy(prof->name, "adobergb");
  g->profiles = g_list_append(g->profiles, prof);
  prof->pos = ++pos;

  // add std RGB profile:
  prof = (dt_iop_color_profile_t *)malloc(sizeof(dt_iop_color_profile_t));
  strcpy(prof->filename, "linear_rgb");
  strcpy(prof->name, "linear_rgb");
  g->profiles = g_list_append(g->profiles, prof);
  prof->pos = ++pos;

  // XYZ built-in
  prof = (dt_iop_color_profile_t *)malloc(sizeof(dt_iop_color_profile_t));
  strcpy(prof->filename, "XYZ");
  strcpy(prof->name, "XYZ");
  g->profiles = g_list_append(g->profiles, prof);
  prof->pos = ++pos;

  // infrared built-in
  prof = (dt_iop_color_profile_t *)malloc(sizeof(dt_iop_color_profile_t));
  strcpy(prof->filename, "infrared");
  strcpy(prof->name, "infrared");
  g->profiles = g_list_append(g->profiles, prof);
  prof->pos = ++pos;

  // read datadir/color/in/*.icc
  char datadir[1024], dirname[1024], filename[1024];
  dt_get_datadir(datadir, 1024);
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
        dt_iop_color_profile_t *prof = (dt_iop_color_profile_t *)malloc(sizeof(dt_iop_color_profile_t));
        char name[1024];
        cmsGetProfileInfoASCII(tmpprof, cmsInfoDescription, getenv("LANG"), getenv("LANG")+3, name, 1024);
        strcpy(prof->name, name);

        strcpy(prof->filename, d_name);
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
  g->label1 = GTK_LABEL(gtk_label_new(_("intent")));
  g->label2 = GTK_LABEL(gtk_label_new(_("profile")));
  gtk_misc_set_alignment(GTK_MISC(g->label1), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(g->label2), 0.0, 0.5);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label1), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label2), TRUE, TRUE, 0);
  g->cbox1 = GTK_COMBO_BOX(gtk_combo_box_new_text());
  gtk_combo_box_append_text(g->cbox1, _("perceptual"));
  gtk_combo_box_append_text(g->cbox1, _("relative colorimetric"));
  gtk_combo_box_append_text(g->cbox1,C_("rendering intent", "saturation"));
  gtk_combo_box_append_text(g->cbox1, _("absolute colorimetric"));
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
      gtk_combo_box_append_text(g->cbox2, _("srgb (e.g. jpg)"));
    else if(!strcmp(prof->name, "adobergb"))
      gtk_combo_box_append_text(g->cbox2, _("adobe rgb"));
    else if(!strcmp(prof->name, "linear_rgb"))
      gtk_combo_box_append_text(g->cbox2, _("linear rgb"));
    else if(!strcmp(prof->name, "infrared"))
      gtk_combo_box_append_text(g->cbox2, _("linear infrared bgr"));
    else if(!strcmp(prof->name, "XYZ"))
      gtk_combo_box_append_text(g->cbox2, _("linear xyz"));
    else
      gtk_combo_box_append_text(g->cbox2, prof->name);
    l = g_list_next(l);
  }
  gtk_combo_box_set_active(g->cbox1, 0);
  gtk_combo_box_set_active(g->cbox2, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->cbox1), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->cbox2), TRUE, TRUE, 0);

  char tooltip[1024];
  gtk_object_set(GTK_OBJECT(g->cbox1), "tooltip-text", _("rendering intent"), (char *)NULL);
  snprintf(tooltip, 1024, _("icc profiles in %s/color/in"), datadir);
  gtk_object_set(GTK_OBJECT(g->cbox2), "tooltip-text", tooltip, (char *)NULL);

  g_signal_connect (G_OBJECT (g->cbox1), "changed",
                    G_CALLBACK (intent_changed),
                    (gpointer)self);
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
    free(g->profiles->data);
    g->profiles = g_list_delete_link(g->profiles, g->profiles);
  }
  free(self->gui_data);
  self->gui_data = NULL;
}
