#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

// call this to find a and b parameters.
// to begin, only find the a parameter.
// should be called with the pfm of the original image and the one
// of the denoised image as commandline parameters.

static double*
read_pfm(const char *filename, int *wd, int*ht)
{
  FILE *f = fopen(filename, "rb");
  if(!f) return 0;
  fscanf(f, "PF\n%d %d\n%*[^\n]", wd, ht);
  fgetc(f); // eat only one newline

  double *p = (double *)calloc(sizeof(double),3*(*wd)*(*ht));
  float *fp = (float *)calloc(sizeof(float),3*(*wd)*(*ht));
  fread(fp, sizeof(float)*3, (*wd)*(*ht), f);
  for(int k=0;k<3*(*wd)*(*ht);k++) p[k] = (double)fmaxf(0.0f, fp[k]);
  fclose(f);
  free(fp);
  return p;
}

#if 0
static void
write_pfm(const char *filename, double *buf, int wd, int ht)
{
  FILE *f = fopen(filename, "wb");
  if(!f) return;
  fprintf(f, "PF\n%d %d\n-1.0\n", wd, ht);
  fwrite(buf, sizeof(double)*3, wd*ht, f);
  fclose(f);
}
#endif

// filter only vertically, as the gradient is evolving on the x axis, all the values
// for a same x are supposed to be the same
void mean_filter(const int radius, const double* in, double* out, const int width, const int height)
{
  double* h_mean = in;
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

double median(double array[3])
{
  double min = array[0];
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

static inline double
clamp(double f, double m, double M)
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
  double *input = read_pfm(arg[1], &wd, &ht);
  double *inputblurred = calloc(wd*ht*3, sizeof(double));
  double *input2 = read_pfm(arg[2], &wd2, &ht2);
  assert(wd == wd2);
  assert(ht == ht2);
  double *input2blurred = calloc(wd*ht*3, sizeof(double));
  const double radius = 200;
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
          double pixel_diff = input[index] - inputblurred[index];
          double pixel_diff2 = input2[index] - input2blurred[index];
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
    double array_for_median[3][3] = {{0.0}};
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
        double level_normalized = level / (double)NB_CLASSES ;
        double level2 = level_normalized + 0.0001;
        fprintf(stdout, "%f %f %f %f %d %d %d\n", level_normalized, var[0][level] / level2, var[1][level] / level2,
              var[2][level] / level2, nb_elts[0][level], nb_elts[1][level], nb_elts[2][level]);
      }
    }
  }
  if(argc >= 10 && !strcmp(arg[3], "-c"))
  {
    const double a[3] = { atof(arg[4]), atof(arg[5]), atof(arg[6]) },
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
          input[index] = 2 * pow(input[index]+b[c], -p[c]/2+1) / ((-p[c]+2) * sqrt(a[c]));
          input2[index] = 2 * pow(input2[index]+b[c], -p[c]/2+1) / ((-p[c]+2) * sqrt(a[c]));

          unsigned level = (unsigned)(inputblurred[index] * NB_CLASSES);
          unsigned level2 = (unsigned)(input2blurred[index] * NB_CLASSES);
          inputblurred[index] = 2 * pow(inputblurred[index]+b[c], -p[c]/2+1) / ((-p[c]+2) * sqrt(a[c]));
          input2blurred[index] = 2 * pow(input2blurred[index]+b[c], -p[c]/2+1) / ((-p[c]+2) * sqrt(a[c]));

          double pixel_diff = input[index] - inputblurred[index];
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
    double array_for_median[3][3] = {{0.0}};
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
        fprintf(stdout, "%f %f %f %f %d %d %d\n", level / (double)NB_CLASSES, var[0][level], var[1][level],
              var[2][level], nb_elts[0][level], nb_elts[1][level], nb_elts[2][level]);
    }
  }
  if(argc >= 10 && !strcmp(arg[3], "-b"))
  {
    const double a[3] = { atof(arg[4]), atof(arg[5]), atof(arg[6]) },
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
          input[index] = 2 * pow(input[index]+b[c], -p[c]/2+1) / ((-p[c]+2) /** sqrt(a[c])*/);
          input2[index] = 2 * pow(input2[index]+b[c], -p[c]/2+1) / ((-p[c]+2) /** sqrt(a[c])*/);
        }
      }
    }
