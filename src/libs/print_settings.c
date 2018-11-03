/*
    This file is part of darktable,
    copyright (c) 2014-2017 pascal obry.

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
#include "common/image_cache.h"
#include "common/metadata.h"
#include "common/pdf.h"
#include "common/printprof.h"
#include "common/styles.h"
#include "common/tags.h"
#include "common/variables.h"
#include "control/jobs.h"
#include "dtgtk/resetlabel.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include "libs/lib_api.h"

DT_MODULE(3)

static gboolean _bauhaus_combobox_set_active_text(GtkWidget *cb, const gchar *text);

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

typedef struct dt_lib_print_settings_t
{
  GtkWidget *profile, *intent, *style, *style_mode, *papers, *media;
  GtkWidget *printers, *orientation, *pprofile, *pintent;
  GtkWidget *width, *height, *black_point_compensation;
  GtkWidget *info;
  GList *profiles;
  GtkButton *print_button;
  GtkToggleButton *lock_button;
  GtkWidget *b_top, *b_bottom, *b_left, *b_right;
  GtkDarktableToggleButton *dtba[9];	                                   // Alignment buttons
  GList *paper_list, *media_list;
  gboolean lock_activated;
  dt_print_info_t prt;
  int32_t image_id;
  int32_t iwidth, iheight;
  int unit;
  int v_intent, v_pintent;
  int v_icctype, v_picctype;
  char *v_iccprofile, *v_piccprofile, *v_style;
  gboolean v_style_append, v_black_point_compensation;
} dt_lib_print_settings_t;

typedef struct dt_lib_print_job_t
{
  int imgid;
  gchar *job_title;
  dt_print_info_t prt;
  gchar* style;
  gboolean style_append, black_point_compensation;
  dt_colorspaces_color_profile_type_t buf_icc_type, p_icc_type;
  gchar *buf_icc_profile, *p_icc_profile;
  dt_iop_color_intent_t buf_icc_intent, p_icc_intent;
  uint16_t *buf;
  dt_pdf_page_t *pdf_page;
  dt_pdf_image_t *pdf_image;
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

static double units[3] = {1.0, 0.1, 1.0/25.4};

static void _update_slider (dt_lib_print_settings_t *ps);

static const int min_borders = -100; // this is in mm

int
position ()
{
  return 990;
}

// callbacks for in-memory export

typedef struct dt_print_format_t
{
  int max_width, max_height;
  int width, height;
  char style[128];
  gboolean style_append;
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
                       void *exif, int exif_len, int imgid, int num, int total)
{
  dt_print_format_t *d = (dt_print_format_t *)data;

  d->params->buf = (uint16_t *)malloc(d->width * d->height * 3 * (d->bpp == 8?1:2));

  if (d->bpp == 8)
  {
    const uint8_t *in_ptr = (const uint8_t *)in;
    uint8_t *out_ptr = (uint8_t *)d->params->buf;
    for(int y = 0; y < d->height; y++)
    {
      for(int x = 0; x < d->width; x++, in_ptr += 4, out_ptr += 3)
        memcpy(out_ptr, in_ptr, 3);
    }
  }
  else
  {
    const uint16_t *in_ptr = (const uint16_t *)in;
    uint16_t *out_ptr = (uint16_t *)d->params->buf;
    for(int y = 0; y < d->height; y++)
    {
      for(int x = 0; x < d->width; x++, in_ptr += 4, out_ptr += 3)
        memcpy(out_ptr, in_ptr, 6);
    }
  }

  return 0;
}

static int _print_job_run(dt_job_t *job)
{
  dt_lib_print_job_t *params = dt_control_job_get_params(job);

  // user margin are already in the proper orientation landscape/portrait
  double width, height;
  double margin_w = params->prt.page.margin_left + params->prt.page.margin_right;
  double margin_h = params->prt.page.margin_top + params->prt.page.margin_bottom;

  if (params->prt.page.landscape)
  {
    width = params->prt.paper.height;
    height = params->prt.paper.width;
    margin_w += params->prt.printer.hw_margin_top + params->prt.printer.hw_margin_bottom;
    margin_h += params->prt.printer.hw_margin_left + params->prt.printer.hw_margin_right;
  }
  else
  {
    width = params->prt.paper.width;
    height = params->prt.paper.height;
    margin_w += params->prt.printer.hw_margin_left + params->prt.printer.hw_margin_right;
    margin_h += params->prt.printer.hw_margin_top + params->prt.printer.hw_margin_bottom;
  }

  const int32_t width_pix = (width * params->prt.printer.resolution) / 25.4;
  const int32_t height_pix = (height * params->prt.printer.resolution) / 25.4;

  const double pa_width  = (width  - margin_w) / 25.4;
  const double pa_height = (height - margin_h) / 25.4;

  dt_print(DT_DEBUG_PRINT, "[print] printable area for image %u : %3.2fin x %3.2fin\n", params->imgid, pa_width, pa_height);

  // compute the needed size for picture for the given printer resolution

  const int max_width  = (pa_width  * params->prt.printer.resolution);
  const int max_height = (pa_height * params->prt.printer.resolution);

  dt_print(DT_DEBUG_PRINT, "[print] max image size %d x %d (at resolution %d)\n", max_width, max_height, params->prt.printer.resolution);

  dt_imageio_module_format_t buf;
  buf.mime = mime;
  buf.levels = levels;
  buf.bpp = bpp;
  buf.write_image = write_image;

  dt_print_format_t dat;
  dat.max_width = max_width;
  dat.max_height = max_height;
  dat.style[0] = '\0';
  dat.style_append = params->style_append;
  dat.bpp = *params->p_icc_profile ? 16 : 8; // set to 16bit when a profile is to be applied
  dat.params = params;

  if (params->style) g_strlcpy(dat.style, params->style, sizeof(dat.style));

  // let the user know something is happening
  dt_control_job_set_progress(job, 0.05);
  dt_control_log(_("processing `%s' for `%s'"), params->job_title, params->prt.printer.name);

  const int high_quality = 1;
  const int upscale = 1;
  const dt_colorspaces_color_profile_t *buf_profile = dt_colorspaces_get_output_profile(params->imgid, params->buf_icc_type, params->buf_icc_profile);

  dt_imageio_export_with_flags(params->imgid, "unused", &buf, (dt_imageio_module_data_t *)&dat, 1, 0, high_quality, upscale, 0,
                               NULL, FALSE, params->buf_icc_type, params->buf_icc_profile, params->buf_icc_intent,  NULL, NULL, 1, 1);

  // after exporting we know the real size of the image, compute the layout

  // compute print-area (in inches)
  int32_t px=0, py=0, pwidth=0, pheight=0;
  int32_t ax=0, ay=0, awidth=0, aheight=0;
  int32_t ix=0, iy=0, iwidth=0, iheight=0;
  int32_t iwpix=dat.width, ihpix=dat.height;

  dt_get_print_layout (params->imgid, &params->prt, width_pix, height_pix,
                       &iwpix, &ihpix,
                       &px, &py, &pwidth, &pheight,
                       &ax, &ay, &awidth, &aheight,
                       &ix, &iy, &iwidth, &iheight);

  const int margin_top    = iy;
  const int margin_left   = ix;
  const int margin_right  = pwidth - iwidth - ix;
  const int margin_bottom = pheight - iheight - iy;

  dt_print(DT_DEBUG_PRINT, "[print] margins top %d ; bottom %d ; left %d ; right %d\n",
           margin_top, margin_bottom, margin_left, margin_right);

  // we have the exported buffer, let's apply the printer profile

  if (*params->p_icc_profile)
  {
    const dt_colorspaces_color_profile_t *pprof = dt_colorspaces_get_profile(params->p_icc_type, params->p_icc_profile,
                                                                             DT_PROFILE_DIRECTION_OUT);
    if (!pprof)
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
        dt_control_log(_("error getting output profile for image %d"), params->imgid);
        fprintf(stderr, "error getting output profile for image %d\n", params->imgid);
        dt_control_queue_redraw();
        return 1;
      }
      if (dt_apply_printer_profile((void **)&(params->buf), dat.width, dat.height, dat.bpp, buf_profile->profile,
                                   pprof->profile, params->p_icc_intent, params->black_point_compensation))
      {
        dt_control_log(_("cannot apply printer profile `%s'"), params->p_icc_profile);
        fprintf(stderr, "cannot apply printer profile `%s'\n", params->p_icc_profile);
        dt_control_queue_redraw();
        return 1;
      }
    }
  }

  const float page_width  = dt_pdf_mm_to_point(width);
  const float page_height = dt_pdf_mm_to_point(height);

  if(dt_control_job_get_state(job) == DT_JOB_STATE_CANCELLED) return 0;
  dt_control_job_set_progress(job, 0.9);

  dt_loc_get_tmp_dir(params->pdf_filename, sizeof(params->pdf_filename));
  g_strlcat(params->pdf_filename, "/pf.XXXXXX.pdf", sizeof(params->pdf_filename));

  gint fd = g_mkstemp(params->pdf_filename);
  if(fd == -1)
  {
    dt_control_log(_("failed to create temporary pdf for printing"));
    fprintf(stderr, "failed to create temporary pdf for printing\n");
    return 1;
  }
  close(fd);

  const int icc_id = 0;

  dt_pdf_t *pdf = dt_pdf_start(params->pdf_filename, page_width, page_height, params->prt.printer.resolution, DT_PDF_STREAM_ENCODER_FLATE);

/*
  // ??? should a profile be embedded here?
  if (*printer_profile)
    icc_id = dt_pdf_add_icc(pdf, printer_profile);
*/
  params->pdf_image = dt_pdf_add_image(pdf, (uint8_t *)params->buf, dat.width, dat.height, 8, icc_id, 0.0);

  //  PDF bounding-box has origin on bottom-left
  params->pdf_image->bb_x      = dt_pdf_pixel_to_point((float)margin_left, params->prt.printer.resolution);
  params->pdf_image->bb_y      = dt_pdf_pixel_to_point((float)margin_bottom, params->prt.printer.resolution);
  params->pdf_image->bb_width  = dt_pdf_pixel_to_point((float)iwidth, params->prt.printer.resolution);
  params->pdf_image->bb_height = dt_pdf_pixel_to_point((float)iheight, params->prt.printer.resolution);

  if (params->prt.page.landscape && (dat.width > dat.height))
    params->pdf_image->rotate_to_fit = TRUE;
  else
    params->pdf_image->rotate_to_fit = FALSE;

  params->pdf_page = dt_pdf_add_page(pdf, &params->pdf_image, 1);
  dt_pdf_finish(pdf, &params->pdf_page, 1);

  if(dt_control_job_get_state(job) == DT_JOB_STATE_CANCELLED) return 0;
  dt_control_job_set_progress(job, 0.95);

  // send to CUPS

  dt_print_file (params->imgid, params->pdf_filename, params->job_title, &params->prt);
  dt_control_job_set_progress(job, 1.0);

  // add tag for this image

  char tag[256] = { 0 };
  guint tagid = 0;
  snprintf (tag, sizeof(tag), "darktable|printed|%s", params->prt.printer.name);
  dt_tag_new(tag, &tagid);
  dt_tag_attach(tagid, params->imgid);

  return 0;
}

