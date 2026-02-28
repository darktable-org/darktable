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

#include <limits.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <cmocka.h>

#include "ai/backend.h"

#ifndef TEST_MODEL_DIR
#error "TEST_MODEL_DIR must be defined by CMake"
#endif

/* Shared environment for all tests */
static dt_ai_environment_t *env;

/* ---- setup / teardown ---- */

static int group_setup(void **state)
{
  env = dt_ai_env_init(TEST_MODEL_DIR);
  *state = env;
  return env ? 0 : -1;
}

static int group_teardown(void **state)
{
  dt_ai_env_destroy(env);
  env = NULL;
  return 0;
}

/* ---- test: environment init ---- */

static void test_env_init(void **state)
{
  assert_non_null(env);
}

/* ---- test: model discovery ---- */

static void test_model_discovery(void **state)
{
  const int count = dt_ai_get_model_count(env);
  assert_int_equal(count, 1);

  const dt_ai_model_info_t *info = dt_ai_get_model_info_by_index(env, 0);
  assert_non_null(info);
  assert_string_equal(info->id, "test-multiply");
  assert_string_equal(info->name, "Test Multiply");
  assert_string_equal(info->task_type, "test");
  assert_string_equal(info->backend, "onnx");
  assert_int_equal(info->num_inputs, 1);
}

/* ---- test: model lookup by ID ---- */

static void test_model_lookup(void **state)
{
  const dt_ai_model_info_t *info
    = dt_ai_get_model_info_by_id(env, "test-multiply");
  assert_non_null(info);
  assert_string_equal(info->id, "test-multiply");

  /* non-existent model */
  const dt_ai_model_info_t *none
    = dt_ai_get_model_info_by_id(env, "does-not-exist");
  assert_null(none);
}

/* ---- test: model load ---- */

static void test_model_load(void **state)
{
  dt_ai_context_t *ctx
    = dt_ai_load_model(env, "test-multiply", NULL, DT_AI_PROVIDER_CPU);
  assert_non_null(ctx);
  dt_ai_unload_model(ctx);
}

/* ---- test: I/O introspection ---- */

static void test_introspection(void **state)
{
  dt_ai_context_t *ctx
    = dt_ai_load_model(env, "test-multiply", NULL, DT_AI_PROVIDER_CPU);
  assert_non_null(ctx);

  assert_int_equal(dt_ai_get_input_count(ctx), 1);
  assert_int_equal(dt_ai_get_output_count(ctx), 1);

  assert_string_equal(dt_ai_get_input_name(ctx, 0), "x");
  assert_string_equal(dt_ai_get_output_name(ctx, 0), "y");

  assert_int_equal(dt_ai_get_input_type(ctx, 0), DT_AI_FLOAT);
  assert_int_equal(dt_ai_get_output_type(ctx, 0), DT_AI_FLOAT);

  int64_t shape[8];
  const int ndim = dt_ai_get_output_shape(ctx, 0, shape, 8);
  assert_int_equal(ndim, 4);
  assert_int_equal(shape[0], 1);
  assert_int_equal(shape[1], 3);
  assert_int_equal(shape[2], 4);
  assert_int_equal(shape[3], 4);

  dt_ai_unload_model(ctx);
}

/* ---- test: inference ---- */

static void test_inference(void **state)
{
  dt_ai_context_t *ctx
    = dt_ai_load_model(env, "test-multiply", NULL, DT_AI_PROVIDER_CPU);
  assert_non_null(ctx);

  /* input: all 1.0 */
  const int n = 1 * 3 * 4 * 4;
  float input_data[48];  /* 1*3*4*4 = 48 */
  for(int i = 0; i < n; i++) input_data[i] = 1.0f;

  int64_t in_shape[] = { 1, 3, 4, 4 };
  dt_ai_tensor_t input = {
    .data = input_data,
    .type = DT_AI_FLOAT,
    .shape = in_shape,
    .ndim = 4
  };

  /* output buffer */
  float output_data[48];
  memset(output_data, 0, sizeof(output_data));

  int64_t out_shape[] = { 1, 3, 4, 4 };
  dt_ai_tensor_t output = {
    .data = output_data,
    .type = DT_AI_FLOAT,
    .shape = out_shape,
    .ndim = 4
  };

  const int ret = dt_ai_run(ctx, &input, 1, &output, 1);
  assert_int_equal(ret, 0);

  /* y = x * 2 → all outputs should be 2.0 */
  for(int i = 0; i < n; i++)
  {
    assert_float_equal(output_data[i], 2.0f, 1e-6f);
  }

  dt_ai_unload_model(ctx);
}

