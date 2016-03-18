/* 
 * This file is part of Filmulator.
 *
 * Copyright 2013 Omer Mano and Carlo Vaccari
 *
 * Filmulator is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Filmulator is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Filmulator. If not, see <http://www.gnu.org/licenses/>
 */
#ifndef MATRIX_H
#define MATRIX_H
#include <limits>
#include <algorithm>
#include <math.h>
#include <emmintrin.h>
#include <iostream>

#ifdef _OPENMP
#include <omp.h>
#endif

#include "assert.h" //Included later so NDEBUG has an effect

template <class T>
class matrix
{
	private:
		T* data;
		int num_rows;
		int num_cols;
        inline void slow_transpose_to(const matrix<T> &target) const;
        inline void fast_transpose_to(const matrix<T> &target) const;
        inline void transpose4x4_SSE(float *A, float *B, const int lda,
                                     const int ldb) const;
        inline void transpose_block_SSE4x4(float *A, float *B, const int n,
                                           const int m, const int lda,
                                           const int ldb,
                                           const int block_size) const;
        inline void transpose_scalar_block(float *A, float *B, const int lda,
                                     const int ldb, const int block_size) const;
        inline void transpose_block(float *A, float *B, const int n,
                                           const int m, const int lda,
                                           const int ldb,
                                           const int block_size) const;
	public:
		matrix(const int nrows = 0, const int ncols = 0);
		matrix(const matrix<T> &toCopy);
		~matrix();
		void set_size(const int nrows, const int ncols);
		void free();
		int nr() const;
		int nc() const;
        T& operator()(const int row, const int col) const;
		//template <class U> //Never gets called if use matrix<U>
		matrix<T>& operator=(const matrix<T> &toCopy);
		template <class U>		
		matrix<T>& operator=(const U value);
		template <class U>		
		const matrix<T> add (const matrix<U> &rhs) const;
		template <class U>
		const matrix<T> add (const U value) const ;
        template <class U>
        const matrix<T>& add_this(const U value);
		template <class U>
		const matrix<T> subtract (const matrix<U> &rhs) const ;
		template <class U>
		const matrix<T> subtract (const U value) const;
		template <class U>
		const matrix<T> pointmult (const matrix<U> &rhs) const;		
		template <class U>
		const matrix<T> mult (const U value) const;
        template <class U>
        const matrix<T>& mult_this(const U value);
		template <class U>
		const matrix<T> divide (const U value) const;
        inline void transpose_to(const matrix<T> &target) const;
		double sum();
		T max();
		T min();
		double mean();
		double variance();
};

template <class T>
double sum(matrix<T> &mat);

template <class T>
T max(matrix<T> &mat);

template <class T>
T min(matrix<T> &mat);

template <class T>
double mean(matrix<T> &mat);

template <class T>
double variance(matrix<T> &mat);

template <class T, class U>
const matrix<T> operator+(const matrix<T> &mat1, const matrix<U> &mat2);

template <class T, class U>
const matrix<T> operator+(const U value, const matrix<T> &mat);

template <class T, class U>
const matrix<T> operator+(const matrix<T> &mat, const U value);

template <class T, class U>
const matrix<T> operator+=(matrix<T> &mat, const U value);

template <class T, class U>
const matrix<T> operator-(const matrix<T> &mat1, const matrix<U> &mat2);

template <class T, class U>
const matrix<T> operator-(const matrix<T> &mat, const U value);

template <class T, class U>
const matrix<T> operator%(const matrix<T> &mat1, const matrix<U> &mat2);

template <class T, class U>
const matrix<T> operator*(const U value, const matrix<T> &mat);

template <class T, class U>
const matrix<T> operator*(const matrix<T> &mat, const U value);

template <class T, class U>
const matrix<T> operator*=(matrix<T> &mat, const U value);

template <class T, class U>
const matrix<T> operator/(const matrix<T> &mat1, const U value);

// IMPLEMENTATION:

template <class T>
matrix<T>::matrix(const int nrows, const int ncols)
{
	assert(nrows >= 0 && ncols >= 0);
	num_rows = nrows;
	num_cols = ncols;
    if (nrows == 0 || ncols == 0)
    {
        data = nullptr;
    }
    else
    {
        data = new T[nrows*ncols];
    }
}

template <class T>
matrix<T>::matrix(const matrix<T> &toCopy)
{
	if(this == &toCopy)
		return;
	
	num_rows = toCopy.num_rows;
	num_cols = toCopy.num_cols;
	data = new T[num_rows*num_cols];

#ifdef _OPENMP
#pragma omp parallel for
#endif
	for(int row = 0; row < num_rows; row++)
		for(int col = 0; col < num_cols; col++)
			data[row*num_cols + col] = 
				toCopy.data[row*num_cols + col];
}

