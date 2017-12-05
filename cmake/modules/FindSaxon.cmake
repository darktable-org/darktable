# - Find Saxon XSLT processors.
#
# Saxon 6.5.[345] are supported.
#
# The following important variables are created:
# XSLT_SAXON_COMMAND
# XSLT_XSLTPROC_EXECUTABLE
# Xslt_SAXON_EXTENSIONS
# Saxon_FOUND
#
find_package (Java COMPONENTS Runtime)
if (JAVA_RUNTIME)
  if (NOT JAVA_CLASSPATH)
    set (JAVA_CLASSPATH $ENV{CLASSPATH} CACHE STRING "java classpath")
  endif (NOT JAVA_CLASSPATH)
  set (Xslt_CLASSPATH ${JAVA_CLASSPATH})

  find_file (JAVA_RESOLVER_LIBRARY
    NAMES resolver.jar xml-commons-resolver-1.1.jar xml-commons-resolver.jar
    PATH_SUFFIXES share/java
    DOC "location of the XML commons resolver java library from the apache project"
    CMAKE_FIND_ROOT_PATH_BOTH
  )
  mark_as_advanced (JAVA_RESOLVER_LIBRARY)
  if (JAVA_RESOLVER_LIBRARY)
    if (Xslt_CLASSPATH)
      set (Xslt_CLASSPATH "${Xslt_CLASSPATH}:${JAVA_RESOLVER_LIBRARY}")
    else (Xslt_CLASSPATH)
      set (Xslt_CLASSPATH "${JAVA_RESOLVER_LIBRARY}")
    endif (Xslt_CLASSPATH)
  endif (JAVA_RESOLVER_LIBRARY)

  find_path (JAVA_PROPERTIES_CATALOGMANAGER
    NAMES CatalogManager.properties
    PATHS /etc
    PATH_SUFFIXES xml/resolver share/java share/xml
    DOC "location of the catalog manager properties file from the XML commons resolver"
    CMAKE_FIND_ROOT_PATH_BOTH
  )
  mark_as_advanced (JAVA_PROPERTIES_CATALOGMANAGER)
  if (JAVA_PROPERTIES_CATALOGMANAGER)
    if (Xslt_CLASSPATH)
      set (Xslt_CLASSPATH "${Xslt_CLASSPATH}:${JAVA_PROPERTIES_CATALOGMANAGER}")
    else (Xslt_CLASSPATH)
      set (Xslt_CLASSPATH "${JAVA_PROPERTIES_CATALOGMANAGER}")
    endif (Xslt_CLASSPATH)
  endif (JAVA_PROPERTIES_CATALOGMANAGER)

  #
  # Find Saxon 6.5.x
  #
  find_file (SAXON
    NAMES saxon.jar saxon6.jar saxon-6.5.5.jar saxon-6.5.4.jar saxon-6.5.3.jar saxon9he.jar
    PATH_SUFFIXES share/java
                  share/java/saxon
                  share/java/saxon6
                  share/saxon-6.5/lib
                  java/saxon
    HINTS ENV SAXON_INSTALL_DIR
    DOC "location of saxon 6.5.x JAR file"
    CMAKE_FIND_ROOT_PATH_BOTH
  )
  mark_as_advanced (SAXON)

  find_file (JAVA_DOCBOOK_XSL_SAXON_LIBRARY
    NAMES docbook-xsl-saxon.jar saxon.jar saxon65.jar saxon653.jar saxon654.jar saxon655.jar
    PATH_SUFFIXES share/xml/docbook/stylesheet/nwalsh/current/extensions
                  share/xml/docbook-xsl/extensions
                  share/xml/docbook/xsl/extensions
                  share/java
                  share/java/saxon
                  share/java/docbook-xsl-saxon
                  share/saxon-6.5/lib
                  java/docbook-xsl-saxon
    HINTS ENV SAXON_INSTALL_DIR
    DOC "location of saxon 6.5.x DocBook XSL extension JAR file"
    CMAKE_FIND_ROOT_PATH_BOTH
  )
  mark_as_advanced (JAVA_DOCBOOK_XSL_SAXON_LIBRARY)
  set (Xslt_SAXON_EXTENSIONS "${JAVA_DOCBOOK_XSL_SAXON_LIBRARY}")


  if (SAXON)
    set (Xslt_SAXON_CLASSPATH "${Xslt_CLASSPATH}:${SAXON}")
    if (Xslt_SAXON_EXTENSIONS)
      set (Xslt_SAXON_CLASSPATH "${Xslt_SAXON_CLASSPATH}:${Xslt_SAXON_EXTENSIONS}")
    endif (Xslt_SAXON_EXTENSIONS)
    set ( XSLT_SAXON_COMMAND
      ${JAVA_RUNTIME} -cp "${Xslt_SAXON_CLASSPATH}" com.icl.saxon.StyleSheet
    )
    if (JAVA_RESOLVER_LIBRARY)
      list ( APPEND XSLT_SAXON_COMMAND
	-x org.apache.xml.resolver.tools.ResolvingXMLReader
	-y org.apache.xml.resolver.tools.ResolvingXMLReader
	-u
      )
      if (JAVA_PROPERTIES_CATALOGMANAGER)
	list ( APPEND XSLT_SAXON_COMMAND
	  -r org.apache.xml.resolver.tools.CatalogResolver
	)
      endif (JAVA_PROPERTIES_CATALOGMANAGER)
    endif (JAVA_RESOLVER_LIBRARY)
  endif (SAXON)
endif (JAVA_RUNTIME)


if (XSLT_SAXON_COMMAND AND JAVA_DOCBOOK_XSL_SAXON_LIBRARY)
  set (Saxon_FOUND true)
endif (XSLT_SAXON_COMMAND AND JAVA_DOCBOOK_XSL_SAXON_LIBRARY)

if (NOT Saxon_FOUND)
  if (NOT Saxon_FIND_QUIETLY)
    message (STATUS "No saxon XSLT processor and/or no docbook saxon extension library found.")
  endif (NOT Saxon_FIND_QUIETLY)
  if (Saxon_FIND_REQUIRED)
    message (FATAL_ERROR "No saxon XSLT processor and/or no docbook saxon extension library found but it is required.")
  endif (Saxon_FIND_REQUIRED)
endif (NOT Saxon_FOUND)
