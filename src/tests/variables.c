#include "common/darktable.h"
#include "common/database.h"
#include "common/variables.h"

#include <sqlite3.h>
#include <stdio.h>

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

typedef struct test_case_t
{
  char *input, *expected_result;
} test_case_t;

typedef struct test_t
{
  char *filename, *jobcode, sequence;
  test_case_t test_cases[];
} test_t;

// the two entry points differ only in separator handling, so the cases run
// through whichever one the caller passes
typedef char *(*expand_fn_t)(dt_variables_params_t *, gchar *, const gboolean);

int run_test(const test_t *test, int *n_tests, int *n_failed, expand_fn_t expand)
{
  dt_variables_params_t *params;
  dt_variables_params_init(&params);
  params->filename = test->filename;//"abcdef12345abcdef";
  params->jobcode = test->jobcode;//"ABCDEF12345ABCDEF";
  params->sequence = test->sequence;

  *n_failed = 0;
  *n_tests = 0;
  for(const test_case_t *test_case = test->test_cases; test_case->input; test_case++)
  {
    (*n_tests)++;
    char *result = expand(params, test_case->input, FALSE);
    if(g_strcmp0(result, test_case->expected_result))
    {
      (*n_failed)++;
      printf("  [FAIL] input: '%s', result: '%s', expected: '%s'\n", test_case->input, result, test_case->expected_result);
    }
    else
      printf("  [OK] input: '%s', result: '%s'\n", test_case->input, result);
  }

  dt_variables_params_destroy(params);

  return *n_failed > 0 ? 1 : 0;
}


static const test_t test_variables = {
  "abcdef12345abcdef", "ABCDEF12345ABCDEF", 23,
  {
    {"$(FILE_NAME)", "abcdef12345abcdef"},
    {"foo-$(FILE_NAME)-bar", "foo-abcdef12345abcdef-bar"},
    {"äöü-$(FILE_NAME)-äöü", "äöü-abcdef12345abcdef-äöü"},
    {"$(FILE_NAME).$(SEQUENCE)", "abcdef12345abcdef.0023"},
    {"$(NONEXISTANT)", ""},
    {"foo-$(NONEXISTANT)-bar", "foo--bar"},

    {NULL, NULL}
  }
};

static const test_t test_simple_substitutions = {
  "abcdef12345abcdef", "ABCDEF12345ABCDEF", 23,
  {
    {"$(NONEXISTANT-invälid)", "invälid"},
    {"$(FILE_NAME-invälid)", "abcdef12345abcdef"},

    {"$(NONEXISTANT+exißts)", ""},
    {"$(FILE_NAME+exißts)", "exißts"},

    {"$(NONEXISTANT:0)", ""},
    {"$(FILE_NAME:0)", "abcdef12345abcdef"},
    {"$(FILE_NAME:5)", "f12345abcdef"},
    {"$(FILE_NAME:42)", ""},
    {"$(FILE_NAME:-5)", "bcdef"},
    {"$(FILE_NAME:-42)", "abcdef12345abcdef"},
    {"$(FILE_NAME:0:5)", "abcde"},
    {"$(FILE_NAME:5:3)", "f12"},
    {"$(FILE_NAME:5:42)", "f12345abcdef"},
    {"$(FILE_NAME:-5:3)", "bcd"},
    {"$(FILE_NAME:-7:-2)", "5abcd"},
    {"$(FILE_NAME:)", "abcdef12345abcdef"},
    {"$(FILE_NAME:5:)", ""},

    {"$(NONEXISTANT#abc)", ""},
    {"$(FILE_NAME#abc)", "def12345abcdef"},
    {"$(FILE_NAME#def)", "abcdef12345abcdef"},

    {"$(NONEXISTANT%abc)", ""},
    {"$(FILE_NAME%abc)", "abcdef12345abcdef"},
    {"$(FILE_NAME%def)", "abcdef12345abc"},

    {"$(NONEXISTANT/abc/def)", ""},
    {"$(FILE_NAME/abc/foobar)", "foobardef12345abcdef"},
    {"$(FILE_NAME/def/foobar)", "abcfoobar12345abcdef"},
    {"$(FILE_NAME//abc/foobar)", "foobardef12345foobardef"},
    {"$(FILE_NAME//def/foobar)", "abcfoobar12345abcfoobar"},
    {"$(FILE_NAME/#abc/foobar)", "foobardef12345abcdef"},
    {"$(FILE_NAME/#def/foobar)", "abcdef12345abcdef"},
    {"$(FILE_NAME/%abc/foobar)", "abcdef12345abcdef"},
    {"$(FILE_NAME/%def/foobar)", "abcdef12345abcfoobar"},

    {"$(NONEXISTANT^)", ""},
    {"$(NONEXISTANT^^)", ""},
    {"$(FILE_NAME^)", "Abcdef12345abcdef"},
    {"$(FILE_NAME^^)", "ABCDEF12345ABCDEF"},

    {"$(NONEXISTANT,)", ""},
    {"$(NONEXISTANT,,)", ""},
    {"$(JOBCODE,)", "aBCDEF12345ABCDEF"},
    {"$(JOBCODE,,)", "abcdef12345abcdef"},

    {NULL, NULL}
  }
};

