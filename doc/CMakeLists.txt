#
# Install Doc files
#
set(DOC_FILES README.md ../AUTHORS TRANSLATORS.md ../LICENSE)
install(FILES ${DOC_FILES} DESTINATION ${CMAKE_INSTALL_DOCDIR} COMPONENT DTDocuments)

# create config docs
if(NOT ${Xsltproc_BIN} STREQUAL "Xsltproc_BIN-NOTFOUND")
  add_custom_command(
    DEPENDS ${CMAKE_SOURCE_DIR}/tools/generate_darktablerc_doc.xsl ${DARKTABLE_DATADIR}/darktableconfig.dtd ${DARKTABLE_DATADIR}/darktableconfig.xml
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/darktablerc.html
    COMMAND ${Xsltproc_BIN} --nonet ${CMAKE_SOURCE_DIR}/tools/generate_darktablerc_doc.xsl ${DARKTABLE_DATADIR}/darktableconfig.xml > ${CMAKE_CURRENT_BINARY_DIR}/darktablerc.html
  )
else(NOT ${Xsltproc_BIN} STREQUAL "Xsltproc_BIN-NOTFOUND")
  if(NOT ${Saxon_BIN} STREQUAL "Saxon_BIN-NOTFOUND")
    add_custom_command(
      DEPENDS ${CMAKE_SOURCE_DIR}/tools/generate_darktablerc_doc.xsl ${DARKTABLE_DATADIR}/darktableconfig.dtd  ${DARKTABLE_DATADIR}/darktableconfig.xml
      OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/darktablerc.html
      COMMAND ${Saxon_BIN} ${DARKTABLE_DATADIR}/darktableconfig.xml ${CMAKE_SOURCE_DIR}/tools/generate_darktablerc_doc.xsl > ${CMAKE_CURRENT_BINARY_DIR}/darktablerc.html
    )
  else(NOT ${Saxon_BIN} STREQUAL "Saxon_BIN-NOTFOUND")
    message(WARNING "darktablerc.html not generated")
  endif(NOT ${Saxon_BIN} STREQUAL "Saxon_BIN-NOTFOUND")
endif(NOT ${Xsltproc_BIN} STREQUAL "Xsltproc_BIN-NOTFOUND")

add_custom_target(darktablerc.html_file ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/darktablerc.html)
if(USE_XMLLINT)
  add_dependencies(darktablerc.html_file validate_darktableconfig_xml)
endif(USE_XMLLINT)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/darktablerc.html DESTINATION ${CMAKE_INSTALL_DOCDIR} COMPONENT DTDocuments)

# Build manual page
add_subdirectory(man)