static void _print_job_cleanup(void *p)
{
  dt_lib_print_job_t *params = p;
  if(params->pdf_filename[0]) g_unlink(params->pdf_filename);
  free(params->pdf_image);
  free(params->pdf_page);
  free(params->buf);
  g_free(params->style);
  g_free(params->buf_icc_profile);
  g_free(params->p_icc_profile);
  g_free(params->job_title);
  free(params);
}

static void
_print_button_clicked (GtkWidget *widget, gpointer user_data)
{
  const dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_print_settings_t *ps = (dt_lib_print_settings_t *)self->data;

  const int imgid = dt_view_filmstrip_get_activated_imgid(darktable.view_manager);

  if (imgid == -1)
  {
    dt_control_log(_("cannot print until a picture is selected"));
    return;
  }
  if (strlen(ps->prt.printer.name) == 0 || ps->prt.printer.resolution == 0)
  {
    dt_control_log(_("cannot print until a printer is selected"));
    return;
  }
  if (ps->prt.paper.width == 0 || ps->prt.paper.height == 0)
  {
    dt_control_log(_("cannot print until a paper is selected"));
    return;
  }

  dt_job_t *job = dt_control_job_create(&_print_job_run, "print image %d", imgid);
  if(!job) return;

  dt_lib_print_job_t *params = calloc(1, sizeof(dt_lib_print_job_t));
  dt_control_job_set_params(job, params, _print_job_cleanup);

  params->imgid = imgid;
  memcpy(&params->prt, &ps->prt, sizeof(dt_print_info_t));

  // what to call the image?
  GList *res;
  if((res = dt_metadata_get(params->imgid, "Xmp.dc.title", NULL)) != NULL)
  {
    // FIXME: in metadata_view.c, non-printables are filtered, should we do this here?
    params->job_title = g_strdup((gchar *)res->data);
    g_list_free_full(res, &g_free);
  }
  else
  {
    const dt_image_t *img = dt_image_cache_get(darktable.image_cache, params->imgid, 'r');
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

  dt_get_printer_info (printer_name, &ps->prt.printer);

  // if this is a turboprint printer, disable the printer profile

  if (ps->prt.printer.is_turboprint)
    dt_bauhaus_combobox_set(ps->pprofile, 0);

  // if there is 0 hardware margins, set the user marging to 15mm

  if (ps->prt.printer.hw_margin_top == 0)
  {
    ps->prt.page.margin_top = 15;
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(ps->b_top), ps->prt.page.margin_top * units[ps->unit]);
  }
  if (ps->prt.printer.hw_margin_bottom == 0)
  {
    ps->prt.page.margin_bottom = 15;
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(ps->b_bottom), ps->prt.page.margin_bottom * units[ps->unit]);
  }
  if (ps->prt.printer.hw_margin_left == 0)
  {
    ps->prt.page.margin_left = 15;
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(ps->b_left), ps->prt.page.margin_left * units[ps->unit]);
  }
  if (ps->prt.printer.hw_margin_right == 0)
  {
    ps->prt.page.margin_right = 15;
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(ps->b_right), ps->prt.page.margin_right * units[ps->unit]);
  }

  dt_conf_set_string("plugins/print/print/printer", printer_name);

  char *default_paper = dt_conf_get_string("plugins/print/print/paper");

  // next add corresponding papers

  // first clear current list

  dt_bauhaus_combobox_clear(ps->papers);

  // then add papers for the given printer

  if(ps->paper_list) g_list_free_full(ps->paper_list, free);

  ps->paper_list = dt_get_papers (&ps->prt.printer);
  GList *papers = ps->paper_list;
  int np = 0;
  gboolean ispaperset = FALSE;

  while (papers)
  {
    const dt_paper_info_t *p = (dt_paper_info_t *)papers->data;
    dt_bauhaus_combobox_add(ps->papers, p->common_name);

    if (ispaperset == FALSE && (!g_strcmp0(default_paper, p->common_name) || default_paper[0] == '\0'))
    {
      dt_bauhaus_combobox_set(ps->papers, np);
      ispaperset = TRUE;
    }

    np++;
    papers = g_list_next (papers);
  }

  //  paper not found in this printer
  if (!ispaperset) dt_bauhaus_combobox_set(ps->papers, 0);

  const dt_paper_info_t *paper = dt_get_paper(ps->paper_list, default_paper);

  if (paper)
    memcpy(&ps->prt.paper, paper, sizeof(dt_paper_info_t));

  g_free (default_paper);

  // next add corresponding supported media

  char *default_medium = dt_conf_get_string("plugins/print/print/medium");

  // first clear current list

  dt_bauhaus_combobox_clear(ps->media);

  // then add papers for the given printer

  if(ps->media_list) g_list_free_full(ps->media_list, free);

  ps->media_list = dt_get_media_type (&ps->prt.printer);
  GList *media = ps->media_list;
  gboolean ismediaset = FALSE;

  np = 0;

  while (media)
  {
    const dt_medium_info_t *m = (dt_medium_info_t *)media->data;
    dt_bauhaus_combobox_add(ps->media, m->common_name);

    if (ismediaset == FALSE && (!g_strcmp0(default_medium, m->common_name) || default_medium[0] == '\0'))
    {
      dt_bauhaus_combobox_set(ps->media, np);
      ismediaset = TRUE;
    }

    np++;
    media = g_list_next (media);
  }

  //  media not found in this printer
  if (!ismediaset) dt_bauhaus_combobox_set(ps->media, 0);

  const dt_medium_info_t *medium = dt_get_medium(ps->media_list, default_medium);

  if (medium)
    memcpy(&ps->prt.medium, medium, sizeof(dt_medium_info_t));

  g_free (default_medium);

  dt_view_print_settings(darktable.view_manager, &ps->prt);
}

