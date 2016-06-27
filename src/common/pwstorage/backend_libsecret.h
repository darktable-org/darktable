// This file is part of darktable
//
// Copyright (c) 2014 Moritz Lipp <mlq@pwmt.org>.
// Copyright (c) 2016 tobias ellinghaus <me@houz.org>.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#ifndef __BACKEND_LIBSECRET_H__
#define __BACKEND_LIBSECRET_H__

#include <glib.h>

typedef struct backend_libsecret_context_t
{
  int placeholder; // we have to allocate one of these to signal that init didn't fail
} backend_libsecret_context_t;

/**
 * Initializes a new libsecret backend context.
 *
 * @return The libsecret context
 */
const backend_libsecret_context_t *dt_pwstorage_libsecret_new();

/**
 * Destroys the libsecret backend context.
 *
 * @param context The libsecret context
 */
void dt_pwstorage_libsecret_destroy(const backend_libsecret_context_t *context);

/**
 * Store (key,value) pairs.
 *
 * @param context The libsecret context
 * @param slot The name of the slot
 * @param attributes List of (key,value) pairs
 *
 * @return TRUE If function succeeded, otherwise FALSE
 */
gboolean dt_pwstorage_libsecret_set(const backend_libsecret_context_t *context, const gchar *slot,
                                    GHashTable *attributes);

/**
 * Loads (key, value) pairs
 *
 * @param context The libsecret context
 * @param slot The name of the slot
 *
 * @return table List of (key,value) pairs
 */
GHashTable *dt_pwstorage_libsecret_get(const backend_libsecret_context_t *context, const gchar *slot);

#endif // __BACKEND_LIBSECRET_H__

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
