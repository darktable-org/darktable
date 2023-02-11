/*
 *    This file is part of darktable,
 *    Copyright (C) 2015-2020 darktable developers.
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

/*
 *  this is a simple PDF writer, capable of creating multi page PDFs with embedded images.
 *  it is NOT meant to be a full fledged PDF library, and shall never turn into something like that!
 */

// add the following define to compile this into a standalone test program:
// #define STANDALONE
// or use
// gcc -W -Wall -std=c99 -lz -lm `pkg-config --cflags --libs glib-2.0` -g -O3 -fopenmp -DSTANDALONE -o darktable-pdf pdf.c

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#define _XOPEN_SOURCE 700
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <zlib.h>

#include <glib/gstdio.h>

#ifdef STANDALONE
#define PACKAGE_STRING "darktable pdf library"
#else
#define PACKAGE_STRING darktable_package_string
#endif

#include "pdf.h"
#include "common/math.h"
#include "common/utility.h"

#define SKIP_SPACES(s)  {while(*(s) == ' ')(s)++;}

// puts the length as described in str as pdf points into *length
// returns 0 on error
// a length has a number, followed by a unit if it's != 0.0
int dt_pdf_parse_length(const char *str, float *length)
{
  int res = 0;
  char *nptr, *endptr;

  if(str == NULL || length == NULL)
    return 0;

  SKIP_SPACES(str);

  nptr = g_strdelimit(g_strdup(str), ",", '.');

  *length =  g_ascii_strtod(nptr, &endptr);

  if(endptr == NULL || errno == ERANGE)
    goto end;

  // 0 is 0 is 0, why should we care about the unit?
  if(*length == 0.0 && nptr != endptr)
  {
    res = 1;
    goto end;
  }

  // we don't want NAN, INF or parse errors (== 0.0)
  if(!isnormal(*length))
    goto end;

  SKIP_SPACES(endptr);

  for(int i = 0; dt_pdf_units[i].name; i++)
  {
    if(!g_strcmp0(endptr, dt_pdf_units[i].name))
    {
      *length *= dt_pdf_units[i].factor;
      res = 1;
      break;
    }
  }

end:
  g_free(nptr);
  return res;
}

// a paper size has 2 numbers, separated by 'x' or '*' and a unit, either one per number or one in the end (for both)
// <n> <u>? [x|*] <n> <u>
// alternatively it could be a well defined format
int dt_pdf_parse_paper_size(const char *str, float *width, float *height)
{
  int res = 0;
  gboolean width_has_unit = FALSE;
  char *ptr, *nptr, *endptr;

  if(str == NULL || width == NULL || height == NULL)
    return 0;

  // first check if this is a well known size
  for(int i = 0; dt_pdf_paper_sizes[i].name; i++)
  {
    if(!strcasecmp(str, dt_pdf_paper_sizes[i].name))
    {
      *width = dt_pdf_paper_sizes[i].width;
      *height = dt_pdf_paper_sizes[i].height;
      return 1;
    }
  }

  ptr = nptr = g_strdelimit(g_strdup(str), ",", '.');

  // width
  SKIP_SPACES(nptr);

  *width =  g_ascii_strtod(nptr, &endptr);

  if(endptr == NULL || *endptr == '\0' || errno == ERANGE || !isnormal(*width))
    goto end;

  nptr = endptr;

  // unit?
  SKIP_SPACES(nptr);

  for(int i = 0; dt_pdf_units[i].name; i++)
  {
    if(g_str_has_prefix(nptr, dt_pdf_units[i].name))
    {
      *width *= dt_pdf_units[i].factor;
      width_has_unit = TRUE;
      nptr += strlen(dt_pdf_units[i].name);
      break;
    }
  }

  // x
  SKIP_SPACES(nptr);

  if(*nptr != 'x' && *nptr != '*')
    goto end;

  nptr++;

  // height
  SKIP_SPACES(nptr);

  *height =  g_ascii_strtod(nptr, &endptr);

  if(endptr == NULL || *endptr == '\0' || errno == ERANGE || !isnormal(*height))
    goto end;

  nptr = endptr;

  // unit
  SKIP_SPACES(nptr);

  for(int i = 0; dt_pdf_units[i].name; i++)
  {
    if(!g_strcmp0(nptr, dt_pdf_units[i].name))
    {
      *height *= dt_pdf_units[i].factor;
      if(width_has_unit == FALSE)
        *width *= dt_pdf_units[i].factor;
      res = 1;
      break;
    }
  }

end:
  g_free(ptr);
  return res;
}

