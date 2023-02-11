/*
    This file is part of darktable,
    Copyright (C) 2012-2023 darktable developers.

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

// Original copyright notice from image_to_j2k.c from openjpeg:
/*
 * Copyright (c) 2002-2007, Communications and Remote Sensing Laboratory, Universite catholique de Louvain
 *(UCL), Belgium
 * Copyright (c) 2002-2007, Professor Benoit Macq
 * Copyright (c) 2001-2003, David Janssens
 * Copyright (c) 2002-2003, Yannick Verschueren
 * Copyright (c) 2003-2007, Francois-Olivier Devaux and Antonin Descampe
 * Copyright (c) 2005, Herve Drolon, FreeImage Team
 * Copyright (c) 2006-2007, Parvatha Elangovan
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS `AS IS'
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "bauhaus/bauhaus.h"
#include "common/darktable.h"
#include "common/exif.h"
#include "control/conf.h"
#include "imageio/imageio_common.h"
#include "imageio/imageio_module.h"
#include "imageio/format/imageio_format_api.h"
#include <stdio.h>
#include <stdlib.h>

#include <openjpeg.h>

typedef enum
{
  J2K_CFMT = 0,
  JP2_CFMT = 1
} dt_imageio_j2k_format_t;

// borrowed from blender
#define DOWNSAMPLE_FLOAT_TO_8BIT(_val) (_val) <= 0.0f ? 0 : ((_val) >= 1.0f ? 255 : (int)roundf(255.0f * (_val)))
#define DOWNSAMPLE_FLOAT_TO_12BIT(_val) (_val) <= 0.0f ? 0 : ((_val) >= 1.0f ? 4095 : (int)roundf(4095.0f * (_val)))
#define DOWNSAMPLE_FLOAT_TO_16BIT(_val)                                                                      \
  (_val) <= 0.0f ? 0 : ((_val) >= 1.0f ? 65535 : (int)roundf(65535.0f * (_val)))

DT_MODULE(2)

typedef enum
{
  DT_J2K_PRESET_OFF,
  DT_J2K_PRESET_CINEMA2K_24,
  DT_J2K_PRESET_CINEMA2K_48,
  DT_J2K_PRESET_CINEMA4K_24
} dt_imageio_j2k_preset_t;

typedef struct dt_imageio_j2k_t
{
  dt_imageio_module_data_t global;
  int bpp;
  dt_imageio_j2k_format_t format;
  dt_imageio_j2k_preset_t preset;
  int quality;
} dt_imageio_j2k_t;

typedef struct dt_imageio_j2k_gui_t
{
  GtkWidget *format;
  GtkWidget *preset;
  GtkWidget *quality;
} dt_imageio_j2k_gui_t;

void init(dt_imageio_module_format_t *self)
{
#ifdef USE_LUA
  dt_lua_register_module_member(darktable.lua_state.state, self, dt_imageio_j2k_t, bpp, int);
  luaA_enum(darktable.lua_state.state, dt_imageio_j2k_format_t);
  luaA_enum_value_name(darktable.lua_state.state, dt_imageio_j2k_format_t, J2K_CFMT, "j2k");
  luaA_enum_value_name(darktable.lua_state.state, dt_imageio_j2k_format_t, JP2_CFMT, "jp2");
  dt_lua_register_module_member(darktable.lua_state.state, self, dt_imageio_j2k_t, format,
                                dt_imageio_j2k_format_t);
  dt_lua_register_module_member(darktable.lua_state.state, self, dt_imageio_j2k_t, quality, int);
  luaA_enum(darktable.lua_state.state, dt_imageio_j2k_preset_t);
  luaA_enum_value_name(darktable.lua_state.state, dt_imageio_j2k_preset_t, DT_J2K_PRESET_OFF, "off");
  luaA_enum_value_name(darktable.lua_state.state, dt_imageio_j2k_preset_t, DT_J2K_PRESET_CINEMA2K_24,
                       "cinema2k_24");
  luaA_enum_value_name(darktable.lua_state.state, dt_imageio_j2k_preset_t, DT_J2K_PRESET_CINEMA2K_48,
                       "cinema2k_48");
  luaA_enum_value_name(darktable.lua_state.state, dt_imageio_j2k_preset_t, DT_J2K_PRESET_CINEMA4K_24,
                       "cinema4k_24");
  dt_lua_register_module_member(darktable.lua_state.state, self, dt_imageio_j2k_t, preset,
                                dt_imageio_j2k_preset_t);
#endif
}
void cleanup(dt_imageio_module_format_t *self)
{
}

/**
sample error callback expecting a FILE* client object
*/
static void error_callback(const char *msg, void *client_data)
{
  FILE *stream = (FILE *)client_data;
  fprintf(stream, "[ERROR] %s", msg);
}
/**
sample warning callback expecting a FILE* client object
*/
static void warning_callback(const char *msg, void *client_data)
{
  FILE *stream = (FILE *)client_data;
  fprintf(stream, "[WARNING] %s", msg);
}
/**
sample debug callback expecting a FILE* client object
*/
static void info_callback(const char *msg, void *client_data)
{
  FILE *stream = (FILE *)client_data;
  fprintf(stream, "[INFO] %s", msg);
}

