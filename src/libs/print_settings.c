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
#include "dtgtk/resetlabel.h"
#include "libs/lib.h"
#include "gui/gtk.h"
#include "bauhaus/bauhaus.h"

DT_MODULE(1)

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
  GtkComboBox *profile, *pprofile, *intent, *style, *style_mode, *pintent, *printers, *papers;
  GtkWidget *landscape, *portrait;
  GList *profiles;
  GtkButton *print_button;
  GtkToggleButton *lock_button;
  GtkWidget *b_top, *b_bottom, *b_left, *b_right;
  GtkDarktableToggleButton *dtba[9];	                                   // Alignment buttons
  GList *paper_list;
  gboolean lock_activated;
  dt_print_info_t prt;
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

  int margin_top = iy;
  int margin_left = ix;
  int margin_right = ix + iwidth;
  int margin_bottom = iy + iheight;

  if (ps->prt.page.landscape)
  {
    margin_top = iy;
    margin_left = ix;
    margin_right = pwidth - iwidth - ix;
    margin_bottom = pheight - iheight - iy;
  }
  else
  {
    margin_top = iy;
    margin_left = ix;
    margin_right = pwidth - iwidth - ix;
    margin_bottom = pheight - iheight - iy;
  }

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

  // export as TIFF 16bit (if a profile is given, 8bit otherwise) on disk
  // CUPS can only print directly 8bit pictures. If we print directly we then export as 8bit TIFF
  // otherwise we use 16bit format here, the file will be converted to 8bit later when applying the
  // printer profile.

  const char *storage_name = "disk";
  const char *format_name = "prt";

  dt_imageio_module_storage_t *mstorage = dt_imageio_get_storage_by_name(storage_name);
  dt_imageio_module_format_t *mformat = dt_imageio_get_format_by_name(format_name);
  int format_index = dt_imageio_get_index_of_format(mformat);
  int storage_index = dt_imageio_get_index_of_storage(mstorage);

  // get printer profile filename

  const gchar *printer_profile = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(ps->pprofile));

  GList *prof = ps->profiles;
  char printer_profile_filename[PATH_MAX] = {0};

  while(prof)
  {
    dt_lib_export_profile_t *p = (dt_lib_export_profile_t *)prof->data;
    if (strcmp(p->name, printer_profile)==0)
    {
      g_strlcpy(printer_profile_filename, p->filename, sizeof(printer_profile_filename));
      break;
    }
    prof = g_list_next(prof);
  }

  // the exported filename, on /tmp we do not want to mess with user's home directory

  char filename[PATH_MAX] = { 0 };

  snprintf(filename, sizeof(filename), "/tmp/pf_%d", imgid);

  // set parameters for the disk storage module
  // make sure we overwrite the file as we really want to keep the filename as created here. This is the file
  // that will be sent to CUPS, we do not want to have any _nn suffix added.

  dt_conf_set_string("plugins/imageio/storage/print/file_directory", filename);
  dt_conf_set_bool("plugins/imageio/storage/print/overwrite", TRUE);

  // set parameters for the tiff format module

  int bpp;
  if (*printer_profile_filename)
    bpp = 16;
  else
    bpp = 8;

  dt_conf_set_int("plugins/imageio/format/print/bpp", bpp);
  dt_conf_set_int("plugins/imageio/format/print/compress", 0);

  // make sure the export resolution wrote into the TIFF file corresponds to the printer resolution.
  // this is needed to ensure that the image will fit exactly on the page.
  dt_conf_set_int("plugins/imageio/format/print/resolution", ps->prt.printer.resolution);

  char style[128] = {0};
  char* tmp = dt_conf_get_string("plugins/print/print/style");
  if (tmp)
  {
    g_strlcpy(style, tmp, sizeof(style));
    g_free(tmp);
  }

  const gboolean style_append = dt_conf_get_bool("plugins/print/print/style_append");

  GList *list = NULL;

  list = g_list_append (list, GINT_TO_POINTER(imgid));

  //  record printer intent and profile

  ps->prt.printer.intent = gtk_combo_box_get_active(GTK_COMBO_BOX(ps->pintent));
  g_strlcpy(ps->prt.printer.profile, printer_profile_filename, sizeof(ps->prt.printer.profile));

  dt_control_print(list, max_width, max_height, format_index, storage_index, style, style_append, filename, &ps->prt);
}

