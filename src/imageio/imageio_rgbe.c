/*
    This file is part of darktable,
    Copyright (C) 2009-2023 darktable developers.

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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "common/matrices.h"
#include "develop/imageop.h"         // for IOP_CS_RGB
#include "imageio/imageio_rgbe.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/* THIS CODE CARRIES NO GUARANTEE OF USABILITY OR FITNESS FOR ANY PURPOSE.
 * WHILE THE AUTHORS HAVE TRIED TO ENSURE THE PROGRAM WORKS CORRECTLY,
 * IT IS STRICTLY USE AT YOUR OWN RISK.
 *
 * based on code written by Greg Ward */


typedef struct
{
  int valid;            /* indicate which fields are valid */
  char programtype[16]; /* listed at beginning of file to identify it
                         * after "#?".  defaults to "RGBE" */
  float gamma;          /* image has already been gamma corrected with
                         * given gamma.  defaults to 1.0 (no correction) */
  float exposure;       /* a value of 1.0 in an image corresponds to
                         * <exposure> watts/steradian/m^2.
                         * defaults to 1.0 */
  float primaries[8];   /* xy for R, G an B primaries plus white point
                         * defaults to:
                         * 0.640 0.330 0.290 0.600 0.150 0.060 0.333 0.333 */
} rgbe_header_info;

/* flags indicating which fields in an rgbe_header_info are valid */
#define RGBE_VALID_PROGRAMTYPE 0x01
#define RGBE_VALID_GAMMA 0x02
#define RGBE_VALID_EXPOSURE 0x04

/* return codes for rgbe routines */
#define RGBE_RETURN_SUCCESS 0
#define RGBE_RETURN_FAILURE -1

#define RGBE_DATA_RED 0
#define RGBE_DATA_GREEN 1
#define RGBE_DATA_BLUE 2
/* number of floats per pixel */
#define RGBE_DATA_SIZE 3

enum rgbe_error_codes
{
  rgbe_read_error,
  rgbe_write_error,
  rgbe_format_error,
  rgbe_memory_error,
};

/* default error routine.  change this to change error handling */
static int rgbe_error(int rgbe_error_code, char *msg)
{
  switch(rgbe_error_code)
  {
    case rgbe_read_error:
      perror("RGBE read error");
      break;
    case rgbe_write_error:
      perror("RGBE write error");
      break;
    case rgbe_format_error:
      dt_print(DT_DEBUG_ALWAYS, "RGBE bad file format: %s\n", msg);
      break;
    default:
    case rgbe_memory_error:
      dt_print(DT_DEBUG_ALWAYS, "RGBE error: %s\n", msg);
  }
  return RGBE_RETURN_FAILURE;
}

/* standard conversion from rgbe to float pixels */
/* note: Ward uses ldexp(col+0.5,exp-(128+8)).  However we wanted pixels */
/*       in the range [0,1] to map back into the range [0,1].            */
static void rgbe2float(float *red, float *green, float *blue, unsigned char rgbe[4])
{
  if(rgbe[3]) /*nonzero pixel*/
  {
    const float f = ldexpf(1.0f, rgbe[3] - (int)(128 + 8));
    *red = rgbe[0] * f;
    *green = rgbe[1] * f;
    *blue = rgbe[2] * f;
  }
  else
    *red = *green = *blue = 0.0f;
}

