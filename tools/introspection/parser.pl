#!/usr/bin/perl
#  This file is part of darktable,
#  copyright (c) 2013-2014 tobias ellinghaus.
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

# 0 -- print nothing
# 1 -- print errors
# 2 -- print warnings
# 3 -- print debug info
our $ERROR_LEVEL = 0;

use strict;
use warnings;

use FindBin;
use lib "$FindBin::RealBin";

use scanner;
use parser;
use ast;
use code_gen;

my $base_dir = $ARGV[0];
my $input_file = $ARGV[1];
my $output_file = $ARGV[2];

if(!defined($base_dir) or !defined($input_file) or !defined($output_file))
{
  print STDERR "usage: parse.pl <base dir> <input file> <output_file>\n";
  exit(1);
}

# set the directory where to look for #includes
$scanner::folder = $base_dir;

read_file($input_file);

# dump_tokens(); exit 1;

my %types;
my $version = -1;
my $params_type;

while()
{
  @token = get_token();
  last if($token[$P_TYPE] == $T_NONE);
  if(istypedef(\@token))
  {
    my $ast = parse();
    if(defined($ast))
    {
      $ast->fix_types(\%types);
  #     $ast->print_tree(0);
  #     print "===========\n";
      $types{$ast->{name}} = \$ast;
    }
  }
  elsif(isdtmoduleintrospection(\@token))
  {
    ($version, $params_type) = parse_dt_module_introspection();
  }
}

open my $OUT, '>', $output_file;

if(defined($params_type))
{
  # needed for variable metadata like min, max, default and description
  parse_comments();

  my $code_generated = 0;
  my $params = $types{$params_type};
  if(defined($params) && $$params->check_tree())
  {
    $code_generated = code_gen::print_code($OUT, $$params, $input_file, $version, $params_type);
  }

  if(!$code_generated)
  {
    print STDERR "error: can't generate introspection data for type `$params_type'.\n";
    code_gen::print_fallback($OUT, $input_file, $version, $params_type);
  }
}
else
{
  my $source_file_name = $input_file;
  if($input_file =~ /([^\/]*)$/) { $source_file_name = $1; }
  print STDERR "no introspection requested for $source_file_name.\n";
  print $OUT "/* no introspection requested for $source_file_name */\n#include \"$input_file\"\n";
}

close $OUT;

################# some debug functions #################

# sub print_token
# {
#   my $token = shift;
#   print @$token[0]." : ".@$token[1]." : ".@$token[2]." : ".@$token[3]."\n";
# }
#
# sub dump_tokens
# {
#   while()
#   {
#     my @token = get_token();
#     last if($token[$P_TYPE] == $T_NONE);
#     print_token(\@token);
#   }
# }
#
# sub dump_comments
# {
#   my $lineno = 0;
#   foreach(@comments)
#   {
#     if(defined($_))
#     {
#       print "$lineno:\n";
#       my %line = %{$_};
#       foreach(@{$line{raw}})
#       {
#         print "  ".$_."\n";
#       }
#     }
#     $lineno++;
#   }
# }

# modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
# vim: shiftwidth=2 expandtab tabstop=2 cindent
# kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
