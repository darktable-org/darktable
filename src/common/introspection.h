/*
    This file is part of darktable,
    Copyright (C) 2014-2020 darktable developers.

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

#pragma once

#include <inttypes.h>
#include <glib.h>
#include <stdlib.h>

// some typedefs for structs that hold the data in a machine readable form

#define DT_INTROSPECTION_VERSION 8

// clang-format off

struct dt_iop_module_so_t;
union dt_introspection_field_t;

typedef enum dt_introspection_type_t
{
  DT_INTROSPECTION_TYPE_NONE = 0,
  DT_INTROSPECTION_TYPE_OPAQUE,
  DT_INTROSPECTION_TYPE_FLOAT,
  DT_INTROSPECTION_TYPE_DOUBLE,
  DT_INTROSPECTION_TYPE_FLOATCOMPLEX,
  DT_INTROSPECTION_TYPE_CHAR,
  DT_INTROSPECTION_TYPE_INT8,
  DT_INTROSPECTION_TYPE_UINT8,
  DT_INTROSPECTION_TYPE_SHORT,
  DT_INTROSPECTION_TYPE_USHORT,
  DT_INTROSPECTION_TYPE_INT,
  DT_INTROSPECTION_TYPE_UINT,
  DT_INTROSPECTION_TYPE_LONG,
  DT_INTROSPECTION_TYPE_ULONG,
  DT_INTROSPECTION_TYPE_BOOL,
  DT_INTROSPECTION_TYPE_ARRAY,
  DT_INTROSPECTION_TYPE_ENUM,
  DT_INTROSPECTION_TYPE_STRUCT,
  DT_INTROSPECTION_TYPE_UNION
} dt_introspection_type_t;

typedef struct dt_introspection_type_header_t
{
  dt_introspection_type_t             type;            // type of the field
  const char                         *type_name;       // the type as specified in the source. mostly interesting for enum, struct and the like
  const char                         *name;            // variable name, possibly with the name of parent structs, separated with '.'
  const char                         *field_name;      // variable name without any parents
  const char                         *description;     // some human readable description taken from the comments
  size_t                              size;            // size of the field in bytes
  size_t                              offset;          // offset from the beginning of the start of params. TODO: use start of parent struct instead?
  struct dt_iop_module_so_t          *so;              // a pointer to the dlopen'ed module
} dt_introspection_type_header_t;

typedef struct dt_introspection_type_opaque_t
{
  dt_introspection_type_header_t      header;
} dt_introspection_type_opaque_t;

typedef struct dt_introspection_type_float_t
{
  dt_introspection_type_header_t      header;
  float                               Min;             // minimum allowed value for this float field. taken from comments. defaults to -G_MAXFLOAT
  float                               Max;             // maximum allowed value for this float field. taken from comments. defaults to G_MAXFLOAT
  float                               Default;         // default value for this float field. taken from comments. defaults to 0.0
} dt_introspection_type_float_t;

typedef struct dt_introspection_type_double_t
{
  dt_introspection_type_header_t      header;
  double                              Min;             // minimum allowed value for this double field. taken from comments. defaults to -G_MAXDOUBLE
  double                              Max;             // maximum allowed value for this double field. taken from comments. defaults to G_MAXDOUBLE
  double                              Default;         // default value for this double field. taken from comments. defaults to 0.0
} dt_introspection_type_double_t;

typedef struct dt_introspection_type_float_complex_t
{
  dt_introspection_type_header_t      header;
  float _Complex                      Min;             // minimum allowed value for this float complex field. taken from comments. defaults to -G_MAXFLOAT + -G_MAXFLOAT * _Complex_I
  float _Complex                      Max;             // maximum allowed value for this float complex field. taken from comments. defaults to G_MAXFLOAT + G_MAXFLOAT * _Complex_I
  float _Complex                      Default;         // default value for this float complex field. taken from comments. defaults to 0.0 + 0.0 * _Complex_I
} dt_introspection_type_float_complex_t;

typedef struct dt_introspection_type_char_t
{
  dt_introspection_type_header_t      header;
  char                                Min;             // minimum allowed value for this char field. taken from comments. defaults to CHAR_MIN
  char                                Max;             // maximum allowed value for this char field. taken from comments. defaults to CHAR_MAX
  char                                Default;         // default value for this char field. taken from comments. defaults to 0
} dt_introspection_type_char_t;

typedef struct dt_introspection_type_int8_t
{
  dt_introspection_type_header_t      header;
  int8_t                              Min;             // minimum allowed value for this int8_t field. taken from comments. defaults to G_MININT8
  int8_t                              Max;             // maximum allowed value for this int8_t field. taken from comments. defaults to G_MAXINT8
  char                                Default;         // default value for this int8_t field. taken from comments. defaults to 0
} dt_introspection_type_int8_t;

typedef struct dt_introspection_type_uint8_t
{
  dt_introspection_type_header_t      header;
  uint8_t                             Min;             // minimum allowed value for this uint8_t field. taken from comments. defaults to 0
  uint8_t                             Max;             // maximum allowed value for this uint8_t field. taken from comments. defaults to G_MAXUINT8
  uint8_t                             Default;         // default value for this uint8_t field. taken from comments. defaults to 0
} dt_introspection_type_uint8_t;

typedef struct dt_introspection_type_short_t
{
  dt_introspection_type_header_t      header;
  short                               Min;             // minimum allowed value for this short field. taken from comments. defaults to G_MINSHORT
  short                               Max;             // maximum allowed value for this short field. taken from comments. defaults to G_MAXSHORT
  short                               Default;         // default value for this short field. taken from comments. defaults to 0
} dt_introspection_type_short_t;

typedef struct dt_introspection_type_ushort_t
{
  dt_introspection_type_header_t      header;
  unsigned short                      Min;             // minimum allowed value for this unsigned short field. taken from comments. defaults to 0
  unsigned short                      Max;             // maximum allowed value for this unsigned short field. taken from comments. defaults to G_MAXUSHORT
  unsigned short                      Default;         // default value for this unsigned short field. taken from comments. defaults to 0
} dt_introspection_type_ushort_t;

typedef struct dt_introspection_type_int_t
{
  dt_introspection_type_header_t      header;
  int                                 Min;             // minimum allowed value for this int field. taken from comments. defaults to G_MININT
  int                                 Max;             // maximum allowed value for this int field. taken from comments. defaults to G_MAXINT
  int                                 Default;         // default value for this int field. taken from comments. defaults to 0
} dt_introspection_type_int_t;

typedef struct dt_introspection_type_uint_t
{
  dt_introspection_type_header_t      header;
  unsigned int                        Min;             // minimum allowed value for this unsigned int field. taken from comments. defaults to 0
  unsigned int                        Max;             // maximum allowed value for this unsigned int field. taken from comments. defaults to G_MAXUINT
  unsigned int                        Default;         // default value for this unsigned int field. taken from comments. defaults to 0
} dt_introspection_type_uint_t;

typedef struct dt_introspection_type_long_t
{
  dt_introspection_type_header_t      header;
  long                                Min;             // minimum allowed value for this long field. taken from comments. defaults to G_MINLONG
  long                                Max;             // maximum allowed value for this long field. taken from comments. defaults to G_MAXLONG
  long                                Default;         // default value for this long field. taken from comments. defaults to 0
} dt_introspection_type_long_t;

typedef struct dt_introspection_type_ulong_t
{
  dt_introspection_type_header_t      header;
  unsigned long                       Min;             // minimum allowed value for this unsigned long field. taken from comments. defaults to 0
  unsigned long                       Max;             // maximum allowed value for this unsigned long field. taken from comments. defaults to G_MAXULONG
  unsigned long                       Default;         // default value for this unsigned long field. taken from comments. defaults to 0
} dt_introspection_type_ulong_t;

typedef struct dt_introspection_type_bool_t
{
  dt_introspection_type_header_t      header;
  gboolean                            Default;         // default value for this gboolean field. taken from comments. defaults to FALSE
} dt_introspection_type_bool_t;

typedef struct dt_introspection_type_array_t
{
  dt_introspection_type_header_t      header;
  size_t                              count;           // number of elements in the array
  dt_introspection_type_t             type;            // type of the elements
  union dt_introspection_field_t     *field;           // the relevant data of the elements, depending on type
} dt_introspection_type_array_t;

typedef struct dt_introspection_type_enum_tuple_t
{
  const char                         *name;            // the id of the enum value as a string
  int                                 value;           // the enum value
  const char                         *description;     // some human readable description taken from the comments
} dt_introspection_type_enum_tuple_t;

typedef struct dt_introspection_type_enum_t
{
  dt_introspection_type_header_t      header;
  size_t                              entries;         // # entries in values (without the closing {NULL, 0})
  dt_introspection_type_enum_tuple_t *values;          // the enum tuples, consisting of { "STRING", VALUE }. terminated with { NULL, 0 }
  int                                 Default;         // default value for this enum field. taken from comments. defaults to 0
} dt_introspection_type_enum_t;

typedef struct dt_introspection_type_struct_t
{
  dt_introspection_type_header_t      header;
  size_t                              entries;         // # entries in fields (without the closing NULL)
  union dt_introspection_field_t    **fields;          // the fields of the struct. NULL terminated
} dt_introspection_type_struct_t;

typedef struct dt_introspection_type_union_t
{
  dt_introspection_type_header_t      header;
  size_t                              entries;         // # entries in fields (without the closing NULL)
  union dt_introspection_field_t    **fields;          // the fields of the union. NULL terminated
} dt_introspection_type_union_t;

// sorry for the camel case/Capitals, but we have to avoid reserved keywords
typedef union dt_introspection_field_t
{
  dt_introspection_type_header_t        header;        // the common header
  dt_introspection_type_opaque_t        Opaque;        // some binary blob
  dt_introspection_type_float_t         Float;         // a float
  dt_introspection_type_double_t        Double;        // a double
  dt_introspection_type_float_complex_t FloatComplex;  // a float complex
  dt_introspection_type_char_t          Char;          // a char
  dt_introspection_type_int8_t          Int8;          // a signed char or int8_t
  dt_introspection_type_uint8_t         UInt8;         // an unsigned char or uint8_t
  dt_introspection_type_short_t         Short;         // a short
  dt_introspection_type_ushort_t        UShort;        // an unsigned short
  dt_introspection_type_int_t           Int;           // an int
  dt_introspection_type_uint_t          UInt;          // an unsigned int
  dt_introspection_type_long_t          Long;          // a long
  dt_introspection_type_ulong_t         ULong;         // an unsigned long
  dt_introspection_type_bool_t          Bool;          // a gboolean
  dt_introspection_type_array_t         Array;         // an array
  dt_introspection_type_enum_t          Enum;          // an enum
  dt_introspection_type_struct_t        Struct;        // a struct
  dt_introspection_type_union_t         Union;         // an union
} dt_introspection_field_t;

typedef struct dt_introspection_t
{
  int                                 api_version;     // introspection API version
  int                                 params_version;  // the version of the params layout. taken from DT_MODULE_INTROSPECTION()
  const char                         *type_name;       // the typedef'ed name for this type as passed to DT_MODULE_INTROSPECTION()
  size_t                              size;            // size of the params struct
  dt_introspection_field_t           *field;           // the type of the params. should always be a DT_INTROSPECTION_TYPE_STRUCT
  size_t                              self_size;       // size of dt_iop_module_t. useful to not need dt headers
  size_t                              default_params;  // offset of the default_params in dt_iop_module_t. useful to not need dt headers
  GHashTable                         *sections;        // section names associated with parameter offsets
} dt_introspection_t;

// clang-format on

/** helper function to access array elements -- make sure to cast the result correctly!
 *
 * @param self field description of the array
 * @param start pointer into the params blob to the start of the array
 * @param element the element of the array to return
 * @param child if non-%NULL, it returns the field description of the child element
 * @return the pointer into the params blob to the requested element, or %NULL if not found
 **/
