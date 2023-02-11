/*
    This file is part of darktable,
    Copyright (C) 2010-2021 darktable developers.

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
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "gui/color_picker_proxy.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"

#include <gtk/gtk.h>
#include <stdlib.h>

// This is an example implementation of an image operation module that does nothing useful.
// It demonstrates how the different functions work together. To build your own module,
// take all of the functions that are mandatory, stripping them of comments.
// Then add only the optional functions that are required to implement the functionality
// you need. Don't copy default implementations (hint: if you don't need to change or add
// anything, you probably don't need the copy). Make sure you choose descriptive names
// for your fields and variables. The ones given here are just examples; rename them.
//
// To have your module compile and appear in darkroom, add it to CMakeLists.txt, with
//  add_iop(useless "useless.c")
// and to iop_order.c, in the initialisation of legacy_order & v30_order with:
//  { {XX.0f }, "useless", 0},

// This is the version of the module's parameters,
// and includes version information about compile-time dt.
// The first released version should be 1.
DT_MODULE_INTROSPECTION(3, dt_iop_useless_params_t)

// TODO: some build system to support dt-less compilation and translation!

// Enums used in params_t can have $DESCRIPTIONs that will be used to
// automatically populate a combobox with dt_bauhaus_combobox_from_params.
// They are also used in the history changes tooltip.
// Combobox options will be presented in the same order as defined here.
// These numbers must not be changed when a new version is introduced.
typedef enum dt_iop_useless_type_t
{
  DT_USELESS_NONE = 0,     // $DESCRIPTION: "No"
  DT_USELESS_FIRST = 1,    // $DESCRIPTION: "First option"
  DT_USELESS_SECOND = 2,   // $DESCRIPTION: "Second one"
} dt_iop_useless_type_t;

typedef struct dt_iop_useless_params_t
{
  // The parameters defined here fully record the state of the module and are stored
  // (as a serialized binary blob) into the db.
  // Make sure everything is in here does not depend on temporary memory (pointers etc).
  // This struct defines the layout of self->params and self->default_params.
  // You should keep changes to this struct to a minimum.
  // If you have to change this struct, it will break
  // user data bases, and you have to increment the version
  // of DT_MODULE_INTROSPECTION(VERSION) above and provide a legacy_params upgrade path!
  //
  // Tags in the comments get picked up by the introspection framework and are
  // used in gui_init to set range and labels (for widgets and history)
  // and value checks before commit_params.
  // If no explicit init() is specified, the default implementation uses $DEFAULT tags
  // to initialise self->default_params, which is then used in gui_init to set widget defaults.
  //
  // These field names are just examples; chose meaningful ones! For performance reasons, align
  // to 4 byte boundaries (use gboolean, not bool).
  int checker_scale; // $MIN: 0 $MAX: 10 $DEFAULT: 1 $DESCRIPTION: "Size"
  float factor;      // $MIN: -5.0 $MAX: 5.0 $DEFAULT: 0
  gboolean check;    // $DESCRIPTION: "Checkbox option"
  dt_iop_useless_type_t method; // $DEFAULT: DT_USELESS_SECOND $DESCRIPTION: "Parameter choices"
} dt_iop_useless_params_t;

typedef struct dt_iop_useless_gui_data_t
{
  // Whatever you need to make your gui happy and provide access to widgets between gui_init, gui_update etc.
  // Stored in self->gui_data while in darkroom.
  // To permanently store per-user gui configuration settings, you could use dt_conf_set/_get.
  GtkWidget *scale, *factor, *check, *method, *extra; // this is needed by gui_update
} dt_iop_useless_gui_data_t;

typedef struct dt_iop_useless_global_data_t
{
  // This is optionally stored in self->global_data
  // and can be used to alloc globally needed stuff
  // which is needed in gui mode and during processing.

  // We don't need it for this example (as for most dt plugins).
} dt_iop_useless_global_data_t;

// this returns a translatable name
const char *name()
{
  // make sure you put all your translatable strings into _() !
  return _("Silly example");
}

// some additional flags (self explanatory i think):
int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING;
  // optionally add IOP_FLAGS_ALLOW_TILING and implement tiling_callback
}

// where does it appear in the gui?
int default_group()
{
  return IOP_GROUP_BASIC | IOP_GROUP_TECHNICAL;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

// Whenever new fields are added to (or removed from) dt_iop_..._params_t or when their meaning
// changes, a translation from the old to the new version must be added here.
// A verbatim copy of the old struct definition should be copied into the routine with a _v?_t ending.
// Since this will get very little future testing (because few developers still have very
// old versions lying around) existing code should be changed as little as possible, if at all.
//
// Upgrading from an older version than the previous one should always go through all in between versions
// (unless there was a bug) so that the end result will always be the same.
//
// Be careful with changes to structs that are included in _params_t
//
// Individually copy each existing field that is still in the new version. This is robust even if reordered.
// If only new fields were added at the end, one call can be used:
//   memcpy(n, o, sizeof *o);
//
// Hardcode the default values for new fields that were added, rather than referring to default_params;
// in future, the field may not exist anymore or the default may change. The best default for a new version
// to replicate a previous version might not be the optimal default for a fresh image.
//
// FIXME: the calling logic needs to be improved to call upgrades from consecutive version in sequence.
int legacy_params(dt_iop_module_t *self, const void *const old_params, const int old_version,
                  void *new_params, const int new_version)
{
  typedef dt_iop_useless_params_t dt_iop_useless_params_v3_t; // always create copy of current so code below doesn't need to be touched

  typedef struct dt_iop_useless_params_v2_t
  {
    int checker_scale;
    float factor;
  } dt_iop_useless_params_v2_t;

  if(old_version == 2 && new_version == 3)
  {
    dt_iop_useless_params_v2_t *o = (dt_iop_useless_params_v2_t *)old_params;
    dt_iop_useless_params_v3_t *n = (dt_iop_useless_params_v3_t *)new_params;

    memcpy(n, o, sizeof *o);
    n->check = FALSE;
    n->method = DT_USELESS_SECOND;
    return 0;
  }

  typedef struct dt_iop_useless_params_v1_t
  {
    int checker_scale;
  } dt_iop_useless_params_v1_t;

  if(old_version == 1 && new_version == 2)
  {
    dt_iop_useless_params_v1_t *o = (dt_iop_useless_params_v1_t *)old_params;
    dt_iop_useless_params_v2_t *n = (dt_iop_useless_params_v2_t *)new_params;

    n->checker_scale = o->checker_scale;
    n->factor = 0.0;
    return 0;
  }
  return 1;
}

static const int mask_id = 1; // key "0" is reserved for the pipe
static const char *mask_name = "useless checkerboard";

void commit_params(dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  memcpy(piece->data, p1, self->params_size);

  // there is no real need for this, but if the number of masks can be changed by the user this is the way to go.
  // otherwise we can have old stale masks floating around
  g_hash_table_remove_all(self->raster_mask.source.masks);
  g_hash_table_insert(self->raster_mask.source.masks, GINT_TO_POINTER(mask_id), g_strdup(mask_name));
}

#if 0
/** optional, always needed if tiling is permitted by setting IOP_FLAGS_ALLOW_TILING
    Also define this if the module uses more memory on the OpenCl device than the in& output buffers. 
*/
void tiling_callback(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                     const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out,
                     struct dt_develop_tiling_t *tiling)
{
  tiling->factor = 2.0f;    // input buffer + output buffer; increase if additional memory allocated
  tiling->factor_cl = 2.0f; // same, but for OpenCL code path running on GPU
  tiling->maxbuf = 1.0f;    // largest buffer needed regardless of how tiling splits up the processing
  tiling->maxbuf_cl = 1.0f; // same, but for running on GPU
  tiling->overhead = 0;     // number of bytes of fixed overhead
  tiling->overlap = 0;      // how many pixels do we need to access from the neighboring tile?
  tiling->xalign = 1;
  tiling->yalign = 1;
}
#endif

