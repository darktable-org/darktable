-- Autogeneration Script for lua_autoc headers.
-- Expect that results will need cleaning up. It isn't very accurate.

if #arg ~= 1 then
  print("Usage: lua autogen.lua <header.h>")
else

f = io.open(arg[1], "r")
text = f:read("*all")
io.close(f)

text = string.gsub(text, "\r", "")
text = string.gsub(text, "\n", "")

function findall(text, pattern)
  local matches = {}
  for word in string.gmatch(text, pattern) do
    table.insert(matches, word)
  end
  return matches
end
  
typedefs = findall(text, "typedef struct {.-} %w-;")
funcdefs = findall(text, "[%w%*]- [%w_]-%(.-%);")

print("")

for k,v in pairs(typedefs) do
  local _, _, members, typename = string.find(v, "typedef struct {(.-)} (%w-);")
  print(string.format("luaA_struct(%s);", typename))
  
  function string:split(sep)
    local sep, fields = sep or ":", {}
    local pattern = string.format("([^%s]+)", sep)
    self:gsub(pattern, function(c) fields[#fields+1] = c end)
    return fields
  end
  
  for _, mem in pairs(members:split(";")) do
    local meminfo = mem:split(" ")
    print(string.format("luaA_struct_member(%s, %s, %s);", typename, meminfo[2], meminfo[1]))
  end
  
  print("")
end

for k,v in pairs(funcdefs) do

  local _, _, typename, name, args = string.find(v, "([%w%*]-) ([%w_]-)%((.-)%);")
  
  local argtypes = {}
  for _, arg in pairs(args:split(",")) do
    local arginfo = arg:split(" ")
    table.insert(argtypes, arginfo[1])
  end
  
  if typename == "void" then
    fstring = string.format("luaA_function_void(%s, %i", name, #argtypes)
  else
    fstring = string.format("luaA_function(%s, %s, %i", name, typename, #argtypes)
  end
  for _, v in pairs(argtypes) do fstring = fstring .. string.format(", %s", v) end
  fstring = fstring .. ");"
  
  print(fstring)
  
end

end

