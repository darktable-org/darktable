/*
   This file is part of darktable,
   copyright (c) 2009--2013 johannes hanika.
   copyright (c) 2011 henrik andersson.
   copyright (c) 2012 tobias ellinghaus.

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

#define __STDC_FORMAT_MACROS

extern "C" {
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "version.h"

#include <glib.h>
#include <zlib.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sqlite3.h>
}

#include <cmath>
#include <iostream>
#include <fstream>
#include <string>
#include <sstream>
#include <cassert>

#include <exiv2/easyaccess.hpp>
#include <exiv2/xmp.hpp>
#include <exiv2/error.hpp>
#include <exiv2/image.hpp>
#include <exiv2/exif.hpp>
#include <exiv2/preview.hpp>

using namespace std;

extern "C" {
#include "common/exif.h"
#include "common/darktable.h"
#include "common/colorlabels.h"
#include "common/imageio_jpeg.h"
#include "common/image_cache.h"
#include "common/imageio.h"
#include "common/metadata.h"
#include "common/tags.h"
#include "common/debug.h"
#include "control/conf.h"
#include "develop/imageop.h"
}

static void _exif_import_tags(dt_image_t *img, Exiv2::XmpData::iterator &pos);

// this array should contain all XmpBag and XmpSeq keys used by dt
const char *dt_xmp_keys[]
    = { "Xmp.dc.subject", "Xmp.lr.hierarchicalSubject", "Xmp.darktable.colorlabels",
        "Xmp.darktable.history_modversion", "Xmp.darktable.history_enabled",
        "Xmp.darktable.history_operation", "Xmp.darktable.history_params", "Xmp.darktable.blendop_params",
        "Xmp.darktable.blendop_version", "Xmp.darktable.multi_priority", "Xmp.darktable.multi_name",
        "Xmp.dc.creator", "Xmp.dc.publisher", "Xmp.dc.title", "Xmp.dc.description", "Xmp.dc.rights" };

static const guint dt_xmp_keys_n = G_N_ELEMENTS(dt_xmp_keys); // the number of XmpBag XmpSeq keys that dt uses


/* a few helper functions inspired by
   https://projects.kde.org/projects/kde/kdegraphics/libs/libkexiv2/repository/revisions/master/entry/libkexiv2/kexiv2gps.cpp
   */

static double _gps_string_to_number(const gchar *input)
{
  double res = 0;
  gchar *s = g_strdup(input);
  gchar dir = toupper(s[strlen(s) - 1]);
  gchar **list = g_strsplit(s, ",", 0);
  if(list)
  {
    if(list[2] == NULL) // format DDD,MM.mm{N|S}
      res = g_ascii_strtoll(list[0], NULL, 10) + (g_ascii_strtod(list[1], NULL) / 60.0);
    else if(list[3] == NULL) // format DDD,MM,SS{N|S}
      res = g_ascii_strtoll(list[0], NULL, 10) + (g_ascii_strtoll(list[1], NULL, 10) / 60.0)
            + (g_ascii_strtoll(list[2], NULL, 10) / 3600.0);
    if(dir == 'S' || dir == 'W') res *= -1.0;
  }
  g_strfreev(list);
  g_free(s);
  return res;
}

static gboolean _gps_rationale_to_number(const double r0_1, const double r0_2, const double r1_1,
                                         const double r1_2, const double r2_1, const double r2_2, char sign,
                                         double *result)
{
  if(!result) return FALSE;
  double res = 0.0;
  // Latitude decoding from Exif.
  double num, den, min, sec;
  num = r0_1;
  den = r0_2;
  if(den == 0) return FALSE;
  res = num / den;

  num = r1_1;
  den = r1_2;
  if(den == 0) return FALSE;
  min = num / den;
  if(min != -1.0) res += min / 60.0;

  num = r2_1;
  den = r2_2;
  if(den == 0)
  {
    // be relaxed and accept 0/0 seconds. See #246077.
    if(num == 0)
      den = 1;
    else
      return FALSE;
  }
  sec = num / den;
  if(sec != -1.0) res += sec / 3600.0;

  if(sign == 'S' || sign == 'W') res *= -1.0;

  *result = res;
  return TRUE;
}

// inspired by ufraw_exiv2.cc:

static void dt_strlcpy_to_utf8(char *dest, size_t dest_max, Exiv2::ExifData::const_iterator &pos,
                               Exiv2::ExifData &exifData)
{
  std::string str = pos->print(&exifData);

  char *s = g_locale_to_utf8(str.c_str(), str.length(), NULL, NULL, NULL);
  if(s != NULL)
  {
    g_strlcpy(dest, s, dest_max);
    g_free(s);
  }
  else
  {
    g_strlcpy(dest, str.c_str(), dest_max);
  }
}

// function to remove known dt keys from xmpdata, so not to append them twice
// this should work because dt first reads all known keys
static void dt_remove_known_keys(Exiv2::XmpData &xmp)
{
  for(unsigned int i = 0; i < dt_xmp_keys_n; i++)
  {
    Exiv2::XmpData::iterator pos = xmp.findKey(Exiv2::XmpKey(dt_xmp_keys[i]));
    if(pos != xmp.end()) xmp.erase(pos);
  }
}

// FIXME: according to http://www.exiv2.org/doc/classExiv2_1_1Metadatum.html#63c2b87249ba96679c29e01218169124
// there is no need to pass xmpData
static bool dt_exif_read_xmp_data(dt_image_t *img, Exiv2::XmpData &xmpData, bool look_for_version,
                                  bool use_defaul_rating)
{
  try
  {
    Exiv2::XmpData::iterator pos;

    int version = look_for_version ? 0 : 1;
    if((pos = xmpData.findKey(Exiv2::XmpKey("Xmp.darktable.xmp_version"))) != xmpData.end())
    {
      version = pos->toLong();
    }

    // older darktable version did not write this data correctly:
    // the reasoning behind strdup'ing all the strings before passing it to sqlite3 is, that
    // they are somehow corrupt after the call to sqlite3_prepare_v2() -- don't ask me
    // why for they don't get passed to that function.
    if(version > 0)
    {
      if((pos = xmpData.findKey(Exiv2::XmpKey("Xmp.dc.rights"))) != xmpData.end())
      {
        // rights
        char *rights = strdup(pos->toString().c_str());
        char *adr = rights;
        if(strncmp(rights, "lang=", 5) == 0)
        {
          rights = strchr(rights, ' ');
          if(rights != NULL) rights++;
        }
        dt_metadata_set(img->id, "Xmp.dc.rights", rights);
        free(adr);
      }
      if((pos = xmpData.findKey(Exiv2::XmpKey("Xmp.dc.description"))) != xmpData.end())
      {
        // description
        char *description = strdup(pos->toString().c_str());
        char *adr = description;
        if(strncmp(description, "lang=", 5) == 0)
        {
          description = strchr(description, ' ');
          if(description != NULL) description++;
        }
        dt_metadata_set(img->id, "Xmp.dc.description", description);
        free(adr);
      }
      if((pos = xmpData.findKey(Exiv2::XmpKey("Xmp.dc.title"))) != xmpData.end())
      {
        // title
        char *title = strdup(pos->toString().c_str());
        char *adr = title;
        if(strncmp(title, "lang=", 5) == 0)
        {
          title = strchr(title, ' ');
          if(title != NULL) title++;
        }
        dt_metadata_set(img->id, "Xmp.dc.title", title);
        free(adr);
      }
      if((pos = xmpData.findKey(Exiv2::XmpKey("Xmp.dc.creator"))) != xmpData.end())
      {
        // creator
        char *creator = strdup(pos->toString().c_str());
        char *adr = creator;
        if(strncmp(creator, "lang=", 5) == 0)
        {
          creator = strchr(creator, ' ');
          if(creator != NULL) creator++;
        }
        dt_metadata_set(img->id, "Xmp.dc.creator", creator);
        free(adr);
      }
      if((pos = xmpData.findKey(Exiv2::XmpKey("Xmp.dc.publisher"))) != xmpData.end())
      {
        // publisher
        char *publisher = strdup(pos->toString().c_str());
        char *adr = publisher;
        if(strncmp(publisher, "lang=", 5) == 0)
        {
          publisher = strchr(publisher, ' ');
          if(publisher != NULL) publisher++;
        }
        dt_metadata_set(img->id, "Xmp.dc.publisher", publisher);
        free(adr);
      }
    }

    if((pos = xmpData.findKey(Exiv2::XmpKey("Xmp.xmp.Rating"))) != xmpData.end())
    {
      int stars = pos->toLong();
      if(use_defaul_rating && stars == 0) stars = dt_conf_get_int("ui_last/import_initial_rating");

      stars = (stars == -1) ? 6 : stars;
      img->flags = (img->flags & ~0x7) | (0x7 & stars);
    }

    if((pos = xmpData.findKey(Exiv2::XmpKey("Xmp.xmp.Label"))) != xmpData.end())
    {
      std::string label = pos->toString();
      if(label == "Red") // Is it really called like that in XMP files?
        dt_colorlabels_set_label(img->id, 0);
      else if(label == "Yellow") // Is it really called like that in XMP files?
        dt_colorlabels_set_label(img->id, 1);
      else if(label == "Green")
        dt_colorlabels_set_label(img->id, 2);
      else if(label == "Blue") // Is it really called like that in XMP files?
        dt_colorlabels_set_label(img->id, 3);
      else if(label == "Purple") // Is it really called like that in XMP files?
        dt_colorlabels_set_label(img->id, 4);
    }
    if((pos = xmpData.findKey(Exiv2::XmpKey("Xmp.darktable.colorlabels"))) != xmpData.end())
    {
      // TODO: store these in dc:subject or xmp:Label?
      // color labels
      const int cnt = pos->count();
      dt_colorlabels_remove_labels(img->id);
      for(int i = 0; i < cnt; i++)
      {
        dt_colorlabels_set_label(img->id, pos->toLong(i));
      }
    }

    if((pos = xmpData.findKey(Exiv2::XmpKey("Xmp.lr.hierarchicalSubject"))) != xmpData.end())
      _exif_import_tags(img, pos);
    else if((pos = xmpData.findKey(Exiv2::XmpKey("Xmp.dc.subject"))) != xmpData.end())
      _exif_import_tags(img, pos);

    /* read gps location */
    if((pos = xmpData.findKey(Exiv2::XmpKey("Xmp.exif.GPSLatitude"))) != xmpData.end())
    {
      img->latitude = _gps_string_to_number(pos->toString().c_str());
    }

    if((pos = xmpData.findKey(Exiv2::XmpKey("Xmp.exif.GPSLongitude"))) != xmpData.end())
    {
      img->longitude = _gps_string_to_number(pos->toString().c_str());
    }

    return true;
  }
  catch(Exiv2::AnyError &e)
  {
    std::string s(e.what());
    std::cerr << "[exiv2] " << s << std::endl;
    return false;
  }
}

