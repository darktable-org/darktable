# FIXME: Tell CMake to cache the pdf files so they are not regenerated each time.
cmake_minimum_required(VERSION 2.8.5)

FILE(GLOB_RECURSE IMAGE_FILES RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} "*.png")
FILE(GLOB_RECURSE IMAGES_PATHS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} "images")
FILE(GLOB_RECURSE XML_FILES RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} "*.xml")

message(STATUS ${IMAGES_PATHS})

if(BUILD_USERMANUAL)
	add_custom_target(usermanual ALL)
	add_custom_target(lua-api ALL)
else(BUILD_USERMANUAL)
	add_custom_target(usermanual)
	add_custom_target(lua-api)
endif(BUILD_USERMANUAL)

set(can_build_usermanual ON)

find_program(Fop_BIN fop)
find_program(Xml2po_BIN xml2po)
find_package(Saxon)
find_program(Xslt_BIN xsltproc)
find_program(Convert_BIN convert)

if(${Fop_BIN} STREQUAL "Fop_BIN-NOTFOUND")
	message("Missing fop")
	set(can_build_usermanual OFF)
endif()

if((NOT Saxon_FOUND) AND (${Xslt_BIN} STREQUAL "Xslt_BIN-NOTFOUND"))
	message("No Xslt processor found")
	set(can_build_usermanual OFF)
endif()

if(NOT Saxon_FOUND)
        message("Xslt processor saxon not found. HTML usermanual will be built with xsltproc;")
        message("expect usermanual with inconsistent screenshot and image dimensions.")
	set(XSLT_XSLTPROC_EXECUTABLE "${Xslt_BIN}")
endif()

if(${Xml2po_BIN} STREQUAL "Xml2po_BIN-NOTFOUND")
	message("Missing xml2po. No localized usermanuals are build")
# 	set(can_build_usermanual OFF)
endif()

if(${Convert_BIN} STREQUAL "Convert_BIN-NOTFOUND")
	message("Missing convert from ImageMagick")
	set(can_build_usermanual OFF)
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
			COMMAND ${Convert_BIN} -resize ${TARGET_MEDIA_RESIZE} -quality ${TARGET_MEDIA_COMPRESSION_QUALITY} -filter Lanczos ${CMAKE_CURRENT_SOURCE_DIR}/${image}.png ${CMAKE_CURRENT_BINARY_DIR}/media/${new_image}.jpg
			COMMENT "Transforming image ${image}.png into media target" VERBATIM
		)
	endforeach()
	add_custom_target(target_media_images DEPENDS  ${TARGET_MEDIA_IMAGES})

	#
	# First pass generate one big docbook xml
	# all generation of original language documents should depend on and use this file
        # all translated usermanuals should depend on the profiled xml file
	#
	string(REGEX REPLACE "/" "\\\\/" binary_path ${CMAKE_CURRENT_BINARY_DIR})
	add_custom_command(
		OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/darktable_profile.xml
		COMMAND xsltproc --xinclude --output ${CMAKE_CURRENT_BINARY_DIR}/darktable_profile.xml xsl/darktable_profile.xsl darktable.xml
		WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
		DEPENDS darktable.xml ${XML_FILES}
		COMMENT "Generating the profiled docbook xml file" VERBATIM
	)
	add_custom_target(darktable_profile_xml DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/darktable_profile.xml)
	add_custom_command(
		OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/darktable_single.xml
		COMMAND cat ${CMAKE_CURRENT_BINARY_DIR}/darktable_profile.xml | sed "s/fileref=\"\\([^>]*\\)\\(\\/images\\/\\)\\([^>]*\\)\\(.png\\)\"/fileref=\"${binary_path}\\/media\\/images\\/\\1\\/\\3.jpg\"/g ; s/fileref=\"images\\/\\([^>]*\\)\\(.png\\)\"/fileref=\"${binary_path}\\/media\\/images\\/\\1.jpg\"/g" > ${CMAKE_CURRENT_BINARY_DIR}/darktable_single.xml
		WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
		DEPENDS darktable_profile.xml ${XML_FILES}
		COMMENT "Generating the main docbook xml file" VERBATIM
	)
	add_custom_target(darktable_single_xml DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/darktable_single.xml)



