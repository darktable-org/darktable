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

#include "develop/masks/roundtrip.h"

#include "common/darktable.h"
#include "common/debug.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/masks.h"
#include "develop/masks/harvest_read.h"
#include "develop/masks/scratch_image.h"

#include <json-glib/json-glib.h>
#include <sqlite3.h>
#include <stdio.h>

// A single scratch image reused for every edit, wiped between them. Real
// darktable ids start at 1 and this runs against a throwaway database (the
// caller is expected to pass --library :memory:), so there is nothing to
// collide with.
#define ROUNDTRIP_IMGID 1

// ---------------------------------------------------------------------------
// snapshotting: everything the mask depends on, as comparable text
// ---------------------------------------------------------------------------

static gint _form_by_id(gconstpointer a, gconstpointer b)
{
  return ((const dt_masks_form_t *)a)->formid - ((const dt_masks_form_t *)b)->formid;
}

/** Render the mask-relevant state of a loaded dev as a canonical string.

    Text rather than a struct comparison so that a mismatch names the field
    that moved: this test exists to catch persistence bugs, and "group_opacity
    1 vs 0 on member 3" is an answer, where "the snapshots differ" is another
    investigation.

    Forms are emitted in formid order, not list order. The list order out of
    the database is not part of the mask's meaning (members are ordered inside
    their group's point list, which *is* emitted in order), and sorting keeps
    an incidental reordering from being reported as a round-trip failure. */
static gchar *_snapshot(dt_develop_t *dev)
{
  GString *s = g_string_new(NULL);

  // dev->history, not dev->iop: dt_dev_read_history_ext() fills the history
  // stack, and a module's blend_params change only when the stack is popped
  // onto it, which needs pipes. dev->iop would give two empty module lists,
  // which compare equal whatever migration did. The history item is also what
  // dt_dev_write_history_ext() writes back.
  // Only the last history item: simulating the user's edit appends one, which
  // a comparison of whole stacks would report. The last item is the effective
  // state a reload has to reproduce.
  GList *last = g_list_last(dev->history);
  for(GList *m = last; m; m = NULL)
  {
    const dt_dev_history_item_t *h = m->data;
    const dt_develop_blend_params_t *bp = h->blend_params;
    if(!bp || bp->mask_mode == DEVELOP_MASK_DISABLED) continue;
    g_string_append_printf(s,
      "module %s.%d enabled=%d mask_mode=%u mask_id=%d combine=%u opacity=%.6f\n"
      "  blendif=%u feather=%.6f/%u blur=%.6f contrast=%.6f brightness=%.6f details=%.6f\n"
      "  raster=%s.%d id=%d inv=%d\n",
      h->op_name, h->multi_priority, h->enabled ? 1 : 0,
      bp->mask_mode, bp->mask_id, bp->mask_combine, bp->opacity,
      bp->blendif, bp->feathering_radius, bp->feathering_guide, bp->blur_radius,
      bp->contrast, bp->brightness, bp->details,
      bp->raster_mask_source, bp->raster_mask_instance,
      bp->raster_mask_id, bp->raster_mask_invert ? 1 : 0);
  }

  GList *sorted = g_list_copy(dev->forms);
  sorted = g_list_sort(sorted, _form_by_id);
  for(GList *f = sorted; f; f = g_list_next(f))
  {
    const dt_masks_form_t *form = f->data;
    g_string_append_printf(s, "form %d type=%d version=%d points=%u\n",
                           form->formid, form->type, form->version,
                           g_list_length(form->points));
    if(!(form->type & DT_MASKS_GROUP)) continue;
    for(GList *p = form->points; p; p = g_list_next(p))
    {
      const dt_masks_point_group_t *pt = p->data;
      g_string_append_printf(s,
        "  member %d parent=%d state=%d opacity=%.6f group_opacity=%.6f"
        " refine=%d\n",
        pt->formid, pt->parentid, pt->state, pt->opacity,
        pt->group_opacity, pt->refinement.enabled);
    }
  }
  g_list_free(sorted);

  return g_string_free(s, FALSE);
}

/** Does every group list, nested ones included, start with its marker?

    Comparing load #1 with load #2 catches state that changes across a save,
    and nothing else -- a migration that produced the *same wrong* tree both
    times would pass it. That is a real blind spot, because the two loads take
    genuinely different paths (the first migrates from classic blend_params,
    the second finds them already flexi and no-ops), and the interesting
    failure is precisely one of them silently doing nothing.

    So each load is also checked for what a normalization that did not run
    leaves behind: a list with no marker, which the flexi fold renders as no
    mask at all (#21905). Checking it on both loads means a normalization that
    failed to run is caught even when it fails identically twice.

    A difference group of several members is indistinguishable here from a
    run wrongly left merged, so that case is left to --verify-masks, which
    renders both.

    Returns a description of the first violation, or NULL. */
