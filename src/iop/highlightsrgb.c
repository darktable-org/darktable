/*
    This file is part of darktable,
    Copyright (C) 2022 darktable developers.

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

#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "bauhaus/bauhaus.h"
#include "common/opencl.h"
#include "common/imagebuf.h"
#include "common/box_filters.h"
#include "common/gaussian.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/masks.h"
#include "develop/imageop_math.h"
#include "develop/imageop_gui.h"
#include "develop/tiling.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"

#include <gtk/gtk.h>
#include <inttypes.h>

DT_MODULE_INTROSPECTION(1, dt_iop_highlightsrgb_params_t)

typedef enum dt_iop_highlightsrgb_mode_t
{
  DT_IOP_HIGHLIGHTSRGB_CLIP = 0,      // $DESCRIPTION: "clipped"
  DT_IOP_HIGHLIGHTSRGB_LAPLACIAN = 1, // $DESCRIPTION: "guide laplacians"
  DT_IOP_HIGHLIGHTSRGB_RECOVERY = 2,  // $DESCRIPTION: "highlights recovery"
} dt_iop_highlightsrgb_mode_t;

typedef struct dt_iop_highlightsrgb_params_t
{
  dt_iop_highlightsrgb_mode_t mode; // $DEFAULT: DT_IOP_HIGHLIGHTSRGB_CLIP $DESCRIPTION: "method"
  float clip;                       // $MIN: 0.0 $MAX: 2.0  $DEFAULT: 1.0  $DESCRIPTION: "clipping threshold"
  float recovery;                   // $MIN: 0.0 $MAX: 1.0  $DEFAULT: 0.4  $DESCRIPTION: "effect strength"
  float combine;                    // $MIN: 0.0 $MAX: 10.0 $DEFAULT: 2.0  $DESCRIPTION: "combine segments"
  float feathering_details;         // $MIN: 2.0 $MAX: 8.0  $DEFAULT: 6.0  $DESCRIPTION: "details feathering"
  float feathering_colors;          // $MIN: -2.0 $MAX: 2.0 $DEFAULT: 0.0  $DESCRIPTION: "colors feathering"
  float noise_level;                // $MIN: 0. $MAX: 1.0   $DEFAULT: 0.05 $DESCRIPTION: "noise level"
  float freserved1;
  float freserved2;
  float freserved3;
  int ireserved1;
  int ireserved2;
  int ireserved3;
} dt_iop_highlightsrgb_params_t;


typedef struct dt_iop_highlightsrgb_gui_data_t
{
  GtkWidget *mode;
  GtkWidget *clip;
  GtkWidget *recovery, *combine;
  GtkWidget *feathering_details, *feathering_colors, *noise_level;
} dt_iop_highlightsrgb_gui_data_t;

typedef dt_iop_highlightsrgb_params_t dt_iop_highlightsrgb_data_t;

typedef struct dt_iop_highlightsrgb_global_data_t
{

} dt_iop_highlightsrgb_global_data_t;

const char *name()
{
  return _("highlights rgb");
}

const char *description(struct dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("avoid magenta highlights and try to recover highlights colors"),
                                      _("corrective"),
                                      _("linear, scene-referred"),
                                      _("reconstruction"),
                                      _("linear, scene-referred"));
}

int flags()
{
  return IOP_FLAGS_ONE_INSTANCE;
}

int default_group()
{
  return IOP_GROUP_BASIC | IOP_GROUP_TECHNICAL;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return iop_cs_rgb;
}


void commit_params(dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_highlightsrgb_params_t *p = (dt_iop_highlightsrgb_params_t *)p1;
  dt_iop_highlightsrgb_data_t *d = (dt_iop_highlightsrgb_data_t *)piece->data;
  memcpy(d, p, sizeof(*p));

  // none of the available modes supports cl yet
  piece->process_cl_ready = 0;

  // so far also no tiling
  piece->process_tiling_ready = 0;
}

#include "iop/hl_rgb/recovery.c"
#include "iop/hl_rgb/laplacian.c"

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid, void *const ovoid,
             const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_highlightsrgb_params_t *d   = (dt_iop_highlightsrgb_params_t *)piece->data;
//  dt_iop_highlightsrgb_gui_data_t *g = (dt_iop_highlightsrgb_gui_data_t *)self->gui_data;
//  const size_t ch = piece->colors;
  if(!dt_iop_have_required_input_format(4, self, piece->colors, ivoid, ovoid, roi_in, roi_out))
    fprintf(stderr, "[highlightsrgb] expects rgb input data\n");

  switch(d->mode)
  {
    case DT_IOP_HIGHLIGHTSRGB_RECOVERY:
      process_recovery(self, piece, ivoid, ovoid, roi_in, roi_out);   
      break;

    case DT_IOP_HIGHLIGHTSRGB_LAPLACIAN:
      process_laplacian(self, piece, ivoid, ovoid, roi_in, roi_out);   
      break;

    default:
      dt_iop_copy_image_roi(ovoid, ivoid, 4, roi_in, roi_out, TRUE);
      break;
  }
}

void gui_changed(dt_iop_module_t *self, GtkWidget *w, void *previous)
{
  dt_iop_highlightsrgb_params_t   *p = (dt_iop_highlightsrgb_params_t *)self->params;
  dt_iop_highlightsrgb_gui_data_t *g = (dt_iop_highlightsrgb_gui_data_t *)self->gui_data;

  dt_bauhaus_combobox_set_from_value(g->mode, p->mode);

  const gboolean recover =   (p->mode == DT_IOP_HIGHLIGHTSRGB_RECOVERY);
  const gboolean laplacian = (p->mode == DT_IOP_HIGHLIGHTSRGB_LAPLACIAN);

  gtk_widget_set_visible(g->recovery, recover);
  gtk_widget_set_visible(g->combine, recover);
  gtk_widget_set_visible(g->feathering_details, laplacian);
  gtk_widget_set_visible(g->feathering_colors, laplacian);
  gtk_widget_set_visible(g->noise_level, laplacian);

  dt_bauhaus_slider_set(g->clip, p->clip);
  dt_bauhaus_slider_set(g->recovery, p->recovery);
  dt_bauhaus_slider_set(g->combine, p->combine);
  dt_bauhaus_slider_set(g->feathering_details, p->feathering_details);
  dt_bauhaus_slider_set(g->feathering_colors, p->feathering_colors);
  dt_bauhaus_slider_set(g->noise_level, p->noise_level);
}

void gui_update(struct dt_iop_module_t *self)
{
  gui_changed(self, NULL, NULL);
}


void gui_init(dt_iop_module_t *self)
{
  dt_iop_highlightsrgb_gui_data_t *g = IOP_GUI_ALLOC(highlightsrgb);

  g->mode = dt_bauhaus_combobox_from_params(self, "mode");
  gtk_widget_set_tooltip_text(g->mode, _("highlight reconstruction method"));

  g->clip = dt_bauhaus_slider_from_params(self, "clip");
  dt_bauhaus_slider_set_digits(g->clip, 3);
  gtk_widget_set_tooltip_text(g->clip, _("manually adjust the clipping threshold against magenta highlights."
                                         " Necessary for images with incorrect white point settings."));

  g->recovery = dt_bauhaus_slider_from_params(self, "recovery");
  gtk_widget_set_tooltip_text(g->recovery, _("reduces an existing color cast in regions where color planes are clipped")); 
  dt_bauhaus_slider_set_factor(g->recovery, 100.0f);
  dt_bauhaus_slider_set_format(g->recovery, "%.0f%%");

  g->combine = dt_bauhaus_slider_from_params(self, "combine");
  dt_bauhaus_slider_set_digits(g->combine, 0);
  gtk_widget_set_tooltip_text(g->combine, _("combine close segments")); 


  g->feathering_details = dt_bauhaus_slider_from_params(self, "feathering_details");
  gtk_widget_set_tooltip_text(g->feathering_details, _("increase to preserve the sharpness of details in clipped areas\n"
                                                       "decrease to smoothen edge artifacts in clipped areas"));

  g->feathering_colors = dt_bauhaus_slider_from_params(self, "feathering_colors");
  gtk_widget_set_tooltip_text(g->feathering_details, _("increase if unwanted colors start to bleed on clipped areas\n"
                                                       "decrease to propagate colors further in clipped areas"));

  g->noise_level = dt_bauhaus_slider_from_params(self, "noise_level");
  gtk_widget_set_tooltip_text(g->noise_level, _("increase if unwanted colors start to bleed on clipped areas\n"
                                                "decrease to propagate colors further in clipped areas"));
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
