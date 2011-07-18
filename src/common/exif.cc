/*
   This file is part of darktable,
   copyright (c) 2009--2010 johannes hanika.

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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

extern "C"
{
#include "common/exif.h"
#include "common/darktable.h"
#include "common/colorlabels.h"
#include "common/image_cache.h"
#include "common/imageio.h"
#include "common/metadata.h"
#include "common/tags.h"
#include "common/debug.h"
}
// #include <libexif/exif-data.h>
#include <exiv2/easyaccess.hpp>
#include <exiv2/xmp.hpp>
#include <exiv2/error.hpp>
#include <exiv2/image.hpp>
#include <exiv2/exif.hpp>
#include <sqlite3.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <cassert>
#include <glib.h>

#define DT_XMP_KEYS_NUM 12 // the number of XmpBag XmpSeq keys that dt uses

//this array should contain all XmpBag and XmpSeq keys used by dt
const char *dt_xmp_keys[DT_XMP_KEYS_NUM] =
{
  "Xmp.dc.subject", "Xmp.darktable.colorlabels",
  "Xmp.darktable.history_modversion", "Xmp.darktable.history_enabled",
  "Xmp.darktable.history_operation", "Xmp.darktable.history_params", "Xmp.darktable.blendop_params",
  "Xmp.dc.creator", "Xmp.dc.publisher", "Xmp.dc.title", "Xmp.dc.description", "Xmp.dc.rights"
};

// inspired by ufraw_exiv2.cc:

static void dt_strlcpy_to_utf8(char *dest, size_t dest_max,
                               Exiv2::ExifData::const_iterator &pos, Exiv2::ExifData& exifData)
{
  std::string str = pos->print(&exifData);

  char *s = g_locale_to_utf8(str.c_str(), str.length(),
                             NULL, NULL, NULL);
  if ( s!=NULL )
  {
    g_strlcpy(dest, s, dest_max);
    g_free(s);
  }
  else
  {
    g_strlcpy(dest, str.c_str(), dest_max);
  }
}

//function to remove known dt keys from xmpdata, so not to append them twice
//this should work because dt first reads all known keys
static void dt_remove_known_keys(Exiv2::XmpData &xmp)
{
  for(int i=0; i<DT_XMP_KEYS_NUM; i++)
  {
    Exiv2::XmpData::iterator pos = xmp.findKey(Exiv2::XmpKey(dt_xmp_keys[i]));
    if(pos != xmp.end()) xmp.erase(pos);
  }
}


int dt_exif_read(dt_image_t *img, const char* path)
{
  try
  {
    Exiv2::Image::AutoPtr image;
    image = Exiv2::ImageFactory::open(path);
    assert(image.get() != 0);
    image->readMetadata();
    Exiv2::ExifData &exifData = image->exifData();
    if (exifData.empty())
    {
      std::string error(path);
      error += ": no exif data found in the file";
      throw Exiv2::Error(1, error);
    }

    /* List of tag names taken from exiv2's printSummary() in actions.cpp */
    Exiv2::ExifData::const_iterator pos;
    /* Read shutter time */
    if ( (pos=exifData.findKey(Exiv2::ExifKey("Exif.Photo.ExposureTime")))
         != exifData.end() )
    {
      // dt_strlcpy_to_utf8(uf->conf->shutterText, max_name, pos, exifData);
      img->exif_exposure = pos->toFloat ();
    }
    else if ( (pos=exifData.findKey(Exiv2::ExifKey("Exif.Photo.ShutterSpeedValue")))
              != exifData.end() )
    {
      // uf_strlcpy_to_utf8(uf->conf->shutterText, max_name, pos, exifData);
      img->exif_exposure = 1.0/pos->toFloat ();
    }
    /* Read aperture */
    if ( (pos=exifData.findKey(Exiv2::ExifKey("Exif.Photo.FNumber")))
         != exifData.end() )
    {
      img->exif_aperture = pos->toFloat ();
    }
    else if ( (pos=exifData.findKey(
                     Exiv2::ExifKey("Exif.Photo.ApertureValue")))
              != exifData.end() )
    {
      img->exif_aperture = pos->toFloat ();
    }
    /* Read ISO speed */
    if ( (pos=Exiv2::isoSpeed(exifData) )
         != exifData.end() )
    {
      img->exif_iso = pos->toFloat ();
    }
#if EXIV2_MINOR_VERSION>19
    /* Read focal length  */
    if ( (pos=Exiv2::focalLength(exifData))
         != exifData.end() )
    {
      img->exif_focal_length = pos->toFloat ();
    }

    if ( (pos=Exiv2::subjectDistance(exifData))
         != exifData.end() )
    {
      img->exif_focus_distance = pos->toFloat ();
    }
#endif
    /** read image orientation */
    if ( (pos=exifData.findKey(Exiv2::ExifKey("Exif.Image.Orientation")))
         != exifData.end() )
    {
      img->orientation = dt_image_orientation_to_flip_bits(pos->toLong());
    }

    /* Read lens name */
    if (((pos = exifData.findKey(Exiv2::ExifKey("Exif.CanonCs.LensType"))) != exifData.end()) ||
        ((pos = exifData.findKey(Exiv2::ExifKey("Exif.Canon.0x0095")))     != exifData.end()))
    {
      dt_strlcpy_to_utf8(img->exif_lens, 52, pos, exifData);
    }
    else if ( (pos=exifData.findKey(Exiv2::ExifKey("Exif.Panasonic.LensType"))) != exifData.end() )
    {
      dt_strlcpy_to_utf8(img->exif_lens, 52, pos, exifData);
    }
    else if ( (pos=Exiv2::lensName(exifData)) != exifData.end() )
    {
      dt_strlcpy_to_utf8(img->exif_lens, 52, pos, exifData);
    }

