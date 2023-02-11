#pragma once

#if !defined(_RELEASE) && !defined(__cplusplus) && !defined(_WIN32)

//
// We needed to poison certain functions in order to disallow their usage
// but not in bundled libs
//

// this is ugly, but needed, because else compilation will fail with:
// darktable/src/common/poison.h:16:20: error: poisoning existing macro "strncat" [-Werror]
//  #pragma GCC poison strncat  // use g_strncat
#pragma GCC system_header

//#pragma GCC poison sprintf  // use snprintf
#pragma GCC poison vsprintf // use vsnprintf
#pragma GCC poison strcpy   // use g_strlcpy
//#pragma GCC poison strncpy  // use g_strlcpy
#pragma GCC poison strcat  // use g_strncat
#pragma GCC poison strncat // use g_strncat
#pragma GCC poison pthread_create // use dt_pthread_create, musl issues
#pragma GCC poison fopen // use g_fopen
// #pragma GCC poison open // use g_open -- this one doesn't work
#pragma GCC poison unlink // use g_unlink

#endif

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
