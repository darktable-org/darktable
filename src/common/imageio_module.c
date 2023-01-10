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

#include "common/imageio_module.h"
#include "common/darktable.h"
#include "common/file_location.h"
#include "common/imageio.h"
#include "control/conf.h"
#include "control/control.h"
#include "control/signal.h"
#include "gui/accelerators.h"
#include <stdlib.h>

static gint dt_imageio_sort_modules_storage(gconstpointer a, gconstpointer b)
{
  const dt_imageio_module_storage_t *am = (const dt_imageio_module_storage_t *)a;
  const dt_imageio_module_storage_t *bm = (const dt_imageio_module_storage_t *)b;
  return strcmp(am->name(am), bm->name(bm));
}

static gint dt_imageio_sort_modules_format(gconstpointer a, gconstpointer b)
{
  const dt_imageio_module_format_t *am = (const dt_imageio_module_format_t *)a;
  const dt_imageio_module_format_t *bm = (const dt_imageio_module_format_t *)b;
  return strcmp(am->name(), bm->name());
}

/** Default implementation of dimension module function, used if format module does not implement dimension()
 */
static int _default_format_dimension(dt_imageio_module_format_t *module, dt_imageio_module_data_t *data,
                                     uint32_t *width, uint32_t *height)
{
  // assume no limits
  *width = 0;
  *height = 0;
  return 0;
}
/** Default implementation of flags, used if format module does not implement flags() */
static int _default_format_flags(dt_imageio_module_data_t *data)
{
  return 0;
}
/** Default implementation of levels, used if format module does not implement levels() */
static int _default_format_levels(dt_imageio_module_data_t *data)
{
  return IMAGEIO_RGB | IMAGEIO_INT8;
}
/** Default implementation of gui_init function (a NOP), used when no gui is existing. this is easier than
 * checking for that case all over the place */
static void _default_format_gui_init(struct dt_imageio_module_format_t *self)
{
}

static int dt_imageio_load_module_format(dt_imageio_module_format_t *module, const char *libname,
                                         const char *module_name)
{
  g_strlcpy(module->plugin_name, module_name, sizeof(module->plugin_name));

#define INCLUDE_API_FROM_MODULE_LOAD "imageio_load_module_format"
#include "imageio/format/imageio_format_api.h"

  if(!module->dimension) module->dimension = _default_format_dimension;
  if(!module->flags) module->flags = _default_format_flags;
  if(!module->levels) module->levels = _default_format_levels;

  module->widget = NULL;
  module->parameter_lua_type = LUAA_INVALID_TYPE;
  // Can by set by the module function to false if something went wrong.
  module->ready = TRUE;

#ifdef USE_LUA
  {
    char pseudo_type_name[1024];
    snprintf(pseudo_type_name, sizeof(pseudo_type_name), "dt_imageio_module_format_data_%s",
             module->plugin_name);
    luaA_Type my_type
        = luaA_type_add(darktable.lua_state.state, pseudo_type_name, module->params_size(module));
    module->parameter_lua_type = dt_lua_init_type_type(darktable.lua_state.state, my_type);
    luaA_struct_type(darktable.lua_state.state, my_type);
    dt_lua_register_format_type(darktable.lua_state.state, module, my_type);
#endif
    module->init(module);
    if(!module->ready)
    {
      goto api_h_error;
    }

#ifdef USE_LUA
    lua_pushcfunction(darktable.lua_state.state, dt_lua_type_member_luaautoc);
    dt_lua_type_register_struct_type(darktable.lua_state.state, my_type);
  }
#endif

  if(darktable.gui)
  {
    if(!module->gui_init || !module->module) goto api_h_error;

    module->actions = (dt_action_t){ DT_ACTION_TYPE_SECTION,
      module->plugin_name,
      module->name(),
      NULL,
      NULL,
      NULL };

    dt_action_insert_sorted(&darktable.control->actions_format, &module->actions);
  }
  else
  {
    module->gui_init = _default_format_gui_init;
  }

  return 0;
}


