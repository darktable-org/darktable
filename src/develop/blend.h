/*
    This file is part of darktable,
    Copyright (C) 2011-2020 darktable developers.

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

#pragma once

#include "common/iop_profile.h"
#include "common/opencl.h"
#include "develop/pixelpipe.h"
#include "dtgtk/button.h"
#include "dtgtk/gradientslider.h"
#include "gui/color_picker_proxy.h"

#define DEVELOP_BLEND_VERSION (9)

typedef enum dt_develop_blend_mode_t
{
  DEVELOP_BLEND_DISABLED = 0x00,
  DEVELOP_BLEND_NORMAL = 0x01, /* deprecated as it did clamping */
  DEVELOP_BLEND_LIGHTEN = 0x02,
  DEVELOP_BLEND_DARKEN = 0x03,
  DEVELOP_BLEND_MULTIPLY = 0x04,
  DEVELOP_BLEND_AVERAGE = 0x05,
  DEVELOP_BLEND_ADD = 0x06,
  DEVELOP_BLEND_SUBSTRACT = 0x07,
  DEVELOP_BLEND_DIFFERENCE = 0x08, /* deprecated */
  DEVELOP_BLEND_SCREEN = 0x09,
  DEVELOP_BLEND_OVERLAY = 0x0A,
  DEVELOP_BLEND_SOFTLIGHT = 0x0B,
  DEVELOP_BLEND_HARDLIGHT = 0x0C,
  DEVELOP_BLEND_VIVIDLIGHT = 0x0D,
  DEVELOP_BLEND_LINEARLIGHT = 0x0E,
  DEVELOP_BLEND_PINLIGHT = 0x0F,
  DEVELOP_BLEND_LIGHTNESS = 0x10,
  DEVELOP_BLEND_CHROMA = 0x11,
  DEVELOP_BLEND_HUE = 0x12,
  DEVELOP_BLEND_COLOR = 0x13,
  DEVELOP_BLEND_INVERSE = 0x14,   /* deprecated */
  DEVELOP_BLEND_UNBOUNDED = 0x15, /* deprecated as new normal takes over */
  DEVELOP_BLEND_COLORADJUST = 0x16,
  DEVELOP_BLEND_DIFFERENCE2 = 0x17,
  DEVELOP_BLEND_NORMAL2 = 0x18,
  DEVELOP_BLEND_BOUNDED = 0x19,
  DEVELOP_BLEND_LAB_LIGHTNESS = 0x1A,
  DEVELOP_BLEND_LAB_COLOR = 0x1B,
  DEVELOP_BLEND_HSV_LIGHTNESS = 0x1C,
  DEVELOP_BLEND_HSV_COLOR = 0x1D,
  DEVELOP_BLEND_LAB_L = 0x1E,
  DEVELOP_BLEND_LAB_A = 0x1F,
  DEVELOP_BLEND_LAB_B = 0x20,
  DEVELOP_BLEND_RGB_R = 0x21,
  DEVELOP_BLEND_RGB_G = 0x22,
  DEVELOP_BLEND_RGB_B = 0x23
} dt_develop_blend_mode_t;

typedef enum dt_develop_mask_mode_t
{
  DEVELOP_MASK_DISABLED = 0,                                                         // off
  DEVELOP_MASK_ENABLED = 1,                                                          // uniformly
  DEVELOP_MASK_MASK = 1 << 1,                                                        // drawn mask
  DEVELOP_MASK_CONDITIONAL = 1 << 2,                                                 // parametric mask
  DEVELOP_MASK_RASTER = 1 << 3,                                                      // raster mask
  DEVELOP_MASK_MASK_CONDITIONAL = (DEVELOP_MASK_MASK | DEVELOP_MASK_CONDITIONAL)     // drawn & parametric
} dt_develop_mask_mode_t;

