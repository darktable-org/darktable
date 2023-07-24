#
# Install Doc files
#
set(DOC_FILES README.md ../AUTHORS TRANSLATORS.md ../LICENSE)
install(FILES ${DOC_FILES} DESTINATION ${CMAKE_INSTALL_DOCDIR} COMPONENT DTDocuments)

# Build manual page
if ((NOT WIN32) OR BUILD_MSYS2_INSTALL)
  add_subdirectory(man)
endif()
