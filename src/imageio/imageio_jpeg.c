/*
    This file is part of darktable,
    Copyright (C) 2009-2023 darktable developers.

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
#include "common/exif.h"
#include "imageio/imageio.h"
#include "imageio/imageio_jpeg.h"

#include <setjmp.h>

// error functions

struct dt_imageio_jpeg_error_mgr
{
  struct jpeg_error_mgr pub;
  jmp_buf setjmp_buffer;
} dt_imageio_jpeg_error_mgr;

typedef struct dt_imageio_jpeg_error_mgr *dt_imageio_jpeg_error_ptr;

static void dt_imageio_jpeg_error_exit(j_common_ptr cinfo)
{
  dt_imageio_jpeg_error_ptr myerr = (dt_imageio_jpeg_error_ptr)cinfo->err;
  (*cinfo->err->output_message)(cinfo);
  longjmp(myerr->setjmp_buffer, 1);
}

// destination functions
static void dt_imageio_jpeg_init_destination(j_compress_ptr cinfo)
{
}
static boolean dt_imageio_jpeg_empty_output_buffer(j_compress_ptr cinfo)
{
  fprintf(stderr, "[imageio_jpeg] output buffer full!\n");
  return FALSE;
}
static void dt_imageio_jpeg_term_destination(j_compress_ptr cinfo)
{
}

// source functions
static void dt_imageio_jpeg_init_source(j_decompress_ptr cinfo)
{
}
static boolean dt_imageio_jpeg_fill_input_buffer(j_decompress_ptr cinfo)
{
  return 1;
}
static void dt_imageio_jpeg_skip_input_data(j_decompress_ptr cinfo, long num_bytes)
{
  ssize_t i = cinfo->src->bytes_in_buffer - num_bytes;
  if(i < 0) i = 0;
  cinfo->src->bytes_in_buffer = i;
  cinfo->src->next_input_byte += num_bytes;
}
static void dt_imageio_jpeg_term_source(j_decompress_ptr cinfo)
{
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

#define EXIF_MARKER (JPEG_APP0 + 1) /* JPEG marker code for Exif */
#define ICC_MARKER (JPEG_APP0 + 2)  /* JPEG marker code for ICC */
#define ICC_OVERHEAD_LEN 14         /* size of non-profile data in APP2 */
#define MAX_BYTES_IN_MARKER 65533   /* maximum data len of a JPEG marker */
#define MAX_DATA_BYTES_IN_MARKER (MAX_BYTES_IN_MARKER - ICC_OVERHEAD_LEN)


/*
 * Prepare for reading an ICC profile
 */

static void setup_read_icc_profile(j_decompress_ptr cinfo)
{
  /* Tell the library to keep any APP2 data it may find */
  jpeg_save_markers(cinfo, ICC_MARKER, 0xFFFF);
}

/*
 * Prepare for reading an Exif blob
 */

static void setup_read_exif(j_decompress_ptr cinfo)
{
  /* Tell the library to keep any APP1 data it may find */
  jpeg_save_markers(cinfo, EXIF_MARKER, 0xFFFF);
}


int dt_imageio_jpeg_decompress_header(const void *in, size_t length, dt_imageio_jpeg_t *jpg)
{
  jpeg_create_decompress(&(jpg->dinfo));
  jpg->src.init_source = dt_imageio_jpeg_init_source;
  jpg->src.fill_input_buffer = dt_imageio_jpeg_fill_input_buffer;
  jpg->src.skip_input_data = dt_imageio_jpeg_skip_input_data;
  jpg->src.resync_to_restart = jpeg_resync_to_restart;
  jpg->src.term_source = dt_imageio_jpeg_term_source;
  jpg->src.next_input_byte = (JOCTET *)in;
  jpg->src.bytes_in_buffer = length;

  struct dt_imageio_jpeg_error_mgr jerr;
  jpg->dinfo.err = jpeg_std_error(&jerr.pub);
  jerr.pub.error_exit = dt_imageio_jpeg_error_exit;
  if(setjmp(jerr.setjmp_buffer))
  {
    jpeg_destroy_decompress(&(jpg->dinfo));
    return 1;
  }

  jpg->dinfo.src = &(jpg->src);
  setup_read_exif(&(jpg->dinfo));
  setup_read_icc_profile(&(jpg->dinfo));
  jpeg_read_header(&(jpg->dinfo), TRUE);
#ifdef JCS_EXTENSIONS
  jpg->dinfo.out_color_space = JCS_EXT_RGBX;
  jpg->dinfo.out_color_components = 4;
#else
  jpg->dinfo.out_color_space = JCS_RGB;
  jpg->dinfo.out_color_components = 3;
#endif
  // jpg->dinfo.buffered_image = TRUE;
  jpg->width = jpg->dinfo.image_width;
  jpg->height = jpg->dinfo.image_height;
  return 0;
}

