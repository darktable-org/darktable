-- automated test for darktable.ai Lua API
-- exercises tensor operations without requiring AI models
-- usage: copy to ~/.config/darktable/lua/ and add require("test_ai_api") to luarc
-- check terminal output for PASS/FAIL

local dt = require "darktable"

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

local function approx(a, b, eps)
  return math.abs(a - b) < (eps or 0.001)
end

-- create_tensor
local t = dt.ai.create_tensor({1, 3, 4, 4})
check("create_tensor returns tensor", t ~= nil)

-- shape
local s = t:shape()
check("shape returns table", type(s) == "table")
check("shape[1] == 1", s[1] == 1)
check("shape[2] == 3", s[2] == 3)
check("shape[3] == 4", s[3] == 4)
check("shape[4] == 4", s[4] == 4)

-- ndim
check("ndim == 4", t:ndim() == 4)

-- size
check("size == 48", t:size() == 48)

-- get/set
t:set({0, 0, 0, 0}, 0.5)
check("set+get round-trip", t:get({0, 0, 0, 0}) == 0.5)

t:set({0, 2, 3, 3}, 0.75)
check("set+get corner", t:get({0, 2, 3, 3}) == 0.75)

-- zero-initialized
check("zero-initialized", t:get({0, 1, 1, 1}) == 0.0)

-- tostring
local str = tostring(t)
check("tostring contains shape", str:find("1x3x4x4") ~= nil)

-- crop
t:set({0, 0, 0, 0}, 1.0)
t:set({0, 0, 0, 1}, 2.0)
t:set({0, 0, 1, 0}, 3.0)
t:set({0, 0, 1, 1}, 4.0)
local c = t:crop(0, 0, 2, 2)
check("crop shape H", c:shape()[3] == 2)
check("crop shape W", c:shape()[4] == 2)
check("crop shape C", c:shape()[2] == 3)
check("crop value [0,0]", c:get({0, 0, 0, 0}) == 1.0)
check("crop value [0,1]", c:get({0, 0, 0, 1}) == 2.0)
check("crop value [1,0]", c:get({0, 0, 1, 0}) == 3.0)
check("crop value [1,1]", c:get({0, 0, 1, 1}) == 4.0)

-- paste
local dst = dt.ai.create_tensor({1, 1, 4, 4})
local src = dt.ai.create_tensor({1, 1, 2, 2})
src:set({0, 0, 0, 0}, 10.0)
src:set({0, 0, 0, 1}, 20.0)
src:set({0, 0, 1, 0}, 30.0)
src:set({0, 0, 1, 1}, 40.0)
dst:paste(src, 1, 1)
check("paste value [1,1]", dst:get({0, 0, 1, 1}) == 10.0)
check("paste value [1,2]", dst:get({0, 0, 1, 2}) == 20.0)
check("paste value [2,1]", dst:get({0, 0, 2, 1}) == 30.0)
check("paste value [2,2]", dst:get({0, 0, 2, 2}) == 40.0)
check("paste untouched [0,0]", dst:get({0, 0, 0, 0}) == 0.0)

-- dot product
local a = dt.ai.create_tensor({4})
local b = dt.ai.create_tensor({4})
a:set({0}, 1.0); a:set({1}, 2.0); a:set({2}, 3.0); a:set({3}, 4.0)
b:set({0}, 1.0); b:set({1}, 1.0); b:set({2}, 1.0); b:set({3}, 1.0)
check("dot product", a:dot(b) == 10.0)

-- dot product orthogonal
local c1 = dt.ai.create_tensor({2})
local c2 = dt.ai.create_tensor({2})
c1:set({0}, 1.0); c1:set({1}, 0.0)
c2:set({0}, 0.0); c2:set({1}, 1.0)
check("dot orthogonal == 0", c1:dot(c2) == 0.0)