#undef SKIP_SPACES


static const char *stream_encoder_filters[] = {"/ASCIIHexDecode", "/FlateDecode"};

static void _pdf_set_offset(dt_pdf_t *pdf, int id, size_t offset)
{
  id--; // object ids start at 1
  if(id >= pdf->n_offsets)
  {
    pdf->n_offsets = MAX(pdf->n_offsets * 2, id);
    pdf->offsets = realloc(pdf->offsets, sizeof(size_t) * pdf->n_offsets);
  }
  pdf->offsets[id] = offset;
}

dt_pdf_t *dt_pdf_start(const char *filename, float width, float height, float dpi, dt_pdf_stream_encoder_t default_encoder)
{
  dt_pdf_t *pdf = calloc(1, sizeof(dt_pdf_t));
  if(!pdf) return NULL;

  pdf->fd = g_fopen(filename, "wb");
  if(!pdf->fd)
  {
    free(pdf);
    return NULL;
  }

  pdf->page_width = width;
  pdf->page_height = height;
  pdf->dpi = dpi;
  pdf->default_encoder = default_encoder;
  // object counting starts at 1, and the first 2 are reserved for the document catalog + pages dictionary
  pdf->next_id = 3;
  pdf->next_image = 0;

  pdf->n_offsets = 4;
  pdf->offsets = calloc(pdf->n_offsets, sizeof(size_t));

  size_t bytes_written = 0;

  // file header
  // pdf specs encourage to put 4 binary bytes in a comment
  bytes_written += fprintf(pdf->fd, "%%PDF-1.3\n\xde\xad\xbe\xef\n");

  // document catalog
  _pdf_set_offset(pdf, 1, bytes_written);
  bytes_written += fprintf(pdf->fd,
    "1 0 obj\n"
    "<<\n"
    "/Pages 2 0 R\n"
    "/Type /Catalog\n"
    ">>\n"
    "endobj\n"
  );

  pdf->bytes_written += bytes_written;

  return pdf;
}

// TODO: maybe OpenMP-ify, it's quite fast already (the fwrite is the slowest part), but wouldn't hurt
static size_t _pdf_stream_encoder_ASCIIHex(dt_pdf_t *pdf, const unsigned char *data, size_t len)
{
  const char hex[16] = { '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f' };

  char buf[512]; // keep this a multiple of 2!

  for(size_t i = 0; i < len; i++)
  {
    const int hi = data[i] >> 4;
    const int lo = data[i] & 15;
    buf[(2 * i) % sizeof(buf)] = hex[hi];
    buf[(2 * i + 1) % sizeof(buf)] = hex[lo];
    if((i + 1) % (sizeof(buf) / 2) == 0 || (i + 1) == len)
      fwrite(buf, 1, (i % (sizeof(buf) / 2) + 1) * 2, pdf->fd);
  }
  return len * 2;
}

// using zlib we get quite small files, but it's slow
static size_t _pdf_stream_encoder_Flate(dt_pdf_t *pdf, const unsigned char *data, size_t len)
{
  int result;
  uLongf destLen = compressBound(len);
  unsigned char *buffer = (unsigned char *)malloc(destLen);

  result = compress(buffer, &destLen, data, len);

  if(result != Z_OK)
  {
    free(buffer);
    return 0;
  }

  fwrite(buffer, 1, destLen, pdf->fd);

  free(buffer);
  return destLen;
}

