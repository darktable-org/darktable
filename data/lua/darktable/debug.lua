local indent_string = "   "
local table = require "table"
local dt = require "darktable"
local debug = require "debug"
local introspect_internal
local introspect_body
local M = {debug = false, max_depth = 10, known = {[_G] = "_G", [_ENV] = "_ENV" }}
local depth = 0;

local function get_userdata_type(object)
  if type(object) ~= "userdata" then return end
  local metatable = getmetatable(object);
  if not metatable then return end
  return metatable.__luaA_TypeName
end

local function header(object,indent,name,obj_expand_mode)
  if name == nil then name = "" end
  local obj_type = type(object)
  local complement =""
  local user_type = get_userdata_type(object);
  if user_type then
    complement = complement..","..user_type
  end
  if obj_type ~= "number" then
    name = tostring(name)
  end
  return indent..name.." ("..obj_type..complement..")"
end

local function key(object) 
  local user_type = get_userdata_type(object);
  if not user_type then
    return object
  end
  if user_type == "dt_lua_image_t" then
    return "dt_lua_image_t"..object.id
  elseif user_type == "dt_lua_film_t" then
    return "dt_lua_film_t"..object.id
  end
  return object
end

local function introspect_metatable(object,indent,name,known,ancestors)
  if not M.debug then return "" end
  if name == nil then name = "(unknown)" end
  local metatable = getmetatable(object);
  if metatable ==  nil then return indent.."(no metatable)\n" end
  local result = indent..name..".metatable"
  table.insert(ancestors,name)
  result = result..introspect_body(metatable,indent..indent_string,"metatable",known,ancestors)
  table.remove(ancestors);
  return result
end

local function introspect_uservalue(object,indent,name,known,ancestors)
  if not M.debug then return "" end
  if name == nil then name = "(unknown)" end
  local uservalue = debug.getuservalue(object);
  if uservalue ==  nil then return indent.."(no uservalue)\n" end
  local result = indent..name..".uservalue"
  table.insert(ancestors,name)
  result = result..introspect_body(uservalue,indent..indent_string,"uservalue",known,ancestors)
  table.remove(ancestors);
  return result
end

introspect_body = function (object,indent,name,known,ancestors)
  if name == nil then name = "(unknown)" end
  local result = ""
  if #indent > M.max_depth*#indent_string then
    return "max depth\n"
  end
  local obj_expand_mode = type(object)
  -- simple types
  if obj_expand_mode == "nil" then
    return "\n"
  elseif obj_expand_mode == "number" then
    return " : "..object.."\n"
  elseif obj_expand_mode == "string" then
    return " : \""..object.."\"\n"
  elseif obj_expand_mode == "boolean" then
    return  " : "..tostring(object).."\n"
  end
  -- complex types
  if known[key(object)] then
    return ": "..known[key(object)].."\n";
  end
  known[key(object)]= table.concat(ancestors,".").."."..name;
  if obj_expand_mode == "table" then
    if next(object) == nil then return " : empty\n" end
    result = " :\n"
    for k,v in pairs(object) do
      table.insert(ancestors,name)
      result = result..introspect_internal(v,indent..indent_string,k,known,ancestors)
      table.remove(ancestors);
    end
    result = result..introspect_metatable(object,indent..indent_string,name,known,ancestors)
    result = result..introspect_uservalue(object,indent..indent_string,name,known,ancestors)
    return result;
  elseif obj_expand_mode == "function" then
    return "\n"
  elseif obj_expand_mode == "thread" then
    return "\n"
  elseif obj_expand_mode == "userdata" then
    local metatable = getmetatable(object);
    if metatable and metatable.__tostring then 
      result = result.. " : "..tostring(object).."\n"
    else
      result = result.. " :\n"
    end
    if metatable and metatable.__pairs then 
      for k,v in pairs(object) do
        table.insert(ancestors,name)
        result = result..introspect_internal(v,indent..indent_string,k,known,ancestors)
        table.remove(ancestors);
      end
    end
    result = result..introspect_metatable(object,indent..indent_string,name,known,ancestors)
    result = result..introspect_uservalue(object,indent..indent_string,name,known,ancestors)
    return result
  end
  error("unknown type of object")
end

introspect_internal = function(object,indent,name,known,ancestors)
  local result = header(object,indent,name)
  return result..introspect_body(object,indent,name,known,ancestors)
end

function M.dump(object,name,orig_known) 
  if name == nil or name == "" then
    name = "toplevel"
  end
  if orig_known == nil then
    orig_known = M.known
  end
  local known = {}
  for k,v in pairs(orig_known) do
    known[key(k)] = v
  end
  if(key(object)) then
    known[key(object)] = nil -- always document the object that was actually asked
  end
  return introspect_internal(object,"",name,known,{})
end

function M.type(object)
  if type(object) ~= "userdata" then return type(object) end
  local metatable = getmetatable(object);
  if not metatable or not metatable.__luaA_TypeName then return "userdata" end
  return metatable.__luaA_TypeName
end

dt.debug = M
return M
--
-- vim: shiftwidth=2 expandtab tabstop=2 cindent syntax=lua
