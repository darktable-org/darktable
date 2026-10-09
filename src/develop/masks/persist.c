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

#include "develop/masks/persist.h"

#include "common/darktable.h"
#include "develop/blend_gui_internal.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/masks.h"
#include "develop/masks/harvest_read.h"
#include "develop/masks/postedit_internal.h"
#include "develop/masks/scratch_image.h"
#include "develop/masks/verify_internal.h"

#ifdef _OPENMP
#include <omp.h>
#endif

#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <stdio.h>
#include <string.h>

// the scratch image both arms are driven through, as in roundtrip.c: real ids
// start at 1 and this runs against a throwaway database, so nothing collides
#define PERSIST_IMGID 1

// Both arms run the identical blend over data that should be identical, so a
// real match is bit-exact; the same reasoning as verify.c.
#define PERSIST_EPS 1e-6

// ---------------------------------------------------------------------------
// the sequences
// ---------------------------------------------------------------------------

/* A sequence is a list of steps: see postedit_internal.h for the step
   vocabulary itself (pokes, shape controls, and the two list operations), and
   persist.h for why each step's scope is resolved against the group as it
   stands in each arm independently rather than pinned once up front. */
typedef struct
{
  const char *name;
  const char *seam;   // what a divergence here would mean, for the report
  int n;
  step_t step[3];
} seq_t;

/* The sequences, grouped by the seam each block covers.

   A sequence is short on purpose. What is being asked is whether a *single*
   save is transparent; a longer chain multiplies the opportunities but tests
   the same thing, and it makes a failure harder to read. Where three steps
   appear it is because the seam genuinely needs them: something must set the
   group, something must cross the save, and something must read the group
   back. */
