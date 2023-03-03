#  This file is part of darktable,
#  copyright (c) 2013-2020 tobias ellinghaus.
#
#  darktable is free software: you can redistribute it and/or modify
#  it under the terms of the GNU General Public License as published by
#  the Free Software Foundation, either version 3 of the License, or
#  (at your option) any later version.
#
#  darktable is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  GNU General Public License for more details.
#
#  You should have received a copy of the GNU General Public License
#  along with darktable.  If not, see <http://www.gnu.org/licenses/>.

use ast;

package code_gen;

my $DT_INTROSPECTION_VERSION = 8;

my $is_old_gplusplus = "defined(__cplusplus) && !defined(__clang__) && defined(__GNUC__) && ((__GNUC__*100 + __GNUC_MINOR__) <= 406)";
my $is_not_old_gplusplus = "!( defined(__cplusplus) && !defined(__clang__) && defined(__GNUC__) && ((__GNUC__*100 + __GNUC_MINOR__) <= 406) )";

sub print_fallback
{
  my ($OUT, $input_file, $version, $params_type) = @_;

  print $OUT <<END;
/*
 * this code is auto generated. do not edit. change the sources in tools/introspection/ instead.
 * there were errors when generating this code. giving up
 */
#warning "could not generate introspection for $input_file"

#include "$input_file"

#ifdef __cplusplus
extern "C"
{
#endif

#include "common/introspection.h"

static dt_introspection_field_t introspection_linear[2]
#if $is_not_old_gplusplus
= {
  { .Opaque = { {DT_INTROSPECTION_TYPE_OPAQUE, (char*)"", (char*)"", (char*)"", (char*)"", sizeof($params_type), 0, NULL}, } },
  { .header = {DT_INTROSPECTION_TYPE_NONE, NULL, NULL, NULL, NULL, 0, 0, NULL} }
}
#endif
;


static dt_introspection_t introspection = {
  $DT_INTROSPECTION_VERSION,
  $version,
  "$params_type",
  sizeof($params_type),
  &introspection_linear[0],
  sizeof(dt_iop_module_t),
  G_STRUCT_OFFSET(dt_iop_module_t, default_params)
};

dt_introspection_field_t* get_introspection_linear()
{
  return introspection_linear;
}

dt_introspection_t* get_introspection()
{
  return &introspection;
}

int introspection_init(struct dt_iop_module_so_t *self, int api_version)
{
  // here we check that the generated code matches the api at compile time and also at runtime
  if(introspection.api_version != DT_INTROSPECTION_VERSION || api_version != DT_INTROSPECTION_VERSION)
    return 1;

#if $is_old_gplusplus
  introspection_linear[0].Opaque = { {DT_INTROSPECTION_TYPE_OPAQUE, (char*)"", (char*)"", (char*)"", sizeof($params_type), 0, NULL}, };
  introspection_linear[1].header = {DT_INTROSPECTION_TYPE_NONE, NULL, NULL, NULL, NULL, 0, 0, NULL};
#endif

  for(int i = 0; i <= 1; i++)
    introspection_linear[i].header.so = self;

  return 0;
}

void * get_p(const void * param, const char * name)
{
  return NULL;
}

dt_introspection_field_t * get_f(const char * name)
{
  return NULL;
}

#ifdef __cplusplus
}
#endif

END

}

sub print_code
{
  my ($OUT, $root, $input_file, $version, $params_type) = @_;

  # in our model we want the params to be a typedef of a struct
  return 0 if(ref $root ne "ast_typedef_node");
  my $start_node = $root->{type};
  return 0 if(ref $start_node ne "ast_type_struct_node");

  # collect data from the ast
  $root->get_introspection_code("", $params_type);

  my $max_linear = @ast::linear;

  # print c code
  print $OUT <<END;
/*
 * this code is auto generated. do not edit. change the sources in tools/introspection/ instead.
 *
 * the parse tree:
 *
END
  ast::print_tree($OUT, $root, " * ");
  print $OUT <<END;
 *
 */

#include "$input_file"

#ifdef __cplusplus
extern "C"
{
#endif

#include <string.h>
#include "common/introspection.h"

static dt_introspection_field_t introspection_linear[$max_linear+1]
#if $is_not_old_gplusplus
= {
END

  foreach(@ast::linear)
  {
    print $OUT "{\n  $_\n},\n";
  }
  print $OUT <<END;
  { .header = {DT_INTROSPECTION_TYPE_NONE, NULL, NULL, NULL, NULL, 0, 0, NULL} }
}
#endif
;

static dt_introspection_t introspection = {
  $DT_INTROSPECTION_VERSION,
  $version,
  "$params_type",
  sizeof($params_type),
  &introspection_linear[$max_linear-1],
  sizeof(dt_iop_module_t),
  G_STRUCT_OFFSET(dt_iop_module_t, default_params)
};

dt_introspection_field_t* get_introspection_linear()
{
  return introspection_linear;
}

dt_introspection_t* get_introspection()
{
  return &introspection;
}

int introspection_init(struct dt_iop_module_so_t *self, int api_version)
{
  // here we check that the generated code matches the api at compile time and also at runtime
  if(introspection.api_version != DT_INTROSPECTION_VERSION || api_version != DT_INTROSPECTION_VERSION)
    return 1;

#if $is_old_gplusplus
END
  my $i = 0;
  foreach(@ast::linear)
  {
    print $OUT "  introspection_linear[$i]$_;\n";
    $i++;
  }
  print $OUT <<END;
  introspection_linear[$max_linear].header = {DT_INTROSPECTION_TYPE_NONE, NULL, NULL, NULL, NULL, 0, 0, NULL};
#endif

  for(int i = 0; i <= $max_linear; i++)
    introspection_linear[i].header.so = self;

END

  foreach(@ast::arrays)
  {
    print $OUT "  $_\n";
  }
  print $OUT "\n";

  foreach(@ast::assignments)
  {
    print $OUT "  $_\n";
  }

  print $OUT <<END;

  return 0;
}

void * get_p(const void * param, const char * name)
{
  $params_type * p = ($params_type*)param;
END
  print $OUT " ";
  foreach(@ast::varnames)
  {
    print $OUT " if(!strcmp(name, \"@$_[1]\"))\n    return &(p->@$_[1]);\n  else";
  }
  print $OUT <<END;

    return NULL;
}

dt_introspection_field_t * get_f(const char * name)
{
END
  print $OUT " ";
  foreach(@ast::varnames)
  {
    print $OUT " if(!g_ascii_strcasecmp(name, \"@$_[1]\"))\n    return &(introspection_linear[@$_[0]]);\n  else";
  }
  print $OUT <<END;

    return NULL;
}

#ifdef __cplusplus
}
#endif

END
  return 1;
}

1;

# modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
# vim: shiftwidth=2 expandtab tabstop=2 cindent
# kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
