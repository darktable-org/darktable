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

static void intent_changed (GtkComboBox *widget, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_colorout_params_t *p = (dt_iop_colorout_params_t *)self->params;
  p->intent = (dt_iop_color_intent_t)gtk_combo_box_get_active(widget);
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
  gchar *text = gtk_combo_box_get_active_text(widget);
  fprintf(stderr, "[iop_color] color profile %s seems to have disappeared!\n", text);
  g_free(text);
}

void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *i, void *o, int x, int y, float scale, int width, int height)
{
  dt_iop_colorout_data_t *d = (dt_iop_colorout_data_t *)piece->data;
  float *in  = (float *)i;
  float *out = (float *)o;
#ifdef _OPENMP // not thread safe?
// #pragma omp parallel for schedule(static) default(none) shared(out, width, height, in) firstprivate(d)
#endif
  for(int k=0;k<width*height;k++)
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
}

void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_colorout_params_t *p = (dt_iop_colorout_params_t *)p1;
#ifdef HAVE_GEGL
  // pull in new params to gegl
  #error "gegl version needs some more care!"
#else
  dt_iop_colorout_data_t *d = (dt_iop_colorout_data_t *)piece->data;
  if(d->output) cmsCloseProfile(d->output);
  if(!strcmp(p->iccprofile, "sRGB"))
  { // default: sRGB
    d->output = NULL;
    d->Lab   = cmsCreateLabProfile(NULL);//cmsD50_xyY());
    d->xform = cmsCreateTransform(d->Lab, TYPE_Lab_DBL, cmsCreate_sRGBProfile(), TYPE_RGB_DBL, p->intent, 0);
  }
  else
  { // else: load file name
    char datadir[1024];
    char filename[1024];
    dt_get_datadir(datadir, 1024);
    snprintf(filename, 1024, "%s/color/out/%s", datadir, p->iccprofile);
    d->output = cmsOpenProfileFromFile(filename, "r");
    d->Lab   = cmsCreateLabProfile(NULL);//cmsD50_xyY());
    d->xform = cmsCreateTransform(d->Lab, TYPE_Lab_DBL, d->output, TYPE_RGB_DBL, p->intent, 0);
  }
#endif
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
  dt_iop_colorout_data_t *d = (dt_iop_colorout_data_t *)piece->data;
  if(d->output) cmsCloseProfile(d->output);
  free(piece->data);
#endif
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_module_t *module = (dt_iop_module_t *)self;
  dt_iop_colorout_gui_data_t *g = (dt_iop_colorout_gui_data_t *)self->gui_data;
  dt_iop_colorout_params_t *p = (dt_iop_colorout_params_t *)module->params;
  gtk_combo_box_set_active(g->cbox1, (int)p->intent);
  GList *prof = g->profiles;
  while(prof)
  {
    dt_iop_color_profile_t *pp = (dt_iop_color_profile_t *)prof->data;
    if(!strcmp(pp->name, p->iccprofile))
    {
      gtk_combo_box_set_active(g->cbox2, pp->pos);
      return;
    }
    prof = g_list_next(prof);
  }
  fprintf(stderr, "[iop_color] could not find requested profile `%s'!\n", p->iccprofile);
}

void init(dt_iop_module_t *module)
{
  // module->data = malloc(sizeof(dt_iop_colorout_data_t));
  module->params = malloc(sizeof(dt_iop_colorout_params_t));
  module->default_params = malloc(sizeof(dt_iop_colorout_params_t));
  module->params_size = sizeof(dt_iop_colorout_params_t);
  module->gui_data = NULL;
  dt_iop_colorout_params_t tmp = (dt_iop_colorout_params_t){"sRGB", DT_INTENT_PERCEPTUAL};
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

void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_colorout_gui_data_t));
  dt_iop_colorout_gui_data_t *g = (dt_iop_colorout_gui_data_t *)self->gui_data;
  // dt_iop_colorout_params_t *p = (dt_iop_colorout_params_t *)self->params;

  g->profiles = NULL;
  dt_iop_color_profile_t *prof = (dt_iop_color_profile_t *)malloc(sizeof(dt_iop_color_profile_t));
  strcpy(prof->filename, "sRGB");
  strcpy(prof->name, "sRGB");
  prof->pos = 0;
  // TODO: read datadir/color/in/*.icc!
  // cmsGetProductName(hProfile)
  g->profiles = g_list_append(g->profiles, prof);

  self->widget = GTK_WIDGET(gtk_hbox_new(FALSE, 0));
  g->vbox1 = GTK_VBOX(gtk_vbox_new(FALSE, 0));
  g->vbox2 = GTK_VBOX(gtk_vbox_new(FALSE, 0));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->vbox1), FALSE, FALSE, 5);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->vbox2), TRUE, TRUE, 5);
  g->label1 = GTK_LABEL(gtk_label_new("intent"));
  g->label2 = GTK_LABEL(gtk_label_new("profile"));
  gtk_misc_set_alignment(GTK_MISC(g->label1), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(g->label2), 0.0, 0.5);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label1), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label2), TRUE, TRUE, 0);
  g->cbox1 = GTK_COMBO_BOX(gtk_combo_box_new_text());
  gtk_combo_box_append_text(g->cbox1, "perceptual");
  gtk_combo_box_append_text(g->cbox1, "relative colorimetric");
  gtk_combo_box_append_text(g->cbox1, "saturation");
  gtk_combo_box_append_text(g->cbox1, "absolute colorimetric");
  g->cbox2 = GTK_COMBO_BOX(gtk_combo_box_new_text());
  GList *l = g->profiles;
  while(l)
  {
    dt_iop_color_profile_t *prof = (dt_iop_color_profile_t *)l->data;
    gtk_combo_box_append_text(g->cbox2, prof->name);
    l = g_list_next(l);
  }
  gtk_combo_box_set_active(g->cbox1, 0);
  gtk_combo_box_set_active(g->cbox2, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->cbox1), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->cbox2), TRUE, TRUE, 0);

  g_signal_connect (G_OBJECT (g->cbox1), "changed",
                    G_CALLBACK (intent_changed),
                    (gpointer)self);
  g_signal_connect (G_OBJECT (g->cbox2), "changed",
                    G_CALLBACK (profile_changed),
                    (gpointer)self);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  dt_iop_colorout_gui_data_t *g = (dt_iop_colorout_gui_data_t *)self->gui_data;
  while(g->profiles)
    g->profiles = g_list_delete_link(g->profiles, g->profiles);
  free(self->gui_data);
  self->gui_data = NULL;
}