static int dt_imageio_load_modules_format(dt_imageio_t *iio)
{
  iio->plugins_format = NULL;
  GList *res = NULL;

  char plugindir[PATH_MAX] = { 0 }, plugin_name[256];
  const gchar *d_name;

  dt_loc_get_plugindir(plugindir, sizeof(plugindir));
  g_strlcat(plugindir, "/plugins/imageio/format", sizeof(plugindir));

  GDir *dir = g_dir_open(plugindir, 0, NULL);

  if(!dir) return 1;

  const int name_offset = strlen(SHARED_MODULE_PREFIX);
  const int name_end = strlen(SHARED_MODULE_PREFIX) + strlen(SHARED_MODULE_SUFFIX);

  while((d_name = g_dir_read_name(dir)))
  {
    // get lib*.so
    if(!g_str_has_prefix(d_name, SHARED_MODULE_PREFIX)) continue;
    if(!g_str_has_suffix(d_name, SHARED_MODULE_SUFFIX)) continue;
    g_strlcpy(plugin_name, d_name + name_offset, strlen(d_name) - name_end + 1);
    dt_imageio_module_format_t *module =
      (dt_imageio_module_format_t *)calloc(1, sizeof(dt_imageio_module_format_t));
    gchar *libname = g_module_build_path(plugindir, (const gchar *)plugin_name);
    if(dt_imageio_load_module_format(module, libname, plugin_name))
    {
      g_free(libname);
      free(module);
      continue;
    }
    module->gui_data = NULL;
    if(darktable.gui) ++darktable.gui->reset;
    module->gui_init(module);
    if(darktable.gui) --darktable.gui->reset;
    if(module->widget) g_object_ref(module->widget);
    g_free(libname);
    res = g_list_insert_sorted(res, module, dt_imageio_sort_modules_format);
  }
  g_dir_close(dir);
  iio->plugins_format = res;

  return 0;
}

/** Default implementation of supported function, used if storage modules not implements supported() */
static gboolean default_supported(struct dt_imageio_module_storage_t *self,
                                  struct dt_imageio_module_format_t *format)
{
  return TRUE;
}
/** Default implementation of dimension module function, used if storage modules does not implements
 * dimension() */
static int _default_storage_dimension(struct dt_imageio_module_storage_t *self, dt_imageio_module_data_t *data,
                                      uint32_t *width, uint32_t *height)
{
  return 0;
}
/** a NOP for when a default should do nothing */
static void _default_storage_nop(struct dt_imageio_module_storage_t *self)
{
}

static int dt_imageio_load_module_storage(dt_imageio_module_storage_t *module, const char *libname,
                                          const char *module_name)
{
  g_strlcpy(module->plugin_name, module_name, sizeof(module->plugin_name));

#define INCLUDE_API_FROM_MODULE_LOAD "imageio_load_module_storage"
#include "imageio/storage/imageio_storage_api.h"

  if(darktable.gui)
  {
    if(!module->gui_init) goto api_h_error;

    module->actions = (dt_action_t){ DT_ACTION_TYPE_SECTION,
      module->plugin_name,
      module->name(module),
      NULL,
      NULL,
      NULL };

    dt_action_insert_sorted(&darktable.control->actions_storage, &module->actions);
  }
  else
  {
    module->gui_init = _default_storage_nop;
  }
  if(!module->dimension) module->dimension = _default_storage_dimension;
  if(!module->recommended_dimension) module->recommended_dimension = _default_storage_dimension;
  if(!module->export_dispatched) module->export_dispatched = _default_storage_nop;

  module->widget = NULL;
  module->parameter_lua_type = LUAA_INVALID_TYPE;

#ifdef USE_LUA
  {
    char pseudo_type_name[1024];
    snprintf(pseudo_type_name, sizeof(pseudo_type_name), "dt_imageio_module_storage_data_%s",
             module->plugin_name);
    luaA_Type my_type
        = luaA_type_add(darktable.lua_state.state, pseudo_type_name, module->params_size(module));
    module->parameter_lua_type = dt_lua_init_type_type(darktable.lua_state.state, my_type);
    luaA_struct_type(darktable.lua_state.state, my_type);
    dt_lua_register_storage_type(darktable.lua_state.state, module, my_type);
#endif
    module->init(module);
#ifdef USE_LUA
    lua_pushcfunction(darktable.lua_state.state, dt_lua_type_member_luaautoc);
    dt_lua_type_register_struct_type(darktable.lua_state.state, my_type);
  }
#endif

  return 0;
}

