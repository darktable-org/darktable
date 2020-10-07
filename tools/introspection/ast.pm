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

use scanner;

package ast;

my $INDENT = 2;

my $OUT = \*STDOUT;

# ugly, not thread safe global variables:
our @varnames;
our @linear;
our @arrays;
our @assignments;
my $linearisation_pos;

my %enum_arrays; # a mapping of typename to linearisation_pos. this allows us to re-use enum value definitions

sub print_debug
{
  my ($self, $message) = @_;
  print STDERR $message if($main::ERROR_LEVEL >= 3);
}

sub print_out
{
  my $message = shift;
  print $OUT $message;
}

sub print_tree
{
  my ($NEW_OUT, $ast, $prefix) = @_;
  my $OLD_OUT = $OUT;
  $OUT = $NEW_OUT;
  $ast->print_tree($prefix, 0);
  $OUT = $OLD_OUT;
}

sub mark_for_translation
{
  my $string = shift;

  my $GETTEXT_CONTEXT = "introspection description";

  my $result = "(char*)\"$string\"";
  # we do not want to support a context as it break all translations see #5498
  # $result = "NC_(\"$GETTEXT_CONTEXT\", $result)" if($string ne "");
  $result = "N_($result)" if($string ne "");

  return $result;
}

#################### BASE ####################

package ast_node;

@ISA = 'ast';

sub new
{
  my ($self, $token) = @_;
  my $reference = {};
  my $lineno = $token->[$parser::P_LINENO];
  my $filename = $token->[$parser::P_FILENAME];

  bless($reference, $self);
  $reference->{lineno} = $lineno;
  $reference->{filename} = $filename;
  $reference->{location} = $filename.":".$lineno;

  $self->print_debug("$filename:$lineno -- new ".(ref $reference)."\n");

  return $reference;
}

sub print_error
{
  my ($self, $message) = @_;
  print STDERR "error: ".$self->{location}.": $message\n" if($main::ERROR_LEVEL >= 1);
}

sub print_warning
{
  my ($self, $message) = @_;
  print STDERR "warning: ".$self->{location}.": $message\n" if($main::ERROR_LEVEL >= 2);
}

sub print_tree
{
  my ($self, $prefix, $indent) = @_;
  my $spaces = " "x$indent;
  ast::print_out($prefix.$spaces."FIXME: ast_node -- this type shouldn't be instantiated\n");
}

sub get_introspection_code
{
  my $self = shift;
  my $t = ref $self;
  $self->print_debug("FIXME: $t isn't handled in get_introspection_code\n");
}

sub get_description
{
  my $self = shift;

  my %comment_line = %{$scanner::comments{$self->{filename}}[$self->{lineno}]};
  my $description = "";
  $description = $comment_line{description} if(defined($comment_line{description}));
  return $description;
}

sub get_default
{
  my $self = shift;

  my %comment_line = %{$scanner::comments{$self->{filename}}[$self->{lineno}]};
  $default = $comment_line{default};
  return $default;
}

sub get_range
{
  my $self = shift;
  my %comment_line = %{$scanner::comments{$self->{filename}}[$self->{lineno}]};

  $min = $comment_line{min};
  $max = $comment_line{max};

  return ($min, $max);
}

sub add_to_linear
{
  my ($self, $varname, $line) = @_;
  push(@linear, $line);
  $self->{linearisation_pos} = $linearisation_pos;
  push(@varnames, [$linearisation_pos, $varname]) if($varname ne "");
  $linearisation_pos++;
}

#################### TYPEDEF ####################

package ast_typedef_node;

@ISA = 'ast_node';

sub new
{
  my ($self, $token, $type, $name) = @_;

  my $reference = $self->SUPER::new($token);
  bless($reference, $self);

  $reference->{type} = $type;
  $reference->{name} = $name;

  return $reference;
}

sub fix_types
{
  my ($self, $types_ref) = @_;
  if((ref $self->{type}) eq "ast_type_typedef_node")
  {
    my $type = $types_ref->{$self->{type}->{name}};
    if(defined($type))
    {
      $self->{type} = $$type->{type};
    }
  }
  else
  {
    $self->{type}->fix_types($types_ref);
  }
}

sub check_tree
{
  my $self = shift;
  return $self->{type}->check_tree();
}

sub print_tree
{
  my ($self, $prefix, $indent) = @_;
  my $spaces = " "x$indent;
  ast::print_out($prefix.$spaces."typedef\n");
  $self->{type}->print_tree($prefix, $indent+$INDENT);
  ast::print_out($prefix.$spaces.$self->{name}."\n");
}

