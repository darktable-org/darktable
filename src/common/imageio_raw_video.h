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
#ifndef DT_RAW_VIDEO_H
#define DT_RAW_VIDEO_H

#include "common/image.h"
#include "common/mipmap_cache.h"

// we got it from magick lantern
/* raw image info (geometry, calibration levels, color, DR etc); parts of this were copied from CHDK */
struct raw_info {
    int api_version;            // increase this when changing the structure
//     void* buffer;               // points to image data
    int buffer;                 // this always has to be 32 bit, so the actual implementation with a pointer doesn't work on 64 bit systems!

    int height, width, pitch;
    int frame_size;
    int bits_per_pixel;         // 14

    int black_level;            // autodetected
    int white_level;            // somewhere around 13000 - 16000, varies with camera, settings etc
                                // would be best to autodetect it, but we can't do this reliably yet
    union                       // DNG JPEG info
    {
        struct
        {
            int x, y;           // DNG JPEG top left corner
            int width, height;  // DNG JPEG size
        } jpeg;
        struct
        {
            int origin[2];
            int size[2];
        } crop;
    };
    union                       // DNG active sensor area (Y1, X1, Y2, X2)
    {
        struct
        {
            int y1, x1, y2, x2;
        } active_area;
        int dng_active_area[4];
    };
    int exposure_bias[2];       // DNG Exposure Bias (idk what's that)
    int cfa_pattern;            // stick to 0x02010100 (RGBG) if you can
    int calibration_illuminant1;
    int color_matrix1[18];      // DNG Color Matrix

    int dynamic_range;          // EV x100, from analyzing black level and noise (very close to DxO)
} __attribute__((packed));

/* file footer data */
typedef struct
{
    unsigned char magic[4];
    unsigned short xRes;
    unsigned short yRes;
    unsigned int frameSize;
    unsigned int frameCount;
    unsigned int frameSkip;
    unsigned int sourceFpsx1000;
    unsigned int reserved3;
    unsigned int reserved4;
    struct raw_info raw_info;
} lv_rec_file_footer_t;


dt_imageio_retval_t dt_imageio_open_raw_video(dt_image_t *img, const char *filename, dt_mipmap_cache_allocator_t a);
int dt_imageio_is_raw_video(const char *filename);
lv_rec_file_footer_t * dt_imageio_raw_video_get_footer(const char *filename);
void dt_imageio_raw_video_get_wb_coeffs(const lv_rec_file_footer_t *footer, float *coeffs, float *pre_mul);

#endif

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