#if 0
    /* Read flash mode */
    if ( (pos=exifData.findKey(Exiv2::ExifKey("Exif.Photo.Flash")))
         != exifData.end() )
    {
      uf_strlcpy_to_utf8(uf->conf->flashText, max_name, pos, exifData);
    }
    /* Read White Balance Setting */
    if ( (pos=exifData.findKey(Exiv2::ExifKey("Exif.Photo.WhiteBalance")))
         != exifData.end() )
    {
      uf_strlcpy_to_utf8(uf->conf->whiteBalanceText, max_name, pos, exifData);
    }
#endif

    // Also read IPTC metadata. I'm not sure if dt_exif_read() is the right place for it ...
    // FIXME: We should pick a few more from http://www.exiv2.org/iptc.html
    Exiv2::IptcData &iptcData = image->iptcData();
    Exiv2::IptcData::iterator iptcPos;

    if( (iptcPos=iptcData.findKey(Exiv2::IptcKey("Iptc.Application2.Keywords")))
        != iptcData.end() )
    {
      while(iptcPos != iptcData.end())
      {
        std::string str = iptcPos->print(&exifData);
        guint tagid = 0;
        dt_tag_new(str.c_str(),&tagid);
        dt_tag_attach(tagid, img->id);
        iptcPos++;
      }
    }

    if ( (pos=exifData.findKey(Exiv2::ExifKey("Exif.Image.Make")))
         != exifData.end() )
    {
      dt_strlcpy_to_utf8(img->exif_maker, 32, pos, exifData);
      for(char *c=img->exif_maker+31; c > img->exif_maker; c--) if(*c != ' ' && *c != '\0')
        {
          *(c+1) = '\0';
          break;
        }
    }
    if ( (pos=exifData.findKey(Exiv2::ExifKey("Exif.Image.Model")))
         != exifData.end() )
    {
      dt_strlcpy_to_utf8(img->exif_model, 32, pos, exifData);
      for(char *c=img->exif_model+31; c > img->exif_model; c--) if(*c != ' ' && *c != '\0')
        {
          *(c+1) = '\0';
          break;
        }
    }
    if ( (pos=exifData.findKey(Exiv2::ExifKey("Exif.Photo.DateTimeOriginal")))
         != exifData.end() )
    {
      dt_strlcpy_to_utf8(img->exif_datetime_taken, 20, pos, exifData);
    }

    if ( (pos=exifData.findKey(Exiv2::ExifKey("Exif.Image.Artist")))
         != exifData.end() )
    {
      std::string str = pos->print(&exifData);
      dt_metadata_set(img->id, "Xmp.dc.creator", str.c_str());
    }
    else if ( (iptcPos=iptcData.findKey(Exiv2::IptcKey("Iptc.Application2.Writer")))
              != iptcData.end() )
    {
      std::string str = iptcPos->print(&exifData);
      dt_metadata_set(img->id, "Xmp.dc.creator", str.c_str());
    }
    else if ( (pos=exifData.findKey(Exiv2::ExifKey("Exif.Canon.OwnerName")))
              != exifData.end() )
    {
      std::string str = pos->print(&exifData);
      dt_metadata_set(img->id, "Xmp.dc.creator", str.c_str());
    }

    // FIXME: Should the UserComment go into the description? Or do we need an extra field for this?
    if ( (pos=exifData.findKey(Exiv2::ExifKey("Exif.Photo.UserComment")))
         != exifData.end() )
    {
      std::string str = pos->print(&exifData);
      dt_metadata_set(img->id, "Xmp.dc.description", str.c_str());
    }

    if ( (pos=exifData.findKey(Exiv2::ExifKey("Exif.Image.Copyright")))
         != exifData.end() )
    {
      std::string str = pos->print(&exifData);
      dt_metadata_set(img->id, "Xmp.dc.rights", str.c_str());
    }

    // workaround for an exiv2 bug writing random garbage into exif_lens for this camera:
    // http://dev.exiv2.org/issues/779
    if(!strcmp(img->exif_model, "DMC-GH2")) sprintf(img->exif_lens, "(unknown)");

    img->exif_inited = 1;
    img->dirty = 1;
    return 0;
  }
  catch (Exiv2::AnyError& e)
  {
    std::string s(e.what());
    std::cerr << "[exiv2] " << s << std::endl;
    return 1;
  }
}

#if 0
void *dt_exif_data_new(uint8_t *data,uint32_t size)
{
  Exiv2::ExifData *exifData= new Exiv2::ExifData ;
  Exiv2::Image::AutoPtr image = Exiv2::ImageFactory::open( (Exiv2::byte *) data, size );
  image->readMetadata();
  //_mimeType = image->mimeType();
  (*exifData) = image->exifData();

  return exifData;
}

const char *dt_exif_data_get_value(void *exif_data, const char *key,char *value,uint32_t vsize)
{
  Exiv2::ExifData *exifData=(Exiv2::ExifData *)exif_data;
  Exiv2::ExifData::iterator pos;
  if( (pos=exifData->findKey(Exiv2::ExifKey(key))) != exifData->end() )
  {
    std::stringstream vv;
    vv << (*exifData)[key];
    sprintf(value,"%s",vv.str().c_str());
    fprintf(stderr,"Value: %s\n",vv.str().c_str());
    return value;
  }
  return NULL;
}
#endif

#if 0
#include <stdio.h>
void *dt_exif_data_new(uint8_t *data,uint32_t size)
{
  // use libexif to parse the binary exif ifd into a exiv2 metadata
  Exiv2::ExifData *exifData=new Exiv2::ExifData;
  ::ExifData *ed;

  if( (ed = exif_data_new_from_data((unsigned char *)data, size)) != NULL)
  {
    char value[1024]= {0};
    int ifd_index=EXIF_IFD_EXIF;
    for(uint32_t i = 0; i < ed->ifd[ifd_index]->count; i++)
    {
      char key[1024]="Exif.Photo.";
      exif_entry_get_value(ed->ifd[ifd_index]->entries[i],value,1024);
      g_strlcat(key,exif_tag_get_name(ed->ifd[ifd_index]->entries[i]->tag), 1024);
      fprintf(stderr,"Adding key '%s' value '%s'\n",key,value);
      (*exifData)[key] = value;
    }
  }

  return exifData;
}

