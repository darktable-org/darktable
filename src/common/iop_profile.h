/*
    This file is part of darktable,
    Copyright (C) 2020-2023 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef DT_IOP_PROFILE_H
#define DT_IOP_PROFILE_H

#include "common/colorspaces_inline_conversions.h"
#include "common/colorspaces.h"
#include "develop/imageop.h"

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef HAVE_OPENCL
#include <CL/cl.h>           // for cl_mem
#endif

struct dt_iop_module_t;
struct dt_develop_t;
struct dt_dev_pixelpipe_t;
struct dt_dev_pixelpipe_iop_t;

typedef struct dt_iop_order_iccprofile_info_t
{
  dt_colorspaces_color_profile_type_t type;
  char filename[DT_IOP_COLOR_ICC_LEN];
  dt_iop_color_intent_t intent;
  dt_colormatrix_t matrix_in; // don't align on more than 16 bits or OpenCL will fail
  dt_colormatrix_t matrix_out;
  int lutsize;
  float *lut_in[3];
  float *lut_out[3];
  float unbounded_coeffs_in[3][3] DT_ALIGNED_PIXEL;
  float unbounded_coeffs_out[3][3] DT_ALIGNED_PIXEL;
  int nonlinearlut;
  float grey;
  dt_colormatrix_t matrix_in_transposed;  // same as matrix_in, but stored such as to permit vectorization
  dt_colormatrix_t matrix_out_transposed; // same as matrix_out, but stored such as to permit vectorization
} dt_iop_order_iccprofile_info_t;

/** must be called before using profile_info, default lutsize = 0 */
void dt_ioppr_init_profile_info(dt_iop_order_iccprofile_info_t *profile_info,
                                const int lutsize);
/** must be called when done with profile_info */
void dt_ioppr_cleanup_profile_info(dt_iop_order_iccprofile_info_t *profile_info);

/** returns the profile info from dev profiles info list that matches
 * (profile_type, profile_filename) NULL if not found
 */
dt_iop_order_iccprofile_info_t *
dt_ioppr_get_profile_info_from_list(struct dt_develop_t *dev,
                                    dt_colorspaces_color_profile_type_t profile_type,
                                    const char *profile_filename);

/** adds the profile info from (profile_type, profile_filename) to the
 * dev profiles info list if not already exists returns the generated
 * profile or the existing one
 */
dt_iop_order_iccprofile_info_t *
dt_ioppr_add_profile_info_to_list(struct dt_develop_t *dev,
                                  const dt_colorspaces_color_profile_type_t profile_type,
                                  const char *profile_filename,
                                  const int intent);

/** returns a reference to the work profile info as set on colorin iop
 * only if module is between colorin and colorout, otherwise returns NULL
 * work profile must not be cleanup()
 */
dt_iop_order_iccprofile_info_t *dt_ioppr_get_iop_work_profile_info(struct dt_iop_module_t *module,
                                                                   GList *iop_list);
dt_iop_order_iccprofile_info_t *dt_ioppr_get_iop_input_profile_info(struct dt_iop_module_t *module,
                                                                    GList *iop_list);

/** set the work profile (type, filename) on the pipe, should be called on process*()
 * if matrix cannot be generated it default to linear rec 2020
 * returns the actual profile that has been set
 */
dt_iop_order_iccprofile_info_t *
dt_ioppr_set_pipe_work_profile_info(struct dt_develop_t *dev,
                                    struct dt_dev_pixelpipe_t *pipe,
                                    const dt_colorspaces_color_profile_type_t type,
                                    const char *filename,
                                    const int intent);

dt_iop_order_iccprofile_info_t *
dt_ioppr_set_pipe_input_profile_info(struct dt_develop_t *dev,
                                     struct dt_dev_pixelpipe_t *pipe,
                                     const dt_colorspaces_color_profile_type_t type,
                                     const char *filename,
                                     const int intent,
                                     const dt_colormatrix_t *matrix_in);

dt_iop_order_iccprofile_info_t *
dt_ioppr_set_pipe_output_profile_info(struct dt_develop_t *dev,
                                      struct dt_dev_pixelpipe_t *pipe,
                                      const dt_colorspaces_color_profile_type_t type,
                                      const char *filename,
                                      const int intent);

/** returns a reference to the histogram profile info
 * histogram profile must not be cleanup()
 */
dt_iop_order_iccprofile_info_t *dt_ioppr_get_histogram_profile_info(struct dt_develop_t *dev);