static size_t _pdf_write_stream(dt_pdf_t *pdf, dt_pdf_stream_encoder_t encoder, const unsigned char *data, size_t len)
{
  size_t stream_size = 0;
  switch(encoder)
  {
    case DT_PDF_STREAM_ENCODER_ASCII_HEX:
      stream_size = _pdf_stream_encoder_ASCIIHex(pdf, data, len);
      break;
    case DT_PDF_STREAM_ENCODER_FLATE:
      stream_size = _pdf_stream_encoder_Flate(pdf, data, len);
      break;
  }
  return stream_size;
}

int dt_pdf_add_icc(dt_pdf_t *pdf, const char *filename)
{
  size_t len;
  unsigned char *data = (unsigned char *)dt_read_file(filename, &len);
  if(data)
  {
    int icc_id = dt_pdf_add_icc_from_data(pdf, data, len);
    free(data);
    return icc_id;
  }
  else
    return 0;
}

int dt_pdf_add_icc_from_data(dt_pdf_t *pdf, const unsigned char *data, size_t size)
{
  int icc_id = pdf->next_id++;
  int length_id = pdf->next_id++;
  size_t bytes_written = 0;

  // length of the stream
  _pdf_set_offset(pdf, icc_id, pdf->bytes_written + bytes_written);
  bytes_written += fprintf(pdf->fd,
                           "%d 0 obj\n"
                           "<<\n"
                           "/N 3\n" // should we ever support CMYK profiles then this has to be set to 4 for those
                           "/Alternate /DeviceRGB\n"
                           "/Length %d 0 R\n"
                           "/Filter [ /ASCIIHexDecode ]\n"
                           ">>\n"
                           "stream\n",
                           icc_id, length_id
  );

  size_t stream_size = _pdf_stream_encoder_ASCIIHex(pdf, data, size);
  bytes_written += stream_size;

  bytes_written += fprintf(pdf->fd,
                           "\n"
                           "endstream\n"
                           "endobj\n"
  );

  // length of the stream
  _pdf_set_offset(pdf, length_id, pdf->bytes_written + bytes_written);
  bytes_written += fprintf(pdf->fd, "%d 0 obj\n"
                                    "%zu\n"
                                    "endobj\n",
                           length_id, stream_size);

  pdf->bytes_written += bytes_written;

  return icc_id;
}

