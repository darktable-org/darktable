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

#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <glib.h>

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define DT_LIVE_BRIDGE_VERSION 1
#define DT_LIVE_BRIDGE_SERVICE "org.darktable.service"
#define DT_LIVE_BRIDGE_PATH "/darktable"
#define DT_LIVE_BRIDGE_INTERFACE "org.darktable.service.Remote"

static void usage(FILE *stream, const char *progname)
{
  fprintf(stream,
          "Usage:\n"
          "  %s get-session\n"
          "  %s set-exposure <EV>\n"
          "  %s --help\n",
          progname, progname, progname);
}

static gboolean print_json_only(const gchar *json, GError **error)
{
  g_autoptr(JsonParser) parser = json_parser_new();
  if(!json_parser_load_from_data(parser, json, -1, error)) return FALSE;

  g_autoptr(JsonGenerator) generator = json_generator_new();
  JsonNode *root = json_parser_get_root(parser);
  json_generator_set_root(generator, root);
  json_generator_set_pretty(generator, FALSE);

  g_autofree gchar *normalized = json_generator_to_data(generator, NULL);
  fputs(normalized, stdout);
  fputc('\n', stdout);
  return TRUE;
}

static gboolean call_lua(const gchar *lua_source, gchar **json_result, GError **error)
{
  g_autoptr(GDBusConnection) connection = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, error);
  if(connection == NULL) return FALSE;

  g_autoptr(GVariant) result = g_dbus_connection_call_sync(connection, DT_LIVE_BRIDGE_SERVICE,
                                                           DT_LIVE_BRIDGE_PATH,
                                                           DT_LIVE_BRIDGE_INTERFACE, "Lua",
                                                           g_variant_new("(s)", lua_source),
                                                           G_VARIANT_TYPE("(s)"),
                                                           G_DBUS_CALL_FLAGS_NONE, -1, NULL,
                                                           error);
  if(result == NULL) return FALSE;

  g_variant_get(result, "(s)", json_result);
  return TRUE;
}