/** returns the active work/input/output profile on the pipe */
dt_iop_order_iccprofile_info_t *dt_ioppr_get_pipe_work_profile_info(struct dt_dev_pixelpipe_t *pipe);
dt_iop_order_iccprofile_info_t *dt_ioppr_get_pipe_input_profile_info(struct dt_dev_pixelpipe_t *pipe);
dt_iop_order_iccprofile_info_t *dt_ioppr_get_pipe_output_profile_info(struct dt_dev_pixelpipe_t *pipe);

/** Get the relevant RGB -> XYZ profile at the position of current module */
dt_iop_order_iccprofile_info_t *dt_ioppr_get_pipe_current_profile_info(struct dt_iop_module_t *module,
                                                                       struct dt_dev_pixelpipe_t *pipe);

/** returns the current setting of the work profile on colorin iop */
void dt_ioppr_get_work_profile_type(struct dt_develop_t *dev,
                                    dt_colorspaces_color_profile_type_t *profile_type,
                                    const char **profile_filename);
/** returns the current setting of the input profile on colorin iop */
void dt_ioppr_get_input_profile_type(struct dt_develop_t *dev,
                                     dt_colorspaces_color_profile_type_t *profile_type,
                                     const char **profile_filename);
/** returns the current setting of the export profile on colorout iop */
void dt_ioppr_get_export_profile_type(struct dt_develop_t *dev,
                                      dt_colorspaces_color_profile_type_t *profile_type,
                                      const char **profile_filename);
/** returns the current setting of the histogram profile */
void dt_ioppr_get_histogram_profile_type(dt_colorspaces_color_profile_type_t *profile_type,
                                         const char **profile_filename);

/** transforms image from cst_from to cst_to colorspace using profile_info */
void dt_ioppr_transform_image_colorspace(struct dt_iop_module_t *self,
                                         const float *const image_in,
                                         float *const image_out,
                                         const int width,
                                         const int height,
                                         const int cst_from,
                                         const int cst_to,
                                         int *converted_cst,
                                         const dt_iop_order_iccprofile_info_t *const profile_info);

void dt_ioppr_transform_image_colorspace_rgb
  (const float *const image_in,
   float *const image_out,
   const int width,
   const int height,
   const dt_iop_order_iccprofile_info_t *const profile_info_from,
   const dt_iop_order_iccprofile_info_t *const profile_info_to,
   const char *message);

#ifdef HAVE_OPENCL
typedef struct dt_colorspaces_cl_global_t
{
  int kernel_colorspaces_transform_lab_to_rgb_matrix;
  int kernel_colorspaces_transform_rgb_matrix_to_lab;
  int kernel_colorspaces_transform_rgb_matrix_to_rgb;
} dt_colorspaces_cl_global_t;

// must be in synch with colorspaces.cl dt_colorspaces_iccprofile_info_cl_t
typedef struct dt_colorspaces_iccprofile_info_cl_t
{
  cl_float matrix_in[9];
  cl_float matrix_out[9];
  cl_int lutsize;
  cl_float unbounded_coeffs_in[3][3];
  cl_float unbounded_coeffs_out[3][3];
  cl_int nonlinearlut;
  cl_float grey;
} dt_colorspaces_iccprofile_info_cl_t;

dt_colorspaces_cl_global_t *dt_colorspaces_init_cl_global(void);
void dt_colorspaces_free_cl_global(dt_colorspaces_cl_global_t *g);

/** sets profile_info_cl using profile_info
 * to be used as a parameter when calling opencl
 */
void dt_ioppr_get_profile_info_cl(const dt_iop_order_iccprofile_info_t *const profile_info,
                                  dt_colorspaces_iccprofile_info_cl_t *profile_info_cl);
/** returns the profile_info trc
 * to be used as a parameter when calling opencl
 */
cl_float *dt_ioppr_get_trc_cl(const dt_iop_order_iccprofile_info_t *const profile_info);

/** build the required parameters for a kernel that uses a profile info */
cl_int dt_ioppr_build_iccprofile_params_cl(const dt_iop_order_iccprofile_info_t *const profile_info,
                                           const int devid,
                                           dt_colorspaces_iccprofile_info_cl_t **_profile_info_cl,
                                           cl_float **_profile_lut_cl,
                                           cl_mem *_dev_profile_info,
                                           cl_mem *_dev_profile_lut);
/** free parameters build with the previous function */
void dt_ioppr_free_iccprofile_params_cl(dt_colorspaces_iccprofile_info_cl_t **_profile_info_cl,
                                        cl_float **_profile_lut_cl, cl_mem *_dev_profile_info,
                                        cl_mem *_dev_profile_lut);

