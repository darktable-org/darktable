/*
    This file is part of darktable,
    Copyright (C) 2014-2021 darktable developers.

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

#include <glib.h>

#include "bauhaus/bauhaus.h"
#include "common/collection.h"
#include "common/colorspaces.h"
#include "common/cups_print.h"
#include "common/file_location.h"
#include "common/image_cache.h"
#include "common/metadata.h"
#include "common/pdf.h"
#include "common/printprof.h"
#include "common/printing.h"
#include "common/styles.h"
#include "common/tags.h"
#include "common/variables.h"
#include "control/jobs.h"
#include "dtgtk/resetlabel.h"
#include "gui/accelerators.h"
#include "gui/drag_and_drop.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include "libs/lib_api.h"

DT_MODULE(4)

const char *name(dt_lib_module_t *self)
{
  return _("print settings");
}

const char **views(dt_lib_module_t *self)
{
  static const char *v[] = {"print", NULL};
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_RIGHT_CENTER;
}

typedef enum _set_controls
{
  BOX_LEFT   = 1 << 0,
  BOX_RIGHT  = 1 << 1,
  BOX_TOP    = 1 << 2,
  BOX_BOTTOM = 1 << 3,
  BOX_TOP_LEFT     = BOX_LEFT | BOX_TOP,
  BOX_TOP_RIGHT    = BOX_RIGHT | BOX_TOP,
  BOX_BOTTOM_LEFT  = BOX_LEFT | BOX_BOTTOM,
  BOX_BOTTOM_RIGHT = BOX_RIGHT | BOX_BOTTOM,
  BOX_ALL          = BOX_LEFT | BOX_RIGHT | BOX_TOP | BOX_BOTTOM
} dt_box_control_set;

typedef enum _unit_t
{
  UNIT_MM = 0,
  UNIT_CM,
  UNIT_IN,
  UNIT_N // needs to be the last one
} _unit_t;


static const float units[UNIT_N] = { 1.0f, 0.1f, 1.0f/25.4f };
static const gchar *_unit_names[] = { N_("mm"), N_("cm"), N_("inch"), NULL };

typedef struct dt_lib_print_settings_t
{
  GtkWidget *profile, *intent, *style, *style_mode, *papers, *media;
  GtkWidget *printers, *orientation, *pprofile, *pintent;
  GtkWidget *width, *height, *black_point_compensation;
  GtkWidget *info;
  GtkWidget *b_x, *b_y;
  GtkWidget *b_width, *b_height;
  GtkWidget *del;
  GtkWidget *grid, *grid_size, *snap_grid;
  GtkWidget *borderless;
  GList *profiles;
  GtkButton *print_button;
  GtkToggleButton *lock_button;
  GtkWidget *b_top, *b_bottom, *b_left, *b_right;
  GtkDarktableToggleButton *dtba[9];	                                   // Alignment buttons
  GList *paper_list, *media_list;
  gboolean lock_activated;
  dt_print_info_t prt;
  dt_images_box imgs;
  _unit_t unit;
  int v_intent, v_pintent;
  int v_icctype, v_picctype;
  char *v_iccprofile, *v_piccprofile, *v_style;
  gboolean v_style_append, v_black_point_compensation;
  gboolean busy;

  // for adding new area
  gboolean creation;
  gboolean dragging;
  float x1, y1, x2, y2;
  int selected;                    // selected area in imgs.box
  int last_selected;               // last selected area to edit
  dt_box_control_set sel_controls; // which border/corner is selected
  float click_pos_x, click_pos_y;
  gboolean has_changed;
} dt_lib_print_settings_t;

typedef struct dt_lib_print_job_t
{
  gchar *job_title;
  dt_print_info_t prt;
  gchar* style;
  gboolean style_append, black_point_compensation;
  dt_colorspaces_color_profile_type_t buf_icc_type, p_icc_type;
  gchar *buf_icc_profile, *p_icc_profile;
  dt_iop_color_intent_t buf_icc_intent, p_icc_intent;
  dt_images_box imgs;
  uint16_t *buf; // ??? should be removed
  dt_pdf_page_t *pdf_page;
  char pdf_filename[PATH_MAX];
} dt_lib_print_job_t;

typedef struct dt_lib_export_profile_t
{
  dt_colorspaces_color_profile_type_t type; // filename is only used for type DT_COLORSPACE_FILE
  char filename[512];                       // icc file name
  char name[512];                           // product name
  int  pos, ppos;                           // position in combo boxen
} dt_lib_export_profile_t;

typedef struct _dialog_description
{
  const char *name;
} dialog_description_t;

static void _update_slider(dt_lib_print_settings_t *ps);
static void _width_changed(GtkWidget *widget, gpointer user_data);
static void _height_changed(GtkWidget *widget, gpointer user_data);
static void _x_changed(GtkWidget *widget, gpointer user_data);
static void _y_changed(GtkWidget *widget, gpointer user_data);

int
position()
{
  return 990;
}

/* get paper dimension for the orientation (in mm) */
static void _get_page_dimension(dt_print_info_t *prt, float *width, float *height)
{
  if(prt->page.landscape)
  {
    *width = prt->paper.height;
    *height = prt->paper.width;
  }
  else
  {
    *width = prt->paper.width;
    *height = prt->paper.height;
  }
}

static void _precision_by_unit(_unit_t unit, int *n_digits, float *incr, char **format)
{
  // this gives us these precisions
  //  unit  precision  increment
  //   mm      1          1
  //   cm      0.1        0.1
  //   in      0.01       0.05
  //
  // This allows for >= 1mm precision display regardless of unit, and
  // allows for entering common fractions (e.g. 1/4 as .25) for
  // inches. Increment is kept to 1mm except in the cases of inches,
  // where we round up from 0.03937 (1mm) to 0.05 to keep to a factor
  // of a power of ten.
  *n_digits = ceilf(log10f(1.0f / units[unit]));
  if(incr)
  {
    *incr = roundf(units[unit] * 20.0f) / 20.0f;
  }
  if(format)
  {
    *format = g_strdup_printf("%%.%df", *n_digits);
  }
}

// unit conversion

static float _to_mm(dt_lib_print_settings_t *ps, double value)
{
  return value / units[ps->unit];
}

// horizontal mm to pixels
static float _mm_to_hscreen(dt_lib_print_settings_t *ps, const float value, const gboolean offset)
{
  float width, height;
  _get_page_dimension(&ps->prt, &width, &height);

  return (offset ? ps->imgs.screen.page.x : 0)
    + (ps->imgs.screen.page.width * value / width);
}

// vertical mm to pixels
static float _mm_to_vscreen(dt_lib_print_settings_t *ps, const float value, const gboolean offset)
{
  float width, height;
  _get_page_dimension(&ps->prt, &width, &height);

  return (offset ? ps->imgs.screen.page.y : 0)
    + (ps->imgs.screen.page.height * value / height);
}

static float _hscreen_to_mm(dt_lib_print_settings_t *ps, const float value, const gboolean offset)
{
  float width, height;
  _get_page_dimension(&ps->prt, &width, &height);

  return width * (value - (offset ? ps->imgs.screen.page.x : 0.0f))
    / ps->imgs.screen.page.width;
}

static float _vscreen_to_mm(dt_lib_print_settings_t *ps, const float value, const gboolean offset)
{
  float width, height;
  _get_page_dimension(&ps->prt, &width, &height);

  return height * (value - (offset ? ps->imgs.screen.page.y : 0.0f))
    / ps->imgs.screen.page.height;
}


static inline float _percent_unit_of(dt_lib_print_settings_t *ps, float ref, float value)
{
  return value * ref * units[ps->unit];
}

// callbacks for in-memory export

typedef struct dt_print_format_t
{
  dt_imageio_module_data_t head;
  int bpp;
  dt_lib_print_job_t *params;
} dt_print_format_t;

static int bpp(dt_imageio_module_data_t *data)
{
  const dt_print_format_t *d = (dt_print_format_t *)data;
  return d->bpp;
}

static int levels(dt_imageio_module_data_t *data)
{
  const dt_print_format_t *d = (dt_print_format_t *)data;
  return IMAGEIO_RGB | (d->bpp == 8 ? IMAGEIO_INT8 : IMAGEIO_INT16);
}

static const char *mime(dt_imageio_module_data_t *data)
{
  return "memory";
}

static int write_image(dt_imageio_module_data_t *data, const char *filename, const void *in,
                       dt_colorspaces_color_profile_type_t over_type, const char *over_filename,
                       void *exif, int exif_len, int imgid, int num, int total, dt_dev_pixelpipe_t *pipe,
                       const gboolean export_masks)
{
  dt_print_format_t *d = (dt_print_format_t *)data;

  d->params->buf = (uint16_t *)malloc((size_t)3 * (d->bpp == 8?1:2) * d->head.width * d->head.height);

  if(d->bpp == 8)
  {
    const uint8_t *in_ptr = (const uint8_t *)in;
    uint8_t *out_ptr = (uint8_t *)d->params->buf;
    for(int y = 0; y < d->head.height; y++)
    {
      for(int x = 0; x < d->head.width; x++, in_ptr += 4, out_ptr += 3)
        memcpy(out_ptr, in_ptr, 3);
    }
  }
  else
  {
    const uint16_t *in_ptr = (const uint16_t *)in;
    uint16_t *out_ptr = (uint16_t *)d->params->buf;
    for(int y = 0; y < d->head.height; y++)
    {
      for(int x = 0; x < d->head.width; x++, in_ptr += 4, out_ptr += 3)
        memcpy(out_ptr, in_ptr, 6);
    }
  }

  return 0;
}

// export image imgid with given max_width & max_height, set iwidth & iheight with the
// final image size as exported.
static int _export_image(dt_job_t *job, dt_image_box *img)
{
  dt_lib_print_job_t *params = dt_control_job_get_params(job);

  dt_imageio_module_format_t buf;
  buf.mime = mime;
  buf.levels = levels;
  buf.bpp = bpp;
  buf.write_image = write_image;

  dt_print_format_t dat;
  dat.head.max_width = img->max_width;
  dat.head.max_height = img->max_height;
  dat.head.style[0] = '\0';
  dat.head.style_append = params->style_append;
  dat.bpp = *params->p_icc_profile ? 16 : 8; // set to 16bit when a profile is to be applied
  dat.params = params;

  if(params->style) g_strlcpy(dat.head.style, params->style, sizeof(dat.head.style));

  // let the user know something is happening
  dt_control_job_set_progress(job, 0.05);
  dt_control_log(_("processing `%s' for `%s'"), params->job_title, params->prt.printer.name);

  const gboolean high_quality = TRUE;
  const gboolean upscale = TRUE;
  const gboolean export_masks = FALSE;
  const gboolean is_scaling = FALSE;

  dt_imageio_export_with_flags
    (img->imgid, "unused", &buf, (dt_imageio_module_data_t *)&dat, TRUE, FALSE,
     high_quality, upscale, is_scaling, FALSE, NULL, FALSE, export_masks, params->buf_icc_type,
     params->buf_icc_profile, params->buf_icc_intent,  NULL, NULL, 1, 1, NULL);

  img->exp_width = dat.head.width;
  img->exp_height = dat.head.height;

  // we have the exported buffer, let's apply the printer profile

  const dt_colorspaces_color_profile_t *buf_profile =
    dt_colorspaces_get_output_profile(img->imgid,
                                      params->buf_icc_type,
                                      params->buf_icc_profile);
  if(*params->p_icc_profile)
  {
    const dt_colorspaces_color_profile_t *pprof =
      dt_colorspaces_get_profile(params->p_icc_type, params->p_icc_profile,
                                 DT_PROFILE_DIRECTION_OUT);
    if(!pprof)
    {
      dt_control_log(_("cannot open printer profile `%s'"), params->p_icc_profile);
      fprintf(stderr, "cannot open printer profile `%s'\n", params->p_icc_profile);
      dt_control_queue_redraw();
      return 1;
    }
    else
    {
      if(!buf_profile || !buf_profile->profile)
      {
        dt_control_log(_("error getting output profile for image %d"), img->imgid);
        fprintf(stderr, "error getting output profile for image %d\n", img->imgid);
        dt_control_queue_redraw();
        return 1;
      }
      if(dt_apply_printer_profile
         ((void **)&(params->buf), dat.head.width, dat.head.height, dat.bpp, buf_profile->profile,
          pprof->profile, params->p_icc_intent, params->black_point_compensation))
      {
        dt_control_log(_("cannot apply printer profile `%s'"), params->p_icc_profile);
        fprintf(stderr, "cannot apply printer profile `%s'\n", params->p_icc_profile);
        dt_control_queue_redraw();
        return 1;
      }
    }
  }

  img->buf = params->buf;
  params->buf = NULL;

  return 0;
}

static void _create_pdf(dt_job_t *job, dt_images_box imgs, const float width, const float height)
{
  dt_lib_print_job_t *params = dt_control_job_get_params(job);

  const float page_width  = dt_pdf_mm_to_point(width);
  const float page_height = dt_pdf_mm_to_point(height);
  const int icc_id = 0;

  dt_pdf_image_t *pdf_image[MAX_IMAGE_PER_PAGE];

  // create the PDF page
  dt_pdf_t *pdf = dt_pdf_start(params->pdf_filename, page_width, page_height,
                               params->prt.printer.resolution, DT_PDF_STREAM_ENCODER_FLATE);

/*
  // ??? should a profile be embedded here?
  if (*printer_profile)
    icc_id = dt_pdf_add_icc(pdf, printer_profile);
*/
  int32_t count = 0;

  for(int k=0; k<imgs.count; k++)
  {
    const int resolution = params->prt.printer.resolution;
    const dt_image_box *box = &imgs.box[k];

    if(box->imgid > -1)
    {
      pdf_image[count] =
        dt_pdf_add_image(pdf, (uint8_t *)box->buf, box->exp_width, box->exp_height,
                         8, icc_id, 0.0);

      //  PDF bounding-box has origin on bottom-left
      pdf_image[count]->bb_x      = dt_pdf_pixel_to_point(box->print.x, resolution);
      pdf_image[count]->bb_y      = dt_pdf_pixel_to_point(box->print.y, resolution);
      pdf_image[count]->bb_width  = dt_pdf_pixel_to_point(box->print.width, resolution);
      pdf_image[count]->bb_height = dt_pdf_pixel_to_point(box->print.height, resolution);
      count++;
    }
  }

  params->pdf_page = dt_pdf_add_page(pdf, pdf_image, count);
  dt_pdf_finish(pdf, &params->pdf_page, 1);

  // now releases all the buf
  for(int k=0; k<imgs.count; k++)
  {
    dt_image_box *box = &imgs.box[k];
    g_free(box->buf);
    box->buf = NULL;
  }
}

void _fill_box_values(dt_lib_print_settings_t *ps)
{
  float x = 0.0f, y = 0.0f, swidth = 0.0f, sheight = 0.0f;

  if(ps->last_selected != -1)
  {
    dt_image_box *box = &ps->imgs.box[ps->last_selected];

    float width, height;
    _get_page_dimension(&ps->prt, &width, &height);

    x       = _percent_unit_of(ps, width, box->pos.x);
    y       = _percent_unit_of(ps, height, box->pos.y);
    swidth  = _percent_unit_of(ps, width, box->pos.width);
    sheight = _percent_unit_of(ps, height, box->pos.height);

    for(int i=0; i<9; i++)
    {
      ++darktable.gui->reset;
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ps->dtba[i]), (i == box->alignment));
      --darktable.gui->reset;
    }
  }

  ++darktable.gui->reset;

  gtk_spin_button_set_value(GTK_SPIN_BUTTON(ps->b_x), x);

  gtk_spin_button_set_value(GTK_SPIN_BUTTON(ps->b_y), y);

  gtk_spin_button_set_value(GTK_SPIN_BUTTON(ps->b_width), swidth);

  gtk_spin_button_set_value(GTK_SPIN_BUTTON(ps->b_height), sheight);

  --darktable.gui->reset;
}

