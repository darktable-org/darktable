# FIXME: Tell CMake to cache the pdf files so they are not regenerated each time.
cmake_minimum_required(VERSION 2.6)

FILE(GLOB_RECURSE IMAGE_FILES RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} "*.png")
FILE(GLOB_RECURSE IMAGES_PATHS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} "images")

message(STATUS ${IMAGES_PATHS})

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

	set(TARGET_MEDIA_COMPRESSION_QUALITY "90")
	set(TARGET_MEDIA_RESIZE "800x800>")
	
	# Create directory tree for media target image transformation
	foreach(image ${IMAGE_FILES})
		# Rewrite output url for transformed images
		string(REGEX REPLACE "\\images/" "" _image ${image})
		set(_image "images/${_image}")
		# cut out filename from path
		string(REGEX REPLACE "\\/[^/]+.png$" "" path ${_image})
		file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/media/${path})
	endforeach()

	# Images converting for different media targets
	set(TARGET_MEDIA_IMAGES "")
	
	foreach(image ${IMAGE_FILES})
		# Remove image extensions png
		string(REGEX REPLACE "\\.png$" "" image ${image})
		
		# Rewrite output url for transformed images
		string(REGEX REPLACE "\\images/" "" new_image ${image})
		set(new_image "images/${new_image}")
		set(TARGET_MEDIA_IMAGES ${TARGET_MEDIA_IMAGES} "${CMAKE_CURRENT_BINARY_DIR}/media/${new_image}.jpg")
		add_custom_command(
			OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/media/${new_image}.jpg"
			COMMAND convert -resize ${TARGET_MEDIA_RESIZE} -quality ${TARGET_MEDIA_COMPRESSION_QUALITY} -filter Lanczos ${CMAKE_CURRENT_SOURCE_DIR}/${image}.png ${CMAKE_CURRENT_BINARY_DIR}/media/${new_image}.jpg 
			COMMENT "Tranforming image ${image}.png into media target" VERBATIM
		)
	endforeach()
	add_custom_target(target_media_images DEPENDS  ${TARGET_MEDIA_IMAGES})

	#
	# First pass generate one big docbook xml
	# all document generation should depend and use this file
	#
	string(REGEX REPLACE "/" "\\\\/" binary_path ${CMAKE_CURRENT_BINARY_DIR})
	add_custom_command(
		OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/darktable_single.xml
		COMMAND xsltproc --xinclude --output ${CMAKE_CURRENT_BINARY_DIR}/darktable_single.tmp xsl/darktable_profile.xsl darktable.xml
		COMMAND cat ${CMAKE_CURRENT_BINARY_DIR}/darktable_single.tmp | sed "s/fileref=\"\\([^>]*\\)\\(\\/images\\/\\)\\([^>]*\\)\\(.png\\)\"/fileref=\"${binary_path}\\/media\\/images\\/\\1\\/\\3.jpg\"/g ; s/fileref=\"images\\/\\([^>]*\\)\\(.png\\)\"/fileref=\"${binary_path}\\/media\\/images\\/\\1.jpg\"/g" > ${CMAKE_CURRENT_BINARY_DIR}/darktable_single.xml
		WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
		DEPENDS darktable.xml
		COMMENT "Generating the main docbook xml file" VERBATIM
	)
	add_custom_target(darktable_single_xml DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/darktable_single.xml)
	
	#
	# add target for final xml file
	#
	add_custom_command(
		OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/darktable_single_final.xml
		COMMAND xsltproc --output ${CMAKE_CURRENT_BINARY_DIR}/darktable_single_final.xml --stringparam profile.status "final" xsl/darktable_profile.xsl ${CMAKE_CURRENT_BINARY_DIR}/darktable_single.xml
		WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
		DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/darktable_single.xml
		COMMENT "Profiling final docbook xml file" VERBATIM
	)
	add_custom_target(darktable_single_final_xml DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/darktable_single.xml)
	
	#
	# draft pdf usermanual
	#
	add_custom_command(
		OUTPUT darktable-usermanual.pdf
		COMMAND xsltproc --output ${CMAKE_CURRENT_BINARY_DIR}/darktable-usermanual.fo xsl/darktable_fo.xsl  ${CMAKE_CURRENT_BINARY_DIR}/darktable_single.xml
		COMMAND fop -c fopconfig.xml ${CMAKE_CURRENT_BINARY_DIR}/darktable-usermanual.fo -pdf darktable-usermanual.pdf
		WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
		DEPENDS  ${CMAKE_CURRENT_BINARY_DIR}/darktable_single.xml
		COMMENT "Building usermanual" VERBATIM
	)
	add_custom_target(darktable-usermanual DEPENDS darktable-usermanual.pdf)
	add_dependencies(darktable-usermanual target_media_images darktable_single_xml )
	install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/darktable-usermanual.pdf DESTINATION ./share/doc/darktable/usermanual OPTIONAL)

	#
	# draft chunked html usermanual for web
	#
	add_custom_command(
		OUTPUT  ${CMAKE_CURRENT_BINARY_DIR}/html/usermanual.css
		COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/usermanual.css ${CMAKE_CURRENT_BINARY_DIR}/html/usermanual.css
		DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/usermanual.css
		COMMENT "Updating usermanual.css" VERBATIM
	)
	
	add_custom_command(
		OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/html/index.html
		COMMAND xsltproc --stringparam base.dir ${CMAKE_CURRENT_BINARY_DIR}/html/ xsl/darktable_html.xsl ${CMAKE_CURRENT_BINARY_DIR}/darktable_single.xml
		COMMAND find ${CMAKE_CURRENT_BINARY_DIR}/html -name *.html -exec sed -i s/${binary_path}\\/media\\///g {} \;
		COMMAND find ${CMAKE_CURRENT_BINARY_DIR}/html -name *.html -exec sed -i "s/<body[^>]*>/<body class='usermanual' onLoad='parent.onContentLoad(this);'>/g" {} \;
		COMMAND cp -r ${CMAKE_CURRENT_BINARY_DIR}/media/images ${CMAKE_CURRENT_BINARY_DIR}/html/
		WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
		DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/darktable_single.xml ${CMAKE_CURRENT_BINARY_DIR}/html/usermanual.css
		COMMENT "Building html usermanual" VERBATIM
	)
	add_custom_target(darktable-usermanual-html DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/html/index.html )
	add_dependencies(darktable-usermanual-html target_media_images darktable_single_xml )
	install(DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/html DESTINATION ./share/doc/darktable/usermanual OPTIONAL)
	
	#
	# draft chunked usermanual for wordpress
	#
	
	add_custom_command(
		OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/wp/index.html.php
		COMMAND xsltproc  --stringparam chunker.output.encoding UTF-8 --stringparam base.dir ${CMAKE_CURRENT_BINARY_DIR}/wp/ xsl/darktable_html.xsl ${CMAKE_CURRENT_BINARY_DIR}/darktable_single.xml
		COMMAND find ${CMAKE_CURRENT_BINARY_DIR}/wp -name *.html -exec sed -i s/${binary_path}\\/media\\///g {} \;
		COMMAND find ${CMAKE_CURRENT_BINARY_DIR}/wp -name *.html -exec sed -i "s/<html>.*<title>/<?php define('WP_USE_THEMES', false);require('\\.\\.\\/wp-config\\.php'); add_filter('wp_title', 'set_title_page'); function set_title_page(){echo '/g" {} \;
		COMMAND find ${CMAKE_CURRENT_BINARY_DIR}/wp -name *.html -exec sed -i "s/<\\/title>.*<\\/head>/ | user manual | ';}?>/g" {} \;

		COMMAND find ${CMAKE_CURRENT_BINARY_DIR}/wp -name *.html -exec sed -i "s/<body[^>]*>/<?php get_header(); ?><div id=\"primary\"><div id=\"content\" role=\"main\"><div class=\"usermanual\">/g" {} \;
		COMMAND find ${CMAKE_CURRENT_BINARY_DIR}/wp -name *.html -exec sed -i "s/<\\/body><\\/html>/<\\/div><\\/div><\\/div><?php get_footer(); ?>/g" {} \;
		COMMAND find ${CMAKE_CURRENT_BINARY_DIR}/wp -name *.html -exec sed -i "s/\\.html/\\.html\\.php/g" {} \;
		COMMAND find ${CMAKE_CURRENT_BINARY_DIR}/wp -name *.html -exec mv {} {}.php \;
		COMMAND cp -r ${CMAKE_CURRENT_BINARY_DIR}/media/images ${CMAKE_CURRENT_BINARY_DIR}/wp/
		WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
		DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/darktable_single.xml
		COMMENT "Building php usermanual" VERBATIM
	)
	add_custom_target(darktable-usermanual-wordpress DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/wp/index.html.php )
	add_dependencies(darktable-usermanual-wordpress target_media_images darktable_single_xml )
	install(DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/wp DESTINATION ./share/doc/darktable/usermanual OPTIONAL)
	
	#
	# final translated usermanual's
	#
	add_custom_command(
		OUTPUT darktable-usermanual-en.pdf
		COMMAND xsltproc --output ${CMAKE_CURRENT_BINARY_DIR}/darktable-usermanual_profiled_final.fo xsl/darktable_fo.xsl ${CMAKE_CURRENT_BINARY_DIR}/darktable-usermanual_profiled_final.xml
		COMMAND fop -c fopconfig.xml ${CMAKE_CURRENT_BINARY_DIR}/darktable-usermanual_profiled_final.fo -pdf darktable-usermanual-final.pdf
		WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
		DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/darktable_single.xml
		COMMENT "Building final usermanual" VERBATIM
	)
	add_custom_target(darktable-usermanual-final DEPENDS darktable-usermanual-en.pdf )
	add_dependencies(darktable-usermanual-final target_media_images darktable_single_final_xml )
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
			set(xmlfile "${CMAKE_CURRENT_BINARY_DIR}/darktable_single_final.xml")
			set(xmllangfile "${CMAKE_CURRENT_BINARY_DIR}/darktable_single_final-${language}.xml")
			add_custom_command(
				OUTPUT ${pdffile}
				COMMAND xml2po -e -p ${pofile} -l ${language} ${xmlfile} > ${xmllangfile}
				COMMAND xsltproc --output ${fofile} xsl/darktable_fo.xsl ${xmllangfile}
				COMMAND fop -c fopconfig.xml ${fofile} -pdf ${pdffile}
				DEPENDS ${pofile} ${CMAKE_CURRENT_BINARY_DIR}/darktable_single_final.xml
				WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
				COMMENT "Building ${language} usermanual" VERBATIM
			)
			add_custom_target(darktable-usermanual-${language} DEPENDS ${pdffile})
			add_dependencies(usermanual darktable-usermanual-${language})
			add_dependencies(darktable-usermanual-${language} target_media_images darktable_single_final_xml )
			install(FILES ${pdffile} DESTINATION ./share/doc/darktable/usermanual OPTIONAL)
		endforeach(language)
	endif()

else(can_build_usermanual)
	message("Can NOT build usermanual")
endif(can_build_usermanual)

add_subdirectory(po)

