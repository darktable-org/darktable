#ifndef RAW_DECODER_H
#define RAW_DECODER_H

#include "RawDecoderException.h"
#include "FileMap.h"
#include "BitPumpJPEG.h" // Includes bytestream
#include "RawImage.h"
#include "BitPumpMSB.h"
#include "BitPumpMSB16.h"
#include "BitPumpMSB32.h"
#include "BitPumpPlain.h"
#include "CameraMetaData.h"
#include "TiffIFD.h"

/* 
    RawSpeed - RAW file decoder.

    Copyright (C) 2009-2014 Klaus Post
    Copyright (C) 2014 Pedro Côrte-Real

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

    http://www.klauspost.com
*/

namespace RawSpeed {

class RawDecoder;

/* Class with information delivered to RawDecoder::decodeThreaded() */
class RawDecoderThread
{
  public:
    RawDecoderThread() {error = 0; taskNo = -1;};
    uint32 start_y;
    uint32 end_y;
    const char* error;
    pthread_t threadid;
    RawDecoder* parent;
    uint32 taskNo;
};

class RawDecoder 
{
public:
  /* Construct decoder instance - FileMap is a filemap of the file to be decoded */
  /* The FileMap is not owned by this class, will not be deleted, and must remain */
  /* valid while this object exists */
  RawDecoder(FileMap* file);
  virtual ~RawDecoder(void);

  /* Check if the decoder can decode the image from this camera */
  /* A RawDecoderException will be thrown if the camera isn't supported */
  /* Unknown cameras does NOT generate any specific feedback */
  /* This function must be overridden by actual decoders */
  void checkSupport(CameraMetaData *meta);

  /* Attempt to decode the image */
  /* A RawDecoderException will be thrown if the image cannot be decoded, */
  /* and there will not be any data in the mRaw image. */
  RawImage decodeRaw();

  /* This will apply metadata information from the camera database, */
  /* such as crop, black+white level, etc. */
  /* This function is expected to use the protected "setMetaData" */
  /* after retrieving make, model and mode if applicate. */
  /* If meta-data is set during load, this function can be empty. */
  /* The image is expected to be cropped after this, but black/whitelevel */
  /* compensation is not expected to be applied to the image */
  void decodeMetaData(CameraMetaData *meta);

  /* Called function for filters that are capable of doing simple multi-threaded decode */
  /* The delivered class gives information on what part of the image should be decoded. */
  virtual void decodeThreaded(RawDecoderThread* t);

  /* Allows access to the root IFD structure */
  /* If image isn't TIFF based NULL will be returned */
  virtual TiffIFD* getRootIFD() {return NULL;}

  /* The decoded image - undefined if image has not or could not be decoded. */
  /* Remember this is automatically refcounted, so a reference is retained until this class is destroyed */
  RawImage mRaw; 

  /* You can set this if you do not want Rawspeed to attempt to decode images, */
  /* where it does not have reliable information about CFA, cropping, black and white point */
  /* It is pretty safe to leave this disabled (default behaviour), but if you do not want to */
  /* support unknown cameras, you can enable this */
  /* DNGs are always attempted to be decoded, so this variable has no effect on DNGs */
  bool failOnUnknown;

  /* Set how to handle bad pixels. */
  /* If you disable this parameter, no bad pixel interpolation will be done */
  bool interpolateBadPixels;

  /* Apply stage 1 DNG opcodes. */
  /* This usually maps out bad pixels, etc */
  bool applyStage1DngOpcodes;

  /* Apply crop - if false uncropped image is delivered */
  bool applyCrop;

  /* This will skip all corrections, and deliver the raw data */
  /* This will skip any compression curves or other things that */
  /* is needed to get the correct values */
  /* Only enable if you are sure that is what you want */
  bool uncorrectedRawValues;

  /* Should Fuji images be rotated? */
  bool fujiRotate;

  /* Vector of objects that will be destroyed alongside the decoder */
  vector<FileMap*> ownedObjects;

  /* Retrieve the main RAW chunk */
  /* Returns NULL if unknown */
  virtual FileMap* getCompressedData() {return NULL;}

protected:
  /* Attempt to decode the image */
  /* A RawDecoderException will be thrown if the image cannot be decoded, */
  /* and there will not be any data in the mRaw image. */
  /* This function must be overridden by actual decoders. */
  virtual RawImage decodeRawInternal() = 0;
  virtual void decodeMetaDataInternal(CameraMetaData *meta) = 0;
  virtual void checkSupportInternal(CameraMetaData *meta) = 0;

