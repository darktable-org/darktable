#ifdef HAVE_CONFIG_H
  #include "config.h"
#endif
#ifdef HAVE_GEGL
  #include "develop/pixelpipe_gegl.c"
#else
  #include "develop/pixelpipe_hb.c"
#endif
