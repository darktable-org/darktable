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

#pragma once

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "common/color_harmony.h"
#include "common/colorspaces.h"
#include "common/dtpthread.h"
#include "develop/format.h"
#include <glib.h>
#include <inttypes.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/** return value of image io functions. */
typedef enum dt_imageio_retval_t
{
  DT_IMAGEIO_OK = 0,              // all good :)
  DT_IMAGEIO_FILE_NOT_FOUND,      // file has been lost
  DT_IMAGEIO_LOAD_FAILED,         // file either corrupted or in a format not supported by the current loader,
                                  // and a more detailed error from among those below is not available.
  DT_IMAGEIO_UNSUPPORTED_FORMAT,  // the file type is not supported; may be one which is a build-time option
  DT_IMAGEIO_UNSUPPORTED_CAMERA,  // the file type is supported, but the camera model is not
  DT_IMAGEIO_UNSUPPORTED_FEATURE, // the file uses an unsupported feature such as compression type
  DT_IMAGEIO_FILE_CORRUPTED,      // invalid data was detected while parsing the file
  DT_IMAGEIO_IOERROR,             // a read error occurred while loading the file
  DT_IMAGEIO_CACHE_FULL,          // buffer allocation for image data failed
  DT_IMAGEIO_UNRECOGNIZED         // file format was not recognized by loader(s)
} dt_imageio_retval_t;

typedef enum dt_imageio_write_xmp_t
{
  DT_WRITE_XMP_NEVER = 0,
  DT_WRITE_XMP_LAZY = 1,
  DT_WRITE_XMP_ALWAYS = 2
} dt_imageio_write_xmp_t;