static void
_printer_changed (GtkWidget *combo, const dt_lib_module_t *self)
{
  const gchar *printer_name = dt_bauhaus_combobox_get_text(combo);

  if (printer_name)
    _set_printer (self, printer_name);
}

static void
_paper_changed (GtkWidget *combo, const dt_lib_module_t *self)
{
  dt_lib_print_settings_t *ps = (dt_lib_print_settings_t *)self->data;

  const gchar *paper_name = dt_bauhaus_combobox_get_text(combo);

  if (!paper_name) return;

  const dt_paper_info_t *paper = dt_get_paper(ps->paper_list, paper_name);

  if (paper)
    memcpy(&ps->prt.paper, paper, sizeof(dt_paper_info_t));

  ps->iwidth = ps->iheight = 0;

  dt_conf_set_string("plugins/print/print/paper", paper_name);
  dt_view_print_settings(darktable.view_manager, &ps->prt);

  _update_slider(ps);
}

static void
_media_changed (GtkWidget *combo, const dt_lib_module_t *self)
{
  dt_lib_print_settings_t *ps = (dt_lib_print_settings_t *)self->data;

  const gchar *medium_name = dt_bauhaus_combobox_get_text(combo);

  if (!medium_name) return;

  const dt_medium_info_t *medium = dt_get_medium(ps->media_list, medium_name);

  if (medium)
    memcpy(&ps->prt.medium, medium, sizeof(dt_medium_info_t));

  dt_conf_set_string("plugins/print/print/medium", medium_name);
  dt_view_print_settings(darktable.view_manager, &ps->prt);

  _update_slider(ps);
}

static double to_mm(dt_lib_print_settings_t *ps, double value)
{
  return value / units[ps->unit];
}

