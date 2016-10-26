/*
    This file is part of darktable,
    copyright (c) 2009--2012 johannes hanika.

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

//Matrix class
#ifndef MATRIX_H
#define MATRIX_H
#include <limits>
#include <algorithm>
#include <math.h>

#ifdef _OPENMP
#include <omp.h>
#endif

using std::vector;

#include "assert.h" //Included later so NDEBUG has an effect

template <class T> class matrix
{
private:
  T *data;
  int num_rows;
  int num_cols;
  inline void slow_transpose_to(const matrix<T> &target) const;
  inline void fast_transpose_to(const matrix<T> &target) const;
  inline void transpose4x4_SSE(float *A, float *B, const int lda, const int ldb) const;
  inline void transpose_block_SSE4x4(float *A, float *B, const int n, const int m, const int lda,
                                     const int ldb, const int block_size) const;
  inline void transpose_scalar_block(float *A, float *B, const int lda, const int ldb,
                                     const int block_size) const;
  inline void transpose_block(float *A, float *B, const int n, const int m, const int lda, const int ldb,
                              const int block_size) const;

public:
  matrix(const int nrows = 0, const int ncols = 0);
  matrix(const matrix<T> &toCopy);
  ~matrix();
  void set_size(const int nrows, const int ncols);
  void free();
  int nr() const;
  int nc() const;
  T &operator()(const int row, const int col) const;
  // template <class U> //Never gets called if use matrix<U>
  matrix<T> &operator=(const matrix<T> &toCopy);
  template <class U> matrix<T> &operator=(const U value);
  template <class U> const matrix<T> add(const matrix<U> &rhs) const;
  template <class U> const matrix<T> add(const U value) const;
  template <class U> const matrix<T> &add_this(const U value);
  template <class U> const matrix<T> subtract(const matrix<U> &rhs) const;
  template <class U> const matrix<T> subtract(const U value) const;
  template <class U> const matrix<T> pointmult(const matrix<U> &rhs) const;
  template <class U> const matrix<T> mult(const U value) const;
  template <class U> const matrix<T> &mult_this(const U value);
  template <class U> const matrix<T> divide(const U value) const;
  inline void transpose_to(const matrix<T> &target) const;
  double sum();
  T max();
  T min();
  double mean();
  double variance();
};

template <class T> double sum(matrix<T> &mat);

template <class T> T max(matrix<T> &mat);

template <class T> T min(matrix<T> &mat);

template <class T> double mean(matrix<T> &mat);

template <class T> double variance(matrix<T> &mat);

template <class T, class U> const matrix<T> operator+(const matrix<T> &mat1, const matrix<U> &mat2);

template <class T, class U> const matrix<T> operator+(const U value, const matrix<T> &mat);

template <class T, class U> const matrix<T> operator+(const matrix<T> &mat, const U value);

template <class T, class U> const matrix<T> operator+=(matrix<T> &mat, const U value);

template <class T, class U> const matrix<T> operator-(const matrix<T> &mat1, const matrix<U> &mat2);

template <class T, class U> const matrix<T> operator-(const matrix<T> &mat, const U value);

template <class T, class U> const matrix<T> operator%(const matrix<T> &mat1, const matrix<U> &mat2);

template <class T, class U> const matrix<T> operator*(const U value, const matrix<T> &mat);

template <class T, class U> const matrix<T> operator*(const matrix<T> &mat, const U value);

template <class T, class U> const matrix<T> operator*=(matrix<T> &mat, const U value);

template <class T, class U> const matrix<T> operator/(const matrix<T> &mat1, const U value);

// IMPLEMENTATION:

template <class T> matrix<T>::matrix(const int nrows, const int ncols)
{
  assert(nrows >= 0 && ncols >= 0);
  num_rows = nrows;
  num_cols = ncols;
  if(nrows == 0 || ncols == 0)
  {
    data = nullptr;
  }
  else
  {
    data = new T[nrows * ncols];
  }
}

template <class T> matrix<T>::matrix(const matrix<T> &toCopy)
{
  if(this == &toCopy) return;

  num_rows = toCopy.num_rows;
  num_cols = toCopy.num_cols;
  data = new T[num_rows * num_cols];

#ifdef _OPENMP
#pragma omp parallel for
#endif
  for(int row = 0; row < num_rows; row++)
    for(int col = 0; col < num_cols; col++) data[row * num_cols + col] = toCopy.data[row * num_cols + col];
}

template <class T> matrix<T>::~matrix()
{
  delete[] data;
}

template <class T> void matrix<T>::set_size(const int nrows, const int ncols)
{
  assert(nrows >= 0 && ncols >= 0);
  num_rows = nrows;
  num_cols = ncols;
  delete[] data;
  data = new(std::nothrow) T[nrows * ncols];
  //if(data == nullptr) std::cout << "matrix::set_size memory could not be alloc'd" << std::endl;
}

template <class T> void matrix<T>::free()
{
  set_size(0, 0);
}

template <class T> int matrix<T>::nr() const
{
  return num_rows;
}

template <class T> int matrix<T>::nc() const
{
  return num_cols;
}

template <class T> T &matrix<T>::operator()(const int row, const int col) const
{
  assert(row < num_rows && col < num_cols);
  return data[row * num_cols + col];
}

template <class T> // template<class U>
matrix<T> &matrix<T>::operator=(const matrix<T> &toCopy)
{
  if(this == &toCopy) return *this;

  set_size(toCopy.nr(), toCopy.nc());

#ifdef _OPENMP
#pragma omp parallel for shared(toCopy)
#endif
  for(int row = 0; row < num_rows; row++)
    for(int col = 0; col < num_cols; col++) data[row * num_cols + col] = toCopy.data[row * num_cols + col];
  return *this;
}

template <class T> template <class U> matrix<T> &matrix<T>::operator=(const U value)
{

#ifdef _OPENMP
#pragma omp parallel for
#endif
  for(int row = 0; row < num_rows; row++)
    for(int col = 0; col < num_cols; col++) data[row * num_cols + col] = value;
  return *this;
}

template <class T> template <class U> const matrix<T> matrix<T>::add(const matrix<U> &rhs) const
{
  assert(num_rows == rhs.num_rows && num_cols == rhs.num_cols);
  matrix<T> result(num_rows, num_cols);

  T *pdata = data;
  int pnum_cols = num_cols;
#ifdef _OPENMP
#pragma omp parallel for shared(pdata, pnum_cols, result, rhs)
#endif
  for(int row = 0; row < num_rows; row++)
    for(int col = 0; col < num_cols; col++)
      result.data[row * num_cols + col] = data[row * num_cols + col] + rhs.data[row * num_cols + col];
  return result;
}

template <class T> template <class U> const matrix<T> matrix<T>::add(const U value) const
{
  matrix<T> result(num_rows, num_cols);

  T *pdata = data;
  int pnum_cols = num_cols;
#ifdef _OPENMP
#pragma omp parallel for shared(pdata, pnum_cols)
#endif
  for(int row = 0; row < num_rows; row++)
    for(int col = 0; col < num_cols; col++)
      result.data[row * num_cols + col] = data[row * num_cols + col] + value;
  return result;
}

template <class T> template <class U> const matrix<T> &matrix<T>::add_this(const U value)
{
  T *pdata = data;
  int pnum_cols = num_cols;
#ifdef _OPENMP
#pragma omp parallel for shared(pdata, pnum_cols)
#endif
  for(int row = 0; row < num_rows; row++)
    for(int col = 0; col < num_cols; col++) data[row * num_cols + col] += value;
  return *this;
}

template <class T> template <class U> const matrix<T> matrix<T>::subtract(const matrix<U> &rhs) const
{
  assert(num_rows == rhs.num_rows && num_cols == rhs.num_cols);
  matrix<T> result(num_rows, num_cols);

  T *pdata = data;
  int pnum_cols = num_cols;
#ifdef _OPENMP
#pragma omp parallel for shared(pdata, pnum_cols, result, rhs)
#endif
  for(int row = 0; row < num_rows; row++)
    for(int col = 0; col < num_cols; col++)
      result.data[row * num_cols + col] = data[row * num_cols + col] - rhs.data[row * num_cols + col];
  return result;
}

template <class T> template <class U> const matrix<T> matrix<T>::subtract(const U value) const
{
  matrix<T> result(num_rows, num_cols);

  T *pdata = data;
  int pnum_cols = num_cols;
#ifdef _OPENMP
#pragma omp parallel for shared(pdata, pnum_cols, result)
#endif
  for(int row = 0; row < num_rows; row++)
    for(int col = 0; col < num_cols; col++)
      result.data[row * num_cols + col] = data[row * num_cols + col] + value;
  return result;
}

template <class T> template <class U> const matrix<T> matrix<T>::pointmult(const matrix<U> &rhs) const
{
  matrix<T> result(num_rows, num_cols);

#ifdef _OPENMP
#pragma omp parallel for shared(result, rhs)
#endif
  for(int row = 0; row < num_rows; row++)
    for(int col = 0; col < num_cols; col++)
      result.data[row * num_cols + col] = data[row * num_cols + col] * rhs.data[row * num_cols + col];
  return result;
}

template <class T> template <class U> const matrix<T> matrix<T>::mult(const U value) const
{
  matrix<T> result(num_rows, num_cols);

#ifdef _OPENMP
#pragma omp parallel for shared(result)
#endif
  for(int row = 0; row < num_rows; row++)
    for(int col = 0; col < num_cols; col++)
      result.data[row * num_cols + col] = data[row * num_cols + col] * value;
  return result;
}

template <class T> template <class U> const matrix<T> &matrix<T>::mult_this(const U value)
{

#ifdef _OPENMP
#pragma omp parallel for
#endif
  for(int row = 0; row < num_rows; row++)
    for(int col = 0; col < num_cols; col++) data[row * num_cols + col] *= value;
  return *this;
}

template <class T> template <class U> const matrix<T> matrix<T>::divide(const U value) const
{
  matrix<T> result(num_rows, num_cols);

#ifdef _OPENMP
#pragma omp parallel for shared(result)
#endif
  for(int row = 0; row < num_rows; row++)
    for(int col = 0; col < num_cols; col++)
      result.data[row * num_cols + col] = data[row * num_cols + col] / value;
  return result;
}

template <class T> inline void matrix<T>::slow_transpose_to(const matrix<T> &target) const
{
  assert(target.num_rows == num_cols && target.num_cols == num_rows);

#ifdef _OPENMP
#pragma omp parallel for shared(target)
#endif
  for(int row = 0; row < num_rows; row++)
    for(int col = 0; col < num_cols; col++) target.data[col * num_rows + row] = data[row * num_cols + col];
}

/*
template <> inline void matrix<float>::fast_transpose_to(const matrix<float> &target) const
{
  assert(target.num_rows == num_cols && target.num_cols == num_rows);

  transpose_block_SSE4x4(data, target.data, num_rows, num_cols, num_cols, num_rows, 16);
}
*/

