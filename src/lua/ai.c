/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#if defined(USE_LUA) && defined(HAVE_AI)

#include "lua/ai.h"
#include "ai/backend.h"
#include "common/ai_models.h"
#include "common/colorspaces.h"
#include "common/darktable.h"
#include "common/exif.h"
#include "common/image_cache.h"
#include "common/mipmap_cache.h"
#include "develop/format.h"
#include "imageio/imageio_common.h"
#include "imageio/imageio_dng.h"
#include "imageio/imageio_module.h"
#include "lua/image.h"
#include "lua/types.h"

#include <lcms2.h>
#include <string.h>
#include <tiffio.h>

/* maximum tensor dimensions */
#define MAX_TENSOR_DIMS 8

/* --- tensor type --- */

typedef struct dt_lua_ai_tensor_t
{
  float *data;
  int64_t shape[MAX_TENSOR_DIMS];
  int ndim;
  size_t size; // total number of elements
} dt_lua_ai_tensor_t;

/* --- context type (wraps dt_ai_context_t *) --- */

typedef dt_ai_context_t *dt_lua_ai_context_t;

/* --- model metadata type --- */

typedef struct dt_lua_ai_model_t
{
  char id[64];
  char name[128];
  char description[256];
  char task[32];
  int status;
  gboolean is_default;
} dt_lua_ai_model_t;

/* ================================================================
 * darktable.ai — Lua API for AI model inference
 *
 * provides tensor creation/manipulation, model loading, inference,
 * and image I/O (file, pipeline export, raw CFA, DNG output)
 * ================================================================ */

/* ================================================================
 * tensor implementation
 * ================================================================ */

// GC: frees the heap-allocated float data
static int _tensor_gc(lua_State *L)
{
  dt_lua_ai_tensor_t *t = luaL_checkudata(L, 1, "dt_lua_ai_tensor_t");
  g_free(t->data);
  t->data = NULL;
  return 0;
}

// tostring(tensor) → "tensor(1x3x512x512)"
static int _tensor_tostring(lua_State *L)
{
  dt_lua_ai_tensor_t *t = luaL_checkudata(L, 1, "dt_lua_ai_tensor_t");
  char buf[256];
  int pos = snprintf(buf, sizeof(buf), "tensor(");
  for(int i = 0; i < t->ndim; i++)
    pos += snprintf(buf + pos, sizeof(buf) - pos,
                    "%s%" PRId64, i ? "x" : "", t->shape[i]);
  snprintf(buf + pos, sizeof(buf) - pos, ")");
  lua_pushstring(L, buf);
  return 1;
}

// tensor:get({i, j, ...}) → float
// read a value by multi-dimensional index (0-based)
static int _tensor_get(lua_State *L)
{
  dt_lua_ai_tensor_t *t = luaL_checkudata(L, 1, "dt_lua_ai_tensor_t");
  if(!t->data)
    return luaL_error(L, "tensor has been freed");

  // read index table from arg 2
  luaL_checktype(L, 2, LUA_TTABLE);
  const int nidx = lua_rawlen(L, 2);
  if(nidx != t->ndim)
    return luaL_error(L, "expected %d indices, got %d",
                      t->ndim, nidx);

  size_t offset = 0;
  size_t stride = t->size;
  for(int i = 0; i < t->ndim; i++)
  {
    lua_rawgeti(L, 2, i + 1);
    const int idx = lua_tointeger(L, -1);
    lua_pop(L, 1);
    if(idx < 0 || idx >= t->shape[i])
      return luaL_error(L, "index %d out of range [0, %" PRId64 ")",
                        idx, t->shape[i]);
    stride /= (size_t)t->shape[i];
    offset += (size_t)idx * stride;
  }

  lua_pushnumber(L, (double)t->data[offset]);
  return 1;
}

// tensor:set({i, j, ...}, value)
// write a float value by multi-dimensional index (0-based)
static int _tensor_set(lua_State *L)
{
  dt_lua_ai_tensor_t *t = luaL_checkudata(L, 1, "dt_lua_ai_tensor_t");
  if(!t->data)
    return luaL_error(L, "tensor has been freed");

  luaL_checktype(L, 2, LUA_TTABLE);
  const float val = luaL_checknumber(L, 3);
  const int nidx = lua_rawlen(L, 2);
  if(nidx != t->ndim)
    return luaL_error(L, "expected %d indices, got %d",
                      t->ndim, nidx);

  size_t offset = 0;
  size_t stride = t->size;
  for(int i = 0; i < t->ndim; i++)
  {
    lua_rawgeti(L, 2, i + 1);
    const int idx = lua_tointeger(L, -1);
    lua_pop(L, 1);
    if(idx < 0 || idx >= t->shape[i])
      return luaL_error(L, "index %d out of range [0, %" PRId64 ")",
                        idx, t->shape[i]);
    stride /= (size_t)t->shape[i];
    offset += (size_t)idx * stride;
  }

  t->data[offset] = val;
  return 0;
}

// tensor:crop(y, x, h, w)
// extract a sub-region from the last two dimensions (H, W) of an
// NCHW tensor. returns a new tensor [N, C, h, w]
static int _tensor_crop(lua_State *L)
{
  dt_lua_ai_tensor_t *t
    = luaL_checkudata(L, 1, "dt_lua_ai_tensor_t");
  if(!t->data)
    return luaL_error(L, "tensor has been freed");
  if(t->ndim < 2)
    return luaL_error(L, "crop requires at least 2 dimensions");

  const int y = luaL_checkinteger(L, 2);
  const int x = luaL_checkinteger(L, 3);
  const int h = luaL_checkinteger(L, 4);
  const int w = luaL_checkinteger(L, 5);

  const int H = (int)t->shape[t->ndim - 2];
  const int W = (int)t->shape[t->ndim - 1];

  if(y < 0 || x < 0 || h <= 0 || w <= 0
     || y + h > H || x + w > W)
    return luaL_error(L,
      "crop region (%d,%d,%d,%d) out of bounds (%d,%d)",
      y, x, h, w, H, W);

  // compute leading dimensions (everything before H,W)
  size_t leading = 1;
  for(int i = 0; i < t->ndim - 2; i++)
    leading *= (size_t)t->shape[i];

  const size_t total = leading * (size_t)h * w;
  dt_lua_ai_tensor_t *out
    = lua_newuserdata(L, sizeof(dt_lua_ai_tensor_t));
  memset(out, 0, sizeof(*out));
  out->data = g_try_malloc(total * sizeof(float));
  if(!out->data)
    return luaL_error(L, "failed to allocate crop tensor");

  // copy row by row for each leading slice
  for(size_t s = 0; s < leading; s++)
  {
    const float *src = t->data + s * (size_t)H * W;
    float *dst = out->data + s * (size_t)h * w;
    for(int row = 0; row < h; row++)
      memcpy(dst + (size_t)row * w,
             src + (size_t)(y + row) * W + x,
             (size_t)w * sizeof(float));
  }

  out->ndim = t->ndim;
  for(int i = 0; i < t->ndim - 2; i++)
    out->shape[i] = t->shape[i];
  out->shape[t->ndim - 2] = h;
  out->shape[t->ndim - 1] = w;
  out->size = total;
  luaL_setmetatable(L, "dt_lua_ai_tensor_t");
  return 1;
}

