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

#include "develop/masks/undo.h"

#include "common/darktable.h"
#include "common/history.h"
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

// the scratch image every cycle is driven through, as in persist.c and
// roundtrip.c: this runs against a throwaway database, so nothing collides
#define UNDO_IMGID 1

// Both sides run the identical blend over data that should be identical, so a
// real match is bit-exact -- the same reasoning as verify.c and persist.c.
#define UNDO_EPS 1e-6

// ---------------------------------------------------------------------------
// the edits that get undone
// ---------------------------------------------------------------------------

/* One step each, not sequences.

   --persist-masks chains steps because what it asks about is a seam BETWEEN
   edits. Undo is per edit by construction: the darkroom records one snapshot
   per change and ctrl-z walks them one at a time, so a chain here would only
   be testing the same swap repeatedly while making a failure harder to place.

   The vocabulary is the panel's, taken whole rather than sampled: whatever a
   user can do is a thing they can then undo, and an undo that restores five of
   the six fields is exactly the kind of defect a sampled list walks past. The
   structural and geometry steps carry their own weight -- a deletion has to
   put a member back, and a shape edit has to put point data back, which is a
   different half of the snapshot from the state bits. */
typedef struct
{
  const char *name;
  const char *seam;   // what a divergence here would mean, for the report
  step_t step;
} undo_case_t;

static const undo_case_t _cases[] =
{
  // --- the group controls, all read back from the group's marker
  { "flexi-difference", "a group operator is not restored",
    { POKE_FLEXI_DIFFERENCE, SCOPE_GROUP } },
  { "flexi-minimum", "a group operator is not restored",
    { POKE_FLEXI_MINIMUM, SCOPE_GROUP } },
  { "flexi-product", "a group operator is not restored",
    { POKE_FLEXI_PRODUCT, SCOPE_GROUP } },
  { "group-opacity",  "a group opacity is not restored",
    { POKE_GROUP_OPACITY, SCOPE_GROUP } },
  { "group-refine",   "a group refinement is not restored",
    { POKE_GROUP_REFINE, SCOPE_GROUP } },
  { "group-invert",   "an invert-output is not restored",
    { POKE_GROUP_INVERT, SCOPE_GROUP } },
  { "group-bypass",   "a bypass is not restored",
    { POKE_GROUP_BYPASS, SCOPE_GROUP } },

  // --- the per-element controls
  { "elem-disable",   "a disabled element is not restored",
    { POKE_ELEM_DISABLE, SCOPE_LAST } },
  { "elem-inverse",   "an inverted shape is not restored",
    { POKE_ELEM_INVERSE, SCOPE_FIRST } },
  { "elem-opacity",   "an element opacity is not restored",
    { POKE_ELEM_OPACITY, SCOPE_FIRST } },
  { "elem-refine",    "an element refinement is not restored",
    { POKE_ELEM_REFINE, SCOPE_FIRST } },
  { "elem-hidden",    "a hidden element is not restored",
    { POKE_ELEM_HIDDEN, SCOPE_LAST } },

  /* --- the member list. An undo of a deletion has to put a member BACK, which
     nothing else here asks of the snapshot: every case above restores a field
     of a member that never went away. If the pre-edit snapshot shared its
     point list with the tree the deletion then mutated, this is where it
     shows. */
  { "remove",         "a deleted member is not restored",
    { POKE_N, SCOPE_LAST, STEP_REMOVE } },
  { "reorder",        "a reordered list is not restored",
    { POKE_N, SCOPE_LAST, STEP_MOVE_UP } },

  /* --- the shapes. The other half of the snapshot: a form's point blob rather
     than a group member's fields. dt_masks_dup_forms_deep() has to have copied
     the points and not aliased them, and the write has to put the pre-edit
     blob back into masks_history -- a shallow copy anywhere along that path
     leaves the edited shape in place while the operator bits undo correctly,
     which reads as "undo works" until someone looks at the canvas. */
  { "geom-translate", "a moved shape is not restored",
    GEOM_STEP(GEOM_TRANSLATE, SCOPE_FIRST) },
  { "geom-size",      "a resized shape is not restored",
    GEOM_STEP(GEOM_SIZE, SCOPE_FIRST) },
  { "geom-feather",   "a re-feathered shape is not restored",
    GEOM_STEP(GEOM_FEATHER, SCOPE_FIRST) },
  { "geom-node",      "a dragged node is not restored",
    GEOM_STEP(GEOM_NODE, SCOPE_FIRST) },
  { "geom-rotation",  "a rotated shape is not restored",
    GEOM_STEP(GEOM_ROTATION, SCOPE_FIRST) },
};

