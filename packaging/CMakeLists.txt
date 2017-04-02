
IF(WIN32)
  SET(plugin_dest_dir lib/darktable/plugins)

  set(APPS "\${CMAKE_INSTALL_PREFIX}/bin/darktable.exe")  # paths to executables

  get_filename_component(MINGW_PATH ${CMAKE_CXX_COMPILER} PATH )
  set(DIRS ${MINGW_PATH}) # directories to search for prerequisites

  # run Bundle utilities
  INSTALL(CODE "
    file(GLOB_RECURSE DTPLUGINS
      \"\${CMAKE_INSTALL_PREFIX}/${plugin_dest_dir}/*${CMAKE_SHARED_LIBRARY_SUFFIX}\")
    list(APPEND DTPLUGINS \"\${CMAKE_INSTALL_PREFIX}/bin/libdarktable.dll\")
    include(\"${CMAKE_ROOT}/Modules/BundleUtilities.cmake\")
    set(gp_tool \"objdump\")
    fixup_bundle(\"${APPS}\" \"\${DTPLUGINS}\" \"${DIRS}\")
    " COMPONENT DTApplication)
ENDIF(WIN32)