static int initialise_4K_poc(opj_poc_t *POC, int numres)
{
  POC[0].tile = 1;
  POC[0].resno0 = 0;
  POC[0].compno0 = 0;
  POC[0].layno1 = 1;
  POC[0].resno1 = numres - 1;
  POC[0].compno1 = 3;
  POC[0].prg1 = OPJ_CPRL;
  POC[1].tile = 1;
  POC[1].resno0 = numres - 1;
  POC[1].compno0 = 0;
  POC[1].layno1 = 1;
  POC[1].resno1 = numres;
  POC[1].compno1 = 3;
  POC[1].prg1 = OPJ_CPRL;
  return 2;
}

static void cinema_parameters(opj_cparameters_t *parameters)
{
  parameters->tile_size_on = 0;
  parameters->cp_tdx = 1;
  parameters->cp_tdy = 1;

  /*Tile part*/
  parameters->tp_flag = 'C';
  parameters->tp_on = 1;

  /*Tile and Image shall be at (0,0)*/
  parameters->cp_tx0 = 0;
  parameters->cp_ty0 = 0;
  parameters->image_offset_x0 = 0;
  parameters->image_offset_y0 = 0;

  /*Codeblock size= 32*32*/
  parameters->cblockw_init = 32;
  parameters->cblockh_init = 32;
  parameters->csty |= 0x01;

  /*The progression order shall be CPRL*/
  parameters->prog_order = OPJ_CPRL;

  /* No ROI */
  parameters->roi_compno = -1;

  parameters->subsampling_dx = 1;
  parameters->subsampling_dy = 1;

  /* 9-7 transform */
  parameters->irreversible = 1;
}