// tensor:paste(source, y, x)
// copy a source tensor into this tensor at position (y, x) in the
// last two dimensions. source must have matching leading dimensions
static int _tensor_paste(lua_State *L)
{
  dt_lua_ai_tensor_t *dst
    = luaL_checkudata(L, 1, "dt_lua_ai_tensor_t");
  dt_lua_ai_tensor_t *src
    = luaL_checkudata(L, 2, "dt_lua_ai_tensor_t");
  if(!dst->data || !src->data)
    return luaL_error(L, "tensor has been freed");
  if(dst->ndim != src->ndim)
    return luaL_error(L,
      "dimension mismatch: dst has %d dims, src has %d",
      dst->ndim, src->ndim);
  if(dst->ndim < 2)
    return luaL_error(L, "paste requires at least 2 dimensions");

  // verify leading dimensions match
  for(int i = 0; i < dst->ndim - 2; i++)
  {
    if(dst->shape[i] != src->shape[i])
      return luaL_error(L,
        "leading dimension %d mismatch: dst=%" PRId64
        ", src=%" PRId64, i, dst->shape[i], src->shape[i]);
  }

  const int y = luaL_checkinteger(L, 3);
  const int x = luaL_checkinteger(L, 4);

  const int dst_H = (int)dst->shape[dst->ndim - 2];
  const int dst_W = (int)dst->shape[dst->ndim - 1];
  const int src_h = (int)src->shape[src->ndim - 2];
  const int src_w = (int)src->shape[src->ndim - 1];

  if(y < 0 || x < 0
     || y + src_h > dst_H || x + src_w > dst_W)
    return luaL_error(L,
      "paste region (%d,%d,%d,%d) out of bounds (%d,%d)",
      y, x, src_h, src_w, dst_H, dst_W);

  // compute leading dimensions
  size_t leading = 1;
  for(int i = 0; i < dst->ndim - 2; i++)
    leading *= (size_t)dst->shape[i];

  // copy row by row for each leading slice
  for(size_t s = 0; s < leading; s++)
  {
    float *d = dst->data + s * (size_t)dst_H * dst_W;
    const float *sr = src->data + s * (size_t)src_h * src_w;
    for(int row = 0; row < src_h; row++)
      memcpy(d + (size_t)(y + row) * dst_W + x,
             sr + (size_t)row * src_w,
             (size_t)src_w * sizeof(float));
  }

  return 0;
}

static inline float _linear_to_srgb(const float v)
{
  if(v <= 0.0f) return 0.0f;
  return (v <= 0.0031308f)
    ? 12.92f * v
    : 1.055f * powf(v, 1.0f / 2.4f) - 0.055f;
}

static inline float _srgb_to_linear(const float v)
{
  if(v <= 0.0f) return 0.0f;
  return (v <= 0.04045f)
    ? v / 12.92f
    : powf((v + 0.055f) / 1.055f, 2.4f);
}

// tensor:linear_to_srgb()
// convert all values in-place from linear RGB to sRGB gamma.
// values > 1.0 are preserved (wide-gamut safe)
static int _tensor_linear_to_srgb(lua_State *L)
{
  dt_lua_ai_tensor_t *t
    = luaL_checkudata(L, 1, "dt_lua_ai_tensor_t");
  if(!t->data)
    return luaL_error(L, "tensor has been freed");
  for(size_t i = 0; i < t->size; i++)
    t->data[i] = _linear_to_srgb(t->data[i]);
  return 0;
}

// tensor:srgb_to_linear()
// convert all values in-place from sRGB gamma to linear RGB
static int _tensor_srgb_to_linear(lua_State *L)
{
  dt_lua_ai_tensor_t *t
    = luaL_checkudata(L, 1, "dt_lua_ai_tensor_t");
  if(!t->data)
    return luaL_error(L, "tensor has been freed");
  for(size_t i = 0; i < t->size; i++)
    t->data[i] = _srgb_to_linear(t->data[i]);
  return 0;
}

// tensor.shape → table of dimension sizes, e.g. {1, 3, 512, 512}
static int _tensor_shape(lua_State *L)
{
  dt_lua_ai_tensor_t *t
    = luaL_checkudata(L, 1, "dt_lua_ai_tensor_t");
  lua_newtable(L);
  for(int i = 0; i < t->ndim; i++)
  {
    lua_pushinteger(L, t->shape[i]);
    lua_rawseti(L, -2, i + 1);
  }
  return 1;
}

// tensor.ndim → number of dimensions
static int _tensor_ndim(lua_State *L)
{
  dt_lua_ai_tensor_t *t
    = luaL_checkudata(L, 1, "dt_lua_ai_tensor_t");
  lua_pushinteger(L, t->ndim);
  return 1;
}

// tensor.size → total number of elements
static int _tensor_size(lua_State *L)
{
  dt_lua_ai_tensor_t *t
    = luaL_checkudata(L, 1, "dt_lua_ai_tensor_t");
  lua_pushinteger(L, (lua_Integer)t->size);
  return 1;
}

// tensor:dot(other) → float
// compute dot product of two 1D tensors (or flattened).
// both tensors must have the same total number of elements
static int _tensor_dot(lua_State *L)
{
  dt_lua_ai_tensor_t *a
    = luaL_checkudata(L, 1, "dt_lua_ai_tensor_t");
  dt_lua_ai_tensor_t *b
    = luaL_checkudata(L, 2, "dt_lua_ai_tensor_t");
  if(!a->data || !b->data)
    return luaL_error(L, "tensor has been freed");
  if(a->size != b->size)
    return luaL_error(L,
      "dot product requires same size: %zu vs %zu",
      a->size, b->size);

  double sum = 0.0;
  for(size_t i = 0; i < a->size; i++)
    sum += (double)a->data[i] * (double)b->data[i];

  lua_pushnumber(L, sum);
  return 1;
}

// tensor:fill(value) → self
// fill all elements with a scalar value, in place
static int _tensor_fill(lua_State *L)
{
  dt_lua_ai_tensor_t *t
    = luaL_checkudata(L, 1, "dt_lua_ai_tensor_t");
  if(!t->data) return luaL_error(L, "tensor has been freed");
  const float v = (float)luaL_checknumber(L, 2);
  for(size_t i = 0; i < t->size; i++) t->data[i] = v;
  lua_settop(L, 1);  // return self for chaining
  return 1;
}

// tensor:scale_add(scale [, offset]) → self
// in-place: t[i] = t[i] * scale + offset (offset defaults to 0).
// scalar arithmetic primitive — the building block for normalize,
// black-level subtract, gain match, etc.
static int _tensor_scale_add(lua_State *L)
{
  dt_lua_ai_tensor_t *t
    = luaL_checkudata(L, 1, "dt_lua_ai_tensor_t");
  if(!t->data) return luaL_error(L, "tensor has been freed");
  const float s = (float)luaL_checknumber(L, 2);
  const float b = (float)luaL_optnumber(L, 3, 0.0);
  for(size_t i = 0; i < t->size; i++) t->data[i] = t->data[i] * s + b;
  lua_settop(L, 1);
  return 1;
}

// tensor:sum() → float
// sum of all elements (double accumulation, returned as Lua number)
static int _tensor_sum(lua_State *L)
{
  dt_lua_ai_tensor_t *t
    = luaL_checkudata(L, 1, "dt_lua_ai_tensor_t");
  if(!t->data) return luaL_error(L, "tensor has been freed");
  double s = 0.0;
  for(size_t i = 0; i < t->size; i++) s += t->data[i];
  lua_pushnumber(L, s);
  return 1;
}

// tensor:mean() → float
// mean of all elements
static int _tensor_mean(lua_State *L)
{
  dt_lua_ai_tensor_t *t
    = luaL_checkudata(L, 1, "dt_lua_ai_tensor_t");
  if(!t->data) return luaL_error(L, "tensor has been freed");
  if(t->size == 0) return luaL_error(L, "mean of empty tensor");
  double s = 0.0;
  for(size_t i = 0; i < t->size; i++) s += t->data[i];
  lua_pushnumber(L, s / (double)t->size);
  return 1;
}

