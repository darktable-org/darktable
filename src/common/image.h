/*
    This file is part of darktable,
    copyright (c) 2009--2011 johannes hanika.

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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "common/darktable.h"
#include "common/dtpthread.h"
#include "develop/format.h"
#include <glib.h>
#include <inttypes.h>

/** return value of image io functions. */
typedef enum dt_imageio_retval_t
{
  DT_IMAGEIO_OK = 0,         // all good :)
  DT_IMAGEIO_FILE_NOT_FOUND, // file has been lost
  DT_IMAGEIO_FILE_CORRUPTED, // file contains garbage
  DT_IMAGEIO_CACHE_FULL      // dt's caches are full :(
} dt_imageio_retval_t;

typedef enum
{
  // the first 0x7 in flags are reserved for star ratings.
  DT_IMAGE_DELETE = 1,
  DT_IMAGE_OKAY = 2,
  DT_IMAGE_NICE = 3,
  DT_IMAGE_EXCELLENT = 4,
  // next field unused, but it used to be.
  // old DB entries might have it set.
  // To reuse : force to 0 in DB loading and force to 0 in DB saving
  // Use it to store a state that doesn't need to go in DB
  DT_IMAGE_THUMBNAIL_DEPRECATED = 16,
  // set during import if the image is low-dynamic range, i.e. doesn't need demosaic, wb, highlight clipping
  // etc.
  DT_IMAGE_LDR = 32,
  // set during import if the image is raw data, i.e. it needs demosaicing.
  DT_IMAGE_RAW = 64,
  // set during import if images is a high-dynamic range image..
  DT_IMAGE_HDR = 128,
  // set when marked for deletion
  DT_IMAGE_REMOVE = 256,
  // set when auto-applying presets have been applied to this image.
  DT_IMAGE_AUTO_PRESETS_APPLIED = 512,
  // legacy flag. is set for all new images. i hate to waste a bit on this :(
  DT_IMAGE_NO_LEGACY_PRESETS = 1024,
  // local copy status
  DT_IMAGE_LOCAL_COPY = 2048,
  // image has an associated .txt file for overlay
  DT_IMAGE_HAS_TXT = 4096,
  // image has an associated wav file
  DT_IMAGE_HAS_WAV = 8192,
  // image is a bayer pattern with 4 colors (e.g., CYGM or RGBE)
  DT_IMAGE_4BAYER = 16384,
} dt_image_flags_t;

typedef enum dt_image_colorspace_t
{
  DT_IMAGE_COLORSPACE_NONE,
  DT_IMAGE_COLORSPACE_SRGB,
  DT_IMAGE_COLORSPACE_ADOBE_RGB
} dt_image_colorspace_t;

typedef struct dt_image_raw_parameters_t
{
  unsigned legacy : 24;
  unsigned user_flip : 8; // +8 = 32 bits.
} dt_image_raw_parameters_t;

typedef enum dt_image_orientation_t
{
  ORIENTATION_NULL = -1,        //-1, or autodetect
  ORIENTATION_NONE = 0,         // 0
  ORIENTATION_FLIP_Y = 1 << 0,  // 1
  ORIENTATION_FLIP_X = 1 << 1,  // 2
  ORIENTATION_SWAP_XY = 1 << 2, // 4

  /* ClockWise rotation == "-"; CounterClockWise rotation == "+" */
  ORIENTATION_FLIP_HORIZONTALLY = ORIENTATION_FLIP_Y, // 1
  ORIENTATION_FLIP_VERTICALLY = ORIENTATION_FLIP_X, // 2
  ORIENTATION_ROTATE_180_DEG = ORIENTATION_FLIP_Y | ORIENTATION_FLIP_X, // 3
  ORIENTATION_400 /* ??? */ = ORIENTATION_SWAP_XY, // 4
  ORIENTATION_ROTATE_CCW_90_DEG = ORIENTATION_FLIP_Y | ORIENTATION_SWAP_XY, // 5
  ORIENTATION_ROTATE_CW_90_DEG = ORIENTATION_FLIP_X | ORIENTATION_SWAP_XY, // 6
  ORIENTATION_421 /* ??? */ = ORIENTATION_FLIP_Y | ORIENTATION_FLIP_X | ORIENTATION_SWAP_XY // 7
} dt_image_orientation_t;

