cmake_minimum_required(VERSION 2.8.5)

#
# Install Doc files
#
set(DOC_FILES README AUTHORS TRANSLATORS LICENSE)
install(FILES ${DOC_FILES} DESTINATION ${CMAKE_INSTALL_DOCDIR})

# create config docs
if(NOT ${Xsltproc_BIN} STREQUAL "Xsltproc_BIN-NOTFOUND")
  add_custom_command(
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../tools/generate_darktablerc_doc.xsl ${CMAKE_CURRENT_BINARY_DIR}/../data/darktableconfig.dtd ${CMAKE_CURRENT_BINARY_DIR}/../data/darktableconfig.xml
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/darktablerc.html
    COMMAND ${Xsltproc_BIN} ${CMAKE_CURRENT_SOURCE_DIR}/../tools/generate_darktablerc_doc.xsl ${CMAKE_CURRENT_BINARY_DIR}/../data/darktableconfig.xml > ${CMAKE_CURRENT_BINARY_DIR}/darktablerc.html
  )
else(NOT ${Xsltproc_BIN} STREQUAL "Xsltproc_BIN-NOTFOUND")
  if(NOT ${Saxon_BIN} STREQUAL "Saxon_BIN-NOTFOUND")
    add_custom_command(
      DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../tools/generate_darktablerc_doc.xsl ${CMAKE_CURRENT_BINARY_DIR}/../data/darktableconfig.dtd ${CMAKE_CURRENT_BINARY_DIR}/../data/darktableconfig.xml
      OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/darktablerc.html
      COMMAND ${Saxon_BIN} ${CMAKE_CURRENT_BINARY_DIR}/../data/darktableconfig.xml ${CMAKE_CURRENT_SOURCE_DIR}/../tools/generate_darktablerc_doc.xsl > ${CMAKE_CURRENT_BINARY_DIR}/darktablerc.html
    )
  else(NOT ${Saxon_BIN} STREQUAL "Saxon_BIN-NOTFOUND")
    message(WARNING "darktablerc.html not generated")
  endif(NOT ${Saxon_BIN} STREQUAL "Saxon_BIN-NOTFOUND")
endif(NOT ${Xsltproc_BIN} STREQUAL "Xsltproc_BIN-NOTFOUND")

add_custom_target(darktablerc.html_file ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/darktablerc.html)
if(USE_XMLLINT)
  add_dependencies(darktablerc.html_file validate_darktableconfig_xml)
endif(USE_XMLLINT)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/darktablerc.html DESTINATION ${CMAKE_INSTALL_DOCDIR})

# we remove the usermanual from the release tarball as it adds an extra 77 MB which people don't need in general
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/usermanual)
  add_subdirectory(usermanual)
else(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/usermanual)
  message(WARNING "the sources of the usermanual are missing so you won't be able to generate it. if this is a release tarball then that's normal.\nplease grab them from github if you want to work on the manual.")
endif(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/usermanual)

# Build manual page
add_subdirectory(man)
