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

/*
 * how to run these tests:
 *
 *   cmake -B build -DUSE_AI=ON
 *   cmake --build build --target test_ai_backend
 *   cd build && ctest -R test_ai_backend -V
 *
 * TEST_MODEL_DIR is set automatically by CMake to point at the test
 * fixtures directory containing sample ONNX models
 */

#include <limits.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <cmocka.h>

#include "ai/backend.h"

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

#ifndef _WIN32
#include <glib/gstdio.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#ifndef TEST_MODEL_DIR
#error "TEST_MODEL_DIR must be defined by CMake"
#endif

// shared environment for all tests
static dt_ai_environment_t *env;

#ifndef _WIN32
// real-worker tests spawn tools/ai/remote_worker.py; the safety-net
// shutdown lives with the other worker helpers below
static void _stop_worker(void);
#endif

// setup / teardown

static int group_setup(void **state)
{
  env = dt_ai_env_init(TEST_MODEL_DIR);
  *state = env;
  return env ? 0 : -1;
}

static int group_teardown(void **state)
{
#ifndef _WIN32
  _stop_worker(); // safety net if a real-worker test aborted mid-run
#endif
  dt_ai_env_destroy(env);
  env = NULL;
  return 0;
}

// test: environment init

static void test_env_init(void **state)
{
  assert_non_null(env);
}

// test: model discovery

static void test_model_discovery(void **state)
{
  const int count = dt_ai_get_model_count(env);
  assert_int_equal(count, 2);

  // discovery order is not defined, so look the fixture up rather than
  // indexing blindly
  const dt_ai_model_info_t *info = NULL;
  for(int i = 0; i < count; i++)
  {
    const dt_ai_model_info_t *m = dt_ai_get_model_info_by_index(env, i);
    assert_non_null(m);
    if(!strcmp(m->id, "test-multiply")) info = m;
  }
  assert_non_null(info);
  assert_string_equal(info->id, "test-multiply");
  assert_string_equal(info->name, "Test Multiply");
  assert_string_equal(info->task_type, "test");
  assert_string_equal(info->backend, "onnx");
  assert_int_equal(info->num_inputs, 1);
}

// test: model lookup by ID

static void test_model_lookup(void **state)
{
  const dt_ai_model_info_t *info
    = dt_ai_get_model_info_by_id(env, "test-multiply");
  assert_non_null(info);
  assert_string_equal(info->id, "test-multiply");

  // non-existent model
  const dt_ai_model_info_t *none
    = dt_ai_get_model_info_by_id(env, "does-not-exist");
  assert_null(none);
}

// test: model load

static void test_model_load(void **state)
{
  dt_ai_context_t *ctx
    = dt_ai_load_model(env, "test-multiply", NULL, DT_AI_PROVIDER_CPU);
  assert_non_null(ctx);
  dt_ai_unload_model(ctx);
}

// test: I/O introspection

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

// test: inference

static void test_inference(void **state)
{
  dt_ai_context_t *ctx
    = dt_ai_load_model(env, "test-multiply", NULL, DT_AI_PROVIDER_CPU);
  assert_non_null(ctx);

  // input: all 1.0
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

  // output buffer
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

  // y = x * 2 → all outputs should be 2.0
  for(int i = 0; i < n; i++)
  {
    assert_float_equal(output_data[i], 2.0f, 1e-6f);
  }

  dt_ai_unload_model(ctx);
}

// test: provider setting

static void test_provider_change(void **state)
{
  // stub returns "cpu" → init should have set CPU
  assert_int_equal(dt_ai_env_get_provider(env), DT_AI_PROVIDER_CPU);

  // change to CoreML
  dt_ai_env_set_provider(env, DT_AI_PROVIDER_COREML);
  assert_int_equal(dt_ai_env_get_provider(env), DT_AI_PROVIDER_COREML);

  // change to AUTO
  dt_ai_env_set_provider(env, DT_AI_PROVIDER_AUTO);
  assert_int_equal(dt_ai_env_get_provider(env), DT_AI_PROVIDER_AUTO);

  // restore CPU for remaining tests
  dt_ai_env_set_provider(env, DT_AI_PROVIDER_CPU);
  assert_int_equal(dt_ai_env_get_provider(env), DT_AI_PROVIDER_CPU);
}