typedef enum dt_develop_mask_combine_mode_t
{
  DEVELOP_COMBINE_NORM = 0x00,
  DEVELOP_COMBINE_INV = 0x01,
  DEVELOP_COMBINE_EXCL = 0x00,
  DEVELOP_COMBINE_INCL = 0x02,
  DEVELOP_COMBINE_MASKS_POS = 0x04,
  DEVELOP_COMBINE_NORM_EXCL = (DEVELOP_COMBINE_NORM | DEVELOP_COMBINE_EXCL),
  DEVELOP_COMBINE_NORM_INCL = (DEVELOP_COMBINE_NORM | DEVELOP_COMBINE_INCL),
  DEVELOP_COMBINE_INV_EXCL = (DEVELOP_COMBINE_INV | DEVELOP_COMBINE_EXCL),
  DEVELOP_COMBINE_INV_INCL = (DEVELOP_COMBINE_INV | DEVELOP_COMBINE_INCL)
} dt_develop_mask_combine_mode_t;

typedef enum dt_develop_mask_feathering_guide_t
{
  DEVELOP_MASK_GUIDE_IN = 0x01,
  DEVELOP_MASK_GUIDE_OUT = 0x02
} dt_develop_mask_feathering_guide_t;

typedef enum dt_develop_blendif_channels_t
{
  DEVELOP_BLENDIF_L_in = 0,
  DEVELOP_BLENDIF_A_in = 1,
  DEVELOP_BLENDIF_B_in = 2,

  DEVELOP_BLENDIF_L_out = 4,
  DEVELOP_BLENDIF_A_out = 5,
  DEVELOP_BLENDIF_B_out = 6,

  DEVELOP_BLENDIF_GRAY_in = 0,
  DEVELOP_BLENDIF_RED_in = 1,
  DEVELOP_BLENDIF_GREEN_in = 2,
  DEVELOP_BLENDIF_BLUE_in = 3,

  DEVELOP_BLENDIF_GRAY_out = 4,
  DEVELOP_BLENDIF_RED_out = 5,
  DEVELOP_BLENDIF_GREEN_out = 6,
  DEVELOP_BLENDIF_BLUE_out = 7,

  DEVELOP_BLENDIF_C_in = 8,
  DEVELOP_BLENDIF_h_in = 9,

  DEVELOP_BLENDIF_C_out = 12,
  DEVELOP_BLENDIF_h_out = 13,

  DEVELOP_BLENDIF_H_in = 8,
  DEVELOP_BLENDIF_S_in = 9,
  DEVELOP_BLENDIF_l_in = 10,

  DEVELOP_BLENDIF_H_out = 12,
  DEVELOP_BLENDIF_S_out = 13,
  DEVELOP_BLENDIF_l_out = 14,

  DEVELOP_BLENDIF_MAX = 14,
  DEVELOP_BLENDIF_unused = 15,

  DEVELOP_BLENDIF_active = 31,

  DEVELOP_BLENDIF_SIZE = 16,

  DEVELOP_BLENDIF_Lab_MASK = 0x3377,
  DEVELOP_BLENDIF_RGB_MASK = 0x77FF
} dt_develop_blendif_channels_t;


/** blend legacy parameters version 1 */
typedef struct dt_develop_blend_params1_t
{
  uint32_t mode;
  float opacity;
  uint32_t mask_id;
} dt_develop_blend_params1_t;

/** blend legacy parameters version 2 */
typedef struct dt_develop_blend_params2_t
{
  /** blending mode */
  uint32_t mode;
  /** mixing opacity */
  float opacity;
  /** id of mask in current pipeline */
  uint32_t mask_id;
  /** blendif mask */
  uint32_t blendif;
  /** blendif parameters */
  float blendif_parameters[4 * 8];
} dt_develop_blend_params2_t;

