/*
   This file is part of darktable,
   Copyright (C) 2009-2024 darktable developers.

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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <glib.h>
#include <sqlite3.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

#include <array>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include <exiv2/exiv2.hpp>

#include "control/control.h"

#if defined(_WIN32) && defined(EXV_UNICODE_PATH)
  #define WIDEN(s) pugi::as_wide(s)
#else
  #define WIDEN(s) (s)
#endif

#include <pugixml.hpp>

using namespace std;

#include "common/color_harmony.h"
#include "common/colorlabels.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/dng_opcode.h"
#include "common/image_cache.h"
#include "common/exif.h"
#include "common/metadata.h"
#include "common/ratings.h"
#include "common/tags.h"
#include "common/iop_order.h"
#include "common/variables.h"
#include "common/utility.h"
#include "common/history.h"
#include "common/datetime.h"
#include "control/conf.h"
#include "develop/imageop.h"
#include "develop/blend.h"
#include "develop/masks.h"
#include "imageio/imageio_common.h"
#include "imageio/imageio_jpeg.h"

#define DT_XMP_EXIF_VERSION 5

#if EXIV2_TEST_VERSION(0,28,0)
#define AnyError Error
#define toLong toInt64
#endif

// For these models we can't calculate the correct crop factor or, for some we could, but we
// prefer to take it from here, rather than complicate the calculation code with exceptions
static const struct dt_model_cropfactor dt_cropfactors[] = {
  {.model = "FinePix SL1000", // exiv2 doesn't yet read the tags we need to calculate correctly
   .cropfactor = 5.6f
  },
  {.model = "FinePix E550", // tags contain incorrect data, so formula gives us incorrect result
   .cropfactor = 4.65f
  },
  {.model = "XF10", // resolution was calculated relative to dimensions not typical of Fuji
   .cropfactor = 1.5f
  },
  {.model = "FinePix S1", // resolution was calculated relative to dimensions not typical of Fuji
   .cropfactor = 5.6f
  },
  {.model = "FinePix HS10 HS11", // calculation gives a slightly inaccurate result
   .cropfactor = 5.64f
  },
};

// Persistent list of Exiv2 tags. Set up in dt_init().
static GList *exiv2_taglist = NULL;

static const char *_get_exiv2_type(const int type)
{
  switch(type)
  {
    case 1:
      return "Byte";
    case 2:
      return "Ascii";
    case 3:
      return "Short";
    case 4:
      return "Long";
    case 5:
      return "Rational"; // two LONGs: numerator and denumerator of a fraction
    case 6:
      return "SByte";
    case 7:
      return "Undefined";
    case 8:
      return "SShort";
    case 9:
      return "SLong";
    case 10:
      return "SRational"; // two SLONGs: numerator and denumerator of a fraction.
    case 11:
      return "Float"; //  single precision (4-byte) IEEE format
    case 12:
      return "Double"; // double precision (8-byte) IEEE format.
    case 13:
      return "Ifd";  // 32-bit (4-byte) unsigned integer
    case 16:
      return "LLong"; // 64-bit (8-byte) unsigned integer
    case 17:
      return "LLong"; // 64-bit (8-byte) signed integer
    case 18:
      return "Ifd8"; // 64-bit (8-byte) unsigned integer
    case 0x10000:
      return "String";
    case 0x10001:
      return "Date";
    case 0x10002:
      return "Time";
    case 0x10003:
      return "Comment";
    case 0x10004:
      return "Directory";
    case 0x10005:
      return "XmpText";
    case 0x10006:
      return "XmpAlt";
    case 0x10007:
      return "XmpBag";
    case 0x10008:
      return "XmpSeq";
    case 0x10009:
      return "LangAlt";
    case 0x1fffe:
      return "Invalid";
    case 0x1ffff:
      return "LastType";
    default:
      return "Invalid";
  }
}

static void _get_xmp_tags(const char *prefix,
                          GList **taglist)
{
  const Exiv2::XmpPropertyInfo *pl = Exiv2::XmpProperties::propertyList(prefix);
  if(pl)
  {
    for(int i = 0; pl[i].name_ != 0; ++i)
    {
      char *tag = g_strdup_printf("Xmp.%s.%s,%s",
                                  prefix, pl[i].name_, _get_exiv2_type(pl[i].typeId_));
      *taglist = g_list_prepend(*taglist, tag);
    }
  }
}

/*
  The correction matrices are taken from http://www.brucelindbloom.com - chromatic Adaption.
  using Bradford method: found Illuminant -> D65

  ISOStudioTungsten, DaylightFluorescent, DayWhiteFluorescent, CoolWhiteFluorescent,
  WhiteFluorescent and WarmWhiteFluorescent were calculated with xy coord from DNG SDK as reference -> XYZ -> D65

  xy coord and temperatures are the same as in DNG SDK and habe not been changed in dt.
*/

static const struct dt_dng_illuminant_data_t illuminant_data[DT_LS_Other + 1] =
{
  { 0,    "Unknown Illuminant",   {0.0, 0.0},           {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0}},
  { 5500, "Daylight",             {0.3324, 0.3474},     {0.9726856, -0.0135482, 0.0361731, -0.0167463, 1.0049102, 0.0120598, 0.0070026, -0.0116372, 1.1869548}},
  { 4230, "Fluorescent",          {0.37208, 0.37529},   {0.9212269, -0.0449128, 0.1211620, -0.0553723, 1.0277243, 0.0403563, 0.0235086, -0.0391019, 1.6390644}},
  { 2850, "Tungsten",             {0.4476, 0.4074},     {0.8446965, -0.1179225, 0.3948108, -0.1366303, 1.1041226, 0.1291718, 0.0798489, -0.1348999, 3.1924009}},
  { 5500, "Flash",                {0.3324, 0.3474},     {0.9726856, -0.0135482, 0.0361731, -0.0167463, 1.0049102, 0.0120598, 0.0070026, -0.0116372, 1.1869548}},
  { 0,    "Unknown Illuminant5",  {0.0, 0.0},           {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0}},
  { 0,    "Unknown Illuminant6",  {0.0, 0.0},           {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0}},
  { 0,    "Unknown Illuminant7",  {0.0, 0.0},           {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0}},
  { 0,    "Unknown Illuminant8",  {0.0, 0.0},           {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0}},
  { 5500, "FineWeather",          {0.3324, 0.3474},     {0.9726856, -0.0135482, 0.0361731, -0.0167463, 1.0049102, 0.0120598, 0.0070026, -0.0116372, 1.1869548}},
  { 6500, "CloudyWeather",        {0.3127, 0.3290},     {1.0, 0.0, 0.0,   0.0, 1.0, 0.0,   0.0, 0.0, 1.0}},
  { 7500, "Shade",                {0.2990, 0.3149},     {1.0206905, 0.0091588, -0.0228796, 0.0115005, 0.9984917, -0.0076762, -0.0043619, 0.0072053, 0.8853432}},
  { 6430, "DaylightFluorescent",  {0.31310, 0.33727},   {1.0096114, 0.0061501, 0.0068113, 0.0102539, 0.9888663, 0.0015575, 0.0023119, -0.0044823, 1.0525915}},
  { 5000, "DayWhiteFluorescent",  {0.34588, 0.35875},   {0.9554129, -0.0231280, 0.0637169, -0.0283629, 1.0099053, 0.0211824, 0.0124188, -0.0206922, 1.3330592}},
  { 4150, "CoolWhiteFluorescent", {0.37417, 0.37281},   {0.9147843, -0.0492842, 0.1202810, -0.0622085, 1.034984, 0.0404480, 0.0228014, -0.0375807, 1.6259804}},
  { 3450, "WhiteFluorescent",     {0.40910, 0.39430},   {0.8805388, -0.0774890, 0.2293784, -0.0932136, 1.0589267, 0.0757827, 0.0453660, -0.0760107, 2.2417979}},
  { 2940, "WarmWhiteFluorescent", {0.44018, 0.40329},   {0.8488316, -0.1107439, 0.3471428, -0.1310107, 1.0986874, 0.1141548, 0.0694025, -0.1167541, 2.9109462}},
  { 2850, "StandardLightA",       {0.4476, 0.4074},     {0.8446965, -0.1179225, 0.3948108, -0.1366303, 1.1041226, 0.1291718, 0.0798489, -0.1348999, 3.1924009}},
  { 4871, "StandardLightB",       {0.348483, 0.351747}, {0.9415037, -0.0321240, 0.0584672, -0.0428238, 1.0250998, 0.0203309, 0.0101511, -0.0161170, 1.2847354}},
  { 6774, "StandardLightC",       {0.310061, 0.316150}, {0.9904476, -0.0071683, -0.0116156, -0.0123712, 1.0155950, -0.0029282, -0.0035635, 0.0067697, 0.9181569}},
  { 5500, "D55",                  {0.3324, 0.3474},     {0.9726856, -0.0135482, 0.0361731, -0.0167463, 1.0049102, 0.0120598, 0.0070026, -0.0116372, 1.1869548}},
  { 6500, "D65",                  {0.3127, 0.3290},     {1.0, 0.0, 0.0,   0.0, 1.0, 0.0,   0.0, 0.0, 1.0}},
  { 7500, "D75",                  {0.2990, 0.3149},     {1.0206905, 0.0091588, -0.0228796, 0.0115005, 0.9984917, -0.0076762, -0.0043619, 0.0072053, 0.8853432}},
  { 5000, "D50",                  {0.3457, 0.3585},     {0.9555766, -0.0230393, 0.0631636, -0.0282895, 1.0099416, 0.0210077, 0.0122982, -0.0204830, 1.3299098}},
  { 3200, "ISOStudioTungsten",    {0.40910, 0.39430},   {0.8663030, -0.0913083, 0.2771784, -0.1090504, 1.0746895, 0.0913841, 0.0550856, -0.0924636, 2.5119387}},
  { 6500, "Other illuminant",     {0.3127, 0.3290},     {1.0, 0.0, 0.0,   0.0, 1.0, 0.0,   0.0, 0.0, 1.0}}
};


static inline const char *_illu_to_str(const dt_dng_illuminant_t illu)
{
  return illuminant_data[illu].name;
}

static inline int _illu_to_temp(const dt_dng_illuminant_t illu)
{
  return illuminant_data[illu].temp;
}

// we append i immediately after the title only if it's in the range 1-3
static inline void _print_matrix_data(const char *title, const int i, float *M)
{
  dt_print(DT_DEBUG_IMAGEIO, "%s%s%s%s = %.4f %.4f %.4f | %.4f %.4f %.4f | %.4f %.4f %.4f",
    title,
    i == 1 ? "1" : "",
    i == 2 ? "2" : "",
    i == 3 ? "3" : "",
    M[0], M[1], M[2], M[3], M[4], M[5], M[6], M[7], M[8]);
}

void dt_exif_set_exiv2_taglist()
{
  if(exiv2_taglist)
    return;

  try
  {
    const Exiv2::GroupInfo *groupList = Exiv2::ExifTags::groupList();
    if(groupList)
    {
      while(groupList->tagList_)
      {
        const std::string groupName(groupList->groupName_);
        if(groupName.substr(0, 3) != "Sub" &&
            groupName != "Image2" &&
            groupName != "Image3" &&
            groupName != "Thumbnail"
            )
        {
          const Exiv2::TagInfo *tagInfo = groupList->tagList_();
          while(tagInfo->tag_ != 0xFFFF)
          {
            char *tag = g_strdup_printf("Exif.%s.%s,%s",
                                        groupList->groupName_,
                                        tagInfo->name_,
                                        _get_exiv2_type(tagInfo->typeId_));
            exiv2_taglist = g_list_prepend(exiv2_taglist, tag);
            tagInfo++;
          }
        }
        groupList++;
      }
    }

    const Exiv2::DataSet *iptcEnvelopeList = Exiv2::IptcDataSets::envelopeRecordList();
    while(iptcEnvelopeList->number_ != 0xFFFF)
    {
      char *tag = g_strdup_printf("Iptc.Envelope.%s,%s%s",
                                  iptcEnvelopeList->name_,
                                  _get_exiv2_type(iptcEnvelopeList->type_),
                                  iptcEnvelopeList->repeatable_ ? "-R" : "");
      exiv2_taglist = g_list_prepend(exiv2_taglist, tag);
      iptcEnvelopeList++;
    }

    const Exiv2::DataSet *iptcApplication2List =
      Exiv2::IptcDataSets::application2RecordList();
    while(iptcApplication2List->number_ != 0xFFFF)
    {
      char *tag = g_strdup_printf("Iptc.Application2.%s,%s%s",
                                  iptcApplication2List->name_,
                                  _get_exiv2_type(iptcApplication2List->type_),
                                  iptcApplication2List->repeatable_ ? "-R" : "");
      exiv2_taglist = g_list_prepend(exiv2_taglist, tag);
      iptcApplication2List++;
    }

    _get_xmp_tags("dc", &exiv2_taglist);
    _get_xmp_tags("xmp", &exiv2_taglist);
    _get_xmp_tags("xmpRights", &exiv2_taglist);
    _get_xmp_tags("xmpMM", &exiv2_taglist);
    _get_xmp_tags("xmpBJ", &exiv2_taglist);
    _get_xmp_tags("xmpTPg", &exiv2_taglist);
    _get_xmp_tags("xmpDM", &exiv2_taglist);
    _get_xmp_tags("pdf", &exiv2_taglist);
    _get_xmp_tags("photoshop", &exiv2_taglist);
    _get_xmp_tags("crs", &exiv2_taglist);
    _get_xmp_tags("tiff", &exiv2_taglist);
    _get_xmp_tags("exif", &exiv2_taglist);
    _get_xmp_tags("exifEX", &exiv2_taglist);
    _get_xmp_tags("aux", &exiv2_taglist);
    _get_xmp_tags("iptc", &exiv2_taglist);
    _get_xmp_tags("iptcExt", &exiv2_taglist);
    _get_xmp_tags("plus", &exiv2_taglist);
    _get_xmp_tags("mwg-rs", &exiv2_taglist);
    _get_xmp_tags("mwg-kw", &exiv2_taglist);
    _get_xmp_tags("dwc", &exiv2_taglist);
    _get_xmp_tags("dcterms", &exiv2_taglist);
    _get_xmp_tags("digiKam", &exiv2_taglist);
    _get_xmp_tags("kipi", &exiv2_taglist);
    _get_xmp_tags("GPano", &exiv2_taglist);
    _get_xmp_tags("lr", &exiv2_taglist);
    _get_xmp_tags("MP", &exiv2_taglist);
    _get_xmp_tags("MPRI", &exiv2_taglist);
    _get_xmp_tags("MPReg", &exiv2_taglist);
    _get_xmp_tags("acdsee", &exiv2_taglist);
    _get_xmp_tags("mediapro", &exiv2_taglist);
    _get_xmp_tags("expressionmedia", &exiv2_taglist);
    _get_xmp_tags("MicrosoftPhoto", &exiv2_taglist);
  }
  catch (Exiv2::AnyError& e)
  {
    const char *errstring = e.what();
    dt_print(DT_DEBUG_IMAGEIO, "[exiv2 taglist] %s", errstring);
  }
}

const GList* dt_exif_get_exiv2_taglist()
{
  if(!exiv2_taglist)
    dt_exif_set_exiv2_taglist();
  return exiv2_taglist;
}

static const char *_exif_get_exiv2_tag_type(const char *tagname)
{
  if(!tagname) return NULL;
  for(GList *tag = exiv2_taglist; tag; tag = g_list_next(tag))
  {
    char *t = (char *)tag->data;
    if(g_str_has_prefix(t, tagname) && t[strlen(tagname)] == ',')
    {
      if(t)
      {
        t += strlen(tagname) + 1;
        return t;
      }
    }
  }
  return NULL;
}

// Exiv2's readMetadata is not thread safe in 0.26. So we lock it.
// Since readMetadata might throw an exception we wrap it into
// some C++ magic to make sure we unlock in all cases. Well, actually
// not magic but basic RAII.
// FIXME: Check again once we rely on 0.27.
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

static void _read_xmp_timestamps(Exiv2::XmpData &xmpData,
                                 dt_image_t *img,
                                 const int xmp_version);

static void _read_xmp_harmony_guide(Exiv2::XmpData &xmpData,
                                    dt_image_t *img,
                                    const int xmp_version);

// This array should contain all XmpBag and XmpSeq keys used by dt
const char *dt_xmp_keys[]
    = { "Xmp.dc.subject",                     "Xmp.lr.hierarchicalSubject",
        "Xmp.darktable.colorlabels",          "Xmp.darktable.history",
        "Xmp.darktable.history_modversion",   "Xmp.darktable.history_enabled",
        "Xmp.darktable.history_end",          "Xmp.darktable.iop_order_version",
        "Xmp.darktable.iop_order_list",       "Xmp.darktable.history_operation",
        "Xmp.darktable.history_params",       "Xmp.darktable.blendop_params",
        "Xmp.darktable.blendop_version",      "Xmp.darktable.multi_priority",
        "Xmp.darktable.multi_name",           "Xmp.darktable.multi_name_hand_edited",
        "Xmp.darktable.iop_order",            "Xmp.darktable.xmp_version",
        "Xmp.darktable.raw_params",           "Xmp.darktable.auto_presets_applied",
        "Xmp.darktable.mask_id",              "Xmp.darktable.mask_type",
        "Xmp.darktable.mask_name",            "Xmp.darktable.masks_history",
        "Xmp.darktable.mask_num",             "Xmp.darktable.mask_points",
        "Xmp.darktable.mask_version",         "Xmp.darktable.mask",
        "Xmp.darktable.mask_nb",              "Xmp.darktable.mask_src",
        "Xmp.darktable.history_basic_hash",   "Xmp.darktable.history_auto_hash",
        "Xmp.darktable.history_current_hash", "Xmp.darktable.import_timestamp",
        "Xmp.darktable.change_timestamp",     "Xmp.darktable.export_timestamp",
        "Xmp.darktable.print_timestamp",      "Xmp.darktable.version_name",
        "Xmp.darktable.harmony_guide_type",   "Xmp.darktable.harmony_guide_rotation",
        "Xmp.darktable.harmony_guide_width",
        "Xmp.acdsee.notes",                   "Xmp.dc.creator",
        "Xmp.dc.publisher",                   "Xmp.dc.title",
        "Xmp.dc.description",                 "Xmp.dc.rights",
        "Xmp.dc.format",                      "Xmp.xmpMM.DerivedFrom",
        "Xmp.xmpMM.PreservedFileName" };

// The number of XmpBag XmpSeq keys that dt uses
static const guint dt_xmp_keys_n = G_N_ELEMENTS(dt_xmp_keys);

// Inspired by ufraw_exiv2.cc:
static void _strlcpy_to_utf8(char *dest,
                             const size_t dest_max,
                             Exiv2::ExifData::const_iterator &pos,
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
  g_strstrip(dest);
}

// If a numerical Exif value n has no string representation in Exiv2
// then the returned string is a useless "(n)" which can be discarded
static void _exif_value_str(const int value,
                            char *dest,
                            const size_t dest_max,
                            Exiv2::ExifData::const_iterator &pos,
                            Exiv2::ExifData &exifData)
{
  _strlcpy_to_utf8(dest, dest_max, pos, exifData);
  gchar *str_value = g_strdup_printf("(%d)", value);
  if(g_strcmp0(dest, str_value) == 0)
  {
    dest[0] = '\0';
  }
  g_free(str_value);
}


// Function to remove known dt keys and subtrees from xmpdata, so not
// to append them twice. This should work because dt first reads all
// known keys.
static void _remove_known_keys(Exiv2::XmpData &xmp)
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

      // Stop iterating once the key no longer matches what we are
      // trying to delete. This assumes sorted input.
      if(!(g_str_has_prefix(ckey, dt_xmp_keys[i])
           && (ckey[len] == '[' || ckey[len] == '\0')))
        break;
      pos = xmp.erase(pos);
    }
  }
}

static void _remove_exif_keys(Exiv2::ExifData &exif,
                              const char *keys[],
                              const unsigned int n_keys)
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
      // The only exception we may get is "invalid" tag, which is not
      // important enough to either stop the function, or even display
      // a message (it's probably the tag that is not implemented in
      // the Exiv2 version used).
    }
  }
}

static void _remove_xmp_keys(Exiv2::XmpData &xmp,
                             const char *keys[],
                             const unsigned int n_keys)
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
      // The only exception we may get is "invalid" tag, which is not
      // important enough to either stop the function, or even display
      // a message (it's probably the tag that is not implemented in
      // the Exiv2 version used).
    }
  }
}

static bool _exif_read_xmp_tag(Exiv2::XmpData &xmpData,
                               Exiv2::XmpData::iterator *pos,
                               string key)
{
  try
  {
    return (*pos = xmpData.findKey(Exiv2::XmpKey(key))) != xmpData.end()
      && (*pos)->size();
  }
  catch(Exiv2::AnyError &e)
  {
    const char *errstring = e.what();
    dt_print(DT_DEBUG_IMAGEIO, "[exiv2 read_xmp_tag] %s", errstring);
    return false;
  }
}
#define FIND_XMP_TAG(key) _exif_read_xmp_tag(xmpData, &pos, key)


// FIXME: according to
// http://www.exiv2.org/doc/classExiv2_1_1Metadatum.html#63c2b87249ba96679c29e01218169124
// there is no need to pass xmpData
// version = -1 --> version ignored
static bool _exif_decode_xmp_data(dt_image_t *img,
                                  Exiv2::XmpData &xmpData,
                                  const int version,
                                  const bool exif_read)
{
  // As this can be called several times during the image lifetime, clean up first
  GList *imgs = NULL;
  imgs = g_list_prepend(imgs, GINT_TO_POINTER(img->id));
  try
  {
    Exiv2::XmpData::iterator pos;

    // Older darktable version did not write this data correctly: the
    // reasoning behind strdup'ing all the strings before passing it
    // to sqlite3 is, that they are somehow corrupt after the call to
    // sqlite3_prepare_v2() -- don't ask me why for they don't get
    // passed to that function.
    if(version == -1 || version > 0)
    {
      if(!exif_read) dt_metadata_clear(imgs, FALSE);
      for(unsigned int i = 0; i < DT_METADATA_NUMBER; i++)
      {
        const gchar *key = dt_metadata_get_key(i);
        if(FIND_XMP_TAG(key))
        {
          char *value = strdup(pos->toString().c_str());
          char *adr = value;

          // Skip any lang="" or charset=xxx
          while(!strncmp(value, "lang=", 5) || !strncmp(value, "charset=", 8))
          {
            while(*value != ' ' && *value) value++;
            while(*value == ' ') value++;
          }
          dt_metadata_set_import(img->id, key, value);
          free(adr);
        }
      }
    }

    // If XMP file(s) are found, read the rating from here.
    if(FIND_XMP_TAG("Xmp.darktable.xmp_version")
       || !dt_conf_get_bool("ui_last/ignore_exif_rating"))
    {
      if(FIND_XMP_TAG("Xmp.xmp.Rating"))
      {
        const int stars = pos->toLong();
        dt_image_set_xmp_rating(img, stars);
      }
      else
        dt_image_set_xmp_rating(img, -2);
    }

    if(!exif_read) dt_colorlabels_remove_all_labels(img->id);
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
    // If Xmp.xmp.Label not managed from an external app use dt colors
    else if(FIND_XMP_TAG("Xmp.darktable.colorlabels"))
    {
      // color labels
      const int cnt = pos->count();
      for(int i = 0; i < cnt; i++)
      {
        dt_colorlabels_set_label(img->id, pos->toLong(i));
      }
    }

    if((dt_image_get_xmp_mode() != DT_WRITE_XMP_NEVER) ||
       dt_conf_get_bool("ui_last/import_last_tags_imported"))
    {
      GList *tags = NULL;

      // Preserve dt tags which are not saved in xmp file
      if(!exif_read) dt_tag_set_tags(tags, imgs, TRUE, TRUE, FALSE);
      if(FIND_XMP_TAG("Xmp.lr.hierarchicalSubject"))
        _exif_import_tags(img, pos);
      else if(FIND_XMP_TAG("Xmp.dc.subject"))
        _exif_import_tags(img, pos);
    }

    // Read GPS location
    if(FIND_XMP_TAG("Xmp.exif.GPSLatitude"))
    {
      img->geoloc.latitude = dt_util_gps_string_to_number(pos->toString().c_str());
    }

    if(FIND_XMP_TAG("Xmp.exif.GPSLongitude"))
    {
      img->geoloc.longitude = dt_util_gps_string_to_number(pos->toString().c_str());
    }

    if(FIND_XMP_TAG("Xmp.exif.GPSAltitude"))
    {
      Exiv2::XmpData::const_iterator ref =
        xmpData.findKey(Exiv2::XmpKey("Xmp.exif.GPSAltitudeRef"));
      if(ref != xmpData.end() && ref->size())
      {
        std::string sign_str = ref->toString();
        const char *sign = sign_str.c_str();
        double elevation = 0.0;
        if(dt_util_gps_elevation_to_number(pos->toRational(0).first,
                                           pos->toRational(0).second,
                                           sign[0], &elevation))
          img->geoloc.elevation = elevation;
      }
    }

    // Read lens type from Xmp.exifEX.LensModel
    if(FIND_XMP_TAG("Xmp.exifEX.LensModel"))
    {
      // Lens model
      char *lens = strdup(pos->toString().c_str());
      char *adr =  lens;
      if(strncmp(lens, "lang=", 5) == 0)
      {
        lens = strchr(lens, ' ');
        if(lens != NULL) lens++;
      }
      // No need to do any Unicode<->locale conversion, the field is specified as ASCII
      g_strlcpy(img->exif_lens, lens, sizeof(img->exif_lens));
      free(adr);
    }

    // Read timestamp from Xmp.exif.DateTimeOriginal
    if(FIND_XMP_TAG("Xmp.exif.DateTimeOriginal")
       || FIND_XMP_TAG("Xmp.photoshop.DateCreated"))
    {
      char *datetime = strdup(pos->toString().c_str());
      dt_datetime_exif_to_img(img, datetime);
      free(datetime);
    }

    if(imgs) g_list_free(imgs);
    imgs = NULL;
    return true;
  }
  catch(Exiv2::AnyError &e)
  {
    if(imgs) g_list_free(imgs);
    imgs = NULL;
    const char *errstring = e.what();
    dt_print(DT_DEBUG_IMAGEIO,
             "[exiv2 _exif_decode_xmp_data] %s: %s",
             img->filename,
             errstring);
    return false;
  }
}

