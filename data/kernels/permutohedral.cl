/*
    This file is part of darktable,
    copyright (c) 2026 darktable developers.

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

// the 5D permutohedral lattice of src/iop/Permutohedral.h, as surface blur
// (src/iop/bilateral.cc) uses it, plus that module's direct stamp for
// small radii.
//
// The CPU fills one hash table per thread and merges them. Here all pixels
// insert into one open-addressing table: slots holds two words per slot, the
// fingerprint of the key with the insert round in its low 3 bits, and the
// index of the key's entry. A slot is claimed by atomic_cmpxchg() of the
// first word. The winner writes the key after the swap, so a work item that
// meets a slot with its own fingerprint, claimed in this round, can't compare
// keys yet and takes it as its own. The splat then counts the vertices it
// can't find, and the host runs another round for those. Keeping the round
// in the swapped word, rather than in a separate array, is what stops a
// racing work item from comparing against a key that is still unwritten and
// inserting a duplicate.
//
// Keys are the CPU's 5 shorts, kept with the same 16-bit wrap-around, and the
// bucket is the CPU's hash. The table layout doesn't change the output; only
// the order of the float sums does.

#include "common.h"

#define PL_D 5

// the prm buffer: 1/sigma for x, y, r, g, b, the lattice's scale factors,
// then the roi origin in the image
#define PRM_SCALE 5
#define PRM_ORIGIN 10

#define CNT_ENTRIES 0
#define CNT_OVERFLOW 1
#define CNT_MISSING 2

static inline int _sext16(const int v)
{
  return (v << 16) >> 16;
}

static inline uint3 _pack(const int *k)
{
  return (uint3)(((uint)k[0] & 0xffffu) | ((uint)k[1] << 16),
                 ((uint)k[2] & 0xffffu) | ((uint)k[3] << 16),
                 (uint)k[4] & 0xffffu);
}

// Key::setHash()
static inline uint _hash(const int *k)
{
  uint h = 0u;
  for(int i = 0; i < PL_D; i++)
  {
    h += (uint)k[i];
    h *= 2531011u;
  }
  return h;
}

static inline uint _fmix(uint h)
{
  h ^= h >> 16;
  h *= 0x85ebca6bu;
  h ^= h >> 13;
  h *= 0xc2b2ae35u;
  h ^= h >> 16;
  return h;
}

// an independent hash: Key::setHash() is linear in the coordinates, so
// different vertices often share it, and it can't tell them apart
static inline uint _fingerprint(const int *k)
{
  uint h = 0x9747b28cu;
  for(int i = 0; i < PL_D; i++)
    h = _fmix(h ^ ((uint)k[i] & 0xffffu)) + 0x9e3779b9u * (uint)(i + 1);
  h &= ~7u;
  return h == 0u ? 8u : h;
}

// PermutohedralLattice::splat() without the table: the D+1 vertices of the
// simplex that holds the pixel, and their barycentric weights
static void _simplex(const int x, const int y, const float4 p, global const float *prm,
                     int *keys, float *weight)
{
  float position[PL_D];
  position[0] = ((float)x + prm[PRM_ORIGIN]) * prm[0];
  position[1] = ((float)y + prm[PRM_ORIGIN + 1]) * prm[1];
  position[2] = p.x * prm[2];
  position[3] = p.y * prm[3];
  position[4] = p.z * prm[4];

  float elevated[PL_D + 1];
  int greedy[PL_D + 1];
  int rank[PL_D + 1];
  float barycentric[PL_D + 2];

  elevated[PL_D] = -PL_D * position[PL_D - 1] * prm[PRM_SCALE + PL_D - 1];
  for(int i = PL_D - 1; i > 0; i--)
    elevated[i] = (elevated[i + 1] - i * position[i - 1] * prm[PRM_SCALE + i - 1]
                   + (i + 2) * position[i] * prm[PRM_SCALE + i]);
  elevated[0] = elevated[1] + 2 * position[0] * prm[PRM_SCALE];

  const float scale = 1.0f / (PL_D + 1);
  for(int i = 0; i <= PL_D; i++)
  {
    const float v = elevated[i] * scale;
    const float up = ceil(v) * (PL_D + 1);
    const float down = floor(v) * (PL_D + 1);
    greedy[i] = (up - elevated[i] < elevated[i] - down) ? (int)up : (int)down;
  }

  int sum = 0;
  for(int i = 0; i <= PL_D; i++) sum += greedy[i];
  sum /= PL_D + 1;

  for(int i = 0; i <= PL_D; i++) rank[i] = 0;
  for(int i = 0; i < PL_D; i++)
    for(int j = i + 1; j <= PL_D; j++)
      if(elevated[i] - greedy[i] < elevated[j] - greedy[j])
        rank[i]++;
      else
        rank[j]++;

  if(sum > 0)
  {
    for(int i = 0; i <= PL_D; i++)
    {
      if(rank[i] >= PL_D + 1 - sum)
      {
        greedy[i] -= PL_D + 1;
        rank[i] += sum - (PL_D + 1);
      }
      else
        rank[i] += sum;
    }
  }
  else if(sum < 0)
  {
    for(int i = 0; i <= PL_D; i++)
    {
      if(rank[i] < -sum)
      {
        greedy[i] += PL_D + 1;
        rank[i] += (PL_D + 1) + sum;
      }
      else
        rank[i] += sum;
    }
  }

  for(int i = 0; i < PL_D + 2; i++) barycentric[i] = 0.0f;
  for(int i = 0; i <= PL_D; i++)
  {
    barycentric[PL_D - rank[i]] += (elevated[i] - greedy[i]) * scale;
    barycentric[PL_D + 1 - rank[i]] -= (elevated[i] - greedy[i]) * scale;
  }
  barycentric[0] += 1.0f + barycentric[PL_D + 1];

  for(int r = 0; r <= PL_D; r++)
  {
    // the canonical simplex of the PermutohedralLattice constructor
    for(int i = 0; i < PL_D; i++)
      keys[r * PL_D + i] = _sext16(greedy[i] + (rank[i] <= PL_D - r ? r : r - (PL_D + 1)));
    weight[r] = barycentric[r];
  }
}

static inline int _keys_match(global const uint *keys, const uint e, const uint3 pk)
{
  return keys[3 * e] == pk.x && keys[3 * e + 1] == pk.y && keys[3 * e + 2] == pk.z;
}

// HashTablePermutohedral::lookupOffset() with create = false, from a kernel
// that runs after every insert has finished
static int _find(const int *k, global const uint *slots, global const uint *keys,
                 const uint capacity, const uint entries)
{
  const uint3 pk = _pack(k);
  const uint fp = _fingerprint(k);
  uint b = _hash(k) % capacity;
  for(uint n = 0; n < capacity; n++)
  {
    const uint w = slots[2 * b];
    if(w == 0u) return -1;
    if((w & ~7u) == fp)
    {
      const uint e = slots[2 * b + 1];
      if(e < entries && _keys_match(keys, e, pk)) return (int)e;
    }
    b = (b + 1 == capacity) ? 0u : b + 1;
  }
  return -1;
}

static void _insert(const int *k, global uint *slots, global uint *keys, global uint *counters,
                    const uint capacity, const uint max_entries, const uint round)
{
  const uint3 pk = _pack(k);
  const uint fp = _fingerprint(k);
  const uint want = fp | round;
  uint b = _hash(k) % capacity;
  for(uint n = 0; n < capacity; n++)
  {
    const uint orig = atomic_cmpxchg((volatile global uint *)&slots[2 * b], 0u, want);
    if(orig == 0u)
    {
      const uint e = atomic_inc((volatile global uint *)&counters[CNT_ENTRIES]);
      if(e < max_entries)
      {
        keys[3 * e] = pk.x;
        keys[3 * e + 1] = pk.y;
        keys[3 * e + 2] = pk.z;
        slots[2 * b + 1] = e;
      }
      else
        counters[CNT_OVERFLOW] = 1u;
      return;
    }
    if((orig & ~7u) == fp)
    {
      // claimed in this round: its key may not be written yet, see the top
      if((orig & 7u) == round) return;
      const uint e = slots[2 * b + 1];
      if(e < max_entries && _keys_match(keys, e, pk)) return;
    }
    b = (b + 1 == capacity) ? 0u : b + 1;
  }
  counters[CNT_OVERFLOW] = 1u;
}

kernel void
permutohedral_insert(read_only image2d_t in, global uint *slots, global uint *keys,
                     global uint *counters, global const float *prm, const int width,
                     const int height, const uint capacity, const uint max_entries,
                     const uint round)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;

  int vk[(PL_D + 1) * PL_D];
  float w[PL_D + 1];
  _simplex(x, y, read_imagef(in, sampleri, (int2)(x, y)), prm, vk, w);
  for(int r = 0; r <= PL_D; r++)
    _insert(vk + r * PL_D, slots, keys, counters, capacity, max_entries, round);
}

kernel void
permutohedral_splat(read_only image2d_t in, global const uint *slots, global const uint *keys,
                    global uint *counters, global float *values, global const float *prm,
                    const int width, const int height, const uint capacity, const uint entries)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;

  const float4 p = read_imagef(in, sampleri, (int2)(x, y));
  int vk[(PL_D + 1) * PL_D];
  float w[PL_D + 1];
  _simplex(x, y, p, prm, vk, w);
  for(int r = 0; r <= PL_D; r++)
  {
    const int e = _find(vk + r * PL_D, slots, keys, capacity, entries);
    if(e < 0)
    {
      atomic_inc((volatile global uint *)&counters[CNT_MISSING]);
      continue;
    }
    atomic_add_f(values + 4 * e, w[r] * p.x);
    atomic_add_f(values + 4 * e + 1, w[r] * p.y);
    atomic_add_f(values + 4 * e + 2, w[r] * p.z);
    atomic_add_f(values + 4 * e + 3, w[r]);
  }
}

static inline float4 _value(global const float *values, const int e)
{
  return e < 0 ? (float4)(0.0f) : vload4(e, values);
}

// one pass of PermutohedralLattice::blur() along lattice axis j. The entries
// are laid out row_width wide, which keeps the work sizes small for tens of
// millions of entries.
kernel void
permutohedral_blur(global const uint *slots, global const uint *keys, global const float *src,
                   global float *dst, const uint capacity, const uint entries, const int row_width,
                   const int j)
{
  const int gx = get_global_id(0);
  const uint e = (uint)get_global_id(1) * (uint)row_width + (uint)gx;
  if(gx >= row_width || e >= entries) return;

  int k[PL_D];
  k[0] = _sext16((int)(keys[3 * e] & 0xffffu));
  k[1] = (int)keys[3 * e] >> 16;
  k[2] = _sext16((int)(keys[3 * e + 1] & 0xffffu));
  k[3] = (int)keys[3 * e + 1] >> 16;
  k[4] = _sext16((int)(keys[3 * e + 2] & 0xffffu));

  // Key(origin, dim, direction)
  int n1[PL_D];
  int n2[PL_D];
  for(int i = 0; i < PL_D; i++)
  {
    n1[i] = _sext16(k[i] + 1);
    n2[i] = _sext16(k[i] - 1);
  }
  if(j < PL_D)
  {
    n1[j] = _sext16(k[j] - PL_D);
    n2[j] = _sext16(k[j] + PL_D);
  }

  const float4 vm = _value(src, _find(n1, slots, keys, capacity, entries));
  const float4 vp = _value(src, _find(n2, slots, keys, capacity, entries));
  const float4 v = _value(src, (int)e);
  vstore4(0.25f * vm + 0.5f * v + 0.25f * vp, e, dst);
}

// PermutohedralLattice::slice() and the normalization of process(). The
// simplex is computed again instead of stored, which saves the 52 bytes
// per pixel of the CPU's replay array.
kernel void
permutohedral_slice(read_only image2d_t in, write_only image2d_t out, global const uint *slots,
                    global const uint *keys, global const float *values, global const float *prm,
                    const int width, const int height, const uint capacity, const uint entries)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;

  int vk[(PL_D + 1) * PL_D];
  float w[PL_D + 1];
  _simplex(x, y, read_imagef(in, sampleri, (int2)(x, y)), prm, vk, w);
  float4 col = (float4)(0.0f);
  for(int r = 0; r <= PL_D; r++)
  {
    const int e = _find(vk + r * PL_D, slots, keys, capacity, entries);
    if(e >= 0) col += w[r] * _value(values, e);
  }
  write_imagef(out, (int2)(x, y), (float4)(col.xyz / col.w, 1.0f));
}

// the direct stamp of process() for radii up to MAX_DIRECT_STAMP_RADIUS:
// weights holds the normalized spatial gaussian, (2 rad + 1)^2 values. The
// border of rad pixels is copied, as on the CPU.
kernel void
bilateral_direct(read_only image2d_t in, write_only image2d_t out, global const float *weights,
                 const int width, const int height, const int rad, const float4 isig2col)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;

  const float4 pixel = read_imagef(in, sampleri, (int2)(x, y));
  if(y < rad || y >= height - rad || x < rad || x >= width - rad)
  {
    write_imagef(out, (int2)(x, y), pixel);
    return;
  }

  const int wd = 2 * rad + 1;
  float sumw = 0.0f;
  float4 res = (float4)(0.0f);
  for(int l = -rad; l <= rad; l++)
  {
    for(int k = -rad; k <= rad; k++)
    {
      const float4 inp = read_imagef(in, sampleri, (int2)(x + k, y + l));
      const float4 chandiff = (pixel - inp) * (pixel - inp) * isig2col;
      const float diff = chandiff.x + chandiff.y + chandiff.z;
      const float pix_weight = weights[(l + rad) * wd + k + rad] * exp(-diff);
      res += inp * pix_weight;
      sumw += pix_weight;
    }
  }
  write_imagef(out, (int2)(x, y), res / sumw);
}