static const seq_t _sequences[] =
{
  // ---- one edit, one save. The floor: if these do not hold, nothing longer
  // means anything, and a failure names the single control that did not
  // survive rather than an interaction.
  { "single:flexi-difference", "a group operator does not survive a save",
    1, { { POKE_FLEXI_DIFFERENCE, SCOPE_GROUP } } },
  { "single:flexi-minimum", "a group operator does not survive a save",
    1, { { POKE_FLEXI_MINIMUM, SCOPE_GROUP } } },
  { "single:group-opacity", "a group opacity does not survive a save",
    1, { { POKE_GROUP_OPACITY, SCOPE_GROUP } } },
  { "single:group-refine", "a group refinement does not survive a save",
    1, { { POKE_GROUP_REFINE, SCOPE_GROUP } } },
  { "single:group-invert", "an invert-output does not survive a save",
    1, { { POKE_GROUP_INVERT, SCOPE_GROUP } } },
  { "single:group-bypass", "a bypass does not survive a save",
    1, { { POKE_GROUP_BYPASS, SCOPE_GROUP } } },
  { "single:elem-disable", "a disabled element does not survive a save",
    1, { { POKE_ELEM_DISABLE, SCOPE_LAST } } },
  { "single:elem-opacity", "an element opacity does not survive a save",
    1, { { POKE_ELEM_OPACITY, SCOPE_FIRST } } },

  // ---- an operator change, then another group control. Both are read from
  // the group's marker, so a save that stores one and drops the other shows
  // up here.
  { "operator:difference then opacity", "a group's settings do not survive a save together",
    2, { { POKE_FLEXI_DIFFERENCE, SCOPE_GROUP }, { POKE_GROUP_OPACITY, SCOPE_GROUP } } },
  { "operator:sum then invert", "a group's settings do not survive a save together",
    2, { { POKE_FLEXI_SUM, SCOPE_GROUP }, { POKE_GROUP_INVERT, SCOPE_GROUP } } },
  { "operator:exclusion then refine", "a group's settings do not survive a save together",
    2, { { POKE_FLEXI_EXCLUSION, SCOPE_GROUP }, { POKE_GROUP_REFINE, SCOPE_GROUP } } },

  // ---- migration's own output, built on. The first step changes nothing
  // structural, so the second is applied to a group whose markers and
  // disable bits came from the migration (_normalize_group() in
  // migrate_legacy.c) and have now been through storage once.
  { "migrated:opacity then refine", "migration's markers are lost by a save that did not touch them",
    2, { { POKE_ELEM_OPACITY, SCOPE_FIRST }, { POKE_GROUP_REFINE, SCOPE_GROUP } } },
  { "migrated:disable then minimum", "a disable bit set on a migrated group is lost by a save",
    2, { { POKE_ELEM_DISABLE, SCOPE_LAST }, { POKE_FLEXI_MINIMUM, SCOPE_GROUP } } },

  // ---- a group modifier set before the save and the operator or a setting
  // changed after it: the modifier must leave the rest of the group alone.
  { "modifier:bypass then minimum", "a modifier and the group it sits on disagree across a save",
    2, { { POKE_GROUP_BYPASS, SCOPE_GROUP }, { POKE_FLEXI_MINIMUM, SCOPE_GROUP } } },
  { "modifier:invert then difference", "a modifier and the group it sits on disagree across a save",
    2, { { POKE_GROUP_INVERT, SCOPE_GROUP }, { POKE_FLEXI_DIFFERENCE, SCOPE_GROUP } } },
  { "modifier:bypass then opacity", "a modifier and the group it sits on disagree across a save",
    2, { { POKE_GROUP_BYPASS, SCOPE_GROUP }, { POKE_GROUP_OPACITY, SCOPE_GROUP } } },

  // ---- the member list itself. Deleting a shape and reordering rows are
  // ordinary panel actions, and difference and exclusion fold in list order,
  // so both change what a later control reads back. Nothing in the poke
  // vocabulary can express either, which is why they are here: without them
  // the checks are silent about a whole axis of what the panel can do.
  { "structural:remove then minimum", "a deletion is not carried by the save",
    2, { { POKE_N, SCOPE_LAST, STEP_REMOVE }, { POKE_FLEXI_MINIMUM, SCOPE_GROUP } } },
  { "structural:remove then opacity", "a deletion is not carried by the save",
    2, { { POKE_N, SCOPE_FIRST, STEP_REMOVE }, { POKE_GROUP_OPACITY, SCOPE_GROUP } } },
  { "structural:reorder then difference", "a reorder is not carried by the save",
    2, { { POKE_N, SCOPE_LAST, STEP_MOVE_UP }, { POKE_FLEXI_DIFFERENCE, SCOPE_GROUP } } },
  { "structural:reorder then minimum", "a reorder is not carried by the save",
    2, { { POKE_N, SCOPE_LAST, STEP_MOVE_UP }, { POKE_FLEXI_MINIMUM, SCOPE_GROUP } } },
  { "structural:difference then remove", "a deletion after an operator change is not carried",
    2, { { POKE_FLEXI_DIFFERENCE, SCOPE_GROUP }, { POKE_N, SCOPE_LAST, STEP_REMOVE } } },

  /* ---- the shapes themselves. Everything above edits how members combine;
     these edit what they are, which is the one part of a mask with a per-type
     serialized representation of its own -- a blob of dt_masks_point_<type>_t
     in masks_history, written by code the harvested forms never exercise,
     because they arrive already-serialized and go back out unchanged.

     A shape control is therefore the only step here whose *first* half is at
     risk: a poke that fails to persist loses a state bit, while a geometry
     edit that fails to persist loses the shape. path.c's resize is the sharp
     case -- it keeps a cached baseline next to the points, so a save has to
     either carry that or reconstruct it, and a shape whose baseline came back
     wrong renders at the wrong size on the second open and at the right one on
     the first.

     Paired with a group control, for the same reason as every other block: a
     lost group setting and a lost shape look nothing alike in the report, and
     pairing them costs one step. */
  { "geom:translate then minimum", "a moved shape does not survive a save",
    2, { GEOM_STEP(GEOM_TRANSLATE, SCOPE_FIRST), { POKE_FLEXI_MINIMUM, SCOPE_GROUP } } },
  { "geom:size then difference", "a resized shape does not survive a save",
    2, { GEOM_STEP(GEOM_SIZE, SCOPE_FIRST), { POKE_FLEXI_DIFFERENCE, SCOPE_GROUP } } },
  { "geom:feather then opacity", "a re-feathered shape does not survive a save",
    2, { GEOM_STEP(GEOM_FEATHER, SCOPE_FIRST), { POKE_GROUP_OPACITY, SCOPE_GROUP } } },
  { "geom:node then exclusion", "a dragged node does not survive a save",
    2, { GEOM_STEP(GEOM_NODE, SCOPE_FIRST), { POKE_FLEXI_EXCLUSION, SCOPE_GROUP } } },
  { "geom:rotation then product", "a rotated shape does not survive a save",
    2, { GEOM_STEP(GEOM_ROTATION, SCOPE_FIRST), { POKE_FLEXI_PRODUCT, SCOPE_GROUP } } },
  { "geom:translate then translate", "a shape edited twice across two saves drifts",
    2, { GEOM_STEP(GEOM_TRANSLATE, SCOPE_FIRST),
         GEOM_STEP(GEOM_TRANSLATE, SCOPE_FIRST) } },
  { "geom:size then size", "a resize baseline is rebuilt from the stored shape",
    2, { GEOM_STEP(GEOM_SIZE, SCOPE_FIRST), GEOM_STEP(GEOM_SIZE, SCOPE_FIRST) } },
  { "geom:remove then translate", "a shape edit after a deletion addresses the wrong member",
    2, { { POKE_N, SCOPE_FIRST, STEP_REMOVE },
         GEOM_STEP(GEOM_TRANSLATE, SCOPE_FIRST) } },

  // ---- three steps: change the group, cross a save with an unrelated
  // change, then read the group back. Two saves, and the middle edit is what
  // stops the third from being a repeat of the first.
  { "chain:difference opacity invert", "a group setting does not survive an intervening save",
    3, { { POKE_FLEXI_DIFFERENCE, SCOPE_GROUP },
         { POKE_ELEM_OPACITY, SCOPE_FIRST },
         { POKE_GROUP_INVERT, SCOPE_GROUP } } },
  { "chain:refine sum minimum", "a group refinement does not survive operator changes",
    3, { { POKE_GROUP_REFINE, SCOPE_GROUP },
         { POKE_FLEXI_SUM, SCOPE_GROUP },
         { POKE_FLEXI_MINIMUM, SCOPE_GROUP } } },
};