static void
_update_slider (dt_lib_print_settings_t *ps)
{
  dt_view_print_settings(darktable.view_manager, &ps->prt);

  // if widget are created, let's display the current image size

  if (ps->image_id != -1 && ps->width && ps->height && ps->info)
  {
    int32_t px=0, py=0, pwidth=0, pheight=0;
    int32_t ax=0, ay=0, awidth=0, aheight=0;
    int32_t ix=0, iy=0, iwidth=0, iheight=0;
    int32_t iwpix=ps->iwidth, ihpix=ps->iheight;
    int32_t pa_width, pa_height;

    if (ps->prt.page.landscape)
    {
      pa_height = (int32_t)ps->prt.paper.width;
      pa_width = (int32_t)ps->prt.paper.height;
    }
    else
    {
      pa_width = (int32_t)ps->prt.paper.width;
      pa_height = (int32_t)ps->prt.paper.height;
    }

    dt_get_print_layout(ps->image_id, &ps->prt, pa_width, pa_height,
                        &iwpix, &ihpix,
                        &px, &py, &pwidth, &pheight,
                        &ax, &ay, &awidth, &aheight,
                        &ix, &iy, &iwidth, &iheight);

    if (ps->iwidth==0 || ps->iheight==0)
    {
      ps->iwidth = iwpix;
      ps->iheight = ihpix;
    }

    const double h = iheight * units[ps->unit];
    const double w = iwidth * units[ps->unit];
    char *value;

    value = g_strdup_printf("%3.2f", w);
    gtk_label_set_text(GTK_LABEL(ps->width), value);
    g_free(value);

    value = g_strdup_printf("%3.2f", h);
    gtk_label_set_text(GTK_LABEL(ps->height), value);
    g_free(value);

    // compute the image down/up scale and report information
    double scale;

    if (iwidth >= awidth)
      scale = dt_pdf_point_to_pixel(dt_pdf_mm_to_point((double)awidth), ps->prt.printer.resolution) / ps->iwidth;
    else
      scale = dt_pdf_point_to_pixel(dt_pdf_mm_to_point((double)aheight), ps->prt.printer.resolution) / ps->iheight;

    value = g_strdup_printf(_("%3.2f (dpi:%d)"), scale, scale<=1.0 ? (int)ps->prt.printer.resolution : (int)(ps->prt.printer.resolution / scale));
    gtk_label_set_text(GTK_LABEL(ps->info), value);
    g_free(value);
  }

  // set the max range for the borders depending on the others border and never allow to have an image size of 0 or less
  const int min_size = 5; // minimum size in mm
  const int pa_max_height = ps->prt.paper.height - ps->prt.printer.hw_margin_top - ps->prt.printer.hw_margin_bottom - min_size;
  const int pa_max_width  = ps->prt.paper.width  - ps->prt.printer.hw_margin_left - ps->prt.printer.hw_margin_right - min_size;

  gtk_spin_button_set_range (GTK_SPIN_BUTTON(ps->b_top),
                             min_borders * units[ps->unit], (pa_max_height - ps->prt.page.margin_bottom) * units[ps->unit]);
  gtk_spin_button_set_range (GTK_SPIN_BUTTON(ps->b_left),
                             min_borders * units[ps->unit], (pa_max_width - ps->prt.page.margin_right) * units[ps->unit]);
  gtk_spin_button_set_range (GTK_SPIN_BUTTON(ps->b_right),
                             min_borders * units[ps->unit], (pa_max_width - ps->prt.page.margin_left) * units[ps->unit]);
  gtk_spin_button_set_range (GTK_SPIN_BUTTON(ps->b_bottom),
                             min_borders * units[ps->unit], (pa_max_height - ps->prt.page.margin_top) * units[ps->unit]);
}

static void
_top_border_callback (GtkWidget *spin, gpointer user_data)
{
  const dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_print_settings_t *ps = (dt_lib_print_settings_t *)self->data;
  const double value = gtk_spin_button_get_value(GTK_SPIN_BUTTON(spin));

  ps->prt.page.margin_top = to_mm(ps, value);

  if (ps->lock_activated == TRUE)
  {
    ps->prt.page.margin_bottom = to_mm(ps, value);
    ps->prt.page.margin_left = to_mm(ps, value);
    ps->prt.page.margin_right = to_mm(ps, value);

    gtk_spin_button_set_value(GTK_SPIN_BUTTON(ps->b_bottom), value);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(ps->b_left), value);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(ps->b_right), value);
  }

  _update_slider (ps);
}

static void
_bottom_border_callback (GtkWidget *spin, gpointer user_data)
{
  const dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_print_settings_t *ps = (dt_lib_print_settings_t *)self->data;
  const double value = gtk_spin_button_get_value(GTK_SPIN_BUTTON(spin));

  ps->prt.page.margin_bottom = to_mm(ps, value);
  _update_slider (ps);
}

static void
_left_border_callback (GtkWidget *spin, gpointer user_data)
{
  const dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_print_settings_t *ps = (dt_lib_print_settings_t *)self->data;
  const double value = gtk_spin_button_get_value(GTK_SPIN_BUTTON(spin));

  ps->prt.page.margin_left = to_mm(ps, value);
  _update_slider (ps);
}

static void
_right_border_callback (GtkWidget *spin, gpointer user_data)
{
  const dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_print_settings_t *ps = (dt_lib_print_settings_t *)self->data;
  const double value = gtk_spin_button_get_value(GTK_SPIN_BUTTON(spin));

  ps->prt.page.margin_right = to_mm(ps, value);
  _update_slider (ps);
}

static void
_lock_callback (GtkWidget *button, gpointer user_data)
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
  int index=-1;
  const dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_print_settings_t *ps = (dt_lib_print_settings_t *)self->data;

  for(int i=0; i<9; i++)
  {
    /* block signal handler */
    g_signal_handlers_block_by_func (ps->dtba[i],_alignment_callback,user_data);

    if( GTK_WIDGET(ps->dtba[i]) == tb )
    {
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ps->dtba[i]),TRUE);
      index=i;
    }
    else gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ps->dtba[i]),FALSE);

    /* unblock signal handler */
    g_signal_handlers_unblock_by_func (ps->dtba[i],_alignment_callback,user_data);
  }
  ps->prt.page.alignment = index;
  _update_slider (ps);
}

static void
_orientation_changed (GtkWidget *combo, dt_lib_module_t *self)
{
  dt_lib_print_settings_t *ps = (dt_lib_print_settings_t *)self->data;

  ps->prt.page.landscape = dt_bauhaus_combobox_get (combo);

  _update_slider (ps);
}

static void
_unit_changed (GtkWidget *combo, dt_lib_module_t *self)
{
  dt_lib_print_settings_t *ps = (dt_lib_print_settings_t *)self->data;

  int unit = dt_bauhaus_combobox_get(combo);
  if(unit < 0) return; // shouldn't happen, but it could be -1 if nothing is selected
  ps->unit = unit;
  dt_conf_set_int("plugins/print/print/unit", ps->unit);

  const double margin_top = ps->prt.page.margin_top;
  const double margin_left = ps->prt.page.margin_left;
  const double margin_right = ps->prt.page.margin_right;
  const double margin_bottom = ps->prt.page.margin_bottom;

  const int n_digits = (int)(1.0 / (units[ps->unit] * 10.0));

  gtk_spin_button_set_digits(GTK_SPIN_BUTTON(ps->b_top),    n_digits);
  gtk_spin_button_set_digits(GTK_SPIN_BUTTON(ps->b_bottom), n_digits);
  gtk_spin_button_set_digits(GTK_SPIN_BUTTON(ps->b_left),   n_digits);
  gtk_spin_button_set_digits(GTK_SPIN_BUTTON(ps->b_right),  n_digits);

  const float incr = units[ps->unit];

  gtk_spin_button_set_increments(GTK_SPIN_BUTTON(ps->b_top), incr, incr);
  gtk_spin_button_set_increments(GTK_SPIN_BUTTON(ps->b_bottom), incr, incr);
  gtk_spin_button_set_increments(GTK_SPIN_BUTTON(ps->b_left), incr, incr);
  gtk_spin_button_set_increments(GTK_SPIN_BUTTON(ps->b_right), incr, incr);

  _update_slider (ps);

  // convert margins to new unit
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(ps->b_top),    margin_top * units[ps->unit]);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(ps->b_bottom), margin_bottom * units[ps->unit]);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(ps->b_left),   margin_left * units[ps->unit]);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(ps->b_right),  margin_right * units[ps->unit]);

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
  GList *prof = ps->profiles;
  while(prof)
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
    prof = g_list_next(prof);
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
  GList *prof = ps->profiles;
  while(prof)
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
    prof = g_list_next(prof);
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
_printer_bpc_callback (GtkWidget *widget, dt_lib_module_t *self)
{
  dt_lib_print_settings_t *ps = (dt_lib_print_settings_t *)self->data;
  ps->v_black_point_compensation = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ps->black_point_compensation));
  dt_conf_set_bool("plugins/print/print/black_point_compensation", ps->v_black_point_compensation);
}