// this adds an image to the pdf file and returns the info needed to reference it later.
// if icc_id is 0 then we suppose the pixel data to be in output device space, otherwise the ICC profile object is referenced.
// if image == NULL only the outline can be shown later
dt_pdf_image_t *dt_pdf_add_image(dt_pdf_t *pdf, const unsigned char *image, int width, int height, int bpp, int icc_id, float border)
{
  size_t stream_size = 0;
  size_t bytes_written = 0;

  dt_pdf_image_t *pdf_image = calloc(1, sizeof(dt_pdf_image_t));
  if(!pdf_image) return NULL;

  pdf_image->width = width;
  pdf_image->height = height;
  pdf_image->outline_mode = (image == NULL);
  // no need to do fancy math here:
  pdf_image->bb_x = border;
  pdf_image->bb_y = border;
  pdf_image->bb_width = pdf->page_width - (2 * border);
  pdf_image->bb_height = pdf->page_height - (2 * border);

  // just draw outlines if the image is missing
  if(pdf_image->outline_mode) return pdf_image;

  pdf_image->object_id = pdf->next_id++;
  pdf_image->name_id = pdf->next_image++;

  int length_id = pdf->next_id++;

  // the image
  //start
  _pdf_set_offset(pdf, pdf_image->object_id, pdf->bytes_written + bytes_written);
  bytes_written += fprintf(pdf->fd,
    "%d 0 obj\n"
    "<<\n"
    "/Type /XObject\n"
    "/Subtype /Image\n"
    "/Name /Im%d\n"
    "/Filter [ %s ]\n"
    "/Width %d\n"
    "/Height %d\n",
    pdf_image->object_id, pdf_image->name_id, stream_encoder_filters[pdf->default_encoder], width, height
  );
  // As I understand it in the printing case DeviceRGB (==> icc_id = 0) is enough since the pixel data is in the device space then.
  if(icc_id > 0)
    bytes_written += fprintf(pdf->fd, "/ColorSpace [ /ICCBased %d 0 R ]\n", icc_id);
  else
    bytes_written += fprintf(pdf->fd, "/ColorSpace /DeviceRGB\n");
  bytes_written += fprintf(pdf->fd,
    "/BitsPerComponent %d\n"
    "/Intent /Perceptual\n" // TODO: allow setting it from the outside
    "/Length %d 0 R\n"
    ">>\n"
    "stream\n",
    bpp, length_id
  );

  // the stream
  stream_size = _pdf_write_stream(pdf, pdf->default_encoder, image, (size_t)3 * (bpp / 8) * width * height);
  if(stream_size == 0)
  {
    free(pdf_image);
    return NULL;
  }
  bytes_written += stream_size;

  //end
  bytes_written += fprintf(pdf->fd,
    "\n"
    "endstream\n"
    "endobj\n"
  );

  // length of the last stream
  _pdf_set_offset(pdf, length_id, pdf->bytes_written + bytes_written);
  bytes_written += fprintf(pdf->fd, "%d 0 obj\n"
                                    "%zu\n"
                                    "endobj\n",
                           length_id, stream_size);

  pdf->bytes_written += bytes_written;
  pdf_image->size = bytes_written;

  return pdf_image;
}

