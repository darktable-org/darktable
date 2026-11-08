/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "common/introspection.h"
#include <stdio.h>
#include <string.h>

int introspection_init(struct dt_iop_module_so_t *self, int api_version);
dt_introspection_field_t *get_f(const char *name);

/* Checks use runtime metadata produced by the real parser and C compiler.
 * They intentionally stay active when this test is built with NDEBUG. */
static int check_enum(const char *field_name, const dt_introspection_type_enum_tuple_t *expected,
                      size_t count)
{
  dt_introspection_field_t *field = get_f(field_name);
  if(!field || field->header.type != DT_INTROSPECTION_TYPE_ENUM || !field->Enum.values
     || field->Enum.entries != count)
  {
    fprintf(stderr, "%s: missing enum metadata or wrong number of choices\n", field_name);
    return 0;
  }
  for(size_t index = 0; index < count; index++)
  {
    const dt_introspection_type_enum_tuple_t *actual = &field->Enum.values[index];
    if(!actual->name || strcmp(actual->name, expected[index].name)
       || actual->value != expected[index].value || !actual->description
       || strcmp(actual->description, expected[index].description))
    {
      fprintf(stderr, "%s: choice %zu does not match its declared symbol, value and label\n",
              field_name, index);
      return 0;
    }
    const char *symbol = dt_introspection_get_enum_name(field, expected[index].value);
    int value = 0;
    if(!symbol || strcmp(symbol, expected[index].name)
       || !dt_introspection_get_enum_value(field, expected[index].name, &value)
       || value != expected[index].value)
    {
      fprintf(stderr, "%s: enum symbol/value lookup is inconsistent\n", field_name);
      return 0;
    }
  }
  if(field->Enum.values[count].name)
  {
    fprintf(stderr, "%s: enum choices lack the expected closing sentinel\n", field_name);
    return 0;
  }
  return 1;
}

int main(void)
{
  const dt_introspection_type_enum_tuple_t pair[] = {
    { "ANONYMOUS_PAIR_LOW", 3, "pair low" }, { "ANONYMOUS_PAIR_HIGH", 7, "pair high" }
  };
  const dt_introspection_type_enum_tuple_t triple[] = {
    { "ANONYMOUS_TRIPLE_LOW", -4, "triple low" }, { "ANONYMOUS_TRIPLE_MID", 5, "triple middle" },
    { "ANONYMOUS_TRIPLE_HIGH", 12, "triple high" }
  };
  const dt_introspection_type_enum_tuple_t named[] = {
    { "NAMED_MODE_FIRST", 100, "named first" }, { "NAMED_MODE_LAST", 200, "named last" }
  };
  if(introspection_init(NULL, DT_INTROSPECTION_VERSION)
     || !check_enum("pair", pair, 2) || !check_enum("triple", triple, 3)
     || !check_enum("pair_again", pair, 2) || !check_enum("named", named, 2)
     || !check_enum("named_again", named, 2)) return 1;
  puts("Anonymous and reused enum fields retain their declared symbols, values and labels");
  return 0;
}
