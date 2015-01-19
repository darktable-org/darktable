/*
 *    This file is part of darktable,
 *    copyright (c) 2015 tobias ellinghaus.
 *
 *    darktable is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 *
 *    darktable is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "version.h"
#include "common/colorspaces.h"
#include "common/darktable.h"
#include "common/imageio.h"
#include "common/imageio_module.h"
#include "common/variables.h"
#include "common/pdf.h"
#include "control/control.h"
#include "gui/gtkentry.h"
#include "dtgtk/button.h"
#include "bauhaus/bauhaus.h"

DT_MODULE(1)

/************ the format part ************/

// clang-format off

// the params type for our custom imageio format
typedef struct dt_imageio_pdf_format_t
{
  dt_imageio_module_data_t  parent;
  dt_pdf_t                 *pdf;
  float                     border;
  int                       bpp;
  gboolean                  only_outline;
  dt_pdf_image_t           *image; // to get it back to the storage
  int                       icc_id;
} dt_imageio_pdf_format_t;

// clang-format on

static const char *_format_name()
{
  return "pdf internal";
}

static size_t _format_params_size(dt_imageio_module_format_t *self)
{
  return sizeof(dt_imageio_module_data_t);
}

static void *_format_get_params(dt_imageio_module_format_t *self)
{
  dt_imageio_pdf_format_t *d = (dt_imageio_pdf_format_t *)calloc(1, sizeof(dt_imageio_pdf_format_t));
  return d;
}

static void _format_free_params(dt_imageio_module_format_t *self, dt_imageio_module_data_t *data)
{
  free(data);
}

static int _format_set_params(dt_imageio_module_format_t *self, const void *params, const int size)
{
  if(size != self->params_size(self)) return 1;
  return 0;
}

static const char *_format_mime(dt_imageio_module_data_t *data)
{
  return "memory"; // there is special casing in the core for "memory" to not raise the tmp file signal. we want that.
}

static int _format_dimension(dt_imageio_module_format_t *self, dt_imageio_module_data_t *data, uint32_t *width, uint32_t *height)
{
  *width = 0;
  *height = 0;
  return 0;
}

static int _format_bpp(dt_imageio_module_data_t *data)
{
  return ((dt_imageio_pdf_format_t *)data)->bpp;
}

static int _format_write_image(dt_imageio_module_data_t *data, const char *filename, const void *in, void *exif,
                               int exif_len, int imgid)
{
  dt_imageio_pdf_format_t *d = (dt_imageio_pdf_format_t *)data;
  uint8_t *image = NULL;

  // TODO
  // decide if we want to push that conversion step into the pdf lib and maybe do it on the fly while writing.
  // that would get rid of one buffer in the case of ASCII_HEX
  if(!d->only_outline)
  {
    if(d->bpp == 8)
    {
      image = (uint8_t *)malloc(data->width * data->height * 3);
      const uint8_t *in_ptr = (const uint8_t *)in;
      uint8_t *out_ptr = image;
      for(int y = 0; y < data->height; y++)
      {
        for(int x = 0; x < data->width; x++, in_ptr += 4, out_ptr += 3)
          memcpy(out_ptr, in_ptr, 3);
      }
    }
    else
    {
      image = (uint8_t *)malloc(data->width * data->height * 3 * sizeof(uint16_t));
      const uint16_t *in_ptr = (const uint16_t *)in;
      uint16_t *out_ptr = (uint16_t *)image;
      for(int y = 0; y < data->height; y++)
      {
        for(int x = 0; x < data->width; x++, in_ptr += 4, out_ptr += 3)
        {
          for(int c = 0; c < 3; c++)
            out_ptr[c] = (0xff00 & (in_ptr[c] << 8)) | (in_ptr[c] >> 8);
        }
      }
    }
  }

  d->image = dt_pdf_add_image(d->pdf, image, d->parent.width, d->parent.height, d->bpp, d->icc_id, d->border);

  free(image);

  return 0;
}

static int _format_levels(dt_imageio_module_data_t *data)
{
  return IMAGEIO_RGB | (((dt_imageio_pdf_format_t *)data)->bpp == 8 ? IMAGEIO_INT8 : IMAGEIO_INT16);
}

static int _format_flags(dt_imageio_module_data_t *data)
{
  return 0;
}

// clang-format off

static dt_imageio_module_format_t pdf_format =
{
  .plugin_name        = "pdf_internal",
  .module             = NULL,
  .widget             = NULL,
  .gui_data           = NULL,
  .version            = NULL,
  .name               = _format_name,
  .gui_init           = NULL,
  .gui_cleanup        = NULL,
  .gui_reset          = NULL,
  .init               = NULL,
  .cleanup            = NULL,
  .legacy_params      = NULL,
  .params_size        = _format_params_size,
  .get_params         = _format_get_params,
  .free_params        = _format_free_params,
  .set_params         = _format_set_params,
  .mime               = _format_mime,
  .extension          = NULL,
  .dimension          = _format_dimension,
  .bpp                = _format_bpp,
  .write_image        = _format_write_image,
  .levels             = _format_levels,
  .flags              = _format_flags,
  .read_image         = NULL,
  .parameter_lua_type = LUAA_INVALID_TYPE
};

/************ the storage part ************/

