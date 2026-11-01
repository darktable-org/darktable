/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid

import android.app.Application
import android.graphics.Bitmap
import android.net.Uri
import android.provider.DocumentsContract
import androidx.lifecycle.AndroidViewModel
import java.io.File
import java.util.UUID
import java.util.concurrent.atomic.AtomicLong
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.example.darktableandroid.nativecore.EditorModule
import org.example.darktableandroid.nativecore.EditHistory
import org.example.darktableandroid.nativecore.CurvePoint
import org.example.darktableandroid.nativecore.CropRect
import org.example.darktableandroid.nativecore.curveCoordinates
import org.example.darktableandroid.nativecore.ExportFormat
import org.example.darktableandroid.nativecore.ExportSettings
import org.example.darktableandroid.nativecore.NativeCore
import org.example.darktableandroid.nativecore.RuntimeAssets
import org.example.darktableandroid.nativecore.parseExportFormats
import org.example.darktableandroid.nativecore.parseEditHistory
import org.example.darktableandroid.nativecore.parseModules
import org.example.darktableandroid.nativecore.instanceSource
import org.example.darktableandroid.nativecore.retainModuleSelection
import org.example.darktableandroid.nativecore.previewArgbPixels
import org.example.darktableandroid.nativecore.accepts
import org.example.darktableandroid.nativecore.textParameterForWrite
import org.example.darktableandroid.storage.UriCache
import org.example.darktableandroid.storage.ToolAssetCache

sealed interface EditorState {
    data object Empty : EditorState
    data class Working(val stage: Stage, val previous: Ready? = null) : EditorState
    data class Ready(
        val source: Uri,
        val sessionId: Long,
        val preview: Bitmap,
        val modules: List<EditorModule>,
        val exportFormats: List<ExportFormat>,
        val notice: String? = null,
        val history: EditHistory? = null,
        val selectedModule: String? = null,
    ) : EditorState
    data class Failed(val source: Uri?, val message: String) : EditorState
    enum class Stage { COPYING, DECODING, RENDERING, EDITING, EXPORTING, CANCELLING }
}

data class RuntimeAvailability(val checking: Boolean = true, val available: Boolean = false, val message: String? = null)

class EditorViewModel(application: Application) : AndroidViewModel(application) {
    private val mutableState = MutableStateFlow<EditorState>(EditorState.Empty)
    val state: StateFlow<EditorState> = mutableState.asStateFlow()
    private val mutableRuntime = MutableStateFlow(RuntimeAvailability())
    val runtime: StateFlow<RuntimeAvailability> = mutableRuntime.asStateFlow()
    // A separate owner scope lets cleanup finish after Android clears the ViewModel.
    private val engineScope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    private val nativeDispatcher = Dispatchers.IO.limitedParallelism(1)
    private var work: Job? = null
    private var session: NativeSession? = null
    private var pendingSource: Uri? = null
    private val generation = AtomicLong(UUID.randomUUID().mostSignificantBits and Long.MAX_VALUE)

    init {
        engineScope.launch {
            val initialization = withContext(nativeDispatcher) {
                runCatching {
                    check(NativeCore.capabilities() and 1 != 0) {
                        getApplication<Application>().getString(R.string.engine_unavailable)
                    }
                    RuntimeAssets.initializeNative(getApplication())
                }
            }
            val available = initialization.isSuccess
            mutableRuntime.value = RuntimeAvailability(
                checking = false,
                available = available,
                message = initialization.exceptionOrNull()?.let(::errorMessage),
            )
            pendingSource?.let { pendingSource = null; if(available) open(it) }
        }
    }

