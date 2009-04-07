/* -*- C++ -*-
 * File: libraw_types.h
 * Copyright 2008-2009 Alex Tutubalin <lexa@lexa.ru>
 * Created: Sat Mar  8 , 2008
 *
 * LibRaw C data structures
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 */

#ifndef _LIBRAW_TYPES_H
#define _LIBRAW_TYPES_H

#ifndef WIN32
#include <sys/time.h>
#endif
#include <stdio.h>
#ifdef _OPENMP
#include <omp.h>
#endif


#ifdef __cplusplus
extern "C" {
#endif

#ifndef USE_LCMS
#define NO_LCMS
#endif

#include "libraw_const.h"
#include "libraw_version.h"

typedef long long INT64;
typedef unsigned long long UINT64;
//#define ushort UshORt
typedef unsigned char uchar;
typedef unsigned short ushort;

#ifdef WIN32
#ifdef LIBRAW_NODLL
# define DllDef
#else
# ifdef LIBRAW_BUILDLIB
#    define DllDef   __declspec( dllexport )
# else
#    define DllDef   __declspec( dllimport )
# endif
#endif
// NO Win32
#else
#  define DllDef
#endif


//class LibRaw;

typedef void (* memory_callback)(void * data, const char *file, const char *where);

DllDef void default_memory_callback(void *data,const char *file, const char *where);

typedef void (*data_callback)(void *data,const char *file, const int offset);

DllDef void default_data_callback(void *data,const char *file, const int offset);

typedef int (* progress_callback) (void *data,enum LibRaw_progress stage, int iteration,int expected);

typedef struct
{
    memory_callback mem_cb;
    void*  memcb_data;

    data_callback data_cb;
    void*       datacb_data;

    progress_callback progress_cb;
    void *progresscb_data;
} libraw_callbacks_t;

// Output bitmap type

typedef struct
{
    enum LibRaw_image_formats type; 
    ushort      height,
                width,
                colors,
                bits,
                gamma_corrected;
#if 0//def _OPENMP
#pragma omp firstprivate(colors,height,width)
#endif
    unsigned int  data_size; // размер поля данных в байтах
    unsigned char data[1]; // we'll allocate more!
}libraw_processed_image_t;


//Decoded from exif and used in calculations
typedef struct
{
    char        make[64];
    char        model[64];

    unsigned    raw_count;
    unsigned    dng_version;
    unsigned    is_foveon;
    int         colors;

    unsigned    filters; // camera CFA pattern mask
    char        cdesc[5];

}libraw_iparams_t;

typedef struct
{
    ushort      raw_height, 
                raw_width, 
                height, 
                width, 
                top_margin, 
                left_margin;
    ushort      iheight,
                iwidth;
#if 0//def _OPENMP
#pragma omp firstprivate(iheight,iwidth)
#endif
    double      pixel_aspect;
    int         flip;

    // masked border sizes
    ushort      right_margin,bottom_margin; // right masked width and bottom height, inited after idendify()

} libraw_image_sizes_t;

//Phase One  data
struct ph1_t
{
    int format, key_off, t_black, black_off, split_col, tag_21a;
    float tag_210;
};


typedef struct
{
    // 32 bits total
    unsigned curve_state        : 3;
    unsigned rgb_cam_state      : 3;
    unsigned cmatrix_state      : 3;
    unsigned pre_mul_state      : 3;
    unsigned cam_mul_state      : 3;
    unsigned filler             : 17;
} color_data_state_t;

typedef struct
{
    color_data_state_t   color_flags;
    ushort      white[8][8]; // white block extracted from ciff/CRW
    float       cam_mul[4]; // camera white balance (from RAW)
    float       pre_mul[4]; // either set in identify() or calculated. Used on output
    float       cmatrix[3][4]; // camera color matrix
    float       rgb_cam[3][4]; // another way to set color matrix
    float       cam_xyz[4][3]; // Camera to XYZ matrix (DNG coeffs)
    ushort      curve[0x4001]; // camera tone curve/ljpeg curve
    unsigned    black;
    unsigned    maximum;
    struct ph1_t       phase_one_data;
    float       flash_used; // canon/CRW only
    float       canon_ev; // canon/CRW only
    char        model2[64];
    // profile
    void        *profile;
    unsigned    profile_length;
}libraw_colordata_t;

typedef struct
{
    enum LibRaw_thumbnail_formats tformat;
    ushort      twidth, 
                theight;
    unsigned    tlength;
    int         tcolors;
    
    // thumbnail buffer
    char       *thumb;
}libraw_thumbnail_t;

// Decoded from exif/raw, but not used in real calculations
typedef struct
{
    float       iso_speed; 
    float       shutter;
    float       aperture;
    float       focal_len;
    time_t      timestamp; 
    unsigned    shot_order;
    unsigned    gpsdata[32];
    // string variables
    char        desc[512],
                artist[64];
} libraw_imgother_t;

typedef struct
{
    unsigned    greybox[4];     /* -A  x1 y1 x2 y2 */
    double      aber[4];        /* -C */
    float       user_mul[4];    /* -r mul0 mul1 mul2 mul3 */
    unsigned    shot_select;    /* -s */
    float       bright;         /* -b */
    float       threshold;      /*  -n */
#if 0//def _OPENMP
#pragma omp firstprivate(threshold)
#endif
    int         half_size;      /* -h */
    int         four_color_rgb; /* -f */
    int         document_mode;  /* -d/-D */
    int         highlight;      /* -H */
//    int         verbose;      /* -v */
    int         use_auto_wb;    /* -a */
    int         use_camera_wb;  /* -w */
    int         use_camera_matrix; /* +M/-M */
    int         output_color;   /* -o */
    char        *output_profile; /* -o */
    char        *camera_profile; /* -p */
    char        *bad_pixels;    /* -P */
    char        *dark_frame;    /* -K */
    int         output_bps;     /* -4 */
    int         gamma_16bit;    /* -1 */
    int         output_tiff;    /* -T */
    int         user_flip;      /* -t */
    int         user_qual;      /* -q */
    int         user_black;     /* -k */
    int         user_sat;       /* -S */

    int         med_passes;     /* -m */
    int         no_auto_bright; /* -W */
    int         use_fuji_rotate;/* -j */
    enum LibRaw_filtering    filtering_mode; 
}libraw_output_params_t;

typedef struct
{
    ushort  *buffer; // actual pixel buffer size=(raw_width*raw_height - width*height)
    ushort  *tl;     // top left   size=(top_margin*left_margin)
    ushort  *top;    // top        size=(top_margin*width)
    ushort  *tr;     // top right  size=((raw_width-width-left_margin)*top_margin)
    ushort  *left;   // left       size=(left_margin*height)
    ushort  *right;  // right      size=(raw_width-width-left_margin)*height;
    ushort  *bl;     // bottom left size=(raw_height-height-top_margin)*left_margin
    ushort  *bottom; // bottom      size=(raw_height-height-top_margin)*width
    ushort  *br;     // bottom right size=(raw_height-height-top_margin)*
    ushort  (*ph1_black)[2]; // Phase One black
}libraw_masked_t;

typedef struct
{
    unsigned int                progress_flags;
    unsigned int                process_warnings;
    libraw_iparams_t            idata;
    libraw_image_sizes_t        sizes;
    libraw_colordata_t          color;
    libraw_imgother_t           other;
    libraw_thumbnail_t          thumbnail;
    libraw_masked_t             masked_pixels;
    ushort                      (*image)[4] ;
#if 0//def _OPENMP
#pragma omp shared(image)
#endif
    libraw_output_params_t     params;
    // pointer to LibRaw class for use in C calls
    void                *parent_class;      
} libraw_data_t;


#ifdef __cplusplus
}
#endif

#endif
