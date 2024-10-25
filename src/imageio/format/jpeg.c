/*
    This file is part of darktable,
    Copyright (C) 2010-2024 darktable developers.

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
#include "bauhaus/bauhaus.h"
#include "common/colorspaces.h"
#include "common/darktable.h"
#include "common/exif.h"
#include "control/conf.h"
#include "imageio/imageio_common.h"
#include "imageio/imageio_module.h"
#include "imageio/format/imageio_format_api.h"
#include <inttypes.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
// this fixes a rather annoying, long time bug in libjpeg :(
#undef HAVE_STDLIB_H
#undef HAVE_STDDEF_H
#include <jpeglib.h>
#undef HAVE_STDLIB_H
#undef HAVE_STDDEF_H

DT_MODULE(3)

typedef enum dt_imageio_jpeg_subsample_t
{
  DT_SUBSAMPLE_AUTO,
  DT_SUBSAMPLE_444,
  DT_SUBSAMPLE_440,
  DT_SUBSAMPLE_422,
  DT_SUBSAMPLE_420
} dt_imageio_jpeg_subsample_t;

typedef struct dt_imageio_jpeg_t
{
  dt_imageio_module_data_t global;
  int quality;
  dt_imageio_jpeg_subsample_t subsample;
  struct jpeg_source_mgr src;
  struct jpeg_destination_mgr dest;
  struct jpeg_decompress_struct dinfo;
  struct jpeg_compress_struct cinfo;
  FILE *f;
} dt_imageio_jpeg_t;

typedef struct dt_imageio_jpeg_gui_data_t
{
  GtkWidget *quality;
  GtkWidget *subsample;
} dt_imageio_jpeg_gui_data_t;

// error functions
struct dt_imageio_jpeg_error_mgr
{
  struct jpeg_error_mgr pub;
  jmp_buf setjmp_buffer;
} dt_imageio_jpeg_error_mgr;

typedef struct dt_imageio_jpeg_error_mgr *dt_imageio_jpeg_error_ptr;

static void dt_imageio_jpeg_error_exit(j_common_ptr cinfo)
{
  const dt_imageio_jpeg_error_ptr myerr = (dt_imageio_jpeg_error_ptr)cinfo->err;
  (*cinfo->err->output_message)(cinfo);
  longjmp(myerr->setjmp_buffer, 1);
}

/*
 * Since an ICC profile can be larger than the maximum size of a JPEG marker
 * (64K), we need provisions to split it into multiple markers.  The format
 * defined by the ICC specifies one or more APP2 markers containing the
 * following data:
 *  Identifying string  ASCII "ICC_PROFILE\0"  (12 bytes)
 *  Marker sequence number  1 for first APP2, 2 for next, etc (1 byte)
 *  Number of markers Total number of APP2's used (1 byte)
 *      Profile data    (remainder of APP2 data)
 * Decoders should use the marker sequence numbers to reassemble the profile,
 * rather than assuming that the APP2 markers appear in the correct sequence.
 */

#define ICC_MARKER (JPEG_APP0 + 2) /* JPEG marker code for ICC */
#define ICC_OVERHEAD_LEN 14        /* size of non-profile data in APP2 */
#define MAX_BYTES_IN_MARKER 65533  /* maximum data len of a JPEG marker */
#define MAX_DATA_BYTES_IN_MARKER (MAX_BYTES_IN_MARKER - ICC_OVERHEAD_LEN)


/*
 * This routine writes the given ICC profile data into a JPEG file.
 * It *must* be called AFTER calling jpeg_start_compress() and BEFORE
 * the first call to jpeg_write_scanlines().
 * (This ordering ensures that the APP2 marker(s) will appear after the
 * SOI and JFIF or Adobe markers, but before all else.)
 */

