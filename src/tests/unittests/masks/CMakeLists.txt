# Behavioural tests for the flexi masks panel's model layer. These link
# lib_darktable and call into blend_gui.c's exported model/gesture functions
# (see develop/blend_gui_internal.h); they never initialise GTK, so they run
# headless in CI.
add_cmocka_test(test_flexi_model
                SOURCES test_flexi_model.c flexi_fixture.c
                LINK_LIBRARIES lib_darktable cmocka)

# Cache-invalidation: which edits must (and must not) move the mask hash the
# pixelpipe keys its cached masks on.
add_cmocka_test(test_flexi_cache
                SOURCES test_flexi_cache.c flexi_fixture.c
                LINK_LIBRARIES lib_darktable cmocka)

# Mask blob version migration: carrying already-saved edits forward.
add_cmocka_test(test_flexi_persistence
                SOURCES test_flexi_persistence.c flexi_fixture.c
                LINK_LIBRARIES lib_darktable cmocka)

# The drag-and-drop paths other than element-onto-element.
add_cmocka_test(test_flexi_dnd
                SOURCES test_flexi_dnd.c flexi_fixture.c
                LINK_LIBRARIES lib_darktable cmocka)

# Group identity and isolation state: solo family, numbering, refinement scope.
add_cmocka_test(test_flexi_groups
                SOURCES test_flexi_groups.c flexi_fixture.c
                LINK_LIBRARIES lib_darktable cmocka)

# What the panel decides to show: badges, adaptive parametric rows, prefs.
add_cmocka_test(test_flexi_panel
                SOURCES test_flexi_panel.c flexi_fixture.c
                LINK_LIBRARIES lib_darktable cmocka)

# The classic -> flexi migration case table (structure, not pixels).
add_cmocka_test(test_flexi_migrate
                SOURCES test_flexi_migrate.c flexi_fixture.c
                LINK_LIBRARIES lib_darktable cmocka)

# The mask operators themselves: what union/intersection/... actually compute.
add_cmocka_test(test_flexi_compose
                SOURCES test_flexi_compose.c flexi_fixture.c
                LINK_LIBRARIES lib_darktable cmocka)

# The panel's own controls: precise-entry popup placement and the value
# conversions its numbers go through.
add_cmocka_test(test_flexi_controls
                SOURCES test_flexi_controls.c flexi_fixture.c
                LINK_LIBRARIES lib_darktable cmocka)

# Containment, not behaviour: is every mask state the panel can reach also one
# the harvest checks sweep? See the file header for why this is not a GUI test.
add_cmocka_test(test_flexi_ui_coverage
                SOURCES test_flexi_ui_coverage.c flexi_fixture.c
                LINK_LIBRARIES lib_darktable cmocka)

# Which raster-mask consumers the pipe considers live: the one mask decision
# made across modules, and the only one whose inputs differ between the
# darkroom and export pipes. See the file header for the two ways it fails.
add_cmocka_test(test_flexi_raster_prune
                SOURCES test_flexi_raster_prune.c flexi_fixture.c
                LINK_LIBRARIES lib_darktable cmocka)

# Is the synthetic probe image actually adequate to replay harvested masks
# against? Measured per channel, not assumed -- see the file header.
add_cmocka_test(test_probe_image
                SOURCES test_probe_image.c
                LINK_LIBRARIES lib_darktable cmocka)

# The styling contract (dev-doc/flexi_masks/styling.md): the classes a theme
# or the CSS tweaks can target. Reads the sources as text, no GTK.
add_cmocka_test(test_flexi_styling
                SOURCES test_flexi_styling.c
                LINK_LIBRARIES lib_darktable cmocka)
target_compile_definitions(test_flexi_styling PRIVATE
                           DT_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

foreach(_t test_flexi_model test_flexi_cache test_flexi_persistence test_flexi_dnd
           test_flexi_groups test_flexi_panel test_flexi_migrate test_flexi_compose
           test_flexi_controls test_flexi_ui_coverage test_flexi_raster_prune
           test_flexi_styling test_probe_image)
  target_include_directories(${_t} PRIVATE
                             ${CMAKE_CURRENT_SOURCE_DIR}
                             ${CMAKE_SOURCE_DIR}/src)
  if(WIN32)
      _copy_required_library(${_t} lib_darktable)
  endif(WIN32)
endforeach()
