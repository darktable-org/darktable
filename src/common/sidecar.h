/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <glib.h>

/* Find readable, nonempty regular sidecars without interpreting any part of
 * filename as a glob pattern. The unnumbered sidecar is first, followed by
 * numbered sidecars in lexical order. The caller owns the paths and list.
 * This implementation also works on Android API 26, which has no glob(). */
GList *dt_sidecar_find_duplicates(const char *filename);
