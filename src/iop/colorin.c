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
  // pthread_mutex_lock(&darktable.plugin_threadsafe);
  dt_iop_colorin_data_t *d = (dt_iop_colorin_data_t *)piece->data;
  float *in  = (float *)i;
  float *out = (float *)o;
  // not thread safe.
  for(int k=0;k<roi_out->width*roi_out->height;k++)
  {
    double XYZ[3] = {0., 0., 0.};
    cmsCIELab Lab;
    if(d->cmatrix[0][0] > -666.0)
    {
      for(int c=0;c<3;c++)
        for(int j=0;j<3;j++)
          XYZ[c] += d->cmatrix[c][j] * in[3*k + j];
    }
    else
    {
      for(int c=0;c<3;c++) XYZ[c] = in[3*k + c];
    }
    // convert to (L,a/L,b/L) to be able to change L without changing saturation.
    cmsDoTransform(d->xform, XYZ, &Lab, 1);
    // Lab.L = XYZ[0]; Lab.a = XYZ[1]; Lab.b = XYZ[2];
    out[3*k + 0] = Lab.L;
    if(Lab.L > 0)
    {
      out[3*k + 1] = 100.0*Lab.a/Lab.L;
      out[3*k + 2] = 100.0*Lab.b/Lab.L;
    }
    else
    {
      out[3*k + 1] = Lab.a;
      out[3*k + 2] = Lab.b;
    }
  }
  // pthread_mutex_unlock(&darktable.plugin_threadsafe);
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
  d->input = NULL;
  if(d->xform) cmsDeleteTransform(d->xform);
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
    d->xform = cmsCreateTransform(d->input, TYPE_RGB_DBL, d->Lab, TYPE_Lab_DBL, p->intent, 0);
  else
  {
    if(strcmp(p->iccprofile, "sRGB"))
    { // use linear_rgb as fallback for missing profiles:
      d->input = dt_colorspaces_create_linear_rgb_profile();
    }
    if(!d->input) d->input = dt_colorspaces_create_srgb_profile();
    d->xform = cmsCreateTransform(d->input, TYPE_RGB_DBL, d->Lab, TYPE_Lab_DBL, p->intent, 0);
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
  d->xform = NULL;
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
  if(d->xform) cmsDeleteTransform(d->xform);
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
  fprintf(stderr, "[colorin] could not find requested profile `%s'!\n", p->iccprofile);
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
  // (void)cmsErrorAction(LCMS_ERROR_IGNORE);
  (void)cmsErrorAction(LCMS_ERROR_SHOW);
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
        strcpy(prof->name, cmsTakeProductDesc(tmpprof));
        strcpy(prof->filename, d_name);
        cmsCloseProfile(tmpprof);
        g->profiles = g_list_append(g->profiles, prof);
        prof->pos = ++pos;
      }
    }
    g_dir_close(dir);
  }

  self->widget = GTK_WIDGET(gtk_hbox_new(FALSE, 0));
  g->vbox1 = GTK_VBOX(gtk_vbox_new(TRUE, 0));
  g->vbox2 = GTK_VBOX(gtk_vbox_new(TRUE, 0));
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
  gtk_object_set(GTK_OBJECT(g->cbox1), "tooltip-text", _("rendering intent"), NULL);
  snprintf(tooltip, 1024, _("icc profiles in %s/color/in"), datadir);
  gtk_object_set(GTK_OBJECT(g->cbox2), "tooltip-text", tooltip, NULL);

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
    g->profiles = g_list_delete_link(g->profiles, g->profiles);
  free(self->gui_data);
  self->gui_data = NULL;
}
