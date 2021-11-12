# This file defines the Feature Logging macros.
#
# MACRO_LOG_FEATURE(VAR FEATURE DESCRIPTION URL [REQUIRED [MIN_VERSION [COMMENTS]]])
#   Logs the information so that it can be displayed at the end
#   of the configure run
#   VAR : TRUE or FALSE, indicating whether the feature is supported
#   FEATURE: name of the feature, e.g. "libjpeg"
#   DESCRIPTION: description what this feature provides
#   URL: home page
#   REQUIRED: TRUE or FALSE, indicating whether the feature is required
#   MIN_VERSION: minimum version number. empty string if unneeded
#   COMMENTS: More info you may want to provide.  empty string if unnecessary
#
# MACRO_DISPLAY_FEATURE_LOG()
#   Call this to display the collected results.
#   Exits CMake with a FATAL error message if a required feature is missing
#
# Example:
#
# INCLUDE(MacroLogFeature)
#
# FIND_PACKAGE(JPEG)
# MACRO_LOG_FEATURE(JPEG_FOUND "libjpeg" "Support JPEG images" "http://www.ijg.org" TRUE "3.2a" "")
# ...
# MACRO_DISPLAY_FEATURE_LOG()

# Copyright (c) 2006, Alexander Neundorf, <neundorf@kde.org>
# Copyright (c) 2006, Allen Winter, <winter@kde.org>
# Copyright (c) 2009, Sebastian Trueg, <trueg@kde.org>
#
# Redistribution and use is allowed according to the terms of the BSD license.
# For details see the accompanying LICENSE file.

IF (NOT _macroLogFeatureAlreadyIncluded)
   SET(_file ${CMAKE_BINARY_DIR}/MissingRequirements.txt)
   IF (EXISTS ${_file})
      FILE(REMOVE ${_file})
   ENDIF (EXISTS ${_file})

   SET(_file ${CMAKE_BINARY_DIR}/EnabledFeatures.txt)
   IF (EXISTS ${_file})
      FILE(REMOVE ${_file})
   ENDIF (EXISTS ${_file})

   SET(_file ${CMAKE_BINARY_DIR}/DisabledFeatures.txt)
   IF (EXISTS ${_file})
      FILE(REMOVE ${_file})
  ENDIF (EXISTS ${_file})

  SET(_macroLogFeatureAlreadyIncluded TRUE)

  INCLUDE(FeatureSummary)

ENDIF (NOT _macroLogFeatureAlreadyIncluded)


MACRO(MACRO_LOG_FEATURE _var _package _description _url ) # _required _minvers _comments)

   STRING(TOUPPER "${ARGV4}" _required)
   SET(_minvers "${ARGV5}")
   SET(_comments "${ARGV6}")

   IF (${_var})
     SET(_LOGFILENAME ${CMAKE_BINARY_DIR}/EnabledFeatures.txt)
   ELSE (${_var})
     IF ("${_required}" STREQUAL "TRUE")
       SET(_LOGFILENAME ${CMAKE_BINARY_DIR}/MissingRequirements.txt)
     ELSE ("${_required}" STREQUAL "TRUE")
       SET(_LOGFILENAME ${CMAKE_BINARY_DIR}/DisabledFeatures.txt)
     ENDIF ("${_required}" STREQUAL "TRUE")
   ENDIF (${_var})

   SET(_logtext "   * ${_package}")

   IF (NOT ${_var})
      IF (${_minvers} MATCHES ".*")
        SET(_logtext "${_logtext} (${_minvers} or higher)")
      ENDIF (${_minvers} MATCHES ".*")
      SET(_logtext "${_logtext}  <${_url}>\n     ")
   ELSE (NOT ${_var})
     SET(_logtext "${_logtext} - ")
   ENDIF (NOT ${_var})

   SET(_logtext "${_logtext}${_description}")

   IF (NOT ${_var})
      IF (${_comments} MATCHES ".*")
        SET(_logtext "${_logtext}\n     ${_comments}")
      ENDIF (${_comments} MATCHES ".*")
