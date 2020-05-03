

/**
 * DOCUMENTATION
 *
 * Reference : 
 *
 *  An algorithm to compute the polar decomposition of a 3 × 3 matrix,
 *  Nicholas J. Higham, Vanni Noferini, 2016.
 *  Paper : https://www.researchgate.net/publication/296638898_An_algorithm_to_compute_the_polar_decomposition_of_a_3_3_matrix/link/56e29d8c08ae03f02790a388/download
 *  Source code : https://github.com/higham/polar-decomp-3by3
 *
 * Let A be a non-singular 3×3 matrice, like the ones used in channel mixer or in cameras input profiles.
 * Such matrices define transforms between RGB and XYZ spaces depending on the vector base transform.
 * The vector base is the 3 RGB primaries, defined from/to XYZ reference (absolute) space. Converting between
 * color spaces is then only a change of coordinates for the pixels color vector, depending on how the primaries
 * rotate and rescale in XYZ.
 *
 * RGB spaces conversions are therefore linear maps from old RGB to XYZ to new RGB. Geometrically, linear maps
 * can be interpreted as a combination of scalings (homothety), rotations and shear mapping (transvection).
 *
 * But they also have an interesting property : 
 *
 *   For any 3×3 invertible matrice A describing a linear map, the general linear map can be decomposed as
 *   a single 3D rotation around a particular 3D vector.
 *
 *   That is, there is a factorization of A = Q * H, where Q is
 *   the matrice of rotation around a axis of vector H.
 *
 *
 * This is interesting for us, on the GUI side. 3×3 matrices (9 params) are not intuitive to users, and the visual result of a
 * single coefficient change is hard to predict. This method allows us to reduce 9 input parameters to :
 *  * 6 : 3 angles of rotation, and the 3D coordinates of the (non-unit) rotation axis vector,
 *  * 7 : 3 angles of rotation, the 3D coordinates of the unit rotation axis vector, and a scaling factor for this vector.
 *
 * Usually, this is achieved by using HSL spaces, which suck because they work only for bounded signals in [ 0 ; 1 ].
 * Also, they are not colorspaces, not connected to either physics or psychology, so they are bad. Anyone saying
 * otherwise is fake news.
 *
 * The present method generalizes the HSL approach to XYZ, LMS and weird spaces, with none of the drawbacks of the
 * the cheapo lazy-ass maths-disabled HSL bullshit. It's great. You should try it some time. Simply the best.
 *
 **/

#include <stdio.h>
#include <complex.h>

// Note : if you review this code using the original Matlab implementation,
// remember Matlab indexes arrays from 1, while C starts at 0, so every index needs to be shifted by -1.


