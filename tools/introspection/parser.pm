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

package parser;

use strict;
use warnings;

use scanner;

use Exporter;
our @ISA = 'Exporter';
our @EXPORT = qw( parse parse_dt_module_introspection parse_comments get_typedef );


sub print_error
{
  my $message = shift;
  print STDERR "error: ".$token[$P_FILENAME].":".$token[$P_LINENO].": $message\n" if($main::ERROR_LEVEL >= 1);
}

################# the exposed parser functions #################

sub parse
{
  return parse_typedef();
}

# DT_MODULE_INTROSPECTION(<version>, <params_type>)
sub parse_dt_module_introspection
{
  if(!isdtmoduleintrospection(@token)) { print_error "expecting 'DT_MODULE_INTROSPECTION', found '".token2string(@token)."'"; return; }
  advance_token();

  if(!isleftround(@token)) { print_error "expecting '(', found '".token2string(@token)."'"; return; }
  advance_token();

  if(!isinteger(@token)) { print_error "expecting an integer literal, found '".token2string(@token)."'"; return; }
  my $version = $token[$P_VALUE];
  advance_token();

  if(!iscomma(@token)) { print_error "expecting ',', found '".token2string(@token)."'"; return; }
  advance_token();

  my $params_type = parse_id();
  return if(!defined($params_type));

  if(!isrightround(@token)) { print_error "expecting ')', found '".token2string(@token)."'"; return; }

  return ($version, $params_type);
}

