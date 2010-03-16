/*
    This file is part of darktable,
    copyright (c) 2010 Henrik Andersson.

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
#include "imageio_tiff.h"
#include "common/exif.h"
#define DT_TIFFIO_STRIPE 20

int dt_imageio_tiff_write_16(const char *filename, const uint16_t *in, const int width, const int height, void *exif, int exif_len)
{
  TIFF *tif=TIFFOpen(filename,"w");
  
  TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, width);
  TIFFSetField(tif, TIFFTAG_IMAGELENGTH, height);
  TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 16);
  TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 3);
  TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, DT_TIFFIO_STRIPE);
  TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_DEFLATE);
  TIFFSetField(tif, TIFFTAG_ZIPQUALITY, 9);
  TIFFSetField(tif, TIFFTAG_PREDICTOR, 2);
  TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
  TIFFSetField(tif, TIFFTAG_FILLORDER, FILLORDER_MSB2LSB);
  TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
  TIFFSetField(tif, TIFFTAG_XRESOLUTION, 150.0);
  TIFFSetField(tif, TIFFTAG_YRESOLUTION, 150.0);
  TIFFSetField(tif, TIFFTAG_RESOLUTIONUNIT, RESUNIT_INCH);

  uint32_t rowsize=(width*3)*sizeof(uint16_t);
  uint32_t stripesize=rowsize*DT_TIFFIO_STRIPE;
  const uint8_t *in8=(const uint8_t *)in;
  uint8_t *stripedata=(uint8_t*)in;
  uint32_t stripe=0;
  uint32_t insize=((width*height)*3)*sizeof(uint16_t);
  while(stripedata<(in8+insize)-(stripesize)) {
    TIFFWriteEncodedStrip(tif,stripe++,stripedata,stripesize);
    stripedata+=stripesize;  
  }
  uint8_t *last_stripe=(uint8_t *)malloc(stripesize);
  memset(last_stripe,0,stripesize);
  memcpy(last_stripe,stripedata,(in8+insize)-stripedata);
  TIFFWriteEncodedStrip(tif,stripe++,last_stripe,stripesize);
  free(last_stripe);
  TIFFClose(tif);
  
  if(exif)
    dt_exif_write_blob(exif,exif_len,filename);
  
  return 1;
}

int dt_imageio_tiff_write_8(const char *filename, const uint8_t *in, const int width, const int height, void *exif, int exif_len)
{
  TIFF *tif=TIFFOpen(filename,"w");
  
  TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, width);
  TIFFSetField(tif, TIFFTAG_IMAGELENGTH, height);
  TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 8);
  TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 3);
  TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, DT_TIFFIO_STRIPE);
  TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_DEFLATE);
  TIFFSetField(tif, TIFFTAG_ZIPQUALITY, 9);
  TIFFSetField(tif, TIFFTAG_PREDICTOR, 2);
  TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
  TIFFSetField(tif, TIFFTAG_FILLORDER, FILLORDER_MSB2LSB);
  TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
  TIFFSetField(tif, TIFFTAG_XRESOLUTION, 150.0);
  TIFFSetField(tif, TIFFTAG_YRESOLUTION, 150.0);
  TIFFSetField(tif, TIFFTAG_RESOLUTIONUNIT, RESUNIT_INCH);
 
  uint32_t rowsize=width*3;
  uint32_t stripesize=rowsize*DT_TIFFIO_STRIPE;
  uint8_t *rowdata=malloc(stripesize);
  uint8_t *wdata=rowdata;
  uint32_t stripe=0;
  
  for (int y = 0; y < height; y++)
  {
    for(int x=0;x<width;x++) 
      for(int k=0;k<3;k++) 
      {
        (wdata)[0] = in[4*width*y + 4*x + k];
          wdata++;
      }
    if((wdata-stripesize)==rowdata)
    {
      TIFFWriteEncodedStrip(tif,stripe++,rowdata,rowsize*DT_TIFFIO_STRIPE);
      wdata=rowdata;
    }
  }	
  TIFFClose(tif);
  
  if(exif)
    dt_exif_write_blob(exif,exif_len,filename);
  
  return 1;
}

int dt_imageio_tiff_read_header(const char *filename, dt_imageio_tiff_t *tiff)
{
  tiff->handle = TIFFOpen(filename, "r");
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