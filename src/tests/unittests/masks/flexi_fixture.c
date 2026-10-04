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

#include "flexi_fixture.h"
#include "control/conf.h"

// defined in the generated conf_gen.h, compiled into lib_darktable
extern void dt_confgen_init(void);

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <cmocka.h>

#include <stdio.h>
#include <string.h>

dt_develop_t flexi_dev;
dt_iop_module_t flexi_module;
dt_iop_gui_blend_data_t flexi_bd;
dt_develop_blend_params_t flexi_bp;

static dt_masks_form_t *_grp = NULL;

static dt_masks_state_t _flexi_op_from_letter(const char c)
{
  switch(c)
  {
    case 'u': return 0;
    case 'o': return DT_MASKS_STATE_FLEXI_SCREEN;
    case 's': return DT_MASKS_STATE_FLEXI_SUM;
    case 'i': return DT_MASKS_STATE_FLEXI_MINIMUM;
    case 'm': return DT_MASKS_STATE_FLEXI_PRODUCT;
    case 'd': return DT_MASKS_STATE_FLEXI_DIFFERENCE;
    case 'x': return DT_MASKS_STATE_FLEXI_EXCLUSION;
    default: fail_msg("unknown group letter '%c' in layout string", c);
  }
  return 0; // unreachable; keeps the compiler quiet
}

static char _letter_from_flexi_op(const int state)
{
  switch(state & DT_MASKS_STATE_FLEXI_OP)
  {
    case 0: return 'u';
    case DT_MASKS_STATE_FLEXI_SCREEN: return 'o';
    case DT_MASKS_STATE_FLEXI_SUM: return 's';
    case DT_MASKS_STATE_FLEXI_MINIMUM: return 'i';
    case DT_MASKS_STATE_FLEXI_PRODUCT: return 'm';
    case DT_MASKS_STATE_FLEXI_DIFFERENCE: return 'd';
    case DT_MASKS_STATE_FLEXI_EXCLUSION: return 'x';
    default: return '?';
  }
}

static dt_masks_state_t _classic_op_from_letter(const char c)
{
  switch(c)
  {
    case 'u': return DT_MASKS_STATE_UNION;
    case 'i': return DT_MASKS_STATE_INTERSECTION;
    case 'd': return DT_MASKS_STATE_DIFFERENCE;
    case 'x': return DT_MASKS_STATE_EXCLUSION;
    case 's': return DT_MASKS_STATE_SUM;
    default: fail_msg("unknown classic operator letter '%c'", c);
  }
  return DT_MASKS_STATE_UNION; // unreachable; keeps the compiler quiet
}

static char _letter_from_classic_op(const int state)
{
  switch(state & DT_MASKS_STATE_OP_COMBINE)
  {
    case DT_MASKS_STATE_INTERSECTION: return 'i';
    case DT_MASKS_STATE_DIFFERENCE: return 'd';
    case DT_MASKS_STATE_EXCLUSION: return 'x';
    case DT_MASKS_STATE_SUM: return 's';
    default: return 0; // union, or none
  }
}

// a minimal shape form so dt_masks_get_from_id() resolves this element. Type
// matters only where the model distinguishes shapes from parametric elements;
// tests that need a parametric element set the type themselves afterwards.
static void _add_form(const dt_mask_id_t fid)
{
  if(dt_masks_get_from_id_ext(flexi_dev.forms, fid)) return;
  dt_masks_form_t *f = calloc(1, sizeof(dt_masks_form_t));
  f->formid = fid;
  f->type = DT_MASKS_CIRCLE;
  snprintf(f->name, sizeof(f->name), "circle #%d", (int)fid);
  flexi_dev.forms = g_list_append(flexi_dev.forms, f);
}

static dt_masks_point_group_t *_new_point(const dt_mask_id_t formid, const dt_mask_id_t parent)
{
  dt_masks_point_group_t *pt = calloc(1, sizeof(dt_masks_point_group_t));
  pt->formid = formid;
  pt->parentid = parent;
  pt->state = DT_MASKS_STATE_USE | DT_MASKS_STATE_SHOW;
  pt->opacity = 1.0f;
  pt->group_opacity = 1.0f;
  return pt;
}

typedef struct _parser_t
{
  const char *p;
  int groups; // groups read so far
} _parser_t;

// "~" and "@<opacity>", in either order
static void _parse_settings(_parser_t *ps, gboolean *inverted, float *opacity)
{
  for(;;)
  {
    if(*ps->p == '~')
    {
      *inverted = TRUE;
      ps->p++;
    }
    else if(*ps->p == '@')
    {
      char *end = NULL;
      *opacity = strtof(ps->p + 1, &end);
      ps->p = end;
    }
    else
      return;
  }
}