// test: unload + cleanup

static void test_cleanup(void **state)
{
  dt_ai_context_t *ctx
    = dt_ai_load_model(env, "test-multiply", NULL, DT_AI_PROVIDER_CPU);
  assert_non_null(ctx);

  // unload should not crash
  dt_ai_unload_model(ctx);

  // double-unload NULL should be safe
  dt_ai_unload_model(NULL);
}

// test: error paths — NULL and invalid arguments

static void test_error_null_env(void **state)
{
  // NULL env should return NULL / 0, not crash
  assert_null(dt_ai_load_model(NULL, "test-multiply", NULL, DT_AI_PROVIDER_CPU));
  assert_int_equal(dt_ai_get_model_count(NULL), 0);
  assert_null(dt_ai_get_model_info_by_index(NULL, 0));
  assert_null(dt_ai_get_model_info_by_id(NULL, "test-multiply"));
  assert_null(dt_ai_get_model_info_by_id(env, NULL));
}

static void test_error_bad_model_id(void **state)
{
  // non-existent model ID
  dt_ai_context_t *ctx
    = dt_ai_load_model(env, "no-such-model", NULL, DT_AI_PROVIDER_CPU);
  assert_null(ctx);
}

static void test_error_bad_model_file(void **state)
{
  // existing model ID but non-existent .onnx file
  dt_ai_context_t *ctx
    = dt_ai_load_model(env, "test-multiply", "nonexistent.onnx", DT_AI_PROVIDER_CPU);
  assert_null(ctx);
}

static void test_error_introspection_bounds(void **state)
{
  dt_ai_context_t *ctx
    = dt_ai_load_model(env, "test-multiply", NULL, DT_AI_PROVIDER_CPU);
  assert_non_null(ctx);

  // NULL context
  assert_int_equal(dt_ai_get_input_count(NULL), 0);
  assert_int_equal(dt_ai_get_output_count(NULL), 0);
  assert_null(dt_ai_get_input_name(NULL, 0));
  assert_null(dt_ai_get_output_name(NULL, 0));

  // out-of-range index
  assert_null(dt_ai_get_input_name(ctx, 99));
  assert_null(dt_ai_get_output_name(ctx, -1));

  // output shape with NULL shape array
  assert_int_equal(dt_ai_get_output_shape(ctx, 0, NULL, 0), -1);

  // output shape with too-small buffer
  int64_t shape[2];
  const int ndim = dt_ai_get_output_shape(ctx, 0, shape, 2);
  /* should return actual ndim (4) but only write 2 elements */
  assert_int_equal(ndim, 4);

  dt_ai_unload_model(ctx);
}

static void test_error_run_bad_args(void **state)
{
  // dt_ai_run with NULL context
  float dummy[48];
  int64_t shape[] = { 1, 3, 4, 4 };
  dt_ai_tensor_t t = { .data = dummy, .type = DT_AI_FLOAT, .shape = shape, .ndim = 4 };
  assert_int_not_equal(dt_ai_run(NULL, &t, 1, &t, 1), 0);
}

// test: provider string conversion

static void test_provider_strings(void **state)
{
  // round-trip all known providers
  for(int i = 0; i < DT_AI_PROVIDER_COUNT; i++)
  {
    const char *str = dt_ai_providers[i].config_string;
    dt_ai_provider_t parsed = dt_ai_provider_from_string(str);
    assert_int_equal(parsed, dt_ai_providers[i].value);
  }

  // display name lookup
  const char *cpu_name = dt_ai_provider_to_string(DT_AI_PROVIDER_CPU);
  assert_non_null(cpu_name);
  assert_string_equal(cpu_name, "CPU");

  // unknown string falls back to AUTO
  assert_int_equal(dt_ai_provider_from_string("bogus"), DT_AI_PROVIDER_AUTO);
  assert_int_equal(dt_ai_provider_from_string(NULL), DT_AI_PROVIDER_AUTO);
  assert_int_equal(dt_ai_provider_from_string(""), DT_AI_PROVIDER_AUTO);

  // provider table completeness
  assert_int_equal(dt_ai_providers[0].value, DT_AI_PROVIDER_AUTO);
  assert_int_equal(dt_ai_providers[DT_AI_PROVIDER_COUNT - 1].value, DT_AI_PROVIDER_DIRECTML);
}

