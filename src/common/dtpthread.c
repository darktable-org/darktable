#include <sys/resource.h>

#include "common/dtpthread.h"

gboolean dt_openmp_init_stacksize(void)
{
  pthread_attr_t attr;
  size_t stacksize;
  gboolean ret=TRUE;

  pthread_attr_init(&attr);
  pthread_attr_getstacksize(&attr, &stacksize);
  
  const gchar *openmp_stacksize_env=openmp_stacksize_env=g_getenv("OMP_STACKSIZE");
  
  gchar *s=g_strdup_printf("%dB", SAFESTACKSIZE);
  
  if (stacksize < SAFESTACKSIZE && g_strcmp0(openmp_stacksize_env, s) != 0 )
  {
    ret=g_setenv("OMP_STACKSIZE", s, TRUE);
  }
  
  g_free(s);
  
  pthread_attr_destroy(&attr);
  
  return ret;
}

int dt_pthread_create(pthread_t *thread, void *(*start_routine)(void*), void *arg)
{
  pthread_attr_t attr;
  size_t stacksize;
  int ret;
  
  pthread_attr_init(&attr);
  pthread_attr_getstacksize(&attr, &stacksize);
  
  if (stacksize < SAFESTACKSIZE)
  {
    pthread_attr_setstacksize(&attr, SAFESTACKSIZE);
  }
  
  ret=pthread_create(thread, &attr, start_routine, arg);
  
  pthread_attr_destroy(&attr);
  
  return ret;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// // vim: shiftwidth=2 expandtab tabstop=2 cindent
