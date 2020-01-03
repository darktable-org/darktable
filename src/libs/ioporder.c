/*
    This file is part of darktable,
    copyright (c) 2019-2020 pascal obry.

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

#include "common/darktable.h"
#include "common/debug.h"
#include "control/signal.h"
#include "bauhaus/bauhaus.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include <gtk/gtk.h>
#include <stdlib.h>

DT_MODULE(1)

enum dt_ioporder_t
{
  DT_IOP_ORDER_UNSAFE      = 0,
  DT_IOP_ORDER_CUSTOM      = 1,
  DT_IOP_ORDER_LEGACY      = 2,
  DT_IOP_ORDER_RECOMMENDED = 3,
  DT_IOP_ORDER_LAST
} dt_ioporder_t;

typedef struct dt_lib_ioporder_t
{
  int current_mode;
  GtkWidget *widget;
} dt_lib_ioporder_t;

const char *name(dt_lib_module_t *self)
{
  return _("module order version");
}

const char **views(dt_lib_module_t *self)
{
  static const char *v[] = {"darkroom", NULL};
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_LEFT_CENTER;
}

int position()
{
  return 880;
}

void update(dt_lib_module_t *self)
{
  dt_lib_ioporder_t *d = (dt_lib_ioporder_t *)self->data;
  const int32_t imgid = darktable.develop->image_storage.id;

  int current_iop_order_version = dt_image_get_iop_order_version(imgid);

  int mode = DT_IOP_ORDER_UNSAFE;

  if(current_iop_order_version > DT_IOP_ORDER_PRESETS_START_ID)
  {
    const GList *entries =  dt_bauhaus_combobox_get_entries(d->widget);

    // four first entries are built-in
    for(int k=0; k<4; k++) entries = g_list_next(entries);
    int count = 4;

    while(entries)
    {
      const dt_bauhaus_combobox_entry_t *entry = (dt_bauhaus_combobox_entry_t *)entries->data;
      const int iop_order_version = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(d->widget), entry->label));

      if(current_iop_order_version == iop_order_version)
      {
        mode = count;
        break;
      }
      entries = g_list_next(entries);
      count++;
    }

    // preset not found, set to custom
    if(mode == DT_IOP_ORDER_UNSAFE)
      mode = DT_IOP_ORDER_CUSTOM;
  }
  else
  {
    if (current_iop_order_version == 2)
      mode = DT_IOP_ORDER_LEGACY;
    else if(current_iop_order_version == 5)
      mode = DT_IOP_ORDER_RECOMMENDED;
  }

  GList *iop_order_list = dt_ioppr_get_iop_order_list(&current_iop_order_version, TRUE);

  /*
    Check if user has changed the order (custom order).
    We do not check for iop-order but the actual order of modules
    compared to the canonical order of modules for the given iop-order version.
  */
  GList *modules = g_list_first(darktable.develop->iop);
  GList *iop = iop_order_list;

  while(modules)
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)modules->data;

    if(strcmp(mod->op, "mask_manager"))
    {
      // check module in iop_order_list
      if(iop && !strcmp(((dt_iop_order_entry_t *)iop->data)->operation, "mask_manager")) iop = g_list_next(iop);

      if(iop)
      {
        dt_iop_order_entry_t *entry = (dt_iop_order_entry_t *)iop->data;
        if(strcmp(entry->operation, mod->op))
        {
          mode = DT_IOP_ORDER_CUSTOM;
          break;
        }
        iop = g_list_next(iop);
      }
    }

    // skip all same modules (duplicate instances) if any
    while(modules && !strcmp(((dt_iop_module_t *)modules->data)->op, mod->op)) modules = g_list_next(modules);
  }

  g_list_free(iop_order_list);

  const int reset = darktable.gui->reset;
  darktable.gui->reset = 1;

  dt_bauhaus_combobox_set(d->widget, mode);
  d->current_mode = mode;

  darktable.gui->reset = reset;

  if(mode == DT_IOP_ORDER_UNSAFE)
    dt_control_log("this picture is using an unsafe iop-order, please select a proper one");
}