  /* Helper function for decoders - splits the image vertically and starts of decoder threads */
  /* The function returns when all threads are done */
  /* All errors are silently pushed into the "errors" array.*/
  /* If all threads report an error an exception will be thrown*/
  void startThreads();

  /* Helper function for decoders -  */
  /* The function returns when all tasks are done */
  /* All errors are silently pushed into the "errors" array.*/
  /* If all threads report an error an exception will be thrown*/
  void startTasks(uint32 tasks);

  /* Check the camera and mode against the camera database. */
  /* A RawDecoderException will be thrown if the camera isn't supported */
  /* Unknown cameras does NOT generate any errors, but returns false */
  bool checkCameraSupported(CameraMetaData *meta, string make, string model, string mode);

  /* Helper function for decodeMetaData(), that find the camera in the CameraMetaData DB */
  /* and sets common settings such as crop, black- white level, and sets CFA information */
  virtual void setMetaData(CameraMetaData *meta, string make, string model, string mode, int iso_speed = 0);

  /* Helper function for decoders, that will unpack uncompressed image data */
  /* input: Input image, positioned at first pixel */
  /* size: Size of the image to decode in pixels */
  /* offset: offset to write the data into the final image */
  /* inputPitch: Number of bytes between each line in the input image */
  /* bitPerPixel: Number of bits to read for each input pixel. */
  /* order: Order of the bits - see Common.h for possibilities. */
  void readUncompressedRaw(ByteStream &input, iPoint2D& size, iPoint2D& offset, int inputPitch, int bitPerPixel, BitOrder order);

  /* Faster version for unpacking 12 bit LSB data */
  void Decode12BitRaw(ByteStream &input, uint32 w, uint32 h);

  /* Faster version for unpacking 12 bit LSB data with a control byte every 10 pixels */
  void Decode12BitRawWithControl(ByteStream &input, uint32 w, uint32 h);

  /* Faster version for unpacking 12 bit MSB data with a control byte every 10 pixels */
  void Decode12BitRawBEWithControl(ByteStream &input, uint32 w, uint32 h);

  /* Faster version for unpacking 12 bit MSB data */
  void Decode12BitRawBE(ByteStream &input, uint32 w, uint32 h);

  /* Faster version for unpacking 12 bit MSB data with interlaced lines */
  void Decode12BitRawBEInterlaced(ByteStream &input, uint32 w, uint32 h);

  /* Faster version for reading unpacked 12 bit MSB data */
  void Decode12BitRawBEunpacked(ByteStream &input, uint32 w, uint32 h);

  /* Faster version for reading unpacked 12 bit MSB data that is left aligned (needs >> 4 shift) */
  void Decode12BitRawBEunpackedLeftAligned(ByteStream &input, uint32 w, uint32 h);
  
  /* Faster version for reading unpacked 14 bit MSB data */
  void Decode14BitRawBEunpacked(ByteStream &input, uint32 w, uint32 h);

  /* Faster version for reading unpacked 16 bit LSB data */
  void Decode16BitRawUnpacked(ByteStream &input, uint32 w, uint32 h);

  /* Faster version for reading unpacked 16 bit MSB data */
  void Decode16BitRawBEunpacked(ByteStream &input, uint32 w, uint32 h);

  /* Faster version for reading unpacked 12 bit LSB data */
  void Decode12BitRawUnpacked(ByteStream &input, uint32 w, uint32 h);

  /* Generic decompressor for uncompressed images */
  /* order: Order of the bits - see Common.h for possibilities. */
  void decodeUncompressed(TiffIFD *rawIFD, BitOrder order);

  /* The Raw input file to be decoded */
  FileMap *mFile; 

  /* Decoder version - defaults to 0, but can be overridden by decoders */
  /* This can be used to avoid newer version of an xml file to indicate that a file */
  /* can be decoded, when a specific version of the code is needed */
  /* Higher number in camera xml file: Files for this camera will not be decoded */
  /* Higher number in code than xml: Image will be decoded. */
  int decoderVersion;

  /* Hints set for the camera after checkCameraSupported has been called from the implementation*/
   map<string,string> hints;
};

class RawSlice {
public:
  RawSlice() { h = offset = count = 0;};
  ~RawSlice() {};
  uint32 h;
  uint32 offset;
  uint32 count;
};

} // namespace RawSpeed

#endif