/** blend legacy parameters version 3 */
typedef struct dt_develop_blend_params3_t
{
  /** blending mode */
  uint32_t mode;
  /** mixing opacity */
  float opacity;
  /** id of mask in current pipeline */
  uint32_t mask_id;
  /** blendif mask */
  uint32_t blendif;
  /** blendif parameters */
  float blendif_parameters[4 * DEVELOP_BLENDIF_SIZE];
} dt_develop_blend_params3_t;

/** blend legacy parameters version 4 */
typedef struct dt_develop_blend_params4_t
{
  /** blending mode */
  uint32_t mode;
  /** mixing opacity */
  float opacity;
  /** id of mask in current pipeline */
  uint32_t mask_id;
  /** blendif mask */
  uint32_t blendif;
  /** blur radius */
  float radius;
  /** blendif parameters */
  float blendif_parameters[4 * DEVELOP_BLENDIF_SIZE];
} dt_develop_blend_params4_t;

/** blend legacy parameters version 5 (identical to version 6)*/
typedef struct dt_develop_blend_params5_t
{
  /** what kind of masking to use: off, non-mask (uniformly), hand-drawn mask and/or conditional mask */
  uint32_t mask_mode;
  /** blending mode */
  uint32_t blend_mode;
  /** mixing opacity */
  float opacity;
  /** how masks are combined */
  uint32_t mask_combine;
  /** id of mask in current pipeline */
  uint32_t mask_id;
  /** blendif mask */
  uint32_t blendif;
  /** blur radius */
  float radius;
  /** some reserved fields for future use */
  uint32_t reserved[4];
  /** blendif parameters */
  float blendif_parameters[4 * DEVELOP_BLENDIF_SIZE];
} dt_develop_blend_params5_t;

/** blend legacy parameters version 6 (identical to version 7) */
typedef struct dt_develop_blend_params6_t
{
  /** what kind of masking to use: off, non-mask (uniformly), hand-drawn mask and/or conditional mask */
  uint32_t mask_mode;
  /** blending mode */
  uint32_t blend_mode;
  /** mixing opacity */
  float opacity;
  /** how masks are combined */
  uint32_t mask_combine;
  /** id of mask in current pipeline */
  uint32_t mask_id;
  /** blendif mask */
  uint32_t blendif;
  /** blur radius */
  float radius;
  /** some reserved fields for future use */
  uint32_t reserved[4];
  /** blendif parameters */
  float blendif_parameters[4 * DEVELOP_BLENDIF_SIZE];
} dt_develop_blend_params6_t;

/** blend legacy parameters version 7 */
typedef struct dt_develop_blend_params7_t
{
  /** what kind of masking to use: off, non-mask (uniformly), hand-drawn mask and/or conditional mask */
  uint32_t mask_mode;
  /** blending mode */
  uint32_t blend_mode;
  /** mixing opacity */
  float opacity;
  /** how masks are combined */
  uint32_t mask_combine;
  /** id of mask in current pipeline */
  uint32_t mask_id;
  /** blendif mask */
  uint32_t blendif;
  /** blur radius */
  float radius;
  /** some reserved fields for future use */
  uint32_t reserved[4];
  /** blendif parameters */
  float blendif_parameters[4 * DEVELOP_BLENDIF_SIZE];
} dt_develop_blend_params7_t;

/** blend legacy parameters version 8 */
typedef struct dt_develop_blend_params8_t
{
  /** what kind of masking to use: off, non-mask (uniformly), hand-drawn mask and/or conditional mask */
  uint32_t mask_mode;
  /** blending mode */
  uint32_t blend_mode;
  /** mixing opacity */
  float opacity;
  /** how masks are combined */
  uint32_t mask_combine;
  /** id of mask in current pipeline */
  uint32_t mask_id;
  /** blendif mask */
  uint32_t blendif;
  /** feathering radius */
  float feathering_radius;
  /** feathering guide */
  uint32_t feathering_guide;
  /** blur radius */
  float blur_radius;
  /** mask contrast enhancement */
  float contrast;
  /** mask brightness adjustment */
  float brightness;
  /** some reserved fields for future use */
  uint32_t reserved[4];
  /** blendif parameters */
  float blendif_parameters[4 * DEVELOP_BLENDIF_SIZE];
} dt_develop_blend_params8_t;

