# Only build on real unix system (excludes MingW, MSYS, etc.)
if (UNIX AND NOT WIN32)
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
		noisy-before-colorin.xmp
		smooth-before-colorin.xmp
		noisy-after-colorin.xmp
		smooth-after-colorin.xmp
                subr.sh
            DESTINATION
               ${CMAKE_INSTALL_LIBEXECDIR}/darktable/tools
            COMPONENT
                DTApplication)
endif()
