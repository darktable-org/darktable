/*
    This file is part of darktable,
    Copyright (C) 2019-2025 darktable developers.

    Copyright (c) 2019      Andreas Schneider

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

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "bauhaus/bauhaus.h"
#include "common/colorspaces.h"
#include "common/darktable.h"
#include "control/conf.h"
#include "imageio/imageio_common.h"
#include "imageio/imageio_module.h"
#include "imageio/format/imageio_format_api.h"

#include <ultrahdr_api.h>

DT_MODULE(1)

/*
 * Gain-map resolution choices. The value is the downscale factor passed to
 * uhdr_enc_set_gainmap_scale_factor(): 1 = full resolution, 2 = half, 4 = quarter.
 */
enum ultrahdr_gainmap_downscale_e
{
  ULTRAHDR_GAINMAP_FULL = 0,
  ULTRAHDR_GAINMAP_HALF,
  ULTRAHDR_GAINMAP_QUARTER,
};

static const int ultrahdr_gainmap_scale[] = { 1, 2, 4 };

/* Module defaults, also declared in data/darktableconfig.xml.in. Used for the
 * slider construction default and the gui_reset() values; the runtime conf
 * keys are read with dt_conf_get_int() (the xml supplies the stored default). */
#define ULTRAHDR_DEFAULT_QUALITY 95
#define ULTRAHDR_DEFAULT_GAINMAP_QUALITY 95

typedef struct dt_imageio_ultrahdr_t
{
  dt_imageio_module_data_t global;
  int bpp;
  int quality;
  int gainmap_quality;
  int gainmap_downscale;
} dt_imageio_ultrahdr_t;

typedef struct dt_imageio_ultrahdr_gui_t
{
  GtkWidget *quality;
  GtkWidget *gainmap_quality;
  GtkWidget *gainmap_downscale;
} dt_imageio_ultrahdr_gui_t;

void init(dt_imageio_module_format_t *self)
{
#ifdef USE_LUA
  dt_lua_register_module_member(darktable.lua_state.state,
                                self,
                                dt_imageio_ultrahdr_t,
                                quality,
                                int);
  dt_lua_register_module_member(darktable.lua_state.state,
                                self,
                                dt_imageio_ultrahdr_t,
                                gainmap_quality,
                                int);
  dt_lua_register_module_member(darktable.lua_state.state,
                                self,
                                dt_imageio_ultrahdr_t,
                                gainmap_downscale,
                                int);
#endif
}

void cleanup(dt_imageio_module_format_t *self)
{
}