// There is no fast transpose in the general case
template <class T> inline void matrix<T>::fast_transpose_to(const matrix<T> &target) const
{
  slow_transpose_to(target);
}

template <class T> inline void matrix<T>::transpose_to(const matrix<T> &target) const
{
  slow_transpose_to(target);
}

/*
template <> inline void matrix<float>::transpose_to(const matrix<float> &target) const
{
  // Fast transpose only work with matricies with dimensions of multiples of 16
  if((num_rows % 16 != 0) || (num_cols % 16 != 0))
    slow_transpose_to(target);
  else
    fast_transpose_to(target);
}
*/

/*
template <class T>
inline void matrix<T>::transpose4x4_SSE(float *A, float *B, const int lda, const int ldb) const
{
  __m128 row1 = _mm_load_ps(&A[0 * lda]);
  __m128 row2 = _mm_load_ps(&A[1 * lda]);
  __m128 row3 = _mm_load_ps(&A[2 * lda]);
  __m128 row4 = _mm_load_ps(&A[3 * lda]);
  _MM_TRANSPOSE4_PS(row1, row2, row3, row4);
  _mm_store_ps(&B[0 * ldb], row1);
  _mm_store_ps(&B[1 * ldb], row2);
  _mm_store_ps(&B[2 * ldb], row3);
  _mm_store_ps(&B[3 * ldb], row4);
}
*/

/*
// block_size = 16 works best
template <class T>
inline void matrix<T>::transpose_block_SSE4x4(float *A, float *B, const int n, const int m, const int lda,
                                              const int ldb, const int block_size) const
{
#ifdef _OPENMP
#pragma omp parallel for
#endif
  for(int i = 0; i < n; i += block_size)
    for(int j = 0; j < m; j += block_size)
    {
      int max_i2 = i + block_size < n ? i + block_size : n;
      int max_j2 = j + block_size < m ? j + block_size : m;
      for(int i2 = i; i2 < max_i2; i2 += 4)
        for(int j2 = j; j2 < max_j2; j2 += 4)
          transpose4x4_SSE(&A[i2 * lda + j2], &B[j2 * ldb + i2], lda, ldb);
    }
}
*/

template <class T>
inline void matrix<T>::transpose_scalar_block(float *A, float *B, const int lda, const int ldb,
                                              const int block_size) const
{
#ifdef _OPENMP
#pragma omp parallel for
#endif
  for(int i = 0; i < block_size; i++)
  {
    for(int j = 0; j < block_size; j++)
    {
      B[j * ldb + i] = A[i * lda + j];
    }
  }
}

template <class T>
inline void matrix<T>::transpose_block(float *A, float *B, const int n, const int m, const int lda,
                                       const int ldb, const int block_size) const
{
#ifdef _OPENMP
#pragma omp parallel for
#endif
  for(int i = 0; i < n; i += block_size)
  {
    for(int j = 0; j < m; j += block_size)
    {
      transpose_scalar_block(&A[i * lda + j], &B[j * ldb + i], lda, ldb, block_size);
    }
  }
}

template <class T> double matrix<T>::sum()
{
  double sum = 0;

#ifdef _OPENMP
#pragma omp parallel for reduction(+ : sum)
#endif
  for(int row = 0; row < num_rows; row++)
    for(int col = 0; col < num_cols; col++) sum += data[row * num_cols + col];
  return sum;
}

template <class T> T matrix<T>::max()
{
  T shared_max;

#ifdef _OPENMP
#pragma omp parallel
#endif
  {
    T max = std::numeric_limits<T>::min();
#ifdef _OPENMP
#pragma omp for schedule(dynamic)
#endif
    for(int row = 0; row < num_rows; row++)
      for(int col = 0; col < num_cols; col++) max = std::max(data[row * num_cols + col], max);
#ifdef _OPENMP
#pragma omp critical
#endif
    {
      shared_max = std::max(shared_max, max);
    }
  }
  return shared_max;
}


template <class T> T matrix<T>::min()
{
  T shared_min;

// T* pdata = data;
// int pnum_cols = num_cols;
#ifdef _OPENMP
#pragma omp parallel // shared(pdata, pnum_cols)
#endif
  {
    T min = std::numeric_limits<T>::max();
#ifdef _OPENMP
#pragma omp for schedule(dynamic)
#endif
    for(int row = 0; row < num_rows; row++)
      for(int col = 0; col < num_cols; col++) min = std::min(data[row * num_cols + col], min);
#ifdef _OPENMP
#pragma omp critical
#endif
    {
      shared_min = std::min(shared_min, min);
    }
  }
  return shared_min;
}

