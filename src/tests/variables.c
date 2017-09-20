#include "common/darktable.h"
#include "common/variables.h"

#include <stdio.h>

typedef struct test_t
{
  char *input, *expected_result;
} test_t;

int run_test(const test_t *tests, int *n_tests, int *n_failed)
{
  dt_variables_params_t *params;
  dt_variables_params_init(&params);
  params->sequence = 23;
  params->filename = "abcdef12345abcdef";
  params->jobcode = "ABCDEF12345ABCDEF";

  *n_failed = 0;
  *n_tests = 0;
  for(const test_t *test = tests; test->input; test++)
  {
    (*n_tests)++;
    char *result = dt_variables_expand(params, test->input, FALSE);
    if(g_strcmp0(result, test->expected_result))
    {
      (*n_failed)++;
      printf("  [FAIL] input: '%s', result: '%s', expected: '%s'\n", test->input, result, test->expected_result);
    }
    else
      printf("  [OK] input: '%s', result: '%s'\n", test->input, result);
  }

  dt_variables_params_destroy(params);

  return *n_failed > 0 ? 1 : 0;
}

static const test_t test_variables[] = {
  {"$(FILE_NAME)", "abcdef12345abcdef"},
  {"foo-$(FILE_NAME)-bar", "foo-abcdef12345abcdef-bar"},
  {"äöü-$(FILE_NAME)-äöü", "äöü-abcdef12345abcdef-äöü"},
  {"$(FILE_NAME).$(SEQUENCE)", "abcdef12345abcdef.0023"},
  {"$(NONEXISTANT)", ""},
  {"foo-$(NONEXISTANT)-bar", "foo--bar"},

  {NULL, NULL}
};

static const test_t test_simple_substitutions[] = {
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
};

static const test_t test_recursive_substitutions[] = {
  {"x$(TITLE-$(FILE_NAME))y", "xabcdef12345abcdefy"},
  {"x$(TITLE-a-$(FILE_NAME)-b)y", "xa-abcdef12345abcdef-by"},
  {"x$(SEQUENCE-$(FILE_NAME))y", "x0023y"},
  {"x$(FILE_NAME/12345/$(SEQUENCE))y", "xabcdef0023abcdefy"},
  {"x$(FILE_NAME/12345/.$(SEQUENCE).)y", "xabcdef.0023.abcdefy"},

  {NULL, NULL}
};

static const test_t test_broken_variables[] = {
  {"$(NONEXISTANT", "$(NONEXISTANT"},
  {"x(NONEXISTANT23", "x(NONEXISTANT23"},
  {"$(FILE_NAME", "$(FILE_NAME"},
  {"x$(FILE_NAME", "x$(FILE_NAME"},
  {"x$(TITLE-$(FILE_NAME)", "x$(TITLE-$(FILE_NAME)"},

  {NULL, NULL}
};

#define TEST(t) \
{\
    int n_failed = 0, n_tests = 0;\
    n_test_functions++;\
    printf("running test '" #t "'\n");\
    n_test_functions_failed += run_test(t, &n_tests, &n_failed);\
    n_tests_overall += n_tests;\
    n_failed_overall += n_failed;\
    printf("%d / %d tests failed\n\n", n_failed, n_tests);\
}

int main()
{
  char *argv[] = {"darktable-test-variables", "--library", ":memory:", "--conf", "write_sidecar_files=FALSE", NULL};
  int argc = sizeof(argv) / sizeof(*argv) - 1;

  // init dt without gui and without data.db:
  if(dt_init(argc, argv, FALSE, FALSE, NULL)) exit(1);

  int n_tests_overall = 0, n_failed_overall = 0, n_test_functions = 0, n_test_functions_failed = 0;

  TEST(test_variables)

  TEST(test_simple_substitutions)

  TEST(test_recursive_substitutions)

  TEST(test_broken_variables)

  printf("%d / %d tests failed (%d / %d)\n",
         n_failed_overall,
         n_tests_overall,
         n_test_functions_failed,
         n_test_functions);

  dt_cleanup();

  return 0;
}
