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
/*
 * cmocka unit tests for what the layered presenter replays above a
 * presenting widget (gui/presenter_layers.c, _above()): the children each
 * GtkOverlay on the way up stacks above it, bottom to top, and with
 * popovers the toplevel's popovers after them. presenter_layers.c is
 * included for its static helpers. GTK3 cannot create a widget without a
 * display, so the suite is skipped (exit 77) where gtk_init_check() fails,
 * as in a headless ctest run
 */

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include <cmocka.h>

#include "gui/presenter_layers.c"

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

#define SKIP_RC 77

typedef struct _tree_t
{
  GtkWidget *window, *outer, *inner, *box, *presenting, *beside;
  GtkWidget *over1, *over2, *over3;
  GtkWidget *pop1, *pop2;
} _tree_t;

// window > outer overlay [ box [ inner overlay [ presenting, over1, over2 ],
// beside ], over3 ], and two popovers
static int _setup(void **state)
{
  _tree_t *t = g_new0(_tree_t, 1);
  t->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  t->outer = gtk_overlay_new();
  t->inner = gtk_overlay_new();
  t->box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  t->presenting = gtk_drawing_area_new();
  t->beside = gtk_label_new("beside");
  t->over1 = gtk_label_new("over1");
  t->over2 = gtk_label_new("over2");
  t->over3 = gtk_label_new("over3");
  gtk_container_add(GTK_CONTAINER(t->window), t->outer);
  gtk_container_add(GTK_CONTAINER(t->outer), t->box);
  gtk_overlay_add_overlay(GTK_OVERLAY(t->outer), t->over3);
  gtk_box_pack_start(GTK_BOX(t->box), t->inner, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(t->box), t->beside, FALSE, FALSE, 0);
  gtk_container_add(GTK_CONTAINER(t->inner), t->presenting);
  gtk_overlay_add_overlay(GTK_OVERLAY(t->inner), t->over1);
  gtk_overlay_add_overlay(GTK_OVERLAY(t->inner), t->over2);
  t->pop1 = gtk_popover_new(t->presenting);
  t->pop2 = gtk_popover_new(t->beside);
  *state = t;
  return 0;
}

static int _teardown(void **state)
{
  _tree_t *t = *state;
  gtk_widget_destroy(t->pop1);
  gtk_widget_destroy(t->pop2);
  gtk_widget_destroy(t->window);
  g_free(t);
  return 0;
}

static guint _index(GPtrArray *a,
                    const GtkWidget *widget)
{
  guint k;
  return g_ptr_array_find(a, widget, &k) ? k : G_MAXUINT;
}

static void test_overlays_bottom_to_top(void **state)
{
  _tree_t *t = *state;
  GPtrArray *a = g_ptr_array_new();
  _above(t->presenting, FALSE, a);
  assert_int_equal(a->len, 3);
  assert_ptr_equal(g_ptr_array_index(a, 0), t->over1);
  assert_ptr_equal(g_ptr_array_index(a, 1), t->over2);
  assert_ptr_equal(g_ptr_array_index(a, 2), t->over3);
  g_ptr_array_free(a, TRUE);
}

static void test_only_what_is_above(void **state)
{
  _tree_t *t = *state;
  GPtrArray *a = g_ptr_array_new();
  // an overlay child: the main child and the children before it are below
  _above(t->over1, FALSE, a);
  assert_int_equal(a->len, 2);
  assert_ptr_equal(g_ptr_array_index(a, 0), t->over2);
  assert_ptr_equal(g_ptr_array_index(a, 1), t->over3);
  // a box puts its children beside each other, not above
  assert_int_equal(_index(a, t->beside), G_MAXUINT);
  g_ptr_array_free(a, TRUE);
}

static void test_popovers_last(void **state)
{
  _tree_t *t = *state;
  GPtrArray *a = g_ptr_array_new();
  _above(t->presenting, TRUE, a);
  assert_int_equal(a->len, 5);
  assert_ptr_equal(g_ptr_array_index(a, 2), t->over3);
  // in the order GtkWindow stacks them, whatever widget they point at
  GPtrArray *listed = g_ptr_array_new();
  _gather_t g = { .popovers = TRUE, .widgets = listed };
  gtk_container_forall(GTK_CONTAINER(t->window), _gather, &g);
  assert_int_equal(listed->len, 2);
  assert_ptr_equal(g_ptr_array_index(a, 3), g_ptr_array_index(listed, 0));
  assert_ptr_equal(g_ptr_array_index(a, 4), g_ptr_array_index(listed, 1));
  assert_int_not_equal(_index(a, t->pop1), G_MAXUINT);
  assert_int_not_equal(_index(a, t->pop2), G_MAXUINT);
  g_ptr_array_free(listed, TRUE);
  g_ptr_array_free(a, TRUE);
}

static void test_no_popovers_unless_asked(void **state)
{
  _tree_t *t = *state;
  GPtrArray *a = g_ptr_array_new();
  _above(t->presenting, FALSE, a);
  assert_int_equal(_index(a, t->pop1), G_MAXUINT);
  assert_int_equal(_index(a, t->pop2), G_MAXUINT);
  g_ptr_array_free(a, TRUE);
}

int main(int argc, char *argv[])
{
  if(!gtk_init_check(&argc, &argv))
  {
    fprintf(stderr, "no display: GTK3 cannot create widgets, skipped\n");
    return SKIP_RC;
  }

  const struct CMUnitTest tests[] = {
    cmocka_unit_test_setup_teardown(test_overlays_bottom_to_top, _setup, _teardown),
    cmocka_unit_test_setup_teardown(test_only_what_is_above, _setup, _teardown),
    cmocka_unit_test_setup_teardown(test_popovers_last, _setup, _teardown),
    cmocka_unit_test_setup_teardown(test_no_popovers_unless_asked, _setup, _teardown),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
