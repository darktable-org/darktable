/*
    This file is part of darktable,
    Copyright (C) 2021-2023 darktable developers.

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
// our includes go first:
#include "bauhaus/bauhaus.h"
#include "common/dwt.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "dtgtk/drawingarea.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"

// #include <fftw3.h> // one day, include FFT convolution
#include <gtk/gtk.h>
#include <stdlib.h>

DT_MODULE_INTROSPECTION(1, dt_iop_blurs_params_t)

typedef enum dt_iop_blur_type_t
{
  DT_BLUR_LENS = 0,     // $DESCRIPTION: "lens"
  DT_BLUR_MOTION = 1,   // $DESCRIPTION: "motion"
  DT_BLUR_GAUSSIAN = 2, // $DESCRIPTION: "gaussian"
} dt_iop_blur_type_t;


typedef struct dt_iop_blurs_params_t
{
  dt_iop_blur_type_t type; // $DEFAULT: DT_BLUR_LENS $DESCRIPTION: "blur type"
  int radius;              // $MIN: 4 $MAX: 128 $DEFAULT: 8 $DESCRIPTION: "blur radius"

  // lens blur params
  int blades;              // $MIN: 3 $MAX: 11 $DEFAULT: 5 $DESCRIPTION: "diaphragm blades"
  float concavity;         // $MIN: 1. $MAX: 9.  $DEFAULT: 1. $DESCRIPTION: "concavity"
  float linearity;         // $MIN: 0. $MAX: 1.  $DEFAULT: 1. $DESCRIPTION: "linearity"
  float rotation;          // $MIN: -1.57 $MAX: 1.57 $DEFAULT: 0. $DESCRIPTION: "rotation"

  // motion blur params
  float angle;             // $MIN: -3.14 $MAX: 3.14 $DEFAULT: 0. $DESCRIPTION: "direction"
  float curvature;         // $MIN: -2.   $MAX: 2.   $DEFAULT: 0. $DESCRIPTION: "curvature"
  float offset;            // $MIN: -1.   $MAX: 1.   $DEFAULT: 0  $DESCRIPTION: "offset"

} dt_iop_blurs_params_t;


typedef struct dt_iop_blurs_gui_data_t
{
  GtkWidget *type, *radius, *blades, *concavity, *linearity, *rotation, *angle, *curvature, *offset;
  GtkDrawingArea *area;
  unsigned char *img;
  int img_cached;
  float img_width;
} dt_iop_blurs_gui_data_t;


typedef struct dt_iop_blurs_global_data_t
{
  int kernel_blurs_convolve;
} dt_iop_blurs_global_data_t;


const char *name()
{
  return _("blurs");
}

const char *aliases()
{
  return _("blur|lens|motion");
}

const char **description(struct dt_iop_module_t *self)
{
  return dt_iop_set_description(self,
                                _("simulate physically-accurate lens and motion blurs"),
                                _("creative"), _("linear, RGB, scene-referred"), _("linear, RGB"),
                                _("linear, RGB, scene-referred"));
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING;
}


int default_group()
{
  return IOP_GROUP_EFFECTS | IOP_GROUP_EFFECT;
}


int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}


void commit_params(dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  memcpy(piece->data, p1, self->params_size);
}

// B spline filter
#define FSIZE 5

inline static void blur_2D_Bspline(const float *const restrict in, float *const restrict out,
                                   const size_t width, const size_t height)
{
#ifdef _OPENMP
#pragma omp parallel for default(none) dt_omp_firstprivate(width, height, in, out) \
    schedule(simd: static)    \
    collapse(2)
#endif
  for(size_t i = 0; i < height; i++)
  {
    for(size_t j = 0; j < width; j++)
    {
      const size_t index = (i * width + j);
      float acc = 0.f;

      for(size_t ii = 0; ii < FSIZE; ++ii)
        for(size_t jj = 0; jj < FSIZE; ++jj)
        {
          const size_t row = CLAMP((int)i + (int)(ii - (FSIZE - 1) / 2), (int)0, (int)height - 1);
          const size_t col = CLAMP((int)j + (int)(jj - (FSIZE - 1) / 2), (int)0, (int)width - 1);
          const size_t k_index = (row * width + col);

          static const float DT_ALIGNED_ARRAY filter[FSIZE]
              = { 1.0f / 16.0f, 4.0f / 16.0f, 6.0f / 16.0f, 4.0f / 16.0f, 1.0f / 16.0f };

          acc += filter[ii] * filter[jj] * in[k_index];
        }

      out[index] = acc;
    }
  }
}


static inline void init_kernel(float *const restrict buffer, const size_t width, const size_t height)
{
  // init an empty kernel with zeros
#ifdef _OPENMP
#pragma omp parallel for simd default(none) dt_omp_firstprivate(width, height, buffer) \
    schedule(simd: static) aligned(buffer:64)
#endif
  for(size_t k = 0; k < height * width; k++) buffer[k] = 0.f;
}


static inline void create_lens_kernel(float *const restrict buffer,
                                      const size_t width, const size_t height,
                                      const float n, const float m, const float k, const float rotation)
{
  // n is number of diaphragm blades
  // m is the concavity, aka the number of vertices on straight lines (?)
  // k is the roundness vs. linearity factor
  //   see https://math.stackexchange.com/a/4160104/498090
  // buffer sizes need to be odd

  // Spatial coordinates rounding error
  const float eps = 1.f / (float)width;
  const float radius = (float)(width - 1) / 2.f - 1;

#ifdef _OPENMP
#pragma omp parallel for simd default(none) dt_omp_firstprivate(width, height, buffer, n, m, k, rotation, eps, radius) \
    schedule(simd: static) aligned(buffer:64) collapse(2)
#endif
  for(size_t i = 0; i < height; i++)
    for(size_t j = 0; j < width; j++)
    {
      // get normalized kernel coordinates in [-1 ; 1]
      const float x = (float)(i - 1) / radius - 1;
      const float y = (float)(j - 1) / radius - 1;

      // get current radial distance from kernel center
      const float r = dt_fast_hypotf(x, y);

      // get the radial distance at current angle of the shape envelope
      const float M = cosf((2.f * asinf(k) + M_PI_F * m) / (2.f * n))
                      / cosf((2.f * asinf(k * cosf(n * (atan2f(y, x) + rotation))) + M_PI_F * m) / (2.f * n));

      // write 1 if we are inside the envelope of the shape, else 0
      buffer[i * width + j] = (M >= r + eps);
    }
}


static inline void create_motion_kernel(float *const restrict buffer,
                                        const size_t width, const size_t height,
                                        const float angle, const float curvature, const float offset)
{
  // Compute the polynomial params from user params
  const float A = curvature / 2.f;
  const float B = 1.f;
  const float C = -A * offset * offset + B * offset;
  // Note : C ensures the polynomial arc always goes through the central pixel
  // so we don't shift pixels. This is meant to allow seamless connection
  // with unmasked areas when using masked blur.

  // Spatial coordinates rounding error
  const float eps = 1.f / (float)width;

  const float radius = (float)(width - 1) / 2.f - 1;
  const float corr_angle = -M_PI_F / 4.f - angle;

  // Matrix of rotation
  const float M[2][2] = { { cosf(corr_angle), -sinf(corr_angle) },
                          { sinf(corr_angle), cosf(corr_angle) } };

#ifdef _OPENMP
#pragma omp parallel for simd default(none) dt_omp_firstprivate(width, height, buffer, A, B, C, radius, offset, M, eps) \
    schedule(simd: static) aligned(buffer:64)
#endif
  for(size_t i = 0; i < 8 * width; i++)
  {
    // Note : for better smoothness of the polynomial discretization,
    // we oversample 8 times, meaning we evaluate the polynomial
    // every eighth of pixel

    // get normalized kernel coordinates in [-1 ; 1]
    const float x = (float)(i / 8.f - 1) / radius - 1;
    //const float y = (j - 1) / radius - 1; // not used here

    // build the motion path : 2nd order polynomial
    const float X = x - offset;
    const float y = X * X * A + X * B + C;

    // rotate the motion path around the kernel center
    const float rot_x = x * M[0][0] + y * M[0][1];
    const float rot_y = x * M[1][0] + y * M[1][1];

    // convert back to kernel absolute coordinates ± eps
    const int y_f[2] = { roundf((rot_y + 1) * radius - eps),
                         roundf((rot_y + 1) * radius + eps) };
    const int x_f[2] = { roundf((rot_x + 1) * radius - eps),
                         roundf((rot_x + 1) * radius + eps) };

    // write 1 if we are inside the envelope of the shape, else 0
    // leave 1px padding on each border of the kernel for the anti-aliasing
    for(int l = 0; l < 2; l++)
      for(int m = 0; m < 2; m++)
      {
        if(x_f[l] > 0 && x_f[l] < width - 1 && y_f[m] > 0 && y_f[m] < width - 1)
          buffer[y_f[m] * width + x_f[l]] = 1.f;
      }
  }
}


static inline void create_gauss_kernel(float *const restrict buffer,
                                       const size_t width, const size_t height)
{
  // This is not optimized. Gauss kernel is separable and can be turned into
  // 2 × 1D convolutions.
  const float radius = (width - 1) / 2.f - 1;

#ifdef _OPENMP
#pragma omp parallel for simd default(none) dt_omp_firstprivate(width, height, buffer, radius) \
    schedule(simd: static) aligned(buffer:64) collapse(2)
#endif
  for(size_t i = 0; i < height; i++)
    for(size_t j = 0; j < width; j++)
    {
      // get normalized kernel coordinates in [-1 ; 1]
      const float x = (float)(i - 1) / radius - 1;
      const float y = (float)(j - 1) / radius - 1;

      // get current square radial distance from kernel center
      const float r_2 = x * x + y * y;
      buffer[i * width + j] = expf(-4.f * r_2);
    }
}



static inline void build_gui_kernel(unsigned char *const buffer, const size_t width, const size_t height,
                                    dt_iop_blurs_params_t *p)
{
  float *const restrict kernel_1 = dt_alloc_align_float(width * height);
  float *const restrict kernel_2 = dt_alloc_align_float(width * height);
  if(!kernel_1 || !kernel_2)
  {
    dt_print(DT_DEBUG_ALWAYS,"[blurs] out of memory, skipping build_gui_kernel\n");
    goto cleanup;
  }

  if(p->type == DT_BLUR_LENS)
  {
    create_lens_kernel(kernel_1, width, height, p->blades, p->concavity, p->linearity, p->rotation);

    // anti-aliasing step
    blur_2D_Bspline(kernel_1, kernel_2, width, height);
  }
  else if(p->type == DT_BLUR_MOTION)
  {
    init_kernel(kernel_1, width, height);
    create_motion_kernel(kernel_1, width, height, p->angle, p->curvature, p->offset);

    // anti-aliasing step
    blur_2D_Bspline(kernel_1, kernel_2, width, height);
  }
  else if(p->type == DT_BLUR_GAUSSIAN)
  {
    create_gauss_kernel(kernel_2, width, height);
  }

  // Convert to Gtk/Cairo RGBA 8×4 bits
#ifdef _OPENMP
#pragma omp parallel for simd default(none) dt_omp_firstprivate(width, height, buffer, kernel_2) \
    schedule(simd: static) aligned(buffer, kernel_2:64)
#endif
  for(size_t k = 0; k < height * width; k++)
  {
    buffer[k * 4] = buffer[k * 4 + 1] = buffer[k * 4 + 2] = buffer[k * 4 + 3] = roundf(255.f * kernel_2[k]);
  }
cleanup:
  dt_free_align(kernel_1);
  dt_free_align(kernel_2);
}


static inline float compute_norm(float *const buffer, const size_t width, const size_t height)
{
  float norm = 0.f;

#ifdef _OPENMP
#pragma omp parallel for simd default(none) dt_omp_firstprivate(width, height, buffer) \
    schedule(simd: static) aligned(buffer:64) reduction(+:norm)
#endif
  for(size_t i = 0; i < width * height; i++)
  {
    norm += buffer[i];
  }

  return norm;
}


static inline void normalize(float *const buffer, const size_t width, const size_t height, const float norm)
{
#ifdef _OPENMP
#pragma omp parallel for simd default(none) dt_omp_firstprivate(width, height, buffer, norm) \
    schedule(simd: static) aligned(buffer:64)
#endif
  for(size_t i = 0; i < width * height; i++)
  {
    buffer[i] /= norm;
  }
}


static inline void build_pixel_kernel(float *const buffer, const size_t width, const size_t height,
                                      dt_iop_blurs_params_t *p)
{
  float *const restrict kernel_1 = dt_alloc_align_float(width * height);
  if(!kernel_1)
  {
    dt_print(DT_DEBUG_ALWAYS,"[blurs] out of memory, skippping build_pixel_kernel\n");
    return;
  }

  if(p->type == DT_BLUR_LENS)
  {
    create_lens_kernel(kernel_1, width, height, p->blades, p->concavity, p->linearity, p->rotation + M_PI_F);

    // anti-aliasing step
    blur_2D_Bspline(kernel_1, buffer, width, height);
  }
  else if(p->type == DT_BLUR_MOTION)
  {
    init_kernel(kernel_1, width, height);
    create_motion_kernel(kernel_1, width, height, p->angle + M_PI_F, p->curvature, p->offset);

    // anti-aliasing step
    blur_2D_Bspline(kernel_1, buffer, width, height);
  }
  else if(p->type == DT_BLUR_GAUSSIAN)
  {
    create_gauss_kernel(buffer, width, height);
  }

  // normalize to respect the conservation of energy law
  const float norm = compute_norm(buffer, width, height);
  normalize(buffer, width, height, norm);

  dt_free_align(kernel_1);
}

#if 0

// This crashes on the FFT step - not sure why and no time to investigate opaque libs now
// FFT convolution should be faster for large blurs because it is o(N log2(N))
// where N is the width of the kernel
// TODO

static void process_fft(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece,
                        const void *const restrict ivoid, void *const restrict ovoid,
                        const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_blurs_params_t *p = (dt_iop_blurs_params_t *)piece->data;
  const float scale = piece->iscale / roi_in->scale;

  if(!dt_iop_have_required_input_format(4, self, piece->colors, ivoid, ovoid, roi_in, roi_out))
    return;

  const float *const restrict in = __builtin_assume_aligned(ivoid, 64);
  //float *const restrict out = __builtin_assume_aligned(ovoid, 64);

  // FFT needs odd buffer sizes, so fix that here
  const int is_width_even = (roi_in->width % 2 == 0);
  const int is_height_even = (roi_in->height % 2 == 0);
  const size_t padded_width = roi_in->width + 1 * is_width_even;
  const size_t padded_height = roi_in->height + 1 * is_height_even;

  float *const restrict padded_in = dt_alloc_align_float(padded_width * padded_height * 4);
  float *const restrict padded_out = dt_alloc_align_float(padded_width * padded_height * 4);
  if(!padded_in || !padded_out)
  {
    dt_print(DT_DEBUG_ALWAYS,"[blurs] out of memory, skipping process_fft\n");
    goto cleanup;
  }

  // Write the image in the padded buffer
#ifdef _OPENMP
#pragma omp parallel for simd default(none) dt_omp_firstprivate(padded_width, padded_height, roi_in, in, padded_in) \
    schedule(simd: static) aligned(in, padded_in:64)
#endif
  for(size_t i = 0; i < roi_in->height; i++)
    for(size_t j = 0; j < roi_in->width; j++)
    {
      const size_t index_in = (i * roi_in->width + j) * 4;
      const size_t index_out = (i * padded_width + j) * 4;
      for_four_channels(c, aligned(in, padded_in : 64)) padded_in[index_out + c] = in[index_in + c];
    }

  // Write the padding if needed
  if(padded_width > roi_in->width)
  {
#ifdef _OPENMP
#pragma omp parallel for simd default(none) dt_omp_firstprivate(padded_width, padded_height, roi_in, in, padded_in) \
    schedule(simd: static) aligned(in, padded_in:64)
#endif
  for(size_t i = 0; i < roi_in->height; i++)
    {
      const size_t index_in = (i * (roi_in->width - 1)) * 4;
      const size_t index_out = (i * (padded_width - 1)) * 4;
      for_four_channels(c, aligned(in, padded_in : 64)) padded_in[index_out + c] = in[index_in + c];
    }
  }

  if(padded_height > roi_in->height)
  {
#ifdef _OPENMP
#pragma omp parallel for simd default(none) dt_omp_firstprivate(padded_width, padded_height, roi_in, in, padded_in) \
    schedule(simd: static) aligned(in, padded_in:64)
#endif
  for(size_t j = 0; j < roi_in->width; j++)
    {
      const size_t index_in = ((roi_in->height - 1) * roi_in->width + j) * 4;
      const size_t index_out = ((padded_height - 1) * padded_width + j) * 4;
      for_four_channels(c, aligned(in, padded_in : 64)) padded_in[index_out + c] = in[index_in + c];
    }
  }

  // Init the blur kernel
  const size_t radius = MAX(roundf(p->radius / scale), 1);
  const size_t kernel_width = 2 * radius + 1;

  float *const restrict kernel = dt_alloc_align_float(kernel_width * kernel_width);
  build_pixel_kernel(kernel, kernel_width, kernel_width, p);

  // Convert to padded kernel - copy kernel in the center
  float *const restrict padded_kernel = dt_alloc_align_float(padded_width * padded_height);
  const size_t offset_i = (padded_height - 1) / 2 - (kernel_width - 1) / 2;
  const size_t offset_j = (padded_width - 1) / 2 - (kernel_width - 1) / 2;
  const size_t i_reach = offset_i + kernel_width;
  const size_t j_reach = offset_j + kernel_width;

#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
    dt_omp_firstprivate(padded_width, padded_height, padded_kernel, kernel, offset_i, offset_j, i_reach, j_reach, kernel_width) \
    schedule(simd: static) aligned(kernel, padded_kernel:64)
#endif
  for(size_t i = 0; i < padded_width; i++)
    for(size_t j = 0; j < padded_width; j++)
    {
      const size_t padded_idx = (i * padded_width) + j;

      if(i >= offset_i && i < i_reach && j >= offset_j && j < j_reach)
      {
        const size_t i_kern = i - offset_i;
        const size_t j_kern = j - offset_j;
        const size_t kern_idx = i_kern * kernel_width + j_kern;
        padded_kernel[padded_idx] = kernel[kern_idx];
      }
      else
        padded_kernel[padded_idx] = 0.f;
    }

  // Init the FFT transforms
  int threads = fftw_init_threads();
  fftw_plan_with_nthreads(threads);

  // TODO: things go well until this point

  // Plan the dimensions of the FFT
  // notice we use fftwf prefix to use the float 32 variant of fftw
  int rank = 2;     /* 2D FFT */
  int n[2] = { padded_width, padded_height };
  int howmany = 4; /* 4 channels : RGBa */
  int idist = 1;   /* channels are distanced by one float in memory */
  int odist = 1;   /* channels are distanced by one float in memory */
  int istride = 4; /* array is not contiguous in memory */
  int ostride = 4; /* array is not contiguous in memory */
  int *inembed = n;
  int *onembed = n;

  fftwf_complex *const restrict kernel_fft = fftwf_alloc_complex(padded_height * padded_height * 4);
  fftwf_complex *const restrict image_fft = fftwf_alloc_complex(padded_height * padded_height * 4);

  // FFT convert the kernel
  fftwf_plan kernel_plan = fftwf_plan_many_dft_r2c(rank, n, howmany,
                                 padded_kernel, inembed,
                                 istride, idist,
                                 kernel_fft, onembed,
                                 ostride, odist,
                                 FFTW_ESTIMATE);
  fftwf_execute(kernel_plan);

  // Clean FFT
  fftwf_destroy_plan(kernel_plan);
  fftwf_free(kernel_fft);
  fftwf_free(image_fft);
  fftw_cleanup_threads();

  dt_free_align(kernel);
  dt_free_align(padded_kernel);
