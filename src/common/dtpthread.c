#include <sys/resource.h>

#include "common/dtpthread.h"

#define MUSLSTACKSIZE 80*1024
#define SAFESTACKSIZE 8*1024*1024

int dt_pthread_create(pthread_t *thread, void *(*start_routine)(void*), void *arg)
{
  pthread_attr_t attr;
  size_t stacksize;
  int ret;
  
  pthread_attr_init(&attr);
  pthread_attr_getstacksize(&attr, &stacksize);
  
  if (stacksize == MUSLSTACKSIZE)
  {
    pthread_attr_setstacksize(&attr, SAFESTACKSIZE);
  }
  
  ret=pthread_create(thread, &attr, start_routine, arg);
  
  pthread_attr_destroy(&attr);
  
  return ret;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// // vim: shiftwidth=2 expandtab tabstop=2 cindent