static void _parse_group(_parser_t *ps, dt_masks_form_t *form)
{
  const dt_masks_state_t flexi_op = _flexi_op_from_letter(*ps->p++);
  dt_masks_point_group_t *marker = _new_point(FLEXI_GID(ps->groups++), form->formid);
  marker->state = DT_MASKS_STATE_GROUP_MARKER | flexi_op;
  gboolean inverted = FALSE;
  _parse_settings(ps, &inverted, &marker->group_opacity);
  if(inverted) marker->state |= DT_MASKS_STATE_OP_INVERT;
  form->points = g_list_append(form->points, marker);

  if(*ps->p++ != '{') fail_msg("expected '{' in layout string");
  while(*ps->p && *ps->p != '}')
  {
    dt_masks_point_group_t *pt;
    if(g_ascii_isdigit(*ps->p))
    {
      char *end = NULL;
      const dt_mask_id_t fid = (dt_mask_id_t)strtol(ps->p, &end, 10);
      ps->p = end;
      _add_form(fid);
      pt = _new_point(fid, form->formid);
    }
    else
    {
      dt_masks_form_t *sub = calloc(1, sizeof(dt_masks_form_t));
      sub->formid = FLEXI_SUB(ps->groups);
      sub->type = DT_MASKS_GROUP;
      snprintf(sub->name, sizeof(sub->name), "group #%d", ps->groups);
      flexi_dev.forms = g_list_append(flexi_dev.forms, sub);
      _parse_group(ps, sub);
      pt = _new_point(sub->formid, form->formid);
    }
    inverted = FALSE;
    _parse_settings(ps, &inverted, &pt->opacity);
    if(inverted) pt->state |= DT_MASKS_STATE_INVERSE;
    form->points = g_list_append(form->points, pt);
    if(*ps->p == ',') ps->p++;
  }
  if(*ps->p++ != '}') fail_msg("expected '}' in layout string");
}

// a fresh fixture holding the mask's group form, with no points yet
static dt_masks_form_t *_start(void)
{
  flexi_teardown();

  memset(&flexi_dev, 0, sizeof(flexi_dev));
  memset(&flexi_module, 0, sizeof(flexi_module));
  memset(&flexi_bd, 0, sizeof(flexi_bd));
  // as dt_iop_gui_init_blending does: the refine-bypass snapshot takes it
  dt_pthread_mutex_init(&flexi_bd.lock);
  memset(&flexi_bp, 0, sizeof(flexi_bp));

  _grp = calloc(1, sizeof(dt_masks_form_t));
  _grp->formid = FLEXI_MASK_ID;
  _grp->type = DT_MASKS_GROUP;
  flexi_dev.forms = g_list_append(NULL, _grp);

  flexi_bp.mask_id = FLEXI_MASK_ID;
  flexi_bp.mask_mode = DEVELOP_MASK_ENABLED | DEVELOP_MASK_FLEXI;
  flexi_module.blend_params = &flexi_bp;
  flexi_module.blend_data = &flexi_bd;
  flexi_module.dev = &flexi_dev;
  flexi_bd.module = &flexi_module;
  // every mask-id field starts INVALID, not zero -- a zeroed blend_data would
  // read as "element 0 is soloed" (see the matching initialisation in
  // blend_gui.c's panel setup)
  flexi_bd.panel_selected_formid = INVALID_MASKID;
  flexi_bd.panel_selected_group_cid = INVALID_MASKID;
  flexi_bd.solo_formid = INVALID_MASKID;
  flexi_bd.soloedit_formid = INVALID_MASKID;
  flexi_bd.solo_group_key = 0;

  darktable.develop = &flexi_dev;
  return _grp;
}

dt_masks_form_t *flexi_build(const char *layout)
{
  dt_masks_form_t *grp = _start();
  _parser_t ps = { layout, 0 };
  _parse_group(&ps, grp);
  if(*ps.p) fail_msg("trailing text in layout string: %s", ps.p);
  return grp;
}

dt_masks_form_t *flexi_build_classic(const char *members)
{
  dt_masks_form_t *grp = _start();
  gchar **tok = g_strsplit(members, ",", -1);
  for(int k = 0; tok[k]; k++)
  {
    const char *t = g_strstrip(tok[k]);
    if(!*t) continue;
    const dt_masks_state_t op =
      g_ascii_isdigit(*t) ? DT_MASKS_STATE_UNION : _classic_op_from_letter(*t++);
    const dt_mask_id_t fid = (dt_mask_id_t)atoi(t);
    _add_form(fid);
    dt_masks_point_group_t *pt = _new_point(fid, grp->formid);
    pt->state |= op;
    grp->points = g_list_append(grp->points, pt);
  }
  g_strfreev(tok);
  return grp;
}

dt_masks_form_t *flexi_group(void)
{
  return _grp;
}

char *flexi_layout(void)
{
  return flexi_tree_of(_grp);
}

static void _append_settings(GString *s, const gboolean inverted, const float opacity)
{
  if(inverted) g_string_append_c(s, '~');
  if(opacity != 1.0f) g_string_append_printf(s, "@%g", opacity);
}

