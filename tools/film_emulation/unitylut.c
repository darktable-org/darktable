#include <stdio.h>
#include <stdlib.h>


int main(int argc, char *arg[])
{
  FILE *f = fopen("unity.pfm", "wb");
  fprintf(f, "PF\n512 512\n-1.0\n");
  float *buf = (float *)malloc(512*512*3*sizeof(float));
  for(int k=0;k<64;k++)
  for(int j=0;j<64;j++)
  for(int i=0;i<64;i++)
  {
    int x = i + (k&7)*64;
    int y = j + (k/8)*64;

    buf[(x + 512*y)*3 + 0] = i/63.0f;
    buf[(x + 512*y)*3 + 1] = j/63.0f;
    buf[(x + 512*y)*3 + 2] = k/63.0f;
  }
  fwrite(buf, sizeof(float), 3*512*512, f);
  fclose(f);
  exit(0);
}
