/*
   This file is part of darktable,
   copyright (c) 2009--2013 johannes hanika.
   copyright (c) 2011 henrik andersson.
   copyright (c) 2012-2017 tobias ellinghaus.

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

#include <glib.h>
#include <sqlite3.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>
}

#include <cassert>
#include <cmath>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include <exiv2/exiv2.hpp>

#if defined(_WIN32) && defined(EXV_UNICODE_PATH)
  #define WIDEN(s) pugi::as_wide(s)
#else
  #define WIDEN(s) (s)
#endif

#include <pugixml.hpp>

using namespace std;

extern "C" {
#include "common/colorlabels.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/exif.h"
#include "common/image_cache.h"
#include "common/imageio.h"
#include "common/imageio_jpeg.h"
#include "common/metadata.h"
#include "common/tags.h"
#include "control/conf.h"
#include "develop/imageop.h"
#include "develop/blend.h"
#include "develop/masks.h"
}

// exiv2's readMetadata is not thread safe in 0.26. so we lock it. since readMetadata might throw an exception we
// wrap it into some c++ magic to make sure we unlock in all cases. well, actually not magic but basic raii.
// FIXME: check again once we rely on 0.27
class Lock
{
public:
  Lock() { dt_pthread_mutex_lock(&darktable.exiv2_threadsafe); }
  ~Lock() { dt_pthread_mutex_unlock(&darktable.exiv2_threadsafe); }
};

#define read_metadata_threadsafe(image)                       \
{                                                             \
  Lock lock;                                                  \
  image->readMetadata();                                      \
}

static void _exif_import_tags(dt_image_t *img, Exiv2::XmpData::iterator &pos);

// this array should contain all XmpBag and XmpSeq keys used by dt
const char *dt_xmp_keys[]
    = { "Xmp.dc.subject", "Xmp.lr.hierarchicalSubject", "Xmp.darktable.colorlabels", "Xmp.darktable.history",
        "Xmp.darktable.history_modversion", "Xmp.darktable.history_enabled", "Xmp.darktable.history_end",
        "Xmp.darktable.history_operation", "Xmp.darktable.history_params", "Xmp.darktable.blendop_params",
        "Xmp.darktable.blendop_version", "Xmp.darktable.multi_priority", "Xmp.darktable.multi_name",
        "Xmp.darktable.xmp_version", "Xmp.darktable.raw_params", "Xmp.darktable.auto_presets_applied",
        "Xmp.darktable.mask_id", "Xmp.darktable.mask_type", "Xmp.darktable.mask_name",
        "Xmp.darktable.mask_version", "Xmp.darktable.mask", "Xmp.darktable.mask_nb", "Xmp.darktable.mask_src",
        "Xmp.dc.creator", "Xmp.dc.publisher", "Xmp.dc.title", "Xmp.dc.description", "Xmp.dc.rights",
        "Xmp.xmpMM.DerivedFrom" };

static const guint dt_xmp_keys_n = G_N_ELEMENTS(dt_xmp_keys); // the number of XmpBag XmpSeq keys that dt uses

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

// function to remove known dt keys and subtrees from xmpdata, so not to append them twice
// this should work because dt first reads all known keys
static void dt_remove_known_keys(Exiv2::XmpData &xmp)
{
  xmp.sortByKey();
  for(unsigned int i = 0; i < dt_xmp_keys_n; i++)
  {
    Exiv2::XmpData::iterator pos = xmp.findKey(Exiv2::XmpKey(dt_xmp_keys[i]));

    while(pos != xmp.end())
    {
      std::string key = pos->key();
      const char *ckey = key.c_str();
      size_t len = key.size();
      // stop iterating once the key no longer matches what we are trying to delete. this assumes sorted input
      if(!(g_str_has_prefix(ckey, dt_xmp_keys[i]) && (ckey[len] == '[' || ckey[len] == '\0')))
        break;
      pos = xmp.erase(pos);
    }
  }
}

static void dt_remove_exif_keys(Exiv2::ExifData &exif, const char *keys[], unsigned int n_keys)
{
  for(unsigned int i = 0; i < n_keys; i++)
  {
    try
    {
      Exiv2::ExifData::iterator pos;
      while((pos = exif.findKey(Exiv2::ExifKey(keys[i]))) != exif.end())
        exif.erase(pos);
    }
    catch(Exiv2::AnyError &e)
    {
      // the only exception we may get is "invalid" tag, which is not
      // important enough to either stop the function, or even display
      // a message (it's probably the tag is not implemented in the
      // exiv2 version used)
    }
  }
}

static void dt_remove_xmp_keys(Exiv2::XmpData &xmp, const char *keys[], unsigned int n_keys)
{
  for(unsigned int i = 0; i < n_keys; i++)
  {
    try
    {
      Exiv2::XmpData::iterator pos;
      while((pos = xmp.findKey(Exiv2::XmpKey(keys[i]))) != xmp.end())
        xmp.erase(pos);
    }
    catch(Exiv2::AnyError &e)
    {
      // the only exception we may get is "invalid" tag, which is not
      // important enough to either stop the function, or even display
      // a message (it's probably the tag is not implemented in the
      // exiv2 version used)
    }
  }
}

static bool dt_exif_read_xmp_tag(Exiv2::XmpData &xmpData, Exiv2::XmpData::iterator *pos, string key)
{
  try
  {
    return (*pos = xmpData.findKey(Exiv2::XmpKey(key))) != xmpData.end() && (*pos)->size();
  }
  catch(Exiv2::AnyError &e)
  {
    std::string s(e.what());
    std::cerr << "[exiv2] " << s << std::endl;
    return false;
  }
}
#define FIND_XMP_TAG(key) dt_exif_read_xmp_tag(xmpData, &pos, key)


// FIXME: according to http://www.exiv2.org/doc/classExiv2_1_1Metadatum.html#63c2b87249ba96679c29e01218169124
// there is no need to pass xmpData
// version = -1 -> version ignored
static bool dt_exif_read_xmp_data(dt_image_t *img, Exiv2::XmpData &xmpData, int version,
                                  bool use_default_rating)
{
  try
  {
    Exiv2::XmpData::iterator pos;

    // older darktable version did not write this data correctly:
    // the reasoning behind strdup'ing all the strings before passing it to sqlite3 is, that
    // they are somehow corrupt after the call to sqlite3_prepare_v2() -- don't ask me
    // why for they don't get passed to that function.
    if(version == -1 || version > 0)
    {
      if(FIND_XMP_TAG("Xmp.dc.rights"))
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
      if(FIND_XMP_TAG("Xmp.dc.description"))
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
      if(FIND_XMP_TAG("Xmp.dc.title"))
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
      if(FIND_XMP_TAG("Xmp.dc.creator"))
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
      if(FIND_XMP_TAG("Xmp.dc.publisher"))
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

    if(FIND_XMP_TAG("Xmp.xmp.Rating"))
    {
      int stars = pos->toLong();
      if(use_default_rating && stars == 0) stars = dt_conf_get_int("ui_last/import_initial_rating");

      stars = (stars == -1) ? 6 : stars;
      img->flags = (img->flags & ~0x7) | (0x7 & stars);
    }

    if(FIND_XMP_TAG("Xmp.xmp.Label"))
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
    if(FIND_XMP_TAG("Xmp.darktable.colorlabels"))
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

    if(FIND_XMP_TAG("Xmp.lr.hierarchicalSubject"))
      _exif_import_tags(img, pos);
    else if(FIND_XMP_TAG("Xmp.dc.subject"))
      _exif_import_tags(img, pos);

    /* read gps location */
    if(FIND_XMP_TAG("Xmp.exif.GPSLatitude"))
    {
      img->latitude = dt_util_gps_string_to_number(pos->toString().c_str());
    }

    if(FIND_XMP_TAG("Xmp.exif.GPSLongitude"))
    {
      img->longitude = dt_util_gps_string_to_number(pos->toString().c_str());
    }

    if(FIND_XMP_TAG("Xmp.exif.GPSAltitude"))
    {
      Exiv2::XmpData::const_iterator ref = xmpData.findKey(Exiv2::XmpKey("Xmp.exif.GPSAltitudeRef"));
      if(ref != xmpData.end() && ref->size())
      {
        std::string sign_str = ref->toString();
        const char *sign = sign_str.c_str();
        double elevation = 0.0;
        if(dt_util_gps_elevation_to_number(pos->toRational(0).first, pos->toRational(0).second, sign[0], &elevation))
          img->elevation = elevation;
      }
    }

    /* read lens type from Xmp.exifEX.LensModel */
    if(FIND_XMP_TAG("Xmp.exifEX.LensModel"))
    {
      // lens model
      char *lens = strdup(pos->toString().c_str());
      char *adr =  lens;
      if(strncmp(lens, "lang=", 5) == 0)
      {
        lens = strchr(lens, ' ');
        if(lens != NULL) lens++;
      }
      // no need to do any Unicode<->locale conversion, the field is specified as ASCII
      g_strlcpy(img->exif_lens, lens, sizeof(img->exif_lens));
      free(adr);
    }

    /* read timestamp from Xmp.exif.DateTimeOriginal */
    if(FIND_XMP_TAG("Xmp.exif.DateTimeOriginal"))
    {
      char *datetime = strdup(pos->toString().c_str());

      /*
       * exiftool (but apparently not evix2) convert
       * e.g. "2017:10:23 12:34:56" to "2017-10-23T12:34:54" (ISO)
       * revert this to the format expected by exif and darktable
       */

      // replace 'T' by ' ' (space)
      char *c ;
      while ( ( c = strchr(datetime,'T') ) != NULL )
      {
	*c = ' ';
      }
      // replace '-' by ':'
      while ( ( c = strchr(datetime,'-')) != NULL ) {
	*c = ':';
      }

      g_strlcpy(img->exif_datetime_taken, datetime, sizeof(img->exif_datetime_taken));
      free(datetime);
    }

    return true;
  }
  catch(Exiv2::AnyError &e)
  {
    std::string s(e.what());
    std::cerr << "[exiv2] " << img->filename << ": " << s << std::endl;
    return false;
  }
}

