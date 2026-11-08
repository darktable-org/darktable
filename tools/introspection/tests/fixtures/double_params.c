/* SPDX-License-Identifier: GPL-3.0-or-later */
#define N_(text) (text)
#define DT_MODULE_INTROSPECTION(version, params)

typedef struct dt_iop_module_t
{
  void *default_params;
} dt_iop_module_t;

/* The original CLAHE ABI is a pair of doubles. Embedding that layout after a
 * byte also exercises target-specific alignment without packing the struct. */
typedef struct double_pair_t
{
  double radius; // $MIN: 0.0 $MAX: 256.0 $DEFAULT: 64.0 $DESCRIPTION: "radius"
  double slope; // $MIN: 1.0 $MAX: 3.0 $DEFAULT: 1.25 $DESCRIPTION: "amount"
} double_pair_t;

typedef struct double_fixture_params_t
{
  char before;
  double_pair_t pair;
  double precise; // $MIN: -2.0 $MAX: 2.0 $DEFAULT: 1.234567890123 $DESCRIPTION: "precise"
  double unbounded;
  double weights[2]; // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0
  char after;
} double_fixture_params_t;

DT_MODULE_INTROSPECTION(1, double_fixture_params_t)
