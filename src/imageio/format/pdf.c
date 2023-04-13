/*
    This file is part of darktable,
    Copyright (C) 2015-2023 darktable developers.

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

#include "common/pdf.h"
#include "bauhaus/bauhaus.h"
#include "common/colorspaces.h"
#include "common/darktable.h"
#include "common/variables.h"
#include "control/control.h"
#include "dtgtk/button.h"
#include "gui/gtkentry.h"
#include "gui/accelerators.h"
#include "imageio/imageio_common.h"
#include "imageio/imageio_module.h"
#include "imageio/format/imageio_format_api.h"

#include <strings.h>

DT_MODULE(1)

// clang-format off

// gui data
typedef struct pdf_t
{
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
  const dt_colorspaces_color_profile_t *profile;
  int   icc_id;
} _pdf_icc_t;

// saved params -- just there to get the sizeof() without worrying about padding, ...
typedef struct dt_imageio_pdf_params_t
{
  dt_imageio_module_data_t  global;
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
  char                    *actual_filename;
  dt_pdf_t                *pdf;
  GList                   *images;
  GList                   *icc_profiles;
  float                    page_border;
} dt_imageio_pdf_t;

// clang-format on

#ifdef USE_LUA
static int orientation_member(lua_State *L)
{
  dt_imageio_pdf_t *d = (dt_imageio_pdf_t *)lua_touserdata(L,1);
  dt_lua_orientation_t orientation;
  if(lua_gettop(L) != 3)
  {
    if(d->params.orientation == ORIENTATION_LANDSCAPE) {
      orientation = GTK_ORIENTATION_HORIZONTAL;
    } else {
      orientation = GTK_ORIENTATION_VERTICAL;
    }
    luaA_push(L,dt_lua_orientation_t,&orientation);
    return 1;
  }
  else
  {
    luaA_to(L,dt_lua_orientation_t,&orientation,3);
    if(orientation == GTK_ORIENTATION_HORIZONTAL) {
      d->params.orientation = ORIENTATION_LANDSCAPE;
    } else {
      d->params.orientation = ORIENTATION_PORTRAIT;
    }
    return 0;
  }
}
#endif // USE_LUA


void init(dt_imageio_module_format_t *self)
{
#ifdef USE_LUA
  lua_State* L = darktable.lua_state.state ;

  luaA_enum(L, _pdf_pages_t);
  luaA_enum_value_name(L, _pdf_pages_t, PAGES_ALL, "all");
  luaA_enum_value_name(L, _pdf_pages_t, PAGES_SINGLE, "single");
  luaA_enum_value_name(L, _pdf_pages_t, PAGES_CONTACT, "contact");

  luaA_enum(L, _pdf_mode_t);
  luaA_enum_value_name(L, _pdf_mode_t, MODE_NORMAL, "normal");
  luaA_enum_value_name(L, _pdf_mode_t, MODE_DRAFT, "draft");
  luaA_enum_value_name(L, _pdf_mode_t, MODE_DEBUG, "debug");

  luaA_enum(L, dt_pdf_stream_encoder_t);
  luaA_enum_value_name(L, dt_pdf_stream_encoder_t, DT_PDF_STREAM_ENCODER_ASCII_HEX, "uncompressed");
  luaA_enum_value_name(L, dt_pdf_stream_encoder_t, DT_PDF_STREAM_ENCODER_FLATE, "deflate");

  dt_lua_register_module_member_indirect(L, self, dt_imageio_pdf_t, params,dt_imageio_pdf_params_t,title, char_128);
  dt_lua_register_module_member_indirect(L, self, dt_imageio_pdf_t, params,dt_imageio_pdf_params_t,size, char_64);
  dt_lua_register_module_member_indirect(L, self, dt_imageio_pdf_t, params,dt_imageio_pdf_params_t,border, char_64);
  dt_lua_register_module_member_indirect(L, self, dt_imageio_pdf_t, params,dt_imageio_pdf_params_t,dpi, float);
  dt_lua_register_module_member_indirect(L, self, dt_imageio_pdf_t, params,dt_imageio_pdf_params_t,rotate, bool);
  dt_lua_register_module_member_indirect(L, self, dt_imageio_pdf_t, params,dt_imageio_pdf_params_t,pages, _pdf_pages_t);
  dt_lua_register_module_member_indirect(L, self, dt_imageio_pdf_t, params,dt_imageio_pdf_params_t,icc, bool);
  dt_lua_register_module_member_indirect(L, self, dt_imageio_pdf_t, params,dt_imageio_pdf_params_t,mode, _pdf_mode_t);
  dt_lua_register_module_member_indirect(L, self, dt_imageio_pdf_t, params,dt_imageio_pdf_params_t,compression, dt_pdf_stream_encoder_t);



  lua_pushcfunction(L, orientation_member);
  dt_lua_type_register_type(L, self->parameter_lua_type, "orientation");
#endif // USE_LUA
}

void cleanup(dt_imageio_module_format_t *self)
{
}

static int _paper_size(dt_imageio_pdf_params_t *d, float *page_width, float *page_height, float *page_border)
{
  float width, height, border;

  if(!dt_pdf_parse_paper_size(d->size, &width, &height))
  {
    dt_print(DT_DEBUG_ALWAYS, "[imageio_format_pdf] invalid paper size: `%s'!\n", d->size);
    dt_control_log(_("invalid paper size"));
    return 1;
  }

  if(!dt_pdf_parse_length(d->border, &border))
  {
    dt_print(DT_DEBUG_ALWAYS, "[imageio_format_pdf] invalid border size: `%s'! using 0\n", d->border);
    dt_control_log(_("invalid border size, using 0"));
//     return 1;
    border = 0.0;
  }

  if(d->orientation == ORIENTATION_LANDSCAPE)
  {
    float w = width, h = height;
    width = MAX(w, h);
    height = MIN(w, h);
  }
  else
  {
    float w = width, h = height;
    width = MIN(w, h);
    height = MAX(w, h);
  }

  *page_width = width;
  *page_height = height;
  *page_border = border;

  return 0;
}


int write_image(dt_imageio_module_data_t *data, const char *filename, const void *in,
                dt_colorspaces_color_profile_type_t over_type, const char *over_filename,
                void *exif, int exif_len, dt_imgid_t imgid, int num, int total, struct dt_dev_pixelpipe_t *pipe,
                const gboolean export_masks)
{
  dt_imageio_pdf_t *d = (dt_imageio_pdf_t *)data;

  // init the pdf. we start counting with 1.
  if(num == 1)
  {
    float page_width, page_height, page_border;
    float page_dpi = d->params.dpi;

    if(_paper_size(&d->params, &page_width, &page_height, &page_border))
      return 1;

    unsigned int compression = d->params.compression;
    compression = MIN(compression, DT_PDF_STREAM_ENCODER_FLATE);


    dt_pdf_t *pdf = dt_pdf_start(filename, page_width, page_height, page_dpi, compression);
    if(!pdf)
    {
      dt_print(DT_DEBUG_ALWAYS, "[imageio_format_pdf] could not export to file: `%s'!\n", filename);
      dt_control_log(_("could not export to file `%s'!"), filename);
      return 1;
    }

    // TODO: escape ')' and maybe also '('
    pdf->title = *d->params.title ? d->params.title : NULL;

    d->pdf = pdf;
    d->actual_filename = g_strdup(filename);
    d->page_border = page_border;
  } // init the pdf

  // add the icc profile
  int icc_id = 0;
  if(d->params.icc && d->params.mode == MODE_NORMAL)
  {
    // get the id of the profile
    const dt_colorspaces_color_profile_t *profile = dt_colorspaces_get_output_profile(imgid, over_type, over_filename);

    // look it up in the list
    for(GList *iter = d->icc_profiles; iter; iter = g_list_next(iter))
    {
      _pdf_icc_t *icc = (_pdf_icc_t *)iter->data;
      if(icc->profile == profile)
      {
        icc_id = icc->icc_id;
        break;
      }
    }
    if(icc_id == 0)
    {
      uint32_t len = 0;
      cmsSaveProfileToMem(profile->profile, NULL, &len);
      if(len > 0)
      {
        unsigned char *buf = malloc(sizeof(unsigned char) * len);
        cmsSaveProfileToMem(profile->profile, buf, &len);
        icc_id = dt_pdf_add_icc_from_data(d->pdf, buf, len);
        free(buf);
        _pdf_icc_t *icc = (_pdf_icc_t *)malloc(sizeof(_pdf_icc_t));
        icc->profile = profile;
        icc->icc_id = icc_id;
        d->icc_profiles = g_list_append(d->icc_profiles, icc);
      }
    }
  }

  void *image_data = NULL;

  // TODO
  // decide if we want to push that conversion step into the pdf lib and maybe do it on the fly while writing.
  // that would get rid of one buffer in the case of ASCII_HEX
  if(d->params.mode == MODE_NORMAL)
  {
    if(d->params.bpp == 8)
    {
      image_data = dt_alloc_align(64, (size_t)3 * data->width * data->height);
      const uint8_t *in_ptr = (const uint8_t *)in;
      uint8_t *out_ptr = (uint8_t *)image_data;
      for(int y = 0; y < data->height; y++)
      {
        for(int x = 0; x < data->width; x++, in_ptr += 4, out_ptr += 3)
          memcpy(out_ptr, in_ptr, 3);
      }
    }
    else
    {
      image_data = dt_alloc_align(64, sizeof(uint16_t) * 3 * data->width * data->height);
      const uint16_t *in_ptr = (const uint16_t *)in;
      uint16_t *out_ptr = (uint16_t *)image_data;
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

  dt_pdf_image_t *image = dt_pdf_add_image(d->pdf, image_data, d->params.global.width, d->params.global.height, d->params.bpp, icc_id, d->page_border);

  dt_free_align(image_data);

  d->images = g_list_append(d->images, image);


  // finish the pdf
  if(num == total)
  {
    int n_images = g_list_length(d->images);
    dt_pdf_page_t **pages = malloc(sizeof(dt_pdf_page_t *) * n_images);

    gboolean outline_mode = d->params.mode != MODE_NORMAL;
    gboolean show_bb = d->params.mode == MODE_DEBUG;

    // add a page for every image
    int i = 0;
    for(const GList *iter = d->images; iter; iter = g_list_next(iter))
    {
      dt_pdf_image_t *page = (dt_pdf_image_t *)iter->data;
      page->outline_mode = outline_mode;
      page->show_bb = show_bb;
      page->rotate_to_fit = d->params.rotate;
      pages[i] = dt_pdf_add_page(d->pdf, &page, 1);
      i++;
    }

    // add the contact sheet(s)
    // TODO

    dt_pdf_finish(d->pdf, pages, n_images);

    // we allocated the images and pages. the main pdf object gets free'ed in dt_pdf_finish().
    g_list_free_full(d->images, free);
    for(i = 0; i < n_images; i++) free(pages[i]);
    free(pages);
    g_free(d->actual_filename);
    g_list_free_full(d->icc_profiles, free);

    d->pdf = NULL;
    d->images = NULL;
    d->actual_filename = NULL;
    d->icc_profiles = NULL;
  } // finish the pdf

  return 0;
}

int bpp(dt_imageio_module_data_t *p)
{
  return ((dt_imageio_pdf_params_t *)p)->bpp;
}

int levels(dt_imageio_module_data_t *p)
{
  return IMAGEIO_RGB | (((dt_imageio_pdf_params_t *)p)->bpp == 8 ? IMAGEIO_INT8 : IMAGEIO_INT16);
}

const char *mime(dt_imageio_module_data_t *data)
{
  return "application/pdf";
}

const char *extension(dt_imageio_module_data_t *data)
{
  return "pdf";
}

const char *name()
{
  return _("PDF");
}

int flags(dt_imageio_module_data_t *data)
{
  return FORMAT_FLAGS_NO_TMPFILE;
}

int dimension(struct dt_imageio_module_format_t *self, dt_imageio_module_data_t *data, uint32_t *width, uint32_t *height)
{
  if(data)
  {
    dt_imageio_pdf_t *d = (dt_imageio_pdf_t *)data;

    float page_width, page_height, page_border;
    float page_dpi = d->params.dpi;

    if(_paper_size(&d->params, &page_width, &page_height, &page_border))
      return 1;

    *width = dt_pdf_point_to_pixel(page_width - 2 * page_border, page_dpi) + 0.5;
    *height = dt_pdf_point_to_pixel(page_height - 2 * page_border, page_dpi) + 0.5;

    if(d->params.rotate)
      *width = *height = MAX(*width, *height);
  }
  return 0;
}

static void size_toggle_callback(GtkWidget *widget, gpointer user_data);

// set the paper size dropdown from the UNTRANSLATED string
static void _set_paper_size(dt_imageio_module_format_t *self, const char *text)
{
  pdf_t *d = (pdf_t *)self->gui_data;

  if(text == NULL || *text == '\0')
  {
    _set_paper_size(self, dt_pdf_paper_sizes[0].name);
    return;
  }

  g_signal_handlers_block_by_func(d->size, size_toggle_callback, self);

  int pos = 0;
  for(; pos < dt_bauhaus_combobox_length(d->size); pos++)
  {
    if((pos < dt_pdf_paper_sizes_n && !strcasecmp(text, dt_pdf_paper_sizes[pos].name))
        || !strcasecmp(text, dt_bauhaus_combobox_get_entry(d->size, pos)))
      break;
  }

  if(pos < dt_bauhaus_combobox_length(d->size))
  {
    // we jumped out of the loop -> found it
    dt_bauhaus_combobox_set(d->size, pos);
    dt_conf_set_string("plugins/imageio/format/pdf/size", text);
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
      dt_conf_set_string("plugins/imageio/format/pdf/size", text);
    }
    else
    {
      dt_control_log(_("invalid paper size"));
      gchar *old_size = dt_conf_get_string("plugins/imageio/format/pdf/size");
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

static void title_changed_callback(GtkWidget *widget, gpointer user_data)
{
  dt_conf_set_string("plugins/imageio/format/pdf/title", gtk_entry_get_text(GTK_ENTRY(widget)));
}

static void border_changed_callback(GtkWidget *widget, gpointer user_data)
{
  dt_conf_set_string("plugins/imageio/format/pdf/border", gtk_entry_get_text(GTK_ENTRY(widget)));
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
  dt_conf_set_int("plugins/imageio/format/pdf/orientation", dt_bauhaus_combobox_get(widget));
}

static void dpi_changed_callback(GtkWidget *widget, gpointer user_data)
{
  dt_conf_set_float("plugins/imageio/format/pdf/dpi", gtk_spin_button_get_value(GTK_SPIN_BUTTON(widget)));
}

static void rotate_toggle_callback(GtkWidget *widget, gpointer user_data)
{
  dt_conf_set_bool("plugins/imageio/format/pdf/rotate", dt_bauhaus_combobox_get(widget) == 1);
}

static void pages_toggle_callback(GtkWidget *widget, gpointer user_data)
{
  dt_conf_set_int("plugins/imageio/format/pdf/pages", dt_bauhaus_combobox_get(widget));
}

static void icc_toggle_callback(GtkWidget *widget, gpointer user_data)
{
  dt_conf_set_bool("plugins/imageio/format/pdf/icc", dt_bauhaus_combobox_get(widget) == 1);
}

static void mode_toggle_callback(GtkWidget *widget, gpointer user_data)
{
  dt_conf_set_int("plugins/imageio/format/pdf/mode", dt_bauhaus_combobox_get(widget));
}

static void bpp_toggle_callback(GtkWidget *widget, gpointer user_data)
{
  const int sel = dt_bauhaus_combobox_get(widget);
  // we don't allow typing in that dropdown so -1 shouldn't happen, but coverity doesn't know that
  if(sel >= 0)
    dt_conf_set_int("plugins/imageio/format/pdf/bpp", _pdf_bpp[sel].bpp);
}

static void compression_toggle_callback(GtkWidget *widget, gpointer user_data)
{
  dt_conf_set_int("plugins/imageio/format/pdf/compression", dt_bauhaus_combobox_get(widget));
}

void gui_init(dt_imageio_module_format_t *self)
{
  pdf_t *d = calloc(1, sizeof(pdf_t));
  self->gui_data = (void *)d;
  self->widget = gtk_grid_new();
  GtkGrid *grid = GTK_GRID(self->widget);
  gtk_grid_set_row_spacing(grid, DT_PIXEL_APPLY_DPI(5));
  gtk_grid_set_column_spacing(grid, DT_PIXEL_APPLY_DPI(8));

  int line = 0;

  // title

  gtk_grid_attach(grid, dt_ui_label_new(_("title")), 0, ++line, 1, 1);

  d->title = GTK_ENTRY(dt_action_entry_new(DT_ACTION(self), N_("title"), G_CALLBACK(title_changed_callback), self,
                                           _("enter the title of the PDF"),
                                           dt_conf_get_string_const("plugins/imageio/format/pdf/title")));
  gtk_entry_set_placeholder_text(d->title, "untitled");
  gtk_widget_set_hexpand(GTK_WIDGET(d->title), TRUE);
  gtk_grid_attach(grid, GTK_WIDGET(d->title), 1, line, 1, 1);

  // paper size

  d->size = dt_bauhaus_combobox_new_full(DT_ACTION(self), NULL, N_("paper size"),
                                         _("paper size of the PDF\neither one from the list or "
                                           "\"<width> [unit] x <height> <unit>\"\n"
                                           "example: 210 mm x 2.97 cm"),
                                         0, size_toggle_callback, self, NULL);
  dt_bauhaus_combobox_set_editable(d->size, 1);
  for(int i = 0; dt_pdf_paper_sizes[i].name; i++)
    dt_bauhaus_combobox_add(d->size, _(dt_pdf_paper_sizes[i].name));
  gtk_grid_attach(grid, GTK_WIDGET(d->size), 0, ++line, 2, 1);
  gchar *size_str = dt_conf_get_string("plugins/imageio/format/pdf/size");
  _set_paper_size(self, size_str);
  g_free(size_str);

  // orientation

  DT_BAUHAUS_COMBOBOX_NEW_FULL(d->orientation, self, NULL, N_("page orientation"),
                               _("paper orientation of the PDF"),
                               dt_conf_get_int("plugins/imageio/format/pdf/orientation"),
                               orientation_toggle_callback, self,
                               N_("portrait"), N_("landscape"));
  gtk_grid_attach(grid, GTK_WIDGET(d->orientation), 0, ++line, 2, 1);

  // border

  gtk_grid_attach(grid, dt_ui_label_new(_("border")), 0, ++line, 1, 1);

  d->border = GTK_ENTRY(dt_action_entry_new(DT_ACTION(self), N_("border"), G_CALLBACK(border_changed_callback), self,
                                           _("empty space around the PDF\n"
                                             "format: size + unit\nexamples: 10 mm, 1 inch"),
                                           dt_conf_get_string_const("plugins/imageio/format/pdf/border")));
  gtk_entry_set_max_length(d->border, sizeof(((dt_imageio_pdf_params_t *)NULL)->border) - 1);
  gtk_entry_set_placeholder_text(d->border, "0 mm");
  gtk_grid_attach(grid, GTK_WIDGET(d->border), 1, line, 1, 1);

  // dpi

  gtk_grid_attach(grid, dt_ui_label_new(_("dpi")), 0, ++line, 1, 1);

  d->dpi = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(1, 5000, 1));
  gtk_grid_attach(grid, GTK_WIDGET(d->dpi), 1, line, 1, 1);
  gtk_widget_set_tooltip_text(GTK_WIDGET(d->dpi), _("dpi of the images inside the PDF"));
  gtk_spin_button_set_value(d->dpi, dt_conf_get_float("plugins/imageio/format/pdf/dpi"));
  g_signal_connect(G_OBJECT(d->dpi), "value-changed", G_CALLBACK(dpi_changed_callback), self);

  // rotate images yes|no

  DT_BAUHAUS_COMBOBOX_NEW_FULL(d->rotate, self, NULL, N_("rotate images"),
                               _("images can be rotated to match the PDF orientation "
                                 "to waste less space when printing"),
                               dt_conf_get_bool("plugins/imageio/format/pdf/rotate"),
                               rotate_toggle_callback, self,
                               N_("no"), N_("yes"));
  gtk_grid_attach(grid, GTK_WIDGET(d->rotate), 0, ++line, 2, 1);

  // pages all|single images|contact sheet

  DT_BAUHAUS_COMBOBOX_NEW_FULL(d->pages, self, NULL, N_("TODO: pages"),
                               _("what pages should be added to the PDF"),
                               dt_conf_get_int("plugins/imageio/format/pdf/pages"),
                               pages_toggle_callback, self,
                               N_("all"), N_("single images"), N_("contact sheet"));
  gtk_grid_attach(grid, GTK_WIDGET(d->pages), 0, ++line, 2, 1);
  gtk_widget_set_no_show_all(d->pages, TRUE); // TODO

  // embedded icc profile yes|no

  DT_BAUHAUS_COMBOBOX_NEW_FULL(d->icc, self, NULL, N_("embed ICC profiles"),
                               _("images can be tagged with their ICC profile"),
                               dt_conf_get_bool("plugins/imageio/format/pdf/icc"),
                               icc_toggle_callback, self,
                               N_("no"), N_("yes"));
  gtk_grid_attach(grid, GTK_WIDGET(d->icc), 0, ++line, 2, 1);

  // bpp

  d->bpp = dt_bauhaus_combobox_new_action(DT_ACTION(self));
  dt_bauhaus_widget_set_label(d->bpp, NULL, N_("bit depth"));
  int sel = 0;
  int bpp = dt_conf_get_int("plugins/imageio/format/pdf/bpp");
  for(int i = 0; _pdf_bpp[i].name; i++)
  {
    dt_bauhaus_combobox_add(d->bpp, _(_pdf_bpp[i].name));
    if(_pdf_bpp[i].bpp == bpp) sel = i;
  }
  gtk_grid_attach(grid, GTK_WIDGET(d->bpp), 0, ++line, 2, 1);
  g_signal_connect(G_OBJECT(d->bpp), "value-changed", G_CALLBACK(bpp_toggle_callback), self);
  gtk_widget_set_tooltip_text(d->bpp, _("bits per channel of the embedded images"));
  dt_bauhaus_combobox_set(d->bpp, sel);

  // compression

  DT_BAUHAUS_COMBOBOX_NEW_FULL(d->compression, self, NULL, N_("compression"),
                               _("method used for image compression\n"
                                 "uncompressed -- fast but big files\n"
                                 "deflate -- smaller files but slower"),
                               dt_conf_get_int("plugins/imageio/format/pdf/compression"),
                               compression_toggle_callback, self,
                               N_("uncompressed"), N_("deflate"));
  gtk_grid_attach(grid, GTK_WIDGET(d->compression), 0, ++line, 2, 1);

  // image mode normal|draft|debug

  DT_BAUHAUS_COMBOBOX_NEW_FULL(d->mode, self, NULL, N_("image mode"),
                               _("normal -- just put the images into the PDF\n"
                                 "draft -- images are replaced with boxes\n"
                                 "debug -- only show the outlines and bounding boxes"),
                               dt_conf_get_int("plugins/imageio/format/pdf/mode"),
                               mode_toggle_callback, self,
                               N_("normal"), N_("draft"), N_("debug"));
  gtk_grid_attach(grid, GTK_WIDGET(d->mode), 0, ++line, 2, 1);
}

void gui_cleanup(dt_imageio_module_format_t *self)
{
  free(self->gui_data);
}

void gui_reset(dt_imageio_module_format_t *self)
{
  pdf_t *d = (pdf_t *)self->gui_data;

  dpi_changed_callback(GTK_WIDGET(d->dpi), self);
  icc_toggle_callback(GTK_WIDGET(d->icc), self);
  mode_toggle_callback(GTK_WIDGET(d->mode), self);
  orientation_toggle_callback(GTK_WIDGET(d->orientation), self);
  pages_toggle_callback(GTK_WIDGET(d->pages), self);
  rotate_toggle_callback(GTK_WIDGET(d->rotate), self);
  size_toggle_callback(GTK_WIDGET(d->size), self);
  title_changed_callback(GTK_WIDGET(d->title), self);
  bpp_toggle_callback(GTK_WIDGET(d->bpp), self);
  compression_toggle_callback(GTK_WIDGET(d->compression), self);
}

size_t params_size(dt_imageio_module_format_t *self)
{
  return sizeof(dt_imageio_pdf_params_t);
}

void *get_params(dt_imageio_module_format_t *self)
{
  dt_imageio_pdf_t *d = (dt_imageio_pdf_t *)calloc(1, sizeof(dt_imageio_pdf_t));

  if(d)
  {
    const char *text = dt_conf_get_string_const("plugins/imageio/format/pdf/title");
    g_strlcpy(d->params.title, text, sizeof(d->params.title));

    text = dt_conf_get_string_const("plugins/imageio/format/pdf/border");
    g_strlcpy(d->params.border, text, sizeof(d->params.border));

    text = dt_conf_get_string_const("plugins/imageio/format/pdf/size");
    g_strlcpy(d->params.size, text, sizeof(d->params.size));

    d->params.bpp = dt_conf_get_int("plugins/imageio/format/pdf/bpp");
    d->params.compression = dt_conf_get_int("plugins/imageio/format/pdf/compression");
    d->params.dpi = dt_conf_get_float("plugins/imageio/format/pdf/dpi");
    d->params.icc = dt_conf_get_bool("plugins/imageio/format/pdf/icc");
    d->params.mode = dt_conf_get_int("plugins/imageio/format/pdf/mode");
    d->params.orientation = dt_conf_get_int("plugins/imageio/format/pdf/orientation");
    d->params.pages = dt_conf_get_int("plugins/imageio/format/pdf/pages");
    d->params.rotate = dt_conf_get_bool("plugins/imageio/format/pdf/rotate");
  }

  return d;
}

// in normal operations we free these after exporting the last image, but when an export
// gets cancelled that last image doesn't get exported, so we have to take care of it here.
void free_params(dt_imageio_module_format_t *self, dt_imageio_module_data_t *params)
{
  dt_imageio_pdf_t *d = (dt_imageio_pdf_t *)params;

  if(d->pdf)
    dt_pdf_finish(d->pdf, NULL, 0);

  g_list_free_full(d->images, free);

  if(d->actual_filename)
  {
    g_unlink(d->actual_filename); // no need to leave broken files on disk
    g_free(d->actual_filename);
  }

  g_list_free_full(d->icc_profiles, free);

  d->pdf = NULL;
  d->images = NULL;
  d->actual_filename = NULL;
  d->icc_profiles = NULL;

  free(params);
}

int set_params(dt_imageio_module_format_t *self, const void *params, const int size)
{
  if(size != self->params_size(self)) return 1;
  const dt_imageio_pdf_t *d = (dt_imageio_pdf_t *)params;
  pdf_t *g = (pdf_t *)self->gui_data;

  for(int i = 0; _pdf_bpp[i].name; i++)
  {
    if(_pdf_bpp[i].bpp == d->params.bpp)
      dt_bauhaus_combobox_set(g->bpp, i);
  }

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

  dt_conf_set_string("plugins/imageio/format/pdf/title", d->params.title);
  dt_conf_set_string("plugins/imageio/format/pdf/border", d->params.border);
  dt_conf_set_int("plugins/imageio/format/pdf/bpp", d->params.bpp);
  dt_conf_set_int("plugins/imageio/format/pdf/compression", d->params.compression);
  dt_conf_set_float("plugins/imageio/format/pdf/dpi", d->params.dpi);
  dt_conf_set_bool("plugins/imageio/format/pdf/icc", d->params.icc);
  dt_conf_set_int("plugins/imageio/format/pdf/mode", d->params.mode);
  dt_conf_set_int("plugins/imageio/format/pdf/orientation", d->params.orientation);
  dt_conf_set_int("plugins/imageio/format/pdf/pages", d->params.pages);
  dt_conf_set_bool("plugins/imageio/format/pdf/rotate", d->params.rotate);

  return 0;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