dt_pdf_page_t *dt_pdf_add_page(dt_pdf_t *pdf, dt_pdf_image_t **images, int n_images)
{
  dt_pdf_page_t *pdf_page = calloc(1, sizeof(dt_pdf_page_t));
  if(!pdf_page) return NULL;
  pdf_page->object_id = pdf->next_id++;
  int content_id = pdf->next_id++;
  int length_id = pdf->next_id++;
  size_t stream_size = 0, bytes_written = 0;

  // the page object
  _pdf_set_offset(pdf, pdf_page->object_id, pdf->bytes_written + bytes_written);
  bytes_written += fprintf(pdf->fd,
    "%d 0 obj\n"
    "<<\n"
    "/Type /Page\n"
    "/Parent 2 0 R\n"
    "/Resources <<\n"
    "/XObject <<",
    pdf_page->object_id
  );
  for(int i = 0; i < n_images; i++)
    bytes_written += fprintf(pdf->fd, "/Im%d %d 0 R\n", images[i]->name_id, images[i]->object_id);
  bytes_written += fprintf(pdf->fd,
    ">>\n"
    "/ProcSet [ /PDF /Text /ImageC ] >>\n"
    "/MediaBox [0 0 %d %d]\n"
    "/Contents %d 0 R\n"
    ">>\n"
    "endobj\n",
    (int)(pdf->page_width + 0.5), (int)(pdf->page_height + 0.5), content_id
  );

  // page content
  _pdf_set_offset(pdf, content_id, pdf->bytes_written + bytes_written);
  bytes_written += fprintf(pdf->fd,
    "%d 0 obj\n"
    "<<\n"
    "/Length %d 0 R\n"
    ">>\n"
    "stream\n",
    content_id, length_id
  );

  // the stream -- we need its size in the length object
  // we want the image printed with at least the given DPI, scaling it down to fit the page if it is too big
  gboolean portrait_page = pdf->page_width < pdf->page_height;

  for(int i = 0; i < n_images; i++)
  {
    // fit the image into the bounding box that comes with the image
    float scale_x, scale_y, translate_x, translate_y;
    float width, height;
    gboolean portrait_image = images[i]->width < images[i]->height;
    gboolean rotate_to_fit = images[i]->rotate_to_fit && (portrait_page != portrait_image);
    if(rotate_to_fit)
    {
      width = images[i]->height;
      height = images[i]->width;
    }
    else
    {
      width = images[i]->width;
      height = images[i]->height;
    }

    float image_aspect_ratio = width / height;
    float bb_aspect_ratio = images[i]->bb_width / images[i]->bb_height;

    if(image_aspect_ratio <= bb_aspect_ratio)
    {
      // scale to fit height
      float height_in_point = (height / pdf->dpi) * 72.0;
      scale_y = MIN(images[i]->bb_height, height_in_point);
      scale_x = scale_y * image_aspect_ratio;
    }
    else
    {
      // scale to fit width
      float width_in_point = (width / pdf->dpi) * 72.0;
      scale_x = MIN(images[i]->bb_width, width_in_point);
      scale_y = scale_x / image_aspect_ratio;
    }

    // center inside image's bounding box
    translate_x = images[i]->bb_x + 0.5 * (images[i]->bb_width - scale_x);
    translate_y = images[i]->bb_y + 0.5 * (images[i]->bb_height - scale_y);

    if(rotate_to_fit && !images[i]->outline_mode)
    {
      float tmp = scale_x;
      scale_x = scale_y;
      scale_y = tmp;
      translate_x += scale_y;
    }

    // unfortunately regular fprintf honors the decimal separator as set by the current locale,
    // we want '.' in all cases though.
    char translate_x_str[G_ASCII_DTOSTR_BUF_SIZE];
    char translate_y_str[G_ASCII_DTOSTR_BUF_SIZE];
    char scale_x_str[G_ASCII_DTOSTR_BUF_SIZE];
    char scale_y_str[G_ASCII_DTOSTR_BUF_SIZE];

    g_ascii_dtostr(translate_x_str, G_ASCII_DTOSTR_BUF_SIZE, translate_x);
    g_ascii_dtostr(translate_y_str, G_ASCII_DTOSTR_BUF_SIZE, translate_y);
    g_ascii_dtostr(scale_x_str, G_ASCII_DTOSTR_BUF_SIZE, scale_x);
    g_ascii_dtostr(scale_y_str, G_ASCII_DTOSTR_BUF_SIZE, scale_y);

    if(images[i]->outline_mode)
    {
      // instead of drawign the image we just draw the outlines
      stream_size += fprintf(pdf->fd,
        "q\n"
        "[4 6] 0 d\n"
        "%s %s %s %s re\n"
        "S\n"
        "Q\n",
        translate_x_str, translate_y_str, scale_x_str, scale_y_str
      );
    }
    else
    {
      stream_size += fprintf(pdf->fd,
        "q\n"
        "1 0 0 1 %s %s cm\n", // translate
        translate_x_str, translate_y_str
      );
      if(rotate_to_fit)
        stream_size += fprintf(pdf->fd,
          "0 1 -1 0 0 0 cm\n" // rotate
        );
      stream_size += fprintf(pdf->fd,
        "%s 0 0 %s 0 0 cm\n" // scale
        "/Im%d Do\n"
        "Q\n",
        scale_x_str, scale_y_str, images[i]->name_id
      );
    }

    // DEBUG: draw the bounding box
    if(images[i]->show_bb)
    {
      char bb_x_str[G_ASCII_DTOSTR_BUF_SIZE];
      char bb_y_str[G_ASCII_DTOSTR_BUF_SIZE];
      char bb_w_str[G_ASCII_DTOSTR_BUF_SIZE];
      char bb_h_str[G_ASCII_DTOSTR_BUF_SIZE];

      g_ascii_dtostr(bb_x_str, G_ASCII_DTOSTR_BUF_SIZE, images[i]->bb_x);
      g_ascii_dtostr(bb_y_str, G_ASCII_DTOSTR_BUF_SIZE, images[i]->bb_y);
      g_ascii_dtostr(bb_w_str, G_ASCII_DTOSTR_BUF_SIZE, images[i]->bb_width);
      g_ascii_dtostr(bb_h_str, G_ASCII_DTOSTR_BUF_SIZE, images[i]->bb_height);

      stream_size += fprintf(pdf->fd,
        "q\n"
        "%s %s %s %s re\n"
        "S\n"
        "Q\n",
        bb_x_str, bb_y_str, bb_w_str, bb_h_str
      );
    }
  }

  bytes_written += fprintf(pdf->fd,
    "endstream\n"
    "endobj\n"
  );
  bytes_written += stream_size;

  // length of the last stream
  _pdf_set_offset(pdf, length_id, pdf->bytes_written + bytes_written);
  bytes_written += fprintf(pdf->fd, "%d 0 obj\n"
                                    "%zu\n"
                                    "endobj\n",
                           length_id, stream_size);

  pdf_page->size = bytes_written;
  pdf->bytes_written += bytes_written;

  return pdf_page;
}

