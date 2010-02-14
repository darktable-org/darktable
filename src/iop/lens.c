#ifdef HAVE_CONFIG_H
  #include "config.h"
#endif
#include <stdlib.h>
#include <math.h>
#include <assert.h>
#include <string.h>
#include <gtk/gtk.h>
#include <inttypes.h>
#include <ctype.h>
#ifdef HAVE_GEGL
  #include <gegl.h>
#endif
#include "develop/develop.h"
#include "develop/imageop.h"
#include "control/control.h"
#include "gui/gtk.h"
#include "gui/draw.h"
#include "iop/lens.h"

DT_MODULE(1)

const char *name()
{
  return _("lens distortions");
}

void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *i, void *o, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_lensfun_data_t *d = (dt_iop_lensfun_data_t *)piece->data;
  float *in  = (float *)i;
  float *out = (float *)o;

  if(!d->lens->Maker)
  {
    memcpy(out, in, 3*sizeof(float)*roi_out->width*roi_out->height);
    return;
  }

  const float orig_w = roi_in->scale*piece->iwidth,
              orig_h = roi_in->scale*piece->iheight;
  pthread_mutex_lock(&darktable.plugin_threadsafe);
  lfModifier *modifier = lf_modifier_new(d->lens, d->crop, orig_w, orig_h);

  int modflags = lf_modifier_initialize(
      modifier, d->lens, LF_PF_F32,
      d->focal, d->aperture,
      d->distance, d->scale,
      d->target_geom, d->modify_flags, d->inverse);
  pthread_mutex_unlock(&darktable.plugin_threadsafe);

  if(d->inverse)
  {
    // reverse direction (useful for renderings)
    if (modflags & (LF_MODIFY_TCA | LF_MODIFY_DISTORTION |
          LF_MODIFY_GEOMETRY | LF_MODIFY_SCALE))
    {
      // acquire temp memory for distorted pixel coords
      const size_t req2 = roi_in->width*2*3*sizeof(float);
      if(req2 > 0 && d->tmpbuf2_len < req2)
      {
        d->tmpbuf2_len = req2;
        d->tmpbuf2 = (float *)realloc(d->tmpbuf2, req2);
      }
      // TODO: openmp this?
      float *buf = out;
      for (int y = 0; y < roi_out->height; y++)
      {
        if (!lf_modifier_apply_subpixel_geometry_distortion (
              modifier, roi_out->x, roi_out->y+y, roi_out->width, 1, d->tmpbuf2)) break;
        // reverse transform the global coords from lf to our buffer
        const float *pi = d->tmpbuf2;
        for (int x = 0; x < roi_out->width; x++)
        {
          for(int c=0;c<3;c++) 
          {
            const float pi0 = pi[0] - roi_in->x, pi1 = pi[1] - roi_in->y;
            const int ii = (int)pi0, jj = (int)pi1;
            if(ii >= 0 && jj >= 0 && ii <= roi_in->width-2 && jj <= roi_in->height-2) 
            {
              const float fi = pi0 - ii, fj = pi1 - jj;
              buf[c] = // in[3*(roi_in->width*jj + ii) + c];
              ((1.0f-fj)*(1.0f-fi)*in[3*(roi_in->width*(jj)   + (ii)  ) + c] +
               (1.0f-fj)*(     fi)*in[3*(roi_in->width*(jj)   + (ii+1)) + c] +
               (     fj)*(     fi)*in[3*(roi_in->width*(jj+1) + (ii+1)) + c] +
               (     fj)*(1.0f-fi)*in[3*(roi_in->width*(jj+1) + (ii)  ) + c]);
            }
            else for(int c=0;c<3;c++) buf[c] = 0.0f;
            pi+=2;
          }
          buf += 3;
        }
      }
    }
    else
    {
      memcpy(out, in, 3*sizeof(float)*roi_out->width*roi_out->height);
    }

    if (modflags & LF_MODIFY_VIGNETTING)
    {
      // TODO: openmp this? is lf thread-safe?
      for (int y = 0; y < roi_out->height; y++)
      {
        /* Colour correction: vignetting and CCI */
        // actually this way row stride does not matter. but give a buffer pointer
        // offset by -roi_in.x
        float *buf = out - 3*(roi_out->width*roi_out->y + roi_out->x);
        if(lf_modifier_apply_color_modification (modifier,
              buf + 3*roi_out->width*y, roi_out->x, roi_out->y + y,
              roi_out->width, 1, LF_CR_3 (RED, GREEN, BLUE), 3*roi_out->width)) break;
      }
    }
  }
  else // correct distortions:
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
        // actually this way row stride does not matter. but give a buffer pointer
        // offset by -roi_in.x
        float *buf = d->tmpbuf - 3*(roi_in->width*roi_in->y + roi_in->x);
        if(lf_modifier_apply_color_modification (modifier,
              buf + 3*roi_in->width*y, roi_in->x, roi_in->y + y,
              roi_in->width, 1, LF_CR_3 (RED, GREEN, BLUE), 3*roi_in->width)) break;
      }
    }

    const size_t req2 = roi_out->width*2*3*sizeof(float);
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
              modifier, roi_out->x, roi_out->y+y, roi_out->width, 1, d->tmpbuf2)) break;
        // reverse transform the global coords from lf to our buffer
        const float *pi = d->tmpbuf2;
        for (int x = 0; x < roi_out->width; x++)
        {
          for(int c=0;c<3;c++) 
          {
            const float pi0 = pi[0] - roi_in->x, pi1 = pi[1] - roi_in->y;
            const int ii = (int)pi0, jj = (int)pi1;
            if(ii >= 0 && jj >= 0 && ii <= roi_in->width-2 && jj <= roi_in->height-2) 
            {
              const float fi = pi0 - ii, fj = pi1 - jj;
              out[c] = // in[3*(roi_in->width*jj + ii) + c];
              ((1.0f-fj)*(1.0f-fi)*d->tmpbuf[3*(roi_in->width*(jj)   + (ii)  ) + c] +
               (1.0f-fj)*(     fi)*d->tmpbuf[3*(roi_in->width*(jj)   + (ii+1)) + c] +
               (     fj)*(     fi)*d->tmpbuf[3*(roi_in->width*(jj+1) + (ii+1)) + c] +
               (     fj)*(1.0f-fi)*d->tmpbuf[3*(roi_in->width*(jj+1) + (ii)  ) + c]);
            }
            else for(int c=0;c<3;c++) out[c] = 0.0f;
            pi+=2;
          }
          out += 3;
        }
      }
    }
    else
    {
      size_t len = sizeof(float)*3*roi_out->width*roi_out->height;
      if (d->tmpbuf_len >= len)
           memcpy(out, d->tmpbuf, len);
      else memcpy(out, in, len);
    }
  }
  lf_modifier_destroy(modifier);
}

