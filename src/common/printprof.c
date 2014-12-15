/*
    This file is part of darktable,
    copyright (c) 2014 pascal obry

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

    NOTE: this code is highly inspired from the tifficc tool from Little-CMS
*/

#include <glib.h>
#include <unistd.h>
#include "lcms2_plugin.h"
#include "tiffio.h"
#include "common/printprof.h"

typedef struct dt_print_profile_data_t
{
  cmsBool BlackWhiteCompensation;
  int     Width;
  cmsBool GamutCheck;

  int Intent;
  int ProofingIntent;
  int PrecalcMode ;

  cmsFloat64Number ObserverAdaptationState;

  const char *cInpProf;
  const char *cOutProf;
  TIFF *in, *out;
} dt_print_profile_data_t;

// Out of mememory is a fatal error
static
void OutOfMem(cmsUInt32Number size)
{
  fprintf(stderr, "error: out of memory on allocating %d bytes.", size);
}

// Return a pixel type on depending on the number of channels
int PixelTypeFromChanCount(int ColorChannels)
{
    switch (ColorChannels) {

        case 1: return PT_GRAY;
        case 2: return PT_MCH2;
        case 3: return PT_MCH3;
        case 4: return PT_CMYK;
        case 5: return PT_MCH5;
        case 6: return PT_MCH6;
        case 7: return PT_MCH7;
        case 8: return PT_MCH8;
        case 9: return PT_MCH9;
        case 10: return PT_MCH10;
        case 11: return PT_MCH11;
        case 12: return PT_MCH12;
        case 13: return PT_MCH13;
        case 14: return PT_MCH14;
        case 15: return PT_MCH15;

        default:

            fprintf(stderr, "error: what a weird separation of %d channels?!?!", ColorChannels);
            return -1;
    }
}

// Return number of channels of pixel type
int ChanCountFromPixelType(int ColorChannels)
{
    switch (ColorChannels) {

      case PT_GRAY: return 1;

      case PT_RGB:
      case PT_CMY:
      case PT_Lab:
      case PT_YUV:
      case PT_YCbCr: return 3;

      case PT_CMYK: return 4 ;
      case PT_MCH2: return 2 ;
      case PT_MCH3: return 3 ;
      case PT_MCH4: return 4 ;
      case PT_MCH5: return 5 ;
      case PT_MCH6: return 6 ;
      case PT_MCH7: return 7 ;
      case PT_MCH8: return 8 ;
      case PT_MCH9: return 9 ;
      case PT_MCH10: return 10;
      case PT_MCH11: return 11;
      case PT_MCH12: return 12;
      case PT_MCH13: return 12;
      case PT_MCH14: return 14;
      case PT_MCH15: return 15;

      default:

          fprintf(stderr, "error: unsupported color space of %d channels", ColorChannels);
          return -1;
    }
}

// -----------------------------------------------------------------------------------------------

// In TIFF, Lab is encoded in a different way, so let's use the plug-in
// capabilities of lcms2 to change the meaning of TYPE_Lab_8.

// * 0xffff / 0xff00 = (255 * 257) / (255 * 256) = 257 / 256
static int FromLabV2ToLabV4(int x)
{
    int a;

    a = ((x << 8) | x) >> 8;  // * 257 / 256
    if ( a > 0xffff) return 0xffff;
    return a;
}

// * 0xf00 / 0xffff = * 256 / 257
static int FromLabV4ToLabV2(int x)
{
    return ((x << 8) + 0x80) / 257;
}

// Formatter for 8bit Lab TIFF (photometric 8)
static
unsigned char* UnrollTIFFLab8(struct _cmstransform_struct* CMMcargo,
                              register cmsUInt16Number wIn[],
                              register cmsUInt8Number* accum,
                              register cmsUInt32Number Stride)
{
    wIn[0] = (cmsUInt16Number) FromLabV2ToLabV4((accum[0]) << 8);
    wIn[1] = (cmsUInt16Number) FromLabV2ToLabV4(((accum[1] > 127) ? (accum[1] - 128) : (accum[1] + 128)) << 8);
    wIn[2] = (cmsUInt16Number) FromLabV2ToLabV4(((accum[2] > 127) ? (accum[2] - 128) : (accum[2] + 128)) << 8);

    return accum + 3;
}