// our writing order is a little strange since we write object 2 (the pages dictionary) at the end of the file
// because we don't know the number of pages / objects in advance (due to lazy coding)
void dt_pdf_finish(dt_pdf_t *pdf, dt_pdf_page_t **pages, int n_pages)
{
  int info_id = pdf->next_id++;
  size_t bytes_written = 0;

  // the pages dictionary
  _pdf_set_offset(pdf, 2, pdf->bytes_written + bytes_written);
  bytes_written += fprintf(pdf->fd,
    "2 0 obj\n" // yes, this is hardcoded to be object 2, even if written in the end
    "<<\n"
    "/Type /Pages\n"
    "/Kids [\n"
  );
  for(int i = 0; i < n_pages; i++)
    bytes_written += fprintf(pdf->fd, "%d 0 R\n", pages[i]->object_id);
  bytes_written += fprintf(pdf->fd,
    "]\n"
    "/Count %d\n"
    ">>\n"
    "endobj\n",
    n_pages
  );

  // the info

  // the method to get the time_str is taken from pdftex
  char time_str[30];
  time_t t;
  struct tm lt, gmt;
  size_t size;
  int off, off_hours, off_mins;

  /* get the time */
  t = time(NULL);
  localtime_r(&t, &lt);
  size = strftime(time_str, sizeof(time_str), "D:%Y%m%d%H%M%S", &lt);
  /* expected format: "YYYYmmddHHMMSS" */
  if(size == 0)
  {
    /* unexpected, contents of time_str is undefined */
    time_str[0] = '\0';
    goto time_error;
  }

  /* correction for seconds: %S can be in range 00..61,
   *  the PDF reference expects 00..59,
   *  therefore we map "60" and "61" to "59" */
  if(time_str[14] == '6')
  {
    time_str[14] = '5';
    time_str[15] = '9';
    time_str[16] = '\0';    /* for safety */
  }

  /* get the time zone offset */
  gmtime_r(&t, &gmt);

  /* this calculation method was found in exim's tod.c */
  off = 60 * (lt.tm_hour - gmt.tm_hour) + lt.tm_min - gmt.tm_min;
  if(lt.tm_year != gmt.tm_year)
    off += (lt.tm_year > gmt.tm_year) ? 1440 : -1440;
  else if(lt.tm_yday != gmt.tm_yday)
    off += (lt.tm_yday > gmt.tm_yday) ? 1440 : -1440;

  if(off == 0)
  {
    time_str[size++] = 'Z';
    time_str[size] = 0;
  }
  else
  {
    off_hours = off / 60;
    off_mins = abs(off - off_hours * 60);
    snprintf(&time_str[size], 9, "%+03d'%02d'", off_hours, off_mins);
  }

time_error:

  _pdf_set_offset(pdf, info_id, pdf->bytes_written + bytes_written);
  bytes_written += fprintf(pdf->fd,
    "%d 0 obj\n"
    "<<\n"
    "/Title (%s)\n",
    info_id, pdf->title ? pdf->title : "untitled"
  );
  if(*time_str)
  {
    bytes_written += fprintf(pdf->fd,
      "/CreationDate (%s)\n"
      "/ModDate (%s)\n",
      time_str, time_str
    );
  }
  bytes_written += fprintf(pdf->fd, "/Producer (%s https://www.darktable.org)\n"
                                    ">>\n"
                                    "endobj\n",
                           PACKAGE_STRING);

  pdf->bytes_written += bytes_written;

  // the cross reference table
  fprintf(pdf->fd,
    "xref\n"
    "0 %d\n"
    "0000000000 65535 f \n",
    pdf->next_id
  );
  for(int i = 0; i < pdf->next_id - 1; i++) fprintf(pdf->fd, "%010zu 00000 n \n", pdf->offsets[i]);

  // the trailer
  fprintf(pdf->fd,
    "trailer\n"
    "<<\n"
    "/Size %d\n"
    "/Info %d 0 R\n" // we want to have the Info last in the file, so this is /Size - 1
    "/Root 1 0 R\n"
    "/ID [<dead> <babe>]\n" // TODO find something less necrophilic, maybe hash of image + history? or just of filename + date :)
    ">>\n",
    pdf->next_id, info_id
  );

  // and finally the file footer with the offset of the xref section
  fprintf(pdf->fd, "startxref\n"
                   "%zu\n"
                   "%%%%EOF\n",
          pdf->bytes_written);

  fclose(pdf->fd);
  free(pdf->offsets);
  free(pdf);
}

