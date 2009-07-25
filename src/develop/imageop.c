#include "control/control.h"
#include "develop/imageop.h"
#include "develop/develop.h"
#include "gui/gtk.h"

#include <lcms.h>
#include <strings.h>
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <gmodule.h>
#include <pthread.h>

int dt_iop_load_module(dt_iop_module_t *module, dt_develop_t *dev, const char *op)
{
  const gchar *err;
  pthread_mutex_init(&module->params_mutex, NULL);
  module->instance = dev->iop_instance++;
  module->dt = &darktable;
  module->dev = dev;
  module->widget = NULL;
  module->off = NULL;
  module->enabled = module->default_enabled = 1; // all modules enabled by default.
  strncpy(module->op, op, 20);
  // load module from disk
  gchar *libname = g_module_build_path(DATADIR"/plugins", (const gchar *)op);
  module->module = g_module_open(libname, G_MODULE_BIND_LAZY);
  g_free(libname);
  if(!module->module) goto error;
  if(!g_module_symbol(module->module, "gui_update",             (gpointer)&(module->gui_update)))             goto error;
  if(!g_module_symbol(module->module, "gui_init",               (gpointer)&(module->gui_init)))               goto error;
  if(!g_module_symbol(module->module, "gui_cleanup",            (gpointer)&(module->gui_cleanup)))            goto error;
  if(!g_module_symbol(module->module, "init",                   (gpointer)&(module->init)))                   goto error;
  if(!g_module_symbol(module->module, "cleanup",                (gpointer)&(module->cleanup)))                goto error;
  if(!g_module_symbol(module->module, "commit_params",          (gpointer)&(module->commit_params)))          goto error;
  if(!g_module_symbol(module->module, "init_pipe",              (gpointer)&(module->init_pipe)))              goto error;
  if(!g_module_symbol(module->module, "cleanup_pipe",           (gpointer)&(module->cleanup_pipe)))           goto error;
  if(!g_module_symbol(module->module, "process",                (gpointer)&(module->process)))                goto error;
  module->init(module);
  module->enabled = module->default_enabled; // apply (possibly new) default.
  return 0;
error:
  err = g_module_error();
  fprintf(stderr, "[iop_load_module] failed to open operation `%s': %s\n", op, err);
  if(module->module) g_module_close(module->module);
  return 1;
}

void dt_iop_unload_module(dt_iop_module_t *module)
{
  module->cleanup(module);
  pthread_mutex_destroy(&module->params_mutex);
  if(module->module) g_module_close(module->module);
}

void dt_iop_commit_params(dt_iop_module_t *module, dt_iop_params_t *params, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  uint64_t hash = 5381;
  module->commit_params(module, params, pipe, piece);
  const char *str = (const char *)params;
  for(int i=0;i<module->params_size;i++) hash = ((hash << 5) + hash) ^ str[i];
  if(piece->enabled) hash = ((hash << 5) + hash) ^ 13;
  piece->hash = hash;
}

void dt_iop_gui_update(dt_iop_module_t *module)
{
  module->gui_update(module);
  if(module->off) gtk_toggle_button_set_active(module->off, 1-module->enabled);
}

void dt_iop_gui_off_callback(GtkToggleButton *togglebutton, gpointer user_data)
{
  dt_iop_module_t *module = (dt_iop_module_t *)user_data;
  if(!darktable.gui->reset)
  {
    if(gtk_toggle_button_get_active(togglebutton)) module->enabled = 0;
    else module->enabled = 1;
    dt_dev_add_history_item(module->dev, module);
    // close parent expander.
    gtk_expander_set_expanded(module->expander, module->enabled);
  }
  char tooltip[512];
  snprintf(tooltip, 512, "%s is switched %s", module->op, module->enabled ? "on" : "off");
  gtk_object_set(GTK_OBJECT(togglebutton), "tooltip-text", tooltip, NULL);
}

static void dt_iop_gui_expander_callback(GObject *object, GParamSpec *param_spec, gpointer user_data)
{
  GtkExpander *expander = GTK_EXPANDER (object);
  dt_iop_module_t *module = (dt_iop_module_t *)user_data;
  if (gtk_expander_get_expanded (expander)) gtk_widget_show(module->widget);
  else gtk_widget_hide(module->widget);
}

