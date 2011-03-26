cmake_minimum_required(VERSION 2.6)

install(FILES data/cameras.xml data/showcameras.xsl DESTINATION ${SHARE_INSTALL}/rawspeed)

set(RAWSPEED_SOURCES
  "RawSpeed/ArwDecoder.cpp"
  "RawSpeed/BitPumpJPEG.cpp"
  "RawSpeed/BitPumpMSB32.cpp"
  "RawSpeed/BitPumpMSB.cpp"
  "RawSpeed/BitPumpPlain.cpp"
  "RawSpeed/BlackArea.cpp"
  "RawSpeed/ByteStream.cpp"
  "RawSpeed/ByteStreamSwap.cpp"
  "RawSpeed/Camera.cpp"
  "RawSpeed/CameraMetaData.cpp"
  "RawSpeed/CameraMetadataException.cpp"
  "RawSpeed/ColorFilterArray.cpp"
  "RawSpeed/Common.cpp"
  "RawSpeed/Cr2Decoder.cpp"
  "RawSpeed/DngDecoder.cpp"
  "RawSpeed/DngDecoderSlices.cpp"
  "RawSpeed/FileIOException.cpp"
  "RawSpeed/FileMap.cpp"
  "RawSpeed/FileReader.cpp"
  "RawSpeed/IOException.cpp"
  "RawSpeed/LJpegDecompressor.cpp"
  "RawSpeed/LJpegPlain.cpp"
  "RawSpeed/NefDecoder.cpp"
  "RawSpeed/NikonDecompressor.cpp"
  "RawSpeed/OrfDecoder.cpp"
  "RawSpeed/PefDecoder.cpp"
  "RawSpeed/PentaxDecompressor.cpp"
  "RawSpeed/RawDecoder.cpp"
  "RawSpeed/RawDecoderException.cpp"
  "RawSpeed/RawImage.cpp"
  "RawSpeed/Rw2Decoder.cpp"
  "RawSpeed/StdAfx.cpp"
  "RawSpeed/TiffEntryBE.cpp"
  "RawSpeed/TiffEntry.cpp"
  "RawSpeed/TiffIFDBE.cpp"
  "RawSpeed/TiffIFD.cpp"
  "RawSpeed/TiffParser.cpp"
  "RawSpeed/TiffParserException.cpp"
  "RawSpeed/TiffParserHeaderless.cpp"
  "RawSpeed/TiffParserOlympus.cpp"
  "RawSpeed/RawImageDataU16.cpp"
  "RawSpeed/RawImageDataFloat.cpp"
  "RawSpeed/SrwDecoder.cpp"
    )

#
# build librawspeed
#
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fPIC")

add_library(rawspeed STATIC ${RAWSPEED_SOURCES})
target_link_libraries(rawspeed)