static bool dt_exif_read_iptc_tag(Exiv2::IptcData &iptcData, Exiv2::IptcData::const_iterator *pos, string key)
{
  try
  {
    return (*pos = iptcData.findKey(Exiv2::IptcKey(key))) != iptcData.end() && (*pos)->size();
  }
  catch(Exiv2::AnyError &e)
  {
    std::string s(e.what());
    std::cerr << "[exiv2] " << s << std::endl;
    return false;
  }
}
#define FIND_IPTC_TAG(key) dt_exif_read_iptc_tag(iptcData, &pos, key)


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
    if(FIND_IPTC_TAG("Iptc.Application2.Caption"))
    {
      std::string str = pos->print(/*&iptcData*/);
      dt_metadata_set(img->id, "Xmp.dc.description", str.c_str());
    }
    if(FIND_IPTC_TAG("Iptc.Application2.Copyright"))
    {
      std::string str = pos->print(/*&iptcData*/);
      dt_metadata_set(img->id, "Xmp.dc.rights", str.c_str());
    }
    if(FIND_IPTC_TAG("Iptc.Application2.Writer"))
    {
      std::string str = pos->print(/*&iptcData*/);
      dt_metadata_set(img->id, "Xmp.dc.creator", str.c_str());
    }
    else if(FIND_IPTC_TAG("Iptc.Application2.Contact"))
    {
      std::string str = pos->print(/*&iptcData*/);
      dt_metadata_set(img->id, "Xmp.dc.creator", str.c_str());
    }

    return true;
  }
  catch(Exiv2::AnyError &e)
  {
    std::string s(e.what());
    std::cerr << "[exiv2] " << img->filename << ": " << s << std::endl;
    return false;
  }
}

static bool dt_exif_read_exif_tag(Exiv2::ExifData &exifData, Exiv2::ExifData::const_iterator *pos, string key)
{
  try
  {
    return (*pos = exifData.findKey(Exiv2::ExifKey(key))) != exifData.end() && (*pos)->size();
  }
  catch(Exiv2::AnyError &e)
  {
    std::string s(e.what());
    std::cerr << "[exiv2] " << s << std::endl;
    return false;
  }
}
#define FIND_EXIF_TAG(key) dt_exif_read_exif_tag(exifData, &pos, key)

static void _find_datetime_taken(Exiv2::ExifData &exifData, Exiv2::ExifData::const_iterator pos,
                                 char *exif_datetime_taken)
{
  if(FIND_EXIF_TAG("Exif.Image.DateTimeOriginal"))
  {
    dt_strlcpy_to_utf8(exif_datetime_taken, 20, pos, exifData);
  }
  else if(FIND_EXIF_TAG("Exif.Photo.DateTimeOriginal"))
  {
    dt_strlcpy_to_utf8(exif_datetime_taken, 20, pos, exifData);
  }
  else
  {
    *exif_datetime_taken = '\0';
  }
}

