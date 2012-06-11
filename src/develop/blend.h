/*
    This file is part of darktable,
    copyright (c) 2011 henrik andersson.

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

#ifndef DT_DEVELOP_BLEND_H
#define DT_DEVELOP_BLEND_H

#include "dtgtk/button.h"
#include "dtgtk/icon.h"
#include "dtgtk/tristatebutton.h"
#include "dtgtk/slider.h"
#include "dtgtk/tristatebutton.h"
#include "dtgtk/gradientslider.h"
#include "develop/pixelpipe.h"
#include "common/opencl.h"

#define DEVELOP_BLEND_VERSION				(2)


#define DEVELOP_BLEND_MASK_FLAG				0x80
#define DEVELOP_BLEND_DISABLED				0x00
#define DEVELOP_BLEND_NORMAL				0x01
#define DEVELOP_BLEND_LIGHTEN				0x02
#define DEVELOP_BLEND_DARKEN				0x03
#define DEVELOP_BLEND_MULTIPLY				0x04
#define DEVELOP_BLEND_AVERAGE				0x05
#define DEVELOP_BLEND_ADD					0x06
#define DEVELOP_BLEND_SUBSTRACT				0x07
#define DEVELOP_BLEND_DIFFERENCE				0x08
#define DEVELOP_BLEND_SCREEN				0x09
#define DEVELOP_BLEND_OVERLAY				0x0A
#define DEVELOP_BLEND_SOFTLIGHT			0x0B
#define DEVELOP_BLEND_HARDLIGHT			0x0C
#define DEVELOP_BLEND_VIVIDLIGHT			0x0D
#define DEVELOP_BLEND_LINEARLIGHT			0x0E
#define DEVELOP_BLEND_PINLIGHT				0x0F
#define DEVELOP_BLEND_LIGHTNESS				0x10
#define DEVELOP_BLEND_CHROMA				0x11
#define DEVELOP_BLEND_HUE				0x12
#define DEVELOP_BLEND_COLOR				0x13
#define DEVELOP_BLEND_INVERSE				0x14


typedef enum dt_develop_blendif_channels_t
{
  DEVELOP_BLENDIF_L_low     = 0,
  DEVELOP_BLENDIF_A_low     = 1,
  DEVELOP_BLENDIF_B_low     = 2,

  DEVELOP_BLENDIF_L_up      = 4,
  DEVELOP_BLENDIF_A_up      = 5,
  DEVELOP_BLENDIF_B_up      = 6,

  DEVELOP_BLENDIF_GRAY_low  = 0,
  DEVELOP_BLENDIF_RED_low   = 1,
  DEVELOP_BLENDIF_GREEN_low = 2,
  DEVELOP_BLENDIF_BLUE_low  = 3,

  DEVELOP_BLENDIF_GRAY_up   = 4,
  DEVELOP_BLENDIF_RED_up    = 5,
  DEVELOP_BLENDIF_GREEN_up  = 6,
  DEVELOP_BLENDIF_BLUE_up   = 7,


  DEVELOP_BLENDIF_MAX   = 8
}
dt_develop_blendif_channels_t;


typedef struct dt_develop_blend_params_t
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
  float blendif_parameters[4*DEVELOP_BLENDIF_MAX];
}
dt_develop_blend_params_t;



typedef struct dt_blendop_t
{
  int kernel_blendop_Lab;
  int kernel_blendop_RAW;
  int kernel_blendop_rgb;
  int kernel_blendop_copy_alpha;
}
dt_blendop_t;


/** blend legacy parameters version 1 */
typedef struct dt_develop_blend_1_params_t
{
  uint32_t mode;
  float opacity;
  uint32_t mask_id;
}
dt_develop_blend_1_params_t;


typedef struct dt_iop_gui_blendop_modes_t
{
  int mode;
  char *name;
}
dt_iop_gui_blendop_modes_t;



/** blend gui data */
typedef struct dt_iop_gui_blend_data_t
{
  int blendif_support;
  dt_iop_colorspace_type_t csp;
  dt_iop_module_t *module;
  dt_iop_gui_blendop_modes_t modes[30];
  int number_modes;
  GtkWidget *iopw;
  GtkWidget *blendif_enable;
  GtkVBox *box;
  GtkVBox *blendif_box;
  GtkDarktableGradientSlider *upper_slider;
  GtkDarktableGradientSlider *lower_slider;
  GtkLabel *upper_label[4];
  GtkLabel *lower_label[4];
  GtkLabel *upper_picker_label;
  GtkLabel *lower_picker_label;
  void (*scale_print[4])(float value, char *string, int n);
  GtkWidget *blend_modes_combo;
  GtkWidget *opacity_slider;
  int channel;
  GtkNotebook* channel_tabs;
  GdkColor colors[4][3];
  float increments[4];
}
dt_iop_gui_blend_data_t;




#define DT_DEVELOP_BLEND_WITH_MASK(p) ((p->mode&DEVELOP_BLEND_MASK_FLAG)?1:0)

/** global init of blendops */
void dt_develop_blend_init(dt_blendop_t *gd);

/** apply blend */
void dt_develop_blend_process (struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, void *i, void *o, const struct dt_iop_roi_t *roi_in, const struct dt_iop_roi_t *roi_out);

/** get blend version */
int dt_develop_blend_version (void);

/** update blendop params from older versions */
int dt_develop_blend_legacy_params (dt_iop_module_t *module, const void *const old_params, const int old_version, void *new_params, const int new_version, const int lenght);

/** gui related stuff */
void dt_iop_gui_init_blendif(GtkVBox *blendw, dt_iop_module_t *module);
void dt_iop_gui_init_blending(GtkWidget *iopw, dt_iop_module_t *module);
void dt_iop_gui_update_blendif(dt_iop_module_t *module);

/** routine to translate from mode id to sequence in option list */
int dt_iop_gui_blending_mode_seq(dt_iop_gui_blend_data_t *bd, int mode);


#ifdef HAVE_OPENCL
/** apply blend for opencl modules*/
int dt_develop_blend_process_cl (struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out, const struct dt_iop_roi_t *roi_in, const struct dt_iop_roi_t *roi_out);
#endif

#endif

// These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
