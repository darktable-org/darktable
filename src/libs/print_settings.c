/*
    This file is part of darktable,
    copyright (c) 2014-2015 pascal obry.

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

#include "common/collection.h"
#include "common/colorspaces.h"
#include "common/image_cache.h"
#include "common/styles.h"
#include "common/variables.h"
#include "common/cups_print.h"
#include "common/image_cache.h"
#include "common/pdf.h"
#include "common/tags.h"
#include "common/printprof.h"
#include "dtgtk/resetlabel.h"
#include "libs/lib.h"
#include "gui/gtk.h"
#include "bauhaus/bauhaus.h"

DT_MODULE(1)

static gboolean _bauhaus_combobox_set_active_text(GtkWidget *cb, const gchar *text);

const char*
name ()
{
  return _("print settings");
}

uint32_t views()
{
  return DT_VIEW_PRINT;
}

uint32_t container()
{
  return DT_UI_CONTAINER_PANEL_RIGHT_CENTER;
}

typedef struct dt_lib_print_settings_t
{
  GtkWidget *profile, *intent, *style, *style_mode, *papers;
  GtkWidget *printers, *orientation, *pprofile, *pintent;
  GtkWidget *width, *height, *black_point_compensation;
  GtkWidget *info;
  GList *profiles;
  GtkButton *print_button;
  GtkToggleButton *lock_button;
  GtkWidget *b_top, *b_bottom, *b_left, *b_right;
  GtkDarktableToggleButton *dtba[9];	                                   // Alignment buttons
  GList *paper_list;
  gboolean lock_activated;
  dt_print_info_t prt;
  uint16_t *buf;
  int32_t image_id;
  int32_t iwidth, iheight;
  int unit;
  int v_intent, v_pintent;
  char *v_iccprofile, *v_piccprofile, *v_style;
  gboolean v_style_append, v_black_point_compensation;
} dt_lib_print_settings_t;

typedef struct dt_lib_export_profile_t
{
  char filename[512]; // icc file name
  char name[512];     // product name
  int  pos;           // position in combo box
}
dt_lib_export_profile_t;

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
  dt_lib_print_settings_t *ps;
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
                       void *exif, int exif_len, int imgid, int num, int total)
{
  dt_print_format_t *d = (dt_print_format_t *)data;

  d->ps->buf = (uint16_t *)malloc(d->width * d->height * 3 * (d->bpp == 8?1:2));

  if (d->bpp == 8)
  {
    const uint8_t *in_ptr = (const uint8_t *)in;
    uint8_t *out_ptr = (uint8_t *)d->ps->buf;
    for(int y = 0; y < d->height; y++)
    {
      for(int x = 0; x < d->width; x++, in_ptr += 4, out_ptr += 3)
        memcpy(out_ptr, in_ptr, 3);
    }
  }
  else
  {
    const uint16_t *in_ptr = (const uint16_t *)in;
    uint16_t *out_ptr = (uint16_t *)d->ps->buf;
    for(int y = 0; y < d->height; y++)
    {
      for(int x = 0; x < d->width; x++, in_ptr += 4, out_ptr += 3)
        memcpy(out_ptr, in_ptr, 6);
    }
  }

  return 0;
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

  dt_control_log(_("prepare printing image %d on `%s'"), imgid, ps->prt.printer.name);

  // user margin are already in the proper orientation landscape/portrait
  double width, height;
  double margin_w = ps->prt.page.margin_left + ps->prt.page.margin_right;
  double margin_h = ps->prt.page.margin_top + ps->prt.page.margin_bottom;

  if (ps->prt.page.landscape)
  {
    width = ps->prt.paper.height;
    height = ps->prt.paper.width;
    margin_w += ps->prt.printer.hw_margin_top + ps->prt.printer.hw_margin_bottom;
    margin_h += ps->prt.printer.hw_margin_left + ps->prt.printer.hw_margin_right;
  }
  else
  {
    width = ps->prt.paper.width;
    height = ps->prt.paper.height;
    margin_w += ps->prt.printer.hw_margin_left + ps->prt.printer.hw_margin_right;
    margin_h += ps->prt.printer.hw_margin_top + ps->prt.printer.hw_margin_bottom;
  }

  const int32_t width_pix = (width * ps->prt.printer.resolution) / 25.4;
  const int32_t height_pix = (height * ps->prt.printer.resolution) / 25.4;

  const double pa_width  = (width  - margin_w) / 25.4;
  const double pa_height = (height - margin_h) / 25.4;

  dt_print(DT_DEBUG_PRINT, "[print] printable area for image %u : %3.2fin x %3.2fin\n", imgid, pa_width, pa_height);

  // compute the needed size for picture for the given printer resolution

  const int max_width  = (pa_width  * ps->prt.printer.resolution);
  const int max_height = (pa_height * ps->prt.printer.resolution);

  dt_print(DT_DEBUG_PRINT, "[print] max image size %d x %d (at resolution %d)\n", max_width, max_height, ps->prt.printer.resolution);

  dt_imageio_module_format_t buf;
  buf.mime = mime;
  buf.levels = levels;
  buf.bpp = bpp;
  buf.write_image = write_image;

  dt_print_format_t dat;
  dat.max_width = max_width;
  dat.max_height = max_height;
  dat.style[0] = '\0';
  dat.style_append = ps->v_style_append;
  dat.bpp = *ps->v_piccprofile ? 16 : 8; // set to 16bit when a profile is to be applied
  dat.ps = ps;

  char* style = dt_conf_get_string("plugins/print/print/style");
  if (style)
  {
    g_strlcpy(dat.style, style, sizeof(dat.style));
    g_free(style);
  }

  // the flags are: ignore exif, display byteorder, high quality, upscale, thumbnail
  const int high_quality = 1;
  const int upscale = 1;
  dt_imageio_export_with_flags(imgid, "unused", &buf, (dt_imageio_module_data_t *)&dat, 1, 0,
                               high_quality, upscale, 0, NULL, FALSE, 0, 0, 1, 1);

  // after exporting we know the real size of the image, compute the layout

  // compute print-area (in inches)
  int32_t px=0, py=0, pwidth=0, pheight=0;
  int32_t ax=0, ay=0, awidth=0, aheight=0;
  int32_t ix=0, iy=0, iwidth=0, iheight=0;
  int32_t iwpix=dat.width, ihpix=dat.height;

  dt_get_print_layout (imgid, &ps->prt, width_pix, height_pix,
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

  if (*ps->v_piccprofile)
    if (dt_apply_printer_profile(imgid, (void **)&(dat.ps->buf), dat.width, dat.height, dat.bpp,
                                 ps->v_piccprofile, ps->v_pintent, ps->v_black_point_compensation))
    {
      free(dat.ps->buf);
      dt_control_log(_("cannot apply printer profile `%s'"), ps->v_piccprofile);
      fprintf(stderr, "cannot apply printer profile `%s'\n", ps->v_piccprofile);
      dt_control_queue_redraw();
      return;
    }

  const float page_width  = dt_pdf_mm_to_point(width);
  const float page_height = dt_pdf_mm_to_point(height);

  char filename[PATH_MAX] = { 0 };
  dt_loc_get_tmp_dir(filename, sizeof(filename));
  g_strlcat(filename, "/pf.XXXXXX.pdf", sizeof(filename));

  gint fd = g_mkstemp(filename);
  if(fd == -1)
  {
    free(dat.ps->buf);
    dt_control_log("failed to create temporary pdf for printing");
    fprintf(stderr, "failed to create temporary pdf for printing\n");
    return;
  }
  close(fd);

  const int icc_id = 0;

  dt_pdf_t *pdf = dt_pdf_start(filename, page_width, page_height, ps->prt.printer.resolution, DT_PDF_STREAM_ENCODER_FLATE);

/*
  // ??? should a profile be embedded here?
  if (*printer_profile)
    icc_id = dt_pdf_add_icc(pdf, printer_profile);
*/
  dt_pdf_image_t *pdf_image = dt_pdf_add_image(pdf, (uint8_t *)dat.ps->buf, dat.width, dat.height, 8, icc_id, 0.0);

  //  PDF bounding-box has origin on bottom-left
  pdf_image->bb_x      = dt_pdf_pixel_to_point((float)margin_left, ps->prt.printer.resolution);
  pdf_image->bb_y      = dt_pdf_pixel_to_point((float)margin_bottom, ps->prt.printer.resolution);
  pdf_image->bb_width  = dt_pdf_pixel_to_point((float)iwidth, ps->prt.printer.resolution);
  pdf_image->bb_height = dt_pdf_pixel_to_point((float)iheight, ps->prt.printer.resolution);

  if (ps->prt.page.landscape && (dat.width > dat.height))
    pdf_image->rotate_to_fit = TRUE;
  else
    pdf_image->rotate_to_fit = FALSE;

  dt_pdf_page_t *pdf_page = dt_pdf_add_page(pdf, &pdf_image, 1);
  dt_pdf_finish(pdf, &pdf_page, 1);

  // free memory

  free (dat.ps->buf);
  free (pdf_image);
  free (pdf_page);

  // send to CUPS

  dt_print_file (imgid, filename, &ps->prt);

  unlink(filename);

  // add tag for this image

  char tag[256] = { 0 };
  guint tagid = 0;
  snprintf (tag, sizeof(tag), "darktable|printed|%s", ps->prt.printer.name);
  dt_tag_new(tag, &tagid);
  dt_tag_attach(tagid, imgid);
}

