/* SPDX-License-Identifier: GPL-3.0-or-later */
package org.example.darktableandroid

import android.net.Uri
import androidx.activity.compose.BackHandler
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.ime
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.FilterChip
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.ListItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.saveable.rememberSaveableStateHolder
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalSoftwareKeyboardController
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalFocusManager
import androidx.compose.ui.platform.LocalUriHandler
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.stateDescription
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import org.example.darktableandroid.nativecore.EditorModule
import org.example.darktableandroid.nativecore.MaskPhotoPoint

/** A persistent editor: parameter scrolling has no dismiss or navigation gesture. */
@Composable
internal fun EditorWorkspace(
    state: EditorState,
    runtime: RuntimeAvailability,
    onOpen: () -> Unit,
    onExport: () -> Unit,
    onUndo: () -> Unit,
    onRedo: () -> Unit,
    onCancel: () -> Unit,
    onRetry: (Uri) -> Unit,
    onSelectModule: (String) -> Unit,
    onEnabled: (String, Boolean) -> Unit,
    onRefreshPreview: () -> Unit = {},
    toneSample: ToneEqualizerSample? = null,
    onToneSample: (String, Float, Float) -> Unit = { _, _, _ -> },
    onToneAdjust: (ToneEqualizerSample, Double, Long, Boolean) -> Unit = { _, _, _, _ -> },
    maskPhotoSelection: MaskPhotoSelection? = null,
    onLoadMaskPhoto: (String) -> Unit = {},
    onPlaceMaskPhoto: (MaskPhotoTarget, Float, Float) -> Unit = { _, _, _ -> },
    moduleContent: @Composable (EditorModule, String) -> Unit,
) {
    val ready = when(state) {
        is EditorState.Ready -> state
        is EditorState.Working -> state.previous
        else -> null
    }
    val busy = state is EditorState.Working
    val toneUpdating = state is EditorState.Working && state.stage == EditorState.Stage.TONE_ADJUSTING
    val keyboardVisible = WindowInsets.ime.getBottom(LocalDensity.current) > 0
    val keyboard = LocalSoftwareKeyboardController.current
    val focus = LocalFocusManager.current
    val workspaceState = rememberSaveableStateHolder()
    var workspaceSession by rememberSaveable { mutableStateOf<Long?>(null) }
    var tabName by rememberSaveable { mutableStateOf(WorkspaceTab.ADJUSTMENTS.name) }
    val tab = WorkspaceTab.entries.firstOrNull { it.name == tabName } ?: WorkspaceTab.ADJUSTMENTS
    val toneModule = ready?.modules?.singleOrNull {
        it.operation == ready.selectedModule && it.operation.substringBefore('@') == "toneequal"
    }
    var toneImageMode by rememberSaveable(ready?.sessionId, ready?.selectedModule, tabName) { mutableStateOf(false) }
    var tonePoint by remember(ready?.sessionId, ready?.selectedModule) { mutableStateOf<ToneEqualizerPoint?>(null) }
    val maskModule = ready?.modules?.singleOrNull { module ->
        module.operation == ready.selectedModule && module.parameters.any { maskControlAddress(it.name) != null }
    }
    var maskImageMode by rememberSaveable(ready?.sessionId, ready?.selectedModule, tabName) { mutableStateOf(false) }
    var maskFormId by rememberSaveable(ready?.sessionId, ready?.selectedModule, tabName) { mutableStateOf<Int?>(null) }
    var maskSource by rememberSaveable(ready?.sessionId, ready?.selectedModule, tabName) { mutableStateOf(false) }
    var maskLoaded by remember(ready?.sessionId, ready?.selectedModule, tabName) { mutableStateOf(false) }
    val maskAvailable = maskModule?.enabled == true && ready?.previewCurrent == true &&
        workspaceTabForOperation(maskModule.operation) == tab
    val currentMaskSelection = maskPhotoSelection?.takeIf { selection ->
        ready != null && selection.sessionId == ready.sessionId && selection.module == maskModule?.operation &&
            selection.preview === ready.preview
    }
    val maskForms = currentMaskSelection?.points?.forms.orEmpty()
    val maskForm = maskForms.singleOrNull { it.id == maskFormId } ?: maskForms.firstOrNull()
    val maskUsesSource = maskForm?.let { it.center == null || (maskSource && it.source != null) } ?: false
    val maskActive = maskAvailable && maskImageMode
    val maskTarget = if(maskActive && currentMaskSelection != null && maskForm != null)
        MaskPhotoTarget(currentMaskSelection.sessionId, currentMaskSelection.module, currentMaskSelection.preview,
            maskForm.id, maskUsesSource) else null
    val maskPoint = maskForm?.let { if(maskUsesSource) it.source else it.center }
    val toneAvailable = tab == WorkspaceTab.ADJUSTMENTS && toneModule?.enabled == true && ready?.previewCurrent == true
    val toneActive = toneAvailable && toneImageMode
    val currentSample = toneSample?.takeIf { sample ->
        ready != null && sample.sessionId == ready.sessionId && sample.module == toneModule?.operation &&
            sample.preview === ready.preview && sample.x.isFinite() && sample.y.isFinite() &&
            sample.x >= 0f && sample.x < 1f && sample.y >= 0f && sample.y < 1f &&
            sample.exposure.isFinite() && sample.selectedExposure.isFinite() &&
            (sample.pendingExposure == null || sample.pendingExposure.isFinite())
    }
    val selectedSample = currentSample?.takeIf { sample -> tonePoint?.let { it.x == sample.x && it.y == sample.y } == true }
    fun sampleTone(point: ToneEqualizerPoint) {
        if(toneActive && !busy && toneModule != null) {
            tonePoint = point
            onToneSample(toneModule.operation, point.x, point.y)
        }
    }
    LaunchedEffect(toneAvailable) {
        if(!toneAvailable) { toneImageMode = false; tonePoint = null }
    }
    LaunchedEffect(maskAvailable, maskImageMode, currentMaskSelection, busy) {
        if(!maskAvailable || !maskImageMode) {
            maskImageMode = false
            maskLoaded = false
            maskFormId = null
            maskSource = false
        } else if(currentMaskSelection != null) {
            maskLoaded = true
            maskFormId = maskForm?.id
            maskSource = maskUsesSource
        } else if(maskLoaded && !busy) {
            // Generic edits, Undo and image changes revoke the projection lease.
            maskImageMode = false
            maskLoaded = false
            maskFormId = null
            maskSource = false
        }
    }
    LaunchedEffect(ready?.preview) {
        // A tone-only adjustment may return the same selection bound to a new
        // preview. Other edits invalidate its image geometry and input mask.
        if(currentSample == null) tonePoint = null
    }
    LaunchedEffect(ready?.sessionId) {
        if(ready != null && workspaceSession != ready.sessionId) {
            workspaceSession = ready.sessionId
            tabName = WorkspaceTab.ADJUSTMENTS.name
        }
    }
    Scaffold(modifier = Modifier.fillMaxSize().imePadding(), bottomBar = {
        NavigationBar {
            WorkspaceTab.entries.forEach { item ->
                NavigationBarItem(selected = tab == item, onClick = {
                    focus.clearFocus()
                    keyboard?.hide()
                    tabName = item.name
                },
                    icon = { Icon(painterResource(item.iconResource()), null) },
                    label = { Text(stringResource(item.labelResource()), maxLines = 2, overflow = TextOverflow.Ellipsis) })
            }
        }
    }) { padding ->
        Column(Modifier.fillMaxSize().padding(padding)) {
            WorkspacePreview(state, runtime, ready, busy, onOpen, onExport, onUndo, onRedo,
                onCancel, onRetry, onRefreshPreview, Modifier.fillMaxWidth().weight(if(keyboardVisible) .25f else 1f)
                    .testTag("editor_preview_pane"), toneActive, tonePoint, ::sampleTone,
                maskActive, maskTarget, maskPoint, onPlaceMaskPhoto)
            HorizontalDivider()
            Box(Modifier.fillMaxWidth().weight(if(keyboardVisible) .75f else 1f).testTag("editor_tools_pane")) {
                if(tab == WorkspaceTab.SETTINGS) WorkspaceAppInfo(runtime)
                else if(ready == null) WorkspaceEmptyTools(state, runtime, onOpen)
                else workspaceState.SaveableStateProvider("image-${ready.sessionId}") {
                    WorkspaceTools(ready, tab, busy, onSelectModule, onEnabled,
                        onSelectTab = { tabName = it.name }, beforeControls = { selected ->
                        if(selected.operation == toneModule?.operation) {
                            ToneEqualizerTouchEditor((!busy || toneUpdating) && toneAvailable, toneActive, selectedSample,
                                onImageMode = { active ->
                                    toneImageMode = active; tonePoint = null
                                    if(active) { maskImageMode = false; maskLoaded = false }
                                },
                                onSelectCentre = { sampleTone(ToneEqualizerPoint(.5f, .5f)) },
                                onAdjust = onToneAdjust, selectionEnabled = !busy && toneAvailable)
                            HorizontalDivider()
                        }
                        if(selected.operation == maskModule?.operation) {
                            MaskPhotoEditor(!busy && maskAvailable, maskActive, currentMaskSelection?.points,
                                maskForm?.id, maskUsesSource, onImageMode = { active ->
                                    maskImageMode = active
                                    maskLoaded = false
                                    maskFormId = null
                                    maskSource = false
                                    if(active) {
                                        toneImageMode = false; tonePoint = null
                                        onLoadMaskPhoto(selected.operation)
                                    }
                                }, onForm = { id -> maskFormId = id; maskSource = false },
                                onSource = { maskSource = it })
                            HorizontalDivider()
                        }
                    }, moduleContent = moduleContent)
                }
            }
        }
    }
}

