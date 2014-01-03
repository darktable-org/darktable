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
  TIFF *handle;
}
dt_imageio_tiff_t;

typedef struct dt_imageio_tiff_gui_t
{
  GtkToggleButton *b8, *b16;
}
dt_imageio_tiff_gui_t;


int write_image (dt_imageio_module_data_t *d_tmp, const char *filename, const void *in_void, void *exif, int exif_len, int imgid)
{
  dt_imageio_tiff_t *d=(dt_imageio_tiff_t*)d_tmp;
  // Fetch colorprofile into buffer if wanted
  uint8_t *profile = NULL;
  uint32_t profile_len = 0;
  int rc = 0;

  if(imgid > 0)
  {
    cmsHPROFILE out_profile = dt_colorspaces_create_output_profile(imgid);
    cmsSaveProfileToMem(out_profile, 0, &profile_len);
    if (profile_len > 0)
    {
      profile=malloc(profile_len);
      cmsSaveProfileToMem(out_profile, profile, &profile_len);
    }
    dt_colorspaces_cleanup_profile(out_profile);
  }

  // Create tiff image
  TIFF *tif=TIFFOpen(filename,"wb");
  if(d->bpp == 8) TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 8);
  else            TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 16);
  TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_DEFLATE);
  TIFFSetField(tif, TIFFTAG_FILLORDER, FILLORDER_MSB2LSB);
  if(profile!=NULL)
    TIFFSetField(tif, TIFFTAG_ICCPROFILE, profile_len, profile);
  TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, d->width);
  TIFFSetField(tif, TIFFTAG_IMAGELENGTH, d->height);
  TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
  TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
  TIFFSetField(tif, TIFFTAG_PREDICTOR, 1);		// Reference www.awaresystems.be/imaging/tiff/tifftags/predictor.html
  TIFFSetField(tif, TIFFTAG_RESOLUTIONUNIT, RESUNIT_INCH);
  TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, DT_TIFFIO_STRIPE);
  TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 3);
  TIFFSetField(tif, TIFFTAG_XRESOLUTION, 300.0);
  TIFFSetField(tif, TIFFTAG_YRESOLUTION, 300.0);
  TIFFSetField(tif, TIFFTAG_ZIPQUALITY, 9);

  const uint8_t  *in8 =(const uint8_t  *)in_void;
  const uint16_t *in16=(const uint16_t *)in_void;
  if(d->bpp == 16)
  {
    uint32_t rowsize=(d->width*3)*sizeof(uint16_t);
    uint32_t stripesize=rowsize*DT_TIFFIO_STRIPE;
    uint16_t *rowdata = (uint16_t *)malloc(stripesize);
    uint16_t *wdata = rowdata;
    uint32_t stripe=0;
    // uint32_t insize=((d->width*d->height)*3)*sizeof(uint16_t);
    // while(stripedata<(in8+insize)-(stripesize)) {
    // TIFFWriteEncodedStrip(tif,stripe++,stripedata,stripesize);
    // stripedata+=stripesize;
    // }
    for (int y = 0; y < d->height; y++)
    {
      for(int x=0; x<d->width; x++)
        for(int k=0; k<3; k++)
        {
          (wdata)[0] = in16[4*d->width*y + 4*x + k];
          wdata++;
        }
      if((wdata-stripesize/sizeof(uint16_t))==rowdata)
      {
        TIFFWriteEncodedStrip(tif,stripe++,rowdata,rowsize*DT_TIFFIO_STRIPE);
        wdata=rowdata;
      }
    }
    if((wdata-stripesize/sizeof(uint16_t))!=rowdata)
      TIFFWriteEncodedStrip(tif,stripe,rowdata,(wdata-rowdata)*sizeof(uint16_t));
    TIFFClose(tif);
    free(rowdata);
  }
  else
  {
    uint32_t rowsize=(d->width*3)*sizeof(uint8_t);
    uint32_t stripesize=rowsize*DT_TIFFIO_STRIPE;
    uint8_t *rowdata = (uint8_t *)malloc(stripesize);
    uint8_t *wdata = rowdata;
    uint32_t stripe=0;

    for (int y = 0; y < d->height; y++)
    {
      for(int x=0; x<d->width; x++)
        for(int k=0; k<3; k++)
        {
          (wdata)[0] = in8[4*d->width*y + 4*x + k];
          wdata++;
        }
      if((wdata-stripesize)==rowdata)
      {
        TIFFWriteEncodedStrip(tif,stripe++,rowdata,rowsize*DT_TIFFIO_STRIPE);
        wdata=rowdata;
      }
    }
    if((wdata-stripesize)!=rowdata)
      TIFFWriteEncodedStrip(tif,stripe,rowdata,wdata-rowdata);
    TIFFClose(tif);
    free(rowdata);
  }

  if(exif)
    rc = dt_exif_write_blob(exif,exif_len,filename);

  free(profile);

  /*
   * Until we get symbolic error status codes, if rc is 1, return 0.
   */
  return ((rc == 1) ? 0 : 1);
}

#if 0
int dt_imageio_tiff_read_header(const char *filename, dt_imageio_tiff_t *tiff)
{
  tiff->handle = TIFFOpen(filename, "rb");
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
  if(d->bpp < 12) d->bpp = 8;
  else            d->bpp = 16;
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
  if(d->bpp < 12) gtk_toggle_button_set_active(g->b8, TRUE);
  else            gtk_toggle_button_set_active(g->b16, TRUE);
  dt_conf_set_int("plugins/imageio/format/tiff/bpp", d->bpp);
  return 0;
}

int bpp(dt_imageio_module_data_t *p)
{
  return ((dt_imageio_tiff_t*)p)->bpp;
}

int levels(dt_imageio_module_data_t *p)
{
  return IMAGEIO_RGB | (((dt_imageio_tiff_t*)p)->bpp == 8 ? IMAGEIO_INT8 : IMAGEIO_INT16);
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
  return _("TIFF (8/16-bit)");
}

static void
radiobutton_changed (GtkRadioButton *radiobutton, gpointer user_data)
{
  int bpp = GPOINTER_TO_INT(user_data);
  if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(radiobutton)))
    dt_conf_set_int("plugins/imageio/format/tiff/bpp", bpp);
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
  self->widget = gtk_hbox_new(TRUE, 5);
  GtkWidget *radiobutton = gtk_radio_button_new_with_label(NULL, _("8-bit"));
  gui->b8 = GTK_TOGGLE_BUTTON(radiobutton);
  gtk_box_pack_start(GTK_BOX(self->widget), radiobutton, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(radiobutton), "toggled", G_CALLBACK(radiobutton_changed), GINT_TO_POINTER(8));
  if(bpp < 12) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(radiobutton), TRUE);
  radiobutton = gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(radiobutton), _("16-bit"));
  gui->b16 = GTK_TOGGLE_BUTTON(radiobutton);
  gtk_box_pack_start(GTK_BOX(self->widget), radiobutton, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(radiobutton), "toggled", G_CALLBACK(radiobutton_changed), GINT_TO_POINTER(16));
  if(bpp >= 12) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(radiobutton), TRUE);
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