static bool _exif_read_iptc_tag(Exiv2::IptcData &iptcData,
                                Exiv2::IptcData::const_iterator *pos,
                                string key)
{
  try
  {
    return (*pos = iptcData.findKey(Exiv2::IptcKey(key)))
      != iptcData.end() && (*pos)->size();
  }
  catch(Exiv2::AnyError &e)
  {
    const char *errstring = e.what();
    dt_print(DT_DEBUG_IMAGEIO, "[exiv2 read_iptc_tag] %s", errstring);
    return false;
  }
}
#define FIND_IPTC_TAG(key) _exif_read_iptc_tag(iptcData, &pos, key)


// FIXME: according to
// http://www.exiv2.org/doc/classExiv2_1_1Metadatum.html#63c2b87249ba96679c29e01218169124
// there is no need to pass iptcData
static bool _exif_decode_iptc_data(dt_image_t *img,
                                   Exiv2::IptcData &iptcData)
{
  try
  {
    Exiv2::IptcData::const_iterator pos;
    iptcData.sortByKey(); // this helps to quickly find all Iptc.Application2.Keywords

    if((pos = iptcData.findKey(Exiv2::IptcKey("Iptc.Application2.Keywords")))
       != iptcData.end())
    {
      while(pos != iptcData.end())
      {
        std::string key = pos->key();
        if(g_strcmp0(key.c_str(), "Iptc.Application2.Keywords")) break;
        std::string str = pos->print();
        char *tag = dt_util_foo_to_utf8(str.c_str());
        guint tagid = 0;
        dt_tag_new(tag, &tagid);
        dt_tag_attach(tagid, img->id, FALSE, FALSE);
        g_free(tag);
        ++pos;
      }
      DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_TAG_CHANGED);
    }
    if(FIND_IPTC_TAG("Iptc.Application2.Caption"))
    {
      std::string str = pos->print(/*&iptcData*/);
      dt_metadata_set_import(img->id, "Xmp.dc.description", str.c_str());
    }
    if(FIND_IPTC_TAG("Iptc.Application2.Copyright"))
    {
      std::string str = pos->print(/*&iptcData*/);
      dt_metadata_set_import(img->id, "Xmp.dc.rights", str.c_str());
    }
    if(FIND_IPTC_TAG("Iptc.Application2.Byline"))
    {
      std::string str = pos->print(/*&iptcData*/);
      dt_metadata_set_import(img->id, "Xmp.dc.creator", str.c_str());
    }
    else if(FIND_IPTC_TAG("Iptc.Application2.Writer"))
    {
      std::string str = pos->print(/*&iptcData*/);
      dt_metadata_set_import(img->id, "Xmp.dc.creator", str.c_str());
    }
    else if(FIND_IPTC_TAG("Iptc.Application2.Contact"))
    {
      std::string str = pos->print(/*&iptcData*/);
      dt_metadata_set_import(img->id, "Xmp.dc.creator", str.c_str());
    }
    if(FIND_IPTC_TAG("Iptc.Application2.DateCreated"))
    {
      // Exiv2 already converts IPTC date and time into ISO 8601 format
      GString *datetime = g_string_new(pos->toString().c_str());

      datetime = g_string_append(datetime, "T");
      if(FIND_IPTC_TAG("Iptc.Application2.TimeCreated"))
      {
        gchar *time = g_strdup(pos->toString().c_str());
        datetime = g_string_append(datetime, time);
        g_free(time);
      }
      else
        datetime = g_string_append(datetime, "00:00:00");

      dt_datetime_exif_to_img(img, datetime->str);
      g_string_free(datetime, TRUE);
    }

    return true;
  }
  catch(Exiv2::AnyError &e)
  {
    const char *errstring = e.what();
    dt_print(DT_DEBUG_IMAGEIO,
             "[exiv2 _exif_decode_iptc_data] %s: %s",
             img->filename,
             errstring);
    return false;
  }
}

static bool _exif_read_exif_tag(Exiv2::ExifData &exifData,
                                Exiv2::ExifData::const_iterator *pos,
                                string key)
{
  try
  {
    return (*pos = exifData.findKey(Exiv2::ExifKey(key)))
      != exifData.end() && (*pos)->size();
  }
  catch(Exiv2::AnyError &e)
  {
    const char *errstring = e.what();
    dt_print(DT_DEBUG_IMAGEIO, "[exiv2 read_exif_tag] %s", errstring);
    return false;
  }
}
#define FIND_EXIF_TAG(key) _exif_read_exif_tag(exifData, &pos, key)

// Support DefaultUserCrop, what is the safe Exif tag?
// DefaultUserCrop is known by name only from Exiv2 0.27.4.
// Magic-nr taken from DNG spec, the spec also says it has 4 floats (top,left,bottom,right).
// We only take them if a) we find a value != the default *and* b) data are plausible.
static bool _check_usercrop(Exiv2::ExifData &exifData,
                            dt_image_t *img)
{
  Exiv2::ExifData::const_iterator pos =
    exifData.findKey(Exiv2::ExifKey("Exif.SubImage1.0xc7b5"));

  // DNGs without an embedded preview have the raw image tags under
  // Exif.Image instead of Exif.SubImage1.
  if(pos == exifData.end())
    pos = exifData.findKey(Exiv2::ExifKey("Exif.Image.0xc7b5"));
  if(pos != exifData.end() && pos->count() == 4 && pos->size())
  {
    dt_boundingbox_t crop;
    for(int i = 0; i < 4; i++) crop[i] = pos->toFloat(i);
    if(((crop[0] > 0)
        ||(crop[1] > 0)
        ||(crop[2] < 1)
        ||(crop[3] < 1))
       && (crop[2] - crop[0] > 0.05f)
       && (crop[3] - crop[1] > 0.05f))
    {
      for(int i=0; i<4; i++) img->usercrop[i] = crop[i];
      return TRUE;
    }
  }
  return FALSE;
}

static void _check_linear_response_limit(Exiv2::ExifData &exifData,
                                         dt_image_t *img)
{
  bool found_one = false;

  // currently this only reads the dng tag, could also be used for other raws

  Exiv2::ExifData::const_iterator linr =
    exifData.findKey(Exiv2::ExifKey("Exif.Image.LinearResponseLimit"));
  if(linr != exifData.end() && linr->count() == 1)
  {
    img->linear_response_limit = linr->toFloat();
    found_one = true;
  }

  if(found_one)
    dt_print(DT_DEBUG_IMAGEIO, "[exif] `%s` has LinearResponseLimit %.4f",
      img->filename, img->linear_response_limit);
}

static gboolean _check_dng_opcodes(Exiv2::ExifData &exifData,
                                   dt_image_t *img)
{
  gboolean has_opcodes = FALSE;

  Exiv2::ExifData::const_iterator pos =
    exifData.findKey(Exiv2::ExifKey("Exif.SubImage1.OpcodeList2"));

  // DNGs without an embedded preview have the opcodes under
  // Exif.Image instead of Exif.SubImage1.
  if(pos == exifData.end())
    pos = exifData.findKey(Exiv2::ExifKey("Exif.Image.OpcodeList2"));
  if(pos != exifData.end())
  {
    uint8_t *data = (uint8_t *)g_try_malloc(pos->size());
    if(data)
    {
      pos->copy(data, Exiv2::invalidByteOrder);
      dt_dng_opcode_process_opcode_list_2(data, pos->size(), img);
      g_free(data);
      has_opcodes = TRUE;
    }
  }

  Exiv2::ExifData::const_iterator posb =
    exifData.findKey(Exiv2::ExifKey("Exif.SubImage1.OpcodeList3"));

  // DNGs without an embedded preview have the opcodes under
  // Exif.Image instead of Exif.SubImage1.
  if(posb == exifData.end())
    posb = exifData.findKey(Exiv2::ExifKey("Exif.Image.OpcodeList3"));
  if(posb != exifData.end())
  {
    uint8_t *data = (uint8_t *)g_try_malloc(posb->size());
    if(data)
    {
      posb->copy(data, Exiv2::invalidByteOrder);
      dt_dng_opcode_process_opcode_list_3(data, posb->size(), img);
      g_free(data);
      has_opcodes = TRUE;
    }
  }
  return has_opcodes;
}

static gboolean _check_lens_correction_data(Exiv2::ExifData &exifData,
                                            dt_image_t *img)
{
  Exiv2::ExifData::const_iterator pos, posd, posc, posv;

  /*
   * Sony lens correction data
   */
  if(Exiv2::versionNumber() >= EXIV2_MAKE_VERSION(0, 27, 4)
    && _exif_read_exif_tag(exifData, &posd, "Exif.SubImage1.DistortionCorrParams")
    && _exif_read_exif_tag(exifData, &posc, "Exif.SubImage1.ChromaticAberrationCorrParams")
    && _exif_read_exif_tag(exifData, &posv, "Exif.SubImage1.VignettingCorrParams"))
  {
    // Validate
    const int nc = posd->toLong(0);
    if(nc <= 16 && 2*nc == posc->toLong(0) && nc == posv->toLong(0))
    {
      img->exif_correction_type = CORRECTION_TYPE_SONY;
      img->exif_correction_data.sony.nc = nc;
      for(int i = 0; i < nc; i++)
      {
        img->exif_correction_data.sony.distortion[i] = posd->toLong(i + 1);
        img->exif_correction_data.sony.ca_r[i] = posc->toLong(i + 1);
        img->exif_correction_data.sony.ca_b[i] = posc->toLong(nc + i + 1);
        img->exif_correction_data.sony.vignetting[i] = posv->toLong(i + 1);
      }
    }
  }

  /*
   * Fuji lens correction data
   */
  if(Exiv2::versionNumber() >= EXIV2_MAKE_VERSION(0, 27, 4)
    && _exif_read_exif_tag(exifData, &posd, "Exif.Fujifilm.GeometricDistortionParams")
    && _exif_read_exif_tag(exifData, &posc, "Exif.Fujifilm.ChromaticAberrationParams")
    && _exif_read_exif_tag(exifData, &posv, "Exif.Fujifilm.VignettingParams"))
  {
    // X-Trans IV/V
    if(posd->count() == 19 && posc->count() == 29 && posv->count() == 19)
    {
      const int nc = 9;
      img->exif_correction_type = CORRECTION_TYPE_FUJI;
      img->exif_correction_data.fuji.nc = nc;
      for(int i = 0; i < nc; i++)
      {
        const float kd = posd->toFloat(i + 1);
        const float kc = posc->toFloat(i + 1);
        const float kv = posv->toFloat(i + 1);

        // Check that the knots position is the same for distortion, ca and vignetting,
        if(kd != kc || kd != kv)
        {
          img->exif_correction_type = CORRECTION_TYPE_NONE;
          break;
        }

        img->exif_correction_data.fuji.knots[i] = kd;
        img->exif_correction_data.fuji.distortion[i] = posd->toFloat(i + 10);
        img->exif_correction_data.fuji.ca_r[i] = posc->toFloat(i + 10);
        img->exif_correction_data.fuji.ca_b[i] = posc->toFloat(i + 19);
        img->exif_correction_data.fuji.vignetting[i] = posv->toFloat(i + 10);
      }

      // Account for the 1.25x crop modes in some Fuji cameras
      if(FIND_EXIF_TAG("Exif.Fujifilm.CropMode")
         && (pos->toLong() == 2 || pos->toLong() == 4))
        img->exif_correction_data.fuji.cropf = 1.25f;
      else
        img->exif_correction_data.fuji.cropf = 1;
    }
    // X-Trans I/II/III
    else if(posd->count() == 23 && posc->count() == 31 && posv->count() == 23)
    {
      const int nc = 11;
      img->exif_correction_type = CORRECTION_TYPE_FUJI;
      img->exif_correction_data.fuji.nc = nc;
      for(int i = 0; i < nc; i++)
      {
        const float kd = posd->toFloat(i + 1);
        float kc = 0;
        // ca data doesn't provide first knot (0)
        if(i != 0) kc = posc->toFloat(i);
        const float kv = posv->toFloat(i + 1);
        // check that the knots position is the same for distortion, ca and vignetting,
        if(kd != kc || kd != kv)
        {
          img->exif_correction_type = CORRECTION_TYPE_NONE;
          break;
        }

        img->exif_correction_data.fuji.knots[i] = kd;
        img->exif_correction_data.fuji.distortion[i] = posd->toFloat(i + 12);

        // ca data doesn't provide first knot (0)
        if(i == 0)
        {
          img->exif_correction_data.fuji.ca_r[i] = 0;
          img->exif_correction_data.fuji.ca_b[i] = 0;
        }
        else
        {
          img->exif_correction_data.fuji.ca_r[i] = posc->toFloat(i + 10);
          img->exif_correction_data.fuji.ca_b[i] = posc->toFloat(i + 20);
        }
        img->exif_correction_data.fuji.vignetting[i] = posv->toFloat(i + 12);
      }

      // Account for the 1.25x crop modes in some Fuji cameras
      if(FIND_EXIF_TAG("Exif.Fujifilm.CropMode")
         && (pos->toLong() == 2 || pos->toLong() == 4))
        img->exif_correction_data.fuji.cropf = 1.25f;
      else
        img->exif_correction_data.fuji.cropf = 1;
    }
  }

  /*
   * Olympus distortion correction
   */
  if(Exiv2::versionNumber() >= EXIV2_MAKE_VERSION(0, 27, 4)
     && _exif_read_exif_tag(exifData, &pos, "Exif.OlympusIp.0x150a"))
  {
    if(pos->count() == 4)
    {
      for(int i = 0; i < 4; i++)
      {
        const float kd = pos->toFloat(i);
        img->exif_correction_data.olympus.dist[i] = kd;
        if(kd != 0 && i < 3)
        {
          // Assume it's valid if any of the first three elements are nonzero. Ignore
          // the fourth element since the null value for no correction is '0 0 0 1'.
          img->exif_correction_type = CORRECTION_TYPE_OLYMPUS;
          img->exif_correction_data.olympus.has_dist = TRUE;
        }
      }
    }
  }

  /*
   * Olympus CA correction
   */
  if(Exiv2::versionNumber() >= EXIV2_MAKE_VERSION(0, 27, 4)
    && _exif_read_exif_tag(exifData, &pos, "Exif.OlympusIp.0x150c"))
  {
    if(pos->count() == 6)
    {
      for(int i = 0; i < 6; i++)
      {
        const float kc = pos->toFloat(i);
        img->exif_correction_data.olympus.ca[i] = kc;
        if(kc != 0)
        {
          img->exif_correction_type = CORRECTION_TYPE_OLYMPUS;
          img->exif_correction_data.olympus.has_ca = TRUE;
        }
      }
    }
  }

  return img->exif_correction_type != CORRECTION_TYPE_NONE;
}

void dt_exif_img_check_additional_tags(dt_image_t *img,
                                       const char *filename)
{
  try
  {
    std::unique_ptr<Exiv2::Image> image(Exiv2::ImageFactory::open(WIDEN(filename)));
    assert(image.get() != 0);
    read_metadata_threadsafe(image);
    Exiv2::ExifData &exifData = image->exifData();
    if(!exifData.empty())
    {
      _check_usercrop(exifData, img);
      _check_dng_opcodes(exifData, img);
      _check_lens_correction_data(exifData, img);
      _check_linear_response_limit(exifData, img);
    }
    return;
  }
  catch(Exiv2::AnyError &e)
  {
    const char *errstring = e.what();
    dt_print(DT_DEBUG_IMAGEIO, "[exiv2 reading additional exif tags] %s: %s", filename, errstring);
    return;
  }
}

static void _find_datetime_taken(Exiv2::ExifData &exifData,
                                 Exiv2::ExifData::const_iterator pos,
                                 char *exif_datetime_taken)
{
  // Note: We allow a longer "datetime original" field with an unnecessary
  // trailing byte(s) due to buggy software that creates it.
  // See https://github.com/darktable-org/darktable/issues/17389
  if((FIND_EXIF_TAG("Exif.Image.DateTimeOriginal")
      || FIND_EXIF_TAG("Exif.Photo.DateTimeOriginal"))
     && pos->size() >= DT_DATETIME_EXIF_LENGTH)
  {
    _strlcpy_to_utf8(exif_datetime_taken, DT_DATETIME_EXIF_LENGTH, pos, exifData);
    if(FIND_EXIF_TAG("Exif.Photo.SubSecTimeOriginal")
       && pos->size() > 1)
    {
      char msec[4];
      _strlcpy_to_utf8(msec, sizeof(msec), pos, exifData);
      dt_datetime_add_subsec_to_exif(exif_datetime_taken, DT_DATETIME_LENGTH, msec);
    }
  }
  else
  {
    *exif_datetime_taken = '\0';
  }
}

static void _find_exif_maker(Exiv2::ExifData &exifData,
                             Exiv2::ExifData::const_iterator pos,
                             char *maker,
                             const size_t m_size)
{
  // Look for maker & model first so we can use that info later
  if(FIND_EXIF_TAG("Exif.Image.Make"))
  {
    _strlcpy_to_utf8(maker, m_size, pos, exifData);
  }
  else if(FIND_EXIF_TAG("Exif.PanasonicRaw.Make"))
  {
    _strlcpy_to_utf8(maker, m_size, pos, exifData);
  }

  for(char *c = maker + m_size - 1; c > maker; c--)
    if(*c != ' ' && *c != '\0')
    {
      *(c + 1) = '\0';
      break;
    }
}

static void _find_exif_model(Exiv2::ExifData &exifData,
                             Exiv2::ExifData::const_iterator pos,
                             char *model,
                             const size_t m_size)
{
  if(FIND_EXIF_TAG("Exif.Image.Model"))
  {
    _strlcpy_to_utf8(model, m_size, pos, exifData);
  }
  else if(FIND_EXIF_TAG("Exif.PanasonicRaw.Model"))
  {
    _strlcpy_to_utf8(model, m_size, pos, exifData);
  }

  for(char *c = model + m_size - 1; c > model; c--)
    if(*c != ' ' && *c != '\0')
    {
      *(c + 1) = '\0';
      break;
    }
}

static void _find_exif_makermodel(Exiv2::ExifData &exifData,
                                  Exiv2::ExifData::const_iterator pos,
                                  dt_image_basic_exif_t *basic_exif)
{
  char exif_maker[sizeof(basic_exif->maker)];
  char exif_model[sizeof(basic_exif->model)];
  char model[sizeof(basic_exif->model)];
  exif_maker[0] = exif_model[0] = basic_exif->maker[0] =
    model[0] = basic_exif->model[0] = '\0';

  // Look for maker & model first so we can use that info later
  _find_exif_maker(exifData, pos, exif_maker, sizeof(exif_maker));
  _find_exif_model(exifData, pos, exif_model, sizeof(exif_model));
  dt_imageio_lookup_makermodel(exif_maker, exif_model,
                               basic_exif->maker, sizeof(basic_exif->maker),
                               model, sizeof(model),
                               basic_exif->model, sizeof(basic_exif->model));
}

