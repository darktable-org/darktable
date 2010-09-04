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
#include "iop/colorout.h"
#include "develop/develop.h"
#include "control/control.h"
#include "control/conf.h"
#include "gui/gtk.h"
#include "common/colorspaces.h"

DT_MODULE(1)

const char *name()
{
  return _("output color profile");
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
  dt_iop_colorout_params_t *p = (dt_iop_colorout_params_t *)self->params;
  p->intent = (dt_iop_color_intent_t)gtk_combo_box_get_active(widget);
  dt_dev_add_history_item(darktable.develop, self);
}

static void display_intent_changed (GtkComboBox *widget, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_colorout_params_t *p = (dt_iop_colorout_params_t *)self->params;
  p->displayintent = (dt_iop_color_intent_t)gtk_combo_box_get_active(widget);
  dt_dev_add_history_item(darktable.develop, self);
}

static void profile_changed (GtkComboBox *widget, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_colorout_params_t *p = (dt_iop_colorout_params_t *)self->params;
  dt_iop_colorout_gui_data_t *g = (dt_iop_colorout_gui_data_t *)self->gui_data;
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
  fprintf(stderr, "[colorout] color profile %s seems to have disappeared!\n", p->iccprofile);
}

static void display_profile_changed (GtkComboBox *widget, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_colorout_params_t *p = (dt_iop_colorout_params_t *)self->params;
  dt_iop_colorout_gui_data_t *g = (dt_iop_colorout_gui_data_t *)self->gui_data;
  int pos = gtk_combo_box_get_active(widget);
  GList *prof = g->profiles;
  while(prof)
  { // could use g_list_nth. this seems safer?
    dt_iop_color_profile_t *pp = (dt_iop_color_profile_t *)prof->data;
    if(pp->pos == pos)
    {
      strcpy(p->displayprofile, pp->filename);
      dt_dev_add_history_item(darktable.develop, self);
      return;
    }
    prof = g_list_next(prof);
  }
  // should really never happen.
  fprintf(stderr, "[colorout] display color profile %s seems to have disappeared!\n", p->displayprofile);
}

#if 0
static double
lab_f_1(double t)
{
  const double Limit = (24.0/116.0);

  if (t <= Limit)
  {
    double tmp;

    tmp = (108.0/841.0) * (t - (16.0/116.0));
    if (tmp <= 0.0) return 0.0;
    else return tmp;
  }

  return t * t * t;
}
#endif

void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *i, void *o, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  // pthread_mutex_lock(&darktable.plugin_threadsafe);
  dt_iop_colorout_data_t *d = (dt_iop_colorout_data_t *)piece->data;
  float *in  = (float *)i;
  float *out = (float *)o;
#ifdef _OPENMP
  #pragma omp parallel for schedule(static) default(none) shared(out, roi_out, in, d)
#endif
  for(int k=0;k<roi_out->width*roi_out->height;k++)
  {
    double rgb[3];
    cmsCIELab Lab;
    Lab.L = in[3*k+0];
    Lab.a = in[3*k+1]*Lab.L*(1.0/100.0);
    Lab.b = in[3*k+2]*Lab.L*(1.0/100.0);
#pragma omp critical
    { // lcms is not thread safe
    cmsDoTransform(d->xform, &Lab, rgb, 1);
    }
    for(int c=0;c<3;c++) out[3*k + c] = rgb[c];
  }
  // pthread_mutex_unlock(&darktable.plugin_threadsafe);
}

void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  // pthread_mutex_lock(&darktable.plugin_threadsafe);
  dt_iop_colorout_params_t *p = (dt_iop_colorout_params_t *)p1;
#ifdef HAVE_GEGL
  // pull in new params to gegl
  #error "gegl version needs some more care!"