// FIXME: according to http://www.exiv2.org/doc/classExiv2_1_1Metadatum.html#63c2b87249ba96679c29e01218169124
// there is no need to pass iptcData
static bool dt_exif_read_iptc_data(dt_image_t *img, Exiv2::IptcData &iptcData)
{
  try
  {
    Exiv2::IptcData::const_iterator pos;
    iptcData.sortByKey(); // this helps to quickly find all Iptc.Application2.Keywords

    if((pos = iptcData.findKey(Exiv2::IptcKey("Iptc.Application2.Keywords"))) != iptcData.end())
    {
      while(pos != iptcData.end())
      {
        std::string key = pos->key();
        if(g_strcmp0(key.c_str(), "Iptc.Application2.Keywords")) break;
        std::string str = pos->print();
        char *tag = dt_util_foo_to_utf8(str.c_str());
        guint tagid = 0;
        dt_tag_new(tag, &tagid);
        dt_tag_attach(tagid, img->id);
        g_free(tag);
        ++pos;
      }
    }
    if((pos = iptcData.findKey(Exiv2::IptcKey("Iptc.Application2.Caption"))) != iptcData.end())
    {
      std::string str = pos->print(/*&iptcData*/);
      dt_metadata_set(img->id, "Xmp.dc.description", str.c_str());
    }
    if((pos = iptcData.findKey(Exiv2::IptcKey("Iptc.Application2.Copyright"))) != iptcData.end())
    {
      std::string str = pos->print(/*&iptcData*/);
      dt_metadata_set(img->id, "Xmp.dc.rights", str.c_str());
    }
    if((pos = iptcData.findKey(Exiv2::IptcKey("Iptc.Application2.Writer"))) != iptcData.end())
    {
      std::string str = pos->print(/*&iptcData*/);
      dt_metadata_set(img->id, "Xmp.dc.creator", str.c_str());
    }
    else if((pos = iptcData.findKey(Exiv2::IptcKey("Iptc.Application2.Contact"))) != iptcData.end())
    {
      std::string str = pos->print(/*&iptcData*/);
      dt_metadata_set(img->id, "Xmp.dc.creator", str.c_str());
    }

    return true;
  }
  catch(Exiv2::AnyError &e)
  {
    std::string s(e.what());
    std::cerr << "[exiv2] " << s << std::endl;
    return false;
  }
}

