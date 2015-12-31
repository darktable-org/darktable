#include "wintime.h"
#include <string.h>

struct tm *localtime_r(const time_t *timep, struct tm *result)
{
  struct tm *ret = localtime(timep);

  if (ret)
  {
    memcpy(result, ret, sizeof(struct tm));
  }
  return result;
}

struct tm *gmtime_r(const time_t *timep, struct tm *result)
{
  struct tm *ret = gmtime(timep);

  if (ret)
  {
    memcpy(result, ret, sizeof(struct tm));
  }

  return result;
}