static void _set_printer(dt_lib_module_t *self, const char *printer_name)
{
  dt_lib_print_settings_t *ps = (dt_lib_print_settings_t *)self->data;
  dt_printer_info_t *printer = dt_get_printer_info (printer_name);

  if (!printer) return;

  memcpy(&ps->prt.printer, printer, sizeof(dt_printer_info_t));

  // if there is 0 hardware margins, set the user marging to 15mm

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

  GtkTreeModel *store = gtk_combo_box_get_model(ps->papers);

  if (store)
    gtk_list_store_clear(GTK_LIST_STORE (store));

  // then add papers for the given printer

  ps->paper_list = dt_get_papers (printer_name);
  GList *papers = ps->paper_list;
  int np = 0;
  gboolean ispaperset = FALSE;

  while (papers)
  {
    dt_paper_info_t *p = (dt_paper_info_t *)papers->data;
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(ps->papers), p->common_name);

    if (ispaperset == FALSE && (!strcmp(default_paper, p->common_name) || default_paper[0] == '\0'))
    {
      gtk_combo_box_set_active(GTK_COMBO_BOX(ps->papers), np);
      ispaperset = TRUE;
    }

    np++;
    papers = g_list_next (papers);
  }

  dt_paper_info_t *paper = dt_get_paper(ps->paper_list, default_paper);

  if (paper)
    memcpy(&ps->prt.paper, paper, sizeof(dt_paper_info_t));

  dt_ellipsize_combo(ps->papers);
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
_paper_changed (GtkComboBoxText *combo, dt_lib_module_t *self)
{
  dt_lib_print_settings_t *ps = (dt_lib_print_settings_t *)self->data;

  const gchar *paper_name = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(combo));

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
_style_callback(GtkComboBox *widget, dt_lib_module_t *self)
{
  dt_lib_print_settings_t *ps = (dt_lib_print_settings_t *)self->data;

  if(gtk_combo_box_get_active(ps->style) == 0)
    dt_conf_set_string("plugins/print/print/style", "");
  else
  {
    gchar *style = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(ps->style));
    dt_conf_set_string("plugins/print/print/style", style);
  }
}

static void
_style_mode_changed(GtkComboBox *widget, dt_lib_module_t *self)
{
  dt_lib_print_settings_t *ps = (dt_lib_print_settings_t *)self->data;

  if(gtk_combo_box_get_active(ps->style_mode) == 0)
    dt_conf_set_bool("plugins/print/print/style_append", FALSE);
  else
    dt_conf_set_bool("plugins/print/print/style_append", TRUE);
}