/** modify regions of interest (optional, per pixel ops don't need this) */
// void modify_roi_out(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, dt_iop_roi_t
// *roi_out, const dt_iop_roi_t *roi_in);
// void modify_roi_in(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, const dt_iop_roi_t
// *roi_out, dt_iop_roi_t *roi_in);

#if 0
/** modify pixel coordinates according to the pixel shifts the module applies (optional, per-pixel ops don't need) */
int distort_transform(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, float *points, size_t points_count)
{
  const dt_iop_useless_params_t *d = (dt_iop_useless_params_t *)piece->data;

  const float adjx = 0.0 * d->factor;
  const float adjy = 0.0;

  // nothing to be done if parameters are set to neutral values (no pixel shifts)
  if(adjx == 0.0 && adjy == 0.0) return 1;

  // apply the coordinate adjustment to each provided point
  for(size_t i = 0; i < points_count * 2; i += 2)
  {
    points[i] -= adjx;
    points[i + 1] -= adjy;
  }

  return 1;  // return 1 on success, 0 if one or more points could not be transformed
}
#endif

#if 0
/** undo pixel shifts the module applies (optional, per-pixel ops don't need this) */
int distort_backtransform(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, float *points, size_t points_count)
{
  const dt_iop_useless_params_t *d = (dt_iop_useless_params_t *)piece->data;

  const float adjx = 0.0 * d->factor;
  const float adjy = 0.0;

  // nothing to be done if parameters are set to neutral values (no pixel shifts)
  if(adjx == 0.0 && adjy == 0.0) return 1;

  // apply the inverse coordinate adjustment to each provided point
  for(size_t i = 0; i < points_count * 2; i += 2)
  {
    points[i] += adjx;
    points[i + 1] += adjy;
  }

  return 1;  // return 1 on success, 0 if one or more points could not be back-transformed
}
#endif