static inline void *dt_introspection_access_array(dt_introspection_field_t *self, void *start,
                                                  unsigned int element, dt_introspection_field_t **child)
{
  if(!(start && self && self->header.type == DT_INTROSPECTION_TYPE_ARRAY && element < self->Array.count))
    return NULL;

  if(child) *child = self->Array.field;
  return (void *)((char *)start + element * self->Array.field->header.size);
}


/** helper function to access elements in a struct -- make sure to cast the result correctly!
 *
 * @param self field description of the struct
 * @param start pointer into the params blob to the start of the struct
 * @param name the name of the child to look for
 * @param child if non-%NULL, it returns the field description of the child element
 * @return the pointer into the params blob to the requested element, or %NULL if not found
 **/
static inline void *dt_introspection_get_child(dt_introspection_field_t *self, void *start, const char *name,
                                               dt_introspection_field_t **child)
{
  if(!(start && self && name && *name)) return NULL;

  dt_introspection_field_t **iter;

  if(self->header.type == DT_INTROSPECTION_TYPE_STRUCT)
    iter = self->Struct.fields;
  else if(self->header.type == DT_INTROSPECTION_TYPE_UNION)
    iter = self->Union.fields;
  else
    return NULL;

  while(*iter)
  {
    if(!g_strcmp0((*iter)->header.field_name, name))
    {
      size_t parent_offset = self->header.offset;
      size_t child_offset = (*iter)->header.offset;
      size_t relative_offset = child_offset - parent_offset;
      if(child) *child = *iter;
      return (void *)((char *)start + relative_offset);
    }
    iter++;
  }
  return NULL;
}

/** helper function to get the symbolic name of an enum value
 *
 * @param self field description of the enum
 * @param value the value that should be looked up
 * @return the pointer to the name string, or %NULL if not found
 **/
static inline const char *dt_introspection_get_enum_name(dt_introspection_field_t *self, int value)
{
  if(!(self && self->header.type == DT_INTROSPECTION_TYPE_ENUM)) return NULL;

  for(dt_introspection_type_enum_tuple_t *iter = self->Enum.values; iter->name; iter++)
    if(iter->value == value)
      return iter->name;

  return NULL;
}

/** helper function to get the enum value of a symbolic name
 *
 * @param self field description of the enum
 * @param name the name that should be looked up
 * @param value the value that was found
 * @return TRUE if the name was found, or FALSE otherwise
 **/
static inline gboolean dt_introspection_get_enum_value(dt_introspection_field_t *self, const char *name, int *value)
{
  if(!(self && self->header.type == DT_INTROSPECTION_TYPE_ENUM)) return FALSE;

  for(dt_introspection_type_enum_tuple_t *iter = self->Enum.values; iter->name; iter++)
    if(!g_strcmp0(iter->name, name))
    {
      *value = iter->value;
      return TRUE;
    }

  return FALSE;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
