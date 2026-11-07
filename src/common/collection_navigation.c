/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

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
#include "common/collection_navigation.h"
#include "common/collection.h"
#include "control/conf.h"

#include <string.h>

static dt_collection_navigation_t _history;
static gboolean _restoring;

void dt_collection_navigation_clear(dt_collection_navigation_t *history)
{
  for(unsigned int i = 0; i < history->length; i++)
    g_free(history->entries[i].rules);
  memset(history, 0, sizeof(*history));
}

void dt_collection_navigation_record(dt_collection_navigation_t *history,
                                     const char *rules, const int position)
{
  if(!rules || !*rules) return;
  if(history->length)
  {
    history->entries[history->current].position = MAX(1, position);
    if(!g_strcmp0(history->entries[history->current].rules, rules)) return;

    // a new destination after going back replaces the forward trail
    while(history->length > history->current + 1)
      g_free(history->entries[--history->length].rules);
  }
  if(history->length == DT_COLLECTION_NAVIGATION_LIMIT)
  {
    g_free(history->entries[0].rules);
    memmove(history->entries, history->entries + 1,
            (DT_COLLECTION_NAVIGATION_LIMIT - 1) * sizeof(history->entries[0]));
    history->length--;
  }
  history->current = history->length++;
  history->entries[history->current] = (dt_collection_navigation_entry_t)
    { g_strdup(rules), 1 };
}

gboolean dt_collection_navigation_can_move(const dt_collection_navigation_t *history,
                                            const int direction)
{
  return history->length && ((direction == -1 && history->current > 0)
                            || (direction == 1 && history->current + 1 < history->length));
}

const dt_collection_navigation_entry_t *dt_collection_navigation_move(
    dt_collection_navigation_t *history, const int direction, const int position)
{
  if(!dt_collection_navigation_can_move(history, direction)) return NULL;
  history->entries[history->current].position = MAX(1, position);
  history->current += direction;
  return &history->entries[history->current];
}

void dt_collection_navigation_record_current(void)
{
  if(_restoring) return;
  char rules[4096];
  if(!dt_collection_serialize(rules, sizeof(rules), FALSE))
    dt_collection_navigation_record(&_history, rules,
      dt_conf_get_int("plugins/lighttable/collect/history_pos0"));
}

void dt_collection_navigation_cleanup(void)
{
  dt_collection_navigation_clear(&_history);
  _restoring = FALSE;
}

gboolean dt_collection_navigation_available(const int direction)
{
  return dt_collection_navigation_can_move(&_history, direction);
}

void dt_collection_navigation_go(const int direction)
{
  const dt_collection_navigation_entry_t *entry = dt_collection_navigation_move(
    &_history, direction, dt_conf_get_int("plugins/lighttable/collect/history_pos0"));
  if(!entry) return;

  // thumbtable consumes this offset when the deserializer raises collection-changed
  dt_conf_set_int("plugins/lighttable/collect/history_next_pos", entry->position);
  _restoring = TRUE;
  dt_collection_deserialize(entry->rules, FALSE);
  _restoring = FALSE;
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
