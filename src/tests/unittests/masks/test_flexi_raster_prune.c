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

// Which raster-mask consumers does the pipe consider live?
//
// A module that publishes a raster mask keeps a table of the modules consuming
// it. dt_dev_pixelpipe_prune_stale_raster_users() decides, once per pipe run,
// which of those entries are real. The consequences run both ways and neither
// is cosmetic:
//
//   - keep a phantom, and the source republishes and invalidates every
//     downstream cache line forever
//   - drop a live one, and the source stops storing its mask, so the consumer
//     silently blends with nothing
//
// This is the only mask decision in the tree that is made ACROSS modules, and
// the only one whose inputs differ between the darkroom pipe and the export
// pipe. Both make it hard to test any other way, and it can go wrong two
// ways:
//
//   - judging consumers from module->enabled, which tracks the darkroom GUI
//     and is unmaintained in the export pipe, drops a live export consumer's
//     mask (integration test 0167-raster-mask). piece->enabled is the
//     authority, and only in this pipe.
//   - knowing only the exclusive raster sink (blend_params.mask_mode &
//     DEVELOP_MASK_RASTER, raster_mask.sink.source) prunes a live flexi
//     consumer: a DT_MASKS_RASTER form element is a group member, mask_mode
//     is MASK/FLEXI and sink.source is unset.
//
// Two shapes of consumer times several ways to be stale is a matrix, and the
// cells are only interesting together: fixing one can break another. So it is
// swept as a matrix here rather than sampled.
//
// The pipe is built by hand rather than driven through synch_all(), which
// would mean standing up a history stack to reach a decision that reads none
// of it. Everything the function actually reads is real: the module and piece
// structs, the users table, the form tree behind dev->forms.

#include "flexi_fixture.h"

#include "develop/pixelpipe_hb.h"

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <cmocka.h>

// ---------------------------------------------------------------------------
// the bench
// ---------------------------------------------------------------------------

#define SOURCE_OP "exposure"
#define SINK_OP   "colorbalancergb"
#define RASTER_ID 7

typedef struct
{
  dt_develop_t dev;

  dt_iop_module_so_t source_so, sink_so;
  dt_iop_module_t source, sink;
  dt_dev_pixelpipe_t pipe;
  dt_dev_pixelpipe_iop_t source_piece, sink_piece;
  dt_develop_blend_params_t sink_bp;

  GList *forms;          // owned: the sink's mask group and its members
  dt_mask_id_t next_fid;
} bench_t;

static bench_t B;

/* The prune reads exactly five things: module->dev, the source's users table,
   the source's op and multi_priority (through dt_iop_module_is), the pieces in
   pipe->nodes with their ->enabled and ->blendop_data, and dev->forms. It does
   not read the sink's raster_mask.sink, which another pipe's history replay
   clears. Everything else on these structs stays
   zeroed on purpose -- a field this needed but did not set would show up as a
   crash, not as a quietly wrong answer. */
static void _bench_init(void)
{
  memset(&B, 0, sizeof(B));
  B.next_fid = 100;

  // dt_iop_module_so_t::op is a char array, and dt_iop_module_is() reads the
  // SO's copy rather than the module's
  g_strlcpy(B.source_so.op, SOURCE_OP, sizeof(B.source_so.op));
  g_strlcpy(B.sink_so.op, SINK_OP, sizeof(B.sink_so.op));

  B.source.so = &B.source_so;
  B.sink.so = &B.sink_so;
  g_strlcpy(B.source.op, SOURCE_OP, sizeof(B.source.op));
  g_strlcpy(B.sink.op, SINK_OP, sizeof(B.sink.op));
  B.source.dev = &B.dev;
  B.sink.dev = &B.dev;
  B.source.raster_mask.source.users = g_hash_table_new(NULL, NULL);
  // the prune holds this while it reads the table, see dt_iop_raster_users_lock()
  dt_pthread_recursive_mutex_init(&B.source.raster_mask.source.users_lock);

  /* Zeroed, then set per case. The prune reads only mask_mode, mask_id and the
     raster source (raster_mask_source/instance) of these params, so a real
     default set would only add
     noise, and leaving the rest at zero means a field it started reading would
     surface as a wrong answer here rather than being papered over. */
  memset(&B.sink_bp, 0, sizeof(B.sink_bp));
  B.sink.blend_params = &B.sink_bp;

  /* The darkroom's arrangement: module and piece agree, and the module's
     blend_params are the same object the piece points at. Every case starts
     from that, so a prune reading the module instead of the piece still gets
     the right answer -- and only the export case below, which desyncs them on
     purpose, tells the two apart. Starting from a desynced bench instead would
     make half the suite fail for the export bug and none of it point at the
     export bug. */
  B.source_piece.module = &B.source;
  B.source_piece.enabled = TRUE;
  B.source.enabled = TRUE;
  B.sink_piece.module = &B.sink;
  B.sink_piece.enabled = TRUE;
  B.sink.enabled = TRUE;
  B.sink_piece.blendop_data = &B.sink_bp;

  B.pipe.nodes = g_list_append(NULL, &B.source_piece);
  B.pipe.nodes = g_list_append(B.pipe.nodes, &B.sink_piece);
}