static gchar *_check_groups(dt_develop_t *dev,
                            const dt_mask_id_t formid,
                            const dt_masks_point_group_t *ref,
                            const int depth)
{
  if(depth > DT_MASKS_NESTING_MAX) return NULL;
  dt_masks_form_t *grp = dt_masks_get_from_id(dev, formid);
  if(!grp || !(grp->type & DT_MASKS_GROUP) || (grp->type & (DT_MASKS_CLONE | DT_MASKS_OBJECT)))
    return NULL;

  // a group's list holds its marker first, and no other
  if(!grp->points || !dt_masks_point_is_marker(grp->points->data))
    return g_strdup_printf("group %d does not start with its marker", grp->formid);
  for(const GList *p = grp->points->next; p; p = g_list_next(p))
    if(dt_masks_point_is_marker(p->data))
      return g_strdup_printf("group %d holds a second marker", grp->formid);

  // a nested group's settings are its own, never its reference's
  if(ref
     && (ref->opacity != 1.0f || (ref->state & DT_MASKS_STATE_INVERSE)
         || ref->refinement.enabled != DT_MASKS_REFINE_OFF))
    return g_strdup_printf("the reference to nested group %d has settings of its own",
                           grp->formid);

  for(GList *p = grp->points; p; p = g_list_next(p))
  {
    const dt_masks_point_group_t *pt = p->data;
    if(dt_masks_point_is_marker(pt)) continue;
    gchar *deeper = _check_groups(dev, pt->formid, pt, depth + 1);
    if(deeper) return deeper;
  }
  return NULL;
}

static gchar *_check_group_invariant(dt_develop_t *dev)
{
  // walked from each module's own mask_id, not over dev->forms: dev->forms is
  // per image, and every masks_history row is a full cumulative snapshot (see
  // migrate_legacy.c's header), so it carries groups of other modules and
  // groups orphaned by earlier edits. Those never render, migration does not
  // touch them, and a rendering invariant means nothing for them
  GList *last = g_list_last(dev->history);
  for(GList *m = last; m; m = NULL)
  {
    const dt_dev_history_item_t *h = m->data;
    const dt_develop_blend_params_t *bp = h->blend_params;
    if(!bp || !(bp->mask_mode & DEVELOP_MASK_FLEXI)) continue;
    if(!dt_is_valid_maskid(bp->mask_id)) continue;
    gchar *v = _check_groups(dev, bp->mask_id, NULL, 0);
    if(v) return v;
  }
  return NULL;
}

/** Load the scratch image through the real history reader and snapshot it.
    `write_back` then also stores the loaded state, the way a user's first edit
    would; `violation` (may be NULL) receives the group invariant report. */
static gchar *_load_and_snapshot(const gboolean write_back, gchar **violation)
{
  dt_develop_t dev;
  dt_masks_scratch_open(&dev, ROUNDTRIP_IMGID);

  gchar *snap = _snapshot(&dev);
  if(violation) *violation = _check_group_invariant(&dev);

  if(write_back)
  {
    // simulate the user opening the image and touching the mask, which, not
    // the load, is what writes.
    //
    // _dev_write_history_item() stores a history item's own forms snapshot
    // (dt_dev_history_item_t.forms), and a freshly read stack has none: only
    // _dev_add_history_item_ext() fills it, by deep-copying dev->forms. A
    // dt_dev_write_history_ext() straight after a read wipes masks_history and
    // writes nothing, which looks like the normalization being lost.
    //
    // So: pop the stack onto the modules (they then carry the migrated
    // blend_params), then add a history item, which snapshots dev->forms after
    // dt_masks_normalize_flexi_groups() has run on it.
    dt_dev_pop_history_items_ext(&dev, dev.history_end);

    for(GList *m = dev.iop; m; m = g_list_next(m))
    {
      dt_iop_module_t *mod = m->data;
      if(mod->blend_params && (mod->blend_params->mask_mode & DEVELOP_MASK_FLEXI))
      {
        // dt_dev_add_masks_history_item_ext, NOT dt_dev_add_history_item_ext:
        // only the masks variant passes include_masks=TRUE down to
        // _dev_add_history_item_ext, and only that snapshots dev->forms into
        // the item. The plain variant appends an item with forms == NULL,
        // which writes no masks at all -- indistinguishable, from the
        // outside, from the normalization being lost.
        dt_dev_add_masks_history_item_ext(&dev, mod, FALSE, TRUE);
        break;
      }
    }

    dt_dev_write_history_ext(&dev, ROUNDTRIP_IMGID);
  }

  dt_dev_cleanup(&dev);
  return snap;
}

// ---------------------------------------------------------------------------
// driver
// ---------------------------------------------------------------------------

