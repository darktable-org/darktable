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

#include "develop/masks/lockcheck.h"

#include "common/darktable.h"
#include "common/debug.h"
#include "common/history.h"
#include "common/styles.h"
#include "develop/blend.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/masks.h"
#include "develop/masks/scratch_image.h"

#include <stdio.h>

// the image pasted onto, and the one pasted from
#define LOCK_DEST 1
#define LOCK_SRC 2

// the destination's mask and the source's, told apart by their ids and by a
// refinement value each; opacity is blending, which the lock leaves alone
#define DEST_GROUP 100
#define SRC_GROUP 200
#define DEST_FEATHER 5.0f
#define SRC_FEATHER 9.0f
#define DEST_OPACITY 50.0f
#define SRC_OPACITY 80.0f

#define LOCK_STYLE "masks lock check"

// a group holding one circle, both under the given ids
static GList *_mask_forms(const dt_mask_id_t group_id)
{
  dt_masks_form_t *circle = dt_masks_create(DT_MASKS_CIRCLE);
  circle->formid = group_id + 1;
  dt_masks_point_circle_t *c = calloc(1, sizeof(dt_masks_point_circle_t));
  c->center[0] = c->center[1] = 0.5f;
  c->radius = 0.1f;
  c->border = 0.05f;
  circle->points = g_list_append(NULL, c);

  dt_masks_form_t *group = dt_masks_create(DT_MASKS_GROUP);
  group->formid = group_id;
  dt_masks_point_group_t *m = calloc(1, sizeof(dt_masks_point_group_t));
  m->formid = circle->formid;
  m->parentid = group_id;
  m->state = DT_MASKS_STATE_SHOW | DT_MASKS_STATE_USE;
  m->opacity = 1.0f;
  m->group_opacity = 1.0f;
  group->points = g_list_append(NULL, m);

  return g_list_append(g_list_append(NULL, group), circle);
}

// `also` is a mask written with this one: every masks history item carries
// the image's whole set of forms, not only its own module's
static void _seed(const dt_imgid_t imgid,
                  const int num,
                  const char *operation,
                  const dt_mask_id_t group_id,
                  const float feather,
                  const float opacity,
                  const gboolean locked,
                  const dt_mask_id_t also)
{
  dt_develop_blend_params_t bp;
  dt_develop_blend_init_blend_parameters(&bp, DEVELOP_BLEND_CS_RGB_SCENE);
  bp.mask_mode = DEVELOP_MASK_ENABLED | DEVELOP_MASK_FLEXI;
  bp.mask_id = group_id;
  bp.feathering_radius = feather;
  bp.opacity = opacity;
  bp.mask_lock = locked ? 1 : 0;

  GList *forms = _mask_forms(group_id);
  if(also) forms = g_list_concat(forms, _mask_forms(also));
  dt_masks_scratch_seed_history(imgid, num, operation, 0, dt_develop_blend_version(),
                                &bp, forms);
  g_list_free_full(forms, (GDestroyNotify)dt_masks_free_form);
}

static void _set_history_end(const dt_imgid_t imgid, const int end)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "UPDATE main.images SET history_end = ?2 WHERE id = ?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, end);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

// the destination: exposure with the mask the checks look for. The source:
// exposure with another mask at num 0, and sharpen at num 1, a module the
// destination does not have
static void _setup(const gboolean dest_locked, const gboolean src_locked)
{
  dt_masks_scratch_wipe_history(LOCK_DEST);
  dt_masks_scratch_wipe_history(LOCK_SRC);
  dt_masks_scratch_seed_image(LOCK_DEST, 4000, 3000);
  dt_masks_scratch_seed_image(LOCK_SRC, 4000, 3000);

  _seed(LOCK_DEST, 0, "exposure", DEST_GROUP, DEST_FEATHER, DEST_OPACITY, dest_locked, 0);
  _seed(LOCK_SRC, 0, "exposure", SRC_GROUP, SRC_FEATHER, SRC_OPACITY, src_locked, 0);
  _seed(LOCK_SRC, 1, "sharpen", SRC_GROUP + 10, SRC_FEATHER, SRC_OPACITY, FALSE,
        SRC_GROUP);
  _set_history_end(LOCK_SRC, 2);

  dt_styles_delete_by_name(LOCK_STYLE);
}

typedef struct _expect_t
{
  dt_mask_id_t mask_id;
  gboolean locked;
  float feather;
  float opacity;
  // a style carries no forms (data.style_items has no column for them), so
  // a mask a style brings along is only an id
  gboolean form;
} _expect_t;

