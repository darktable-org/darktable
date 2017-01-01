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
  #PNG
    ${MINGW_PATH}/libpng16-16.dll
  #JPEG
    ${MINGW_PATH}/libjpeg-8.dll
    ${MINGW_PATH}/libturbojpeg-0.dll
  #TIFF
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
    ${MINGW_PATH}/lua52.dll
  #PUGIXML
    ${MINGW_PATH}/libpugixml.dll
  #OSMGPSMAP
    ${MINGW_PATH}/libosmgpsmap-1.0-1.dll
  #DRMINGW
    ${MINGW_PATH}/exchndl.dll
    ${MINGW_PATH}/mgwhelp.dll
  #GCCRUNTIME
    ${MINGW_PATH}/libgomp-1.dll
    )

  include(InstallRequiredSystemLibraries)

  install(PROGRAMS ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS} DESTINATION bin COMPONENT DTDependencies)
  # TODO: Add auxilliary files for GraphicsMagick, libgphoto2, gdk-pixbuf, adwaita-icon-theme

endif(WIN32)

endfunction()