@Composable
private fun WorkspacePreview(
    state: EditorState, runtime: RuntimeAvailability, ready: EditorState.Ready?, busy: Boolean,
    onOpen: () -> Unit, onExport: () -> Unit, onUndo: () -> Unit, onRedo: () -> Unit,
    onCancel: () -> Unit, onRetry: (Uri) -> Unit, onRefreshPreview: () -> Unit, modifier: Modifier,
    toneActive: Boolean,
    tonePoint: ToneEqualizerPoint?,
    onTonePoint: (ToneEqualizerPoint) -> Unit,
    maskActive: Boolean,
    maskTarget: MaskPhotoTarget?,
    maskPoint: MaskPhotoPoint?,
    onPlaceMaskPhoto: (MaskPhotoTarget, Float, Float) -> Unit,
) {
    Box(modifier.background(MaterialTheme.colorScheme.surfaceVariant)) {
        ready?.let {
            Box(Modifier.fillMaxSize().padding(horizontal = 8.dp).padding(bottom = 48.dp)) {
                Image(it.preview.asImageBitmap(), stringResource(R.string.preview_description),
                    Modifier.fillMaxSize(), contentScale = ContentScale.Fit)
                if(toneActive) ToneEqualizerPhotoTarget(it.preview.width, it.preview.height, !busy, tonePoint, onTonePoint)
                if(maskActive) MaskPhotoTargetOverlay(it.preview.width, it.preview.height, !busy,
                    maskTarget, maskPoint, onPlaceMaskPhoto)
            }
        }
        if(ready == null) Column(Modifier.align(Alignment.Center).padding(24.dp),
            horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.spacedBy(12.dp)) {
            when {
                runtime.checking || state is EditorState.Working -> CircularProgressIndicator()
                state is EditorState.Failed -> {
                    Text(state.message, color = MaterialTheme.colorScheme.error)
                    state.source?.let { uri -> TextButton(onClick = { onRetry(uri) }) { Text(stringResource(R.string.retry)) } }
                }
                !runtime.available -> Text(runtime.message.orEmpty(), color = MaterialTheme.colorScheme.error)
                else -> Text(stringResource(R.string.empty_state))
            }
        }
        if(state is EditorState.Working) Surface(Modifier.align(Alignment.TopCenter).fillMaxWidth(),
            color = MaterialTheme.colorScheme.surface.copy(alpha = 0.96f)) {
            Column {
                LinearProgressIndicator(Modifier.fillMaxWidth())
                Row(Modifier.fillMaxWidth().padding(start = 12.dp), verticalAlignment = Alignment.CenterVertically) {
                    Text(stringResource(state.stage.workspaceLabelResource()), Modifier.weight(1f),
                        style = MaterialTheme.typography.bodySmall, maxLines = 1, overflow = TextOverflow.Ellipsis)
                    if(state.stage != EditorState.Stage.TONE_ADJUSTING)
                        TextButton(onClick = onCancel, enabled = state.stage != EditorState.Stage.CANCELLING) {
                            Text(stringResource(R.string.cancel))
                        }
                }
            }
        } else if(ready != null && !ready.previewCurrent) {
            Surface(Modifier.align(Alignment.TopCenter).fillMaxWidth(),
                color = MaterialTheme.colorScheme.surface.copy(alpha = 0.96f)) {
                Column(Modifier.padding(horizontal = 8.dp, vertical = 4.dp)) {
                    Text(stringResource(R.string.tone_touch_stale_preview), style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.error)
                    ready.notice?.let { notice ->
                        Text(notice, style = MaterialTheme.typography.bodySmall, maxLines = 2, overflow = TextOverflow.Ellipsis)
                    }
                    TextButton(onClick = onRefreshPreview, enabled = !busy,
                        modifier = Modifier.heightIn(min = 48.dp).testTag("editor_retry_preview")) {
                        Text(stringResource(R.string.tone_touch_retry_preview))
                    }
                }
            }
        } else ready?.notice?.let { notice ->
            Surface(Modifier.align(Alignment.TopCenter).fillMaxWidth(),
                color = MaterialTheme.colorScheme.surface.copy(alpha = 0.96f)) {
                Text(notice, Modifier.padding(8.dp), style = MaterialTheme.typography.bodySmall,
                    maxLines = 3, overflow = TextOverflow.Ellipsis)
            }
        }
        Surface(Modifier.align(Alignment.BottomCenter).fillMaxWidth(), tonalElevation = 2.dp) {
            Row(Modifier.height(48.dp).fillMaxWidth(), horizontalArrangement = Arrangement.SpaceEvenly,
                verticalAlignment = Alignment.CenterVertically) {
                WorkspaceAction(R.drawable.workspace_ic_open, R.string.open_photo, "editor_open",
                    runtime.available && !busy, onOpen)
                WorkspaceAction(R.drawable.workspace_ic_undo, R.string.undo, "editor_undo",
                    !busy && ready?.history?.canUndo == true, onUndo)
                WorkspaceAction(R.drawable.workspace_ic_redo, R.string.redo, "editor_redo",
                    !busy && ready?.history?.canRedo == true, onRedo)
                WorkspaceAction(R.drawable.workspace_ic_export, R.string.export, "editor_export",
                    !busy && ready?.exportFormats?.isNotEmpty() == true, onExport)
            }
        }
    }
}

