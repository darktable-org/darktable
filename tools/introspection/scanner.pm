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

package scanner;

use strict;
use warnings;

use Exporter;
our @ISA = 'Exporter';
our @EXPORT = qw( @token @comments
                  $P_LINENO $P_FILENAME $P_TYPE $P_VALUE
                  $T_NONE $T_IDENT $T_KEYWORD $T_INTEGER_LITERAL $T_OPERATOR
                  $K_UNSIGNED $K_SIGNED $K_GBOOLEAN $K_CHAR $K_UCHAR $K_SHORT $K_USHORT $K_INT $K_UINT $K_LONG $K_ULONG $K_FLOAT $K_DOUBLE $K_COMPLEX $K_TYPEDEF $K_STRUCT $K_UNION $K_CONST $K_VOLATILE $K_STATIC $K_ENUM $K_VOID $K_DT_MODULE_INTROSPECTION
                  $O_ASTERISK $O_AMPERSAND $O_SEMICOLON $O_COMMA $O_COLON $O_SLASH $O_LEFTROUND $O_RIGHTROUND $O_LEFTCURLY $O_RIGHTCURLY $O_LEFTSQUARE $O_RIGHTSQUARE $O_EQUAL
                  read_file get_token look_ahead token2string
                  isid isinteger issemicolon istypedef isstruct isunion isenum isleftcurly isrightcurly isleftround isrightround isleftsquare isrightsquare 
                  iscomma isasterisk isequal isconst isvolatile isdtmoduleintrospection
                );


################# the scanner #################

my %history; # we don't like cyclic includes

my $lineno = 1;
my $file;
our $folder = "";
my @tokens;
our @token;
our @comments;

my @code;

# parser layout
our $P_LINENO = 0;
our $P_FILENAME = 1;
our $P_TYPE = 2;
our $P_VALUE = 3;

my $i = 0;
# token types
our $T_NONE = $i++;
our $T_IDENT = $i++;
our $T_KEYWORD = $i++;
our $T_INTEGER_LITERAL = $i++;
our $T_OPERATOR = $i++;

$i = 0;
# keywords
my  @K_readable;
our $K_UNSIGNED = $i++; push(@K_readable, 'unsigned');
our $K_SIGNED = $i++; push(@K_readable, 'signed');
our $K_GBOOLEAN = $i++; push(@K_readable, 'gboolean');
our $K_CHAR = $i++; push(@K_readable, 'char');
our $K_UCHAR = $i++; push(@K_readable, 'uchar');
our $K_SHORT = $i++; push(@K_readable, 'short');
our $K_USHORT = $i++; push(@K_readable, 'ushort');
our $K_INT = $i++; push(@K_readable, 'int');
our $K_UINT = $i++; push(@K_readable, 'uint');
our $K_LONG = $i++; push(@K_readable, 'long');
our $K_ULONG = $i++; push(@K_readable, 'ulong');
our $K_FLOAT = $i++; push(@K_readable, 'float');
our $K_DOUBLE = $i++; push(@K_readable, 'double');
our $K_COMPLEX = $i++; push(@K_readable, 'complex');
our $K_TYPEDEF = $i++; push(@K_readable, 'typedef');
our $K_STRUCT = $i++; push(@K_readable, 'struct');
our $K_UNION = $i++; push(@K_readable, 'union');
our $K_CONST = $i++; push(@K_readable, 'const');
our $K_VOLATILE = $i++; push(@K_readable, 'volatile');
our $K_STATIC = $i++; push(@K_readable, 'static');
our $K_ENUM = $i++; push(@K_readable, 'enum');
our $K_VOID = $i++; push(@K_readable, 'void');
our $K_DT_MODULE_INTROSPECTION = $i++; push(@K_readable, 'DT_MODULE_INTROSPECTION');
my  @keywords = (
      ['unsigned', $K_UNSIGNED],
      ['signed', $K_SIGNED],
      ['gboolean', $K_GBOOLEAN],
      ['char', $K_CHAR],
      ['gchar', $K_CHAR],
      ['int8_t', $K_CHAR],
      ['short', $K_SHORT],
      ['int16_t', $K_SHORT],
      ['uint16_t', $K_USHORT],
      ['int', $K_INT],
      ['gint', $K_INT],
      ['uint', $K_UINT],
      ['uint32_t', $K_UINT],
      ['int32_t', $K_INT],
      ['long', $K_LONG],
      ['float', $K_FLOAT],
      ['double', $K_DOUBLE],
      ['complex', $K_COMPLEX],
      ['typedef', $K_TYPEDEF],
      ['struct', $K_STRUCT],
      ['union', $K_UNION],
      ['const', $K_CONST],
      ['volatile', $K_VOLATILE],
      ['static', $K_STATIC],
      ['enum', $K_ENUM],
      ['void', $K_VOID],
      ['DT_MODULE_INTROSPECTION', $K_DT_MODULE_INTROSPECTION]
);