typedef enum
{
  // the first 0x7 in flags are reserved for star ratings.
  // see view.h:
  //  DT_VIEW_DESERT = 0,
  //  DT_VIEW_STAR_1 = 1,
  //  DT_VIEW_STAR_2 = 2,
  //  DT_VIEW_STAR_3 = 3,
  //  DT_VIEW_STAR_4 = 4,
  //  DT_VIEW_STAR_5 = 5,
  DT_IMAGE_REJECTED = 8,

  // next field unused, but it used to be.
  // old DB entries might have it set.
  // To reuse : force to 0 in DB loading and force to 0 in DB saving
  // Use it to store a state that doesn't need to go in DB
  DT_IMAGE_THUMBNAIL_DEPRECATED = 16,
  // set during import if the image is low-dynamic range, i.e. doesn't
  // need demosaic, wb, highlight clipping etc.
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
  // image was detected as monochrome
  DT_IMAGE_MONOCHROME = 32768,
  // DNG image has exif tags which are not cached in the database but
  // must be read and stored in dt_image_t when the image is loaded.
  DT_IMAGE_HAS_ADDITIONAL_EXIF_TAGS = 65536,
  // image is an sraw
  DT_IMAGE_S_RAW = 1 << 17,
  // image has a monochrome preview tested
  DT_IMAGE_MONOCHROME_PREVIEW = 1 << 18,
  // image has been set to monochrome via demosaic module
  DT_IMAGE_MONOCHROME_BAYER = 1 << 19,
  // image has a flag set to use the monochrome workflow in the modules supporting it
  DT_IMAGE_MONOCHROME_WORKFLOW = 1 << 20,
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

typedef enum dt_exif_image_orientation_t
{
  EXIF_ORIENTATION_NONE              = 1,
  EXIF_ORIENTATION_FLIP_HORIZONTALLY = 2,
  EXIF_ORIENTATION_FLIP_VERTICALLY   = 4,
  EXIF_ORIENTATION_ROTATE_180_DEG    = 3,
  EXIF_ORIENTATION_TRANSPOSE         = 5,
  EXIF_ORIENTATION_ROTATE_CCW_90_DEG = 8,
  EXIF_ORIENTATION_ROTATE_CW_90_DEG  = 6,
  EXIF_ORIENTATION_TRANSVERSE        = 7
} dt_exif_image_orientation_t;

#define DT_EXIF_TAG_UNINITIALIZED (-FLT_MAX)

typedef enum dt_image_orientation_t
{
  ORIENTATION_NULL    = -1,     //-1 $DESCRIPTION: "autodetect"
  ORIENTATION_NONE    = 0,      // 0 $DESCRIPTION: "no rotation"
  ORIENTATION_FLIP_Y  = 1 << 0, // 1 $DESCRIPTION: "flip vertically"
  ORIENTATION_FLIP_X  = 1 << 1, // 2 $DESCRIPTION: "flip horizontally"
  ORIENTATION_SWAP_XY = 1 << 2, // 4 $DESCRIPTION: "transpose"

  /* ClockWise rotation == "-"; CounterClockWise rotation == "+" */
  ORIENTATION_FLIP_HORIZONTALLY = ORIENTATION_FLIP_X,                       // 2
  ORIENTATION_FLIP_VERTICALLY   = ORIENTATION_FLIP_Y,                       // 1
  ORIENTATION_ROTATE_180_DEG    = ORIENTATION_FLIP_Y | ORIENTATION_FLIP_X,  // 3 $DESCRIPTION: "rotate 180°"
  ORIENTATION_TRANSPOSE         = ORIENTATION_SWAP_XY,                      // 4
  ORIENTATION_ROTATE_CCW_90_DEG = ORIENTATION_FLIP_X | ORIENTATION_SWAP_XY, // 6 $DESCRIPTION: "rotate 90°"
  ORIENTATION_ROTATE_CW_90_DEG  = ORIENTATION_FLIP_Y | ORIENTATION_SWAP_XY, // 5 $DESCRIPTION: "rotate -90°"
  ORIENTATION_TRANSVERSE        = ORIENTATION_FLIP_Y | ORIENTATION_FLIP_X | ORIENTATION_SWAP_XY // 7 $DESCRIPTION: "transverse"
} dt_image_orientation_t;

typedef enum dt_image_correction_type_t
{
  CORRECTION_TYPE_NONE,
  CORRECTION_TYPE_SONY,
  CORRECTION_TYPE_FUJI,
  CORRECTION_TYPE_DNG,
  CORRECTION_TYPE_OLYMPUS
} dt_image_correction_type_t;

typedef union dt_image_correction_data_t
{
  struct {
    int nc;
    short distortion[16], ca_r[16], ca_b[16], vignetting[16];
  } sony;
  struct {
    int nc;
    float cropf;
    float knots[11], distortion[11], ca_r[11], ca_b[11], vignetting[11];
  } fuji;
  struct {
    int planes;
    float cwarp[3][6]; // for up to 3 planes warp rectilinear
    float centre_warp[2];
    float cvig[5];     // for vignetting
    float centre_vig[2];
    gboolean has_warp;
    gboolean has_vignette;
  } dng;
  struct {
    gboolean has_dist;
    float dist[4];
    gboolean has_ca;
    float ca[6];
  } olympus;
} dt_image_correction_data_t;

typedef enum dt_image_loader_t
{
  LOADER_UNKNOWN  =  0,
  LOADER_TIFF     =  1,
  LOADER_PNG      =  2,
  LOADER_J2K      =  3,
  LOADER_JPEG     =  4,
  LOADER_EXR      =  5,
  LOADER_RGBE     =  6,
  LOADER_PFM      =  7,
  LOADER_GM       =  8,
  LOADER_RAWSPEED =  9,
  LOADER_PNM      = 10,
  LOADER_AVIF     = 11,
  LOADER_IM       = 12,
  LOADER_HEIF     = 13,
  LOADER_LIBRAW   = 14,
  LOADER_WEBP     = 15,
  LOADER_JPEGXL   = 16,
  LOADER_QOI      = 17,
  LOADER_COUNT    = 18 // keep last
} dt_image_loader_t;

static const struct
{
  const char *tooltip;
  const char flag;
} loaders_info[LOADER_COUNT] =
{
  { N_("unknown"),         '.'}, // EMPTY_FIELD
  { N_("TIFF"),            't'},
  { N_("PNG"),             'p'},
  { N_("JPEG 2000"),       'J'},
  { N_("JPEG"),            'j'},
  { N_("EXR"),             'e'},
  { N_("RGBE"),            'R'},
  { N_("PFM"),             'P'},
  { N_("GraphicsMagick"),  'g'},
  { N_("RawSpeed"),        'r'},
  { N_("Netpbm"),          'n'},
  { N_("AVIF"),            'a'},
  { N_("ImageMagick"),     'i'},
  { N_("HEIF"),            'h'},
  { N_("LibRaw"),          'l'},
  { N_("WebP"),            'w'},
  { N_("JPEG XL"),         'L'},
  { N_("QOI"),             'q'}
};

// flags which can be passed via jobs
typedef enum dt_image_job_flag_t
{
  DT_IMAGE_JOB_NONE        = 0,
  DT_IMAGE_JOB_NO_METADATA = 1 << 0   // no metadata on exif refresh
} dt_image_job_flag_t;

typedef struct dt_image_geoloc_t
{
  double longitude, latitude, elevation;
} dt_image_geoloc_t;

struct dt_cache_entry_t;

// TODO: add color labels and such as cacheable
// __attribute__ ((aligned (128)))
typedef struct dt_image_t
{
  // minimal exif data here (all in multiples of 4-byte to interface nicely with c++):
  gboolean exif_inited;
  dt_image_orientation_t orientation;
  float exif_exposure;
  float exif_exposure_bias;
  float exif_aperture;
  float exif_iso;
  float exif_focal_length;
  float exif_focus_distance;
  float exif_crop;
  char exif_maker[64];
  char exif_model[64];
  char exif_lens[128];
  char exif_whitebalance[64];
  char exif_flash[64];
  char exif_exposure_program[64];
  char exif_metering_mode[64];
  GTimeSpan exif_datetime_taken;

  dt_image_correction_type_t exif_correction_type;
  dt_image_correction_data_t exif_correction_data;

  char camera_maker[64];
  char camera_model[64];
  char camera_alias[64];
  char camera_makermodel[128];
  gboolean camera_missing_sample;

  char filename[DT_MAX_FILENAME_LEN];

  // common stuff

  // to understand this, look at comment for dt_histogram_roi_t
  int32_t width, height, final_width, final_height;
  // p_width and p_height are updated by rawprepare
  int32_t p_width, p_height;
  // written by the image loader, data come from rawspeed; **not** changed by rawprepare
  int32_t crop_x, crop_y;
  int32_t crop_right, crop_bottom;
  float aspect_ratio;

  // used by library
  int32_t num, flags, film_id, version;
  dt_imgid_t id;
  dt_imgid_t group_id;
  //timestamps
  GTimeSpan import_timestamp, change_timestamp, export_timestamp, print_timestamp;

  dt_image_loader_t loader;

  dt_iop_buffer_dsc_t buf_dsc;

  float d65_color_matrix[9]; // the 3x3 matrix embedded in some DNGs
  uint8_t *profile;          // embedded profile, for example from JPEGs
  uint32_t profile_size;
  dt_image_colorspace_t colorspace; // the colorspace that is
                                    // specified in exif. mostly used
                                    // for jpeg files

  dt_image_raw_parameters_t legacy_flip; // unfortunately needed to
                                         // convert old bits to new
                                         // flip module.

  /* gps coords */
  dt_image_geoloc_t geoloc;

  /* color harmony guide */
  dt_color_harmony_guide_t color_harmony_guide;

  /* needed in exposure iop for Deflicker */
  uint16_t raw_black_level;
  uint16_t raw_black_level_separate[4];
  uint32_t raw_white_point;

  /* needed to fix some manufacturers madness */
  uint32_t fuji_rotation_pos;
  float pixel_aspect_ratio;

  /* might help to improve highlights reconstruction module */
  float linear_response_limit;

  /* White balance coeffs from the raw */
  dt_aligned_pixel_t wb_coeffs;

  /* Adobe coeffs from the raw */
  float adobe_XYZ_to_CAM[4][3];

  /* DefaultUserCrop */
  dt_boundingbox_t usercrop;

  /* GainMaps from DNG OpcodeList2 exif tag */
  GList *dng_gain_maps;

  /* convenience pointer back into the image cache, so we can return
   * dt_image_t* there directly. */
  struct dt_cache_entry_t *cache_entry;

  dt_image_job_flag_t job_flags;

  /* result of attempting to load the image, needed to be able to report why the image can't be displayed */
  dt_imageio_retval_t load_status;
} dt_image_t;

// should be in datetime.h, workaround to solve cross references
#define DT_DATETIME_LENGTH 24       // includes msec

typedef struct dt_image_basic_exif_t
{
  char datetime[DT_DATETIME_LENGTH];
  char maker[64];
  char model[64];
} dt_image_basic_exif_t;

// image buffer operations:
/** inits basic values to sensible defaults. */
void dt_image_init(dt_image_t *img);
/** Refresh makermodel from the raw and exif values **/
void dt_image_refresh_makermodel(dt_image_t *img);
/** returns TRUE if the image contains low-dynamic range data. */
gboolean dt_image_is_ldr(const dt_image_t *img);
/** returns TRUE if the image contains mosaic data. */
gboolean dt_image_is_raw(const dt_image_t *img);
/** returns TRUE if the image contains float data. */
gboolean dt_image_is_hdr(const dt_image_t *img);
/** set the monochrome flags if monochrome is TRUE and clear it otherwise */
void dt_image_set_monochrome_flag(const dt_imgid_t imgid, const gboolean monochrome);
/** returns TRUE if this image was taken using a monochrome camera */
gboolean dt_image_is_monochrome(const dt_image_t *img);
/** returns TRUE is image has a raw bayer sensor with RGB data */
gboolean dt_image_is_bayerRGB(const dt_image_t *img);
/** returns TRUE if the image supports a color correction matrix */
gboolean dt_image_is_matrix_correction_supported(const dt_image_t *img);
/** returns TRUE if the image supports the rawprepare module */
gboolean dt_image_is_rawprepare_supported(const dt_image_t *img);
/** returns the bitmask containing info about monochrome images */
int dt_image_monochrome_flags(const dt_image_t *img);
/** returns TRUE if the image has been tested to be monochrome and the
 * image wants monochrome workflow */
gboolean dt_image_use_monochrome_workflow(const dt_image_t *img);
/** returns the image filename */
char *dt_image_get_filename(const dt_imgid_t imgid);
/** returns true if the image exists on the database */
gboolean dt_image_exists(const dt_imgid_t imgid);
/** returns the full path name where the image was imported
 * from. from_cache=TRUE check and return local cached filename if
 * any. */
void dt_image_full_path(const dt_imgid_t imgid,
                        char *pathname,
                        const size_t pathname_len,
                        gboolean *from_cache);
/** returns the full directory of the associated film roll. */
void dt_image_film_roll_directory(const dt_image_t *img,
                                  char *pathname,
                                  const size_t pathname_len);
/** returns the portion of the path used for the film roll name. */
const char *dt_image_film_roll_name(const char *path);
/** returns the film roll name, i.e. without the path. */
void dt_image_film_roll(const dt_image_t *img,
                        char *pathname,
                        const size_t pathname_len);
/** appends version numbering for duplicated images without querying the db. */
void dt_image_path_append_version_no_db(const int version,
                                        char *pathname,
                                        const size_t pathname_len);
/** appends version numbering for duplicated images. */
void dt_image_path_append_version(const dt_imgid_t imgid,
                                  char *pathname,
                                  const size_t pathname_len);
/** prints a one-line exif information string. */
void dt_image_print_exif(const dt_image_t *img,
                         char *line,
                         const size_t line_len);
/* set rating to img flags */
void dt_image_set_xmp_rating(dt_image_t *img, const int rating);
/* get rating from img flags */
int dt_image_get_xmp_rating(const dt_image_t *img);
int dt_image_get_xmp_rating_from_flags(const int flags);
/** finds all xmp duplicates for the given image in the database. */
GList* dt_image_find_duplicates(const char* filename);
/** get image id by filename */
dt_imgid_t dt_image_get_id_full_path(const gchar *filename);
/** get image id by film_id and filename */
dt_imgid_t dt_image_get_id(const dt_filmid_t film_id,
                           const gchar *filename);
/** imports a new image from raw/etc file and adds it to the data base and image cache. Use from threads other than lua.*/
dt_imgid_t dt_image_import(dt_filmid_t film_id,
                           const char *filename,
                           const gboolean override_ignore_nonraws,
                           const gboolean raise_signals);
/** imports a new image from raw/etc file and adds it to the data base
 * and image cache. Use from lua thread.*/
dt_imgid_t dt_image_import_lua(const dt_filmid_t film_id,
                               const char *filename,
                               const gboolean override_ignore_nonraws);
/** removes the given image from the database. */
void dt_image_remove(const dt_imgid_t imgid);
/** duplicates the given image in the database with the duplicate
    getting the supplied version number. if that version already
    exists just return the imgid without producing new
    duplicate. called with newversion -1 a new duplicate is produced
    with the next free version number. */
dt_imgid_t dt_image_duplicate_with_version(const dt_imgid_t imgid,
                                           const int32_t newversion);
/** duplicates the given image in the database. */
dt_imgid_t dt_image_duplicate(const dt_imgid_t imgid);
/** flips the image, clock wise, if given flag. */
void dt_image_flip(const dt_imgid_t imgid, const int32_t cw);
void dt_image_set_flip(const dt_imgid_t imgid, const dt_image_orientation_t user_flip);
dt_image_orientation_t dt_image_get_orientation(const dt_imgid_t imgid);
/** get max width and height of the final processed image with its current history stack */
gboolean dt_image_get_final_size(const dt_imgid_t imgid, int *width, int *height);
void dt_image_update_final_size(const dt_imgid_t imgid);
/** set image location lon/lat/ele */
void dt_image_set_location(const dt_imgid_t imgid,
                           const dt_image_geoloc_t *geoloc,
                           const gboolean undo_on,
                           const gboolean group_on);
/** set images location lon/lat/ele */
void dt_image_set_locations(const GList *img,
                            const dt_image_geoloc_t *geoloc,
                            const gboolean undo_on);
/** set images locations lon/lat/ele */
void dt_image_set_images_locations(const GList *imgs,
                                   const GArray *gloc,
                                   const gboolean undo_on);
/** get image location lon/lat/ele */
void dt_image_get_location(const dt_imgid_t imgid, dt_image_geoloc_t *geoloc);
/** returns TRUE if current hash is not basic nor auto_apply, FALSE otherwise. */
gboolean dt_image_altered(const dt_imgid_t imgid);
/** returns TRUE if current hash is basic, FALSE otherwise. */
gboolean dt_image_basic(const dt_imgid_t imgid);
/** set the image final/cropped aspect ratio */
float dt_image_set_aspect_ratio(const dt_imgid_t imgid, const gboolean raise);
/** set the image raw aspect ratio */
void dt_image_set_raw_aspect_ratio(const dt_imgid_t imgid);
/** set the image final/cropped aspect ratio */
void dt_image_set_aspect_ratio_to(const dt_imgid_t imgid,
                                  const float aspect_ratio,
                                  const gboolean raise);
/** set the image final/cropped aspect ratio if different from stored*/
void dt_image_set_aspect_ratio_if_different(const dt_imgid_t imgid,
                                            const float aspect_ratio,
                                            const gboolean raise);
/** reset the image final/cropped aspect ratio to 0.0 */
void dt_image_reset_aspect_ratio(const dt_imgid_t imgid,
                                 const gboolean raise);
/** reset the image final/cropped aspect ratio to 0.0 */
gboolean dt_image_set_history_end(const dt_imgid_t imgid,
                                  const int history_end);
/** get the ratio of cropped raw sensor data */
float dt_image_get_sensor_ratio(const dt_image_t *img);

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
    case EXIF_ORIENTATION_NONE:
      return ORIENTATION_NONE;
    case EXIF_ORIENTATION_FLIP_HORIZONTALLY:
      return ORIENTATION_FLIP_HORIZONTALLY;
    case EXIF_ORIENTATION_ROTATE_180_DEG:
      return ORIENTATION_ROTATE_180_DEG;
    case EXIF_ORIENTATION_FLIP_VERTICALLY:
      return ORIENTATION_FLIP_VERTICALLY;
    case EXIF_ORIENTATION_TRANSPOSE:
      return ORIENTATION_TRANSPOSE;
    case EXIF_ORIENTATION_ROTATE_CW_90_DEG:
      return ORIENTATION_ROTATE_CW_90_DEG;
    case EXIF_ORIENTATION_TRANSVERSE:
      return ORIENTATION_TRANSVERSE;
    case EXIF_ORIENTATION_ROTATE_CCW_90_DEG:
      return ORIENTATION_ROTATE_CCW_90_DEG;
    default:
      return ORIENTATION_NONE;
  }
}

