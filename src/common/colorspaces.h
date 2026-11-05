/*
    This file is part of darktable,
    Copyright (C) 2010-2026 darktable developers.

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

#include "common/hdr-transfer.h"

#include "common/darktable.h"
#include "common/dttypes.h"

#include <lcms2.h>

G_BEGIN_DECLS

// this was removed from lcms2 in 2.4
#ifndef TYPE_XYZA_FLT
  #define TYPE_XYZA_FLT (FLOAT_SH(1)|COLORSPACE_SH(PT_XYZ)|EXTRA_SH(1)|CHANNELS_SH(3)|BYTES_SH(4))
#endif

// CIE 1931 2° as used in dt
//D50 (ProPhoto RGB)
static const cmsCIExyY D50xyY = {0.34567, 0.35850, 1.0};

//D65 (sRGB, AdobeRGB, Rec2020)
static const cmsCIExyY D65xyY = {0.31271, 0.32902, 1.0};

/** colorspace enums, must be in synch with dt_iop_colorspace_type_t
 * in color_conversion.cl */
typedef enum dt_iop_colorspace_type_t
{
  IOP_CS_NONE = -1,
  IOP_CS_RAW = 0,
  IOP_CS_LAB = 1,
  IOP_CS_RGB = 2,
  IOP_CS_LCH = 3,
  IOP_CS_HSL = 4,
  IOP_CS_JZCZHZ = 5,
} dt_iop_colorspace_type_t;

// max iccprofile file name length
#define DT_IOP_COLOR_ICC_LEN 512

// constants fit to the ones from lcms.h:
typedef enum dt_iop_color_intent_t
{
  DT_INTENT_PERCEPTUAL = INTENT_PERCEPTUAL,                       // 0
  DT_INTENT_RELATIVE_COLORIMETRIC = INTENT_RELATIVE_COLORIMETRIC, // 1
  DT_INTENT_SATURATION = INTENT_SATURATION,                       // 2
  DT_INTENT_ABSOLUTE_COLORIMETRIC = INTENT_ABSOLUTE_COLORIMETRIC, // 3
  DT_INTENT_LAST
} dt_iop_color_intent_t;

typedef enum dt_colorspaces_profile_type_t
{
  DT_COLORSPACES_PROFILE_TYPE_INPUT = 1,
  DT_COLORSPACES_PROFILE_TYPE_WORK = 2,
  DT_COLORSPACES_PROFILE_TYPE_EXPORT = 3,
  DT_COLORSPACES_PROFILE_TYPE_DISPLAY = 4,
  DT_COLORSPACES_PROFILE_TYPE_SOFTPROOF = 5,
  DT_COLORSPACES_PROFILE_TYPE_HISTOGRAM = 6,
  DT_COLORSPACES_PROFILE_TYPE_DISPLAY2 = 7
} dt_colorspaces_profile_type_t;

typedef enum dt_colorspaces_color_profile_type_t
{
  DT_COLORSPACE_NONE = -1,
  DT_COLORSPACE_FILE = 0,
  DT_COLORSPACE_SRGB = 1,
  DT_COLORSPACE_ADOBERGB = 2,
  DT_COLORSPACE_LIN_REC709 = 3,
  DT_COLORSPACE_LIN_REC2020 = 4,
  DT_COLORSPACE_XYZ = 5,
  DT_COLORSPACE_LAB = 6,
  DT_COLORSPACE_INFRARED = 7,
  DT_COLORSPACE_DISPLAY = 8,
  DT_COLORSPACE_EMBEDDED_ICC = 9,
  DT_COLORSPACE_EMBEDDED_MATRIX = 10,
  DT_COLORSPACE_STANDARD_MATRIX = 11,
  DT_COLORSPACE_ENHANCED_MATRIX = 12,
  DT_COLORSPACE_VENDOR_MATRIX = 13,
  DT_COLORSPACE_ALTERNATE_MATRIX = 14,
  DT_COLORSPACE_BRG = 15,
  DT_COLORSPACE_EXPORT = 16, // export and softproof are categories and will return NULL with dt_colorspaces_get_profile()
  DT_COLORSPACE_SOFTPROOF = 17,
  DT_COLORSPACE_WORK = 18,
  DT_COLORSPACE_DISPLAY2 = 19,
  DT_COLORSPACE_REC709 = 20,
  DT_COLORSPACE_PROPHOTO_RGB = 21,
  DT_COLORSPACE_PQ_REC2020 = 22,
  DT_COLORSPACE_HLG_REC2020 = 23,
  DT_COLORSPACE_PQ_P3 = 24,
  DT_COLORSPACE_HLG_P3 = 25,
  DT_COLORSPACE_DISPLAY_P3 = 26,
  DT_COLORSPACE_FORWARD_MATRIX = 27,
  DT_COLORSPACE_DNG_LOOK = 28,
  DT_COLORSPACE_LAST = 29
} dt_colorspaces_color_profile_type_t;