/** blend parameters current version */
typedef struct dt_develop_blend_params_t
{
  /** what kind of masking to use: off, non-mask (uniformly), hand-drawn mask and/or conditional mask
   *  or raster mask */
  uint32_t mask_mode;
  /** blending mode */
  uint32_t blend_mode;
  /** mixing opacity */
  float opacity;
  /** how masks are combined */
  uint32_t mask_combine;
  /** id of mask in current pipeline */
  uint32_t mask_id;
  /** blendif mask */
  uint32_t blendif;
  /** feathering radius */
  float feathering_radius;
  /** feathering guide */
  uint32_t feathering_guide;
  /** blur radius */
  float blur_radius;
  /** mask contrast enhancement */
  float contrast;
  /** mask brightness adjustment */
  float brightness;
  /** some reserved fields for future use */
  uint32_t reserved[4];
  /** blendif parameters */
  float blendif_parameters[4 * DEVELOP_BLENDIF_SIZE];
  dt_dev_operation_t raster_mask_source;
  int raster_mask_instance;
  int raster_mask_id;
  gboolean raster_mask_invert;
} dt_develop_blend_params_t;


typedef struct dt_blendop_cl_global_t
{
  int kernel_blendop_mask_Lab;
  int kernel_blendop_mask_RAW;
  int kernel_blendop_mask_rgb;
  int kernel_blendop_Lab;
  int kernel_blendop_RAW;
  int kernel_blendop_rgb;
  int kernel_blendop_mask_tone_curve;
  int kernel_blendop_set_mask;
  int kernel_blendop_display_channel;
} dt_blendop_cl_global_t;


typedef struct dt_iop_gui_blendif_colorstop_t
{
  float stoppoint;
  GdkRGBA color;
} dt_iop_gui_blendif_colorstop_t;

typedef struct dt_iop_gui_blendif_channel_t
{
  char *label;
  char *tooltip;
  float increment;
  int numberstops;
  const dt_iop_gui_blendif_colorstop_t *colorstops;
  dt_develop_blendif_channels_t param_channels[2];
  dt_dev_pixelpipe_display_mask_t display_channel;
  void (*scale_print)(float value, char *string, int n);
  int (*altdisplay)(GtkWidget *, dt_iop_module_t *, int);
  char *name;
} dt_iop_gui_blendif_channel_t;

typedef struct dt_iop_gui_blendif_filter_t
{
  GtkDarktableGradientSlider *slider;
  GtkLabel *head;
  GtkLabel *label[4];
  GtkLabel *picker_label;
  GtkWidget *polarity;
} dt_iop_gui_blendif_filter_t;

typedef struct dt_iop_blend_name_value_t
{
  char name[25];
  int value;
} dt_develop_name_value_t;

extern const dt_develop_name_value_t dt_develop_blend_mode_names[];
extern const dt_develop_name_value_t dt_develop_mask_mode_names[];
extern const dt_develop_name_value_t dt_develop_combine_masks_names[];
extern const dt_develop_name_value_t dt_develop_feathering_guide_names[];
extern const dt_develop_name_value_t dt_develop_invert_mask_names[];

#define DEVELOP_MASKS_NB_SHAPES 5

