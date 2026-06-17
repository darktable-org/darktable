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
    property var  selectedSet:   ({})   // rawPath → true
    property int  selectedCount: 0
    // Incremented to bust delegate bindings that check selectedSet membership.
    property int  _selGen: 0

    function toggleSelection(rawPath) {
        var s = Object.assign({}, selectedSet)
        if (rawPath in s) {
            delete s[rawPath]
            selectedCount--
        } else {
            s[rawPath] = true
            selectedCount++
        }
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

    // ── header ────────────────────────────────────────────────────────────────
    header: ToolBar {
        Material.background: selectionMode ? "#1a3a5c" : "#1e1e1e"

        Behavior on Material.background { ColorAnimation { duration: 120 } }

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 4
            anchors.rightMargin: 4

            // Cancel selection
            ToolButton {
                text: "✕"
                font.pixelSize: 18
                visible: rootPage.selectionMode
                onClicked: rootPage.clearSelection()
            }

            Label {
                Layout.fillWidth: true
                text: rootPage.selectionMode
                      ? (rootPage.selectedCount === 0
                         ? "Select images"
                         : rootPage.selectedCount + " selected")
                      : "Gallery  (" + imageModel.count + ")"
                font.pixelSize: 16
                font.bold: !rootPage.selectionMode
                horizontalAlignment: rootPage.selectionMode ? Text.AlignLeft : Text.AlignHCenter
            }

            // Share selected
            ToolButton {
                icon.source: "icons/share.svg"
                icon.color:  "white"
                icon.width:  24
                icon.height: 24
                text: "Share"
                display: AbstractButton.TextBesideIcon
                visible: rootPage.selectionMode && rootPage.selectedCount > 0
                Material.accent: Material.Blue
                highlighted: true
                onClicked: rootPage.shareSelected()
            }

            // Select-all toggle (only shown when in selection mode)
            ToolButton {
                text: rootPage.selectedCount === imageModel.count ? "None" : "All"
                visible: rootPage.selectionMode
                font.pixelSize: 13
                onClicked: {
                    if (rootPage.selectedCount === imageModel.count) {
                        rootPage.clearSelection()
                        rootPage.selectionMode = true
                    } else {
                        var paths = imageModel.allRawPaths()
                        var s = ({})
                        for (var i = 0; i < paths.length; i++) s[paths[i]] = true
                        rootPage.selectedSet   = s
                        rootPage.selectedCount = paths.length
                        rootPage._selGen++
                    }
                }
            }
        }
    }

    // ── thumbnail grid ────────────────────────────────────────────────────────
    GridView {
        id: grid
        anchors.fill: parent
        anchors.margins: 2

        model: imageModel
        cellWidth:  (width - 4) / columns
        cellHeight: cellWidth

        readonly property int columns: Math.max(2, Math.floor(width / 180))

        ScrollBar.vertical: ScrollBar {}

        delegate: Item {
            id: cell
            width:  grid.cellWidth
            height: grid.cellHeight

            // Reactive: re-evaluated whenever _selGen changes.
            readonly property bool selected: {
                rootPage._selGen  // dependency
                return model.rawPath in rootPage.selectedSet
            }

            Rectangle {
                id: cellBg
                anchors { fill: parent; margins: 2 }
                color: "#222"
                radius: 4
                clip: true

                // ── proxy image ───────────────────────────────────────────────
                Image {
                    anchors.fill: parent
                    source: (model.previewThumbPath || model.hasProxy)
                            ? "image://avif" + model.rawPath + "?k=" + model.previewKey
                            : ""
                    sourceSize.width: 400
                    fillMode:     Image.PreserveAspectCrop
                    asynchronous: true
                    smooth:       true

                    Rectangle {
                        anchors.fill: parent
                        color: "#333"
                        visible: parent.status !== Image.Ready

                        Column {
                            anchors.centerIn: parent
                            spacing: 6
                            BusyIndicator {
                                anchors.horizontalCenter: parent.horizontalCenter
                                running: (model.previewThumbPath || model.hasProxy)
                                         && parent.parent.status === Image.Loading
                                visible: running
                            }
                            Label {
                                text: "…"
                                color: "#555"
                                font.pixelSize: 10
                                visible: !model.previewThumbPath && !model.hasProxy
                            }
                        }
                    }
                }

                // ── selection overlay ─────────────────────────────────────────
                Rectangle {
                    anchors.fill: parent
                    color: cell.selected ? "#6030a0f0" : "transparent"
                    visible: rootPage.selectionMode

                    Rectangle {
                        anchors { top: parent.top; right: parent.right; margins: 6 }
                        width: 22; height: 22; radius: 11
                        color:  cell.selected ? "#2196F3" : "transparent"
                        border { color: "white"; width: 2 }

                        Text {
                            anchors.centerIn: parent
                            text: "✓"
                            color: "white"
                            font.pixelSize: 13
                            visible: cell.selected
                        }
                    }
                }

                // ── star rating overlay ───────────────────────────────────────
                Row {
                    anchors { bottom: parent.bottom; left: parent.left; margins: 4 }
                    spacing: 1
                    visible: !rootPage.selectionMode
                    Repeater {
                        model: 5
                        Text {
                            text:  "★"
                            color: index < (model.rating ?? 0) ? "#f4a020" : "#555"
                            font.pixelSize: 10
                        }
                    }
                }

                // ── color label dot ───────────────────────────────────────────
                Rectangle {
                    anchors { bottom: parent.bottom; right: parent.right; margins: 5 }
                    width: 8; height: 8; radius: 4
                    visible: !rootPage.selectionMode && (model.colorLabel ?? -1) >= 0
                    color: {
                        const colors = ["#e02020","#e0b020","#20a020","#2060e0","#9020c0"]
                        return (model.colorLabel ?? -1) >= 0 ? colors[model.colorLabel] : "transparent"
                    }
                }

                // ── tap / long-press ──────────────────────────────────────────
                MouseArea {
                    anchors.fill: parent
                    onClicked: {
                        if (rootPage.selectionMode) {
                            rootPage.toggleSelection(model.rawPath)
                            // Auto-exit if last item deselected
                            if (rootPage.selectedCount === 0)
                                rootPage.selectionMode = false
                        } else {
                            rootPage.StackView.view?.push("DarkroomView.qml", {
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
        visible: imageModel.count === 0

        Label {
            anchors.horizontalCenter: parent.horizontalCenter
            text: "No images yet"
            font.pixelSize: 20
            color: "#666"
        }
        Label {
            anchors.horizontalCenter: parent.horizontalCenter
            text: p2p.connected
                  ? "Waiting for images from desktop darktable…"
                  : "Configure a passphrase in Settings to connect."
            color: "#555"
            font.pixelSize: 13
            wrapMode: Text.WordWrap
            width: 260
            horizontalAlignment: Text.AlignHCenter
        }
    }
}