static void
_profile_changed(GtkComboBox *widget, dt_lib_module_t *self)
{
  dt_lib_print_settings_t *ps = (dt_lib_print_settings_t *)self->data;
  int pos = gtk_combo_box_get_active(widget);
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
_printer_profile_changed(GtkComboBox *widget, dt_lib_module_t *self)
{
  dt_lib_print_settings_t *ps = (dt_lib_print_settings_t *)self->data;
  const gchar *printer_profile = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(widget));
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
_printer_intent_callback (GtkComboBox *widget, dt_lib_module_t *self)
{
  int pos = gtk_combo_box_get_active(widget);
  dt_conf_set_int("plugins/print/printer/iccintent", pos);
}

static void
_intent_callback (GtkComboBox *widget, dt_lib_module_t *self)
{
  int pos = gtk_combo_box_get_active(widget);
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
  self->widget = gtk_table_new(18, 2, FALSE);
  GtkWidget *label;
  GtkComboBox *comb;
  int tpos = 1; // the ligne on the table
  char tooltip[1024];

  gtk_table_set_row_spacings(GTK_TABLE(self->widget), 5);

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
  d->papers = GTK_COMBO_BOX(gtk_combo_box_text_new());

  GList *printers = dt_get_printers();
  int np=0;
  int printer_index = 0;
  char printer_name[128] = { 0 };

  label = gtk_label_new(_("printer"));
  gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
  gtk_table_attach(GTK_TABLE(self->widget), label, 0, 1, tpos, tpos+1, GTK_SHRINK|GTK_EXPAND|GTK_FILL, 0, 0, 0);

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
  gtk_table_attach(GTK_TABLE(self->widget), GTK_WIDGET(comb), 1, 2, tpos, tpos+1, GTK_SHRINK|GTK_EXPAND|GTK_FILL, 0, 0, 0);
  d->printers = comb;
  g_signal_connect(G_OBJECT(d->printers), "changed", G_CALLBACK(_printer_changed), self);
  g_list_free (printers);

  ////////////////////////// PAGE SETTINGS

  label = dtgtk_label_new(_("page"), DARKTABLE_LABEL_TAB | DARKTABLE_LABEL_ALIGN_RIGHT);
  gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
  tpos++;
  gtk_table_attach(GTK_TABLE(self->widget), label, 0, 2, tpos, tpos+1, GTK_EXPAND|GTK_FILL, 0, 0, 0);

  //// papers

  label = gtk_label_new(_("paper"));
  gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
  tpos++;
  gtk_table_attach(GTK_TABLE(self->widget), label, 0, 1, tpos, tpos+1, GTK_SHRINK|GTK_EXPAND|GTK_FILL, 0, 0, 0);

  gtk_table_attach(GTK_TABLE(self->widget), GTK_WIDGET(d->papers), 1, 2, tpos, tpos+1, GTK_EXPAND|GTK_FILL, 0, 0, 0);
  g_signal_connect(G_OBJECT(d->papers), "changed", G_CALLBACK(_paper_changed), self);

  // now set recorded default/active printer

  gtk_combo_box_set_active(GTK_COMBO_BOX(comb), printer_index);
  _set_printer(self, printer_name);

  //// portrait / landscape

  d->landscape = gtk_radio_button_new_with_label(NULL, _("landscape"));
  d->portrait = gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON (d->landscape), _("portrait"));
  tpos++;
  gtk_table_attach(GTK_TABLE(self->widget), d->landscape, 0, 1, tpos, tpos+1, GTK_SHRINK|GTK_EXPAND|GTK_FILL, 0, 0, 0);
  gtk_table_attach(GTK_TABLE(self->widget), d->portrait, 1, 2, tpos, tpos+1, GTK_SHRINK|GTK_EXPAND|GTK_FILL, 0, 0, 0);
  g_signal_connect(G_OBJECT(d->landscape), "toggled", G_CALLBACK(_orientation_callback), self);
  g_signal_connect(G_OBJECT(d->portrait), "toggled", G_CALLBACK(_orientation_callback), self);

  //// borders

  GtkTable *bds = GTK_TABLE(gtk_table_new(3,3,TRUE));

  d->lock_activated = FALSE;

  //d->b_top  = gtk_spin_button_new_with_range(0, 10000, 1);
  g_object_set(G_OBJECT(d->b_top), "tooltip-text", _("top margin (in mm)"), (char *)NULL);
  gtk_table_attach(bds, GTK_WIDGET(d->b_top), 1, 2, 0, 1, GTK_EXPAND|GTK_FILL, 0, 0, 0);

  //d->b_left  = gtk_spin_button_new_with_range(0, 10000, 1);
  g_object_set(bds, "tooltip-text", _("left margin (in mm)"), (char *)NULL);
  gtk_table_attach(bds, GTK_WIDGET(d->b_left), 0, 1, 1, 2, GTK_EXPAND|GTK_FILL, 0, 0, 0);

  d->lock_button = GTK_TOGGLE_BUTTON(
    dtgtk_togglebutton_new_with_label(_("lock"), NULL, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER));
  g_object_set(G_OBJECT(d->lock_button), "tooltip-text", _("change all borders uniformly"), (char *)NULL);
  gtk_table_attach(bds, GTK_WIDGET(d->lock_button), 1, 2, 1, 2, GTK_EXPAND|GTK_FILL, 0, 0, 0);
  tpos++;

  //d->b_right  = gtk_spin_button_new_with_range(0, 10000, 1);
  g_object_set(G_OBJECT(d->b_right), "tooltip-text", _("right margin (in mm)"), (char *)NULL);
  gtk_table_attach(bds, GTK_WIDGET(d->b_right), 2, 3, 1, 2, GTK_EXPAND|GTK_FILL, 0, 0, 0);

  //d->b_bottom  = gtk_spin_button_new_with_range(0, 10000, 1);
  g_object_set(G_OBJECT(d->b_bottom), "tooltip-text", _("bottom margin (in mm)"), (char *)NULL);
  gtk_table_attach(bds, GTK_WIDGET(d->b_bottom), 1, 2, 2, 3, GTK_EXPAND, 0, 0, 0);

  tpos++;
  gtk_table_attach(GTK_TABLE(self->widget), GTK_WIDGET(bds), 0, 2, tpos, tpos+1, GTK_SHRINK|GTK_EXPAND|GTK_FILL, 0, 0, 0);

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
  GtkTable *bat = GTK_TABLE(gtk_table_new(3,3,TRUE));
  for(int i=0; i<9; i++)
  {
    d->dtba[i] = DTGTK_TOGGLEBUTTON (dtgtk_togglebutton_new (dtgtk_cairo_paint_alignment,CPF_STYLE_FLAT|(CPF_SPECIAL_FLAG<<(i+1))));
    gtk_widget_set_size_request (GTK_WIDGET (d->dtba[i]), DT_PIXEL_APPLY_DPI(16), DT_PIXEL_APPLY_DPI(16));
    gtk_table_attach (GTK_TABLE (bat), GTK_WIDGET (d->dtba[i]), (i%3),(i%3)+1,(i/3),(i/3)+1,0,0,0,0);
    g_signal_connect (G_OBJECT (d->dtba[i]), "toggled",G_CALLBACK (_alignment_callback), self);
  }
  GtkWidget *hbox2 = gtk_hbox_new(FALSE,0);
  GtkWidget *label4 = gtk_label_new(_("alignment"));
  gtk_box_pack_start(GTK_BOX(hbox2),GTK_WIDGET(label4),TRUE,TRUE,0);
  gtk_box_pack_start(GTK_BOX(hbox2), GTK_WIDGET(bat), TRUE, TRUE, 0);
  tpos++;
  gtk_table_attach(GTK_TABLE(self->widget), GTK_WIDGET(hbox2), 0, 2, tpos, tpos+1, GTK_EXPAND|GTK_FILL, 0, 0, 0);

  ////////////////////////// PRINT SETTINGS

  label = dtgtk_label_new(_("print settings"), DARKTABLE_LABEL_TAB | DARKTABLE_LABEL_ALIGN_RIGHT);
  gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
  tpos++;
  gtk_table_attach(GTK_TABLE(self->widget), label, 0, 2, tpos, tpos+1, GTK_EXPAND|GTK_FILL, 0, 0, 0);

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

  tpos++;
  GList *l = d->profiles;
  label = gtk_label_new(_("export profile"));
  gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
  tpos++;
  gtk_table_attach(GTK_TABLE(self->widget), label, 0, 1, tpos, tpos+1, GTK_EXPAND|GTK_FILL, 0, 0, 0);
  d->profile = GTK_COMBO_BOX(gtk_combo_box_text_new());
  dt_ellipsize_combo(d->profile);
  gtk_table_attach(GTK_TABLE(self->widget), GTK_WIDGET(d->profile), 1, 2, tpos, tpos+1, GTK_SHRINK|GTK_EXPAND|GTK_FILL, 0, 0, 0);
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(d->profile), _("image settings"));

  const gchar *iccprofile = dt_conf_get_string("plugins/print/print/iccprofile");
  int combo_idx = -1, n=0;

  while(l)
  {
    dt_lib_export_profile_t *prof = (dt_lib_export_profile_t *)l->data;
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(d->profile), prof->name);
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

  gtk_combo_box_set_active(d->profile, combo_idx);

  snprintf(tooltip, sizeof(tooltip), _("output ICC profiles in %s/color/out or %s/color/out"), confdir, datadir);
  g_object_set(G_OBJECT(d->profile), "tooltip-text", tooltip, (char *)NULL);
  g_signal_connect(G_OBJECT(d->profile), "changed", G_CALLBACK(_profile_changed), (gpointer)self);

  //  Add export intent combo

  tpos++;
  label = gtk_label_new(_("intent"));
  gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
  gtk_table_attach(GTK_TABLE(self->widget), label, 0, 1, tpos, tpos+1, GTK_EXPAND|GTK_FILL, 0, 0, 0);
  d->intent = GTK_COMBO_BOX(gtk_combo_box_text_new());
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(d->intent), _("image settings"));
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(d->intent), _("perceptual"));
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(d->intent), _("relative colorimetric"));
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(d->intent), C_("rendering intent", "saturation"));
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(d->intent), _("absolute colorimetric"));
  gtk_table_attach(GTK_TABLE(self->widget), GTK_WIDGET(d->intent), 1, 2, tpos, tpos+1, GTK_EXPAND|GTK_FILL, 0, 0, 0);
  gtk_combo_box_set_active(GTK_COMBO_BOX(d->intent), 0);

  g_signal_connect (G_OBJECT (d->intent), "changed", G_CALLBACK (_intent_callback), (gpointer)self);

  //  Add export style combo

  label = gtk_label_new(_("style"));
  gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
  tpos++;
  gtk_table_attach(GTK_TABLE(self->widget), label, 0, 1, tpos, tpos+1, GTK_EXPAND|GTK_FILL, 0, 0, 0);
  d->style = GTK_COMBO_BOX(gtk_combo_box_text_new());

  dt_ellipsize_combo(d->style);

  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(d->style), _("none"));

  GList *styles = dt_styles_get_list("");
  const gchar *current_style = dt_conf_get_string("plugins/print/print/style");
  combo_idx = -1; n=0;

  while (styles)
  {
    dt_style_t *style=(dt_style_t *)styles->data;
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(d->style), style->name);
    n++;
    if (strcmp(style->name,current_style)==0)
      combo_idx=n;
    styles=g_list_next(styles);
  }
  gtk_table_attach(GTK_TABLE(self->widget), GTK_WIDGET(d->style), 1, 2, tpos, tpos+1, GTK_EXPAND|GTK_FILL, 0, 0, 0);
  g_object_set(G_OBJECT(d->style), "tooltip-text", _("temporary style to append while printing"), (char *)NULL);

  // style not found, maybe a style has been removed? revert to none
  if (combo_idx == -1)
  {
    dt_conf_set_string("plugins/print/print/style", "");
    combo_idx=0;
  }
  gtk_combo_box_set_active(GTK_COMBO_BOX(d->style), combo_idx);

  g_signal_connect (G_OBJECT (d->style), "changed",
                    G_CALLBACK (_style_callback),
                    (gpointer)self);

  //  Whether to add/replace style items

  label = gtk_label_new(_("mode"));
  gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
  tpos++;
  gtk_table_attach(GTK_TABLE(self->widget), label, 0, 1, tpos, tpos+1, GTK_EXPAND | GTK_FILL, 0, 0, 0);
  d->style_mode = GTK_COMBO_BOX(gtk_combo_box_text_new());

  dt_ellipsize_combo(d->style_mode);

  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(d->style_mode), _("replace history"));
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(d->style_mode), _("append history"));

  if (dt_conf_get_bool("plugins/print/print/style_append"))
    gtk_combo_box_set_active(d->style_mode, 1);
  else
    gtk_combo_box_set_active(d->style_mode, 0);

  gtk_table_attach(GTK_TABLE(self->widget), GTK_WIDGET(d->style_mode), 1, 2, tpos, tpos+1, GTK_EXPAND|GTK_FILL, 0, 0, 0);
  g_object_set(G_OBJECT(d->style_mode), "tooltip-text", _("whether the style is appended to the history or replacing the history"),
               (char *)NULL);

  g_signal_connect(G_OBJECT(d->style_mode), "changed", G_CALLBACK(_style_mode_changed), (gpointer)self);

  //  Add printer profile combo

  tpos++;
  l = d->profiles;
  label = gtk_label_new(_("printer profile"));
  gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
  tpos++;
  gtk_table_attach(GTK_TABLE(self->widget), label, 0, 1, tpos, tpos+1, GTK_EXPAND|GTK_FILL, 0, 0, 0);
  d->pprofile = GTK_COMBO_BOX(gtk_combo_box_text_new());
  dt_ellipsize_combo(d->pprofile);
  gtk_table_attach(GTK_TABLE(self->widget), GTK_WIDGET(d->pprofile), 1, 2, tpos, tpos+1, GTK_SHRINK|GTK_EXPAND|GTK_FILL, 0, 0, 0);
  const gchar *printer_profile = dt_conf_get_string("plugins/print/printer/iccprofile");
  combo_idx = -1;
  n=0;

  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(d->pprofile), _("none"));
  while(l)
  {
    dt_lib_export_profile_t *prof = (dt_lib_export_profile_t *)l->data;
    // do not add built-in profile, these are in no way for printing
    if (strcmp(prof->filename,"sRGB")!=0 && strcmp(prof->filename,"adobergb")!=0)
    {
      gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(d->pprofile), prof->name);
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
  gtk_combo_box_set_active(d->pprofile, combo_idx);

  snprintf(tooltip, sizeof(tooltip), _("output ICC profiles in %s/color/out or %s/color/out"), confdir, datadir);
  g_object_set(G_OBJECT(d->pprofile), "tooltip-text", tooltip, (char *)NULL);
  g_signal_connect(G_OBJECT(d->pprofile), "changed", G_CALLBACK(_printer_profile_changed), (gpointer)self);

  //  Add printer intent combo

  tpos++;
  label = gtk_label_new(_("intent"));
  gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
  gtk_table_attach(GTK_TABLE(self->widget), label, 0, 1, tpos, tpos+1, GTK_EXPAND|GTK_FILL, 0, 0, 0);
  d->pintent = GTK_COMBO_BOX(gtk_combo_box_text_new());
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(d->pintent), _("perceptual"));
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(d->pintent), _("relative colorimetric"));
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(d->pintent), C_("rendering intent", "saturation"));
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(d->pintent), _("absolute colorimetric"));
  gtk_table_attach(GTK_TABLE(self->widget), GTK_WIDGET(d->pintent), 1, 2, tpos, tpos+1, GTK_EXPAND|GTK_FILL, 0, 0, 0);
  gtk_combo_box_set_active(d->pintent, dt_conf_get_int("plugins/print/print/iccintent") + 1);

  g_signal_connect (G_OBJECT (d->pintent), "changed", G_CALLBACK (_printer_intent_callback), (gpointer)self);

  // Print button

  GtkButton *button = GTK_BUTTON(gtk_button_new_with_label(_("print")));
  d->print_button = button;
  g_object_set(G_OBJECT(button), "tooltip-text", _("print with current settings (ctrl-p)"), (char *)NULL);
  tpos++;
  gtk_table_attach(GTK_TABLE(self->widget), GTK_WIDGET(button), 1, 2, tpos, tpos+1, GTK_EXPAND|GTK_FILL, 0, 0, 0);

  g_signal_connect (G_OBJECT (button), "clicked",
                    G_CALLBACK (_print_button_clicked),
                    (gpointer)self);
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
    _combo_box_set_active_text(ps->papers, paper);

  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON(ps->portrait), !landscape);

  if (profile[0] != '\0')
    _combo_box_set_active_text(ps->profile, profile);
  gtk_combo_box_set_active (GTK_COMBO_BOX(ps->intent), intent);

  if (pprofile[0] != '\0')
    _combo_box_set_active_text(ps->pprofile, pprofile);
  gtk_combo_box_set_active (GTK_COMBO_BOX(ps->pintent), pintent);

  if (style[0] != '\0')
    _combo_box_set_active_text(ps->style, style);
  gtk_combo_box_set_active (GTK_COMBO_BOX(ps->style_mode), style_mode);

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
  gtk_combo_box_set_active(GTK_COMBO_BOX(ps->profile), 0);
  gtk_combo_box_set_active(GTK_COMBO_BOX(ps->pprofile), 0);
  gtk_combo_box_set_active(ps->pintent, dt_conf_get_int("plugins/print/print/iccintent") + 1);
  gtk_combo_box_set_active(GTK_COMBO_BOX(ps->style), 0);
  gtk_combo_box_set_active(GTK_COMBO_BOX(ps->intent), 0);

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
