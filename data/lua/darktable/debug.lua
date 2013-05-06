local indent_string ="   "
local table = require "table"
local dt = require "darktable"
local debug = require "debug"
local introspect_internal
local introspect_body
local M = {debug = false}
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
	if(obj_expand_mode == "constructor") then complement = complement..",constructor" end
	if(obj_expand_mode == "member constructor") then complement = complement..",member constructor" end
	local user_type = get_userdata_type(object);
	if user_type then
		complement = complement..","..user_type
	end
	return indent..name.." ("..obj_type..complement..")"
end

local function expand_mode(object,indent,name,known,ancestors)
	local obj_expand_mode = type(object)
	if obj_expand_mod == "nil" then return nil end
	if known[object] then
		return "known"
	end
	return obj_expand_mode
end

local function introspect_metatable(object,indent,name,known,ancestors)
	if not M.debug then return "" end
	if name == nil then name = "(unknown)" end
	local metatable = getmetatable(object);
	if metatable ==  nil then return "" end
	local result = indent..name..".metatable"
	table.insert(ancestors,name)
	result = result..introspect_body(metatable,indent..indent_string,"metatable","metatable",known,ancestors)
	table.remove(ancestors);
	return result
end


introspect_body = function (object,indent,name,obj_expand_mode,known,ancestors)
	if name == nil then name = "(unknown)" end
	local result = ""
	if #indent > 10*#indent_string then
		return "max depth\n"
	end
	if obj_expand_mode == "nil" then
		return "\n"
	elseif obj_expand_mode == "number" then
		return " : "..object.."\n"
	elseif obj_expand_mode == "string" then
		return " : \""..object.."\"\n"
	elseif obj_expand_mode == "boolean" then
		return  " : "..tostring(object).."\n"
	elseif obj_expand_mode == "table" then
		known[object]= table.concat(ancestors,".").."."..name;
		if next(object) == nil then return " : empty\n" end
		result = " :\n"
		for k,v in pairs(object) do
			table.insert(ancestors,name)
			result = result..introspect_internal(v,indent..indent_string,k,known,ancestors)
			table.remove(ancestors);
		end
		result = result..introspect_metatable(object,indent..indent_string,name,known,ancestors)
		return result;
	elseif obj_expand_mode == "function" then
		known[object]= table.concat(ancestors,".").."."..name;
		return "\n"
	elseif obj_expand_mode == "thread" then
		known[object]= table.concat(ancestors,".").."."..name;
		return "\n"
	elseif obj_expand_mode == "userdata" then
		known[object]= table.concat(ancestors,".").."."..name;
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
		return result
	elseif obj_expand_mode == "metatable" then
		known[object]= table.concat(ancestors,".").."."..name
		if next(object) == nil then 
			return " : empty\n" 
		end
		result = " :\n"
		for k,v in pairs(object) do
			table.insert(ancestors,name)
			result = result..introspect_internal(v,indent..indent_string,k,known,ancestors)
			table.remove(ancestors);
		end
		return result;

	elseif obj_expand_mode == "known" then
		return ": "..known[object].."\n";
	end
	error("unknown type of object")

end

introspect_internal = function(object,indent,name,known,ancestors)
	local obj_expand_mode =  expand_mode(object,indent,name,known,ancestors);
	local result = header(object,indent,name,obj_expand_mode)
	return result..introspect_body(object,indent,name,obj_expand_mode,known,ancestors)
end



function M.dump(object,name) 
	return introspect_internal(object,"",name,{},{})
end

dt.debug = M
return M
