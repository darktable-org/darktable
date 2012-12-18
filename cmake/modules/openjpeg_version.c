#include <openjpeg.h>
#include <stdio.h>
int main() {
  printf("%s", opj_version());
  return 0;
}