#ifdef JCS_EXTENSIONS
static int decompress_jsc(dt_imageio_jpeg_t *jpg, uint8_t *out)
{
  uint8_t *tmp = out;
  while(jpg->dinfo.output_scanline < jpg->dinfo.image_height)
  {
    if(jpeg_read_scanlines(&(jpg->dinfo), &tmp, 1) != 1)
    {
      return 1;
    }
    tmp += 4 * jpg->width;
  }
  return 0;
}
#endif

static int decompress_plain(dt_imageio_jpeg_t *jpg, uint8_t *out)
{
  JSAMPROW row_pointer[1];
  row_pointer[0] = (uint8_t *)dt_alloc_align(64, (size_t)jpg->dinfo.output_width * jpg->dinfo.num_components);
  uint8_t *tmp = out;
  while(jpg->dinfo.output_scanline < jpg->dinfo.image_height)
  {
    if(jpeg_read_scanlines(&(jpg->dinfo), row_pointer, 1) != 1)
    {
      dt_free_align(row_pointer[0]);
      return 1;
    }
    for(unsigned int i = 0; i < jpg->dinfo.image_width; i++)
    {
      for(int k = 0; k < 3; k++) tmp[4 * i + k] = row_pointer[0][3 * i + k];
    }
    tmp += 4 * jpg->width;
  }
  dt_free_align(row_pointer[0]);
  return 0;
}

int dt_imageio_jpeg_decompress(dt_imageio_jpeg_t *jpg, uint8_t *out)
{
  struct dt_imageio_jpeg_error_mgr jerr;
  jpg->dinfo.err = jpeg_std_error(&jerr.pub);
  jerr.pub.error_exit = dt_imageio_jpeg_error_exit;
  if(setjmp(jerr.setjmp_buffer))
  {
    jpeg_destroy_decompress(&(jpg->dinfo));
    return 1;
  }

#ifdef JCS_EXTENSIONS
  /*
   * Do a run-time detection for JCS_EXTENSIONS:
   * it might have been only available at build-time
   */
  int jcs_alpha_valid = 1;
  if(setjmp(jerr.setjmp_buffer))
  {
    if(jpg->dinfo.out_color_space == JCS_EXT_RGBX && jpg->dinfo.out_color_components == 4)
    {
      // ok, no JCS_EXTENSIONS, fall-back to slow plain code.
      jpg->dinfo.out_color_components = 3;
      jpg->dinfo.out_color_space = JCS_RGB;
      jcs_alpha_valid = 0;
    }
    else
    {
      jpeg_destroy_decompress(&(jpg->dinfo));
      return 1;
    }
  }
#endif

  (void)jpeg_start_decompress(&(jpg->dinfo));

  if(setjmp(jerr.setjmp_buffer))
  {
    jpeg_destroy_decompress(&(jpg->dinfo));
    return 1;
  }

#ifdef JCS_EXTENSIONS
  if(jcs_alpha_valid)
  {
    if(decompress_jsc(jpg, out)) return 1;
  }
  else
  {
    if(decompress_plain(jpg, out)) return 1;
  }
#else
  if(decompress_plain(jpg, out)) return 1;
#endif

  if(setjmp(jerr.setjmp_buffer))
  {
    jpeg_destroy_decompress(&(jpg->dinfo));
    return 1;
  }

  // jpg->dinfo.src = NULL;
  (void)jpeg_finish_decompress(&(jpg->dinfo));
  jpeg_destroy_decompress(&(jpg->dinfo));
  return 0;
}