/** modify a mask according to the pixel shifts the module applies (optional, per-pixel ops don't need this) */
// void distort_mask(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, const float *const in,
// float *const out, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out);

/** process, all real work is done here.
    NOTE: process() must never use the Gtk+ API. All GUI modifications must be
          done in the Gtk+ thread. This is to be conducted in gui_update or
          gui_changed. If process detect a state and something it to be change on the UI
          a signal should be used (raise a signal here) and a corresponding callback
          must be connected to this signal.
*/
void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid, void *const ovoid,
             const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  // this is called for preview and full pipe separately, each with its own pixelpipe piece.
  // get our data struct:
  dt_iop_useless_params_t *d = (dt_iop_useless_params_t *)piece->data;
  // the total scale is composed of scale before input to the pipeline (iscale),
  // and the scale of the roi.
  const float scale = piece->iscale / roi_in->scale;
  // how many colors in our buffer?
  const size_t ch = piece->colors;

  // most modules only support a single type of input data, so we can check whether that format has been supplied
  // and simply pass along the data if not (setting a trouble flag to inform the user)
  dt_iop_useless_gui_data_t *g = (dt_iop_useless_gui_data_t *)self->gui_data;
  if(!dt_iop_have_required_input_format(4 /*we need full-color pixels*/, self, piece->colors,
                                         ivoid, ovoid, roi_in, roi_out))
    return;

  // we create a raster mask as an example
  float *mask = NULL;
  if(piece->pipe->store_all_raster_masks || dt_iop_is_raster_mask_used(piece->module, mask_id))
  {
    // Attempt to allocate all of the buffers we need.  For this example, we need one buffer that is equal in
    // dimensions to the output buffer, has one color channel, and has been zero'd.  (See common/imagebuf.h for
    // more details on all of the options.)
    if(!dt_iop_alloc_image_buffers(module, roi_in, roi_out,
                                    1/*ch per pixel*/ | DT_IMGSZ_OUTPUT | DT_IMGSZ_FULL | DT_IMGSZ_CLEARBUF, &mask,
                                    0 /* end of list of buffers to allocate */))
    {
      // Uh oh, we didn't have enough memory!  If multiple buffers were requested, any that had already
      // been allocated have been freed, and the module's trouble flag has been set.  We can simply pass
      // through the input image and return now, since there isn't anything else we need to clean up at
      // this point.
      dt_iop_copy_image_roi(ovoid, ivoid, ch, roi_in, roi_out, TRUE);
      return;
    }
  }
  else
    g_hash_table_remove(piece->raster_masks, GINT_TO_POINTER(mask_id));

