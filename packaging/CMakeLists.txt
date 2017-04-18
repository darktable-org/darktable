
IF(WIN32)
  SET(plugin_dest_dir lib/darktable/plugins)

  set(APPS "\${CMAKE_INSTALL_PREFIX}/bin/darktable.exe")  # paths to executables

  get_filename_component(MINGW_PATH ${CMAKE_CXX_COMPILER} PATH )
  set(DIRS "${MINGW_PATH}/../../") # directories to search for prerequisites

  INSTALL(CODE "
    set(EXTRA_DEPS \"${CMAKE_CURRENT_BINARY_DIR}/deps_generated.cmake\")

    function(process_file filename)
      execute_process(COMMAND sh \"${CMAKE_CURRENT_SOURCE_DIR}/windows/get_deps.sh\" \"\${filename}\" \"\${EXTRA_DEPS}\" ${DIRS})
    endfunction()

    file(GLOB_RECURSE DTPLUGINS \"\${CMAKE_INSTALL_PREFIX}/${plugin_dest_dir}/*${CMAKE_SHARED_LIBRARY_SUFFIX}\")
    list(APPEND DTPLUGINS \"\${CMAKE_INSTALL_PREFIX}/bin/libdarktable.dll\")

    file(WRITE \"\${EXTRA_DEPS}\" \"file(INSTALL \")

    process_file(\"${APPS}\")
    foreach(filename IN LISTS DTPLUGINS)
      process_file(\"\${filename}\")
    endforeach()

    file(APPEND \"\${EXTRA_DEPS}\" \" DESTINATION \\\"\${CMAKE_INSTALL_PREFIX}/bin\\\")\")

    include(\"\${EXTRA_DEPS}\")
    " COMPONENT DTApplication)
ENDIF(WIN32)