/** same as the C version, both return TRUE in case everything went fine */
gboolean dt_ioppr_transform_image_colorspace_cl
  (struct dt_iop_module_t *self,
   const int devid,
   cl_mem dev_img_in,
   cl_mem dev_img_out,
   const int width,
   const int height,
   const int cst_from,
   const int cst_to,
   int *converted_cst,
   const dt_iop_order_iccprofile_info_t *const profile_info);

gboolean dt_ioppr_transform_image_colorspace_rgb_cl
  (const int devid,
   cl_mem dev_img_in,
   cl_mem dev_img_out,
   const int width,
   const int height,
   const dt_iop_order_iccprofile_info_t *const profile_info_from,
   const dt_iop_order_iccprofile_info_t *const profile_info_to,
   const char *message);
#endif

/** the following must have the matrix_in and matrix_out generated */

#ifdef _OPENMP
#pragma omp declare simd aligned(lut:64)
#endif
static inline float extrapolate_lut(const float *const lut, const float v, const int lutsize)
{
  // TODO: check if optimization is worthwhile!
  const float ft = CLAMPS(v * (lutsize - 1), 0, lutsize - 1);
  const int t = (ft < lutsize - 2) ? ft : lutsize - 2;
  const float f = ft - t;
  const float l1 = lut[t];
  const float l2 = lut[t + 1];
  return l1 * (1.0f - f) + l2 * f;
}


#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline float eval_exp(const float coeff[3], const float x)
{
  return coeff[1] * powf(x * coeff[0], coeff[2]);
}


#ifdef _OPENMP
#pragma omp declare simd \
  aligned(rgb_in, rgb_out, unbounded_coeffs:16) \
  aligned(lut:64) \
  uniform(rgb_in, rgb_out, unbounded_coeffs, lut)
#endif
static inline void dt_ioppr_apply_trc(const dt_aligned_pixel_t rgb_in,
                                      dt_aligned_pixel_t rgb_out,
                                      float *const lut[3],
                                      const float unbounded_coeffs[3][3],
                                      const int lutsize)
{
  for(int c = 0; c < 3; c++)
  {
    rgb_out[c] = (lut[c][0] >= 0.0f)
      ? ((rgb_in[c] < 1.0f)
         ? extrapolate_lut(lut[c], rgb_in[c], lutsize)
         : eval_exp(unbounded_coeffs[c], rgb_in[c]))
      : rgb_in[c];
  }
}

#ifdef _OPENMP
#pragma omp declare simd \
  aligned(rgb, matrix_in, unbounded_coeffs_in:16) \
  aligned(lut_in:64) \
  uniform(rgb, matrix_in, lut_in, unbounded_coeffs_in)
#endif
static inline float dt_ioppr_get_rgb_matrix_luminance(const dt_aligned_pixel_t rgb,
                                                      const dt_colormatrix_t matrix_in,
                                                      float *const lut_in[3],
                                                      const float unbounded_coeffs_in[3][3],
                                                      const int lutsize,
                                                      const int nonlinearlut)
{
  float luminance = 0.f;

  if(nonlinearlut)
  {
    dt_aligned_pixel_t linear_rgb;
    dt_ioppr_apply_trc(rgb, linear_rgb, lut_in, unbounded_coeffs_in, lutsize);
    luminance = matrix_in[1][0] * linear_rgb[0]
              + matrix_in[1][1] * linear_rgb[1]
              + matrix_in[1][2] * linear_rgb[2];
  }
  else
    luminance = matrix_in[1][0] * rgb[0] + matrix_in[1][1] * rgb[1] + matrix_in[1][2] * rgb[2];

  return luminance;
}


#ifdef _OPENMP
#pragma omp declare simd \
  aligned(unbounded_coeffs_in:16) \
  aligned(lut_in:64) \
  uniform(lut_in, unbounded_coeffs_in)
#endif
static inline void dt_ioppr_rgb_matrix_to_xyz(const dt_aligned_pixel_t rgb,
                                              dt_aligned_pixel_t xyz,
                                              const dt_colormatrix_t matrix_in_transposed,
                                              float *const lut_in[3],
                                              const float unbounded_coeffs_in[3][3],
                                              const int lutsize,
                                              const int nonlinearlut)
{
  if(nonlinearlut)
  {
    dt_aligned_pixel_t linear_rgb;
    dt_ioppr_apply_trc(rgb, linear_rgb, lut_in, unbounded_coeffs_in, lutsize);
    dt_apply_transposed_color_matrix(linear_rgb, matrix_in_transposed, xyz);
  }
  else
    dt_apply_transposed_color_matrix(rgb, matrix_in_transposed, xyz);
}

