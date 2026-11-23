import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Dialogs
import QtQuick.Layouts

// Lightroom-style library: folders, a thumbnail grid, multi-select, and
// actions that apply to every selected photo
Item {
    id: root
    property bool wide: true
    readonly property int selectedCount: lab.selection.length
    readonly property var labelColors: ["#e5484d", "#f5c400", "#30a46c", "#3e63dd", "#8e4ec6"]

    function isSelected(id) { return lab.selection.indexOf(id) >= 0 }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        // ------------------------------------------------------------ folders
        Rectangle {
            visible: root.wide
            Layout.fillHeight: true
            Layout.preferredWidth: 250
            color: Theme.panel

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 12
                spacing: 6

                Button {
                    text: "+ Import folder"
                    highlighted: true
                    Layout.fillWidth: true
                    onClicked: folderDialog.open()
                }
                SectionHeader { title: "Folders" }
                ListView {
                    id: folderList
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: [{ id: -1, name: "All photographs", count: -1 }].concat(lab.folders)
                    delegate: ItemDelegate {
                        required property var modelData
                        width: folderList.width
                        highlighted: lab.currentFolder === modelData.id
                        onClicked: lab.showFolder(modelData.id)
                        contentItem: RowLayout {
                            Text {
                                text: modelData.name
                                color: Theme.text
                                elide: Text.ElideMiddle
                                Layout.fillWidth: true
                            }
                            Text {
                                text: modelData.count >= 0 ? modelData.count : ""
                                color: Theme.textDim
                            }
                        }
                        ToolTip.visible: hovered && modelData.path !== undefined
                        ToolTip.text: modelData.path || ""
                    }
                }
            }
        }

        // --------------------------------------------------------------- grid
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            // filter bar
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: 48
                color: Theme.window
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 12
                    anchors.rightMargin: 12
                    spacing: 8
                    ToolButton {
                        visible: !root.wide
                        text: "Import"
                        onClicked: folderDialog.open()
                    }
                    Text { text: "Show"; color: Theme.textDim }
                    ComboBox {
                        id: ratingFilter
                        implicitWidth: 150
                        model: ["all", "★ and up", "★★ and up", "★★★ and up",
                                "★★★★ and up", "★★★★★", "rejected"]
                        currentIndex: lab.showRejected ? 6 : lab.minRating
                        onActivated: (i) => {
                            if(i === 6) lab.setShowRejected(true)
                            else { lab.setShowRejected(false); lab.setMinRating(i) }
                        }
                    }
                    Item { Layout.fillWidth: true }
                    Text {
                        text: root.selectedCount > 0 ? root.selectedCount + " of " + lab.images.length + " selected"
                                                     : lab.images.length + " photos"
                        color: Theme.textDim
                    }
                    ToolButton { text: "Select all"; onClicked: lab.selectAll() }
                    ToolButton {
                        text: "None"
                        enabled: root.selectedCount > 0
                        onClicked: lab.clearSelection()
                    }
                    Slider {
                        id: zoom
                        visible: root.wide
                        from: 120; to: 360; value: 200
                        implicitWidth: 120
                    }
                }
            }

            GridView {
                id: grid
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                focus: true
                readonly property int columns: Math.max(2, Math.floor(width / (root.wide ? zoom.value : 130)))
                cellWidth: Math.floor(width / columns)
                cellHeight: cellWidth
                model: lab.images
                ScrollBar.vertical: ScrollBar {}

                delegate: Item {
                    id: cell
                    required property var modelData
                    required property int index
                    width: grid.cellWidth
                    height: grid.cellHeight
                    readonly property bool selected: root.isSelected(modelData.id)

                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: 4
                        radius: 4
                        color: cell.selected ? Theme.panelRaised : Theme.panel
                        border.width: cell.selected ? 2 : 0
                        border.color: Theme.accent

                        Image {
                            anchors.fill: parent
                            anchors.margins: 8
                            anchors.bottomMargin: 26
                            fillMode: Image.PreserveAspectFit
                            asynchronous: true
                            cache: false
                            sourceSize.width: 400
                            sourceSize.height: 400
                            source: "image://thumb/" + cell.modelData.id + "/"
                                    + (lab.thumbVersions[String(cell.modelData.id)] || 0)
                            opacity: cell.modelData.rating === 6 ? 0.35 : 1
                        }

                        RowLayout {
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.bottom: parent.bottom
                            anchors.margins: 6
                            spacing: 4
                            Text {
                                text: cell.modelData.rating === 6 ? "✕"
                                      : "★".repeat(cell.modelData.rating)
                                color: cell.modelData.rating === 6 ? "#e5484d" : "#f5c400"
                                font.pixelSize: 12
                            }
                            Repeater {
                                model: 5
                                delegate: Rectangle {
                                    required property int index
                                    visible: (cell.modelData.labels >> index) & 1
                                    width: 9; height: 9; radius: 5
                                    color: root.labelColors[index]
                                }
                            }
                            Item { Layout.fillWidth: true }
                            Text {
                                visible: cell.modelData.edited
                                text: "●"
                                color: Theme.accent
                                font.pixelSize: 10
                                ToolTip.visible: false
                            }
                        }
                    }

                    MouseArea {
                        anchors.fill: parent
                        onClicked: (m) => {
                            grid.forceActiveFocus()
                            lab.select(cell.modelData.id,
                                       (m.modifiers & Qt.ShiftModifier) ? 2
                                       : (m.modifiers & Qt.ControlModifier) ? 1 : 0)
                        }
                        onDoubleClicked: lab.openImage(cell.modelData.id)
                        onPressAndHold: lab.select(cell.modelData.id, 1) // touch: add to selection
                    }
                }

                Text {
                    anchors.centerIn: parent
                    visible: lab.images.length === 0
                    text: lab.ready ? "No photos here yet. Import a folder to start." : "Starting the darktable engine..."
                    color: Theme.textDim
                    font.pixelSize: 16
                }
            }

            // ----------------------------------------------- selection actions
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: actions.implicitHeight + 16
                color: Theme.panel
                enabled: root.selectedCount > 0

                Flow {
                    id: actions
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.margins: 8
                    spacing: 4

                    Repeater {
                        model: 6
                        delegate: ToolButton {
                            required property int index
                            text: index === 0 ? "0★" : index + "★"
                            onClicked: lab.rateSelected(index)
                            ToolTip.visible: hovered
                            ToolTip.text: "rate " + index + " (key " + index + ")"
                        }
                    }
                    ToolButton {
                        text: "Reject"
                        onClicked: lab.rejectSelected()
                        ToolTip.visible: hovered
                        ToolTip.text: "key X"
                    }
                    Repeater {
                        model: 5
                        delegate: ToolButton {
                            required property int index
                            contentItem: Rectangle {
                                implicitWidth: 14; implicitHeight: 14; radius: 7
                                color: root.labelColors[index]
                            }
                            onClicked: lab.toggleLabelSelected(index)
                        }
                    }
                    ToolSeparator {}
                    Button {
                        text: "+ Add LUT to selected"
                        highlighted: true
                        onClicked: lutDialog.open()
                    }
                    Button {
                        text: "Style..."
                        onClicked: stylePopup.open()
                    }
                    Button {
                        text: "Paste edit"
                        enabled: lab.hasCopiedEdit
                        onClicked: lab.pasteEditToSelected()
                        ToolTip.visible: hovered
                        ToolTip.text: lab.hasCopiedEdit ? "paste the copied sliders, LUTs and style (Ctrl+V)"
                                                        : "copy an edit in Develop first"
                    }
                    Button {
                        text: "Export..."
                        onClicked: exportFolderDialog.open()
                    }
                }
            }
        }
    }

    Popup {
        id: stylePopup
        anchors.centerIn: parent
        width: Math.min(root.width - 32, 520)
        height: Math.min(root.height - 32, 560)
        modal: true
        ColumnLayout {
            anchors.fill: parent
            Text {
                text: "Apply a style to " + root.selectedCount + " photos"
                color: Theme.text
                font.pixelSize: 16
            }
            TextField {
                id: styleSearch
                placeholderText: "search styles"
                Layout.fillWidth: true
            }
            ListView {
                id: styleList
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                model: [""].concat(lab.styles.filter(s => s.toLowerCase().indexOf(styleSearch.text.toLowerCase()) >= 0))
                delegate: ItemDelegate {
                    required property string modelData
                    width: styleList.width
                    text: modelData === "" ? "none (remove style)" : modelData.replace(/^.*\|/, "")
                    onClicked: { lab.styleSelected(modelData); stylePopup.close() }
                }
            }
        }
    }

    Shortcut { sequences: ["0"]; enabled: root.visible; onActivated: lab.rateSelected(0) }
    Shortcut { sequences: ["1"]; enabled: root.visible; onActivated: lab.rateSelected(1) }
    Shortcut { sequences: ["2"]; enabled: root.visible; onActivated: lab.rateSelected(2) }
    Shortcut { sequences: ["3"]; enabled: root.visible; onActivated: lab.rateSelected(3) }
    Shortcut { sequences: ["4"]; enabled: root.visible; onActivated: lab.rateSelected(4) }
    Shortcut { sequences: ["5"]; enabled: root.visible; onActivated: lab.rateSelected(5) }
    Shortcut { sequences: ["X"]; enabled: root.visible; onActivated: lab.rejectSelected() }
    Shortcut { sequences: [StandardKey.SelectAll]; enabled: root.visible; onActivated: lab.selectAll() }
    Shortcut { sequences: [StandardKey.Paste]; enabled: root.visible; onActivated: lab.pasteEditToSelected() }

    FolderDialog {
        id: folderDialog
        title: "Import a folder (subfolders included)"
        onAccepted: lab.importFolder(selectedFolder, true)
    }
    FolderDialog {
        id: exportFolderDialog
        title: "Export the selected photos to"
        onAccepted: lab.exportSelected(selectedFolder, 92)
    }
    FileDialog {
        id: lutDialog
        title: "Add a LUT to the selected photos"
        nameFilters: ["LUTs (*.cube *.3dl *.png)", "All files (*)"]
        onAccepted: lab.addLutToSelected(selectedFile)
    }
}
