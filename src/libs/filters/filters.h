/*
    This file is part of darktable,
    Copyright (C) 2010-2022 darktable developers.

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

#include "common/collection.h"
#include "libs/lib.h"

#define PARAM_STRING_SIZE 256 // FIXME: is this enough !?

typedef struct dt_lib_filters_rule_t
{
  dt_collection_properties_t prop;

  GtkWidget *w_widget_box;
  char raw_text[PARAM_STRING_SIZE];
  GtkWidget *w_special_box;
  void *w_specific; // structure which contains all the widgets specific to the rule type

  int manual_widget_set; // when we update manually the widget, we don't want events to be handled
  gboolean cleaning;     // if we have started a gui_cleanup (we don't want certain event to occurs)
  gboolean leaving;      // if the lib that owned the filter lost focus because the view is changing

  gboolean topbar;

  void *parent; // parent structure. This may esp. be used for the following function
  void (*rule_changed)(void *rule);
} dt_lib_filters_rule_t;

gboolean dt_filters_exists(const dt_collection_properties_t prop, const gboolean top);
gboolean dt_filters_update(dt_lib_filters_rule_t *rule, gchar *last_where_ext);
void dt_filters_init(dt_lib_filters_rule_t *rule, const dt_collection_properties_t prop, const gchar *text,
                     dt_lib_module_t *self, gboolean top);
void dt_filters_reset(dt_lib_filters_rule_t *rule, const gboolean signal);
void dt_filters_free(dt_lib_filters_rule_t *rule);

gchar *dt_filters_colors_pretty_print(const gchar *raw_txt);

int dt_filters_get_count();
dt_collection_properties_t dt_filters_get_prop_by_pos(const int pos);