static bool dt_exif_read_exif_data(dt_image_t *img, Exiv2::ExifData &exifData)
{
  try
  {
    /* List of tag names taken from exiv2's printSummary() in actions.cpp */
    Exiv2::ExifData::const_iterator pos;
    /* Read shutter time */
    if((pos = exifData.findKey(Exiv2::ExifKey("Exif.Photo.ExposureTime"))) != exifData.end() && pos->size())
    {
      // dt_strlcpy_to_utf8(uf->conf->shutterText, max_name, pos, exifData);
      img->exif_exposure = pos->toFloat();
    }
    else if((pos = exifData.findKey(Exiv2::ExifKey("Exif.Photo.ShutterSpeedValue"))) != exifData.end()
            && pos->size())
    {
      // uf_strlcpy_to_utf8(uf->conf->shutterText, max_name, pos, exifData);
      img->exif_exposure = 1.0 / pos->toFloat();
    }
    /* Read aperture */
    if((pos = exifData.findKey(Exiv2::ExifKey("Exif.Photo.FNumber"))) != exifData.end() && pos->size())
    {
      img->exif_aperture = pos->toFloat();
    }
    else if((pos = exifData.findKey(Exiv2::ExifKey("Exif.Photo.ApertureValue"))) != exifData.end()
            && pos->size())
    {
      img->exif_aperture = pos->toFloat();
    }
    /* Read ISO speed - Nikon happens to return a pair for Lo and Hi modes */
    if((pos = Exiv2::isoSpeed(exifData)) != exifData.end() && pos->size())
    {
      // if standard exif iso tag, use the old way of interpreting the return value to be more regression-save
      if(strcmp(pos->key().c_str(), "Exif.Photo.ISOSpeedRatings") == 0)
      {
        int isofield = pos->count() > 1 ? 1 : 0;
        img->exif_iso = pos->toFloat(isofield);
      }
      else
      {
        std::ostringstream os;
        pos->write(os, &exifData);
        std::string os_str = os.str();
        const char *exifstr = os_str.c_str();
        img->exif_iso = (float)std::atof(exifstr);
        // beware the following does not result in the same!:
        // img->exif_iso = (float) std::atof( pos->toString().c_str() );
      }
    }
#if EXIV2_MINOR_VERSION > 19
    /* Read focal length  */
    if((pos = Exiv2::focalLength(exifData)) != exifData.end() && pos->size())
    {
      img->exif_focal_length = pos->toFloat();
    }

    if((pos = exifData.findKey(Exiv2::ExifKey("Exif.NikonLd2.FocusDistance"))) != exifData.end()
       && pos->size())
    {
      float value = pos->toFloat();
      img->exif_focus_distance = (0.01 * pow(10, value / 40));
    }
    else if((pos = exifData.findKey(Exiv2::ExifKey("Exif.NikonLd3.FocusDistance"))) != exifData.end()
            && pos->size())
    {
      float value = pos->toFloat();
      img->exif_focus_distance = (0.01 * pow(10, value / 40));
    }
    else if((pos = exifData.findKey(Exiv2::ExifKey("Exif.OlympusFi.FocusDistance"))) != exifData.end()
            && pos->size())
    {
      /* the distance is stored as a rational (fraction). according to
       * http://www.dpreview.com/forums/thread/1173960?page=4
       * some Olympus cameras have a wrong denominator of 10 in there while the nominator is always in mm.
       * thus we ignore the denominator
       * and divide with 1000.
       * "I've checked a number of E-1 and E-300 images, and I agree that the FocusDistance looks like it is
       * in mm for the E-1. However,
       * it looks more like cm for the E-300.
       * For both cameras, this value is stored as a rational. With the E-1, the denominator is always 1,
       * while for the E-300 it is 10.
       * Therefore, it looks like the numerator in both cases is in mm (which makes a bit of sense, in an odd
       * sort of way). So I think
       * what I will do in ExifTool is to take the numerator and divide by 1000 to display the focus distance
       * in meters."
       *   -- Boardhead, dpreview forums in 2005
       */
      int nominator = pos->toRational(0).first;
      img->exif_focus_distance = fmax(0.0, (0.001 * nominator));
    }
#if EXIV2_MINOR_VERSION > 24
    else if((pos = exifData.findKey(Exiv2::ExifKey("Exif.CanonFi.FocusDistanceUpper"))) != exifData.end()
            && pos->size())
    {
      float FocusDistanceUpper = pos->toFloat();
      if(FocusDistanceUpper <= 0.0f || FocusDistanceUpper >= 0xffff)
      {
        img->exif_focus_distance = 0.0f;
      }
      else if((pos = exifData.findKey(Exiv2::ExifKey("Exif.CanonFi.FocusDistanceLower"))) != exifData.end()
              && pos->size())
      {
        float FocusDistanceLower = pos->toFloat();
        img->exif_focus_distance = (FocusDistanceLower + FocusDistanceUpper) / 200;
      }
    }
#endif
    else if((pos = Exiv2::subjectDistance(exifData)) != exifData.end() && pos->size())
    {
      img->exif_focus_distance = pos->toFloat();
    }
#endif
    /** read image orientation */
    if((pos = exifData.findKey(Exiv2::ExifKey("Exif.Image.Orientation"))) != exifData.end() && pos->size())
    {
      img->orientation = dt_image_orientation_to_flip_bits(pos->toLong());
    }
    else if((pos = exifData.findKey(Exiv2::ExifKey("Exif.PanasonicRaw.Orientation"))) != exifData.end()
            && pos->size())
    {
      img->orientation = dt_image_orientation_to_flip_bits(pos->toLong());
    }

    /* minolta and sony have their own rotation */
    if(((pos = exifData.findKey(Exiv2::ExifKey("Exif.MinoltaCs5D.Rotation"))) != exifData.end()
        && pos->size()) || ((pos = exifData.findKey(Exiv2::ExifKey("Exif.MinoltaCs7D.Rotation")))
                            != exifData.end() && pos->size()))
    {
      switch(pos->toLong())
      {
        case 76:                                           // 90 cw
          img->orientation = ORIENTATION_ROTATE_CW_90_DEG; // swap x/y, flip x
          break;
        case 82:                                            // 270 cw
          img->orientation = ORIENTATION_ROTATE_CCW_90_DEG; // swap x/y, flip y
          break;
        default:                               // 72, horizontal
          img->orientation = ORIENTATION_NONE; // do nothing
      }
    }

    /* read gps location */
    if((pos = exifData.findKey(Exiv2::ExifKey("Exif.GPSInfo.GPSLatitude"))) != exifData.end() && pos->size())
    {
      Exiv2::ExifData::const_iterator ref = exifData.findKey(Exiv2::ExifKey("Exif.GPSInfo.GPSLatitudeRef"));
      if(ref != exifData.end() && ref->size())
      {
        std::string sign_str = ref->toString();
        const char *sign = sign_str.c_str();
        double latitude = 0.0;
        if(_gps_rationale_to_number(pos->toRational(0).first, pos->toRational(0).second,
                                    pos->toRational(1).first, pos->toRational(1).second,
                                    pos->toRational(2).first, pos->toRational(2).second, sign[0], &latitude))
          img->latitude = latitude;
      }
    }

    if((pos = exifData.findKey(Exiv2::ExifKey("Exif.GPSInfo.GPSLongitude"))) != exifData.end() && pos->size())
    {
      Exiv2::ExifData::const_iterator ref = exifData.findKey(Exiv2::ExifKey("Exif.GPSInfo.GPSLongitudeRef"));
      if(ref != exifData.end() && ref->size())
      {
        std::string sign_str = ref->toString();
        const char *sign = sign_str.c_str();
        double longitude = 0.0;
        if(_gps_rationale_to_number(pos->toRational(0).first, pos->toRational(0).second,
                                    pos->toRational(1).first, pos->toRational(1).second,
                                    pos->toRational(2).first, pos->toRational(2).second, sign[0], &longitude))
          img->longitude = longitude;
      }
    }

    /* Read lens name */
    if((((pos = exifData.findKey(Exiv2::ExifKey("Exif.CanonCs.LensType"))) != exifData.end())
        || ((pos = exifData.findKey(Exiv2::ExifKey("Exif.Canon.0x0095"))) != exifData.end())) && pos->size())
    {
      dt_strlcpy_to_utf8(img->exif_lens, sizeof(img->exif_lens), pos, exifData);
    }
    else if((pos = exifData.findKey(Exiv2::ExifKey("Exif.Panasonic.LensType"))) != exifData.end()
            && pos->size())
    {
      dt_strlcpy_to_utf8(img->exif_lens, sizeof(img->exif_lens), pos, exifData);
    }
    else if((pos = exifData.findKey(Exiv2::ExifKey("Exif.OlympusEq.LensType"))) != exifData.end()
            && pos->size())
    {
      dt_strlcpy_to_utf8(img->exif_lens, sizeof(img->exif_lens), pos, exifData);
    }
#if EXIV2_MINOR_VERSION > 20
    else if((pos = exifData.findKey(Exiv2::ExifKey("Exif.OlympusEq.LensModel"))) != exifData.end()
            && pos->size())
    {
      dt_strlcpy_to_utf8(img->exif_lens, sizeof(img->exif_lens), pos, exifData);
    }
#endif
    else if((pos = Exiv2::lensName(exifData)) != exifData.end() && pos->size())
    {
      dt_strlcpy_to_utf8(img->exif_lens, sizeof(img->exif_lens), pos, exifData);
    }
    else if((pos = exifData.findKey(Exiv2::ExifKey("Exif.Photo.LensModel"))) != exifData.end() && pos->size())
    {
      dt_strlcpy_to_utf8(img->exif_lens, sizeof(img->exif_lens), pos, exifData);
    }

#if 0
    /* Read flash mode */
    if ( (pos=exifData.findKey(Exiv2::ExifKey("Exif.Photo.Flash")))
         != exifData.end() && pos->size())
    {
      uf_strlcpy_to_utf8(uf->conf->flashText, max_name, pos, exifData);
    }
    /* Read White Balance Setting */
    if ( (pos=exifData.findKey(Exiv2::ExifKey("Exif.Photo.WhiteBalance")))
         != exifData.end() && pos->size())
    {
      uf_strlcpy_to_utf8(uf->conf->whiteBalanceText, max_name, pos, exifData);
    }
#endif

    if((pos = exifData.findKey(Exiv2::ExifKey("Exif.Image.Make"))) != exifData.end() && pos->size())
    {
      dt_strlcpy_to_utf8(img->exif_maker, sizeof(img->exif_maker), pos, exifData);
    }
    else if((pos = exifData.findKey(Exiv2::ExifKey("Exif.PanasonicRaw.Make"))) != exifData.end()
            && pos->size())
    {
      dt_strlcpy_to_utf8(img->exif_maker, sizeof(img->exif_maker), pos, exifData);
    }

    for(char *c = img->exif_maker + sizeof(img->exif_maker) - 1; c > img->exif_maker; c--)
      if(*c != ' ' && *c != '\0')
      {
        *(c + 1) = '\0';
        break;
      }

    if((pos = exifData.findKey(Exiv2::ExifKey("Exif.Image.Model"))) != exifData.end() && pos->size())
    {
      dt_strlcpy_to_utf8(img->exif_model, sizeof(img->exif_model), pos, exifData);
    }
    else if((pos = exifData.findKey(Exiv2::ExifKey("Exif.PanasonicRaw.Model"))) != exifData.end()
            && pos->size())
    {
      dt_strlcpy_to_utf8(img->exif_model, sizeof(img->exif_model), pos, exifData);
    }

    for(char *c = img->exif_model + sizeof(img->exif_model) - 1; c > img->exif_model; c--)
      if(*c != ' ' && *c != '\0')
      {
        *(c + 1) = '\0';
        break;
      }

    if((pos = exifData.findKey(Exiv2::ExifKey("Exif.Image.DateTimeOriginal"))) != exifData.end()
       && pos->size())
    {
      dt_strlcpy_to_utf8(img->exif_datetime_taken, 20, pos, exifData);
    }
    else if((pos = exifData.findKey(Exiv2::ExifKey("Exif.Photo.DateTimeOriginal"))) != exifData.end()
            && pos->size())
    {
      dt_strlcpy_to_utf8(img->exif_datetime_taken, 20, pos, exifData);
    }

    if((pos = exifData.findKey(Exiv2::ExifKey("Exif.Image.Artist"))) != exifData.end() && pos->size())
    {
      std::string str = pos->print(&exifData);
      dt_metadata_set(img->id, "Xmp.dc.creator", str.c_str());
    }
    else if((pos = exifData.findKey(Exiv2::ExifKey("Exif.Canon.OwnerName"))) != exifData.end() && pos->size())
    {
      std::string str = pos->print(&exifData);
      dt_metadata_set(img->id, "Xmp.dc.creator", str.c_str());
    }

    // FIXME: Should the UserComment go into the description? Or do we need an extra field for this?
    if((pos = exifData.findKey(Exiv2::ExifKey("Exif.Photo.UserComment"))) != exifData.end() && pos->size())
    {
      std::string str = pos->print(&exifData);
      dt_metadata_set(img->id, "Xmp.dc.description", str.c_str());
    }

    if((pos = exifData.findKey(Exiv2::ExifKey("Exif.Image.Copyright"))) != exifData.end() && pos->size())
    {
      std::string str = pos->print(&exifData);
      dt_metadata_set(img->id, "Xmp.dc.rights", str.c_str());
    }

    if((pos = exifData.findKey(Exiv2::ExifKey("Exif.Image.Rating"))) != exifData.end() && pos->size())
    {
      int stars = pos->toLong();
      if(stars == 0)
      {
        stars = dt_conf_get_int("ui_last/import_initial_rating");
      }
      else
      {
        stars = (stars == -1) ? 6 : stars;
      }
      img->flags = (img->flags & ~0x7) | (0x7 & stars);
    }
    else if((pos = exifData.findKey(Exiv2::ExifKey("Exif.Image.RatingPercent"))) != exifData.end()
            && pos->size())
    {
      int stars = pos->toLong() * 5. / 100;
      if(stars == 0)
      {
        stars = dt_conf_get_int("ui_last/import_initial_rating");
      }
      else
      {
        stars = (stars == -1) ? 6 : stars;
      }
      img->flags = (img->flags & ~0x7) | (0x7 & stars);
    }

    // read embedded color matrix as used in DNGs
    {
      int is_1_65 = -1, is_2_65 = -1; // -1: not found, 0: some random type, 1: D65
      if((pos = exifData.findKey(Exiv2::ExifKey("Exif.Image.CalibrationIlluminant1"))) != exifData.end()
         && pos->size())
      {
        is_1_65 = (pos->toLong() == 21) ? 1 : 0;
      }
      if((pos = exifData.findKey(Exiv2::ExifKey("Exif.Image.CalibrationIlluminant2"))) != exifData.end()
         && pos->size())
      {
        is_2_65 = (pos->toLong() == 21) ? 1 : 0;
      }

      // use the d65 (type == 21) matrix if we found it, otherwise use whatever we got
      Exiv2::ExifData::const_iterator cm1_pos = exifData.findKey(Exiv2::ExifKey("Exif.Image.ColorMatrix1"));
      Exiv2::ExifData::const_iterator cm2_pos = exifData.findKey(Exiv2::ExifKey("Exif.Image.ColorMatrix2"));
      if(is_1_65 == 1 && cm1_pos != exifData.end() && cm1_pos->count() == 9 && cm1_pos->size())
        for(int i = 0; i < 9; i++) img->d65_color_matrix[i] = cm1_pos->toFloat(i);
      else if(is_2_65 == 1 && cm2_pos != exifData.end() && cm2_pos->count() == 9 && cm2_pos->size())
        for(int i = 0; i < 9; i++) img->d65_color_matrix[i] = cm2_pos->toFloat(i);
      else if(is_1_65 == 0 && cm1_pos != exifData.end() && cm1_pos->count() == 9 && cm1_pos->size())
        for(int i = 0; i < 9; i++) img->d65_color_matrix[i] = cm1_pos->toFloat(i);
      else if(is_2_65 == 0 && cm2_pos != exifData.end() && cm2_pos->count() == 9 && cm2_pos->size())
        for(int i = 0; i < 9; i++) img->d65_color_matrix[i] = cm2_pos->toFloat(i);
    }

    // some files have the colorspace explicitly set. try to read that.
    // is_ldr -> none
    // 0x01   -> sRGB
    // 0x02   -> AdobeRGB
    // 0xffff -> Uncalibrated
    //          + Exif.Iop.InteroperabilityIndex of 'R03' -> AdobeRGB
    //          + Exif.Iop.InteroperabilityIndex of 'R98' -> sRGB
    if(dt_image_is_ldr(img)
       && (pos = exifData.findKey(Exiv2::ExifKey("Exif.Photo.ColorSpace"))) != exifData.end() && pos->size())
    {
      int colorspace = pos->toLong();
      if(colorspace == 0x01)
        img->colorspace = DT_IMAGE_COLORSPACE_SRGB;
      else if(colorspace == 0x02)
        img->colorspace = DT_IMAGE_COLORSPACE_ADOBE_RGB;
      else if(colorspace == 0xffff)
      {
        if((pos = exifData.findKey(Exiv2::ExifKey("Exif.Iop.InteroperabilityIndex"))) != exifData.end()
           && pos->size())
        {
          std::string interop_index = pos->toString();
          if(interop_index == "R03")
            img->colorspace = DT_IMAGE_COLORSPACE_ADOBE_RGB;
          else if(interop_index == "R98")
            img->colorspace = DT_IMAGE_COLORSPACE_SRGB;
        }
      }
    }

#if EXIV2_MINOR_VERSION < 23
    // workaround for an exiv2 bug writing random garbage into exif_lens for this camera:
    // http://dev.exiv2.org/issues/779
    if(!strcmp(img->exif_model, "DMC-GH2")) snprintf(img->exif_lens, sizeof(img->exif_lens), "(unknown)");
#endif

    // Workaround for an issue on newer Sony NEX cams.
    // The default EXIF field is not used by Sony to store lens data
    // http://dev.exiv2.org/issues/883
    // http://darktable.org/redmine/issues/8813
    // FIXME: This is still a workaround
    if((!strncmp(img->exif_model, "NEX", 3)) || (!strncmp(img->exif_model, "ILCE", 4)))
    {
      snprintf(img->exif_lens, sizeof(img->exif_lens), "(unknown)");
      if((pos = exifData.findKey(Exiv2::ExifKey("Exif.Photo.LensModel"))) != exifData.end() && pos->size())
      {
        std::string str = pos->print(&exifData);
        snprintf(img->exif_lens, sizeof(img->exif_lens), "%s", str.c_str());
      }
    };

    img->exif_inited = 1;
    return true;
  }
  catch(Exiv2::AnyError &e)
  {
    std::string s(e.what());
    std::cerr << "[exiv2] " << s << std::endl;
    return false;
  }
}

static void dt_exif_apply_global_overwrites(dt_image_t *img)
{
  if(dt_conf_get_bool("ui_last/import_apply_metadata") == TRUE)
  {
    char *str;

    str = dt_conf_get_string("ui_last/import_last_creator");
    if(str != NULL && str[0] != '\0') dt_metadata_set(img->id, "Xmp.dc.creator", str);
    g_free(str);

    str = dt_conf_get_string("ui_last/import_last_rights");
    if(str != NULL && str[0] != '\0') dt_metadata_set(img->id, "Xmp.dc.rights", str);
    g_free(str);

    str = dt_conf_get_string("ui_last/import_last_publisher");
    if(str != NULL && str[0] != '\0') dt_metadata_set(img->id, "Xmp.dc.publisher", str);
    g_free(str);

    str = dt_conf_get_string("ui_last/import_last_tags");
    if(str != NULL && str[0] != '\0') dt_tag_attach_string_list(str, img->id);
    g_free(str);
  }
}

// TODO: can this blob also contain xmp and iptc data?
int dt_exif_read_from_blob(dt_image_t *img, uint8_t *blob, const int size)
{
  try
  {
    Exiv2::ExifData exifData;
    Exiv2::ExifParser::decode(exifData, blob, size);
    bool res = dt_exif_read_exif_data(img, exifData);
    dt_exif_apply_global_overwrites(img);
    return res ? 0 : 1;
  }
  catch(Exiv2::AnyError &e)
  {
    std::string s(e.what());
    std::cerr << "[exiv2] " << s << std::endl;
    return 1;
  }
}

/**
 * Get the largest possible thumbnail from the image
 */
