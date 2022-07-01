/*
    This file is part of darktable,
    Copyright (C) 2010-2021 darktable developers.

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
#include "common/imageio.h"
#include "common/imageio_module.h"
#include "control/conf.h"
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

DT_MODULE(2)

typedef struct dt_imageio_jpeg_t
{
  dt_imageio_module_data_t global;
  int quality;
  struct jpeg_source_mgr src;
  struct jpeg_destination_mgr dest;
  struct jpeg_decompress_struct dinfo;
  struct jpeg_compress_struct cinfo;
  FILE *f;
} dt_imageio_jpeg_t;

typedef struct dt_imageio_jpeg_gui_data_t
{
  GtkWidget *quality;
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

static void write_icc_profile(j_compress_ptr cinfo, const JOCTET *icc_data_ptr, unsigned int icc_data_len)
{
  int cur_marker = 1;       /* per spec, counting starts at 1 */

  /* Calculate the number of markers we'll need, rounding up of course */
  unsigned int num_markers = icc_data_len / MAX_DATA_BYTES_IN_MARKER;
  if(num_markers * MAX_DATA_BYTES_IN_MARKER != icc_data_len) num_markers++;

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


#if 0
/*
 * Prepare for reading an ICC profile
 */

void
setup_read_icc_profile (j_decompress_ptr cinfo)
{
  /* Tell the library to keep any APP2 data it may find */
  jpeg_save_markers(cinfo, ICC_MARKER, 0xFFFF);
}


/*
 * Handy subroutine to test whether a saved marker is an ICC profile marker.
 */

static boolean
marker_is_icc (jpeg_saved_marker_ptr marker)
{
  return
    marker->marker == ICC_MARKER &&
    marker->data_length >= ICC_OVERHEAD_LEN &&
    /* verify the identifying string */
    GETJOCTET(marker->data[0]) == 0x49 &&
    GETJOCTET(marker->data[1]) == 0x43 &&
    GETJOCTET(marker->data[2]) == 0x43 &&
    GETJOCTET(marker->data[3]) == 0x5F &&
    GETJOCTET(marker->data[4]) == 0x50 &&
    GETJOCTET(marker->data[5]) == 0x52 &&
    GETJOCTET(marker->data[6]) == 0x4F &&
    GETJOCTET(marker->data[7]) == 0x46 &&
    GETJOCTET(marker->data[8]) == 0x49 &&
    GETJOCTET(marker->data[9]) == 0x4C &&
    GETJOCTET(marker->data[10]) == 0x45 &&
    GETJOCTET(marker->data[11]) == 0x0;
}


/*
 * See if there was an ICC profile in the JPEG file being read;
 * if so, reassemble and return the profile data.
 *
 * TRUE is returned if an ICC profile was found, FALSE if not.
 * If TRUE is returned, *icc_data_ptr is set to point to the
 * returned data, and *icc_data_len is set to its length.
 *
 * IMPORTANT: the data at **icc_data_ptr has been allocated with malloc()
 * and must be freed by the caller with free() when the caller no longer
 * needs it.  (Alternatively, we could write this routine to use the
 * IJG library's memory allocator, so that the data would be freed implicitly
 * at jpeg_finish_decompress() time.  But it seems likely that many apps
 * will prefer to have the data stick around after decompression finishes.)
 *
 * NOTE: if the file contains invalid ICC APP2 markers, we just silently
 * return FALSE.  You might want to issue an error message instead.
 */