// test: env_refresh preserves discovered models ----

static void test_env_refresh(void **state)
{
  const int before = dt_ai_get_model_count(env);
  dt_ai_env_refresh(env);
  const int after = dt_ai_get_model_count(env);
  assert_int_equal(before, after);

  // model is still findable after refresh
  const dt_ai_model_info_t *info
    = dt_ai_get_model_info_by_id(env, "test-multiply");
  assert_non_null(info);
  assert_string_equal(info->id, "test-multiply");
}

// test: load with optimization levels

static void test_load_opt_levels(void **state)
{
  // DT_AI_OPT_BASIC
  dt_ai_context_t *ctx_basic
    = dt_ai_load_model_ext(env, "test-multiply", NULL,
                           DT_AI_PROVIDER_CPU, DT_AI_OPT_BASIC, NULL, 0);
  assert_non_null(ctx_basic);

  // verify inference still works with basic optimization
  float in[48], out[48];
  for(int i = 0; i < 48; i++) in[i] = 3.0f;
  int64_t shape[] = { 1, 3, 4, 4 };
  dt_ai_tensor_t inp = { .data = in, .type = DT_AI_FLOAT, .shape = shape, .ndim = 4 };
  dt_ai_tensor_t outp = { .data = out, .type = DT_AI_FLOAT, .shape = shape, .ndim = 4 };
  assert_int_equal(dt_ai_run(ctx_basic, &inp, 1, &outp, 1), 0);
  assert_float_equal(out[0], 6.0f, 1e-6f);
  dt_ai_unload_model(ctx_basic);

  // DT_AI_OPT_DISABLED
  dt_ai_context_t *ctx_none
    = dt_ai_load_model_ext(env, "test-multiply", NULL,
                           DT_AI_PROVIDER_CPU, DT_AI_OPT_DISABLED, NULL, 0);
  assert_non_null(ctx_none);
  dt_ai_unload_model(ctx_none);
}

// test: env_init with empty/invalid path

static void test_env_init_empty(void **state)
{
  // non-existent path: should succeed with 0 models
  dt_ai_environment_t *e = dt_ai_env_init("/no/such/path/xyz");
  assert_non_null(e);
  assert_int_equal(dt_ai_get_model_count(e), 0);
  dt_ai_env_destroy(e);

  // NULL path: still creates env (scans default dirs only)
  dt_ai_environment_t *e2 = dt_ai_env_init(NULL);
  assert_non_null(e2);
  dt_ai_env_destroy(e2);
}

// test: an undersized caller buffer on the ORT-allocated path
//
// the model leaves its spatial dims symbolic, so dt_ai_run() lets ORT
// allocate and copies back. the caller here declares a 2x2 output while
// the model produces 4x4, which used to copy all 48 floats into a buffer
// holding 12