#define CASE_N ((int)(sizeof(_cases) / sizeof(_cases[0])))

// ---------------------------------------------------------------------------
// the scratch image
// ---------------------------------------------------------------------------

/** one extracted mask state: what a render needs, owned by the caller */
typedef struct
{
  dt_develop_blend_params_t bp;
  GList *forms;
  gboolean ok;
} state_t;

static void _state_free(state_t *s)
{
  g_list_free_full(s->forms, (GDestroyNotify)dt_masks_free_form);
  s->forms = NULL;
  s->ok = FALSE;
}

/** read the scratch image back into a state the caller owns */
static state_t _read_state(void)
{
  state_t s = { .ok = FALSE };
  s.ok = dt_masks_scratch_read_last(UNDO_IMGID, &s.bp, &s.forms);
  return s;
}

// ---------------------------------------------------------------------------
// the cycle
// ---------------------------------------------------------------------------

/* Drive one edit / undo / redo over the scratch image, filling the four states
   the comparison needs. Returns FALSE if the image came back with no flexi
   mask to edit, in which case nothing is filled.

   The order is _pop_undo()'s (libs/history.c), with the pieces that only exist
   under a GUI left out:

     open, pop           - the darkroom's state on entering it
     duplicate history   - what _lib_history_will_change_callback records
     edit, add item      - the user's change
     write               - what the darkroom does on leaving the image
     swap in the copy    - the undo itself
     write, reload       - _pop_undo's dt_dev_write_history +
                           dt_dev_reload_history_items

   The one substitution is that reload is a full close-and-reopen. That is
   strictly stronger than dt_dev_reload_history_items(), which re-reads from
   the same database rows this has just written anyway.

   dt_dev_add_masks_history_item_ext, NOT the plain variant: only the masks one
   passes include_masks = TRUE down to _dev_add_history_item_ext, and only that
   snapshots dev->forms into the item. The plain variant appends an item with
   forms == NULL, which stores no masks at all (persist.c pays for the same
   trap). */
static gboolean _undo_cycle(const step_t *st, const int group_index,
                            state_t *before, state_t *edited,
                            state_t *undone, state_t *redone)
{
  dt_develop_t dev;
  dt_masks_scratch_open(&dev, UNDO_IMGID);
  dt_dev_pop_history_items_ext(&dev, dev.history_end);

  dt_iop_module_t *mod = NULL;
  dt_masks_form_t *grp = NULL;
  for(GList *m = dev.iop; m; m = g_list_next(m))
  {
    dt_iop_module_t *cand = m->data;
    if(!dt_masks_postedit_target_group(&dev, cand->blend_params)) continue;
    GList *all = dt_masks_postedit_groups(&dev, cand->blend_params);
    grp = g_list_nth_data(all, group_index);
    g_list_free(all);
    if(grp) mod = cand;
    break;
  }
  if(!mod || !grp)
  {
    dt_dev_cleanup(&dev);
    return FALSE;
  }

  // the state as the darkroom shows it on entry
  memcpy(&before->bp, mod->blend_params, sizeof(dt_develop_blend_params_t));
  before->forms = dt_masks_dup_forms_deep(dev.forms, NULL);
  before->ok = TRUE;

  /* What the undo stack records, taken the way libs/history.c takes it: a deep
     duplicate of the whole stack made BEFORE the change. This is the object
     under test. */
  GList *pre_history = dt_history_duplicate(dev.history);
  const int pre_end = dev.history_end;

  // the edit
  dt_masks_postedit_apply_step(&dev, grp, st);
  dt_dev_add_masks_history_item_ext(&dev, mod, FALSE, TRUE);
  dt_dev_write_history_ext(&dev, UNDO_IMGID);

  memcpy(&edited->bp, mod->blend_params, sizeof(dt_develop_blend_params_t));
  edited->forms = dt_masks_dup_forms_deep(dev.forms, NULL);
  edited->ok = TRUE;

  // and what a redo would restore: the post-change stack, duplicated the same
  // way, because that is what the undo swaps out and holds on to
  GList *post_history = dt_history_duplicate(dev.history);
  const int post_end = dev.history_end;

  // --- undo: swap the pre-change stack back in, write it, reload
  g_list_free_full(dev.history, dt_dev_free_history_item);
  dev.history = pre_history;
  dev.history_end = pre_end;
  dt_dev_write_history_ext(&dev, UNDO_IMGID);
  *undone = _read_state();

  // --- redo: the same swap in the other direction
  g_list_free_full(dev.history, dt_dev_free_history_item);
  dev.history = post_history;
  dev.history_end = post_end;
  dt_dev_write_history_ext(&dev, UNDO_IMGID);
  *redone = _read_state();

  dt_dev_cleanup(&dev);
  return TRUE;
}