template <class T>
matrix<T>::~matrix()
{
    delete [] data;
}

template <class T>
void matrix<T>::set_size(const int nrows, const int ncols)
{
	assert(nrows >= 0 && ncols >= 0);
	num_rows = nrows;
    num_cols = ncols;
    delete [] data;
    data = new (std::nothrow) T[nrows*ncols];
    if (data == nullptr)
        std::cout << "matrix::set_size memory could not be alloc'd" << std::endl;
}

template <class T>
void matrix<T>::free()
{
	set_size(0,0);
}

template <class T> 
int matrix<T>::nr() const
{
	return num_rows;
}

template <class T>
int matrix<T>::nc() const
{
	return num_cols;
}

template <class T>
T& matrix<T>::operator()(const int row, const int col) const
{
	assert(row < num_rows && col < num_cols);
	return data[row*num_cols + col];
}

template <class T> //template<class U>
matrix<T>& matrix<T>::operator=(const matrix<T> &toCopy)
{
	if(this == &toCopy)
		return *this;
	
	set_size(toCopy.nr(),toCopy.nc());

#ifdef _OPENMP
#pragma omp parallel for shared(toCopy)
#endif
	for(int row = 0; row < num_rows; row++)
		for(int col = 0; col < num_cols; col++)
			data[row*num_cols + col] = 
				toCopy.data[row*num_cols + col];
	return *this;
}

template <class T> template<class U>
matrix<T>& matrix<T>::operator=(const U value)
{

#ifdef _OPENMP
#pragma omp parallel for
#endif
    for(int row = 0; row < num_rows; row++)
		for(int col = 0; col < num_cols; col++)
			data[row*num_cols + col] = value;
	return *this;
}

template <class T> template<class U>
const matrix<T> matrix<T>::add(const matrix<U> &rhs) const
{
	assert(num_rows == rhs.num_rows && num_cols == rhs.num_cols);
	matrix<T> result(num_rows,num_cols);

    T* pdata = data;
    int pnum_cols = num_cols;
#ifdef _OPENMP
#pragma omp parallel for shared(pdata,pnum_cols,result,rhs)
#endif
    for(int row = 0; row < num_rows; row++)
		for(int col = 0; col < num_cols; col++)
			result.data[row*num_cols + col] = 
				data[row*num_cols + col] +
				rhs.data[row*num_cols + col];
	return result;
}

template <class T> template<class U>
const matrix<T> matrix<T>::add(const U value) const
{
	matrix<T> result(num_rows,num_cols);

    T* pdata = data;
    int pnum_cols = num_cols;
#ifdef _OPENMP
#pragma omp parallel for shared(pdata,pnum_cols)
#endif
    for(int row = 0; row < num_rows; row++)
		for(int col = 0; col < num_cols; col++)
			result.data[row*num_cols + col] = 
				data[row*num_cols + col] +
				value;
	return result;
}

template <class T> template<class U>
const matrix<T>& matrix<T>::add_this(const U value)
{
    T* pdata = data;
    int pnum_cols = num_cols;
#ifdef _OPENMP
#pragma omp parallel for shared(pdata,pnum_cols)
#endif
    for(int row = 0; row < num_rows; row++)
        for(int col = 0; col < num_cols; col++)
            data[row*num_cols + col] += value;
    return *this;
}

template <class T> template<class U>
const matrix<T> matrix<T>::subtract(const matrix<U> &rhs) const
{
	assert(num_rows == rhs.num_rows && num_cols == rhs.num_cols);
	matrix<T> result(num_rows,num_cols);

    T* pdata = data;
    int pnum_cols = num_cols;
#ifdef _OPENMP
#pragma omp parallel for shared(pdata,pnum_cols,result,rhs)
#endif
    for(int row = 0; row < num_rows; row++)
		for(int col = 0; col < num_cols; col++)
			result.data[row*num_cols + col] = 
				data[row*num_cols + col] -
				rhs.data[row*num_cols + col];
	return result;
}

template <class T> template<class U>
const matrix<T> matrix<T>::subtract(const U value) const
{
	matrix<T> result(num_rows,num_cols);

    T* pdata = data;
    int pnum_cols = num_cols;
#ifdef _OPENMP
#pragma omp parallel for shared(pdata,pnum_cols,result)
#endif
    for(int row = 0; row < num_rows; row++)
		for(int col = 0; col < num_cols; col++)
			result.data[row*num_cols + col] = 
				data[row*num_cols + col] +
				value;
	return result;
}