#define SEQ_N ((int)(sizeof(_sequences) / sizeof(_sequences[0])))

// ---------------------------------------------------------------------------
// driving the scratch image
// ---------------------------------------------------------------------------

/** the `index`-th group of dt_masks_postedit_groups(), or NULL when the tree
    has fewer */
static dt_masks_form_t *_group_at(dt_develop_t *dev,
                                  const dt_develop_blend_params_t *bp,
                                  const int index)
{
  GList *all = dt_masks_postedit_groups(dev, bp);
  dt_masks_form_t *grp = g_list_nth_data(all, index);
  g_list_free(all);
  return grp;
}

/** One step of the persisted arm: read, apply the step, write back.

    The write is not a bare dt_dev_write_history_ext(). _dev_write_history_item()
    persists a history item's OWN forms snapshot, and a freshly-read stack has
    none -- only _dev_add_history_item_ext() fills it, by deep-copying
    dev->forms. So the sequence has to be the real one: pop the stack onto the
    modules, change the mask, then add a masks history item, which is what
    snapshots dev->forms. Calling the writer straight after a read instead
    wipes masks_history and stores nothing, which reads from the outside
    exactly like the change being lost -- roundtrip.c documents the same trap.

    Returns FALSE if the image came back with no flexi mask to edit. */
static gboolean _read_poke_write(const step_t *st, const int group_index)
{
  dt_develop_t dev;
  dt_masks_scratch_open(&dev, PERSIST_IMGID);
  dt_dev_pop_history_items_ext(&dev, dev.history_end);

  gboolean ok = FALSE;
  for(GList *m = dev.iop; m; m = g_list_next(m))
  {
    dt_iop_module_t *mod = m->data;
    if(!dt_masks_postedit_target_group(&dev, mod->blend_params)) continue;
    dt_masks_form_t *grp = _group_at(&dev, mod->blend_params, group_index);
    if(!grp) break;

    {
      dt_masks_postedit_apply_step(&dev, grp, st);
      // dt_dev_add_masks_history_item_ext, NOT the plain variant: only the
      // masks one passes include_masks = TRUE down to
      // _dev_add_history_item_ext, and only that snapshots dev->forms into the
      // item. The plain variant appends an item with forms == NULL, which
      // stores no masks at all.
      dt_dev_add_masks_history_item_ext(&dev, mod, FALSE, TRUE);
      ok = TRUE;
    }
    break;
  }

  if(ok) dt_dev_write_history_ext(&dev, PERSIST_IMGID);
  dt_dev_cleanup(&dev);
  return ok;
}

