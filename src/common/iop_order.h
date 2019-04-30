/*
    This file is part of darktable,
    copyright (c) 2018 edgardo hoszowski.

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

#ifndef DT_IOP_ORDER_H
#define DT_IOP_ORDER_H

#ifdef HAVE_OPENCL
#include "CL/cl.h"           // for cl_mem
#endif

struct dt_iop_module_t;
struct dt_develop_t;
struct dt_dev_pixelpipe_t;

typedef struct dt_iop_order_entry_t
{
  double iop_order;
  char operation[20];
} dt_iop_order_entry_t;

typedef struct dt_iop_order_rule_t
{
  char op_prev[20];
  char op_next[20];
} dt_iop_order_rule_t;

/** returns a list of dt_iop_order_entry_t and updates *_version */
GList *dt_ioppr_get_iop_order_list(int *_version);
/** returns the dt_iop_order_entry_t of iop_order_list with operation = op_name */
dt_iop_order_entry_t *dt_ioppr_get_iop_order_entry(GList *iop_order_list, const char *op_name);
/** returns the iop_order from iop_order_list list with operation = op_name */
double dt_ioppr_get_iop_order(GList *iop_order_list, const char *op_name);

/** check if there's duplicate iop_order entries in iop_list */
void dt_ioppr_check_duplicate_iop_order(GList **_iop_list, GList *history_list);

/** sets the default iop_order to iop_list */
void dt_ioppr_set_default_iop_order(GList **_iop_list, GList *iop_order_list);
/** adjusts iop_list and iop_order_list to the current version of iop order */
void dt_ioppr_legacy_iop_order(GList **_iop_list, GList **_iop_order_list, GList *history_list, const int old_version);

/** returns 1 if there's a module_so without a iop_order defined */
int dt_ioppr_check_so_iop_order(GList *iop_list, GList *iop_order_list);

/* returns a list of dt_iop_order_rule_t with the current iop order rules */
GList *dt_ioppr_get_iop_order_rules();

/** returns a duplicate of iop_order_list */
GList *dt_ioppr_iop_order_copy_deep(GList *iop_order_list);

/** sort two modules by iop_order */
gint dt_sort_iop_by_order(gconstpointer a, gconstpointer b);

/** returns the iop_order before module_next if module can be moved */
double dt_ioppr_get_iop_order_before_iop(GList *iop_list, struct dt_iop_module_t *module, struct dt_iop_module_t *module_next,
                                  const int validate_order, const int log_error);
/** returns the iop_order after module_prev if module can be moved */
double dt_ioppr_get_iop_order_after_iop(GList *iop_list, struct dt_iop_module_t *module, struct dt_iop_module_t *module_prev,
                                 const int validate_order, const int log_error);

/** moves module before/after module_next/previous on pipe */
int dt_ioppr_move_iop_before(GList **_iop_list, struct dt_iop_module_t *module, struct dt_iop_module_t *module_next,
                       const int validate_order, const int log_error);
int dt_ioppr_move_iop_after(GList **_iop_list, struct dt_iop_module_t *module, struct dt_iop_module_t *module_prev,
                      const int validate_order, const int log_error);

// must be in synch with filename in dt_colorspaces_color_profile_t in colorspaces.h
#define DT_IOPPR_COLOR_ICC_LEN 512

typedef struct dt_iop_order_iccprofile_info_t
{
  int type; // a dt_colorspaces_color_profile_type_t
  char filename[DT_IOPPR_COLOR_ICC_LEN];
  int intent; // a dt_iop_color_intent_t
  float matrix_in[9];
  float matrix_out[9];
  int lutsize;
  float *lut_in[3];
  float *lut_out[3];
  float unbounded_coeffs_in[3][3];
  float unbounded_coeffs_out[3][3];
  int nonlinearlut;
  float grey;
} dt_iop_order_iccprofile_info_t;

#undef DT_IOPPR_COLOR_ICC_LEN

/** must be called before using profile_info, default lutsize = 0 */
void dt_ioppr_init_profile_info(dt_iop_order_iccprofile_info_t *profile_info, const int lutsize);
/** must be called when done with profile_info */
void dt_ioppr_cleanup_profile_info(dt_iop_order_iccprofile_info_t *profile_info);

/** returns the profile info from dev profiles info list that matches (profile_type, profile_filename)
 * NULL if not found
 */
dt_iop_order_iccprofile_info_t *dt_ioppr_get_profile_info_from_list(struct dt_develop_t *dev, const int profile_type, const char *profile_filename);
/** adds the profile info from (profile_type, profile_filename) to the dev profiles info list if not already exists
 * returns the generated profile or the existing one
 */
dt_iop_order_iccprofile_info_t *dt_ioppr_add_profile_info_to_list(struct dt_develop_t *dev, const int profile_type, const char *profile_filename, const int intent);

/** returns a reference to the work profile info as set on colorin iop
 * only if module is between colorin and colorout, otherwise returns NULL
 * work profile must not be cleanup()
 */
dt_iop_order_iccprofile_info_t *dt_ioppr_get_iop_work_profile_info(struct dt_iop_module_t *module, GList *iop_list);

