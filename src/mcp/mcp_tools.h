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

#pragma once

#include <json-glib/json-glib.h>

/** load tool metadata (name/description/inputSchema) from a JSON file; returns
    the number of tools, or -1 if the file is missing or not a JSON array */
int mcp_tools_load(const char *path);

/** JSON array of tool descriptors for a `tools/list` result (caller owns it) */
JsonNode *mcp_tools_list_node(void);

/** dispatch a `tools/call`, returning an MCP tool result { content, isError }
    if `name` is unknown, sets *found = FALSE and returns NULL */
JsonNode *mcp_tools_call_node(const char *name, JsonObject *arguments, gboolean *found);