/* Built-in CICP HDR profiles use RGB transfer functions, not ICC shapers. */
static inline int dt_colorspaces_hdr_transfer(const dt_colorspaces_color_profile_type_t type)
{
  switch(type)
  {
    case DT_COLORSPACE_PQ_REC2020:
    case DT_COLORSPACE_PQ_P3:
      return DT_HDR_PQ;
    case DT_COLORSPACE_HLG_REC2020:
      return DT_HDR_HLG_REC2020;
    case DT_COLORSPACE_HLG_P3:
      return DT_HDR_HLG_P3;
    default:
      return DT_HDR_NONE;
  }
}

/* Owned copy of a matrix profile with linear shapers, for HDR RGB math. */
cmsHPROFILE dt_colorspaces_linearize_profile(cmsHPROFILE profile);

typedef enum dt_colorspaces_color_mode_t
{
  DT_PROFILE_NORMAL = 0,
  DT_PROFILE_SOFTPROOF,
  DT_PROFILE_GAMUTCHECK
} dt_colorspaces_color_mode_t;

typedef enum dt_colorspaces_profile_direction_t
{
  DT_PROFILE_DIRECTION_IN = 1 << 0,
  DT_PROFILE_DIRECTION_OUT = 1 << 1,
  DT_PROFILE_DIRECTION_DISPLAY = 1 << 2,
  DT_PROFILE_DIRECTION_CATEGORY = 1 << 3, // categories will return NULL with dt_colorspaces_get_profile()
  DT_PROFILE_DIRECTION_WORK = 1 << 4,
  DT_PROFILE_DIRECTION_DISPLAY2 = 1 << 5,
  DT_PROFILE_DIRECTION_ANY = DT_PROFILE_DIRECTION_IN | DT_PROFILE_DIRECTION_OUT | DT_PROFILE_DIRECTION_DISPLAY
                             | DT_PROFILE_DIRECTION_CATEGORY
                             | DT_PROFILE_DIRECTION_WORK
                             | DT_PROFILE_DIRECTION_DISPLAY2
} dt_colorspaces_profile_direction_t;

/* CICP color primaries (Recommendation ITU-T H.273) */
typedef enum dt_colorspaces_cicp_color_primaries_t
{
    DT_CICP_COLOR_PRIMARIES_REC709 = 1,
    DT_CICP_COLOR_PRIMARIES_UNSPECIFIED = 2,
    DT_CICP_COLOR_PRIMARIES_REC2020 = 9,
    DT_CICP_COLOR_PRIMARIES_XYZ = 10,
    DT_CICP_COLOR_PRIMARIES_P3 = 12 // D65
} dt_colorspaces_cicp_color_primaries_t;

