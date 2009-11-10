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

static lfDatabase *dt_iop_lensfun_db = NULL;

void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *i, void *o, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_lensfun_data_t *d = (dt_iop_lensfun_data_t *)piece->data;
  float *in  = (float *)i;
  float *out = (float *)o;

  lfModifier *modifier = lf_modifier_new( d->lens, d->crop, piece->iwidth, piece->iheight);

  // TODO: build these in gui somewhere:
  // float real_scale = powf(2.0, uf->conf->lens_scale);

  int modflags = lf_modifier_initialize(
      modifier, d->lens, LF_PF_F32,
      d->focal, d->aperture,
      d->distance, d->scale,
      d->target_geom, d->modify_flags, d->inverse);

  if(d->inverse)
  {
    // TODO: reverse direction (useful for renderings)
  }
  else
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
        // TODO: actually this way row stride does not matter. but give a buffer pointer
        //       offset by -roi_in.x * sizeof pixel :)
        if(lf_modifier_apply_color_modification (modifier,
              d->tmpbuf + 3*roi_in->width*y, 0.0, y,
              roi_in->width, 1, LF_CR_3 (RED, GREEN, BLUE), 0)) break;
      }
    }

    const size_t req2 = roi_in->width*2*3*sizeof(float);
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
              modifier, 0, y, roi_out->width, 1, d->tmpbuf2)) break;
        // TODO: reverse transform the global coords from lf to our buffer!
        const float *pi = d->tmpbuf2;
        for (int x = 0; x < roi_out->width; x++)
        {
          for(int c=0;c<3;c++) 
          {
            const int ii = (int)pi[0], jj = (int)pi[1];
            if(ii >= 0 || jj >= 0 || ii <= roi_in->width-2 || jj <= roi_in->height-2) 
            {
              const float fi = pi[0] - ii, fj = pi[1] - jj;
              out[c] = // in[3*(roi_in->width*jj + ii) + c];
              ((1.0f-fj)*(1.0f-fi)*in[3*(roi_in->width*(jj)   + (ii)  ) + c] +
               (1.0f-fj)*(     fi)*in[3*(roi_in->width*(jj)   + (ii+1)) + c] +
               (     fj)*(     fi)*in[3*(roi_in->width*(jj+1) + (ii+1)) + c] +
               (     fj)*(1.0f-fi)*in[3*(roi_in->width*(jj+1) + (ii)  ) + c]);
            }
            else for(int c=0;c<3;c++) out[c] = 0.0f;
            pi++;
          }
          out += 3;
        }
      }
    }
  }
  lf_modifier_destroy(modifier);
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

void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_lensfun_params_t *p = (dt_iop_lensfun_params_t *)p1;
#ifdef HAVE_GEGL
  // pull in new params to gegl