static bool _exif_decode_exif_data(dt_image_t *img, Exiv2::ExifData &exifData)
{
  try
  {
    // List of tag names taken from Exiv2's printSummary() in actions.cpp
    Exiv2::ExifData::const_iterator pos;

    _find_exif_maker(exifData, pos, img->exif_maker, sizeof(img->exif_maker));
    _find_exif_model(exifData, pos, img->exif_model, sizeof(img->exif_model));

    // Make sure we copy the Exif make and model to the correct place if needed
    dt_image_refresh_makermodel(img);

    // Read shutter time
    if((pos = Exiv2::exposureTime(exifData)) != exifData.end() && pos->size())
    {
      img->exif_exposure = pos->toFloat();
    }
    else if(FIND_EXIF_TAG("Exif.Photo.ShutterSpeedValue")
            || FIND_EXIF_TAG("Exif.Image.ShutterSpeedValue"))
    {
      img->exif_exposure = exp2f(-1.0f * pos->toFloat()); // convert from APEX value
    }

    // Read exposure bias
    if(FIND_EXIF_TAG("Exif.Photo.ExposureBiasValue")
       || FIND_EXIF_TAG("Exif.Image.ExposureBiasValue"))
    {
      img->exif_exposure_bias = pos->toFloat();
    }

    // Read aperture
    if((pos = Exiv2::fNumber(exifData)) != exifData.end() && pos->size())
    {
      img->exif_aperture = pos->toFloat();
    }
    else if(FIND_EXIF_TAG("Exif.Photo.ApertureValue")
            || FIND_EXIF_TAG("Exif.Image.ApertureValue"))
    {
      img->exif_aperture = exp2f(pos->toFloat() / 2.0f); // convert from APEX value
    }

    // Read ISO speed - Nikon happens to return a pair for Lo and Hi modes
    if((pos = Exiv2::isoSpeed(exifData)) != exifData.end() && pos->size())
    {
      // If standard Exif ISO tag, use the old way of interpreting the
      // return value to be more regression-safe
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
    // some newer cameras support ISO settings that exceed the 16 bit
    // of Exif's ISOSpeedRatings
    if(img->exif_iso == 65535 || img->exif_iso == 0)
    {
      if(FIND_EXIF_TAG("Exif.PentaxDng.ISO")
         || FIND_EXIF_TAG("Exif.Pentax.ISO"))
      {
        std::string str = pos->print();
        img->exif_iso = (float)std::atof(str.c_str());
      }
      else if((!g_strcmp0(img->exif_maker, "SONY")
               || !g_strcmp0(img->exif_maker, "Canon"))
              && FIND_EXIF_TAG("Exif.Photo.RecommendedExposureIndex"))
      {
        img->exif_iso = pos->toFloat();
      }
    }

    // Read focal length
    if((pos = Exiv2::focalLength(exifData)) != exifData.end() && pos->size())
    {
      // This works around a bug in Exiv2 the developers refuse to fix.
      // For details see http://dev.exiv2.org/issues/1083
      if(pos->key() == "Exif.Canon.FocalLength" && pos->count() == 4)
        img->exif_focal_length = pos->toFloat(1);
      else
        img->exif_focal_length = pos->toFloat();
    }

    // Read focal length in 35mm if available and try to calculate crop factor.
    if(FIND_EXIF_TAG("Exif.Photo.FocalLengthIn35mmFilm"))
    {
      const float focal_length_35mm = pos->toFloat();
      if(focal_length_35mm > 0.0f && img->exif_focal_length > 0.0f)
        img->exif_crop = focal_length_35mm / img->exif_focal_length;
      else
        img->exif_crop = 0.0f;
    }

    // If the tag for the equivalent focal length is missing or contains zero,
    // let's try to get the crop factor by calculating the diagonal of the sensor:
    if(img->exif_crop == 0.0f && FIND_EXIF_TAG("Exif.Photo.FocalPlaneXResolution"))
    {
      float x_resolution = pos->toFloat();
      float y_resolution = 0.0f;
      if(FIND_EXIF_TAG("Exif.Photo.FocalPlaneYResolution"))
        y_resolution = pos->toFloat();
      guint res_unit = 1;
      if(FIND_EXIF_TAG("Exif.Photo.FocalPlaneResolutionUnit"))
        res_unit = pos->toLong();
      if(res_unit == 2) // inch
      {
        x_resolution /= 25.4f;
        y_resolution /= 25.4f;
      }
      else
      if(res_unit == 3) // centimeter
      {
        x_resolution /= 10.0f;
        y_resolution /= 10.0f;
      }
      guint image_width = 0;
      guint image_height = 0;

      // We are entering the zoo of image dimensions metadata.
      // Let's first try the Exif way of telling dimensions.
      // For Canon and Sigma cameras, these are the valid raw image
      // dimensions matching the the resolution tags.
      // For Fujifilm cameras, this will get the pixel dimensions
      // of the preview, not the sensor, because that's what the data
      // in the resolution tags on most Fujifilm cameras seems to be
      // calculated for.
      if(FIND_EXIF_TAG("Exif.Photo.PixelXDimension"))
        image_width = pos->toLong();
      if(FIND_EXIF_TAG("Exif.Photo.PixelYDimension"))
        image_height = pos->toLong();

      // Then try the Adobe DNG way of telling dimensions.
      // Exif.Image.ImageWidth/Length tags are also present in DNG files,
      // but may contain the pixel dimensions of the preview image, which in
      // this case will cause the diagonal calculation to be incorrect.
      if(image_width == 0 && FIND_EXIF_TAG("Exif.SubImage1.NewSubfileType"))
      {
        if(pos->toLong() == 0)  // Primary image
        {
          if(FIND_EXIF_TAG("Exif.SubImage1.ImageWidth"))
            image_width = pos->toLong();
          if(FIND_EXIF_TAG("Exif.SubImage1.ImageLength"))
            image_height = pos->toLong();
        }
      }
      // The following tags in certain formats may contain pixel dimensions of
      // the preview instead of the full image, while resolution is calculated
      // relative to the full image dimensions. So we check them last.
      if(image_width == 0)
      {
        if(FIND_EXIF_TAG("Exif.Image.ImageWidth"))
          image_width = pos->toLong();
        if(FIND_EXIF_TAG("Exif.Image.ImageLength"))
          image_height = pos->toLong();
      }

      const float x_size_mm = (float)image_width / x_resolution;
      const float y_size_mm = (float)image_height / y_resolution;
      if(image_width && image_height) // We've got the data and can calculate the crop factor
      {
        const float sensor_diagonal = dt_fast_hypotf(x_size_mm, y_size_mm);
        const float fullframe_diagonal = dt_fast_hypotf(36.0f, 24.0f);
        img->exif_crop = fullframe_diagonal / sensor_diagonal;

        // Improve the accuracy of the calculated EFL in the most noticeable cases
        // by adjusting slightly inaccurate calculated crop factor to the real value.
        // When the focal length and EFL are slightly different for a crop factor
        // of 1, it will look odd... So this is really a cosmetic fix to avoid
        // unexpected numbers rather than correcting really inaccurate values.
        if(fabsf(1.0f - img->exif_crop) < 0.1f) img->exif_crop = 1.0f;
        if(fabsf(2.0f - img->exif_crop) < 0.1f) img->exif_crop = 2.0f;
      }
      else
        img->exif_crop = 0.0f; // Will be shown as "no data" in the image information module
    }

    // Override crop factors for models for which we can't calculate the correct value
    for (size_t i = 0; i < sizeof(dt_cropfactors)/sizeof(dt_cropfactors[0]); i++)
    {
      if(!strcmp(img->exif_model, dt_cropfactors[i].model))
      {
        img->exif_crop = dt_cropfactors[i].cropfactor;
        break;
      }
    }

    if(_check_usercrop(exifData, img))
    {
      img->flags |= DT_IMAGE_HAS_ADDITIONAL_EXIF_TAGS;
      guint tagid = 0;
      char tagname[64];
      snprintf(tagname, sizeof(tagname), "darktable|mode|exif-crop");
      dt_tag_new(tagname, &tagid);
      dt_tag_attach(tagid, img->id, FALSE, FALSE);
    }

    if(_check_dng_opcodes(exifData, img)
       || _check_lens_correction_data(exifData, img))
    {
      img->flags |= DT_IMAGE_HAS_ADDITIONAL_EXIF_TAGS;
    }

    /*
     * Get the focus distance in meters.
     */
    if(Exiv2::testVersion(0, 27, 4)
       && FIND_EXIF_TAG("Exif.NikonLd4.LensID")
       && pos->toLong() != 0)
    {
      // Z lens, need to specifically look for the 2-nd instance of Exif.NikonLd4.FocusDistance
      // unless using Exiv2 0.28.x and later (also expanded to 2 bytes of precision since 0.28.1).
#if EXIV2_TEST_VERSION(0, 28, 0)
      if(FIND_EXIF_TAG("Exif.NikonLd4.FocusDistance2"))
      {
        float value = pos->toFloat();
        if(Exiv2::testVersion(0, 28, 1)) value /= 256.0f;
#else
      pos = exifData.end();
      for(auto it = exifData.begin(); it != exifData.end(); it++)
      {
        if(it->key() == "Exif.NikonLd4.FocusDistance") pos = it;
      }
      if(pos != exifData.end() && pos->size())
      {
        float value = pos->toFloat();
#endif
        img->exif_focus_distance = 0.01f * pow(10.0f, value / 40.0f);
      }
    }
    else if(FIND_EXIF_TAG("Exif.NikonLd2.FocusDistance")
            || FIND_EXIF_TAG("Exif.NikonLd3.FocusDistance")
            || (Exiv2::testVersion(0, 27, 4)
                && FIND_EXIF_TAG("Exif.NikonLd4.FocusDistance")))
    {
      float value = pos->toFloat();
      img->exif_focus_distance = 0.01f * pow(10.0f, value / 40.0f);
    }
    else if(FIND_EXIF_TAG("Exif.OlympusFi.FocusDistance"))
    {
      /*
      The distance is stored as a rational (fraction) according to
      http://www.dpreview.com/forums/thread/1173960?page=4

      Some Olympus cameras have a wrong denominator of 10 in there
      while the nominator is always in mm. Thus we ignore the
      denominator and divide with 1000.

      "I've checked a number of E-1 and E-300 images, and I agree
      that the FocusDistance looks like it is in mm for the
      E-1. However, it looks more like cm for the E-300.

      For both cameras, this value is stored as a rational. With the
      E-1, the denominator is always 1, while for the E-300 it is 10.

      Therefore, it looks like the numerator in both cases is in mm
      (which makes a bit of sense, in an odd sort of way). So I
      think what I will do in ExifTool is to take the numerator and
      divide by 1000 to display the focus distance in meters."
      -- Boardhead, dpreview forums in 2005
       */
      int nominator = pos->toRational(0).first;
      img->exif_focus_distance = fmax(0.0, (0.001 * nominator));
    }
    else if(FIND_EXIF_TAG("Exif.CanonFi.FocusDistanceUpper"))
    {
      const float FocusDistanceUpper = pos->toFloat();
      if(FocusDistanceUpper <= 0.0f
         || (int)FocusDistanceUpper >= 0xffff)
      {
        img->exif_focus_distance = 0.0f;
      }
      else
      {
        img->exif_focus_distance = FocusDistanceUpper / 100.0;
        if(FIND_EXIF_TAG("Exif.CanonFi.FocusDistanceLower"))
        {
          const float FocusDistanceLower = pos->toFloat();
          if(FocusDistanceLower > 0.0f && (int)FocusDistanceLower < 0xffff)
          {
            img->exif_focus_distance += FocusDistanceLower / 100.0;
            img->exif_focus_distance /= 2.0;
          }
        }
      }
    }
    else if(FIND_EXIF_TAG("Exif.CanonSi.SubjectDistance"))
    {
      img->exif_focus_distance = pos->toFloat() / 100.0;
    }
    else if((pos = Exiv2::subjectDistance(exifData)) != exifData.end() && pos->size())
    {
      img->exif_focus_distance = pos->toFloat();
    }
    else if(FIND_EXIF_TAG("Exif.Sony2Fp.FocusPosition2"))
    {
      const float focus_position = pos->toFloat();

      if(focus_position
         && FIND_EXIF_TAG("Exif.Photo.FocalLengthIn35mmFilm"))
      {
        const float focal_length_35mm = pos->toFloat();

        // http://u88.n24.queensu.ca/exiftool/forum/index.php/topic,3688.msg29653.html#msg29653
        img->exif_focus_distance =
          (pow(2, focus_position / 16 - 5) + 1) * focal_length_35mm / 1000;
      }
    }

    /*
     * Read image orientation
     */
    if(FIND_EXIF_TAG("Exif.Image.Orientation"))
    {
      img->orientation = dt_image_orientation_to_flip_bits(pos->toLong());
    }
    else if(FIND_EXIF_TAG("Exif.PanasonicRaw.Orientation"))
    {
      img->orientation = dt_image_orientation_to_flip_bits(pos->toLong());
    }
    // For e.g. Sinar backs the raw orientation is in a different subdirectory
    if(FIND_EXIF_TAG("Exif.Thumbnail.PhotometricInterpretation")
       && (32803 == pos->toLong())
       && FIND_EXIF_TAG("Exif.Thumbnail.Orientation"))
    {
      img->orientation = dt_image_orientation_to_flip_bits(pos->toLong());
    }

    // Read GPS location
    if(FIND_EXIF_TAG("Exif.GPSInfo.GPSLatitude"))
    {
      Exiv2::ExifData::const_iterator ref =
        exifData.findKey(Exiv2::ExifKey("Exif.GPSInfo.GPSLatitudeRef"));
      if(ref != exifData.end() && ref->size() && pos->count() == 3)
      {
        std::string sign_str = ref->toString();
        const char *sign = sign_str.c_str();
        double latitude = 0.0;
        if(dt_util_gps_rationale_to_number
           (pos->toRational(0).first, pos->toRational(0).second,
            pos->toRational(1).first, pos->toRational(1).second,
            pos->toRational(2).first, pos->toRational(2).second, sign[0], &latitude))
          img->geoloc.latitude = latitude;
      }
    }

    if(FIND_EXIF_TAG("Exif.GPSInfo.GPSLongitude"))
    {
      Exiv2::ExifData::const_iterator ref =
        exifData.findKey(Exiv2::ExifKey("Exif.GPSInfo.GPSLongitudeRef"));
      if(ref != exifData.end() && ref->size() && pos->count() == 3)
      {
        std::string sign_str = ref->toString();
        const char *sign = sign_str.c_str();
        double longitude = 0.0;
        if(dt_util_gps_rationale_to_number
           (pos->toRational(0).first, pos->toRational(0).second,
            pos->toRational(1).first, pos->toRational(1).second,
            pos->toRational(2).first, pos->toRational(2).second, sign[0], &longitude))
          img->geoloc.longitude = longitude;
      }
    }

    if(FIND_EXIF_TAG("Exif.GPSInfo.GPSAltitude"))
    {
      Exiv2::ExifData::const_iterator ref =
        exifData.findKey(Exiv2::ExifKey("Exif.GPSInfo.GPSAltitudeRef"));
      if(ref != exifData.end() && ref->size())
      {
        std::string sign_str = ref->toString();
        const char *sign = sign_str.c_str();
        double elevation = 0.0;
        if(dt_util_gps_elevation_to_number(pos->toRational(0).first,
                                           pos->toRational(0).second,
                                           sign[0],
                                           &elevation))
          img->geoloc.elevation = elevation;
      }
    }

    // Read lens name
    if((FIND_EXIF_TAG("Exif.CanonCs.LensType")
        && pos->toLong() != 61182   // prefer the other tag for RF lenses
        && pos->toLong() != 0
        && pos->toLong() != 65535)
       || FIND_EXIF_TAG("Exif.Canon.LensModel"))
    {
      _strlcpy_to_utf8(img->exif_lens, sizeof(img->exif_lens), pos, exifData);
    }
    else if(FIND_EXIF_TAG("Exif.PentaxDng.LensType"))
    {
      _strlcpy_to_utf8(img->exif_lens, sizeof(img->exif_lens), pos, exifData);
    }
    else if(FIND_EXIF_TAG("Exif.Panasonic.LensType"))
    {
      _strlcpy_to_utf8(img->exif_lens, sizeof(img->exif_lens), pos, exifData);
    }
    else if(FIND_EXIF_TAG("Exif.OlympusEq.LensType"))
    {
      // For every Olympus camera Exif.OlympusEq.LensType is present.
      _strlcpy_to_utf8(img->exif_lens, sizeof(img->exif_lens), pos, exifData);

      // We have to check if Exif.OlympusEq.LensType has been translated by
      // Exiv2. If it hasn't, fall back to Exif.OlympusEq.LensModel.
      std::string lens(img->exif_lens);
      if(std::string::npos == lens.find_first_not_of(" 1234567890"))
      {
        // Exif.OlympusEq.LensType contains only digits and spaces.
        // This means that Exiv2 couldn't convert it to human readable form.
        if(FIND_EXIF_TAG("Exif.OlympusEq.LensModel"))
        {
          _strlcpy_to_utf8(img->exif_lens, sizeof(img->exif_lens), pos, exifData);
        }
        // Just in case Exif.OlympusEq.LensModel hasn't been found.
        else if(FIND_EXIF_TAG("Exif.Photo.LensModel"))
        {
          _strlcpy_to_utf8(img->exif_lens, sizeof(img->exif_lens), pos, exifData);
        }
        dt_print(DT_DEBUG_ALWAYS, "[exif] Warning: lens \"%s\" unknown as \"%s\"",
                 img->exif_lens, lens.c_str());
      }
    }
    else if(Exiv2::testVersion(0,27,4)
            && FIND_EXIF_TAG("Exif.NikonLd4.LensID") && pos->toLong() == 0)
    {
      // Z body w/ FTZ adapter or recent F body (e.g. D780, D6) detected.
      // Prioritize the legacy ID lookup instead of Exif.Photo.LensModel included
      // in the default Exiv2::lensName() search below.
      if(FIND_EXIF_TAG("Exif.NikonLd4.LensIDNumber"))
        _strlcpy_to_utf8(img->exif_lens, sizeof(img->exif_lens), pos, exifData);
    }
    else if((pos = Exiv2::lensName(exifData)) != exifData.end() && pos->size())
    {
      _strlcpy_to_utf8(img->exif_lens, sizeof(img->exif_lens), pos, exifData);
    }

    // Check if an "Unknown Lens (xxx)" was returned by using regex,
    // and retry Exif.Photo.LensModel as last resort
    GMatchInfo *match_info;

    GRegex *regex = g_regex_new("\\(\\d+\\)$",
                        (GRegexCompileFlags) 0,
                        (GRegexMatchFlags) 0,
                        NULL);
    g_regex_match_full(regex,
                       img->exif_lens,
                       -1,
                       0,
                       (GRegexMatchFlags) 0,
                       &match_info,
                       NULL);
    const int match_count = g_match_info_get_match_count(match_info);
    if(match_count > 0
        && FIND_EXIF_TAG("Exif.Photo.LensModel"))
    {
      _strlcpy_to_utf8(img->exif_lens, sizeof(img->exif_lens), pos, exifData);
    }

    g_match_info_free(match_info);
    g_regex_unref(regex);

    // Use pretty name for Canon RF & RF-S lenses (as in ExifTool/Exiv2/Lensfun).
    if(g_str_has_prefix(img->exif_lens, "RF"))
    {
      char *pretty;
      if(img->exif_lens[2] == '-')
        pretty = g_strconcat("Canon RF-S ", &img->exif_lens[4], (char *)NULL);
      else
        pretty = g_strconcat("Canon RF ", &img->exif_lens[2], (char *)NULL);
      g_strlcpy(img->exif_lens, pretty, sizeof(img->exif_lens));
      g_free(pretty);
    }

    // Capitalize Nikon Z-mount lenses properly for UI presentation.
    if(g_str_has_prefix(img->exif_lens, "NIKKOR")
       || g_str_has_prefix(img->exif_lens, "TAMRON"))
    {
      for(size_t i = 1; i <= 5; ++i)
        img->exif_lens[i] = g_ascii_tolower(img->exif_lens[i]);
    }

    // Handle Sigma lenses on modern bodies (e.g. Nikon Z) using already clean
    // Exif.Photo.LensMake and Exif.Photo.LensModel.
    if(g_ascii_strcasecmp(img->exif_lens, "Sigma") && FIND_EXIF_TAG("Exif.Photo.LensMake")
       && pos->toString() == "SIGMA")
    {
      char *pretty;
      pretty = g_strconcat("Sigma ", img->exif_lens, (char *)NULL);
      g_strlcpy(img->exif_lens, pretty, sizeof(img->exif_lens));
      g_free(pretty);
    }

    if((pos = Exiv2::whiteBalance(exifData)) != exifData.end() && pos->size())
    {
      const int value = pos->toLong();
      _exif_value_str(value, img->exif_whitebalance,
                      sizeof(img->exif_whitebalance), pos, exifData);
    }

    if(FIND_EXIF_TAG("Exif.Photo.Flash"))
    {
      const int value = pos->toLong();
      if(value != 0)
      {
        _strlcpy_to_utf8(img->exif_flash, sizeof(img->exif_flash), pos, exifData);
      }
    }

    if(FIND_EXIF_TAG("Exif.Photo.ExposureProgram"))
    {
      const int value = pos->toLong();
      if(value != 0)
      {
        _exif_value_str(value, img->exif_exposure_program,
                        sizeof(img->exif_exposure_program), pos, exifData);
      }
    }

    if(FIND_EXIF_TAG("Exif.Photo.MeteringMode"))
    {
      const int value = pos->toLong();
      if(value != 0)
      {
        _exif_value_str(value, img->exif_metering_mode,
                        sizeof(img->exif_metering_mode), pos, exifData);
      }
    }

    char datetime[DT_DATETIME_LENGTH];
    _find_datetime_taken(exifData, pos, datetime);
    dt_datetime_exif_to_img(img, datetime);

    if(FIND_EXIF_TAG("Exif.Image.Artist"))
    {
      std::string str = pos->print(&exifData);
      dt_metadata_set_import(img->id, "Xmp.dc.creator", str.c_str());
    }
    else if(FIND_EXIF_TAG("Exif.Canon.OwnerName"))
    {
      std::string str = pos->print(&exifData);
      dt_metadata_set_import(img->id, "Xmp.dc.creator", str.c_str());
    }

    // FIXME: Should the UserComment go into the description? Or do we
    // need an extra field for this?
    if(FIND_EXIF_TAG("Exif.Photo.UserComment"))
    {
      std::string str = pos->print(&exifData);
      Exiv2::CommentValue value(str);
      std::string str2 = value.comment();
      if(str2 != "binary comment")
        dt_metadata_set_import(img->id, "Xmp.dc.description", str2.c_str());
    }
    else if(FIND_EXIF_TAG("Exif.Image.ImageDescription"))
    {
      std::string str = pos->print(&exifData);
      dt_metadata_set_import(img->id, "Xmp.dc.description", str.c_str());
    }

    if(FIND_EXIF_TAG("Exif.Image.Copyright"))
    {
      std::string str = pos->print(&exifData);
      dt_metadata_set_import(img->id, "Xmp.dc.rights", str.c_str());
    }

    if(!dt_conf_get_bool("ui_last/ignore_exif_rating"))
    {
      if(FIND_EXIF_TAG("Exif.Image.Rating"))
      {
        const int stars = pos->toLong();
        dt_image_set_xmp_rating(img, stars);
      }
      else if(FIND_EXIF_TAG("Exif.Image.RatingPercent"))
      {
        const int stars = pos->toLong() * 5. / 100;
        dt_image_set_xmp_rating(img, stars);
      }
      else
        dt_image_set_xmp_rating(img, -2);
    }

    // Read embedded color matrix and DNG related tags.
    if(FIND_EXIF_TAG("Exif.Image.DNGVersion"))
    {
      gboolean missing_sample = FALSE;

      // initialize matrixes and data with noop / defaults
      float CM[3][9];
      float FM[3][9];
      float CC[3][9];
      float AB[9];

      for(int k = 0; k < 3; k++)    // found illuminants
        for(int y = 0; y < 3; y++)
          for(int x = 0; x < 3; x++)
          {
            const int i = 3 * y + x;
            CM[k][i] = FM[k][i] = CC[k][i] = AB[i] = x == y ? 1.0f : 0.0f;
          }

      // flags about what we got as exif tags
      gboolean has_CM[3] = { FALSE, FALSE, FALSE };
      gboolean has_FM[3] = { FALSE, FALSE, FALSE };
      gboolean has_CC[3] = { FALSE, FALSE, FALSE };
      gboolean has_AB = FALSE;

      dt_dng_illuminant_t illu[3] = { DT_LS_Unknown, DT_LS_Unknown, DT_LS_Unknown };

      // make sure for later testing fallback later via
      // `find_temperature_from_raw_coeffs` if there is no valid illuminant
      dt_mark_colormatrix_invalid(&img->d65_color_matrix[0]);

#if EXIV2_TEST_VERSION(0,27,4)
      Exiv2::ExifData::const_iterator ab_pos =
        exifData.findKey(Exiv2::ExifKey("Exif.Image.AnalogBalance"));
      if(ab_pos != exifData.end() && ab_pos->count() == 3)
      {
        AB[0] = ab_pos->toFloat(0);
        AB[4] = ab_pos->toFloat(1);
        AB[8] = ab_pos->toFloat(2);
        has_AB = TRUE;
      }
#endif

      if(FIND_EXIF_TAG("Exif.Image.CalibrationIlluminant1"))
        illu[0] = MIN(DT_LS_Other, (dt_dng_illuminant_t) pos->toLong());

      if(FIND_EXIF_TAG("Exif.Image.CalibrationIlluminant2"))
        illu[1] = MIN(DT_LS_Other, (dt_dng_illuminant_t) pos->toLong());

#if EXIV2_TEST_VERSION(0,27,4)
      if(FIND_EXIF_TAG("Exif.Image.CalibrationIlluminant3"))
        illu[2] = MIN(DT_LS_Other, (dt_dng_illuminant_t) pos->toLong());
#endif

      Exiv2::ExifData::const_iterator cm1_pos =
        exifData.findKey(Exiv2::ExifKey("Exif.Image.ColorMatrix1"));
      if(cm1_pos != exifData.end() && cm1_pos->count() == 9)
      {
        for(int i = 0; i < 9; i++) CM[0][i] = cm1_pos->toFloat(i);
        has_CM[0] = TRUE;

        Exiv2::ExifData::const_iterator cc1_pos =
          exifData.findKey(Exiv2::ExifKey("Exif.Image.CameraCalibration1"));
        if(cc1_pos != exifData.end() && cc1_pos->count() == 9)
        {
          for(int i = 0; i < 9; i++) CC[0][i] = cc1_pos->toFloat(i);
          has_CC[0] = TRUE;
        }

        Exiv2::ExifData::const_iterator fm1_pos =
          exifData.findKey(Exiv2::ExifKey("Exif.Image.ForwardMatrix1"));
        if(fm1_pos != exifData.end() && fm1_pos->count() == 9)
        {
          for(int i = 0; i < 9; i++) FM[0][i] = fm1_pos->toFloat(i);
          has_FM[0] = TRUE;
        }
      }

      Exiv2::ExifData::const_iterator cm2_pos =
        exifData.findKey(Exiv2::ExifKey("Exif.Image.ColorMatrix2"));
      if(cm2_pos != exifData.end() && cm2_pos->count() == 9)
      {
        for(int i = 0; i < 9; i++) CM[1][i] = cm2_pos->toFloat(i);
        has_CM[1] = TRUE;

        Exiv2::ExifData::const_iterator cc2_pos =
          exifData.findKey(Exiv2::ExifKey("Exif.Image.CameraCalibration2"));
        if(cc2_pos != exifData.end() && cc2_pos->count() == 9)
        {
          for(int i = 0; i < 9; i++) CC[1][i] = cc2_pos->toFloat(i);
          has_CC[1] = TRUE;
        }

        Exiv2::ExifData::const_iterator fm2_pos =
          exifData.findKey(Exiv2::ExifKey("Exif.Image.ForwardMatrix2"));
        if(fm2_pos != exifData.end() && fm2_pos->count() == 9)
        {
          for(int i = 0; i < 9; i++) FM[1][i] = fm2_pos->toFloat(i);
          has_FM[1] = TRUE;
        }
      }

#if EXIV2_TEST_VERSION(0,27,4)
      Exiv2::ExifData::const_iterator cm3_pos =
        exifData.findKey(Exiv2::ExifKey("Exif.Image.ColorMatrix3"));
      if(cm3_pos != exifData.end() && cm3_pos->count() == 9)
      {
        for(int i = 0; i < 9; i++) CM[2][i] = cm3_pos->toFloat(i);
        has_CM[2] = TRUE;

        Exiv2::ExifData::const_iterator cc3_pos =
          exifData.findKey(Exiv2::ExifKey("Exif.Image.CameraCalibration3"));
        if(cc3_pos != exifData.end() && cc3_pos->count() == 9)
        {
          for(int i = 0; i < 9; i++) CC[2][i] = cc3_pos->toFloat(i);
          has_CC[2] = TRUE;
        }

        Exiv2::ExifData::const_iterator fm3_pos =
          exifData.findKey(Exiv2::ExifKey("Exif.Image.ForwardMatrix3"));
        if(fm3_pos != exifData.end() && fm3_pos->count() == 9)
        {
          for(int i = 0; i < 9; i++) FM[2][i] = fm3_pos->toFloat(i);
          has_FM[2] = TRUE;
        }
      }
#endif

      dt_print(DT_DEBUG_IMAGEIO, "[exif] check dng exif data in `%s`", img->filename);
      if(has_AB)
        dt_print(DT_DEBUG_IMAGEIO, "has AnalogBalance %.4f %.4f %.4f", AB[0], AB[4], AB[8]);

      for(int k = 0; k < 3; k++)
      {
        const gboolean anymat = has_CM[k] || has_CC[k] || has_FM[k];

        // Currently dt does not support DT_LS_Other so we do a fallback but backreport and request samples
        if(illu[k] == DT_LS_Other)
        {
          dt_control_log(_("detected OtherIlluminant in `%s`, please report via darktable github"), img->filename);
          dt_print(DT_DEBUG_IMAGEIO, "detected not-implemented OtherIlluminant in `%s`, please report via darktable github", img->filename);
          illu[k] = DT_LS_D65;
          missing_sample = TRUE;
        }

        if(illu[k] != DT_LS_Unknown)
          dt_print(DT_DEBUG_IMAGEIO, "has CalibrationIlluminant%i = %s", k+1, _illu_to_str(illu[k]));
        else if(anymat)
          dt_print(DT_DEBUG_IMAGEIO, "undefined CalibrationIlluminant%i", k+1);

        if(has_CM[k]) _print_matrix_data("has ColorMatrix", k+1, CM[k]);
        if(has_CC[k]) _print_matrix_data("has CameraCalibration", k+1, CC[k]);
        if(has_FM[k]) _print_matrix_data("has ForwardMatrix", k+1, FM[k]);
      }

      // Find the best illuminant
      // Prepare checks for required interpolation
      // FIXME interpolate the matrixes
      int sel_illu = -1;

      int sel_temp = 0;
      int found_illus = 0;
      int min_temp = 100000;
      const int D65temp = _illu_to_temp(DT_LS_D65);
      int delta_min = D65temp;

      // Which illuminant will be used for the color matrix?
      // We first try to find D65 or take the next higher.
      for(int i = 0; i < 3; ++i)
      {
        if(illu[i] != DT_LS_Unknown)
        {
          const int temp_cur = _illu_to_temp(illu[i]);
          min_temp = MIN(min_temp, temp_cur);
          const int delta_cur = abs(temp_cur - D65temp);
          if(temp_cur > sel_temp && delta_cur <= delta_min)
          {
            sel_illu = i;
            sel_temp = temp_cur;
            delta_min = delta_cur;
          }
          found_illus++;
        }
      }

      const gboolean interpolate = found_illus > 1 && sel_temp > D65temp && min_temp < D65temp && min_temp > 0;
      if(interpolate)
      {
        dt_control_log(_("special exif illuminants in `%s`, please report via darktable github"), img->filename);
        dt_print(DT_DEBUG_ALWAYS, "special exif illuminants in `%s`, please report via darktable github", img->filename);
        missing_sample = TRUE;
      }


      // If there is none defined we'll use the first valid color matrix assuming it's D65 (keep dt < 3.8 behavior).
      if(sel_illu == -1)
      {
        for(int i = 0; i < 3; i++)
        {
          if(illu[i] == DT_LS_Unknown && has_CM[i])
          {
            sel_illu = i;
            sel_temp = D65temp;
            dt_print(DT_DEBUG_IMAGEIO, "[exif] Unknown Illuminant fallback to D65");
            illu[i] = DT_LS_D65;
            break;
          }
        }
      }

      if(sel_illu > -1 && sel_illu < 3)
      {
        const dt_dng_illuminant_t illuminant = illu[sel_illu];
        const gboolean forward_suggested = has_FM[sel_illu];
        if(forward_suggested)
        {
          dt_control_log(_("forward matrix in `%s`, please report via darktable github"), img->filename);
          missing_sample = TRUE;
        }
        dt_print(DT_DEBUG_IMAGEIO,
          "[exif] %s%s%s: selected from  [1] %s (%iK), [2] %s (%iK), [3] %s (%iK)",
                 _illu_to_str(illuminant),
                interpolate ? " (should be interpolated)" : "",
                forward_suggested ? " (forward)" : "",
                 _illu_to_str(illu[0]), _illu_to_temp(illu[0]),
                 _illu_to_str(illu[1]), _illu_to_temp(illu[1]),
                 _illu_to_str(illu[2]), _illu_to_temp(illu[2]));

        float DT_ALIGNED_ARRAY cameratoXYZ[9];
        float DT_ALIGNED_ARRAY ABxCC[9];

        mat3mul(ABxCC, AB, CC[sel_illu]);
        mat3mul(cameratoXYZ, ABxCC, CM[sel_illu]);

        mat3mul(img->d65_color_matrix, illuminant_data[illuminant].CA, cameratoXYZ);
        _print_matrix_data("dt_image_t d65_color_matrix", 0, img->d65_color_matrix);
      }
      if(missing_sample)
      {
        guint tagid = 0;
        char tagname[32];
        snprintf(tagname, sizeof(tagname), "darktable|issue|no-samples");
        dt_tag_new(tagname, &tagid);
        dt_tag_attach(tagid, img->id, FALSE, FALSE);
      }
    }

    gboolean is_monochrome = FALSE;
    gboolean is_hdr = dt_image_is_hdr(img);

    // Finding out about DNG HDR and monochrome images can be done
    // here while reading Exif data.
    if(FIND_EXIF_TAG("Exif.Image.DNGVersion"))
    {
      int format = 1;
      int bps = 0;
      int spp = 0;
      int phi = 0;

      if(FIND_EXIF_TAG("Exif.SubImage1.SampleFormat"))
        format = pos->toLong();
      else if(FIND_EXIF_TAG("Exif.Image.SampleFormat"))
        format = pos->toLong();

      if(FIND_EXIF_TAG("Exif.SubImage1.BitsPerSample"))
        bps = pos->toLong();
      else if(FIND_EXIF_TAG("Exif.Image.BitsPerSample"))
        bps = pos->toLong();

      if(FIND_EXIF_TAG("Exif.SubImage1.SamplesPerPixel"))
        spp = pos->toLong();
      else if(FIND_EXIF_TAG("Exif.Image.SamplesPerPixel"))
        spp = pos->toLong();

      if(FIND_EXIF_TAG("Exif.SubImage1.PhotometricInterpretation"))
        phi = pos->toLong();
      else if(FIND_EXIF_TAG("Exif.Image.PhotometricInterpretation"))
        phi = pos->toLong();

      if((format == 3) && (bps >= 16) && ((phi == 32803) || (phi == 34892))) is_hdr = TRUE;

      if((spp == 1) && (phi == 34892)) is_monochrome = TRUE;
    }

    if(is_hdr)
      dt_imageio_set_hdr_tag(img);

    if(is_monochrome)
    {
      img->flags |= DT_IMAGE_MONOCHROME;
      dt_imageio_update_monochrome_workflow_tag(img->id, DT_IMAGE_MONOCHROME);
    }

    // Some files have the colorspace explicitly set. Try to read that.
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

    // Improve lens detection for Sony SAL lenses.
    if(FIND_EXIF_TAG("Exif.Sony2.LensID")
       && pos->toLong() != 65535
       && pos->print().find('|') == std::string::npos)
    {
      _strlcpy_to_utf8(img->exif_lens, sizeof(img->exif_lens), pos, exifData);
    }
    // Workaround for an issue on newer Sony NEX cameras.
    // The default EXIF field is not used by Sony to store lens data
    // http://dev.exiv2.org/issues/883
    // http://darktable.org/redmine/issues/8813
    // FIXME: This is still a workaround
    else if((!strncmp(img->exif_model, "NEX", 3))
            || (!strncmp(img->exif_model, "ILCE", 4)))
    {
      snprintf(img->exif_lens, sizeof(img->exif_lens), "(unknown)");
      if(FIND_EXIF_TAG("Exif.Photo.LensModel"))
      {
        std::string str = pos->print(&exifData);
        snprintf(img->exif_lens, sizeof(img->exif_lens), "%s", str.c_str());
      }
    };

    img->exif_inited = TRUE;
    return true;
  }
  catch(Exiv2::AnyError &e)
  {
    const char *errstring = e.what();
    dt_print(DT_DEBUG_IMAGEIO, "[exiv2 _exif_decode_exif_data] %s: %s", img->filename, errstring);
    return false;
  }
}

void dt_exif_apply_default_metadata(dt_image_t *img)
{
  if(dt_conf_get_bool("ui_last/import_apply_metadata")
     && !(img->job_flags & DT_IMAGE_JOB_NO_METADATA))
  {
    char *str;

    for(unsigned int i = 0; i < DT_METADATA_NUMBER; i++)
    {
      if(dt_metadata_get_type(i) != DT_METADATA_TYPE_INTERNAL)
      {
        const char *name = dt_metadata_get_name(i);
        char *setting = g_strdup_printf("plugins/lighttable/metadata/%s_flag", name);
        const gboolean hidden = dt_conf_get_int(setting) & DT_METADATA_FLAG_HIDDEN;
        g_free(setting);
        // don't import hidden stuff
        if(!hidden)
        {
          setting = g_strdup_printf("ui_last/import_last_%s", name);
          str = dt_conf_get_string(setting);
          if(str && str[0])
          {
            // calculated metadata
            dt_variables_params_t *params;
            dt_variables_params_init(&params);
            params->filename = img->filename;
            params->jobcode = "import";
            params->sequence = 0;
            params->imgid = img->id;
            params->img = (void *)img;
            // at this time only Exif info is available
            gchar *result = dt_variables_expand(params, str, FALSE);
            dt_variables_params_destroy(params);
            if(result && result[0])
            {
              g_free(str);
              str = result;
            }
            dt_metadata_set(img->id, dt_metadata_get_key(i), str, FALSE);
            g_free(str);
          }
          g_free(setting);
        }
      }
    }

    str = dt_conf_get_string("ui_last/import_last_tags");
    if(dt_is_valid_imgid(img->id)
       && str != NULL
       && str[0] != '\0')
    {
      GList *imgs = NULL;
      imgs = g_list_prepend(imgs, GINT_TO_POINTER(img->id));
      dt_tag_attach_string_list(str, imgs, FALSE);
      g_list_free(imgs);
    }
    g_free(str);
  }
}

// TODO: can this blob also contain XMP and IPTC data?
gboolean dt_exif_read_from_blob(dt_image_t *img,
                                uint8_t *blob,
                                const int size)
{
  try
  {
    Exiv2::ExifData exifData;
    Exiv2::ExifParser::decode(exifData, blob, size);
    bool res = _exif_decode_exif_data(img, exifData);
    dt_exif_apply_default_metadata(img);
    return res ? FALSE : TRUE;
  }
  catch(Exiv2::AnyError &e)
  {
    const char *errstring = e.what();
    dt_print(DT_DEBUG_IMAGEIO, "[exiv2 dt_exif_read_from_blob] %s: %s", img->filename, errstring);
    return TRUE;
  }
}

/**
 * Get the largest possible thumbnail from the image
 */
gboolean dt_exif_get_thumbnail(const char *path,
                               uint8_t **buffer,
                               size_t *size,
                               char **mime_type)
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
      dt_print(DT_DEBUG_LIGHTTABLE,
               "[exiv2 dt_exif_get_thumbnail] couldn't find thumbnail for %s", path);
      return TRUE;
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
      dt_print(DT_DEBUG_IMAGEIO,
               "[exiv2 dt_exif_get_thumbnail] couldn't allocate memory for thumbnail for %s",
               path);
      return TRUE;
    }

    memcpy(*buffer, tmp, _size);

    return FALSE;
  }
  catch(Exiv2::AnyError &e)
  {
    dt_print(DT_DEBUG_IMAGEIO,
             "[exiv2 dt_exif_get_thumbnail] %s: %s",
             path,
             e.what());
    return TRUE;
  }
}