/* CICP transfer characteristics (Recommendation ITU-T H.273) */
typedef enum dt_colorspaces_cicp_transfer_characteristics_t
{
    DT_CICP_TRANSFER_CHARACTERISTICS_REC709 = 1,
    DT_CICP_TRANSFER_CHARACTERISTICS_UNSPECIFIED = 2,
    DT_CICP_TRANSFER_CHARACTERISTICS_REC601 = 6,
    DT_CICP_TRANSFER_CHARACTERISTICS_LINEAR = 8,
    DT_CICP_TRANSFER_CHARACTERISTICS_SRGB = 13,
    DT_CICP_TRANSFER_CHARACTERISTICS_REC2020_10B = 14,
    DT_CICP_TRANSFER_CHARACTERISTICS_REC2020_12B = 15,
    DT_CICP_TRANSFER_CHARACTERISTICS_PQ = 16,
    DT_CICP_TRANSFER_CHARACTERISTICS_HLG = 18
} dt_colorspaces_cicp_transfer_characteristics_t;

/* CICP matrix coefficients (Recommendation ITU-T H.273) */
typedef enum dt_colorspaces_cicp_matrix_coefficients_t
{
    DT_CICP_MATRIX_COEFFICIENTS_IDENTITY = 0,
    DT_CICP_MATRIX_COEFFICIENTS_UNSPECIFIED = 2
} dt_colorspaces_cicp_matrix_coefficients_t;

/* a new screen profile replaces a system display profile under the same
 * type and file name. FILE profiles are read once at startup and built-ins
 * never change, so for every other type, type and file name identify the
 * profile's content */
static inline gboolean dt_colorspaces_replaced_in_session(const dt_colorspaces_color_profile_type_t type)
{
  return type == DT_COLORSPACE_DISPLAY || type == DT_COLORSPACE_DISPLAY2;
}

/* what a pipe's output values mean: this profile's encoding (RGB, Lab or
 * XYZ, PQ and HLG with their transfer functions), made with this intent and
 * rendered for a peak of headroom times reference white. resolved: what
 * the pipe produced after every fallback, not a request. type
 * DT_COLORSPACE_NONE: no profile describes the values */
typedef struct dt_color_encoding_t
{
  dt_colorspaces_color_profile_type_t type;
  char filename[DT_IOP_COLOR_ICC_LEN];
  // the registry's profile the values were made with. registry profiles
  // live for the session, so this names the content even after a new
  // screen profile replaced a system display profile of the same type
  cmsHPROFILE profile;
  dt_iop_color_intent_t intent;
  float headroom;
} dt_color_encoding_t;

/* the two encodings name the same profile content */
static inline gboolean dt_color_encoding_same_content(const dt_color_encoding_t *a,
                                                      const dt_color_encoding_t *b)
{
  return a->type == b->type
    && (!dt_colorspaces_replaced_in_session(a->type) || a->profile == b->profile)
    && !strcmp(a->filename, b->filename);
}

static inline gboolean dt_color_encoding_equal(const dt_color_encoding_t *a,
                                               const dt_color_encoding_t *b)
{
  return dt_color_encoding_same_content(a, b)
    && a->intent == b->intent
    && a->headroom == b->headroom;
}

/* what a window wants. image is what its pipes render to: display-referred
 * RGB, 1.0 at reference white and up to image.headroom above it. device is
 * what untagged 8-bit values mean in the window, everything GTK draws
 * there. both are resolved: profile names the registry's profile, NULL if
 * there is none. as the label of a frame, image is what the pipe produced
 * and the rest is the output the frame was made for */
typedef struct dt_display_output_t
{
  dt_color_encoding_t image;
  dt_color_encoding_t device;
  // the platform owns the window's output: the display profile selection
  // has no effect on it
  gboolean platform_owned;
  // a color-managed presenter shows the values in the profile; otherwise
  // they reach the screen as the device values the display profile assumes.
  // a managed output is linear Rec.2020 today
  gboolean managed;
  // changes whenever anything above changes; continues across owners
  uint32_t serial;
} dt_display_output_t;

/* the same output, whatever the serial */
static inline gboolean dt_display_output_equal(const dt_display_output_t *a,
                                               const dt_display_output_t *b)
{
  return dt_color_encoding_equal(&a->image, &b->image)
    && dt_color_encoding_equal(&a->device, &b->device)
    && a->platform_owned == b->platform_owned
    && a->managed == b->managed;
}

