#include <stdio.h>
#include <math.h>

void QR_dec( double *A, double *Q, double *R, int rows, int cols) {
	// The function decomposes the input matrix A into the matrices Q and R: one simmetric, one orthonormal and one upper triangular, by using the Gram-Schmidt method.
	// The input matrice A is defined as A[rows][cols], so are the output matrices Q and R.
	// This function is meant to be used in the polar decomposition algorithm and has been tested with different sizes of input matrices. 
	// Supposedly the algorithm works with any matrix, as long as the columns vectors are independent.
	//
	// For tests for the standalone function please refer to the original github repo this has been developed in. 
	// https://github.com/DavidePatria/QR_decomposition_C/blob/main/README.md
	
	// As already mentioned in the README the matrices orders are: A mxn => Q mxn , R nxn and rank(A) must be n
	// The matrix A[m x n] = [A_00, A_01, ... A_0n;  ...... ; A_m0, ... , A_mn] can be accessed as a vector that has 
	// all its rows consecutively written in a long vector, even if passed as a *A and defined as A[m][n].
	 

	//vectors for internal coputations
	double T[rows];
	double S[rows];
	double norm;
	int i,ii, j, jj, k, kk;
	double r;

	for (i=0; i<cols; i++) {
		printf("\n");
		
		// scrolling a column and copying it
		for(ii=0; ii<rows; ii++) {
			Q[ii*cols +i] = A[ii*cols + i] ;

		}

		for(j=0; j<i; j++) {

			//copying columns into auxiliary variables
			for(jj=0; jj<rows; jj++) {
				T[jj] = Q[cols*jj + j];
				S[jj] = A[cols*jj + i];
			}
		
			//temporary storing T*K in r
			r = 0;
			for(k=0; k<rows; k++) {
				r += T[k] * S[k];
			}

			// setting R[j][i] to r
			R[cols*j + i] = r;

			for(kk=0; kk<rows; kk++) {
				//multiplying vector T by r
				T[kk] *= r;
				//subtract T[kk] from i-th column of Q
				Q[cols*kk + i] -= T[kk];
			}
		}

		// rezeroing norm at each cycle
		norm = 0;
		// norm of the i-th column
		for(k=0; k<rows; k++) {
			// computing norm^2
			norm += Q[cols*k + i]*Q[cols*k + i];
		}
		norm = sqrt(norm);

		// assigning i-th element of R diagonal 
		R[cols*i + i] = norm;

		for(k=0; k<rows; k++) {
			Q[cols*k + i] /= R[cols*i + i];
		}
	}
}