void modify_roi_out(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, dt_iop_roi_t *roi_out, const dt_iop_roi_t *roi_in)
{
  *roi_out = *roi_in;
}

void modify_roi_in(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, const dt_iop_roi_t *roi_out, dt_iop_roi_t *roi_in)
{
  dt_iop_lensfun_data_t *d = (dt_iop_lensfun_data_t *)piece->data;
  *roi_in = *roi_out;
  // inverse transform with given params

  if(!d->lens->Maker) return;

  const float orig_w = roi_in->scale*piece->iwidth,
              orig_h = roi_in->scale*piece->iheight;
  lfModifier *modifier = lf_modifier_new(d->lens, d->crop, orig_w, orig_h);

  float xm = INFINITY, xM = - INFINITY, ym = INFINITY, yM = - INFINITY;

  int modflags = lf_modifier_initialize(
      modifier, d->lens, LF_PF_F32,
      d->focal, d->aperture,
      d->distance, d->scale,
      d->target_geom, d->modify_flags, d->inverse);

  if (modflags & (LF_MODIFY_TCA | LF_MODIFY_DISTORTION |
        LF_MODIFY_GEOMETRY | LF_MODIFY_SCALE))
  {
    // acquire temp memory for distorted pixel coords
    const size_t req2 = roi_in->width*2*3*sizeof(float);
    if(req2 > 0 && d->tmpbuf2_len < req2)
    {
      d->tmpbuf2_len = req2;
      d->tmpbuf2 = (float *)realloc(d->tmpbuf2, req2);
    }
    for (int y = 0; y < roi_out->height; y++)
    {
      if (!lf_modifier_apply_subpixel_geometry_distortion (
            modifier, roi_out->x, roi_out->y+y, roi_out->width, 1, d->tmpbuf2)) break;
      // reverse transform the global coords from lf to our buffer
      const float *pi = d->tmpbuf2;
      for (int x = 0; x < roi_out->width; x++)
      {
        for(int c=0;c<3;c++) 
        {
          xm = fminf(xm, pi[0]); xM = fmaxf(xM, pi[0]);
          ym = fminf(ym, pi[1]); yM = fmaxf(yM, pi[1]);
          pi+=2;
        }
      }
    }
    roi_in->x = fmaxf(0.0f, xm); roi_in->y = fmaxf(0.0f, ym);
    roi_in->width = fminf(orig_w-roi_in->x, xM - roi_in->x + 10); roi_in->height = fminf(orig_h-roi_in->y, yM - roi_in->y + 10);
  }
  lf_modifier_destroy(modifier);
}

void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_lensfun_params_t *p = (dt_iop_lensfun_params_t *)p1;
#ifdef HAVE_GEGL
  // pull in new params to gegl