static void _set_printer(const dt_lib_module_t *self, const char *printer_name)
{
  dt_lib_print_settings_t *ps = (dt_lib_print_settings_t *)self->data;
  dt_printer_info_t *printer = dt_get_printer_info (printer_name);

  if (!printer) return;

  memcpy(&ps->prt.printer, printer, sizeof(dt_printer_info_t));
  free(printer);
  printer = NULL;

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

  ps->paper_list = dt_get_papers (printer_name);
  GList *papers = ps->paper_list;
  int np = 0;
  gboolean ispaperset = FALSE;

  while (papers)
  {
    const dt_paper_info_t *p = (dt_paper_info_t *)papers->data;
    dt_bauhaus_combobox_add(ps->papers, p->common_name);

    if (ispaperset == FALSE && (!strcmp(default_paper, p->common_name) || default_paper[0] == '\0'))
    {
      dt_bauhaus_combobox_set(ps->papers, np);
      ispaperset = TRUE;
    }

    np++;
    papers = g_list_next (papers);
  }

  const dt_paper_info_t *paper = dt_get_paper(ps->paper_list, default_paper);

  if (paper)
    memcpy(&ps->prt.paper, paper, sizeof(dt_paper_info_t));

  g_free (default_paper);

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
    char value[20];

    sprintf(value, "%3.2f", w);
    gtk_label_set_text(GTK_LABEL(ps->width), value);

    sprintf(value, "%3.2f", h);
    gtk_label_set_text(GTK_LABEL(ps->height), value);

    // compute the image down/up scale and report information
    double scale;

    if (iwidth >= awidth)
      scale = dt_pdf_point_to_pixel(dt_pdf_mm_to_point((double)awidth), ps->prt.printer.resolution) / ps->iwidth;
    else
      scale = dt_pdf_point_to_pixel(dt_pdf_mm_to_point((double)aheight), ps->prt.printer.resolution) / ps->iheight;

    sprintf(value, _("%3.2f (dpi:%d)"), scale, scale<=1.0 ? (int)ps->prt.printer.resolution : (int)(ps->prt.printer.resolution / scale));
    gtk_label_set_text(GTK_LABEL(ps->info), value);
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

  if (ps->v_style) g_free(ps->v_style);
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
    // could use g_list_nth. this seems safer?
    dt_lib_export_profile_t *pp = (dt_lib_export_profile_t *)prof->data;
    if(pp->pos == pos)
    {
      dt_conf_set_string("plugins/lighttable/export/iccprofile", pp->filename);
      dt_conf_set_string("plugins/print/print/iccprofile", pp->filename);
      if (ps->v_iccprofile) g_free(ps->v_iccprofile);
      ps->v_iccprofile = g_strdup(pp->filename);
      return;
    }
    prof = g_list_next(prof);
  }
  dt_conf_set_string("plugins/lighttable/export/iccprofile", "image");
  dt_conf_set_string("plugins/print/print/iccprofile", "image");
  if (ps->v_iccprofile) g_free(ps->v_iccprofile);
  ps->v_iccprofile = g_strdup("image");
}