cleanup:
  dt_free_align(padded_in);
  dt_free_align(padded_out);
}
#endif

// Spatial convolution should be slower for large blurs because it is o(N²) where N is the width of the kernel
// but code is much simpler and easier to debug

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece,
                    const void *const restrict ivoid, void *const restrict ovoid,
                    const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_blurs_params_t *p = (dt_iop_blurs_params_t *)piece->data;
  const float scale = fmaxf(piece->iscale / roi_in->scale, 1.f);

  if(!dt_iop_have_required_input_format(4, self, piece->colors, ivoid, ovoid, roi_in, roi_out))
    return;

  const float *const restrict in = __builtin_assume_aligned(ivoid, 64);
  float *const restrict out = __builtin_assume_aligned(ovoid, 64);

  // Init the blur kernel
  const int radius = MAX(roundf(p->radius / scale), 2);
  const size_t kernel_width = 2 * radius + 1;

  float *const restrict kernel = dt_alloc_align_float(kernel_width * kernel_width);
  build_pixel_kernel(kernel, kernel_width, kernel_width, p);

#ifdef _OPENMP
#pragma omp parallel for default(none) \
    dt_omp_firstprivate(roi_out, in, out, kernel, kernel_width, radius) \
    schedule(simd: static) collapse(2)