// gui data
typedef struct pdf_t
{
  GtkEntry       *filename;
  GtkWidget      *overwrite;
  GtkEntry       *title;
  GtkWidget      *size;
  GtkWidget      *orientation;
  GtkEntry       *border;
  GtkSpinButton  *dpi;
  GtkWidget      *rotate;
  GtkWidget      *pages;
  GtkWidget      *icc;
  GtkWidget      *mode;
  GtkWidget      *bpp;
  GtkWidget      *compression;
} pdf_t;

typedef enum _pdf_orientation_t
{
  ORIENTATION_PORTRAIT  = 0,
  ORIENTATION_LANDSCAPE = 1
} _pdf_orientation_t;

typedef enum _pdf_pages_t
{
  PAGES_ALL     = 0,
  PAGES_SINGLE  = 1,
  PAGES_CONTACT = 2
} _pdf_pages_t;

typedef enum _pdf_mode_t
{
  MODE_NORMAL = 0,
  MODE_DRAFT  = 1,
  MODE_DEBUG  = 2,
} _pdf_mode_t;

static const struct
{
  char *name;
  int   bpp;
} _pdf_bpp[] =
{
  { N_("8 bit"),   8 },
  { N_("16 bit"), 16 },
  { NULL,          0 }
};

typedef struct _pdf_icc_t
{
  char *name;
  int   icc_id;
} _pdf_icc_t;

// saved params -- just there to get the sizeof() without worrying about padding, ...
typedef struct dt_imageio_pdf_params_t
{
  dt_imageio_module_data_t  parent;
  char                      filename[DT_MAX_PATH_FOR_PARAMS];
  char                      title[128];
  char                      size[64];
  _pdf_orientation_t        orientation;
  char                      border[64];
  float                     dpi;
  gboolean                  rotate;
  _pdf_pages_t              pages;
  gboolean                  icc;
  _pdf_mode_t               mode;
  dt_pdf_stream_encoder_t   compression;
  int                       bpp;

  // the following are unused at the moment
  int                       intent;
} dt_imageio_pdf_params_t;

// the real type used in the code
typedef struct dt_imageio_pdf_t
{
  dt_imageio_pdf_params_t  params;
  gboolean                 overwrite;
  char                    *actual_filename;
  dt_pdf_t                *pdf;
  GList                   *images;
  GList                   *icc_profiles;
} dt_imageio_pdf_t;

// clang-format on


const char *name(const struct dt_imageio_module_storage_t *self)
{
  return _("pdf");
}

// we only want our own format to be used that we set during export manually
int supported(struct dt_imageio_module_storage_t *self, struct dt_imageio_module_format_t *format)
{
  return 0;
}

static void button_clicked(GtkWidget *widget, dt_imageio_module_storage_t *self)
{
  pdf_t *d = (pdf_t *)self->gui_data;
  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  GtkWidget *filechooser = gtk_file_chooser_dialog_new(
      _("select directory"), GTK_WINDOW(win), GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER, _("_Cancel"),
      GTK_RESPONSE_CANCEL, _("_Select as output destination"), GTK_RESPONSE_ACCEPT, (char *)NULL);

  gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(filechooser), FALSE);
  gchar *old = g_strdup(gtk_entry_get_text(d->filename));
  char *c = g_strstr_len(old, -1, "$");
  if(c) *c = '\0';
  gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(filechooser), old);
  g_free(old);
  if(gtk_dialog_run(GTK_DIALOG(filechooser)) == GTK_RESPONSE_ACCEPT)
  {
    gchar *dir = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(filechooser));
    char composed[PATH_MAX] = { 0 };
    snprintf(composed, sizeof(composed), "%s/$(FILE_NAME)", dir);
    gtk_entry_set_text(GTK_ENTRY(d->filename), composed);
    g_free(dir);
  }
  gtk_widget_destroy(filechooser);
}

static void size_toggle_callback(GtkWidget *widget, gpointer user_data);

// set the paper size dropdown from the UNTRANSLATED string
static void _set_paper_size(dt_imageio_module_storage_t *self, const char *text)
{
  pdf_t *d = (pdf_t *)self->gui_data;

  if(text == NULL || *text == '\0') return;

  g_signal_handlers_block_by_func(d->size, size_toggle_callback, self);

  const GList *labels = dt_bauhaus_combobox_get_labels(d->size);
  int pos = 0;

  while(labels)
  {
    const char *l = (char*)labels->data;
    if((pos < dt_pdf_paper_sizes_n && !strcasecmp(text, dt_pdf_paper_sizes[pos].name)) || !strcasecmp(text, l))
      break;
    pos++;
    labels = g_list_next(labels);
  }

  if(labels)
  {
    // we jumped out of the loop -> found it
    dt_bauhaus_combobox_set(d->size, pos);
    dt_conf_set_string("plugins/imageio/storage/pdf/size", text);
  }
  else
  {
    // newly seen -- check if it is valid
    float width, height;
    if(dt_pdf_parse_paper_size(text, &width, &height))
    {
      // seems to be ok
      dt_bauhaus_combobox_add(d->size, text);
      dt_bauhaus_combobox_set(d->size, pos);
      dt_conf_set_string("plugins/imageio/storage/pdf/size", text);
    }
    else
    {
      dt_control_log(_("invalid paper size"));
      gchar *old_size = dt_conf_get_string("plugins/imageio/storage/pdf/size");
      if(old_size)
      {
        // safeguard against strange stuff in config
        if(dt_pdf_parse_paper_size(old_size, &width, &height))
          _set_paper_size(self, old_size);
        else
          _set_paper_size(self, dt_pdf_paper_sizes[0].name);

        g_free(old_size);
      }
    }
  }

  g_signal_handlers_unblock_by_func(d->size, size_toggle_callback, self);

}

