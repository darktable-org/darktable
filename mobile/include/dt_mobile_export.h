/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DT_MOBILE_EXPORT_H
#define DT_MOBILE_EXPORT_H
#include <stddef.h>
#include <stdint.h>

typedef enum dt_mobile_export_pixel_format {
  DT_MOBILE_EXPORT_PIXEL_DEFAULT = 0,
  DT_MOBILE_EXPORT_UINT8 = 1,
  DT_MOBILE_EXPORT_UINT16 = 2,
  DT_MOBILE_EXPORT_FLOAT32 = 3
} dt_mobile_export_pixel_format;
typedef enum dt_mobile_export_profile {
  DT_MOBILE_EXPORT_PROFILE_IMAGE = 0,
  DT_MOBILE_EXPORT_PROFILE_SRGB = 1,
  DT_MOBILE_EXPORT_PROFILE_ADOBE_RGB = 2,
  DT_MOBILE_EXPORT_PROFILE_LINEAR_REC709 = 3,
  DT_MOBILE_EXPORT_PROFILE_LINEAR_REC2020 = 4,
  DT_MOBILE_EXPORT_PROFILE_REC709 = 5,
  DT_MOBILE_EXPORT_PROFILE_PROPHOTO_RGB = 6,
  DT_MOBILE_EXPORT_PROFILE_DISPLAY_P3 = 7
} dt_mobile_export_profile;
typedef enum dt_mobile_export_intent {
  DT_MOBILE_EXPORT_INTENT_IMAGE = 0,
  DT_MOBILE_EXPORT_INTENT_PERCEPTUAL = 1,
  DT_MOBILE_EXPORT_INTENT_RELATIVE = 2,
  DT_MOBILE_EXPORT_INTENT_SATURATION = 3,
  DT_MOBILE_EXPORT_INTENT_ABSOLUTE = 4
} dt_mobile_export_intent;
typedef enum dt_mobile_export_metadata {
  DT_MOBILE_EXPORT_METADATA_DEFAULT = 0,
  /* Do not copy source EXIF/XMP. ICC and encoder-generated fields can remain. */
  DT_MOBILE_EXPORT_METADATA_SOURCE_NONE = 1
} dt_mobile_export_metadata;

typedef struct dt_mobile_export_settings {
  size_t struct_size;
  uint32_t max_width, max_height; /* 0 = unrestricted; aspect preserved; no upscale */
  int quality;                  /* JPEG 5..100 */
  int pixel_format;             /* dt_mobile_export_pixel_format */
  int png_compression;          /* -1 = format setting, otherwise 0..9 */
  int tiff_compression;         /* -1 = format setting, 0 none, 1 deflate, 2 predictor */
  int tiff_compression_level;   /* -1 = format setting, otherwise 1..9 */
  int jpeg_subsample;           /* -1 = format setting; 0 auto, 1 444, 2 440, 3 422, 4 420 */
  int dpi;                      /* 0 = format setting; JPEG/TIFF 72..9600 */
  int output_profile;           /* dt_mobile_export_profile */
  int rendering_intent;         /* dt_mobile_export_intent */
  int metadata_policy;          /* dt_mobile_export_metadata */
} dt_mobile_export_settings;

#define DT_MOBILE_EXPORT_SETTINGS_INIT \
  { sizeof(dt_mobile_export_settings), 0, 0, 95, DT_MOBILE_EXPORT_PIXEL_DEFAULT, \
    -1, -1, -1, -1, 0, DT_MOBILE_EXPORT_PROFILE_IMAGE, \
    DT_MOBILE_EXPORT_INTENT_IMAGE, DT_MOBILE_EXPORT_METADATA_DEFAULT }
#endif