static void change_order_callback(GtkWidget *widget, dt_lib_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_lib_ioporder_t *d = (dt_lib_ioporder_t *)self->data;

  const int mode = dt_bauhaus_combobox_get(widget);
  const int32_t imgid = darktable.develop->image_storage.id;
  const int current_iop_order_version = dt_image_get_iop_order_version(imgid);
  int new_iop_order_version = DT_IOP_ORDER_UNSAFE;

  if(mode <= DT_IOP_ORDER_CUSTOM)
  {
    dt_bauhaus_combobox_set(widget, d->current_mode);
    return;
  }

  if(mode == DT_IOP_ORDER_LEGACY)
    new_iop_order_version = 2;
  else if(mode == DT_IOP_ORDER_RECOMMENDED)
    new_iop_order_version = 5;
  else // preset
  {
    const char *name = dt_bauhaus_combobox_get_text(widget);
    new_iop_order_version = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), name));
  }

  if(current_iop_order_version != new_iop_order_version
     || d->current_mode == DT_IOP_ORDER_CUSTOM)
  {
    dt_dev_write_history(darktable.develop);

    dt_ioppr_migrate_iop_order(darktable.develop, imgid, current_iop_order_version, new_iop_order_version);

    // invalidate buffers and force redraw of darkroom
    dt_dev_invalidate_all(darktable.develop);
  }

  d->current_mode = mode;
}

static void _image_loaded_callback(gpointer instace, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  update(self);
}

static void _fill_iop_order(dt_lib_module_t *self)
{
  dt_lib_ioporder_t *d = (dt_lib_ioporder_t *)self->data;

  const int reset = darktable.gui->reset;
  darktable.gui->reset = 1;

  dt_bauhaus_combobox_clear(d->widget);

  dt_bauhaus_combobox_add(d->widget, _("unsafe, select one below"));
  dt_bauhaus_combobox_add(d->widget, _("custom order"));
  dt_bauhaus_combobox_add(d->widget, _("legacy"));
  dt_bauhaus_combobox_add(d->widget, _("recommended"));

  // fill preset iop-order

  sqlite3_stmt *stmt;

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT name, op_params"
                              " FROM data.presets "
                              " WHERE operation='ioporder'", -1, &stmt, NULL);

  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const char *name = (char *)sqlite3_column_text(stmt, 0);
    const char *buf = (char *)sqlite3_column_blob(stmt, 1);
    const int iop_order_version = *(int32_t *)buf;
    dt_bauhaus_combobox_add(d->widget, name);
    g_object_set_data(G_OBJECT(d->widget), name, GUINT_TO_POINTER(iop_order_version));
  }

  sqlite3_finalize(stmt);

  darktable.gui->reset = reset;
}

void gui_init(dt_lib_module_t *self)
{
  dt_lib_ioporder_t *d = (dt_lib_ioporder_t *)malloc(sizeof(dt_lib_ioporder_t));

  self->data = (void *)d;
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  d->widget = dt_bauhaus_combobox_new(NULL);
  d->current_mode = DT_IOP_ORDER_UNSAFE;

  _fill_iop_order(self);

  gtk_widget_set_tooltip_text
    (d->widget,
     _("information:\n"
       "  unsafe\t\t: an unsafe/broken iop-order, select one below\n"
       "  custom\t\t: a customr iop-order\n"
       "or select an iop-order version either:\n"
       "  legacy\t\t\t: legacy iop order used prior to 3.0\n"
       "  recommended\t: newly iop-order introduced in v3.0"));
  g_signal_connect(G_OBJECT(d->widget), "value-changed",
                   G_CALLBACK(change_order_callback), (gpointer)self);

  gtk_box_pack_start(GTK_BOX(self->widget), d->widget, TRUE, TRUE, 0);

  dt_control_signal_connect(darktable.signals, DT_SIGNAL_DEVELOP_IMAGE_CHANGED,
                            G_CALLBACK(_image_loaded_callback), self);
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_DEVELOP_INITIALIZE,
                            G_CALLBACK(_image_loaded_callback), self);
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_DEVELOP_HISTORY_CHANGE,
                            G_CALLBACK(_image_loaded_callback), self);
}

void gui_cleanup(dt_lib_module_t *self)
{
  free(self->data);
  self->data = NULL;
}

