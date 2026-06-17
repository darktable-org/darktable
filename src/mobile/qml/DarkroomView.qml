import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import DarktableMobile

Page {
    id: root

    required property string rawPath
    required property string proxyPath
    required property string filename
    required property int    rating
    required property int    colorLabel
    required property bool   hasProxy

    // Bumped when a refreshed preview JPEG arrives; triggers image reload.
    property int previewKey: 0

    // Local copies of editable fields so we can diff before pushing
    property int  localRating:     rating
    property int  localColorLabel: colorLabel
    property bool dirty: localRating !== rating || localColorLabel !== colorLabel

    // On open: fetch the full-size preview from peers so the darkroom shows a
    // full-resolution JPEG instead of falling back to MediaCodec AVIF decode.
    Component.onCompleted: {
        if (root.hasProxy)
            p2p.fetchPreview(root.rawPath, "full")
    }

    // Reload the image when a fresher preview arrives from peers.
    Connections {
        target: p2p
        function onPreviewUpdated(path) {
            if (path === root.rawPath)
                root.previewKey++
        }
    }

    background: Rectangle { color: "black" }

    // ── header ────────────────────────────────────────────────────────────────
    header: ToolBar {
        Material.background: "#1e1e1e"
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 4
            ToolButton {
                icon.name: "arrow-back"
                text: "‹"
                font.pixelSize: 22
                onClicked: StackView.view.pop()
            }
            Label {
                Layout.fillWidth: true
                text: root.filename
                elide: Text.ElideMiddle
                font.pixelSize: 14
            }
            ToolButton {
                text: "⬆"
                font.pixelSize: 20
                visible: root.hasProxy || root.previewKey > 0
                onClicked: shareHelper.shareRawPaths([root.rawPath])
            }
            ToolButton {
                text: "Push"
                visible: root.dirty
                Material.accent: Material.Orange
                highlighted: true
                onClicked: root.pushEdits()
            }
        }
    }

    // ── pinch-to-zoom image viewer ────────────────────────────────────────────
    Flickable {
        id: flick
        anchors { top: parent.top; left: parent.left; right: parent.right; bottom: editPanel.top }
        contentWidth:  Math.max(width,  image.width  * image.scale)
        contentHeight: Math.max(height, image.height * image.scale)
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        Image {
            id: image
            // No sourceSize → provider picks the full-res preview JPEG,
            // falling back to the desktop AVIF plugin on non-Android.
            source: (root.hasProxy || root.previewKey > 0)
                    ? ("image://avif" + root.rawPath + "?k=" + root.previewKey)
                    : ""
            anchors.centerIn: parent
            fillMode: Image.PreserveAspectFit
            width:  flick.width
            height: flick.height
            smooth: true
            asynchronous: true

            transformOrigin: Item.Center

            BusyIndicator {
                anchors.centerIn: parent
                running: image.status === Image.Loading
            }

            Label {
                anchors.centerIn: parent
                text: "No proxy available\nTap ↓ to request from peers"
                visible: !root.hasProxy && image.status !== Image.Loading
                color: "#888"
                font.pixelSize: 14
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
            }
        }

        PinchArea {
            anchors.fill: parent
            property real startScale: 1.0

            onPinchStarted: startScale = image.scale
            onPinchUpdated: {
                image.scale = Math.max(1.0, Math.min(5.0, startScale * pinch.scale))
                // Re-center if zoomed out past 1×
                if (image.scale <= 1.0) {
                    flick.contentX = 0
                    flick.contentY = 0
                }
            }

            MouseArea {
                anchors.fill: parent
                onDoubleClicked: {
                    image.scale = image.scale > 1.0 ? 1.0 : 2.5
                    flick.contentX = 0
                    flick.contentY = 0
                }
                // Allow Flickable to handle single-tap drag
                onPressed: mouse.accepted = false
            }
        }
    }

    // ── edit panel ─────────────────────────────────────────────────────────────
    Rectangle {
        id: editPanel
        anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
        height: 110
        color: "#1a1a1a"

        Column {
            anchors { fill: parent; margins: 12 }
            spacing: 10

            // Star rating
            RowLayout {
                width: parent.width
                Label {
                    text: "Rating"
                    color: "#aaa"
                    font.pixelSize: 13
                    Layout.preferredWidth: 60
                }
                Repeater {
                    model: 5
                    ToolButton {
                        readonly property int starValue: index + 1
                        text: "★"
                        font.pixelSize: 24
                        contentItem: Text {
                            text: "★"
                            font.pixelSize: 24
                            color: starValue <= root.localRating ? "#f4a020" : "#555"
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment:   Text.AlignVCenter
                        }
                        onClicked: {
                            root.localRating = (root.localRating === starValue) ? 0 : starValue
                        }
                    }
                }
                // Clear
                ToolButton {
                    text: "✕"
                    font.pixelSize: 14
                    contentItem: Text {
                        text: "✕"; color: "#666"
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    visible: root.localRating > 0
                    onClicked: root.localRating = 0
                }
            }

            // Color label
            RowLayout {
                width: parent.width
                Label {
                    text: "Label"
                    color: "#aaa"
                    font.pixelSize: 13
                    Layout.preferredWidth: 60
                }
                Repeater {
                    model: ["#e02020","#e0b020","#20a020","#2060e0","#9020c0"]
                    Rectangle {
                        width:  28; height: 28; radius: 14
                        color:  modelData
                        opacity: root.localColorLabel === index ? 1.0 : 0.35
                        border { color: "white"; width: root.localColorLabel === index ? 2 : 0 }
                        MouseArea {
                            anchors.fill: parent
                            onClicked: root.localColorLabel = (root.localColorLabel === index) ? -1 : index
                        }
                    }
                }
                // Fetch proxy button (shown when no proxy yet)
                ToolButton {
                    text: "⬇ Fetch"
                    visible: !root.hasProxy
                    Layout.alignment: Qt.AlignRight
                    onClicked: p2p.fetchProxy(root.rawPath)
                }
            }
        }
    }

    // ── edit push logic ───────────────────────────────────────────────────────
    function pushEdits() {
        // Load current XMP from disk, patch the two fields, save, and push.
        // The XmpIO helpers are not directly callable from QML, so we route
        // through a small helper exposed on the p2p context object.
        // For the prototype we call the C++ slot via Connections.
        editWorker.apply()
    }

    // A thin bridge to C++ for the file-patching work that can't be done in QML.
    // In production this would be a proper QML-exposed helper; for the prototype
    // it's a placeholder that the developer wires up in main.cpp or a plugin.
    QtObject {
        id: editWorker
        function apply() {
            // Signal intent to C++ via a dummy property write so main.cpp can
            // listen and do the actual XmpIO work.  This keeps all file I/O
            // out of the QML engine.
            editPendingChanged()
        }
    }

    signal editPendingChanged()

    // Update model immediately so the grid reflects the change before the
    // network round-trip completes.
    onEditPendingChanged: {
        imageModel.setRating(root.rawPath, root.localRating)
        imageModel.setColorLabel(root.rawPath, root.localColorLabel)
        // Sync rating and colorLabel back so dirty clears
        root.rating     = root.localRating
        root.colorLabel = root.localColorLabel
    }
}
