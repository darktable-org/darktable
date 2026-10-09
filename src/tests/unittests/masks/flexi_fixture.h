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

#pragma once

// Mock environment for the flexi masks panel's model layer.
//
// The panel's group model -- which elements each group holds, what each
// group's operator is, what is selected -- is a pure structure: a tree of
// dt_masks_form_t of type DT_MASKS_GROUP, each group's `points` list holding
// its marker first (see DT_MASKS_STATE_GROUP_MARKER), then its members
// bottom-up. No GTK widget is involved in any of it. The only global the
// model reaches for is darktable.develop, and only to resolve a formid to a
// form via dt_masks_get_from_id -- which just walks dev->forms.
//
// So the whole mock is: a dt_develop_t holding a forms list, an iop module
// pointing at it, and a blend_data for the panel's own scratch state. No
// gtk_init, no display, no database, no pixelpipe.
//
// LAYOUT STRINGS
//
// Building points lists by hand makes tests unreadable, so scenarios are
// written as layout strings that mirror what the panel shows, the way
// dev-doc/masks_data_model.md writes a tree:
//
//     "u{1,2,i{3,4}}"
//
// is the mask, a maximum group holding elements 1 and 2 (1 at the bottom) and
// on top of them a minimum group holding 3 and 4. A group's letter is its
// flexi operator: u = maximum, o = screen, s = sum, i = minimum, m = product,
// d = difference, x = exclusion. "{}" is a group with no members. A trailing
// "~" inverts a group or an element, and "@0.5" sets its opacity; on a
// nested group both may also follow its "}", for the settings of the member
// that refers to it.
//
// flexi_build() turns such a string into a live mask, giving the n-th group
// of the string, in reading order, the marker id FLEXI_GID(n) and, past the
// mask's own, the form id FLEXI_SUB(n); flexi_layout() writes a live mask
// back. A test is then a round trip through the model:
//
//     flexi_build("u{1,2,i{3}}");
//     dt_masks_model_drop_element_onto_element(mod, grp, 1, 3, TRUE);
//     assert_layout("u{2,i{3,1}}");

#include "common/darktable.h"
#include "develop/blend.h"
#include "develop/blend_gui_internal.h"
#include "develop/imageop.h"
#include "develop/masks.h"

#include <glib.h>

// the form id of the mask flexi_build() makes
#define FLEXI_MASK_ID ((dt_mask_id_t)1000)
// the marker id flexi_build() gives the n-th group of its layout string
#define FLEXI_GID(n) ((dt_mask_id_t)(900 + (n)))
// the form id of the n-th group, for n > 0: group 0 is the mask, FLEXI_MASK_ID
#define FLEXI_SUB(n) ((dt_mask_id_t)(800 + (n)))

// the fixture's live objects, valid between flexi_build() and flexi_teardown()
extern dt_develop_t flexi_dev;
extern dt_iop_module_t flexi_module;
extern dt_iop_gui_blend_data_t flexi_bd;
extern dt_develop_blend_params_t flexi_bp;

/** build a mask from a layout string; returns its group. */
dt_masks_form_t *flexi_build(const char *layout);

/** build a classic mask: a group with no marker, whose members each carry
    their own operator. "1,2,i3,d4" is elements 1 and 2 in union, then 3
    intersected and 4 subtracted; the letters are classic's u(nion),
    i(ntersection), d(ifference), x = e(x)clusion and s(um), union where there
    is none */
dt_masks_form_t *flexi_build_classic(const char *members);

/** the mask built by the last flexi_build() */
dt_masks_form_t *flexi_group(void);

/** the current mask as a layout string. Caller frees. */
char *flexi_layout(void);

/** group form `g` as a layout string; a classic list, which has no marker, as
    its members with their operators, in braces. Caller frees. */
char *flexi_tree_of(const dt_masks_form_t *g);
/** cmocka assertion: the tree of group form `g` equals `expect`. */
void flexi_assert_tree_(const dt_masks_form_t *g,
                        const char *expect,
                        const char *file,
                        const int line);
#define assert_tree(g, expect) flexi_assert_tree_((g), (expect), __FILE__, __LINE__)
/** cmocka assertion: the current mask's layout equals `expect`. */
#define assert_layout(expect) flexi_assert_tree_(flexi_group(), (expect), __FILE__, __LINE__)

/** remember `ord` as the displayed number of group `cid` */
void flexi_set_ordinal(const dt_mask_id_t cid, const int ord);
/** the remembered number for `cid`, or 0 */
int flexi_get_ordinal(const dt_mask_id_t cid);

/** bring up a scratch darktable.conf backed by a temp file, so tests can
    exercise code that reads panel preferences. Opt-in: only the suites that
    need it call this, and it is torn down by flexi_conf_cleanup(). */
void flexi_conf_init(void);
void flexi_conf_cleanup(void);

/** free everything the fixture allocated. Safe to call twice. */
void flexi_teardown(void);

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
