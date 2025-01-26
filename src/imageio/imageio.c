/*
    This file is part of darktable,
    Copyright (C) 2009-2024 darktable developers.

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
#include "common/colorlabels.h"
#include "common/colorspaces.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/exif.h"
#include "common/image_cache.h"
#include "common/mipmap_cache.h"
#include "common/styles.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/blend.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "imageio/imageio_common.h"
#include "imageio/imageio_module.h"
#ifdef HAVE_OPENEXR
#include "imageio/imageio_exr.h"
#endif
#ifdef HAVE_OPENJPEG
#include "imageio/imageio_j2k.h"
#endif
#ifdef HAVE_LIBJXL
#include "imageio/imageio_jpegxl.h"
#endif
#include "imageio/imageio_gm.h"
#include "imageio/imageio_im.h"
#include "imageio/imageio_jpeg.h"
#include "imageio/imageio_pfm.h"
#include "imageio/imageio_png.h"
#include "imageio/imageio_pnm.h"
#include "imageio/imageio_qoi.h"
#include "imageio/imageio_rawspeed.h"
#include "imageio/imageio_libraw.h"
#include "imageio/imageio_rgbe.h"
#include "imageio/imageio_tiff.h"
#ifdef HAVE_LIBAVIF
#include "imageio/imageio_avif.h"
#endif
#ifdef HAVE_LIBHEIF
#include "imageio/imageio_heif.h"
#endif
#ifdef HAVE_WEBP
#include "imageio/imageio_webp.h"
#endif
#include "imageio/imageio_libraw.h"

#ifdef HAVE_GRAPHICSMAGICK
#include <magick/api.h>
#include <magick/blob.h>
#elif defined HAVE_IMAGEMAGICK
  #ifdef HAVE_IMAGEMAGICK7
  #include <MagickWand/MagickWand.h>
  #else
  #include <wand/MagickWand.h>
  #endif
#endif

#include <assert.h>
#include <glib/gstdio.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifdef USE_LUA
#include "lua/image.h"
#endif

typedef enum {
  DT_FILETYPE_UNKNOWN,
  DT_FILETYPE_NONIMAGE,
  DT_FILETYPE_BMP,
  DT_FILETYPE_DJVU,
  DT_FILETYPE_FITS,
  DT_FILETYPE_GIF,
  DT_FILETYPE_JPEG,
  DT_FILETYPE_JPEG2000,
  DT_FILETYPE_PNG,
  DT_FILETYPE_PNM,
  DT_FILETYPE_QOI,
  DT_FILETYPE_TIFF,
  DT_FILETYPE_BIGTIFF,
  DT_FILETYPE_WEBP,
  DT_FILETYPE_OTHER_LDR,
  DT_FILETYPE_AVIF,
  DT_FILETYPE_HEIC,
  DT_FILETYPE_JPEGXL,
  DT_FILETYPE_OPENEXR,
  DT_FILETYPE_PFM,
  DT_FILETYPE_RGBE,
  DT_FILETYPE_OTHER_HDR,
  DT_FILETYPE_ARW,	// Sony Alpha
  DT_FILETYPE_CRW,	// Canon
  DT_FILETYPE_CR2,
  DT_FILETYPE_CR3,
  DT_FILETYPE_ERF,	// Epson - files are TIFF/EP
  DT_FILETYPE_IIQ,	// Leaf/PhaseOne - TIFF with extra magic
  DT_FILETYPE_KODAK,
  DT_FILETYPE_MRW,	// Minolta
  DT_FILETYPE_NEF,	// Nikon
  DT_FILETYPE_ORF,	// Olympus - TIFF with custom magic at start
  DT_FILETYPE_PEF,	// Pentax
  DT_FILETYPE_RAF,	// Fujifilm
  DT_FILETYPE_RW2,	// Panasonic
  DT_FILETYPE_SRW,
  DT_FILETYPE_X3F,	// Sigma Foveon
  DT_FILETYPE_OTHER_RAW,
  DT_FILETYPE_DNG,
} dt_filetype_t;

// the longest prefix of the file we want to be able to examine
#define MAX_SIGNATURE 512
// the longest string of magic bytes
#define MAX_MAGIC 32

// declare the image-loading function's type
typedef dt_imageio_retval_t dt_image_loader_fn_t(dt_image_t *img,
                                                 const char *filename,
                                                 dt_mipmap_buffer_t *buf);

// a surrogate loader function for any types whose libraries haven't been linked while building
static dt_imageio_retval_t _unsupported_type(dt_image_t *img,
                                              const char *filename,
                                              dt_mipmap_buffer_t *buf)
{
  return DT_IMAGEIO_UNSUPPORTED_FORMAT;
}

// redirect loaders to surrogate as needed
#ifndef HAVE_OPENJPEG
#define dt_imageio_open_j2k _unsupported_type
#endif

#ifndef HAVE_WEBP
#define dt_imageio_open_webp _unsupported_type
#endif

#ifndef HAVE_LIBJXL
#define dt_imageio_open_jpegxl _unsupported_type
#endif

#ifndef HAVE_LIBAVIF
#define dt_imageio_open_avif _unsupported_type
#endif

#ifndef HAVE_LIBHEIF
#define dt_imageio_open_heif _unsupported_type
#endif

#ifndef HAVE_GRAPHICSMAGICK
#define dt_imageio_open_gm _unsupported_type
#endif

#ifndef HAVE_IMAGESMAGICK
#define dt_imageio_open_im _unsupported_type
#endif

#ifndef HAVE_OPENEXR
#define dt_imageio_open_exr _unsupported_type
#endif

typedef struct {
  dt_filetype_t filetype;
  gboolean      hdr;
  unsigned      offset;	           // start offset of signature in file
  unsigned      length;	           // length of signature in bytes
  dt_image_loader_fn_t *loader;	   // the function with which to load the image (NULL if special handling needed)
  gchar         magic[MAX_MAGIC];  // the actual signature bytes
  const char   *searchstring;	   // sub-signature which might be anywhere in first 512 bytes of file
} dt_magic_bytes_t;

// the signatures for the file types we know about.  More specific ones need to come before
// less specific ones, e.g. TIFF needs to come after DNG and nearly all camera formats, since
// the latter are all TIFF containers
// various signatures were found in magic/Magdir/images from https://gibhub.com/file/file and at
// https://en.wikipedia.org/wiki/List_of_file_signatures,
// https://libopenraw.freedesktop.org/formats/,
// https://www.garykessler.net/library/file_sigs.html, and
// https://www.iana.org/assignments/media-types/media-types.xhtml#image
static const dt_magic_bytes_t _magic_signatures[] = {
  // FITS image
  { DT_FILETYPE_FITS, FALSE, 0, 9, dt_imageio_open_exotic,
    { 'S', 'I', 'M', 'P', 'L', 'E', ' ', ' ', '=' } },
  // GIF image
  { DT_FILETYPE_GIF, FALSE, 0, 4, dt_imageio_open_exotic,
    { 'G', 'I', 'F', '8' } },
  // JPEG
  { DT_FILETYPE_JPEG, FALSE, 0, 3, dt_imageio_open_jpeg,
    { 0xFF, 0xD8, 0xFF } }, // SOI marker
  // JPEG-2000, j2k format
  { DT_FILETYPE_JPEG2000, FALSE, 0, 4, dt_imageio_open_j2k,
    { 0xFF, 0x4F, 0xFF, 0x51 } },
  // JPEG-2000, jp2 format
  { DT_FILETYPE_JPEG2000, FALSE, 0, 12, dt_imageio_open_j2k,
    { 0x00, 0x00, 0x00, 0x0C, 'j', 'P', ' ', ' ', 0x0D, 0x0A, 0x87, 0x0A } },
  // JPEG-XL image (direct codestream)
  { DT_FILETYPE_JPEGXL, TRUE, 0, 2, dt_imageio_open_jpegxl,
    { 0xFF, 0x0A } },
  // JPEG-XL image (ISOBMFF container)
  { DT_FILETYPE_JPEGXL, TRUE, 0, 12, dt_imageio_open_jpegxl,
    { 0x00, 0x00, 0x00, 0x0C, 'J', 'X', 'L', ' ', 0x0D, 0x0A, 0x87, 0x0A } },
  // PNG image
  { DT_FILETYPE_PNG, FALSE, 0, 5, dt_imageio_open_png,
    { 0x89, 'P', 'N', 'G', 0x0D } },
  // WEBP image
  { DT_FILETYPE_WEBP, FALSE, 8, 4, dt_imageio_open_webp,
    { 'W', 'E', 'B', 'P' } },  // full signature is RIFF????WEPB, where ???? is the file size
  // HEIC/HEIF image
  { DT_FILETYPE_HEIC, FALSE, 4, 8, dt_imageio_open_heif,
    { 'f', 't', 'y', 'p', 'h', 'e', 'i', 'c' } },
  { DT_FILETYPE_HEIC, TRUE, 4, 8, dt_imageio_open_heif,
    { 'f', 't', 'y', 'p', 'h', 'e', 'i', 'x' } }, // 10-bit
  { DT_FILETYPE_HEIC, FALSE, 4, 8, dt_imageio_open_heif,
    { 'f', 't', 'y', 'p', 'j', '2', 'k', 'i' } }, // JPEG 2000 encapsulated in HEIF
  { DT_FILETYPE_HEIC, FALSE, 4, 8, dt_imageio_open_heif,
    { 'f', 't', 'y', 'p', 'a', 'v', 'c', 'i' } }, // AVC (H.264) encoded HEIF
  // AVIF image
  { DT_FILETYPE_AVIF, TRUE, 4, 8, dt_imageio_open_avif,
    { 'f', 't', 'y', 'p', 'a', 'v', 'i', 'f' } },
//  { DT_FILETYPE_AVIF, TRUE, 4, 8, dt_imageio_open_avif,
//    { 'f', 't', 'y', 'p', 'm', 'i', 'f', '1' } },  //alternate? HEIF or AVIF, depending on bytes 16-19
  // Quite OK Image Format (QOI)
  { DT_FILETYPE_QOI, FALSE, 0, 4, dt_imageio_open_qoi,
    { 'q', 'o', 'i', 'f' } },
  // OpenEXR image
  { DT_FILETYPE_OPENEXR, TRUE, 0, 4, dt_imageio_open_exr,
    { 'v', '/', '1', 0x01 } },
  // RGBE (.hdr)  image
  { DT_FILETYPE_RGBE, TRUE, 0, 11, dt_imageio_open_rgbe,
    { '#', '?', 'R', 'A', 'D', 'I', 'A', 'N', 'C', 'E', 0x0A } },
  { DT_FILETYPE_RGBE, TRUE, 0, 7, dt_imageio_open_rgbe,
    { '#', '?', 'R', 'G', 'B', 'E', 0x0A } },
  // original v1 CRW
  { DT_FILETYPE_CRW, TRUE, 0, 14, dt_imageio_open_rawspeed,
    { 'I', 'I', 0x1A, 0x00, 0x00, 0x00, 'H', 'E', 'A', 'P', 'C', 'C', 'D', 'R' } },
  // most CR2
  { DT_FILETYPE_CR2, TRUE, 0, 10, dt_imageio_open_rawspeed,
    { 'I', 'I', '*', 0x00, 0x10, 0x00, 0x00, 0x00, 'C', 'R' } },
  // CR3 (ISOBMFF)
  { DT_FILETYPE_CR3, TRUE, 0, 24, dt_imageio_open_libraw,
    { 0x00, 0x00, 0x00, 0x18, 'f', 't', 'y', 'p', 'c', 'r', 'x', ' ',
      0x00, 0x00, 0x00, 0x01, 'c', 'r', 'x', ' ', 'i', 's', 'o', 'm' } },
  // older Canon RAW formats using TIF extension
  { DT_FILETYPE_CRW, TRUE, 0, 10, dt_imageio_open_rawspeed,
    { 'I', 'I', '*', 0x00, 0x00, 0x03, 0x00, 0x00, 0xFF, 0x01 } }, // i.e. DCS1
  { DT_FILETYPE_CRW, TRUE, 0, 10, dt_imageio_open_rawspeed,
    { 'M', 'M', 0x00, '*', 0x00, 0x00, 0x00, 0x10, 0xBA, 0xB0 } }, // i.e. 1D, 1Ds
  { DT_FILETYPE_CRW, TRUE, 0, 10, dt_imageio_open_rawspeed,
    { 'M', 'M', 0x00, '*', 0x00, 0x00, 0x11, 0x34, 0x00, 0x04 } }, // i.e. D2000
  // older Kodak RAW formats using TIF extension
  { DT_FILETYPE_KODAK, TRUE, 0, 10, dt_imageio_open_rawspeed,
    { 'I', 'I', '*', 0x00, 0x00, 0x03, 0x00, 0x00, 0x7C, 0x01 } }, // i.e. DCS460D
  { DT_FILETYPE_KODAK, TRUE, 0, 10, dt_imageio_open_rawspeed,
    { 'M', 'M', 0x00, '*', 0x00, 0x00, 0x11, 0xA8, 0x00, 0x04 } }, // i.e. DCS520C
  { DT_FILETYPE_KODAK, TRUE, 0, 10, dt_imageio_open_rawspeed,
    { 'M', 'M', 0x00, '*', 0x00, 0x00, 0x11, 0x76, 0x00, 0x04 } }, // i.e. DCS560C
  // IIQ raw images, may use either .IIQ or .TIF extension
  { DT_FILETYPE_IIQ, TRUE, 8, 4, dt_imageio_open_rawspeed,
    { 'I', 'I', 'I', 'I' } },
  // Fujifilm RAF
  { DT_FILETYPE_RAF, TRUE, 0, 15, dt_imageio_open_rawspeed,
    { 'F', 'U', 'J', 'I', 'F', 'I', 'L', 'M', 'C', 'C', 'D', '-', 'R', 'A', 'W' }},
  // Minolta MRW file
  { DT_FILETYPE_MRW, TRUE, 0, 4, dt_imageio_open_rawspeed,
    { 0x00, 'M', 'R', 'M' } },
  // Olympus ORF file
  { DT_FILETYPE_ORF, TRUE, 0, 4, dt_imageio_open_rawspeed,
    { 'I', 'I', 'R', 'O' } },   // most Olympus models
  { DT_FILETYPE_ORF, TRUE, 0, 4, dt_imageio_open_rawspeed,
    { 'I', 'I', 'R', 'S' } },	// C7070WZ
  { DT_FILETYPE_ORF, TRUE, 0, 4, dt_imageio_open_rawspeed,
    { 'M', 'M', 'O', 'R' } },   // E-10
  // Panasonic RW2 file
  { DT_FILETYPE_RW2, TRUE, 0, 8, dt_imageio_open_rawspeed,
    { 'I', 'I', 'U', 0x00, 0x08, 0x00, 0x00, 0x00 } },
  // Sigma Foveon X3F file
  { DT_FILETYPE_X3F, TRUE, 0, 4, NULL,
    { 'F', 'O', 'V', 'b' } },
  // Nikon NEF files are TIFFs with (usually) the string "NIKON CORP" early in the file
  { DT_FILETYPE_NEF, FALSE, 0, 4, dt_imageio_open_rawspeed,
    { 'I', 'I', '*', 0x00 }, "NIKON CORP" },
  { DT_FILETYPE_NEF, FALSE, 0, 4, dt_imageio_open_rawspeed,
    { 'M', 'M', 0x00, '*' }, "NIKON CORP" },
  // Epson ERF files are TIFFs with the string "EPSON" early in the file
  { DT_FILETYPE_ERF, FALSE, 0, 4, dt_imageio_open_rawspeed,
    { 'I', 'I', '*', 0x00 }, "EPSON" },
  { DT_FILETYPE_ERF, FALSE, 0, 4, dt_imageio_open_rawspeed,
    { 'M', 'M', 0x00, '*' }, "EPSON" },
  // Pentax/Ricoh PEF files are TIFFs with the string "PENTAX" early in the file
  { DT_FILETYPE_PEF, FALSE, 0, 4, dt_imageio_open_rawspeed,
    { 'I', 'I', '*', 0x00 }, "PENTAX" },
  { DT_FILETYPE_PEF, FALSE, 0, 4, dt_imageio_open_rawspeed,
    { 'M', 'M', 0x00, '*' }, "PENTAX" },
  // Samsung SRW files are TIFFs with the string "SAMSUNG" early in the file
  { DT_FILETYPE_SRW, FALSE, 0, 4, dt_imageio_open_rawspeed,
    { 'I', 'I', '*', 0x00 }, "SAMSUNG" },
  { DT_FILETYPE_SRW, FALSE, 0, 4, dt_imageio_open_rawspeed,
    { 'M', 'M', 0x00, '*' }, "SAMSUNG" },
  // Sony ARW files are TIFFs with the string "SONY" early in the file
  { DT_FILETYPE_ARW, FALSE, 0, 4, dt_imageio_open_rawspeed,
    { 'I', 'I', '*', 0x00 }, "SONY" },
  { DT_FILETYPE_ARW, FALSE, 0, 4, dt_imageio_open_rawspeed,
    { 'M', 'M', 0x00, '*' }, "SONY" },
  // little-endian (Intel) TIFF
  { DT_FILETYPE_TIFF, FALSE, 0, 4, NULL, // may be DNG or any of many camera raw types
    { 'I', 'I', '*', 0x00 } },
  // big-endian (Motorola) TIFF
  { DT_FILETYPE_TIFF, FALSE, 0, 4, NULL, // may be DNG or any of many camera raw types
    { 'M', 'M', 0x00, '*' } },
  // little-endian (Intel) BigTIFF
  { DT_FILETYPE_BIGTIFF, FALSE, 0, 4, dt_imageio_open_tiff,
    { 'I', 'I', '+', 0x00 } },
  // big-endian (Motorola) BigTIFF
  { DT_FILETYPE_BIGTIFF, FALSE, 0, 4, dt_imageio_open_tiff,
    { 'M', 'M', 0x00, '+' } },
  // GIMP .xcf file
  { DT_FILETYPE_OTHER_LDR, FALSE, 0, 8, dt_imageio_open_exotic,
    { 'g', 'i', 'm', 'p', ' ', 'x', 'c', 'f' } },
  // X PixMap
  { DT_FILETYPE_OTHER_LDR, FALSE, 0, 9, dt_imageio_open_exotic,
    { '/', '*', ' ', 'X', 'P', 'M', ' ', '*', '/' } },
  // MNG image (multi-image/animated PNG)
  { DT_FILETYPE_OTHER_LDR, FALSE, 0, 8, dt_imageio_open_exotic,
    { 0x8A, 'M', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A } },
  // JNG image (MNG lossy-compressed with JPEG)
  { DT_FILETYPE_OTHER_LDR, FALSE, 0, 8, dt_imageio_open_exotic,
    { 0x8B, 'J', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A } },
  // Kodak Cineon image
  { DT_FILETYPE_OTHER_LDR, FALSE, 0, 4, dt_imageio_open_exotic,
    { 0x80, 0x2A, 0x5F, 0xD7 } },
  // ASCII NetPNM (pbm)
  { DT_FILETYPE_PNM, FALSE, 0, 3, dt_imageio_open_exotic,
    { 'P', '1', 0x0A } },
  // ASCII NetPNM (pgm)
  { DT_FILETYPE_PNM, FALSE, 0, 3, dt_imageio_open_exotic,
    { 'P', '2', 0x0A } },
  // ASCII NetPNM (ppm)
  { DT_FILETYPE_PNM, FALSE, 0, 3, dt_imageio_open_exotic,
    { 'P', '3', 0x0A } },
  // binary NetPNM (pbm)
  { DT_FILETYPE_PNM, FALSE, 0, 3, dt_imageio_open_pnm,
    { 'P', '4', 0x0A } },
  // binary NetPNM (pgm)
  { DT_FILETYPE_PNM, FALSE, 0, 3, dt_imageio_open_pnm,
    { 'P', '5', 0x0A } },
  // binary NetPNM (ppm)
  { DT_FILETYPE_PNM, FALSE, 0, 3, dt_imageio_open_pnm,
    { 'P', '6', 0x0A } },
  // binary NetPNM "Portable Arbitrary Map" (pam)
  { DT_FILETYPE_PNM, FALSE, 0, 3, dt_imageio_open_exotic,
    { 'P', '7', 0x0A } },
  // Windows BMP bitmap image
  { DT_FILETYPE_BMP, FALSE, 0, 2, dt_imageio_open_exotic,
    { 'B', 'M' } },
  // Portable float map (PFM) image
  { DT_FILETYPE_PFM, TRUE, 0, 2, dt_imageio_open_pfm,
    { 'P', 'F' } },  // color
  { DT_FILETYPE_PFM, TRUE, 0, 2, dt_imageio_open_pfm,
    { 'P', 'f' } },  // grayscale
  // DjVu -- additional checks needed
  { DT_FILETYPE_DJVU, TRUE, 4, 4, dt_imageio_open_exotic,
    { 'F', 'O', 'R', 'M' } },
  // ========= other image types which we may not support ==========
  // Corel Paint Shop Pro image
  { DT_FILETYPE_OTHER_LDR, FALSE, 0, 4, dt_imageio_open_exotic,
    { '~', 'B', 'K', 0x00 } },
  // DPX image (big endian)
  { DT_FILETYPE_OTHER_LDR, FALSE, 0, 4, dt_imageio_open_exotic,
    { 'S', 'D', 'P', 'X' } },
  // DPX image (little endian)
  { DT_FILETYPE_OTHER_LDR, FALSE, 0, 4, dt_imageio_open_exotic,
    { 'X', 'P', 'D', 'S' } },
  // FBM (Fuzzy Bitmap) image
  { DT_FILETYPE_OTHER_LDR, FALSE, 0, 7, dt_imageio_open_exotic,
    { '%', 'b', 'i', 't', 'm', 'a', 'p' } },
  // Free Lossless Image Format
  { DT_FILETYPE_OTHER_LDR, FALSE, 0, 4, dt_imageio_open_exotic,
    { 'F', 'L', 'I', 'F' } },
  //  JBIG2 image
  { DT_FILETYPE_OTHER_LDR, FALSE, 0, 8, dt_imageio_open_exotic,
    { 0x97, 'J', 'B', '2', 0x0D, 0x0A, 0x1A, 0x0A } },
  // Paint.NET image
  { DT_FILETYPE_OTHER_LDR, FALSE, 0, 4, dt_imageio_open_exotic,
    { 'P', 'D', 'N', '3' } },
  // Photoshop Document
  { DT_FILETYPE_OTHER_LDR, FALSE, 0, 4, dt_imageio_open_exotic,
    { '8', 'B', 'P', 'S' } },
  // AutoCAD .DWG (drawing) file
  { DT_FILETYPE_OTHER_LDR, FALSE, 0, 3, dt_imageio_open_exotic,
    { 'A', 'C', '1' } },
  // DICOM medical file format
  { DT_FILETYPE_OTHER_LDR, FALSE, 128, 4, dt_imageio_open_exotic,
    { 'D', 'I', 'C', 'M' } },
  // Encapsulated Postscript file
  { DT_FILETYPE_OTHER_LDR, FALSE, 0, 13, dt_imageio_open_exotic,
    { '%', '!', 'P', 'S', '-', 'A', 'd', 'o', 'b', 'e', '-', '3', '.' } },
  // JPEG-XR image
  { DT_FILETYPE_OTHER_LDR, TRUE, 0, 4, _unsupported_type,
    { 'I', 'I', 0xBC, 1 } },
  // JPEG-XS image
  { DT_FILETYPE_OTHER_LDR, TRUE, 0, 12, _unsupported_type,
    { 0x00, 0x00, 0x00, 0x0C, 'J', 'X', 'S', ' ', 0x0D, 0x0A, 0x87, 0x0A } },
  // ========= common non-image file formats, useful for detecting misnamed files =========
  // Zip archive, includes most modern document formats
  { DT_FILETYPE_NONIMAGE, FALSE, 0, 4, _unsupported_type,
    { 'P', 'K', 0x03, 0x04 } },
  { DT_FILETYPE_NONIMAGE, FALSE, 0, 4, _unsupported_type,
    { 'P', 'K', 0x05, 0x06 } }, // empty archive
  { DT_FILETYPE_NONIMAGE, FALSE, 0, 4, _unsupported_type,
    { 'P', 'K', 0x07, 0x08 } }, // spanned archive
  // gzip compressed file
  { DT_FILETYPE_NONIMAGE, FALSE, 0, 2, _unsupported_type,
    { 0x1F, 0x8B } },
  // xz compressed file
  { DT_FILETYPE_NONIMAGE, FALSE, 0, 5, _unsupported_type,
    { 0xFD, '7', 'z', 'X', 'Z' } },
  // bzip2 compressed file
  { DT_FILETYPE_NONIMAGE, FALSE, 0, 3, _unsupported_type,
    { 'B', 'Z', 'h' } },
  // 7-Zip compressed file
  { DT_FILETYPE_NONIMAGE, FALSE, 0, 6, _unsupported_type,
    { '7', 'z', 0xBC, 0xAF, 0x27, 0x1C } },
  // Zstandard compressed file
  { DT_FILETYPE_NONIMAGE, FALSE, 0, 4, _unsupported_type,
    { 0x28, 0xB5, 0x2F, 0xFD } },
  // XML file, such as .xmp sidecars
  { DT_FILETYPE_NONIMAGE, FALSE, 0, 5, _unsupported_type,
    { '<', '?', 'x', 'm', 'l' } },  // UTF-8
  { DT_FILETYPE_NONIMAGE, FALSE, 3, 5, _unsupported_type,
    { '<', '?', 'x', 'm', 'l' } },  // UTF-8 with BOM
  { DT_FILETYPE_NONIMAGE, FALSE, 0, 10, _unsupported_type,
    { '<', 0, '?', 0, 'x', 0, 'm', 0, 'l', 0 } }, // UTF-16LE
  { DT_FILETYPE_NONIMAGE, FALSE, 0, 5, _unsupported_type,
    { 0, '<', 0, '?', 0, 'x', 0, 'm', 0, 'l' } }, // UTF-16BE
  // GPX track file
  { DT_FILETYPE_NONIMAGE, FALSE, 0, 5, _unsupported_type,
    { '<', 'g', 'p', 'x', ' ' } },
  // MPEG-4 video
  { DT_FILETYPE_NONIMAGE, FALSE, 4, 8, _unsupported_type,
    { 'f', 't', 'y', 'p', 'M', 'S', 'N', 'V' } },
  // Flash Video
  { DT_FILETYPE_NONIMAGE, FALSE, 0, 3, _unsupported_type,
    { 'F', 'L', 'V' } },
  // .WAV, .AVI, CorelShow!, or MacroMind movie file
  { DT_FILETYPE_NONIMAGE, FALSE, 0, 4, _unsupported_type,
    { 'R', 'I', 'F', 'F' } },
  // Ogg container for audio/video
  { DT_FILETYPE_NONIMAGE, FALSE, 0, 4, _unsupported_type,
    { 'O', 'g', 'g', 'S' } },
  // Postscript document
  { DT_FILETYPE_NONIMAGE, FALSE, 0, 4, _unsupported_type,
    { '%', '!', 'P', 'S' } },
  // UTF-8 text file with BOM
  { DT_FILETYPE_NONIMAGE, FALSE, 0, 3, _unsupported_type,
    { 0xEF, 0xBB, 0xBF } },
  // PDF document
  { DT_FILETYPE_NONIMAGE, FALSE, 0, 5, _unsupported_type,
    { '%', 'P', 'D', 'F', '-' } },
  // HTML file
  { DT_FILETYPE_NONIMAGE, FALSE, 0, 5, _unsupported_type,
    { '<', 'H', 'T', 'M', 'L' } },
  { DT_FILETYPE_NONIMAGE, FALSE, 0, 5, _unsupported_type,
    { '<', 'h', 't', 'm', 'l' } }
};

// signatures which require additional checks before acceptance
static dt_magic_bytes_t _windows_BMP_signature = { DT_FILETYPE_BMP, FALSE, 0, 2, NULL, { 40, 0 } };

// Note: 'dng' is not included as it can contain anything. We will
// need to open and examine dng images to find out the type of
// content.
static const gchar *_supported_raw[]
    = { "3fr", "ari", "arw", "bay", "cr2", "cr3", "crw", "dc2", "dcr", "erf", "fff",
        "ia",  "iiq", "k25", "kc2", "kdc", "mdc", "mef", "mos", "mrw", "nef", "nrw",
        "orf", "ori", "pef", "raf", "raw", "rw2", "rwl", "sr2", "srf", "srw", "sti",
        "x3f", NULL };
static const gchar *_supported_ldr[]
    = { "bmp", "bmq", "cap", "cin", "cine", "cs1", "dcm",  "gif", "gpr", "j2c",  "j2k",
        "jng", "jp2", "jpc", "jpeg", "jpg", "miff", "mng", "pbm", "pfm",  "pgm",
        "png", "pnm", "ppm", "pxn",  "qoi", "qtk",  "rdc", "tif", "tiff", "webp",
        NULL };
static const gchar *_supported_hdr[]
    = { "avif", "exr", "hdr", "heic", "heif", "hif", "jxl", "pfm", NULL };

static inline gboolean _image_handled(dt_imageio_retval_t ret)
{
  return ret == DT_IMAGEIO_OK || ret == DT_IMAGEIO_CACHE_FULL || ret == DT_IMAGEIO_UNSUPPORTED_FEATURE;
}

static gboolean _memfind(const char *needle, const char *haystack, size_t hs_len)
{
  if(!needle)
    return FALSE;
  const size_t n_len = strlen(needle);
  for(size_t offset = 0; offset < hs_len - n_len; offset++)
  {
    if(haystack[offset] == needle[0] && memcmp(haystack + offset, needle, n_len) == 0)
      return TRUE;
  }
  return FALSE;
}

static const dt_magic_bytes_t *_find_signature(const char *filename)
{
  if(!filename || !*filename)
    return NULL;
  FILE *fin = g_fopen(filename, "rb");
  if(!fin)
    return NULL;
  // read possible signatur block from file
  gchar magicbuf[MAX_SIGNATURE];
  memset(magicbuf, '\0', sizeof(magicbuf));
  size_t count = fread(magicbuf, 1, sizeof(magicbuf), fin);
  fclose(fin);
  if(count < MAX_MAGIC)
    return NULL;
  for(size_t i = 0; i < sizeof(_magic_signatures)/sizeof(_magic_signatures[0]); i++)
  {
    const dt_magic_bytes_t *info = &_magic_signatures[i];
    if(memcmp(magicbuf + info->offset, info->magic, info->length) == 0)
    {
      if(info->searchstring && !_memfind(info->searchstring, magicbuf, sizeof(magicbuf)))
        continue;  // not a match after all
      // any extra checks go here, e.g. if detected as TIFF, try to determine which camera RAW it is
      if(info->filetype == DT_FILETYPE_DJVU)
      {
        // verify that this is actually a DjVu file by checking the secondary signature
        if(memcmp(magicbuf + 12, "DJVU", 4) != 0 && memcmp(magicbuf + 12, "DJVM", 4) != 0 &&
           memcmp(magicbuf + 12, "BM44", 4) != 0)
          continue;
      }
      return info;
    }
  }
  // alternate signature for BMP
  if(magicbuf[0] == 40 && magicbuf[1] == 0 && magicbuf[12] == 1 && magicbuf[13] == 0)
    return &_windows_BMP_signature;
  return NULL;
}

static dt_imageio_retval_t _open_by_magic_number(dt_image_t *img, const char *filename, dt_mipmap_buffer_t *buf)
{
  const dt_magic_bytes_t *sig = _find_signature(filename);
  if(sig && sig->loader)
    return sig->loader(img, filename, buf);
  return DT_IMAGEIO_UNRECOGNIZED;
}

gboolean dt_imageio_is_raw_by_extension(const char *extension)
{
  const char *ext = g_str_has_prefix(extension, ".") ? extension + 1 : extension;
  for(const char **i = _supported_raw; *i != NULL; i++)
  {
    if(!g_ascii_strcasecmp(ext, *i))
      return TRUE;
  }
  return FALSE;
}

// get the type of image from its extension
dt_image_flags_t dt_imageio_get_type_from_extension(const char *extension)
{
  const char *ext = g_str_has_prefix(extension, ".") ? extension + 1 : extension;
  for(const char **i = _supported_raw; *i != NULL; i++)
  {
    if(!g_ascii_strcasecmp(ext, *i))
    {
      return DT_IMAGE_RAW;
    }
  }
  for(const char **i = _supported_hdr; *i != NULL; i++)
  {
    if(!g_ascii_strcasecmp(ext, *i))
    {
      return DT_IMAGE_HDR;
    }
  }
  for(const char **i = _supported_ldr; *i != NULL; i++)
  {
    if(!g_ascii_strcasecmp(ext, *i))
    {
      return DT_IMAGE_LDR;
    }
  }
  // default to 0
  return 0;
}

// load a full-res thumbnail:
gboolean dt_imageio_large_thumbnail(const char *filename,
                                    uint8_t **buffer,
                                    int32_t *width,
                                    int32_t *height,
                                    dt_colorspaces_color_profile_type_t *color_space)
{
  int res = TRUE;

  uint8_t *buf = NULL;
  char *mime_type = NULL;
  size_t bufsize;

  // get the biggest thumb from exif
  if(dt_exif_get_thumbnail(filename, &buf, &bufsize, &mime_type))
    goto error;

  if(strcmp(mime_type, "image/jpeg") == 0)
  {
    // Decompress the JPG into our own memory format
    dt_imageio_jpeg_t jpg;
    if(dt_imageio_jpeg_decompress_header(buf, bufsize, &jpg))
      goto error;

    *buffer = dt_alloc_align_uint8(4 * jpg.width * jpg.height);
    if(!*buffer) goto error;

    *width = jpg.width;
    *height = jpg.height;
    // TODO: check if the embedded thumbs have a color space set!
    // currently we assume that it's always sRGB
    *color_space = DT_COLORSPACE_SRGB;
    if(dt_imageio_jpeg_decompress(&jpg, *buffer))
    {
      dt_free_align(*buffer);
      *buffer = NULL;
      goto error;
    }

    res = FALSE;
  }
  else
  {
#ifdef HAVE_GRAPHICSMAGICK
    ExceptionInfo exception;
    Image *image = NULL;
    ImageInfo *image_info = NULL;

    GetExceptionInfo(&exception);
    image_info = CloneImageInfo((ImageInfo *)NULL);

    image = BlobToImage(image_info, buf, bufsize, &exception);

    if(exception.severity != UndefinedException)
      CatchException(&exception);

    if(!image)
    {
      dt_print(DT_DEBUG_ALWAYS,
               "[dt_imageio_large_thumbnail GM] thumbnail not found?");
      goto error_gm;
    }

    *width = image->columns;
    *height = image->rows;
    *color_space = DT_COLORSPACE_SRGB; // FIXME: this assumes that
                                       // embedded thumbnails are
                                       // always srgb

    *buffer = dt_alloc_align_uint8(4 * image->columns * image->rows);
    if(!*buffer) goto error_gm;

    for(uint32_t row = 0; row < image->rows; row++)
    {
      uint8_t *bufprt = *buffer + (size_t)4 * row * image->columns;
      const int gm_ret = DispatchImage(image, 0, row, image->columns, 1, "RGBP",
                                       CharPixel, bufprt, &exception);

      if(exception.severity != UndefinedException) CatchException(&exception);

      if(gm_ret != MagickPass)
      {
        dt_print(DT_DEBUG_ALWAYS,
                 "[dt_imageio_large_thumbnail GM] error_gm reading thumbnail");
        dt_free_align(*buffer);
        *buffer = NULL;
        goto error_gm;
      }
    }

    res = FALSE;

  error_gm:
    if(image) DestroyImage(image);
    if(image_info) DestroyImageInfo(image_info);
    DestroyExceptionInfo(&exception);
    if(res) goto error;
#elif defined HAVE_IMAGEMAGICK
    MagickWand *image = NULL;
    MagickBooleanType mret;

    image = NewMagickWand();
    mret = MagickReadImageBlob(image, buf, bufsize);

    if(mret != MagickTrue)
    {
      dt_print(DT_DEBUG_ALWAYS,
               "[dt_imageio_large_thumbnail IM] thumbnail not found?");
      goto error_im;
    }

    *width = MagickGetImageWidth(image);
    *height = MagickGetImageHeight(image);

    switch(MagickGetImageColorspace(image))
    {
      case sRGBColorspace:
        *color_space = DT_COLORSPACE_SRGB;
        break;
      default:
        dt_print(DT_DEBUG_ALWAYS,
                 "[dt_imageio_large_thumbnail IM] could not map colorspace, using sRGB");
        *color_space = DT_COLORSPACE_SRGB;
        break;
    }

    *buffer = malloc(sizeof(uint8_t) * (*width) * (*height) * 4);
    if(*buffer == NULL)
      goto error_im;

    mret = MagickExportImagePixels(image, 0, 0, *width, *height, "RGBP",
                                   CharPixel, *buffer);
    if(mret != MagickTrue)
    {
      free(*buffer);
      *buffer = NULL;
      dt_print(DT_DEBUG_ALWAYS,
          "[dt_imageio_large_thumbnail IM] error while reading thumbnail");
      goto error_im;
    }

    res = FALSE;

error_im:
    DestroyMagickWand(image);
    if(res) goto error;
#else
    dt_print(DT_DEBUG_ALWAYS,
      "[dt_imageio_large_thumbnail] error: The thumbnail image is not in "
      "JPEG format, and DT was built without neither GraphicsMagick or "
      "ImageMagick. Please rebuild DT with GraphicsMagick or ImageMagick "
      "support enabled.\n");
#endif
  }

  if(res)
  {
    dt_print(DT_DEBUG_ALWAYS,
             "[dt_imageio_large_thumbnail] error: Not a supported thumbnail "
             "image format or broken thumbnail: %s",
             mime_type);
    goto error;
  }

error:
  free(mime_type);
  free(buf);
  return res;
}

gboolean dt_imageio_has_mono_preview(const char *filename)
{
  dt_colorspaces_color_profile_type_t color_space;
  uint8_t *tmp = NULL;
  int32_t thumb_width = 0, thumb_height = 0;
  gboolean mono = FALSE;

  if(dt_imageio_large_thumbnail(filename, &tmp, &thumb_width,
                                &thumb_height, &color_space))
    goto cleanup;
  if((thumb_width < 32) || (thumb_height < 32) || (tmp == NULL))
    goto cleanup;

  mono = TRUE;
  for(int y = 0; y < thumb_height; y++)
  {
    uint8_t *in = (uint8_t *)tmp + (size_t)4 * y * thumb_width;
    for(int x = 0; x < thumb_width; x++, in += 4)
    {
      if((in[0] != in[1]) || (in[0] != in[2]) || (in[1] != in[2]))
      {
        mono = FALSE;
        goto cleanup;
      }
    }
  }

  cleanup:

  dt_print(DT_DEBUG_IMAGEIO,
           "[dt_imageio_has_mono_preview] testing `%s', monochrome=%s, %ix%i",
           filename, mono ? "YES" : "FALSE", thumb_width, thumb_height);
  dt_free_align(tmp);
  return mono;
}

void dt_imageio_flip_buffers(char *out,
                             const char *in,
                             const size_t bpp,
                             const int wd,
                             const int ht,
                             const int fwd,
                             const int fht,
                             const int stride,
                             const dt_image_orientation_t orientation)
{
  if(!orientation)
  {
    DT_OMP_FOR()
    for(int j = 0; j < ht; j++)
      memcpy(out + (size_t)j * bpp * wd, in + (size_t)j * stride, bpp * wd);
    return;
  }
  int ii = 0;
  int jj = 0;
  int si = bpp;
  int sj = wd * bpp;
  if(orientation & ORIENTATION_SWAP_XY)
  {
    sj = bpp;
    si = ht * bpp;
  }
  if(orientation & ORIENTATION_FLIP_Y)
  {
    jj = (int)fht - jj - 1;
    sj = -sj;
  }
  if(orientation & ORIENTATION_FLIP_X)
  {
    ii = (int)fwd - ii - 1;
    si = -si;
  }
  DT_OMP_FOR()
  for(int j = 0; j < ht; j++)
  {
    char *out2 = out + (size_t)labs(sj) * jj + (size_t)labs(si) * ii + (size_t)sj * j;
    const char *in2 = in + (size_t)stride * j;
    for(int i = 0; i < wd; i++)
    {
      memcpy(out2, in2, bpp);
      in2 += bpp;
      out2 += si;
    }
  }
}

void dt_imageio_flip_buffers_ui8_to_float(float *out,
                                          const uint8_t *in,
                                          const float black,
                                          const float white,
                                          const int ch,
                                          const int wd,
                                          const int ht,
                                          const int fwd,
                                          const int fht,
                                          const int stride,
                                          const dt_image_orientation_t orientation)
{
  const float scale = 1.0f / (white - black);
  if(!orientation)
  {
    DT_OMP_FOR()
    for(int j = 0; j < ht; j++)
      for(int i = 0; i < wd; i++)
        for(int k = 0; k < ch; k++)
          out[4 * ((size_t)j * wd + i) + k] =
            (in[(size_t)j * stride + (size_t)ch * i + k] - black) * scale;
    return;
  }
  int ii = 0;
  int jj = 0;
  int si = 4;
  int sj = wd * 4;
  if(orientation & ORIENTATION_SWAP_XY)
  {
    sj = 4;
    si = ht * 4;
  }
  if(orientation & ORIENTATION_FLIP_Y)
  {
    jj = (int)fht - jj - 1;
    sj = -sj;
  }
  if(orientation & ORIENTATION_FLIP_X)
  {
    ii = (int)fwd - ii - 1;
    si = -si;
  }
  DT_OMP_FOR()
  for(int j = 0; j < ht; j++)
  {
    float *out2 = out + (size_t)labs(sj) * jj + (size_t)labs(si) * ii + sj * j;
    const uint8_t *in2 = in + (size_t)stride * j;
    for(int i = 0; i < wd; i++)
    {
      for(int k = 0; k < ch; k++) out2[k] = (in2[k] - black) * scale;
      in2 += ch;
      out2 += si;
    }
  }
}

size_t dt_imageio_write_pos(const int i,
                            const int j,
                            const int wd,
                            const int ht,
                            const float fwd,
                            const float fht,
                            const dt_image_orientation_t orientation)
{
  int ii = i, jj = j, w = wd, fw = fwd, fh = fht;
  if(orientation & ORIENTATION_SWAP_XY)
  {
    w = ht;
    ii = j;
    jj = i;
    fw = fht;
    fh = fwd;
  }
  if(orientation & ORIENTATION_FLIP_X) ii = (int)fw - ii - 1;
  if(orientation & ORIENTATION_FLIP_Y) jj = (int)fh - jj - 1;
  return (size_t)jj * w + ii;
}

gboolean dt_imageio_is_ldr(const char *filename)
{
  const dt_magic_bytes_t *sig = _find_signature(filename);
  return sig && !sig->hdr;
}

void dt_imageio_to_fractional(const float in,
                              uint32_t *num,
                              uint32_t *den)
{
  if(!(in >= 0))
  {
    *num = *den = 0;
    return;
  }
  *den = 1;
  *num = (int)(in * *den + .5f);

  while(fabsf(*num / (float)*den - in) > 0.001f)
  {
    *den *= 10;
    *num = (int)(in * *den + .5f);
  }
}

gboolean dt_imageio_export(const dt_imgid_t imgid,
                           const char *filename,
                           dt_imageio_module_format_t *format,
                           dt_imageio_module_data_t *format_params,
                           const gboolean high_quality,
                           const gboolean upscale,
                           const gboolean copy_metadata,
                           const gboolean export_masks,
                           const dt_colorspaces_color_profile_type_t icc_type,
                           const gchar *icc_filename,
                           const dt_iop_color_intent_t icc_intent,
                           dt_imageio_module_storage_t *storage,
                           dt_imageio_module_data_t *storage_params,
                           const int num,
                           const int total,
                           dt_export_metadata_t *metadata)
{
  if(strcmp(format->mime(format_params), "x-copy") == 0)
    /* This is a just a copy, skip process and just export */
    return (format->write_image(format_params, filename, NULL, icc_type,
                               icc_filename, NULL, 0, imgid, num, total, NULL,
                               export_masks)) != 0;
  else
  {
    const gboolean is_scaling =
      dt_conf_is_equal("plugins/lighttable/export/resizing", "scaling");

    return dt_imageio_export_with_flags(imgid, filename, format, format_params,
                                        FALSE, FALSE, high_quality, upscale,
                                        is_scaling, FALSE, NULL, copy_metadata,
                                        export_masks, icc_type, icc_filename,
                                        icc_intent, storage, storage_params,
                                        num, total, metadata, -1);
  }
}