#else
  dt_iop_colorout_data_t *d = (dt_iop_colorout_data_t *)piece->data;
  // dt_iop_colorout_global_data_t *gd = (dt_iop_colorout_global_data_t *)self->data;
  gchar *overprofile = dt_conf_get_string("plugins/lighttable/export/iccprofile");
  const int overintent = dt_conf_get_int("plugins/lighttable/export/iccintent");
  if(d->output) dt_colorspaces_cleanup_profile(d->output);
  d->output = NULL;
  if(d->xform) cmsDeleteTransform(d->xform);

  if(pipe->type == DT_DEV_PIXELPIPE_EXPORT)
  {
    // get export override:
    if(overprofile && strcmp(overprofile, "image")) snprintf(p->iccprofile, DT_IOP_COLOR_ICC_LEN, "%s", overprofile);
    if(overintent >= 0) p->intent = overintent;
    if(!strcmp(p->iccprofile, "sRGB"))
    { // default: sRGB
      d->output = dt_colorspaces_create_srgb_profile();
    }
    else if(!strcmp(p->iccprofile, "linear_rgb"))
    {
      d->output = dt_colorspaces_create_linear_rgb_profile();
    }
    else if(!strcmp(p->iccprofile, "adobergb"))
    {
      d->output = dt_colorspaces_create_adobergb_profile();
    }
    else if(!strcmp(p->iccprofile, "X profile"))
    { // x default
      if(darktable.control->xprofile_data) d->output = cmsOpenProfileFromMem(darktable.control->xprofile_data, darktable.control->xprofile_size);
      else d->output = NULL;
    }
    else
    { // else: load file name
      char datadir[1024];
      char filename[1024];
      dt_get_datadir(datadir, 1024);
      snprintf(filename, 1024, "%s/color/out/%s", datadir, p->iccprofile);
      d->output = cmsOpenProfileFromFile(filename, "r");
    }
    if(!d->output) d->output = dt_colorspaces_create_srgb_profile();
    d->xform = cmsCreateTransform(d->Lab, TYPE_Lab_DBL, d->output, TYPE_RGB_DBL, p->intent, 0);
    // d->xform = cmsCreateTransform(d->Lab, TYPE_RGB_DBL, d->output, TYPE_RGB_DBL, p->intent, 0);
  }
  else
  {
    if(!strcmp(p->displayprofile, "sRGB"))
    { // default: sRGB
      d->output = dt_colorspaces_create_srgb_profile();
    }
    else if(!strcmp(p->displayprofile, "linear_rgb"))
    {
      d->output = dt_colorspaces_create_linear_rgb_profile();
    }
    else if(!strcmp(p->displayprofile, "adobergb"))
    {
      d->output = dt_colorspaces_create_adobergb_profile();
    }
    else if(!strcmp(p->displayprofile, "X profile"))
    { // x default
      if(darktable.control->xprofile_data) d->output = cmsOpenProfileFromMem(darktable.control->xprofile_data, darktable.control->xprofile_size);
      else d->output = NULL;
    }
    else
    { // else: load file name
      char datadir[1024];
      char filename[1024];
      dt_get_datadir(datadir, 1024);
      snprintf(filename, 1024, "%s/color/out/%s", datadir, p->displayprofile);
      d->output = cmsOpenProfileFromFile(filename, "r");
    }
    if(!d->output) d->output = dt_colorspaces_create_srgb_profile();
    d->xform = cmsCreateTransform(d->Lab, TYPE_Lab_DBL, d->output, TYPE_RGB_DBL, p->displayintent, 0);
    // d->xform = cmsCreateTransform(d->Lab, TYPE_RGB_DBL, d->output, TYPE_RGB_DBL, p->displayintent, 0);
  }
  // user selected a non-supported output profile, check that:
  if(!d->xform)
  {
    dt_control_log(_("unsupported output profile has been replaced by sRGB!"));
    if(d->output) dt_colorspaces_cleanup_profile(d->output);
    d->output = dt_colorspaces_create_srgb_profile();
    d->xform = cmsCreateTransform(d->Lab, TYPE_Lab_DBL, d->output, TYPE_RGB_DBL, p->intent, 0);
    // d->xform = cmsCreateTransform(d->Lab, TYPE_RGB_DBL, d->output, TYPE_RGB_DBL, p->intent, 0);
  }
#endif
  g_free(overprofile);
  // pthread_mutex_unlock(&darktable.plugin_threadsafe);
}

void init_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
#ifdef HAVE_GEGL
  // create part of the gegl pipeline