int write_image(struct dt_imageio_module_data_t *data,
                const char *filename,
                const void *in,
                dt_colorspaces_color_profile_type_t over_type,
                const char *over_filename,
                void *exif,
                int exif_len,
                dt_imgid_t imgid,
                int num,
                int total,
                struct dt_dev_pixelpipe_t *pipe,
                const gboolean export_masks)
{
  dt_imageio_ultrahdr_t *d = (dt_imageio_ultrahdr_t *)data;

  uhdr_codec_private_t *enc = NULL;
  uint32_t *packed = NULL;
  FILE *f = NULL;
  int rc = 1;

  const size_t width = d->global.width;
  const size_t height = d->global.height;

  /*
   * Determine the actual (export vs colorout) color profile used. UltraHDR
   * requires a PQ (SMPTE ST 2084) output profile: colorout has already baked
   * the PQ curve into the float samples, so the buffer is PQ-encoded, in the
   * chosen primaries (Rec2020 or P3). We pass it to libultrahdr as UHDR_CT_PQ.
   */
  const dt_colorspaces_color_profile_t *cp =
    dt_colorspaces_get_output_profile(imgid, over_type, over_filename);

  uhdr_color_gamut_t cg;
  switch(cp->type)
  {
    case DT_COLORSPACE_PQ_REC2020:
      cg = UHDR_CG_BT_2100;
      break;
    case DT_COLORSPACE_PQ_P3:
      cg = UHDR_CG_DISPLAY_P3;
      break;
    default:
      dt_print(DT_DEBUG_IMAGEIO,
               "UltraHDR export requires a PQ output color profile "
               "(PQ Rec2020 or PQ P3); got %s",
               dt_colorspaces_get_name(cp->type, filename));
      return 1;
  }

  dt_print(DT_DEBUG_IMAGEIO,
           "Exporting UltraHDR image [%s] "
           "[width: %zu, height: %zu, quality: %d, gain map quality: %d, "
           "gain map scale: 1/%d]",
           filename,
           width,
           height,
           d->quality,
           d->gainmap_quality,
           ultrahdr_gainmap_scale[d->gainmap_downscale]);

  /*
   * Pack the interleaved float32 RGBA buffer into RGBA1010102.
   *
   * The float samples are PQ-encoded and normalized to [0, 1]. We quantize each
   * of R, G, B to 10 bits and pack little-endian per the libultrahdr header:
   * Red 9:0, Green 19:10, Blue 29:20, Alpha 31:30 (alpha is opaque, 0x3).
   */
  packed = dt_alloc_align_type(uint32_t, width * height);
  if(packed == NULL)
  {
    dt_print(DT_DEBUG_IMAGEIO, "Failed to allocate UltraHDR pixel buffer");
    goto out;
  }

  const float *const restrict in_data = (const float *)in;

  DT_OMP_FOR_SIMD()
  for(size_t k = 0; k < width * height; k++)
  {
    const float *const restrict in_pixel = &in_data[4 * k];

    const uint32_t r = (uint32_t)CLAMP(lroundf(CLAMP(in_pixel[0], 0.0f, 1.0f) * 1023.0f), 0, 1023);
    const uint32_t g = (uint32_t)CLAMP(lroundf(CLAMP(in_pixel[1], 0.0f, 1.0f) * 1023.0f), 0, 1023);
    const uint32_t b = (uint32_t)CLAMP(lroundf(CLAMP(in_pixel[2], 0.0f, 1.0f) * 1023.0f), 0, 1023);

    packed[k] = r | (g << 10) | (b << 20) | (0x3u << 30);
  }

  uhdr_raw_image_t img = { 0 };
  img.fmt = UHDR_IMG_FMT_32bppRGBA1010102;
  img.cg = cg;
  img.ct = UHDR_CT_PQ;
  img.range = UHDR_CR_FULL_RANGE;
  img.w = (unsigned int)width;
  img.h = (unsigned int)height;
  img.planes[UHDR_PLANE_PACKED] = packed;
  img.stride[UHDR_PLANE_PACKED] = (unsigned int)width; /* stride in pixels */

  enc = uhdr_create_encoder();
  if(enc == NULL)
  {
    dt_print(DT_DEBUG_IMAGEIO, "uhdr_create_encoder failed");
    goto out;
  }

  uhdr_error_info_t e;

  /*
   * API-0 single-input encode: feed only the HDR rendition. libultrahdr
   * internally tone-maps to an SDR base, computes the ISO 21496-1 gain map,
   * and muxes the base JPEG + gain map + MPF container. No SDR input needed.
   */
  e = uhdr_enc_set_raw_image(enc, &img, UHDR_HDR_IMG);
  if(e.error_code != UHDR_CODEC_OK)
  {
    dt_print(DT_DEBUG_IMAGEIO, "uhdr_enc_set_raw_image failed: %s", e.detail);
    goto out;
  }

  e = uhdr_enc_set_quality(enc, d->quality, UHDR_BASE_IMG);
  if(e.error_code != UHDR_CODEC_OK)
  {
    dt_print(DT_DEBUG_IMAGEIO, "uhdr_enc_set_quality (base) failed: %s", e.detail);
    goto out;
  }

  e = uhdr_enc_set_quality(enc, d->gainmap_quality, UHDR_GAIN_MAP_IMG);
  if(e.error_code != UHDR_CODEC_OK)
  {
    dt_print(DT_DEBUG_IMAGEIO, "uhdr_enc_set_quality (gain map) failed: %s", e.detail);
    goto out;
  }

  e = uhdr_enc_set_gainmap_scale_factor(enc, ultrahdr_gainmap_scale[d->gainmap_downscale]);
  if(e.error_code != UHDR_CODEC_OK)
  {
    dt_print(DT_DEBUG_IMAGEIO, "uhdr_enc_set_gainmap_scale_factor failed: %s", e.detail);
    goto out;
  }

  e = uhdr_enc_set_output_format(enc, UHDR_CODEC_JPG);
  if(e.error_code != UHDR_CODEC_OK)
  {
    dt_print(DT_DEBUG_IMAGEIO, "uhdr_enc_set_output_format failed: %s", e.detail);
    goto out;
  }

  e = uhdr_encode(enc);
  if(e.error_code != UHDR_CODEC_OK)
  {
    dt_print(DT_DEBUG_IMAGEIO, "uhdr_encode failed: %s", e.detail);
    goto out;
  }

  uhdr_compressed_image_t *out = uhdr_get_encoded_stream(enc);
  if(out == NULL || out->data == NULL || out->data_sz == 0)
  {
    dt_print(DT_DEBUG_IMAGEIO, "uhdr_get_encoded_stream returned empty data");
    goto out;
  }

  /*
   * Write the encoder-owned buffer to disk before releasing the encoder.
   */
  f = g_fopen(filename, "wb");
  if(f == NULL)
  {
    dt_print(DT_DEBUG_IMAGEIO, "Failed to open `%s' for writing", filename);
    goto out;
  }

  const size_t cnt = fwrite(out->data, 1, out->data_sz, f);
  fclose(f);
  f = NULL;

  if(cnt != out->data_sz)
  {
    dt_print(DT_DEBUG_IMAGEIO, "Failed to write UltraHDR image `%s'", filename);
    g_unlink(filename);
    goto out;
  }

  /*
   * TODO: embed EXIF/XMP. libultrahdr offers uhdr_enc_set_exif_data(), but it
   * must be set before uhdr_encode() and darktable's exif blob would need
   * conversion; left out for v1 (flags() returns 0). No metadata is written.
   */

  rc = 0; /* success */

out:
  if(f != NULL)
    fclose(f);
  if(enc != NULL)
    uhdr_release_encoder(enc);
  dt_free_align(packed);

  return rc;
}