sub get_introspection_code
{
  my ($self, $name_prefix, $params_type) = @_;
  $params_type = $self->{name};

  $linearisation_pos = 0;

  # we have to add the outermost struct here
  my $description = $self->get_description();
  $description = ast::mark_for_translation($description);
  my $header = "DT_INTROSPECTION_TYPE_STRUCT, (char*)\"$self->{name}\", (char*)\"\", (char*)\"\", $description, sizeof($params_type), 0, NULL";
  my $specific = $self->{type}->get_introspection_code($name_prefix, $params_type);
  my $linear_line = ".Struct = {\n    { $header },\n    $specific\n  }";
  $self->{type}->add_to_linear("", $linear_line);
}

#################### TYPE BASE ####################

package ast_type_node;

@ISA = 'ast_node';

sub new
{
  my ($self, $token) = @_;

  my $reference = $self->SUPER::new($token);
  bless($reference, $self);

  $reference->{static} = 0;
  $reference->{const} = 0;

  return $reference;
}

sub set_const
{
  my $self = shift;
  $self->{const} = 1;
}

sub set_static
{
  my $self = shift;
  $self->{static} = 1;
}

sub set_unsigned
{
  my $self = shift;
  $self->print_error("unexpected 'unsigned'");
  return 0;
}

sub set_signed
{
  my $self = shift;
  $self->print_error("unexpected 'signed'");
  return 0;
}

sub fix_types
{
 # do nothing in the general case
}

sub check_tree
{
  return 1;
}

sub get_type
{
  $self->print_debug("FIXME: ast_type_node -- this type shouldn't be instantiated\n");
}

sub get_type_name
{
  my $self = shift;
  return lc $self->get_type();
}

sub get_static_const
{
  my $self = shift;
  my @result;
  push(@result, "static") if($self->{static});
  push(@result, "const") if($self->{const});
  push(@result, "unsigned") if($self->{unsigned} == 1);
  my $string = join(" ", @result);
  $string .= " " if(@result > 0);
  return $string;
}

sub print_tree
{
  my ($self, $prefix, $indent) = @_;
  my $spaces = " "x$indent;
  my $extra = $self->get_static_const();

  ast::print_out($prefix.$spaces.$extra.$self->{code_type}."\n");
}

#################### NUMERIC WRAPPER TYPE ####################
# the only reason to have this is to have common get_...() functions that call get_limits()

package ast_type_numeric_node;

@ISA = 'ast_type_node';

sub get_default
{
  my $self = shift;

  my $default = $self->SUPER::get_default();
  my ($_min, $_max), $default = $self->get_limits() unless(defined($default));
  return $default;
}

sub get_range
{
  my $self = shift;

  my ($min, $max) = $self->SUPER::get_range();
  my ($min_limits, $max_limits, $default_limits) = $self->get_limits();

  $min = $min_limits unless(defined($min));
  $max = $max_limits unless(defined($max));

  return ($min, $max);
}

sub get_introspection_code
{
  my ($self, $name_prefix, $params_type, $declaration) = @_;

  my ($min, $max) = $self->get_range();
  my ($min_declaration, $max_declaration) = $declaration->get_range();

  $min = $min_declaration if(defined($min_declaration));
  $max = $max_declaration if(defined($max_declaration));

  my $default = $self->get_default();
  my $default_declaration = $declaration->get_default();

  $default = $default_declaration if(defined($default_declaration));

  return "/*Min*/ $min, /*Max*/ $max, /*Default*/ $default";
}

#################### TYPEDEF TYPE ####################

package ast_type_typedef_node;

@ISA = 'ast_type_node';

sub new
{
  my ($self, $token) = @_;
  my $name = $token->[$parser::P_VALUE];

  my $reference = $self->SUPER::new($token);
  bless($reference, $self);

  $reference->{name} = $name;

  return $reference;
}

sub get_type
{
  return "Opaque";
}

sub get_type_name
{
  my $self = shift;
  return $self->{name};
}

sub print_tree
{
  my ($self, $prefix, $indent) = @_;
  my $spaces = " "x$indent;
  my $extra = $self->get_static_const();
  ast::print_out($prefix.$spaces.$extra.$self->{name}."\n");
}

sub get_introspection_code
{
  return "/* no data for this type */";
}

#################### CHAR / INT8_T TYPE ####################

package ast_type_char_node;

@ISA = 'ast_type_numeric_node';

