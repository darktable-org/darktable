cmake_minimum_required(VERSION 2.6)

###################################
# ADD NEW MANPAGES HERE
###################################
set(MANPAGES
  "darktable.pod"
  "darktable-cli.pod"
)
###################################
# THAT'S IT, NO MORE CHANGES NEEDED
###################################




if(NOT MAN_INSTALL_DIR)
  if(CMAKE_SYSTEM_NAME MATCHES "^(DragonFly|FreeBSD|NetBSD|OpenBSD)$")
    set(MAN_INSTALL_DIR "./man/man1")
  else()
    set(MAN_INSTALL_DIR "./share/man/man1")
  endif()
endif(NOT MAN_INSTALL_DIR)

add_custom_target(manpages ALL)

macro(add_manpage pod)
  list(APPEND MANPAGE_SOURCES ${pod})
  string(REPLACE ".pod" ".1" manpage ${pod})
  set(source ${CMAKE_CURRENT_SOURCE_DIR}/${pod})
  add_custom_command(
    OUTPUT ${manpage}
    COMMAND ${CMAKE_SOURCE_DIR}/tools/makeman.sh ${source} ${CMAKE_CURRENT_SOURCE_DIR}/../AUTHORS ${CMAKE_CURRENT_BINARY_DIR}/${manpage}
    DEPENDS ${source} ${CMAKE_CURRENT_SOURCE_DIR}/../AUTHORS ${CMAKE_SOURCE_DIR}/tools/makeman.sh
  )
  add_custom_target(man_${manpage} ALL DEPENDS ${manpage})
  add_dependencies(manpages man_${manpage})
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${manpage} DESTINATION ${MAN_INSTALL_DIR})
endmacro (add_manpage)

foreach(manpage ${MANPAGES})
  add_manpage(${manpage})
endforeach(manpage)


# we also want to have translated manpages
find_program(po4a-updatepo_BIN po4a-updatepo)
find_program(po4a-translate_BIN po4a-translate)

# target to update .po files
if(${po4a-updatepo_BIN} STREQUAL "po4a-updatepo_BIN-NOTFOUND")
  message("Missing po4a-updatepo. Can NOT update manpage translations")
else()
  add_custom_target(update-manpages)

  foreach(source ${MANPAGE_SOURCES})
    list(APPEND files "-m")
    list(APPEND files ${source})
  endforeach(source)

  file(STRINGS "po/LINGUAS" LANGUAGES)
  foreach(language ${LANGUAGES})
    set(pofile "po/${language}.po")

    add_custom_command(
      OUTPUT ${pofile}
      COMMAND ${po4a-updatepo_BIN} -f pod ${files} -p ${pofile}
      DEPENDS ${MANPAGE_SOURCES}
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    add_custom_target(update-manpage-${language} DEPENDS ${MANPAGE_SOURCES} ${pofile})
    add_dependencies(update-manpages update-manpage-${language})
  endforeach(language)
endif()

# create and install translated manpages
if(${po4a-translate_BIN} STREQUAL "po4a-translate_BIN-NOTFOUND")
  message("Missing po4a-translate. Can NOT create translated manpages")
else()
  file(STRINGS "po/LINGUAS" LANGUAGES)
  foreach(language ${LANGUAGES})
    set(pofile "po/${language}.po")
    add_custom_target(man_${language} ALL)
    add_dependencies(manpages man_${language})
    foreach(pod ${MANPAGE_SOURCES})
      string(REPLACE ".pod" ".1" manpage ${pod})
      set(target "${CMAKE_CURRENT_BINARY_DIR}/${language}/${manpage}")
      set(tmppod "${CMAKE_CURRENT_BINARY_DIR}/${language}/${pod}")
      # create translated .pod
      add_custom_command(
        OUTPUT ${tmppod}
        COMMAND ${po4a-translate_BIN} -f pod -m ${pod} -p ${pofile} -l ${tmppod} -k 0
        DEPENDS ${pofile} ${pod}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
      )
      # turn the .pod into a manpage
      add_custom_command(
        OUTPUT ${target}
        COMMAND ${CMAKE_SOURCE_DIR}/tools/makeman.sh ${tmppod} ${CMAKE_CURRENT_SOURCE_DIR}/../AUTHORS ${target}
        DEPENDS ${tmppod} ${CMAKE_CURRENT_SOURCE_DIR}/../AUTHORS ${CMAKE_SOURCE_DIR}/tools/makeman.sh
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
      )
      add_custom_target(man_${language}_${manpage} ALL DEPENDS ${target})
#       add_dependencies(manpages man_${language}_${manpage})
      add_dependencies(man_${language} man_${language}_${manpage})
      install(FILES ${target} DESTINATION ${MAN_INSTALL_DIR}/../${language}/man1/)
    endforeach(pod)
  endforeach(language)
endif()