/* the 8-bit value a display-encoded float shows as on an 8-bit surface.
 * built from comparisons and conversions, not libm calls, so that loops
 * over it vectorize */
static inline uint8_t dt_display_to_8bit(const float v)
{
  const float x = 255.0f * v;
  // NaN fails both comparisons and goes to 0
  const float c = x > 0.0f ? (x < 255.0f ? x : 255.0f) : 0.0f;
  // c is in [0, 255], so the truncation is its floor and t + 0.5 is exact.
  // halves round up; lrintf() would round them to even. c - t >= 0.5 would
  // be the same test, but -ffp-contract may fuse that subtraction with the
  // multiply above, and the unrounded product rounds differently
  const int t = (int)c;
  return (uint8_t)(t + (c >= (float)t + 0.5f));
}

/* n pixels of display-encoded float RGBA through dt_display_to_8bit(), as
 * RGBX or, with bgr, as BGRX. the fourth byte is 0xff */
void dt_display_to_8bit_pixels(const float *const __restrict__ in,
                               uint8_t *const __restrict__ out,
                               const size_t n,
                               const gboolean bgr);

/* the mask and channel views are authored as sRGB device values, which a
 * display profile output sends to the screen as they are. a managed
 * output, linear Rec.2020, gets them converted */
static inline void dt_display_output_diagnostic(const dt_display_output_t *output,
                                                float *const rgb)
{
  if(!output->managed || output->image.type != DT_COLORSPACE_LIN_REC2020) return;

  float lin[3];
  for(int c = 0; c < 3; c++)
    lin[c] = rgb[c] <= 0.04045f ? rgb[c] / 12.92f
                                : powf((rgb[c] + 0.055f) / 1.055f, 2.4f);

  // BT.709 to BT.2020 primaries, ITU-R BT.2087
  rgb[0] = 0.6274040f * lin[0] + 0.3292820f * lin[1] + 0.0433136f * lin[2];
  rgb[1] = 0.0690970f * lin[0] + 0.9195400f * lin[1] + 0.0113612f * lin[2];
  rgb[2] = 0.0163916f * lin[0] + 0.0880132f * lin[1] + 0.8955950f * lin[2];
}

typedef struct dt_colorspaces_t
{
  GList *profiles;

  // xatom color profile:
  pthread_rwlock_t xprofile_lock;
  gchar *colord_profile_file;
  const uint8_t *xprofile_data;
  int xprofile_size;

  gchar *colord_profile_file2;
  const uint8_t *xprofile_data2;
  int xprofile_size2;

  // every profile the system display entries have had, owning the bytes
  // xprofile_data points to. none is freed before cleanup: a profile read
  // from the registry stays valid after a replacement, without the lock
  GList *system_display_profiles;

  // the current set of selected profiles
  dt_colorspaces_color_profile_type_t display_type;
  dt_colorspaces_color_profile_type_t display2_type;
  dt_colorspaces_color_profile_type_t softproof_type;
  dt_colorspaces_color_profile_type_t histogram_type;
  char display_filename[512];
  char display2_filename[512];
  char softproof_filename[512];
  char histogram_filename[512];
  dt_iop_color_intent_t display_intent;
  dt_iop_color_intent_t display2_intent;
  dt_iop_color_intent_t softproof_intent;

  dt_colorspaces_color_mode_t mode;

  // run these through dt_colorspaces_transform_to_display(), which needs the
  // matching hdr transfer
  cmsHTRANSFORM transform_srgb_to_display, transform_adobe_rgb_to_display;
  cmsHTRANSFORM transform_srgb_to_display2, transform_adobe_rgb_to_display2;
  int display_hdr_transfer, display2_hdr_transfer;

  // what each window wants, index 0 the main window: described from the
  // selection unless the platform owns it. written under xprofile_lock
  dt_display_output_t outputs[2];

} dt_colorspaces_t;