@Composable
private fun WorkspaceAction(icon: Int, label: Int, tag: String, enabled: Boolean, onClick: () -> Unit) {
    IconButton(onClick = onClick, enabled = enabled, modifier = Modifier.testTag(tag)) {
        Icon(painterResource(icon), stringResource(label))
    }
}

@Composable
private fun WorkspaceTools(
    ready: EditorState.Ready, tab: WorkspaceTab, busy: Boolean,
    onSelectModule: (String) -> Unit, onEnabled: (String, Boolean) -> Unit,
    onSelectTab: (WorkspaceTab) -> Unit,
    beforeControls: @Composable (EditorModule) -> Unit,
    moduleContent: @Composable (EditorModule, String) -> Unit,
) {
    var adjustOperation by rememberSaveable { mutableStateOf("") }
    var detailOperation by rememberSaveable { mutableStateOf("") }
    var geometryOperation by rememberSaveable { mutableStateOf("") }
    var query by rememberSaveable { mutableStateOf("") }
    var searchVisible by rememberSaveable { mutableStateOf(false) }
    var enabledOnly by rememberSaveable { mutableStateOf(false) }
    fun rememberOperation(operation: String) {
        when(workspaceTabForOperation(operation)) {
            WorkspaceTab.ADJUSTMENTS -> adjustOperation = operation
            WorkspaceTab.DETAIL -> detailOperation = operation
            WorkspaceTab.GEOMETRY -> geometryOperation = operation
            WorkspaceTab.SETTINGS -> Unit
        }
    }
    val remembered = when(tab) {
        WorkspaceTab.ADJUSTMENTS -> adjustOperation
        WorkspaceTab.DETAIL -> detailOperation
        WorkspaceTab.GEOMETRY -> geometryOperation
        WorkspaceTab.SETTINGS -> ""
    }
    val currentInCategory = ready.selectedModule?.takeIf { workspaceTabForOperation(it) == tab }
    // Search is navigation, not an edit or a filter on the selected tool. Keep
    // normal category selection independent from the query until a result is tapped.
    val shown = ready.modules.workspaceModules(tab)
    val selected = shown.preferredWorkspaceModule(tab, currentInCategory ?: remembered)
    LaunchedEffect(ready.selectedModule) { ready.selectedModule?.let(::rememberOperation) }
    LaunchedEffect(selected?.operation, busy) {
        if(!busy && selected != null && selected.operation != ready.selectedModule) onSelectModule(selected.operation)
    }
    val keyboard = LocalSoftwareKeyboardController.current
    val focus = LocalFocusManager.current
    val searchFocus = remember { FocusRequester() }
    fun closeSearch() {
        searchVisible = false
        query = ""
        enabledOnly = false
        focus.clearFocus()
        keyboard?.hide()
    }
    BackHandler(enabled = searchVisible) { closeSearch() }
    LaunchedEffect(searchVisible) {
        if(searchVisible) searchFocus.requestFocus()
    }
    val savedControls = rememberSaveableStateHolder()
    Column(Modifier.fillMaxSize()) {
        Row(Modifier.fillMaxWidth().padding(start = 12.dp, end = 4.dp, top = 6.dp),
            verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(4.dp)) {
            var expanded by rememberSaveable(tab) { mutableStateOf(false) }
            Box(Modifier.weight(1f)) {
                val chooseDescription = stringResource(R.string.workspace_choose_tool)
                OutlinedButton(onClick = { expanded = true }, enabled = !busy && shown.isNotEmpty(),
                    modifier = Modifier.fillMaxWidth().testTag("editor_tool_picker")
                        .semantics { contentDescription = chooseDescription }) {
                    Text(selected?.name ?: stringResource(R.string.workspace_no_tools), Modifier.weight(1f),
                        maxLines = 1, overflow = TextOverflow.Ellipsis)
                    Icon(painterResource(R.drawable.workspace_ic_expand), null)
                }
                DropdownMenu(expanded = expanded, onDismissRequest = { expanded = false },
                    modifier = Modifier.heightIn(max = 280.dp).widthIn(min = 240.dp, max = 360.dp)) {
                    shown.forEach { module ->
                        DropdownMenuItem(text = {
                            Text(module.name, fontWeight = if(module.operation == selected?.operation)
                                FontWeight.SemiBold else FontWeight.Normal)
                        }, enabled = !busy, onClick = {
                            expanded = false
                            closeSearch()
                            rememberOperation(module.operation)
                            onSelectModule(module.operation)
                        }, trailingIcon = {
                            Text(stringResource(if(module.enabled) R.string.workspace_enabled else R.string.workspace_disabled),
                                style = MaterialTheme.typography.labelSmall)
                        })
                    }
                }
            }
            selected?.let { module ->
                val description = stringResource(R.string.workspace_tool_toggle, module.name)
                val status = stringResource(if(module.enabled) R.string.workspace_enabled else R.string.workspace_disabled)
                Switch(checked = module.enabled, onCheckedChange = { onEnabled(module.operation, it) },
                    enabled = !busy && module.canDisable,
                    modifier = Modifier.testTag("editor_tool_switch").semantics {
                        contentDescription = description
                        stateDescription = status
                    })
            }
            IconButton(onClick = {
                if(searchVisible) closeSearch() else searchVisible = true
            }, modifier = Modifier.testTag("editor_tool_search_button")) {
                Icon(painterResource(if(searchVisible) R.drawable.workspace_ic_close else R.drawable.workspace_ic_search),
                    stringResource(if(searchVisible) R.string.workspace_close_search else R.string.tools_search))
            }
        }
        if(searchVisible) Row(Modifier.fillMaxWidth().padding(horizontal = 12.dp),
            verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            OutlinedTextField(value = query, onValueChange = { query = it },
                modifier = Modifier.weight(1f).focusRequester(searchFocus).testTag("editor_tool_search_input"), singleLine = true,
                label = { Text(stringResource(R.string.tools_search)) },
                keyboardOptions = KeyboardOptions(imeAction = ImeAction.Search),
                keyboardActions = KeyboardActions(onSearch = { keyboard?.hide() }))
            FilterChip(selected = enabledOnly, onClick = { enabledOnly = !enabledOnly },
                modifier = Modifier.testTag("editor_tool_search_enabled"),
                label = { Text(stringResource(R.string.tools_enabled)) })
        }
        if(!searchVisible) selected?.takeIf { !it.enabled }?.let {
            Text(stringResource(R.string.workspace_disabled_hint), Modifier.padding(horizontal = 12.dp, vertical = 4.dp),
                style = MaterialTheme.typography.bodySmall)
        }
        HorizontalDivider(Modifier.padding(top = 4.dp))
        if(searchVisible) {
            WorkspaceToolSearchResults(filterEditingTools(ready.modules, query, enabledOnly), busy) { module ->
                closeSearch()
                rememberOperation(module.operation)
                onSelectTab(module.workspaceTab())
                onSelectModule(module.operation)
            }
        } else if(selected == null) {
            Text(stringResource(R.string.tools_no_matches), Modifier.padding(16.dp), style = MaterialTheme.typography.bodyMedium)
        } else savedControls.SaveableStateProvider(selected.operation) {
            Column(Modifier.fillMaxSize().testTag("editor_parameter_scroll")
                .verticalScroll(rememberScrollState()).padding(horizontal = 12.dp, vertical = 8.dp)) {
                beforeControls(selected)
                moduleContent(selected, "")
                Spacer(Modifier.height(12.dp))
            }
        }
    }
}

