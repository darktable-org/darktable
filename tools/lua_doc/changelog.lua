

para = function() return "\n\n" end
node_to_string = function(node,name) 
	if name then
		return name
	else
		return node:get_name()
	end
end

code = function(text) 
	return text
end

startlist = function() return "" end
endlist = function() return "" end
listel = function(text) return "\n* "..text end


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




local function print_content(node,version_info)
	local result = ""
	local history = doc.get_version_info(node)
	if next(history) then
		for version, notes in pairs(history) do
			if not version_info[version] then 
				version_info[version] = {}
			end
			version_info[version][node] = notes
		end
		result = result.."\n"
	end
	for k,v in doc.unskiped_children(node) do
		print_content(v,version_info)
	end
	return result;
end


M = {}

function M.get_doc()
	local version_info = {}
	print_content(doc.toplevel,version_info)
	local result = "CHANGELOG for API "..real_darktable.configuration.api_version_string.."(darktable version "..real_darktable.configuration.version..")\n\n"
	for version, content in sorted_pairs(version_info) do
		result = result.."API "..version.."\n"
		for object,notes in sorted_pairs(content) do
			result = result.."\t"..node_to_string(object).." :\n"
			for _,note in ipairs(notes) do
				result = result.."\t\t* "..note.."\n"
			end
		end

	end
	return result
	
end



return M;