static void filename_changed_callback(GtkWidget *widget, gpointer user_data)
{
  dt_conf_set_string("plugins/imageio/storage/pdf/filename", gtk_entry_get_text(GTK_ENTRY(widget)));
}

static void title_changed_callback(GtkWidget *widget, gpointer user_data)
{
  dt_conf_set_string("plugins/imageio/storage/pdf/title", gtk_entry_get_text(GTK_ENTRY(widget)));
}

static void border_changed_callback(GtkWidget *widget, gpointer user_data)
{
  dt_conf_set_string("plugins/imageio/storage/pdf/border", gtk_entry_get_text(GTK_ENTRY(widget)));
}

static void size_toggle_callback(GtkWidget *widget, gpointer user_data)
{
  unsigned int pos = dt_bauhaus_combobox_get(widget);
  if(pos < dt_pdf_paper_sizes_n)
    _set_paper_size(user_data, dt_pdf_paper_sizes[pos].name); // has to be untranslated
  else
    _set_paper_size(user_data, dt_bauhaus_combobox_get_text(widget));
}

static void orientation_toggle_callback(GtkWidget *widget, gpointer user_data)
{
  dt_conf_set_int("plugins/imageio/storage/pdf/orientation", dt_bauhaus_combobox_get(widget));
}

static void dpi_changed_callback(GtkWidget *widget, gpointer user_data)
{
  dt_conf_set_float("plugins/imageio/storage/pdf/dpi", gtk_spin_button_get_value(GTK_SPIN_BUTTON(widget)));
}

static void rotate_toggle_callback(GtkWidget *widget, gpointer user_data)
{
  dt_conf_set_bool("plugins/imageio/storage/pdf/rotate", dt_bauhaus_combobox_get(widget) == 1);
}

static void pages_toggle_callback(GtkWidget *widget, gpointer user_data)
{
  dt_conf_set_int("plugins/imageio/storage/pdf/pages", dt_bauhaus_combobox_get(widget));
}

static void icc_toggle_callback(GtkWidget *widget, gpointer user_data)
{
  dt_conf_set_bool("plugins/imageio/storage/pdf/icc", dt_bauhaus_combobox_get(widget) == 1);
}

static void mode_toggle_callback(GtkWidget *widget, gpointer user_data)
{
  dt_conf_set_int("plugins/imageio/storage/pdf/mode", dt_bauhaus_combobox_get(widget));
}

static void bpp_toggle_callback(GtkWidget *widget, gpointer user_data)
{
  const int sel = dt_bauhaus_combobox_get(widget);
  dt_conf_set_int("plugins/imageio/storage/pdf/bpp", _pdf_bpp[sel].bpp);
}

static void compression_toggle_callback(GtkWidget *widget, gpointer user_data)
{
  dt_conf_set_int("plugins/imageio/storage/pdf/compression", dt_bauhaus_combobox_get(widget));
}

