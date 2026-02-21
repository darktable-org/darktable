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
    cmocka_unit_test(test_cleanup),
  };

  return cmocka_run_group_tests(tests, group_setup, group_teardown);
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
