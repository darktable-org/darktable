/*
    This file is part of darktable,
    Copyright (C) 2012-2020 darktable developers.

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

#pragma once

// simple implementation of a heap/priority queue, using uint64_t as key and
// float values to sort the elements.
// meant to support scheduling of background jobs with priorities.
typedef struct heap_t
{
  uint32_t size;
  uint32_t end;
  uint64_t *keys;
  float *vals;
} heap_t;

heap_t *heap_init(uint32_t size)
{
  heap_t *h = (heap_t *)malloc(sizeof(heap_t));
  h->keys = (uint64_t *)malloc(sizeof(uint64_t) * size);
  h->vals = (float *)malloc(sizeof(float) * size);
  h->size = size;
  h->end = 0;
  return h;
}

void heap_cleanup(heap_t *h)
{
  free(h->keys);
  free(h->vals);
  free(h);
}

int heap_empty(heap_t *h)
{
  return h->end == 0;
}

int heap_full(heap_t *h)
{
  return h->end >= h->size;
}

static uint32_t heap_parent(uint32_t i)
{
  return (i - 1) / 2;
}

static uint32_t heap_child(uint32_t i, uint32_t right)
{
  return 2 * i + 1 + right;
}

static void heap_swap(heap_t *h, uint32_t i, uint32_t j)
{
  uint64_t tmpi = h->keys[i];
  h->keys[i] = h->keys[j];
  h->keys[j] = tmpi;

  float tmpf = h->vals[i];
  h->vals[i] = h->vals[j];
  h->vals[j] = tmpf;
}

int heap_insert(heap_t *h, uint64_t key, float val)
{
  uint32_t pos = (h->end)++;
  h->keys[pos] = key;
  h->vals[pos] = val;

  while(pos >= 1)
  {
    uint32_t prt = heap_parent(pos);
    if(h->vals[prt] < h->vals[pos])
    {
      heap_swap(h, prt, pos);
      pos = prt;
    }
    else
    {
      break;
    }
  }
}

void heap_remove(heap_t *h, uint64_t *key, float *val)
{
  *key = h->keys[0];
  *val = h->vals[0];

  if(--(h->end) == 0) return;
  h->keys[0] = h->keys[h->end];
  h->vals[0] = h->vals[h->end];

  uint32_t pos = 0;
  while(1)
  {
    uint32_t largest = pos;
    uint32_t c0 = heap_child(pos, 0);
    uint32_t c1 = heap_child(pos, 1);
    if(c0 < h->end && h->vals[c0] > h->vals[pos]) largest = c0;
    if(c1 < h->end && h->vals[c1] > h->vals[largest]) largest = c1;
    if(largest != pos)
    {
      heap_swap(h, largest, pos);
      pos = largest;
    }
    else
      break;
  }
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
