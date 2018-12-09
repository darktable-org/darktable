#-------------------------------------------------------------------------------
# _detach_debuginfo(<target> <dest>)
#
# Helper macro to detach the debug information from the target.
#
# The debug info of the given target are extracted and saved on a different file
# with the extension '.dbg', that is installed alongside the target in the .debug folder.
#-------------------------------------------------------------------------------
macro(_detach_debuginfo target dest)
    set (CMAKE_OBJCOPY, objcopy)

    add_custom_command (TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E make_directory .debug
      COMMAND objcopy --only-keep-debug $<TARGET_FILE:${target}> $<TARGET_FILE_DIR:${target}>/$<TARGET_FILE_NAME:${target}>.dbg
      COMMAND objcopy --strip-debug $<TARGET_FILE:${target}>
      COMMAND objcopy --add-gnu-debuglink=$<TARGET_FILE_NAME:${target}>.dbg $<TARGET_FILE:${target}>
      COMMENT "Detaching debug infos for ${target}."
    )

    # ensure that the debug file is installed on 'make install'...
    install(
      FILES $<TARGET_FILE_DIR:${target}>/$<TARGET_FILE_NAME:${target}>.dbg
      DESTINATION ${dest}/.debug
      COMPONENT DTDebugSymbols)

    # ... and removed on 'make clean'.
    set_property(DIRECTORY APPEND PROPERTY ADDITIONAL_MAKE_CLEAN_FILES $<TARGET_FILE_NAME:${target}>.dbg)
endmacro()

function(InstallDependencyFiles)

