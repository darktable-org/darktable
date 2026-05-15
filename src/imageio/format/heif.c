/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

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

#include "bauhaus/bauhaus.h"
#include "common/darktable.h"
#include "common/exif.h"
#include "control/conf.h"
#include "imageio/imageio_common.h"
#include "imageio/imageio_module.h"
#include "imageio/format/imageio_format_api.h"

#include <glib/gstdio.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include <libheif/heif.h>

DT_MODULE(1)

typedef enum
{
  HEIF_LOSSLESS = 0,
  HEIF_LOSSY = 1
} compression_type_t;

typedef enum dt_imageio_heif_subsample_t
{
  DT_SUBSAMPLE_AUTO,
  DT_SUBSAMPLE_444,
  DT_SUBSAMPLE_422,
  DT_SUBSAMPLE_420
} dt_imageio_heif_subsample_t;


typedef struct dt_imageio_heif_t
{
  dt_imageio_module_data_t global;
  int bit_depth;
  int quality;
  int compression_type;
  dt_imageio_heif_subsample_t subsample;
} dt_imageio_heif_t;

typedef struct dt_imageio_heif_gui_t
{
  GtkWidget *bit_depth;
  GtkWidget *quality;
  GtkWidget *compression_type;
  GtkWidget *subsample;
} dt_imageio_heif_gui_t;

void init(dt_imageio_module_format_t *self)
{
  // We could add check if libheif we link against supports encoding and if not
  // heif export can be disabled with "self->ready = FALSE". See avif exporter
  // for example.
#ifdef USE_LUA
  luaA_struct(darktable.lua_state.state, dt_imageio_heif_t);

  dt_lua_register_module_member(darktable.lua_state.state,
                                self,
                                dt_imageio_heif_t,
                                bit_depth,
                                int);

  dt_lua_register_module_member(darktable.lua_state.state,
                                self,
                                dt_imageio_heif_t,
                                quality,
                                int);

  dt_lua_register_module_member(darktable.lua_state.state,
                                self,
                                dt_imageio_heif_t,
                                compression_type,
                                int);

  // subsample
  luaA_enum(darktable.lua_state.state, enum dt_imageio_heif_subsample_t);
  luaA_enum_value(darktable.lua_state.state,
                  enum dt_imageio_heif_subsample_t,
                  DT_SUBSAMPLE_AUTO);
  luaA_enum_value(darktable.lua_state.state,
                  enum dt_imageio_heif_subsample_t,
                  DT_SUBSAMPLE_444);
  luaA_enum_value(darktable.lua_state.state,
                  enum dt_imageio_heif_subsample_t,
                  DT_SUBSAMPLE_422);
  luaA_enum_value(darktable.lua_state.state,
                  enum dt_imageio_heif_subsample_t,
                  DT_SUBSAMPLE_420);
  dt_lua_register_module_member(darktable.lua_state.state,
                                self,
                                dt_imageio_heif_t,
                                subsample,
                                enum dt_imageio_heif_subsample_t);
#endif
}

void cleanup(dt_imageio_module_format_t *self)
{
}