// ---------------------------------------------------------------------------
// rendering a stored state
// ---------------------------------------------------------------------------

/** Point an already-initialized replay at a different mask, taking ownership
    of `forms`. dt_masks_verify_render_mask() re-reads r->dev.forms into the pipe every time,
    so nothing else needs updating. */
static void _install_state(replay_t *r,
                           const dt_develop_blend_params_t *bp,
                           GList *forms)
{
  g_list_free_full(r->dev.forms, (GDestroyNotify)dt_masks_free_form);
  r->dev.forms = forms;
  // into the module's own allocation: it owns that buffer and frees it on
  // cleanup, so repointing it would double-free
  memcpy(r->module.blend_params, bp, sizeof(dt_develop_blend_params_t));
}

// ---------------------------------------------------------------------------
// one edit
// ---------------------------------------------------------------------------

typedef enum
{
  PERSIST_OK = 0,
  PERSIST_DIFFERENT,
  PERSIST_SKIPPED,
  PERSIST_ERROR
} persist_result_t;

typedef struct
{
  persist_result_t result;
  const char *skip_reason;
  int compared;
  int disagreed;
  int live;             // sequences that actually changed the mask
  int worst_seq;        // index into _sequences[], or -1
  double worst_diff;

  // how many groups the module renders through: 1 for a flat mask, more when
  // the top group has nested ones. Reported so a run says how much of the
  // nested surface it actually reached.
  int groups;
} persist_report_t;

typedef struct
{
  int compared;
  int disagreed;
  int live;
} seq_tally_t;