void gui_init(dt_imageio_module_storage_t *self)
{
  pdf_t *d = calloc(1, sizeof(pdf_t));
  self->gui_data = (void *)d;
  self->widget = gtk_grid_new();
  GtkGrid *grid = GTK_GRID(self->widget);
  gtk_grid_set_row_spacing(grid, DT_PIXEL_APPLY_DPI(5));
  gtk_grid_set_column_spacing(grid, DT_PIXEL_APPLY_DPI(8));

  GtkWidget *widget;
  int line = 0;

  // filename

  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_PIXEL_APPLY_DPI(8));
  gtk_grid_attach(grid, hbox, 0, line++, 2, 1);

  widget = gtk_entry_new();
  gtk_box_pack_start(GTK_BOX(hbox), widget, TRUE, TRUE, 0);
  gchar *str = dt_conf_get_string("plugins/imageio/storage/pdf/filename");
  if(str)
  {
    gtk_entry_set_text(GTK_ENTRY(widget), str);
    g_free(str);
  }
  d->filename = GTK_ENTRY(widget);
  dt_gui_key_accel_block_on_focus_connect(widget);

  dt_gtkentry_setup_completion(GTK_ENTRY(widget), dt_gtkentry_get_default_path_compl_list());

  char *tooltip_text = dt_gtkentry_build_completion_tooltip_text(
      _("enter the path where to put the exported pdf\nrecognized variables (using the first image):"),
      dt_gtkentry_get_default_path_compl_list());
  g_object_set(G_OBJECT(widget), "tooltip-text", tooltip_text, (char *)NULL);
  g_signal_connect(G_OBJECT(widget), "changed", G_CALLBACK(filename_changed_callback), self);
  g_free(tooltip_text);

  widget = dtgtk_button_new(dtgtk_cairo_paint_directory, CPF_DO_NOT_USE_BORDER);
  gtk_widget_set_hexpand(widget, FALSE);
  gtk_widget_set_halign(widget, GTK_ALIGN_START);
  g_object_set(G_OBJECT(widget), "tooltip-text", _("select directory"), (char *)NULL);
  gtk_box_pack_start(GTK_BOX(hbox), widget, FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(widget), "clicked", G_CALLBACK(button_clicked), self);

  // overwrite

  d->overwrite = dt_bauhaus_combobox_new(NULL);
  dt_bauhaus_widget_set_label(d->overwrite, NULL, _("on conflict"));
  dt_bauhaus_combobox_add(d->overwrite, _("create unique filename"));
  dt_bauhaus_combobox_add(d->overwrite, _("overwrite"));
  gtk_grid_attach(grid, d->overwrite, 0, ++line, 2, 1);
  dt_bauhaus_combobox_set(d->overwrite, 0);

  // title

  widget = gtk_label_new(_("title"));
  gtk_widget_set_halign(widget, GTK_ALIGN_START);
  g_object_set(G_OBJECT(widget), "xalign", 0.0, NULL);
  gtk_grid_attach(grid, widget, 0, ++line, 1, 1);

  d->title = GTK_ENTRY(gtk_entry_new());
  gtk_widget_set_hexpand(GTK_WIDGET(d->title), TRUE);
  gtk_grid_attach(grid, GTK_WIDGET(d->title), 1, line, 1, 1);
  dt_gui_key_accel_block_on_focus_connect(GTK_WIDGET(d->title));
  g_object_set(G_OBJECT(d->title), "tooltip-text", _("enter the title of the pdf"), (char *)NULL);
  str = dt_conf_get_string("plugins/imageio/storage/pdf/title");
  if(str)
  {
    gtk_entry_set_text(GTK_ENTRY(d->title), str);
    g_free(str);
  }
  g_signal_connect(G_OBJECT(d->title), "changed", G_CALLBACK(title_changed_callback), self);

  // paper size

  d->size = dt_bauhaus_combobox_new(NULL);
  dt_bauhaus_combobox_set_editable(d->size, 1);
  dt_bauhaus_widget_set_label(d->size, NULL, _("paper size"));
  for(int i = 0; dt_pdf_paper_sizes[i].name; i++)
    dt_bauhaus_combobox_add(d->size, _(dt_pdf_paper_sizes[i].name));
  gtk_grid_attach(grid, GTK_WIDGET(d->size), 0, ++line, 2, 1);
  g_signal_connect(G_OBJECT(d->size), "value-changed", G_CALLBACK(size_toggle_callback), self);
  g_object_set(G_OBJECT(d->size), "tooltip-text", _("paper size of the pdf\neither one from the list or \"<width> [unit] x <height> <unit>\nexample: 210 mm x 2.97 cm"), (char *)NULL);
  str = dt_conf_get_string("plugins/imageio/storage/pdf/size");
  if(str)
  {
    _set_paper_size(self, str);
    g_free(str);
  }

  // orientation

  d->orientation = dt_bauhaus_combobox_new(NULL);
  dt_bauhaus_widget_set_label(d->orientation, NULL, _("page orientation"));
  dt_bauhaus_combobox_add(d->orientation, _("portrait"));
  dt_bauhaus_combobox_add(d->orientation, _("landscape"));
  gtk_grid_attach(grid, GTK_WIDGET(d->orientation), 0, ++line, 2, 1);
  g_signal_connect(G_OBJECT(d->orientation), "value-changed", G_CALLBACK(orientation_toggle_callback), self);
  g_object_set(G_OBJECT(d->orientation), "tooltip-text", _("paper orientation of the pdf"), (char *)NULL);
  dt_bauhaus_combobox_set(d->orientation, dt_conf_get_int("plugins/imageio/storage/pdf/orientation"));

  // border

  widget = gtk_label_new(_("border"));
  gtk_widget_set_halign(widget, GTK_ALIGN_START);
  g_object_set(G_OBJECT(widget), "xalign", 0.0, NULL);
  gtk_grid_attach(grid, widget, 0, ++line, 1, 1);

  d->border = GTK_ENTRY(gtk_entry_new());
  gtk_entry_set_max_length(d->border, sizeof(((dt_imageio_pdf_params_t *)NULL)->border) - 1);
  gtk_grid_attach(grid, GTK_WIDGET(d->border), 1, line, 1, 1);
  dt_gui_key_accel_block_on_focus_connect(GTK_WIDGET(d->border));
  g_object_set(G_OBJECT(d->border), "tooltip-text", _("empty space around the pdf\nformat: size + unit\nexamples: 10 mm, 1 inch"), (char *)NULL);
  str = dt_conf_get_string("plugins/imageio/storage/pdf/border");
  if(str)
  {
    gtk_entry_set_text(GTK_ENTRY(d->border), str);
    g_free(str);
  }
  g_signal_connect(G_OBJECT(d->border), "changed", G_CALLBACK(border_changed_callback), self);

  // dpi

  widget = gtk_label_new(_("dpi"));
  gtk_widget_set_halign(widget, GTK_ALIGN_START);
  g_object_set(G_OBJECT(widget), "xalign", 0.0, NULL);
  gtk_grid_attach(grid, widget, 0, ++line, 1, 1);

  d->dpi = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(1, 5000, 1));
  gtk_grid_attach(grid, GTK_WIDGET(d->dpi), 1, line, 1, 1);
  dt_gui_key_accel_block_on_focus_connect(GTK_WIDGET(d->dpi));
  g_object_set(G_OBJECT(d->dpi), "tooltip-text", _("dpi of the images inside the pdf"), (char *)NULL);
  gtk_spin_button_set_value(d->dpi, dt_conf_get_float("plugins/imageio/storage/pdf/dpi"));
  g_signal_connect(G_OBJECT(d->dpi), "value-changed", G_CALLBACK(dpi_changed_callback), self);

  // rotate images yes|no

  d->rotate = dt_bauhaus_combobox_new(NULL);
  dt_bauhaus_widget_set_label(d->rotate, NULL, _("TODO: rotate images"));
  dt_bauhaus_combobox_add(d->rotate, _("no"));
  dt_bauhaus_combobox_add(d->rotate, _("yes"));
  gtk_grid_attach(grid, GTK_WIDGET(d->rotate), 0, ++line, 2, 1);
  g_signal_connect(G_OBJECT(d->rotate), "value-changed", G_CALLBACK(rotate_toggle_callback), self);
  g_object_set(G_OBJECT(d->rotate), "tooltip-text",
               _("images can be rotated to match the pdf orientation to waste less space when printing"),
               (char *)NULL);
  dt_bauhaus_combobox_set(d->rotate, dt_conf_get_bool("plugins/imageio/storage/pdf/rotate"));
  gtk_widget_set_sensitive(d->rotate, FALSE); // TODO

  // pages all|single images|contact sheet

  d->pages = dt_bauhaus_combobox_new(NULL);
  dt_bauhaus_widget_set_label(d->pages, NULL, _("TODO: pages"));
  dt_bauhaus_combobox_add(d->pages, _("all"));
  dt_bauhaus_combobox_add(d->pages, _("single images"));
  dt_bauhaus_combobox_add(d->pages, _("contact sheet"));
  gtk_grid_attach(grid, GTK_WIDGET(d->pages), 0, ++line, 2, 1);
  g_signal_connect(G_OBJECT(d->pages), "value-changed", G_CALLBACK(pages_toggle_callback), self);
  g_object_set(G_OBJECT(d->pages), "tooltip-text", _("what pages should be added to the pdf"), (char *)NULL);
  dt_bauhaus_combobox_set(d->pages, dt_conf_get_int("plugins/imageio/storage/pdf/pages"));
  gtk_widget_set_sensitive(d->pages, FALSE); // TODO

  // embedded icc profile yes|no

  d->icc = dt_bauhaus_combobox_new(NULL);
  dt_bauhaus_widget_set_label(d->icc, NULL, _("embed icc profiles"));
  dt_bauhaus_combobox_add(d->icc, _("no"));
  dt_bauhaus_combobox_add(d->icc, _("yes"));
  gtk_grid_attach(grid, GTK_WIDGET(d->icc), 0, ++line, 2, 1);
  g_signal_connect(G_OBJECT(d->icc), "value-changed", G_CALLBACK(icc_toggle_callback), self);
  g_object_set(G_OBJECT(d->icc), "tooltip-text", _("images can be tagged with their icc profile"), (char *)NULL);
  dt_bauhaus_combobox_set(d->icc, dt_conf_get_bool("plugins/imageio/storage/pdf/icc"));

  // image mode normal|draft|debug

  d->mode = dt_bauhaus_combobox_new(NULL);
  dt_bauhaus_widget_set_label(d->mode, NULL, _("image mode"));
  dt_bauhaus_combobox_add(d->mode, _("normal"));
  dt_bauhaus_combobox_add(d->mode, _("draft"));
  dt_bauhaus_combobox_add(d->mode, _("debug"));
  gtk_grid_attach(grid, GTK_WIDGET(d->mode), 0, ++line, 2, 1);
  g_signal_connect(G_OBJECT(d->mode), "value-changed", G_CALLBACK(mode_toggle_callback), self);
  g_object_set(G_OBJECT(d->mode), "tooltip-text",
               _("normal -- just put the images into the pdf\n"
                 "draft mode -- images are replaced with boxes\n"
                 "debug -- only show the outlines and bounding boxen"),
               (char *)NULL);
  dt_bauhaus_combobox_set(d->mode, dt_conf_get_int("plugins/imageio/storage/pdf/mode"));

  // bpp

  d->bpp = dt_bauhaus_combobox_new(NULL);
  dt_bauhaus_widget_set_label(d->bpp, NULL, _("bit depth"));
  for(int i = 0; _pdf_bpp[i].name; i++)
    dt_bauhaus_combobox_add(d->bpp, _(_pdf_bpp[i].name));
  gtk_grid_attach(grid, GTK_WIDGET(d->bpp), 0, ++line, 2, 1);
  g_signal_connect(G_OBJECT(d->bpp), "value-changed", G_CALLBACK(bpp_toggle_callback), self);
  g_object_set(G_OBJECT(d->bpp), "tooltip-text", _("bits per channel of the embedded images"), (char *)NULL);
  dt_bauhaus_combobox_set(d->bpp, dt_conf_get_int("plugins/imageio/storage/pdf/bpp"));

  // compression

  d->compression = dt_bauhaus_combobox_new(NULL);
  dt_bauhaus_widget_set_label(d->compression, NULL, _("compression"));
  dt_bauhaus_combobox_add(d->compression, _("uncompressed"));
  dt_bauhaus_combobox_add(d->compression, _("deflate"));
  gtk_grid_attach(grid, GTK_WIDGET(d->compression), 0, ++line, 2, 1);
  g_signal_connect(G_OBJECT(d->compression), "value-changed", G_CALLBACK(compression_toggle_callback), self);
  g_object_set(G_OBJECT(d->compression), "tooltip-text", _("method used for image compression\nuncompressed -- fast but big files\ndeflate -- smaller files but slower"), (char *)NULL);
  dt_bauhaus_combobox_set(d->compression, dt_conf_get_int("plugins/imageio/storage/pdf/compression"));

}