static void cinema_setup_encoder(opj_cparameters_t *parameters, opj_image_t *image, float *rates)
{
  int i;
  float temp_rate;

  switch(parameters->cp_cinema)
  {
    case OPJ_CINEMA2K_24:
    case OPJ_CINEMA2K_48:
      parameters->cp_rsiz = OPJ_CINEMA2K;
      if(parameters->numresolution > 6)
      {
        parameters->numresolution = 6;
      }
      if(!((image->comps[0].w == 2048) | (image->comps[0].h == 1080)))
      {
        fprintf(stdout,
                "Image coordinates %d x %d is not 2K compliant.\nJPEG Digital Cinema Profile-3 "
                "(2K profile) compliance requires that at least one of coordinates match 2048 x 1080\n",
                image->comps[0].w, image->comps[0].h);
        parameters->cp_rsiz = OPJ_STD_RSIZ;
      }
      break;

    case OPJ_CINEMA4K_24:
      parameters->cp_rsiz = OPJ_CINEMA4K;
      if(parameters->numresolution < 1)
      {
        parameters->numresolution = 1;
      }
      else if(parameters->numresolution > 7)
      {
        parameters->numresolution = 7;
      }
      if(!((image->comps[0].w == 4096) | (image->comps[0].h == 2160)))
      {
        fprintf(stdout,
                "Image coordinates %d x %d is not 4K compliant.\nJPEG Digital Cinema Profile-4"
                "(4K profile) compliance requires that at least one of coordinates match 4096 x 2160\n",
                image->comps[0].w, image->comps[0].h);
        parameters->cp_rsiz = OPJ_STD_RSIZ;
      }
      parameters->numpocs = initialise_4K_poc(parameters->POC, parameters->numresolution);
      break;
    default:
      break;
  }

  switch(parameters->cp_cinema)
  {
    case OPJ_CINEMA2K_24:
    case OPJ_CINEMA4K_24:
      for(i = 0; i < parameters->tcp_numlayers; i++)
      {
        if(rates[i] == 0)
        {
          parameters->tcp_rates[0]
              = ((float)(image->numcomps * image->comps[0].w * image->comps[0].h * image->comps[0].prec))
                / (OPJ_CINEMA_24_CS * 8 * image->comps[0].dx * image->comps[0].dy);
        }
        else
        {
          temp_rate
              = ((float)(image->numcomps * image->comps[0].w * image->comps[0].h * image->comps[0].prec))
                / (rates[i] * 8 * image->comps[0].dx * image->comps[0].dy);
          if(temp_rate > OPJ_CINEMA_24_CS)
          {
            parameters->tcp_rates[i]
                = ((float)(image->numcomps * image->comps[0].w * image->comps[0].h * image->comps[0].prec))
                  / (OPJ_CINEMA_24_CS * 8 * image->comps[0].dx * image->comps[0].dy);
          }
          else
          {
            parameters->tcp_rates[i] = rates[i];
          }
        }
      }
      parameters->max_comp_size = OPJ_CINEMA_24_COMP;
      break;

    case OPJ_CINEMA2K_48:
      for(i = 0; i < parameters->tcp_numlayers; i++)
      {
        if(rates[i] == 0)
        {
          parameters->tcp_rates[0]
              = ((float)(image->numcomps * image->comps[0].w * image->comps[0].h * image->comps[0].prec))
                / (OPJ_CINEMA_48_CS * 8 * image->comps[0].dx * image->comps[0].dy);
        }
        else
        {
          temp_rate
              = ((float)(image->numcomps * image->comps[0].w * image->comps[0].h * image->comps[0].prec))
                / (rates[i] * 8 * image->comps[0].dx * image->comps[0].dy);
          if(temp_rate > OPJ_CINEMA_48_CS)
          {
            parameters->tcp_rates[0]
                = ((float)(image->numcomps * image->comps[0].w * image->comps[0].h * image->comps[0].prec))
                  / (OPJ_CINEMA_48_CS * 8 * image->comps[0].dx * image->comps[0].dy);
          }
          else
          {
            parameters->tcp_rates[i] = rates[i];
          }
        }
      }
      parameters->max_comp_size = OPJ_CINEMA_48_COMP;
      break;
    default:
      break;
  }
  parameters->cp_disto_alloc = 1;
}