static void _persist_edit(JsonObject *edit,
                          persist_report_t *rep,
                          seq_tally_t *tally)
{
  memset(rep, 0, sizeof(*rep));
  rep->result = PERSIST_SKIPPED;
  rep->worst_seq = -1;

  // among others, an already-flexi edit is skipped: it was never migrated,
  // and the question here is about a migrated mask being built on
  dt_masks_harvest_edit_t e;
  rep->skip_reason = dt_masks_harvest_read_classic_edit(edit, &e);
  if(rep->skip_reason) return;

  int w, h;
  dt_masks_verify_replay_size(e.width, e.height, &w, &h);

  if(!dt_masks_scratch_reset_to_migrated(PERSIST_IMGID, &e, NULL, NULL))
  {
    g_list_free_full(e.forms, (GDestroyNotify)dt_masks_free_form);
    rep->skip_reason = "history row could not be seeded";
    return;
  }

  // the replay renders; it is built once and repointed at each state below.
  // Seeded with the harvested classic params so that a raster edit gets its
  // synthetic source attached from the fields it names -- no poke here touches
  // those, so the source stays right for every render.
  replay_t r;
  const char *init_err =
    dt_masks_verify_replay_init(&r, e.operation, &e.bp,
                                dt_masks_dup_forms_deep(e.forms, NULL),
                                e.width, e.height, w, h);
  if(init_err)
  {
    g_list_free_full(e.forms, (GDestroyNotify)dt_masks_free_form);
    rep->result = PERSIST_ERROR;
    rep->skip_reason = init_err;
    return;
  }

  const size_t npix = (size_t)w * h;

  // the baseline: the migrated mask as the first open leaves it in memory,
  // with nothing poked. Used only to tell a sequence that genuinely changed
  // something from one that was inert on this edit, so that a pass is not
  // reported as evidence when both arms rendered the same untouched mask.
  dt_develop_blend_params_t bp;
  GList *forms = NULL;
  if(!dt_masks_scratch_reset_to_migrated(PERSIST_IMGID, &e, &bp, &forms))
  {
    rep->result = PERSIST_ERROR;
    rep->skip_reason = "the seeded image came back with no history";
    goto out;
  }
  _install_state(&r, &bp, forms);

  // a mask this check cannot poke: no group means no run and no member
  if(!dt_masks_postedit_target_group(&r.dev, &bp))
  {
    rep->skip_reason = "no group to edit";
    goto out;
  }

  float *base = dt_masks_verify_render_mask(&r, NULL);
  if(!base)
  {
    rep->result = PERSIST_ERROR;
    rep->skip_reason = "the blend published no mask";
    goto out;
  }

  rep->result = PERSIST_OK;

  // how many groups the module renders through, counted once on the un-poked
  // tree: no poke here creates or destroys a group, so the count is stable and
  // both arms resolve the same index to the same group
  GList *g0 = dt_masks_postedit_groups(&r.dev, &bp);
  const int ngroups = (int)g_list_length(g0);
  g_list_free(g0);
  rep->groups = ngroups;

  for(int gi = 0; gi < ngroups; gi++)
  for(int q = 0; q < SEQ_N; q++)
  {
    const seq_t *seq = &_sequences[q];

    /* ---- arm A is the session that never closes: seed the classic edit,
       open it once, and from then on only change things. Reseeding per
       sequence also undoes what the previous sequence's arm B wrote, so each
       sequence starts where it says it does. */
    if(!dt_masks_scratch_reset_to_migrated(PERSIST_IMGID, &e, &bp, &forms))
      continue;
    _install_state(&r, &bp, forms);
    dt_masks_form_t *grp = _group_at(&r.dev, &bp, gi);
    if(!grp) continue;

    for(int s = 0; s < seq->n; s++)
      dt_masks_postedit_apply_step(&r.dev, grp, &seq->step[s]);
    float *a = dt_masks_verify_render_mask(&r, NULL);

    /* ---- arm B is the same edits with the image closed and reopened between
       every one of them. Back to the same starting point first. */
    gboolean b_ok = dt_masks_scratch_reset_to_migrated(PERSIST_IMGID, &e, NULL, NULL);
    for(int s = 0; b_ok && s < seq->n; s++)
      b_ok = _read_poke_write(&seq->step[s], gi);

    float *b = NULL;
    if(b_ok && dt_masks_scratch_read_last(PERSIST_IMGID, &bp, &forms))
    {
      _install_state(&r, &bp, forms);
      b = dt_masks_verify_render_mask(&r, NULL);
    }

    if(a && b)
    {
      rep->compared++;
      tally[q].compared++;

      const gboolean live = dt_masks_verify_max_abs_diff(a, base, npix) > PERSIST_EPS;
      if(live) { rep->live++; tally[q].live++; }

      const double d = dt_masks_verify_max_abs_diff(a, b, npix);
      if(d > PERSIST_EPS)
      {
        rep->disagreed++;
        tally[q].disagreed++;
        rep->result = PERSIST_DIFFERENT;
        if(d > rep->worst_diff) { rep->worst_diff = d; rep->worst_seq = q; }
      }
    }
    else if(!a || !b_ok)
    {
      // an arm that could not be built at all is an error, not a pass: a
      // missing render compares equal to nothing and would otherwise vanish
      rep->result = PERSIST_ERROR;
      rep->skip_reason = "an arm could not be rendered";
    }

    dt_free_align(a);
    dt_free_align(b);
  }

  dt_free_align(base);

out:
  dt_masks_verify_replay_cleanup(&r);
  g_list_free_full(e.forms, (GDestroyNotify)dt_masks_free_form);
}

// ---------------------------------------------------------------------------
// driver
// ---------------------------------------------------------------------------

static const char *_result_name(const persist_result_t r)
{
  switch(r)
  {
    case PERSIST_OK:        return "identical";
    case PERSIST_DIFFERENT: return "different";
    case PERSIST_SKIPPED:   return "skipped";
    default:                return "error";
  }
}