    fun open(uri: Uri) {
        if(mutableRuntime.value.checking) { pendingSource = uri; return }
        if(!mutableRuntime.value.available) return
        val request = generation.incrementAndGet()
        val previousWork = work
        val previousSession = session
        session = null
        previousSession?.cancel()
        previousWork?.cancel()
        mutableState.value = EditorState.Working(EditorState.Stage.COPYING)
        work = engineScope.launch {
            var opened: NativeSession? = null
            var retained = false
            try {
                releaseAfter(previousWork, previousSession)
                ensureActive()
                val cached = withContext(Dispatchers.IO) {
                    UriCache(getApplication()).copyForNative(uri) { request != generation.get() }
                }
                ensureActive()
                mutableState.value = EditorState.Working(EditorState.Stage.DECODING)
                // Keep ownership of a returned handle even if cancellation happens during open.
                val active = withContext(NonCancellable) {
                    withContext(nativeDispatcher) { NativeSession(NativeCore.open(cached.file.absolutePath),
                        File(getApplication<Application>().cacheDir, "tool-assets/${UUID.randomUUID()}")) }
                }
                opened = active
                ensureActive()
                if(request != generation.get()) throw CancellationException()
                session = active
                mutableState.value = EditorState.Working(EditorState.Stage.RENDERING)
                val result = withContext(nativeDispatcher) {
                    val handle = active.handle()
                    EditorState.Ready(uri, request, render(handle), parseModules(NativeCore.listModules(handle)),
                        parseExportFormats(NativeCore.listExportFormats(handle)),
                        history = parseEditHistory(NativeCore.history(handle)))
                }
                ensureActive()
                if(request == generation.get()) {
                    retained = true
                    mutableState.value = result
                }
            } catch(_: CancellationException) {
                // The replacing request owns the screen.
            } catch(failure: Throwable) {
                if(request == generation.get()) mutableState.value = EditorState.Failed(uri, errorMessage(failure))
            } finally {
                if(!retained || request != generation.get()) {
                    if(session === opened) session = null
                    nativeCleanup { opened?.close() }
                }
            }
        }
    }

    fun setParameter(operation: String, parameter: String, value: Double) {
        if(!value.isFinite()) return
        edit { NativeCore.setParameter(it, operation, parameter, value) }
    }

    fun setModuleEnabled(operation: String, enabled: Boolean) {
        edit { NativeCore.setModuleEnabled(it, operation, enabled) }
    }

    fun setTextParameter(operation: String, parameter: String, value: String) {
        val current = mutableState.value as? EditorState.Ready ?: return
        val control = current.modules.singleOrNull { it.operation == operation }
            ?.textParameterForWrite(parameter, value) ?: return
        if(control.accepts(value)) edit { NativeCore.setTextParameter(it, operation, parameter, value) }
    }

    fun importToolFile(operation: String, parameter: String, uri: Uri, expectedSessionId: Long) {
        val current = mutableState.value as? EditorState.Ready ?: return
        if(current.sessionId != expectedSessionId) return
        val active = session ?: return
        val control = current.modules.singleOrNull { it.operation == operation }
            ?.textParameters?.singleOrNull { it.name == parameter && it.isFile } ?: return
        val request = generation.get()
        edit { handle ->
            val file = ToolAssetCache(getApplication()).copyForNative(uri, active.assetsDirectory, control.extensions) {
                request != generation.get()
            }
            check(request == generation.get()) { "The image changed during file import" }
            check(control.accepts(file.absolutePath)) { "The imported file path exceeds the native tool buffer" }
            NativeCore.setTextParameter(handle, operation, parameter, file.absolutePath)
        }
    }

    fun selectModule(operation: String?) {
        val current = mutableState.value as? EditorState.Ready ?: return
        val selected = retainModuleSelection(current.modules, operation)
        if(operation != null && selected == null) return
        mutableState.value = current.copy(selectedModule = selected)
    }

    fun newInstance(operation: String) {
        val current = mutableState.value as? EditorState.Ready ?: return
        if(current.modules.instanceSource(operation) == null) return
        editWithSelection { NativeCore.newInstance(it, operation) }
    }

    fun setCurve(operation: String, channel: Int, interpolation: Int, nodes: List<CurvePoint>) {
        val coordinates = curveCoordinates(nodes)
        edit { NativeCore.setCurve(it, operation, channel, interpolation, coordinates) }
    }

    fun setCrop(operation: String, bounds: CropRect) {
        edit { NativeCore.setCrop(it, operation, bounds.left, bounds.top, bounds.right, bounds.bottom) }
    }

    fun setRgbCurveOptions(operation: String, mode: Int, preserveColors: Int) {
        val current = mutableState.value as? EditorState.Ready ?: return
        val options = current.modules.firstOrNull { it.operation == operation }?.rgbCurveOptions ?: return
        if(options.accepts(mode, preserveColors)) edit { NativeCore.setRgbCurveOptions(it, operation, mode, preserveColors) }
    }

    fun setWhiteBalanceGains(operation: String, gains: FloatArray) {
        val current = mutableState.value as? EditorState.Ready ?: return
        val balance = current.modules.firstOrNull { it.operation == operation }?.whiteBalance ?: return
        val captured = gains.copyOf()
        if(balance.acceptsGains(captured)) edit { NativeCore.setWhiteBalanceGains(it, operation, captured) }
    }