boolean
read_icc_profile (j_decompress_ptr cinfo,
                  JOCTET **icc_data_ptr,
                  unsigned int *icc_data_len)
{
  int num_markers = 0;
#define MAX_SEQ_NO 255 /* sufficient since marker numbers are bytes */
  char marker_present[MAX_SEQ_NO+1];    /* 1 if marker found */
  unsigned int data_length[MAX_SEQ_NO+1]; /* size of profile data in marker */
  unsigned int data_offset[MAX_SEQ_NO+1]; /* offset for data in marker */

  *icc_data_ptr = NULL;   /* avoid confusion if FALSE return */
  *icc_data_len = 0;

  /* This first pass over the saved markers discovers whether there are
   * any ICC markers and verifies the consistency of the marker numbering.
   */

  for(int seq_no = 1; seq_no <= MAX_SEQ_NO; seq_no++)
    marker_present[seq_no] = 0;

  for(jpeg_saved_marker_ptr marker = cinfo->marker_list; marker != NULL; marker = marker->next)
  {
    if(marker_is_icc(marker))
    {
      if(num_markers == 0)
        num_markers = GETJOCTET(marker->data[13]);
      else if(num_markers != GETJOCTET(marker->data[13]))
        return FALSE;   /* inconsistent num_markers fields */
      const int seq_no = GETJOCTET(marker->data[12]);
      if(seq_no <= 0 || seq_no > num_markers)
        return FALSE;   /* bogus sequence number */
      if(marker_present[seq_no])
        return FALSE;   /* duplicate sequence numbers */
      marker_present[seq_no] = 1;
      data_length[seq_no] = marker->data_length - ICC_OVERHEAD_LEN;
    }
  }

  if(num_markers == 0)
    return FALSE;

  /* Check for missing markers, count total space needed,
   * compute offset of each marker's part of the data.
   */

  unsigned int total_length = 0;
  for(int seq_no = 1; seq_no <= num_markers; seq_no++)
  {
    if(marker_present[seq_no] == 0)
      return FALSE;   /* missing sequence number */
    data_offset[seq_no] = total_length;
    total_length += data_length[seq_no];
  }

  if(total_length <= 0)
    return FALSE;   /* found only empty markers? */

  /* Allocate space for assembled data */
  JOCTET *icc_data = (JOCTET *)calloc(total_length, sizeof(JOCTET));
  if(icc_data == NULL)
    return FALSE;   /* oops, out of memory */

  /* and fill it in */
  for(jpeg_saved_marker_ptr marker = cinfo->marker_list; marker != NULL; marker = marker->next)
  {
    if(marker_is_icc(marker))
    {
      const int seq_no = GETJOCTET(marker->data[12]);
      JOCTET *dst_ptr = icc_data + data_offset[seq_no];
      JOCTET FAR *src_ptr = marker->data + ICC_OVERHEAD_LEN;
      unsigned int length = data_length[seq_no];
      while (length--)
      {
        *dst_ptr++ = *src_ptr++;
      }
    }
  }

  *icc_data_ptr = icc_data;
  *icc_data_len = total_length;

  return TRUE;
}
#endif
#undef ICC_MARKER
#undef ICC_OVERHEAD_LEN
#undef MAX_BYTES_IN_MARKER
#undef MAX_DATA_BYTES_IN_MARKER
#undef MAX_SEQ_NO


int write_image(dt_imageio_module_data_t *jpg_tmp, const char *filename, const void *in_tmp,
                dt_colorspaces_color_profile_type_t over_type, const char *over_filename,
                void *exif, int exif_len, int imgid, int num, int total, struct dt_dev_pixelpipe_t *pipe,
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

  const int resolution = dt_conf_get_int("metadata/resolution");
  jpg->cinfo.density_unit = 1;
  jpg->cinfo.X_density = resolution;
  jpg->cinfo.Y_density = resolution;

  jpeg_start_compress(&(jpg->cinfo), TRUE);

  cmsHPROFILE out_profile = dt_colorspaces_get_output_profile(imgid, over_type, over_filename)->profile;
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

  uint8_t *row = dt_alloc_align(64, sizeof(uint8_t) * 3 * jpg->global.width);
  const uint8_t *buf;
  while(jpg->cinfo.next_scanline < jpg->cinfo.image_height)
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

  dt_exif_write_blob(exif, exif_len, filename, 1);

  return 0;
}

static int __attribute__((__unused__)) read_header(const char *filename, dt_imageio_jpeg_t *jpg)
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

