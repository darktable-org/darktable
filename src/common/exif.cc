
#include "common/exif.h"
#include <exiv2/image.hpp>
#include <exiv2/exif.hpp>
#include <exiv2/canonmn.hpp>
#include <sstream>
#include <cassert>
#include <glib.h>

// inspired by ufraw_exiv2.cc:

static void dt_strlcpy_to_utf8(char *dest, size_t dest_max,
	Exiv2::ExifData::iterator pos, Exiv2::ExifData& exifData)
{
  std::string str = pos->print(&exifData);
  std::stringstream ss;
  // (void)Exiv2::ExifTags::printTag(ss, 0x0016, Exiv2::canonIfdId, pos->value(), &exifData);
  (void)Exiv2::CanonMakerNote::printCsLensType(ss, pos->value(), &exifData);
  // std::ostream &os, uint16_t tag, IfdId ifdId, const Value &value, const ExifData *pExifData=0)
  str = ss.str();

  char *s = g_locale_to_utf8(str.c_str(), str.length(),
      NULL, NULL, NULL);
  if ( s!=NULL ) {
    g_strlcpy(dest, s, dest_max);
    g_free(s);
  } else {
    g_strlcpy(dest, str.c_str(), dest_max);
  }
}

extern "C" int dt_exif_read(dt_image_t *img, const char* path)
{
  /* Redirect exiv2 errors to a string buffer */
  std::ostringstream stderror;
  std::streambuf *savecerr = std::cerr.rdbuf();
  std::cerr.rdbuf(stderror.rdbuf());

  try {
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
#if 0
    /* Read shutter time */
    if ( (pos=exifData.findKey(Exiv2::ExifKey("Exif.Photo.ExposureTime")))
        != exifData.end() ) {
      uf_strlcpy_to_utf8(uf->conf->shutterText, max_name, pos, exifData);
      uf->conf->shutter = pos->toFloat ();
    } else if ( (pos=exifData.findKey(
            Exiv2::ExifKey("Exif.Photo.ShutterSpeedValue")))
        != exifData.end() ) {
      uf_strlcpy_to_utf8(uf->conf->shutterText, max_name, pos, exifData);
      uf->conf->shutter = 1.0 / pos->toFloat ();
    }
    /* Read aperture */
    if ( (pos=exifData.findKey(Exiv2::ExifKey("Exif.Photo.FNumber")))
        != exifData.end() ) {
      uf_strlcpy_to_utf8(uf->conf->apertureText, max_name, pos, exifData);
      uf->conf->aperture = pos->toFloat ();
    } else if ( (pos=exifData.findKey(
            Exiv2::ExifKey("Exif.Photo.ApertureValue")))
        != exifData.end() ) {
      uf_strlcpy_to_utf8(uf->conf->apertureText, max_name, pos, exifData);
      uf->conf->aperture = pos->toFloat ();
    }
    /* Read ISO speed */
    if ( (pos=exifData.findKey(Exiv2::ExifKey("Exif.Photo.ISOSpeedRatings")))
        != exifData.end() ) {
      uf_strlcpy_to_utf8(uf->conf->isoText, max_name, pos, exifData);
    } else if ( (pos=exifData.findKey(Exiv2::ExifKey(
              "Exif.CanonSi.ISOSpeed"))) != exifData.end() ) {
      uf_strlcpy_to_utf8(uf->conf->isoText, max_name, pos, exifData);
    } else if ( (pos=exifData.findKey(Exiv2::ExifKey("Exif.Nikon1.ISOSpeed")))
        != exifData.end() ) {
      uf_strlcpy_to_utf8(uf->conf->isoText, max_name, pos, exifData);
    } else if ( (pos=exifData.findKey(Exiv2::ExifKey("Exif.Nikon2.ISOSpeed")))
        != exifData.end() ) {
      uf_strlcpy_to_utf8(uf->conf->isoText, max_name, pos, exifData);
    } else if ( (pos=exifData.findKey(Exiv2::ExifKey("Exif.Nikon3.ISOSpeed")))
        != exifData.end() ) {
      uf_strlcpy_to_utf8(uf->conf->isoText, max_name, pos, exifData);
    } else if ( (pos=exifData.findKey(
            Exiv2::ExifKey("Exif.MinoltaCsNew.ISOSpeed")))
        != exifData.end() ) {
      uf_strlcpy_to_utf8(uf->conf->isoText, max_name, pos, exifData);
    } else if ( (pos=exifData.findKey(
            Exiv2::ExifKey("Exif.MinoltaCsOld.ISOSpeed")))
        != exifData.end() ) {
      uf_strlcpy_to_utf8(uf->conf->isoText, max_name, pos, exifData);
    } else if ( (pos=exifData.findKey(
            Exiv2::ExifKey("Exif.MinoltaCs5D.ISOSpeed")))
        != exifData.end() ) {
      uf_strlcpy_to_utf8(uf->conf->isoText, max_name, pos, exifData);
    } else if ( (pos=exifData.findKey(Exiv2::ExifKey(
              "Exif.MinoltaCs7D.ISOSpeed")))
        != exifData.end() ) {
      uf_strlcpy_to_utf8(uf->conf->isoText, max_name, pos, exifData);
    }
    /* Read focal length */
    if ( (pos=exifData.findKey(Exiv2::ExifKey("Exif.Photo.FocalLength")))
        != exifData.end() ) {
      uf_strlcpy_to_utf8(uf->conf->focalLenText, max_name, pos, exifData);
      uf->conf->focal_len = pos->toFloat ();
    }
    /* Read focal length in 35mm equivalent */
    if ( (pos=exifData.findKey(Exiv2::ExifKey(
              "Exif.Photo.FocalLengthIn35mmFilm")))
        != exifData.end() ) {
      uf_strlcpy_to_utf8(uf->conf->focalLen35Text, max_name, pos, exifData);
    }
#endif
    /* Read lens name */
    if ( (pos=exifData.findKey(Exiv2::ExifKey("Exif.Nikon3.Lens")))
        != exifData.end() ) {
      dt_strlcpy_to_utf8(img->exif_lens, 30, pos, exifData);
#if EXIV2_TEST_VERSION(0,17,91)		/* Exiv2 0.18-pre1 */
    } else if ( (pos=exifData.findKey(Exiv2::ExifKey("Exif.CanonCs.LensType")))
        != exifData.end() ) {
      dt_strlcpy_to_utf8(img->exif_lens, 30, pos, exifData);
#endif
    } else if ( (pos=exifData.findKey(Exiv2::ExifKey("Exif.Canon.0x0095")))
        != exifData.end() ) {
      dt_strlcpy_to_utf8(img->exif_lens, 30, pos, exifData);
    } else if ( (pos=exifData.findKey(Exiv2::ExifKey("Exif.Minolta.LensID")))
        != exifData.end() ) {
      dt_strlcpy_to_utf8(img->exif_lens, 30, pos, exifData);
#if EXIV2_TEST_VERSION(0,16,0)
    } else if ( (pos=exifData.findKey(Exiv2::ExifKey("Exif.Pentax.LensType")))
        != exifData.end() ) {
      dt_strlcpy_to_utf8(img->exif_lens, 30, pos, exifData);
#endif
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

    if ( (pos=exifData.findKey(Exiv2::ExifKey("Exif.Image.Make")))
        != exifData.end() ) {
      uf_strlcpy_to_utf8(uf->conf->real_make, max_name, pos, exifData);
    }
    if ( (pos=exifData.findKey(Exiv2::ExifKey("Exif.Image.Model")))
        != exifData.end() ) {
      uf_strlcpy_to_utf8(uf->conf->real_model, max_name, pos, exifData);
    }
#endif

#if 0 // TODO: use this to replace libexif during writing!
    /* Store all EXIF data read in. */
#if EXIV2_TEST_VERSION(0,17,91)		/* Exiv2 0.18-pre1 */
    Exiv2::Blob blob;
    Exiv2::ExifParser::encode(blob, Exiv2::bigEndian, exifData);
    uf->inputExifBufLen = blob.size();
    uf->inputExifBuf = g_new(unsigned char, uf->inputExifBufLen);
    memcpy(uf->inputExifBuf, &blob[0], blob.size());
#else
    Exiv2::DataBuf buf(exifData.copy());
    uf->inputExifBufLen = buf.size_;
    uf->inputExifBuf = g_new(unsigned char, uf->inputExifBufLen);
    memcpy(uf->inputExifBuf, buf.pData_, buf.size_);
#endif
    ufraw_message(UFRAW_SET_LOG, "EXIF data read using exiv2, buflen %d\n",
        uf->inputExifBufLen);
    g_strlcpy(uf->conf->exifSource, EXV_PACKAGE_STRING, max_name);

    ufraw_message(UFRAW_SET_LOG, "%s\n", stderror.str().c_str());
#endif
    std::cerr.rdbuf(savecerr);

    return 0;
  }
  catch (Exiv2::AnyError& e)
  {
    std::cerr.rdbuf(savecerr);
    std::string s(e.what());
    std::cerr << s << std::endl;
    return 1;
  }
}