// ---------------------------------------------------------------------------
// one edit
// ---------------------------------------------------------------------------

typedef enum
{
  UNDO_OK = 0,
  UNDO_DIFFERENT,
  UNDO_SKIPPED,
  UNDO_ERROR
} undo_result_t;

typedef struct
{
  undo_result_t result;
  const char *skip_reason;
  int compared;          // cycles that produced a comparison
  int disagreed;         // ... of which did not come back
  int live;              // ... of which changed the mask at all
  int undo_bad;          // of the disagreements, which direction failed
  int redo_bad;
  int worst_case;        // index into _cases[], or -1
  double worst_diff;
  int groups;
} undo_report_t;

typedef struct
{
  int compared, disagreed, live, undo_bad, redo_bad;
} case_tally_t;

/** Point an already-initialized replay at a state and render it. The replay
    keeps ownership of nothing here: `forms` is duplicated in. */
static float *_render_state(replay_t *r, const state_t *s)
{
  if(!s->ok) return NULL;
  g_list_free_full(r->dev.forms, (GDestroyNotify)dt_masks_free_form);
  r->dev.forms = dt_masks_dup_forms_deep(s->forms, NULL);
  // into the module's own allocation: it owns that buffer and frees it on
  // cleanup, so repointing it would double-free
  memcpy(r->module.blend_params, &s->bp, sizeof(dt_develop_blend_params_t));
  return dt_masks_verify_render_mask(r, NULL);
}