GtkWidget *dt_iop_gui_get_expander(dt_iop_module_t *module)
{
  // gamma is always needed for display (down to uint8_t)
  // tonecurve and temperature for color conversion/histogram triggers
  if(!strcmp(module->op, "gamma") || 
     !strcmp(module->op, "tonecurve") ||
     !strcmp(module->op, "temperature"))
  {
    module->expander = GTK_EXPANDER(gtk_expander_new((const gchar *)(module->op)));
    gtk_expander_set_expanded(module->expander, FALSE);
    gtk_expander_set_spacing(module->expander, 10);
    gtk_container_add(GTK_CONTAINER(module->expander), module->widget);
    return GTK_WIDGET(module->expander);
  }

  GtkHBox *hbox = GTK_HBOX(gtk_hbox_new(FALSE, 0));
  GtkVBox *vbox = GTK_VBOX(gtk_vbox_new(FALSE, 0));
  module->expander = GTK_EXPANDER(gtk_expander_new((const gchar *)(module->op)));
  gtk_expander_set_expanded(module->expander, FALSE);
  // gtk_object_set(GTK_OBJECT(module->widget), "no-show-all", TRUE, NULL);
  gtk_widget_hide(module->widget);
  gtk_expander_set_spacing(module->expander, 10);
  // GtkToggleButton *button = GTK_TOGGLE_BUTTON(gtk_toggle_button_new());
  GtkToggleButton *button = GTK_TOGGLE_BUTTON(gtk_check_button_new());
  char tooltip[512];
  snprintf(tooltip, 512, "%s is switched %s", module->op, module->enabled ? "on" : "off");
  gtk_object_set(GTK_OBJECT(button), "tooltip-text", tooltip, NULL);
  gtk_toggle_button_set_active(button, 1-module->enabled);
  char filename[512];
  snprintf(filename, 512, "%s/pixmaps/off.png", DATADIR);
  GtkWidget *image = gtk_image_new_from_file(filename);
  gtk_button_set_image(GTK_BUTTON(button), image);
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(module->expander), TRUE, TRUE, 0);
  gtk_box_pack_end  (GTK_BOX(hbox), GTK_WIDGET(button), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(hbox), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(vbox), module->widget, TRUE, TRUE, 0);

  g_signal_connect (G_OBJECT (button), "toggled",
                    G_CALLBACK (dt_iop_gui_off_callback), module);
  g_signal_connect (G_OBJECT (module->expander), "notify::expanded",
                  G_CALLBACK (dt_iop_gui_expander_callback), module);
  // gtk_widget_add_events(GTK_WIDGET(hbox), GDK_BUTTON_PRESS_MASK);
  module->off = button;
  return GTK_WIDGET(vbox);
}

void dt_iop_clip_and_zoom(const float *i, int32_t ix, int32_t iy, int32_t iw, int32_t ih, int32_t ibw, int32_t ibh,
                                float *o, int32_t ox, int32_t oy, int32_t ow, int32_t oh, int32_t obw, int32_t obh)
{
  /* FIXME: broken:
  // optimized 1:1 branch:
  if(iw == ow && ih == oh)
  {
    int x = ix, y = iy;
    const int oh2 = MIN(oh, ih - iy);
    const int ow2 = MIN(ow, iw - ix);
    int idx = 0;
    for(int j=0;j<oh2;j++)
    {
      for(int l=0;l<ow2;l++)
      {
        for(int k=0;k<3;k++) o[3*idx + k] = i[3*(ibw*y + x) + k];
        x++; idx++;
      }
      y++; x = ix;
      idx = obw*j;
    }
    return;
  }*/
  // general case
  const float scalex = iw/(float)ow;
  const float scaley = ih/(float)oh;
  int32_t ix2 = MAX(ix, 0);
  int32_t iy2 = MAX(iy, 0);
  int32_t ox2 = MAX(ox, 0);
  int32_t oy2 = MAX(oy, 0);
  int32_t oh2 = MIN(MIN(oh, (ibh - iy2)/scaley), obh - oy2);
  int32_t ow2 = MIN(MIN(ow, (ibw - ix2)/scalex), obw - ox2);
  assert((int)(ix2 + ow2*scalex) <= ibw);
  assert((int)(iy2 + oh2*scaley) <= ibh);
  assert(ox2 + ow2 <= obw);
  assert(oy2 + oh2 <= obh);
  assert(ix2 >= 0 && iy2 >= 0 && ox2 >= 0 && oy2 >= 0);
  float x = ix2, y = iy2;
  for(int s=0;s<oh2;s++)
  {
    int idx = ox2 + obw*(oy2+s);
    for(int t=0;t<ow2;t++)
    {
      for(int k=0;k<3;k++) o[3*idx + k] =  //i[3*(ibw* (int)y +             (int)x             ) + k)];
             (i[(3*(ibw*(int32_t) y +            (int32_t) (x + .5f*scalex)) + k)] +
              i[(3*(ibw*(int32_t)(y+.5f*scaley) +(int32_t) (x + .5f*scalex)) + k)] +
              i[(3*(ibw*(int32_t)(y+.5f*scaley) +(int32_t) (x             )) + k)] +
              i[(3*(ibw*(int32_t) y +            (int32_t) (x             )) + k)])*0.25;
      x += scalex; idx++;
    }
    y += scaley; x = ix2;
  }
}