gboolean dt_masks_persist_harvest_section(const char *json_path, FILE *rf)
{
  setvbuf(stdout, NULL, _IOLBF, 0);

#ifdef _OPENMP
  /* single-threaded for the same reason as verify.c: a reduction whose float
     addition order depends on thread scheduling moves the last bits of the
     mask between runs, and this compares at 1e-6. The verdict would hold, as
     both arms run the same code, but the liveness count, which says the sweep
     was not vacuous, would not be stable. Set here, whatever ran before */
  omp_set_num_threads(1);
#endif

  JsonParser *parser;
  JsonArray *edits = dt_masks_harvest_open_edits(json_path, "persist", &parser);
  if(!edits) return FALSE;

  const guint n = json_array_get_length(edits);
  printf("[persist] saving and reopening between edits, over %u harvested"
         " edits from %s\n", n, json_path);

  if(rf) fprintf(rf, "\n  \"source\": \"%s\",\n  \"edits\": [", json_path);
  gboolean first_report = TRUE;

  seq_tally_t *tally = calloc((size_t)SEQ_N, sizeof(seq_tally_t));
  if(!tally)
  {
    g_object_unref(parser);
    return FALSE;
  }

  /* Exact repeats reuse the first occurrence's verdict rather than being
     replayed again -- see dt_masks_harvest_edit_key(). Every occurrence is
     still counted and reported. */
  GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  // `distinct` counts every edit actually replayed, skips included; `swept`
  // only the ones that got as far as a comparison. The repeat count is
  // `total - distinct`, not computed from `swept`, which can be smaller
  int distinct = 0, swept_distinct = 0;

  int total = 0, identical = 0, different = 0, skipped = 0, errors = 0;
  int compared = 0, disagreed = 0, live = 0, vacuous = 0;
  int nested_edits = 0, groups_swept = 0;

  for(guint i = 0; i < n; i++)
  {
    JsonObject *edit = json_array_get_object_element(edits, i);
    if(!edit) continue;

    gchar *key = dt_masks_harvest_edit_key(edit);
    const persist_report_t *cached = key ? g_hash_table_lookup(seen, key) : NULL;
    if(cached)
    {
      g_free(key);
      switch(cached->result)
      {
        case PERSIST_OK:        total++; identical++; break;
        case PERSIST_DIFFERENT: total++; different++; break;
        case PERSIST_SKIPPED:   skipped++;            break;
        default:                total++; errors++;    break;
      }
      compared += cached->compared;
      disagreed += cached->disagreed;
      live += cached->live;
      if(cached->compared == 0 && cached->result == PERSIST_OK) vacuous++;
      if(rf)
      {
        fprintf(rf, "%s\n    {\"index\": %u, \"result\": \"%s\","
                    " \"repeat\": true}",
                first_report ? "" : ",", i, _result_name(cached->result));
        first_report = FALSE;
      }
      continue;
    }

    persist_report_t rep;
    _persist_edit(edit, &rep, tally);
    distinct++;

    if(rep.result == PERSIST_SKIPPED)
    {
      dt_masks_harvest_remember(seen, key, &rep, sizeof(rep));
      DT_MASKS_HARVEST_SKIP(rep.skip_reason ? rep.skip_reason : "unspecified");
    }

    total++;
    swept_distinct++;
    groups_swept += rep.groups;
    if(rep.groups > 1) nested_edits++;
    compared += rep.compared;
    disagreed += rep.disagreed;
    live += rep.live;

    /* An edit that produced no comparison at all is reported, not counted as
       a pass. It cannot have failed, which is exactly the problem: a silent
       zero here would be indistinguishable from 24 sequences agreeing. */
    if(rep.compared == 0 && rep.result != PERSIST_ERROR) vacuous++;

    if(rep.result == PERSIST_OK) identical++;
    else if(rep.result == PERSIST_DIFFERENT)
    {
      different++;
      const seq_t *worst = rep.worst_seq >= 0 ? &_sequences[rep.worst_seq] : NULL;
      printf("[persist] DIFFERENT at edit %u (%s): %d/%d sequences disagree,"
             " worst '%s' by %.6f -- %s\n",
             i, dt_masks_harvest_obj_str(edit, "operation", "?"), rep.disagreed, rep.compared,
             worst ? worst->name : "?", rep.worst_diff,
             worst ? worst->seam : "?");
    }
    else errors++;

    if(rf)
    {
      const seq_t *worst = rep.worst_seq >= 0 ? &_sequences[rep.worst_seq] : NULL;
      fprintf(rf, "%s\n    {\"index\": %u, \"operation\": \"%s\","
                  " \"result\": \"%s\", \"repeat\": false,"
                  " \"compared\": %d, \"disagreed\": %d, \"live\": %d",
              first_report ? "" : ",", i, dt_masks_harvest_obj_str(edit, "operation", "?"),
              _result_name(rep.result), rep.compared, rep.disagreed, rep.live);
      if(worst)
        fprintf(rf, ", \"worst_sequence\": \"%s\", \"seam\": \"%s\","
                    " \"worst_diff\": %.9f",
                worst->name, worst->seam, rep.worst_diff);
      if(rep.result == PERSIST_ERROR && rep.skip_reason)
        fprintf(rf, ", \"error\": \"%s\"", rep.skip_reason);
      fputc('}', rf);
      first_report = FALSE;
    }

    dt_masks_harvest_remember(seen, key, &rep, sizeof(rep));

    if((i + 1) % 50 == 0) printf("[persist]   %u/%u ...\n", i + 1, n);
  }

  g_object_unref(parser);
  g_hash_table_destroy(seen);

  const gboolean passed = different == 0 && errors == 0 && vacuous == 0;

  if(rf)
  {
    fputs("\n  ],\n  \"summary\": {\n", rf);
    fprintf(rf, "    \"passed\": %s,\n", passed ? "true" : "false");
    fprintf(rf, "    \"harvested\": %u,\n", n);
    fprintf(rf, "    \"swept\": %d,\n", total);
    fprintf(rf, "    \"distinct_edits\": %d,\n", distinct);
    fprintf(rf, "    \"distinct_swept\": %d,\n", swept_distinct);
    fprintf(rf, "    \"identical\": %d,\n", identical);
    fprintf(rf, "    \"different\": %d,\n", different);
    fprintf(rf, "    \"errors\": %d,\n", errors);
    fprintf(rf, "    \"skipped\": %d,\n", skipped);
    fprintf(rf, "    \"sequences_compared\": %d,\n", compared);
    fprintf(rf, "    \"sequences_disagreed\": %d,\n", disagreed);
    fprintf(rf, "    \"sequences_live\": %d,\n", live);
    fprintf(rf, "    \"swept_nothing\": %d,\n", vacuous);
    fprintf(rf, "    \"groups_swept\": %d,\n", groups_swept);
    fprintf(rf, "    \"edits_with_nested_group\": %d,\n", nested_edits);
    fputs("    \"per_sequence\": [", rf);
    for(int q = 0; q < SEQ_N; q++)
      fprintf(rf, "%s\n      {\"name\": \"%s\", \"seam\": \"%s\","
                  " \"compared\": %d, \"disagreed\": %d, \"live\": %d}",
              q ? "," : "", _sequences[q].name, _sequences[q].seam,
              tally[q].compared, tally[q].disagreed, tally[q].live);
    fputs("\n    ]\n  }", rf);
  }

  printf("[persist]\n");
  printf("[persist] edits             : %d swept  (%d distinct swept,"
         " %d reused as repeats; %d edits replayed in all)\n",
         total, swept_distinct, total - swept_distinct, distinct);
  printf("[persist]   identical       : %d\n", identical);
  printf("[persist]   DIFFERENT       : %d\n", different);
  printf("[persist]   skipped         : %d\n", skipped);
  printf("[persist]   errors          : %d\n", errors);
  printf("[persist]   swept NOTHING   : %d  (no sequence could be compared;"
         " would pass vacuously)\n", vacuous);
  printf("[persist]\n");
  printf("[persist] groups swept       : %d  (%d edits had a nested group)\n",
         groups_swept, nested_edits);
  printf("[persist] sequences compared : %d\n", compared);
  printf("[persist]   disagreed        : %d\n", disagreed);
  printf("[persist]   changed the mask : %d  (the rest are legitimately inert"
         " on their edit)\n", live);
  printf("[persist]\n");
  printf("[persist] per sequence                          compared  disagreed  live\n");
  for(int q = 0; q < SEQ_N; q++)
    printf("[persist]   %-38s %8d %10d %5d\n", _sequences[q].name,
           tally[q].compared, tally[q].disagreed, tally[q].live);

  free(tally);
  return passed;
}

gboolean dt_masks_persist_harvest(const char *json_path, const char *report_path)
{
  return dt_masks_harvest_report(json_path, report_path, "persist",
                                 dt_masks_persist_harvest_section);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
