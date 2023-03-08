/*
   this file has been taken from ImageStack (http://code.google.com/p/imagestack/)
   and adjusted slightly to fit darktable.

   ImageStack is released under the new bsd license:

Copyright (c) 2010, Andrew Adams
All rights reserved.

Redistribution and use in source and binary forms, with or without modification, are permitted provided that
the following conditions are met:

    * Redistributions of source code must retain the above copyright notice, this list of conditions and the
following disclaimer.
    * Redistributions in binary form must reproduce the above copyright notice, this list of conditions and
the following disclaimer in the documentation and/or other materials provided with the distribution.
    * Neither the name of the Stanford Graphics Lab nor the names of its contributors may be used to endorse
or promote products derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED
WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
DAMAGE.
*/

#pragma once

/*******************************************************************
 * Permutohedral Lattice implementation from:                      *
 * Fast High-Dimensional Filtering using the Permutohedral Lattice *
 * Andrew Adams, Jongmin Baek, Abe Davis                           *
 *******************************************************************/

#include <algorithm>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <iostream>

/*******************************************************************
 * Hash table implementation for permutohedral lattice             *
 *                                                                 *
 * The lattice points are stored sparsely using a hash table.      *
 * The key for each point is its spatial location in the (d+1)-    *
 * dimensional space.                                              *
 *                                                                 *
 *******************************************************************/
template <int VD>
struct HashTablePermutohedralValue {
    typedef HashTablePermutohedralValue Value;
    HashTablePermutohedralValue() = default;

    HashTablePermutohedralValue(int init)
    {
      for(int i = 0; i < VD; i++)
      {
        value[i] = init;
      }
    }

    HashTablePermutohedralValue(const Value &) = default; // let the compiler write the copy constructor

    Value &operator=(const Value &) = default;

    static void clear(float *val)
    {
      for(int i = 0; i < VD; i++)
	 val[i] = 0;
    }

    void add(const Value &other)
    {
      for(int i = 0; i < VD; i++)
      {
        value[i] += other.value[i];
      }
    }

    void add(const float *other, float weight)
    {
      for(int i = 0; i < VD; i++)
      {
        value[i] += weight * other[i];
      }
    }

    void addTo(float *dest, float weight) const
    {
      for(int i = 0; i < VD; i++)
      {
        dest[i] += weight * value[i];
      }
    }

    void mix(const Value *left, const Value *center, const Value *right)
    {
      for(int i = 0; i < VD; i++)
      {
        value[i] = (0.25f * left->value[i] + 0.5f * center->value[i] + 0.25f * right->value[i]);
      }
    }

    float value[VD]{};
};

template <>
struct HashTablePermutohedralValue<4> {
    typedef HashTablePermutohedralValue Value;
    HashTablePermutohedralValue() = default;

    HashTablePermutohedralValue(int init)
    {
    for_four_channels(i)
      value[i] = init;
    }

    HashTablePermutohedralValue(const Value &) = default; // let the compiler write the copy constructor

    Value &operator=(const Value &) = default;

    static void clear(float *val)
    {
    for_four_channels(i)
      val[i] = 0;
    }

    void add(const Value &other)
    {
    for_four_channels(i)
      value[i] += other.value[i];
    }

    void add(const float *other, float weight)
    {
    for_four_channels(i)
      value[i] += weight * other[i];
    }

    void addTo(float *dest, float weight) const
    {
    for_four_channels(i, aligned(dest))
      dest[i] += weight * value[i];
    }

    void mix(const Value *left, const Value *center, const Value *right)
    {
    for_four_channels(i)
      value[i] = (0.25f * left->value[i] + 0.5f * center->value[i] + 0.25f * right->value[i]);
    }

    dt_aligned_pixel_t value;
};