@Composable
private fun WorkspaceToolSearchResults(results: List<EditorModule>, busy: Boolean, onSelect: (EditorModule) -> Unit) {
    Text(stringResource(R.string.workspace_search_results, results.size),
        Modifier.padding(horizontal = 12.dp, vertical = 6.dp).testTag("editor_tool_search_count"),
        style = MaterialTheme.typography.labelMedium)
    if(results.isEmpty()) {
        Text(stringResource(R.string.tools_no_matches), Modifier.padding(16.dp), style = MaterialTheme.typography.bodyMedium)
    } else LazyColumn(Modifier.fillMaxSize().testTag("editor_tool_search_results")) {
        items(results, key = { it.operation }) { module ->
            ListItem(modifier = Modifier.fillMaxWidth()
                .testTag("editor_tool_search_result_${module.operation}")
                .clickable(enabled = !busy) { onSelect(module) },
                headlineContent = { Text(module.name) },
                supportingContent = {
                    Text(stringResource(R.string.workspace_search_result_detail,
                        stringResource(module.workspaceTab().labelResource()), module.operation))
                },
                trailingContent = {
                    Text(stringResource(if(module.enabled) R.string.workspace_enabled else R.string.workspace_disabled),
                        style = MaterialTheme.typography.labelSmall)
                })
            HorizontalDivider()
        }
    }
}