sub new
{
  my ($self, $token) = @_;

  my $reference = $self->SUPER::new($token);
  bless($reference, $self);

  # here "unsigned" is a tri state: 0: signed, -1: unsigned, 2: default.
  # notice that "default" can be either one, depending on platform!
  # because we are using "int8_t" and "uint8_t" for explicitly specified signed cases
  # we never generate "unsigned char", thus the unsigned = 1 shouldn't happen for this type!
  $reference->{unsigned} = 2; # platform specific "char"
  $reference->{code_type} = "char";

  return $reference;
}

# make this a "uint8_t"
sub set_unsigned
{
  my $self = shift;
  $self->{unsigned} = -1;
  $self->{code_type} = "uint8_t";
  return 1;
}

# make this an "int8_t"
sub set_signed
{
  my $self = shift;
  $self->{unsigned} = 0;
  $self->{code_type} = "int8_t";
  return 1;
}

# we still use UChar/Char types of the union. only the content is different.
# TODO: shall we change that?
sub get_type
{
  my $self = shift;
  return "Int8" if($self->{unsigned} == 0);
  return "UInt8" if($self->{unsigned} == -1);
  return "Char";
}

sub get_limits
{
  my $self = shift;
  return ("G_MININT8", "G_MAXINT8", "0") if($self->{unsigned} == 0);
  return ("0", "G_MAXUINT8", "0") if($self->{unsigned} == -1);
  return ("CHAR_MIN", "CHAR_MAX", "0");
}

#################### SHORT TYPE ####################

package ast_type_short_node;

@ISA = 'ast_type_numeric_node';

sub new
{
  my ($self, $token) = @_;

  my $reference = $self->SUPER::new($token);
  bless($reference, $self);

  $reference->{unsigned} = 0;
  $reference->{code_type} = "short";

  return $reference;
}

sub set_unsigned
{
  my $self = shift;
  $self->{unsigned} = 1;
  return 1;
}

sub set_signed
{
  my $self = shift;
  $self->{unsigned} = 0;
  return 1;
}

sub get_type
{
  my $self = shift;
  return "UShort" if($self->{unsigned});
  return "Short";
}

sub get_limits
{
  my $self = shift;
  return ("0", "G_MAXUSHORT", "0") if($self->{unsigned});
  return ("G_MINSHORT", "G_MAXSHORT", "0");
}

#################### INT TYPE ####################

package ast_type_int_node;

@ISA = 'ast_type_numeric_node';

sub new
{
  my ($self, $token) = @_;

  my $reference = $self->SUPER::new($token);
  bless($reference, $self);

  $reference->{unsigned} = 0;
  $reference->{code_type} = "int";

  return $reference;
}

sub set_unsigned
{
  my $self = shift;
  $self->{unsigned} = 1;
  return 1;
}

sub set_signed
{
  my $self = shift;
  $self->{unsigned} = 0;
  return 1;
}

sub get_type
{
  my $self = shift;
  return "UInt" if($self->{unsigned});
  return "Int";
}

sub get_limits
{
  my $self = shift;
  return ("0", "G_MAXUINT", "0") if($self->{unsigned});
  return ("G_MININT", "G_MAXINT", "0");
}

#################### VOID TYPE ####################

package ast_type_void_node;

@ISA = 'ast_type_node';

sub new
{
  my ($self, $token) = @_;

  my $reference = $self->SUPER::new($token);
  bless($reference, $self);

  return $reference;
}

sub get_type
{
  return "Void";
}

sub get_default
{
  # no idea if that makes sense. having void in params doesn't sound that good of an idea in the first place
  return "TODO";
}

sub print_tree
{
  my ($self, $prefix, $indent) = @_;
  my $spaces = " "x$indent;
  my $extra = $self->get_static_const();
  ast::print_out($prefix.$spaces.$extra."void\n");
}

sub get_introspection_code
{
  return "TODO"; # do we even allow these?
}

#################### LONG TYPE ####################

package ast_type_long_node;

@ISA = 'ast_type_numeric_node';

sub new
{
  my ($self, $token) = @_;

  my $reference = $self->SUPER::new($token);
  bless($reference, $self);

  $reference->{unsigned} = 0;
  $reference->{code_type} = "long";

  return $reference;
}

sub set_unsigned
{
  my $self = shift;
  $self->{unsigned} = 1;
  return 1;
}

sub set_signed
{
  my $self = shift;
  $self->{unsigned} = 0;
  return 1;
}

sub get_type
{
  my $self = shift;
  return "ULong" if($self->{unsigned});
  return "Long";
}