// iterate over all output pixels (same coordinates as input)
#ifdef _OPENMP
// optional: parallelize it!
#pragma omp parallel for default(none) schedule(static) shared(d) dt_omp_firstprivate(scale, ivoid, ovoid, roi_in, roi_out, ch, mask)
#endif
  for(int j = 0; j < roi_out->height; j++)
  {
    float *in = ((float *)ivoid)
                + (size_t)ch * roi_in->width
                  * j; // make sure to address input, output and temp buffers with size_t as we want to also
    float *out = ((float *)ovoid) + (size_t)ch * roi_out->width * j; // correctly handle huge images
    float *out_mask = mask ? &(mask[(size_t)roi_out->width * j]) : NULL;
    for(int i = 0; i < roi_out->width; i++)
    {
      // calculate world space coordinates:
      int wi = (roi_in->x + i) * scale, wj = (roi_in->y + j) * scale;
      if((wi / d->checker_scale + wj / d->checker_scale) & 1)
      {
        for_each_channel(c, aligned(in,out))  // vectorize if possible
          out[c] = in[c] * (1.0 - d->factor); // does this for c=0..2 or c=0..3, whichever is faster
        if(out_mask) out_mask[i] = 1.0;
      }
      else
      {
        copy_pixel(out, in);
      }
      in += ch;
      out += ch;
    }
  }

  // now that the mask is generated we can publish it
  if(mask)
    g_hash_table_replace(piece->raster_masks, GINT_TO_POINTER(mask_id), mask);
}

/** Optional init and cleanup */
void init(dt_iop_module_t *module)
{
  // Allocates memory for a module instance and fills default_params.
  // If this callback is not provided, the standard implementation in
  // dt_iop_default_init is used, which looks at the $DEFAULT introspection tags
  // in the comments of the params_t struct definition.
  // An explicit implementation of init is only required if not all fields are
  // fully supported by dt_iop_default_init, for example arrays with non-identical values.
  // In that case, dt_iop_default_init can be called first followed by additional initialisation.
  // The values in params will not be used and default_params can be overwritten by
  // reload_params on a per-image basis.
  dt_iop_default_init(module);

  // Any non-default settings; for example disabling the on/off switch:
  module->hide_enable_button = 1;
  // To make this work correctly, you also need to hide the widgets, otherwise moving one
  // would enable the module anyway. The standard way is to set up a gtk_stack and show
  // the page that only has a label with an explanatory text when the module can't be used.
}

void init_global(dt_iop_module_so_t *module)
{
  module->data = malloc(sizeof(dt_iop_useless_global_data_t));
}

void cleanup(dt_iop_module_t *module)
{
  // Releases any memory allocated in init(module)
  // Implement this function explicitly if the module allocates additional memory besides (default_)params.
  // this is rare.
  free(module->params);
  module->params = NULL;
  free(module->default_params);
  module->default_params = NULL;
}

void cleanup_global(dt_iop_module_so_t *module)
{
  free(module->data);
  module->data = NULL;
}