/** return the raw orientation from heif transforms */
static inline dt_image_orientation_t dt_image_transformation_to_flip_bits(const int angle, const int flip)
{
  if(angle == 1)
  {
    if(flip == 1) return ORIENTATION_TRANSVERSE;
    if(flip == 0) return ORIENTATION_TRANSPOSE;
    return ORIENTATION_ROTATE_CCW_90_DEG;
  }
  if(angle == 2)
  {
    if(flip == 1) return ORIENTATION_FLIP_VERTICALLY;
    if(flip == 0) return ORIENTATION_FLIP_HORIZONTALLY;
    return ORIENTATION_ROTATE_180_DEG;
  }
  if(angle == 3)
  {
    if(flip == 1) return ORIENTATION_TRANSPOSE;
    if(flip == 0) return ORIENTATION_TRANSVERSE;
    return ORIENTATION_ROTATE_CW_90_DEG;
  }
  if(flip == 1) return ORIENTATION_FLIP_HORIZONTALLY;
  if(flip == 0) return ORIENTATION_FLIP_VERTICALLY;
  return ORIENTATION_NONE;
}

/** physically move image with imgid and its duplicates to the film roll
 *  given by filmid. returns TRUE on error, FALSE on success. */
gboolean dt_image_move(const dt_imgid_t imgid, const dt_filmid_t filmid);
/** physically move image with imgid and its duplicates to the film roll
 *  given by filmid and the name given by newname.
 *  returns TRUE on error, FALSE on success. */