static double _get_pipescale(dt_dev_pixelpipe_t *pipe,
                             const int width,
                             const int height,
                             const double max_scale)
{
  const double scalex = width > 0
    ? fmin((double)width / (double)pipe->processed_width, max_scale)
    : max_scale;

  const double scaley = height > 0
    ? fmin((double)height / (double)pipe->processed_height, max_scale)
    : max_scale;

  return fmin(scalex, scaley);
}

// internal function: to avoid exif blob reading + 8-bit byteorder
// flag + high-quality override
gboolean dt_imageio_export_with_flags(const dt_imgid_t imgid,
                                      const char *filename,
                                      dt_imageio_module_format_t *format,
                                      dt_imageio_module_data_t *format_params,
                                      const gboolean ignore_exif,
                                      const gboolean display_byteorder,
                                      const gboolean high_quality,
                                      const gboolean upscale,
                                      const gboolean is_scaling,
                                      const gboolean thumbnail_export,
                                      const char *filter,
                                      const gboolean copy_metadata,
                                      const gboolean export_masks,
                                      const dt_colorspaces_color_profile_type_t icc_type,
                                      const gchar *icc_filename,
                                      const dt_iop_color_intent_t icc_intent,
                                      dt_imageio_module_storage_t *storage,
                                      dt_imageio_module_data_t *storage_params,
                                      int num,
                                      const int total,
                                      dt_export_metadata_t *metadata,
                                      const int history_end)
{
  dt_develop_t dev;
  dt_dev_init(&dev, FALSE);
  dt_dev_load_image(&dev, imgid);
  if(history_end != -1)
    dt_dev_pop_history_items_ext(&dev, history_end);

  if(!thumbnail_export)
    dt_set_backthumb_time(600.0); // make sure we don't interfere

  dt_mipmap_buffer_t buf;
  dt_mipmap_cache_get(darktable.mipmap_cache, &buf, imgid,
                        DT_MIPMAP_FULL, DT_MIPMAP_BLOCKING, 'r');

  const dt_image_t *img = &dev.image_storage;

  if(!buf.buf || !buf.width || !buf.height)
  {
    if(img->load_status == DT_IMAGEIO_FILE_NOT_FOUND)
      dt_control_log(_("image `%s' is not available!"), img->filename);
    else if(img->load_status == DT_IMAGEIO_LOAD_FAILED
            || img->load_status == DT_IMAGEIO_IOERROR
            || img->load_status == DT_IMAGEIO_CACHE_FULL)
    {
      dt_print(DT_DEBUG_ALWAYS,
               "[dt_imageio_export_with_flags] mipmap allocation for `%s' failed (status %d)",
               filename,img->load_status);
      dt_control_log(_("unable to load image `%s'!"), img->filename);
    }
    else
      dt_control_log(_("image '%s' not supported"), img->filename);
    goto error_early;
  }

  const int wd = img->width;
  const int ht = img->height;

  dt_times_t start;
  dt_get_perf_times(&start);
  dt_dev_pixelpipe_t pipe;
  gboolean res = thumbnail_export
    ? dt_dev_pixelpipe_init_thumbnail(&pipe, wd, ht)
    : dt_dev_pixelpipe_init_export(&pipe, wd, ht,
                                   format->levels(format_params), export_masks);
  if(!res)
  {
    dt_control_log(
      _("failed to allocate memory for %s, please lower the threads used"
        " for export or buy more memory."),
      thumbnail_export ? C_("noun", "thumbnail export") : C_("noun", "export"));
    goto error;
  }

  const int final_history_end = history_end == -1 ? dev.history_end : history_end;
  const gboolean use_style = !thumbnail_export && format_params->style[0] != '\0';
  const gboolean appending = format_params->style_append != FALSE;
  //  If a style is to be applied during export, add the iop params into the history
  if(use_style)
  {
    GList *style_items = dt_styles_get_item_list(format_params->style, FALSE, -1, TRUE);
    if(!style_items)
    {
      dt_print(DT_DEBUG_ALWAYS,
               "[imageio] cannot find the style '%s' to apply during export",
               format_params->style);
      if(darktable.gui)
        dt_control_log(_("cannot find the style '%s' to apply during export"),
                       format_params->style);
      else
        dt_print(DT_DEBUG_ALWAYS,
                 "[imageio] please check that you have imported this style into darktable"
                 " and specified it in the command line without the .dtstyle extension\n");
      goto error;
    }

    GList *modules_used = NULL;

    if(!appending) dt_dev_pop_history_items_ext(&dev, 0);

    dt_ioppr_update_for_style_items(&dev, style_items, appending);

    for(GList *st_items = style_items; st_items; st_items = g_list_next(st_items))
    {
      dt_style_item_t *st_item = st_items->data;
      gboolean ok = TRUE;
      gboolean autoinit = FALSE;

      /* check for auto-init module */
      if(st_item->params_size == 0)
      {
        // get iop for this operation as we need the corresponding
        // default parameters
        const dt_iop_module_t *module =
          dt_iop_get_module_from_list(dev.iop, st_item->operation);
        if(module)
        {
          st_item->params_size = module->params_size;
          st_item->params = (dt_iop_params_t *)malloc(st_item->params_size);
          memcpy(st_item->params, module->default_params, module->params_size);
          autoinit = TRUE;
        }
        else
        {
          dt_print(DT_DEBUG_ALWAYS,
                  "[dt_imageio_export_with_flags] cannot find module %s for style",
                  st_item->operation);
          ok = FALSE;
        }
      }

      if(ok)
      {
        dt_styles_apply_style_item(&dev, st_item, &modules_used, !autoinit && appending);
      }
    }

    g_list_free(modules_used);
    g_list_free_full(style_items, dt_style_item_free);
  }
  else if(history_end != -1)
    dt_dev_pop_history_items_ext(&dev, final_history_end);

  dt_ioppr_resync_modules_order(&dev);

  dt_dev_pixelpipe_set_icc(&pipe, icc_type, icc_filename, icc_intent);
  dt_dev_pixelpipe_set_input(&pipe, &dev, (float *)buf.buf,
                             buf.width, buf.height, buf.iscale);
  dt_dev_pixelpipe_create_nodes(&pipe, &dev);
  dt_dev_pixelpipe_synch_all(&pipe, &dev);

  if(darktable.unmuted & DT_DEBUG_IMAGEIO)
  {
    char mbuf[2048] = { 0 };
    for(GList *nodes = pipe.nodes; nodes; nodes = g_list_next(nodes))
    {
      dt_dev_pixelpipe_iop_t *piece = nodes->data;
      if(piece->enabled)
      {
        g_strlcat(mbuf, " ", sizeof(mbuf));
        g_strlcat(mbuf, piece->module->op, sizeof(mbuf));
        g_strlcat(mbuf, dt_iop_get_instance_id(piece->module), sizeof(mbuf));
      }
    }

    dt_print(DT_DEBUG_ALWAYS,
             "[dt_imageio_export_with_flags] %s%s%s%s%s modules:%s",
             use_style && appending      ? "append style history " : "",
             use_style && !appending     ? "replace style history " : "",
             use_style                   ? "`" : "",
             use_style && format_params  ? format_params->style : "",
             use_style                   ? "'." : "",
             mbuf);
  }

  if(filter)
  {
    if(!strncmp(filter, "pre:", 4))
      dt_dev_pixelpipe_disable_after(&pipe, filter + 4);
    if(!strncmp(filter, "post:", 5))
      dt_dev_pixelpipe_disable_before(&pipe, filter + 5);
  }

  dt_dev_pixelpipe_get_dimensions(&pipe, &dev, pipe.iwidth, pipe.iheight,
                                  &pipe.processed_width,
                                  &pipe.processed_height);

  dt_show_times(&start, "[export] creating pixelpipe");

  // find output color profile for this image:
  gboolean sRGB = TRUE;
  if(icc_type == DT_COLORSPACE_SRGB) { }
  else if(icc_type == DT_COLORSPACE_NONE)
  {
    dt_iop_module_t *colorout = NULL;
    for(GList *modules = dev.iop; modules; modules = g_list_next(modules))
    {
      colorout = (dt_iop_module_t *)modules->data;
      if(colorout->get_p && strcmp(colorout->op, "colorout") == 0)
      {
        const dt_colorspaces_color_profile_type_t *type =
          colorout->get_p(colorout->params, "type");
        sRGB = (!type || *type == DT_COLORSPACE_SRGB);
        break; // colorout can't have > 1 instance
      }
    }
  }
  else
    sRGB = FALSE;

  // get only once at the beginning, in case the user changes it on the way:
  const gboolean high_quality_processing = high_quality;

  int width = MAX(format_params->max_width, 0);
  int height = MAX(format_params->max_height, 0);

  if(!thumbnail_export && width == 0 && height == 0)
  {
    width = pipe.processed_width;
    height = pipe.processed_height;
  }

  // note: not perfect but a reasonable good guess looking at overall pixelpipe requirements
  // and specific stuff in finalscale.
  const double max_possible_scale = fmin(100.0, fmax(1.0, // keep maximum allowed scale as we had in 4.6
      (double)dt_get_available_pipe_mem(&pipe) / (double)(1 + 64 * sizeof(float) * pipe.processed_width * pipe.processed_height)));

  const gboolean doscale = upscale && ((width > 0 || height > 0) || is_scaling);
  const double max_scale = doscale ? max_possible_scale : 1.00;

  double scale = _get_pipescale(&pipe, width, height, max_scale);
  float origin[2] = { 0.0f, 0.0f };

  if(dt_dev_distort_backtransform_plus(&dev, &pipe, 0.0,
                                       DT_DEV_TRANSFORM_DIR_ALL, origin, 1))
  {
    if(width == 0) width = pipe.processed_width;
    if(height == 0) height = pipe.processed_height;
    scale = _get_pipescale(&pipe, width, height, max_scale);

    if(is_scaling)
    {
      // scaling
      double _num, _denum;
      dt_imageio_resizing_factor_get_and_parsing(&_num, &_denum);
      const double scale_factor = _num / _denum;
      if(!thumbnail_export)
      {
        scale = fmin(scale_factor, max_scale);
      }
    }
  }

  const int processed_width = floor(scale * pipe.processed_width);
  const int processed_height = floor(scale * pipe.processed_height);
  const gboolean size_warning = processed_width < 1 || processed_height < 1;
  dt_print(DT_DEBUG_IMAGEIO,
           "[dt_imageio_export] %s%s imgid %d, %ix%i --> %ix%i (scale=%.4f, maxscale=%.4f)."
           " upscale=%s, hq=%s",
           size_warning ? "**missing size** " : "",
           thumbnail_export ? "thumbnail" : "export", imgid,
           pipe.processed_width, pipe.processed_height,
           processed_width, processed_height, scale, max_scale,
           upscale ? "yes" : "no",
           high_quality_processing || scale > 1.0f ? "yes" : "no");

  const int bpp = format->bpp(format_params);

  dt_get_perf_times(&start);
  const gboolean hq_process = high_quality_processing || scale > 1.0f;
  if(hq_process)
  {
    /*
     * if high quality processing was requested, downsampling will be done
     * at the very end of the pipe (just before border and watermark)
     */
    dt_dev_pixelpipe_process_no_gamma(&pipe, &dev, 0, 0,
                                      processed_width, processed_height, scale);
  }
  else
  {
    // else, downsampling will be right after demosaic

    // so we need to turn temporarily disable in-pipe late downsampling iop.

    // find the finalscale module
    dt_dev_pixelpipe_iop_t *finalscale = NULL;
    {
      for(const GList *nodes = g_list_last(pipe.nodes);
          nodes;
          nodes = g_list_previous(nodes))
      {
        dt_dev_pixelpipe_iop_t *node = nodes->data;
        if(dt_iop_module_is(node->module->so, "finalscale"))
        {
          finalscale = node;
          break;
        }
      }
    }

    if(finalscale) finalscale->enabled = FALSE;

    // do the processing (8-bit with special treatment, to make sure
    // we can use openmp further down):
    if(bpp == 8)
      dt_dev_pixelpipe_process(&pipe, &dev, 0, 0,
                               processed_width, processed_height, scale, DT_DEVICE_NONE);
    else
      dt_dev_pixelpipe_process_no_gamma(&pipe, &dev, 0, 0,
                                        processed_width, processed_height, scale);

    if(finalscale) finalscale->enabled = TRUE;
  }
  dt_show_times(&start,
                thumbnail_export
                  ? "[dev_process_thumbnail] pixel pipeline processing"
                  : "[dev_process_export] pixel pipeline processing");

  uint8_t *outbuf = pipe.backbuf;
  if(outbuf == NULL)
  {
    dt_print(DT_DEBUG_IMAGEIO,
             "[dt_imageio_export_with_flags] no valid output buffer");
    goto error;
  }

  // downconversion to low-precision formats:
  if(bpp == 8)
  {
    if(display_byteorder)
    {
      if(hq_process)
      {
        const float *const inbuf = (float *)outbuf;
        for(size_t k = 0; k < (size_t)processed_width * processed_height; k++)
        {
          // convert in place, this is unfortunately very serial..
          const uint8_t r = roundf(CLAMP(inbuf[4 * k + 2] * 0xff, 0, 0xff));
          const uint8_t g = roundf(CLAMP(inbuf[4 * k + 1] * 0xff, 0, 0xff));
          const uint8_t b = roundf(CLAMP(inbuf[4 * k + 0] * 0xff, 0, 0xff));
          outbuf[4 * k + 0] = r;
          outbuf[4 * k + 1] = g;
          outbuf[4 * k + 2] = b;
        }
      }
      // else processing output was 8-bit already, and no need to swap order
    }
    else // need to flip
    {
      // ldr output: char
      if(hq_process)
      {
        const float *const inbuf = (float *)outbuf;
        for(size_t k = 0; k < (size_t)processed_width * processed_height; k++)
        {
          // convert in place, this is unfortunately very serial..
          const uint8_t r = roundf(CLAMP(inbuf[4 * k + 0] * 0xff, 0, 0xff));
          const uint8_t g = roundf(CLAMP(inbuf[4 * k + 1] * 0xff, 0, 0xff));
          const uint8_t b = roundf(CLAMP(inbuf[4 * k + 2] * 0xff, 0, 0xff));
          outbuf[4 * k + 0] = r;
          outbuf[4 * k + 1] = g;
          outbuf[4 * k + 2] = b;
        }
      }
      else
      { // !display_byteorder, need to swap:
        uint8_t *const buf8 = pipe.backbuf;
        DT_OMP_FOR()
        // just flip byte order
        for(size_t k = 0; k < (size_t)processed_width * processed_height; k++)
        {
          uint8_t tmp = buf8[4 * k + 0];
          buf8[4 * k + 0] = buf8[4 * k + 2];
          buf8[4 * k + 2] = tmp;
        }
      }
    }
  }
  else if(bpp == 16)
  {
    // uint16_t per color channel
    float *buff = (float *)outbuf;
    uint16_t *buf16 = (uint16_t *)outbuf;
    for(int y = 0; y < processed_height; y++)
      for(int x = 0; x < processed_width; x++)
      {
        // convert in place
        const size_t k = (size_t)processed_width * y + x;
        for(int i = 0; i < 3; i++)
          buf16[4 * k + i] = roundf(CLAMP(buff[4 * k + i] * 0xffff, 0, 0xffff));
      }
  }
  // else output float, no further harm done to the pixels :)

  format_params->width = processed_width;
  format_params->height = processed_height;

  // Check if all the metadata export flags are set for AVIF/EXR/JPEG XL/XCF (opt-in)
  //
  // TODO: this is a workaround as these formats do not support fine
  // grained metadata control through dt_exif_xmp_attach_export()
  // below due to lack of exiv2 write support
  //
  // Note: that this is done only when we do not ignore_exif, so we have a proper filename
  //       otherwise the export is done in a memory buffer.
  gboolean md_flags_set = TRUE;
  if(!ignore_exif
     && (!strcmp(format->mime(NULL), "image/avif")
         || !strcmp(format->mime(NULL), "image/x-exr")
         || !strcmp(format->mime(NULL), "image/jxl")
         || !strcmp(format->mime(NULL), "image/x-xcf")))
  {
    const int32_t meta_all =
      DT_META_EXIF | DT_META_METADATA | DT_META_GEOTAG | DT_META_TAG
      | DT_META_HIERARCHICAL_TAG | DT_META_DT_HISTORY | DT_META_PRIVATE_TAG
      | DT_META_SYNONYMS_TAG | DT_META_OMIT_HIERARCHY;
    md_flags_set = metadata ? (metadata->flags & meta_all) == meta_all : FALSE;
  }

  if(!ignore_exif && md_flags_set)
  {
    uint8_t *exif_profile = NULL; // Exif data should be 65536 bytes
                                  // max, but if original size is
                                  // close to that, adding new tags
                                  // could make it go over that... so
                                  // let it be and see what happens
                                  // when we write the image
    char pathname[PATH_MAX] = { 0 };
    gboolean from_cache = TRUE;
    dt_image_full_path(imgid, pathname, sizeof(pathname), &from_cache);

    // last param is dng mode, it's false here
    const int length = dt_exif_read_blob(&exif_profile, pathname, imgid, sRGB,
                                         processed_width, processed_height, FALSE);

    res = (format->write_image(format_params, filename, outbuf, icc_type,
                              icc_filename, exif_profile, length, imgid,
                              num, total, &pipe, export_masks)) != 0;

    free(exif_profile);
  }
  else
  {
    res = (format->write_image(format_params, filename, outbuf, icc_type,
                              icc_filename, NULL, 0, imgid, num, total,
                              &pipe, export_masks)) != 0;
  }

  if(res)
    goto error;

  /* now write xmp into that container, if possible */
  if(copy_metadata
     && (format->flags(format_params) & FORMAT_FLAGS_SUPPORT_XMP))
  {
    dt_exif_xmp_attach_export(imgid, filename, metadata, &dev, &pipe);
    // no need to cancel the export if this fail
  }

  dt_dev_pixelpipe_cleanup(&pipe);
  dt_dev_cleanup(&dev);
  dt_mipmap_cache_release(darktable.mipmap_cache, &buf);

  if(!thumbnail_export && strcmp(format->mime(format_params), "memory")
    && !(format->flags(format_params) & FORMAT_FLAGS_NO_TMPFILE))
  {
#ifdef USE_LUA
    //Synchronous calling of lua intermediate-export-image events
    dt_lua_lock();

    lua_State *L = darktable.lua_state.state;

    luaA_push(L, dt_lua_image_t, &imgid);

    lua_pushstring(L, filename);

    luaA_push_type(L, format->parameter_lua_type, format_params);

    if(storage)
      luaA_push_type(L, storage->parameter_lua_type, storage_params);
    else
      lua_pushnil(L);

    dt_lua_event_trigger(L, "intermediate-export-image", 4);

    dt_lua_unlock();
#endif

    DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_IMAGE_EXPORT_TMPFILE, imgid, filename, format,
                            format_params, storage, storage_params);
  }

  if(!thumbnail_export)
    dt_set_backthumb_time(5.0);
  return FALSE; // success

