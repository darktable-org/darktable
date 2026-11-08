/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "common/introspection.h"
#include "fixtures/double_params.c"
#include <stddef.h>
#include <stdio.h>
#include <string.h>

int introspection_init(struct dt_iop_module_so_t *self, int api_version);
dt_introspection_t *get_introspection(void);
dt_introspection_field_t *get_f(const char *name);
void *get_p(const void *params, const char *name);

#define CHECK(condition) do { if(!(condition)) { \
  fprintf(stderr, "double introspection:%d: %s\n", __LINE__, #condition); return 1; } } while(0)

static int matches(const char *name, size_t offset, double minimum, double maximum, double original)
{
  const dt_introspection_field_t *field = get_f(name);
  return field && field->header.type == DT_INTROSPECTION_TYPE_DOUBLE
    && field->header.size == sizeof(double) && field->header.offset == offset
    && field->Double.Min == minimum && field->Double.Max == maximum
    && field->Double.Default == original;
}

int main(void)
{
  CHECK(!introspection_init(NULL, DT_INTROSPECTION_VERSION));
  CHECK(get_introspection()->size == sizeof(double_fixture_params_t));
  CHECK(matches("pair.radius", offsetof(double_fixture_params_t, pair.radius), 0., 256., 64.));
  CHECK(matches("pair.slope", offsetof(double_fixture_params_t, pair.slope), 1., 3., 1.25));
  CHECK(matches("precise", offsetof(double_fixture_params_t, precise), -2., 2., 1.234567890123));
  CHECK(matches("unbounded", offsetof(double_fixture_params_t, unbounded), -G_MAXDOUBLE, G_MAXDOUBLE, 0.));
  CHECK(get_f("pair")->header.size == sizeof(double_pair_t));
  const dt_introspection_field_t *weights = get_f("weights");
  CHECK(weights && weights->header.type == DT_INTROSPECTION_TYPE_ARRAY
        && weights->Array.type == DT_INTROSPECTION_TYPE_DOUBLE && weights->Array.count == 2
        && weights->header.offset == offsetof(double_fixture_params_t, weights)
        && weights->header.size == sizeof(((double_fixture_params_t *)0)->weights)
        && weights->Array.field && weights->Array.field->header.type == DT_INTROSPECTION_TYPE_DOUBLE);

  double_fixture_params_t params;
  memset(&params, 0xa5, sizeof(params));
  params.before = 'a'; params.after = 'z';
  params.pair.radius = 64.; params.pair.slope = 1.25;
  params.precise = 1.234567890123; params.unbounded = -.75;
  params.weights[0] = -.2; params.weights[1] = .7;
  CHECK(get_p(&params, "pair.radius") == &params.pair.radius);
  CHECK(get_p(&params, "pair.slope") == &params.pair.slope);
  *(double *)get_p(&params, "pair.radius") = 127.125;
  CHECK(params.pair.radius == 127.125 && params.pair.slope == 1.25
        && params.precise == 1.234567890123 && params.unbounded == -.75
        && params.weights[0] == -.2 && params.weights[1] == .7
        && params.before == 'a' && params.after == 'z');
  puts("Double metadata retains native offsets, precision, bounds and parameter layout");
  return 0;
}