#error "lensfun needs to be ported to GEGL!"
#else
  dt_iop_lensfun_data_t *d = (dt_iop_lensfun_data_t *)piece->data;

  lfDatabase *dt_iop_lensfun_db = (lfDatabase *)self->data;
  const lfCamera *camera = NULL;
  const lfCamera **cam = NULL;
  if(p->camera[0])
  {
    pthread_mutex_lock(&darktable.plugin_threadsafe);
    cam = lf_db_find_cameras_ext(dt_iop_lensfun_db,
        NULL, p->camera, 0);
    if(cam) camera = cam[0];
    pthread_mutex_unlock(&darktable.plugin_threadsafe);
  }
  if(p->lens[0])
  {
    pthread_mutex_lock(&darktable.plugin_threadsafe);
    const lfLens **lens = lf_db_find_lenses_hd(dt_iop_lensfun_db, camera, NULL,
        p->lens, 0);
    pthread_mutex_unlock(&darktable.plugin_threadsafe);
    if(lens)
    {
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

void init(dt_iop_module_t *module)
{
  pthread_mutex_lock(&darktable.plugin_threadsafe);
  lfDatabase *dt_iop_lensfun_db = lf_db_new();
  module->data = (void *)dt_iop_lensfun_db;
  if(lf_db_load(dt_iop_lensfun_db) != LF_NO_ERROR)
  {
    char path[1024];
    dt_get_datadir(path, 1024);
    char *c = path + strlen(path);
    for(;c>path && *c != '/';c--);
    sprintf(c, "/lensfun");
    dt_iop_lensfun_db->HomeDataDir = path;
    if(lf_db_load(dt_iop_lensfun_db) != LF_NO_ERROR)
    {
      fprintf(stderr, "[iop_lens]: could not load lensfun database!\n");
    }
  }
  pthread_mutex_unlock(&darktable.plugin_threadsafe);
  module->params = malloc(sizeof(dt_iop_lensfun_params_t));
  module->default_params = malloc(sizeof(dt_iop_lensfun_params_t));
  module->default_enabled = 0;
  module->params_size = sizeof(dt_iop_lensfun_params_t);
  module->gui_data = NULL;
  module->priority = 940;
  // get all we can from exif:
  dt_iop_lensfun_params_t tmp;
  strncpy(tmp.lens, module->dev->image->exif_lens, 52);
  strncpy(tmp.camera, module->dev->image->exif_model, 52);
  tmp.crop     = module->dev->image->exif_crop;
  tmp.aperture = module->dev->image->exif_aperture;
  tmp.focal    = module->dev->image->exif_focal_length;
  tmp.scale    = 1.0;
  tmp.inverse  = 0;
  tmp.modify_flags = LF_MODIFY_TCA | LF_MODIFY_VIGNETTING |
    LF_MODIFY_DISTORTION | LF_MODIFY_GEOMETRY | LF_MODIFY_SCALE;
  tmp.distance = 5.0;
  tmp.target_geom = LF_RECTILINEAR;

  // init crop from db:
  dt_image_t *img = module->dev->image;
  char model[100];  // truncate often complex descriptions.
  strncpy(model, img->exif_model, 100);
  for(char cnt = 0, *c = model; c < model+100 && *c != '\0'; c++) if(*c == ' ') if(++cnt == 2) *c = '\0';
  if(img->exif_maker[0] || model[0])
  {
    pthread_mutex_lock(&darktable.plugin_threadsafe);
    const lfCamera **cam = lf_db_find_cameras_ext(dt_iop_lensfun_db,
        img->exif_maker, img->exif_model, 0);
    pthread_mutex_unlock(&darktable.plugin_threadsafe);
    if(cam)
    {
      img->exif_crop = tmp.crop = cam[0]->CropFactor;
      lf_free(cam);
    }
  }

  memcpy(module->params, &tmp, sizeof(dt_iop_lensfun_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_lensfun_params_t));
}

void cleanup(dt_iop_module_t *module)
{
  lfDatabase *dt_iop_lensfun_db = (lfDatabase *)module->data;
  lf_db_destroy(dt_iop_lensfun_db);
  free(module->gui_data);
  module->gui_data = NULL;
  free(module->params);
  module->params = NULL;
}








/// ############################################################
/// gui stuff: inspired by ufraws lensfun tab:


static GtkComboBoxEntry *combo_entry_text (
    GtkWidget *container, guint x, guint y, gchar *lbl, gchar *tip)
{
  GtkWidget *label, *combo;

  (void)label;

  combo = gtk_combo_box_entry_new_text ();
  if (GTK_IS_TABLE (container))
    gtk_table_attach (GTK_TABLE (container), combo, x+1, x+2, y, y+1, 0, 0, 2, 0);
  else if (GTK_IS_BOX (container))
    gtk_box_pack_start (GTK_BOX (container), combo, TRUE, TRUE, 2);
  gtk_object_set(GTK_OBJECT(combo), "tooltip-text", tip, NULL);

  return GTK_COMBO_BOX_ENTRY (combo);
}

/* simple function to compute the floating-point precision
   which is enough for "normal use". The criteria is to have
   about 3 leading digits after the initial zeros.  */
static int precision (double x, double adj)
{
  x *= adj;
  if (x < 1.0)
    if (x < 0.1)
      if (x < 0.01)
        return 5;
      else
        return 4;
    else
      return 3;
  else
    if (x < 100.0)
      if (x < 10.0)
        return 2;
      else
        return 1;
    else
      return 0;
}

static GtkComboBoxEntry *combo_entry_numeric (
    GtkWidget *container, guint x, guint y, gchar *lbl, gchar *tip,
    gdouble val, gdouble precadj, gdouble *values, int nvalues)
{
  int i;
  char txt [30];
  GtkEntry *entry;
  GtkComboBoxEntry *combo;

  combo = combo_entry_text (container, x, y, lbl, tip);
  entry = GTK_ENTRY (GTK_BIN (combo)->child);

  gtk_entry_set_width_chars (entry, 4);

  snprintf (txt, sizeof (txt), "%.*f", precision (val, precadj), val);
  gtk_entry_set_text (entry, txt);

  for (i = 0; i < nvalues; i++)
  {
    gdouble v = values [i];
    snprintf (txt, sizeof (txt), "%.*f", precision (v, precadj), v);
    gtk_combo_box_append_text (GTK_COMBO_BOX (combo), txt);
  }

  return combo;
}

static GtkComboBoxEntry *combo_entry_numeric_log (
    GtkWidget *container, guint x, guint y, gchar *lbl, gchar *tip,
    gdouble val, gdouble min, gdouble max, gdouble step, gdouble precadj)
{
  int phase, nvalues;
  gdouble *values = NULL;
  for (phase = 0; phase < 2; phase++)
  {
    nvalues = 0;
    gboolean done = FALSE;
    gdouble v = min;
    while (!done)
    {
      if (v > max)
      {
        v = max;
        done = TRUE;
      }

      if (values)
        values [nvalues++] = v;
      else
        nvalues++;

      v *= step;
    }
    if (!values)
      values = g_new (gdouble, nvalues);
  }

  GtkComboBoxEntry *cbe = combo_entry_numeric (
      container, x, y, lbl, tip, val, precadj, values, nvalues);
  g_free (values);
  return cbe;
}

/* -- ufraw ptr array functions -- */

static int ptr_array_insert_sorted (
    GPtrArray *array, const void *item, GCompareFunc compare)
{
  int length = array->len;
  g_ptr_array_set_size (array, length + 1);
  const void **root = (const void **)array->pdata;

  int m = 0, l = 0, r = length - 1;

  // Skip trailing NULL, if any
  if (l <= r && !root [r])
    r--;

  while (l <= r)
  {
    m = (l + r) / 2;
    int cmp = compare (root [m], item);

    if (cmp == 0)
    {
      ++m;
      goto done;
    }
    else if (cmp < 0)
      l = m + 1;
    else
      r = m - 1;
  }
  if (r == m)
    m++;

done:
  memmove (root + m + 1, root + m, (length - m) * sizeof (void *));
  root [m] = item;
  return m;
}

static int ptr_array_find_sorted (
    const GPtrArray *array, const void *item, GCompareFunc compare)
{
  int length = array->len;
  void **root = array->pdata;

  int l = 0, r = length - 1;
  int m = 0, cmp = 0;

  if (!length)
    return -1;

  // Skip trailing NULL, if any
  if (!root [r])
    r--;

  while (l <= r)
  {
    m = (l + r) / 2;
    cmp = compare (root [m], item);

    if (cmp == 0)
      return m;
    else if (cmp < 0)
      l = m + 1;
    else
      r = m - 1;
  }

  return -1;
}

static void ptr_array_insert_index (
    GPtrArray *array, const void *item, int index)
{
  const void **root;
  int length = array->len;
  g_ptr_array_set_size (array, length + 1);
  root = (const void **)array->pdata;
  memmove (root + index + 1, root + index, (length - index) * sizeof (void *));
  root [index] = item;
}

/* -- end ufraw ptr array functions -- */

/* -- camera -- */

static void camera_set (dt_iop_module_t *self, const lfCamera *cam)
{
  dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)self->gui_data;
  dt_iop_lensfun_params_t *p = (dt_iop_lensfun_params_t *)self->params;
  gchar *fm;
  const char *maker, *model, *variant;
  char _variant [100];

  strncpy(p->camera, cam->Model, 52);
  g->camera = cam;
  if (!cam)
  {
    gtk_entry_set_text(GTK_ENTRY(g->camera_model), "");
    gtk_object_set(GTK_OBJECT(g->camera_model), "tooltip-text", "", NULL);
    return;
  }

  maker = lf_mlstr_get (cam->Maker);
  model = lf_mlstr_get (cam->Model);
  variant = lf_mlstr_get (cam->Variant);

  if (model)
  {
    if (maker)
      fm = g_strdup_printf ("%s, %s", maker, model);
    else
      fm = g_strdup_printf ("%s", model);
    gtk_entry_set_text (GTK_ENTRY (g->camera_model), fm);
    g_free (fm);
  }

  if (variant)
    snprintf (_variant, sizeof (_variant), " (%s)", variant);
  else
    _variant [0] = 0;

  fm = g_strdup_printf (_("maker:\t\t%s\n"
        "model:\t\t%s%s\n"
        "mount:\t\t%s\n"
        "crop factor:\t%.1f"),
      maker, model, _variant,
      cam->Mount, cam->CropFactor);
  gtk_object_set(GTK_OBJECT(g->camera_model), "tooltip-text", fm, NULL);
  g_free (fm);
}

static void camera_menu_select (
    GtkMenuItem *menuitem, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  camera_set (self, (lfCamera *)g_object_get_data(G_OBJECT(menuitem), "lfCamera"));
  if(!darktable.gui->reset) dt_dev_add_history_item(darktable.develop, self);
}

static void camera_menu_fill (dt_iop_module_t *self, const lfCamera *const *camlist)
{
  dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)self->gui_data;
  unsigned i;
  GPtrArray *makers, *submenus;

  if (g->camera_menu)
  {
    gtk_widget_destroy (GTK_WIDGET(g->camera_menu));
    g->camera_menu = NULL;
  }

  /* Count all existing camera makers and create a sorted list */
  makers = g_ptr_array_new ();
  submenus = g_ptr_array_new ();
  for (i = 0; camlist [i]; i++)
  {
    GtkWidget *submenu, *item;
    const char *m = lf_mlstr_get (camlist [i]->Maker);
    int idx = ptr_array_find_sorted (makers, m, (GCompareFunc)g_utf8_collate);
    if (idx < 0)
    {
      /* No such maker yet, insert it into the array */
      idx = ptr_array_insert_sorted (makers, m, (GCompareFunc)g_utf8_collate);
      /* Create a submenu for cameras by this maker */
      submenu = gtk_menu_new ();
      ptr_array_insert_index (submenus, submenu, idx);
    }

    submenu = g_ptr_array_index (submenus, idx);
    /* Append current camera name to the submenu */
    m = lf_mlstr_get (camlist [i]->Model);
    if (!camlist [i]->Variant)
      item = gtk_menu_item_new_with_label (m);
    else
    {
      gchar *fm = g_strdup_printf ("%s (%s)", m, camlist [i]->Variant);
      item = gtk_menu_item_new_with_label (fm);
      g_free (fm);
    }
    gtk_widget_show (item);
    g_object_set_data(G_OBJECT(item), "lfCamera", (void *)camlist [i]);
    g_signal_connect(G_OBJECT(item), "activate",
        G_CALLBACK(camera_menu_select), self);
    gtk_menu_shell_append (GTK_MENU_SHELL (submenu), item);
  }

  g->camera_menu = GTK_MENU(gtk_menu_new ());
  for (i = 0; i < makers->len; i++)
  {
    GtkWidget *item = gtk_menu_item_new_with_label (g_ptr_array_index (makers, i));
    gtk_widget_show (item);
    gtk_menu_shell_append (GTK_MENU_SHELL (g->camera_menu), item);
    gtk_menu_item_set_submenu (
        GTK_MENU_ITEM (item), (GtkWidget *)g_ptr_array_index (submenus, i));
  }

  g_ptr_array_free (submenus, TRUE);
  g_ptr_array_free (makers, TRUE);
}

