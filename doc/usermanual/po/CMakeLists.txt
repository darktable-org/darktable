cmake_minimum_required(VERSION 2.8.5)

set(can_update_usermanual ON)

find_program(Xsltproc_BIN xsltproc)
find_program(Xml2po_BIN xml2po)

if(${Xsltproc_BIN} STREQUAL "Xsltproc_BIN-NOTFOUND")
	message("Missing xsltproc")
	set(can_update_usermanual OFF)
endif()

if(${Xml2po_BIN} STREQUAL "Xml2po_BIN-NOTFOUND")
	message("Missing xml2po")
	set(can_update_usermanual OFF)
endif()

if(can_update_usermanual )

	add_custom_target(update-usermanual)

	# update template
	add_custom_command(
		OUTPUT darktable-usermanual.pot
		COMMAND xsltproc --xinclude --output ${CMAKE_CURRENT_BINARY_DIR}/darktable-usermanual_profiled.xml ../xsl/darktable_profile.xsl ../darktable.xml
		COMMAND xml2po -a -e -o darktable-usermanual.pot ${CMAKE_CURRENT_BINARY_DIR}/darktable-usermanual_profiled.xml
		WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
		DEPENDS ../darktable.xml ../xsl/darktable_profile.xsl
		COMMENT "Updating usermanual template" VERBATIM
	)
	add_custom_target(update-template DEPENDS darktable-usermanual.pot)

	# localized draft usermanuals
	file(STRINGS "LINGUAS" LANGUAGES)
	foreach(language ${LANGUAGES})
		set(pofile "${language}.po")
		add_custom_command(
			OUTPUT ${pofile}
			COMMAND sh -c "if test ! -f ${pofile}; then ${CMAKE_COMMAND} -E copy darktable-usermanual.pot ${pofile}; fi"
			COMMAND xml2po -a -e -u ${pofile} ${CMAKE_CURRENT_BINARY_DIR}/darktable-usermanual_profiled.xml
			WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
			COMMENT "Updating ${language} usermanual pofile" VERBATIM
		)
		add_custom_target(update-usermanual-${language} DEPENDS darktable-usermanual.pot ${pofile})
		add_dependencies(update-usermanual update-usermanual-${language})
		add_dependencies(update-usermanual-${language} update-template)
	endforeach(language)

else(can_update_usermanual )
	message("Can NOT update usermanual translations")
endif(can_update_usermanual )