template <class T> double matrix<T>::mean()
{
  assert(num_rows > 0 && num_cols > 0);
  double size = num_rows * num_cols;
  return sum() / size;
}

template <class T> double matrix<T>::variance()
{
  double m = mean();
  double size = num_rows * num_cols;
  double variance = 0;

#ifdef _OPENMP
#pragma omp parallel for reduction(+ : variance)
#endif
  for(int row = 0; row < num_rows; row++)
    for(int col = 0; col < num_cols; col++) variance += pow(data[row * num_cols + col] - m, 2);
  return variance / size;
}

// Non object functions

template <class T> double sum(matrix<T> &mat)
{
  return mat.sum();
}

template <class T> T max(matrix<T> &mat)
{
  return mat.max();
}

template <class T> T min(matrix<T> &mat)
{
  return mat.min();
}


template <class T> double mean(matrix<T> &mat)
{
  return mat.mean();
}

template <class T> double variance(matrix<T> &mat)
{
  return mat.variance();
}

template <class T, class U> const matrix<T> operator+(const matrix<T> &mat1, const matrix<U> &mat2)
{
  return mat1.add(mat2);
}

template <class T, class U> const matrix<T> operator+(const U value, const matrix<T> &mat)
{
  return mat.add(value);
}

template <class T, class U> const matrix<T> operator+(const matrix<T> &mat, const U value)
{
  return mat.add(value);
}

template <class T, class U> const matrix<T> operator+=(matrix<T> &mat, const U value)
{
  return mat.add_this(value);
}

template <class T, class U> const matrix<T> operator-(const matrix<T> &mat1, const matrix<U> &mat2)
{
  return mat1.subtract(mat2);
}

template <class T, class U> const matrix<T> operator-(const matrix<T> &mat, const U value)
{
  return mat.subtact(value);
}

template <class T, class U> const matrix<T> operator%(const matrix<T> &mat1, const matrix<U> &mat2)
{
  return mat1.pointmult(mat2);
}

template <class T, class U> const matrix<T> operator*(const U value, const matrix<T> &mat)
{
  return mat.mult(value);
}

template <class T, class U> const matrix<T> operator*(const matrix<T> &mat, const U value)
{
  return mat.mult(value);
}

template <class T, class U> const matrix<T> operator*=(matrix<T> &mat, const U value)
{
  return mat.mult_this(value);
}

template <class T, class U> const matrix<T> operator/(const matrix<T> &mat, const U value)
{
  return mat.divide(value);
}

#endif // MATRIX_H

//Function signatures


matrix<float> exposure(matrix<float> input_image, float crystals_per_pixel,
        float rolloff_boundary);

//Equalizes the concentration of developer across the reservoir and all pixels.
void agitate( matrix<float> &developerConcentration, float activeLayerThickness,
              float &reservoirDeveloperConcentration, float reservoirThickness,
              float pixelsPerMillimeter );

//This simulates one step of the development reaction.
void develop( matrix<float> &crystalRad,
              float crystalGrowthConst,
              const matrix<float> &activeCrystalsPerPixel,
              matrix<float> &silverSaltDensity,
              matrix<float> &develConcentration,
              float activeLayerThickness,
              float developerConsumptionConst,
              float silverSaltConsumptionConst,
              float timestep);

void diffuse_short_convolution(matrix<float> &developer_concentration,
                               const float sigma_const,
                               const float pixels_per_millimeter,
                               const float timestep);

void layer_mix(matrix<float> &developer_concentration,
               float active_layer_thickness,
               float &reservoir_developer_concentration,
               float reservoir_thickness,
               float layer_mix_const,
               float layer_time_divisor,
               float pixels_per_millimeter,
               float timestep);

void filmulate(const float *const in,
               float *const out,
               const int width_in,
               const int height_in,
               const int x_out,
               const int y_out,
               const int width_out,
               const int height_out,
               const float rolloff_boundary,
               const float film_area,
               const float layer_mix_const,
               const int agitate_count);

//Mixes all of the developer in the active layer and reservoir.
void agitate(matrix<float> &developerConcentration, float activeLayerThickness,
             float &reservoirDeveloperConcentration, float reservoirThickness, float pixelsPerMillimeter)
{
  int npixels = developerConcentration.nc() * developerConcentration.nr();
  float totalDeveloper = sum(developerConcentration) * activeLayerThickness / pow(pixelsPerMillimeter, 2)
                         + reservoirDeveloperConcentration * reservoirThickness;
  float contactLayerSize = npixels * activeLayerThickness / pow(pixelsPerMillimeter, 2);
  reservoirDeveloperConcentration = totalDeveloper / (reservoirThickness + contactLayerSize);
  developerConcentration = reservoirDeveloperConcentration;
  return;
}

//This runs one iteration of the differential equation for the chemical reaction of film development.
void develop(matrix<float> &crystalRad, float crystalGrowthConst, const matrix<float> &activeCrystalsPerPixel,
             matrix<float> &silverSaltDensity, matrix<float> &develConcentration, float activeLayerThickness,
             float developerConsumptionConst, float silverSaltConsumptionConst, float timestep)
{

  // Setting up dimensions and boundaries.
  int height = develConcentration.nr();
  int width = develConcentration.nc();
  // We still count columns of pixels, because we must process them
  // whole, so to ensure this we runthree adjacent elements at a time.

  // Here we pre-compute some repeatedly used values.
  float cgc = crystalGrowthConst * timestep;
  float dcc = 2.0 * developerConsumptionConst / (activeLayerThickness * 3.0);
  float sscc = silverSaltConsumptionConst * 2.0;

  // These are only used once per loop, so they don't have to be matrices.
  float dCrystalRadR;
  float dCrystalRadG;
  float dCrystalRadB;
  float dCrystalVolR;
  float dCrystalVolG;
  float dCrystalVolB;

  // These are the column indices for red, green, and blue.
  int row, col, colr, colg, colb;

#ifdef _OPENMP
#pragma omp parallel shared(develConcentration, silverSaltDensity, crystalRad, activeCrystalsPerPixel, cgc,  \
                            dcc, sscc) private(row, col, colr, colg, colb, dCrystalRadR, dCrystalRadG,       \
                                               dCrystalRadB, dCrystalVolR, dCrystalVolG, dCrystalVolB)
#endif
  {

#ifdef _OPENMP
#pragma omp for schedule(dynamic) nowait
#endif
    for(row = 0; row < height; row++)
    {
      for(col = 0; col < width; col++)
      {
        colr = col * 3;
        colg = colr + 1;
        colb = colr + 2;
        // This is the rate of thickness accumulating on the crystals.
        dCrystalRadR = develConcentration(row, col) * silverSaltDensity(row, colr) * cgc;
        dCrystalRadG = develConcentration(row, col) * silverSaltDensity(row, colg) * cgc;
        dCrystalRadB = develConcentration(row, col) * silverSaltDensity(row, colb) * cgc;

        // The volume change is proportional to 4*pi*r^2*dr.
        // We kinda shuffled around the constants, so ignore the lack of
        // the 4 and the pi.
        // However, there are varying numbers of crystals, so we also
        // multiply by the number of crystals per pixel.
        dCrystalVolR = dCrystalRadR * crystalRad(row, colr) * crystalRad(row, colr)
                       * activeCrystalsPerPixel(row, colr);
        dCrystalVolG = dCrystalRadG * crystalRad(row, colg) * crystalRad(row, colg)
                       * activeCrystalsPerPixel(row, colg);
        dCrystalVolB = dCrystalRadB * crystalRad(row, colb) * crystalRad(row, colb)
                       * activeCrystalsPerPixel(row, colb);

        // Now we apply the new crystal radius.
        crystalRad(row, colr) += dCrystalRadR;
        crystalRad(row, colg) += dCrystalRadG;
        crystalRad(row, colb) += dCrystalRadB;

        // Here is where we consume developer. The 3 layers of film,
        //(one per color) share the same developer.
        develConcentration(row, col) -= dcc * (dCrystalVolR + dCrystalVolG + dCrystalVolB);

        // Prevent developer concentration from going negative.
        if(develConcentration(row, col) < 0)
        {
          develConcentration(row, col) = 0;
        }
        // Here, silver salts are consumed in proportion to how much
        // silver was deposited on the crystals. Unlike the developer,
        // each color layer has its own separate amount in this sim.
        silverSaltDensity(row, colr) -= sscc * dCrystalVolR;
        silverSaltDensity(row, colg) -= sscc * dCrystalVolG;
        silverSaltDensity(row, colb) -= sscc * dCrystalVolB;
      }
    }
  }
  return;
}