const char *dt_exif_data_get_value(void *exif_data, const char *key,char *value,uint32_t vsize)
{
  Exiv2::ExifData *exifData=(Exiv2::ExifData *)exif_data;
  Exiv2::ExifData::iterator pos;
  if( (pos=exifData->findKey(Exiv2::ExifKey(key))) != exifData->end() )
  {
    dt_strlcpy_to_utf8(value, vsize, pos,*exifData);
  }
  return NULL;
}
#endif


int dt_exif_write_blob(uint8_t *blob,uint32_t size, const char* path)
{
  try
  {
    Exiv2::Image::AutoPtr image = Exiv2::ImageFactory::open(path);
    assert (image.get() != 0);
    image->readMetadata();
    Exiv2::ExifData &imgExifData = image->exifData();
    Exiv2::ExifData blobExifData;
    Exiv2::ExifParser::decode(blobExifData, blob+6, size);
    Exiv2::ExifData::const_iterator end = blobExifData.end();
    for (Exiv2::ExifData::const_iterator i = blobExifData.begin(); i != end; ++i)
    {
      Exiv2::ExifKey key(i->key());
      if( imgExifData.findKey(key) == imgExifData.end() )
        imgExifData.add(Exiv2::ExifKey(i->key()),&i->value());
    }
    // Remove thumbnail
    Exiv2::ExifData::iterator it;
    if( (it=imgExifData.findKey(Exiv2::ExifKey("Exif.Thumbnail.Compression"))) !=imgExifData.end() ) imgExifData.erase(it);
    if( (it=imgExifData.findKey(Exiv2::ExifKey("Exif.Thumbnail.XResolution"))) !=imgExifData.end() ) imgExifData.erase(it);
    if( (it=imgExifData.findKey(Exiv2::ExifKey("Exif.Thumbnail.YResolution"))) !=imgExifData.end() ) imgExifData.erase(it);
    if( (it=imgExifData.findKey(Exiv2::ExifKey("Exif.Thumbnail.ResolutionUnit"))) !=imgExifData.end() ) imgExifData.erase(it);
    if( (it=imgExifData.findKey(Exiv2::ExifKey("Exif.Thumbnail.JPEGInterchangeFormat"))) !=imgExifData.end() ) imgExifData.erase(it);
    if( (it=imgExifData.findKey(Exiv2::ExifKey("Exif.Thumbnail.JPEGInterchangeFormatLength"))) !=imgExifData.end() ) imgExifData.erase(it);

    imgExifData.sortByTag();
    image->writeMetadata();
  }
  catch (Exiv2::AnyError& e)
  {
    std::string s(e.what());
    std::cerr << "[exiv2] " << s << std::endl;
    return 0;
  }
  return 1;
}