// Formatter for 16bit Lab TIFF (photometric 8)
static
unsigned char* UnrollTIFFLab16(struct _cmstransform_struct* CMMcargo,
                              register cmsUInt16Number wIn[],
                              register cmsUInt8Number* accum,
                              register cmsUInt32Number Stride )
{
    cmsUInt16Number* accum16 = (cmsUInt16Number*) accum;

    wIn[0] = (cmsUInt16Number) FromLabV2ToLabV4(accum16[0]);
    wIn[1] = (cmsUInt16Number) FromLabV2ToLabV4(((accum16[1] > 0x7f00) ? (accum16[1] - 0x8000) : (accum16[1] + 0x8000)) );
    wIn[2] = (cmsUInt16Number) FromLabV2ToLabV4(((accum16[2] > 0x7f00) ? (accum16[2] - 0x8000) : (accum16[2] + 0x8000)) );

    return accum + 3 * sizeof(cmsUInt16Number);
}

static
unsigned char* PackTIFFLab8(struct _cmstransform_struct* CMMcargo,
                            register cmsUInt16Number wOut[],
                            register cmsUInt8Number* output,
                            register cmsUInt32Number Stride)
{
    int a, b;

    *output++ = (cmsUInt8Number) (FromLabV4ToLabV2(wOut[0] + 0x0080) >> 8);

    a = (FromLabV4ToLabV2(wOut[1]) + 0x0080) >> 8;
    b = (FromLabV4ToLabV2(wOut[2]) + 0x0080) >> 8;

    *output++ = (cmsUInt8Number) ((a < 128) ? (a + 128) : (a - 128));
    *output++ = (cmsUInt8Number) ((b < 128) ? (b + 128) : (b - 128));

    return output;
}

static
unsigned char* PackTIFFLab16(struct _cmstransform_struct* CMMcargo,
                            register cmsUInt16Number wOut[],
                            register cmsUInt8Number* output,
                            register cmsUInt32Number Stride)
{
    int a, b;
    cmsUInt16Number* output16 = (cmsUInt16Number*) output;

    *output16++ = (cmsUInt16Number) FromLabV4ToLabV2(wOut[0]);

    a = FromLabV4ToLabV2(wOut[1]);
    b = FromLabV4ToLabV2(wOut[2]);

    *output16++ = (cmsUInt16Number) ((a < 0x7f00) ? (a + 0x8000) : (a - 0x8000));
    *output16++ = (cmsUInt16Number) ((b < 0x7f00) ? (b + 0x8000) : (b - 0x8000));

    return (cmsUInt8Number*) output16;
}


static
cmsFormatter TiffFormatterFactory(cmsUInt32Number Type,
                                  cmsFormatterDirection Dir,
                                  cmsUInt32Number dwFlags)
{
    cmsFormatter Result = { NULL };
    int bps           = T_BYTES(Type);
    int IsTiffSpecial = (Type >> 23) & 1;

    if (IsTiffSpecial && !(dwFlags & CMS_PACK_FLAGS_FLOAT))
    {
        if (Dir == cmsFormatterInput)
        {
          Result.Fmt16 = (bps == 1) ? UnrollTIFFLab8 : UnrollTIFFLab16;
        }
        else
          Result.Fmt16 = (bps == 1) ? PackTIFFLab8 : PackTIFFLab16;
    }

    return Result;
}

static cmsPluginFormatters TiffLabPlugin = { {cmsPluginMagicNumber, 2000, cmsPluginFormattersSig, NULL}, TiffFormatterFactory };



