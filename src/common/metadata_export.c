/*
    This file is part of darktable,
    Copyright (C) 2019-2020 darktable developers.

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
#include "control/conf.h"
#include "common/metadata_export.h"

uint32_t dt_lib_export_metadata_default_flags(void)
{
  const uint32_t flags = DT_META_EXIF | DT_META_METADATA | DT_META_GEOTAG | DT_META_TAG | DT_META_DT_HISTORY;
  return flags;
}

const char flags_keyword[] = "plugins/lighttable/export/metadata_flags";
const char formula_keyword[] = "plugins/lighttable/export/metadata_formula";

uint32_t dt_lib_export_metadata_get_conf_flags(void)
{
  const char *metadata_flags = dt_conf_get_string_const(flags_keyword);
  const int32_t flags = strtol(metadata_flags, NULL, 16);
  return flags;
}

char *dt_lib_export_metadata_get_conf(void)
{
  char *metadata_presets = NULL;
  if(dt_conf_key_exists(flags_keyword))
  {
    metadata_presets = dt_conf_get_string(flags_keyword);
    int i = 0;
    char *conf_keyword = g_strdup_printf("%s%d", formula_keyword, i);
    while(dt_conf_key_exists(conf_keyword))
    {
      gchar *nameformula = dt_conf_get_string(conf_keyword);
      g_free(conf_keyword);
      if(nameformula[0])
      {
        char *formula = g_strstr_len(nameformula, strlen(nameformula), ";");
        if(formula)
        {
          formula[0] = '\0';
          formula ++;
          metadata_presets = dt_util_dstrcat(metadata_presets,"\1%s\1%s", nameformula, formula);
        }
      }
      g_free(nameformula);
      i++;
      conf_keyword = g_strdup_printf("%s%d", formula_keyword, i);
    }
    g_free(conf_keyword);
  }
  else
  {
    metadata_presets = g_strdup_printf("%x", dt_lib_export_metadata_default_flags());
  }
  return metadata_presets;
}

void dt_lib_export_metadata_set_conf(const char *metadata_presets)
{
  GList *list = dt_util_str_to_glist("\1", metadata_presets);
  int i = 0;
  char *conf_keyword = NULL;
  char *nameformula = NULL;
  if(list)
  {
    char *flags_hexa = list->data;
    dt_conf_set_string(flags_keyword, flags_hexa);
    list = g_list_remove(list, flags_hexa);
    g_free(flags_hexa);
    if(list)
    {
      for(GList *tags = list; tags; tags = g_list_next(tags))
      {
        const char *tagname = (char *)tags->data;
        tags = g_list_next(tags);
        if(!tags) break;
        const char *formula = (char *)tags->data;
        nameformula = g_strdup_printf("%s;%s", tagname, formula);
        conf_keyword = g_strdup_printf("%s%d", formula_keyword, i);
        dt_conf_set_string(conf_keyword, nameformula);
        g_free(nameformula);
        g_free(conf_keyword);
        i++;
      }
    }
  }
  else dt_conf_set_string(flags_keyword, "");
  g_list_free_full(list, g_free);

  // clean up deprecated formulas
  conf_keyword = g_strdup_printf("%s%d", formula_keyword, i);
  while(dt_conf_key_exists(conf_keyword))
  {
    dt_conf_set_string(conf_keyword, "");
    g_free(conf_keyword);
    i++;
    conf_keyword = g_strdup_printf("%s%d", formula_keyword, i);
  }
  g_free(conf_keyword);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
