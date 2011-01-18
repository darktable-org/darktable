# FIXME: Add a check to see if xsltproc, xml2po and fop are installed and add the targets to ALL if that's the case.
cmake_minimum_required(VERSION 2.6)

# regular usermanual
add_custom_command(
	OUTPUT darktable-usermanual.pdf
	COMMAND xsltproc --output ${CMAKE_CURRENT_BINARY_DIR}/darktable-usermanual.fo xsl/darktable_fo.xsl darktable.xml
	COMMAND fop -c fopconfig.xml ${CMAKE_CURRENT_BINARY_DIR}/darktable-usermanual.fo -pdf darktable-usermanual.pdf
	WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
	COMMENT "Building usermanual" VERBATIM
)
add_custom_target(darktable-usermanual DEPENDS darktable-usermanual.pdf)

# final usermanual
add_custom_command(
	OUTPUT darktable-usermanual-final.pdf
	COMMAND xsltproc --output ${CMAKE_CURRENT_BINARY_DIR}/darktable-usermanual_profiled_final.xml xsl/darktable_profile.xsl darktable.xml 
	COMMAND xsltproc --output ${CMAKE_CURRENT_BINARY_DIR}/darktable-usermanual_profiled_final.fo xsl/darktable_fo.xsl ${CMAKE_CURRENT_BINARY_DIR}/darktable-usermanual_profiled_final.xml
	COMMAND fop -c fopconfig.xml ${CMAKE_CURRENT_BINARY_DIR}/darktable-usermanual_profiled_final.fo -pdf darktable-usermanual-final.pdf
	WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
	COMMENT "Building final usermanual" VERBATIM
)
add_custom_target(darktable-usermanual-final DEPENDS darktable-usermanual-final.pdf)

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
		DEPENDS ${pofile}
		WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
		COMMENT "Building ${language} usermanual" VERBATIM
	)
	add_custom_target(darktable-usermanual-${language} DEPENDS ${pdffile})
endforeach(language)
