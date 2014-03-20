local table = require "table"
local dt = require "darktable"
local debug = require "debug"
local dump = require("darktable.debug").dump
local M={}
local meta_node = {__index = {}}

----------------------------------------
--  INTERNAL : PARSING OF SOURCE TREE --
----------------------------------------
local create_documentation_node
local toplevel = {}

local function set_attribute(node,attribute,value)
	node._luadoc_attributes[attribute] = value
	if not toplevel.attributes then return end
	if(not toplevel.attributes[attribute]) then
		toplevel.attributes[attribute] = true --avoid infinite recursion in the next line
		toplevel.attributes[attribute] = create_documentation_node(nil,toplevel.attributes,attribute)
	end
end

local function is_value(node)
	local obj_type = type(node)
	if obj_type == "nil" then return true end -- caught earlier, allows to use the function elsewhere
	if obj_type == "number" then return true end
	if obj_type == "string" then return true end
	if obj_type == "thread" then return true end
	if obj_type == "boolean" then return true end
	return false
end
local nojoin = {}
local known = {}
local function create_empty_node(node,node_type,parent,prev_name)
	local result = {}
	setmetatable(result,meta_node)
	result._luadoc_type = node_type
	result._luadoc_order = {}
	result._luadoc_attributes = {}
	result._luadoc_version = {}
	if parent and not parent._luadoc_type then 
		error("parent should be a doc node, not a real node")
	end
	if parent then
		result._luadoc_parents = {{ parent , prev_name }}
		result._luadoc_orig_parent = { parent , prev_name }
	else
		result._luadoc_parents = {{ nil , "" }}
		result._luadoc_orig_parent = { nil , "" }
	end
	if not is_value(node) and not nojoin[node] then
		known[node] = result
	end
	return result
end


local function is_type(node)
	has_entry,entry = pcall(function() return node.__luaA_Type end)
	return has_entry and entry and not node.__singleton
end

local function document_unknown(node,parent,prev_name)
	local result = create_empty_node(node,"undocumented",parent,prev_name)
	set_attribute(result,"reported_type","undocumented")
	return result
end

local function document_type_sub(node,result,parent,prev_name)
	for field,value in pairs(node) do
		if field == "__eq" then
			set_attribute(result,"has_equal",true)
		elseif field == "__pairs" then
			set_attribute(result,"has_pairs",true)
		elseif field == "__ipairs" then
			set_attribute(result,"has_ipairs",true)
		elseif field == "__len" then
			set_attribute(result,"has_length",true)
		elseif field == "__tostring" then
			set_attribute(result,"has_tostring",true)
		elseif field == "__singleton" then
			set_attribute(result,"is_singleton",true)
		elseif field == "__number_index" then
			nojoin[value] = true
			result["#"] = document_unknown(value,result,"#")
			set_attribute(result["#"],"read",true)
			set_attribute(result["#"],"is_attribute",true)
		elseif field == "__number_newindex" then
			nojoin[value] = true
			if not result["#"] then
				result["#"] = document_unknown(value,result,"#")
			end
			set_attribute(result["#"],"write",true)
			set_attribute(result["#"],"is_attribute",true)
		elseif field == "__get" then
			for k,v in pairs(node.__get) do
				nojoin[v] = true
				result[k] = document_unknown(v,result,k)
				set_attribute(result[k],"read",true)
				set_attribute(result[k],"is_attribute",true)
			end
			for k,v in pairs(node.__set) do
				nojoin[v] = true
				if not result[k] then
					result[k] = document_unknown(v,result,k)
				end
				set_attribute(result[k],"write",true)
				set_attribute(result[k],"is_attribute",true)
			end
		elseif field == "__luaA_ParentMetatable" then
				local type_node = create_documentation_node(value,toplevel.types,value.__luaA_TypeName);
				set_attribute(result,"parent",type_node)
		elseif (field == "__index"
			or field == "__newindex"
			or field == "__luaA_TypeName"
			or field == "__luaA_Type"
			or field == "__inext"
			or field == "__set"
			or field == "__next"
			or field == "__module_type"
			or field == "__associated_object"
			or field == "__gc"
			or field == "__values"
			or field == "__init"
			)	then
			-- nothing
		else
			print("ERROR undocumented metafield "..field.." for type "..prev_name)
		end
	end
	set_attribute(result,"reported_type","dt_type")
	return result
end

local function document_type_from_obj(obj,type_doc)
	if(type_doc._luadoc_in_obj_rec) then return end
	type_doc._luadoc_in_obj_rec = true
	for k,v in pairs(obj) do
		if type_doc[k] and M.get_attribute(type_doc[k],"reported_type")== "undocumented" then
			M.remove_parent(type_doc[k],type_doc)
			type_doc[k] = create_documentation_node(v,type_doc,k)
		elseif type(k) == "number" and M.get_attribute(type_doc["#"],"reported_type")== "undocumented" then
			M.remove_parent(type_doc["#"],type_doc)
			type_doc["#"] = create_documentation_node(v,type_doc,"#")
		end
	end
	type_doc._luadoc_in_obj_rec = false
	if M.get_attribute(type_doc,"parent") then
		document_type_from_obj(obj, M.get_attribute(type_doc,"parent"))
	end