static gchar *build_lua_command(const gchar *command, gboolean have_exposure, double exposure)
{
  const char *lua_template =
    "local bridge = rawget(_G, '__darktable_live_bridge_v1')\n"
    "if not bridge then\n"
    "  local darktable = require 'darktable'\n"
    "  bridge = {\n"
    "    bridgeVersion = 1,\n"
    "    exposureAction = nil,\n"
    "    view = '',\n"
    "    renderSequence = 0,\n"
    "    historyChangeSequence = 0,\n"
    "    imageLoadSequence = 0\n"
    "  }\n"
    "\n"
    "  local function json_escape(value)\n"
    "    local escaped = {}\n"
    "    for index = 1, #value do\n"
    "      local byte = string.byte(value, index)\n"
    "      if byte == 92 then escaped[#escaped + 1] = string.char(92, 92)\n"
    "      elseif byte == 34 then escaped[#escaped + 1] = string.char(92, 34)\n"
    "      elseif byte == 8 then escaped[#escaped + 1] = string.char(92, 98)\n"
    "      elseif byte == 12 then escaped[#escaped + 1] = string.char(92, 102)\n"
    "      elseif byte == 10 then escaped[#escaped + 1] = string.char(92, 110)\n"
    "      elseif byte == 13 then escaped[#escaped + 1] = string.char(92, 114)\n"
    "      elseif byte == 9 then escaped[#escaped + 1] = string.char(92, 116)\n"
    "      elseif byte <= 31 then escaped[#escaped + 1] = string.char(92, 117) .. string.format('%%04x', byte)\n"
    "      else escaped[#escaped + 1] = string.char(byte) end\n"
    "    end\n"
    "    return table.concat(escaped)\n"
    "  end\n"
    "\n"
    "  local function json_number(value)\n"
    "    if type(value) ~= 'number' or value ~= value or value == math.huge or value == -math.huge then\n"
    "      error('invalid numeric value')\n"
    "    end\n"
    "    local encoded = string.format('%%.17g', value)\n"
    "    if encoded == '-0' then encoded = '0' end\n"
    "    return encoded\n"
    "  end\n"
    "\n"
    "  local function json_encode(value)\n"
    "    local value_type = type(value)\n"
    "    if value_type == 'nil' then\n"
    "      return 'null'\n"
    "    elseif value_type == 'string' then\n"
    "      return '\"' .. json_escape(value) .. '\"'\n"
    "    elseif value_type == 'number' then\n"
    "      return json_number(value)\n"
    "    elseif value_type == 'boolean' then\n"
    "      return value and 'true' or 'false'\n"
    "    elseif value_type == 'table' then\n"
    "      local max_index = 0\n"
    "      local count = 0\n"
    "      local is_array = true\n"
    "      for key, _ in pairs(value) do\n"
    "        count = count + 1\n"
    "        if type(key) ~= 'number' or key < 1 or key %% 1 ~= 0 then\n"
    "          is_array = false\n"
    "          break\n"
    "        end\n"
    "        if key > max_index then max_index = key end\n"
    "      end\n"
    "      if is_array and max_index == count then\n"
    "        local items = {}\n"
    "        for index = 1, max_index do\n"
    "          items[index] = json_encode(value[index])\n"
    "        end\n"
    "        return '[' .. table.concat(items, ',') .. ']'\n"
    "      end\n"
    "\n"
    "      local keys = {}\n"
    "      for key, _ in pairs(value) do\n"
    "        if type(key) ~= 'string' then error('json object key must be a string') end\n"
    "        keys[#keys + 1] = key\n"
    "      end\n"
    "      table.sort(keys)\n"
    "      local items = {}\n"
    "      for _, key in ipairs(keys) do\n"
    "        items[#items + 1] = '\"' .. json_escape(key) .. '\":' .. json_encode(value[key])\n"
    "      end\n"
    "      return '{' .. table.concat(items, ',') .. '}'\n"
    "    end\n"
    "    error('unsupported json value type: ' .. value_type)\n"
    "  end\n"
    "\n"
    "  local function update_view()\n"
    "    local current_view = darktable.gui.current_view()\n"
    "    bridge.view = current_view and tostring(current_view) or ''\n"
    "  end\n"
    "\n"
    "  local function current_image()\n"
    "    update_view()\n"
    "    if bridge.view ~= 'darkroom' then\n"
    "      return nil, 'unsupported-view'\n"
    "    end\n"
    "\n"
    "    local darkroom = darktable.gui.views.darkroom\n"
    "    local image = darkroom and darkroom.display_image and darkroom.display_image() or nil\n"
    "    if not image then\n"
    "      return nil, 'no-active-image'\n"
    "    end\n"
    "\n"
    "    return image, nil\n"
    "  end\n"
    "\n"
    "  local function session_object()\n"
    "    update_view()\n"
    "    return {\n"
    "      view = bridge.view,\n"
    "      renderSequence = bridge.renderSequence,\n"
    "      historyChangeSequence = bridge.historyChangeSequence,\n"
    "      imageLoadSequence = bridge.imageLoadSequence\n"
    "    }\n"
    "  end\n"
    "\n"
    "  local function active_image_object(image)\n"
    "    return {\n"
    "      imageId = image.id,\n"
    "      directoryPath = image.path,\n"
    "      fileName = image.filename,\n"
    "      sourceAssetPath = image.path .. '/' .. image.filename\n"
    "    }\n"
    "  end\n"
    "\n"
    "  local function unavailable(reason)\n"
    "    return {\n"
    "      bridgeVersion = bridge.bridgeVersion,\n"
    "      reason = reason,\n"
    "      session = session_object(),\n"
    "      status = 'unavailable'\n"
    "    }\n"
    "  end\n"
    "\n"
    "  local function numbers_equal(left, right)\n"
    "    return type(left) == 'number' and type(right) == 'number'\n"
    "      and left == left and right == right and math.abs(left - right) <= 1e-6\n"
    "  end\n"
    "\n"
    "  local exposure_soft_min = -3.0\n"
    "  local exposure_soft_max = 4.0\n"
    "\n"
    "  local function exposure_position_to_ev(position)\n"
    "    return exposure_soft_min + ((exposure_soft_max - exposure_soft_min) * position)\n"
    "  end\n"
    "\n"
    "  local function exposure_ev_to_position(value)\n"
    "    return (value - exposure_soft_min) / (exposure_soft_max - exposure_soft_min)\n"
    "  end\n"
    "\n"
    "  local function read_exposure_action(action)\n"
    "    local ok, value = pcall(darktable.gui.action, action, '', '', 0)\n"
    "    if ok and type(value) == 'number' and value == value then return value end\n"
    "    error('exposure action unavailable')\n"
    "  end\n"
    "\n"
    "  local function write_exposure_action(action, value)\n"
    "    local ok, current = pcall(darktable.gui.action, action, '', 'set', value)\n"
    "    if ok and type(current) == 'number' and current == current then return current end\n"
    "    error('exposure action unavailable after update')\n"
    "  end\n"
    "\n"
    "  local function wait_for_exposure_settle(image_id, action, expected, render_before, history_before)\n"
    "    local current = read_exposure_action(action)\n"
    "    local previous_read = current\n"
    "    local stable_reads = 0\n"
    "    local observed_render_sequence = bridge.renderSequence\n"
    "\n"
    "    for _ = 1, 40 do\n"
    "      darktable.control.sleep(100)\n"
    "      local image, reason = current_image()\n"
    "      if not image then return current, observed_render_sequence, reason end\n"
    "      if image.id ~= image_id then return current, observed_render_sequence, 'image-changed' end\n"
    "\n"
    "      current = read_exposure_action(action)\n"
    "      observed_render_sequence = bridge.renderSequence\n"
    "\n"
    "      if numbers_equal(current, previous_read) then\n"
    "        stable_reads = stable_reads + 1\n"
    "      else\n"
    "        stable_reads = 0\n"
    "      end\n"
    "\n"
    "      previous_read = current\n"
    "\n"
    "      if (bridge.renderSequence > render_before or bridge.historyChangeSequence > history_before)\n"
    "         and (numbers_equal(current, expected) or stable_reads >= 2) then\n"
    "        break\n"
    "      end\n"
    "    end\n"
    "\n"
    "    return current, observed_render_sequence, nil\n"
    "  end\n"
    "\n"
    "  local function available_payload(image)\n"
    "    local current = exposure_position_to_ev(read_exposure_action(bridge.exposure_action()))\n"
    "    return {\n"
    "      activeImage = active_image_object(image),\n"
    "      bridgeVersion = bridge.bridgeVersion,\n"
    "      exposure = { current = current },\n"
    "      session = session_object(),\n"
    "      status = 'ok'\n"
    "    }\n"
    "  end\n"
    "\n"
    "  local function get_session()\n"
    "    local image, reason = current_image()\n"
    "    if not image then return unavailable(reason) end\n"
    "    return available_payload(image)\n"
    "  end\n"
    "\n"
    "  local function exposure_action()\n"
    "    if bridge.exposureAction then return bridge.exposureAction end\n"
    "    local candidates = { 'iop/exposure/exposure', 'exposure/exposure' }\n"
    "    for _, candidate in ipairs(candidates) do\n"
    "      local ok, value = pcall(darktable.gui.action, candidate, '', '', 0)\n"
    "      if ok and type(value) == 'number' and value == value then\n"
    "        bridge.exposureAction = candidate\n"
    "        return candidate\n"
    "      end\n"
    "    end\n"
    "    error('exposure action unavailable')\n"
    "  end\n"
    "\n"
    "  local function set_exposure(requested)\n"
    "    local image, reason = current_image()\n"
    "    if not image then return unavailable(reason) end\n"
    "\n"
    "    local action = bridge.exposure_action()\n"
    "    local previous_position = read_exposure_action(action)\n"
    "    local current_position = previous_position\n"
    "    local requested_position = exposure_ev_to_position(requested)\n"
    "\n"
    "    local sequence_before = bridge.renderSequence\n"
    "    local history_before = bridge.historyChangeSequence\n"
    "    if not numbers_equal(exposure_position_to_ev(previous_position), requested) then\n"
    "      write_exposure_action(action, requested)\n"
    "    end\n"
    "\n"
    "    local requested_render_sequence\n"
    "    current_position, requested_render_sequence = wait_for_exposure_settle(image.id, action, requested_position, sequence_before, history_before)\n"
    "    if requested_render_sequence == nil then requested_render_sequence = bridge.renderSequence end\n"
    "\n"
    "    local payload = available_payload(image)\n"
    "    payload.exposure = {\n"
    "      previous = exposure_position_to_ev(previous_position),\n"
    "      requested = requested,\n"
    "      current = exposure_position_to_ev(current_position),\n"
    "      requestedRenderSequence = requested_render_sequence\n"
    "    }\n"
    "    return payload\n"
    "  end\n"
    "\n"
    "  local function on_view_changed(_, _, new_view)\n"
    "    bridge.view = new_view and tostring(new_view) or ''\n"
    "  end\n"
    "\n"
    "  local function on_image_loaded()\n"
    "    bridge.imageLoadSequence = bridge.imageLoadSequence + 1\n"
    "    update_view()\n"
    "  end\n"
    "\n"
    "  local function on_history_changed()\n"
    "    bridge.historyChangeSequence = bridge.historyChangeSequence + 1\n"
    "  end\n"
    "\n"
    "  local function on_pixelpipe_complete()\n"
    "    bridge.renderSequence = bridge.renderSequence + 1\n"
    "  end\n"
    "\n"
    "  local function register_bridge_event(event_name, callback)\n"
    "    local ok, err = pcall(darktable.register_event, 'darktable-live-bridge', event_name, callback)\n"
    "    if ok then return end\n"
    "    ok, err = pcall(darktable.register_event, event_name, callback)\n"
    "    if ok then return end\n"
    "    error(err)\n"
    "  end\n"
    "\n"
    "  register_bridge_event('view-changed', on_view_changed)\n"
    "  register_bridge_event('darkroom-image-loaded', on_image_loaded)\n"
    "  register_bridge_event('darkroom-image-history-changed', on_history_changed)\n"
    "  register_bridge_event('pixelpipe-processing-complete', on_pixelpipe_complete)\n"
    "\n"
    "  bridge.update_view = update_view\n"
    "  bridge.current_image = current_image\n"
    "  bridge.exposure_action = exposure_action\n"
    "  bridge.session_object = session_object\n"
    "  bridge.get_session = get_session\n"
    "  bridge.set_exposure = set_exposure\n"
    "  bridge.json_encode = json_encode\n"
    "  bridge.update_view()\n"
    "  rawset(_G, '__darktable_live_bridge_v1', bridge)\n"
    "end\n"
    "\n"
    "local command = %s\n"
    "local response\n"
    "if command == 'get-session' then\n"
    "  response = bridge.get_session()\n"
    "elseif command == 'set-exposure' then\n"
    "  response = bridge.set_exposure(%s)\n"
    "else\n"
    "  error('unknown command: ' .. tostring(command))\n"
    "end\n"
    "return bridge.json_encode(response)\n";

  const char *command_literal = NULL;
  const char *exposure_literal = "nil";
  gchar exposure_buffer[G_ASCII_DTOSTR_BUF_SIZE] = { 0 };

  if(g_strcmp0(command, "get-session") == 0)
    command_literal = "'get-session'";
  else if(g_strcmp0(command, "set-exposure") == 0)
    command_literal = "'set-exposure'";

  if(have_exposure)
  {
    g_ascii_dtostr(exposure_buffer, sizeof(exposure_buffer), exposure);
    exposure_literal = exposure_buffer;
  }

  return g_strdup_printf(lua_template, command_literal, exposure_literal);
}

