/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

// double-click on a gradient that is still being created, see #20759
//
// with continuous creation the first click's release saves the form and
// installs a fresh, empty one as the gui form, so the second click of a
// double-click arrives while creating, with no point to reset curvature
// on. it must be treated like an ordinary creation click, as circle and
// ellipse treat it (they never special-case GDK_2BUTTON_PRESS): the old
// code read form->points->data on a form whose points list is NULL
//
// a panel action can also leave an empty form behind with creation off:
// there a double-click must not deref the points list either
//
// no darktable init is needed: the exercised path touches only form and gui

#include "develop/masks.h"

#include <gdk/gdk.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>
#include <cmocka.h>

static void _setup_creation_form(dt_masks_form_t *form,
                                 dt_masks_form_gui_t *gui)
{
  memset(form, 0, sizeof(*form));
  form->type = DT_MASKS_GRADIENT;
  form->points = NULL;
  form->functions = &dt_masks_functions_gradient;

  memset(gui, 0, sizeof(*gui));
  gui->creation = TRUE;
  gui->posx = 10.f;
  gui->posy = 20.f;
}

static void _assert_started_drag(dt_masks_form_t *form,
                                 dt_masks_form_gui_t *gui,
                                 const int result)
{
  assert_int_equal(result, 0);
  // the form stays untouched: its point is placed on button release
  assert_null(form->points);
  // the creation branch started the drag from the click position
  assert_true(gui->form_dragging);
  assert_float_equal(gui->posx_source, 10.f, 0.f);
  assert_float_equal(gui->posy_source, 20.f, 0.f);
}

static void test_doubleclick_during_creation_does_not_crash(void **state)
{
  dt_masks_form_t form;
  dt_masks_form_gui_t gui;
  _setup_creation_form(&form, &gui);

  const int result = dt_masks_functions_gradient.button_pressed(NULL, 0.5f, 0.5f, 1.0,
                                                                GDK_BUTTON_PRIMARY,
                                                                GDK_2BUTTON_PRESS, 0,
                                                                &form, 0, &gui, 0);
  _assert_started_drag(&form, &gui, result);
}

static void test_singleclick_during_creation_starts_drag(void **state)
{
  dt_masks_form_t form;
  dt_masks_form_gui_t gui;
  _setup_creation_form(&form, &gui);

  const int result = dt_masks_functions_gradient.button_pressed(NULL, 0.5f, 0.5f, 1.0,
                                                                GDK_BUTTON_PRIMARY,
                                                                GDK_BUTTON_PRESS, 0,
                                                                &form, 0, &gui, 0);
  _assert_started_drag(&form, &gui, result);
}

static void test_doubleclick_on_empty_form_not_creating(void **state)
{
  dt_masks_form_t form;
  dt_masks_form_gui_t gui;
  _setup_creation_form(&form, &gui);
  // a panel action cleared creation without replacing the empty form
  gui.creation = FALSE;

  const int result = dt_masks_functions_gradient.button_pressed(NULL, 0.5f, 0.5f, 1.0,
                                                                GDK_BUTTON_PRIMARY,
                                                                GDK_2BUTTON_PRESS, 0,
                                                                &form, 0, &gui, 0);
  assert_int_equal(result, 0);
  assert_null(form.points);
  assert_false(gui.form_dragging);
}

int main(int argc, char *argv[])
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_doubleclick_during_creation_does_not_crash),
    cmocka_unit_test(test_singleclick_during_creation_starts_drag),
    cmocka_unit_test(test_doubleclick_on_empty_form_not_creating)
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
