/*
    This file is part of darktable,
    Copyright (C) 2009-2020 darktable developers.

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

#pragma once

#include "common/colorspaces.h"
#include "common/image.h"
#include "develop/pixelpipe.h"

/** wrapper around exiv2, C++ */
#ifdef __cplusplus
extern "C" {
#endif

typedef enum dt_dng_illuminant_t // from adobes dng_sdk
{
	DT_LS_Unknown              =  0,
	DT_LS_Daylight             =  1,
	DT_LS_Fluorescent          =  2,
	DT_LS_Tungsten             =  3,
	DT_LS_Flash                =  4,
	DT_LS_FineWeather          =  9,
	DT_LS_CloudyWeather        = 10,
	DT_LS_Shade                = 11,
	DT_LS_DaylightFluorescent  = 12, // D  5700 - 7100K
	DT_LS_DayWhiteFluorescent  = 13, // N  4600 - 5500K
	DT_LS_CoolWhiteFluorescent = 14, // W  3800 - 4500K
	DT_LS_WhiteFluorescent     = 15, // WW 3250 - 3800K
	DT_LS_WarmWhiteFluorescent = 16, // L  2600 - 3250K
	DT_LS_StandardLightA       = 17,
	DT_LS_StandardLightB       = 18,
	DT_LS_StandardLightC       = 19,
	DT_LS_D55                  = 20,
	DT_LS_D65                  = 21,
	DT_LS_D75                  = 22,
	DT_LS_D50                  = 23,
	DT_LS_ISOStudioTungsten    = 24,
	DT_LS_Other                = 255
} dt_dng_illuminant_t;


/** set the list of available tags from Exvi2 */
void dt_exif_set_exiv2_taglist();

/** get the list of available tags from Exvi2 */
/** must not be freed */
const GList* dt_exif_get_exiv2_taglist();

/** read metadata from file with full path name, XMP data trumps IPTC data trumps EXIF data, store to image
 * struct. returns 0 on success. */
int dt_exif_read(dt_image_t *img, const char *path);

/** read exif data to image struct from given data blob, wherever you got it from. */
int dt_exif_read_from_blob(dt_image_t *img, uint8_t *blob, const int size);

/** write exif to blob, return length in bytes. blob will be allocated by the function. sRGB should be true
 * if sRGB colorspace is used as output. */
int dt_exif_read_blob(uint8_t **blob, const char *path, const int imgid, const int sRGB, const int out_width,
                      const int out_height, const int dng_mode);

/** Reads exif tags that are not cached in the database */
void dt_exif_img_check_additional_tags(dt_image_t *img, const char *filename);

/** write blob to file exif. merges with existing exif information.*/
int dt_exif_write_blob(uint8_t *blob, uint32_t size, const char *path, const int compressed);

/** write xmp sidecar file. */
int dt_exif_xmp_write(const int imgid, const char *filename);

/** write xmp packet inside an image. */
int dt_exif_xmp_attach_export(const int imgid, const char *filename, void *metadata,
    dt_develop_t *dev, dt_dev_pixelpipe_t *pipe);

/** get the xmp blob for imgid. */
char *dt_exif_xmp_read_string(const int imgid);

/** read xmp sidecar file. */
int dt_exif_xmp_read(dt_image_t *img, const char *filename, const int history_only);

/** apply default import metadata */
void dt_exif_apply_default_metadata(dt_image_t *img);

/** fetch largest exif thumbnail jpg bytestream into buffer*/
int dt_exif_get_thumbnail(const char *path, uint8_t **buffer, size_t *size, char **mime_type);

/** thread safe init and cleanup. */
void dt_exif_init();
void dt_exif_cleanup();

/** encode / decode op params */
char *dt_exif_xmp_encode(const unsigned char *input, const int len, int *output_len);
char *dt_exif_xmp_encode_internal(const unsigned char *input, const int len, int *output_len, gboolean do_compress);
unsigned char *dt_exif_xmp_decode(const char *input, const int len, int *output_len);

/** look for color space hints in data and tell the caller if it's sRGB, AdobeRGB or something else. used for mipmaps */
dt_colorspaces_color_profile_type_t dt_exif_get_color_space(const uint8_t *data, size_t size);

/** look for basic info in data. used import jobs */
void dt_exif_get_basic_data(const uint8_t *data, size_t size, dt_image_basic_exif_t *basic_exif);

#ifdef __cplusplus
}
#endif

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