static int _export_and_setup_pos(dt_job_t *job, dt_image_box *img, const int32_t idx)
{
  dt_lib_print_job_t *params = dt_control_job_get_params(job);

  float width, height;
  _get_page_dimension(&params->prt, &width, &height);

  dt_printing_setup_page(&params->imgs, width, height, params->prt.printer.resolution);

  dt_print(DT_DEBUG_PRINT, "[print] max image size %d x %d (at resolution %d)\n",
           img->max_width, img->max_height, params->prt.printer.resolution);

  if(_export_image(job, img)) return 1;

  dt_printing_setup_image(&params->imgs, idx,
                          img->imgid, img->exp_width, img->exp_height, img->alignment);

  return 0;
}

static int _print_job_run(dt_job_t *job)
{
  dt_lib_print_job_t *params = dt_control_job_get_params(job);

  // get first image on a box, needed as print leader

  int imgid = -1;

  // compute the needed size for picture for the given printer resolution

  for(int k=0; k<params->imgs.count; k++)
  {
    if(params->imgs.box[k].imgid > -1)
    {
      if(imgid == -1) imgid = params->imgs.box[k].imgid;
      if(_export_and_setup_pos(job, &params->imgs.box[k], k))
        return 1;
    }
  }

  if(dt_control_job_get_state(job) == DT_JOB_STATE_CANCELLED) return 0;
  dt_control_job_set_progress(job, 0.9);

  dt_loc_get_tmp_dir(params->pdf_filename, sizeof(params->pdf_filename));
  g_strlcat(params->pdf_filename, "/pf.XXXXXX.pdf", sizeof(params->pdf_filename));

  const gint fd = g_mkstemp(params->pdf_filename);
  if(fd == -1)
  {
    dt_control_log(_("failed to create temporary pdf for printing"));
    fprintf(stderr, "failed to create temporary pdf for printing\n");
    return 1;
  }
  close(fd);

  float width, height;
  _get_page_dimension(&params->prt, &width, &height);

  _create_pdf(job, params->imgs, width, height);

  if(dt_control_job_get_state(job) == DT_JOB_STATE_CANCELLED) return 0;
  dt_control_job_set_progress(job, 0.95);

  // send to CUPS

  dt_print_file(imgid, params->pdf_filename, params->job_title, &params->prt);
  dt_control_job_set_progress(job, 1.0);

  // add tag for this image

  char tag[256] = { 0 };
  guint tagid = 0;
  snprintf (tag, sizeof(tag), "darktable|printed|%s", params->prt.printer.name);
  dt_tag_new(tag, &tagid);

  for(int k=0; k<params->imgs.count; k++)
  {
    if(params->imgs.box[k].imgid > -1)
      if(dt_tag_attach(tagid, params->imgs.box[k].imgid, FALSE, FALSE))
        DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_TAG_CHANGED);

    /* register print timestamp in cache */
    dt_image_cache_set_print_timestamp(darktable.image_cache, params->imgs.box[k].imgid);
  }

  return 0;
}

static void _page_new_area_clicked(GtkWidget *widget, gpointer user_data)
{
  const dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_print_settings_t *ps = (dt_lib_print_settings_t *)self->data;

  if(ps->imgs.count == MAX_IMAGE_PER_PAGE)
  {
    dt_control_log(_("maximum image per page reached"));
    return;
  }

  dt_control_change_cursor(GDK_PLUS);
  ps->creation = TRUE;
  ps->has_changed = TRUE;
}

static void _page_clear_area_clicked(GtkWidget *widget, gpointer user_data)
{
  const dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_print_settings_t *ps = (dt_lib_print_settings_t *)self->data;

  ps->has_changed = TRUE;
  dt_printing_clear_boxes(&ps->imgs);
  gtk_widget_set_sensitive(ps->del, FALSE);
  dt_control_queue_redraw_center();
}

static void _page_delete_area(const dt_lib_module_t *self, const int box_index)
{
  dt_lib_print_settings_t *ps = (dt_lib_print_settings_t *)self->data;

  if(box_index == -1) return;

  for(int k=box_index; k<MAX_IMAGE_PER_PAGE-1; k++)
  {
    memcpy(&ps->imgs.box[k], &ps->imgs.box[k+1], sizeof(dt_image_box));
  }
  ps->last_selected = -1;
  ps->selected = -1;
  dt_printing_clear_box(&ps->imgs.box[MAX_IMAGE_PER_PAGE-1]);
  ps->imgs.count--;

  if(ps->imgs.count > 0)
    ps->selected = 0;
  else
    gtk_widget_set_sensitive(ps->del, FALSE);

  _fill_box_values(ps);

  ps->has_changed = TRUE;
  dt_control_queue_redraw_center();
}

static void _page_delete_area_clicked(GtkWidget *widget, gpointer user_data)
{
  const dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_print_settings_t *ps = (dt_lib_print_settings_t *)self->data;

  _page_delete_area(self, ps->last_selected);
}

static void _print_job_cleanup(void *p)
{
  dt_lib_print_job_t *params = p;
  if(params->pdf_filename[0]) g_unlink(params->pdf_filename);
  free(params->pdf_page);
  free(params->buf);
  g_free(params->style);
  g_free(params->buf_icc_profile);
  g_free(params->p_icc_profile);
  g_free(params->job_title);
  free(params);
}

static void _print_button_clicked(GtkWidget *widget, gpointer user_data)
{
  const dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_print_settings_t *ps = (dt_lib_print_settings_t *)self->data;

  int imgid = -1;

  // at least one image on a box

  for(int k=0; k<ps->imgs.count; k++)
  {
    if(ps->imgs.box[k].imgid > -1)
    {
      imgid = ps->imgs.box[k].imgid;
      break;
    }
  }

  if(imgid == -1)
  {
    dt_control_log(_("cannot print until a picture is selected"));
    return;
  }
  if(strlen(ps->prt.printer.name) == 0 || ps->prt.printer.resolution == 0)
  {
    dt_control_log(_("cannot print until a printer is selected"));
    return;
  }
  if(ps->prt.paper.width == 0 || ps->prt.paper.height == 0)
  {
    dt_control_log(_("cannot print until a paper is selected"));
    return;
  }

  dt_job_t *job = dt_control_job_create(&_print_job_run, "print image %d", imgid);
  if(!job) return;

  dt_lib_print_job_t *params = calloc(1, sizeof(dt_lib_print_job_t));
  dt_control_job_set_params(job, params, _print_job_cleanup);

  memcpy(&params->prt,  &ps->prt, sizeof(dt_print_info_t));
  memcpy(&params->imgs, &ps->imgs, sizeof(ps->imgs));

  // what to call the image?
  GList *res;
  if((res = dt_metadata_get(imgid, "Xmp.dc.title", NULL)) != NULL)
  {
    // FIXME: in metadata_view.c, non-printables are filtered, should we do this here?
    params->job_title = g_strdup((gchar *)res->data);
    g_list_free_full(res, &g_free);
  }
  else
  {
    const dt_image_t *img = dt_image_cache_get(darktable.image_cache, imgid, 'r');
    if(!img)
    {
      // in this case no need to release from cache what we couldn't get
      dt_control_log(_("cannot get image %d for printing"), imgid);
      dt_control_job_dispose(job);
      return;
    }
    params->job_title = g_strdup(img->filename);
    dt_image_cache_read_release(darktable.image_cache, img);
  }
  // FIXME: ellipsize title/printer as the export completed message is ellipsized
  gchar *message = g_strdup_printf(_("processing `%s' for `%s'"), params->job_title, params->prt.printer.name);
  dt_control_job_add_progress(job, message, TRUE);
  g_free(message);

  // FIXME: getting this from conf as w/prior code, but switch to getting from ps
  params->style = dt_conf_get_string("plugins/print/print/style");
  params->style_append = ps->v_style_append;

  // FIXME: getting these from conf as w/prior code, but switch to getting them from ps
  params->buf_icc_type = dt_conf_get_int("plugins/print/print/icctype");
  params->buf_icc_profile = dt_conf_get_string("plugins/print/print/iccprofile");
  params->buf_icc_intent = dt_conf_get_int("plugins/print/print/iccintent");

  params->p_icc_type = ps->v_picctype;
  params->p_icc_profile = g_strdup(ps->v_piccprofile);
  params->p_icc_intent = ps->v_pintent;
  params->black_point_compensation = ps->v_black_point_compensation;

  dt_control_add_job(darktable.control, DT_JOB_QUEUE_USER_EXPORT, job);
}

static void _set_printer(const dt_lib_module_t *self, const char *printer_name)
{
  dt_lib_print_settings_t *ps = (dt_lib_print_settings_t *)self->data;

  dt_get_printer_info(printer_name, &ps->prt.printer);

  // if this is a turboprint printer, disable the printer profile

  if(ps->prt.printer.is_turboprint)
    dt_bauhaus_combobox_set(ps->pprofile, 0);

  dt_conf_set_string("plugins/print/print/printer", printer_name);

  // add papers for the given printer
  dt_bauhaus_combobox_clear(ps->papers);
  if(ps->paper_list) g_list_free_full(ps->paper_list, free);
  ps->paper_list = dt_get_papers (&ps->prt.printer);
  for(const GList *papers = ps->paper_list; papers; papers = g_list_next (papers))
  {
    const dt_paper_info_t *p = (dt_paper_info_t *)papers->data;
    dt_bauhaus_combobox_add(ps->papers, p->common_name);
  }
  const char *default_paper = dt_conf_get_string_const("plugins/print/print/paper");
  if(!dt_bauhaus_combobox_set_from_text(ps->papers, default_paper))
    dt_bauhaus_combobox_set(ps->papers, 0);

  // add corresponding supported media
  dt_bauhaus_combobox_clear(ps->media);
  if(ps->media_list) g_list_free_full(ps->media_list, free);
  ps->media_list = dt_get_media_type(&ps->prt.printer);
  for(const GList *media = ps->media_list; media; media = g_list_next (media))
  {
    const dt_medium_info_t *m = (dt_medium_info_t *)media->data;
    dt_bauhaus_combobox_add(ps->media, m->common_name);
  }
  const char *default_medium = dt_conf_get_string_const("plugins/print/print/medium");
  if(!dt_bauhaus_combobox_set_from_text(ps->media, default_medium))
    dt_bauhaus_combobox_set(ps->media, 0);

  dt_view_print_settings(darktable.view_manager, &ps->prt, &ps->imgs);
}

static void
_printer_changed(GtkWidget *combo, const dt_lib_module_t *self)
{
  const gchar *printer_name = dt_bauhaus_combobox_get_text(combo);

  if(printer_name)
    _set_printer (self, printer_name);
}

static void
_paper_changed(GtkWidget *combo, const dt_lib_module_t *self)
{
  dt_lib_print_settings_t *ps = (dt_lib_print_settings_t *)self->data;

  const gchar *paper_name = dt_bauhaus_combobox_get_text(combo);

  if(!paper_name) return;

  const dt_paper_info_t *paper = dt_get_paper(ps->paper_list, paper_name);

  if(paper)
    memcpy(&ps->prt.paper, paper, sizeof(dt_paper_info_t));

  float width, height;
  _get_page_dimension(&ps->prt, &width, &height);

  dt_printing_setup_page(&ps->imgs, width, height, ps->prt.printer.resolution);

  dt_conf_set_string("plugins/print/print/paper", paper_name);
  dt_view_print_settings(darktable.view_manager, &ps->prt, &ps->imgs);

  _update_slider(ps);
}

static void
_media_changed(GtkWidget *combo, const dt_lib_module_t *self)
{
  dt_lib_print_settings_t *ps = (dt_lib_print_settings_t *)self->data;

  const gchar *medium_name = dt_bauhaus_combobox_get_text(combo);

  if(!medium_name) return;

  const dt_medium_info_t *medium = dt_get_medium(ps->media_list, medium_name);

  if(medium)
    memcpy(&ps->prt.medium, medium, sizeof(dt_medium_info_t));

  dt_conf_set_string("plugins/print/print/medium", medium_name);
  dt_view_print_settings(darktable.view_manager, &ps->prt, &ps->imgs);

  _update_slider(ps);
}

static void
_update_slider(dt_lib_print_settings_t *ps)
{
  dt_view_print_settings(darktable.view_manager, &ps->prt, &ps->imgs);

  // if widget are created, let's display the current image size

  // FIXME: why doesn't this update when units are changed?
  if(ps->selected != -1
     && ps->imgs.box[ps->selected].imgid != -1
     && ps->width && ps->height
     && ps->info)
  {
    const dt_image_box *box = &ps->imgs.box[ps->selected];

    dt_image_pos box_size_mm, box_size;
    dt_printing_get_image_pos_mm(&ps->imgs, box, &box_size_mm);
    dt_printing_get_image_pos(&ps->imgs, box, &box_size);

    const double w = box_size_mm.width * units[ps->unit];
    const double h = box_size_mm.height * units[ps->unit];
    char *value, *precision;
    int n_digits;
    _precision_by_unit(ps->unit, &n_digits, NULL, &precision);

    value = g_strdup_printf(precision, w);
    gtk_label_set_text(GTK_LABEL(ps->width), value);
    g_free(value);

    value = g_strdup_printf(precision, h);
    gtk_label_set_text(GTK_LABEL(ps->height), value);
    g_free(value);
    g_free(precision);

    // compute the image down/up scale and report information

    const float iwidth  = box->img_width;
    const float iheight = box->img_height;
    const float awidth  = box_size.width;
    const float aheight = box_size.height;

    const double scale = (iwidth <= awidth)
      ? awidth / iwidth
      : aheight/ iheight;

    value = g_strdup_printf(_("%3.2f (dpi:%d)"), scale,
                            scale <= 1.0
                            ? (int)ps->prt.printer.resolution
                            : (int)(ps->prt.printer.resolution / scale));
    gtk_label_set_text(GTK_LABEL(ps->info), value);
    g_free(value);
  }
}

static void
_top_border_callback(GtkWidget *spin, gpointer user_data)
{
  const dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_print_settings_t *ps = (dt_lib_print_settings_t *)self->data;
  const double value = gtk_spin_button_get_value(GTK_SPIN_BUTTON(spin));

  dt_conf_set_float("plugins/print/print/top_margin", value);

  ps->prt.page.margin_top = _to_mm(ps, value);

  if(ps->lock_activated == TRUE)
  {
    ps->prt.page.margin_bottom = _to_mm(ps, value);
    ps->prt.page.margin_left = _to_mm(ps, value);
    ps->prt.page.margin_right = _to_mm(ps, value);

    gtk_spin_button_set_value(GTK_SPIN_BUTTON(ps->b_bottom), value);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(ps->b_left), value);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(ps->b_right), value);

    dt_conf_set_float("plugins/print/print/bottom_margin", value);
    dt_conf_set_float("plugins/print/print/left_margin", value);
    dt_conf_set_float("plugins/print/print/right_margin", value);
  }

  _update_slider(ps);
}

static void
_bottom_border_callback(GtkWidget *spin, gpointer user_data)
{
  const dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_print_settings_t *ps = (dt_lib_print_settings_t *)self->data;
  const double value = gtk_spin_button_get_value(GTK_SPIN_BUTTON(spin));

  dt_conf_set_float("plugins/print/print/bottom_margin", value);

  ps->prt.page.margin_bottom = _to_mm(ps, value);
  _update_slider(ps);
}

static void
_left_border_callback(GtkWidget *spin, gpointer user_data)
{
  const dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_print_settings_t *ps = (dt_lib_print_settings_t *)self->data;
  const double value = gtk_spin_button_get_value(GTK_SPIN_BUTTON(spin));

  dt_conf_set_float("plugins/print/print/left_margin", value);

  ps->prt.page.margin_left = _to_mm(ps, value);
  _update_slider(ps);
}

static void
_right_border_callback(GtkWidget *spin, gpointer user_data)
{
  const dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_print_settings_t *ps = (dt_lib_print_settings_t *)self->data;
  const double value = gtk_spin_button_get_value(GTK_SPIN_BUTTON(spin));

  dt_conf_set_float("plugins/print/print/right_margin", value);

  ps->prt.page.margin_right = _to_mm(ps, value);
  _update_slider(ps);
}