int dt_exif_get_thumbnail(const char *path, uint8_t **buffer, size_t *size, char **mime_type)
{
  try
  {
    Exiv2::Image::AutoPtr image = Exiv2::ImageFactory::open(path);
    assert(image.get() != 0);
    image->readMetadata();

    // Get a list of preview images available in the image. The list is sorted
    // by the preview image pixel size, starting with the smallest preview.
    Exiv2::PreviewManager loader(*image);
    Exiv2::PreviewPropertiesList list = loader.getPreviewProperties();
    if(list.empty())
    {
      std::cerr << "[exiv2] couldn't find thumbnail for " << path << std::endl;
      return 1;
    }

    // Select the largest one
    // FIXME: We could probably select a smaller thumbnail to match the mip size
    //        we actually want to create. Is it really much faster though?
    Exiv2::PreviewProperties selected = list.back();

    // Get the selected preview image
    Exiv2::PreviewImage preview = loader.getPreviewImage(selected);
    const unsigned  char *tmp = preview.pData();
    size_t _size = preview.size();

    *size = _size;
    *mime_type = strdup(preview.mimeType().c_str());
    *buffer = (uint8_t *)malloc(_size);
    if(!*buffer) {
      std::cerr << "[exiv2] couldn't allocate memory for thumbnail for " << path << std::endl;
      return 1;
    }
    //std::cerr << "[exiv2] "<< path << ": found thumbnail "<< preview.width() << "x" << preview.height() << std::endl;
    memcpy(*buffer, tmp, _size);

    return 0;
  }
  catch(Exiv2::AnyError &e)
  {
    std::string s(e.what());
    std::cerr << "[exiv2] " << path << ": " << s << std::endl;
    return 1;
  }
}

/** read the metadata of an image.
 * XMP data trumps IPTC data trumps EXIF data
 */
int dt_exif_read(dt_image_t *img, const char *path)
{
  // at least set datetime taken to something useful in case there is no exif data in this file (pfm, png,
  // ...)
  struct stat statbuf;

  if(!stat(path, &statbuf))
  {
    struct tm result;
    strftime(img->exif_datetime_taken, 20, "%Y:%m:%d %H:%M:%S", localtime_r(&statbuf.st_mtime, &result));
  }

  try
  {
    Exiv2::Image::AutoPtr image;
    image = Exiv2::ImageFactory::open(path);
    assert(image.get() != 0);
    image->readMetadata();
    bool res = true;

    // EXIF metadata
    Exiv2::ExifData &exifData = image->exifData();
    if(!exifData.empty())
      res = dt_exif_read_exif_data(img, exifData);
    else
      img->exif_inited = 1;

    // these get overwritten by IPTC and XMP. is that how it should work?
    dt_exif_apply_global_overwrites(img);

    // IPTC metadata.
    Exiv2::IptcData &iptcData = image->iptcData();
    if(!iptcData.empty()) res = dt_exif_read_iptc_data(img, iptcData) && res;

    // XMP metadata
    Exiv2::XmpData &xmpData = image->xmpData();
    if(!xmpData.empty()) res = dt_exif_read_xmp_data(img, xmpData, false, true) && res;

    // Initialize size - don't wait for full raw to be loaded to get this
    // information. If use_embedded_thumbnail is set, it will take a
    // change in development history to have this information
    img->height = image->pixelHeight();
    img->width = image->pixelWidth();

    return res ? 0 : 1;
  }
  catch(Exiv2::AnyError &e)
  {
    std::string s(e.what());
    std::cerr << "[exiv2] " << path << ": " << s << std::endl;
    return 1;
  }
}

int dt_exif_write_blob(uint8_t *blob, uint32_t size, const char *path)
{
  try
  {
    Exiv2::Image::AutoPtr image = Exiv2::ImageFactory::open(path);
    assert(image.get() != 0);
    image->readMetadata();
    Exiv2::ExifData &imgExifData = image->exifData();
    Exiv2::ExifData blobExifData;
    Exiv2::ExifParser::decode(blobExifData, blob + 6, size);
    Exiv2::ExifData::const_iterator end = blobExifData.end();
    for(Exiv2::ExifData::const_iterator i = blobExifData.begin(); i != end; ++i)
    {
      Exiv2::ExifKey key(i->key());
      if(imgExifData.findKey(key) == imgExifData.end())
        imgExifData.add(Exiv2::ExifKey(i->key()), &i->value());
    }
    // Remove thumbnail
    Exiv2::ExifData::iterator it;
    if((it = imgExifData.findKey(Exiv2::ExifKey("Exif.Thumbnail.Compression"))) != imgExifData.end())
      imgExifData.erase(it);
    if((it = imgExifData.findKey(Exiv2::ExifKey("Exif.Thumbnail.XResolution"))) != imgExifData.end())
      imgExifData.erase(it);
    if((it = imgExifData.findKey(Exiv2::ExifKey("Exif.Thumbnail.YResolution"))) != imgExifData.end())
      imgExifData.erase(it);
    if((it = imgExifData.findKey(Exiv2::ExifKey("Exif.Thumbnail.ResolutionUnit"))) != imgExifData.end())
      imgExifData.erase(it);
    if((it = imgExifData.findKey(Exiv2::ExifKey("Exif.Thumbnail.JPEGInterchangeFormat"))) != imgExifData.end())
      imgExifData.erase(it);
    if((it = imgExifData.findKey(Exiv2::ExifKey("Exif.Thumbnail.JPEGInterchangeFormatLength")))
       != imgExifData.end())
      imgExifData.erase(it);

    imgExifData.sortByTag();
    image->writeMetadata();
  }
  catch(Exiv2::AnyError &e)
  {
    std::string s(e.what());
    std::cerr << "[exiv2] " << s << std::endl;
    return 0;
  }
  return 1;
}