static bool dt_exif_read_exif_data(dt_image_t *img, Exiv2::ExifData &exifData)
{
  try
  {
    /* List of tag names taken from exiv2's printSummary() in actions.cpp */
    Exiv2::ExifData::const_iterator pos;

    // look for maker & model first so we can use that info later
    if(FIND_EXIF_TAG("Exif.Image.Make"))
    {
      dt_strlcpy_to_utf8(img->exif_maker, sizeof(img->exif_maker), pos, exifData);
    }
    else if(FIND_EXIF_TAG("Exif.PanasonicRaw.Make"))
    {
      dt_strlcpy_to_utf8(img->exif_maker, sizeof(img->exif_maker), pos, exifData);
    }

    for(char *c = img->exif_maker + sizeof(img->exif_maker) - 1; c > img->exif_maker; c--)
      if(*c != ' ' && *c != '\0')
      {
        *(c + 1) = '\0';
        break;
      }

    if(FIND_EXIF_TAG("Exif.Image.Model"))
    {
      dt_strlcpy_to_utf8(img->exif_model, sizeof(img->exif_model), pos, exifData);
    }
    else if(FIND_EXIF_TAG("Exif.PanasonicRaw.Model"))
    {
      dt_strlcpy_to_utf8(img->exif_model, sizeof(img->exif_model), pos, exifData);
    }

    for(char *c = img->exif_model + sizeof(img->exif_model) - 1; c > img->exif_model; c--)
      if(*c != ' ' && *c != '\0')
      {
        *(c + 1) = '\0';
        break;
      }

    // Make sure we copy the exif make and model to the correct place if needed
    dt_image_refresh_makermodel(img);

    /* Read shutter time */
    if(FIND_EXIF_TAG("Exif.Photo.ExposureTime"))
    {
      // dt_strlcpy_to_utf8(uf->conf->shutterText, max_name, pos, exifData);
      img->exif_exposure = pos->toFloat();
    }
    else if(FIND_EXIF_TAG("Exif.Photo.ShutterSpeedValue"))
    {
      // uf_strlcpy_to_utf8(uf->conf->shutterText, max_name, pos, exifData);
      img->exif_exposure = 1.0 / pos->toFloat();
    }
    /* Read aperture */
    if(FIND_EXIF_TAG("Exif.Photo.FNumber"))
    {
      img->exif_aperture = pos->toFloat();
    }
    else if(FIND_EXIF_TAG("Exif.Photo.ApertureValue"))
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
        std::string str = pos->print();
        img->exif_iso = (float)std::atof(str.c_str());
      }
    }
    // some newer cameras support iso settings that exceed the 16 bit of exif's ISOSpeedRatings
    if(img->exif_iso == 65535 || img->exif_iso == 0)
    {
      if(FIND_EXIF_TAG("Exif.PentaxDng.ISO") || FIND_EXIF_TAG("Exif.Pentax.ISO"))
      {
        std::string str = pos->print();
        img->exif_iso = (float)std::atof(str.c_str());
      }
      else if((!g_strcmp0(img->exif_maker, "SONY") || !g_strcmp0(img->exif_maker, "Canon"))
        && FIND_EXIF_TAG("Exif.Photo.RecommendedExposureIndex"))
      {
        img->exif_iso = pos->toFloat();
      }
    }

    /* Read focal length  */
    if((pos = Exiv2::focalLength(exifData)) != exifData.end() && pos->size())
    {
      // This works around a bug in exiv2 the developers refuse to fix
      // For details see http://dev.exiv2.org/issues/1083
      if (pos->key() == "Exif.Canon.FocalLength" && pos->count() == 4)
        img->exif_focal_length = pos->toFloat(1);
      else
        img->exif_focal_length = pos->toFloat();
    }

    /* Read focal length in 35mm if available and try to calculate crop factor */
    if(FIND_EXIF_TAG("Exif.Photo.FocalLengthIn35mmFilm"))
    {
      const float focal_length_35mm = pos->toFloat();
      if(focal_length_35mm > 0.0f && img->exif_focal_length > 0.0f)
        img->exif_crop = focal_length_35mm / img->exif_focal_length;
      else
        img->exif_crop = 1.0f;
    }

    if(FIND_EXIF_TAG("Exif.NikonLd2.FocusDistance"))
    {
      float value = pos->toFloat();
      img->exif_focus_distance = (0.01 * pow(10, value / 40));
    }
    else if(FIND_EXIF_TAG("Exif.NikonLd3.FocusDistance"))
    {
      float value = pos->toFloat();
      img->exif_focus_distance = (0.01 * pow(10, value / 40));
    }
    else if(FIND_EXIF_TAG("Exif.OlympusFi.FocusDistance"))
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
    else if(Exiv2::testVersion(0,25,0) && FIND_EXIF_TAG("Exif.CanonFi.FocusDistanceUpper"))
    {
      float FocusDistanceUpper = pos->toFloat();
      if(FocusDistanceUpper <= 0.0f || FocusDistanceUpper >= 0xffff)
      {
        img->exif_focus_distance = 0.0f;
      }
      else if(FIND_EXIF_TAG("Exif.CanonFi.FocusDistanceLower"))
      {
        float FocusDistanceLower = pos->toFloat();
        img->exif_focus_distance = (FocusDistanceLower + FocusDistanceUpper) / 200;
      }
    }
    else if((pos = Exiv2::subjectDistance(exifData)) != exifData.end() && pos->size())
    {
      img->exif_focus_distance = pos->toFloat();
    }
    /** read image orientation */
    if(FIND_EXIF_TAG("Exif.Image.Orientation"))
    {
      img->orientation = dt_image_orientation_to_flip_bits(pos->toLong());
    }
    else if(FIND_EXIF_TAG("Exif.PanasonicRaw.Orientation"))
    {
      img->orientation = dt_image_orientation_to_flip_bits(pos->toLong());
    }

    /* read gps location */
    if(FIND_EXIF_TAG("Exif.GPSInfo.GPSLatitude"))
    {
      Exiv2::ExifData::const_iterator ref = exifData.findKey(Exiv2::ExifKey("Exif.GPSInfo.GPSLatitudeRef"));
      if(ref != exifData.end() && ref->size())
      {
        std::string sign_str = ref->toString();
        const char *sign = sign_str.c_str();
        double latitude = 0.0;
        if(dt_util_gps_rationale_to_number(pos->toRational(0).first, pos->toRational(0).second,
                                           pos->toRational(1).first, pos->toRational(1).second,
                                           pos->toRational(2).first, pos->toRational(2).second, sign[0], &latitude))
          img->latitude = latitude;
      }
    }

    if(FIND_EXIF_TAG("Exif.GPSInfo.GPSLongitude"))
    {
      Exiv2::ExifData::const_iterator ref = exifData.findKey(Exiv2::ExifKey("Exif.GPSInfo.GPSLongitudeRef"));
      if(ref != exifData.end() && ref->size())
      {
        std::string sign_str = ref->toString();
        const char *sign = sign_str.c_str();
        double longitude = 0.0;
        if(dt_util_gps_rationale_to_number(pos->toRational(0).first, pos->toRational(0).second,
                                           pos->toRational(1).first, pos->toRational(1).second,
                                           pos->toRational(2).first, pos->toRational(2).second, sign[0], &longitude))
          img->longitude = longitude;
      }
    }

    if(FIND_EXIF_TAG("Exif.GPSInfo.GPSAltitude"))
    {
      Exiv2::ExifData::const_iterator ref = exifData.findKey(Exiv2::ExifKey("Exif.GPSInfo.GPSAltitudeRef"));
      if(ref != exifData.end() && ref->size())
      {
        std::string sign_str = ref->toString();
        const char *sign = sign_str.c_str();
        double elevation = 0.0;
        if(dt_util_gps_elevation_to_number(pos->toRational(0).first, pos->toRational(0).second, sign[0], &elevation))
          img->elevation = elevation;
      }
    }

    /* Read lens name */
    if((FIND_EXIF_TAG("Exif.CanonCs.LensType") && pos->print(&exifData) != "(0)"
        && pos->print(&exifData) != "(65535)")
       || FIND_EXIF_TAG("Exif.Canon.0x0095"))
    {
      dt_strlcpy_to_utf8(img->exif_lens, sizeof(img->exif_lens), pos, exifData);
    }
    else if(Exiv2::testVersion(0,25,0) && FIND_EXIF_TAG("Exif.PentaxDng.LensType"))
    {
      dt_strlcpy_to_utf8(img->exif_lens, sizeof(img->exif_lens), pos, exifData);
    }
    else if(FIND_EXIF_TAG("Exif.Panasonic.LensType"))
    {
      dt_strlcpy_to_utf8(img->exif_lens, sizeof(img->exif_lens), pos, exifData);
    }
    else if(FIND_EXIF_TAG("Exif.OlympusEq.LensType"))
    {
      /* For every Olympus camera Exif.OlympusEq.LensType is present. */
      dt_strlcpy_to_utf8(img->exif_lens, sizeof(img->exif_lens), pos, exifData);

      /* We have to check if Exif.OlympusEq.LensType has been translated by
       * exiv2. If it hasn't, fall back to Exif.OlympusEq.LensModel. */
      std::string lens(img->exif_lens);
      if(std::string::npos == lens.find_first_not_of(" 1234567890"))
      {
        /* Exif.OlympusEq.LensType contains only digits and spaces.
         * This means that exiv2 couldn't convert it to human readable
         * form. */
        if(FIND_EXIF_TAG("Exif.OlympusEq.LensModel"))
        {
          dt_strlcpy_to_utf8(img->exif_lens, sizeof(img->exif_lens), pos, exifData);
        }
        /* Just in case Exif.OlympusEq.LensModel hasn't been found */
        else if(FIND_EXIF_TAG("Exif.Photo.LensModel"))
        {
          dt_strlcpy_to_utf8(img->exif_lens, sizeof(img->exif_lens), pos, exifData);
        }
        fprintf(stderr, "[exif] Warning: lens \"%s\" unknown as \"%s\"\n", img->exif_lens, lens.c_str());
      }
    }
    else if((pos = Exiv2::lensName(exifData)) != exifData.end() && pos->size())
    {
      dt_strlcpy_to_utf8(img->exif_lens, sizeof(img->exif_lens), pos, exifData);
    }
    else if(FIND_EXIF_TAG("Exif.Photo.LensModel"))
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

    _find_datetime_taken(exifData, pos, img->exif_datetime_taken);

    if(FIND_EXIF_TAG("Exif.Image.Artist"))
    {
      std::string str = pos->print(&exifData);
      dt_metadata_set(img->id, "Xmp.dc.creator", str.c_str());
    }
    else if(FIND_EXIF_TAG("Exif.Canon.OwnerName"))
    {
      std::string str = pos->print(&exifData);
      dt_metadata_set(img->id, "Xmp.dc.creator", str.c_str());
    }

    // FIXME: Should the UserComment go into the description? Or do we need an extra field for this?
    if(FIND_EXIF_TAG("Exif.Photo.UserComment"))
    {
      std::string str = pos->print(&exifData);
      dt_metadata_set(img->id, "Xmp.dc.description", str.c_str());
    }

    if(FIND_EXIF_TAG("Exif.Image.Copyright"))
    {
      std::string str = pos->print(&exifData);
      dt_metadata_set(img->id, "Xmp.dc.rights", str.c_str());
    }

    if(FIND_EXIF_TAG("Exif.Image.Rating"))
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
    else if(FIND_EXIF_TAG("Exif.Image.RatingPercent"))
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
      if(FIND_EXIF_TAG("Exif.Image.CalibrationIlluminant1"))
      {
        is_1_65 = (pos->toLong() == 21) ? 1 : 0;
      }
      if(FIND_EXIF_TAG("Exif.Image.CalibrationIlluminant2"))
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
      else if(cm1_pos != exifData.end() && cm1_pos->count() == 9 && cm1_pos->size())
        for(int i = 0; i < 9; i++) img->d65_color_matrix[i] = cm1_pos->toFloat(i);
      else if(cm2_pos != exifData.end() && cm2_pos->count() == 9 && cm2_pos->size())
        for(int i = 0; i < 9; i++) img->d65_color_matrix[i] = cm2_pos->toFloat(i);
    }

    // some files have the colorspace explicitly set. try to read that.
    // is_ldr -> none
    // 0x01   -> sRGB
    // 0x02   -> AdobeRGB
    // 0xffff -> Uncalibrated
    //          + Exif.Iop.InteroperabilityIndex of 'R03' -> AdobeRGB
    //          + Exif.Iop.InteroperabilityIndex of 'R98' -> sRGB
    if(dt_image_is_ldr(img) && FIND_EXIF_TAG("Exif.Photo.ColorSpace"))
    {
      int colorspace = pos->toLong();
      if(colorspace == 0x01)
        img->colorspace = DT_IMAGE_COLORSPACE_SRGB;
      else if(colorspace == 0x02)
        img->colorspace = DT_IMAGE_COLORSPACE_ADOBE_RGB;
      else if(colorspace == 0xffff)
      {
        if(FIND_EXIF_TAG("Exif.Iop.InteroperabilityIndex"))
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

    // Improve lens detection for Sony SAL lenses.
    if(FIND_EXIF_TAG("Exif.Sony2.LensID") && pos->toLong() != 65535 && pos->print().find('|') == std::string::npos)
    {
      dt_strlcpy_to_utf8(img->exif_lens, sizeof(img->exif_lens), pos, exifData);
    }
    // Workaround for an issue on newer Sony NEX cams.
    // The default EXIF field is not used by Sony to store lens data
    // http://dev.exiv2.org/issues/883
    // http://darktable.org/redmine/issues/8813
    // FIXME: This is still a workaround
    else if((!strncmp(img->exif_model, "NEX", 3)) || (!strncmp(img->exif_model, "ILCE", 4)))
    {
      snprintf(img->exif_lens, sizeof(img->exif_lens), "(unknown)");
      if(FIND_EXIF_TAG("Exif.Photo.LensModel"))
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
    std::cerr << "[exiv2] " << img->filename << ": " << s << std::endl;
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
    std::cerr << "[exiv2] " << img->filename << ": " << s << std::endl;
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
    std::unique_ptr<Exiv2::Image> image(Exiv2::ImageFactory::open(WIDEN(path)));
    assert(image.get() != 0);
    read_metadata_threadsafe(image);

    // Get a list of preview images available in the image. The list is sorted
    // by the preview image pixel size, starting with the smallest preview.
    Exiv2::PreviewManager loader(*image);
    Exiv2::PreviewPropertiesList list = loader.getPreviewProperties();
    if(list.empty())
    {
      dt_print(DT_DEBUG_LIGHTTABLE, "[exiv2] couldn't find thumbnail for %s", path);
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
    std::unique_ptr<Exiv2::Image> image(Exiv2::ImageFactory::open(WIDEN(path)));
    assert(image.get() != 0);
    read_metadata_threadsafe(image);
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
    if(!xmpData.empty()) res = dt_exif_read_xmp_data(img, xmpData, -1, true) && res;

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

int dt_exif_write_blob(uint8_t *blob, uint32_t size, const char *path, const int compressed)
{
  try
  {
    std::unique_ptr<Exiv2::Image> image(Exiv2::ImageFactory::open(WIDEN(path)));
    assert(image.get() != 0);
    read_metadata_threadsafe(image);
    Exiv2::ExifData &imgExifData = image->exifData();
    Exiv2::ExifData blobExifData;
    Exiv2::ExifParser::decode(blobExifData, blob + 6, size);
    Exiv2::ExifData::const_iterator end = blobExifData.end();
    Exiv2::ExifData::iterator it;
    for(Exiv2::ExifData::const_iterator i = blobExifData.begin(); i != end; ++i)
    {
      // add() does not override! we need to delete existing key first.
      Exiv2::ExifKey key(i->key());
      if((it = imgExifData.findKey(key)) != imgExifData.end()) imgExifData.erase(it);

      imgExifData.add(Exiv2::ExifKey(i->key()), &i->value());
    }

    {
      // Remove thumbnail
      static const char *keys[] = {
        "Exif.Thumbnail.Compression",
        "Exif.Thumbnail.XResolution",
        "Exif.Thumbnail.YResolution",
        "Exif.Thumbnail.ResolutionUnit",
        "Exif.Thumbnail.JPEGInterchangeFormat",
        "Exif.Thumbnail.JPEGInterchangeFormatLength"
      };
      static const guint n_keys = G_N_ELEMENTS(keys);
      dt_remove_exif_keys(imgExifData, keys, n_keys);
    }

    // only compressed images may set PixelXDimension and PixelYDimension
    if(!compressed)
    {
      static const char *keys[] = {
        "Exif.Photo.PixelXDimension",
        "Exif.Photo.PixelYDimension"
      };
      static const guint n_keys = G_N_ELEMENTS(keys);
      dt_remove_exif_keys(imgExifData, keys, n_keys);
    }

    imgExifData.sortByTag();
    image->writeMetadata();
  }
  catch(Exiv2::AnyError &e)
  {
    std::string s(e.what());
    std::cerr << "[exiv2] " << path << ": " << s << std::endl;
    return 0;
  }
  return 1;
}

int dt_exif_read_blob(uint8_t **buf, const char *path, const int imgid, const int sRGB, const int out_width,
                      const int out_height, const int dng_mode)
{
  *buf = NULL;
  try
  {
    std::unique_ptr<Exiv2::Image> image(Exiv2::ImageFactory::open(WIDEN(path)));
    assert(image.get() != 0);
    read_metadata_threadsafe(image);
    Exiv2::ExifData &exifData = image->exifData();

    // get rid of thumbnails
    Exiv2::ExifThumb(exifData).erase();

    // ufraw-style exif stripping:
    Exiv2::ExifData::iterator pos;
    {
    /* Delete original TIFF data, which is irrelevant*/
      static const char *keys[] = {
        "Exif.Image.ImageWidth",
        "Exif.Image.ImageLength",
        "Exif.Image.BitsPerSample",
        "Exif.Image.Compression",
        "Exif.Image.PhotometricInterpretation",
        "Exif.Image.FillOrder",
        "Exif.Image.SamplesPerPixel",
        "Exif.Image.StripOffsets",
        "Exif.Image.RowsPerStrip",
        "Exif.Image.StripByteCounts",
        "Exif.Image.PlanarConfiguration",
        "Exif.Image.DNGVersion",
        "Exif.Image.DNGBackwardVersion"
      };
      static const guint n_keys = G_N_ELEMENTS(keys);
      dt_remove_exif_keys(exifData, keys, n_keys);
    }

    if(!dng_mode)
    {
      /* Delete various MakerNote fields only applicable to the raw file */

      exifData["Exif.Image.Orientation"] = uint16_t(1);

      {
        static const char *keys[] = {
          // Embedded color profile info
          "Exif.Image.BaselineExposureOffset",
          "Exif.Image.CalibrationIlluminant1",
          "Exif.Image.CalibrationIlluminant2",
          "Exif.Image.ColorMatrix1",
          "Exif.Image.ColorMatrix2",
          "Exif.Image.DefaultBlackRender",
          "Exif.Image.ForwardMatrix1",
          "Exif.Image.ForwardMatrix2",
          "Exif.Image.ProfileCalibrationSignature",
          "Exif.Image.ProfileCopyright",
          "Exif.Image.ProfileEmbedPolicy",
          "Exif.Image.ProfileHueSatMapData1",
          "Exif.Image.ProfileHueSatMapData2",
          "Exif.Image.ProfileHueSatMapDims",
          "Exif.Image.ProfileHueSatMapEncoding",
          "Exif.Image.ProfileLookTableData",
          "Exif.Image.ProfileLookTableDims",
          "Exif.Image.ProfileLookTableEncoding",
          "Exif.Image.ProfileName",
          "Exif.Image.ProfileToneCurve",
          "Exif.Image.ReductionMatrix1",
          "Exif.Image.ReductionMatrix2",

          // Canon color space info
          "Exif.Canon.ColorSpace",
          "Exif.Canon.ColorData",

          // Nikon thumbnail data
          "Exif.Nikon3.Preview",
          "Exif.NikonPreview.JPEGInterchangeFormat",

          // DNG private data
          "Exif.Image.DNGPrivateData",

          // Pentax thumbnail data
          "Exif.Pentax.PreviewResolution",
          "Exif.Pentax.PreviewLength",
          "Exif.Pentax.PreviewOffset",
          "Exif.PentaxDng.PreviewResolution",
          "Exif.PentaxDng.PreviewLength",
          "Exif.PentaxDng.PreviewOffset",
          // Pentax color info
          "Exif.PentaxDng.ColorInfo",

          // Minolta thumbnail data
          "Exif.Minolta.Thumbnail",
          "Exif.Minolta.ThumbnailOffset",
          "Exif.Minolta.ThumbnailLength",

          // Sony thumbnail data
          "Exif.SonyMinolta.ThumbnailOffset",
          "Exif.SonyMinolta.ThumbnailLength",

          // Olympus thumbnail data
          "Exif.Olympus.Thumbnail",
          "Exif.Olympus.ThumbnailOffset",
          "Exif.Olympus.ThumbnailLength"
        };
        static const guint n_keys = G_N_ELEMENTS(keys);
        dt_remove_exif_keys(exifData, keys, n_keys);
      }

      // remove subimage* trees, related to thumbnails or HDR usually
      for(Exiv2::ExifData::iterator i = exifData.begin(); i != exifData.end();)
      {
        static const std::string needle = "Exif.SubImage";
        if(i->key().compare(0, needle.length(), needle) == 0)
          i = exifData.erase(i);
        else
          ++i;
      }

#if EXIV2_MINOR_VERSION >= 23
      {
        // Exiv2 versions older than 0.23 drop all EXIF if the code below is executed
        // Samsung makernote cleanup, the entries below have no relevance for exported images
        static const char *keys[] = {
          "Exif.Samsung2.SensorAreas",
          "Exif.Samsung2.ColorSpace",
          "Exif.Samsung2.EncryptionKey",
          "Exif.Samsung2.WB_RGGBLevelsUncorrected",
          "Exif.Samsung2.WB_RGGBLevelsAuto",
          "Exif.Samsung2.WB_RGGBLevelsIlluminator1",
          "Exif.Samsung2.WB_RGGBLevelsIlluminator2",
          "Exif.Samsung2.WB_RGGBLevelsBlack",
          "Exif.Samsung2.ColorMatrix",
          "Exif.Samsung2.ColorMatrixSRGB",
          "Exif.Samsung2.ColorMatrixAdobeRGB",
          "Exif.Samsung2.ToneCurve1",
          "Exif.Samsung2.ToneCurve2",
          "Exif.Samsung2.ToneCurve3",
          "Exif.Samsung2.ToneCurve4"
        };
        static const guint n_keys = G_N_ELEMENTS(keys);
        dt_remove_exif_keys(exifData, keys, n_keys);
      }
#endif

      /* Write appropriate color space tag if using sRGB output */
      if(sRGB)
        exifData["Exif.Photo.ColorSpace"] = uint16_t(1); /* sRGB */
      else
        exifData["Exif.Photo.ColorSpace"] = uint16_t(0xFFFF); /* Uncalibrated */
    }

    /* Replace RAW dimension with output dimensions (for example after crop/scale, or orientation for dng
     * mode) */
    if(out_width > 0) exifData["Exif.Photo.PixelXDimension"] = (uint32_t)out_width;
    if(out_height > 0) exifData["Exif.Photo.PixelYDimension"] = (uint32_t)out_height;

    int resolution = dt_conf_get_int("metadata/resolution");
    if(resolution > 0)
    {
      exifData["Exif.Image.XResolution"] = Exiv2::Rational(resolution, 1);
      exifData["Exif.Image.YResolution"] = Exiv2::Rational(resolution, 1);
      exifData["Exif.Image.ResolutionUnit"] = uint16_t(2); /* inches */
    }
    else
    {
      static const char *keys[] = {
        "Exif.Image.XResolution",
        "Exif.Image.YResolution",
        "Exif.Image.ResolutionUnit"
      };
      static const guint n_keys = G_N_ELEMENTS(keys);
      dt_remove_exif_keys(exifData, keys, n_keys);
    }

    exifData["Exif.Image.Software"] = darktable_package_string;

    // TODO: find a nice place for the missing metadata (tags, publisher, colorlabels?). Additionally find out
    // how to embed XMP data.
    //       And shall we add a description of the history stack to Exif.Image.ImageHistory?
    if(imgid >= 0)
    {
      /* Delete metadata taken from the original file if it's fileds we manage in dt, too */
      static const char * keys[] = {
        "Exif.Image.Artist",
        "Exif.Image.ImageDescription",
        "Exif.Photo.UserComment",
        "Exif.Image.Copyright",
        "Exif.Image.Rating",
        "Exif.Image.RatingPercent",
        "Exif.GPSInfo.GPSVersionID",
        "Exif.GPSInfo.GPSLongitudeRef",
        "Exif.GPSInfo.GPSLatitudeRef",
        "Exif.GPSInfo.GPSLongitude",
        "Exif.GPSInfo.GPSLatitude",
        "Exif.GPSInfo.GPSAltitudeRef",
        "Exif.GPSInfo.GPSAltitude"
      };
      static const guint n_keys = G_N_ELEMENTS(keys);
      dt_remove_exif_keys(exifData, keys, n_keys);

      GList *res = dt_metadata_get(imgid, "Xmp.dc.creator", NULL);
      if(res != NULL)
      {
        exifData["Exif.Image.Artist"] = (char *)res->data;
        g_list_free_full(res, &g_free);
      }

      res = dt_metadata_get(imgid, "Xmp.dc.description", NULL);
      if(res != NULL)
      {
        char *desc = (char *)res->data;
        if(g_str_is_ascii(desc))
          exifData["Exif.Image.ImageDescription"] = desc;
        else
          exifData["Exif.Photo.UserComment"] = desc;
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
      if(!std::isnan(cimg->elevation))
      {
        exifData["Exif.GPSInfo.GPSVersionID"] = "02 02 00 00";
        exifData["Exif.GPSInfo.GPSAltitudeRef"] = (cimg->elevation < 0) ? "1" : "0";

        long ele_dm = (int)floor(fabs(10.0 * cimg->elevation));
        gchar *ele_str = g_strdup_printf("%ld/10", ele_dm);
        exifData["Exif.GPSInfo.GPSAltitude"] = ele_str;
        g_free(ele_str);
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
    *buf = (uint8_t *)malloc(length+6);
    if (!*buf)
    {
      return 0;
    }
    memcpy(*buf, "Exif\000\000", 6);
    memcpy(*buf + 6, &(blob[0]), length);
    return length + 6;
  }
  catch(Exiv2::AnyError &e)
  {
    // std::cerr.rdbuf(savecerr);
    std::string s(e.what());
    std::cerr << "[exiv2] " << path << ": " << s << std::endl;
    free(*buf);
    *buf = NULL;
    return 0;
  }
}

// encode binary blob into text:
char *dt_exif_xmp_encode(const unsigned char *input, const int len, int *output_len)
{
#define COMPRESS_THRESHOLD 100

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

  return dt_exif_xmp_encode_internal(input, len, output_len, do_compress);

#undef COMPRESS_THRESHOLD
}

char *dt_exif_xmp_encode_internal(const unsigned char *input, const int len, int *output_len, gboolean do_compress)
{
  char *output = NULL;

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
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT id FROM data.tags WHERE name = ?1", -1,
                              &stmt_sel_id, NULL);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "INSERT INTO data.tags (id, name) VALUES (NULL, ?1)",
                              -1, &stmt_ins_tags, NULL);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "INSERT INTO main.tagged_images (tagid, imgid) VALUES (?1, ?2)", -1,
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

  // update used_tags
  dt_tag_update_used_tags();
}

typedef struct history_entry_t
{
  char *operation;
  gboolean enabled;
  int modversion;
  unsigned char *params;
  int params_len;
  char *multi_name;
  int multi_priority;
  int blendop_version;
  unsigned char *blendop_params;
  int blendop_params_len;

  // sanity checking
  gboolean have_operation, have_params, have_modversion;
} history_entry_t;

// used for a hash table that maps mask_id to the mask data
typedef struct mask_entry_t
{
  int mask_id;
  int mask_type;
  char *mask_name;
  int mask_version;
  unsigned char *mask;
  int mask_len;
  int mask_nb;
  unsigned char *mask_src;
  int mask_src_len;
  gboolean already_added;
} mask_entry_t;

static void print_history_entry(history_entry_t *entry) __attribute__((unused));
static void print_history_entry(history_entry_t *entry)
{
  if(!entry || !entry->operation)
  {
    std::cout << "malformed entry" << std::endl;
    return;
  }

  std::cout << entry->operation << std::endl;
  std::cout << "  modversion      :" <<  entry->modversion                                    << std::endl;
  std::cout << "  enabled         :" <<  entry->enabled                                       << std::endl;
  std::cout << "  params          :" << (entry->params ? "<found>" : "<missing>")             << std::endl;
  std::cout << "  multi_name      :" << (entry->multi_name ? entry->multi_name : "<missing>") << std::endl;
  std::cout << "  multi_priority  :" <<  entry->multi_priority                                << std::endl;
  std::cout << "  blendop_version :" <<  entry->blendop_version                               << std::endl;
  std::cout << "  blendop_params  :" << (entry->blendop_params ? "<found>" : "<missing>")     << std::endl;
  std::cout << std::endl;
}

static void free_history_entry(gpointer data)
{
  history_entry_t *entry = (history_entry_t *)data;
  g_free(entry->operation);
  g_free(entry->multi_name);
  free(entry->params);
  free(entry->blendop_params);
  free(entry);
}

// we have to use pugixml as the old format could contain empty rdf:li elements in the multi_name array
// which causes problems when accessing it with libexiv2 :(
// superold is a flag indicating that data is wrapped in <rdf:Bag> instead of <rdf:Seq>.
static GList *read_history_v1(const std::string &xmpPacket, const char *filename, const int superold)
{
  GList *history_entries = NULL;

  pugi::xml_document doc;
#if defined(PUGIXML_VERSION) && PUGIXML_VERSION >= 150
  pugi::xml_parse_result result = doc.load_string(xmpPacket.c_str());
#else
  pugi::xml_parse_result result = doc.load(xmpPacket.c_str());
#endif

  if(!result)
  {
    std::cerr << "XML '" << filename << "' parsed with errors" << std::endl;
    std::cerr << "Error description: " << result.description() << std::endl;
    std::cerr << "Error offset: " << result.offset << std::endl;
    return NULL;
  }

  // get the old elements
  // select_single_node() is deprecated and just kept for old versions shipped in some distributions
#if defined(PUGIXML_VERSION) && PUGIXML_VERSION >= 150
  pugi::xpath_node modversion      = superold ?
    doc.select_node("//darktable:history_modversion/rdf:Bag"):
    doc.select_node("//darktable:history_modversion/rdf:Seq");
  pugi::xpath_node enabled         = superold ?
    doc.select_node("//darktable:history_enabled/rdf:Bag"):
    doc.select_node("//darktable:history_enabled/rdf:Seq");
  pugi::xpath_node operation       = superold ?
    doc.select_node("//darktable:history_operation/rdf:Bag"):
    doc.select_node("//darktable:history_operation/rdf:Seq");
  pugi::xpath_node params          = superold ?
    doc.select_node("//darktable:history_params/rdf:Bag"):
    doc.select_node("//darktable:history_params/rdf:Seq");
  pugi::xpath_node blendop_params  = superold ?
    doc.select_node("//darktable:blendop_params/rdf:Bag"):
    doc.select_node("//darktable:blendop_params/rdf:Seq");
  pugi::xpath_node blendop_version = superold ?
    doc.select_node("//darktable:blendop_version/rdf:Bag"):
    doc.select_node("//darktable:blendop_version/rdf:Seq");
  pugi::xpath_node multi_priority  = superold ?
    doc.select_node("//darktable:multi_priority/rdf:Bag"):
    doc.select_node("//darktable:multi_priority/rdf:Seq");
  pugi::xpath_node multi_name      = superold ?
    doc.select_node("//darktable:multi_name/rdf:Bag"):
    doc.select_node("//darktable:multi_name/rdf:Bag");
#else
  pugi::xpath_node modversion      = superold ?
    doc.select_single_node("//darktable:history_modversion/rdf:Bag"):
    doc.select_single_node("//darktable:history_modversion/rdf:Seq");
  pugi::xpath_node enabled         = superold ?
    doc.select_single_node("//darktable:history_enabled/rdf:Bag"):
    doc.select_single_node("//darktable:history_enabled/rdf:Seq");
  pugi::xpath_node operation       = superold ?
    doc.select_single_node("//darktable:history_operation/rdf:Bag"):
    doc.select_single_node("//darktable:history_operation/rdf:Seq");
  pugi::xpath_node params          = superold ?
    doc.select_single_node("//darktable:history_params/rdf:Bag"):
    doc.select_single_node("//darktable:history_params/rdf:Seq");
  pugi::xpath_node blendop_params  = superold ?
    doc.select_single_node("//darktable:blendop_params/rdf:Bag"):
    doc.select_single_node("//darktable:blendop_params/rdf:Seq");
  pugi::xpath_node blendop_version = superold ?
    doc.select_single_node("//darktable:blendop_version/rdf:Bag"):
    doc.select_single_node("//darktable:blendop_version/rdf:Seq");
  pugi::xpath_node multi_priority  = superold ?
    doc.select_single_node("//darktable:multi_priority/rdf:Bag"):
    doc.select_single_node("//darktable:multi_priority/rdf:Seq");
  pugi::xpath_node multi_name      = superold ?
    doc.select_single_node("//darktable:multi_name/rdf:Bag"):
    doc.select_single_node("//darktable:multi_name/rdf:Bag");
#endif

  // fill the list of history entries. we are iterating over history_operation as we know that it's there.
  // the other iters are taken care of manually.
  auto modversion_iter = modversion.node().children().begin();
  auto enabled_iter = enabled.node().children().begin();
  auto params_iter = params.node().children().begin();
  auto blendop_params_iter = blendop_params.node().children().begin();
  auto blendop_version_iter = blendop_version.node().children().begin();
  auto multi_priority_iter = multi_priority.node().children().begin();
  auto multi_name_iter = multi_name.node().children().begin();

  for(pugi::xml_node operation_iter: operation.node().children())
  {
    history_entry_t *current_entry = (history_entry_t *)calloc(1, sizeof(history_entry_t));
    current_entry->blendop_version = 1; // default version in case it's not specified
    history_entries = g_list_append(history_entries, current_entry);

    current_entry->operation = g_strdup(operation_iter.child_value());

    current_entry->enabled = g_strcmp0(enabled_iter->child_value(), "0") != 0;

    current_entry->modversion = atoi(modversion_iter->child_value());

    current_entry->params = dt_exif_xmp_decode(params_iter->child_value(), strlen(params_iter->child_value()),
                                               &current_entry->params_len);

    if(multi_name && multi_name_iter != multi_name.node().children().end())
    {
      current_entry->multi_name = g_strdup(multi_name_iter->child_value());
      multi_name_iter++;
    }

    if(multi_priority && multi_priority_iter != multi_priority.node().children().end())
    {
      current_entry->multi_priority = atoi(multi_priority_iter->child_value());
      multi_priority_iter++;
    }

    if(blendop_version && blendop_version_iter != blendop_version.node().children().end())
    {
      current_entry->blendop_version = atoi(blendop_version_iter->child_value());
      blendop_version_iter++;
    }

    if(blendop_params && blendop_params_iter != blendop_params.node().children().end())
    {
      current_entry->blendop_params = dt_exif_xmp_decode(blendop_params_iter->child_value(),
                                                         strlen(blendop_params_iter->child_value()),
                                                         &current_entry->blendop_params_len);
      blendop_params_iter++;
    }

    modversion_iter++;
    enabled_iter++;
    params_iter++;
  }

  return history_entries;
}

static GList *read_history_v2(Exiv2::XmpData &xmpData, const char *filename)
{
  GList *history_entries = NULL;
  history_entry_t *current_entry = NULL;

  for(auto history = xmpData.findKey(Exiv2::XmpKey("Xmp.darktable.history")); history != xmpData.end(); history++)
  {
    // TODO: support human readable params via introspection with something like this:
    // XmpText: Xmp.darktable.history[1]/darktable:settings[1]/darktable:name = width
    // XmpText: Xmp.darktable.history[1]/darktable:settings[1]/darktable:value = 23

    char *key = g_strdup(history->key().c_str());
    char *key_iter = key;
    if(g_str_has_prefix(key, "Xmp.darktable.history["))
    {
      key_iter += strlen("Xmp.darktable.history[");
      errno = 0;
      unsigned int n = strtol(key_iter, &key_iter, 10);
      if(errno)
      {
        std::cerr << "error reading history from '" << key << "' (" << filename << ")" << std::endl;
        g_list_free_full(history_entries, free_history_entry);
        g_free(key);
        return NULL;
      }

      // skip everything that isn't part of the actual array
      if(*(key_iter++) != ']')
      {
        std::cerr << "error reading history from '" << key << "' (" << filename << ")" << std::endl;
        g_list_free_full(history_entries, free_history_entry);
        g_free(key);
        return NULL;
      }
      if(*(key_iter++) != '/') goto skip;
      if(*key_iter == '?') key_iter++;

      // make sure we are filling in the details of the correct entry
      unsigned int length = g_list_length(history_entries);
      if(n > length)
      {
        current_entry = (history_entry_t *)calloc(1, sizeof(history_entry_t));
        current_entry->blendop_version = 1; // default version in case it's not specified
        history_entries = g_list_append(history_entries, current_entry);
      }
      else if(n < length)
      {
        // AFAICT this can't happen with regular exiv2 parsed XMP data, but better safe than sorry.
        // it can happen though when constructing things in a unusual order and then passing it to us without
        // serializing it in between
        current_entry = (history_entry_t *)g_list_nth_data(history_entries, n - 1); // XMP starts counting at 1!
      }

      // go on reading things into current_entry
      if(g_str_has_prefix(key_iter, "darktable:operation"))
      {
        current_entry->have_operation = TRUE;
        current_entry->operation = g_strdup(history->value().toString().c_str());
      }
      else if(g_str_has_prefix(key_iter, "darktable:enabled"))
      {
        current_entry->enabled = history->value().toLong() == 1;
      }
      else if(g_str_has_prefix(key_iter, "darktable:modversion"))
      {
        current_entry->have_modversion = TRUE;
        current_entry->modversion = history->value().toLong();
      }
      else if(g_str_has_prefix(key_iter, "darktable:params"))
      {
        current_entry->have_params = TRUE;
        current_entry->params = dt_exif_xmp_decode(history->value().toString().c_str(), history->value().size(),
                                                   &current_entry->params_len);
      }
      else if(g_str_has_prefix(key_iter, "darktable:multi_name"))
      {
        current_entry->multi_name = g_strdup(history->value().toString().c_str());
      }
      else if(g_str_has_prefix(key_iter, "darktable:multi_priority"))
      {
        current_entry->multi_priority = history->value().toLong();
      }
      else if(g_str_has_prefix(key_iter, "darktable:blendop_version"))
      {
        current_entry->blendop_version = history->value().toLong();
      }
      else if(g_str_has_prefix(key_iter, "darktable:blendop_params"))
      {
        current_entry->blendop_params = dt_exif_xmp_decode(history->value().toString().c_str(),
                                                           history->value().size(),
                                                           &current_entry->blendop_params_len);
      }

    }
skip:
    g_free(key);
  }

  // a final sanity check
  for(GList *iter = history_entries; iter; iter = g_list_next(iter))
  {
    history_entry_t *entry = (history_entry_t *)iter->data;
    if(!(entry->have_operation && entry->have_params && entry->have_modversion))
    {
      std::cerr << "[exif] error: reading history from '" << filename << "' failed due to missing tags" << std::endl;
      g_list_free_full(history_entries, free_history_entry);
      history_entries = NULL;
      break;
    }
  }

  return history_entries;
}

void free_mask_entry(gpointer data)
{
  mask_entry_t *entry = (mask_entry_t *)data;
  g_free(entry->mask_name);
  free(entry->mask);
  free(entry->mask_src);
  free(entry);
}

static GHashTable *read_masks(Exiv2::XmpData &xmpData, const char *filename)
{
  GHashTable *mask_entries = g_hash_table_new_full(g_int_hash, g_int_equal, NULL, free_mask_entry);

  // TODO: turn that into something like Xmp.darktable.history!
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
      for(int i = 0; i < cnt; i++)
      {
        mask_entry_t *entry = (mask_entry_t *)calloc(1, sizeof(mask_entry_t));

        entry->mask_id = mask_id->toLong(i);
        entry->mask_type = mask_type->toLong(i);
        std::string mask_name_str = mask_name->toString(i);
        if(mask_name_str.c_str() != NULL)
          entry->mask_name = g_strdup(mask_name_str.c_str());
        else
          entry->mask_name = g_strdup("form");

        entry->mask_version = mask_version->toLong(i);

        std::string mask_str = mask->toString(i);
        const char *mask_c = mask_str.c_str();
        const size_t mask_c_len = strlen(mask_c);
        entry->mask = dt_exif_xmp_decode(mask_c, mask_c_len, &entry->mask_len);

        entry->mask_nb = mask_nb->toLong(i);

        std::string mask_src_str = mask_src->toString(i);
        const char *mask_src_c = mask_src_str.c_str();
        const size_t mask_src_c_len = strlen(mask_src_c);
        entry->mask_src = dt_exif_xmp_decode(mask_src_c, mask_src_c_len, &entry->mask_src_len);

        g_hash_table_insert(mask_entries, &entry->mask_id, (gpointer)entry);
      }
    }
  }

  return mask_entries;
}

static void add_mask_entry_to_db(int imgid, mask_entry_t *entry)
{
  // add the mask entry only once
  if(entry->already_added)
    return;
  entry->already_added = TRUE;

  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(
    dt_database_get(darktable.db),
                              "INSERT INTO main.mask (imgid, formid, form, name, version, points, points_count, source) "
                              "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, entry->mask_id);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, entry->mask_type);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 4, entry->mask_name, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 5, entry->mask_version);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 6, entry->mask, entry->mask_len, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 7, entry->mask_nb);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 8, entry->mask_src, entry->mask_src_len, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

static void add_non_clone_mask_entries_to_db(gpointer key, gpointer value, gpointer user_data)
{
  int imgid = *(int *)user_data;
  mask_entry_t *entry = (mask_entry_t *)value;
  if(!(entry->mask_type & (DT_MASKS_CLONE | DT_MASKS_NON_CLONE))) add_mask_entry_to_db(imgid, entry);
}

static void add_mask_entries_to_db(int imgid, GHashTable *mask_entries, int mask_id)
{
  if(mask_id <= 0) return;

  // look for mask_id in the hash table
  mask_entry_t *entry = (mask_entry_t *)g_hash_table_lookup(mask_entries, &mask_id);

  if(!entry) return;

  // if it's a group: recurse into the children first
  if(entry->mask_type & DT_MASKS_GROUP)
  {
    dt_masks_point_group_t *group = (dt_masks_point_group_t *)entry->mask;
    if((int)(entry->mask_nb * sizeof(dt_masks_point_group_t)) != entry->mask_len)
    {
      fprintf(stderr, "[masks] error loading masks from xmp file, bad binary blob size.\n");
      return;
    }
    for(int i = 0; i < entry->mask_nb; i++)
      add_mask_entries_to_db(imgid, mask_entries, group[i].formid);
  }

  add_mask_entry_to_db(imgid, entry);
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
    std::unique_ptr<Exiv2::Image> image(Exiv2::ImageFactory::open(WIDEN(filename)));
    assert(image.get() != 0);
    read_metadata_threadsafe(image);
    Exiv2::XmpData &xmpData = image->xmpData();

    sqlite3_stmt *stmt;

    Exiv2::XmpData::iterator pos;

    int version = 0;
    if((pos = xmpData.findKey(Exiv2::XmpKey("Xmp.darktable.xmp_version"))) != xmpData.end())
      version = pos->toLong();

    if(!history_only)
    {
      // otherwise we ignore title, description, ... from non-dt xmp files :(
      size_t ns_pos = image->xmpPacket().find("xmlns:darktable=\"http://darktable.sf.net/\"");
      bool is_a_dt_xmp = (ns_pos != std::string::npos);
      dt_exif_read_xmp_data(img, xmpData, is_a_dt_xmp ? version : -1, false);
    }


    // convert legacy flip bits (will not be written anymore, convert to flip history item here):
    if((pos = xmpData.findKey(Exiv2::XmpKey("Xmp.darktable.raw_params"))) != xmpData.end())
    {
      int32_t i = pos->toLong();
      dt_image_raw_parameters_t raw_params = *(dt_image_raw_parameters_t *)&i;
      int32_t user_flip = raw_params.user_flip;
      img->legacy_flip.user_flip = user_flip;
      img->legacy_flip.legacy = 0;
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


    // masks
    // clean all old masks for this image
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "DELETE FROM main.mask WHERE imgid = ?1", -1,
                                &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, img->id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    // read the masks from the file first so we can add them to the db while reading history entries
    GHashTable *mask_entries = read_masks(xmpData, filename);

    // now add all masks that are not used for cloning. keeping them might be useful.
    // TODO: make this configurable? or remove it altogether?
    sqlite3_exec(dt_database_get(darktable.db), "BEGIN TRANSACTION", NULL, NULL, NULL);
    g_hash_table_foreach(mask_entries, add_non_clone_mask_entries_to_db, &img->id);
    sqlite3_exec(dt_database_get(darktable.db), "COMMIT", NULL, NULL, NULL);

    // history
    int num = 0;
    gboolean all_ok = TRUE;
    GList *history_entries = NULL;

    if(version < 2)
    {
      std::string &xmpPacket = image->xmpPacket();
      history_entries = read_history_v1(xmpPacket, filename, 0);
      if(!history_entries) // didn't work? try super old version with rdf:Bag
        history_entries = read_history_v1(xmpPacket, filename, 1);
    }
    else if(version == 2)
      history_entries = read_history_v2(xmpData, filename);
    else
    {
      std::cerr << "error: Xmp schema version " << version << " in " << filename << " not supported" << std::endl;
      g_hash_table_destroy(mask_entries);
      return 1;
    }

    sqlite3_exec(dt_database_get(darktable.db), "BEGIN TRANSACTION", NULL, NULL, NULL);

    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "DELETE FROM main.history WHERE imgid = ?1", -1,
                                &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, img->id);
    if(sqlite3_step(stmt) != SQLITE_DONE)
    {
      fprintf(stderr, "[exif] error deleting history for image %d\n", img->id);
      fprintf(stderr, "[exif]   %s\n", sqlite3_errmsg(dt_database_get(darktable.db)));
      all_ok = FALSE;
      goto end;
    }

    sqlite3_finalize(stmt);

    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "INSERT INTO main.history (imgid, num, module, operation, op_params, enabled, "
                                "blendop_params, blendop_version, multi_priority, multi_name) "
                                "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10)", -1, &stmt, NULL);

    for(GList *iter = history_entries; iter; iter = g_list_next(iter))
    {
      history_entry_t *entry = (history_entry_t *)iter->data;
//       print_history_entry(entry);

      DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, img->id);
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, num);
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, entry->modversion);
      DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 4, entry->operation, -1, SQLITE_TRANSIENT);
      DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 5, entry->params, entry->params_len, SQLITE_TRANSIENT);
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 6, entry->enabled);
      if(entry->blendop_params)
      {
        DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 7, entry->blendop_params, entry->blendop_params_len, SQLITE_TRANSIENT);

        // check what mask entries belong to this iop and add them to the db
        const dt_develop_blend_params_t *blendop_params = (dt_develop_blend_params_t *)entry->blendop_params;
        add_mask_entries_to_db(img->id, mask_entries, blendop_params->mask_id);
      }
      else
      {
        sqlite3_bind_null(stmt, 7);
      }
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 8, entry->blendop_version);
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 9, entry->multi_priority);
      if(entry->multi_name)
      {
        DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 10, entry->multi_name, -1, SQLITE_TRANSIENT);
      }
      else
      {
        DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 10, "", -1, SQLITE_TRANSIENT); // "" instead of " " should be fine now
      }

      if(sqlite3_step(stmt) != SQLITE_DONE)
      {
        fprintf(stderr, "[exif] error adding history entry for image %d\n", img->id);
        fprintf(stderr, "[exif]   %s\n", sqlite3_errmsg(dt_database_get(darktable.db)));
        all_ok = FALSE;
        goto end;
      }
      sqlite3_reset(stmt);
      sqlite3_clear_bindings(stmt);

      num++;
    }
    sqlite3_finalize(stmt);

    // we shouldn't change history_end when no history was read!
    if((pos = xmpData.findKey(Exiv2::XmpKey("Xmp.darktable.history_end"))) != xmpData.end() && num > 0)
    {
      int history_end = MIN(pos->toLong(), num);
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                  "UPDATE main.images SET history_end = ?1 WHERE id = ?2", -1,
                                  &stmt, NULL);
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, history_end);
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, img->id);
      if(sqlite3_step(stmt) != SQLITE_DONE)
      {
        fprintf(stderr, "[exif] error writing history_end for image %d\n", img->id);
        fprintf(stderr, "[exif]   %s\n", sqlite3_errmsg(dt_database_get(darktable.db)));
        all_ok = FALSE;
        goto end;
      }
    }
    else
    {
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                  "UPDATE main.images SET history_end = (SELECT IFNULL(MAX(num) + 1, 0) "
                                  "FROM main.history WHERE imgid = ?1) WHERE id = ?1", -1,
                                  &stmt, NULL);
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, img->id);
      if(sqlite3_step(stmt) != SQLITE_DONE)
      {
        fprintf(stderr, "[exif] error writing history_end for image %d\n", img->id);
        fprintf(stderr, "[exif]   %s\n", sqlite3_errmsg(dt_database_get(darktable.db)));
        all_ok = FALSE;
        goto end;
      }
    }