#      SET(_logtext "${_logtext}\n") #double-space missing features?
   ENDIF (NOT ${_var})

   FILE(APPEND "${_LOGFILENAME}" "${_logtext}\n")

   IF(COMMAND SET_PACKAGE_PROPERTIES)
     SET_PACKAGE_PROPERTIES("${_package}" PROPERTIES URL "${_url}" DESCRIPTION "\"${_description}\"")
   ELSEIF(COMMAND SET_PACKAGE_INFO)  # in FeatureSummary.cmake since CMake 2.8.3
     SET_PACKAGE_INFO("${_package}" "\"${_description}\"" "${_url}" "\"${_comments}\"")
   ENDIF(COMMAND SET_PACKAGE_PROPERTIES)

ENDMACRO(MACRO_LOG_FEATURE)


MACRO(MACRO_DISPLAY_FEATURE_LOG)
   IF(COMMAND FEATURE_SUMMARY) # in FeatureSummary.cmake since CMake 2.8.3
      FEATURE_SUMMARY(FILENAME ${CMAKE_CURRENT_BINARY_DIR}/FindPackageLog.txt
                      WHAT ALL)
   ENDIF(COMMAND FEATURE_SUMMARY)

   SET(_missingFile ${CMAKE_BINARY_DIR}/MissingRequirements.txt)
   SET(_enabledFile ${CMAKE_BINARY_DIR}/EnabledFeatures.txt)
   SET(_disabledFile ${CMAKE_BINARY_DIR}/DisabledFeatures.txt)

   IF (EXISTS ${_missingFile} OR EXISTS ${_enabledFile} OR EXISTS ${_disabledFile})
     SET(_printSummary TRUE)
   ENDIF (EXISTS ${_missingFile} OR EXISTS ${_enabledFile} OR EXISTS ${_disabledFile})

   IF(_printSummary)
     SET(_missingDeps 0)
     IF (EXISTS ${_enabledFile})
       FILE(READ ${_enabledFile} _enabled)
       FILE(REMOVE ${_enabledFile})
       SET(_summary "${_summary}\n-----------------------------------------------------------------------------\n-- The following external packages were located on your system.\n-- This installation will have the extra features provided by these packages.\n-----------------------------------------------------------------------------\n${_enabled}")
     ENDIF (EXISTS ${_enabledFile})


     IF (EXISTS ${_disabledFile})
       SET(_missingDeps 1)
       FILE(READ ${_disabledFile} _disabled)
       FILE(REMOVE ${_disabledFile})
       SET(_summary "${_summary}\n-----------------------------------------------------------------------------\n-- The following OPTIONAL packages could NOT be located on your system.\n-- Consider installing them to enable more features from this software.\n-----------------------------------------------------------------------------\n${_disabled}")
     ENDIF (EXISTS ${_disabledFile})


     IF (EXISTS ${_missingFile})
       SET(_missingDeps 1)
       FILE(READ ${_missingFile} _requirements)
       SET(_summary "${_summary}\n-----------------------------------------------------------------------------\n-- The following REQUIRED packages could NOT be located on your system.\n-- You must install these packages before continuing.\n-----------------------------------------------------------------------------\n${_requirements}")
       FILE(REMOVE ${_missingFile})
       SET(_haveMissingReq 1)
     ENDIF (EXISTS ${_missingFile})


     IF (NOT ${_missingDeps})
       SET(_summary "${_summary}\n-----------------------------------------------------------------------------\n-- Congratulations! All external packages have been found.")
     ENDIF (NOT ${_missingDeps})


     MESSAGE(${_summary})
     MESSAGE("-----------------------------------------------------------------------------\n")


     IF(_haveMissingReq)
       MESSAGE(FATAL_ERROR "Exiting: Missing Requirements")
     ENDIF(_haveMissingReq)

   ENDIF(_printSummary)

ENDMACRO(MACRO_DISPLAY_FEATURE_LOG)