int dt_imageio_jpeg_compress(const uint8_t *in, uint8_t *out, const int width, const int height,
                             const int quality)
{
  struct dt_imageio_jpeg_error_mgr jerr;
  dt_imageio_jpeg_t jpg;
  jpg.dest.init_destination = dt_imageio_jpeg_init_destination;
  jpg.dest.empty_output_buffer = dt_imageio_jpeg_empty_output_buffer;
  jpg.dest.term_destination = dt_imageio_jpeg_term_destination;
  jpg.dest.next_output_byte = (JOCTET *)out;
  jpg.dest.free_in_buffer = sizeof(uint8_t) * 4 * width * height;

  jpg.cinfo.err = jpeg_std_error(&jerr.pub);
  jerr.pub.error_exit = dt_imageio_jpeg_error_exit;
  if(setjmp(jerr.setjmp_buffer))
  {
    jpeg_destroy_compress(&(jpg.cinfo));
    return 1;
  }
  jpeg_create_compress(&(jpg.cinfo));
  jpg.cinfo.dest = &(jpg.dest);

  jpg.cinfo.image_width = width;
  jpg.cinfo.image_height = height;
  jpg.cinfo.input_components = 3;
  jpg.cinfo.in_color_space = JCS_RGB;
  jpeg_set_defaults(&(jpg.cinfo));
  jpeg_set_quality(&(jpg.cinfo), quality, TRUE);
  if(quality > 90) jpg.cinfo.comp_info[0].v_samp_factor = 1;
  if(quality > 92) jpg.cinfo.comp_info[0].h_samp_factor = 1;
  jpeg_start_compress(&(jpg.cinfo), TRUE);
  uint8_t *row = dt_alloc_align(64, sizeof(uint8_t) * 3 * width);
  const uint8_t *buf;
  while(jpg.cinfo.next_scanline < jpg.cinfo.image_height)
  {
    JSAMPROW tmp[1];
    buf = in + jpg.cinfo.next_scanline * jpg.cinfo.image_width * 4;
    for(int i = 0; i < width; i++)
      for(int k = 0; k < 3; k++) row[3 * i + k] = buf[4 * i + k];
    tmp[0] = row;
    jpeg_write_scanlines(&(jpg.cinfo), tmp, 1);
  }
  jpeg_finish_compress(&(jpg.cinfo));
  dt_free_align(row);
  jpeg_destroy_compress(&(jpg.cinfo));
  return sizeof(uint8_t) * 4 * width * height - jpg.dest.free_in_buffer;
}


/*
 * This routine writes the given ICC profile data into a JPEG file.
 * It *must* be called AFTER calling jpeg_start_compress() and BEFORE
 * the first call to jpeg_write_scanlines().
 * (This ordering ensures that the APP2 marker(s) will appear after the
 * SOI and JFIF or Adobe markers, but before all else.)
 */