end
M.document_type_from_obj = document_type_from_obj

local function document_type(node,parent,prev_name)
	local result = create_empty_node(node,"dt_type",parent,prev_name);
	document_type_sub(node,result,parent,prev_name)
	return result
end

local function is_known(node)
	if nojoin[node] then return false end
	return known[node] ~= nil
end
local function document_known(node,parent,prev_name)
	table.insert(known[node]._luadoc_parents,{parent , prev_name})
	return known[node]
end

local function is_dt_singleton(node)
	local obj_type = type(node)
	if obj_type ~= "userdata" then return false end
	local mt = getmetatable(node)
	return mt and mt.__singleton
end
local function document_dt_singleton(node,parent,prev_name)
	if(is_known(node)) then
		local result = document_known(node,parent,prev_name);
		local cur_parent = parent;
		while cur_parent do
			if is_type(cur_parent) then return result end
			cur_parent = M.get_main_parent(cur_parent)
		end
		result:set_main_parent(parent)
		return result
	end
	local result = create_empty_node(node,"dt_singleton",parent,prev_name);
	local mt = getmetatable(node)
	document_type_sub(mt,result,parent,prev_name)
	document_type_from_obj(node,result)
	set_attribute(result,"reported_type","dt_singleton")
	return result
end

local function is_dt_userdata(node)
	local obj_type = type(node)
	if obj_type ~= "userdata" then return false end
	local mt = getmetatable(node)
	return is_type(mt)
end
local function document_dt_userdata(node,parent,prev_name)
	local result = create_empty_node(node,"dt_userdata",parent,prev_name);
	local mt = getmetatable(node)
	local ret_node = create_documentation_node(mt,result,"reported_type")
	set_attribute(result,"reported_type",tostring(ret_node))
	M.remove_parent(ret_node,result)
	document_type_from_obj(node,ret_node)
	return result
end

local function is_function(node)
	local obj_type = type(node)
	if obj_type == "function" then return true end
	return false
end
local function document_function(node,parent,prev_name)
	local result = create_empty_node(node,"function",parent,prev_name);
	set_attribute(result,"signature",{})
	set_attribute(result,"reported_type","function")
	return result
end

local function is_table(node)
	local obj_type = type(node)
	if obj_type == "table" then return true end
	return false
end
local function document_table(node,parent,prev_name)
	local result = create_empty_node(node,"table",parent,prev_name);
	for k,v in pairs(node) do
		if not result[k] then
			result[k] = create_documentation_node(v,result,k)
		end
	end
	set_attribute(result,"reported_type",type(node))
	return result
end

local function document_value(node,parent,prev_name)
	local result = create_empty_node(node,type(node),parent,prev_name);
	set_attribute(result,"reported_type",type(node))
	return result
end

local function is_nil(node)
	return node == nil
end
local function document_nil(node,parent,prev_name)
	local result = create_empty_node(node,"documentation node",parent,prev_name)
	set_attribute(result,"reported_type","documentation node")
	return result
end

local function document_event(node,parent,prev_name)
	local result = create_empty_node(node,"event",parent,prev_name);
	result.callback = document_function(nil,result,"callback");
	result.callback:set_text("")
	result.extra_registration_parameters = create_documentation_node(nil,result,"extra_registration_parameters");
	result.extra_registration_parameters:set_real_name("extra registration parameters")
	set_attribute(result,"reported_type","event")
	return result
end

local type_matcher ={
	{is_nil , document_nil},
	{is_value , document_value},
	{is_dt_singleton , document_dt_singleton},
	{is_known , document_known},
	{is_type , document_type},
	{is_table , document_table},
	{is_function , document_function},
	{is_dt_userdata , document_dt_userdata},
}

create_documentation_node = function(node,parent,prev_name)
	for _,v in pairs(type_matcher) do
		if v[1](node) then return v[2](node,parent,prev_name)  end
	end
	return create_empty_node(node,"undocumented node type "..type(node),parent,prev_name)
end



