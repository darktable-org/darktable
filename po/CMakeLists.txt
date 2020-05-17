file(GLOB LANGUAGES *.po)
add_custom_target(locales ALL)
foreach(language ${LANGUAGES})
    string(REGEX REPLACE "(.+(\\\\|/))+" "" language ${language})
    string(REGEX REPLACE "\\.po$" "" language ${language})
    set(pofile ${CMAKE_CURRENT_SOURCE_DIR}/${language}.po)
    set(mofile ${DARKTABLE_LOCALEDIR}/${language}/LC_MESSAGES/${CMAKE_PROJECT_NAME}.mo)
    file(MAKE_DIRECTORY ${DARKTABLE_LOCALEDIR}/${language}/LC_MESSAGES)
    add_custom_command(OUTPUT ${mofile}
                       COMMAND ${GETTEXT_MSGFMT_EXECUTABLE} -v -c "${pofile}" -o ${mofile}
                       MAIN_DEPENDENCY ${pofile}
                       COMMENT "Building ${language} locale"
                       VERBATIM)
    add_custom_target(locale_${language} ALL DEPENDS ${mofile})  # Make sure the mofiles are always built
    add_dependencies(locales locale_${language})
    install(FILES ${mofile}
            DESTINATION ${CMAKE_INSTALL_LOCALEDIR}/${language}/LC_MESSAGES
            COMPONENT DTApplication)
endforeach(language)
