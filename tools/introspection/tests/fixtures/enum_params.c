/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The generator reads this as a real module source. Keep the module shell
 * minimal so the generated metadata can be tested without the GUI runtime. */
#define N_(text) (text)
#define DT_MODULE_INTROSPECTION(version, params)

typedef struct dt_iop_module_t
{
  void *default_params;
} dt_iop_module_t;

typedef enum
{
  ANONYMOUS_PAIR_LOW = 3,  // $DESCRIPTION: "pair low"
  ANONYMOUS_PAIR_HIGH = 7, // $DESCRIPTION: "pair high"
} anonymous_pair_t;

typedef enum
{
  ANONYMOUS_TRIPLE_LOW = -4, // $DESCRIPTION: "triple low"
  ANONYMOUS_TRIPLE_MID = 5,  // $DESCRIPTION: "triple middle"
  ANONYMOUS_TRIPLE_HIGH = 12 // $DESCRIPTION: "triple high"
} anonymous_triple_t;

typedef enum named_mode_t
{
  NAMED_MODE_FIRST = 100, // $DESCRIPTION: "named first"
  NAMED_MODE_LAST = 200   // $DESCRIPTION: "named last"
} named_mode_t;

typedef struct enum_fixture_params_t
{
  anonymous_pair_t pair;
  anonymous_triple_t triple;
  anonymous_pair_t pair_again;
  named_mode_t named;
  named_mode_t named_again;
} enum_fixture_params_t;

DT_MODULE_INTROSPECTION(1, enum_fixture_params_t)
