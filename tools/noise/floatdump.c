#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

int main(int argc, char *arg[])
{
  // silly bugger needs big endian floats, so swap:
  union { float f; uint32_t i;} input;
  fscanf(stdin, "%f", &input.f);
  input.i = ((input.i & 0xffff0000u) >> 16) | ((input.i & 0x0000ffffu) << 16);
  input.i = ((input.i & 0xff00ff00u) >>  8) | ((input.i & 0x00ff00ffu) <<  8);
  fprintf(stdout, "%08X", input.i);
  return 0;
}
