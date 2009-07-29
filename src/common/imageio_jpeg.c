
#ifdef HAVE_CONFIG_H
  #include "config.h"
#endif
#include "common/imageio_jpeg.h"
#include <setjmp.h>

// error functions

struct dt_imageio_jpeg_error_mgr
{
  struct jpeg_error_mgr pub;
  jmp_buf setjmp_buffer;
}
dt_imageio_jpeg_error_mgr;

typedef struct dt_imageio_jpeg_error_mgr *dt_imageio_jpeg_error_ptr;

void dt_imageio_jpeg_error_exit (j_common_ptr cinfo)
{
  dt_imageio_jpeg_error_ptr myerr = (dt_imageio_jpeg_error_ptr) cinfo->err;
  (*cinfo->err->output_message) (cinfo);
  longjmp(myerr->setjmp_buffer, 1);
}

// destination functions
void dt_imageio_jpeg_init_destination(j_compress_ptr cinfo) {}
boolean dt_imageio_jpeg_empty_output_buffer(j_compress_ptr cinfo)
{
  fprintf(stderr, "[imageio_jpeg] output buffer full!\n");
  return FALSE;
}
void dt_imageio_jpeg_term_destination(j_compress_ptr cinfo) {}

// source functions
void dt_imageio_jpeg_init_source(j_decompress_ptr cinfo) {}
boolean dt_imageio_jpeg_fill_input_buffer(j_decompress_ptr cinfo) { return 1; }
void dt_imageio_jpeg_skip_input_data(j_decompress_ptr cinfo, long num_bytes)
{
  int i = cinfo->src->bytes_in_buffer - num_bytes;
  if (i < 0) i = 0;
  cinfo->src->bytes_in_buffer = i;
  cinfo->src->next_input_byte += num_bytes;
}
void dt_imageio_jpeg_term_source(j_decompress_ptr cinfo) {}


int dt_imageio_jpeg_decompress_header(const void *in, size_t length, dt_imageio_jpeg_t *jpg)
{
  jpg->src.init_source = dt_imageio_jpeg_init_source;
  jpg->src.fill_input_buffer = dt_imageio_jpeg_fill_input_buffer;
  jpg->src.skip_input_data = dt_imageio_jpeg_skip_input_data;
  jpg->src.resync_to_restart = jpeg_resync_to_restart;
  jpg->src.term_source = dt_imageio_jpeg_term_source;
  jpg->src.next_input_byte = (JOCTET*)in;
  jpg->src.bytes_in_buffer = length;

  struct dt_imageio_jpeg_error_mgr jerr;
  jpg->dinfo.err = jpeg_std_error(&jerr.pub);
  jerr.pub.error_exit = dt_imageio_jpeg_error_exit;
  if (setjmp(jerr.setjmp_buffer))
  {
	  jpeg_destroy_decompress(&(jpg->dinfo));
    return 1;
  }

  jpeg_create_decompress(&(jpg->dinfo));
  jpg->dinfo.src = &(jpg->src);
  jpg->dinfo.buffered_image = TRUE;
  jpeg_read_header(&(jpg->dinfo), TRUE);
  jpg->width  = jpg->dinfo.image_width;
  jpg->height = jpg->dinfo.image_height;
  return 0;
}

int dt_imageio_jpeg_decompress(dt_imageio_jpeg_t *jpg, uint8_t *out)
{
  (void)jpeg_start_decompress(&(jpg->dinfo));
	JSAMPROW row_pointer[1];
	row_pointer[0] = (uint8_t *)malloc(jpg->dinfo.output_width*jpg->dinfo.num_components);
  uint8_t *tmp = out;
	while(jpg->dinfo.output_scanline < jpg->dinfo.image_height)
	{
		if(jpeg_read_scanlines(&(jpg->dinfo), row_pointer, 1) != 1) return 1;
		for(int i=0; i<jpg->dinfo.image_width;i++) for(int k=0;k<3;k++)
			tmp[4*i+k] = row_pointer[0][3*i+k];
    tmp += 4*jpg->width;
	}
  // jpg->dinfo.src = NULL;
	// (void)jpeg_finish_decompress(&(jpg->dinfo)); // ???
	jpeg_destroy_decompress(&(jpg->dinfo));
	free(row_pointer[0]);
  return 0;
}

int dt_imageio_jpeg_compress(const uint8_t *in, uint8_t *out, const int width, const int height, const int quality)
{
	struct dt_imageio_jpeg_error_mgr jerr;
  dt_imageio_jpeg_t jpg;
  jpg.dest.init_destination = dt_imageio_jpeg_init_destination;
  jpg.dest.empty_output_buffer= dt_imageio_jpeg_empty_output_buffer;
  jpg.dest.term_destination = dt_imageio_jpeg_term_destination;
  jpg.dest.next_output_byte = (JOCTET *)out;
  jpg.dest.free_in_buffer = 4*width*height*sizeof(uint8_t);

  jpg.cinfo.err = jpeg_std_error(&jerr.pub);
  jerr.pub.error_exit = dt_imageio_jpeg_error_exit;
  if (setjmp(jerr.setjmp_buffer))
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
  jpeg_set_linear_quality(&(jpg.cinfo), quality, TRUE);
	jpeg_start_compress(&(jpg.cinfo), TRUE);
  uint8_t row[3*width];
  const uint8_t *buf;
	while(jpg.cinfo.next_scanline < jpg.cinfo.image_height)
	{
		JSAMPROW tmp[1];
    buf = in + jpg.cinfo.next_scanline * jpg.cinfo.image_width * 4;
    for(int i=0;i<width;i++) for(int k=0;k<3;k++) row[3*i+k] = buf[4*i+k];
    tmp[0] = row;
		jpeg_write_scanlines(&(jpg.cinfo), tmp, 1);
	}
	jpeg_finish_compress (&(jpg.cinfo));
	jpeg_destroy_compress(&(jpg.cinfo));
  return 4*width*height*sizeof(uint8_t) - jpg.dest.free_in_buffer;
}