#else
  piece->data = malloc(sizeof(dt_iop_colorout_data_t));
  dt_iop_colorout_data_t *d = (dt_iop_colorout_data_t *)piece->data;
  d->output = NULL;
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
  dt_iop_colorout_data_t *d = (dt_iop_colorout_data_t *)piece->data;
  if(d->output) dt_colorspaces_cleanup_profile(d->output);
  dt_colorspaces_cleanup_profile(d->Lab);
  if(d->xform) cmsDeleteTransform(d->xform);
  free(piece->data);
  // pthread_mutex_unlock(&darktable.plugin_threadsafe);
#endif
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_module_t *module = (dt_iop_module_t *)self;
  dt_iop_colorout_gui_data_t *g = (dt_iop_colorout_gui_data_t *)self->gui_data;
  dt_iop_colorout_params_t *p = (dt_iop_colorout_params_t *)module->params;
  gtk_combo_box_set_active(g->cbox1, (int)p->intent);
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
  // GtkWidget *widget = glade_xml_get_widget (darktable.gui->main_window, "center");
  // module->data = malloc(sizeof(dt_iop_colorout_global_data_t));
  // dt_iop_colorout_global_data_t *d = (dt_iop_colorout_global_data_t *)module->data;
  // get_display_profile(widget, &d->data, &d->data_size);
  module->params = malloc(sizeof(dt_iop_colorout_params_t));
  module->default_params = malloc(sizeof(dt_iop_colorout_params_t));
  module->params_size = sizeof(dt_iop_colorout_params_t);
  module->gui_data = NULL;
  module->priority = 900;
  module->hide_enable_button = 1;
  dt_iop_colorout_params_t tmp = (dt_iop_colorout_params_t){"sRGB", "X profile", DT_INTENT_PERCEPTUAL};
  memcpy(module->params, &tmp, sizeof(dt_iop_colorout_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_colorout_params_t));
}

void cleanup(dt_iop_module_t *module)
{
  // dt_iop_colorout_global_data_t *d = (dt_iop_colorout_global_data_t *)module->data;
  // g_free(d->data);
  // d->data = NULL;
  // free(module->data);
  // module->data = NULL;
  free(module->gui_data);
  module->gui_data = NULL;
  free(module->params);
  module->params = NULL;
}