static void test_dynamic_output_undersized(void **state)
{
  dt_ai_context_t *ctx = dt_ai_load_model(env, "test-multiply-dynamic",
                                          NULL, DT_AI_PROVIDER_CPU);
  assert_non_null(ctx);

  // the fixture is only useful if ORT really reports it as unresolved
  int64_t model_shape[8];
  const int model_ndim = dt_ai_get_output_shape(ctx, 0, model_shape, 8);
  assert_int_equal(model_ndim, 4);
  assert_true(model_shape[2] <= 0 || model_shape[3] <= 0);

  float input_data[48];
  for(int i = 0; i < 48; i++) input_data[i] = 1.0f;

  int64_t in_shape[] = { 1, 3, 4, 4 };
  dt_ai_tensor_t input = {
    .data = input_data,
    .type = DT_AI_FLOAT,
    .shape = in_shape,
    .ndim = 4
  };

  // 12 declared elements, then a canary the copy must never reach
  const float canary = -12345.0f;
  float output_data[64];
  for(int i = 0; i < 64; i++) output_data[i] = canary;

  int64_t out_shape[] = { 1, 3, 2, 2 };
  dt_ai_tensor_t output = {
    .data = output_data,
    .type = DT_AI_FLOAT,
    .shape = out_shape,
    .ndim = 4
  };

  const int ret = dt_ai_run(ctx, &input, 1, &output, 1);

  // nothing may have been written past what the caller declared
  for(int i = 12; i < 64; i++)
    assert_float_equal(output_data[i], canary, 0.0f);

  // and a partial tensor is not a usable result, so the run must fail
  assert_int_not_equal(ret, 0);

  dt_ai_unload_model(ctx);
}

#ifndef _WIN32
// ---- remote routing (DT_AI_REMOTE) -------------------------------------
//
// the worker endpoint is a loopback listener we control. the client must
// route CPU-preference loads to it, must not require local weights, and
// must fail a load (never fall back locally) when the endpoint is dead.

// connections the client opened against our fake worker
static int _remote_accepted;

static int _remote_listener(guint16 *out_port)
{
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  if(fd < 0) return -1;
  struct sockaddr_in addr = { 0 };
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if(bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0
     || listen(fd, 4) != 0)
  {
    close(fd);
    return -1;
  }
  struct sockaddr_in got;
  socklen_t len = sizeof(got);
  if(getsockname(fd, (struct sockaddr *)&got, &len) != 0)
  {
    close(fd);
    return -1;
  }
  *out_port = ntohs(got.sin_port);
  return fd;
}

// accepts the one queued connection and closes it: the client's load
// then fails on EOF, but the accepted connection proves the remote
// route was taken. poll-bounded so a client that never connects (the
// failing-before case) leaves 0 accepted instead of hanging the suite
static gpointer _accept_and_close(gpointer data)
{
  const int fd = GPOINTER_TO_INT(data);
  struct pollfd p = { .fd = fd, .events = POLLIN };
  if(poll(&p, 1, 5000) == 1)
  {
    const int cfd = accept(fd, NULL, NULL);
    if(cfd >= 0)
    {
      close(cfd);
      _remote_accepted++;
    }
  }
  close(fd);
  return NULL;
}

// run one CPU-provider load against the fake worker; joins the accept
// thread and unsets DT_AI_REMOTE before returning
static dt_ai_context_t *_remote_cpu_load(dt_ai_environment_t *e,
                                         const char *model_id,
                                         const int lfd, const guint16 port)
{
  gchar *spec = g_strdup_printf("127.0.0.1:%u", port);
  g_setenv("DT_AI_REMOTE", spec, TRUE);
  GThread *t = g_thread_new("accept", _accept_and_close,
                            GINT_TO_POINTER(lfd));
  dt_ai_context_t *ctx = dt_ai_load_model_ext(
    e, model_id, NULL, DT_AI_PROVIDER_CPU, DT_AI_OPT_DISABLED, NULL, 0);
  g_thread_join(t);
  g_unsetenv("DT_AI_REMOTE");
  g_free(spec);
  return ctx;
}

// test: an explicit CPU request (the segmentation decoder path) routes
// to the remote worker instead of staying local

static void test_remote_cpu_routes_to_worker(void **state)
{
  guint16 port = 0;
  const int lfd = _remote_listener(&port);
  assert_int_not_equal(lfd, -1);
  _remote_accepted = 0;

  // the model IS in the local registry: only the endpoint may answer
  dt_ai_context_t *ctx = _remote_cpu_load(env, "test-multiply", lfd, port);

  assert_int_equal(_remote_accepted, 1); // reached the worker
  assert_null(ctx);                      // accept+close fails the load loudly
}