----------------------------------------
--  HELPERS             --
----------------------------------------
function M.debug_print(node,name) 
	if not name then name = "<undocumented>" end
	print(name.." : "..node._luadoc_type);
	for k,v in pairs(node) do
		if(k == "_luadoc_attributes") then
			local concat=""
			for k2,v2 in pairs(v) do
				concat = concat..k2.."("..tostring(v2)..") "
			end
			print("\t"..k.." : "..concat)
		elseif(k == "_luadoc_orig_parent") then
			print("\t"..k.." : "..M.get_orig_name(node))
		elseif(k == "_luadoc_parents") then
			local concat=""
			for k2,v2 in pairs(v) do
				concat = concat..M.get_name(v2[1]).."."..v2[2].." "
			end
			print("\t"..k.." : "..concat)
		elseif(k == "_luadoc_order" and node._luadoc_order_first_key) then
			local concat=node._luadoc_order_first_key
			local key = node._luadoc_order_first_key
			while key do
				key = node._luadoc_order[key]
				if key then
					concat = concat.." => "..key
				end
			end
			print("\t"..k.." : "..concat)
		else
			print("\t"..k.." : "..tostring(v))
		end

	end
end


local function get_ancestor_tree(node)
	if not node then return nil,nil end
	if node._luadoc_in_rec then return nil,nil end
	node._luadoc_in_rec = true
	local best_tree = nil
	local best_depth = nil
	if(node._luadoc_main_parent) then
		for k,v in pairs(node._luadoc_parents) do
			if v[1] == node._luadoc_main_parent then
				local ptree,pdepth = get_ancestor_tree(v[1])
				node._luadoc_in_rec = nil
				return {k,ptree},pdepth
			end
		end
	end
	for k,v in pairs(node._luadoc_parents) do
		if not v[1] then
			best_depth = 0
			best_tree = nil
		else
			local ptree,pdepth = get_ancestor_tree(v[1])
			if not best_depth and pdepth then 
				best_depth = pdepth 
				best_tree = {k,ptree}
			elseif pdepth and pdepth < best_depth then
				best_depth = pdepth 
				best_tree = {k,ptree}
			end
		end
	end
	node._luadoc_in_rec = nil
	return best_tree,best_depth
end

	
function M.set_main_parent(node,parent)
	node._luadoc_main_parent = parent
end

function M.get_main_parent(node)
	local ancestor_tree,depth = get_ancestor_tree(node)
	if not ancestor_tree then return end
	return node._luadoc_parents[ancestor_tree[1]][1],node._luadoc_parents[ancestor_tree[1]][2]
end

function M.is_main_parent(node,parent,parent_name)
	local ancestor_tree,depth = get_ancestor_tree(node)
	if not parent  then return not ancestor_tree end -- no parent is valid if node has no ancestor

	return parent == node._luadoc_parents[ancestor_tree[1]][1] and parent_name == node._luadoc_parents[ancestor_tree[1]][2]
end