sub get_limits
{
  my $self = shift;
  return ("0", "G_MAXULONG", "0") if($self->{unsigned});
  return ("G_MINLONG", "G_MAXLONG", "0");
}

#################### FLOAT TYPE ####################

package ast_type_float_node;

@ISA = 'ast_type_numeric_node';

sub new
{
  my ($self, $token) = @_;

  my $reference = $self->SUPER::new($token);
  bless($reference, $self);

  $reference->{code_type} = "float";

  return $reference;
}

sub get_type
{
  return "Float";
}

sub get_limits
{
  my $self = shift;
  return ("-G_MAXFLOAT", "G_MAXFLOAT", "0.0");
}

#################### DOUBLE TYPE ####################

package ast_type_double_node;

@ISA = 'ast_type_numeric_node';

sub new
{
  my ($self, $token) = @_;

  my $reference = $self->SUPER::new($token);
  bless($reference, $self);

  $reference->{code_type} = "double";

  return $reference;
}

sub get_type
{
  return "Double";
}

sub check_tree
{
  my $self = shift;
  $self->print_warning("'double' shouldn't be used due to different padding on 32 and 64 bit platforms");
  return 0;
}

sub get_limits
{
  my $self = shift;
  return ("-G_MINDOUBLE", "G_MAXDOUBLE", "0.0");
}

#################### FLOAT COMPLEX TYPE ####################

package ast_type_float_complex_node;

@ISA = 'ast_type_numeric_node';

sub new
{
  my ($self, $token) = @_;

  my $reference = $self->SUPER::new($token);
  bless($reference, $self);

  $reference->{code_type} = "float complex";

  return $reference;
}

sub get_type
{
  return "FloatComplex";
}

sub get_limits
{
  my $self = shift;
  return ("-G_MAXFLOAT + -G_MAXFLOAT * _Complex_I", "G_MAXFLOAT + G_MAXFLOAT * _Complex_I", "0.0 + 0.0 * _Complex_I");
}

#################### GBOOLEAN TYPE ####################

package ast_type_gboolean_node;

@ISA = 'ast_type_node';

sub new
{
  my ($self, $token) = @_;

  my $reference = $self->SUPER::new($token);
  bless($reference, $self);

  $reference->{code_type} = "boolean";

  return $reference;
}

sub get_type
{
  return "Bool";
}

sub get_default
{
  my $self = shift;

  my $default = $self->SUPER::get_default();
  $default = "FALSE" unless(defined($default));

  return $default;
}

sub get_introspection_code
{
  my ($self, $name_prefix, $params_type, $declaration) = @_;

  my $default = $self->get_default();

  return "/*Default*/ $default";
}

#################### STRUCT OR UNION TYPE ####################

package ast_type_struct_or_union_node;

@ISA = 'ast_type_node';

sub new
{
  my ($self, $token, $name, $decl_list) = @_;

  my $reference = $self->SUPER::new($token);
  bless($reference, $self);

  $reference->{name} = $name;
  $reference->{decl_list} = $decl_list;

  return $reference;
}

sub fix_types
{
  my ($self, $types_ref) = @_;
  foreach(@{$self->{decl_list}})
  {
    $_->fix_types($types_ref);
  }
}

sub check_tree
{
  my $self = shift;
  my $result = 1;
  foreach(@{$self->{decl_list}})
  {
    my $res = $_->check_tree();
    $result = 0 if (!$res);
  }
  return $result;
}

sub get_type
{
  $self->print_debug("FIXME: ast_type_struct_or_union_node -- this type shouldn't be instantiated\n");
}

sub get_type_name
{
  my $self = shift;
  return $self->{name};
}

sub print_tree
{
  my ($self, $prefix, $indent) = @_;
  my $spaces = " "x$indent;
  my $extra = $self->get_static_const();
  ast::print_out($prefix.$spaces.$extra.$self->{type}." ".$self->{name}."\n");
  foreach(@{$self->{decl_list}})
  {
    $_->print_tree($prefix, $indent+$INDENT);
  }
}

sub get_introspection_code
{
  my ($self, $name_prefix, $params_type) = @_;
  $name_prefix .= "." if($name_prefix ne "");
  my $entries = 0;
  my $children = "";
  foreach(@{$self->{decl_list}})
  {
    $entries++;
    $_->get_introspection_code($name_prefix, $params_type);
    $children .= "\n    &introspection_linear[".$_->{linearisation_pos}."],";
  }

  # add an entry to @arrays and @assignments
  push(@arrays, "static dt_introspection_field_t *f".$linearisation_pos."[] = {".$children."\n    NULL\n  };");
  push(@assignments, "introspection_linear[$linearisation_pos].".$self->get_type().".fields = f$linearisation_pos;");

  return "/*entries*/ $entries, /*fields*/ NULL";
}