void gui_cleanup(dt_imageio_module_storage_t *self)
{
  pdf_t *d = (pdf_t *)self->gui_data;
  dt_gui_key_accel_block_on_focus_disconnect(GTK_WIDGET(d->filename));
  dt_gui_key_accel_block_on_focus_disconnect(GTK_WIDGET(d->title));
  dt_gui_key_accel_block_on_focus_disconnect(GTK_WIDGET(d->dpi));
  free(self->gui_data);
}

void gui_reset(dt_imageio_module_storage_t *self)
{
  pdf_t *d = (pdf_t *)self->gui_data;

  dpi_changed_callback(GTK_WIDGET(d->dpi), self);
  filename_changed_callback(GTK_WIDGET(d->filename), self);
  icc_toggle_callback(GTK_WIDGET(d->icc), self);
  mode_toggle_callback(GTK_WIDGET(d->mode), self);
  orientation_toggle_callback(GTK_WIDGET(d->orientation), self);
  pages_toggle_callback(GTK_WIDGET(d->pages), self);
  rotate_toggle_callback(GTK_WIDGET(d->rotate), self);
  size_toggle_callback(GTK_WIDGET(d->size), self);
  title_changed_callback(GTK_WIDGET(d->title), self);
  bpp_toggle_callback(GTK_WIDGET(d->bpp), self);
  compression_toggle_callback(GTK_WIDGET(d->compression), self);
  dt_bauhaus_combobox_set(d->overwrite, 0);
}