static void
_intent_callback (GtkWidget *widget, dt_lib_module_t *self)
{
  dt_lib_print_settings_t *ps = (dt_lib_print_settings_t *)self->data;
  const int pos = dt_bauhaus_combobox_get(widget);
  // record the intent that will override the out rendering module on export
  dt_conf_set_int("plugins/print/print/iccintent", pos - 1);
  ps->v_intent = pos - 1;
}

static void _set_orientation(dt_lib_print_settings_t *ps)
{
  if (ps->image_id <= 0)
    return;

  dt_mipmap_buffer_t buf;
  dt_mipmap_cache_get(darktable.mipmap_cache, &buf, ps->image_id, DT_MIPMAP_0, DT_MIPMAP_BEST_EFFORT, 'r');

  // If there's a mipmap available, figure out orientation based upon
  // its dimensions. Otherwise, don't touch orientation until the
  // mipmap arrives.
  if (buf.size != DT_MIPMAP_NONE)
  {
    ps->prt.page.landscape = (buf.width > buf.height);
    dt_view_print_settings(darktable.view_manager, &ps->prt);
    dt_bauhaus_combobox_set (ps->orientation, ps->prt.page.landscape==TRUE?1:0);
  }

  dt_mipmap_cache_release(darktable.mipmap_cache, &buf);
}

static void _print_settings_activate_or_update_callback(gpointer instance,gpointer user_data)
{
  const dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_print_settings_t *ps = (dt_lib_print_settings_t *)self->data;

  ps->image_id = dt_view_filmstrip_get_activated_imgid(darktable.view_manager);
  ps->iwidth = ps->iheight = 0;
  _set_orientation (ps);
}

static GList* _get_profiles ()
{
  //  Create list of profiles
  GList *list = NULL;

  dt_lib_export_profile_t *prof = (dt_lib_export_profile_t *)g_malloc0(sizeof(dt_lib_export_profile_t));
  prof->type = DT_COLORSPACE_SRGB;
  dt_utf8_strlcpy(prof->name, _("sRGB (web-safe)"), sizeof(prof->name));
  prof->pos = -2;
  prof->ppos = -2;
  list = g_list_append(list, prof);

  prof = (dt_lib_export_profile_t *)g_malloc0(sizeof(dt_lib_export_profile_t));
  prof->type = DT_COLORSPACE_ADOBERGB;
  dt_utf8_strlcpy(prof->name, _("Adobe RGB (compatible)"), sizeof(prof->name));
  prof->pos = -2;
  prof->ppos = -2;
  list = g_list_append(list, prof);

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
      list = g_list_append(list, prof);
    }
  }

  return list;
}

static void _new_printer_callback(dt_printer_info_t *printer, void *user_data)
{
  static int count = 0;
  const dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  const dt_lib_print_settings_t *d = (dt_lib_print_settings_t*)self->data;

  char *default_printer = dt_conf_get_string("plugins/print/print/printer");

  g_signal_handlers_block_by_func(G_OBJECT(d->printers), G_CALLBACK(_printer_changed), NULL);

  dt_bauhaus_combobox_add(d->printers, printer->name);

  if (!g_strcmp0(default_printer, printer->name) || default_printer[0]=='\0')
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
  dt_control_signal_connect(darktable.signals,
                            DT_SIGNAL_VIEWMANAGER_FILMSTRIP_ACTIVATE,
                            G_CALLBACK(_print_settings_activate_or_update_callback),
                            self);

  // when an updated mipmap, we may have new orientation information
  // about the current image. This updates the image_id as well and
  // zeros out dimensions, but there should be no harm in that
  dt_control_signal_connect(darktable.signals,
                            DT_SIGNAL_DEVELOP_MIPMAP_UPDATED,
                            G_CALLBACK(_print_settings_activate_or_update_callback),
                            self);

  // NOTE: it would be proper to set image_id here to -1, but this seems to make no difference
}

void view_leave(struct dt_lib_module_t *self,struct dt_view_t *old_view,struct dt_view_t *new_view)
{
  dt_control_signal_disconnect(darktable.signals,
                               G_CALLBACK(_print_settings_activate_or_update_callback),
                               self);
}

