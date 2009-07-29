
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

/** write exif to blob, return length in bytes. blob needs to be as large at 65535 bytes. */
int dt_exif_read_blob(uint8_t *blob, const char* path);

#ifdef __cplusplus
}
#endif

#endif