static void
_lock_callback(GtkWidget *button, gpointer user_data)
{
  const dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_print_settings_t *ps = (dt_lib_print_settings_t *)self->data;

  ps->lock_activated = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(button));

  dt_conf_set_bool("plugins/print/print/lock_borders", ps->lock_activated);

  gtk_widget_set_sensitive(GTK_WIDGET(ps->b_bottom), !ps->lock_activated);
  gtk_widget_set_sensitive(GTK_WIDGET(ps->b_left), !ps->lock_activated);
  gtk_widget_set_sensitive(GTK_WIDGET(ps->b_right), !ps->lock_activated);

  //  get value of top and set it to all other borders

  const double value = gtk_spin_button_get_value(GTK_SPIN_BUTTON(ps->b_top));
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(ps->b_bottom), value);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(ps->b_left), value);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(ps->b_right), value);

  _update_slider (ps);
}

static void
_alignment_callback(GtkWidget *tb, gpointer user_data)
{
  if(darktable.gui->reset) return;

  int index=-1;
  const dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_print_settings_t *ps = (dt_lib_print_settings_t *)self->data;

  for(int i=0; i<9; i++)
  {
    /* block signal handler */
    g_signal_handlers_block_by_func(ps->dtba[i],_alignment_callback, user_data);

    if(GTK_WIDGET(ps->dtba[i]) == tb)
    {
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ps->dtba[i]), TRUE);
      index=i;
    }
    else gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ps->dtba[i]), FALSE);

    /* unblock signal handler */
    g_signal_handlers_unblock_by_func(ps->dtba[i], _alignment_callback, user_data);
  }

  if(ps->last_selected != -1)
  {
    dt_printing_setup_image(&ps->imgs, ps->last_selected,
                            ps->imgs.box[ps->last_selected].imgid, 100, 100, index);
  }

  _update_slider(ps);
}

static void
_orientation_changed(GtkWidget *combo, dt_lib_module_t *self)
{
  dt_lib_print_settings_t *ps = (dt_lib_print_settings_t *)self->data;

  ps->prt.page.landscape = dt_bauhaus_combobox_get(combo);

  _update_slider(ps);
}

static void
_snap_grid_callback(GtkWidget *widget, dt_lib_module_t *self)
{
  dt_control_queue_redraw_center();
}

static void
_grid_callback(GtkWidget *widget, dt_lib_module_t *self)
{
  dt_control_queue_redraw_center();
}

static void _grid_size_changed(GtkWidget *widget, dt_lib_module_t *self)
{
  if(darktable.gui->reset) return;

  dt_lib_print_settings_t *ps = (dt_lib_print_settings_t *)self->data;
  const float value = gtk_spin_button_get_value(GTK_SPIN_BUTTON(ps->grid_size));
  dt_conf_set_float("plugins/print/print/grid_size", _to_mm(ps, value));

  dt_control_queue_redraw_center();
}

static void
_unit_changed(GtkWidget *combo, dt_lib_module_t *self)
{
  if(darktable.gui->reset) return;

  dt_lib_print_settings_t *ps = (dt_lib_print_settings_t *)self->data;

  const float grid_size = dt_conf_get_float("plugins/print/print/grid_size");

  const int unit = dt_bauhaus_combobox_get(combo);
  if(unit < 0) return; // shouldn't happen, but it could be -1 if nothing is selected
  ps->unit = unit;
  dt_conf_set_string("plugins/print/print/unit", _unit_names[unit]);

  const double margin_top = ps->prt.page.margin_top;
  const double margin_left = ps->prt.page.margin_left;
  const double margin_right = ps->prt.page.margin_right;
  const double margin_bottom = ps->prt.page.margin_bottom;

  int n_digits;
  float incr;
  _precision_by_unit(ps->unit, &n_digits, &incr, NULL);

  ++darktable.gui->reset;

  gtk_spin_button_set_digits(GTK_SPIN_BUTTON(ps->b_top), n_digits);
  gtk_spin_button_set_digits(GTK_SPIN_BUTTON(ps->b_bottom), n_digits);
  gtk_spin_button_set_digits(GTK_SPIN_BUTTON(ps->b_left), n_digits);
  gtk_spin_button_set_digits(GTK_SPIN_BUTTON(ps->b_right), n_digits);
  gtk_spin_button_set_increments(GTK_SPIN_BUTTON(ps->b_top), incr, 10.f*incr);
  gtk_spin_button_set_increments(GTK_SPIN_BUTTON(ps->b_bottom), incr, 10.f*incr);
  gtk_spin_button_set_increments(GTK_SPIN_BUTTON(ps->b_left), incr, 10.f*incr);
  gtk_spin_button_set_increments(GTK_SPIN_BUTTON(ps->b_right), incr, 10.f*incr);

  gtk_spin_button_set_digits(GTK_SPIN_BUTTON(ps->b_x), n_digits);
  gtk_spin_button_set_digits(GTK_SPIN_BUTTON(ps->b_y), n_digits);
  gtk_spin_button_set_digits(GTK_SPIN_BUTTON(ps->b_width), n_digits);
  gtk_spin_button_set_digits(GTK_SPIN_BUTTON(ps->b_height), n_digits);
  gtk_spin_button_set_increments(GTK_SPIN_BUTTON(ps->b_x), incr, 10.f*incr);
  gtk_spin_button_set_increments(GTK_SPIN_BUTTON(ps->b_y), incr, 10.f*incr);
  gtk_spin_button_set_increments(GTK_SPIN_BUTTON(ps->b_width), incr, 10.f*incr);
  gtk_spin_button_set_increments(GTK_SPIN_BUTTON(ps->b_height), incr, 10.f*incr);

  gtk_spin_button_set_digits(GTK_SPIN_BUTTON(ps->grid_size), n_digits);
  gtk_spin_button_set_increments(GTK_SPIN_BUTTON(ps->grid_size), incr, 10.f*incr);

  _update_slider(ps);

  // convert margins to new unit
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(ps->b_top),    margin_top * units[ps->unit]);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(ps->b_bottom), margin_bottom * units[ps->unit]);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(ps->b_left),   margin_left * units[ps->unit]);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(ps->b_right),  margin_right * units[ps->unit]);

  // grid size
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(ps->grid_size), grid_size * units[ps->unit]);

  --darktable.gui->reset;

  _fill_box_values(ps);
}

static void
_style_callback(GtkWidget *widget, dt_lib_module_t *self)
{
  dt_lib_print_settings_t *ps = (dt_lib_print_settings_t *)self->data;

  if(dt_bauhaus_combobox_get(ps->style) == 0)
  {
    dt_conf_set_string("plugins/print/print/style", "");
    gtk_widget_set_sensitive(GTK_WIDGET(ps->style_mode), FALSE);
  }
  else
  {
    const gchar *style = dt_bauhaus_combobox_get_text(ps->style);
    dt_conf_set_string("plugins/print/print/style", style);
    gtk_widget_set_sensitive(GTK_WIDGET(ps->style_mode), TRUE);
  }

  g_free(ps->v_style);
  ps->v_style = dt_conf_get_string("plugins/print/print/style");
}

static void
_style_mode_changed(GtkWidget *widget, dt_lib_module_t *self)
{
  dt_lib_print_settings_t *ps = (dt_lib_print_settings_t *)self->data;

  if(dt_bauhaus_combobox_get(ps->style_mode) == 0)
    ps->v_style_append = FALSE;
  else
    ps->v_style_append = TRUE;

  dt_conf_set_bool("plugins/print/print/style_append", ps->v_style_append);
}

static void
_profile_changed(GtkWidget *widget, dt_lib_module_t *self)
{
  dt_lib_print_settings_t *ps = (dt_lib_print_settings_t *)self->data;
  const int pos = dt_bauhaus_combobox_get(widget);
  for(const GList *prof = ps->profiles; prof; prof = g_list_next(prof))
  {
    dt_lib_export_profile_t *pp = (dt_lib_export_profile_t *)prof->data;
    if(pp->pos == pos)
    {
      dt_conf_set_int("plugins/print/print/icctype", pp->type);
      dt_conf_set_string("plugins/print/print/iccprofile", pp->filename);
      g_free(ps->v_iccprofile);
      ps->v_icctype = pp->type;
      ps->v_iccprofile = g_strdup(pp->filename);
      return;
    }
  }
  dt_conf_set_int("plugins/print/print/icctype", DT_COLORSPACE_NONE);
  dt_conf_set_string("plugins/print/print/iccprofile", "");
  g_free(ps->v_iccprofile);
  ps->v_icctype = DT_COLORSPACE_NONE;
  ps->v_iccprofile = g_strdup("");
}

static void
_printer_profile_changed(GtkWidget *widget, dt_lib_module_t *self)
{
  dt_lib_print_settings_t *ps = (dt_lib_print_settings_t *)self->data;
  const int pos = dt_bauhaus_combobox_get(widget);
  for(const GList *prof = ps->profiles; prof; prof = g_list_next(prof))
  {
    dt_lib_export_profile_t *pp = (dt_lib_export_profile_t *)prof->data;
    if(pp->ppos == pos)
    {
      dt_conf_set_int("plugins/print/printer/icctype", pp->type);
      dt_conf_set_string("plugins/print/printer/iccprofile", pp->filename);
      g_free(ps->v_piccprofile);
      ps->v_picctype = pp->type;
      ps->v_piccprofile = g_strdup(pp->filename);

      // activate the black compensation and printer intent
      gtk_widget_set_sensitive(GTK_WIDGET(ps->black_point_compensation), TRUE);
      return;
    }
  }
  dt_conf_set_int("plugins/print/printer/icctype", DT_COLORSPACE_NONE);
  dt_conf_set_string("plugins/print/printer/iccprofile", "");
  g_free(ps->v_piccprofile);
  ps->v_picctype = DT_COLORSPACE_NONE;
  ps->v_piccprofile = g_strdup("");
  gtk_widget_set_sensitive(GTK_WIDGET(ps->black_point_compensation), FALSE);
}

static void
_printer_intent_callback (GtkWidget *widget, dt_lib_module_t *self)
{
  dt_lib_print_settings_t *ps = (dt_lib_print_settings_t *)self->data;
  const int pos = dt_bauhaus_combobox_get(widget);
  dt_conf_set_int("plugins/print/printer/iccintent", pos);
  ps->v_pintent = pos;
  ps->prt.printer.intent = pos;
}

static void
_printer_bpc_callback(GtkWidget *widget, dt_lib_module_t *self)
{
  dt_lib_print_settings_t *ps = (dt_lib_print_settings_t *)self->data;
  ps->v_black_point_compensation = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ps->black_point_compensation));
  dt_conf_set_bool("plugins/print/print/black_point_compensation", ps->v_black_point_compensation);
}

static void
_intent_callback(GtkWidget *widget, dt_lib_module_t *self)
{
  dt_lib_print_settings_t *ps = (dt_lib_print_settings_t *)self->data;
  const int pos = dt_bauhaus_combobox_get(widget);
  // record the intent that will override the out rendering module on export
  dt_conf_set_int("plugins/print/print/iccintent", pos - 1);
  ps->v_intent = pos - 1;
}

static void _set_orientation(dt_lib_print_settings_t *ps, int32_t imgid)
{
  dt_mipmap_buffer_t buf;
  dt_mipmap_cache_get(darktable.mipmap_cache, &buf,
                      imgid, DT_MIPMAP_0, DT_MIPMAP_BEST_EFFORT, 'r');

  // If there's a mipmap available, figure out orientation based upon
  // its dimensions. Otherwise, don't touch orientation until the
  // mipmap arrives.
  if(buf.size != DT_MIPMAP_NONE)
  {
    ps->prt.page.landscape = (buf.width > buf.height);
    dt_view_print_settings(darktable.view_manager, &ps->prt, &ps->imgs);
    dt_bauhaus_combobox_set(ps->orientation, ps->prt.page.landscape == TRUE ? 1 : 0);
  }

  dt_mipmap_cache_release(darktable.mipmap_cache, &buf);
  dt_control_queue_redraw_center();
}

static void _load_image_full_page(dt_lib_print_settings_t *ps, int32_t imgid)
{
  _set_orientation(ps, imgid);

  dt_printing_setup_box(&ps->imgs, 0,
                        ps->imgs.screen.page.x, ps->imgs.screen.page.y,
                        ps->imgs.screen.page.width, ps->imgs.screen.page.height);
  float width, height;
  _get_page_dimension(&ps->prt, &width, &height);

  dt_printing_setup_page(&ps->imgs, width, height, ps->prt.printer.resolution);

  dt_printing_setup_image(&ps->imgs, 0, imgid, 100, 100, ALIGNMENT_CENTER);

  dt_control_queue_redraw_center();
}

static void _print_settings_activate_or_update_callback(gpointer instance, int imgid, gpointer user_data)
{
  const dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_print_settings_t *ps = (dt_lib_print_settings_t *)self->data;

  // load an image with a simple click on the filmstrip only if a single image is present
  if(ps->imgs.count == 1)
  {
    if(ps->has_changed)
    {
      dt_printing_setup_image(&ps->imgs, 0, imgid, 100, 100, ps->imgs.box[0].alignment);
    }
    else
    {
      dt_printing_clear_box(&ps->imgs.box[0]);
      _load_image_full_page(ps, imgid);
    }
  }
}

static GList* _get_profiles()
{
  //  Create list of profiles
  GList *list = NULL;

  dt_lib_export_profile_t *prof = (dt_lib_export_profile_t *)g_malloc0(sizeof(dt_lib_export_profile_t));
  prof->type = DT_COLORSPACE_SRGB;
  dt_utf8_strlcpy(prof->name, _("sRGB (web-safe)"), sizeof(prof->name));
  prof->pos = -2;
  prof->ppos = -2;
  list = g_list_prepend(list, prof);

  prof = (dt_lib_export_profile_t *)g_malloc0(sizeof(dt_lib_export_profile_t));
  prof->type = DT_COLORSPACE_ADOBERGB;
  dt_utf8_strlcpy(prof->name, _("Adobe RGB (compatible)"), sizeof(prof->name));
  prof->pos = -2;
  prof->ppos = -2;
  list = g_list_prepend(list, prof);

  // chances are this is the working profile, and hence reasonable to
  // use as the export profile before we convert to printer profile
  prof = (dt_lib_export_profile_t *)g_malloc0(sizeof(dt_lib_export_profile_t));
  prof->type = DT_COLORSPACE_LIN_REC2020;
  dt_utf8_strlcpy(prof->name, _("linear Rec2020 RGB"), sizeof(prof->name));
  prof->pos = -2;
  prof->ppos = -2;
  list = g_list_prepend(list, prof);

  // add the profiles from datadir/color/out/*.icc
  for(GList *iter = darktable.color_profiles->profiles; iter; iter = g_list_next(iter))
  {
    dt_colorspaces_color_profile_t *p = (dt_colorspaces_color_profile_t *)iter->data;
    if(p->type == DT_COLORSPACE_FILE)
    {
      prof = (dt_lib_export_profile_t *)g_malloc0(sizeof(dt_lib_export_profile_t));
      g_strlcpy(prof->name, p->name, sizeof(prof->name));
      g_strlcpy(prof->filename, p->filename, sizeof(prof->filename));
      prof->type = DT_COLORSPACE_FILE;
      prof->pos = -2;
      prof->ppos = -2;
      list = g_list_prepend(list, prof);
    }
  }

  return g_list_reverse(list);  // list was built in reverse order, so un-reverse it
}

static void _new_printer_callback(dt_printer_info_t *printer, void *user_data)
{
  static int count = 0;
  const dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  const dt_lib_print_settings_t *d = (dt_lib_print_settings_t*)self->data;

  char *default_printer = dt_conf_get_string("plugins/print/print/printer");

  g_signal_handlers_block_by_func(G_OBJECT(d->printers), G_CALLBACK(_printer_changed), NULL);

  dt_bauhaus_combobox_add(d->printers, printer->name);

  if(!g_strcmp0(default_printer, printer->name) || default_printer[0]=='\0')
  {
    dt_bauhaus_combobox_set(d->printers, count);
    _set_printer(self, printer->name);
  }
  count++;
  g_free(default_printer);

  g_signal_handlers_unblock_by_func(G_OBJECT(d->printers), G_CALLBACK(_printer_changed), NULL);
}

