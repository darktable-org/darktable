// TODO: allow to generate a profile from raw_info.color_matrix1!

/*
    This file is part of darktable,
    copyright (c) 2013 tobias ellinghaus.
    copyright (C) 2013 Magic Lantern Team

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
#include "common/darktable.h"
#include "common/imageio_raw_video.h"
#include "imageio.h"

// #include <stdio.h>
// #include <stdlib.h>
// #include <strings.h>
// #include <math.h>
// #include <assert.h>


#define PA(p) ((int)(p->a))
#define PB(p) ((int)(p->b_lo | (p->b_hi << 12)))
#define PC(p) ((int)(p->c_lo | (p->c_hi << 10)))
#define PD(p) ((int)(p->d_lo | (p->d_hi << 8)))
#define PE(p) ((int)(p->e_lo | (p->e_hi << 6)))
#define PF(p) ((int)(p->f_lo | (p->f_hi << 4)))
#define PG(p) ((int)(p->g_lo | (p->g_hi << 2)))
#define PH(p) ((int)(p->h))

/* group 8 pixels in 14 bytes to simplify decoding */
struct raw_pixblock
{
  unsigned int b_hi: 2;
  unsigned int a: 14;     // even lines: red; odd lines: green
  unsigned int c_hi: 4;
  unsigned int b_lo: 12;
  unsigned int d_hi: 6;
  unsigned int c_lo: 10;
  unsigned int e_hi: 8;
  unsigned int d_lo: 8;
  unsigned int f_hi: 10;
  unsigned int e_lo: 6;
  unsigned int g_hi: 12;
  unsigned int f_lo: 4;
  unsigned int h: 14;     // even lines: green; odd lines: blue
  unsigned int g_lo: 2;
} __attribute__((packed));