#endif
  for(int i = 0; i < roi_out->height; i++)
    for(int j = 0; j < roi_out->width; j++)
    {
      const size_t index = ((i * roi_out->width) + j) * 4;
      float DT_ALIGNED_PIXEL acc[4] = { 0.f };

      if(i >= radius && j >= radius && i < roi_out->height - radius && j < roi_out->width - radius)
      {
        // We are in the safe area, no need to check for out-of-bounds
        for(int l = -radius; l <= radius; l++)
          for(int m = -radius; m <= radius; m++)
          {
            const int ii = i + l;
            const int jj = j + m;
            const size_t idx_shift = ((ii * roi_out->width) + jj) * 4;

            const int ik = l + radius;
            const int jk = m + radius;
            const size_t idx_kernel = (ik * kernel_width) + jk;
            const float k = kernel[idx_kernel];

            for_four_channels(c, aligned(in : 64)) acc[c] += k * in[idx_shift + c];
          }
      }
      else
      {
        // We are close to borders, we need to clamp indices to bounds
        // assume constant boundary conditions
        for(int l = -radius; l <= radius; l++)
          for(int m = -radius; m <= radius; m++)
          {
            const int ii = CLAMP((int)i + l, (int)0, (int)roi_out->height - 1);
            const int jj = CLAMP((int)j + m, (int)0, (int)roi_out->width - 1);
            const size_t idx_shift = ((ii * roi_out->width) + jj) * 4;

            const int ik = l + radius;
            const int jk = m + radius;
            const size_t idx_kernel = (ik * kernel_width) + jk;
            const float k = kernel[idx_kernel];

            for_four_channels(c, aligned(in : 64)) acc[c] += k * in[idx_shift + c];
          }
      }

      for_each_channel(c, aligned(out : 64) aligned(acc : 16)) out[index + c] = acc[c];

      // copy alpha
      out[index + 3] = in[index + 3];
    }
  dt_free_align(kernel);
}