int read_image(dt_imageio_module_data_t *jpg_tmp, uint8_t *out)
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
  row_pointer[0] = (uint8_t *)dt_alloc_align(64, (size_t)jpg->dinfo.output_width * jpg->dinfo.num_components);
  uint8_t *tmp = out;
  while(jpg->dinfo.output_scanline < jpg->dinfo.image_height)
  {
    if(jpeg_read_scanlines(&(jpg->dinfo), row_pointer, 1) != 1) return 1;
    if(jpg->dinfo.num_components < 3)
      for(JDIMENSION i = 0; i < jpg->dinfo.image_width; i++)
        for(int k = 0; k < 3; k++) tmp[4 * i + k] = row_pointer[0][jpg->dinfo.num_components * i + 0];
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
  return sizeof(dt_imageio_module_data_t) + sizeof(int);
}

void *legacy_params(dt_imageio_module_format_t *self, const void *const old_params,
                    const size_t old_params_size, const int old_version, const int new_version,
                    size_t *new_size)
{
  if(old_version == 1 && new_version == 2)
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
    dt_imageio_jpeg_t *n = (dt_imageio_jpeg_t *)malloc(sizeof(dt_imageio_jpeg_t));

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
    *new_size = self->params_size(self);
    return n;
  }
  return NULL;
}

void *get_params(dt_imageio_module_format_t *self)
{
  // adjust this if more params are stored (subsampling etc)
  dt_imageio_jpeg_t *d = (dt_imageio_jpeg_t *)calloc(1, sizeof(dt_imageio_jpeg_t));
  d->quality = dt_conf_get_int("plugins/imageio/format/jpeg/quality");
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
  const dt_imageio_jpeg_t *d = (dt_imageio_jpeg_t *)params;
  dt_imageio_jpeg_gui_data_t *g = (dt_imageio_jpeg_gui_data_t *)self->gui_data;
  dt_bauhaus_slider_set(g->quality, d->quality);
  return 0;
}

int dimension(struct dt_imageio_module_format_t *self, struct dt_imageio_module_data_t *data, uint32_t *width,
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
  dt_lua_register_module_member(darktable.lua_state.state, self, dt_imageio_jpeg_t, quality, int);
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

static void quality_changed(GtkWidget *slider, gpointer user_data)
{
  const int quality = (int)dt_bauhaus_slider_get(slider);
  dt_conf_set_int("plugins/imageio/format/jpeg/quality", quality);
}

void gui_init(dt_imageio_module_format_t *self)
{
  dt_imageio_jpeg_gui_data_t *g = (dt_imageio_jpeg_gui_data_t *)malloc(sizeof(dt_imageio_jpeg_gui_data_t));
  self->gui_data = g;
  // construct gui with jpeg specific options:
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  self->widget = box;
  // quality slider
  g->quality = dt_bauhaus_slider_new_with_range(NULL,
                                                dt_confgen_get_int("plugins/imageio/format/jpeg/quality", DT_MIN),
                                                dt_confgen_get_int("plugins/imageio/format/jpeg/quality", DT_MAX),
                                                1,
                                                dt_confgen_get_int("plugins/imageio/format/jpeg/quality", DT_DEFAULT),
                                                0);
  dt_bauhaus_widget_set_label(g->quality, NULL, N_("quality"));
  dt_bauhaus_slider_set_default(g->quality, dt_confgen_get_int("plugins/imageio/format/jpeg/quality", DT_DEFAULT));
  dt_bauhaus_slider_set(g->quality, dt_conf_get_int("plugins/imageio/format/jpeg/quality"));
  gtk_box_pack_start(GTK_BOX(box), GTK_WIDGET(g->quality), TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(g->quality), "value-changed", G_CALLBACK(quality_changed), NULL);
  // TODO: add more options: subsample dreggn
}

void gui_cleanup(dt_imageio_module_format_t *self)
{
  free(self->gui_data);
}

void gui_reset(dt_imageio_module_format_t *self)
{
  dt_imageio_jpeg_gui_data_t *g = (dt_imageio_jpeg_gui_data_t *)self->gui_data;
  dt_bauhaus_slider_set(g->quality, dt_confgen_get_int("plugins/imageio/format/jpeg/quality", DT_DEFAULT));
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
