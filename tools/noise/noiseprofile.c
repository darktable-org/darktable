#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

// call this to find a and b parameters.
// to begin, only find the a parameter.
// should be called with the pfm of the original image and the one
// of the denoised image as commandline parameters.

static float*
read_pfm(const char *filename, int *wd, int*ht)
{
  FILE *f = fopen(filename, "rb");
  if(!f) return 0;
  fscanf(f, "PF\n%d %d\n%*[^\n]", wd, ht);
  fgetc(f); // eat only one newline

  float *p = (float *)calloc(sizeof(float),3*(*wd)*(*ht));
  fread(p, sizeof(float)*3, (*wd)*(*ht), f);
  for(int k=0;k<3*(*wd)*(*ht);k++) p[k] = fmaxf(0.0f, p[k]);
  fclose(f);
  return p;
}

#if 0
static void
write_pfm(const char *filename, float *buf, int wd, int ht)
{
  FILE *f = fopen(filename, "wb");
  if(!f) return;
  fprintf(f, "PF\n%d %d\n-1.0\n", wd, ht);
  fwrite(buf, sizeof(float)*3, wd*ht, f);
  fclose(f);
}
#endif

// filter only vertically, as the gradient is evolving on the x axis, all the values
// for a same x are supposed to be the same
void mean_filter(const int radius, const float* in, float* out, const int width, const int height)
{
  float* h_mean = in;
  // vertical pass
  for(int j = 0; j < width; j++)
  {
    double sliding_mean[3] = { 0.0f, 0.0f, 0.0f };
    for(int c = 0; c < 3; c++)
    {
      sliding_mean[c] += h_mean[j*3+c];
    }
    for(int i = 1; i < radius; i++)
    {
      for(int c = 0; c < 3; c++)
      {
        sliding_mean[c] += 2*h_mean[(i*width+j)*3+c];
      }
    }
    for(int i = 0; i < height; i++)
    {
      int to_be_added_pixel_index;
      int to_be_removed_pixel_index;
      if(i+radius+1 >= height)
      {
        int diff = i+radius+1-height;
        to_be_added_pixel_index = ((i-diff)*width+j)*3;
      }
      else
      {
        to_be_added_pixel_index = ((i+radius+1)*width+j)*3;
      }
      if(i-radius < 0)
      {
        to_be_removed_pixel_index = ((radius-i)*width+j)*3;
      }
      else
      {
        to_be_removed_pixel_index = ((i-radius)*width+j)*3;
      }
      for(int c = 0; c < 3; c++)
      {
        out[(i*width+j)*3+c] = sliding_mean[c] / (2*radius+1);
        sliding_mean[c] += h_mean[to_be_added_pixel_index+c];
        sliding_mean[c] -= h_mean[to_be_removed_pixel_index+c];
        if(sliding_mean[c] < 0.0f) sliding_mean[c] = 0.0f;
      }
    }
  }
}

float median(float array[3])
{
  float min = array[0];
  int min_index = 0;
  if(array[1] < min)
  {
    min = array[1];
    min_index = 1;
  }
  if(array[2] < min)
  {
    min = array[2];
    min_index = 2;
  }
  int index1 = (min_index + 1) % 3;
  int index2 = (min_index + 2) % 3;
  if(array[index1] < array[index2])
    return array[index1];
  else
    return array[index2];
}

#define MIN(a,b) ((a>b)?b:a)
#define MAX(a,b) ((a>b)?a:b)

static inline float
clamp(float f, float m, float M)
{
  return MAX(MIN(f, M), m);
}