#	#
#	# add target for final xml file
#	#
#	add_custom_command(
#		OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/darktable_single_final.xml
#		COMMAND xsltproc --output ${CMAKE_CURRENT_BINARY_DIR}/darktable_single_final.xml --stringparam profile.status "final" xsl/darktable_profile.xsl ${CMAKE_CURRENT_BINARY_DIR}/darktable_single.xml
#		WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
#		DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/darktable_single.xml
#		COMMENT "Profiling final docbook xml file" VERBATIM
#	)
#	add_custom_target(darktable_single_final_xml DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/darktable_single.xml)

	#
	# draft pdf usermanual
	#
	add_custom_command(
		OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/darktable-usermanual.pdf
		COMMAND xsltproc --output ${CMAKE_CURRENT_BINARY_DIR}/darktable-usermanual.fo xsl/darktable_fo.xsl  ${CMAKE_CURRENT_BINARY_DIR}/darktable_single.xml
		COMMAND fop -c fopconfig.xml ${CMAKE_CURRENT_BINARY_DIR}/darktable-usermanual.fo -pdf ${CMAKE_CURRENT_BINARY_DIR}/darktable-usermanual.pdf
		WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
		DEPENDS  ${CMAKE_CURRENT_BINARY_DIR}/darktable_single.xml
		COMMENT "Building usermanual" VERBATIM
	)
	add_custom_target(darktable-usermanual DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/darktable-usermanual.pdf)
  add_dependencies(usermanual darktable-usermanual)
	add_dependencies(darktable-usermanual target_media_images darktable_single_xml )
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/darktable-usermanual.pdf DESTINATION ${CMAKE_INSTALL_DOCDIR}/usermanual OPTIONAL)

	#
	# draft chunked html usermanual for web
	#
	add_custom_command(
		OUTPUT  ${CMAKE_CURRENT_BINARY_DIR}/html/usermanual.css
		COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/usermanual.css ${CMAKE_CURRENT_BINARY_DIR}/html/usermanual.css
		DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/usermanual.css
		COMMENT "Updating usermanual.css" VERBATIM
	)

        if (Saxon_FOUND)
	    add_custom_command(
		OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/html/index.html
                COMMAND ${XSLT_SAXON_COMMAND} ${CMAKE_CURRENT_BINARY_DIR}/darktable_single.xml xsl/darktable_html_single_ext.xsl chunker.output.encoding=UTF-8 base.dir=${CMAKE_CURRENT_BINARY_DIR}/html/
		COMMAND find ${CMAKE_CURRENT_BINARY_DIR}/html -name *.html -exec sed -n -i "1h;1!H;\${;g;s/${binary_path}\\/media\\///g;p;}" {} \;
		COMMAND find ${CMAKE_CURRENT_BINARY_DIR}/html -name *.html -exec sed -n -i "1h;1!H;\${;g;s/<body[^>]*>/<body class='usermanual' onLoad='parent.onContentLoad(this);'>/g;p;}" {} \;
		COMMAND cp -r ${CMAKE_CURRENT_BINARY_DIR}/media/images ${CMAKE_CURRENT_BINARY_DIR}/html/
		WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
		DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/darktable_single.xml ${CMAKE_CURRENT_BINARY_DIR}/html/usermanual.css
		COMMENT "Building html usermanual" VERBATIM
	)
        else (Saxon_FOUND)
            add_custom_command(
		OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/html/index.html
                COMMAND ${XSLT_XSLTPROC_EXECUTABLE} --stringparam chunker.output.encoding UTF-8 --stringparam base.dir ${CMAKE_CURRENT_BINARY_DIR}/html/ xsl/darktable_html_single.xsl ${CMAKE_CURRENT_BINARY_DIR}/darktable_single.xml
		COMMAND find ${CMAKE_CURRENT_BINARY_DIR}/html -name *.html -exec sed -i s/${binary_path}\\/media\\///g {} \;
		COMMAND find ${CMAKE_CURRENT_BINARY_DIR}/html -name *.html -exec sed -i "s/<body[^>]*>/<body class='usermanual' onLoad='parent.onContentLoad(this);'>/g" {} \;
		COMMAND cp -r ${CMAKE_CURRENT_BINARY_DIR}/media/images ${CMAKE_CURRENT_BINARY_DIR}/html/
		WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
		DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/darktable_single.xml ${CMAKE_CURRENT_BINARY_DIR}/html/usermanual.css
		COMMENT "Building html usermanual" VERBATIM
	)
        endif (Saxon_FOUND)
	add_custom_target(darktable-usermanual-html DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/html/index.html )
	add_dependencies(darktable-usermanual-html target_media_images darktable_single_xml )
    install(DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/html DESTINATION ${CMAKE_INSTALL_DOCDIR}/usermanual OPTIONAL)

	#
	# draft chunked usermanual for wordpress
	#

        if (Saxon_FOUND)
            add_custom_command(
		OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/wp/index.html.php
                COMMAND ${XSLT_SAXON_COMMAND} ${CMAKE_CURRENT_BINARY_DIR}/darktable_single.xml xsl/darktable_html_ext.xsl chunker.output.encoding=UTF-8 base.dir=${CMAKE_CURRENT_BINARY_DIR}/wp/
		COMMAND find ${CMAKE_CURRENT_BINARY_DIR}/wp -name *.html -exec sed -n -i "1h;1!H;\${;g;s/${binary_path}\\/media\\///g;p;}" {} \;
		COMMAND find ${CMAKE_CURRENT_BINARY_DIR}/wp -name *.html -exec sed -n -i "1h;1!H;\${;g;s/<html>.*<title>/<?php define('WP_USE_THEMES', false);require('\\.\\.\\/wp-config\\.php'); add_filter('wp_title', 'set_title_page'); function set_title_page(){echo '/g;p;}" {} \;
		COMMAND find ${CMAKE_CURRENT_BINARY_DIR}/wp -name *.html -exec sed -n -i "1h;1!H;\${;g;s/<\\/title>.*<\\/head>/ | user manual | ';}?>/g;p;}" {} \;

		COMMAND find ${CMAKE_CURRENT_BINARY_DIR}/wp -name *.html -exec sed -n -i "1h;1!H;\${;g;s/<body[^>]*>/<?php get_header(); ?><div id=\"primary\"><div id=\"content\" role=\"main\"><div class=\"usermanual\">/g;p;}" {} \;
		COMMAND find ${CMAKE_CURRENT_BINARY_DIR}/wp -name *.html -exec sed -n -i "1h;1!H;\${;g;s/<\\/body><\\/html>/<\\/div><\\/div><\\/div><?php get_footer(); ?>/g;p;}" {} \;
		COMMAND find ${CMAKE_CURRENT_BINARY_DIR}/wp -name *.html -exec sed -n -i "1h;1!H;\${;g;s|\"\\([^\":]*\\.html\\)|\"\\1.php|g;p;}" {} \;
		COMMAND find ${CMAKE_CURRENT_BINARY_DIR}/wp -name *.html -exec mv {} {}.php \;
		COMMAND cp -r ${CMAKE_CURRENT_BINARY_DIR}/media/images ${CMAKE_CURRENT_BINARY_DIR}/wp/
		WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
		DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/darktable_single.xml
		COMMENT "Building php usermanual" VERBATIM
	)
        else (Saxon_FOUND)
            add_custom_command(
		OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/wp/index.html.php
                COMMAND ${XSLT_XSLTPROC_EXECUTABLE} --stringparam chunker.output.encoding UTF-8 --stringparam base.dir ${CMAKE_CURRENT_BINARY_DIR}/wp/ xsl/darktable_html.xsl ${CMAKE_CURRENT_BINARY_DIR}/darktable_single.xml
		COMMAND find ${CMAKE_CURRENT_BINARY_DIR}/wp -name *.html -exec sed -i s/${binary_path}\\/media\\///g {} \;
		COMMAND find ${CMAKE_CURRENT_BINARY_DIR}/wp -name *.html -exec sed -i "s/<html>.*<title>/<?php define('WP_USE_THEMES', false);require('\\.\\.\\/wp-config\\.php'); add_filter('wp_title', 'set_title_page'); function set_title_page(){echo '/g" {} \;
		COMMAND find ${CMAKE_CURRENT_BINARY_DIR}/wp -name *.html -exec sed -i "s/<\\/title>.*<\\/head>/ | user manual | ';}?>/g" {} \;

		COMMAND find ${CMAKE_CURRENT_BINARY_DIR}/wp -name *.html -exec sed -i "s/<body[^>]*>/<?php get_header(); ?><div id=\"primary\"><div id=\"content\" role=\"main\"><div class=\"usermanual\">/g" {} \;
		COMMAND find ${CMAKE_CURRENT_BINARY_DIR}/wp -name *.html -exec sed -i "s/<\\/body><\\/html>/<\\/div><\\/div><\\/div><?php get_footer(); ?>/g" {} \;
		COMMAND find ${CMAKE_CURRENT_BINARY_DIR}/wp -name *.html -exec sed -i "s|\"\\([^\":]*\\.html\\)|\"\\1.php|g" {} \;
		COMMAND find ${CMAKE_CURRENT_BINARY_DIR}/wp -name *.html -exec mv {} {}.php \;
		COMMAND cp -r ${CMAKE_CURRENT_BINARY_DIR}/media/images ${CMAKE_CURRENT_BINARY_DIR}/wp/
		WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
		DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/darktable_single.xml
		COMMENT "Building php usermanual" VERBATIM
	)
        endif (Saxon_FOUND)
	add_custom_target(darktable-usermanual-wordpress DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/wp/index.html.php )
	add_dependencies(darktable-usermanual-wordpress target_media_images darktable_single_xml )
    install(DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/wp DESTINATION ${CMAKE_INSTALL_DOCDIR}/usermanual OPTIONAL)