dt_imageio_retval_t dt_imageio_open_raw_video(dt_image_t *img, const char *filename, dt_mipmap_cache_allocator_t a)
{
  int ret;
  lv_rec_file_footer_t lv_rec_footer;
  struct raw_info raw_info;
  uint8_t* raw = NULL;

  FILE* fin = fopen(filename, "rb");
  if(!fin) return DT_IMAGEIO_FILE_CORRUPTED;

  ret = fseek(fin, -192, SEEK_END);
  if(ret != 0) goto error_corrupt;

  ret = fread(&lv_rec_footer, 1, sizeof(lv_rec_file_footer_t), fin);
  if(ret != sizeof(lv_rec_file_footer_t)) goto error_corrupt;

  raw_info = lv_rec_footer.raw_info;

  if(strncmp((char*)lv_rec_footer.magic, "RAWM", 4)) goto error_corrupt;
  if(raw_info.api_version != 1) goto error_corrupt;
  if(raw_info.bits_per_pixel != 14) goto error_corrupt;

  img->width  = lv_rec_footer.xRes;
  img->height = lv_rec_footer.yRes;
  img->bpp = sizeof(uint16_t);
  img->flags &= ~DT_IMAGE_LDR;
  img->flags |= DT_IMAGE_RAW;
  // TODO: try to guess the correct model from the color_matrix1
  g_strlcpy(img->exif_maker, "Canon", sizeof(img->exif_maker));
  g_strlcpy(img->exif_model, "Canikon", sizeof(img->exif_model));

//   switch(raw_info.cfa_pattern)
//   {
//     case (0<<24) | (1<<16) | (1<<8) | 2:
//       img->filters = 0x94949494;
//       break;
//     case (1<<24) | (2<<16) | (0<<8) | 1:
//       img->filters = 0x49494949;
//       break;
//     case (1<<24) | (0<<16) | (2<<8) | 1:
//       img->filters = 0x61616161;
//       break;
//     case (2<<24) | (1<<16) | (1<<8) | 0:
//       img->filters = 0x16161616;
//       break;
//     default:
//       printf("[raw video] ERROR: unknown cfa_pattern: 0x%x\n", raw_info.cfa_pattern);
//       goto error_corrupt;
//   }
  img->filters = 0x94949494; // why?

  /* override the resolution from raw_info with the one from lv_rec_footer, if they don't match */
  if(lv_rec_footer.xRes != raw_info.width)
  {
    raw_info.width = lv_rec_footer.xRes;
    raw_info.pitch = raw_info.width * 14/8;
    raw_info.active_area.x1 = 0;
    raw_info.active_area.x2 = raw_info.width;
    raw_info.jpeg.x = 0;
    raw_info.jpeg.width = raw_info.width;
  }

  if(lv_rec_footer.yRes != raw_info.height)
  {
    raw_info.height = lv_rec_footer.yRes;
    raw_info.active_area.y1 = 0;
    raw_info.active_area.y2 = raw_info.height;
    raw_info.jpeg.y = 0;
    raw_info.jpeg.height = raw_info.height;
  }

  raw_info.frame_size = lv_rec_footer.frameSize;

  uint16_t *buf = (uint16_t*)dt_mipmap_cache_alloc(img, DT_MIPMAP_FULL, a);
  if(!buf) goto error_cache_full;

// get raw data blob
  int frame = MAX(0, MIN(img->sub_id, lv_rec_footer.frameCount-1));
  fseek(fin, frame*lv_rec_footer.frameSize, SEEK_SET);

  raw = malloc(lv_rec_footer.frameSize);
  ret = fread(raw, 1, lv_rec_footer.frameSize, fin);
  if(ret != lv_rec_footer.frameSize) goto error_corrupt;

  int black = raw_info.black_level;
  int white = raw_info.white_level;
  const float scale = 65535.0f/(white-black);

  uint16_t *buf_ptr = buf;
  for(struct raw_pixblock *row = (struct raw_pixblock*)raw; (void*)row < (void*)raw + raw_info.pitch * raw_info.height; row += raw_info.pitch / sizeof(struct raw_pixblock))
  {
    for(struct raw_pixblock *p = row; (void*)p < (void*)row + raw_info.pitch; p++)
    {
      int pa = PA(p);
      int pb = PB(p);
      int pc = PC(p);
      int pd = PD(p);
      int pe = PE(p);
      int pf = PF(p);
      int pg = PG(p);
      int ph = PH(p);

      *buf_ptr++ = (pa - black) * scale;
      *buf_ptr++ = (pb - black) * scale;
      *buf_ptr++ = (pc - black) * scale;
      *buf_ptr++ = (pd - black) * scale;
      *buf_ptr++ = (pe - black) * scale;
      *buf_ptr++ = (pf - black) * scale;
      *buf_ptr++ = (pg - black) * scale;
      *buf_ptr++ = (ph - black) * scale;
    }
  }

  free(raw);
  fclose(fin);
  return DT_IMAGEIO_OK;

error_corrupt:
  fclose(fin);
  free(raw);
  return DT_IMAGEIO_FILE_CORRUPTED;
error_cache_full:
  fclose(fin);
  return DT_IMAGEIO_CACHE_FULL;
}

int dt_imageio_is_raw_video(const char *filename)
{
  if(sizeof(lv_rec_file_footer_t) != 192)
  {
    printf("[raw video] FIXME: sizeof(lv_rec_file_footer_t) != 192\n");
    return 0;
  }

  FILE* fin = fopen(filename, "rb");
  if(fin)
  {
    int ret;
    lv_rec_file_footer_t lv_rec_footer;
    struct raw_info raw_info;

    ret = fseek(fin, -192, SEEK_END);
    if(ret != 0)
    {
      fclose(fin);
      return 0;
    }
    ret = fread(&lv_rec_footer, 1, sizeof(lv_rec_file_footer_t), fin);
    fclose(fin);
    if(ret != sizeof(lv_rec_file_footer_t))
      return 0;
    raw_info = lv_rec_footer.raw_info;

    if(strncmp((char*)lv_rec_footer.magic, "RAWM", 4)) return 0;

    if(raw_info.api_version != 1)
    {
      printf("[raw video] API version mismatch: %d\n", raw_info.api_version);
      return 1; // this file can't be loaded later on, but it is still a raw video
    }

    return 1;
  }
  return 0;
}