int dt_imageio_jpeg_write(const char *filename, const uint8_t *in, const int width, const int height, const int quality, void *exif, int exif_len)
{
	struct dt_imageio_jpeg_error_mgr jerr;
  dt_imageio_jpeg_t jpg;

  jpg.cinfo.err = jpeg_std_error(&jerr.pub);
  jerr.pub.error_exit = dt_imageio_jpeg_error_exit;
  if (setjmp(jerr.setjmp_buffer))
  {
	  jpeg_destroy_compress(&(jpg.cinfo));
    return 1;
  }
	jpeg_create_compress(&(jpg.cinfo));
  FILE *f = fopen(filename, "wb");
  if(!f) return 1;
  jpeg_stdio_dest(&(jpg.cinfo), f);

	jpg.cinfo.image_width = width;	
	jpg.cinfo.image_height = height;
	jpg.cinfo.input_components = 3;
	jpg.cinfo.in_color_space = JCS_RGB;
	jpeg_set_defaults(&(jpg.cinfo));
  jpeg_set_linear_quality(&(jpg.cinfo), quality, TRUE);
	jpeg_start_compress(&(jpg.cinfo), TRUE);

  if(exif && exif_len > 0 && exif_len < 65534)
    jpeg_write_marker(&(jpg.cinfo), JPEG_APP0+1, exif, exif_len);

  uint8_t row[3*width];
  const uint8_t *buf;
	while(jpg.cinfo.next_scanline < jpg.cinfo.image_height)
	{
		JSAMPROW tmp[1];
    buf = in + jpg.cinfo.next_scanline * jpg.cinfo.image_width * 4;
    for(int i=0;i<width;i++) for(int k=0;k<3;k++) row[3*i+k] = buf[4*i+k];
    tmp[0] = row;
		jpeg_write_scanlines(&(jpg.cinfo), tmp, 1);
	}
	jpeg_finish_compress (&(jpg.cinfo));
	jpeg_destroy_compress(&(jpg.cinfo));
  fclose(f);
  return 0;
}

int dt_imageio_jpeg_read_header(const char *filename, dt_imageio_jpeg_t *jpg)
{
  jpg->f = fopen(filename, "rb");
  if(!jpg->f) return 1;

  struct dt_imageio_jpeg_error_mgr jerr;
  jpg->dinfo.err = jpeg_std_error(&jerr.pub);
  jerr.pub.error_exit = dt_imageio_jpeg_error_exit;
  if (setjmp(jerr.setjmp_buffer))
  {
	  jpeg_destroy_decompress(&(jpg->dinfo));
    fclose(jpg->f);
    return 1;
  }
  jpeg_create_decompress(&(jpg->dinfo));
  jpeg_stdio_src(&(jpg->dinfo), jpg->f);
  jpg->dinfo.buffered_image = TRUE;
  jpeg_read_header(&(jpg->dinfo), TRUE);
  jpg->width  = jpg->dinfo.image_width;
  jpg->height = jpg->dinfo.image_height;
  return 0;
}

int dt_imageio_jpeg_read(dt_imageio_jpeg_t *jpg, uint8_t *out)
{
  (void)jpeg_start_decompress(&(jpg->dinfo));
	JSAMPROW row_pointer[1];
	row_pointer[0] = (uint8_t *)malloc(jpg->dinfo.output_width*jpg->dinfo.num_components);
  uint8_t *tmp = out;
	while(jpg->dinfo.output_scanline < jpg->dinfo.image_height)
	{
		if(jpeg_read_scanlines(&(jpg->dinfo), row_pointer, 1) != 1) return 1;
    if(jpg->dinfo.num_components < 3)
		  for(int i=0; i<jpg->dinfo.image_width;i++) for(int k=0;k<3;k++)
			  tmp[4*i+k] = row_pointer[0][jpg->dinfo.num_components*i+0];
    else
		  for(int i=0; i<jpg->dinfo.image_width;i++) for(int k=0;k<3;k++)
		  	tmp[4*i+k] = row_pointer[0][3*i+k];
    tmp += 4*jpg->width;
	}
	// (void)jpeg_finish_decompress(&(jpg->dinfo));
	jpeg_destroy_decompress(&(jpg->dinfo));
	free(row_pointer[0]);
  fclose(jpg->f);
  return 0;
}