error:
  dt_dev_pixelpipe_cleanup(&pipe);
error_early:
  dt_dev_cleanup(&dev);
  dt_mipmap_cache_release(darktable.mipmap_cache, &buf);

  if(!thumbnail_export)
    dt_set_backthumb_time(5.0);
  return TRUE;
}


// fallback read method in case file could not be opened yet.
// use GraphicsMagick (if supported) to read exotic LDRs
dt_imageio_retval_t dt_imageio_open_exotic(dt_image_t *img,
                                           const char *filename,
                                           dt_mipmap_buffer_t *buf)
{
  // if buf is NULL, don't proceed
  if(!buf) return DT_IMAGEIO_OK;
  dt_imageio_retval_t ret = dt_imageio_open_gm(img, filename, buf);
  if(_image_handled(ret)) return ret;
  ret = dt_imageio_open_im(img, filename, buf);
  if(_image_handled(ret)) return ret;

  return DT_IMAGEIO_LOAD_FAILED;
}

void dt_imageio_update_monochrome_workflow_tag(const int32_t id, const int mask)
{
  if(mask & (DT_IMAGE_MONOCHROME | DT_IMAGE_MONOCHROME_PREVIEW | DT_IMAGE_MONOCHROME_BAYER))
  {
    guint tagid = 0;
    char tagname[64];
    snprintf(tagname, sizeof(tagname), "darktable|mode|monochrome");
    dt_tag_new(tagname, &tagid);
    dt_tag_attach(tagid, id, FALSE, FALSE);
  }
  else
    dt_tag_detach_by_string("darktable|mode|monochrome", id, FALSE, FALSE);

  DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_TAG_CHANGED);
}