int write_image(dt_imageio_module_data_t *data,
                const char *filename,
                const void *in_tmp, // ptr to input image buf
                dt_colorspaces_color_profile_type_t icc_type,
                const char *icc_filename,
                void *exif,
                int exif_len,
                dt_imgid_t imgid,
                int num,
                int total,
                struct dt_dev_pixelpipe_t *pipe,
                const gboolean export_masks)
{
  dt_imageio_heif_t *d = (dt_imageio_heif_t *)data;

  const size_t width = d->global.width;
  const size_t height = d->global.height;
  const int bit_depth = d->bit_depth;

  struct heif_image* image = NULL;
  struct heif_color_profile_nclx nclx_profile;
  struct heif_error err;
  uint8_t *icc_profile_data = NULL;

  const int subsample = dt_conf_get_int("plugins/imageio/format/heif/subsample");
  char* subsample_string = "";



  switch(bit_depth)
  {
    case 8:
      err = heif_image_create(width, height,
                              heif_colorspace_RGB,
                              heif_chroma_interleaved_RGB,
                              &image);
      break;
    case 10:
    case 12:
      err = heif_image_create(width, height,
                              heif_colorspace_RGB,
                              heif_chroma_interleaved_RRGGBB_LE,
                              &image);
      break;
    default:
      dt_print(DT_DEBUG_ALWAYS,
               "[heif export] Unsupported bit depth: %d",
               bit_depth);
      return 1; // failure

  } // end switch

  if(err.code != heif_error_Ok)
  {
    dt_print(DT_DEBUG_ALWAYS,
             "[heif export] Failed in heif_image_create for %s",
             filename);
    return 1; // failure
  }

  heif_image_add_plane(image, heif_channel_interleaved, width, height, bit_depth);

  int rowbytes;

  // Get a pointer to the actual pixel data.
  // The 'rowbytes' is returned as "bytes per line".
  // Returns NULL if a non-existing channel was given.
  uint8_t* pixels = heif_image_get_plane(image, heif_channel_interleaved, &rowbytes);

  dt_print(DT_DEBUG_ALWAYS, "[heif export] rowbytes = %d", rowbytes);

  const float max_channel_f = (float)((1 << bit_depth) - 1);
  const float *const restrict in_data = (const float *)in_tmp;
  uint8_t *const restrict out = (uint8_t *)pixels;

  dt_times_t start_time = { 0 }, end_time = { 0 };
  dt_get_times(&start_time);

  // this conversion loop is copied from avif exporter
  switch(bit_depth)
  {
    case 8:
    DT_OMP_FOR(collapse(2))
      for(size_t row = 0; row < height; row++)
      {
        for(size_t x = 0; x < width; x++)
        {
            const float *in_pixel = &in_data[(size_t)4 * ((row * width) + x)];
            uint8_t *out_pixel = (uint8_t *)&out[(row * rowbytes) + (3 * sizeof(uint8_t) * x)];

            out_pixel[0] = (uint8_t)roundf(CLAMP(in_pixel[0] * max_channel_f, 0, max_channel_f));
            out_pixel[1] = (uint8_t)roundf(CLAMP(in_pixel[1] * max_channel_f, 0, max_channel_f));
            out_pixel[2] = (uint8_t)roundf(CLAMP(in_pixel[2] * max_channel_f, 0, max_channel_f));
        }
      }
      break;
    case 10:
    case 12:
    DT_OMP_FOR(collapse(2))
      for(size_t row = 0; row < height; row++)
      {
        for(size_t x = 0; x < width; x++)
        {
            const float *in_pixel = &in_data[(size_t)4 * ((row * width) + x)];
            uint16_t *out_pixel = (uint16_t *)&out[(row * rowbytes) + (3 * sizeof(uint16_t) * x)];

            out_pixel[0] = (uint16_t)roundf(CLAMP(in_pixel[0] * max_channel_f, 0, max_channel_f));
            out_pixel[1] = (uint16_t)roundf(CLAMP(in_pixel[1] * max_channel_f, 0, max_channel_f));
            out_pixel[2] = (uint16_t)roundf(CLAMP(in_pixel[2] * max_channel_f, 0, max_channel_f));
        }
      }

  } // end switch

  dt_get_times(&end_time);
  const float tclock = end_time.clock - start_time.clock;
  const float uclock = end_time.user - start_time.user;
  dt_control_log("encoding tool %.4f (with uclock=%.4f)\n", tclock, uclock);


  // Determine the actual color profile used
  const dt_colorspaces_color_profile_t *cp = dt_colorspaces_get_output_profile(imgid, icc_type, icc_filename);
  if(!cp)
  {
    dt_control_log("color profile is null");
    return 1;
  }
  dt_control_log(
           "[heif colorprofile profile: %s]",
           dt_colorspaces_get_name(cp->type, filename));

  // !! matrix coefficient may be overriden later if save as RGB (not YUV)
  gboolean need_to_embed_icc = FALSE;
  switch(cp->type)
  {
  case DT_COLORSPACE_SRGB:
    nclx_profile.color_primaries = heif_color_primaries_ITU_R_BT_709_5; // 1
    nclx_profile.transfer_characteristics = heif_transfer_characteristic_IEC_61966_2_1; // 13
    nclx_profile.matrix_coefficients = heif_matrix_coefficients_ITU_R_BT_709_5; // 1
    break;
  case DT_COLORSPACE_REC709:
    nclx_profile.color_primaries = heif_color_primaries_ITU_R_BT_709_5; // 1
    nclx_profile.transfer_characteristics = heif_transfer_characteristic_ITU_R_BT_709_5; // 1
    nclx_profile.matrix_coefficients = heif_matrix_coefficients_ITU_R_BT_709_5; // 1
    break;
  case DT_COLORSPACE_LIN_REC709:
    nclx_profile.color_primaries = heif_color_primaries_ITU_R_BT_709_5; // 1
    nclx_profile.transfer_characteristics = heif_transfer_characteristic_linear; // 8
    nclx_profile.matrix_coefficients = heif_matrix_coefficients_ITU_R_BT_709_5; // 1
    break;
  case DT_COLORSPACE_LIN_REC2020:
    nclx_profile.color_primaries = heif_color_primaries_ITU_R_BT_2020_2_and_2100_0; // 9
    nclx_profile.transfer_characteristics = heif_transfer_characteristic_linear; // 8
    nclx_profile.matrix_coefficients = heif_matrix_coefficients_ITU_R_BT_2020_2_non_constant_luminance; // 9
    break;
  case DT_COLORSPACE_PQ_REC2020:
    nclx_profile.color_primaries = heif_color_primaries_ITU_R_BT_2020_2_and_2100_0; // 9
    nclx_profile.transfer_characteristics = heif_transfer_characteristic_ITU_R_BT_2100_0_PQ; // 16
    nclx_profile.matrix_coefficients = heif_matrix_coefficients_ITU_R_BT_2020_2_non_constant_luminance; // 9
    break;
  case DT_COLORSPACE_HLG_REC2020:
    nclx_profile.color_primaries = heif_color_primaries_ITU_R_BT_2020_2_and_2100_0; // 9
    nclx_profile.transfer_characteristics = heif_transfer_characteristic_ITU_R_BT_2100_0_HLG; // 18
    nclx_profile.matrix_coefficients = heif_matrix_coefficients_ITU_R_BT_2020_2_non_constant_luminance; // 9
    break;
  case DT_COLORSPACE_PQ_P3:
    nclx_profile.color_primaries = heif_color_primaries_SMPTE_EG_432_1; // 12
    nclx_profile.transfer_characteristics = heif_transfer_characteristic_ITU_R_BT_2100_0_PQ; // 16
    nclx_profile.matrix_coefficients = heif_matrix_coefficients_ITU_R_BT_2020_2_non_constant_luminance; // 9
    break;
  case DT_COLORSPACE_HLG_P3:
    nclx_profile.color_primaries = heif_color_primaries_SMPTE_EG_432_1; // 12
    nclx_profile.transfer_characteristics = heif_transfer_characteristic_ITU_R_BT_2100_0_HLG; // 18
    nclx_profile.matrix_coefficients = heif_matrix_coefficients_ITU_R_BT_601_6; // 6
    break;
  case DT_COLORSPACE_DISPLAY_P3:
   nclx_profile.color_primaries = heif_color_primaries_SMPTE_EG_432_1; // 12
   nclx_profile.transfer_characteristics = heif_transfer_characteristic_IEC_61966_2_1; // 13
   nclx_profile.matrix_coefficients = heif_matrix_coefficients_chromaticity_derived_non_constant_luminance; // 12
  default:
    need_to_embed_icc = TRUE;
    break;
  }

  if(!need_to_embed_icc)
    heif_image_set_nclx_color_profile(image, &nclx_profile);

  if(need_to_embed_icc)
  {
    dt_control_log("we will embed icc");
    uint32_t icc_profile_len;
    cmsSaveProfileToMem(cp->profile, NULL, &icc_profile_len);
    if(icc_profile_len > 0)
    {
      icc_profile_data = malloc(icc_profile_len);
      if(icc_profile_data == NULL)
      {
        dt_print(DT_DEBUG_ALWAYS,
                 "[heif export] failed to allocate ICC profile");
        // what we have to free?
        return 1; // failure
      }
      cmsSaveProfileToMem(cp->profile, icc_profile_data, &icc_profile_len);
      heif_image_set_raw_color_profile(image, "prof", icc_profile_data, icc_profile_len);
    }
  }


  // temp code to avoid variable not used error
  if(need_to_embed_icc)
    dt_control_log("icc profile will be embedded into heif file");

  struct heif_context* context = heif_context_alloc();

  struct heif_encoder* encoder;
  // we will use HEVC compression as the most commonly supported
  err = heif_context_get_encoder_for_format(context, heif_compression_HEVC, &encoder);
  if (err.code != heif_error_Ok)
  {
    dt_print(DT_DEBUG_ALWAYS,
             "[heif export] error getting HEVC encoder: %s (%d)",
             err.message, err.code);
    heif_context_free(context);
    heif_image_release(image);
    return 1; // failure
  }

  switch(subsample)
  {
    case DT_SUBSAMPLE_AUTO:
      // these thresholds and their description in the
      // subsampling widget tooltip must match each other
      if(d->quality > 90)
        subsample_string = "444";
      else if(d->quality > 80)
        subsample_string = "422";
      else
        subsample_string = "420";
      break;
    case DT_SUBSAMPLE_444:
      subsample_string = "444";
      break;
    case DT_SUBSAMPLE_422:
      subsample_string = "422";
      break;
    case DT_SUBSAMPLE_420:
      subsample_string = "420";
      break;
  }
  err = heif_encoder_set_parameter_string(encoder, "chroma", subsample_string);

  if (err.code != heif_error_Ok)
  {
    dt_print(DT_DEBUG_ALWAYS,
             "[heif export] failed to set chrome subsampling %s: %s",
             subsample_string, err.message);
    heif_encoder_release(encoder);
    heif_context_free(context);
    heif_image_release(image);
    return 1; // failure
  }


  // not needed if we already set lossy quality
  // heif_encoder_set_lossless(encoder, FALSE);
  if(d->compression_type == HEIF_LOSSLESS)
  {
    // When choosing lossless, the user's intention is obviously to preserve
    // all image information as much as the format allows.
    // In the case of HEIF, it is not enough to simply switch the encoder to
    // lossless mode. It is also necessary to disable color subsampling and
    // ensure RGB recording so that information is not lost during
    // the conversion to YCbCr.
    heif_encoder_set_lossless(encoder, TRUE);
    nclx_profile.matrix_coefficients = heif_matrix_coefficients_RGB_GBR;
    nclx_profile.version = 1;
    heif_image_set_nclx_color_profile(image, &nclx_profile);
    heif_encoder_set_parameter_string(encoder, "chroma", "444");
  }
  else
  {
    heif_encoder_set_lossless(encoder, FALSE);
    heif_encoder_set_lossy_quality(encoder, d->quality);
  }

  heif_encoder_set_logging_level(encoder, 0);

  struct heif_encoding_options* options = heif_encoding_options_alloc();
#ifdef HAVE_LIBSHARPYUV
  options->color_conversion_options.preferred_chroma_downsampling_algorithm = heif_chroma_downsampling_sharp_yuv;
  options->color_conversion_options.only_use_preferred_chroma_algorithm = false;
#endif

  struct heif_image_handle* handle;
  err = heif_context_encode_image(context,
                                  image,
                                  encoder,
                                  options,    // options
                                  &handle);

  if (err.code != heif_error_Ok)
  {
    dt_print(DT_DEBUG_ALWAYS,
             "[heif export] error encoding image: %s (%d)",
             err.message, err.code);
    heif_encoder_release(encoder);
    heif_context_free(context);
    heif_image_release(image);
    return 1; // failure
  }

  // Workaround; remove when exiv2 implements HEIF write support and use dt_exif_write_blob() at the end
  if(exif && exif_len > 0)
  {
    err = heif_context_add_exif_metadata(context, handle, exif, exif_len);
    if(err.code != heif_error_Ok)
    {
     dt_print(DT_DEBUG_ALWAYS,
              "[heif export] failed to save EXIF metadata: %s (%d)",
              err.message, err.code);
     return 1; // failure
    }
  }

  if(exif && exif_len > 0)
  {
    char *xmp_string = dt_exif_xmp_read_string(imgid);
    const size_t xmp_len = strlen(xmp_string);
    if(xmp_string && (xmp_len > 0))
    {
      err = heif_context_add_XMP_metadata(context, handle, xmp_string, xmp_len);
      g_free(xmp_string);
      if(err.code != heif_error_Ok)
      {
         dt_print(DT_DEBUG_ALWAYS,
                  "[heif export] failed to save XMP metadata: %s (%d)",
                  err.message, err.code);
         return 1; // failure
      }
    }
  }

  heif_image_handle_release(handle);

  err = heif_context_write_to_file(context, filename);
  if (err.code != heif_error_Ok) {
    dt_print(DT_DEBUG_ALWAYS,
             "[heif export] error writing image to file: %s (%d)",
             err.message, err.code);
    heif_encoder_release(encoder);
    heif_context_free(context);
    heif_image_release(image);
    return 1; // failure
  }

  heif_context_free(context);
  heif_image_release(image);
  heif_encoder_release(encoder);

  return 0; // success
}