#ifdef _OPENMP
#pragma omp declare simd \
  aligned(unbounded_coeffs_out:16) \
  aligned(lut_out:64) \
  uniform(lut_out, unbounded_coeffs_out)
#endif
static inline void dt_ioppr_xyz_to_rgb_matrix(const dt_aligned_pixel_t xyz,
                                              dt_aligned_pixel_t rgb,
                                              const dt_colormatrix_t matrix_out_transposed,
                                              float *const lut_out[3],
                                              const float unbounded_coeffs_out[3][3],
                                              const int lutsize,
                                              const int nonlinearlut)
{
  if(nonlinearlut)
  {
    dt_aligned_pixel_t linear_rgb;
    dt_apply_transposed_color_matrix(xyz, matrix_out_transposed, linear_rgb);
    dt_ioppr_apply_trc(linear_rgb, rgb, lut_out, unbounded_coeffs_out, lutsize);
  }
  else
    dt_apply_transposed_color_matrix(xyz, matrix_out_transposed, rgb);
}


#ifdef _OPENMP
#pragma omp declare simd \
  aligned(unbounded_coeffs_out:16) \
  aligned(lut_out:64) \
  uniform(lut_out, unbounded_coeffs_out)
#endif
static inline void dt_ioppr_lab_to_rgb_matrix(const dt_aligned_pixel_t lab,
                                              dt_aligned_pixel_t rgb,
                                              const dt_colormatrix_t matrix_out_transposed,
                                              float *const lut_out[3],
                                              const float unbounded_coeffs_out[3][3],
                                              const int lutsize,
                                              const int nonlinearlut)
{
  dt_aligned_pixel_t xyz;
  dt_Lab_to_XYZ(lab, xyz);

  if(nonlinearlut)
  {
    dt_aligned_pixel_t linear_rgb;
    dt_apply_transposed_color_matrix(xyz, matrix_out_transposed, linear_rgb);
    dt_ioppr_apply_trc(linear_rgb, rgb, lut_out, unbounded_coeffs_out, lutsize);
  }
  else
  {
    dt_apply_transposed_color_matrix(xyz, matrix_out_transposed, rgb);
  }
}

#ifdef _OPENMP
#pragma omp declare simd \
  aligned(unbounded_coeffs_in:16) \
  aligned(lut_in:64) \
  uniform(lut_in, unbounded_coeffs_in)
#endif
static inline void dt_ioppr_rgb_matrix_to_lab(const dt_aligned_pixel_t rgb,
                                              dt_aligned_pixel_t lab,
                                              const dt_colormatrix_t matrix_in_transposed,
                                              float *const lut_in[3],
                                              const float unbounded_coeffs_in[3][3],
                                              const int lutsize,
                                              const int nonlinearlut)
{
  dt_aligned_pixel_t xyz = { 0.f };
  dt_ioppr_rgb_matrix_to_xyz(rgb, xyz, matrix_in_transposed, lut_in,
                             unbounded_coeffs_in, lutsize, nonlinearlut);
  dt_XYZ_to_Lab(xyz, lab);
}

static inline float dt_ioppr_get_profile_info_middle_grey
  (const dt_iop_order_iccprofile_info_t *const profile_info)
{
  return profile_info->grey;
}

#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline float dt_ioppr_compensate_middle_grey
  (const float x,
   const dt_iop_order_iccprofile_info_t *const profile_info)
{
  // we transform the curve nodes from the image colorspace to lab
  dt_aligned_pixel_t lab = { 0.0f };
  const dt_aligned_pixel_t rgb = { x, x, x };
  dt_ioppr_rgb_matrix_to_lab(rgb, lab, profile_info->matrix_in_transposed, profile_info->lut_in,
                             profile_info->unbounded_coeffs_in,
                             profile_info->lutsize,
                             profile_info->nonlinearlut);
  return lab[0] * .01f;
}

#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline float dt_ioppr_uncompensate_middle_grey
  (const float x,
   const dt_iop_order_iccprofile_info_t *const profile_info)
{
  // we transform the curve nodes from lab to the image colorspace
  const dt_aligned_pixel_t lab = { x * 100.f, 0.0f, 0.0f };
  dt_aligned_pixel_t rgb = { 0.0f };

  dt_ioppr_lab_to_rgb_matrix(lab, rgb, profile_info->matrix_out_transposed, profile_info->lut_out,
                             profile_info->unbounded_coeffs_out,
                             profile_info->lutsize, profile_info->nonlinearlut);
  return rgb[0];
}

#endif
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
