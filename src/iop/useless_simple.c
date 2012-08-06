/*
    This file is part of darktable,
    copyright (c) 2009--2012 johannes hanika.
    copyright (c) 2012 tobias ellinghaus.

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
#include "develop/imageop.h"
#include "bauhaus/bauhaus.h"
#include "gui/gtk.h"
#include "gui/simple_gui.h"

#include <gtk/gtk.h>
#include <stdlib.h>

// this is the version of the modules parameters,
// and includes version information about compile-time dt
DT_MODULE(1)

// TODO: some build system to support dt-less compilation and translation!

typedef struct dt_iop_useless_global_data_t
{
  // this is optionally stored in self->global_data
  // and can be used to alloc globally needed stuff
  // which is needed in gui mode and during processing.

  // we don't need it for this example (as for most dt plugins)
}
dt_iop_useless_global_data_t;

// this returns a translatable name
const char *name()
{
  // make sure you put all your translatable strings into _() !
  return _("simple gui api test");
}

// some additional flags (self explanatory i think):
int
flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES;
}

// where does it appear in the gui?
int
groups()
{
  return IOP_GROUP_BASIC;
}

// implement this, if you have esoteric output bytes per pixel. default is 4*float
/*
int
output_bpp(dt_iop_module_t *module, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  if(pipe->type != DT_DEV_PIXELPIPE_PREVIEW && module->dev->image->filters) return sizeof(float);
  else return 4*sizeof(float);
}
*/


/** modify regions of interest (optional, per pixel ops don't need this) */
// void modify_roi_out(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, dt_iop_roi_t *roi_out, const dt_iop_roi_t *roi_in);
// void modify_roi_in(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, const dt_iop_roi_t *roi_out, dt_iop_roi_t *roi_in);

/** process, all real work is done here. */
void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *i, void *o, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  // this is called for preview and full pipe separately, each with its own pixelpipe piece.
  // get our data struct:
  int *d = (int*)piece->data; // the default param format is an array of int or float, depending on the type of widget
  float *foo = (float*)(&d[0]);
  int checker_scale = *foo;
  int color = d[1];
  // the total scale is composed of scale before input to the pipeline (iscale),
  // and the scale of the roi.
  const float scale = piece->iscale/roi_in->scale;
  // how many colors in our buffer?
  const int ch = piece->colors;
  // iterate over all output pixels (same coordinates as input)
#ifdef _OPENMP
  // optional: parallelize it!
  #pragma omp parallel for default(none) schedule(static) shared(i,o,roi_in,roi_out,d,checker_scale,color)
#endif
  for(int j=0; j<roi_out->height; j++)
  {
    float *in  = ((float *)i) + ch*roi_in->width *j;
    float *out = ((float *)o) + ch*roi_out->width*j;
    for(int i=0; i<roi_out->width; i++)
    {
      // calculate world space coordinates:
      int wi = (roi_in->x + i) * scale, wj = (roi_in->y + j) * scale;
      if((wi/checker_scale+wj/checker_scale)&1) for(int c=0; c<3; c++) out[c] = (color == c)?1:0;
      else                                            for(int c=0; c<3; c++) out[c] = in[c];
      in += ch;
      out += ch;
    }
  }
}

/** optional: if this exists, it will be called to init new defaults if a new image is loaded from film strip mode. */
// void reload_defaults(dt_iop_module_t *module)
// {
  // change default_enabled depending on type of image, or set new default_params even.

  // if this callback exists, it has to write default_params and default_enabled.
// }

/** init, cleanup, commit to pipeline. when using the simple api you don't need to care about params, ... */
void init(dt_iop_module_t *module)
{
  // order has to be changed by editing the dependencies in tools/iop_dependencies.py
  module->priority = 901; // module order created by iop_dependencies.py, do not edit!
}

/** some sample callbacks. buttons don't have default callbacks, but others can just be overwritten. */
static void
button_callback(GtkWidget *w, gpointer data)
{
  printf("button was clicked!\n");
}

static void
togglebutton_callback(GtkWidget *w, gpointer i)
{
  printf("toggle button is now %s active (parameter was %d).\n", gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w))?"":"no longer", GPOINTER_TO_INT(i));
}

/** gui callbacks, these are needed. */
dt_gui_simple_t* gui_init_simple(dt_iop_module_t *self)
{
  static char *combobox_entries[] = {"red", "green", "blue", NULL}; // has to be NULL terminated!
  static dt_gui_simple_t gui = {
    0, // not used currently
    {
      /** a slider */
      {.slider = {
        DT_SIMPLE_GUI_SLIDER,
        "scale",
        "the scale of the checker board",
        NULL,
        1, 100, 1, 50,
        0,
        NULL,
        NULL
      }},

      /** a combobox */
      {.combobox = {
        DT_SIMPLE_GUI_COMBOBOX,
        "color",
        "select color of the checker board",
        combobox_entries,
        0,
        NULL,
        NULL
      }},

      /** a button */
      {.button = {
        DT_SIMPLE_GUI_BUTTON,
        "do nothing",
        "this button does nothing, it's just looking nice",
        NULL,
        0,
        0xdeadbeef, // default is not used for regular buttons
        &button_callback,
        NULL // passing NULL will result in getting self as the parameter
      }},

      /** a toggle button */
      {.button = {
        DT_SIMPLE_GUI_TOGGLE_BUTTON,
        NULL,
        "another button which does nothing",
        dtgtk_cairo_paint_triangle,
        CPF_DIRECTION_RIGHT|CPF_STYLE_FLAT|CPF_DO_NOT_USE_BORDER,
        0, // start in the disabled state
        &togglebutton_callback,
        GINT_TO_POINTER(23)
      }},

      /** the last element has to be of type DT_SIMPLE_GUI_NONE */
      {.type = DT_SIMPLE_GUI_NONE}
    }
  };

  return &gui;
}

/** not needed when using the simple gui api. */
// void gui_init(dt_iop_module_t* self);
// void gui_cleanup(dt_iop_module_t *self);
// void gui_update(dt_iop_module_t *self);

/** additional, optional callbacks to capture darkroom center events. */
// void gui_post_expose(dt_iop_module_t *self, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx, int32_t pointery);
// int mouse_moved(dt_iop_module_t *self, double x, double y, int which);
// int button_pressed(dt_iop_module_t *self, double x, double y, int which, int type, uint32_t state);
// int button_released(struct dt_iop_module_t *self, double x, double y, int which, uint32_t state);
// int scrolled(dt_iop_module_t *self, double x, double y, int up, uint32_t state);

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