    fun setWhiteBalanceSetting(operation: String, setting: Int) {
        val current = mutableState.value as? EditorState.Ready ?: return
        val balance = current.modules.firstOrNull { it.operation == operation }?.whiteBalance ?: return
        if(balance.acceptsSetting(setting)) edit { NativeCore.setWhiteBalanceSetting(it, operation, setting) }
    }

    fun setWhiteBalanceTemperature(operation: String, kelvin: Double, tint: Double) {
        val current = mutableState.value as? EditorState.Ready ?: return
        val temperature = current.modules.firstOrNull { it.operation == operation }?.whiteBalance?.temperature ?: return
        if(temperature.accepts(kelvin, tint)) edit { NativeCore.setWhiteBalanceTemperature(it, operation, kelvin, tint) }
    }

    fun undo() {
        if((mutableState.value as? EditorState.Ready)?.history?.canUndo != true) return
        edit { NativeCore.undo(it) }
    }

    fun redo() {
        if((mutableState.value as? EditorState.Ready)?.history?.canRedo != true) return
        edit { NativeCore.redo(it) }
    }

    private fun edit(change: (Long) -> Unit) = editWithSelection { handle -> change(handle); null }

    private fun editWithSelection(change: (Long) -> String?) {
        val current = mutableState.value as? EditorState.Ready ?: return
        val currentSession = session ?: return
        val request = generation.get()
        mutableState.value = EditorState.Working(EditorState.Stage.EDITING, current)
        work = engineScope.launch {
            var preferredSelection: String? = null
            try {
                val updated = withContext(nativeDispatcher) {
                    val handle = currentSession.handle()
                    preferredSelection = change(handle)
                    val modules = parseModules(NativeCore.listModules(handle))
                    current.copy(preview = render(handle), modules = modules,
                        history = parseEditHistory(NativeCore.history(handle)), notice = null,
                        selectedModule = retainModuleSelection(modules, preferredSelection ?: current.selectedModule))
                }
                if(request == generation.get()) mutableState.value = updated
            } catch(_: CancellationException) {
            } catch(failure: Throwable) {
                if(request == generation.get()) {
                    // A native edit may have committed before preview rendering failed.
                    // Keep history controls and tool values aligned with that session.
                    val recovered = withContext(nativeDispatcher) {
                        runCatching {
                            val handle = currentSession.handle()
                            val modules = parseModules(NativeCore.listModules(handle))
                            current.copy(modules = modules, history = parseEditHistory(NativeCore.history(handle)),
                                selectedModule = retainModuleSelection(modules, preferredSelection ?: current.selectedModule))
                        }.getOrElse { current.copy(history = null) }
                    }
                    if(request == generation.get()) mutableState.value = recovered.copy(notice = errorMessage(failure))
                }
            }
        }
    }

    /** target must be the new document returned by this app's ACTION_CREATE_DOCUMENT request. */
    fun export(target: Uri, format: ExportFormat, quality: Int, expectedSessionId: Long, expectedSource: Uri) =
        export(target, format, quality, null, false, expectedSessionId, expectedSource)

    /** Decode now so a caller cannot mutate settings while the export job is queued. */
    fun export(target: Uri, format: ExportFormat, settings: IntArray, expectedSessionId: Long, expectedSource: Uri) =
        export(target, format, 95, ExportSettings.fromNativeArray(settings), true, expectedSessionId, expectedSource)

