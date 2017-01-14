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

if (WIN32)
  # Dependency files (files which needs to be installed alongside the darktable binaries)
  # must be in the bin directory
  message( STATUS "WIN32: Adding dependency files to install" )
  get_filename_component(MINGW_PATH ${CMAKE_CXX_COMPILER} PATH )

  set( CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS
  #GTK3
    ${MINGW_PATH}/libgailutil-3-0.dll
    ${MINGW_PATH}/libgdk-3-0.dll
    ${MINGW_PATH}/libgtk-3-0.dll
    ${MINGW_PATH}/libatk-1.0-0.dll
    ${MINGW_PATH}/libcairo-2.dll
    ${MINGW_PATH}/libcairo-gobject-2.dll
    ${MINGW_PATH}/libcairo-script-interpreter-2.dll
    ${MINGW_PATH}/libgdk_pixbuf-2.0-0.dll
    ${MINGW_PATH}/libgio-2.0-0.dll
    ${MINGW_PATH}/libglib-2.0-0.dll
    ${MINGW_PATH}/libgmodule-2.0-0.dll
    ${MINGW_PATH}/libgobject-2.0-0.dll
    ${MINGW_PATH}/libgthread-2.0-0.dll
    ${MINGW_PATH}/libjson-glib-1.0-0.dll
    ${MINGW_PATH}/libepoxy-0.dll
    ${MINGW_PATH}/libpango-1.0-0.dll
    ${MINGW_PATH}/libpangocairo-1.0-0.dll
    ${MINGW_PATH}/libpangoft2-1.0-0.dll
    ${MINGW_PATH}/libpangowin32-1.0-0.dll
    ${MINGW_PATH}/libpixman-1-0.dll
    ${MINGW_PATH}/gdk-pixbuf-query-loaders.exe
    ${MINGW_PATH}/gtk-query-immodules-3.0.exe
    ${MINGW_PATH}/gtk-update-icon-cache.exe
  #LIBXML
    ${MINGW_PATH}/libxml2-2.dll
  #LIBSOUP
    ${MINGW_PATH}/libsoup-2.4-1.dll
  #GPHOTO
    ${MINGW_PATH}/libexif-12.dll
    ${MINGW_PATH}/libgphoto2-6.dll
    ${MINGW_PATH}/libgphoto2_port-12.dll
  #OPENEXR
    ${MINGW_PATH}/libHalf-2_2.dll
    ${MINGW_PATH}/libIex-2_2.dll
    ${MINGW_PATH}/libIexMath-2_2.dll
    ${MINGW_PATH}/libIlmThread-2_2.dll
    ${MINGW_PATH}/libImath-2_2.dll
    ${MINGW_PATH}/libIlmImf-2_2.dll
    ${MINGW_PATH}/libIlmImfUtil-2_2.dll
  #LENSFUN
    ${MINGW_PATH}/libtre-5.dll
    ${MINGW_PATH}/libsystre-0.dll
    ${MINGW_PATH}/liblensfun.dll
  #RSVG2
    ${MINGW_PATH}/librsvg-2-2.dll
  #SQLLITE3
    ${MINGW_PATH}/libsqlite3-0.dll
  #CURL
    ${MINGW_PATH}/libcurl-4.dll
  #C-ARES
    ${MINGW_PATH}/libcares-2.dll
  #LIBIDN
    ${MINGW_PATH}/libidn-11.dll
  #LIBMETALINK
    ${MINGW_PATH}/libmetalink-3.dll
  #LIBSSH2
    ${MINGW_PATH}/libssh2-1.dll
  #RTMPDUMP
  #OPENSSL
    ${MINGW_PATH}/libeay32.dll
    ${MINGW_PATH}/ssleay32.dll
  #NGHTTP2
    ${MINGW_PATH}/libnghttp2-14.dll
  #CA-CERTIFICATES
  #JANSSON
    ${MINGW_PATH}/libjansson-4.dll
  #SPDYLAY
    ${MINGW_PATH}/libspdylay-7.dll
  #PNG
    ${MINGW_PATH}/libpng16-16.dll
  #JPEG
    ${MINGW_PATH}/libjpeg-8.dll
    ${MINGW_PATH}/libturbojpeg-0.dll
  #ZLIB
    ${MINGW_PATH}/zlib1.dll
    ${MINGW_PATH}/libminizip-1.dll
  #XZ
    ${MINGW_PATH}/liblzma-5.dll
  #TIFF
    ${MINGW_PATH}/libtiff-5.dll
    ${MINGW_PATH}/libtiffxx-5.dll
  #LCMS2
    ${MINGW_PATH}/liblcms2-2.dll
  #EXIV2
    ${MINGW_PATH}/libexiv2-14.dll
  #FLICKR
    ${MINGW_PATH}/libflickcurl-1.dll
  #OPENJPEG
    ${MINGW_PATH}/libopenjp2-7.dll
    ${MINGW_PATH}/libopenjp3d-7.dll
    ${MINGW_PATH}/libopenjpip-7.dll
    ${MINGW_PATH}/libopenjpwl-7.dll
    ${MINGW_PATH}/libopenmj2-7.dll
  #LIBSECRET
    ${MINGW_PATH}/libgpg-error-0.dll
    ${MINGW_PATH}/libgcrypt-20.dll
    ${MINGW_PATH}/libsecret-1-0.dll
  #GRAPHICKSMAGICK
    ${MINGW_PATH}/libltdl-7.dll
    ${MINGW_PATH}/libGraphicsMagick++-12.dll
    ${MINGW_PATH}/libGraphicsMagick-3.dll
    ${MINGW_PATH}/libGraphicsMagickWand-2.dll
  #LUA
    ${MINGW_PATH}/lua53.dll
  #PUGIXML
    ${MINGW_PATH}/libpugixml.dll
  #OSMGPSMAP
    ${MINGW_PATH}/libosmgpsmap-1.0-1.dll
  #DRMINGW
    ${MINGW_PATH}/exchndl.dll
    ${MINGW_PATH}/mgwhelp.dll
  #GETTEXT
    ${MINGW_PATH}/libasprintf-0.dll
    ${MINGW_PATH}/libgettextlib-0-19-7.dll
    ${MINGW_PATH}/libgettextpo-0.dll
    ${MINGW_PATH}/libgettextsrc-0-19-7.dll
    ${MINGW_PATH}/libintl-8.dll
  #FONTCONFIG
    ${MINGW_PATH}/libfontconfig-1.dll
    ${MINGW_PATH}/fc-cache.exe
    ${MINGW_PATH}/fc-cat.exe
    ${MINGW_PATH}/fc-list.exe
    ${MINGW_PATH}/fc-match.exe
    ${MINGW_PATH}/fc-pattern.exe
    ${MINGW_PATH}/fc-query.exe
    ${MINGW_PATH}/fc-scan.exe
    ${MINGW_PATH}/fc-validate.exe
  #EXPAT
    ${MINGW_PATH}/libexpat-1.dll
  #FREETYPE
    ${MINGW_PATH}/libfreetype-6.dll
  #HARFBUZZ
    ${MINGW_PATH}/libharfbuzz-0.dll
    ${MINGW_PATH}/libharfbuzz-gobject-0.dll
    ${MINGW_PATH}/libharfbuzz-icu-0.dll
  #GRAPHITE2
    ${MINGW_PATH}/libgraphite2.dll
  #BZIP2
    ${MINGW_PATH}/libbz2-1.dll
  #LIBICONV
    ${MINGW_PATH}/libcharset-1.dll
    ${MINGW_PATH}/libiconv-2.dll
  #WINEDITLINE
    ${MINGW_PATH}/edit.dll
  #PCRE
    ${MINGW_PATH}/libpcre-1.dll
    ${MINGW_PATH}/libpcre16-0.dll
    ${MINGW_PATH}/libpcre32-0.dll
    ${MINGW_PATH}/libpcrecpp-0.dll
    ${MINGW_PATH}/libpcreposix-0.dll
  #LIBFFI
    ${MINGW_PATH}/libffi-6.dll
  #LIBCROCO
    ${MINGW_PATH}/libcroco-0.6-3.dll
  #GCCRUNTIME
    ${MINGW_PATH}/libgomp-1.dll
    ${MINGW_PATH}/libgcc_s_seh-1.dll
    ${MINGW_PATH}/libwinpthread-1.dll
    ${MINGW_PATH}/libstdc++-6.dll
    )

  install(PROGRAMS ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS} DESTINATION bin COMPONENT DTDependencies)
  # TODO: Add auxilliary files for GraphicsMagick, libgphoto2, gdk-pixbuf, adwaita-icon-theme, openssl

endif(WIN32)

endfunction()