/* ---- test: provider setting ---- */

static void test_provider_change(void **state)
{
  /* stub returns "cpu" → init should have set CPU */
  assert_int_equal(dt_ai_env_get_provider(env), DT_AI_PROVIDER_CPU);

  /* change to CoreML */
  dt_ai_env_set_provider(env, DT_AI_PROVIDER_COREML);
  assert_int_equal(dt_ai_env_get_provider(env), DT_AI_PROVIDER_COREML);

  /* change to AUTO */
  dt_ai_env_set_provider(env, DT_AI_PROVIDER_AUTO);
  assert_int_equal(dt_ai_env_get_provider(env), DT_AI_PROVIDER_AUTO);

  /* restore CPU for remaining tests */
  dt_ai_env_set_provider(env, DT_AI_PROVIDER_CPU);
  assert_int_equal(dt_ai_env_get_provider(env), DT_AI_PROVIDER_CPU);
}

/* ---- test: unload + cleanup ---- */

static void test_cleanup(void **state)
{
  dt_ai_context_t *ctx
    = dt_ai_load_model(env, "test-multiply", NULL, DT_AI_PROVIDER_CPU);
  assert_non_null(ctx);

  /* unload should not crash */
  dt_ai_unload_model(ctx);

  /* double-unload NULL should be safe */
  dt_ai_unload_model(NULL);
}

/* ---- test: error paths — NULL and invalid arguments ---- */

static void test_error_null_env(void **state)
{
  /* NULL env should return NULL / 0, not crash */
  assert_null(dt_ai_load_model(NULL, "test-multiply", NULL, DT_AI_PROVIDER_CPU));
  assert_int_equal(dt_ai_get_model_count(NULL), 0);
  assert_null(dt_ai_get_model_info_by_index(NULL, 0));
  assert_null(dt_ai_get_model_info_by_id(NULL, "test-multiply"));
  assert_null(dt_ai_get_model_info_by_id(env, NULL));
}

static void test_error_bad_model_id(void **state)
{
  /* non-existent model ID */
  dt_ai_context_t *ctx
    = dt_ai_load_model(env, "no-such-model", NULL, DT_AI_PROVIDER_CPU);
  assert_null(ctx);
}

static void test_error_bad_model_file(void **state)
{
  /* existing model ID but non-existent .onnx file */
  dt_ai_context_t *ctx
    = dt_ai_load_model(env, "test-multiply", "nonexistent.onnx", DT_AI_PROVIDER_CPU);
  assert_null(ctx);
}

static void test_error_introspection_bounds(void **state)
{
  dt_ai_context_t *ctx
    = dt_ai_load_model(env, "test-multiply", NULL, DT_AI_PROVIDER_CPU);
  assert_non_null(ctx);

  /* NULL context */
  assert_int_equal(dt_ai_get_input_count(NULL), 0);
  assert_int_equal(dt_ai_get_output_count(NULL), 0);
  assert_null(dt_ai_get_input_name(NULL, 0));
  assert_null(dt_ai_get_output_name(NULL, 0));

  /* out-of-range index */
  assert_null(dt_ai_get_input_name(ctx, 99));
  assert_null(dt_ai_get_output_name(ctx, -1));

  /* output shape with NULL shape array */
  assert_int_equal(dt_ai_get_output_shape(ctx, 0, NULL, 0), -1);

  /* output shape with too-small buffer */
  int64_t shape[2];
  const int ndim = dt_ai_get_output_shape(ctx, 0, shape, 2);
  /* should return actual ndim (4) but only write 2 elements */
  assert_int_equal(ndim, 4);

  dt_ai_unload_model(ctx);
}

static void test_error_run_bad_args(void **state)
{
  /* dt_ai_run with NULL context */
  float dummy[48];
  int64_t shape[] = { 1, 3, 4, 4 };
  dt_ai_tensor_t t = { .data = dummy, .type = DT_AI_FLOAT, .shape = shape, .ndim = 4 };
  assert_int_not_equal(dt_ai_run(NULL, &t, 1, &t, 1), 0);
}

/* ---- test: provider string conversion ---- */