#################### STRUCT TYPE ####################

package ast_type_struct_node;

@ISA = 'ast_type_struct_or_union_node';

sub new
{
  my ($self, $token, $name, $decl_list) = @_;

  my $reference = $self->SUPER::new($token, $name, $decl_list);
  bless($reference, $self);

  $reference->{type} = 'struct';

  return $reference;
}

sub get_type
{
  return "Struct";
}

#################### UNION TYPE ####################

package ast_type_union_node;

@ISA = 'ast_type_struct_or_union_node';

sub new
{
  my ($self, $token, $name, $decl_list) = @_;

  my $reference = $self->SUPER::new($token, $name, $decl_list);
  bless($reference, $self);

  $reference->{type} = 'union';

  return $reference;
}

sub get_type
{
  return "Union";
}

#################### ENUM TYPE ####################

package ast_type_enum_node;

@ISA = 'ast_type_node';

sub new
{
  my ($self, $token, $name, $enumerator_list) = @_;

  my $reference = $self->SUPER::new($token);
  bless($reference, $self);

  $reference->{name} = $name;
  $reference->{enumerator_list} = $enumerator_list;

  return $reference;
}

sub get_type
{
  return "Enum";
}

sub get_type_name
{
  my $self = shift;
  return $self->{name};
}

sub get_default
{
  my $self = shift;

  my $default = $self->SUPER::get_default();
  $default = "0" unless(defined $default);

  return $default;
}

sub print_tree
{
  my ($self, $prefix, $indent) = @_;
  my $spaces = " "x$indent;
  my $extra = $self->get_static_const();

  ast::print_out($prefix.$spaces.$extra."enum ".$self->{name}."\n");
  $spaces .= " "x$INDENT;
  foreach(@{$self->{enumerator_list}})
  {
    my ($id, $token) = @$_;
    my %comment_line = %{$scanner::comments{${@$token[0]}[$parser::P_FILENAME]}[${@$token[0]}[$parser::P_LINENO]]};
    my $description = "";
    $description = " : ".$comment_line{description} if(defined($comment_line{description}));
    ast::print_out($prefix.$spaces.$id.$description."\n");
  }
}

sub get_introspection_code
{
  my ($self, $name_prefix, $params_type, $declaration) = @_;
  my @enumerator_list = @{$self->{enumerator_list}};
  my $size = @enumerator_list;

  # add entry to @arrays and @assignments
  # we only do that once per type. if the same enum is used more than once then we can reuse that array.
  my $_linearisation_pos;
  if(not defined $enum_arrays{$self->{name}})
  {
    my $arrays_line = "static dt_introspection_type_enum_tuple_t f".$linearisation_pos."[] = { ";
    foreach(@enumerator_list)
    {
      my ($id, $token) = @$_;
      my %comment_line = %{$scanner::comments{${@$token[0]}[$parser::P_FILENAME]}[${@$token[0]}[$parser::P_LINENO]]};
      my $description = "";
      $description = $comment_line{description} if(defined($comment_line{description}));
      $description = ast::mark_for_translation($description);
      $arrays_line .= "\n    { \"$id\", $id, $description },";
    }
    $arrays_line .= "\n    { NULL, 0 },\n  };";
    push(@arrays, $arrays_line);
    $enum_arrays{$self->{name}} = $linearisation_pos;
    $_linearisation_pos = $linearisation_pos;
  }
  else
  {
    $_linearisation_pos = $enum_arrays{$self->{name}};
  }
  push(@assignments, "introspection_linear[$linearisation_pos].Enum.values = f$_linearisation_pos;");

  my $default = $declaration->get_default();
  $default = $self->get_default() unless(defined($default));

  return "/*entries*/ $size, /*values*/ NULL, /*Default*/ $default";
}

#################### DECLARATION ####################

package ast_declaration_node;

@ISA = 'ast_node';

sub new
{
  my ($self, $token, $type, $declaration) = @_;

  my $reference = $self->SUPER::new($token);
  bless($reference, $self);

  $reference->{type} = $type;
  $reference->{declaration} = $declaration;

  return $reference;
}

