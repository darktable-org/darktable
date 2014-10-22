#!/usr/bin/env lua

--[[
Takes two mandatory and one optiona argument
* a file containing a dump of the old API
* a file containing a dump of the new API
* optionally "true" to have verbose output

This script will compare the two API and report any differences it sees
optionnaly printing details of the difference

Use this to quickly assess what has changed between two versions of DT

Note that dumps are obtained via lua_doc/dumper.lua
]]

table = require "table"
args = {...}
oldapi = require(string.sub(args[1],1,-5))
newapi = require(string.sub(args[2],1,-5))
verbose = args[3]


local function sorted_pairs (t, f)
  local a = {}
  for n in pairs(t) do table.insert(a, n) end
  table.sort(a, f)
  local i = 0      -- iterator variable
  local iter = function ()   -- iterator function
    i = i + 1
    if a[i] == nil then return nil
    else return a[i], t[a[i]]
    end
  end
  return iter
end

local function length(ptree)
  if #ptree == 0 then return 0 end
  return length(ptree[2])+1
end


function find_parent(node,ptree) 
  if type(node) ~= "table" then return end

  if length(ptree) == 0 then
    node.__parent = {}
    for k,v in sorted_pairs(node) do
      find_parent(v,{k,node.__parent})
    end
    return
  end

  if ptree[1] == "__parent" then return end

  if not node.__parent  or length(node.__parent) > length(ptree) then
    node.__parent = ptree
    for k,v in sorted_pairs(node) do
      find_parent(v,{k,ptree})
    end
  end
end



local function build_name(ptree)
  if #ptree == 0 then return "" end
  if #ptree[2] == 0 then return ptree[1] end
  return build_name(ptree[2]).."."..ptree[1]
end


local function specific_filter(old,new,name,ptree) 
  if ptree[1] == "read" and ptree[2][1] == "__attributes" then
    return true
  end
  if ptree[1] == "write" and ptree[2][1] == "__attributes" then
    return true
  end
  if ptree[1] == "is_attribute" and ptree[2][1] == "__attributes" then
    return true
  end
  if ptree[1] == "is_self" and ptree[2][1] == "__attributes" then
    return true
  end
  if ptree[1] == "parent" and ptree[2][1] == "__attributes" then
    return true
  end
  return false
end


local known = {}
function dump_node(node,prefix)
  if type(node) ~= "table" then
    return tostring(node)
  elseif known[node] then
    return build_name(node.__parent)
  elseif verbose== "nodes" then
    known[node] = true
    local result="{\n"
    for k,v in sorted_pairs(node) do
      if k ~= "__parent" and k ~="__done" then
        result = result..prefix.."   "..k.." = "..dump_node(v,prefix.."      ").."\n"
      end
    end
    result = result ..prefix.."}"
    return result
  else
    return build_name(node.__parent)
  end
end

function trace_change(name,change_type,old,new)
  print(name.." : "..change_type)
  if verbose then
    print("**************** old ************************")
    known = {}
    print(dump_node(old,""))
    print("**************** new ************************")
    known = {}
    print(dump_node(new,""))
    print("*********************************************")
  end
end

local function compare_any(old,new,ptree)
  local name
  if ptree[1] == "__done" then return end
  if ptree[1] == "__parent" then return end
  if type(old) == "table" and old.__done then return end
  if type(old) == "table" then
    name = build_name(old.__parent)
    old.__done = true;
  else
    name = build_name(ptree)
  end
  if specific_filter(old,new,name,ptree) then
    return
  end
  if old == nil then
    trace_change(name,"Node added",old,new)
    return
  end
  if new == nil then
    trace_change(name,"Node deleted",old,new)
    return
  end
  if type(old) ~= type(new) then
    trace_change(name,"Type changed from "..type(old).." to "..type(new),old,new)
    return
  end
  if type(old) ~= "table" then
    if old ~= new then
      trace_change(name,"("..type(old)..") value changed",old,new)
    end
    return
  else
    for k,v in sorted_pairs(old) do 
      compare_any(v,new[k],{k,old.__parent})
    end
    for k,v in sorted_pairs(new) do 
      if not old[k] then
        compare_any(old[k],v,{k,new.__parent})
      end
    end
  end

end



find_parent(oldapi,{})
find_parent(newapi,{})

compare_any(oldapi,newapi,{})

-- vim: shiftwidth=2 expandtab tabstop=2 cindent syntax=lua
