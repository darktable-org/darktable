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

#include "common/colorspaces.h"
#include "common/image_cache.h"
#include "common/styles.h"
#include "common/variables.h"
#include "common/cups_print.h"
#include "common/image_cache.h"
#include "common/pdf.h"
#include "common/tags.h"
#include "dtgtk/resetlabel.h"
#include "libs/lib.h"
#include "gui/gtk.h"
#include "gtk/gtk.h"
#include "bauhaus/bauhaus.h"

DT_MODULE(1)

static gboolean _bauhaus_combo_box_set_active_text(GtkWidget *cb, const gchar *text);
static gboolean _combo_box_set_active_text(GtkComboBox *cb, const gchar *text);

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
  GtkComboBox *printers;
  GtkWidget *profile, *intent, *style, *style_mode, *papers;
  GtkWidget *landscape, *portrait, *pprofile, *pintent;
  GList *profiles;
  GtkButton *print_button;
  GtkToggleButton *lock_button;
  GtkWidget *b_top, *b_bottom, *b_left, *b_right;
  GtkDarktableToggleButton *dtba[9];	                                   // Alignment buttons
  GList *paper_list;
  gboolean lock_activated;
  dt_print_info_t prt;
  uint16_t *buf;
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
  dt_lib_print_settings_t *ps;
} dt_print_format_t;

static int bpp(dt_imageio_module_data_t *data)
{
  return 8;
}

static int levels(dt_imageio_module_data_t *data)
{
  return IMAGEIO_RGB | IMAGEIO_INT8;
}

static const char *mime(dt_imageio_module_data_t *data)
{
  return "memory";
}

static int write_image(dt_imageio_module_data_t *datai, const char *filename, const void *in, void *exif,
                       int exif_len, int imgid)
{
  dt_print_format_t *data = (dt_print_format_t *)datai;

  data->ps->buf = (uint16_t *)malloc(data->width * data->height * 3 * sizeof(uint16_t));

  const uint8_t *in_ptr = (const uint8_t *)in;
  uint8_t *out_ptr = (uint8_t *)data->ps->buf;
  for(int y = 0; y < data->height; y++)
  {
    for(int x = 0; x < data->width; x++, in_ptr += 4, out_ptr += 3)
      memcpy(out_ptr, in_ptr, 3);
  }

  return 0;
}