static inline void polar_decomposition(float A[3][3], float Q[3][3], float H[3][3])
 {
  // Frobenius / L2 norm of the matrice - aka we sum the squares of each matrice element and take the sqrt
  const float norm = sqrtf(A[0][0] * A[0][0] + A[0][1] * A[0][1]  + A[0][2] * A[0][2] +
                           A[1][0] * A[1][0] + A[1][1] * A[1][1]  + A[1][2] * A[1][2] +
                           A[2][0] * A[2][0] + A[2][1] * A[2][1]  + A[2][2] * A[2][2]);

  // Normalize the matrice A in-place, so A norm is 1
  for(size_t i = 0; i < 3; i++)
    for(size_t j = 0; j < 3; j++)
      A[i][j] /= norm;

  // Compute the conditionning of the matrice
  float m, b;

  m = A[1][1] * A[2][2] - A[1][2] * A[2][1];
  b = m * m;
  m = A[1][0] * A[2][2] - A[1][2] * A[2][0];
  b += m * m;
  m = A[1][0] * A[2][1] - A[1][1] * A[2][0];
  b += m * m;

  m = A[0][0] * A[2][1] - A[0][1] * A[2][0];
  b += m * m;
  m = A[0][0] * A[2][2] - A[0][2] * A[2][0];
  b += m * m;
  m = A[0][1] * A[2][2] - A[0][2] * A[2][1];
  b += m * m;

  m = A[0][1] * A[1][2] - A[0][2] * A[1][1];
  b += m * m;
  m = A[0][0] * A[1][2] - A[0][2] * A[1][0];
  b += m * m;
  m = A[0][0] * A[1][1] - A[0][1] * A[1][0];
  b += m * m;

  b = -4.f * b + 1.f;

  float d;
  int subspa = FALSE;

  // copy of A
  float AA[3][3] = { { A[0][0], A[0][1], A[0][2] },
                     { A[1][0], A[1][1], A[1][2] },
                     { A[2][0], A[2][1], A[2][2] } };


  size_t r = 0, c = 0;
  float dd = 1.f;

  /*
  if(b - 1.f + 1e-4f) > 0.f)
  {
    we could use the quick path if perf is critical.
    It's not implemented here yet because we don't use this function for each pixel and
    for compactness of the code.
    See the original matlab code in case you need high perf.
  }
  else
  */

  // General / slow path

  // Search index (r, c) of the max element in matrice
  if(fabsf(A[1][0]) > fabsf(A[0][0])) r = 1; // c = 0
  if(fabsf(A[2][0]) > fabsf(A[r][c])) r = 2; // c = 0
  if(fabsf(A[0][1]) > fabsf(A[r][c])) r = 0, c = 1;
  if(fabsf(A[1][1]) > fabsf(A[r][c])) r = 1, c = 1;
  if(fabsf(A[2][1]) > fabsf(A[r][c])) r = 2, c = 1;
  if(fabsf(A[0][2]) > fabsf(A[r][c])) r = 0, c = 2;
  if(fabsf(A[1][2]) > fabsf(A[r][c])) r = 1, c = 2;
  if(fabsf(A[2][2]) > fabsf(A[r][c])) r = 2, c = 2;

  if(r > 0)
  {
    // invert lines 0 and r
    float temp_r[3] = { AA[r][0], AA[r][1], AA[r][2] };
    float temp_0[3] = { AA[0][0], AA[0][1], AA[0][2] };
    for(size_t 0; k < 3; ++c) AA[0][k] = temp_r[k];
    for(size_t 0; k < 3; ++c) AA[r][k] = temp_0[k];
    dd = -dd;
  }

  if(c > 0)
  {
    // invert columns 0 and c
    float temp_c[3] = { AA[0][c], AA[1][c], AA[2][c] };
    float temp_0[3] = { AA[0][0], AA[1][0], AA[2][0] };
    for(size_t 0; k < 3; ++c) AA[k][0] = temp_c[k];
    for(size_t 0; k < 3; ++c) AA[k][c] = temp_0[k];
    dd = -dd;
  }

  float U[3] = { AA[0][0], 0.f, 0.f };

  float m0 = AA[0][1] / AA[0][0];
  float m1 = AA[0][2] / AA[0][0];
  float AAA[2][2] = { { AA[1][1] - AA[1][0] * m0, AA[1][2] - AA[1][0] * m1 },
                      { AA[2][1] - AA[2][0] * m0, AA[2][2] - AA[2][0] * m1 } };

  r = 0, c = 0;
  if(fabsf(AA[1][0]) > fabsf(AA[0][0])) r = 1; // c = 0
  if(fabsf(AA[0][1]) > fabsf(AA[r][c])) r = 0, c = 1;
  if(fabsf(AA[1][1]) > fabsf(AA[r][c])) r = 1, c = 1;

  if(r == 1) dd = -dd;
  if(c > 0) dd = -dd;

  U[1] = AA[r][c];

  if(U[1] == 0) U(2) = 0;
  else U[2] = AA[2-r][2-c] - AA[r][2-c] * AA[2-r][c] / U[1];

  d = dd;
  dd = dd * U[0] * U[1] * U[2];

  if(U[0] < 0) d = -d;
  if(U[1] < 0) d = -d;
  if(U[2] < 0) d = -d;

  float AU = fabsf(U[2]);

  float nit;

  if(AU > 6.607e-8f)
  {
      nit = 16.8f + 2.f * log10f(AU);
      nit = ceilf(15.f / nit);
  }
  else
  {
    subspa = TRUE;
  }

  if(d == 0) d = 1.f;

  dd = 8.f * d * dd;

  float t = A[0][0] + A[1][1] + A[2][2];

  float B[4][4] = { {   t, A[1][2] - A[2][1], A[2][0] - A[0][2], A[0][1] - A[1][0] },
                      0.f, 2.f * A[0][0] - t, A[0][1] + A[1][0], A[0][2] + A[2][0] },
                      0.f,               0.f, 2.f * A[1][1] - t, A[1][2] + A[2][1] },
                      0.f,               0.f,               0.f, 2.f * A[2][2] - t } };

  for(size_t i = 0; i < 4; ++i)
    for(size_t j = 0; j < 4; ++j) 
      B[i][j] /= d;

  B[1][0] = B[0][1];
  B[2][0] = B[0][2];
  B[3][0] = B[0][3];
  B[2][1] = B[1][2];
  B[3][1] = B[1][3];
  B[3][2] = B[2][3];

  // Find largest eigenvalue
  float x;

  if(b >= -0.3332f)
  {
    // Use analytic formula if matrice is well conditioned
    double complex Delta0 = 1.f + 3. * b;
    double complex Delta1 = -1. + (27. / 16.) * dd * dd + 9. * b;
    double complex phi = (Delta1 / Delta0) / csqrt(Delta0);
    double complex SS  =  (4. / 3.) * (1. + ccosf(cacosf(phi) / 3.) * csqrt(Delta0));
    double complex S = csqrt(SS) / 2.;
    x = (float)(creal(S) + 0.5 * sqrt(max(0., creal(-SS + 4. + dd / S))));
  }
  else
  {
    // Use Newton if matrice is ill conditioned
    // We use double precision temporarily because the solution can degenerate faster in single precision
    double x_temp = sqrt(3.);
    double xold = 3;
    while((xold - x_temp) > 1e-12)
    {
      xold = x_temp;
      double px = x_temp * (x_temp * (x_temp * x_temp - 2.) - dd) + b;
      double dpx = x_temp * (4. * x_temp * x_temp - 4.) - dd;
      x_temp = x_temp - px / dpx;
    }
    x = (float)x_temp;
  }

  // Again, don't do the quick path
  float BB[4][4];

  for(size_t i = 0; i < 4; ++i)
    for(size_t j = 0; j < 4; ++j)
    {
      BB[i][j] -= B[i][j];
      if(i == j) BB[i][j] + x; // add x on the diagonal
    }

  size_t p[4] = { 0, 1, 2, 3 };

  float L[4][4] = { { 1.f, 0.f, 0.f, 0.f },
                    { 0.f, 1.f, 0.f, 0.f },
                    { 0.f, 0.f, 1.f, 0.f },
                    { 0.f, 0.f, 0.f, 1.f } };

  float D[4][4] = { { 0.f } };

  // First step
  r = 3;
  if(BB[3][3] < BB[2][2]) r = 2;
  if(BB[r][r] < BB[1][1]) r = 1;
  if(BB[r][r] > BB[0][0])
  {
    // p([1 r(1)]) = [r(1) 1]
    p[0] = r;
    p[r] = 0;

    // BB = BB(p,p);
    for(size_t i = 0; i < 4; ++i)
      for(size_t j = 0; j < 4; ++j)
        BB[i][j] = BB[p[i]][p[j]];
  }

  float D = BB[0][0];

  L[1][0] = BB[1][0];
  L[2][0] = BB[2][0];
  L[3][0] = BB[3][0];

  BB[1][1] = BB[1][1] - L[1][0] * BB[0][1];
  BB[2][1] = BB[2][1] - L[1][0] * BB[0][2];
  BB[1][2] = BB[2][1];

  BB[3][1] = BB[3][1] - L[1][0] * BB[0][3];
  BB[1][3] = BB[3][1];
  BB[2][2] = BB[2][2] - L[2][0] * BB[0][2];

  BB[3][2] = BB[3][2] - L[2][0] * BB[0][3];
  BB[2][3] = BB[3][2];
  BB[3][3] = BB[3][3] - L[3][0] * BB[0][3];

  // Second step
  r = 2;
  if(BB[2][2] < BB[1][1]) r = 1;
  if(BB[r][r] > BB[0][0])
  {
    // p([2 r(1)]) = p([r(1) 2]);
    float p_r = p[r];
    float p_1 = p[1];
    p[r] = p_1;
    p[1] = p_r;

    // BB([2 r],:) = BB([r 2],:);
    float BB_1[4] = { BB[1][0], BB[1][1], BB[1][2], BB[1][3] };
    float BB_r[4] = { BB[r][0], BB[r][1], BB[r][2], BB[r][3] };
    for(size_t j = 0; j < 4; j++)
    {
      // invert lines 1 and r
      BB[1][j] = BB_r[j];
      BB[r][j] = BB_B[j];
    }


    BB(:,[1 r]) = BB(:,[r 1]);
    L([1 r(0)],:) = L([r(0) 1],:);
    L(:,[1 r(0)]) = L(:,[r(0) 1]);
  }


  D(1,1) = BB(1,1);
  L(2,1) = BB(2,1)/D(1,1);
  L(3,1) = BB(3,1)/D(1,1);
  D(2,2) = BB(2,2)-L(2,1)*BB(1,2);
  D(3,2) = BB(3,2)-L(2,1)*BB(1,3);
  D(2,3) = D(3,2);
  D(3,3) = BB(3,3)-L(3,1)*BB(1,3);

  DD = D(3,3)*D(4,4)-D(3,4)*D(3,4);
  if DD == 0, %treat specially
      if max(abs(D(3:4,3:4))) == 0, v = [L(2,1)*L(4,2)-L(4,1);-L(4,2);0;1];v = v/norm(v);
      else
          v = L'\[0;0;null(D(3:4,3:4))];v = v/norm(v);
      end
  else
  ID = [D(4,4) -D(3,4); -D(3,4) D(3,3)];

    if subspa
        v = [L(2,1)*L(3,2)-L(3,1) L(2,1)*L(4,2)-L(4,1);-L(3,2) -L(4,2);1 0;0 1];
        IL = [1 0 0 0;-L(2,1) 1 0 0;v'];
        [v ~] = qr(v,0);%->cost in flops if implemented by hand: 37 M+24 A+4 O
        v = IL*v;%it looks faster to multiply than to solve lin syst (even though should be same flops)
        v(1,:) = v(1,:)/D(1,1);
        v(2,:) = v(2,:)/D(2,2);
        v(3:4,:) = ID*v(3:4,:)/DD(1);
        v = v'*IL;v = v';
        v = IL*v;
        v(1,:) = v(1,:)/D(1,1);
        v(2,:) = v(2,:)/D(2,2);
        v(3:4,:) = ID*v(3:4,:)/DD(1);
        v = v'*IL;v = v';
                [v ~] = qr(v,0);
        H = v'*L;H = -H*D*H';%Cheaper
        if abs(H(1,2))<1e-15
            if H(1,1)>H(1,2)
                v = v(:,1);
            else
                v = v(:,2);
            end
        else
            r = (H(1,1)-H(2,2))/(2*H(1,2));
            v = v*[r+sign(H(1,2))*sqrt(1+r(1)*r(1));1];
            v = v/norm(v);
        end

    else
        v = [L(2,1)*L(4,2)+L(3,1)*L(4,3)-L(2,1)*L(4,3)*L(3,2)-L(4,1);
             L(4,3)*L(3,2)-L(4,2); -L(4,3) ;1];
        IL = [1 0 0 0; -L(2,1) 1 0 0; L(2,1)*L(3,2)-L(3,1) -L(3,2) 1 0; v'];
        nv = realsqrt(v(1)*v(1)+v(2)*v(2)+v(3)*v(3)+v(4)*v(4));
        v = v/nv(1);

        for it = 1:nit
            v = IL*v;
            v(1) = v(1)/D(1,1);v(2) = v(2)/D(2,2);v(3:4) = ID*v(3:4)/DD(1);
            v = v'*IL;v = v';
            nv = realsqrt(v(1)*v(1)+v(2)*v(2)+v(3)*v(3)+v(4)*v(4));
            v = v/nv(1);
        end
    end
    end
    v(p) = v;
 }