void view_enter(struct dt_lib_module_t *self,struct dt_view_t *old_view,struct dt_view_t *new_view)
{
  // user activated a new image via the filmstrip or user entered view
  // mode which activates an image: get image_id and orientation
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_VIEWMANAGER_THUMBTABLE_ACTIVATE,
                            G_CALLBACK(_print_settings_activate_or_update_callback), self);

  // when an updated mipmap, we may have new orientation information
  // about the current image. This updates the image_id as well and
  // zeros out dimensions, but there should be no harm in that
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals,
                            DT_SIGNAL_DEVELOP_MIPMAP_UPDATED,
                            G_CALLBACK(_print_settings_activate_or_update_callback),
                            self);

  // NOTE: it would be proper to set image_id here to -1, but this seems to make no difference
}

void view_leave(struct dt_lib_module_t *self,struct dt_view_t *old_view,struct dt_view_t *new_view)
{
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals,
                               G_CALLBACK(_print_settings_activate_or_update_callback),
                               self);
}

static gboolean _expose_again(gpointer user_data)
{
  dt_lib_print_settings_t *ps = (dt_lib_print_settings_t *)user_data;

  if(ps->imgs.imgid_to_load != -1)
  {
    _load_image_full_page(ps, ps->imgs.imgid_to_load);
    ps->imgs.imgid_to_load = -1;
  }

  dt_control_queue_redraw_center();
  return FALSE;
}

void _get_control(dt_lib_print_settings_t *ps, float x, float y)
{
  const float dist = 20.0;

  const dt_image_box *b = &ps->imgs.box[ps->selected];

  ps->sel_controls = 0;

  if(fabsf(b->screen.x - x) < dist)
    ps->sel_controls |= BOX_LEFT;

  if(fabsf(b->screen.y - y) < dist)
    ps->sel_controls |= BOX_TOP;

  if(fabsf((b->screen.x + b->screen.width) - x) < dist)
    ps->sel_controls |= BOX_RIGHT;

  if(fabsf((b->screen.y + b->screen.height) - y) < dist)
    ps->sel_controls |= BOX_BOTTOM;

  if(ps->sel_controls == 0) ps->sel_controls = BOX_ALL;
}

int mouse_leave(struct dt_lib_module_t *self)
{
  dt_lib_print_settings_t *ps = (dt_lib_print_settings_t *)self->data;

  if(ps->last_selected != -1)
  {
    dt_control_set_mouse_over_id(ps->imgs.box[ps->last_selected].imgid);
  }

  return 0;
}

static void _snap_to_grid(dt_lib_print_settings_t *ps, float *x, float *y)
{
  if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ps->snap_grid)))
  {
    // V lines
    const float step = gtk_spin_button_get_value(GTK_SPIN_BUTTON(ps->grid_size)) * units[ps->unit];

    // only display the grid if a step of 5 pixels
    const float diff = DT_PIXEL_APPLY_DPI(5);

    float grid_pos = (float)ps->imgs.screen.page.x;

    const float h_step = _mm_to_hscreen(ps, step, FALSE);

    while(grid_pos < ps->imgs.screen.page.x + ps->imgs.screen.page.width)
    {
      if(fabsf(*x - grid_pos) < diff) *x = grid_pos;
      grid_pos += h_step;
    }

    // H lines
    grid_pos = (float)ps->imgs.screen.page.y;

    const float v_step = _mm_to_vscreen(ps, step, FALSE);

    while(grid_pos < ps->imgs.screen.page.y + ps->imgs.screen.page.height)
    {
      if(fabsf(*y - grid_pos) < diff) *y = grid_pos;
      grid_pos += v_step;
    }
  }
  // FIXME: should clamp values to page size here?
}

int mouse_moved(struct dt_lib_module_t *self, double x, double y, double pressure, int which)
{
  dt_lib_print_settings_t *ps = (dt_lib_print_settings_t *)self->data;

  gboolean expose = FALSE;

  if(ps->creation)
    dt_control_change_cursor(GDK_PLUS);

  if(ps->creation && ps->dragging)
  {
    ps->x2 = x;
    ps->y2 = y;
    _snap_to_grid(ps, &ps->x2, &ps->y2);
    expose = TRUE;
  }
  else if(ps->dragging)
  {
    dt_image_box *b = &ps->imgs.box[ps->selected];
    const float dx = x - ps->click_pos_x;
    const float dy = y - ps->click_pos_y;
    const float coef = dx / b->screen.width;

    switch(ps->sel_controls)
    {
       case BOX_ALL:
         ps->x1 = b->screen.x + dx;
         ps->y1 = b->screen.y + dy;
         ps->x2 = b->screen.x + b->screen.width + dx;
         ps->y2 = b->screen.y + b->screen.height + dy;
         break;
       case BOX_LEFT:
         ps->x1 = b->screen.x + dx;
         break;
       case BOX_TOP:
         ps->y1 = b->screen.y + dy;
         break;
       case BOX_RIGHT:
         ps->x2 = b->screen.x + b->screen.width + dx;
         break;
       case BOX_BOTTOM:
         ps->y2 = b->screen.y + b->screen.height + dy;
         break;
       case BOX_TOP_LEFT:
         ps->x1 = b->screen.x + dx;
         ps->y1 = b->screen.y + (coef * b->screen.height);
         break;
       case BOX_TOP_RIGHT:
         ps->x2 = b->screen.x + b->screen.width + dx;
         ps->y1 = b->screen.y - (coef * b->screen.height);
         break;
       case BOX_BOTTOM_LEFT:
         ps->x1 = b->screen.x + dx;
         ps->y2 = b->screen.y + b->screen.height - (coef * b->screen.height);
         break;
       case BOX_BOTTOM_RIGHT:
         ps->x2 = b->screen.x + b->screen.width + dx;
         ps->y2 = b->screen.y + b->screen.height + (coef * b->screen.height);
         break;
       default:
         break;
    }
    expose = TRUE;

    _snap_to_grid(ps, &ps->x1, &ps->y1);
    _snap_to_grid(ps, &ps->x2, &ps->y2);
  }
  else if(!ps->creation)
  {
    const int bidx = dt_printing_get_image_box(&ps->imgs, x, y);
    ps->sel_controls = 0;

    if(bidx == -1)
    {
      if(ps->selected != -1) expose = TRUE;
      ps->selected = -1;
    }
    else
    {
      expose = TRUE;
      ps->selected = bidx;
      _fill_box_values(ps);
      _get_control(ps, x, y);
    }
  }

  if(expose) dt_control_queue_redraw_center();

  return 0;
}

static void _swap(float *a, float *b)
{
  const float tmp = *a;
  *a = *b;
  *b = tmp;
}

int button_released(struct dt_lib_module_t *self, double x, double y, int which, uint32_t state)
{
  dt_lib_print_settings_t *ps = (dt_lib_print_settings_t *)self->data;

  if(ps->dragging)
  {
    int idx = -1;

    gtk_widget_set_sensitive(ps->del, TRUE);

    // handle new area
    if(ps->creation)
    {
      idx = ps->imgs.count++;
    }
    else if(ps->selected != -1)
    {
      idx = ps->selected;
    }

    if(idx != -1)
    {
      // make sure the area is in the the printable area taking into account the margins

      // don't allow a too small area
      if(ps->x2 < ps->x1) _swap(&ps->x1, &ps->x2);
      if(ps->y2 < ps->y1) _swap(&ps->y1, &ps->y2);

      const float dx = ps->x2 - ps->x1;
      const float dy = ps->y2 - ps->y1;

      dt_printing_setup_box(&ps->imgs, idx, ps->x1, ps->y1, dx, dy);
      // make the new created box the last edited one
      ps->last_selected = idx;
      _fill_box_values(ps);
    }
  }

  _update_slider(ps);

  ps->creation = FALSE;
  ps->dragging = FALSE;

  dt_control_change_cursor(GDK_LEFT_PTR);

  return 0;
}

int button_pressed(struct dt_lib_module_t *self, double x, double y, double pressure,
                   int which, int type, uint32_t state)
{
  dt_lib_print_settings_t *ps = (dt_lib_print_settings_t *)self->data;

  ps->click_pos_x = x;
  ps->click_pos_y = y;
  ps->last_selected = -1;

  if(ps->creation)
  {
    ps->dragging = TRUE;
    ps->selected = -1;
    ps->x1 = ps->x2 = x;
    ps->y1 = ps->y2 = y;

    _snap_to_grid(ps, &ps->x1, &ps->y1);
  }
  else if(ps->selected > 0
          && (which == 2 || (which == 1 && dt_modifier_is(state, GDK_CONTROL_MASK))))
  {
    // middle click (or ctrl-click), move selected image down
    dt_image_box b;
    memcpy(&b, &ps->imgs.box[ps->selected], sizeof(dt_image_box));
    memcpy(&ps->imgs.box[ps->selected], &ps->imgs.box[ps->selected-1], sizeof(dt_image_box));
    memcpy(&ps->imgs.box[ps->selected-1], &b, sizeof(dt_image_box));
  }
  else if(ps->selected != -1 && which == 1)
  {
    dt_image_box *b = &ps->imgs.box[ps->selected];

    ps->dragging = TRUE;
    ps->x1 = b->screen.x;
    ps->y1 = b->screen.y;
    ps->x2 = b->screen.x + b->screen.width;
    ps->y2 = b->screen.y + b->screen.height;

    ps->last_selected = ps->selected;
    ps->has_changed = TRUE;

    _get_control(ps, x, y);

    dt_control_change_cursor(GDK_HAND1);
  }
  else if(ps->selected != -1 && which == 3)
  {
    dt_image_box *b = &ps->imgs.box[ps->selected];

    // if image present remove it, otherwise remove the box
    if(b->imgid != -1)
      b->imgid = -1;
    else
      _page_delete_area(self, ps->selected);

    ps->last_selected = ps->selected;
    ps->has_changed = TRUE;
  }

  return 0;
}

void _cairo_rectangle(cairo_t *cr, const int sel_controls,
                               const int x1, const int y1, const int x2, const int y2)
{
  const float sel_width = DT_PIXEL_APPLY_DPI(3.0);
  const float std_width = DT_PIXEL_APPLY_DPI(1.0);

  const gboolean all = sel_controls == BOX_ALL;

  cairo_move_to(cr, x1, y1);
  cairo_set_line_width(cr, (all || sel_controls == BOX_LEFT) ? sel_width : std_width);
  cairo_line_to(cr, x1, y2);
  cairo_stroke(cr);

  cairo_move_to(cr, x1, y2);
  cairo_set_line_width(cr, (all || sel_controls == BOX_BOTTOM) ? sel_width : std_width);
  cairo_line_to(cr, x2, y2);
  cairo_stroke(cr);

  cairo_move_to(cr, x2, y2);
  cairo_set_line_width(cr, (all || sel_controls == BOX_RIGHT) ? sel_width : std_width);
  cairo_line_to(cr, x2, y1);
  cairo_stroke(cr);

  cairo_move_to(cr, x2, y1);
  cairo_set_line_width(cr, (all || sel_controls == BOX_TOP) ? sel_width : std_width);
  cairo_line_to(cr, x1, y1);
  cairo_stroke(cr);

  if(sel_controls == 0)
  {
    const double dash[] = { DT_PIXEL_APPLY_DPI(3.0), DT_PIXEL_APPLY_DPI(3.0) };
    cairo_set_dash(cr, dash, 2, 0);

    // no image inside
    cairo_move_to(cr, x1, y1);
    cairo_line_to(cr, x2, y2);

    cairo_move_to(cr, x1, y2);
    cairo_line_to(cr, x2, y1);
    cairo_stroke(cr);
  }

  cairo_set_dash(cr, NULL, 0, 0);
  cairo_set_line_width(cr, sel_width);

  if(sel_controls == BOX_TOP_LEFT)
  {
    cairo_rectangle (cr, x1, y1, DT_PIXEL_APPLY_DPI(15), DT_PIXEL_APPLY_DPI(15));
    cairo_stroke(cr);
  }

  if(sel_controls == BOX_TOP_RIGHT)
  {
    cairo_rectangle (cr, x2 - DT_PIXEL_APPLY_DPI(15), y1,
                     DT_PIXEL_APPLY_DPI(15), DT_PIXEL_APPLY_DPI(15));
    cairo_stroke(cr);
  }

  if(sel_controls == BOX_BOTTOM_LEFT)
  {
    cairo_rectangle (cr, x1, y2 - DT_PIXEL_APPLY_DPI(15),
                     DT_PIXEL_APPLY_DPI(15), DT_PIXEL_APPLY_DPI(15));
    cairo_stroke(cr);
  }

  if(sel_controls == BOX_BOTTOM_RIGHT)
  {
    cairo_rectangle (cr, x2 - DT_PIXEL_APPLY_DPI(15), y2 - DT_PIXEL_APPLY_DPI(15),
                     DT_PIXEL_APPLY_DPI(15), DT_PIXEL_APPLY_DPI(15));
    cairo_stroke(cr);
  }
}

