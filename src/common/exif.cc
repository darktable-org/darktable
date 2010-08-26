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

#include "common/exif.h"

#if 0
#include <libexif/exif-data.h>
#endif

#include <exiv2/image.hpp>
#include <exiv2/exif.hpp>
#include <exiv2/canonmn.hpp>
#include <sstream>
#include <cassert>
#include <glib.h>


// inspired by ufraw_exiv2.cc:

static void dt_strlcpy_to_utf8(char *dest, size_t dest_max,
  Exiv2::ExifData::iterator &pos, Exiv2::ExifData& exifData)
{
  std::string str = pos->print(&exifData);
  // std::stringstream ss;
  // (void)Exiv2::ExifTags::printTag(ss, 0x0016, Exiv2::canonIfdId, pos->value(), &exifData);
  // (void)Exiv2::CanonMakerNote::printCsLensType(ss, pos->value(), &exifData);
  // std::ostream &os, uint16_t tag, IfdId ifdId, const Value &value, const ExifData *pExifData=0)
  // str = ss.str();

  char *s = g_locale_to_utf8(str.c_str(), str.length(),
      NULL, NULL, NULL);
  if ( s!=NULL ) {
    g_strlcpy(dest, s, dest_max);
    g_free(s);
  } else {
    g_strlcpy(dest, str.c_str(), dest_max);
  }
}

