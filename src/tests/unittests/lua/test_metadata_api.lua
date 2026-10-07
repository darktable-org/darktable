-- test for the per-image script storage in the darktable Lua API:
--   darktable.metadata.register(script, key [, options])
--   image:get_metadata(script, key)
--   image:set_metadata(script, key, value)
--
-- usage: copy to ~/.config/darktable/lua/ and add require("test_metadata_api")
-- to luarc, then start darktable with -d lua on a throwaway library holding at
-- least two images; the test writes values on them and restores them afterwards
-- check terminal output for PASS/FAIL

local dt = require "darktable"

local SCRIPT = "test_metadata_api"
local PREFIX = "Xmp.darktable.lua_" .. SCRIPT .. "_"

local pass = 0
local fail = 0

local function check(name, condition)
  if condition then
    pass = pass + 1
  else
    fail = fail + 1
    dt.print_error("FAIL: " .. name)
  end
end

-- true when calling f(...) raises an error
local function raises(f, ...)
  local ok = pcall(f, ...)
  return not ok
end

local function key_exists(key)
  return dt.metadata.exists(PREFIX .. key)
end


-- register --------------------------------------------------------------

local function test_register_creates_prefixed_keys()
  dt.metadata.register(SCRIPT, "state")
  check("hidden key is created under the prefix", key_exists("state"))

  dt.metadata.register(SCRIPT, "state")
  check("registering the same key twice is harmless", key_exists("state"))

  dt.metadata.register(SCRIPT, "score", { visible = true, private = false })
  check("public key is created under the prefix", key_exists("score"))
end

local function test_register_rejects_bad_names()
  check("dash in script name is rejected",
        raises(dt.metadata.register, "my-script", "x"))
  check("dot in key is rejected",
        raises(dt.metadata.register, SCRIPT, "a.b"))
  check("empty script name is rejected",
        raises(dt.metadata.register, "", "x"))
  check("rejected names create no key",
        not dt.metadata.exists("Xmp.darktable.lua__x"))
  check("options must be a table",
        raises(dt.metadata.register, SCRIPT, "opt", "yes"))
end


-- get / set -------------------------------------------------------------

local function test_values_are_stored_per_image(a, b)
  a:set_metadata(SCRIPT, "state", "")
  check("cleared key reads as empty string",
        a:get_metadata(SCRIPT, "state") == "")

  local json = '{"n":1,"tags":["x","y"]}'
  a:set_metadata(SCRIPT, "state", json)
  check("value round-trips unchanged",
        a:get_metadata(SCRIPT, "state") == json)

  b:set_metadata(SCRIPT, "state", "other")
  check("image b has its own value",
        b:get_metadata(SCRIPT, "state") == "other")
  check("image a keeps its value",
        a:get_metadata(SCRIPT, "state") == json)

  a:set_metadata(SCRIPT, "state", "v2")
  check("overwrite replaces the value",
        a:get_metadata(SCRIPT, "state") == "v2")

  a:set_metadata(SCRIPT, "score", "42")
  check("second key on the same image is independent",
        a:get_metadata(SCRIPT, "state") == "v2")
  check("second key holds its own value",
        a:get_metadata(SCRIPT, "score") == "42")
end

local function test_unregistered_keys_raise(a)
  check("get on an unregistered key raises",
        raises(a.get_metadata, a, SCRIPT, "nope"))
  check("set on an unregistered key raises",
        raises(a.set_metadata, a, SCRIPT, "nope", "v"))
  check("failed calls create no key",
        not key_exists("nope"))
  check("set without a value raises",
        raises(a.set_metadata, a, SCRIPT, "state"))
end


-- run -------------------------------------------------------------------

test_register_creates_prefixed_keys()
test_register_rejects_bad_names()

local a = dt.database[1]
local b = dt.database[2]
if a == nil or b == nil then
  dt.print_error("metadata API test: needs two images in the library, skipping get/set")
else
  local saved = {
    a_state = a:get_metadata(SCRIPT, "state"),
    b_state = b:get_metadata(SCRIPT, "state"),
    a_score = a:get_metadata(SCRIPT, "score"),
  }

  test_values_are_stored_per_image(a, b)
  test_unregistered_keys_raise(a)

  a:set_metadata(SCRIPT, "state", saved.a_state)
  b:set_metadata(SCRIPT, "state", saved.b_state)
  a:set_metadata(SCRIPT, "score", saved.a_score)
end

-- report
local total = pass + fail
dt.print_log(string.format(
  "metadata API test: %d/%d passed, %d failed", pass, total, fail))
if fail == 0 then
  dt.print("metadata API test: all " .. total .. " tests passed")
else
  dt.print("metadata API test: " .. fail .. " FAILED out of " .. total)
end