static void
_print_button_clicked (GtkWidget *widget, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_print_settings_t *ps = (dt_lib_print_settings_t *)self->data;

  const int imgid = dt_view_filmstrip_get_activated_imgid(darktable.view_manager);

  if (imgid == -1)
  {
    dt_control_log(_("cannot print until a picture is selected"));
    dt_control_queue_redraw();
    return;
  }
  if (strlen(ps->prt.printer.name) == 0 || ps->prt.printer.resolution == 0)
  {
    dt_control_log(_("cannot print until a printer is selected"));
    dt_control_queue_redraw();
    return;
  }
  if (ps->prt.paper.width == 0 || ps->prt.paper.height == 0)
  {
    dt_control_log(_("cannot print until a paper is selected"));
    dt_control_queue_redraw();
    return;
  }

  // compute print-area (in inches)
  double width, height;
  int32_t px=0, py=0, pwidth=0, pheight=0;
  int32_t ax=0, ay=0, awidth=0, aheight=0;
  int32_t ix=0, iy=0, iwidth=0, iheight=0;

  // user margin are already in the proper orientation landscape/portrait
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

  dt_get_print_layout (imgid, &ps->prt, width_pix, height_pix,
                       &px, &py, &pwidth, &pheight,
                       &ax, &ay, &awidth, &aheight,
                       &ix, &iy, &iwidth, &iheight);

  const double pa_width  = (width  - margin_w) / 25.4;
  const double pa_height = (height - margin_h) / 25.4;

  fprintf(stderr, "[print] area for image %u : %3.2fin x %3.2fin\n", imgid, pa_width, pa_height);

  const int margin_top    = iy;
  const int margin_left   = ix;
  const int margin_right  = pwidth - iwidth - ix;
  const int margin_bottom = pheight - iheight - iy;

  dt_conf_set_int("plugins/imageio/format/print/margin-top", margin_top);
  if (ps->prt.page.landscape)
  {
    dt_conf_set_int("plugins/imageio/format/print/margin-right", margin_right);
    dt_conf_set_int("plugins/imageio/format/print/margin-left", 0);
  }
  else
  {
    dt_conf_set_int("plugins/imageio/format/print/margin-left", margin_left);
    dt_conf_set_int("plugins/imageio/format/print/margin-right", 0);
  }
  dt_conf_set_int("plugins/imageio/format/print/margin-bottom", 0);

  fprintf(stderr, "[print] margins top %d ; bottom %d ; left %d ; right %d\n",
          margin_top, margin_bottom, margin_left, margin_right);

  // compute the needed size for picture for the given printer resolution

  int max_width  = (pa_width  * ps->prt.printer.resolution);
  int max_height = (pa_height * ps->prt.printer.resolution);

  // make sure we are a multiple of four

  max_width -= max_width % 4;
  max_height -= max_height % 4;

  fprintf(stderr, "[print] max image size %d x %d (at resolution %d)\n", max_width, max_height, ps->prt.printer.resolution);

  dt_imageio_module_format_t buf;
  buf.mime = mime;
  buf.levels = levels;
  buf.bpp = bpp;
  buf.write_image = write_image;

  dt_print_format_t dat;
  dat.max_width = max_width;
  dat.max_height = max_height;
  dat.style[0] = '\0';
  dat.style_append = dt_conf_get_bool("plugins/print/print/style_append");
  dat.ps = ps;

  char* style = dt_conf_get_string("plugins/print/print/style");
  if (style)
  {
    g_strlcpy(dat.style, style, sizeof(dat.style));
    g_free(style);
  }

  // the flags are: ignore exif, display byteorder, high quality, thumbnail
  const int high_quality = 1;
  dt_imageio_export_with_flags
    (imgid, "unused", &buf, (dt_imageio_module_data_t *)&dat, 1, 0, high_quality, 0, NULL, FALSE, 0, 0);

  float border;
  dt_pdf_parse_length("0 mm", &border);

  const float page_width  = dt_pdf_mm_to_point(width);
  const float page_height = dt_pdf_mm_to_point(height);

  char filename[PATH_MAX] = { 0 };
  snprintf(filename, sizeof(filename), "/tmp/pf_%d.pdf", imgid);

  int icc_id = 0;
  const gchar *printer_profile = dt_conf_get_string("plugins/print/printer/iccprofile");

  dt_pdf_t *pdf = dt_pdf_start(filename, page_width, page_height, ps->prt.printer.resolution, DT_PDF_STREAM_ENCODER_FLATE);

  if (*printer_profile)
    icc_id = dt_pdf_add_icc(pdf, printer_profile);

  dt_pdf_image_t *pdf_image = dt_pdf_add_image(pdf, (uint8_t *)dat.ps->buf, dat.width, dat.height, 8, icc_id, border);

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

static void _set_printer(dt_lib_module_t *self, const char *printer_name)
{
  dt_lib_print_settings_t *ps = (dt_lib_print_settings_t *)self->data;
  dt_printer_info_t *printer = dt_get_printer_info (printer_name);

  if (!printer) return;

  memcpy(&ps->prt.printer, printer, sizeof(dt_printer_info_t));

  // if there is 0 hardware margins, set de user marging to 20mm

  if (ps->prt.printer.hw_margin_top == 0)
  {
    ps->prt.page.margin_top = 15;
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(ps->b_top), ps->prt.page.margin_top);
  }
  if (ps->prt.printer.hw_margin_bottom == 0)
  {
    ps->prt.page.margin_bottom = 15;
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(ps->b_bottom), ps->prt.page.margin_bottom);
  }
  if (ps->prt.printer.hw_margin_left == 0)
  {
    ps->prt.page.margin_left = 15;
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(ps->b_left), ps->prt.page.margin_left);
  }
  if (ps->prt.printer.hw_margin_right == 0)
  {
    ps->prt.page.margin_right = 15;
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(ps->b_right), ps->prt.page.margin_right);
  }

  dt_conf_set_string("plugins/print/print/printer", printer_name);

  const char *default_paper = dt_conf_get_string("plugins/print/print/paper");

  // next add corresponding papers

  // first clear current list

  dt_bauhaus_combobox_clear(ps->papers);

  // then add papers for the given printer

  ps->paper_list = dt_get_papers (printer_name);
  GList *papers = ps->paper_list;
  int np = 0;
  gboolean ispaperset = FALSE;

  while (papers)
  {
    dt_paper_info_t *p = (dt_paper_info_t *)papers->data;
    dt_bauhaus_combobox_add(ps->papers, p->common_name);

    if (ispaperset == FALSE && (!strcmp(default_paper, p->common_name) || default_paper[0] == '\0'))
    {
      dt_bauhaus_combobox_set(ps->papers, np);
      ispaperset = TRUE;
    }

    np++;
    papers = g_list_next (papers);
  }

  dt_paper_info_t *paper = dt_get_paper(ps->paper_list, default_paper);

  if (paper)
    memcpy(&ps->prt.paper, paper, sizeof(dt_paper_info_t));

  dt_view_print_settings(darktable.view_manager, &ps->prt);
}