typedef enum dt_image_loader_t
{
  LOADER_UNKNOWN = 0,
  LOADER_TIFF = 1,
  LOADER_PNG = 2,
  LOADER_J2K = 3,
  LOADER_JPEG = 4,
  LOADER_EXR = 5,
  LOADER_RGBE = 6,
  LOADER_PFM = 7,
  LOADER_GM = 8,
  LOADER_RAWSPEED = 9,
  LOADER_PNM = 10,
} dt_image_loader_t;

struct dt_cache_entry_t;
// TODO: add color labels and such as cachable
// __attribute__ ((aligned (128)))
typedef struct dt_image_t
{
  // minimal exif data here (all in multiples of 4-byte to interface nicely with c++):
  int32_t exif_inited;
  dt_image_orientation_t orientation;
  float exif_exposure;
  float exif_aperture;
  float exif_iso;
  float exif_focal_length;
  float exif_focus_distance;
  float exif_crop;
  char exif_maker[64];
  char exif_model[64];
  char exif_lens[128];
  char exif_datetime_taken[20];

  char camera_maker[64];
  char camera_model[64];
  char camera_alias[64];
  char camera_makermodel[128];
  char camera_legacy_makermodel[128];

  char filename[DT_MAX_FILENAME_LEN];

  // common stuff

  // to understand this, look at comment for dt_histogram_roi_t
  int32_t width, height;
  int32_t crop_x, crop_y, crop_width, crop_height;

  // used by library
  int32_t num, flags, film_id, id, group_id, version;
  dt_image_loader_t loader;

  dt_iop_buffer_dsc_t buf_dsc;

  float d65_color_matrix[9]; // the 3x3 matrix embedded in some DNGs
  uint8_t *profile;          // embedded profile, for example from JPEGs
  uint32_t profile_size;
  dt_image_colorspace_t colorspace; // the colorspace that is specified in exif. mostly used for jpeg files

  dt_image_raw_parameters_t legacy_flip; // unfortunately needed to convert old bits to new flip module.

  /* gps coords */
  double longitude;
  double latitude;
  double elevation;

  /* needed in exposure iop for Deflicker */
  uint16_t raw_black_level;
  uint16_t raw_black_level_separate[4];
  uint32_t raw_white_point;

  /* needed to fix some manufacturers madness */
  uint32_t fuji_rotation_pos;
  float pixel_aspect_ratio;

  /* White balance coeffs from the raw */
  float wb_coeffs[4];
  /* convenience pointer back into the image cache, so we can return dt_image_t* there directly. */
  struct dt_cache_entry_t *cache_entry;
} dt_image_t;

// image buffer operations:
/** inits basic values to sensible defaults. */
void dt_image_init(dt_image_t *img);
/** Refresh makermodel from the raw and exif values **/
void dt_image_refresh_makermodel(dt_image_t *img);
/** returns non-zero if the image contains low-dynamic range data. */
int dt_image_is_ldr(const dt_image_t *img);
/** returns non-zero if the image contains mosaic data. */
int dt_image_is_raw(const dt_image_t *img);
/** returns non-zero if the image contains float data. */
int dt_image_is_hdr(const dt_image_t *img);
/** returns non-zero if this image was taken using a monochrome camera */
int dt_image_is_monochrome(const dt_image_t *img);
/** returns the full path name where the image was imported from. from_cache=TRUE check and return local
 * cached filename if any. */
void dt_image_full_path(const int imgid, char *pathname, size_t pathname_len, gboolean *from_cache);
/** returns the full directory of the associated film roll. */
void dt_image_film_roll_directory(const dt_image_t *img, char *pathname, size_t pathname_len);
/** returns the portion of the path used for the film roll name. */
const char *dt_image_film_roll_name(const char *path);
/** returns the film roll name, i.e. without the path. */
void dt_image_film_roll(const dt_image_t *img, char *pathname, size_t pathname_len);
/** appends version numbering for duplicated images without querying the db. */
void dt_image_path_append_version_no_db(int version, char *pathname, size_t pathname_len);
/** appends version numbering for duplicated images. */
void dt_image_path_append_version(int imgid, char *pathname, size_t pathname_len);
/** prints a one-line exif information string. */
void dt_image_print_exif(const dt_image_t *img, char *line, size_t line_len);
/** look for duplicate's xmp files and read them. */
void dt_image_read_duplicates(uint32_t id, const char *filename);
/** imports a new image from raw/etc file and adds it to the data base and image cache. Use from threads other than lua.*/
uint32_t dt_image_import(int32_t film_id, const char *filename, gboolean override_ignore_jpegs);
/** imports a new image from raw/etc file and adds it to the data base and image cache. Use from lua thread.*/
uint32_t dt_image_import_lua(int32_t film_id, const char *filename, gboolean override_ignore_jpegs);
/** removes the given image from the database. */
void dt_image_remove(const int32_t imgid);
/** duplicates the given image in the database with the duplicate getting the supplied version number. if that
   version
    already exists just return the imgid without producing new duplicate. called with newversion -1 a new
   duplicate
    is produced with the next free version number. */