typedef struct dt_colorspaces_color_profile_t
{
  dt_colorspaces_color_profile_type_t type; // filename is only used for type DT_COLORSPACE_FILE
  char filename[DT_IOP_COLOR_ICC_LEN];      // icc file name
  char name[512];                           // product name, displayed in GUI
  cmsHPROFILE profile;                      // the actual profile
  int in_pos;                               // position in input combo box, -1 if not applicable
  int out_pos;                              // position in output combo box, -1 if not applicable
  int display_pos;                          // position in display combo box, -1 if not applicable
  int display2_pos;                         // position in display2 combo box, -1 if not applicable
  int category_pos;                         // position in category combo box, -1 if not applicable
  int work_pos;                             // position in working combo box, -1 if not applicable
} dt_colorspaces_color_profile_t;

typedef struct dt_colorspaces_cicp_t
{
    dt_colorspaces_cicp_color_primaries_t color_primaries;
    dt_colorspaces_cicp_transfer_characteristics_t transfer_characteristics;
    dt_colorspaces_cicp_matrix_coefficients_t matrix_coefficients;
} dt_colorspaces_cicp_t;

/** populate the global color profile lists */
dt_colorspaces_t *dt_colorspaces_init();

/** cleanup on shutdown */
void dt_colorspaces_cleanup(dt_colorspaces_t *self);

/** create a profile from a xyz->camera matrix. */
cmsHPROFILE dt_colorspaces_create_xyzimatrix_profile(float cam_xyz[3][3]);
cmsHPROFILE dt_colorspaces_create_xyzmatrix_profile(const float mat[3][3]);

/** create a ICC virtual profile from the shipped presets in
 * darktable. */
cmsHPROFILE dt_colorspaces_create_darktable_profile(const char *makermodel);

/** create a ICC virtual profile from the shipped vendor matrices in
 * darktable. */
cmsHPROFILE dt_colorspaces_create_vendor_profile(const char *makermodel);

/** create a ICC virtual profile from the shipped alternate matrices
 * in darktable. */
cmsHPROFILE dt_colorspaces_create_alternate_profile(const char *makermodel);

/** return the work profile as set in colorin */
const dt_colorspaces_color_profile_t *dt_colorspaces_get_work_profile
  (const dt_imgid_t imgid);

/** return the output profile as set in colorout, taking export
 * override into account if passed in. */
const dt_colorspaces_color_profile_t *dt_colorspaces_get_output_profile
  (const dt_imgid_t imgid,
   dt_colorspaces_color_profile_type_t over_type,
   const char *over_filename);

/** return an rgb lcms2 profile from data. if data points to a
 * grayscale profile a new rgb profile is created that has the same
 * TRC, black and white point and rec709 primaries. */
cmsHPROFILE dt_colorspaces_get_rgb_profile_from_mem(uint8_t *data,
                                                    const uint32_t size);

/** free the resources of a profile created with the functions above. */
void dt_colorspaces_cleanup_profile(cmsHPROFILE p);

/** extracts tonecurves and color matrix prof to XYZ from a given
 * input profile, returns 0 on success (curves and matrix are inverted
 * for input) */
int dt_colorspaces_get_matrix_from_input_profile(cmsHPROFILE prof,
                                                 dt_colormatrix_t matrix,
                                                 float *lutr,
                                                 float *lutg,
                                                 float *lutb,
                                                 const int lutsize);

/** extracts tonecurves and color matrix prof to XYZ from a given
 * output profile, returns 0 on success. */
int dt_colorspaces_get_matrix_from_output_profile(cmsHPROFILE prof,
                                                  dt_colormatrix_t matrix,
                                                  float *lutr,
                                                  float *lutg,
                                                  float *lutb,
                                                  const int lutsize);

/** create a temporary profile to be removed by
 * dt_colorspaces_cleanup_profile */
cmsHPROFILE dt_colorspaces_make_temporary_profile(cmsHPROFILE profile);

/** wrapper to get the name from a color profile. this tries to handle
 * character encodings. */
void dt_colorspaces_get_profile_name(cmsHPROFILE p,
                                     const char *language,
                                     const char *country,
                                     char *name,
                                     const size_t len);