size_t params_size(dt_imageio_module_format_t *self)
{
  return sizeof(dt_imageio_ultrahdr_t);
}

void *get_params(dt_imageio_module_format_t *self)
{
  dt_imageio_ultrahdr_t *d = calloc(1, sizeof(dt_imageio_ultrahdr_t));

  if(d == NULL)
    return NULL;

  d->bpp = 32;
  d->quality = dt_conf_get_int("plugins/imageio/format/ultrahdr/quality");
  d->gainmap_quality = dt_conf_get_int("plugins/imageio/format/ultrahdr/gainmap_quality");
  d->gainmap_downscale = dt_conf_get_int("plugins/imageio/format/ultrahdr/gainmap_downscale");

  if(d->gainmap_downscale < 0 || d->gainmap_downscale > ULTRAHDR_GAINMAP_QUARTER)
    d->gainmap_downscale = ULTRAHDR_GAINMAP_FULL;

  return d;
}

int set_params(dt_imageio_module_format_t *self,
               const void *params,
               const int size)
{
  if(size != self->params_size(self))
    return 1;
  const dt_imageio_ultrahdr_t *d = (dt_imageio_ultrahdr_t *)params;

  dt_imageio_ultrahdr_gui_t *g = self->gui_data;
  dt_bauhaus_slider_set(g->quality, d->quality);
  dt_bauhaus_slider_set(g->gainmap_quality, d->gainmap_quality);
  dt_bauhaus_combobox_set(g->gainmap_downscale, d->gainmap_downscale);

  return 0;
}

void free_params(dt_imageio_module_format_t *self,
                 dt_imageio_module_data_t *params)
{
  free(params);
}

int bpp(struct dt_imageio_module_data_t *data)
{
  return 32; /* always request float */
}

int levels(struct dt_imageio_module_data_t *data)
{
  /* PQ-encoded float buffer packed to 10-bit RGBA */
  return IMAGEIO_RGB | IMAGEIO_INT10;
}

const char *mime(dt_imageio_module_data_t *data)
{
  return "image/jpeg";
}

const char *extension(dt_imageio_module_data_t *data)
{
  return "jpg";
}

const char *name()
{
  return _("UltraHDR JPEG");
}

int flags(struct dt_imageio_module_data_t *data)
{
  /* No XMP/EXIF embedding for v1, see TODO in write_image(). */
  return 0;
}

int dimension(struct dt_imageio_module_format_t *self,
              struct dt_imageio_module_data_t *data,
              uint32_t *width,
              uint32_t *height)
{
  /* No hard limit imposed by the module. */
  return 0;
}