$i = 0;
# operators
my  @O_readable;
our $O_ASTERISK = $i++; push(@O_readable, '*');
our $O_AMPERSAND = $i++; push(@O_readable, '&');
our $O_SEMICOLON = $i++; push(@O_readable, ';');
our $O_COMMA = $i++; push(@O_readable, ',');
our $O_COLON = $i++; push(@O_readable, ':');
our $O_SLASH = $i++; push(@O_readable, '/');
our $O_LEFTROUND = $i++; push(@O_readable, '(');
our $O_RIGHTROUND = $i++; push(@O_readable, ')');
our $O_LEFTCURLY = $i++; push(@O_readable, '{');
our $O_RIGHTCURLY = $i++; push(@O_readable, '}');
our $O_LEFTSQUARE = $i++; push(@O_readable, '[');
our $O_RIGHTSQUARE = $i++; push(@O_readable, ']');
our $O_EQUAL = $i++; push(@O_readable, '=');
our $O_PLUS = $i++; push(@O_readable, '+');
our $O_MINUS = $i++; push(@O_readable, '-');
our $O_LESS = $i++; push(@O_readable, '<');
our $O_LESSLESS = $i++; push(@O_readable, '<<');
our $O_GREATER = $i++; push(@O_readable, '>');
our $O_GREATERGREATER = $i++; push(@O_readable, '>>');
our $O_PERCENT = $i++; push(@O_readable, '%');
our $O_CIRCUMFLEX = $i++; push(@O_readable, '^');

