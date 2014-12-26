--- usage:
-- require = require"require".require
-- :o)

local error, ipairs, newproxy, type = error, ipairs, newproxy, type

local t_concat = table.concat

--- Helpers

local function _tostring(s) return ""..s end

local function checkstring(s)
    local success, res = pcall(_tostring, s)
    if success then return res end
    error("bad argument #1 to 'require' (string expected, got "..type(s)..")", 3)
end

--- for Lua 5.1

local package, p_loaded = package, package.loaded

local sentinel = newproxy and newproxy() or string.char(1,2,3,4,5)

local function require51 (name)
    name = checkstring(name)
    if p_loaded[name] == sentinel then
        error("loop or previous error loading module '"..name.."'", 2)
    end

    local module = p_loaded[name]
    if module then return module end

    local msg = {}
    local loader
    for _, searcher in ipairs(package.loaders) do
        loader = searcher(name)
        if type(loader) == "function" then break end
        if type(loader) == "string" then
            -- `loader` is actually an error message
            msg[#msg + 1] = loader
        end
        loader = nil
    end
    if loader == nil then
        error("module '" .. name .. "' not found: "..t_concat(msg), 2)
    end
    p_loaded[name] = sentinel
    local res = loader(name)
    if res ~= nil then
        module = res
    elseif p_loaded[name] == sentinel or not p_loaded[name] then
        module = true
    end

    p_loaded[name] = module
    return module
end

--- for Lua 5.2

local function require52 (name)
    name = checkstring(name)
    local module = p_loaded[name]
    if module then return module end

    local msg = {}
    local loader, param
    for _, searcher in ipairs(package.searchers) do
        loader, param = searcher(name)
        if type(loader) == "function" then break end
        if type(loader) == "string" then
            -- `loader` is actually an error message
            msg[#msg + 1] = loader
        end
        loader = nil
    end
    if loader == nil then
        error("module '" .. name .. "' not found: "..table.concat(msg), 2)
    end
    local res = loader(name, param)
    if res ~= nil then
        module = res
    elseif not p_loaded[name] then
        module = true
    end

    p_loaded[name] = module
    return module
end

local module = {
    VERSION = "0.1.7",
    require51 = require51,
    require52 = require52
}

if _VERSION == "Lua 5.1" then module.require = require51 end
if _VERSION == "Lua 5.2" then module.require = require52 end

--- rerequire :o)

for _, o in ipairs{
    {"rerequiredefault", require},
    {"rerequire", module.require},
    {"rerequire51", require51},
    {"rerequire52", require52}
} do
    local rereq, req = o[1], o[2]
    module[rereq] = function(name)
        p_loaded[name] = nil
        return req(name)
    end
end

return module