static void write_icc_profile(j_compress_ptr cinfo,
                              const JOCTET *icc_data_ptr,
                              unsigned int icc_data_len)
{
  int cur_marker = 1;       /* per spec, counting starts at 1 */

  /* Calculate the number of markers we'll need, rounding up of course */
  unsigned int num_markers = icc_data_len / MAX_DATA_BYTES_IN_MARKER;
  if(num_markers * MAX_DATA_BYTES_IN_MARKER != icc_data_len)
    num_markers++;

  while(icc_data_len > 0)
  {
    /* length of profile to put in this marker */
    unsigned int length = icc_data_len;
    if(length > MAX_DATA_BYTES_IN_MARKER)
      length = MAX_DATA_BYTES_IN_MARKER;
    icc_data_len -= length;

    /* Write the JPEG marker header (APP2 code and marker length) */
    jpeg_write_m_header(cinfo, ICC_MARKER, (unsigned int)(length + ICC_OVERHEAD_LEN));

    /* Write the marker identifying string "ICC_PROFILE" (null-terminated).
     * We code it in this less-than-transparent way so that the code works
     * even if the local character set is not ASCII.
     */
    jpeg_write_m_byte(cinfo, 0x49);
    jpeg_write_m_byte(cinfo, 0x43);
    jpeg_write_m_byte(cinfo, 0x43);
    jpeg_write_m_byte(cinfo, 0x5F);
    jpeg_write_m_byte(cinfo, 0x50);
    jpeg_write_m_byte(cinfo, 0x52);
    jpeg_write_m_byte(cinfo, 0x4F);
    jpeg_write_m_byte(cinfo, 0x46);
    jpeg_write_m_byte(cinfo, 0x49);
    jpeg_write_m_byte(cinfo, 0x4C);
    jpeg_write_m_byte(cinfo, 0x45);
    jpeg_write_m_byte(cinfo, 0x0);

    /* Add the sequencing info */
    jpeg_write_m_byte(cinfo, cur_marker);
    jpeg_write_m_byte(cinfo, (int)num_markers);

    /* Add the profile data */
    while(length--)
    {
      jpeg_write_m_byte(cinfo, *icc_data_ptr);
      icc_data_ptr++;
    }
    cur_marker++;
  }
}


#undef ICC_MARKER
#undef ICC_OVERHEAD_LEN
#undef MAX_BYTES_IN_MARKER
#undef MAX_DATA_BYTES_IN_MARKER
#undef MAX_SEQ_NO