template <int KD, int VD> class HashTablePermutohedral
{
public:
  // Struct for a key
  struct Key
  {
    Key() = default;

    Key(const Key &origin, int dim, int direction) // construct neighbor in dimension 'dim'
    {
#ifdef _OPENMP
#pragma omp simd
#endif
      for(int i = 0; i < KD; i++)
	 key[i] = origin.key[i] + direction;
      key[dim] = origin.key[dim] - direction * KD;
      setHash();
    }

    Key(const Key &) = default; // let the compiler write the copy constructor

    Key &operator=(const Key &) = default;

    void setKey(int idx, short val)
    {
      key[idx] = val;
    }

    void setHash()
    {
      size_t k = 0;
      for(int i = 0; i < KD; i++)
      {
        k += key[i];
        k *= 2531011;
      }
      hash = (unsigned)k;
    }

    bool operator==(const Key &other) const
    {
      if(hash != other.hash) return false;
#if 1
      return memcmp(key, other.key, sizeof(key)) == 0;
#else
      for(int i = 0; i < KD; i++)
      {
        if(key[i] != other.key[i]) return false;
      }
      return true;
#endif
    }

    unsigned hash;    // cache the hash value for this key
    short key[KD];    // key is a KD-dimensional vector
  };

public:
  // Struct for an associated value
  typedef HashTablePermutohedralValue<VD> Value;

public:
  /* Constructor
   *  kd_: the dimensionality of the position vectors on the hyperplane.
   *  vd_: the dimensionality of the value vectors
   */
  HashTablePermutohedral(size_t num_entries = 0)
  {
    capacity = 0;
    capacity_bits = 1;
    alloc_entries = 0;
    filled = 0;
    entries = nullptr;
    keys = nullptr;
    values = nullptr;
  }

  HashTablePermutohedral(const HashTablePermutohedral &) = delete;

  ~HashTablePermutohedral()
  {
    delete[] entries;
    delete[] keys;
    delete[] values;
  }

  HashTablePermutohedral &operator=(const HashTablePermutohedral &) = delete;

  // Returns the number of vectors stored.
  size_t size() const
  {
    return filled;
  }

  size_t maxFill() const
  {
    return alloc_entries;
  }

  // Returns a pointer to the keys array.
  const Key *getKeys() const
  {
    return keys;
  }

  // Returns a pointer to the values array.
  Value *getValues() const
  {
    return values;
  }

  /* Returns the index into the hash table for a given key.
   *     key: a reference to the position vector.
   *  create: a flag specifying whether an entry should be created,
   *          should an entry with the given key not found.
   */
  int lookupOffset(const Key &key, bool create = true)
  {
    size_t h = key.hash & capacity_bits;
    // Find the entry with the given key
    while(1)
    {
      Entry e = entries[h];
      // check if the cell is empty
      if(e.keyIdx == -1)
      {
        if(!create) return -1; // Return not found.
        // Double hash table size if necessary
        if(filled >= maxFill())
        {
          grow();
        }
        // need to create an entry. Store the given key.
        keys[filled] = key;
        entries[h].keyIdx = filled;
        return filled++;
      }

      // check if the cell has a matching key
      if(keys[e.keyIdx] == key) return e.keyIdx;

      // increment the bucket with wraparound
      h = (h + 1) & capacity_bits;
    }
  }

  /* Looks up the value vector associated with a given key vector.
   *        k : reference to the key vector to be looked up.
   *   create : true if a non-existing key should be created.
   */
  Value *lookup(const Key &k, bool create = true)
  {
    int offset = lookupOffset(k, create);
    return (offset < 0) ? nullptr : values + offset;
  };

  /* Grows the size of the hash table */
  void grow(int order = 1)
  {
    if(order > 0)
    {
      auto_grow++;
      growExact(capacity << (order-1));
    }
  }

  /* initialize the hash table so that it can hold exactly num_entries without resizing */
  void setSize(size_t num_entries)
  {
    capacity = 1 << 15;
    capacity_bits = 0x7fff;
    if(num_entries == 0)
      num_entries = capacity/2;
    else
    {
      while (capacity < 2*num_entries)
      {
      capacity <<= 1;
      capacity_bits = (capacity_bits << 1) | 1;
      }
    }
    alloc_entries = num_entries;
    filled = 0;
    entries = new Entry[capacity];
    keys = new Key[maxFill()];
    values = new Value[maxFill()];
    init_alloc = total_alloc = capacity * sizeof(Entry) + maxFill() * sizeof(Key) + maxFill() * sizeof(Value);
  }
   
  /* grow the size of the hash table so that it can hold exactly num_entries
   * without requiring resizing.  The actual index array will be rounded up
   * to the next higher power of two.
   */
  void growExact(size_t num_entries)
  {
    size_t oldCapacity = capacity;
    while(capacity < num_entries * 2)
    {
      capacity *= 2;
      capacity_bits = (capacity_bits << 1) | 1;
    }
    alloc_entries = num_entries;

    // Migrate the value vectors.
    Value *newValues = new Value[maxFill()];
    std::copy(values, values + filled, newValues);
    delete[] values;
    values = newValues;

    // Migrate the key vectors.
    Key *newKeys = new Key[maxFill()];
    std::copy(keys, keys + filled, newKeys);
    delete[] keys;
    keys = newKeys;

    Entry *newEntries = new Entry[capacity];

    // Migrate the table of indices.
    for(size_t i = 0; i < oldCapacity; i++)
    {
      if(entries[i].keyIdx == -1) continue;
      size_t h = keys[entries[i].keyIdx].hash & capacity_bits;
      while(newEntries[h].keyIdx != -1)
      {
        h = (h + 1) & capacity_bits;
      }
      newEntries[h] = entries[i];
    }
    delete[] entries;
    entries = newEntries;
    total_alloc = capacity * sizeof(Entry) + maxFill() * sizeof(Key) + maxFill() * sizeof(Value);
  }
   
private:
  // Private struct for the hash table entries.
  struct Entry
  {
    int keyIdx{ -1 };
  };

  Key *keys;
  Value *values;
  Entry *entries;
  size_t capacity, filled, alloc_entries;
  unsigned long capacity_bits;
public:
  size_t init_alloc { 0 };
  size_t total_alloc { 0 };
  size_t auto_grow { 0 };
};