@Composable
private fun WorkspaceEmptyTools(state: EditorState, runtime: RuntimeAvailability, onOpen: () -> Unit) {
    Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(20.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp)) {
        Text(stringResource(R.string.workspace_choose_photo), style = MaterialTheme.typography.titleMedium)
        Text(stringResource(R.string.workspace_layout_hint), style = MaterialTheme.typography.bodyMedium)
        Button(onClick = onOpen, enabled = runtime.available && state !is EditorState.Working) {
            Text(stringResource(R.string.open_photo))
        }
    }
}

@Composable
private fun WorkspaceAppInfo(runtime: RuntimeAvailability) {
    val uriHandler = LocalUriHandler.current
    Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(20.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp)) {
        Text(stringResource(R.string.about), style = MaterialTheme.typography.titleMedium)
        Text(stringResource(R.string.unofficial_notice), style = MaterialTheme.typography.bodyMedium)
        HorizontalDivider()
        Text(stringResource(R.string.workspace_license_title), style = MaterialTheme.typography.titleSmall)
        SelectionContainer { Text("GPL-3.0-or-later", style = MaterialTheme.typography.bodyMedium) }
        TextButton(onClick = { uriHandler.openUri("https://github.com/darktable-org/darktable") }) {
            Text(stringResource(R.string.workspace_upstream))
        }
        SelectionContainer {
            Text("Upstream commit: 249f20eda79fdbc650e89778694c4829dba64d4b",
                style = MaterialTheme.typography.bodySmall)
        }
        Text(stringResource(if(runtime.available) R.string.workspace_runtime_ready else R.string.workspace_runtime_missing),
            style = MaterialTheme.typography.bodySmall)
    }
}