static void test_provider_strings(void **state)
{
  /* round-trip all known providers */
  for(int i = 0; i < DT_AI_PROVIDER_COUNT; i++)
  {
    const char *str = dt_ai_providers[i].config_string;
    dt_ai_provider_t parsed = dt_ai_provider_from_string(str);
    assert_int_equal(parsed, dt_ai_providers[i].value);
  }

  /* display name lookup */
  const char *cpu_name = dt_ai_provider_to_string(DT_AI_PROVIDER_CPU);
  assert_non_null(cpu_name);
  assert_string_equal(cpu_name, "CPU");

  /* unknown string falls back to AUTO */
  assert_int_equal(dt_ai_provider_from_string("bogus"), DT_AI_PROVIDER_AUTO);
  assert_int_equal(dt_ai_provider_from_string(NULL), DT_AI_PROVIDER_AUTO);
  assert_int_equal(dt_ai_provider_from_string(""), DT_AI_PROVIDER_AUTO);

  /* provider table completeness */
  assert_int_equal(dt_ai_providers[0].value, DT_AI_PROVIDER_AUTO);
  assert_int_equal(dt_ai_providers[DT_AI_PROVIDER_COUNT - 1].value, DT_AI_PROVIDER_DIRECTML);
}

/* ---- test: env_refresh preserves discovered models ---- */

static void test_env_refresh(void **state)
{
  const int before = dt_ai_get_model_count(env);
  dt_ai_env_refresh(env);
  const int after = dt_ai_get_model_count(env);
  assert_int_equal(before, after);

  /* model is still findable after refresh */
  const dt_ai_model_info_t *info
    = dt_ai_get_model_info_by_id(env, "test-multiply");
  assert_non_null(info);
  assert_string_equal(info->id, "test-multiply");
}

/* ---- test: load with optimization levels ---- */

static void test_load_opt_levels(void **state)
{
  /* DT_AI_OPT_BASIC */
  dt_ai_context_t *ctx_basic
    = dt_ai_load_model_ext(env, "test-multiply", NULL,
                           DT_AI_PROVIDER_CPU, DT_AI_OPT_BASIC, NULL, 0);
  assert_non_null(ctx_basic);

  /* verify inference still works with basic optimization */
  float in[48], out[48];
  for(int i = 0; i < 48; i++) in[i] = 3.0f;
  int64_t shape[] = { 1, 3, 4, 4 };
  dt_ai_tensor_t inp = { .data = in, .type = DT_AI_FLOAT, .shape = shape, .ndim = 4 };
  dt_ai_tensor_t outp = { .data = out, .type = DT_AI_FLOAT, .shape = shape, .ndim = 4 };
  assert_int_equal(dt_ai_run(ctx_basic, &inp, 1, &outp, 1), 0);
  assert_float_equal(out[0], 6.0f, 1e-6f);
  dt_ai_unload_model(ctx_basic);

  /* DT_AI_OPT_DISABLED */
  dt_ai_context_t *ctx_none
    = dt_ai_load_model_ext(env, "test-multiply", NULL,
                           DT_AI_PROVIDER_CPU, DT_AI_OPT_DISABLED, NULL, 0);
  assert_non_null(ctx_none);
  dt_ai_unload_model(ctx_none);
}

/* ---- test: env_init with empty/invalid path ---- */

static void test_env_init_empty(void **state)
{
  /* non-existent path: should succeed with 0 models */
  dt_ai_environment_t *e = dt_ai_env_init("/no/such/path/xyz");
  assert_non_null(e);
  assert_int_equal(dt_ai_get_model_count(e), 0);
  dt_ai_env_destroy(e);

  /* NULL path: still creates env (scans default dirs only) */
  dt_ai_environment_t *e2 = dt_ai_env_init(NULL);
  assert_non_null(e2);
  dt_ai_env_destroy(e2);
}

/* ---- main ---- */

int main(int argc, char *argv[])
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_env_init),
    cmocka_unit_test(test_model_discovery),
    cmocka_unit_test(test_model_lookup),
    cmocka_unit_test(test_model_load),
    cmocka_unit_test(test_introspection),
    cmocka_unit_test(test_inference),
    cmocka_unit_test(test_provider_change),
    cmocka_unit_test(test_cleanup),
    cmocka_unit_test(test_error_null_env),
    cmocka_unit_test(test_error_bad_model_id),
    cmocka_unit_test(test_error_bad_model_file),
    cmocka_unit_test(test_error_introspection_bounds),
    cmocka_unit_test(test_error_run_bad_args),
    cmocka_unit_test(test_provider_strings),
    cmocka_unit_test(test_env_refresh),
    cmocka_unit_test(test_load_opt_levels),
    cmocka_unit_test(test_env_init_empty),
  };

  return cmocka_run_group_tests(tests, group_setup, group_teardown);
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