#error "lensfun needs to be ported to GEGL!"
#else
  dt_iop_lensfun_data_t *d = (dt_iop_lensfun_data_t *)piece->data;

  const lfCamera *camera = NULL;
  const lfCamera **cam = NULL;
  dt_image_t *img = self->dev->image;
  if(img->exif_maker[0] || img->exif_model[0])
  {
    cam = lf_db_find_cameras_ext(dt_iop_lensfun_db,
        img->exif_maker, img->exif_model, 0);
    if(cam) camera = cam[0];
  }
  if(p->lens[0])
  {
    const lfLens **lens = lf_db_find_lenses_hd(dt_iop_lensfun_db, camera, NULL,
        p->lens, 0);
    if(lens)
    {
      printf("lensfun found lens %s\n", lens[0]->Model);
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
  dt_iop_lensfun_db = lf_db_new ();
  if(lf_db_load(dt_iop_lensfun_db) != LF_NO_ERROR)
  {
    fprintf(stderr, "[iop_lens]: could not load lensfun database!\n");
  }
  // module->data = malloc(sizeof(dt_iop_lensfun_data_t));
  module->params = malloc(sizeof(dt_iop_lensfun_params_t));
  module->default_params = malloc(sizeof(dt_iop_lensfun_params_t));
  module->default_enabled = 0;
  module->params_size = sizeof(dt_iop_lensfun_params_t);
  module->gui_data = NULL;
  module->priority = 940;
  // get all we can from exif:
  dt_iop_lensfun_params_t tmp;
  strncpy(tmp.lens, module->dev->image->exif_lens, 52);
  tmp.crop     = module->dev->image->exif_crop;
  tmp.aperture = module->dev->image->exif_aperture;
  tmp.focal    = module->dev->image->exif_focal_length;
  tmp.scale    = 1.0;
  tmp.inverse  = 0;
  tmp.modify_flags = LF_MODIFY_TCA | LF_MODIFY_VIGNETTING |
    LF_MODIFY_DISTORTION | LF_MODIFY_GEOMETRY | LF_MODIFY_SCALE;
  tmp.distance = 5.0;
  tmp.target_geom = LF_RECTILINEAR;
  printf("exif returns crop %f\n", tmp.crop);

  // init crop from db:
  dt_image_t *img = module->dev->image;
  char model[100];  // truncate often complex descriptions.
  strncpy(model, img->exif_model, 100);
  for(char cnt = 0, *c = model; c < model+100 && *c != '\0'; c++) if(*c == ' ') if(++cnt == 2) *c = '\0';
  printf("lensfun searching for %s - %s\n", img->exif_maker, model);
  if(img->exif_maker[0] || model[0])
  {
    printf("...\n");
    // const lfCamera **cam = lf_db_find_cameras_ext(dt_iop_lensfun_db,
    // img->exif_maker, model, LF_SEARCH_LOOSE);
    const lfCamera **cam = lf_db_find_cameras_ext(dt_iop_lensfun_db,
        img->exif_maker, img->exif_model, 0);
    if(cam)
    {
      printf("lensfun db found %s - %s\n", cam[0]->Maker, cam[0]->Model);
      img->exif_crop = tmp.crop = cam[0]->CropFactor;
      lf_free(cam);
    }
  }

  printf("lensfun inited with crop %f\n", tmp.crop);

  memcpy(module->params, &tmp, sizeof(dt_iop_lensfun_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_lensfun_params_t));
}

void cleanup(dt_iop_module_t *module)
{
  lf_db_destroy(dt_iop_lensfun_db);
  free(module->gui_data);
  module->gui_data = NULL;
  free(module->params);
  module->params = NULL;
}








/// ############################################################
/// gui stuff: inspired by ufraws lensfun tab:


void gui_update(struct dt_iop_module_t *self)
{
  // dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)self->gui_data;
  // dt_iop_lensfun_params_t *p = (dt_iop_lensfun_params_t *)self->params;
  // gtk_range_set_value(GTK_RANGE(g->scale1), p->cx);
}

#if 0
GtkWidget *stock_image_button(const gchar *stock_id, GtkIconSize size,
    const char *tip, GCallback callback, void *data)
{
  GtkWidget *button;
  button = gtk_button_new();
  // gtk_container_add(GTK_CONTAINER(button), gtk_image_new_from_stock(stock_id, size));
  if (tip != NULL)
    gtk_object_set(GTK_OBJECT(button), "tooltip-text", tip, NULL);
  g_signal_connect(G_OBJECT(button), "clicked", callback, data);
  return button;
}

GtkWidget *stock_icon_button(const gchar *stock_id,
    const char *tip, GCallback callback, void *data)
{
  return stock_image_button(stock_id, GTK_ICON_SIZE_BUTTON,
      tip, callback, data);
}
#endif

/* -- ufraw ptr array functions -- */

int ptr_array_insert_sorted (
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

int ptr_array_find_sorted (
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

void ptr_array_insert_index (
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

  fm = g_strdup_printf (_("Maker:\t\t%s\n"
        "Model:\t\t%s%s\n"
        "Mount:\t\t%s\n"
        "Crop factor:\t%.1f"),
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
  dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)self->gui_data;
  const lfCamera **camlist;
  char make [200], model [200];
  const gchar *txt = gtk_entry_get_text(GTK_ENTRY(g->camera_model));

  (void)button;

  parse_maker_model (txt, make, sizeof (make), model, sizeof (model));

  // TODO: if txt == ""
  //   camlist = lf_db_get_cameras (dt_iop_lensfun_db);
  // and remove second button.
  camlist = lf_db_find_cameras_ext (dt_iop_lensfun_db, make, model, 0);
  if (!camlist)
    return;

  camera_menu_fill (self, camlist);
  lf_free (camlist);

  gtk_menu_popup (GTK_MENU (g->camera_menu), NULL, NULL, NULL, NULL,
      0, gtk_get_current_event_time ());
}

#if 0
static void camera_list_clicked(
    GtkWidget *button, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)self->gui_data;
  const lfCamera *const *camlist;

  (void)button;

  camlist = lf_db_get_cameras (dt_iop_lensfun_db);
  if (!camlist)
    return;

  camera_menu_fill (self, camlist);

  gtk_menu_popup (GTK_MENU (g->camera_menu), NULL, NULL, NULL, NULL,
      0, gtk_get_current_event_time ());
}
#endif

/* -- end camera -- */

#if 0
/* Update all lens model-related controls to reflect current model */
static void lens_update_controls (preview_data *data)
{
  gtk_combo_box_set_active (GTK_COMBO_BOX (data->LensDistortionModel),
      CFG->lens_distortion.Model - LF_DIST_MODEL_NONE);
  gtk_combo_box_set_active (GTK_COMBO_BOX (data->LensTCAModel),
      CFG->lens_tca.Model - LF_TCA_MODEL_NONE);
  gtk_combo_box_set_active (GTK_COMBO_BOX (data->LensVignettingModel),
      CFG->lens_vignetting.Model - LF_VIGNETTING_MODEL_NONE);
  gtk_combo_box_set_active (GTK_COMBO_BOX (data->LensFromGeometrySel),
      CFG->lens->Type);
  gtk_combo_box_set_active (GTK_COMBO_BOX (data->LensToGeometrySel),
      CFG->cur_lens_type);
}
#endif

#if 0
static void lens_interpolate (preview_data *data, const lfLens *lens)
{
  lfDistortionModel old_dist_model = CFG->lens_distortion.Model;
  lfTCAModel old_tca_model = CFG->lens_tca.Model;
  lfVignettingModel old_vignetting_model = CFG->lens_vignetting.Model;

  /* Interpolate all models and set the temp values accordingly */
  if (!lf_lens_interpolate_distortion (lens, CFG->focal_len, &CFG->lens_distortion))
    memset (&CFG->lens_distortion, 0, sizeof (CFG->lens_distortion));
  if (!lf_lens_interpolate_tca (lens, CFG->focal_len, &CFG->lens_tca))
    memset (&CFG->lens_tca, 0, sizeof (CFG->lens_tca));
  if (!lf_lens_interpolate_vignetting (lens, CFG->focal_len, CFG->aperture,
        CFG->subject_distance, &CFG->lens_vignetting))
    memset (&CFG->lens_vignetting, 0, sizeof (CFG->lens_vignetting));

  lens_update_controls (data);

  /* If the model does not change, the parameter sliders won't be updated.
   * To solve this, we'll call the "changed" callback manually.
   */
  if (CFG->lens_distortion.Model != LF_DIST_MODEL_NONE &&
      old_dist_model == CFG->lens_distortion.Model)
    g_signal_emit_by_name (GTK_COMBO_BOX (data->LensDistortionModel),
        "changed", NULL, NULL);
  if (CFG->lens_tca.Model != LF_TCA_MODEL_NONE &&
      old_tca_model == CFG->lens_tca.Model)
    g_signal_emit_by_name (GTK_COMBO_BOX (data->LensTCAModel),
        "changed", NULL, NULL);
  if (CFG->lens_vignetting.Model != LF_VIGNETTING_MODEL_NONE &&
      old_vignetting_model == CFG->lens_vignetting.Model)
    g_signal_emit_by_name (GTK_COMBO_BOX (data->LensVignettingModel),
        "changed", NULL, NULL);

  if (data->UF->postproc_ops & LF_MODIFY_VIGNETTING)
    preview_invalidate_layer (data, ufraw_develop_phase);
  else
    preview_invalidate_layer (data, ufraw_lensfun_phase);
  render_preview (data);
}
#endif

#if 0
static void lens_comboentry_update (GtkComboBox *widget, float *valuep)
{
  preview_data *data = get_preview_data (widget);
  if (sscanf (gtk_combo_box_get_active_text (widget), "%f", valuep) == 1)
    lens_interpolate (data, CFG->lens);
}
#endif

#if 0
static void delete_children (GtkWidget *widget, gpointer data)
{
  (void)data;
  gtk_widget_destroy (widget);
}
#endif

static void lens_set (dt_iop_module_t *self, const lfLens *lens)
{
  dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)self->gui_data;
  dt_iop_lensfun_params_t *p = (dt_iop_lensfun_params_t *)self->params;
  gchar *fm;
  const char *maker, *model;
  unsigned i;
#if 0
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
#endif

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

#if 0
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
      data->LensParamBox, 0, 0, _("focal"), _("focal length"),
      CFG->focal_len, 10.0, focal_values + ffi, fli - ffi);
  g_signal_connect (G_OBJECT(cbe), "changed",
      G_CALLBACK(lens_comboentry_update), &CFG->focal_len);

  ffi = 0;
  for (i = 0; i < sizeof (aperture_values) / sizeof (gdouble); i++)
    if (aperture_values [i] < lens->MinAperture)
      ffi = i + 1;
  cbe = combo_entry_numeric (
      data->LensParamBox, 0, 0, _("f"), _("f-number (aperture)"),
      CFG->aperture, 10.0, aperture_values + ffi, sizeof (aperture_values) / sizeof (gdouble) - ffi);
  g_signal_connect (G_OBJECT(cbe), "changed",
      G_CALLBACK(lens_comboentry_update), &CFG->aperture);

  cbe = combo_entry_numeric_log (
      data->LensParamBox, 0, 0, _("distance"), _("distance to subject"),
      CFG->subject_distance, 0.25, 1000, 1.41421356237309504880, 10.0);
  g_signal_connect (G_OBJECT(cbe), "changed",
      G_CALLBACK(lens_comboentry_update), &CFG->subject_distance);

  gtk_widget_show_all (g->lens_param_box);
#endif

  // CFG->cur_lens_type = LF_UNKNOWN;
  // CFG->lens_scale = 0.0;

  // lens_interpolate (data, lens);
}