// tensor:bayer_pack() → tensor
// reshape a [1,1,H,W] CFA into [1,4,H/2,W/2] with 4 spatial phases:
//   ch 0 = pixels at (2y,   2x)    (top-left of each 2×2 block)
//   ch 1 = pixels at (2y,   2x+1)
//   ch 2 = pixels at (2y+1, 2x)
//   ch 3 = pixels at (2y+1, 2x+1)  (bottom-right)
// channel-to-color mapping depends on the CFA filter pattern at the
// chosen origin — the caller is expected to know it (load_raw returns
// `filters` in the metadata table; index with the FC table from there).
// H and W must both be even.
static int _tensor_bayer_pack(lua_State *L)
{
  dt_lua_ai_tensor_t *t
    = luaL_checkudata(L, 1, "dt_lua_ai_tensor_t");
  if(!t->data) return luaL_error(L, "tensor has been freed");
  if(t->ndim != 4 || t->shape[0] != 1 || t->shape[1] != 1)
    return luaL_error(L, "bayer_pack requires [1,1,H,W] tensor");

  const int H = (int)t->shape[2];
  const int W = (int)t->shape[3];
  if((H & 1) || (W & 1))
    return luaL_error(L, "bayer_pack requires even H,W (got %dx%d)", H, W);

  const int H2 = H / 2;
  const int W2 = W / 2;
  const size_t plane = (size_t)H2 * W2;
  const size_t total = plane * 4;

  dt_lua_ai_tensor_t *out
    = lua_newuserdata(L, sizeof(dt_lua_ai_tensor_t));
  memset(out, 0, sizeof(*out));
  out->data = g_try_malloc(total * sizeof(float));
  if(!out->data) return luaL_error(L, "failed to allocate packed tensor");

  float *c0 = out->data + 0 * plane;
  float *c1 = out->data + 1 * plane;
  float *c2 = out->data + 2 * plane;
  float *c3 = out->data + 3 * plane;
  for(int y = 0; y < H2; y++)
  {
    const float *r0 = t->data + (size_t)(2 * y) * W;
    const float *r1 = r0 + W;
    for(int x = 0; x < W2; x++)
    {
      const size_t k = (size_t)y * W2 + x;
      c0[k] = r0[2 * x];
      c1[k] = r0[2 * x + 1];
      c2[k] = r1[2 * x];
      c3[k] = r1[2 * x + 1];
    }
  }

  out->ndim = 4;
  out->shape[0] = 1;
  out->shape[1] = 4;
  out->shape[2] = H2;
  out->shape[3] = W2;
  out->size = total;
  luaL_setmetatable(L, "dt_lua_ai_tensor_t");
  return 1;
}

// tensor:bayer_unpack() → tensor
// inverse of bayer_pack: [1,4,H,W] → [1,1,2H,2W], reassembling the 4
// phases into a full CFA. channel order must match the pack convention
// (ch 0 = (2y,2x), ch 1 = (2y,2x+1), ch 2 = (2y+1,2x), ch 3 = (2y+1,2x+1)).
static int _tensor_bayer_unpack(lua_State *L)
{
  dt_lua_ai_tensor_t *t
    = luaL_checkudata(L, 1, "dt_lua_ai_tensor_t");
  if(!t->data) return luaL_error(L, "tensor has been freed");
  if(t->ndim != 4 || t->shape[0] != 1 || t->shape[1] != 4)
    return luaL_error(L, "bayer_unpack requires [1,4,H,W] tensor");

  const int H2 = (int)t->shape[2];
  const int W2 = (int)t->shape[3];
  const int H = H2 * 2;
  const int W = W2 * 2;
  const size_t plane = (size_t)H2 * W2;
  const size_t total = (size_t)H * W;

  dt_lua_ai_tensor_t *out
    = lua_newuserdata(L, sizeof(dt_lua_ai_tensor_t));
  memset(out, 0, sizeof(*out));
  out->data = g_try_malloc(total * sizeof(float));
  if(!out->data) return luaL_error(L, "failed to allocate unpacked tensor");

  const float *c0 = t->data + 0 * plane;
  const float *c1 = t->data + 1 * plane;
  const float *c2 = t->data + 2 * plane;
  const float *c3 = t->data + 3 * plane;
  for(int y = 0; y < H2; y++)
  {
    float *r0 = out->data + (size_t)(2 * y) * W;
    float *r1 = r0 + W;
    for(int x = 0; x < W2; x++)
    {
      const size_t k = (size_t)y * W2 + x;
      r0[2 * x]     = c0[k];
      r0[2 * x + 1] = c1[k];
      r1[2 * x]     = c2[k];
      r1[2 * x + 1] = c3[k];
    }
  }

  out->ndim = 4;
  out->shape[0] = 1;
  out->shape[1] = 1;
  out->shape[2] = H;
  out->shape[3] = W;
  out->size = total;
  luaL_setmetatable(L, "dt_lua_ai_tensor_t");
  return 1;
}

/* ================================================================
 * context implementation
 * ================================================================ */

// helper: collect tensors from a Lua table into a dt_ai_tensor_t array
static int _collect_tensors(lua_State *L, const int tbl_idx,
                            dt_ai_tensor_t **out, const char *label)
{
  const int n = lua_rawlen(L, tbl_idx);
  if(n <= 0)
    return luaL_error(L, "%s table is empty", label);

  *out = g_new0(dt_ai_tensor_t, n);
  for(int i = 0; i < n; i++)
  {
    lua_rawgeti(L, tbl_idx, i + 1);
    dt_lua_ai_tensor_t *t = luaL_testudata(L, -1, "dt_lua_ai_tensor_t");
    lua_pop(L, 1);
    if(!t || !t->data)
    {
      g_free(*out);
      *out = NULL;
      return luaL_error(L, "%s[%d] is not a valid tensor",
                        label, i + 1);
    }
    (*out)[i].data = t->data;
    (*out)[i].type = DT_AI_FLOAT;
    (*out)[i].shape = t->shape;
    (*out)[i].ndim = t->ndim;
  }
  return n;
}

