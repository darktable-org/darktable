cmake_minimum_required(VERSION 2.8.5)

find_program(po4a-updatepo_BIN po4a-updatepo)

# target to update .po files
if(${po4a-updatepo_BIN} STREQUAL "po4a-updatepo_BIN-NOTFOUND")
  message("Missing po4a-updatepo. Can NOT update manpage translations")
else()
  add_custom_target(update-manpages)

  set(files)
  set(deps)
  foreach(source ${MANPAGE_SOURCES})
    list(APPEND files "-m")
    list(APPEND files ${source})
    list(APPEND deps ../${source})
  endforeach(source)

  file(STRINGS "LINGUAS" LANGUAGES)
  foreach(language ${LANGUAGES})
    set(pofile "${language}.po")
    add_custom_command(
      OUTPUT ${pofile}
      COMMAND ${po4a-updatepo_BIN} -f pod ${files} -p po/${pofile}
      DEPENDS ${deps}
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../
    )
    add_custom_target(update-manpage-${language} DEPENDS ${pofile})
    add_dependencies(update-manpages update-manpage-${language})
  endforeach(language)
endif()