int dt_exif_read_blob(uint8_t *buf, const char* path, const int sRGB, const int imgid)
{
  try
  {
    Exiv2::Image::AutoPtr image;
    image = Exiv2::ImageFactory::open(path);
    assert(image.get() != 0);
    image->readMetadata();
    Exiv2::ExifData &exifData = image->exifData();
    //     Exiv2::XmpData &xmpData = image->xmpData();  // TODO: I'm not sure how to embed xmp data into the blob.

    /* Dont bail, lets return a blob with UserComment and Software
       if (exifData.empty())
       {
       std::string error(path);
       error += ": no exif data found in ";
       error += path;
       throw Exiv2::Error(1, error);
       }*/
    exifData["Exif.Image.Orientation"] = uint16_t(1);

    // ufraw-style exif stripping:
    Exiv2::ExifData::iterator pos;
    /* Delete original TIFF data, which is irrelevant*/
    if ( (pos=exifData.findKey(Exiv2::ExifKey("Exif.Image.ImageWidth")))
         != exifData.end() )
      exifData.erase(pos);
    if ( (pos=exifData.findKey(Exiv2::ExifKey("Exif.Image.ImageLength")))
         != exifData.end() )
      exifData.erase(pos);
    if ( (pos=exifData.findKey(Exiv2::ExifKey("Exif.Image.BitsPerSample")))
         != exifData.end() )
      exifData.erase(pos);
    if ( (pos=exifData.findKey(Exiv2::ExifKey("Exif.Image.Compression")))
         != exifData.end() )
      exifData.erase(pos);
    if ( (pos=exifData.findKey(Exiv2::ExifKey("Exif.Image.PhotometricInterpretation")))
         != exifData.end() )
      exifData.erase(pos);
    if ( (pos=exifData.findKey(Exiv2::ExifKey("Exif.Image.FillOrder")))
         != exifData.end() )
      exifData.erase(pos);
    if ( (pos=exifData.findKey(Exiv2::ExifKey("Exif.Image.SamplesPerPixel")))
         != exifData.end() )
      exifData.erase(pos);
    if ( (pos=exifData.findKey(Exiv2::ExifKey("Exif.Image.StripOffsets")))
         != exifData.end() )
      exifData.erase(pos);
    if ( (pos=exifData.findKey(Exiv2::ExifKey("Exif.Image.RowsPerStrip")))
         != exifData.end() )
      exifData.erase(pos);
    if ( (pos=exifData.findKey(Exiv2::ExifKey("Exif.Image.StripByteCounts")))
         != exifData.end() )
      exifData.erase(pos);
    if ( (pos=exifData.findKey(Exiv2::ExifKey("Exif.Image.XResolution")))
         != exifData.end() )
      exifData.erase(pos);
    if ( (pos=exifData.findKey(Exiv2::ExifKey("Exif.Image.YResolution")))
         != exifData.end() )
      exifData.erase(pos);
    if ( (pos=exifData.findKey(Exiv2::ExifKey("Exif.Image.PlanarConfiguration")))
         != exifData.end() )
      exifData.erase(pos);
    if ( (pos=exifData.findKey(Exiv2::ExifKey("Exif.Image.ResolutionUnit")))
         != exifData.end() )
      exifData.erase(pos);

    /* Delete various MakerNote fields only applicable to the raw file */

    // Nikon thumbnail data
    if ( (pos=exifData.findKey(Exiv2::ExifKey("Exif.Nikon3.Preview")))
         != exifData.end() )
      exifData.erase(pos);
    if ( (pos=exifData.findKey(Exiv2::ExifKey("Exif.NikonPreview.JPEGInterchangeFormat")))
         != exifData.end() )
      exifData.erase(pos);

    // DNG private data
    if ( (pos=exifData.findKey(Exiv2::ExifKey("Exif.Image.DNGPrivateData")))
         != exifData.end() )
      exifData.erase(pos);

    // Pentax thumbnail data
    if ( (pos=exifData.findKey(Exiv2::ExifKey("Exif.Pentax.PreviewResolution")))
         != exifData.end() )
      exifData.erase(pos);
    if ( (pos=exifData.findKey(Exiv2::ExifKey("Exif.Pentax.PreviewLength")))
         != exifData.end() )
      exifData.erase(pos);
    if ( (pos=exifData.findKey(Exiv2::ExifKey("Exif.Pentax.PreviewOffset")))
         != exifData.end() )
      exifData.erase(pos);

    // Minolta thumbnail data
    if ( (pos=exifData.findKey(Exiv2::ExifKey("Exif.Minolta.Thumbnail")))
         != exifData.end() )
      exifData.erase(pos);
    if ( (pos=exifData.findKey(Exiv2::ExifKey("Exif.Minolta.ThumbnailOffset")))
         != exifData.end() )
      exifData.erase(pos);
    if ( (pos=exifData.findKey(Exiv2::ExifKey("Exif.Minolta.ThumbnailLength")))
         != exifData.end() )
      exifData.erase(pos);

    // Olympus thumbnail data
    if ( (pos=exifData.findKey(Exiv2::ExifKey("Exif.Olympus.Thumbnail")))
         != exifData.end() )
      exifData.erase(pos);
    if ( (pos=exifData.findKey(Exiv2::ExifKey("Exif.Olympus.ThumbnailOffset")))
         != exifData.end() )
      exifData.erase(pos);
    if ( (pos=exifData.findKey(Exiv2::ExifKey("Exif.Olympus.ThumbnailLength")))
         != exifData.end() )
      exifData.erase(pos);

    /* Write appropriate color space tag if using sRGB output */
    if (sRGB)
      exifData["Exif.Photo.ColorSpace"] = uint16_t(1);      /* sRGB */
    else
      exifData["Exif.Photo.ColorSpace"] = uint16_t(0xFFFF); /* Uncalibrated */

    exifData["Exif.Image.Software"] = PACKAGE_STRING;

    // TODO: find a nice place for the missing metadata (tags, publisher, colorlabels?). Additionally find out how to embed XMP data.
    //       And shall we add a description of the history stack to Exif.Image.ImageHistory?
    if(imgid >= 0)
    {
      GList *res = dt_metadata_get(imgid, "Xmp.dc.creator", NULL);
      if(res != NULL)
      {
        exifData["Exif.Image.Artist"] = (char*)res->data;
        //         xmpData["Xmp.dc.creator"] = (char*)res->data;
        g_free(res->data);
        g_list_free(res);
      }

      res = dt_metadata_get(imgid, "Xmp.dc.title", NULL);
      if(res != NULL)
      {
        exifData["Exif.Image.ImageDescription"] = (char*)res->data;
        //         xmpData["Xmp.dc.title"] = (char*)res->data;
        g_free(res->data);
        g_list_free(res);
      }

      res = dt_metadata_get(imgid, "Xmp.dc.description", NULL);
      if(res != NULL)
      {
        exifData["Exif.Photo.UserComment"] = (char*)res->data;
        //         xmpData["Xmp.dc.description"] = (char*)res->data;
        g_free(res->data);
        g_list_free(res);
      }

      res = dt_metadata_get(imgid, "Xmp.dc.rights", NULL);
      if(res != NULL)
      {
        exifData["Exif.Image.Copyright"] = (char*)res->data;
        //         xmpData["Xmp.dc.rights"] = (char*)res->data;
        g_free(res->data);
        g_list_free(res);
      }

      res = dt_metadata_get(imgid, "Xmp.xmp.Rating", NULL);
      if(res != NULL)
      {
        int rating = GPOINTER_TO_INT(res->data)+1;
        exifData["Exif.Image.Rating"] = rating;
        exifData["Exif.Image.RatingPercent"] = int(rating/5.*100.);
        //         xmpData["Xmp.xmp.Rating"] = rating;
        g_list_free(res);
      }
    }

    Exiv2::Blob blob;
    Exiv2::ExifParser::encode(blob, Exiv2::bigEndian, exifData);
    const int length = blob.size();
    memcpy(buf, "Exif\000\000", 6);
    if(length > 0 && length < 65534)
      memcpy(buf+6, &(blob[0]), length);
    return length;
  }
  catch (Exiv2::AnyError& e)
  {
    // std::cerr.rdbuf(savecerr);
    std::string s(e.what());
    std::cerr << "[exiv2] " << s << std::endl;
    return 0;
  }
}