/** Put your local callbacks here, be sure to make them static so they won't be visible outside this file! */
static void extra_callback(GtkWidget *w, dt_iop_module_t *self)
{
  // this is important to avoid cycles!
  if(darktable.gui->reset) return;

  dt_iop_useless_params_t *p = (dt_iop_useless_params_t *)self->params;
  dt_iop_useless_gui_data_t *g = (dt_iop_useless_gui_data_t *)self->gui_data;

  float extra = dt_bauhaus_slider_get(w);

  // Setting a widget value will trigger a callback that will update params.
  // If this is not desirable (because it might result in a cycle) then use
  // ++darktable.gui->reset;
  dt_bauhaus_slider_set(g->factor, p->factor + extra);
  // and reverse with --darktable.gui->reset;

  // If any params updated directly, not via a callback, then
  // let core know of the changes
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

/** optional gui callbacks. */
void gui_changed(dt_iop_module_t *self, GtkWidget *w, void *previous)
{
  // If defined, this gets called when any of the introspection based widgets
  // (created with dt_bauhaus_..._from_params) are changed.
  // The updated value from the widget is already set in params.
  // any additional side-effects can be achieved here.
  dt_iop_useless_params_t *p = (dt_iop_useless_params_t *)self->params;
  dt_iop_useless_gui_data_t *g = (dt_iop_useless_gui_data_t *)self->gui_data;

  // Test which widget was changed.
  // If allowing w == NULL, this can be called from gui_update, so that
  // gui configuration adjustments only need to be dealt with once, here.
  if(!w || w == g->method)
  {
    gtk_widget_set_visible(g->check, p->method == DT_USELESS_SECOND);
  }

  // Widget configurations that don't depend any any current params values should
  // go in reload_defaults (if they depend on the image) or gui_init.
}

void color_picker_apply(dt_iop_module_t *self, GtkWidget *picker, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_useless_params_t *p = (dt_iop_useless_params_t *)self->params;
  dt_iop_useless_gui_data_t *g = (dt_iop_useless_gui_data_t *)self->gui_data;

  // This automatically gets called when any of the color pickers set up with
  // dt_color_picker_new in gui_init is used. If there is more than one,
  // check which one is active first.
  if(picker == g->factor)
  {
    p->factor = self->picked_color[1];
  }

  dt_dev_add_history_item(darktable.develop, self, TRUE);
  dt_control_queue_redraw_widget(self->widget);
}

/** gui setup and update, these are needed. */
void gui_update(dt_iop_module_t *self)
{
  // This gets called when switching to darkroom, with each image change or when
  // a different history item is selected.
  // Here, all the widgets need to be set to the current values in param.
  //
  // Note, this moves data from params -> gui. All fields at same time.
  // The opposite direction, gui -> params happens one field at a time, for example
  // when the user manipulates a slider. It is handled by gui_changed (and the
  // automatic callback) for introspection based widgets or by the explicit callback
  // set up manually (see example of extra_callback above).
  dt_iop_useless_gui_data_t *g = (dt_iop_useless_gui_data_t *)self->gui_data;
  dt_iop_useless_params_t *p = (dt_iop_useless_params_t *)self->params;

  dt_bauhaus_slider_set(g->scale, p->checker_scale);

  // For introspection based widgets (dt_bauhaus_slider_from_params) do not use
  // any transformations here (for example *100 for percentages) because that will
  // break enforcement of $MIN/$MAX.
  // Use dt_bauhaus_slider_set_factor/offset in gui_init instead.
  dt_bauhaus_slider_set(g->factor, p->factor);

  // dt_bauhaus_toggle_from_params creates a standard gtk_toggle_button.
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->check), p->check);

  // Use set_from_value to correctly handle out of order values.
  dt_bauhaus_combobox_set_from_value(g->method, p->method);

  // Any configuration changes to the gui that depend on field values should be done here,
  // or can be done in gui_changed which can then be called from here with widget == NULL.
  gui_changed(self, NULL, NULL);
}

/** optional: if this exists, it will be called to init new defaults if a new image is
 * loaded from film strip mode. */
void reload_defaults(dt_iop_module_t *module)
{
  // This only has to be provided if module settings or default_params need to depend on
  // image type (raw?) or exif data.
  // Make sure to always reset to the default for non-special cases, otherwise the override
  // will stick when switching to another image.
  dt_iop_useless_params_t *d = (dt_iop_useless_params_t *)module->default_params;

  // As an example, switch off for non-raw images. The enable button was already hidden in init().
  if(!dt_image_is_raw(&module->dev->image_storage))
  {
    module->default_enabled = 0;
  }
  else
  {
    module->default_enabled = 1;
    d->checker_scale = 3; // something dependent on exif, for example.
  }

  // If we are in darkroom, gui_init will already have been called and has initialised
  // module->gui_data and widgets.
  // So if default values have been changed, it may then be necessary to also change the
  // default values in widgets. Resetting the individual widgets will then have the same
  // effect as resetting the whole module at once.
  dt_iop_useless_gui_data_t *g = (dt_iop_useless_gui_data_t *)module->gui_data;
  if(g)
  {
    dt_bauhaus_slider_set_default(g->scale, d->checker_scale);
  }
}

