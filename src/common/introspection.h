/*
    This file is part of darktable,
    copyright (c) 2013-2014 tobias ellinghaus.

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

#ifndef __INTROSPECTION_H__
#define __INTROSPECTION_H__

#include <stdlib.h>
#include <glib.h>

// some typedefs for structs that hold the data in a machine readable form

#define DT_INTROSPECTION_VERSION 1

union dt_introspection_field_t;

typedef enum dt_introspection_type_t
{
  DT_INTROSPECTION_TYPE_NONE = 0,
  DT_INTROSPECTION_TYPE_OPAQUE,
  DT_INTROSPECTION_TYPE_FLOAT,
  DT_INTROSPECTION_TYPE_DOUBLE,
  DT_INTROSPECTION_TYPE_CHAR,
  DT_INTROSPECTION_TYPE_UCHAR,
  DT_INTROSPECTION_TYPE_SHORT,
  DT_INTROSPECTION_TYPE_USHORT,
  DT_INTROSPECTION_TYPE_INT,
  DT_INTROSPECTION_TYPE_UINT,
  DT_INTROSPECTION_TYPE_LONG,
  DT_INTROSPECTION_TYPE_ULONG,
  DT_INTROSPECTION_TYPE_BOOL,
  DT_INTROSPECTION_TYPE_ARRAY,
  DT_INTROSPECTION_TYPE_ENUM,
  DT_INTROSPECTION_TYPE_STRUCT
} dt_introspection_type_t;

typedef struct dt_introspection_type_header_t
{
  dt_introspection_type_t             type;         // type of the field
  char                               *name;         // variable name, possibly with the name of parent structs, separated with '.'
  char                               *field_name;   // variable name without any parents
  char                               *description;  // some human readable description taken from the comments
  size_t                              size;         // size of the field in bytes
  size_t                              offset;       // offset from the beginning of the start of params. TODO: use start of parent struct instead?
} dt_introspection_type_header_t;

typedef struct dt_introspection_type_opaque_t
{
  dt_introspection_type_header_t      header;
} dt_introspection_type_opaque_t;

typedef struct dt_introspection_type_float_t
{
  dt_introspection_type_header_t      header;
  float                               Min;          // minimum allowed value for this float field. taken from comments. defaults to -G_MAXFLOAT
  float                               Max;          // minimum allowed value for this float field. taken from comments. defau to G_MAXFLOAT
  float                               Default;      // default value for this char field. taken from comments. defaults to 0
} dt_introspection_type_float_t;

typedef struct dt_introspection_type_double_t
{
  dt_introspection_type_header_t      header;
  double                              Min;          // minimum allowed value for this double field. taken from comments. defaults to -G_MAXDOUBLE
  double                              Max;          // minimum allowed value for this double field. taken from comments. defau to G_MAXDOUBLE
  double                              Default;      // default value for this double field. taken from comments. defaults to 0
} dt_introspection_type_double_t;

typedef struct dt_introspection_type_char_t
{
  dt_introspection_type_header_t      header;
  char                                Min;          // minimum allowed value for this char field. taken from comments. defaults to G_MININT8
  char                                Max;          // maximum allowed value for this char field. taken from comments. defaults to G_MAXINT8
  char                                Default;      // default value for this char field. taken from comments. defaults to 0
} dt_introspection_type_char_t;

typedef struct dt_introspection_type_uchar_t
{
  dt_introspection_type_header_t      header;
  unsigned char                       Min;          // minimum allowed value for this char field. taken from comments. defaults to 0
  unsigned char                       Max;          // maximum allowed value for this char field. taken from comments. defaults to G_MAXUINT8
  unsigned char                       Default;      // default value for this char field. taken from comments. defaults to 0
} dt_introspection_type_uchar_t;

typedef struct dt_introspection_type_short_t
{
  dt_introspection_type_header_t      header;
  short                               Min;          // minimum allowed value for this short field. taken from comments. defaults to G_MINSHORT
  short                               Max;          // maximum allowed value for this short field. taken from comments. defaults to G_MAXSHORT
  short                               Default;      // default value for this short field. taken from comments. defaults to 0
} dt_introspection_type_short_t;

typedef struct dt_introspection_type_ushort_t
{
  dt_introspection_type_header_t      header;
  unsigned short                      Min;          // minimum allowed value for this char field. taken from comments. defaults to 0
  unsigned short                      Max;          // maximum allowed value for this char field. taken from comments. defaults to G_MAXUSHORT
  unsigned short                      Default;      // default value for this char field. taken from comments. defaults to 0
} dt_introspection_type_ushort_t;

typedef struct dt_introspection_type_int_t
{
  dt_introspection_type_header_t      header;
  int                                 Min;          // minimum allowed value for this int field. taken from comments. defaults to G_MININT
  int                                 Max;          // maximum allowed value for this int field. taken from comments. defaults to G_MAXINT
  int                                 Default;      // default value for this int field. taken from comments. defaults to 0
} dt_introspection_type_int_t;

typedef struct dt_introspection_type_uint_t
{
  dt_introspection_type_header_t      header;
  unsigned int                        Min;          // minimum allowed value for this unsigned int field. taken from comments. defaults to 0
  unsigned int                        Max;          // maximum allowed value for this unsigned int field. taken from comments. defaults to G_MAXUINT
  unsigned int                        Default;      // default value for this unsigned int field. taken from comments. defaults to 0
} dt_introspection_type_uint_t;

typedef struct dt_introspection_type_long_t
{
  dt_introspection_type_header_t      header;
  long                                Min;          // minimum allowed value for this long field. taken from comments. defaults to G_MINLONG
  long                                Max;          // maximum allowed value for this long field. taken from comments. defaults to G_MAXLONG
  long                                Default;      // default value for this long field. taken from comments. defaults to 0
} dt_introspection_type_long_t;

typedef struct dt_introspection_type_ulong_t
{
  dt_introspection_type_header_t      header;
  unsigned long                       Min;          // minimum allowed value for this unsigned long field. taken from comments. defaults to 0
  unsigned long                       Max;          // maximum allowed value for this unsigned long field. taken from comments. defaults to G_MAXULONG
  unsigned long                       Default;      // default value for this unsigned long field. taken from comments. defaults to 0
} dt_introspection_type_ulong_t;

typedef struct dt_introspection_type_bool_t
{
  dt_introspection_type_header_t      header;
  gboolean                            Default;      // default value for this gboolean field. taken from comments. defaults to FALSE
} dt_introspection_type_bool_t;

typedef struct dt_introspection_type_array_t
{
  dt_introspection_type_header_t      header;
  size_t                              count;        // number of elements in the array
  dt_introspection_type_t             type;         // type of the elements
  union dt_introspection_field_t     *field;        // the relevant data of the elements, depending on type
} dt_introspection_type_array_t;

typedef struct dt_introspection_type_enum_tuple_t
{
  char                               *name;
  int                                 value;
} dt_introspection_type_enum_tuple_t;

typedef struct dt_introspection_type_enum_t
{
  dt_introspection_type_header_t      header;
  size_t                              entries;      // # entries in values (without the closing {NULL, 0})
  dt_introspection_type_enum_tuple_t *values;       // the enum tuples, consisting of { "STRING", VALUE }. terminated with { NULL, 0 }
} dt_introspection_type_enum_t;

typedef struct dt_introspection_type_struct_t
{
  dt_introspection_type_header_t      header;
  size_t                              entries;      // # entries in fields (without the closing NULL)
  union dt_introspection_field_t    **fields;       // the fields of the struct. NULL terminated
} dt_introspection_type_struct_t;

// sorry for the camel case/Capitals, but we have to avoid reserved keywords
typedef union dt_introspection_field_t
{
  dt_introspection_type_header_t      header;       // the common header
  dt_introspection_type_opaque_t      Opaque;       // some binary blob
  dt_introspection_type_float_t       Float;        // a float
  dt_introspection_type_double_t      Double;       // a double
  dt_introspection_type_char_t        Char;         // a char
  dt_introspection_type_uchar_t       UChar;        // an unsigned char
  dt_introspection_type_short_t       Short;        // a short
  dt_introspection_type_ushort_t      UShort;       // an unsigned short
  dt_introspection_type_int_t         Int;          // an int
  dt_introspection_type_uint_t        UInt;         // an unsigned int
  dt_introspection_type_long_t        Long;         // a long
  dt_introspection_type_ulong_t       ULong;        // an unsigned long
  dt_introspection_type_bool_t        Bool;         // a gboolean
  dt_introspection_type_array_t       Array;        // an array
  dt_introspection_type_enum_t        Enum;         // an enum
  dt_introspection_type_struct_t      Struct;       // a struct
} dt_introspection_field_t;

typedef struct dt_introspection_t
{
  int                                 api_version;    // introspection API version
  int                                 params_version; // the version of the params layout. taken from DT_MODULE()
  size_t                              size;           // size of the params struct
  dt_introspection_field_t           *field;          // the type of the params. should always be a DT_INTROSPECTION_TYPE_STRUCT
} dt_introspection_t;


#endif // __INTROSPECTION_H__

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