/* Read the metadata of an image.
 * XMP data trumps IPTC data trumps EXIF data.
 */
gboolean dt_exif_read(dt_image_t *img,
                      const char *path)
{
  // At least set 'datetime taken' to something useful in case there is
  // no Exif data in this file (pfm, png, ...)
  struct stat statbuf;

  if(!stat(path, &statbuf))
  {
    dt_datetime_unix_to_img(img, &statbuf.st_mtime);
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
    {
      res = _exif_decode_exif_data(img, exifData);
      if(dt_conf_get_bool("ui/detect_mono_exif"))
      {
        const int oldflags =
          dt_image_monochrome_flags(img)
          | (img->flags & DT_IMAGE_MONOCHROME_WORKFLOW);
        if(dt_imageio_has_mono_preview(path))
          img->flags |= (DT_IMAGE_MONOCHROME_PREVIEW
                         | DT_IMAGE_MONOCHROME_WORKFLOW);
        else
          img->flags &= ~(DT_IMAGE_MONOCHROME_PREVIEW
                          | DT_IMAGE_MONOCHROME_WORKFLOW);

        if(oldflags != (dt_image_monochrome_flags(img)
                        | (img->flags & DT_IMAGE_MONOCHROME_WORKFLOW)))
          dt_imageio_update_monochrome_workflow_tag(img->id,
                                                    dt_image_monochrome_flags(img));
      }
    }
    else
      img->exif_inited = TRUE;

    // These get overwritten by IPTC and XMP. Is that how it should work?
    dt_exif_apply_default_metadata(img);

    // IPTC metadata.
    Exiv2::IptcData &iptcData = image->iptcData();
    if(!iptcData.empty()) res = _exif_decode_iptc_data(img, iptcData) && res;

    // XMP metadata.
    Exiv2::XmpData &xmpData = image->xmpData();
    if(!xmpData.empty())
      res = _exif_decode_xmp_data(img, xmpData, -1, true) && res;

    // Initialize size - don't wait for full raw to be loaded to get this
    // information. If use_embedded_thumbnail is set, it will take a
    // change in development history to have this information.
    img->height = image->pixelHeight();
    img->width = image->pixelWidth();

    return res ? FALSE : TRUE;
  }
  catch(Exiv2::AnyError &e)
  {
    dt_print(DT_DEBUG_IMAGEIO,
             "[exiv2 dt_exif_read] %s: %s",
             path,
             e.what());
    return TRUE;
  }
}

int dt_exif_write_blob(uint8_t *blob,
                       uint32_t size,
                       const char *path,
                       const int compressed)
{
  try
  {
    std::unique_ptr<Exiv2::Image> image(Exiv2::ImageFactory::open(WIDEN(path)));
    assert(image.get() != 0);
    read_metadata_threadsafe(image);
    Exiv2::ExifData &imgExifData = image->exifData();
    Exiv2::ExifData blobExifData;
    Exiv2::ExifParser::decode(blobExifData, blob, size);
    Exiv2::ExifData::const_iterator end = blobExifData.end();
    Exiv2::ExifData::iterator it;
    for(Exiv2::ExifData::const_iterator i = blobExifData.begin(); i != end; ++i)
    {
      // add() does not override! We need to delete existing key first.
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
      _remove_exif_keys(imgExifData, keys, n_keys);
    }

    // Only compressed images may set PixelXDimension and PixelYDimension.
    if(!compressed)
    {
      static const char *keys[] = {
        "Exif.Photo.PixelXDimension",
        "Exif.Photo.PixelYDimension"
      };
      static const guint n_keys = G_N_ELEMENTS(keys);
      _remove_exif_keys(imgExifData, keys, n_keys);
    }

    imgExifData.sortByTag();
    image->writeMetadata();
  }
  catch(Exiv2::AnyError &e)
  {
    dt_print(DT_DEBUG_IMAGEIO,
             "[exiv2 dt_exif_write_blob] %s: %s",
             path,
             e.what());
    return 0;
  }
  return 1;
}

static void _remove_exif_geotag(Exiv2::ExifData &exifData)
{
  static const char *keys[] =
  {
    "Exif.GPSInfo.GPSLatitude",
    "Exif.GPSInfo.GPSLongitude",
    "Exif.GPSInfo.GPSAltitude",
    "Exif.GPSInfo.GPSLatitudeRef",
    "Exif.GPSInfo.GPSLongitudeRef",
    "Exif.GPSInfo.GPSAltitudeRef",
    "Exif.GPSInfo.GPSVersionID"
  };
  static const guint n_keys = G_N_ELEMENTS(keys);
  _remove_exif_keys(exifData, keys, n_keys);
}

int dt_exif_read_blob(uint8_t **buf,
                      const char *path,
                      const dt_imgid_t imgid,
                      const gboolean sRGB,
                      const int out_width,
                      const int out_height,
                      const gboolean dng_mode)
{
  *buf = NULL;
  try
  {
    std::unique_ptr<Exiv2::Image> image(Exiv2::ImageFactory::open(WIDEN(path)));
    assert(image.get() != 0);
    read_metadata_threadsafe(image);
    Exiv2::ExifData &exifData = image->exifData();

    // Get rid of thumbnails
    Exiv2::ExifThumb(exifData).erase();
    Exiv2::ExifData::const_iterator pos;

    {
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
        "Exif.Image.TileWidth",
        "Exif.Image.TileLength",
        "Exif.Image.TileOffsets",
        "Exif.Image.TileByteCounts",
        "Exif.Image.PlanarConfiguration"
      };
      static const guint n_keys = G_N_ELEMENTS(keys);
      _remove_exif_keys(exifData, keys, n_keys);
    }

    // Many tags should be removed in all cases as they are simply wrong
    // also for DNG files.

    // Remove subimage* trees, related to thumbnails or HDR usually; also UserCrop.
    for(Exiv2::ExifData::iterator i = exifData.begin(); i != exifData.end();)
    {
      static const std::string needle = "Exif.SubImage";
      if(i->key().compare(0, needle.length(), needle) == 0)
        i = exifData.erase(i);
      else
        ++i;
    }

    {
      static const char *keys[] = {
        // Canon color space info
        "Exif.Canon.ColorSpace",
        "Exif.Canon.ColorData",

        // Nikon thumbnail data
        "Exif.Nikon3.Preview",
        "Exif.NikonPreview.JPEGInterchangeFormat",

        // TIFF/EP & Exif stuff irrelevant after developing
        "Exif.Image.CFARepeatPatternDim",
        "Exif.Image.CFAPattern",
        "Exif.Image.InterColorProfile",
        "Exif.Image.SpectralSensitivity",
        "Exif.Image.OECF",
        "Exif.Image.SpatialFrequencyResponse",
        "Exif.Image.Noise",
        "Exif.Image.SensingMethod",
        "Exif.Image.TIFFEPStandardID",
        "Exif.Photo.SpectralSensitivity",
        "Exif.Photo.OECF",
        "Exif.Photo.SpatialFrequencyResponse",
        "Exif.Photo.SensingMethod",
        "Exif.Photo.CFAPattern",

        // DNG stuff that is irrelevant or misleading
        "Exif.Image.DNGVersion",
        "Exif.Image.DNGBackwardVersion",
        "Exif.Image.DNGPrivateData",
        "Exif.Image.DefaultBlackRender",
        "Exif.Image.DefaultCropOrigin",
        "Exif.Image.DefaultCropSize",
        "Exif.Image.RawDataUniqueID",
        "Exif.Image.OriginalRawFileName",
        "Exif.Image.OriginalRawFileData",
        "Exif.Image.ActiveArea",
        "Exif.Image.MaskedAreas",
        "Exif.Image.AsShotICCProfile",
        "Exif.Image.OpcodeList1",
        "Exif.Image.OpcodeList2",
        "Exif.Image.OpcodeList3",
        "Exif.Image.AsShotNeutral",
        "Exif.Image.AsShotWhiteXY",
        "Exif.Image.BaselineExposure",
        "Exif.Image.BaselineNoise",
        "Exif.Image.BaselineSharpness",
        "Exif.Image.LinearResponseLimit",
        "Exif.Image.ShadowScale",
        "Exif.Image.PreviewApplicationName",
        "Exif.Image.PreviewApplicationVersion",
        "Exif.Image.PreviewSettingsDigest",
        "Exif.Image.PreviewColorSpace",
        "Exif.Image.PreviewDateTime",
        "Exif.Image.NoiseProfile",
        "Exif.Image.NewRawImageDigest",
        "Exif.Image.RawImageDigest",

        "Exif.Photo.MakerNote",

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
        "Exif.Olympus.ThumbnailLength",

        "Exif.Image.BaselineExposureOffset",

        // Samsung makernote cleanup, the entries below have no
        // relevance for exported images
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
      _remove_exif_keys(exifData, keys, n_keys);
    }

    static const char *dngkeys[] = {
      // Embedded color profile info
      "Exif.Image.ColorMatrix1",
      "Exif.Image.ColorMatrix2",
      "Exif.Image.CameraCalibration1",
      "Exif.Image.CameraCalibration2",
      "Exif.Image.ReductionMatrix1",
      "Exif.Image.ReductionMatrix2",
      "Exif.Image.AnalogBalance",
      "Exif.Image.CalibrationIlluminant1",
      "Exif.Image.CalibrationIlluminant2",
      "Exif.Image.CameraCalibrationSignature",
      "Exif.Image.ProfileCalibrationSignature",
      "Exif.Image.ExtraCameraProfiles",
      "Exif.Image.AsShotProfileName",
      "Exif.Image.ProfileName",
      "Exif.Image.ProfileHueSatMapDims",
      "Exif.Image.ProfileHueSatMapData1",
      "Exif.Image.ProfileHueSatMapData2",
      "Exif.Image.ProfileToneCurve",
      "Exif.Image.ProfileEmbedPolicy",
      "Exif.Image.ProfileCopyright",
      "Exif.Image.ForwardMatrix1",
      "Exif.Image.ForwardMatrix2",
      "Exif.Image.ProfileLookTableDims",
      "Exif.Image.ProfileLookTableData",
      "Exif.Image.ProfileLookTableEncoding",

#if EXIV2_TEST_VERSION(0,27,4)
      "Exif.Image.ProfileHueSatMapEncoding",
      "Exif.Image.ColorMatrix3",
      "Exif.Image.CameraCalibration3",
      "Exif.Image.CalibrationIlluminant3",
      "Exif.Image.ReductionMatrix3",
      "Exif.Image.ProfileHueSatMapData3",
      "Exif.Image.ForwardMatrix3"
#else
      "Exif.Image.ProfileHueSatMapEncoding"
#endif

    };
    static const guint n_dngkeys = G_N_ELEMENTS(dngkeys);
    _remove_exif_keys(exifData, dngkeys, n_dngkeys);

    // Write appropriate color space tag if using sRGB output.
    if(sRGB)
      exifData["Exif.Photo.ColorSpace"] = uint16_t(1); // sRGB
    else
      exifData["Exif.Photo.ColorSpace"] = uint16_t(0xFFFF); // Uncalibrated

    // We don't write the orientation here for DNG as it is set in
    // dt_imageio_dng_write_tiff_header or might be defined in this
    // blob.
    if(!dng_mode) exifData["Exif.Image.Orientation"] = uint16_t(1);

    // Replace RAW dimension with output dimensions (for example after
    // crop/scale, or orientation for DNG mode).
    if(out_width > 0) exifData["Exif.Photo.PixelXDimension"] = (uint32_t)out_width;
    if(out_height > 0) exifData["Exif.Photo.PixelYDimension"] = (uint32_t)out_height;

    const int resolution = dt_conf_get_int("metadata/resolution");
    exifData["Exif.Image.XResolution"] = Exiv2::Rational(resolution, 1);
    exifData["Exif.Image.YResolution"] = Exiv2::Rational(resolution, 1);
    exifData["Exif.Image.ResolutionUnit"] = uint16_t(2); // inches

    exifData["Exif.Image.Software"] = darktable_package_string;

    // TODO: Find a nice place for the missing metadata (tags,
    // publisher, colorlabels?). Additionally find out how to embed
    // XMP data. And shall we add a description of the history stack
    // to Exif.Image.ImageHistory?
    if(dt_is_valid_imgid(imgid))
    {
      // Delete metadata taken from the original file if it's fields
      // we manage in dt, too.
      static const char * keys[] = {
        "Exif.Image.Artist",
        "Exif.Image.ImageDescription",
        "Exif.Photo.UserComment",
        "Exif.Image.Copyright",
        "Exif.Image.Rating",
        "Exif.Image.RatingPercent",
        "Exif.Photo.SubSecTimeOriginal",
        "Exif.GPSInfo.GPSVersionID",
        "Exif.GPSInfo.GPSLongitudeRef",
        "Exif.GPSInfo.GPSLatitudeRef",
        "Exif.GPSInfo.GPSLongitude",
        "Exif.GPSInfo.GPSLatitude",
        "Exif.GPSInfo.GPSAltitudeRef",
        "Exif.GPSInfo.GPSAltitude"
      };
      static const guint n_keys = G_N_ELEMENTS(keys);
      _remove_exif_keys(exifData, keys, n_keys);

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
#if EXIV2_TEST_VERSION(0,27,4)
      else
        // Mandatory tag for TIFF/EP and recommended for Exif, empty is ok (unknown)
        // but correctly written only by Exiv2 >= 0.27.4.
        exifData["Exif.Image.ImageDescription"] = "";
#endif

      res = dt_metadata_get(imgid, "Xmp.dc.rights", NULL);
      if(res != NULL)
      {
        exifData["Exif.Image.Copyright"] = (char *)res->data;
        g_list_free_full(res, &g_free);
      }
#if EXIV2_TEST_VERSION(0,27,4)
      else
        // Mandatory tag for TIFF/EP and optional for Exif, empty is ok (unknown)
        // but correctly written only by exiv2 >= 0.27.4.
        exifData["Exif.Image.Copyright"] = "";
#endif

      res = dt_metadata_get(imgid, "Xmp.xmp.Rating", NULL);
      if(res != NULL)
      {
        const int rating = GPOINTER_TO_INT(res->data) + 1;
        exifData["Exif.Image.Rating"] = rating;
        g_list_free(res);
      }

      // GPS data
      _remove_exif_geotag(exifData);
      const dt_image_t *cimg = dt_image_cache_get(darktable.image_cache, imgid, 'r');
      if(!std::isnan(cimg->geoloc.longitude) && !std::isnan(cimg->geoloc.latitude))
      {
        exifData["Exif.GPSInfo.GPSVersionID"] = "02 02 00 00";
        exifData["Exif.GPSInfo.GPSLongitudeRef"] = (cimg->geoloc.longitude < 0) ? "W" : "E";
        exifData["Exif.GPSInfo.GPSLatitudeRef"] = (cimg->geoloc.latitude < 0) ? "S" : "N";

        const long long_deg = (int)floor(fabs(cimg->geoloc.longitude));
        const long lat_deg = (int)floor(fabs(cimg->geoloc.latitude));
        const long long_min = (int)floor
          ((fabs(cimg->geoloc.longitude)
            - floor(fabs(cimg->geoloc.longitude))) * 60000000);
        const long lat_min = (int)floor
          ((fabs(cimg->geoloc.latitude)
            - floor(fabs(cimg->geoloc.latitude))) * 60000000);
        gchar *long_str = g_strdup_printf("%ld/1 %ld/1000000 0/1", long_deg, long_min);
        gchar *lat_str = g_strdup_printf("%ld/1 %ld/1000000 0/1", lat_deg, lat_min);
        exifData["Exif.GPSInfo.GPSLongitude"] = long_str;
        exifData["Exif.GPSInfo.GPSLatitude"] = lat_str;
        g_free(long_str);
        g_free(lat_str);
      }
      if(!std::isnan(cimg->geoloc.elevation))
      {
        exifData["Exif.GPSInfo.GPSVersionID"] = "02 02 00 00";
        exifData["Exif.GPSInfo.GPSAltitudeRef"] = (cimg->geoloc.elevation < 0) ? "1" : "0";

        long ele_dm = (int)floor(fabs(10.0 * cimg->geoloc.elevation));
        gchar *ele_str = g_strdup_printf("%ld/10", ele_dm);
        exifData["Exif.GPSInfo.GPSAltitude"] = ele_str;
        g_free(ele_str);
      }

      // According to the Exif spec DateTime is to be set to the last
      // modification time while DateTimeOriginal is to be kept. For
      // us "keeping" it means to write out what we have in DB to
      // support people adding a time offset in the geotagging module.
      gchar new_datetime[DT_DATETIME_EXIF_LENGTH];
      dt_datetime_now_to_exif(new_datetime);
      exifData["Exif.Image.DateTime"] = new_datetime;
      gchar datetime[DT_DATETIME_LENGTH];
      dt_datetime_img_to_exif(datetime, sizeof(datetime), cimg);
      datetime[DT_DATETIME_EXIF_LENGTH - 1] = '\0';
      exifData["Exif.Image.DateTimeOriginal"] = datetime;
      exifData["Exif.Photo.DateTimeOriginal"] = datetime;
      if(g_strcmp0(&datetime[DT_DATETIME_EXIF_LENGTH], "000"))
        exifData["Exif.Photo.SubSecTimeOriginal"] = &datetime[DT_DATETIME_EXIF_LENGTH];

      dt_image_cache_read_release(darktable.image_cache, cimg);
    }

    Exiv2::Blob blob;
    Exiv2::ExifParser::encode(blob, Exiv2::bigEndian, exifData);
    const size_t length = blob.size();
    *buf = (uint8_t *)malloc(length);
    if(!*buf)
    {
      return 0;
    }
    memcpy(*buf, &(blob[0]), length);
    return length;
  }
  catch(Exiv2::AnyError &e)
  {
    // std::cerr.rdbuf(savecerr);
    dt_print(DT_DEBUG_IMAGEIO,
             "[exiv2 dt_exif_read_blob] %s: %s",
             path,
             e.what());
    free(*buf);
    *buf = NULL;
    return 0;
  }
}

