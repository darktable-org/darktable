if(USE_XMLLINT)
	add_custom_target(
		validate_cameras_xml ALL
		COMMAND ${Xmllint_BIN} --valid --noout ${CMAKE_CURRENT_SOURCE_DIR}/data/cameras.xml
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
else(WIN32)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fPIC")
endif(WIN32)

#
# set development flags for rawspeed
#
if(NOT SOURCE_PACKAGE AND NOT APPLE)
  # apple build fails on deprecated warnings..
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Werror -Werror=type-limits")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Werror -Werror=type-limits")
elseif(SOURCE_PACKAGE)
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -D_RELEASE")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -D_RELEASE")
endif()

add_library(rawspeed STATIC ${RAWSPEED_SOURCES})
if(USE_XMLLINT)
  add_dependencies(rawspeed validate_cameras_xml)
endif(USE_XMLLINT)
target_link_libraries(rawspeed ${RAWSPEED_LIBS})

# Adapted from src/cli/CMakeLists.txt
if (BUILD_RS_IDENTIFY)
  add_definitions(-DRS_CAMERAS_XML_PATH="${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_DATAROOTDIR}/darktable/rawspeed/cameras.xml")
  include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../..)
  include_directories(${CMAKE_CURRENT_BINARY_DIR}/../..)

  add_executable(darktable-rs-identify rawspeed-identify.cpp)
  # set_target_properties(darktable-rs-identify PROPERTIES CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
  # set_target_properties(darktable-rs-identify PROPERTIES CMAKE_INSTALL_RPATH_USE_LINK_PATH FALSE)
  # set_target_properties(darktable-rs-identify PROPERTIES INSTALL_RPATH ${RPATH_DT}/../${CMAKE_INSTALL_LIBDIR}/darktable)
  set_target_properties(darktable-rs-identify PROPERTIES LINKER_LANGUAGE CXX)
  if(CMAKE_COMPILER_IS_GNUCC)
	  if (CMAKE_C_COMPILER_VERSION VERSION_GREATER 4.3)
		  if (CMAKE_SYSTEM_NAME MATCHES "^(DragonFly|FreeBSD|NetBSD|OpenBSD)$")
			  message("-- Force link to libintl on *BSD with GCC 4.3+")
			  target_link_libraries(darktable-rs-identify -lintl)
		  endif()
	  endif()
  endif()
  add_dependencies(darktable-rs-identify rawspeed)
  target_link_libraries(darktable-rs-identify rawspeed_static ${Pugixml_LIBRARIES} ${JPEG_LIBRARIES})
  install(TARGETS darktable-rs-identify DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
