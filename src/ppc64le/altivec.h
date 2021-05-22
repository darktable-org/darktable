
#ifndef __APPLE_ALTIVEC__
/* Prevent gcc from defining the keywords as macros. Do not manually
 * undef for c99 stdbool.h compat.
 */
  #define __APPLE_ALTIVEC__ 1
  #include_next <altivec.h>
  #undef __APPLE_ALTIVEC__
#else
  #include_next <altivec.h>
#endif