int dt_exif_read(dt_image_t *img, const char* path)
{
  /* Redirect exiv2 errors to a string buffer */
  // std::ostringstream stderror;
  // std::streambuf *savecerr = std::cerr.rdbuf();
  // std::cerr.rdbuf(stderror.rdbuf());

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
    Exiv2::ExifData::iterator pos;
    /* Read shutter time */
    if ( (pos=exifData.findKey(Exiv2::ExifKey("Exif.Photo.ExposureTime")))
        != exifData.end() ) {
      // dt_strlcpy_to_utf8(uf->conf->shutterText, max_name, pos, exifData);
      img->exif_exposure = pos->toFloat ();
    } else if ( (pos=exifData.findKey(
            Exiv2::ExifKey("Exif.Photo.ShutterSpeedValue")))
        != exifData.end() ) {
      // uf_strlcpy_to_utf8(uf->conf->shutterText, max_name, pos, exifData);
      img->exif_exposure = 1.0/pos->toFloat ();
    }
    /* Read aperture */
    if ( (pos=exifData.findKey(Exiv2::ExifKey("Exif.Photo.FNumber")))
        != exifData.end() ) {
      img->exif_aperture = pos->toFloat ();
    } else if ( (pos=exifData.findKey(
            Exiv2::ExifKey("Exif.Photo.ApertureValue")))
        != exifData.end() ) {
      img->exif_aperture = pos->toFloat ();
    }
    /* Read ISO speed */
    if ( (pos=exifData.findKey(Exiv2::ExifKey("Exif.Photo.ISOSpeedRatings")))
        != exifData.end() ) {
      img->exif_iso = pos->toFloat ();
    } else if ( (pos=exifData.findKey(Exiv2::ExifKey(
              "Exif.CanonSi.ISOSpeed"))) != exifData.end() ) {
      img->exif_iso = pos->toFloat ();
    } else if ( (pos=exifData.findKey(Exiv2::ExifKey("Exif.Nikon1.ISOSpeed")))
        != exifData.end() ) {
      img->exif_iso = pos->toFloat ();
    } else if ( (pos=exifData.findKey(Exiv2::ExifKey("Exif.Nikon2.ISOSpeed")))
        != exifData.end() ) {
      img->exif_iso = pos->toFloat ();
    } else if ( (pos=exifData.findKey(Exiv2::ExifKey("Exif.Nikon3.ISOSpeed")))
        != exifData.end() ) {
      img->exif_iso = pos->toFloat ();
    } else if ( (pos=exifData.findKey(
            Exiv2::ExifKey("Exif.MinoltaCsNew.ISOSpeed")))
        != exifData.end() ) {
      img->exif_iso = pos->toFloat ();
    } else if ( (pos=exifData.findKey(
            Exiv2::ExifKey("Exif.MinoltaCsOld.ISOSpeed")))
        != exifData.end() ) {
      img->exif_iso = pos->toFloat ();
    } else if ( (pos=exifData.findKey(
            Exiv2::ExifKey("Exif.MinoltaCs5D.ISOSpeed")))
        != exifData.end() ) {
      img->exif_iso = pos->toFloat();
    } else if ( (pos=exifData.findKey(Exiv2::ExifKey(
              "Exif.MinoltaCs7D.ISOSpeed")))
        != exifData.end() ) {
      img->exif_iso = pos->toFloat();
    }
    /* Read focal length */
    if ( (pos=exifData.findKey(Exiv2::ExifKey("Exif.Photo.FocalLength")))
        != exifData.end() ) {
      img->exif_focal_length = pos->toFloat();
    }
#if 0
    /* Read focal length in 35mm equivalent */
    if ( (pos=exifData.findKey(Exiv2::ExifKey(
              "Exif.Photo.FocalLengthIn35mmFilm")))
        != exifData.end() ) {
      img->exif_focal_length = pos->toFloat ();
    }
#endif
    /** read image orientation */
    if ( (pos=exifData.findKey(Exiv2::ExifKey("Exif.Image.Orientation")))
        != exifData.end() ) {
      const int orient = pos->toLong();
      switch(orient)
      {
        case 1:
          img->orientation = 0 | 0 | 1;
          break;
        case 2:
          img->orientation = 0 | 2 | 1;
          break;
        case 3:
          img->orientation = 0 | 2 | 0;
          break;
        case 4:
          img->orientation = 0 | 0 | 0;
          break;
        case 5:
          img->orientation = 4 | 0 | 0;
          break;
        case 6:
          img->orientation = 4 | 2 | 0;
          break;
        case 7:
          img->orientation = 4 | 2 | 1;
          break;
        case 8:
          img->orientation = 4 | 0 | 1;
          break;
        default:
          img->orientation = 0;
          break;
      }
    }
    /* Read lens name */
    if ( (pos=exifData.findKey(Exiv2::ExifKey("Exif.Nikon3.Lens")))
        != exifData.end() )
    {
      dt_strlcpy_to_utf8(img->exif_lens, 52, pos, exifData);
    }
    else if (((pos = exifData.findKey(Exiv2::ExifKey("Exif.CanonCs.LensType"))) != exifData.end()) ||
             ((pos = exifData.findKey(Exiv2::ExifKey("Exif.Canon.0x0095")))     != exifData.end()))
    {
      dt_strlcpy_to_utf8(img->exif_lens, 52, pos, exifData);
    }
    else if ( (pos=exifData.findKey(Exiv2::ExifKey("Exif.Minolta.LensID"))) != exifData.end() )
    {
      dt_strlcpy_to_utf8(img->exif_lens, 52, pos, exifData);
    }
    else if ( (pos=exifData.findKey(Exiv2::ExifKey("Exif.Pentax.LensType"))) != exifData.end() )
    {
      dt_strlcpy_to_utf8(img->exif_lens, 52, pos, exifData);
    }
#if 0
    /* Read flash mode */
    if ( (pos=exifData.findKey(Exiv2::ExifKey("Exif.Photo.Flash")))
        != exifData.end() ) {
      uf_strlcpy_to_utf8(uf->conf->flashText, max_name, pos, exifData);
    }
    /* Read White Balance Setting */
    if ( (pos=exifData.findKey(Exiv2::ExifKey("Exif.Photo.WhiteBalance")))
        != exifData.end() ) {
      uf_strlcpy_to_utf8(uf->conf->whiteBalanceText, max_name, pos, exifData);
    }
#endif

    if ( (pos=exifData.findKey(Exiv2::ExifKey("Exif.Image.Make")))
        != exifData.end() ) {
      dt_strlcpy_to_utf8(img->exif_maker, 32, pos, exifData);
    }
    if ( (pos=exifData.findKey(Exiv2::ExifKey("Exif.Image.Model")))
        != exifData.end() ) {
      dt_strlcpy_to_utf8(img->exif_model, 32, pos, exifData);
    }
    if ( (pos=exifData.findKey(Exiv2::ExifKey("Exif.Photo.DateTimeOriginal")))
        != exifData.end() ) {
      dt_strlcpy_to_utf8(img->exif_datetime_taken, 20, pos, exifData);
    }

    // std::cerr.rdbuf(savecerr);

    // std::cout << "time c++: " << img->exif_datetime_taken << std::endl;
    // std::cout << "lens c++: " << img->exif_lens << std::endl;
    // std::cout << "lensptr : " << (long int)(img->exif_lens) << std::endl;
    // std::cout << "imgptr  : " << (long int)(img) << std::endl;
    img->exif_inited = 1;
    return 0;
  }
  catch (Exiv2::AnyError& e)
  {
    // std::cerr.rdbuf(savecerr);
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
    char value[1024]={0};
    int ifd_index=EXIF_IFD_EXIF;
    for(uint32_t i = 0; i < ed->ifd[ifd_index]->count; i++) {
      char key[1024]="Exif.Photo.";
      exif_entry_get_value(ed->ifd[ifd_index]->entries[i],value,1024);
      strcat(key,exif_tag_get_name(ed->ifd[ifd_index]->entries[i]->tag));
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

int dt_exif_read_blob(uint8_t *buf, const char* path, const int sRGB)
{
  try
  {
    Exiv2::Image::AutoPtr image;
    image = Exiv2::ImageFactory::open(path);
    assert(image.get() != 0);
    image->readMetadata();
    Exiv2::ExifData &exifData = image->exifData();
    /* Dont bail, lets return a blob with UserComment and Software
    if (exifData.empty())
    {
      std::string error(path);
      error += ": no exif data found in ";
      error += path;
      throw Exiv2::Error(1, error);
    }*/
    exifData["Exif.Image.Orientation"] = uint16_t(1);
    exifData["Exif.Photo.UserComment"]
        = "Developed using Darktable "PACKAGE_VERSION;

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
      exifData["Exif.Photo.ColorSpace"] = uint16_t(1); /* sRGB */

    exifData["Exif.Image.Software"] = "Darktable "PACKAGE_VERSION;

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
