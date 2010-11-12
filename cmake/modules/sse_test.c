#include <stdio.h>
#include <xmmintrin.h>
int main (int argc, char **argv)
{
	__m128i c,a,b;
	c=_mm_add_epi32(a,b);
	return 0;
} 