private fun WorkspaceTab.labelResource(): Int = when(this) {
    WorkspaceTab.ADJUSTMENTS -> R.string.workspace_tab_adjustments
    WorkspaceTab.DETAIL -> R.string.workspace_tab_detail
    WorkspaceTab.GEOMETRY -> R.string.workspace_tab_geometry
    WorkspaceTab.SETTINGS -> R.string.workspace_tab_settings
}

private fun WorkspaceTab.iconResource(): Int = when(this) {
    WorkspaceTab.ADJUSTMENTS -> R.drawable.workspace_ic_adjust
    WorkspaceTab.DETAIL -> R.drawable.workspace_ic_detail
    WorkspaceTab.GEOMETRY -> R.drawable.workspace_ic_geometry
    WorkspaceTab.SETTINGS -> R.drawable.workspace_ic_info
}

private fun EditorState.Stage.workspaceLabelResource(): Int = when(this) {
    EditorState.Stage.COPYING -> R.string.stage_copying
    EditorState.Stage.DECODING -> R.string.stage_decoding
    EditorState.Stage.RENDERING -> R.string.stage_rendering
    EditorState.Stage.EDITING -> R.string.stage_editing
    EditorState.Stage.TONE_ADJUSTING -> R.string.tone_touch_updating
    EditorState.Stage.MASK_LOADING -> R.string.mask_photo_preparing
    EditorState.Stage.SAMPLING -> R.string.tone_touch_sampling
    EditorState.Stage.EXPORTING -> R.string.stage_exporting
    EditorState.Stage.CANCELLING -> R.string.stage_cancelling
}