// Build up the pixeltype descriptor
static
cmsUInt32Number GetInputPixelType(TIFF *Bank)
{
    uint16 Photometric, bps, spp, extra, PlanarConfig, *info;
    uint16 Compression, reverse = 0;
    int ColorChannels, IsPlanar = 0, pt = 0, IsFlt;
    int labTiffSpecial = FALSE;

    TIFFGetField(Bank,           TIFFTAG_PHOTOMETRIC,   &Photometric);
    TIFFGetFieldDefaulted(Bank,  TIFFTAG_BITSPERSAMPLE, &bps);

    if (bps == 1)
        fprintf(stderr, "error: sorry, bilevel TIFFs has nothing to do with ICC profiles");

    if (bps != 8 && bps != 16 && bps != 32)
        fprintf(stderr, "error: sorry, 8, 16 or 32 bits per sample only");

    TIFFGetFieldDefaulted(Bank, TIFFTAG_SAMPLESPERPIXEL, &spp);
    TIFFGetFieldDefaulted(Bank, TIFFTAG_PLANARCONFIG, &PlanarConfig);

    switch (PlanarConfig) {

     case PLANARCONFIG_CONTIG: IsPlanar = 0; break;
     case PLANARCONFIG_SEPARATE: IsPlanar = 1; break;
     default:

         fprintf(stderr, "error: unsupported planar configuration (=%d) ", (int) PlanarConfig);
    }

    // If Samples per pixel == 1, PlanarConfiguration is irrelevant and need
    // not to be included.

    if (spp == 1) IsPlanar = 0;

    // Any alpha?

    TIFFGetFieldDefaulted(Bank, TIFFTAG_EXTRASAMPLES, &extra, &info);

    ColorChannels = spp - extra;

    switch (Photometric) {

    case PHOTOMETRIC_MINISWHITE:

        reverse = 1;

        // ... fall through ...

    case PHOTOMETRIC_MINISBLACK:
        pt = PT_GRAY;
        break;

    case PHOTOMETRIC_RGB:
        pt = PT_RGB;
        break;


     case PHOTOMETRIC_PALETTE:
         fprintf(stderr, "error: sorry, palette images not supported");
         break;

     case PHOTOMETRIC_SEPARATED:

         pt = PixelTypeFromChanCount(ColorChannels);
         break;

     case PHOTOMETRIC_YCBCR:
         TIFFGetField(Bank, TIFFTAG_COMPRESSION, &Compression);
         {
             uint16 subx, suby;

             pt = PT_YCbCr;
             TIFFGetFieldDefaulted(Bank, TIFFTAG_YCBCRSUBSAMPLING, &subx, &suby);
             if (subx != 1 || suby != 1)
                 fprintf(stderr, "error: sorry, subsampled images not supported");

         }
         break;

     case PHOTOMETRIC_ICCLAB:
         pt = PT_LabV2;
         break;

     case PHOTOMETRIC_CIELAB:
         pt = PT_Lab;
         labTiffSpecial = TRUE;
         break;


     case PHOTOMETRIC_LOGLUV:      // CIE Log2(L) (u',v')

         TIFFSetField(Bank, TIFFTAG_SGILOGDATAFMT, SGILOGDATAFMT_16BIT);
         pt = PT_YUV;             // *ICCSpace = icSigLuvData;
         bps = 16;                // 16 bits forced by LibTiff
         break;

     default:
         fprintf(stderr, "error: unsupported TIFF color space (Photometric %d)", Photometric);
    }

    // Convert bits per sample to bytes per sample

    bps >>= 3;
    IsFlt = (bps == 0) || (bps == 4);

    return (FLOAT_SH(IsFlt)|COLORSPACE_SH(pt)|PLANAR_SH(IsPlanar)|EXTRA_SH(extra)|CHANNELS_SH(ColorChannels)|BYTES_SH(bps)|FLAVOR_SH(reverse) | (labTiffSpecial << 23) );
}