void gui_post_expose(struct dt_lib_module_t *self, cairo_t *cr, int32_t width, int32_t height,
                     int32_t pointerx, int32_t pointery)
{
  dt_lib_print_settings_t *ps = (dt_lib_print_settings_t *)self->data;

  if(ps->imgs.imgid_to_load != -1)
  {
    // we set orientation and delay the reload to ensure the
    // page is properly set before trying to display the image.
    _set_orientation(ps, ps->imgs.imgid_to_load);
    g_timeout_add(250, _expose_again, ps);
  }

  // display grid

  // 1mm

  const float step = gtk_spin_button_get_value(GTK_SPIN_BUTTON(ps->grid_size)) / units[ps->unit];

  // only display grid if spacing more than 5 pixels
  if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ps->grid))
     && (int)_mm_to_hscreen(ps, step, FALSE) > DT_PIXEL_APPLY_DPI(5))
  {
    const double dash[] = { DT_PIXEL_APPLY_DPI(5.0), DT_PIXEL_APPLY_DPI(5.0) };
    cairo_set_source_rgba(cr, 1, .2, .2, 0.6);

    // V lines
    float grid_pos = (float)ps->imgs.screen.page.x;

    const float h_step = _mm_to_hscreen(ps, step, FALSE);
    int n = 0;

    while(grid_pos < ps->imgs.screen.page.x + ps->imgs.screen.page.width)
    {
      cairo_set_dash(cr, dash, ((n % 5) == 0) ? 0 : 2, DT_PIXEL_APPLY_DPI(5));
      cairo_set_line_width(cr, ((n % 5) == 0) ? DT_PIXEL_APPLY_DPI(1.0) : DT_PIXEL_APPLY_DPI(0.5));
      cairo_move_to(cr, grid_pos, ps->imgs.screen.page.y);
      cairo_line_to(cr, grid_pos, ps->imgs.screen.page.y + ps->imgs.screen.page.height);
      cairo_stroke(cr);
      grid_pos += h_step;
      n++;
    }

    // H lines
    grid_pos = (float)ps->imgs.screen.page.y;

    const float v_step = _mm_to_vscreen(ps, step, FALSE);
    n = 0;

    while(grid_pos < ps->imgs.screen.page.y + ps->imgs.screen.page.height)
    {
      cairo_set_dash(cr, dash, ((n % 5) == 0) ? 0 : 2, DT_PIXEL_APPLY_DPI(5));
      cairo_set_line_width(cr, ((n % 5) == 0) ? DT_PIXEL_APPLY_DPI(1.0) : DT_PIXEL_APPLY_DPI(0.5));
      cairo_move_to(cr, ps->imgs.screen.page.x, grid_pos);
      cairo_line_to(cr, ps->imgs.screen.page.x + ps->imgs.screen.page.width, grid_pos);
      cairo_stroke(cr);
      grid_pos += v_step;
      n++;
    }
  }

  // disable dash
  cairo_set_source_rgba(cr, 1, .2, .2, 0.6);
  cairo_set_dash(cr, NULL, 0, 0);

  const float scaler = 1.0f / darktable.gui->ppd_thb;

  for(int k=0; k<ps->imgs.count; k++)
  {
    dt_image_box *img = &ps->imgs.box[k];

    if(img->imgid != -1)
    {
      cairo_surface_t *surf = NULL;

      // screen coordinate default to current box if there is no image
      dt_image_pos screen;

      dt_printing_setup_image(&ps->imgs, k, img->imgid, 100, 100, img->alignment);

      dt_printing_get_screen_pos(&ps->imgs, img, &screen);

      const dt_view_surface_value_t res =
        dt_view_image_get_surface(img->imgid, screen.width, screen.height, &surf, TRUE);

      if(res != DT_VIEW_SURFACE_OK)
      {
        // if the image is missing, we reload it again
        g_timeout_add(250, _expose_again, ps);
        if(!ps->busy) dt_control_log_busy_enter();
        ps->busy = TRUE;
      }
      else
      {
        cairo_save(cr);
        cairo_translate(cr, screen.x, screen.y);
        cairo_scale(cr, scaler, scaler);
        cairo_set_source_surface(cr, surf, 0, 0);
        const double alpha = (ps->dragging || (ps->selected != -1 && ps->selected != k)) ? 0.25 : 1.0;
        cairo_paint_with_alpha(cr, alpha);
        cairo_surface_destroy(surf);
        cairo_restore(cr);
        if(ps->busy) dt_control_log_busy_leave();
        ps->busy = FALSE;
      }
    }

    if(k == ps->selected || img->imgid == -1)
    {
      cairo_set_source_rgba(cr, .4, .4, .4, 1.0);
      _cairo_rectangle(cr, (k == ps->selected) ? ps->sel_controls : 0,
                       img->screen.x, img->screen.y,
                       img->screen.x + img->screen.width, img->screen.y + img->screen.height);
      cairo_stroke(cr);
    }

    if(k == ps->imgs.motion_over)
    {
      cairo_set_source_rgba(cr, .2, .2, .2, 1.0);
      cairo_rectangle(cr, img->screen.x, img->screen.y, img->screen.width, img->screen.height);
      cairo_fill(cr);
    }
  }

  // now display new area if any
  if(ps->dragging || ps->selected != -1)
  {
    float dx1, dy1, dx2, dy2, dwidth, dheight; // displayed values
    float x1, y1, x2, y2;                      // box screen coordinates

    float pwidth, pheight;
    _get_page_dimension(&ps->prt, &pwidth, &pheight);

    if(ps->dragging)
    {
      x1      = ps->x1;
      y1      = ps->y1;
      x2      = ps->x2;
      y2      = ps->y2;

      dx1     = _hscreen_to_mm(ps, ps->x1, TRUE) * units[ps->unit];
      dy1     = _vscreen_to_mm(ps, ps->y1, TRUE) * units[ps->unit];
      dx2     = _hscreen_to_mm(ps, ps->x2, TRUE) * units[ps->unit];
      dy2     = _vscreen_to_mm(ps, ps->y2, TRUE) * units[ps->unit];
      dwidth  = fabsf(dx2 - dx1);
      dheight = fabsf(dy2 - dy1);
    }
    else
    {
      const dt_image_box *box = &ps->imgs.box[ps->selected];

      // we could use a simpler solution but we want to use the same formulae used
      // to fill the editable box values to avoid discrepancies between values due
      // to rounding errors.

      dx1     = _percent_unit_of(ps, pwidth, box->pos.x);
      dy1     = _percent_unit_of(ps, pheight, box->pos.y);
      dwidth  = _percent_unit_of(ps, pwidth, box->pos.width);
      dheight = _percent_unit_of(ps, pheight, box->pos.height);
      dx2     = dx1 + dwidth;
      dy2     = dy1 + dheight;

      x1 = box->screen.x;
      y1 = box->screen.y;
      x2 = box->screen.x + box->screen.width;
      y2 = box->screen.y + box->screen.height;
    }

    cairo_set_source_rgba(cr, .4, .4, .4, 1.0);
    _cairo_rectangle(cr, ps->sel_controls, x1, y1, x2, y2);

    // display corner coordinates
    // FIXME: here and elsewhere eliminate hardcoded RGB values -- use CSS
    char dimensions[16];
    PangoLayout *layout;
    PangoRectangle ext;
    PangoFontDescription *desc = pango_font_description_copy_static(darktable.bauhaus->pango_font_desc);
    pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
    pango_font_description_set_absolute_size(desc, DT_PIXEL_APPLY_DPI(16) * PANGO_SCALE);
    layout = pango_cairo_create_layout(cr);
    pango_layout_set_font_description(layout, desc);
    const double text_h = DT_PIXEL_APPLY_DPI(16+2);
    const double margin = DT_PIXEL_APPLY_DPI(6);
    const double dash = DT_PIXEL_APPLY_DPI(4.0);
    int n_digits;
    char *precision;
    _precision_by_unit(ps->unit, &n_digits, NULL, &precision);
    double xp, yp;

    yp = y1 + (y2 - y1 - text_h) * 0.5;

    if(x1 >= ps->imgs.screen.page.x && x1 <= (ps->imgs.screen.page.x + ps->imgs.screen.page.width))
    {
      snprintf(dimensions, sizeof(dimensions), precision, dx1);
      pango_layout_set_text(layout, dimensions, -1);
      pango_layout_get_pixel_extents(layout, NULL, &ext);
      xp = ps->imgs.screen.page.x + (x1 - text_h - ps->imgs.screen.page.x - ext.width) * 0.5;
      if(xp < ps->imgs.screen.page.x + 3 * margin)
      {
        xp = x1 + 2 * margin;
        // somewhat hacky, assumes that all numeric labels are about
        // the same width
        yp = MIN(y2 - text_h, yp + ext.width + 0.5 * text_h + margin * 3);
      }
      cairo_set_source_rgba(cr, .7, .7, .7, .9);
      cairo_move_to(cr, ps->imgs.screen.page.x, yp + text_h * 0.5);
      cairo_line_to(cr, x1, yp + text_h * 0.5);
      cairo_stroke_preserve(cr);
      cairo_set_source_rgba(cr, .5, .5, .5, .9);
      cairo_set_dash(cr, &dash, 1, dash);
      cairo_stroke(cr);
      cairo_set_dash(cr, NULL, 0, 0);
      dt_gui_draw_rounded_rectangle(cr, ext.width + 2 * margin, text_h + 2 * margin, xp - margin, yp - margin);
      cairo_set_source_rgb(cr, .8, .8, .8);
      cairo_move_to(cr, xp, yp);
      pango_cairo_show_layout(cr, layout);
    }

    if(x2 >= ps->imgs.screen.page.x && x2 <= (ps->imgs.screen.page.x + ps->imgs.screen.page.width))
    {
      snprintf(dimensions, sizeof(dimensions), precision, pwidth * units[ps->unit] - dx2);
      pango_layout_set_text(layout, dimensions, -1);
      pango_layout_get_pixel_extents(layout, NULL, &ext);
      xp = x2 + (ps->imgs.screen.page.x + ps->imgs.screen.page.width - x2 - ext.width) * 0.5;
      if(xp + ext.width + margin > ps->imgs.screen.page.x + ps->imgs.screen.page.width)
        xp = x2 - ext.width - 2 * margin;
      cairo_set_source_rgba(cr, .7, .7, .7, .9);
      cairo_move_to(cr, x2, yp + text_h * 0.5);
      cairo_line_to(cr, ps->imgs.screen.page.x + ps->imgs.screen.page.width, yp + text_h * 0.5);
      cairo_stroke_preserve(cr);
      cairo_set_source_rgba(cr, .5, .5, .5, .9);
      cairo_set_dash(cr, &dash, 1, dash);
      cairo_stroke(cr);
      cairo_set_dash(cr, NULL, 0, 0);
      dt_gui_draw_rounded_rectangle(cr, ext.width + 2 * margin, text_h + 2 * margin,
                                    xp - margin, yp - margin);
      cairo_set_source_rgb(cr, .8, .8, .8);
      cairo_move_to(cr, xp, yp);
      pango_cairo_show_layout(cr, layout);
    }

    xp = x1 + (x2 - x1 - text_h) * 0.5;

    if(y1 >= ps->imgs.screen.page.y && y1 <= (ps->imgs.screen.page.y + ps->imgs.screen.page.height))
    {
      snprintf(dimensions, sizeof(dimensions), precision, dy1);
      pango_layout_set_text(layout, dimensions, -1);
      pango_layout_get_pixel_extents(layout, NULL, &ext);
      yp = ps->imgs.screen.page.y + (y1 - text_h - ps->imgs.screen.page.y - ext.width) * 0.5;
      if(yp < ps->imgs.screen.page.y + 3 * margin)
      {
        xp = MIN(x2 - text_h, xp + ext.width + 0.5 * text_h + margin * 3);
        yp = y1 + 2 * margin;
      }
      cairo_set_source_rgba(cr, .7, .7, .7, .9);
      cairo_move_to(cr, xp + text_h * 0.5, ps->imgs.screen.page.y);
      cairo_line_to(cr, xp + text_h * 0.5, y1);
      cairo_stroke_preserve(cr);
      cairo_set_source_rgba(cr, .5, .5, .5, .9);
      cairo_set_dash(cr, &dash, 1, dash);
      cairo_stroke(cr);
      cairo_set_dash(cr, NULL, 0, 0);
      dt_gui_draw_rounded_rectangle(cr, text_h + 2 * margin, ext.width + 2 * margin,
                                    xp - margin, yp - margin);
      cairo_set_source_rgb(cr, .8, .8, .8);
      cairo_move_to(cr, xp + text_h * 0.5, yp + ext.width * 0.5);
      cairo_save(cr);
      cairo_rotate(cr, -M_PI_2);
      cairo_rel_move_to(cr, -0.5 * ext.width, -0.5 * text_h);
      pango_cairo_update_layout(cr, layout);
      pango_cairo_show_layout(cr, layout);
      cairo_restore(cr);
    }

    if(y2 >= ps->imgs.screen.page.y && y2 <= (ps->imgs.screen.page.y + ps->imgs.screen.page.height))
    {
      snprintf(dimensions, sizeof(dimensions), precision, pheight * units[ps->unit] - dy2);
      pango_layout_set_text(layout, dimensions, -1);
      pango_layout_get_pixel_extents(layout, NULL, &ext);
      yp = y2 + (ps->imgs.screen.page.y + ps->imgs.screen.page.height - y2 - ext.width) * 0.5;
      if(yp + ext.width + margin > ps->imgs.screen.page.y + ps->imgs.screen.page.height)
        yp = y2 - ext.width - 2 * margin;
      cairo_set_source_rgba(cr, .7, .7, .7, .9);
      cairo_move_to(cr, xp + text_h * 0.5, y2);
      cairo_line_to(cr, xp + text_h * 0.5, ps->imgs.screen.page.y + ps->imgs.screen.page.height);
      cairo_stroke_preserve(cr);
      cairo_set_source_rgba(cr, .5, .5, .5, .9);
      cairo_set_dash(cr, &dash, 1, dash);
      cairo_stroke(cr);
      cairo_set_dash(cr, NULL, 0, 0);
      dt_gui_draw_rounded_rectangle(cr, text_h + 2 * margin, ext.width + 2 * margin,
                                    xp - margin, yp - margin);
      cairo_set_source_rgb(cr, .8, .8, .8);
      cairo_move_to(cr, xp + text_h * 0.5, yp + ext.width * 0.5);
      cairo_save(cr);
      cairo_rotate(cr, -M_PI_2);
      cairo_rel_move_to(cr, -0.5 * ext.width, -0.5 * text_h);
      pango_cairo_update_layout(cr, layout);
      pango_cairo_show_layout(cr, layout);
      cairo_restore(cr);
    }

    // display width and height
    snprintf(dimensions, sizeof(dimensions), precision, dwidth);
    pango_layout_set_text(layout, dimensions, -1);
    pango_layout_get_pixel_extents(layout, NULL, &ext);
    xp = (x1 + x2 - ext.width) * .5;
    if(y1 > text_h * 0.5 + margin)
      yp = y1 - text_h * 0.5;
    else
      yp = y1 + text_h - 2 * margin;
    cairo_set_source_rgba(cr, .5, .5, .5, .9);
    dt_gui_draw_rounded_rectangle(cr, ext.width + 2 * margin, text_h + 2 * margin,
                                  xp - margin, yp - margin);
    cairo_set_source_rgb(cr, .8, .8, .8);
    cairo_move_to(cr, xp, yp);
    pango_cairo_show_layout(cr, layout);

    snprintf(dimensions, sizeof(dimensions), precision, dheight);
    pango_layout_set_text(layout, dimensions, -1);
    pango_layout_get_pixel_extents(layout, NULL, &ext);
    if(x1 > text_h * 0.5 + margin)
      xp = x1 - text_h * 0.5;
    else
      xp = x1 + text_h - 2 * margin;
    yp = (y1 + y2) * .5;
    cairo_set_source_rgba(cr, .5, .5, .5, .9);
    dt_gui_draw_rounded_rectangle(cr, text_h + 2 * margin, ext.width + 2 * margin,
                                  xp - margin, yp - margin - 0.5 * ext.width);
    cairo_set_source_rgb(cr, .8, .8, .8);
    cairo_move_to(cr, xp + text_h * 0.5, yp);
    cairo_save(cr);
    cairo_rotate(cr, -M_PI_2);
    cairo_rel_move_to(cr, -0.5 * ext.width, -0.5 * text_h);
    pango_cairo_update_layout(cr, layout);
    pango_cairo_show_layout(cr, layout);
    cairo_restore(cr);

    pango_font_description_free(desc);
    g_object_unref(layout);
    g_free(precision);
  }

  if(ps->imgs.screen.borderless)
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ps->borderless), TRUE);
  else
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ps->borderless), FALSE);
}

static void _width_changed(GtkWidget *widget, gpointer user_data)
{
  if(darktable.gui->reset) return;

  dt_lib_print_settings_t *ps = (dt_lib_print_settings_t *)user_data;

  const float nv = gtk_spin_button_get_value(GTK_SPIN_BUTTON(widget));
  const float nv_mm = nv / units[ps->unit];

  dt_image_box *box = &ps->imgs.box[ps->last_selected];

  dt_printing_setup_box(&ps->imgs, ps->last_selected,
                        box->screen.x, box->screen.y,
                        _mm_to_hscreen(ps, nv_mm, FALSE), box->screen.height);

  ps->has_changed = TRUE;
  dt_control_queue_redraw_center();
}

static void _height_changed(GtkWidget *widget, gpointer user_data)
{
  if(darktable.gui->reset) return;

  dt_lib_print_settings_t *ps = (dt_lib_print_settings_t *)user_data;

  const float nv = gtk_spin_button_get_value(GTK_SPIN_BUTTON(widget));
  const float nv_mm = nv / units[ps->unit];

  dt_image_box *box = &ps->imgs.box[ps->last_selected];

  dt_printing_setup_box(&ps->imgs, ps->last_selected,
                        box->screen.x, box->screen.y,
                        box->screen.width, _mm_to_vscreen(ps, nv_mm, FALSE));

  ps->has_changed = TRUE;
  dt_control_queue_redraw_center();
}

static void _x_changed(GtkWidget *widget, gpointer user_data)
{
  if(darktable.gui->reset) return;

  dt_lib_print_settings_t *ps = (dt_lib_print_settings_t *)user_data;

  const float nv = gtk_spin_button_get_value(GTK_SPIN_BUTTON(widget));
  const float nv_mm = nv / units[ps->unit];

  dt_image_box *box = &ps->imgs.box[ps->last_selected];

  dt_printing_setup_box(&ps->imgs, ps->last_selected,
                        _mm_to_hscreen(ps, nv_mm, TRUE), box->screen.y,
                        box->screen.width, box->screen.height);

  ps->has_changed = TRUE;
  dt_control_queue_redraw_center();
}

static void _y_changed(GtkWidget *widget, gpointer user_data)
{
  if(darktable.gui->reset) return;

  dt_lib_print_settings_t *ps = (dt_lib_print_settings_t *)user_data;

  const float nv = gtk_spin_button_get_value(GTK_SPIN_BUTTON(widget));
  const float nv_mm = nv / units[ps->unit];

  dt_image_box *box = &ps->imgs.box[ps->last_selected];

  dt_printing_setup_box(&ps->imgs, ps->last_selected,
                        box->screen.x, _mm_to_vscreen(ps, nv_mm, TRUE),
                        box->screen.width, box->screen.height);

  ps->has_changed = TRUE;
  dt_control_queue_redraw_center();
}

