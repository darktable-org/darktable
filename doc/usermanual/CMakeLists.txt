# FIXME: Tell CMake to cache the pdf files so they are not regenerated each time.
cmake_minimum_required(VERSION 2.6)

if(BUILD_USERMANUAL)
	add_custom_target(usermanual ALL)
else(BUILD_USERMANUAL)
	add_custom_target(usermanual)
endif(BUILD_USERMANUAL)

set(can_build_usermanual ON)

find_program(Fop_BIN fop)
find_program(Xsltproc_BIN xsltproc)
find_program(Xml2po_BIN xml2po)

if(${Fop_BIN} STREQUAL "Fop_BIN-NOTFOUND")
	message("Missing fop")
	set(can_build_usermanual OFF)
endif()

if(${Xsltproc_BIN} STREQUAL "Xsltproc_BIN-NOTFOUND")
	message("Missing xsltproc")
	set(can_build_usermanual OFF)
endif()

if(${Xml2po_BIN} STREQUAL "Xml2po_BIN-NOTFOUND")
	message("Missing xml2po. No localized usermanuals are build")
# 	set(can_build_usermanual OFF)
endif()

if(can_build_usermanual)

	# regular usermanual
	add_custom_command(
		OUTPUT darktable-usermanual.pdf
		COMMAND xsltproc --output ${CMAKE_CURRENT_BINARY_DIR}/darktable-usermanual.fo xsl/darktable_fo.xsl darktable.xml
		COMMAND fop -c fopconfig.xml ${CMAKE_CURRENT_BINARY_DIR}/darktable-usermanual.fo -pdf darktable-usermanual.pdf
		WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
		DEPENDS darktable.xml
		COMMENT "Building usermanual" VERBATIM
	)
	add_custom_target(darktable-usermanual DEPENDS darktable-usermanual.pdf)
	add_dependencies(usermanual darktable-usermanual)
	install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/darktable-usermanual.pdf DESTINATION ./share/doc/darktable/usermanual OPTIONAL)

	# final usermanual
	add_custom_command(
		OUTPUT darktable-usermanual-final.pdf
		COMMAND xsltproc --output ${CMAKE_CURRENT_BINARY_DIR}/darktable-usermanual_profiled_final.xml xsl/darktable_profile.xsl darktable.xml 
		COMMAND xsltproc --output ${CMAKE_CURRENT_BINARY_DIR}/darktable-usermanual_profiled_final.fo xsl/darktable_fo.xsl ${CMAKE_CURRENT_BINARY_DIR}/darktable-usermanual_profiled_final.xml
		COMMAND fop -c fopconfig.xml ${CMAKE_CURRENT_BINARY_DIR}/darktable-usermanual_profiled_final.fo -pdf darktable-usermanual-final.pdf
		WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
		DEPENDS darktable.xml
		COMMENT "Building final usermanual" VERBATIM
	)
	add_custom_target(darktable-usermanual-final DEPENDS darktable-usermanual-final.pdf)
	add_dependencies(usermanual darktable-usermanual-final)
	install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/darktable-usermanual-final.pdf DESTINATION ./share/doc/darktable/usermanual OPTIONAL)

	if(NOT ${Xml2po_BIN} STREQUAL "Xml2po_BIN-NOTFOUND")
		# localized final usermanuals
		file(GLOB LANGUAGES po/*.po)
		foreach(language ${LANGUAGES})
			set(pofile ${language})
			string(REGEX REPLACE "(.+(\\\\|/))+" "" language ${language})
			string(REGEX REPLACE "\\.po$" "" language ${language})
			set(pdffile "darktable-usermanual-${language}.pdf")
			set(fofile "${CMAKE_CURRENT_BINARY_DIR}/darktable-usermanual-${language}.fo")
			set(xmlfile "${CMAKE_CURRENT_BINARY_DIR}/darktable-usermanual_profiled_final.xml")
			set(xmllangfile "${CMAKE_CURRENT_BINARY_DIR}/darktable-usermanual_profiled_final-${language}.xml")
			add_custom_command(
				OUTPUT ${pdffile}
				COMMAND xsltproc --output ${xmlfile} xsl/darktable_profile.xsl darktable.xml
				COMMAND xml2po -e -p ${pofile} -l ${language} ${xmlfile} > ${xmllangfile}
				COMMAND xsltproc --output ${fofile} xsl/darktable_fo.xsl ${xmllangfile}
				COMMAND fop -c fopconfig.xml ${fofile} -pdf ${pdffile}
				DEPENDS ${pofile} darktable.xml
				WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
				COMMENT "Building ${language} usermanual" VERBATIM
			)
			add_custom_target(darktable-usermanual-${language} DEPENDS ${pdffile})
			add_dependencies(usermanual darktable-usermanual-${language})
			install(FILES ${pdffile} DESTINATION ./share/doc/darktable/usermanual OPTIONAL)
		endforeach(language)
	endif()

else(can_build_usermanual)
	message("Can NOT build usermanual")
endif(can_build_usermanual)

add_subdirectory(po)