static void lens_menu_select (
    GtkMenuItem *menuitem, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  lens_set (self, (lfLens *)g_object_get_data(G_OBJECT(menuitem), "lfLens"));
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
  dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)self->gui_data;
  const lfLens **lenslist;
  char make [200], model [200];
  const gchar *txt = gtk_entry_get_text(GTK_ENTRY(g->lens_model));

  (void)button;

  parse_maker_model (txt, make, sizeof (make), model, sizeof (model));
  lenslist = lf_db_find_lenses_hd (dt_iop_lensfun_db, g->camera,
      make [0] ? make : NULL,
      model [0] ? model : NULL, 0);
  if (!lenslist)
    return;

  lens_menu_fill (self, lenslist);
  lf_free (lenslist);

  gtk_menu_popup (GTK_MENU (g->lens_menu), NULL, NULL, NULL, NULL,
      0, gtk_get_current_event_time ());
}

#if 0
static void lens_list_clicked(
    GtkWidget *button, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)self->gui_data;

  (void)button;

  if (g->camera)
  {
    const lfLens **lenslist = lf_db_find_lenses_hd (
        dt_iop_lensfun_db, g->camera, NULL, NULL, 0);
    if (!lenslist)
      return;
    lens_menu_fill (self, lenslist);
    lf_free (lenslist);
  }
  else
  {
    const lfLens *const *lenslist = lf_db_get_lenses (dt_iop_lensfun_db);
    if (!lenslist)
      return;
    lens_menu_fill (self, lenslist);
  }

  gtk_menu_popup (GTK_MENU (g->lens_menu), NULL, NULL, NULL, NULL,
      0, gtk_get_current_event_time ());
}
#endif