// Rearrange pixel type to build output descriptor
static
cmsUInt32Number ComputeOutputFormatDescriptor(cmsUInt32Number dwInput, int OutColorSpace, int bps)
{
    int IsPlanar  = T_PLANAR(dwInput);
    int Channels  = ChanCountFromPixelType(OutColorSpace);
    int IsFlt = (bps == 0) || (bps == 4);

    return (FLOAT_SH(IsFlt)|COLORSPACE_SH(OutColorSpace)|PLANAR_SH(IsPlanar)|CHANNELS_SH(Channels)|BYTES_SH(bps));
}

// Strip based transforms

static
int StripBasedXform(cmsHTRANSFORM hXForm, TIFF* in, TIFF* out, int nPlanes)
{
  tsize_t BufSizeIn  = TIFFStripSize(in);
  tsize_t BufSizeOut = TIFFStripSize(out);
  unsigned char *BufferIn, *BufferOut;
  ttile_t i, StripCount = TIFFNumberOfStrips(in) / nPlanes;
  uint32 sw;
  uint32 sl;
  uint32 iml;
  int j;
  int PixelCount;

  TIFFGetFieldDefaulted(in, TIFFTAG_IMAGEWIDTH,  &sw);
  TIFFGetFieldDefaulted(in, TIFFTAG_ROWSPERSTRIP, &sl);
  TIFFGetFieldDefaulted(in, TIFFTAG_IMAGELENGTH, &iml);

  // It is possible to get infinite rows per strip
  if (sl == 0 || sl > iml)
    sl = iml;   // One strip for whole image

  BufferIn = (unsigned char *) _TIFFmalloc(BufSizeIn * nPlanes);
  if (!BufferIn) OutOfMem(BufSizeIn * nPlanes);

  BufferOut = (unsigned char *) _TIFFmalloc(BufSizeOut * nPlanes);
  if (!BufferOut) OutOfMem(BufSizeOut * nPlanes);

  for (i = 0; i < StripCount; i++)
  {
    for (j=0; j < nPlanes; j++)
    {
      if (TIFFReadEncodedStrip(in, i + (j * StripCount),
                               BufferIn + (j * BufSizeIn), BufSizeIn) < 0)   goto cleanup;
    }

    PixelCount = (int) sw * (iml < sl ? iml : sl);
    iml -= sl;

    cmsDoTransform(hXForm, BufferIn, BufferOut, PixelCount);

    for (j=0; j < nPlanes; j++)
    {
      if (TIFFWriteEncodedStrip(out, i + (j * StripCount),
                                BufferOut + j * BufSizeOut, BufSizeOut) < 0) goto cleanup;
    }
  }

  _TIFFfree(BufferIn);
  _TIFFfree(BufferOut);
  return 1;

 cleanup:

  _TIFFfree(BufferIn);
  _TIFFfree(BufferOut);
  return 0;
}