static void parse_maker_model (
    const char *txt, char *make, size_t sz_make, char *model, size_t sz_model)
{
  const gchar *sep;

  while (txt [0] && isspace (txt [0]))
    txt++;
  sep = strchr (txt, ',');
  if (sep)
  {
    size_t len = sep - txt;
    if (len > sz_make - 1)
      len = sz_make - 1;
    memcpy (make, txt, len);
    make [len] = 0;

    while (*++sep && isspace (sep [0]))
      ;
    len = strlen (sep);
    if (len > sz_model - 1)
      len = sz_model - 1;
    memcpy (model, sep, len);
    model [len] = 0;
  }
  else
  {
    size_t len = strlen (txt);
    if (len > sz_model - 1)
      len = sz_model - 1;
    memcpy (model, txt, len);
    model [len] = 0;
    make [0] = 0;
  }
}

static void camera_search_clicked(
    GtkWidget *button, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  lfDatabase *dt_iop_lensfun_db = (lfDatabase *)self->data;
  dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)self->gui_data;
  char make [200], model [200];
  const gchar *txt = gtk_entry_get_text(GTK_ENTRY(g->camera_model));

  (void)button;

  if(txt[0] == '\0')
  {
    const lfCamera *const *camlist;
    pthread_mutex_lock(&darktable.plugin_threadsafe);
    camlist = lf_db_get_cameras (dt_iop_lensfun_db);
    pthread_mutex_unlock(&darktable.plugin_threadsafe);
    if (!camlist) return;
    camera_menu_fill (self, camlist);
  }
  else
  {
    parse_maker_model (txt, make, sizeof (make), model, sizeof (model));
    pthread_mutex_lock(&darktable.plugin_threadsafe);
    const lfCamera **camlist = lf_db_find_cameras_ext (dt_iop_lensfun_db, make, model, 0);
    pthread_mutex_unlock(&darktable.plugin_threadsafe);
    if (!camlist) return;
    camera_menu_fill (self, camlist);
    lf_free (camlist);
  }

  gtk_menu_popup (GTK_MENU (g->camera_menu), NULL, NULL, NULL, NULL,
      0, gtk_get_current_event_time ());
}

