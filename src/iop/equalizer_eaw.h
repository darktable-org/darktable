
// float dreggn(const int i, const int j, const int ii, const int jj, int l, float ** weight_a, const int wd)
// {
//   // assert((j>>(l-1)) < (height>>(l-1)));
//   assert((i>>(l-1)) < wd);
//   return 1.0/(fabsf(weight_a[l][wd*((j)>>(l-1)) + ((i)>>(l-1))] - weight_a[l][wd*((jj)>>(l-1)) + ((ii)>>(l-1))])+1.e-5);
// }
// #define gweight(i, j, ii, jj) dreggn(i, j, ii, jj, l, weight_a, wd)
// edge-avoiding wavelet:
#define gweight(i, j, ii, jj) 1.0/(fabsf(weight_a[l][wd*((j)>>(l-1)) + ((i)>>(l-1))] - weight_a[l][wd*((jj)>>(l-1)) + ((ii)>>(l-1))])+1.e-5)
// std cdf(2,2) wavelet:
// #define gweight(i, j, ii, jj) 1.0
#define gbuf(BUF, A, B) ((BUF)[3*width*((B)) + 3*((A)) + ch])


void dt_iop_equalizer_wtf(float *buf, float **weight_a, const int l, const int width, const int height)
{
  const int wd = (int)(1 + (width>>(l-1))), ht = (int)(1 + (height>>(l-1)));
  int ch = 0;
  // store weights for luma channel only, chroma uses same basis.
  // const int wd = (width+(1<<(l-2)))>>(l-1), ht = (height+(1<<(l-2)))>>(l-1);
  memset(weight_a[l], 0, sizeof(float)*wd*ht);
  for(int j=0;j<ht;j++) for(int i=0;i<wd;i++) weight_a[l][j*wd+i] = gbuf(buf, i<<(l-1), j<<(l-1));
  // printf("storing weights for %d X %d to level %d\n", width>>(l-1), height>>(l-1), l);

  const int step = 1<<l;
  const int st = step/2;

  for(int j=0;j<height;j++)
  { // rows
    // predict, get detail
    int i = st;
    for(;i<width-st;i+=step) for(ch=0;ch<3;ch++)
      gbuf(buf, i, j) -= (gweight(i, j, i-st, j)*gbuf(buf, i-st, j) + gweight(i, j, i+st, j)*gbuf(buf, i+st, j))
        /(gweight(i, j, i-st, j) + gweight(i, j, i+st, j));
    if(i < width) for(ch=0;ch<3;ch++) gbuf(buf, i, j) -= gbuf(buf, i-st, j);
    // update coarse
    for(ch=0;ch<3;ch++) gbuf(buf, 0, j) += gbuf(buf, st, j)*0.5;
    for(i=step;i<width-st;i+=step) for(ch=0;ch<3;ch++) 
      gbuf(buf, i, j) += (gweight(i, j, i-st, j)*gbuf(buf, i-st, j) + gweight(i, j, i+st, j)*gbuf(buf, i+st, j))
        /(2.0*(gweight(i, j, i-st, j)+gweight(i, j, i+st, j)));
  }
  for(int i=0;i<width;i++)
  { // cols
    int j = st;
    // predict, get detail
    for(;j<height-st;j+=step) for(ch=0;ch<3;ch++) 
      gbuf(buf, i, j) -= (gweight(i, j, i, j-st)*gbuf(buf, i, j-st) + gweight(i, j, i, j+st)*gbuf(buf, i, j+st))
        /(gweight(i, j, i, j-st) + gweight(i, j, i, j+st));
    if(j < height) for(ch=0;ch<3;ch++) gbuf(buf, i, j) -= gbuf(buf, i, j-st);
    // update
    for(ch=0;ch<3;ch++) gbuf(buf, i, 0) += gbuf(buf, i, st)*0.5;
    for(j=step;j<height-st;j+=step) for(ch=0;ch<3;ch++) 
      gbuf(buf, i, j) += (gweight(i, j, i, j-st)*gbuf(buf, i, j-st) + gweight(i, j, i, j+st)*gbuf(buf, i, j+st))
        /(2.0*(gweight(i, j, i, j-st) + gweight(i, j, i, j+st)));
  }
}

void dt_iop_equalizer_iwtf(float *buf, float **weight_a, const int l, const int width, const int height)
{
  const int step = 1<<l;
  const int st = step/2;
  const int wd = (int)(1 + (width>>(l-1)));

  for(int i=0;i<width;i++)
  { //cols
    // update coarse
    for(int ch=0;ch<3;ch++) gbuf(buf, i, 0) -= gbuf(buf, i, st)*0.5f;
    for(int j=step;j<height-st;j+=step) for(int ch=0;ch<3;ch++) 
      gbuf(buf, i, j) -= (gweight(i, j, i, j-st)*gbuf(buf, i, j-st) + gweight(i, j, i, j+st)*gbuf(buf, i, j+st))
        /(2.0*(gweight(i, j, i, j-st) + gweight(i, j, i, j+st)));
    // predict
    int j=st;
    for(;j<height-st;j+=step) for(int ch=0;ch<3;ch++)
      gbuf(buf, i, j) += (gweight(i, j, i, j-st)*gbuf(buf, i, j-st) + gweight(i, j, i, j+st)*gbuf(buf, i, j+st))
        /(gweight(i, j, i, j-st) + gweight(i, j, i, j+st));
    if(j < height) for(int ch=0;ch<3;ch++) gbuf(buf, i, j) += gbuf(buf, i, j-st);
  }
  for(int j=0;j<height;j++)
  { // rows
    // update
    for(int ch=0;ch<3;ch++) gbuf(buf, 0, j) -= gbuf(buf, st, j)*0.5f;
    for(int i=step;i<width-st;i+=step) for(int ch=0;ch<3;ch++)
      gbuf(buf, i, j) -= (gweight(i, j, i-st, j)*gbuf(buf, i-st, j) + gweight(i, j, i+st, j)*gbuf(buf, i+st, j))
        /(2.0*(gweight(i, j, i-st, j)+gweight(i, j, i+st, j)));
    // predict
    int i = st;
    for(;i<width-st;i+=step) for(int ch=0;ch<3;ch++)
      gbuf(buf, i, j) += (gweight(i, j, i-st, j)*gbuf(buf, i-st, j) + gweight(i, j, i+st, j)*gbuf(buf, i+st, j))
        /(gweight(i, j, i-st, j) + gweight(i, j, i+st, j));
    if(i < width) for(int ch=0;ch<3;ch++) gbuf(buf, i, j) += gbuf(buf, i-st, j);
  }
}

#undef gbuf
#undef gweight