static void
_printer_profile_changed(GtkWidget *widget, dt_lib_module_t *self)
{
  dt_lib_print_settings_t *ps = (dt_lib_print_settings_t *)self->data;
  const gchar *printer_profile = dt_bauhaus_combobox_get_text(widget);
  GList *prof = ps->profiles;
  while(prof)
  {
    // could use g_list_nth. this seems safer?
    dt_lib_export_profile_t *pp = (dt_lib_export_profile_t *)prof->data;
    if(strcmp(pp->name,printer_profile)==0)
    {
      dt_conf_set_string("plugins/print/printer/iccprofile", pp->filename);
      if (ps->v_piccprofile) g_free(ps->v_piccprofile);
      ps->v_piccprofile = g_strdup(pp->filename);

      // activate the black compensation and printer intent
      gtk_widget_set_sensitive(GTK_WIDGET(ps->pintent), TRUE);
      gtk_widget_set_sensitive(GTK_WIDGET(ps->black_point_compensation), TRUE);
      return;
    }
    prof = g_list_next(prof);
  }
  dt_conf_set_string("plugins/print/printer/iccprofile", "");
  if (ps->v_piccprofile) g_free(ps->v_piccprofile);
  ps->v_piccprofile = g_strdup("");
  gtk_widget_set_sensitive(GTK_WIDGET(ps->pintent), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(ps->black_point_compensation), FALSE);
}

