/*
    This file is part of darktable,
    Copyright (C) 2019-2023 darktable developers.

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

#include "common/darktable.h"
#include "develop/imageop.h"

/** save preset to file */
void dt_presets_save_to_file(const int rowid, const char *preset_name, const char *filedir);

/** load preset from file */
int dt_presets_import_from_file(const char *preset_path);

/** does the module support autoapplying presets ? */
gboolean dt_presets_module_can_autoapply(const gchar *operation);

/** get preset name for given module params */
char *dt_presets_get_name(const char *module_name,
                          const void *params,
                          const uint32_t param_size,
                          const gboolean is_default_params,
                          const void *blend_params,
                          const uint32_t blend_params_size);

/** get currently active preset name for the module */
gchar *dt_get_active_preset_name(dt_iop_module_t *module, gboolean *writeprotect);

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