// Encode binary blob into text:
char *dt_exif_xmp_encode(const unsigned char *input,
                         const int len,
                         int *output_len)
{
#define COMPRESS_THRESHOLD 100

  gboolean do_compress = FALSE;

  // If input data field exceeds a certain size we compress it and convert to base64;
  // main reason for compression: make more XMP data fit into 64k segment within
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

char *dt_exif_xmp_encode_internal(const unsigned char *input,
                                  const int len,
                                  int *output_len,
                                  gboolean do_compress)
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

    const int outlen = strlen(buffer2) + 5; // leading "gz" + compression
                                            // factor + base64 string +
                                            // trailing '\0'
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
    const char hex[16] = { '0', '1', '2', '3', '4', '5', '6', '7',
                           '8', '9', 'a', 'b', 'c', 'd', 'e', 'f' };

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
unsigned char *dt_exif_xmp_decode(const char *input,
                                  const int len,
                                  int *output_len)
{
  unsigned char *output = NULL;

  // Check if data is in compressed format
  if(!strncmp(input, "gz", 2))
  {
    // We have compressed data in base64 representation with leading "gz"

    // Get stored compression factor so we know the needed buffer size for uncompress
    const float factor = 10 * (input[2] - '0') + (input[3] - '0');

    // Get a rw copy of input buffer omitting leading "gz" and compression factor
    unsigned char *buffer = (unsigned char *)strdup(input + 4);
    if(!buffer) return NULL;

    // Decode from base64 to compressed binary
    gsize compressed_size;
    g_base64_decode_inplace((char *)buffer, &compressed_size);

    // Do the actual uncompress step
    int result = Z_BUF_ERROR;
    uLongf bufLen = factor * compressed_size;
    uLongf destLen;

    // We know the actual compression factor but if that fails we retry with
    // increasing buffer sizes, eg. we don't know (unlikely) factors > 99.
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
// We have uncompressed data in hexadecimal ascii representation

// ascii table:
// 48- 57 0-9
// 97-102 a-f
#define TO_BINARY(a) (a > 57 ? a - 97 + 10 : a - 48)

    // Make sure that we don't find any unexpected characters
    // indicating corrupted data.
    if(strspn(input, "0123456789abcdef") != strlen(input))
      return NULL;

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

static void _exif_import_tags(dt_image_t *img,
                              Exiv2::XmpData::iterator &pos)
{
  // Tags in array
  const int cnt = pos->count();

  sqlite3_stmt *stmt_sel_id, *stmt_ins_tags, *stmt_ins_tagged;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT id FROM data.tags WHERE name = ?1", -1,
                              &stmt_sel_id, NULL);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "INSERT INTO data.tags (id, name) VALUES (NULL, ?1)",
                              -1, &stmt_ins_tags, NULL);
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2
    (dt_database_get(darktable.db),
     "INSERT INTO main.tagged_images (tagid, imgid, position)"
     "  VALUES (?1, ?2,"
     "    (SELECT (IFNULL(MAX(position),0) & 0xFFFFFFFF00000000) + (1 << 32)"
     "      FROM main.tagged_images))",
     -1, &stmt_ins_tagged, NULL);
  // clang-format on

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

      // Check if tag is available, get its id:
      for(int k = 0; k < 2; k++)
      {
        DT_DEBUG_SQLITE3_BIND_TEXT(stmt_sel_id, 1, tag, -1, SQLITE_TRANSIENT);
        if(sqlite3_step(stmt_sel_id) == SQLITE_ROW)
          tagid = sqlite3_column_int(stmt_sel_id, 0);
        sqlite3_reset(stmt_sel_id);
        sqlite3_clear_bindings(stmt_sel_id);

        if(tagid > 0) break;

        dt_print(DT_DEBUG_ALWAYS, "[xmp_import] creating tag: %s", tag);

        // Create this tag (increment id, leave icon empty), retry.
        DT_DEBUG_SQLITE3_BIND_TEXT(stmt_ins_tags, 1, tag, -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt_ins_tags);
        sqlite3_reset(stmt_ins_tags);
        sqlite3_clear_bindings(stmt_ins_tags);
      }
      // Associate image and tag.
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

typedef struct history_entry_t
{
  char *operation;
  gboolean enabled;
  int modversion;
  unsigned char *params;
  int params_len;
  char *multi_name;
  int multi_name_hand_edited;
  int multi_priority;
  int blendop_version;
  unsigned char *blendop_params;
  int blendop_params_len;
  int num;
  double iop_order; // kept for compatibility with xmp version < 4

  // Sanity checking
  gboolean have_operation, have_params, have_modversion;
} history_entry_t;

// Used for a hash table that maps mask_id to the mask data
typedef struct mask_entry_t
{
  int mask_id;
  int mask_type;
  char *mask_name;
  int mask_version;
  unsigned char *mask_points;
  int mask_points_len;
  int mask_nb;
  unsigned char *mask_src;
  int mask_src_len;
  gboolean already_added;
  int mask_num;
  int version;
} mask_entry_t;

static void _print_history_entry(history_entry_t *entry) __attribute__((unused));
static void _print_history_entry(history_entry_t *entry)
{
  if(!entry || !entry->operation)
  {
    std::cout << "malformed entry" << std::endl;
    return;
  }

  std::cout << entry->operation << std::endl;
  std::cout << "  modversion      :"
            <<  entry->modversion
            << std::endl;
  std::cout << "  enabled         :"
            <<  entry->enabled
            << std::endl;
  std::cout << "  params          :"
            << (entry->params ? "<found>" : "<missing>")
            << std::endl;
  std::cout << "  multi_name      :"
            << (entry->multi_name ? entry->multi_name : "<missing>")
            << std::endl;
  std::cout << "  multi_priority  :"
            <<  entry->multi_priority
            << std::endl;
  std::cout << "  iop_order       :"
            << entry->iop_order
            << std::endl;
  std::cout << "  blendop_version :"
            <<  entry->blendop_version
            << std::endl;
  std::cout << "  blendop_params  :"
            << (entry->blendop_params ? "<found>" : "<missing>")
            << std::endl;
  std::cout << std::endl;
}

static void _free_history_entry(gpointer data)
{
  history_entry_t *entry = (history_entry_t *)data;
  g_free(entry->operation);
  g_free(entry->multi_name);
  free(entry->params);
  free(entry->blendop_params);
  free(entry);
}

// We have to use pugixml as the old format could contain empty rdf:li
// elements in the multi_name array which causes problems when
// accessing it with libexiv2 :(. superold is a flag indicating that
// data is wrapped in <rdf:Bag> instead of <rdf:Seq>.
static GList *_read_history_v1(const std::string &xmpPacket,
                              const char *filename,
                              const int superold)
{
  GList *history_entries = NULL;

  pugi::xml_document doc;
  pugi::xml_parse_result result = doc.load_string(xmpPacket.c_str());

  if(!result)
  {
    dt_print(DT_DEBUG_IMAGEIO,
             "XML '%s' parsed with errors\n"
             "Error description: %s\n"
             "Error offset: %td",
             filename,
             result.description(),
             result.offset);
    return NULL;
  }

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
    doc.select_node("//darktable:multi_name/rdf:Seq");

  // Fill the list of history entries. We are iterating over
  // history_operation as we know that it's there. The other iters
  // are taken care of manually.
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
    if(!current_entry)
       break;
    current_entry->blendop_version = 1; // default version in case it's not specified
    history_entries = g_list_append(history_entries, current_entry);

    current_entry->operation = g_strdup(operation_iter.child_value());

    current_entry->enabled = g_strcmp0(enabled_iter->child_value(), "0") != 0;

    current_entry->modversion = atoi(modversion_iter->child_value());

    current_entry->params = dt_exif_xmp_decode(params_iter->child_value(),
                                               strlen(params_iter->child_value()),
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
      current_entry->blendop_params =
        dt_exif_xmp_decode(blendop_params_iter->child_value(),
                           strlen(blendop_params_iter->child_value()),
                           &current_entry->blendop_params_len);
      blendop_params_iter++;
    }

    current_entry->iop_order = -1.0;

    modversion_iter++;
    enabled_iter++;
    params_iter++;
  }

  return history_entries;
}

static GList *_read_history_v2(Exiv2::XmpData &xmpData,
                               const char *filename)
{
  GList *history_entries = NULL;
  history_entry_t *current_entry = NULL;

  for(auto history = xmpData.findKey(Exiv2::XmpKey("Xmp.darktable.history"));
      history != xmpData.end();
      history++)
  {
    // TODO: Support human readable params via introspection with something like this:
    // XmpText: Xmp.darktable.history[1]/darktable:settings[1]/darktable:name = width
    // XmpText: Xmp.darktable.history[1]/darktable:settings[1]/darktable:value = 23

    std::string key_item = history->key();
    char *key = g_strdup(key_item.c_str());
    char *key_iter = key;
    if(g_str_has_prefix(key, "Xmp.darktable.history["))
    {
      key_iter += strlen("Xmp.darktable.history[");
      errno = 0;
      unsigned int n = strtol(key_iter, &key_iter, 10);
      if(errno)
      {
        dt_print(DT_DEBUG_IMAGEIO,
                 "error reading history from '%s' (%s)",
                 key,
                 filename);
        g_list_free_full(history_entries, _free_history_entry);
        g_free(key);
        return NULL;
      }

      // Skip everything that isn't part of the actual array
      if(*(key_iter++) != ']')
      {
        dt_print(DT_DEBUG_IMAGEIO,
                 "error reading history from '%s' (%s)",
                 key,
                 filename);
        g_list_free_full(history_entries, _free_history_entry);
        g_free(key);
        return NULL;
      }
      if(*(key_iter++) != '/') goto skip;
      if(*key_iter == '?') key_iter++;

      // Make sure we are filling in the details of the correct entry
      unsigned int length = g_list_length(history_entries);
      if(n > length)
      {
        current_entry = (history_entry_t*)calloc(1, sizeof(history_entry_t));
	if(current_entry)
	{
	  current_entry->blendop_version = 1; // default version in case it's not specified
	  current_entry->iop_order = -1.0;
	  history_entries = g_list_append(history_entries, current_entry);
	}
      }
      else if(n < length)
      {
        // AFAICT this can't happen with regular Exiv2 parsed XMP
        // data, but better safe than sorry. It can happen though
        // when constructing things in a unusual order and then
        // passing it to us without serializing it in between.
        current_entry =
          (history_entry_t *)g_list_nth_data(history_entries, n - 1);
        // XMP starts counting at 1!
      }

      // Go on reading things into current_entry
      if(g_str_has_prefix(key_iter, "darktable:operation"))
      {
        current_entry->have_operation = TRUE;
        std::string value_item = history->toString();
        current_entry->operation = g_strdup(value_item.c_str());
      }
      else if(g_str_has_prefix(key_iter, "darktable:num"))
      {
        current_entry->num = history->toLong();
      }
      else if(g_str_has_prefix(key_iter, "darktable:enabled"))
      {
        current_entry->enabled = history->toLong() == 1;
      }
      else if(g_str_has_prefix(key_iter, "darktable:modversion"))
      {
        current_entry->have_modversion = TRUE;
        current_entry->modversion = history->toLong();
      }
      else if(g_str_has_prefix(key_iter, "darktable:params"))
      {
        current_entry->have_params = TRUE;
        std::string value_item = history->toString();
        current_entry->params = dt_exif_xmp_decode(value_item.c_str(),
                                                   history->size(),
                                                   &current_entry->params_len);
      }
      else if(g_str_has_prefix(key_iter, "darktable:multi_name_hand_edited"))
      {
        current_entry->multi_name_hand_edited = history->toLong() == 1;
      }
      else if(g_str_has_prefix(key_iter, "darktable:multi_name"))
      {
        std::string value_item = history->toString();
        current_entry->multi_name = g_strdup(value_item.c_str());
      }
      else if(g_str_has_prefix(key_iter, "darktable:multi_priority"))
      {
        current_entry->multi_priority = history->toLong();
      }
      else if(g_str_has_prefix(key_iter, "darktable:iop_order"))
      {
        // We ensure reading the iop_order as a high precision float
        std::string value_item = history->toString();
        string str = g_strdup(value_item.c_str());
        static const std::locale& c_locale = std::locale("C");
        std::istringstream istring(str);
        istring.imbue(c_locale);
        istring >> current_entry->iop_order;
      }
      else if(g_str_has_prefix(key_iter, "darktable:blendop_version"))
      {
        current_entry->blendop_version = history->toLong();
      }
      else if(g_str_has_prefix(key_iter, "darktable:blendop_params"))
      {
        std::string value_item = history->toString();
        current_entry->blendop_params =
          dt_exif_xmp_decode(value_item.c_str(),
                             history->size(),
                             &current_entry->blendop_params_len);
      }
    }
skip:
    g_free(key);
  }

  // A final sanity check
  for(GList *iter = history_entries; iter; iter = g_list_next(iter))
  {
    history_entry_t *entry = (history_entry_t *)iter->data;
    if(!(entry->have_operation && entry->have_params && entry->have_modversion))
    {
      dt_print(DT_DEBUG_IMAGEIO,
               "[exif] error: reading history from '%s' failed due to missing tags",
               filename);
      g_list_free_full(history_entries, _free_history_entry);
      history_entries = NULL;
      break;
    }
  }

  return history_entries;
}

static void _free_mask_entry(gpointer data)
{
  mask_entry_t *entry = (mask_entry_t *)data;
  g_free(entry->mask_name);
  free(entry->mask_points);
  free(entry->mask_src);
  free(entry);
}

static GHashTable *_read_masks(Exiv2::XmpData &xmpData,
                               const char *filename,
                               const int version)
{
  GHashTable *mask_entries =
    g_hash_table_new_full(g_int_hash, g_int_equal, NULL, _free_mask_entry);

  // TODO: turn that into something like Xmp.darktable.history!
  Exiv2::XmpData::iterator mask;
  Exiv2::XmpData::iterator mask_name;
  Exiv2::XmpData::iterator mask_type;
  Exiv2::XmpData::iterator mask_version;
  Exiv2::XmpData::iterator mask_id;
  Exiv2::XmpData::iterator mask_nb;
  Exiv2::XmpData::iterator mask_src;
  if((mask = xmpData.findKey(Exiv2::XmpKey("Xmp.darktable.mask"))) != xmpData.end()
    && (mask_src =
        xmpData.findKey(Exiv2::XmpKey("Xmp.darktable.mask_src"))) != xmpData.end()
    && (mask_name =
        xmpData.findKey(Exiv2::XmpKey("Xmp.darktable.mask_name"))) != xmpData.end()
    && (mask_type =
        xmpData.findKey(Exiv2::XmpKey("Xmp.darktable.mask_type"))) != xmpData.end()
    && (mask_version =
        xmpData.findKey(Exiv2::XmpKey("Xmp.darktable.mask_version"))) != xmpData.end()
    && (mask_id = xmpData.findKey(Exiv2::XmpKey("Xmp.darktable.mask_id"))) != xmpData.end()
    && (mask_nb = xmpData.findKey(Exiv2::XmpKey("Xmp.darktable.mask_nb"))) != xmpData.end())
  {
    // Fixes API change happened after Exiv2 v0.27.2.1
    const size_t cnt = (size_t)mask->count();
    const size_t mask_src_cnt = (size_t)mask_src->count();
    const size_t mask_name_cnt = (size_t)mask_name->count();
    const size_t mask_type_cnt = (size_t)mask_type->count();
    const size_t mask_version_cnt = (size_t)mask_version->count();
    const size_t mask_id_cnt = (size_t)mask_id->count();
    const size_t mask_nb_cnt = (size_t)mask_nb->count();

    if(cnt == mask_src_cnt
       && cnt == mask_name_cnt
       && cnt == mask_type_cnt
       && cnt == mask_version_cnt
       && cnt == mask_id_cnt
       && cnt == mask_nb_cnt)
    {
      for(size_t i = 0; i < cnt; i++)
      {
        mask_entry_t *entry = (mask_entry_t*)calloc(1, sizeof(mask_entry_t));
	if(!entry)
	   break;
        entry->version = version;
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
        entry->mask_points = dt_exif_xmp_decode(mask_c, mask_c_len,
                                                &entry->mask_points_len);

        entry->mask_nb = mask_nb->toLong(i);

        std::string mask_src_str = mask_src->toString(i);
        const char *mask_src_c = mask_src_str.c_str();
        const size_t mask_src_c_len = strlen(mask_src_c);
        entry->mask_src = dt_exif_xmp_decode(mask_src_c, mask_src_c_len,
                                             &entry->mask_src_len);

        g_hash_table_insert(mask_entries, &entry->mask_id, (gpointer)entry);
      }
    }
  }

  return mask_entries;
}

static GList *_read_masks_v3(Exiv2::XmpData &xmpData,
                             const char *filename,
                             const int version)
{
  GList *history_entries = NULL;
  mask_entry_t *current_entry = NULL;

  for(auto history = xmpData.findKey(Exiv2::XmpKey("Xmp.darktable.masks_history"));
      history != xmpData.end();
      history++)
  {
    // TODO: Support human readable params via introspection with something like this:
    // XmpText: Xmp.darktable.history[1]/darktable:settings[1]/darktable:name = width
    // XmpText: Xmp.darktable.history[1]/darktable:settings[1]/darktable:value = 23

    std::string key_item = history->key();
    char *key = g_strdup(key_item.c_str());
    char *key_iter = key;
    if(g_str_has_prefix(key, "Xmp.darktable.masks_history["))
    {
      key_iter += strlen("Xmp.darktable.masks_history[");
      errno = 0;
      unsigned int n = strtol(key_iter, &key_iter, 10);
      if(errno)
      {
        dt_print(DT_DEBUG_IMAGEIO,
                 "error reading masks history from '%s' (%s)",
                 key,
                 filename);
        g_list_free_full(history_entries, _free_mask_entry);
        g_free(key);
        return NULL;
      }

      // Skip everything that isn't part of the actual array
      if(*(key_iter++) != ']')
      {
        dt_print(DT_DEBUG_IMAGEIO,
                 "error reading masks history from '%s' (%s)",
                 key,
                 filename);
        g_list_free_full(history_entries, _free_mask_entry);
        g_free(key);
        return NULL;
      }
      if(*(key_iter++) != '/') goto skip;
      if(*key_iter == '?') key_iter++;

      // Make sure we are filling in the details of the correct entry
      unsigned int length = g_list_length(history_entries);
      if(n > length)
      {
        current_entry = (mask_entry_t*)calloc(1, sizeof(mask_entry_t));
	if(current_entry)
	{
          current_entry->version = version;
	  history_entries = g_list_append(history_entries, current_entry);
	}
      }
      else if(n < length)
      {
        // AFAICT this can't happen with regular Exiv2 parsed XMP
        // data, but better safe than sorry. It can happen though
        // when constructing things in a unusual order and then
        // passing it to us without serializing it in between.
        current_entry =
          (mask_entry_t *)g_list_nth_data(history_entries, n - 1);
        // XMP starts counting at 1!
      }

      // Go on reading things into current_entry
      if(g_str_has_prefix(key_iter, "darktable:mask_num"))
      {
        current_entry->mask_num = history->toLong();
      }
      else if(g_str_has_prefix(key_iter, "darktable:mask_id"))
      {
        current_entry->mask_id = history->toLong();
      }
      else if(g_str_has_prefix(key_iter, "darktable:mask_type"))
      {
        current_entry->mask_type = history->toLong();
      }
      else if(g_str_has_prefix(key_iter, "darktable:mask_name"))
      {
        std::string value_item = history->toString();
        current_entry->mask_name = g_strdup(value_item.c_str());
      }
      else if(g_str_has_prefix(key_iter, "darktable:mask_version"))
      {
        current_entry->mask_version = history->toLong();
      }
      else if(g_str_has_prefix(key_iter, "darktable:mask_points"))
      {
        std::string value_item = history->toString();
        current_entry->mask_points = dt_exif_xmp_decode(value_item.c_str(),
                                                        history->size(),
                                                        &current_entry->mask_points_len);
      }
      else if(g_str_has_prefix(key_iter, "darktable:mask_nb"))
      {
        current_entry->mask_nb = history->toLong();
      }
      else if(g_str_has_prefix(key_iter, "darktable:mask_src"))
      {
        std::string value_item = history->toString();
        current_entry->mask_src = dt_exif_xmp_decode(value_item.c_str(),
                                                     history->size(),
                                                     &current_entry->mask_src_len);
      }

    }
skip:
    g_free(key);
  }

  return history_entries;
}

static void _add_mask_entry_to_db(const dt_imgid_t imgid,
                                  mask_entry_t *entry)
{
  // Add the mask entry only once
  if(entry->already_added)
    return;
  entry->already_added = TRUE;

  const int mask_num = 0;

  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(
    dt_database_get(darktable.db),
    "INSERT INTO main.masks_history"
    " (imgid, num, formid, form, name, version, points, points_count, source)"
    " VALUES (?1, ?9, ?2, ?3, ?4, ?5, ?6, ?7, ?8)",
    -1, &stmt, NULL);

  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, entry->mask_id);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, entry->mask_type);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 4, entry->mask_name, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 5, entry->mask_version);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 6, entry->mask_points,
                             entry->mask_points_len, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 7, entry->mask_nb);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 8, entry->mask_src,
                             entry->mask_src_len, SQLITE_TRANSIENT);
  if(entry->version < 3)
  {
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 9, mask_num);
  }
  else
  {
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 9, entry->mask_num);
  }
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

static void _add_non_clone_mask_entries_to_db(gpointer key,
                                              gpointer value,
                                              gpointer user_data)
{
  dt_imgid_t imgid = *(int *)user_data;
  mask_entry_t *entry = (mask_entry_t *)value;
  if(!(entry->mask_type & (DT_MASKS_CLONE | DT_MASKS_NON_CLONE)))
    _add_mask_entry_to_db(imgid, entry);
}

static void _add_mask_entries_to_db(const dt_imgid_t imgid,
                                    GHashTable *mask_entries,
                                    const int mask_id)
{
  if(mask_id <= 0) return;

  // Look for mask_id in the hash table
  mask_entry_t *entry = (mask_entry_t *)g_hash_table_lookup(mask_entries, &mask_id);

  if(!entry) return;

  // If it's a group: recurse into the children first
  if(entry->mask_type & DT_MASKS_GROUP)
  {
    dt_masks_point_group_t *group = (dt_masks_point_group_t *)entry->mask_points;
    if((int)(entry->mask_nb * sizeof(dt_masks_point_group_t)) != entry->mask_points_len)
    {
      dt_print(DT_DEBUG_ALWAYS,
               "[masks] error loading masks from XMP file, bad binary blob size.");
      return;
    }
    for(int i = 0; i < entry->mask_nb; i++)
      _add_mask_entries_to_db(imgid, mask_entries, group[i].formid);
  }

  _add_mask_entry_to_db(imgid, entry);
}