void dt_iop_sRGB_to_Lab(const float *in, float *out, int x, int y, float scale, int width, int height)
{
  // TODO: use lcms dbl/16-bit + upconversion?
  cmsHPROFILE hsRGB = cmsCreate_sRGBProfile();
  cmsHPROFILE  hLab  = cmsCreateLabProfile(NULL);//cmsD50_xyY());

  cmsHTRANSFORM  xform = cmsCreateTransform(hsRGB, TYPE_RGB_DBL, hLab, TYPE_Lab_DBL, 
      INTENT_PERCEPTUAL, 0);//cmsFLAGS_NOTPRECALC);

  for(int j=0;j<height;j++) for(int i=0;i<width;i++)
  {
    double RGB[3];
    cmsCIELab Lab;

    for(int k=0;k<3;k++) RGB[k] = in[3*(width*j + i) + k];
    cmsDoTransform(xform, RGB, &Lab, 1);
    out[3*(width*j+i) + 0] = Lab.L;
    out[3*(width*j+i) + 1] = Lab.a;
    out[3*(width*j+i) + 2] = Lab.b;
  }
}

void dt_iop_Lab_to_sRGB_16(uint16_t *in, uint16_t *out, int x, int y, float scale, int width, int height)
{
  cmsHPROFILE hsRGB = cmsCreate_sRGBProfile();
  cmsHPROFILE hLab  = cmsCreateLabProfile(NULL);//cmsD50_xyY());

  cmsHTRANSFORM  xform = cmsCreateTransform(hLab, TYPE_Lab_16, hsRGB, TYPE_RGB_16, 
      INTENT_PERCEPTUAL, 0);//cmsFLAGS_NOTPRECALC);

  for(int j=0;j<height;j++)
  {
    uint16_t *lab = in + 3*width*j;
    uint16_t *rgb = out + 3*width*j;
    cmsDoTransform(xform, lab, rgb, width);
  }
}

void dt_iop_Lab_to_sRGB(const float *in, float *out, int x, int y, float scale, int width, int height)
{
  cmsHPROFILE hsRGB = cmsCreate_sRGBProfile();
  cmsHPROFILE hLab  = cmsCreateLabProfile(NULL);//cmsD50_xyY());

  cmsHTRANSFORM  xform = cmsCreateTransform(hLab, TYPE_Lab_DBL, hsRGB, TYPE_RGB_DBL, 
      INTENT_PERCEPTUAL, 0);//cmsFLAGS_NOTPRECALC);

  for(int j=0;j<height;j++) for(int i=0;i<width;i++)
  {
    double RGB[3];
    cmsCIELab Lab;

    Lab.L = in[3*(width*j+i) + 0];
    Lab.a = in[3*(width*j+i) + 1];
    Lab.b = in[3*(width*j+i) + 2];
    cmsDoTransform(xform, &Lab, RGB, 1);
    for(int k=0;k<3;k++) out[3*(width*j + i) + k] = RGB[k];
  }
}

