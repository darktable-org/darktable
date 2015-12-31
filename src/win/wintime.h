#ifndef __WINTIME_H__
#define __WINTIME_H__

#include <time.h>

extern struct tm *localtime_r(const time_t *timep, struct tm *result);
extern struct tm *gmtime_r(const time_t *timep, struct tm *result);

#endif
