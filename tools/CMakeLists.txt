if (BUILD_CURVE_TOOLS)
    add_subdirectory(basecurve)
endif()
if (BUILD_NOISE_TOOLS)
    add_subdirectory(noise)
endif()

install(
  FILES
      common.sh  purge_from_cache.sh  purge_non_existing_images.sh  purge_unused_tags.sh
  DESTINATION
      ${CMAKE_INSTALL_DATAROOTDIR}/darktable/tools
  PERMISSIONS
      OWNER_READ OWNER_EXECUTE OWNER_WRITE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE
)
