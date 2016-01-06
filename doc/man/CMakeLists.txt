cmake_minimum_required(VERSION 2.8.5)

###################################
# ADD NEW MANPAGES HERE
###################################
set(MANPAGES
  "darktable.pod"
  "darktable-cli.pod"
  "darktable-viewer.pod"
  "darktable-generate-cache.pod"
  "darktable-cltest.pod"
  "darktable-cmstest.pod"
)
###################################
# THAT'S IT, NO MORE CHANGES NEEDED
###################################




add_custom_target(manpages ALL)
set(MANPAGE_SOURCES)

macro(add_manpage pod)
  list(APPEND MANPAGE_SOURCES ${pod})
  string(REPLACE ".pod" ".1" manpage ${pod})
  set(source ${CMAKE_CURRENT_SOURCE_DIR}/${pod})
  set(target ${CMAKE_CURRENT_BINARY_DIR}/${manpage})
  add_custom_command(
    OUTPUT ${target}
    COMMAND ${CMAKE_SOURCE_DIR}/tools/makeman.sh ${source} ${CMAKE_CURRENT_SOURCE_DIR}/../AUTHORS ${target}
    DEPENDS ${source} ${CMAKE_CURRENT_SOURCE_DIR}/../AUTHORS ${CMAKE_SOURCE_DIR}/tools/makeman.sh
  )
  add_custom_target(manpage_${manpage} ALL DEPENDS ${target})
  add_dependencies(manpages manpage_${manpage})
  install(FILES ${target} DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)
endmacro (add_manpage)

foreach(manpage ${MANPAGES})
  add_manpage(${manpage})
endforeach(manpage)


# we also want to have translated manpages
find_program(po4a-translate_BIN po4a-translate)

# create and install translated manpages
if(${po4a-translate_BIN} STREQUAL "po4a-translate_BIN-NOTFOUND")
  message("Missing po4a-translate. Can NOT create translated manpages")
else()
  file(STRINGS "po/LINGUAS" LANGUAGES)
  foreach(language ${LANGUAGES})
    set(pofile "po/${language}.po")
    add_custom_target(manpage-${language} ALL)
    add_dependencies(manpages manpage-${language})
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
      add_custom_target(manpage-${language}-${manpage} ALL DEPENDS ${target})
      add_dependencies(manpage-${language} manpage-${language}-${manpage})
      install(FILES ${target} DESTINATION ${CMAKE_INSTALL_MANDIR}/${language}/man1)
    endforeach(pod)
  endforeach(language)
endif()

# we have to split the update targets to a different directory to prevent cmake from calling it all the time :-(
add_subdirectory(po)
