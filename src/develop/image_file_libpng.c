#include "image_file.h"
#include <png.h>

int write_image_file(uint8_t* model_output_bytes, unsigned int height,
                     unsigned int width, const char* output_file){
  png_image image;
  memset(&image, 0, (sizeof image));
  image.version = PNG_IMAGE_VERSION;
  image.format = PNG_FORMAT_BGR;
  image.height = height;
  image.width = width;
  int ret = 0;
  if (png_image_write_to_file(&image, output_file, 0 /*convert_to_8bit*/, model_output_bytes, 0 /*row_stride*/,
			      NULL /*colormap*/) == 0) {
    printf("write to '%s' failed:%s\n", output_file, image.message);
    ret = -1;
  }
  return ret;
}