sub fix_types
{
  my ($self, $types_ref) = @_;
  if((ref $self->{type}) eq "ast_type_typedef_node")
  {
    my $type = $types_ref->{$self->{type}->{name}};
    if(defined($type))
    {
      $self->{type} = $$type->{type};
    }
  }
}

sub check_tree
{
  my $self = shift;
  return $self->{type}->check_tree();
}

sub print_tree
{
  my ($self, $prefix, $indent) = @_;
  my $spaces = " "x($indent + $INDENT);

  my $description = $self->{declaration}->get_description();
  $description = $self->{type}->get_description() if($description eq "");

  my $default = $self->{declaration}->get_default();
  $default = $self->{type}->get_default() unless(defined($default));

  my ($min, $max) = $self->{declaration}->get_range();
  my ($min_type, $max_type) = $self->{type}->get_range();
  $min = $min_type unless(defined($min));
  $max = $max_type unless(defined($max));
  my $range = "$min .. $max" if(defined($min) and defined($max));

  $self->{type}->print_tree($prefix, $indent);
  $self->{declaration}->print_tree($prefix, $indent);
  ast::print_out($prefix.$spaces."description: ".$description."\n") if($description ne "");
  ast::print_out($prefix.$spaces."default: ".$default."\n") if(defined($default));
  ast::print_out($prefix.$spaces."range: ".$range."\n") if(defined($range));
  ast::print_out("$prefix\n");
}

sub get_introspection_code
{
  my ($self, $name_prefix, $params_type) = @_;
  my @dimension_list = @{$self->{declaration}->{dimension_list}};
  my $dimensions = @dimension_list;
  my $varname = $name_prefix.$self->{declaration}->{id};
  my $inner_varname = $varname.("[0]"x$dimensions);
  my $field_name = $self->{declaration}->{id}.("[0]"x$dimensions);

  my $union_type = $self->{type}->get_type();
  my $type = "DT_INTROSPECTION_TYPE_".uc($union_type);

  my $type_name = $self->{type}->get_type_name();

  my $description = $self->get_description();
  $description = $self->{type}->get_description() if($description eq "");
  $description = ast::mark_for_translation($description);

  my $header = "$type, (char*)\"$type_name\", (char*)\"$inner_varname\", (char*)\"$field_name\", $description, sizeof((($params_type*)NULL)->$inner_varname), G_STRUCT_OFFSET($params_type, $varname), NULL";
  my $specific = $self->{type}->get_introspection_code($inner_varname, $params_type, $self->{declaration});
  my $linear_line = ".$union_type = {\n    { $header },\n    $specific\n  }";
  $self->add_to_linear($inner_varname, $linear_line);

  # is this an array?
  if($dimensions)
  {
    my $subtype = $type;
    my $depth = $dimensions;
    foreach(@dimension_list) # TODO: use a normal for loop ...
    {
      $depth--;
      $inner_varname = $varname.("[0]"x$depth);
      my $array_type_name = $type_name.("[]"x($dimensions - $depth));
      $field_name = $self->{declaration}->{id}.("[0]"x$depth);
      $header = "DT_INTROSPECTION_TYPE_ARRAY, (char*)\"$array_type_name\", (char*)\"$inner_varname\", (char*)\"$field_name\", $description, sizeof((($params_type*)NULL)->$inner_varname), G_STRUCT_OFFSET($params_type, $varname), NULL";
      $specific = "/*count*/ G_N_ELEMENTS((($params_type*)NULL)->$inner_varname), /*type*/ $subtype, /*field*/ &introspection_linear[".($linearisation_pos-1)."]";
      $linear_line = ".Array = {\n    { $header },\n    $specific\n  }";
      $self->add_to_linear($inner_varname, $linear_line);
      $subtype = "DT_INTROSPECTION_TYPE_ARRAY";
    }
  }
}

#################### DECLARATOR ####################

package ast_declarator_node;

@ISA = 'ast_node';

sub new
{
  my ($self, $token, $id, $dimension_list) = @_;

  my $reference = $self->SUPER::new($token);
  bless($reference, $self);

  $reference->{id} = $id;
  $reference->{dimension_list} = $dimension_list;

  return $reference;
}

sub print_tree
{
  my ($self, $prefix, $indent) = @_;
  my $spaces = " "x$indent;
  my $dimension_list = "";
  foreach(@{$self->{dimension_list}})
  {
    $dimension_list .= "[".$_."]";
  }
  ast::print_out($prefix.$spaces.$self->{id}." ".$dimension_list."\n");
}

1;

# modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
# vim: shiftwidth=2 expandtab tabstop=2 cindent
# kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