void dt_imageio_set_hdr_tag(dt_image_t *img)
{
  guint tagid = 0;
  char tagname[64];
  snprintf(tagname, sizeof(tagname), "darktable|mode|hdr");
  dt_tag_new(tagname, &tagid);
  dt_tag_attach(tagid, img->id, FALSE, FALSE);
  img->flags |= DT_IMAGE_HDR;
  img->flags &= ~DT_IMAGE_LDR;
}

// =================================================
//   combined reading
// =================================================

dt_imageio_retval_t dt_imageio_open(dt_image_t *img,
                                    const char *filename,
                                    dt_mipmap_buffer_t *buf)
{
  /* first of all, check if file exists, don't bother to test loading
   * if not exists */
  if(!g_file_test(filename, G_FILE_TEST_IS_REGULAR))
    return DT_IMAGEIO_FILE_NOT_FOUND;

  const int32_t was_hdr = (img->flags & DT_IMAGE_HDR);
  const int32_t was_bw = dt_image_monochrome_flags(img);

  dt_imageio_retval_t ret = DT_IMAGEIO_LOAD_FAILED;
  img->loader = LOADER_UNKNOWN;

  // check for known magic numbers and call the appropriate loader if we recognize a magic number
  ret = _open_by_magic_number(img, filename, buf);

  // Go to fallback path if we didn't recognize the magic bytes (UNRECOGNIZED)
  // or the main loader has rejected the file (UNSUPPORTED_FORMAT)
  if((ret == DT_IMAGEIO_UNRECOGNIZED) || (ret == DT_IMAGEIO_UNSUPPORTED_FORMAT))
  {
    // special case - most camera RAW files are TIFF containers, so if we have an LDR file extension,
    // try loading the file as TIFF
    if(dt_imageio_is_ldr(filename))
      ret = dt_imageio_open_tiff(img, filename, buf);

    // try using rawspeed to load a raw
    if(!_image_handled(ret))
      ret = dt_imageio_open_rawspeed(img, filename, buf);

    // fallback that tries to open file via LibRaw to support Canon CR3
    if(!_image_handled(ret))
      ret = dt_imageio_open_libraw(img, filename, buf);

    // there are reports that AVIF and HEIF files with alternate magic bytes exist, so try loading
    // as such if we haven't yet succeeded
    if(!_image_handled(ret))
      ret = dt_imageio_open_avif(img, filename, buf);

    if(!_image_handled(ret))
      ret = dt_imageio_open_heif(img, filename, buf);

    if(!_image_handled(ret))
      ret = dt_imageio_open_exr(img, filename, buf);

    if(!_image_handled(ret))
      ret = dt_imageio_open_rgbe(img, filename, buf);

    if(!_image_handled(ret))
      ret = dt_imageio_open_j2k(img, filename, buf);

    if(!_image_handled(ret))
      ret = dt_imageio_open_jpeg(img, filename, buf);

    if(!_image_handled(ret))
      ret = dt_imageio_open_pnm(img, filename, buf);

    // final fallback that tries to open file via GraphicsMagick or ImageMagick
    if(!_image_handled(ret))
      ret = dt_imageio_open_exotic(img, filename, buf);

    //  if nothing succeeded, declare the image format unsupported
    if(!_image_handled(ret))
      ret = DT_IMAGEIO_UNSUPPORTED_FORMAT;
  }

  if((ret == DT_IMAGEIO_OK) && !was_hdr && (img->flags & DT_IMAGE_HDR))
    dt_imageio_set_hdr_tag(img);

  if((ret == DT_IMAGEIO_OK) && (was_bw != dt_image_monochrome_flags(img)))
    dt_imageio_update_monochrome_workflow_tag(img->id, dt_image_monochrome_flags(img));

  img->p_width = img->width - img->crop_x - img->crop_right;
  img->p_height = img->height - img->crop_y - img->crop_bottom;

  return ret;
}