template <class T> template <class U>
const matrix<T> matrix<T>::pointmult(const matrix<U> &rhs) const
{
	matrix<T> result(num_rows,num_cols);

#ifdef _OPENMP
#pragma omp parallel for shared(result,rhs)
#endif
    for(int row = 0; row < num_rows; row++)
		for(int col = 0; col < num_cols; col++)
			result.data[row*num_cols + col] = 
				data[row*num_cols + col] *
				rhs.data[row*num_cols + col];
	return result;
}

template <class T> template<class U>
const matrix<T> matrix<T>::mult(const U value) const
{
	matrix<T> result(num_rows,num_cols);

#ifdef _OPENMP
#pragma omp parallel for shared(result)
#endif
    for(int row = 0; row < num_rows; row++)
		for(int col = 0; col < num_cols; col++)
			result.data[row*num_cols + col] = 
				data[row*num_cols + col] *
				value;
	return result;
}

template <class T> template<class U>
const matrix<T>& matrix<T>::mult_this(const U value)
{

#ifdef _OPENMP
#pragma omp parallel for
#endif
    for(int row = 0; row < num_rows; row++)
		for(int col = 0; col < num_cols; col++)
			data[row*num_cols + col] *= value;
	return *this;
}

template <class T> template<class U>
const matrix<T> matrix<T>::divide(const U value) const
{
	matrix<T> result(num_rows,num_cols);

#ifdef _OPENMP
#pragma omp parallel for shared(result)
#endif
    for(int row = 0; row < num_rows; row++)
		for(int col = 0; col < num_cols; col++)
			result.data[row*num_cols + col] = 
				data[row*num_cols + col] /
				value;
	return result;
}

template <class T>
inline void matrix<T>::slow_transpose_to (const matrix<T> &target) const
{
    assert(target.num_rows == num_cols && target.num_cols == num_rows);

#ifdef _OPENMP
#pragma omp parallel for shared(target)
#endif
    for(int row = 0; row < num_rows; row++)
		for(int col = 0; col < num_cols; col++)
			target.data[col*num_rows + row] = 
				data[row*num_cols + col];
}

template<>
inline void matrix<float>::fast_transpose_to (const matrix<float> &target) const
{
    assert(target.num_rows == num_cols && target.num_cols == num_rows);

    transpose_block_SSE4x4(data,target.data,num_rows,num_cols,
                           num_cols,num_rows, 16);
}

//There is no fast transpose in the general case
template <class T>
inline void matrix<T>::fast_transpose_to (const matrix<T> &target) const
{
    slow_transpose_to(target);
}

template <class T>
inline void matrix<T>::transpose_to (const matrix<T> &target) const
{
    slow_transpose_to(target);
}

template<>
inline void matrix<float>::transpose_to (const matrix<float> &target) const
{
    //Fast transpose only work with matricies with dimensions of multiples of 16
    if((num_rows%16 != 0) || (num_cols%16 !=0))
        slow_transpose_to(target);
    else
        fast_transpose_to(target);
}

template<class T>
inline void matrix<T>::transpose4x4_SSE(float *A, float *B, const int lda,
                                        const int ldb) const
{
    __m128 row1 = _mm_load_ps(&A[0*lda]);
    __m128 row2 = _mm_load_ps(&A[1*lda]);
    __m128 row3 = _mm_load_ps(&A[2*lda]);
    __m128 row4 = _mm_load_ps(&A[3*lda]);
     _MM_TRANSPOSE4_PS(row1, row2, row3, row4);
     _mm_store_ps(&B[0*ldb], row1);
     _mm_store_ps(&B[1*ldb], row2);
     _mm_store_ps(&B[2*ldb], row3);
     _mm_store_ps(&B[3*ldb], row4);
}

//block_size = 16 works best
template<class T>
inline void matrix<T>::transpose_block_SSE4x4(float *A, float *B, const int n,
                                   const int m, const int lda, const int ldb,
                                   const int block_size) const
{
#ifdef _OPENMP
    #pragma omp parallel for
#endif
    for(int i=0; i<n; i+=block_size)
        for(int j=0; j<m; j+=block_size)
        {
            int max_i2 = i+block_size < n ? i + block_size : n;
            int max_j2 = j+block_size < m ? j + block_size : m;
            for(int i2=i; i2<max_i2; i2+=4) 
                for(int j2=j; j2<max_j2; j2+=4) 
                    transpose4x4_SSE(&A[i2*lda +j2], &B[j2*ldb + i2], lda, ldb);
                
        }
}

