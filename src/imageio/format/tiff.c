/*
    This file is part of darktable,
    copyright (c) 2010--2011 Henrik Andersson and johannes hanika

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
#include <memory.h>
#include <stdlib.h>
#include <stdio.h>
#include <stddef.h>
#include <inttypes.h>
#include <tiffio.h>
#include "common/darktable.h"
#include "common/imageio_module.h"
#include "common/imageio.h"
#include "common/exif.h"
#include "common/colorspaces.h"
#include "control/conf.h"
#include "common/imageio_format.h"
#define DT_TIFFIO_STRIPE 64

DT_MODULE(1)

typedef struct dt_imageio_tiff_t
{
  int max_width, max_height;
  int width, height;
  char style[128];
  int bpp;
  int compress;
  TIFF *handle;
}
dt_imageio_tiff_t;

typedef struct dt_imageio_tiff_gui_t
{
  GtkComboBox *bpp;
  GtkComboBox *compress;
}
dt_imageio_tiff_gui_t;


int write_image (dt_imageio_module_data_t *d_tmp, const char *filename, const void *in_void, void *exif, int exif_len, int imgid)
{
  dt_imageio_tiff_t *d=(dt_imageio_tiff_t*)d_tmp;

  uint8_t* profile = NULL;
  uint32_t profile_len = 0;

  TIFF* tif = NULL;

  void* rowdata = NULL;
  uint32_t rowsize = 0;
  uint32_t stripesize = 0;
  uint32_t stripe = 0;

  int rc = 1; // default to error

  if(imgid > 0)
  {
    cmsHPROFILE out_profile = dt_colorspaces_create_output_profile(imgid);
    cmsSaveProfileToMem(out_profile, 0, &profile_len);
    if (profile_len > 0)
    {
      profile = malloc(profile_len);
      if (!profile)
      {
        rc = 1;
        goto exit;
      }
      cmsSaveProfileToMem(out_profile, profile, &profile_len);
    }
    dt_colorspaces_cleanup_profile(out_profile);
  }

  // Create little endian tiff image
  tif = TIFFOpen(filename,"wl");
  if (!tif)
  {
    rc = 1;
    goto exit;
  }

  // http://partners.adobe.com/public/developer/en/tiff/TIFFphotoshop.pdf (dated 2002)
  // "A proprietary ZIP/Flate compression code (0x80b2) has been used by some"
  // "software vendors. This code should be considered obsolete. We recommend"
  // "that TIFF implentations recognize and read the obsolete code but only"
  // "write the official compression code (0x0008)."
  // http://www.awaresystems.be/imaging/tiff/tifftags/compression.html
  // http://www.awaresystems.be/imaging/tiff/tifftags/predictor.html
  if (d->compress == 1)
  {
    TIFFSetField(tif, TIFFTAG_COMPRESSION, (uint16_t)COMPRESSION_ADOBE_DEFLATE);
    TIFFSetField(tif, TIFFTAG_PREDICTOR, (uint16_t)1);
    TIFFSetField(tif, TIFFTAG_ZIPQUALITY, (uint16_t)9);
  }
  else if (d->compress == 2)
  {
    TIFFSetField(tif, TIFFTAG_COMPRESSION, (uint16_t)COMPRESSION_ADOBE_DEFLATE);
    TIFFSetField(tif, TIFFTAG_PREDICTOR, (uint16_t)2);
    TIFFSetField(tif, TIFFTAG_ZIPQUALITY, (uint16_t)9);
  }
  else if (d->compress == 3)
  {
    TIFFSetField(tif, TIFFTAG_COMPRESSION, (uint16_t)COMPRESSION_ADOBE_DEFLATE);
    if (d->bpp == 32)
      TIFFSetField(tif, TIFFTAG_PREDICTOR, (uint16_t)3);
    else
      TIFFSetField(tif, TIFFTAG_PREDICTOR, (uint16_t)2);
    TIFFSetField(tif, TIFFTAG_ZIPQUALITY, (uint16_t)9);
  }
  else // (d->compress == 0)
  {
    TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_NONE);
  }

  TIFFSetField(tif, TIFFTAG_FILLORDER, (uint16_t)FILLORDER_MSB2LSB);
  if (profile != NULL)
  {
    TIFFSetField(tif, TIFFTAG_ICCPROFILE, (uint32_t)profile_len, profile);
  }
  TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, (uint16_t)3);
  TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, (uint16_t)d->bpp);
  TIFFSetField(tif, TIFFTAG_SAMPLEFORMAT, (uint16_t)(d->bpp == 32 ? SAMPLEFORMAT_IEEEFP : SAMPLEFORMAT_UINT));
  TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, (uint32_t)d->width);
  TIFFSetField(tif, TIFFTAG_IMAGELENGTH, (uint32_t)d->height);
  TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, (uint16_t)PHOTOMETRIC_RGB);
  TIFFSetField(tif, TIFFTAG_PLANARCONFIG, (uint16_t)PLANARCONFIG_CONTIG);
  TIFFSetField(tif, TIFFTAG_RESOLUTIONUNIT, (uint16_t)RESUNIT_INCH);
  TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, (uint32_t)DT_TIFFIO_STRIPE);
  TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, (uint16_t)3);
  TIFFSetField(tif, TIFFTAG_XRESOLUTION, 300.f);
  TIFFSetField(tif, TIFFTAG_YRESOLUTION, 300.f);

  rowsize = (d->width*3) * d->bpp / 8;
  stripesize = rowsize * DT_TIFFIO_STRIPE;
  stripe = 0;

  rowdata = malloc(stripesize);
  if (!rowdata)
  {
    rc = 1;
    goto exit;
  }

  if (d->bpp == 32)
  {
    float* in = (float*)in_void;
    float* wdata = rowdata;

    for (int y = 0; y < d->height; y++)
    {
      for (int x = 0; x < d->width; x++)
      {
        wdata[0] = in[x * 4 + 0];
        wdata[1] = in[x * 4 + 1];
        wdata[2] = in[x * 4 + 2];
        wdata += 3;
      }

      if ((uintptr_t)wdata - (uintptr_t)rowdata == (uintptr_t)stripesize)
      {
        TIFFWriteEncodedStrip(tif, stripe++, rowdata, (size_t)(rowsize * DT_TIFFIO_STRIPE));
        wdata = rowdata;
      }

      in += d->width * 4;
    }
    if ((uintptr_t)wdata - (uintptr_t)rowdata != (uintptr_t)stripesize)
    {
      TIFFWriteEncodedStrip(tif, stripe++, rowdata, (size_t)((uintptr_t)wdata - (uintptr_t)rowdata));
      wdata = rowdata;
    }
  }
  else if (d->bpp == 16)
  {
    uint16_t* in = (uint16_t*)in_void;
    uint16_t* wdata = rowdata;

    for (int y = 0; y < d->height; y++)
    {
      for(int x=0; x<d->width; x++)
      {
        wdata[0] = in[4*x + 0];
        wdata[1] = in[4*x + 1];
        wdata[2] = in[4*x + 2];
        wdata += 3;
      }

      if((uintptr_t)wdata - (uintptr_t)rowdata == (uintptr_t)stripesize)
      {
        TIFFWriteEncodedStrip(tif, stripe++, rowdata, (size_t)(rowsize * DT_TIFFIO_STRIPE));
        wdata = rowdata;
      }

      in += d->width*4;
    }
    if ((uintptr_t)wdata - (uintptr_t)rowdata != (uintptr_t)stripesize)
    {
      TIFFWriteEncodedStrip(tif, stripe, rowdata, (size_t)((uintptr_t)wdata - (uintptr_t)rowdata));
    }
  }
  else
  {
    uint8_t* in = (uint8_t*)in_void;
    uint8_t* wdata = rowdata;

    for (int y = 0; y < d->height; y++)
    {
      for(int x=0; x<d->width; x++)
      {
        wdata[0] = in[4*x + 0];
        wdata[1] = in[4*x + 1];
        wdata[2] = in[4*x + 2];
        wdata += 3;
      }

      if((uintptr_t)wdata - (uintptr_t)rowdata == (uintptr_t)stripesize)
      {
        TIFFWriteEncodedStrip(tif, stripe++, rowdata, (size_t)(rowsize * DT_TIFFIO_STRIPE));
        wdata = rowdata;
      }

      in += d->width*4;
    }
    if((uintptr_t)wdata - (uintptr_t)rowdata != (uintptr_t)stripesize)
    {
      TIFFWriteEncodedStrip(tif, stripe, rowdata, (size_t)((uintptr_t)wdata - (uintptr_t)rowdata));
    }
  }

  // success
  rc = 0;

exit:
  // close the file before adding exif data
  if (tif)
  {
    TIFFClose(tif);
    tif = NULL;
  }
  if(!rc && exif)
  {
    rc = dt_exif_write_blob(exif,exif_len,filename);
    // Until we get symbolic error status codes, if rc is 1, return 0
    rc = (rc == 1) ? 0 : 1;
  }
  if (profile)
  {
    free(profile);
    profile = NULL;
  }
  if (rowdata)
  {
    free(rowdata);
    rowdata = NULL;
  }

  return rc;
}

#if 0
int dt_imageio_tiff_read_header(const char *filename, dt_imageio_tiff_t *tiff)
{
  tiff->handle = TIFFOpen(filename, "rl");
  if( tiff->handle )
  {
    TIFFGetField(tiff->handle, TIFFTAG_IMAGEWIDTH, &tiff->width);
    TIFFGetField(tiff->handle, TIFFTAG_IMAGELENGTH, &tiff->height);
  }
  return 1;
}

int dt_imageio_tiff_read(dt_imageio_tiff_t *tiff, uint8_t *out)
{
  TIFFClose(tiff->handle);
  return 1;
}
#endif

size_t
params_size(dt_imageio_module_format_t *self)
{
  return sizeof(dt_imageio_tiff_t) - sizeof(TIFF*);
}

void*
get_params(dt_imageio_module_format_t *self)
{
  dt_imageio_tiff_t *d = (dt_imageio_tiff_t *)malloc(sizeof(dt_imageio_tiff_t));
  memset(d, 0, sizeof(dt_imageio_tiff_t));
  d->bpp = dt_conf_get_int("plugins/imageio/format/tiff/bpp");
  if (d->bpp == 16) d->bpp = 16;
  else if(d->bpp == 32) d->bpp = 32;
  else d->bpp = 8;
  d->compress = dt_conf_get_int("plugins/imageio/format/tiff/compress");
  return d;
}

void
free_params(dt_imageio_module_format_t *self, dt_imageio_module_data_t *params)
{
  free(params);
}

int
set_params(dt_imageio_module_format_t *self, const void *params, const int size)
{
  if(size != self->params_size(self)) return 1;
  dt_imageio_tiff_t *d = (dt_imageio_tiff_t *)params;
  dt_imageio_tiff_gui_t *g = (dt_imageio_tiff_gui_t *)self->gui_data;

  if (d->bpp == 16)
    gtk_combo_box_set_active(g->bpp, 1);
  else if (d->bpp == 32)
    gtk_combo_box_set_active(g->bpp, 2);
  else // (d->bpp == 8)
    gtk_combo_box_set_active(g->bpp, 0);

  gtk_combo_box_set_active(g->compress, d->compress);

  return 0;
}

int bpp(dt_imageio_module_data_t *p)
{
  return ((dt_imageio_tiff_t*)p)->bpp;
}

int compress(dt_imageio_module_data_t *p)
{
  return ((dt_imageio_tiff_t*)p)->compress;
}

int levels(dt_imageio_module_data_t *p)
{
  int ret = IMAGEIO_RGB;

  if (((dt_imageio_tiff_t*)p)->bpp == 8)
    ret |= IMAGEIO_INT8;
  else if (((dt_imageio_tiff_t*)p)->bpp == 16)
    ret |= IMAGEIO_INT16;
  else if (((dt_imageio_tiff_t*)p)->bpp == 32)
    ret |= IMAGEIO_FLOAT;

  return ret;
}

const char*
mime(dt_imageio_module_data_t *data)
{
  return "image/tiff";
}

const char*
extension(dt_imageio_module_data_t *data)
{
  return "tif";
}

const char*
name ()
{
  return _("TIFF (8/16/32-bit)");
}

static void
bpp_combobox_changed(GtkComboBox *widget, gpointer user_data)
{
  int bpp = gtk_combo_box_get_active(widget);

  if (bpp == 1)
    dt_conf_set_int("plugins/imageio/format/tiff/bpp", 16);
  else if (bpp == 2)
    dt_conf_set_int("plugins/imageio/format/tiff/bpp", 32);
  else // (bpp == 0)
    dt_conf_set_int("plugins/imageio/format/tiff/bpp", 8);
}

static void
compress_combobox_changed(GtkComboBox *widget, gpointer user_data)
{
  int compress = gtk_combo_box_get_active(widget);
  dt_conf_set_int("plugins/imageio/format/tiff/compress", compress);
}

void init(dt_imageio_module_format_t *self)
{
#ifdef USE_LUA
  dt_lua_register_module_member(darktable.lua_state.state,self,dt_imageio_tiff_t,bpp,int);
#endif
}
void cleanup(dt_imageio_module_format_t *self) {}

// TODO: some quality/compression stuff?
void gui_init (dt_imageio_module_format_t *self)
{
  dt_imageio_tiff_gui_t *gui = (dt_imageio_tiff_gui_t *)malloc(sizeof(dt_imageio_tiff_gui_t));
  self->gui_data = (void *)gui;

  int bpp = dt_conf_get_int("plugins/imageio/format/tiff/bpp");
  
  int compress = dt_conf_get_int("plugins/imageio/format/tiff/compress");

  self->widget = gtk_vbox_new(TRUE, 5);

  GtkComboBoxText *bpp_combo = GTK_COMBO_BOX_TEXT(gtk_combo_box_text_new());
  gui->bpp = GTK_COMBO_BOX(bpp_combo);
  gtk_combo_box_text_append_text(bpp_combo, _("8 bit"));
  gtk_combo_box_text_append_text(bpp_combo, _("16 bit"));
  gtk_combo_box_text_append_text(bpp_combo, _("32 bit (float)"));
  if (bpp == 16)
    gtk_combo_box_set_active(GTK_COMBO_BOX(bpp_combo), 1);
  else if (bpp == 32)
    gtk_combo_box_set_active(GTK_COMBO_BOX(bpp_combo), 2);
  else // (bpp == 8)
    gtk_combo_box_set_active(GTK_COMBO_BOX(bpp_combo), 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(bpp_combo), TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(bpp_combo), "changed", G_CALLBACK(bpp_combobox_changed), NULL);

  GtkComboBoxText *compress_combo = GTK_COMBO_BOX_TEXT(gtk_combo_box_text_new());
  gui->compress = GTK_COMBO_BOX(compress_combo);
  gtk_combo_box_text_append_text(compress_combo, _("uncompressed"));
  gtk_combo_box_text_append_text(compress_combo, _("deflate"));
  gtk_combo_box_text_append_text(compress_combo, _("deflate with predictor"));
  gtk_combo_box_text_append_text(compress_combo, _("deflate with predictor (float)"));
  gtk_combo_box_set_active(GTK_COMBO_BOX(compress_combo), compress);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(compress_combo), TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(compress_combo), "changed", G_CALLBACK(compress_combobox_changed), NULL);
}

void gui_cleanup (dt_imageio_module_format_t *self)
{
  free(self->gui_data);
}

void gui_reset   (dt_imageio_module_format_t *self)
{
  // TODO: reset to conf? reset to factory defaults?
}

int flags(dt_imageio_module_data_t *data)
{
  return FORMAT_FLAGS_SUPPORT_XMP;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
