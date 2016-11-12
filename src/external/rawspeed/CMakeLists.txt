include(CheckCompilerFlagAndEnableIt)

CHECK_COMPILER_FLAG_AND_ENABLE_IT(-Wno-error=shadow)

# should be < 64Kb
math(EXPR MAX_MEANINGFUL_SIZE 16*1024)
CHECK_COMPILER_FLAG_AND_ENABLE_IT(-Wframe-larger-than=${MAX_MEANINGFUL_SIZE})
CHECK_COMPILER_FLAG_AND_ENABLE_IT(-Wstack-usage=${MAX_MEANINGFUL_SIZE})

# as small as possible, but 1Mb+ is ok.
math(EXPR MAX_MEANINGFUL_SIZE 16*1024)
CHECK_COMPILER_FLAG_AND_ENABLE_IT(-Wlarger-than=${MAX_MEANINGFUL_SIZE})

if(USE_XMLLINT)
	add_custom_target(
		validate_cameras_xml ALL
		COMMAND ${Xmllint_BIN} --nonet --valid --noout --schema ${CMAKE_CURRENT_SOURCE_DIR}/data/cameras.xsd ${CMAKE_CURRENT_SOURCE_DIR}/data/cameras.xml
		DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/data/cameras.xml
		WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/
		COMMENT "Checking validity of external/rawspeed/data/cameras.xml"
	)
endif(USE_XMLLINT)

install(FILES data/cameras.xml data/showcameras.xsl DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/darktable/rawspeed)

FILE(GLOB RAWSPEED_SOURCES
        "RawSpeed/AriDecoder.cpp"
        "RawSpeed/ArwDecoder.cpp"
        "RawSpeed/BitPumpJPEG.cpp"
        "RawSpeed/BitPumpMSB.cpp"
        "RawSpeed/BitPumpMSB16.cpp"
        "RawSpeed/BitPumpMSB32.cpp"
        "RawSpeed/BitPumpPlain.cpp"
        "RawSpeed/BlackArea.cpp"
        "RawSpeed/ByteStream.cpp"
        "RawSpeed/ByteStreamSwap.cpp"
        "RawSpeed/Camera.cpp"
        "RawSpeed/CameraMetaData.cpp"
        "RawSpeed/CameraMetadataException.cpp"
        "RawSpeed/CameraSensorInfo.cpp"
        "RawSpeed/CiffEntry.cpp"
        "RawSpeed/CiffIFD.cpp"
        "RawSpeed/CiffParser.cpp"
        "RawSpeed/CiffParserException.cpp"
        "RawSpeed/ColorFilterArray.cpp"
        "RawSpeed/Common.cpp"
        "RawSpeed/Cr2Decoder.cpp"
        "RawSpeed/CrwDecoder.cpp"
        "RawSpeed/DcrDecoder.cpp"
        "RawSpeed/DcsDecoder.cpp"
        "RawSpeed/DngDecoder.cpp"
        "RawSpeed/DngDecoderSlices.cpp"
        "RawSpeed/DngOpcodes.cpp"
        "RawSpeed/ErfDecoder.cpp"
        "RawSpeed/FileIOException.cpp"
        "RawSpeed/FileMap.cpp"
        "RawSpeed/FileReader.cpp"
        "RawSpeed/FileWriter.cpp"
        "RawSpeed/HasselbladDecompressor.cpp"
        "RawSpeed/IOException.cpp"
        "RawSpeed/KdcDecoder.cpp"
        "RawSpeed/LJpegDecompressor.cpp"
        "RawSpeed/LJpegPlain.cpp"
        "RawSpeed/MefDecoder.cpp"
        "RawSpeed/MosDecoder.cpp"
        "RawSpeed/MrwDecoder.cpp"
        "RawSpeed/NakedDecoder.cpp"
        "RawSpeed/NefDecoder.cpp"
        "RawSpeed/NikonDecompressor.cpp"
        "RawSpeed/OrfDecoder.cpp"
        "RawSpeed/PefDecoder.cpp"
        "RawSpeed/PentaxDecompressor.cpp"
        "RawSpeed/RafDecoder.cpp"
        "RawSpeed/RawDecoder.cpp"
        "RawSpeed/RawDecoderException.cpp"
        "RawSpeed/RawImage.cpp"
        "RawSpeed/RawImageDataFloat.cpp"
        "RawSpeed/RawImageDataU16.cpp"
        "RawSpeed/RawParser.cpp"
        "RawSpeed/Rw2Decoder.cpp"
        "RawSpeed/SrwDecoder.cpp"
        "RawSpeed/StdAfx.cpp"
        "RawSpeed/ThreefrDecoder.cpp"
        "RawSpeed/TiffEntry.cpp"
        "RawSpeed/TiffEntryBE.cpp"
        "RawSpeed/TiffIFD.cpp"
        "RawSpeed/TiffIFDBE.cpp"
        "RawSpeed/TiffParser.cpp"
        "RawSpeed/TiffParserException.cpp"
        "RawSpeed/X3fDecoder.cpp"
        "RawSpeed/X3fParser.cpp"
)

#
# build librawspeed
#
if(WIN32)
  set(RAWSPEED_LIBS "msvcrt")
endif(WIN32)

add_library(rawspeed STATIC ${RAWSPEED_SOURCES})

set_target_properties(rawspeed
  PROPERTIES
    POSITION_INDEPENDENT_CODE True
)

if(USE_XMLLINT)
  add_dependencies(rawspeed validate_cameras_xml)
endif(USE_XMLLINT)

target_link_libraries(rawspeed ${RAWSPEED_LIBS} ${Pugixml_LIBRARIES} ${JPEG_LIBRARIES})

#
# the rawspeed part is a bit of a hack:
# the static linking didn't work since it was pulling -lstdc++ and -lm into linker flags.
# so we do a custom dependency and pretend an imported librawsped.a so no other -l are
# appended.
#
add_library(rawspeed_static STATIC IMPORTED GLOBAL)
set_target_properties(rawspeed_static
  PROPERTIES
    IMPORTED_LOCATION ${CMAKE_CURRENT_BINARY_DIR}/librawspeed.a
    INTERFACE_LINK_LIBRARIES $<TARGET_PROPERTY:rawspeed,INTERFACE_LINK_LIBRARIES>
    POSITION_INDEPENDENT_CODE True
)

add_dependencies(rawspeed_static rawspeed)

# Adapted from src/cli/CMakeLists.txt
if (BUILD_RS_IDENTIFY)
  add_executable(darktable-rs-identify rawspeed-identify.cpp)

  target_compile_definitions(darktable-rs-identify
    PRIVATE -DRS_CAMERAS_XML_PATH="${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_DATAROOTDIR}/darktable/rawspeed/cameras.xml"
  )

  set_target_properties(darktable-rs-identify
    PROPERTIES
      LINKER_LANGUAGE CXX)

  target_link_libraries(darktable-rs-identify rawspeed_static)

  install(TARGETS darktable-rs-identify DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