static void
_printer_changed (GtkComboBoxText *combo, dt_lib_module_t *self)
{
  const gchar *printer_name = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(combo));

  if (printer_name)
    _set_printer (self, printer_name);
}

static void
_paper_changed (GtkWidget *combo, dt_lib_module_t *self)
{
  dt_lib_print_settings_t *ps = (dt_lib_print_settings_t *)self->data;

  const gchar *paper_name = dt_bauhaus_combobox_get_text(combo);

  if (!paper_name) return;

  dt_paper_info_t *paper = dt_get_paper(ps->paper_list, paper_name);

  if (paper)
    memcpy(&ps->prt.paper, paper, sizeof(dt_paper_info_t));

  dt_conf_set_string("plugins/print/print/paper", paper_name);
  dt_view_print_settings(darktable.view_manager, &ps->prt);
}

static void
_update_slider (dt_lib_print_settings_t *ps)
{
  dt_view_print_settings(darktable.view_manager, &ps->prt);
}

static void
_top_border_callback (GtkWidget *spin, gpointer user_data)
{
  const dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_print_settings_t *ps = (dt_lib_print_settings_t *)self->data;
  const double value = gtk_spin_button_get_value(GTK_SPIN_BUTTON(spin));

  ps->prt.page.margin_top = value;

  if (ps->lock_activated == TRUE)
  {
    ps->prt.page.margin_bottom = value;
    ps->prt.page.margin_left = value;
    ps->prt.page.margin_right = value;

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

  ps->prt.page.margin_bottom = value;
  _update_slider (ps);
}

static void
_left_border_callback (GtkWidget *spin, gpointer user_data)
{
  const dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_print_settings_t *ps = (dt_lib_print_settings_t *)self->data;
  const double value = gtk_spin_button_get_value(GTK_SPIN_BUTTON(spin));

  ps->prt.page.margin_left = value;
  _update_slider (ps);
}

static void
_right_border_callback (GtkWidget *spin, gpointer user_data)
{
  const dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_print_settings_t *ps = (dt_lib_print_settings_t *)self->data;
  const double value = gtk_spin_button_get_value(GTK_SPIN_BUTTON(spin));

  ps->prt.page.margin_right = value;
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
}

static void
_alignment_callback(GtkWidget *tb, gpointer user_data)
{
  int index=-1;
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
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
_orientation_callback (GtkWidget *radio, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_print_settings_t *ps = (dt_lib_print_settings_t *)self->data;

  ps->prt.page.landscape = !gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON(ps->portrait));

  _update_slider (ps);
}

static void
_style_callback(GtkWidget *widget, dt_lib_module_t *self)
{
  dt_lib_print_settings_t *ps = (dt_lib_print_settings_t *)self->data;

  if(dt_bauhaus_combobox_get(ps->style) == 0)
    dt_conf_set_string("plugins/print/print/style", "");
  else
  {
    const gchar *style = dt_bauhaus_combobox_get_text(ps->style);
    dt_conf_set_string("plugins/print/print/style", style);
  }
}

static void
_style_mode_changed(GtkWidget *widget, dt_lib_module_t *self)
{
  dt_lib_print_settings_t *ps = (dt_lib_print_settings_t *)self->data;

  if(dt_bauhaus_combobox_get(ps->style_mode) == 0)
    dt_conf_set_bool("plugins/print/print/style_append", FALSE);
  else
    dt_conf_set_bool("plugins/print/print/style_append", TRUE);
}

static void
_profile_changed(GtkWidget *widget, dt_lib_module_t *self)
{
  dt_lib_print_settings_t *ps = (dt_lib_print_settings_t *)self->data;
  int pos = dt_bauhaus_combobox_get(widget);
  GList *prof = ps->profiles;
  while(prof)
  {
    // could use g_list_nth. this seems safer?
    dt_lib_export_profile_t *pp = (dt_lib_export_profile_t *)prof->data;
    if(pp->pos == pos)
    {
      dt_conf_set_string("plugins/lighttable/export/iccprofile", pp->filename);
      dt_conf_set_string("plugins/print/print/iccprofile", pp->filename);
      return;
    }
    prof = g_list_next(prof);
  }
  dt_conf_set_string("plugins/lighttable/export/iccprofile", "image");
  dt_conf_set_string("plugins/print/print/iccprofile", "image");
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
      return;
    }
    prof = g_list_next(prof);
  }
  dt_conf_set_string("plugins/print/printer/iccprofile", "");
}