/* -- end camera -- */

static void lens_comboentry_focal_update (GtkComboBox *widget, dt_iop_module_t *self)
{
  dt_iop_lensfun_params_t *p = (dt_iop_lensfun_params_t *)self->params;
  (void)sscanf (gtk_combo_box_get_active_text (widget), "%f", &p->focal);
  if(!darktable.gui->reset) dt_dev_add_history_item(darktable.develop, self);
}

static void lens_comboentry_aperture_update (GtkComboBox *widget, dt_iop_module_t *self)
{
  dt_iop_lensfun_params_t *p = (dt_iop_lensfun_params_t *)self->params;
  (void)sscanf (gtk_combo_box_get_active_text (widget), "%f", &p->aperture);
  if(!darktable.gui->reset) dt_dev_add_history_item(darktable.develop, self);
}

static void lens_comboentry_distance_update (GtkComboBox *widget, dt_iop_module_t *self)
{
  dt_iop_lensfun_params_t *p = (dt_iop_lensfun_params_t *)self->params;
  (void)sscanf (gtk_combo_box_get_active_text (widget), "%f", &p->distance);
  if(!darktable.gui->reset) dt_dev_add_history_item(darktable.develop, self);
}

static void delete_children (GtkWidget *widget, gpointer data)
{
  (void)data;
  gtk_widget_destroy (widget);
}

static void lens_set (dt_iop_module_t *self, const lfLens *lens)
{
  dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)self->gui_data;
  dt_iop_lensfun_params_t *p = (dt_iop_lensfun_params_t *)self->params;
  gchar *fm;
  const char *maker, *model;
  unsigned i;
  GtkComboBoxEntry *cbe;
  static gdouble focal_values [] =
  {
    4.5, 8, 10, 12, 14, 15, 16, 17, 18, 20, 24, 28, 30, 31, 35, 38, 40, 43,
    45, 50, 55, 60, 70, 75, 77, 80, 85, 90, 100, 105, 110, 120, 135,
    150, 200, 210, 240, 250, 300, 400, 500, 600, 800, 1000
  };
  static gdouble aperture_values [] =
  {
    1, 1.2, 1.4, 1.7, 2, 2.4, 2.8, 3.4, 4, 4.8, 5.6, 6.7,
    8, 9.5, 11, 13, 16, 19, 22, 27, 32, 38
  };

  if (!lens)
  {
    gtk_entry_set_text(GTK_ENTRY(g->lens_model), "");
    gtk_object_set(GTK_OBJECT(g->lens_model), "tooltip-text", "", NULL);
    return;
  }

  maker = lf_mlstr_get (lens->Maker);
  model = lf_mlstr_get (lens->Model);

  strncpy(p->lens, model, 52);

  if (model)
  {
    if (maker)
      fm = g_strdup_printf ("%s, %s", maker, model);
    else
      fm = g_strdup_printf ("%s", model);
    gtk_entry_set_text (GTK_ENTRY (g->lens_model), fm);
    g_free (fm);
  }

  char focal [100], aperture [100], mounts [200];

  if (lens->MinFocal < lens->MaxFocal)
    snprintf (focal, sizeof (focal), "%g-%gmm", lens->MinFocal, lens->MaxFocal);
  else
    snprintf (focal, sizeof (focal), "%gmm", lens->MinFocal);
  if (lens->MinAperture < lens->MaxAperture)
    snprintf (aperture, sizeof (aperture), "%g-%g", lens->MinAperture, lens->MaxAperture);
  else
    snprintf (aperture, sizeof (aperture), "%g", lens->MinAperture);

  mounts [0] = 0;
  if (lens->Mounts)
    for (i = 0; lens->Mounts [i]; i++)
    {
      if (i > 0)
        g_strlcat (mounts, ", ", sizeof (mounts));
      g_strlcat (mounts, lens->Mounts [i], sizeof (mounts));
    }

  fm = g_strdup_printf (_("maker:\t\t%s\n"
        "model:\t\t%s\n"
        "focal range:\t%s\n"
        "aperture:\t\t%s\n"
        "crop factor:\t%.1f\n"
        "type:\t\t%s\n"
        "mounts:\t\t%s"),
      maker ? maker : "?", model ? model : "?",
      focal, aperture, lens->CropFactor,
      lf_get_lens_type_desc (lens->Type, NULL), mounts);
  gtk_object_set(GTK_OBJECT(g->lens_model), "tooltip-text", fm, NULL);
  g_free (fm);

  /* Create the focal/aperture/distance combo boxes */
  gtk_container_foreach (
      GTK_CONTAINER (g->lens_param_box), delete_children, NULL);

  int ffi = 0, fli = -1;
  for (i = 0; i < sizeof (focal_values) / sizeof (gdouble); i++)
  {
    if (focal_values [i] < lens->MinFocal)
      ffi = i + 1;
    if (focal_values [i] > lens->MaxFocal && fli == -1)
      fli = i;
  }
  if (lens->MaxFocal == 0 || fli < 0)
    fli = sizeof (focal_values) / sizeof (gdouble);
  if (fli < ffi)
    fli = ffi + 1;
  cbe = combo_entry_numeric (
      g->lens_param_box, 0, 0, _("mm"), _("focal length (mm)"),
      p->focal, 10.0, focal_values + ffi, fli - ffi);
  g_signal_connect (G_OBJECT(cbe), "changed",
      G_CALLBACK(lens_comboentry_focal_update), self);

  ffi = 0;
  for (i = 0; i < sizeof (aperture_values) / sizeof (gdouble); i++)
    if (aperture_values [i] < lens->MinAperture)
      ffi = i + 1;
  cbe = combo_entry_numeric (
      g->lens_param_box, 0, 0, _("f/"), _("f-number (aperture)"),
      p->aperture, 10.0, aperture_values + ffi, sizeof (aperture_values) / sizeof (gdouble) - ffi);
  g_signal_connect (G_OBJECT(cbe), "changed",
      G_CALLBACK(lens_comboentry_aperture_update), self);

  cbe = combo_entry_numeric_log (
      g->lens_param_box, 0, 0, _("d"), _("distance to subject"),
      p->distance, 0.25, 1000, 1.41421356237309504880, 10.0);
  g_signal_connect (G_OBJECT(cbe), "changed",
      G_CALLBACK(lens_comboentry_distance_update), self);

  gtk_widget_show_all (g->lens_param_box);

  // TODO: autoscale!
}

