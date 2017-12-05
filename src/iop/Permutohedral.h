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

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*******************************************************************
 * Hash table implementation for permutohedral lattice             *
 *                                                                 *
 * The lattice points are stored sparsely using a hash table.      *
 * The key for each point is its spatial location in the (d+1)-    *
 * dimensional space.                                              *
 *                                                                 *
 *******************************************************************/
template <int KD, int VD> class HashTablePermutohedral
{
public:
  /* Constructor
   *  kd_: the dimensionality of the position vectors on the hyperplane.
   *  vd_: the dimensionality of the value vectors
   */
  HashTablePermutohedral()
  {
    capacity = 1 << 15;
    capacity_bits = 0x7fff;
    filled = 0;
    entries = new Entry[capacity];
    keys = new short[KD * capacity / 2];
    values = new float[VD * capacity / 2];
    memset(values, 0, sizeof(float) * VD * capacity / 2);
  }

  ~HashTablePermutohedral()
  {
    delete[] entries;
    delete[] keys;
    delete[] values;
  }

  // Returns the number of vectors stored.
  int size()
  {
    return filled;
  }

  // Returns a pointer to the keys array.
  const short *getKeys()
  {
    return keys;
  }

  // Returns a pointer to the values array.
  float *getValues()
  {
    return values;
  }

  /* Returns the index into the hash table for a given key.
   *     key: a pointer to the position vector.
   *       h: hash of the position vector.
   *  create: a flag specifying whether an entry should be created,
   *          should an entry with the given key not found.
   */
  int lookupOffset(const short *key, size_t h, bool create = true)
  {

    // Double hash table size if necessary
    if(filled >= (capacity / 2) - 1)
    {
      grow();
    }

    // Find the entry with the given key
    while(1)
    {
      Entry e = entries[h];
      // check if the cell is empty
      if(e.keyIdx == -1)
      {
        if(!create) return -1; // Return not found.
        // need to create an entry. Store the given key.
        for(int i = 0; i < KD; i++) keys[filled * KD + i] = key[i];
        e.keyIdx = filled * KD;
        e.valueIdx = filled * VD;
        entries[h] = e;
        filled++;
        return e.valueIdx;
      }

      // check if the cell has a matching key
      bool match = true;
      for(int i = 0; i < KD && match; i++) match = keys[e.keyIdx + i] == key[i];
      if(match) return e.valueIdx;

      // increment the bucket with wraparound
      h++;
      if(h == capacity) h = 0;
    }
  }

  /* Looks up the value vector associated with a given key vector.
   *        k : pointer to the key vector to be looked up.
   *   create : true if a non-existing key should be created.
   */
  float *lookup(const short *k, bool create = true)
  {
    size_t h = hash(k) & capacity_bits;
    int offset = lookupOffset(k, h, create);
    if(offset < 0)
      return NULL;
    else
      return values + offset;
  };

  /* Hash function used in this implementation. A simple base conversion. */
  size_t hash(const short *key)
  {
    size_t k = 0;
    for(int i = 0; i < KD; i++)
    {
      k += key[i];
      k *= 2531011;
    }
    return k;
  }

private:
  /* Grows the size of the hash table */
  void grow()
  {
    size_t oldCapacity = capacity;
    capacity *= 2;
    capacity_bits = (capacity_bits << 1) | 1;

    // Migrate the value vectors.
    float *newValues = new float[VD * capacity / 2];
    memset(newValues, 0, sizeof(float) * VD * capacity / 2);
    memcpy(newValues, values, sizeof(float) * VD * filled);
    delete[] values;
    values = newValues;

    // Migrate the key vectors.
    short *newKeys = new short[KD * capacity / 2];
    memcpy(newKeys, keys, sizeof(short) * KD * filled);
    delete[] keys;
    keys = newKeys;

    Entry *newEntries = new Entry[capacity];

    // Migrate the table of indices.
    for(size_t i = 0; i < oldCapacity; i++)
    {
      if(entries[i].keyIdx == -1) continue;
      size_t h = hash(keys + entries[i].keyIdx) & capacity_bits;
      while(newEntries[h].keyIdx != -1)
      {
        h++;
        if(h == capacity) h = 0;
      }
      newEntries[h] = entries[i];
    }
    delete[] entries;
    entries = newEntries;
  }

  // Private struct for the hash table entries.
  struct Entry
  {
    Entry() : keyIdx(-1), valueIdx(-1)
    {
    }
    int keyIdx;
    int valueIdx;
  };

  short *keys;
  float *values;
  Entry *entries;
  size_t capacity, filled;
  unsigned long capacity_bits;
};

