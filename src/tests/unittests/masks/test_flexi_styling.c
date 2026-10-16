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

// The masks panel's styling contract (dev-doc/flexi_masks/styling.md), read
// from the sources as text, so it needs no display:
// - every class the page documents is set by the panel and styled by the theme
// - no theme rule aimed at one of those parts uses an id, which is what lets a
//   CSS tweak starting with #masks-panel always win

#include <glib.h>
#include <string.h>

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <cmocka.h>

// the parts and their states, and the hooks the panel sets and documents for
// a theme without the default theme styling them: the cluster elements are
// only ever context, and the open and joined states are there for a theme
// with rounded corners
static const char *_parts[] =
{
  "dt_masks_lead", "dt_masks_drawer", "dt_masks_icon", "dt_masks_expander", "dt_masks_eye",
  "dt_masks_notes", "dt_masks_picker", "dt_masks_link", "dt_masks_badge",
  "dt_masks_channel_eye", "dt_masks_row", "dt_masks_header", "dt_masks_element_header",
  "dt_masks_group_header", "dt_masks_cluster_header", "dt_masks_row_name",
  "dt_masks_group_block", "dt_masks_group_elements", "dt_masks_card", "dt_masks_param_card",
  "dt_masks_props_card", "dt_masks_group_card", "dt_masks_note",
};
static const char *_states[] =
{
  "dt_masks_inverted", "dt_masks_channel", "dt_masks_soloed", "dt_masks_disabled",
  "dt_masks_no_effect", "dt_masks_selected", "dt_masks_implied", "dt_masks_hovered",
  "dt_masks_drop_target", "dt_masks_root", "dt_masks_pending",
};
static const char *_hooks[] =
{
  "dt_masks_cluster_elements", "dt_masks_open", "dt_masks_has_card", "dt_masks_joined",
};

static gchar *_read(const char *rel)
{
  gchar *path = g_build_filename(DT_SOURCE_DIR, rel, NULL);
  gchar *text = NULL;
  if(!g_file_get_contents(path, &text, NULL, NULL)) fail_msg("cannot read %s", path);
  g_free(path);
  return text;
}

// `.name` as a whole class in a selector or in prose: not the head of a
// longer name like .dt_masks_channel_eye for .dt_masks_channel
static gboolean _has_class(const char *text, const char *name)
{
  gchar *pattern = g_strdup_printf("\\.%s(?![a-z_-])", name);
  const gboolean found = g_regex_match_simple(pattern, text, 0, 0);
  g_free(pattern);
  return found;
}

static gboolean _code_sets(const char *code, const char *name)
{
  gchar *quoted = g_strdup_printf("\"%s\"", name);
  const gboolean found = strstr(code, quoted) != NULL;
  g_free(quoted);
  return found;
}

static void _check_all(const char *const *names, const int n, const gboolean styled)
{
  gchar *code = _read("src/develop/blend_gui.c");
  gchar *css = _read("data/themes/darktable.css");
  gchar *doc = _read("dev-doc/flexi_masks/styling.md");
  for(int i = 0; i < n; i++)
  {
    if(!_code_sets(code, names[i])) fail_msg("the panel never sets .%s", names[i]);
    if(styled && !_has_class(css, names[i])) fail_msg("the theme never styles .%s", names[i]);
    if(!_has_class(doc, names[i])) fail_msg("styling.md does not document .%s", names[i]);
  }
  g_free(code);
  g_free(css);
  g_free(doc);
}

static void test_every_part_is_set_styled_and_documented(void **state)
{
  _check_all(_parts, G_N_ELEMENTS(_parts), TRUE);
}

static void test_every_state_is_set_styled_and_documented(void **state)
{
  // .dt_masks_disabled is set for user CSS only: the theme lights a disabled
  // eye through the same rule as a soloed one, which names both
  _check_all(_states, G_N_ELEMENTS(_states), TRUE);
}

static void test_every_hook_is_set_and_documented(void **state)
{
  _check_all(_hooks, G_N_ELEMENTS(_hooks), FALSE);
}

// the last compound selector of `sel`: what the rule actually styles
static const char *_subject(const char *sel)
{
  const char *last = sel;
  for(const char *c = sel; *c; c++)
    if(*c == ' ' || *c == '>' || *c == '+' || *c == '~') last = c + 1;
  return last;
}

static void test_no_theme_rule_on_a_part_uses_an_id(void **state)
{
  gchar *css = _read("data/themes/darktable.css");
  GRegex *comments = g_regex_new("/\\*.*?\\*/", G_REGEX_DOTALL, 0, NULL);
  gchar *bare = g_regex_replace_literal(comments, css, -1, 0, "", 0, NULL);
  g_regex_unref(comments);

  // every rule's selector list, up to its opening brace
  gchar **blocks = g_strsplit(bare, "}", -1);
  int aimed = 0;
  for(gchar **b = blocks; *b; b++)
  {
    gchar *brace = strchr(*b, '{');
    if(!brace) continue;
    *brace = '\0';
    gchar **sels = g_strsplit(*b, ",", -1);
    for(gchar **s = sels; *s; s++)
    {
      gchar *sel = g_strstrip(*s);
      const char *subject = _subject(sel);
      for(int i = 0; i < G_N_ELEMENTS(_parts); i++)
        if(_has_class(subject, _parts[i]))
        {
          aimed++;
          if(strchr(sel, '#'))
            fail_msg("\"%s\" styles .%s with an id, which a CSS tweak starting with"
                     " #masks-panel can tie or lose to", sel, _parts[i]);
        }
    }
    g_strfreev(sels);
  }
  g_strfreev(blocks);
  g_free(bare);
  g_free(css);
  // the parse found the rules at all
  assert_true(aimed >= G_N_ELEMENTS(_parts));
}

int main(void)
{
  const struct CMUnitTest tests[] =
  {
    cmocka_unit_test(test_every_part_is_set_styled_and_documented),
    cmocka_unit_test(test_every_state_is_set_styled_and_documented),
    cmocka_unit_test(test_every_hook_is_set_and_documented),
    cmocka_unit_test(test_no_theme_rule_on_a_part_uses_an_id),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