int write_image(dt_imageio_module_data_t *jpg_tmp,
                const char *filename,
                const void *in_tmp,
                dt_colorspaces_color_profile_type_t over_type,
                const char *over_filename,
                void *exif, int exif_len,
                dt_imgid_t imgid,
                int num,
                int total,
                struct dt_dev_pixelpipe_t *pipe,
                const gboolean export_masks)
{
  dt_imageio_jpeg_t *jpg = (dt_imageio_jpeg_t *)jpg_tmp;
  const uint8_t *in = (const uint8_t *)in_tmp;
  struct dt_imageio_jpeg_error_mgr jerr;

  jpg->cinfo.err = jpeg_std_error(&jerr.pub);
  jerr.pub.error_exit = dt_imageio_jpeg_error_exit;
  if(setjmp(jerr.setjmp_buffer))
  {
    jpeg_destroy_compress(&(jpg->cinfo));
    return 1;
  }
  jpeg_create_compress(&(jpg->cinfo));
  FILE *f = g_fopen(filename, "wb");
  if(!f) return 1;
  jpeg_stdio_dest(&(jpg->cinfo), f);

  jpg->cinfo.image_width = jpg->global.width;
  jpg->cinfo.image_height = jpg->global.height;
  jpg->cinfo.input_components = 3;
  jpg->cinfo.in_color_space = JCS_RGB;
  jpeg_set_defaults(&(jpg->cinfo));
  jpeg_set_quality(&(jpg->cinfo), jpg->quality, TRUE);

  if(jpg->quality > 90) jpg->cinfo.comp_info[0].v_samp_factor = 1;
  if(jpg->quality > 92) jpg->cinfo.comp_info[0].h_samp_factor = 1;
  if(jpg->quality > 95) jpg->cinfo.dct_method = JDCT_FLOAT;
  if(jpg->quality < 50) jpg->cinfo.dct_method = JDCT_IFAST;
  if(jpg->quality < 80) jpg->cinfo.smoothing_factor = 20;
  if(jpg->quality < 60) jpg->cinfo.smoothing_factor = 40;
  if(jpg->quality < 40) jpg->cinfo.smoothing_factor = 60;
  jpg->cinfo.optimize_coding = 1;

  // Common part for all subsampling formulas:
  jpg->cinfo.comp_info[1].h_samp_factor = 1;
  jpg->cinfo.comp_info[1].v_samp_factor = 1;
  jpg->cinfo.comp_info[2].h_samp_factor = 1;
  jpg->cinfo.comp_info[2].v_samp_factor = 1;

  const int subsample = dt_conf_get_int("plugins/imageio/format/jpeg/subsample");
  switch(subsample)
  {
    case 1: // 1x1 1x1 1x1 (4:4:4) : No chroma subsampling
    {
      jpg->cinfo.comp_info[0].h_samp_factor = 1;
      jpg->cinfo.comp_info[0].v_samp_factor = 1;
      break;
    }
    case 2: // 1x2 1x1 1x1 (4:4:0) : Color sampling rate halved vertically
    {
      jpg->cinfo.comp_info[0].h_samp_factor = 1;
      jpg->cinfo.comp_info[0].v_samp_factor = 2;
      break;
    }
    case 3: // 2x1 1x1 1x1 (4:2:2) : Color sampling rate halved horizontally
    {
      jpg->cinfo.comp_info[0].h_samp_factor = 2;
      jpg->cinfo.comp_info[0].v_samp_factor = 1;
      break;
    }
    case 4: // 2x2 1x1 1x1 (4:2:0) : Color sampling rate halved horizontally and vertically
    {
      jpg->cinfo.comp_info[0].h_samp_factor = 2;
      jpg->cinfo.comp_info[0].v_samp_factor = 2;
      break;
    }
  }

  const int resolution = dt_conf_get_int("metadata/resolution");
  jpg->cinfo.density_unit = 1;
  jpg->cinfo.X_density = resolution;
  jpg->cinfo.Y_density = resolution;

  jpeg_start_compress(&(jpg->cinfo), TRUE);

  cmsHPROFILE out_profile =
    dt_colorspaces_get_output_profile(imgid, over_type, over_filename)->profile;
  uint32_t len = 0;
  cmsSaveProfileToMem(out_profile, NULL, &len);
  if(len > 0)
  {
    unsigned char *buf = malloc(sizeof(unsigned char) * len);
    if(buf)
    {
      cmsSaveProfileToMem(out_profile, buf, &len);
      write_icc_profile(&(jpg->cinfo), buf, len);
      free(buf);
    }
  }

  uint8_t *row = dt_alloc_align_uint8(3 * jpg->global.width);
  const uint8_t *buf;
  while(row && jpg->cinfo.next_scanline < jpg->cinfo.image_height)
  {
    JSAMPROW tmp[1];
    buf = in + (size_t)jpg->cinfo.next_scanline * jpg->cinfo.image_width * 4;
    for(int i = 0; i < jpg->global.width; i++)
      for(int k = 0; k < 3; k++) row[3 * i + k] = buf[4 * i + k];
    tmp[0] = row;
    jpeg_write_scanlines(&(jpg->cinfo), tmp, 1);
  }
  jpeg_finish_compress(&(jpg->cinfo));
  dt_free_align(row);
  jpeg_destroy_compress(&(jpg->cinfo));
  fclose(f);

  if(exif) dt_exif_write_blob(exif, exif_len, filename, 1);

  return 0;
}