#	#
#	# final translated usermanual's
#	#
#	add_custom_command(
#		OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/darktable-usermanual-en.pdf
#		COMMAND xsltproc --output ${CMAKE_CURRENT_BINARY_DIR}/darktable-usermanual_profiled_final.fo xsl/darktable_fo.xsl ${CMAKE_CURRENT_BINARY_DIR}/darktable_single_final.xml
#		COMMAND fop -c fopconfig.xml ${CMAKE_CURRENT_BINARY_DIR}/darktable-usermanual_profiled_final.fo -pdf ${CMAKE_CURRENT_BINARY_DIR}/darktable-usermanual-final.pdf
#		WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
#		DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/darktable_single.xml
#		COMMENT "Building final usermanual" VERBATIM
#	)
#	add_custom_target(darktable-usermanual-final DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/darktable-usermanual-en.pdf )
#	add_dependencies(darktable-usermanual-final target_media_images darktable_single_final_xml )
#	add_dependencies(usermanual darktable-usermanual-final)
#	install(FILES ${CMAKE_CURRENT_BINARY_DIR}/darktable-usermanual-final.pdf DESTINATION ${CMAKE_INSTALL_DOCDIR}/usermanual OPTIONAL)
#
#	if(NOT ${Xml2po_BIN} STREQUAL "Xml2po_BIN-NOTFOUND")
#		# localized final usermanuals
#		file(GLOB LANGUAGES po/*.po)
#		foreach(language ${LANGUAGES})
#			set(pofile ${language})
#			string(REGEX REPLACE "(.+(\\\\|/))+" "" language ${language})
#			string(REGEX REPLACE "\\.po$" "" language ${language})
#			set(pdffile "${CMAKE_CURRENT_BINARY_DIR}/darktable-usermanual-${language}.pdf")
#			set(fofile "${CMAKE_CURRENT_BINARY_DIR}/darktable-usermanual-${language}.fo")
#			set(xmlfile "${CMAKE_CURRENT_BINARY_DIR}/darktable_single_final.xml")
#			set(xmllangfile "${CMAKE_CURRENT_BINARY_DIR}/darktable_single_final-${language}.xml")
#			add_custom_command(
#				OUTPUT ${pdffile}
#				COMMAND xml2po -e -p ${pofile} -l ${language} ${xmlfile} > ${xmllangfile}
#				COMMAND xsltproc --output ${fofile} xsl/darktable_fo.xsl ${xmllangfile}
#				COMMAND fop -c fopconfig.xml ${fofile} -pdf ${pdffile}
#				DEPENDS ${pofile} ${CMAKE_CURRENT_BINARY_DIR}/darktable_single_final.xml
#				WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
#				COMMENT "Building ${language} usermanual" VERBATIM
#			)
#			add_custom_target(darktable-usermanual-${language} DEPENDS ${pdffile})
#			add_dependencies(usermanual darktable-usermanual-${language})
#			add_dependencies(darktable-usermanual-${language} target_media_images darktable_single_final_xml )
#			install(FILES ${pdffile} DESTINATION ${CMAKE_INSTALL_DOCDIR}/usermanual OPTIONAL)
#		endforeach(language)
#	endif()

	#
	# draft translated usermanuals
	#
	if(NOT ${Xml2po_BIN} STREQUAL "Xml2po_BIN-NOTFOUND")
		# localized draft usermanuals
		file(STRINGS "po/LINGUAS" LANGUAGES)
		foreach(language ${LANGUAGES})
			set(pofile "po/${language}.po")
			set(pdffile "${CMAKE_CURRENT_BINARY_DIR}/darktable-usermanual-${language}.pdf")
			set(fofile "${CMAKE_CURRENT_BINARY_DIR}/darktable-usermanual-${language}.fo")
			set(xmlfile "${CMAKE_CURRENT_BINARY_DIR}/darktable_profile.xml")
			set(xmllangfile "${CMAKE_CURRENT_BINARY_DIR}/darktable_single-${language}.xml")
			set(xmllangtmp "${CMAKE_CURRENT_BINARY_DIR}/darktable_single-${language}.tmp")
			add_custom_command(
				OUTPUT ${pdffile}
				COMMAND xml2po -e -p ${pofile} -l ${language} ${xmlfile} > ${xmllangtmp}
		                COMMAND cat ${xmllangtmp} | sed "s/fileref=\"\\([^>]*\\)\\(\\/images\\/\\)\\([^>]*\\)\\(.png\\)\"/fileref=\"${binary_path}\\/media\\/images\\/\\1\\/\\3.jpg\"/g ; s/fileref=\"images\\/\\([^>]*\\)\\(.png\\)\"/fileref=\"${binary_path}\\/media\\/images\\/\\1.jpg\"/g" > ${xmllangfile}
				COMMAND xsltproc --output ${fofile} xsl/darktable_fo.xsl ${xmllangfile}
				COMMAND fop -c fopconfig.xml ${fofile} -pdf ${pdffile}
				DEPENDS ${pofile} ${CMAKE_CURRENT_BINARY_DIR}/darktable_single.xml
				WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
				COMMENT "Building ${language} usermanual" VERBATIM
			)
			add_custom_target(darktable-usermanual-${language} DEPENDS ${pdffile})
			add_dependencies(usermanual darktable-usermanual-${language})
			add_dependencies(darktable-usermanual-${language} target_media_images darktable_profile_xml )
            install(FILES ${pdffile} DESTINATION ${CMAKE_INSTALL_DOCDIR}/usermanual OPTIONAL)
		endforeach(language)
	endif()