// This uses a convolution forward and backward with a particular
// 4-length, 1-dimensional kernel to mimic a gaussian.
// In the first pass, it starts at 0, then goes out 4 standard deviations
// onto 0-clamped padding, then convolves back to the start.
// Naturally this attenuates the edges, so it does the same to all ones,
// and divides the image by that.

// Based on the paper "Recursive implementation of the Gaussian filter"
// in Signal Processing 44 (1995) 139-151
// Referencing code from here:
// https://github.com/halide/Halide/blob/e23f83b9bde63ed64f4d9a2fbe1ed29b9cfbf2e6/test/generator/gaussian_blur_generator.cpp
void diffuse_short_convolution(matrix<float> &developer_concentration, const float sigma_const,
                               const float pixels_per_millimeter, const float timestep)
{
  const int height = developer_concentration.nr();
  const int width = developer_concentration.nc();

  // Compute the standard deviation of the blur we want, in pixels.
  const double sigma = sqrt(timestep * pow(sigma_const * pixels_per_millimeter, 2));

  // We set the padding to be 4 standard deviations so as to catch as much as possible.
  int paddedWidth = width + 4 * sigma + 3;
  int paddedHeight = height + 4 * sigma + 3;

  double q; // constant for computing coefficients
  if(sigma < 2.5)
  {
    q = 3.97156 - 4.14554 * sqrt(1 - 0.26891 * sigma);
  }
  else
  {
    q = 0.98711 * sigma - 0.96330;
  }

  double denom = 1.57825 + 2.44413 * q + 1.4281 * q * q + 0.422205 * q * q * q;
  double coeff[4];

  coeff[1] = (2.44413 * q + 2.85619 * q * q + 1.26661 * q * q * q) / denom;
  coeff[2] = (-1.4281 * q * q - 1.26661 * q * q * q) / denom;
  coeff[3] = (0.422205 * q * q * q) / denom;
  coeff[0] = 1 - (coeff[1] + coeff[2] + coeff[3]);

  // We blur ones in order to cancel the edge attenuation.

  // First we do horizontally.
  vector<double> attenuationX(paddedWidth);
  // Set up the boundary
  attenuationX[0] = coeff[0]; // times 1
  attenuationX[1] = coeff[0] + coeff[1] * attenuationX[0];
  attenuationX[2] = coeff[0] + coeff[1] * attenuationX[1] + coeff[2] * attenuationX[0];
  // Go over the image width
  for(int i = 3; i < width; i++)
  {
    attenuationX[i] = coeff[0] + // times 1
                      coeff[1] * attenuationX[i - 1] + coeff[2] * attenuationX[i - 2]
                      + coeff[3] * attenuationX[i - 3];
  }
  // Fill in the padding (which is all zeros)
  for(int i = width; i < paddedWidth; i++)
  {
    // All zeros, so no coeff[0]*1 here.
    attenuationX[i]
        = coeff[1] * attenuationX[i - 1] + coeff[2] * attenuationX[i - 2] + coeff[3] * attenuationX[i - 3];
  }
  // And go back.
  for(int i = paddedWidth - 3 - 1; i >= 0; i--)
  {
    attenuationX[i] = coeff[0] * attenuationX[i] + coeff[1] * attenuationX[i + 1]
                      + coeff[2] * attenuationX[i + 2] + coeff[3] * attenuationX[i + 3];
  }

#ifdef _OPENMP
#pragma omp parallel for
#endif
  for(int i = 0; i < width; i++)
  {
    if(attenuationX[i] <= 0)
    {
      //std::cout << "gonna blow X" << std::endl;
    }
    else // we can invert this
    {
      attenuationX[i] = 1.0 / attenuationX[i];
    }
  }

  // And now vertically.
  vector<double> attenuationY(paddedHeight);
  // Set up the boundary
  attenuationY[0] = coeff[0]; // times 1
  attenuationY[1] = coeff[0] + coeff[1] * attenuationY[0];
  attenuationY[2] = coeff[0] + coeff[1] * attenuationY[1] + coeff[2] * attenuationY[0];
  // Go over the image height
  for(int i = 3; i < height; i++)
  {
    attenuationY[i] = coeff[0] + // times 1
                      coeff[1] * attenuationY[i - 1] + coeff[2] * attenuationY[i - 2]
                      + coeff[3] * attenuationY[i - 3];
  }
  // Fill in the padding (which is all zeros)
  for(int i = height; i < paddedHeight; i++)
  {
    // All zeros, so no coeff[0]*1 here.
    attenuationY[i]
        = coeff[1] * attenuationY[i - 1] + coeff[2] * attenuationY[i - 2] + coeff[3] * attenuationY[i - 3];
  }
  // And go back.
  for(int i = paddedHeight - 3 - 1; i >= 0; i--)
  {
    attenuationY[i] = coeff[0] * attenuationY[i] + coeff[1] * attenuationY[i + 1]
                      + coeff[2] * attenuationY[i + 2] + coeff[3] * attenuationY[i + 3];
  }

#ifdef _OPENMP
#pragma omp parallel for
#endif
  for(int i = 0; i < height; i++)
  {
    if(attenuationY[i] <= 0)
    {
      //std::cout << "gonna blow Y" << std::endl;
    }
    else
    {
      attenuationY[i] = 1.0 / attenuationY[i];
    }
  }

#ifdef _OPENMP
#pragma omp parallel shared(developer_concentration, coeff, attenuationX)
#endif
  {
    // X direction blurring.
    // We slice by individual rows.
    vector<double> devel_concX(paddedWidth);
#ifdef _OPENMP
#pragma omp for schedule(dynamic)
#endif
    for(int row = 0; row < height; row++)
    {
      // Copy data into the temp.
      for(int col = 0; col < width; col++)
      {
        devel_concX[col] = double(developer_concentration(row, col));
      }
      // Set up the boundary
      devel_concX[0] = coeff[0] * devel_concX[0];
      devel_concX[1] = coeff[0] * devel_concX[1] + coeff[1] * devel_concX[0];
      devel_concX[2] = coeff[0] * devel_concX[2] + coeff[1] * devel_concX[1] + coeff[2] * devel_concX[0];
      // Iterate over the main part of the image, except for the setup
      for(int col = 3; col < width; col++)
      {
        devel_concX[col] = coeff[0] * devel_concX[col]
                         + coeff[1] * devel_concX[col - 1]
                         + coeff[2] * devel_concX[col - 2]
                         + coeff[3] * devel_concX[col - 3];
      }
      // Iterate over the zeroed tail
      for(int col = width; col < paddedWidth; col++)
      {
        devel_concX[col] = coeff[1] * devel_concX[col - 1]
                         + coeff[2] * devel_concX[col - 2]
                         + coeff[3] * devel_concX[col - 3];
      }
      // And go back
      for(int col = paddedWidth - 3 - 1; col >= 0; col--)
      {
        devel_concX[col] = coeff[0] * devel_concX[col]
                         + coeff[1] * devel_concX[col + 1]
                         + coeff[2] * devel_concX[col + 2]
                         + coeff[3] * devel_concX[col + 3];
      }
// And undo the attenuation, copying back from the temp.
#ifdef _OPENMP
#pragma omp simd
#endif
      for(int col = 0; col < width; col++)
      {
        developer_concentration(row, col) = devel_concX[col] * attenuationX[col];
      }
    }
  }

#ifdef _OPENMP
#pragma omp parallel shared(developer_concentration, coeff, attenuationY)
#endif
  {
    // Y direction blurring. We slice into columns a whole number of cache lines wide.
    // Each cache line is 8 doubles wide.
    matrix<double> devel_concY;
    int thickness = 8; // of the slice
    devel_concY.set_size(paddedHeight, thickness);
    int slices = ceil(width / float(thickness));
#ifdef _OPENMP
#pragma omp for schedule(dynamic)
#endif
    for(int slice = 0; slice < slices; slice++)
    {
      int offset = slice * thickness;
      int iter = thickness; // number of columns to loop through
      if(offset + thickness > width) // If it's the last row,
      {
        iter = width - offset; // Don't go beyond the bounds
      }

      // Copy data into the temp.
      if(iter < 8) // we can't SIMD this nicely
      {
        for(int row = 0; row < height; row++)
        {
          for(int col = 0; col < iter; col++)
          {
            devel_concY(row, col) = developer_concentration(row, col + offset);
          }
        }
      }
      else // we can simd this
      {
        for(int row = 0; row < height; row++)
        {
#ifdef _OPENMP
#pragma omp simd
#endif
          for(int col = 0; col < 8; col++)
          {
            devel_concY(row, col) = developer_concentration(row, col + offset);
          }
        }
      }

// Set up the boundary.
#ifdef _OPENMP
#pragma omp simd
#endif
      for(int col = 0; col < 8; col++)
      {
        devel_concY(0, col) = coeff[0] * devel_concY(0, col);
        devel_concY(1, col) = coeff[0] * devel_concY(1, col)
                            + coeff[1] * devel_concY(0, col);
        devel_concY(2, col) = coeff[0] * devel_concY(2, col)
                            + coeff[1] * devel_concY(1, col)
                            + coeff[2] * devel_concY(0, col);
      }
      // Iterate over the main part of the image, except for the setup.
      for(int row = 3; row < height; row++)
      {
#ifdef _OPENMP
#pragma omp simd
#endif
        for(int col = 0; col < 8; col++)
        {
          devel_concY(row, col) = coeff[0] * devel_concY(row, col)
                                + coeff[1] * devel_concY(row - 1, col)
                                + coeff[2] * devel_concY(row - 2, col)
                                + coeff[3] * devel_concY(row - 3, col);
        }
      }
      // Iterate over the zeroed tail
      for(int row = height; row < paddedHeight; row++)
      {
#ifdef _OPENMP
#pragma omp simd
#endif
        for(int col = 0; col < 8; col++)
        {
          devel_concY(row, col) = coeff[1] * devel_concY(row - 1, col)
                                + coeff[2] * devel_concY(row - 2, col)
                                + coeff[3] * devel_concY(row - 3, col);
        }
      }
      // And go back
      for(int row = paddedHeight - 3 - 1; row >= 0; row--)
      {
#ifdef _OPENMP
#pragma omp simd
#endif
        for(int col = 0; col < 8; col++)
        {
          devel_concY(row, col) = coeff[0] * devel_concY(row, col)
                                + coeff[1] * devel_concY(row + 1, col)
                                + coeff[2] * devel_concY(row + 2, col)
                                + coeff[3] * devel_concY(row + 3, col);
        }
      }
      // And undo the attenuation, copying back from the temp.
      if(iter < 8) // we can't SIMD this nicely
      {
        for(int row = 0; row < height; row++)
        {
          for(int col = 0; col < iter; col++)
          {
            developer_concentration(row, col + offset) = devel_concY(row, col) * attenuationY[row];
          }
        }
      }
      else
      {
        for(int row = 0; row < height; row++)
        {
#ifdef _OPENMP
#pragma omp simd
#endif
          for(int col = 0; col < 8; col++)
          {
            developer_concentration(row, col + offset) = devel_concY(row, col) * attenuationY[row];
          }
        }
      }
    }
  }
}

