#ifndef DT_IMAGE_COMPRESSION
#include <inttypes.h>

/** K. Roimela, T. Aarnio and J. Itäranta. High Dynamic Range Texture Compression. Proceedings of SIGGRAPH 2006. */
void dt_image_compress(const float *in, uint8_t *out, const int32_t width, const int32_t height);
void dt_image_uncompress(const uint8_t *in, float *out, const int32_t width, const int32_t height);

#endif