#if 0
    // blur after VST
    double *outputmean = calloc(wd*ht*3, sizeof(double));
    double *output2mean = calloc(wd*ht*3, sizeof(double));
    mean_filter(radius, input, outputblurred, wd, ht);
    mean_filter(radius, input2, output2blurred, wd, ht);
    // algebraic low bias backtransform
    for(int i = radius; i < ht-radius; i++)
    {
      for(int j = 0; j < wd; j++)
      {
        for(int c = 0; c < 3; c++)
        {
          int index = (i * wd + j) * 3 + c;

          double alpha = 2.0/(/*sqrt(a[c])**/(2.0-p[c]));
          double beta = 1.0-p[c]/2.0;
          double a0 = alpha;
          double a1 = (1.0-beta)/(2.0*alpha*beta*alpha/a[c]);
          double delta = outputblurred[index]*outputblurred[index] + 4.0*a0*a1;
          double z1 = (outputblurred[index] + sqrt(delta))/(2.0*a0);
          double z2 = outputblurred[index] / a0;
          outputblurred[index] = pow(z1, 1.0/beta) - b[c];
          //outputblurred[index] = pow(z2, 1.0/beta) - b[c];
          //outputblurred[index] = pow(z1, 1.0/beta) - pow(z2, 1.0/beta);

          delta = output2blurred[index]*output2blurred[index] + 4.0*a0*a1;
          z1 = (output2blurred[index] + sqrt(delta))/(2.0*a0);
          z2 = output2blurred[index] / a0;
          output2blurred[index] = pow(z1, 1.0/beta) - b[c];
          //output2blurred[index] = pow(z2, 1.0/beta) - b[c];
        }
      }
    }
#endif
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

          double diff = input[index];
          double diff2 = input2[index];

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
        if(nb_elts[c][level] > 0)
        {
          bias[c][level] /= nb_elts[c][level];
          double var = bias[c][level];
          double alpha = 2.0/(/*sqrt(a[c])**/(2.0-p[c]));
          double beta = 1.0-p[c]/2.0;
          double a0 = alpha;
          double a1 = (1.0-beta)/(2.0*alpha*beta*alpha/a[c]);
          double delta = var*var + 4.0*a0*a1;
          double z1 = (var + sqrt(delta))/(2.0*a0);
          bias[c][level] = pow(z1, 1.0/beta) - b[c];
          double z2 = var / a0;
          bias[c][level] = pow(z2, 1.0/beta) - b[c];
        }
      }
    }

    double array_for_median[3][3] = {{0.0}};
    for(int c = 0; c < 3; c++)
    {
      array_for_median[c][0] = bias[c][0];
    }
    for(int level = 0; level < NB_CLASSES-1; level++)
    {
      for(int c = 0; c < 3; c++)
      {
        array_for_median[c][(level+1)%3] = bias[c][level+1];
        bias[c][level] = median(array_for_median[c]) + 0.0001;
      }
      double level_normalized = level / (double)NB_CLASSES ;
      double level2 = level_normalized + 0.0001;
      if(nb_elts[0][level] > 100 && nb_elts[1][level] > 100 && nb_elts[2][level] > 100)
        fprintf(stdout, "%f %f %f %f %d %d %d\n", level_normalized, bias[0][level] / level2, bias[1][level] / level2,
              bias[2][level] / level2, nb_elts[0][level], nb_elts[1][level], nb_elts[2][level]);
    }
#if 0
    free(outputblurred);
    free(output2blurred);
#endif
  }
  free(inputblurred);
  free(input);
  free(input2blurred);
  free(input2);
  exit(0);
}