#
# LUA API GENERATION
#

	#
	# draft pdf API
	#
	add_custom_command(
		OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/darktable-lua-api.pdf
		COMMAND xsltproc --output ${CMAKE_CURRENT_BINARY_DIR}/darktable-lua-api.fo xsl/darktable_fo.xsl  ${CMAKE_CURRENT_SOURCE_DIR}/lua/lua_api.xml
		COMMAND fop -c fopconfig.xml ${CMAKE_CURRENT_BINARY_DIR}/darktable-lua-api.fo -pdf ${CMAKE_CURRENT_BINARY_DIR}/darktable-lua-api.pdf
		WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
		DEPENDS  ${CMAKE_CURRENT_SOURCE_DIR}/lua/lua_api.xml
		COMMENT "Building Lua API" VERBATIM
	)
	add_custom_target(darktable-lua-api DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/darktable-lua-api.pdf)
  add_dependencies(lua-api darktable-lua-api)
	add_dependencies(darktable-lua-api target_media_images )
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/darktable-lua-api.pdf DESTINATION ${CMAKE_INSTALL_DOCDIR}/lua-api OPTIONAL)

	#
	# draft chunked html api for web
	#
	add_custom_command(
		OUTPUT  ${CMAKE_CURRENT_BINARY_DIR}/html_lua_api/usermanual.css
		COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/usermanual.css ${CMAKE_CURRENT_BINARY_DIR}/html_lua_api/usermanual.css
		DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/usermanual.css
		COMMENT "Updating usermanual.css" VERBATIM
	)

        if (Saxon_FOUND)
	    add_custom_command(
		OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/html_lua_api/index.html
                COMMAND ${XSLT_SAXON_COMMAND} ${CMAKE_CURRENT_SOURCE_DIR}/lua/lua_api.xml xsl/darktable_html_single_ext.xsl chunker.output.encoding=UTF-8 base.dir=${CMAKE_CURRENT_BINARY_DIR}/html_lua_api/
		COMMAND find ${CMAKE_CURRENT_BINARY_DIR}/html_lua_api -name *.html -exec sed -n -i "1h;1!H;\${;g;s/${binary_path}\\/media\\///g;p;}" {} \;
		COMMAND find ${CMAKE_CURRENT_BINARY_DIR}/html_lua_api -name *.html -exec sed -n -i "1h;1!H;\${;g;s/<body[^>]*>/<body class='usermanual' onLoad='parent.onContentLoad(this);'>/g;p;}" {} \;
		COMMAND cp -r ${CMAKE_CURRENT_BINARY_DIR}/media/images ${CMAKE_CURRENT_BINARY_DIR}/html_lua_api/
		WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
		DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/lua/lua_api.xml ${CMAKE_CURRENT_BINARY_DIR}/html_lua_api/usermanual.css
		COMMENT "Building html Lua API" VERBATIM
	)
        else (Saxon_FOUND)
            add_custom_command(
		OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/html_lua_api/index.html
                COMMAND ${XSLT_XSLTPROC_EXECUTABLE} --stringparam chunker.output.encoding UTF-8 --stringparam base.dir ${CMAKE_CURRENT_BINARY_DIR}/html_lua_api/ xsl/darktable_html_single.xsl ${CMAKE_CURRENT_SOURCE_DIR}/lua/lua_api.xml
		COMMAND find ${CMAKE_CURRENT_BINARY_DIR}/html_lua_api -name *.html -exec sed -i s/${binary_path}\\/media\\///g {} \;
		COMMAND find ${CMAKE_CURRENT_BINARY_DIR}/html_lua_api -name *.html -exec sed -i "s/<body[^>]*>/<body class='usermanual' onLoad='parent.onContentLoad(this);'>/g" {} \;
		COMMAND cp -r ${CMAKE_CURRENT_BINARY_DIR}/media/images ${CMAKE_CURRENT_BINARY_DIR}/html_lua_api/
		WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
		DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/lua/lua_api.xml ${CMAKE_CURRENT_BINARY_DIR}/html_lua_api/usermanual.css
		COMMENT "Building html Lua API" VERBATIM
	)
        endif (Saxon_FOUND)
	add_custom_target(darktable-lua-api-html DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/html_lua_api/index.html )
	add_dependencies(darktable-lua-api-html target_media_images)
    install(DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/html_lua_api DESTINATION ${CMAKE_INSTALL_DOCDIR}/lua-api OPTIONAL)

	#
	# draft chunked usermanual for wordpress
	#

        if (Saxon_FOUND)
            add_custom_command(
		OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/wp_lua_api/index.html.php
                COMMAND ${XSLT_SAXON_COMMAND} ${CMAKE_CURRENT_SOURCE_DIR}/lua/lua_api.xml xsl/darktable_html_ext.xsl chunker.output.encoding=UTF-8 base.dir=${CMAKE_CURRENT_BINARY_DIR}/wp_lua_api/
		COMMAND find ${CMAKE_CURRENT_BINARY_DIR}/wp_lua_api -name *.html -exec sed -n -i "1h;1!H;\${;g;s/${binary_path}\\/media\\///g;p;}" {} \;
		COMMAND find ${CMAKE_CURRENT_BINARY_DIR}/wp_lua_api -name *.html -exec sed -n -i "1h;1!H;\${;g;s/<html>.*<title>/<?php define('WP_USE_THEMES', false);require('\\.\\.\\/wp-config\\.php'); add_filter('wp_title', 'set_title_page'); function set_title_page(){echo '/g;p;}" {} \;
		COMMAND find ${CMAKE_CURRENT_BINARY_DIR}/wp_lua_api -name *.html -exec sed -n -i "1h;1!H;\${;g;s/<\\/title>.*<\\/head>/ | user manual | ';}?>/g;p;}" {} \;

		COMMAND find ${CMAKE_CURRENT_BINARY_DIR}/wp_lua_api -name *.html -exec sed -n -i "1h;1!H;\${;g;s/<body[^>]*>/<?php get_header(); ?><div id=\"primary\"><div id=\"content\" role=\"main\"><div class=\"usermanual\">/g;p;}" {} \;
		COMMAND find ${CMAKE_CURRENT_BINARY_DIR}/wp_lua_api -name *.html -exec sed -n -i "1h;1!H;\${;g;s/<\\/body><\\/html>/<\\/div><\\/div><\\/div><?php get_footer(); ?>/g;p;}" {} \;
		COMMAND find ${CMAKE_CURRENT_BINARY_DIR}/wp_lua_api -name *.html -exec sed -n -i "1h;1!H;\${;g;s|\"\\([^\":]*\\.html\\)|\"\\1.php|g;p;}" {} \;
		COMMAND find ${CMAKE_CURRENT_BINARY_DIR}/wp_lua_api -name *.html -exec mv {} {}.php \;
		COMMAND cp -r ${CMAKE_CURRENT_BINARY_DIR}/media/images ${CMAKE_CURRENT_BINARY_DIR}/wp_lua_api/
		WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
		DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/lua/lua_api.xml
		COMMENT "Building php Lua API" VERBATIM
	)
        else (Saxon_FOUND)
            add_custom_command(
		OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/wp_lua_api/index.html.php
                COMMAND ${XSLT_XSLTPROC_EXECUTABLE} --stringparam chunker.output.encoding UTF-8 --stringparam base.dir ${CMAKE_CURRENT_BINARY_DIR}/wp_lua_api/ xsl/darktable_html.xsl ${CMAKE_CURRENT_SOURCE_DIR}/lua/lua_api.xml
		COMMAND find ${CMAKE_CURRENT_BINARY_DIR}/wp_lua_api -name *.html -exec sed -i s/${binary_path}\\/media\\///g {} \;
		COMMAND find ${CMAKE_CURRENT_BINARY_DIR}/wp_lua_api -name *.html -exec sed -i "s/<html>.*<title>/<?php define('WP_USE_THEMES', false);require('\\.\\.\\/wp-config\\.php'); add_filter('wp_title', 'set_title_page'); function set_title_page(){echo '/g" {} \;
		COMMAND find ${CMAKE_CURRENT_BINARY_DIR}/wp_lua_api -name *.html -exec sed -i "s/<\\/title>.*<\\/head>/ | user manual | ';}?>/g" {} \;

		COMMAND find ${CMAKE_CURRENT_BINARY_DIR}/wp_lua_api -name *.html -exec sed -i "s/<body[^>]*>/<?php get_header(); ?><div id=\"primary\"><div id=\"content\" role=\"main\"><div class=\"usermanual\">/g" {} \;
		COMMAND find ${CMAKE_CURRENT_BINARY_DIR}/wp_lua_api -name *.html -exec sed -i "s/<\\/body><\\/html>/<\\/div><\\/div><\\/div><?php get_footer(); ?>/g" {} \;
		COMMAND find ${CMAKE_CURRENT_BINARY_DIR}/wp_lua_api -name *.html -exec sed -i "s|\"\\([^\":]*\\.html\\)|\"\\1.php|g" {} \;
		COMMAND find ${CMAKE_CURRENT_BINARY_DIR}/wp_lua_api -name *.html -exec mv {} {}.php \;
		COMMAND cp -r ${CMAKE_CURRENT_BINARY_DIR}/media/images ${CMAKE_CURRENT_BINARY_DIR}/wp_lua_api/
		WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
		DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/lua/lua_api.xml
		COMMENT "Building php Lua API" VERBATIM
	)
        endif (Saxon_FOUND)
	add_custom_target(darktable-lua-api-wordpress DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/wp_lua_api/index.html.php )
	add_dependencies(darktable-lua-api-wordpress target_media_images)
    install(DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/wp_lua_api DESTINATION ${CMAKE_INSTALL_DOCDIR}/lua-api OPTIONAL)


	#
	# draft translated API
	#
	if(NOT ${Xml2po_BIN} STREQUAL "Xml2po_BIN-NOTFOUND")
		# localized draft usermanuals
		file(STRINGS "po/LINGUAS" LANGUAGES)
		foreach(language ${LANGUAGES})
			set(pofile "po/${language}.po")
			set(pdffile "${CMAKE_CURRENT_BINARY_DIR}/darktable-lua-api-${language}.pdf")
			set(fofile "${CMAKE_CURRENT_BINARY_DIR}/darktable-lua-api-${language}.fo")
			set(xmlfile "${CMAKE_CURRENT_SOURCE_DIR}/lua/lua_api.xml")
			set(xmllangfile "${CMAKE_CURRENT_BINARY_DIR}/darktable_lua_api-${language}.xml")
			set(xmllangtmp "${CMAKE_CURRENT_BINARY_DIR}/darktable_lua_api-${language}.tmp")
			add_custom_command(
				OUTPUT ${pdffile}
				COMMAND xml2po -e -p ${pofile} -l ${language} ${xmlfile} > ${xmllangtmp}
		                COMMAND cat ${xmllangtmp} | sed "s/fileref=\"\\([^>]*\\)\\(\\/images\\/\\)\\([^>]*\\)\\(.png\\)\"/fileref=\"${binary_path}\\/media\\/images\\/\\1\\/\\3.jpg\"/g ; s/fileref=\"images\\/\\([^>]*\\)\\(.png\\)\"/fileref=\"${binary_path}\\/media\\/images\\/\\1.jpg\"/g" > ${xmllangfile}
				COMMAND xsltproc --output ${fofile} xsl/darktable_fo.xsl ${xmllangfile}
				COMMAND fop -c fopconfig.xml ${fofile} -pdf ${pdffile}
				DEPENDS ${pofile} ${CMAKE_CURRENT_SOURCE_DIR}/lua/lua_api.xml
				WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
				COMMENT "Building ${language} API" VERBATIM
			)
			add_custom_target(darktable-lua-api-${language} DEPENDS ${pdffile})
			add_dependencies(lua-api darktable-lua-api-${language})
			add_dependencies(darktable-lua-api-${language} target_media_images darktable_profile_xml )
            install(FILES ${pdffile} DESTINATION ${CMAKE_INSTALL_DOCDIR}/lua-api OPTIONAL)
		endforeach(language)
	endif()
else(can_build_usermanual)
	message("Can NOT build Lua API documentation")
endif(can_build_usermanual)

add_subdirectory(po)