end:
    sqlite3_finalize(stmt);

    g_list_free_full(history_entries, free_history_entry);
    g_hash_table_destroy(mask_entries);

    if(all_ok)
    {
      sqlite3_exec(dt_database_get(darktable.db), "COMMIT", NULL, NULL, NULL);
    }
    else
    {
      std::cerr << "[exif] error reading history from '" << filename << "'" << std::endl;
      sqlite3_exec(dt_database_get(darktable.db), "ROLLBACK TRANSACTION", NULL, NULL, NULL);
      return 1;
    }

  }
  catch(Exiv2::AnyError &e)
  {
    // actually nobody's interested in that if the file doesn't exist:
    // std::string s(e.what());
    // std::cerr << "[exiv2] " << filename << ": " << s << std::endl;
    return 1;
  }
  return 0;
}

// helper to create an xmp data thing. throws exiv2 exceptions if stuff goes wrong.
static void dt_exif_xmp_read_data(Exiv2::XmpData &xmpData, const int imgid)
{
  const int xmp_version = 2;
  int stars = 1, raw_params = 0, history_end = -1;
  double longitude = NAN, latitude = NAN, altitude = NAN;
  gchar *filename = NULL;
  // get stars and raw params from db
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT filename, flags, raw_parameters, "
                                                             "longitude, latitude, altitude, history_end "
                                                             "FROM main.images WHERE id = ?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    filename = (gchar *)sqlite3_column_text(stmt, 0);
    stars = sqlite3_column_int(stmt, 1);
    raw_params = sqlite3_column_int(stmt, 2);
    if(sqlite3_column_type(stmt, 3) == SQLITE_FLOAT) longitude = sqlite3_column_double(stmt, 3);
    if(sqlite3_column_type(stmt, 4) == SQLITE_FLOAT) latitude = sqlite3_column_double(stmt, 4);
    if(sqlite3_column_type(stmt, 5) == SQLITE_FLOAT) altitude = sqlite3_column_double(stmt, 5);
    history_end = sqlite3_column_int(stmt, 6);
  }

  // We have to erase the old ratings first as exiv2 seems to not change it otherwise.
  Exiv2::XmpData::iterator pos = xmpData.findKey(Exiv2::XmpKey("Xmp.xmp.Rating"));
  if(pos != xmpData.end()) xmpData.erase(pos);
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

    xmpData["Xmp.exif.GPSVersionID"] = "2.2.0.0";
    xmpData["Xmp.exif.GPSLongitude"] = long_str;
    xmpData["Xmp.exif.GPSLatitude"] = lat_str;
    g_free(long_str);
    g_free(lat_str);
    g_free(str);
  }
  if(!std::isnan(altitude))
  {
    xmpData["Xmp.exif.GPSAltitudeRef"] = (altitude < 0) ? "1" : "0";

    long ele_dm = (int)floor(fabs(10.0 * altitude));
    gchar *ele_str = g_strdup_printf("%ld/10", ele_dm);
    xmpData["Xmp.exif.GPSAltitude"] = ele_str;
    g_free(ele_str);
  }
  sqlite3_finalize(stmt);

  // the meta data
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT key, value FROM main.meta_data WHERE id = ?1",
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
  std::unique_ptr<Exiv2::Value> v1(Exiv2::Value::create(Exiv2::xmpSeq)); // or xmpBag or xmpAlt.

  std::unique_ptr<Exiv2::Value> v2(Exiv2::Value::create(Exiv2::xmpSeq)); // or xmpBag or xmpAlt.

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
  std::unique_ptr<Exiv2::Value> v(Exiv2::Value::create(Exiv2::xmpSeq)); // or xmpBag or xmpAlt.

  /* Already initialized v = Exiv2::Value::create(Exiv2::xmpSeq); // or xmpBag or xmpAlt.*/
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT color FROM main.color_labels WHERE imgid=?1",
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
      "SELECT imgid, formid, form, name, version, points, points_count, source FROM main.mask WHERE imgid = ?1",
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
  xmpData.add(Exiv2::XmpKey("Xmp.darktable.history"), &tv);

  DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get(darktable.db),
      "SELECT module, operation, op_params, enabled, blendop_params, "
      "blendop_version, multi_priority, multi_name FROM main.history WHERE imgid = ?1 ORDER BY num",
      -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    int32_t modversion = sqlite3_column_int(stmt, 0);
    const char *operation = (const char *)sqlite3_column_text(stmt, 1);
    int32_t params_len = sqlite3_column_bytes(stmt, 2);
    const void *params_blob = sqlite3_column_blob(stmt, 2);
    int32_t enabled = sqlite3_column_int(stmt, 3);
    const void *blendop_blob = sqlite3_column_blob(stmt, 4);
    int32_t blendop_params_len = sqlite3_column_bytes(stmt, 4);
    int32_t blendop_version = sqlite3_column_int(stmt, 5);
    int32_t multi_priority = sqlite3_column_int(stmt, 6);
    const char *multi_name = (const char *)sqlite3_column_text(stmt, 7);

    if(!operation) continue; // no op is fatal.

    char *params = dt_exif_xmp_encode((const unsigned char *)params_blob, params_len, NULL);

    snprintf(key, sizeof(key), "Xmp.darktable.history[%d]/darktable:operation", num);
    xmpData[key] = operation;
    snprintf(key, sizeof(key), "Xmp.darktable.history[%d]/darktable:enabled", num);
    xmpData[key] = enabled;
    snprintf(key, sizeof(key), "Xmp.darktable.history[%d]/darktable:modversion", num);
    xmpData[key] = modversion;
    snprintf(key, sizeof(key), "Xmp.darktable.history[%d]/darktable:params", num);
    xmpData[key] = params;
    snprintf(key, sizeof(key), "Xmp.darktable.history[%d]/darktable:multi_name", num);
    xmpData[key] = multi_name ? multi_name : "";
    snprintf(key, sizeof(key), "Xmp.darktable.history[%d]/darktable:multi_priority", num);
    xmpData[key] = multi_priority;
    if(blendop_blob)
    {
      // this shouldn't fail in general, but reading is robust enough to allow it,
      // and flipping images from LT will result in this being left out
      char *blendop_params = dt_exif_xmp_encode((const unsigned char *)blendop_blob, blendop_params_len, NULL);
      snprintf(key, sizeof(key), "Xmp.darktable.history[%d]/darktable:blendop_version", num);
      xmpData[key] = blendop_version;
      snprintf(key, sizeof(key), "Xmp.darktable.history[%d]/darktable:blendop_params", num);
      xmpData[key] = blendop_params;
      free(blendop_params);
    }

    free(params);

    num++;
  }

  if(history_end == -1) history_end = num - 1;
  else history_end = MIN(history_end, num - 1); // safeguard for some old buggy libraries
  xmpData["Xmp.darktable.history_end"] = history_end;

  sqlite3_finalize(stmt);
  g_list_free_full(tags, g_free);
  g_list_free_full(hierarchical, g_free);
}