int dt_exif_read_blob(uint8_t *buf, const char *path, const int imgid, const int sRGB, const int out_width,
                      const int out_height, const int dng_mode)
{
  try
  {
    Exiv2::Image::AutoPtr image;
    image = Exiv2::ImageFactory::open(path);
    assert(image.get() != 0);
    image->readMetadata();
    Exiv2::ExifData &exifData = image->exifData();
    // needs to be reset, even in dng mode, as the buffers are flipped during raw import
    exifData["Exif.Image.Orientation"] = uint16_t(1);

    // get rid of thumbnails
    Exiv2::ExifThumb(exifData).erase();

    // ufraw-style exif stripping:
    Exiv2::ExifData::iterator pos;
    /* Delete original TIFF data, which is irrelevant*/
    if((pos = exifData.findKey(Exiv2::ExifKey("Exif.Image.ImageWidth"))) != exifData.end())
      exifData.erase(pos);
    if((pos = exifData.findKey(Exiv2::ExifKey("Exif.Image.ImageLength"))) != exifData.end())
      exifData.erase(pos);
    if((pos = exifData.findKey(Exiv2::ExifKey("Exif.Image.BitsPerSample"))) != exifData.end())
      exifData.erase(pos);
    if((pos = exifData.findKey(Exiv2::ExifKey("Exif.Image.Compression"))) != exifData.end())
      exifData.erase(pos);
    if((pos = exifData.findKey(Exiv2::ExifKey("Exif.Image.PhotometricInterpretation"))) != exifData.end())
      exifData.erase(pos);
    if((pos = exifData.findKey(Exiv2::ExifKey("Exif.Image.FillOrder"))) != exifData.end())
      exifData.erase(pos);
    if((pos = exifData.findKey(Exiv2::ExifKey("Exif.Image.SamplesPerPixel"))) != exifData.end())
      exifData.erase(pos);
    if((pos = exifData.findKey(Exiv2::ExifKey("Exif.Image.StripOffsets"))) != exifData.end())
      exifData.erase(pos);
    if((pos = exifData.findKey(Exiv2::ExifKey("Exif.Image.RowsPerStrip"))) != exifData.end())
      exifData.erase(pos);
    if((pos = exifData.findKey(Exiv2::ExifKey("Exif.Image.StripByteCounts"))) != exifData.end())
      exifData.erase(pos);
    if((pos = exifData.findKey(Exiv2::ExifKey("Exif.Image.PlanarConfiguration"))) != exifData.end())
      exifData.erase(pos);

    if(!dng_mode)
    {
      /* Delete various MakerNote fields only applicable to the raw file */

      // Nikon thumbnail data
      if((pos = exifData.findKey(Exiv2::ExifKey("Exif.Nikon3.Preview"))) != exifData.end())
        exifData.erase(pos);
      if((pos = exifData.findKey(Exiv2::ExifKey("Exif.NikonPreview.JPEGInterchangeFormat"))) != exifData.end())
        exifData.erase(pos);

      // DNG private data
      if((pos = exifData.findKey(Exiv2::ExifKey("Exif.Image.DNGPrivateData"))) != exifData.end())
        exifData.erase(pos);

      // Pentax thumbnail data
      if((pos = exifData.findKey(Exiv2::ExifKey("Exif.Pentax.PreviewResolution"))) != exifData.end())
        exifData.erase(pos);
      if((pos = exifData.findKey(Exiv2::ExifKey("Exif.Pentax.PreviewLength"))) != exifData.end())
        exifData.erase(pos);
      if((pos = exifData.findKey(Exiv2::ExifKey("Exif.Pentax.PreviewOffset"))) != exifData.end())
        exifData.erase(pos);

      // Minolta thumbnail data
      if((pos = exifData.findKey(Exiv2::ExifKey("Exif.Minolta.Thumbnail"))) != exifData.end())
        exifData.erase(pos);
      if((pos = exifData.findKey(Exiv2::ExifKey("Exif.Minolta.ThumbnailOffset"))) != exifData.end())
        exifData.erase(pos);
      if((pos = exifData.findKey(Exiv2::ExifKey("Exif.Minolta.ThumbnailLength"))) != exifData.end())
        exifData.erase(pos);

      // Sony thumbnail data
      if((pos = exifData.findKey(Exiv2::ExifKey("Exif.SonyMinolta.ThumbnailOffset"))) != exifData.end())
        exifData.erase(pos);
      if((pos = exifData.findKey(Exiv2::ExifKey("Exif.SonyMinolta.ThumbnailLength"))) != exifData.end())
        exifData.erase(pos);

      // Olympus thumbnail data
      if((pos = exifData.findKey(Exiv2::ExifKey("Exif.Olympus.Thumbnail"))) != exifData.end())
        exifData.erase(pos);
      if((pos = exifData.findKey(Exiv2::ExifKey("Exif.Olympus.ThumbnailOffset"))) != exifData.end())
        exifData.erase(pos);
      if((pos = exifData.findKey(Exiv2::ExifKey("Exif.Olympus.ThumbnailLength"))) != exifData.end())
        exifData.erase(pos);

#if EXIV2_MINOR_VERSION >= 23
      // Exiv2 versions older than 0.23 drop all EXIF if the code below is executed
      // Samsung makernote cleanup, the entries below have no relevance for exported images
      if((pos = exifData.findKey(Exiv2::ExifKey("Exif.Samsung2.SensorAreas"))) != exifData.end())
        exifData.erase(pos);
      if((pos = exifData.findKey(Exiv2::ExifKey("Exif.Samsung2.ColorSpace"))) != exifData.end())
        exifData.erase(pos);
      if((pos = exifData.findKey(Exiv2::ExifKey("Exif.Samsung2.EncryptionKey"))) != exifData.end())
        exifData.erase(pos);
      if((pos = exifData.findKey(Exiv2::ExifKey("Exif.Samsung2.WB_RGGBLevelsUncorrected"))) != exifData.end())
        exifData.erase(pos);
      if((pos = exifData.findKey(Exiv2::ExifKey("Exif.Samsung2.WB_RGGBLevelsAuto"))) != exifData.end())
        exifData.erase(pos);
      if((pos = exifData.findKey(Exiv2::ExifKey("Exif.Samsung2.WB_RGGBLevelsIlluminator1"))) != exifData.end())
        exifData.erase(pos);
      if((pos = exifData.findKey(Exiv2::ExifKey("Exif.Samsung2.WB_RGGBLevelsIlluminator2"))) != exifData.end())
        exifData.erase(pos);
      if((pos = exifData.findKey(Exiv2::ExifKey("Exif.Samsung2.WB_RGGBLevelsBlack"))) != exifData.end())
        exifData.erase(pos);
      if((pos = exifData.findKey(Exiv2::ExifKey("Exif.Samsung2.ColorMatrix"))) != exifData.end())
        exifData.erase(pos);
      if((pos = exifData.findKey(Exiv2::ExifKey("Exif.Samsung2.ColorMatrixSRGB"))) != exifData.end())
        exifData.erase(pos);
      if((pos = exifData.findKey(Exiv2::ExifKey("Exif.Samsung2.ColorMatrixAdobeRGB"))) != exifData.end())
        exifData.erase(pos);
      if((pos = exifData.findKey(Exiv2::ExifKey("Exif.Samsung2.ToneCurve1"))) != exifData.end())
        exifData.erase(pos);
      if((pos = exifData.findKey(Exiv2::ExifKey("Exif.Samsung2.ToneCurve2"))) != exifData.end())
        exifData.erase(pos);
      if((pos = exifData.findKey(Exiv2::ExifKey("Exif.Samsung2.ToneCurve3"))) != exifData.end())
        exifData.erase(pos);
      if((pos = exifData.findKey(Exiv2::ExifKey("Exif.Samsung2.ToneCurve4"))) != exifData.end())
        exifData.erase(pos);
#endif

      /* Write appropriate color space tag if using sRGB output */
      if(sRGB)
        exifData["Exif.Photo.ColorSpace"] = uint16_t(1); /* sRGB */
      else
        exifData["Exif.Photo.ColorSpace"] = uint16_t(0xFFFF); /* Uncalibrated */
    }

    /* Replace RAW dimension with output dimensions (for example after crop/scale, or orientation for dng
     * mode) */
    if(out_width > 0) exifData["Exif.Photo.PixelXDimension"] = out_width;
    if(out_height > 0) exifData["Exif.Photo.PixelYDimension"] = out_height;

    int resolution = dt_conf_get_int("metadata/resolution");
    if(resolution > 0)
    {
      exifData["Exif.Image.XResolution"] = resolution;
      exifData["Exif.Image.YResolution"] = resolution;
      exifData["Exif.Image.ResolutionUnit"] = uint16_t(2); /* inches */
    }
    else
    {
      if((pos = exifData.findKey(Exiv2::ExifKey("Exif.Image.XResolution"))) != exifData.end())
        exifData.erase(pos);
      if((pos = exifData.findKey(Exiv2::ExifKey("Exif.Image.YResolution"))) != exifData.end())
        exifData.erase(pos);
      if((pos = exifData.findKey(Exiv2::ExifKey("Exif.Image.ResolutionUnit"))) != exifData.end())
        exifData.erase(pos);
    }

    exifData["Exif.Image.Software"] = PACKAGE_STRING;

    // TODO: find a nice place for the missing metadata (tags, publisher, colorlabels?). Additionally find out
    // how to embed XMP data.
    //       And shall we add a description of the history stack to Exif.Image.ImageHistory?
    if(imgid >= 0)
    {
      GList *res = dt_metadata_get(imgid, "Xmp.dc.creator", NULL);
      if(res != NULL)
      {
        exifData["Exif.Image.Artist"] = (char *)res->data;
        g_list_free_full(res, &g_free);
      }

      res = dt_metadata_get(imgid, "Xmp.dc.title", NULL);
      if(res != NULL)
      {
        exifData["Exif.Image.ImageDescription"] = (char *)res->data;
        g_list_free_full(res, &g_free);
      }

      res = dt_metadata_get(imgid, "Xmp.dc.description", NULL);
      if(res != NULL)
      {
        exifData["Exif.Photo.UserComment"] = (char *)res->data;
        g_list_free_full(res, &g_free);
      }

      res = dt_metadata_get(imgid, "Xmp.dc.rights", NULL);
      if(res != NULL)
      {
        exifData["Exif.Image.Copyright"] = (char *)res->data;
        g_list_free_full(res, &g_free);
      }

      res = dt_metadata_get(imgid, "Xmp.xmp.Rating", NULL);
      if(res != NULL)
      {
        int rating = GPOINTER_TO_INT(res->data) + 1;
        exifData["Exif.Image.Rating"] = rating;
        exifData["Exif.Image.RatingPercent"] = int(rating / 5. * 100.);
        g_list_free(res);
      }

      // GPS data
      const dt_image_t *cimg = dt_image_cache_get(darktable.image_cache, imgid, 'r');
      if(!std::isnan(cimg->longitude) && !std::isnan(cimg->latitude))
      {
        exifData["Exif.GPSInfo.GPSVersionID"] = "02 02 00 00";
        exifData["Exif.GPSInfo.GPSLongitudeRef"] = (cimg->longitude < 0) ? "W" : "E";
        exifData["Exif.GPSInfo.GPSLatitudeRef"] = (cimg->latitude < 0) ? "S" : "N";

        long long_deg = (int)floor(fabs(cimg->longitude));
        long lat_deg = (int)floor(fabs(cimg->latitude));
        long long_min = (int)floor((fabs(cimg->longitude) - floor(fabs(cimg->longitude))) * 60000000);
        long lat_min = (int)floor((fabs(cimg->latitude) - floor(fabs(cimg->latitude))) * 60000000);
        gchar *long_str = g_strdup_printf("%ld/1 %ld/1000000 0/1", long_deg, long_min);
        gchar *lat_str = g_strdup_printf("%ld/1 %ld/1000000 0/1", lat_deg, lat_min);
        exifData["Exif.GPSInfo.GPSLongitude"] = long_str;
        exifData["Exif.GPSInfo.GPSLatitude"] = lat_str;
        g_free(long_str);
        g_free(lat_str);
      }

      // According to the Exif specs DateTime is to be set to the last modification time while
      // DateTimeOriginal is to be kept.
      // For us "keeping" it means to write out what we have in DB to support people adding a time offset in
      // the geotagging module.
      gchar new_datetime[20];
      dt_gettime(new_datetime, sizeof(new_datetime));
      exifData["Exif.Image.DateTime"] = new_datetime;
      exifData["Exif.Image.DateTimeOriginal"] = cimg->exif_datetime_taken;
      exifData["Exif.Photo.DateTimeOriginal"] = cimg->exif_datetime_taken;
      // FIXME: What about DateTimeDigitized? we currently update it, too, which might not be what is expected
      // for scanned images
      exifData["Exif.Photo.DateTimeDigitized"] = cimg->exif_datetime_taken;

      dt_image_cache_read_release(darktable.image_cache, cimg);
    }

    Exiv2::Blob blob;
    Exiv2::ExifParser::encode(blob, Exiv2::bigEndian, exifData);
    const int length = blob.size();
    memcpy(buf, "Exif\000\000", 6);
    if(length > 0 && length < 65534)
    {
      memcpy(buf + 6, &(blob[0]), length);
      return length + 6;
    }
    return 6;
  }
  catch(Exiv2::AnyError &e)
  {
    // std::cerr.rdbuf(savecerr);
    std::string s(e.what());
    std::cerr << "[exiv2] " << s << std::endl;
    return 0;
  }
}

// encode binary blob into text:
char *dt_exif_xmp_encode(const unsigned char *input, const int len, int *output_len)
{
#define COMPRESS_THRESHOLD 100

  char *output = NULL;
  gboolean do_compress = FALSE;

  // if input data field exceeds a certain size we compress it and convert to base64;
  // main reason for compression: make more xmp data fit into 64k segment within
  // JPEG output files.
  char *config = dt_conf_get_string("compress_xmp_tags");
  if(config)
  {
    if(!strcmp(config, "always"))
      do_compress = TRUE;
    else if((len > COMPRESS_THRESHOLD) && !strcmp(config, "only large entries"))
      do_compress = TRUE;
    else
      do_compress = FALSE;
    g_free(config);
  }

  if(do_compress)
  {
    int result;
    uLongf destLen = compressBound(len);
    unsigned char *buffer1 = (unsigned char *)malloc(destLen);

    result = compress(buffer1, &destLen, input, len);

    if(result != Z_OK)
    {
      free(buffer1);
      return NULL;
    }

    // we store the compression factor
    const int factor = MIN(len / destLen + 1, 99);

    char *buffer2 = (char *)g_base64_encode(buffer1, destLen);
    free(buffer1);
    if(!buffer2) return NULL;

    int outlen = strlen(buffer2) + 5; // leading "gz" + compression factor + base64 string + trailing '\0'
    output = (char *)malloc(outlen);
    if(!output)
    {
      g_free(buffer2);
      return NULL;
    }

    output[0] = 'g';
    output[1] = 'z';
    output[2] = factor / 10 + '0';
    output[3] = factor % 10 + '0';
    g_strlcpy(output + 4, buffer2, outlen);
    g_free(buffer2);

    if(output_len) *output_len = outlen;
  }
  else
  {
    const char hex[16] = { '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f' };

    output = (char *)malloc(2 * len + 1);
    if(!output) return NULL;

    if(output_len) *output_len = 2 * len + 1;

    for(int i = 0; i < len; i++)
    {
      const int hi = input[i] >> 4;
      const int lo = input[i] & 15;
      output[2 * i] = hex[hi];
      output[2 * i + 1] = hex[lo];
    }
    output[2 * len] = '\0';
  }

  return output;

#undef COMPRESS_THRESHOLD
}

// and back to binary
unsigned char *dt_exif_xmp_decode(const char *input, const int len, int *output_len)
{
  unsigned char *output = NULL;

  // check if data is in compressed format
  if(!strncmp(input, "gz", 2))
  {
    // we have compressed data in base64 representation with leading "gz"

    // get stored compression factor so we know the needed buffer size for uncompress
    const float factor = 10 * (input[2] - '0') + (input[3] - '0');

    // get a rw copy of input buffer omitting leading "gz" and compression factor
    unsigned char *buffer = (unsigned char *)strdup(input + 4);
    if(!buffer) return NULL;

    // decode from base64 to compressed binary
    gsize compressed_size;
    g_base64_decode_inplace((char *)buffer, &compressed_size);

    // do the actual uncompress step
    int result = Z_BUF_ERROR;
    uLongf bufLen = factor * compressed_size;
    uLongf destLen;

    // we know the actual compression factor but if that fails we re-try with
    // increasing buffer sizes, eg. we don't know (unlikely) factors > 99
    do
    {
      if(output) free(output);
      output = (unsigned char *)malloc(bufLen);
      if(!output) break;

      destLen = bufLen;

      result = uncompress(output, &destLen, buffer, compressed_size);

      bufLen *= 2;

    } while(result == Z_BUF_ERROR);

    free(buffer);

    if(result != Z_OK)
    {
      if(output) free(output);
      return NULL;
    }

    if(output_len) *output_len = destLen;
  }
  else
  {
// we have uncompressed data in hexadecimal ascii representation

// ascii table:
// 48- 57 0-9
// 97-102 a-f
#define TO_BINARY(a) (a > 57 ? a - 97 + 10 : a - 48)

    // make sure that we don't find any unexpected characters indicating corrupted data
    if(strspn(input, "0123456789abcdef") != strlen(input)) return NULL;

    output = (unsigned char *)malloc(len / 2);
    if(!output) return NULL;

    if(output_len) *output_len = len / 2;

    for(int i = 0; i < len / 2; i++)
    {
      const int hi = TO_BINARY(input[2 * i]);
      const int lo = TO_BINARY(input[2 * i + 1]);
      output[i] = (hi << 4) | lo;
    }
#undef TO_BINARY
  }

  return output;
}