void
gui_init (dt_lib_module_t *self)
{
  dt_lib_print_settings_t *d = (dt_lib_print_settings_t*)malloc(sizeof(dt_lib_print_settings_t));
  self->data = d;
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
  dt_gui_add_help_link(self->widget, "print_chapter.html#print_overview");

  char datadir[PATH_MAX] = { 0 };
  char confdir[PATH_MAX] = { 0 };
  dt_loc_get_user_config_dir(confdir, sizeof(confdir));
  dt_loc_get_datadir(datadir, sizeof(datadir));
  char *system_profile_dir = g_build_filename(datadir, "color", "out", NULL);
  char *user_profile_dir = g_build_filename(confdir, "color", "out", NULL);

  GtkWidget *label;

  d->paper_list = NULL;
  d->media_list = NULL;
  d->iwidth = d->iheight = 0;
  d->unit = 0;
  d->width = d->height = NULL;
  d->v_piccprofile = NULL;
  d->v_iccprofile = NULL;
  d->v_style = NULL;

  dt_init_print_info(&d->prt);
  dt_view_print_settings(darktable.view_manager, &d->prt);

  d->profiles = _get_profiles();

  d->image_id = -1;

  //  create the spin-button now as values could be set when the printer has no hardware margin

  d->b_top    = gtk_spin_button_new_with_range(0, 1000, 1);
  d->b_left   = gtk_spin_button_new_with_range(0, 1000, 1);
  d->b_right  = gtk_spin_button_new_with_range(0, 1000, 1);
  d->b_bottom = gtk_spin_button_new_with_range(0, 1000, 1);

  gtk_entry_set_alignment (GTK_ENTRY(d->b_top), 1);
  gtk_entry_set_alignment (GTK_ENTRY(d->b_left), 1);
  gtk_entry_set_alignment (GTK_ENTRY(d->b_right), 1);
  gtk_entry_set_alignment (GTK_ENTRY(d->b_bottom), 1);

  dt_gui_key_accel_block_on_focus_connect(GTK_WIDGET(d->b_top));
  dt_gui_key_accel_block_on_focus_connect(GTK_WIDGET(d->b_left));
  dt_gui_key_accel_block_on_focus_connect(GTK_WIDGET(d->b_right));
  dt_gui_key_accel_block_on_focus_connect(GTK_WIDGET(d->b_bottom));

  ////////////////////////// PRINTER SETTINGS

  // create papers combo as filled when adding printers
  d->papers = dt_bauhaus_combobox_new(NULL);

  label = dt_ui_section_label_new(_("printer"));
  gtk_box_pack_start(GTK_BOX(self->widget), label, TRUE, TRUE, 0);
  dt_gui_add_help_link(self->widget, "print_usage.html#print_printer_section");
  d->printers = dt_bauhaus_combobox_new(NULL);

  gtk_box_pack_start(GTK_BOX(self->widget), d->printers, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(d->printers), "value-changed", G_CALLBACK(_printer_changed), self);

  //// media

  d->media = dt_bauhaus_combobox_new(NULL);

  dt_bauhaus_widget_set_label(d->media, NULL, _("media"));

  g_signal_connect(G_OBJECT(d->media), "value-changed", G_CALLBACK(_media_changed), self);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(d->media), TRUE, TRUE, 0);

  //  Add printer profile combo

  d->pprofile = dt_bauhaus_combobox_new(NULL);
  dt_bauhaus_widget_set_label(d->pprofile, NULL, _("profile"));

  int combo_idx, n;
  GList *l = d->profiles;

  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(d->pprofile), TRUE, TRUE, 0);
  int printer_profile_type = dt_conf_get_int("plugins/print/printer/icctype");
  gchar *printer_profile = dt_conf_get_string("plugins/print/printer/iccprofile");
  combo_idx = -1;
  n = 0;

  dt_bauhaus_combobox_add(d->pprofile, _("color management in printer driver"));
  while(l)
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
    l = g_list_next(l);
  }

  g_free (printer_profile);

  // profile not found, maybe a profile has been removed? revert to none
  if (combo_idx == -1)
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

  d->pintent = dt_bauhaus_combobox_new(NULL);
  dt_bauhaus_widget_set_label(d->pintent, NULL, _("intent"));
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
  dt_gui_add_help_link(self->widget, "print_page_section.html#print_page_section");

  //// papers

  dt_bauhaus_widget_set_label(d->papers, NULL, _("paper size"));

  g_signal_connect(G_OBJECT(d->papers), "value-changed", G_CALLBACK(_paper_changed), self);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(d->papers), TRUE, TRUE, 0);

  //// portrait / landscape

  d->orientation = dt_bauhaus_combobox_new(NULL);
  dt_bauhaus_widget_set_label(d->orientation, NULL, _("orientation"));
  dt_bauhaus_combobox_add(d->orientation, _("portrait"));
  dt_bauhaus_combobox_add(d->orientation, _("landscape"));

  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(d->orientation), TRUE, TRUE, 0);

  GtkWidget *ucomb = dt_bauhaus_combobox_new(NULL);
  dt_bauhaus_combobox_add(ucomb, _("mm"));
  dt_bauhaus_combobox_add(ucomb, _("cm"));
  dt_bauhaus_combobox_add(ucomb, _("inch"));
  gtk_box_pack_start(GTK_BOX(self->widget), ucomb, TRUE, TRUE, 0);

  g_signal_connect(G_OBJECT(d->orientation), "value-changed", G_CALLBACK(_orientation_changed), self);
  g_signal_connect(G_OBJECT(ucomb), "value-changed", G_CALLBACK(_unit_changed), self);

  d->unit = dt_conf_get_int("plugins/print/print/unit");
  dt_bauhaus_combobox_set(ucomb, d->unit);

  dt_bauhaus_combobox_set (d->orientation, d->prt.page.landscape?1:0);

  //// image dimensions, create them now as we need them

  GtkWidget *hboxdim = gtk_box_new(GTK_ORIENTATION_HORIZONTAL,0);
  label = gtk_label_new(_("image width/height"));
  gtk_box_pack_start(GTK_BOX(hboxdim),GTK_WIDGET(label),TRUE,TRUE,0);
  d->width = gtk_label_new(_("width"));
  gtk_box_pack_start(GTK_BOX(hboxdim),GTK_WIDGET(d->width),TRUE,TRUE,0);
  label = gtk_label_new(_(" x "));
  gtk_box_pack_start(GTK_BOX(hboxdim),GTK_WIDGET(label),TRUE,TRUE,0);
  d->height = gtk_label_new(_("height"));
  gtk_box_pack_start(GTK_BOX(hboxdim),GTK_WIDGET(d->height),TRUE,TRUE,0);

  //// image information (downscale/upscale)

  GtkWidget *hboxinfo = gtk_box_new(GTK_ORIENTATION_HORIZONTAL,0);
  label = gtk_label_new(_("scale factor"));
  gtk_box_pack_start(GTK_BOX(hboxinfo),GTK_WIDGET(label),TRUE,TRUE,0);
  d->info = gtk_label_new("1.0");
  gtk_box_pack_start(GTK_BOX(hboxinfo),GTK_WIDGET(d->info),TRUE,TRUE,0);
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

  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(bds), TRUE, TRUE, 0);

  g_signal_connect (G_OBJECT (d->b_top), "value-changed",
                    G_CALLBACK (_top_border_callback), self);
  g_signal_connect (G_OBJECT (d->b_bottom), "value-changed",
                    G_CALLBACK (_bottom_border_callback), self);
  g_signal_connect (G_OBJECT (d->b_left), "value-changed",
                    G_CALLBACK (_left_border_callback), self);
  g_signal_connect (G_OBJECT (d->b_right), "value-changed",
                    G_CALLBACK (_right_border_callback), self);
  g_signal_connect (G_OBJECT(d->lock_button), "toggled",
                    G_CALLBACK(_lock_callback), self);

  const gboolean lock_active = dt_conf_get_bool("plugins/print/print/lock_borders");
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->lock_button), lock_active);

  // pack image dimension hbox here

  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hboxdim), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hboxinfo), TRUE, TRUE, 0);

  //// alignments

  // Create the 3x3 gtk table toggle button table...
  GtkGrid *bat = GTK_GRID(gtk_grid_new());
  gtk_grid_set_row_spacing(bat, DT_PIXEL_APPLY_DPI(3));
  gtk_grid_set_column_spacing(bat, DT_PIXEL_APPLY_DPI(3));
  for(int i=0; i<9; i++)
  {
    d->dtba[i] = DTGTK_TOGGLEBUTTON (dtgtk_togglebutton_new (dtgtk_cairo_paint_alignment,CPF_STYLE_FLAT|(CPF_SPECIAL_FLAG<<i), NULL));
    gtk_grid_attach (GTK_GRID (bat), GTK_WIDGET (d->dtba[i]), (i%3), i/3, 1, 1);
    g_signal_connect (G_OBJECT (d->dtba[i]), "toggled",G_CALLBACK (_alignment_callback), self);
  }
  d->prt.page.alignment = ALIGNMENT_CENTER;
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->dtba[d->prt.page.alignment]),TRUE);

  GtkWidget *hbox22 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL,0);
  GtkWidget *label4 = gtk_label_new(_("alignment"));
  gtk_box_pack_start(GTK_BOX(hbox22),GTK_WIDGET(label4),TRUE,TRUE,0);
  gtk_box_pack_start(GTK_BOX(hbox22), GTK_WIDGET(bat), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hbox22), TRUE, TRUE, 0);

  ////////////////////////// PRINT SETTINGS

  label = dt_ui_section_label_new(_("print settings"));
  gtk_box_pack_start(GTK_BOX(self->widget), label, TRUE, TRUE, 0);
  dt_gui_add_help_link(self->widget, "print_settings.html#print_settings");

  //  Add export profile combo

  d->profile = dt_bauhaus_combobox_new(NULL);
  dt_bauhaus_widget_set_label(d->profile, NULL, _("profile"));

  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(d->profile), TRUE, TRUE, 0);
  dt_bauhaus_combobox_add(d->profile, _("image settings"));

  int icctype = dt_conf_get_int("plugins/print/print/icctype");
  gchar *iccprofile = dt_conf_get_string("plugins/print/print/iccprofile");
  combo_idx = -1;
  n = 0;

  l = d->profiles;
  while(l)
  {
    dt_lib_export_profile_t *prof = (dt_lib_export_profile_t *)l->data;
    dt_bauhaus_combobox_add(d->profile, prof->name);
    prof->pos = ++n;
    if(prof->type == icctype && (prof->type != DT_COLORSPACE_FILE || !g_strcmp0(prof->filename, iccprofile)))
    {
      g_free(d->v_iccprofile);
      d->v_icctype = icctype;
      d->v_iccprofile = g_strdup(iccprofile);
      combo_idx = n;
    }
    l = g_list_next(l);
  }

  if (combo_idx == -1)
  {
    dt_conf_set_int("plugins/print/print/icctype", DT_COLORSPACE_NONE);
    dt_conf_set_string("plugins/print/print/iccprofile", "");
    g_free(d->v_iccprofile);
    d->v_icctype = DT_COLORSPACE_NONE;
    d->v_iccprofile = g_strdup("");
    combo_idx = 0;
  }
  g_free (iccprofile);

  dt_bauhaus_combobox_set(d->profile, combo_idx);

  tooltip = g_strdup_printf(_("output ICC profiles in %s or %s"), user_profile_dir, system_profile_dir);
  gtk_widget_set_tooltip_text(d->profile, tooltip);
  g_free(tooltip);

  g_signal_connect(G_OBJECT(d->profile), "value-changed", G_CALLBACK(_profile_changed), (gpointer)self);

  //  Add export intent combo

  d->intent = dt_bauhaus_combobox_new(NULL);
  dt_bauhaus_widget_set_label(d->intent, NULL, _("intent"));

  dt_bauhaus_combobox_add(d->intent, _("image settings"));
  dt_bauhaus_combobox_add(d->intent, _("perceptual"));
  dt_bauhaus_combobox_add(d->intent, _("relative colorimetric"));
  dt_bauhaus_combobox_add(d->intent, C_("rendering intent", "saturation"));
  dt_bauhaus_combobox_add(d->intent, _("absolute colorimetric"));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(d->intent), TRUE, TRUE, 0);

  dt_bauhaus_combobox_set(d->intent, dt_conf_get_int("plugins/print/print/iccintent") + 1);

  g_signal_connect (G_OBJECT (d->intent), "value-changed", G_CALLBACK (_intent_callback), (gpointer)self);

  //  Add export style combo

  d->style = dt_bauhaus_combobox_new(NULL);
  dt_bauhaus_widget_set_label(d->style, NULL, _("style"));

  dt_bauhaus_combobox_add(d->style, _("none"));

  GList *styles = dt_styles_get_list("");
  gchar *current_style = dt_conf_get_string("plugins/print/print/style");
  combo_idx = -1; n=0;

  while (styles)
  {
    dt_style_t *style=(dt_style_t *)styles->data;
    dt_bauhaus_combobox_add(d->style, style->name);
    n++;
    if (g_strcmp0(style->name,current_style)==0)
    {
      g_free(d->v_style);
      d->v_style = g_strdup(current_style);
      combo_idx=n;
    }
    styles=g_list_next(styles);
  }
  g_free(current_style);
  g_list_free_full(styles, dt_style_free);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(d->style), TRUE, TRUE, 0);
  gtk_widget_set_tooltip_text(d->style, _("temporary style to use while printing"));

  // style not found, maybe a style has been removed? revert to none
  if (combo_idx == -1)
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

  d->style_mode = dt_bauhaus_combobox_new(NULL);
  dt_bauhaus_widget_set_label(d->style_mode, NULL, _("mode"));

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

  GtkButton *button = GTK_BUTTON(gtk_button_new_with_label(_("print")));
  d->print_button = button;
  gtk_widget_set_tooltip_text(GTK_WIDGET(button), _("print with current settings"));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(button), TRUE, TRUE, 0);
  dt_gui_add_help_link(GTK_WIDGET(button), "print_button.html#print_button");

  g_signal_connect (G_OBJECT (button), "clicked",
                    G_CALLBACK (_print_button_clicked),
                    (gpointer)self);

  g_free(system_profile_dir);
  g_free(user_profile_dir);

  // Let's start the printer discovery now

  dt_printers_discovery(_new_printer_callback, self);
}

