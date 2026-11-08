# SPDX-License-Identifier: GPL-3.0-or-later
# Keep libxcf pinned at 64efecf63ddd952ccf654e479714aaae6f8c5986. Compile
# a generated copy with close/error cleanup fixed; the original encoder and
# pixel layout are unchanged. Apply this to host and Android builds alike.
function(dt_mobile_prepare_xcf_writer)
  get_target_property(xcf_source_dir xcf SOURCE_DIR)
  set(xcf_original "${xcf_source_dir}/xcf.c")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${xcf_original}")
  file(READ "${xcf_original}" xcf_source)
  set(close_before [=[int xcf_close(XCF *xcf)
{
  if(!xcf) return 1;

  if(xcf->state == XCF_STATE_ERROR)
  {
    PRINT_ERROR("error: the file is in error state. better add some error handling.");
    return 0;
  }

  int res = 1;]=])
  set(close_after [=[int xcf_close(XCF *xcf)
{
  if(!xcf) return 1;

  if(xcf->state == XCF_STATE_ERROR)
  {
    PRINT_ERROR("error: the file is in error state. better add some error handling.");
  }

  int res = 1;]=])
  set(flush_before "  if(xcf->fd) fclose(xcf->fd);")
  set(flush_after [=[  /* Release resources after earlier failures too, and report buffered IO errors. */
  if(xcf->fd)
  {
    if(ferror(xcf->fd)) res = 0;
    if(fclose(xcf->fd)) res = 0;
  }]=])
  foreach(fix close flush)
    string(FIND "${xcf_source}" "${${fix}_before}" context)
    if(context EQUAL -1)
      message(FATAL_ERROR "Pinned libxcf ${fix} context changed; review the XCF writer IO fix")
    endif()
    string(REPLACE "${${fix}_before}" "${${fix}_after}" xcf_source "${xcf_source}")
  endforeach()
  set(xcf_generated "${CMAKE_CURRENT_BINARY_DIR}/xcf-writer-io.c")
  # Avoid recompiling an unchanged generated source on every configure.
  set(xcf_previous "")
  if(EXISTS "${xcf_generated}")
    file(READ "${xcf_generated}" xcf_previous)
  endif()
  if(NOT xcf_previous STREQUAL xcf_source)
    file(WRITE "${xcf_generated}" "${xcf_source}")
  endif()
  get_target_property(xcf_sources xcf SOURCES)
  set(xcf_fixed_sources "")
  set(replaced FALSE)
  foreach(source IN LISTS xcf_sources)
    if(source STREQUAL "xcf.c" OR source STREQUAL "${xcf_original}")
      list(APPEND xcf_fixed_sources "${xcf_generated}")
      set(replaced TRUE)
    elseif(IS_ABSOLUTE "${source}")
      list(APPEND xcf_fixed_sources "${source}")
    else()
      list(APPEND xcf_fixed_sources "${xcf_source_dir}/${source}")
    endif()
  endforeach()
  if(NOT replaced)
    message(FATAL_ERROR "Pinned libxcf target no longer contains xcf.c")
  endif()
  set_property(TARGET xcf PROPERTY SOURCES "${xcf_fixed_sources}")
endfunction()