static void lens_menu_select (
    GtkMenuItem *menuitem, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  lens_set (self, (lfLens *)g_object_get_data(G_OBJECT(menuitem), "lfLens"));
  if(!darktable.gui->reset) dt_dev_add_history_item(darktable.develop, self);
}

static void lens_menu_fill (
    dt_iop_module_t *self, const lfLens *const *lenslist)
{
  dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)self->gui_data;
  unsigned i;
  GPtrArray *makers, *submenus;

  if (g->lens_menu)
  {
    gtk_widget_destroy (GTK_WIDGET(g->lens_menu));
    g->lens_menu = NULL;
  }

  /* Count all existing lens makers and create a sorted list */
  makers = g_ptr_array_new ();
  submenus = g_ptr_array_new ();
  for (i = 0; lenslist [i]; i++)
  {
    GtkWidget *submenu, *item;
    const char *m = lf_mlstr_get (lenslist [i]->Maker);
    int idx = ptr_array_find_sorted (makers, m, (GCompareFunc)g_utf8_collate);
    if (idx < 0)
    {
      /* No such maker yet, insert it into the array */
      idx = ptr_array_insert_sorted (makers, m, (GCompareFunc)g_utf8_collate);
      /* Create a submenu for lenses by this maker */
      submenu = gtk_menu_new ();
      ptr_array_insert_index (submenus, submenu, idx);
    }

    submenu = g_ptr_array_index (submenus, idx);
    /* Append current lens name to the submenu */
    item = gtk_menu_item_new_with_label (lf_mlstr_get (lenslist [i]->Model));
    gtk_widget_show (item);
    g_object_set_data(G_OBJECT(item), "lfLens", (void *)lenslist [i]);
    g_signal_connect(G_OBJECT(item), "activate",
        G_CALLBACK(lens_menu_select), self);
    gtk_menu_shell_append (GTK_MENU_SHELL (submenu), item);
  }

  g->lens_menu = GTK_MENU(gtk_menu_new ());
  for (i = 0; i < makers->len; i++)
  {
    GtkWidget *item = gtk_menu_item_new_with_label (g_ptr_array_index (makers, i));
    gtk_widget_show (item);
    gtk_menu_shell_append (GTK_MENU_SHELL (g->lens_menu), item);
    gtk_menu_item_set_submenu (
        GTK_MENU_ITEM (item), (GtkWidget *)g_ptr_array_index (submenus, i));
  }

  g_ptr_array_free (submenus, TRUE);
  g_ptr_array_free (makers, TRUE);
}

static void lens_search_clicked(
    GtkWidget *button, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  lfDatabase *dt_iop_lensfun_db = (lfDatabase *)self->data;
  dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)self->gui_data;
  const lfLens **lenslist;
  char make [200], model [200];
  const gchar *txt = gtk_entry_get_text(GTK_ENTRY(g->lens_model));

  (void)button;

  if(txt[0] != '\0')
  {
    parse_maker_model (txt, make, sizeof (make), model, sizeof (model));
    pthread_mutex_lock(&darktable.plugin_threadsafe);
    lenslist = lf_db_find_lenses_hd (dt_iop_lensfun_db, g->camera,
        make [0] ? make : NULL,
        model [0] ? model : NULL, 0);
    pthread_mutex_unlock(&darktable.plugin_threadsafe);
    if (!lenslist) return;
    lens_menu_fill (self, lenslist);
    lf_free (lenslist);
  }
  else
  {
    pthread_mutex_lock(&darktable.plugin_threadsafe);
    const lfLens *const *lenslist = lf_db_get_lenses (dt_iop_lensfun_db);
    pthread_mutex_unlock(&darktable.plugin_threadsafe);
    if (!lenslist) return;
    lens_menu_fill (self, lenslist);
  }

  gtk_menu_popup (GTK_MENU (g->lens_menu), NULL, NULL, NULL, NULL,
      0, gtk_get_current_event_time ());
}