// ctx:run({inputs}, {outputs})  — pre-allocated outputs, writes in-place
// ctx:run(input1, input2, ...)  — auto-allocate, returns output tensors
static int _context_run(lua_State *L)
{
  dt_lua_ai_context_t *p = luaL_checkudata(L, 1, "dt_lua_ai_context_t");
  if(!p || !*p)
    return luaL_error(L, "model context is closed");
  dt_ai_context_t *ctx = *p;

  // detect calling convention: two tables = pre-allocated,
  // otherwise varargs = auto-allocate
  const gboolean two_table
    = (lua_gettop(L) == 3
       && lua_istable(L, 2) && lua_istable(L, 3));

  if(two_table)
  {
    // pre-allocated path: ctx:run({inputs}, {outputs})
    dt_ai_tensor_t *inputs = NULL;
    const int n_in = _collect_tensors(L, 2, &inputs, "input");

    dt_ai_tensor_t *outputs = NULL;
    const int n_out = _collect_tensors(L, 3, &outputs, "output");

    const int ret = dt_ai_run(ctx, inputs, n_in, outputs, n_out);
    g_free(inputs);
    g_free(outputs);

    if(ret != 0)
      return luaL_error(L, "inference failed (error %d)", ret);

    return 0; // outputs written in-place
  }

  // auto-allocate path: ctx:run(input1, input2, ...)
  const int n_in = lua_gettop(L) - 1;
  if(n_in < 1)
    return luaL_error(L,
      "run() requires input tensors or {inputs},{outputs} tables");

  dt_ai_tensor_t *inputs = g_new0(dt_ai_tensor_t, n_in);
  for(int i = 0; i < n_in; i++)
  {
    dt_lua_ai_tensor_t *t = luaL_testudata(L, i + 2, "dt_lua_ai_tensor_t");
    if(!t || !t->data)
    {
      g_free(inputs);
      return luaL_error(L, "input %d is not a valid tensor", i + 1);
    }
    inputs[i].data = t->data;
    inputs[i].type = DT_AI_FLOAT;
    inputs[i].shape = t->shape;
    inputs[i].ndim = t->ndim;
  }

  const int n_out = dt_ai_get_output_count(ctx);
  if(n_out <= 0)
  {
    g_free(inputs);
    return luaL_error(L, "model has no outputs");
  }

  dt_ai_tensor_t *outputs = g_new0(dt_ai_tensor_t, n_out);
  float **out_bufs = g_new0(float *, n_out);
  int64_t (*out_shapes)[MAX_TENSOR_DIMS]
    = g_malloc0(n_out * MAX_TENSOR_DIMS * sizeof(int64_t));
  int *out_ndims = g_new0(int, n_out);

  for(int i = 0; i < n_out; i++)
  {
    out_ndims[i] = dt_ai_get_output_shape(ctx, i,
                                           out_shapes[i],
                                           MAX_TENSOR_DIMS);
    if(out_ndims[i] <= 0)
    {
      for(int j = 0; j < i; j++) g_free(out_bufs[j]);
      g_free(out_bufs);
      g_free(out_shapes);
      g_free(out_ndims);
      g_free(outputs);
      g_free(inputs);
      return luaL_error(L, "cannot query output %d shape", i);
    }

    size_t sz = 1;
    gboolean dynamic = FALSE;
    for(int d = 0; d < out_ndims[i]; d++)
    {
      if(out_shapes[i][d] <= 0)
      {
        dynamic = TRUE;
        break;
      }
      sz *= (size_t)out_shapes[i][d];
    }

    if(!dynamic)
    {
      out_bufs[i] = g_try_malloc(sz * sizeof(float));
      if(!out_bufs[i])
      {
        for(int j = 0; j < i; j++) g_free(out_bufs[j]);
        g_free(out_bufs);
        g_free(out_shapes);
        g_free(out_ndims);
        g_free(outputs);
        g_free(inputs);
        return luaL_error(L,
          "failed to allocate output %d buffer", i);
      }
    }

    outputs[i].data = out_bufs[i];
    outputs[i].type = DT_AI_FLOAT;
    outputs[i].shape = out_shapes[i];
    outputs[i].ndim = out_ndims[i];
  }

  const int ret = dt_ai_run(ctx, inputs, n_in, outputs, n_out);
  g_free(inputs);

  if(ret != 0)
  {
    for(int i = 0; i < n_out; i++)
    {
      if(outputs[i].data != out_bufs[i])
        g_free(outputs[i].data);
      g_free(out_bufs[i]);
    }
    g_free(out_bufs);
    g_free(out_shapes);
    g_free(out_ndims);
    g_free(outputs);
    return luaL_error(L, "inference failed (error %d)", ret);
  }

  // wrap output buffers as Lua tensors
  for(int i = 0; i < n_out; i++)
  {
    dt_lua_ai_tensor_t *lt
      = lua_newuserdata(L, sizeof(dt_lua_ai_tensor_t));
    memset(lt, 0, sizeof(*lt));
    if(outputs[i].data != out_bufs[i])
    {
      g_free(out_bufs[i]);
      lt->data = (float *)outputs[i].data;
    }
    else
    {
      lt->data = out_bufs[i];
    }
    lt->ndim = outputs[i].ndim;
    memcpy(lt->shape, outputs[i].shape,
           outputs[i].ndim * sizeof(int64_t));
    lt->size = 1;
    for(int d = 0; d < lt->ndim; d++)
      lt->size *= (size_t)lt->shape[d];
    luaL_setmetatable(L, "dt_lua_ai_tensor_t");
  }

  g_free(out_bufs);
  g_free(out_shapes);
  g_free(out_ndims);
  g_free(outputs);
  return n_out;
}

// ctx:close()
// unload the model and free resources. also called on GC
static int _context_close(lua_State *L)
{
  dt_lua_ai_context_t *p = luaL_checkudata(L, 1, "dt_lua_ai_context_t");
  if(p && *p)
  {
    dt_ai_unload_model(*p);
    *p = NULL;
  }
  return 0;
}

/* ================================================================
 * namespace functions
 * ================================================================ */

// darktable.ai.models()
// returns a table of available models. each entry is a table with:
//   id (string), name (string), description (string),
//   task (string), status (int), is_default (bool)
static int _ai_models(lua_State *L)
{
  dt_ai_registry_t *reg = darktable.ai_registry;
  if(!reg)
  {
    lua_newtable(L);
    return 1;
  }

  g_mutex_lock(&reg->lock);
  lua_newtable(L);
  int idx = 1;
  for(GList *l = reg->models; l; l = g_list_next(l))
  {
    dt_ai_model_t *m = l->data;
    lua_newtable(L);
    lua_pushstring(L, m->id);
    lua_setfield(L, -2, "id");
    lua_pushstring(L, m->name);
    lua_setfield(L, -2, "name");
    lua_pushstring(L, m->description ? m->description : "");
    lua_setfield(L, -2, "description");
    lua_pushstring(L, m->task);
    lua_setfield(L, -2, "task");
    lua_pushinteger(L, m->status);
    lua_setfield(L, -2, "status");
    lua_pushboolean(L, m->is_default);
    lua_setfield(L, -2, "is_default");
    lua_rawseti(L, -2, idx++);
  }
  g_mutex_unlock(&reg->lock);
  return 1;
}

// darktable.ai.model_for_task(task)
// returns the model id of the enabled model for a given task
// (e.g. "denoise", "upscale", "mask"), or nil if none is active
static int _ai_model_for_task(lua_State *L)
{
  const char *task = luaL_checkstring(L, 1);
  char *model_id = dt_ai_models_get_active_for_task(task);
  if(model_id && model_id[0])
  {
    lua_pushstring(L, model_id);
    g_free(model_id);
    return 1;
  }
  g_free(model_id);
  lua_pushnil(L);
  return 1;
}

// darktable.ai.load_model(model_id [, provider])
// load an ONNX model by id (e.g. "denoise-nind").
// optional provider: "cpu", "cuda", "coreml", "directml", etc.
// returns a context object for inference
static int _ai_load_model(lua_State *L)
{
  const char *model_id = luaL_checkstring(L, 1);
  dt_ai_provider_t provider = DT_AI_PROVIDER_AUTO;

  if(lua_gettop(L) >= 2 && !lua_isnil(L, 2))
  {
    const char *prov_str = luaL_checkstring(L, 2);
    provider = dt_ai_provider_from_string(prov_str);
  }

  dt_ai_environment_t *env
    = dt_ai_registry_get_env(darktable.ai_registry);
  if(!env)
    return luaL_error(L, "AI subsystem is not available");

  dt_ai_context_t *ctx
    = dt_ai_load_model(env, model_id, NULL, provider);
  if(!ctx)
    return luaL_error(L, "failed to load model '%s'", model_id);

  dt_lua_ai_context_t *p = lua_newuserdata(L, sizeof(dt_lua_ai_context_t));
  *p = ctx;
  luaL_setmetatable(L, "dt_lua_ai_context_t");
  return 1;
}