/******************************************************************
 * The algorithm class that performs the filter                   *
 *                                                                *
 * PermutohedralLattice::splat(...) and                           *
 * PermutohedralLattic::slice() do almost all the work.           *
 *                                                                *
 ******************************************************************/
template <int D, int VD> class PermutohedralLattice
{
private:
  // short-hand for types we use
  typedef HashTablePermutohedral<D, VD> HashTable;
  typedef typename HashTable::Key Key;
  typedef typename HashTable::Value Value;

public:
  /* Constructor
   *     d_ : dimensionality of key vectors
   *    vd_ : dimensionality of value vectors
   * nData_ : number of points in the input
   */
  PermutohedralLattice(size_t nData_, int nThreads_ = 1, size_t grid_points = ~0L) : nData(nData_), nThreads(nThreads_)
  {
    // Allocate storage for various arrays
    float *scaleFactorTmp = new float[D];
    int *canonicalTmp = new int[(D + 1) * (D + 1)];

    replay = new ReplayEntry[nData];

    // compute the coordinates of the canonical simplex, in which
    // the difference between a contained point and the zero
    // remainder vertex is always in ascending order. (See pg.4 of paper.)
    for(int i = 0; i <= D; i++)
    {
      for(int j = 0; j <= D - i; j++) canonicalTmp[i * (D + 1) + j] = i;
      for(int j = D - i + 1; j <= D; j++) canonicalTmp[i * (D + 1) + j] = i - (D + 1);
    }
    canonical = canonicalTmp;

    // Compute parts of the rotation matrix E. (See pg.4-5 of paper.)
    for(int i = 0; i < D; i++)
    {
      // the diagonal entries for normalization
      scaleFactorTmp[i] = 1.0f / (sqrtf((float)(i + 1) * (i + 2)));

      /* We presume that the user would like to do a Gaussian blur of standard deviation
       * 1 in each dimension (or a total variance of d, summed over dimensions.)
       * Because the total variance of the blur performed by this algorithm is not d,
       * we must scale the space to offset this.
       *
       * The total variance of the algorithm is (See pg.6 and 10 of paper):
       *  [variance of splatting] + [variance of blurring] + [variance of splatting]
       *   = d(d+1)(d+1)/12 + d(d+1)(d+1)/2 + d(d+1)(d+1)/12
       *   = 2d(d+1)(d+1)/3.
       *
       * So we need to scale the space by (d+1)sqrt(2/3).
       */
      scaleFactorTmp[i] *= (D + 1) * sqrtf(2.0 / 3);
    }
    scaleFactor = scaleFactorTmp;

    size_t effective_MP = estimatedHashEntries(grid_points, nData);
    size_t points = ((D+1) * nData) < effective_MP ? ((D+1) * nData) : effective_MP;

    hashTables = new HashTable[nThreads];
    for(int i = 0; i < nThreads; i++)
    {
       hashTables[i].setSize(points / nThreads);
    }
  }

  PermutohedralLattice(const PermutohedralLattice &) = delete;

  ~PermutohedralLattice()
  {
    delete[] scaleFactor;
    delete[] replay;
    delete[] canonical;
    delete[] hashTables;
  }

  PermutohedralLattice &operator=(const PermutohedralLattice &) = delete;

  /* compute the expected number of hash table entries we will need */
  static size_t estimatedHashEntries(size_t grid_points, size_t num_pixels)
  {
    // as the number of grid points increases, the number which
    // actually occur in the image becomes an ever-smaller
    // percentage.  Scale the grid points to take account of this.
    // Empirically, it appears that the number of actually-used
    // points roughly doubles for every order of magnitude increase
    // in total grid points.
    double points_per_MP = MAX(0.1, grid_points / (float)num_pixels);
    double base_factor = 50.0; // absolute scaling factor
    double scaled = pow(1.8,log10(points_per_MP/base_factor));
    size_t eff_pixels = (size_t)(scaled * num_pixels);
    return ((D+1) * num_pixels) < eff_pixels ? ((D+1) * num_pixels) : eff_pixels;
  }

  /* compute the expected bytes of storage needed */
  static size_t estimatedBytes(size_t grid_points, size_t num_pixels)
  {
     size_t hash_entries = estimatedHashEntries(grid_points, num_pixels);
     size_t round_up = 1;
     while (round_up < 2*hash_entries) round_up <<= 1;
     // we need to store not only the Key, Value, and Entry arrays, we
     // also need an additional copy of the Value array while blurring
     // and storage for the remapping array while merging
     size_t mergesize = hash_entries * 2 * (sizeof(Value)+sizeof(Key)) + round_up * sizeof(int);
     size_t blursize = hash_entries * (2*sizeof(Value)+sizeof(Key)) + (hash_entries+round_up) * sizeof(int);
     return MAX(mergesize, blursize);
  }

  /* Performs splatting with given position and value vectors */
  void splat(float *position, float *value, size_t replay_index, int thread_index = 0) const
  {
    DT_ALIGNED_PIXEL float elevated[D + 1];
    DT_ALIGNED_PIXEL int greedy[D + 1];
    DT_ALIGNED_PIXEL int rank[D + 1];
    DT_ALIGNED_PIXEL float barycentric[D + 2];
    Key key;

    // first rotate position into the (d+1)-dimensional hyperplane
    elevated[D] = -D * position[D - 1] * scaleFactor[D - 1];
    for(int i = D - 1; i > 0; i--)
      elevated[i]
          = (elevated[i + 1] - i * position[i - 1] * scaleFactor[i - 1] + (i + 2) * position[i] * scaleFactor[i]);
    elevated[0] = elevated[1] + 2 * position[0] * scaleFactor[0];

    // prepare to find the closest lattice points
    constexpr float scale = 1.0f / (D + 1);

    // greedily search for the closest zero-colored lattice point
#ifdef _OPENMP
#pragma omp simd
#endif
    for(size_t i = 0; i <= D; i++)
    {
      float v = elevated[i] * scale;
      float up = ceilf(v) * (D + 1);
      float down = floorf(v) * (D + 1);

      if(up - elevated[i] < elevated[i] - down)
        greedy[i] = up;
      else
        greedy[i] = down;
    }
    int sum = 0;
    for(size_t i = 0; i <= D; i++)
       sum += greedy[i];

    sum /= D + 1;

    // rank differential to find the permutation between this simplex and the canonical one.
    // (See pg. 3-4 in paper.)
    memset(rank, 0, sizeof rank);
    for(int i = 0; i < D; i++)
      for(int j = i + 1; j <= D; j++)
        if(elevated[i] - greedy[i] < elevated[j] - greedy[j])
          rank[i]++;
        else
          rank[j]++;

    if(sum > 0)
    {
      // sum too large - the point is off the hyperplane.
      // need to bring down the ones with the smallest differential
      for(int i = 0; i <= D; i++)
      {
        if(rank[i] >= D + 1 - sum)
        {
          greedy[i] -= D + 1;
          rank[i] += sum - (D + 1);
        }
        else
          rank[i] += sum;
      }
    }
    else if(sum < 0)
    {
      // sum too small - the point is off the hyperplane
      // need to bring up the ones with largest differential
      for(int i = 0; i <= D; i++)
      {
        if(rank[i] < -sum)
        {
          greedy[i] += D + 1;
          rank[i] += (D + 1) + sum;
        }
        else
          rank[i] += sum;
      }
    }

    // Compute barycentric coordinates (See pg.10 of paper.)
    memset(barycentric, 0, sizeof barycentric);
    for(int i = 0; i <= D; i++)
    {
      barycentric[D - rank[i]] += (elevated[i] - greedy[i]) * scale;
      barycentric[D + 1 - rank[i]] -= (elevated[i] - greedy[i]) * scale;
    }
    barycentric[0] += 1.0f + barycentric[D + 1];

    // Splat the value into each vertex of the simplex, with barycentric weights.
    replay[replay_index].table = thread_index;
    for(int remainder = 0; remainder <= D; remainder++)
    {
      // Compute the location of the lattice point explicitly (all but the last coordinate - it's redundant
      // because they sum to zero)
      for(int i = 0; i < D; i++) key.key[i] = greedy[i] + canonical[remainder * (D + 1) + rank[i]];
      key.setHash();

      // Retrieve pointer to the value at this vertex.
      Value *val = hashTables[thread_index].lookup(key, true);

      // Accumulate values with barycentric weight.
      val->add(value, barycentric[remainder]);

      // Record this interaction to use later when slicing
      replay[replay_index].offset[remainder] = val - hashTables[thread_index].getValues();
      replay[replay_index].weight[remainder] = barycentric[remainder];
    }
  }

  /* Merge the multiple threads' hash tables into the totals. */
  void merge_splat_threads()
  {
    if(nThreads <= 1) return;

    /* Because growing the hash table is expensive, we want to avoid having to do it multiple times.
     * Only a small percentage of entries in the individual hash tables have the same key, so we
     * won't waste much space if we simply grow the destination table enough to hold the sum of the
     * entries in the individual tables
     */
    size_t alloc_entries = hashTables[0].maxFill();
    size_t total_bytes = 0;
    size_t total_grows = hashTables[0].auto_grow;
    size_t init_bytes = hashTables[0].init_alloc;
    size_t total_entries = hashTables[0].size();
    for(int i = 1; i < nThreads; i++)
    {
       alloc_entries += hashTables[i].maxFill();
       total_entries += hashTables[i].size();
       init_bytes += hashTables[i].init_alloc;
       total_bytes += hashTables[i].total_alloc;
       total_grows += hashTables[i].auto_grow;
    }
    hashTables[0].growExact(total_entries);
    total_bytes += hashTables[0].total_alloc;
    /* Merge the multiple hash tables into one, creating an offset remap table. */
    int **offset_remap = new int *[nThreads];
    size_t remap_bytes = 0;
    for(int i = 1; i < nThreads; i++)
    {
      const Key *oldKeys = hashTables[i].getKeys();
      const Value *oldVals = hashTables[i].getValues();
      const size_t filled = hashTables[i].size();
      offset_remap[i] = new int[filled];
      remap_bytes += filled * sizeof(int);
      for(size_t j = 0; j < filled; j++)
      {
        Value *val = hashTables[0].lookup(oldKeys[j], true);
        val->add(oldVals[j]);
        offset_remap[i][j] = val - hashTables[0].getValues();
      }
    }
    if(darktable.unmuted & DT_DEBUG_MEMORY)
    {
       float fill_factor = 100.0f * total_entries / alloc_entries;
       std::cerr << "[permutohedral] hash tables " << total_bytes << " bytes (" << init_bytes
		 << " initially), " << total_entries << " entries" << std::endl
		 << "[permutohedral] tables grew " << total_grows << " times, replay using "
		 << (sizeof(ReplayEntry)*nData) << " bytes for " << nData << " pixels" << std::endl
		 << "[permutohedral] fill factor " << fill_factor << "%, remap using "
		 << remap_bytes << " bytes," << std::endl;
    }

    /* Rewrite the offsets in the replay structure from the above generated table. */
#ifdef _OPENMP
#pragma omp parallel for default(none) if(nData >= 100000) \
   dt_omp_firstprivate(nData, replay, offset_remap) \
   schedule(static)
#endif
    for(size_t i = 0; i < nData; i++)
    {
      if(replay[i].table > 0)
      {
        for(int dim = 0; dim <= D; dim++)
          replay[i].offset[dim] = offset_remap[replay[i].table][replay[i].offset[dim]];
      }
    }

    for(int i = 1; i < nThreads; i++) delete[] offset_remap[i];
    delete[] offset_remap;
  }

  /* Performs slicing out of position vectors. Note that the barycentric weights and the simplex
   * containing each position vector were calculated and stored in the splatting step.
   * We may reuse this to accelerate the algorithm. (See pg. 6 in paper.)
   */
  void slice(float *col, size_t replay_index) const
  {
    const Value *base = hashTables[0].getValues();
    Value::clear(col);
    ReplayEntry &r = replay[replay_index];
    for(int i = 0; i <= D; i++)
    {
      base[r.offset[i]].addTo(col, r.weight[i]);
    }
  }

  /* Performs a Gaussian blur along each projected axis in the hyperplane. */
  void blur() const
  {
    // Prepare arrays
    Value *newValue = new Value[hashTables[0].size()];
    Value *oldValue = hashTables[0].getValues();
    const Value *hashTableBase = oldValue;
    const Key *keyBase = hashTables[0].getKeys();
    const Value zero{ 0 };
    const Value *const zeroPtr = &zero;

    if (darktable.unmuted & DT_DEBUG_MEMORY)
       std::cerr << "[permutohedral] blur using " << (sizeof(Value)*hashTables[0].size())
		 << " bytes for newValue"<<std::endl;

    // For each of d+1 axes,
    for(int j = 0; j <= D; j++)
    {
#ifdef _OPENMP
#pragma omp parallel for default(none) \
    dt_omp_firstprivate(j, oldValue, newValue, hashTableBase, hashTables, keyBase, zeroPtr) \
    schedule(static)
#endif
      // For each vertex in the lattice,
      for(size_t i = 0; i < hashTables[0].size(); i++) // blur point i in dimension j
      {
        const Key &key = keyBase[i]; // keys to current vertex
        // construct keys to the neighbors along the given axis.
        Key neighbor1(key, j, +1);
        Key neighbor2(key, j, -1);

        const Value *oldVal = oldValue + i;

        const Value *vm1 = hashTables[0].lookup(neighbor1, false); // look up first neighbor
        vm1 = vm1 ? vm1 - hashTableBase + oldValue : zeroPtr;

        const Value *vp1 = hashTables[0].lookup(neighbor2, false); // look up second neighbor
        vp1 = vp1 ? vp1 - hashTableBase + oldValue : zeroPtr;

        // Mix values of the three vertices
        newValue[i].mix(vm1, oldVal, vp1);
      }
      std::swap(newValue, oldValue);
      // the freshest data is now in oldValue, and newValue is ready to be written over
    }

    // depending where we ended up, we may have to copy data
    if(oldValue != hashTableBase)
    {
      std::copy(oldValue, oldValue + hashTables[0].size(), hashTables[0].getValues());
      delete[] oldValue;
    }
    else
    {
      delete[] newValue;
    }
  }

private:
  size_t nData;
  int nThreads;
  const float *scaleFactor;
  const int *canonical;

  // slicing is done by replaying splatting (ie storing the sparse matrix)
  struct ReplayEntry
  {
    // since every dimension of a lattice point gets handled by the same thread,
    // we only need to store the id of the hash table once, instead of for each dimension
    int table;
    int offset[D + 1];
    float weight[D + 1];
  } * replay;

  HashTable *hashTables;
};

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