/** get a nice printable name. */
const char *dt_colorspaces_get_name(dt_colorspaces_color_profile_type_t type,
                                    const char *filename);

/** common functions to change between colorspaces, used in iop modules */
//void rgb2hsl(const dt_aligned_pixel_t rgb, float *h, float *s, float *l);
//void hsl2rgb(dt_aligned_pixel_t rgb, float h, float s, float l);
static inline void rgb2hsl(const dt_aligned_pixel_t rgb,
                           float *h,
                           float *s,
                           float *l)
{
  const float r = rgb[0], g = rgb[1], b = rgb[2];
  const float pmax = max3f(rgb);
  const float pmin = min3f(rgb);
  const float delta = (pmax - pmin);

  float hv = 0, sv = 0, lv = (pmin + pmax) / 2.0;

  if(delta != 0.0f)
  {
    sv = lv < 0.5 ? delta / fmaxf(pmax + pmin, 1.52587890625e-05f)
                  : delta / fmaxf(2.0 - pmax - pmin, 1.52587890625e-05f);

    if(pmax == r)
      hv = (g - b) / delta;
    else if(pmax == g)
      hv = 2.0 + (b - r) / delta;
    else if(pmax == b)
      hv = 4.0 + (r - g) / delta;
    hv /= 6.0;
    if(hv < 0.0)
      hv += 1.0;
    else if(hv > 1.0)
      hv -= 1.0;
  }
  *h = hv;
  *s = sv;
  *l = lv;
}

// for efficiency, 'hue' must be pre-scaled to be in 0..6
static inline float hue2rgb(float m1, float m2, float hue)
{
  // compute the value for one of the RGB channels from the hue angle.
  // If 1 <= angle < 3, return m2; if 4 <= angle <= 6, return m1;
  // otherwise, linearly interpolate between m1 and m2.
  if(hue < 1.0f)
    return (m1 + (m2 - m1) * hue);
  else if(hue < 3.0f)
    return m2;
  else
    return hue < 4.0f ? (m1 + (m2 - m1) * (4.0f - hue)) : m1;
}

static inline void hsl2rgb(dt_aligned_pixel_t rgb,
                           float h,
                           const float s,
                           const float l)
{
  if(s == 0)
  {
    rgb[0] = rgb[1] = rgb[2] = l;
    rgb[3] = 0.0f;
    return;
  }
  const float m2 = l < 0.5 ? l * (1.0 + s) : l + s - l * s;
  const float m1 = (2.0 * l - m2);
  h *= 6.0f;  // pre-scale hue angle
  rgb[0] = hue2rgb(m1, m2, h < 4.0f ? h + 2.0f : h - 4.0f);
  rgb[1] = hue2rgb(m1, m2, h);
  rgb[2] = hue2rgb(m1, m2, h > 2.0f ? h - 2.0f : h + 4.0f);
  rgb[3] = 0.0f;
}



/** trigger updating the display profile from the system settings (x
 * atom, colord, ...) */
void dt_colorspaces_set_display_profile
  (const dt_colorspaces_color_profile_type_t profile_type);

/** get the profile described by type & filename. this doesn't
 *  support image specifics like embedded profiles or camera
 *  matrices */
const dt_colorspaces_color_profile_t *
dt_colorspaces_get_profile(dt_colorspaces_color_profile_type_t type,
                           const char *filename,
                           const dt_colorspaces_profile_direction_t direction);

/** check whether filename is the same profil as fullname, this is
 *  taking into account that fullname is always the fullpathname to
 *  the profile and filename may be a full pathname or just a base
 *  name */
gboolean  dt_colorspaces_is_profile_equal(const char *fullname,
                                          const char *filename);

/** try to infer profile type from CICP */
dt_colorspaces_color_profile_type_t dt_colorspaces_cicp_to_type
  (const dt_colorspaces_cicp_t *cicp,
   const char *filename);

/** what a window wants, defined from dt_colorspaces_init() on, whether the
 * window exists or not: pass DT_COLORSPACE_DISPLAY for the main window,
 * DT_COLORSPACE_DISPLAY2 for the second one */