// test: remote loads have no local model prerequisites — an env that
// discovered nothing must still contact the worker

static void test_remote_needs_no_local_models(void **state)
{
  guint16 port = 0;
  const int lfd = _remote_listener(&port);
  assert_int_not_equal(lfd, -1);
  _remote_accepted = 0;

  dt_ai_environment_t *empty_env = dt_ai_env_init("/no/such/models");
  assert_non_null(empty_env);
  // must be absent from the local registry whatever the machine has
  // installed (the XDG fallback can contribute real models)
  assert_null(dt_ai_get_model_info_by_id(empty_env, "remote-only-fixture"));

  dt_ai_context_t *ctx
    = _remote_cpu_load(empty_env, "remote-only-fixture", lfd, port);

  assert_int_equal(_remote_accepted, 1);
  assert_null(ctx);
  dt_ai_env_destroy(empty_env);
}

// test: a configured but dead endpoint fails the load without falling
// back to the locally available model / ORT runtime

static void test_remote_endpoint_failure_stays_remote(void **state)
{
  // grab an ephemeral port, then close the listener: a guaranteed-dead
  // loopback endpoint
  guint16 port = 0;
  const int lfd = _remote_listener(&port);
  assert_int_not_equal(lfd, -1);
  const int closed = close(lfd);
  assert_int_equal(closed, 0);

  gchar *spec = g_strdup_printf("127.0.0.1:%u", port);
  g_setenv("DT_AI_REMOTE", spec, TRUE);
  dt_ai_context_t *ctx = dt_ai_load_model_ext(
    env, "test-multiply", NULL, DT_AI_PROVIDER_CPU,
    DT_AI_OPT_DISABLED, NULL, 0);
  g_unsetenv("DT_AI_REMOTE");
  g_free(spec);
  assert_null(ctx); // no local fallback even though the model is present

  // sanity: the identical load succeeds locally once DT_AI_REMOTE is
  // unset, so the NULL above came from the remote contract, not from a
  // broken fixture
  ctx = dt_ai_load_model_ext(env, "test-multiply", NULL,
                             DT_AI_PROVIDER_CPU, DT_AI_OPT_DISABLED, NULL, 0);
  assert_non_null(ctx);
  dt_ai_unload_model(ctx);
}
// ---- real end-to-end: this C client against tools/ai/remote_worker.py --
//
// needs a python interpreter with numpy + onnxruntime; point
// REMOTE_WORKER_PYTHON at one (default "python3"). the tests skip
// where such an interpreter is unavailable.

static GPid _worker_pid = 0;

static void _stop_worker(void)
{
  if(!_worker_pid) return;
  kill(_worker_pid, SIGTERM);
  waitpid(_worker_pid, NULL, 0);
  g_spawn_close_pid(_worker_pid);
  _worker_pid = 0;
}

static gboolean _worker_python_available(void)
{
  const char *py = g_getenv("REMOTE_WORKER_PYTHON");
  gchar *cmd = g_strdup_printf("%s -c \"import onnxruntime, numpy\"",
                               py && *py ? py : "python3");
  gint rc = -1;
  const gboolean ok
    = g_spawn_command_line_sync(cmd, NULL, NULL, &rc, NULL) && rc == 0;
  g_free(cmd);
  return ok;
}