//This is a tone curve of sorts to control the maximum exposure in filmulator.
matrix<float> exposure(matrix<float> input_image, float crystals_per_pixel, float rolloff_boundary)
{
  if(rolloff_boundary < 1.0f)
  {
    rolloff_boundary = 1.0f;
  }
  else if(rolloff_boundary > 65534.0f)
  {
    rolloff_boundary = 65534.0f;
  }
  int nrows = input_image.nr();
  int ncols = input_image.nc();
  float input;
  float crystal_headroom = 65535 - rolloff_boundary;
#ifdef _OPENMP
#pragma omp parallel shared(input_image, crystals_per_pixel, rolloff_boundary, nrows, ncols,                 \
                            crystal_headroom) private(input)
#endif
  {
#ifdef _OPENMP
#pragma omp for schedule(dynamic) nowait
#endif
    for(int row = 0; row < nrows; row++)
    {
      for(int col = 0; col < ncols; col++)
      {
        input = std::max(0.0f, input_image(row, col));
        if(input > rolloff_boundary)
          input
              = 65535 - (crystal_headroom * crystal_headroom / (input + crystal_headroom - rolloff_boundary));
        input_image(row, col) = input * crystals_per_pixel * 0.00015387105;
        // Magic number mostly for historical reasons
      }
    }
  }
  return input_image;
}

// This function implements diffusion between the active developer layer
// adjacent to the film and the reservoir of inactive developer.
void layer_mix(matrix<float> &developer_concentration, float active_layer_thickness,
               float &reservoir_developer_concentration, float reservoir_thickness, float layer_mix_const,
               float layer_time_divisor, float pixels_per_millimeter, float timestep)
{
  int length = developer_concentration.nr();
  int width = developer_concentration.nc();

  // layer_time_divisor adjusts the ratio between the timestep used to compute
  // the diffuse within the layer and this diffuse.
  float layer_mix = pow(layer_mix_const, timestep / layer_time_divisor);

  // layer_mix is the proportion of developer that stays in the layer.

  // This gives us the amount of developer that comes from the reservoir.
  float reservoir_portion = (1 - layer_mix) * reservoir_developer_concentration;

  // This lets us count how much developer got added to the layer in total.
  double sum = 0;

// Here we add developer to the layer.
#ifdef _OPENMP
#pragma omp parallel shared(developer_concentration) firstprivate(layer_mix, reservoir_portion)\
    reduction(+ : sum)
#endif
  {
#ifdef _OPENMP
#pragma omp for schedule(dynamic) nowait
#endif
    for(int row = 0; row < length; row++)
    {
      float temp;
      for(int col = 0; col < width; col++)
      {
        temp = developer_concentration(row, col) * layer_mix + reservoir_portion;
        // Here we accumulate how much developer went into the layer.
        sum += temp - developer_concentration(row, col);
        developer_concentration(row, col) = temp;
      }
    }
  }

  // Now, we must adjust sum to ensure that the parameters
  // are orthogonal. It's sketchy, okay?
  float reservoir_concentration_change
      = sum * active_layer_thickness / (pow(pixels_per_millimeter, 2) * reservoir_thickness);

  // The reservoir thickness is not actually the reservoir thickness, but volume.
  // This is a major weirdness from when it was originally thickness on the outside
  // but we called it volume because that's what it is on the inside, like here.

  // Now, we subtract how much went into the layer from the reservoir.
  reservoir_developer_concentration -= reservoir_concentration_change;
  return;
}