void dt_colorspaces_get_display_output(const dt_colorspaces_color_profile_type_t window,
                                       dt_display_output_t *output);

/** what the display profile selection of a window describes: its profile
 * and intent as image and device encoding, with the headroom of the
 * profile's transfer function, serial 0. no side effects; hold
 * xprofile_lock */
void dt_colorspaces_describe_selection(dt_colorspaces_t *self,
                                       const dt_colorspaces_color_profile_type_t window,
                                       dt_display_output_t *output);

/** every setter of a window's display selection (profile, intent, a new
 * system display profile, the PQ peak) calls this. the window's output
 * follows the selection unless the platform owns it, which keeps the
 * selection for when it gives the output back */
void dt_colorspaces_display_selection_changed(const dt_colorspaces_color_profile_type_t window);

/** the profile GTK's own surface in a window is shown in, if the platform
 * owns the window's output; otherwise FALSE, and that is the display
 * profile */
gboolean dt_colorspaces_get_gtk_output(const dt_colorspaces_color_profile_type_t window,
                                       dt_color_encoding_t *gtk);

/** for the presenter: the platform takes over a window's output, which
 * colorspaces resolves; NULL gives it back to the display profile
 * selection. TRUE if the output changed */
gboolean dt_colorspaces_set_window_output(const dt_colorspaces_color_profile_type_t window,
                                          const dt_display_output_t *output);

/** 8-bit RGBA to the screen's BGRA through one of the display transforms
 * in dt_colorspaces_t */
void dt_colorspaces_transform_to_display(cmsHTRANSFORM xform,
                                         const int hdr,
                                         const uint8_t *const in,
                                         uint8_t *const out,
                                         const int width,
                                         const int height,
                                         const size_t in_stride,
                                         const size_t out_stride);

/** Calculate CAM->XYZ, XYZ->CAM matrices **/
gboolean dt_colorspaces_conversion_matrices_xyz(const float adobe_XYZ_to_CAM[4][3],
                                           float in_XYZ_to_CAM[9],
                                           double XYZ_to_CAM[4][3],
                                           double CAM_to_XYZ[3][4]);

/** Calculate CAM->RGB, RGB->CAM matrices and default WB multipliers */
gboolean dt_colorspaces_conversion_matrices_rgb(const float adobe_XYZ_to_CAM[4][3],
                                           double RGB_to_CAM[4][3],
                                           double CAM_to_RGB[3][4],
                                           const float *embedded_matrix,
                                           double mul[4]);

/** Applies CYGM WB coeffs to an image that's already been converted
 * to RGB by dt_colorspaces_cygm_to_rgb */
void dt_colorspaces_cygm_apply_coeffs_to_rgb(float *out,
                                             const float *in,
                                             const int num,
                                             double RGB_to_CAM[4][3],
                                             double CAM_to_RGB[3][4],
                                             const dt_aligned_pixel_t coeffs);

/** convert CYGM buffer to RGB */
void dt_colorspaces_cygm_to_rgb(float *out,
                                const int num,
                                const double CAM_to_RGB[3][4]);

/** convert RGB buffer to CYGM */
void dt_colorspaces_rgb_to_cygm(float *out,
                                const int num,
                                const double RGB_to_CAM[4][3]);

gboolean dt_colorspaces_get_primaries_and_whitepoint_from_profile(cmsHPROFILE prof,
                                                                  float primaries[3][2],
                                                                  float whitepoint[2]);

/** Calculate RGB <-> XYZ matrices from the given primaries and whitepoint */
void dt_make_transposed_matrices_from_primaries_and_whitepoint(const float primaries[3][2],
                                                               const float whitepoint[2],
                                                               dt_colormatrix_t RGB_to_XYZ_transposed);

/** TRUE if the profile type has primaries wider than sRGB */
gboolean dt_colorspaces_profile_is_wide_gamut(const dt_colorspaces_color_profile_type_t type);

G_END_DECLS

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