#ifdef STANDALONE

// just for debugging to read a ppm file
float * read_ppm(const char * filename, int * wd, int * ht)
{
  FILE *f = g_fopen(filename, "rb");

  if(!f)
  {
    fprintf(stderr, "can't open input file\n");
    return NULL;
  }

  char magic[3];
  int width, height, max;
  fscanf(f, "%c%c %d %d %d ", &magic[0], &magic[1], &width, &height, &max);
  if(magic[0] != 'P' || magic[1] != '6')
  {
    fprintf(stderr, "wrong input file format\n");
    fclose(f);
    return NULL;
  }

  float *image = (float*)malloc(sizeof(float) * width * height * 3);

  if(max <= 255)
  {
    // read a 8 bit PPM
    uint8_t *tmp = (uint8_t *)malloc(sizeof(uint8_t) * width * height * 3);
    int res = fread(tmp, sizeof(uint8_t) * 3, width * height, f);
    if(res != width * height)
    {
      fprintf(stderr, "error reading 8 bit PPM\n");
      free(tmp);
      free(image);
      fclose(f);
      return NULL;
    }
    // and transform it into 0..1 range
    #ifdef _OPENMP
    #pragma omp parallel for schedule(static) default(none) shared(image, tmp, width, height, max)
    #endif
    for(int i = 0; i < width * height * 3; i++)
      image[i] = (float)tmp[i] / max;
    free(tmp);
  }
  else
  {
    // read a 16 bit PPM
    uint16_t *tmp = (uint16_t *)malloc(sizeof(uint16_t) * width * height * 3);
    int res = fread(tmp, sizeof(uint16_t) * 3, width * height, f);
    if(res != width * height)
    {
      fprintf(stderr, "error reading 16 bit PPM\n");
      free(tmp);
      free(image);
      fclose(f);
      return NULL;
    }
    // swap byte order
    #ifdef _OPENMP
    #pragma omp parallel for schedule(static) default(none) shared(tmp, width, height)
    #endif
    for(int k = 0; k < 3 * width * height; k++)
      tmp[k] = ((tmp[k] & 0xff) << 8) | (tmp[k] >> 8);
    // and transform it into 0..1 range
    #ifdef _OPENMP
    #pragma omp parallel for schedule(static) default(none) shared(image, tmp, max, width, height)
    #endif
    for(int i = 0; i < width * height * 3; i++)
      image[i] = (float)tmp[i] / max;
    free(tmp);
  }
  fclose(f);

  if(wd) *wd = width;
  if(ht) *ht = height;
  return image;
}