static void write_icc_profile(j_compress_ptr cinfo, const JOCTET *icc_data_ptr, unsigned int icc_data_len)
{
  unsigned int num_markers; /* total number of markers we'll write */
  int cur_marker = 1;       /* per spec, counting starts at 1 */

  /* Calculate the number of markers we'll need, rounding up of course */
  num_markers = icc_data_len / MAX_DATA_BYTES_IN_MARKER;
  if(num_markers * MAX_DATA_BYTES_IN_MARKER != icc_data_len) num_markers++;

  while(icc_data_len > 0)
  {
    /* length of profile to put in this marker */
    unsigned int length = icc_data_len;
    if(length > MAX_DATA_BYTES_IN_MARKER) length = MAX_DATA_BYTES_IN_MARKER;
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


/*
 * Handy subroutine to test whether a saved marker is an ICC profile marker.
 */

static boolean marker_is_icc(jpeg_saved_marker_ptr marker)
{
  return marker->marker == ICC_MARKER && marker->data_length >= ICC_OVERHEAD_LEN
         &&
         /* verify the identifying string */
         GETJOCTET(marker->data[0]) == 0x49 && GETJOCTET(marker->data[1]) == 0x43
         && GETJOCTET(marker->data[2]) == 0x43 && GETJOCTET(marker->data[3]) == 0x5F
         && GETJOCTET(marker->data[4]) == 0x50 && GETJOCTET(marker->data[5]) == 0x52
         && GETJOCTET(marker->data[6]) == 0x4F && GETJOCTET(marker->data[7]) == 0x46
         && GETJOCTET(marker->data[8]) == 0x49 && GETJOCTET(marker->data[9]) == 0x4C
         && GETJOCTET(marker->data[10]) == 0x45 && GETJOCTET(marker->data[11]) == 0x0;
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

static boolean read_icc_profile(j_decompress_ptr dinfo, JOCTET **icc_data_ptr, unsigned int *icc_data_len)
{
  jpeg_saved_marker_ptr marker;
  int num_markers = 0;
  int seq_no;
  JOCTET *icc_data;
  unsigned int total_length;
#define MAX_SEQ_NO 255                      /* sufficient since marker numbers are bytes */
  char marker_present[MAX_SEQ_NO + 1];      /* 1 if marker found */
  unsigned int data_length[MAX_SEQ_NO + 1]; /* size of profile data in marker */
  unsigned int data_offset[MAX_SEQ_NO + 1]; /* offset for data in marker */

  *icc_data_ptr = NULL; /* avoid confusion if FALSE return */
  *icc_data_len = 0;

  /* This first pass over the saved markers discovers whether there are
   * any ICC markers and verifies the consistency of the marker numbering.
   */

  for(seq_no = 1; seq_no <= MAX_SEQ_NO; seq_no++) marker_present[seq_no] = 0;

  for(marker = dinfo->marker_list; marker != NULL; marker = marker->next)
  {
    if(marker_is_icc(marker))
    {
      if(num_markers == 0)
        num_markers = GETJOCTET(marker->data[13]);
      else if(num_markers != GETJOCTET(marker->data[13]))
        return FALSE; /* inconsistent num_markers fields */
      seq_no = GETJOCTET(marker->data[12]);
      if(seq_no <= 0 || seq_no > num_markers) return FALSE; /* bogus sequence number */
      if(marker_present[seq_no]) return FALSE;              /* duplicate sequence numbers */
      marker_present[seq_no] = 1;
      data_length[seq_no] = marker->data_length - ICC_OVERHEAD_LEN;
    }
  }

  if(num_markers == 0) return FALSE;

  /* Check for missing markers, count total space needed,
   * compute offset of each marker's part of the data.
   */

  total_length = 0;
  for(seq_no = 1; seq_no <= num_markers; seq_no++)
  {
    if(marker_present[seq_no] == 0) return FALSE; /* missing sequence number */
    data_offset[seq_no] = total_length;
    total_length += data_length[seq_no];
  }

  if(total_length == 0) return FALSE; /* found only empty markers? */

  /* Allocate space for assembled data */
  icc_data = (JOCTET *)g_malloc(total_length * sizeof(JOCTET));

  /* and fill it in */
  for(marker = dinfo->marker_list; marker != NULL; marker = marker->next)
  {
    if(marker_is_icc(marker))
    {
      JOCTET FAR *src_ptr;
      JOCTET *dst_ptr;
      unsigned int length;
      seq_no = GETJOCTET(marker->data[12]);
      dst_ptr = icc_data + data_offset[seq_no];
      src_ptr = marker->data + ICC_OVERHEAD_LEN;
      length = data_length[seq_no];
      while(length--)
      {
        *dst_ptr++ = *src_ptr++;
      }
    }
  }

  *icc_data_ptr = icc_data;
  *icc_data_len = total_length;

  return TRUE;
}
#undef ICC_MARKER
#undef ICC_OVERHEAD_LEN
#undef MAX_BYTES_IN_MARKER
#undef MAX_DATA_BYTES_IN_MARKER
#undef MAX_SEQ_NO


int dt_imageio_jpeg_write_with_icc_profile(const char *filename, const uint8_t *in, const int width,
                                           const int height, const int quality, const void *exif, int exif_len,
                                           int imgid)
{
  struct dt_imageio_jpeg_error_mgr jerr;
  dt_imageio_jpeg_t jpg;

  jpg.cinfo.err = jpeg_std_error(&jerr.pub);
  jerr.pub.error_exit = dt_imageio_jpeg_error_exit;
  if(setjmp(jerr.setjmp_buffer))
  {
    jpeg_destroy_compress(&(jpg.cinfo));
    return 1;
  }
  jpeg_create_compress(&(jpg.cinfo));
  FILE *f = g_fopen(filename, "wb");
  if(!f) return 1;
  jpeg_stdio_dest(&(jpg.cinfo), f);

  jpg.cinfo.image_width = width;
  jpg.cinfo.image_height = height;
  jpg.cinfo.input_components = 3;
  jpg.cinfo.in_color_space = JCS_RGB;
  jpeg_set_defaults(&(jpg.cinfo));
  jpeg_set_quality(&(jpg.cinfo), quality, TRUE);
  if(quality > 90) jpg.cinfo.comp_info[0].v_samp_factor = 1;
  if(quality > 92) jpg.cinfo.comp_info[0].h_samp_factor = 1;
  jpeg_start_compress(&(jpg.cinfo), TRUE);

  if(imgid > 0)
  {
    // the code in this block is never being used. should that ever change make sure to honour the
    // color profile overwriting the one set in colorout, too. dt_colorspaces_get_output_profile() doesn't do that!
    cmsHPROFILE out_profile = dt_colorspaces_get_output_profile(imgid, DT_COLORSPACE_NONE, "")->profile;
    uint32_t len = 0;
    cmsSaveProfileToMem(out_profile, 0, &len);
    if(len > 0)
    {
      unsigned char *buf = dt_alloc_align(64, sizeof(unsigned char) * len);
      cmsSaveProfileToMem(out_profile, buf, &len);
      write_icc_profile(&(jpg.cinfo), buf, len);
      dt_free_align(buf);
    }
  }

  if(exif && exif_len > 0 && exif_len < 65534) jpeg_write_marker(&(jpg.cinfo), JPEG_APP0 + 1, exif, exif_len);

  uint8_t *row = dt_alloc_align(64, sizeof(uint8_t) * 3 * width);
  const uint8_t *buf;
  while(jpg.cinfo.next_scanline < jpg.cinfo.image_height)
  {
    JSAMPROW tmp[1];
    buf = in + jpg.cinfo.next_scanline * jpg.cinfo.image_width * 4;
    for(int i = 0; i < width; i++)
      for(int k = 0; k < 3; k++) row[3 * i + k] = buf[4 * i + k];
    tmp[0] = row;
    jpeg_write_scanlines(&(jpg.cinfo), tmp, 1);
  }
  jpeg_finish_compress(&(jpg.cinfo));
  dt_free_align(row);
  jpeg_destroy_compress(&(jpg.cinfo));
  fclose(f);
  return 0;
}

int dt_imageio_jpeg_write(const char *filename, const uint8_t *in, const int width, const int height,
                          const int quality, const void *exif, int exif_len)
{
  return dt_imageio_jpeg_write_with_icc_profile(filename, in, width, height, quality, exif, exif_len, -1);
}

int dt_imageio_jpeg_read_header(const char *filename, dt_imageio_jpeg_t *jpg)
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
  setup_read_exif(&(jpg->dinfo));
  setup_read_icc_profile(&(jpg->dinfo));
  // jpg->dinfo.buffered_image = TRUE;
  jpeg_read_header(&(jpg->dinfo), TRUE);
#ifdef JCS_EXTENSIONS
  jpg->dinfo.out_color_space = JCS_EXT_RGBX;
  jpg->dinfo.out_color_components = 4;
#else
  jpg->dinfo.out_color_space = JCS_RGB;
  jpg->dinfo.out_color_components = 3;
#endif
  jpg->width = jpg->dinfo.image_width;
  jpg->height = jpg->dinfo.image_height;
  return 0;
}

#ifdef JCS_EXTENSIONS
static int read_jsc(dt_imageio_jpeg_t *jpg, uint8_t *out)
{
  uint8_t *tmp = out;
  while(jpg->dinfo.output_scanline < jpg->dinfo.image_height)
  {
    if(jpeg_read_scanlines(&(jpg->dinfo), &tmp, 1) != 1)
    {
      return 1;
    }
    tmp += 4 * jpg->width;
  }
  return 0;
}
#endif

static int read_plain(dt_imageio_jpeg_t *jpg, uint8_t *out)
{
  JSAMPROW row_pointer[1];
  row_pointer[0] = (uint8_t *)dt_alloc_align(64, (size_t)jpg->dinfo.output_width * jpg->dinfo.num_components);
  uint8_t *tmp = out;
  while(jpg->dinfo.output_scanline < jpg->dinfo.image_height)
  {
    if(jpeg_read_scanlines(&(jpg->dinfo), row_pointer, 1) != 1)
    {
      jpeg_destroy_decompress(&(jpg->dinfo));
      dt_free_align(row_pointer[0]);
      fclose(jpg->f);
      return 1;
    }
    for(unsigned int i = 0; i < jpg->dinfo.image_width; i++)
      for(int k = 0; k < 3; k++) tmp[4 * i + k] = row_pointer[0][3 * i + k];
    tmp += 4 * jpg->width;
  }
  dt_free_align(row_pointer[0]);
  return 0;
}

int dt_imageio_jpeg_read(dt_imageio_jpeg_t *jpg, uint8_t *out)
{
  struct dt_imageio_jpeg_error_mgr jerr;
  jpg->dinfo.err = jpeg_std_error(&jerr.pub);
  jerr.pub.error_exit = dt_imageio_jpeg_error_exit;
  if(setjmp(jerr.setjmp_buffer))
  {
    jpeg_destroy_decompress(&(jpg->dinfo));
    fclose(jpg->f);
    return 1;
  }

#ifdef JCS_EXTENSIONS
  /*
   * Do a run-time detection for JCS_EXTENSIONS:
   * it might have been only available at build-time
   */
  int jcs_alpha_valid = 1;
  if(setjmp(jerr.setjmp_buffer))
  {
    if(jpg->dinfo.out_color_space == JCS_EXT_RGBX && jpg->dinfo.out_color_components == 4)
    {
      // ok, no JCS_EXTENSIONS, fall-back to slow plain code.
      jpg->dinfo.out_color_components = 3;
      jpg->dinfo.out_color_space = JCS_RGB;
      jcs_alpha_valid = 0;
    }
    else
    {
      jpeg_destroy_decompress(&(jpg->dinfo));
      return 1;
    }
  }
#endif
  (void)jpeg_start_decompress(&(jpg->dinfo));

  if(setjmp(jerr.setjmp_buffer))
  {
    jpeg_destroy_decompress(&(jpg->dinfo));
    fclose(jpg->f);
    return 1;
  }

#ifdef JCS_EXTENSIONS
  if(jcs_alpha_valid)
  {
    read_jsc(jpg, out);
  }
  else
  {
    read_plain(jpg, out);
  }
#else
  read_plain(jpg, out);
#endif

  if(setjmp(jerr.setjmp_buffer))
  {
    jpeg_destroy_decompress(&(jpg->dinfo));
    fclose(jpg->f);
    return 1;
  }

  (void)jpeg_finish_decompress(&(jpg->dinfo));

  jpeg_destroy_decompress(&(jpg->dinfo));
  fclose(jpg->f);
  return 0;
}

int dt_imageio_jpeg_read_profile(dt_imageio_jpeg_t *jpg, uint8_t **out)
{
  unsigned int length = 0;
  boolean res = read_icc_profile(&(jpg->dinfo), out, &length);
  jpeg_destroy_decompress(&(jpg->dinfo));
  fclose(jpg->f);
  return res ? length : 0;
}

dt_colorspaces_color_profile_type_t dt_imageio_jpeg_read_color_space(dt_imageio_jpeg_t *jpg)
{
  for(jpeg_saved_marker_ptr marker = jpg->dinfo.marker_list; marker != NULL; marker = marker->next)
  {
    if(marker->marker == EXIF_MARKER && marker->data_length > 6)
      return dt_exif_get_color_space(marker->data + 6, marker->data_length - 6);
  }

  return DT_COLORSPACE_DISPLAY; // nothing embedded
}

dt_imageio_retval_t dt_imageio_open_jpeg(dt_image_t *img, const char *filename, dt_mipmap_buffer_t *mbuf)
{
  const char *ext = filename + strlen(filename);
  while(*ext != '.' && ext > filename) ext--;

  // JFIF ("JPEG File Interchange Format") has the same container as regular JPEG, only a different metadata
  // format (instead of the more common Exif metadata format)
  // See https://en.wikipedia.org/wiki/JPEG_File_Interchange_Format
  if(g_ascii_strcasecmp(ext, ".jpg") && g_ascii_strcasecmp(ext, ".jpeg") && g_ascii_strcasecmp(ext, ".jfif"))
    return DT_IMAGEIO_LOAD_FAILED;

  if(!img->exif_inited) (void)dt_exif_read(img, filename);

  dt_imageio_jpeg_t jpg;
  if(dt_imageio_jpeg_read_header(filename, &jpg)) return DT_IMAGEIO_LOAD_FAILED;
  img->width = jpg.width;
  img->height = jpg.height;

  uint8_t *tmp = (uint8_t *)dt_alloc_align(64, sizeof(uint8_t) * 4 * jpg.width * jpg.height);
  if(dt_imageio_jpeg_read(&jpg, tmp))
  {
    dt_free_align(tmp);
    return DT_IMAGEIO_LOAD_FAILED;
  }

  img->buf_dsc.channels = 4;
  img->buf_dsc.datatype = TYPE_FLOAT;
  void *buf = dt_mipmap_cache_alloc(mbuf, img);
  if(!buf)
  {
    dt_free_align(tmp);
    return DT_IMAGEIO_CACHE_FULL;
  }

  dt_imageio_flip_buffers_ui8_to_float((float *)buf, tmp, 0.0f, 255.0f, 4, jpg.width, jpg.height, jpg.width,
                                       jpg.height, 4 * jpg.width, 0);

  dt_free_align(tmp);

  img->loader = LOADER_JPEG;
  return DT_IMAGEIO_OK;
}



// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