static void _bench_cleanup(void)
{
  g_list_free(B.pipe.nodes);
  B.pipe.nodes = NULL;
  if(B.source.raster_mask.source.users)
    g_hash_table_destroy(B.source.raster_mask.source.users);
  B.source.raster_mask.source.users = NULL;
  dt_pthread_mutex_destroy(&B.source.raster_mask.source.users_lock);
  g_list_free_full(B.dev.forms, (GDestroyNotify)dt_masks_free_form);
  B.dev.forms = NULL;
}

/** register the sink as a consumer, the way dt_iop_piece_set_raster does */
static void _register_user(void)
{
  g_hash_table_insert(B.source.raster_mask.source.users, &B.sink,
                      GINT_TO_POINTER(RASTER_ID));
}

static gboolean _still_a_user(void)
{
  return g_hash_table_contains(B.source.raster_mask.source.users, &B.sink);
}

static void _prune(void)
{
  dt_dev_pixelpipe_prune_stale_raster_users(&B.pipe, &B.source);
}

// --- the two consumer shapes ------------------------------------------------

/** the classic exclusive sink: the whole module blends through the raster mask */
static void _make_classic_sink(void)
{
  B.sink_bp.mask_mode = DEVELOP_MASK_ENABLED | DEVELOP_MASK_RASTER;
  B.sink_bp.mask_id = NO_MASKID;
  // the piece's params name the source, as dt_iop_commit_blend_params reads them
  g_strlcpy(B.sink_bp.raster_mask_source, SOURCE_OP, sizeof(B.sink_bp.raster_mask_source));
  B.sink_bp.raster_mask_instance = 0;
  B.sink_bp.raster_mask_id = RASTER_ID;
  B.sink.raster_mask.sink.source = &B.source;
  B.sink.raster_mask.sink.id = RASTER_ID;
}

/** the flexi consumer: a DT_MASKS_RASTER element inside the sink's mask group.
    Note what is NOT set -- mask_mode carries no DEVELOP_MASK_RASTER and
    sink.source stays NULL, which is precisely why the legacy test is blind to
    it. `instance` names which source instance the element points at. */
static void _make_flexi_sink(const int instance)
{
  dt_masks_form_t *elem = dt_masks_create(DT_MASKS_RASTER);
  elem->formid = B.next_fid++;
  dt_masks_point_raster_t *rp = malloc(sizeof(dt_masks_point_raster_t));
  memset(rp, 0, sizeof(*rp));
  g_strlcpy(rp->source, SOURCE_OP, sizeof(rp->source));
  rp->instance = instance;
  rp->id = RASTER_ID;
  elem->points = g_list_append(NULL, rp);

  dt_masks_form_t *grp = dt_masks_create(DT_MASKS_GROUP);
  grp->formid = B.next_fid++;
  dt_masks_point_group_t *pt = malloc(sizeof(dt_masks_point_group_t));
  memset(pt, 0, sizeof(*pt));
  pt->formid = elem->formid;
  pt->parentid = grp->formid;
  pt->state = DT_MASKS_STATE_USE | DT_MASKS_STATE_SHOW;
  pt->opacity = 1.0f;
  pt->group_opacity = 1.0f;
  grp->points = g_list_append(NULL, dt_masks_marker_new(B.dev.forms, grp, 0));
  grp->points = g_list_append(grp->points, pt);

  B.dev.forms = g_list_append(B.dev.forms, elem);
  B.dev.forms = g_list_append(B.dev.forms, grp);

  B.sink_bp.mask_mode = DEVELOP_MASK_ENABLED | DEVELOP_MASK_MASK | DEVELOP_MASK_FLEXI;
  B.sink_bp.mask_id = grp->formid;
  B.sink.raster_mask.sink.source = NULL;
}

