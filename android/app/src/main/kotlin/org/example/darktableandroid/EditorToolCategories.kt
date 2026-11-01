/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid

import org.example.darktableandroid.nativecore.EditorModule
import org.example.darktableandroid.nativecore.hasEditableControls

/** Presentation groups only: this never changes native processing order or enabled state. */
internal enum class WorkspaceTab { ADJUSTMENTS, DETAIL, GEOMETRY, SETTINGS }

private val reviewedWorkspaceGroups = mapOf(
    WorkspaceTab.ADJUSTMENTS to setOf(
        "invert", "temperature", "rawprepare", "highlights", "colorin", "colorout",
        "exposure", "tonemap", "toneequal", "graduatednd", "profile_gamma", "channelmixerrgb",
        "negadoctor", "primaries", "colorchecker", "colortransfer", "colormapping", "channelmixer",
        "basicadj", "colorharmonizer", "colorbalance", "colorequal", "colorbalancergb", "rgbcurve",
        "rgblevels", "basecurve", "filmic", "sigmoid", "agx", "filmicrgb", "lut3d", "colisa",
        "tonecurve", "levels", "shadhi", "zonesystem", "globaltonemap", "relight", "colorcorrection",
        "colorcontrast", "velvia", "vibrance", "colorzones", "colorize", "lowlight", "monochrome",
        "splittoning", "colorreconstruct",
    ),
    WorkspaceTab.DETAIL to setOf(
        "rasterfile", "cacorrect", "hotpixels", "rawdenoise", "demosaic", "denoiseprofile", "bilateral",
        "cacorrectrgb", "hazeremoval", "spots", "retouch", "equalizer", "diffuse", "blurs", "nlmeans",
        "defringe", "atrous", "lowpass", "highpass", "sharpen", "bilat", "clahe",
    ),
    WorkspaceTab.GEOMETRY to setOf(
        "scalepixels", "rotatepixels", "lens", "ashift", "flip", "enlargecanvas", "overlay", "clipping",
        "liquify", "crop", "censorize", "bloom", "grain", "soften", "vignette", "dither", "borders", "watermark",
    ),
)

private val workspaceOperationTabs = reviewedWorkspaceGroups.entries.flatMap { (tab, operations) ->
    operations.map { it to tab }
}.let { entries ->
    check(entries.map { it.first }.distinct().size == entries.size) { "An editing tool belongs to more than one workspace tab" }
    entries.toMap()
}

/** Instances keep their full native ID; only their family determines the visual group. */
internal fun workspaceTabForOperation(operation: String): WorkspaceTab =
    workspaceOperationTabs[operation.substringBefore('@')] ?: WorkspaceTab.ADJUSTMENTS

internal fun EditorModule.workspaceTab(): WorkspaceTab = workspaceTabForOperation(operation)

/** Unknown future tools remain discoverable in Adjustments. Settings contains no processing tools. */
internal fun List<EditorModule>.workspaceModules(
    tab: WorkspaceTab,
    query: String = "",
    enabledOnly: Boolean = false,
): List<EditorModule> = filterEditingTools(filter { it.workspaceTab() == tab }, query, enabledOnly)

/**
 * Resolve a remembered native identity against the current visible list. Undo can retire an
 * instance and filters can hide one; neither should leave a stale editor addressing that ID.
 * This only selects a tool. The power switch remains the explicit way to enable it.
 */
internal fun List<EditorModule>.preferredWorkspaceModule(
    tab: WorkspaceTab,
    rememberedOperation: String? = null,
): EditorModule? {
    val candidates = workspaceModules(tab)
    rememberedOperation?.let { remembered ->
        candidates.singleOrNull { it.operation == remembered }?.let { return it }
    }
    val preferred = when(tab) {
        WorkspaceTab.ADJUSTMENTS -> listOf("exposure", "temperature", "basicadj")
        WorkspaceTab.DETAIL -> listOf("denoiseprofile", "sharpen", "retouch")
        WorkspaceTab.GEOMETRY -> listOf("crop", "ashift", "lens")
        WorkspaceTab.SETTINGS -> return null
    }
    fun preferredTool(enabledOnly: Boolean): EditorModule? = preferred.firstNotNullOfOrNull { operation ->
        candidates.singleOrNull { it.operation == operation && it.hasEditableControls && (!enabledOnly || it.enabled) }
    }
    return preferredTool(true)
        ?: candidates.firstOrNull { it.enabled && it.hasEditableControls }
        ?: preferredTool(false)
        ?: candidates.firstOrNull { it.hasEditableControls }
        ?: candidates.firstOrNull()
}