gboolean dt_image_rename(const dt_imgid_t imgid, const dt_filmid_t filmid, const gchar *newname);
/** physically copy image to the folder of the film roll with filmid and
 *  duplicate update database entries. */
dt_imgid_t dt_image_copy(const dt_imgid_t imgid, const dt_filmid_t filmid);
/** physically copy image to the folder of the film roll with filmid and
 *  the name given by newname, and duplicate update database entries. */
dt_imgid_t dt_image_copy_rename(const dt_imgid_t imgid,
                                const dt_filmid_t filmid,
                                const gchar *newname);
gboolean dt_image_local_copy_set(const dt_imgid_t imgid);
gboolean dt_image_local_copy_reset(const dt_imgid_t imgid);
/* check whether it is safe to remove a file */
gboolean dt_image_safe_remove(const dt_imgid_t imgid);
/* try to sync .xmp for all local copies */
void dt_image_local_copy_synch(void);
// xmp functions:
gboolean dt_image_write_sidecar_file(const dt_imgid_t imgid);
void dt_image_synch_xmp(const int32_t selected);
void dt_image_synch_xmps(const GList *img);
void dt_image_synch_all_xmp(const gchar *pathname);
/** get the mode xmp sidecars are written */
dt_imageio_write_xmp_t dt_image_get_xmp_mode();