void gui_init(dt_lib_module_t *self)
{
  dt_lib_print_settings_t *d = (dt_lib_print_settings_t*)malloc(sizeof(dt_lib_print_settings_t));
  self->data = d;
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
  dt_gui_add_help_link(self->widget, dt_get_help_url("print_overview"));

  char datadir[PATH_MAX] = { 0 };
  char confdir[PATH_MAX] = { 0 };
  dt_loc_get_user_config_dir(confdir, sizeof(confdir));
  dt_loc_get_datadir(datadir, sizeof(datadir));
  char *system_profile_dir = g_build_filename(datadir, "color", "out", NULL);
  char *user_profile_dir = g_build_filename(confdir, "color", "out", NULL);

  GtkWidget *label;

  d->paper_list = NULL;
  d->media_list = NULL;
  d->unit = 0;
  d->width = d->height = NULL;
  d->v_piccprofile = NULL;
  d->v_iccprofile = NULL;
  d->v_style = NULL;
  d->creation = d->dragging = FALSE;
  d->selected = -1;
  d->last_selected = -1;
  d->has_changed = FALSE;

  dt_init_print_info(&d->prt);
  dt_view_print_settings(darktable.view_manager, &d->prt, &d->imgs);

  d->profiles = _get_profiles();

  d->imgs.motion_over = -1;

  const char *str = dt_conf_get_string_const("plugins/print/print/unit");
  const char **names = _unit_names;
  for(_unit_t i=0; *names; names++, i++)
    if(g_strcmp0(str, *names) == 0)
      d->unit = i;

  dt_printing_clear_boxes(&d->imgs);

  // set all margins + unit from settings

  const float top_b = dt_conf_get_float("plugins/print/print/top_margin");
  const float bottom_b = dt_conf_get_float("plugins/print/print/bottom_margin");
  const float left_b = dt_conf_get_float("plugins/print/print/left_margin");
  const float right_b = dt_conf_get_float("plugins/print/print/right_margin");

  d->prt.page.margin_top = _to_mm(d, top_b);
  d->prt.page.margin_bottom = _to_mm(d, bottom_b);
  d->prt.page.margin_left = _to_mm(d, left_b);
  d->prt.page.margin_right = _to_mm(d, right_b);

  //  create the spin-button now as values could be set when the printer has no hardware margin

  // FIXME: set digits/increments on all of these by calling _unit_changed() later?
  int n_digits;
  float incr;
  _precision_by_unit(d->unit, &n_digits, &incr, NULL);

  d->b_top    = gtk_spin_button_new_with_range(0, 1000, incr);
  d->b_left   = gtk_spin_button_new_with_range(0, 1000, incr);
  d->b_right  = gtk_spin_button_new_with_range(0, 1000, incr);
  d->b_bottom = gtk_spin_button_new_with_range(0, 1000, incr);
  gtk_spin_button_set_digits(GTK_SPIN_BUTTON(d->b_top),    n_digits);
  gtk_spin_button_set_digits(GTK_SPIN_BUTTON(d->b_bottom), n_digits);
  gtk_spin_button_set_digits(GTK_SPIN_BUTTON(d->b_left),   n_digits);
  gtk_spin_button_set_digits(GTK_SPIN_BUTTON(d->b_right),  n_digits);

  d->b_x      = gtk_spin_button_new_with_range(0, 1000, incr);
  d->b_y      = gtk_spin_button_new_with_range(0, 1000, incr);
  d->b_width  = gtk_spin_button_new_with_range(0, 1000, incr);
  d->b_height = gtk_spin_button_new_with_range(0, 1000, incr);
  gtk_spin_button_set_digits(GTK_SPIN_BUTTON(d->b_x), n_digits);
  gtk_spin_button_set_digits(GTK_SPIN_BUTTON(d->b_y), n_digits);
  gtk_spin_button_set_digits(GTK_SPIN_BUTTON(d->b_width), n_digits);
  gtk_spin_button_set_digits(GTK_SPIN_BUTTON(d->b_height), n_digits);

  d->grid_size = gtk_spin_button_new_with_range(0, 100, incr);
  gtk_spin_button_set_digits(GTK_SPIN_BUTTON(d->grid_size),  n_digits);

  gtk_entry_set_alignment(GTK_ENTRY(d->b_top), 1);
  gtk_entry_set_alignment(GTK_ENTRY(d->b_left), 1);
  gtk_entry_set_alignment(GTK_ENTRY(d->b_right), 1);
  gtk_entry_set_alignment(GTK_ENTRY(d->b_bottom), 1);

  gtk_entry_set_alignment(GTK_ENTRY(d->b_x), 1);
  gtk_entry_set_alignment(GTK_ENTRY(d->b_y), 1);
  gtk_entry_set_alignment(GTK_ENTRY(d->b_width), 1);
  gtk_entry_set_alignment(GTK_ENTRY(d->b_height), 1);

  gtk_entry_set_alignment(GTK_ENTRY(d->grid_size), 1);


  ////////////////////////// PRINTER SETTINGS

  // create papers combo as filled when adding printers
  d->papers = dt_bauhaus_combobox_new_action(DT_ACTION(self));

  label = dt_ui_section_label_new(_("printer"));
  gtk_box_pack_start(GTK_BOX(self->widget), label, TRUE, TRUE, 0);
  dt_gui_add_help_link(self->widget, dt_get_help_url("print_settings_printer"));
  d->printers = dt_bauhaus_combobox_new_action(DT_ACTION(self));

  gtk_box_pack_start(GTK_BOX(self->widget), d->printers, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(d->printers), "value-changed", G_CALLBACK(_printer_changed), self);

  //// media

  d->media = dt_bauhaus_combobox_new_action(DT_ACTION(self));

  dt_bauhaus_widget_set_label(d->media, N_("printer"), N_("media"));

  g_signal_connect(G_OBJECT(d->media), "value-changed", G_CALLBACK(_media_changed), self);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(d->media), TRUE, TRUE, 0);

  //  Add printer profile combo

  d->pprofile = dt_bauhaus_combobox_new_action(DT_ACTION(self));
  dt_bauhaus_widget_set_label(d->pprofile, N_("printer"), N_("profile"));

  int combo_idx, n;

  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(d->pprofile), TRUE, TRUE, 0);
  int printer_profile_type = dt_conf_get_int("plugins/print/printer/icctype");
  const char *printer_profile = dt_conf_get_string_const("plugins/print/printer/iccprofile");
  combo_idx = -1;
  n = 0;

  dt_bauhaus_combobox_add(d->pprofile, _("color management in printer driver"));
  for(const GList *l = d->profiles; l; l = g_list_next(l))
  {
    dt_lib_export_profile_t *prof = (dt_lib_export_profile_t *)l->data;
    // do not add built-in profiles, these are in no way for printing
    if(prof->type == DT_COLORSPACE_FILE)
    {
      dt_bauhaus_combobox_add(d->pprofile, prof->name);
      prof->ppos = ++n;
      if(prof->type == printer_profile_type &&
        (prof->type != DT_COLORSPACE_FILE || !g_strcmp0(prof->filename, printer_profile)))
      {
        g_free(d->v_piccprofile);
        d->v_picctype = printer_profile_type;
        d->v_piccprofile = g_strdup(printer_profile);
        combo_idx = n;
      }
    }
  }

  // profile not found, maybe a profile has been removed? revert to none
  if(combo_idx == -1)
  {
    dt_conf_set_int("plugins/print/printer/icctype", DT_COLORSPACE_NONE);
    dt_conf_set_string("plugins/print/printer/iccprofile", "");
    g_free(d->v_piccprofile);
    d->v_picctype = DT_COLORSPACE_NONE;
    d->v_piccprofile = g_strdup("");
    combo_idx = 0;
  }
  dt_bauhaus_combobox_set(d->pprofile, combo_idx);

  char *tooltip = g_strdup_printf(_("printer ICC profiles in %s or %s"), user_profile_dir, system_profile_dir);
  gtk_widget_set_tooltip_text(d->pprofile, tooltip);
  g_free(tooltip);

  g_signal_connect(G_OBJECT(d->pprofile), "value-changed", G_CALLBACK(_printer_profile_changed), (gpointer)self);

  //  Add printer intent combo

  d->pintent = dt_bauhaus_combobox_new_action(DT_ACTION(self));
  dt_bauhaus_widget_set_label(d->pintent, N_("printer"), N_("intent"));
  dt_bauhaus_combobox_add(d->pintent, _("perceptual"));
  dt_bauhaus_combobox_add(d->pintent, _("relative colorimetric"));
  dt_bauhaus_combobox_add(d->pintent, C_("rendering intent", "saturation"));
  dt_bauhaus_combobox_add(d->pintent, _("absolute colorimetric"));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(d->pintent), TRUE, TRUE, 0);

  d->v_pintent = dt_conf_get_int("plugins/print/printer/iccintent");
  dt_bauhaus_combobox_set(d->pintent, d->v_pintent);

  g_signal_connect (G_OBJECT (d->pintent), "value-changed", G_CALLBACK (_printer_intent_callback), (gpointer)self);
  d->prt.printer.intent = d->v_pintent;

  d->black_point_compensation = gtk_check_button_new_with_label(_("black point compensation"));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(d->black_point_compensation), TRUE, FALSE, 0);
  g_signal_connect(d->black_point_compensation, "toggled", G_CALLBACK(_printer_bpc_callback), (gpointer)self);

  d->v_black_point_compensation = dt_conf_get_bool("plugins/print/print/black_point_compensation");
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->black_point_compensation), d->v_black_point_compensation);

  gtk_widget_set_tooltip_text(d->black_point_compensation,
                              _("activate black point compensation when applying the printer profile"));

  gtk_widget_set_sensitive(GTK_WIDGET(d->black_point_compensation), combo_idx==0?FALSE:TRUE);

  ////////////////////////// PAGE SETTINGS

  label = dt_ui_section_label_new(_("page"));
  gtk_box_pack_start(GTK_BOX(self->widget), label, TRUE, TRUE, 0);
  dt_gui_add_help_link(self->widget, dt_get_help_url("print_settings_page"));

  //// papers

  dt_bauhaus_widget_set_label(d->papers, NULL, N_("paper size"));

  g_signal_connect(G_OBJECT(d->papers), "value-changed", G_CALLBACK(_paper_changed), self);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(d->papers), TRUE, TRUE, 0);

  //// portrait / landscape

  d->orientation = dt_bauhaus_combobox_new_action(DT_ACTION(self));
  dt_bauhaus_widget_set_label(d->orientation, NULL, N_("orientation"));
  dt_bauhaus_combobox_add(d->orientation, _("portrait"));
  dt_bauhaus_combobox_add(d->orientation, _("landscape"));
  g_signal_connect(G_OBJECT(d->orientation), "value-changed", G_CALLBACK(_orientation_changed), self);
  dt_bauhaus_combobox_set(d->orientation, d->prt.page.landscape?1:0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(d->orientation), TRUE, TRUE, 0);

  // NOTE: units has no label, which makes for cleaner UI but means that no action can be assigned
  GtkWidget *ucomb =
    dt_bauhaus_combobox_new_full(DT_ACTION(self), NULL, NULL,
                                 _("measurement units"),
                                 d->unit, (GtkCallback)_unit_changed, self,
                                 _unit_names);
  gtk_box_pack_start(GTK_BOX(self->widget), ucomb, TRUE, TRUE, 0);

  //// image dimensions, create them now as we need them

  GtkWidget *hboxdim = gtk_box_new(GTK_ORIENTATION_HORIZONTAL,0);
  label = gtk_label_new(_("image width/height"));
  gtk_box_pack_start(GTK_BOX(hboxdim), GTK_WIDGET(label), TRUE, TRUE, DT_PIXEL_APPLY_DPI(3));
  d->width = gtk_label_new(_("width"));
  gtk_box_pack_start(GTK_BOX(hboxdim), GTK_WIDGET(d->width), TRUE, TRUE, 0);
  label = gtk_label_new(_(" x "));
  gtk_box_pack_start(GTK_BOX(hboxdim), GTK_WIDGET(label), TRUE, TRUE, 0);
  d->height = gtk_label_new(_("height"));
  gtk_box_pack_start(GTK_BOX(hboxdim), GTK_WIDGET(d->height), TRUE, TRUE, 0);

  //// image information (downscale/upscale)

  GtkWidget *hboxinfo = gtk_box_new(GTK_ORIENTATION_HORIZONTAL,0);
  label = gtk_label_new(_("scale factor"));
  gtk_box_pack_start(GTK_BOX(hboxinfo), GTK_WIDGET(label), TRUE, TRUE, DT_PIXEL_APPLY_DPI(3));
  d->info = gtk_label_new("1.0");
  gtk_box_pack_start(GTK_BOX(hboxinfo), GTK_WIDGET(d->info), TRUE, TRUE, 0);
  gtk_widget_set_tooltip_text(hboxinfo,
               _("image scale factor from native printer DPI:\n"
                 " < 1 means that it is downscaled (best quality)\n"
                 " > 1 means that the image is upscaled\n"
                 " a too large value may result in poor print quality"));

  //// borders

  GtkGrid *bds = GTK_GRID(gtk_grid_new());
  gtk_grid_set_row_spacing(bds, DT_PIXEL_APPLY_DPI(3));
  gtk_grid_set_column_spacing(bds, DT_PIXEL_APPLY_DPI(3));

  d->lock_activated = FALSE;

  //d->b_top  = gtk_spin_button_new_with_range(0, 10000, 1);
  gtk_widget_set_tooltip_text(GTK_WIDGET(d->b_top), _("top margin"));
  gtk_grid_attach(bds, GTK_WIDGET(d->b_top), 1, 0, 1, 1);

  //d->b_left  = gtk_spin_button_new_with_range(0, 10000, 1);
  gtk_widget_set_tooltip_text(GTK_WIDGET(d->b_left), _("left margin"));
  gtk_grid_attach(bds, GTK_WIDGET(d->b_left), 0, 1, 1, 1);

  d->lock_button = GTK_TOGGLE_BUTTON(gtk_toggle_button_new_with_label(_("lock")));
  gtk_widget_set_tooltip_text(GTK_WIDGET(d->lock_button), _("change all margins uniformly"));
  gtk_grid_attach(bds, GTK_WIDGET(d->lock_button), 1, 1, 1, 1);

  //d->b_right  = gtk_spin_button_new_with_range(0, 10000, 1);
  gtk_widget_set_tooltip_text(GTK_WIDGET(d->b_right), _("right margin"));
  gtk_grid_attach(bds, GTK_WIDGET(d->b_right), 2, 1, 1, 1);

  //d->b_bottom  = gtk_spin_button_new_with_range(0, 10000, 1);
  gtk_widget_set_tooltip_text(GTK_WIDGET(d->b_bottom), _("bottom margin"));
  gtk_grid_attach(bds, GTK_WIDGET(d->b_bottom), 1, 2, 1, 1);

  gtk_widget_set_halign(GTK_WIDGET(bds), GTK_ALIGN_CENTER);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(bds), TRUE, TRUE, 0);

  gtk_spin_button_set_value(GTK_SPIN_BUTTON(d->b_top), top_b);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(d->b_bottom), bottom_b);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(d->b_left), left_b);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(d->b_right), right_b);

  g_signal_connect(G_OBJECT (d->b_top), "value-changed",
                   G_CALLBACK (_top_border_callback), self);
  g_signal_connect(G_OBJECT (d->b_bottom), "value-changed",
                   G_CALLBACK (_bottom_border_callback), self);
  g_signal_connect(G_OBJECT (d->b_left), "value-changed",
                   G_CALLBACK (_left_border_callback), self);
  g_signal_connect(G_OBJECT (d->b_right), "value-changed",
                   G_CALLBACK (_right_border_callback), self);
  g_signal_connect(G_OBJECT(d->lock_button), "toggled",
                   G_CALLBACK(_lock_callback), self);

  gtk_widget_set_halign(GTK_WIDGET(hboxdim), GTK_ALIGN_CENTER);
  gtk_widget_set_halign(GTK_WIDGET(hboxinfo), GTK_ALIGN_CENTER);

  const gboolean lock_active = dt_conf_get_bool("plugins/print/print/lock_borders");
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->lock_button), lock_active);

  // grid & snap grid
  {
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

    d->grid = gtk_check_button_new_with_label(_("display grid"));
    // d->grid_size = gtk_spin_button_new_with_range(0, 100, 0.1);
    gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(d->grid), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(d->grid_size), TRUE, TRUE, 0);

    gtk_spin_button_set_value(GTK_SPIN_BUTTON(d->grid_size),
                              dt_conf_get_float("plugins/print/print/grid_size") * units[d->unit]);

    gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(hbox), TRUE, TRUE, 0);

    d->snap_grid = gtk_check_button_new_with_label(_("snap to grid"));
    gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(d->snap_grid), TRUE, TRUE, 0);

    gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(vbox), TRUE, TRUE, 0);

    g_signal_connect(G_OBJECT(d->grid_size), "value-changed", G_CALLBACK(_grid_size_changed), self);
    g_signal_connect(G_OBJECT(d->grid), "toggled", G_CALLBACK(_grid_callback), self);
    g_signal_connect(d->snap_grid, "toggled", G_CALLBACK(_snap_grid_callback), (gpointer)self);
  }

  d->borderless = gtk_check_button_new_with_label(_("borderless mode required"));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(d->borderless), TRUE, TRUE, 0);
  gtk_widget_set_tooltip_text(d->borderless,
                              _("indicates that the borderless mode should be activated\n"
                                "in the printer driver because the selected margins are\n"
                                "below the printer hardware margins"));
  gtk_widget_set_sensitive(d->borderless, FALSE);

  // pack image dimension hbox here

  label = dt_ui_section_label_new(_("image layout"));
  gtk_box_pack_start(GTK_BOX(self->widget), label, TRUE, TRUE, 0);
  dt_gui_add_help_link(self->widget, dt_get_help_url("print_image_layout"));

  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hboxdim), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hboxinfo), TRUE, TRUE, 0);

  //// alignments

  // Auto-fit: Create the 3x3 gtk table toggle button table...
  GtkGrid *bat = GTK_GRID(gtk_grid_new());
  gtk_grid_set_row_spacing(bat, DT_PIXEL_APPLY_DPI(3));
  gtk_grid_set_column_spacing(bat, DT_PIXEL_APPLY_DPI(3));
  for(int i=0; i<9; i++)
  {
    d->dtba[i]
        = DTGTK_TOGGLEBUTTON(dtgtk_togglebutton_new(dtgtk_cairo_paint_alignment, (CPF_SPECIAL_FLAG << i), NULL));
    gtk_grid_attach (GTK_GRID (bat), GTK_WIDGET (d->dtba[i]), (i%3), i/3, 1, 1);
    g_signal_connect (G_OBJECT (d->dtba[i]), "toggled",G_CALLBACK (_alignment_callback), self);
  }

  GtkWidget *hbox22 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL,0);
  GtkWidget *label4 = gtk_label_new(_("alignment"));
  gtk_box_pack_start(GTK_BOX(hbox22),GTK_WIDGET(label4),TRUE,TRUE,0);
  gtk_box_pack_start(GTK_BOX(hbox22), GTK_WIDGET(bat), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hbox22), TRUE, TRUE, 0);

  // Manual fit
  GtkWidget *hfitbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  GtkWidget *mfitbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

  GtkGrid *fitbut = GTK_GRID(gtk_grid_new());
  gtk_grid_set_row_spacing(fitbut, DT_PIXEL_APPLY_DPI(3));
  gtk_grid_set_column_spacing(fitbut, DT_PIXEL_APPLY_DPI(3));
  gtk_grid_set_column_homogeneous(fitbut, TRUE);
  gtk_grid_set_row_homogeneous(fitbut, TRUE);

  GtkWidget *bnew = dt_action_button_new(self, N_("new image area"), _page_new_area_clicked, self,
                                         _("add a new image area on the page\n"
                                           "click and drag on the page to place the area\n"
                                           "drag&drop image from film strip on it"), 0, 0);

  d->del = dt_action_button_new(self, N_("delete image area"), _page_delete_area_clicked, self,
                                _("delete the currently selected image area"), 0, 0);
  gtk_widget_set_sensitive(d->del, FALSE);

  GtkWidget *bclear = dt_action_button_new(self, N_("clear layout"), _page_clear_area_clicked, self,
                                           _("remove all image areas from the page"), 0, 0);

  gtk_grid_attach(fitbut, GTK_WIDGET(bnew), 0, 0, 2, 1);
  gtk_grid_attach(fitbut, GTK_WIDGET(d->del), 0, 1, 1, 1);
  gtk_grid_attach(fitbut, GTK_WIDGET(bclear), 1, 1, 1, 1);

  gtk_box_pack_start(GTK_BOX(mfitbox), GTK_WIDGET(fitbut), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(hfitbox), GTK_WIDGET(mfitbox), TRUE, TRUE, 0);

  // X x Y
  GtkWidget *box;
  // FIXME: add labels to x/y/width/height as otherwise are obscure -- and there is the horizontal space to do this

  box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL,0);
  // d->b_x = gtk_spin_button_new_with_range(0, 1000, 1);
  gtk_widget_set_tooltip_text(d->b_x, _("image area x origin (in current unit)"));
  gtk_entry_set_width_chars(GTK_ENTRY(d->b_x), 5);

  // d->b_y = gtk_spin_button_new_with_range(0, 1000, 1);
  gtk_widget_set_tooltip_text(d->b_y, _("image area y origin (in current unit)"));
  gtk_entry_set_width_chars(GTK_ENTRY(d->b_y), 5);

  gtk_box_pack_start(GTK_BOX(box), GTK_WIDGET(d->b_x), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(box), GTK_WIDGET(d->b_y), TRUE, TRUE, 0);

  gtk_box_pack_start(GTK_BOX(hfitbox), GTK_WIDGET(box), TRUE, TRUE, 0);

  // width x height
  box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL,0);
  // d->b_width = gtk_spin_button_new_with_range(0, 1000, 1);
  gtk_widget_set_tooltip_text(d->b_width, _("image area width (in current unit)"));
  gtk_entry_set_width_chars(GTK_ENTRY(d->b_width), 5);

  // d->b_height = gtk_spin_button_new_with_range(0, 1000, 1);
  gtk_widget_set_tooltip_text(d->b_height, _("image area height (in current unit)"));
  gtk_entry_set_width_chars(GTK_ENTRY(d->b_height), 5);

  gtk_box_pack_start(GTK_BOX(box), GTK_WIDGET(d->b_width), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(box), GTK_WIDGET(d->b_height), TRUE, TRUE, 0);

  gtk_box_pack_start(GTK_BOX(hfitbox), GTK_WIDGET(box), TRUE, TRUE, 0);

  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hfitbox), TRUE, TRUE, 0);

  gtk_widget_add_events(d->b_x, GDK_BUTTON_PRESS_MASK);
  gtk_widget_add_events(d->b_y, GDK_BUTTON_PRESS_MASK);
  gtk_widget_add_events(d->b_width, GDK_BUTTON_PRESS_MASK);
  gtk_widget_add_events(d->b_height, GDK_BUTTON_PRESS_MASK);

  g_signal_connect(G_OBJECT(d->b_x), "value-changed", G_CALLBACK(_x_changed), (gpointer)d);
  g_signal_connect(G_OBJECT(d->b_y), "value-changed", G_CALLBACK(_y_changed), (gpointer)d);
  g_signal_connect(G_OBJECT(d->b_width), "value-changed", G_CALLBACK(_width_changed), (gpointer)d);
  g_signal_connect(G_OBJECT(d->b_height), "value-changed", G_CALLBACK(_height_changed), (gpointer)d);

  ////////////////////////// PRINT SETTINGS

  label = dt_ui_section_label_new(_("print settings"));
  gtk_box_pack_start(GTK_BOX(self->widget), label, TRUE, TRUE, 0);
  dt_gui_add_help_link(self->widget, dt_get_help_url("print_settings"));

  //  Add export profile combo

  d->profile = dt_bauhaus_combobox_new_action(DT_ACTION(self));
  dt_bauhaus_widget_set_label(d->profile, NULL, N_("profile"));

  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(d->profile), TRUE, TRUE, 0);
  dt_bauhaus_combobox_add(d->profile, _("image settings"));

  const int icctype = dt_conf_get_int("plugins/print/print/icctype");
  const gchar *iccprofile = dt_conf_get_string_const("plugins/print/print/iccprofile");
  combo_idx = -1;
  n = 0;

  for(const GList *l = d->profiles; l; l = g_list_next(l))
  {
    dt_lib_export_profile_t *prof = (dt_lib_export_profile_t *)l->data;
    dt_bauhaus_combobox_add(d->profile, prof->name);
    prof->pos = ++n;
    if(prof->type == icctype
       && (prof->type != DT_COLORSPACE_FILE || !g_strcmp0(prof->filename, iccprofile)))
    {
      g_free(d->v_iccprofile);
      d->v_icctype = icctype;
      d->v_iccprofile = g_strdup(iccprofile);
      combo_idx = n;
    }
  }

  if(combo_idx == -1)
  {
    dt_conf_set_int("plugins/print/print/icctype", DT_COLORSPACE_NONE);
    dt_conf_set_string("plugins/print/print/iccprofile", "");
    g_free(d->v_iccprofile);
    d->v_icctype = DT_COLORSPACE_NONE;
    d->v_iccprofile = g_strdup("");
    combo_idx = 0;
  }

  dt_bauhaus_combobox_set(d->profile, combo_idx);

  tooltip = g_strdup_printf(_("output ICC profiles in %s or %s"), user_profile_dir, system_profile_dir);
  gtk_widget_set_tooltip_text(d->profile, tooltip);
  g_free(tooltip);

  g_signal_connect(G_OBJECT(d->profile), "value-changed", G_CALLBACK(_profile_changed), (gpointer)self);

  //  Add export intent combo

  d->intent = dt_bauhaus_combobox_new_action(DT_ACTION(self));
  dt_bauhaus_widget_set_label(d->intent, NULL, N_("intent"));

  dt_bauhaus_combobox_add(d->intent, _("image settings"));
  dt_bauhaus_combobox_add(d->intent, _("perceptual"));
  dt_bauhaus_combobox_add(d->intent, _("relative colorimetric"));
  dt_bauhaus_combobox_add(d->intent, C_("rendering intent", "saturation"));
  dt_bauhaus_combobox_add(d->intent, _("absolute colorimetric"));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(d->intent), TRUE, TRUE, 0);

  dt_bauhaus_combobox_set(d->intent, dt_conf_get_int("plugins/print/print/iccintent") + 1);

  g_signal_connect (G_OBJECT (d->intent), "value-changed", G_CALLBACK (_intent_callback), (gpointer)self);

  //  Add export style combo

  d->style = dt_bauhaus_combobox_new_action(DT_ACTION(self));
  dt_bauhaus_widget_set_label(d->style, NULL, N_("style"));

  dt_bauhaus_combobox_add(d->style, _("none"));

  GList *styles = dt_styles_get_list("");
  const char *current_style = dt_conf_get_string_const("plugins/print/print/style");
  combo_idx = -1; n=0;

  for(const GList *st_iter = styles; st_iter; st_iter = g_list_next(st_iter))
  {
    dt_style_t *style=(dt_style_t *)st_iter->data;
    dt_bauhaus_combobox_add(d->style, style->name);
    n++;
    if(g_strcmp0(style->name,current_style)==0)
    {
      g_free(d->v_style);
      d->v_style = g_strdup(current_style);
      combo_idx=n;
    }
  }
  g_list_free_full(styles, dt_style_free);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(d->style), TRUE, TRUE, 0);
  gtk_widget_set_tooltip_text(d->style, _("temporary style to use while printing"));

  // style not found, maybe a style has been removed? revert to none
  if(combo_idx == -1)
  {
    dt_conf_set_string("plugins/print/print/style", "");
    g_free(d->v_style);
    d->v_style = g_strdup("");
    combo_idx=0;
  }
  dt_bauhaus_combobox_set(d->style, combo_idx);

  g_signal_connect (G_OBJECT (d->style), "value-changed",
                    G_CALLBACK (_style_callback),
                    (gpointer)self);

  //  Whether to add/replace style items

  d->style_mode = dt_bauhaus_combobox_new_action(DT_ACTION(self));
  dt_bauhaus_widget_set_label(d->style_mode, NULL, N_("mode"));

  dt_bauhaus_combobox_add(d->style_mode, _("replace history"));
  dt_bauhaus_combobox_add(d->style_mode, _("append history"));

  d->v_style_append = dt_conf_get_bool("plugins/print/print/style_append");
  dt_bauhaus_combobox_set(d->style_mode, d->v_style_append?1:0);

  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(d->style_mode), TRUE, TRUE, 0);
  gtk_widget_set_tooltip_text(d->style_mode,
                              _("whether the style items are appended to the history or replacing the history"));

  gtk_widget_set_sensitive(GTK_WIDGET(d->style_mode), combo_idx==0?FALSE:TRUE);

  g_signal_connect(G_OBJECT(d->style_mode), "value-changed", G_CALLBACK(_style_mode_changed), (gpointer)self);

  // Print button

  GtkWidget *button = dt_action_button_new(self, N_("print"), _print_button_clicked, self,
                                           _("print with current settings"), GDK_KEY_p, GDK_CONTROL_MASK);
  d->print_button = GTK_BUTTON(button);
  gtk_box_pack_start(GTK_BOX(self->widget), button, TRUE, TRUE, 0);
  dt_gui_add_help_link(button, dt_get_help_url("print_settings_button"));

  g_free(system_profile_dir);
  g_free(user_profile_dir);

  // Let's start the printer discovery now

  dt_printers_discovery(_new_printer_callback, self);
}