#if HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_blurs_params_t *p = (dt_iop_blurs_params_t *)piece->data;
  dt_iop_blurs_global_data_t *const gd = (dt_iop_blurs_global_data_t *)self->global_data;

  cl_int err = DT_OPENCL_DEFAULT_ERROR;

  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;


  // Init the blur kernel
  const float scale = fmaxf(piece->iscale / roi_in->scale, 1.f);
  const int radius = MAX(roundf(p->radius / scale), 2);
  const size_t kernel_width = 2 * radius + 1;

  float *const restrict kernel = dt_alloc_align_float(kernel_width * kernel_width);
  build_pixel_kernel(kernel, kernel_width, kernel_width, p);

  cl_mem kernel_cl = dt_opencl_copy_host_to_device(devid, kernel, kernel_width, kernel_width, sizeof(float));

  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_blurs_convolve, width, height,
    CLARG(dev_in), CLARG(kernel_cl), CLARG(dev_out), CLARG(roi_out->width), CLARG(roi_out->height),
    CLARG(radius));
  if(err != CL_SUCCESS) goto error;

  // cleanup and exit on success
  dt_free_align(kernel);
  dt_opencl_release_mem_object(kernel_cl);
  return TRUE;

error:
  if(kernel) dt_free_align(kernel);
  if(kernel_cl) dt_opencl_release_mem_object(kernel_cl);
  dt_print(DT_DEBUG_OPENCL, "[opencl_blurs] couldn't enqueue kernel! %s\n", cl_errstr(err));
  return FALSE;
}