// we don't use the real type to get rid of pointers, but don't have to care about struct magic
size_t params_size(dt_imageio_module_storage_t *self)
{
  return sizeof(dt_imageio_pdf_params_t);
}

void init(dt_imageio_module_storage_t *self)
{
#ifdef USE_LUA
  // TODO
#endif
}

void *get_params(dt_imageio_module_storage_t *self)
{
  pdf_t *g = (pdf_t *)self->gui_data;
  dt_imageio_pdf_t *d = (dt_imageio_pdf_t *)calloc(1, sizeof(dt_imageio_pdf_t));

  char *text = dt_conf_get_string("plugins/imageio/storage/pdf/filename");
  g_strlcpy(d->params.filename, text, sizeof(d->params.filename));
  g_free(text);

  text = dt_conf_get_string("plugins/imageio/storage/pdf/title");
  g_strlcpy(d->params.title, text, sizeof(d->params.title));
  g_free(text);

  text = dt_conf_get_string("plugins/imageio/storage/pdf/border");
  g_strlcpy(d->params.border, text, sizeof(d->params.border));
  g_free(text);

  text = dt_conf_get_string("plugins/imageio/storage/pdf/size");
  g_strlcpy(d->params.size, text, sizeof(d->params.size));
  g_free(text);

  d->params.bpp = dt_conf_get_int("plugins/imageio/storage/pdf/bpp");
  d->params.compression = dt_conf_get_int("plugins/imageio/storage/pdf/compression");
  d->params.dpi = dt_conf_get_float("plugins/imageio/storage/pdf/dpi");
  d->params.icc = dt_conf_get_bool("plugins/imageio/storage/pdf/icc");
  d->params.mode = dt_conf_get_int("plugins/imageio/storage/pdf/mode");
  d->params.orientation = dt_conf_get_int("plugins/imageio/storage/pdf/orientation");
  d->params.pages = dt_conf_get_int("plugins/imageio/storage/pdf/pages");
  d->params.rotate = dt_conf_get_bool("plugins/imageio/storage/pdf/rotate");

  d->overwrite = dt_bauhaus_combobox_get(g->overwrite) == 1;

  return d;
}

void free_params(dt_imageio_module_storage_t *self, dt_imageio_module_data_t *params)
{
  free(params);
}

int set_params(dt_imageio_module_storage_t *self, const void *params, const int size)
{
  if(size != self->params_size(self)) return 1;
  const dt_imageio_pdf_t *d = (dt_imageio_pdf_t *)params;
  pdf_t *g = (pdf_t *)self->gui_data;

  for(int i = 0; _pdf_bpp[i].name; i++)
  {
    if(_pdf_bpp[i].bpp == d->params.bpp)
      dt_bauhaus_combobox_set(g->bpp, i);
  }

  gtk_entry_set_text(g->filename, d->params.filename);
  dt_bauhaus_combobox_set(g->overwrite, 0);
  gtk_entry_set_text(g->title, d->params.title);
  gtk_entry_set_text(g->border, d->params.border);
  dt_bauhaus_combobox_set(g->compression, d->params.compression);
  gtk_spin_button_set_value(g->dpi, d->params.dpi);
  dt_bauhaus_combobox_set(g->icc, d->params.icc);
  dt_bauhaus_combobox_set(g->mode, d->params.mode);
  dt_bauhaus_combobox_set(g->orientation, d->params.orientation);
  dt_bauhaus_combobox_set(g->pages, d->params.pages);
  dt_bauhaus_combobox_set(g->rotate, d->params.rotate);
  _set_paper_size(self, d->params.size);

  dt_conf_set_string("plugins/imageio/storage/pdf/filename", d->params.filename);
  dt_conf_set_string("plugins/imageio/storage/pdf/title", d->params.title);
  dt_conf_set_string("plugins/imageio/storage/pdf/border", d->params.border);
  dt_conf_set_int("plugins/imageio/storage/pdf/bpp", d->params.bpp);
  dt_conf_set_int("plugins/imageio/storage/pdf/compression", d->params.compression);
  dt_conf_set_float("plugins/imageio/storage/pdf/dpi", d->params.dpi);
  dt_conf_set_bool("plugins/imageio/storage/pdf/icc", d->params.icc);
  dt_conf_set_int("plugins/imageio/storage/pdf/mode", d->params.mode);
  dt_conf_set_int("plugins/imageio/storage/pdf/orientation", d->params.orientation);
  dt_conf_set_int("plugins/imageio/storage/pdf/pages", d->params.pages);
  dt_conf_set_bool("plugins/imageio/storage/pdf/rotate", d->params.rotate);

  return 0;
}

