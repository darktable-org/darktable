#
# Install Doc files
#
# For the end user, in fact, the files in this directory are of little or no relevance.
# So we've limited the installation files to just the authors and license information.
#
set(DOC_FILES ../AUTHORS ../LICENSE)

if ((NOT WIN32) OR BUILD_MSYS2_INSTALL)
  install(FILES ${DOC_FILES} DESTINATION ${CMAKE_INSTALL_DOCDIR} COMPONENT DTDocuments)
  add_subdirectory(man)
else()
  # Windows doesn't have a standardized location for apps doc subfolders like Linux does,
  # so we put the doc files in the root of installation folder for better visibility.
  #
  install(FILES ${DOC_FILES} DESTINATION ${CMAKE_INSTALL_BINDIR}/.. COMPONENT DTDocuments)
endif()