#define NB_CLASSES 2000
int main(int argc, char *arg[])
{
  if(argc < 3)
  {
    fprintf(stderr, "usage: %s input_noisy.pfm input_smooth.pfm\n", arg[0]);
    exit(1);
  }
  int wd, ht, wd2, ht2;
  float *input = read_pfm(arg[1], &wd, &ht);
  float *inputblurred = calloc(wd*ht*3, sizeof(float));
  float *input2 = read_pfm(arg[2], &wd2, &ht2);
  assert(wd == wd2);
  assert(ht == ht2);
  float *input2blurred = calloc(wd*ht*3, sizeof(float));
  const float radius = 75;
  mean_filter(radius, input, inputblurred, wd, ht);
  mean_filter(radius, input2, input2blurred, wd, ht);
  double var[3][NB_CLASSES];
  unsigned nb_elts[3][NB_CLASSES];
  for(int level = 0; level < NB_CLASSES; level++)
  {
    for(int c = 0; c < 3; c++)
    {
      var[c][level] = 0.0;
      nb_elts[c][level] = 0;
    }
  }
  if(argc < 10)
  {
    for(int i = radius; i < ht-radius; i++)
    {
      for(int j = 0; j < wd; j++)
      {
        for(int c = 0; c < 3; c++)
        {
          int index = (i * wd + j) * 3 + c;
          float pixel_diff = input[index] - inputblurred[index];
          float pixel_diff2 = input2[index] - input2blurred[index];
          unsigned level = (unsigned)(inputblurred[index] * NB_CLASSES);
          unsigned level2 = (unsigned)(input2blurred[index] * NB_CLASSES);
          if(level < NB_CLASSES)
          {
            var[c][level] += pixel_diff * pixel_diff;
            nb_elts[c][level]++;
          }
          if(level2 < NB_CLASSES)
          {
            var[c][level2] += pixel_diff2 * pixel_diff2;
            nb_elts[c][level2]++;
          }
        }
      }
    }
    for(int level = 0; level < NB_CLASSES; level++)
    {
      for(int c = 0; c < 3; c++)
      {
        if(nb_elts[c][level] > 0) var[c][level] /= nb_elts[c][level];
      }
    }
    float array_for_median[3][3] = {{0.0}};
    for(int c = 0; c < 3; c++)
    {
      array_for_median[c][0] = var[c][0];
    }
    for(int level = 0; level < NB_CLASSES-1; level++)
    {
      for(int c = 0; c < 3; c++)
      {
        array_for_median[c][(level+1)%3] = var[c][level+1];
        var[c][level] = median(array_for_median[c]);
      }

      if(nb_elts[0][level] > 100 && nb_elts[1][level] > 100 && nb_elts[2][level] > 100 && level > 0)
      {
        float level_normalized = level / (float)NB_CLASSES ;
        float level2 = level_normalized + 0.0001;
        fprintf(stdout, "%f %f %f %f %d %d %d\n", level_normalized, var[0][level] / level2, var[1][level] / level2,
              var[2][level] / level2, nb_elts[0][level], nb_elts[1][level], nb_elts[2][level]);
      }
    }
  }
  if(argc >= 10 && !strcmp(arg[3], "-c"))
  {
    const float a[3] = { atof(arg[4]), atof(arg[5]), atof(arg[6]) },
                p[3] = { atof(arg[7]), atof(arg[8]), atof(arg[9]) },
                b[3] = { atof(arg[10]), atof(arg[11]), atof(arg[12])};

    // perform VST
    for(int i = radius; i < ht-radius; i++)
    {
      for(int j = 0; j < wd; j++)
      {
        for(int c = 0; c < 3; c++)
        {
          int index = (i * wd + j) * 3 + c;
          input[index] = 2 * powf(input[index]+b[c], -p[c]/2+1) / ((-p[c]+2) * sqrt(a[c]));
          input2[index] = 2 * powf(input2[index]+b[c], -p[c]/2+1) / ((-p[c]+2) * sqrt(a[c]));

          unsigned level = (unsigned)(inputblurred[index] * NB_CLASSES);
          unsigned level2 = (unsigned)(input2blurred[index] * NB_CLASSES);
          inputblurred[index] = 2 * powf(inputblurred[index]+b[c], -p[c]/2+1) / ((-p[c]+2) * sqrt(a[c]));
          input2blurred[index] = 2 * powf(input2blurred[index]+b[c], -p[c]/2+1) / ((-p[c]+2) * sqrt(a[c]));

          float pixel_diff = input[index] - inputblurred[index];
          var[c][level] += pixel_diff * pixel_diff;
          nb_elts[c][level]++;
          pixel_diff = input2[index] - input2blurred[index];
          var[c][level2] += pixel_diff * pixel_diff;
          nb_elts[c][level2]++;
        }
      }
    }
    for(int level = 0; level < NB_CLASSES; level++)
    {
      for(int c = 0; c < 3; c++)
      {
        if(nb_elts[c][level] > 0)
        {
          var[c][level] /= nb_elts[c][level];
        }
      }
    }
    float array_for_median[3][3] = {{0.0}};
    for(int c = 0; c < 3; c++)
    {
      array_for_median[c][0] = var[c][0];
    }
    for(int level = 0; level < NB_CLASSES-1; level++)
    {
      for(int c = 0; c < 3; c++)
      {
        array_for_median[c][(level+1)%3] = var[c][level+1];
        var[c][level] = median(array_for_median[c]);
      }
      if(nb_elts[0][level] > 100 && nb_elts[1][level] > 100 && nb_elts[2][level] > 100)
        fprintf(stdout, "%f %f %f %f %d %d %d\n", level / (float)NB_CLASSES, var[0][level], var[1][level],
              var[2][level], nb_elts[0][level], nb_elts[1][level], nb_elts[2][level]);
    }
  }
  if(argc >= 10 && !strcmp(arg[3], "-b"))
  {
    const float a[3] = { atof(arg[4]), atof(arg[5]), atof(arg[6]) },
                p[3] = { atof(arg[7]), atof(arg[8]), atof(arg[9]) },
                b[3] = { atof(arg[10]), atof(arg[11]), atof(arg[12])};

    // perform VST
    for(int i = radius; i < ht-radius; i++)
    {
      for(int j = 0; j < wd; j++)
      {
        for(int c = 0; c < 3; c++)
        {
          int index = (i * wd + j) * 3 + c;
          input[index] = 2 * powf(input[index]+b[c], -p[c]/2+1) / ((-p[c]+2) * sqrt(a[c]));
          input2[index] = 2 * powf(input2[index]+b[c], -p[c]/2+1) / ((-p[c]+2) * sqrt(a[c]));
        }
      }
    }
    // blur after VST
    float *outputblurred = calloc(wd*ht*3, sizeof(float));
    float *output2blurred = calloc(wd*ht*3, sizeof(float));
    mean_filter(radius, input, outputblurred, wd, ht);
    mean_filter(radius, input2, output2blurred, wd, ht);
    // algebraic backtransform + print difference to real mean
    for(int i = radius; i < ht-radius; i++)
    {
      for(int j = 0; j < wd; j++)
      {
        for(int c = 0; c < 3; c++)
        {
          int index = (i * wd + j) * 3 + c;
          outputblurred[index] = MAX(powf(outputblurred[index]*sqrt(a[c])*(2-p[c])/2, 2/(2-p[c]))-b[c],0.0f);
          output2blurred[index] = MAX(powf(output2blurred[index]*sqrt(a[c])*(2-p[c])/2, 2/(2-p[c]))-b[c],0.0f);
        }
      }
    }
    double bias[3][NB_CLASSES] = {{0.0}};
    for(int i = radius; i < ht-radius; i++)
    {
      for(int j = 0; j < wd; j++)
      {
        for(int c = 0; c < 3; c++)
        {
          int index = (i * wd + j) * 3 + c;
          unsigned level = (unsigned)(inputblurred[index] * NB_CLASSES);
          unsigned level2 = (unsigned)(input2blurred[index] * NB_CLASSES);

          float diff = outputblurred[index];
          float diff2 = output2blurred[index];

          bias[c][level] += diff;
          nb_elts[c][level]++;
          bias[c][level2] += diff2;
          nb_elts[c][level2]++;
        }
      }
    }
    for(int level = 0; level < NB_CLASSES; level++)
    {
      for(int c = 0; c < 3; c++)
      {
        if(nb_elts[c][level] > 0) bias[c][level] /= nb_elts[c][level];
      }
    }
    float array_for_median[3][3] = {{0.0}};
    for(int c = 0; c < 3; c++)
    {
      array_for_median[c][0] = bias[c][0];
    }
    for(int level = 0; level < NB_CLASSES-1; level++)
    {
      for(int c = 0; c < 3; c++)
      {
        array_for_median[c][(level+1)%3] = bias[c][level+1];
        bias[c][level] = median(array_for_median[c]);
      }
      float level_normalized = level / (float)NB_CLASSES ;
      float level2 = level_normalized + 0.0001;
      if(nb_elts[0][level] > 100 && nb_elts[1][level] > 100 && nb_elts[2][level] > 100)
        fprintf(stdout, "%f %f %f %f %d %d %d\n", level_normalized, bias[0][level] / level2, bias[1][level] / level2,
              bias[2][level] / level2, nb_elts[0][level], nb_elts[1][level], nb_elts[2][level]);
    }
    free(outputblurred);
    free(output2blurred);
  }
  free(inputblurred);
  free(input);
  free(input2blurred);
  free(input2);
  exit(0);
}
