/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid.nativecore

object NativeCore {
    init { System.loadLibrary("dt_mobile") }
    external fun capabilities(): Int
    external fun configureRuntime(dataDir: String, moduleDir: String, tempRoot: String)
    external fun open(path: String): Long
    external fun lastError(handle: Long): String
    external fun renderPreview(handle: Long, maxWidth: Int, maxHeight: Int, dimensions: IntArray): ByteArray
    external fun listModules(handle: Long): String
    external fun history(handle: Long): String
    external fun undo(handle: Long)
    external fun redo(handle: Long)
    external fun newInstance(handle: Long, module: String): String
    external fun setParameter(handle: Long, module: String, parameter: String, value: Double)
    external fun sampleToneEqualizer(handle: Long, module: String, x: Double, y: Double): Double
    external fun adjustToneEqualizer(handle: Long, module: String, exposure: Double, offset: Double)
    external fun toneEqualizerOutput(handle: Long, module: String, exposure: Double): Double
    external fun setToneEqualizerTarget(handle: Long, module: String, exposure: Double, target: Double, gesture: Long): Double
    external fun setTextParameter(handle: Long, module: String, parameter: String, value: String)
    external fun setCurve(handle: Long, module: String, channel: Int, interpolation: Int, coordinates: FloatArray)
    external fun setRgbCurveOptions(handle: Long, module: String, mode: Int, preserveColors: Int)
    external fun setWhiteBalanceGains(handle: Long, module: String, gains: FloatArray)
    external fun setWhiteBalanceSetting(handle: Long, module: String, setting: Int)
    external fun setWhiteBalanceTemperature(handle: Long, module: String, kelvin: Double, tint: Double)
    external fun setCrop(handle: Long, module: String, left: Float, top: Float, right: Float, bottom: Float)
    external fun setModuleEnabled(handle: Long, module: String, enabled: Boolean)
    external fun listExportFormats(handle: Long): String
    external fun exportImage(handle: Long, path: String, format: String, quality: Int)
    external fun exportWithSettings(handle: Long, path: String, format: String, settings: IntArray)
    external fun cancel(handle: Long)
    external fun close(handle: Long)
}