// Creates minimum required tags
static
void WriteOutputTags(TIFF *out, int Colorspace, int BytesPerSample)
{
  int BitsPerSample = (8 * BytesPerSample);
  int nChannels     = ChanCountFromPixelType(Colorspace);
/*
  uint16 Extra[] = { EXTRASAMPLE_UNASSALPHA,
                     EXTRASAMPLE_UNASSALPHA,
                     EXTRASAMPLE_UNASSALPHA,
                     EXTRASAMPLE_UNASSALPHA,
                     EXTRASAMPLE_UNASSALPHA,
                     EXTRASAMPLE_UNASSALPHA,
                     EXTRASAMPLE_UNASSALPHA,
                     EXTRASAMPLE_UNASSALPHA,
                     EXTRASAMPLE_UNASSALPHA,
                     EXTRASAMPLE_UNASSALPHA,
                     EXTRASAMPLE_UNASSALPHA
  };
*/

  switch (Colorspace) {

  case PT_GRAY:
      TIFFSetField(out, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK);
      TIFFSetField(out, TIFFTAG_SAMPLESPERPIXEL, 1);
      TIFFSetField(out, TIFFTAG_BITSPERSAMPLE, BitsPerSample);
      break;

  case PT_RGB:
      TIFFSetField(out, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
      TIFFSetField(out, TIFFTAG_SAMPLESPERPIXEL, 3);
      TIFFSetField(out, TIFFTAG_BITSPERSAMPLE, BitsPerSample);
      break;

  case PT_CMY:
      TIFFSetField(out, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_SEPARATED);
      TIFFSetField(out, TIFFTAG_SAMPLESPERPIXEL, 3);
      TIFFSetField(out, TIFFTAG_INKSET, 2);
      TIFFSetField(out, TIFFTAG_BITSPERSAMPLE, BitsPerSample);
      break;

  case PT_CMYK:
      TIFFSetField(out, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_SEPARATED);
      TIFFSetField(out, TIFFTAG_SAMPLESPERPIXEL, 4);
      TIFFSetField(out, TIFFTAG_INKSET, INKSET_CMYK);
      TIFFSetField(out, TIFFTAG_BITSPERSAMPLE, BitsPerSample);
      break;

  case PT_Lab:
      if (BitsPerSample == 16)
          TIFFSetField(out, TIFFTAG_PHOTOMETRIC, 9);
      else
          TIFFSetField(out, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_CIELAB);
      TIFFSetField(out, TIFFTAG_SAMPLESPERPIXEL, 3);
      TIFFSetField(out, TIFFTAG_BITSPERSAMPLE, BitsPerSample);    // Needed by TIFF Spec
      break;


      // Multi-ink separations
  case PT_MCH2:
  case PT_MCH3:
  case PT_MCH4:
  case PT_MCH5:
  case PT_MCH6:
  case PT_MCH7:
  case PT_MCH8:
  case PT_MCH9:
  case PT_MCH10:
  case PT_MCH11:
  case PT_MCH12:
  case PT_MCH13:
  case PT_MCH14:
  case PT_MCH15:

      TIFFSetField(out, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_SEPARATED);
      TIFFSetField(out, TIFFTAG_SAMPLESPERPIXEL, nChannels);
      TIFFSetField(out, TIFFTAG_INKSET, 2);
      TIFFSetField(out, TIFFTAG_NUMBEROFINKS, nChannels);

      TIFFSetField(out, TIFFTAG_BITSPERSAMPLE, BitsPerSample);
      break;


  default:
      fprintf(stderr, "error: unsupported output colorspace");
    }
}


// Copies a bunch of tages

static
void CopyOtherTags(TIFF* in, TIFF* out)
{
#define CopyField(tag, v)                                       \
  if (TIFFGetField(in, tag, &v)) TIFFSetField(out, tag, v)

  short shortv;
  uint32 ow, ol;
  cmsFloat32Number floatv;
  char *stringv;
  uint32 longv;

  CopyField(TIFFTAG_SUBFILETYPE, longv);

  TIFFGetField(in, TIFFTAG_IMAGEWIDTH, &ow);
  TIFFGetField(in, TIFFTAG_IMAGELENGTH, &ol);

  TIFFSetField(out, TIFFTAG_IMAGEWIDTH, ow);
  TIFFSetField(out, TIFFTAG_IMAGELENGTH, ol);

  CopyField(TIFFTAG_PLANARCONFIG, shortv);
  CopyField(TIFFTAG_COMPRESSION, shortv);

  CopyField(TIFFTAG_PREDICTOR, shortv);

  CopyField(TIFFTAG_THRESHHOLDING, shortv);
  CopyField(TIFFTAG_FILLORDER, shortv);
  CopyField(TIFFTAG_ORIENTATION, shortv);
  CopyField(TIFFTAG_MINSAMPLEVALUE, shortv);
  CopyField(TIFFTAG_MAXSAMPLEVALUE, shortv);
  CopyField(TIFFTAG_XRESOLUTION, floatv);
  CopyField(TIFFTAG_YRESOLUTION, floatv);
  CopyField(TIFFTAG_RESOLUTIONUNIT, shortv);
  CopyField(TIFFTAG_ROWSPERSTRIP, longv);
  CopyField(TIFFTAG_XPOSITION, floatv);
  CopyField(TIFFTAG_YPOSITION, floatv);
  CopyField(TIFFTAG_IMAGEDEPTH, longv);
  CopyField(TIFFTAG_TILEDEPTH, longv);

  CopyField(TIFFTAG_TILEWIDTH,  longv);
  CopyField(TIFFTAG_TILELENGTH, longv);

  CopyField(TIFFTAG_ARTIST, stringv);
  CopyField(TIFFTAG_IMAGEDESCRIPTION, stringv);
  CopyField(TIFFTAG_MAKE, stringv);
  CopyField(TIFFTAG_MODEL, stringv);

  CopyField(TIFFTAG_DATETIME, stringv);
  CopyField(TIFFTAG_HOSTCOMPUTER, stringv);
  CopyField(TIFFTAG_PAGENAME, stringv);
  CopyField(TIFFTAG_DOCUMENTNAME, stringv);
}


static
cmsHPROFILE GetTIFFProfile(TIFF* in)
{
  cmsCIExyYTRIPLE Primaries;
  cmsFloat32Number* chr;
  cmsCIExyY WhitePoint;
  cmsFloat32Number* wp;
  int i;
  cmsToneCurve* Curve[3];
  cmsUInt16Number *gmr, *gmg, *gmb;
  cmsHPROFILE hProfile;
  cmsUInt32Number EmbedLen;
  cmsUInt8Number* EmbedBuffer;

  if (TIFFGetField(in, TIFFTAG_ICCPROFILE, &EmbedLen, &EmbedBuffer))
  {
    hProfile = cmsOpenProfileFromMem(EmbedBuffer, EmbedLen);
    if (hProfile) return hProfile;
  }

  // Try to see if "colorimetric" tiff

  if (TIFFGetField(in, TIFFTAG_PRIMARYCHROMATICITIES, &chr))
  {
    Primaries.Red.x   =  chr[0];
    Primaries.Red.y   =  chr[1];
    Primaries.Green.x =  chr[2];
    Primaries.Green.y =  chr[3];
    Primaries.Blue.x  =  chr[4];
    Primaries.Blue.y  =  chr[5];

    Primaries.Red.Y = Primaries.Green.Y = Primaries.Blue.Y = 1.0;

    if (TIFFGetField(in, TIFFTAG_WHITEPOINT, &wp))
    {
      WhitePoint.x = wp[0];
      WhitePoint.y = wp[1];
      WhitePoint.Y = 1.0;

      // Transferfunction is a bit harder....

      TIFFGetFieldDefaulted(in, TIFFTAG_TRANSFERFUNCTION,
                            &gmr,
                            &gmg,
                            &gmb);

      Curve[0] = cmsBuildTabulatedToneCurve16(NULL, 256, gmr);
      Curve[1] = cmsBuildTabulatedToneCurve16(NULL, 256, gmg);
      Curve[2] = cmsBuildTabulatedToneCurve16(NULL, 256, gmb);

      hProfile = cmsCreateRGBProfileTHR(NULL, &WhitePoint, &Primaries, Curve);

      for (i=0; i < 3; i++)
        cmsFreeToneCurve(Curve[i]);
      return hProfile;
    }
  }
  return NULL;
}

// Transform one image
static
int TransformImage(dt_print_profile_data_t *pp)
{
  cmsHPROFILE hIn, hOut, hProof;
  cmsHTRANSFORM xform;
  cmsUInt32Number wInput, wOutput;
  int OutputColorSpace;
  int bps = pp->Width / 8;
  cmsUInt32Number dwFlags = 0;
  int nPlanes;

  // Observer adaptation state (only meaningful on absolute colorimetric intent)

  cmsSetAdaptationState(pp->ObserverAdaptationState);

  if (pp->BlackWhiteCompensation)
    dwFlags |= cmsFLAGS_BLACKPOINTCOMPENSATION;

  switch (pp->PrecalcMode)
  {
  case 0: dwFlags |= cmsFLAGS_NOOPTIMIZE; break;
  case 2: dwFlags |= cmsFLAGS_HIGHRESPRECALC; break;
  case 3: dwFlags |= cmsFLAGS_LOWRESPRECALC; break;
  case 1: break;

  default: fprintf(stderr, "error: unknown precalculation mode '%d'", pp->PrecalcMode);
  }

  if (pp->GamutCheck)
    dwFlags |= cmsFLAGS_GAMUTCHECK;

  hProof = NULL;
  hOut = NULL;

  hIn =  GetTIFFProfile(pp->in);

  if (!hIn)
    return 0;

  hOut = cmsOpenProfileFromFileTHR(NULL, pp->cOutProf, "r");

  // Take input color space

  wInput = GetInputPixelType(pp->in);

  // Assure both, input profile and input TIFF are on same colorspace

  if (_cmsLCMScolorSpace(cmsGetColorSpace(hIn)) != (int) T_COLORSPACE(wInput))
    fprintf(stderr, "error: input profile is not operating in proper color space");

  OutputColorSpace = _cmsLCMScolorSpace(cmsGetColorSpace(hOut));

  wOutput = ComputeOutputFormatDescriptor(wInput, OutputColorSpace, bps);

  WriteOutputTags(pp->out, OutputColorSpace, bps);
  CopyOtherTags(pp->in, pp->out);

  xform = cmsCreateProofingTransform(hIn, wInput,
                                     hOut, wOutput,
                                     hProof, pp->Intent,
                                     pp->ProofingIntent,
                                     dwFlags);
  cmsCloseProfile(hIn);
  cmsCloseProfile(hOut);

  if (hProof)
    cmsCloseProfile(hProof);

  if (xform == NULL) return 0;

  // Planar stuff
  if (T_PLANAR(wInput))
    nPlanes = T_CHANNELS(wInput) + T_EXTRA(wInput);
  else
    nPlanes = 1;

  StripBasedXform(xform, pp->in, pp->out, nPlanes);

  cmsDeleteTransform(xform);

  TIFFWriteDirectory(pp->out);

  return 1;
}

int dt_apply_printer_profile(char *filename, char *profile, int intent)
{
  char outfilename[PATH_MAX];
  dt_print_profile_data_t pp;
  int res;

  pp.BlackWhiteCompensation  = FALSE;
  pp.Width                   = 8;
  pp.GamutCheck              = FALSE;
  pp.Intent                  = intent;
  pp.ProofingIntent          = INTENT_PERCEPTUAL;
  pp.PrecalcMode             = 1;
  pp.ObserverAdaptationState = 1.0;  // According ICC 4.3 this is the default
  pp.cInpProf                = NULL;
  pp.cOutProf                = NULL;
  pp.in                      = NULL;
  pp.out                     = NULL;

  cmsPlugin(&TiffLabPlugin);

  pp.cOutProf = profile;

  pp.in = TIFFOpen(filename, "r");

  g_strlcpy(outfilename, filename, PATH_MAX);
  g_strlcat(outfilename, ".icc.tif", PATH_MAX);

  pp.out = TIFFOpen(outfilename, "w");

  if (pp.out == NULL) {
    TIFFClose(pp.in);
  }

  do {
    res = TransformImage(&pp);
  } while (TIFFReadDirectory(pp.in));


  TIFFClose(pp.in);
  TIFFClose(pp.out);

  if (res)
  {
    unlink(filename);
    rename(outfilename, filename);
    return 1;
  }
  else
  {
    unlink(outfilename);
    return 0;
  }
}
