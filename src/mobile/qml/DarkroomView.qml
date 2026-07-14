import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import DarktableMobile

Page {
    id: root

    required property int    modelIndex
    required property string rawPath
    required property string proxyPath
    required property string filename
    required property int    rating
    required property int    colorLabel
    required property bool   hasProxy

    // Bumped when a refreshed preview JPEG arrives; triggers image reload.
    property int previewKey: 0

    property int  localRating:     rating
    property int  localColorLabel: colorLabel

    // On open: always request the full-size JPEG preview from peers.
    // Mobile display is JPEG-only; the AVIF proxy is irrelevant here.
    Component.onCompleted: p2p.fetchPreview(root.rawPath, "full")

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
                icon.source: "icons/back.svg"
                icon.color:  "white"
                icon.width:  24
                icon.height: 24
                display: AbstractButton.IconOnly
                // StackView.view is only attached to the Page itself, not to
                // items inside header:. Reference it via the page id.
                onClicked: root.StackView.view.pop()
            }
            Label {
                Layout.fillWidth: true
                text: root.filename
                elide: Text.ElideMiddle
                font.pixelSize: 14
            }
            ToolButton {
                icon.source: "icons/share.svg"
                icon.color:  "white"
                icon.width:  24
                icon.height: 24
                display: AbstractButton.IconOnly
                visible: root.previewKey > 0
                onClicked: shareHelper.shareRawPaths([root.rawPath])
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
            // Display is JPEG-only on mobile: provider serves .preview-full.jpg
            // falling back to .preview-thumb.jpg. AVIF decoding is not used.
            source: root.previewKey > 0
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
                text: "No preview available\nTap ↓ to request from peers"
                visible: root.previewKey <= 0
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

                property real swipePressX: 0

                onDoubleClicked: {
                    image.scale = image.scale > 1.0 ? 1.0 : 2.5
                    flick.contentX = 0
                    flick.contentY = 0
                }
                onPressed: (mouse) => {
                    if (image.scale <= 1.0) {
                        // Track horizontal position to detect navigation swipes.
                        // Accept the press so we get the release; at 1× there is
                        // nothing for the Flickable to pan anyway.
                        swipePressX = mouse.x
                        mouse.accepted = true
                    } else {
                        // Zoomed in — let the Flickable handle panning.
                        mouse.accepted = false
                    }
                }
                onReleased: (mouse) => {
                    const dx = mouse.x - swipePressX
                    if (dx < -60)       root.navigateNext()
                    else if (dx > 60)   root.navigatePrev()
                }
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
                            root.pushEdits()
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
                    onClicked: { root.localRating = 0; root.pushEdits() }
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
                            onClicked: {
                                root.localColorLabel = (root.localColorLabel === index) ? -1 : index
                                root.pushEdits()
                            }
                        }
                    }
                }
                ToolButton {
                    text: "⬇ Fetch"
                    visible: root.previewKey <= 0
                    Layout.alignment: Qt.AlignRight
                    onClicked: p2p.fetchPreview(root.rawPath, "full")
                }
            }
        }
    }

    // ── swipe navigation ──────────────────────────────────────────────────────
    function navigateTo(idx) {
        if (idx < 0 || idx >= filterModel.count) return
        const m = filterModel.get(idx)
        root.StackView.view.replace(null, "DarkroomView.qml", {
            modelIndex: idx,
            rawPath:    m.rawPath,
            proxyPath:  m.proxyPath,
            filename:   m.filename,
            rating:     m.rating,
            colorLabel: m.colorLabel,
            hasProxy:   m.hasProxy,
            previewKey: m.previewKey,
        }, StackView.Immediate)
    }
    function navigateNext() { navigateTo(root.modelIndex + 1) }
    function navigatePrev() { navigateTo(root.modelIndex - 1) }

    // ── edit push logic ───────────────────────────────────────────────────────
    function pushEdits() {
        // Update model immediately so the grid reflects the change before the
        // network round-trip completes.
        imageModel.setRating(root.rawPath, root.localRating)
        imageModel.setColorLabel(root.rawPath, root.localColorLabel)
        root.rating     = root.localRating
        root.colorLabel = root.localColorLabel

        // Write XMP sidecar to disk and push to peers via the daemon.
        p2p.applyAndPushEdits(root.rawPath, root.localRating, root.localColorLabel)
    }
}