size_t params_size(dt_imageio_module_format_t *self)
{
  return sizeof(dt_imageio_heif_t);
}

void *get_params(dt_imageio_module_format_t *self)
{
  dt_imageio_heif_t *d = calloc(1, sizeof(dt_imageio_heif_t));
  if(d == NULL)
    return NULL;

  d->compression_type = dt_conf_get_int("plugins/imageio/format/heif/compression_type");
  d->bit_depth = dt_conf_get_int("plugins/imageio/format/heif/bitdepth");
  d->subsample = dt_conf_get_int("plugins/imageio/format/heif/subsample");

  switch(d->compression_type)
  {
    case HEIF_LOSSLESS:
      d->quality = 100;
      break;
    case HEIF_LOSSY:
      d->quality = dt_conf_get_int("plugins/imageio/format/heif/quality");
      break;
  }

  return d;
}

void free_params(dt_imageio_module_format_t *self, dt_imageio_module_data_t *params)
{
  free(params);
}

int set_params(dt_imageio_module_format_t *self, const void *params, const int size)
{
  if(size != self->params_size(self)) return 1;

  const dt_imageio_heif_t *d = (dt_imageio_heif_t *)params;
  dt_imageio_heif_gui_t *g = self->gui_data;
  dt_bauhaus_combobox_set_from_value(g->bit_depth, d->bit_depth);
  dt_bauhaus_combobox_set(g->compression_type, d->compression_type);
  dt_bauhaus_slider_set(g->quality, d->quality);
  dt_bauhaus_combobox_set(g->subsample, d->subsample);
  return 0;
}