char *dt_exif_xmp_read_string(const int imgid)
{
  try
  {
    char input_filename[PATH_MAX] = { 0 };
    gboolean from_cache = FALSE;
    dt_image_full_path(imgid, input_filename, sizeof(input_filename), &from_cache);

    // first take over the data from the source image
    Exiv2::XmpData xmpData;
    if(g_file_test(input_filename, G_FILE_TEST_EXISTS))
    {
      std::string xmpPacket;

      Exiv2::DataBuf buf = Exiv2::readFile(WIDEN(input_filename));
      xmpPacket.assign(reinterpret_cast<char *>(buf.pData_), buf.size_);
      Exiv2::XmpParser::decode(xmpData, xmpPacket);
      // because XmpSeq or XmpBag are added to the list, we first have
      // to remove these so that we don't end up with a string of duplicates
      dt_remove_known_keys(xmpData);
    }

    // now add whatever we have in the sidecar XMP. this overwrites stuff from the source image
    dt_image_path_append_version(imgid, input_filename, sizeof(input_filename));
    g_strlcat(input_filename, ".xmp", sizeof(input_filename));
    if(g_file_test(input_filename, G_FILE_TEST_EXISTS))
    {
      Exiv2::XmpData sidecarXmpData;
      std::string xmpPacket;

      Exiv2::DataBuf buf = Exiv2::readFile(WIDEN(input_filename));
      xmpPacket.assign(reinterpret_cast<char *>(buf.pData_), buf.size_);
      Exiv2::XmpParser::decode(sidecarXmpData, xmpPacket);

      for(Exiv2::XmpData::const_iterator it = sidecarXmpData.begin(); it != sidecarXmpData.end(); ++it)
        xmpData.add(*it);
    }

    dt_remove_known_keys(xmpData); // is this needed?

    // last but not least attach what we have in DB to the XMP. in theory that should be
    // the same as what we just copied over from the sidecar file, but you never know ...
    dt_exif_xmp_read_data(xmpData, imgid);

    // serialize the xmp data and output the xmp packet
    std::string xmpPacket;
    if(Exiv2::XmpParser::encode(xmpPacket, xmpData,
      Exiv2::XmpParser::useCompactFormat | Exiv2::XmpParser::omitPacketWrapper) != 0)
    {
      throw Exiv2::Error(1, "[xmp_write] failed to serialize xmp data");
    }
    return g_strdup(xmpPacket.c_str());
  }
  catch(Exiv2::AnyError &e)
  {
    std::cerr << "[xmp_read_blob] caught exiv2 exception '" << e << "'\n";
    return NULL;
  }
}

