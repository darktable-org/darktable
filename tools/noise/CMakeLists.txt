# Only build on real unix system (exludes MingW, MSYS, etc.)
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
                subr.sh
                profiling-shot.xmp
            DESTINATION
               ${CMAKE_INSTALL_LIBEXECDIR}/darktable/tools
            COMPONENT
                DTApplication)
endif()