-- linear_to_srgb / srgb_to_linear round-trip
local gamma = dt.ai.create_tensor({1, 1, 1, 3})
gamma:set({0, 0, 0, 0}, 0.0)
gamma:set({0, 0, 0, 1}, 0.5)
gamma:set({0, 0, 0, 2}, 1.0)
gamma:linear_to_srgb()
check("srgb(0.0) == 0.0", gamma:get({0, 0, 0, 0}) == 0.0)
check("srgb(0.5) > 0.5", gamma:get({0, 0, 0, 1}) > 0.5)
check("srgb(1.0) == 1.0", approx(gamma:get({0, 0, 0, 2}), 1.0))
gamma:srgb_to_linear()
check("round-trip 0.0", approx(gamma:get({0, 0, 0, 0}), 0.0))
check("round-trip 0.5", approx(gamma:get({0, 0, 0, 1}), 0.5))
check("round-trip 1.0", approx(gamma:get({0, 0, 0, 2}), 1.0))

-- wide gamut preservation
local wg = dt.ai.create_tensor({1})
wg:set({0}, 1.5)
wg:linear_to_srgb()
check("wide gamut > 1.0", wg:get({0}) > 1.0)
wg:srgb_to_linear()
check("wide gamut round-trip", approx(wg:get({0}), 1.5, 0.01))

-- fill
local f = dt.ai.create_tensor({2, 3})
f:fill(7.0)
check("fill [0,0]", f:get({0, 0}) == 7.0)
check("fill [1,2]", f:get({1, 2}) == 7.0)
check("fill returns self", f:fill(0.0) == f)

-- scale_add
local sa = dt.ai.create_tensor({4})
sa:set({0}, 1.0)
sa:set({1}, 2.0)
sa:set({2}, 3.0)
sa:set({3}, 4.0)
sa:scale_add(2.0, 1.0)  -- t = 2*t + 1
check("scale_add [0]", sa:get({0}) == 3.0)
check("scale_add [3]", sa:get({3}) == 9.0)
sa:scale_add(0.5)  -- offset defaults to 0
check("scale_add default offset [0]", sa:get({0}) == 1.5)

-- sum / mean
local r = dt.ai.create_tensor({4})
r:set({0}, 1.0); r:set({1}, 2.0); r:set({2}, 3.0); r:set({3}, 4.0)
check("sum == 10", r:sum() == 10.0)
check("mean == 2.5", r:mean() == 2.5)

-- bayer_pack / bayer_unpack round-trip
local cfa = dt.ai.create_tensor({1, 1, 4, 4})
for y = 0, 3 do
  for x = 0, 3 do
    cfa:set({0, 0, y, x}, y * 4 + x)
  end
end
local packed = cfa:bayer_pack()
check("packed shape C", packed:shape()[2] == 4)
check("packed shape H", packed:shape()[3] == 2)
check("packed shape W", packed:shape()[4] == 2)
-- ch 0 = (2y, 2x) → original (0,0)=0, (0,2)=2, (2,0)=8, (2,2)=10
check("packed ch0 [0,0]", packed:get({0, 0, 0, 0}) == 0)
check("packed ch0 [1,1]", packed:get({0, 0, 1, 1}) == 10)
-- ch 3 = (2y+1, 2x+1) → original (1,1)=5, (3,3)=15
check("packed ch3 [0,0]", packed:get({0, 3, 0, 0}) == 5)
check("packed ch3 [1,1]", packed:get({0, 3, 1, 1}) == 15)
local back = packed:bayer_unpack()
check("unpacked shape H", back:shape()[3] == 4)
check("unpacked shape W", back:shape()[4] == 4)
for y = 0, 3 do
  for x = 0, 3 do
    if back:get({0, 0, y, x}) ~= y * 4 + x then
      check("bayer round-trip ["..y..","..x.."]", false)
    end
  end
end
check("bayer round-trip all", true)

-- models() returns a table
local models = dt.ai.models()
check("models returns table", type(models) == "table")

-- model_for_task returns string or nil
local mid = dt.ai.model_for_task("nonexistent_task")
check("model_for_task unknown == nil", mid == nil)

-- report
local total = pass + fail
dt.print_log(string.format(
  "AI API test: %d/%d passed, %d failed", pass, total, fail))
if fail == 0 then
  dt.print("AI API test: all " .. total .. " tests passed")
else
  dt.print("AI API test: " .. fail .. " FAILED out of " .. total)
end