// darktable.ai.create_tensor({d1, d2, ...})
// create a zero-filled float tensor with the given shape.
// up to 8 dimensions. returns a tensor object
static int _ai_create_tensor(lua_State *L)
{
  luaL_checktype(L, 1, LUA_TTABLE);
  const int ndim = lua_rawlen(L, 1);
  if(ndim <= 0 || ndim > MAX_TENSOR_DIMS)
    return luaL_error(L,
      "shape must have 1-%d dimensions, got %d",
      MAX_TENSOR_DIMS, ndim);

  int64_t shape[MAX_TENSOR_DIMS];
  size_t total = 1;
  for(int i = 0; i < ndim; i++)
  {
    lua_rawgeti(L, 1, i + 1);
    shape[i] = lua_tointeger(L, -1);
    lua_pop(L, 1);
    if(shape[i] <= 0)
      return luaL_error(L, "dimension %d must be positive", i);
    total *= (size_t)shape[i];
  }

  dt_lua_ai_tensor_t *t
    = lua_newuserdata(L, sizeof(dt_lua_ai_tensor_t));
  memset(t, 0, sizeof(*t));
  t->data = g_try_malloc0(total * sizeof(float));
  if(!t->data)
    return luaL_error(L, "failed to allocate tensor");
  t->ndim = ndim;
  t->size = total;
  memcpy(t->shape, shape, ndim * sizeof(int64_t));
  luaL_setmetatable(L, "dt_lua_ai_tensor_t");
  return 1;
}

// load_image(path [, width, height]) — file path variant
// loads an image file via GdkPixbuf, optional resize.
// returns NCHW float tensor [1, C, H, W] with values in [0,1]
static int _load_image_from_file(lua_State *L)
{
  const char *path = luaL_checkstring(L, 1);
  const int req_w = luaL_optinteger(L, 2, 0);
  const int req_h = luaL_optinteger(L, 3, 0);

  GError *err = NULL;
  GdkPixbuf *pb = gdk_pixbuf_new_from_file(path, &err);
  if(!pb)
  {
    const char *msg = err ? err->message : "unknown error";
    lua_pushfstring(L, "cannot load image '%s': %s", path, msg);
    if(err) g_error_free(err);
    return lua_error(L);
  }

  // optional resize
  if(req_w > 0 && req_h > 0)
  {
    GdkPixbuf *scaled
      = gdk_pixbuf_scale_simple(pb, req_w, req_h,
                                GDK_INTERP_BILINEAR);
    g_object_unref(pb);
    if(!scaled)
      return luaL_error(L, "failed to resize image");
    pb = scaled;
  }

  const int w = gdk_pixbuf_get_width(pb);
  const int h = gdk_pixbuf_get_height(pb);
  const int ch = gdk_pixbuf_get_n_channels(pb);
  const int stride = gdk_pixbuf_get_rowstride(pb);
  const guchar *pixels = gdk_pixbuf_get_pixels(pb);

  // create NCHW tensor (batch=1, channels=3)
  const int C = MIN(ch, 3);
  const size_t total = (size_t)1 * C * h * w;
  dt_lua_ai_tensor_t *t
    = lua_newuserdata(L, sizeof(dt_lua_ai_tensor_t));
  memset(t, 0, sizeof(*t));
  t->data = g_try_malloc(total * sizeof(float));
  if(!t->data)
  {
    g_object_unref(pb);
    return luaL_error(L, "failed to allocate tensor");
  }

  // HWC uint8 → NCHW float32
  for(int c = 0; c < C; c++)
    for(int y = 0; y < h; y++)
      for(int x = 0; x < w; x++)
        t->data[c * h * w + y * w + x]
          = pixels[y * stride + x * ch + c] / 255.0f;

  g_object_unref(pb);

  t->ndim = 4;
  t->shape[0] = 1;
  t->shape[1] = C;
  t->shape[2] = h;
  t->shape[3] = w;
  t->size = total;
  luaL_setmetatable(L, "dt_lua_ai_tensor_t");
  return 1;
}

// memory capture for load_from_imgid
typedef struct _lua_capture_t
{
  dt_imageio_module_data_t parent;
  float *pixels;
  int cap_w;
  int cap_h;
} _lua_capture_t;

static int _lua_capture_write(dt_imageio_module_data_t *data,
                              const char *filename,
                              const void *in,
                              const dt_colorspaces_color_profile_type_t over_type,
                              const char *over_filename,
                              void *exif, const int exif_len,
                              const dt_imgid_t imgid,
                              const int num, const int total,
                              struct dt_dev_pixelpipe_t *pipe,
                              const gboolean export_masks)
{
  _lua_capture_t *c = (_lua_capture_t *)data;
  const int w = data->width;
  const int h = data->height;
  const size_t sz = (size_t)w * h * 4 * sizeof(float);
  c->pixels = g_try_malloc(sz);
  if(c->pixels)
  {
    memcpy(c->pixels, in, sz);
    c->cap_w = w;
    c->cap_h = h;
  }
  return c->pixels ? 0 : 1;
}

static int _lua_capture_bpp(dt_imageio_module_data_t *data)
{
  return 32;
}

static int _lua_capture_levels(dt_imageio_module_data_t *data)
{
  return IMAGEIO_RGB | IMAGEIO_FLOAT;
}

static const char *_lua_capture_mime(dt_imageio_module_data_t *data)
{
  return "memory";
}

// load_image(image [, max_width, max_height]) — image object variant
// exports through the full darktable pipeline.
// 0 = full resolution (default).
// returns NCHW float tensor [1, 3, H, W] in linear RGB
static int _load_image_from_imgid(lua_State *L)
{
  dt_lua_image_t imgid;
  luaA_to(L, dt_lua_image_t, &imgid, 1);
  const int max_w = luaL_optinteger(L, 2, 0);
  const int max_h = luaL_optinteger(L, 3, 0);

  _lua_capture_t cap = {0};
  cap.parent.max_width = max_w;
  cap.parent.max_height = max_h;

  dt_imageio_module_format_t fmt = {
    .mime = _lua_capture_mime,
    .levels = _lua_capture_levels,
    .bpp = _lua_capture_bpp,
    .write_image = _lua_capture_write};

  dt_imageio_export_with_flags(imgid,
                               "unused",
                               &fmt,
                               (dt_imageio_module_data_t *)&cap,
                               TRUE,   // ignore_exif
                               FALSE,  // display_byteorder
                               TRUE,   // high_quality
                               FALSE,  // upscale
                               FALSE,  // is_scaling
                               1.0,    // scale_factor
                               FALSE,  // thumbnail_export
                               NULL,   // filter
                               FALSE,  // copy_metadata
                               FALSE,  // export_masks
                               dt_colorspaces_get_work_profile(imgid)->type,
                               NULL,
                               DT_INTENT_PERCEPTUAL,
                               NULL, NULL, 1, 1, NULL, -1);

  if(!cap.pixels)
    return luaL_error(L, "failed to export image %d", imgid);

  const int w = cap.cap_w;
  const int h = cap.cap_h;
  const int C = 3;
  const size_t total = (size_t)1 * C * h * w;

  dt_lua_ai_tensor_t *t
    = lua_newuserdata(L, sizeof(dt_lua_ai_tensor_t));
  memset(t, 0, sizeof(*t));
  t->data = g_try_malloc(total * sizeof(float));
  if(!t->data)
  {
    g_free(cap.pixels);
    return luaL_error(L, "failed to allocate tensor");
  }

  // RGBx 4ch float → NCHW 3ch float
  for(int c = 0; c < C; c++)
    for(int y = 0; y < h; y++)
      for(int x = 0; x < w; x++)
        t->data[c * h * w + y * w + x]
          = cap.pixels[((size_t)y * w + x) * 4 + c];

  g_free(cap.pixels);

  t->ndim = 4;
  t->shape[0] = 1;
  t->shape[1] = C;
  t->shape[2] = h;
  t->shape[3] = w;
  t->size = total;
  luaL_setmetatable(L, "dt_lua_ai_tensor_t");
  return 1;
}