gboolean dt_imageio_lookup_makermodel(const char *maker,
                                      const char *model,
                                      char *mk,
                                      const int mk_len,
                                      char *md,
                                      const int md_len,
                                      char *al,
                                      const int al_len)
{
  // At this stage, we can't tell which loader is used to open the image.
  gboolean found = dt_rawspeed_lookup_makermodel(maker, model,
                                                 mk, mk_len,
                                                 md, md_len,
                                                 al, al_len);
  if(found == FALSE)
  {
    // Special handling for CR3 raw files via libraw
    found = dt_libraw_lookup_makermodel(maker, model,
                                        mk, mk_len,
                                        md, md_len,
                                        al, al_len);
  }
  return found;
}

typedef struct _imageio_preview_t
{
  dt_imageio_module_data_t head;
  int bpp;
  uint8_t *buf;
  uint32_t width, height;
} _imageio_preview_t;

static int _preview_write_image(dt_imageio_module_data_t *data,
                                const char *filename,
                                const void *in,
                                const dt_colorspaces_color_profile_type_t over_type,
                                const char *over_filename,
                                void *exif,
                                const int exif_len,
                                const dt_imgid_t imgid,
                                const int num,
                                const int total,
                                dt_dev_pixelpipe_t*pipe,
                                const gboolean export_masks)
{
  _imageio_preview_t *d = (_imageio_preview_t *)data;

  memcpy(d->buf, in, sizeof(uint32_t) * data->width * data->height);
  d->width = data->width;
  d->height = data->height;

  return 0;
}