/* -- end lens -- */

static void target_geometry_changed (GtkComboBox *widget, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_lensfun_params_t *p = (dt_iop_lensfun_params_t *)self->params;

  int pos = gtk_combo_box_get_active(widget);
  p->target_geom = pos + LF_UNKNOWN + 1;
  if(darktable.gui->reset) return;
  dt_dev_add_history_item(darktable.develop, self);
}

static void reverse_toggled(GtkToggleButton *togglebutton, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_lensfun_params_t *p = (dt_iop_lensfun_params_t *)self->params;
  if(gtk_toggle_button_get_active(togglebutton)) p->inverse = 1;
  else p->inverse = 0;
  if(darktable.gui->reset) return;
  dt_dev_add_history_item(darktable.develop, self);
}

static void scale_changed(GtkRange *range, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_lensfun_params_t *p = (dt_iop_lensfun_params_t *)self->params;
  if(darktable.gui->reset) return;
  p->scale = gtk_range_get_value(range);
  dt_dev_add_history_item(darktable.develop, self);
}

static float get_autoscale(dt_iop_module_t *self)
{
  dt_iop_lensfun_params_t   *p = (dt_iop_lensfun_params_t   *)self->params;
  dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)self->gui_data;
  lfDatabase *dt_iop_lensfun_db = (lfDatabase *)self->data;
  float scale = 1.0;
  if(p->lens[0] != '\0')
  {
    char make [200], model [200];
    const gchar *txt = gtk_entry_get_text(GTK_ENTRY(g->lens_model));
    parse_maker_model (txt, make, sizeof (make), model, sizeof (model));
    pthread_mutex_lock(&darktable.plugin_threadsafe);
    const lfLens **lenslist = lf_db_find_lenses_hd (dt_iop_lensfun_db, g->camera, NULL, p->lens, 0);
    if(lenslist && !lenslist[1]) 
    {
      // create dummy modifier
      lfModifier *modifier = lf_modifier_new(lenslist[0], p->crop, self->dev->image->width, self->dev->image->height);
      (void)lf_modifier_initialize(
          modifier, lenslist[0], LF_PF_F32,
          p->focal, p->aperture,
          p->distance, p->scale,
          p->target_geom, p->modify_flags, p->inverse);
      scale = lf_modifier_get_auto_scale (modifier, p->inverse);
      lf_modifier_destroy(modifier);
    }
    lf_free (lenslist);
    pthread_mutex_unlock(&darktable.plugin_threadsafe);
  }
  return scale;
}

static void autoscale_pressed(GtkWidget *button, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  const float scale = get_autoscale(self);
  dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)self->gui_data;
  gtk_range_set_value(GTK_RANGE(g->scale), scale);
}