//The main filmulation algorithm here. This converts the input and output brightnesses too.
void filmulate(const float *const in, float *const out, const int width_in, const int height_in,
               const int x_out, const int y_out, const int width_out, const int height_out,
               const float rolloff_boundary, const float film_area, const float layer_mix_const,
               const int agitate_count)
{

  // Magic numbers
  float initial_developer_concentration = 1.0f;
  float reservoir_thickness = 1000.0f;
  float active_layer_thickness = 0.1f;
  float crystals_per_pixel = 500.0f;
  float initial_crystal_radius = 0.00001f;
  float initial_silver_salt_density = 1.0f;
  float developer_consumption_const = 2000000.0f;
  float crystal_growth_const = 0.00001f;
  float silver_salt_consumption_const = 2000000.0f;
  float total_development_time = 100.0f;
  // int agitate_count = 1;
  int development_steps = 12;
  // float film_area = 864.0f;
  float sigma_const = 0.2f;
  // float layer_mix_const = 0.2f;
  float layer_time_divisor = 20.0f;
  // float rolloff_boundary = 51275.0f;

  const int nrows = height_in;
  const int ncols = width_in;

  // Load things into matrices for Filmulator.
  matrix<float> input_image;
  input_image.set_size(nrows, ncols * 3);
#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none) shared(input_image)
#endif
  for(int i = 0; i < nrows; i++)
  {
    for(int j = 0; j < ncols; j++)
    {
      input_image(i, j * 3) = 65535.0f * in[(j + i * ncols) * 4];
      input_image(i, j * 3 + 1) = 65535.0f * in[(j + i * ncols) * 4 + 1];
      input_image(i, j * 3 + 2) = 65535.0f * in[(j + i * ncols) * 4 + 2];
    }
  }

  int npix = nrows * ncols;

  // Now we activate some of the crystals on the film. This is literally
  // akin to exposing film to light.
  matrix<float> active_crystals_per_pixel;
  active_crystals_per_pixel = exposure(input_image, crystals_per_pixel, rolloff_boundary);

  // We set the crystal radius to a small seed value for each color.
  matrix<float> crystal_radius;
  crystal_radius.set_size(nrows, ncols * 3);
  crystal_radius = initial_crystal_radius;

  // All layers share developer, so we only make it the original image size.
  matrix<float> developer_concentration;
  developer_concentration.set_size(nrows, ncols);
  developer_concentration = initial_developer_concentration;

  // Each layer gets its own silver salt which will feed crystal growth.
  matrix<float> silver_salt_density;
  silver_salt_density.set_size(nrows, ncols * 3);
  silver_salt_density = initial_silver_salt_density;

// Now, we set up the reservoir.
// Because we don't want the film area to influence the brightness, we
// increase the reservoir size in proportion.
#define FILMSIZE 864; // 36x24mm
  reservoir_thickness *= film_area / FILMSIZE;
  float reservoir_developer_concentration = initial_developer_concentration;

  // This is a value used in diffuse to set the length scale.
  float pixels_per_millimeter = sqrt(npix / film_area);

  // Here we do some math for the control logic for the differential
  // equation approximation computations.
  float timestep = total_development_time / development_steps;
  int agitate_period;
  if(agitate_count > 0)
    agitate_period = floor(development_steps / agitate_count);
  else
    agitate_period = 3 * development_steps;
  int half_agitate_period = floor(agitate_period / 2);

  // Now we begin the main development/diffusion loop, which approximates the
  // differential equation of film development.
  for(int i = 0; i <= development_steps; i++)
  {
    // This is where we perform the chemical reaction part.
    // The crystals grow.
    // The developer in the active layer is consumed.
    // So is the silver salt in the film.
    // The amount consumed increases as the crystals grow larger.
    // Because the developer and silver salts are consumed in bright regions,
    // this reduces the rate at which they grow. This gives us global
    // contrast reduction.
    develop(crystal_radius, crystal_growth_const, active_crystals_per_pixel, silver_salt_density,
            developer_concentration, active_layer_thickness, developer_consumption_const,
            silver_salt_consumption_const, timestep);

    // Now, we are going to perform the diffusion part.
    // Here we mix the layer among itself, which grants us the
    // local contrast increases.
    // diffuse(developer_concentration,
    diffuse_short_convolution(developer_concentration, sigma_const, pixels_per_millimeter, timestep);

    // This performs mixing between the active layer adjacent to the film
    // and the reservoir.
    // This keeps the effects from getting too crazy.
    layer_mix(developer_concentration, active_layer_thickness, reservoir_developer_concentration,
              reservoir_thickness, layer_mix_const, layer_time_divisor, pixels_per_millimeter, timestep);

    // I want agitation to only occur in the middle of development, not
    // at the very beginning or the ends. So, I add half the agitate
    // period to the current cycle count.
    if((i + half_agitate_period) % agitate_period == 0)
      agitate(developer_concentration, active_layer_thickness, reservoir_developer_concentration,
              reservoir_thickness, pixels_per_millimeter);
  }

  // Done filmulating, now do some housecleaning
  silver_salt_density.free();
  developer_concentration.free();


  // Now we compute the density (opacity) of the film.
  // We assume that overlapping crystals or dye clouds are
  // nonexistant. It works okay, for now...
  // The output is crystal_radius^2 * active_crystals_per_pixel

  matrix<float> output_density = crystal_radius % crystal_radius % active_crystals_per_pixel * 500.0f;

// Convert back to darktable's RGBA.
#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none) shared(output_density)
#endif
  for(int i = 0; i < height_out; i++)
  {
    for(int j = 0; j < width_out; j++)
    {
      out[(j + i * width_out) * 4]
          = std::min(1.0f, std::max(0.0f, output_density(i + y_out, (j + x_out) * 3)));
      out[(j + i * width_out) * 4 + 1]
          = std::min(1.0f, std::max(0.0f, output_density(i + y_out, (j + x_out) * 3 + 1)));
      out[(j + i * width_out) * 4 + 2]
          = std::min(1.0f, std::max(0.0f, output_density(i + y_out, (j + x_out) * 3 + 2)));
      out[(j + i * width_out) * 4 + 3] = in[(j + i * ncols) * 4 + 3]; // copy the alpha channel
    }
  }
}

extern "C" {
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
// our includes go first:
#include "bauhaus/bauhaus.h"
#include "common/colorspaces.h"
#include "develop/imageop.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"

#include <gtk/gtk.h>
#include <stdlib.h>
#include <math.h>

// this is the version of the modules parameters,
// and includes version information about compile-time dt
DT_MODULE_INTROSPECTION(2, dt_iop_filmulate_params_t)

typedef struct dt_iop_filmulate_params_t
{
  // these are stored in db.
  // make sure everything is in here does not
  // depend on temporary memory (pointers etc)
  // stored in self->params and self->default_params
  // also, since this is stored in db, you should keep changes to this struct
  // to a minimum. if you have to change this struct, it will break
  // users data bases, and you should increment the version
  // of DT_MODULE(VERSION) above!
  int color_space_size;
  float rolloff_boundary;
  float film_area;
  float layer_mix_const;
  int agitate_count;
} dt_iop_filmulate_params_t;

typedef struct dt_iop_filmulate_gui_data_t
{
  GtkWidget *color_space_size, *rolloff_boundary, *film_area, *drama, *overdrive;
} dt_iop_filmulate_gui_data_t;

// this returns a translatable name
const char *name()
{
  // make sure you put all your translatable strings into _() !
  return _("filmulate");
}

//this returns a tooltip for the 'more modules' list
const char *description()
{
  return _("tone mapping based on literally simulating film development");
}

// some additional flags (self explanatory i think):
int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING;
}

// where does it appear in the gui?
int groups()
{
  return IOP_GROUP_TONE;
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_filmulate_params_t *p = (dt_iop_filmulate_params_t *)p1;
  dt_iop_filmulate_params_t *d = (dt_iop_filmulate_params_t *)piece->data;

  d->color_space_size = p->color_space_size;
  d->rolloff_boundary = p->rolloff_boundary * 65535.0f;
  d->film_area = powf(p->film_area, 2.0f);
  d->layer_mix_const = p->layer_mix_const / 100.0f;
  d->agitate_count = p->agitate_count;
}

