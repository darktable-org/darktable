# - Find XSLT processors.
#
# xsltproc and Saxon 6.5.[345] are supported.
#
# The following important variables are created:
# XSLT_SAXON_COMMAND
# XSLT_XSLTPROC_EXECUTABLE
# Xslt_SAXON_EXTENSIONS
# Xslt_FOUND
#
find_package (Java)
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
    NAMES saxon.jar saxon-6.5.5.jar saxon-6.5.4.jar saxon-6.5.3.jar
    PATH_SUFFIXES share/java
    DOC "location of saxon 6.5.x JAR file"
    CMAKE_FIND_ROOT_PATH_BOTH
  )
  mark_as_advanced (SAXON)

  find_file (JAVA_DOCBOOK_XSL_SAXON_LIBRARY
    NAMES saxon65.jar saxon653.jar saxon654.jar saxon655.jar
    PATH_SUFFIXES share/xml/docbook/stylesheet/nwalsh/current/extensions
    PATH_SUFFIXES share/xml/docbook-xsl/extensions
    PATH_SUFFIXES share/xml/docbook/xsl/extensions
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

find_program (XSLT_XSLTPROC_EXECUTABLE
  NAMES xsltproc
  DOC   "path to the libxslt XSLT processor xsltproc"
)
mark_as_advanced (XSLT_XSLTPROC_EXECUTABLE)


if (XSLT_XSLTPROC_EXECUTABLE OR XSLT_SAXON_COMMAND)
  set (Xslt_FOUND true)
endif (XSLT_XSLTPROC_EXECUTABLE OR XSLT_SAXON_COMMAND)

if (NOT Xslt_FOUND)
  if (NOT Xslt_FIND_QUIETLY)
    message (STATUS "No supported XSLT processor found. Supported XSLT processors are: xsltproc and saxon-6.5.x")
  endif (NOT Xslt_FIND_QUIETLY)
  if (Xslt_FIND_REQUIRED)
    message (FATAL_ERROR "No supported XSLT processor found but it is required.")
  endif (Xslt_FIND_REQUIRED)
endif (NOT Xslt_FOUND)
