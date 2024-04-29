if (BUILD_CURVE_TOOLS)
  add_subdirectory(basecurve)
endif()
if (BUILD_NOISE_TOOLS)
  add_subdirectory(noise)
endif()

set(TOOLS common.sh extract_wb_from_images.sh purge_from_cache.sh purge_non_existing_images.sh purge_unused_tags.sh)
set(TOOLSWIN purge_nonexistent_images.bat)

if ((NOT WIN32) OR BUILD_MSYS2_INSTALL)

  install(PROGRAMS ${TOOLS} DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/darktable/tools)

else()

  install(PROGRAMS ${TOOLSWIN} DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/darktable/tools)

endif()