static gboolean _bauhaus_combobox_set_active_text(GtkWidget *cb, const gchar *text)
{
  g_assert(text != NULL);
  g_assert(cb != NULL);
  const GList *labels = dt_bauhaus_combobox_get_labels(cb);
  const GList *iter = labels;
  int i = 0;
  while(iter)
  {
    if(!g_strcmp0((gchar*)iter->data, text))
    {
      dt_bauhaus_combobox_set(cb, i);
      return TRUE;
    }
    i++;
    iter = g_list_next(iter);
  }
  return FALSE;
}

void init_presets(dt_lib_module_t *self)
{
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

    int32_t new_profile_len = strlen(profile_filename) + 1;
    int32_t new_pprofile_len = strlen(pprofile_filename) + 1;

    // now we got everything to reassemble the new params
    size_t new_params_size = old_params_size - profile_len - pprofile_len;
    new_params_size += 2 * sizeof(dt_colorspaces_color_profile_type_t);
    new_params_size += new_profile_len + new_pprofile_len;
    void *new_params = malloc(new_params_size);

    size_t pos = 0;
    //   char *printer
    memcpy(new_params + pos, printer, printer_len);
    pos += printer_len;
    //   char *paper
    memcpy(new_params + pos, paper, paper_len);
    pos += paper_len;
    //   int32_t landscape
    memcpy(new_params + pos, &landscape, sizeof(int32_t));
    pos += sizeof(int32_t);
    //   int32_t f_profile_type
    memcpy(new_params + pos, &profile_type, sizeof(int32_t));
    pos += sizeof(int32_t);
    //   char *f_profile
    memcpy(new_params + pos, profile_filename, new_profile_len);
    pos += new_profile_len;
    //   int32_t intent
    memcpy(new_params + pos, &intent, sizeof(int32_t));
    pos += sizeof(int32_t);
    //   int32_t f_pprofile_type
    memcpy(new_params + pos, &pprofile_type, sizeof(int32_t));
    pos += sizeof(int32_t);
    //   char *f_pprofile
    memcpy(new_params + pos, pprofile_filename, new_pprofile_len);
    pos += new_pprofile_len;
    //   <rest>
    memcpy(new_params + pos, buf, old_params_size - ((char *)buf - (char *)old_params));

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
  if (!printer) return 1;
  const int32_t printer_len = strlen(printer) + 1;
  buf += printer_len;

  const char *paper = buf;
  if (!paper) return 1;
  const int32_t paper_len = strlen(paper) + 1;
  buf += paper_len;

  const int32_t landscape = *(int32_t *)buf;
  buf +=  sizeof(int32_t);

  const int32_t f_profile_type = *(int32_t *)buf;
  buf +=  sizeof(int32_t);

  const char *f_profile = buf;
  if (!f_profile) return 1;
  const int32_t profile_len = strlen(f_profile) + 1;
  buf += profile_len;

  const int32_t intent = *(int32_t *)buf;
  buf += sizeof(int32_t);

  const int32_t f_pprofile_type = *(int32_t *)buf;
  buf +=  sizeof(int32_t);

  const char *f_pprofile = buf;
  if (!f_pprofile) return 1;
  const int32_t pprofile_len = strlen(f_pprofile) + 1;
  buf += pprofile_len;

  const int32_t pintent = *(int32_t *)buf;
  buf += sizeof(int32_t);

  const int32_t bpc = *(int32_t *)buf;
  buf += sizeof(int32_t);

  const char *style = buf;
  if (!style) return 1;
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
  if (!media) return 1;
  const int32_t media_len = strlen(media) + 1;
  // buf += media_len;

  // ensure that the size is correct
  if(size != printer_len + paper_len + media_len + profile_len + pprofile_len + style_len + 8 * sizeof(int32_t) + 4 * sizeof(double))
    return 1;

  // set the GUI with corresponding values
  if (printer[0] != '\0')
    _bauhaus_combobox_set_active_text(ps->printers, printer);

  if (paper[0] != '\0')
    _bauhaus_combobox_set_active_text(ps->papers, paper);

  if (media[0] != '\0')
    _bauhaus_combobox_set_active_text(ps->media, media);

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

  if (style[0] != '\0')
    _bauhaus_combobox_set_active_text(ps->style, style);
  dt_bauhaus_combobox_set (ps->style_mode, style_mode);

  gtk_spin_button_set_value (GTK_SPIN_BUTTON(ps->b_top), b_top * units[ps->unit]);
  gtk_spin_button_set_value (GTK_SPIN_BUTTON(ps->b_bottom), b_bottom * units[ps->unit]);
  gtk_spin_button_set_value (GTK_SPIN_BUTTON(ps->b_left), b_left * units[ps->unit]);
  gtk_spin_button_set_value (GTK_SPIN_BUTTON(ps->b_right), b_right * units[ps->unit]);

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ps->dtba[alignment]),TRUE);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ps->black_point_compensation), bpc);

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
  const int32_t alignment = ps->prt.page.alignment;

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
  *size = printer_len + paper_len + media_len + profile_len + pprofile_len + style_len + 8 * sizeof(int32_t) + 4 * sizeof(double);

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

  g_assert(pos == *size);

  return params;
}