void init_global(dt_iop_module_so_t *module)
{
  const int program = 34;
  dt_iop_blurs_global_data_t *gd = (dt_iop_blurs_global_data_t *)malloc(sizeof(dt_iop_blurs_global_data_t));
  module->data = gd;
  gd->kernel_blurs_convolve = dt_opencl_create_kernel(program, "convolve");
}


void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_blurs_global_data_t *gd = (dt_iop_blurs_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_blurs_convolve);
  free(module->data);
  module->data = NULL;
}
#endif


void gui_changed(dt_iop_module_t *self, GtkWidget *w, void *previous)
{
  dt_iop_blurs_params_t *p = (dt_iop_blurs_params_t *)self->params;
  dt_iop_blurs_gui_data_t *g = (dt_iop_blurs_gui_data_t *)self->gui_data;

  if(!w || w == g->type)
  {
    if(p->type == DT_BLUR_LENS)
    {
      gtk_widget_hide(g->angle);
      gtk_widget_hide(g->curvature);
      gtk_widget_hide(g->offset);

      gtk_widget_show(g->blades);
      gtk_widget_show(g->concavity);
      gtk_widget_show(g->rotation);
      gtk_widget_show(g->linearity);
    }
    else if(p->type == DT_BLUR_MOTION)
    {
      gtk_widget_show(g->angle);
      gtk_widget_show(g->curvature);
      gtk_widget_show(g->offset);

      gtk_widget_hide(g->blades);
      gtk_widget_hide(g->concavity);
      gtk_widget_hide(g->rotation);
      gtk_widget_hide(g->linearity);
    }
    else if(p->type == DT_BLUR_GAUSSIAN)
    {
      gtk_widget_hide(g->angle);
      gtk_widget_hide(g->curvature);
      gtk_widget_hide(g->offset);

      gtk_widget_hide(g->blades);
      gtk_widget_hide(g->concavity);
      gtk_widget_hide(g->rotation);
      gtk_widget_hide(g->linearity);
    }
  }

  // update kernel view
  if(g->img_cached)
  {
    build_gui_kernel(g->img, g->img_width, g->img_width, p);
    gtk_widget_queue_draw(GTK_WIDGET(g->area));
  }
}