if (WIN32 AND NOT BUILD_MSYS2_INSTALL)
  # Dependency files (files which needs to be installed alongside the darktable binaries)
  # Please note these are ONLY the files which are not geing detected by fixup_bundle()
  # must be in the bin directory
  message( STATUS "WIN32: Adding dependency files to install" )
  get_filename_component(MINGW_PATH ${CMAKE_CXX_COMPILER} PATH )

  set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
  file(GLOB CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS
  #GTK3
    ${MINGW_PATH}/libgailutil*.dll
    ${MINGW_PATH}/libcairo-script-interpreter*.dll
    ${MINGW_PATH}/libgthread*.dll
    ${MINGW_PATH}/gdk-pixbuf-query-loaders.exe
    ${MINGW_PATH}/gtk-query-immodules*.exe
    ${MINGW_PATH}/gtk-update-icon-cache.exe
    ${MINGW_PATH}/gspawn-win64-helper.exe
    ${MINGW_PATH}/gspawn-win64-helper-console.exe
  #LZO2
    ${MINGW_PATH}/liblzo*.dll
  #OPENEXR
    ${MINGW_PATH}/libIexMath*.dll
    ${MINGW_PATH}/libIlmImfUtil*.dll
  #C-ARES
    ${MINGW_PATH}/libcares*.dll
  #LIBMETALINK
    ${MINGW_PATH}/libmetalink*.dll
  #JANSSON
    ${MINGW_PATH}/libjansson*.dll
  #SPDYLAY
    ${MINGW_PATH}/libspdylay*.dll
  #JPEG
    ${MINGW_PATH}/libturbojpeg*.dll
  #ZLIB
    ${MINGW_PATH}/libminizip*.dll
  #TIFF
    ${MINGW_PATH}/libtiffxx*.dll
  #OPENJPEG
    ${MINGW_PATH}/libopenjp3d*.dll
    ${MINGW_PATH}/libopenjpip*.dll
    ${MINGW_PATH}/libopenjpwl*.dll
    ${MINGW_PATH}/libopenmj2*.dll
  #GRAPHICKSMAGICK
    ${MINGW_PATH}/libltdl*.dll
    ${MINGW_PATH}/libGraphicsMagick++*.dll
    ${MINGW_PATH}/libGraphicsMagickWand*.dll
  #GETTEXT
    ${MINGW_PATH}/libasprintf*.dll
    ${MINGW_PATH}/libgettextlib*.dll
    ${MINGW_PATH}/libgettextpo*.dll
    ${MINGW_PATH}/libgettextsrc*.dll
  #FONTCONFIG
    ${MINGW_PATH}/fc-cache.exe
    ${MINGW_PATH}/fc-cat.exe
    ${MINGW_PATH}/fc-list.exe
    ${MINGW_PATH}/fc-match.exe
    ${MINGW_PATH}/fc-pattern.exe
    ${MINGW_PATH}/fc-query.exe
    ${MINGW_PATH}/fc-scan.exe
    ${MINGW_PATH}/fc-validate.exe
  #HARFBUZZ
    ${MINGW_PATH}/libharfbuzz-gobject*.dll
    ${MINGW_PATH}/libharfbuzz-icu*.dll
  #LIBICONV
    ${MINGW_PATH}/libcharset*.dll
  #WINEDITLINE
    ${MINGW_PATH}/edit.dll
  #PCRE
    ${MINGW_PATH}/libpcre16*.dll
    ${MINGW_PATH}/libpcre32*.dll
    ${MINGW_PATH}/libpcrecpp*.dll
    ${MINGW_PATH}/libpcreposix*.dll
  #LIBWEBP
    ${MINGW_PATH}/libwebpdecoder*.dll
    ${MINGW_PATH}/libwebpdemux*.dll
    #${MINGW_PATH}/libwebpextras*.dll
    ${MINGW_PATH}/libwebpmux*.dll
  #GNUTLS
    ${MINGW_PATH}/libgnutlsxx*.dll
  #GMP
    ${MINGW_PATH}/libgmpxx*.dll
  #LIBUSB1
    ${MINGW_PATH}/libusb*.dll
    )

  install(PROGRAMS ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS} DESTINATION bin COMPONENT DTApplication)

  # TODO: Add auxiliary files for openssl?

  # Add pixbuf loader libraries
  # FILE(GLOB_RECURSE GDK_PIXBUF "${MINGW_PATH}/../lib/gdk-pixbuf-2.0/2.10.0/loaders/*.dll"  )
  install(DIRECTORY
      "${MINGW_PATH}/../lib/gdk-pixbuf-2.0"
      DESTINATION lib/
      COMPONENT DTApplication
      PATTERN "*.a" EXCLUDE)

  # Add adwaita-icon-theme files
  install(DIRECTORY
      "${MINGW_PATH}/../share/icons/adwaita/"
      DESTINATION share/icons/adwaita/
      COMPONENT DTApplication)

  # fixup hicolor theme
  install(FILES
      "${MINGW_PATH}/../share/icons/hicolor/index.theme"
      DESTINATION share/icons/hicolor/
      COMPONENT DTApplication)

  # Add gtk schemas files
  install(DIRECTORY
      "${MINGW_PATH}/../share/glib-2.0/schemas/"
      DESTINATION share/glib-2.0/schemas/
      COMPONENT DTApplication)

  # Add libthai files
  install(DIRECTORY
      "${MINGW_PATH}/../share/libthai/"
      DESTINATION share/libthai/
      COMPONENT DTApplication)

  # Add libgphoto2 files
  install(DIRECTORY
      "${MINGW_PATH}/../lib/libgphoto2"
      DESTINATION lib/
      COMPONENT DTApplication
      PATTERN "*.a" EXCLUDE)

  install(DIRECTORY
      "${MINGW_PATH}/../lib/libgphoto2_port"
      DESTINATION lib/
      COMPONENT DTApplication
      PATTERN "*.a" EXCLUDE
      PATTERN "usb.dll" EXCLUDE)

  # Add GraphicsMagick libraries
  install(DIRECTORY
      "${MINGW_PATH}/../lib/GraphicsMagick-${GraphicsMagick_PKGCONF_VERSION}/modules-Q8/coders"
      DESTINATION lib/GraphicsMagick-${GraphicsMagick_PKGCONF_VERSION}/modules-Q8/
      COMPONENT DTApplication
      FILES_MATCHING PATTERN "*"
      PATTERN "*.a" EXCLUDE
      PATTERN "*.la" EXCLUDE)

  # Add lensfun libraries
  set(LENSFUN_DB_GLOBAL "${MINGW_PATH}/../share/lensfun/version_1")
  set(LENSFUN_DB_UPDATES "${MINGW_PATH}/../var/lib/lensfun-updates/version_1")
  set(LENSFUN_DB "${LENSFUN_DB_GLOBAL}")
  if(EXISTS "${LENSFUN_DB_UPDATES}")
    file(READ "${LENSFUN_DB_GLOBAL}/timestamp.txt" LENSFUN_TS)
    file(READ "${LENSFUN_DB_UPDATES}/timestamp.txt" LENSFUN_TS_UPDATE)
    if(LENSFUN_TS LESS LENSFUN_TS_UPDATE)
      set(LENSFUN_DB "${LENSFUN_DB_UPDATES}")
    endif()
  endif()
  message(STATUS "Installing lensfun database from ${LENSFUN_DB}")
  install(DIRECTORY
      "${LENSFUN_DB}"
      DESTINATION share/lensfun/
      COMPONENT DTApplication)

  # Add iso-codes
  if(ISO_CODES_FOUND)
    install(FILES
        "${ISO_CODES_LOCATION}/iso_639-2.json"
        DESTINATION share/iso-codes/json/
        COMPONENT DTApplication
    )
    file(GLOB ISO_CODES_MO_FILES RELATIVE "${ISO_CODES_LOCALEDIR}" "${ISO_CODES_LOCALEDIR}/*/LC_MESSAGES/iso_639.mo")
    foreach(MO ${ISO_CODES_MO_FILES})
      string(REPLACE "iso_639.mo" "" MO_TARGET_DIR "${MO}")
      install(FILES "${ISO_CODES_LOCALEDIR}/${MO}" DESTINATION "share/locale/${MO_TARGET_DIR}" COMPONENT DTApplication)
    endforeach()
  endif(ISO_CODES_FOUND)

endif(WIN32 AND NOT BUILD_MSYS2_INSTALL)

endfunction()