// spawn the real worker on a free loopback port and wait until it accepts
static gboolean _start_worker(guint16 *out_port)
{
  guint16 port = 0;
  const int lfd = _remote_listener(&port); // bind :0, read the port
  if(lfd < 0) return FALSE;
  close(lfd); // hand the port to the worker

  const char *py = g_getenv("REMOTE_WORKER_PYTHON");
  gchar port_arg[16];
  g_snprintf(port_arg, sizeof(port_arg), "--port=%u", port);
  gchar *argv[] = { (gchar *)(py && *py ? py : "python3"),
                    (gchar *)SRCDIR "/tools/ai/remote_worker.py",
                    (gchar *)"--models-root", (gchar *)TEST_MODEL_DIR,
                    (gchar *)"--bind", (gchar *)"127.0.0.1",
                    port_arg, NULL };
  GError *err = NULL;
  const gboolean spawned = g_spawn_async(NULL, argv, NULL,
                                         G_SPAWN_SEARCH_PATH, NULL, NULL,
                                         &_worker_pid, &err);
  if(!spawned)
  {
    _worker_pid = 0;
    g_error_free(err);
    return FALSE;
  }

  for(int i = 0; i < 100; i++) // ~10s for the worker to start listening
  {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = { 0 };
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0)
    {
      close(fd);
      *out_port = port;
      return TRUE;
    }
    close(fd);
    g_usleep(G_USEC_PER_SEC / 10);
  }
  _stop_worker();
  return FALSE;
}

// load through the real worker at the caller's opt level and check one
// multiply run; DT_AI_OPT_DEFAULT (-1) is serialized as-is and the
// WORKER must resolve it from the model manifest
static void _assert_remote_run(dt_ai_environment_t *e, const char *model_id,
                               const dt_ai_opt_level_t opt_level)
{
  dt_ai_context_t *ctx = dt_ai_load_model_ext(e, model_id, NULL,
                                              DT_AI_PROVIDER_CPU,
                                              opt_level, NULL, 0);
  assert_non_null(ctx);

  float in[48], out[48];
  for(int i = 0; i < 48; i++) in[i] = 3.0f;
  int64_t shape[] = { 1, 3, 4, 4 };
  dt_ai_tensor_t inp = {
    .data = in, .type = DT_AI_FLOAT, .shape = shape, .ndim = 4 };
  dt_ai_tensor_t outp = {
    .data = out, .type = DT_AI_FLOAT, .shape = shape, .ndim = 4 };
  assert_int_equal(dt_ai_run(ctx, &inp, 1, &outp, 1), 0);
  assert_float_equal(out[0], 6.0f, 1e-6f);
  dt_ai_unload_model(ctx);
}

// regression: dt_ai_load_model() passes DT_AI_OPT_DEFAULT (-1); the
// worker resolves it from the fixture manifest (no ort_optimization
// key there → all) instead of rejecting the frame
static void test_remote_real_worker_default_opt(void **state)
{
  if(!_worker_python_available()) { skip(); return; }
  guint16 port = 0;
  assert_true(_start_worker(&port));

  gchar *spec = g_strdup_printf("127.0.0.1:%u", port);
  g_setenv("DT_AI_REMOTE", spec, TRUE);
  _assert_remote_run(env, "test-multiply", DT_AI_OPT_DEFAULT);

  // an explicit level is a caller override: passed through unchanged
  dt_ai_context_t *ctx = dt_ai_load_model_ext(env, "test-multiply", NULL,
                                              DT_AI_PROVIDER_CPU,
                                              DT_AI_OPT_BASIC, NULL, 0);
  assert_non_null(ctx);
  dt_ai_unload_model(ctx);

  g_unsetenv("DT_AI_REMOTE");
  g_free(spec);
  _stop_worker();
}