int main(int argc, char **argv)
{
  const char *progname = argc > 0 ? argv[0] : "darktable-live-bridge";

  if(argc == 2 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h")))
  {
    usage(stdout, progname);
    return 0;
  }

  const gchar *command = NULL;
  gboolean have_exposure = FALSE;
  double exposure = 0.0;

  if(argc == 2 && !strcmp(argv[1], "get-session"))
  {
    command = "get-session";
  }
  else if(argc == 3 && !strcmp(argv[1], "set-exposure"))
  {
    char *endptr = NULL;
    errno = 0;
    exposure = g_ascii_strtod(argv[2], &endptr);
    if(errno != 0 || endptr == argv[2] || (endptr && *endptr != '\0') || !isfinite(exposure))
    {
      fprintf(stderr, "invalid exposure value\n");
      return 1;
    }
    command = "set-exposure";
    have_exposure = TRUE;
  }
  else
  {
    usage(stderr, progname);
    return 1;
  }

  g_autofree gchar *lua_source = build_lua_command(command, have_exposure, exposure);
  if(lua_source == NULL)
  {
    fprintf(stderr, "failed to build Lua command\n");
    return 1;
  }

  g_autofree gchar *json_result = NULL;
  g_autoptr(GError) error = NULL;
  if(!call_lua(lua_source, &json_result, &error))
  {
    fprintf(stderr, "%s\n", error != NULL ? error->message : "DBus call failed");
    return 1;
  }

  if(json_result == NULL || json_result[0] == '\0')
  {
    fprintf(stderr, "darktable returned an empty response\n");
    return 1;
  }

  if(!print_json_only(json_result, &error))
  {
    fprintf(stderr, "%s\n", error != NULL ? error->message : "invalid JSON response");
    return 1;
  }

  return 0;
}
