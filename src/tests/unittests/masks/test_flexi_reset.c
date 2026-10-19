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

/* What a module reset does to its mask (dt_iop_reset_params, the reset
   button's work), on real modules of a headless darktable.

   A reset drops the mask unless it is locked. dt_iop_reload_defaults() commits
   the default blend params on its own, lock included, so the reset has to read
   the lock from before it: reading it after made every locked mask go. Real
   modules, as that is where reload_defaults does its work, and both kinds of
   mask_id: an ordinary module's mask, and retouch's own shapes beside its
   mask (blend_mask_id).

   Removing forms updates the module's GUI (dt_masks_iop_update), which a
   headless darktable has none of, so no test here lets the reset remove one:
   a locked mask keeps its forms, and the others name none */

#include "../catalog/catalog_fixture.h"
#include "common/darktable.h"
#include "develop/blend.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/masks.h"

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <cmocka.h>

static dt_develop_t _dev;
static dt_develop_t *_saved_develop;

static int _setup(void **state)
{
  if(dt_test_catalog_setup(state)) return -1;
  const dt_imgid_t imgid = dt_test_catalog_image("mire1.cr2");
  if(!dt_is_valid_imgid(imgid)) return -1;

  // as dt_masks_scratch_open does: dt_dev_init leaves the modules unloaded,
  // and the forms code reaches the dev through darktable.develop
  dt_dev_init(&_dev, FALSE);
  _dev.iop = dt_iop_load_modules(&_dev);
  _dev.image_storage.id = imgid;
  dt_dev_read_history_ext(&_dev, imgid, TRUE);
  _saved_develop = darktable.develop;
  darktable.develop = &_dev;
  return 0;
}

static int _teardown(void **state)
{
  darktable.develop = _saved_develop;
  dt_dev_cleanup(&_dev);
  return dt_test_catalog_teardown(state);
}

static dt_iop_module_t *_module(const char *op)
{
  for(GList *l = _dev.iop; l; l = g_list_next(l))
  {
    dt_iop_module_t *m = l->data;
    if(dt_iop_module_is(m, op)) return m;
  }
  fail_msg("module %s not loaded", op);
  return NULL;
}

// a flexi mask holding one parametric element, in the group the module's
// blend mask renders (dt_develop_blend_mask_group_id)
static dt_mask_id_t _give_mask(dt_iop_module_t *m, const gboolean locked)
{
  dt_masks_form_t *grp = dt_masks_module_group_create(&_dev, m);
  dt_masks_form_t *param = dt_masks_create(DT_MASKS_PARAMETRIC);
  _dev.forms = g_list_append(_dev.forms, param);
  dt_masks_group_insert_point(&_dev, m, param);
  m->blend_params->mask_mode = DEVELOP_MASK_ENABLED | DEVELOP_MASK_FLEXI;
  m->blend_params->mask_lock = locked ? 1 : 0;
  assert_int_equal(dt_develop_blend_mask_group_id(m, m->blend_params), grp->formid);
  return grp->formid;
}

static gboolean _has_form(const dt_mask_id_t id)
{
  return dt_masks_get_from_id(&_dev, id) != NULL;
}

static void test_reset_keeps_a_locked_mask(void **state)
{
  dt_iop_module_t *m = _module("exposure");
  const dt_mask_id_t grid = _give_mask(m, TRUE);

  dt_iop_reset_params(m);

  assert_true(dt_develop_blend_mask_locked(m->blend_params));
  assert_int_equal(m->blend_params->mask_mode, DEVELOP_MASK_ENABLED | DEVELOP_MASK_FLEXI);
  assert_int_equal(m->blend_params->mask_id, grid);
  assert_true(_has_form(grid));
}

// an unlocked mask goes: a uniform one, which has no form to remove
static void test_reset_drops_an_unlocked_mask(void **state)
{
  dt_iop_module_t *m = _module("colorbalancergb");
  m->blend_params->mask_mode = DEVELOP_MASK_ENABLED | DEVELOP_MASK_FLEXI;
  m->blend_params->mask_id = NO_MASKID;
  m->blend_params->mask_lock = 0;

  dt_iop_reset_params(m);

  assert_false(dt_develop_blend_mask_locked(m->blend_params));
  assert_int_equal(m->blend_params->mask_mode, DEVELOP_MASK_DISABLED);
}

// retouch's own shapes are its params, not its mask: they go whatever the
// lock, while the locked mask, in blend_mask_id, stays. The shapes' id names
// no form, so the reset has nothing to remove (see the file header)
static void test_reset_of_retouch_keeps_its_locked_mask_only(void **state)
{
  dt_iop_module_t *m = _module("retouch");
  const dt_mask_id_t own_id = 424242;
  assert_false(_has_form(own_id));
  m->blend_params->mask_id = own_id;
  const dt_mask_id_t grid = _give_mask(m, TRUE);

  dt_iop_reset_params(m);

  assert_true(dt_develop_blend_mask_locked(m->blend_params));
  assert_int_equal(m->blend_params->mask_mode, DEVELOP_MASK_ENABLED | DEVELOP_MASK_FLEXI);
  assert_int_equal(m->blend_params->blend_mask_id, grid);
  assert_true(_has_form(grid));
  assert_int_not_equal(m->blend_params->mask_id, own_id);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_reset_keeps_a_locked_mask),
    cmocka_unit_test(test_reset_drops_an_unlocked_mask),
    cmocka_unit_test(test_reset_of_retouch_keeps_its_locked_mask_only),
  };
  return cmocka_run_group_tests(tests, _setup, _teardown);
}