static int __attribute__((__unused__)) read_header(const char *filename,
                                                   dt_imageio_jpeg_t *jpg)
{
  jpg->f = g_fopen(filename, "rb");
  if(!jpg->f) return 1;

  struct dt_imageio_jpeg_error_mgr jerr;
  jpg->dinfo.err = jpeg_std_error(&jerr.pub);
  jerr.pub.error_exit = dt_imageio_jpeg_error_exit;
  if(setjmp(jerr.setjmp_buffer))
  {
    jpeg_destroy_decompress(&(jpg->dinfo));
    fclose(jpg->f);
    return 1;
  }
  jpeg_create_decompress(&(jpg->dinfo));
  jpeg_stdio_src(&(jpg->dinfo), jpg->f);
  // jpg->dinfo.buffered_image = TRUE;
  jpeg_read_header(&(jpg->dinfo), TRUE);
  jpg->global.width = jpg->dinfo.image_width;
  jpg->global.height = jpg->dinfo.image_height;
  return 0;
}

int read_image(dt_imageio_module_data_t *jpg_tmp,
               uint8_t *out)
{
  dt_imageio_jpeg_t *jpg = (dt_imageio_jpeg_t *)jpg_tmp;
  struct dt_imageio_jpeg_error_mgr jerr;
  jpg->dinfo.err = jpeg_std_error(&jerr.pub);
  jerr.pub.error_exit = dt_imageio_jpeg_error_exit;
  if(setjmp(jerr.setjmp_buffer))
  {
    jpeg_destroy_decompress(&(jpg->dinfo));
    fclose(jpg->f);
    return 1;
  }
  (void)jpeg_start_decompress(&(jpg->dinfo));
  JSAMPROW row_pointer[1];
  row_pointer[0] = (uint8_t *)dt_alloc_aligned((size_t)jpg->dinfo.output_width * jpg->dinfo.num_components);
  uint8_t *tmp = out;
  while(row_pointer[0] && jpg->dinfo.output_scanline < jpg->dinfo.image_height)
  {
    if(jpeg_read_scanlines(&(jpg->dinfo), row_pointer, 1) != 1)
      return 1;

    if(jpg->dinfo.num_components < 3)
      for(JDIMENSION i = 0; i < jpg->dinfo.image_width; i++)
        for(int k = 0; k < 3; k++)
          tmp[4 * i + k] = row_pointer[0][jpg->dinfo.num_components * i + 0];
    else
      for(JDIMENSION i = 0; i < jpg->dinfo.image_width; i++)
        for(int k = 0; k < 3; k++) tmp[4 * i + k] = row_pointer[0][3 * i + k];
    tmp += 4 * jpg->global.width;
  }
  if(setjmp(jerr.setjmp_buffer))
  {
    jpeg_destroy_decompress(&(jpg->dinfo));
    dt_free_align(row_pointer[0]);
    fclose(jpg->f);
    return 1;
  }
  (void)jpeg_finish_decompress(&(jpg->dinfo));
  jpeg_destroy_decompress(&(jpg->dinfo));
  dt_free_align(row_pointer[0]);
  fclose(jpg->f);
  return 0;
}

size_t params_size(dt_imageio_module_format_t *self)
{
  return sizeof(dt_imageio_module_data_t)
    + sizeof(int)
    + sizeof(dt_imageio_jpeg_subsample_t);
}