/** modify regions of interest; filmulation requires the full image. **/
// The region of interest out is going to be the same as what it's given.
// Filmulator does not change this.
/*
void modify_roi_out(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                    dt_iop_roi_t *roi_out, const dt_iop_roi_t *roi_in)
{
  *roi_out = *roi_in;
}
*/

// dt_iop_roi_t has 5 components: x, y, width, height, scale.
// The width and height are the viewport size.
//  When modifying roi_in, filmulator wants to change this to be the full image, scaled by the scale.
// The scale is the output relative to the input.
//===================NO IT ISN'T; we basically ignore it.
//  Filmulator doesn't want to change this.
// x and y are the viewport location relative to the full image area, at the viewport scale.
//  Filmulator wants to set this to 0.
void modify_roi_in(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                   const dt_iop_roi_t *roi_out, dt_iop_roi_t *roi_in)
{
  roi_in->scale = roi_out->scale;

  roi_in->x = 0;
  roi_in->y = 0;
  roi_in->width = round(piece->buf_in.width * roi_out->scale);
  roi_in->height = round(piece->buf_in.height * roi_out->scale);
}

/** process, all real work is done here. */
void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const i, void *const o,
             const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  // Get the data struct.
  dt_iop_filmulate_params_t *d = (dt_iop_filmulate_params_t *)piece->data;
  const int color_space_size = d->color_space_size;
  const float rolloff_boundary = d->rolloff_boundary;
  const float film_area = d->film_area;
  const float layer_mix_const = d->layer_mix_const;
  const int agitate_count = d->agitate_count;
  dt_iop_color_intent_t intent = DT_INTENT_PERCEPTUAL;

  const cmsHPROFILE Lab
      = dt_colorspaces_get_profile(DT_COLORSPACE_LAB, "", DT_PROFILE_DIRECTION_ANY)->profile;
  const cmsHPROFILE Rec2020
      = dt_colorspaces_get_profile(DT_COLORSPACE_LIN_REC2020, "", DT_PROFILE_DIRECTION_ANY)->profile;
  const cmsHPROFILE Rec709
      = dt_colorspaces_get_profile(DT_COLORSPACE_LIN_REC709, "", DT_PROFILE_DIRECTION_ANY)->profile;
  cmsHTRANSFORM transform_lab_to_lin_rgba, transform_lin_rgba_to_lab;
  if(0 == color_space_size)
  {
    transform_lab_to_lin_rgba = cmsCreateTransform(Lab, TYPE_LabA_FLT, Rec2020, TYPE_RGBA_FLT, intent, 0);
    transform_lin_rgba_to_lab = cmsCreateTransform(Rec2020, TYPE_RGBA_FLT, Lab, TYPE_LabA_FLT, intent, 0);
  }
  else
  {
    transform_lab_to_lin_rgba = cmsCreateTransform(Lab, TYPE_LabA_FLT, Rec709, TYPE_RGBA_FLT, intent, 0);
    transform_lin_rgba_to_lab = cmsCreateTransform(Rec709, TYPE_RGBA_FLT, Lab, TYPE_LabA_FLT, intent, 0);
  }

  const int width_in = roi_in->width;
  const int height_in = roi_in->height;
  const int x_out = roi_out->x;
  const int y_out = roi_out->y;
  const int width_out = roi_out->width;
  const int height_out = roi_out->height;

  // Temp buffer for the whole image
  float *rgbbufin = (float *)calloc(width_in * height_in * 4, sizeof(float));
  float *rgbbufout = (float *)calloc(width_out * height_out * 4, sizeof(float));

// Turn Lab into linear Rec2020
#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none) shared(rgbbufin, transform_lab_to_lin_rgba)
#endif
  for(int y = 0; y < height_in; y++)
  {
    const float *in = (float *)i + y * width_in * 4;
    float *out = rgbbufin + y * width_in * 4;
    cmsDoTransform(transform_lab_to_lin_rgba, in, out, width_in);
  }


  // Filmulate things!
  filmulate(rgbbufin, rgbbufout,
            width_in, height_in,
            x_out, y_out,
            width_out, height_out,
            rolloff_boundary, film_area,
            layer_mix_const, agitate_count);

  free(rgbbufin);
// Turn back to Lab
#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none) shared(rgbbufout, transform_lin_rgba_to_lab)
#endif
  for(int y = 0; y < height_out; y++)
  {
    const float *in = rgbbufout + y * width_out * 4;
    float *out = (float *)o + y * width_out * 4;
    cmsDoTransform(transform_lin_rgba_to_lab, in, out, width_out);
  }
  free(rgbbufout);
}

/** optional: if this exists, it will be called to init new defaults if a new image is loaded from film strip
 * mode. */
void reload_defaults(dt_iop_module_t *module)
{
  // change default_enabled depending on type of image, or set new default_params even.

  // if this callback exists, it has to write default_params and default_enabled.
}

/** init, cleanup, commit to pipeline */
void init(dt_iop_module_t *module)
{
  // we don't need global data:
  module->params = calloc(1, sizeof(dt_iop_filmulate_params_t));
  module->default_params = calloc(1, sizeof(dt_iop_filmulate_params_t));
  // our module is disabled by default
  // by default:
  module->default_enabled = 0;
  // order has to be changed by editing the dependencies in tools/iop_dependencies.py
  module->priority = 515; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_filmulate_params_t);
  module->gui_data = NULL;
  // init defaults:
  dt_iop_filmulate_params_t tmp
      = (dt_iop_filmulate_params_t){ 0, 51275.0f / 65535.0f, sqrtf(864.0f), 20.0f, 1 };

  memcpy(module->params, &tmp, sizeof(dt_iop_filmulate_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_filmulate_params_t));
}

void cleanup(dt_iop_module_t *module)
{
  free(module->params);
  module->params = NULL;
  free(module->default_params);
  module->default_params = NULL;

}