// Get MAX multi_priority
int _get_max_multi_priority(GList *history,
                            const char *operation)
{
  int max_prio = 0;

  for(GList *iter = history; iter; iter = g_list_next(iter))
  {
    history_entry_t *entry = (history_entry_t *)iter->data;

    if(!strcmp(entry->operation, operation))
      max_prio = MAX(max_prio, entry->multi_priority);
  }

  return max_prio;
}

static gboolean _image_altered_deprecated(const dt_imgid_t imgid)
{
  sqlite3_stmt *stmt;

  const gboolean basecurve_auto_apply = dt_is_display_referred();

  char query[1024] = { 0 };

  // clang-format off
  snprintf(query, sizeof(query),
           "SELECT 1"
           " FROM main.history, main.images"
           " WHERE id=?1 AND imgid=id AND num<history_end AND enabled=1"
           "       AND operation NOT IN ('flip', 'dither', 'highlights', 'rawprepare',"
           "                             'colorin', 'colorout', 'gamma', 'demosaic',"
           "                             'temperature'%s)",
           basecurve_auto_apply ? ", 'basecurve'" : "");
  // clang-format on

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);

  const gboolean altered = (sqlite3_step(stmt) == SQLITE_ROW);
  sqlite3_finalize(stmt);

  return altered;
}

// Need a write lock on *img (non-const) to write stars (and soon color labels).
gboolean dt_exif_xmp_read(dt_image_t *img,
                          const char *filename,
                          const int history_only)
{
  // Exclude pfm to avoid stupid errors on the console
  const char *c = filename + strlen(filename) - 4;
  if(c >= filename && !strcmp(c, ".pfm")) return TRUE;
  try
  {
    // Read XMP sidecar
    std::unique_ptr<Exiv2::Image> image(Exiv2::ImageFactory::open(WIDEN(filename)));
    assert(image.get() != 0);
    read_metadata_threadsafe(image);
    Exiv2::XmpData &xmpData = image->xmpData();

    sqlite3_stmt *stmt;

    Exiv2::XmpData::iterator pos;

    int xmp_version = 0;
    GList *iop_order_list = NULL;
    dt_iop_order_t iop_order_version = DT_IOP_ORDER_LEGACY;

    int num_masks = 0;
    if((pos = xmpData.findKey(Exiv2::XmpKey("Xmp.darktable.xmp_version"))) != xmpData.end())
      xmp_version = pos->toLong();

    if(!history_only)
    {
      // Otherwise we ignore title, description, ... from non-dt XMP files :(
      const size_t ns_pos =
        image->xmpPacket().find("xmlns:darktable=\"http://darktable.sf.net/\"");
      const bool is_a_dt_xmp = (ns_pos != std::string::npos);
      _exif_decode_xmp_data(img, xmpData, is_a_dt_xmp ? xmp_version : -1, false);
    }


    // Convert legacy flip bits (will not be written anymore, convert to
    // flip history item here):
    if((pos = xmpData.findKey(Exiv2::XmpKey("Xmp.darktable.raw_params"))) != xmpData.end())
    {
      union {
          int32_t in;
          dt_image_raw_parameters_t out;
      } raw_params;
      raw_params.in = pos->toLong();
      const int32_t user_flip = raw_params.out.user_flip;
      img->legacy_flip.user_flip = user_flip;
      img->legacy_flip.legacy = 0;
    }

    int32_t preset_applied = 0;

    if((pos = xmpData.findKey(Exiv2::XmpKey("Xmp.darktable.auto_presets_applied")))
       != xmpData.end())
    {
      preset_applied = pos->toLong();

      // In any case, this is no legacy image.
      img->flags |= DT_IMAGE_NO_LEGACY_PRESETS;
    }
    else if(xmpData.findKey(Exiv2::XmpKey("Xmp.darktable.xmp_version")) == xmpData.end())
    {
      // If there is no darktable xmp_version in the XMP, this XMP
      // must have been generated by another program; since this is
      // the first time darktable sees it, there can't be legacy
      // presets.
      img->flags |= DT_IMAGE_NO_LEGACY_PRESETS;
    }
    else
    {
      // So we are legacy (thus have to clear the no-legacy flag)
      img->flags &= ~DT_IMAGE_NO_LEGACY_PRESETS;
    }
    // When we are reading the XMP data it doesn't make sense to flag
    // the image as removed
    img->flags &= ~DT_IMAGE_REMOVE;

    if(xmp_version == 4 || xmp_version == 5)
    {
      if((pos = xmpData.findKey(Exiv2::XmpKey("Xmp.darktable.iop_order_version")))
         != xmpData.end())
      {
        iop_order_version = (dt_iop_order_t)pos->toLong();
      }

      if((pos = xmpData.findKey(Exiv2::XmpKey("Xmp.darktable.iop_order_list")))
         != xmpData.end())
      {
        iop_order_list = dt_ioppr_deserialize_text_iop_order_list(pos->toString().c_str());
      }
      else
        iop_order_list = dt_ioppr_get_iop_order_list_version(iop_order_version);
    }
    else if(xmp_version == 3)
    {
      iop_order_version = DT_IOP_ORDER_LEGACY;

      if((pos = xmpData.findKey(Exiv2::XmpKey("Xmp.darktable.iop_order_version")))
         != xmpData.end())
      {
        //  All iop-order version before 3 are legacy one. Starting
        //  with version 3 we have the first attempts to propose the
        //  final v3 iop-order.
        iop_order_version = pos->toLong() < 3
          ? DT_IOP_ORDER_LEGACY
          : DT_DEFAULT_IOP_ORDER_RAW;

        iop_order_list = dt_ioppr_get_iop_order_list_version(iop_order_version);
      }
      else
        iop_order_list = dt_ioppr_get_iop_order_list_version(DT_IOP_ORDER_LEGACY);
    }
    else
    {
      iop_order_version = DT_IOP_ORDER_LEGACY;
      iop_order_list = dt_ioppr_get_iop_order_list_version(DT_IOP_ORDER_LEGACY);
    }

    // masks
    GHashTable *mask_entries = NULL;
    GList *mask_entries_v3 = NULL;

    // Clean all old masks for this image
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "DELETE FROM main.masks_history WHERE imgid = ?1", -1,
                                &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, img->id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    // Read the masks from the file first so we can add them to the db
    // while reading history entries.
    if(xmp_version < 3)
      mask_entries = _read_masks(xmpData, filename, xmp_version);
    else
      mask_entries_v3 = _read_masks_v3(xmpData, filename, xmp_version);

    // Now add all masks that are not used for cloning. Keeping them might be useful.
    // TODO: Make this configurable? or remove it altogether?
    dt_database_start_transaction(darktable.db);

    if(xmp_version < 3)
    {
      g_hash_table_foreach(mask_entries, _add_non_clone_mask_entries_to_db, &img->id);
    }
    else
    {
      for(GList *m_entries = g_list_first(mask_entries_v3);
          m_entries;
          m_entries = g_list_next(m_entries))
      {
        mask_entry_t *mask_entry = (mask_entry_t *)m_entries->data;

        _add_mask_entry_to_db(img->id, mask_entry);
      }
    }

    dt_database_release_transaction(darktable.db);

    // History
    int num = 0;
    gboolean all_ok = TRUE;
    GList *history_entries = NULL;
    gboolean has_highlights = FALSE;
    gboolean has_rawprepare = FALSE;
    int add_to_history_end = 0;

    if(xmp_version < 2)
    {
      std::string &xmpPacket = image->xmpPacket();
      history_entries = _read_history_v1(xmpPacket, filename, 0);
      if(!history_entries) // didn't work? try super old version with rdf:Bag
        history_entries = _read_history_v1(xmpPacket, filename, 1);
    }
    else if(xmp_version == 2
            || xmp_version == 3
            || xmp_version == 4
            || xmp_version == 5)
    {
      history_entries = _read_history_v2(xmpData, filename);
    }
    else
    {
      dt_print(DT_DEBUG_IMAGEIO,
               "error: XMP schema version %d in '%s' not supported",
               xmp_version,
               filename);
      g_hash_table_destroy(mask_entries);
      return TRUE;
    }

    dt_database_start_transaction(darktable.db);

    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "DELETE FROM main.history WHERE imgid = ?1", -1,
                                &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, img->id);
    if(sqlite3_step(stmt) != SQLITE_DONE)
    {
      dt_print(DT_DEBUG_ALWAYS,
               "[exif] error deleting history for image %d", img->id);
      dt_print(DT_DEBUG_ALWAYS,
               "[exif]   %s", sqlite3_errmsg(dt_database_get(darktable.db)));
      all_ok = FALSE;
      goto end;
    }
    sqlite3_finalize(stmt);

    if(xmp_version < 5)
    {
      /*
      Check if the XMP has the now default enabled highlights module.

      If not we want to add this module but with the CLIP method instead of
      the new default OPPOSED one. This is to keep compatibility with old-edits.

      Starting with xmp_version = 5 all modules are always recorded, so we don't
      want to check for this.
      */

      for(GList *iter = history_entries; iter; iter = g_list_next(iter))
        {
          history_entry_t *entry = (history_entry_t *)iter->data;

          if(!strcmp(entry->operation, "highlights"))
            has_highlights = TRUE;
          else if(!strcmp(entry->operation, "rawprepare"))
            has_rawprepare = TRUE;
        }

      // Module highlights is not part of history, add a simple CLIP method
      if(has_rawprepare && !has_highlights)
        {
          // The following lines are Exif encoded parameters. The parameters from
          // iop dt_iop_highlights_params_t are taken as raw bytes and encoded to
          // be stored into the XMP. We have captured here:
          //   - The default CLIP parameters (only the method being set to
          //     DT_IOP_HIGHLIGHTS_CLIP and the clip field are actually needed)
          //     for version 4 of highlights.
          //   - The default blend parameters (no blending activated) for
          //     version 13 of dt_develop_blend_params_t.

          const char *default_clip =
            "000000000000803f00000000000000000000803f00000000"
            "1e00000006000000cdcccc3e000000400000000000000000";
          const char *no_blend = "gz11eJxjYGBgkGAAgRNODGiAEV0AJ2iwh+CRyscOAAdeGQQ=";

          history_entry_t *entry = (history_entry_t*)calloc(1, sizeof(history_entry_t));
	  if(entry)
	  {
            entry->operation = g_strdup("highlights");
	    entry->enabled = TRUE;
	    entry->modversion = 4;
	    entry->params = dt_exif_xmp_decode(default_clip,
	                                       strlen(default_clip),
	                                       &entry->params_len);
	    entry->multi_name = g_strdup("");
	    entry->multi_name_hand_edited = FALSE;
	    entry->multi_priority = 0;
	    entry->blendop_version = 13;
	    entry->blendop_params = dt_exif_xmp_decode(no_blend,
	                                               strlen(no_blend),
	                                               &entry->blendop_params_len);
	    // We insert the module in second position, just after rawprepare.
	    // This is to ensure that history_end do include this module.
	    // Just adding one to history_end won't work if the history_end
	    // is not pointing to the last history item.
	    entry->num = 1;
	    entry->iop_order = -1;

	    add_to_history_end++;
	    history_entries = g_list_append(history_entries, entry);
	  }
        }
    }

    // clang-format off
    DT_DEBUG_SQLITE3_PREPARE_V2
      (dt_database_get(darktable.db),
       "INSERT INTO main.history"
       " (imgid, num, module, operation, op_params, enabled,"
       "  blendop_params, blendop_version, multi_priority,"
       "  multi_name, multi_name_hand_edited)"
       " VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11)", -1, &stmt, NULL);
    // clang-format on

    for(GList *iter = history_entries; iter; iter = g_list_next(iter))
    {
      history_entry_t *entry = (history_entry_t *)iter->data;

      DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, img->id);
      if(xmp_version < 3)
      {
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, num);
      }
      else
      {
        DT_DEBUG_SQLITE3_BIND_INT
          (stmt, 2,
           entry->num
           + (entry->num > 1
              || (entry->num == 1 && strcmp(entry->operation, "highlights"))
              ? add_to_history_end
              : 0));
      }
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, entry->modversion);
      DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 4, entry->operation, -1, SQLITE_TRANSIENT);
      DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 5, entry->params,
                                 entry->params_len, SQLITE_TRANSIENT);
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 6, entry->enabled);
      if(entry->blendop_params)
      {
        DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 7, entry->blendop_params,
                                   entry->blendop_params_len, SQLITE_TRANSIENT);

        if(xmp_version < 3)
        {
          // Check what mask entries belong to this iop and add them to the db
          const dt_develop_blend_params_t *blendop_params =
            (dt_develop_blend_params_t *)entry->blendop_params;
          _add_mask_entries_to_db(img->id, mask_entries, blendop_params->mask_id);
        }
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
        DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 10, "", -1, SQLITE_TRANSIENT);
      }
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 11, entry->multi_name_hand_edited);

      if(sqlite3_step(stmt) != SQLITE_DONE)
      {
        dt_print(DT_DEBUG_ALWAYS,
                 "[exif] error adding history entry for image %d", img->id);
        dt_print(DT_DEBUG_ALWAYS,
                 "[exif]   %s", sqlite3_errmsg(dt_database_get(darktable.db)));
        all_ok = FALSE;
        goto end;
      }
      sqlite3_reset(stmt);
      sqlite3_clear_bindings(stmt);

      num++;
    }
    sqlite3_finalize(stmt);

    // We now need to create and store the proper iop-order taking
    // into account all multi-instances for previous xmp versions.

    if(xmp_version < 4)
    {
      // In this version we had iop-order, use it

      for(GList *iter = history_entries; iter; iter = g_list_next(iter))
      {
        history_entry_t *entry = (history_entry_t *)iter->data;

        dt_iop_order_entry_t *e = (dt_iop_order_entry_t *)malloc(sizeof(dt_iop_order_entry_t));
        memcpy(e->operation, entry->operation, sizeof(e->operation));
        e->instance = entry->multi_priority;

        if(xmp_version < 3)
        {
          // Prior to v3 there was no iop-order, all multi instances
          // where grouped, use the multi_priority to restore the
          // order.
          GList *base_order =
            dt_ioppr_get_iop_order_link(iop_order_list, entry->operation, -1);

          if(base_order)
            e->o.iop_order_f = ((dt_iop_order_entry_t *)(base_order->data))->o.iop_order_f
              - entry->multi_priority / 100.0f;
          else
          {
            dt_print(DT_DEBUG_ALWAYS,
                     "[exif] cannot get iop-order for module '%s', XMP may be corrupted",
                     entry->operation);
            g_list_free_full(iop_order_list, free);
            g_list_free_full(history_entries, _free_history_entry);
            g_list_free_full(mask_entries_v3, _free_mask_entry);
            if(mask_entries) g_hash_table_destroy(mask_entries);
            g_free(e);
            return TRUE;
          }
        }
        else
        {
          // Otherwise use the iop_order for the entry
          e->o.iop_order_f = entry->iop_order; // legacy iop-order is
                                               // used to insert item
                                               // at the right
                                               // location
        }

        // Remove a current entry from the iop-order list if found as
        // it will be replaced, possibly with another iop-order with a
        // new item in the history.

        GList *link =
          dt_ioppr_get_iop_order_link(iop_order_list, e->operation, e->instance);
        if(link)
          iop_order_list = g_list_delete_link(iop_order_list, link);

        iop_order_list = g_list_append(iop_order_list, e);
      }

      // And finally reorder the full list based on the iop-order.
      iop_order_list = g_list_sort(iop_order_list, dt_sort_iop_list_by_order_f);
    }

    // If masks have been read, create a mask manager entry in history.
    if(xmp_version < 3)
    {
      DT_DEBUG_SQLITE3_PREPARE_V2
        (dt_database_get(darktable.db),
         "SELECT COUNT(*) FROM main.masks_history WHERE imgid = ?1", -1,
         &stmt, NULL);
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, img->id);

      if(sqlite3_step(stmt) == SQLITE_ROW)
        num_masks = sqlite3_column_int(stmt, 0);
      sqlite3_finalize(stmt);

      if(num_masks > 0)
      {
        // Make room for mask_manager entry
        DT_DEBUG_SQLITE3_PREPARE_V2
          (dt_database_get(darktable.db),
           "UPDATE main.history SET num = num + 1 WHERE imgid = ?1", -1,
           &stmt, NULL);
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, img->id);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        // Insert mask_manager entry
        // clang-format off
        DT_DEBUG_SQLITE3_PREPARE_V2
          (dt_database_get(darktable.db),
           "INSERT INTO main.history"
           " (imgid, num, module, operation, op_params, enabled, "
           "  blendop_params, blendop_version, multi_priority, multi_name) "
           "VALUES"
           " (?1, 0, 1, 'mask_manager', NULL, 0, NULL, 0, 0, '')",
           -1, &stmt, NULL);
        // clang-format on
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, img->id);

        if(sqlite3_step(stmt) != SQLITE_DONE)
        {
          dt_print(DT_DEBUG_ALWAYS,
                   "[exif] error adding mask history entry for image %d", img->id);
          dt_print(DT_DEBUG_ALWAYS,
                   "[exif]   %s", sqlite3_errmsg(dt_database_get(darktable.db)));
          all_ok = FALSE;
          goto end;
        }
        sqlite3_finalize(stmt);

        num++;
      }
    }

    // We shouldn't change history_end when no history was read!
    if((pos = xmpData.findKey(Exiv2::XmpKey("Xmp.darktable.history_end")))
       != xmpData.end() && num > 0)
    {
      stmt = NULL;

      int history_end = MIN(pos->toLong(), num) + add_to_history_end;
      if(num_masks > 0)
        history_end++;

      if((history_end < 1) && preset_applied)
        preset_applied = -1;

      const gboolean ok = dt_image_set_history_end(img->id, history_end);

      if(!ok)
      {
        dt_print(DT_DEBUG_ALWAYS,
                 "[exif] error writing history_end for image %d", img->id);
        dt_print(DT_DEBUG_ALWAYS,
                 "[exif]   %s", sqlite3_errmsg(dt_database_get(darktable.db)));
        all_ok = FALSE;
        goto end;
      }
    }
    else
    {
      if(preset_applied) preset_applied = -1;
      // clang-format off
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                  "UPDATE main.images "
                                  " SET history_end = (SELECT IFNULL(MAX(num) + 1, 0)"
                                  "                    FROM main.history"
                                  "                    WHERE imgid = ?1)"
                                  " WHERE id = ?1", -1,
                                  &stmt, NULL);
      // clang-format on
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, img->id);

      if(sqlite3_step(stmt) != SQLITE_DONE)
      {
        dt_print(DT_DEBUG_ALWAYS,
                 "[exif] error writing history_end for image %d", img->id);
        dt_print(DT_DEBUG_ALWAYS,
                 "[exif]   %s", sqlite3_errmsg(dt_database_get(darktable.db)));
        all_ok = FALSE;
        goto end;
      }
    }
    if(!dt_ioppr_write_iop_order_list(iop_order_list, img->id))
    {
      dt_print(DT_DEBUG_ALWAYS,
               "[exif] error writing iop_list for image %d", img->id);
      dt_print(DT_DEBUG_ALWAYS,
               "[exif]   %s", sqlite3_errmsg(dt_database_get(darktable.db)));
      all_ok = FALSE;
      goto end;
    }

  end:

    _read_xmp_timestamps(xmpData, img, xmp_version);

    _read_xmp_harmony_guide(xmpData, img, xmp_version);

    if(stmt)
      sqlite3_finalize(stmt);

    // Set or clear bit in image struct. ONLY set if the
    // Xmp.darktable.auto_presets_applied was 1 AND there was a
    // history in xmp.
    if(preset_applied > 0)
    {
      img->flags |= DT_IMAGE_AUTO_PRESETS_APPLIED;
    }
    else
    {
      // Not found for old or buggy xmp where it was found but history was 0.
      img->flags &= ~DT_IMAGE_AUTO_PRESETS_APPLIED;

      if(preset_applied < 0)
      {
        dt_print(DT_DEBUG_ALWAYS,
                 "[exif] dt_exif_xmp_read for %s, id %i found auto_presets_applied"
                 " but there was no history",
                 filename,img->id);
      }
    }

    g_list_free_full(iop_order_list, free);
    g_list_free_full(history_entries, _free_history_entry);
    g_list_free_full(mask_entries_v3, _free_mask_entry);
    if(mask_entries) g_hash_table_destroy(mask_entries);

    if(all_ok)
    {
      dt_database_release_transaction(darktable.db);

      // history_hash
      dt_history_hash_values_t hash = {NULL, 0, NULL, 0, NULL, 0};
      if((pos = xmpData.findKey(Exiv2::XmpKey("Xmp.darktable.history_basic_hash")))
         != xmpData.end())
      {
        hash.basic = dt_exif_xmp_decode(pos->toString().c_str(),
                                        strlen(pos->toString().c_str()),
                                        &hash.basic_len);
      }
      if((pos = xmpData.findKey(Exiv2::XmpKey("Xmp.darktable.history_auto_hash")))
         != xmpData.end())
      {
        hash.auto_apply = dt_exif_xmp_decode(pos->toString().c_str(),
                                             strlen(pos->toString().c_str()),
                                             &hash.auto_apply_len);
      }
      if((pos = xmpData.findKey(Exiv2::XmpKey("Xmp.darktable.history_current_hash")))
         != xmpData.end())
      {
        hash.current = dt_exif_xmp_decode(pos->toString().c_str(),
                                          strlen(pos->toString().c_str()),
                                          &hash.current_len);
      }
      if(hash.basic || hash.auto_apply || hash.current)
      {
        dt_history_hash_write(img->id, &hash);
      }
      else
      {
        // No choice, use the history itself applying the former rules.
        dt_history_hash_t hash_flag = DT_HISTORY_HASH_CURRENT;
        if(!_image_altered_deprecated(img->id))
          // We assume the image has an history
          hash_flag = (dt_history_hash_t)(hash_flag | DT_HISTORY_HASH_BASIC);
        dt_history_hash_write_from_history(img->id, hash_flag);
      }
    }
    else
    {
      dt_print(DT_DEBUG_IMAGEIO,
               "[exif] error reading history from '%s'",
               filename);
      dt_database_rollback_transaction(darktable.db);
      return TRUE;
    }

  }
  catch(Exiv2::AnyError &e)
  {
    // Actually nobody's interested in that if the file doesn't exist:
    // std::string s(e.what());
    // std::cerr << "[exiv2] " << filename << ": " << s << std::endl;
    return TRUE;
  }
  return FALSE;
}