void *legacy_params(dt_imageio_module_format_t *self,
                    const void *const old_params,
                    const size_t old_params_size,
                    const int old_version,
                    int *new_version,
                    size_t *new_size)
{
  typedef struct dt_imageio_jpeg_v2_t
  {
    dt_imageio_module_data_t global;
    int quality;
    struct jpeg_source_mgr src;
    struct jpeg_destination_mgr dest;
    struct jpeg_decompress_struct dinfo;
    struct jpeg_compress_struct cinfo;
    FILE *f;
  } dt_imageio_jpeg_v2_t;

  if(old_version == 1)
  {
    typedef struct dt_imageio_jpeg_v1_t
    {
      int max_width, max_height;
      int width, height;
      char style[128];
      int quality;
      struct jpeg_source_mgr src;
      struct jpeg_destination_mgr dest;
      struct jpeg_decompress_struct dinfo;
      struct jpeg_compress_struct cinfo;
      FILE *f;
    } dt_imageio_jpeg_v1_t;

    const dt_imageio_jpeg_v1_t *o = (dt_imageio_jpeg_v1_t *)old_params;
    dt_imageio_jpeg_v2_t *n = malloc(sizeof(dt_imageio_jpeg_v2_t));

    n->global.max_width = o->max_width;
    n->global.max_height = o->max_height;
    n->global.width = o->width;
    n->global.height = o->height;
    g_strlcpy(n->global.style, o->style, sizeof(o->style));
    n->global.style_append = FALSE;
    n->quality = o->quality;
    n->src = o->src;
    n->dest = o->dest;
    n->dinfo = o->dinfo;
    n->cinfo = o->cinfo;
    n->f = o->f;

    *new_version = 2;
    *new_size = sizeof(dt_imageio_module_data_t) + sizeof(int);
    return n;
  }

  typedef struct dt_imageio_jpeg_v3_t
  {
    dt_imageio_module_data_t global;
    int quality;
    dt_imageio_jpeg_subsample_t subsample;
    struct jpeg_source_mgr src;
    struct jpeg_destination_mgr dest;
    struct jpeg_decompress_struct dinfo;
    struct jpeg_compress_struct cinfo;
    FILE *f;
  } dt_imageio_jpeg_v3_t;

  if(old_version == 2)
  {
    const dt_imageio_jpeg_v2_t *o = (dt_imageio_jpeg_v2_t *)old_params;
    dt_imageio_jpeg_v3_t *n = malloc(sizeof(dt_imageio_jpeg_v3_t));

    n->global.max_width = o->global.max_width;
    n->global.max_height = o->global.max_height;
    n->global.width = o->global.width;
    n->global.height = o->global.height;
    g_strlcpy(n->global.style, o->global.style, sizeof(o->global.style));
    n->global.style_append = o->global.style_append;
    n->quality = o->quality;
    n->subsample = DT_SUBSAMPLE_AUTO;
    n->src = o->src;
    n->dest = o->dest;
    n->dinfo = o->dinfo;
    n->cinfo = o->cinfo;
    n->f = o->f;

    *new_version = 3;
    *new_size = sizeof(dt_imageio_module_data_t)
      + sizeof(int) + sizeof(dt_imageio_jpeg_subsample_t);
    return n;
  }

  return NULL;
}

void *get_params(dt_imageio_module_format_t *self)
{
  // adjust this if more params are stored (subsampling etc)
  dt_imageio_jpeg_t *d = calloc(1, sizeof(dt_imageio_jpeg_t));
  d->quality = dt_conf_get_int("plugins/imageio/format/jpeg/quality");
  d->subsample = dt_conf_get_int("plugins/imageio/format/jpeg/subsample");
  return d;
}

void free_params(dt_imageio_module_format_t *self,
                 dt_imageio_module_data_t *params)
{
  free(params);
}

int set_params(dt_imageio_module_format_t *self,
               const void *params,
               const int size)
{
  if(size != self->params_size(self)) return 1;
  const dt_imageio_jpeg_t *d = (dt_imageio_jpeg_t *)params;
  dt_imageio_jpeg_gui_data_t *g = self->gui_data;
  dt_bauhaus_slider_set(g->quality, d->quality);
  dt_bauhaus_combobox_set(g->subsample, d->subsample);
  return 0;
}

int dimension(struct dt_imageio_module_format_t *self,
              struct dt_imageio_module_data_t *data,
              uint32_t *width,
              uint32_t *height)
{
  /* maximum dimensions supported by JPEG images */
  *width = 65535U;
  *height = 65535U;
  return 1;
}

int bpp(dt_imageio_module_data_t *p)
{
  return 8;
}

