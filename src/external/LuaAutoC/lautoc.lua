#!/usr/bin/env lua
-- LuaAutoC - Automagically use C Functions and Structs with the Lua API
-- https://github.com/orangeduck/LuaAutoC
-- Daniel Holden - contact@theorangeduck.com
-- Licensed under BSD

-- Findall Function

function findall(text, pattern)
  local matches = {}
  for word in string.gmatch(text, pattern) do
    table.insert(matches, word)
  end
  return matches
end

-- String Split Function

function string:split(sep)
  local sep, fields = sep or ":", {}
  local pattern = string.format("([^%s]+)", sep)
  self:gsub(pattern, function(c) fields[#fields+1] = c end)
  return fields
end

-- Check Arguments

if #arg ~= 1 then
  print('Usage: lua lautoc.lua <header.h>')
  return
end

-- Open Header

local file = io.open(arg[1], 'r')

if file == nil then
  print(string.format('Error: Cannot load file "%s"', arg[1]))
  return
end

local text = file:read('*all')

io.close(file)

-- Remove all newlines

local text = string.gsub(text, "\r", "")
local text = string.gsub(text, "\n", "")

-- Find all typedefs, structs, and functions

typestrts = findall(text, "typedef struct {.-} %w-;")
typeenums = findall(text, "typedef enum {.-} %w-;")
funcs     = findall(text, "[%w%*]- [%w_]-%(.-%);")

-- Output Typedef Enum Code

for k,v in pairs(typeenums) do
  local _, _, members, typename = string.find(v, "typedef enum {(.-)} (%w-);")
  
  print(string.format("luaA_enum(%s);", typename))
  
  for _, mem in pairs(members:split(",")) do
    local _, _, name = string.find(mem, "(%w+)")
    print(string.format("luaA_enum_value(%s, %s);", typename, name))
  end
  
  print("")
end

-- Output Typedef Struct Code

for k,v in pairs(typestrts) do
  local _, _, members, typename = string.find(v, "typedef struct {(.-)} (%w-);")
  
  print(string.format("luaA_struct(%s);", typename))
  
  for _, mem in pairs(members:split(";")) do
    local meminfo = mem:split(" ")
    print(string.format("luaA_struct_member(%s, %s, %s);", typename, meminfo[2], meminfo[1]))
  end
  
  print("")
end

-- Output Function Code

for k,v in pairs(funcs) do
  local _, _, typename, name, args = string.find(v, "([%w%*]-) ([%w_]-)%((.-)%);")
  
  local argtypes = {}
  for _, arg in pairs(args:split(",")) do
    table.insert(argtypes, arg:split(" ")[1])
  end

  local fstring = string.format("luaA_function(%s, %s", name, typename)
  for _, v in pairs(argtypes) do
    local fstring = fstring .. string.format(", %s", v)
  end
  local fstring = fstring .. ");"
  
  print(fstring)
end

