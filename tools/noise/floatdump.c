#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

int main(int argc, char *arg[])
{
  float input;
  fscanf(stdin, "%f", &input);
  // silly bugger needs big endian floats, so swap:
  uint32_t i = *(uint32_t *)&input;
  i = ((i & 0xffff0000u) >> 16) | ((i & 0x0000ffffu) << 16);
  i = ((i & 0xff00ff00u) >> 8) | ((i & 0x00ff00ffu) << 8);
  fprintf(stdout, "%08X", i);
  exit(0);
}