static void
_printer_intent_callback (GtkWidget *widget, dt_lib_module_t *self)
{
  int pos = dt_bauhaus_combobox_get(widget);
  dt_conf_set_int("plugins/print/printer/iccintent", pos);
}

static void
_intent_callback (GtkWidget *widget, dt_lib_module_t *self)
{
  int pos = dt_bauhaus_combobox_get(widget);
  // record the intent that will override the out rendering module on export
  dt_conf_set_int("plugins/lighttable/export/iccintent", pos - 1);
  dt_conf_set_int("plugins/print/print/iccintent", pos - 1);
}

static void _print_settings_filmstrip_activate_callback(gpointer instance,gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_print_settings_t *ps = (dt_lib_print_settings_t *)self->data;

  if (ps->prt.page.landscape)
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON(ps->landscape), TRUE);
  else
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON(ps->portrait), TRUE);
}

void
gui_init (dt_lib_module_t *self)
{
  dt_lib_print_settings_t *d = (dt_lib_print_settings_t*)malloc(sizeof(dt_lib_print_settings_t));
  self->data = d;
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);

  GtkWidget *label;
  GtkComboBox *comb;
  char tooltip[1024];

  dt_init_print_info(&d->prt);
  d->prt.page.landscape = TRUE;
  dt_view_print_settings(darktable.view_manager, &d->prt);

  dt_control_signal_connect(darktable.signals,
                            DT_SIGNAL_VIEWMANAGER_FILMSTRIP_ACTIVATE,
                            G_CALLBACK(_print_settings_filmstrip_activate_callback),
                            self);

  //  create the spin-button now as values could be set when the printer has no hardware margin

  d->b_top    = gtk_spin_button_new_with_range(0, 10000, 1);
  d->b_left   = gtk_spin_button_new_with_range(0, 10000, 1);
  d->b_right  = gtk_spin_button_new_with_range(0, 10000, 1);
  d->b_bottom = gtk_spin_button_new_with_range(0, 10000, 1);

  ////////////////////////// PRINTER SETTINGS

  // create papers combo as filled when adding printers
  d->papers = dt_bauhaus_combobox_new(NULL);

  GList *printers = dt_get_printers();
  int np=0;
  int printer_index = 0;
  char printer_name[128] = { 0 };

  label = dt_ui_section_label_new(_("printer"));
  gtk_box_pack_start(GTK_BOX(self->widget), label, TRUE, TRUE, 0);

  comb = GTK_COMBO_BOX(gtk_combo_box_text_new());

  const char *default_printer = dt_conf_get_string("plugins/print/print/printer");

  // we need the printer details, so request them here
  while (printers)
  {
    dt_printer_info_t *printer = (dt_printer_info_t *)printers->data;
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(comb), printer->name);
    if (!strcmp(default_printer, printer->name) || default_printer[0]=='\0')
    {
      // record the printer to set as we want to set this when the paper widget is realized
      printer_index = np;
      strncpy(printer_name,printer->name,sizeof(printer_name));
    }
    printers = g_list_next (printers);
    np++;
  }

  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(comb), TRUE, TRUE, 0);
  d->printers = comb;
  g_signal_connect(G_OBJECT(d->printers), "changed", G_CALLBACK(_printer_changed), self);
  g_list_free (printers);

  ////////////////////////// PAGE SETTINGS

  label = dt_ui_section_label_new(_("page"));
  gtk_box_pack_start(GTK_BOX(self->widget), label, TRUE, TRUE, 0);

  //// papers

  dt_bauhaus_widget_set_label(d->papers, NULL, _("paper"));

  g_signal_connect(G_OBJECT(d->papers), "value-changed", G_CALLBACK(_paper_changed), self);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(d->papers), TRUE, TRUE, 0);

  // now set recorded default/active printer

  gtk_combo_box_set_active(GTK_COMBO_BOX(comb), printer_index);
  _set_printer(self, printer_name);

  //// portrait / landscape

  d->landscape = gtk_radio_button_new_with_label(NULL, _("landscape"));
  d->portrait = gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON (d->landscape), _("portrait"));
  GtkBox *hbox2 = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_PIXEL_APPLY_DPI(5)));
  gtk_box_pack_start(GTK_BOX(hbox2), GTK_WIDGET(d->landscape), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(hbox2), GTK_WIDGET(d->portrait), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hbox2), TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(d->landscape), "toggled", G_CALLBACK(_orientation_callback), self);
  g_signal_connect(G_OBJECT(d->portrait), "toggled", G_CALLBACK(_orientation_callback), self);

  //// borders

  GtkGrid *bds = GTK_GRID(gtk_grid_new());
  gtk_grid_set_row_spacing(bds, DT_PIXEL_APPLY_DPI(3));
  gtk_grid_set_column_spacing(bds, DT_PIXEL_APPLY_DPI(3));

  d->lock_activated = FALSE;

  //d->b_top  = gtk_spin_button_new_with_range(0, 10000, 1);
  g_object_set(G_OBJECT(d->b_top), "tooltip-text", _("top margin (in mm)"), (char *)NULL);
  gtk_grid_attach(bds, GTK_WIDGET(d->b_top), 1, 0, 1, 1);

  //d->b_left  = gtk_spin_button_new_with_range(0, 10000, 1);
  g_object_set(bds, "tooltip-text", _("left margin (in mm)"), (char *)NULL);
  gtk_grid_attach(bds, GTK_WIDGET(d->b_left), 0, 1, 1, 1);

  d->lock_button = GTK_TOGGLE_BUTTON(gtk_toggle_button_new_with_label(_("lock")));
  g_object_set(G_OBJECT(d->lock_button), "tooltip-text", _("change all borders uniformly"), (char *)NULL);
  gtk_grid_attach(bds, GTK_WIDGET(d->lock_button), 1, 1, 1, 1);

  //d->b_right  = gtk_spin_button_new_with_range(0, 10000, 1);
  g_object_set(G_OBJECT(d->b_right), "tooltip-text", _("right margin (in mm)"), (char *)NULL);
  gtk_grid_attach(bds, GTK_WIDGET(d->b_right), 2, 1, 1, 1);

  //d->b_bottom  = gtk_spin_button_new_with_range(0, 10000, 1);
  g_object_set(G_OBJECT(d->b_bottom), "tooltip-text", _("bottom margin (in mm)"), (char *)NULL);
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

  //// alignments

  // Create the 3x3 gtk table toggle button table...
  GtkGrid *bat = GTK_GRID(gtk_grid_new());
  for(int i=0; i<9; i++)
  {
    d->dtba[i] = DTGTK_TOGGLEBUTTON (dtgtk_togglebutton_new (dtgtk_cairo_paint_alignment,CPF_STYLE_FLAT|(CPF_SPECIAL_FLAG<<(i+1))));
    gtk_widget_set_size_request (GTK_WIDGET (d->dtba[i]), DT_PIXEL_APPLY_DPI(16), DT_PIXEL_APPLY_DPI(16));
    gtk_grid_attach (GTK_GRID (bat), GTK_WIDGET (d->dtba[i]), (i%3), i/3, 1, 1);
    g_signal_connect (G_OBJECT (d->dtba[i]), "toggled",G_CALLBACK (_alignment_callback), self);
  }
  GtkWidget *hbox22 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL,0);
  GtkWidget *label4 = gtk_label_new(_("alignment"));
  gtk_box_pack_start(GTK_BOX(hbox22),GTK_WIDGET(label4),TRUE,TRUE,0);
  gtk_box_pack_start(GTK_BOX(hbox22), GTK_WIDGET(bat), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hbox22), TRUE, TRUE, 0);

  ////////////////////////// PRINT SETTINGS

  label = dt_ui_section_label_new(_("print settings"));
  gtk_box_pack_start(GTK_BOX(self->widget), label, TRUE, TRUE, 0);

  //  Create list of profiles

  d->profiles = NULL;

  dt_lib_export_profile_t *prof = (dt_lib_export_profile_t *)g_malloc0(sizeof(dt_lib_export_profile_t));
  g_strlcpy(prof->filename, "sRGB", sizeof(prof->filename));
  dt_utf8_strlcpy(prof->name, _("sRGB (web-safe)"), sizeof(prof->name));
  int pos;
  prof->pos = 1;
  d->profiles = g_list_append(d->profiles, prof);

  prof = (dt_lib_export_profile_t *)g_malloc0(sizeof(dt_lib_export_profile_t));
  g_strlcpy(prof->filename, "adobergb", sizeof(prof->filename));
  dt_utf8_strlcpy(prof->name, _("Adobe RGB (compatible)"), sizeof(prof->name));
  pos = prof->pos = 2;
  d->profiles = g_list_append(d->profiles, prof);

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
        d->profiles = g_list_append(d->profiles, prof);
      }
    }
    g_dir_close(dir);
  }

  //  Add export profile combo

  d->profile = dt_bauhaus_combobox_new(NULL);
  dt_bauhaus_widget_set_label(d->profile, NULL, _("export profile"));

  GList *l = d->profiles;

  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(d->profile), TRUE, TRUE, 0);
  dt_bauhaus_combobox_add(d->profile, _("image settings"));

  const gchar *iccprofile = dt_conf_get_string("plugins/print/print/iccprofile");
  int combo_idx = -1, n=0;

  while(l)
  {
    dt_lib_export_profile_t *prof = (dt_lib_export_profile_t *)l->data;
    dt_bauhaus_combobox_add(d->profile, prof->name);
    n++;
    if (strcmp(prof->filename, iccprofile)==0)
      combo_idx=n;
    l = g_list_next(l);
  }

  if (combo_idx == -1)
  {
    dt_conf_set_string("plugins/print/print/iccprofile", "image");
    combo_idx=0;
  }

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
  const gchar *current_style = dt_conf_get_string("plugins/print/print/style");
  combo_idx = -1; n=0;

  while (styles)
  {
    dt_style_t *style=(dt_style_t *)styles->data;
    dt_bauhaus_combobox_add(d->style, style->name);
    n++;
    if (strcmp(style->name,current_style)==0)
      combo_idx=n;
    styles=g_list_next(styles);
  }
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(d->style), TRUE, TRUE, 0);
  g_object_set(G_OBJECT(d->style), "tooltip-text", _("temporary style to append while printing"), (char *)NULL);

  // style not found, maybe a style has been removed? revert to none
  if (combo_idx == -1)
  {
    dt_conf_set_string("plugins/print/print/style", "");
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

  if (dt_conf_get_bool("plugins/print/print/style_append"))
    dt_bauhaus_combobox_set(d->style_mode, 1);
  else
    dt_bauhaus_combobox_set(d->style_mode, 0);

  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(d->style_mode), TRUE, TRUE, 0);
  g_object_set(G_OBJECT(d->style_mode), "tooltip-text", _("whether the style is appended to the history or replacing the history"),
               (char *)NULL);

  g_signal_connect(G_OBJECT(d->style_mode), "value-changed", G_CALLBACK(_style_mode_changed), (gpointer)self);

  //  Add printer profile combo

  d->pprofile = dt_bauhaus_combobox_new(NULL);
  dt_bauhaus_widget_set_label(d->pprofile, NULL, _("printer profile"));

  l = d->profiles;
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(d->pprofile), TRUE, TRUE, 0);
  const gchar *printer_profile = dt_conf_get_string("plugins/print/printer/iccprofile");
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
        combo_idx=n;
    }
    l = g_list_next(l);
  }

  // profile not found, maybe a profile has been removed? revert to none
  if (combo_idx == -1)
  {
    dt_conf_set_string("plugins/print/printer/iccprofile", "");
    combo_idx=0;
  }
  dt_bauhaus_combobox_set(d->pprofile, combo_idx);

  snprintf(tooltip, sizeof(tooltip), _("output ICC profiles in %s/color/out or %s/color/out"), confdir, datadir);
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
  dt_bauhaus_combobox_set(d->pintent, dt_conf_get_int("plugins/print/printer/iccintent"));

  g_signal_connect (G_OBJECT (d->pintent), "value-changed", G_CALLBACK (_printer_intent_callback), (gpointer)self);

  // Print button

  GtkButton *button = GTK_BUTTON(gtk_button_new_with_label(_("print")));
  d->print_button = button;
  g_object_set(G_OBJECT(button), "tooltip-text", _("print with current settings (ctrl-p)"), (char *)NULL);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(button), TRUE, TRUE, 0);

  g_signal_connect (G_OBJECT (button), "clicked",
                    G_CALLBACK (_print_button_clicked),
                    (gpointer)self);
}

