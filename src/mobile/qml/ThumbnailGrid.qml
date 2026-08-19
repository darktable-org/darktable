import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

Page {
    id: rootPage
    objectName: "gallery"
    background: Rectangle { color: "#111111" }

    // ── selection state ───────────────────────────────────────────────────────
    property bool selectionMode: false
    property var  selectedSet:   ({})
    property int  selectedCount: 0
    property int  _selGen: 0

    function toggleSelection(rawPath) {
        var s = Object.assign({}, selectedSet)
        if (rawPath in s) { delete s[rawPath]; selectedCount-- }
        else              { s[rawPath] = true;  selectedCount++ }
        selectedSet = s
        _selGen++
    }

    function clearSelection() {
        selectedSet   = ({})
        selectedCount = 0
        _selGen++
        selectionMode = false
    }

    function shareSelected() {
        shareHelper.shareRawPaths(Object.keys(selectedSet))
        clearSelection()
    }

    // ── helpers ───────────────────────────────────────────────────────────────
    // Format "20260615" → "2026-06-15"
    function formatRoll(roll) {
        return roll.length === 8
            ? roll.slice(0, 4) + "-" + roll.slice(4, 6) + "-" + roll.slice(6)
            : roll
    }

    readonly property var colorNames:  ["Red","Yellow","Green","Blue","Purple"]
    readonly property var colorValues: ["#e02020","#e0b020","#20a020","#2060e0","#9020c0"]

    // ── header ────────────────────────────────────────────────────────────────
    header: ToolBar {
        Material.background: rootPage.selectionMode ? "#1a3a5c" : "#1e1e1e"
        Behavior on Material.background { ColorAnimation { duration: 120 } }

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 4
            anchors.rightMargin: 4

            ToolButton {
                text: "✕"; font.pixelSize: 18
                visible: rootPage.selectionMode
                onClicked: rootPage.clearSelection()
            }

            Label {
                Layout.fillWidth: true
                text: {
                    if (rootPage.selectionMode)
                        return rootPage.selectedCount === 0
                               ? "Select images"
                               : rootPage.selectedCount + " selected"
                    const total    = imageModel.count
                    const filtered = filterModel.count
                    const suffix   = filterModel.isFiltered()
                                     ? " (" + filtered + "/" + total + ")"
                                     : " (" + total + ")"
                    return "Gallery" + suffix
                }
                font.pixelSize: 16
                font.bold: !rootPage.selectionMode
                horizontalAlignment: rootPage.selectionMode ? Text.AlignLeft : Text.AlignHCenter
            }

            ToolButton {
                icon.source: "icons/share.svg"
                icon.color: "white"; icon.width: 24; icon.height: 24
                text: "Share"; display: AbstractButton.TextBesideIcon
                visible: rootPage.selectionMode && rootPage.selectedCount > 0
                highlighted: true; Material.accent: Material.Blue
                onClicked: rootPage.shareSelected()
            }

            ToolButton {
                text: rootPage.selectedCount === filterModel.count ? "None" : "All"
                visible: rootPage.selectionMode; font.pixelSize: 13
                onClicked: {
                    if (rootPage.selectedCount === filterModel.count) {
                        rootPage.clearSelection()
                        rootPage.selectionMode = true
                    } else {
                        var paths = filterModel.allRawPaths()
                        var s = ({})
                        for (var i = 0; i < paths.length; i++) s[paths[i]] = true
                        rootPage.selectedSet   = s
                        rootPage.selectedCount = paths.length
                        rootPage._selGen++
                    }
                }
            }

            // Filter toggle button — shows a dot badge when filters are active
            ToolButton {
                visible: !rootPage.selectionMode
                text: "⊘"
                font.pixelSize: 20
                onClicked: filterPopup.open()

                // Active-filter badge
                Rectangle {
                    anchors { top: parent.top; right: parent.right; margins: 6 }
                    width: 8; height: 8; radius: 4
                    color: Material.accent
                    visible: filterModel.isFiltered()
                }
            }
        }
    }

    // ── thumbnail grid ────────────────────────────────────────────────────────
    GridView {
        id: grid
        anchors.fill: parent
        anchors.margins: 2

        model: filterModel
        cellWidth:  (width - 4) / columns
        cellHeight: cellWidth

        readonly property int columns: Math.max(2, Math.floor(width / 180))

        ScrollBar.vertical: ScrollBar {}

        delegate: Item {
            id: cell
            width:  grid.cellWidth
            height: grid.cellHeight

            readonly property bool selected: {
                rootPage._selGen
                return model.rawPath in rootPage.selectedSet
            }

            // Retry preview fetch every 30 s while this cell is on screen and
            // has no preview yet.  Stops automatically when previewKey goes > 0
            // (preview arrived) or the daemon disconnects.  Destroyed with the
            // delegate when the cell scrolls out of the GridView's cull rect.
            Timer {
                interval: 30000
                repeat:   true
                running:  model.previewKey <= 0 && p2p.connected
                onTriggered: p2p.fetchPreview(model.rawPath, "thumb")
            }

            Rectangle {
                id: cellBg
                anchors { fill: parent; margins: 2 }
                color: "#222"; radius: 4; clip: true

                Image {
                    anchors.fill: parent
                    source: (model.previewThumbPath || model.hasProxy)
                            ? "image://avif" + model.rawPath + "?k=" + model.previewKey
                            : ""
                    sourceSize.width: 400
                    fillMode: Image.PreserveAspectCrop
                    asynchronous: true; smooth: true

                    Rectangle {
                        anchors.fill: parent; color: "#333"
                        visible: parent.status !== Image.Ready
                        Column {
                            anchors.centerIn: parent; spacing: 6
                            BusyIndicator {
                                anchors.horizontalCenter: parent.horizontalCenter
                                running: (model.previewThumbPath || model.hasProxy)
                                         && parent.parent.status === Image.Loading
                                visible: running
                            }
                            Label {
                                text: "…"; color: "#555"; font.pixelSize: 10
                                visible: !model.previewThumbPath && !model.hasProxy
                            }
                        }
                    }
                }

                Rectangle {
                    anchors.fill: parent
                    color: cell.selected ? "#6030a0f0" : "transparent"
                    visible: rootPage.selectionMode
                    Rectangle {
                        anchors { top: parent.top; right: parent.right; margins: 6 }
                        width: 22; height: 22; radius: 11
                        color: cell.selected ? "#2196F3" : "transparent"
                        border { color: "white"; width: 2 }
                        Text {
                            anchors.centerIn: parent; text: "✓"; color: "white"
                            font.pixelSize: 13; visible: cell.selected
                        }
                    }
                }

                Row {
                    anchors { bottom: parent.bottom; left: parent.left; margins: 4 }
                    spacing: 1; visible: !rootPage.selectionMode
                    Repeater {
                        model: 5
                        Text {
                            text: "★"
                            color: index < (model.rating ?? 0) ? "#f4a020" : "#555"
                            font.pixelSize: 10
                        }
                    }
                }

                Rectangle {
                    anchors { bottom: parent.bottom; right: parent.right; margins: 5 }
                    width: 8; height: 8; radius: 4
                    visible: !rootPage.selectionMode && (model.colorLabel ?? -1) >= 0
                    color: {
                        const cl = model.colorLabel ?? -1
                        return cl >= 0 ? rootPage.colorValues[cl] : "transparent"
                    }
                }

                MouseArea {
                    anchors.fill: parent
                    onClicked: {
                        if (rootPage.selectionMode) {
                            rootPage.toggleSelection(model.rawPath)
                            if (rootPage.selectedCount === 0)
                                rootPage.selectionMode = false
                        } else {
                            rootPage.StackView.view?.push("DarkroomView.qml", {
                                modelIndex: index,
                                rawPath:    model.rawPath,
                                proxyPath:  model.proxyPath,
                                filename:   model.filename,
                                rating:     model.rating,
                                colorLabel: model.colorLabel,
                                hasProxy:   model.hasProxy,
                                previewKey: model.previewKey,
                            })
                        }
                    }
                    onPressAndHold: {
                        if (!rootPage.selectionMode) {
                            rootPage.selectionMode = true
                            rootPage.toggleSelection(model.rawPath)
                        }
                    }
                }
            }
        }
    }

    // ── empty state ───────────────────────────────────────────────────────────
    Column {
        anchors.centerIn: parent
        spacing: 12
        visible: filterModel.count === 0

        Label {
            anchors.horizontalCenter: parent.horizontalCenter
            text: imageModel.count === 0 ? "No images yet"
                                         : "No images match the current filter"
            font.pixelSize: 20; color: "#666"
        }
        Label {
            anchors.horizontalCenter: parent.horizontalCenter
            text: {
                if (imageModel.count > 0)
                    return "Adjust the filter to see images"
                return p2p.connected
                       ? "Waiting for images from desktop darktable…"
                       : "Configure a passphrase in Settings to connect."
            }
            color: "#555"; font.pixelSize: 13
            wrapMode: Text.WordWrap; width: 260
            horizontalAlignment: Text.AlignHCenter
        }
        Button {
            anchors.horizontalCenter: parent.horizontalCenter
            text: "Clear filters"
            visible: imageModel.count > 0 && filterModel.isFiltered()
            onClicked: {
                filterModel.filmRoll   = ""
                filterModel.minRating  = 0
                filterModel.colorLabel = -2
            }
        }
    }

    // ── filter chip component ─────────────────────────────────────────────────
    component FilterChip: Rectangle {
        id: chip
        property string label: ""
        property bool   active: false
        signal tapped()

        height: 32
        width: chipLabel.implicitWidth + 20
        radius: 16
        color:  active ? "#3a7bd5" : "#383838"
        border { color: active ? "#3a7bd5" : "#555"; width: 1 }

        Text {
            id: chipLabel
            anchors.centerIn: parent
            text: chip.label
            color: chip.active ? "white" : "#bbb"
            font.pixelSize: 13
        }
        MouseArea { anchors.fill: parent; onClicked: chip.tapped() }
    }

    // ── filter popup ──────────────────────────────────────────────────────────
    Popup {
        id: filterPopup
        parent: Overlay.overlay
        width:  Math.min(rootPage.width, 480)
        x:      (rootPage.width  - width)  / 2
        y:      rootPage.height - height - 16
        padding: 16
        modal: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

        background: Rectangle {
            color: "#252525"; radius: 12
            layer.enabled: true
            layer.effect: null
        }

        Column {
            width: parent.width
            spacing: 14

            // ── drag handle ───────────────────────────────────────────────────
            Rectangle {
                anchors.horizontalCenter: parent.horizontalCenter
                width: 40; height: 4; radius: 2; color: "#555"
            }

            // ── film roll ─────────────────────────────────────────────────────
            Column {
                width: parent.width; spacing: 6
                Label { text: "FILM ROLL"; color: "#888"; font.pixelSize: 11 }
                ScrollView {
                    width: parent.width; height: 36
                    ScrollBar.horizontal.policy: ScrollBar.AsNeeded
                    ScrollBar.vertical.policy: ScrollBar.AlwaysOff
                    Row {
                        spacing: 6
                        // "All" chip
                        FilterChip {
                            label: "All"
                            active: filterModel.filmRoll === ""
                            onTapped: filterModel.filmRoll = ""
                        }
                        Repeater {
                            model: filterModel.filmRolls
                            FilterChip {
                                label: rootPage.formatRoll(modelData)
                                active: filterModel.filmRoll === modelData
                                onTapped: filterModel.filmRoll =
                                          (filterModel.filmRoll === modelData ? "" : modelData)
                            }
                        }
                    }
                }
            }

            // ── minimum rating ────────────────────────────────────────────────
            Column {
                width: parent.width; spacing: 6
                Label { text: "MINIMUM RATING"; color: "#888"; font.pixelSize: 11 }
                Row {
                    spacing: 6
                    FilterChip {
                        label: "All"
                        active: filterModel.minRating === 0
                        onTapped: filterModel.minRating = 0
                    }
                    Repeater {
                        model: 5
                        FilterChip {
                            readonly property int stars: index + 1
                            label: "≥" + stars + "★"
                            active: filterModel.minRating === stars
                            onTapped: filterModel.minRating =
                                      (filterModel.minRating === stars ? 0 : stars)
                        }
                    }
                }
            }

            // ── color label ───────────────────────────────────────────────────
            Column {
                width: parent.width; spacing: 6
                Label { text: "COLOR LABEL"; color: "#888"; font.pixelSize: 11 }
                Row {
                    spacing: 8
                    FilterChip {
                        label: "All"
                        active: filterModel.colorLabel === -2
                        onTapped: filterModel.colorLabel = -2
                    }
                    FilterChip {
                        label: "None"
                        active: filterModel.colorLabel === -1
                        onTapped: filterModel.colorLabel =
                                  (filterModel.colorLabel === -1 ? -2 : -1)
                    }
                    Repeater {
                        model: rootPage.colorValues
                        Rectangle {
                            width: 32; height: 32; radius: 16
                            color: modelData
                            opacity: filterModel.colorLabel === index ? 1.0 : 0.5
                            border {
                                color: "white"
                                width: filterModel.colorLabel === index ? 2 : 0
                            }
                            MouseArea {
                                anchors.fill: parent
                                onClicked: filterModel.colorLabel =
                                           (filterModel.colorLabel === index ? -2 : index)
                            }
                        }
                    }
                }
            }

            // ── sort ──────────────────────────────────────────────────────────
            Column {
                width: parent.width; spacing: 6
                Label { text: "SORT BY"; color: "#888"; font.pixelSize: 11 }
                Row {
                    spacing: 6
                    FilterChip {
                        label: "Date ↓"
                        active: filterModel.sortByDate
                        onTapped: filterModel.sortByDate = true
                    }
                    FilterChip {
                        label: "Name A-Z"
                        active: !filterModel.sortByDate
                        onTapped: filterModel.sortByDate = false
                    }
                }
            }

            // ── clear all ─────────────────────────────────────────────────────
            Button {
                width: parent.width
                text: "Clear All Filters"
                flat: true
                Material.foreground: filterModel.isFiltered() ? Material.accent : "#555"
                enabled: filterModel.isFiltered()
                onClicked: {
                    filterModel.filmRoll   = ""
                    filterModel.minRating  = 0
                    filterModel.colorLabel = -2
                }
            }
        }
    }
}