// darktable.ai.load_image(path_or_image [, w, h])
// overloaded: string arg loads from file, image object exports
// through the darktable pipeline. returns NCHW float tensor
static int _ai_load_image(lua_State *L)
{
  if(lua_isstring(L, 1))
    return _load_image_from_file(L);
  if(dt_lua_isa(L, 1, dt_lua_image_t))
    return _load_image_from_imgid(L);
  return luaL_error(L,
    "load_image expects a file path (string) or image object");
}

// darktable.ai.load_raw(image)
// load raw CFA sensor data from a darktable image object.
// returns two values:
//   1. tensor [1,1,H,W] — single-channel float CFA mosaic
//   2. metadata table: imgid, filters, black_level,
//      black_level_separate, white_level, wb_coeffs,
//      color_matrix, width, height, xtrans (X-Trans only)
static int _ai_load_raw(lua_State *L)
{
  dt_lua_image_t imgid;
  luaA_to(L, dt_lua_image_t, &imgid, 1);

  // get image metadata
  const dt_image_t *img = dt_image_cache_get(imgid, 'r');
  if(!img)
    return luaL_error(L, "cannot access image %d", imgid);

  // save metadata before releasing cache
  const uint32_t filters = img->buf_dsc.filters;
  uint8_t xtrans[6][6];
  memcpy(xtrans, img->buf_dsc.xtrans, sizeof(xtrans));
  const uint16_t black_level = img->raw_black_level;
  uint16_t black_separate[4];
  memcpy(black_separate, img->raw_black_level_separate,
         sizeof(black_separate));
  const uint32_t white_point = img->raw_white_point;
  dt_aligned_pixel_t wb_coeffs;
  memcpy(wb_coeffs, img->wb_coeffs, sizeof(wb_coeffs));
  float color_matrix[4][3];
  memcpy(color_matrix, img->adobe_XYZ_to_CAM,
         sizeof(color_matrix));
  const dt_iop_buffer_type_t datatype = img->buf_dsc.datatype;
  const int channels = img->buf_dsc.channels;

  dt_image_cache_read_release(img);

  // load full-resolution raw buffer
  dt_mipmap_buffer_t buf;
  dt_mipmap_cache_get(&buf, imgid, DT_MIPMAP_FULL,
                      DT_MIPMAP_BLOCKING, 'r');
  if(!buf.buf || buf.width <= 0 || buf.height <= 0)
  {
    dt_mipmap_cache_release(&buf);
    return luaL_error(L, "cannot load raw data for image %d",
                      imgid);
  }

  const int w = buf.width;
  const int h = buf.height;
  const size_t total = (size_t)w * h;

  // create tensor [1, 1, H, W]
  dt_lua_ai_tensor_t *t
    = lua_newuserdata(L, sizeof(dt_lua_ai_tensor_t));
  memset(t, 0, sizeof(*t));
  t->data = g_try_malloc(total * sizeof(float));
  if(!t->data)
  {
    dt_mipmap_cache_release(&buf);
    return luaL_error(L, "failed to allocate tensor");
  }

  // convert raw buffer to float.
  // raw CFA images have channels==1; if the loader delivered
  // multi-channel data (e.g. demosaiced), take only channel 0
  if(datatype == TYPE_FLOAT)
  {
    const float *src = (const float *)buf.buf;
    if(channels == 1)
      memcpy(t->data, src, total * sizeof(float));
    else
      for(size_t i = 0; i < total; i++)
        t->data[i] = src[i * channels];
  }
  else
  {
    const uint16_t *src = (const uint16_t *)buf.buf;
    for(size_t i = 0; i < total; i++)
      t->data[i] = (float)src[i * channels];
  }

  dt_mipmap_cache_release(&buf);

  t->ndim = 4;
  t->shape[0] = 1;
  t->shape[1] = 1;
  t->shape[2] = h;
  t->shape[3] = w;
  t->size = total;
  luaL_setmetatable(L, "dt_lua_ai_tensor_t");

  // build metadata table
  lua_newtable(L);

  lua_pushinteger(L, imgid);
  lua_setfield(L, -2, "imgid");

  lua_pushinteger(L, filters);
  lua_setfield(L, -2, "filters");

  lua_pushinteger(L, black_level);
  lua_setfield(L, -2, "black_level");

  lua_pushinteger(L, white_point);
  lua_setfield(L, -2, "white_level");

  lua_pushinteger(L, w);
  lua_setfield(L, -2, "width");

  lua_pushinteger(L, h);
  lua_setfield(L, -2, "height");

  // black_level_separate[4]
  lua_newtable(L);
  for(int i = 0; i < 4; i++)
  {
    lua_pushinteger(L, black_separate[i]);
    lua_rawseti(L, -2, i + 1);
  }
  lua_setfield(L, -2, "black_level_separate");

  // wb_coeffs[3]
  lua_newtable(L);
  for(int i = 0; i < 3; i++)
  {
    lua_pushnumber(L, wb_coeffs[i]);
    lua_rawseti(L, -2, i + 1);
  }
  lua_setfield(L, -2, "wb_coeffs");

  // color_matrix[4][3]
  lua_newtable(L);
  for(int r = 0; r < 4; r++)
  {
    lua_newtable(L);
    for(int c = 0; c < 3; c++)
    {
      lua_pushnumber(L, color_matrix[r][c]);
      lua_rawseti(L, -2, c + 1);
    }
    lua_rawseti(L, -2, r + 1);
  }
  lua_setfield(L, -2, "color_matrix");

  // xtrans[6][6] (only for X-Trans: filters == 9u)
  if(filters == 9u)
  {
    lua_newtable(L);
    for(int r = 0; r < 6; r++)
    {
      lua_newtable(L);
      for(int c = 0; c < 6; c++)
      {
        lua_pushinteger(L, xtrans[r][c]);
        lua_rawseti(L, -2, c + 1);
      }
      lua_rawseti(L, -2, r + 1);
    }
    lua_setfield(L, -2, "xtrans");
  }

  return 2; // tensor, metadata
}

// read EXIF blob from the source raw image. caller frees with g_free.
// returns blob length; 0 means none (and *out_blob stays NULL)
static int _read_exif_for_imgid(dt_imgid_t imgid,
                                int width, int height,
                                uint8_t **out_blob)
{
  *out_blob = NULL;
  if(!dt_is_valid_imgid(imgid)) return 0;

  char srcpath[PATH_MAX] = {0};
  dt_image_full_path(imgid, srcpath, sizeof(srcpath), NULL);
  if(!srcpath[0]) return 0;

  return dt_exif_read_blob(out_blob, srcpath, imgid, FALSE,
                           width, height, TRUE);
}