static gboolean _bauhaus_combo_box_set_active_text(GtkWidget *cb, const gchar *text)
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

static gboolean _combo_box_set_active_text(GtkComboBox *cb, const gchar *text)
 {
   g_assert(text != NULL);
   g_assert(cb != NULL);
  GtkTreeModel *model = gtk_combo_box_get_model(cb);
  GtkTreeIter iter;
  if(gtk_tree_model_get_iter_first(model, &iter))
   {
    int k = -1;
    do
     {
      k++;
      GValue value = {
        0,
      };
      gtk_tree_model_get_value(model, &iter, 0, &value);
      gchar *v = NULL;
      if(G_VALUE_HOLDS_STRING(&value) && (v = (gchar *)g_value_get_string(&value)) != NULL)
      {
        if(strcmp(v, text) == 0)
        {
          gtk_combo_box_set_active(cb, k);
          return TRUE;
        }
      }
    } while(gtk_tree_model_iter_next(model, &iter));
   }
   return FALSE;
 }

void init_presets(dt_lib_module_t *self)
{
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

  const char *profile = buf;
  if (!profile) return 1;
  const int32_t profile_len = strlen(profile) + 1;
  buf += profile_len;

  const int32_t intent = *(int32_t *)buf;
  buf += sizeof(int32_t);

  const char *pprofile = buf;
  if (!pprofile) return 1;
  const int32_t pprofile_len = strlen(pprofile) + 1;
  buf += pprofile_len;

  const int32_t pintent = *(int32_t *)buf;
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
  if(size != printer_len + paper_len + profile_len + pprofile_len + style_len + 5 * sizeof(int32_t) + 4 * sizeof(double))
    return 1;

  // set the GUI with corresponding values
  if (printer[0] != '\0')
    _combo_box_set_active_text(ps->printers, printer);

  if (paper[0] != '\0')
    _bauhaus_combo_box_set_active_text(ps->papers, paper);

  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON(ps->portrait), !landscape);

  if (profile[0] != '\0')
    _bauhaus_combo_box_set_active_text(ps->profile, profile);
  dt_bauhaus_combobox_set (ps->intent, intent);

  if (pprofile[0] != '\0')
    _bauhaus_combo_box_set_active_text(ps->pprofile, pprofile);
  dt_bauhaus_combobox_set (ps->pintent, pintent);

  if (style[0] != '\0')
    _bauhaus_combo_box_set_active_text(ps->style, style);
  dt_bauhaus_combobox_set (ps->style_mode, style_mode);

  gtk_spin_button_set_value (GTK_SPIN_BUTTON(ps->b_top), b_top);
  gtk_spin_button_set_value (GTK_SPIN_BUTTON(ps->b_bottom), b_bottom);
  gtk_spin_button_set_value (GTK_SPIN_BUTTON(ps->b_left), b_left);
  gtk_spin_button_set_value (GTK_SPIN_BUTTON(ps->b_right), b_right);

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ps->dtba[alignment]),TRUE);

  return 0;
}