void gui_reset (dt_lib_module_t *self)
{
  dt_lib_ioporder_t *d = (dt_lib_ioporder_t *)self->data;
  // the module reset is use to select the recommended iop-order
  dt_bauhaus_combobox_set(d->widget, DT_IOP_ORDER_RECOMMENDED);
}

void *_serialize_preset(const dt_iop_order_entry_t mod[], const int32_t version, int *size)
{
  // compute size of all modules
  *size = sizeof(int32_t);

  int k=0;
  while(mod[k].operation[0])
  {
    *size += strlen(mod[k].operation) + sizeof(int32_t) + sizeof(double);
    k++;
  }

  // allocate the parameter buffer
  char *params = (char *)malloc(*size);

  // set set preset iop-order version
  int pos = 0;

  memcpy(params+pos, &version, sizeof(int32_t));
  pos += sizeof(int32_t);

  k=0;
  while(mod[k].operation[0])
  {
    // write the iop-order
    memcpy(params+pos, &(mod[k].iop_order), sizeof(double));
    pos += sizeof(double);

    // write the len of the module name
    const int32_t len = strlen(mod[k].operation);
    memcpy(params+pos, &len, sizeof(int32_t));
    pos += sizeof(int32_t);

    // write the module name
    memcpy(params+pos, mod[k].operation, len);
    pos += len;

    k++;
  }

  return params;
}

