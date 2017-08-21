file(GLOB LANGUAGES *.po)
add_custom_target(locales ALL)
foreach(language ${LANGUAGES})
    string(REGEX REPLACE "(.+(\\\\|/))+" "" language ${language})
    string(REGEX REPLACE "\\.po$" "" language ${language})
    set(pofile ${CMAKE_CURRENT_SOURCE_DIR}/${language}.po)
    set(mofile ${CMAKE_CURRENT_BINARY_DIR}/${language}.mo)
    add_custom_command(OUTPUT ${mofile}
                       COMMAND ${GETTEXT_MSGFMT_EXECUTABLE} -v -c "${pofile}" -o ${mofile}
                       MAIN_DEPENDENCY ${pofile}
                       COMMENT "Building ${language} locale"
                       VERBATIM)
    add_custom_target(locale_${language} ALL DEPENDS ${mofile})  # Make sure the mofiles are always built
    add_dependencies(locales locale_${language})
    install(FILES ${mofile}
            DESTINATION ${CMAKE_INSTALL_LOCALEDIR}/${language}/LC_MESSAGES
            RENAME ${CMAKE_PROJECT_NAME}.mo
            COMPONENT DTApplication)
endforeach(language)
