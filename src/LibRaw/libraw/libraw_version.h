/* -*- C++ -*-
 * File: libraw_version.h
 * Copyright 2008-2009 LibRaw LLC (info@libraw.org)
 * Created: Mon Sept  8, 2008 
 *
 * LibRaw C++ interface
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 */

#ifndef __VERSION_H
#define __VERSION_H

#define LIBRAW_MAJOR_VERSION  0
#define LIBRAW_MINOR_VERSION  8
#define LIBRAW_PATCH_VERSION  1
#define LIBRAW_VERSION_TAIL   Release

#define _LIBRAW_VERSION_MAKE(a,b,c,d) #a"."#b"."#c"-"#d
#define LIBRAW_VERSION_MAKE(a,b,c,d) _LIBRAW_VERSION_MAKE(a,b,c,d)

#define LIBRAW_VERSION_STR LIBRAW_VERSION_MAKE(LIBRAW_MAJOR_VERSION,LIBRAW_MINOR_VERSION,LIBRAW_PATCH_VERSION,LIBRAW_VERSION_TAIL)

#define LIBRAW_MAKE_VERSION(major,minor,patch) \
    (((major) << 16) | ((minor) << 8) | (patch))

#define LIBRAW_VERSION \
    LIBRAW_MAKE_VERSION(LIBRAW_MAJOR_VERSION,LIBRAW_MINOR_VERSION,LIBRAW_PATCH_VERSION)

#define LIBRAW_CHECK_VERSION(major,minor,patch) \
    ( LibRaw::versionNumber() >= LIBRAW_MAKE_VERSION(major,minor,patch) )


#endif