// reads the destination back from the database and compares its exposure
static gboolean _check(const char *name, const _expect_t *want)
{
  dt_develop_t dev;
  dt_masks_scratch_open(&dev, LOCK_DEST);
  dt_dev_pop_history_items_ext(&dev, dev.history_end);

  const dt_iop_module_t *mod = dt_iop_get_module_by_op_priority(dev.iop, "exposure", 0);
  const dt_develop_blend_params_t *bp = mod ? mod->blend_params : NULL;
  const gboolean has_form =
    bp && dt_masks_get_from_id_ext(dev.forms, bp->mask_id) != NULL;

  const gboolean ok = bp
    && bp->mask_id == want->mask_id
    && dt_develop_blend_mask_locked(bp) == want->locked
    && bp->feathering_radius == want->feather
    && bp->opacity == want->opacity
    && has_form == want->form;

  printf("%s  %s", ok ? "PASS" : "FAIL", name);
  if(!ok)
  {
    if(bp)
      printf("  (mask_id %d lock %u feather %g opacity %g form %s;"
             " want mask_id %d lock %d feather %g opacity %g form %s)",
             bp->mask_id, bp->mask_lock, bp->feathering_radius, bp->opacity,
             has_form ? "found" : "missing",
             want->mask_id, want->locked, want->feather, want->opacity,
             want->form ? "found" : "missing");
    else
      printf("  (no exposure module on the destination)");
  }
  printf("\n");

  dt_dev_cleanup(&dev);
  return ok;
}

static void _paste(const gboolean merge, GList *ops)
{
  dt_history_copy_and_paste_on_image(LOCK_SRC, LOCK_DEST, merge, ops,
                                     FALSE, FALSE, FALSE);
}

static void _apply_style(void)
{
  dt_styles_create_from_image(LOCK_STYLE, "", LOCK_SRC, NULL, FALSE);
  dt_styles_apply_to_image(LOCK_STYLE, FALSE, FALSE, LOCK_DEST);
  dt_styles_delete_by_name(LOCK_STYLE);
}

gboolean dt_masks_lock_check(void)
{
  // what a locked destination keeps: its mask, and the lock. Only the
  // opacity differs between cases, taken from whatever replaced the module
  const _expect_t kept_pasted = { DEST_GROUP, TRUE, DEST_FEATHER, SRC_OPACITY, TRUE };
  const _expect_t kept_reset = { DEST_GROUP, TRUE, DEST_FEATHER, 100.0f, TRUE };
  const _expect_t replaced = { SRC_GROUP, FALSE, SRC_FEATHER, SRC_OPACITY, TRUE };
  const _expect_t replaced_by_style = { SRC_GROUP, FALSE, SRC_FEATHER, SRC_OPACITY, FALSE };

  gboolean ok = TRUE;
  GList *ops_exposure = g_list_append(NULL, GINT_TO_POINTER(0));
  GList *ops_sharpen = g_list_append(NULL, GINT_TO_POINTER(1));

  _setup(TRUE, FALSE);
  _paste(TRUE, NULL);
  ok &= _check("paste, append mode, onto a locked mask", &kept_pasted);

  _setup(TRUE, FALSE);
  _paste(FALSE, NULL);
  ok &= _check("paste, overwrite mode, onto a locked mask", &kept_pasted);

  _setup(TRUE, FALSE);
  _paste(FALSE, ops_exposure);
  ok &= _check("selective paste, overwrite mode, of the locked module", &kept_pasted);

  // overwrite resets every module it does not paste, so the locked one gets
  // what a module reset gives it: default blending, its own mask
  _setup(TRUE, FALSE);
  _paste(FALSE, ops_sharpen);
  ok &= _check("selective paste, overwrite mode, of another module", &kept_reset);

  _setup(TRUE, FALSE);
  _apply_style();
  ok &= _check("style onto a locked mask", &kept_pasted);

  // the controls: without a lock the mask is replaced, and a lock coming in
  // with the pasted or applied params does not lock the destination
  _setup(FALSE, FALSE);
  _paste(FALSE, ops_exposure);
  ok &= _check("selective paste, overwrite mode, onto an unlocked mask", &replaced);

  _setup(FALSE, TRUE);
  _apply_style();
  ok &= _check("style from a locked mask onto an unlocked one", &replaced_by_style);

  g_list_free(ops_exposure);
  g_list_free(ops_sharpen);

  printf("%s\n", ok ? "all lock checks passed" : "some lock checks FAILED");
  return ok;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
