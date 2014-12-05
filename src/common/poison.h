#ifndef _RELEASE
#ifndef POISON_H
#define POISON_H
//
// We needed to poison certaion functions in order to disallow their usage
// but not in bundled libs
//

// this is ugly, but needed, because else compilation will fail with:
// darktable/src/common/poison.h:16:20: error: poisoning existing macro "strncat" [-Werror]
//  #pragma GCC poison strncat  // use g_strncat
#pragma GCC system_header

//#pragma GCC poison sprintf  // use snprintf
#pragma GCC poison vsprintf // use vsnprintf
#pragma GCC poison strcpy   // use g_strncpy
//#pragma GCC poison strncpy  // use g_strncpy
#pragma GCC poison strcat  // use g_strncat
#pragma GCC poison strncat // use g_strncat
#endif
#endif
