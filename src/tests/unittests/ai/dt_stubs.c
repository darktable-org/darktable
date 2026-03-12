/*
    Minimal stubs for darktable symbols required by libdarktable_ai.
    Provides just enough for the AI backend to link and run without
    the full darktable runtime.
*/

#include <glib.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* darktable global — the AI backend accesses darktable.unmuted
   via the dt_debug_if macro.  Set all bits so debug output is enabled. */
char darktable[8192] __attribute__((aligned(16)));

/* Enable all debug output: set unmuted to 0xFFFFFFFF.
   darktable_t layout: dt_codepath_t (4 bytes), int32_t num_openmp_threads (4 bytes),
   int32_t unmuted (offset 8). */
__attribute__((constructor))
static void _init_darktable_stub(void)
{
  /* Set unmuted field at offset 8 to all-bits-on */
  int32_t *unmuted = (int32_t *)(darktable + 8);
  *unmuted = 0x7FFFFFFF;
}

void dt_print_ext(const char *msg, ...)
{
  va_list ap;
  va_start(ap, msg);
  vfprintf(stderr, msg, ap);
  va_end(ap);
  fputc('\n', stderr);
}

gchar *dt_conf_get_string(const char *name)
{
  return g_strdup("cpu");
}

void dt_loc_get_user_config_dir(char *configdir, size_t bufsize)
{
  if(configdir && bufsize > 0) configdir[0] = '\0';
}