int bpp(dt_imageio_module_data_t *p)
{
  // 32 means we always request untranslated internal darktable pixel format
  // of 4 floats (RGB + padding to 16 bytes) and will change pixel format to
  // what encoder expects as input.
  // Another option is to return a value that corresponds to the user's choice
  // for encoding. Then we will have a buffer in the input format already
  // prepared for encoding. For example, if we return 8 here, the input pixel
  // buffer will be in the format of 8-bit integer values.
  return 32;
}

int levels(dt_imageio_module_data_t *p)
{
  const dt_imageio_heif_t *d = (dt_imageio_heif_t *)p;
  int ret = IMAGEIO_RGB;

  if(d->bit_depth == 8)
    ret |= IMAGEIO_INT8;
  else if(d->bit_depth == 10)
    ret |= IMAGEIO_INT10;
  else
    ret |= IMAGEIO_INT12;

  return ret;
}

const char *mime(dt_imageio_module_data_t *data)
{
  return "image/heif";
}

const char *extension(dt_imageio_module_data_t *data)
{
  // "heif" indicates the content may have been encoded by any codec.
  // "heic" indicates the content was encoded using the HEVC codec.
  // If we are not going to use other codecs, we can choose "heic" for
  // better compatibility with Apple devices.
  // If we add the ability to select the AV1 codec in this module, it would
  // actually mean the ability to export to AVIF format (which uses the same
  // container but the AV1 codec), then we would need to choose the extension
  // here depending on the codec selected by the user.
  // For AV1, the extension should be "avif".
  return "heic";
}

