#
# Install Doc files
#
set(DOC_FILES README.md ../AUTHORS TRANSLATORS.md ../LICENSE)
install(FILES ${DOC_FILES} DESTINATION ${CMAKE_INSTALL_DOCDIR} COMPONENT DTDocuments)

# Build manual page
add_subdirectory(man)
