#ifdef HAVE_CONFIG_H
  #include "config.h"
#endif
#include "common/imageio_pfm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

int dt_imageio_open_pfm(dt_image_t *img, const char *filename)
{
  img->shrink = 0;

  const char *ext = filename + strlen(filename);
  while(*ext != '.' && ext > filename) ext--;
  if(strncmp(ext, ".pfm", 4) && strncmp(ext, ".PFM", 4) && strncmp(ext, ".Pfm", 4)) return 1;
  FILE *f = fopen(filename, "rb");
  int ret = 0;
  int cols = 3;
  char head[2] = {'X', 'X'};
  ret = fscanf(f, "%c%c\n", head, head+1);
  if(ret != 2 || head[0] != 'P') goto error;
  if(head[1] == 'F') cols = 3;
  else if(head[1] == 'f') cols = 1;
  else goto error;
  ret = fscanf(f, "%d %d\n%*[^\n]\n", &img->width, &img->height);
  if(ret != 2) goto error;

  if(img->output_width  == 0) img->output_width  = img->width;
  if(img->output_height == 0) img->output_height = img->height;

  if(dt_image_alloc(img, DT_IMAGE_FULL)) goto error;
  dt_image_check_buffer(img, DT_IMAGE_FULL, 3*img->width*img->height*sizeof(uint8_t));
  if(cols == 3) ret = fread(img->pixels, 3*sizeof(float), img->width*img->height, f);
  else for(int j=0; j < img->height; j++)
    for(int i=0; i < img->width; i++)
    {
      ret = fread(img->pixels + 3*(img->width*j + i), sizeof(float), 1, f);
      img->pixels[3*(img->width*j + i) + 2] =
      img->pixels[3*(img->width*j + i) + 1] =
      img->pixels[3*(img->width*j + i) + 0];
    }
  // repair nan/inf etc
  for(int i=0; i < img->width*img->height*3; i++) img->pixels[i] = fmaxf(0.0f, fminf(10000.0, img->pixels[i]));
  float *line = (float *)malloc(sizeof(float)*3*img->width);
  for(int j=0; j < img->height/2; j++)
  {
    memcpy(line,                                         img->pixels + img->width*j*3,                 3*sizeof(float)*img->width);
    memcpy(img->pixels + img->width*j*3,                 img->pixels + img->width*(img->height-1-j)*3, 3*sizeof(float)*img->width);
    memcpy(img->pixels + img->width*(img->height-1-j)*3, line,                                         3*sizeof(float)*img->width);
  }
  free(line);
  dt_image_release(img, DT_IMAGE_FULL, 'w');
  return 0;

error:
  fclose(f);
  return 1;
}