static void quality_changed(GtkWidget *slider, gpointer user_data)
{
  const int quality = (int)dt_bauhaus_slider_get(slider);
  dt_conf_set_int("plugins/imageio/format/ultrahdr/quality", quality);
}

static void gainmap_quality_changed(GtkWidget *slider, gpointer user_data)
{
  const int quality = (int)dt_bauhaus_slider_get(slider);
  dt_conf_set_int("plugins/imageio/format/ultrahdr/gainmap_quality", quality);
}

static void gainmap_downscale_changed(GtkWidget *widget, gpointer user_data)
{
  const int idx = dt_bauhaus_combobox_get(widget);
  dt_conf_set_int("plugins/imageio/format/ultrahdr/gainmap_downscale", idx);
}

void gui_init(dt_imageio_module_format_t *self)
{
  dt_imageio_ultrahdr_gui_t *gui = malloc(sizeof(dt_imageio_ultrahdr_gui_t));
  self->gui_data = (void *)gui;

  const int quality = dt_conf_get_int("plugins/imageio/format/ultrahdr/quality");
  const int gainmap_quality = dt_conf_get_int("plugins/imageio/format/ultrahdr/gainmap_quality");
  const int gainmap_downscale = dt_conf_get_int("plugins/imageio/format/ultrahdr/gainmap_downscale");

  /*
   * Base image quality slider
   */
  gui->quality = dt_bauhaus_slider_new_with_range((dt_iop_module_t *)self,
                                                  0,   /* min */
                                                  100, /* max */
                                                  1,   /* step */
                                                  ULTRAHDR_DEFAULT_QUALITY, /* default */
                                                  0);  /* digits */
  dt_bauhaus_widget_set_label(gui->quality, NULL, N_("quality"));
  gtk_widget_set_tooltip_text(gui->quality,
          _("the quality of the SDR base image, less quality means fewer details"));
  dt_bauhaus_slider_set(gui->quality, quality);

  /*
   * Gain map quality slider
   */
  gui->gainmap_quality = dt_bauhaus_slider_new_with_range((dt_iop_module_t *)self,
                                                          0,   /* min */
                                                          100, /* max */
                                                          1,   /* step */
                                                          ULTRAHDR_DEFAULT_GAINMAP_QUALITY, /* default */
                                                          0);  /* digits */
  dt_bauhaus_widget_set_label(gui->gainmap_quality, NULL, N_("gain map quality"));
  gtk_widget_set_tooltip_text(gui->gainmap_quality,
          _("the quality of the HDR gain map, less quality means a coarser gain map"));
  dt_bauhaus_slider_set(gui->gainmap_quality, gainmap_quality);

  /*
   * Gain map resolution combo box
   */
  DT_BAUHAUS_COMBOBOX_NEW_FULL(gui->gainmap_downscale, self, NULL, N_("gain map resolution"),
                               _("resolution of the HDR gain map relative to the image.\n\n"
                                 "full - same resolution as the image\n"
                                 "half - half resolution (smaller file)\n"
                                 "quarter - quarter resolution (smallest file)"),
                               gainmap_downscale, gainmap_downscale_changed, self,
                               N_("full"), N_("half"), N_("quarter"));

  g_signal_connect(G_OBJECT(gui->quality),
                   "value-changed",
                   G_CALLBACK(quality_changed),
                   NULL);
  g_signal_connect(G_OBJECT(gui->gainmap_quality),
                   "value-changed",
                   G_CALLBACK(gainmap_quality_changed),
                   NULL);

  self->widget = dt_gui_vbox(gui->quality, gui->gainmap_quality, gui->gainmap_downscale);
}

void gui_cleanup(dt_imageio_module_format_t *self)
{
  free(self->gui_data);
}

void gui_reset(dt_imageio_module_format_t *self)
{
  dt_imageio_ultrahdr_gui_t *gui = self->gui_data;

  const int quality = ULTRAHDR_DEFAULT_QUALITY;
  const int gainmap_quality = ULTRAHDR_DEFAULT_GAINMAP_QUALITY;
  const int gainmap_downscale = ULTRAHDR_GAINMAP_FULL;

  dt_bauhaus_slider_set(gui->quality, quality);
  dt_bauhaus_slider_set(gui->gainmap_quality, gainmap_quality);
  dt_bauhaus_combobox_set(gui->gainmap_downscale, gainmap_downscale);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