int levels(dt_imageio_module_data_t *p)
{
  return IMAGEIO_RGB | IMAGEIO_INT8;
}

const char *mime(dt_imageio_module_data_t *data)
{
  return "image/jpeg";
}

const char *extension(dt_imageio_module_data_t *data)
{
  return "jpg";
}

int flags(dt_imageio_module_data_t *data)
{
  return FORMAT_FLAGS_SUPPORT_XMP;
}

void init(dt_imageio_module_format_t *self)
{
#ifdef USE_LUA
  dt_lua_register_module_member(darktable.lua_state.state, self,
                                dt_imageio_jpeg_t, quality, int);
#endif
}
void cleanup(dt_imageio_module_format_t *self)
{
}

// =============================================================================
//  gui stuff:
// =============================================================================

const char *name()
{
  return _("JPEG (8-bit)");
}

static void quality_changed(GtkWidget *slider,
                            gpointer user_data)
{
  const int quality = (int)dt_bauhaus_slider_get(slider);
  dt_conf_set_int("plugins/imageio/format/jpeg/quality", quality);
}

static void subsample_combobox_changed(GtkWidget *widget,
                                       gpointer user_data)
{
  const dt_imageio_jpeg_subsample_t subsample = dt_bauhaus_combobox_get(widget);
  dt_conf_set_int("plugins/imageio/format/jpeg/subsample", subsample);
}

void gui_init(dt_imageio_module_format_t *self)
{
  dt_imageio_jpeg_gui_data_t *g = malloc(sizeof(dt_imageio_jpeg_gui_data_t));
  self->gui_data = g;

  const dt_imageio_jpeg_subsample_t subsample =
    dt_conf_get_int("plugins/imageio/format/jpeg/subsample");

  // construct gui with jpeg specific options:
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  self->widget = box;
  // quality slider
  g->quality = dt_bauhaus_slider_new_with_range
    ((dt_iop_module_t*)self,
     dt_confgen_get_int("plugins/imageio/format/jpeg/quality", DT_MIN),
     dt_confgen_get_int("plugins/imageio/format/jpeg/quality", DT_MAX),
     1,
     dt_confgen_get_int("plugins/imageio/format/jpeg/quality", DT_DEFAULT),
     0);

  dt_bauhaus_widget_set_label(g->quality, NULL, N_("quality"));
  dt_bauhaus_slider_set(g->quality, dt_conf_get_int("plugins/imageio/format/jpeg/quality"));
  gtk_box_pack_start(GTK_BOX(box), GTK_WIDGET(g->quality), TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(g->quality), "value-changed",
                   G_CALLBACK(quality_changed), NULL);

  DT_BAUHAUS_COMBOBOX_NEW_FULL
    (g->subsample,
     self,
     NULL,
     N_("chroma subsampling"),
     _("chroma subsampling setting for JPEG encoder.\n"
       "auto - use subsampling determined by the quality value\n"
       "4:4:4 - no chroma subsampling\n"
       "4:4:0 - color sampling rate halved vertically\n"
       "4:2:2 - color sampling rate halved horizontally\n"
       "4:2:0 - color sampling rate halved horizontally and vertically"),
     subsample,
     subsample_combobox_changed,
     self,
     N_("auto"), N_("4:4:4"), N_("4:4:0"), N_("4:2:2"), N_("4:2:0"));

  gtk_box_pack_start(GTK_BOX(box), g->subsample, TRUE, TRUE, 0);

}

void gui_cleanup(dt_imageio_module_format_t *self)
{
  free(self->gui_data);
}

void gui_reset(dt_imageio_module_format_t *self)
{
  dt_imageio_jpeg_gui_data_t *g = self->gui_data;
  dt_bauhaus_slider_set(g->quality,
                        dt_confgen_get_int("plugins/imageio/format/jpeg/quality",
                                           DT_DEFAULT));
  dt_bauhaus_combobox_set(g->subsample, DT_SUBSAMPLE_AUTO);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