/** first line that differs, for the report */
static gchar *_first_difference(const char *a, const char *b)
{
  gchar **la = g_strsplit(a, "\n", -1);
  gchar **lb = g_strsplit(b, "\n", -1);
  gchar *out = NULL;
  for(int i = 0; la[i] || lb[i]; i++)
  {
    const char *x = la[i] ? la[i] : "(end)";
    const char *y = lb[i] ? lb[i] : "(end)";
    if(strcmp(x, y))
    {
      out = g_strdup_printf("line %d: after load '%s' | after reload '%s'", i + 1, x, y);
      break;
    }
    if(!la[i] || !lb[i]) break;
  }
  g_strfreev(la);
  g_strfreev(lb);
  return out ? out : g_strdup("(no line-level difference found)");
}

// the verdict on an edit, which an exact repeat of it reuses
typedef struct
{
  int kind; // 0 same, 1 different, 2 error
  gchar *diff;
  gboolean no_module;
} roundtrip_cached_t;

static void _roundtrip_cached_free(gpointer data)
{
  roundtrip_cached_t *c = data;
  g_free(c->diff);
  free(c);
}

gboolean dt_masks_roundtrip_harvest_section(const char *json_path, FILE *rf)
{
  setvbuf(stdout, NULL, _IOLBF, 0);

  JsonParser *parser;
  JsonArray *edits = dt_masks_harvest_open_edits(json_path, "roundtrip", &parser);
  if(!edits) return FALSE;

  const guint n = json_array_get_length(edits);
  printf("[roundtrip] round-tripping %u harvested edits from %s\n", n, json_path);

  if(rf) fprintf(rf, "\n  \"source\": \"%s\",\n  \"edits\": [", json_path);
  gboolean first_report = TRUE;

  /* Exact repeats reuse the first occurrence's verdict rather than being
     seeded, loaded and snapshotted again -- see dt_masks_harvest_edit_key().
     Every occurrence is still counted and reported. */
  GHashTable *seen =
    g_hash_table_new_full(g_str_hash, g_str_equal, g_free, _roundtrip_cached_free);
  int distinct = 0;

  int total = 0, same = 0, differ = 0, skipped = 0, errors = 0;
  int no_module = 0, multi_instance = 0;

  for(guint i = 0; i < n; i++)
  {
    JsonObject *edit = json_array_get_object_element(edits, i);
    if(!edit) continue;

    // among others, an already-flexi edit is skipped: it has no migration to
    // survive, and the round trip would be testing the plain history reader,
    // which is not what this is for
    dt_masks_harvest_edit_t e;
    const char *why = dt_masks_harvest_read_classic_edit(edit, &e);
    if(why) DT_MASKS_HARVEST_SKIP(why);
    const char *op = e.operation;
    const int mp = e.multi_priority;

    dt_masks_scratch_wipe_history(ROUNDTRIP_IMGID);
    dt_masks_scratch_seed_image(ROUNDTRIP_IMGID, e.width, e.height);
    // the iop-order entry must exist before the history row referencing it,
    // or dt_dev_read_history_ext() drops the row (see scratch_image.h)
    if(op) dt_masks_scratch_seed_iop_order(ROUNDTRIP_IMGID, op, mp);
    const gboolean seeded =
      op && dt_masks_scratch_seed_history(ROUNDTRIP_IMGID, 0, op, mp, e.blendop_version,
                                          &e.bp, e.forms);
    g_list_free_full(e.forms, (GDestroyNotify)dt_masks_free_form);

    if(!seeded) DT_MASKS_HARVEST_SKIP("history row could not be seeded");

    total++;
    if(mp > 0) multi_instance++;

    gchar *key = dt_masks_harvest_edit_key(edit);
    const roundtrip_cached_t *cached = key ? g_hash_table_lookup(seen, key) : NULL;
    if(cached)
    {
      g_free(key);
      if(cached->no_module) no_module++;
      if(cached->kind == 0) same++;
      else if(cached->kind == 1) differ++;
      else errors++;
      if(rf)
      {
        gchar *esc = cached->diff ? g_strescape(cached->diff, NULL) : NULL;
        fprintf(rf, "%s\n    {\"index\": %u, \"operation\": \"%s\","
                    " \"mask_mode\": %u, \"result\": \"%s\", \"repeat\": true"
                    "%s%s%s}",
                first_report ? "" : ",", i, op, e.bp.mask_mode,
                cached->kind == 1 ? "different" : (cached->kind == 0 ? "same" : "error"),
                esc ? ", \"first_difference\": \"" : "", esc ? esc : "", esc ? "\"" : "");
        g_free(esc);
        first_report = FALSE;
      }
      if((i + 1) % 250 == 0) printf("[roundtrip]   %u/%u ...\n", i + 1, n);
      continue;
    }
    distinct++;

    // load #1: the real migration runs here (the stored blendop_version is
    // classic), then writes everything back
    gchar *v1 = NULL, *v2 = NULL;
    gchar *snap1 = _load_and_snapshot(TRUE, &v1);
    // load #2: blendop_params are flexi now, so migration no-ops and the
    // stored forms have to carry what load #1 derived in memory
    gchar *snap2 = _load_and_snapshot(FALSE, &v2);

    /* a guard against passing vacuously, not a statistic.
       dt_dev_read_history_ext() silently drops a history row whose
       (operation, multi_priority) has no iop-order entry, and two snapshots of
       a dev with no modules compare equal whatever migration did. A non-zero
       count means the run proves nothing about those edits, so it is
       reported next to the pass count */
    if(snap1 && !strstr(snap1, "module ")) no_module++;

    gchar *diff = NULL;
    if(!snap1 || !snap2)
    {
      errors++;
    }
    else if(v1 || v2)
    {
      differ++;
      diff = g_strdup_printf("group invariant violated after %s: %s",
                             v1 ? "load" : "reload", v1 ? v1 : v2);
      printf("[roundtrip] INVARIANT at edit %u (%s): %s\n", i, op, diff);
    }
    else if(!strcmp(snap1, snap2))
    {
      same++;
    }
    else
    {
      differ++;
      diff = _first_difference(snap1, snap2);
      printf("[roundtrip] DIFFERENT at edit %u (%s): %s\n", i, op, diff);
    }

    if(rf)
    {
      gchar *esc = diff ? g_strescape(diff, NULL) : NULL;
      fprintf(rf, "%s\n    {\"index\": %u, \"operation\": \"%s\", \"mask_mode\": %u,"
                  " \"result\": \"%s\", \"repeat\": false%s%s%s}",
              first_report ? "" : ",", i, op, e.bp.mask_mode,
              diff ? "different" : (snap1 && snap2 ? "same" : "error"),
              esc ? ", \"first_difference\": \"" : "", esc ? esc : "", esc ? "\"" : "");
      g_free(esc);
      first_report = FALSE;
    }

    if(key)
    {
      roundtrip_cached_t *store = calloc(1, sizeof(roundtrip_cached_t));
      if(store)
      {
        store->kind = diff ? 1 : ((snap1 && snap2) ? 0 : 2);
        store->diff = diff ? g_strdup(diff) : NULL;
        store->no_module = snap1 && !strstr(snap1, "module ");
        g_hash_table_insert(seen, key, store);
      }
      else g_free(key);
    }

    g_free(v1);
    g_free(v2);
    g_free(diff);
    g_free(snap1);
    g_free(snap2);

    if((i + 1) % 250 == 0) printf("[roundtrip]   %u/%u ...\n", i + 1, n);
  }

  g_object_unref(parser);

  const gboolean passed = differ == 0 && errors == 0 && no_module == 0;

  // the report carries every figure the summary below prints, so it can be read
  // on its own without the terminal output of the run that produced it
  if(rf)
  {
    fputs("\n  ],\n  \"summary\": {\n", rf);
    fprintf(rf, "    \"passed\": %s,\n", passed ? "true" : "false");
    fprintf(rf, "    \"harvested\": %u,\n", n);
    fprintf(rf, "    \"round_tripped\": %d,\n", total);
    fprintf(rf, "    \"distinct_edits\": %d,\n", distinct);
    fprintf(rf, "    \"unchanged\": %d,\n", same);
    fprintf(rf, "    \"different\": %d,\n", differ);
    fprintf(rf, "    \"errors\": %d,\n", errors);
    fprintf(rf, "    \"skipped\": %d,\n", skipped);
    fprintf(rf, "    \"multi_instance\": %d,\n", multi_instance);
    fprintf(rf, "    \"loaded_with_no_module\": %d\n", no_module);
    fputs("  }", rf);
  }

  printf("[roundtrip]\n");
  g_hash_table_destroy(seen);
  printf("[roundtrip] round-tripped   : %d  (%d distinct, %d exact repeats reused)\n",
         total, distinct, total - distinct);
  printf("[roundtrip]   unchanged     : %d\n", same);
  printf("[roundtrip]   DIFFERENT     : %d\n", differ);
  printf("[roundtrip]   errors        : %d\n", errors);
  printf("[roundtrip]   skipped       : %d  (already flexi, or unreconstructable)\n",
         skipped);
  printf("[roundtrip]   of those, multi-instance (multi_priority > 0) : %d\n",
         multi_instance);
  printf("[roundtrip]   loaded with NO module at all (would pass vacuously) : %d\n",
         no_module);

  return passed;
}

gboolean dt_masks_roundtrip_harvest(const char *json_path, const char *report_path)
{
  return dt_masks_harvest_report(json_path, report_path, "roundtrip",
                                 dt_masks_roundtrip_harvest_section);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