    private fun export(target: Uri, format: ExportFormat, quality: Int, settings: ExportSettings?,
                       extended: Boolean, expectedSessionId: Long, expectedSource: Uri) {
        val current = mutableState.value as? EditorState.Ready
        val currentSession = session
        if(target == expectedSource || target == current?.source) {
            if(current != null) mutableState.value = current.copy(
                notice = getApplication<Application>().getString(R.string.source_overwrite))
            return
        }
        if(current == null || currentSession == null || current.sessionId != expectedSessionId || current.source != expectedSource) {
            engineScope.launch { removeNewExport(target) }
            if(current != null) mutableState.value = current.copy(
                notice = getApplication<Application>().getString(R.string.export_source_changed))
            return
        }
        val request = generation.get()
        mutableState.value = EditorState.Working(EditorState.Stage.EXPORTING, current)
        work = engineScope.launch {
            var temporary: File? = null
            var committed = false
            try {
                withContext(nativeDispatcher) {
                    // Reuse the current session's advertised encoder, not metadata reconstructed
                    // after a document-picker or activity recreation.
                    val installed = checkNotNull(current.exportFormats.firstOrNull { it.id == format.id }) {
                        "The selected export format is unavailable"
                    }
                    if(extended) check(settings != null && installed.options?.isValid(settings) == true) {
                        "The selected export settings are unavailable"
                    }
                    val extension = installed.extension.filter(Char::isLetterOrDigit).take(12).ifEmpty { "export" }
                    temporary = File.createTempFile("darktable-export-", ".$extension", getApplication<Application>().cacheDir)
                    if(extended) NativeCore.exportWithSettings(currentSession.handle(), temporary!!.absolutePath,
                        installed.id, checkNotNull(settings).toNativeArray())
                    else NativeCore.exportImage(currentSession.handle(), temporary!!.absolutePath,
                        installed.id, quality.coerceIn(0, 100))
                    ensureActive()
                    check(temporary!!.length() > 0L) { "The export encoder produced an empty file" }
                    getApplication<Application>().contentResolver.openOutputStream(target, "wt").use { output ->
                        checkNotNull(output) { "The document provider returned no output stream" }
                        temporary!!.inputStream().buffered().use { input ->
                            val buffer = ByteArray(DEFAULT_BUFFER_SIZE)
                            while(true) {
                                ensureActive()
                                val count = input.read(buffer)
                                if(count < 0) break
                                output.write(buffer, 0, count)
                            }
                        }
                    }
                    committed = true
                }
                if(request == generation.get()) mutableState.value = current.copy(
                    notice = getApplication<Application>().getString(R.string.export_complete))
            } catch(_: CancellationException) {
            } catch(failure: Throwable) {
                if(request == generation.get()) mutableState.value = current.copy(notice = errorMessage(failure))
            } finally {
                withContext(NonCancellable + Dispatchers.IO) { temporary?.delete() }
                if(!committed) removeNewExport(target)
            }
        }
    }

    private suspend fun removeNewExport(target: Uri) = withContext(NonCancellable + Dispatchers.IO) {
        // Providers can reject deletion; never attempt to delete a source or any unrelated document.
        runCatching { DocumentsContract.deleteDocument(getApplication<Application>().contentResolver, target) }
        Unit
    }

    fun cancel(showState: Boolean = true) {
        pendingSource = null
        val request = generation.incrementAndGet()
        val previousWork = work
        val previousSession = session
        session = null
        previousSession?.cancel()
        previousWork?.cancel()
        if(showState) mutableState.value = EditorState.Working(EditorState.Stage.CANCELLING)
        work = engineScope.launch {
            releaseAfter(previousWork, previousSession)
            if(showState && request == generation.get()) mutableState.value = EditorState.Empty
        }
    }

    private fun render(handle: Long): Bitmap {
        val dimensions = IntArray(2)
        val rgba = NativeCore.renderPreview(handle, 2048, 2048, dimensions)
        val width = dimensions[0]
        val height = dimensions[1]
        check(width in 1..2048 && height in 1..2048 && rgba.size.toLong() == width.toLong() * height * 4) {
            "The native renderer returned invalid preview dimensions"
        }
        return Bitmap.createBitmap(previewArgbPixels(rgba), width, height, Bitmap.Config.ARGB_8888)
    }

    private suspend fun releaseAfter(previousWork: Job?, previousSession: NativeSession?) = withContext(NonCancellable) {
        previousWork?.join()
        withContext(nativeDispatcher) { previousSession?.close() }
    }

    private suspend fun nativeCleanup(block: () -> Unit) = withContext(NonCancellable) {
        withContext(nativeDispatcher) { block() }
    }

    private fun errorMessage(failure: Throwable) = failure.message ?: getApplication<Application>().getString(R.string.operation_failed)

    override fun onCleared() {
        cancel(false)
        work?.invokeOnCompletion { engineScope.cancel() }
        super.onCleared()
    }
}

/** Only the owning job uses the handle; close waits for that job to finish. */
private class NativeSession(private var pointer: Long, val assetsDirectory: File) {
    init { check(pointer != 0L) { "The native engine returned an invalid session" } }
    @Synchronized fun handle(): Long = pointer.also { check(it != 0L) { "The image session is closed" } }
    @Synchronized fun cancel() { if(pointer != 0L) NativeCore.cancel(pointer) }
    @Synchronized fun close() {
        if(pointer != 0L) {
            val closing = pointer
            pointer = 0L
            try { NativeCore.close(closing) } finally { assetsDirectory.deleteRecursively() }
        }
    }
}