static int dt_imageio_load_modules_storage(dt_imageio_t *iio)
{
  iio->plugins_storage = NULL;
  dt_imageio_module_storage_t *module;
  char plugindir[PATH_MAX] = { 0 }, plugin_name[256];
  const gchar *d_name;
  dt_loc_get_plugindir(plugindir, sizeof(plugindir));
  g_strlcat(plugindir, "/plugins/imageio/storage", sizeof(plugindir));
  GDir *dir = g_dir_open(plugindir, 0, NULL);
  if(!dir) return 1;
  const int name_offset = strlen(SHARED_MODULE_PREFIX),
            name_end = strlen(SHARED_MODULE_PREFIX) + strlen(SHARED_MODULE_SUFFIX);
  while((d_name = g_dir_read_name(dir)))
  {
    // get lib*.so
    if(!g_str_has_prefix(d_name, SHARED_MODULE_PREFIX)) continue;
    if(!g_str_has_suffix(d_name, SHARED_MODULE_SUFFIX)) continue;
    g_strlcpy(plugin_name, d_name + name_offset, strlen(d_name) - name_end + 1);
    module = (dt_imageio_module_storage_t *)calloc(1, sizeof(dt_imageio_module_storage_t));
    gchar *libname = g_module_build_path(plugindir, (const gchar *)plugin_name);
    if(dt_imageio_load_module_storage(module, libname, plugin_name))
    {
      g_free(libname);
      free(module);
      continue;
    }
    module->gui_data = NULL;
    module->gui_init(module);
    if(module->widget) g_object_ref(module->widget);
    g_free(libname);
    dt_imageio_insert_storage(module);
  }
  g_dir_close(dir);
  return 0;
}

void dt_imageio_init(dt_imageio_t *iio)
{
  iio->plugins_format = NULL;
  iio->plugins_storage = NULL;

  dt_imageio_load_modules_format(iio);
  dt_imageio_load_modules_storage(iio);
}

void dt_imageio_cleanup(dt_imageio_t *iio)
{
  while(iio->plugins_format)
  {
    dt_imageio_module_format_t *module = (dt_imageio_module_format_t *)(iio->plugins_format->data);
    module->gui_cleanup(module);
    module->cleanup(module);
    if(module->widget) g_object_unref(module->widget);
    if(module->module) g_module_close(module->module);
    free(module);
    iio->plugins_format = g_list_delete_link(iio->plugins_format, iio->plugins_format);
  }
  while(iio->plugins_storage)
  {
    dt_imageio_module_storage_t *module = (dt_imageio_module_storage_t *)(iio->plugins_storage->data);
    module->gui_cleanup(module);
    if(module->widget) g_object_unref(module->widget);
    if(module->module) g_module_close(module->module);
    free(module);
    iio->plugins_storage = g_list_delete_link(iio->plugins_storage, iio->plugins_storage);
  }
}

dt_imageio_module_format_t *dt_imageio_get_format()
{
  dt_imageio_t *iio = darktable.imageio;
  const char *format_name = dt_conf_get_string_const("plugins/lighttable/export/format_name");
  dt_imageio_module_format_t *format = dt_imageio_get_format_by_name(format_name);
  // if the format from the config isn't available default to jpeg, if that's not available either just use
  // the first we have
  if(!format) format = dt_imageio_get_format_by_name("jpeg");
  if(!format) format = iio->plugins_format->data;
  return format;
}

