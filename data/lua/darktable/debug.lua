local indent_string ="   "
local table = require "table"
local dt = require "darktable"
local debug = require "debug"
local introspect_internal
local introspect_body
local M = {debug = dt.configuration.verbose}
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

local function expand_mode(object,indent,name,...)
	local obj_expand_mode = type(object)
	local ancestors ={...}
	--if ancestors[2] == dt.modules then return "constructor" end
	--if object == dt.styles.members then return "constructor" end
	if object == dt.images then return "constructor" end
	if ancestors[2] == dt.images and next(dt.images()) ~= name then return "skipped" end
	--if object == dt.gui.selection then return "constructor" end
	if get_userdata_type(ancestors[1]) == "dt_image_t" and name == "get_history" then return "member constructor" end
	for _,v in pairs(ancestors) do
		if v == object and get_userdata_type(v) == get_userdata_type(object) then
			return "recursion"
		end
	end

	return obj_expand_mode
end

local function introspect_metatable(object,indent,name,obj_expand_mode,...)
	if not M.debug then return "" end
	if name == nil then name = "(unknown)" end
	local metatable = getmetatable(object);
	if metatable ==  nil then return indent..name..".metatable :  none\n" end
	local result = indent..name..".metatable"
	result = result..introspect_body(metatable,indent..indent_string,"metatable","metatable",object,...)
	return result
end


introspect_body = function (object,indent,name,obj_expand_mode,...)
	local result = ""
	local ancestors ={...}
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
		if next(object) == nil then return " : empty\n" end
		result = " :\n"
		for k,v in pairs(object) do
			result = result..introspect_internal(v,indent..indent_string,k,object,...)
		end
		result = result..introspect_metatable(object,indent..indent_string,name,...)
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
				result = result..introspect_internal(v,indent..indent_string,k,object,...)
			end
		end
		result = result..introspect_metatable(object,indent..indent_string,name,...)
		return result
	elseif obj_expand_mode == "metatable" then
		if next(object) == nil then return " : empty\n" end
		result = " :\n"
		for k,v in pairs(object) do
			result = result..introspect_internal(v,indent..indent_string,k,object,...)
		end
		return result;


		
	elseif obj_expand_mode == "constructor" then
		local son = object()
		local son_type = expand_mode(son,indent,name,object,...)
		result = " : "..header(son,"","object",son_type)
		result = result..introspect_body(son,indent..indent_string,nil,son_type,object,...)
		return result;
	elseif obj_expand_mode == "member constructor" then
		local son = object(ancestors[1])
		local son_type = expand_mode(son,indent,name,object,...)
		result = " : "..header(son,"","object",son_type)
		result = result..introspect_body(son,indent..indent_string,nil,son_type,object,...)
		return result;
	elseif obj_expand_mode == "skipped" then
		return ": skipped\n"
	elseif obj_expand_mode == "recursion" then
		return ": skipped (recursion)\n"
	end
	error("unknown type of object")

end

introspect_internal = function(object,indent,name,...)
	local obj_expand_mode =  expand_mode(object,indent,name,...);
	local result = header(object,indent,name,obj_expand_mode)
	return result..introspect_body(object,indent,name,obj_expand_mode,...)
end



function M.dump(object,name) 
	return introspect_internal(object,"",name)
end

dt.debug = M
return M
