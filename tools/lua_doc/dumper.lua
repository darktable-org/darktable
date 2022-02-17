

para = function() return "\n" end
node_to_string = function(node,name) 
	if name then
		return name
	else
		return node:get_name()
	end
end

code = function(text) 
	return "<code>"..text.."</code>"
end
emphasis = function(text)
  return "<em>"..text.."</em>"
end

startlist = function() return "\n" end
endlist = function() return "\n" end
listel = function(text) return "\n* "..text end

url = function(text,content) 
  if content then
    return "<ulink url=\""..text.."\">"..content.."</ulink>"
  else
    return text
  end
end

require "content"
doc = require "core"
table = require "table"
dump = require("darktable.debug").dump
dt = require "darktable"


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

local print_node
local add_table_entry

local function print_content(obj,obj_name)
  local result 
  if(type(obj) == "boolean") then
    result = tostring(obj)
  elseif type(obj) == "string" then
    result = "[==["..obj.."]==]"
  elseif type(obj) == "table" and obj._luadoc_type then
    result = print_node(obj,obj_name)
  elseif type(obj) == "table" then
    result = "{\n"
    for k,v in sorted_pairs(obj) do
      result = result..add_table_entry(k,v,obj_name.."[\""..k.."\"]")
    end
    result = result.."}"
  elseif type(obj) == "number" then
    result = tostring(obj)
  elseif type(obj) == "nil" then
    result ="nil"
  else
    error("unhandle attribute type\n"..type(obj))
  end

  return result

end


local known = {}

function print_node(node,node_name)
  local result
  if known[node] then
    table.insert(known[node],node_name)
    return "{} --[=["..known[node].target.."]=]"
    --return "{}"
  else
    known[node] = {target = node_name }
  end
  result = "{\n"
  result = result..add_table_entry("__text",doc.get_text(node),node_name..".__text")
  result = result..add_table_entry("__attributes",node._luadoc_attributes,node_name..".__attributes")

  for k,v in node:unskipped_children() do
    result = result..add_table_entry(k,v,node_name.."[\""..k.."\"]")
  end
  result=result.."}"
  return result
end


add_table_entry = function(key,object,name)
  return "[\""..key.."\"]".." = "..  print_content(object,name)..  ",\n"

end


M = {}

function M.get_doc()
  result = "API = "..  print_node(doc.toplevel,"API").."\n"
  for _,known_entry in pairs(known) do
    for _,target in ipairs(known_entry) do
      result = result..target.." = "..known_entry["target"].."\n"
    end
  end
  return result..[==[return API
--
-- vim: shiftwidth=2 expandtab tabstop=2 cindent syntax=lua
]==]

end


--for k, v in darktable.gui.libs:unskipped_children() do
--  print(v._luadoc_attributes["position"].."aaa")
--end



return M;
--
-- vim: shiftwidth=2 expandtab tabstop=2 cindent syntax=lua
