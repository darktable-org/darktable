local indent_string ="   "
local table = require "table"
local dt = require "darktable"
local debug = false
local introspect_internal

local function header(object,indent,name,obj_type)
	if name == nil then name = "" end
	return indent..name.." ("..obj_type..")"
end

local function introspect_type(object,indent,name,...)
	local obj_type = type(object)
	ancestors ={...}
	if ancestors[2] == dt.modules then return "constructor" end
	if object == dt.styles.members then return "constructor" end
	if object == dt.images then return "constructor" end
	if object == dt.gui.selection then return "constructor" end
	return obj_type
end

function introspect_metatable(object,indent,name,obj_type,...)
	if not debug then return "" end
	if name == nil then name = "(unknown)" end
	local metatable = getmetatable(object);
	if metatable ==  nil then return indent..name..".metatable :  none\n" end
	local result = indent..name..".metatable"
	result = result..introspect_body(metatable,indent..indent_string,"metatable","metatable",object,...)
	return result
end


introspect_body = function (object,indent,name,obj_type,...)
	local result = ""
	if obj_type == "nil" then
		return "\n"
	elseif obj_type == "number" then
		return " : "..object.."\n"
	elseif obj_type == "string" then
		return " : \""..object.."\"\n"
	elseif obj_type == "boolean" then
		return  " : "..tostring(object).."\n"
	elseif obj_type == "table" then
		if next(object) == nil then return " : empty\n" end
		result = " :\n"
		for k,v in pairs(object) do
			result = result..introspect_internal(v,indent..indent_string,k,object,...)
		end
		result = result..introspect_metatable(object,indent..indent_string,name,...)
		return result;
	elseif obj_type == "function" then
		return "\n"
	elseif obj_type == "thread" then
		return "\n"
	elseif obj_type == "userdata" then
		local metatable = getmetatable(object);
		if metatable and metatable.__tostring then 
			result = result.. " : "..tostring(object).."\n"
		else
			result = result.. " :\n"
		end
		if metatable and metatable.__pairs then 
			for k,v in pairs(object) do
				result = result..introspect_internal(v,indent..indent_string,k,object,...)
			end
		end
		result = result..introspect_metatable(object,indent..indent_string,name,...)
		return result
	elseif obj_type == "metatable" then
		if next(object) == nil then return " : empty\n" end
		result = " :\n"
		for k,v in pairs(object) do
			result = result..introspect_internal(v,indent..indent_string,k,object,...)
		end
		return result;


		
	elseif obj_type == "constructor" then
		local son = object()
		local son_type = introspect_type(son,indent,name,object,...)
		result = " : "..header(son,"","object",son_type)
		result = result..introspect_body(son,indent..indent_string,nil,son_type,object,...)
		return result;
	end

end

introspect_internal = function(object,indent,name,...)
	local obj_type =  introspect_type(object,indent,name,...);
	local result = header(object,indent,name,obj_type)
	return result..introspect_body(object,indent,name,obj_type,...)
end



function introspect(object,name,param_debug) 
	debug = param_debug
	print(introspect_internal(object,"",name))
end