/** put your local callbacks here, be sure to make them static so they won't be visible outside this file! */
static void color_space_size_callback(GtkWidget *w, dt_iop_module_t *self)
{
  // This is important to avoid cycles!
  if(darktable.gui->reset)
  {
    return;
  }
  dt_iop_filmulate_params_t *p = (dt_iop_filmulate_params_t *)self->params;

  // If overdrive is off, we agitate once. If overdrive is on, we don't agitate.
  p->color_space_size = dt_bauhaus_combobox_get(w);
  // Let core know of the changes
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void rolloff_boundary_callback(GtkWidget *w, dt_iop_module_t *self)
{
  // This is important to avoid cycles!
  if(darktable.gui->reset)
  {
    return;
  }
  dt_iop_filmulate_params_t *p = (dt_iop_filmulate_params_t *)self->params;

  p->rolloff_boundary = dt_bauhaus_slider_get(w);
  // Let core know of the changes
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

// The slider goes from 0 to 65535, but we want to show 0 to 1.
static float rolloff_boundary_scaled_callback(GtkWidget *self, float input, dt_bauhaus_callback_t dir)
{
  float output;
  switch(dir)
  {
    case DT_BAUHAUS_SET:
      output = input * 65535.0f;
      break;
    case DT_BAUHAUS_GET:
      output = input / 65535.0f;
      break;
    default:
      output = input;
  }
  return output;
}

static void film_area_callback(GtkWidget *w, dt_iop_module_t *self)
{
  // This is important to avoid cycles!
  if(darktable.gui->reset)
  {
    return;
  }
  dt_iop_filmulate_params_t *p = (dt_iop_filmulate_params_t *)self->params;

  // The film area control is logarithmic WRT the linear dimensions of film.
  // But in the backend, it's actually using square millimeters of simulated film.
  // p->film_area = powf(expf(dt_bauhaus_slider_get(w)),2.0f);
  p->film_area = dt_bauhaus_slider_get(w);
  // Let core know of the changes
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

// The film size slider displays the exponential of the linear slider position.
static float film_dimensions_callback(GtkWidget *self, float input, dt_bauhaus_callback_t dir)
{
  float output;
  switch(dir)
  {
    case DT_BAUHAUS_SET:
      // output = exp(input);
      output = log(fmax(input, 1e-15f));
      break;
    case DT_BAUHAUS_GET:
      // output = log(fmax(input, 1e-15f));
      output = exp(input);
      break;
    default:
      output = input;
  }
  return output;
}

static void drama_callback(GtkWidget *w, dt_iop_module_t *self)
{
  // This is important to avoid cycles!
  if(darktable.gui->reset)
  {
    return;
  }
  dt_iop_filmulate_params_t *p = (dt_iop_filmulate_params_t *)self->params;

  // Drama goes from 0 to 100, but the relevant parameter in the backend is 0 to 1.
  p->layer_mix_const = dt_bauhaus_slider_get(w);
  // Let core know of the changes
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

// The slider goes from 0 to 1, but we want to show 0 to 100.
static float drama_scaled_callback(GtkWidget *self, float input, dt_bauhaus_callback_t dir)
{
  float output;
  switch(dir)
  {
    case DT_BAUHAUS_SET:
      output = input / 100.0f;
      break;
    case DT_BAUHAUS_GET:
      output = input * 100.0f;
      break;
    default:
      output = input;
  }
  return output;
}

static void overdrive_callback(GtkWidget *w, dt_iop_module_t *self)
{
  // This is important to avoid cycles!
  if(darktable.gui->reset)
  {
    return;
  }
  dt_iop_filmulate_params_t *p = (dt_iop_filmulate_params_t *)self->params;

  // If overdrive is off, we agitate once. If overdrive is on, we don't agitate.
  p->agitate_count = (dt_bauhaus_combobox_get(w) == 0) ? 1 : 0;
  // Let core know of the changes
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

/** gui callbacks, these are needed. */
void gui_update(dt_iop_module_t *self)
{
  // let gui slider match current parameters:
  dt_iop_filmulate_gui_data_t *g = (dt_iop_filmulate_gui_data_t *)self->gui_data;
  dt_iop_filmulate_params_t *p = (dt_iop_filmulate_params_t *)self->params;
  dt_bauhaus_combobox_set(g->color_space_size, p->color_space_size);
  dt_bauhaus_slider_set(g->rolloff_boundary, p->rolloff_boundary);
  dt_bauhaus_slider_set(g->film_area, p->film_area);
  dt_bauhaus_slider_set(g->drama, p->layer_mix_const);
  dt_bauhaus_combobox_set(g->overdrive, (p->agitate_count == 0) ? 1 : 0);
}

void gui_init(dt_iop_module_t *self)
{
  // init the slider (more sophisticated layouts are possible with gtk tables and boxes):
  self->gui_data = malloc(sizeof(dt_iop_filmulate_gui_data_t));
  dt_iop_filmulate_gui_data_t *g = (dt_iop_filmulate_gui_data_t *)self->gui_data;

  // Create the widgets
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  g->color_space_size = dt_bauhaus_combobox_new(self);
  g->rolloff_boundary = dt_bauhaus_slider_new_with_range(self, 1.0f, 65535.0f, 512.0f, 51275.0f, 2);
  g->film_area = dt_bauhaus_slider_new_with_range(self, 1.2f, 6.0f, 0.1f, logf(sqrtf(864.0f)), 2);
  g->drama = dt_bauhaus_slider_new_with_range(self, 0.0f, 1.0f, 0.01f, 0.2f, 2);
  g->overdrive = dt_bauhaus_combobox_new(self);

  // Scaling things for the sliders
  dt_bauhaus_slider_set_callback(g->rolloff_boundary, rolloff_boundary_scaled_callback);
  dt_bauhaus_slider_set_callback(g->film_area, film_dimensions_callback);
  dt_bauhaus_slider_set_callback(g->drama, drama_scaled_callback);

  // Values for the comboboxes
  dt_bauhaus_combobox_add(g->color_space_size, _("Rec2020"));
  dt_bauhaus_combobox_add(g->color_space_size, _("Rec709"));
  dt_bauhaus_combobox_add(g->overdrive, _("off"));
  dt_bauhaus_combobox_add(g->overdrive, _("on"));

  dt_bauhaus_widget_set_label(g->color_space_size, NULL, _("color space size"));
  gtk_widget_set_tooltip_text(g->color_space_size,
                              _("filmulation works in RGB.\nRec2020 is a bigger space, good if you're using "
                                "larger output spaces.\nRec709 is good for sRGB output color space, and helps "
                                "attenuate the value of bright colors naturally."));
  dt_bauhaus_widget_set_label(g->rolloff_boundary, NULL, _("rolloff boundary"));
  gtk_widget_set_tooltip_text(g->rolloff_boundary, _("sets the point above which the highlights gently stop "
                                                     "getting brighter. if you've got completely unclipped "
                                                     "highlights before filmulation, raise this to 1."));
  dt_bauhaus_widget_set_label(g->film_area, NULL, _("film size"));
  gtk_widget_set_tooltip_text(
      g->film_area,
      _("larger sizes emphasize smaller details and overall flatten the image. smaller sizes emphasize "
        "larger regional contrasts. don't use larger sizes with high drama or you'll get the hdr look."));
  dt_bauhaus_widget_set_label(g->drama, NULL, _("drama"));
  gtk_widget_set_tooltip_text(g->drama, _("pulls down highlights to retain detail. this is the real "
                                          "\"filmy\" effect. this not only helps bring down highlights, but "
                                          "can rescue extremely saturated regions such as flowers."));
  dt_bauhaus_widget_set_label(g->overdrive, NULL, _("overdrive mode"));
  gtk_widget_set_tooltip_text(g->overdrive,
                              _("in case of emergency, break glass and press this button. this increases the "
                                "filminess, in case 100 Drama was not enough for you."));

  // Add widgets to the gui
  gtk_box_pack_start(GTK_BOX(self->widget), g->color_space_size, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->rolloff_boundary, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->film_area, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->drama, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->overdrive, TRUE, TRUE, 0);

  // Connect to the signals when widgets are changed
  g_signal_connect(G_OBJECT(g->color_space_size), "value-changed", G_CALLBACK(color_space_size_callback),
                   self);
  g_signal_connect(G_OBJECT(g->rolloff_boundary), "value-changed", G_CALLBACK(rolloff_boundary_callback),
                   self);
  g_signal_connect(G_OBJECT(g->film_area), "value-changed", G_CALLBACK(film_area_callback), self);
  g_signal_connect(G_OBJECT(g->drama), "value-changed", G_CALLBACK(drama_callback), self);
  g_signal_connect(G_OBJECT(g->overdrive), "value-changed", G_CALLBACK(overdrive_callback), self);
}

void gui_cleanup(dt_iop_module_t *self)
{
  // nothing else necessary, gtk will clean up the slider.
  free(self->gui_data);
  self->gui_data = NULL;
}

} // extern "C"

/** additional, optional callbacks to capture darkroom center events. */
// void gui_post_expose(dt_iop_module_t *self, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx,
// int32_t pointery);
// int mouse_moved(dt_iop_module_t *self, double x, double y, double pressure, int which);
// int button_pressed(dt_iop_module_t *self, double x, double y, double pressure, int which, int type,
// uint32_t state);
// int button_released(struct dt_iop_module_t *self, double x, double y, int which, uint32_t state);
// int scrolled(dt_iop_module_t *self, double x, double y, int up, uint32_t state);

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
