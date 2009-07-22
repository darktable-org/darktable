
// TODO: remove assertions

// edge-avoiding wavelet:
#define gweight(i, j, ii, jj) 1.0/(fabsf(weight_a[l][(width>>(l-1))*((j)>>(l-1)) + ((i)>>(l-1))] - weight_a[l][(width>>(l-1))*((jj)>>(l-1)) + ((ii)>>(l-1))])+1.e-5)
// std cdf(2,2) wavelet:
// #define gweight(i, j, ii, jj) 1.0
#define gbuf(BUF, A, B) ((BUF)[3*width*((B)) + 3*((A)) + ch])


void dt_iop_equalizer_wtf(float *buf, float **weight_a, const int l, const int width, const int height, const int ch)
{
  if(ch == 0)
  { // store weights for luma channel only, chroma uses same basis.
    memset(weight_a[l], 0, sizeof(float)*(width>>(l-1))*(height>>(l-1)));
    for(int j=0;j<height>>(l-1);j++) for(int i=0;i<width>>(l-1);i++) weight_a[l][j*(width>>(l-1))+i] = gbuf(buf, i<<(l-1), j<<(l-1));
    // printf("storing weights for %d X %d to level %d\n", width>>(l-1), height>>(l-1), l);
  }

  assert(l > 0); // 1 is first level.
  const int step = 1<<l;
  const int st = step/2;

  for(int j=0;j<height;j++)
  { // rows
    // predict, get detail
    int i = st;
    for(;i<width-st;i+=step)
      gbuf(buf, i, j) -= (gweight(i, j, i-st, j)*gbuf(buf, i-st, j) + gweight(i, j, i+st, j)*gbuf(buf, i+st, j))
        /(gweight(i, j, i-st, j) + gweight(i, j, i+st, j));
    assert(i-st >=0);
    if(i < width) gbuf(buf, i, j) -= gbuf(buf, i-st, j);
    // update coarse
    assert(width > st);
    gbuf(buf, 0, j) += gbuf(buf, st, j)*0.5;
    for(i=step;i<width-st;i+=step)
      gbuf(buf, i, j) += (gweight(i, j, i-st, j)*gbuf(buf, i-st, j) + gweight(i, j, i+st, j)*gbuf(buf, i+st, j))
        /(2.0*(gweight(i, j, i-st, j)+gweight(i, j, i+st, j)));
  }
  for(int i=0;i<width;i++)
  { // cols
    int j = st;
    // predict, get detail
    for(;j<height-st;j+=step)
      gbuf(buf, i, j) -= (gweight(i, j, i, j-st)*gbuf(buf, i, j-st) + gweight(i, j, i, j+st)*gbuf(buf, i, j+st))
        /(gweight(i, j, i, j-st) + gweight(i, j, i, j+st));
    assert(j-st >=0);
    if(j < height) gbuf(buf, i, j) -= gbuf(buf, i, j-st);
    // update
    assert(height > st);
    gbuf(buf, i, 0) += gbuf(buf, i, st)*0.5;
    for(j=step;j<height-st;j+=step)
      gbuf(buf, i, j) += (gweight(i, j, i, j-st)*gbuf(buf, i, j-st) + gweight(i, j, i, j+st)*gbuf(buf, i, j+st))
        /(2.0*(gweight(i, j, i, j-st) + gweight(i, j, i, j+st)));
  }
}

void dt_iop_equalizer_iwtf(float *buf, float **weight_a, const int l, const int width, const int height, const int ch)
{
  assert(l > 0); // 1 is first level.
  const int step = 1<<l;
  const int st = step/2;

  for(int i=0;i<width;i++)
  { //cols
    // update coarse
    assert(height > st);
    gbuf(buf, i, 0) -= gbuf(buf, i, st)*0.5f;
    for(int j=step;j<height-st;j+=step)
      gbuf(buf, i, j) -= (gweight(i, j, i, j-st)*gbuf(buf, i, j-st) + gweight(i, j, i, j+st)*gbuf(buf, i, j+st))
        /(2.0*(gweight(i, j, i, j-st) + gweight(i, j, i, j+st)));
    // predict
    int j=st;
    for(;j<height-st;j+=step)
      gbuf(buf, i, j) += (gweight(i, j, i, j-st)*gbuf(buf, i, j-st) + gweight(i, j, i, j+st)*gbuf(buf, i, j+st))
        /(gweight(i, j, i, j-st) + gweight(i, j, i, j+st));
    assert(j-st >= 0);
    if(j < height) gbuf(buf, i, j) += gbuf(buf, i, j-st);
  }
  for(int j=0;j<height;j++)
  { // rows
    // update
    assert(st < width);
    gbuf(buf, 0, j) -= gbuf(buf, st, j)*0.5f;
    for(int i=step;i<width-st;i+=step)
      gbuf(buf, i, j) -= (gweight(i, j, i-st, j)*gbuf(buf, i-st, j) + gweight(i, j, i+st, j)*gbuf(buf, i+st, j))
        /(2.0*(gweight(i, j, i-st, j)+gweight(i, j, i+st, j)));
    // predict
    int i = st;
    for(;i<width-st;i+=step)
      gbuf(buf, i, j) += (gweight(i, j, i-st, j)*gbuf(buf, i-st, j) + gweight(i, j, i+st, j)*gbuf(buf, i+st, j))
        /(gweight(i, j, i-st, j) + gweight(i, j, i+st, j));
    assert(i-st >= 0);
    if(i < width) gbuf(buf, i, j) += gbuf(buf, i-st, j);
  }
}

#undef gbuf
#undef gweight