int write_image(dt_imageio_module_data_t *j2k_tmp, const char *filename, const void *in_tmp,
                dt_colorspaces_color_profile_type_t over_type, const char *over_filename,
                void *exif, int exif_len, int imgid, int num, int total, struct dt_dev_pixelpipe_t *pipe,
                const gboolean export_masks)
{
  int rc = 1;
  const float *in = (const float *)in_tmp;
  dt_imageio_j2k_t *j2k = (dt_imageio_j2k_t *)j2k_tmp;
  opj_cparameters_t parameters; /* compression parameters */
  float *rates = NULL;
  opj_image_t *image = NULL;
  const int quality = CLAMP(j2k->quality, 1, 100);

  /* set encoding parameters to default values */
  opj_set_default_encoder_parameters(&parameters);

  /* compression ratio */
  /* invert range, from 10-100, 100-1
  * where jpeg see's 1 and highest quality (lossless) and 100 is very low quality*/
  parameters.tcp_rates[0] = 100 - quality + 1;

  parameters.tcp_numlayers = 1; /* only one resolution */
  parameters.cp_disto_alloc = 1;
  parameters.cp_rsiz = OPJ_STD_RSIZ;

  parameters.cod_format = j2k->format;
  parameters.cp_cinema = (OPJ_CINEMA_MODE)j2k->preset;

  if(parameters.cp_cinema)
  {
    rates = (float *)calloc(parameters.tcp_numlayers, sizeof(float));
    for(int i = 0; i < parameters.tcp_numlayers; i++)
    {
      rates[i] = parameters.tcp_rates[i];
    }
    cinema_parameters(&parameters);
  }

  /* Create comment for codestream */
  parameters.cp_comment = g_strdup_printf("Created with %s", darktable_package_string);

  /*Converting the image to a format suitable for encoding*/
  {
    const int subsampling_dx = parameters.subsampling_dx;
    const int subsampling_dy = parameters.subsampling_dy;
    const int numcomps = 3;
    const int prec = 12; // TODO: allow other bitdepths!
    const int w = j2k->global.width, h = j2k->global.height;

    opj_image_cmptparm_t cmptparm[4]; /* RGBA: max. 4 components */
    memset(&cmptparm[0], 0, sizeof(opj_image_cmptparm_t) * numcomps);

    for(int i = 0; i < numcomps; i++)
    {
      cmptparm[i].prec = prec;
      cmptparm[i].sgnd = 0;
      cmptparm[i].dx = subsampling_dx;
      cmptparm[i].dy = subsampling_dy;
      cmptparm[i].w = w;
      cmptparm[i].h = h;
    }
    image = opj_image_create(numcomps, &cmptparm[0], OPJ_CLRSPC_SRGB);
    if(!image)
    {
      dt_print(DT_DEBUG_ALWAYS, "Error: opj_image_create() failed\n");
      free(rates);
      rc = 0;
      goto exit;
    }

    /* set image offset and reference grid */
    image->x0 = parameters.image_offset_x0;
    image->y0 = parameters.image_offset_y0;
    image->x1 = parameters.image_offset_x0 + (w - 1) * subsampling_dx + 1;
    image->y1 = parameters.image_offset_y0 + (h - 1) * subsampling_dy + 1;

    switch(prec)
    {
//      case 8:
//        for(int i = 0; i < w * h; i++)
//        {
//          for(int k = 0; k < numcomps; k++) image->comps[k].data[i] = DOWNSAMPLE_FLOAT_TO_8BIT(in[i * 4 + k]);
//        }
//        break;
      case 12:
        for(int i = 0; i < w * h; i++)
        {
          for(int k = 0; k < numcomps; k++)
            image->comps[k].data[i] = DOWNSAMPLE_FLOAT_TO_12BIT(in[i * 4 + k]);
        }
        break;
//      case 16:
//        for(int i = 0; i < w * h; i++)
//        {
//          for(int k = 0; k < numcomps; k++)
//            image->comps[k].data[i] = DOWNSAMPLE_FLOAT_TO_16BIT(in[i * 4 + k]);
//        }
//        break;
//      default:
//        dt_print(DT_DEBUG_ALWAYS, "Error: this shouldn't happen, there is no bit depth of %d for jpeg 2000 images.\n",
//                prec);
//        free(rates);
//        opj_image_destroy(image);
//        return 1;
    }
  }

  /*Encoding image*/

  /* Decide if MCT should be used */
  parameters.tcp_mct = image->numcomps == 3 ? 1 : 0;

  if(parameters.cp_cinema)
  {
    cinema_setup_encoder(&parameters, image, rates);
    free(rates);
  }

  /* encode the destination image */
  /* ---------------------------- */
  OPJ_CODEC_FORMAT codec;
  if(parameters.cod_format == J2K_CFMT) /* J2K format output */
    codec = OPJ_CODEC_J2K;
  else
    codec = OPJ_CODEC_JP2;

  opj_stream_t *cstream = NULL;

  /* get a J2K/JP2 compressor handle */
  opj_codec_t *ccodec = opj_create_compress(codec);

  opj_set_error_handler(ccodec, error_callback, stderr);
  opj_set_warning_handler(ccodec, warning_callback, stderr);
  opj_set_info_handler(ccodec, info_callback, stderr);

  g_strlcpy(parameters.outfile, filename, sizeof(parameters.outfile));

  /* setup the encoder parameters using the current image and user parameters */
  opj_setup_encoder(ccodec, &parameters, image);

  /* open a byte stream for writing */
  /* allocate memory for all tiles */
  cstream = opj_stream_create_default_file_stream(parameters.outfile, OPJ_FALSE);
  if(!cstream)
  {
    opj_destroy_codec(ccodec);
    opj_image_destroy(image);
    dt_print(DT_DEBUG_ALWAYS, "failed to create output stream\n");
    rc = 0;
    goto exit;
  }

  if(!opj_start_compress(ccodec, image, cstream))
  {
    opj_stream_destroy(cstream);
    opj_destroy_codec(ccodec);
    opj_image_destroy(image);
    dt_print(DT_DEBUG_ALWAYS, "failed to encode image: opj_start_compress\n");
    rc = 0;
    goto exit;
  }

  /* encode the image */
  if(!opj_encode(ccodec, cstream))
  {
    opj_stream_destroy(cstream);
    opj_destroy_codec(ccodec);
    opj_image_destroy(image);
    dt_print(DT_DEBUG_ALWAYS, "failed to encode image: opj_encode\n");
    rc = 0;
    goto exit;
  }

  /* encode the image */
  if(!opj_end_compress(ccodec, cstream))
  {
    opj_stream_destroy(cstream);
    opj_destroy_codec(ccodec);
    opj_image_destroy(image);
    dt_print(DT_DEBUG_ALWAYS, "failed to encode image: opj_end_compress\n");
    rc = 0;
    goto exit;
  }

  opj_stream_destroy(cstream);
  opj_destroy_codec(ccodec);

  /* add exif data blob. seems to not work for j2k files :( */
  if(exif && j2k->format == JP2_CFMT) rc = dt_exif_write_blob(exif, exif_len, filename, 1);

  /* free image data */
  opj_image_destroy(image);

exit:
  /* free user parameters structure */
  g_free(parameters.cp_comment);
  free(parameters.cp_matrice);
  free(parameters.mct_data);

  return ((rc == 1) ? 0 : 1);
}