// add an offset to the exif_datetime_taken field
void dt_image_add_time_offset(const dt_imgid_t imgid, const long int offset);
// set datetime to exif_datetime_taken field
void dt_image_set_datetime(const GList *imgs,
                           const char *datetime,
                           const gboolean undo_on);
// set datetimeS to exif_datetime_taken field
void dt_image_set_datetimes(const GList *imgs,
                            const GArray *dtime,
                            const gboolean undo_on);
// return image datetime string into the given buffer (size = DT_DATETIME_LENGTH)
void dt_image_get_datetime(const dt_imgid_t imgid, char *datetime);

/** helper function to get the audio file filename that is
 * accompanying the image. g_free() after use */
char *dt_image_get_audio_path(const dt_imgid_t imgid);
char *dt_image_get_audio_path_from_path(const char *image_path);
/** helper function to get the text file filename that is accompanying
 * the image. g_free() after use */
char *dt_image_get_text_path(const dt_imgid_t imgid);
char *dt_image_get_text_path_from_path(const char *image_path);

float dt_image_get_exposure_bias(const struct dt_image_t *image_storage);

/** handle message for missing camera samples reported by rawspeed */
char *dt_image_camera_missing_sample_message(const struct dt_image_t *img,
                                             const gboolean logmsg);
void dt_image_check_camera_missing_sample(const struct dt_image_t *img);

/**   insert the new data if it does not exists.
      returns the corresponding id for the name */
int32_t dt_image_get_camera_maker_id(const char *name);
int32_t dt_image_get_camera_model_id(const char *name);
int32_t dt_image_get_camera_lens_id(const char *name);
int32_t dt_image_get_whitebalance_id(const char *name);
int32_t dt_image_get_flash_id(const char *name);
int32_t dt_image_get_exposure_program_id(const char *name);
int32_t dt_image_get_metering_mode_id(const char *name);
int32_t dt_image_get_camera_id(const char *maker, const char *model);

#ifdef __cplusplus
} // extern "C"
#endif /* __cplusplus */

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