static void
_printer_intent_callback (GtkWidget *widget, dt_lib_module_t *self)
{
  dt_lib_print_settings_t *ps = (dt_lib_print_settings_t *)self->data;
  const int pos = dt_bauhaus_combobox_get(widget);
  dt_conf_set_int("plugins/print/printer/iccintent", pos);
  ps->v_pintent = pos;
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
  dt_conf_set_int("plugins/lighttable/export/iccintent", pos - 1);
  dt_conf_set_int("plugins/print/print/iccintent", pos - 1);
  ps->v_intent = pos - 1;
}

static void _set_orientation(dt_lib_print_settings_t *ps)
{
  if (ps->image_id <= 0)
    return;

  dt_mipmap_buffer_t buf;
  dt_mipmap_cache_get(darktable.mipmap_cache, &buf, ps->image_id, DT_MIPMAP_3, DT_MIPMAP_BEST_EFFORT, 'r');

  if (buf.width > buf.height)
    ps->prt.page.landscape = TRUE;
  else
    ps->prt.page.landscape = FALSE;

  dt_mipmap_cache_release(darktable.mipmap_cache, &buf);
}

static void _print_settings_filmstrip_activate_callback(gpointer instance,gpointer user_data)
{
  const dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_print_settings_t *ps = (dt_lib_print_settings_t *)self->data;

  ps->image_id = dt_view_filmstrip_get_activated_imgid(darktable.view_manager);
  ps->iwidth = ps->iheight = 0;

  _set_orientation (ps);

  dt_bauhaus_combobox_set (ps->orientation, ps->prt.page.landscape==TRUE?1:0);
}

