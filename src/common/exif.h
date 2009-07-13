
#ifndef DT_EXIF_H
#define DT_EXIF_H

#include "common/image.h"

/** wrapper around exiv2, C++ */
#ifdef __cplusplus
extern "C"
{
#endif

/** read exif data from file with full path name, store to image struct. returns 0 on success. */
int dt_exif_read(dt_image_t *img, const char* path);

// TODO: write exif to blob, pass on to png/jpg write.
#ifdef __cplusplus
}
#endif

#endif