/** blend gui data */
typedef struct dt_iop_gui_blend_data_t
{
  int blendif_support;
  int blend_inited;
  int blendif_inited;
  int masks_support;
  int masks_inited;
  int raster_inited;

  dt_iop_colorspace_type_t csp;
  dt_iop_module_t *module;

  GList *masks_modes;
  GList *masks_modes_toggles;

  GtkWidget *iopw;
  GtkBox *top_box;
  GtkBox *bottom_box;
  GtkBox *masks_modes_box;
  GtkBox *blendif_box;
  GtkBox *masks_box;
  GtkBox *raster_box;

  GtkWidget *selected_mask_mode;
  GtkWidget *colorpicker;
  GtkWidget *colorpicker_set_values;
  dt_iop_gui_blendif_filter_t filter[2];
  GtkWidget *showmask;
  GtkWidget *suppress;
  GtkWidget *masks_combine_combo;
  GtkWidget *blend_modes_combo;
  GtkWidget *masks_invert_combo;
  GtkWidget *opacity_slider;
  GtkWidget *masks_feathering_guide_combo;
  GtkWidget *feathering_radius_slider;
  GtkWidget *blur_radius_slider;
  GtkWidget *contrast_slider;
  GtkWidget *brightness_slider;

  const dt_iop_gui_blendif_channel_t *channel;
  int tab;
  int altmode[8][2];
  dt_dev_pixelpipe_display_mask_t save_for_leave;
  int timeout_handle;
  GtkNotebook *channel_tabs;

  GtkWidget *masks_combo;
  GtkWidget *masks_shapes[DEVELOP_MASKS_NB_SHAPES];
  int masks_type[DEVELOP_MASKS_NB_SHAPES];
  GtkWidget *masks_edit;
  GtkWidget *masks_polarity;
  int *masks_combo_ids;
  int masks_shown;

  GtkWidget *raster_combo;
  GtkWidget *raster_polarity;

  int control_button_pressed;
  dt_pthread_mutex_t lock;
} dt_iop_gui_blend_data_t;


/** global init of blendops */
dt_blendop_cl_global_t *dt_develop_blend_init_cl_global(void);
/** global cleanup of blendops */
void dt_develop_blend_free_cl_global(dt_blendop_cl_global_t *b);

/** apply blend */
void dt_develop_blend_process(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                              const void *const i, void *const o, const struct dt_iop_roi_t *const roi_in,
                              const struct dt_iop_roi_t *const roi_out);

/** get blend version */
int dt_develop_blend_version(void);

/** check if content of params is all zero, indicating a non-initialized set of blend parameters which needs
 * special care. */
gboolean dt_develop_blend_params_is_all_zero(const void *params, size_t length);

/** update blendop params from older versions */
int dt_develop_blend_legacy_params(dt_iop_module_t *module, const void *const old_params,
                                   const int old_version, void *new_params, const int new_version,
                                   const int length);
int dt_develop_blend_legacy_params_from_so(dt_iop_module_so_t *module_so, const void *const old_params,
                                           const int old_version, void *new_params, const int new_version,
                                           const int length);

/** gui related stuff */
void dt_iop_gui_init_blendif(GtkBox *blendw, dt_iop_module_t *module);
void dt_iop_gui_init_blending(GtkWidget *iopw, dt_iop_module_t *module);
void dt_iop_gui_update_blending(dt_iop_module_t *module);
void dt_iop_gui_update_blendif(dt_iop_module_t *module);
void dt_iop_gui_update_masks(dt_iop_module_t *module);
void dt_iop_gui_cleanup_blending(dt_iop_module_t *module);
void dt_iop_gui_blending_lose_focus(dt_iop_module_t *module);

gboolean blend_color_picker_apply(dt_iop_module_t *module, GtkWidget *picker, dt_dev_pixelpipe_iop_t *piece);

/** routine to translate from mode id to sequence in option list */
int dt_iop_gui_blending_mode_seq(dt_iop_gui_blend_data_t *bd, int mode);


#ifdef HAVE_OPENCL
/** apply blend for opencl modules*/
int dt_develop_blend_process_cl(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                                cl_mem dev_in, cl_mem dev_out, const struct dt_iop_roi_t *roi_in,
                                const struct dt_iop_roi_t *roi_out);
#endif

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