void gui_init(struct dt_iop_module_t *self)
{
  // pthread_mutex_lock(&darktable.plugin_threadsafe);
  self->gui_data = malloc(sizeof(dt_iop_colorout_gui_data_t));
  dt_iop_colorout_gui_data_t *g = (dt_iop_colorout_gui_data_t *)self->gui_data;
  // dt_iop_colorout_params_t *p = (dt_iop_colorout_params_t *)self->params;

  g->profiles = NULL;
  dt_iop_color_profile_t *prof = (dt_iop_color_profile_t *)malloc(sizeof(dt_iop_color_profile_t));
  strcpy(prof->filename, "sRGB");
  strcpy(prof->name, "sRGB");
  int pos;
  prof->pos = 0;
  g->profiles = g_list_append(g->profiles, prof);

  prof = (dt_iop_color_profile_t *)malloc(sizeof(dt_iop_color_profile_t));
  strcpy(prof->filename, "adobergb");
  strcpy(prof->name, "adobergb");
  pos = prof->pos = 1;
  g->profiles = g_list_append(g->profiles, prof);

  prof = (dt_iop_color_profile_t *)malloc(sizeof(dt_iop_color_profile_t));
  strcpy(prof->filename, "X profile");
  strcpy(prof->name, "X profile");
  pos = prof->pos = 2;
  g->profiles = g_list_append(g->profiles, prof);

  prof = (dt_iop_color_profile_t *)malloc(sizeof(dt_iop_color_profile_t));
  strcpy(prof->filename, "linear_rgb");
  strcpy(prof->name, "linear_rgb");
  pos = prof->pos = 3;
  g->profiles = g_list_append(g->profiles, prof);

  // read datadir/color/out/*.icc
  char datadir[1024], dirname[1024], filename[1024];
  dt_get_datadir(datadir, 1024);
  snprintf(dirname, 1024, "%s/color/out", datadir);
  cmsHPROFILE tmpprof;
  const gchar *d_name;
  GDir *dir = g_dir_open(dirname, 0, NULL);
  (void)cmsErrorAction(LCMS_ERROR_IGNORE);
  if(dir)
  {
    while((d_name = g_dir_read_name(dir)))
    {
      snprintf(filename, 1024, "%s/%s", dirname, d_name);
      tmpprof = cmsOpenProfileFromFile(filename, "r");
      if(tmpprof)
      {
        dt_iop_color_profile_t *prof = (dt_iop_color_profile_t *)malloc(sizeof(dt_iop_color_profile_t));
        strcpy(prof->name, cmsTakeProductDesc(tmpprof));
        strcpy(prof->filename, d_name);
        prof->pos = ++pos;
        cmsCloseProfile(tmpprof);
        g->profiles = g_list_append(g->profiles, prof);
      }
    }
    g_dir_close(dir);
  }

  self->widget = GTK_WIDGET(gtk_hbox_new(FALSE, 0));
  g->vbox1 = GTK_VBOX(gtk_vbox_new(FALSE, 0));
  g->vbox2 = GTK_VBOX(gtk_vbox_new(FALSE, 0));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->vbox1), FALSE, FALSE, 5);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->vbox2), TRUE, TRUE, 5);
  g->label1 = GTK_LABEL(gtk_label_new(_("output intent")));
  g->label2 = GTK_LABEL(gtk_label_new(_("output profile")));
  g->label4 = GTK_LABEL(gtk_label_new(_("display intent")));
  g->label3 = GTK_LABEL(gtk_label_new(_("display profile")));
  gtk_misc_set_alignment(GTK_MISC(g->label1), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(g->label2), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(g->label3), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(g->label4), 0.0, 0.5);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label1), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label2), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label4), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label3), TRUE, TRUE, 0);
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
  GList *l = g->profiles;
  while(l)
  {
    dt_iop_color_profile_t *prof = (dt_iop_color_profile_t *)l->data;
    if(!strcmp(prof->name, "X profile"))
    {
      gtk_combo_box_append_text(g->cbox2, _("system display profile"));
      gtk_combo_box_append_text(g->cbox3, _("system display profile"));
    }
    else if(!strcmp(prof->name, "linear_rgb"))
    {
      gtk_combo_box_append_text(g->cbox2, _("linear rgb"));
      gtk_combo_box_append_text(g->cbox3, _("linear rgb"));
    }
    else if(!strcmp(prof->name, "sRGB"))
    {
      gtk_combo_box_append_text(g->cbox2, _("srgb (web-safe)"));
      gtk_combo_box_append_text(g->cbox3, _("srgb (web-safe)"));
    }
    else if(!strcmp(prof->name, "adobergb"))
    {
      gtk_combo_box_append_text(g->cbox2, _("adobe rgb"));
      gtk_combo_box_append_text(g->cbox3, _("adobe rgb"));
    }
    else
    {
      gtk_combo_box_append_text(g->cbox2, prof->name);
      gtk_combo_box_append_text(g->cbox3, prof->name);
    }
    l = g_list_next(l);
  }
  gtk_combo_box_set_active(g->cbox1, 0);
  gtk_combo_box_set_active(g->cbox2, 0);
  gtk_combo_box_set_active(g->cbox3, 0);
  gtk_combo_box_set_active(g->cbox4, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->cbox1), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->cbox2), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->cbox4), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->cbox3), TRUE, TRUE, 0);

  char tooltip[1024];
  gtk_object_set(GTK_OBJECT(g->cbox1), "tooltip-text", _("rendering intent"), NULL);
  snprintf(tooltip, 1024, _("icc profiles in %s/color/out"), datadir);
  gtk_object_set(GTK_OBJECT(g->cbox2), "tooltip-text", tooltip, NULL);
  snprintf(tooltip, 1024, _("display icc profiles in %s/color/out"), datadir);
  gtk_object_set(GTK_OBJECT(g->cbox3), "tooltip-text", tooltip, NULL);

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
  // pthread_mutex_unlock(&darktable.plugin_threadsafe);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  dt_iop_colorout_gui_data_t *g = (dt_iop_colorout_gui_data_t *)self->gui_data;
  while(g->profiles)
  {
    free(g->profiles->data);
    g->profiles = g_list_delete_link(g->profiles, g->profiles);
  }
  free(self->gui_data);
  self->gui_data = NULL;
}