size_t params_size(dt_imageio_module_format_t *self)
{
  return sizeof(dt_imageio_j2k_t);
}

void *legacy_params(dt_imageio_module_format_t *self, const void *const old_params,
                    const size_t old_params_size, const int old_version, const int new_version,
                    size_t *new_size)
{
  if(old_version == 1 && new_version == 2)
  {
    typedef struct dt_imageio_j2k_v1_t
    {
      int max_width, max_height;
      int width, height;
      char style[128];
      int bpp;
      dt_imageio_j2k_format_t format;
      dt_imageio_j2k_preset_t preset;
      int quality;
    } dt_imageio_j2k_v1_t;

    dt_imageio_j2k_v1_t *o = (dt_imageio_j2k_v1_t *)old_params;
    dt_imageio_j2k_t *n = (dt_imageio_j2k_t *)malloc(sizeof(dt_imageio_j2k_t));

    n->global.max_width = o->max_width;
    n->global.max_height = o->max_height;
    n->global.width = o->width;
    n->global.height = o->height;
    g_strlcpy(n->global.style, o->style, sizeof(o->style));
    n->global.style_append = FALSE;
    n->bpp = o->bpp;
    n->format = o->format;
    n->preset = o->preset;
    n->quality = o->quality;
    *new_size = self->params_size(self);
    return n;
  }
  return NULL;
}

void *get_params(dt_imageio_module_format_t *self)
{
  dt_imageio_j2k_t *d = (dt_imageio_j2k_t *)calloc(1, sizeof(dt_imageio_j2k_t));
  d->bpp = 12; // can be 8, 12 or 16
  d->format = dt_conf_get_int("plugins/imageio/format/j2k/format");
  d->preset = dt_conf_get_int("plugins/imageio/format/j2k/preset");
  d->quality = dt_conf_get_int("plugins/imageio/format/j2k/quality");
  if(d->quality <= 0 || d->quality > 100) d->quality = 100;
  return d;
}

void free_params(dt_imageio_module_format_t *self, dt_imageio_module_data_t *params)
{
  free(params);
}

int set_params(dt_imageio_module_format_t *self, const void *params, const int size)
{
  if(size != self->params_size(self)) return 1;
  dt_imageio_j2k_t *d = (dt_imageio_j2k_t *)params;
  dt_imageio_j2k_gui_t *g = (dt_imageio_j2k_gui_t *)self->gui_data;
  dt_bauhaus_combobox_set(g->format, d->format);
  dt_bauhaus_combobox_set(g->preset, d->preset);
  dt_bauhaus_slider_set(g->quality, d->quality);
  return 0;
}

int bpp(dt_imageio_module_data_t *p)
{
  return 32;
}

int levels(dt_imageio_module_data_t *p)
{
  // TODO: adapt as soon as this module supports various bitdepths
  return IMAGEIO_RGB | IMAGEIO_INT12;
}

const char *mime(dt_imageio_module_data_t *data)
{
  return "image/jp2";
}