void export_dispatched(dt_imageio_module_storage_t *self)
{
  pdf_t *g = (pdf_t *)self->gui_data;
  dt_bauhaus_combobox_set(g->overwrite, 0);
}

int dimension(dt_imageio_module_storage_t *self, dt_imageio_module_data_t *data, uint32_t *width, uint32_t *height)
{
  if(data)
  {
    *width = data->max_width;
    *height = data->max_height;
  }
  return 0;
}

int initialize_store(dt_imageio_module_storage_t *self, dt_imageio_module_data_t *sdata,
                      dt_imageio_module_format_t **format, dt_imageio_module_data_t **fdata,
                      GList **images, const gboolean high_quality)
{
  dt_imageio_pdf_t *d = (dt_imageio_pdf_t *)sdata;

  // let's play dirty and change whatever format was set to our internal one
  (*format)->free_params(*format, *fdata);
  *format = &pdf_format;
  *fdata = calloc(1, sizeof(dt_imageio_pdf_format_t));
  dt_imageio_pdf_format_t *f = (dt_imageio_pdf_format_t *)*fdata;

  // general file system setup
  dt_variables_params_t *vp = NULL;
  dt_variables_params_init(&vp);

  char filename[PATH_MAX] = { 0 };
  char dirname[PATH_MAX] = { 0 };
  gboolean from_cache = FALSE;
  int fail = 0;
  int imgid = GPOINTER_TO_INT((*images)->data);

  dt_image_full_path(imgid, dirname, sizeof(dirname), &from_cache);

  dt_pthread_mutex_lock(&darktable.plugin_threadsafe);

  {
    // if filenamepattern is a directory just add ${FILE_NAME} as default ...
    if(g_file_test(d->params.filename, G_FILE_TEST_IS_DIR)
      || ((d->params.filename + strlen(d->params.filename))[0] == '/'
           || (d->params.filename + strlen(d->params.filename))[0] == '\\'))
      snprintf(d->params.filename + strlen(d->params.filename), sizeof(d->params.filename) - strlen(d->params.filename), "$(FILE_NAME)");

    gchar *fixed_path = dt_util_fix_path(d->params.filename);
    g_strlcpy(d->params.filename, fixed_path, sizeof(d->params.filename));
    g_free(fixed_path);

    vp->filename = dirname;
    vp->jobcode = "export";
    vp->imgid = imgid;
    vp->sequence = 0; // only one file on disk in the end
    dt_variables_expand(vp, d->params.filename, TRUE);
    g_strlcpy(filename, dt_variables_get_result(vp), sizeof(filename));
    g_strlcpy(dirname, filename, sizeof(dirname));

    const char *ext = "pdf";
    char *c = dirname + strlen(dirname);
    for(; c > dirname && *c != '/'; c--)
      ;
    if(*c == '/')
    {
      if(c > dirname) // /.../.../foo
        c[0] = '\0';
      else // /foo
        c[1] = '\0';
    }
    else if(c == dirname) // foo
    {
      c[0] = '.';
      c[1] = '\0';
    }

    if(g_mkdir_with_parents(dirname, 0755))
    {
      fprintf(stderr, "[imageio_storage_pdf] could not create directory: `%s'!\n", dirname);
      dt_control_log(_("could not create directory `%s'!"), dirname);
      fail = 1;
      goto failed;
    }
    if(g_access(dirname, W_OK) != 0)
    {
      fprintf(stderr, "[imageio_storage_pdf] could not write to directory: `%s'!\n", dirname);
      dt_control_log(_("could not write to directory `%s'!"), dirname);
      fail = 1;
      goto failed;
    }

    c = filename + strlen(filename);

    sprintf(c, ".%s", ext);

    /* prevent overwrite of files */
failed:
    if(!d->overwrite)
    {
      int seq = 1;
      if(!fail && g_file_test(filename, G_FILE_TEST_EXISTS))
      {
        do
        {
          sprintf(c, "_%.2d.%s", seq, ext);
          seq++;
        } while(g_file_test(filename, G_FILE_TEST_EXISTS));
      }
    }
  } // end of critical block

  dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);

  dt_variables_params_destroy(vp);

  if(fail) return 1;

  // specific pdf setup
  float page_width, page_height, border;
  float page_dpi = d->params.dpi;

  if(!dt_pdf_parse_paper_size(d->params.size, &page_width, &page_height))
  {
    fprintf(stderr, "[imageio_storage_pdf] invalid paper size: `%s'!\n", d->params.size);
    dt_control_log(_("invalid paper size"));
    return 1;
  }

  if(!dt_pdf_parse_length(d->params.border, &border))
  {
    fprintf(stderr, "[imageio_storage_pdf] invalid border size: `%s'!\n", d->params.border);
    dt_control_log(_("invalid border size"));
    return 1;
  }

  if(d->params.orientation == ORIENTATION_LANDSCAPE)
  {
    float w = page_width, h = page_height;
    page_width = MAX(w, h);
    page_height = MIN(w, h);
  }
  else
  {
    float w = page_width, h = page_height;
    page_width = MIN(w, h);
    page_height = MAX(w, h);
  }

  // export in the size we want the images in the end
  sdata->max_width = dt_pdf_point_to_pixel(page_width - 2 * border, page_dpi) + 0.5;
  sdata->max_height = dt_pdf_point_to_pixel(page_height - 2 * border, page_dpi) + 0.5;

  unsigned int compression = d->params.compression;
  compression = MIN(compression, DT_PDF_STREAM_ENCODER_FLATE);


  dt_pdf_t *pdf = dt_pdf_start(filename, page_width, page_height, page_dpi, compression);
  if(!pdf)
  {
    fprintf(stderr, "[imageio_storage_pdf] could not export to file: `%s'!\n", filename);
    dt_control_log(_("could not export to file `%s'!"), filename);
    return 1;
  }

  // TODO: escape ')' and maybe also '('
  pdf->title = d->params.title;

  f->pdf = pdf;
  f->border = border;
  f->only_outline = d->params.mode != MODE_NORMAL;
  f->bpp = d->params.bpp;
  d->pdf = pdf;
  d->actual_filename = g_strdup(filename);

  return 0;
}