// darktable.ai.save_dng(tensor, imgid, path)
// write a Bayer CFA tensor as a re-importable uint16 DNG.
// tensor must be [1,1,H,W] in raw ADC units (same range as
// dt_image_t::raw_white_point — i.e. what load_raw returns).
// all DNG metadata (CFA pattern, BlackLevel, WhiteLevel,
// AsShotNeutral, ColorMatrix1, Make/Model) is sourced from imgid.
// EXIF from the source raw is copied automatically.
static int _ai_save_dng(lua_State *L)
{
  dt_lua_ai_tensor_t *t = luaL_checkudata(L, 1, "dt_lua_ai_tensor_t");
  if(!t || !t->data)
    return luaL_error(L, "tensor has been freed");
  if(t->ndim != 4 || t->shape[0] != 1 || t->shape[1] != 1)
    return luaL_error(L,
      "save_dng requires [1,1,H,W] CFA tensor");

  dt_lua_image_t imgid;
  luaA_to(L, dt_lua_image_t, &imgid, 2);
  const char *path = luaL_checkstring(L, 3);

  const int H = (int)t->shape[2];
  const int W = (int)t->shape[3];
  const size_t total = (size_t)W * H;

  // quantize float CFA → uint16 (load_raw returned raw ADC units, so
  // values are already in [0, white_point]; just clamp + cast)
  uint16_t *cfa = g_try_malloc(total * sizeof(uint16_t));
  if(!cfa) return luaL_error(L, "failed to allocate CFA buffer");
  for(size_t i = 0; i < total; i++)
  {
    const float v = t->data[i];
    cfa[i] = v <= 0.0f ? 0
           : v >= 65535.0f ? 65535
           : (uint16_t)(v + 0.5f);
  }

  uint8_t *exif_blob = NULL;
  const int exif_len = _read_exif_for_imgid(imgid, W, H, &exif_blob);

  const dt_image_t *img = dt_image_cache_get(imgid, 'r');
  if(!img)
  {
    g_free(cfa);
    g_free(exif_blob);
    return luaL_error(L, "cannot access image %d", imgid);
  }

  const int rc = dt_imageio_dng_write_cfa_bayer(path, cfa, W, H,
                                                img, exif_blob, exif_len,
                                                NULL /* preview */);
  dt_image_cache_read_release(img);
  g_free(cfa);
  g_free(exif_blob);

  if(rc) return luaL_error(L, "failed to write DNG '%s'", path);
  return 0;
}

// darktable.ai.save_dng_linear(tensor, imgid, path)
// write a demosaicked 3-channel tensor as a re-importable LinearRaw
// DNG. tensor must be [1,3,H,W] in float-normalized camRGB
// (1.0 = source sensor white point after black subtract). black /
// white levels and color metadata are sourced from imgid.
static int _ai_save_dng_linear(lua_State *L)
{
  dt_lua_ai_tensor_t *t = luaL_checkudata(L, 1, "dt_lua_ai_tensor_t");
  if(!t || !t->data)
    return luaL_error(L, "tensor has been freed");
  if(t->ndim != 4 || t->shape[0] != 1 || t->shape[1] != 3)
    return luaL_error(L,
      "save_dng_linear requires [1,3,H,W] RGB tensor");

  dt_lua_image_t imgid;
  luaA_to(L, dt_lua_image_t, &imgid, 2);
  const char *path = luaL_checkstring(L, 3);

  const int H = (int)t->shape[2];
  const int W = (int)t->shape[3];
  const size_t plane = (size_t)W * H;

  // NCHW float planar → interleaved float[3]; the DNG writer takes
  // RGB×width×height in that order
  float *rgb = g_try_malloc(plane * 3 * sizeof(float));
  if(!rgb) return luaL_error(L, "failed to allocate RGB buffer");
  const float *r = t->data;
  const float *g = t->data + plane;
  const float *b = t->data + 2 * plane;
  for(size_t i = 0; i < plane; i++)
  {
    rgb[3 * i + 0] = r[i];
    rgb[3 * i + 1] = g[i];
    rgb[3 * i + 2] = b[i];
  }

  uint8_t *exif_blob = NULL;
  const int exif_len = _read_exif_for_imgid(imgid, W, H, &exif_blob);

  const dt_image_t *img = dt_image_cache_get(imgid, 'r');
  if(!img)
  {
    g_free(rgb);
    g_free(exif_blob);
    return luaL_error(L, "cannot access image %d", imgid);
  }

  const int rc = dt_imageio_dng_write_linear(path, rgb, W, H,
                                             img, exif_blob, exif_len,
                                             NULL /* preview */);
  dt_image_cache_read_release(img);
  g_free(rgb);
  g_free(exif_blob);

  if(rc) return luaL_error(L, "failed to write linear DNG '%s'", path);
  return 0;
}

// tensor:save_tiff(path [, bpp [, image]])
// save tensor as TIFF image. bpp = 16 (default) or 32.
// optional image object: embeds the working ICC profile from
// that image so darktable interprets the color space correctly.
// tensor must be NCHW [1, C, H, W] with C=1 or C=3.
// 16-bit: float values clamped to [0,1] and mapped to [0,65535]
// 32-bit: float values written directly
static int _tensor_save_tiff(lua_State *L)
{
  dt_lua_ai_tensor_t *t
    = luaL_checkudata(L, 1, "dt_lua_ai_tensor_t");
  const char *path = luaL_checkstring(L, 2);
  const int bpp = luaL_optinteger(L, 3, 16);

  if(!t || !t->data)
    return luaL_error(L, "tensor has been freed");
  if(t->ndim != 4 || t->shape[0] != 1)
    return luaL_error(L,
      "save_tiff requires NCHW tensor with batch=1");
  if(bpp != 16 && bpp != 32)
    return luaL_error(L, "bpp must be 16 or 32, got %d", bpp);

  const int C = (int)t->shape[1];
  const int H = (int)t->shape[2];
  const int W = (int)t->shape[3];

  if(C != 3 && C != 1)
    return luaL_error(L,
      "save_tiff requires 1 or 3 channels, got %d", C);

  TIFF *tif = TIFFOpen(path, "w");
  if(!tif)
    return luaL_error(L, "cannot open '%s' for writing", path);

  TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, W);
  TIFFSetField(tif, TIFFTAG_IMAGELENGTH, H);
  TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, C);
  TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, bpp);
  TIFFSetField(tif, TIFFTAG_SAMPLEFORMAT,
               bpp == 32 ? SAMPLEFORMAT_IEEEFP : SAMPLEFORMAT_UINT);
  TIFFSetField(tif, TIFFTAG_PHOTOMETRIC,
               C == 1 ? PHOTOMETRIC_MINISBLACK : PHOTOMETRIC_RGB);
  TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
  TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, 1);
  TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_NONE);

  // embed ICC profile from source image if provided
  if(lua_gettop(L) >= 4 && dt_lua_isa(L, 4, dt_lua_image_t))
  {
    dt_lua_image_t imgid;
    luaA_to(L, dt_lua_image_t, &imgid, 4);
    const dt_colorspaces_color_profile_t *cp
      = dt_colorspaces_get_work_profile(imgid);
    if(cp && cp->profile)
    {
      uint32_t icc_len = 0;
      cmsSaveProfileToMem(cp->profile, NULL, &icc_len);
      if(icc_len > 0)
      {
        uint8_t *icc_buf = g_try_malloc(icc_len);
        if(icc_buf)
        {
          cmsSaveProfileToMem(cp->profile, icc_buf, &icc_len);
          TIFFSetField(tif, TIFFTAG_ICCPROFILE, icc_len, icc_buf);
          g_free(icc_buf);
        }
      }
    }
  }

  if(bpp == 32)
  {
    // write float scanlines (NCHW → HWC interleaved)
    float *row = g_try_malloc((size_t)W * C * sizeof(float));
    if(!row)
    {
      TIFFClose(tif);
      return luaL_error(L, "failed to allocate row buffer");
    }
    for(int y = 0; y < H; y++)
    {
      for(int x = 0; x < W; x++)
        for(int c = 0; c < C; c++)
          row[x * C + c] = t->data[c * H * W + y * W + x];
      TIFFWriteScanline(tif, row, y, 0);
    }
    g_free(row);
  }
  else
  {
    // write 16-bit scanlines (NCHW → HWC, clamped [0,1])
    uint16_t *row
      = g_try_malloc((size_t)W * C * sizeof(uint16_t));
    if(!row)
    {
      TIFFClose(tif);
      return luaL_error(L, "failed to allocate row buffer");
    }
    for(int y = 0; y < H; y++)
    {
      for(int x = 0; x < W; x++)
        for(int c = 0; c < C; c++)
        {
          float v = t->data[c * H * W + y * W + x];
          v = CLAMP(v, 0.0f, 1.0f);
          row[x * C + c] = (uint16_t)(v * 65535.0f + 0.5f);
        }
      TIFFWriteScanline(tif, row, y, 0);
    }
    g_free(row);
  }

  TIFFClose(tif);
  return 0;
}