int dt_exif_xmp_attach(const int imgid, const char *filename)
{
  try
  {
    char input_filename[PATH_MAX] = { 0 };
    gboolean from_cache = TRUE;
    dt_image_full_path(imgid, input_filename, sizeof(input_filename), &from_cache);

    std::unique_ptr<Exiv2::Image> img(Exiv2::ImageFactory::open(WIDEN(filename)));
    // unfortunately it seems we have to read the metadata, to not erase the exif (which we just wrote).
    // will make export slightly slower, oh well.
    // img->clearXmpPacket();
    read_metadata_threadsafe(img);

    try
    {
      // initialize XMP and IPTC data with the one from the original file
      std::unique_ptr<Exiv2::Image> input_image(Exiv2::ImageFactory::open(WIDEN(input_filename)));
      if(input_image.get() != 0)
      {
        read_metadata_threadsafe(input_image);
        img->setIptcData(input_image->iptcData());
        img->setXmpData(input_image->xmpData());
      }
    }
    catch(Exiv2::AnyError &e)
    {
      std::cerr << "[xmp_attach] " << input_filename << ": caught exiv2 exception '" << e << "'\n";
    }

    Exiv2::XmpData &xmpData = img->xmpData();

    // now add whatever we have in the sidecar XMP. this overwrites stuff from the source image
    dt_image_path_append_version(imgid, input_filename, sizeof(input_filename));
    g_strlcat(input_filename, ".xmp", sizeof(input_filename));
    if(g_file_test(input_filename, G_FILE_TEST_EXISTS))
    {
      Exiv2::XmpData sidecarXmpData;
      std::string xmpPacket;

      Exiv2::DataBuf buf = Exiv2::readFile(WIDEN(input_filename));
      xmpPacket.assign(reinterpret_cast<char *>(buf.pData_), buf.size_);
      Exiv2::XmpParser::decode(sidecarXmpData, xmpPacket);

      for(Exiv2::XmpData::const_iterator it = sidecarXmpData.begin(); it != sidecarXmpData.end(); ++it)
        xmpData.add(*it);
    }

    dt_remove_known_keys(xmpData); // is this needed?

    {
      // We also want to make sure to not have some tags that might
      // have come in from XMP files created by digikam or similar
      static const char *keys[] = {
        "Xmp.tiff.Orientation"
      };
      static const guint n_keys = G_N_ELEMENTS(keys);
      dt_remove_xmp_keys(xmpData, keys, n_keys);
    }

    // last but not least attach what we have in DB to the XMP. in theory that should be
    // the same as what we just copied over from the sidecar file, but you never know ...
    dt_exif_xmp_read_data(xmpData, imgid);

    img->writeMetadata();
    return 0;
  }
  catch(Exiv2::AnyError &e)
  {
    std::cerr << "[xmp_attach] " << filename << ": caught exiv2 exception '" << e << "'\n";
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
    char *checksum_old = NULL;
    if(g_file_test(filename, G_FILE_TEST_EXISTS))
    {
      // we want to avoid writing the sidecar file if it didn't change to avoid issues when using the same images
      // from different computers. sample use case: images on NAS, several computers using them NOT AT THE SAME TIME and
      // the xmp crawler is used to find changed sidecars.
      FILE *fd = g_fopen(filename, "rb");
      if(fd)
      {
        fseek(fd, 0, SEEK_END);
        size_t end = ftell(fd);
        rewind(fd);
        unsigned char *content = (unsigned char *)malloc(end * sizeof(char));
        if(content)
        {
          if(fread(content, sizeof(unsigned char), end, fd) == end)
            checksum_old = g_compute_checksum_for_data(G_CHECKSUM_MD5, content, end);
          free(content);
        }
        fclose(fd);
      }

      Exiv2::DataBuf buf = Exiv2::readFile(WIDEN(filename));
      xmpPacket.assign(reinterpret_cast<char *>(buf.pData_), buf.size_);
      Exiv2::XmpParser::decode(xmpData, xmpPacket);
      // because XmpSeq or XmpBag are added to the list, we first have
      // to remove these so that we don't end up with a string of duplicates
      dt_remove_known_keys(xmpData);
    }

    // initialize xmp data:
    dt_exif_xmp_read_data(xmpData, imgid);

    // serialize the xmp data and output the xmp packet
    if(Exiv2::XmpParser::encode(xmpPacket, xmpData,
       Exiv2::XmpParser::useCompactFormat | Exiv2::XmpParser::omitPacketWrapper) != 0)
    {
      throw Exiv2::Error(1, "[xmp_write] failed to serialize xmp data");
    }

    // hash the new data and compare it to the old hash (if applicable)
    const char *xml_header = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    gboolean write_sidecar = TRUE;
    if(checksum_old)
    {
      GChecksum *checksum = g_checksum_new(G_CHECKSUM_MD5);
      if(checksum)
      {
        g_checksum_update(checksum, (unsigned char*)xml_header, -1);
        g_checksum_update(checksum, (unsigned char*)xmpPacket.c_str(), -1);
        const char *checksum_new = g_checksum_get_string(checksum);
        write_sidecar = g_strcmp0(checksum_old, checksum_new) != 0;
        g_checksum_free(checksum);
      }
      g_free(checksum_old);
    }

    if(write_sidecar)
    {
      // using std::ofstream isn't possible here -- on Windows it doesn't support Unicode filenames with mingw
      FILE *fout = g_fopen(filename, "wb");
      if(fout)
      {
        fprintf(fout, "%s", xml_header);
        fprintf(fout, "%s", xmpPacket.c_str());
        fclose(fout);
      }
    }

    return 0;
  }
  catch(Exiv2::AnyError &e)
  {
    std::cerr << "[xmp_write] " << filename << ": caught exiv2 exception '" << e << "'\n";
    return -1;
  }
}

