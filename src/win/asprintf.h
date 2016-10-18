
/**
 * `asprintf.h' - asprintf.c
 *
 * copyright (c) 2014 joseph werle <joseph.werle@gmail.com>
 */

#ifndef HAVE_ASPRINTF
#ifndef ASPRINTF_H
#define ASPRINTF_H 1

#include <stdarg.h>

/**
 * Sets `char **' pointer to be a buffer
 * large enough to hold the formatted string
 * accepting a `va_list' args of variadic
 * arguments.
 */

int
vasprintf (char **, const char *, va_list);

/**
 * Sets `char **' pointer to be a buffer
 * large enough to hold the formatted
 * string accepting `n' arguments of
 * variadic arguments.
 */

int
asprintf (char **, const char *, ...);

#endif
#endif
