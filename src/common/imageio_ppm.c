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
#include <glib/gstdio.h>
#include "imageio_ppm.h"
#define DT_TIFFIO_STRIPE 20

int dt_imageio_ppm_write_16(const char *filename, const uint16_t *in, const int width, const int height)
{
  int status=0;
  uint16_t *row=(uint16_t*)in;
  uint32_t rowlength=(width*3);
  uint16_t swappedrow[rowlength];
  FILE *f = fopen(filename, "wb");
  if(f)
  {
    (void)fprintf(f, "P6\n%d %d\n65535\n", width, height);
    for(int y=0;y<height;y++)
    {
      for(int x=0;x<3*width;x++) swappedrow[x] = (0xff00 & (row[x]<<8))|(row[x]>>8);;
      int cnt = fwrite(swappedrow, (width*3)*sizeof(uint16_t), 1, f);
      if(cnt != 1) break;
      row+=rowlength;
    }
    fclose(f);
    status=0;
  }
  return status;
}