// no local weights: the local model dir holds config.json only and the
// .onnx lives solely in the worker's root. manifests stay local for
// enumeration and preprocessing; weights are remote-only
static void test_remote_real_worker_manifest_only(void **state)
{
  if(!_worker_python_available()) { skip(); return; }
  guint16 port = 0;
  assert_true(_start_worker(&port));

  gchar *tmp = g_dir_make_tmp("dt-ai-manifests-XXXXXX", NULL);
  assert_non_null(tmp);
  gchar *mdir = g_build_filename(tmp, "test-multiply", NULL);
  assert_int_equal(g_mkdir(mdir, 0700), 0);
  gchar *src
    = g_build_filename(TEST_MODEL_DIR, "test-multiply", "config.json", NULL);
  gchar *dst = g_build_filename(mdir, "config.json", NULL);
  gchar *text = NULL;
  gsize len = 0;
  assert_true(g_file_get_contents(src, &text, &len, NULL));
  assert_true(g_file_set_contents(dst, text, len, NULL));
  g_free(text);
  g_free(src);
  g_free(dst);

  dt_ai_environment_t *e = dt_ai_env_init(tmp);
  assert_non_null(e);
  // metadata-only local dir: registry entry exists, no .onnx beside it
  assert_non_null(dt_ai_get_model_info_by_id(e, "test-multiply"));

  gchar *spec = g_strdup_printf("127.0.0.1:%u", port);
  g_setenv("DT_AI_REMOTE", spec, TRUE);
  _assert_remote_run(e, "test-multiply", DT_AI_OPT_DEFAULT);
  g_unsetenv("DT_AI_REMOTE");
  g_free(spec);

  dt_ai_env_destroy(e);
  g_rmdir(mdir);
  g_rmdir(tmp);
  g_free(mdir);
  g_free(tmp);
  _stop_worker();
}

// rank-0 int64 scalar round trip against the real worker (MoGe-style
// num_tokens): empty shape on the wire, bytes = one element. covers the
// caller-buffer path and the ORT-allocated path
static void test_remote_real_worker_scalar(void **state)
{
  if(!_worker_python_available()) { skip(); return; }
  guint16 port = 0;
  assert_true(_start_worker(&port));

  gchar *spec = g_strdup_printf("127.0.0.1:%u", port);
  g_setenv("DT_AI_REMOTE", spec, TRUE);

  int64_t unused_shape[1] = { 1 }; // rank 0: never dereferenced

  // caller buffer: one int64 in, one int64 out
  dt_ai_context_t *ctx = dt_ai_load_model_ext(env, "test-multiply",
                                              "scalar.onnx",
                                              DT_AI_PROVIDER_CPU,
                                              DT_AI_OPT_ALL, NULL, 0);
  assert_non_null(ctx);
  int64_t in_value = 7;
  int64_t out_value = 0;
  dt_ai_tensor_t input = {
    .data = &in_value, .type = DT_AI_INT64, .shape = unused_shape, .ndim = 0 };
  dt_ai_tensor_t output = {
    .data = &out_value, .type = DT_AI_INT64, .shape = unused_shape, .ndim = 0 };
  assert_int_equal(dt_ai_run(ctx, &input, 1, &output, 1), 0);
  assert_int_equal(out_value, 14);
  assert_int_equal(output.ndim, 0);
  dt_ai_unload_model(ctx);

  // ORT-allocated path: NULL output data, rank-0 allocation request
  ctx = dt_ai_load_model_ext(env, "test-multiply", "scalar.onnx",
                             DT_AI_PROVIDER_CPU, DT_AI_OPT_ALL, NULL, 0);
  assert_non_null(ctx);
  dt_ai_tensor_t output2 = {
    .data = NULL, .type = DT_AI_INT64, .shape = unused_shape, .ndim = 0 };
  assert_int_equal(dt_ai_run(ctx, &input, 1, &output2, 1), 0);
  assert_non_null(output2.data);
  assert_int_equal(output2.ndim, 0);
  assert_int_equal(*(int64_t *)output2.data, 14);
  g_free(output2.data);
  dt_ai_unload_model(ctx);

  g_unsetenv("DT_AI_REMOTE");
  g_free(spec);
  _stop_worker();
}
#endif // _WIN32

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
    cmocka_unit_test(test_dynamic_output_undersized),
#ifndef _WIN32
    cmocka_unit_test(test_remote_cpu_routes_to_worker),
    cmocka_unit_test(test_remote_needs_no_local_models),
    cmocka_unit_test(test_remote_endpoint_failure_stays_remote),
    cmocka_unit_test(test_remote_real_worker_default_opt),
    cmocka_unit_test(test_remote_real_worker_manifest_only),
    cmocka_unit_test(test_remote_real_worker_scalar),
#endif
  };

  return cmocka_run_group_tests(tests, group_setup, group_teardown);
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
