#include <sys/resource.h>

#include "common/dtpthread.h"

void dt_pthread_attr_init(pthread_attr_t *attr)
{
  pthread_attr_init(attr);
#if !defined(__GLIBC__)
  size_t stacksize;
  struct rlimit rlim;

  pthread_attr_getstacksize(attr, &stacksize);
  getrlimit(RLIMIT_STACK, &rlim);

  if ( stacksize < rlim.rlim_cur )
  {
    pthread_attr_setstacksize(attr, rlim.rlim_cur);
  }
#endif
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// // vim: shiftwidth=2 expandtab tabstop=2 cindent