dt_colorspaces_color_profile_type_t dt_exif_get_color_space(const uint8_t *data, size_t size)
{
  try
  {
    Exiv2::ExifData::const_iterator pos;
    Exiv2::ExifData exifData;
    Exiv2::ExifParser::decode(exifData, data, size);

    // 0x01   -> sRGB
    // 0x02   -> AdobeRGB
    // 0xffff -> Uncalibrated
    //          + Exif.Iop.InteroperabilityIndex of 'R03' -> AdobeRGB
    //          + Exif.Iop.InteroperabilityIndex of 'R98' -> sRGB
    if((pos = exifData.findKey(Exiv2::ExifKey("Exif.Photo.ColorSpace"))) != exifData.end() && pos->size())
    {
      int colorspace = pos->toLong();
      if(colorspace == 0x01)
        return DT_COLORSPACE_SRGB;
      else if(colorspace == 0x02)
        return DT_COLORSPACE_ADOBERGB;
      else if(colorspace == 0xffff)
      {
        if((pos = exifData.findKey(Exiv2::ExifKey("Exif.Iop.InteroperabilityIndex"))) != exifData.end()
          && pos->size())
        {
          std::string interop_index = pos->toString();
          if(interop_index == "R03")
            return DT_COLORSPACE_ADOBERGB;
          else if(interop_index == "R98")
            return DT_COLORSPACE_SRGB;
        }
      }
    }

    return DT_COLORSPACE_DISPLAY; // nothing embedded
  }
  catch(Exiv2::AnyError &e)
  {
    std::string s(e.what());
    std::cerr << "[exiv2] " << s << std::endl;
    return DT_COLORSPACE_DISPLAY;
  }
}