void
gui_cleanup (dt_lib_module_t *self)
{
  dt_lib_print_settings_t *ps = (dt_lib_print_settings_t *)self->data;

  dt_gui_key_accel_block_on_focus_disconnect(GTK_WIDGET(ps->b_top));
  dt_gui_key_accel_block_on_focus_disconnect(GTK_WIDGET(ps->b_left));
  dt_gui_key_accel_block_on_focus_disconnect(GTK_WIDGET(ps->b_right));
  dt_gui_key_accel_block_on_focus_disconnect(GTK_WIDGET(ps->b_bottom));

  g_list_free_full(ps->profiles, g_free);
  g_list_free_full(ps->paper_list, free);
  g_list_free_full(ps->media_list, free);

  g_free(ps->v_iccprofile);
  g_free(ps->v_piccprofile);
  g_free(ps->v_style);

  free(self->data);
  self->data = NULL;
}

void
gui_reset (dt_lib_module_t *self)
{
  dt_lib_print_settings_t *ps = (dt_lib_print_settings_t *)self->data;

  gtk_spin_button_set_value(GTK_SPIN_BUTTON(ps->b_top), 15 * units[ps->unit]);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(ps->b_bottom), 15 * units[ps->unit]);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(ps->b_left), 15 * units[ps->unit]);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(ps->b_right), 15 * units[ps->unit]);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ps->dtba[ALIGNMENT_CENTER]), TRUE);
  ps->prt.page.alignment = ALIGNMENT_CENTER;
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

  // reset page orientation to fit the picture

  _set_orientation (ps);
}

void init_key_accels(dt_lib_module_t *self)
{
  dt_accel_register_lib(self, NC_("accel", "print"), GDK_KEY_p, GDK_CONTROL_MASK);
}

void connect_key_accels(dt_lib_module_t *self)
{
  dt_lib_print_settings_t *d = (dt_lib_print_settings_t *)self->data;

  dt_accel_connect_button_lib(self, "print", GTK_WIDGET(d->print_button));
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