dt_imageio_module_storage_t *dt_imageio_get_storage()
{
  dt_imageio_t *iio = darktable.imageio;
  const char *storage_name = dt_conf_get_string_const("plugins/lighttable/export/storage_name");
  dt_imageio_module_storage_t *storage = dt_imageio_get_storage_by_name(storage_name);
  // if the storage from the config isn't available default to disk, if that's not available either just use
  // the first we have
  if(!storage) storage = dt_imageio_get_storage_by_name("disk");
  if(!storage) storage = iio->plugins_storage->data;
  return storage;
}

dt_imageio_module_format_t *dt_imageio_get_format_by_name(const char *name)
{
  if(!name) return NULL;
  dt_imageio_t *iio = darktable.imageio;
  for(GList *it = iio->plugins_format; it; it = g_list_next(it))
  {
    dt_imageio_module_format_t *module = (dt_imageio_module_format_t *)it->data;
    if(!strcmp(module->plugin_name, name)) return module;
  }
  return NULL;
}

dt_imageio_module_storage_t *dt_imageio_get_storage_by_name(const char *name)
{
  if(!name) return NULL;
  dt_imageio_t *iio = darktable.imageio;
  for(GList *it = iio->plugins_storage; it; it = g_list_next(it))
  {
    dt_imageio_module_storage_t *module = (dt_imageio_module_storage_t *)it->data;
    if(!strcmp(module->plugin_name, name)) return module;
  }
  return NULL;
}

dt_imageio_module_format_t *dt_imageio_get_format_by_index(int index)
{
  dt_imageio_t *iio = darktable.imageio;
  GList *it = g_list_nth(iio->plugins_format, index);
  if(!it) it = iio->plugins_format;
  return (dt_imageio_module_format_t *)it->data;
}

dt_imageio_module_storage_t *dt_imageio_get_storage_by_index(int index)
{
  dt_imageio_t *iio = darktable.imageio;
  GList *it = g_list_nth(iio->plugins_storage, index);
  if(!it) it = iio->plugins_storage;
  return (dt_imageio_module_storage_t *)it->data;
}

int dt_imageio_get_index_of_format(dt_imageio_module_format_t *format)
{
  dt_imageio_t *iio = darktable.imageio;
  return g_list_index(iio->plugins_format, format);
}
int dt_imageio_get_index_of_storage(dt_imageio_module_storage_t *storage)
{
  dt_imageio_t *iio = darktable.imageio;
  return g_list_index(iio->plugins_storage, storage);
}

void dt_imageio_insert_storage(dt_imageio_module_storage_t *storage)
{
  darktable.imageio->plugins_storage
      = g_list_insert_sorted(darktable.imageio->plugins_storage, storage, dt_imageio_sort_modules_storage);
  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_IMAGEIO_STORAGE_CHANGE);
}

void dt_imageio_remove_storage(dt_imageio_module_storage_t *storage)
{
  darktable.imageio->plugins_storage  = g_list_remove(darktable.imageio->plugins_storage, storage);
  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_IMAGEIO_STORAGE_CHANGE);
}

gchar *dt_imageio_resizing_factor_get_and_parsing(double *num, double *denum)
{
  double _num, _denum;
  gchar *scale_str = dt_conf_get_string("plugins/lighttable/export/resizing_factor");

  char sep[4] = "";
  snprintf( sep, 4, "%g", (double) 3/2);
  int i = -1;
  while(scale_str[++i])
  {
      if((scale_str[i] == '.') || (scale_str[i] == ',')) scale_str[i] = sep[1];
  }

  gchar *pdiv = strchr(scale_str, '/');

  if(pdiv == NULL)
  {
    _num = atof(scale_str);
    _denum = 1;
  }
  else if(pdiv-scale_str == 0)
  {
    _num = 1;
    _denum = atof(pdiv + 1);
}
  else
{
    _num = atof(scale_str);
    _denum = atof(pdiv+1);
  }

  if(_num == 0.0) _num = 1.0;
  if(_denum == 0.0) _denum = 1.0;

  *num = _num;
  *denum = _denum;

  dt_conf_set_string("plugins/lighttable/export/resizing_factor", scale_str);
  return scale_str;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
