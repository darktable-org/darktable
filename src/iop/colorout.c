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
#include "gui/gtk.h"

const char *name()
{
  return _("output color profile");
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

void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *i, void *o, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  // pthread_mutex_lock(&darktable.plugin_threadsafe);
  dt_iop_colorout_data_t *d = (dt_iop_colorout_data_t *)piece->data;
  float *in  = (float *)i;
  float *out = (float *)o;
#ifdef _OPENMP // not thread safe?
// #pragma omp parallel for schedule(static) default(none) shared(out, width, height, in) firstprivate(d)
#endif
  for(int k=0;k<roi_out->width*roi_out->height;k++)
  {
    double RGB[3];
    cmsCIELab Lab;
    // convert from La/Lb/L to be able to change L without changing saturation.
    Lab.L = in[3*k+0];
    Lab.a = in[3*k+1]*Lab.L*(1.0/100.0);
    Lab.b = in[3*k+2]*Lab.L*(1.0/100.0);
    cmsDoTransform(d->xform, &Lab, RGB, 1);
    // RGB[0] = Lab.L; RGB[1] = Lab.a; RGB[2] = Lab.b;
    for(int c=0;c<3;c++) out[3*k + c] = RGB[c];
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
  if(d->output) cmsCloseProfile(d->output);

  if(pipe->type == DT_DEV_PIXELPIPE_EXPORT)
  {
    if(!strcmp(p->iccprofile, "sRGB"))
    { // default: sRGB
      d->output = NULL;
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
      snprintf(filename, 1024, "%s/color/out/%s", datadir, p->iccprofile);
      d->output = cmsOpenProfileFromFile(filename, "r");
    }
    d->Lab   = cmsCreateLabProfile(NULL);//cmsD50_xyY());
    if(d->output)
      d->xform = cmsCreateTransform(d->Lab, TYPE_Lab_DBL, d->output, TYPE_RGB_DBL, p->intent, 0);
    else
      d->xform = cmsCreateTransform(d->Lab, TYPE_Lab_DBL, cmsCreate_sRGBProfile(), TYPE_RGB_DBL, p->intent, 0);
  }
  else
  {
    if(!strcmp(p->displayprofile, "sRGB"))
    { // default: sRGB
      d->output = NULL;
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
    d->Lab   = cmsCreateLabProfile(NULL);//cmsD50_xyY());
    if(d->output)
      d->xform = cmsCreateTransform(d->Lab, TYPE_Lab_DBL, d->output, TYPE_RGB_DBL, p->displayintent, 0);
    else
      d->xform = cmsCreateTransform(d->Lab, TYPE_Lab_DBL, cmsCreate_sRGBProfile(), TYPE_RGB_DBL, p->displayintent, 0);
  }
#endif
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
  if(d->output) cmsCloseProfile(d->output);
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
  gtk_combo_box_set_active(g->cbox2, 0);
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
  int pos = prof->pos = 0;
  g->profiles = g_list_append(g->profiles, prof);
  prof = (dt_iop_color_profile_t *)malloc(sizeof(dt_iop_color_profile_t));
  strcpy(prof->filename, "X profile");
  strcpy(prof->name, "X profile");
  pos = prof->pos = 1;
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
  gtk_combo_box_append_text(g->cbox1, _("saturation"));
  gtk_combo_box_append_text(g->cbox1, _("absolute colorimetric"));
  g->cbox4 = GTK_COMBO_BOX(gtk_combo_box_new_text());
  gtk_combo_box_append_text(g->cbox4, _("perceptual"));
  gtk_combo_box_append_text(g->cbox4, _("relative colorimetric"));
  gtk_combo_box_append_text(g->cbox4, _("saturation"));
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
    g->profiles = g_list_delete_link(g->profiles, g->profiles);
  free(self->gui_data);
  self->gui_data = NULL;
}