int32_t dt_image_duplicate_with_version(const int32_t imgid, const int32_t newversion);
/** duplicates the given image in the database. */
int32_t dt_image_duplicate(const int32_t imgid);
/** flips the image, clock wise, if given flag. */
void dt_image_flip(const int32_t imgid, const int32_t cw);
void dt_image_set_flip(const int32_t imgid, const dt_image_orientation_t user_flip);
dt_image_orientation_t dt_image_get_orientation(const int imgid);
/** get max width and height of the final processed image with its current hisotry stack */
gboolean dt_image_get_final_size(const int32_t imgid, int *width, int *height);
/** set image location lon/lat */
void dt_image_set_location(const int32_t imgid, double lon, double lat);
/** set image location lon/lat/ele */
void dt_image_set_location_and_elevation(const int32_t imgid, double lon, double lat, double ele);
/** returns 1 if there is history data found for this image, 0 else. */
int dt_image_altered(const uint32_t imgid);
/** set the image final/cropped aspect ratio */
void dt_image_set_aspect_ratio(const int32_t imgid);
/** set the image final/cropped aspect ratio */
void dt_image_set_aspect_ratio_to(const int32_t imgid, double aspect_ratio);
/** returns the orientation bits of the image from exif. */
static inline dt_image_orientation_t dt_image_orientation(const dt_image_t *img)
{
  return img->orientation != ORIENTATION_NULL ? img->orientation : ORIENTATION_NONE;
}

/** return the raw orientation, from jpg orientation. */
static inline dt_image_orientation_t dt_image_orientation_to_flip_bits(const int orient)
{
  switch(orient)
  {
    case 1:
      return ORIENTATION_NONE;
    case 2:
      return ORIENTATION_FLIP_HORIZONTALLY;
    case 3:
      return ORIENTATION_ROTATE_180_DEG;
    case 4:
      return ORIENTATION_FLIP_VERTICALLY;
    case 5:
      return ORIENTATION_400; // ???
    case 6:
      return ORIENTATION_ROTATE_CW_90_DEG;
    case 7:
      return ORIENTATION_421; // ???
    case 8:
      return ORIENTATION_ROTATE_CCW_90_DEG;
    default:
      return ORIENTATION_NONE;
  }
}

/** physically move image with imgid and its duplicates to the film roll
 *  given by filmid. returns -1 on error, 0 on success. */
int32_t dt_image_move(const int32_t imgid, const int32_t filmid);
/** physically cope image to the folder of the film roll with filmid and
 *  duplicate update database entries. */
int32_t dt_image_copy(const int32_t imgid, const int32_t filmid);
int dt_image_local_copy_set(const int32_t imgid);
int dt_image_local_copy_reset(const int32_t imgid);
/* check whether it is safe to remove a file */
gboolean dt_image_safe_remove(const int32_t imgid);
/* try to sync .xmp for all local copies */
void dt_image_local_copy_synch(void);
// xmp functions:
void dt_image_write_sidecar_file(int imgid);
void dt_image_synch_xmp(const int selected);
void dt_image_synch_all_xmp(const gchar *pathname);

// add an offset to the exif_datetime_taken field
void dt_image_add_time_offset(const int imgid, const long int offset);

/** helper function to get the audio file filename that is accompanying the image. g_free() after use */
char *dt_image_get_audio_path(const int32_t imgid);
char *dt_image_get_audio_path_from_path(const char *image_path);
/** helper function to get the text file filename that is accompanying the image. g_free() after use */
char *dt_image_get_text_path(const int32_t imgid);
char *dt_image_get_text_path_from_path(const char *image_path);

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