/* minimal header reading.  modify if you want to parse more information */
int RGBE_ReadHeader(FILE *fp, int *width, int *height, rgbe_header_info *info)
{
  char buf[128];

  if(info)
  {
    info->valid = 0;
    info->programtype[0] = 0;
    info->gamma = info->exposure = 1.0;
    static const float default_primaries[] = { 0.640, 0.330, 0.290, 0.600, 0.150, 0.060, 0.333, 0.333 };
    memcpy(info->primaries, default_primaries, sizeof(info->primaries));
  }
  if(fgets(buf, sizeof(buf) / sizeof(buf[0]), fp) == NULL) return rgbe_error(rgbe_read_error, NULL);
  if((buf[0] != '#') || (buf[1] != '?'))
  {
    /* if you want to require the magic token then uncomment the next line */
    /*return rgbe_error(rgbe_format_error,"bad initial token"); */
  }
  else if(info)
  {
    info->valid |= RGBE_VALID_PROGRAMTYPE;
    size_t i;
    for(i = 0; i < sizeof(info->programtype) - 1; i++)
    {
      if((buf[i + 2] == 0) || isspace(buf[i + 2])) break;
      info->programtype[i] = buf[i + 2];
    }
    info->programtype[i] = 0;
    if(fgets(buf, sizeof(buf) / sizeof(buf[0]), fp) == 0) return rgbe_error(rgbe_read_error, NULL);
  }
  gboolean format_is_rgbe = FALSE;
  for(;;)
  {
    if((buf[0] == 0) || (buf[0] == '\n'))
      break;
    else if(strcmp(buf, "FORMAT=32-bit_rle_rgbe\n") == 0)
      format_is_rgbe = TRUE;
    else if(info)
    {
      if(g_str_has_prefix(buf, "GAMMA="))
      {
        char *startptr = buf + strlen("GAMMA="), *endptr;
        float tmp = g_ascii_strtod(startptr, &endptr);
        if(startptr != endptr)
        {
          info->gamma = tmp;
          info->valid |= RGBE_VALID_GAMMA;
        }
      }
      else if(g_str_has_prefix(buf, "EXPOSURE="))
      {
        char *startptr = buf + strlen("EXPOSURE="), *endptr;
        float tmp = g_ascii_strtod(startptr, &endptr);
        if(startptr != endptr)
        {
          info->exposure = tmp;
          info->valid |= RGBE_VALID_EXPOSURE;
        }
      }
      else if(g_str_has_prefix(buf, "PRIMARIES="))
      {
        float tmp[8];
        gboolean all_ok = TRUE;
        char *startptr = buf + strlen("PRIMARIES="), *endptr;
        for(int i = 0; i < 8; i++)
        {
          tmp[i] = g_ascii_strtod(startptr, &endptr);
          if(startptr == endptr)
          {
            all_ok = FALSE;
            break;
          }
          startptr = endptr;
        }
        if(all_ok) memcpy(info->primaries, tmp, sizeof(info->primaries));
      }
    }

    if(fgets(buf, sizeof(buf) / sizeof(buf[0]), fp) == 0) return rgbe_error(rgbe_read_error, NULL);
  }
  if(!format_is_rgbe)
    return rgbe_error(rgbe_format_error, "no FORMAT specifier found or it's not 32-bit_rle_rgbe");
  while(!strcmp(buf, "\n")) // be nice and accept more than one blank line
    if(fgets(buf, sizeof(buf) / sizeof(buf[0]), fp) == 0) return rgbe_error(rgbe_read_error, NULL);
  if(sscanf(buf, "-Y %d +X %d", height, width) < 2)
    return rgbe_error(rgbe_format_error, "missing image size specifier");
  return RGBE_RETURN_SUCCESS;
}

/* simple read routine.  will not correctly handle run length encoding */
int RGBE_ReadPixels(FILE *fp, float *data, int numpixels)
{
  unsigned char rgbe[4];

  while(numpixels-- > 0)
  {
    if(fread(rgbe, sizeof(rgbe), 1, fp) < 1) return rgbe_error(rgbe_read_error, NULL);
    rgbe2float(&data[RGBE_DATA_RED], &data[RGBE_DATA_GREEN], &data[RGBE_DATA_BLUE], rgbe);
    data += RGBE_DATA_SIZE;
  }
  return RGBE_RETURN_SUCCESS;
}