static gboolean dt_iop_tonecurve_draw(GtkWidget *widget, cairo_t *crf, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_blurs_gui_data_t *g = (dt_iop_blurs_gui_data_t *)self->gui_data;
  dt_iop_blurs_params_t *p = (dt_iop_blurs_params_t *)self->params;

  GtkAllocation allocation;
  GtkStyleContext *context = gtk_widget_get_style_context(widget);
  gtk_widget_get_allocation(widget, &allocation);
  gtk_render_background(context, crf, 0, 0, allocation.width, allocation.height);

  if(allocation.width != g->img_width)
  {
    // Widget size changed, flush the cache buffer and restart
    g->img_cached = FALSE;
    if(g->img) dt_free_align(g->img);
  }

  if(!g->img_cached)
  {
    g->img = dt_alloc_align(64, sizeof(unsigned char) * 4 * allocation.width * allocation.width);
    g->img_width = allocation.width;
    g->img_cached = TRUE;
    build_gui_kernel(g->img, g->img_width, g->img_width, p);

    // Note: if params change, we silently recompute the img in the buffer
    // no need to flush the cache. Flush only if buffer size changes,
    // aka GUI widget gets resized.
  }

  // Paint the kernel
  const int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, g->img_width);
  cairo_surface_t *cst = cairo_image_surface_create_for_data(g->img, CAIRO_FORMAT_ARGB32,
                            g->img_width, g->img_width, stride);

  cairo_set_source_surface(crf, cst, 0, 0);
  cairo_paint(crf);
  cairo_surface_destroy(cst);
  return TRUE;
}