static int _preview_bpp(dt_imageio_module_data_t *data)
{
  return 8;
}

static int _preview_levels(dt_imageio_module_data_t *data)
{
  return IMAGEIO_RGB | IMAGEIO_INT8;
}

static const char *_preview_mime(dt_imageio_module_data_t *data)
{
  return "memory";
}

cairo_surface_t *dt_imageio_preview(const dt_imgid_t imgid,
                                    const size_t width,
                                    const size_t height,
                                    const int history_end,
                                    const char *style_name)
{
  dt_imageio_module_format_t buf;
  buf.mime = _preview_mime;
  buf.levels = _preview_levels;
  buf.bpp = _preview_bpp;
  buf.write_image = _preview_write_image;

  _imageio_preview_t dat;
  dat.head.max_width = width;
  dat.head.max_height = height;
  dat.head.width = width;
  dat.head.height = height;
  dat.head.style_append = TRUE;
  dat.bpp = 8;
  dat.buf = (uint8_t *)dt_alloc_aligned(sizeof(uint32_t) * width * height);

  g_strlcpy(dat.head.style, style_name, sizeof(dat.head.style));

  const gboolean high_quality = FALSE;
  const gboolean upscale = TRUE;
  const gboolean export_masks = FALSE;
  const gboolean is_scaling = FALSE;

  dt_imageio_export_with_flags
    (imgid, "preview", &buf, (dt_imageio_module_data_t *)&dat, TRUE, TRUE,
     high_quality, upscale, is_scaling, FALSE, NULL, FALSE, export_masks,
     DT_COLORSPACE_DISPLAY, NULL, DT_INTENT_LAST, NULL, NULL, 1, 1, NULL,
     history_end);

  const int32_t stride =
    cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, dat.head.width);

  cairo_surface_t *surface = dt_cairo_image_surface_create_for_data
    (dat.buf, CAIRO_FORMAT_RGB24, dat.head.width, dat.head.height, stride);

  return surface;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
