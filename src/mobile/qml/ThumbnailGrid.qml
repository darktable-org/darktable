import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

Page {
    id: rootPage
    objectName: "gallery"
    background: Rectangle { color: "#111111" }

    header: ToolBar {
        Material.background: "#1e1e1e"
        Label {
            anchors.centerIn: parent
            text:  "Gallery  (" + imageModel.count + ")"
            font.pixelSize: 16
            font.bold: true
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
            width:  grid.cellWidth
            height: grid.cellHeight

            Rectangle {
                anchors { fill: parent; margins: 2 }
                color: "#222"
                radius: 4
                clip: true

                // ── proxy image ───────────────────────────────────────────────
                Image {
                    anchors.fill: parent
                    // Route all display through the avif provider using the
                    // canonical raw path. The provider checks for a peer-
                    // fetched JPEG preview first; on desktop it falls back to
                    // the Qt AVIF plugin. ?k= cache-busts when a preview
                    // refresh arrives (previewKey is bumped by updatePreview).
                    source: (model.previewThumbPath || model.hasProxy)
                            ? "image://avif" + model.rawPath + "?k=" + model.previewKey
                            : ""
                    // sourceSize hint tells the provider this is a thumbnail
                    // request (≤480 px) so it picks the thumb JPEG variant.
                    sourceSize.width: 400
                    fillMode:     Image.PreserveAspectCrop
                    asynchronous: true
                    smooth:       true

                    // Placeholder while loading or no proxy
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

                // ── star rating overlay ───────────────────────────────────────
                Row {
                    anchors { bottom: parent.bottom; left: parent.left; margins: 4 }
                    spacing: 1
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
                    visible: (model.colorLabel ?? -1) >= 0
                    color: {
                        const colors = ["#e02020","#e0b020","#20a020","#2060e0","#9020c0"]
                        return (model.colorLabel ?? -1) >= 0 ? colors[model.colorLabel] : "transparent"
                    }
                }

                // ── tap to open darkroom ──────────────────────────────────────
                MouseArea {
                    anchors.fill: parent
                    onClicked: {
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
