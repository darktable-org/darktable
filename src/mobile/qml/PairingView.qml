import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import QtMultimedia

Page {
    id: root
    objectName: "pairing"
    background: Rectangle { color: "#000" }

    signal done()

    header: ToolBar {
        Material.background: "#1e1e1e"
        RowLayout {
            anchors.fill: parent
            ToolButton {
                text: "✕"
                font.pixelSize: 18
                onClicked: { cam.active = false; root.done() }
            }
            Label {
                Layout.fillWidth: true
                text: "Scan Pairing QR"
                font.pixelSize: 16
                font.bold: true
                horizontalAlignment: Text.AlignHCenter
            }
            // Spacer to balance the close button
            Item { width: 48 }
        }
    }

    // ── camera session ────────────────────────────────────────────────────────
    // Camera starts only after permission is granted (requestCameraPermission
    // is called from Component.onCompleted below).
    CaptureSession {
        id: session
        camera: Camera {
            id: cam
            active: false   // enabled after permission granted
            focusMode: Camera.FocusModeAuto
        }
        videoOutput: viewfinder
    }

    VideoOutput {
        id: viewfinder
        anchors.fill: parent
    }

    // ── permission denied overlay ─────────────────────────────────────────────
    Rectangle {
        anchors.fill: parent
        color: "#cc000000"
        visible: permissionDenied
        Column {
            anchors.centerIn: parent
            spacing: 16
            Label {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "Camera permission is required\nto scan the pairing QR code."
                color: "white"
                horizontalAlignment: Text.AlignHCenter
                font.pixelSize: 14
                wrapMode: Text.WordWrap
                width: 260
            }
            Button {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "Grant Permission"
                highlighted: true
                onClicked: qrScanner.requestCameraPermission()
            }
        }
    }

    property bool permissionDenied: false

    onVisibleChanged: if(!visible) cam.active = false

    Component.onCompleted: qrScanner.requestCameraPermission()

    Connections {
        target: qrScanner
        function onCameraReady() {
            root.permissionDenied = false
            cam.active = root.visible
            qrScanner.attachTo(viewfinder.videoSink)
        }
        function onCameraPermissionDenied() {
            root.permissionDenied = true
        }
    }

    // ── aiming reticle ────────────────────────────────────────────────────────
    Rectangle {
        id: reticle
        anchors.centerIn: parent
        width:  220; height: 220
        color:  "transparent"
        border { color: "#ff8800"; width: 2 }
        radius: 10

        // Corner L-shapes (top-left, top-right, bottom-left, bottom-right)
        Repeater {
            model: [
                {hx: 0, hy: 0, hw: 28, hh: 4,  vx: 0, vy: 0, vw: 4, vh: 28},
                {hx: 1, hy: 0, hw: 28, hh: 4,  vx: 1, vy: 0, vw: 4, vh: 28},
                {hx: 0, hy: 1, hw: 28, hh: 4,  vx: 0, vy: 1, vw: 4, vh: 28},
                {hx: 1, hy: 1, hw: 28, hh: 4,  vx: 1, vy: 1, vw: 4, vh: 28}
            ]
            Item {
                required property var modelData
                anchors.fill: parent
                Rectangle {
                    width:  parent.modelData.hw; height: parent.modelData.hh
                    color:  "#ff8800"
                    anchors {
                        left:   parent.modelData.hx === 0 ? parent.left  : undefined
                        right:  parent.modelData.hx === 1 ? parent.right : undefined
                        top:    parent.modelData.hy === 0 ? parent.top   : undefined
                        bottom: parent.modelData.hy === 1 ? parent.bottom: undefined
                    }
                }
                Rectangle {
                    width:  parent.modelData.vw; height: parent.modelData.vh
                    color:  "#ff8800"
                    anchors {
                        left:   parent.modelData.vx === 0 ? parent.left  : undefined
                        right:  parent.modelData.vx === 1 ? parent.right : undefined
                        top:    parent.modelData.vy === 0 ? parent.top   : undefined
                        bottom: parent.modelData.vy === 1 ? parent.bottom: undefined
                    }
                }
            }
        }

        // Scan-line animation while active
        Rectangle {
            id: scanLine
            width: parent.width - 4
            height: 2
            anchors.horizontalCenter: parent.horizontalCenter
            color: "#80ff8800"
            y: 2
            SequentialAnimation on y {
                running: root.visible
                loops: Animation.Infinite
                NumberAnimation { to: reticle.height - 4; duration: 1800; easing.type: Easing.InOutSine }
                NumberAnimation { to: 2;                  duration: 1800; easing.type: Easing.InOutSine }
            }
        }
    }

    Label {
        anchors { bottom: parent.bottom; horizontalCenter: parent.horizontalCenter }
        bottomPadding: 48
        text: "Point camera at the QR code shown in darktable preferences"
        color: "white"
        font.pixelSize: 12
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.WordWrap
        width: parent.width - 48
        style: Text.Outline
        styleColor: "#000"
    }

    // ── pairing confirmation dialog ───────────────────────────────────────────
    Dialog {
        id: confirmDialog
        anchors.centerIn: parent
        modal: true
        title: "Accept Pairing?"
        Material.theme: Material.Dark

        property string peersText: ""

        contentItem: Column {
            spacing: 12
            topPadding: 8; bottomPadding: 8
            leftPadding: 16; rightPadding: 16

            Label {
                text: "Apply the scanned passphrase and connect to:"
                color: "#ccc"
                font.pixelSize: 13
                wrapMode: Text.WordWrap
                width: 260
            }
            Label {
                text: confirmDialog.peersText.length > 0
                      ? confirmDialog.peersText
                      : "(no static peers — mDNS will discover nearby desktops)"
                color: Material.accentColor
                font.pixelSize: 12
                wrapMode: Text.WrapAnywhere
                width: 260
            }
            Label {
                text: "The sync daemon will be restarted."
                color: "#888"
                font.pixelSize: 11
                wrapMode: Text.WordWrap
                width: 260
            }
        }

        footer: DialogButtonBox {
            Button {
                text: "Accept"
                highlighted: true
                DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
            }
            Button {
                text: "Cancel"
                flat: true
                DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
            }
        }

        onAccepted: {
            pairing.accept()
            cam.active = false
            root.done()
        }
        onRejected: {
            pairing.reject()
            qrScanner.resume()
        }
    }

    // ── QR decode handler ─────────────────────────────────────────────────────
    Connections {
        target: qrScanner
        function onQrDecoded(text) {
            if(pairing.parseUrl(text)) {
                confirmDialog.peersText = pairing.pendingPeers
                confirmDialog.open()
            } else {
                // Not a darktable URL; keep trying.
                qrScanner.resume()
            }
        }
    }
}