const char *name()
{
  return _("HEIF");
}

static inline int _bit_depth_to_pos(int bit_depth)
{
  switch(bit_depth)
  {
    case 12:
      return 2;
    case 10:
      return 1;
    case 8:
      return 0;
    default:
      break;
  }
  return 0; // 8 bpc
}

static void bit_depth_changed(GtkWidget *widget, gpointer user_data)
{
  const int bit_depth_pos = (int)dt_bauhaus_combobox_get(widget);
  int bit_depth;
  switch(bit_depth_pos)
  {
    case 0:
      bit_depth = 8;
      break;
    case 1:
      bit_depth = 10;
      break;
    case 2:
      bit_depth = 12;
      break;
    default:
      bit_depth = 8;
      break;
  }
  dt_conf_set_int("plugins/imageio/format/heif/bitdepth", bit_depth);
}

static void quality_changed(GtkWidget *slider, gpointer user_data)
{
  const int quality = (int)dt_bauhaus_slider_get(slider);
  dt_conf_set_int("plugins/imageio/format/heif/quality", quality);
}

static void compression_type_changed(GtkWidget *widget, gpointer user_data)
{
  const int compression_type = dt_bauhaus_combobox_get(widget);
  dt_imageio_module_format_t *module = (dt_imageio_module_format_t *)user_data;
  dt_imageio_heif_gui_t *gui = module->gui_data;

  dt_conf_set_int("plugins/imageio/format/heif/compression_type", compression_type);

  // lossless mode is incompatible with reduced encoding quality or data loss
  // due to subsampling, so we hide these controls
  gtk_widget_set_visible(gui->quality, compression_type != HEIF_LOSSLESS);
  gtk_widget_set_visible(gui->subsample, compression_type != HEIF_LOSSLESS);
}