int store(dt_imageio_module_storage_t *self, dt_imageio_module_data_t *sdata, const int imgid,
          dt_imageio_module_format_t *format, dt_imageio_module_data_t *fdata, const int num, const int total,
          const gboolean high_quality)
{
  dt_imageio_pdf_t *d = (dt_imageio_pdf_t *)sdata;
  dt_imageio_pdf_format_t *f = (dt_imageio_pdf_format_t *)fdata;

  if(imgid > 0 && d->params.icc && d->params.mode == MODE_NORMAL)
  {
    // get the id of the profile
    char *profile_name = dt_colorspaces_get_output_profile_name(imgid);
    int icc_id = 0;

    // look it up in the list
    for(GList *iter = d->icc_profiles; iter; iter = g_list_next(iter))
    {
      _pdf_icc_t *icc = (_pdf_icc_t *)iter->data;
      if(!g_strcmp0(profile_name, icc->name))
      {
        icc_id = icc->icc_id;
        break;
      }
    }
    if(icc_id == 0)
    {
      cmsHPROFILE profile = dt_colorspaces_create_output_profile(imgid);
      uint32_t len = 0;
      cmsSaveProfileToMem(profile, 0, &len);
      if(len > 0)
      {
        unsigned char buf[len];
        cmsSaveProfileToMem(profile, buf, &len);
        icc_id = dt_pdf_add_icc_from_data(d->pdf, buf, len);
        _pdf_icc_t *icc = (_pdf_icc_t *)malloc(sizeof(_pdf_icc_t));
        icc->name = profile_name;
        icc->icc_id = icc_id;
        d->icc_profiles = g_list_append(d->icc_profiles, icc);
      }
      else
        free(profile_name);
      dt_colorspaces_cleanup_profile(profile);
    }
    else
      free(profile_name);

    f->icc_id = icc_id;
  }


  if(dt_imageio_export_with_flags(imgid, "unused", format, fdata, 1, 0, high_quality, 0, NULL, FALSE, self, sdata) != 0)
  {
    fprintf(stderr, "[imageio_storage_pdf] could not export to file: `%s'!\n", d->actual_filename);
    dt_control_log(_("could not export to file `%s'!"), d->actual_filename);
    return 1;
  }

  // now fdata should contain the image pointer, we are taking over the ownership of the pointer
  if(f->image)
  {
    d->images = g_list_append(d->images, f->image);
    f->image = NULL;
  }

  printf("[export_job] exported to `%s'\n", d->actual_filename);
  char *trunc = d->actual_filename + strlen(d->actual_filename) - 32;
  if(trunc < d->actual_filename) trunc = d->actual_filename;
  dt_control_log(_("%d/%d exported to `%s%s'"), num, total, trunc != d->actual_filename ? ".." : "", trunc);
  return 0;
}

void finalize_store(dt_imageio_module_storage_t *self, dt_imageio_module_data_t *sdata)
{
  dt_imageio_pdf_t *d = (dt_imageio_pdf_t *)sdata;

  int n_images = g_list_length(d->images);
  dt_pdf_page_t *pages[n_images];

  gboolean outline_mode = d->params.mode != MODE_NORMAL;
  gboolean show_bb = d->params.mode == MODE_DEBUG;

  // add a page for every image
  GList *iter = d->images;
  int i = 0;
  while(iter)
  {
    dt_pdf_image_t *image = (dt_pdf_image_t *)iter->data;
    image->outline_mode = outline_mode;
    image->show_bb = show_bb;
    pages[i] = dt_pdf_add_page(d->pdf, &image, 1);
    iter = g_list_next(iter);
    i++;
  }

  dt_pdf_finish(d->pdf, pages, n_images);

  // we allocated the images and pages. the main pdf object gets free'ed in dt_pdf_finish().
  g_list_free_full(d->images, free);
  for(int i = 0; i < n_images; i++)
    free(pages[i]);
  g_free(d->actual_filename);
  for(GList *iter = d->icc_profiles; iter; iter = g_list_next(iter))
  {
    _pdf_icc_t *icc = (_pdf_icc_t *)iter->data;
    g_free(icc->name);
  }
  g_list_free_full(d->icc_profiles, free);
}


// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