static void _exif_import_tags(dt_image_t *img, Exiv2::XmpData::iterator &pos)
{
  // tags in array
  const int cnt = pos->count();

  sqlite3_stmt *stmt_sel_id, *stmt_ins_tags, *stmt_ins_tagged;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select id from tags where name = ?1", -1,
                              &stmt_sel_id, NULL);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "insert into tags (id, name) values (null, ?1)",
                              -1, &stmt_ins_tags, NULL);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "insert into tagged_images (tagid, imgid) values (?1, ?2)", -1,
                              &stmt_ins_tagged, NULL);
  for(int i = 0; i < cnt; i++)
  {
    char tagbuf[1024];
    std::string pos_str = pos->toString(i);
    g_strlcpy(tagbuf, pos_str.c_str(), sizeof(tagbuf));
    int tagid = -1;
    char *tag = tagbuf;
    while(tag)
    {
      char *next_tag = strstr(tag, ",");
      if(next_tag) *(next_tag++) = 0;
      // check if tag is available, get its id:
      for(int k = 0; k < 2; k++)
      {
        DT_DEBUG_SQLITE3_BIND_TEXT(stmt_sel_id, 1, tag, -1, SQLITE_TRANSIENT);
        if(sqlite3_step(stmt_sel_id) == SQLITE_ROW) tagid = sqlite3_column_int(stmt_sel_id, 0);
        sqlite3_reset(stmt_sel_id);
        sqlite3_clear_bindings(stmt_sel_id);

        if(tagid > 0) break;

        fprintf(stderr, "[xmp_import] creating tag: %s\n", tag);
        // create this tag (increment id, leave icon empty), retry.
        DT_DEBUG_SQLITE3_BIND_TEXT(stmt_ins_tags, 1, tag, -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt_ins_tags);
        sqlite3_reset(stmt_ins_tags);
        sqlite3_clear_bindings(stmt_ins_tags);
      }
      // associate image and tag.
      DT_DEBUG_SQLITE3_BIND_INT(stmt_ins_tagged, 1, tagid);
      DT_DEBUG_SQLITE3_BIND_INT(stmt_ins_tagged, 2, img->id);
      sqlite3_step(stmt_ins_tagged);
      sqlite3_reset(stmt_ins_tagged);
      sqlite3_clear_bindings(stmt_ins_tagged);

      tag = next_tag;
    }
  }
  sqlite3_finalize(stmt_sel_id);
  sqlite3_finalize(stmt_ins_tags);
  sqlite3_finalize(stmt_ins_tagged);
}

// need a write lock on *img (non-const) to write stars (and soon color labels).
int dt_exif_xmp_read(dt_image_t *img, const char *filename, const int history_only)
{
  // exclude pfm to avoid stupid errors on the console
  const char *c = filename + strlen(filename) - 4;
  if(c >= filename && !strcmp(c, ".pfm")) return 1;
  try
  {
    // read xmp sidecar
    Exiv2::Image::AutoPtr image;
    image = Exiv2::ImageFactory::open(filename);
    assert(image.get() != 0);
    image->readMetadata();
    Exiv2::XmpData &xmpData = image->xmpData();

    sqlite3_stmt *stmt;

#if 0
    // get rid of old meta data
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "delete from meta_data where id = ?1", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, img->id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    // remove from tagged_images
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "delete from tagged_images where imgid = ?1", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, img->id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
#endif

    if(!history_only)
    {
      // otherwise we ignore title, description, ... from non-dt xmp files :(
      size_t pos = image->xmpPacket().find("xmlns:darktable=\"http://darktable.sf.net/\"");
      bool is_a_dt_xmp = (pos != std::string::npos);
      dt_exif_read_xmp_data(img, xmpData, is_a_dt_xmp, false);
    }

    Exiv2::XmpData::iterator pos;

    // convert legacy flip bits (will not be written anymore, convert to flip history item here):
    if((pos = xmpData.findKey(Exiv2::XmpKey("Xmp.darktable.raw_params"))) != xmpData.end())
    {
      int32_t i = pos->toLong();
      dt_image_raw_parameters_t raw_params = *(dt_image_raw_parameters_t *)&i;
      int32_t user_flip = raw_params.user_flip;
      img->legacy_flip.user_flip = user_flip;
      img->legacy_flip.legacy = 0;
    }

    // GPS data
    if((pos = xmpData.findKey(Exiv2::XmpKey("Xmp.exif.GPSLatitude"))) != xmpData.end())
    {
      img->latitude = _gps_string_to_number(pos->toString().c_str());
    }

    if((pos = xmpData.findKey(Exiv2::XmpKey("Xmp.exif.GPSLongitude"))) != xmpData.end())
    {
      img->longitude = _gps_string_to_number(pos->toString().c_str());
    }

    if((pos = xmpData.findKey(Exiv2::XmpKey("Xmp.darktable.auto_presets_applied"))) != xmpData.end())
    {
      int32_t i = pos->toLong();
      // set or clear bit in image struct
      if(i == 1) img->flags |= DT_IMAGE_AUTO_PRESETS_APPLIED;
      if(i == 0) img->flags &= ~DT_IMAGE_AUTO_PRESETS_APPLIED;
      // in any case, this is no legacy image.
      img->flags |= DT_IMAGE_NO_LEGACY_PRESETS;
    }
    else
    {
      // not found means 0 (old xmp)
      img->flags &= ~DT_IMAGE_AUTO_PRESETS_APPLIED;
      // so we are legacy (thus have to clear the no-legacy flag)
      img->flags &= ~DT_IMAGE_NO_LEGACY_PRESETS;
    }
    // when we are reading the xmp data it doesn't make sense to flag the image as removed
    img->flags &= ~DT_IMAGE_REMOVE;

    // forms
    Exiv2::XmpData::iterator mask;
    Exiv2::XmpData::iterator mask_name;
    Exiv2::XmpData::iterator mask_type;
    Exiv2::XmpData::iterator mask_version;
    Exiv2::XmpData::iterator mask_id;
    Exiv2::XmpData::iterator mask_nb;
    Exiv2::XmpData::iterator mask_src;
    if((mask = xmpData.findKey(Exiv2::XmpKey("Xmp.darktable.mask"))) != xmpData.end()
       && (mask_src = xmpData.findKey(Exiv2::XmpKey("Xmp.darktable.mask_src"))) != xmpData.end()
       && (mask_name = xmpData.findKey(Exiv2::XmpKey("Xmp.darktable.mask_name"))) != xmpData.end()
       && (mask_type = xmpData.findKey(Exiv2::XmpKey("Xmp.darktable.mask_type"))) != xmpData.end()
       && (mask_version = xmpData.findKey(Exiv2::XmpKey("Xmp.darktable.mask_version"))) != xmpData.end()
       && (mask_id = xmpData.findKey(Exiv2::XmpKey("Xmp.darktable.mask_id"))) != xmpData.end()
       && (mask_nb = xmpData.findKey(Exiv2::XmpKey("Xmp.darktable.mask_nb"))) != xmpData.end())
    {
      const int cnt = mask->count();
      if(cnt == mask_src->count() && cnt == mask_name->count() && cnt == mask_type->count()
         && cnt == mask_version->count() && cnt == mask_id->count() && cnt == mask_nb->count())
      {
        // clean all registered form for this image
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "delete from mask where imgid = ?1", -1,
                                    &stmt, NULL);
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, img->id);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        // register all forms
        for(int i = 0; i < cnt; i++)
        {
          DT_DEBUG_SQLITE3_PREPARE_V2(
              dt_database_get(darktable.db),
              "insert into mask (imgid, formid, form, name, version, points, points_count, source) "
              "values (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)",
              -1, &stmt, NULL);
          DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, img->id);
          DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, mask_id->toLong(i));
          DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, mask_type->toLong(i));
          std::string mask_name_str = mask_name->toString(i);
          if(mask_name_str.c_str() != NULL)
          {
            const char *mname = mask_name_str.c_str();
            DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 4, mname, -1, SQLITE_TRANSIENT);
          }
          else
          {
            const char *mname = "form";
            DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 4, mname, -1, SQLITE_TRANSIENT);
          }
          DT_DEBUG_SQLITE3_BIND_INT(stmt, 5, mask_version->toLong());
          std::string mask_str = mask->toString(i);
          const char *mask_c = mask_str.c_str();
          const size_t mask_c_len = strlen(mask_c);
          int mask_len = 0;
          const unsigned char *mask_d = dt_exif_xmp_decode(mask_c, mask_c_len, &mask_len);
          DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 6, mask_d, mask_len, SQLITE_TRANSIENT);
          DT_DEBUG_SQLITE3_BIND_INT(stmt, 7, mask_nb->toLong(i));

          std::string mask_src_str = mask_src->toString(i);
          const char *mask_src_c = mask_src_str.c_str();
          const size_t mask_src_c_len = strlen(mask_src_c);
          int mask_src_len = 0;
          unsigned char *mask_src = dt_exif_xmp_decode(mask_src_c, mask_src_c_len, &mask_src_len);
          DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 8, mask_src, mask_src_len, SQLITE_TRANSIENT);

          sqlite3_step(stmt);
          sqlite3_finalize(stmt);
        }
      }
    }

    // history
    Exiv2::XmpData::iterator ver;
    Exiv2::XmpData::iterator en;
    Exiv2::XmpData::iterator op;
    Exiv2::XmpData::iterator param;
    Exiv2::XmpData::iterator blendop = xmpData.findKey(Exiv2::XmpKey("Xmp.darktable.blendop_params"));
    Exiv2::XmpData::iterator blendop_version
        = xmpData.findKey(Exiv2::XmpKey("Xmp.darktable.blendop_version"));
    Exiv2::XmpData::iterator multi_priority = xmpData.findKey(Exiv2::XmpKey("Xmp.darktable.multi_priority"));
    Exiv2::XmpData::iterator multi_name = xmpData.findKey(Exiv2::XmpKey("Xmp.darktable.multi_name"));

    if((ver = xmpData.findKey(Exiv2::XmpKey("Xmp.darktable.history_modversion"))) != xmpData.end()
       && (en = xmpData.findKey(Exiv2::XmpKey("Xmp.darktable.history_enabled"))) != xmpData.end()
       && (op = xmpData.findKey(Exiv2::XmpKey("Xmp.darktable.history_operation"))) != xmpData.end()
       && (param = xmpData.findKey(Exiv2::XmpKey("Xmp.darktable.history_params"))) != xmpData.end())
    {
      const int cnt = ver->count();
      if(cnt == en->count() && cnt == op->count() && cnt == param->count())
      {
        // clear history
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "delete from history where imgid = ?1", -1,
                                    &stmt, NULL);
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, img->id);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        sqlite3_stmt *stmt_sel_num, *stmt_ins_hist, *stmt_upd_hist;
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                    "select num from history where imgid = ?1 and num = ?2", -1,
                                    &stmt_sel_num, NULL);
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                    "insert into history (imgid, num) values (?1, ?2)", -1, &stmt_ins_hist,
                                    NULL);
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                    "update history set operation = ?1, op_params = ?2, "
                                    "blendop_params = ?7, blendop_version = ?8, multi_priority = ?9, "
                                    "multi_name = ?10, module = ?3, enabled = ?4 "
                                    "where imgid = ?5 and num = ?6",
                                    -1, &stmt_upd_hist, NULL);
        for(int i = 0; i < cnt; i++)
        {
          const int modversion = ver->toLong(i);
          const int enabled = en->toLong(i);
          std::string op_str = op->toString(i);
          const char *operation = op_str.c_str();
          std::string param_str = param->toString(i);
          const char *param_c = param_str.c_str();
          const size_t param_c_len = strlen(param_c);
          int params_len = 0;
          unsigned char *params = dt_exif_xmp_decode(param_c, param_c_len, &params_len);
          // TODO: why this update set?
          DT_DEBUG_SQLITE3_BIND_INT(stmt_sel_num, 1, img->id);
          DT_DEBUG_SQLITE3_BIND_INT(stmt_sel_num, 2, i);
          if(sqlite3_step(stmt_sel_num) != SQLITE_ROW)
          {
            DT_DEBUG_SQLITE3_BIND_INT(stmt_ins_hist, 1, img->id);
            DT_DEBUG_SQLITE3_BIND_INT(stmt_ins_hist, 2, i);
            sqlite3_step(stmt_ins_hist);
            sqlite3_reset(stmt_ins_hist);
            sqlite3_clear_bindings(stmt_ins_hist);
          }

          DT_DEBUG_SQLITE3_BIND_TEXT(stmt_upd_hist, 1, operation, -1, SQLITE_TRANSIENT);
          DT_DEBUG_SQLITE3_BIND_BLOB(stmt_upd_hist, 2, params, params_len, SQLITE_TRANSIENT);
          DT_DEBUG_SQLITE3_BIND_INT(stmt_upd_hist, 3, modversion);
          DT_DEBUG_SQLITE3_BIND_INT(stmt_upd_hist, 4, enabled);
          DT_DEBUG_SQLITE3_BIND_INT(stmt_upd_hist, 5, img->id);
          DT_DEBUG_SQLITE3_BIND_INT(stmt_upd_hist, 6, i);

          /* check if we got blendop from xmp */
          unsigned char *blendop_params = NULL;
          if(blendop != xmpData.end() && blendop->size() > 0 && blendop->count() > i
             && blendop->toString(i).c_str() != NULL)
          {
            std::string blendop_str = blendop->toString(i);
            int blendop_size = 0;
            blendop_params
                = dt_exif_xmp_decode(blendop_str.c_str(), strlen(blendop_str.c_str()), &blendop_size);
            DT_DEBUG_SQLITE3_BIND_BLOB(stmt_upd_hist, 7, blendop_params, blendop_size, SQLITE_TRANSIENT);
          }
          else
            sqlite3_bind_null(stmt_upd_hist, 7);

          /* check if we got blendop_version from xmp; if not assume 1 as default */
          int blversion = 1;
          if(blendop_version != xmpData.end() && blendop_version->count() > i)
          {
            blversion = blendop_version->toLong(i);
          }
          DT_DEBUG_SQLITE3_BIND_INT(stmt_upd_hist, 8, blversion);

          /* multi instances */
          int mprio = 0;
          if(multi_priority != xmpData.end() && multi_priority->count() > i)
            mprio = multi_priority->toLong(i);
          DT_DEBUG_SQLITE3_BIND_INT(stmt_upd_hist, 9, mprio);
          if(multi_name != xmpData.end() && multi_name->size() > 0 && multi_name->count() > i
             && multi_name->toString(i).c_str() != NULL)
          {
            std::string multi_name_str = multi_name->toString(i);
            const char *mname = multi_name_str.c_str();
            DT_DEBUG_SQLITE3_BIND_TEXT(stmt_upd_hist, 10, mname, -1, SQLITE_TRANSIENT);
          }
          else
          {
            const char *mname = " ";
            DT_DEBUG_SQLITE3_BIND_TEXT(stmt_upd_hist, 10, mname, -1, SQLITE_TRANSIENT);
          }


          sqlite3_step(stmt_upd_hist);
          free(params);
          free(blendop_params);

          sqlite3_reset(stmt_sel_num);
          sqlite3_clear_bindings(stmt_sel_num);
          sqlite3_reset(stmt_upd_hist);
          sqlite3_clear_bindings(stmt_upd_hist);
        }
        sqlite3_finalize(stmt_sel_num);
        sqlite3_finalize(stmt_ins_hist);
        sqlite3_finalize(stmt_upd_hist);
      }
    }
  }
  catch(Exiv2::AnyError &e)
  {
    // actually nobody's interested in that if the file doesn't exist:
    // std::string s(e.what());
    // std::cerr << "[exiv2] " << s << std::endl;
    return 1;
  }
  return 0;
}