void *get_params(dt_lib_module_t *self, int *size)
{
  const dt_lib_print_settings_t *ps = (dt_lib_print_settings_t *)self->data;

  // get the data
  const char *printer = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(ps->printers));
  const char *paper = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(ps->papers));
  const char *profile = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(ps->profile));
  const int32_t intent =  gtk_combo_box_get_active(GTK_COMBO_BOX(ps->intent));
  const char *style = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(ps->style));
  const int32_t style_mode = gtk_combo_box_get_active(GTK_COMBO_BOX(ps->style_mode));
  const char *pprofile = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(ps->pprofile));
  const int32_t pintent =  gtk_combo_box_get_active(GTK_COMBO_BOX(ps->pintent));
  const int32_t landscape = !gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON(ps->portrait));
  const double b_top = gtk_spin_button_get_value(GTK_SPIN_BUTTON(ps->b_top));
  const double b_bottom = gtk_spin_button_get_value(GTK_SPIN_BUTTON(ps->b_bottom));
  const double b_left = gtk_spin_button_get_value(GTK_SPIN_BUTTON(ps->b_left));
  const double b_right = gtk_spin_button_get_value(GTK_SPIN_BUTTON(ps->b_right));
  const int32_t alignment = ps->prt.page.alignment;

  // compute the size of individual items, always get the \0 for strings
  const int32_t printer_len = strlen (printer) + 1;
  const int32_t paper_len = strlen (paper) + 1;
  const int32_t profile_len = strlen (profile) + 1;
  const int32_t pprofile_len = strlen (pprofile) + 1;
  const int32_t style_len = strlen (style) + 1;

  // compute the size of all parameters
  *size = printer_len + paper_len + profile_len + pprofile_len + style_len + 5 * sizeof(int32_t) + 4 * sizeof(double);

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

  g_list_free_full(ps->profiles, g_free);
  free(self->data);
  self->data = NULL;
}