sub read_file
{
  $file = shift;

  return if(defined($history{$file}));
  $history{$file} = 1;

  open(IN, "<$file") or return;
  $lineno = 1;
  my @tmp = <IN>;
  close(IN);
  my $result = join('', @tmp);
  unshift(@code, split(//, $result));
}

# TODO: support something else than decimal numbers, i.e., octal and hex
sub read_number
{
  my $c = shift(@code);
  my @buf;
  while($c =~ /[0-9]/)
  {
    push(@buf, $c);
    $c = shift(@code);
  }
  unshift(@code, $c);
  return join('', @buf);
}

sub read_string
{
  my $c = shift(@code);
  my @buf;
  while(defined($c) && $c =~ /[a-zA-Z_0-9]/)
  {
    push(@buf, $c);
    $c = shift(@code);
  }
  unshift(@code, $c);
  return join('', @buf);
}

sub handle_comment
{
  my $_lineno = $lineno;
  shift(@code);
  my $c = $code[0];
  my @buf;
  if($c eq '/')
  {
    # a comment of the form '//'. this goes till the end of the line
    while(defined($c) && $c ne "\n")
    {
      push(@buf, $c);
      $c = shift(@code);
    }
    unshift(@code, $c);
    $lineno++;
  }
  elsif($c eq '*')
  {
    # a comment of the form '/*'. this goes till we find '*/'
    while(defined($c) && ($c ne '*' || $code[0] ne '/'))
    {
      $lineno++ if($c eq "\n");
      push(@buf, $c);
      $c = shift(@code);
    }
    push(@buf, $c);
  }
  else
  {
    # can't happen
    print STDERR "comment error\n";
  }
  my $comment = join('', @buf);

  push(@{$comments[$_lineno]{raw}}, $comment);
}

sub handle_include
{
  my $c = ' ';
  $c = shift(@code) while($c eq ' ');
  my $end;
  if($c eq '"') { $end = '"'; }
#   elsif($c eq '<') { $end = '>'; }
  else
  {
    unshift(@code, $c);
    return;
  }
  $c = shift(@code);
  my @buf;
  while(defined($c) && $c ne $end)
  {
    if($c eq "\n") # no idea how to handle this, just ignore it
    {
      unshift(@code, $c);
      ++$lineno;
      return;
    }
    push(@buf, $c);
    $c = shift(@code);
  }
  unshift(@code, $c);
  return if(!defined($c));

  my $filename = join('', @buf);

  if($filename =~ /^iop|^common/)
  {
    # add the current filename and lineno to the code stream so we
    # can reset these when the included file is scanned
    # note that all entries in @code coming from the files are single characters,
    # so we can safely add longer strings
    unshift(@code, 'undo_include', $file, $lineno);
    read_file($folder.$filename);
  }
}

sub handle_define
{
  # just read until the end of the line
  my $c = ' ';
  $c = shift(@code) while(defined($code[0]) && $c ne "\n");
  unshift(@code, $c);
}

sub handle_preprocessor
{
  my $string = read_string();
  if($string eq "include") { handle_include(); }
  elsif($string eq "define") { handle_define(); }
  unshift(@code, ' ');
}

sub read_token
{
  for(; defined($code[0]); shift(@code))
  {
    my $c = $code[0];
    if($c eq "\n") { ++$lineno;}
    elsif($c eq " " || $c eq "\t") { next; }
    elsif($c eq "#") { shift(@code); handle_preprocessor(); next; }
    elsif($c eq "undo_include") { shift(@code); $file = shift(@code); $lineno = shift(@code); }
    elsif($c eq "&") { shift(@code); return ($lineno, $file, $T_OPERATOR, $O_AMPERSAND); }
    elsif($c eq "*") { shift(@code); return ($lineno, $file, $T_OPERATOR, $O_ASTERISK); }
    elsif($c eq "/" && ($code[1] eq "/" || $code[1] eq "*" ))
    {
      handle_comment();
      next;
    }
    elsif($c eq ";") { shift(@code); return ($lineno, $file, $T_OPERATOR, $O_SEMICOLON); }
    elsif($c eq ",") { shift(@code); return ($lineno, $file, $T_OPERATOR, $O_COMMA); }
    elsif($c eq "(") { shift(@code); return ($lineno, $file, $T_OPERATOR, $O_LEFTROUND); }
    elsif($c eq ")") { shift(@code); return ($lineno, $file, $T_OPERATOR, $O_RIGHTROUND); }
    elsif($c eq "{") { shift(@code); return ($lineno, $file, $T_OPERATOR, $O_LEFTCURLY); }
    elsif($c eq "}") { shift(@code); return ($lineno, $file, $T_OPERATOR, $O_RIGHTCURLY); }
    elsif($c eq "[") { shift(@code); return ($lineno, $file, $T_OPERATOR, $O_LEFTSQUARE); }
    elsif($c eq "]") { shift(@code); return ($lineno, $file, $T_OPERATOR, $O_RIGHTSQUARE); }
    elsif($c eq ":") { shift(@code); return ($lineno, $file, $T_OPERATOR, $O_COLON); }
    elsif($c eq "=") { shift(@code); return ($lineno, $file, $T_OPERATOR, $O_EQUAL); }
    elsif($c eq "+") { shift(@code); return ($lineno, $file, $T_OPERATOR, $O_PLUS); }
    elsif($c eq "-") { shift(@code); return ($lineno, $file, $T_OPERATOR, $O_MINUS); }
    elsif($c eq "<")
    {
      shift(@code);
      if($code[0] eq "<")
      {
        shift(@code);
        return ($lineno, $file, $T_OPERATOR, $O_LESSLESS);
      }
      else
      {
        return ($lineno, $file, $T_OPERATOR, $O_LESS);
      }
    }
    elsif($c eq ">")
    {
      shift(@code);
      if($code[0] eq ">")
      {
        shift(@code);
        return ($lineno, $file, $T_OPERATOR, $O_GREATERGREATER);
      }
      else
      {
        return ($lineno, $file, $T_OPERATOR, $O_GREATER);
      }
    }
    elsif($c eq "%") { shift(@code); return ($lineno, $file, $T_OPERATOR, $O_PERCENT); }
    elsif($c eq "^") { shift(@code); return ($lineno, $file, $T_OPERATOR, $O_CIRCUMFLEX); }
    elsif($c =~ /^[0-9]$/)
    {
      my $number = read_number();
      return ($lineno, $file, $T_INTEGER_LITERAL, $number);
    }
    elsif($c =~ /^[a-zA-Z_]$/)
    {
      my $string = read_string();
      foreach(@keywords)
      {
        my @entry = @{$_};
        if($string eq $entry[0])
        {
          return ($lineno, $file, $T_KEYWORD, $entry[1]);
        }
      }
      return ($lineno, $file, $T_IDENT, "$string");
    }
    else {
      # we don't care that we can't understand every input symbol, we just read over them until we reach something we know.
      # everything we see from there on should be handled by the scanner/parser
      # print STDERR "scanner error: ".$c."\n";
    }
  }
  return ($lineno, $file, $T_NONE, 0);
}

sub get_token
{
  my $n_tokens = @tokens;
  return read_token() if($n_tokens == 0);
  return @{shift(@tokens)};
}

sub look_ahead
{
  my $steps = shift;
  my $n_tokens = @tokens;

  return $tokens[$steps-1] if($n_tokens >= $steps);

  my @token;
  for(my $i = $n_tokens; $i < $steps; ++$i )
  {
    @token = read_token();
    return @token if($token[$P_TYPE] == $T_NONE);              # Can't look ahead that far.
    push(@tokens, [@token]);
  }
  return @token;
}

sub token2string
{
  my $token = shift;
  my $result;

  if   ($token[$P_TYPE] == $T_NONE)            { $result = '<EMPTY TOKEN>'; }
  elsif($token[$P_TYPE] == $T_IDENT)           { $result = $token[$P_VALUE]; }
  elsif($token[$P_TYPE] == $T_KEYWORD)         { $result = $K_readable[$token[$P_VALUE]]; }
  elsif($token[$P_TYPE] == $T_INTEGER_LITERAL) { $result = $token[$P_VALUE]; }
  elsif($token[$P_TYPE] == $T_OPERATOR)        { $result = $O_readable[$token[$P_VALUE]]; }
  else                                         { $result = '<UNKNOWN TOKEN TYPE>'; }

  return $result;
}

sub issemicolon { my $token = shift; return ($token[$P_TYPE] == $T_OPERATOR && $token[$P_VALUE] == $O_SEMICOLON); }
sub isleftcurly { my $token = shift; return ($token[$P_TYPE] == $T_OPERATOR && $token[$P_VALUE] == $O_LEFTCURLY); }
sub isrightcurly { my $token = shift; return ($token[$P_TYPE] == $T_OPERATOR && $token[$P_VALUE] == $O_RIGHTCURLY); }
sub isleftround { my $token = shift; return ($token[$P_TYPE] == $T_OPERATOR && $token[$P_VALUE] == $O_LEFTROUND); }
sub isrightround { my $token = shift; return ($token[$P_TYPE] == $T_OPERATOR && $token[$P_VALUE] == $O_RIGHTROUND); }
sub isleftsquare { my $token = shift; return ($token[$P_TYPE] == $T_OPERATOR && $token[$P_VALUE] == $O_LEFTSQUARE); }
sub isrightsquare { my $token = shift; return ($token[$P_TYPE] == $T_OPERATOR && $token[$P_VALUE] == $O_RIGHTSQUARE); }
sub iscomma { my $token = shift; return ($token[$P_TYPE] == $T_OPERATOR && $token[$P_VALUE] == $O_COMMA); }
sub isasterisk { my $token = shift; return ($token[$P_TYPE] == $T_OPERATOR && $token[$P_VALUE] == $O_ASTERISK); }
sub isequal { my $token = shift; return ($token[$P_TYPE] == $T_OPERATOR && $token[$P_VALUE] == $O_EQUAL); }
sub isid { my $token = shift; return ($token[$P_TYPE] == $T_IDENT); }
sub isinteger { my $token = shift; return ($token[$P_TYPE] == $T_INTEGER_LITERAL); }
sub istypedef { my $token = shift; return ($token[$P_TYPE] == $T_KEYWORD && $token[$P_VALUE] == $K_TYPEDEF); }
sub isstruct { my $token = shift; return ($token[$P_TYPE] == $T_KEYWORD && $token[$P_VALUE] == $K_STRUCT); }
sub isunion { my $token = shift; return ($token[$P_TYPE] == $T_KEYWORD && $token[$P_VALUE] == $K_UNION); }
sub isenum { my $token = shift; return ($token[$P_TYPE] == $T_KEYWORD && $token[$P_VALUE] == $K_ENUM); }
sub isconst { my $token = shift; return ($token[$P_TYPE] == $T_KEYWORD && $token[$P_VALUE] == $K_CONST); }
sub isvolatile { my $token = shift; return ($token[$P_TYPE] == $T_KEYWORD && $token[$P_VALUE] == $K_VOLATILE); }
sub isdtmoduleintrospection { my $token = shift; return ($token[$P_TYPE] == $T_KEYWORD && $token[$P_VALUE] == $K_DT_MODULE_INTROSPECTION); }

1;

# modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
# vim: shiftwidth=2 expandtab tabstop=2 cindent
# kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
