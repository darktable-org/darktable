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


#-------------------------------------------------------------------------------
# _copy_required_library(<target> <library>)
#
# Helper function to copy required library (specified by target) alongside the
# target binary.
#
# This is required as Win doesn't have a RPATH
#-------------------------------------------------------------------------------
function(_copy_required_library target library)
  message( STATUS "WIN32: Adding post-build step to copy required lib alongside target binary")
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${library}> $<TARGET_FILE_DIR:${target}>
  )
endfunction()


#-------------------------------------------------------------------------------
# _install_translations(<catalog> <src_localedir>)
#
# Helper macro to install all available translations for a given catalog.
#-------------------------------------------------------------------------------
macro(_install_translations catalog src_localedir)
  file(GLOB MO_FILES RELATIVE "${src_localedir}" "${src_localedir}/*/LC_MESSAGES/${catalog}.mo")
  foreach(MO ${MO_FILES})
    get_filename_component(MO_TARGET_DIR "${MO}" DIRECTORY)
    install(FILES "${src_localedir}/${MO}" DESTINATION "share/locale/${MO_TARGET_DIR}" COMPONENT DTApplication)
  endforeach()
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
    ${MINGW_PATH}/gdbus.exe
  #LZO2
    ${MINGW_PATH}/liblzo*.dll
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
  #GNUTLS
    ${MINGW_PATH}/libgnutlsxx*.dll
  #GMP
    ${MINGW_PATH}/libgmpxx*.dll
  #LIBUSB1
    ${MINGW_PATH}/libusb*.dll
  #OPENSSL
    ${MINGW_PATH}/libcrypto*.dll
    ${MINGW_PATH}/libssl*.dll
    )

  if(OpenEXR_FOUND)
    file(GLOB TMP_SYSTEM_RUNTIME_LIBS
      #OPENEXR
      ${MINGW_PATH}/libIexMath*.dll
      ${MINGW_PATH}/libIlmImfUtil*.dll
    )
    list(APPEND CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS ${TMP_SYSTEM_RUNTIME_LIBS})
  endif()

  if(OpenJPEG_FOUND)
    file(GLOB TMP_SYSTEM_RUNTIME_LIBS
      #OPENJPEG
      ${MINGW_PATH}/libopenjp3d*.dll
      ${MINGW_PATH}/libopenjpip*.dll
      ${MINGW_PATH}/libopenjpwl*.dll
      ${MINGW_PATH}/libopenmj2*.dll
    )
    list(APPEND CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS ${TMP_SYSTEM_RUNTIME_LIBS})
  endif()

  if(GraphicsMagick_FOUND)
    file(GLOB TMP_SYSTEM_RUNTIME_LIBS
      #GRAPHICKSMAGICK
      ${MINGW_PATH}/libltdl*.dll
      ${MINGW_PATH}/libGraphicsMagick++*.dll
      ${MINGW_PATH}/libGraphicsMagickWand*.dll
    )
    list(APPEND CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS ${TMP_SYSTEM_RUNTIME_LIBS})
  endif()

  # workaround for msys2 gmic 2.9.0-3. Should be reviewed when gmic 2.9.3 is available
  if(GMIC_FOUND)
    file(GLOB TMP_SYSTEM_RUNTIME_LIBS
      #GMIC
      ${MINGW_PATH}/libopencv_core*.dll
      ${MINGW_PATH}/libopencv_videoio*.dll
    )
    list(APPEND CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS ${TMP_SYSTEM_RUNTIME_LIBS})
  endif()
  
  if(JXL_FOUND)
    file(GLOB TMP_SYSTEM_RUNTIME_LIBS
      #LIBJXL
      ${MINGW_PATH}/libjxl.dll
      ${MINGW_PATH}/libjxl_threads.dll
    )
    list(APPEND CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS ${TMP_SYSTEM_RUNTIME_LIBS})
  endif()

  if(WebP_FOUND)
    file(GLOB TMP_SYSTEM_RUNTIME_LIBS
      #LIBWEBP
      ${MINGW_PATH}/libwebpdecoder*.dll
      ${MINGW_PATH}/libwebpdemux*.dll
      #${MINGW_PATH}/libwebpextras*.dll
      ${MINGW_PATH}/libwebpmux*.dll
    )
    list(APPEND CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS ${TMP_SYSTEM_RUNTIME_LIBS})
  endif()

  # Add GLib and GTK translations
  _install_translations(glib20 ${MINGW_PATH}/../share/locale)
  _install_translations(gtk30 ${MINGW_PATH}/../share/locale)

  # TODO: Add auxiliary files for openssl?

  # Add pixbuf loader libraries
  # FILE(GLOB_RECURSE GDK_PIXBUF "${MINGW_PATH}/../lib/gdk-pixbuf-2.0/2.10.0/loaders/*.dll"  )
  install(DIRECTORY
      "${MINGW_PATH}/../lib/gdk-pixbuf-2.0"
      DESTINATION lib/
      COMPONENT DTApplication
      PATTERN "*.a" EXCLUDE)

  # Add glib-networking modules
  install(DIRECTORY
      "${MINGW_PATH}/../lib/gio/modules/"
      DESTINATION lib/gio/modules/
      COMPONENT DTApplication)

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

  # Add libgphoto2 files and dependencies
  if(Gphoto2_FOUND)
    file(GLOB TMP_SYSTEM_RUNTIME_LIBS
      ${MINGW_PATH}/imagequant.dll
      ${MINGW_PATH}/libexif*.dll
      ${MINGW_PATH}/libgd.dll
      ${MINGW_PATH}/libusb*.dll
      ${MINGW_PATH}/libXpm-noX*.dll
    )
    list(APPEND CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS ${TMP_SYSTEM_RUNTIME_LIBS})

    install(DIRECTORY
        "${MINGW_PATH}/../lib/libgphoto2"
        DESTINATION lib/
        COMPONENT DTApplication
        PATTERN "*.a" EXCLUDE)
    _install_translations(libgphoto2-6 ${MINGW_PATH}/../share/locale)

    install(DIRECTORY
        "${MINGW_PATH}/../lib/libgphoto2_port"
        DESTINATION lib/
        COMPONENT DTApplication
        PATTERN "*.a" EXCLUDE
        PATTERN "usb.dll" EXCLUDE)
  endif()

  # Add GraphicsMagick libraries
  if(GraphicsMagick_FOUND)
    install(DIRECTORY
        "${MINGW_PATH}/../lib/GraphicsMagick-${GraphicsMagick_PKGCONF_VERSION}/modules-Q16/coders"
        DESTINATION lib/GraphicsMagick-${GraphicsMagick_PKGCONF_VERSION}/modules-Q16/
        COMPONENT DTApplication
        FILES_MATCHING PATTERN "*"
        PATTERN "*.a" EXCLUDE
        PATTERN "*.la" EXCLUDE)
  endif()

  # Add lensfun libraries
  if(LensFun_FOUND)
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
  endif(LensFun_FOUND)

  # Add iso-codes
  if(IsoCodes_FOUND)
    install(FILES
        "${IsoCodes_LOCATION}/iso_639-2.json"
        DESTINATION share/iso-codes/json/
        COMPONENT DTApplication
    )
    _install_translations(iso_639-2 ${IsoCodes_LOCALEDIR})
  endif(IsoCodes_FOUND)

  # Add ca-cert for curl
  install(FILES
      "${MINGW_PATH}/../etc/ssl/certs/ca-bundle.crt"
      DESTINATION share/curl/
      RENAME curl-ca-bundle.crt
      COMPONENT DTApplication)

  # Add libavif files
  if(libavif_FOUND)
    file(GLOB TMP_SYSTEM_RUNTIME_LIBS
      #LIBAVIF
      ${MINGW_PATH}/libavif*.dll
    )
    list(APPEND CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS ${TMP_SYSTEM_RUNTIME_LIBS})
  endif(libavif_FOUND)

  # Add rsvg2 files
  if(Rsvg2_FOUND)
    file(GLOB TMP_SYSTEM_RUNTIME_LIBS
      #RSVG2
      ${MINGW_PATH}/librsvg*.dll
    )
    list(APPEND CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS ${TMP_SYSTEM_RUNTIME_LIBS})
  endif(Rsvg2_FOUND)

  list(REMOVE_DUPLICATES CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)

  install(PROGRAMS ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS} DESTINATION bin COMPONENT DTApplication)

endif(WIN32 AND NOT BUILD_MSYS2_INSTALL)

endfunction()
