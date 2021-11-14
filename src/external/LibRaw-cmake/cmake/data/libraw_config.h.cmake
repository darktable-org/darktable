/* -*- C++ -*-
 * File: libraw_version.h
 * Copyright 2008-2013 LibRaw LLC (info@libraw.org)
 * Created: Mon Sept  8, 2008 
 *
 * LibRaw C++ interface
 *

LibRaw is free software; you can redistribute it and/or modify
it under the terms of the one of two licenses as you choose:

1. GNU LESSER GENERAL PUBLIC LICENSE version 2.1
(See the file LICENSE.LGPL provided in LibRaw distribution archive for details).

2. COMMON DEVELOPMENT AND DISTRIBUTION LICENSE (CDDL) Version 1.0
(See the file LICENSE.CDDL provided in LibRaw distribution archive for details).

 */

#ifndef __LIBRAW_CONFIG_H
#define __LIBRAW_CONFIG_H

/* Define to 1 if LibRaw have been compiled with DNG deflate codec support */
#cmakedefine LIBRAW_USE_DNGDEFLATECODEC 1

/* Define to 1 if LibRaw have been compiled with DNG lossy codec support */
#cmakedefine LIBRAW_USE_DNGLOSSYCODEC 1

/* Define to 1 if LibRaw have been compiled with OpenMP support */
#cmakedefine LIBRAW_USE_OPENMP 1

/* Define to 1 if LibRaw have been compiled with LCMS support */
#cmakedefine LIBRAW_USE_LCMS 1

/* Define to 1 if LibRaw have been compiled with RedCine codec support */
#cmakedefine LIBRAW_USE_REDCINECODEC 1

/* Define to 1 if LibRaw have been compiled with RawSpeed codec support */
#cmakedefine LIBRAW_USE_RAWSPEED 1

/* Define to 1 if LibRaw have been compiled with debug message from dcraw */
#cmakedefine LIBRAW_USE_DCRAW_DEBUG 1

/* Define to 1 if LibRaw have been compiled with Foveon X3F support */
#cmakedefine LIBRAW_USE_X3FTOOLS 1

/* Define to 1 if LibRaw have been compiled with Raspberry Pi RAW support */
#cmakedefine LIBRAW_USE_6BY9RPI 1

#endif