gboolean dt_exif_get_datetime_taken(const uint8_t *data, size_t size, time_t *datetime_taken)
{
  try
  {
    Exiv2::ExifData::const_iterator pos;
    std::unique_ptr<Exiv2::Image> image(Exiv2::ImageFactory::open(data, size));
    read_metadata_threadsafe(image);
    Exiv2::ExifData &exifData = image->exifData();

    char exif_datetime_taken[20];
    _find_datetime_taken(exifData, pos, exif_datetime_taken);

    if(*exif_datetime_taken)
    {
      struct tm exif_tm= {0};
      if(sscanf(exif_datetime_taken,"%d:%d:%d %d:%d:%d",
        &exif_tm.tm_year,
        &exif_tm.tm_mon,
        &exif_tm.tm_mday,
        &exif_tm.tm_hour,
        &exif_tm.tm_min,
        &exif_tm.tm_sec) == 6)
      {
        exif_tm.tm_year -= 1900;
        exif_tm.tm_mon--;
        *datetime_taken = mktime(&exif_tm);
        return TRUE;
      }
    }

    return FALSE;
  }
  catch(Exiv2::AnyError &e)
  {
    std::string s(e.what());
    std::cerr << "[exiv2] " << s << std::endl;
    return FALSE;
  }
}

static void dt_exif_log_handler(int log_level, const char *message)
{
  if(log_level >= Exiv2::LogMsg::level())
  {
    // We don't seem to need \n in the format string as exiv2 includes it
    // in the messages themselves
    dt_print(DT_DEBUG_CAMERA_SUPPORT, "[exiv2] %s", message);
  }
}

void dt_exif_init()
{
  // preface the exiv2 messages with "[exiv2] "
  Exiv2::LogMsg::setHandler(&dt_exif_log_handler);

  Exiv2::XmpParser::initialize();
  // this has to stay with the old url (namespace already propagated outside dt)
  Exiv2::XmpProperties::registerNs("http://darktable.sf.net/", "darktable");
  Exiv2::XmpProperties::registerNs("http://ns.adobe.com/lightroom/1.0/", "lr");
  Exiv2::XmpProperties::registerNs("http://cipa.jp/exif/1.0/", "exifEX");
}

void dt_exif_cleanup()
{
  Exiv2::XmpParser::terminate();
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