#if 0
static void reset_adjustment_value (GtkWidget *widget, const lfParameter *param)
{
  GtkAdjustment *adj = (GtkAdjustment *)g_object_get_data (
      G_OBJECT (widget), "Adjustment");

  gtk_adjustment_set_value (adj, param->Default);
}

static GtkAdjustment *append_term (
    GtkWidget *table, int y, const lfParameter *param,
    float *term, GCallback callback)
{
  double step, page;
  double tmp = (param->Max - param->Min) / 100000.0;
  for (step = 0.00001; ; step *= 10.0)
    if (step >= tmp)
      break;

  tmp = (param->Max - param->Min) / 10.0;
  for (page = 0.00001; ; page *= 10.0)
    if (page >= tmp)
      break;

  long accuracy = 0;
  for (tmp = step; tmp < 1.0; tmp *= 10)
    accuracy++;

  GtkAdjustment *adj = adjustment_scale (
      GTK_TABLE (table), 0, y, param->Name, *term, term,
      param->Min, param->Max, step, page, accuracy, FALSE, NULL, callback,
      NULL, NULL, NULL);

  GtkWidget *button = stock_icon_button(GTK_STOCK_REFRESH, NULL,
      G_CALLBACK (reset_adjustment_value), (void *)param);
  gtk_table_attach (GTK_TABLE (table), button, 7, 8, y, y + 1, 0, 0, 0, 0);
  g_object_set_data (G_OBJECT(button), "Adjustment", adj);

  return adj;
}
#endif