sub parse_comments
{
  my $lineno = 0;
  foreach(@comments)
  {
    if(defined($_))
    {
      my $joined_line = "";
      foreach(@{$_->{raw}})
      {
        $joined_line .= $_;
      }
      if($joined_line =~ /\$MIN[\s]*:[\s]*([^\s,]+)/) { $_->{min} = $1; }
      if($joined_line =~ /\$MAX[\s]*:[\s]*([^\s,]+)/) { $_->{max} = $1; }
      if($joined_line =~ /\$DEFAULT[\s]*:[\s]*([^\s,]+)/) { $_->{default} = $1; }
      if($joined_line =~ /\$DESCRIPTION[\s]*:[\s]*"([^"]+)"/) { $_->{description} = $1; }
      elsif($joined_line =~ /\$DESCRIPTION[\s]*:[\s]*([^"\s,]+)/) { $_->{description} = $1; }
    }
    $lineno++;
  }
}

################# the parser internals #################

sub advance_token
{
  @token = get_token();
  if($token[$P_TYPE] == $T_NONE) { print_error "premature EOF"; }
}

sub parse_id
{
  if(!isid(@token)) { print_error "expecting identifier, found '".token2string(@token)."'"; return; }
  my $id = $token[$P_VALUE];
  advance_token();
  return $id;
}

sub parse_typedef
{
  if(!istypedef(@token)) { print_error "expecting 'typedef', found '".token2string(@token)."'"; return; }
  advance_token();

  my $type = parse_type();
  return if(!defined($type));

  my $name = parse_id();
  return if(!defined($name));

  if(!issemicolon(@token)) { print_error "expecting ';', found '".token2string(@token)."'"; return; }

  return ast_typedef_node->new(\@token, $type, $name);
}

sub parse_type
{
  my $ast;
  if(isid(@token))
  {
    # some custom type we don't know. parsing that is out of scope for us. however, we try to fix these later
    $ast = ast_type_typedef_node->new(\@token);
    return if(!defined($ast));
    advance_token();
  }
  elsif($token[$P_TYPE] == $T_KEYWORD)
  {
    if   ($token[$P_VALUE] == $K_UNSIGNED) { advance_token(); $ast = parse_type(); return if(!defined($ast)); return if(!$ast->set_unsigned()); }
    elsif($token[$P_VALUE] == $K_SIGNED) { advance_token(); $ast = parse_type(); return if(!defined($ast)); return if(!$ast->set_signed()); }
    elsif($token[$P_VALUE] == $K_GBOOLEAN) { $ast = ast_type_gboolean_node->new(\@token); advance_token(); }
    elsif($token[$P_VALUE] == $K_CHAR) { $ast = ast_type_char_node->new(\@token); advance_token(); }
    elsif($token[$P_VALUE] == $K_SHORT) { $ast = ast_type_short_node->new(\@token); advance_token(); }
    elsif($token[$P_VALUE] == $K_USHORT) { $ast = ast_type_short_node->new(\@token); $ast->set_unsigned(); advance_token(); }
    elsif($token[$P_VALUE] == $K_INT) { $ast = ast_type_int_node->new(\@token); advance_token(); }
    elsif($token[$P_VALUE] == $K_UINT) { $ast = ast_type_int_node->new(\@token); $ast->set_unsigned(); advance_token(); }
    elsif($token[$P_VALUE] == $K_LONG) { $ast = ast_type_long_node->new(\@token); advance_token(); }
    elsif($token[$P_VALUE] == $K_FLOAT) {
      my @next_token = look_ahead(1);
      if($next_token[$P_TYPE] == $T_KEYWORD && $next_token[$P_VALUE] == $K_COMPLEX)
      {
        advance_token();
        $ast = ast_type_float_complex_node->new(\@token);
      }
      else
      {
        $ast = ast_type_float_node->new(\@token);
      }
      advance_token();
    }
    elsif($token[$P_VALUE] == $K_DOUBLE) { $ast = ast_type_double_node->new(\@token);  advance_token(); }
    elsif($token[$P_VALUE] == $K_STRUCT || $token[$P_VALUE] == $K_UNION) { $ast = parse_struct_or_union(); return if(!defined($ast)); }
    elsif($token[$P_VALUE] == $K_CONST) { advance_token(); $ast = parse_type(); return if(!defined($ast)); $ast->set_const(); }
    elsif($token[$P_VALUE] == $K_STATIC) { advance_token(); $ast = parse_type(); return if(!defined($ast)); $ast->set_static(); }
    elsif($token[$P_VALUE] == $K_ENUM) { $ast = parse_enum(); return if(!defined($ast)); }
    elsif($token[$P_VALUE] == $K_VOID) { $ast = ast_type_void_node->new(\@token); advance_token(); }
    else { print_error "'".token2string(@token)."' is not a valid type"; return; }
  }
  else { print_error "expecting a standard type or an identifier, found '".token2string(@token)."'"; return; }

  return $ast;
}

sub parse_struct_or_union
{
  my $name;
  my $type;

  if(isstruct(@token) || isunion(@token)) { $type = $token[$P_VALUE]; }
  else { print_error "expecting 'struct', found '".token2string(@token)."'"; return; }

  advance_token();

  if(isid(@token)) { $name = parse_id(); return if(!defined($name));}

  if(!isleftcurly(@token)) { print_error "expecting '{', found '".token2string(@token)."'"; return; }

  advance_token();

  my @decl_list = parse_struct_decl_list();
  return if(!@decl_list);

  if(!isrightcurly(@token)) { print_error "expecting '}', found '".token2string(@token)."'"; return; }

  my $ast;
  if($type == $K_STRUCT) { $ast = ast_type_struct_node->new(\@token, $name, \@decl_list); }
  else { $ast = ast_type_union_node->new(\@token, $name, \@decl_list); }

  advance_token();

  return $ast;
}

sub parse_struct_decl_list
{
  my @decl_list;
  while(!isrightcurly(@token))
  {
    my $type = parse_type();
    return if(!defined($type));
    my @declarators = parse_struct_declarator_list();
    return if(!@declarators);
    if(!issemicolon(@token)) { print_error "expecting ';', found '".token2string(@token)."'"; return; }
    advance_token();

    # we want to split something like "int foo, bar;" into "int foo; int bar;"
    foreach(@declarators)
    {
      my $declaration = $_;
      my @tmp_token = ($declaration->{lineno}, $declaration->{filename}, $T_NONE, 0);
      push(@decl_list, ast_declaration_node->new(\@tmp_token, $type, $declaration));
    }
  }
  return @decl_list;
}

sub parse_struct_declarator_list
{
  my @declarators;
  my $declarator = parse_struct_declarator();
  return if(!defined($declarator));
  push(@declarators, $declarator);
  while(iscomma(@token))
  {
    advance_token();
    my $declarator = parse_struct_declarator();
    return if(!defined($declarator));
    push(@declarators, $declarator);
  }
  return @declarators;
}

# we don't support bitfields, so this is a simplified version of what it should be
sub parse_struct_declarator
{
  return parse_declarator();
}

sub parse_enum
{
  my $name;

  if(!isenum(@token)) { print_error "expecting 'enum', found '".token2string(@token)."'"; return; }

  advance_token();

  if(isid(@token)) { $name = parse_id(); return if(!defined($name)); }

  if(!isleftcurly(@token)) { print_error "expecting '{', found '".token2string(@token)."'"; return; }

  advance_token();

  my @enumerator_list = parse_enumerator_list();
  return if(!@enumerator_list);

  if(!isrightcurly(@token)) { print_error "expecting '}', found '".token2string(@token)."'"; return; }

  my $ast = ast_type_enum_node->new(\@token, $name, \@enumerator_list);

  advance_token();

  return $ast;
}

sub parse_enumerator_list
{
  my @enumerator_list;
  while(!isrightcurly(@token))
  {
    my $id = parse_id();
    return if(!defined($id));
    push(@enumerator_list, $id);
    if(isequal(@token))
    {
      # TODO: for now skip everything until a ',' or '}' is found
      while(!iscomma(@token) && !isrightcurly(@token)) { advance_token(); }
    }
    if(iscomma(@token)) { advance_token(); }
  }
  return @enumerator_list;
}

sub parse_declarator
{
  if(isasterisk(@token))
  {
    my $pointer = parse_pointer();
    return if(!defined($pointer));
  }
  return parse_direct_declarator();
}

sub parse_pointer
{
  if(!isasterisk(@token)) { print_error "expecting '*', found '".token2string(@token)."'"; return; }
  print_error "found a pointer. you really shouldn't do that (if it's in the type used for parameters).\n";
  return;

  while(isasterisk(@token) || isconst(@token) || isvolatile(@token))
  {
    advance_token();
  }
}

# only a subset of the C standard is supported here: <id> '[' <integer> ']'
sub parse_direct_declarator
{
  my $declarator;
  if(isleftround(@token))
  {
    advance_token();
    $declarator = parse_declarator();
    return if(!defined($declarator));
    if(!isrightround(@token)) { print_error "expecting ')', found '".token2string(@token)."'"; return; }
    advance_token();
  }
  else
  {
    my @dimension_list;
    my $id = parse_id();
    return if(!defined($id));
    while(isleftsquare(@token))
    {
      my $size = parse_array();
      return if(!defined($size));
      push(@dimension_list, $size);
    }
    $declarator = ast_declarator_node->new(\@token, $id, \@dimension_list);
  }
  return $declarator;
}

sub parse_array
{
  my $size = "";
  my $depth = 0;
  my $space = "";
  if(!isleftsquare(@token)) { print_error "expecting '[', found '".token2string(@token)."'"; return; }

  advance_token();

  while(!(isrightsquare(@token) && $depth == 0))
  {
    $depth++ if(isleftsquare(@token));
    $depth-- if(isrightsquare(@token));
    $size .= $space.token2string(@token);
    $space = " ";
    advance_token();
  }

  if(!isrightsquare(@token)) { print_error "expecting ']', found '".token2string(@token)."'"; return; }

  advance_token();
  return $size;
}

1;

# modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
# vim: shiftwidth=2 expandtab tabstop=2 cindent
# kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