static void _undo_edit(JsonObject *edit, undo_report_t *rep, case_tally_t *tally)
{
  memset(rep, 0, sizeof(*rep));
  rep->result = UNDO_SKIPPED;
  rep->worst_case = -1;

  // among others, an already-flexi edit is skipped: it was never migrated,
  // and this asks about undoing an edit to a migrated mask
  dt_masks_harvest_edit_t e;
  rep->skip_reason = dt_masks_harvest_read_classic_edit(edit, &e);
  if(rep->skip_reason) return;

  int w, h;
  dt_masks_verify_replay_size(e.width, e.height, &w, &h);

  if(!dt_masks_scratch_reset_to_migrated(UNDO_IMGID, &e, NULL, NULL))
  {
    g_list_free_full(e.forms, (GDestroyNotify)dt_masks_free_form);
    rep->skip_reason = "history row could not be seeded";
    return;
  }

  // the replay renders; built once and repointed at each state below. Seeded
  // with the harvested classic params so a raster edit gets its synthetic
  // source attached from the fields it names.
  replay_t r;
  const char *init_err =
    dt_masks_verify_replay_init(&r, e.operation, &e.bp,
                                dt_masks_dup_forms_deep(e.forms, NULL),
                                e.width, e.height, w, h);
  if(init_err)
  {
    g_list_free_full(e.forms, (GDestroyNotify)dt_masks_free_form);
    rep->result = UNDO_ERROR;
    rep->skip_reason = init_err;
    return;
  }

  const size_t npix = (size_t)w * h;

  // how many groups the module renders through, counted once: no case here
  // creates or destroys a group, so the count is stable
  {
    state_t opened = _read_state();
    if(opened.ok)
    {
      // dt_masks_postedit_groups resolves member formids through the dev, so the opened
      // tree has to be installed before it is walked
      g_list_free_full(r.dev.forms, (GDestroyNotify)dt_masks_free_form);
      r.dev.forms = dt_masks_dup_forms_deep(opened.forms, NULL);
      GList *g0 = dt_masks_postedit_groups(&r.dev, &opened.bp);
      rep->groups = (int)g_list_length(g0);
      g_list_free(g0);
    }
    _state_free(&opened);
  }

  if(rep->groups == 0)
  {
    rep->skip_reason = "no group to edit";
    goto out;
  }

  rep->result = UNDO_OK;

  for(int gi = 0; gi < rep->groups; gi++)
  for(int c = 0; c < CASE_N; c++)
  {
    // every cycle starts from the same migrated image: the previous cycle
    // wrote its redo state back, so without this each case would be building
    // on the last one's edit rather than on the first open
    if(!dt_masks_scratch_reset_to_migrated(UNDO_IMGID, &e, NULL, NULL))
      continue;

    state_t before = { .ok = FALSE }, edited = { .ok = FALSE };
    state_t undone = { .ok = FALSE }, redone = { .ok = FALSE };
    if(!_undo_cycle(&_cases[c].step, gi, &before, &edited, &undone, &redone))
    {
      _state_free(&before); _state_free(&edited);
      _state_free(&undone); _state_free(&redone);
      continue;
    }

    float *r_before = _render_state(&r, &before);
    float *r_edited = _render_state(&r, &edited);
    float *r_undone = _render_state(&r, &undone);
    float *r_redone = _render_state(&r, &redone);

    if(r_before && r_edited && r_undone && r_redone)
    {
      rep->compared++;
      tally[c].compared++;

      /* Did the edit do anything at all? Reported, never failed on: plenty of
         controls are legitimately inert on a given edit (a difference between
         shapes that do not overlap, an operator on a group of one member, which
         has nothing to combine), and on an inert edit the undo is trivially correct. A pass
         built only out of those would be evidence of nothing, which is what
         this counter exists to make visible. */
      const gboolean live = dt_masks_verify_max_abs_diff(r_before, r_edited, npix) > UNDO_EPS;
      if(live) { rep->live++; tally[c].live++; }

      const double d_undo = dt_masks_verify_max_abs_diff(r_before, r_undone, npix);
      const double d_redo = dt_masks_verify_max_abs_diff(r_edited, r_redone, npix);
      const double worst = MAX(d_undo, d_redo);

      if(worst > UNDO_EPS)
      {
        rep->disagreed++;
        tally[c].disagreed++;
        if(d_undo > UNDO_EPS) { rep->undo_bad++; tally[c].undo_bad++; }
        if(d_redo > UNDO_EPS) { rep->redo_bad++; tally[c].redo_bad++; }
        rep->result = UNDO_DIFFERENT;
        if(worst > rep->worst_diff)
        {
          rep->worst_diff = worst;
          rep->worst_case = c;
        }
      }
    }
    else if(before.ok && edited.ok)
    {
      // one arm published a mask and another did not: a divergence in its own
      // right, and not one a pixel metric can express
      rep->compared++;
      tally[c].compared++;
      if((r_before != NULL) != (r_undone != NULL)
         || (r_edited != NULL) != (r_redone != NULL))
      {
        rep->disagreed++;
        tally[c].disagreed++;
        rep->result = UNDO_DIFFERENT;
        if(rep->worst_case < 0) rep->worst_case = c;
        rep->worst_diff = MAX(rep->worst_diff, 1.0);
      }
    }

    dt_free_align(r_before);
    dt_free_align(r_edited);
    dt_free_align(r_undone);
    dt_free_align(r_redone);
    _state_free(&before); _state_free(&edited);
    _state_free(&undone); _state_free(&redone);
  }

out:
  dt_masks_verify_replay_cleanup(&r);
  g_list_free_full(e.forms, (GDestroyNotify)dt_masks_free_form);
}

// ---------------------------------------------------------------------------
// driver
// ---------------------------------------------------------------------------

static const char *_result_name(const undo_result_t r)
{
  switch(r)
  {
    case UNDO_OK:        return "identical";
    case UNDO_DIFFERENT: return "different";
    case UNDO_SKIPPED:   return "skipped";
    default:             return "error";
  }
}

