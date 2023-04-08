/*
    This file is part of darktable,
    Copyright (C) 2020-2023 darktable developers.

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
#include "develop/pixelpipe_hb.h"
#include "external/libxcf/xcf.h"
#include "imageio/imageio_common.h"
#include "imageio/imageio_module.h"
#include "imageio/format/imageio_format_api.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

DT_MODULE(1)

typedef struct dt_imageio_xcf_gui_t
{
  GtkWidget *bpp;
} dt_imageio_xcf_gui_t;

typedef struct dt_imageio_xcf_t
{
  dt_imageio_module_data_t global;
  int bpp;
} dt_imageio_xcf_t;

int write_image(dt_imageio_module_data_t *data, const char *filename, const void *ivoid,
                dt_colorspaces_color_profile_type_t over_type, const char *over_filename,
                void *exif, int exif_len, dt_imgid_t imgid, int num, int total, struct dt_dev_pixelpipe_t *pipe,
                const gboolean export_masks)
{
  const dt_imageio_xcf_t *const d = (dt_imageio_xcf_t *)data;

  int res = 1;

  uint8_t *profile = NULL;
  uint32_t profile_len = 0;
  gboolean profile_is_linear = TRUE;

  cmsHPROFILE out_profile = dt_colorspaces_get_output_profile(imgid, over_type, over_filename)->profile;
  cmsSaveProfileToMem(out_profile, NULL, &profile_len);
  if(profile_len > 0)
  {
    profile = malloc(profile_len);
    if(!profile)
    {
      dt_print(DT_DEBUG_ALWAYS, "[xcf] error: can't allocate %u bytes of memory\n", profile_len);
      return 1;
    }
    cmsSaveProfileToMem(out_profile, profile, &profile_len);

    // try to figure out if the profile is linear
    if(cmsIsMatrixShaper(out_profile))
    {
      const cmsToneCurve *red_curve = (cmsToneCurve *)cmsReadTag(out_profile, cmsSigRedTRCTag);
      const cmsToneCurve *green_curve = (cmsToneCurve *)cmsReadTag(out_profile, cmsSigGreenTRCTag);
      const cmsToneCurve *blue_curve = (cmsToneCurve *)cmsReadTag(out_profile, cmsSigBlueTRCTag);
      if(red_curve && green_curve && blue_curve)
      {
        profile_is_linear = cmsIsToneCurveLinear(red_curve) && cmsIsToneCurveLinear(green_curve)
                            && cmsIsToneCurveLinear(blue_curve);
      }
    }
  }

  XCF *xcf = xcf_open(filename);

  if(!xcf)
  {
    dt_print(DT_DEBUG_ALWAYS, "[xcf] error: can't open `%s'\n", filename);
    goto exit;
  }

  xcf_set(xcf, XCF_BASE_TYPE, XCF_BASE_TYPE_RGB);
  xcf_set(xcf, XCF_WIDTH, d->global.width);
  xcf_set(xcf, XCF_HEIGHT, d->global.height);

  if(d->bpp == 8)
    xcf_set(xcf, XCF_PRECISION, profile_is_linear ? XCF_PRECISION_I_8_L : XCF_PRECISION_I_8_G);
  else if(d->bpp == 16)
    xcf_set(xcf, XCF_PRECISION, profile_is_linear ? XCF_PRECISION_I_16_L : XCF_PRECISION_I_16_G);
  else if(d->bpp == 32)
    xcf_set(xcf, XCF_PRECISION, profile_is_linear ? XCF_PRECISION_F_32_L : XCF_PRECISION_F_32_G);
  else
  {
    dt_print(DT_DEBUG_ALWAYS, "[xcf] error: bpp of %d is not supported\n", d->bpp);
    goto exit;
  }

  if(profile)
  {
    xcf_set(xcf, XCF_PROP, XCF_PROP_PARASITES, "icc-profile", XCF_PARASITE_PERSISTENT | XCF_PARASITE_UNDOABLE,
            profile_len, profile);
  }

  xcf_set(xcf, XCF_N_LAYERS, 1);
  int n_channels = 0;
  if(export_masks && pipe)
  {
    for(GList *iter = pipe->nodes; iter; iter = g_list_next(iter))
      n_channels += g_hash_table_size(((dt_dev_pixelpipe_iop_t *)iter->data)->raster_masks);
  }
  xcf_set(xcf, XCF_N_CHANNELS, n_channels);
  xcf_set(xcf, XCF_OMIT_BASE_ALPHA, 1);

  char *comment = g_strdup_printf("Created with %s", darktable_package_string);
  xcf_set(xcf, XCF_PROP, XCF_PROP_PARASITES, "gimp-comment", XCF_PARASITE_PERSISTENT, strlen(comment) + 1, comment);
  g_free(comment);

  if(exif && exif_len > 0)
  {
    // Prepend the libexif expected "Exif\0\0" APP1 prefix (see GIMP parasites.txt)
    uint8_t *exif_buf = g_malloc0(exif_len + 6);
    if(!exif_buf)
    {
      dt_print(DT_DEBUG_ALWAYS, "[xcf] error: can't allocate %d bytes of memory\n", exif_len + 6);
      goto exit;
    }
    memcpy(exif_buf, "Exif\0\0", 6);
    memcpy(exif_buf + 6, exif, exif_len);
    xcf_set(xcf, XCF_PROP, XCF_PROP_PARASITES, "exif-data", XCF_PARASITE_PERSISTENT, exif_len + 6, exif_buf);
    g_free(exif_buf);
  }

  // TODO: workaround; uses valid exif as a way to indicate ALL metadata was requested
  if(exif && exif_len > 0)
  {
    char *xmp_string = dt_exif_xmp_read_string(imgid);
    size_t xmp_len;
    if(xmp_string && (xmp_len = strlen(xmp_string)) > 0)
    {
      xcf_set(xcf, XCF_PROP, XCF_PROP_PARASITES, "gimp-metadata", XCF_PARASITE_PERSISTENT, xmp_len, xmp_string);
      g_free(xmp_string);
    }
  }

  xcf_add_layer(xcf);
  xcf_set(xcf, XCF_WIDTH, d->global.width);
  xcf_set(xcf, XCF_HEIGHT, d->global.height);
  xcf_set(xcf, XCF_NAME, _("image"));
  // we only add one layer and omit its alpha channel. thus we can just pass ivoid and ignore its 4th channel!
  xcf_add_data(xcf, ivoid, 4);

  if(n_channels > 0)
    for(GList *iter = pipe->nodes; iter; iter = g_list_next(iter))
    {
      dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)iter->data;

      GHashTableIter rm_iter;
      gpointer key, value;

      g_hash_table_iter_init(&rm_iter, piece->raster_masks);
      while(g_hash_table_iter_next(&rm_iter, &key, &value))
      {
        gboolean free_mask = TRUE;
        float *raster_mask = dt_dev_get_raster_mask(pipe, piece->module, GPOINTER_TO_INT(key), NULL, &free_mask);

        if(!raster_mask)
        {
          // this should never happen
          dt_print(DT_DEBUG_ALWAYS, "error: can't get raster mask from `%s'\n", piece->module->name());
          goto exit;
        }

        xcf_add_channel(xcf);
        xcf_set(xcf, XCF_PROP, XCF_PROP_VISIBLE, 0);

        const char *pagename = g_hash_table_lookup(piece->module->raster_mask.source.masks, key);
        if(pagename)
          xcf_set(xcf, XCF_NAME, pagename);
        else
          xcf_set(xcf, XCF_NAME, piece->module->name());

        void *channel_data = NULL;
        gboolean free_channel_data = TRUE;
        if(d->bpp == 8)
        {
          channel_data = malloc(sizeof(uint8_t) * d->global.width * d->global.height);
          uint8_t *ch = (uint8_t *)channel_data;
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(ch, d, raster_mask) \
  schedule(simd:static)
#endif
          for(size_t i = 0; i < (size_t)d->global.width * d->global.height; ++i)
            ch[i] = (uint8_t)roundf(CLIP(raster_mask[i]) * 255.0f);
        }
        else if(d->bpp == 16)
        {
          channel_data = malloc(sizeof(uint16_t) * d->global.width * d->global.height);
          uint16_t *ch = (uint16_t *)channel_data;
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(ch, d, raster_mask) \
  schedule(simd:static)
#endif
          for(size_t i = 0; i < (size_t)d->global.width * d->global.height; ++i)
            ch[i] = (uint16_t)roundf(CLIP(raster_mask[i]) * 65535.0f);
        }
        else if(d->bpp == 32)
        {
          channel_data = raster_mask;
          free_channel_data = FALSE;
        }

        xcf_add_data(xcf, channel_data, 1);

        if(free_channel_data)
          free(channel_data);
        if(free_mask)
          dt_free_align(raster_mask);
      }
    }

  res = 0;

exit:
  xcf_close(xcf);
  free(profile);

  return res;
}

size_t params_size(dt_imageio_module_format_t *self)
{
  return sizeof(dt_imageio_xcf_t);
}

void *get_params(dt_imageio_module_format_t *self)
{
  dt_imageio_xcf_t *d = (dt_imageio_xcf_t *)calloc(1, sizeof(dt_imageio_xcf_t));

  const char *conf_bpp = dt_conf_get_string_const("plugins/imageio/format/xcf/bpp");
  d->bpp = atoi(conf_bpp);
  if(d->bpp != 16 && d->bpp != 32)
    d->bpp = 8;

  return d;
}

void free_params(dt_imageio_module_format_t *self, dt_imageio_module_data_t *params)
{
  free(params);
}

int set_params(dt_imageio_module_format_t *self, const void *params, int size)
{
  if(size != params_size(self)) return 1;
  const dt_imageio_xcf_t *d = (dt_imageio_xcf_t *)params;
  const dt_imageio_xcf_gui_t *g = (dt_imageio_xcf_gui_t *)self->gui_data;

  if(d->bpp == 16)
    dt_bauhaus_combobox_set(g->bpp, 1);
  else if(d->bpp == 32)
    dt_bauhaus_combobox_set(g->bpp, 2);
  else // (d->bpp == 8)
    dt_bauhaus_combobox_set(g->bpp, 0);

  return 0;
}

int flags(dt_imageio_module_data_t *data)
{
  return FORMAT_FLAGS_SUPPORT_LAYERS;
}

int bpp(dt_imageio_module_data_t *p)
{
  return ((dt_imageio_xcf_t *)p)->bpp;
}

int levels(dt_imageio_module_data_t *p)
{
  const int bpp = ((dt_imageio_xcf_t *)p)->bpp;
  int ret = IMAGEIO_RGB;

  if(bpp == 8)
    ret |= IMAGEIO_INT8;
  else if(bpp == 16)
    ret |= IMAGEIO_INT16;
  else if(bpp == 32)
    ret |= IMAGEIO_FLOAT;

  return ret;
}

const char *mime(dt_imageio_module_data_t *data)
{
  return "image/x-xcf";
}

const char *extension(dt_imageio_module_data_t *data)
{
  return "xcf";
}

const char *name()
{
  return _("XCF");
}

void init(dt_imageio_module_format_t *self)
{
#ifdef USE_LUA
  dt_lua_register_module_member(darktable.lua_state.state, self, dt_imageio_xcf_t, bpp, int);
#endif
}
void cleanup(dt_imageio_module_format_t *self)
{
}

static void bpp_combobox_changed(GtkWidget *widget, gpointer user_data)
{
  const int bpp = dt_bauhaus_combobox_get(widget);

  if(bpp == 1)
    dt_conf_set_int("plugins/imageio/format/xcf/bpp", 16);
  else if(bpp == 2)
    dt_conf_set_int("plugins/imageio/format/xcf/bpp", 32);
  else // (bpp == 0)
    dt_conf_set_int("plugins/imageio/format/xcf/bpp", 8);
}

void gui_init(dt_imageio_module_format_t *self)
{
  dt_imageio_xcf_gui_t *gui = (dt_imageio_xcf_gui_t *)malloc(sizeof(dt_imageio_xcf_gui_t));
  self->gui_data = (void *)gui;

  int bpp = 32;
  if(dt_conf_key_exists("plugins/imageio/format/xcf/bpp"))
  {
    const char *conf_bpp = dt_conf_get_string_const("plugins/imageio/format/xcf/bpp");
    bpp = atoi(conf_bpp);
  }

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  // Bit depth combo box
  DT_BAUHAUS_COMBOBOX_NEW_FULL(gui->bpp, self, NULL, N_("bit depth"), NULL,
                               bpp == 16 ? 1 : bpp == 32 ? 2 : 0,
                               bpp_combobox_changed, NULL,
                               N_("8 bit"), N_("16 bit"), N_("32 bit (float)"));
  gtk_box_pack_start(GTK_BOX(self->widget), gui->bpp, TRUE, TRUE, 0);
}

void gui_cleanup(dt_imageio_module_format_t *self)
{
  free(self->gui_data);
}

void gui_reset(dt_imageio_module_format_t *self)
{
  dt_imageio_xcf_gui_t *gui = (dt_imageio_xcf_gui_t *)self->gui_data;
  dt_bauhaus_combobox_set(gui->bpp, 2); // bpp = 32
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