static GList* _get_profiles ()
{
  //  Create list of profiles
  GList *list = NULL;

  dt_lib_export_profile_t *prof = (dt_lib_export_profile_t *)g_malloc0(sizeof(dt_lib_export_profile_t));
  g_strlcpy(prof->filename, "sRGB", sizeof(prof->filename));
  dt_utf8_strlcpy(prof->name, _("sRGB (web-safe)"), sizeof(prof->name));
  int pos;
  prof->pos = 1;
  list = g_list_append(list, prof);

  prof = (dt_lib_export_profile_t *)g_malloc0(sizeof(dt_lib_export_profile_t));
  g_strlcpy(prof->filename, "adobergb", sizeof(prof->filename));
  dt_utf8_strlcpy(prof->name, _("Adobe RGB (compatible)"), sizeof(prof->name));
  pos = prof->pos = 2;
  list = g_list_append(list, prof);

  // read datadir/color/out/*.icc
  char datadir[PATH_MAX] = { 0 };
  char confdir[PATH_MAX] = { 0 };
  char dirname[PATH_MAX] = { 0 };
  char filename[PATH_MAX] = { 0 };
  dt_loc_get_user_config_dir(confdir, sizeof(confdir));
  dt_loc_get_datadir(datadir, sizeof(datadir));
  cmsHPROFILE tmpprof;
  const gchar *d_name;
  snprintf(dirname, sizeof(dirname), "%s/color/out", confdir);
  if(!g_file_test(dirname, G_FILE_TEST_IS_DIR))
    snprintf(dirname, sizeof(dirname), "%s/color/out", datadir);
  GDir *dir = g_dir_open(dirname, 0, NULL);
  if(dir)
  {
    while((d_name = g_dir_read_name(dir)))
    {
      snprintf(filename, sizeof(filename), "%s/%s", dirname, d_name);
      tmpprof = cmsOpenProfileFromFile(filename, "r");
      if(tmpprof)
      {
        char *lang = getenv("LANG");
        if (!lang) lang = "en_US";

        dt_lib_export_profile_t *prof = (dt_lib_export_profile_t *)g_malloc0(sizeof(dt_lib_export_profile_t));
        dt_colorspaces_get_profile_name(tmpprof, lang, lang+3, prof->name, sizeof(prof->name));
        g_strlcpy(prof->filename, filename, sizeof(prof->filename));
        prof->pos = ++pos;
        cmsCloseProfile(tmpprof);
        list = g_list_append(list, prof);
      }
    }
    g_dir_close(dir);
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

  if (!strcmp(default_printer, printer->name) || default_printer[0]=='\0')
  {
    dt_bauhaus_combobox_set(d->printers, count);
    _set_printer(self, printer->name);
  }
  count++;
  g_free(default_printer);

  g_signal_handlers_unblock_by_func(G_OBJECT(d->printers), G_CALLBACK(_printer_changed), NULL);
}

void
gui_init (dt_lib_module_t *self)
{
  dt_lib_print_settings_t *d = (dt_lib_print_settings_t*)malloc(sizeof(dt_lib_print_settings_t));
  self->data = d;
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);

  char datadir[PATH_MAX] = { 0 };
  char confdir[PATH_MAX] = { 0 };
  dt_loc_get_user_config_dir(confdir, sizeof(confdir));
  dt_loc_get_datadir(datadir, sizeof(datadir));

  GtkWidget *label;
  char tooltip[1024];

  d->paper_list = NULL;
  d->iwidth = d->iheight = 0;
  d->unit = 0;
  d->width = d->height = NULL;
  d->v_piccprofile = NULL;
  d->v_iccprofile = NULL;
  d->v_style = NULL;

  dt_init_print_info(&d->prt);
  dt_view_print_settings(darktable.view_manager, &d->prt);

  dt_control_signal_connect(darktable.signals,
                            DT_SIGNAL_VIEWMANAGER_FILMSTRIP_ACTIVATE,
                            G_CALLBACK(_print_settings_filmstrip_activate_callback),
                            self);

  d->profiles = _get_profiles();

  //  get orientation of the selectd image if possible

  d->image_id = -1;

  GList *selected_images = dt_collection_get_selected(darktable.collection, 1);
  if(selected_images)
  {
    int imgid = GPOINTER_TO_INT(selected_images->data);
    d->image_id = imgid;
  }
  g_list_free(selected_images);

  _set_orientation(d);

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

  d->printers = dt_bauhaus_combobox_new(NULL);

  gtk_box_pack_start(GTK_BOX(self->widget), d->printers, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(d->printers), "value-changed", G_CALLBACK(_printer_changed), self);

  //  Add printer profile combo

  d->pprofile = dt_bauhaus_combobox_new(NULL);
  dt_bauhaus_widget_set_label(d->pprofile, NULL, _("profile"));

  int combo_idx = -1, n=0;
  GList *l = d->profiles;

  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(d->pprofile), TRUE, TRUE, 0);
  gchar *printer_profile = dt_conf_get_string("plugins/print/printer/iccprofile");
  combo_idx = -1;
  n=0;

  dt_bauhaus_combobox_add(d->pprofile, _("none"));
  while(l)
  {
    dt_lib_export_profile_t *prof = (dt_lib_export_profile_t *)l->data;
    // do not add built-in profile, these are in no way for printing
    if (strcmp(prof->filename,"sRGB")!=0 && strcmp(prof->filename,"adobergb")!=0)
    {
      dt_bauhaus_combobox_add(d->pprofile, prof->name);
      n++;
      if (strcmp(prof->filename,printer_profile)==0)
      {
        if(d->v_piccprofile) g_free(d->v_piccprofile);
        d->v_piccprofile = g_strdup(printer_profile);
        combo_idx=n;
      }
    }
    l = g_list_next(l);
  }

  g_free (printer_profile);

  // profile not found, maybe a profile has been removed? revert to none
  if (combo_idx == -1)
  {
    dt_conf_set_string("plugins/print/printer/iccprofile", "");
    if(d->v_piccprofile) g_free(d->v_piccprofile);
    d->v_piccprofile = g_strdup("");
    combo_idx=0;
  }
  dt_bauhaus_combobox_set(d->pprofile, combo_idx);

  snprintf(tooltip, sizeof(tooltip), _("printer ICC profiles in %s/color/out or %s/color/out"), confdir, datadir);
  g_object_set(G_OBJECT(d->pprofile), "tooltip-text", tooltip, (char *)NULL);
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

  d->black_point_compensation = gtk_check_button_new_with_label(_("black point compensation"));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(d->black_point_compensation), TRUE, FALSE, 0);
  g_signal_connect(d->black_point_compensation, "toggled", G_CALLBACK(_printer_bpc_callback), (gpointer)self);

  d->v_black_point_compensation = dt_conf_get_bool("plugins/print/print/black_point_compensation");
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->black_point_compensation), d->v_black_point_compensation);

  g_object_set(d->black_point_compensation, "tooltip-text",
               _("activate black point compensation when applying the printer profile"), (char *)NULL);

  gtk_widget_set_sensitive(GTK_WIDGET(d->pintent), combo_idx==0?FALSE:TRUE);
  gtk_widget_set_sensitive(GTK_WIDGET(d->black_point_compensation), combo_idx==0?FALSE:TRUE);

  ////////////////////////// PAGE SETTINGS

  label = dt_ui_section_label_new(_("page"));
  gtk_box_pack_start(GTK_BOX(self->widget), label, TRUE, TRUE, 0);

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
  g_object_set(G_OBJECT(hboxinfo), "tooltip-text",
               _("image scale factor from native printer DPI:\n"
                 " < 0 means that it is downscaled (best quality)\n"
                 " > 0 means that the image is upscaled\n"
                 " a too large value may result in poor print quality"), (char *)NULL);

  //// borders

  GtkGrid *bds = GTK_GRID(gtk_grid_new());
  gtk_grid_set_row_spacing(bds, DT_PIXEL_APPLY_DPI(3));
  gtk_grid_set_column_spacing(bds, DT_PIXEL_APPLY_DPI(3));

  d->lock_activated = FALSE;

  //d->b_top  = gtk_spin_button_new_with_range(0, 10000, 1);
  g_object_set(G_OBJECT(d->b_top), "tooltip-text", _("top margin"), (char *)NULL);
  gtk_grid_attach(bds, GTK_WIDGET(d->b_top), 1, 0, 1, 1);

  //d->b_left  = gtk_spin_button_new_with_range(0, 10000, 1);
  g_object_set(G_OBJECT(d->b_left), "tooltip-text", _("left margin"), (char *)NULL);
  gtk_grid_attach(bds, GTK_WIDGET(d->b_left), 0, 1, 1, 1);

  d->lock_button = GTK_TOGGLE_BUTTON(gtk_toggle_button_new_with_label(_("lock")));
  g_object_set(G_OBJECT(d->lock_button), "tooltip-text", _("change all margins uniformly"), (char *)NULL);
  gtk_grid_attach(bds, GTK_WIDGET(d->lock_button), 1, 1, 1, 1);

  //d->b_right  = gtk_spin_button_new_with_range(0, 10000, 1);
  g_object_set(G_OBJECT(d->b_right), "tooltip-text", _("right margin"), (char *)NULL);
  gtk_grid_attach(bds, GTK_WIDGET(d->b_right), 2, 1, 1, 1);

  //d->b_bottom  = gtk_spin_button_new_with_range(0, 10000, 1);
  g_object_set(G_OBJECT(d->b_bottom), "tooltip-text", _("bottom margin"), (char *)NULL);
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

  // pack image dimention hbox here

  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hboxdim), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hboxinfo), TRUE, TRUE, 0);

  //// alignments

  // Create the 3x3 gtk table toggle button table...
  GtkGrid *bat = GTK_GRID(gtk_grid_new());
  gtk_grid_set_row_spacing(bat, DT_PIXEL_APPLY_DPI(3));
  gtk_grid_set_column_spacing(bat, DT_PIXEL_APPLY_DPI(3));
  for(int i=0; i<9; i++)
  {
    d->dtba[i] = DTGTK_TOGGLEBUTTON (dtgtk_togglebutton_new (dtgtk_cairo_paint_alignment,CPF_STYLE_FLAT|(CPF_SPECIAL_FLAG<<i)));
    gtk_grid_attach (GTK_GRID (bat), GTK_WIDGET (d->dtba[i]), (i%3), i/3, 1, 1);
    g_signal_connect (G_OBJECT (d->dtba[i]), "toggled",G_CALLBACK (_alignment_callback), self);
  }
  d->prt.page.alignment = center;
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->dtba[d->prt.page.alignment]),TRUE);

  GtkWidget *hbox22 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL,0);
  GtkWidget *label4 = gtk_label_new(_("alignment"));
  gtk_box_pack_start(GTK_BOX(hbox22),GTK_WIDGET(label4),TRUE,TRUE,0);
  gtk_box_pack_start(GTK_BOX(hbox22), GTK_WIDGET(bat), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hbox22), TRUE, TRUE, 0);

  ////////////////////////// PRINT SETTINGS

  label = dt_ui_section_label_new(_("print settings"));
  gtk_box_pack_start(GTK_BOX(self->widget), label, TRUE, TRUE, 0);

  //  Add export profile combo

  d->profile = dt_bauhaus_combobox_new(NULL);
  dt_bauhaus_widget_set_label(d->profile, NULL, _("profile"));

  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(d->profile), TRUE, TRUE, 0);
  dt_bauhaus_combobox_add(d->profile, _("image settings"));

  gchar *iccprofile = dt_conf_get_string("plugins/print/print/iccprofile");
  combo_idx = -1;
  n=0;

  l = d->profiles;
  while(l)
  {
    dt_lib_export_profile_t *prof = (dt_lib_export_profile_t *)l->data;
    dt_bauhaus_combobox_add(d->profile, prof->name);
    n++;
    if (strcmp(prof->filename, iccprofile)==0)
    {
      if(d->v_iccprofile) g_free(d->v_iccprofile);
      d->v_iccprofile = g_strdup(iccprofile);
      combo_idx=n;
    }
    l = g_list_next(l);
  }

  if (combo_idx == -1)
  {
    dt_conf_set_string("plugins/print/print/iccprofile", "image");
    if(d->v_iccprofile) g_free(d->v_iccprofile);
    d->v_iccprofile = g_strdup("");
    combo_idx=0;
  }
  g_free (iccprofile);

  dt_bauhaus_combobox_set(d->profile, combo_idx);

  snprintf(tooltip, sizeof(tooltip), _("output ICC profiles in %s/color/out or %s/color/out"), confdir, datadir);
  g_object_set(G_OBJECT(d->profile), "tooltip-text", tooltip, (char *)NULL);
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

  dt_bauhaus_combobox_set(d->intent, 0);

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
    if (strcmp(style->name,current_style)==0)
    {
      if(d->v_style) g_free(d->v_style);
      d->v_style = g_strdup(current_style);
      combo_idx=n;
    }
    styles=g_list_next(styles);
  }
  g_free(current_style);
  g_list_free_full(styles, dt_style_free);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(d->style), TRUE, TRUE, 0);
  g_object_set(G_OBJECT(d->style), "tooltip-text", _("temporary style to use while printing"), (char *)NULL);

  // style not found, maybe a style has been removed? revert to none
  if (combo_idx == -1)
  {
    dt_conf_set_string("plugins/print/print/style", "");
    if(d->v_style) g_free(d->v_style);
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
  g_object_set(G_OBJECT(d->style_mode), "tooltip-text", _("whether the style is appended to the history or replacing the history"),
               (char *)NULL);

  gtk_widget_set_sensitive(GTK_WIDGET(d->style_mode), combo_idx==0?FALSE:TRUE);

  g_signal_connect(G_OBJECT(d->style_mode), "value-changed", G_CALLBACK(_style_mode_changed), (gpointer)self);

  // Print button

  GtkButton *button = GTK_BUTTON(gtk_button_new_with_label(_("print")));
  d->print_button = button;
  g_object_set(G_OBJECT(button), "tooltip-text", _("print with current settings (ctrl-p)"), (char *)NULL);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(button), TRUE, TRUE, 0);

  g_signal_connect (G_OBJECT (button), "clicked",
                    G_CALLBACK (_print_button_clicked),
                    (gpointer)self);

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