// add history metadata to XmpData
static void _set_xmp_dt_history(Exiv2::XmpData &xmpData,
                                const dt_imgid_t imgid,
                                int history_end)
{
  sqlite3_stmt *stmt;

  // Masks:
  char key[1024];

  // Masks history:
  int num = 1;

  // Create an array:
  Exiv2::XmpTextValue tvm("");
  tvm.setXmpArrayType(Exiv2::XmpValue::xaSeq);
  xmpData.add(Exiv2::XmpKey("Xmp.darktable.masks_history"), &tvm);
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get(darktable.db),
      "SELECT imgid, formid, form, name, version, points, points_count, source, num"
      " FROM main.masks_history"
      " WHERE imgid = ?1"
      " ORDER BY num",
      -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const int32_t mask_num = sqlite3_column_int(stmt, 8);
    const int32_t mask_id = sqlite3_column_int(stmt, 1);
    const int32_t mask_type = sqlite3_column_int(stmt, 2);
    const char *mask_name = (const char *)sqlite3_column_text(stmt, 3);
    const int32_t mask_version = sqlite3_column_int(stmt, 4);
    int32_t len = sqlite3_column_bytes(stmt, 5);
    char *mask_d =
      dt_exif_xmp_encode((const unsigned char *)sqlite3_column_blob(stmt, 5), len, NULL);
    const int32_t mask_nb = sqlite3_column_int(stmt, 6);
    len = sqlite3_column_bytes(stmt, 7);
    char *mask_src =
      dt_exif_xmp_encode((const unsigned char *)sqlite3_column_blob(stmt, 7), len, NULL);

    snprintf(key, sizeof(key),
             "Xmp.darktable.masks_history[%d]/darktable:mask_num", num);
    xmpData[key] = mask_num;
    snprintf(key, sizeof(key),
             "Xmp.darktable.masks_history[%d]/darktable:mask_id", num);
    xmpData[key] = mask_id;
    snprintf(key, sizeof(key),
             "Xmp.darktable.masks_history[%d]/darktable:mask_type", num);
    xmpData[key] = mask_type;
    snprintf(key, sizeof(key),
             "Xmp.darktable.masks_history[%d]/darktable:mask_name", num);
    xmpData[key] = mask_name;
    snprintf(key, sizeof(key),
             "Xmp.darktable.masks_history[%d]/darktable:mask_version", num);
    xmpData[key] = mask_version;
    snprintf(key, sizeof(key),
             "Xmp.darktable.masks_history[%d]/darktable:mask_points", num);
    xmpData[key] = mask_d;
    snprintf(key, sizeof(key),
             "Xmp.darktable.masks_history[%d]/darktable:mask_nb", num);
    xmpData[key] = mask_nb;
    snprintf(key, sizeof(key),
             "Xmp.darktable.masks_history[%d]/darktable:mask_src", num);
    xmpData[key] = mask_src;

    free(mask_d);
    free(mask_src);

    num++;
  }
  sqlite3_finalize(stmt);

  // History stack:
  num = 1;

  // Create an array:
  Exiv2::XmpTextValue tv("");
  tv.setXmpArrayType(Exiv2::XmpValue::xaSeq);
  xmpData.add(Exiv2::XmpKey("Xmp.darktable.history"), &tv);
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get(darktable.db),
      "SELECT module, operation, op_params, enabled, blendop_params, "
      "       blendop_version, multi_priority, multi_name, num, multi_name_hand_edited"
      " FROM main.history"
      " WHERE imgid = ?1"
      " ORDER BY num",
      -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const int32_t modversion = sqlite3_column_int(stmt, 0);
    const char *operation = (const char *)sqlite3_column_text(stmt, 1);
    const int32_t params_len = sqlite3_column_bytes(stmt, 2);
    const void *params_blob = sqlite3_column_blob(stmt, 2);
    const int32_t enabled = sqlite3_column_int(stmt, 3);
    const void *blendop_blob = sqlite3_column_blob(stmt, 4);
    const int32_t blendop_params_len = sqlite3_column_bytes(stmt, 4);
    const int32_t blendop_version = sqlite3_column_int(stmt, 5);
    const int32_t multi_priority = sqlite3_column_int(stmt, 6);
    const char *multi_name = (const char *)sqlite3_column_text(stmt, 7);
    const int32_t hist_num = sqlite3_column_int(stmt, 8);
    const int32_t multi_name_hand_edited = sqlite3_column_int(stmt, 9);

    if(!operation) continue; // no op is fatal.

    char *params = dt_exif_xmp_encode((const unsigned char *)params_blob, params_len, NULL);

    snprintf(key, sizeof(key),
             "Xmp.darktable.history[%d]/darktable:num", num);
    xmpData[key] = hist_num;
    snprintf(key, sizeof(key),
             "Xmp.darktable.history[%d]/darktable:operation", num);
    xmpData[key] = operation;
    snprintf(key, sizeof(key),
             "Xmp.darktable.history[%d]/darktable:enabled", num);
    xmpData[key] = enabled;
    snprintf(key, sizeof(key),
             "Xmp.darktable.history[%d]/darktable:modversion", num);
    xmpData[key] = modversion;
    snprintf(key, sizeof(key),
             "Xmp.darktable.history[%d]/darktable:params", num);
    xmpData[key] = params;
    snprintf(key, sizeof(key),
             "Xmp.darktable.history[%d]/darktable:multi_name", num);
    xmpData[key] = multi_name ? multi_name : "";
    snprintf(key, sizeof(key),
             "Xmp.darktable.history[%d]/darktable:multi_name_hand_edited", num);
    xmpData[key] = multi_name_hand_edited;
    snprintf(key, sizeof(key), "Xmp.darktable.history[%d]/darktable:multi_priority", num);
    xmpData[key] = multi_priority;

    if(blendop_blob)
    {
      // This shouldn't fail in general, but reading is robust enough to allow it,
      // and flipping images from LT will result in this being left out.
      char *blendop_params =
        dt_exif_xmp_encode((const unsigned char *)blendop_blob, blendop_params_len, NULL);
      snprintf(key, sizeof(key),
               "Xmp.darktable.history[%d]/darktable:blendop_version", num);
      xmpData[key] = blendop_version;
      snprintf(key, sizeof(key),
               "Xmp.darktable.history[%d]/darktable:blendop_params", num);
      xmpData[key] = blendop_params;
      free(blendop_params);
    }

    free(params);

    num++;
  }

  sqlite3_finalize(stmt);
  if(history_end == -1)
    history_end = num - 1;
  else
    history_end = MIN(history_end, num - 1); // safeguard for some old buggy libraries

  xmpData["Xmp.darktable.history_end"] = history_end;
}

// Add timestamps to XmpData.
static void _set_xmp_timestamps(Exiv2::XmpData &xmpData,
                                const dt_imgid_t imgid)
{
  static const char *keys[] =
  {
    "Xmp.darktable.import_timestamp",
    "Xmp.darktable.change_timestamp",
    "Xmp.darktable.export_timestamp",
    "Xmp.darktable.print_timestamp"
  };
  static const guint n_keys = G_N_ELEMENTS(keys);
  _remove_xmp_keys(xmpData, keys, n_keys);

  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get(darktable.db),
      "SELECT import_timestamp, change_timestamp, export_timestamp, print_timestamp"
      " FROM main.images"
      " WHERE id = ?1",
      -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  // clang-format on

  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    if(sqlite3_column_type(stmt, 0) != SQLITE_NULL)
      xmpData["Xmp.darktable.import_timestamp"] = sqlite3_column_int64(stmt, 0);
    if(sqlite3_column_type(stmt, 1) != SQLITE_NULL)
      xmpData["Xmp.darktable.change_timestamp"] = sqlite3_column_int64(stmt, 1);
    if(sqlite3_column_type(stmt, 2) != SQLITE_NULL)
      xmpData["Xmp.darktable.export_timestamp"] = sqlite3_column_int64(stmt, 2);
    if(sqlite3_column_type(stmt, 3) != SQLITE_NULL)
      xmpData["Xmp.darktable.print_timestamp"] = sqlite3_column_int64(stmt, 3);
  }
  sqlite3_finalize(stmt);
}

static void _set_xmp_harmony_guide(Exiv2::XmpData &xmpData,
                                   const dt_imgid_t imgid)
{
  static const char *keys[] =
  {
    "Xmp.darktable.harmony_guide_type",
    "Xmp.darktable.harmony_guide_rotation",
    "Xmp.darktable.harmony_guide_width",
  };
  static const guint n_keys = G_N_ELEMENTS(keys);
  _remove_xmp_keys(xmpData, keys, n_keys);

  dt_color_harmony_guide_t guide;

  if(dt_color_harmony_get(imgid, &guide))
  {
    xmpData["Xmp.darktable.harmony_guide_type"] = guide.type;
    xmpData["Xmp.darktable.harmony_guide_rotation"] = guide.rotation;
    xmpData["Xmp.darktable.harmony_guide_width"] = guide.width;
  }
}

GTimeSpan _convert_unix_to_gtimespan(const time_t unix)
{
  GDateTime *gdt = g_date_time_new_from_unix_utc(unix);
  if(gdt)
  {
    GTimeSpan gts = dt_datetime_gdatetime_to_gtimespan(gdt);
    g_date_time_unref(gdt);
    return gts;
  }
  return 0;
}

// Read timestamps from XmpData
void _read_xmp_timestamps(Exiv2::XmpData &xmpData,
                          dt_image_t *img,
                          const int xmp_version)
{
  Exiv2::XmpData::iterator pos;

  // Do not read for import_ts. It must be updated at each import.
  if((pos = xmpData.findKey(Exiv2::XmpKey("Xmp.darktable.change_timestamp")))
     != xmpData.end())
  {
    if(xmp_version > 5)
      img->change_timestamp = pos->toLong();
    else if(pos->toLong() >= 1)
      img->change_timestamp = _convert_unix_to_gtimespan(pos->toLong());
  }
  if((pos = xmpData.findKey(Exiv2::XmpKey("Xmp.darktable.export_timestamp")))
     != xmpData.end())
  {
    if(xmp_version > 5)
      img->export_timestamp = pos->toLong();
    else if(pos->toLong() >= 1)
      img->export_timestamp = _convert_unix_to_gtimespan(pos->toLong());
  }
  if((pos = xmpData.findKey(Exiv2::XmpKey("Xmp.darktable.print_timestamp")))
     != xmpData.end())
  {
    if(xmp_version > 5)
      img->print_timestamp = pos->toLong();
    else if(pos->toLong() >= 1)
      img->print_timestamp = _convert_unix_to_gtimespan(pos->toLong());
  }
}

// Read color harmony guides from XmpData
void _read_xmp_harmony_guide(Exiv2::XmpData &xmpData,
                             dt_image_t *img,
                             const int xmp_version)
{
  Exiv2::XmpData::iterator pos;

  if((pos = xmpData.findKey(Exiv2::XmpKey("Xmp.darktable.harmony_guide_type")))
     != xmpData.end())
  {
    img->color_harmony_guide.type = (dt_color_harmony_type_t)pos->toLong();
  }
  if((pos = xmpData.findKey(Exiv2::XmpKey("Xmp.darktable.harmony_guide_rotation")))
     != xmpData.end())
  {
    img->color_harmony_guide.rotation = (int) pos->toLong();
  }
  if((pos = xmpData.findKey(Exiv2::XmpKey("Xmp.darktable.harmony_guide_width")))
     != xmpData.end())
  {
      img->color_harmony_guide.width = (dt_color_harmony_width_t)pos->toLong();
  }
}

static void _remove_xmp_exif_geotag(Exiv2::XmpData &xmpData)
{
  static const char *keys[] =
  {
    "Xmp.exif.GPSVersionID",
    "Xmp.exif.GPSLongitude",
    "Xmp.exif.GPSLatitude",
    "Xmp.exif.GPSAltitudeRef",
    "Xmp.exif.GPSAltitude"
  };
  static const guint n_keys = G_N_ELEMENTS(keys);
  _remove_xmp_keys(xmpData, keys, n_keys);
}

static void _set_xmp_exif_geotag(Exiv2::XmpData &xmpData,
                                 double longitude,
                                 double latitude,
                                 const double altitude)
{
  _remove_xmp_exif_geotag(xmpData);

  if(!std::isnan(longitude) && !std::isnan(latitude))
  {
    char long_dir = 'E';
    char lat_dir = 'N';

    if(longitude < 0)
      long_dir = 'W';
    if(latitude < 0)
      lat_dir = 'S';

    longitude = fabs(longitude);
    latitude = fabs(latitude);

    const int long_deg = (int)floor(longitude);
    const int lat_deg = (int)floor(latitude);
    const double long_min = (longitude - (double)long_deg) * 60.0;
    const double lat_min = (latitude - (double)lat_deg) * 60.0;

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
}

static void _set_xmp_dt_metadata(Exiv2::XmpData &xmpData,
                                 const dt_imgid_t imgid,
                                 const gboolean export_flag)
{
  sqlite3_stmt *stmt;
  // Metadata
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT key, value FROM main.meta_data WHERE id = ?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    int keyid = sqlite3_column_int(stmt, 0);
    if(export_flag
       && (dt_metadata_get_type(keyid) != DT_METADATA_TYPE_INTERNAL))
    {
      const gchar *name = dt_metadata_get_name(keyid);
      gchar *setting = g_strdup_printf("plugins/lighttable/metadata/%s_flag", name);
      const uint32_t flag =  dt_conf_get_int(setting);
      g_free(setting);
      if(!(flag & (DT_METADATA_FLAG_PRIVATE | DT_METADATA_FLAG_HIDDEN)))
        xmpData[dt_metadata_get_key(keyid)] = sqlite3_column_text(stmt, 1);
    }
    else
      xmpData[dt_metadata_get_key(keyid)] = sqlite3_column_text(stmt, 1);
  }
  sqlite3_finalize(stmt);

  // Color labels
  char val[2048];
  std::unique_ptr<Exiv2::Value> v(Exiv2::Value::create(Exiv2::xmpSeq));
  // or xmpBag or xmpAlt.

  /* Already initialized v = Exiv2::Value::create(Exiv2::xmpSeq); // or xmpBag or xmpAlt.*/
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT color FROM main.color_labels WHERE imgid=?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    snprintf(val, sizeof(val), "%d", sqlite3_column_int(stmt, 0));
    v->read(val);
  }
  sqlite3_finalize(stmt);
  if(v->count() > 0) xmpData.add(Exiv2::XmpKey("Xmp.darktable.colorlabels"), v.get());
}

// Helper to create an xmp data thing. Throws Exiv2 exceptions if
// stuff goes wrong.
static void _exif_xmp_read_data(Exiv2::XmpData &xmpData,
                                const dt_imgid_t imgid,
                                const char *info)
{
  const double start = dt_get_debug_wtime();
  const int xmp_version = DT_XMP_EXIF_VERSION;
  int stars = 1, raw_params = 0, history_end = -1;
  double longitude = NAN, latitude = NAN, altitude = NAN;
  gchar *filename = NULL;
  gchar *iop_order_list = NULL;
  GTimeSpan gts = 0;

  // Get stars and raw params from db
  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2
    (dt_database_get(darktable.db),
     "SELECT filename, flags, raw_parameters, "
     "       longitude, latitude, altitude, history_end, datetime_taken"
     " FROM main.images"
     " WHERE id = ?1",
     -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    filename = (gchar *)sqlite3_column_text(stmt, 0);
    stars = sqlite3_column_int(stmt, 1);
    raw_params = sqlite3_column_int(stmt, 2);
    if(sqlite3_column_type(stmt, 3) == SQLITE_FLOAT)
      longitude = sqlite3_column_double(stmt, 3);
    if(sqlite3_column_type(stmt, 4) == SQLITE_FLOAT)
      latitude = sqlite3_column_double(stmt, 4);
    if(sqlite3_column_type(stmt, 5) == SQLITE_FLOAT)
      altitude = sqlite3_column_double(stmt, 5);
    history_end = sqlite3_column_int(stmt, 6);
    gts = sqlite3_column_int64(stmt, 7);
  }

  // Get iop-order list
  const dt_iop_order_t iop_order_version = dt_ioppr_get_iop_order_version(imgid);
  GList *iop_list = dt_ioppr_get_iop_order_list(imgid, TRUE);

  if(iop_order_version == DT_IOP_ORDER_CUSTOM
     || dt_ioppr_has_multiple_instances(iop_list))
  {
    iop_order_list = dt_ioppr_serialize_text_iop_order_list(iop_list);
  }
  g_list_free_full(iop_list, free);

  // Store datetime_taken as DateTimeOriginal to take into account the
  // user's selected date/time.
  gchar exif_datetime[DT_DATETIME_LENGTH];
  dt_datetime_gtimespan_to_exif(exif_datetime, sizeof(exif_datetime), gts);
  xmpData["Xmp.exif.DateTimeOriginal"] = exif_datetime;

  // We have to erase the old ratings first as Exiv2 seems to not change it otherwise.
  Exiv2::XmpData::iterator pos = xmpData.findKey(Exiv2::XmpKey("Xmp.xmp.Rating"));
  if(pos != xmpData.end()) xmpData.erase(pos);
  xmpData["Xmp.xmp.Rating"] = dt_image_get_xmp_rating_from_flags(stars);

  // The original file name
  if(filename) xmpData["Xmp.xmpMM.DerivedFrom"] = filename;

  // Timestamps
  _set_xmp_timestamps(xmpData, imgid);

  _set_xmp_harmony_guide(xmpData, imgid);

  // GPS data
  _set_xmp_exif_geotag(xmpData, longitude, latitude, altitude);

  // The metadata
  _set_xmp_dt_metadata(xmpData, imgid, FALSE);

  // Get tags from db, store in Dublin Core
  std::unique_ptr<Exiv2::Value> v1(Exiv2::Value::create(Exiv2::xmpBag));

  std::unique_ptr<Exiv2::Value> v2(Exiv2::Value::create(Exiv2::xmpBag));

  GList *tags = dt_tag_get_list(imgid);
  for(GList *tag = tags; tag; tag = g_list_next(tag))
  {
    v1->read((char *)tag->data);
  }
  if(v1->count() > 0)
    xmpData.add(Exiv2::XmpKey("Xmp.dc.subject"), v1.get());
  g_list_free_full(tags, g_free);

  GList *hierarchical = dt_tag_get_hierarchical(imgid);
  for(GList *hier = hierarchical; hier; hier = g_list_next(hier))
  {
    v2->read((char *)hier->data);
  }
  if(v2->count() > 0)
    xmpData.add(Exiv2::XmpKey("Xmp.lr.hierarchicalSubject"), v2.get());
  g_list_free_full(hierarchical, g_free);
  // TODO: Add tags to IPTC namespace as well.

  xmpData["Xmp.darktable.xmp_version"] = xmp_version;
  xmpData["Xmp.darktable.raw_params"] = raw_params;
  if(stars & DT_IMAGE_AUTO_PRESETS_APPLIED)
    xmpData["Xmp.darktable.auto_presets_applied"] = 1;
  else
    xmpData["Xmp.darktable.auto_presets_applied"] = 0;
  _set_xmp_dt_history(xmpData, imgid, history_end);

  // We need to read the iop-order list
  xmpData["Xmp.darktable.iop_order_version"] = iop_order_version;
  if(iop_order_list)
    xmpData["Xmp.darktable.iop_order_list"] = iop_order_list;

  sqlite3_finalize(stmt);
  g_free(iop_order_list);

  // Store history hash
  dt_history_hash_values_t hash;
  dt_history_hash_read(imgid, &hash);

  if(hash.basic)
  {
    char* value = dt_exif_xmp_encode(hash.basic, hash.basic_len, NULL);
    xmpData["Xmp.darktable.history_basic_hash"] = value;
    free(value);
  }

  if(hash.auto_apply)
  {
    char* value = dt_exif_xmp_encode(hash.auto_apply, hash.auto_apply_len, NULL);
    xmpData["Xmp.darktable.history_auto_hash"] = value;
    free(value);
  }

  if(hash.current)
  {
    char* value = dt_exif_xmp_encode(hash.current, hash.current_len, NULL);
    xmpData["Xmp.darktable.history_current_hash"] = value;
    free(value);
  }

  dt_history_hash_free(&hash);

  const double end = dt_get_debug_wtime();
  dt_print(DT_DEBUG_DEV,
           "exif_xmp_read_data: %s. imgid=%i, hist=%i, took %.3fs",
           info,
           imgid,
           history_end,
           end - start);
}

// Helper to create an xmp data thing. Throws Exiv2 exceptions if stuff goes wrong.
static void _exif_xmp_read_data_export(Exiv2::XmpData &xmpData,
                                       const dt_imgid_t imgid,
                                       dt_export_metadata_t *metadata)
{
  const int xmp_version = DT_XMP_EXIF_VERSION;
  int stars = 1, raw_params = 0, history_end = -1;
  double longitude = NAN, latitude = NAN, altitude = NAN;
  gchar *filename = NULL;
  GTimeSpan gts = 0;
  gchar *iop_order_list = NULL;

  // Get stars and raw params from db
  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2
    (dt_database_get(darktable.db),
     "SELECT filename, flags, raw_parameters, "
     "       longitude, latitude, altitude, history_end, datetime_taken"
     " FROM main.images"
     " WHERE id = ?1",
     -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    filename = (gchar *)sqlite3_column_text(stmt, 0);
    stars = sqlite3_column_int(stmt, 1);
    raw_params = sqlite3_column_int(stmt, 2);
    if(sqlite3_column_type(stmt, 3) == SQLITE_FLOAT)
      longitude = sqlite3_column_double(stmt, 3);
    if(sqlite3_column_type(stmt, 4) == SQLITE_FLOAT)
      latitude = sqlite3_column_double(stmt, 4);
    if(sqlite3_column_type(stmt, 5) == SQLITE_FLOAT)
      altitude = sqlite3_column_double(stmt, 5);
    history_end = sqlite3_column_int(stmt, 6);
    gts = sqlite3_column_int64(stmt, 7);
  }

  // Get iop-order list
  const dt_iop_order_t iop_order_version = dt_ioppr_get_iop_order_version(imgid);
  GList *iop_list = dt_ioppr_get_iop_order_list(imgid, TRUE);

  if(iop_order_version == DT_IOP_ORDER_CUSTOM
     || dt_ioppr_has_multiple_instances(iop_list))
  {
    iop_order_list = dt_ioppr_serialize_text_iop_order_list(iop_list);
  }
  g_list_free_full(iop_list, free);

  if(metadata->flags & DT_META_METADATA)
  {
    // Store datetime_taken as DateTimeOriginal to take into account
    // the user's selected date/time.
    if(!(metadata->flags & DT_META_EXIF))
    {
      gchar exif_datetime[DT_DATETIME_LENGTH];
      dt_datetime_gtimespan_to_exif(exif_datetime, sizeof(exif_datetime), gts);
      xmpData["Xmp.exif.DateTimeOriginal"] = exif_datetime;
    }
    // We have to erase the old ratings first as Exiv2 seems to not change it otherwise.
    Exiv2::XmpData::iterator pos = xmpData.findKey(Exiv2::XmpKey("Xmp.xmp.Rating"));
    if(pos != xmpData.end()) xmpData.erase(pos);
    xmpData["Xmp.xmp.Rating"] = dt_image_get_xmp_rating_from_flags(stars);

    // The original file name
    if(filename) xmpData["Xmp.xmpMM.DerivedFrom"] = filename;
  }

  // GPS data
  if(metadata->flags & DT_META_GEOTAG)
    _set_xmp_exif_geotag(xmpData, longitude, latitude, altitude);
  else
    _remove_xmp_exif_geotag(xmpData);


  // The metadata
  if(metadata->flags & DT_META_METADATA)
    _set_xmp_dt_metadata(xmpData, imgid, TRUE);

  // Tags
  if(metadata->flags & DT_META_TAG)
  {
    // Get tags from db, store in Dublin Core
    std::unique_ptr<Exiv2::Value> v1(Exiv2::Value::create(Exiv2::xmpBag));
    GList *tags = dt_tag_get_list_export(imgid, metadata->flags);
    for(GList *tag = tags; tag; tag = g_list_next(tag))
    {
      v1->read((char *)tag->data);
    }
    if(v1->count() > 0)
      xmpData.add(Exiv2::XmpKey("Xmp.dc.subject"), v1.get());
    g_list_free_full(tags, g_free);
  }

  if(metadata->flags & DT_META_HIERARCHICAL_TAG)
  {
    std::unique_ptr<Exiv2::Value> v2(Exiv2::Value::create(Exiv2::xmpBag));
    GList *hierarchical = dt_tag_get_hierarchical_export(imgid, metadata->flags);

    for(GList *hier = hierarchical; hier; hier = g_list_next(hier))
    {
      v2->read((char *)hier->data);
    }
    if(v2->count() > 0)
      xmpData.add(Exiv2::XmpKey("Xmp.lr.hierarchicalSubject"), v2.get());
    g_list_free_full(hierarchical, g_free);
  }

  if(metadata->flags & DT_META_DT_HISTORY)
  {
    xmpData["Xmp.darktable.xmp_version"] = xmp_version;
    xmpData["Xmp.darktable.raw_params"] = raw_params;
    if(stars & DT_IMAGE_AUTO_PRESETS_APPLIED)
      xmpData["Xmp.darktable.auto_presets_applied"] = 1;
    else
      xmpData["Xmp.darktable.auto_presets_applied"] = 0;
    _set_xmp_dt_history(xmpData, imgid, history_end);

    // We need to read the iop-order list
    xmpData["Xmp.darktable.iop_order_version"] = iop_order_version;
    if(iop_order_list) xmpData["Xmp.darktable.iop_order_list"] = iop_order_list;
  }

  sqlite3_finalize(stmt);
  g_free(iop_order_list);
}

char *dt_exif_xmp_read_string(const dt_imgid_t imgid)
{
  try
  {
    char input_filename[PATH_MAX] = { 0 };
    dt_image_full_path(imgid, input_filename, sizeof(input_filename), NULL);

    // First take over the data from the source image
    Exiv2::XmpData xmpData;
    if(g_file_test(input_filename, G_FILE_TEST_EXISTS))
    {
      std::string xmpPacket;

      Exiv2::DataBuf buf = Exiv2::readFile(WIDEN(input_filename));
#if EXIV2_TEST_VERSION(0,28,0)
      xmpPacket.assign(buf.c_str(), buf.size());
#else
      xmpPacket.assign(reinterpret_cast<char *>(buf.pData_), buf.size_);
#endif
      Exiv2::XmpParser::decode(xmpData, xmpPacket);
      // Because XmpSeq or XmpBag are added to the list, we first have to
      // remove these so that we don't end up with a string of duplicates.
      _remove_known_keys(xmpData);
    }

    // nNw add whatever we have in the sidecar XMP. This overwrites
    // stuff from the source image.
    dt_image_path_append_version(imgid, input_filename, sizeof(input_filename));
    g_strlcat(input_filename, ".xmp", sizeof(input_filename));
    if(g_file_test(input_filename, G_FILE_TEST_EXISTS))
    {
      Exiv2::XmpData sidecarXmpData;
      std::string xmpPacket;

      Exiv2::DataBuf buf = Exiv2::readFile(WIDEN(input_filename));
#if EXIV2_TEST_VERSION(0,28,0)
      xmpPacket.assign(buf.c_str(), buf.size());
#else
      xmpPacket.assign(reinterpret_cast<char *>(buf.pData_), buf.size_);
#endif
      Exiv2::XmpParser::decode(sidecarXmpData, xmpPacket);

      for(Exiv2::XmpData::const_iterator it = sidecarXmpData.begin();
          it != sidecarXmpData.end();
          ++it)
        xmpData.add(*it);
    }

    _remove_known_keys(xmpData); // is this needed?

    // Last but not least, attach what we have in DB to the XMP. In theory that should be
    // the same as what we just copied over from the sidecar file, but you never know ...
    _exif_xmp_read_data(xmpData, imgid, "dt_exif_xmp_read_string");

    // Serialize the xmp data and output the xmp packet
    std::string xmpPacket;
    if(Exiv2::XmpParser::encode(xmpPacket, xmpData,
                                Exiv2::XmpParser::useCompactFormat
                                | Exiv2::XmpParser::omitPacketWrapper) != 0)
    {
      throw Exiv2::Error(Exiv2::ErrorCode::kerErrorMessage, "[xmp_write] failed to serialize xmp data");
    }
    return g_strdup(xmpPacket.c_str());
  }
  catch(Exiv2::AnyError &e)
  {
    dt_print(DT_DEBUG_IMAGEIO,
             "[xmp_read_blob] caught exiv2 exception '%s'",
             e.what());
    return NULL;
  }
}