const char *extension(dt_imageio_module_data_t *data_tmp)
{
  const dt_imageio_j2k_t *data = (dt_imageio_j2k_t *)data_tmp;
  if(data->format == J2K_CFMT)
    return "j2k";
  else
    return "jp2";
}

const char *name()
{
  return _("JPEG 2000 (12-bit)");
}

static void preset_changed(GtkWidget *widget, gpointer user_data)
{
  const int preset = dt_bauhaus_combobox_get(widget);
  dt_conf_set_int("plugins/imageio/format/j2k/preset", preset);
}

static void format_changed(GtkWidget *widget, gpointer user_data)
{
  const int format = dt_bauhaus_combobox_get(widget);
  dt_conf_set_int("plugins/imageio/format/j2k/format", format);
}

static void quality_changed(GtkWidget *slider, gpointer user_data)
{
  int quality = (int)dt_bauhaus_slider_get(slider);
  dt_conf_set_int("plugins/imageio/format/j2k/quality", quality);
}

// TODO: some quality/compression stuff in case "off" is selected
void gui_init(dt_imageio_module_format_t *self)
{
  dt_imageio_j2k_gui_t *gui = (dt_imageio_j2k_gui_t *)malloc(sizeof(dt_imageio_j2k_gui_t));
  self->gui_data = (void *)gui;
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  const int format_last = dt_conf_get_int("plugins/imageio/format/j2k/format");
  const int preset_last = dt_conf_get_int("plugins/imageio/format/j2k/preset");
  const int quality_last = dt_conf_get_int("plugins/imageio/format/j2k/quality");

  DT_BAUHAUS_COMBOBOX_NEW_FULL(gui->format, self, NULL, N_("Format"), NULL,
                               format_last, format_changed, self,
                               N_("J2k"), N_("Jp2"));
  gtk_box_pack_start(GTK_BOX(self->widget), gui->format, TRUE, TRUE, 0);

  gui->quality = dt_bauhaus_slider_new_with_range((dt_iop_module_t*)self,
                                                  dt_confgen_get_int("plugins/imageio/format/j2k/quality", DT_MIN),
                                                  dt_confgen_get_int("plugins/imageio/format/j2k/quality", DT_MAX),
                                                  1,
                                                  dt_confgen_get_int("plugins/imageio/format/j2k/quality", DT_DEFAULT),
                                                  0);
  dt_bauhaus_widget_set_label(gui->quality, NULL, N_("Quality"));
  dt_bauhaus_slider_set_default(gui->quality, dt_confgen_get_int("plugins/imageio/format/j2k/quality", DT_DEFAULT));
  if(quality_last > 0 && quality_last <= 100) dt_bauhaus_slider_set(gui->quality, quality_last);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(gui->quality), TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(gui->quality), "value-changed", G_CALLBACK(quality_changed), NULL);

  DT_BAUHAUS_COMBOBOX_NEW_FULL(gui->preset, self, NULL, N_("DCP mode"), NULL,
                               preset_last, preset_changed, self,
                               N_("Off"),
                               N_("Cinema2K, 24FPS"),
                               N_("Cinema2K, 48FPS"),
                               N_("Cinema4K, 24FPS"));
  gtk_box_pack_start(GTK_BOX(self->widget), gui->preset, TRUE, TRUE, 0);

  // TODO: options for "off"
}

void gui_cleanup(dt_imageio_module_format_t *self)
{
  free(self->gui_data);
}

void gui_reset(dt_imageio_module_format_t *self)
{
  const int format_def = dt_confgen_get_int("plugins/imageio/format/j2k/format", DT_DEFAULT);
  const int preset_def = dt_confgen_get_int("plugins/imageio/format/j2k/preset", DT_DEFAULT);
  const int quality_def = dt_confgen_get_int("plugins/imageio/format/j2k/quality", DT_DEFAULT);
  dt_imageio_j2k_gui_t *gui = (dt_imageio_j2k_gui_t *)self->gui_data;
  dt_bauhaus_combobox_set(gui->format, format_def);
  dt_bauhaus_combobox_set(gui->preset, preset_def);
  dt_bauhaus_combobox_set(gui->quality, quality_def);
}

int flags(dt_imageio_module_data_t *data)
{
  dt_imageio_j2k_t *j = (dt_imageio_j2k_t *)data;
  return ((j && j->format == JP2_CFMT) ? FORMAT_FLAGS_SUPPORT_XMP : 0);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