int RGBE_ReadPixels_RLE(FILE *fp, float *data, int scanline_width, int num_scanlines)
{
  unsigned char rgbe[4], *scanline_buffer, *ptr_end;
  int count;
  unsigned char buf[2];

  if((scanline_width < 8) || (scanline_width > 0x7fff)) /* run length encoding is not allowed so read flat*/
    return RGBE_ReadPixels(fp, data, scanline_width * num_scanlines);
  scanline_buffer = NULL;
  /* read in each successive scanline */
  while(num_scanlines > 0)
  {
    if(fread(rgbe, sizeof(rgbe), 1, fp) < 1)
    {
      free(scanline_buffer);
      return rgbe_error(rgbe_read_error, NULL);
    }
    if((rgbe[0] != 2) || (rgbe[1] != 2) || (rgbe[2] & 0x80))
    {
      /* this file is not run length encoded */
      rgbe2float(&data[0], &data[1], &data[2], rgbe);
      data += RGBE_DATA_SIZE;
      free(scanline_buffer);
      return RGBE_ReadPixels(fp, data, scanline_width * num_scanlines - 1);
    }
    if((((int)rgbe[2]) << 8 | rgbe[3]) != scanline_width)
    {
      free(scanline_buffer);
      return rgbe_error(rgbe_format_error, "wrong scanline width");
    }
    if(scanline_buffer == NULL)
      scanline_buffer = (unsigned char *)malloc(sizeof(unsigned char) * 4 * scanline_width);
    if(scanline_buffer == NULL) return rgbe_error(rgbe_memory_error, "unable to allocate buffer space");

    unsigned char *ptr = &scanline_buffer[0];
    /* read each of the four channels for the scanline into the buffer */
    for(int i = 0; i < 4; i++)
    {
      ptr_end = &scanline_buffer[(i + 1) * scanline_width];
      while(ptr < ptr_end)
      {
        if(fread(buf, sizeof(buf[0]) * 2, 1, fp) < 1)
        {
          free(scanline_buffer);
          return rgbe_error(rgbe_read_error, NULL);
        }
        if(buf[0] > 128)
        {
          /* a run of the same value */
          count = buf[0] - 128;
          if((count == 0) || (count > ptr_end - ptr))
          {
            free(scanline_buffer);
            return rgbe_error(rgbe_format_error, "bad scanline data");
          }
          while(count-- > 0) *ptr++ = buf[1];
        }
        else
        {
          /* a non-run */
          count = buf[0];
          if((count == 0) || (count > ptr_end - ptr))
          {
            free(scanline_buffer);
            return rgbe_error(rgbe_format_error, "bad scanline data");
          }
          *ptr++ = buf[1];
          if(--count > 0)
          {
            if(fread(ptr, sizeof(*ptr) * count, 1, fp) < 1)
            {
              free(scanline_buffer);
              return rgbe_error(rgbe_read_error, NULL);
            }
            ptr += count;
          }
        }
      }
    }
    /* now convert data from buffer into floats */
    for(int i = 0; i < scanline_width; i++)
    {
      rgbe[0] = scanline_buffer[i];
      rgbe[1] = scanline_buffer[i + scanline_width];
      rgbe[2] = scanline_buffer[i + 2 * scanline_width];
      rgbe[3] = scanline_buffer[i + 3 * scanline_width];
      rgbe2float(&data[RGBE_DATA_RED], &data[RGBE_DATA_GREEN], &data[RGBE_DATA_BLUE], rgbe);
      data += RGBE_DATA_SIZE;
    }
    num_scanlines--;
  }
  free(scanline_buffer);
  return RGBE_RETURN_SUCCESS;
}

#undef RGBE_VALID_PROGRAMTYPE
#undef RGBE_VALID_GAMMA
#undef RGBE_VALID_EXPOSURE

#undef RGBE_RETURN_SUCCESS
#undef RGBE_RETURN_FAILURE

#undef RGBE_DATA_RED
#undef RGBE_DATA_GREEN
#undef RGBE_DATA_BLUE
#undef RGBE_DATA_SIZE