static const test_t test_recursive_substitutions = {
  "abcdef12345abcdef", "ABCDEF12345ABCDEF", 23,
  {
    {"x$(TITLE-$(FILE_NAME))y", "xabcdef12345abcdefy"},
    {"x$(TITLE-a-$(FILE_NAME)-b)y", "xa-abcdef12345abcdef-by"},
    {"x$(SEQUENCE-$(FILE_NAME))y", "x0023y"},
    {"x$(FILE_NAME/12345/$(SEQUENCE))y", "xabcdef0023abcdefy"},
    {"x$(FILE_NAME/12345/.$(SEQUENCE).)y", "xabcdef.0023.abcdefy"},

    {NULL, NULL}
  }
};

static const test_t test_broken_variables = {
  "abcdef12345abcdef", "ABCDEF12345ABCDEF", 23,
  {
    {"$(NONEXISTANT", "$(NONEXISTANT"},
    {"x(NONEXISTANT23", "x(NONEXISTANT23"},
    {"$(FILE_NAME", "$(FILE_NAME"},
    {"x$(FILE_NAME", "x$(FILE_NAME"},
    {"x$(TITLE-$(FILE_NAME)", "x$(TITLE-abcdef12345abcdef"},

    {NULL, NULL}
  }
};

static const test_t test_escapes = {
  "/home/test/Images/IMG_0123.CR2", "/home/test/", 23,
  {
    {"foobarbaz", "foobarbaz"},
    {"foo/bar/baz", "foo/bar/baz"},
    {"foo\\bar\\baz", "foobarbaz"},
    {"foo\\\\bar\\\\baz", "foo\\bar\\baz"},
    {"foo\\$(bar", "foo$(bar"},
    {"foo$\\(bar", "foo$(bar"},
    {"foo\\$\\(bar", "foo$(bar"},
    {"foo\\$(bar$(SEQUENCE)baz", "foo$(bar0023baz"},
    {"foo$(bar$(SEQUENCE)baz", "foo$(bar0023baz"},
    {"$(FILE_FOLDER)/darktable_exported/img_$(SEQUENCE)", "/home/test/Images/darktable_exported/img_0023"},
    {"$(FILE_FOLDER)/darktable_exported/$(FILE_NAME)", "/home/test/Images/darktable_exported/IMG_0123"},

    {NULL, NULL}
  }
};

static const test_t test_real_paths = {
  "/home/test/Images/0023/IMG_0123.CR2", "/home/test", 23,
  {
    {"$(FILE_FOLDER#$(JOBCODE))", "/Images/0023"},
    {"$(FILE_FOLDER#$(JOBCODE)/Images)", "/0023"},

    {"$(FILE_FOLDER%$(SEQUENCE))", "/home/test/Images/"},
    {"$(FILE_FOLDER%/$(SEQUENCE))", "/home/test/Images"},

    {"$(FILE_FOLDER/test/$(SEQUENCE))", "/home/0023/Images/0023"},
    {"$(FILE_FOLDER/test/$(SEQUENCE)-$(SEQUENCE))", "/home/0023-0023/Images/0023"},
    {"$(FILE_FOLDER/test/$(SEQUENCE//0/o))", "/home/oo23/Images/0023"},
    {"$(FILE_FOLDER/$(SEQUENCE)/XXX)", "/home/test/Images/XXX"},
    {"$(FILE_FOLDER/$(JOBCODE)\\///media/)", "/media/Images/0023"},
    {"$(FILE_FOLDER/\\/home\\/test\\///media/exports/)/darktable_exported/img_$(SEQUENCE)", "/media/exports/Images/0023/darktable_exported/img_0023"},

    {"$(FILE_FOLDER/", "$(FILE_FOLDER/"},
    {"$(FILE_FOLDER/home", "$(FILE_FOLDER/home"},
    {"$(FILE_FOLDER/home/media", "$(FILE_FOLDER/home/media"},
    {"$(FILE_FOLDER/home/media)", "/media/test/Images/0023"},

    {NULL, NULL}
  }
};


