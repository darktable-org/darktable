
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
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

