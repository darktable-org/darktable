/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid

import org.example.darktableandroid.nativecore.LensCorrection
import org.example.darktableandroid.nativecore.ModuleParameter
import org.example.darktableandroid.nativecore.ModuleTextParameter

internal data class LensCorrectionControls(
    val method: ModuleParameter?,
    val actions: List<ModuleParameter>,
    val shooting: List<ModuleParameter>,
    val correction: List<ModuleParameter>,
    val manualVignette: List<ModuleParameter>,
    val cameraProfile: ModuleTextParameter?,
    val lensProfile: ModuleTextParameter?,
    /** Known fields stay with this editor even when their method does not consume them. */
    val names: Set<String>,
    val textNames: Set<String>,
)

private val lensFieldNames = setOf("method", "modify_flags", "inverse", "target_geom", "tca_override",
    "scale", "tca_r", "tca_b", "focal", "aperture", "distance", "crop", "cor_dist_ft", "cor_vig_ft",
    "cor_ca_r_ft", "cor_ca_b_ft", "scale_md", "v_strength", "v_radius", "v_steepness",
    "lens.autodetect", "lens.autoscale")
private val lensTextNames = setOf("camera", "lens", "lens.camera_profile", "lens.profile")

internal fun lensCorrectionControls(
    info: LensCorrection,
    parameters: List<ModuleParameter>,
    textParameters: List<ModuleTextParameter>,
): LensCorrectionControls {
    fun fields(vararg names: String): List<ModuleParameter> = names.mapNotNull { name -> parameters.singleOrNull { it.name == name } }
    // The original camera/lens arrays remain the native buffer authority. The
    // capability object adds strict catalog-selection operations without
    // requiring those aliases to be repeated in the text metadata array.
    fun profileControl(name: String, legacy: String, selected: String): ModuleTextParameter? =
        textParameters.singleOrNull { it.name == name }
            ?: textParameters.singleOrNull { it.name == legacy }?.copy(name = name, value = selected)
    val method = parameters.singleOrNull { it.name == "method" }?.let { parameter ->
        parameter.copy(choices = parameter.choices.filter { it.value != 0.0 || info.embeddedAvailable })
    }
    val supported = info.supportedFlags
    val correction = when(info.method) {
        // The original embedded warp strength can scale geometry, chromatic
        // aberration, or both. Its native field availability is authoritative;
        // a CA-only three-plane DNG warp does not advertise DISTORTION.
        0 -> if(info.embeddedAvailable) fields("modify_flags", "scale_md", "cor_dist_ft") +
            (if(supported and 2 != 0) fields("cor_vig_ft") else emptyList()) +
            (if(supported and 1 != 0) fields("cor_ca_r_ft", "cor_ca_b_ft") else emptyList()) else emptyList()
        1 -> if(info.databaseAvailable && info.cameraMatched && info.lensMatched) fields("modify_flags", "inverse", "target_geom", "scale", "tca_override") +
            (if(parameters.singleOrNull { it.name == "tca_override" }?.value == 1.0)
                fields("tca_r", "tca_b") else emptyList()) else emptyList()
        else -> emptyList()
    }
    return LensCorrectionControls(method,
        fields("lens.autodetect") + if(info.method == 1 && info.databaseAvailable && info.cameraMatched && info.lensMatched)
            fields("lens.autoscale") else emptyList(),
        if(info.method == 1 && info.databaseAvailable && info.cameraMatched) fields("focal", "aperture", "distance", "crop") else emptyList(),
        correction, fields("v_strength") + if(parameters.singleOrNull { it.name == "v_strength" }?.value?.let { it > 0.0 } == true)
            fields("v_radius", "v_steepness") else emptyList(),
        profileControl("lens.camera_profile", "camera", info.camera),
        if(info.cameraMatched) profileControl("lens.profile", "lens", info.lens) else null,
        parameters.map { it.name }.filterTo(linkedSetOf()) { it in lensFieldNames },
        textParameters.map { it.name }.filterTo(linkedSetOf()) { it in lensTextNames })
}