// tensor:save(path)
// save tensor as 8-bit image (PNG/JPEG/TIFF, detected from extension).
// tensor must be NCHW [1, C, H, W] with C=1 or C=3.
// float values are clamped to [0,1] and mapped to [0,255]
static int _tensor_save(lua_State *L)
{
  dt_lua_ai_tensor_t *t = luaL_checkudata(L, 1, "dt_lua_ai_tensor_t");
  const char *path = luaL_checkstring(L, 2);

  if(!t || !t->data)
    return luaL_error(L, "tensor has been freed");
  if(t->ndim != 4 || t->shape[0] != 1)
    return luaL_error(L,
      "save requires NCHW tensor with batch=1");

  const int C = (int)t->shape[1];
  const int H = (int)t->shape[2];
  const int W = (int)t->shape[3];

  if(C != 3 && C != 1)
    return luaL_error(L,
      "save requires 1 or 3 channels, got %d", C);

  const int out_ch = 3;
  guchar *pixels = g_try_malloc((size_t)W * H * out_ch);
  if(!pixels)
    return luaL_error(L, "failed to allocate pixel buffer");

  // NCHW float32 → HWC uint8
  for(int y = 0; y < H; y++)
    for(int x = 0; x < W; x++)
      for(int c = 0; c < out_ch; c++)
      {
        const int src_c = (c < C) ? c : 0; // grayscale→RGB
        float v = t->data[src_c * H * W + y * W + x];
        v = CLAMP(v, 0.0f, 1.0f);
        pixels[(y * W + x) * out_ch + c]
          = (guchar)(v * 255.0f + 0.5f);
      }

  GdkPixbuf *pb
    = gdk_pixbuf_new_from_data(pixels, GDK_COLORSPACE_RGB,
                                FALSE, 8, W, H, W * out_ch,
                                NULL, NULL);
  if(!pb)
  {
    g_free(pixels);
    return luaL_error(L, "failed to create pixbuf for save");
  }

  // determine format from extension
  const char *ext = strrchr(path, '.');
  const char *fmt = "png";
  if(ext)
  {
    if(!g_ascii_strcasecmp(ext, ".jpg")
       || !g_ascii_strcasecmp(ext, ".jpeg"))
      fmt = "jpeg";
    else if(!g_ascii_strcasecmp(ext, ".tiff")
            || !g_ascii_strcasecmp(ext, ".tif"))
      fmt = "tiff";
  }

  GError *err = NULL;
  gboolean ok = gdk_pixbuf_save(pb, path, fmt, &err, NULL);
  g_object_unref(pb);
  g_free(pixels);

  if(!ok)
  {
    const char *msg = err ? err->message : "unknown error";
    lua_pushfstring(L, "cannot save image '%s': %s", path, msg);
    if(err) g_error_free(err);
    return lua_error(L);
  }

  return 0;
}

/* ================================================================
 * initialization
 * ================================================================ */

int dt_lua_init_ai(lua_State *L)
{
  // tensor type: value userdata with custom GC (heap-allocated data)
  luaL_newmetatable(L, "dt_lua_ai_tensor_t");
  lua_pushcfunction(L, _tensor_gc);
  lua_setfield(L, -2, "__gc");
  lua_pushcfunction(L, _tensor_tostring);
  lua_setfield(L, -2, "__tostring");
  lua_newtable(L);
  lua_pushcfunction(L, _tensor_get);
  lua_setfield(L, -2, "get");
  lua_pushcfunction(L, _tensor_set);
  lua_setfield(L, -2, "set");
  lua_pushcfunction(L, _tensor_save);
  lua_setfield(L, -2, "save");
  lua_pushcfunction(L, _tensor_save_tiff);
  lua_setfield(L, -2, "save_tiff");
  lua_pushcfunction(L, _tensor_linear_to_srgb);
  lua_setfield(L, -2, "linear_to_srgb");
  lua_pushcfunction(L, _tensor_srgb_to_linear);
  lua_setfield(L, -2, "srgb_to_linear");
  lua_pushcfunction(L, _tensor_shape);
  lua_setfield(L, -2, "shape");
  lua_pushcfunction(L, _tensor_ndim);
  lua_setfield(L, -2, "ndim");
  lua_pushcfunction(L, _tensor_size);
  lua_setfield(L, -2, "size");
  lua_pushcfunction(L, _tensor_dot);
  lua_setfield(L, -2, "dot");
  lua_pushcfunction(L, _tensor_crop);
  lua_setfield(L, -2, "crop");
  lua_pushcfunction(L, _tensor_paste);
  lua_setfield(L, -2, "paste");
  lua_pushcfunction(L, _tensor_fill);
  lua_setfield(L, -2, "fill");
  lua_pushcfunction(L, _tensor_scale_add);
  lua_setfield(L, -2, "scale_add");
  lua_pushcfunction(L, _tensor_sum);
  lua_setfield(L, -2, "sum");
  lua_pushcfunction(L, _tensor_mean);
  lua_setfield(L, -2, "mean");
  lua_pushcfunction(L, _tensor_bayer_pack);
  lua_setfield(L, -2, "bayer_pack");
  lua_pushcfunction(L, _tensor_bayer_unpack);
  lua_setfield(L, -2, "bayer_unpack");
  lua_setfield(L, -2, "__index");
  lua_pop(L, 1);

  // context type: pointer userdata with GC
  luaL_newmetatable(L, "dt_lua_ai_context_t");
  lua_pushcfunction(L, _context_close);
  lua_setfield(L, -2, "__gc");
  lua_newtable(L);
  lua_pushcfunction(L, _context_run);
  lua_setfield(L, -2, "run");
  lua_pushcfunction(L, _context_close);
  lua_setfield(L, -2, "close");
  lua_setfield(L, -2, "__index");
  lua_pop(L, 1);

  // darktable.ai namespace as singleton
  dt_lua_push_darktable_lib(L);
  luaA_Type type_id = dt_lua_init_singleton(L, "ai_lib", NULL);
  lua_setfield(L, -2, "ai");
  lua_pop(L, 1);

  lua_pushcfunction(L, _ai_models);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, type_id, "models");

  lua_pushcfunction(L, _ai_model_for_task);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, type_id, "model_for_task");

  lua_pushcfunction(L, _ai_load_model);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, type_id, "load_model");

  lua_pushcfunction(L, _ai_create_tensor);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, type_id, "create_tensor");

  lua_pushcfunction(L, _ai_load_image);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, type_id, "load_image");

  lua_pushcfunction(L, _ai_load_raw);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, type_id, "load_raw");

  lua_pushcfunction(L, _ai_save_dng);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, type_id, "save_dng");

  lua_pushcfunction(L, _ai_save_dng_linear);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, type_id, "save_dng_linear");

  return 0;
}

#else /* !USE_LUA || !HAVE_AI */

#include "lua/ai.h"

int dt_lua_init_ai(lua_State *L)
{
  (void)L;
  return 0;
}

#endif

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