static void _tree_into(GString *s, const dt_masks_form_t *g, const int depth)
{
  const GList *l = g->points;
  const dt_masks_point_group_t *mk = l && dt_masks_point_is_marker(l->data) ? l->data : NULL;
  if(mk)
  {
    g_string_append_c(s, _letter_from_flexi_op(mk->state));
    _append_settings(s, (mk->state & DT_MASKS_STATE_OP_INVERT) != 0, mk->group_opacity);
    l = g_list_next(l);
  }
  g_string_append_c(s, '{');
  for(gboolean first = TRUE; l; l = g_list_next(l), first = FALSE)
  {
    const dt_masks_point_group_t *pt = l->data;
    if(!first) g_string_append_c(s, ',');
    if(dt_masks_point_is_marker(pt))
    {
      // not a state the model allows: shown, so that a test says so
      g_string_append(s, "MARKER");
      continue;
    }
    const char classic = mk ? 0 : _letter_from_classic_op(pt->state);
    if(classic) g_string_append_c(s, classic);
    const dt_masks_form_t *f = dt_masks_get_from_id_ext(flexi_dev.forms, pt->formid);
    if(f && f != g && (f->type & DT_MASKS_GROUP) && depth < DT_MASKS_NESTING_MAX)
      _tree_into(s, f, depth + 1);
    else
      g_string_append_printf(s, "%d", (int)pt->formid);
    _append_settings(s, (pt->state & DT_MASKS_STATE_INVERSE) != 0, pt->opacity);
  }
  g_string_append_c(s, '}');
}

char *flexi_tree_of(const dt_masks_form_t *g)
{
  GString *s = g_string_new(NULL);
  if(g) _tree_into(s, g, 0);
  return g_string_free(s, FALSE);
}

void flexi_assert_tree_(const dt_masks_form_t *g,
                        const char *expect,
                        const char *file,
                        const int line)
{
  char *got = flexi_tree_of(g);
  if(strcmp(got, expect) != 0)
  {
    // print both before failing: cmocka's string diff alone is hard to read
    // for these, and the layout is the whole point of the assertion
    print_error("%s:%d: tree mismatch\n  expected: %s\n  actual:   %s\n", file, line, expect,
                got);
    g_free(got);
    fail();
  }
  g_free(got);
}

void flexi_set_ordinal(const dt_mask_id_t cid, const int ord)
{
  if(!flexi_bd.group_ordinals)
    flexi_bd.group_ordinals = g_hash_table_new(g_direct_hash, g_direct_equal);
  g_hash_table_insert(flexi_bd.group_ordinals, GINT_TO_POINTER(cid),
                      GINT_TO_POINTER(ord));
}

int flexi_get_ordinal(const dt_mask_id_t cid)
{
  if(!flexi_bd.group_ordinals) return 0;
  return GPOINTER_TO_INT(
    g_hash_table_lookup(flexi_bd.group_ordinals, GINT_TO_POINTER(cid)));
}

static gchar *_conf_path = NULL;

void flexi_conf_init(void)
{
  if(darktable.conf) return;
  _conf_path = g_build_filename(g_get_tmp_dir(), "flexi_unittest_rc", NULL);
  // start from a clean slate every run, so one test's writes cannot leak into
  // the next run's expectations
  g_unlink(_conf_path);
  darktable.conf = calloc(1, sizeof(dt_conf_t));
  // the defaults/min/max table, generated from darktableconfig.xml into
  // conf_gen.h and compiled into lib_darktable. dt_conf_init sanitizes values
  // against it and dt_conf_get_* falls back to it for unset keys, so without
  // this every lookup hits a NULL table.
  dt_confgen_init();
  dt_conf_init(darktable.conf, _conf_path, FALSE, NULL);
}

void flexi_conf_cleanup(void)
{
  if(!darktable.conf) return;
  dt_conf_cleanup(darktable.conf);
  free(darktable.conf);
  darktable.conf = NULL;
  if(_conf_path)
  {
    g_unlink(_conf_path);
    g_free(_conf_path);
    _conf_path = NULL;
  }
}

void flexi_teardown(void)
{
  // every form the fixture or the code under test registered, nested groups'
  // points with them
  for(GList *l = _grp ? flexi_dev.forms : NULL; l; l = g_list_next(l))
  {
    dt_masks_form_t *f = l->data;
    if(f->type & (DT_MASKS_GROUP | DT_MASKS_OBJECT)) g_list_free_full(f->points, free);
    free(f);
  }
  if(_grp) g_list_free(flexi_dev.forms);
  flexi_dev.forms = NULL;
  _grp = NULL;

  if(flexi_bd.group_ordinals)
  {
    g_hash_table_destroy(flexi_bd.group_ordinals);
    flexi_bd.group_ordinals = NULL;
  }

  darktable.develop = NULL;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