void init_presets(dt_lib_module_t *self)
{
  int size = 0;
  char *params = NULL;

  // ------------------------------------------------- IOP Order V2

  const dt_iop_order_entry_t v2[] = {
    {  1.0, "rawprepare"},
    {  2.0, "invert"},
    {  3.0, "temperature"},
    {  4.0, "highlights"},
    {  5.0, "cacorrect"},
    {  6.0, "hotpixels"},
    {  7.0, "rawdenoise"},
    {  8.0, "demosaic"},
    {  9.0, "mask_manager"},
    { 10.0, "denoiseprofile"},
    { 11.0, "tonemap"},
    { 12.0, "exposure"},
    { 13.0, "spots"},
    { 14.0, "retouch"},
    { 15.0, "lens"},
    { 16.0, "ashift"},
    { 17.0, "liquify"},
    { 18.0, "rotatepixels"},
    { 19.0, "scalepixels"},
    { 20.0, "flip"},
    { 21.0, "clipping"},
    { 21.5, "toneequal"},
    { 22.0, "graduatednd"},
    { 23.0, "basecurve"},
    { 24.0, "bilateral"},
    { 25.0, "profile_gamma"},
    { 26.0, "hazeremoval"},
    { 27.0, "colorin"},
    { 27.5, "basicadj"},
    { 28.0, "colorreconstruct"},
    { 29.0, "colorchecker"},
    { 30.0, "defringe"},
    { 31.0, "equalizer"},
    { 32.0, "vibrance"},
    { 33.0, "colorbalance"},
    { 34.0, "colorize"},
    { 35.0, "colortransfer"},
    { 36.0, "colormapping"},
    { 37.0, "bloom"},
    { 38.0, "nlmeans"},
    { 39.0, "globaltonemap"},
    { 40.0, "shadhi"},
    { 41.0, "atrous"},
    { 42.0, "bilat"},
    { 43.0, "colorzones"},
    { 44.0, "lowlight"},
    { 45.0, "monochrome"},
    { 46.0, "filmic"},
    { 46.5, "filmicrgb"},
    { 47.0, "colisa"},
    { 48.0, "zonesystem"},
    { 49.0, "tonecurve"},
    { 50.0, "levels"},
    { 50.2, "rgblevels"},
    { 50.5, "rgbcurve"},
    { 51.0, "relight"},
    { 52.0, "colorcorrection"},
    { 53.0, "sharpen"},
    { 54.0, "lowpass"},
    { 55.0, "highpass"},
    { 56.0, "grain"},
    { 56.5, "lut3d"},
    { 57.0, "colorcontrast"},
    { 58.0, "colorout"},
    { 59.0, "channelmixer"},
    { 60.0, "soften"},
    { 61.0, "vignette"},
    { 62.0, "splittoning"},
    { 63.0, "velvia"},
    { 64.0, "clahe"},
    { 65.0, "finalscale"},
    { 66.0, "overexposed"},
    { 67.0, "rawoverexposed"},
    { 67.5, "dither"},
    { 68.0, "borders"},
    { 69.0, "watermark"},
    { 71.0, "gamma"},
    {  0.0, "" }
  };

  params = _serialize_preset(v2, 1002, &size);
  dt_lib_presets_add("legPS", self->plugin_name, self->version(), (const char *)params, size);
  free(params);

  // ------------------------------------------------- IOP Order V5

  const dt_iop_order_entry_t v5[] = {
    {  1.0, "rawprepare"},
    {  2.0, "invert"},
    {  3.0, "temperature"},
    {  4.0, "highlights"},
    {  5.0, "cacorrect"},
    {  6.0, "hotpixels"},
    {  7.0, "rawdenoise"},
    {  8.0, "demosaic"},
    {  9.0, "denoiseprofile"},
    { 10.0, "bilateral"},
    { 11.0, "rotatepixels"},
    { 12.0, "scalepixels"},
    { 13.0, "lens"},
    { 14.0, "hazeremoval"},
    { 15.0, "ashift"},
    { 16.0, "flip"},
    { 17.0, "clipping"},
    { 18.0, "liquify"},
    { 19.0, "spots"},
    { 20.0, "retouch"},
    { 21.0, "exposure"},
    { 22.0, "mask_manager"},
    { 23.0, "tonemap"},
    { 24.0, "toneequal"},
    { 25.0, "graduatednd"},
    { 26.0, "profile_gamma"},
    { 27.0, "equalizer"},
    { 28.0, "colorin"},

    { 29.0, "nlmeans"},         // signal processing (denoising)
                                //    -> needs a signal as scene-referred as possible (even if it works in Lab)
    { 30.0, "colorchecker"},    // calibration to "neutral" exchange colour space
                                //    -> improve colour calibration of colorin and reproductibility
                                //    of further edits (styles etc.)
    { 31.0, "defringe"},        // desaturate fringes in Lab, so needs properly calibrated colours
                                //    in order for chromaticity to be meaningful,
    { 32.0, "atrous"},          // frequential operation, needs a signal as scene-referred as possible to avoid halos
    { 33.0, "lowpass"},         // same
    { 34.0, "highpass"},        // same
    { 35.0, "sharpen"},         // same, worst than atrous in same use-case, less control overall
    { 36.0, "lut3d"},           // apply a creative style or film emulation, possibly non-linear,
                                //    so better move it after frequential ops that need L2 Hilbert spaces
                                //    of square summable functions
    { 37.0, "colortransfer"},   // probably better if source and destination colours are neutralized in the same
                                //    colour exchange space, hence after colorin and colorcheckr,
                                //    but apply after frequential ops in case it does non-linear witchcraft,
                                //    just to be safe
    { 59.0, "colormapping"},    // same
    { 38.0, "channelmixer"},    // does exactly the same thing as colorin, aka RGB to RGB matrix conversion,
                                //    but coefs are user-defined instead of calibrated and read from ICC profile.
                                //    Really versatile yet under-used module, doing linear ops,
                                //    very good in scene-referred workflow
    { 39.0, "basicadj"},        // module mixing view/model/control at once, usage should be discouraged
    { 40.0, "colorbalance"},    // scene-referred color manipulation
    { 41.0, "rgbcurve"},        // really versatile way to edit colour in scene-referred and display-referred workflow
    { 42.0, "rgblevels"},       // same
    { 43.0, "basecurve"},       // conversion from scene-referred to display referred, reverse-engineered
                                //    on camera JPEG default look
    { 44.0, "filmic"},          // same, but different (parametric) approach
    { 45.0, "filmicrgb"},       // same, upgraded
    { 46.0, "colisa"},          // edit contrast while damaging colour
    { 47.0, "tonecurve"},       // same
    { 48.0, "levels"},          // same
    { 49.0, "shadhi"},          // same
    { 50.0, "zonesystem"},      // same
    { 51.0, "globaltonemap"},   // same
    { 52.0, "relight"},         // flatten local contrast while pretending do add lightness
    { 53.0, "bilat"},           // improve clarity/local contrast after all the bad things we have done
                                //    to it with tonemapping
    { 54.0, "colorcorrection"}, // now that the colours have been damaged by contrast manipulations,
                                // try to recover them - global adjustment of white balance for shadows and highlights
    { 55.0, "colorcontrast"},   // adjust chrominance globally
    { 56.0, "velvia"},          // same
    { 57.0, "vibrance"},        // same, but more subtle
    { 58.0, "colorzones"},      // same, but locally
    { 60.0, "bloom"},           // creative module
    { 61.0, "colorize"},        // creative module
    { 62.0, "lowlight"},        // creative module
    { 63.0, "monochrome"},      // creative module
    { 64.0, "grain"},           // creative module
    { 65.0, "soften"},          // creative module
    { 66.0, "splittoning"},     // creative module
    { 67.0, "vignette"},        // creative module
    { 68.0, "colorreconstruct"},// try to salvage blown areas before ICC intents in LittleCMS2 do things with them.
    { 69.0, "colorout"},
    { 70.0, "clahe"},
    { 71.0, "finalscale"},
    { 72.0, "overexposed"},
    { 73.0, "rawoverexposed"},
    { 74.0, "dither"},
    { 75.0, "borders"},
    { 76.0, "watermark"},
    { 77.0, "gamma"},
    {  0.0, "" }
  };

  params = _serialize_preset(v5, 1005, &size);
  dt_lib_presets_add("recPS", self->plugin_name, self->version(), (const char *)params, size);
  free(params);
}