#define TEST(t) \
{\
    int n_failed = 0, n_tests = 0;\
    n_test_functions++;\
    printf("running test '" #t "'\n");\
    n_test_functions_failed += run_test(&t, &n_tests, &n_failed, dt_variables_expand);\
    n_tests_overall += n_tests;\
    n_failed_overall += n_failed;\
    printf("%d / %d tests failed\n\n", n_failed, n_tests);\
}

#define TEST_PATH(test)\
{\
    printf("[%s]\n", #test);\
    int n_tests, n_failed;\
    n_test_functions++;\
    if(run_test(&test, &n_tests, &n_failed, dt_variables_expand_path)) n_test_functions_failed++;\
    n_tests_overall += n_tests;\
    n_failed_overall += n_failed;\
    printf("%d / %d tests failed\n\n", n_failed, n_tests);\
}

static const test_t test_paths = {
  "/home/test/Images/IMG_0123.CR2", "/home/test/", 23,
  {
    // a path pattern must survive expansion, separators and all
    {"$(FILE_FOLDER)/exported/$(FILE_NAME)",
     "/home/test/Images/exported/IMG_0123"},
    {"/home/test/$(JOBCODE)/img_$(SEQUENCE)",
     "/home/test//home/test//img_0023"},
    // "\/" escapes the delimiter inside a substitution and must mean the
    // same on every platform, so normalization has to leave it alone
    {"$(FILE_FOLDER/\\/home/X)", "X/test/Images"},
#ifdef _WIN32
    // the case this entry point exists for: separators survive, and so
    // does the variable that a trailing separator would otherwise escape
    {"D:\\photos\\$(FILE_NAME)", "D:/photos/IMG_0123"},
    // every other backslash is a separator here, so it cannot also escape
    {"foo\\$(bar", "foo/$(bar"},
#endif

    {NULL, NULL}
  }
};

// --- list-valued variables ------------------------------------------------

static int _sql_exec(const char *query)
{
  char *err = NULL;
  if(sqlite3_exec(dt_database_get(darktable.db), query, NULL, NULL, &err) != SQLITE_OK)
  {
    printf("  [FAIL] sql: %s\n", err ? err : "unknown error");
    sqlite3_free(err);
    return 1;
  }
  return 0;
}

static int _check_paths(const char *label,
                        GList *paths,
                        const int expected_count,
                        const char **expected)
{
  int failed = 0;
  const int count = g_list_length(paths);
  if(count != expected_count)
  {
    printf("  [FAIL] %s: got %d paths, expected %d\n", label, count, expected_count);
    return 1;
  }

  int i = 0;
  for(GList *l = paths; l; l = g_list_next(l), i++)
  {
    if(g_strcmp0((char *)l->data, expected[i]))
    {
      printf("  [FAIL] %s: path %d is '%s', expected '%s'\n",
             label, i, (char *)l->data, expected[i]);
      failed = 1;
    }
  }
  if(!failed) printf("  [OK] %s: %d paths\n", label, count);
  return failed;
}

static int test_category_each(void)
{
  int failed = 0;
  const dt_imgid_t imgid = 4242;

  // a plain image with two people and, separately, two crossed categories
  failed += _sql_exec("INSERT INTO main.images (id) VALUES (4242)");
  failed += _sql_exec("INSERT INTO data.tags (id, name) VALUES "
                      "(1, 'Person|John'), (2, 'Person|Jane'), "
                      "(3, 'Team|A|1'), (4, 'Team|A|2'), "
                      "(5, 'Team|B|1'), (6, 'Team|B|2')");
  failed += _sql_exec("INSERT INTO main.tagged_images (imgid, tagid, position) VALUES "
                      "(4242, 1, 0), (4242, 2, 0), (4242, 3, 0), (4242, 4, 0), "
                      "(4242, 5, 0), (4242, 6, 0)");

  dt_variables_params_t *params;
  dt_variables_params_init(&params);
  params->imgid = imgid;
  params->sequence = 0;

  // one path per subtag, ordered by tag name
  {
    GList *paths = dt_variables_expand_path_multi(
      params, g_strdup("/out/$(CATEGORY_EACH[0,Person])/img"), FALSE);
    const char *expected[] = {"/out/Jane/img", "/out/John/img"};
    failed += _check_paths("category_each one per tag", paths, 2, expected);
    g_list_free_full(paths, g_free);
  }

  // two list variables multiply
  {
    const char *pattern =
      "/out/$(CATEGORY_EACH[0,Team])/$(CATEGORY_EACH[1,Team])/img";
    GList *paths =
      dt_variables_expand_path_multi(params, g_strdup(pattern), FALSE);
    const char *expected[] = {"/out/A/1/img", "/out/A/2/img",
                              "/out/B/1/img", "/out/B/2/img"};
    failed += _check_paths("category_each cartesian product", paths, 4, expected);
    g_list_free_full(paths, g_free);
  }

  // the single-value API collapses to the comma-joined scalar
  {
    char *path = dt_variables_expand_path(
      params, g_strdup("/out/$(CATEGORY_EACH[0,Person])/img"), FALSE);
    if(g_strcmp0(path, "/out/Jane,John/img"))
    {
      printf("  [FAIL] category_each scalar fallback: got '%s'\n", path);
      failed++;
    }
    else printf("  [OK] category_each scalar fallback\n");
    g_free(path);
  }

  // no matching tags still yields one (empty) value
  {
    GList *paths = dt_variables_expand_path_multi(
      params, g_strdup("/out/$(CATEGORY_EACH[0,Nothing])/img"), FALSE);
    const char *expected[] = {"/out//img"};
    failed += _check_paths("category_each no match", paths, 1, expected);
    g_list_free_full(paths, g_free);
  }

  // a pattern without list variables yields exactly one path
  {
    GList *paths = dt_variables_expand_path_multi(
      params, g_strdup("/out/img"), FALSE);
    const char *expected[] = {"/out/img"};
    failed += _check_paths("no list variable", paths, 1, expected);
    g_list_free_full(paths, g_free);
  }

  // too many combinations must fail rather than flood the filesystem
  {
    failed += _sql_exec(
      "WITH RECURSIVE seq(n) AS (SELECT 1 UNION ALL SELECT n+1 FROM seq WHERE n < 1025) "
      "INSERT INTO data.tags (id, name) "
      "SELECT 10000 + n, 'Many|T' || printf('%04d', n) FROM seq");
    failed += _sql_exec(
      "WITH RECURSIVE seq(n) AS (SELECT 1 UNION ALL SELECT n+1 FROM seq WHERE n < 1025) "
      "INSERT INTO main.tagged_images (imgid, tagid, position) "
      "SELECT 4242, 10000 + n, 0 FROM seq");

    GList *paths = dt_variables_expand_path_multi(
      params, g_strdup("/out/$(CATEGORY_EACH[0,Many])/img"), FALSE);
    if(paths)
    {
      printf("  [FAIL] category_each overflow: got %d paths, expected failure\n",
             g_list_length(paths));
      failed++;
      g_list_free_full(paths, g_free);
    }
    else printf("  [OK] category_each overflow fails\n");
  }

  dt_variables_params_destroy(params);
  return failed;
}

int main(int argc, char* argv[])
{
  char *argv_override[] = {"darktable-test-variables", "--library", ":memory:", "--conf", "write_sidecar_files=never", NULL};
  int argc_override = sizeof(argv_override) / sizeof(*argv_override) - 1;

  // init dt without gui and without data.db:
  if(dt_init(argc_override, argv_override, FALSE, FALSE, NULL)) exit(1);

  int n_tests_overall = 0, n_failed_overall = 0, n_test_functions = 0, n_test_functions_failed = 0;

  TEST(test_variables)

  TEST(test_simple_substitutions)

  TEST(test_recursive_substitutions)

  TEST(test_broken_variables)

  TEST(test_escapes)

  TEST(test_real_paths)

  TEST_PATH(test_paths)

  {
    printf("running test 'test_category_each'\n");
    n_test_functions++;
    const int category_each_failed = test_category_each();
    n_tests_overall += 6;
    n_failed_overall += category_each_failed;
    if(category_each_failed) n_test_functions_failed++;
    printf("%d failures\n\n", category_each_failed);
  }

  printf("%d / %d tests failed (%d / %d)\n",
         n_failed_overall,
         n_tests_overall,
         n_test_functions_failed,
         n_test_functions);

  dt_cleanup();

  return 0;
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