void gui_init(dt_iop_module_t *self)
{
  // Allocates memory for the module's user interface in the darkroom and
  // sets up the widgets in it.
  //
  // self->widget needs to be set to the top level widget.
  // This can be a (vertical) box, a grid or even a notebook. Modules that are
  // disabled for certain types of images (for example non-raw) may use a stack
  // where one of the pages contains just a label explaining why it is disabled.
  //
  // Widgets that are directly linked to a field in params_t may be set up using the
  // dt_bauhaus_..._from_params family. They take a string with the field
  // name in the params_t struct definition. The $MIN, $MAX and $DEFAULT tags will be
  // used to set up the widget (slider) ranges and default values and the $DESCRIPTION
  // is used as the widget label.
  //
  // The _from_params calls also set up an automatic callback that updates the field in params
  // whenever the widget is changed. In addition, gui_changed is called, if it exists,
  // so that any other required changes, to dependent fields or to gui widgets, can be made.
  //
  // Whenever self->params changes (switching images or history) the widget values have to
  // be updated in gui_update.
  //
  // Do not set the value of widgets or configure them depending on field values here;
  // this should be done in gui_update (or gui_changed or individual widget callbacks)
  //
  // If any default values for(slider) widgets or options (in comboboxes) depend on the
  // type of image, then the widgets have to be updated in reload_params.
  dt_iop_useless_gui_data_t *g = IOP_GUI_ALLOC(useless);

  // If the first widget is created using a _from_params call, self->widget does not have to
  // be explicitly initialised, as a new vertical box will be created automatically.
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  // Linking a slider to an integer will make it take only whole numbers (step=1).
  // The new slider is added to self->widget
  g->scale = dt_bauhaus_slider_from_params(self, "checker_scale");

  // If the field name should be used as label too, it does not need a $DESCRIPTION;
  // mark it for translation here using N_()
  //
  // A colorpicker can be attached to a slider, as here, or put standalone in a box.
  // When a color is picked, color_picker_apply is called with either the slider or the
  // button that triggered it.
  g->factor = dt_color_picker_new(self, DT_COLOR_PICKER_AREA,
              dt_bauhaus_slider_from_params(self, N_("Factor")));
  // The initial slider range can be reduced from the introspection $MIN - $MAX
  dt_bauhaus_slider_set_soft_range(g->factor, 0.5f, 1.5f);
  // The default step is range/100, but can be changed here
  dt_bauhaus_slider_set_step(g->factor, .1);
  dt_bauhaus_slider_set_digits(g->factor, 2);
  // Additional parameters determine how the value will be shown.
  dt_bauhaus_slider_set_format(g->factor, "%");
  // For a percentage, use factor 100.
  dt_bauhaus_slider_set_factor(g->factor, -100.0f);
  dt_bauhaus_slider_set_offset(g->factor, 100.0f);
  // Tooltips explain the otherwise compact interface
  gtk_widget_set_tooltip_text(g->factor, _("Adjust factor"));

  // A combobox linked to struct field will be filled with the values and $DESCRIPTIONs
  // in the struct definition, in the same order. The automatic callback will put the
  // enum value, not the position within the combobox list, in the field.
  g->method = dt_bauhaus_combobox_from_params(self, "method");

  g->check = dt_bauhaus_toggle_from_params(self, "check");

  // Any widgets that are _not_ directly linked to a field need to have a custom callback
  // function set up to respond to the "value-changed" signal.
  g->extra = dt_bauhaus_slider_new_with_range(self, -0.5, 0.5, 0, 0, 2);
  dt_bauhaus_widget_set_label(g->extra, NULL, N_("Extra"));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->extra), TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(g->extra), "value-changed", G_CALLBACK(extra_callback), self);
}

void gui_cleanup(dt_iop_module_t *self)
{
  // This only needs to be provided if gui_init allocates any memory or resources besides
  // self->widget and gui_data_t. The default function (if an explicit one isn't provided here)
  // takes care of gui_data_t (and gtk destroys the widget anyway). If you override the default,
  // you have to do whatever you have to do, and also call IOP_GUI_FREE to clean up gui_data_t.

  IOP_GUI_FREE;
}

/** additional, optional callbacks to capture darkroom center events. */
// void gui_post_expose(dt_iop_module_t *self, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx,
// int32_t pointery);
// int mouse_moved(dt_iop_module_t *self, double x, double y, double pressure, int which);
// int button_pressed(dt_iop_module_t *self, double x, double y, double pressure, int which, int type,
// uint32_t state);
// int button_released(struct dt_iop_module_t *self, double x, double y, int which, uint32_t state);
// int scrolled(dt_iop_module_t *self, double x, double y, int up, uint32_t state);

// optional: if mouse events are handled by the iop, we can add text to the help screen by declaring
// the mouse actions and their descriptions
#if 0
GSList *mouse_actions(dt_iop_module_t *self)
{
  GSList *lm = NULL;
  // add the first action
  lm = dt_mouse_action_create_format(lm, DT_MOUSE_ACTION_SCROLL, GDK_SHIFT_MASK,
                                     _("[%s] Some action"), self->name());
  // append a second action to the list we will return
  lm = dt_mouse_action_create_format(lm, DT_MOUSE_ACTION_LEFT_DRAG, GDK_CONTROL_MASK | GDK_SHIFT_MASK,
                                     _("[%s] Other action"), self->name());
  return lm;
}
#endif

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