function M.all_children(node)
	return function(table,key)
		local nk,nv
		if key == nil and node._luadoc_order_first_key then
			nk = node._luadoc_order_first_key
			nv = table[nk]
			return nk,nv
		end
		if table._luadoc_order[key] then 
			nk = table._luadoc_order[key]
			nv = table[nk]
			return nk,nv
		end
		local nk,nv
		if table._luadoc_order[key] == false then 
			nk,nv = next(table)
		else
			nk,nv = next(table,key)
		end
		while 
			( type(nk) == "string" and nk:sub(1,#"_luadoc_") == "_luadoc_")
			or table._luadoc_order[nk] ~= nil 
			do
			nk,nv = next(table,nk)
		end
		return nk,nv
	end,node,nil
end

function M.unskiped_children(node)
	local my_all_children = M.all_children(node)
	return function(table,key)
		local nk,nv = key,nil
		while(true) do
			nk,nv = my_all_children(table,nk)
			if not nk then return nil, nil end
			if not M.get_attribute(nv,"skiped") then return nk,nv end 
		end
	end,node,nil
end

function M.remove_parent(node,parent)
	for k,v in ipairs(node._luadoc_parents) do
		if v[1] == parent then
			table.remove(node._luadoc_parents,k)
		end
	end
end

function M.set_real_name(node,name)
	node._luadoc_name = name
end

local function set_forced_next(node,son_name)
	if not node or not node[son_name] then
		return 
	end
	if node._luadoc_order[son_name] then
		return 
	end
	for k,v in pairs(node._luadoc_order) do
		if v == false then
			node._luadoc_order[k] = son_name
		end
	end
	node._luadoc_order[son_name] = false
	if not node._luadoc_order_first_key then
		node._luadoc_order_first_key = son_name
	end
end


function M.set_text(node,text)
	node._luadoc_text = text
	for k,v in ipairs(node._luadoc_parents) do
		set_forced_next(v[1],v[2])
	end
	return node

end


function M.add_version_info(node,version,text)
	if not text then -- only two parameters
		text = version
		version ="undocumented_version" -- easy grep for the "undocumented" keyword"
	end
	if not node._luadoc_version[version] then
		node._luadoc_version[version] = {}
	end
	table.insert(node._luadoc_version[version],text);
end
function M.get_version_info(node)
	return node._luadoc_version
end

function M.set_alias(original,node)
	for k,v in ipairs(node._luadoc_parents) do
		v[1][v[2]] = original
		table.insert(original._luadoc_parents,{v[1],v[2]})
		table.remove(node._luadoc_parents,k)
		set_forced_next(v[1],v[2])
	end
end


local function get_name_sub(node,ancestors)
	if not node then return "" end
	if not ancestors then return "" end -- our node is the toplevel node

	local subname = get_name_sub(node._luadoc_parents[ancestors[1]][1],ancestors[2])
	local prev_name = node._luadoc_parents[ancestors[1]][2]

	if subname == "" then
		return prev_name
	else
		return subname.."."..prev_name
	end
end

function M.get_short_name(node)
	if node._luadoc_name then
		return  node._luadoc_name
	end
	local ancestors = get_ancestor_tree(node)
	return node._luadoc_parents[ancestors[1]][2]
end

function M.get_name(node)
	local ancestors = get_ancestor_tree(node)
	return get_name_sub(node,ancestors)
end


function M.get_text(node)
	if node._luadoc_text then
		return node._luadoc_text
	else
		return "undocumented "..M.get_name(node)
	end
end


function M.get_orig_name(node) 
	if not node then return "" end
	local parent_info = node._luadoc_orig_parent
	if not parent_info[1] then
		return "<top>"
	end
	return M.get_orig_name(parent_info[1]).."."..parent_info[2]
end

function M.get_attribute(node,attribute)
	return node._luadoc_attributes[attribute]
end

function M.add_parameter(node,param_name,param_type,text)
	local subnode = create_empty_node(nil,"param",node,param_name)
	set_attribute(subnode,"reported_type",param_type)
	--M.set_real_name(subnode,param_name)
	if M.get_attribute(node,"reported_type") ~= "function" and 
		M.get_attribute(node,"reported_type") ~= "documentation node" then
		error("not a function documentation : ".. M.get_attribute(node,"reported_type"))
	end
	local signature = M.get_attribute(node,"signature")
	if not signature then
		signature = {}
	end
	table.insert(signature,subnode)
	set_attribute(node,"signature",signature)
	if text then
		M.set_text(subnode,text)
	end
	return subnode
end

function M.add_return(node,param_type,text)
	local subnode = create_empty_node(nil,"param",node,"return")
	set_attribute(subnode,"reported_type",param_type)
	if M.get_attribute(node,"reported_type") ~= "function" then
		error("not a function documentation")
	end
	--M.set_real_name(subnode,"return")
	set_attribute(node,"ret_val",subnode)
	if text then
		M.set_text(subnode,text)
	end
	return subnode
end

function M.set_skiped(node)
	set_attribute(node,"skiped",true)
end

meta_node.__index.set_text = M.set_text
meta_node.__index.add_parameter = M.add_parameter
meta_node.__index.add_return = M.add_return
meta_node.__index.set_real_name = M.set_real_name
meta_node.__index.all_children = M.all_children
meta_node.__index.unskiped_children = M.unskiped_children
meta_node.__index.set_attribute = set_attribute
meta_node.__index.get_attribute = M.get_attribute
meta_node.__index.set_alias = M.set_alias
meta_node.__index.get_short_name = M.get_short_name
meta_node.__index.set_main_parent = M.set_main_parent
meta_node.__index.remove_parent = M.remove_parent
meta_node.__index.debug_print = M.debug_print
meta_node.__index.set_skiped = M.set_skiped
meta_node.__index.add_version_info = M.add_version_info
meta_node.__index.get_version_info = M.get_version_info
meta_node.__index.get_name = M.get_name
meta_node.__tostring = function(node)
	return node_to_string(node)
end
meta_node.__lt = function(a,b)
	return tostring(a) < tostring(b)
end

--------------------------
-- GENERATE DOCUMENTATION
--------------------------
toplevel = create_documentation_node()
toplevel.attributes = create_documentation_node(nil,toplevel,"attributes")

toplevel.types = create_documentation_node(nil,toplevel,"types")
for k,v in pairs(debug.getregistry()) do
	if is_type(v) then
		toplevel.types[k] = create_documentation_node(v,toplevel.types,k);
	end
end

toplevel.darktable = create_documentation_node(nil,toplevel,"darktable")
for k,v in pairs(dt) do
	toplevel.darktable[k] = create_documentation_node(v,toplevel.darktable,k);
end

toplevel.events = create_documentation_node(nil,toplevel,"events")
for k,v in pairs(debug.getregistry().dt_lua_event_list) do
	toplevel.events[k] = document_event(v,toplevel.events,k);
	set_attribute(toplevel.events[k],"reported_type","event")
end

M.toplevel = toplevel
M.create_documentation_node = create_documentation_node
M.document_function = document_function
return M;