// encode binary blob into text:
void dt_exif_xmp_encode (const unsigned char *input, char *output, const int len)
{
  const char hex[16] = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
  for(int i=0; i<len; i++)
  {
    const int hi = input[i] >> 4;
    const int lo = input[i] & 15;
    output[2*i]   = hex[hi];
    output[2*i+1] = hex[lo];
  }
  output[2*len] = '\0';
}

// and back to binary
void dt_exif_xmp_decode (const char *input, unsigned char *output, const int len)
{
  // ascii table:
  // 48- 57 0-9
  // 97-102 a-f
#define TO_BINARY(a) (a > 57 ? a - 97 + 10 : a - 48)
  for(int i=0; i<len/2; i++)
  {
    const int hi = TO_BINARY( input[2*i  ] );
    const int lo = TO_BINARY( input[2*i+1] );
    output[i] = (hi << 4) | lo;
  }
#undef TO_BINARY
}

int dt_exif_xmp_read (dt_image_t *img, const char* filename, const int history_only)
{
  try
  {
    // read xmp sidecar
    Exiv2::Image::AutoPtr image;
    image = Exiv2::ImageFactory::open(filename);
    assert(image.get() != 0);
    image->readMetadata();
    Exiv2::XmpData &xmpData = image->xmpData();

    sqlite3_stmt *stmt;

    // get rid of old meta data
    DT_DEBUG_SQLITE3_PREPARE_V2(darktable.db, "delete from meta_data where id = ?1", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, img->id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    Exiv2::XmpData::iterator pos;
    int version = 0;
    if((pos=xmpData.findKey(Exiv2::XmpKey("Xmp.darktable.xmp_version"))) != xmpData.end() )
    {
      version = pos->toLong();
    }

    // older darktable version did not write this data correctly:
    // the reasoning behind strdup'ing all the strings before passing it to sqlite3 is, that
    // they are somehow corrupt after the call to sqlite3_prepare_v2() -- don't ask my
    // why for they don't get passed to that function.
    if(version > 0)
    {
      if (!history_only && (pos=xmpData.findKey(Exiv2::XmpKey("Xmp.dc.rights"))) != xmpData.end() )
      {
        // rights
        char *rights = strdup(pos->toString().c_str());
        char *adr = rights;
        if(strncmp(rights, "lang=", 5) == 0)
        {
          rights = strchr(rights, ' ');
          if(rights != NULL)
            rights++;
        }
        dt_metadata_set(img->id, "Xmp.dc.rights", rights);
        free(adr);
      }
      if (!history_only && (pos=xmpData.findKey(Exiv2::XmpKey("Xmp.dc.description"))) != xmpData.end() )
      {
        // description
        char *description = strdup(pos->toString().c_str());
        char *adr = description;
        if(strncmp(description, "lang=", 5) == 0)
        {
          description = strchr(description, ' ');
          if(description != NULL)
            description++;
        }
        dt_metadata_set(img->id, "Xmp.dc.description", description);
        free(adr);
      }
      if (!history_only && (pos=xmpData.findKey(Exiv2::XmpKey("Xmp.dc.title"))) != xmpData.end() )
      {
        // title
        char *title = strdup(pos->toString().c_str());
        char *adr = title;
        if(strncmp(title, "lang=", 5) == 0)
        {
          title = strchr(title, ' ');
          if(title != NULL)
            title++;
        }
        dt_metadata_set(img->id, "Xmp.dc.title", title);
        free(adr);
      }
      if (!history_only && (pos=xmpData.findKey(Exiv2::XmpKey("Xmp.dc.creator"))) != xmpData.end() )
      {
        // creator
        char *creator = strdup(pos->toString().c_str());
        char *adr = creator;
        if(strncmp(creator, "lang=", 5) == 0)
        {
          creator = strchr(creator, ' ');
          if(creator != NULL)
            creator++;
        }
        dt_metadata_set(img->id, "Xmp.dc.creator", creator);
        free(adr);
      }
      if (!history_only && (pos=xmpData.findKey(Exiv2::XmpKey("Xmp.dc.publisher"))) != xmpData.end() )
      {
        // publisher
        char *publisher = strdup(pos->toString().c_str());
        char *adr = publisher;
        if(strncmp(publisher, "lang=", 5) == 0)
        {
          publisher = strchr(publisher, ' ');
          if(publisher != NULL)
            publisher++;
        }
        dt_metadata_set(img->id, "Xmp.dc.publisher", publisher);
        free(adr);
      }
    }

    int stars = 1;
    int raw_params = -16711632;
    int set = 0;
    if (!history_only && (pos=xmpData.findKey(Exiv2::XmpKey("Xmp.xmp.Rating"))) != xmpData.end() )
    {
      stars = (pos->toLong() == -1) ? 6 : pos->toLong();
      set = 1;
    }
    if (!history_only && (pos=xmpData.findKey(Exiv2::XmpKey("Xmp.xmp.Label"))) != xmpData.end() )
    {
      std::string label = pos->toString();
      if(label == "Red")                       // Is it really called like that in XMP files?
        dt_colorlabels_set_label(img->id, 0);
      else if(label == "Yellow")               // Is it really called like that in XMP files?
        dt_colorlabels_set_label(img->id, 1);
      else if(label == "Green")
        dt_colorlabels_set_label(img->id, 2);
      else if(label == "Blue")                 // Is it really called like that in XMP files?
        dt_colorlabels_set_label(img->id, 3);
      else if(label == "Purple")               // Is it really called like that in XMP files?
        dt_colorlabels_set_label(img->id, 4);
    }
    if ( (pos=xmpData.findKey(Exiv2::XmpKey("Xmp.darktable.raw_params"))) != xmpData.end() )
    {
      raw_params = pos->toLong();
      set = 1;
    }
    if (set)
    {
      // set in cache and write through.
      *((int32_t *)&img->raw_params) = raw_params;
      img->flags = (img->flags & ~0x7) | (0x7 & stars);
      img->dirty = 1;
      dt_image_cache_flush_no_sidecars(img);
    }
    if (!history_only && (pos=xmpData.findKey(Exiv2::XmpKey("Xmp.dc.subject"))) != xmpData.end() )
    {
      // consistency: strip all tags from image (tagged_image, tagxtag)
      DT_DEBUG_SQLITE3_PREPARE_V2(darktable.db, "update tagxtag set count = count - 1 where "
                                  "(id2 in (select tagid from tagged_images where imgid = ?2)) or "
                                  "(id1 in (select tagid from tagged_images where imgid = ?2))", -1, &stmt, NULL);
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, img->id);
      sqlite3_step(stmt);
      sqlite3_finalize(stmt);

      // remove from tagged_images
      DT_DEBUG_SQLITE3_PREPARE_V2(darktable.db, "delete from tagged_images where imgid = ?1", -1, &stmt, NULL);
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, img->id);
      sqlite3_step(stmt);
      sqlite3_finalize(stmt);

      // tags in array
      const int cnt = pos->count();

      sqlite3_stmt *stmt_sel_id, *stmt_ins_tags, *stmt_ins_tagxtag, *stmt_upd_tagxtag, *stmt_ins_tagged, *stmt_upd_tagxtag2;
      DT_DEBUG_SQLITE3_PREPARE_V2(darktable.db, "select id from tags where name = ?1", -1, &stmt_sel_id, NULL);
      DT_DEBUG_SQLITE3_PREPARE_V2(darktable.db, "insert into tags (id, name) values (null, ?1)", -1, &stmt_ins_tags, NULL);
      DT_DEBUG_SQLITE3_PREPARE_V2(darktable.db, "insert into tagxtag select id, ?1, 0 from tags", -1, &stmt_ins_tagxtag, NULL);
      DT_DEBUG_SQLITE3_PREPARE_V2(darktable.db, "update tagxtag set count = 1000000 where id1 = ?1 and id2 = ?1", -1, &stmt_upd_tagxtag, NULL);
      DT_DEBUG_SQLITE3_PREPARE_V2(darktable.db, "insert into tagged_images (tagid, imgid) values (?1, ?2)", -1, &stmt_ins_tagged, NULL);
      DT_DEBUG_SQLITE3_PREPARE_V2(darktable.db, "update tagxtag set count = count + 1 where "
                                  "(id1 = ?1 and id2 in (select tagid from tagged_images where imgid = ?2)) or "
                                  "(id2 = ?1 and id1 in (select tagid from tagged_images where imgid = ?2))", -1, &stmt_upd_tagxtag2, NULL);
      for(int i=0; i<cnt; i++)
      {
        int tagid = -1;
        // check if tag is available, get its id:
        for(int k=0; k<2; k++)
        {
          const char *tag = pos->toString(i).c_str();
          DT_DEBUG_SQLITE3_BIND_TEXT(stmt_sel_id, 1, tag, strlen(tag), SQLITE_TRANSIENT);
          if(sqlite3_step(stmt_sel_id) == SQLITE_ROW)
            tagid = sqlite3_column_int(stmt_sel_id, 0);
          sqlite3_reset(stmt_sel_id);
          sqlite3_clear_bindings(stmt_sel_id);

          if(tagid > 0)
          {
            if(k == 1)
            {
              DT_DEBUG_SQLITE3_BIND_INT(stmt_ins_tagxtag, 1, tagid);
              sqlite3_step(stmt_ins_tagxtag);
              sqlite3_reset(stmt_ins_tagxtag);
              sqlite3_clear_bindings(stmt_ins_tagxtag);

              DT_DEBUG_SQLITE3_BIND_INT(stmt_upd_tagxtag, 1, tagid);
              sqlite3_step(stmt_upd_tagxtag);
              sqlite3_reset(stmt_upd_tagxtag);
              sqlite3_clear_bindings(stmt_upd_tagxtag);
            }
            break;
          }
          // create this tag (increment id, leave icon empty), retry.
          DT_DEBUG_SQLITE3_BIND_TEXT(stmt_ins_tags, 1, tag, strlen(tag), SQLITE_TRANSIENT);
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
        DT_DEBUG_SQLITE3_BIND_INT(stmt_upd_tagxtag2, 1, tagid);
        DT_DEBUG_SQLITE3_BIND_INT(stmt_upd_tagxtag2, 2, img->id);
        sqlite3_step(stmt_upd_tagxtag2);
        sqlite3_reset(stmt_upd_tagxtag2);
        sqlite3_clear_bindings(stmt_upd_tagxtag2);
      }
      sqlite3_finalize(stmt_sel_id);
      sqlite3_finalize(stmt_ins_tags);
      sqlite3_finalize(stmt_ins_tagxtag);
      sqlite3_finalize(stmt_upd_tagxtag);
      sqlite3_finalize(stmt_ins_tagged);
      sqlite3_finalize(stmt_upd_tagxtag2);

    }
    if (!history_only && (pos=xmpData.findKey(Exiv2::XmpKey("Xmp.darktable.colorlabels"))) != xmpData.end() )
    {
      // TODO: store these in dc:subject or xmp:Label?
      // color labels
      const int cnt = pos->count();
      dt_colorlabels_remove_labels(img->id);
      for(int i=0; i<cnt; i++)
      {
        dt_colorlabels_set_label(img->id, pos->toLong(i));
      }
    }

    // history
    Exiv2::XmpData::iterator ver;
    Exiv2::XmpData::iterator en;
    Exiv2::XmpData::iterator op;
    Exiv2::XmpData::iterator param;
    Exiv2::XmpData::iterator blendop = xmpData.findKey(Exiv2::XmpKey("Xmp.darktable.blendop_params"));
         
    if ( (ver=xmpData.findKey(Exiv2::XmpKey("Xmp.darktable.history_modversion"))) != xmpData.end() &&
         (en=xmpData.findKey(Exiv2::XmpKey("Xmp.darktable.history_enabled")))     != xmpData.end() &&
         (op=xmpData.findKey(Exiv2::XmpKey("Xmp.darktable.history_operation")))   != xmpData.end() &&
         (param=xmpData.findKey(Exiv2::XmpKey("Xmp.darktable.history_params")))   != xmpData.end() )
    {
      const int cnt = ver->count();
      if(cnt == en->count() && cnt == op->count() && cnt == param->count())
      {
        // clear history
        DT_DEBUG_SQLITE3_PREPARE_V2(darktable.db, "delete from history where imgid = ?1", -1, &stmt, NULL);
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, img->id);
        sqlite3_step(stmt);
        sqlite3_finalize (stmt);
        sqlite3_stmt *stmt_sel_num, *stmt_ins_hist, *stmt_upd_hist;
        DT_DEBUG_SQLITE3_PREPARE_V2(darktable.db, "select num from history where imgid = ?1 and num = ?2", -1, &stmt_sel_num, NULL);
        DT_DEBUG_SQLITE3_PREPARE_V2(darktable.db, "insert into history (imgid, num) values (?1, ?2)", -1, &stmt_ins_hist, NULL);
        DT_DEBUG_SQLITE3_PREPARE_V2(darktable.db, "update history set operation = ?1, op_params = ?2, blendop_params = ?7, module = ?3, enabled = ?4 where imgid = ?5 and num = ?6", -1, &stmt_upd_hist, NULL);
        for(int i=0; i<cnt; i++)
        {
          const int modversion = ver->toLong(i);
          const int enabled = en->toLong(i);
          const char *operation = op->toString(i).c_str();
          const char *param_c = param->toString(i).c_str();
          const int param_c_len = strlen(param_c);
          const int params_len = param_c_len/2;
          unsigned char *params = (unsigned char *)malloc(params_len);
          dt_exif_xmp_decode(param_c, params, param_c_len);
          // TODO: why this update set?
          DT_DEBUG_SQLITE3_BIND_INT(stmt_sel_num, 1, img->id);
          DT_DEBUG_SQLITE3_BIND_INT(stmt_sel_num, 2, i);
          if(sqlite3_step(stmt_sel_num) != SQLITE_ROW)
          {
            DT_DEBUG_SQLITE3_BIND_INT(stmt_ins_hist, 1, img->id);
            DT_DEBUG_SQLITE3_BIND_INT(stmt_ins_hist, 2, i);
            sqlite3_step (stmt_ins_hist);
            sqlite3_reset(stmt_ins_hist);
            sqlite3_clear_bindings(stmt_ins_hist);
          }
        
          DT_DEBUG_SQLITE3_BIND_TEXT(stmt_upd_hist, 1, operation, strlen(operation), SQLITE_TRANSIENT);
          DT_DEBUG_SQLITE3_BIND_BLOB(stmt_upd_hist, 2, params, params_len, SQLITE_TRANSIENT);
          DT_DEBUG_SQLITE3_BIND_INT(stmt_upd_hist, 3, modversion);
          DT_DEBUG_SQLITE3_BIND_INT(stmt_upd_hist, 4, enabled);
          DT_DEBUG_SQLITE3_BIND_INT(stmt_upd_hist, 5, img->id);
          DT_DEBUG_SQLITE3_BIND_INT(stmt_upd_hist, 6, i);
          
          /* check if we got blendop from xmp */
          unsigned char *blendop_params = NULL;
          unsigned int blendop_size = 0;
          if(blendop != xmpData.end() && blendop->toString(i).c_str() != NULL) {
            blendop_size = strlen(blendop->toString(i).c_str())/2;
            blendop_params = (unsigned char *)malloc(blendop_size);
            dt_exif_xmp_decode(blendop->toString(i).c_str(),blendop_params,strlen(blendop->toString(i).c_str()));
            DT_DEBUG_SQLITE3_BIND_BLOB(stmt_upd_hist, 7, blendop_params, blendop_size, SQLITE_TRANSIENT);
          } else
            sqlite3_bind_null(stmt_upd_hist, 7);
          
          sqlite3_step (stmt_upd_hist);
          free(params);

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
  catch (Exiv2::AnyError& e)
  {
    // actually nobody's interested in that if the file doesn't exist:
    // std::string s(e.what());
    // std::cerr << "[exiv2] " << s << std::endl;

    // legacy fallback:
    char dtfilename[1024];
    g_strlcpy(dtfilename, filename, 1024);
    char *c = dtfilename + strlen(dtfilename);
    while(c > dtfilename && *c != '.') c--;
    sprintf(c, ".dttags");
    if(!history_only) dt_imageio_dttags_read(img, dtfilename);
    sprintf(c, ".dt");
    dt_imageio_dt_read(img->id, dtfilename);
    return 0;
  }
  return 0;
}

// write xmp sidecar file:
int dt_exif_xmp_write (const int imgid, const char* filename)
{
  const int xmp_version = 1;
  // refuse to write sidecar for non-existent image:
  char imgfname[1024];

  dt_image_full_path(imgid, imgfname, 1024);
  if(!g_file_test(imgfname, G_FILE_TEST_IS_REGULAR)) return 1;

  try
  {
    Exiv2::XmpData xmpData;
    std::string xmpPacket;
    if(g_file_test(filename, G_FILE_TEST_EXISTS))
    {
      Exiv2::DataBuf buf = Exiv2::readFile(filename);
      xmpPacket.assign(reinterpret_cast<char*>(buf.pData_), buf.size_);
      Exiv2::XmpParser::decode(xmpData, xmpPacket);
      //because XmpSeq or XmpBag are added to the list, we first have
      //to remove these so that we don't end up with a string of duplicates
      dt_remove_known_keys(xmpData);
    }
    int stars = 1, raw_params = 0;
    // get stars and raw params from db
    sqlite3_stmt *stmt;
    DT_DEBUG_SQLITE3_PREPARE_V2(darktable.db, "select flags, raw_parameters from images where id = ?1", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
    if(sqlite3_step(stmt) == SQLITE_ROW)
    {
      stars      = sqlite3_column_int(stmt, 0);
      raw_params = sqlite3_column_int(stmt, 1);
    }
    sqlite3_finalize(stmt);
    xmpData["Xmp.xmp.Rating"] = ((stars & 0x7) == 6) ? -1 : (stars & 0x7); //rejected image = -1, others = 0..5

    // the meta data
    DT_DEBUG_SQLITE3_PREPARE_V2(darktable.db, "select key, value from meta_data where id = ?1", -1, &stmt, NULL);
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

    // get tags from db, store in dublin core
    Exiv2::Value::AutoPtr v = Exiv2::Value::create(Exiv2::xmpSeq); // or xmpBag or xmpAlt.
    DT_DEBUG_SQLITE3_PREPARE_V2(darktable.db, "select name from tags join tagged_images on tagged_images.tagid = tags.id where imgid = ?1", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
    while(sqlite3_step(stmt) == SQLITE_ROW)
      v->read((char *)sqlite3_column_text(stmt, 0));
    sqlite3_finalize(stmt);
    xmpData.add(Exiv2::XmpKey("Xmp.dc.subject"), v.get());

    // color labels
    char val[2048];
    v = Exiv2::Value::create(Exiv2::xmpSeq); // or xmpBag or xmpAlt.
    DT_DEBUG_SQLITE3_PREPARE_V2(darktable.db, "select color from color_labels where imgid=?1", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
    while(sqlite3_step(stmt) == SQLITE_ROW)
    {
      snprintf(val, 2048, "%d", sqlite3_column_int(stmt, 0));
      v->read(val);
    }
    sqlite3_finalize(stmt);
    xmpData.add(Exiv2::XmpKey("Xmp.darktable.colorlabels"), v.get());

    // history stack:
    char key[1024];
    int num = 1;

    // create an array:
    Exiv2::XmpTextValue tv("");
    tv.setXmpArrayType(Exiv2::XmpValue::xaBag);
    xmpData.add(Exiv2::XmpKey("Xmp.darktable.history_modversion"), &tv);
    xmpData.add(Exiv2::XmpKey("Xmp.darktable.history_enabled"), &tv);
    xmpData.add(Exiv2::XmpKey("Xmp.darktable.history_operation"), &tv);
    xmpData.add(Exiv2::XmpKey("Xmp.darktable.history_params"), &tv);
    xmpData.add(Exiv2::XmpKey("Xmp.darktable.blendop_params"), &tv);

    // reset tv
    tv.setXmpArrayType(Exiv2::XmpValue::xaNone);

    DT_DEBUG_SQLITE3_PREPARE_V2(darktable.db, "select * from history where imgid = ?1 order by num", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
    while(sqlite3_step(stmt) == SQLITE_ROW)
    {
      int32_t modversion = sqlite3_column_int(stmt, 2);
      snprintf(val, 2048, "%d", modversion);
      tv.read(val);
      snprintf(key, 1024, "Xmp.darktable.history_modversion[%d]", num);
      xmpData.add(Exiv2::XmpKey(key), &tv);

      int32_t enabled = sqlite3_column_int(stmt, 5);
      snprintf(val, 2048, "%d", enabled);
      tv.read(val);
      snprintf(key, 1024, "Xmp.darktable.history_enabled[%d]", num);
      xmpData.add(Exiv2::XmpKey(key), &tv);

      const char *op = (const char *)sqlite3_column_text(stmt, 3);
      tv.read(op);
      snprintf(key, 1024, "Xmp.darktable.history_operation[%d]", num);
      xmpData.add(Exiv2::XmpKey(key), &tv);

      /* read and add history params */
      int32_t len = sqlite3_column_bytes(stmt, 4);
      char *vparams = (char *)malloc(2*len + 1);
      dt_exif_xmp_encode ((const unsigned char *)sqlite3_column_blob(stmt, 4), vparams, len);
      tv.read(vparams);
      snprintf(key, 1024, "Xmp.darktable.history_params[%d]", num);
      xmpData.add(Exiv2::XmpKey(key), &tv);
      free(vparams);
      
      /* read and add blendop params */
      len = sqlite3_column_bytes(stmt, 6);
      vparams = (char *)malloc(2*len + 1);
      dt_exif_xmp_encode ((const unsigned char *)sqlite3_column_blob(stmt, 6), vparams, len);
      tv.read(vparams);
      snprintf(key, 1024, "Xmp.darktable.blendop_params[%d]", num);
      xmpData.add(Exiv2::XmpKey(key), &tv);
      free(vparams);

      num ++;
    }
    sqlite3_finalize (stmt);

    // serialize the xmp data and output the xmp packet
    if (0 != Exiv2::XmpParser::encode(xmpPacket, xmpData))
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
  catch (Exiv2::AnyError& e)
  {
    std::cerr << "[xmp_write] caught exiv2 exception '" << e << "'\n";
    return -1;
  }
}

void dt_exif_init()
{
  // mute exiv2:
  // Exiv2::LogMsg::setLevel(Exiv2::LogMsg::error);

  Exiv2::XmpParser::initialize();
  Exiv2::XmpProperties::registerNs("http://darktable.sf.net/", "darktable");
}

void dt_exif_cleanup()
{
  Exiv2::XmpParser::terminate();
}

// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