void
gui_reset (dt_lib_module_t *self)
{
  dt_lib_print_settings_t *ps = (dt_lib_print_settings_t *)self->data;

  gtk_spin_button_set_value(GTK_SPIN_BUTTON(ps->b_top), 15);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(ps->b_bottom), 15);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(ps->b_left), 15);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(ps->b_right), 15);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ps->dtba[center]),TRUE);
  ps->prt.page.alignment = center;
  dt_bauhaus_combobox_set(ps->profile, 0);
  dt_bauhaus_combobox_set(ps->pprofile, 0);
  dt_bauhaus_combobox_set(ps->pintent, dt_conf_get_int("plugins/print/print/iccintent") + 1);
  dt_bauhaus_combobox_set(ps->style, 0);
  dt_bauhaus_combobox_set(ps->intent, 0);

  // reset page orientation to fit the picture

  ps->prt.page.landscape = TRUE;

  const int imgid = dt_view_filmstrip_get_activated_imgid(darktable.view_manager);
  dt_mipmap_buffer_t buf;
  dt_mipmap_cache_get(darktable.mipmap_cache, &buf, imgid, DT_MIPMAP_3, DT_MIPMAP_BEST_EFFORT, 'r');

  if (buf.width > buf.height)
    ps->prt.page.landscape = TRUE;
  else
    ps->prt.page.landscape = FALSE;

  dt_mipmap_cache_release(darktable.mipmap_cache, &buf);

  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON(ps->portrait), !ps->prt.page.landscape);
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