static void _remove_xmp_key(Exiv2::XmpData &xmp,
                            const char *key)
{
  try
  {
    Exiv2::XmpData::iterator pos = xmp.findKey(Exiv2::XmpKey(key));
    if(pos != xmp.end())
      xmp.erase(pos);
  }
  catch(Exiv2::AnyError &e)
  {
  }
}

static void _remove_xmp_keys(Exiv2::XmpData &xmpData,
                             const char *key)
{
  try
  {
    const std::string needle = key;
    for(Exiv2::XmpData::iterator i = xmpData.begin(); i != xmpData.end();)
    {
      if(i->key().compare(0, needle.length(), needle) == 0)
        i = xmpData.erase(i);
      else
        ++i;
    }
  }
  catch(Exiv2::AnyError &e)
  {
  }
}

static void _remove_exif_key(Exiv2::ExifData &exif,
                             const char *key)
{
  try
  {
    Exiv2::ExifData::iterator pos = exif.findKey(Exiv2::ExifKey(key));
    if(pos != exif.end())
      exif.erase(pos);
  }
  catch(Exiv2::AnyError &e)
  {
  }
}

static void _remove_iptc_key(Exiv2::IptcData &iptc,
                             const char *key)
{
  try
  {
    Exiv2::IptcData::iterator pos;
    while((pos = iptc.findKey(Exiv2::IptcKey(key))) != iptc.end())
      iptc.erase(pos);
  }
  catch(Exiv2::AnyError &e)
  {
  }
}

enum class RegionType {MWG, MP};

const std::pair<std::array<float, 4>, bool> getRegionNormalized(
    Exiv2::XmpData &xmp,
    const std::string &regionKey,
    RegionType regionType,
    int imgWidth,
    int imgHeight)
{
  using Exiv2::XmpData;
  using Exiv2::XmpKey;

  XmpData::const_iterator hIt;
  if((hIt = xmp.findKey(XmpKey(regionKey + "/mwg-rs:Area/stArea:h"))) == xmp.end())
  {
    return std::make_pair(std::array<float, 4>({ 0.0, 0.0, 0.0, 0.0 }), false);
  }
  const float h = hIt->toFloat() * imgHeight;

  XmpData::const_iterator wIt;
  if((wIt = xmp.findKey(XmpKey(regionKey + "/mwg-rs:Area/stArea:w"))) == xmp.end())
  {
    return std::make_pair(std::array<float, 4>({ 0.0, 0.0, 0.0, 0.0 }), false);
  }
  const float w = wIt->toFloat() * imgWidth;

  XmpData::const_iterator xIt;
  if((xIt = xmp.findKey(XmpKey(regionKey + "/mwg-rs:Area/stArea:x"))) == xmp.end())
  {
    return std::make_pair(std::array<float, 4>({ 0.0, 0.0, 0.0, 0.0 }), false);
  }
  float x = xIt->toFloat() * imgWidth;

  XmpData::const_iterator yIt;
  if((yIt = xmp.findKey(XmpKey(regionKey + "/mwg-rs:Area/stArea:y"))) == xmp.end())
  {
    return std::make_pair(std::array<float, 4>({ 0.0, 0.0, 0.0, 0.0 }), false);
  }
  float y = yIt->toFloat() * imgHeight;

  // MWG regions use (x,y) as the center of the region.
  if(regionType == RegionType::MWG)
  {
    x -= w/2;
    y -= h/2;
  }

  return std::make_pair(std::array<float, 4>({ x, y, x + w, y + h }), true);
}

/* Transform face tags using the pixelpipe transform, so areas are written
 * correctly when exporting.
 *
 * We will make several assumptions or else we wont't do nothing:
 *
 * - Xmp.mwg-rs.Regions/mwg-rs:AppliedToDimensions exists, has
 *   stDim:h, stDim:w and stDim:unit defined and the latter is set to
 *   pixels.
 * - Xmp.mwg-rs.Regions/mwg-rs:RegionList[i]./mwg-rs:Area/stArea:unit
     is set to "normalized" for every region.
 *
 * TODO: Support Xmp.MP.RegionInfo/MPRI:Regions[i]/MPReg:Rectangle tags.
 */
static void _transform_face_tags(Exiv2::XmpData &xmp,
                                 dt_develop_t *dev,
                                 dt_dev_pixelpipe_t *pipe)
{
  using Exiv2::XmpData;
  using Exiv2::XmpKey;
  using Exiv2::XmpTextValue;

  // Finish early if we didn't get the develop struct.
  if(dev == NULL || pipe == NULL)
  {
    return;
  }
  // Also finish early if we don't have face tags.
  if(xmp.findKey(XmpKey("Xmp.mwg-rs.Regions")) == xmp.end())
  {
    return;
  }

  // Global properties
  // Units should be specified
  XmpData::const_iterator unit;
  if((unit = xmp.findKey
      (XmpKey("Xmp.mwg-rs.Regions/mwg-rs:AppliedToDimensions/stDim:unit"))) == xmp.end())
  {
    return;
  }
  // We only know how to handle pixel units.
  if(unit->toString() != "pixel")
  {
    return;
  }

  XmpData::const_iterator imgHeightIt;
  if((imgHeightIt = xmp.findKey
      (XmpKey("Xmp.mwg-rs.Regions/mwg-rs:AppliedToDimensions/stDim:h"))) == xmp.end())
  {
    return;
  }
  // TODO: assert that we actually fit an int.
  const int imgHeight = imgHeightIt->toLong();

  XmpData::const_iterator imgWidthIt;
  if((imgWidthIt = xmp.findKey
      (XmpKey("Xmp.mwg-rs.Regions/mwg-rs:AppliedToDimensions/stDim:w"))) == xmp.end())
  {
    return;
  }
  // TODO: assert that we actually fit an int.
  const int imgWidth = imgWidthIt->toLong();

  /* Gather the regions for transformation */
  std::vector<std::array<float, 4> > regions;
  std::size_t i = 0;
  while(true)
  {
    // Go for the next region (indices start at 1).
    i++;
    const std::string regionKey =
      "Xmp.mwg-rs.Regions/mwg-rs:RegionList[" + std::to_string(i) + "]";
    if(xmp.findKey(XmpKey(regionKey)) == xmp.end())
    {
      break;
    }

    XmpData::const_iterator regionUnitIt;
    if((regionUnitIt = xmp.findKey
        (XmpKey(regionKey + "/mwg-rs:Area/stArea:unit"))) == xmp.end())
    {
      return;
    }
    std::string regionUnit = regionUnitIt->toString();
    if(regionUnit == "normalized")
    {
      const std::pair<std::array<float, 4>, bool> result
          = getRegionNormalized(xmp, regionKey, RegionType::MWG, imgWidth, imgHeight);
      if(result.second)
      {
        regions.emplace_back(result.first);
      }
    }
    else
    {
      return;
    }
  }

  // Now run the regions throught the pixelpipe transformations.
  std::vector<float> points;
  points.reserve(8 * regions.size());

  for(const auto &r : regions)
  {
    // bottom-left
    points.emplace_back(r[0]);
    points.emplace_back(r[1]);
    // top-left
    points.emplace_back(r[0]);
    points.emplace_back(r[3]);
    // top-right
    points.emplace_back(r[2]);
    points.emplace_back(r[3]);
    // bottom-right
    points.emplace_back(r[2]);
    points.emplace_back(r[1]);
  }

  if(dt_dev_distort_transform_plus(dev, pipe, 0.0f,
                                   DT_DEV_TRANSFORM_DIR_ALL,
                                   points.data(), 4 * regions.size())
     != 1)
  {
    return;
  }

  // Overwrite the regions with the transformed coordinates.
  const int finalHeight = pipe->final_height;
  const int finalWidth = pipe->final_width;
  for(i = 0; i < regions.size(); i++)
  {
    const std::size_t idx = 8 * i;
    const auto x_minmax = std::minmax({ points[idx],
                                        points[idx + 2],
                                        points[idx + 4],
                                        points[idx + 6] });

    const auto y_minmax = std::minmax({ points[idx + 1],
                                        points[idx + 3],
                                        points[idx + 5],
                                        points[idx + 7] });
    // Clamp to the final image.
    const float x = std::max(x_minmax.first / finalWidth, 0.0f);
    const float y = std::max(y_minmax.first / finalHeight, 0.0f);
    const float w = std::min(x_minmax.second / finalWidth, 1.0f) - x;
    const float h = std::min(y_minmax.second / finalHeight, 1.0f) - y;

    std::string regionKey =
      "Xmp.mwg-rs.Regions/mwg-rs:RegionList[" + std::to_string(i + 1) + "]";
    // For x and y, we have to retranslate them to be the center of the region.
    xmp[regionKey + "/mwg-rs:Area/stArea:x"] = XmpTextValue(std::to_string(x + w/2));
    xmp[regionKey + "/mwg-rs:Area/stArea:y"] = XmpTextValue(std::to_string(y + h/2));
    xmp[regionKey + "/mwg-rs:Area/stArea:h"] = XmpTextValue(std::to_string(h));
    xmp[regionKey + "/mwg-rs:Area/stArea:w"] = XmpTextValue(std::to_string(w));
  }

  // Finally, overwrite the dimensions with the image dimensions.
  xmp["Xmp.mwg-rs.Regions/mwg-rs:AppliedToDimensions/stDim:h"] =
    XmpTextValue(std::to_string(finalHeight));
  xmp["Xmp.mwg-rs.Regions/mwg-rs:AppliedToDimensions/stDim:w"] =
    XmpTextValue(std::to_string(finalWidth));
}


gboolean dt_exif_xmp_attach_export(const dt_imgid_t imgid,
                                   const char *filename,
                                   void *metadata,
                                   dt_develop_t *dev,
                                   dt_dev_pixelpipe_t *pipe)
{
  dt_export_metadata_t *m = (dt_export_metadata_t *)metadata;
  try
  {
    char input_filename[PATH_MAX] = { 0 };
    gboolean from_cache = TRUE;
    dt_image_full_path(imgid, input_filename, sizeof(input_filename), &from_cache);

    std::unique_ptr<Exiv2::Image> img(Exiv2::ImageFactory::open(WIDEN(filename)));

    // Unfortunately it seems we have to read the metadata, to not
    // erase the exif (which we just wrote). Will make export slightly
    // slower, oh well.  img->clearXmpPacket();
    read_metadata_threadsafe(img);

    try
    {
      // Initialize XMP and IPTC data with the one from the original file.
      std::unique_ptr<Exiv2::Image> input_image
        (Exiv2::ImageFactory::open(WIDEN(input_filename)));
      if(input_image.get() != 0)
      {
        read_metadata_threadsafe(input_image);
        img->setIptcData(input_image->iptcData());
        img->setXmpData(input_image->xmpData());
      }
    }
    catch(Exiv2::AnyError &e)
    {
      dt_print(DT_DEBUG_IMAGEIO,
               "[xmp_attach] %s: caught exiv2 exception '%s'",
               input_filename,
               e.what());
    }

    Exiv2::XmpData &xmpData = img->xmpData();

    // Now add whatever we have in the sidecar XMP. This overwrites
    // stuff from the source image.
    dt_image_path_append_version(imgid, input_filename, sizeof(input_filename));
    g_strlcat(input_filename, ".xmp", sizeof(input_filename));
    if(g_file_test(input_filename, G_FILE_TEST_EXISTS))
    {
      Exiv2::XmpData sidecarXmpData;
      std::string xmpPacket;

      Exiv2::DataBuf buf = Exiv2::readFile(WIDEN(input_filename));
#if EXIV2_TEST_VERSION(0,28,0)
      xmpPacket.assign(buf.c_str(), buf.size());
#else
      xmpPacket.assign(reinterpret_cast<char *>(buf.pData_), buf.size_);
#endif
      Exiv2::XmpParser::decode(sidecarXmpData, xmpPacket);

      for(Exiv2::XmpData::const_iterator it = sidecarXmpData.begin();
          it != sidecarXmpData.end();
          ++it)
        xmpData.add(*it);
    }

    _remove_known_keys(xmpData); // is this needed?

    {
      // We also want to make sure to not have some tags that might
      // have come in from XMP files created by digikam or similar.
      static const char *keys[] = {
        "Xmp.tiff.Orientation"
      };
      static const guint n_keys = G_N_ELEMENTS(keys);
      _remove_xmp_keys(xmpData, keys, n_keys);
    }

    // Last but not least, attach what we have in DB to the XMP. In theory that should be
    // the same as what we just copied over from the sidecar file, but you never know ...
    // Make sure to remove all geotags if necessary.
    if(m)
    {
      Exiv2::ExifData exifOldData;
      Exiv2::ExifData &exifData = img->exifData();
      if(!(m->flags & DT_META_EXIF))
      {
        for(Exiv2::ExifData::const_iterator i = exifData.begin();
            i != exifData.end();
            ++i)
        {
          exifOldData[i->key()] = i->value();
        }
        img->clearExifData();
      }

      _exif_xmp_read_data_export(xmpData, imgid, m);

      Exiv2::IptcData &iptcData = img->iptcData();

      if(!(m->flags & DT_META_GEOTAG))
        _remove_exif_geotag(exifData);

      // Calculated metadata
      dt_variables_params_t *params;
      dt_variables_params_init(&params);
      params->filename = input_filename;
      params->jobcode = "infos";
      params->sequence = 0;
      params->imgid = imgid;

      dt_variables_set_tags_flags(params, m->flags);
      for(GList *tags = m->list; tags; tags = g_list_next(tags))
      {
        gchar *tagname = (gchar *)tags->data;
        tags = g_list_next(tags);
        if(!tags) break;
        gchar *formula = (gchar *)tags->data;
        if(formula[0])
        {
          if(!(m->flags & DT_META_EXIF)
             && (formula[0] == '=')
             && g_str_has_prefix(tagname, "Exif."))
          {
            // Remove this specific exif
            Exiv2::ExifData::const_iterator pos;
            if(_exif_read_exif_tag(exifOldData, &pos, tagname))
            {
              exifData[tagname] = pos->value();
            }
          }
          else
          {
            gchar *result = dt_variables_expand(params, formula, FALSE);
            if(result && result[0])
            {
              if(g_str_has_prefix(tagname, "Xmp."))
              {
                const char *type = _exif_get_exiv2_tag_type(tagname);

                // If xmpBag or xmpSeq, split the list when necessary
                // else provide the string as is (can be a list of strings).
                if(!g_strcmp0(type, "XmpBag") || !g_strcmp0(type, "XmpSeq"))
                {
                  char *tuple = g_strrstr(result, ",");
                  while(tuple)
                  {
                    tuple[0] = '\0';
                    tuple++;
                    xmpData[tagname] = tuple;
                    tuple = g_strrstr(result, ",");
                  }
                }
                xmpData[tagname] = result;
              }
              else if(g_str_has_prefix(tagname, "Iptc."))
              {
                const char *type = _exif_get_exiv2_tag_type(tagname);
                if(!g_strcmp0(type, "String-R"))
                {
                  // Clean up the original tags before giving new values.
                  _remove_iptc_key(iptcData, tagname);

                  // Convert the input list (separator ", ") into different tags.
                  // FIXME If an element of the list contains a ", "
                  // it is not correctly exported.
                  Exiv2::IptcKey key(tagname);
                  Exiv2::Iptcdatum id(key);
                  gchar **values = g_strsplit(result, ", ", 0);
                  if(values)
                  {
                    gchar **entry = values;
                    while (*entry)
                    {
                      char *e = g_strstrip(*entry);
                      if(*e)
                      {
                        id.setValue(e);
                        iptcData.add(id);
                      }
                      entry++;
                    }
                  }
                g_strfreev(values);
                }
                else iptcData[tagname] = result;
              }
              else if(g_str_has_prefix(tagname, "Exif."))
              {
                const char *type = _exif_get_exiv2_tag_type(tagname);
                if((!g_strcmp0(type, "Rational")
                    || !g_strcmp0(type, "SRational"))
                   && (g_strstr_len(result, strlen(result), "/") == NULL))
                {
                  float float_value = (float)std::atof(result);
                  if(!std::isnan(float_value))
                  {
                    g_free(result);
                    int int_value = (int)float_value;
                    int divisor = 1;
                    while(fabs(float_value - int_value) > 0.000001)
                    {
                      divisor *= 10;
                      float_value *= 10.0;
                      int_value = (int)float_value;
                    }
                    result = g_strdup_printf("%d/%d", (int)float_value, divisor);
                  }
                }
                exifData[tagname] = result;
              }
            }
            g_free(result);
          }
        }
        else
        {
          if(g_str_has_prefix(tagname, "Xmp."))
            _remove_xmp_key(xmpData, tagname);
          else if(g_str_has_prefix(tagname, "Exif."))
            _remove_exif_key(exifData, tagname);
          else if(g_str_has_prefix(tagname, "Iptc."))
            _remove_iptc_key(iptcData, tagname);
        }
      }
      dt_variables_params_destroy(params);
    }

    // Transform face tags before writing them.
    _transform_face_tags(xmpData, dev, pipe);

    try
    {
      img->writeMetadata();
    }
    catch(Exiv2::AnyError &e)
    {
      if(e.code() == Exiv2::ErrorCode::kerTooLargeJpegSegment)
      {
        _remove_xmp_keys(xmpData, "Xmp.darktable.history");
        _remove_xmp_keys(xmpData, "Xmp.darktable.masks_history");
        _remove_xmp_keys(xmpData, "Xmp.darktable.auto_presets_applied");
        _remove_xmp_keys(xmpData, "Xmp.darktable.iop_order");
        try
        {
          img->writeMetadata();
        }
        catch(Exiv2::AnyError &e2)
        {
          dt_print(DT_DEBUG_IMAGEIO,
                   "[dt_exif_xmp_attach_export] without history %s: caught exiv2 exception '%s'",
                   filename,
                   e2.what());
          return TRUE;
        }
      }
      else
        throw;
    }
    return FALSE;
  }
  catch(Exiv2::AnyError &e)
  {
    dt_print(DT_DEBUG_IMAGEIO,
             "[dt_exif_xmp_attach_export] %s: caught exiv2 exception '%s'",
             filename,
             e.what());
    return TRUE;
  }
}

// Write XMP sidecar file: returns TRUE in case of errors.
gboolean dt_exif_xmp_write(const dt_imgid_t imgid,
                           const char *filename,
                           const gboolean force_write)
{
  // Refuse to write sidecar for non-existent image:
  char imgfname[PATH_MAX] = { 0 };
  gboolean from_cache = TRUE;

  dt_image_full_path(imgid, imgfname, sizeof(imgfname), &from_cache);
  if(!g_file_test(imgfname, G_FILE_TEST_IS_REGULAR)) return TRUE;

  try
  {
    Exiv2::XmpData xmpData;
    std::string xmpPacket;
    char *checksum_old = NULL;
    if(!force_write && g_file_test(filename, G_FILE_TEST_EXISTS))
    {
      // we want to avoid writing the sidecar file if it didn't change
      // to avoid issues when using the same images from different
      // computers. Sample use case: images on NAS, several computers
      // using them NOT AT THE SAME TIME and the XMP crawler is used
      // to find changed sidecars.
      errno = 0;
      size_t end;
      unsigned char *content = (unsigned char*)dt_read_file(filename, &end);
      if(content)
      {
        checksum_old = g_compute_checksum_for_data(G_CHECKSUM_MD5, content, end);
        free(content);
      }
      else
      {
        dt_print(DT_DEBUG_ALWAYS,
                 "cannot read XMP file '%s': '%s'", filename, strerror(errno));
        dt_control_log(_("cannot read XMP file '%s': '%s'"), filename, strerror(errno));
      }

      Exiv2::DataBuf buf = Exiv2::readFile(WIDEN(filename));
#if EXIV2_TEST_VERSION(0,28,0)
      xmpPacket.assign(buf.c_str(), buf.size());
#else
      xmpPacket.assign(reinterpret_cast<char *>(buf.pData_), buf.size_);
#endif
      Exiv2::XmpParser::decode(xmpData, xmpPacket);

      // Because XmpSeq or XmpBag are added to the list, we first have to
      // remove these so that we don't end up with a string of duplicates.
      _remove_known_keys(xmpData);
    }

    // Initialize xmp data:
    _exif_xmp_read_data(xmpData, imgid, "dt_exif_xmp_write");

    // Serialize the xmp data and output the xmp packet.
    if(Exiv2::XmpParser::encode(xmpPacket, xmpData,
       Exiv2::XmpParser::useCompactFormat | Exiv2::XmpParser::omitPacketWrapper) != 0)
    {
      throw Exiv2::Error(Exiv2::ErrorCode::kerErrorMessage, "[xmp_write] failed to serialize xmp data");
    }

    // Hash the new data and compare it to the old hash (if applicable).
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
      // Using std::ofstream isn't possible here -- on Windows it
      // doesn't support Unicode filenames with mingw.
      errno = 0;
      FILE *fout = g_fopen(filename, "wb");
      if(fout)
      {
        fprintf(fout, "%s", xml_header);
        fprintf(fout, "%s", xmpPacket.c_str());
        fclose(fout);
      }
      else
      {
        dt_print(DT_DEBUG_ALWAYS,
                 "cannot write XMP file '%s': '%s'", filename, strerror(errno));
        dt_control_log(_("cannot write XMP file '%s': '%s'"), filename, strerror(errno));
        return TRUE;
      }
    }

    return FALSE;
  }
  catch(Exiv2::AnyError &e)
  {
    dt_print(DT_DEBUG_IMAGEIO,
             "[dt_exif_xmp_write] %s: caught exiv2 exception '%s'",
             filename,
             e.what());
    return TRUE;
  }
}

dt_colorspaces_color_profile_type_t dt_exif_get_color_space(const uint8_t *data,
                                                            const size_t size)
{
  try
  {
    Exiv2::ExifData::const_iterator pos;
    Exiv2::ExifData exifData;
    Exiv2::ExifParser::decode(exifData, data, size);
    // clang-format off
    // 0x01   -> sRGB
    // 0x02   -> AdobeRGB
    // 0xffff -> Uncalibrated
    //          + Exif.Iop.InteroperabilityIndex of 'R03' -> AdobeRGB
    //          + Exif.Iop.InteroperabilityIndex of 'R98' -> sRGB
    // clang-format on
    if((pos = exifData.findKey(Exiv2::ExifKey("Exif.Photo.ColorSpace")))
       != exifData.end() && pos->size())
    {
      int colorspace = pos->toLong();
      if(colorspace == 0x01)
        return DT_COLORSPACE_SRGB;
      else if(colorspace == 0x02)
        return DT_COLORSPACE_ADOBERGB;
      else if(colorspace == 0xffff)
      {
        if((pos = exifData.findKey(Exiv2::ExifKey("Exif.Iop.InteroperabilityIndex")))
           != exifData.end()
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
    dt_print(DT_DEBUG_IMAGEIO,
             "[exiv2 dt_exif_get_color_space] %s",
             e.what());
    return DT_COLORSPACE_DISPLAY;
  }
}

void dt_exif_get_basic_data(const uint8_t *data,
                            const size_t size,
                            dt_image_basic_exif_t *basic_exif)
{
  try
  {
    Exiv2::ExifData::const_iterator pos;
    std::unique_ptr<Exiv2::Image> image(Exiv2::ImageFactory::open(data, size));
    read_metadata_threadsafe(image);
    Exiv2::ExifData &exifData = image->exifData();

    _find_datetime_taken(exifData, pos, basic_exif->datetime);
    _find_exif_makermodel(exifData, pos, basic_exif);
  }
  catch(Exiv2::AnyError &e)
  {
    dt_print(DT_DEBUG_IMAGEIO,
             "[exiv2 dt_exif_get_basic_data] %s",
             e.what());
  }
}

static void _exif_log_handler(const int log_level,
                              const char *message)
{
  if(log_level >= Exiv2::LogMsg::level())
  {
    // We don't seem to need \n in the format string as Exiv2 includes it
    // in the messages themselves.
    dt_print(DT_DEBUG_CAMERA_SUPPORT, "[exiv2] %s", message);
  }
}

void dt_exif_init()
{
  // Preface the Exiv2 messages with "[exiv2] "
  Exiv2::LogMsg::setHandler(&_exif_log_handler);

  #if EXIV2_TEST_VERSION(0,27,4) && !EXIV2_TEST_VERSION(0,28,3)
  Exiv2::enableBMFF();
  #endif

  Exiv2::XmpParser::initialize();

  // This has to stay with the old url (namespace already propagated outside dt).
  Exiv2::XmpProperties::registerNs("http://darktable.sf.net/", "darktable");

  // Check if Exiv2 version already knows these prefixes.
  try
  {
    Exiv2::XmpProperties::propertyList("lr");
  }
  catch(Exiv2::AnyError &e)
  {
    // If Lightroom is not known register it.
    Exiv2::XmpProperties::registerNs("http://ns.adobe.com/lightroom/1.0/", "lr");
  }
  try
  {
    Exiv2::XmpProperties::propertyList("exifEX");
  }
  catch(Exiv2::AnyError &e)
  {
    // If exifEX is not known register it.
    Exiv2::XmpProperties::registerNs("http://cipa.jp/exif/1.0/", "exifEX");
  }
}

void dt_exif_cleanup()
{
  Exiv2::XmpParser::terminate();
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
