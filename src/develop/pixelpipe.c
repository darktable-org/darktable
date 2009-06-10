#ifdef HAVE_GEGL
  #include "develop/pixelpipe_gegl.c"
#else
  #include "develop/pixelpipe_hb.c"
#endif