// this function is borrowed from OpenEXR code
///////////////////////////////////////////////////////////////////////////
//
// Copyright (c) 2003, Industrial Light & Magic, a division of Lucas
// Digital Ltd. LLC
//
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
// *       Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
// *       Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
// *       Neither the name of Industrial Light & Magic nor the names of
// its contributors may be used to endorse or promote products derived
// from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
///////////////////////////////////////////////////////////////////////////
static void _xy2matrix(const float r[2], const float g[2], const float b[2],
                       const float w[2], const float Y, float M[4][4])
{
  float X = w[0] * Y / w[1];
  float Z = (1 - w[0] - w[1]) * Y / w[1];

  //
  // Scale factors for matrix rows
  //
  float d = r[0]   * (b[1]  - g[1]) +
  b[0]  * (g[1] - r[1]) +
  g[0] * (r[1]   - b[1]);
  float Sr = (X * (b[1] - g[1]) -
  g[0] * (Y * (b[1] - 1) +
  b[1]  * (X + Z)) +
  b[0]  * (Y * (g[1] - 1) +
  g[1] * (X + Z))) / d;
  float Sg = (X * (r[1] - b[1]) +
  r[0]   * (Y * (b[1] - 1) +
  b[1]  * (X + Z)) -
  b[0]  * (Y * (r[1] - 1) +
  r[1]   * (X + Z))) / d;
  float Sb = (X * (g[1] - r[1]) -
  r[0]   * (Y * (g[1] - 1) +
  g[1] * (X + Z)) +
  g[0] * (Y * (r[1] - 1) +
  r[1]   * (X + Z))) / d;

  //
  // Assemble the matrix
  //
  for(int i = 0; i < 4; i++) M[i][3] = M[3][i] = 0.0;
  M[3][3] = 1.0;
  M[0][0] = Sr * r[0];
  M[0][1] = Sr * r[1];
  M[0][2] = Sr * (1 - r[0] - r[1]);
  M[1][0] = Sg * g[0];
  M[1][1] = Sg * g[1];
  M[1][2] = Sg * (1 - g[0] - g[1]);
  M[2][0] = Sb * b[0];
  M[2][1] = Sb * b[1];
  M[2][2] = Sb * (1 - b[0] - b[1]);
}

dt_imageio_retval_t dt_imageio_open_rgbe(dt_image_t *img, const char *filename, dt_mipmap_buffer_t *mbuf)
{
  const char *ext = filename + strlen(filename);
  while(*ext != '.' && ext > filename) ext--;
  if(strncmp(ext, ".hdr", 4) && strncmp(ext, ".HDR", 4) && strncmp(ext, ".Hdr", 4))
    return DT_IMAGEIO_LOAD_FAILED;

  FILE *f = g_fopen(filename, "rb");
  if(!f) return DT_IMAGEIO_LOAD_FAILED;

  rgbe_header_info info;
  if(RGBE_ReadHeader(f, &img->width, &img->height, &info)) goto error_corrupt;

  img->buf_dsc.channels = 4;
  img->buf_dsc.datatype = TYPE_FLOAT;
  float *buf = (float *)dt_mipmap_cache_alloc(mbuf, img);
  if(!buf) goto error_cache_full;

  if(RGBE_ReadPixels_RLE(f, buf, img->width, img->height)) goto error_corrupt;
  fclose(f);

  // repair nan/inf etc
  const size_t width = img->width;
  const size_t height = img->height;
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(width, height, buf) \
  collapse(2)
#endif
  for(size_t i = width * height; i > 0; i--)
    for(int c = 0; c < 3; c++)
      buf[4 * (i - 1) + c] = fmaxf(0.0f, fminf(10000.0, buf[3 * (i - 1) + c]));

  // set the color matrix
  float m[4][4];
  _xy2matrix(&info.primaries[0], &info.primaries[2], &info.primaries[4], &info.primaries[6], 1.0, m);

  float mat[3][3];

  for(int i = 0; i < 3; i++)
    for(int j = 0; j < 3; j++)
    {
      mat[i][j] = m[j][i];
    }

  mat3inv((float *)img->d65_color_matrix, (float *)mat);

  img->buf_dsc.cst = IOP_CS_RGB;
  img->buf_dsc.filters = 0u;
  img->flags &= ~DT_IMAGE_LDR;
  img->flags &= ~DT_IMAGE_RAW;
  img->flags &= ~DT_IMAGE_S_RAW;
  img->flags |= DT_IMAGE_HDR;
  img->loader = LOADER_RGBE;
  return DT_IMAGEIO_OK;

error_corrupt:
  fclose(f);
  return DT_IMAGEIO_LOAD_FAILED;
error_cache_full:
  fclose(f);
  return DT_IMAGEIO_CACHE_FULL;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
