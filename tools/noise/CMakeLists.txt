# Only build on real *nix system or MSYS2 installation
if ((NOT WIN32) OR BUILD_MSYS2_INSTALL)
    add_executable(darktable-noiseprofile noiseprofile.c)
    target_link_libraries(darktable-noiseprofile m)

    install(TARGETS
                darktable-noiseprofile
            DESTINATION
                ${CMAKE_INSTALL_LIBEXECDIR}/darktable/tools
            COMPONENT
                DTApplication)

    install(PROGRAMS
                darktable-gen-noiseprofile
            DESTINATION
                ${CMAKE_INSTALL_LIBEXECDIR}/darktable/tools
            COMPONENT
                DTApplication)

    install(FILES
                subr.sh
                profiling-shot.xmp
            DESTINATION
               ${CMAKE_INSTALL_LIBEXECDIR}/darktable/tools
            COMPONENT
                DTApplication)
endif()