void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_lensfun_gui_data_t));
  lfDatabase *dt_iop_lensfun_db = (lfDatabase *)self->data;
  dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)self->gui_data;
  dt_iop_lensfun_params_t *p = (dt_iop_lensfun_params_t *)self->params;
  g->camera = NULL;
  g->camera_menu = NULL;
  g->lens_menu = NULL;

  GtkWidget *button;
  GtkWidget *label;
  GtkWidget *vbox1, *vbox2, *hbox;

  self->widget = gtk_vbox_new(FALSE, 2);
  hbox  = gtk_hbox_new(FALSE, 0);
  vbox1 = gtk_vbox_new(TRUE, 2);
  vbox2 = gtk_vbox_new(TRUE, 2);
  gtk_box_pack_start(GTK_BOX(self->widget), hbox, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(hbox), vbox1, FALSE, FALSE, 5);
  gtk_box_pack_start(GTK_BOX(hbox), vbox2, TRUE, TRUE, 5);

  // camera selector
  g->camera_model = GTK_ENTRY(gtk_entry_new());
  gtk_editable_set_editable(GTK_EDITABLE(g->camera_model), TRUE);
  gtk_box_pack_start(GTK_BOX(vbox1), GTK_WIDGET(g->camera_model), TRUE, TRUE, 0);
  gtk_entry_set_text(g->camera_model, self->dev->image->exif_model);

  button = gtk_button_new_with_label(_("cam"));
  gtk_object_set(GTK_OBJECT(button), "tooltip-text", _("search for camera using a pattern\n"
        "format: [maker, ][model]"), NULL);
  g_signal_connect (G_OBJECT (button), "clicked",
      G_CALLBACK (camera_search_clicked), self);
  gtk_box_pack_start(GTK_BOX(vbox2), button, TRUE, TRUE, 0);

  // lens selector
  g->lens_model = GTK_ENTRY(gtk_entry_new());
  gtk_editable_set_editable(GTK_EDITABLE(g->lens_model), TRUE);
  gtk_box_pack_start(GTK_BOX(vbox1), GTK_WIDGET(g->lens_model), TRUE, TRUE, 0);
  gtk_entry_set_text(g->lens_model, self->dev->image->exif_lens);

  button = gtk_button_new_with_label(_("lens"));
  gtk_object_set(GTK_OBJECT(button), "tooltip-text",
      _("search for lens using a pattern\n"
        "format: [maker, ][model]"), NULL);
  g_signal_connect (G_OBJECT (button), "clicked",
      G_CALLBACK (lens_search_clicked), self);
  gtk_box_pack_start(GTK_BOX(vbox2), button, TRUE, TRUE, 0);

  // lens properties
  g->lens_param_box = gtk_hbox_new(FALSE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->lens_param_box), TRUE, TRUE, 0);

  hbox  = gtk_hbox_new(FALSE, 0);
  vbox1 = gtk_vbox_new(TRUE, 2);
  vbox2 = gtk_vbox_new(TRUE, 2);
  gtk_box_pack_start(GTK_BOX(self->widget), hbox, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(hbox), vbox1, FALSE, FALSE, 5);
  gtk_box_pack_start(GTK_BOX(hbox), vbox2, TRUE, TRUE, 5);

  // if unambigious info is there, use it.
  if(self->dev->image->exif_lens[0] != '\0')
  {
    char make [200], model [200];
    const gchar *txt = gtk_entry_get_text(GTK_ENTRY(g->lens_model));
    parse_maker_model (txt, make, sizeof (make), model, sizeof (model));
    pthread_mutex_lock(&darktable.plugin_threadsafe);
    const lfLens **lenslist = lf_db_find_lenses_hd (dt_iop_lensfun_db, g->camera,
        make [0] ? make : NULL,
        model [0] ? model : NULL, 0);
    if(lenslist && !lenslist[1]) lens_set (self, lenslist[0]);
    lf_free (lenslist);
    pthread_mutex_unlock(&darktable.plugin_threadsafe);
  }

  // target geometry
  g->target_geom = GTK_COMBO_BOX(gtk_combo_box_new_text());
  gtk_object_set(GTK_OBJECT(g->target_geom), "tooltip-text",
      _("target geometry"), NULL);
  gtk_combo_box_append_text(g->target_geom, _("rectilinear"));
  gtk_combo_box_append_text(g->target_geom, _("fisheye"));
  gtk_combo_box_append_text(g->target_geom, _("panoramic"));
  gtk_combo_box_append_text(g->target_geom, _("equirectangular"));
  gtk_combo_box_set_active(g->target_geom, p->target_geom - LF_UNKNOWN - 1);
  g_signal_connect (G_OBJECT (g->target_geom), "changed",
      G_CALLBACK (target_geometry_changed),
      (gpointer)self);
  gtk_box_pack_start(GTK_BOX(vbox2), GTK_WIDGET(g->target_geom), TRUE, TRUE, 0);
  label = gtk_label_new(_("geometry"));
  gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
  gtk_box_pack_start(GTK_BOX(vbox1), label, TRUE, TRUE, 0);

  // scale
  g->scale = GTK_HSCALE(gtk_hscale_new_with_range(0.1, 2.0, 0.01));
  gtk_scale_set_digits(GTK_SCALE(g->scale), 2);
  gtk_scale_set_value_pos(GTK_SCALE(g->scale), GTK_POS_LEFT);
  gtk_range_set_value(GTK_RANGE(g->scale), p->scale);
  g_signal_connect (G_OBJECT (g->scale), "value-changed",
                    G_CALLBACK (scale_changed), self);
  hbox = gtk_hbox_new(FALSE, 0);
  button = gtk_button_new_with_label(_("auto"));
  g_signal_connect (G_OBJECT (button), "clicked",
                    G_CALLBACK (autoscale_pressed), self);
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(g->scale), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(hbox), button, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(vbox2), hbox, TRUE, TRUE, 0);
  // gtk_box_pack_start(GTK_BOX(vbox2), GTK_WIDGET(g->scale), TRUE, TRUE, 0);
  label = gtk_label_new(_("scale"));
  gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
  gtk_box_pack_start(GTK_BOX(vbox1), label, TRUE, TRUE, 0);

  // reverse direction
  g->reverse = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("reverse")));
  gtk_object_set(GTK_OBJECT(g->reverse), "tooltip-text", _("apply distortions instead of correcting them"), NULL);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->reverse), p->inverse);
  gtk_box_pack_start(GTK_BOX(vbox1), gtk_label_new(""), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(vbox2), GTK_WIDGET(g->reverse), TRUE, TRUE, 0);
  g_signal_connect (G_OBJECT (g->reverse), "toggled",
                    G_CALLBACK (reverse_toggled), self);
}


void gui_update(struct dt_iop_module_t *self)
{
  // let gui elements reflect params
  dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)self->gui_data;
  dt_iop_lensfun_params_t *p = (dt_iop_lensfun_params_t *)self->params;
  lfDatabase *dt_iop_lensfun_db = (lfDatabase *)self->data;
  gtk_entry_set_text(g->camera_model, p->camera);
  gtk_entry_set_text(g->lens_model, p->lens);
  gtk_combo_box_set_active(g->target_geom, p->target_geom - LF_UNKNOWN - 1);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->reverse), p->inverse);
  gtk_range_set_value(GTK_RANGE(g->scale), p->scale);
  const lfCamera **cam = NULL;
  g->camera = NULL;
  if(p->camera[0])
  {
    pthread_mutex_lock(&darktable.plugin_threadsafe);
    cam = lf_db_find_cameras_ext(dt_iop_lensfun_db,
        NULL, p->camera, 0);
    pthread_mutex_unlock(&darktable.plugin_threadsafe);
    if(cam) g->camera = cam[0];
  }
  if(p->lens[0])
  {
    char make [200], model [200];
    const gchar *txt = gtk_entry_get_text(GTK_ENTRY(g->lens_model));
    parse_maker_model (txt, make, sizeof (make), model, sizeof (model));
    pthread_mutex_lock(&darktable.plugin_threadsafe);
    const lfLens **lenslist = lf_db_find_lenses_hd (dt_iop_lensfun_db, g->camera,
        make [0] ? make : NULL,
        model [0] ? model : NULL, 0);
    if(lenslist && !lenslist[1]) lens_set (self, lenslist[0]);
    lf_free (lenslist);
    pthread_mutex_unlock(&darktable.plugin_threadsafe);
  }
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