/** drop the raster element from the flexi sink's group, leaving the group (and
    the module's mask mode) otherwise intact -- what removing the row does */
static void _drop_flexi_element(void)
{
  dt_masks_form_t *grp = dt_masks_get_from_id_ext(B.dev.forms, B.sink_bp.mask_id);
  assert_non_null(grp);
  g_list_free_full(grp->points, free);
  grp->points = NULL;
}

// ---------------------------------------------------------------------------
// the matrix
// ---------------------------------------------------------------------------

/* Both consumer shapes, fully wired and enabled, are kept. The floor: if these
   fail, every other cell below is measuring nothing. */
static void test_a_live_consumer_survives(void **state)
{
  _bench_init();
  _make_classic_sink();
  _register_user();
  _prune();
  assert_true(_still_a_user());
  _bench_cleanup();

  _bench_init();
  _make_flexi_sink(0);
  _register_user();
  _prune();
  assert_true(_still_a_user());
  _bench_cleanup();
}

/* A consumer with no node in THIS pipe is gone: the module may have been
   deleted, or may simply not be in this pipe. Only the pointer is compared,
   so a freed module is never dereferenced -- which is the whole reason the
   test is by pointer and not by op. */
static void test_a_consumer_absent_from_the_pipe_is_dropped(void **state)
{
  _bench_init();
  _make_classic_sink();
  _register_user();

  B.pipe.nodes = g_list_remove(B.pipe.nodes, &B.sink_piece);
  _prune();
  assert_false(_still_a_user());
  _bench_cleanup();
}

/* Disabled in this pipe, in both consumer shapes. The export pipe legitimately
   carries nodes for modules the darkroom has switched off. */
static void test_a_disabled_piece_is_dropped(void **state)
{
  _bench_init();
  _make_classic_sink();
  _register_user();
  B.sink_piece.enabled = FALSE;   // the module stays enabled: the piece decides
  _prune();
  assert_false(_still_a_user());
  _bench_cleanup();

  _bench_init();
  _make_flexi_sink(0);
  _register_user();
  B.sink_piece.enabled = FALSE;
  _prune();
  assert_false(_still_a_user());
  _bench_cleanup();
}

/* THE EXPORT CELL.

   In the export pipe module->enabled and module->blend_params track the
   darkroom UI and are stale by construction; piece->enabled and
   piece->blendop_data are the authority. A prune that consulted the module
   would drop this consumer, and only on export -- the darkroom would look
   perfectly fine.

   Both shapes, because the flexi path resolves its forms through dev->forms
   (also per-dev and correct in the export pipe) while the classic path reads
   the piece's blendop_data, so they fail differently. */
static void test_module_state_does_not_override_the_piece(void **state)
{
  // what the module carries in an export pipe: whatever the darkroom left
  // there, which is not this consumer's current state
  dt_develop_blend_params_t stale;
  memset(&stale, 0, sizeof(stale));

  _bench_init();
  _make_classic_sink();
  _register_user();
  // what an export pipe looks like: the piece says yes, the module says no.
  // Another pipe's history replay also clears the module's raster sink
  B.sink.enabled = FALSE;
  B.sink.blend_params = &stale;
  B.sink.raster_mask.sink.source = NULL;
  _prune();
  assert_true(_still_a_user());
  _bench_cleanup();

  _bench_init();
  _make_flexi_sink(0);
  _register_user();
  B.sink.enabled = FALSE;
  B.sink.blend_params = &stale;
  _prune();
  assert_true(_still_a_user());
  _bench_cleanup();
}

