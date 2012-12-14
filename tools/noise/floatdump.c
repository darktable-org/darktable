#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

int main(int argc, char *arg[])
{
  float input;
  fscanf(stdin, "%f", &input);
  fprintf(stdout, "%X", *(uint32_t *)&input);
  exit(0);
}
