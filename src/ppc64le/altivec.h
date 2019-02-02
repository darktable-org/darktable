
#include_next <altivec.h>

#undef pixel

#ifdef __cplusplus
#undef bool // type/macro name collisions are bad.
#else
#define bool _Bool // needed for some of the Lua headers, AFAICT.
#endif