int set_params(dt_lib_module_t *self, const void *params, int size)
{
  if(!params) return 1;

  const int32_t imgid = darktable.develop->image_storage.id;

  // get the parameters buffer
  const char *buf = (char *)params;

  // load all params and create the iop-list

  const int current_iop_order = dt_image_get_iop_order_version(imgid);

  int32_t iop_order_version = 0;

  GList *iop_order_list = dt_ioppr_deserialize_iop_order_list(buf, size, &iop_order_version);

  if(iop_order_version < DT_IOP_ORDER_PRESETS_START_ID) return 1;

  // set pipe iop order

  dt_dev_write_history(darktable.develop);

  dt_ioppr_migrate_iop_order(darktable.develop, imgid, current_iop_order, iop_order_version);

  // invalidate buffers and force redraw of darkroom

  dt_dev_invalidate_all(darktable.develop);

  _fill_iop_order(self);
  update(self);

  if(iop_order_list) g_list_free(iop_order_list);

  return 0;
}

void *get_params(dt_lib_module_t *self, int *size)
{
  dt_lib_ioporder_t *d = (dt_lib_ioporder_t *)self->data;

  // do not allow recording unsafe or built-in iop-order
  // only custom order can be recorded.
  if(d->current_mode != DT_IOP_ORDER_CUSTOM) return NULL;

  GList *modules = g_list_first(darktable.develop->iop);

  // compute size of all modules
  *size = sizeof(int32_t);

  while(modules)
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)modules->data;
    *size += strlen(mod->op) + sizeof(int32_t) + sizeof(double);
    modules = g_list_next(modules);
  }

  // compute iop-order preset version

  int32_t version = DT_IOP_ORDER_PRESETS_START_ID + 1;

  // add the count of all current ioporder presets

  sqlite3_stmt *stmt;

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT COUNT(*)"
                              " FROM data.presets "
                              " WHERE operation='ioporder'", -1, &stmt, NULL);

  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    version += sqlite3_column_int(stmt, 0);
  }

  sqlite3_finalize(stmt);

  // allocate the parameter buffer
  char *params = (char *)malloc(*size);

  // store all modules in proper order
  modules = g_list_first(darktable.develop->iop);

  // set set preset iop-order version
  int pos = 0;

  memcpy(params+pos, &version, sizeof(int32_t));
  pos += sizeof(int32_t);

  while(modules)
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)modules->data;

    // write the iop-order
    memcpy(params+pos, &(mod->iop_order), sizeof(double));
    pos += sizeof(double);

    // write the len of the module name
    const int32_t len = strlen(mod->op);
    memcpy(params+pos, &len, sizeof(int32_t));
    pos += sizeof(int32_t);

    // write the module name
    memcpy(params+pos, mod->op, len);
    pos += len;

    modules = g_list_next(modules);
  }

  return params;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