/** set the work profile (type, filename) on the pipe, should be called on process*()
 * if matrix cannot be generated it default to linear rec 2020
 * returns the actual profile that has been set
 */
dt_iop_order_iccprofile_info_t *dt_ioppr_set_pipe_work_profile_info(struct dt_develop_t *dev, struct dt_dev_pixelpipe_t *pipe, 
    const int type, const char *filename, const int intent);
/** returns a reference to the histogram profile info
 * histogram profile must not be cleanup()
 */
dt_iop_order_iccprofile_info_t *dt_ioppr_get_histogram_profile_info(struct dt_develop_t *dev);

/** returns the active work profile on the pipe */
dt_iop_order_iccprofile_info_t *dt_ioppr_get_pipe_work_profile_info(struct dt_dev_pixelpipe_t *pipe);

/** returns the current setting of the work profile on colorin iop */
void dt_ioppr_get_work_profile_type(struct dt_develop_t *dev, int *profile_type, char **profile_filename);
/** returns the current setting of the export profile on colorout iop */
void dt_ioppr_get_export_profile_type(struct dt_develop_t *dev, int *profile_type, char **profile_filename);
/** returns the current setting of the histogram profile */
void dt_ioppr_get_histogram_profile_type(int *profile_type, char **profile_filename);

/** transforms image from cst_from to cst_to colorspace using profile_info */
void dt_ioppr_transform_image_colorspace(struct dt_iop_module_t *self, const float *const image_in,
                                         float *const image_out, const int width, const int height,
                                         const int cst_from, const int cst_to, int *converted_cst,
                                         const dt_iop_order_iccprofile_info_t *const profile_info);

void dt_ioppr_transform_image_colorspace_rgb(const float *const image_in, float *const image_out, const int width,
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
void dt_ioppr_get_profile_info_cl(const dt_iop_order_iccprofile_info_t *const profile_info, dt_colorspaces_iccprofile_info_cl_t *profile_info_cl);
/** returns the profile_info trc
 * to be used as a parameter when calling opencl
 */
cl_float *dt_ioppr_get_trc_cl(const dt_iop_order_iccprofile_info_t *const profile_info);

/** build the required parameters for a kernell that uses a profile info */
cl_int dt_ioppr_build_iccprofile_params_cl(const dt_iop_order_iccprofile_info_t *const profile_info,
                                           const int devid, dt_colorspaces_iccprofile_info_cl_t **_profile_info_cl,
                                           cl_float **_profile_lut_cl, cl_mem *_dev_profile_info,
                                           cl_mem *_dev_profile_lut);
/** free parameters build with the previous function */
void dt_ioppr_free_iccprofile_params_cl(dt_colorspaces_iccprofile_info_cl_t **_profile_info_cl,
                                        cl_float **_profile_lut_cl, cl_mem *_dev_profile_info,
                                        cl_mem *_dev_profile_lut);

/** same as the C version */
int dt_ioppr_transform_image_colorspace_cl(struct dt_iop_module_t *self, const int devid, cl_mem dev_img_in,
                                           cl_mem dev_img_out, const int width, const int height,
                                           const int cst_from, const int cst_to, int *converted_cst,
                                           const dt_iop_order_iccprofile_info_t *const profile_info);

int dt_ioppr_transform_image_colorspace_rgb_cl(const int devid, cl_mem dev_img_in, cl_mem dev_img_out,
                                               const int width, const int height,
                                               const dt_iop_order_iccprofile_info_t *const profile_info_from,
                                               const dt_iop_order_iccprofile_info_t *const profile_info_to,
                                               const char *message);
#endif

/** the following must have the matrix_in and matrix out generated */
float dt_ioppr_get_rgb_matrix_luminance(const float *const rgb, const dt_iop_order_iccprofile_info_t *const profile_info);
float dt_ioppr_get_profile_info_middle_grey(const dt_iop_order_iccprofile_info_t *const profile_info);

float dt_ioppr_compensate_middle_grey(const float x, const dt_iop_order_iccprofile_info_t *const profile_info);
float dt_ioppr_uncompensate_middle_grey(const float x, const dt_iop_order_iccprofile_info_t *const profile_info);

void dt_ioppr_rgb_matrix_to_xyz(const float *const rgb, float *xyz, const dt_iop_order_iccprofile_info_t *const profile_info);
void dt_ioppr_lab_to_rgb_matrix(const float *const lab, float *rgb, const dt_iop_order_iccprofile_info_t *const profile_info);
void dt_ioppr_rgb_matrix_to_lab(const float *const rgb, float *lab, const dt_iop_order_iccprofile_info_t *const profile_info);

// for debug only
int dt_ioppr_check_db_integrity();
int dt_ioppr_check_iop_order(struct dt_develop_t *dev, const int imgid, const char *msg);
void dt_ioppr_print_module_iop_order(GList *iop_list, const char *msg);
void dt_ioppr_print_history_iop_order(GList *history_list, const char *msg);
void dt_ioppr_print_iop_order(GList *iop_order_list, const char *msg);

#endif
