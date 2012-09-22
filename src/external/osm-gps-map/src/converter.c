/* -*- Mode: C; indent-tabs-mode: nil; c-basic-offset: 4; tab-width: 4 -*- */
/* vim:set et sw=4 ts=4 cino=t0,(0: */
/*
 * converter.c
 * Copyright (C) Marcus Bauer 2008 <marcus.bauer@gmail.com>
 * Copyright (C) John Stowers 2008 <john.stowers@gmail.com>
 *
 * This is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 2.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include <math.h>
#include <stdio.h>

#include "private.h"
#include "converter.h"


float
deg2rad(float deg)
{
    return (deg * M_PI / 180.0);
}

float
rad2deg(float rad)
{
    return (rad / M_PI * 180.0);
}


int
lat2pixel(  int zoom,
            float lat)
{
    float lat_m;
    int pixel_y;

    lat_m = atanh(sin(lat));

    /* the formula is
     *
     * pixel_y = -(2^zoom * TILESIZE * lat_m) / 2PI + (2^zoom * TILESIZE) / 2
     */
    pixel_y = -(int)( (lat_m * TILESIZE * (1 << zoom) ) / (2*M_PI)) +
        ((1 << zoom) * (TILESIZE/2) );


    return pixel_y;
}


int
lon2pixel(  int zoom,
            float lon)
{
    int pixel_x;

    /* the formula is
     *
     * pixel_x = (2^zoom * TILESIZE * lon) / 2PI + (2^zoom * TILESIZE) / 2
     */
    pixel_x = (int)(( lon * TILESIZE * (1 << zoom) ) / (2*M_PI)) +
        ( (1 << zoom) * (TILESIZE/2) );
    return pixel_x;
}

float
pixel2lon(  float zoom,
            int pixel_x)
{
    float lon;

    lon = ((pixel_x - ( exp(zoom * M_LN2) * (TILESIZE/2) ) ) *2*M_PI) / 
        (TILESIZE * exp(zoom * M_LN2) );

    return lon;
}

float
pixel2lat(  float zoom,
            int pixel_y)
{
    float lat, lat_m;

    lat_m = (-( pixel_y - ( exp(zoom * M_LN2) * (TILESIZE/2) ) ) * (2*M_PI)) /
        (TILESIZE * exp(zoom * M_LN2));

    lat = asin(tanh(lat_m));

    return lat;
}