void gui_update(dt_iop_module_t *self)
{
// FIXME check why needed
  gui_changed(self, NULL, NULL);
}

#define DEG_TO_RAD 180.f / M_PI_F

void gui_init(dt_iop_module_t *self)
{
  dt_iop_blurs_gui_data_t *g = IOP_GUI_ALLOC(blurs);

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  // Image buffer to store the kernel look
  // Don't recompute it in the drawing function, only when a param is changed
  // then serve it from cache to the drawing function.
  g->img_cached = FALSE;
  g->img = NULL;
  g->img_width = 0.f;

  g->area = GTK_DRAWING_AREA(dtgtk_drawing_area_new_with_aspect_ratio(1.f));
  g_signal_connect(G_OBJECT(g->area), "draw", G_CALLBACK(dt_iop_tonecurve_draw), self);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->area), TRUE, TRUE, 0);

  g->radius = dt_bauhaus_slider_from_params(self, "radius");
  dt_bauhaus_slider_set_format(g->radius, " px");

  g->type = dt_bauhaus_combobox_from_params(self, "type");

  g->blades = dt_bauhaus_slider_from_params(self, "blades");
  g->concavity = dt_bauhaus_slider_from_params(self, "concavity");
  g->linearity = dt_bauhaus_slider_from_params(self, "linearity");
  g->rotation = dt_bauhaus_slider_from_params(self, "rotation");
  dt_bauhaus_slider_set_factor(g->rotation, DEG_TO_RAD);
  dt_bauhaus_slider_set_format(g->rotation, "°");

  g->angle = dt_bauhaus_slider_from_params(self, "angle");
  dt_bauhaus_slider_set_factor(g->angle, DEG_TO_RAD);
  dt_bauhaus_slider_set_format(g->angle, "°");


  g->curvature = dt_bauhaus_slider_from_params(self, "curvature");
  g->offset = dt_bauhaus_slider_from_params(self, "offset");

}

void gui_cleanup(dt_iop_module_t *self)
{
  dt_iop_blurs_gui_data_t *g = (dt_iop_blurs_gui_data_t *)self->gui_data;
  if(g->img) dt_free_align(g->img);
  IOP_GUI_FREE;
}


// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