// helper to create an xmp data thing. throws exiv2 exceptions if stuff goes wrong.
static void dt_exif_xmp_read_data(Exiv2::XmpData &xmpData, const int imgid)
{
  const int xmp_version = 1;
  int stars = 1, raw_params = 0;
  double longitude = NAN, latitude = NAN;
  gchar *filename = NULL;
  // get stars and raw params from db
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get(darktable.db),
      "select filename, flags, raw_parameters, longitude, latitude from images where id = ?1", -1, &stmt,
      NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    filename = (gchar *)sqlite3_column_text(stmt, 0);
    stars = sqlite3_column_int(stmt, 1);
    raw_params = sqlite3_column_int(stmt, 2);
    if(sqlite3_column_type(stmt, 3) == SQLITE_FLOAT) longitude = sqlite3_column_double(stmt, 3);
    if(sqlite3_column_type(stmt, 4) == SQLITE_FLOAT) latitude = sqlite3_column_double(stmt, 4);
  }
  xmpData["Xmp.xmp.Rating"] = ((stars & 0x7) == 6) ? -1 : (stars & 0x7); // rejected image = -1, others = 0..5

  // The original file name
  if(filename) xmpData["Xmp.xmpMM.DerivedFrom"] = filename;

  // GPS data
  if(!std::isnan(longitude) && !std::isnan(latitude))
  {
    char long_dir = 'E', lat_dir = 'N';
    if(longitude < 0) long_dir = 'W';
    if(latitude < 0) lat_dir = 'S';

    longitude = fabs(longitude);
    latitude = fabs(latitude);

    int long_deg = (int)floor(longitude);
    int lat_deg = (int)floor(latitude);
    double long_min = (longitude - (double)long_deg) * 60.0;
    double lat_min = (latitude - (double)lat_deg) * 60.0;

    char *str = (char *)g_malloc(G_ASCII_DTOSTR_BUF_SIZE);

    g_ascii_formatd(str, G_ASCII_DTOSTR_BUF_SIZE, "%08f", long_min);
    gchar *long_str = g_strdup_printf("%d,%s%c", long_deg, str, long_dir);
    g_ascii_formatd(str, G_ASCII_DTOSTR_BUF_SIZE, "%08f", lat_min);
    gchar *lat_str = g_strdup_printf("%d,%s%c", lat_deg, str, lat_dir);

    xmpData["Xmp.exif.GPSLongitude"] = long_str;
    xmpData["Xmp.exif.GPSLatitude"] = lat_str;
    g_free(long_str);
    g_free(lat_str);
    g_free(str);
  }
  sqlite3_finalize(stmt);

  // the meta data
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select key, value from meta_data where id = ?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    int key = sqlite3_column_int(stmt, 0);
    switch(key)
    {
      case DT_METADATA_XMP_DC_CREATOR:
        xmpData["Xmp.dc.creator"] = sqlite3_column_text(stmt, 1);
        break;
      case DT_METADATA_XMP_DC_PUBLISHER:
        xmpData["Xmp.dc.publisher"] = sqlite3_column_text(stmt, 1);
        break;
      case DT_METADATA_XMP_DC_TITLE:
        xmpData["Xmp.dc.title"] = sqlite3_column_text(stmt, 1);
        break;
      case DT_METADATA_XMP_DC_DESCRIPTION:
        xmpData["Xmp.dc.description"] = sqlite3_column_text(stmt, 1);
        break;
      case DT_METADATA_XMP_DC_RIGHTS:
        xmpData["Xmp.dc.rights"] = sqlite3_column_text(stmt, 1);
        break;
    }
  }
  sqlite3_finalize(stmt);

  xmpData["Xmp.darktable.xmp_version"] = xmp_version;
  xmpData["Xmp.darktable.raw_params"] = raw_params;

  if(stars & DT_IMAGE_AUTO_PRESETS_APPLIED)
    xmpData["Xmp.darktable.auto_presets_applied"] = 1;
  else
    xmpData["Xmp.darktable.auto_presets_applied"] = 0;

  // get tags from db, store in dublin core
  Exiv2::Value::AutoPtr v1 = Exiv2::Value::create(Exiv2::xmpSeq); // or xmpBag or xmpAlt.
  Exiv2::Value::AutoPtr v2 = Exiv2::Value::create(Exiv2::xmpSeq); // or xmpBag or xmpAlt.

  GList *tags = dt_tag_get_list(imgid);
  while(tags)
  {
    v1->read((char *)tags->data);
    tags = g_list_next(tags);
  }

  GList *hierarchical = dt_tag_get_hierarchical(imgid);
  while(hierarchical)
  {
    v2->read((char *)hierarchical->data);
    hierarchical = g_list_next(hierarchical);
  }

  if(v1->count() > 0) xmpData.add(Exiv2::XmpKey("Xmp.dc.subject"), v1.get());
  if(v2->count() > 0) xmpData.add(Exiv2::XmpKey("Xmp.lr.hierarchicalSubject"), v2.get());
  /* TODO: Add tags to IPTC namespace as well */

  // color labels
  char val[2048];
  Exiv2::Value::AutoPtr v = Exiv2::Value::create(Exiv2::xmpSeq); // or xmpBag or xmpAlt.
  /* Already initialized v = Exiv2::Value::create(Exiv2::xmpSeq); // or xmpBag or xmpAlt.*/
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select color from color_labels where imgid=?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    snprintf(val, sizeof(val), "%d", sqlite3_column_int(stmt, 0));
    v->read(val);
  }
  sqlite3_finalize(stmt);
  if(v->count() > 0) xmpData.add(Exiv2::XmpKey("Xmp.darktable.colorlabels"), v.get());

  // masks:
  char key[1024];
  int num = 1;

  // create an array:
  Exiv2::XmpTextValue tvm("");
  tvm.setXmpArrayType(Exiv2::XmpValue::xaSeq);
  xmpData.add(Exiv2::XmpKey("Xmp.darktable.mask_id"), &tvm);
  xmpData.add(Exiv2::XmpKey("Xmp.darktable.mask_type"), &tvm);
  xmpData.add(Exiv2::XmpKey("Xmp.darktable.mask_name"), &tvm);
  xmpData.add(Exiv2::XmpKey("Xmp.darktable.mask_version"), &tvm);
  xmpData.add(Exiv2::XmpKey("Xmp.darktable.mask"), &tvm);
  xmpData.add(Exiv2::XmpKey("Xmp.darktable.mask_nb"), &tvm);
  xmpData.add(Exiv2::XmpKey("Xmp.darktable.mask_src"), &tvm);

  // reset tv
  tvm.setXmpArrayType(Exiv2::XmpValue::xaNone);

  DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get(darktable.db),
      "select imgid, formid, form, name, version, points, points_count, source from mask where imgid = ?1",
      -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    int32_t mask_id = sqlite3_column_int(stmt, 1);
    snprintf(val, sizeof(val), "%d", mask_id);
    tvm.read(val);
    snprintf(key, sizeof(key), "Xmp.darktable.mask_id[%d]", num);
    xmpData.add(Exiv2::XmpKey(key), &tvm);

    int32_t mask_type = sqlite3_column_int(stmt, 2);
    snprintf(val, sizeof(val), "%d", mask_type);
    tvm.read(val);
    snprintf(key, sizeof(key), "Xmp.darktable.mask_type[%d]", num);
    xmpData.add(Exiv2::XmpKey(key), &tvm);

    const char *mask_name = (const char *)sqlite3_column_text(stmt, 3);
    tvm.read(mask_name);
    snprintf(key, sizeof(key), "Xmp.darktable.mask_name[%d]", num);
    xmpData.add(Exiv2::XmpKey(key), &tvm);

    int32_t mask_version = sqlite3_column_int(stmt, 4);
    snprintf(val, sizeof(val), "%d", mask_version);
    tvm.read(val);
    snprintf(key, sizeof(key), "Xmp.darktable.mask_version[%d]", num);
    xmpData.add(Exiv2::XmpKey(key), &tvm);

    int32_t len = sqlite3_column_bytes(stmt, 5);
    char *mask_d = dt_exif_xmp_encode((const unsigned char *)sqlite3_column_blob(stmt, 5), len, NULL);
    tvm.read(mask_d);
    snprintf(key, sizeof(key), "Xmp.darktable.mask[%d]", num);
    xmpData.add(Exiv2::XmpKey(key), &tvm);
    free(mask_d);

    int32_t mask_nb = sqlite3_column_int(stmt, 6);
    snprintf(val, sizeof(val), "%d", mask_nb);
    tvm.read(val);
    snprintf(key, sizeof(key), "Xmp.darktable.mask_nb[%d]", num);
    xmpData.add(Exiv2::XmpKey(key), &tvm);

    len = sqlite3_column_bytes(stmt, 7);
    char *mask_src = dt_exif_xmp_encode((const unsigned char *)sqlite3_column_blob(stmt, 7), len, NULL);
    tvm.read(mask_src);
    snprintf(key, sizeof(key), "Xmp.darktable.mask_src[%d]", num);
    xmpData.add(Exiv2::XmpKey(key), &tvm);
    free(mask_src);

    num++;
  }
  sqlite3_finalize(stmt);


  // history stack:
  num = 1;

  // create an array:
  Exiv2::XmpTextValue tv("");
  tv.setXmpArrayType(Exiv2::XmpValue::xaSeq);
  xmpData.add(Exiv2::XmpKey("Xmp.darktable.history_modversion"), &tv);
  xmpData.add(Exiv2::XmpKey("Xmp.darktable.history_enabled"), &tv);
  xmpData.add(Exiv2::XmpKey("Xmp.darktable.history_operation"), &tv);
  xmpData.add(Exiv2::XmpKey("Xmp.darktable.history_params"), &tv);
  xmpData.add(Exiv2::XmpKey("Xmp.darktable.blendop_params"), &tv);
  xmpData.add(Exiv2::XmpKey("Xmp.darktable.blendop_version"), &tv);
  xmpData.add(Exiv2::XmpKey("Xmp.darktable.multi_priority"), &tv);
  xmpData.add(Exiv2::XmpKey("Xmp.darktable.multi_name"), &tv);

  // reset tv
  tv.setXmpArrayType(Exiv2::XmpValue::xaNone);

  DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get(darktable.db),
      "select imgid, num, module, operation, op_params, enabled, blendop_params, "
      "blendop_version, multi_priority, multi_name from history where imgid = ?1 order by num",
      -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    int32_t modversion = sqlite3_column_int(stmt, 2);
    snprintf(val, sizeof(val), "%d", modversion);
    tv.read(val);
    snprintf(key, sizeof(key), "Xmp.darktable.history_modversion[%d]", num);
    xmpData.add(Exiv2::XmpKey(key), &tv);

    int32_t enabled = sqlite3_column_int(stmt, 5);
    snprintf(val, sizeof(val), "%d", enabled);
    tv.read(val);
    snprintf(key, sizeof(key), "Xmp.darktable.history_enabled[%d]", num);
    xmpData.add(Exiv2::XmpKey(key), &tv);

    const char *op = (const char *)sqlite3_column_text(stmt, 3);
    if(!op) continue; // no op is fatal.
    tv.read(op);
    snprintf(key, sizeof(key), "Xmp.darktable.history_operation[%d]", num);
    xmpData.add(Exiv2::XmpKey(key), &tv);

    /* read and add history params */
    int32_t len = sqlite3_column_bytes(stmt, 4);
    char *vparams = dt_exif_xmp_encode((const unsigned char *)sqlite3_column_blob(stmt, 4), len, NULL);
    tv.read(vparams);
    snprintf(key, sizeof(key), "Xmp.darktable.history_params[%d]", num);
    xmpData.add(Exiv2::XmpKey(key), &tv);
    free(vparams);

    /* read and add blendop params */
    const void *blob = sqlite3_column_blob(stmt, 6);
    if(!blob) continue; // no params, no history item.
    len = sqlite3_column_bytes(stmt, 6);
    vparams = dt_exif_xmp_encode((const unsigned char *)blob, len, NULL);
    tv.read(vparams);
    snprintf(key, sizeof(key), "Xmp.darktable.blendop_params[%d]", num);
    xmpData.add(Exiv2::XmpKey(key), &tv);
    free(vparams);

    /* read and add blendop version */
    int32_t blversion = sqlite3_column_int(stmt, 7);
    snprintf(val, sizeof(val), "%d", blversion);
    tv.read(val);
    snprintf(key, sizeof(key), "Xmp.darktable.blendop_version[%d]", num);
    xmpData.add(Exiv2::XmpKey(key), &tv);

    /* read and add multi instances */
    int32_t mprio = sqlite3_column_int(stmt, 8);
    snprintf(val, sizeof(val), "%d", mprio);
    tv.read(val);
    snprintf(key, sizeof(key), "Xmp.darktable.multi_priority[%d]", num);
    xmpData.add(Exiv2::XmpKey(key), &tv);
    const char *mname = (const char *)sqlite3_column_text(stmt, 9);
    if(mname)
      tv.read(mname);
    else
      tv.read("");
    snprintf(key, sizeof(key), "Xmp.darktable.multi_name[%d]", num);
    xmpData.add(Exiv2::XmpKey(key), &tv);

    num++;
  }
  sqlite3_finalize(stmt);
  g_list_free_full(tags, g_free);
  g_list_free_full(hierarchical, g_free);
}