/* De-synced: the module is in the table but no longer points back at this
   source. The classic sink switched to another source or to a drawn mask; the
   flexi group had its raster row deleted. Both are phantoms. */
static void test_a_desynced_consumer_is_dropped(void **state)
{
  _bench_init();
  _make_classic_sink();
  _register_user();
  // switched its mask away from raster, the way the blend mode combo does
  B.sink_bp.mask_mode = DEVELOP_MASK_ENABLED | DEVELOP_MASK_MASK;
  _prune();
  assert_false(_still_a_user());
  _bench_cleanup();

  _bench_init();
  _make_classic_sink();
  _register_user();
  // repointed at another module: the piece's params name it
  g_strlcpy(B.sink_bp.raster_mask_source, "colorin", sizeof(B.sink_bp.raster_mask_source));
  _prune();
  assert_false(_still_a_user());
  _bench_cleanup();

  _bench_init();
  _make_flexi_sink(0);
  _register_user();
  _drop_flexi_element();
  _prune();
  assert_false(_still_a_user());
  _bench_cleanup();
}

/* The flexi element names its source by op AND instance, so a consumer reading
   from a different instance of the same module is not a consumer of this one.
   Multi-instance is where a name-only match quietly keeps the wrong source
   publishing. */
static void test_a_flexi_element_naming_another_instance_is_dropped(void **state)
{
  _bench_init();
  _make_flexi_sink(1);          // source instance 0 is the one being pruned
  _register_user();
  _prune();
  assert_false(_still_a_user());
  _bench_cleanup();

  // ... and it is kept once the source really is that instance
  _bench_init();
  B.source.multi_priority = 1;
  _make_flexi_sink(1);
  _register_user();
  _prune();
  assert_true(_still_a_user());
  _bench_cleanup();
}

/* A flexi consumer whose mask_id names nothing (a group deleted out from under
   it) is a phantom, and resolving it must not crash. NO_MASKID and a dangling
   id are different paths through _raster_form_consumes. */
static void test_a_flexi_consumer_with_no_group_is_dropped(void **state)
{
  _bench_init();
  _make_flexi_sink(0);
  _register_user();
  B.sink_bp.mask_id = NO_MASKID;
  _prune();
  assert_false(_still_a_user());
  _bench_cleanup();

  _bench_init();
  _make_flexi_sink(0);
  _register_user();
  B.sink_bp.mask_id = 9999;     // no such form
  _prune();
  assert_false(_still_a_user());
  _bench_cleanup();
}

/* A source nobody consumes, and a source whose table is empty: the prune must
   be a no-op rather than reaching into a NULL table. Cheap, and the early
   return it guards is the one line a refactor is most likely to move. */
static void test_an_empty_table_is_left_alone(void **state)
{
  _bench_init();
  _prune();
  assert_int_equal(g_hash_table_size(B.source.raster_mask.source.users), 0);
  _bench_cleanup();

  _bench_init();
  g_hash_table_destroy(B.source.raster_mask.source.users);
  B.source.raster_mask.source.users = NULL;
  _prune();                     // must not crash
  _bench_cleanup();
}

int main(void)
{
  const struct CMUnitTest tests[] =
  {
    cmocka_unit_test(test_a_live_consumer_survives),
    cmocka_unit_test(test_a_consumer_absent_from_the_pipe_is_dropped),
    cmocka_unit_test(test_a_disabled_piece_is_dropped),
    cmocka_unit_test(test_module_state_does_not_override_the_piece),
    cmocka_unit_test(test_a_desynced_consumer_is_dropped),
    cmocka_unit_test(test_a_flexi_element_naming_another_instance_is_dropped),
    cmocka_unit_test(test_a_flexi_consumer_with_no_group_is_dropped),
    cmocka_unit_test(test_an_empty_table_is_left_alone),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
