local page_name="/redmine/projects/darktable/wiki/LuaAPI"

local function get_node_with_link(node,name)
	if node:get_attribute("skipped") then return name end
	return "\""..name.."\":"..page_name.."#"..node:get_name(true)
end

para = function() return "\n\n" end
node_to_string = function(node,name) 
	if name then
		return get_node_with_link(node,name)
	else
		return get_node_with_link(node,node:get_name())
	end
end

code = function(text) 
	return "\n\n<pre><code class=\"lua\">"..text.."</code></pre>\n\n"
end

startlist = function() return "\n\n" end
endlist = function() return "" end
listel = function(text) return "\n* "..text end
emphasis = function(text) return "_"..text.."_" end


require "content"
doc = require "core"
table = require "table"
dump = require("darktable.debug").dump

local parse_doc_node

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


local function get_reported_type(node,simple)
	if not node:get_reported_type() then
		doc.debug_print(node)
		error("all types should have a reported type")
	end
	local rtype = tostring(node:get_reported_type())
	if rtype == "documentation node" then rtype = nil end
	if rtype == "dt_singleton" then rtype = nil end
	if( rtype and not simple and doc.get_attribute(node,"signature")) then
		rtype = rtype.."( "
		local sig = doc.get_attribute(node,"signature")
		for k,v in pairs(sig) do
      if(doc.get_attribute(v,"is_self")) then
        rtype = doc.get_short_name(v)..":"..rtype
      elseif(doc.get_attribute(v,"optional")) then
				rtype = rtype.."[ _"..doc.get_short_name(v).."_ : "..get_reported_type(v,true).."]"
        if next(sig,k) then
          rtype = rtype..", "
        end
			else
				rtype = rtype.."_"..doc.get_short_name(v).."_ : "..get_reported_type(v,true)
        if next(sig,k) then
          rtype = rtype..", "
        end
			end
		end
		rtype = rtype.." )"
	end
	if(not simple and doc.get_attribute(node,"ret_val")) then
		rtype = rtype.." : "..get_reported_type(doc.get_attribute(node,"ret_val",true))
	end
	return rtype
end

local function print_attributes(node)
	local concat=""
	local result = ""
	for k2,v2 in sorted_pairs(node._luadoc_attributes) do
		if not doc.get_attribute(doc.toplevel.attributes[k2],"internal_attr") then
			if(type(v2) == "boolean") then
				concat = concat..get_node_with_link(doc.toplevel.attributes[k2],k2).." "
			elseif type(v2) == "string" then
				result = result.."\t*"..get_node_with_link(doc.toplevel.attributes[k2],k2).." :* "..v2.."\n\n" 
			elseif type(v2) == "table" and v2._luadoc_type then
				result = result.."\t*"..get_node_with_link(doc.toplevel.attributes[k2],k2).." :* "..tostring(v2).."\n\n" 
			elseif type(v2) == "table" then
				result = result.."\t*"..get_node_with_link(doc.toplevel.attributes[k2],k2).." :*\n\n"
				for k,v in pairs(v2) do
					result = result.."* "..tostring(v).."\n"
				end
				result = result.."\n\n"
			elseif type(v2) == "number" then
				result = result.."\t*"..get_node_with_link(doc.toplevel.attributes[k2],k2).." :* "..tostring(v2).."\n\n" 
			else
				error("unhandle attribute type\n"..dump(v2,k2))
			end
		end
	end
	if concat ~="" then
		result = result.."\t*Attributes* : "..concat.."\n\n"
	end
	return result

end

local function print_content(node)
	local rtype = get_reported_type(node)
	local result = ""
	if  rtype then
		result = result .."\t*type* : "..rtype.."\n\n"
	end
	result = result ..doc.get_text(node).."\n"
	result = result ..print_attributes(node)
	result = result.."\n"
	local sig = doc.get_attribute(node,"signature")
	local ret_val = doc.get_attribute(node,"ret_val")
	if(sig) then
		for k,v in pairs(sig) do
			result = result .. parse_doc_node(v,node,doc.get_short_name(v)).."\n";
		end
		result = result.."\n"
	end
	if(ret_val) then
		result = result .. parse_doc_node(ret_val,node,doc.get_short_name(ret_val)).."\n";
		result = result.."\n"
	end

	for k,v in doc.unskipped_children(node) do
		result = result .. parse_doc_node(v,node,k).."\n";
	end
	return result;
end

local function depth(node)
	if doc.get_name(node,true) == "" then return 0 end
	return depth(doc.get_main_parent(node)) +1
end

parse_doc_node = function(node,parent,prev_name)
	local node_name
	local parent_name = doc.get_name(parent)
	local result = ""
	if doc.is_main_parent(node,parent,prev_name) then
		node_name = doc.get_name(node)
	else
		if parent_name == "" then
			node_name = prev_name
		else 
			node_name =  doc.get_name(parent).."."..prev_name
		end
	end

	local depth = depth(node);
	if depth > 6 then depth = 6 end

	if(node._luadoc_type == "param") then
		local tmp_node = doc.get_main_parent(node)
		local tmp_string = "p"
		while tmp_node:get_reported_type() == "function" do
			tmp_string = tmp_string.."("
			tmp_node = doc.get_main_parent(tmp_node)
		end
		if(node:get_short_name() == "return") then
			result = result .. tmp_string.."(#"..node_name.."). _return_\n\n"
		else
			result = result .. tmp_string.."(#"..node_name.."). *"..node:get_short_name().."*\n\n"
		end
	elseif depth ~= 0 then
		result = result .. "h"..depth.."(#"..node_name.."). "..prev_name.."\n\n"
	end
	if(not doc.is_main_parent(node,parent,prev_name) ) then
		result = result .. "see "..get_node_with_link(node,doc.get_name(node,true)).."\n\n"
	else
		result = result .. print_content(node,parent)
	end
	return result;
end


M = {}

M.page_name = page_name


function M.get_doc()
	doc.toplevel:set_text(
	[[This documentation is for the *development* version of darktable. for the stable version, please visit "the user manual":https://www.darktable.org/usermanual/index.html.php
	]]..doc.get_text(doc.toplevel)..
	[[



	This documentation was generated with darktable version ]]..real_darktable.configuration.version..[[.]])
	return "{{>toc}}\n\n"..parse_doc_node(doc.toplevel,nil,"")
end



return M;
--
-- vim: shiftwidth=2 expandtab tabstop=2 cindent syntax=lua