static void subsample_combobox_changed(GtkWidget *widget,
                                       gpointer user_data)
{
  const dt_imageio_heif_subsample_t subsample = dt_bauhaus_combobox_get(widget);
  dt_conf_set_int("plugins/imageio/format/heif/subsample", subsample);
}

void gui_init(dt_imageio_module_format_t *self)
{
  dt_imageio_heif_gui_t *gui = malloc(sizeof(dt_imageio_heif_gui_t));

  const int quality =
    dt_conf_get_int("plugins/imageio/format/heif/quality");

  const int compression_type =
    dt_conf_get_int("plugins/imageio/format/heif/compression_type");

  const int bit_depth =
    dt_conf_get_int("plugins/imageio/format/heif/bitdepth");

  const int bit_depth_pos = _bit_depth_to_pos(bit_depth);

  const dt_imageio_heif_subsample_t subsample =
    dt_conf_get_int("plugins/imageio/format/heif/subsample");

  self->gui_data = (void *)gui;

  DT_BAUHAUS_COMBOBOX_NEW_FULL(gui->bit_depth, self, NULL, N_("bit depth"),
                               NULL, bit_depth_pos, bit_depth_changed, self,
                               N_("8 bit"), N_("10 bit"), N_("12 bit"));

  DT_BAUHAUS_COMBOBOX_NEW_FULL(gui->compression_type, self, NULL, N_("compression"), NULL,
                               compression_type, compression_type_changed, self,
                               N_("lossless"), N_("lossy"));


  // Quality slider
  gui->quality =
    dt_bauhaus_slider_new_with_range
      ((dt_iop_module_t*)self,
       dt_confgen_get_int("plugins/imageio/format/heif/quality", DT_MIN), // min
       dt_confgen_get_int("plugins/imageio/format/heif/quality", DT_MAX), // max
       1,  // step
       dt_confgen_get_int("plugins/imageio/format/heif/quality", DT_DEFAULT), // default
       0); // digits

  dt_bauhaus_widget_set_label(gui->quality,  NULL, N_("quality"));

  gtk_widget_set_tooltip_text(gui->quality,
          _("the quality of an image, less quality means fewer details"));

  dt_bauhaus_slider_set(gui->quality, quality);

  g_signal_connect(G_OBJECT(gui->quality),
                   "value-changed",
                   G_CALLBACK(quality_changed),
                   NULL);

  DT_BAUHAUS_COMBOBOX_NEW_FULL
    (gui->subsample,
     self,
     NULL,
     N_("chroma subsampling"),
     _("chroma subsampling setting for HEIF encoder.\n"
       "auto - use subsampling determined by the quality value\n"
       "4:4:4 - no chroma subsampling\n"
       "4:2:2 - color sampling rate halved horizontally\n"
       "4:2:0 - color sampling rate halved horizontally and vertically"),
     subsample,
     subsample_combobox_changed,
     self,
     N_("auto"), N_("4:4:4"), N_("4:2:2"), N_("4:2:0"));

  // control conditional visibility
  gtk_widget_set_visible(gui->quality, compression_type != HEIF_LOSSLESS);
  gtk_widget_set_visible(gui->subsample, compression_type != HEIF_LOSSLESS);
  gtk_widget_set_no_show_all(gui->quality, TRUE);
  gtk_widget_set_no_show_all(gui->subsample, TRUE);

  self->widget = dt_gui_vbox(gui->bit_depth,
                             gui->compression_type,
                             gui->quality,
                             gui->subsample);
}

void gui_cleanup(dt_imageio_module_format_t *self)
{
  free(self->gui_data);
}

void gui_reset(dt_imageio_module_format_t *self)
{
  dt_imageio_heif_gui_t *gui = self->gui_data;

  const int bit_depth =
    dt_confgen_get_int("plugins/imageio/format/heif/bitdepth", DT_DEFAULT);
  const int quality =
    dt_confgen_get_int("plugins/imageio/format/heif/quality", DT_DEFAULT);
  const int compression_type =
    dt_confgen_get_int("plugins/imageio/format/heif/compression_type", DT_DEFAULT);

  dt_bauhaus_slider_set(gui->quality, quality);
  dt_bauhaus_combobox_set(gui->compression_type, compression_type);
  dt_bauhaus_combobox_set(gui->bit_depth, _bit_depth_to_pos(bit_depth));
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
