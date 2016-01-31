#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <magick/api.h>
#include <math.h>

typedef struct {
  double max_delta;
  double sum_differences;
  unsigned long npixels;
} stats_info;

void init_stats(stats_info *stats) {
  stats->max_delta = 0;
  stats->sum_differences = 0;
  stats->npixels = 0;
}

void print_stats(stats_info *stats) {
  fprintf(stdout, "max_delta %lf\n", stats->max_delta);
  fprintf(stdout, "mean_pixel_error %lf\n", (double) stats->sum_differences / (double) stats->npixels);
}

static MagickPassFail
compare_pixels(void *mutable_data,
               const void *immutable_data,
               const Image *first_image,
               const PixelPacket *first_pixels,
               const IndexPacket *first_indexes,
               const Image *second_image,
               const PixelPacket *second_pixels,
               const IndexPacket *second_indexes,
               const long npixels,
               ExceptionInfo *exception) {

  stats_info *stats = (stats_info *) mutable_data;

  int i;
  for (i=0; i < npixels; i++) {
    double r1 = first_pixels[i].red;
    double g1 = first_pixels[i].green;
    double b1 = first_pixels[i].blue;
    double r2 = second_pixels[i].red;
    double g2 = second_pixels[i].green;
    double b2 = second_pixels[i].blue;

    #define SQ(x) ((x)*(x))
    double delta = sqrt(SQ(r1-r2)+SQ(g1-g2)+SQ(b1-b2));
    stats->sum_differences += delta;
    if (delta > stats->max_delta) {
      //fprintf(stderr, "New maximum %0.2lf (%.0lf,%.0lf,%.0lf) vs (%.0lf,%.0lf,%.0lf)\n", delta, r1,g1,b1, r2,g2,b2);
      stats->max_delta = delta;
    }
  }
}

int main ( int argc, char **argv )
{
  InitializeMagick(NULL);

  ExceptionInfo exception;
  GetExceptionInfo(&exception);

  if (argc != 3) {
    fprintf (stderr, "Usage: %s infile outfile\n", argv[0] );
    return 1;
  }

  ImageInfo *imageInfo1=CloneImageInfo(0);
  strcpy(imageInfo1->filename, argv[1]);

  ImageInfo *imageInfo2=CloneImageInfo(0);
  strcpy(imageInfo2->filename, argv[2]);

  Image *image1 = ReadImage(imageInfo1, &exception);
  if (!image1) {
    CatchException(&exception);
    exit(2);
  }

  Image *image2 = ReadImage(imageInfo2, &exception);
  if (!image2) {
    CatchException(&exception);
    exit(2);
  }

  if (image1->columns != image2->columns || image1->rows != image2->rows) {
    fprintf(stderr, "Images are not same size %ldx%ld vs %ldx%ld\n", 
                    image1->rows, image1->columns,
                    image2->rows, image2->columns);
    exit(2);
  }

  stats_info stats;
  init_stats(&stats);
  stats.npixels = image1->columns*image1->rows;

  PixelIteratorOptions options;
  InitializePixelIteratorOptions(&options, &exception);
  options.max_threads = 1;
  PixelIterateDualRead(compare_pixels, &options, "compare_images", &stats, NULL,
                       image1->columns,image1->rows,
                       image1,0,0,
                       image2,0,0,
                       &exception);

  print_stats(&stats);

  return 0;
}