void *legacy_params(dt_lib_module_t *self, const void *const old_params, const size_t old_params_size,
                    const int old_version, int *new_version, size_t *new_size)
{
  if(old_version == 1)
  {
    // we added the profile type
    //
    // old format:
    //   char *printer
    //   char *paper
    //   int32_t landscape
    //   char *f_profile
    //   int32_t intent
    //   char *f_pprofile
    //   <rest>
    //
    // new format:
    //   char *printer
    //   char *paper
    //   int32_t landscape
    //   int32_t f_profile_type
    //   char *f_profile
    //   int32_t intent
    //   int32_t f_pprofile_type
    //   char *f_pprofile
    //   <rest>

    const char *buf = (const char *)old_params;

    // printer
    const char *printer = buf;
    const int32_t printer_len = strlen(printer) + 1;
    buf += printer_len;

    // paper
    const char *paper = buf;
    const int32_t paper_len = strlen(paper) + 1;
    buf += paper_len;

    // landscape
    const int32_t landscape = *(int32_t *)buf;
    buf +=  sizeof(int32_t);

    // profile
    const char *profile = buf;
    const int32_t profile_len = strlen(profile) + 1;
    buf += profile_len;

    // intent
    const int32_t intent = *(int32_t *)buf;
    buf += sizeof(int32_t);

    // pprofile
    const char *pprofile = buf;
    const int32_t pprofile_len = strlen(pprofile) + 1;
    buf += pprofile_len;


    // now we got all fields from the start of the buffer and buf points to the beginning or <rest>

    // find the new values for the two profiles
    dt_colorspaces_color_profile_type_t profile_type, pprofile_type;
    const char *profile_filename = "", *pprofile_filename = "";

    if(*profile == '\0' || !g_strcmp0(profile, "none"))
    {
      profile_type = DT_COLORSPACE_NONE;
    }
    else if(!g_strcmp0(profile, "sRGB"))
    {
      profile_type = DT_COLORSPACE_SRGB;
    }
    else if(!g_strcmp0(profile, "adobergb"))
    {
      profile_type = DT_COLORSPACE_ADOBERGB;
    }
    else
    {
      profile_type = DT_COLORSPACE_FILE;
      profile_filename = &profile[1]; // the old code had a '/' in the beginning
    }

    // in theory pprofile can't be srgb or adobergb, but checking for them won't hurt
    if(*pprofile == '\0')
    {
      pprofile_type = DT_COLORSPACE_NONE;
    }
    else if(!g_strcmp0(pprofile, "sRGB"))
    {
      pprofile_type = DT_COLORSPACE_SRGB;
    }
    else if(!g_strcmp0(pprofile, "adobergb"))
    {
      pprofile_type = DT_COLORSPACE_ADOBERGB;
    }
    else
    {
      pprofile_type = DT_COLORSPACE_FILE;
      pprofile_filename = &pprofile[1]; // the old code had a '/' in the beginning
    }

    const int32_t new_profile_len = strlen(profile_filename) + 1;
    const int32_t new_pprofile_len = strlen(pprofile_filename) + 1;

    // now we got everything to reassemble the new params
    size_t new_params_size = old_params_size - profile_len - pprofile_len;
    new_params_size += 2 * sizeof(dt_colorspaces_color_profile_type_t);
    new_params_size += new_profile_len + new_pprofile_len;
    void *new_params = malloc(new_params_size);

    size_t pos = 0;
    //   char *printer
    memcpy((uint8_t *)new_params + pos, printer, printer_len);
    pos += printer_len;
    //   char *paper
    memcpy((uint8_t *)new_params + pos, paper, paper_len);
    pos += paper_len;
    //   int32_t landscape
    memcpy((uint8_t *)new_params + pos, &landscape, sizeof(int32_t));
    pos += sizeof(int32_t);
    //   int32_t f_profile_type
    memcpy((uint8_t *)new_params + pos, &profile_type, sizeof(int32_t));
    pos += sizeof(int32_t);
    //   char *f_profile
    memcpy((uint8_t *)new_params + pos, profile_filename, new_profile_len);
    pos += new_profile_len;
    //   int32_t intent
    memcpy((uint8_t *)new_params + pos, &intent, sizeof(int32_t));
    pos += sizeof(int32_t);
    //   int32_t f_pprofile_type
    memcpy((uint8_t *)new_params + pos, &pprofile_type, sizeof(int32_t));
    pos += sizeof(int32_t);
    //   char *f_pprofile
    memcpy((uint8_t *)new_params + pos, pprofile_filename, new_pprofile_len);
    pos += new_pprofile_len;
    //   <rest>
    memcpy((uint8_t *)new_params + pos, buf, old_params_size - ((char *)buf - (char *)old_params));

    *new_size = new_params_size;
    *new_version = 2;
    return new_params;
  }
  else if(old_version == 2)
  {
    // add upscale to params
    size_t new_params_size = old_params_size + 1;
    void *new_params = calloc(1, new_params_size);

    memcpy(new_params, old_params, old_params_size);
    // no media type specified
    ((char *)new_params)[old_params_size] = '\0';

    *new_size = new_params_size;
    *new_version = 3;
    return new_params;
  }
  else if(old_version == 3)
  {
    // no box
    size_t new_params_size = old_params_size + sizeof(int32_t) + 4 * sizeof(float);
    void *new_params = calloc(1, new_params_size);

    memcpy(new_params, old_params, old_params_size);

    // single image box specified (there is no way to create a box on the size
    // of the page at this stage).
    int32_t idx = old_params_size;
    *(int32_t *)((uint8_t *)new_params + idx) = 1;
    idx += sizeof(int32_t);
    *(float *)((uint8_t *)new_params + idx) = 0.05f;
    idx += sizeof(float);
    *(float *)((uint8_t *)new_params + idx) = 0.05f;
    idx += sizeof(float);
    *(float *)((uint8_t *)new_params + idx) = 0.90f;
    idx += sizeof(float);
    *(float *)((uint8_t *)new_params + idx) = 0.90f;
    // idx += sizeof(float);

    *new_size = new_params_size;
    *new_version = 4;
    return new_params;
  }

  return NULL;
}

