/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid.nativecore

private val editorDecimal = Regex("[+-]?(?:[0-9]+(?:[.,][0-9]*)?|[.,][0-9]+)(?:[eE][+-]?[0-9]+)?")

/** Plain decimal input: one comma or dot is a decimal mark, never a grouping separator. */
private fun String.editorDecimalOrNull(): String? = trim().takeIf(editorDecimal::matches)?.replace(',', '.')

internal fun String.toEditorDoubleOrNull(): Double? =
    editorDecimalOrNull()?.toDoubleOrNull()?.takeIf { it.isFinite() }

// Parse Float directly, as before, to retain its native decimal-to-Float rounding.
internal fun String.toEditorFloatOrNull(): Float? =
    editorDecimalOrNull()?.toFloatOrNull()?.takeIf { it.isFinite() }
