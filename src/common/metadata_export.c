/*
    This file is part of darktable,
    copyright (c) 2019 philippe weyland.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "common/darktable.h"
#include "common/debug.h"

const char *dt_export_xmp_keys[]
    = { "Xmp.dc.creator", "Xmp.dc.publisher", "Xmp.dc.title", "Xmp.dc.description", "Xmp.dc.rights",
        "Xmp.dc.subject",

        "Xmp.exif.GPSLatitude", "Xmp.exif.GPSLongitude", "Xmp.exif.GPSAltitude",
        "Xmp.exif.DateTimeOriginal",
        "Xmp.exifEX.LensModel",

        "Exif.Image.DateTimeOriginal", "Exif.Image.Make", "Exif.Image.Model", "Exif.Image.Orientation",
        "Exif.Image.Artist", "Exif.Image.Copyright", "Exif.Image.Rating",

        "Exif.GPSInfo.GPSLatitude", "Exif.GPSInfo.GPSLongitude", "Exif.GPSInfo.GPSAltitude",
        "Exif.GPSInfo.GPSLatitudeRef", "Exif.GPSInfo.GPSLongitudeRef", "Exif.GPSInfo.GPSAltitudeRef",
        "Exif.GPSInfo.GPSVersionID",

        "Exif.Photo.DateTimeOriginal", "Exif.Photo.ExposureTime", "Exif.Photo.ShutterSpeedValue",
        "Exif.Photo.FNumber", "Exif.Photo.ApertureValue", "Exif.Photo.ISOSpeedRatings",
        "Exif.Photo.FocalLengthIn35mmFilm", "Exif.Photo.LensModel", "Exif.Photo.Flash",
        "Exif.Photo.WhiteBalance", "Exif.Photo.UserComment", "Exif.Photo.ColorSpace",

        "Xmp.xmp.CreateDate", "Xmp.xmp.CreatorTool", "Xmp.xmp.Identifier", "Xmp.xmp.Label", "Xmp.xmp.ModifyDate",
        "Xmp.xmp.Nickname", "Xmp.xmp.Rating",

        "Iptc.Application2.Subject", "Iptc.Application2.Keywords", "Iptc.Application2.LocationName",
        "Iptc.Application2.City", "Iptc.Application2.SubLocation", "Iptc.Application2.ProvinceState",
        "Iptc.Application2.CountryName", "Iptc.Application2.Copyright", "Iptc.Application2.Caption",
        "Iptc.Application2.Byline", "Iptc.Application2.ObjectName",

        "Xmp.tiff.ImageWidth","Xmp.tiff.ImageLength","Xmp.tiff.Artist", "Xmp.tiff.Copyright"
       };
const guint dt_export_xmp_keys_n = G_N_ELEMENTS(dt_export_xmp_keys);

// TODO replace the following list by a dynamic exiv2 list able to provide info type
// Here are listed only string or XmpText. Can be added as needed.
const char **dt_lib_export_metadata_get_export_keys(guint *dt_export_keys_n)
{
  *dt_export_keys_n = dt_export_xmp_keys_n;
  return dt_export_xmp_keys;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