lv_rec_file_footer_t * dt_imageio_raw_video_get_footer(const char *filename)
{
  int ret;
  lv_rec_file_footer_t *lv_rec_footer;
  struct raw_info raw_info;

  FILE* fin = fopen(filename, "rb");
  if(!fin) return NULL;

  lv_rec_footer = (lv_rec_file_footer_t*)malloc(sizeof(lv_rec_file_footer_t));

  ret = fseek(fin, -192, SEEK_END);
  if(ret != 0) goto error_corrupt;

  ret = fread(lv_rec_footer, 1, sizeof(lv_rec_file_footer_t), fin);
  if(ret != sizeof(lv_rec_file_footer_t)) goto error_corrupt;

  raw_info = lv_rec_footer->raw_info;

  if(strncmp((char*)lv_rec_footer->magic, "RAWM", 4)) goto error_corrupt;
  if(raw_info.api_version != 1) goto error_corrupt;
  if(raw_info.bits_per_pixel != 14) goto error_corrupt;

  /* override the resolution from raw_info with the one from lv_rec_footer, if they don't match */
  if(lv_rec_footer->xRes != raw_info.width)
  {
    raw_info.width = lv_rec_footer->xRes;
    raw_info.pitch = raw_info.width * 14/8;
    raw_info.active_area.x1 = 0;
    raw_info.active_area.x2 = raw_info.width;
    raw_info.jpeg.x = 0;
    raw_info.jpeg.width = raw_info.width;
  }

  if(lv_rec_footer->yRes != raw_info.height)
  {
    raw_info.height = lv_rec_footer->yRes;
    raw_info.active_area.y1 = 0;
    raw_info.active_area.y2 = raw_info.height;
    raw_info.jpeg.y = 0;
    raw_info.jpeg.height = raw_info.height;
  }

  raw_info.frame_size = lv_rec_footer->frameSize;

  fclose(fin);
  return lv_rec_footer;

error_corrupt:
  fclose(fin);
  free(lv_rec_footer);
  return NULL;
}

void dt_imageio_raw_video_get_wb_coeffs(const lv_rec_file_footer_t *footer, float *coeffs, float *pre_mul)
{
  // coeffs
  double asn[] = {1.0, 2.477, 1.462}; // magic numbers, taken from magick lantern
  const double xyz_rgb[3][3] = {      /* XYZ from RGB */
    { 0.412453, 0.357580, 0.180423 },
    { 0.212671, 0.715160, 0.072169 },
    { 0.019334, 0.119193, 0.950227 } };

  for(int i = 0; i < 3; i++) coeffs[i] = 1 / asn[i];

  coeffs[0] /= coeffs[1];
  coeffs[2] /= coeffs[1];
  coeffs[1] = 1.0f;

  // pre_mul
  double cam_xyz[4][3];
  const int *ptr = footer->raw_info.color_matrix1;
  for(int a = 0; a < 3; a++)
    for(int b = 0; b < 3; b++)
    {
      cam_xyz[a][b] = (double)*ptr / (double)*(ptr+1);
      ptr += 2;
    }

  double cam_rgb[4][3], num;
  int i, j, k;

  for(i = 0; i < 3; i++)    /* Multiply out XYZ colorspace */
    for(j = 0; j < 3; j++)
      for(cam_rgb[i][j] = k = 0; k < 3; k++)
        cam_rgb[i][j] += cam_xyz[i][k] * xyz_rgb[k][j];

  for(i = 0; i < 3; i++) {    /* Normalize cam_rgb so that */
    for(num = j = 0; j < 3; j++)   /* cam_rgb * (1,1,1) is (1,1,1,1) */
      num += cam_rgb[i][j];
    for(j = 0; j < 3; j++)
      cam_rgb[i][j] /= num;
    pre_mul[i] = 1.0 / num;
  }
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