gboolean dt_masks_undo_harvest_section(const char *json_path, FILE *rf)
{
  setvbuf(stdout, NULL, _IOLBF, 0);

#ifdef _OPENMP
  // single-threaded for the same reason as every other replay check: a
  // reduction whose float addition order depends on thread scheduling makes
  // the last bits of the mask move between runs, and this compares at 1e-6
  omp_set_num_threads(1);
#endif

  JsonParser *parser;
  JsonArray *edits = dt_masks_harvest_open_edits(json_path, "undo", &parser);
  if(!edits) return FALSE;

  const guint n = json_array_get_length(edits);
  printf("[undo] undoing and redoing every panel action, over %u harvested"
         " edits from %s\n", n, json_path);

  if(rf) fprintf(rf, "\n  \"source\": \"%s\",\n  \"edits\": [", json_path);
  gboolean first_report = TRUE;

  case_tally_t *tally = calloc((size_t)CASE_N, sizeof(case_tally_t));
  if(!tally)
  {
    g_object_unref(parser);
    return FALSE;
  }

  /* Exact repeats reuse the first occurrence's verdict rather than being
     replayed again -- see dt_masks_harvest_edit_key(). Every occurrence is
     still counted and reported. */
  GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  int distinct = 0, swept_distinct = 0;

  int total = 0, identical = 0, different = 0, skipped = 0, errors = 0;
  int compared = 0, disagreed = 0, live = 0, vacuous = 0;
  int undo_bad = 0, redo_bad = 0;
  int nested_edits = 0, groups_swept = 0;
  double worst_diff = 0.0;
  int worst_index = -1;

  for(guint i = 0; i < n; i++)
  {
    JsonObject *edit = json_array_get_object_element(edits, i);
    if(!edit) continue;

    gchar *key = dt_masks_harvest_edit_key(edit);
    const undo_report_t *cached = key ? g_hash_table_lookup(seen, key) : NULL;
    if(cached)
    {
      g_free(key);
      switch(cached->result)
      {
        case UNDO_OK:        total++; identical++; break;
        case UNDO_DIFFERENT: total++; different++; break;
        case UNDO_SKIPPED:   skipped++;            break;
        default:             total++; errors++;    break;
      }
      compared += cached->compared;
      disagreed += cached->disagreed;
      live += cached->live;
      undo_bad += cached->undo_bad;
      redo_bad += cached->redo_bad;
      if(cached->compared == 0 && cached->result == UNDO_OK) vacuous++;
      if(rf)
      {
        fprintf(rf, "%s\n    {\"index\": %u, \"result\": \"%s\","
                    " \"repeat\": true}",
                first_report ? "" : ",", i, _result_name(cached->result));
        first_report = FALSE;
      }
      continue;
    }

    undo_report_t rep;
    _undo_edit(edit, &rep, tally);
    distinct++;

    if(rep.result == UNDO_SKIPPED)
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
    undo_bad += rep.undo_bad;
    redo_bad += rep.redo_bad;

    /* An edit that produced no comparison at all is reported, not counted as a
       pass. It cannot have failed, which is exactly the problem: a silent zero
       here would be indistinguishable from every case agreeing. */
    if(rep.compared == 0 && rep.result != UNDO_ERROR) vacuous++;

    if(rep.result == UNDO_OK) identical++;
    else if(rep.result == UNDO_DIFFERENT)
    {
      different++;
      const undo_case_t *worst =
        rep.worst_case >= 0 ? &_cases[rep.worst_case] : NULL;
      printf("[undo] DIFFERENT at edit %u (%s): %d/%d cycles did not come back"
             " (%d undo, %d redo), worst '%s' by %.6f -- %s\n",
             i, dt_masks_harvest_obj_str(edit, "operation", "?"), rep.disagreed, rep.compared,
             rep.undo_bad, rep.redo_bad, worst ? worst->name : "?",
             rep.worst_diff, worst ? worst->seam : "?");
      if(rep.worst_diff > worst_diff)
      {
        worst_diff = rep.worst_diff;
        worst_index = (int)i;
      }
    }
    else errors++;

    if(rf)
    {
      const undo_case_t *worst =
        rep.worst_case >= 0 ? &_cases[rep.worst_case] : NULL;
      fprintf(rf, "%s\n    {\"index\": %u, \"operation\": \"%s\","
                  " \"result\": \"%s\", \"repeat\": false,"
                  " \"compared\": %d, \"disagreed\": %d, \"live\": %d,"
                  " \"undo_failed\": %d, \"redo_failed\": %d",
              first_report ? "" : ",", i, dt_masks_harvest_obj_str(edit, "operation", "?"),
              _result_name(rep.result), rep.compared, rep.disagreed, rep.live,
              rep.undo_bad, rep.redo_bad);
      if(worst)
        fprintf(rf, ", \"worst_case\": \"%s\", \"seam\": \"%s\","
                    " \"worst_diff\": %.9g",
                worst->name, worst->seam, rep.worst_diff);
      if(rep.result == UNDO_ERROR && rep.skip_reason)
        fprintf(rf, ", \"error\": \"%s\"", rep.skip_reason);
      fputc('}', rf);
      first_report = FALSE;
    }

    dt_masks_harvest_remember(seen, key, &rep, sizeof(rep));

    if((i + 1) % 50 == 0) printf("[undo]   %u/%u ...\n", i + 1, n);
  }

  g_object_unref(parser);
  g_hash_table_destroy(seen);

  const gboolean passed =
    different == 0 && errors == 0 && vacuous == 0;

  printf("[undo]\n");
  printf("[undo] edits swept          : %d  (%d distinct, %d of those swept)\n",
         total, distinct, swept_distinct);
  printf("[undo]   identical          : %d\n", identical);
  printf("[undo]   DIFFERENT          : %d\n", different);
  printf("[undo]   skipped            : %d\n", skipped);
  printf("[undo]   errors             : %d\n", errors);
  printf("[undo]   swept NOTHING      : %d  (no cycle ran; not a pass)\n", vacuous);
  printf("[undo]\n");
  printf("[undo] groups swept         : %d  (%d edits had a nested group)\n",
         groups_swept, nested_edits);
  printf("[undo] cycles compared      : %d\n", compared);
  printf("[undo]   did not come back  : %d  (%d undo, %d redo)\n",
         disagreed, undo_bad, redo_bad);
  printf("[undo]   changed the mask   : %d  (the rest were inert on their"
         " edit, so their undo is trivially right)\n", live);
  if(worst_index >= 0)
    printf("[undo]   worst difference   : %.9g (edit %d)\n",
           worst_diff, worst_index);
  printf("[undo]\n");
  printf("[undo] per action                    compared  not back  undo  redo  live\n");
  for(int c = 0; c < CASE_N; c++)
  {
    if(tally[c].compared == 0) continue;
    printf("[undo]   %-30s %8d  %8d  %4d  %4d  %4d\n",
           _cases[c].name, tally[c].compared, tally[c].disagreed,
           tally[c].undo_bad, tally[c].redo_bad, tally[c].live);
  }

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
    fprintf(rf, "    \"cycles_compared\": %d,\n", compared);
    fprintf(rf, "    \"cycles_disagreed\": %d,\n", disagreed);
    fprintf(rf, "    \"cycles_live\": %d,\n", live);
    fprintf(rf, "    \"undo_failed\": %d,\n", undo_bad);
    fprintf(rf, "    \"redo_failed\": %d,\n", redo_bad);
    fprintf(rf, "    \"swept_nothing\": %d,\n", vacuous);
    fprintf(rf, "    \"groups_swept\": %d,\n", groups_swept);
    fprintf(rf, "    \"edits_with_nested_group\": %d,\n", nested_edits);
    fprintf(rf, "    \"worst_diff\": %.9g,\n", worst_diff);
    fputs("    \"per_action\": [", rf);
    gboolean first_case = TRUE;
    for(int c = 0; c < CASE_N; c++)
    {
      if(tally[c].compared == 0) continue;
      fprintf(rf, "%s\n      {\"action\": \"%s\", \"compared\": %d,"
                  " \"disagreed\": %d, \"undo_failed\": %d,"
                  " \"redo_failed\": %d, \"live\": %d}",
              first_case ? "" : ",", _cases[c].name, tally[c].compared,
              tally[c].disagreed, tally[c].undo_bad, tally[c].redo_bad,
              tally[c].live);
      first_case = FALSE;
    }
    fputs("\n    ]\n  }\n", rf);
  }

  free(tally);
  return passed;
}

gboolean dt_masks_undo_harvest(const char *json_path, const char *report_path)
{
  FILE *rf = report_path ? g_fopen(report_path, "wb") : NULL;
  if(report_path && !rf)
    fprintf(stderr, "[undo] cannot write report to %s\n", report_path);

  if(rf) fputs("{", rf);
  const gboolean ok = dt_masks_undo_harvest_section(json_path, rf);
  if(rf)
  {
    fputs("}\n", rf);
    fclose(rf);
    printf("[undo] per-edit report written to %s\n", report_path);
  }
  return ok;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