int main(int argc, char *argv[])
{
  if(argc < 3)
  {
    fprintf(stderr, "usage: %s <input PPM> [<input PPM> ...] <output PDF>\n", argv[0]);
    exit(1);
  }

  // example for A4 portrait, which is 210 mm x 297 mm.
  float page_width, page_height, border;
  dt_pdf_parse_length("10 mm", &border); // add an empty space of 1 cm for the sake of demonstration
  dt_pdf_parse_paper_size("a4", &page_width, &page_height);

  // since this is just stupid example code we are going to put the images into the pdf twice:
  // I am not 100% sure if image objects may be reused like that in PDFs, but it seems to work

  dt_pdf_t *pdf = dt_pdf_start(argv[argc - 1], page_width, page_height, 360, DT_PDF_STREAM_ENCODER_FLATE);

  // we can load icc profiles and assign them to images. For testing something like
  // https://github.com/boxerab/graphicsmagick/raw/master/profiles/BRG.icc works really good
  int icc_id = dt_pdf_add_icc(pdf, "BRG.icc");

  const int n_images = argc - 2;
  const int n_pages = argc - 1;

  dt_pdf_image_t *images[n_images];
  dt_pdf_page_t *pages[n_pages]; // one extra page for the stupid image dump

  // load all the images. it doesn't matter when we do it, as long as they are loaded before
  // creating the page they should appear on (and even that is just a constraint of this code)
  for(int i = 0; i < n_images; i++)
  {
    int width, height;
    float *image = read_ppm(argv[i + 1], &width, &height);
    if(!image) exit(1);
    uint16_t *data = (uint16_t *)malloc(sizeof(uint16_t) * 3 * width * height);
    if(!data)
    {
      free(image);
      exit(1);
    }

#ifdef _OPENMP
  #pragma omp parallel for schedule(static) default(none) shared(image, data, width, height)
#endif
    for(int i = 0; i < width * height * 3; i++)
      data[i] = CLIP(image[i]) * 65535;

    images[i] = dt_pdf_add_image(pdf, (unsigned char *)data, width, height, 16, icc_id, border);
    free(image);
    free(data);
  }

  // add pages with one image each, filling the page minus borders
  for(int i = 0; i < n_images; i++)
    pages[i] = dt_pdf_add_page(pdf, &images[i], 1);

  // add the whole bunch of images to the last page
  // images' default bounding boxen span the whole page, so set them a little smaller first, also enable bounding box drawing.
  // we can also set outline mode afterwards. note that it is NOT safe to load images with outline_mode = 1 and then set it to 0 later!
  {
    // TODO: use border and add new pages when we filled one up
    float bb_size = dt_pdf_mm_to_point(60);
    int n_x = page_width / bb_size;
    float bb_empty = (page_width - (n_x * bb_size)) / n_x;
    float bb_step = bb_empty + bb_size;

    float x = bb_empty * 0.5, y = bb_empty * 0.5;

    for(int i = 0; i < n_images; i++)
    {
      images[i]->outline_mode = TRUE;
      images[i]->show_bb = TRUE;
      images[i]->bb_width = bb_size;
      images[i]->bb_height = bb_size;
      images[i]->bb_x = x;
      images[i]->bb_y = y;
      x += bb_step;
      if((i+1) % n_x == 0)
      {
        x = bb_empty * 0.5;
        y += bb_step;
      }
    }
  }

  pages[n_images] = dt_pdf_add_page(pdf, images, n_images);

  dt_pdf_finish(pdf, pages, n_pages);

  for(int i = 0; i < n_images; i++)
    free(images[i]);
  for(int i = 0; i < n_pages; i++)
    free(pages[i]);

  return 0;
}

#endif // STANDALONE

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