int set_params(dt_lib_module_t *self, const void *params, int size)
{
  dt_lib_print_settings_t *ps = (dt_lib_print_settings_t *)self->data;

  if(!params) return 1;

  // get the parameters buffer
  const char *buf = (char *)params;

  // get individual items
  const char *printer = buf;
  if(!printer) return 1;
  const int32_t printer_len = strlen(printer) + 1;
  buf += printer_len;

  const char *paper = buf;
  if(!paper) return 1;
  const int32_t paper_len = strlen(paper) + 1;
  buf += paper_len;

  const int32_t landscape = *(int32_t *)buf;
  buf +=  sizeof(int32_t);

  const int32_t f_profile_type = *(int32_t *)buf;
  buf +=  sizeof(int32_t);

  const char *f_profile = buf;
  if(!f_profile) return 1;
  const int32_t profile_len = strlen(f_profile) + 1;
  buf += profile_len;

  const int32_t intent = *(int32_t *)buf;
  buf += sizeof(int32_t);

  const int32_t f_pprofile_type = *(int32_t *)buf;
  buf +=  sizeof(int32_t);

  const char *f_pprofile = buf;
  if(!f_pprofile) return 1;
  const int32_t pprofile_len = strlen(f_pprofile) + 1;
  buf += pprofile_len;

  const int32_t pintent = *(int32_t *)buf;
  buf += sizeof(int32_t);

  const int32_t bpc = *(int32_t *)buf;
  buf += sizeof(int32_t);

  const char *style = buf;
  if(!style) return 1;
  const int32_t style_len = strlen(style) + 1;
  buf += style_len;

  const int32_t style_mode = *(int32_t *)buf;
  buf += sizeof(int32_t);

  const double b_top = *(double *)buf;
  buf += sizeof(double);

  const double b_bottom = *(double *)buf;
  buf += sizeof(double);

  const double b_left = *(double *)buf;
  buf += sizeof(double);

  const double b_right = *(double *)buf;
  buf += sizeof(double);

  const int32_t alignment = *(int32_t *)buf;
  buf += sizeof(int32_t);

  const char *media = buf;
  if(!media) return 1;
  const int32_t media_len = strlen(media) + 1;
  buf += media_len;

  ps->imgs.count = *(int32_t *)buf;
  buf += sizeof(int32_t);

  for(int k=0; k<ps->imgs.count; k++)
  {
    ps->imgs.box[k].pos.x = *(float *)buf;
    buf += sizeof(float);
    ps->imgs.box[k].pos.y = *(float *)buf;
    buf += sizeof(float);
    ps->imgs.box[k].pos.width = *(float *)buf;
    buf += sizeof(float);
    ps->imgs.box[k].pos.height = *(float *)buf;
    buf += sizeof(float);
  }

  // ensure that the size is correct
  if(size != printer_len + paper_len + media_len + profile_len + pprofile_len + style_len + 8 * sizeof(int32_t) + 4 * sizeof(double) + sizeof(int32_t) + (ps->imgs.count * 4 * sizeof(float)))
    return 1;

  // set the GUI with corresponding values
  if(printer[0] != '\0')
    dt_bauhaus_combobox_set_from_text(ps->printers, printer);

  if(paper[0] != '\0')
    dt_bauhaus_combobox_set_from_text(ps->papers, paper);

  if(media[0] != '\0')
    dt_bauhaus_combobox_set_from_text(ps->media, media);

  dt_bauhaus_combobox_set (ps->orientation, landscape);

  dt_bauhaus_combobox_set(ps->profile, 0);
  for(GList *iter = ps->profiles; iter; iter = g_list_next(iter))
  {
    dt_lib_export_profile_t *p = (dt_lib_export_profile_t *)iter->data;
    if(f_profile_type == p->type && (f_profile_type != DT_COLORSPACE_FILE || !g_strcmp0(f_profile, p->filename)))
    {
      dt_bauhaus_combobox_set(ps->profile, p->pos);
      break;
    }
  }

  dt_bauhaus_combobox_set (ps->intent, intent);

  dt_bauhaus_combobox_set(ps->pprofile, 0);
  for(GList *iter = ps->profiles; iter; iter = g_list_next(iter))
  {
    dt_lib_export_profile_t *p = (dt_lib_export_profile_t *)iter->data;
    if(f_pprofile_type == p->type && (f_pprofile_type != DT_COLORSPACE_FILE || !g_strcmp0(f_pprofile, p->filename)))
    {
      dt_bauhaus_combobox_set(ps->pprofile, p->ppos);
      break;
    }
  }

  dt_bauhaus_combobox_set (ps->pintent, pintent);
  ps->prt.printer.intent = pintent;

  if(style[0] != '\0')
    dt_bauhaus_combobox_set_from_text(ps->style, style);
  dt_bauhaus_combobox_set (ps->style_mode, style_mode);

  gtk_spin_button_set_value(GTK_SPIN_BUTTON(ps->b_top), b_top * units[ps->unit]);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(ps->b_bottom), b_bottom * units[ps->unit]);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(ps->b_left), b_left * units[ps->unit]);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(ps->b_right), b_right * units[ps->unit]);

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ps->dtba[alignment]), TRUE);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ps->black_point_compensation), bpc);

  dt_control_queue_redraw_center();

  return 0;
}

void *get_params(dt_lib_module_t *self, int *size)
{
  const dt_lib_print_settings_t *ps = (dt_lib_print_settings_t *)self->data;

  // get the data
  const char *printer = dt_bauhaus_combobox_get_text(ps->printers);
  const char *paper = dt_bauhaus_combobox_get_text(ps->papers);
  const char *media = dt_bauhaus_combobox_get_text(ps->media);
  const int32_t profile_pos = dt_bauhaus_combobox_get(ps->profile);
  const int32_t intent =  dt_bauhaus_combobox_get(ps->intent);
  const char *style = dt_bauhaus_combobox_get_text(ps->style);
  const int32_t style_mode = dt_bauhaus_combobox_get(ps->style_mode);
  const int32_t pprofile_pos = dt_bauhaus_combobox_get(ps->pprofile);
  const int32_t pintent =  dt_bauhaus_combobox_get(ps->pintent);
  const int32_t landscape = dt_bauhaus_combobox_get(ps->orientation);
  const int32_t bpc = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ps->black_point_compensation));
  const double b_top = ps->prt.page.margin_top;
  const double b_bottom = ps->prt.page.margin_bottom;
  const double b_left = ps->prt.page.margin_left;
  const double b_right = ps->prt.page.margin_right;
  const int32_t alignment = 0;

  dt_colorspaces_color_profile_type_t profile_type = DT_COLORSPACE_NONE, pprofile_type = DT_COLORSPACE_NONE;
  const char *profile = "", *pprofile = "";
  for(GList *iter = ps->profiles; iter; iter = g_list_next(iter))
  {
    dt_lib_export_profile_t *p = (dt_lib_export_profile_t *)iter->data;
    if(p->pos == profile_pos)
    {
      profile_type = p->type;
      profile = p->filename;
    }
    if(p->ppos == pprofile_pos)
    {
      pprofile_type = p->type;
      pprofile = p->filename;
    }
  }

  // these will be NULL when no printer is connected/found
  if(!printer) printer = "";
  if(!paper) paper = "";
  if(!media) media = "";

  // compute the size of individual items, always get the \0 for strings
  const int32_t printer_len = strlen (printer) + 1;
  const int32_t paper_len = strlen (paper) + 1;
  const int32_t media_len = strlen (media) + 1;
  const int32_t profile_len = strlen (profile) + 1;
  const int32_t pprofile_len = strlen (pprofile) + 1;
  const int32_t style_len = strlen (style) + 1;

  // compute the size of all parameters
  *size = printer_len + paper_len + media_len + profile_len + pprofile_len + style_len + 8 * sizeof(int32_t) + 4 * sizeof(double) + sizeof(int32_t) + (ps->imgs.count * 4 * sizeof(float));

  // allocate the parameter buffer
  char *params = (char *)malloc(*size);

  int pos = 0;

  memcpy(params+pos, printer, printer_len);
  pos += printer_len;
  memcpy(params+pos, paper, paper_len);
  pos += paper_len;
  memcpy(params+pos, &landscape, sizeof(int32_t));
  pos += sizeof(int32_t);
  memcpy(params+pos, &profile_type, sizeof(int32_t));
  pos += sizeof(int32_t);
  memcpy(params+pos, profile, profile_len);
  pos += profile_len;
  memcpy(params+pos, &intent, sizeof(int32_t));
  pos += sizeof(int32_t);
  memcpy(params+pos, &pprofile_type, sizeof(int32_t));
  pos += sizeof(int32_t);
  memcpy(params+pos, pprofile, pprofile_len);
  pos += pprofile_len;
  memcpy(params+pos, &pintent, sizeof(int32_t));
  pos += sizeof(int32_t);
  memcpy(params+pos, &bpc, sizeof(int32_t));
  pos += sizeof(int32_t);
  memcpy(params+pos, style, style_len);
  pos += style_len;
  memcpy(params+pos, &style_mode, sizeof(int32_t));
  pos += sizeof(int32_t);
  memcpy(params+pos, &b_top, sizeof(double));
  pos += sizeof(double);
  memcpy(params+pos, &b_bottom, sizeof(double));
  pos += sizeof(double);
  memcpy(params+pos, &b_left, sizeof(double));
  pos += sizeof(double);
  memcpy(params+pos, &b_right, sizeof(double));
  pos += sizeof(double);
  memcpy(params+pos, &alignment, sizeof(int32_t));
  pos += sizeof(int32_t);
  memcpy(params+pos, media, media_len);
  pos += media_len;

  // boxes
  memcpy(params+pos, &ps->imgs.count, sizeof(int32_t));
  pos += sizeof(int32_t);

  for(int k=0; k<ps->imgs.count; k++)
  {
    memcpy(params+pos, &ps->imgs.box[k].pos.x, sizeof(float));
    pos += sizeof(int32_t);
    memcpy(params+pos, &ps->imgs.box[k].pos.y, sizeof(float));
    pos += sizeof(int32_t);
    memcpy(params+pos, &ps->imgs.box[k].pos.width, sizeof(float));
    pos += sizeof(int32_t);
    memcpy(params+pos, &ps->imgs.box[k].pos.height, sizeof(float));
    pos += sizeof(int32_t);
  }

  g_assert(pos == *size);

  return params;
}

void gui_cleanup(dt_lib_module_t *self)
{
  dt_lib_print_settings_t *ps = (dt_lib_print_settings_t *)self->data;

  // these can be called on shutdown, resulting in null-pointer
  // dereference and division by zero -- not sure what interaction
  // makes them called, but better to disconnect and not have segfault
  g_signal_handlers_disconnect_by_func(G_OBJECT(ps->b_top), G_CALLBACK(_top_border_callback), self);
  g_signal_handlers_disconnect_by_func(G_OBJECT(ps->b_bottom), G_CALLBACK(_bottom_border_callback), self);
  g_signal_handlers_disconnect_by_func(G_OBJECT(ps->b_left), G_CALLBACK(_left_border_callback), self);
  g_signal_handlers_disconnect_by_func(G_OBJECT(ps->b_right), G_CALLBACK(_right_border_callback), self);

  g_list_free_full(ps->profiles, g_free);
  g_list_free_full(ps->paper_list, free);
  g_list_free_full(ps->media_list, free);

  g_free(ps->v_iccprofile);
  g_free(ps->v_piccprofile);
  g_free(ps->v_style);

  free(self->data);
  self->data = NULL;
}

void gui_reset(dt_lib_module_t *self)
{
  dt_lib_print_settings_t *ps = (dt_lib_print_settings_t *)self->data;

  gtk_spin_button_set_value(GTK_SPIN_BUTTON(ps->b_top), 17 * units[ps->unit]);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(ps->b_bottom), 17 * units[ps->unit]);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(ps->b_left), 17 * units[ps->unit]);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(ps->b_right), 17 * units[ps->unit]);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(ps->grid_size), 10 * units[ps->unit]);

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ps->dtba[ALIGNMENT_CENTER]), TRUE);
  ps->prt.printer.intent = DT_INTENT_PERCEPTUAL;
  dt_bauhaus_combobox_set(ps->profile, 0);
  dt_bauhaus_combobox_set(ps->pprofile, 0);
  dt_bauhaus_combobox_set(ps->pintent, 0);
  dt_bauhaus_combobox_set(ps->style, 0);
  dt_bauhaus_combobox_set(ps->intent, 0);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ps->black_point_compensation), TRUE);
  gtk_widget_set_sensitive(GTK_WIDGET(ps->pintent), TRUE);
  gtk_widget_set_sensitive(GTK_WIDGET(ps->black_point_compensation), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(ps->style_mode), FALSE);

  // reset page orientation to fit the picture if a single one is displayed

  const int32_t imgid = (ps->imgs.count > 0) ? ps->imgs.box[0].imgid : -1;
  dt_printing_clear_boxes(&ps->imgs);
  ps->imgs.imgid_to_load = imgid;

  ps->creation = ps->dragging = FALSE;
  ps->selected = -1;
  ps->last_selected = -1;
  ps->has_changed = FALSE;

  dt_control_queue_redraw_center();
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