/* -- end lens -- */


void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_lensfun_gui_data_t));
  dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)self->gui_data;
  // dt_iop_lensfun_params_t *p = (dt_iop_lensfun_params_t *)self->params;
  g->camera = NULL;
  g->camera_menu = NULL;
  g->lens_menu = NULL;

  GtkTable *table;//, *subTable;
  GtkWidget *button;

  table = GTK_TABLE(gtk_table_new(10, 10, FALSE));
  self->widget = GTK_WIDGET(table);

  /* Camera selector */
  // label = gtk_label_new(_("camera"));
  // gtk_misc_set_alignment(GTK_MISC(label), 1.0, 0.5);
  // gtk_table_attach(table, label, 0, 1, 0, 1, GTK_FILL, 0, 2, 0);

  g->camera_model = GTK_ENTRY(gtk_entry_new());
  gtk_editable_set_editable(GTK_EDITABLE(g->camera_model), TRUE);
  gtk_table_attach(table, GTK_WIDGET(g->camera_model), 1, 2, 0, 1,
      GTK_EXPAND|GTK_FILL, 0, 2, 0);

  // button = stock_icon_button(GTK_STOCK_FIND,
  //     _("search for camera using a pattern\n"
  //       "format: [maker, ][model]"),
  //     G_CALLBACK(camera_search_clicked), (gpointer)self);
  button = gtk_button_new_with_label(_("cam"));
  gtk_object_set(GTK_OBJECT(button), "tooltip-text", _("search for camera using a pattern\n"
        "format: [maker, ][model]"), NULL);
  g_signal_connect (G_OBJECT (button), "clicked",
      G_CALLBACK (camera_search_clicked), self);
  gtk_table_attach(table, button, 2, 3, 0, 1, GTK_EXPAND|GTK_FILL, 0, 0, 0);

  // button = stock_icon_button(GTK_STOCK_INDEX,
  //     _("choose camera from complete list"),
  //     G_CALLBACK(camera_list_clicked), (gpointer)self);
  // gtk_table_attach(table, button, 3, 4, 0, 1, 0, 0, 0, 0);

  /* Lens selector */
  // label = gtk_label_new(_("lens"));
  // gtk_misc_set_alignment(GTK_MISC(label), 1.0, 0.5);
  // gtk_table_attach(table, label, 0, 1, 1, 2, GTK_FILL, 0, 2, 0);

  g->lens_model = GTK_ENTRY(gtk_entry_new());
  gtk_editable_set_editable(GTK_EDITABLE(g->lens_model), TRUE);
  gtk_table_attach(table, GTK_WIDGET(g->lens_model), 1, 2, 1, 2,
      GTK_EXPAND|GTK_FILL, 0, 2, 0);

  // button = stock_icon_button(GTK_STOCK_FIND,
  //     _("search for lens using a pattern\n"
  //       "format: [maker, ][model]"),
  //     G_CALLBACK(lens_search_clicked), self);
  // gtk_table_attach(table, button, 2, 3, 1, 2, 0, 0, 0, 0);
  button = gtk_button_new_with_label(_("lens"));
  gtk_object_set(GTK_OBJECT(button), "tooltip-text",
       _("search for lens using a pattern\n"
         "format: [maker, ][model]"), NULL);
  g_signal_connect (G_OBJECT (button), "clicked",
      G_CALLBACK (lens_search_clicked), self);
  gtk_table_attach(table, button, 2, 3, 1, 2, GTK_EXPAND|GTK_FILL, 0, 0, 0);


  // button = stock_icon_button(GTK_STOCK_INDEX,
  //     _("choose lens from list of possible variants"),
  //     G_CALLBACK(lens_list_clicked), self);
  // gtk_table_attach(table, button, 3, 4, 1, 2, 0, 0, 0, 0);

#if 0
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
#endif
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