template<class T>
inline void matrix<T>::transpose_scalar_block(float *A, float *B, const int lda, const int ldb, const int block_size) const {
#ifdef _OPENMP
    #pragma omp parallel for
#endif
    for(int i=0; i<block_size; i++) {
        for(int j=0; j<block_size; j++) {
            B[j*ldb + i] = A[i*lda +j];
        }
    }
}

template<class T>
inline void matrix<T>::transpose_block(float *A, float *B, const int n, const int m, const int lda, const int ldb, const int block_size) const {
#ifdef _OPENMP
    #pragma omp parallel for
#endif
    for(int i=0; i<n; i+=block_size) {
        for(int j=0; j<m; j+=block_size) {
            transpose_scalar_block(&A[i*lda +j], &B[j*ldb + i], lda, ldb, block_size);
        }
    }
}

template <class T>
double matrix<T>::sum()
{
	double sum = 0;

#ifdef _OPENMP
#pragma omp parallel for reduction(+:sum)
#endif
    for(int row = 0; row < num_rows; row++)
		for(int col = 0; col < num_cols; col++)
			sum += data[row*num_cols + col];
	return sum;
}

template <class T>
T matrix<T>::max()
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
		for(int col = 0; col < num_cols; col++)
			max = std::max(data[row*num_cols + col],max);
#ifdef _OPENMP
#pragma omp critical
#endif
        {
        shared_max = std::max(shared_max,max);
        }
    }
	return shared_max;
}


template <class T>
T matrix<T>::min()
{
    T shared_min;

    //T* pdata = data;
    //int pnum_cols = num_cols;
#ifdef _OPENMP
#pragma omp parallel //shared(pdata, pnum_cols)
#endif
    {
	T min = std::numeric_limits<T>::max();
#ifdef _OPENMP
#pragma omp for schedule(dynamic)
#endif
    for(int row = 0; row < num_rows; row++)
		for(int col = 0; col < num_cols; col++)
			min = std::min(data[row*num_cols + col],min);
#ifdef _OPENMP
#pragma omp critical
#endif
        {
        shared_min = std::min(shared_min,min);
        }
    }
	return shared_min;
}

template <class T>
double matrix<T>::mean()
{
	assert(num_rows > 0 && num_cols > 0);
	double size = num_rows*num_cols;
	return sum()/size;
}

template <class T>
double matrix<T>::variance()
{
	double m = mean();
	double size = num_rows*num_cols;
	double variance = 0;

#ifdef _OPENMP
#pragma omp parallel for reduction(+:variance)
#endif
    for(int row = 0; row < num_rows; row++)
		for(int col = 0; col < num_cols; col++)
			 variance += pow(data[row*num_cols+col]-m,2);
	return variance/size;
}

//Non object functions

template <class T>
double sum(matrix<T> &mat)
{
	return mat.sum();
}

template <class T>
T max(matrix<T> &mat)
{
	return mat.max();
}

template <class T>
T min(matrix<T> &mat)
{
	return mat.min();
}


template <class T>
double mean(matrix<T> &mat)
{
	return mat.mean();
}

template <class T>
double variance(matrix<T> &mat)
{
	return mat.variance();
}

template <class T, class U>
const matrix<T> operator+(const matrix<T> &mat1, const matrix<U> &mat2)
{
	return mat1.add(mat2);
}

template <class T, class U>
const matrix<T> operator+(const U value, const matrix<T> &mat)
{
	return mat.add(value);
}

template <class T, class U>
const matrix<T> operator+(const matrix<T> &mat, const U value)
{
	return mat.add(value);
}

template <class T, class U>
const matrix<T> operator+=(matrix<T> &mat, const U value)
{
    return mat.add_this(value);
}

template <class T, class U>
const matrix<T> operator-(const matrix<T> &mat1, const matrix<U> &mat2)
{
	return mat1.subtract(mat2);
}

template <class T, class U>
const matrix<T> operator-(const matrix<T> &mat, const U value)
{
	return mat.subtact(value);
}

template <class T, class U>
const matrix<T> operator%(const matrix<T> &mat1, const matrix<U> &mat2)
{
	return mat1.pointmult(mat2);
}

template <class T, class U>
const matrix<T> operator*(const U value, const matrix<T> &mat)
{
	return mat.mult(value);
}

template <class T, class U>
const matrix<T> operator*(const matrix<T> &mat, const U value)
{
	return mat.mult(value);
}

template <class T, class U>
const matrix<T> operator*=(matrix<T> &mat, const U value)
{
	return mat.mult_this(value);
}

template <class T, class U>
const matrix<T> operator/(const matrix<T> &mat, const U value)
{
	return mat.divide(value);
}

#endif //MATRIX_H
