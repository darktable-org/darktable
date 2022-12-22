/*
    This file is part of darktable,
    Copyright (C) 2018-2020 darktable developers.

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

#include "common/utility.h"
#include "control/conf.h"
#include "common/iop_group.h"

static int _group_number(int group_id)
{
  if      (group_id == IOP_GROUP_EFFECT)  return 5;
  else if(group_id == IOP_GROUP_CORRECT) return 4;
  else if(group_id == IOP_GROUP_COLOR)   return 3;
  else if(group_id == IOP_GROUP_TONE)    return 2;
  else if(group_id == IOP_GROUP_BASIC)   return 1;
  else                                    return 0;
}

int dt_iop_get_group(const dt_iop_module_t *module)
{
  gchar *key = g_strdup_printf("plugins/darkroom/%s/modulegroup", module->op);
  int prefs = dt_conf_get_int(key);

  /* if zero, not found, record it */
  if(!prefs)
  {
    const int default_group = module->default_group();
    dt_conf_set_int(key, _group_number(default_group));
    prefs = default_group;
  }
  else
  {
    gchar *g_key = g_strdup_printf("plugins/darkroom/group_order/%d", prefs);
    prefs = dt_conf_get_int(g_key);

    prefs = 1 << (prefs - 1);

    if(prefs > IOP_GROUP_EFFECT)
      prefs = IOP_GROUP_EFFECT;
    else if(prefs < IOP_GROUP_BASIC)
      prefs = IOP_GROUP_BASIC;

    g_free(g_key);
  }

  g_free(key);
  return prefs;
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