/******************************************************************
 * The algorithm class that performs the filter                   *
 *                                                                *
 * PermutohedralLattice::filter(...) does all the work.           *
 *                                                                *
 ******************************************************************/
template <int D, int VD> class PermutohedralLattice
{
public:
  /* Constructor
   *     d_ : dimensionality of key vectors
   *    vd_ : dimensionality of value vectors
   * nData_ : number of points in the input
   */
  PermutohedralLattice(size_t nData_, int nThreads_ = 1) : nData(nData_), nThreads(nThreads_)
  {

    // Allocate storage for various arrays
    float *scaleFactorTmp = new float[D];
    int *canonicalTmp = new int[(D + 1) * (D + 1)];

    replay = new ReplayEntry[nData * (D + 1)];

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

    hashTables = new HashTablePermutohedral<D, VD>[nThreads];
  }


  ~PermutohedralLattice()
  {
    delete[] scaleFactor;
    delete[] replay;
    delete[] canonical;
    delete[] hashTables;
  }


  /* Performs splatting with given position and value vectors */
  void splat(float *position, float *value, size_t replay_index, int thread_index = 0)
  {
    float elevated[D + 1];
    int greedy[D + 1];
    int rank[D + 1];
    float barycentric[D + 2];
    short key[D];

    // first rotate position into the (d+1)-dimensional hyperplane
    elevated[D] = -D * position[D - 1] * scaleFactor[D - 1];
    for(int i = D - 1; i > 0; i--)
      elevated[i] = (elevated[i + 1] - i * position[i - 1] * scaleFactor[i - 1]
                     + (i + 2) * position[i] * scaleFactor[i]);
    elevated[0] = elevated[1] + 2 * position[0] * scaleFactor[0];

    // prepare to find the closest lattice points
    float scale = 1.0f / (D + 1);

    // greedily search for the closest zero-colored lattice point
    int sum = 0;
    for(int i = 0; i <= D; i++)
    {
      float v = elevated[i] * scale;
      float up = ceilf(v) * (D + 1);
      float down = floorf(v) * (D + 1);

      if(up - elevated[i] < elevated[i] - down)
        greedy[i] = up;
      else
        greedy[i] = down;

      sum += greedy[i];
    }
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
    for(int remainder = 0; remainder <= D; remainder++)
    {
      // Compute the location of the lattice point explicitly (all but the last coordinate - it's redundant
      // because they sum to zero)
      for(int i = 0; i < D; i++) key[i] = greedy[i] + canonical[remainder * (D + 1) + rank[i]];

      // Retrieve pointer to the value at this vertex.
      float *val = hashTables[thread_index].lookup(key, true);

      // Accumulate values with barycentric weight.
      for(int i = 0; i < VD; i++) val[i] += barycentric[remainder] * value[i];

      // Record this interaction to use later when slicing
      replay[replay_index * (D + 1) + remainder].table = thread_index;
      replay[replay_index * (D + 1) + remainder].offset = val - hashTables[thread_index].getValues();
      replay[replay_index * (D + 1) + remainder].weight = barycentric[remainder];
    }
  }

  /* Merge the multiple threads' hash tables into the totals. */
  void merge_splat_threads(void)
  {
    if(nThreads <= 1) return;

    /* Merge the multiple hash tables into one, creating an offset remap table. */
    int **offset_remap = new int *[nThreads];
    for(int i = 1; i < nThreads; i++)
    {
      const short *oldKeys = hashTables[i].getKeys();
      const float *oldVals = hashTables[i].getValues();
      const int filled = hashTables[i].size();
      offset_remap[i] = new int[filled];
      for(int j = 0; j < filled; j++)
      {
        float *val = hashTables[0].lookup(oldKeys + j * D, true);
        const float *oldVal = oldVals + j * VD;
        for(int k = 0; k < VD; k++) val[k] += oldVal[k];
        offset_remap[i][j] = val - hashTables[0].getValues();
      }
    }

    /* Rewrite the offsets in the replay structure from the above generated table. */
    for(int i = 0; i < nData * (D + 1); i++)
      if(replay[i].table > 0) replay[i].offset = offset_remap[replay[i].table][replay[i].offset / VD];

    for(int i = 1; i < nThreads; i++) delete[] offset_remap[i];

    delete[] offset_remap;
  }

  /* Performs slicing out of position vectors. Note that the barycentric weights and the simplex
   * containing each position vector were calculated and stored in the splatting step.
   * We may reuse this to accelerate the algorithm. (See pg. 6 in paper.)
   */
  void slice(float *col, size_t replay_index)
  {
    float *base = hashTables[0].getValues();
    for(int j = 0; j < VD; j++) col[j] = 0;
    for(int i = 0; i <= D; i++)
    {
      ReplayEntry r = replay[replay_index * (D + 1) + i];
      for(int j = 0; j < VD; j++)
      {
        col[j] += r.weight * base[r.offset + j];
      }
    }
  }

  /* Performs a Gaussian blur along each projected axis in the hyperplane. */
  void blur()
  {
    // Prepare arrays
    float *newValue = new float[VD * hashTables[0].size()];
    float *oldValue = hashTables[0].getValues();
    float *hashTableBase = oldValue;

    float zero[VD];
    for(int k = 0; k < VD; k++) zero[k] = 0;

    // For each of d+1 axes,
    for(int j = 0; j <= D; j++)
    {
#ifdef _OPENMP
#pragma omp parallel for shared(j, oldValue, newValue, hashTableBase, zero)
#endif
      // For each vertex in the lattice,
      for(int i = 0; i < hashTables[0].size(); i++) // blur point i in dimension j
      {
        const short *key = hashTables[0].getKeys() + i * (D); // keys to current vertex
        short neighbor1[D + 1];
        short neighbor2[D + 1];
        for(int k = 0; k < D; k++)
        {
          neighbor1[k] = key[k] + 1;
          neighbor2[k] = key[k] - 1;
        }
        neighbor1[j] = key[j] - D;
        neighbor2[j] = key[j] + D; // keys to the neighbors along the given axis.

        float *oldVal = oldValue + i * VD;
        float *newVal = newValue + i * VD;

        float *vm1, *vp1;

        vm1 = hashTables[0].lookup(neighbor1, false); // look up first neighbor
        if(vm1)
          vm1 = vm1 - hashTableBase + oldValue;
        else
          vm1 = zero;

        vp1 = hashTables[0].lookup(neighbor2, false); // look up second neighbor
        if(vp1)
          vp1 = vp1 - hashTableBase + oldValue;
        else
          vp1 = zero;

        // Mix values of the three vertices
        for(int k = 0; k < VD; k++) newVal[k] = (0.25f * vm1[k] + 0.5f * oldVal[k] + 0.25f * vp1[k]);
      }
      float *tmp = newValue;
      newValue = oldValue;
      oldValue = tmp;
      // the freshest data is now in oldValue, and newValue is ready to be written over
    }

    // depending where we ended up, we may have to copy data
    if(oldValue != hashTableBase)
    {
      memcpy(hashTableBase, oldValue, hashTables[0].size() * VD * sizeof(float));
      delete[] oldValue;
    }
    else
    {
      delete[] newValue;
    }
  }

private:
  int nData;
  int nThreads;
  const float *scaleFactor;
  const int *canonical;

  // slicing is done by replaying splatting (ie storing the sparse matrix)
  struct ReplayEntry
  {
    int table;
    int offset;
    float weight;
  } *replay;

  HashTablePermutohedral<D, VD> *hashTables;
};

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