static const char *none = "none";

static const char *_get_profile_filename(GList *profiles, const char *name)
{
  GList *p = profiles;
  while(p)
  {
    // could use g_list_nth. this seems safer?
    dt_lib_export_profile_t *pp = (dt_lib_export_profile_t *)p->data;
    if(!strcmp(pp->name, name))
    {
      const char *ds = strrchr(pp->filename,'/'); // keep last / to ensure full match of filename
      return ds ? ds : pp->filename;
    }
    p = g_list_next(p);
  }
  return none;
}

static const char *_get_profile(GList *profiles, const char *filename)
{
  GList *p = profiles;
  while(p)
  {
    dt_lib_export_profile_t *pp = (dt_lib_export_profile_t *)p->data;
    if(strstr(pp->filename, filename))
    {
      return pp->name;
    }
    p = g_list_next(p);
  }
  return none;
}

int set_params(dt_lib_module_t *self, const void *params, int size)
{
  const dt_lib_print_settings_t *ps = (dt_lib_print_settings_t *)self->data;

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

  const char *f_profile = buf;
  if (!f_profile) return 1;
  const int32_t profile_len = strlen(f_profile) + 1;
  buf += profile_len;

  const int32_t intent = *(int32_t *)buf;
  buf += sizeof(int32_t);

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

  // ensure that the size is correct
  if(size != printer_len + paper_len + profile_len + pprofile_len + style_len + 6 * sizeof(int32_t) + 4 * sizeof(double))
    return 1;

  // set the GUI with corresponding values
  if (printer[0] != '\0')
    _bauhaus_combobox_set_active_text(ps->printers, printer);

  if (paper[0] != '\0')
    _bauhaus_combobox_set_active_text(ps->papers, paper);

  dt_bauhaus_combobox_set (ps->orientation, landscape);

  const char *profile = _get_profile(ps->profiles, f_profile);

  if (profile[0] != '\0')
    _bauhaus_combobox_set_active_text(ps->profile, profile);
  dt_bauhaus_combobox_set (ps->intent, intent);

  const char *pprofile = _get_profile(ps->profiles, f_pprofile);

  if (pprofile[0] != '\0')
    _bauhaus_combobox_set_active_text(ps->pprofile, pprofile);
  dt_bauhaus_combobox_set (ps->pintent, pintent);

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
  const char *profile = _get_profile_filename(ps->profiles, dt_bauhaus_combobox_get_text(ps->profile));
  const int32_t intent =  dt_bauhaus_combobox_get(ps->intent);
  const char *style = dt_bauhaus_combobox_get_text(ps->style);
  const int32_t style_mode = dt_bauhaus_combobox_get(ps->style_mode);
  const char *pprofile = _get_profile_filename(ps->profiles, dt_bauhaus_combobox_get_text(ps->pprofile));
  const int32_t pintent =  dt_bauhaus_combobox_get(ps->pintent);
  const int32_t landscape = dt_bauhaus_combobox_get(ps->orientation);
  const int32_t bpc = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ps->black_point_compensation));
  const double b_top = ps->prt.page.margin_top;
  const double b_bottom = ps->prt.page.margin_bottom;
  const double b_left = ps->prt.page.margin_left;
  const double b_right = ps->prt.page.margin_right;
  const int32_t alignment = ps->prt.page.alignment;

  // these will be NULL when no printer is connected/found
  if(!printer) printer = "";
  if(!paper) paper = "";

  // compute the size of individual items, always get the \0 for strings
  const int32_t printer_len = strlen (printer) + 1;
  const int32_t paper_len = strlen (paper) + 1;
  const int32_t profile_len = strlen (profile) + 1;
  const int32_t pprofile_len = strlen (pprofile) + 1;
  const int32_t style_len = strlen (style) + 1;

  // compute the size of all parameters
  *size = printer_len + paper_len + profile_len + pprofile_len + style_len + 6 * sizeof(int32_t) + 4 * sizeof(double);

  // allocate the parameter buffer
  char *params = (char *)malloc(*size);

  int pos = 0;

  memcpy(params+pos, printer, printer_len);
  pos += printer_len;
  memcpy(params+pos, paper, paper_len);
  pos += paper_len;
  memcpy(params+pos, &landscape, sizeof(int32_t));
  pos += sizeof(int32_t);
  memcpy(params+pos, profile, profile_len);
  pos += profile_len;
  memcpy(params+pos, &intent, sizeof(int32_t));
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

  dt_control_signal_disconnect(darktable.signals,
                               G_CALLBACK(_print_settings_filmstrip_activate_callback),
                               self);

  g_list_free_full(ps->profiles, g_free);
  g_list_free_full(ps->paper_list, free);

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
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ps->dtba[center]),TRUE);
  ps->prt.page.alignment = center;
  dt_bauhaus_combobox_set(ps->profile, 0);
  dt_bauhaus_combobox_set(ps->pprofile, 0);
  dt_bauhaus_combobox_set(ps->pintent, dt_conf_get_int("plugins/print/print/iccintent") + 1);
  dt_bauhaus_combobox_set(ps->style, 0);
  dt_bauhaus_combobox_set(ps->intent, 0);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ps->black_point_compensation), TRUE);
  gtk_widget_set_sensitive(GTK_WIDGET(ps->pintent), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(ps->black_point_compensation), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(ps->style_mode), FALSE);

  // reset page orientation to fit the picture

  _set_orientation (ps);

  dt_bauhaus_combobox_set (ps->orientation, ps->prt.page.landscape?1:0);
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