int dt_exif_xmp_attach(const int imgid, const char *filename)
{
  try
  {
    char input_filename[PATH_MAX] = { 0 };
    gboolean from_cache = FALSE;
    dt_image_full_path(imgid, input_filename, sizeof(input_filename), &from_cache);

    Exiv2::Image::AutoPtr img = Exiv2::ImageFactory::open(filename);
    // unfortunately it seems we have to read the metadata, to not erase the exif (which we just wrote).
    // will make export slightly slower, oh well.
    // img->clearXmpPacket();
    img->readMetadata();

    // initialize XMP and IPTC data with the one from the original file
    Exiv2::Image::AutoPtr input_image = Exiv2::ImageFactory::open(input_filename);
    if(input_image.get() != 0)
    {
      input_image->readMetadata();
      img->setIptcData(input_image->iptcData());
      img->setXmpData(input_image->xmpData());
    }
    dt_exif_xmp_read_data(img->xmpData(), imgid);
    img->writeMetadata();
    return 0;
  }
  catch(Exiv2::AnyError &e)
  {
    std::cerr << "[xmp_attach] caught exiv2 exception '" << e << "'\n";
    return -1;
  }
}

// write xmp sidecar file:
int dt_exif_xmp_write(const int imgid, const char *filename)
{
  // refuse to write sidecar for non-existent image:
  char imgfname[PATH_MAX] = { 0 };
  gboolean from_cache = TRUE;

  dt_image_full_path(imgid, imgfname, sizeof(imgfname), &from_cache);
  if(!g_file_test(imgfname, G_FILE_TEST_IS_REGULAR)) return 1;

  try
  {
    Exiv2::XmpData xmpData;
    std::string xmpPacket;
    if(g_file_test(filename, G_FILE_TEST_EXISTS))
    {
      Exiv2::DataBuf buf = Exiv2::readFile(filename);
      xmpPacket.assign(reinterpret_cast<char *>(buf.pData_), buf.size_);
      Exiv2::XmpParser::decode(xmpData, xmpPacket);
      // because XmpSeq or XmpBag are added to the list, we first have
      // to remove these so that we don't end up with a string of duplicates
      dt_remove_known_keys(xmpData);
    }

    // initialize xmp data:
    dt_exif_xmp_read_data(xmpData, imgid);

    // serialize the xmp data and output the xmp packet
    if(Exiv2::XmpParser::encode(xmpPacket, xmpData) != 0)
    {
      throw Exiv2::Error(1, "[xmp_write] failed to serialize xmp data");
    }
    std::ofstream fout(filename);
    if(fout.is_open())
    {
      fout << xmpPacket;
      fout.close();
    }
    return 0;
  }
  catch(Exiv2::AnyError &e)
  {
    std::cerr << "[xmp_write] caught exiv2 exception '" << e << "'\n";
    return -1;
  }
}

static void dt_exif_log_handler(int log_level, const char *message)
{
  if(log_level >= Exiv2::LogMsg::level()) fprintf(stderr, "[exiv2] %s\n", message);
}

void dt_exif_init()
{
  // mute exiv2:
  //   Exiv2::LogMsg::setLevel(Exiv2::LogMsg::mute);
  // preface the exiv2 messages with "[exiv2] "
  Exiv2::LogMsg::setHandler(&dt_exif_log_handler);

  Exiv2::XmpParser::initialize();
  // this has te stay with the old url (namespace already propagated outside dt)
  Exiv2::XmpProperties::registerNs("http://darktable.sf.net/", "darktable");
  Exiv2::XmpProperties::registerNs("http://ns.adobe.com/lightroom/1.0/", "lr");
}

void dt_exif_cleanup()
{
  Exiv2::XmpParser::terminate();
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
